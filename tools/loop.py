#!/usr/bin/env python3
"""Tessera's self-improvement loop: play, learn, test, repeat. Stop and resume at any time.

    python tools/loop.py                          # start, or continue exactly where it stopped
    python tools/loop.py --first-data data/round3 # first start only: reuse games the current network already played
    python tools/loop.py --status                 # what has happened so far
    Ctrl+C                                        # stop; all finished work is kept

One round:
  1. generate  the best network so far plays games against itself (tools/gen_data.py)
  2. pack      the new games are converted, once, into training records
  3. train     a new network learns, from scratch, from the newest rounds of games
  4. gate      new network vs best network, in a match that stops as soon as the
               result is statistically clear (SPRT on pairs of games)
  5. promote   a network proven stronger becomes the best one and plays the next round.
               One that is not is dropped; its round gets more games and step 3 runs again.

Nothing gets weaker by accident: a network only replaces the best one after winning
the gate match. Everything lives in loop/ (ignored by git); the networks are
loop/nets/v01.nnue, v02.nnue, ...; the newest best one is also copied to loop/best.nnue.

Stopping: Ctrl+C, closing the window or a power cut all lose at most the work in
progress (a few game batches, part of one training pass, a few match games).
Running the same command again continues from there, in whichever step it was.

Settings given on the command line are remembered, so later runs need no options.
Needs Python 3, python-chess (pip install chess), engine/tessera(.exe) and
engine/trainer(.exe); see docs/training.md.
"""
import argparse
import json
import lzma
import math
import os
import random
import secrets
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS = os.path.join(ROOT, "tools")
EXE = ".exe" if os.name == "nt" else ""
ENGINE = os.path.join(ROOT, "engine", "tessera" + EXE)
TRAINER = os.path.join(ROOT, "engine", "trainer" + EXE)
LOOP = os.path.join(ROOT, "loop")
STATE = os.path.join(LOOP, "state.json")
NET_FAILED = "could not load network file"
RECORD_BYTES = 68

DEFAULTS = {
    "positions": 40_000_000,  # new positions per round; the training window doubles what each network learns from
    "more": 20_000_000,       # extra positions for a round whose network failed the gate
    "gen_nodes": 5000,        # average search per move in the games (same as all earlier rounds)
    "window": 2,              # train on the newest this many rounds
    "epochs": 24,
    "hidden": 256,
    "extra": [],              # folders always added to the training data (e.g. data/lichess)
    "match_nodes": 40000,     # search per move in the gate match (about 20 ms)
    "elo0": 0.0, "elo1": 10.0, "alpha": 0.05, "beta": 0.05,
    "max_pairs": 1500,        # 3000 games: a gate that is still undecided then keeps the old network
    "gpu": False,             # train with tools/train_gpu.py on the GPU instead of engine/trainer
}

STOP = threading.Event()      # Ctrl+C


# ---------------------------------------------------------------------------- small helpers

def rel(path):
    """Paths are stored relative to the project when inside it, so the folder can be moved."""
    path = os.path.abspath(path)
    try:
        r = os.path.relpath(path, ROOT)
    except ValueError:            # another drive on Windows
        return path
    return path if r.startswith("..") else r


def absp(path):
    return path if os.path.isabs(path) else os.path.join(ROOT, path)


def save_json(path, obj):
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        json.dump(obj, f, indent=1)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)         # atomic: never half-written, even on a power cut


def load_json(path, default=None):
    if not os.path.exists(path):
        return default
    with open(path) as f:
        return json.load(f)


def copy_atomic(src, dst):
    shutil.copyfile(src, dst + ".tmp")
    os.replace(dst + ".tmp", dst)


def own_group():
    """Child processes that must not receive Ctrl+C themselves (the loop stops them cleanly)."""
    return {"creationflags": subprocess.CREATE_NEW_PROCESS_GROUP} if os.name == "nt" else {"start_new_session": True}


def duration(seconds):
    h, rest = divmod(int(seconds), 3600)
    return f"{h} h {rest // 60:02d} min" if h else f"{rest // 60} min {rest % 60:02d} s"


