// Trainer for Tessera's neural-network evaluation. CPU only, no dependencies,
// safe to stop and resume.
//
//   g++ -O3 -march=native -std=c++17 -pthread tools/trainer.cpp -o trainer
//   ./trainer --data data/packed --out nets                # start or continue
//   Ctrl+C                                                 # stop: progress is saved
//
// Input: *.bin files made by `tessera pack` (68 bytes per position).
// Output: <out>/tessera.nnue (the best network so far; load it with: setoption name EvalFile ...)
//         <out>/checkpoint.bin (everything needed to continue training)
//
// The network (see engine/src/nnue.h): 768 inputs -> H hidden per side -> 1 output.
// What it learns: for each position, a number whose sigmoid should match a blend of
//   * the search score the engine gave the position, squashed to 0..1 (80%), and
//   * the game result, 1 / 0.5 / 0 for the side to move (20%).
// The mix matters a lot on small data: in matches a 50/50 blend was about 140 Elo weaker
// and score-only about 45 Elo weaker than 80/20 (--result-weight sets the result share).
// Training is plain mini-batch gradient descent with Adam; every thread works through
// its share of a batch, the gradients are added up, the weights move one step.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <numeric>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

static const int INPUTS = 768;
static const int QA = 255, QB = 64;
static const double SCALE = 181.0;      // centipawns per unit of network output (matches the engine's own score scale)

#pragma pack(push, 1)
struct Record {
    uint16_t input[32];   // network inputs from White's side, 768 = unused
    int16_t score;        // centipawns for White, 32767 = none
    uint8_t stm;          // 0 = White to move
    uint8_t result2;      // twice the game result for White, 255 = unknown (score-only data)
};
#pragma pack(pop)

struct Options {
    std::string data = "data/packed", out = "nets";
    int hidden = 128, epochs = 30, batch = 16384;
    int threads = std::max(1u, std::thread::hardware_concurrency());
    double lr = 0.001, lrEnd = 0.00003, resultWeight = 0.2;
};

// All trainable numbers in one flat array: input weights [768][H], hidden bias [H],
// output weights [2H], output bias [1].
struct Layout {
    int H;
    size_t ftW, ftB, outW, outB, count;
    explicit Layout(int h) : H(h) { ftW = 0; ftB = (size_t)INPUTS * h; outW = ftB + h; outB = outW + 2 * h; count = outB + 1; }
};

static std::atomic<bool> StopRequested{false};
static void on_signal(int) { StopRequested = true; }

static inline int mirror_input(int f) {          // the same piece seen from Black's side
    int color = f / 384, rest = f % 384;
    return (1 - color) * 384 + (rest / 64) * 64 + ((rest % 64) ^ 56);
}
static inline float sigmoidf(float x) { return 1.0f / (1.0f + std::exp(-x)); }

// Forward pass for one position; with `grad` non-null also the backward pass (adds into grad).
static float process(const Record& r, const Layout& L, const float* P, float* grad, double resultWeight) {
    const int H = L.H;
    int own[32], opp[32], n = 0;
    for (; n < 32 && r.input[n] < INPUTS; n++) {
        const int w = r.input[n], b = mirror_input(w);
        own[n] = r.stm == 0 ? w : b;       // inputs as seen by the side to move ...
        opp[n] = r.stm == 0 ? b : w;       // ... and by the other side
    }
    float accOwn[512], accOpp[512];
    std::memcpy(accOwn, P + L.ftB, sizeof(float) * H);
    std::memcpy(accOpp, P + L.ftB, sizeof(float) * H);
    for (int k = 0; k < n; k++) {
        const float* a = P + L.ftW + (size_t)own[k] * H;
        const float* b = P + L.ftW + (size_t)opp[k] * H;
        for (int i = 0; i < H; i++) { accOwn[i] += a[i]; accOpp[i] += b[i]; }
    }
    float out = P[L.outB];
    float actOwn[512], actOpp[512];
    for (int i = 0; i < H; i++) {
        actOwn[i] = std::min(1.0f, std::max(0.0f, accOwn[i]));
        actOpp[i] = std::min(1.0f, std::max(0.0f, accOpp[i]));
        out += actOwn[i] * actOwn[i] * P[L.outW + i] + actOpp[i] * actOpp[i] * P[L.outW + H + i];
    }
    const float predicted = sigmoidf(out);

    // Target: a blend of the game result and the squashed search score. Positions without a
    // result (imported evaluations) learn from the score alone.
    float result = r.result2 * 0.5f;
    if (r.stm) result = 1.0f - result;
    float target = result;
    if (r.score != 32767) {
        const float score = sigmoidf((r.stm ? -(float)r.score : (float)r.score) / (float)SCALE);
        target = r.result2 == 255 ? score : (float)(resultWeight * result + (1.0 - resultWeight) * score);
    }
    const float diff = predicted - target;
    if (!grad) return diff * diff;

    const float dOut = 2.0f * diff * predicted * (1.0f - predicted);
    grad[L.outB] += dOut;
    float dOwn[512], dOpp[512];
    for (int i = 0; i < H; i++) {
        grad[L.outW + i] += dOut * actOwn[i] * actOwn[i];
        grad[L.outW + H + i] += dOut * actOpp[i] * actOpp[i];
        // derivative of clamp(x)^2 is 2*clamp(x) inside (0,1) and 0 outside
        dOwn[i] = (accOwn[i] > 0.0f && accOwn[i] < 1.0f) ? dOut * P[L.outW + i] * 2.0f * actOwn[i] : 0.0f;
        dOpp[i] = (accOpp[i] > 0.0f && accOpp[i] < 1.0f) ? dOut * P[L.outW + H + i] * 2.0f * actOpp[i] : 0.0f;
        grad[L.ftB + i] += dOwn[i] + dOpp[i];
    }
    for (int k = 0; k < n; k++) {
        float* a = grad + L.ftW + (size_t)own[k] * H;
        float* b = grad + L.ftW + (size_t)opp[k] * H;
        for (int i = 0; i < H; i++) { a[i] += dOwn[i]; b[i] += dOpp[i]; }
    }
    return diff * diff;
}

