/**
 * main.js - bootstrap: builds the views, the engine client and the controller, and connects
 * buttons, keys and dialogs to controller intents. No game logic lives here.
 */

import { createBoardView } from './board-view.js';
import { attachBoardInput } from './input.js';
import { createMoveListView } from './movelist-view.js';
import { createPanelView } from './panel-view.js';
import { createController } from './controller.js';
import { createImportDialog, createMenuDialog, createNewGameDialog, isAnyDialogOpen } from './dialogs.js';
import { ENGINE_WORKER_URL, EngineClient, STUB_WORKER_URL } from './engine-client.js';

const TOAST_MS = 2200;

function byId(id) {
  return document.getElementById(id);
}

// -- small browser helpers ------------------------------------------------------------

function createToast(node) {
  let timer = null;
  return (message) => {
    node.textContent = message;
    node.classList.add('is-visible');
    clearTimeout(timer);
    timer = setTimeout(() => node.classList.remove('is-visible'), TOAST_MS);
  };
}

const darkQuery = window.matchMedia('(prefers-color-scheme: dark)');

/** Applies 'auto' | 'light' | 'dark' to the page; returns true when the page is now dark. */
function applyTheme(theme) {
  const root = document.documentElement;
  if (theme === 'light' || theme === 'dark') root.dataset.theme = theme;
  else delete root.dataset.theme;
  return theme === 'dark' || (theme === 'auto' && darkQuery.matches);
}

/** Clipboard API first, the old selection trick as a fallback (http pages, older browsers). */
async function copyText(text) {
  try {
    await navigator.clipboard.writeText(text);
    return true;
  } catch {
    const area = document.createElement('textarea');
    area.value = text;
    area.setAttribute('readonly', '');
    area.style.position = 'fixed';
    area.style.opacity = '0';
    document.body.append(area);
    area.select();
    let copied = false;
    try {
      copied = document.execCommand('copy');
    } catch {
      copied = false;
    }
    area.remove();
    return copied;
  }
}

function downloadText(filename, text) {
  const url = URL.createObjectURL(new Blob([text], { type: 'application/x-chess-pgn' }));
  const link = document.createElement('a');
  link.href = url;
  link.download = filename;
  document.body.append(link);
  link.click();
  link.remove();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
}

function pgnFilename(now = new Date()) {
  const pad = (value) => String(value).padStart(2, '0');
  return `chess-${now.getFullYear()}-${pad(now.getMonth() + 1)}-${pad(now.getDate())}.pgn`;
}

/** `?engine=stub` skips the real engine (development and tests). */
function engineWorkerUrl() {
  const wanted = new URLSearchParams(window.location.search).get('engine');
  return wanted === 'stub' ? STUB_WORKER_URL : ENGINE_WORKER_URL;
}

// -- wiring ----------------------------------------------------------------------------

function wireButtons(controller, dialogs) {
  const onClick = (id, handler) => byId(id).addEventListener('click', handler);
  onClick('btn-new', () => dialogs.newGame.open(controller.newGameDefaults()));
  onClick('btn-first', () => controller.goFirst());
  onClick('btn-prev', () => controller.step(-1));
  onClick('btn-next', () => controller.step(1));
  onClick('btn-last', () => controller.goLast());
  onClick('btn-live', () => controller.goLast());
  onClick('btn-flip', () => controller.flip());
  onClick('btn-undo', () => controller.undo());
  onClick('btn-hint', () => controller.hint());
  onClick('btn-resign', () => controller.resign());
  onClick('btn-analysis', () => controller.toggleAnalysis());
  onClick('btn-more', () => dialogs.menu.open(controller.menuState()));
}

function wireKeys(controller) {
  const keys = {
    ArrowLeft: () => controller.step(-1),
    ArrowRight: () => controller.step(1),
    ArrowUp: () => controller.goFirst(),
    ArrowDown: () => controller.goLast(),
    Home: () => controller.goFirst(),
    End: () => controller.goLast(),
  };
  document.addEventListener('keydown', (event) => {
    const handler = keys[event.key];
    if (!handler || event.altKey || event.ctrlKey || event.metaKey || isAnyDialogOpen()) return;
    if (event.target.closest?.('input, textarea, select')) return;
    event.preventDefault();
    handler();
  });
}

function createMenuActions(controller, dialogs, toast) {
  const copied = (what) => (ok) => toast(ok ? `${what} copied` : `Copying is blocked in this browser`);
  return {
    'copy-fen': () => copyText(controller.shownFen()).then(copied('FEN')),
    'copy-pgn': () => copyText(controller.pgn()).then(copied('PGN')),
    'download-pgn': () => downloadText(pgnFilename(), controller.pgn()),
    'load-fen': () => dialogs.fen.open(),
    'load-pgn': () => dialogs.pgn.open(),
    'toggle-legal': () => {
      controller.toggleLegalMoves();
      dialogs.menu.reflect(controller.menuState());
    },
    'toggle-theme': () => {
      controller.toggleTheme();
      dialogs.menu.reflect(controller.menuState());
    },
  };
}

function start() {
  const toast = createToast(byId('toast'));
  let controller = null;

  const engine = new EngineClient({
    workerUrl: engineWorkerUrl(),
    onFallback: (reason) => controller?.engineFellBack(reason),
  });
  const views = {
    board: createBoardView(byId('board')),
    moves: createMoveListView(byId('moves'), { onSelect: (ply) => controller.goTo(ply) }),
    panel: createPanelView(),
  };
  controller = createController({ views, engine, toast, applyTheme });

  const dialogs = {};
  const menuActions = createMenuActions(controller, dialogs, toast);
  dialogs.newGame = createNewGameDialog(byId('dialog-new'), (options) => controller.newGame(options));
  dialogs.menu = createMenuDialog(byId('dialog-menu'), (action) => menuActions[action]?.());
  dialogs.fen = createImportDialog(byId('dialog-fen'), (text) => controller.loadFen(text));
  dialogs.pgn = createImportDialog(byId('dialog-pgn'), (text) => controller.loadPgn(text));

  attachBoardInput(views.board, controller.input);
  wireButtons(controller, dialogs);
  wireKeys(controller);
  darkQuery.addEventListener('change', () => controller.systemThemeChanged());
  window.addEventListener('pagehide', () => controller.persist());
  document.addEventListener('visibilitychange', () => {
    if (document.visibilityState === 'hidden') controller.persist();
  });

  controller.start();
}

start();