def log(text):
    stamp = time.strftime("%Y-%m-%d %H:%M")
    print(text, flush=True)
    with open(os.path.join(LOOP, "log.txt"), "a", encoding="utf-8") as f:
        f.write(f"{stamp}  {text}\n")


def acquire_lock():
    """Two loops writing the same files would corrupt them; the lock is released when the process ends."""
    f = open(os.path.join(LOOP, ".lock"), "a+")
    try:
        if os.name == "nt":
            import msvcrt
            f.seek(0)
            msvcrt.locking(f.fileno(), msvcrt.LK_NBLCK, 1)
        else:
            import fcntl
            fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        sys.exit("The loop is already running in another window. Stop that one first (Ctrl+C).")
    return f


def memory_bytes():
    try:
        if os.name == "nt":
            import ctypes

            class MS(ctypes.Structure):
                _fields_ = [("len", ctypes.c_ulong), ("load", ctypes.c_ulong), ("total", ctypes.c_ulonglong),
                            ("avail", ctypes.c_ulonglong), ("a", ctypes.c_ulonglong), ("b", ctypes.c_ulonglong),
                            ("c", ctypes.c_ulonglong), ("d", ctypes.c_ulonglong), ("e", ctypes.c_ulonglong)]
            m = MS()
            m.len = ctypes.sizeof(MS)
            ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(m))
            return m.total
        return os.sysconf("SC_PAGE_SIZE") * os.sysconf("SC_PHYS_PAGES")
    except Exception:
        return 0


# ---------------------------------------------------------------------------- state

def version_dir(v):
    return os.path.join(LOOP, f"v{v:02d}")


def net_path(v):
    return os.path.join(LOOP, "nets", f"v{v:02d}.nnue")


def new_state(args, settings):
    best = os.path.abspath(args.best or os.path.join(ROOT, "engine", "net", "tessera.nnue"))
    if not os.path.isfile(best):
        sys.exit(f"Network not found: {best}")
    os.makedirs(os.path.join(LOOP, "nets"), exist_ok=True)
    copy_atomic(best, net_path(1))
    copy_atomic(best, os.path.join(LOOP, "best.nnue"))
    data = os.path.abspath(args.first_data) if args.first_data else os.path.join(version_dir(1), "games")
    if args.first_data and not os.path.isdir(data):
        sys.exit(f"Folder not found: {data}")
    state = {"format": 1, "settings": settings, "version": 1, "phase": "generate", "attempt": 1,
             "rounds": {"1": {"data": rel(data), "target": settings["positions"], "seconds": {}}},
             "history": [], "started_from": rel(best)}
    save_json(STATE, state)
    log(f"New loop. Version 1 = {rel(best)}" + (f", its games so far: {rel(data)}" if args.first_data else ""))
    return state


def add_seconds(state, phase, seconds):
    sec = state["rounds"][str(state["version"])].setdefault("seconds", {})
    sec[phase] = sec.get(phase, 0) + seconds


# ---------------------------------------------------------------------------- 1. generate

def generate(state, threads):
    v = state["version"]
    rnd = state["rounds"][str(v)]
    s = state["settings"]
    data = absp(rnd["data"])
    print(f"\n== Round {v}, step 1/4: games by network v{v:02d}, {rnd['target']:,} positions in {rel(data)}", flush=True)
    cmd = [sys.executable, os.path.join(TOOLS, "gen_data.py"), "--engine", ENGINE, "--out", data,
           "--target", str(rnd["target"]), "--nodes", str(s["gen_nodes"]), "--net", net_path(v)]
    if threads:
        cmd += ["--threads", str(max(1, threads - 1))]
    code = subprocess.call(cmd, cwd=ROOT)   # it gets Ctrl+C too, saves its finished batches and exits
    have = (load_json(os.path.join(data, "state.json"), {}) or {}).get("positions", 0)
    if have >= rnd["target"]:
        return True
    if code != 0 and not STOP.is_set():
        sys.exit(f"Game generation stopped with an error (code {code}); see the message above.")
    STOP.set()                        # it stopped cleanly before the target: Ctrl+C
    return False


