// Hand-crafted evaluation.
//
// Score = material + piece-square tables (kept incrementally by Position)
//       + pawn structure (cached in a small pawn hash)
//       + piece activity (mobility, outposts, open files, bishop pair)
//       + king safety (pawn shelter + an "attack units" danger model)
//       + passed pawns + threats
// Middlegame and endgame scores are blended by game phase, then the endgame
// part is scaled down for drawish material.
#include "eval.h"
#include "eval_params.h"
#include "eval_trace.h"

#ifdef TUNE
EvalTrace Trace;
#endif

enum { ALL = PT_ALL };

// Debug switch for A/B testing evaluation terms (bit set = term enabled).
int EvalMask = 31;

static const int Tempo = 16;



struct PawnEntry {
    u64 key;
    int score;              // white minus black, packed
    Bitboard passed[2];
};
static PawnEntry PawnTable[1 << 14];

struct EvalInfo {
    Bitboard attackedBy[2][7];
    Bitboard attackedBy2[2];
    Bitboard mobilityArea[2];
    Bitboard kingRing[2];
    int kingAttackers[2];       // number of pieces of color c attacking the enemy king ring
    int kingAttackersByType[2][6];
    int kingAttacks[2];         // attacked squares right next to the enemy king
    int ksq[2];
};

void eval_init() {
    eval_clear();
}

void eval_clear() {
    for (int i = 0; i < (1 << 14); i++) {
        PawnTable[i].key = 0; PawnTable[i].score = 0;
        PawnTable[i].passed[0] = PawnTable[i].passed[1] = 0;
    }
}

static int eval_pawns_side(const Position& pos, int us, Bitboard& passedOut) {
    const int them = !us;
    const Bitboard ours = pos.pieces(us, PAWN), theirs = pos.pieces(them, PAWN);
    int score = 0;
    Bitboard passed = 0;
    Bitboard b = ours;
    while (b) {
        int sq = pop_lsb(b);
        int f = file_of(sq), r = relative_rank(us, sq);
        Bitboard neighbours = ours & AdjacentFiles[f];
        bool doubled = ours & ForwardFile[us][sq];
        bool supported = PawnAttacks[them][sq] & ours;
        bool phalanx = neighbours & RankBB[rank_of(sq)];
        if (!neighbours) { score += Isolated; TRACE(F_ISOLATED, us, 1); }
        if (doubled) { score += Doubled; TRACE(F_DOUBLED, us, 1); }
        if (supported || phalanx) { score += Connected[r]; TRACE(F_CONNECTED + r, us, 1); }
        if (!(PassedMask[us][sq] & theirs) && !doubled) passed |= bit(sq);
    }
    passedOut = passed;
    return score;
}

static const PawnEntry* probe_pawns(const Position& pos) {
    PawnEntry* e = &PawnTable[pos.st->pawnKey & 16383];
#ifndef TUNE
    if (e->key == pos.st->pawnKey) return e;
#endif
    e->key = pos.st->pawnKey;
    e->score = eval_pawns_side(pos, WHITE, e->passed[WHITE]) - eval_pawns_side(pos, BLACK, e->passed[BLACK]);
    return e;
}

static void init_info(const Position& pos, EvalInfo& ei, int us) {
    const int them = !us;
    const int ksq = pos.king_sq(us);
    const Bitboard pawns = pos.pieces(us, PAWN);
    ei.ksq[us] = ksq;
    Bitboard pa = pawn_attacks_bb(pawns, us);
    Bitboard pa2 = us == WHITE ? (((pawns & ~FILE_A) << 7) & ((pawns & ~FILE_H) << 9))
                               : (((pawns & ~FILE_A) >> 9) & ((pawns & ~FILE_H) >> 7));
    Bitboard ka = KingAttacks[ksq];
    ei.attackedBy[us][PAWN] = pa;
    ei.attackedBy[us][KING] = ka;
    ei.attackedBy[us][KNIGHT] = ei.attackedBy[us][BISHOP] = ei.attackedBy[us][ROOK] = ei.attackedBy[us][QUEEN] = 0;
    ei.attackedBy[us][ALL] = pa | ka;
    ei.attackedBy2[us] = pa2 | (pa & ka);

    Bitboard ring = ka | bit(ksq);
    if (relative_rank(us, ksq) == 0) ring |= shift_up(ring, us);
    ei.kingRing[us] = ring;
    ei.kingAttackers[us] = ei.kingAttacks[us] = 0;
    for (int pt = 0; pt < 6; pt++) ei.kingAttackersByType[us][pt] = 0;

    // Squares our pieces are not counted as mobile on: own king/queen,
    // own pawns that are blocked or still on the 2nd/3rd rank, enemy pawn attacks.
    Bitboard lowRanks = us == WHITE ? (RankBB[1] | RankBB[2]) : (RankBB[6] | RankBB[5]);
    Bitboard blocked = pawns & (shift_up(pos.all(), them) | lowRanks);
    ei.mobilityArea[us] = ~(blocked | pos.pieces(us, KING) | pos.pieces(us, QUEEN)
                            | pawn_attacks_bb(pos.pieces(them, PAWN), them));
    (void)them;
}

