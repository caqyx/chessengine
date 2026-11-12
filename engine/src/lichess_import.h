// Import of the Lichess evaluation database (lichess_db_eval.jsonl, public domain / CC0):
// positions evaluated by Stockfish, one JSON object per line:
//   {"fen":"<pieces> <side> <castling> <ep>","evals":[{"pvs":[{"cp":31,"line":"e2e4 ..."},...],
//    "knodes":..,"depth":..},...]}
// For each position the evaluation with the highest depth is used, and its first line, as
// Lichess recommends. Only quiet positions are kept (side to move not in check, first move of
// the line not a capture or promotion), like Tessera's own self-play data.
//
// Stockfish's centipawns are not Tessera's centipawns, and the database does not say whether
// scores are from White's side or the side to move. `lichess-calibrate` answers both by
// letting Tessera search a sample of positions and comparing: the point of view whose scores
// agree with Tessera's (higher correlation) wins, and the scale is the ratio of the spreads.
//
//   tessera lichess-calibrate <file.jsonl> <positions> <nodes>
//   tessera lichess-import <file.jsonl | -> <out folder> <scale> <white|stm> <max records> <skip lines> <first chunk>
//
// The import writes training records (see make_record) in chunks of CHUNK records,
// <out>/lichess-NNNN.bin, each written to a temporary name and renamed when complete, and
// reports how many input lines each finished chunk consumed, so a stopped import can resume.
#pragma once
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include "search.h"
#include "tt.h"

struct LichessEntry {
    std::string fen;
    int cp = 0;               // centipawns, as stored in the file
    std::string move;         // first move of the chosen line, UCI_Chess960 notation
};

// Position of the matching closing bracket for the opening one at s[i], skipping strings.
static size_t json_match(const std::string& s, size_t i) {
    const char open = s[i], close = open == '{' ? '}' : ']';
    int depth = 0;
    bool inString = false;
    for (; i < s.size(); i++) {
        const char c = s[i];
        if (inString) {
            if (c == '\\') i++;
            else if (c == '"') inString = false;
        } else if (c == '"') inString = true;
        else if (c == open) depth++;
        else if (c == close && --depth == 0) return i;
    }
    return std::string::npos;
}

// Integer value of "key": inside s[from, to); returns false if absent.
static bool json_int(const std::string& s, size_t from, size_t to, const char* key, long long& out) {
    const size_t k = s.find(key, from);
    if (k == std::string::npos || k >= to) return false;
    size_t p = k + std::strlen(key);
    while (p < to && (s[p] == ' ' || s[p] == ':')) p++;
    if (p >= to) return false;
    char* end = nullptr;
    out = std::strtoll(s.c_str() + p, &end, 10);
    return end != s.c_str() + p;
}

// The deepest evaluation's first line. False for malformed lines and for mate scores.
static bool parse_lichess_line(const std::string& s, LichessEntry& e) {
    const size_t f = s.find("\"fen\"");
    if (f == std::string::npos) return false;
    const size_t q1 = s.find('"', s.find(':', f) + 1);
    const size_t q2 = q1 == std::string::npos ? q1 : s.find('"', q1 + 1);
    if (q2 == std::string::npos) return false;
    e.fen = s.substr(q1 + 1, q2 - q1 - 1);

    const size_t ev = s.find("\"evals\"");
    if (ev == std::string::npos) return false;
    size_t i = s.find('[', ev);
    if (i == std::string::npos) return false;
    const size_t evEnd = json_match(s, i);
    if (evEnd == std::string::npos) return false;
    long long bestDepth = -1;
    bool found = false;
    for (i = i + 1; i < evEnd; i++) {
        if (s[i] != '{') continue;
        const size_t objEnd = json_match(s, i);
        if (objEnd == std::string::npos || objEnd > evEnd) return false;
        long long depth = 0;
        json_int(s, i, objEnd, "\"depth\"", depth);
        const size_t pvs = s.find("\"pvs\"", i);
        if (pvs != std::string::npos && pvs < objEnd && depth > bestDepth) {
            const size_t pvStart = s.find('{', pvs);
            const size_t pvEnd = pvStart == std::string::npos ? pvStart : json_match(s, pvStart);
            if (pvEnd != std::string::npos && pvEnd < objEnd) {
                long long cp = 0;
                const size_t ln = s.find("\"line\"", pvStart);
                if (ln != std::string::npos && ln < pvEnd) {
                    const size_t l1 = s.find('"', s.find(':', ln) + 1);
                    size_t l2 = l1 + 1;
                    while (l2 < pvEnd && s[l2] != ' ' && s[l2] != '"') l2++;
                    bestDepth = depth;
                    if (json_int(s, pvStart, pvEnd, "\"cp\"", cp)) {
                        e.cp = (int)cp;
                        e.move = s.substr(l1 + 1, l2 - l1 - 1);
                        found = true;
                    } else {
                        found = false;   // the deepest evaluation is a mate score: not used
                    }
                }
            }
        }
        i = objEnd;
    }
    return found;
}

