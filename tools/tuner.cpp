// Texel tuner for Tessera's evaluation weights.
//
// Idea: a good evaluation should predict game results. For every training
// position we know who won the game it came from. We turn the evaluation E
// (centipawns, White's view) into a win probability with a logistic curve,
//     p = 1 / (1 + 10^(-K * E / 400)),
// and move the weights by gradient descent to shrink the squared error between
// p and the actual result (1, 0.5, 0).
//
// The evaluation is linear in the tuned weights: E = sum(weight_i * count_i) +
// everything else. "Everything else" (king danger, passed-pawn race terms,
// endgame knowledge) is kept fixed and stored per position as a residual.
// The counts come from the engine itself, built with -DTUNE.
//
// Build and run (from the repo root):
//   g++ -O3 -std=c++17 -DTUNE -Iengine/src tools/tuner.cpp engine/src/bitboard.cpp \
//       engine/src/position.cpp engine/src/movegen.cpp engine/src/eval.cpp -o /tmp/tuner
//   ./engine/tessera datagen 5000 5000 1 > /tmp/data.txt     # self-play positions
//   /tmp/tuner /tmp/data.txt engine/src/eval_params.h         # rewrites the weights
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
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

struct Term { unsigned short index; signed char count; };
struct Sample {
    unsigned first, n;          // range in the shared term array
    float resMg, resEg;         // parts of the eval that are not tuned
    float mgFactor, egFactor;   // d(E)/d(mg), d(E)/d(eg) from phase, scale and fifty-move damping
    float tempo;                // +16 or -16 times damping
    float result;
};

static std::vector<Term> Terms;
static std::vector<Sample> Samples;
static double Mg[F_COUNT], Eg[F_COUNT];
static const double Tempo = 16.0;

static double predict_cp(const Sample& s) {
    double mg = s.resMg, eg = s.resEg;
    for (unsigned i = s.first; i < s.first + s.n; i++) {
        mg += Mg[Terms[i].index] * Terms[i].count;
        eg += Eg[Terms[i].index] * Terms[i].count;
    }
    return mg * s.mgFactor + eg * s.egFactor + s.tempo;
}

static double sigmoid(double cp, double K) { return 1.0 / (1.0 + std::pow(10.0, -K * cp / 400.0)); }

// Positions of one game sit next to each other in the data and share a result, so the
// validation set must be whole games the tuner never saw: the last tenth of the file.
static size_t ValStart = 0;
static inline bool is_validation(size_t i) { return i >= ValStart; }

static double loss(double K, int part) {   // part: 0 = training, 1 = validation
    double sum = 0;
    size_t n = 0;
    for (size_t i = 0; i < Samples.size(); i++) {
        if (is_validation(i) != (part == 1)) continue;
        double d = Samples[i].result - sigmoid(predict_cp(Samples[i]), K);
        sum += d * d;
        n++;
    }
    return sum / (n ? n : 1);
}

