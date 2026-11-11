// Search: iterative deepening + aspiration windows around a principal
// variation search (alpha-beta), with a transposition table, quiescence
// search, and the usual selectivity: null move, reverse futility, razoring,
// probcut, late move pruning/reductions, SEE pruning, singular extensions.
#include "search.h"
#include "movegen.h"
#include "eval.h"
#include "tt.h"
#include "platform.h"

#ifndef OPENING_VARIETY_DEFAULT
#define OPENING_VARIETY_DEFAULT false
#endif

#ifndef SHOW_WDL_DEFAULT
#define SHOW_WDL_DEFAULT false
#endif

volatile bool SearchStop = false;
bool ShowWdl = SHOW_WDL_DEFAULT;
int MoveOverhead = 10;
int SearchMask = 255;   // 1 check ext, 2 singular, 4 probcut, 8 correction history, 16 razoring, 32 rfp, 64 nmp, 128 lmp/futility

// Search thresholds. Normally compile-time constants; building with -DTUNE_SEARCH turns
// them into UCI options (SP_<name>) so that variants can be compared in matches.
#ifdef TUNE_SEARCH
#define SPARAM(name, value) static int name = value;
#else
#define SPARAM(name, value) static const int name = value;
#endif
SPARAM(RfpMargin, 42)
SPARAM(RazorMargin, 260)
SPARAM(NmpBase, 4)
SPARAM(NmpDiv, 3)
SPARAM(FutBase, 90)
SPARAM(FutMul, 80)
SPARAM(LmpBase, 3)
SPARAM(SeeQuiet, 25)
SPARAM(SeeNoisy, 100)
SPARAM(HistPrune, 3500)
SPARAM(LmrBase, 77)
SPARAM(LmrDiv, 236)
SPARAM(LmrHist, 6000)
SPARAM(ProbCutMargin, 180)
SPARAM(AspDelta, 12)
SPARAM(QsFutility, 180)
SPARAM(SingularMul, 2)
SPARAM(EvalScale, 100)      // percent applied to the static evaluation before the search uses it

static void init_lmr_table();

#ifdef TUNE_SEARCH
struct SearchParam { const char* name; int* value; };
static const SearchParam SearchParams[] = {
    {"RfpMargin", &RfpMargin},
    {"RazorMargin", &RazorMargin},
    {"NmpBase", &NmpBase},
    {"NmpDiv", &NmpDiv},
    {"FutBase", &FutBase},
    {"FutMul", &FutMul},
    {"LmpBase", &LmpBase},
    {"SeeQuiet", &SeeQuiet},
    {"SeeNoisy", &SeeNoisy},
    {"HistPrune", &HistPrune},
    {"LmrBase", &LmrBase},
    {"LmrDiv", &LmrDiv},
    {"LmrHist", &LmrHist},
    {"ProbCutMargin", &ProbCutMargin},
    {"AspDelta", &AspDelta},
    {"QsFutility", &QsFutility},
    {"SingularMul", &SingularMul},
    {"EvalScale", &EvalScale},
};
int search_param_count() { return (int)(sizeof(SearchParams) / sizeof(SearchParams[0])); }
const char* search_param_name(int i) { return SearchParams[i].name; }
int search_param_value(int i) { return *SearchParams[i].value; }
void search_param_set(int i, int value) { *SearchParams[i].value = value; init_lmr_table(); }
#endif

typedef i16 PieceToHist[12][64];

struct Stack {
    PieceToHist* ch;       // continuation history slice of the move played at this ply
    Move currentMove, excluded, killers[2];
    int staticEval;
    int ply;
    int doubleExt;
};

// ---- tables that persist between searches ---------------------------------
static i16 MainHist[2][64][64];
static i16 CaptHist[12][64][6];
static PieceToHist ContHist[13][64];     // [12] = "no previous move"
static Move CounterMoves[12][64];
static i16 PawnCorr[2][16384];           // static-eval correction by pawn structure
static int LmrTable[64][64];

// ---- per-search state -----------------------------------------------------
static Move PvTable[MAX_PLY + 2][MAX_PLY + 2];
static int PvLen[MAX_PLY + 2];
static u64 RootMoveNodes[4096];
static u64 Nodes;
static int SelDepth, RootDepth, NmpMinPly;
static u64 StartTime, HardLimit;
static u64 NodeLimit;
static Move RootBest;
static int Level = 10, LevelNoise = 0, LevelDepth = 0;
static int NoiseAmp = 0;          // noise actually used by the running search
bool OpeningVariety = OPENING_VARIETY_DEFAULT;
static u64 NoiseSeed = 0;

static double ln_approx(double x) {
    int k = 0;
    while (x > 2.0) { x *= 0.5; k++; }
    double y = (x - 1.0) / (x + 1.0), y2 = y * y, term = y, sum = 0;
    for (int i = 1; i < 40; i += 2) { sum += term / i; term *= y2; }
    return 2.0 * sum + k * 0.6931471805599453;
}

