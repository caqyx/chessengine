/**
 * dialogs.js - the modal sheets: new game, the "More" menu, and the two import dialogs.
 *
 * All of them are native <dialog> elements. Each factory wires one dialog and returns an
 * object with an `open` method; what happens with the user's choice is up to the callbacks.
 */

/** True when a click landed on the backdrop: outside the dialog's own box. */
function isBackdropClick(dialog, event) {
  if (event.target !== dialog) return false;
  const box = dialog.getBoundingClientRect();
  const inside =
    event.clientX >= box.left && event.clientX <= box.right && event.clientY >= box.top && event.clientY <= box.bottom;
  return !inside;
}

/** Closes on the Cancel / Close buttons and on a click on the backdrop. */
function wireDismissal(dialog) {
  dialog.addEventListener('click', (event) => {
    if (isBackdropClick(dialog, event) || event.target.closest('[data-close]')) dialog.close();
  });
}

function openModal(dialog) {
  if (!dialog.open) dialog.showModal();
}

/** True while any modal dialog is open (keyboard shortcuts pause then). */
export function isAnyDialogOpen() {
  return document.querySelector('dialog[open]') !== null;
}

/**
 * @param {HTMLDialogElement} dialog
 * @param {(options: {mode: string, color: string, level: number, timeControl: string}) => void} onStart
 */
export function createNewGameDialog(dialog, onStart) {
  const form = dialog.querySelector('form');
  const level = form.elements.level;
  const levelOutput = dialog.querySelector('output');
  const engineOnly = [...dialog.querySelectorAll('[data-engine-only]')];

  function reflect() {
    const againstEngine = form.elements.mode.value === 'engine';
    engineOnly.forEach((field) => {
      field.hidden = !againstEngine;
    });
    levelOutput.textContent = level.value;
  }

  wireDismissal(dialog);
  form.addEventListener('input', reflect);
  form.addEventListener('submit', () => {
    onStart({
      mode: form.elements.mode.value,
      color: form.elements.color.value,
      level: Number(level.value),
      timeControl: form.elements.timeControl.value,
    });
  });

  return {
    /** @param {{mode: string, color: string, level: number, timeControl: string}} options */
    open(options) {
      form.elements.mode.value = options.mode;
      form.elements.color.value = options.color;
      form.elements.timeControl.value = options.timeControl;
      level.value = String(options.level);
      reflect();
      openModal(dialog);
    },
  };
}

/**
 * A dialog with one text field. `onSubmit` returns an error message to show inline (the
 * dialog stays open and nothing else changes) or null when the text was accepted.
 * @param {HTMLDialogElement} dialog
 * @param {(text: string) => string|null} onSubmit
 */
export function createImportDialog(dialog, onSubmit) {
  const form = dialog.querySelector('form');
  const field = form.elements.text;
  const error = dialog.querySelector('.field-error');

  function showError(message) {
    error.textContent = message ?? '';
    error.hidden = !message;
    field.setAttribute('aria-invalid', message ? 'true' : 'false');
  }

  wireDismissal(dialog);
  field.addEventListener('input', () => showError(null));
  form.addEventListener('submit', (event) => {
    const message = onSubmit(field.value);
    if (message) {
      event.preventDefault();
      showError(message);
      field.focus();
    }
  });

  return {
    open() {
      field.value = '';
      showError(null);
      openModal(dialog);
      field.focus();
    },
  };
}

/**
 * @param {HTMLDialogElement} dialog
 * @param {(action: string) => void} onAction  receives the data-action of the pressed item
 */
export function createMenuDialog(dialog, onAction) {
  const legal = dialog.querySelector('[data-action="toggle-legal"]');
  const winChance = dialog.querySelector('[data-action="toggle-winchance"]');
  const theme = dialog.querySelector('[data-action="toggle-theme"]');

  function setSwitch(button, on) {
    button.setAttribute('aria-checked', String(on));
    button.querySelector('.menu-state').textContent = on ? 'On' : 'Off';
  }

  function reflect({ showLegalMoves, winChance: winChanceOn, darkTheme }) {
    setSwitch(legal, showLegalMoves);
    setSwitch(winChance, winChanceOn);
    setSwitch(theme, darkTheme);
  }

  wireDismissal(dialog);
  dialog.addEventListener('click', (event) => {
    const item = event.target.closest('[data-action]');
    if (!item) return;
    const isSwitch = item.getAttribute('role') === 'switch';
    if (!isSwitch) dialog.close();
    onAction(item.dataset.action);
  });

  return {
    /** @param {{showLegalMoves: boolean, winChance: boolean, darkTheme: boolean}} state */
    open(state) {
      reflect(state);
      openModal(dialog);
    },
    /** Updates the switches while the menu stays open. */
    reflect,
  };
}
