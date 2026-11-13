#!/usr/bin/env python3
"""GPU trainer for Tessera's neural network (PyTorch).

Trains exactly the network tools/trainer.cpp trains (same inputs, targets, loss, Adam
settings, learning-rate decay, weight limits and .nnue output), with two differences:

  * it runs on an NVIDIA GPU (CUDA), many times faster than the CPU trainer;
  * it reads the training records from disk as it goes, in shuffled blocks, instead of
    loading them all into memory, so the amount of data is limited by disk space, not RAM.

    python tools/train_gpu.py --data data/packed-round3 data/lichess --out nets-gpu
    python tools/train_net.py --data data/round3 data/lichess --out nets-gpu --gpu   # same, converting text folders first
    Ctrl+C                                    # stops after the current step and saves; the same command continues

Input: folders of *.bin files made by `tessera pack` or tools/import_lichess.py.
Output: <out>/tessera.nnue (the best network so far, by held-out loss)
        <out>/checkpoint-gpu.pt (everything needed to continue), <out>/status.json

Needs PyTorch with CUDA (see docs/training.md). Without a GPU it still works, on the
CPU, but then the C++ trainer is faster.

How the data is read: every file keeps its last 2% as held-out positions, as in the C++
trainer. The rest is cut into blocks of 16,384 consecutive records. Each pass shuffles
the blocks, loads them 256 at a time (about 4 million positions, 280 MB) while the GPU
works on the previous group, and shuffles the positions inside each group. So every
batch mixes positions from hundreds of places in the data, and the order of a pass
depends only on the pass number, which lets an interrupted pass continue exactly.
"""
import argparse
import json
import os
import signal
import sys
import threading
import time

import numpy as np

try:
    import torch
except ImportError:
    sys.exit("PyTorch is not installed. See docs/training.md (GPU training) for the install command.")

INPUTS = 768
PAD = INPUTS                    # index of the always-zero row used for unused input slots
QA, QB = 255, 64
SCALE = 181.0                   # centipawns per unit of network output, as in the engine
RECORD = np.dtype([("inp", "<u2", (32,)), ("score", "<i2"), ("stm", "u1"), ("res", "u1")])
assert RECORD.itemsize == 68
BLOCK = 16384                   # records read in one piece
GROUP = 256                     # blocks shuffled together (about 4.2 million positions)
STOP = threading.Event()


# ---------------------------------------------------------------------------- data

