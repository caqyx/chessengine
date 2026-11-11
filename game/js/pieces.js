/**
 * pieces.js - the piece set, drawn for this game as inline SVG.
 *
 * Every piece lives in a 100 x 100 box and is built from the same three ideas: a flat plinth,
 * a solid body and one unmistakable top (ball, ear, mitre, battlement, coronet, cross).
 * Colours come from CSS custom properties (see board.css):
 *   --piece-fill    body colour
 *   --piece-line    outline
 *   --piece-detail  the few inner lines (eye, slit, bands)
 */

const PLINTH = '<rect class="s" x="22" y="76" width="56" height="13" rx="3"/>';
const NARROW_PLINTH = '<rect class="s" x="26" y="76" width="48" height="13" rx="3"/>';

const SHAPES = {
  p:
    '<path class="s" d="M37 78C39 65 46 58 45 47H55C54 58 61 65 63 78Z"/>' +
    '<circle class="s" cx="50" cy="35" r="14"/>' +
    NARROW_PLINTH,
  n:
    '<path class="s" d="M31 78C31 63 41 57 45 46C39 50 33 54 27 52L20 44C24 34 32 27 40 22L39 9L51 19C68 22 77 40 75 78Z"/>' +
    '<circle class="e" cx="39" cy="33" r="3"/>' +
    '<path class="d" d="M58 30C66 40 67 54 65 68"/>' +
    PLINTH,
  b:
    '<path class="s" d="M41 64H59L65 78H35Z"/>' +
    '<path class="s" d="M50 17C63 29 69 39 69 47C69 58 60 62 50 62C40 62 31 58 31 47C31 39 37 29 50 17Z"/>' +
    '<circle class="s" cx="50" cy="13" r="5.5"/>' +
    '<path class="d" d="M57 31L46 46"/>' +
    '<rect class="s" x="35" y="60" width="30" height="8" rx="3"/>' +
    NARROW_PLINTH,
  r:
    '<path class="s" d="M34 38H66L69 78H31Z"/>' +
    '<path class="s" d="M27 15H39V23H44V15H56V23H61V15H73V38H27Z"/>' +
    '<path class="d" d="M36 68H64"/>' +
    PLINTH,
  q:
    '<path class="s" d="M21 36L33 58L36 27L44 56L50 23L56 56L64 27L67 58L79 36L70 78H30Z"/>' +
    '<circle class="s" cx="21" cy="32" r="5"/>' +
    '<circle class="s" cx="36" cy="23" r="5"/>' +
    '<circle class="s" cx="50" cy="19" r="5"/>' +
    '<circle class="s" cx="64" cy="23" r="5"/>' +
    '<circle class="s" cx="79" cy="32" r="5"/>' +
    '<path class="d" d="M32 68H68"/>' +
    PLINTH,
  k:
    '<path class="s" d="M46 5H54V12H61V20H54V32H46V20H39V12H46Z"/>' +
    '<path class="s" d="M25 44C25 31 41 29 50 37C59 29 75 31 75 44L68 78H32Z"/>' +
    '<path class="d" d="M50 39V56"/>' +
    '<path class="d" d="M31 67H69"/>' +
    PLINTH,
};

const PIECE_NAMES = { p: 'pawn', n: 'knight', b: 'bishop', r: 'rook', q: 'queen', k: 'king' };
const COLOR_NAMES = { w: 'white', b: 'black' };

/** "white knight" etc., for labels. */
export function pieceName(color, type) {
  return `${COLOR_NAMES[color]} ${PIECE_NAMES[type]}`;
}

/**
 * SVG markup of one piece.
 * @param {'w'|'b'} color
 * @param {'p'|'n'|'b'|'r'|'q'|'k'} type
 * @returns {string}
 */
export function pieceSvg(color, type) {
  return `<svg class="piece-svg piece-${color}" viewBox="0 0 100 100" aria-hidden="true" focusable="false">${SHAPES[type]}</svg>`;
}
