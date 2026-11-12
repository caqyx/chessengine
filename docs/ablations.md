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

| Match against the hand-set weights | Weights | Games | W-D-L | Elo for tuned |
|---|---|---|---|---|
| 20,000 nodes per move | first pass (400 epochs) | 300 | 132-129-39 | +111 [+82, +142] |
| 100,000 nodes per move | second pass (1500 epochs) | 200 | 84-78-38 | +81 [+44, +120] |
| 60 ms per move | second pass | 120 | 39-54-27 | +35 [-11, +82] |
| 50,000 nodes per move | shipped | 200 | 66-86-48 | +31 [-5, +68] |

The four matches disagree more than one would like. Pooled, the tuned weights
scored 60.3% over 820 games, about +70 Elo; the honest reading is "clearly
better, somewhere between +30 and +110, probably smaller at longer thinking
times". The shipped weights are the second pass with rarely-used table entries
(for example a queen attacking 20 or more safe squares) replaced by their
nearest well-sampled neighbour; head to head they are level with the second
pass (-10 [-41, +20] over 300 games).

Three things learned on the way:

- Validation has to be on whole games the tuner never saw. With positions of the
  same game on both sides of the split, the error looked great while the weights
  were memorising game results.
- Tuning all 384 piece-square entries separately lost 49 Elo [-79, -19] against
  tuning one value per piece type: 13,000 games are too few for that many
  free parameters.
- Table entries seen in fewer than 1% of positions were fitted to noise and are
  now filled from neighbours. Many of the remaining values are still jumpy; more
  games would smooth them.

The ablation tables above were measured with the hand-set weights.

## Second tuning round: 50,000 games, king safety included

Data: 4,056,141 positions from about 50,000 self-play games (`data/`). The tuner
now also fits the king-safety weights (shelter and storm tables, and the weights
of the danger sum, through the squared penalty) and uses mini-batch Adam.

| Match (20,000 nodes per move unless noted) | Games | W-D-L | Elo |
|---|---|---|---|
| Re-tuned, one value per piece type, vs round-one weights | 400 | 168-136-96 | +63 [+36, +92] |
| Re-tuned, all piece-square entries free, vs the row above | 400 | 140-160-100 | +35 [+9, +62] |
| Shipped (free) vs round-one weights, 100,000 nodes | 200 | 64-84-52 | +21 [-16, +58] |

With 50,000 games the free piece-square tables now generalise (validation error
0.08625 against 0.08652 for the tied variant), the opposite of round one with
13,000 games. As in round one, the gain is large at a small search budget and
much smaller, not statistically secured, at a deeper one.

The tuned evaluation is about 20% larger in scale than the hand-set one. Scaling
it back down for the search (`SP_EvalScale=82`) changed nothing: -7 [-36, +22]
over 300 games at 50,000 nodes.

## Search thresholds (coarse tests)

Build `make tessera-tune` to get the thresholds as `SP_*` UCI options. Each row
is one changed value against the defaults, 400 games at 20,000 nodes.

| Change | W-D-L | Elo | Decision |
|---|---|---|---|
| Reverse futility margin 70 -> 90 | 88-144-168 | -70 [-98, -43] | rejected |
| Reverse futility margin 70 -> 55 | 112-148-140 | -24 [-52, +3] | inconclusive |
| Reverse futility margin 70 -> 42 | 140-156-104 | +31 [+5, +58] | adopted after the check below |
| Same, 200 games at 100,000 nodes | 50-114-36 | +24 [-7, +56] | |
| Futility multiplier 80 -> 110 | 84-168-148 | -56 [-82, -30] | rejected |
| LMR divisor 2.36 -> 2.05 (reduce more) | 88-212-100 | -10 [-34, +13] | no change |
| LMR divisor 2.36 -> 2.75 (reduce less) | 124-172-104 | +17 [-8, +43] | no change |
| Null move base reduction 4 -> 3 | 120-176-104 | +14 [-12, +40] | no change |

