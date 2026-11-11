// UCI protocol front end. Everything here is synchronous: `go` returns after
// printing `bestmove`. The native and wasm entry points decide how to feed it.
#include "uci.h"
#include "search.h"
#include "movegen.h"
#include "eval.h"
#include "tt.h"
#include "platform.h"

#ifndef DEFAULT_HASH_MB
#define DEFAULT_HASH_MB 64
#endif

static Position Pos;
static const char* StartFen = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

void engine_init() {
    bitboards_init();
    position_init();
    eval_init();
    search_init();
    tt_resize(DEFAULT_HASH_MB);
    Pos.set(StartFen);
}

// ---- tiny tokenizer ---------------------------------------------------------
struct Tokens {
    const char* p;
    char tok[128];
    explicit Tokens(const char* s) : p(s) { tok[0] = 0; }
    bool next() {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (!*p) { tok[0] = 0; return false; }
        int n = 0;
        while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') {
            if (n < 127) tok[n++] = *p;
            p++;
        }
        tok[n] = 0;
        return true;
    }
};

static bool str_eq(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}
static char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }
static bool str_ieq(const char* a, const char* b) {
    while (*a && lower(*a) == lower(*b)) { a++; b++; }
    return lower(*a) == lower(*b);
}
static i64 to_int(const char* s) {
    bool neg = *s == '-';
    if (neg || *s == '+') s++;
    i64 v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return neg ? -v : v;
}

static Move parse_move(const Position& pos, const char* s) {
    Move list[MAX_MOVES];
    int n = generate(pos, list, GEN_ALL);
    for (int i = 0; i < n; i++) {
        StrBuf b;
        b.add_move(list[i]);
        if (str_ieq(b.b, s) && pos.legal(list[i])) return list[i];
    }
    return MOVE_NONE;
}

// ---- commands ---------------------------------------------------------------
static void cmd_position(Tokens& t) {
    if (!t.next()) return;
    if (str_eq(t.tok, "startpos")) {
        Pos.set(StartFen);
        t.next();
    } else if (str_eq(t.tok, "fen")) {
        StrBuf fen;
        while (t.next() && !str_eq(t.tok, "moves")) { fen.add(t.tok); fen.add_char(' '); }
        if (!Pos.set(fen.b)) { Pos.set(StartFen); plat_out("info string invalid fen"); return; }
    } else return;
    if (!str_eq(t.tok, "moves")) return;
    while (t.next()) {
        Move m = parse_move(Pos, t.tok);
        if (m == MOVE_NONE) { plat_out("info string illegal move in position command"); break; }
        Pos.make(m);
        Pos.compact_history();
    }
}

static void cmd_setoption(Tokens& t) {
    StrBuf name, value;
    bool inValue = false;
    while (t.next()) {
        if (str_eq(t.tok, "name")) continue;
        if (str_eq(t.tok, "value")) { inValue = true; continue; }
        StrBuf& b = inValue ? value : name;
        if (b.n) b.add_char(' ');
        b.add(t.tok);
    }
    if (str_ieq(name.b, "Hash")) tt_resize((int)to_int(value.b));
    else if (str_ieq(name.b, "Level")) search_set_level((int)to_int(value.b));
    else if (str_ieq(name.b, "Move Overhead")) MoveOverhead = iclamp((int)to_int(value.b), 0, 5000);
    else if (str_ieq(name.b, "Opening Variety")) OpeningVariety = str_ieq(value.b, "true");
    else if (str_ieq(name.b, "EvalMask")) { EvalMask = (int)to_int(value.b); eval_clear(); tt_clear(); }
    else if (str_ieq(name.b, "SearchMask")) SearchMask = (int)to_int(value.b);
    else if (str_ieq(name.b, "Clear Hash")) { tt_clear(); search_clear(); eval_clear(); }
}

