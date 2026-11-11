/**
 * analysis.js - turning engine output into things the UI can show.
 *
 * The engine reports scores from the side to move point of view; everything here is converted
 * to White's point of view first.
 */

import { Chess } from './chess.js';

/** Bar never goes fully to one side on a centipawn score; only a mate fills it. */
const MIN_SHARE = 0.04;
const MAX_PV_PLIES = 10;

/**
 * @param {{type: 'cp'|'mate', value: number}} score  from the side to move point of view
 * @param {'w'|'b'} turn  side to move in the analysed position
 * @returns {{type: 'cp'|'mate', value: number}} the same score seen from White
 */
export function scoreForWhite(score, turn) {
  return { type: score.type, value: turn === 'w' ? score.value : -score.value };
}

/** A finished position as a score: mate value 0 means "already mated". */
export function terminalScore(chess) {
  if (chess.isCheckmate()) return { type: 'mate', value: 0, winner: chess.turn() === 'w' ? 'b' : 'w' };
  return { type: 'cp', value: 0 };
}

/**
 * Share of the eval bar that belongs to White, 0..1.
 * Centipawns go through a logistic curve so that small advantages are visible and large ones
 * flatten out; a mate fills the bar.
 */
export function whiteShare(whiteScore) {
  if (whiteScore.type === 'mate') {
    if (whiteScore.value === 0) return whiteScore.winner === 'w' ? 1 : 0;
    return whiteScore.value > 0 ? 1 : 0;
  }
  const share = 1 / (1 + Math.exp(-0.004 * whiteScore.value));
  return Math.min(1 - MIN_SHARE, Math.max(MIN_SHARE, share));
}

/** "+0.4", "-1.3", "0.0", "M3" (White mates), "-M3" (Black mates). */
export function formatScore(whiteScore) {
  if (whiteScore.type === 'mate') {
    if (whiteScore.value === 0) return whiteScore.winner === 'w' ? '1-0' : '0-1';
    return `${whiteScore.value < 0 ? '-' : ''}M${Math.abs(whiteScore.value)}`;
  }
  const pawns = whiteScore.value / 100;
  if (Math.abs(pawns) < 0.05) return '0.0';
  const digits = Math.abs(pawns) >= 10 ? 0 : 1;
  return `${pawns > 0 ? '+' : '-'}${Math.abs(pawns).toFixed(digits)}`;
}

/**
 * Converts a principal variation from UCI to SAN with move numbers: "12. Nf3 Nc6 13. Bb5".
 * Stops at the first move that is not legal (an engine bug should not break the page).
 * @param {string} fen        position the line starts from
 * @param {string[]} uciMoves
 * @returns {string}
 */
export function pvToSan(fen, uciMoves, maxPlies = MAX_PV_PLIES) {
  let chess;
  try {
    chess = new Chess(fen);
  } catch {
    return '';
  }
  let number = Number(fen.split(' ')[5]) || 1;
  const parts = [];
  for (const uci of uciMoves.slice(0, maxPlies)) {
    const whiteToMove = chess.turn() === 'w';
    const move = chess.move(uci);
    if (!move) break;
    if (whiteToMove) parts.push(`${number}.`);
    else if (parts.length === 0) parts.push(`${number}...`);
    parts.push(move.san);
    if (!whiteToMove) number++;
  }
  return parts.join(' ');
}

/** 'e2e4' -> { from: 'e2', to: 'e4' }, or null for anything that is not a move. */
export function uciToArrow(uci) {
  if (!/^[a-h][1-8][a-h][1-8][qrbn]?$/.test(uci ?? '')) return null;
  return { from: uci.slice(0, 2), to: uci.slice(2, 4) };
}

/** Thinking time without a clock: about 150 ms on level 1 up to 2000 ms on level 10. */
export function moveTimeForLevel(level) {
  const clamped = Math.min(10, Math.max(1, level));
  return Math.round(150 * Math.pow(2000 / 150, (clamped - 1) / 9));
}
