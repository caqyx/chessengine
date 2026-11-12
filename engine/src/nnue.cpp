#include "nnue.h"
#include "position.h"

// Vector paths: AVX2 on PCs (native builds and the Windows .exe), SIMD128 in the browser.
// Both compute exactly the same integers as the plain loops below, so the engine plays
// identical moves whichever path runs; only the speed differs.
#if defined(__AVX2__)
#include <immintrin.h>
#define NNUE_AVX2 1
#elif defined(__wasm_simd128__)
#include <wasm_simd128.h>
#define NNUE_WASM 1
#endif

bool NnueActive = false;
int NnueHidden = 0;

static bool Loaded = false;
static int QA = 255, QB = 64, Scale = 181;
alignas(64) static i16 FtWeights[NNUE_INPUTS][NNUE_MAX_H];
alignas(64) static i16 FtBias[NNUE_MAX_H];
alignas(64) static i16 OutWeights[2 * NNUE_MAX_H];
// The vector output needs (activation x weight) to fit 16 bits and the total to fit 32 bits.
// Checked once per network; a network outside those limits uses the exact 64-bit loop.
static bool FastOutput = false;
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
    i64 worstTotal = 0;
    int worstWeight = 0;
    for (int i = 0; i < 2 * h; i++) {
        const int w = OutWeights[i] < 0 ? -OutWeights[i] : OutWeights[i];
        worstWeight = imax(worstWeight, w);
        worstTotal += (i64)QA * QA * w;
    }
    FastOutput = h % 16 == 0 && (i64)QA * worstWeight < 32768 && worstTotal < 2147483647LL;
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

// One perspective: out = in - sub[0..ns) + add[0..na). 16-bit wrap-around arithmetic, so
// the order of the additions does not change the result.
static inline void apply_rows(i16* out, const i16* in, const i16* const* sub, int ns, const i16* const* add, int na, int h) {
#if defined(NNUE_AVX2)
    if (h % 16 == 0) {
        for (int i = 0; i < h; i += 16) {
            __m256i v = _mm256_loadu_si256((const __m256i*)(in + i));
            for (int k = 0; k < ns; k++) v = _mm256_sub_epi16(v, _mm256_loadu_si256((const __m256i*)(sub[k] + i)));
            for (int k = 0; k < na; k++) v = _mm256_add_epi16(v, _mm256_loadu_si256((const __m256i*)(add[k] + i)));
            _mm256_storeu_si256((__m256i*)(out + i), v);
        }
        return;
    }
#elif defined(NNUE_WASM)
    if (h % 8 == 0) {
        for (int i = 0; i < h; i += 8) {
            v128_t v = wasm_v128_load(in + i);
            for (int k = 0; k < ns; k++) v = wasm_i16x8_sub(v, wasm_v128_load(sub[k] + i));
            for (int k = 0; k < na; k++) v = wasm_i16x8_add(v, wasm_v128_load(add[k] + i));
            wasm_v128_store(out + i, v);
        }
        return;
    }
#endif
    for (int i = 0; i < h; i++) {
        int v = in[i];
        for (int k = 0; k < ns; k++) v -= sub[k][i];
        for (int k = 0; k < na; k++) v += add[k][i];
        out[i] = (i16)v;
    }
}

void nnue_apply(Accumulator dst, const Accumulator src,
                const int* subPiece, const int* subSq, int ns,
                const int* addPiece, const int* addSq, int na) {
    for (int p = 0; p < 2; p++) {
        const i16* sub[3];
        const i16* add[2];
        for (int k = 0; k < ns; k++) sub[k] = FtWeights[nnue_input(p, subPiece[k], subSq[k])];
        for (int k = 0; k < na; k++) add[k] = FtWeights[nnue_input(p, addPiece[k], addSq[k])];
        apply_rows(dst[p], src[p], sub, ns, add, na, NnueHidden);
    }
}

// Sum over one side's hidden units of clamp(x, 0, QA)^2 * weight.
static inline i64 output_sum(const i16* acc, const i16* w, int h) {
#if defined(NNUE_AVX2)
    if (FastOutput) {
        const __m256i zero = _mm256_setzero_si256(), top = _mm256_set1_epi16((short)QA);
        __m256i total = _mm256_setzero_si256();
        for (int i = 0; i < h; i += 16) {
            __m256i c = _mm256_min_epi16(_mm256_max_epi16(_mm256_loadu_si256((const __m256i*)(acc + i)), zero), top);
            __m256i cw = _mm256_mullo_epi16(c, _mm256_loadu_si256((const __m256i*)(w + i)));   // fits 16 bits (checked at load)
            total = _mm256_add_epi32(total, _mm256_madd_epi16(cw, c));                         // c * (c * w), pairs summed into 32 bits
        }
        __m128i s = _mm_add_epi32(_mm256_castsi256_si128(total), _mm256_extracti128_si256(total, 1));
        s = _mm_add_epi32(s, _mm_shuffle_epi32(s, 0x4E));
        s = _mm_add_epi32(s, _mm_shuffle_epi32(s, 0xB1));
        return _mm_cvtsi128_si32(s);
    }
#elif defined(NNUE_WASM)
    if (FastOutput) {
        const v128_t zero = wasm_i16x8_splat(0), top = wasm_i16x8_splat((short)QA);
        v128_t total = wasm_i32x4_splat(0);
        for (int i = 0; i < h; i += 8) {
            v128_t c = wasm_i16x8_min(wasm_i16x8_max(wasm_v128_load(acc + i), zero), top);
            v128_t cw = wasm_i16x8_mul(c, wasm_v128_load(w + i));
            total = wasm_i32x4_add(total, wasm_i32x4_dot_i16x8(cw, c));
        }
        return (i64)wasm_i32x4_extract_lane(total, 0) + wasm_i32x4_extract_lane(total, 1)
             + wasm_i32x4_extract_lane(total, 2) + wasm_i32x4_extract_lane(total, 3);
    }
#endif
    i64 sum = 0;
    for (int i = 0; i < h; i++) {
        i32 c = acc[i] < 0 ? 0 : acc[i] > QA ? QA : acc[i];
        sum += (i64)(c * w[i]) * c;
    }
    return sum;
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

    const i16 (*acc)[NNUE_MAX_H] = pos.accumulator();
    const i16* us = acc[pos.stm];
    const i16* them = acc[!pos.stm];
    const int h = NnueHidden;
    // squared clipped activation times weight, side to move first
    const i64 sum = output_sum(us, OutWeights, h) + output_sum(them, OutWeights + h, h);
    i64 v = (sum / QA + OutBias) * Scale / ((i64)QA * QB);
    v = v * (200 - imin(st->halfmove, 100)) / 200;     // fade towards a draw as the fifty-move rule nears
    return (int)iclamp<i64>(v, -VALUE_MATE_IN_MAX + 1, VALUE_MATE_IN_MAX - 1);
}