static void init_lmr_table() {
    for (int d = 0; d < 64; d++)
        for (int m = 0; m < 64; m++)
            LmrTable[d][m] = (d && m) ? (int)(LmrBase / 100.0 + ln_approx(d) * ln_approx(m) / (LmrDiv / 100.0)) : 0;
}

void search_init() {
    init_lmr_table();
    search_clear();
}

void search_clear() {
    __builtin_memset(MainHist, 0, sizeof(MainHist));
    __builtin_memset(CaptHist, 0, sizeof(CaptHist));
    __builtin_memset(ContHist, 0, sizeof(ContHist));
    __builtin_memset(CounterMoves, 0, sizeof(CounterMoves));
    __builtin_memset(PawnCorr, 0, sizeof(PawnCorr));
}

bool search_set_level(int level) {
    static const int Depths[11] = {0, 1, 2, 3, 4, 5, 6, 8, 10, 13, 0};
    static const int Noise[11] = {0, 260, 200, 150, 110, 80, 55, 35, 20, 8, 0};
    const int before = Level;
    Level = iclamp(level, 1, 10);
    LevelDepth = Depths[Level];
    LevelNoise = Noise[Level];
    return Level != before;
}

// ---- helpers --------------------------------------------------------------
static inline void hist_update(i16& h, int bonus) { h = (i16)(h + bonus - h * iabs(bonus) / 16384); }
static inline int stat_bonus(int depth) { return imin(150 * depth - 50, 1700); }

static inline int captured_type(const Position& pos, Move m) {
    if (is_ep(m)) return PAWN;
    int c = pos.board[to_sq(m)];
    return c == NO_PIECE ? KING : piece_type(c);
}

static inline int quiet_history(const Position& pos, const Stack* ss, Move m) {
    int pc = pos.board[from_sq(m)], to = to_sq(m);
    return MainHist[pos.stm][from_sq(m)][to] + (*(ss - 1)->ch)[pc][to] + (*(ss - 2)->ch)[pc][to] + (*(ss - 4)->ch)[pc][to] / 2;
}

static inline int raw_eval(const Position& pos) {
    int v = (NnueActive ? nnue_evaluate(pos) : evaluate(pos)) * EvalScale / 100;
    if (NoiseAmp) {   // weakened levels: deterministic per-position noise
        u64 h = (pos.st->key ^ NoiseSeed) * 0x9E3779B97F4A7C15ULL;
        v += (int)((h >> 40) % (u64)(2 * NoiseAmp + 1)) - NoiseAmp;
    }
    return v;
}

static inline int corrected_eval(const Position& pos, int raw) {
    int v = raw + ((SearchMask & 8) ? PawnCorr[pos.stm][pos.st->pawnKey & 16383] / 8 : 0);
    return iclamp(v, -VALUE_MATE_IN_MAX + 1, VALUE_MATE_IN_MAX - 1);
}

static inline void check_stop() {
    if (NodeLimit && Nodes >= NodeLimit) SearchStop = true;
    if ((Nodes & 1023) == 0 && HardLimit && RootDepth > 1 && plat_now_ms() - StartTime >= HardLimit) SearchStop = true;
}

// ---- move ordering --------------------------------------------------------
struct ScoredMove { Move m; int score; };

enum { ST_TT, ST_GEN_NOISY, ST_GOOD_NOISY, ST_KILLER1, ST_KILLER2, ST_COUNTER, ST_GEN_QUIET, ST_QUIET, ST_BAD_NOISY, ST_DONE };

struct MovePicker {
    const Position& pos;
    const Stack* ss;
    Move ttMove, k1, k2, counter;
    ScoredMove list[MAX_MOVES];
    int cur, end, badEnd, stage;
    bool noisyOnly;

    MovePicker(const Position& p, const Stack* s, Move tt, bool onlyNoisy)
        : pos(p), ss(s), ttMove(tt), cur(0), end(0), badEnd(0), stage(ST_TT), noisyOnly(onlyNoisy) {
        k1 = k2 = counter = MOVE_NONE;
        if (!onlyNoisy) {
            k1 = s->killers[0]; k2 = s->killers[1];
            Move prev = (s - 1)->currentMove;
            if (prev != MOVE_NONE && prev != MOVE_NULL) counter = CounterMoves[pos.board[to_sq(prev)]][to_sq(prev)];
        }
    }

    void pick_best() {
        int best = cur;
        for (int i = cur + 1; i < end; i++)
            if (list[i].score > list[best].score) best = i;
        ScoredMove t = list[cur]; list[cur] = list[best]; list[best] = t;
    }

