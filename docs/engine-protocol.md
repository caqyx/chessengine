# Engine <-> game contract

The game talks to the engine through a **classic Web Worker** that speaks a
subset of UCI. One message = one UCI line (a plain string), in both directions.

```
game/js/engine-client.js  --postMessage(string)-->  engine/web/engine-worker.js  -->  engine.wasm
                          <--postMessage(string)--
```

## Worker

- URL: `engine/web/engine-worker.js` (relative to the site root, where `index.html` lives).
- Classic worker (`new Worker(url)`), not a module worker.
- It fetches `engine.wasm` from its own directory. Commands sent before the
  wasm is ready are queued, so the client may start sending immediately.
- If the wasm fails to load the worker posts a single line: `error <message>`.

## Commands the worker accepts

| Command | Reply |
|---|---|
| `uci` | `id name ...`, `id author ...`, `option ...` lines, then `uciok` |
| `isready` | `readyok` |
| `ucinewgame` | nothing (clears hash and history) |
| `setoption name Hash value <MB>` | nothing. 1..256, web default 32 |
| `setoption name Level value <1..10>` | nothing. 10 = full strength (default). 1 = beginner |
| `position startpos [moves e2e4 e7e5 ...]` | nothing |
| `position fen <fen> [moves ...]` | nothing |
| `go movetime <ms>` | `info ...` lines, then `bestmove ...` |
| `go depth <n>` | same |
| `go nodes <n>` | same |
| `go wtime <ms> btime <ms> [winc <ms>] [binc <ms>] [movestogo <n>]` | same |
| `go infinite` | same, but never finishes on its own in the web build (see Abort) |

Moves are long algebraic: `e2e4`, `e7e8q`, castling as king move `e1g1`.

## Output while searching

```
info depth 12 seldepth 19 score cp 34 nodes 412345 nps 980000 hashfull 120 time 420 pv e2e4 e7e5 g1f3
info depth 18 seldepth 30 score mate 4 nodes ... pv ...
bestmove e2e4 ponder e7e5
```

- `score cp X` is centipawns from the **side to move** point of view.
- `score mate N` is mate in N moves (negative: side to move gets mated).
- `bestmove 0000` when the position has no legal move.
- `ponder` part is optional.

## Abort (important)

The search runs synchronously inside the worker. A `stop` message sent during a
search is only read **after** that search ends. To cancel a running search:

1. `worker.terminate()`
2. create a new `Worker`
3. re-send options (`Hash`, `Level`) and the `position`

Startup is a few tens of milliseconds, so this is cheap. For analysis use
`go movetime <ms>` (finishes by itself) and terminate+respawn when the position
changes before it finishes.

## Levels

`Level` 1..9 weaken the engine (shallower search plus evaluation noise). The
level only changes strength; the time the engine spends is still controlled by
the `go` command, so the game can use short `movetime` values on low levels.
