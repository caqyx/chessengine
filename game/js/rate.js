/**
 * rate.js - the rating page: plays Tessera against Stockfish at a capped Elo, in the
 * browser, and estimates Tessera's rating from the results.
 *
 * Both engines run as Web Workers speaking UCI (one line per message). Stockfish's cap
 * (UCI_LimitStrength + UCI_Elo) follows the running estimate, so most games are played
 * against an opponent of about Tessera's strength, which is where a game tells the most.
 *
 * Testing hook: rate.html?opponent=tessera replaces Stockfish with a weakened Tessera.
 * That exercises the whole page but does not produce a meaningful rating.
 */
import { Chess } from './chess.js';
import { pieceSvg } from './pieces.js';
import { estimateRating, nextOpponentElo } from './rating-math.js';

const TESSERA_URL = new URL('../../engine/web/engine-worker.js', import.meta.url).href;
const STOCKFISH_URL = new URL('../../third_party/stockfish/stockfish-19-lite-single.js', import.meta.url).href;
const STORAGE_KEY = 'tessera.rating.v1';
const START_ELO = 2500;
const DEFAULT_RANGE = { min: 1320, max: 3190 };   // Stockfish's UCI_Elo range; replaced by what `uci` reports
const WIN_SCORE = 800;        // centipawns both engines must agree on ...
const WIN_STREAK = 3;         // ... for this many of the winner's moves in a row
const MAX_PLIES = 320;

// Each opening is played twice, once with each color.
const OPENINGS = [
  'e4 e5 Nf3 Nc6 Bb5 a6 Ba4 Nf6',
  'd4 d5 c4 e6 Nc3 Nf6 Bg5 Be7',
  'e4 c5 Nf3 d6 d4 cxd4 Nxd4 Nf6 Nc3 a6',
  'd4 Nf6 c4 g6 Nc3 Bg7 e4 d6 Nf3 O-O',
  'e4 e6 d4 d5 Nc3 Bb4 e5 c5',
  'c4 e5 Nc3 Nf6 Nf3 Nc6 g3 d5',
  'e4 c6 d4 d5 Nc3 dxe4 Nxe4 Bf5',
  'd4 d5 c4 c6 Nf3 Nf6 Nc3 dxc4',
  'e4 e5 Nf3 Nc6 Bc4 Bc5 c3 Nf6',
  'd4 Nf6 c4 e6 Nc3 Bb4 e3 O-O',
  'e4 c5 Nf3 Nc6 d4 cxd4 Nxd4 g6',
  'Nf3 d5 g3 Nf6 Bg2 c6 O-O Bg4',
  'e4 d5 exd5 Qxd5 Nc3 Qa5 d4 Nf6',
  'd4 Nf6 c4 e6 Nf3 b6 g3 Bb7',
  'e4 e5 Nf3 Nf6 Nxe5 d6 Nf3 Nxe4',
  'd4 Nf6 c4 g6 Nc3 d5 cxd5 Nxd5 e4 Nxc3',
  'e4 c5 Nf3 e6 d4 cxd4 Nxd4 a6',
  'd4 d5 Nf3 Nf6 Bf4 e6 e3 c5',
  'e4 d6 d4 Nf6 Nc3 g6 f4 Bg7',
  'c4 c5 Nf3 Nf6 Nc3 Nc6 g3 g6',
  'e4 e5 Nc3 Nf6 f4 d5',
  'd4 f5 g3 Nf6 Bg2 e6 Nf3 Be7',
  'e4 Nf6 e5 Nd5 d4 d6 Nf3 Bg4',
  'd4 Nf6 c4 c5 d5 e6 Nc3 exd5 cxd5 d6',
  'e4 e6 d4 d5 Nd2 Nf6 e5 Nfd7',
];

class AbortError extends Error {}

// ---- a UCI engine in a Web Worker ---------------------------------------------------

class UciEngine {
  constructor(name, url, log) {
    this.name = name;
    this.url = url;
    this.log = log;
    this.worker = null;
    this.expectation = null;   // { test, resolve, reject, timer }
    this.onInfo = null;
    this.options = new Map();  // option name -> { min, max } for spin options
  }

  start() {
    this.worker = new Worker(this.url);
    this.worker.onmessage = (event) => {
      if (typeof event.data !== 'string') return;
      for (const line of event.data.split('\n')) this.receive(line.trim());
    };
    this.worker.onerror = (event) => {
      event.preventDefault?.();
      this.fail(new Error(`${this.name}: the engine file could not be loaded or crashed (${event.message || this.url})`));
    };
  }

