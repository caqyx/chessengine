// Texel tuner for Tessera's evaluation weights.
//
// Idea: a good evaluation should predict game results. For every training
// position we know who won the game it came from. We turn the evaluation E
// (centipawns, White's view) into a win probability with a logistic curve,
//     p = 1 / (1 + 10^(-K * E / 400)),
// and move the weights by gradient descent (Adam, mini-batches) to shrink the
// squared error between p and the actual result (1, 0.5, 0).
//
// Two kinds of weights are tuned:
//   * linear ones: E contains weight * count (piece values, mobility, pawns, threats)
//   * king safety: shelter tables and the weights of the "danger" sum, which enters
//     the score through a square (see eval_king); its gradient uses the chain rule.
// Everything else (passed-pawn race terms, endgame knowledge) stays fixed and is
// stored per position as a residual. All counts come from the engine itself,
// built with -DTUNE, and the tuner checks that it can reproduce the engine's score.
//
// Build and run (from the repo root):
//   g++ -O3 -std=c++17 -DTUNE -Iengine/src tools/tuner.cpp engine/src/bitboard.cpp \
//       engine/src/position.cpp engine/src/movegen.cpp engine/src/eval.cpp engine/src/nnue.cpp -o /tmp/tuner
//   ./engine/tessera datagen 5000 5000 1 > /tmp/data.txt     # self-play positions
//   /tmp/tuner /tmp/data.txt engine/src/eval_params.h         # rewrites the weights
// Lines are "fen;result" or "fen;score;result"; several data files may be concatenated.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <numeric>
#include <random>
#include <string>
#include <vector>

#include "eval.h"
#include "eval_params.h"
#include "eval_trace.h"

struct Group {
    const char* name;
    const char* comment;
    const int* values;
    int count, first;      // number of weights, index of the first one
};

static const Group Groups[] = {
    {"MobilityN", "Mobility: indexed by the number of safe squares the piece attacks.", MobilityN, 9, F_MOB_N},
    {"MobilityB", "Bishop mobility", MobilityB, 14, F_MOB_B},
    {"MobilityR", "Rook mobility", MobilityR, 15, F_MOB_R},
    {"MobilityQ", "Queen mobility", MobilityQ, 28, F_MOB_Q},
    {"Connected", "Pawn defended by or next to another pawn, by rank.", Connected, 8, F_CONNECTED},
    {"PassedRank", "Passed pawn, by rank.", PassedRank, 8, F_PASSED_RANK},
};
struct Scalar { const char* name; const int* value; int index; };
static const Scalar Scalars[] = {
    {"BishopPair", &BishopPair, F_BISHOP_PAIR}, {"Isolated", &Isolated, F_ISOLATED}, {"Doubled", &Doubled, F_DOUBLED},
    {"RookOpen", &RookOpen, F_ROOK_OPEN}, {"RookSemiOpen", &RookSemiOpen, F_ROOK_SEMI},
    {"KnightOutpost", &KnightOutpost, F_KNIGHT_OUTPOST}, {"BishopOutpost", &BishopOutpost, F_BISHOP_OUTPOST},
    {"BishopPawns", &BishopPawns, F_BISHOP_PAWNS}, {"ThreatByPawn", &ThreatByPawn, F_THREAT_PAWN},
    {"ThreatMinorOnMajor", &ThreatMinorOnMajor, F_THREAT_MINOR}, {"ThreatRookOnQueen", &ThreatRookOnQueen, F_THREAT_ROOK},
    {"Hanging", &Hanging, F_HANGING},
};

// One flat parameter vector: linear middlegame, linear endgame, shelter/storm, danger weights, endgame share.
enum {
    P_MG = 0,
    P_EG = P_MG + F_COUNT,
    P_SHELTER = P_EG + F_COUNT,      // 16: ShelterBonus[8], StormPenalty[8]
    P_DANGER = P_SHELTER + 16,       // KD_COUNT
    P_DANGER_EG = P_DANGER + KD_COUNT,
    P_COUNT
};
static double P[P_COUNT];