    Move next(bool skipQuiets) {
        Move buf[MAX_MOVES];
        switch (stage) {
        case ST_TT:
            stage = ST_GEN_NOISY;
            if (ttMove) return ttMove;
            // fallthrough
        case ST_GEN_NOISY: {
            int n = generate(pos, buf, GEN_NOISY);
            for (int i = 0; i < n; i++) {
                Move m = buf[i];
                int ct = captured_type(pos, m);
                list[i].m = m;
                list[i].score = SeeValue[ct] * 16 + CaptHist[pos.board[from_sq(m)]][to_sq(m)][ct]
                              + (is_promo(m) ? SeeValue[promo_type(m)] * 16 : 0);
            }
            cur = 0; end = n; badEnd = 0;
            stage = ST_GOOD_NOISY;
        }   // fallthrough
        case ST_GOOD_NOISY:
            while (cur < end) {
                pick_best();
                ScoredMove sm = list[cur++];
                if (sm.m == ttMove) continue;
                if (!noisyOnly && !pos.see_ge(sm.m, 0)) { list[badEnd++] = sm; continue; }
                return sm.m;
            }
            if (noisyOnly) { stage = ST_DONE; return MOVE_NONE; }
            stage = ST_KILLER1;
            // fallthrough
        case ST_KILLER1:
            stage = ST_KILLER2;
            if (!skipQuiets && k1 && k1 != ttMove && !is_noisy(k1) && pos.pseudo_legal(k1)) return k1;
            // fallthrough
        case ST_KILLER2:
            stage = ST_COUNTER;
            if (!skipQuiets && k2 && k2 != ttMove && k2 != k1 && !is_noisy(k2) && pos.pseudo_legal(k2)) return k2;
            // fallthrough
        case ST_COUNTER:
            stage = ST_GEN_QUIET;
            if (!skipQuiets && counter && counter != ttMove && counter != k1 && counter != k2
                && !is_noisy(counter) && pos.pseudo_legal(counter)) return counter;
            // fallthrough
        case ST_GEN_QUIET: {
            int n = 0;
            if (!skipQuiets) {
                n = generate(pos, buf, GEN_QUIET);
                for (int i = 0; i < n; i++) {
                    list[badEnd + i].m = buf[i];
                    list[badEnd + i].score = quiet_history(pos, ss, buf[i]);
                }
            }
            cur = badEnd; end = badEnd + n;
            stage = ST_QUIET;
        }   // fallthrough
        case ST_QUIET:
            while (!skipQuiets && cur < end) {
                pick_best();
                Move m = list[cur++].m;
                if (m == ttMove || m == k1 || m == k2 || m == counter) continue;
                return m;
            }
            cur = 0;
            stage = ST_BAD_NOISY;
            // fallthrough
        case ST_BAD_NOISY:
            if (cur < badEnd) return list[cur++].m;
            stage = ST_DONE;
            // fallthrough
        default:
            return MOVE_NONE;
        }
    }
};

static void update_histories(const Position& pos, Stack* ss, Move best, int depth,
                             const Move* quiets, int nQ, const Move* noisies, int nN) {
    const int bonus = stat_bonus(depth);
    const int us = pos.stm;
    if (!is_noisy(best)) {
        if (ss->killers[0] != best) { ss->killers[1] = ss->killers[0]; ss->killers[0] = best; }
        Move prev = (ss - 1)->currentMove;
        if (prev != MOVE_NONE && prev != MOVE_NULL) CounterMoves[pos.board[to_sq(prev)]][to_sq(prev)] = best;
        for (int i = -1; i < nQ; i++) {
            Move m = i < 0 ? best : quiets[i];
            int b = i < 0 ? bonus : -bonus;
            int pc = pos.board[from_sq(m)], to = to_sq(m);
            hist_update(MainHist[us][from_sq(m)][to], b);
            hist_update((*(ss - 1)->ch)[pc][to], b);
            hist_update((*(ss - 2)->ch)[pc][to], b);
            hist_update((*(ss - 4)->ch)[pc][to], b);
        }
    } else {
        hist_update(CaptHist[pos.board[from_sq(best)]][to_sq(best)][captured_type(pos, best)], bonus);
    }
    for (int i = 0; i < nN; i++) {
        Move m = noisies[i];
        hist_update(CaptHist[pos.board[from_sq(m)]][to_sq(m)][captured_type(pos, m)], -bonus);
    }
}