static int eval_pieces(const Position& pos, EvalInfo& ei, int us) {
    const int them = !us;
    const Bitboard occ = pos.all();
    const Bitboard ourPawns = pos.pieces(us, PAWN), theirPawns = pos.pieces(them, PAWN);
    const Bitboard outpostRanks = us == WHITE ? (RankBB[3] | RankBB[4] | RankBB[5]) : (RankBB[4] | RankBB[3] | RankBB[2]);
    const Bitboard theirKingZone = KingAttacks[pos.king_sq(them)];
    int score = 0;

    for (int pt = KNIGHT; pt <= QUEEN; pt++) {
        Bitboard b = pos.pieces(us, pt);
        while (b) {
            int sq = pop_lsb(b);
            Bitboard att;
            switch (pt) {
                case KNIGHT: att = KnightAttacks[sq]; break;
                case BISHOP: att = bishop_attacks(sq, occ ^ pos.pieces(QUEEN)); break;
                case ROOK: att = rook_attacks(sq, occ ^ pos.pieces(QUEEN) ^ pos.pieces(us, ROOK)); break;
                default: att = queen_attacks(sq, occ); break;
            }
            // A pinned piece only really attacks along the pin line.
            if (pos.st->blockers[us] & bit(sq)) att &= Line[ei.ksq[us]][sq];

            ei.attackedBy2[us] |= ei.attackedBy[us][ALL] & att;
            ei.attackedBy[us][pt] |= att;
            ei.attackedBy[us][ALL] |= att;

            if (att & ei.kingRing[them]) {
                ei.kingAttackers[us]++;
                ei.kingAttackersByType[us][pt]++;
                ei.kingAttacks[us] += popcount(att & theirKingZone);
            }

            int mob = popcount(att & ei.mobilityArea[us]);
            if (pt == KNIGHT) {
                score += MobilityN[mob];
                TRACE(F_MOB_N + mob, us, 1);
                if ((bit(sq) & outpostRanks & ei.attackedBy[us][PAWN])
                    && !(PassedMask[us][sq] & AdjacentFiles[file_of(sq)] & theirPawns)) {
                    score += KnightOutpost;
                    TRACE(F_KNIGHT_OUTPOST, us, 1);
                }
            } else if (pt == BISHOP) {
                score += MobilityB[mob];
                TRACE(F_MOB_B + mob, us, 1);
                if ((bit(sq) & outpostRanks & ei.attackedBy[us][PAWN])
                    && !(PassedMask[us][sq] & AdjacentFiles[file_of(sq)] & theirPawns)) {
                    score += BishopOutpost;
                    TRACE(F_BISHOP_OUTPOST, us, 1);
                }
                Bitboard sameColor = (DARK_SQUARES & bit(sq)) ? DARK_SQUARES : ~DARK_SQUARES;
                score += BishopPawns * popcount(ourPawns & sameColor);
                TRACE(F_BISHOP_PAWNS, us, popcount(ourPawns & sameColor));
            } else if (pt == ROOK) {
                score += MobilityR[mob];
                TRACE(F_MOB_R + mob, us, 1);
                Bitboard file = FileBB[file_of(sq)];
                if (!(file & ourPawns)) {
                    const bool semi = file & theirPawns;
                    score += semi ? RookSemiOpen : RookOpen;
                    TRACE(semi ? F_ROOK_SEMI : F_ROOK_OPEN, us, 1);
                }
            } else {
                score += MobilityQ[mob];
                TRACE(F_MOB_Q + mob, us, 1);
            }
        }
    }
    if (more_than_one(pos.pieces(us, BISHOP))) { score += BishopPair; TRACE(F_BISHOP_PAIR, us, 1); }
    return score;
}

