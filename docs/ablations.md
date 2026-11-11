# Self-play ablations

Each row is the full engine against the same binary with one feature switched
off (debug options `EvalMask` / `SearchMask`), at a fixed node budget per move so
the result does not depend on machine load. 50 openings, each played with both
colors. Elo is from the full engine's point of view, with a 95% interval.

Reproduce: `python3 tools/match.py --e1 engine/tessera --e2 engine/tessera --opt2 "EvalMask=0" --nodes 20000 --games 300 --concurrency 2`

| Feature removed | Option | Nodes/move | Games | W-D-L | Elo for full engine |
|---|---|---|---|---|---|
| All evaluation terms except material + piece-square tables | `EvalMask=0` | 20,000 | 300 | 177-81-42 | +168 [+134, +206] |
| King safety | `EvalMask=29` | 20,000 | 300 | 141-69-90 | +60 [+25, +95] |

Still to run: threats (`EvalMask=27`), passed pawns (`23`), mobility (`30`),
pawn structure (`15`), check extension (`SearchMask=254`), singular extension
(`253`), ProbCut (`251`), correction history (`247`).

Note: a fixed node budget ignores speed. The piece-square-only evaluation is
roughly twice as fast per node, which is worth an estimated 50-70 Elo, so the
real gain of the full evaluation at equal time is smaller than +168 but still
clearly positive.