// ---- quiescence search ----------------------------------------------------
static int qsearch(Position& pos, Stack* ss, int alpha, int beta) {
    const bool pvNode = beta - alpha > 1;
    PvLen[ss->ply] = 0;
    Nodes++;
    check_stop();
    if (SearchStop) return 0;
    if (ss->ply > SelDepth) SelDepth = ss->ply;
    if (pos.is_draw(ss->ply)) return 0;
    const bool inCheck = pos.in_check();
    if (ss->ply >= MAX_PLY - 1) return inCheck ? 0 : corrected_eval(pos, raw_eval(pos));

    const u64 key = pos.st->key;
    bool ttHit;
    TTEntry* tte = tt_probe(key, ttHit);
    Move ttMove = MOVE_NONE;
    int ttScore = VALUE_NONE, ttBound = BOUND_NONE, ttEval = VALUE_NONE;
    if (ttHit) {
        ttMove = tte->move;
        ttScore = tte->score == VALUE_NONE ? VALUE_NONE : score_from_tt(tte->score, ss->ply);
        ttBound = tte->bound();
        ttEval = tte->eval;
    }
    if (!pvNode && ttScore != VALUE_NONE && (ttBound & (ttScore >= beta ? BOUND_LOWER : BOUND_UPPER)))
        return ttScore;

    int rawEval = VALUE_NONE, best = -VALUE_INF, futilityBase = -VALUE_INF;
    if (!inCheck) {
        rawEval = ttEval != VALUE_NONE ? ttEval : raw_eval(pos);
        int staticEval = corrected_eval(pos, rawEval);
        best = staticEval;
        if (ttScore != VALUE_NONE && iabs(ttScore) < VALUE_MATE_IN_MAX
            && (ttBound & (ttScore > best ? BOUND_LOWER : BOUND_UPPER)))
            best = ttScore;
        if (best >= beta) {
            if (!ttHit) tt_store(tte, key, MOVE_NONE, score_to_tt(best, ss->ply), rawEval, 0, BOUND_LOWER);
            return best;
        }
        if (best > alpha) alpha = best;
        futilityBase = staticEval + QsFutility;
    }

    if (ttMove && (!pos.pseudo_legal(ttMove) || (!inCheck && !is_noisy(ttMove)))) ttMove = MOVE_NONE;

    Move bestMove = MOVE_NONE, m;
    int moveCount = 0;
    MovePicker mp(pos, ss, ttMove, !inCheck);
    while ((m = mp.next(false)) != MOVE_NONE) {
        if (!pos.legal(m)) continue;
        moveCount++;
        if (best > -VALUE_MATE_IN_MAX) {
            if (!inCheck) {
                if (!is_promo(m)) {
                    int fv = futilityBase + SeeValue[captured_type(pos, m)];
                    if (fv <= alpha) { if (fv > best) best = fv; continue; }
                }
                if (!pos.see_ge(m, 0)) continue;
            } else {
                if (!is_noisy(m)) break;        // we already have a way out of check
                if (!pos.see_ge(m, 0)) continue;
            }
        }
        ss->currentMove = m;
        ss->ch = &ContHist[pos.board[from_sq(m)]][to_sq(m)];
        pos.make(m);
        tt_prefetch(pos.st->key);
        int v = -qsearch(pos, ss + 1, -beta, -alpha);
        pos.unmake(m);
        if (SearchStop) return 0;
        if (v > best) {
            best = v;
            if (v > alpha) {
                bestMove = m;
                if (v >= beta) break;
                alpha = v;
            }
        }
    }
    if (inCheck && moveCount == 0) return mated_in(ss->ply);

    tt_store(tte, key, bestMove, score_to_tt(best, ss->ply), rawEval, 0, best >= beta ? BOUND_LOWER : BOUND_UPPER);
    return best;
}

