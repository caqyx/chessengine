#pragma once
#include "types.h"

// 10-byte entry, three per 32-byte bucket.
struct TTEntry {
    u16 key;
    Move move;
    i16 score;
    i16 eval;
    u8 depth;
    u8 genBound;   // generation (upper 6 bits) | bound (lower 2 bits); 0 = empty
    int bound() const { return genBound & 3; }
};

struct TTBucket {
    TTEntry e[3];
    u16 pad;
};

void tt_resize(int megabytes);
int tt_megabytes();
void tt_clear();
void tt_new_search();
// Returns the matching entry or null. *slot always receives the entry to write to.
TTEntry* tt_probe(u64 key, bool& hit);
void tt_store(TTEntry* e, u64 key, Move m, int score, int eval, int depth, int bound);
int tt_hashfull();
void tt_prefetch(u64 key);

inline int score_to_tt(int s, int ply) {
    return s >= VALUE_MATE_IN_MAX ? s + ply : s <= -VALUE_MATE_IN_MAX ? s - ply : s;
}
inline int score_from_tt(int s, int ply) {
    return s >= VALUE_MATE_IN_MAX ? s - ply : s <= -VALUE_MATE_IN_MAX ? s + ply : s;
}
