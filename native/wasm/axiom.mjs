// axiom.mjs — AxiomScript in JavaScript, through the native runtime built to WebAssembly
// (axiom-lib.wasm, the embedding API of axiom_api.h). Works in Node and in browsers; no WASI
// support is needed from the host: the few system calls the runtime makes are answered here,
// and there is no file system (a program's read/write fail as a sandbox denial would).
//
//   import { loadAxiom } from './axiom.mjs';
//   const axiom = await loadAxiom();                  // or loadAxiom(bytes | URL | Response)
//   const ax = axiom.create({ onOutput: (stream, text) => ... });
//   ax.run('^main:\n  print(1 + 2)\n');              // → 0, and "3\n" on stream 1
//   ax.define('now_ms', () => Date.now());            // callable from AxiomScript
//   ax.eval('x = 20'); ax.eval('x * 2').value         // → "40"
//   ax.load(worldSource); ax.setInput({ x: 1 }); ax.step(60); ax.state()   // → the --sim JSON
//   ax.render(320, 240)                               // → RGBA pixels for an ImageData
//   ax.free();

const ENOSYS = 52, EBADF = 8, ENOTCAPABLE = 76;

class ProcExit extends Error {
  constructor(code) { super(`exit(${code})`); this.code = code; }
}

// The runtime's system calls: clocks, stdout/stderr, and nothing else.
function wasiImports(getMemory, onRaw) {
  const view = () => new DataView(getMemory().buffer);
  const t0 = typeof performance !== 'undefined' ? performance.now() : 0;
  const calls = {
    args_sizes_get(argc, bufSize) { view().setUint32(argc, 0, true); view().setUint32(bufSize, 0, true); return 0; },
    args_get() { return 0; },
    environ_sizes_get(count, bufSize) { view().setUint32(count, 0, true); view().setUint32(bufSize, 0, true); return 0; },
    environ_get() { return 0; },
    clock_time_get(id, precision, out) {
      const ms = id === 0 ? Date.now() : (typeof performance !== 'undefined' ? performance.now() - t0 : Date.now());
      view().setBigUint64(out, BigInt(Math.round(ms * 1e6)), true);
      return 0;
    },
    clock_res_get(id, out) { view().setBigUint64(out, 1000n, true); return 0; },
    fd_write(fd, iovs, iovsLen, nwritten) {
      const dv = view();
      const mem = new Uint8Array(getMemory().buffer);
      let total = 0;
      const parts = [];
      for (let i = 0; i < iovsLen; i++) {
        const ptr = dv.getUint32(iovs + i * 8, true), len = dv.getUint32(iovs + i * 8 + 4, true);
        parts.push(mem.slice(ptr, ptr + len));
        total += len;
      }
      if (fd === 1 || fd === 2) for (const p of parts) onRaw(fd, p);
      else return EBADF;
      dv.setUint32(nwritten, total, true);
      return 0;
    },
    fd_read(fd, iovs, iovsLen, nread) { view().setUint32(nread, 0, true); return fd === 0 ? 0 : EBADF; },
    fd_fdstat_get(fd, out) {
      if (fd > 2) return EBADF;
      const dv = view();
      dv.setUint8(out, 2);            // character device
      dv.setUint16(out + 2, 0, true);
      dv.setBigUint64(out + 8, 0n, true);
      dv.setBigUint64(out + 16, 0n, true);
      return 0;
    },
    fd_fdstat_set_flags() { return 0; },
    fd_prestat_get() { return EBADF; },   // no preopened directories: no file system
    fd_prestat_dir_name() { return EBADF; },
    fd_close() { return 0; },
    fd_seek() { return ENOSYS; },
    path_open() { return ENOTCAPABLE; },
    path_filestat_get() { return ENOTCAPABLE; },
    poll_oneoff(inp, out, n, nevents) { view().setUint32(nevents, 0, true); return 0; },   // sleep(): no wait
    random_get(buf, len) {
      const bytes = new Uint8Array(getMemory().buffer, buf, len);
      if (typeof crypto !== 'undefined' && crypto.getRandomValues) crypto.getRandomValues(bytes);
      else for (let i = 0; i < len; i++) bytes[i] = Math.floor(Math.random() * 256);
      return 0;
    },
    sched_yield() { return 0; },
    proc_exit(code) { throw new ProcExit(code); },
  };
  // Anything else the runtime might ask for does not exist here.
  return new Proxy(calls, { get: (t, name) => (name in t ? t[name] : () => ENOSYS) });
}

async function wasmBytes(source) {
  if (source instanceof ArrayBuffer || ArrayBuffer.isView(source)) return source;
  if (typeof Response !== 'undefined' && source instanceof Response) return source.arrayBuffer();
  const url = source ?? new URL('./axiom-lib.wasm', import.meta.url);
  if (typeof process !== 'undefined' && process.versions?.node && (!(url instanceof URL) || url.protocol === 'file:')) {
    const { readFile } = await import('node:fs/promises');
    return readFile(url);
  }
  return (await fetch(url)).arrayBuffer();
}

