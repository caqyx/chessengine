// Tessera chess engine - basic types and helpers.
// The engine core is freestanding C++ (no libc / libstdc++), so the same
// sources build as a native UCI binary and as a bare WebAssembly module.
#pragma once
#include <stdint.h>
#include <stddef.h>

typedef uint64_t u64; typedef uint32_t u32; typedef uint16_t u16; typedef uint8_t u8;
typedef int64_t i64;  typedef int32_t i32;  typedef int16_t i16;  typedef int8_t i8;
typedef u64 Bitboard;
typedef u16 Move;

#define ENGINE_NAME "Tessera"
#define ENGINE_VERSION "1.0"

enum { WHITE, BLACK };
enum { PAWN, KNIGHT, BISHOP, ROOK, QUEEN, KING, PT_NB, PT_ALL = 6 };
enum { NO_PIECE = 12 };
enum { SQ_NONE = 64 };
enum { MAX_PLY = 128, MAX_MOVES = 256, MAX_HIST = 1400 };
enum {
    VALUE_DRAW = 0,
    VALUE_MATE = 32000,
    VALUE_INF = 32001,
    VALUE_NONE = 32002,
    VALUE_MATE_IN_MAX = VALUE_MATE - 2 * MAX_PLY,   // |score| >= this means "mate found"
};
enum { BOUND_NONE, BOUND_UPPER, BOUND_LOWER, BOUND_EXACT };
enum { WK_CASTLE = 1, WQ_CASTLE = 2, BK_CASTLE = 4, BQ_CASTLE = 8 };

// Move = from(6) | to(6) | flag(4)
enum {
    MF_QUIET = 0, MF_DOUBLE = 1, MF_KCASTLE = 2, MF_QCASTLE = 3,
    MF_CAPTURE = 4, MF_EP = 5,
    MF_PROMO = 8,           // 8..11  = promote to N,B,R,Q
    MF_PROMO_CAPTURE = 12,  // 12..15 = capture + promote to N,B,R,Q
};
enum : Move { MOVE_NONE = 0, MOVE_NULL = 65 };

inline Move make_move(int from, int to, int flag) { return (Move)(from | (to << 6) | (flag << 12)); }
inline int from_sq(Move m) { return m & 63; }
inline int to_sq(Move m) { return (m >> 6) & 63; }
inline int move_flag(Move m) { return m >> 12; }
inline bool is_capture(Move m) { return (m >> 12) & 4; }
inline bool is_promo(Move m) { return (m >> 12) & 8; }
inline bool is_ep(Move m) { return (m >> 12) == MF_EP; }
inline bool is_castle(Move m) { return (m >> 12) == MF_KCASTLE || (m >> 12) == MF_QCASTLE; }
inline int promo_type(Move m) { return KNIGHT + ((m >> 12) & 3); }
// "Noisy" moves are searched in quiescence: captures and queen promotions.
inline bool is_noisy(Move m) { int f = m >> 12; return (f & 4) || f == MF_PROMO + 3; }

inline int make_piece(int c, int pt) { return c * 6 + pt; }
inline int piece_type(int pc) { return pc >= 6 ? pc - 6 : pc; }
inline int piece_color(int pc) { return pc >= 6; }
inline int file_of(int s) { return s & 7; }
inline int rank_of(int s) { return s >> 3; }
inline int relative_rank(int c, int s) { return (s >> 3) ^ (c * 7); }
inline int make_sq(int f, int r) { return r * 8 + f; }

template <class T> inline T imin(T a, T b) { return a < b ? a : b; }
template <class T> inline T imax(T a, T b) { return a > b ? a : b; }
template <class T> inline T iclamp(T v, T lo, T hi) { return v < lo ? lo : v > hi ? hi : v; }
inline int iabs(int v) { return v < 0 ? -v : v; }

// Packed (middlegame, endgame) score pair.
#define S(mg, eg) ((int)((unsigned int)(eg) << 16) + (mg))
inline int eg_of(int s) { return (i16)(u16)((unsigned)(s + 0x8000) >> 16); }
inline int mg_of(int s) { return (i16)(u16)(unsigned)s; }

inline int mate_in(int ply) { return VALUE_MATE - ply; }
inline int mated_in(int ply) { return -VALUE_MATE + ply; }