  receive(line) {
    if (!line) return;
    if (line.startsWith('info')) {
      this.onInfo?.(line);
    } else {
      this.log(`${this.name} < ${line}`);
    }
    if (line.startsWith('error')) {
      this.fail(new Error(`${this.name}: ${line}`));
      return;
    }
    const expected = this.expectation;
    if (expected && expected.test(line)) {
      this.expectation = null;
      clearTimeout(expected.timer);
      expected.resolve(line);
    }
  }

  fail(error) {
    const expected = this.expectation;
    this.expectation = null;
    if (expected) {
      clearTimeout(expected.timer);
      expected.reject(error);
    } else {
      this.pendingError = error;
    }
  }

  send(command) {
    if (!this.worker) throw new AbortError('stopped');
    if (!command.startsWith('position')) this.log(`${this.name} > ${command}`);
    this.worker.postMessage(command);
  }

  /** Resolves with the first line that passes `test`; rejects after `timeoutMs`. */
  expect(test, timeoutMs, what) {
    if (this.pendingError) {
      const error = this.pendingError;
      this.pendingError = null;
      return Promise.reject(error);
    }
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.expectation = null;
        reject(new Error(`${this.name}: no "${what}" after ${Math.round(timeoutMs / 1000)} s`));
      }, timeoutMs);
      this.expectation = { test, resolve, reject, timer };
    });
  }

  async init() {
    this.start();
    const collect = (line) => {
      const match = /^option name (.+?) type spin .*?min (-?\d+) max (-?\d+)/.exec(line);
      if (match) this.options.set(match[1], { min: Number(match[2]), max: Number(match[3]) });
      return line === 'uciok';
    };
    this.send('uci');
    await this.expect(collect, 60000, 'uciok');   // generous: the first load downloads the wasm
    await this.ready();
  }

  async ready() {
    this.send('isready');
    await this.expect((line) => line === 'readyok', 30000, 'readyok');
  }

  async setOptions(pairs) {
    for (const [name, value] of pairs) this.send(`setoption name ${name} value ${value}`);
    await this.ready();
  }

  async newGame() {
    this.send('ucinewgame');
    await this.ready();
  }

  /** @returns {Promise<{move: string, score: number | null}>} score in centipawns for the mover */
  async go(positionCommand, movetime) {
    let score = null;
    this.onInfo = (line) => {
      const match = /score (cp|mate) (-?\d+)/.exec(line);
      if (!match) return;
      const value = Number(match[2]);
      score = match[1] === 'cp' ? value : (value > 0 ? 1 : -1) * (100000 - Math.abs(value));
    };
    this.send(positionCommand);
    this.send(`go movetime ${movetime}`);
    try {
      const line = await this.expect((l) => l.startsWith('bestmove'), movetime + 20000, 'bestmove');
      return { move: line.split(/\s+/)[1] || '', score };
    } finally {
      this.onInfo = null;
    }
  }

  terminate() {
    this.worker?.terminate();
    this.worker = null;
    this.fail(new AbortError('stopped'));
    this.pendingError = null;
  }
}

// ---- opponents ------------------------------------------------------------------------

function makeStockfish(log) {
  const engine = new UciEngine('Stockfish', STOCKFISH_URL, log);
  return {
    engine,
    label: (elo) => `Stockfish ${elo}`,
    meaningful: true,
    async init() {
      await engine.init();
      await engine.setOptions([['Hash', 16], ['Threads', 1], ['UCI_LimitStrength', 'true']]);
      return engine.options.get('UCI_Elo') || DEFAULT_RANGE;
    },
    setCap: (elo) => engine.setOptions([['UCI_LimitStrength', 'true'], ['UCI_Elo', elo]]),
  };
}

// Self-test stand-in: Tessera at a reduced level. The "Elo" is only a label here.
function makeWeakTessera(log) {
  const engine = new UciEngine('Tessera-weak', TESSERA_URL, log);
  const levelFor = (elo) => Math.min(9, Math.max(1, 1 + Math.round((elo - DEFAULT_RANGE.min) / 210)));
  return {
    engine,
    label: (elo) => `Tessera level ${levelFor(elo)} (self-test ${elo})`,
    meaningful: false,
    async init() {
      await engine.init();
      await engine.setOptions([['Hash', 16]]);
      return DEFAULT_RANGE;
    },
    setCap: (elo) => engine.setOptions([['Level', levelFor(elo)]]),
  };
}

// ---- one game ---------------------------------------------------------------------------

/**
 * @returns {Promise<{score: number, reason: string, plies: number}>} score for Tessera
 */
