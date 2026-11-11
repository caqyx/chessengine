#include "movegen.h"

static inline Move* add_targets(Move* list, int from, Bitboard targets, int flag) {
    while (targets) *list++ = make_move(from, pop_lsb(targets), flag);
    return list;
}

static inline Move* add_promos(Move* list, int from, int to, bool capture, GenType type) {
    int base = capture ? MF_PROMO_CAPTURE : MF_PROMO;
    // Queen promotions and every capturing promotion count as noisy.
    if (type != GEN_QUIET) *list++ = make_move(from, to, base + 3);
    if (capture ? type != GEN_QUIET : type != GEN_NOISY) {
        *list++ = make_move(from, to, base + 0);
        *list++ = make_move(from, to, base + 2);
        *list++ = make_move(from, to, base + 1);
    }
    return list;
}

int generate(const Position& pos, Move* list, GenType type) {
    Move* start = list;
    const int us = pos.stm, them = !us;
    const Bitboard occ = pos.all(), empty = ~occ;
    const Bitboard enemies = pos.byColor[them];
    const int ksq = pos.king_sq(us);
    const Bitboard checkers = pos.st->checkers;
    const bool noisy = type != GEN_QUIET, quiet = type != GEN_NOISY;

    // When in check by one piece, non-king moves must capture it or block.
    Bitboard evasion = ~0ULL;
    if (checkers) {
        if (more_than_one(checkers)) evasion = 0;
        else evasion = Between[ksq][lsb(checkers)] | checkers;
    }

    if (evasion) {
        // Pawns
        const Bitboard pawns = pos.pieces(us, PAWN);
        const Bitboard rank7 = us == WHITE ? RankBB[6] : RankBB[1];
        const Bitboard rank3 = us == WHITE ? RankBB[2] : RankBB[5];
        const int up = us == WHITE ? 8 : -8;
        const Bitboard promoters = pawns & rank7, others = pawns & ~rank7;

        if (quiet) {
            Bitboard single = shift_up(others, us) & empty;
            Bitboard dbl = shift_up(single & rank3, us) & empty & evasion;
            single &= evasion;
            while (single) { int to = pop_lsb(single); *list++ = make_move(to - up, to, MF_QUIET); }
            while (dbl) { int to = pop_lsb(dbl); *list++ = make_move(to - 2 * up, to, MF_DOUBLE); }
        }
        if (promoters) {
            Bitboard push = shift_up(promoters, us) & empty & evasion;
            while (push) { int to = pop_lsb(push); list = add_promos(list, to - up, to, false, type); }
            Bitboard b = promoters;
            while (b) {
                int from = pop_lsb(b);
                Bitboard caps = PawnAttacks[us][from] & enemies & evasion;
                while (caps) list = add_promos(list, from, pop_lsb(caps), true, type);
            }
        }
        if (noisy) {
            Bitboard b = others;
            Bitboard attackable = enemies & evasion;
            // Only loop over pawns that can actually capture something.
            b &= pawn_attacks_bb(attackable, them);
            while (b) {
                int from = pop_lsb(b);
                list = add_targets(list, from, PawnAttacks[us][from] & attackable, MF_CAPTURE);
            }
            if (pos.st->ep != SQ_NONE) {
                int ep = pos.st->ep, capSq = ep - up;
                if (!checkers || (checkers & bit(capSq)) || (evasion & bit(ep))) {
                    Bitboard from = PawnAttacks[them][ep] & others;
                    while (from) *list++ = make_move(pop_lsb(from), ep, MF_EP);
                }
            }
        }

        // Knights, bishops, rooks, queens
        const Bitboard capT = noisy ? enemies & evasion : 0;
        const Bitboard quietT = quiet ? empty & evasion : 0;
        Bitboard b = pos.pieces(us, KNIGHT);
        while (b) {
            int from = pop_lsb(b);
            Bitboard a = KnightAttacks[from];
            list = add_targets(list, from, a & capT, MF_CAPTURE);
            list = add_targets(list, from, a & quietT, MF_QUIET);
        }
        b = pos.pieces(us, BISHOP);
        while (b) {
            int from = pop_lsb(b);
            Bitboard a = bishop_attacks(from, occ);
            list = add_targets(list, from, a & capT, MF_CAPTURE);
            list = add_targets(list, from, a & quietT, MF_QUIET);
        }
        b = pos.pieces(us, ROOK);
        while (b) {
            int from = pop_lsb(b);
            Bitboard a = rook_attacks(from, occ);
            list = add_targets(list, from, a & capT, MF_CAPTURE);
            list = add_targets(list, from, a & quietT, MF_QUIET);
        }
        b = pos.pieces(us, QUEEN);
        while (b) {
            int from = pop_lsb(b);
            Bitboard a = queen_attacks(from, occ);
            list = add_targets(list, from, a & capT, MF_CAPTURE);
            list = add_targets(list, from, a & quietT, MF_QUIET);
        }
    }

    // King
    Bitboard ka = KingAttacks[ksq];
    if (noisy) list = add_targets(list, ksq, ka & enemies, MF_CAPTURE);
    if (quiet) {
        list = add_targets(list, ksq, ka & empty, MF_QUIET);
        if (!checkers) {
            int home = us == WHITE ? 4 : 60;
            int rights = pos.st->castling;
            if ((rights & (us == WHITE ? WK_CASTLE : BK_CASTLE)) && !(occ & (bit(home + 1) | bit(home + 2))))
                *list++ = make_move(home, home + 2, MF_KCASTLE);
            if ((rights & (us == WHITE ? WQ_CASTLE : BQ_CASTLE)) && !(occ & (bit(home - 1) | bit(home - 2) | bit(home - 3))))
                *list++ = make_move(home, home - 2, MF_QCASTLE);
        }
    }
    return (int)(list - start);
}
