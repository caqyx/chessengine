# Tessera

A chess game for the browser and the chess engine it plays against, both written from scratch.

- **Game** (`index.html`, `game/`): plain HTML/CSS/JS, no build step, no dependencies. Play against the engine or a second person, with clocks, move list, takebacks, an analysis mode (eval bar, best-move arrow, engine line) and FEN/PGN import and export.
- **Engine** (`engine/`): Tessera, a C++17 alpha-beta engine. One code base builds a native UCI binary (for GUIs and testing) and a 76 kB WebAssembly module that runs inside the page in a Web Worker.

## Play

Open the site (GitHub Pages: *Settings -> Pages -> Deploy from a branch -> `main` / root*), or locally:

```bash
python3 -m http.server 8000   # serve the repo root; a file:// URL cannot load the wasm worker
# then open http://localhost:8000
```

## Build the engine

```bash
cd engine
make              # native UCI binary -> engine/tessera   (g++ or clang++)
make wasm         # WebAssembly build -> engine/web/engine.wasm   (clang with wasm32 target + wasm-ld)
make test         # perft from the start position + internal consistency self-test
make tessera-tune # same engine with search thresholds as UCI options (SP_*), for match tests
./tessera bench   # fixed-depth search over 12 positions; node count must match between builds
```

The wasm build needs no emscripten and no libc: the engine core is freestanding C++ and imports only two functions from JavaScript (print a line, read the clock). `engine/web/engine.wasm` is committed so the site works without building anything.

Use the native binary in any UCI GUI (Cute Chess, Arena, En Croissant, ...). Options: `Hash` (MB), `Level` (1-10, 10 = full strength), `Move Overhead` (ms).

## How the engine works

**Board.** Bitboards: one 64-bit integer per piece type and per color. Sliding-piece attacks come from magic bitboards (a multiply-and-shift hash from "which squares on the lines are blocked" to a precomputed attack set). Moves are 16 bits. Making a move updates the Zobrist hash, material, piece-square score and game phase incrementally; unmaking just steps a state pointer back.

**Move generation.** Pseudo-legal generation with check evasions already restricted to "capture the checker or block", then a cheap legality test using precomputed pins. Verified with perft on six standard positions (start position depth 6 = 119,060,324 nodes at about 140 M nodes/s).

**Search.** Iterative deepening with aspiration windows around a principal variation search. On top of plain alpha-beta:

| Technique | What it does |
|---|---|
| Transposition table | 3-entry buckets, depth + age replacement; stores score, bound, static eval, best move |
| Quiescence search | resolves captures and check evasions at the leaves, with SEE and delta pruning |
| Move ordering | hash move, captures by victim value + capture history (SEE splits good/bad), killers, counter move, quiets by butterfly + continuation history |
| Null move pruning | with verification search at high depth to avoid zugzwang errors |
| Reverse futility, razoring, futility, late move pruning, SEE pruning | drop moves/nodes that cannot plausibly change the result at low depth |
| ProbCut | a capture that beats beta by a margin at reduced depth cuts the node |
| Late move reductions | later moves get a shallower search first; reduction adapts to history, PV/cut node, checks |
| Singular extensions | extends the hash move when everything else is clearly worse; multi-cut when it is not |
| Correction history | learns per pawn structure how far the static eval was off and corrects it |

**Evaluation.** Hand-crafted, tapered between middlegame and endgame: material and piece-square tables (PeSTO), pawn structure (isolated, doubled, connected, passed; cached in a pawn hash), mobility, outposts, rooks on open files, bishop pair, king safety (pawn shelter plus an attack-unit danger model with safe checks), passed-pawn advancement and king distance, threats, and endgame scaling for drawish material (opposite bishops, no pawns, KBN-K corner knowledge).

**Tuning.** The weights in `engine/src/eval_params.h` are not hand-picked: `tessera datagen` plays self-play games and writes out quiet positions with the search score and the game result, and `tools/tuner.cpp` fits the weights so that the evaluation predicts those results (Texel tuning: logistic regression by gradient descent, validated on games it never trained on). It covers piece-square tables, mobility, pawn structure, threats and the king-safety weights. Two rounds on about 50,000 games (stored in `data/`) each gained clearly at small search budgets and less at larger ones; the measurements are in `docs/ablations.md`.

