#!/usr/bin/env python3
"""Train Tessera's neural network on the self-play data. Safe to stop and resume.

    python3 tools/train_net.py          # start, or continue where you stopped
    Ctrl+C                              # stop; progress is saved

What it does:
  1. Converts every data file to the compact form the trainer reads
     (data/*.txt.xz and data/local/*.txt  ->  data/packed/*.bin). Files that
     have not changed since the last run are skipped.
  2. Runs the trainer (CPU, all threads). After every pass over the data it
     checks the network on games it never trained on and keeps the best one
     in nets/tessera.nnue.

Needs the engine and trainer binaries:
    Linux / WSL:  make -C engine && make -C engine trainer
    Windows:      download tessera.exe and trainer.exe into engine\\ (see docs/training.md)
"""
import argparse
import json
import lzma
import os
import shutil
import signal
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = ".exe" if os.name == "nt" else ""


def parse_args():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--hidden", type=int, default=128, help="size of the network's hidden layer (1..256)")
    ap.add_argument("--epochs", type=int, default=24, help="passes over the data")
    ap.add_argument("--threads", type=int, default=os.cpu_count() or 2)
    ap.add_argument("--out", default=os.path.join(ROOT, "nets"), help="where the network and checkpoint go")
    ap.add_argument("--fresh", action="store_true", help="forget earlier training progress and start over")
    return ap.parse_args()


def need(path, hint):
    if not os.path.exists(path):
        sys.exit(f"Missing {path}\n{hint}")
    return path


def sources():
    """All data files: the compressed ones in the repository and the ones generated locally."""
    found = []
    for folder in (os.path.join(ROOT, "data"), os.path.join(ROOT, "data", "local")):
        if not os.path.isdir(folder):
            continue
        for name in sorted(os.listdir(folder)):
            if name.endswith(".txt") or name.endswith(".txt.xz"):
                found.append(os.path.join(folder, name))
    return found


def pack_all(engine, packed_dir):
    os.makedirs(packed_dir, exist_ok=True)
    manifest_path = os.path.join(packed_dir, "manifest.json")
    manifest = {}
    if os.path.exists(manifest_path):
        with open(manifest_path) as f:
            manifest = json.load(f)
    wanted = set()
    for src in sources():
        name = os.path.basename(src).replace(".txt.xz", "").replace(".txt", "") + ".bin"
        out = os.path.join(packed_dir, name)
        wanted.add(name)
        stamp = [os.path.getsize(src), int(os.path.getmtime(src))]
        if manifest.get(name) == stamp and os.path.exists(out):
            continue
        print(f"Converting {os.path.relpath(src, ROOT)} ...", flush=True)
        text = src
        tmp = None
        if src.endswith(".xz"):
            tmp = tempfile.NamedTemporaryFile(delete=False, suffix=".txt")
            with lzma.open(src, "rb") as zin:
                shutil.copyfileobj(zin, tmp)
            tmp.close()
            text = tmp.name
        try:
            subprocess.run([engine, "pack", text, out + ".tmp"], check=True)
            os.replace(out + ".tmp", out)
        finally:
            if tmp:
                os.unlink(tmp.name)
        manifest[name] = stamp
        with open(manifest_path, "w") as f:
            json.dump(manifest, f)
    # Drop packed files whose source no longer exists.
    for name in os.listdir(packed_dir):
        if name.endswith(".bin") and name not in wanted:
            os.unlink(os.path.join(packed_dir, name))
    total = sum(os.path.getsize(os.path.join(packed_dir, n)) for n in os.listdir(packed_dir) if n.endswith(".bin")) // 68
    print(f"{total:,} positions ready.\n", flush=True)
    return total


def main():
    args = parse_args()
    engine = need(os.path.join(ROOT, "engine", "tessera" + EXE), "Build it:  make -C engine   (Windows: see docs/training.md)")
    trainer = need(os.path.join(ROOT, "engine", "trainer" + EXE), "Build it:  make -C engine trainer   (Windows: see docs/training.md)")
    packed = os.path.join(ROOT, "data", "packed")
    pack_all(engine, packed)
    if args.fresh and os.path.isdir(args.out):
        for name in ("checkpoint.bin", "tessera.nnue"):
            path = os.path.join(args.out, name)
            if os.path.exists(path):
                os.unlink(path)

    cmd = [trainer, "--data", packed, "--out", args.out, "--hidden", str(args.hidden),
           "--epochs", str(args.epochs), "--threads", str(args.threads)]
    proc = subprocess.Popen(cmd)
    # Ctrl+C goes to the trainer as well; it finishes its current step, saves and exits. Just wait for it.
    signal.signal(signal.SIGINT, lambda *_: None)
    code = proc.wait()
    net = os.path.join(args.out, "tessera.nnue")
    if code == 0 and os.path.exists(net):
        rel = os.path.relpath(net, ROOT)
        print(f"\nNetwork: {rel}")
        print("Compare it with the hand-written evaluation (needs: pip install chess):")
        print(f'  python3 tools/match.py --e1 engine/tessera{EXE} --e2 engine/tessera{EXE} --opt1 "EvalFile={rel}" --movetime 100 --games 200 --concurrency 4')
    sys.exit(code)


if __name__ == "__main__":
    main()
