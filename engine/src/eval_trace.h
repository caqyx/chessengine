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

// Ingredients of the king danger score (see eval_king). Each has a weight in KingDanger[].
enum {
    KD_ATT_N, KD_ATT_B, KD_ATT_R, KD_ATT_Q,   // (number of attackers) x (attackers of this type)
    KD_ATTACKS,                               // attacked squares next to the king
    KD_WEAK,                                  // weak squares in the king ring
    KD_CHECK_R, KD_CHECK_Q, KD_CHECK_B, KD_CHECK_N,   // a safe checking square exists
    KD_NO_QUEEN,                              // attacker has no queen
    KD_CONST,
    KD_COUNT
};

#ifdef TUNE
struct KingTrace {
    i8 active;          // the danger model was evaluated for this king
    i16 x[KD_COUNT];
    i8 shelter2[16];    // twice the use count of ShelterBonus[0..7] then StormPenalty[0..7] (negative)
};
struct EvalTrace {
    KingTrace king[2];
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
