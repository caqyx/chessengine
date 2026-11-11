# Stockfish.js (third party)

These files are **not part of Tessera**. They are an unmodified copy of the
"lite, single-threaded" build from the npm package `stockfish` version 19.0.1
(Stockfish.js by Nathan Rugg / Chess.com, based on Stockfish by T. Romstad,
M. Costalba, J. Kiiski, G. Linscott and contributors).

- `stockfish-19-lite-single.js`, `stockfish-19-lite-single.wasm`: the engine
- `Copying.txt`: its license, the GNU General Public License version 3

Source code: https://github.com/nmrugg/stockfish.js (tag v19.0.1) and
https://github.com/official-stockfish/Stockfish

They are used only by `rate.html`, which runs Stockfish in its own Web Worker
as a sparring partner and talks to it over the text-based UCI protocol. No
Stockfish code is linked into or copied into the Tessera engine or the game.