def list_files(folders):
    files = []
    for folder in folders:
        if not os.path.isdir(folder):
            sys.exit(f"Data folder not found: {folder}")
        here = sorted(n for n in os.listdir(folder) if n.endswith(".bin"))
        files += [(os.path.join(folder, n), os.path.getsize(os.path.join(folder, n)) // RECORD.itemsize) for n in here]
    files = [(p, n) for p, n in files if n > 0]
    if not files:
        sys.exit("No training records (*.bin) in " + ", ".join(folders) + ". Convert text data with tools/train_net.py.")
    return files


def make_blocks(files):
    blocks = []
    for fi, (_, n) in enumerate(files):
        train_n = n - n // 50
        for s in range(0, train_n, BLOCK):
            blocks.append((fi, s, min(s + BLOCK, train_n)))
    return blocks


def epoch_plan(blocks, epoch, batch):
    """Groups of block numbers for one pass, and how many batches each group gives."""
    order = np.random.default_rng(1000 + epoch).permutation(len(blocks))
    groups = [order[i:i + GROUP] for i in range(0, len(order), GROUP)]
    counts = [sum(blocks[b][2] - blocks[b][1] for b in g) // batch for g in groups]
    return groups, counts


class Reader:
    """Reads records with plain file reads (fast, and other threads keep running meanwhile)."""

    def __init__(self, files):
        self.paths = [p for p, _ in files]
        self.handles = {}

    def read(self, fi, start, end, out, at):
        f = self.handles.get(fi)
        if f is None:
            f = self.handles[fi] = open(self.paths[fi], "rb")
        f.seek(start * RECORD.itemsize)
        view = memoryview(out)[at * RECORD.itemsize:(at + end - start) * RECORD.itemsize]
        if f.readinto(view) != len(view):
            raise IOError(f"{self.paths[fi]} is shorter than expected")

    def close(self):
        for f in self.handles.values():
            f.close()


def split_fields(raw):
    rec = raw.view(RECORD)
    return (np.ascontiguousarray(rec["inp"]).view(np.int16), np.ascontiguousarray(rec["score"]),
            np.ascontiguousarray(rec["stm"]), np.ascontiguousarray(rec["res"]))


def load_group(reader, blocks, group, seed):
    n = sum(blocks[b][2] - blocks[b][1] for b in group)
    raw = np.empty(n * RECORD.itemsize, dtype=np.uint8)
    at = 0
    for b in sorted(group):                 # in file order: fewer seeks
        fi, s, e = blocks[b]
        reader.read(fi, s, e, raw, at)
        at += e - s
    fields = split_fields(raw)
    perm = np.random.default_rng(seed).permutation(n)
    return fields, perm


def validation_tails(files):
    return [(fi, n - n // 50, n) for fi, (_, n) in enumerate(files) if n // 50 > 0]


# ---------------------------------------------------------------------------- network

class Net(torch.nn.Module):
    def __init__(self, hidden, seed=20261111):
        super().__init__()
        g = torch.Generator().manual_seed(seed)
        self.ft = torch.nn.EmbeddingBag(INPUTS + 1, hidden, mode="sum", padding_idx=PAD)
        with torch.no_grad():
            self.ft.weight.uniform_(-0.1, 0.1, generator=g)
            self.ft.weight[PAD].zero_()
        self.ft_b = torch.nn.Parameter(torch.full((hidden,), 0.2))
        self.out_w = torch.nn.Parameter(torch.empty(2 * hidden).uniform_(-0.05, 0.05, generator=g))
        self.out_b = torch.nn.Parameter(torch.zeros(1))
        self.hidden = hidden

    def forward(self, own, opp):
        a = torch.clamp(self.ft(own) + self.ft_b, 0.0, 1.0)
        b = torch.clamp(self.ft(opp) + self.ft_b, 0.0, 1.0)
        h = self.hidden
        return (a * a) @ self.out_w[:h] + (b * b) @ self.out_w[h:] + self.out_b

    def clip(self):
        """Keep the hidden layer inside what 16-bit integers can hold after quantisation."""
        with torch.no_grad():
            self.ft.weight.clamp_(-3.9, 3.9)
            self.ft_b.clamp_(-3.0, 3.0)


def batch_tensors(inp, score, stm, res, device):
    """Inputs as seen by the side to move and by the other side, and the training target."""
    w = inp.to(device, non_blocking=True).long()
    stm = stm.to(device, non_blocking=True)
    score = score.to(device, non_blocking=True).float()
    res = res.to(device, non_blocking=True).float()
    pad = w >= INPUTS
    color, rest = w // 384, w % 384
    mirrored = (1 - color) * 384 + (rest // 64) * 64 + ((rest % 64) ^ 56)   # the same piece seen from Black's side
    mirrored = torch.where(pad, torch.full_like(w, PAD), mirrored)
    w = torch.where(pad, torch.full_like(w, PAD), w)
    black = (stm == 1).unsqueeze(1)
    own = torch.where(black, mirrored, w)
    opp = torch.where(black, w, mirrored)
    black = black.squeeze(1)
    result = res * 0.5
    result = torch.where(black, 1.0 - result, result)
    has_score = score != 32767
    squashed = torch.sigmoid(torch.where(black, -score, score) / SCALE)
    return own, opp, result, squashed, has_score, res == 255


def loss_terms(net, own, opp, result, squashed, has_score, no_result, result_weight):
    """Sum of squared errors and the number of positions it covers (as tools/trainer.cpp)."""
    pred = torch.sigmoid(net(own, opp))
    blend = result_weight * result + (1.0 - result_weight) * squashed
    target = torch.where(has_score, torch.where(no_result, squashed, blend), result)
    valid = (has_score | ~no_result).float()
    return ((pred - target) ** 2 * valid).sum(), valid.sum()


def export_network(path, net):
    def q(x, mult, lo=-32767, hi=32767):
        x = x.detach().double().cpu().numpy() * mult
        x = np.where(x >= 0, np.floor(x + 0.5), -np.floor(-x + 0.5))      # rounds like C's lround
        return np.clip(x, lo, hi)

    h = net.hidden
    head = np.array([1, h, int(SCALE), QA, QB], dtype="<u4").tobytes()
    body = (q(net.ft.weight[:INPUTS], QA).astype("<i2").tobytes() + q(net.ft_b, QA).astype("<i2").tobytes()
            + q(net.out_w, QB).astype("<i2").tobytes()
            + q(net.out_b, QA * QB, -2**31, 2**31 - 1).astype("<i4").tobytes())
    write_atomic(path, b"TSNN" + head + body)


def import_network(path, hidden):
    """Loads a .nnue file into a float network (used by the tests and for warm starts)."""
    data = open(path, "rb").read()
    assert data[:4] == b"TSNN"
    _, h, _, qa, qb = np.frombuffer(data[4:24], dtype="<u4")
    assert h == hidden, f"{path} has {h} hidden units, not {hidden}"
    o = 24
    ftw = np.frombuffer(data, "<i2", INPUTS * h, o); o += 2 * INPUTS * h
    ftb = np.frombuffer(data, "<i2", h, o); o += 2 * h
    outw = np.frombuffer(data, "<i2", 2 * h, o); o += 4 * h
    outb = np.frombuffer(data, "<i4", 1, o)
    net = Net(hidden)
    with torch.no_grad():
        net.ft.weight[:INPUTS] = torch.from_numpy(ftw.reshape(INPUTS, h) / qa).float()
        net.ft.weight[PAD].zero_()
        net.ft_b.copy_(torch.from_numpy(ftb / qa).float())
        net.out_w.copy_(torch.from_numpy(outw / qb).float())
        net.out_b.copy_(torch.from_numpy(outb / (qa * qb)).float())
    return net


def write_atomic(path, data):
    with open(path + ".tmp", "wb") as f:
        f.write(data)
        f.flush()
        os.fsync(f.fileno())
    os.replace(path + ".tmp", path)


def save_checkpoint(path, ck, net, opt):
    tmp = path + ".tmp"
    torch.save({**ck, "model": net.state_dict(), "optim": opt.state_dict()}, tmp)
    os.replace(tmp, path)


# ---------------------------------------------------------------------------- training

def validation_loss(net, files, device, result_weight):
    reader = Reader(files)
    total, count = 0.0, 0.0
    chunk = 65536
    with torch.no_grad():
        for fi, start, end in validation_tails(files):
            for s in range(start, end, chunk):
                e = min(s + chunk, end)
                raw = np.empty((e - s) * RECORD.itemsize, dtype=np.uint8)
                reader.read(fi, s, e, raw, 0)
                inp, score, stm, res = (torch.from_numpy(a) for a in split_fields(raw))
                t, c = loss_terms(net, *batch_tensors(inp, score, stm, res, device), result_weight)
                total += t.item()
                count += c.item()
    reader.close()
    return total / max(count, 1.0)


def parse_args():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--data", nargs="+", required=True, metavar="FOLDER", help="folders of *.bin records (commas also work)")
    ap.add_argument("--out", required=True, help="where the network and checkpoint go")
    ap.add_argument("--hidden", type=int, default=256, help="hidden units per side (1..256; the engine ships 256)")
    ap.add_argument("--epochs", type=int, default=24, help="passes over the data")
    ap.add_argument("--batch", type=int, default=16384, help="positions per step (same as the C++ trainer)")
    ap.add_argument("--lr", type=float, default=0.001)
    ap.add_argument("--lr-end", type=float, default=0.00003)
    ap.add_argument("--result-weight", type=float, default=0.2, help="share of the game result in the target")
    ap.add_argument("--device", default="auto", help="cuda, cpu or auto (default: the GPU if there is one)")
    ap.add_argument("--fresh", action="store_true", help="forget the checkpoint and start over")
    args = ap.parse_args()
    args.data = [p for item in args.data for p in item.split(",") if p]
    if not 1 <= args.hidden <= 256:
        sys.exit("--hidden must be 1..256")
    return args


def main():
    args = parse_args()
    if args.device == "auto":
        args.device = "cuda" if torch.cuda.is_available() else "cpu"
    device = torch.device(args.device)
    if device.type == "cuda":
        print(f"GPU: {torch.cuda.get_device_name(device)}")
    else:
        torch.set_num_threads(os.cpu_count() or 2)
        print("No CUDA GPU found: training on the CPU (slow; the C++ trainer is faster there). "
              "For the GPU, install the CUDA build of PyTorch: see docs/training.md.")

    files = list_files(args.data)
    blocks = make_blocks(files)
    signature = [[os.path.basename(p), n] for p, n in files]
    total = sum(n for _, n in files)
    train_n = sum(b[2] - b[1] for b in blocks)
    print(f"{total:,} positions in {len(files)} files: {train_n:,} for training, {total - train_n:,} held out")

    os.makedirs(args.out, exist_ok=True)
    ck_path = os.path.join(args.out, "checkpoint-gpu.pt")
    net_path = os.path.join(args.out, "tessera.nnue")
    status_path = os.path.join(args.out, "status.json")
    net = Net(args.hidden).to(device)
    fused = {"fused": True} if device.type == "cuda" else {}
    opt = torch.optim.Adam(net.parameters(), lr=args.lr, betas=(0.9, 0.999), eps=1e-8, **fused)
    ck = {"format": 1, "hidden": args.hidden, "epoch": 0, "batch": 0, "best_val": 1e9, "best_epoch": 0, "data": signature}
    if os.path.exists(ck_path) and not args.fresh:
        saved = torch.load(ck_path, map_location=device, weights_only=False)
        if saved.get("hidden") == args.hidden:
            net.load_state_dict(saved["model"])
            opt.load_state_dict(saved["optim"])
            ck = {k: saved[k] for k in ck}
            if saved["data"] != signature and ck["batch"]:
                print("the data changed since the last run: restarting the interrupted pass")
                ck["batch"] = 0
            ck["data"] = signature
            print(f"Continuing: pass {ck['epoch'] + 1} of {args.epochs}")
        else:
            print("checkpoint has a different --hidden: starting fresh")
    else:
        print(f"Starting: hidden {args.hidden}, {args.epochs} passes")

    def on_signal(*_):
        STOP.set()
    signal.signal(signal.SIGINT, on_signal)
    if hasattr(signal, "SIGTERM"):
        signal.signal(signal.SIGTERM, on_signal)

    def status(finished):
        write_atomic(status_path, json.dumps({"passes_done": ck["epoch"], "epochs": args.epochs, "finished": finished,
                                              "best_val": ck["best_val"], "best_epoch": ck["best_epoch"]}).encode())

    reader = Reader(files)
    from concurrent.futures import ThreadPoolExecutor
    pool = ThreadPoolExecutor(max_workers=1)
    while ck["epoch"] < args.epochs and not STOP.is_set():
        epoch = ck["epoch"]
        groups, counts = epoch_plan(blocks, epoch, args.batch)
        batches = sum(counts)
        progress = epoch / (args.epochs - 1) if args.epochs > 1 else 0.0
        lr = args.lr * (args.lr_end / args.lr) ** progress           # smooth decay from lr to lr-end
        for g in opt.param_groups:
            g["lr"] = lr
        # find the group the saved batch number falls into
        done, gi = 0, 0
        while gi < len(groups) and done + counts[gi] <= ck["batch"]:
            done += counts[gi]
            gi += 1
        seed = lambda i: (1000 + epoch) * 100003 + i                 # noqa: E731
        future = pool.submit(load_group, reader, blocks, groups[gi], seed(gi)) if gi < len(groups) else None
        loss_sum = torch.zeros((), device=device)
        loss_n = torch.zeros((), device=device)
        start, first = time.time(), ck["batch"]
        while future is not None and not STOP.is_set():
            (inp, score, stm, res), perm = future.result()
            nxt = gi + 1
            future = pool.submit(load_group, reader, blocks, groups[nxt], seed(nxt)) if nxt < len(groups) else None
            g_inp, g_score = torch.from_numpy(inp).to(device), torch.from_numpy(score).to(device)
            g_stm, g_res = torch.from_numpy(stm).to(device), torch.from_numpy(res).to(device)
            g_perm = torch.from_numpy(perm).to(device)
            for local in range(ck["batch"] - done, counts[gi]):
                if STOP.is_set():
                    break
                idx = g_perm[local * args.batch:(local + 1) * args.batch]
                terms = batch_tensors(g_inp[idx], g_score[idx], g_stm[idx], g_res[idx], device)
                t, c = loss_terms(net, *terms, args.result_weight)
                opt.zero_grad(set_to_none=True)
                (t / max(c.item(), 1.0) if device.type == "cpu" else t / c.clamp(min=1.0)).backward()
                opt.step()
                net.clip()
                loss_sum += t.detach()
                loss_n += c.detach()
                ck["batch"] += 1
                if ck["batch"] % 50 == 0 or ck["batch"] == batches:
                    sec = max(time.time() - start, 1e-3)
                    rate = (ck["batch"] - first) * args.batch / sec
                    left = (batches - ck["batch"]) * args.batch / max(rate, 1.0)
                    print(f"\rpass {epoch + 1}/{args.epochs}  {100.0 * ck['batch'] / batches:5.1f}%  "
                          f"loss {(loss_sum / loss_n.clamp(min=1)).item():.5f}  {rate:,.0f} positions/s  "
                          f"{left / 60:4.0f} min left in this pass   ", end="", flush=True)
            if STOP.is_set():
                break
            done += counts[gi]
            gi = nxt
        if STOP.is_set():
            break
        val = validation_loss(net, files, device, args.result_weight)
        best = val < ck["best_val"]
        if best:
            ck["best_val"], ck["best_epoch"] = val, epoch + 1
            export_network(net_path, net)
        print(f"\rpass {epoch + 1}/{args.epochs}  done    loss {(loss_sum / loss_n.clamp(min=1)).item():.5f}  "
              f"held-out loss {val:.5f}{'  (best so far, network saved)' if best else ''}" + " " * 30, flush=True)
        ck["epoch"], ck["batch"] = epoch + 1, 0
        save_checkpoint(ck_path, ck, net, opt)       # a finished pass is always saved
        status(False)
    pool.shutdown(wait=True)
    reader.close()
    if STOP.is_set():
        save_checkpoint(ck_path, ck, net, opt)
        status(False)
        print("\nStopped. Progress saved; run the same command to continue.")
        return
    status(True)
    print(f"Training finished. Best network (from pass {ck['best_epoch']}, held-out loss {ck['best_val']:.5f}): {net_path}")


if __name__ == "__main__":
    main()
