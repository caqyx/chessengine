#!/usr/bin/env python3
"""Fit the win / draw / loss model that turns a search score into chances.

Model (x = score in centipawns for one side, m = material phase from 0 = bare
kings to 1 = all pieces on the board):

    P(win)  = sigmoid((x - a) / b)
    P(loss) = sigmoid((-x - a) / b)
    P(draw) = 1 - P(win) - P(loss)
    a = a0 + a1 * m        "how many centipawns for a 50% win chance"
    b = b0 + b1 * m        "how gradual the transition is"

The four numbers are chosen to make the actual results of the self-play games
as likely as possible (maximum likelihood). Needs numpy.

    xz -dk data/selfplay-0[2-5].txt.xz
    python3 tools/fit_wdl.py data/selfplay-02.txt data/selfplay-03.txt ...

Input lines: fen;score;result  (score and result from White's point of view).
"""
import sys

import numpy as np

PHASE = {"n": 1, "b": 1, "r": 2, "q": 4}


def load(paths, stride=1):
    xs, ms, rs = [], [], []
    for path in paths:
        with open(path) as f:
            for i, line in enumerate(f):
                if i % stride:
                    continue
                parts = line.rstrip("\n").split(";")
                if len(parts) != 3:
                    continue          # old format without a score
                board = parts[0].split(" ", 1)[0].lower()
                phase = min(24, sum(PHASE.get(ch, 0) for ch in board))
                xs.append(int(parts[1]))
                ms.append(phase / 24.0)
                rs.append(float(parts[2]))
    return np.array(xs, float), np.array(ms, float), np.array(rs, float)


def sigmoid(z):
    return 1.0 / (1.0 + np.exp(-np.clip(z, -40, 40)))


def probabilities(params, x, m):
    a = params[0] + params[1] * m
    b = np.maximum(params[2] + params[3] * m, 10.0)
    win = sigmoid((x - a) / b)
    loss = sigmoid((-x - a) / b)
    return win, np.maximum(1.0 - win - loss, 1e-6), loss


def negative_log_likelihood(params, x, m, r):
    win, draw, loss = probabilities(params, x, m)
    p = np.where(r == 1.0, win, np.where(r == 0.0, loss, draw))
    return -np.mean(np.log(np.maximum(p, 1e-9)))


def fit(x, m, r):
    """Pattern search: try a step in each direction for each parameter, halve the steps when stuck."""
    params = np.array([150.0, 150.0, 80.0, 40.0])
    steps = np.array([40.0, 40.0, 20.0, 20.0])
    best = negative_log_likelihood(params, x, m, r)
    while steps.max() > 0.25:
        improved = False
        for i in range(4):
            for direction in (1.0, -1.0):
                trial = params.copy()
                trial[i] += direction * steps[i]
                value = negative_log_likelihood(trial, x, m, r)
                if value < best - 1e-9:
                    params, best, improved = trial, value, True
        if not improved:
            steps /= 2
    return params, best


def main():
    paths = sys.argv[1:]
    if not paths:
        sys.exit(__doc__)
    x, m, r = load(paths)
    # Whole games must stay on one side of the split: use the last tenth as a check.
    cut = len(x) - len(x) // 10
    params, train = fit(x[:cut], m[:cut], r[:cut])
    check = negative_log_likelihood(params, x[cut:], m[cut:], r[cut:])
    print(f"positions {len(x)}   log-loss: fitted part {train:.4f}, held-out games {check:.4f}")
    print(f"a0 {params[0]:.1f}  a1 {params[1]:.1f}  b0 {params[2]:.1f}  b1 {params[3]:.1f}")

    print("\nHow well it matches the held-out games (White's point of view):")
    print("score range      positions   predicted W/D/L      actual W/D/L")
    xv, mv, rv = x[cut:], m[cut:], r[cut:]
    win, draw, loss = probabilities(params, xv, mv)
    for lo, hi in [(-10000, -400), (-400, -200), (-200, -100), (-100, -30), (-30, 30), (30, 100), (100, 200), (200, 400), (400, 10000)]:
        sel = (xv >= lo) & (xv < hi)
        if sel.sum() < 100:
            continue
        actual = (np.mean(rv[sel] == 1.0), np.mean(rv[sel] == 0.5), np.mean(rv[sel] == 0.0))
        pred = (win[sel].mean(), draw[sel].mean(), loss[sel].mean())
        print(f"{lo:6d}..{hi:<6d} {sel.sum():9d}   " + " ".join(f"{100 * v:4.0f}%" for v in pred) + "    "
              + " ".join(f"{100 * v:4.0f}%" for v in actual))
    print("\nPaste into engine/src/search.cpp:")
    print(f"static const double WdlA0 = {params[0]:.1f}, WdlA1 = {params[1]:.1f}, WdlB0 = {params[2]:.1f}, WdlB1 = {params[3]:.1f};")


if __name__ == "__main__":
    main()
