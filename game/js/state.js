/**
 * state.js - the one game-state object and the functions that read or change it.
 *
 * Nothing in here touches the DOM, the engine, timers or storage. The controller calls the
 * update functions and then asks the views to render.
 *
 * Shape of a game (see createGame):
 *   chess        rules object holding the live position and its history
 *   moves        chess.history(), kept in step by the update functions (index = ply - 1)
 *   mode         'engine' | 'local'
 *   humanColor   'w' | 'b' (meaningful in engine mode)
 *   level        engine level 1..10
 *   timeControl  id from TIME_CONTROLS
 *   clocks       { w, b } remaining ms, or null without a clock
 *   clockAtPly   clocks after each ply (index = ply), used by takeBack; null without a clock
 *   ended        { result, reason, winner } for results the board cannot show
 *                (resignation, timeout, a result recorded in a loaded PGN), else null
 *   viewPly      ply shown on the board while reviewing, or null when showing the live position
 *   analysisOn   the Analysis toggle
 *   tags         PGN tags of a loaded game ({} otherwise)
 */

import { Chess, START_FEN } from './chess.js';

export { START_FEN };

export const TIME_CONTROLS = [
  { id: 'none', label: 'No clock', baseMs: 0, incMs: 0 },
  { id: '1+0', label: '1 min', baseMs: 60_000, incMs: 0 },
  { id: '3+2', label: '3 + 2', baseMs: 180_000, incMs: 2_000 },
  { id: '5+0', label: '5 min', baseMs: 300_000, incMs: 0 },
  { id: '10+0', label: '10 min', baseMs: 600_000, incMs: 0 },
  { id: '15+10', label: '15 + 10', baseMs: 900_000, incMs: 10_000 },
];

export const DEFAULT_OPTIONS = { mode: 'engine', humanColor: 'w', level: 4, timeControl: 'none' };

const PIECE_VALUES = { p: 1, n: 3, b: 3, r: 5, q: 9, k: 0 };
const START_COUNTS = { p: 8, n: 2, b: 2, r: 2, q: 1 };
const CAPTURE_ORDER = ['q', 'r', 'b', 'n', 'p'];

export function timeControlById(id) {
  return TIME_CONTROLS.find((control) => control.id === id) ?? TIME_CONTROLS[0];
}

function otherColor(color) {
  return color === 'w' ? 'b' : 'w';
}

function clampLevel(level) {
  return Math.min(10, Math.max(1, Math.round(Number(level) || DEFAULT_OPTIONS.level)));
}

// ---------------------------------------------------------------------------
// Creating games
// ---------------------------------------------------------------------------

/** Wraps an existing rules object (fresh, or filled by loadPgn) into a game state. */
function gameFromChess(chess, options) {
  const control = timeControlById(options.timeControl);
  const clocks = control.baseMs ? { w: control.baseMs, b: control.baseMs } : null;
  return {
    chess,
    moves: chess.history(),
    mode: options.mode === 'local' ? 'local' : 'engine',
    humanColor: options.humanColor === 'b' ? 'b' : 'w',
    level: clampLevel(options.level),
    timeControl: control.id,
    clocks,
    clockAtPly: clocks ? [{ ...clocks }] : null,
    ended: null,
    viewPly: null,
    analysisOn: false,
    tags: {},
  };
}

/**
 * A new game from the start position or from `startFen`.
 * @throws {Error} when startFen is not a valid FEN
 */
export function createGame({ startFen = START_FEN, ...options } = {}) {
  return gameFromChess(new Chess(startFen), { ...DEFAULT_OPTIONS, ...options });
}

/**
 * A two-player game holding the moves of a PGN text, or null when the text is not valid.
 * A result the board cannot show (resignation, agreed draw) is kept as a recorded result.
 */
export function createGameFromPgn(text) {
  if (typeof text !== 'string' || !text.trim()) return null;
  const chess = new Chess();
  if (!chess.loadPgn(text)) return null;
  const game = gameFromChess(chess, { ...DEFAULT_OPTIONS, mode: 'local' });
  game.tags = chess.headers();
  const recorded = game.tags.Result;
  if (!chess.isGameOver() && ['1-0', '0-1', '1/2-1/2'].includes(recorded)) {
    game.ended = endedWith(recorded, 'recorded');
  }
  return game;
}

function endedWith(result, reason) {
  const winner = result === '1-0' ? 'w' : result === '0-1' ? 'b' : null;
  return { result, reason, winner };
}

// ---------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------

export function livePly(game) {
  return game.moves.length;
}

export function viewedPly(game) {
  return game.viewPly ?? livePly(game);
}

export function isLive(game) {
  return game.viewPly === null;
}

