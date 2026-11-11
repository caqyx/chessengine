/**
 * controller.js - game flow. Turns user intents and engine/clock events into state updates
 * (state.js), keeps the engine and the clock in step with the state, saves, and asks the
 * views to render.
 *
 * Every change ends in commit(): save -> syncEngine -> render.
 *
 * The engine does one thing at a time. syncEngine() works out what it should be doing for the
 * current state (play a move, analyse the shown position, find a hint, or nothing), and
 * restarts it only when that task changed. Tasks are identified by a key that contains the
 * position, so "the position changed" and "the task changed" are the same test.
 */

import {
  DEFAULT_OPTIONS,
  analysisActive,
  analysisAllowed,
  canMoveNow,
  createGame,
  createGameFromPgn,
  deserializeGame,
  exportPgn,
  fenAtPly,
  firstMoveInfo,
  flagFall,
  isEngineTurn,
  isLive,
  livePly,
  materialSummary,
  outcome,
  playMove,
  recordClocks,
  resign,
  resigningColor,
  serializeGame,
  setViewPly,
  takeBack,
  takeBackPlies,
  timeControlById,
  viewedChess,
  viewedPly,
} from './state.js';
import { createClock, formatClock } from './clock.js';
import { formatScore, moveTimeForLevel, pvToSan, scoreForWhite, terminalScore, uciToArrow, whiteShare } from './analysis.js';
import { loadSavedGame, loadSettings, saveGame, saveSettings } from './storage.js';

const ANALYSIS_MS = 3000;
const HINT_MS = 1200;
const RESIGN_CONFIRM_MS = 3000;
const LOW_TIME_MS = 10_000;

const NOTICE_STUB = 'Engine unavailable, using random mover';
const NOTICE_NO_ENGINE = 'Engine unavailable';

function otherColor(color) {
  return color === 'w' ? 'b' : 'w';
}

/**
 * @param {Object} deps
 * @param {{board: object, moves: object, panel: object}} deps.views
 * @param {import('./engine-client.js').EngineClient} deps.engine
 * @param {(message: string) => void} deps.toast
 * @param {(theme: 'auto'|'light'|'dark') => boolean} deps.applyTheme  returns true when dark is shown
 */