# ---------------------------------------------------------------------------- 2. pack

def text_sources(folder):
    if not os.path.isdir(folder):
        return []
    return [os.path.join(folder, n) for n in sorted(os.listdir(folder)) if n.endswith(".txt") or n.endswith(".txt.xz")]


def pack_folder(src_dir, out_dir):
    """Converts each text file once; a file that changed (more games appended) is converted again."""
    os.makedirs(out_dir, exist_ok=True)
    manifest_path = os.path.join(out_dir, "manifest.json")
    manifest = load_json(manifest_path, {})
    wanted = set()
    for src in text_sources(src_dir):
        name = os.path.basename(src).replace(".txt.xz", "").replace(".txt", "") + ".bin"
        out = os.path.join(out_dir, name)
        wanted.add(name)
        stamp = [os.path.getsize(src), int(os.path.getmtime(src))]
        if manifest.get(name) == stamp and os.path.exists(out):
            continue
        if STOP.is_set():
            return False
        print(f"  converting {rel(src)}", flush=True)
        text, tmp = src, None
        if src.endswith(".xz"):
            fd, tmp = tempfile.mkstemp(suffix=".txt", dir=out_dir)
            with os.fdopen(fd, "wb") as fout, lzma.open(src, "rb") as zin:
                shutil.copyfileobj(zin, fout)
            text = tmp
        try:
            res = subprocess.run([ENGINE, "pack", text, out + ".tmp"], stdout=subprocess.DEVNULL, **own_group())
        finally:
            if tmp:
                os.unlink(tmp)
        if res.returncode != 0:
            sys.exit(f"Converting {src} failed.")
        os.replace(out + ".tmp", out)
        manifest[name] = stamp
        save_json(manifest_path, manifest)
    for name in os.listdir(out_dir):
        if name.endswith(".bin") and name not in wanted:
            os.unlink(os.path.join(out_dir, name))
    return True


def records_in(folder):
    if not os.path.isdir(folder):
        return 0
    return sum(os.path.getsize(os.path.join(folder, n)) for n in os.listdir(folder) if n.endswith(".bin")) // RECORD_BYTES


def pack(state):
    v = state["version"]
    print(f"\n== Round {v}, step 2/4: converting the games for training", flush=True)
    if not pack_folder(absp(state["rounds"][str(v)]["data"]), os.path.join(version_dir(v), "packed")):
        return False
    print(f"  {records_in(os.path.join(version_dir(v), 'packed')):,} positions ready", flush=True)
    return True


# ---------------------------------------------------------------------------- 3. train

def extra_folders(state):
    """Extra data: folders of training records are used as they are, text folders are converted once."""
    out = []
    for folder in state["settings"]["extra"]:
        folder = absp(folder)
        if not os.path.isdir(folder):
            sys.exit(f"Extra data folder not found: {folder}")
        if any(n.endswith(".bin") for n in os.listdir(folder)):
            out.append(folder)
        else:
            packed = os.path.join(LOOP, "extra", os.path.basename(os.path.normpath(folder)))
            if not pack_folder(folder, packed):
                return None
            out.append(packed)
    return out


def train_dir(state):
    return os.path.join(version_dir(state["version"]), f"train-{state['attempt']}")


def checkpoint_epoch(folder, gpu=False):
    if gpu:
        return (load_json(os.path.join(folder, "status.json")) or {}).get("passes_done", 0)   # tools/train_gpu.py
    path = os.path.join(folder, "checkpoint.bin")
    if not os.path.exists(path):
        return 0
    with open(path, "rb") as f:
        head = f.read(12)
    return int.from_bytes(head[8:12], "little") if len(head) == 12 else 0