export function fenAtPly(game, ply) {
  return ply <= 0 ? game.chess.startFen() : game.moves[ply - 1].after;
}

/** A rules object for the position on the board (the live one, or a copy while reviewing). */
export function viewedChess(game) {
  return isLive(game) ? game.chess : new Chess(fenAtPly(game, game.viewPly));
}

/**
 * How the game ended, or null while it is running.
 * @returns {{result: string, reason: string, winner: 'w'|'b'|null}|null}
 *   reason: checkmate | stalemate | threefold | fifty | insufficient | timeout | resignation | recorded
 */
export function outcome(game) {
  if (game.ended) return game.ended;
  const reason = game.chess.gameOverReason();
  return reason ? endedWith(game.chess.result(), reason) : null;
}

export function engineColor(game) {
  return game.mode === 'engine' ? otherColor(game.humanColor) : null;
}

export function isEngineTurn(game) {
  return game.mode === 'engine' && !outcome(game) && game.chess.turn() !== game.humanColor;
}

/** True when a person may move a piece right now. */
export function canMoveNow(game) {
  if (!isLive(game) || outcome(game)) return false;
  return game.mode === 'local' || game.chess.turn() === game.humanColor;
}

/** No engine help while a game against the engine is still running. */
export function analysisAllowed(game) {
  return game.mode === 'local' || outcome(game) !== null;
}

export function analysisActive(game) {
  return game.analysisOn && analysisAllowed(game);
}

/** How many plies Undo takes back: against the engine, back to the person's previous turn. */
export function takeBackPlies(game) {
  const played = livePly(game);
  if (game.mode === 'local') return Math.min(1, played);
  const humanToMove = game.chess.turn() === game.humanColor;
  const wanted = humanToMove ? 2 : 1;
  return played >= wanted ? wanted : 0;
}

/** The colour that resigns when Resign is pressed, or null when resigning makes no sense. */
export function resigningColor(game) {
  if (outcome(game)) return null;
  return game.mode === 'engine' ? game.humanColor : game.chess.turn();
}

/** True when `color` has nothing but a king (cannot win on time). */
function hasBareKing(chess, color) {
  return chess
    .board()
    .flat()
    .every((piece) => !piece || piece.color !== color || piece.type === 'k');
}

/**
 * Pieces each side has taken and the material balance, read off a board array.
 * @returns {{w: string[], b: string[], lead: number}} `w` lists the black piece types White
 *   captured; lead > 0 means White is ahead by that many points
 */
export function materialSummary(board) {
  const counts = { w: { p: 0, n: 0, b: 0, r: 0, q: 0 }, b: { p: 0, n: 0, b: 0, r: 0, q: 0 } };
  let lead = 0;
  for (const piece of board.flat()) {
    if (!piece || piece.type === 'k') continue;
    counts[piece.color][piece.type]++;
    lead += piece.color === 'w' ? PIECE_VALUES[piece.type] : -PIECE_VALUES[piece.type];
  }
  const missing = (color) =>
    CAPTURE_ORDER.flatMap((type) => Array(Math.max(0, START_COUNTS[type] - counts[color][type])).fill(type));
  return { w: missing('b'), b: missing('w'), lead };
}

/** Move number and side of the first move, from the start FEN. */
export function firstMoveInfo(game) {
  const fields = game.chess.startFen().split(' ');
  return { number: Number(fields[5]) || 1, blackStarts: fields[1] === 'b' };
}

// ---------------------------------------------------------------------------
// Updating
// ---------------------------------------------------------------------------

/**
 * Plays a move in the live position. A reviewer keeps looking at the position they chose
 * (viewPly is untouched), so an engine reply does not pull them back to the live board.
 * @param {string|{from: string, to: string, promotion?: string}} input
 * @returns {import('./chess.js').Move|null} null when the move is not legal (nothing changes)
 */
export function playMove(game, input) {
  if (outcome(game)) return null;
  const move = game.chess.move(input);
  if (!move) return null;
  game.moves.push(move);
  return move;
}

/** Takes back `plies` moves, forgets a resignation or timeout and returns to the live view. */
export function takeBack(game, plies) {
  let undone = 0;
  while (undone < plies && game.chess.undo()) undone++;
  if (undone === 0) return 0;
  game.moves.length -= undone;
  game.ended = null;
  game.viewPly = null;
  if (game.clockAtPly) {
    game.clockAtPly.length = Math.min(game.clockAtPly.length, livePly(game) + 1);
    game.clocks = { ...game.clockAtPly[game.clockAtPly.length - 1] };
  }
  return undone;
}

/** Stores the clock values reached after the latest ply. */
export function recordClocks(game, clocks) {
  if (!game.clockAtPly) return;
  game.clocks = { ...clocks };
  game.clockAtPly[livePly(game)] = { ...clocks };
}