export async function loadAxiom(source) {
  const bytes = await wasmBytes(source);
  let exports = null;
  const instances = new Map();   // C pointer → Axiom, for output and host calls
  const utf8 = new TextDecoder(), enc = new TextEncoder();
  const memory = () => exports.memory;

  const cstr = (ptr) => {
    if (!ptr) return null;
    const mem = new Uint8Array(memory().buffer);
    let end = ptr;
    while (mem[end]) end++;
    return utf8.decode(mem.subarray(ptr, end));
  };
  const take = (ptr) => { const s = cstr(ptr); if (ptr) exports.axiom_release(ptr); return s; };
  const alloc = (str) => {
    if (str == null) return 0;
    const b = enc.encode(str);
    const ptr = exports.axiom_js_alloc(b.length + 1);
    const mem = new Uint8Array(memory().buffer);
    mem.set(b, ptr);
    mem[ptr + b.length] = 0;
    return ptr;
  };
  const withStrings = (strs, fn) => {
    const ptrs = strs.map(alloc);
    try { return fn(...ptrs); } finally { for (const p of ptrs) if (p) exports.axiom_release(p); }
  };

  // Raw stdio from the runtime (outside any instance's writer): the console.
  const rawDecoder = { 1: new TextDecoder(), 2: new TextDecoder() };
  const onRaw = (fd, bytes) => {
    const text = rawDecoder[fd].decode(bytes, { stream: true });
    if (text) (fd === 2 ? console.error : console.log)(text.replace(/\n$/, ''));
  };

  const imports = {
    wasi_snapshot_preview1: wasiImports(memory, onRaw),
    axiom: {
      write(ax, stream, data, len) {
        const inst = instances.get(ax);
        const text = utf8.decode(new Uint8Array(memory().buffer, data, len));
        if (inst) inst._output(stream, text);
      },
      host(ax, id, argsPtr) {
        const inst = instances.get(ax);
        const args = JSON.parse(cstr(argsPtr));
        let result;
        try {
          result = inst._hosts[id](...args);
        } catch (e) {
          // A JavaScript exception must not unwind through the runtime's frames: it becomes an
          // AxiomScript error, catchable with ^try.
          withStrings([e?.code && /^[A-Z][A-Z0-9_-]*$/.test(e.code) ? e.code : 'AX-HOST-002', String(e?.message ?? e)],
            (c, m) => exports.axiom_host_error(ax, c, m));
          return 0;
        }
        return result === undefined ? 0 : alloc(JSON.stringify(result) ?? 'null');
      },
    },
  };
  const { instance } = await WebAssembly.instantiate(bytes, imports);
  exports = instance.exports;
  exports._initialize?.();

  class Axiom {
    constructor(opts = {}) {
      this._ptr = exports.axiom_js_new();
      this._hosts = [];
      this._names = new Map();
      this._out = opts.onOutput ?? defaultOutput;
      instances.set(this._ptr, this);
      if (opts.sandbox === false) exports.axiom_sandbox(this._ptr, 0);
      if (opts.args) this.setArgs(opts.args);
    }
    _output(stream, text) { this._out(stream, text); }
    _live() { if (!this._ptr) throw new Error('this Axiom instance was freed'); return this._ptr; }

    setArgs(args) {
      const ptrs = args.map((a) => alloc(String(a)));
      const arr = exports.axiom_js_alloc(4 * Math.max(ptrs.length, 1));
      const dv = new DataView(memory().buffer);
      ptrs.forEach((p, i) => dv.setUint32(arr + 4 * i, p, true));
      exports.axiom_set_args(this._live(), ptrs.length, arr);
      for (const p of ptrs) exports.axiom_release(p);
      exports.axiom_release(arr);
    }
    // A JavaScript function callable from AxiomScript: arguments and result cross as JSON
    // (so functions and entities arrive as null). Throwing raises a catchable error.
    define(name, fn) {
      let id = this._names.get(name);
      if (id === undefined) { id = this._hosts.length; this._names.set(name, id); this._hosts.push(fn); }
      else this._hosts[id] = fn;
      if (withStrings([name], (n) => exports.axiom_js_define(this._live(), n, id))) throw new Error(`not an identifier: ${name}`);
      return this;
    }
    run(source, filename = null) { return withStrings([source, filename], (s, f) => exports.axiom_run(this._live(), s, f)); }
    load(source, filename = null) { return withStrings([source, filename], (s, f) => exports.axiom_load(this._live(), s, f)) === 0; }
    main() { return exports.axiom_main(this._live()); }
    result() { return JSON.parse(take(exports.axiom_result(this._live()))); }
    eval(code) {
      const errs = exports.axiom_js_alloc(4);
      const value = withStrings([code], (c) => take(exports.axiom_js_eval_value(this._live(), c, errs)));
      const errors = new DataView(memory().buffer).getInt32(errs, true);
      exports.axiom_release(errs);
      return { value, errors };
    }
    setInput({ x = 0, y = 0, jump = false, fire = false } = {}) {
      exports.axiom_set_input(this._live(), x, y, jump ? 1 : 0, fire ? 1 : 0);
    }
    step(frames = 1) { return exports.axiom_step(this._live(), frames) !== 0; }   // true: exit() was called
    state() { return JSON.parse(take(exports.axiom_state(this._live()))); }
    render(width, height) {
      const ptr = exports.axiom_render(this._live(), width, height);
      if (!ptr) return null;
      const px = new Uint8ClampedArray(memory().buffer.slice(ptr, ptr + width * height * 4));
      exports.axiom_release(ptr);
      return px;
    }
    get exitCode() { return exports.axiom_exit_code(this._live()); }
    get error() { return cstr(exports.axiom_error(this._live())); }
    free() {
      if (!this._ptr) return;
      instances.delete(this._ptr);
      exports.axiom_free(this._ptr);
      this._ptr = 0;
    }
  }

  return {
    version: cstr(exports.axiom_version()),
    create: (opts) => new Axiom(opts),
    check: (source, filename = null) => JSON.parse(withStrings([source, filename], (s, f) => take(exports.axiom_check(s, f)))),
  };
}

function defaultOutput(stream, text) {
  if (typeof process !== 'undefined' && process.stdout?.write) (stream === 2 ? process.stderr : process.stdout).write(text);
  else (stream === 2 ? console.error : console.log)(text.replace(/\n$/, ''));
}
