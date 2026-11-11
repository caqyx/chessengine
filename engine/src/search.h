#pragma once
#include "position.h"

struct Limits {
    int depth;          // 0 = no limit
    u64 nodes;          // 0 = no limit
    int movetime;       // ms, 0 = not used
    int time[2], inc[2];
    int movestogo;
    bool infinite;
};

struct SearchResult {
    Move best, ponder;
    int score, depth;
    u64 nodes;
    u64 timeMs;
};

extern volatile bool SearchStop;
extern int MoveOverhead;
extern bool ShowWdl;          // add "wdl W D L" (per-mille, side to move) to info lines
extern bool OpeningVariety;   // small eval noise in the first moves so games differ
extern int SearchMask;   // debug switch for A/B testing search features

#ifdef TUNE_SEARCH
int search_param_count();
const char* search_param_name(int i);
int search_param_value(int i);
void search_param_set(int i, int value);
#endif

void search_init();
void search_clear();                 // forget everything learned (new game)
bool search_set_level(int level);    // 1..10, 10 = full strength; returns true if it changed
SearchResult search_go(Position& pos, const Limits& lim, bool printInfo);

// Tiny string builder: the engine core has no libc.
struct StrBuf {
    char b[2048];
    int n;
    StrBuf() : n(0) { b[0] = 0; }
    void add(const char* s) { while (*s && n < 2047) b[n++] = *s++; b[n] = 0; }
    void add_char(char c) { if (n < 2047) { b[n++] = c; b[n] = 0; } }
    void add_int(i64 v) {
        char t[24]; int len = 0;
        bool neg = v < 0;
        u64 u = neg ? (u64)(-(v + 1)) + 1 : (u64)v;
        do { t[len++] = (char)('0' + u % 10); u /= 10; } while (u);
        if (neg) add_char('-');
        while (len) add_char(t[--len]);
    }
    void add_move(Move m) {
        if (m == MOVE_NONE || m == MOVE_NULL) { add("0000"); return; }
        int f = from_sq(m), t = to_sq(m);
        add_char((char)('a' + file_of(f))); add_char((char)('1' + rank_of(f)));
        add_char((char)('a' + file_of(t))); add_char((char)('1' + rank_of(t)));
        if (is_promo(m)) add_char("nbrq"[promo_type(m) - KNIGHT]);
    }
};
