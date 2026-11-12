// Neural-network evaluation ("NNUE": a small network that is cheap to update move by move).
//
// Shape: 768 inputs -> H hidden (computed once per side) -> 1 output.
//   * Inputs: one per (piece colour relative to the viewer, piece type, square), 2*6*64 = 768.
//     Exactly one input is "on" for every piece on the board, so a position turns on at most 32.
//   * Hidden layer ("accumulator"): bias + the weight rows of the inputs that are on. A move
//     switches 2 to 4 inputs, so the accumulator is updated by adding and subtracting a few
//     rows instead of being recomputed. It is kept twice: from White's and from Black's side.
//   * Output: both accumulators (side to move first) through a squared, clipped activation,
//     then one weighted sum.
// Weights are integers (fixed point), trained by tools/trainer.cpp.
#pragma once
#include "types.h"

enum { NNUE_MAX_H = 256, NNUE_INPUTS = 768 };
typedef i16 Accumulator[2][NNUE_MAX_H];

struct Position;

extern bool NnueActive;      // a network is loaded and should be used
extern int NnueHidden;

// Parses a network file image. Returns false (and leaves the old state) if it is malformed.
bool nnue_load(const unsigned char* data, size_t size);
void nnue_set_active(bool on);
bool nnue_loaded();

void nnue_refresh(const Position& pos, Accumulator acc);
void nnue_copy(Accumulator dst, const Accumulator src);
void nnue_add(Accumulator acc, int piece, int sq);
void nnue_sub(Accumulator acc, int piece, int sq);
// dst = src - rows of the removed pieces + rows of the added pieces, in one pass, for both
// perspectives. This is the per-move update; up to 3 removals and 2 additions (castling with
// a capture cannot happen, a capture-promotion is 2 + 1).
void nnue_apply(Accumulator dst, const Accumulator src,
                const int* subPiece, const int* subSq, int ns,
                const int* addPiece, const int* addSq, int na);
// Centipawns from the side to move's point of view.
int nnue_evaluate(const Position& pos);

// Input index of a piece on a square, seen from `perspective` (WHITE or BLACK).
inline int nnue_input(int perspective, int piece, int sq) {
    return (piece_color(piece) != perspective) * 384 + piece_type(piece) * 64 + (perspective == WHITE ? sq : sq ^ 56);
}