def train(state, threads):
    v, s = state["version"], state["settings"]
    folders = []
    for k in range(s["window"]):
        packed = os.path.join(version_dir(v - k), "packed")
        if v - k >= 1 and records_in(packed):
            folders.append(packed)
    extra = extra_folders(state)
    if extra is None:
        return False
    folders += extra
    total = sum(records_in(f) for f in folders)
    out = train_dir(state)
    print(f"\n== Round {v}, step 3/4: training network v{v + 1:02d} (attempt {state['attempt']}) on {total:,} positions", flush=True)
    for f in folders:
        print(f"  {rel(f)}: {records_in(f):,}", flush=True)
    if s.get("gpu"):
        cmd = [sys.executable, os.path.join(TOOLS, "train_gpu.py"), "--data", ",".join(folders), "--out", out,
               "--hidden", str(s["hidden"]), "--epochs", str(s["epochs"])]
    else:
        need, have = total * (RECORD_BYTES + 8), memory_bytes()
        print(f"  needs about {need / 2**30:.1f} GB of memory" + (f" (this PC has {have / 2**30:.0f} GB)" if have else ""), flush=True)
        if have and need > 0.9 * have:
            print("  Warning: that is close to all of it; lower --window, remove an --extra-data folder or use --gpu.")
        cmd = [TRAINER, "--data", ",".join(folders), "--out", out, "--hidden", str(s["hidden"]),
               "--epochs", str(s["epochs"]), "--threads", str(threads or os.cpu_count() or 2)]
    code = subprocess.call(cmd)       # it gets Ctrl+C too, saves its checkpoint and exits
    if checkpoint_epoch(out, s.get("gpu")) >= s["epochs"] and os.path.exists(os.path.join(out, "tessera.nnue")):
        return True
    if code != 0 and not STOP.is_set():
        sys.exit(f"Training stopped with an error (code {code}); see the message above.")
    STOP.set()                        # it saved its checkpoint and stopped: Ctrl+C
    return False


# ---------------------------------------------------------------------------- 4. gate

class Engine:
    """One engine process kept open for the whole match (no start-up cost per game)."""

    def __init__(self, net, hash_mb=16):
        # Started in the project folder and given a relative path: the engine reads words of at
        # most 127 characters, which a deep absolute path on Windows can exceed.
        self.proc = subprocess.Popen([ENGINE], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                                     text=True, bufsize=1, cwd=ROOT, **own_group())
        self.send("uci")
        self.wait("uciok")
        self.send(f"setoption name Hash value {hash_mb}")
        self.send(f"setoption name EvalFile value {rel(net)}")
        if any(NET_FAILED in line for line in self.wait("readyok", ask="isready")):
            raise RuntimeError(f"the engine could not load {net}")

    def send(self, line):
        self.proc.stdin.write(line + "\n")
        self.proc.stdin.flush()

    def wait(self, token, ask=None):
        if ask:
            self.send(ask)
        lines = []
        while True:
            line = self.proc.stdout.readline()
            if not line:
                raise RuntimeError("the engine stopped unexpectedly")
            lines.append(line)
            if line.startswith(token):
                return lines

    def new_game(self):
        self.send("ucinewgame")
        self.wait("readyok", ask="isready")

    def go(self, moves, nodes):
        """Best move and score (centipawns, side to move; mates as +-30000)."""
        self.send("position startpos" + (" moves " + " ".join(moves) if moves else ""))
        self.send(f"go nodes {nodes}")
        score = None
        while True:
            line = self.proc.stdout.readline()
            if not line:
                raise RuntimeError("the engine stopped unexpectedly")
            if line.startswith("info") and " score " in line:
                t = line.split()
                i = t.index("score")
                if t[i + 1] == "cp":
                    score = int(t[i + 2])
                elif t[i + 1] == "mate":
                    m = int(t[i + 2])
                    score = 30000 - abs(m) if m > 0 else -30000 + abs(m)
            elif line.startswith("bestmove"):
                return line.split()[1], score

    def close(self):
        try:
            self.send("quit")
            self.proc.wait(timeout=3)
        except Exception:
            self.proc.kill()


def position_key(board):
    """What makes two positions the same for the repetition rule (pieces, side, castling, en passant)."""
    try:
        return board._transposition_key()
    except AttributeError:
        return " ".join(board.fen().split()[:4])


