/**
 * board-view.js - draws the board: squares, coordinates, highlights, pieces, arrows and the
 * promotion picker. It renders whatever model it is given and holds no game logic.
 *
 * Layers inside the board element, bottom to top:
 *   .squares    64 static cells carrying the highlight classes and coordinate labels
 *   .pieces     one absolutely positioned element per piece, moved with CSS transforms
 *   .arrows     an SVG overlay (best move, hint)
 *   .promotion  the picker, shown while a pawn waits for its new piece
 *
 * @typedef {Object} BoardModel
 * @property {(import('./chess.js').Piece|null)[][]} board   chess.board()
 * @property {'w'|'b'} orientation                          colour at the bottom
 * @property {{from: string, to: string}|null} lastMove
 * @property {string|null} checkSquare                      square of a king in check
 * @property {string|null} selected
 * @property {{square: string, capture: boolean}[]} targets legal destinations to mark
 * @property {{from: string, to: string}[]} arrows
 * @property {boolean} interactive                          false while the board is locked
 */

import { pieceSvg, pieceName } from './pieces.js';

const FILES = 'abcdefgh';
const SVG_NS = 'http://www.w3.org/2000/svg';
const PROMOTION_CHOICES = ['q', 'r', 'b', 'n'];
const LEAVE_MS = 180;

function element(tag, className) {
  const node = document.createElement(tag);
  if (className) node.className = className;
  return node;
}

/** Column and row on screen (0,0 = top left) of a square for the given orientation. */
function cellOf(square, orientation) {
  const file = FILES.indexOf(square[0]);
  const rank = Number(square[1]);
  return orientation === 'w' ? { col: file, row: 8 - rank } : { col: 7 - file, row: rank - 1 };
}

function squareAtCell(col, row, orientation) {
  return orientation === 'w' ? `${FILES[col]}${8 - row}` : `${FILES[7 - col]}${row + 1}`;
}

function isLightSquare(square) {
  return (FILES.indexOf(square[0]) + Number(square[1])) % 2 === 1;
}

function cellDistance(a, b, orientation) {
  const from = cellOf(a, orientation);
  const to = cellOf(b, orientation);
  return Math.abs(from.col - to.col) + Math.abs(from.row - to.row);
}

