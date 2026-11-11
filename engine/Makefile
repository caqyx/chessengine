# Tessera chess engine
#   make            native UCI binary  -> engine/tessera
#   make wasm       WebAssembly module -> engine/web/engine.wasm (used by the browser game)
#   make test       perft + self-test

CXX      ?= g++
ARCH     ?= native
CORE     := src/bitboard.cpp src/position.cpp src/movegen.cpp src/eval.cpp src/tt.cpp src/search.cpp src/uci.cpp
CXXFLAGS := -std=c++17 -O3 -DNDEBUG -Wall -Wextra -fno-exceptions -fno-rtti

native: tessera

tessera: $(CORE) src/main_native.cpp src/*.h
	$(CXX) $(CXXFLAGS) -march=$(ARCH) -flto -pthread $(CORE) src/main_native.cpp -o $@

# Bare wasm32: no libc, no emscripten. Needs clang with the wasm32 target and wasm-ld.
WASM_CXX ?= clang++
wasm: web/engine.wasm

web/engine.wasm: $(CORE) src/main_wasm.cpp src/*.h
	$(WASM_CXX) --target=wasm32 $(CXXFLAGS) -ffreestanding -fno-builtin -nostdlib -mbulk-memory \
	    -DDEFAULT_HASH_MB=32 -DOPENING_VARIETY_DEFAULT=true -flto \
	    -Wl,--no-entry -Wl,--stack-first -Wl,-z,stack-size=4194304 -Wl,--lto-O3 \
	    $(CORE) src/main_wasm.cpp -o $@

# Same engine with the search thresholds exposed as UCI options, for match testing.
tessera-tune: $(CORE) src/main_native.cpp src/*.h
	$(CXX) $(CXXFLAGS) -DTUNE_SEARCH -march=$(ARCH) -flto -pthread $(CORE) src/main_native.cpp -o $@

test: tessera
	./tessera perft 5
	./tessera selftest

clean:
	rm -f tessera tessera-tune web/engine.wasm

.PHONY: native wasm test clean
