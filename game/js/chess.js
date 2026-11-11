/**
 * chess.js - dependency-free chess rules core (ES module).
 *
 * Works in browsers (`<script type="module">`) and in Node (`import`).
 *
 * Layout of this file:
 *   1. constants and square helpers
 *   2. FEN parsing / writing
 *   3. the fast core: attack detection, pseudo-legal generation, make / unmake, perft
 *      (moves are packed integers here; no strings are built)
 *   4. decoration: turning packed moves into public move objects (SAN, UCI, FEN before/after)
 *   5. move input parsing (SAN / UCI / object)
 *   6. PGN writing and parsing
 *   7. the public `Chess` class, a thin wrapper around one internal state object
 *
 * Board: 0x88 mailbox, index = row * 16 + file, row 0 is rank 8, so a8 = 0 and h1 = 119.
 * A square index is on the board when `(index & 0x88) === 0`.
 */

/** FEN of the standard starting position. */
export const START_FEN = 'rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1';

// ---------------------------------------------------------------------------
// 1. Constants and square helpers
// ---------------------------------------------------------------------------

const WHITE = 0;
const BLACK = 1;

const PAWN = 1;
const KNIGHT = 2;
const BISHOP = 3;
const ROOK = 4;
const QUEEN = 5;
const KING = 6;

/** A piece code is `type | color << 3`; 0 means empty. */
const TYPE_CHARS = ' pnbrqk';
const COLOR_CHARS = 'wb';
const PIECE_CHARS = ' PNBRQK  pnbrqk';
const FILE_CHARS = 'abcdefgh';

/** Packed move layout: from (7 bits) | to << 7 | promotion type << 14 | kind << 17. */
const KIND_NORMAL = 0;
const KIND_DOUBLE_PUSH = 1;
const KIND_EN_PASSANT = 2;
const KIND_CASTLE_KINGSIDE = 3;
const KIND_CASTLE_QUEENSIDE = 4;

const CASTLE_WHITE_KING = 1;
const CASTLE_WHITE_QUEEN = 2;
const CASTLE_BLACK_KING = 4;
const CASTLE_BLACK_QUEEN = 8;

const A8 = 0;
const E8 = 4;
const H8 = 7;
const A1 = 112;
const E1 = 116;
const H1 = 119;

const KNIGHT_OFFSETS = [-33, -31, -18, -14, 14, 18, 31, 33];
const KING_OFFSETS = [-17, -16, -15, -1, 1, 15, 16, 17];
const ROOK_DIRECTIONS = [-16, -1, 1, 16];
const BISHOP_DIRECTIONS = [-17, -15, 15, 17];
const PROMOTION_TYPES = [QUEEN, ROOK, BISHOP, KNIGHT];

/** Castling rights that survive a move touching a square (as origin or destination). */
const CASTLE_KEEP = buildCastleKeepTable();

const SEVEN_TAG_ROSTER = ['Event', 'Site', 'Date', 'Round', 'White', 'Black', 'Result'];
const RESULT_TOKENS = ['1-0', '0-1', '1/2-1/2', '*'];

function buildCastleKeepTable() {
  const table = new Uint8Array(128).fill(15);
  table[A1] = 15 & ~CASTLE_WHITE_QUEEN;
  table[H1] = 15 & ~CASTLE_WHITE_KING;
  table[E1] = 15 & ~(CASTLE_WHITE_KING | CASTLE_WHITE_QUEEN);
  table[A8] = 15 & ~CASTLE_BLACK_QUEEN;
  table[H8] = 15 & ~CASTLE_BLACK_KING;
  table[E8] = 15 & ~(CASTLE_BLACK_KING | CASTLE_BLACK_QUEEN);
  return table;
}

/** 'e4' -> 0x88 index, or -1 when the text is not a square name. */
function squareIndex(name) {
  if (typeof name !== 'string' || !/^[a-h][1-8]$/.test(name)) return -1;
  return (56 - name.charCodeAt(1)) * 16 + (name.charCodeAt(0) - 97);
}

function squareName(sq) {
  return FILE_CHARS[sq & 7] + (8 - (sq >> 4));
}

function publicPiece(piece) {
  return piece ? { type: TYPE_CHARS[piece & 7], color: COLOR_CHARS[piece >> 3] } : null;
}

function packMove(from, to, promotion, kind) {
  return from | (to << 7) | (promotion << 14) | (kind << 17);
}

const moveFrom = (code) => code & 127;
const moveTo = (code) => (code >> 7) & 127;
const movePromotion = (code) => (code >> 14) & 7;
const moveKind = (code) => code >> 17;

// ---------------------------------------------------------------------------
// 2. FEN
// ---------------------------------------------------------------------------

function fenError(reason) {
  return new Error(`Invalid FEN: ${reason}`);
}

function parsePlacement(field) {
  const board = new Int8Array(128);
  const kings = [-1, -1];
  const rows = field.split('/');
  if (rows.length !== 8) throw fenError('piece placement must have 8 ranks');
  rows.forEach((text, row) => parsePlacementRow(text, row, board, kings));
  if (kings[WHITE] < 0 || kings[BLACK] < 0) throw fenError('each side needs exactly one king');
  return { board, kings };
}

function parsePlacementRow(text, row, board, kings) {
  let file = 0;
  for (const ch of text) {
    if (ch >= '1' && ch <= '8') {
      file += Number(ch);
      continue;
    }
    const piece = PIECE_CHARS.indexOf(ch); // 0 or -1 for anything that is not a piece letter
    if (piece <= 0) throw fenError(`unexpected character "${ch}" in piece placement`);
    if (file > 7) throw fenError(`rank ${8 - row} has more than 8 squares`);
    placeParsedPiece(piece, row * 16 + file, board, kings);
    file++;
  }
  if (file !== 8) throw fenError(`rank ${8 - row} does not describe 8 squares`);
}

