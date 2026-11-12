// Native entry point: reads UCI from stdin. `go` runs on a worker thread so
// that `stop` and `isready` can be answered while the engine is thinking.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

#include "platform.h"
#include "position.h"
#include "search.h"
#include "uci.h"

u64 plat_now_ms() {
    using namespace std::chrono;
    return (u64)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

void plat_out(const char* line) {
    std::printf("%s\n", line);
    std::fflush(stdout);
}

void* plat_big_realloc(void* old, size_t, size_t newBytes) {
    void* p = std::calloc(1, newBytes);
    if (p) std::free(old);
    return p;
}

const unsigned char* plat_read_file(const char* path, size_t* size) {
    static std::string buffer;   // one file at a time is enough: the network
    std::ifstream in(path, std::ios::binary);
    if (!in) return nullptr;
    buffer.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    *size = buffer.size();
    return (const unsigned char*)buffer.data();
}

// One training record (68 bytes): 32 network input numbers from White's side (768 = unused
// slot), the score for White in centipawns (32767 = none), side to move (0 = White) and twice
// the game result for White (255 = no result known, e.g. positions that come without a game).
static void make_record(const Position& pos, int score, int result2, unsigned char rec[68]) {
    int n = 0;
    for (Bitboard b = pos.all(); b && n < 32;) {
        int sq = pop_lsb(b);
        int f = nnue_input(WHITE, pos.board[sq], sq);
        rec[2 * n] = (unsigned char)(f & 255); rec[2 * n + 1] = (unsigned char)(f >> 8);
        n++;
    }
    for (; n < 32; n++) { rec[2 * n] = 768 & 255; rec[2 * n + 1] = 768 >> 8; }
    if (score != 32767) score = score > 32000 ? 32000 : score < -32000 ? -32000 : score;
    rec[64] = (unsigned char)(score & 255); rec[65] = (unsigned char)((score >> 8) & 255);
    rec[66] = (unsigned char)pos.stm;
    rec[67] = (unsigned char)result2;
}

// tessera pack <in.txt> <out.bin>
// Converts "fen;score;result" lines (or "fen;result") into training records. A result of "-"
// means "no game result" (score-only data).
static int pack_file(const char* inPath, const char* outPath) {
    std::ifstream in(inPath);
    FILE* out = std::fopen(outPath, "wb");
    if (!in || !out) { std::fprintf(stderr, "pack: cannot open files\n"); return 1; }
    static Position pos;
    std::string line;
    size_t written = 0, skipped = 0;
    while (std::getline(in, line)) {
        size_t first = line.find(';'), last = line.rfind(';');
        if (first == std::string::npos || !pos.set(line.substr(0, first).c_str())) { skipped++; continue; }
        const int score = last != first ? std::atoi(line.c_str() + first + 1) : 32767;
        const char* res = line.c_str() + last + 1;
        const int result2 = res[0] == '-' && res[1] == 0 ? 255 : (int)(std::atof(res) * 2 + 0.5);
        unsigned char rec[68];
        make_record(pos, score, result2, rec);
        std::fwrite(rec, 1, sizeof(rec), out);
        written++;
    }
    std::fclose(out);
    std::printf("packed %zu positions (%zu lines skipped)\n", written, skipped);
    return 0;
}

#include "lichess_import.h"

int main(int argc, char** argv) {
    engine_init();
    if (argc == 4 && std::strcmp(argv[1], "pack") == 0) return pack_file(argv[2], argv[3]);
    if (argc >= 2 && std::strncmp(argv[1], "lichess", 7) == 0) return lichess_command(argc, argv);

    // Commands given on the command line run once and exit, e.g. `tessera bench`.
    if (argc > 1) {
        std::string cmd;
        for (int i = 1; i < argc; i++) { if (i > 1) cmd += ' '; cmd += argv[i]; }
        SearchStop = false;
        uci_command(cmd.c_str());
        return 0;
    }

    std::thread worker;
    auto finish = [&](bool stop) {
        if (stop) SearchStop = true;
        if (worker.joinable()) worker.join();
    };

    std::string line;
    while (std::getline(std::cin, line)) {
        size_t a = line.find_first_not_of(" \t\r\n");
        if (a == std::string::npos) continue;
        std::string cmd = line.substr(a, line.find_first_of(" \t\r\n", a) - a);

        if (cmd == "quit") break;
        if (cmd == "stop") { finish(true); continue; }
        if (cmd == "isready") { plat_out("readyok"); continue; }
        if (cmd == "go") {
            finish(true);
            SearchStop = false;
            worker = std::thread([line] { uci_command(line.c_str()); });
            continue;
        }
        finish(true);
        SearchStop = false;
        uci_command(line.c_str());
    }
    finish(true);
    return 0;
}
