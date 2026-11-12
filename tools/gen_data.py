#!/usr/bin/env python3
"""Generate Tessera self-play training data. Safe to stop and resume at any time.

    python3 tools/gen_data.py                       # start, or continue where you stopped
    python3 tools/gen_data.py --out data/round3     # a fresh round in its own folder
    Ctrl+C                               # stop; everything finished so far is kept

How it works: several copies of the engine play games against themselves, each
side with its own random thinking budget. Finished games are written out as
quiet positions, one per line:   fen;score;result
(score = search score in centipawns, result = 1 / 0.5 / 0, both for White).

Work is done in small batches. A batch is only added to the data files once it
is complete, so stopping (or a crash, or a power cut) never leaves half-written
games behind; at most the batches in progress are lost, a few seconds of work.
Progress is remembered in <out>/state.json, so the next run continues with new
games instead of repeating old ones.

Needs only Python 3 and the engine binary (build it with:  make -C engine).
"""
import argparse
import json
import secrets
import os
import signal
import subprocess
import sys
import threading
import time

LINES_PER_FILE = 1_000_000          # start a new data file after this many positions


def parse_args():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    exe = "tessera.exe" if os.name == "nt" else "tessera"
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--engine", default=os.path.join(root, "engine", exe), help="path to the engine binary")
    ap.add_argument("--out", default=os.path.join(root, "data", "local"), help="folder for the data files")
    ap.add_argument("--target", type=int, default=100_000_000, help="stop after this many positions in total")
    ap.add_argument("--threads", type=int, default=max(1, (os.cpu_count() or 2) - 1),
                    help="engines running at once (default: all CPU threads but one)")
    ap.add_argument("--nodes", type=int, default=5000, help="average search budget per move")
    ap.add_argument("--batch", type=int, default=100, help="games per batch")
    ap.add_argument("--same-strength", action="store_true", help="both sides get exactly --nodes (default: random per side)")
    return ap.parse_args()


class Store:
    """The data files plus the progress file. All access goes through one lock."""

    def __init__(self, folder):
        self.folder = folder
        os.makedirs(folder, exist_ok=True)
        self.state_path = os.path.join(folder, "state.json")
        self.lock = threading.Lock()
        # A new folder starts at a random seed, so a new round does not replay the openings
        # (the random first moves) of an earlier round in another folder.
        self.state = {"next_seed": 1 + secrets.randbelow(2_000_000_000), "games": 0, "positions": 0,
                      "file_index": 1, "file_lines": 0}
        if os.path.exists(self.state_path):
            with open(self.state_path) as f:
                self.state.update(json.load(f))
        self.repair_current_file()

    def file_path(self):
        return os.path.join(self.folder, f"selfplay-local-{self.state['file_index']:04d}.txt")

    def repair_current_file(self):
        """After a power cut the last line may be cut in half: drop it, and trust the file over the counter."""
        path = self.file_path()
        if not os.path.exists(path):
            self.state["file_lines"] = 0
            return
        with open(path, "rb+") as f:
            data = f.read()
            keep = data.rfind(b"\n") + 1
            if keep != len(data):
                f.truncate(keep)
            lines = data[:keep].count(b"\n")
        drift = lines - self.state["file_lines"]
        if drift:
            self.state["file_lines"] = lines
            self.state["positions"] = max(0, self.state["positions"] + drift)
            self.save()

    def save(self):
        tmp = self.state_path + ".tmp"
        with open(tmp, "w") as f:
            json.dump(self.state, f)
        os.replace(tmp, self.state_path)       # atomic: the progress file is never half-written

    def take_seed(self):
        with self.lock:
            seed = self.state["next_seed"]
            self.state["next_seed"] += 1
            self.save()
            return seed

    def add_batch(self, text, games):
        lines = text.count("\n")
        with self.lock:
            with open(self.file_path(), "a") as f:
                f.write(text)
                f.flush()
                os.fsync(f.fileno())
            self.state["games"] += games
            self.state["positions"] += lines
            self.state["file_lines"] += lines
            if self.state["file_lines"] >= LINES_PER_FILE:
                self.state["file_index"] += 1
                self.state["file_lines"] = 0
            self.save()
            return self.state["positions"]