function placeParsedPiece(piece, sq, board, kings) {
  const type = piece & 7;
  const row = sq >> 4;
  if (type === PAWN && (row === 0 || row === 7)) throw fenError('pawn on the first or last rank');
  if (type === KING) {
    if (kings[piece >> 3] >= 0) throw fenError('each side needs exactly one king');
    kings[piece >> 3] = sq;
  }
  board[sq] = piece;
}

function parseCastling(field, board) {
  if (!/^(-|[KQkq]{1,4})$/.test(field)) throw fenError('bad castling field');
  let rights = 0;
  if (field.includes('K')) rights |= CASTLE_WHITE_KING;
  if (field.includes('Q')) rights |= CASTLE_WHITE_QUEEN;
  if (field.includes('k')) rights |= CASTLE_BLACK_KING;
  if (field.includes('q')) rights |= CASTLE_BLACK_QUEEN;
  return dropImpossibleCastling(rights, board);
}

/** Rights are only kept while king and rook still stand on their home squares. */
function dropImpossibleCastling(rights, board) {
  const whiteRook = ROOK;
  const blackRook = ROOK | (BLACK << 3);
  if (board[E1] !== KING) rights &= ~(CASTLE_WHITE_KING | CASTLE_WHITE_QUEEN);
  if (board[H1] !== whiteRook) rights &= ~CASTLE_WHITE_KING;
  if (board[A1] !== whiteRook) rights &= ~CASTLE_WHITE_QUEEN;
  if (board[E8] !== (KING | (BLACK << 3))) rights &= ~(CASTLE_BLACK_KING | CASTLE_BLACK_QUEEN);
  if (board[H8] !== blackRook) rights &= ~CASTLE_BLACK_KING;
  if (board[A8] !== blackRook) rights &= ~CASTLE_BLACK_QUEEN;
  return rights;
}

function parseEnPassant(field, board, turn) {
  if (field === '-') return -1;
  const sq = squareIndex(field);
  if (sq < 0) throw fenError('bad en passant field');
  // The pawn that just made a double push belongs to the side NOT to move.
  const pushDirection = turn === WHITE ? 16 : -16;
  const expectedRow = turn === WHITE ? 2 : 5;
  const pushedPawn = PAWN | ((turn ^ 1) << 3);
  const consistent =
    sq >> 4 === expectedRow &&
    board[sq] === 0 &&
    board[sq - pushDirection] === 0 &&
    board[sq + pushDirection] === pushedPawn;
  if (!consistent) throw fenError('en passant square does not match the position');
  return sq;
}

function parseCounter(field, name, fallback) {
  if (field === undefined) return fallback;
  if (!/^\d{1,6}$/.test(field)) throw fenError(`bad ${name}`);
  return Number(field);
}

/**
 * Builds a fresh internal state from a FEN string. Throws on invalid input.
 * The two move counters may be omitted (they default to "0 1").
 */
function createState(fen) {
  if (typeof fen !== 'string') throw fenError('not a string');
  const fields = fen.trim().split(/\s+/);
  if (fields.length < 4 || fields.length > 6) throw fenError('expected 4 to 6 space separated fields');
  const { board, kings } = parsePlacement(fields[0]);
  if (fields[1] !== 'w' && fields[1] !== 'b') throw fenError('side to move must be "w" or "b"');
  const turn = fields[1] === 'w' ? WHITE : BLACK;

  const state = {
    board,
    kings,
    turn,
    castling: parseCastling(fields[2], board),
    ep: parseEnPassant(fields[3], board, turn),
    halfmove: parseCounter(fields[4], 'halfmove clock', 0),
    fullmove: Math.max(1, parseCounter(fields[5], 'fullmove number', 1)),
    undoStack: [], // two numbers per made move, see makeMove
    history: [], // { move, code } per move played through the public API
    positionKeys: [], // repetition key of the start position and of every position after it
    positionCounts: new Map(), // repetition key -> occurrences in the current game
    headers: {}, // PGN tags remembered from loadPgn
    startFen: '',
    legalCache: null, // { codes, moves } for the current position
  };
  if (isAttacked(state, kings[turn ^ 1], turn)) throw fenError('the side not to move is in check');
  state.startFen = toFen(state);
  recordPosition(state);
  return state;
}

function placementString(board) {
  let out = '';
  for (let row = 0; row < 8; row++) {
    let empty = 0;
    for (let file = 0; file < 8; file++) {
      const piece = board[row * 16 + file];
      if (piece === 0) {
        empty++;
        continue;
      }
      if (empty) out += empty;
      empty = 0;
      out += PIECE_CHARS[piece];
    }
    if (empty) out += empty;
    if (row < 7) out += '/';
  }
  return out;
}

function castlingString(rights) {
  if (rights === 0) return '-';
  return (
    (rights & CASTLE_WHITE_KING ? 'K' : '') +
    (rights & CASTLE_WHITE_QUEEN ? 'Q' : '') +
    (rights & CASTLE_BLACK_KING ? 'k' : '') +
    (rights & CASTLE_BLACK_QUEEN ? 'q' : '')
  );
}

function toFen(s) {
  const ep = s.ep < 0 ? '-' : squareName(s.ep);
  return `${placementString(s.board)} ${COLOR_CHARS[s.turn]} ${castlingString(s.castling)} ${ep} ${s.halfmove} ${s.fullmove}`;
}

