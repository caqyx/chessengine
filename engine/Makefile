# Tessera chess engine
#   make            native UCI binary  -> engine/tessera
#   make wasm       WebAssembly modules -> engine/web/engine.wasm + engine-nosimd.wasm (used by the browser game)
#   make trainer    network trainer    -> engine/trainer (see docs/training.md)
#   make test       perft + self-test

CXX      ?= g++
ARCH     ?= native
CORE     := src/bitboard.cpp src/position.cpp src/movegen.cpp src/eval.cpp src/nnue.cpp src/tt.cpp src/search.cpp src/uci.cpp
CXXFLAGS := -std=c++17 -O3 -DNDEBUG -Wall -Wextra -fno-exceptions -fno-rtti

native: tessera

tessera: $(CORE) src/main_native.cpp src/*.h
	$(CXX) $(CXXFLAGS) -march=$(ARCH) -flto -pthread $(CORE) src/main_native.cpp -o $@

# Bare wasm32: no libc, no emscripten. Needs clang with the wasm32 target and wasm-ld.
WASM_CXX ?= clang++
wasm: web/engine.wasm

WASM_FLAGS := --target=wasm32 $(CXXFLAGS) -ffreestanding -fno-builtin -nostdlib -mbulk-memory \
	    -DDEFAULT_HASH_MB=32 -DOPENING_VARIETY_DEFAULT=true -DSHOW_WDL_DEFAULT=true -flto \
	    -Wl,--no-entry -Wl,--stack-first -Wl,-z,stack-size=4194304 -Wl,--lto-O3

# Two builds: with SIMD (about a quarter faster with the network) and without, for
# browsers that lack it. engine-worker.js picks the one the browser supports.
web/engine.wasm: $(CORE) src/main_wasm.cpp src/*.h
	$(WASM_CXX) $(WASM_FLAGS) -msimd128 $(CORE) src/main_wasm.cpp -o web/engine.wasm
	$(WASM_CXX) $(WASM_FLAGS) $(CORE) src/main_wasm.cpp -o web/engine-nosimd.wasm

# Network trainer (CPU, no dependencies). Used by tools/train_net.py.
trainer: ../tools/trainer.cpp
	$(CXX) -std=c++17 -O3 -march=$(ARCH) -pthread ../tools/trainer.cpp -o $@

# Same engine with the search thresholds exposed as UCI options, for match testing.
tessera-tune: $(CORE) src/main_native.cpp src/*.h
	$(CXX) $(CXXFLAGS) -DTUNE_SEARCH -march=$(ARCH) -flto -pthread $(CORE) src/main_native.cpp -o $@

test: tessera
	./tessera perft 5
	./tessera selftest

clean:
	rm -f tessera tessera-tune trainer web/engine.wasm web/engine-nosimd.wasm

.PHONY: native wasm test clean