def play_game(white, black, opening, nodes, chess):
    """White's score (1, 0.5, 0), or None if the loop is stopping. Adjudication as in tools/match.py."""
    board = chess.Board()
    for m in opening:
        board.push_uci(m)
    moves = list(opening)
    white.new_game()
    black.new_game()
    win_streak = [0, 0]
    draw_streak = 0
    seen = {}                    # position -> times it occurred (since the last pawn move or capture)
    while True:
        if STOP.is_set():
            return None
        if board.halfmove_clock == 0:
            seen.clear()
        key = position_key(board)
        seen[key] = seen.get(key, 0) + 1
        if not any(True for _ in board.generate_legal_moves()):
            return (0.0 if board.turn == chess.WHITE else 1.0) if board.is_check() else 0.5
        if seen[key] >= 3 or board.halfmove_clock >= 100 or board.is_insufficient_material():
            return 0.5
        side = 0 if board.turn == chess.WHITE else 1
        mv, cp = (white if side == 0 else black).go(moves, nodes)
        try:
            move = chess.Move.from_uci(mv)
        except ValueError:
            move = None
        if move is None or move not in board.legal_moves:
            return 0.0 if side == 0 else 1.0          # an illegal move loses
        if cp is not None:
            win_streak[side] = win_streak[side] + 1 if cp >= 900 else 0
            if cp <= -900 and win_streak[1 - side] >= 4:   # both sides agree it is lost
                return 0.0 if side == 0 else 1.0
            draw_streak = draw_streak + 1 if abs(cp) <= 8 and board.fullmove_number > 40 else 0
            if draw_streak >= 16:
                return 0.5
        board.push(move)
        moves.append(mv)
        if board.fullmove_number > 300:
            return 0.5


def make_opening(rng, engine, chess, openings):
    """A book line plus two random moves, kept only if the engine finds it roughly balanced.
    Every pair of games gets its own opening, so no game is ever repeated."""
    moves = []
    for _ in range(50):
        board = chess.Board()
        for san in rng.choice(openings).split():
            board.push_san(san)
        for _ in range(2):
            legal = list(board.legal_moves)
            if not legal:
                break
            board.push(rng.choice(legal))
        if board.is_game_over():
            continue
        moves = [m.uci() for m in board.move_stack]
        engine.new_game()
        _, cp = engine.go(moves, 5000)
        if cp is not None and abs(cp) <= 120:
            return moves
    return moves


PAIR_SCORES = [0.0, 0.25, 0.5, 0.75, 1.0]      # mean score of the new network over a pair of games


def sprt(penta, s):
    """Elo estimate, its 95% error, and the log-likelihood ratio of elo1 against elo0 (normal
    approximation on pairs of games, as on fishtest)."""
    n = sum(penta)
    if n == 0:
        return 0.0, 0.0, 0.0
    mean = sum(c * x for c, x in zip(penta, PAIR_SCORES)) / n
    var = sum(c * (x - mean) ** 2 for c, x in zip(penta, PAIR_SCORES)) / n
    var = max(var, 1e-4)

    def score(elo):
        return 1 / (1 + 10 ** (-elo / 400))

    def elo(x):
        x = min(max(x, 1e-4), 1 - 1e-4)
        return -400 * math.log10(1 / x - 1)

    s0, s1 = score(s["elo0"]), score(s["elo1"])
    llr = n * (s1 - s0) * (2 * mean - s0 - s1) / (2 * var)
    se = math.sqrt(var / n)
    return elo(mean), (elo(mean + 1.96 * se) - elo(mean - 1.96 * se)) / 2, llr