// ---------------------------------------------------------------------------
// 3. Fast core: attacks, generation, make / unmake, perft
// ---------------------------------------------------------------------------

/** True when any piece of colour `by` attacks square `sq`. */
function isAttacked(s, sq, by) {
  const b = s.board;
  const tint = by << 3;

  // A pawn attacks the squares diagonally ahead of it, so look one row behind `sq`.
  const behind = by === WHITE ? 16 : -16;
  const pawn = PAWN | tint;
  let t = sq + behind - 1;
  if (!(t & 0x88) && b[t] === pawn) return true;
  t = sq + behind + 1;
  if (!(t & 0x88) && b[t] === pawn) return true;

  const knight = KNIGHT | tint;
  const king = KING | tint;
  for (let i = 0; i < 8; i++) {
    t = sq + KNIGHT_OFFSETS[i];
    if (!(t & 0x88) && b[t] === knight) return true;
    t = sq + KING_OFFSETS[i];
    if (!(t & 0x88) && b[t] === king) return true;
  }

  const queen = QUEEN | tint;
  return (
    isAttackedAlong(b, sq, ROOK_DIRECTIONS, ROOK | tint, queen) ||
    isAttackedAlong(b, sq, BISHOP_DIRECTIONS, BISHOP | tint, queen)
  );
}

function isAttackedAlong(b, sq, directions, slider, queen) {
  for (let i = 0; i < 4; i++) {
    const step = directions[i];
    let t = sq + step;
    while (!(t & 0x88)) {
      const piece = b[t];
      if (piece !== 0) {
        if (piece === slider || piece === queen) return true;
        break;
      }
      t += step;
    }
  }
  return false;
}

function isKingAttacked(s, color) {
  return isAttacked(s, s.kings[color], color ^ 1);
}

/**
 * Writes all pseudo-legal moves (legal except that they may leave the own king in check)
 * into `out` starting at index 0 and returns how many were written.
 */
function generatePseudoLegal(s, out) {
  const b = s.board;
  const us = s.turn;
  let n = 0;
  for (let sq = 0; sq < 120; sq++) {
    if (sq & 0x88) {
      sq += 7;
      continue;
    }
    const piece = b[sq];
    if (piece === 0 || piece >> 3 !== us) continue;
    switch (piece & 7) {
      case PAWN:
        n = addPawnMoves(s, sq, out, n);
        break;
      case KNIGHT:
        n = addStepMoves(s, sq, KNIGHT_OFFSETS, out, n);
        break;
      case BISHOP:
        n = addSlideMoves(s, sq, BISHOP_DIRECTIONS, out, n);
        break;
      case ROOK:
        n = addSlideMoves(s, sq, ROOK_DIRECTIONS, out, n);
        break;
      case QUEEN:
        n = addSlideMoves(s, sq, BISHOP_DIRECTIONS, out, n);
        n = addSlideMoves(s, sq, ROOK_DIRECTIONS, out, n);
        break;
      default:
        n = addStepMoves(s, sq, KING_OFFSETS, out, n);
        n = addCastlingMoves(s, sq, out, n);
    }
  }
  return n;
}

function addPawnMoves(s, from, out, n) {
  const b = s.board;
  const us = s.turn;
  const forward = us === WHITE ? -16 : 16;
  const row = from >> 4;
  const promotes = row === (us === WHITE ? 1 : 6);

  let to = from + forward;
  if (b[to] === 0) {
    if (promotes) {
      n = addPromotions(from, to, out, n);
    } else {
      out[n++] = from | (to << 7);
      const onStartRow = row === (us === WHITE ? 6 : 1);
      if (onStartRow && b[to + forward] === 0) {
        out[n++] = packMove(from, to + forward, 0, KIND_DOUBLE_PUSH);
      }
    }
  }

  for (let side = -1; side <= 1; side += 2) {
    to = from + forward + side;
    if (to & 0x88) continue;
    const target = b[to];
    if (target !== 0) {
      if (target >> 3 === us) continue;
      if (promotes) n = addPromotions(from, to, out, n);
      else out[n++] = from | (to << 7);
    } else if (to === s.ep) {
      out[n++] = packMove(from, to, 0, KIND_EN_PASSANT);
    }
  }
  return n;
}

function addPromotions(from, to, out, n) {
  for (let i = 0; i < 4; i++) out[n++] = packMove(from, to, PROMOTION_TYPES[i], KIND_NORMAL);
  return n;
}

function addStepMoves(s, from, offsets, out, n) {
  const b = s.board;
  const us = s.turn;
  for (let i = 0; i < 8; i++) {
    const to = from + offsets[i];
    if (to & 0x88) continue;
    const target = b[to];
    if (target === 0 || target >> 3 !== us) out[n++] = from | (to << 7);
  }
  return n;
}

function addSlideMoves(s, from, directions, out, n) {
  const b = s.board;
  const us = s.turn;
  for (let i = 0; i < 4; i++) {
    const step = directions[i];
    let to = from + step;
    while (!(to & 0x88)) {
      const target = b[to];
      if (target === 0) {
        out[n++] = from | (to << 7);
      } else {
        if (target >> 3 !== us) out[n++] = from | (to << 7);
        break;
      }
      to += step;
    }
  }
  return n;
}

/**
 * Castling rights guarantee that king and rook are on their home squares (see
 * dropImpossibleCastling and CASTLE_KEEP), so only emptiness and attacks are checked here.
 * The king's destination square is verified by the normal legality filter.
 */
