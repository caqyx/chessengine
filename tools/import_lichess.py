#!/usr/bin/env python3
"""Import Stockfish evaluations from the Lichess evaluation database as training data.

    pip install zstandard                                      # once: reads the .zst file
    python3 tools/import_lichess.py lichess_db_eval.jsonl.zst  # start, or continue where you stopped
    Ctrl+C                                                     # stop; finished chunks are kept

Download the file from https://database.lichess.org/ (the "evaluations" section). It is
public domain (CC0). NOTE: a network trained on it learns partly from Stockfish's judgment,
so Tessera would no longer be "trained only on its own games". Say so if you publish it.

What it does:
  1. Calibration (once): Tessera searches a few thousand positions from the file and compares
     its scores with Stockfish's. That decides whether the file's scores are from White's side
     or the side to move, and how to convert Stockfish centipawns to Tessera's scale.
  2. Import: the file is decompressed on the fly (it never has to exist uncompressed) and the
     engine keeps the quiet positions, converted, as training records in <out>/lichess-NNNN.bin
     (10 million positions each, about 680 MB). Progress is saved after every finished chunk.

Then train with them, for example together with your newest self-play round:
    python3 tools/train_net.py --data data/round3 data/lichess --out nets-lichess
"""
import argparse
import json
import os
import signal
import subprocess
import sys
import tempfile
import threading

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = ".exe" if os.name == "nt" else ""
TOTAL_LINES = 417_862_375        # positions in the database as of November 2026 (for progress only)


def parse_args():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("source", help="lichess_db_eval.jsonl.zst (or an uncompressed .jsonl)")
    ap.add_argument("--out", default=os.path.join(ROOT, "data", "lichess"), help="folder for the training records")
    ap.add_argument("--max", type=int, default=100_000_000, help="stop after this many positions in total")
    ap.add_argument("--engine", default=os.path.join(ROOT, "engine", "tessera" + EXE))
    ap.add_argument("--chunk", type=int, default=10_000_000, help=argparse.SUPPRESS)   # for tests
    return ap.parse_args()


class SourceTruncated(Exception):
    """The compressed file ends in the middle of its data: usually a download that has not finished."""


class _ZstdStream:
    """Decompresses a .zst file frame by frame and notices when the file stops mid-frame
    (the zstandard reader would otherwise simply report a normal end of file)."""

    def __init__(self, raw):
        import zstandard
        self.raw = raw
        self.dctx = zstandard.ZstdDecompressor(max_window_size=2 ** 31)
        self.obj = self.dctx.decompressobj()
        self.fed = False          # current frame has received input
        self.pending = b""

    def read(self, size):
        while True:
            if self.pending:
                out, self.pending = self.pending[:size], self.pending[size:]
                return out
            data = self.raw.read(1 << 20)
            if not data:
                if self.fed and not self.obj.eof:
                    raise SourceTruncated()
                return b""
            while data:
                self.pending += self.obj.decompress(data)
                self.fed = True
                if self.obj.eof:          # frame finished: anything left belongs to the next frame
                    data = self.obj.unused_data
                    self.obj = self.dctx.decompressobj()
                    self.fed = bool(data)
                else:
                    data = b""

    def close(self):
        self.raw.close()


def open_source(path):
    """A binary stream of the decompressed file."""
    raw = open(path, "rb")
    if not path.endswith(".zst"):
        return raw
    try:
        import zstandard  # noqa: F401
    except ImportError:
        sys.exit("Reading .zst needs the zstandard package:  pip install zstandard")
    return _ZstdStream(raw)


def load_state(out):
    path = os.path.join(out, "state.json")
    if os.path.exists(path):
        with open(path) as f:
            return json.load(f)
    return {"scale": None, "pov": None, "lines": 0, "records": 0, "next_chunk": 1}


def save_state(out, state):
    path = os.path.join(out, "state.json")
    with open(path + ".tmp", "w") as f:
        json.dump(state, f, indent=1)
    os.replace(path + ".tmp", path)


def calibrate(args, state):
    print("Calibrating: Tessera searches a sample of positions and compares with Stockfish ...", flush=True)
    stream = open_source(args.source)
    tmp = tempfile.NamedTemporaryFile("wb", delete=False, suffix=".jsonl")
    lines = 0
    while lines < 300_000:
        try:
            block = stream.read(1 << 20)
        except SourceTruncated:
            break                       # calibration only needs the start of the file
        if not block:
            break
        lines += block.count(b"\n")
        tmp.write(block)
    tmp.close()
    stream.close()
    try:
        out = subprocess.run([args.engine, "lichess-calibrate", tmp.name, "4000", "5000"],
                             capture_output=True, text=True, check=True).stdout
    finally:
        os.unlink(tmp.name)
    line = next((l for l in out.splitlines() if l.startswith("calibrate")), "")
    fields = dict(part.split("=", 1) for part in line.split()[1:] if "=" in part)
    if "scale" not in fields:
        sys.exit("Calibration failed:\n" + out)
    state["scale"], state["pov"] = float(fields["scale"]), fields["pov"]
    print(f"  {fields['positions']} positions: scores are from {'White' if state['pov'] == 'white' else 'the side to move'}'s view "
          f"(agreement {fields['corr_white'] if state['pov'] == 'white' else fields['corr_stm']}), "
          f"1 Stockfish centipawn = {state['scale']:.2f} Tessera centipawns\n", flush=True)
    save_state(args.out, state)


