#pragma once
#include "position.h"

void eval_init();
void eval_clear();
// Static evaluation in centipawns from the side to move's point of view.
int evaluate(const Position& pos);
extern int EvalMask;
