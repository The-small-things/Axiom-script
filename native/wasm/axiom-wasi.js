#!/usr/bin/env node
// axiom-wasi.js — run axiom.wasm (the native command built for WebAssembly) under Node's WASI:
//
//   node native/wasm/axiom-wasi.js prog.ax [flags…]      same flags as `axiom`
//
// The host's root is preopened as "/" and the program starts in the host's working directory,
// so paths (the program, ^use, read/write) mean what they do natively.
'use strict';
const fs = require('fs');
const path = require('path');
// node:wasi announces itself as experimental on every run; the program's stderr is not the place.
process.removeAllListeners('warning');
process.on('warning', (w) => { if (w.name !== 'ExperimentalWarning') console.error(w.stack || String(w)); });
const { WASI } = require('node:wasi');

const wasmPath = process.env.AXIOM_WASM || path.join(__dirname, 'axiom.wasm');
const wasi = new WASI({
  version: 'preview1',
  args: ['axiom', ...process.argv.slice(2)],
  env: { ...process.env, AXIOM_WASI_CWD: process.cwd() },
  preopens: { '/': '/' },
  returnOnExit: true,
});
const wasm = new WebAssembly.Module(fs.readFileSync(wasmPath));
const instance = new WebAssembly.Instance(wasm, wasi.getImportObject());
process.exitCode = wasi.start(instance);