def main():
    args = parse_args()
    if not os.path.exists(args.engine):
        sys.exit(f"Engine not found at {args.engine}")
    if not os.path.exists(args.source):
        sys.exit(f"File not found: {args.source}")
    os.makedirs(args.out, exist_ok=True)
    for name in os.listdir(args.out):                 # pieces of a chunk interrupted last time
        if name.endswith(".tmp"):
            os.unlink(os.path.join(args.out, name))
    state = load_state(args.out)
    if state["scale"] is None:
        calibrate(args, state)
    remaining = args.max - state["records"]
    if remaining <= 0:
        print(f"Already have {state['records']:,} positions (limit {args.max:,}). Raise it with --max to import more.")
        return
    print(f"Importing into {args.out}: {state['records']:,} positions so far, up to {args.max:,}. "
          "Ctrl+C stops; run the same command to continue.", flush=True)

    cmd = [args.engine, "lichess-import", "-", args.out, f"{state['scale']:.6f}", state["pov"],
           str(remaining), str(state["lines"]), str(state["next_chunk"]), str(args.chunk)]
    if state["lines"]:
        print(f"Skipping the {state['lines']:,} lines imported earlier (this takes a few minutes) ...", flush=True)
    proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, bufsize=1 << 20)
    records_before = state["records"]

    truncated = {"flag": False}

    def feed():
        stream = open_source(args.source)
        tail = b""                                     # an unfinished last line is held back
        try:
            while True:
                block = stream.read(1 << 20)
                if not block:
                    if tail:
                        proc.stdin.write(tail + b"\n")   # the file's last line, without a newline
                    break
                block = tail + block
                cut = block.rfind(b"\n") + 1
                tail = block[cut:]
                proc.stdin.write(block[:cut])
        except SourceTruncated:
            truncated["flag"] = True                   # the half line is dropped; the next run reads it whole
        except (BrokenPipeError, OSError, ValueError):
            pass                                       # the engine stopped reading: limit reached or stopped
        finally:
            stream.close()
            try:
                proc.stdin.close()
            except OSError:
                pass

    feeder = threading.Thread(target=feed, daemon=True)
    feeder.start()

    stopped = {"flag": False}

    def on_stop(*_):
        stopped["flag"] = True
        proc.kill()

    signal.signal(signal.SIGINT, on_stop)
    for raw in proc.stdout:
        line = raw.decode(errors="replace")
        parts = dict(p.split("=", 1) for p in line.split()[1:] if "=" in p)
        if line.startswith("chunk"):
            state["lines"] = int(parts["lines"])
            state["records"] += int(parts["records"])
            state["next_chunk"] = int(line.split()[1]) + 1
            save_state(args.out, state)
        if line.startswith(("chunk", "progress")):
            read = int(parts.get("lines", 0))
            print(f"\r{read:>13,} lines read ({100 * read / TOTAL_LINES:4.1f}% of the file)   "
                  f"{records_before + int(parts['kept']) if 'kept' in parts else state['records']:>12,} positions kept   ",
                  end="", flush=True)
    proc.wait()
    print()
    feeder.join(timeout=5)
    if stopped["flag"]:
        print(f"Stopped. {state['records']:,} positions saved; run the same command to continue.")
    elif truncated["flag"]:
        print(f"The file ends in the middle of its data: the download is probably not finished.\n"
              f"{state['records']:,} positions are saved. When the download is complete, run the same command to continue.")
    elif proc.returncode != 0:
        print(f"The engine stopped with an error (code {proc.returncode}). {state['records']:,} positions are saved; "
              "run the same command to continue, and report this if it happens again.")
    else:
        print(f"Done: {state['records']:,} positions in {args.out}.")
        py = "python" if os.name == "nt" else "python3"
        print("Train with them, for example:")
        print(f"  {py} {os.path.join('tools', 'train_net.py')} --data {os.path.join('data', 'round3')} {os.path.relpath(args.out, ROOT)} --out nets-lichess")


if __name__ == "__main__":
    main()
