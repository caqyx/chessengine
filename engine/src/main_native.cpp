// Native entry point: reads UCI from stdin. `go` runs on a worker thread so
// that `stop` and `isready` can be answered while the engine is thinking.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

#include "platform.h"
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

int main(int argc, char** argv) {
    engine_init();

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
