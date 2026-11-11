/**
 * engine-client.js - promise based wrapper around the engine Web Worker.
 *
 * The worker speaks a subset of UCI, one line per message (see docs/engine-protocol.md).
 * A running search cannot be interrupted with `stop`, so cancelling means: terminate the
 * worker, start a new one, re-send the options. The position is sent again with every search.
 *
 * Guarantee: the promise of a search that was cancelled or superseded resolves with `null`
 * and its `onInfo` callback is never called again. A result object is only delivered for the
 * search that is still current.
 */

import { START_FEN } from './chess.js';

/** The one place that knows where the real engine worker lives (engine/web/ next to index.html). */
export const ENGINE_WORKER_URL = new URL('../../engine/web/engine-worker.js', import.meta.url).href;

/** Random mover used while the real engine is missing or broken. */
export const STUB_WORKER_URL = new URL('../dev/stub-engine-worker.js', import.meta.url).href;

const HANDSHAKE_TIMEOUT_MS = 12000;
const NUMERIC_INFO_FIELDS = ['depth', 'seldepth', 'multipv', 'nodes', 'nps', 'hashfull', 'time'];

/**
 * @typedef {Object} SearchRequest
 * @property {string} startFen            FEN the game started from
 * @property {string[]} uciMoves          moves played since then, e.g. ['e2e4', 'e7e5']
 * @property {number} [movetime]          fixed thinking time in ms (used when `clock` is absent)
 * @property {{wtime:number, btime:number, winc:number, binc:number}} [clock]
 * @property {number} [level]             1..10, overrides the level set with setLevel()
 *
 * @typedef {Object} EngineInfo
 * @property {number} [depth]
 * @property {{type:'cp'|'mate', value:number, bound?:'lowerbound'|'upperbound'}} [score]
 *           from the side to move point of view
 * @property {string[]} [pv]
 */

/**
 * Parses one `info ...` line. Unknown tokens are skipped.
 * @param {string} line
 * @returns {EngineInfo}
 */
export function parseInfoLine(line) {
  const tokens = line.trim().split(/\s+/);
  const info = {};
  for (let i = 1; i < tokens.length; i++) {
    const token = tokens[i];
    if (NUMERIC_INFO_FIELDS.includes(token)) {
      info[token] = Number(tokens[++i]);
    } else if (token === 'score') {
      info.score = { type: tokens[i + 1] === 'mate' ? 'mate' : 'cp', value: Number(tokens[i + 2]) };
      i += 2;
      if (tokens[i + 1] === 'lowerbound' || tokens[i + 1] === 'upperbound') info.score.bound = tokens[++i];
    } else if (token === 'pv') {
      info.pv = tokens.slice(i + 1);
      break;
    } else if (token === 'string') {
      break;
    }
  }
  return info;
}

/** @returns {{bestmove: string|null, ponder: string|null}} */
function parseBestmoveLine(line) {
  const tokens = line.trim().split(/\s+/);
  const isMove = (text) => /^[a-h][1-8][a-h][1-8][qrbn]?$/.test(text ?? '');
  const ponderAt = tokens.indexOf('ponder');
  return {
    bestmove: isMove(tokens[1]) ? tokens[1] : null,
    ponder: ponderAt > 0 && isMove(tokens[ponderAt + 1]) ? tokens[ponderAt + 1] : null,
  };
}

function positionCommand({ startFen, uciMoves = [] }) {
  const base = !startFen || startFen === START_FEN ? 'position startpos' : `position fen ${startFen}`;
  return uciMoves.length ? `${base} moves ${uciMoves.join(' ')}` : base;
}

function goCommand({ movetime, clock }) {
  if (clock) {
    const ms = (value) => Math.max(1, Math.round(value));
    const inc = (value) => Math.max(0, Math.round(value || 0));
    return `go wtime ${ms(clock.wtime)} btime ${ms(clock.btime)} winc ${inc(clock.winc)} binc ${inc(clock.binc)}`;
  }
  return `go movetime ${Math.max(1, Math.round(movetime ?? 1000))}`;
}

function clampLevel(level) {
  return Math.min(10, Math.max(1, Math.round(Number(level) || 10)));
}

export class EngineClient {
  #workerUrl;
  #stubUrl;
  #onFallback;
  #hashMb;
  #createWorker;

  #worker = null;
  #usingStub = false;
  #dead = false;
  #level = 10;
  #sentLevel = null;
  /** The search in flight: { request, onInfo, resolve }. */
  #pending = null;
  #handshakeTimer = null;
  #ready = null;
  #resolveReady = null;

  /**
   * @param {Object} [options]
   * @param {string} [options.workerUrl]   real engine worker
   * @param {string} [options.stubUrl]     fallback worker; pass null to disable the fallback
   * @param {(reason: string) => void} [options.onFallback]  called once when the stub takes over
   *        (reason is a short technical text), or with 'none' when no engine could be started at all
   * @param {number} [options.hashMb]
   * @param {(url: string) => Worker} [options.createWorker]  for tests
   */
  constructor({
    workerUrl = ENGINE_WORKER_URL,
    stubUrl = STUB_WORKER_URL,
    onFallback = () => {},
    hashMb = 32,
    createWorker = (url) => new Worker(url),
  } = {}) {
    this.#workerUrl = workerUrl;
    this.#stubUrl = stubUrl;
    this.#onFallback = onFallback;
    this.#hashMb = hashMb;
    this.#createWorker = createWorker;
  }