export function resign(game, color) {
  if (outcome(game)) return;
  game.ended = endedWith(color === 'w' ? '0-1' : '1-0', 'resignation');
}

/** `color` ran out of time: a loss, or a draw when the opponent has only a king left. */
export function flagFall(game, color) {
  if (outcome(game)) return;
  const opponent = otherColor(color);
  const result = hasBareKing(game.chess, opponent) ? '1/2-1/2' : color === 'w' ? '0-1' : '1-0';
  game.ended = endedWith(result, 'timeout');
  game.ended.flagged = color;
}

/** Shows the position after `ply` moves; the last ply means the live position. */
export function setViewPly(game, ply) {
  const clamped = Math.min(livePly(game), Math.max(0, Math.round(ply)));
  game.viewPly = clamped === livePly(game) ? null : clamped;
}

// ---------------------------------------------------------------------------
// PGN export
// ---------------------------------------------------------------------------

export function playerNames(game) {
  if (game.mode === 'local') return { w: 'White', b: 'Black' };
  const engine = `Engine level ${game.level}`;
  return game.humanColor === 'w' ? { w: 'Player', b: engine } : { w: engine, b: 'Player' };
}

function pgnDate(date) {
  const pad = (value) => String(value).padStart(2, '0');
  return `${date.getFullYear()}.${pad(date.getMonth() + 1)}.${pad(date.getDate())}`;
}

/** PGN of the whole game, with the result as the UI knows it (resignation, timeout included). */
export function exportPgn(game, now = new Date()) {
  const names = playerNames(game);
  const headers = {
    Event: 'Casual game',
    Site: '?',
    Date: pgnDate(now),
    Round: '-',
    White: names.w,
    Black: names.b,
    ...game.tags,
    Result: outcome(game)?.result ?? '*',
  };
  const control = timeControlById(game.timeControl);
  if (control.baseMs) headers.TimeControl = `${control.baseMs / 1000}+${control.incMs / 1000}`;
  if (game.ended?.reason === 'timeout') headers.Termination = 'time forfeit';
  return game.chess.pgn({ headers });
}

// ---------------------------------------------------------------------------
// Saving and restoring
// ---------------------------------------------------------------------------

/** Plain data for localStorage. */
export function serializeGame(game) {
  return {
    version: 1,
    startFen: game.chess.startFen(),
    moves: game.chess.uciMoves(),
    mode: game.mode,
    humanColor: game.humanColor,
    level: game.level,
    timeControl: game.timeControl,
    clocks: game.clocks,
    clockAtPly: game.clockAtPly,
    ended: game.ended,
    viewPly: game.viewPly,
    analysisOn: game.analysisOn,
    tags: game.tags,
  };
}

function isClockPair(value) {
  return Boolean(value) && Number.isFinite(value.w) && Number.isFinite(value.b);
}

/** Rebuilds a game from serializeGame() output; null when the data is unusable. */
export function deserializeGame(data) {
  try {
    if (!data || data.version !== 1 || !Array.isArray(data.moves)) return null;
    const game = createGame({ ...data, startFen: data.startFen });
    for (const uci of data.moves) {
      if (!game.chess.move(uci)) return null;
    }
    game.moves = game.chess.history();
    restoreClocks(game, data);
    restoreEnding(game, data.ended);
    if (data.tags && typeof data.tags === 'object') game.tags = { ...data.tags };
    game.analysisOn = data.analysisOn === true;
    if (Number.isInteger(data.viewPly)) setViewPly(game, data.viewPly);
    return game;
  } catch {
    return null;
  }
}

function restoreClocks(game, data) {
  if (!game.clocks) return;
  const plies = livePly(game);
  const saved = Array.isArray(data.clockAtPly) ? data.clockAtPly.filter(isClockPair) : [];
  const fallback = isClockPair(data.clocks) ? data.clocks : game.clocks;
  game.clockAtPly = Array.from({ length: plies + 1 }, (_, ply) => ({ ...(saved[ply] ?? fallback) }));
  game.clocks = { ...(isClockPair(data.clocks) ? data.clocks : game.clockAtPly[plies]) };
}

function restoreEnding(game, ended) {
  const results = ['1-0', '0-1', '1/2-1/2'];
  const reasons = ['resignation', 'timeout', 'recorded'];
  if (!ended || !results.includes(ended.result) || !reasons.includes(ended.reason)) return;
  if (game.chess.isGameOver()) return;
  game.ended = endedWith(ended.result, ended.reason);
  if (ended.flagged === 'w' || ended.flagged === 'b') game.ended.flagged = ended.flagged;
}
