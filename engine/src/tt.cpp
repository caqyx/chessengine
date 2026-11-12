#include "tt.h"
#include "platform.h"

static TTBucket* Table = nullptr;
static size_t TableBytes = 0;
static u32 NumBuckets = 0;
static u8 Generation = 4;
static int Megabytes = 0;

int tt_megabytes() { return Megabytes; }

void tt_resize(int megabytes) {
    megabytes = iclamp(megabytes, 1, 4096);
    size_t bytes = (size_t)megabytes * 1024 * 1024;
    void* p = plat_big_realloc(Table, TableBytes, bytes);
    if (!p) return;   // keep the old table if the allocation failed
    Table = (TTBucket*)p;
    TableBytes = bytes;
    NumBuckets = (u32)(bytes / sizeof(TTBucket));
    Megabytes = megabytes;
    tt_clear();
}

void tt_clear() {
    if (Table) __builtin_memset((void*)Table, 0, (size_t)NumBuckets * sizeof(TTBucket));
    Generation = 4;
}

void tt_new_search() {
    Generation = (u8)(Generation + 4);
    if (Generation == 0) Generation = 4;
}

static inline TTBucket* bucket_for(u64 key) {
    return &Table[(u32)(((key >> 32) * (u64)NumBuckets) >> 32)];
}

void tt_prefetch(u64 key) { __builtin_prefetch(bucket_for(key)); }

TTEntry* tt_probe(u64 key, bool& hit) {
    TTBucket* b = bucket_for(key);
    u16 k = (u16)key;
    for (int i = 0; i < 3; i++) {
        if (b->e[i].key == k && b->e[i].genBound) {
            b->e[i].genBound = (u8)(Generation | (b->e[i].genBound & 3));   // refresh age
            hit = true;
            return &b->e[i];
        }
    }
    hit = false;
    // Pick the least valuable entry: shallow and old go first.
    TTEntry* repl = &b->e[0];
    int worst = 1 << 30;
    for (int i = 0; i < 3; i++) {
        TTEntry* e = &b->e[i];
        if (!e->genBound) return e;
        int age = ((Generation - (e->genBound & 0xFC)) & 0xFC) >> 2;
        int worth = e->depth - 8 * age;
        if (worth < worst) { worst = worth; repl = e; }
    }
    return repl;
}

void tt_store(TTEntry* e, u64 key, Move m, int score, int eval, int depth, int bound) {
    u16 k = (u16)key;
    bool same = e->key == k && e->genBound;
    if (m != MOVE_NONE || !same) e->move = m;
    if (bound == BOUND_EXACT || !same || depth + 4 > e->depth) {
        e->key = k;
        e->score = (i16)score;
        e->eval = (i16)eval;
        e->depth = (u8)depth;
        e->genBound = (u8)(Generation | bound);
    }
}

int tt_hashfull() {
    int cnt = 0;
    u32 n = imin<u32>(NumBuckets, 334);
    for (u32 i = 0; i < n; i++)
        for (int j = 0; j < 3; j++)
            if (Table[i].e[j].genBound && (Table[i].e[j].genBound & 0xFC) == Generation) cnt++;
    return n ? cnt * 1000 / (int)(n * 3) : 0;
}
