#include "position.h"
#include "movegen.h"

static u64 ZPiece[13][64], ZCastle[16], ZEp[8], ZSide;
static u8 CastleMask[64];
int PSQ[12][64];
const int SeeValue[13] = {100, 325, 325, 500, 975, 0, 100, 325, 325, 500, 975, 0, 0};
const int PhaseWeight[6] = {0, 1, 1, 2, 4, 0};

static const int MatMG[6] = {82, 337, 365, 477, 1025, 0};
static const int MatEG[6] = {94, 281, 297, 512, 936, 0};

// Piece-square tables: PeSTO (Ronald Friederich), written from rank 8 down to rank 1.
static const i16 PstMG[6][64] = {
    {0, 0, 0, 0, 0, 0, 0, 0,
     98, 134, 61, 95, 68, 126, 34, -11,
     -6, 7, 26, 31, 65, 56, 25, -20,
     -14, 13, 6, 21, 23, 12, 17, -23,
     -27, -2, -5, 12, 17, 6, 10, -25,
     -26, -4, -4, -10, 3, 3, 33, -12,
     -35, -1, -20, -23, -15, 24, 38, -22,
     0, 0, 0, 0, 0, 0, 0, 0},
    {-167, -89, -34, -49, 61, -97, -15, -107,
     -73, -41, 72, 36, 23, 62, 7, -17,
     -47, 60, 37, 65, 84, 129, 73, 44,
     -9, 17, 19, 53, 37, 69, 18, 22,
     -13, 4, 16, 13, 28, 19, 21, -8,
     -23, -9, 12, 10, 19, 17, 25, -16,
     -29, -53, -12, -3, -1, 18, -14, -19,
     -105, -21, -58, -33, -17, -28, -19, -23},
    {-29, 4, -82, -37, -25, -42, 7, -8,
     -26, 16, -18, -13, 30, 59, 18, -47,
     -16, 37, 43, 40, 35, 50, 37, -2,
     -4, 5, 19, 50, 37, 37, 7, -2,
     -6, 13, 13, 26, 34, 12, 10, 4,
     0, 15, 15, 15, 14, 27, 18, 10,
     4, 15, 16, 0, 7, 21, 33, 1,
     -33, -3, -14, -21, -13, -12, -39, -21},
    {32, 42, 32, 51, 63, 9, 31, 43,
     27, 32, 58, 62, 80, 67, 26, 44,
     -5, 19, 26, 36, 17, 45, 61, 16,
     -24, -11, 7, 26, 24, 35, -8, -20,
     -36, -26, -12, -1, 9, -7, 6, -23,
     -45, -25, -16, -17, 3, 0, -5, -33,
     -44, -16, -20, -9, -1, 11, -6, -71,
     -19, -13, 1, 17, 16, 7, -37, -26},
    {-28, 0, 29, 12, 59, 44, 43, 45,
     -24, -39, -5, 1, -16, 57, 28, 54,
     -13, -17, 7, 8, 29, 56, 47, 57,
     -27, -27, -16, -16, -1, 17, -2, 1,
     -9, -26, -9, -10, -2, -4, 3, -3,
     -14, 2, -11, -2, -5, 2, 14, 5,
     -35, -8, 11, 2, 8, 15, -3, 1,
     -1, -18, -9, 10, -15, -25, -31, -50},
    {-65, 23, 16, -15, -56, -34, 2, 13,
     29, -1, -20, -7, -8, -4, -38, -29,
     -9, 24, 2, -16, -20, 6, 22, -22,
     -17, -20, -12, -27, -30, -25, -14, -36,
     -49, -1, -27, -39, -46, -44, -33, -51,
     -14, -14, -22, -46, -44, -30, -15, -27,
     1, 7, -8, -64, -43, -16, 9, 8,
     -15, 36, 12, -54, 8, -28, 24, 14},
};
static const i16 PstEG[6][64] = {
    {0, 0, 0, 0, 0, 0, 0, 0,
     178, 173, 158, 134, 147, 132, 165, 187,
     94, 100, 85, 67, 56, 53, 82, 84,
     32, 24, 13, 5, -2, 4, 17, 17,
     13, 9, -3, -7, -7, -8, 3, -1,
     4, 7, -6, 1, 0, -5, -1, -8,
     13, 8, 8, 10, 13, 0, 2, -7,
     0, 0, 0, 0, 0, 0, 0, 0},
    {-58, -38, -13, -28, -31, -27, -63, -99,
     -25, -8, -25, -2, -9, -25, -24, -52,
     -24, -20, 10, 9, -1, -9, -19, -41,
     -17, 3, 22, 22, 22, 11, 8, -18,
     -18, -6, 16, 25, 16, 17, 4, -18,
     -23, -3, -1, 15, 10, -3, -20, -22,
     -42, -20, -10, -5, -2, -20, -23, -44,
     -29, -51, -23, -15, -22, -18, -50, -64},
    {-14, -21, -11, -8, -7, -9, -17, -24,
     -8, -4, 7, -12, -3, -13, -4, -14,
     2, -8, 0, -1, -2, 6, 0, 4,
     -3, 9, 12, 9, 14, 10, 3, 2,
     -6, 3, 13, 19, 7, 10, -3, -9,
     -12, -3, 8, 10, 13, 3, -7, -15,
     -14, -18, -7, -1, 4, -9, -15, -27,
     -23, -9, -23, -5, -9, -16, -5, -17},
    {13, 10, 18, 15, 12, 12, 8, 5,
     11, 13, 13, 11, -3, 3, 8, 3,
     7, 7, 7, 5, 4, -3, -5, -3,
     4, 3, 13, 1, 2, 1, -1, 2,
     3, 5, 8, 4, -5, -6, -8, -11,
     -4, 0, -5, -1, -7, -12, -8, -16,
     -6, -6, 0, 2, -9, -9, -11, -3,
     -9, 2, 3, -1, -5, -13, 4, -20},
    {-9, 22, 22, 27, 27, 19, 10, 20,
     -17, 20, 32, 41, 58, 25, 30, 0,
     -20, 6, 9, 49, 47, 35, 19, 9,
     3, 22, 24, 45, 57, 40, 57, 36,
     -18, 28, 19, 47, 31, 34, 39, 23,
     -16, -27, 15, 6, 9, 17, 10, 5,
     -22, -23, -30, -16, -16, -23, -36, -32,
     -33, -28, -22, -43, -5, -32, -20, -41},
    {-74, -35, -18, -18, -11, 15, 4, -17,
     -12, 17, 14, 17, 17, 38, 23, 11,
     10, 17, 23, 15, 20, 45, 44, 13,
     -8, 22, 24, 27, 26, 33, 26, 3,
     -18, -4, 21, 24, 27, 23, 9, -11,
     -19, -3, 11, 21, 23, 16, 7, -9,
     -27, -11, 4, 13, 14, 4, -5, -17,
     -53, -34, -21, -11, -28, -14, -24, -43},
};