// Quiet = not in check, and the move is a legal-looking non-capture, non-promotion.
// Castling may be written as king-takes-own-rook (Chess960 notation) or as a two-square king move.
static bool lichess_quiet(const Position& pos, const std::string& mv) {
    if (pos.in_check() || mv.size() < 4) return false;
    if (mv[0] < 'a' || mv[0] > 'h' || mv[1] < '1' || mv[1] > '8' || mv[2] < 'a' || mv[2] > 'h' || mv[3] < '1' || mv[3] > '8')
        return false;
    if (mv.size() > 4) return false;                       // promotion
    const int from = make_sq(mv[0] - 'a', mv[1] - '1'), to = make_sq(mv[2] - 'a', mv[3] - '1');
    const int pc = pos.board[from], target = pos.board[to];
    if (pc == NO_PIECE || piece_color(pc) != pos.stm) return false;
    if (target != NO_PIECE) {
        // Chess960-style castling: king "captures" its own rook
        return piece_type(pc) == KING && target == make_piece(pos.stm, ROOK);
    }
    if (piece_type(pc) == PAWN) {
        if (file_of(from) != file_of(to)) return false;     // en passant
        if (relative_rank(pos.stm, to) == 7) return false;   // promotion without a piece letter
    }
    return true;
}

static int lichess_calibrate(const char* path, int count, u64 nodes) {
    std::ifstream in(path);
    if (!in) { std::fprintf(stderr, "cannot open %s\n", path); return 1; }
    static Position pos;
    Limits lim;
    lim.depth = 0; lim.nodes = nodes; lim.movetime = 0; lim.movestogo = 0; lim.infinite = false;
    lim.time[0] = lim.time[1] = lim.inc[0] = lim.inc[1] = 0;
    OpeningVariety = false;
    search_set_level(10);
    std::vector<double> sfWhite, sfStm, tess;
    std::string line;
    LichessEntry e;
    while ((int)tess.size() < count && std::getline(in, line)) {
        if (!parse_lichess_line(line, e) || std::abs(e.cp) > 600 || !pos.set(e.fen.c_str()) || !lichess_quiet(pos, e.move)) continue;
        SearchStop = false;
        const SearchResult r = search_go(pos, lim, false);
        if (std::abs(r.score) >= 1500) continue;
        tess.push_back(pos.stm == WHITE ? r.score : -r.score);       // Tessera, White's view
        sfWhite.push_back(e.cp);                                      // if the file uses White's view
        sfStm.push_back(pos.stm == WHITE ? e.cp : -e.cp);             // if it uses the side to move's
    }
    const size_t n = tess.size();
    if (n < 100) { std::printf("calibrate failed: only %zu usable positions\n", n); return 1; }
    auto stats = [&](const std::vector<double>& x, double& corr, double& ratio) {
        double mx = 0, my = 0;
        for (size_t i = 0; i < n; i++) { mx += x[i]; my += tess[i]; }
        mx /= n; my /= n;
        double sxx = 0, syy = 0, sxy = 0;
        for (size_t i = 0; i < n; i++) {
            sxx += (x[i] - mx) * (x[i] - mx); syy += (tess[i] - my) * (tess[i] - my); sxy += (x[i] - mx) * (tess[i] - my);
        }
        corr = sxy / std::sqrt(sxx * syy + 1e-9);
        ratio = std::sqrt(syy / (sxx + 1e-9));   // spread of Tessera's scores per unit of Stockfish's
    };
    double cw, rw, cs, rs;
    stats(sfWhite, cw, rw);
    stats(sfStm, cs, rs);
    const bool white = cw >= cs;
    std::printf("calibrate positions=%zu pov=%s scale=%.4f corr_white=%.3f corr_stm=%.3f\n", n, white ? "white" : "stm",
                white ? rw : rs, cw, cs);
    return 0;
}