static void cmd_go(Tokens& t) {
    Limits lim;
    lim.depth = 0; lim.nodes = 0; lim.movetime = 0; lim.movestogo = 0; lim.infinite = false;
    lim.time[0] = lim.time[1] = lim.inc[0] = lim.inc[1] = 0;
    while (t.next()) {
        if (str_eq(t.tok, "infinite")) lim.infinite = true;
        else if (str_eq(t.tok, "depth")) { if (t.next()) lim.depth = (int)to_int(t.tok); }
        else if (str_eq(t.tok, "nodes")) { if (t.next()) lim.nodes = (u64)to_int(t.tok); }
        else if (str_eq(t.tok, "movetime")) { if (t.next()) lim.movetime = (int)imax<i64>(1, to_int(t.tok)); }
        else if (str_eq(t.tok, "wtime")) { if (t.next()) lim.time[WHITE] = (int)imax<i64>(1, to_int(t.tok)); }
        else if (str_eq(t.tok, "btime")) { if (t.next()) lim.time[BLACK] = (int)imax<i64>(1, to_int(t.tok)); }
        else if (str_eq(t.tok, "winc")) { if (t.next()) lim.inc[WHITE] = (int)imax<i64>(0, to_int(t.tok)); }
        else if (str_eq(t.tok, "binc")) { if (t.next()) lim.inc[BLACK] = (int)imax<i64>(0, to_int(t.tok)); }
        else if (str_eq(t.tok, "movestogo")) { if (t.next()) lim.movestogo = (int)to_int(t.tok); }
    }
    SearchResult r = search_go(Pos, lim, true);
    StrBuf b;
    b.add("bestmove "); b.add_move(r.best);
    if (r.best != MOVE_NONE && r.ponder != MOVE_NONE) { b.add(" ponder "); b.add_move(r.ponder); }
    plat_out(b.b);
}

static void cmd_perft(Tokens& t) {
    int depth = t.next() ? (int)to_int(t.tok) : 5;
    u64 start = plat_now_ms();
    u64 nodes = depth > 0 ? perft(Pos, depth) : 1;
    u64 ms = plat_now_ms() - start;
    StrBuf b;
    b.add("perft "); b.add_int(depth); b.add(" nodes "); b.add_int((i64)nodes);
    b.add(" time "); b.add_int((i64)ms);
    b.add(" nps "); b.add_int((i64)(nodes * 1000 / (ms ? ms : 1)));
    plat_out(b.b);
}

static const char* BenchFens[] = {
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
    "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
    "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
    "r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10",
    "r1bq1rk1/pp2ppbp/2np1np1/8/3NP3/2N1BP2/PPPQ2PP/R3KB1R w KQ - 3 9",
    "2r3k1/pp3ppp/2n1b3/3pP3/3P4/1P3N2/P4PPP/2R3K1 b - - 0 22",
    "8/8/4k3/3p4/3P1K2/8/6P1/8 w - - 0 1",
    "r2q1rk1/1b1nbppp/pp1ppn2/8/2PNP3/1PN3P1/PB3PBP/R2Q1RK1 w - - 0 13",
    "3r1rk1/p1q2ppp/1pn1pn2/2p5/2PP4/P1PBPN2/5PPP/R2Q1RK1 w - - 0 14",
    "8/5pk1/6p1/1p5p/1P3P1P/6P1/5K2/8 w - - 0 40",
    "r1b2rk1/2q1bppp/p2p1n2/np2p3/3PP3/2P2N1P/PPBN1PP1/R1BQR1K1 b - - 0 12",
    "6k1/5pp1/7p/8/1R6/6PP/r4P1K/8 w - - 0 35",
};

static void cmd_bench(Tokens& t) {
    int depth = t.next() ? (int)to_int(t.tok) : 12;
    u64 total = 0, start = plat_now_ms();
    Limits lim;
    lim.depth = depth; lim.nodes = 0; lim.movetime = 0; lim.movestogo = 0; lim.infinite = false;
    lim.time[0] = lim.time[1] = lim.inc[0] = lim.inc[1] = 0;
    tt_clear(); search_clear(); eval_clear();
    const bool variety = OpeningVariety;
    OpeningVariety = false;   // bench must be reproducible
    for (unsigned i = 0; i < sizeof(BenchFens) / sizeof(BenchFens[0]); i++) {
        Pos.set(BenchFens[i]);
        SearchStop = false;
        SearchResult r = search_go(Pos, lim, false);
        total += r.nodes;
        StrBuf b;
        b.add("bench "); b.add_int(i + 1); b.add(" best "); b.add_move(r.best);
        b.add(" score "); b.add_int(r.score); b.add(" nodes "); b.add_int((i64)r.nodes);
        plat_out(b.b);
    }
    u64 ms = plat_now_ms() - start;
    StrBuf b;
    b.add("bench total nodes "); b.add_int((i64)total); b.add(" time "); b.add_int((i64)ms);
    b.add(" nps "); b.add_int((i64)(total * 1000 / (ms ? ms : 1)));
    plat_out(b.b);
    OpeningVariety = variety;
    tt_clear();
    Pos.set(StartFen);
}

