// Thin platform layer. Everything the engine needs from the outside world.
#pragma once
#include "types.h"

u64 plat_now_ms();                       // monotonic milliseconds
void plat_out(const char* line);         // emit one output line (no trailing newline needed)
// Resizable zero-filled block for the hash table. Pass the previous block (or null).
void* plat_big_realloc(void* old, size_t oldBytes, size_t newBytes);
// Whole file as bytes (null if it cannot be read). The caller does not free the result.
const unsigned char* plat_read_file(const char* path, size_t* size);
