#pragma once
#include "position.h"

enum GenType { GEN_NOISY, GEN_QUIET, GEN_ALL };

// Generates pseudo-legal moves (check evasions are already restricted to
// blocking / capturing squares). Returns the number of moves written.
int generate(const Position& pos, Move* list, GenType type);