// Cross-checks pseudo_legal()/gives_check() against the move generator on a random walk.
static void cmd_selftest() {
    u64 seed = 88172645463325252ULL, errors = 0, checked = 0;
    for (int game = 0; game < 300; game++) {
        Pos.set(BenchFens[game % 12]);
        for (int ply = 0; ply < 120; ply++) {
            Move list[MAX_MOVES], legal[MAX_MOVES];
            int n = generate(Pos, list, GEN_ALL), nl = 0;
            for (int i = 0; i < n; i++) {
                if (!Pos.pseudo_legal(list[i])) errors++;
                if (Pos.legal(list[i])) legal[nl++] = list[i];
            }
            for (int k = 0; k < 400; k++) {
                seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
                Move m = (Move)(seed >> 20);
                bool gen = false;
                for (int i = 0; i < n; i++) if (list[i] == m) gen = true;
                if (Pos.pseudo_legal(m) != gen) errors++;
                checked++;
            }
            if (!nl) break;
            seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
            Move m = legal[(seed >> 33) % (u64)nl];
            bool gc = Pos.gives_check(m);
            u64 keyBefore = Pos.st->key;
            Pos.make(m);
            if (gc != Pos.in_check()) errors++;
            char fen[128];
            Pos.fen(fen);
            Position* copy = &Pos;   // key must match a fresh setup of the same position
            u64 k1 = copy->st->key;
            static Position Fresh;
            Fresh.set(fen);
            if (Fresh.st->key != k1 || Fresh.st->pawnKey != Pos.st->pawnKey || Fresh.st->psq != Pos.st->psq) errors++;
            Pos.unmake(m);
            if (Pos.st->key != keyBefore) errors++;
            Pos.make(m);
            Pos.compact_history();
            if (Pos.is_draw(0)) break;
        }
    }
    StrBuf b;
    b.add("selftest checked "); b.add_int((i64)checked); b.add(" errors "); b.add_int((i64)errors);
    plat_out(b.b);
    Pos.set(StartFen);
}

void uci_command(const char* line) {
    Tokens t(line);
    if (!t.next()) return;
    if (str_eq(t.tok, "uci")) {
        plat_out("id name " ENGINE_NAME " " ENGINE_VERSION);
        plat_out("id author Szymon Rogalski (caqyx) and Claude");
        StrBuf b;
        b.add("option name Hash type spin default "); b.add_int(DEFAULT_HASH_MB); b.add(" min 1 max 4096");
        plat_out(b.b);
        plat_out("option name Level type spin default 10 min 1 max 10");
        plat_out("option name Move Overhead type spin default 10 min 0 max 5000");
        plat_out(OpeningVariety ? "option name Opening Variety type check default true"
                                : "option name Opening Variety type check default false");
        plat_out("option name Clear Hash type button");
        plat_out("option name EvalMask type spin default 31 min 0 max 31");
        plat_out("option name SearchMask type spin default 255 min 0 max 255");
        plat_out("uciok");
    } else if (str_eq(t.tok, "isready")) plat_out("readyok");
    else if (str_eq(t.tok, "ucinewgame")) { tt_clear(); search_clear(); eval_clear(); }
    else if (str_eq(t.tok, "setoption")) cmd_setoption(t);
    else if (str_eq(t.tok, "position")) cmd_position(t);
    else if (str_eq(t.tok, "go")) cmd_go(t);
    else if (str_eq(t.tok, "perft")) cmd_perft(t);
    else if (str_eq(t.tok, "bench")) cmd_bench(t);
    else if (str_eq(t.tok, "selftest")) cmd_selftest();
    else if (str_eq(t.tok, "eval")) {
        StrBuf b;
        b.add("eval "); b.add_int(evaluate(Pos)); b.add(" cp (side to move)");
        plat_out(b.b);
    } else if (str_eq(t.tok, "d")) {
        char fen[128];
        Pos.fen(fen);
        StrBuf b;
        b.add("fen "); b.add(fen);
        plat_out(b.b);
    }
}