// ---- main search ----------------------------------------------------------
static int search(Position& pos, Stack* ss, int alpha, int beta, int depth, bool cutNode) {
    if (depth <= 0) return qsearch(pos, ss, alpha, beta);

    const bool pvNode = beta - alpha > 1;
    const bool root = ss->ply == 0;
    const int us = pos.stm;
    PvLen[ss->ply] = 0;
    Nodes++;
    check_stop();
    if (SearchStop) return 0;
    if (ss->ply > SelDepth) SelDepth = ss->ply;
    const bool inCheck = pos.in_check();

    if (!root) {
        if (pos.is_draw(ss->ply)) return (int)(Nodes & 2) - 1;   // tiny jitter avoids repetition blindness
        if (ss->ply >= MAX_PLY - 1) return inCheck ? 0 : corrected_eval(pos, raw_eval(pos));
        alpha = imax(alpha, mated_in(ss->ply));
        beta = imin(beta, mate_in(ss->ply + 1));
        if (alpha >= beta) return alpha;
    }

    const Move excluded = ss->excluded;
    const u64 key = pos.st->key;
    bool ttHit;
    TTEntry* tte = tt_probe(key, ttHit);
    Move ttMove = MOVE_NONE;
    int ttScore = VALUE_NONE, ttBound = BOUND_NONE, ttEval = VALUE_NONE, ttDepth = 0;
    if (ttHit) {
        ttMove = tte->move;
        ttScore = tte->score == VALUE_NONE ? VALUE_NONE : score_from_tt(tte->score, ss->ply);
        ttBound = tte->bound();
        ttEval = tte->eval;
        ttDepth = tte->depth;
    }
    if (ttMove && !pos.pseudo_legal(ttMove)) ttMove = MOVE_NONE;

    if (!pvNode && !excluded && ttScore != VALUE_NONE && ttDepth >= depth && pos.st->halfmove < 90
        && (ttBound & (ttScore >= beta ? BOUND_LOWER : BOUND_UPPER)))
        return ttScore;

    // Static evaluation
    int rawEval = VALUE_NONE, staticEval = VALUE_NONE, eval = VALUE_NONE;
    bool improving = false;
    if (inCheck) {
        ss->staticEval = VALUE_NONE;
    } else {
        if (excluded) {
            staticEval = eval = ss->staticEval;
        } else {
            rawEval = ttEval != VALUE_NONE ? ttEval : raw_eval(pos);
            staticEval = eval = corrected_eval(pos, rawEval);
            ss->staticEval = staticEval;
        }
        if (ttScore != VALUE_NONE && iabs(ttScore) < VALUE_MATE_IN_MAX
            && (ttBound & (ttScore > eval ? BOUND_LOWER : BOUND_UPPER)))
            eval = ttScore;
        if ((ss - 2)->staticEval != VALUE_NONE) improving = staticEval > (ss - 2)->staticEval;
        else if ((ss - 4)->staticEval != VALUE_NONE) improving = staticEval > (ss - 4)->staticEval;
        else improving = true;
    }

    (ss + 2)->killers[0] = (ss + 2)->killers[1] = MOVE_NONE;
    (ss + 1)->excluded = MOVE_NONE;
    ss->doubleExt = root ? 0 : (ss - 1)->doubleExt;

    if (!pvNode && !inCheck && !excluded) {
        // Reverse futility pruning: we are so far above beta that a shallow search will not drop below it.
        if ((SearchMask & 32) && depth <= 8 && eval - RfpMargin * (depth - improving) >= beta && eval < VALUE_MATE_IN_MAX && beta > -VALUE_MATE_IN_MAX)
            return (eval + beta) / 2;

        // Razoring: hopeless at low depth, verify with quiescence only.
        if ((SearchMask & 16) && depth <= 3 && eval + RazorMargin * depth <= alpha) {
            int v = qsearch(pos, ss, alpha, alpha + 1);
            if (v <= alpha) return v;
        }

        // Null move pruning: give the opponent a free move; if we still beat beta, cut.
        if ((SearchMask & 64) && depth >= 3 && (ss - 1)->currentMove != MOVE_NULL && eval >= beta
            && staticEval >= beta - 20 * depth + 180 && pos.st->npm[us] > 0
            && beta > -VALUE_MATE_IN_MAX && ss->ply >= NmpMinPly) {
            int R = NmpBase + depth / NmpDiv + imin((eval - beta) / 200, 3);
            ss->currentMove = MOVE_NULL;
            ss->ch = &ContHist[12][0];
            pos.make_null();
            int v = -search(pos, ss + 1, -beta, -beta + 1, depth - R, !cutNode);
            pos.unmake_null();
            if (SearchStop) return 0;
            if (v >= beta) {
                if (v >= VALUE_MATE_IN_MAX) v = beta;
                if (NmpMinPly || depth < 14) return v;
                // Deep nodes: verify without null move to dodge zugzwang.
                NmpMinPly = ss->ply + 3 * (depth - R) / 4;
                int vv = search(pos, ss, beta - 1, beta, depth - R, false);
                NmpMinPly = 0;
                if (SearchStop) return 0;
                if (vv >= beta) return v;
            }
        }

        // ProbCut: a good capture that beats beta by a margin at reduced depth is very likely a cutoff.
        const int probBeta = beta + ProbCutMargin - 50 * improving;
        if ((SearchMask & 4) && depth >= 5 && iabs(beta) < VALUE_MATE_IN_MAX
            && !(ttScore != VALUE_NONE && ttDepth >= depth - 3 && ttScore < probBeta)) {
            MovePicker mp(pos, ss, (ttMove && is_noisy(ttMove)) ? ttMove : (Move)MOVE_NONE, true);
            Move m;
            while ((m = mp.next(true)) != MOVE_NONE) {
                if (!pos.legal(m) || !pos.see_ge(m, probBeta - staticEval)) continue;
                ss->currentMove = m;
                ss->ch = &ContHist[pos.board[from_sq(m)]][to_sq(m)];
                pos.make(m);
                int v = -qsearch(pos, ss + 1, -probBeta, -probBeta + 1);
                if (v >= probBeta) v = -search(pos, ss + 1, -probBeta, -probBeta + 1, depth - 4, !cutNode);
                pos.unmake(m);
                if (SearchStop) return 0;
                if (v >= probBeta) {
                    tt_store(tte, key, m, score_to_tt(v, ss->ply), rawEval, depth - 3, BOUND_LOWER);
                    return v;
                }
            }
        }
    }

    // Internal iterative reduction: no hash move means this node is probably unimportant.
    if (depth >= 4 && !ttMove && (pvNode || cutNode)) depth--;

    const bool ttNoisy = ttMove && is_noisy(ttMove);
    Move quiets[64], noisies[32];
    int nQ = 0, nN = 0;
    int best = -VALUE_INF, moveCount = 0;
    Move bestMove = MOVE_NONE, m;
    bool skipQuiets = false;
    MovePicker mp(pos, ss, ttMove, false);

    while ((m = mp.next(skipQuiets)) != MOVE_NONE) {
        if (m == excluded || !pos.legal(m)) continue;
        moveCount++;
        const bool quiet = !is_noisy(m);
        const bool givesCheck = pos.gives_check(m);
        const int pc = pos.board[from_sq(m)];
        const int hist = quiet ? quiet_history(pos, ss, m) : 0;
        const int lmrBase = LmrTable[imin(depth, 63)][imin(moveCount, 63)];

        // Shallow-depth pruning of moves that are unlikely to matter.
        if ((SearchMask & 128) && !root && best > -VALUE_MATE_IN_MAX && pos.st->npm[us] > 0) {
            int lmrDepth = imax(0, depth - 1 - lmrBase);
            if (quiet) {
                if (moveCount >= (LmpBase + depth * depth) / (2 - improving)) { skipQuiets = true; continue; }
                if (!inCheck && !givesCheck && lmrDepth <= 8 && staticEval + FutBase + FutMul * lmrDepth <= alpha) continue;
                if (lmrDepth <= 4 && hist < -HistPrune * depth) continue;
                if (!pos.see_ge(m, -SeeQuiet * lmrDepth * lmrDepth)) continue;
            } else if (depth <= 8 && !pos.see_ge(m, -SeeNoisy * depth)) continue;
        }

        // Extensions
        int ext = 0;
        if ((SearchMask & 2) && !root && depth >= 8 && m == ttMove && !excluded && ttDepth >= depth - 3 && (ttBound & BOUND_LOWER)
            && iabs(ttScore) < VALUE_MATE_IN_MAX && ss->ply < 2 * RootDepth) {
            // Singular extension: is the hash move the only good move here?
            const int sBeta = ttScore - SingularMul * depth;
            ss->excluded = m;
            int v = search(pos, ss, sBeta - 1, sBeta, (depth - 1) / 2, cutNode);
            ss->excluded = MOVE_NONE;
            if (SearchStop) return 0;
            if (v < sBeta) {
                ext = 1;
                if (!pvNode && v < sBeta - 20 && ss->doubleExt < 8) ext = 2;
            } else if (sBeta >= beta) {
                return sBeta;                       // several moves beat beta: multi-cut
            } else if (ttScore >= beta || cutNode) {
                ext = -1;
            }
        } else if ((SearchMask & 1) && givesCheck && ss->ply < 2 * RootDepth && pos.see_ge(m, 0)) {
            ext = 1;
        }

        int newDepth = depth - 1 + ext;
        ss->doubleExt = (root ? 0 : (ss - 1)->doubleExt) + (ext >= 2);
        ss->currentMove = m;
        ss->ch = &ContHist[pc][to_sq(m)];
        const u64 nodesBefore = Nodes;
        pos.make(m);
        tt_prefetch(pos.st->key);

        int v = 0;
        bool fullDepth;
        // Late move reductions: later moves in the ordering get a shallower null-window search first.
        if (depth >= 2 && moveCount > 1 + root) {
            int r = lmrBase;
            if (quiet) {
                r += !improving;
                r += cutNode;
                r += ttNoisy;
                r -= pvNode;
                r -= (m == mp.k1 || m == mp.k2 || m == mp.counter);
                r -= iclamp(hist / LmrHist, -2, 2);
            } else {
                r = r / 2 + cutNode;
            }
            r -= givesCheck;
            int d = iclamp(newDepth - r, 1, newDepth);
            v = -search(pos, ss + 1, -alpha - 1, -alpha, d, true);
            fullDepth = v > alpha && d < newDepth;
        } else {
            fullDepth = !pvNode || moveCount > 1;
        }
        if (fullDepth) v = -search(pos, ss + 1, -alpha - 1, -alpha, newDepth, !cutNode);
        if (pvNode && (moveCount == 1 || v > alpha)) v = -search(pos, ss + 1, -beta, -alpha, newDepth, false);

        pos.unmake(m);
        if (SearchStop) return 0;
        if (root) RootMoveNodes[m & 4095] += Nodes - nodesBefore;

        if (v > best) {
            best = v;
            if (v > alpha) {
                bestMove = m;
                if (pvNode) {
                    int len = PvLen[ss->ply + 1];
                    PvTable[ss->ply][0] = m;
                    for (int i = 0; i < len; i++) PvTable[ss->ply][i + 1] = PvTable[ss->ply + 1][i];
                    PvLen[ss->ply] = len + 1;
                }
                if (root) RootBest = m;
                if (v >= beta) break;
                alpha = v;
            }
        }
        if (m != bestMove) {
            if (quiet) { if (nQ < 64) quiets[nQ++] = m; }
            else if (nN < 32) noisies[nN++] = m;
        }
    }

    if (moveCount == 0) return excluded ? alpha : inCheck ? mated_in(ss->ply) : 0;

    if (best >= beta && bestMove) update_histories(pos, ss, bestMove, depth, quiets, nQ, noisies, nN);

    if (!excluded) {
        const int bound = best >= beta ? BOUND_LOWER : (pvNode && bestMove) ? BOUND_EXACT : BOUND_UPPER;
        tt_store(tte, key, bestMove, score_to_tt(best, ss->ply), rawEval, depth, bound);

        // Learn how wrong the static eval was for this pawn structure.
        if (!inCheck && (!bestMove || !is_noisy(bestMove))
            && !(bound == BOUND_LOWER && best <= staticEval) && !(bound == BOUND_UPPER && best >= staticEval)
            && iabs(best) < VALUE_MATE_IN_MAX) {
            int bonus = iclamp((best - staticEval) * depth / 8, -256, 256);
            i16& e = PawnCorr[us][pos.st->pawnKey & 16383];
            e = (i16)(e + bonus - e * iabs(bonus) / 1024);
        }
    }
    return best;
}