struct Term { unsigned short index; signed char count; };
struct KingSample { signed char active; short x[KD_COUNT]; signed char shelter2[16]; };
struct Sample {
    unsigned first, n;          // range in the shared term array
    float resMg, resEg;         // parts of the eval that are not tuned
    float mgFactor, egFactor;   // d(E)/d(mg), d(E)/d(eg) from phase, scale and fifty-move damping
    float tempo;                // +16 or -16 times damping
    float result;
    KingSample king[2];
};

static std::vector<Term> Terms;
static std::vector<Sample> Samples;
static const double Tempo = 16.0;

// King term of one side, exactly as eval_king computes it (without integer rounding).
struct KingValue { double mg, eg, shelter, dMgDDanger, dEgDDanger, danger; };
static KingValue king_value(const KingSample& k) {
    KingValue v = {0, 0, 0, 0, 0, 0};
    for (int j = 0; j < 16; j++) v.shelter += P[P_SHELTER + j] * k.shelter2[j] * 0.5;
    v.mg = v.shelter;
    if (!k.active) return v;
    double danger = -0.75 * v.shelter;
    for (int j = 0; j < KD_COUNT; j++) danger += P[P_DANGER + j] * k.x[j];
    if (danger <= 100) return v;
    const bool capped = danger >= 2400;
    if (capped) danger = 2400;
    v.danger = danger;
    v.mg -= danger * danger / 6400.0;
    v.eg -= danger * P[P_DANGER_EG] / 256.0;
    if (!capped) { v.dMgDDanger = -2.0 * danger / 6400.0; v.dEgDDanger = -P[P_DANGER_EG] / 256.0; }
    return v;
}

static double predict_cp(const Sample& s) {
    double mg = s.resMg, eg = s.resEg;
    for (unsigned i = s.first; i < s.first + s.n; i++) {
        mg += P[P_MG + Terms[i].index] * Terms[i].count;
        eg += P[P_EG + Terms[i].index] * Terms[i].count;
    }
    const KingValue w = king_value(s.king[WHITE]), b = king_value(s.king[BLACK]);
    mg += w.mg - b.mg;
    eg += w.eg - b.eg;
    return mg * s.mgFactor + eg * s.egFactor + s.tempo;
}

static double sigmoid(double cp, double K) { return 1.0 / (1.0 + std::pow(10.0, -K * cp / 400.0)); }

// Positions of one game sit next to each other in the data and share a result, so the
// validation set must be whole games the tuner never saw: the last tenth of the data.
static size_t ValStart = 0;

static double loss(double K, int part) {   // part: 0 = training, 1 = validation
    double sum = 0;
    size_t from = part ? ValStart : 0, to = part ? Samples.size() : ValStart;
    for (size_t i = from; i < to; i++) {
        double d = Samples[i].result - sigmoid(predict_cp(Samples[i]), K);
        sum += d * d;
    }
    return sum / (to > from ? to - from : 1);
}

