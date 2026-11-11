// WebAssembly entry point. No libc, no WASI: the module imports two functions
// from JavaScript (output and clock) and exports init / cmd_buffer / run_cmd.
#include "platform.h"
#include "search.h"
#include "uci.h"

extern "C" {
__attribute__((import_module("env"), import_name("js_out"))) void js_out(const char* ptr, int len);
__attribute__((import_module("env"), import_name("js_now"))) double js_now();
extern unsigned char __heap_base;

// The compiler may still emit calls to these for large copies.
__attribute__((used)) void* memcpy(void* dst, const void* src, size_t n) {
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
    return dst;
}
__attribute__((used)) void* memmove(void* dst, const void* src, size_t n) {
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    if (d < s) for (size_t i = 0; i < n; i++) d[i] = s[i];
    else for (size_t i = n; i > 0; i--) d[i - 1] = s[i - 1];
    return dst;
}
__attribute__((used)) void* memset(void* dst, int c, size_t n) {
    unsigned char* d = (unsigned char*)dst;
    for (size_t i = 0; i < n; i++) d[i] = (unsigned char)c;
    return dst;
}
}

u64 plat_now_ms() { return (u64)js_now(); }

void plat_out(const char* line) {
    int n = 0;
    while (line[n]) n++;
    js_out(line, n);
}

// The hash table is the only dynamic allocation, so it simply lives at the
// end of linear memory and grows in place.
void* plat_big_realloc(void*, size_t, size_t newBytes) {
    const size_t PAGE = 65536;
    size_t base = ((size_t)&__heap_base + PAGE - 1) & ~(PAGE - 1);
    size_t have = __builtin_wasm_memory_size(0) * PAGE;
    size_t need = base + newBytes;
    if (need < base) return nullptr;
    if (need > have) {
        size_t pages = (need - have + PAGE - 1) / PAGE;
        if (__builtin_wasm_memory_grow(0, pages) == (size_t)-1) return nullptr;
    }
    return (void*)base;
}

static char CmdBuffer[65536];

extern "C" {
__attribute__((export_name("init"))) void wasm_init() { engine_init(); }
__attribute__((export_name("cmd_buffer"))) char* wasm_cmd_buffer() { return CmdBuffer; }
__attribute__((export_name("cmd_buffer_size"))) int wasm_cmd_buffer_size() { return (int)sizeof(CmdBuffer); }
__attribute__((export_name("run_cmd"))) void wasm_run_cmd() {
    CmdBuffer[sizeof(CmdBuffer) - 1] = 0;
    SearchStop = false;
    uci_command(CmdBuffer);
}
}