static int lichess_import(const char* inPath, const std::string& outDir, double scale, bool stmView,
                          unsigned long long maxRecords, unsigned long long skipLines, int chunk, unsigned long long CHUNK) {
    std::istream* in = &std::cin;
    std::ifstream file;
    if (std::strcmp(inPath, "-") != 0) {
        file.open(inPath);
        if (!file) { std::fprintf(stderr, "cannot open %s\n", inPath); return 1; }
        in = &file;
    } else {
#ifdef _WIN32
        _setmode(_fileno(stdin), _O_BINARY);
#endif
        std::ios::sync_with_stdio(false);
    }
    static Position pos;
    std::string line;
    LichessEntry e;
    unsigned long long lines = 0, kept = 0, inChunk = 0;   // lines: read from the start of the input, skipped ones included
    FILE* out = nullptr;
    std::string tmpName, finalName;
    auto open_chunk = [&]() {
        char name[64];
        std::snprintf(name, sizeof(name), "/lichess-%04d.bin", chunk);
        finalName = outDir + name;
        tmpName = finalName + ".tmp";
        out = std::fopen(tmpName.c_str(), "wb");
        if (!out) { std::fprintf(stderr, "cannot write %s\n", tmpName.c_str()); std::exit(1); }
    };
    auto close_chunk = [&]() {
        std::fclose(out);
        out = nullptr;
        std::remove(finalName.c_str());
        if (std::rename(tmpName.c_str(), finalName.c_str()) != 0) { std::fprintf(stderr, "cannot rename %s\n", tmpName.c_str()); std::exit(1); }
        std::printf("chunk %d records=%llu lines=%llu\n", chunk, inChunk, lines);
        std::fflush(stdout);
        chunk++;
        inChunk = 0;
    };
    while (kept < maxRecords && std::getline(*in, line)) {
        lines++;
        if (lines <= skipLines) continue;   // already imported by an earlier run
        if (lines % 2000000 == 0) { std::printf("progress lines=%llu kept=%llu\n", lines, kept); std::fflush(stdout); }
        if (!parse_lichess_line(line, e) || !pos.set(e.fen.c_str()) || !lichess_quiet(pos, e.move)) continue;
        const int stmCp = stmView ? e.cp : (pos.stm == WHITE ? e.cp : -e.cp);   // side to move's view
        const int score = (int)std::lround((pos.stm == WHITE ? stmCp : -stmCp) * scale);   // Tessera scale, White's view
        if (std::abs(score) >= 1500) continue;       // same cut as Tessera's own data
        if (!out) open_chunk();
        unsigned char rec[68];
        make_record(pos, score, 255, rec);
        std::fwrite(rec, 1, sizeof(rec), out);
        kept++;
        if (++inChunk == CHUNK) close_chunk();
    }
    if (out) close_chunk();
    std::printf("done lines=%llu kept=%llu\n", lines, kept);
    return 0;
}

static int lichess_command(int argc, char** argv) {
    if (std::strcmp(argv[1], "lichess-calibrate") == 0 && argc == 5)
        return lichess_calibrate(argv[2], std::atoi(argv[3]), (u64)std::atoll(argv[4]));
    if (std::strcmp(argv[1], "lichess-import") == 0 && (argc == 9 || argc == 10))
        return lichess_import(argv[2], argv[3], std::atof(argv[4]), std::strcmp(argv[5], "stm") == 0,
                              std::strtoull(argv[6], nullptr, 10), std::strtoull(argv[7], nullptr, 10), std::atoi(argv[8]),
                              argc == 10 ? std::strtoull(argv[9], nullptr, 10) : 10000000ULL);
    std::fprintf(stderr, "usage: tessera lichess-calibrate <file.jsonl> <positions> <nodes>\n"
                         "       tessera lichess-import <file.jsonl|-> <out folder> <scale> <white|stm> <max records> <skip lines> <first chunk> [chunk size]\n");
    return 1;
}