static void write_params(const char* path) {
    FILE* f = std::fopen(path, "w");
    if (!f) { std::perror(path); std::exit(1); }
    auto r = [](double v) { return (int)std::lround(v); };
    auto emit = [&](int i) { std::fprintf(f, "S(%d, %d),", r(P[P_MG + i]), r(P[P_EG + i])); };
    std::fprintf(f,
        "// Evaluation weights, each a packed S(middlegame, endgame) pair in centipawns.\n"
        "// This file is generated: tools/tuner.cpp rewrites it after fitting the weights to\n"
        "// self-play games (Texel tuning). Edit by hand only if you do not plan to re-tune.\n"
        "#pragma once\n#include \"types.h\"\n\n"
        "// Material + piece-square value, [piece type][square], listed from rank 8 down to\n"
        "// rank 1 (a8 first) from White's side. Started from the PeSTO tables.\n"
        "static const int PsqTable[6][64] = {\n");
    static const char* names[6] = {"pawn", "knight", "bishop", "rook", "queen", "king"};
    for (int pt = 0; pt < 6; pt++) {
        std::fprintf(f, "    {   // %s\n", names[pt]);
        for (int rank = 0; rank < 8; rank++) {
            std::fprintf(f, "        ");
            for (int c = 0; c < 8; c++) { emit(F_PSQ + pt * 64 + rank * 8 + c); std::fprintf(f, c < 7 ? " " : "\n"); }
        }
        std::fprintf(f, "    },\n");
    }
    std::fprintf(f, "};\n\n");
    for (const Group& g : Groups) {
        std::fprintf(f, "// %s\nstatic const int %s[%d] = {\n", g.comment, g.name, g.count);
        for (int i = 0; i < g.count; i++) {
            if (i % 6 == 0) std::fprintf(f, "    ");
            emit(g.first + i);
            std::fprintf(f, (i % 6 == 5 || i == g.count - 1) ? "\n" : " ");
        }
        std::fprintf(f, "};\n\n");
    }
    for (const Scalar& s : Scalars)
        std::fprintf(f, "static const int %s = S(%d, %d);\n", s.name, r(P[P_MG + s.index]), r(P[P_EG + s.index]));
    std::fprintf(f,
        "\n// King safety. Shelter and storm are middlegame-only values indexed by the rank of the\n"
        "// nearest own / enemy pawn in front of the king on a file (index 0 = no pawn there).\n"
        "static const int ShelterBonus[8] = {");
    for (int i = 0; i < 8; i++) std::fprintf(f, "%d%s", r(P[P_SHELTER + i]), i < 7 ? ", " : "};\n");
    std::fprintf(f, "static const int StormPenalty[8] = {");
    for (int i = 0; i < 8; i++) std::fprintf(f, "%d%s", r(P[P_SHELTER + 8 + i]), i < 7 ? ", " : "};\n");
    std::fprintf(f,
        "// Weights of the king danger ingredients, in the order of the KD_ enum in eval_trace.h:\n"
        "// attackers x knights, x bishops, x rooks, x queens, attacked squares by the king,\n"
        "// weak ring squares, safe rook / queen / bishop / knight check, attacker has no queen, constant.\n"
        "static const int KingDanger[%d] = {", (int)KD_COUNT);
    for (int i = 0; i < KD_COUNT; i++) std::fprintf(f, "%d%s", r(P[P_DANGER + i]), i < KD_COUNT - 1 ? ", " : "};\n");
    std::fprintf(f, "// Endgame share of the penalty: danger * KingDangerEg / 256.\nstatic const int KingDangerEg = %d;\n",
                 r(P[P_DANGER_EG]));
    std::fclose(f);
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: tuner <data.txt> <out eval_params.h> [passes=40] [learning rate=0.08] [tie|free] [stride=1]\n");
        return 1;
    }
    const int passes = argc > 3 ? std::atoi(argv[3]) : 40;
    const double baseLr = argc > 4 ? std::atof(argv[4]) : 0.08;
    // "tie" (default) tunes one value per piece type and keeps the piece-square shapes;
    // "free" tunes all 384 squares separately, which needs far more games to be safe.
    const bool tiePsq = !(argc > 5 && std::string(argv[5]) == "free");
    const int stride = argc > 6 ? std::max(1, std::atoi(argv[6])) : 1;   // use every n-th position
    bitboards_init();
    position_init();
    eval_init();

    // Starting point: the weights currently compiled in.
    for (int i = 0; i < 384; i++) { P[P_MG + F_PSQ + i] = mg_of(PsqTable[i / 64][i % 64]); P[P_EG + F_PSQ + i] = eg_of(PsqTable[i / 64][i % 64]); }
    for (const Group& g : Groups)
        for (int i = 0; i < g.count; i++) { P[P_MG + g.first + i] = mg_of(g.values[i]); P[P_EG + g.first + i] = eg_of(g.values[i]); }
    for (const Scalar& s : Scalars) { P[P_MG + s.index] = mg_of(*s.value); P[P_EG + s.index] = eg_of(*s.value); }
    for (int i = 0; i < 8; i++) { P[P_SHELTER + i] = ShelterBonus[i]; P[P_SHELTER + 8 + i] = StormPenalty[i]; }
    for (int i = 0; i < KD_COUNT; i++) P[P_DANGER + i] = KingDanger[i];
    P[P_DANGER_EG] = KingDangerEg;

    // Load positions and record which weights each one uses.
    static Position pos;
    std::ifstream in(argv[1]);
    std::string line;
    size_t bad = 0, mismatch = 0, lineNo = 0;
    double worst = 0;
    std::vector<int> seen(P_COUNT, 0);
    std::vector<double> shelterWeight(16, 0.0);   // how much middlegame weight each shelter entry saw
    while (std::getline(in, line)) {
        if (lineNo++ % stride) continue;
        size_t semi = line.find(';'), last = line.rfind(';');
        if (semi == std::string::npos) continue;
        if (!pos.set(line.substr(0, semi).c_str())) { bad++; continue; }
        int value = evaluate(pos);
        if (!pos.pieces(PAWN) && pos.st->npm[0] + pos.st->npm[1] <= 365) continue;   // eval is a hard-coded draw
        Sample s;
        s.first = (unsigned)Terms.size();
        double lmg = 0, leg = 0;
        for (int i = 0; i < F_COUNT; i++) {
            if (!Trace.coeff[i]) continue;
            Terms.push_back({(unsigned short)i, (signed char)Trace.coeff[i]});
            lmg += P[P_MG + i] * Trace.coeff[i];
            leg += P[P_EG + i] * Trace.coeff[i];
            seen[P_MG + i]++;
        }
        s.n = (unsigned)Terms.size() - s.first;
        for (int c = 0; c < 2; c++) {
            s.king[c].active = Trace.king[c].active;
            for (int j = 0; j < KD_COUNT; j++) {
                s.king[c].x[j] = Trace.king[c].x[j];
                if (Trace.king[c].active && Trace.king[c].x[j]) seen[P_DANGER + j]++;
            }
            for (int j = 0; j < 16; j++) {
                s.king[c].shelter2[j] = Trace.king[c].shelter2[j];
                if (Trace.king[c].shelter2[j]) seen[P_SHELTER + j]++;
            }
        }
        const double mgShare = Trace.phase / 24.0;
        for (int c = 0; c < 2; c++)
            for (int j = 0; j < 16; j++)
                if (Trace.king[c].shelter2[j]) shelterWeight[j] += mgShare;
        const KingValue kw = king_value(s.king[WHITE]), kb = king_value(s.king[BLACK]);
        s.resMg = (float)(Trace.mg - lmg - (kw.mg - kb.mg));
        s.resEg = (float)(Trace.eg - leg - (kw.eg - kb.eg));
        double damp = (200 - (Trace.halfmove < 100 ? Trace.halfmove : 100)) / 200.0;
        s.mgFactor = (float)(Trace.phase / 24.0 * damp);
        s.egFactor = (float)((24 - Trace.phase) / 24.0 * Trace.scale / 64.0 * damp);
        s.tempo = (float)((pos.stm == WHITE ? Tempo : -Tempo) * damp);
        s.result = (float)std::atof(line.c_str() + last + 1);
        // The reconstruction must agree with the engine's own answer (up to integer rounding).
        double white = predict_cp(s), engine = pos.stm == WHITE ? value : -value;
        worst = std::max(worst, std::fabs(white - engine));
        if (std::fabs(white - engine) > 3.0) mismatch++;
        Samples.push_back(s);
    }
    std::printf("positions %zu, terms %zu, unreadable %zu, reconstruction mismatches %zu (worst %.1f cp)\n",
                Samples.size(), Terms.size(), bad, mismatch, worst);
    if (Samples.size() < 1000 || mismatch > Samples.size() / 1000) { std::printf("refusing to tune on this data\n"); return 1; }
    ValStart = Samples.size() - Samples.size() / 10;

    // Fit the curve steepness K to the current weights, then keep it fixed.
    double K = 1.0, best = 1e9;
    for (double k = 0.4; k <= 2.6; k += 0.02) {
        double l = loss(k, 0);
        if (l < best) { best = l; K = k; }
    }
    std::printf("K = %.2f   start: train %.6f  validation %.6f\n", K, loss(K, 0), loss(K, 1));

    // Adam over shuffled mini-batches. Danger weights live on a 10x larger scale than
    // centipawn weights, so they get a larger step.
    std::vector<double> grad(P_COUNT), m1(P_COUNT, 0), m2(P_COUNT, 0), lrScale(P_COUNT, 1.0);
    for (int j = 0; j < KD_COUNT; j++) lrScale[P_DANGER + j] = 8.0;
    lrScale[P_DANGER_EG] = 0.25;
    const double b1 = 0.9, b2 = 0.999, eps = 1e-8, c = K * std::log(10.0) / 400.0;
    const size_t batch = 16384;
    std::vector<unsigned> order(ValStart);
    std::iota(order.begin(), order.end(), 0u);
    std::mt19937 rng(20261111);
    double bestVal = loss(K, 1);
    std::vector<double> bestP(P, P + P_COUNT);
    long step = 0;
    for (int pass = 1; pass <= passes; pass++) {
        std::shuffle(order.begin(), order.end(), rng);
        const double lr = baseLr * (1.0 - 0.9 * (pass - 1) / std::max(1, passes - 1));   // decays to a tenth
        for (size_t start = 0; start < order.size(); start += batch) {
            const size_t end = std::min(order.size(), start + batch);
            std::fill(grad.begin(), grad.end(), 0.0);
            for (size_t o = start; o < end; o++) {
                const Sample& s = Samples[order[o]];
                const double p = sigmoid(predict_cp(s), K);
                const double d = -2.0 * (s.result - p) * p * (1 - p) * c;
                const double dMg = d * s.mgFactor, dEg = d * s.egFactor;
                for (unsigned t = s.first; t < s.first + s.n; t++) {
                    grad[P_MG + Terms[t].index] += dMg * Terms[t].count;
                    grad[P_EG + Terms[t].index] += dEg * Terms[t].count;
                }
                for (int side = 0; side < 2; side++) {
                    const KingSample& k = s.king[side];
                    const double sign = side == WHITE ? 1.0 : -1.0;
                    const KingValue v = king_value(k);
                    // danger moves the score through the square (mg) and linearly (eg)
                    const double dDanger = sign * (dMg * v.dMgDDanger + dEg * v.dEgDDanger);
                    for (int j = 0; j < 16; j++) {
                        if (!k.shelter2[j]) continue;
                        const double count = k.shelter2[j] * 0.5;
                        grad[P_SHELTER + j] += sign * dMg * count + dDanger * (-0.75 * count);
                    }
                    if (v.danger > 0) {
                        for (int j = 0; j < KD_COUNT; j++) grad[P_DANGER + j] += dDanger * k.x[j];
                        grad[P_DANGER_EG] += sign * dEg * (-v.danger / 256.0);
                    }
                }
            }
            if (tiePsq) {
                // One shared step per piece type: the piece's value moves, the shape of its table stays.
                for (int base = P_MG; base <= P_EG; base += P_EG - P_MG)
                    for (int pt = 0; pt < 6; pt++) {
                        double sum = 0;
                        for (int sq = 0; sq < 64; sq++) sum += grad[base + F_PSQ + pt * 64 + sq];
                        for (int sq = 0; sq < 64; sq++) grad[base + F_PSQ + pt * 64 + sq] = sum;
                    }
            }
            step++;
            for (int i = 0; i < P_COUNT; i++) {
                const int feature = i < P_EG ? i : i < P_SHELTER ? i - P_EG : -1;
                const bool psq = feature >= 0 && feature < F_PSQ + 384;
                const int uses = feature >= 0 ? seen[P_MG + feature] : i == P_DANGER_EG ? 1000000 : seen[i];
                if (!(psq && tiePsq) && uses < 20) continue;   // too rare to learn anything: keep the starting value
                const double g = grad[i] / (double)(end - start);
                m1[i] = b1 * m1[i] + (1 - b1) * g;
                m2[i] = b2 * m2[i] + (1 - b2) * g * g;
                const double mh = m1[i] / (1 - std::pow(b1, (double)step)), vh = m2[i] / (1 - std::pow(b2, (double)step));
                P[i] -= lr * lrScale[i] * mh / (std::sqrt(vh) + eps);
            }
        }
        const double va = loss(K, 1);
        if (va < bestVal) { bestVal = va; bestP.assign(P, P + P_COUNT); }
        if (pass % 5 == 0 || pass == passes) std::printf("pass %3d  train %.6f  validation %.6f\n", pass, loss(K, 0), va);
    }
    // Keep the weights that did best on games the tuner never trained on.
    std::copy(bestP.begin(), bestP.end(), P);
    std::printf("kept weights with validation %.6f\n", bestVal);

    // Table entries that almost never occur (a queen with 26 safe squares, a connected pawn
    // on the 7th rank) get fitted to noise. Give them the value of the nearest entry in the
    // same table that was seen often enough to trust.
    const int trusted = (int)(Samples.size() / 100);
    for (const Group& g : Groups) {
        for (int i = 0; i < g.count; i++) {
            const int uses = seen[P_MG + g.first + i];
            if (uses == 0 || uses >= trusted) continue;
            for (int d = 1; d < g.count; d++) {
                int j = i - d >= 0 && seen[P_MG + g.first + i - d] >= trusted ? i - d
                      : i + d < g.count && seen[P_MG + g.first + i + d] >= trusted ? i + d : -1;
                if (j < 0) continue;
                std::printf("%s[%d] seen %d times: using the value of [%d]\n", g.name, i, uses, j);
                P[P_MG + g.first + i] = bestP[P_MG + g.first + j];
                P[P_EG + g.first + i] = bestP[P_EG + g.first + j];
                break;
            }
        }
    }

    // Same idea for the shelter and storm tables, which have no smooth neighbours to borrow
    // from. They only count in the middlegame, so an entry that shows up mostly in endgames
    // (a king blockading a pawn on the 7th) has almost no signal: it keeps its starting value.
    for (int j = 0; j < 16; j++) {
        if (shelterWeight[j] >= trusted) continue;
        const int start = j < 8 ? ShelterBonus[j] : StormPenalty[j - 8];
        if (seen[P_SHELTER + j])
            std::printf("%s[%d] has too little middlegame data: keeping %d\n", j < 8 ? "ShelterBonus" : "StormPenalty", j % 8, start);
        P[P_SHELTER + j] = start;
    }

    // Average piece values, as a sanity check a human can read.
    static const char* names[5] = {"pawn", "knight", "bishop", "rook", "queen"};
    for (int pt = 0; pt < 5; pt++) {
        double a = 0, b = 0;
        int n = 0;
        for (int sq = (pt == 0 ? 8 : 0); sq < (pt == 0 ? 56 : 64); sq++) { a += P[P_MG + F_PSQ + pt * 64 + sq]; b += P[P_EG + F_PSQ + pt * 64 + sq]; n++; }
        std::printf("average %-6s mg %4.0f  eg %4.0f\n", names[pt], a / n, b / n);
    }
    write_params(argv[2]);
    std::printf("wrote %s\n", argv[2]);
    return 0;
}