static int shelter_score(const Position& pos, int us, int ksq) {
    const int them = !us;
    const Bitboard ourPawns = pos.pieces(us, PAWN), theirPawns = pos.pieces(them, PAWN);
    // Only pawns level with or in front of the king matter.
    Bitboard front = ~0ULL;
    int kr = rank_of(ksq);
    if (us == WHITE) front <<= 8 * kr; else front >>= 8 * (7 - kr);
    int center = iclamp(file_of(ksq), 1, 6);
    int v = 0;
    for (int f = center - 1; f <= center + 1; f++) {
        Bitboard ours = ourPawns & FileBB[f] & front;
        Bitboard theirs = theirPawns & FileBB[f] & front;
        int ourRank = ours ? relative_rank(us, us == WHITE ? lsb(ours) : msb(ours)) : 0;
        int theirRank = theirs ? relative_rank(us, us == WHITE ? lsb(theirs) : msb(theirs)) : 0;
        v += ShelterBonus[ourRank];
        const bool blocked = ourRank && theirRank == ourRank + 1;   // their pawn is stopped by ours
        v -= blocked ? StormPenalty[theirRank] / 2 : StormPenalty[theirRank];
#ifdef TUNE
        Trace.king[us].shelter2[ourRank] += 2;
        Trace.king[us].shelter2[8 + theirRank] -= blocked ? 1 : 2;
#endif
    }
    return v;
}

static int eval_king(const Position& pos, const EvalInfo& ei, int us) {
    const int them = !us;
    const int ksq = ei.ksq[us];
    int shelter = shelter_score(pos, us, ksq);
    int score = S(shelter, 0);

    if (ei.kingAttackers[them] == 0 && !pos.pieces(them, QUEEN)) return score;

    const Bitboard weak = ei.attackedBy[them][ALL] & ~ei.attackedBy2[us]
                        & (~ei.attackedBy[us][ALL] | ei.attackedBy[us][KING] | ei.attackedBy[us][QUEEN]);
    const Bitboard safe = ~pos.byColor[them] & (~ei.attackedBy[us][ALL] | (weak & ei.attackedBy2[them]));
    const Bitboard occ = pos.all() ^ pos.pieces(us, QUEEN);
    const Bitboard rookLines = rook_attacks(ksq, occ), bishopLines = bishop_attacks(ksq, occ);

    // Attack units: every ingredient of an attack adds to "danger" with its own weight.
    int x[KD_COUNT];
    x[KD_ATT_N] = ei.kingAttackers[them] * ei.kingAttackersByType[them][KNIGHT];
    x[KD_ATT_B] = ei.kingAttackers[them] * ei.kingAttackersByType[them][BISHOP];
    x[KD_ATT_R] = ei.kingAttackers[them] * ei.kingAttackersByType[them][ROOK];
    x[KD_ATT_Q] = ei.kingAttackers[them] * ei.kingAttackersByType[them][QUEEN];
    x[KD_ATTACKS] = ei.kingAttacks[them];
    x[KD_WEAK] = popcount(ei.kingRing[us] & weak);
    Bitboard rookChecks = rookLines & ei.attackedBy[them][ROOK] & safe;
    Bitboard queenChecks = (rookLines | bishopLines) & ei.attackedBy[them][QUEEN] & safe & ~ei.attackedBy[us][QUEEN] & ~rookChecks;
    Bitboard bishopChecks = bishopLines & ei.attackedBy[them][BISHOP] & safe & ~queenChecks;
    Bitboard knightChecks = KnightAttacks[ksq] & ei.attackedBy[them][KNIGHT] & safe;
    x[KD_CHECK_R] = rookChecks != 0;
    x[KD_CHECK_Q] = queenChecks != 0;
    x[KD_CHECK_B] = bishopChecks != 0;
    x[KD_CHECK_N] = knightChecks != 0;
    x[KD_NO_QUEEN] = !pos.pieces(them, QUEEN);
    x[KD_CONST] = 1;

    int danger = -shelter * 3 / 4;
    for (int k = 0; k < KD_COUNT; k++) danger += KingDanger[k] * x[k];
#ifdef TUNE
    Trace.king[us].active = 1;
    for (int k = 0; k < KD_COUNT; k++) Trace.king[us].x[k] = (i16)x[k];
#endif

    // The penalty grows with the square of the danger: one attacker is harmless, three are not.
    if (danger > 100) {
        danger = imin(danger, 2400);
        score -= S(danger * danger / 6400, danger * KingDangerEg / 256);
    }
    return score;
}