def gate(state, threads, cand_net=None, best_net=None, gate_file=None, label=None):
    """Plays pairs of games (same opening, colors swapped) until the SPRT decides.
    Returns 'pass', 'fail', or None when stopped. Progress is saved after every pair."""
    try:
        import chess
    except ImportError:
        sys.exit("The gate match needs python-chess:   pip install chess")
    sys.path.insert(0, TOOLS)
    from match import OPENINGS

    s, v = state["settings"], state["version"]
    cand_net = cand_net or os.path.join(train_dir(state), "tessera.nnue")
    best_net = best_net or net_path(v)
    gate_file = gate_file or os.path.join(version_dir(v), f"gate-{state['attempt']}.json")
    label = label or f"v{v + 1:02d} (new) vs v{v:02d} (best)"
    g = load_json(gate_file) or {"seed": secrets.randbelow(2**31), "next_pair": 0, "penta": [0] * 5,
                                 "wdl": [0, 0, 0], "decision": None}
    if g["decision"]:
        return g["decision"]
    lower = math.log(s["beta"] / (1 - s["alpha"]))
    upper = math.log((1 - s["beta"]) / s["alpha"])
    print(f"\n== Round {v}, step 4/4: gate match {label}, {s['match_nodes']:,} nodes per move", flush=True)
    print(f"  stops when sure whether the new network is at least {s['elo1']:+g} Elo better (LLR reaches "
          f"{upper:.2f}) or not better (LLR reaches {lower:.2f}); at most {2 * s['max_pairs']:,} games", flush=True)

    lock = threading.Lock()
    done_evt = threading.Event()
    errors = []
    run = {"in_flight": 0, "pairs": 0}

    def decide():
        _, _, llr = sprt(g["penta"], s)
        n = sum(g["penta"])
        if n >= 20 and llr >= upper:
            return "pass"
        if n >= 20 and llr <= lower:
            return "fail"
        if n >= s["max_pairs"]:
            return "fail"           # still unclear: keep the network that is known to be good
        return None

    def worker():
        engines = []
        try:
            cand, best = Engine(cand_net), Engine(best_net)
            engines = [cand, best]
            while not STOP.is_set() and not done_evt.is_set():
                with lock:
                    if sum(g["penta"]) + run["in_flight"] >= s["max_pairs"]:
                        break
                    pair = g["next_pair"]
                    g["next_pair"] += 1
                    run["in_flight"] += 1
                try:
                    rng = random.Random(f"{g['seed']}-{pair}")
                    opening = make_opening(rng, best, chess, OPENINGS)
                    a = play_game(cand, best, opening, s["match_nodes"], chess)
                    b = None if a is None else play_game(best, cand, opening, s["match_nodes"], chess)
                finally:
                    with lock:
                        run["in_flight"] -= 1
                if a is None or b is None:
                    break                    # stopping: this unfinished pair is not counted
                with lock:
                    total = a + (1.0 - b)
                    g["penta"][int(round(total * 2))] += 1
                    for x in (a, 1.0 - b):
                        g["wdl"][0 if x == 1 else 1 if x == 0.5 else 2] += 1
                    run["pairs"] += 1
                    decision = decide()
                    if decision:
                        g["decision"] = decision
                        done_evt.set()
                    save_json(gate_file, g)
        except Exception as exc:
            errors.append(str(exc))
            done_evt.set()
        finally:
            for e in engines:
                e.close()

    workers = [threading.Thread(target=worker, daemon=True) for _ in range(threads or os.cpu_count() or 2)]
    start = time.time()
    for t in workers:
        t.start()
    shown = -1
    while any(t.is_alive() for t in workers):
        time.sleep(0.5)
        with lock:
            n = sum(g["penta"])
            if n != shown:
                shown = n
                elo, err, llr = sprt(g["penta"], s)
                w, d, l = g["wdl"]
                rate = run["pairs"] / max(time.time() - start, 1e-3)
                print(f"\r  {2 * n:5d} games  +{w} ={d} -{l}  Elo {elo:+6.1f} +-{err:5.1f}  LLR {llr:+5.2f} "
                      f"[{lower:.2f}, {upper:.2f}]  {120 * rate:5.1f} games/min   ", end="", flush=True)
    for t in workers:
        t.join()
    print(flush=True)
    if errors and not g["decision"]:
        sys.exit("Gate match failed: " + errors[0])
    if not g["decision"] and not STOP.is_set():
        g["decision"] = decide() or "fail"
        save_json(gate_file, g)
    return g["decision"]


# ---------------------------------------------------------------------------- 5. promote