async function playGame({ tessera, opponent, opening, tesseraWhite, movetime, onMove }) {
  const chess = new Chess();
  for (const san of opening.split(' ')) {
    if (!chess.move(san)) throw new Error(`Bad opening line: ${opening}`);
  }
  await tessera.newGame();
  await opponent.newGame();
  onMove(chess, null);

  const result = (whiteScore, reason) => ({
    score: tesseraWhite ? whiteScore : 1 - whiteScore,
    reason,
    plies: chess.history().length,
  });
  const winStreak = { w: 0, b: 0 };
  let quietPlies = 0;

  for (;;) {
    if (chess.isCheckmate()) return result(chess.turn() === 'w' ? 0 : 1, 'checkmate');
    if (chess.isDraw()) return result(0.5, chess.gameOverReason() || 'draw');
    if (chess.history().length >= MAX_PLIES) return result(0.5, 'move limit');

    const side = chess.turn();
    const other = side === 'w' ? 'b' : 'w';
    const engine = (side === 'w') === tesseraWhite ? tessera : opponent;
    const reply = await engine.go(`position startpos moves ${chess.uciMoves().join(' ')}`, movetime);
    const move = chess.move(reply.move);
    if (!move) return result(side === 'w' ? 0 : 1, `illegal move ${reply.move} by ${engine.name}`);
    onMove(chess, move);

    // Stop early once both engines agree the game is decided, or it is a dead draw.
    if (reply.score !== null) {
      winStreak[side] = reply.score >= WIN_SCORE ? winStreak[side] + 1 : 0;
      if (reply.score <= -WIN_SCORE && winStreak[other] >= WIN_STREAK) {
        return result(other === 'w' ? 1 : 0, 'score');
      }
      quietPlies = Math.abs(reply.score) <= 10 && chess.history().length > 80 ? quietPlies + 1 : 0;
      if (quietPlies >= 16) return result(0.5, 'score (draw)');
    }
  }
}

// ---- page -------------------------------------------------------------------------------

const $ = (id) => document.getElementById(id);
const ui = {
  estimate: $('estimate'), estimateSub: $('estimate-sub'), status: $('status'), error: $('error'),
  board: $('board'), results: $('results'), log: $('log'), start: $('start'), reset: $('reset'),
  movetime: $('movetime'), games: $('games'),
};

const state = {
  games: loadGames(),    // finished games: { opp, tesseraWhite, score, reason, plies }
  running: false,
  stopRequested: false,
  engines: [],
  logLines: [],
  meaningful: true,
};

function loadGames() {
  try {
    const saved = JSON.parse(localStorage.getItem(STORAGE_KEY) || 'null');
    if (saved && Array.isArray(saved.games)) {
      if (saved.movetime) queueMicrotask(() => { ui.movetime.value = String(saved.movetime); });
      return saved.games.filter((g) => Number.isFinite(g.opp) && [0, 0.5, 1].includes(g.score));
    }
  } catch (error) {
    // storage blocked or corrupt: start fresh
  }
  return [];
}

function saveGames() {
  try {
    localStorage.setItem(STORAGE_KEY, JSON.stringify({ movetime: Number(ui.movetime.value), games: state.games }));
  } catch (error) {
    // storage blocked: results simply do not survive a reload
  }
}

function log(line) {
  state.logLines.push(line);
  if (state.logLines.length > 80) state.logLines.shift();
  ui.log.textContent = state.logLines.join('\n');
}

function renderBoard(chess, lastMove) {
  const rows = chess.board();
  let html = '';
  for (let r = 0; r < 8; r++) {
    for (let f = 0; f < 8; f++) {
      const square = 'abcdefgh'[f] + (8 - r);
      const piece = rows[r][f];
      const classes = ['sq'];
      if ((r + f) % 2 === 1) classes.push('dark');
      if (lastMove && (lastMove.from === square || lastMove.to === square)) classes.push('last');
      html += `<div class="${classes.join(' ')}">${piece ? pieceSvg(piece.color, piece.type) : ''}</div>`;
    }
  }
  ui.board.innerHTML = html;
}

function renderEstimate() {
  const estimate = estimateRating(state.games);
  if (!estimate) {
    ui.estimate.textContent = '–';
    ui.estimateSub.textContent = 'No games played yet';
    return;
  }
  const rating = Math.round(estimate.rating / 10) * 10;
  const margin = Math.round(estimate.margin / 10) * 10;
  ui.estimate.innerHTML = `${rating} <small>± ${margin}</small>`;
  const points = Number.isInteger(estimate.points) ? estimate.points : estimate.points.toFixed(1);
  const note = state.meaningful ? '' : ' Self-test mode: this number is not a rating.';
  ui.estimateSub.textContent =
    `${points} point${estimate.points === 1 ? '' : 's'} from ${estimate.games} game${estimate.games === 1 ? '' : 's'}. The range is a 95% interval.${note}`;
}