static int eval_threats(const Position& pos, const EvalInfo& ei, int us) {
    const int them = !us;
    const Bitboard theirPieces = pos.byColor[them] & ~pos.pieces(them, KING);
    const Bitboard nonPawns = theirPieces & ~pos.pieces(them, PAWN);
    const int byPawn = popcount(nonPawns & ei.attackedBy[us][PAWN]);
    const int minorOnMajor = popcount((pos.pieces(them, ROOK) | pos.pieces(them, QUEEN))
                                      & (ei.attackedBy[us][KNIGHT] | ei.attackedBy[us][BISHOP]));
    const int rookOnQueen = popcount(pos.pieces(them, QUEEN) & ei.attackedBy[us][ROOK]);
    const int hanging = popcount(theirPieces & ei.attackedBy[us][ALL] & ~ei.attackedBy[them][ALL]);
    TRACE(F_THREAT_PAWN, us, byPawn);
    TRACE(F_THREAT_MINOR, us, minorOnMajor);
    TRACE(F_THREAT_ROOK, us, rookOnQueen);
    TRACE(F_HANGING, us, hanging);
    int score = ThreatByPawn * byPawn + ThreatMinorOnMajor * minorOnMajor
              + ThreatRookOnQueen * rookOnQueen + Hanging * hanging;
    return score;
}

static int eval_passed(const Position& pos, const EvalInfo& ei, int us, Bitboard passed) {
    const int them = !us;
    const int up = us == WHITE ? 8 : -8;
    const Bitboard occ = pos.all();
    int score = 0;
    while (passed) {
        int sq = pop_lsb(passed);
        int r = relative_rank(us, sq);
        int bonus = PassedRank[r];
        TRACE(F_PASSED_RANK + r, us, 1);
        if (r >= 3) {
            int w = 5 * r - 13;
            int block = sq + up;
            int kd = imin<int>(SquareDistance[ei.ksq[them]][block], 5) * 5 - imin<int>(SquareDistance[ei.ksq[us]][block], 5) * 2;
            bonus += S(0, kd * w / 4);
            if (!(occ & bit(block))) {
                Bitboard path = ForwardFile[us][sq];
                Bitboard unsafe = path & (ei.attackedBy[them][ALL] | pos.byColor[them]);
                Bitboard behind = ForwardFile[them][sq] & (pos.pieces(ROOK) | pos.pieces(QUEEN)) & rook_attacks(sq, occ);
                if (behind & pos.byColor[them]) unsafe = path;
                int k = !unsafe ? 18 : !(unsafe & bit(block)) ? 10 : 4;
                if ((behind & pos.byColor[us]) || (ei.attackedBy[us][ALL] & bit(block))) k += 3;
                bonus += S(k * w / 4, k * w / 4);
            }
        }
        score += bonus;
    }
    return score;
}

// How much of the endgame score to keep (0..64) when the material is drawish.
static int scale_factor(const Position& pos, int eg) {
    const int strong = eg > 0 ? WHITE : BLACK, weak = !strong;
    const int strongPawns = popcount(pos.pieces(strong, PAWN));
    const int npmS = pos.st->npm[strong], npmW = pos.st->npm[weak];
    if (strongPawns == 0) {
        if (npmS - npmW <= 365) return npmS < 477 ? 0 : (npmW <= 365 ? 8 : 20);
        // Two knights cannot force mate against a bare king.
        if (npmS == 2 * 337 && !npmW && !pos.pieces(weak, PAWN) && popcount(pos.pieces(strong, KNIGHT)) == 2) return 0;
    }
    if (npmS == 365 && npmW == 365 && popcount(pos.pieces(BISHOP)) == 2
        && pos.pieces(WHITE, BISHOP) && pos.pieces(BLACK, BISHOP)) {
        bool wDark = pos.pieces(WHITE, BISHOP) & DARK_SQUARES, bDark = pos.pieces(BLACK, BISHOP) & DARK_SQUARES;
        if (wDark != bDark) return 30;   // opposite-colored bishops
    }
    return imin(64, 36 + 7 * strongPawns);
}