static void write_params(const char* path) {
    FILE* f = std::fopen(path, "w");
    if (!f) { std::perror(path); std::exit(1); }
    auto emit = [&](int i) { std::fprintf(f, "S(%d, %d),", (int)std::lround(Mg[i]), (int)std::lround(Eg[i])); };
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
        for (int r = 0; r < 8; r++) {
            std::fprintf(f, "        ");
            for (int c = 0; c < 8; c++) { emit(F_PSQ + pt * 64 + r * 8 + c); std::fprintf(f, c < 7 ? " " : "\n"); }
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
        std::fprintf(f, "static const int %s = S(%d, %d);\n", s.name, (int)std::lround(Mg[s.index]), (int)std::lround(Eg[s.index]));
    std::fclose(f);
}

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: tuner <data.txt> <out eval_params.h> [epochs] [learning rate] [tie|free]\n"); return 1; }
    const int epochs = argc > 3 ? std::atoi(argv[3]) : 600;
    // "tie" (default) tunes one value per piece type and keeps the piece-square shapes;
    // "free" tunes all 384 squares separately, which needs far more games to be safe.
    const bool tiePsq = !(argc > 5 && std::string(argv[5]) == "free");
    bitboards_init();
    position_init();
    eval_init();

    // Starting point: the weights currently compiled in.
    for (int i = 0; i < 384; i++) { Mg[F_PSQ + i] = mg_of(PsqTable[i / 64][i % 64]); Eg[F_PSQ + i] = eg_of(PsqTable[i / 64][i % 64]); }
    for (const Group& g : Groups)
        for (int i = 0; i < g.count; i++) { Mg[g.first + i] = mg_of(g.values[i]); Eg[g.first + i] = eg_of(g.values[i]); }
    for (const Scalar& s : Scalars) { Mg[s.index] = mg_of(*s.value); Eg[s.index] = eg_of(*s.value); }

    // Load positions and record which weights each one uses.
    static Position pos;
    std::ifstream in(argv[1]);
    std::string line;
    size_t bad = 0, mismatch = 0;
    std::vector<int> seen(F_COUNT, 0);
    while (std::getline(in, line)) {
        size_t semi = line.find(';');
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
            lmg += Mg[i] * Trace.coeff[i];
            leg += Eg[i] * Trace.coeff[i];
            seen[i]++;
        }
        s.n = (unsigned)Terms.size() - s.first;
        s.resMg = (float)(Trace.mg - lmg);
        s.resEg = (float)(Trace.eg - leg);
        double damp = (200 - (Trace.halfmove < 100 ? Trace.halfmove : 100)) / 200.0;
        s.mgFactor = (float)(Trace.phase / 24.0 * damp);
        s.egFactor = (float)((24 - Trace.phase) / 24.0 * Trace.scale / 64.0 * damp);
        s.tempo = (float)((pos.stm == WHITE ? Tempo : -Tempo) * damp);
        s.result = (float)std::atof(line.c_str() + semi + 1);
        // The reconstruction must agree with the engine's own answer.
        double white = predict_cp(s), engine = pos.stm == WHITE ? value : -value;
        if (std::fabs(white - engine) > 3.0) mismatch++;
        Samples.push_back(s);
    }
    std::printf("positions %zu, terms %zu, unreadable %zu, reconstruction mismatches %zu\n",
                Samples.size(), Terms.size(), bad, mismatch);
    if (Samples.size() < 1000 || mismatch > Samples.size() / 1000) { std::printf("refusing to tune on this data\n"); return 1; }

    ValStart = Samples.size() - Samples.size() / 10;

    // Fit the curve steepness K to the current weights, then keep it fixed.
    double K = 1.0, best = 1e9;
    for (double k = 0.4; k <= 2.6; k += 0.02) {
        double l = loss(k, 0);
        if (l < best) { best = l; K = k; }
    }
    std::printf("K = %.2f   start: train %.6f  validation %.6f\n", K, loss(K, 0), loss(K, 1));

    // Adam gradient descent over all positions at once.
    std::vector<double> gMg(F_COUNT), gEg(F_COUNT), m1(2 * F_COUNT, 0), m2(2 * F_COUNT, 0);
    const double lr = argc > 4 ? std::atof(argv[4]) : 0.5, b1 = 0.9, b2 = 0.999, eps = 1e-8;
    const double c = K * std::log(10.0) / 400.0;
    size_t trainCount = 0;
    for (size_t i = 0; i < Samples.size(); i++) trainCount += !is_validation(i);
    double bestVal = loss(K, 1);
    std::vector<double> bestMg(Mg, Mg + F_COUNT), bestEg(Eg, Eg + F_COUNT);
    for (int epoch = 1; epoch <= epochs; epoch++) {
        std::fill(gMg.begin(), gMg.end(), 0.0);
        std::fill(gEg.begin(), gEg.end(), 0.0);
        for (size_t i = 0; i < Samples.size(); i++) {
            if (is_validation(i)) continue;
            const Sample& s = Samples[i];
            double p = sigmoid(predict_cp(s), K);
            double d = -2.0 * (s.result - p) * p * (1 - p) * c;
            for (unsigned t = s.first; t < s.first + s.n; t++) {
                gMg[Terms[t].index] += d * s.mgFactor * Terms[t].count;
                gEg[Terms[t].index] += d * s.egFactor * Terms[t].count;
            }
        }
        if (tiePsq) {
            // One shared step per piece type: the piece's value moves, the shape of its table stays.
            for (int pt = 0; pt < 6; pt++) {
                double sumMg = 0, sumEg = 0;
                for (int sq = 0; sq < 64; sq++) { sumMg += gMg[F_PSQ + pt * 64 + sq]; sumEg += gEg[F_PSQ + pt * 64 + sq]; }
                for (int sq = 0; sq < 64; sq++) { gMg[F_PSQ + pt * 64 + sq] = sumMg; gEg[F_PSQ + pt * 64 + sq] = sumEg; }
            }
        }
        for (int i = 0; i < F_COUNT; i++) {
            const bool psq = i < F_PSQ + 384;
            if (!(psq && tiePsq) && seen[i] < 20) continue;   // too rare to learn anything: keep the starting value
            for (int phase = 0; phase < 2; phase++) {
                double g = (phase ? gEg[i] : gMg[i]) / trainCount;
                double& a = m1[2 * i + phase];
                double& b = m2[2 * i + phase];
                a = b1 * a + (1 - b1) * g;
                b = b2 * b + (1 - b2) * g * g;
                double step = lr * (a / (1 - std::pow(b1, epoch))) / (std::sqrt(b / (1 - std::pow(b2, epoch))) + eps);
                (phase ? Eg[i] : Mg[i]) -= step;
            }
        }
        if (epoch % 10 == 0 || epoch == epochs) {
            double tr = loss(K, 0), va = loss(K, 1);
            if (va < bestVal) { bestVal = va; bestMg.assign(Mg, Mg + F_COUNT); bestEg.assign(Eg, Eg + F_COUNT); }
            if (epoch % 100 == 0 || epoch == epochs) std::printf("epoch %4d  train %.6f  validation %.6f\n", epoch, tr, va);
        }
    }
    // Keep the weights that did best on positions the tuner never trained on.
    std::copy(bestMg.begin(), bestMg.end(), Mg);
    std::copy(bestEg.begin(), bestEg.end(), Eg);
    std::printf("kept weights with validation %.6f\n", bestVal);

    // Table entries that almost never occur (a queen with 26 safe squares, a connected pawn
    // on the 7th rank) get fitted to noise. Give them the value of the nearest entry in the
    // same table that was seen often enough to trust.
    const int trusted = (int)(Samples.size() / 100);
    for (const Group& g : Groups) {
        for (int i = 0; i < g.count; i++) {
            if (seen[g.first + i] == 0 || seen[g.first + i] >= trusted) continue;
            for (int d = 1; d < g.count; d++) {
                int j = i - d >= 0 && seen[g.first + i - d] >= trusted ? i - d
                      : i + d < g.count && seen[g.first + i + d] >= trusted ? i + d : -1;
                if (j < 0) continue;
                std::printf("%s[%d] seen %d times: using the value of [%d]\n", g.name, i, seen[g.first + i], j);
                Mg[g.first + i] = bestMg[g.first + j];
                Eg[g.first + i] = bestEg[g.first + j];
                break;
            }
        }
    }

    // Average piece values, as a sanity check a human can read.
    static const char* names[5] = {"pawn", "knight", "bishop", "rook", "queen"};
    for (int pt = 0; pt < 5; pt++) {
        double a = 0, b = 0;
        int n = 0;
        for (int sq = (pt == 0 ? 8 : 0); sq < (pt == 0 ? 56 : 64); sq++) { a += Mg[F_PSQ + pt * 64 + sq]; b += Eg[F_PSQ + pt * 64 + sq]; n++; }
        std::printf("average %-6s mg %4.0f  eg %4.0f\n", names[pt], a / n, b / n);
    }
    write_params(argv[2]);
    std::printf("wrote %s\n", argv[2]);
    return 0;
}