function addCastlingMoves(s, from, out, n) {
  const us = s.turn;
  const rights = us === WHITE ? s.castling & 3 : s.castling >> 2;
  if (rights === 0) return n;
  const b = s.board;
  const them = us ^ 1;
  if (isAttacked(s, from, them)) return n;
  if (rights & 1 && b[from + 1] === 0 && b[from + 2] === 0 && !isAttacked(s, from + 1, them)) {
    out[n++] = packMove(from, from + 2, 0, KIND_CASTLE_KINGSIDE);
  }
  if (
    rights & 2 &&
    b[from - 1] === 0 &&
    b[from - 2] === 0 &&
    b[from - 3] === 0 &&
    !isAttacked(s, from - 1, them)
  ) {
    out[n++] = packMove(from, from - 2, 0, KIND_CASTLE_QUEENSIDE);
  }
  return n;
}

/**
 * Plays a packed move on the board. Everything needed to take it back is pushed on
 * `s.undoStack` as two numbers: (captured | castling << 4 | (ep + 1) << 8) and the halfmove clock.
 */
function makeMove(s, code) {
  const b = s.board;
  const from = code & 127;
  const to = (code >> 7) & 127;
  const kind = code >> 17;
  const us = s.turn;
  const piece = b[from];
  const captured = b[to];

  s.undoStack.push(captured | (s.castling << 4) | ((s.ep + 1) << 8), s.halfmove);
  b[to] = piece;
  b[from] = 0;
  s.ep = -1;
  s.halfmove = captured !== 0 ? 0 : s.halfmove + 1;

  const type = piece & 7;
  if (type === PAWN) {
    s.halfmove = 0;
    if (kind === KIND_DOUBLE_PUSH) s.ep = (from + to) >> 1;
    else if (kind === KIND_EN_PASSANT) b[to + (us === WHITE ? 16 : -16)] = 0;
    else if (code & 0x1c000) b[to] = ((code >> 14) & 7) | (us << 3);
  } else if (type === KING) {
    s.kings[us] = to;
    if (kind === KIND_CASTLE_KINGSIDE) {
      b[to - 1] = b[to + 1];
      b[to + 1] = 0;
    } else if (kind === KIND_CASTLE_QUEENSIDE) {
      b[to + 1] = b[to - 2];
      b[to - 2] = 0;
    }
  }

  s.castling &= CASTLE_KEEP[from] & CASTLE_KEEP[to];
  s.fullmove += us;
  s.turn = us ^ 1;
}

/** Exact inverse of makeMove; `code` must be the move made last. */
function unmakeMove(s, code) {
  const b = s.board;
  const from = code & 127;
  const to = (code >> 7) & 127;
  const kind = code >> 17;
  const us = s.turn ^ 1;

  s.turn = us;
  s.fullmove -= us;
  s.halfmove = s.undoStack.pop();
  const saved = s.undoStack.pop();
  s.castling = (saved >> 4) & 15;
  s.ep = (saved >> 8) - 1;

  const piece = code & 0x1c000 ? PAWN | (us << 3) : b[to];
  b[from] = piece;
  b[to] = saved & 15;

  if (kind === KIND_EN_PASSANT) {
    b[to + (us === WHITE ? 16 : -16)] = PAWN | ((us ^ 1) << 3);
  } else if (kind === KIND_CASTLE_KINGSIDE) {
    b[to + 1] = b[to - 1];
    b[to - 1] = 0;
  } else if (kind === KIND_CASTLE_QUEENSIDE) {
    b[to - 2] = b[to + 1];
    b[to + 1] = 0;
  }
  if ((piece & 7) === KING) s.kings[us] = from;
}

/** Makes the move, reports whether it left the mover's king safe, and takes it back. */
function isLegal(s, code) {
  const us = s.turn;
  makeMove(s, code);
  const safe = !isKingAttacked(s, us);
  unmakeMove(s, code);
  return safe;
}

/** All legal moves of the side to move, as a new array of packed moves. */
function generateLegal(s) {
  const pseudo = [];
  const count = generatePseudoLegal(s, pseudo);
  const legal = [];
  for (let i = 0; i < count; i++) {
    if (isLegal(s, pseudo[i])) legal.push(pseudo[i]);
  }
  return legal;
}

function hasLegalMove(s) {
  const pseudo = [];
  const count = generatePseudoLegal(s, pseudo);
  for (let i = 0; i < count; i++) {
    if (isLegal(s, pseudo[i])) return true;
  }
  return false;
}

/**
 * Counts leaf nodes of the legal move tree using make / unmake only.
 * `lists` holds one reusable move buffer per depth so the search allocates nothing.
 */
function perftNodes(s, depth, lists) {
  if (depth <= 0) return 1;
  const list = lists[depth] || (lists[depth] = []);
  const count = generatePseudoLegal(s, list);
  const us = s.turn;
  let nodes = 0;
  for (let i = 0; i < count; i++) {
    const code = list[i];
    makeMove(s, code);
    if (!isKingAttacked(s, us)) nodes += depth === 1 ? 1 : perftNodes(s, depth - 1, lists);
    unmakeMove(s, code);
  }
  return nodes;
}

// ---------------------------------------------------------------------------
// 4. Position facts and move decoration
// ---------------------------------------------------------------------------

/** Legal moves of the current position, cached until the position changes. */
function legalCodes(s) {
  if (!s.legalCache) s.legalCache = { codes: generateLegal(s), moves: null };
  return s.legalCache.codes;
}

/** Public move objects for the current position (same order as legalCodes), cached. */
function legalMoves(s) {
  const codes = legalCodes(s);
  const cache = s.legalCache;
  if (!cache.moves) {
    const before = toFen(s);
    cache.moves = codes.map((code) => decorateMove(s, code, codes, before));
  }
  return cache.moves;
}

