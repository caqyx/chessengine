// Tessera engine worker. Classic Web Worker speaking UCI: one string per
// message in, one line per message out. See docs/engine-protocol.md.
//
// The search runs synchronously inside the wasm module, so a message sent
// while the engine is thinking is only read after `bestmove`. To abort a
// search the page terminates this worker and starts a new one.
'use strict';

let wasm = null;        // exports of the instantiated module
let failed = false;
let pending = [];       // commands received before the module was ready
const decoder = new TextDecoder();

const imports = {
  env: {
    js_out(ptr, len) {
      // memory.buffer is replaced when the hash table grows, so never cache the view
      self.postMessage(decoder.decode(new Uint8Array(wasm.memory.buffer, ptr, len)));
    },
    js_now() {
      return performance.now();
    },
  },
};

function run(line) {
  const ptr = wasm.cmd_buffer();
  const max = wasm.cmd_buffer_size() - 1;
  const mem = new Uint8Array(wasm.memory.buffer);
  const n = Math.min(line.length, max);
  for (let i = 0; i < n; i++) mem[ptr + i] = line.charCodeAt(i) & 0x7f;
  mem[ptr + n] = 0;
  wasm.run_cmd();
}

self.onmessage = (event) => {
  if (failed) return;
  const line = String(event.data);
  if (wasm) run(line);
  else pending.push(line);
};

fetch(new URL('engine.wasm', self.location.href))
  .then((response) => {
    if (!response.ok) throw new Error('engine.wasm HTTP ' + response.status);
    return response.arrayBuffer();
  })
  .then((bytes) => WebAssembly.instantiate(bytes, imports))
  .then((result) => {
    wasm = result.instance.exports;
    wasm.init();
    const queued = pending;
    pending = [];
    for (const line of queued) run(line);
  })
  .catch((err) => {
    failed = true;
    self.postMessage('error ' + (err && err.message ? err.message : String(err)));
  });