static void write_atomic(const fs::path& path, const std::vector<char>& bytes) {
    fs::path tmp = path;
    tmp += ".tmp";
    FILE* f = std::fopen(tmp.string().c_str(), "wb");
    if (!f) { std::perror(tmp.string().c_str()); std::exit(1); }
    std::fwrite(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
    fs::rename(tmp, path);     // the old file is replaced in one step, never half-written
}

template <class T> static void put(std::vector<char>& b, const T& v) { const char* p = (const char*)&v; b.insert(b.end(), p, p + sizeof(T)); }

static void export_network(const fs::path& path, const Layout& L, const std::vector<float>& P) {
    std::vector<char> b = {'T', 'S', 'N', 'N'};
    put<uint32_t>(b, 1); put<uint32_t>(b, (uint32_t)L.H); put<uint32_t>(b, (uint32_t)SCALE); put<uint32_t>(b, QA); put<uint32_t>(b, QB);
    auto q16 = [](double v) { return (int16_t)std::lround(std::max(-32767.0, std::min(32767.0, v))); };
    for (size_t i = 0; i < (size_t)INPUTS * L.H; i++) put<int16_t>(b, q16(P[L.ftW + i] * QA));
    for (int i = 0; i < L.H; i++) put<int16_t>(b, q16(P[L.ftB + i] * QA));
    for (int i = 0; i < 2 * L.H; i++) put<int16_t>(b, q16(P[L.outW + i] * QB));
    put<int32_t>(b, (int32_t)std::lround(P[L.outB] * QA * QB));
    write_atomic(path, b);
}

struct Checkpoint { uint32_t magic = 0x54434B51, hidden = 0, epoch = 0, batch = 0; uint64_t step = 0, positions = 0; double bestVal = 1e9; uint32_t bestEpoch = 0, pad = 0; };

int main(int argc, char** argv) {
    Options opt;
    for (int i = 1; i + 1 < argc; i += 2) {
        std::string k = argv[i], v = argv[i + 1];
        if (k == "--data") opt.data = v;
        else if (k == "--out") opt.out = v;
        else if (k == "--hidden") opt.hidden = std::atoi(v.c_str());
        else if (k == "--epochs") opt.epochs = std::atoi(v.c_str());
        else if (k == "--batch") opt.batch = std::atoi(v.c_str());
        else if (k == "--threads") opt.threads = std::max(1, std::atoi(v.c_str()));
        else if (k == "--lr") opt.lr = std::atof(v.c_str());
        else if (k == "--result-weight") opt.resultWeight = std::atof(v.c_str());
        else { std::fprintf(stderr, "unknown option %s\n", k.c_str()); return 1; }
    }
    if (opt.hidden < 1 || opt.hidden > 256) { std::fprintf(stderr, "--hidden must be 1..256\n"); return 1; }

    // ---- data: every file is split into a training part and a validation tail (2%).
    // Positions of a game are neighbours in the file, so a tail keeps whole games unseen.
    std::vector<fs::path> files;
    for (size_t start = 0; start <= opt.data.size();) {          // --data accepts "dir1,dir2,..."
        size_t comma = opt.data.find(',', start);
        if (comma == std::string::npos) comma = opt.data.size();
        const fs::path dir = opt.data.substr(start, comma - start);
        std::vector<fs::path> here;
        if (!dir.empty() && fs::exists(dir))
            for (auto& e : fs::directory_iterator(dir))
                if (e.path().extension() == ".bin") here.push_back(e.path());
        std::sort(here.begin(), here.end());
        files.insert(files.end(), here.begin(), here.end());
        start = comma + 1;
    }
    std::vector<Record> data;
    std::vector<uint32_t> trainIndex, valIndex;
    for (auto& f : files) {
        const size_t n = fs::file_size(f) / sizeof(Record), base = data.size();
        data.resize(base + n);
        FILE* in = std::fopen(f.string().c_str(), "rb");
        if (!in || std::fread(data.data() + base, sizeof(Record), n, in) != n) { std::fprintf(stderr, "cannot read %s\n", f.string().c_str()); return 1; }
        std::fclose(in);
        const size_t val = n / 50;
        for (size_t i = 0; i < n; i++) {
            const Record& r = data[base + i];
            if (r.score == 32767 && r.result2 == 255) continue;   // nothing to learn from
            (i < n - val ? trainIndex : valIndex).push_back((uint32_t)(base + i));
        }
    }
    if (trainIndex.size() < 100000) { std::fprintf(stderr, "Not enough data in %s (found %zu positions). Run `tessera pack` first.\n", opt.data.c_str(), data.size()); return 1; }
    std::printf("%zu positions in %zu files: %zu for training, %zu held out\n", data.size(), files.size(), trainIndex.size(), valIndex.size());

    // ---- parameters, fresh or from the checkpoint
    const Layout L(opt.hidden);
    std::vector<float> P(L.count), m1(L.count, 0.0f), m2(L.count, 0.0f);
    Checkpoint ck;
    ck.hidden = opt.hidden;
    fs::create_directories(opt.out);
    const fs::path ckPath = fs::path(opt.out) / "checkpoint.bin", netPath = fs::path(opt.out) / "tessera.nnue";
    bool resumed = false;
    if (fs::exists(ckPath)) {
        FILE* in = std::fopen(ckPath.string().c_str(), "rb");
        Checkpoint c;
        if (in && std::fread(&c, sizeof(c), 1, in) == 1 && c.magic == ck.magic && (int)c.hidden == opt.hidden
            && std::fread(P.data(), sizeof(float), L.count, in) == L.count
            && std::fread(m1.data(), sizeof(float), L.count, in) == L.count
            && std::fread(m2.data(), sizeof(float), L.count, in) == L.count) {
            ck = c;
            resumed = true;
        } else {
            std::printf("checkpoint does not fit these settings (different --hidden?): starting fresh\n");
        }
        if (in) std::fclose(in);
    }
    if (!resumed) {
        std::mt19937 rng(20261111);
        std::uniform_real_distribution<float> a(-0.1f, 0.1f), b(-0.05f, 0.05f);
        for (size_t i = 0; i < L.ftB; i++) P[i] = a(rng);
        for (size_t i = L.ftB; i < L.outW; i++) P[i] = 0.2f;
        for (size_t i = L.outW; i < L.outB; i++) P[i] = b(rng);
        P[L.outB] = 0.0f;
        std::fill(m1.begin(), m1.end(), 0.0f);
        std::fill(m2.begin(), m2.end(), 0.0f);
    }
    if (ck.positions && ck.positions != trainIndex.size() && ck.batch) {
        std::printf("the data changed since the last run: restarting the interrupted pass\n");
        ck.batch = 0;
    }
    ck.positions = trainIndex.size();
    std::printf("%s: hidden %d, %d threads, pass %u of %d\n", resumed ? "Continuing" : "Starting", opt.hidden, opt.threads, ck.epoch + 1, opt.epochs);

    auto save = [&]() {
        std::vector<char> b;
        put(b, ck);
        for (auto* v : {&P, &m1, &m2}) b.insert(b.end(), (const char*)v->data(), (const char*)(v->data() + L.count));
        write_atomic(ckPath, b);
    };
    auto validation_loss = [&]() {
        std::vector<double> part(opt.threads, 0.0);
        std::vector<std::thread> pool;
        for (int t = 0; t < opt.threads; t++)
            pool.emplace_back([&, t]() {
                for (size_t i = t; i < valIndex.size(); i += opt.threads) part[t] += process(data[valIndex[i]], L, P.data(), nullptr, opt.resultWeight);
            });
        for (auto& th : pool) th.join();
        return std::accumulate(part.begin(), part.end(), 0.0) / std::max<size_t>(1, valIndex.size());
    };

    std::signal(SIGINT, on_signal);
#ifdef SIGTERM
    std::signal(SIGTERM, on_signal);
#endif

    std::vector<std::vector<float>> grads(opt.threads, std::vector<float>(L.count));
    std::vector<double> lossPart(opt.threads);
    const size_t batches = trainIndex.size() / opt.batch;
    const double b1 = 0.9, b2 = 0.999;

    for (; ck.epoch < (uint32_t)opt.epochs && !StopRequested; ck.epoch++, ck.batch = 0) {
        // The shuffle depends only on the pass number, so an interrupted pass can be continued exactly.
        std::vector<uint32_t> order = trainIndex;
        std::mt19937_64 rng(1000 + ck.epoch);
        std::shuffle(order.begin(), order.end(), rng);
        const double progress = opt.epochs > 1 ? (double)ck.epoch / (opt.epochs - 1) : 0.0;
        const double lr = opt.lr * std::pow(opt.lrEnd / opt.lr, progress);      // smooth decay from lr to lrEnd
        double lossSum = 0;
        size_t lossCount = 0;
        const auto start = std::chrono::steady_clock::now();
        const uint32_t firstBatch = ck.batch;

        for (; ck.batch < batches && !StopRequested; ck.batch++) {
            const uint32_t* idx = order.data() + (size_t)ck.batch * opt.batch;
            std::vector<std::thread> pool;
            for (int t = 0; t < opt.threads; t++)
                pool.emplace_back([&, t]() {
                    std::fill(grads[t].begin(), grads[t].end(), 0.0f);
                    double loss = 0;
                    for (int i = t; i < opt.batch; i += opt.threads) loss += process(data[idx[i]], L, P.data(), grads[t].data(), opt.resultWeight);
                    lossPart[t] = loss;
                });
            for (auto& th : pool) th.join();
            for (int t = 0; t < opt.threads; t++) lossSum += lossPart[t];
            lossCount += opt.batch;

            ck.step++;
            const double c1 = 1.0 - std::pow(b1, (double)ck.step), c2 = 1.0 - std::pow(b2, (double)ck.step);
            const float scale = 1.0f / opt.batch;
            for (size_t i = 0; i < L.count; i++) {
                float g = 0;
                for (int t = 0; t < opt.threads; t++) g += grads[t][i];
                g *= scale;
                m1[i] = (float)(b1 * m1[i] + (1 - b1) * g);
                m2[i] = (float)(b2 * m2[i] + (1 - b2) * g * g);
                P[i] -= (float)(lr * (m1[i] / c1) / (std::sqrt(m2[i] / c2) + 1e-8));
            }
            // Keep the hidden layer inside what 16-bit integers can hold after quantisation.
            for (size_t i = 0; i < L.ftB; i++) P[i] = std::max(-3.9f, std::min(3.9f, P[i]));
            for (size_t i = L.ftB; i < L.outW; i++) P[i] = std::max(-3.0f, std::min(3.0f, P[i]));

            if (ck.batch % 50 == 49 || ck.batch + 1 == batches) {
                const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
                const double perSec = (double)(ck.batch + 1 - firstBatch) * opt.batch / std::max(sec, 1e-3);
                std::printf("\rpass %u/%d  %5.1f%%  loss %.5f  %.0f positions/s   ", ck.epoch + 1, opt.epochs,
                            100.0 * (ck.batch + 1) / batches, lossSum / lossCount, perSec);
                std::fflush(stdout);
            }
        }
        if (StopRequested) {            // stopped in the middle of a pass: remember where
            save();
            std::printf("\nStopped. Progress saved; run the same command to continue.\n");
            return 0;
        }
        const double val = validation_loss();
        // Only a network that does better on games it never saw replaces the saved one. If
        // training starts to memorise its data (held-out loss rising), the best one is kept.
        const bool best = val < ck.bestVal;
        if (best) { ck.bestVal = val; ck.bestEpoch = ck.epoch + 1; export_network(netPath, L, P); }
        std::printf("\rpass %u/%d  done    loss %.5f  held-out loss %.5f%s                    \n", ck.epoch + 1, opt.epochs,
                    lossSum / std::max<size_t>(1, lossCount), val, best ? "  (best so far, network saved)" : "");
        std::fflush(stdout);
        uint32_t e = ck.epoch, b = ck.batch;
        ck.epoch = e + 1; ck.batch = 0;
        save();                         // a finished pass is always saved
        ck.epoch = e; ck.batch = b;
    }
    if (StopRequested) { save(); std::printf("Stopped. Progress saved; run the same command to continue.\n"); return 0; }
    std::printf("Training finished. Best network (from pass %u, held-out loss %.5f): %s\n", ck.bestEpoch, ck.bestVal, netPath.string().c_str());
    return 0;
}