function invalidateCache(s) {
  s.legalCache = null;
}

/** True when the side to move has a legal en passant capture. */
function hasLegalEnPassant(s) {
  if (s.ep < 0) return false;
  const pawn = PAWN | (s.turn << 3);
  const behind = s.turn === WHITE ? 16 : -16;
  for (let side = -1; side <= 1; side += 2) {
    const from = s.ep + behind + side;
    if (from & 0x88 || s.board[from] !== pawn) continue;
    if (isLegal(s, packMove(from, s.ep, 0, KIND_EN_PASSANT))) return true;
  }
  return false;
}

/**
 * Key used for repetition: placement, side to move, castling rights, and the en passant
 * square only when an en passant capture is really possible (FIDE 9.2).
 */
function positionKey(s) {
  const ep = hasLegalEnPassant(s) ? squareName(s.ep) : '-';
  return `${placementString(s.board)} ${COLOR_CHARS[s.turn]} ${castlingString(s.castling)} ${ep}`;
}

function recordPosition(s) {
  const key = positionKey(s);
  s.positionKeys.push(key);
  s.positionCounts.set(key, (s.positionCounts.get(key) || 0) + 1);
}

function forgetPosition(s) {
  const key = s.positionKeys.pop();
  const count = s.positionCounts.get(key) - 1;
  if (count > 0) s.positionCounts.set(key, count);
  else s.positionCounts.delete(key);
}

function repetitionCount(s) {
  return s.positionCounts.get(s.positionKeys[s.positionKeys.length - 1]) || 0;
}

/**
 * No mate is possible: bare kings, a single minor piece, or only bishops that all
 * stand on squares of one colour.
 */
function hasInsufficientMaterial(s) {
  let minors = 0;
  let knights = 0;
  let bishopSquareColors = 0;
  for (let sq = 0; sq < 120; sq++) {
    if (sq & 0x88) {
      sq += 7;
      continue;
    }
    const type = s.board[sq] & 7;
    if (type === PAWN || type === ROOK || type === QUEEN) return false;
    if (type === KNIGHT) knights++;
    if (type === BISHOP) bishopSquareColors |= 1 << (((sq >> 4) + sq) & 1);
    if (type === KNIGHT || type === BISHOP) minors++;
  }
  return minors <= 1 || (knights === 0 && bishopSquareColors !== 3);
}

/** SAN without the check / mate suffix. `legal` is the legal move list of the position. */
function sanWithoutSuffix(s, code, legal) {
  const kind = moveKind(code);
  if (kind === KIND_CASTLE_KINGSIDE) return 'O-O';
  if (kind === KIND_CASTLE_QUEENSIDE) return 'O-O-O';

  const from = moveFrom(code);
  const to = moveTo(code);
  const type = s.board[from] & 7;
  const capture = s.board[to] !== 0 || kind === KIND_EN_PASSANT;
  const target = (capture ? 'x' : '') + squareName(to);

  if (type !== PAWN) return TYPE_CHARS[type].toUpperCase() + disambiguator(s, code, legal) + target;
  const promotion = movePromotion(code);
  return (
    (capture ? FILE_CHARS[from & 7] : '') + target + (promotion ? '=' + TYPE_CHARS[promotion].toUpperCase() : '')
  );
}

/** Origin hint needed when several pieces of one kind can reach the same square. */
function disambiguator(s, code, legal) {
  const from = moveFrom(code);
  const to = moveTo(code);
  const piece = s.board[from];
  let rivals = 0;
  let sameFile = 0;
  let sameRow = 0;
  for (const other of legal) {
    const otherFrom = moveFrom(other);
    if (otherFrom === from || moveTo(other) !== to || s.board[otherFrom] !== piece) continue;
    rivals++;
    if ((otherFrom & 7) === (from & 7)) sameFile++;
    if (otherFrom >> 4 === from >> 4) sameRow++;
  }
  if (rivals === 0) return '';
  if (sameFile === 0) return FILE_CHARS[from & 7];
  if (sameRow === 0) return String(8 - (from >> 4));
  return squareName(from);
}

function flagString(s, code) {
  const kind = moveKind(code);
  const capture = s.board[moveTo(code)] !== 0 || kind === KIND_EN_PASSANT;
  return (
    (kind === KIND_DOUBLE_PUSH ? 'b' : '') +
    (capture ? 'c' : '') +
    (kind === KIND_EN_PASSANT ? 'e' : '') +
    (kind === KIND_CASTLE_KINGSIDE ? 'k' : '') +
    (kind === KIND_CASTLE_QUEENSIDE ? 'q' : '') +
    (movePromotion(code) ? 'p' : '')
  );
}

/** Builds the public move object for a legal packed move of the current position. */
function decorateMove(s, code, legal, before) {
  const from = moveFrom(code);
  const to = moveTo(code);
  const promotion = movePromotion(code);
  const us = s.turn;
  const captured = moveKind(code) === KIND_EN_PASSANT ? PAWN : s.board[to] & 7;

  const move = {
    color: COLOR_CHARS[us],
    from: squareName(from),
    to: squareName(to),
    piece: TYPE_CHARS[s.board[from] & 7],
  };
  if (captured) move.captured = TYPE_CHARS[captured];
  if (promotion) move.promotion = TYPE_CHARS[promotion];
  move.san = sanWithoutSuffix(s, code, legal);
  move.uci = move.from + move.to + (promotion ? TYPE_CHARS[promotion] : '');
  move.flags = flagString(s, code);
  move.before = before;

  makeMove(s, code);
  move.after = toFen(s);
  if (isKingAttacked(s, us ^ 1)) move.san += hasLegalMove(s) ? '+' : '#';
  unmakeMove(s, code);
  return move;
}