  /** True once the random mover replaced the real engine. */
  get usingStub() {
    return this.#usingStub;
  }

  /** True when a search is in flight. */
  get busy() {
    return this.#pending !== null;
  }

  /**
   * Starts the worker. Resolves when an engine (real or stub) answered `readyok`.
   * Calling search() before that is fine: the worker queues commands while it loads.
   * @returns {Promise<void>}
   */
  init() {
    if (!this.#ready) {
      this.#ready = new Promise((resolve) => {
        this.#resolveReady = resolve;
      });
      this.#spawn();
    }
    return this.#ready;
  }

  /** Tells the engine a new game starts (clears its hash). Cancels a running search. */
  newGame() {
    this.cancel();
    this.#post('ucinewgame');
  }

  /** Default playing strength for the following searches, 1 (beginner) .. 10 (full). */
  setLevel(level) {
    this.#level = clampLevel(level);
  }

  /**
   * Searches a position. A search that is still running is cancelled first.
   * @param {SearchRequest} request
   * @param {(info: EngineInfo) => void} [onInfo]  called for every `info` line of this search
   * @returns {Promise<{bestmove: string|null, ponder: string|null}|null>}
   *          null when the search was cancelled or superseded; bestmove is null when the
   *          position has no legal move
   */
  search(request, onInfo) {
    this.init();
    this.cancel();
    if (this.#dead) return Promise.resolve(null);
    return new Promise((resolve) => {
      this.#pending = { request, onInfo, resolve };
      this.#issue(this.#pending);
    });
  }

  /** Cancels the running search (terminate + respawn). Does nothing when idle. */
  cancel() {
    const pending = this.#pending;
    if (!pending) return;
    this.#pending = null;
    this.#spawn();
    pending.resolve(null);
  }

  /** Stops everything; the client cannot be used afterwards. */
  dispose() {
    const pending = this.#pending;
    this.#pending = null;
    this.#dead = true;
    this.#terminate();
    pending?.resolve(null);
  }

  // -- worker lifecycle -----------------------------------------------------

  #terminate() {
    clearTimeout(this.#handshakeTimer);
    if (!this.#worker) return;
    this.#worker.onmessage = null;
    this.#worker.onerror = null;
    this.#worker.onmessageerror = null;
    this.#worker.terminate();
    this.#worker = null;
  }

  /** Replaces the current worker with a fresh one and repeats the handshake and options. */
  #spawn() {
    this.#terminate();
    if (this.#dead) return;
    const url = this.#usingStub ? this.#stubUrl : this.#workerUrl;
    let worker;
    try {
      worker = this.#createWorker(url);
    } catch (error) {
      this.#fail(`cannot start worker: ${error?.message ?? error}`);
      return;
    }
    this.#worker = worker;
    this.#sentLevel = null;
    worker.onmessage = (event) => {
      if (worker === this.#worker) this.#handleLine(String(event.data));
    };
    worker.onerror = (event) => {
      event.preventDefault?.();
      if (worker === this.#worker) this.#fail(event.message || 'worker failed to load');
    };
    worker.onmessageerror = () => {
      if (worker === this.#worker) this.#fail('unreadable message from worker');
    };
    this.#handshakeTimer = setTimeout(() => this.#fail('engine did not answer'), HANDSHAKE_TIMEOUT_MS);
    this.#post('uci');
    this.#post(`setoption name Hash value ${this.#hashMb}`);
    this.#post('isready');
  }

  /** The current worker is unusable: switch to the stub once, otherwise give up. */
  #fail(reason) {
    this.#terminate();
    if (this.#usingStub || !this.#stubUrl) {
      this.#dead = true;
      const pending = this.#pending;
      this.#pending = null;
      pending?.resolve(null);
      this.#onFallback('none');
      this.#resolveReady?.();
      return;
    }
    this.#usingStub = true;
    this.#spawn();
    this.#onFallback(reason);
    if (this.#pending) this.#issue(this.#pending);
  }

  // -- protocol -------------------------------------------------------------

  #post(line) {
    this.#worker?.postMessage(line);
  }

  #issue({ request }) {
    const level = clampLevel(request.level ?? this.#level);
    if (level !== this.#sentLevel) {
      this.#post(`setoption name Level value ${level}`);
      this.#sentLevel = level;
    }
    this.#post(positionCommand(request));
    this.#post(goCommand(request));
  }

  #handleLine(line) {
    if (line.startsWith('error')) {
      this.#fail(line);
    } else if (line.startsWith('readyok')) {
      clearTimeout(this.#handshakeTimer);
      this.#resolveReady?.();
    } else if (line.startsWith('info ')) {
      this.#pending?.onInfo?.(parseInfoLine(line));
    } else if (line.startsWith('bestmove')) {
      const pending = this.#pending;
      this.#pending = null;
      pending?.resolve(parseBestmoveLine(line));
    }
  }
}
