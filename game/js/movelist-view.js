/**
 * movelist-view.js - the move list in SAN with move numbers.
 *
 * One <li> per full move holding the number and up to two move buttons. CSS lays the rows out
 * as a table on wide screens and as a single horizontally scrolling strip on phones.
 */

function element(tag, className, text) {
  const node = document.createElement(tag);
  if (className) node.className = className;
  if (text !== undefined) node.textContent = text;
  return node;
}

function moveButton(san, ply) {
  const button = element('button', 'move', san);
  button.type = 'button';
  button.dataset.ply = String(ply);
  return button;
}

/** Groups SAN moves into rows of { number, white, black } where a side is { san, ply } or null. */
function buildRows(sans, firstNumber, blackStarts) {
  const rows = [];
  let number = firstNumber;
  sans.forEach((san, index) => {
    const whiteMoved = (index % 2 === 0) !== blackStarts;
    const entry = { san, ply: index + 1 };
    if (whiteMoved) {
      rows.push({ number, white: entry, black: null });
    } else {
      if (rows.length === 0) rows.push({ number, white: null, black: null });
      rows[rows.length - 1].black = entry;
      number++;
    }
  });
  return rows;
}

function rowNode(row) {
  const item = element('li', 'move-row');
  item.append(element('span', 'move-number', `${row.number}.`));
  item.append(row.white ? moveButton(row.white.san, row.white.ply) : element('span', 'move move-gap', '...'));
  if (row.black) item.append(moveButton(row.black.san, row.black.ply));
  return item;
}

/** Scrolls `container` just enough to show `node` (never scrolls the page). */
function reveal(container, node) {
  const margin = 8;
  const box = container.getBoundingClientRect();
  const item = node.getBoundingClientRect();
  if (item.top < box.top) container.scrollTop -= box.top - item.top + margin;
  else if (item.bottom > box.bottom) container.scrollTop += item.bottom - box.bottom + margin;
  if (item.left < box.left) container.scrollLeft -= box.left - item.left + margin;
  else if (item.right > box.right) container.scrollLeft += item.right - box.right + margin;
}

/**
 * @param {HTMLOListElement} root
 * @param {{onSelect: (ply: number) => void}} handlers
 */
export function createMoveListView(root, { onSelect }) {
  let builtFor = null;

  root.addEventListener('click', (event) => {
    const button = event.target.closest('button.move');
    if (button) onSelect(Number(button.dataset.ply));
  });

  function rebuild(model) {
    const rows = buildRows(model.sans, model.firstNumber, model.blackStarts);
    if (rows.length === 0) {
      root.replaceChildren(element('li', 'moves-empty', 'Moves will appear here.'));
    } else {
      root.replaceChildren(...rows.map(rowNode));
    }
  }

  function markCurrent(ply) {
    root.querySelector('[aria-current]')?.removeAttribute('aria-current');
    const current = root.querySelector(`button.move[data-ply="${ply}"]`);
    if (current) {
      current.setAttribute('aria-current', 'true');
      reveal(root, current);
    } else {
      root.scrollTop = 0;
      root.scrollLeft = 0;
    }
  }

  return {
    /**
     * @param {{sans: string[], firstNumber: number, blackStarts: boolean, currentPly: number}} model
     */
    render(model) {
      const signature = `${model.firstNumber}|${model.blackStarts}|${model.sans.join(' ')}`;
      if (signature !== builtFor) {
        rebuild(model);
        builtFor = signature;
      }
      markCurrent(model.currentPly);
    },
  };
}