Honest reading: loosening the pruning margins clearly hurts; tightening the
reverse futility margin to 42 was positive in two independent matches and is
now the default, although the 55 result does not fit a smooth curve, so part of
this is noise. Each test can only see effects of about 25 Elo or more. Tuning
all thresholds together (SPSA) needs tens of thousands of games and was not done.
Untested but suggested by the table: a smaller futility multiplier.

## Third tuning round: 100,000 games, half of them mixed strength

50,000 more games in which each side got its own random budget (1,000 to
10,000 nodes per move), added to the earlier 50,000: 8,094,082 positions.

| Match (20,000 nodes per move) | Games | W-D-L | Elo |
|---|---|---|---|
| Weights re-fitted on all 100,000 games vs the shipped ones | 400 | 120-148-132 | -10 [-38, +17] |

No gain, so these weights were not adopted. Doubling the data no longer helps:
about a thousand hand-designed weights have taken what they can from this kind
of data. More strength from the evaluation now needs a model with more
capacity (a neural network), which is what the saved games are for.

## Win / draw / loss model

`tools/fit_wdl.py` fits the chances the game shows to the results of the
7,070,000 positions that have a search score. Check on held-out games:

| Score for White (cp) | Predicted W / D / L | Actual W / D / L |
|---|---|---|
| below -400 | 1 / 6 / 94 % | 1 / 9 / 90 % |
| -400 to -200 | 5 / 29 / 66 % | 5 / 23 / 72 % |
| -200 to -100 | 12 / 42 / 46 % | 12 / 39 / 49 % |
| -100 to -30 | 21 / 44 / 36 % | 20 / 45 / 35 % |
| -30 to 30 | 26 / 48 / 26 % | 23 / 54 / 23 % |
| 30 to 100 | 36 / 43 / 21 % | 36 / 45 / 20 % |
| 100 to 200 | 46 / 42 / 13 % | 48 / 39 / 13 % |
| 200 to 400 | 65 / 30 / 5 % | 71 / 23 / 6 % |
| above 400 | 93 / 6 / 1 % | 90 / 10 / 1 % |

Within a few points everywhere, with too few draws predicted near equality and
too many when one side is clearly ahead. The chances describe Tessera playing
itself at fast speed, not a human game.

## Neural-network evaluation

Network: 768 inputs, 128 hidden units per side, one output. Target: 80% the
engine's search score, 20% the game result. Opponent in every row: the same
engine with the hand-written evaluation.

| Training data | Match | Games | W-D-L | Elo for the network |
|---|---|---|---|---|
| 7.07 million positions | 20,000 nodes per move | 300 | 93-105-102 | -10 [-42, +21] |
| 7.07 million, twice the passes | 20,000 nodes per move | 300 | 99-105-96 | +3 [-28, +35] |
| 7.07 million | 100,000 nodes per move | 200 | 56-88-56 | 0 [-36, +36] |
| 39.7 million (shipped) | 100 ms per move, on the owner's PC | 200 | 70-97-33 | +65 [+31, +100] |
| 39.7 million (shipped) | 20,000 nodes per move | 300 | 171-72-57 | +139 [+104, +177] |

| ~60 million, 256 hidden (shipped) vs the 39.7M 128-hidden network | 100 ms per move, owner's PC, slow output path | 200 | 37-112-51 | -24 [-57, +7] |
| same | 2,000 nodes per move, owner's PC | 200 | 82-72-46 | +63 [+25, +103] |
| same | 100 ms per move, after the output-path fix | 200 | 54-118-28 | +45 [+15, +77] |

The first 256 test lost at equal time only because a weight of 150 (limit 128
for the 16-bit vector path) sent it to the scalar fallback, 1.8x slower than
the 128 network. With a 32-bit vector path it is 5-10% slower, and wins.

The gain is smaller at equal time than at equal nodes because the network is
slower to evaluate: about 1.67 million nodes per second against 2.17 million.
With other targets the 7-million network was much weaker (half result, half
score: -147; score only: -56), see `docs/training.md`.

## What these numbers do not say

They compare Tessera with itself. They say nothing about absolute strength;
that needs matches against other engines (`tools/match.py`, see the README).