static u64 splitmix(u64& s) {
    u64 z = (s += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

void position_init() {
    u64 seed = 0x7E55E7A5EEDULL;
    for (int p = 0; p < 12; p++)
        for (int s = 0; s < 64; s++) ZPiece[p][s] = splitmix(seed);
    for (int s = 0; s < 64; s++) ZPiece[12][s] = 0;
    for (int i = 0; i < 16; i++) ZCastle[i] = splitmix(seed);
    for (int i = 0; i < 8; i++) ZEp[i] = splitmix(seed);
    ZSide = splitmix(seed);

    for (int pt = 0; pt < 6; pt++)
        for (int s = 0; s < 64; s++) {
            int w = S(MatMG[pt] + PstMG[pt][s ^ 56], MatEG[pt] + PstEG[pt][s ^ 56]);
            int b = S(MatMG[pt] + PstMG[pt][s], MatEG[pt] + PstEG[pt][s]);
            PSQ[pt][s] = w;
            PSQ[6 + pt][s] = -b;
        }
    for (int s = 0; s < 64; s++) CastleMask[s] = 15;
    CastleMask[0] = 15 & ~WQ_CASTLE; CastleMask[7] = 15 & ~WK_CASTLE; CastleMask[4] = 15 & ~(WK_CASTLE | WQ_CASTLE);
    CastleMask[56] = 15 & ~BQ_CASTLE; CastleMask[63] = 15 & ~BK_CASTLE; CastleMask[60] = 15 & ~(BK_CASTLE | BQ_CASTLE);
}

void Position::put(int pc, int sq) {
    board[sq] = (u8)pc;
    byType[piece_type(pc)] |= bit(sq);
    byColor[piece_color(pc)] |= bit(sq);
}
void Position::remove(int sq) {
    int pc = board[sq];
    byType[piece_type(pc)] ^= bit(sq);
    byColor[piece_color(pc)] ^= bit(sq);
    board[sq] = NO_PIECE;
}
void Position::move_piece(int from, int to) {
    int pc = board[from];
    Bitboard ft = bit(from) | bit(to);
    byType[piece_type(pc)] ^= ft;
    byColor[piece_color(pc)] ^= ft;
    board[from] = NO_PIECE;
    board[to] = (u8)pc;
}

Bitboard Position::attackers_to(int sq, Bitboard occ) const {
    return (PawnAttacks[BLACK][sq] & pieces(WHITE, PAWN))
         | (PawnAttacks[WHITE][sq] & pieces(BLACK, PAWN))
         | (KnightAttacks[sq] & byType[KNIGHT])
         | (rook_attacks(sq, occ) & (byType[ROOK] | byType[QUEEN]))
         | (bishop_attacks(sq, occ) & (byType[BISHOP] | byType[QUEEN]))
         | (KingAttacks[sq] & byType[KING]);
}

Bitboard Position::slider_blockers(int ksq, int kingColor) const {
    Bitboard snipers = ((RookPseudo[ksq] & (byType[ROOK] | byType[QUEEN]))
                      | (BishopPseudo[ksq] & (byType[BISHOP] | byType[QUEEN]))) & byColor[!kingColor];
    Bitboard occ = all() ^ snipers, blockers = 0;
    while (snipers) {
        int s = pop_lsb(snipers);
        Bitboard b = Between[ksq][s] & occ;
        if (b && !more_than_one(b)) blockers |= b;
    }
    return blockers;
}

void Position::update_check_info() {
    st->checkers = attackers_to(king_sq(stm), all()) & byColor[!stm];
    st->blockers[WHITE] = slider_blockers(king_sq(WHITE), WHITE);
    st->blockers[BLACK] = slider_blockers(king_sq(BLACK), BLACK);
}

bool Position::set(const char* f) {
    for (int i = 0; i < 6; i++) byType[i] = 0;
    byColor[0] = byColor[1] = 0;
    for (int i = 0; i < 64; i++) board[i] = NO_PIECE;
    st = states;
    State* s = st;
    s->key = s->pawnKey = 0; s->checkers = 0; s->blockers[0] = s->blockers[1] = 0;
    s->psq = 0; s->npm[0] = s->npm[1] = 0; s->phase = 0; s->halfmove = 0; s->pliesFromNull = 0;
    s->castling = 0; s->ep = SQ_NONE; s->captured = NO_PIECE; s->move = MOVE_NONE;
    stm = WHITE; fullmove = 1;

    while (*f == ' ') f++;
    int sq = 56;
    static const char* PieceChars = "PNBRQKpnbrqk";
    for (; *f && *f != ' '; f++) {
        char c = *f;
        if (c == '/') sq -= 16;
        else if (c >= '1' && c <= '8') sq += c - '0';
        else {
            int pc = -1;
            for (int i = 0; i < 12; i++) if (PieceChars[i] == c) pc = i;
            if (pc < 0 || sq < 0 || sq > 63) return false;
            put(pc, sq++);
        }
    }
    if (popcount(pieces(WHITE, KING)) != 1 || popcount(pieces(BLACK, KING)) != 1) return false;
    if (byType[PAWN] & (RANK_1 | RANK_8)) return false;

    while (*f == ' ') f++;
    if (*f == 'b') stm = BLACK;
    if (*f) f++;
    while (*f == ' ') f++;
    for (; *f && *f != ' '; f++) {
        if (*f == 'K' && board[4] == make_piece(WHITE, KING) && board[7] == make_piece(WHITE, ROOK)) s->castling |= WK_CASTLE;
        if (*f == 'Q' && board[4] == make_piece(WHITE, KING) && board[0] == make_piece(WHITE, ROOK)) s->castling |= WQ_CASTLE;
        if (*f == 'k' && board[60] == make_piece(BLACK, KING) && board[63] == make_piece(BLACK, ROOK)) s->castling |= BK_CASTLE;
        if (*f == 'q' && board[60] == make_piece(BLACK, KING) && board[56] == make_piece(BLACK, ROOK)) s->castling |= BQ_CASTLE;
    }
    while (*f == ' ') f++;
    if (*f >= 'a' && *f <= 'h' && f[1] >= '1' && f[1] <= '8') {
        int e = make_sq(*f - 'a', f[1] - '1');
        int capSq = stm == WHITE ? e - 8 : e + 8;
        if (relative_rank(stm, e) == 5 && board[capSq] == make_piece(!stm, PAWN) && board[e] == NO_PIECE
            && (PawnAttacks[!stm][e] & pieces(stm, PAWN)))
            s->ep = (u8)e;
        f += 2;
    } else if (*f) f++;
    while (*f == ' ') f++;
    int n = 0; bool any = false;
    for (; *f >= '0' && *f <= '9'; f++) { n = n * 10 + (*f - '0'); any = true; }
    if (any) s->halfmove = imin(n, 200);
    while (*f == ' ') f++;
    n = 0; any = false;
    for (; *f >= '0' && *f <= '9'; f++) { n = n * 10 + (*f - '0'); any = true; }
    if (any && n > 0) fullmove = n;

    for (int i = 0; i < 64; i++) {
        int pc = board[i];
        if (pc == NO_PIECE) continue;
        int pt = piece_type(pc);
        s->key ^= ZPiece[pc][i];
        s->psq += PSQ[pc][i];
        if (pt == PAWN) s->pawnKey ^= ZPiece[pc][i];
        else if (pt != KING) { s->npm[piece_color(pc)] += MatMG[pt]; s->phase += PhaseWeight[pt]; }
    }
    if (stm == BLACK) s->key ^= ZSide;
    s->key ^= ZCastle[s->castling];
    if (s->ep != SQ_NONE) s->key ^= ZEp[file_of(s->ep)];
    update_check_info();
    // The side that just moved must not be left in check.
    if (attackers_to(king_sq(!stm), all()) & byColor[stm]) return false;
    return true;
}

void Position::fen(char* out) const {
    static const char* PieceChars = "PNBRQKpnbrqk";
    char* p = out;
    for (int r = 7; r >= 0; r--) {
        int empty = 0;
        for (int f = 0; f < 8; f++) {
            int pc = board[make_sq(f, r)];
            if (pc == NO_PIECE) { empty++; continue; }
            if (empty) { *p++ = (char)('0' + empty); empty = 0; }
            *p++ = PieceChars[pc];
        }
        if (empty) *p++ = (char)('0' + empty);
        if (r) *p++ = '/';
    }
    *p++ = ' '; *p++ = stm == WHITE ? 'w' : 'b'; *p++ = ' ';
    if (!st->castling) *p++ = '-';
    else {
        if (st->castling & WK_CASTLE) *p++ = 'K';
        if (st->castling & WQ_CASTLE) *p++ = 'Q';
        if (st->castling & BK_CASTLE) *p++ = 'k';
        if (st->castling & BQ_CASTLE) *p++ = 'q';
    }
    *p++ = ' ';
    if (st->ep == SQ_NONE) *p++ = '-';
    else { *p++ = (char)('a' + file_of(st->ep)); *p++ = (char)('1' + rank_of(st->ep)); }
    *p++ = ' ';
    int nums[2] = {st->halfmove, fullmove};
    for (int k = 0; k < 2; k++) {
        char tmp[12]; int len = 0, v = nums[k];
        do { tmp[len++] = (char)('0' + v % 10); v /= 10; } while (v);
        while (len) *p++ = tmp[--len];
        if (k == 0) *p++ = ' ';
    }
    *p = 0;
}

void Position::make(Move m) {
    State* n = st + 1;
    *n = *st;
    st = n;
    int from = from_sq(m), to = to_sq(m), flag = move_flag(m);
    int us = stm, them = !us;
    int pc = board[from], pt = piece_type(pc);
    u64 key = n->key ^ ZSide;
    n->halfmove++;
    n->pliesFromNull++;
    if (n->ep != SQ_NONE) { key ^= ZEp[file_of(n->ep)]; n->ep = SQ_NONE; }

    int captured = NO_PIECE;
    if (flag & 4) {
        int capSq = flag == MF_EP ? (us == WHITE ? to - 8 : to + 8) : to;
        captured = board[capSq];
        int cpt = piece_type(captured);
        remove(capSq);
        key ^= ZPiece[captured][capSq];
        n->psq -= PSQ[captured][capSq];
        if (cpt == PAWN) n->pawnKey ^= ZPiece[captured][capSq];
        else { n->npm[them] -= MatMG[cpt]; n->phase -= PhaseWeight[cpt]; }
        n->halfmove = 0;
    }

    move_piece(from, to);
    key ^= ZPiece[pc][from] ^ ZPiece[pc][to];
    n->psq += PSQ[pc][to] - PSQ[pc][from];

    if (pt == PAWN) {
        n->halfmove = 0;
        n->pawnKey ^= ZPiece[pc][from] ^ ZPiece[pc][to];
        if (flag == MF_DOUBLE) {
            int epSq = (from + to) >> 1;
            if (PawnAttacks[us][epSq] & pieces(them, PAWN)) { n->ep = (u8)epSq; key ^= ZEp[file_of(epSq)]; }
        } else if (flag & 8) {
            int ppt = promo_type(m), promo = make_piece(us, ppt);
            remove(to);
            put(promo, to);
            key ^= ZPiece[pc][to] ^ ZPiece[promo][to];
            n->pawnKey ^= ZPiece[pc][to];
            n->psq += PSQ[promo][to] - PSQ[pc][to];
            n->npm[us] += MatMG[ppt];
            n->phase += PhaseWeight[ppt];
        }
    } else if (flag == MF_KCASTLE || flag == MF_QCASTLE) {
        int rf = flag == MF_KCASTLE ? to + 1 : to - 2;
        int rt = flag == MF_KCASTLE ? to - 1 : to + 1;
        int rook = make_piece(us, ROOK);
        move_piece(rf, rt);
        key ^= ZPiece[rook][rf] ^ ZPiece[rook][rt];
        n->psq += PSQ[rook][rt] - PSQ[rook][rf];
    }

    int nc = n->castling & CastleMask[from] & CastleMask[to];
    if (nc != n->castling) { key ^= ZCastle[n->castling] ^ ZCastle[nc]; n->castling = (u8)nc; }
    n->captured = (u8)captured;
    n->move = m;
    n->key = key;
    stm = them;
    if (us == BLACK) fullmove++;
    update_check_info();
}

void Position::unmake(Move m) {
    stm = !stm;
    int us = stm;
    int from = from_sq(m), to = to_sq(m), flag = move_flag(m);
    if (flag & 8) { remove(to); put(make_piece(us, PAWN), to); }
    else if (flag == MF_KCASTLE) move_piece(to - 1, to + 1);
    else if (flag == MF_QCASTLE) move_piece(to + 1, to - 2);
    move_piece(to, from);
    if (flag & 4) {
        int capSq = flag == MF_EP ? (us == WHITE ? to - 8 : to + 8) : to;
        put(st->captured, capSq);
    }
    st--;
    if (us == BLACK) fullmove--;
}

void Position::make_null() {
    State* n = st + 1;
    *n = *st;
    st = n;
    n->key ^= ZSide;
    if (n->ep != SQ_NONE) { n->key ^= ZEp[file_of(n->ep)]; n->ep = SQ_NONE; }
    n->halfmove++;
    n->pliesFromNull = 0;
    n->captured = NO_PIECE;
    n->move = MOVE_NULL;
    n->checkers = 0;
    stm = !stm;
}

void Position::unmake_null() {
    st--;
    stm = !stm;
}

bool Position::pseudo_legal(Move m) const {
    if (m == MOVE_NONE || m == MOVE_NULL) return false;
    int from = from_sq(m), to = to_sq(m), flag = move_flag(m);
    int pc = board[from];
    if (pc == NO_PIECE || piece_color(pc) != stm) return false;
    int us = stm, them = !us, pt = piece_type(pc);
    Bitboard toBB = bit(to), occ = all();
    if (byColor[us] & toBB) return false;
    if (flag == 6 || flag == 7) return false;
    bool cap = flag & 4;

    if (pt != PAWN) {
        if (flag == MF_KCASTLE || flag == MF_QCASTLE) {
            if (pt != KING || st->checkers) return false;
            int home = us == WHITE ? 4 : 60;
            if (from != home) return false;
            if (flag == MF_KCASTLE)
                return to == home + 2 && (st->castling & (us == WHITE ? WK_CASTLE : BK_CASTLE))
                    && !(occ & (bit(home + 1) | bit(home + 2)));
            return to == home - 2 && (st->castling & (us == WHITE ? WQ_CASTLE : BQ_CASTLE))
                && !(occ & (bit(home - 1) | bit(home - 2) | bit(home - 3)));
        }
        if (flag != MF_QUIET && flag != MF_CAPTURE) return false;
        if (cap != (board[to] != NO_PIECE)) return false;
        Bitboard att = pt == KNIGHT ? KnightAttacks[from]
                     : pt == BISHOP ? bishop_attacks(from, occ)
                     : pt == ROOK ? rook_attacks(from, occ)
                     : pt == QUEEN ? queen_attacks(from, occ)
                     : KingAttacks[from];
        if (!(att & toBB)) return false;
    } else {
        int up = us == WHITE ? 8 : -8;
        if (((flag & 8) != 0) != (relative_rank(us, to) == 7)) return false;
        if (flag == MF_EP) {
            if (to != st->ep || !(PawnAttacks[us][from] & toBB)) return false;
        } else if (cap) {
            if (!(PawnAttacks[us][from] & toBB & byColor[them])) return false;
        } else if (flag == MF_DOUBLE) {
            if (relative_rank(us, from) != 1 || to != from + 2 * up) return false;
            if (occ & (bit(from + up) | toBB)) return false;
        } else {
            if (flag != MF_QUIET && !(flag & 8)) return false;
            if (to != from + up || (occ & toBB)) return false;
        }
    }

    if (st->checkers) {
        if (pt == KING) return true;
        if (more_than_one(st->checkers)) return false;
        Bitboard target = Between[king_sq(us)][lsb(st->checkers)] | st->checkers;
        if (flag == MF_EP) {
            int capSq = us == WHITE ? to - 8 : to + 8;
            return (st->checkers & bit(capSq)) || (target & toBB);
        }
        return (target & toBB) != 0;
    }
    return true;
}

bool Position::legal(Move m) const {
    int us = stm, them = !us;
    int from = from_sq(m), to = to_sq(m), flag = move_flag(m);
    int ksq = king_sq(us);
    if (flag == MF_EP) {
        int capSq = us == WHITE ? to - 8 : to + 8;
        Bitboard occ = (all() ^ bit(from) ^ bit(capSq)) | bit(to);
        return !(rook_attacks(ksq, occ) & byColor[them] & (byType[ROOK] | byType[QUEEN]))
            && !(bishop_attacks(ksq, occ) & byColor[them] & (byType[BISHOP] | byType[QUEEN]));
    }
    if (from == ksq) {
        if (flag == MF_KCASTLE || flag == MF_QCASTLE) {
            int step = flag == MF_KCASTLE ? 1 : -1;
            for (int s = from + step; s != to + step; s += step)
                if (attackers_to(s, all()) & byColor[them]) return false;
            return true;
        }
        return !(attackers_to(to, all() ^ bit(from)) & byColor[them]);
    }
    return !(st->blockers[us] & bit(from)) || (Line[from][to] & bit(ksq));
}

bool Position::gives_check(Move m) const {
    int us = stm, them = !us;
    int from = from_sq(m), to = to_sq(m), flag = move_flag(m);
    int ksq = king_sq(them);
    Bitboard kbb = bit(ksq), occ = all();
    int pt = (flag & 8) ? promo_type(m) : piece_type(board[from]);
    Bitboard occAfter = (occ ^ bit(from)) | bit(to);
    switch (pt) {
        case PAWN: if (PawnAttacks[us][to] & kbb) return true; break;
        case KNIGHT: if (KnightAttacks[to] & kbb) return true; break;
        case BISHOP: if ((BishopPseudo[to] & kbb) && (bishop_attacks(to, occAfter) & kbb)) return true; break;
        case ROOK: if ((RookPseudo[to] & kbb) && (rook_attacks(to, occAfter) & kbb)) return true; break;
        case QUEEN: if (((RookPseudo[to] | BishopPseudo[to]) & kbb) && (queen_attacks(to, occAfter) & kbb)) return true; break;
        default: break;
    }
    if ((st->blockers[them] & bit(from)) && !(Line[from][to] & kbb)) return true;
    if (flag == MF_EP) {
        int capSq = us == WHITE ? to - 8 : to + 8;
        Bitboard o = (occ ^ bit(from) ^ bit(capSq)) | bit(to);
        return (rook_attacks(ksq, o) & byColor[us] & (byType[ROOK] | byType[QUEEN]))
            || (bishop_attacks(ksq, o) & byColor[us] & (byType[BISHOP] | byType[QUEEN]));
    }
    if (flag == MF_KCASTLE || flag == MF_QCASTLE) {
        int rf = flag == MF_KCASTLE ? to + 1 : to - 2;
        int rt = flag == MF_KCASTLE ? to - 1 : to + 1;
        Bitboard o = (occ ^ bit(from) ^ bit(rf)) | bit(to) | bit(rt);
        return (rook_attacks(rt, o) & kbb) != 0;
    }
    return false;
}

// Static exchange evaluation: is the capture sequence on the target square
// worth at least `threshold` for the side making the move?
bool Position::see_ge(Move m, int threshold) const {
    int flag = move_flag(m);
    if (flag == MF_EP || (flag & 8) || flag == MF_KCASTLE || flag == MF_QCASTLE) return 0 >= threshold;
    int from = from_sq(m), to = to_sq(m);
    int swap = SeeValue[board[to]] - threshold;
    if (swap < 0) return false;
    swap = SeeValue[board[from]] - swap;
    if (swap <= 0) return true;

    Bitboard occ = all() ^ bit(from) ^ bit(to);
    int side = stm;
    Bitboard attackers = attackers_to(to, occ);
    Bitboard diag = byType[BISHOP] | byType[QUEEN], straight = byType[ROOK] | byType[QUEEN];
    int res = 1;
    for (;;) {
        side = !side;
        attackers &= occ;
        Bitboard mine = attackers & byColor[side];
        if (!mine) break;
        res ^= 1;
        Bitboard b;
        if ((b = mine & byType[PAWN])) {
            if ((swap = SeeValue[PAWN] - swap) < res) break;
            occ ^= b & -b;
            attackers |= bishop_attacks(to, occ) & diag;
        } else if ((b = mine & byType[KNIGHT])) {
            if ((swap = SeeValue[KNIGHT] - swap) < res) break;
            occ ^= b & -b;
        } else if ((b = mine & byType[BISHOP])) {
            if ((swap = SeeValue[BISHOP] - swap) < res) break;
            occ ^= b & -b;
            attackers |= bishop_attacks(to, occ) & diag;
        } else if ((b = mine & byType[ROOK])) {
            if ((swap = SeeValue[ROOK] - swap) < res) break;
            occ ^= b & -b;
            attackers |= rook_attacks(to, occ) & straight;
        } else if ((b = mine & byType[QUEEN])) {
            if ((swap = SeeValue[QUEEN] - swap) < res) break;
            occ ^= b & -b;
            attackers |= (bishop_attacks(to, occ) & diag) | (rook_attacks(to, occ) & straight);
        } else {
            // King: it can only capture if the other side has no attackers left.
            return (attackers & ~byColor[side]) ? res ^ 1 : res;
        }
    }
    return res;
}

bool Position::has_legal_move() const {
    Move list[MAX_MOVES];
    int n = generate(*this, list, GEN_ALL);
    for (int i = 0; i < n; i++)
        if (legal(list[i])) return true;
    return false;
}

bool Position::is_draw(int ply) const {
    if (st->halfmove >= 100) return !st->checkers || has_legal_move();
    int end = imin(st->halfmove, st->pliesFromNull);
    end = imin(end, (int)(st - states));
    int cnt = 0;
    for (int i = 4; i <= end; i += 2) {
        if ((st - i)->key == st->key) {
            if (i < ply) return true;      // repeated inside the search tree
            if (++cnt == 2) return true;   // real threefold including game history
        }
    }
    return false;
}

void Position::compact_history() {
    int used = (int)(st - states);
    if (used < MAX_HIST - MAX_PLY - 80) return;
    int keep = imin(imin(st->halfmove, used), 220);
    State* src = st - keep;
    for (int i = 0; i <= keep; i++) states[i] = src[i];
    st = states + keep;
}

u64 perft(Position& pos, int depth) {
    Move list[MAX_MOVES];
    int n = generate(pos, list, GEN_ALL);
    u64 nodes = 0;
    for (int i = 0; i < n; i++) {
        if (!pos.legal(list[i])) continue;
        if (depth <= 1) { nodes++; continue; }
        pos.make(list[i]);
        nodes += perft(pos, depth - 1);
        pos.unmake(list[i]);
    }
    return nodes;
}
