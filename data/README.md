# Self-play data

About 100,000 Tessera self-play games, reduced to 8,094,082 quiet positions
(side to move not in check, best move not a capture). Kept for evaluation
tuning now and for training a neural-network evaluation later.

| File | Positions | Games (approx.) | Line format | Engine that played |
|---|---|---|---|---|
| `selfplay-01.txt.xz` | 1,022,547 | 13,000 | `fen;result` | hand-set weights |
| `selfplay-02.txt.xz` | 1,519,252 | 18,500 | `fen;score;result` | first tuned weights |
| `selfplay-03.txt.xz` | 1,514,342 | 18,500 | `fen;score;result` | first tuned weights |
| `selfplay-04.txt.xz` | 2,015,520 | 25,000 | `fen;score;result` | second tuned weights, mixed strength |
| `selfplay-05.txt.xz` | 2,022,421 | 25,000 | `fen;score;result` | second tuned weights, mixed strength |

- `result`: final result of the game the position came from, from White's point
  of view: `1`, `0.5` or `0`.
- `score`: search score of that position in centipawns, from White's point of view
  (5,000 nodes per move).
- Games start with 8 or 9 random legal moves, then both sides search 5,000 nodes
  per move. In the mixed-strength files each side instead gets its own random
  budget between 1,000 and 10,000 nodes for the whole game. Games are adjudicated when both the score stays beyond 1,500 cp for
  five moves, or stays within 5 cp for twelve moves after move 40.
- Positions of one game are consecutive lines. When splitting into training and
  validation sets, split by blocks of lines, never line by line, otherwise the
  same game ends up on both sides.

```bash
xz -dk data/selfplay-02.txt.xz                    # unpack, keep the archive
./engine/tessera datagen 1000 5000 7 > more.txt   # generate more: games, nodes per move, seed
```
