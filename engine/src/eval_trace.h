// Feature bookkeeping for evaluation tuning (only active when built with -DTUNE).
// Each tunable weight has an index; the trace records how often it was applied
// for White minus Black in the last evaluate() call.
#pragma once
#include "types.h"

enum {
    F_PSQ = 0,                       // 6 * 64
    F_MOB_N = F_PSQ + 384,           // 9
    F_MOB_B = F_MOB_N + 9,           // 14
    F_MOB_R = F_MOB_B + 14,          // 15
    F_MOB_Q = F_MOB_R + 15,          // 28
    F_CONNECTED = F_MOB_Q + 28,      // 8
    F_PASSED_RANK = F_CONNECTED + 8, // 8
    F_BISHOP_PAIR = F_PASSED_RANK + 8,
    F_ISOLATED, F_DOUBLED, F_ROOK_OPEN, F_ROOK_SEMI, F_KNIGHT_OUTPOST, F_BISHOP_OUTPOST,
    F_BISHOP_PAWNS, F_THREAT_PAWN, F_THREAT_MINOR, F_THREAT_ROOK, F_HANGING,
    F_COUNT
};

#ifdef TUNE
struct EvalTrace {
    i16 coeff[F_COUNT];
    int mg, eg;         // final middlegame / endgame score, White minus Black
    int phase, scale;   // 0..24 and 0..64
    int halfmove;
};
extern EvalTrace Trace;
#define TRACE(index, color, n) (Trace.coeff[index] = (i16)(Trace.coeff[index] + ((color) == WHITE ? (n) : -(n))))
#else
#define TRACE(index, color, n) ((void)0)
#endif
