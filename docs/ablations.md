# Self-play ablations

Each row is the full engine against the same binary with one feature switched
off (debug options `EvalMask` / `SearchMask`), at a fixed node budget per move so
the result does not depend on machine load. 50 openings, each played with both
colors. Elo is from the full engine's point of view, with a 95% interval.

Reproduce one row:

```bash
python3 tools/match.py --e1 engine/tessera --e2 engine/tessera \
    --opt2 "EvalMask=0" --nodes 20000 --games 300 --concurrency 2
```

## Evaluation terms (20,000 nodes per move, 300 games each)

| Feature removed | Option | W-D-L | Elo for full engine |
|---|---|---|---|
| Everything except material + piece-square tables | `EvalMask=0` | 177-81-42 | +168 [+134, +206] |
| Mobility, outposts, open files, bishop pair | `EvalMask=30` | 141-84-75 | +78 [+44, +112] |
| Passed pawns | `EvalMask=23` | 117-120-63 | +63 [+33, +94] |
| King safety | `EvalMask=29` | 141-69-90 | +60 [+25, +95] |
| Threats | `EvalMask=27` | 108-129-63 | +53 [+23, +83] |
| Pawn structure | `EvalMask=15` | 114-117-69 | +53 [+22, +84] |

Every evaluation term earns its place. A fixed node budget ignores speed: the
piece-square-only evaluation is roughly twice as fast per node, worth an
estimated 50-70 Elo, so the gain of the full evaluation at equal time is
smaller than +168 but still clearly positive.

## Search features (50,000 nodes per move, 200 games each)

| Feature removed | Option | W-D-L | Elo for full engine |
|---|---|---|---|
| Singular extensions | `SearchMask=253` | 72-94-34 | +67 [+32, +103] |
| ProbCut | `SearchMask=251` | 56-90-54 | +3 [-32, +39] |
| Check extension | `SearchMask=254` | 48-102-50 | -3 [-37, +30] |
| Correction history | `SearchMask=247` | 44-104-52 | -14 [-47, +19] |

Singular extensions are a clear gain. ProbCut, the check extension and
correction history are within noise at this sample size and node budget: none
is shown to help, none is shown to hurt. They stay enabled; settling them needs
a few thousand games each at a longer time control.

## Difficulty levels (40 ms per move, 30 games each)

| Match | W-D-L |
|---|---|
| Level 10 vs Level 7 | 28-2-0 |
| Level 7 vs Level 4 | 30-0-0 |
| Level 4 vs Level 1 | 24-4-2 |

## Evaluation tuning (Texel method)

The hand-set weights were fitted to 1,022,547 quiet positions from about 13,000
self-play games (5,000 nodes per move, random 8-ply openings), using
`tessera datagen` and `tools/tuner.cpp`. Tuned: one value per piece type on top
of the piece-square tables, mobility tables, pawn-structure, passed-pawn rank,
outpost, open-file, bishop-pair and threat weights. Not tuned: king safety,
passed-pawn race terms, endgame knowledge.

| Match (tuned vs hand-set weights) | Games | W-D-L | Elo for tuned |
|---|---|---|---|
| 20,000 nodes per move, first pass (400 epochs) | 300 | 132-129-39 | +111 [+82, +142] |
| 100,000 nodes per move, final weights | 200 | 84-78-38 | +81 [+44, +120] |
| 60 ms per move, final weights | 120 | 39-54-27 | +35 [-11, +82] |

Two things learned on the way:

- Validation has to be on whole games the tuner never saw. With positions of the
  same game on both sides of the split, the error looked great while the weights
  were memorising game results.
- Tuning all 384 piece-square entries separately lost 49 Elo [-79, -19] against
  tuning one value per piece type: 13,000 games are too few for that many
  free parameters.

The ablation tables above were measured with the hand-set weights.

## What these numbers do not say

They compare Tessera with itself. They say nothing about absolute strength;
that needs matches against other engines (`tools/match.py`, see the README).
