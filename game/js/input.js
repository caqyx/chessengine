/**
 * input.js - moving pieces with a pointer (mouse, touch or pen).
 *
 * Two ways to move, both built on Pointer Events:
 *   tap-tap   press a piece to select it, press a marked square to move there
 *   drag      press a piece, move the pointer, release over the target square
 *
 * The module knows nothing about chess. It asks the controller through `hooks`:
 *   selection()            -> currently selected square or null
 *   canPickUp(square)      -> true when a piece of the side to move stands there and may be moved
 *   isTarget(from, to)     -> true when from -> to is a legal move
 *   select(square | null)  -> change the selection (the controller re-renders)
 *   move(from, to)         -> play the move (the controller may first ask for a promotion piece)
 */

const DRAG_THRESHOLD_MOUSE = 4;
const DRAG_THRESHOLD_TOUCH = 10;

/**
 * @param {ReturnType<import('./board-view.js').createBoardView>} view
 * @param {object} hooks see above
 */
export function attachBoardInput(view, hooks) {
  const board = view.element;
  /** The press in progress: { pointerId, square, node, startX, startY, dragging, wasSelected, touch } */
  let press = null;

  function onPointerDown(event) {
    if (!event.isPrimary || (event.pointerType === 'mouse' && event.button !== 0)) return;
    if (event.target.closest('.promotion')) return;
    const square = view.squareAt(event.clientX, event.clientY);
    if (!square) return;
    event.preventDefault();

    const selected = hooks.selection();
    if (selected && selected !== square && hooks.isTarget(selected, square)) {
      hooks.move(selected, square);
      return;
    }
    if (!hooks.canPickUp(square)) {
      if (selected) hooks.select(null);
      return;
    }
    hooks.select(square);
    press = {
      pointerId: event.pointerId,
      square,
      node: view.pieceNode(square),
      startX: event.clientX,
      startY: event.clientY,
      dragging: false,
      wasSelected: selected === square,
      touch: event.pointerType !== 'mouse',
    };
    try {
      board.setPointerCapture(event.pointerId);
    } catch {
      // capture is a convenience; dragging still works while the pointer stays over the board
    }
  }

  function onPointerMove(event) {
    if (!press || event.pointerId !== press.pointerId || !press.node) return;
    if (!press.dragging) {
      const distance = Math.hypot(event.clientX - press.startX, event.clientY - press.startY);
      if (distance < (press.touch ? DRAG_THRESHOLD_TOUCH : DRAG_THRESHOLD_MOUSE)) return;
      press.dragging = true;
    }
    // On touch screens the piece floats above the finger so that it stays visible.
    const lift = press.touch ? board.getBoundingClientRect().width / 10 : 0;
    view.dragPiece(press.node, event.clientX, event.clientY, lift);
    const over = view.squareAt(event.clientX, event.clientY - lift);
    view.setHover(over && hooks.isTarget(press.square, over) ? over : null);
  }

  function onPointerUp(event) {
    if (!press || event.pointerId !== press.pointerId) return;
    const finished = press;
    endPress();
    if (!finished.dragging) {
      if (finished.wasSelected) hooks.select(null);
      return;
    }
    const lift = finished.touch ? board.getBoundingClientRect().width / 10 : 0;
    const target = view.squareAt(event.clientX, event.clientY - lift);
    if (target && target !== finished.square && hooks.isTarget(finished.square, target)) {
      view.dropPiece(finished.node, target);
      hooks.move(finished.square, target);
    } else {
      view.dropPiece(finished.node);
    }
  }

  function onPointerCancel(event) {
    if (!press || event.pointerId !== press.pointerId) return;
    const cancelled = press;
    endPress();
    if (cancelled.dragging) view.dropPiece(cancelled.node);
  }

  function endPress() {
    try {
      board.releasePointerCapture(press.pointerId);
    } catch {
      // already released
    }
    press = null;
    view.setHover(null);
  }

  board.addEventListener('pointerdown', onPointerDown);
  board.addEventListener('pointermove', onPointerMove);
  board.addEventListener('pointerup', onPointerUp);
  board.addEventListener('pointercancel', onPointerCancel);
  // A long press must not open the context menu or start a native image drag on the pieces.
  board.addEventListener('contextmenu', (event) => event.preventDefault());
  board.addEventListener('dragstart', (event) => event.preventDefault());
}