**Neural network (experimental).** The engine can replace the hand-written evaluation with a small network (768 inputs, one hidden layer kept up to date move by move, one output; `engine/src/nnue.cpp`) loaded with `setoption name EvalFile value <file>`. It is trained only on Tessera's own games by `tools/trainer.cpp`. No network ships yet: trained on the 7 million scored positions in `data/` it is about level with the hand-written evaluation (-10 Elo [-42, +21] over 300 games), so it needs more data to pull ahead (`docs/training.md`).

**Levels.** Levels 1-9 cap the search depth and add deterministic per-position noise to the evaluation, so weaker levels make plausible human-like mistakes instead of random moves.

## Strength, honestly

Tessera is not stronger than Stockfish and will not be. Stockfish is the product of well over a decade of work by many contributors, a distributed testing fleet that plays millions of games per change, and neural-network evaluation trained on billions of positions. Tessera is a classical engine with hand-set evaluation weights. It plays far above human level, but the realistic comparison is against Stockfish with its strength limited (`UCI_LimitStrength`).

Measure it yourself:

```bash
pip install chess                       # python-chess, used as the match runner
python3 tools/match.py --e1 engine/tessera --e2 /path/to/stockfish \
    --opt2 "UCI_LimitStrength=true;UCI_Elo=2500" --tc 10+0.1 --games 200 --concurrency 2
```

The script plays every opening twice with colors swapped and prints the score, the Elo difference and a 95% interval.

What has been verified in this repository:

- perft matches the published node counts on six positions; `selftest` cross-checks `pseudo_legal()`, `gives_check()` and the incremental hash on 13.8 M random probes with 0 errors;
- native and wasm builds search exactly the same tree (`bench` node counts are identical);
- self-play ablations at a fixed node budget (`docs/ablations.md`): every evaluation term gains 50-80 Elo, the full evaluation gains about +168 over material and piece-square tables alone, singular extensions gain about +67, and two rounds of tuning the weights on self-play games each gained further (between about +20 and +110 depending on the search budget); three search features (ProbCut, check extension, correction history) are within noise;
- the game was driven in headless Chrome against the real wasm engine at phone and desktop sizes (moves by tap and drag, promotion, clocks, analysis, PGN/FEN, reload restore). Not tested: Safari, Firefox, a physical phone.

What has not been measured: strength against any other engine, including Stockfish. No Elo number is claimed here.

### Rate it in the browser

`rate.html` plays Tessera against Stockfish (the small single-threaded browser build, vendored in `third_party/stockfish/` under the GPLv3) with Stockfish's strength capped via `UCI_Elo`. The cap follows the running estimate, both engines get the same time per move, and the page shows an Elo estimate with a 95% interval. About 20 games give roughly +/-150; it works on a phone. The number is a rough placement: the cap was calibrated by Stockfish's authors at much longer time controls.

## Layout

```
index.html                 the game
rate.html                  in-browser rating match against capped Stockfish
third_party/stockfish/     Stockfish.js lite build (GPLv3, not part of Tessera)
game/js/chess.js           rules: legal moves, FEN, SAN, PGN, draw rules (perft-tested)
game/js/*.js, game/css/    UI
game/test/rules.test.mjs   node game/test/rules.test.mjs
engine/src/                engine sources
engine/web/                engine.wasm + engine-worker.js used by the game
docs/engine-protocol.md    contract between game and engine worker
tools/match.py             engine-vs-engine match runner
tools/tuner.cpp            fits the evaluation weights to self-play games
tools/gen_data.py          generates self-play data on your own machine, stop and resume freely (docs/training.md)
tools/train_net.py         trains the neural-network evaluation on that data (wraps tools/trainer.cpp)
tools/fit_wdl.py           fits the win/draw/loss model shown in the game
data/                      4 million self-play positions with scores and results
```

## Credits

Piece-square tables are the PeSTO tables by Ronald Friederich. The search and evaluation ideas are the shared, well-documented toolkit of computer chess (chessprogramming.org); the king-danger model follows the classical Stockfish design. All code in this repository was written for this project.