function renderResults() {
  const symbol = { 1: '1', 0.5: '½', 0: '0' };
  ui.results.innerHTML = state.games
    .map((g, i) =>
      `<tr><td>${i + 1}</td><td>${g.opp}</td><td>${g.tesseraWhite ? 'White' : 'Black'}</td>` +
      `<td>${symbol[g.score]}</td><td>${Math.ceil(g.plies / 2)}</td><td>${escapeHtml(g.reason)}</td></tr>`)
    .join('');
}

function escapeHtml(text) {
  return String(text).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' })[c]);
}

function renderControls() {
  ui.start.textContent = state.running ? 'Stop' : state.games.length ? 'Continue' : 'Start';
  ui.reset.disabled = state.running || !state.games.length;
  ui.movetime.disabled = state.running || state.games.length > 0;   // one time control per run
  ui.games.disabled = state.running;
}

function render() {
  renderEstimate();
  renderResults();
  renderControls();
}

function showError(message) {
  ui.error.textContent = message;
  ui.error.hidden = !message;
}

async function keepScreenOn() {
  try {
    return (await navigator.wakeLock?.request('screen')) || null;
  } catch (error) {
    return null;   // not supported or not allowed: the run still works while the page stays visible
  }
}

async function run() {
  state.running = true;
  state.stopRequested = false;
  showError('');
  renderControls();
  const wakeLock = await keepScreenOn();
  const movetime = Number(ui.movetime.value);
  const target = Math.max(Number(ui.games.value), state.games.length + 2);
  const selfTest = new URLSearchParams(location.search).get('opponent') === 'tessera';

  const tessera = new UciEngine('Tessera', TESSERA_URL, log);
  const opponent = selfTest ? makeWeakTessera(log) : makeStockfish(log);
  state.engines = [tessera, opponent.engine];
  state.meaningful = opponent.meaningful;

  try {
    ui.status.textContent = 'Loading engines…';
    await tessera.init();
    await tessera.setOptions([['Hash', 32], ['Level', 10], ['Opening Variety', 'false']]);
    const range = await opponent.init();

    while (state.games.length < target && !state.stopRequested) {
      const index = state.games.length;
      const tesseraWhite = index % 2 === 0;
      // The second game of a pair repeats the cap and the opening with colors swapped.
      const cap = index % 2 === 1
        ? state.games[index - 1].opp
        : nextOpponentElo(state.games, { start: START_ELO, min: range.min, max: range.max });
      const opening = OPENINGS[Math.floor(index / 2) % OPENINGS.length];
      await opponent.setCap(cap);

      const describe = (chess) =>
        `Game ${index + 1} of ${target} · Tessera (${tesseraWhite ? 'White' : 'Black'}) vs ${opponent.label(cap)}` +
        ` · move ${Math.ceil((chess.history().length + 1) / 2)}`;
      const outcome = await playGame({
        tessera,
        opponent: opponent.engine,
        opening,
        tesseraWhite,
        movetime,
        onMove: (chess, move) => {
          renderBoard(chess, move);
          ui.status.textContent = describe(chess);
        },
      });
      state.games.push({ opp: cap, tesseraWhite, ...outcome });
      saveGames();
      render();
    }
    ui.status.textContent = state.stopRequested
      ? `Stopped after ${state.games.length} games. Continue adds more.`
      : `Finished ${state.games.length} games.`;
  } catch (error) {
    if (error instanceof AbortError) {
      ui.status.textContent = `Stopped after ${state.games.length} finished games. Continue adds more.`;
    } else {
      ui.status.textContent = 'Stopped because of an error.';
      showError(`${error.message}. Open "Engine log" below for details.`);
      log(`error: ${error.message}`);
    }
  } finally {
    for (const engine of state.engines) engine.terminate();
    state.engines = [];
    try { await wakeLock?.release(); } catch (error) { /* already released */ }
    state.running = false;
    render();
  }
}

ui.start.addEventListener('click', () => {
  if (state.running) {
    state.stopRequested = true;
    for (const engine of state.engines) engine.terminate();   // aborts the game in progress
  } else {
    run();
  }
});

ui.reset.addEventListener('click', () => {
  if (state.running) return;
  state.games = [];
  saveGames();
  ui.status.textContent = 'Ready. Keep this page open and the screen on while it runs.';
  showError('');
  render();
});

renderBoard(new Chess(), null);
render();