def check_engine(path):
    if not os.path.exists(path):
        sys.exit(f"Engine not found at {path}\nBuild it first:  make -C engine")
    try:
        out = subprocess.run([path, "datagen", "1", "300", "1", "mix"], capture_output=True, text=True, timeout=120).stdout
    except (OSError, subprocess.TimeoutExpired) as exc:
        sys.exit(f"Could not run the engine: {exc}")
    lines = [l for l in out.splitlines() if l.count(";") == 2]
    if not lines:
        sys.exit("This engine binary has no working 'datagen' command. Rebuild it from the current sources.")


def worker(args, store, stop, running, stats):
    while not stop.is_set():
        if store.state["positions"] >= args.target:
            stop.set()
            break
        seed = store.take_seed()
        cmd = [args.engine, "datagen", str(args.batch), str(args.nodes), str(seed)]
        if not args.same_strength:
            cmd.append("mix")
        # A new process group, so Ctrl+C reaches only this script and it can shut the engines down cleanly.
        extra = {"creationflags": subprocess.CREATE_NEW_PROCESS_GROUP} if os.name == "nt" else {"start_new_session": True}
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, **extra)
        with running["lock"]:
            running["procs"].add(proc)
        out, _ = proc.communicate()
        with running["lock"]:
            running["procs"].discard(proc)
        if stop.is_set() or proc.returncode != 0:
            continue                               # incomplete batch: throw it away
        text = "".join(l + "\n" for l in out.splitlines() if l.count(";") == 2)
        if text:
            total = store.add_batch(text, args.batch)
            with stats["lock"]:
                stats["session"] += text.count("\n")
                stats["total"] = total


def format_duration(seconds):
    hours, rest = divmod(int(seconds), 3600)
    return f"{hours} h {rest // 60:02d} min" if hours else f"{rest // 60} min"


def main():
    args = parse_args()
    check_engine(args.engine)
    store = Store(args.out)
    stop = threading.Event()
    running = {"lock": threading.Lock(), "procs": set()}
    stats = {"lock": threading.Lock(), "session": 0, "total": store.state["positions"]}

    def request_stop(*_):
        if not stop.is_set():
            print("\nStopping: finished batches are saved, batches in progress are dropped ...", flush=True)
        stop.set()
        with running["lock"]:
            for proc in list(running["procs"]):
                try:
                    proc.kill()
                except OSError:
                    pass

    signal.signal(signal.SIGINT, request_stop)
    if hasattr(signal, "SIGTERM"):
        signal.signal(signal.SIGTERM, request_stop)

    have = store.state["positions"]
    print(f"Data folder: {args.out}")
    print(f"Already have {have:,} positions from {store.state['games']:,} games. Target {args.target:,}.")
    if have >= args.target:
        print("Target reached. Raise it with --target to generate more.")
        return
    print(f"Running {args.threads} engines. Press Ctrl+C to stop; run the same command again to continue.\n", flush=True)

    threads = [threading.Thread(target=worker, args=(args, store, stop, running, stats), daemon=True)
               for _ in range(args.threads)]
    start = time.time()
    for t in threads:
        t.start()
    last_shown = -1
    while any(t.is_alive() for t in threads):
        time.sleep(1.0)
        with stats["lock"]:
            session, total = stats["session"], stats["total"]
        if session != last_shown and session > 0:
            last_shown = session
            elapsed = time.time() - start
            rate = session / elapsed
            left = max(0, args.target - total)
            print(f"\r{total:>13,} positions   {100 * total / args.target:5.1f}%   {rate:7,.0f} per second   "
                  f"about {format_duration(left / rate)} left   ", end="", flush=True)
    for t in threads:
        t.join()
    print(f"\nSaved. {store.state['positions']:,} positions from {store.state['games']:,} games so far.")
    if store.state["positions"] >= args.target:
        print("Target reached.")


if __name__ == "__main__":
    main()
