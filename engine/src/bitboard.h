#pragma once
#include "types.h"

struct Magic {
    Bitboard mask;
    u64 magic;
    Bitboard* attacks;
    int shift;
};

extern Bitboard PawnAttacks[2][64], KnightAttacks[64], KingAttacks[64];
extern Bitboard RookPseudo[64], BishopPseudo[64];
extern Bitboard Between[64][64];      // squares strictly between two aligned squares
extern Bitboard Line[64][64];         // full line through two aligned squares
extern Bitboard FileBB[8], RankBB[8], AdjacentFiles[8];
extern Bitboard ForwardFile[2][64];   // squares in front of a pawn on its file
extern Bitboard PassedMask[2][64];    // front span on own + adjacent files
extern u8 SquareDistance[64][64];
extern Magic RookMagics[64], BishopMagics[64];

void bitboards_init();

inline Bitboard bit(int s) { return 1ULL << s; }
inline int lsb(Bitboard b) { return __builtin_ctzll(b); }
inline int msb(Bitboard b) { return 63 - __builtin_clzll(b); }
inline int popcount(Bitboard b) { return __builtin_popcountll(b); }
inline int pop_lsb(Bitboard& b) { int s = lsb(b); b &= b - 1; return s; }
inline bool more_than_one(Bitboard b) { return b & (b - 1); }

inline Bitboard rook_attacks(int s, Bitboard occ) {
    const Magic& m = RookMagics[s];
    return m.attacks[((occ & m.mask) * m.magic) >> m.shift];
}
inline Bitboard bishop_attacks(int s, Bitboard occ) {
    const Magic& m = BishopMagics[s];
    return m.attacks[((occ & m.mask) * m.magic) >> m.shift];
}
inline Bitboard queen_attacks(int s, Bitboard occ) { return rook_attacks(s, occ) | bishop_attacks(s, occ); }

const Bitboard FILE_A = 0x0101010101010101ULL, FILE_H = 0x8080808080808080ULL;
const Bitboard RANK_1 = 0xFFULL, RANK_8 = 0xFF00000000000000ULL;
const Bitboard DARK_SQUARES = 0xAA55AA55AA55AA55ULL;

inline Bitboard shift_up(Bitboard b, int c) { return c == WHITE ? b << 8 : b >> 8; }
inline Bitboard pawn_attacks_bb(Bitboard pawns, int c) {
    return c == WHITE ? ((pawns & ~FILE_A) << 7) | ((pawns & ~FILE_H) << 9)
                      : ((pawns & ~FILE_A) >> 9) | ((pawns & ~FILE_H) >> 7);
}
