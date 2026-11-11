#!/usr/bin/env python3
"""Play a match between two UCI engines and report the Elo difference.

Needs python-chess:   pip install chess

Examples
  # Tessera vs Stockfish limited to 2500 Elo, 200 games at 10s + 0.1s
  python3 tools/match.py --e1 engine/tessera --e2 stockfish \
      --opt2 "UCI_LimitStrength=true;UCI_Elo=2500" --tc 10+0.1 --games 200

  # two Tessera builds against each other, fixed 100 ms per move
  python3 tools/match.py --e1 engine/tessera --e2 /tmp/tessera-old --movetime 100

Every opening is played twice with colors swapped, so --games should be even.
"""
import argparse
import concurrent.futures
import math
import threading
import time

import chess
import chess.engine

# Short, varied opening lines (SAN). Both engines get each one with both colors.
OPENINGS = [
    "e4 e5 Nf3 Nc6 Bb5 a6 Ba4 Nf6",
    "e4 e5 Nf3 Nc6 Bc4 Bc5 c3 Nf6",
    "e4 e5 Nf3 Nc6 d4 exd4 Nxd4 Nf6",
    "e4 e5 Nf3 Nf6 Nxe5 d6 Nf3 Nxe4",
    "e4 e5 Nc3 Nf6 f4 d5",
    "e4 e5 f4 exf4 Nf3 g5",
    "e4 c5 Nf3 d6 d4 cxd4 Nxd4 Nf6 Nc3 a6",
    "e4 c5 Nf3 Nc6 d4 cxd4 Nxd4 g6",
    "e4 c5 Nf3 e6 d4 cxd4 Nxd4 a6",
    "e4 c5 Nc3 Nc6 g3 g6 Bg2 Bg7",
    "e4 c5 c3 Nf6 e5 Nd5 d4 cxd4",
    "e4 e6 d4 d5 Nc3 Bb4 e5 c5",
    "e4 e6 d4 d5 Nd2 Nf6 e5 Nfd7",
    "e4 e6 d4 d5 exd5 exd5 Nf3 Nf6",
    "e4 c6 d4 d5 Nc3 dxe4 Nxe4 Bf5",
    "e4 c6 d4 d5 e5 Bf5 Nf3 e6",
    "e4 d6 d4 Nf6 Nc3 g6 f4 Bg7",
    "e4 d5 exd5 Qxd5 Nc3 Qa5 d4 Nf6",
    "e4 Nf6 e5 Nd5 d4 d6 Nf3 Bg4",
    "e4 g6 d4 Bg7 Nc3 d6 Be3 a6",
    "d4 d5 c4 e6 Nc3 Nf6 Bg5 Be7",
    "d4 d5 c4 c6 Nf3 Nf6 Nc3 dxc4",
    "d4 d5 c4 dxc4 Nf3 Nf6 e3 e6",
    "d4 d5 c4 c6 Nf3 Nf6 e3 Bf5",
    "d4 d5 Nf3 Nf6 Bf4 e6 e3 c5",
    "d4 Nf6 c4 g6 Nc3 Bg7 e4 d6 Nf3 O-O",
    "d4 Nf6 c4 g6 Nc3 d5 cxd5 Nxd5 e4 Nxc3",
    "d4 Nf6 c4 e6 Nc3 Bb4 e3 O-O",
    "d4 Nf6 c4 e6 Nf3 b6 g3 Bb7",
    "d4 Nf6 c4 e6 g3 d5 Bg2 Be7",
    "d4 Nf6 c4 c5 d5 e6 Nc3 exd5 cxd5 d6",
    "d4 Nf6 c4 c5 d5 b5 cxb5 a6",
    "d4 f5 g3 Nf6 Bg2 e6 Nf3 Be7",
    "d4 Nf6 Bg5 Ne4 Bf4 d5 e3 c5",
    "d4 d6 e4 Nf6 Nc3 e5 Nf3 Nbd7",
    "c4 e5 Nc3 Nf6 Nf3 Nc6 g3 d5",
    "c4 c5 Nf3 Nf6 Nc3 Nc6 g3 g6",
    "c4 Nf6 Nc3 e6 e4 d5 e5 d4",
    "c4 e6 Nf3 d5 g3 Nf6 Bg2 Be7",
    "Nf3 d5 g3 Nf6 Bg2 c6 O-O Bg4",
    "Nf3 Nf6 c4 g6 b3 Bg7 Bb2 O-O",
    "Nf3 d5 c4 d4 b4 f6",
    "b3 e5 Bb2 Nc6 e3 Nf6 Bb5 Bd6",
    "f4 d5 Nf3 Nf6 e3 g6 b3 Bg7",
    "e4 e5 Nf3 d6 d4 exd4 Nxd4 Nf6",
    "e4 e5 Bc4 Nf6 d3 c6 Nf3 d5",
    "e4 e5 Nf3 Nc6 Nc3 Nf6 d4 exd4",
    "e4 c5 Nf3 d6 Bb5+ Bd7 Bxd7+ Qxd7",
    "d4 d5 c4 e6 Nf3 c5 cxd5 exd5",
    "d4 e6 c4 f5 g3 Nf6 Bg2 d5",
]


def parse_options(text):
    opts = {}
    for part in (text or "").split(";"):
        if "=" in part:
            k, v = part.split("=", 1)
            v = v.strip()
            opts[k.strip()] = True if v.lower() == "true" else False if v.lower() == "false" else v
    return opts


def score_cp(info, turn):
    sc = info.get("score")
    if sc is None:
        return None
    return sc.pov(turn).score(mate_score=30000)


