#include "nnue.h"
#include "position.h"

bool NnueActive = false;
int NnueHidden = 0;

static bool Loaded = false;
static int QA = 255, QB = 64, Scale = 181;
static i16 FtWeights[NNUE_INPUTS][NNUE_MAX_H];
static i16 FtBias[NNUE_MAX_H];
static i16 OutWeights[2 * NNUE_MAX_H];
static i32 OutBias;

static u32 read_u32(const unsigned char* p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }
static i16 read_i16(const unsigned char* p) { return (i16)(u16)(p[0] | (p[1] << 8)); }

// File layout (little endian): "TSNN", version, hidden, scale, QA, QB, then
// i16 input weights [768][hidden], i16 hidden bias [hidden], i16 output weights [2*hidden], i32 output bias.
bool nnue_load(const unsigned char* d, size_t size) {
    if (size < 24 || d[0] != 'T' || d[1] != 'S' || d[2] != 'N' || d[3] != 'N' || read_u32(d + 4) != 1) return false;
    const int h = (int)read_u32(d + 8);
    if (h < 1 || h > NNUE_MAX_H) return false;
    const size_t need = 24 + (size_t)2 * (NNUE_INPUTS * h + h + 2 * h) + 4;
    if (size < need) return false;
    Scale = (int)read_u32(d + 12); QA = (int)read_u32(d + 16); QB = (int)read_u32(d + 20);
    if (QA < 1 || QB < 1 || Scale < 1) return false;
    const unsigned char* p = d + 24;
    for (int f = 0; f < NNUE_INPUTS; f++)
        for (int i = 0; i < h; i++, p += 2) FtWeights[f][i] = read_i16(p);
    for (int i = 0; i < h; i++, p += 2) FtBias[i] = read_i16(p);
    for (int i = 0; i < 2 * h; i++, p += 2) OutWeights[i] = read_i16(p);
    OutBias = (i32)read_u32(p);
    NnueHidden = h;
    Loaded = true;
    NnueActive = true;
    return true;
}

bool nnue_loaded() { return Loaded; }
void nnue_set_active(bool on) { NnueActive = on && Loaded; }

void nnue_copy(Accumulator dst, const Accumulator src) {
    const int h = NnueHidden;
    for (int p = 0; p < 2; p++)
        for (int i = 0; i < h; i++) dst[p][i] = src[p][i];
}

void nnue_add(Accumulator acc, int piece, int sq) {
    const int h = NnueHidden;
    for (int p = 0; p < 2; p++) {
        const i16* w = FtWeights[nnue_input(p, piece, sq)];
        i16* a = acc[p];
        for (int i = 0; i < h; i++) a[i] = (i16)(a[i] + w[i]);
    }
}

void nnue_sub(Accumulator acc, int piece, int sq) {
    const int h = NnueHidden;
    for (int p = 0; p < 2; p++) {
        const i16* w = FtWeights[nnue_input(p, piece, sq)];
        i16* a = acc[p];
        for (int i = 0; i < h; i++) a[i] = (i16)(a[i] - w[i]);
    }
}

void nnue_refresh(const Position& pos, Accumulator acc) {
    const int h = NnueHidden;
    for (int p = 0; p < 2; p++)
        for (int i = 0; i < h; i++) acc[p][i] = FtBias[i];
    for (Bitboard b = pos.all(); b;) {
        int sq = pop_lsb(b);
        nnue_add(acc, pos.board[sq], sq);
    }
}

int nnue_evaluate(const Position& pos) {
    const State* st = pos.st;
    // Material that cannot mate is a draw whatever the network thinks.
    if (!pos.pieces(PAWN) && st->npm[WHITE] + st->npm[BLACK] <= 365) return 0;

    const i16 (*acc)[NNUE_MAX_H] = pos.accStack[st - pos.states];
    const i16* us = acc[pos.stm];
    const i16* them = acc[!pos.stm];
    const int h = NnueHidden;
    i64 sum = 0;
    for (int i = 0; i < h; i++) {
        i32 c = us[i] < 0 ? 0 : us[i] > QA ? QA : us[i];
        sum += (i64)(c * OutWeights[i]) * c;          // squared clipped activation times weight
    }
    for (int i = 0; i < h; i++) {
        i32 c = them[i] < 0 ? 0 : them[i] > QA ? QA : them[i];
        sum += (i64)(c * OutWeights[h + i]) * c;
    }
    i64 v = (sum / QA + OutBias) * Scale / ((i64)QA * QB);
    v = v * (200 - imin(st->halfmove, 100)) / 200;     // fade towards a draw as the fifty-move rule nears
    return (int)iclamp<i64>(v, -VALUE_MATE_IN_MAX + 1, VALUE_MATE_IN_MAX - 1);
}