// ---------------------------------------------------------------------------
// 5. Move input parsing
// ---------------------------------------------------------------------------

/** Index into the legal move list for any supported input, or -1. */
function findMoveIndex(s, input) {
  if (typeof input === 'string') {
    const text = input.trim();
    const uci = /^([a-h][1-8])([a-h][1-8])([qrbnQRBN])?$/.exec(text);
    if (uci) return findByCoordinates(s, uci[1], uci[2], uci[3], true);
    return findBySan(s, text);
  }
  if (input && typeof input === 'object') {
    return findByCoordinates(s, input.from, input.to, input.promotion, false);
  }
  return -1;
}

/**
 * A promotion move always needs its piece. `strictPromotion` additionally rejects a
 * promotion piece on a move that does not promote (used for UCI strings).
 */
function findByCoordinates(s, from, to, promotion, strictPromotion) {
  const wanted = typeof promotion === 'string' ? promotion.toLowerCase() : undefined;
  return legalMoves(s).findIndex((move) => {
    if (move.from !== from || move.to !== to) return false;
    if (move.promotion) return move.promotion === wanted;
    return !(strictPromotion && wanted);
  });
}

/** Removes everything a lenient SAN comparison ignores: check marks, glyphs, '=', 0-0 spelling. */
function normalizeSan(text) {
  const bare = text
    .replace(/\s*e\.?p\.?$/i, '')
    .replace(/[+#!?]+$/, '')
    .replace(/=/g, '');
  if (/^[0Oo]-[0Oo]-[0Oo]$/.test(bare)) return 'O-O-O';
  if (/^[0Oo]-[0Oo]$/.test(bare)) return 'O-O';
  return bare;
}

function findBySan(s, text) {
  const wanted = normalizeSan(text);
  if (!wanted) return -1;
  const moves = legalMoves(s);
  const exact = moves.findIndex((move) => normalizeSan(move.san) === wanted);
  return exact >= 0 ? exact : findByLooseSan(moves, wanted);
}

/**
 * Accepts SAN with more origin information than necessary (e.g. "Nbd7", "Rd1d4", "e2e4"),
 * as long as exactly one legal move fits the description.
 */
function findByLooseSan(moves, wanted) {
  const parts = /^([NBRQK])?([a-h])?([1-8])?[x:-]?([a-h][1-8])([NBRQnbrq])?$/.exec(wanted);
  if (!parts) return -1;
  const piece = (parts[1] || 'p').toLowerCase();
  const promotion = parts[5] ? parts[5].toLowerCase() : undefined;
  let found = -1;
  for (let i = 0; i < moves.length; i++) {
    const move = moves[i];
    const fits =
      move.piece === piece &&
      move.to === parts[4] &&
      move.promotion === promotion &&
      (!parts[2] || move.from[0] === parts[2]) &&
      (!parts[3] || move.from[1] === parts[3]);
    if (!fits) continue;
    if (found >= 0) return -1; // ambiguous
    found = i;
  }
  return found;
}

// ---------------------------------------------------------------------------
// 6. PGN
// ---------------------------------------------------------------------------

function escapeTagValue(value) {
  return String(value).replace(/\\/g, '\\\\').replace(/"/g, '\\"');
}

function orderedTagNames(tags) {
  const names = Object.keys(tags);
  return [
    ...SEVEN_TAG_ROSTER.filter((name) => names.includes(name)),
    ...names.filter((name) => !SEVEN_TAG_ROSTER.includes(name)),
  ];
}

function formatTags(tags) {
  return orderedTagNames(tags)
    .map((name) => `[${name} "${escapeTagValue(tags[name])}"]`)
    .join('\n');
}

/** "1. e4 e5 2. Nf3 *"; starts with "N... " when black makes the first move. */
function formatMovetext(history, startFen, result) {
  const fields = startFen.split(' ');
  let number = Number(fields[5]);
  let whiteToMove = fields[1] === 'w';
  const tokens = [];
  history.forEach((entry, index) => {
    if (whiteToMove) tokens.push(`${number}.`);
    else if (index === 0) tokens.push(`${number}...`);
    tokens.push(entry.move.san);
    if (!whiteToMove) number++;
    whiteToMove = !whiteToMove;
  });
  tokens.push(result);
  return tokens.join(' ');
}

/** Reads the leading `[Name "value"]` tag pairs; returns the tags and the remaining text. */
function readTagSection(text) {
  const tagPattern = /^\s*\[\s*([A-Za-z0-9_]+)\s+"((?:[^"\\]|\\.)*)"\s*\]/;
  const headers = {};
  let rest = text.charCodeAt(0) === 0xfeff ? text.slice(1) : text;
  for (let match = tagPattern.exec(rest); match; match = tagPattern.exec(rest)) {
    headers[match[1]] = match[2].replace(/\\(.)/g, '$1');
    rest = rest.slice(match[0].length);
  }
  return { headers, movetext: rest };
}

function skipToLineEnd(text, index) {
  const end = text.indexOf('\n', index);
  return end < 0 ? text.length : end;
}

/**
 * Removes {comments}, ;line comments, %escape lines and (nested variations).
 * Returns null when a comment or variation is never closed.
 */
function stripCommentsAndVariations(text) {
  let out = '';
  let depth = 0;
  for (let i = 0; i < text.length; i++) {
    const ch = text[i];
    if (ch === '{') {
      i = text.indexOf('}', i);
      if (i < 0) return null;
      out += ' ';
    } else if (ch === ';' || (ch === '%' && (i === 0 || text[i - 1] === '\n'))) {
      i = skipToLineEnd(text, i);
      out += ' ';
    } else if (ch === '(') {
      depth++;
    } else if (ch === ')') {
      if (--depth < 0) return null;
      out += ' ';
    } else if (depth === 0) {
      out += ch;
    }
  }
  return depth === 0 ? out : null;
}

function isMoveToken(token) {
  return !RESULT_TOKENS.includes(token) && !/^\d+$/.test(token) && !/^e\.?p\.?$/i.test(token);
}

/**
 * Splits PGN text into its tags and the list of move tokens, or null when malformed.
 * A result token in the movetext fills in the Result tag when the tag itself is missing.
 */
function parsePgn(text) {
  if (typeof text !== 'string') return null;
  const { headers, movetext } = readTagSection(text);
  const main = stripCommentsAndVariations(movetext);
  if (main === null) return null;
  const words = main
    .replace(/\$\d+/g, ' ') // NAGs
    .replace(/\d+\.+/g, ' ') // move numbers: "12." and "12..."
    .split(/\s+/)
    .filter(Boolean);
  const writtenResult = words.filter((word) => RESULT_TOKENS.includes(word)).pop();
  if (writtenResult && !('Result' in headers)) headers.Result = writtenResult;
  return { headers, tokens: words.filter(isMoveToken) };
}

// ---------------------------------------------------------------------------
// 7. Public API
// ---------------------------------------------------------------------------

/**
 * @typedef {'p'|'n'|'b'|'r'|'q'|'k'} PieceType
 * @typedef {'w'|'b'} Color
 *
 * @typedef {Object} Piece
 * @property {PieceType} type
 * @property {Color} color
 *
 * @typedef {Object} Move
 * @property {Color} color        side that moves
 * @property {string} from        origin square, e.g. 'e2'
 * @property {string} to          destination square, e.g. 'e4' (for castling: the king's destination)
 * @property {PieceType} piece    the piece that moves (always 'p' for promotions)
 * @property {PieceType} [captured]   type of the captured piece, when the move captures
 * @property {'q'|'r'|'b'|'n'} [promotion]   piece the pawn promotes to
 * @property {string} san         standard algebraic notation, e.g. 'Nbd7', 'exd8=Q+', 'O-O'
 * @property {string} uci         long algebraic / UCI notation, e.g. 'e2e4', 'e7e8q', 'e1g1'
 * @property {string} flags       any of: 'c' capture, 'e' en passant (always together with 'c'),
 *                                'k' kingside castle, 'q' queenside castle, 'p' promotion,
 *                                'b' double pawn push
 * @property {string} before      FEN before the move
 * @property {string} after       FEN after the move
 *
 * @typedef {{from: string, to: string, promotion?: string}} MoveInput
 */

/** A chess game: position, full rules, move history, SAN / UCI / FEN / PGN support. */
export class Chess {
  /** The single internal state object (see createState). */
  #state;

  /**
   * @param {string} [fen] position to start from; defaults to the standard start position
   * @throws {Error} when the FEN is invalid
   */
  constructor(fen = START_FEN) {
    this.#state = createState(fen);
  }

  /**
   * Replaces the game with the given position and clears the history.
   * On an invalid FEN it throws and the current game is left untouched.
   * @param {string} fen
   * @throws {Error} when the FEN is invalid
   */
  load(fen) {
    this.#state = createState(fen);
  }

  /** Returns to the standard start position with an empty history. */
  reset() {
    this.load(START_FEN);
  }

  /** @returns {string} FEN of the current position */
  fen() {
    return toFen(this.#state);
  }

  /** @returns {string} FEN the game started from (normalised form of what was loaded) */
  startFen() {
    return this.#state.startFen;
  }

  /** @returns {Color} side to move */
  turn() {
    return COLOR_CHARS[this.#state.turn];
  }

  /**
   * @param {string} square e.g. 'e4'
   * @returns {Piece|null} the piece on the square; null when empty or not a square name
   */
  get(square) {
    const sq = squareIndex(square);
    return sq < 0 ? null : publicPiece(this.#state.board[sq]);
  }

  /**
   * @returns {(Piece|null)[][]} 8x8 array of ranks from rank 8 down to rank 1;
   *   board()[0][0] is a8 and board()[7][7] is h1
   */
  board() {
    const { board } = this.#state;
    return Array.from({ length: 8 }, (_, row) =>
      Array.from({ length: 8 }, (_, file) => publicPiece(board[row * 16 + file])),
    );
  }

  /**
   * Legal moves of the side to move. The list is cached per position, so repeated calls
   * are cheap; the returned objects are fresh copies.
   * @param {{square?: string}} [options] `square` limits the result to moves from that square
   * @returns {Move[]}
   */
  moves({ square } = {}) {
    const all = legalMoves(this.#state);
    const wanted = square === undefined ? all : all.filter((move) => move.from === square);
    return wanted.map((move) => ({ ...move }));
  }

  /**
   * Plays a move.
   * @param {string|MoveInput} input SAN ('Nf3', 'O-O', 'exd5', 'e8=Q+'), UCI ('e2e4', 'e7e8q')
   *   or `{from, to, promotion}`. A promoting move must name its promotion piece.
   * @returns {Move|null} the move played, or null when it is not legal (nothing changes)
   */
  move(input) {
    const s = this.#state;
    const index = findMoveIndex(s, input);
    if (index < 0) return null;
    const move = legalMoves(s)[index];
    const code = legalCodes(s)[index];
    makeMove(s, code);
    invalidateCache(s);
    recordPosition(s);
    s.history.push({ move, code });
    return { ...move };
  }

  /**
   * Takes back the last move.
   * @returns {Move|null} the move taken back, or null when there is none
   */
  undo() {
    const s = this.#state;
    const entry = s.history.pop();
    if (!entry) return null;
    forgetPosition(s);
    unmakeMove(s, entry.code);
    invalidateCache(s);
    return { ...entry.move };
  }

  /** @returns {Move[]} the moves played so far, oldest first */
  history() {
    return this.#state.history.map((entry) => ({ ...entry.move }));
  }

  /** @returns {string[]} the moves played so far in UCI notation, e.g. ['e2e4', 'e7e5'] */
  uciMoves() {
    return this.#state.history.map((entry) => entry.move.uci);
  }

  /** @returns {boolean} true when the side to move is in check */
  inCheck() {
    return isKingAttacked(this.#state, this.#state.turn);
  }

  /** @returns {boolean} */
  isCheckmate() {
    return this.inCheck() && legalCodes(this.#state).length === 0;
  }

  /** @returns {boolean} */
  isStalemate() {
    return !this.inCheck() && legalCodes(this.#state).length === 0;
  }

  /**
   * @returns {boolean} true for K v K, K + one minor piece v K, and positions where the
   *   only pieces besides the kings are bishops standing on squares of one colour
   */
  isInsufficientMaterial() {
    return hasInsufficientMaterial(this.#state);
  }

  /** @returns {boolean} true when the current position has occurred at least three times */
  isThreefoldRepetition() {
    return repetitionCount(this.#state) >= 3;
  }

  /** @returns {boolean} true when 50 moves by each side passed without capture or pawn move */
  isFiftyMoves() {
    return this.#state.halfmove >= 100;
  }

  /** @returns {boolean} stalemate, insufficient material, threefold repetition or fifty moves */
  isDraw() {
    if (this.isCheckmate()) return false;
    return (
      this.isStalemate() || this.isInsufficientMaterial() || this.isThreefoldRepetition() || this.isFiftyMoves()
    );
  }

  /** @returns {boolean} checkmate or any draw */
  isGameOver() {
    return this.isCheckmate() || this.isDraw();
  }

  /** @returns {'1-0'|'0-1'|'1/2-1/2'|'*'} result according to the position on the board */
  result() {
    if (this.isCheckmate()) return this.#state.turn === WHITE ? '0-1' : '1-0';
    return this.isDraw() ? '1/2-1/2' : '*';
  }

  /** @returns {'checkmate'|'stalemate'|'insufficient'|'threefold'|'fifty'|null} */
  gameOverReason() {
    if (this.isCheckmate()) return 'checkmate';
    if (this.isStalemate()) return 'stalemate';
    if (this.isInsufficientMaterial()) return 'insufficient';
    if (this.isThreefoldRepetition()) return 'threefold';
    if (this.isFiftyMoves()) return 'fifty';
    return null;
  }

  /**
   * PGN text of the game: tag pairs, an empty line, then the movetext on one line.
   * A Result tag is always written (pass `headers.Result` to record e.g. a resignation),
   * and SetUp / FEN tags are added when the game did not start from START_FEN.
   * @param {{headers?: Object<string, string>}} [options] tags to write; they override
   *   tags remembered from loadPgn()
   * @returns {string}
   */
  pgn({ headers = {} } = {}) {
    const s = this.#state;
    const tags = { ...s.headers, ...headers };
    delete tags.SetUp;
    delete tags.FEN;
    tags.Result = this.#pgnResult(headers.Result);
    if (s.startFen !== START_FEN) {
      tags.SetUp = '1';
      tags.FEN = s.startFen;
    }
    return `${formatTags(tags)}\n\n${formatMovetext(s.history, s.startFen, tags.Result)}\n`;
  }

  /** Explicit result wins, then a result decided on the board, then the loaded tag. */
  #pgnResult(explicit) {
    if (RESULT_TOKENS.includes(explicit)) return explicit;
    const onBoard = this.result();
    if (onBoard !== '*') return onBoard;
    const loaded = this.#state.headers.Result;
    return RESULT_TOKENS.includes(loaded) ? loaded : '*';
  }

  /**
   * Replaces the game with the one described by a PGN text. Comments, variations, NAGs,
   * move numbers and the result token are ignored; a FEN tag sets the start position.
   * @param {string} text
   * @returns {boolean} false when the text cannot be parsed or contains an illegal move;
   *   in that case the current game is left untouched
   */
  loadPgn(text) {
    const parsed = parsePgn(text);
    if (!parsed) return false;
    let game;
    try {
      game = new Chess(parsed.headers.FEN ?? START_FEN);
    } catch {
      return false;
    }
    for (const token of parsed.tokens) {
      if (!game.move(token)) return false;
    }
    const { SetUp, FEN, ...headers } = parsed.headers;
    game.#state.headers = headers;
    this.#state = game.#state;
    return true;
  }

  /** @returns {Object<string, string>} PGN tags remembered from the last loadPgn() (a copy) */
  headers() {
    return { ...this.#state.headers };
  }

  /**
   * @param {Color} color
   * @returns {string|null} square of that side's king, e.g. 'e1'; null for an unknown colour
   */
  kingSquare(color) {
    if (color !== 'w' && color !== 'b') return null;
    return squareName(this.#state.kings[color === 'w' ? WHITE : BLACK]);
  }

  /**
   * Counts the leaf nodes of the legal move tree to the given depth (move generator test).
   * Uses make / unmake on the live position and leaves it unchanged.
   * @param {number} depth
   * @returns {number}
   */
  perft(depth) {
    return perftNodes(this.#state, depth, []);
  }
}