def play_game(args, opening, e1_white, results, lock, idx):
    paths = (args.e1, args.e2) if e1_white else (args.e2, args.e1)
    opts = (args.o1, args.o2) if e1_white else (args.o2, args.o1)
    engines = []
    outcome, reason = None, ""
    try:
        for p, o in zip(paths, opts):
            eng = chess.engine.SimpleEngine.popen_uci(p)
            if o:
                eng.configure(o)
            engines.append(eng)
        board = chess.Board()
        for san in opening.split():
            board.push_san(san)
        clocks = [args.base, args.base]          # index 0 = white
        win_streak = [0, 0]
        draw_streak = 0
        while True:
            if board.is_checkmate():
                outcome, reason = (0.0 if board.turn == chess.WHITE else 1.0), "mate"
                break
            if board.is_stalemate() or board.is_insufficient_material() or board.can_claim_draw():
                outcome, reason = 0.5, "draw rule"
                break
            side = 0 if board.turn == chess.WHITE else 1
            if args.movetime:
                limit = chess.engine.Limit(time=args.movetime / 1000)
            elif args.nodes:
                limit = chess.engine.Limit(nodes=args.nodes)
            else:
                limit = chess.engine.Limit(white_clock=clocks[0], black_clock=clocks[1],
                                           white_inc=args.inc, black_inc=args.inc)
            start = time.perf_counter()
            res = engines[side].play(board, limit, info=chess.engine.INFO_SCORE)
            used = time.perf_counter() - start
            if not args.movetime and not args.nodes:
                clocks[side] -= used
                if clocks[side] < -args.margin:
                    outcome, reason = (0.0 if side == 0 else 1.0), "time"
                    break
                clocks[side] = max(clocks[side], 0.0) + args.inc
            if res.move is None or res.move not in board.legal_moves:
                outcome, reason = (0.0 if side == 0 else 1.0), "illegal move"
                break
            cp = score_cp(res.info, board.turn)
            board.push(res.move)
            # Adjudication keeps matches short: clear wins and dead draws.
            if cp is not None:
                win_streak[side] = win_streak[side] + 1 if cp >= args.win_cp else 0
                lose = cp <= -args.win_cp
                if lose and win_streak[1 - side] >= args.win_moves:
                    outcome, reason = (0.0 if side == 0 else 1.0), "adjudicated"
                    break
                draw_streak = draw_streak + 1 if abs(cp) <= 8 and board.fullmove_number > 40 else 0
                if draw_streak >= 16:
                    outcome, reason = 0.5, "adjudicated draw"
                    break
            if board.fullmove_number > 300:
                outcome, reason = 0.5, "too long"
                break
    except Exception as exc:  # engine crash counts as a loss for nobody; report it
        reason = f"error: {exc!r}"
    finally:
        for eng in engines:
            try:
                eng.quit()
            except Exception:
                pass
    with lock:
        if outcome is None:
            results["errors"] += 1
            print(f"game {idx}: {reason}", flush=True)
            return
        e1_score = outcome if e1_white else 1.0 - outcome
        results["w" if e1_score == 1 else "d" if e1_score == 0.5 else "l"] += 1
        results["reasons"][reason] = results["reasons"].get(reason, 0) + 1
        n = results["w"] + results["d"] + results["l"]
        if n % args.report == 0:
            print(f"{n:4d} games  +{results['w']} ={results['d']} -{results['l']}  {summary(results)}", flush=True)


def summary(r):
    n = r["w"] + r["d"] + r["l"]
    if n == 0:
        return ""
    s = (r["w"] + 0.5 * r["d"]) / n
    var = (r["w"] * (1 - s) ** 2 + r["d"] * (0.5 - s) ** 2 + r["l"] * (0 - s) ** 2) / n
    se = math.sqrt(var / n)

    def elo(x):
        x = min(max(x, 1e-4), 1 - 1e-4)
        return -400 * math.log10(1 / x - 1)

    lo, hi = elo(s - 1.96 * se), elo(s + 1.96 * se)
    return f"score {100 * s:.1f}%  Elo {elo(s):+.0f} [{lo:+.0f}, {hi:+.0f}]"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--e1", required=True, help="engine 1 (results are from its point of view)")
    ap.add_argument("--e2", required=True)
    ap.add_argument("--opt1", default="", help='UCI options, e.g. "Hash=64;Level=8"')
    ap.add_argument("--opt2", default="")
    ap.add_argument("--tc", default="5+0.05", help="base+increment in seconds")
    ap.add_argument("--movetime", type=int, default=0, help="fixed ms per move (overrides --tc)")
    ap.add_argument("--nodes", type=int, default=0, help="fixed nodes per move (overrides --tc)")
    ap.add_argument("--games", type=int, default=100)
    ap.add_argument("--concurrency", type=int, default=1)
    ap.add_argument("--margin", type=float, default=0.25, help="seconds of clock overrun tolerated")
    ap.add_argument("--win-cp", type=int, default=900)
    ap.add_argument("--win-moves", type=int, default=4)
    ap.add_argument("--report", type=int, default=10)
    args = ap.parse_args()
    base, _, inc = args.tc.partition("+")
    args.base, args.inc = float(base), float(inc or 0)
    args.o1, args.o2 = parse_options(args.opt1), parse_options(args.opt2)

    results = {"w": 0, "d": 0, "l": 0, "errors": 0, "reasons": {}}
    lock = threading.Lock()
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.concurrency) as pool:
        for g in range(args.games):
            opening = OPENINGS[(g // 2) % len(OPENINGS)]
            pool.submit(play_game, args, opening, g % 2 == 0, results, lock, g + 1)
    print(f"\nFinal: +{results['w']} ={results['d']} -{results['l']}  {summary(results)}")
    print("Game endings:", results["reasons"], "errors:", results["errors"])


if __name__ == "__main__":
    main()
