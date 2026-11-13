// "Mistake school": Tessera as its own teacher.
//
//   lessons <games.txt> <lessons out> <exam out> <nodes> <count> <gap permille>
//   exam <exam.txt> <gap permille>
//
// Both are given on the engine's input (after `setoption name EvalFile ...` if a network file
// is wanted). `lessons` reads self-play data (fen;score;result, score from White's side) and
// looks for positions the network misjudges: its instant evaluation and the search score of
// the game disagree by at least <gap> (in win probability, thousandths). Up to <count> of
// them, spread evenly over the file, are searched again with <nodes> nodes, far deeper than
// the game was played. Where the deep search confirms the network was wrong, the position is
// written with the deep score: a lesson. Every 20th lesson goes to the exam file instead and
// is never trained on. `exam` counts how many exam positions a network now judges within
// <gap> of the deep score: mistakes it no longer makes.
#pragma once
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "eval.h"
#include "search.h"

static double lesson_winprob(int cp) { return 1.0 / (1.0 + std::exp(-cp / 181.0)); }

static bool lesson_parse(const std::string& line, std::string& fen, int& score, std::string& result) {
    const size_t a = line.find(';');
    const size_t b = a == std::string::npos ? a : line.find(';', a + 1);
    if (b == std::string::npos) return false;
    fen = line.substr(0, a);
    score = std::atoi(line.substr(a + 1, b - a - 1).c_str());
    result = line.substr(b + 1);
    while (!result.empty() && (result.back() == '\r' || result.back() == '\n' || result.back() == ' ')) result.pop_back();
    return true;
}

static int lessons_run(const std::string& args) {
    std::istringstream is(args);
    std::string cmd, inPath, outPath, examPath;
    long long nodes = 0, count = 0;
    int gapPermille = 150;
    is >> cmd >> inPath >> outPath >> examPath >> nodes >> count >> gapPermille;
    std::ifstream in(inPath);
    if (!in || nodes <= 0 || count <= 0) { std::printf("lessons failed: bad arguments or cannot open %s\n", inPath.c_str()); return 1; }
    const double gap = gapPermille / 1000.0;
    static Position pos;
    std::vector<std::string> lines, candidates;
    std::string line, fen, result;
    int score;
    while (std::getline(in, line)) {
        if (!lesson_parse(line, fen, score, result) || !pos.set(fen.c_str())) continue;
        const int stmScore = pos.stm == WHITE ? score : -score;
        if (std::fabs(lesson_winprob(evaluate(pos)) - lesson_winprob(stmScore)) >= gap) candidates.push_back(line);
        lines.push_back(std::string());            // only counted
    }
    const size_t total = lines.size();
    std::ofstream out(outPath), exam(examPath);
    if (!out || !exam) { std::printf("lessons failed: cannot write %s\n", outPath.c_str()); return 1; }
    Limits lim;
    lim.depth = 0; lim.nodes = (u64)nodes; lim.movetime = 0; lim.movestogo = 0; lim.infinite = false;
    lim.time[0] = lim.time[1] = lim.inc[0] = lim.inc[1] = 0;
    OpeningVariety = false;
    search_set_level(10);
    const size_t take = candidates.size() < (size_t)count ? candidates.size() : (size_t)count;
    long long written = 0, examined = 0, searched = 0;
    for (size_t i = 0; i < take; i++) {
        const std::string& c = candidates[i * candidates.size() / take];     // evenly spread over the file
        lesson_parse(c, fen, score, result);
        pos.set(fen.c_str());
        const int quick = evaluate(pos);
        SearchStop = false;
        const SearchResult r = search_go(pos, lim, false);
        searched++;
        if (std::abs(r.score) >= 1500) continue;                               // same cut as the self-play data
        if (std::fabs(lesson_winprob(quick) - lesson_winprob(r.score)) < gap) continue;   // the network was right after all
        const int white = pos.stm == WHITE ? r.score : -r.score;
        std::ofstream& dst = ((written + examined) % 20 == 19) ? exam : out;
        dst << fen << ';' << white << ';' << result << '\n';
        if (&dst == &exam) examined++; else written++;
    }
    out.close();
    exam.close();
    std::printf("lessons positions=%zu candidates=%zu searched=%lld written=%lld exam=%lld\n",
                total, candidates.size(), searched, written, examined);
    std::fflush(stdout);
    return 0;
}

static int exam_run(const std::string& args) {
    std::istringstream is(args);
    std::string cmd, path;
    int gapPermille = 150;
    is >> cmd >> path >> gapPermille;
    std::ifstream in(path);
    if (!in) { std::printf("exam failed: cannot open %s\n", path.c_str()); return 1; }
    static Position pos;
    std::string line, fen, result;
    int score;
    long long n = 0, solved = 0;
    while (std::getline(in, line)) {
        if (!lesson_parse(line, fen, score, result) || !pos.set(fen.c_str())) continue;
        const int stmScore = pos.stm == WHITE ? score : -score;
        n++;
        if (std::fabs(lesson_winprob(evaluate(pos)) - lesson_winprob(stmScore)) < gapPermille / 1000.0) solved++;
    }
    std::printf("exam positions=%lld solved=%lld\n", n, solved);
    std::fflush(stdout);
    return 0;
}
