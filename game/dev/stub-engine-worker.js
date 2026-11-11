/*
 * stub-engine-worker.js - stand-in for engine/web/engine-worker.js.
 *
 * A classic Web Worker that speaks the same UCI subset (docs/engine-protocol.md) but plays a
 * random legal move after a short delay, with a few made-up `info` lines on the way (the score
 * is the material balance, and the `wdl` chances are derived from that score).
 * The game falls back to it when the real engine cannot be loaded.
 *
 * The rules come from game/js/chess.js, loaded with a dynamic import (allowed in classic
 * workers). Commands that arrive before the import finished are queued, like the real worker
 * queues commands until its wasm is ready.
 */

'use strict';

const MAX_THINK_MS = 450;
const INFO_STEPS = 4;
const PIECE_VALUES = { p: 100, n: 320, b: 330, r: 500, q: 900, k: 0 };

let rules = null;
const queued = [];
const position = { fen: null, moves: [] };
let searchTimers = [];
let finishSearch = null;

import('../js/chess.js')
  .then((module) => {
    rules = module;
    queued.splice(0).forEach(handleLine);
  })
  .catch((error) => postMessage(`error stub engine could not load the rules: ${error.message}`));

onmessage = (event) => {
  const line = String(event.data).trim();
  if (rules) handleLine(line);
  else queued.push(line);
};

function handleLine(line) {
  const [command, ...args] = line.split(/\s+/);
  if (command === 'uci') {
    postMessage('id name Random Mover (stub)');
    postMessage('id author chessengine dev tools');
    postMessage('option name Hash type spin default 32 min 1 max 256');
    postMessage('option name Level type spin default 10 min 1 max 10');
    postMessage('uciok');
  } else if (command === 'isready') {
    postMessage('readyok');
  } else if (command === 'position') {
    setPosition(args);
  } else if (command === 'go') {
    startSearch(args);
  } else if (command === 'stop') {
    if (finishSearch) finishSearch();
  }
  // ucinewgame, setoption and unknown commands need no answer
}

function setPosition(args) {
  const movesAt = args.indexOf('moves');
  const head = movesAt < 0 ? args : args.slice(0, movesAt);
  position.fen = head[0] === 'fen' ? head.slice(1).join(' ') : rules.START_FEN;
  position.moves = movesAt < 0 ? [] : args.slice(movesAt + 1);
}

/** The game described by the last `position` command, or null when it cannot be built. */
function buildGame() {
  try {
    const game = new rules.Chess(position.fen ?? rules.START_FEN);
    for (const move of position.moves) {
      if (!game.move(move)) return null;
    }
    return game;
  } catch {
    return null;
  }
}

function thinkTime(args) {
  const valueOf = (name) => {
    const at = args.indexOf(name);
    return at < 0 ? null : Number(args[at + 1]);
  };
  if (args.includes('infinite')) return Infinity;
  const movetime = valueOf('movetime');
  if (movetime !== null) return Math.min(movetime, MAX_THINK_MS);
  if (valueOf('wtime') !== null || valueOf('btime') !== null) return 300;
  return 100;
}

/** Material balance in centipawns from the side to move point of view. */
function materialScore(game) {
  let score = 0;
  for (const rank of game.board()) {
    for (const piece of rank) {
      if (piece) score += piece.color === game.turn() ? PIECE_VALUES[piece.type] : -PIECE_VALUES[piece.type];
    }
  }
  return score;
}

/** Made-up win / draw / loss chances (per-mille, side to move) for a centipawn score. */
function wdlForScore(score) {
  const chance = (value) => Math.round(1000 / (1 + Math.exp((60 - value) / 120)));
  const win = chance(score);
  const loss = Math.min(chance(-score), 1000 - win);
  return `${win} ${1000 - win - loss} ${loss}`;
}

function pickRandom(list) {
  return list[Math.floor(Math.random() * list.length)];
}

function startSearch(args) {
  clearSearch();
  const game = buildGame();
  const legal = game ? game.moves() : [];
  if (legal.length === 0) {
    postMessage('bestmove 0000');
    return;
  }

  const best = pickRandom(legal);
  const baseScore = materialScore(game);
  game.move(best.uci);
  const replies = game.moves();
  const reply = replies.length ? pickRandom(replies) : null;
  const score = baseScore + Math.round(Math.random() * 40 - 20);
  const scoreText = best.san.endsWith('#') ? 'mate 1 wdl 1000 0 0' : `cp ${score} wdl ${wdlForScore(score)}`;
  const pv = reply ? `${best.uci} ${reply.uci}` : best.uci;

  const total = thinkTime(args);
  const startedAt = Date.now();
  const postInfo = (depth) => {
    const elapsed = Date.now() - startedAt;
    const nodes = depth * 137 + elapsed * 3;
    postMessage(`info depth ${depth} seldepth ${depth + 1} score ${scoreText} nodes ${nodes} nps 3000 hashfull 0 time ${elapsed} pv ${pv}`);
  };

  finishSearch = () => {
    clearSearch();
    postMessage(reply ? `bestmove ${best.uci} ponder ${reply.uci}` : `bestmove ${best.uci}`);
  };

  postInfo(1);
  if (!Number.isFinite(total)) return; // go infinite: wait for stop
  for (let step = 1; step < INFO_STEPS; step++) {
    searchTimers.push(setTimeout(() => postInfo(step + 1), (total * step) / INFO_STEPS));
  }
  searchTimers.push(setTimeout(finishSearch, total));
}

function clearSearch() {
  searchTimers.forEach(clearTimeout);
  searchTimers = [];
  finishSearch = null;
}