// Extra help for mating a bare king: push it to the edge, bring our king close.
static int mop_up(const Position& pos, int strong) {
    const int weak = !strong;
    int wk = pos.king_sq(weak), sk = pos.king_sq(strong);
    int f = file_of(wk), r = rank_of(wk);
    int centerDist = imax(3 - f, f - 4) + imax(3 - r, r - 4);
    int v = 12 * centerDist + 5 * (14 - (iabs(file_of(sk) - f) + iabs(rank_of(sk) - r)));
    // King + bishop + knight: the mate only works in a corner of the bishop's color.
    if (pos.st->npm[strong] == 337 + 365 && pos.pieces(strong, BISHOP) && pos.pieces(strong, KNIGHT)) {
        bool dark = pos.pieces(strong, BISHOP) & DARK_SQUARES;
        int c1 = dark ? 0 : 7, c2 = dark ? 63 : 56;
        int d1 = iabs(file_of(c1) - f) + iabs(rank_of(c1) - r), d2 = iabs(file_of(c2) - f) + iabs(rank_of(c2) - r);
        v += 25 * (14 - imin(d1, d2));
    }
    return v;
}

int evaluate(const Position& pos) {
    const State* st = pos.st;
    const Bitboard pawns = pos.pieces(PAWN);

    // Dead-draw material: bare kings, or a single minor piece.
    if (!pawns && st->npm[WHITE] + st->npm[BLACK] <= 365) return 0;

#ifdef TUNE
    __builtin_memset(&Trace, 0, sizeof(Trace));
    for (Bitboard b = pos.all(); b;) {
        int sq = pop_lsb(b), pc = pos.board[sq], c = piece_color(pc);
        TRACE(F_PSQ + piece_type(pc) * 64 + (c == WHITE ? sq ^ 56 : sq), c, 1);
    }
#endif
    EvalInfo ei;
    const PawnEntry* pe = probe_pawns(pos);
    int score = st->psq + pe->score;

    init_info(pos, ei, WHITE);
    init_info(pos, ei, BLACK);
    int pieces = eval_pieces(pos, ei, WHITE) - eval_pieces(pos, ei, BLACK);   // also fills the attack maps
    if (EvalMask & 1) score += pieces;
    if (EvalMask & 2) score += eval_king(pos, ei, WHITE) - eval_king(pos, ei, BLACK);
    if (EvalMask & 4) score += eval_threats(pos, ei, WHITE) - eval_threats(pos, ei, BLACK);
    if (EvalMask & 8) score += eval_passed(pos, ei, WHITE, pe->passed[WHITE]) - eval_passed(pos, ei, BLACK, pe->passed[BLACK]);
    if (!(EvalMask & 16)) score -= pe->score;

    int mg = mg_of(score), eg = eg_of(score);

    // Lone king against mating material and no pawns anywhere.
    if (!pawns) {
        if (!st->npm[BLACK] && st->npm[WHITE] >= 477) eg += mop_up(pos, WHITE);
        else if (!st->npm[WHITE] && st->npm[BLACK] >= 477) eg -= mop_up(pos, BLACK);
    }

    int phase = imin(st->phase, 24);
    int sf = scale_factor(pos, eg);
#ifdef TUNE
    Trace.mg = mg; Trace.eg = eg; Trace.phase = phase; Trace.scale = sf; Trace.halfmove = st->halfmove;
#endif
    int v = (mg * phase + eg * (24 - phase) * sf / 64) / 24;
    v = pos.stm == WHITE ? v : -v;
    v += Tempo;
    v = v * (200 - imin(st->halfmove, 100)) / 200;
    return iclamp(v, -VALUE_MATE_IN_MAX + 1, VALUE_MATE_IN_MAX - 1);
}