/** @param {HTMLElement} root the empty .board element */
export function createBoardView(root) {
  const squaresLayer = element('div', 'squares');
  const piecesLayer = element('div', 'pieces');
  const arrowsLayer = document.createElementNS(SVG_NS, 'svg');
  arrowsLayer.setAttribute('class', 'arrows');
  arrowsLayer.setAttribute('viewBox', '0 0 8 8');
  arrowsLayer.setAttribute('aria-hidden', 'true');
  const promotionLayer = element('div', 'promotion');
  promotionLayer.hidden = true;
  root.replaceChildren(squaresLayer, piecesLayer, arrowsLayer, promotionLayer);

  let orientation = null;
  /** square name -> cell element */
  const squareNodes = new Map();
  /** square name -> { node, code } where code is colour + type, e.g. 'wn' */
  let pieces = new Map();
  let hoverSquare = null;
  let closePromotion = null;

  // -- squares ----------------------------------------------------------------

  function buildSquares(nextOrientation) {
    orientation = nextOrientation;
    squareNodes.clear();
    const cells = [];
    for (let row = 0; row < 8; row++) {
      for (let col = 0; col < 8; col++) {
        const square = squareAtCell(col, row, orientation);
        const cell = element('div', `square ${isLightSquare(square) ? 'light' : 'dark'}`);
        cell.dataset.square = square;
        if (col === 0) cell.append(coordinate('rank', square[1]));
        if (row === 7) cell.append(coordinate('file', square[0]));
        squareNodes.set(square, cell);
        cells.push(cell);
      }
    }
    squaresLayer.replaceChildren(...cells);
  }

  function coordinate(kind, text) {
    const label = element('span', `coord coord-${kind}`);
    label.textContent = text;
    return label;
  }

  function markSquares(model) {
    const targets = new Map(model.targets.map((target) => [target.square, target.capture]));
    for (const [square, cell] of squareNodes) {
      const list = cell.classList;
      list.toggle('last', Boolean(model.lastMove) && (model.lastMove.from === square || model.lastMove.to === square));
      list.toggle('selected', model.selected === square);
      list.toggle('check', model.checkSquare === square);
      list.toggle('target', targets.get(square) === false);
      list.toggle('target-capture', targets.get(square) === true);
      list.toggle('hover', hoverSquare === square);
    }
  }

  // -- pieces -----------------------------------------------------------------

  function createPiece(code) {
    const node = element('div', 'piece');
    setPieceCode(node, code);
    return node;
  }

  function setPieceCode(node, code) {
    node.dataset.piece = code;
    node.innerHTML = pieceSvg(code[0], code[1]);
    node.setAttribute('aria-label', pieceName(code[0], code[1]));
  }

  function placePiece(node, square) {
    if (node.classList.contains('dragging')) return;
    const { col, row } = cellOf(square, orientation);
    node.style.transform = `translate(${col * 100}%, ${row * 100}%)`;
  }

  function wantedPieces(board) {
    const wanted = new Map();
    board.forEach((rank, row) =>
      rank.forEach((piece, file) => {
        if (piece) wanted.set(`${FILES[file]}${8 - row}`, piece.color + piece.type);
      }),
    );
    return wanted;
  }

  /** Removes and returns the closest unused piece element showing `code`. */
  function takeClosest(spare, code, square) {
    let best = null;
    for (const [from, entry] of spare) {
      if (entry.code !== code) continue;
      if (!best || cellDistance(from, square, orientation) < cellDistance(best, square, orientation)) best = from;
    }
    if (!best) return null;
    const entry = spare.get(best);
    spare.delete(best);
    return entry;
  }

  /**
   * Makes the piece layer match the board while reusing elements, so that a piece which
   * changed square slides there: unchanged pieces stay, a moved piece is the closest spare
   * element of the same kind, a promoted pawn turns into its new piece, the rest fades out.
   */
  function syncPieces(board, lastMove, instant) {
    const wanted = wantedPieces(board);
    const spare = new Map(pieces);
    const next = new Map();

    for (const [square, code] of wanted) {
      const current = spare.get(square);
      if (current && current.code === code) {
        next.set(square, current);
        spare.delete(square);
      }
    }
    for (const [square, code] of wanted) {
      if (next.has(square)) continue;
      let entry = takeClosest(spare, code, square);
      if (!entry && lastMove && lastMove.to === square && spare.has(lastMove.from)) {
        entry = spare.get(lastMove.from);
        spare.delete(lastMove.from);
        entry = { node: entry.node, code };
        setPieceCode(entry.node, code);
      }
      if (!entry) {
        entry = { node: createPiece(code), code };
        placeWithoutAnimation(entry.node, square);
        piecesLayer.append(entry.node);
      }
      next.set(square, entry);
    }

    for (const entry of spare.values()) removePiece(entry.node, instant);
    pieces = next;
    if (instant) piecesLayer.classList.add('no-anim');
    for (const [square, entry] of pieces) placePiece(entry.node, square);
    if (instant) {
      void piecesLayer.offsetWidth; // apply the positions before transitions are switched back on
      piecesLayer.classList.remove('no-anim');
    }
  }

  function placeWithoutAnimation(node, square) {
    node.classList.add('no-anim');
    placePiece(node, square);
    requestAnimationFrame(() => node.classList.remove('no-anim'));
  }

  function removePiece(node, instant) {
    if (instant) {
      node.remove();
      return;
    }
    node.classList.remove('dragging');
    node.classList.add('leaving');
    setTimeout(() => node.remove(), LEAVE_MS);
  }

  // -- arrows -----------------------------------------------------------------

  function arrowShape({ from, to }) {
    const start = cellOf(from, orientation);
    const end = cellOf(to, orientation);
    const x1 = start.col + 0.5;
    const y1 = start.row + 0.5;
    const x2 = end.col + 0.5;
    const y2 = end.row + 0.5;
    const length = Math.hypot(x2 - x1, y2 - y1) || 1;
    const ux = (x2 - x1) / length;
    const uy = (y2 - y1) / length;
    const head = 0.42;
    const baseX = x2 - ux * head;
    const baseY = y2 - uy * head;
    const half = 0.24;

    const group = document.createElementNS(SVG_NS, 'g');
    group.setAttribute('class', 'arrow');
    const shaft = document.createElementNS(SVG_NS, 'line');
    shaft.setAttribute('x1', x1 + ux * 0.2);
    shaft.setAttribute('y1', y1 + uy * 0.2);
    shaft.setAttribute('x2', baseX);
    shaft.setAttribute('y2', baseY);
    const tip = document.createElementNS(SVG_NS, 'polygon');
    tip.setAttribute(
      'points',
      `${x2},${y2} ${baseX - uy * half},${baseY + ux * half} ${baseX + uy * half},${baseY - ux * half}`,
    );
    group.append(shaft, tip);
    return group;
  }

  function drawArrows(arrows) {
    arrowsLayer.replaceChildren(...arrows.map(arrowShape));
  }

  // -- promotion picker ---------------------------------------------------------

  /**
   * Shows the four promotion pieces on the file of `square`, starting at the promotion rank.
   * @returns {Promise<'q'|'r'|'b'|'n'|null>} null when the picker was dismissed
   */
  function askPromotion(square, color) {
    closePromotion?.(null);
    const { col, row } = cellOf(square, orientation);
    const direction = row === 0 ? 1 : -1;
    const choices = PROMOTION_CHOICES.map((type, index) => {
      const button = element('button', 'promotion-choice');
      button.type = 'button';
      button.dataset.promotion = type;
      button.setAttribute('aria-label', `Promote to ${pieceName(color, type).split(' ')[1]}`);
      button.style.transform = `translate(${col * 100}%, ${(row + direction * index) * 100}%)`;
      button.innerHTML = pieceSvg(color, type);
      return button;
    });
    promotionLayer.replaceChildren(...choices);
    promotionLayer.hidden = false;
    choices[0].focus({ preventScroll: true });

    return new Promise((resolve) => {
      const onClick = (event) => {
        const choice = event.target.closest('.promotion-choice');
        closePromotion(choice ? choice.dataset.promotion : null);
      };
      const onKey = (event) => {
        if (event.key === 'Escape') closePromotion(null);
      };
      closePromotion = (result) => {
        closePromotion = null;
        promotionLayer.hidden = true;
        promotionLayer.replaceChildren();
        promotionLayer.removeEventListener('click', onClick);
        document.removeEventListener('keydown', onKey);
        resolve(result);
      };
      promotionLayer.addEventListener('click', onClick);
      document.addEventListener('keydown', onKey);
    });
  }

  // -- public -----------------------------------------------------------------

  return {
    element: root,

    /** @param {BoardModel} model */
    render(model) {
      const flipped = model.orientation !== orientation;
      if (flipped) buildSquares(model.orientation);
      markSquares(model);
      syncPieces(model.board, model.lastMove, flipped);
      drawArrows(model.arrows);
      root.classList.toggle('is-locked', !model.interactive);
    },

    /** Square under a viewport point, or null outside the board. */
    squareAt(clientX, clientY) {
      const rect = root.getBoundingClientRect();
      const col = Math.floor(((clientX - rect.left) / rect.width) * 8);
      const row = Math.floor(((clientY - rect.top) / rect.height) * 8);
      if (col < 0 || col > 7 || row < 0 || row > 7 || orientation === null) return null;
      return squareAtCell(col, row, orientation);
    },

    pieceNode(square) {
      return pieces.get(square)?.node ?? null;
    },

    /** Moves a picked-up piece so that its centre sits at the pointer (raised by `lift` px). */
    dragPiece(node, clientX, clientY, lift = 0) {
      const rect = root.getBoundingClientRect();
      const size = rect.width / 8;
      node.classList.add('dragging');
      node.style.transform = `translate(${clientX - rect.left - size / 2}px, ${clientY - rect.top - size / 2 - lift}px)`;
    },

    /** Ends a drag. With `square` the piece lands there at once; without, it glides home. */
    dropPiece(node, square = null) {
      node.classList.remove('dragging');
      if (square) {
        placeWithoutAnimation(node, square);
        return;
      }
      for (const [home, entry] of pieces) {
        if (entry.node === node) placePiece(node, home);
      }
    },

    setHover(square) {
      if (square === hoverSquare) return;
      squareNodes.get(hoverSquare)?.classList.remove('hover');
      hoverSquare = square;
      squareNodes.get(hoverSquare)?.classList.add('hover');
    },

    askPromotion,

    cancelPromotion() {
      closePromotion?.(null);
    },
  };
}