// ---- iterative deepening driver --------------------------------------------
// Win / draw / loss chances for a score, fitted to how Tessera's own self-play games
// ended (tools/fit_wdl.py). "a" is the score at which a win becomes a coin flip and "b"
// how gradual the change is; both depend on how much material is left.
static const double WdlA0 = 291.9, WdlA1 = -215.9, WdlB0 = 134.4, WdlB1 = 31.6;
static int RootPhase = 24;

static double exp_approx(double x) {
    x = iclamp(x, -40.0, 40.0);
    double v = 1.0 + x / 65536.0;
    for (int i = 0; i < 16; i++) v *= v;   // (1 + x/n)^n with n = 2^16
    return v;
}

// Per-mille chances from the side to move's point of view; they always add up to 1000.
static void score_to_wdl(int score, int phase, int& win, int& draw, int& loss) {
    if (iabs(score) >= VALUE_MATE_IN_MAX) { win = score > 0 ? 1000 : 0; loss = 1000 - win; draw = 0; return; }
    const double m = imin(phase, 24) / 24.0;
    const double a = WdlA0 + WdlA1 * m, b = imax(WdlB0 + WdlB1 * m, 10.0);
    win = (int)(1000.0 / (1.0 + exp_approx((a - score) / b)) + 0.5);
    loss = (int)(1000.0 / (1.0 + exp_approx((a + score) / b)) + 0.5);
    if (win + loss > 1000) { if (win > loss) win = 1000 - loss; else loss = 1000 - win; }
    draw = 1000 - win - loss;
}

