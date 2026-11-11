/**
 * storage.js - localStorage access that never throws.
 *
 * Storage can be missing, full or blocked (private mode, embedded views). Every call is
 * wrapped, and a failure simply means "nothing saved".
 */

const GAME_KEY = 'chess.game.v1';
const SETTINGS_KEY = 'chess.settings.v1';

export const DEFAULT_SETTINGS = {
  /** 'auto' follows the system; 'light' and 'dark' are explicit choices. */
  theme: 'auto',
  flipped: false,
  showLegalMoves: true,
  /** The win-chance indicator under the status line. */
  winChance: true,
  /** Options last used in the New game dialog. */
  newGame: null,
};

function readJson(key) {
  try {
    const text = window.localStorage.getItem(key);
    return text ? JSON.parse(text) : null;
  } catch {
    return null;
  }
}

function writeJson(key, value) {
  try {
    window.localStorage.setItem(key, JSON.stringify(value));
    return true;
  } catch {
    return false;
  }
}

export function loadSettings() {
  const saved = readJson(SETTINGS_KEY);
  const settings = { ...DEFAULT_SETTINGS, ...(saved && typeof saved === 'object' ? saved : {}) };
  if (!['auto', 'light', 'dark'].includes(settings.theme)) settings.theme = 'auto';
  settings.flipped = settings.flipped === true;
  settings.showLegalMoves = settings.showLegalMoves !== false;
  settings.winChance = settings.winChance !== false;
  return settings;
}

export function saveSettings(settings) {
  return writeJson(SETTINGS_KEY, settings);
}

/** @returns {object|null} whatever saveGame() stored last */
export function loadSavedGame() {
  return readJson(GAME_KEY);
}

export function saveGame(data) {
  return writeJson(GAME_KEY, data);
}