def finish_gate(state, decision):
    v, s = state["version"], state["settings"]
    g = load_json(os.path.join(version_dir(v), f"gate-{state['attempt']}.json"))
    elo, err, llr = sprt(g["penta"], s)
    games = 2 * sum(g["penta"])
    entry = {"round": v, "attempt": state["attempt"], "games": games, "elo": round(elo, 1), "error": round(err, 1),
             "llr": round(llr, 2), "result": decision, "date": time.strftime("%Y-%m-%d %H:%M"),
             "positions": records_in(os.path.join(version_dir(v), "packed"))}
    if decision == "pass":
        copy_atomic(os.path.join(train_dir(state), "tessera.nnue"), net_path(v + 1))
        copy_atomic(net_path(v + 1), os.path.join(LOOP, "best.nnue"))
        state["history"].append(entry)
        state["version"] = v + 1
        state["attempt"] = 1
        state["rounds"][str(v + 1)] = {"data": rel(os.path.join(version_dir(v + 1), "games")),
                                       "target": s["positions"], "seconds": {}}
        log(f"PROMOTED: v{v + 1:02d} beat v{v:02d}, Elo {elo:+.1f} +-{err:.1f} over {games} games. "
            f"New best network: loop/nets/v{v + 1:02d}.nnue (also loop/best.nnue)")
    else:
        state["history"].append(entry)
        state["attempt"] += 1
        state["rounds"][str(v)]["target"] += s["more"]
        log(f"Not better: attempt {state['attempt'] - 1} for v{v + 1:02d} scored Elo {elo:+.1f} +-{err:.1f} over {games} games. "
            f"Kept v{v:02d}; its round gets {s['more']:,} more positions and the network is trained again.")
    state["phase"] = "generate"
    save_json(STATE, state)


# ---------------------------------------------------------------------------- main

def show_status(state):
    s = state["settings"]
    v = state["version"]
    print(f"Best network: v{v:02d}  (loop/nets/v{v:02d}.nnue)   started from {state['started_from']}")
    print(f"Now: round {v}, step '{state['phase']}', attempt {state['attempt']}")
    data = absp(state["rounds"][str(v)]["data"])
    have = (load_json(os.path.join(data, "state.json"), {}) or {}).get("positions", 0)
    print(f"Games of this round: {have:,} of {state['rounds'][str(v)]['target']:,} positions in {rel(data)}")
    print("Settings: " + ", ".join(f"{k}={s[k]}" for k in DEFAULTS))
    if state["history"]:
        print("\nround attempt   games      Elo        result    date")
        for h in state["history"]:
            print(f"{h['round']:5d} {h['attempt']:7d} {h['games']:7d}  {h['elo']:+6.1f} +-{h['error']:<5.1f}  "
                  f"{'promoted' if h['result'] == 'pass' else 'kept old':9s} {h['date']}")
    for key in sorted(state["rounds"], key=int):
        sec = state["rounds"][key].get("seconds", {})
        if sec:
            print(f"time in round {key}: " + ", ".join(f"{p} {duration(t)}" for p, t in sec.items()))


def parse_args():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--status", action="store_true", help="show progress and history, then exit")
    ap.add_argument("--rounds", type=int, default=0, help="stop after this many gate matches in this run (0 = never)")
    ap.add_argument("--threads", type=int, default=0, help="CPU threads to use (default: all)")
    first = ap.add_argument_group("first start only")
    first.add_argument("--best", help="network to start from (default: the one built into the engine)")
    first.add_argument("--first-data", help="folder of games the starting network already played (e.g. data/round3)")
    st = ap.add_argument_group("settings (remembered; give one again to change it)")
    st.add_argument("--positions", type=int, help=f"new positions per round (default {DEFAULTS['positions']:,})")
    st.add_argument("--more", type=int, help=f"positions added after a failed gate (default {DEFAULTS['more']:,})")
    st.add_argument("--gen-nodes", type=int, help=f"search per move in the games (default {DEFAULTS['gen_nodes']})")
    st.add_argument("--window", type=int, help=f"train on the newest N rounds (default {DEFAULTS['window']})")
    st.add_argument("--epochs", type=int, help=f"training passes (default {DEFAULTS['epochs']})")
    st.add_argument("--hidden", type=int, help=f"network size (default {DEFAULTS['hidden']})")
    st.add_argument("--extra-data", nargs="*", metavar="FOLDER", help="folders always added to training (e.g. data/lichess); give none to clear")
    st.add_argument("--match-nodes", type=int, help=f"search per move in the gate match (default {DEFAULTS['match_nodes']})")
    st.add_argument("--elo1", type=float, help=f"Elo gain the gate tests for (default {DEFAULTS['elo1']:g})")
    st.add_argument("--max-pairs", type=int, help=f"gate limit in pairs of games (default {DEFAULTS['max_pairs']})")
    st.add_argument("--gpu", action="store_true", help="train on the GPU with tools/train_gpu.py (needs PyTorch with CUDA)")
    st.add_argument("--cpu", action="store_true", help="train with engine/trainer on the CPU again")
    return ap.parse_args()