static void print_info(int depth, int score, u64 elapsed) {
    StrBuf s;
    s.add("info depth "); s.add_int(depth);
    s.add(" seldepth "); s.add_int(SelDepth);
    if (iabs(score) >= VALUE_MATE_IN_MAX) {
        int n = (VALUE_MATE - iabs(score) + 1) / 2;
        s.add(" score mate "); s.add_int(score > 0 ? n : -n);
    } else {
        s.add(" score cp "); s.add_int(score);
    }
    if (ShowWdl) {
        int w, d, l;
        score_to_wdl(score, RootPhase, w, d, l);
        s.add(" wdl "); s.add_int(w); s.add_char(' '); s.add_int(d); s.add_char(' '); s.add_int(l);
    }
    s.add(" nodes "); s.add_int((i64)Nodes);
    s.add(" nps "); s.add_int((i64)(Nodes * 1000 / (elapsed ? elapsed : 1)));
    s.add(" hashfull "); s.add_int(tt_hashfull());
    s.add(" time "); s.add_int((i64)elapsed);
    s.add(" pv");
    for (int i = 0; i < PvLen[0]; i++) { s.add_char(' '); s.add_move(PvTable[0][i]); }
    plat_out(s.b);
}

SearchResult search_go(Position& pos, const Limits& lim, bool printInfo) {
    static Stack stack[MAX_PLY + 10];
    SearchResult res;
    res.best = res.ponder = MOVE_NONE; res.score = 0; res.depth = 0; res.nodes = 0; res.timeMs = 0;

    StartTime = plat_now_ms();
    SearchStop = false;
    Nodes = 0; NmpMinPly = 0; RootDepth = 1;
    NodeLimit = lim.nodes;
    RootPhase = pos.st->phase;
    NoiseSeed = StartTime * 0x2545F4914F6CDD1DULL + pos.st->key;
    // Full strength is deterministic, which means the same opening every game.
    // A few centipawns of noise in the first moves gives variety at no real cost.
    NoiseAmp = LevelNoise;
    if (OpeningVariety && Level == 10 && pos.fullmove <= 4 && pos.st->phase >= 24) NoiseAmp = 14;
    tt_new_search();
    __builtin_memset(RootMoveNodes, 0, sizeof(RootMoveNodes));
    for (int i = 0; i < MAX_PLY + 10; i++) {
        stack[i].ch = &ContHist[12][0];
        stack[i].currentMove = stack[i].excluded = stack[i].killers[0] = stack[i].killers[1] = MOVE_NONE;
        stack[i].staticEval = VALUE_NONE;
        stack[i].ply = i - 4;
        stack[i].doubleExt = 0;
    }
    Stack* ss = stack + 4;

    // Time budget
    const int us = pos.stm;
    u64 softLimit = 0;
    HardLimit = 0;
    bool clock = false;
    if (lim.movetime > 0) {
        HardLimit = (u64)imax(1, lim.movetime - MoveOverhead);
    } else if (lim.time[us] > 0 && !lim.infinite) {
        clock = true;
        double t = (double)imax(1, lim.time[us] - MoveOverhead), inc = (double)lim.inc[us];
        int mtg = lim.movestogo > 0 ? imin(lim.movestogo, 50) : 25;
        double base = t / mtg + inc * 0.8;
        double hard = imin(base * 3.5, t * 0.6);
        softLimit = (u64)imax(1.0, imin(base * 0.6, hard));
        HardLimit = (u64)imax(1.0, hard);
    }

    Move legalMoves[MAX_MOVES];
    int nLegal = 0;
    {
        Move buf[MAX_MOVES];
        int n = generate(pos, buf, GEN_ALL);
        for (int i = 0; i < n; i++) if (pos.legal(buf[i])) legalMoves[nLegal++] = buf[i];
    }
    if (nLegal == 0) return res;
    RootBest = legalMoves[0];

    int maxDepth = lim.depth > 0 ? imin(lim.depth, MAX_PLY - 8) : MAX_PLY - 8;
    if (LevelDepth) maxDepth = imin(maxDepth, LevelDepth);

    int score = 0, stability = 0;
    Move prevBest = MOVE_NONE;
    for (RootDepth = 1; RootDepth <= maxDepth; RootDepth++) {
        SelDepth = 0;
        int alpha = -VALUE_INF, beta = VALUE_INF, delta = AspDelta, reduce = 0;
        if (RootDepth >= 5) { alpha = imax(score - delta, -VALUE_INF); beta = imin(score + delta, (int)VALUE_INF); }
        int v;
        for (;;) {
            v = search(pos, ss, alpha, beta, imax(1, RootDepth - reduce), false);
            if (SearchStop) break;
            if (v <= alpha) {
                beta = (alpha + beta) / 2;
                alpha = imax(v - delta, -VALUE_INF);
                reduce = 0;
            } else if (v >= beta) {
                beta = imin(v + delta, (int)VALUE_INF);
                if (reduce < 3) reduce++;
            } else break;
            delta += delta / 2;
        }
        if (SearchStop) break;

        score = v;
        res.depth = RootDepth;
        res.score = score;
        res.best = PvLen[0] ? PvTable[0][0] : RootBest;
        res.ponder = PvLen[0] > 1 ? PvTable[0][1] : (Move)MOVE_NONE;
        u64 elapsed = plat_now_ms() - StartTime;
        if (printInfo) print_info(RootDepth, score, elapsed);

        stability = res.best == prevBest ? stability + 1 : 0;
        prevBest = res.best;
        if (clock) {
            if (nLegal == 1) break;
            double frac = (double)RootMoveNodes[res.best & 4095] / (double)(Nodes ? Nodes : 1);
            double scale = (1.55 - frac) * (stability >= 6 ? 0.8 : 1.25 - 0.075 * stability);
            if ((double)elapsed >= (double)softLimit * scale) break;
        }
        // A forced mate that is already shorter than the search depth will not get better.
        if (!lim.infinite && lim.depth == 0 && iabs(score) >= VALUE_MATE_IN_MAX
            && VALUE_MATE - iabs(score) <= RootDepth && RootDepth >= 12 && (clock || lim.movetime))
            break;
    }

    // If the last iteration was cut short, its partial result is still the best information we have.
    if (res.best == MOVE_NONE || (SearchStop && RootBest != res.best)) {
        res.best = RootBest;
        res.ponder = MOVE_NONE;
    }
    res.nodes = Nodes;
    res.timeMs = plat_now_ms() - StartTime;
    return res;
}