export function createController({ views, engine, toast, applyTheme }) {
  const settings = loadSettings();
  const clock = createClock({ onTick: renderClocks, onFlag: handleFlag });

  let game = null;
  /** Changes whenever `game` is replaced, so task keys of different games never collide. */
  let gameSerial = 0;
  let selected = null;
  /** What the engine is doing or has finished for the current state: { key }. */
  let task = { key: null };
  /** Analysis of the shown position: { key, fen, turn, info, terminal }. */
  let analysis = null;
  let lastShare = 0.5;
  /** A delivered hint: { key, arrow }. */
  let hint = null;
  let hintWanted = false;
  let resignArmed = false;
  let resignTimer = null;
  let engineNotice = '';
  let renderQueued = false;
  let darkTheme = applyTheme(settings.theme);

  // -- engine tasks -------------------------------------------------------------

  function positionKey(ply) {
    return `${gameSerial}:${ply}:${fenAtPly(game, ply)}`;
  }

  function desiredTask() {
    if (isEngineTurn(game)) return { kind: 'move', key: `move:${positionKey(livePly(game))}` };
    if (analysisActive(game)) return { kind: 'analysis', key: `analysis:${positionKey(viewedPly(game))}` };
    if (hintWanted && canMoveNow(game)) return { kind: 'hint', key: `hint:${positionKey(livePly(game))}` };
    return null;
  }

  function syncEngine() {
    const wanted = desiredTask();
    const key = wanted ? wanted.key : null;
    if (key === task.key) return;
    engine.cancel();
    task = { key };
    if (wanted?.kind === 'move') searchEngineMove(key);
    else if (wanted?.kind === 'analysis') searchAnalysis(key);
    else if (wanted?.kind === 'hint') searchHint(key);
  }

  function searchFrom(ply) {
    return { startFen: game.chess.startFen(), uciMoves: game.chess.uciMoves().slice(0, ply) };
  }

  /** `go` arguments for the engine's own move: the clocks when there are any, else a fixed time. */
  function moveTiming() {
    if (!game.clocks) return { movetime: moveTimeForLevel(game.level) };
    const remaining = clock.read();
    const { incMs } = timeControlById(game.timeControl);
    return { clock: { wtime: remaining.w, btime: remaining.b, winc: incMs, binc: incMs } };
  }

  function searchEngineMove(key) {
    const request = { ...searchFrom(livePly(game)), level: game.level, ...moveTiming() };
    engine.search(request).then((result) => {
      if (!result || task.key !== key) return;
      playEngineMove(result.bestmove);
    });
  }

  function playEngineMove(uci) {
    const mover = game.chess.turn();
    const move = uci ? playMove(game, uci) : null;
    if (!move) {
      engineNotice = 'The engine did not return a legal move. Undo or start a new game.';
      render();
      return;
    }
    finishMove(mover);
  }

  function searchAnalysis(key) {
    const chess = viewedChess(game);
    analysis = { key, fen: chess.fen(), turn: chess.turn(), info: null, terminal: null };
    if (chess.moves().length === 0) {
      analysis.terminal = terminalScore(chess);
      return;
    }
    const request = { ...searchFrom(viewedPly(game)), movetime: ANALYSIS_MS, level: 10 };
    const onInfo = (info) => {
      if (task.key !== key || !info.score || !info.pv?.length) return;
      analysis.info = info;
      queueRender();
    };
    engine.search(request, onInfo);
  }

  function searchHint(key) {
    const request = { ...searchFrom(livePly(game)), movetime: HINT_MS, level: 10 };
    engine.search(request).then((result) => {
      if (!result || task.key !== key) return;
      const arrow = uciToArrow(result.bestmove);
      hint = arrow ? { key, arrow } : null;
      if (!arrow) hintWanted = false;
      render();
    });
  }

  function forgetHint() {
    hint = null;
    hintWanted = false;
  }

  // -- clock ----------------------------------------------------------------------

  /** The first move of a game is not timed; after that the side to move is on the clock. */
  function syncClockToGame() {
    if (!game.clocks) {
      clock.stop();
      return;
    }
    clock.stop();
    clock.set(game.clocks);
    if (livePly(game) >= 1 && !outcome(game)) clock.run(game.chess.turn());
  }

  function clockAfterMove(mover) {
    if (!game.clocks) return;
    if (clock.running() === mover) clock.add(mover, timeControlById(game.timeControl).incMs);
    recordClocks(game, clock.read());
    if (outcome(game)) clock.stop();
    else clock.run(game.chess.turn());
  }

  function handleFlag(color) {
    game.clocks = clock.read();
    flagFall(game, color);
    selected = null;
    views.board.cancelPromotion();
    commit();
  }

  // -- saving -----------------------------------------------------------------------

  function save() {
    const data = serializeGame(game);
    if (game.clocks) data.clocks = clock.read();
    saveGame(data);
  }

  function commit() {
    save();
    syncEngine();
    render();
  }

  // -- view models --------------------------------------------------------------------

  function sideName(color) {
    if (game.mode === 'local') return color === 'w' ? 'White' : 'Black';
    if (color === game.humanColor) return 'You';
    return engine.usingStub ? 'Random mover' : `Engine, level ${game.level}`;
  }

  function winsText(color) {
    if (game.mode === 'local') return `${color === 'w' ? 'White' : 'Black'} wins.`;
    return color === game.humanColor ? 'You win.' : 'The engine wins.';
  }

  function outcomeText(over) {
    switch (over.reason) {
      case 'checkmate':
        return `Checkmate. ${winsText(over.winner)}`;
      case 'stalemate':
        return 'Stalemate. The game is a draw.';
      case 'threefold':
        return 'Draw by threefold repetition.';
      case 'fifty':
        return 'Draw by the fifty-move rule.';
      case 'insufficient':
        return 'Draw by insufficient material.';
      case 'timeout':
        return over.winner ? `Time is up. ${winsText(over.winner)}` : 'Time is up. Draw: a lone king cannot mate.';
      case 'resignation': {
        const loser = otherColor(over.winner);
        const who = game.mode === 'local' ? (loser === 'w' ? 'White' : 'Black') : 'You';
        return `${who} resigned. ${winsText(over.winner)}`;
      }
      default:
        if (over.winner) return `${over.winner === 'w' ? 'White' : 'Black'} won this game.`;
        return 'This game was drawn.';
    }
  }

  function turnText() {
    const turn = game.chess.turn();
    const check = game.chess.inCheck() ? 'Check. ' : '';
    if (game.mode === 'local') return `${check}${turn === 'w' ? 'White' : 'Black'} to move`;
    return turn === game.humanColor ? `${check}Your move` : `${check}The engine is thinking`;
  }

  function reviewText() {
    if (isLive(game)) return null;
    const ply = viewedPly(game);
    return ply === 0 ? 'Viewing the starting position.' : `Viewing move ${ply} of ${livePly(game)}.`;
  }

  function targetsOf(square) {
    const seen = new Map();
    for (const move of game.chess.moves({ square })) seen.set(move.to, move.flags.includes('c'));
    return [...seen].map(([target, capture]) => ({ square: target, capture }));
  }

  function shownArrows() {
    if (analysisActive(game)) {
      const best = analysis && analysis.key === task.key ? uciToArrow(analysis.info?.pv?.[0]) : null;
      return best ? [best] : [];
    }
    const hintShown = hint && isLive(game) && hint.key === `hint:${positionKey(livePly(game))}`;
    return hintShown ? [hint.arrow] : [];
  }

  function boardModel(chess, ply) {
    const movable = canMoveNow(game);
    const lastMove = ply > 0 ? game.moves[ply - 1] : null;
    const shownSelection = movable ? selected : null;
    return {
      board: chess.board(),
      orientation: settings.flipped ? 'b' : 'w',
      lastMove: lastMove ? { from: lastMove.from, to: lastMove.to } : null,
      checkSquare: chess.inCheck() ? chess.kingSquare(chess.turn()) : null,
      selected: shownSelection,
      targets: shownSelection && settings.showLegalMoves ? targetsOf(shownSelection) : [],
      arrows: shownArrows(),
      interactive: movable,
    };
  }

  function clockModel(color, remaining, over) {
    if (!remaining) return null;
    return {
      text: formatClock(remaining[color]),
      running: !over && clock.running() === color,
      low: remaining[color] < LOW_TIME_MS,
    };
  }

  function playerModel(color, material, remaining, over) {
    return {
      color,
      name: sideName(color),
      captured: material[color],
      lead: color === 'w' ? material.lead : -material.lead,
      toMove: !over && game.chess.turn() === color,
      clock: clockModel(color, remaining, over),
    };
  }

  function playersModel(chess, over) {
    const material = materialSummary(chess.board());
    const remaining = game.clocks ? clock.read() : null;
    const bottom = settings.flipped ? 'b' : 'w';
    return {
      top: playerModel(otherColor(bottom), material, remaining, over),
      bottom: playerModel(bottom, material, remaining, over),
    };
  }

  function controlsModel(ply) {
    const active = analysisActive(game);
    return {
      canBack: ply > 0,
      canForward: ply < livePly(game),
      canUndo: takeBackPlies(game) > 0,
      canHint: canMoveNow(game) && !active,
      hintBusy: Boolean(task.key?.startsWith('hint:')) && !hint,
      canResign: resigningColor(game) !== null,
      resignArmed,
      analysisOn: active,
      analysisLocked: !analysisAllowed(game),
    };
  }

  function analysisModel() {
    const model = { visible: analysisActive(game), share: lastShare, score: '', depth: 0, line: '', flipped: settings.flipped };
    if (!model.visible || !analysis || analysis.key !== task.key) return model;
    if (analysis.terminal) {
      model.share = lastShare = whiteShare(analysis.terminal);
      model.score = analysis.terminal.type === 'mate' ? formatScore(analysis.terminal) : '0.0';
      model.line = analysis.terminal.type === 'mate' ? 'Checkmate' : 'Stalemate';
      return model;
    }
    if (!analysis.info) {
      model.line = engine.usingStub ? 'Random mover is guessing' : 'Engine is thinking';
      return model;
    }
    const score = scoreForWhite(analysis.info.score, analysis.turn);
    model.share = lastShare = whiteShare(score);
    model.score = formatScore(score);
    model.depth = analysis.info.depth ?? 0;
    model.line = pvToSan(analysis.fen, analysis.info.pv);
    return model;
  }

  function moveListModel(ply) {
    const first = firstMoveInfo(game);
    return {
      sans: game.moves.map((move) => move.san),
      firstNumber: first.number,
      blackStarts: first.blackStarts,
      currentPly: ply,
    };
  }

  function render() {
    renderQueued = false;
    const chess = viewedChess(game);
    const ply = viewedPly(game);
    const over = outcome(game);
    views.board.render(boardModel(chess, ply));
    views.moves.render(moveListModel(ply));
    views.panel.render({
      players: playersModel(chess, over),
      status: { line: over ? outcomeText(over) : turnText(), over: Boolean(over), review: reviewText() },
      controls: controlsModel(ply),
      analysis: analysisModel(),
      notice: engineNotice,
    });
  }

  /** Engine info lines can arrive in bursts; draw them at most once per frame. */
  function queueRender() {
    if (renderQueued) return;
    renderQueued = true;
    requestAnimationFrame(() => {
      if (renderQueued) render();
    });
  }

  function renderClocks(remaining) {
    if (!game?.clocks) return;
    const over = outcome(game);
    const bottom = settings.flipped ? 'b' : 'w';
    views.panel.renderClocks(clockModel(otherColor(bottom), remaining, over), clockModel(bottom, remaining, over));
  }

  // -- moves --------------------------------------------------------------------------

  function finishMove(mover) {
    selected = null;
    forgetHint();
    disarmResign();
    clockAfterMove(mover);
    commit();
  }

  /** Asks for the promotion piece when needed; resolves to undefined for ordinary moves. */
  async function choosePromotion(from, to) {
    const options = game.chess.moves({ square: from }).filter((move) => move.to === to);
    if (options.length === 0) return null;
    if (!options.some((move) => move.promotion)) return undefined;
    return views.board.askPromotion(to, game.chess.turn());
  }

  async function movePiece(from, to) {
    if (!canMoveNow(game)) return;
    const startedIn = game;
    const startedAt = livePly(game);
    const promotion = await choosePromotion(from, to);
    const stale = game !== startedIn || livePly(game) !== startedAt || !canMoveNow(game);
    const mover = game.chess.turn();
    const move = promotion === null || stale ? null : playMove(game, { from, to, promotion });
    if (!move) {
      selected = null;
      render();
      return;
    }
    finishMove(mover);
  }

  // -- replacing the game -----------------------------------------------------------------

  function replaceGame(next) {
    engine.cancel();
    views.board.cancelPromotion();
    game = next;
    gameSerial++;
    selected = null;
    analysis = null;
    lastShare = 0.5;
    task = { key: null };
    forgetHint();
    disarmResign();
    if (engineNotice !== NOTICE_STUB && engineNotice !== NOTICE_NO_ENGINE) engineNotice = '';
    engine.newGame();
    engine.setLevel(game.level);
    syncClockToGame();
    commit();
  }

  function currentOptions() {
    return { mode: game.mode, humanColor: game.humanColor, level: game.level, timeControl: game.timeControl };
  }

  function resolveColor(choice) {
    if (choice === 'w' || choice === 'b') return choice;
    return Math.random() < 0.5 ? 'w' : 'b';
  }

  function disarmResign() {
    clearTimeout(resignTimer);
    resignArmed = false;
  }

  /** Shows the position after `ply` moves (review); the latest ply is the live game. */
  function goTo(ply) {
    setViewPly(game, ply);
    selected = null;
    views.board.cancelPromotion();
    commit();
  }

  // -- public: intents ---------------------------------------------------------------------

  return {
    /** Restores the saved game (or starts a fresh one) and draws the first frame. */
    start() {
      const restored = deserializeGame(loadSavedGame());
      const fresh = () => createGame({ ...DEFAULT_OPTIONS });
      game = restored ?? fresh();
      gameSerial++;
      engine.setLevel(game.level);
      engine.init();
      syncClockToGame();
      commit();
    },

    /** Hooks for input.js. */
    input: {
      selection: () => (canMoveNow(game) ? selected : null),
      canPickUp(square) {
        const piece = canMoveNow(game) ? game.chess.get(square) : null;
        return Boolean(piece) && piece.color === game.chess.turn();
      },
      isTarget: (from, to) => canMoveNow(game) && game.chess.moves({ square: from }).some((move) => move.to === to),
      select(square) {
        selected = square;
        render();
      },
      move: movePiece,
    },

    goTo,
    step: (delta) => goTo(viewedPly(game) + delta),
    goFirst: () => goTo(0),
    goLast: () => goTo(livePly(game)),

    undo() {
      const plies = takeBackPlies(game);
      if (plies === 0) return;
      views.board.cancelPromotion();
      takeBack(game, plies);
      selected = null;
      forgetHint();
      disarmResign();
      syncClockToGame();
      commit();
    },

    hint() {
      if (!canMoveNow(game) || analysisActive(game)) return;
      hintWanted = true;
      commit();
    },

    /** The first press arms the button, a second press within a few seconds resigns. */
    resign() {
      const color = resigningColor(game);
      if (!color) return;
      if (!resignArmed) {
        resignArmed = true;
        resignTimer = setTimeout(() => {
          resignArmed = false;
          render();
        }, RESIGN_CONFIRM_MS);
        render();
        return;
      }
      disarmResign();
      views.board.cancelPromotion();
      if (game.clocks) game.clocks = clock.read();
      clock.stop();
      resign(game, color);
      selected = null;
      forgetHint();
      commit();
    },

    toggleAnalysis() {
      if (!analysisAllowed(game)) {
        toast('Analysis opens when the game is over. Use Hint for a suggestion.');
        return;
      }
      game.analysisOn = !game.analysisOn;
      forgetHint();
      commit();
    },

    flip() {
      settings.flipped = !settings.flipped;
      saveSettings(settings);
      views.board.cancelPromotion();
      render();
    },

    /** @param {{mode: string, color: 'w'|'b'|'random', level: number, timeControl: string}} options */
    newGame(options) {
      const humanColor = resolveColor(options.color);
      settings.newGame = { ...options };
      settings.flipped = options.mode === 'engine' && humanColor === 'b';
      saveSettings(settings);
      replaceGame(createGame({ ...options, humanColor }));
    },

    /** Options to preselect in the New game dialog. */
    newGameDefaults() {
      const last = settings.newGame ?? {};
      return {
        mode: last.mode ?? game.mode,
        color: last.color ?? game.humanColor,
        level: last.level ?? game.level,
        timeControl: last.timeControl ?? game.timeControl,
      };
    },

    /** @returns {string|null} an error message, or null when the position was loaded */
    loadFen(text) {
      const fen = text.trim();
      if (!fen) return 'Paste a FEN first.';
      let next;
      try {
        next = createGame({ ...currentOptions(), startFen: fen });
      } catch (error) {
        return `${error.message}.`;
      }
      replaceGame(next);
      return null;
    },

    /** @returns {string|null} an error message, or null when the game was loaded */
    loadPgn(text) {
      if (!text.trim()) return 'Paste a PGN first.';
      const next = createGameFromPgn(text);
      if (!next) return 'This PGN cannot be read. It may contain an illegal or misspelled move.';
      replaceGame(next);
      return null;
    },

    /** FEN of the position on the board (the reviewed one while reviewing). */
    shownFen() {
      return fenAtPly(game, viewedPly(game));
    },

    pgn() {
      return exportPgn(game);
    },

    toggleLegalMoves() {
      settings.showLegalMoves = !settings.showLegalMoves;
      saveSettings(settings);
      render();
    },

    toggleTheme() {
      settings.theme = darkTheme ? 'light' : 'dark';
      saveSettings(settings);
      darkTheme = applyTheme(settings.theme);
    },

    /** The system theme changed; only matters while the user has not chosen one. */
    systemThemeChanged() {
      if (settings.theme === 'auto') darkTheme = applyTheme('auto');
    },

    menuState() {
      return { showLegalMoves: settings.showLegalMoves, darkTheme };
    },

    /** The engine client switched to the stub (or found no engine at all). */
    engineFellBack(reason) {
      engineNotice = reason === 'none' ? NOTICE_NO_ENGINE : NOTICE_STUB;
      if (game) render();
    },

    /** Called when the page is hidden or closed, so the running clock is stored. */
    persist() {
      if (game) save();
    },
  };
}