def main():
    args = parse_args()
    os.makedirs(LOOP, exist_ok=True)
    state = load_json(STATE)
    if args.status:
        if not state:
            sys.exit("The loop has not been started yet.")
        show_status(state)
        return
    for path, hint in ((ENGINE, "the engine"), (TRAINER, "the trainer")):
        if path == TRAINER and (args.gpu or (state and state["settings"].get("gpu"))) and not args.cpu:
            continue
        if not os.path.exists(path):
            sys.exit(f"Missing {path} ({hint}). See docs/training.md for how to build or download it.")
    _lock = acquire_lock()

    changes = {k: getattr(args, k) for k in ("positions", "more", "gen_nodes", "window", "epochs", "hidden",
                                             "match_nodes", "elo1", "max_pairs") if getattr(args, k) is not None}
    if args.extra_data is not None:
        changes["extra"] = [rel(f) for f in args.extra_data]
    if args.gpu or args.cpu:
        changes["gpu"] = bool(args.gpu)
    if state is None:
        state = new_state(args, {**DEFAULTS, **changes})
    else:
        if args.best or args.first_data:
            print("Note: --best and --first-data only matter on the first start; the loop is already running from "
                  f"{state['started_from']}.")
        settings = {**DEFAULTS, **state["settings"]}
        for k, val in changes.items():
            if settings.get(k) != val:
                log(f"Setting changed: {k} = {val}")
                if k == "positions" and state["attempt"] == 1:
                    state["rounds"][str(state["version"])]["target"] = val
        settings.update(changes)
        state["settings"] = settings
        save_json(STATE, state)

    def on_ctrl_c(*_):
        if not STOP.is_set():
            print("\nStopping: saving the work in progress ... (run the same command again to continue)", flush=True)
        STOP.set()

    signal.signal(signal.SIGINT, on_ctrl_c)
    if hasattr(signal, "SIGTERM"):
        signal.signal(signal.SIGTERM, on_ctrl_c)

    print(f"Self-improvement loop. Best network: v{state['version']:02d}. Ctrl+C stops it; run the same command to continue.")
    gates = 0
    steps = {"generate": (lambda: generate(state, args.threads), "pack"),
             "pack": (lambda: pack(state), "train"),
             "train": (lambda: train(state, args.threads), "gate")}
    while not STOP.is_set():
        phase = state["phase"]
        t0 = time.time()
        if phase in steps:
            fn, nxt = steps[phase]
            ok = fn()
            add_seconds(state, phase, time.time() - t0)
            if ok:
                state["phase"] = nxt
            save_json(STATE, state)
            if not ok:
                break
            continue
        decision = gate(state, args.threads)
        add_seconds(state, "gate", time.time() - t0)
        save_json(STATE, state)
        if decision is None:
            break
        finish_gate(state, decision)
        gates += 1
        if args.rounds and gates >= args.rounds:
            print(f"Done: {gates} gate match(es) this run, as asked with --rounds.")
            break
    if STOP.is_set():
        print(f"Stopped in round {state['version']}, step '{state['phase']}'. Everything finished is saved.")


if __name__ == "__main__":
    main()
