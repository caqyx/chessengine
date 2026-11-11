// Rules core test suite. Run with:  node game/test/rules.test.mjs
// No framework: every test() runs immediately, failures are collected, exit code is non-zero on any failure.

import assert from 'node:assert/strict';
import { Chess, START_FEN } from '../js/chess.js';

// ---------------------------------------------------------------------------
// Tiny harness
// ---------------------------------------------------------------------------

const failures = [];
let passed = 0;

function test(name, body) {
  const started = performance.now();
  try {
    body();
    passed++;
    console.log(`ok    ${name} (${(performance.now() - started).toFixed(0)} ms)`);
  } catch (error) {
    failures.push(name);
    console.log(`FAIL  ${name}\n      ${String(error && error.message).split('\n').join('\n      ')}`);
  }
}

/** Deterministic PRNG (mulberry32). */
function createRandom(seed) {
  let a = seed >>> 0;
  return () => {
    a = (a + 0x6d2b79f5) >>> 0;
    let t = a;
    t = Math.imul(t ^ (t >>> 15), t | 1);
    t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

function playAll(game, inputs) {
  for (const input of inputs) {
    const move = game.move(input);
    assert.ok(move, `move ${JSON.stringify(input)} was rejected in ${game.fen()}`);
  }
  return game;
}

const sanList = (game) => game.history().map((move) => move.san);
const legalSans = (game) => game.moves().map((move) => move.san);

// ---------------------------------------------------------------------------
// Perft
// ---------------------------------------------------------------------------

const PERFT_CASES = [
  { name: 'start position', fen: START_FEN, nodes: [20, 400, 8902, 197281, 4865609] },
  {
    name: 'kiwipete',
    fen: 'r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1',
    nodes: [48, 2039, 97862, 4085603],
  },
  { name: 'position 3', fen: '8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1', nodes: [14, 191, 2812, 43238, 674624] },
  {
    name: 'position 4',
    fen: 'r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1',
    nodes: [6, 264, 9467, 422333],
  },
  {
    name: 'position 5',
    fen: 'rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8',
    nodes: [44, 1486, 62379, 2103487],
  },
  {
    name: 'position 6',
    fen: 'r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10',
    nodes: [46, 2079, 89890, 3894594],
  },
];

const perftStarted = performance.now();
let perftNodeTotal = 0;
for (const { name, fen, nodes } of PERFT_CASES) {
  test(`perft ${name} to depth ${nodes.length}`, () => {
    const game = new Chess(fen);
    nodes.forEach((expected, index) => {
      const depth = index + 1;
      const started = performance.now();
      const actual = game.perft(depth);
      const ms = performance.now() - started;
      console.log(`        depth ${depth}: ${String(actual).padStart(8)} nodes  ${ms.toFixed(0).padStart(5)} ms`);
      assert.equal(actual, expected, `${name} depth ${depth}`);
      perftNodeTotal += actual;
    });
    assert.equal(game.fen(), fen, 'perft must leave the position untouched');
    assert.equal(game.history().length, 0);
  });
}
const perftMs = performance.now() - perftStarted;
console.log(
  `      perft total: ${perftNodeTotal} nodes in ${perftMs.toFixed(0)} ms (${Math.round(perftNodeTotal / (perftMs / 1000))} nodes/s)`,
);

test('perft agrees with the public moves() / move() / undo() path', () => {
  const viaPublicApi = (game, depth) => {
    if (depth === 0) return 1;
    let nodes = 0;
    for (const move of game.moves()) {
      assert.ok(game.move(move), `moves() returned an unplayable move ${move.san}`);
      nodes += viaPublicApi(game, depth - 1);
      game.undo();
    }
    return nodes;
  };
  assert.equal(viaPublicApi(new Chess(), 3), 8902);
  assert.equal(viaPublicApi(new Chess(PERFT_CASES[1].fen), 2), 2039);
  assert.equal(viaPublicApi(new Chess(PERFT_CASES[3].fen), 3), 9467);
});

// ---------------------------------------------------------------------------
// Basic API
// ---------------------------------------------------------------------------

test('start position basics', () => {
  const game = new Chess();
  assert.equal(START_FEN, 'rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1');
  assert.equal(game.fen(), START_FEN);
  assert.equal(game.startFen(), START_FEN);
  assert.equal(game.turn(), 'w');
  assert.deepEqual(game.get('e1'), { type: 'k', color: 'w' });
  assert.deepEqual(game.get('d8'), { type: 'q', color: 'b' });
  assert.equal(game.get('e4'), null);
  assert.equal(game.get('z9'), null);
  assert.equal(game.get(undefined), null);
  assert.equal(game.kingSquare('w'), 'e1');
  assert.equal(game.kingSquare('b'), 'e8');
  assert.equal(game.moves().length, 20);
  assert.deepEqual(legalSans(game).sort(), [
    'Na3', 'Nc3', 'Nf3', 'Nh3', 'a3', 'a4', 'b3', 'b4', 'c3', 'c4',
    'd3', 'd4', 'e3', 'e4', 'f3', 'f4', 'g3', 'g4', 'h3', 'h4',
  ]);
  assert.deepEqual(game.moves({ square: 'g1' }).map((move) => move.san).sort(), ['Nf3', 'Nh3']);
  assert.deepEqual(game.moves({ square: 'e1' }), []);
  assert.deepEqual(game.moves({ square: 'e5' }), []);
  assert.equal(game.inCheck(), false);
  assert.equal(game.isGameOver(), false);
  assert.equal(game.result(), '*');
  assert.equal(game.gameOverReason(), null);
  assert.deepEqual(game.history(), []);
  assert.deepEqual(game.uciMoves(), []);
  assert.equal(game.undo(), null);
});

test('board() orientation: [0][0] is a8, [7][7] is h1', () => {
  const board = new Chess().board();
  assert.equal(board.length, 8);
  assert.ok(board.every((rank) => rank.length === 8));
  assert.deepEqual(board[0][0], { type: 'r', color: 'b' });
  assert.deepEqual(board[0][4], { type: 'k', color: 'b' });
  assert.deepEqual(board[7][7], { type: 'r', color: 'w' });
  assert.deepEqual(board[7][3], { type: 'q', color: 'w' });
  assert.deepEqual(board[6][0], { type: 'p', color: 'w' });
  assert.equal(board[4][4], null);
});

test('move object shape', () => {
  const game = new Chess();
  const move = game.move('e4');
  assert.deepEqual(move, {
    color: 'w',
    from: 'e2',
    to: 'e4',
    piece: 'p',
    san: 'e4',
    uci: 'e2e4',
    flags: 'b',
    before: START_FEN,
    after: 'rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq e3 0 1',
  });
  assert.equal(game.fen(), move.after);
  assert.equal(game.turn(), 'b');
  playAll(game, ['d5', 'exd5']);
  const capture = game.history()[2];
  assert.equal(capture.captured, 'p');
  assert.equal(capture.flags, 'c');
  assert.equal(capture.san, 'exd5');
  assert.deepEqual(game.uciMoves(), ['e2e4', 'd7d5', 'e4d5']);
  // Returned objects are copies: mutating them must not corrupt the game.
  game.history()[0].san = 'garbage';
  game.moves().forEach((entry) => (entry.san = 'garbage'));
  assert.deepEqual(sanList(game), ['e4', 'd5', 'exd5']);
  assert.ok(legalSans(game).includes('Qxd5'));
});

test('FEN validation, load() and reset()', () => {
  const invalid = [
    '',
    'not a fen',
    'rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP w KQkq - 0 1', // 7 ranks
    'rnbqkbnr/pppppppp/9/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1', // bad digit
    'rnbqkbnr/ppppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1', // 9 squares
    'rnbqkbnr/ppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1', // 7 squares
    'rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQXBNR w KQkq - 0 1', // unknown piece
    'rnbq1bnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1', // no black king
    'rnbqkbnr/pppppppp/8/8/8/4K3/PPPPPPPP/RNBQKBNR w KQkq - 0 1', // two white kings
    'rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR x KQkq - 0 1', // bad side
    'rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQxq - 0 1', // bad castling
    'rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq e9 0 1', // bad en passant
    'rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq e3 0 1', // en passant without a double push
    'rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - x 1', // bad halfmove clock
    'rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 -1', // bad fullmove number
    'P3k3/8/8/8/8/8/8/4K3 w - - 0 1', // pawn on the last rank
    '4k3/8/8/8/8/8/8/4K2r b - - 0 1', // side not to move is in check
  ];
  for (const fen of invalid) assert.throws(() => new Chess(fen), /Invalid FEN/, `should reject "${fen}"`);
  assert.throws(() => new Chess(null), /Invalid FEN/);
  assert.throws(() => new Chess(42), /Invalid FEN/);

  const game = new Chess();
  playAll(game, ['e4', 'e5']);
  const before = game.fen();
  assert.throws(() => game.load('garbage'), /Invalid FEN/);
  assert.equal(game.fen(), before, 'a failed load() must not change the game');
  assert.equal(game.history().length, 2);

  const custom = 'r1bqkbnr/pppp1ppp/2n5/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R w KQkq - 2 3';
  game.load(custom);
  assert.equal(game.fen(), custom);
  assert.equal(game.startFen(), custom);
  assert.deepEqual(game.history(), []);
  assert.equal(game.undo(), null);
  game.reset();
  assert.equal(game.fen(), START_FEN);
  assert.equal(game.startFen(), START_FEN);

  // A valid en passant square is kept, and written after every double push.
  const withEp = 'rnbqkbnr/ppp1pppp/8/8/3pP3/8/PPPP1PPP/RNBQKBNR b KQkq e3 0 3';
  assert.equal(new Chess(withEp).fen(), withEp);
  assert.ok(legalSans(new Chess(withEp)).includes('dxe3'));
  // Castling rights that the pieces cannot back up are dropped; missing counters default to "0 1".
  assert.equal(new Chess('4k3/8/8/8/8/8/8/4K3 w KQkq - 0 1').fen(), '4k3/8/8/8/8/8/8/4K3 w - - 0 1');
  assert.equal(new Chess('4k3/8/8/8/8/8/8/R3K3 w KQkq -').fen(), '4k3/8/8/8/8/8/8/R3K3 w Q - 0 1');
});

// ---------------------------------------------------------------------------
// A complete hand-written game: SAN generation and parsing
// ---------------------------------------------------------------------------

// 41 moves. Contains: en passant (3. exf6), castling on both wings (7. O-O O-O-O), file
// disambiguation (8. Nbd2, 8... Nge7, 20... Rdh8, 34... Ndf4), rank disambiguation (21... R8h5,
// 36... N6xd4), promotion with capture and check (16... gxf1=Q+), an underpromotion
// (32... g1=N), many checks and a final checkmate.
const LONG_GAME = [
  'e4', 'd5', 'e5', 'f5', 'exf6', 'exf6', 'd4', 'Nc6', 'Nf3', 'Bg4',
  'Be2', 'Qd7', 'O-O', 'O-O-O', 'Nbd2', 'Nge7', 'c4', 'dxc4', 'Nxc4', 'Nd5',
  'a4', 'h5', 'a5', 'h4', 'a6', 'h3', 'axb7+', 'Kb8', 'Ra6', 'hxg2',
  'Qa4', 'gxf1=Q+', 'Bxf1', 'Bxf3', 'Qb5', 'Rh3', 'Bd2', 'Bd6', 'Na5', 'Rdh8',
  'b4', 'R8h5', 'Bxh3', 'Qxh3', 'Qf1', 'Qxf1+', 'Kxf1', 'Rxh2', 'Ke1', 'g5',
  'Nxc6+', 'Kxb7', 'Rxa7+', 'Kxc6', 'b5+', 'Kxb5', 'Ra1', 'g4', 'Rc1', 'g3',
  'Rc2', 'g2', 'Rc1', 'g1=N', 'Rc2', 'Ne2', 'Rc1', 'Ndf4', 'Rc2', 'Ne6',
  'Rc1', 'N6xd4', 'Rb1+', 'Kc4', 'Rc1+', 'Kd3', 'Rc3+', 'Nxc3', 'Bxc3', 'Kxc3',
  'Kf1', 'Rh1#',
];
const LONG_GAME_FINAL_FEN = '8/2p5/3b1p2/8/3n4/2k2b2/5P2/5K1r w - - 2 42';

test('long game: SAN generation matches the written score', () => {
  assert.ok(LONG_GAME.length >= 80, 'the game must be at least 40 full moves');
  const game = new Chess();
  LONG_GAME.forEach((san, ply) => {
    const where = `ply ${ply + 1} (${san})`;
    const candidates = game.moves().filter((move) => move.san === san);
    assert.equal(candidates.length, 1, `${where}: expected exactly one legal move with this SAN, legal: ${legalSans(game)}`);
    const before = game.fen();
    const played = game.move(san);
    assert.ok(played, `${where}: SAN was not accepted`);
    assert.deepEqual(played, candidates[0], where);
    assert.equal(played.before, before, where);
    assert.equal(played.after, game.fen(), where);
    assert.equal(played.color, ply % 2 === 0 ? 'w' : 'b', where);
  });
  assert.equal(game.fen(), LONG_GAME_FINAL_FEN);
  assert.deepEqual(sanList(game), LONG_GAME);
  assert.equal(game.isCheckmate(), true);
  assert.equal(game.result(), '0-1');
  assert.equal(game.gameOverReason(), 'checkmate');
});

test('long game: special moves carry the right data', () => {
  const history = playAll(new Chess(), LONG_GAME).history();
  const byPly = (ply) => history[ply - 1];

  const doublePush = byPly(4); // 2... f5
  assert.equal(doublePush.flags, 'b');
  assert.equal(doublePush.after.split(' ')[3], 'f6');

  const enPassant = byPly(5); // 3. exf6
  assert.equal(enPassant.san, 'exf6');
  assert.equal(enPassant.flags, 'ce');
  assert.equal(enPassant.captured, 'p');
  assert.equal(enPassant.uci, 'e5f6');
  assert.equal(new Chess(enPassant.after).get('f5'), null, 'the captured pawn must leave f5');

  const short = byPly(13); // 7. O-O
  assert.deepEqual([short.san, short.uci, short.flags, short.piece], ['O-O', 'e1g1', 'k', 'k']);
  assert.deepEqual(new Chess(short.after).get('f1'), { type: 'r', color: 'w' });
  assert.equal(short.after.split(' ')[2], 'kq');

  const long = byPly(14); // 7... O-O-O
  assert.deepEqual([long.san, long.uci, long.flags], ['O-O-O', 'e8c8', 'q']);
  assert.deepEqual(new Chess(long.after).get('d8'), { type: 'r', color: 'b' });
  assert.equal(long.after.split(' ')[2], '-');

  const promotion = byPly(32); // 16... gxf1=Q+
  assert.deepEqual(
    [promotion.san, promotion.uci, promotion.flags, promotion.piece, promotion.captured, promotion.promotion],
    ['gxf1=Q+', 'g2f1q', 'cp', 'p', 'r', 'q'],
  );

  const underpromotion = byPly(64); // 32... g1=N
  assert.deepEqual(
    [underpromotion.san, underpromotion.uci, underpromotion.flags, underpromotion.promotion, underpromotion.captured],
    ['g1=N', 'g2g1n', 'p', 'n', undefined],
  );
  assert.deepEqual(new Chess(underpromotion.after).get('g1'), { type: 'n', color: 'b' });
});

test('long game: SAN, UCI and {from,to,promotion} inputs replay to the same game', () => {
  const reference = playAll(new Chess(), LONG_GAME);
  const viaUci = playAll(new Chess(), reference.uciMoves());
  const viaObjects = playAll(
    new Chess(),
    reference.history().map(({ from, to, promotion }) => ({ from, to, promotion })),
  );
  const viaMoveObjects = playAll(new Chess(), reference.history());
  for (const replay of [viaUci, viaObjects, viaMoveObjects]) {
    assert.equal(replay.fen(), LONG_GAME_FINAL_FEN);
    assert.deepEqual(replay.history(), reference.history());
  }
});

test('long game: undo restores every earlier FEN exactly', () => {
  const game = new Chess();
  const fens = [game.fen()];
  for (const san of LONG_GAME) {
    game.move(san);
    fens.push(game.fen());
  }
  for (let ply = LONG_GAME.length; ply > 0; ply--) {
    assert.equal(game.fen(), fens[ply]);
    assert.equal(game.undo().san, LONG_GAME[ply - 1]);
  }
  assert.equal(game.fen(), START_FEN);
  assert.equal(game.undo(), null);
});

// ---------------------------------------------------------------------------
// SAN details
// ---------------------------------------------------------------------------

test('SAN disambiguation: file, then rank, then both', () => {
  // Three queens that can all reach e1.
  const game = new Chess('8/8/k7/8/4Q2Q/8/8/K6Q w - - 0 1');
  const toE1 = game.moves().filter((move) => move.to === 'e1');
  const sanFrom = Object.fromEntries(toE1.map((move) => [move.from, move.san]));
  assert.deepEqual(sanFrom, { e4: 'Qee1', h1: 'Q1e1', h4: 'Qh4e1' });
  for (const [from, san] of Object.entries(sanFrom)) {
    const copy = new Chess(game.fen());
    assert.equal(copy.move(san).from, from);
  }
  // Knights on the same file need the rank, knights on the same rank need the file.
  assert.ok(legalSans(new Chess('4k3/8/8/3N4/8/3N4/8/4K3 w - - 0 1')).includes('N5f4'));
  assert.ok(legalSans(new Chess('4k3/8/8/3N4/8/3N4/8/4K3 w - - 0 1')).includes('N3b4'));
  assert.ok(legalSans(new Chess('4k3/8/8/8/8/8/8/1N2KN2 w - - 0 1')).includes('Nbd2'));
  // A pinned piece is not a rival: only the f3 knight may go to d2, so no hint is written.
  const pinned = new Chess('4k3/8/8/8/1b6/2N2N2/8/4K3 w - - 0 1');
  assert.ok(legalSans(pinned).includes('Nd2'));
  assert.ok(!legalSans(pinned).includes('Nfd2'));
});

test('SAN parser is lenient', () => {
  const afterOpening = () => playAll(new Chess(), ['e4', 'e5', 'Nf3', 'Nc6', 'Bc4', 'Bc5']);

  assert.equal(afterOpening().move('0-0').san, 'O-O');
  assert.equal(afterOpening().move('O-O+').san, 'O-O', 'a wrong check mark is ignored');
  assert.equal(afterOpening().move('Nb1c3').san, 'Nc3', 'over-disambiguation with a full square');
  assert.equal(afterOpening().move('Nbc3').san, 'Nc3', 'over-disambiguation with a file');
  assert.equal(afterOpening().move('N1c3').san, 'Nc3', 'over-disambiguation with a rank');
  assert.equal(afterOpening().move('d2d4').san, 'd4');
  assert.equal(afterOpening().move(' d4! ').san, 'd4');
  assert.equal(afterOpening().move('Bxf7').san, 'Bxf7+', 'missing check mark');
  assert.equal(afterOpening().move('Bxf7#').san, 'Bxf7+', 'wrong mate mark');

  const queenside = new Chess('r3k2r/8/8/8/8/8/8/R3K2R b KQkq - 0 1');
  assert.equal(queenside.move('0-0-0').san, 'O-O-O');

  const promotionFen = '3r4/4P3/8/8/8/8/k7/4K3 w - - 0 1';
  assert.equal(new Chess(promotionFen).move('e8Q').san, 'e8=Q');
  assert.equal(new Chess(promotionFen).move('e8=Q').san, 'e8=Q');
  assert.equal(new Chess(promotionFen).move('exd8Q').san, 'exd8=Q');
  assert.equal(new Chess(promotionFen).move('exd8=N').san, 'exd8=N');
  assert.equal(new Chess(promotionFen).move('e7d8r').san, 'exd8=R');
  assert.equal(new Chess(promotionFen).move('e7e8Q').san, 'e8=Q');
  assert.equal(new Chess(promotionFen).move({ from: 'e7', to: 'e8', promotion: 'b' }).san, 'e8=B');
  assert.equal(new Chess(promotionFen).move('e8'), null, 'a promotion needs its piece');
  assert.equal(new Chess(promotionFen).move('e7e8'), null);
  assert.equal(new Chess(promotionFen).move({ from: 'e7', to: 'e8' }), null);
  assert.equal(new Chess(promotionFen).move('e8=K'), null);

  // Ambiguous or wrong descriptions are refused.
  const twoKnights = new Chess('4k3/8/8/8/8/8/8/1N2KN2 w - - 0 1');
  assert.equal(twoKnights.move('Nd2'), null, 'ambiguous');
  assert.equal(twoKnights.move('Ngd2'), null, 'no knight on the g-file');
  assert.equal(twoKnights.move('Nfd2').from, 'f1');
});

// ---------------------------------------------------------------------------
// PGN
// ---------------------------------------------------------------------------

test('PGN export of the long game and loadPgn round trip', () => {
  const game = playAll(new Chess(), LONG_GAME);
  const headers = { Event: 'Rules test', White: 'Alice', Black: 'Bob "the rook" B.', Site: 'Node', Annotator: 'x\\y' };
  const pgn = game.pgn({ headers });
  const lines = pgn.split('\n');
  assert.equal(lines[0], '[Event "Rules test"]');
  assert.ok(lines.includes('[Black "Bob \\"the rook\\" B."]'));
  assert.ok(lines.includes('[Result "0-1"]'));
  assert.ok(!pgn.includes('[SetUp'), 'no SetUp tag for the standard start');
  assert.ok(!pgn.includes('[FEN'));
  assert.ok(pgn.includes('\n\n1. e4 d5 2. e5 f5 3. exf6 exf6 4. d4 Nc6 '));
  assert.ok(pgn.trimEnd().endsWith('40. Bxc3 Kxc3 41. Kf1 Rh1# 0-1'));

  const copy = new Chess();
  assert.equal(copy.loadPgn(pgn), true);
  assert.equal(copy.fen(), game.fen());
  assert.equal(copy.startFen(), START_FEN);
  assert.deepEqual(copy.history(), game.history());
  assert.deepEqual(copy.headers(), { ...headers, Result: '0-1' });
  assert.equal(copy.pgn(), pgn, 'loaded tags are written back by pgn()');
  assert.equal(copy.undo().san, 'Rh1#', 'a loaded game can be taken back');

  // Without headers only the Result tag is written.
  assert.equal(playAll(new Chess(), ['e4', 'e5', 'Nf3']).pgn(), '[Result "*"]\n\n1. e4 e5 2. Nf3 *\n');
  assert.equal(new Chess().pgn(), '[Result "*"]\n\n*\n');
  // An explicit result (resignation, flag) wins over the undecided position.
  assert.ok(playAll(new Chess(), ['e4']).pgn({ headers: { Result: '0-1' } }).endsWith('1. e4 0-1\n'));
});

test('PGN from a custom FEN (black to move) and round trip', () => {
  const fen = 'r3k2r/pp3ppp/2n5/3pP3/8/2P5/P4PPP/R3K2R b KQkq - 0 14';
  const game = playAll(new Chess(fen), ['f5', 'exf6', 'O-O-O', 'f7', 'Rhf8', 'O-O', 'Rxf7']);
  assert.equal(game.history()[1].flags, 'ce');
  const pgn = game.pgn({ headers: { Event: 'From position' } });
  assert.equal(
    pgn,
    [
      '[Event "From position"]',
      '[Result "*"]',
      '[SetUp "1"]',
      `[FEN "${fen}"]`,
      '',
      '14... f5 15. exf6 O-O-O 16. f7 Rhf8 17. O-O Rxf7 *',
      '',
    ].join('\n'),
  );
  const copy = new Chess();
  assert.equal(copy.loadPgn(pgn), true);
  assert.equal(copy.startFen(), fen);
  assert.equal(copy.fen(), game.fen());
  assert.deepEqual(sanList(copy), sanList(game));
  assert.equal(copy.pgn(), pgn);
  assert.equal(copy.fen().split(' ')[5], '18');
  copy.reset();
  assert.deepEqual(copy.headers(), {}, 'reset() forgets loaded tags');
});

test('loadPgn ignores comments, variations, NAGs, numbers and results', () => {
  const text = [
    String.fromCharCode(0xfeff) + '[Event "Noise"]',
    '[White "A \\"quoted\\" name"]',
    '[Result "0-1"]',
    '',
    '% an escape line that must be skipped',
    '1. f3 {a weak move; really (yes)} e5 $2 (1... d5 2. e4 (2. d4 {nested}) dxe4)',
    '2.g4?? ; the rest of this line is a comment Qh4 Nf3',
    '2... Qh4# $1 0-1',
  ].join('\n');
  const game = new Chess('4k3/8/8/8/8/8/8/4K3 w - - 0 1');
  assert.equal(game.loadPgn(text), true);
  assert.deepEqual(sanList(game), ['f3', 'e5', 'g4', 'Qh4#']);
  assert.equal(game.startFen(), START_FEN);
  assert.equal(game.isCheckmate(), true);
  assert.deepEqual(game.headers(), { Event: 'Noise', White: 'A "quoted" name', Result: '0-1' });

  const compact = new Chess();
  assert.equal(compact.loadPgn('1.e4 e5 2.Nf3 Nc6 3.Bc4 Bc5 4.0-0 Nf6 1/2-1/2'), true);
  assert.deepEqual(sanList(compact), ['e4', 'e5', 'Nf3', 'Nc6', 'Bc4', 'Bc5', 'O-O', 'Nf6']);
  assert.ok(compact.pgn().endsWith('4. O-O Nf6 1/2-1/2\n'), 'a loaded Result tag is kept while the game is undecided');

  const enPassantNote = new Chess();
  assert.equal(enPassantNote.loadPgn('1. e4 a6 2. e5 d5 3. exd6 e.p. *'), true);
  assert.equal(enPassantNote.history()[4].flags, 'ce');

  assert.equal(new Chess().loadPgn(''), true, 'an empty PGN is an empty game');
});

test('loadPgn returns false and keeps the state on bad input', () => {
  const game = playAll(new Chess(), ['d4', 'd5', 'c4']);
  const fen = game.fen();
  const bad = [
    '1. e4 e5 2. Ke3',
    '1. e4 e5 2. Nf3 {never closed',
    '1. e4 (1. d4 e5',
    '1. e4 e5) 2. Nf3',
    '[FEN "this is not a fen"]\n\n1. e4',
    '[SetUp "1"]\n[FEN "4k3/8/8/8/8/8/8/4K3 w - - 0 1"]\n\n1. e4',
    'hello world',
    null,
    undefined,
    42,
  ];
  for (const text of bad) {
    assert.equal(game.loadPgn(text), false, `should reject ${JSON.stringify(text)}`);
    assert.equal(game.fen(), fen);
    assert.deepEqual(sanList(game), ['d4', 'd5', 'c4']);
  }
});

// ---------------------------------------------------------------------------
// Game end conditions
// ---------------------------------------------------------------------------

test("checkmate: fool's mate", () => {
  const game = playAll(new Chess(), ['f3', 'e5', 'g4']);
  assert.equal(game.isGameOver(), false);
  assert.equal(game.move('Qh4').san, 'Qh4#');
  assert.equal(game.inCheck(), true);
  assert.equal(game.isCheckmate(), true);
  assert.equal(game.isStalemate(), false);
  assert.equal(game.isDraw(), false);
  assert.equal(game.isGameOver(), true);
  assert.equal(game.result(), '0-1');
  assert.equal(game.gameOverReason(), 'checkmate');
  assert.deepEqual(game.moves(), []);
  assert.equal(game.move('Kf2'), null);
  game.undo();
  assert.equal(game.isGameOver(), false);
  assert.equal(game.result(), '*');

  const scholars = playAll(new Chess(), ['e4', 'e5', 'Qh5', 'Nc6', 'Bc4', 'Nf6', 'Qxf7#']);
  assert.equal(scholars.result(), '1-0');
});

test('stalemate', () => {
  const game = new Chess('7k/8/6K1/8/8/8/8/5Q2 w - - 0 1');
  assert.equal(game.move('Qf7').san, 'Qf7', 'stalemate is not check');
  assert.equal(game.inCheck(), false);
  assert.equal(game.isStalemate(), true);
  assert.equal(game.isCheckmate(), false);
  assert.equal(game.isDraw(), true);
  assert.equal(game.isGameOver(), true);
  assert.equal(game.result(), '1/2-1/2');
  assert.equal(game.gameOverReason(), 'stalemate');
  assert.deepEqual(game.moves(), []);
});

test('threefold repetition: knights shuffling back and forth', () => {
  const game = new Chess();
  const shuffle = ['Nf3', 'Nf6', 'Ng1', 'Ng8'];
  playAll(game, shuffle);
  assert.equal(game.isThreefoldRepetition(), false, 'start position seen twice');
  playAll(game, shuffle.slice(0, 3));
  assert.equal(game.isThreefoldRepetition(), false);
  assert.equal(game.isGameOver(), false);
  game.move('Ng8');
  assert.equal(game.isThreefoldRepetition(), true, 'start position seen three times');
  assert.equal(game.isDraw(), true);
  assert.equal(game.result(), '1/2-1/2');
  assert.equal(game.gameOverReason(), 'threefold');
  assert.ok(game.moves().length > 0, 'moves stay available: the draw is for the UI to enforce');
  game.undo();
  assert.equal(game.isThreefoldRepetition(), false, 'undo takes the repetition back');
  game.move('Ng8');
  assert.equal(game.isThreefoldRepetition(), true);
});

test('threefold repetition: castling rights and en passant make positions different', () => {
  // Same placement, but the first occurrence still had castling rights.
  const rights = new Chess('r3k3/8/8/8/8/8/8/4K2R w Kq - 0 1');
  playAll(rights, ['Rh2', 'Ra7', 'Rh1', 'Ra8']); // placement repeats, rights are gone
  playAll(rights, ['Rh2', 'Ra7', 'Rh1', 'Ra8']);
  assert.equal(rights.isThreefoldRepetition(), false, 'only two occurrences without castling rights');
  playAll(rights, ['Rh2', 'Ra7', 'Rh1', 'Ra8']);
  assert.equal(rights.isThreefoldRepetition(), true);

  // The en passant square is ignored when no en passant capture is possible ...
  const noCapture = playAll(new Chess(), ['e4']); // FEN says e3, but nothing can capture there
  assert.equal(noCapture.fen().split(' ')[3], 'e3');
  playAll(noCapture, ['Nf6', 'Nf3', 'Ng8', 'Ng1']);
  playAll(noCapture, ['Nf6', 'Nf3', 'Ng8']);
  assert.equal(noCapture.isThreefoldRepetition(), false);
  noCapture.move('Ng1');
  assert.equal(noCapture.isThreefoldRepetition(), true);

  // ... and counted when the capture is really available.
  const withCapture = new Chess('4k1n1/3p4/8/4P3/8/8/8/4K1N1 b - - 0 1');
  withCapture.move('d5');
  assert.ok(legalSans(withCapture).includes('exd6'));
  playAll(withCapture, ['Nf3', 'Nf6', 'Ng1', 'Ng8']);
  playAll(withCapture, ['Nf3', 'Nf6', 'Ng1', 'Ng8']);
  assert.equal(withCapture.isThreefoldRepetition(), false, 'the first occurrence allowed exd6');
  playAll(withCapture, ['Nf3', 'Nf6', 'Ng1', 'Ng8']);
  assert.equal(withCapture.isThreefoldRepetition(), true);

  // A capture that would expose the own king is not possible, so the square does not count:
  // after ...c5, bxc6 would remove both pawns from the fifth rank and open it for the rook.
  const pinned = new Chess('4k1n1/2p5/8/KP5r/8/8/8/6N1 b - - 0 1');
  pinned.move('c5');
  assert.equal(pinned.fen().split(' ')[3], 'c6');
  assert.ok(!legalSans(pinned).includes('bxc6'));
  playAll(pinned, ['Nf3', 'Nf6', 'Ng1', 'Ng8']);
  assert.equal(pinned.isThreefoldRepetition(), false);
  playAll(pinned, ['Nf3', 'Nf6', 'Ng1', 'Ng8']);
  assert.equal(pinned.isThreefoldRepetition(), true);
});

test('fifty-move rule', () => {
  const game = new Chess('8/8/8/4k3/8/8/P3K3/7R w - - 98 80');
  assert.equal(game.isFiftyMoves(), false);
  game.move('Rh2');
  assert.equal(game.fen().split(' ')[4], '99');
  assert.equal(game.isFiftyMoves(), false);
  assert.equal(game.isDraw(), false);
  game.move('Kd5');
  assert.equal(game.fen().split(' ')[4], '100');
  assert.equal(game.isFiftyMoves(), true);
  assert.equal(game.isDraw(), true);
  assert.equal(game.result(), '1/2-1/2');
  assert.equal(game.gameOverReason(), 'fifty');
  game.undo();
  assert.equal(game.isFiftyMoves(), false);

  game.move('Kd5');
  game.move('a3'); // a pawn move resets the clock
  assert.equal(game.fen().split(' ')[4], '0');
  assert.equal(game.isFiftyMoves(), false);
  assert.equal(game.isGameOver(), false);

  const capture = new Chess('8/8/8/4k3/8/8/4K3/r6R w - - 99 80');
  capture.move('Rxa1'); // a capture resets the clock
  assert.equal(capture.fen().split(' ')[4], '0');
  assert.equal(capture.isFiftyMoves(), false);

  // Checkmate on the move that completes the fifty moves is still checkmate.
  const mate = new Chess('7k/8/6K1/8/8/8/8/R7 w - - 99 120');
  assert.equal(mate.move('Ra8').san, 'Ra8#');
  assert.equal(mate.isFiftyMoves(), true);
  assert.equal(mate.isDraw(), false);
  assert.equal(mate.result(), '1-0');
  assert.equal(mate.gameOverReason(), 'checkmate');
});

test('insufficient material', () => {
  const insufficient = [
    ['K v K', '8/8/8/4k3/8/8/4K3/8 w - - 0 1'],
    ['K+N v K', '8/8/8/4k3/8/8/4K1N1/8 w - - 0 1'],
    ['K v K+N', '8/8/8/4k1n1/8/8/4K3/8 w - - 0 1'],
    ['K+B v K', '8/8/8/4k3/8/8/4KB2/8 b - - 0 1'],
    ['K v K+B', '5b2/8/8/4k3/8/8/4K3/8 w - - 0 1'],
    ['K+B v K+B, both on dark squares', '5b2/8/8/4k3/8/8/4K3/2B5 w - - 0 1'],
    ['K+B v K+B, both on light squares', '2b5/8/8/4k3/8/8/4K3/5B2 w - - 0 1'],
  ];
  for (const [name, fen] of insufficient) {
    const game = new Chess(fen);
    assert.equal(game.isInsufficientMaterial(), true, name);
    assert.equal(game.isDraw(), true, name);
    assert.equal(game.result(), '1/2-1/2', name);
    assert.equal(game.gameOverReason(), 'insufficient', name);
  }
  const sufficient = [
    ['start position', START_FEN],
    ['K+B v K+B on opposite colours', '2b5/8/8/4k3/8/8/4K3/2B5 w - - 0 1'],
    ['K+N v K+N', '8/8/8/4k1n1/8/8/4K1N1/8 w - - 0 1'],
    ['K+N+N v K', '8/8/8/4k3/8/8/4KNN1/8 w - - 0 1'],
    ['K+B+N v K', '8/8/8/4k3/8/8/4KBN1/8 w - - 0 1'],
    ['K+B v K+N', '8/8/8/4k1n1/8/8/4KB2/8 w - - 0 1'],
    ['K+P v K', '8/8/8/4k3/8/8/4KP2/8 w - - 0 1'],
    ['K+R v K', '8/8/8/4k3/8/8/4KR2/8 w - - 0 1'],
    ['K+Q v K', '8/8/8/4k3/8/8/4KQ2/8 w - - 0 1'],
  ];
  for (const [name, fen] of sufficient) {
    const game = new Chess(fen);
    assert.equal(game.isInsufficientMaterial(), false, name);
    assert.equal(game.isGameOver(), false, name);
  }
  // Capturing the last pawn ends the game.
  const game = new Chess('8/8/8/8/5k2/8/4p3/4K3 w - - 0 1');
  assert.equal(game.isGameOver(), false);
  game.move('Kxe2');
  assert.equal(game.gameOverReason(), 'insufficient');
});

// ---------------------------------------------------------------------------
// Castling
// ---------------------------------------------------------------------------

test('castling rights are lost when a rook is captured on its home square', () => {
  const game = new Chess('r3k2r/8/8/8/8/8/6b1/R3K2R b KQkq - 0 1');
  assert.equal(game.move('Bxh1').captured, 'r');
  assert.equal(game.fen().split(' ')[2], 'Qkq');
  assert.equal(game.move('O-O'), null);
  assert.equal(game.move('e1g1'), null);
  assert.ok(!legalSans(game).includes('O-O'));
  const castle = game.move('O-O-O');
  assert.deepEqual([castle.uci, castle.flags], ['e1c1', 'q']);
  assert.deepEqual(game.get('d1'), { type: 'r', color: 'w' });
  assert.deepEqual(game.get('c1'), { type: 'k', color: 'w' });
  assert.equal(game.get('a1'), null);
  assert.equal(game.fen().split(' ')[2], 'kq');
  assert.equal(game.kingSquare('w'), 'c1');

  // Rook takes rook: the mover loses one right, the victim loses the matching one.
  const rooks = new Chess('r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1');
  assert.equal(rooks.move('Rxa8+').san, 'Rxa8+');
  assert.equal(rooks.fen().split(' ')[2], 'Kk');
  rooks.undo();
  assert.equal(rooks.fen().split(' ')[2], 'KQkq');
  rooks.move('Rxh8+');
  assert.equal(rooks.fen().split(' ')[2], 'Qq');

  // A new rook arriving on the corner does not bring the right back.
  const returned = new Chess('r3k2r/8/8/8/8/8/6b1/R3K2R b KQkq - 0 1');
  playAll(returned, ['Bxh1', 'Ra7', 'Bd5', 'Rh7', 'Bg8', 'Rh1', 'Rb8']);
  assert.deepEqual(returned.get('h1'), { type: 'r', color: 'w' });
  assert.equal(returned.fen().split(' ')[2], 'k');
  assert.equal(returned.move('O-O'), null);
});

test('castling is illegal out of, through or into check, and fine otherwise', () => {
  const sans = (fen) => legalSans(new Chess(fen)).filter((san) => san.startsWith('O-O'));
  assert.deepEqual(sans('4k3/8/8/8/8/8/8/R3K2R w KQ - 0 1').sort(), ['O-O', 'O-O-O']);
  assert.deepEqual(sans('5r2/6k1/8/8/8/8/8/R3K2R w KQ - 0 1'), ['O-O-O'], 'f1 is attacked');
  assert.deepEqual(sans('6r1/1k6/8/8/8/8/8/R3K2R w KQ - 0 1'), ['O-O-O'], 'g1 is attacked');
  assert.deepEqual(sans('3r4/6k1/8/8/8/8/8/R3K2R w KQ - 0 1'), ['O-O'], 'd1 is attacked');
  assert.deepEqual(sans('2r5/6k1/8/8/8/8/8/R3K2R w KQ - 0 1'), ['O-O'], 'c1 is attacked');
  assert.deepEqual(sans('1r6/6k1/8/8/8/8/8/R3K2R w KQ - 0 1').sort(), ['O-O', 'O-O-O'], 'b1 may be attacked');
  assert.deepEqual(sans('8/6k1/8/4r3/8/8/8/R3K2R w KQ - 0 1'), [], 'king is in check');
  assert.deepEqual(sans('4k3/8/8/8/8/8/8/RN2K1NR w KQ - 0 1'), [], 'pieces in the way');
  assert.deepEqual(sans('4k3/8/8/8/8/8/8/R3K2R w - - 0 1'), [], 'no rights');
  assert.deepEqual(sans('r3k2r/8/8/8/8/8/8/4K3 b kq - 0 1').sort(), ['O-O', 'O-O-O']);

  // Moving the king or a rook gives the rights up for good.
  const game = new Chess('r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1');
  playAll(game, ['Ke2', 'Rh7', 'Ke1', 'Rh8']);
  assert.equal(game.fen().split(' ')[2], 'q');
  assert.deepEqual(legalSans(game).filter((san) => san.startsWith('O-O')), []);
});

// ---------------------------------------------------------------------------
// Illegal input
// ---------------------------------------------------------------------------

test('illegal move input returns null and leaves the state untouched', () => {
  const game = playAll(new Chess(), ['e4', 'e5', 'Bc4', 'Qh4']);
  const fen = game.fen();
  const history = game.history();
  const pgn = game.pgn();
  const illegal = [
    'e5', 'e4e6', 'Ke3', 'Nf6', 'O-O', 'O-O-O', 'Bxf8', 'Qd8', 'e7e5', 'd7d5',
    'f4', // legal-looking, but f2 is pinned by the queen on h4
    'f2f4',
    'e2e4q',
    'Zz9', '', ' ', 'e2', 'e2-e4-e5', '--', 'null',
    { from: 'e4', to: 'e6' },
    { from: 'e7', to: 'e5' },
    { from: 'f2', to: 'f3' },
    { from: 'e4' },
    { to: 'd3' },
    { from: 'zz', to: 'e4' },
    {}, [], null, undefined, 42, true,
  ];
  for (const input of illegal) {
    assert.equal(game.move(input), null, `should reject ${JSON.stringify(input)}`);
    assert.equal(game.fen(), fen);
  }
  assert.deepEqual(game.history(), history);
  assert.equal(game.pgn(), pgn);
  assert.equal(game.turn(), 'w');
  // The game is still fully usable afterwards.
  assert.equal(game.move({ from: 'g1', to: 'f3', promotion: 'q' }).san, 'Nf3', 'a stray promotion on an object is ignored');
  assert.equal(game.undo().san, 'Nf3');
  assert.equal(game.fen(), fen);
});

// ---------------------------------------------------------------------------
// Random games: undo, and SAN / UCI / PGN round trips
// ---------------------------------------------------------------------------

const RANDOM_GAME_PLIES = 300;

// Seeds are chosen so that the random game does not end by mate or stalemate before 300 plies.
for (const seed of [1, 2, 3, 5, 11, 2024]) {
  test(`random game, seed ${seed}: ${RANDOM_GAME_PLIES} plies, round trips, undo back to the start`, () => {
    const random = createRandom(seed);
    const game = new Chess();
    const fens = [game.fen()];
    for (let ply = 0; ply < RANDOM_GAME_PLIES; ply++) {
      const moves = game.moves();
      assert.ok(moves.length > 0, `seed ${seed} ended after ${ply} plies; pick another seed`);
      const chosen = moves[Math.floor(random() * moves.length)];
      assert.equal(chosen.before, game.fen());
      const played = game.move(ply % 3 === 0 ? chosen.san : ply % 3 === 1 ? chosen.uci : chosen);
      assert.deepEqual(played, chosen);
      assert.equal(game.fen(), chosen.after);
      fens.push(game.fen());
    }
    assert.equal(game.history().length, RANDOM_GAME_PLIES);

    const final = game.fen();
    assert.equal(playAll(new Chess(), sanList(game)).fen(), final, 'SAN replay');
    assert.equal(playAll(new Chess(), game.uciMoves()).fen(), final, 'UCI replay');
    const fromPgn = new Chess();
    assert.equal(fromPgn.loadPgn(game.pgn()), true);
    assert.equal(fromPgn.fen(), final, 'PGN replay');
    assert.equal(fromPgn.isThreefoldRepetition(), game.isThreefoldRepetition());

    for (let ply = RANDOM_GAME_PLIES; ply > 0; ply--) {
      assert.equal(game.fen(), fens[ply], `FEN before undoing ply ${ply}`);
      assert.ok(game.undo());
    }
    assert.equal(game.fen(), START_FEN);
    assert.equal(game.history().length, 0);
    assert.equal(game.undo(), null);
    assert.equal(game.isThreefoldRepetition(), false);
    assert.equal(game.moves().length, 20);
  });
}

// ---------------------------------------------------------------------------
// moves() speed (informative: printed, with a very generous limit)
// ---------------------------------------------------------------------------

test('moves() is fast enough to call on every UI interaction', () => {
  const fens = PERFT_CASES.map((entry) => entry.fen);
  const rounds = 300;
  const started = performance.now();
  let count = 0;
  for (let round = 0; round < rounds; round++) {
    for (const fen of fens) count += new Chess(fen).moves().length; // fresh object: no cache
  }
  const perCall = (performance.now() - started) / (rounds * fens.length);
  console.log(`        uncached new Chess(fen).moves(): ${perCall.toFixed(3)} ms per call`);
  assert.equal(count, rounds * (20 + 48 + 14 + 6 + 44 + 46));
  assert.ok(perCall < 5, `moves() took ${perCall.toFixed(2)} ms per call`);
});

// ---------------------------------------------------------------------------

console.log(`\n${passed} passed, ${failures.length} failed`);
if (failures.length > 0) {
  console.log(`failed: ${failures.join('; ')}`);
  process.exit(1);
}
