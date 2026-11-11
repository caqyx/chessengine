# Progress (saved 2026-11-11, work paused)

Done
- Engine (engine/): complete and verified. Native + wasm builds, perft and selftest pass, bench nodes identical in both builds.
- Rules module (game/js/chess.js): complete, 37 tests pass.
- Engine worker + protocol doc, match runner (tools/match.py), README.

In progress
- Game UI (index.html, game/js, game/css): written by a subagent, stopped mid-work. NOT yet verified in a browser; treat as unfinished.
- Self-play ablations: 2 of 10 done (docs/ablations.md).

Blocked
- Measuring Elo against Stockfish: building/running Stockfish in the work environment was denied. tools/match.py is ready to run locally.

Next
1. Resume UI subagent, finish and verify in headless Chrome (phone + desktop, light + dark).
2. Wire UI to engine/web/engine-worker.js and test a full game against the wasm engine.
3. Finish ablations, drop any term that loses Elo.
4. Final README pass, remove this file.
