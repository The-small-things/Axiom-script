// AxiomScript Standard Library (v0.9.0)
//
// The language's design metric is token count: a program an LLM writes should cost as few
// tokens as possible while still being unambiguous. That metric decides what belongs here.
// A helper earns its place when writing it by hand would cost more tokens than calling it —
// which is true of almost everything an ordinary program does: statistics, sorting by a key,
// grouping, regular expressions, file and process I/O, time, encoding, seeded randomness.
//
// Naming rules, so the name can be guessed instead of looked up:
//   * one word where one word is unambiguous (`sum`, `uniq`, `keys`, `now`)
//   * snake_case for two words (`group_by`, `read_json`, `random_int`)
//   * predicates start with `is_` (`is_prime`, `is_empty`)
//   * conversions are `to_x` / `from_x` (`to_json`, `from_json`, `to_hex`)
//   * a function that takes a callback takes it LAST, so the lambda closes the call:
//     `sort_by(people, \p: p.age)`
//
// Every function here is total where it reasonably can be: out-of-range indices, empty inputs,
// and nulls return a neutral value instead of throwing. An LLM cannot see a stack trace during
// generation, so a library that throws on an empty list costs a whole retry cycle; one that
// returns 0 costs nothing. Errors are reserved for cases where continuing would hide a bug
// (a malformed regex, a file write denied by the sandbox).
//
// I/O and process access are gated by the World's sandbox settings (see World.setSandbox):
// in sandbox mode reads are limited to --allow-read paths, writes to --allow-write paths, and
// subprocess execution requires --allow-exec.

const fs = require('fs');
const path = require('path');

// A ctx-aware intrinsic: the interpreter appends the evaluation context as the final argument
// so the function can call back into the language (invoking lambdas, reading the world).
function withCtx(fn) { fn.__ctx = true; return fn; }

// ---------------------------------------------------------------------------------------
// Internal helpers (not exposed to AxiomScript).
// ---------------------------------------------------------------------------------------

function toArray(v) {
  if (v == null) return [];
  if (Array.isArray(v)) return v;
  if (typeof v === 'string') return v.split('');
  if (typeof v[Symbol.iterator] === 'function') return Array.from(v);
  if (typeof v === 'object') return Object.values(v);
  return [v];
}

// mulberry32 — a small, fast, well-distributed PRNG. Seeded randomness matters for a language
// used to write simulations and tests: `seed(1)` makes a run reproducible, which is the
// difference between a test that can assert an outcome and one that can only assert a range.
function makeRng(seed) {
  let a = (seed >>> 0) || 1;
  return function () {
    a |= 0; a = (a + 0x6D2B79F5) | 0;
    let t = Math.imul(a ^ (a >>> 15), 1 | a);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

function cmpValues(a, b) {
  if (typeof a === 'number' && typeof b === 'number') return a - b;
  const sa = String(a), sb = String(b);
  return sa < sb ? -1 : sa > sb ? 1 : 0;
}

function buildRegex(pattern, flags) {
  if (pattern instanceof RegExp) return pattern;
  try { return new RegExp(pattern, flags === undefined || flags === null ? '' : String(flags)); }
  catch (e) { throw new Error(`bad regular expression /${pattern}/: ${e.message}`); }
}

function stdlibIntrinsics(world, rt) {
  // rt is the runtime bridge supplied by interpreter.js: { callValue, truthy, equalsVal,
  // stringify, AxiomError, Vec3, Atom }. Passing it in (rather than requiring the interpreter)
  // keeps this module free of a circular dependency.
  const call = (fn, args, ctx) => rt.callValue(fn, args, ctx);
  const truthy = rt.truthy;
  const fail = (msg, code) => { throw new rt.AxiomError(msg, code || 'AX-STDLIB'); };

  let rng = makeRng((Math.random() * 0xffffffff) >>> 0);

  // --- sandbox gates -------------------------------------------------------------------
  const readable = (p) => {
    const abs = path.resolve(p);
    if (!world.sandbox) return abs;
    if (world.isPathReadable(abs)) return abs;
    fail(`sandbox: reading '${p}' is not allowed (pass --allow-read ${p})`, 'AX-SANDBOX-001');
  };
  const writable = (p) => {
    const abs = path.resolve(p);
    if (!world.sandbox) return abs;
    for (const allowed of world.allowWritePaths) {
      if (abs === allowed || abs.startsWith(allowed + path.sep)) return abs;
    }
    fail(`sandbox: writing '${p}' is not allowed (pass --allow-write ${p})`, 'AX-SANDBOX-001');
  };

  // --- key/predicate coercion ----------------------------------------------------------
  // Anywhere the library takes "a key", it accepts a function OR a field name, so both
  // `sort_by(xs, \v: v.hp)` and `sort_by(xs, "hp")` work. The second form is 4 tokens cheaper
  // and is what an LLM reaches for first.
  const keyOf = (sel, ctx) => {
    if (sel == null) return (v) => v;
    if (typeof sel === 'string') return (v) => (v == null ? null : v[sel]);
    // v0.9.2: a bare library-function name reads as an atom; when it names a function it is
    // that function (`map(xs, abs)`), as it already was for the method form `xs.map(abs)`.
    if (sel instanceof rt.Atom && !(world && (world.fns.has(sel.name) || world.procs.has(sel.name) || typeof world.intrinsics[sel.name] === 'function'))) return (v) => (v == null ? null : v[sel.name]);
    return (v, i) => call(sel, [v, i], ctx);
  };
  const predOf = (sel, ctx) => {
    const k = keyOf(sel, ctx);
    return (v, i) => truthy(k(v, i));
  };

  return {
    // =====================================================================================
    // NUMBERS & MATH
    // =====================================================================================
    sum: withCtx((xs, sel, ctx) => { const k = keyOf(sel, ctx); let t = 0; const a = toArray(xs); for (let i = 0; i < a.length; i++) { const v = k(a[i], i); t += typeof v === 'number' ? v : Number(v) || 0; } return t; }),
    prod: withCtx((xs, sel, ctx) => { const k = keyOf(sel, ctx); let t = 1; const a = toArray(xs); for (let i = 0; i < a.length; i++) t *= Number(k(a[i], i)) || 0; return t; }),
    mean: withCtx((xs, sel, ctx) => { const a = toArray(xs); if (!a.length) return 0; const k = keyOf(sel, ctx); let t = 0; for (let i = 0; i < a.length; i++) t += Number(k(a[i], i)) || 0; return t / a.length; }),
    median: (xs) => { const a = toArray(xs).map(Number).sort((x, y) => x - y); if (!a.length) return 0; const m = a.length >> 1; return a.length % 2 ? a[m] : (a[m - 1] + a[m]) / 2; },
    mode: (xs) => { const a = toArray(xs); if (!a.length) return null; const c = new Map(); let best = a[0], bc = 0; for (const v of a) { const n = (c.get(v) || 0) + 1; c.set(v, n); if (n > bc) { bc = n; best = v; } } return best; },
    variance: (xs) => { const a = toArray(xs).map(Number); if (a.length < 2) return 0; const m = a.reduce((p, c) => p + c, 0) / a.length; return a.reduce((p, c) => p + (c - m) * (c - m), 0) / a.length; },
    stdev: (xs) => { const a = toArray(xs).map(Number); if (a.length < 2) return 0; const m = a.reduce((p, c) => p + c, 0) / a.length; return Math.sqrt(a.reduce((p, c) => p + (c - m) * (c - m), 0) / a.length); },
    // Integer maths. `mod` is the mathematical modulo (always non-negative for a positive
    // divisor) — distinct from `%`, which follows JS and keeps the dividend's sign.
    mod: (a, b) => b === 0 ? 0 : ((a % b) + b) % b,
    divmod: (a, b) => b === 0 ? [0, 0] : [Math.floor(a / b), ((a % b) + b) % b],
    gcd: (a, b) => { a = Math.abs(a | 0); b = Math.abs(b | 0); while (b) { const t = b; b = a % b; a = t; } return a; },
    lcm: (a, b) => { a = Math.abs(a | 0); b = Math.abs(b | 0); if (!a || !b) return 0; let x = a, y = b; while (y) { const t = y; y = x % y; x = t; } return (a / x) * b; },
    fact: (n) => { n = Math.floor(n); if (n < 0) return 0; let r = 1; for (let i = 2; i <= n; i++) r *= i; return r; },
    comb: (n, k) => { n = Math.floor(n); k = Math.floor(k); if (k < 0 || k > n) return 0; k = Math.min(k, n - k); let r = 1; for (let i = 0; i < k; i++) r = r * (n - i) / (i + 1); return Math.round(r); },
    perm: (n, k) => { n = Math.floor(n); k = Math.floor(k); if (k < 0 || k > n) return 0; let r = 1; for (let i = 0; i < k; i++) r *= (n - i); return r; },
    is_prime: (n) => { n = Math.floor(n); if (n < 2) return false; if (n % 2 === 0) return n === 2; for (let i = 3; i * i <= n; i += 2) if (n % i === 0) return false; return true; },
    primes: (limit) => { limit = Math.floor(limit); if (limit < 2) return []; const sieve = new Uint8Array(limit + 1); const out = []; for (let i = 2; i <= limit; i++) { if (sieve[i]) continue; out.push(i); for (let j = i * i; j <= limit; j += i) sieve[j] = 1; } return out; },
    isqrt: (n) => Math.floor(Math.sqrt(Math.max(0, n))),
    inv_lerp: (a, b, v) => a === b ? 0 : (v - a) / (b - a),
    round_to: (x, step) => step === 0 ? x : Math.round(x / step) * step,
    clamp01: (x) => Math.max(0, Math.min(1, x)),
    sinh: (x) => Math.sinh(x), cosh: (x) => Math.cosh(x), tanh: (x) => Math.tanh(x),
    ln: (x) => Math.log(x),
    is_nan: (x) => Number.isNaN(x),
    is_int: (x) => Number.isInteger(x),
    is_finite: (x) => Number.isFinite(x),
    is_bool: (x) => typeof x === 'boolean',
    is_fn: (x) => x != null && (typeof x === 'function' || x.__callable === true),
    is_dict: (x) => !!x && typeof x === 'object' && !Array.isArray(x) && x.constructor === Object,
    inf: () => Infinity,
    nan: () => NaN,
    num: (v) => { const n = Number(v); return Number.isNaN(n) ? 0 : n; },
    to_fixed: (x, digits) => Number(x).toFixed(digits === undefined ? 2 : digits),
    parse_int: (s, radix) => { const n = parseInt(String(s).trim(), radix || 10); return Number.isNaN(n) ? null : n; },
    to_hex: (n) => '0x' + (n >>> 0).toString(16),
    to_bin: (n) => '0b' + (n >>> 0).toString(2),
    to_base: (n, base) => Math.trunc(n).toString(Math.max(2, Math.min(36, base | 0))),
    // Arbitrary-precision integers, for the cases where float64 silently loses digits
    // (hashes, big factorials, cryptography exercises). `+ - * / %` work on two bigs.
    big: (v) => { try { return BigInt(typeof v === 'string' ? v.trim() : Math.trunc(Number(v))); } catch (e) { return 0n; } },
    is_big: (v) => typeof v === 'bigint',

    // =====================================================================================
    // RANDOMNESS (seeded — `seed(n)` makes a whole run reproducible)
    // =====================================================================================
    seed: (n) => { rng = makeRng(Math.floor(Number(n)) >>> 0); return n; },
    random: () => rng(),
    randomRange: (lo, hi) => lo + rng() * (hi - lo),
    randomInt: (lo, hi) => Math.floor(lo + rng() * (hi - lo + 1)),
    random_range: (lo, hi) => lo + rng() * (hi - lo),
    random_int: (lo, hi) => Math.floor(lo + rng() * (hi - lo + 1)),
    pick: (xs) => { const a = toArray(xs); return a.length ? a[Math.floor(rng() * a.length)] : null; },
    shuffle: (xs) => { const a = toArray(xs).slice(); for (let i = a.length - 1; i > 0; i--) { const j = Math.floor(rng() * (i + 1)); const t = a[i]; a[i] = a[j]; a[j] = t; } return a; },
    gauss: (mu, sigma) => { const u = 1 - rng(), v = rng(); return (mu || 0) + (sigma === undefined ? 1 : sigma) * Math.sqrt(-2 * Math.log(u)) * Math.cos(2 * Math.PI * v); },
    uuid: () => 'xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx'.replace(/[xy]/g, (c) => { const r = (rng() * 16) | 0; const v = c === 'x' ? r : ((r & 0x3) | 0x8); return v.toString(16); }),

    // =====================================================================================
    // COLLECTIONS
    // =====================================================================================
    sorted: withCtx((xs, sel, ctx) => {
      const a = toArray(xs).slice();
      if (sel == null) return a.sort(cmpValues);
      // A 2-argument lambda is a comparator; a 1-argument one (or a field name) is a key.
      if (sel && sel.params && sel.params.length >= 2) return a.sort((x, y) => Number(call(sel, [x, y], ctx)) || 0);
      const k = keyOf(sel, ctx);
      return a.sort((x, y) => cmpValues(k(x), k(y)));
    }),
    sort_by: withCtx((xs, sel, ctx) => { const k = keyOf(sel, ctx); return toArray(xs).slice().sort((x, y) => cmpValues(k(x), k(y))); }),
    group_by: withCtx((xs, sel, ctx) => { const k = keyOf(sel, ctx); const out = {}; const a = toArray(xs); for (let i = 0; i < a.length; i++) { const g = rt.stringify(k(a[i], i)); (out[g] = out[g] || []).push(a[i]); } return out; }),
    count_by: withCtx((xs, sel, ctx) => { const k = keyOf(sel, ctx); const out = {}; const a = toArray(xs); for (let i = 0; i < a.length; i++) { const g = rt.stringify(k(a[i], i)); out[g] = (out[g] || 0) + 1; } return out; }),
    partition: withCtx((xs, sel, ctx) => { const p = predOf(sel, ctx); const yes = [], no = []; const a = toArray(xs); for (let i = 0; i < a.length; i++) (p(a[i], i) ? yes : no).push(a[i]); return [yes, no]; }),
    min_by: withCtx((xs, sel, ctx) => { const k = keyOf(sel, ctx); const a = toArray(xs); if (!a.length) return null; let best = a[0], bk = k(a[0], 0); for (let i = 1; i < a.length; i++) { const v = k(a[i], i); if (cmpValues(v, bk) < 0) { bk = v; best = a[i]; } } return best; }),
    max_by: withCtx((xs, sel, ctx) => { const k = keyOf(sel, ctx); const a = toArray(xs); if (!a.length) return null; let best = a[0], bk = k(a[0], 0); for (let i = 1; i < a.length; i++) { const v = k(a[i], i); if (cmpValues(v, bk) > 0) { bk = v; best = a[i]; } } return best; }),
    uniq: withCtx((xs, sel, ctx) => { const k = keyOf(sel, ctx); const seen = new Set(); const out = []; const a = toArray(xs); for (let i = 0; i < a.length; i++) { const key = rt.stringify(k(a[i], i)); if (seen.has(key)) continue; seen.add(key); out.push(a[i]); } return out; }),
    zip: (...arrays) => { const as = arrays.map(toArray); const n = as.length ? Math.min(...as.map(a => a.length)) : 0; const out = []; for (let i = 0; i < n; i++) out.push(as.map(a => a[i])); return out; },
    unzip: (pairs) => { const a = toArray(pairs); const n = a.length ? Math.max(...a.map(p => toArray(p).length)) : 0; const out = []; for (let i = 0; i < n; i++) out.push(a.map(p => toArray(p)[i])); return out; },
    enumerate: (xs) => toArray(xs).map((v, i) => [i, v]),
    chunk: (xs, size) => { const a = toArray(xs); const n = Math.max(1, Math.floor(size) || 1); const out = []; for (let i = 0; i < a.length; i += n) out.push(a.slice(i, i + n)); return out; },
    windows: (xs, size) => { const a = toArray(xs); const n = Math.max(1, Math.floor(size) || 1); const out = []; for (let i = 0; i + n <= a.length; i++) out.push(a.slice(i, i + n)); return out; },
    flatten: (xs, depth) => toArray(xs).flat(depth === undefined ? Infinity : depth),
    reversed: (xs) => toArray(xs).slice().reverse(),
    take: (xs, n) => toArray(xs).slice(0, Math.max(0, Math.floor(n))),
    drop: (xs, n) => toArray(xs).slice(Math.max(0, Math.floor(n))),
    first: (xs, d) => { const a = toArray(xs); return a.length ? a[0] : (d === undefined ? null : d); },
    last: (xs, d) => { const a = toArray(xs); return a.length ? a[a.length - 1] : (d === undefined ? null : d); },
    is_empty: (v) => { if (v == null) return true; if (typeof v === 'string' || Array.isArray(v)) return v.length === 0; if (typeof v === 'object') return Object.keys(v).length === 0; return false; },
    any: withCtx((xs, sel, ctx) => { const p = sel == null ? truthy : predOf(sel, ctx); const a = toArray(xs); for (let i = 0; i < a.length; i++) if (p(a[i], i)) return true; return false; }),
    all: withCtx((xs, sel, ctx) => { const p = sel == null ? truthy : predOf(sel, ctx); const a = toArray(xs); for (let i = 0; i < a.length; i++) if (!p(a[i], i)) return false; return true; }),
    count: withCtx((xs, sel, ctx) => { const a = toArray(xs); if (sel == null) return a.length; if (typeof sel === 'function' || (sel && sel.__callable)) { const p = predOf(sel, ctx); let n = 0; for (let i = 0; i < a.length; i++) if (p(a[i], i)) n++; return n; } let n = 0; for (const v of a) if (rt.equalsVal(v, sel)) n++; return n; }),
    find_index: withCtx((xs, sel, ctx) => { const p = predOf(sel, ctx); const a = toArray(xs); for (let i = 0; i < a.length; i++) if (p(a[i], i)) return i; return -1; }),
    // Set algebra over plain arrays — no separate Set type to learn, and the result is still
    // an array every other function accepts.
    union: (a, b) => { const out = toArray(a).slice(); const seen = new Set(out.map(rt.stringify)); for (const v of toArray(b)) { const k = rt.stringify(v); if (!seen.has(k)) { seen.add(k); out.push(v); } } return out; },
    intersect: (a, b) => { const seen = new Set(toArray(b).map(rt.stringify)); const out = []; const added = new Set(); for (const v of toArray(a)) { const k = rt.stringify(v); if (seen.has(k) && !added.has(k)) { added.add(k); out.push(v); } } return out; },
    difference: (a, b) => { const seen = new Set(toArray(b).map(rt.stringify)); return toArray(a).filter(v => !seen.has(rt.stringify(v))); },
    // Dict helpers. `items` pairs with the multi-variable loop: `*k, v in items(d):`
    keys: (d) => (d && typeof d === 'object') ? Object.keys(d).filter(k => k !== '__type') : [],
    values: (d) => (d && typeof d === 'object') ? Object.keys(d).filter(k => k !== '__type').map(k => d[k]) : [],
    items: (d) => (d && typeof d === 'object') ? Object.keys(d).filter(k => k !== '__type').map(k => [k, d[k]]) : [],
    dict: (pairs) => { const out = {}; for (const p of toArray(pairs)) { const a = toArray(p); out[rt.stringify(a[0])] = a[1]; } return out; },
    merge: (...ds) => Object.assign({}, ...ds.filter(d => d && typeof d === 'object')),
    pick_keys: (d, ks) => { const out = {}; for (const k of toArray(ks)) if (d && k in d) out[k] = d[k]; return out; },
    omit_keys: (d, ks) => { const drop = new Set(toArray(ks).map(String)); const out = {}; for (const k of Object.keys(d || {})) if (!drop.has(k)) out[k] = d[k]; return out; },
    invert: (d) => { const out = {}; for (const k of Object.keys(d || {})) out[rt.stringify(d[k])] = k; return out; },
    has_key: (d, k) => !!d && typeof d === 'object' && Object.prototype.hasOwnProperty.call(d, k),
    clone: (v) => { if (v == null || typeof v !== 'object') return v; if (Array.isArray(v)) return v.map(x => (x && typeof x === 'object') ? JSON.parse(JSON.stringify(x)) : x); try { return JSON.parse(JSON.stringify(v)); } catch (e) { return Object.assign({}, v); } },
    deep_eq: (a, b) => { try { return JSON.stringify(a) === JSON.stringify(b); } catch (e) { return a === b; } },
    // Matrix-shaped helpers — enough linear algebra for grids, games of life, dynamic
    // programming tables and small solvers without reaching for a Mat4.
    grid: withCtx((rows, cols, fill, ctx) => { const out = []; for (let r = 0; r < rows; r++) { const row = []; for (let c = 0; c < cols; c++) row.push((fill && (typeof fill === 'function' || fill.__callable)) ? call(fill, [r, c], ctx) : (fill === undefined ? 0 : fill)); out.push(row); } return out; }),
    transpose: (m) => { const a = toArray(m).map(toArray); if (!a.length) return []; const cols = Math.max(...a.map(r => r.length)); const out = []; for (let c = 0; c < cols; c++) out.push(a.map(r => r[c])); return out; },

    // =====================================================================================
    // STRINGS, ENCODING, REGEX
    // =====================================================================================
    ord: (c) => String(c).charCodeAt(0),
    chr: (n) => String.fromCharCode(n),
    lines: (s) => String(s === null || s === undefined ? '' : s).split(/\r?\n/),
    words: (s) => String(s === null || s === undefined ? '' : s).trim().split(/\s+/).filter(Boolean),
    chars: (s) => String(s === null || s === undefined ? '' : s).split(''),
    capitalize: (s) => { const t = String(s); return t ? t[0].toUpperCase() + t.slice(1) : t; },
    title: (s) => String(s).replace(/\w\S*/g, (w) => w[0].toUpperCase() + w.slice(1).toLowerCase()),
    reverse_str: (s) => String(s).split('').reverse().join(''),
    // Regular expressions. `re_*` take the pattern as a plain string, so no regex literal
    // syntax has to be added to the lexer (and no escaping rules have to be learned twice).
    re_test: (s, pat, flags) => buildRegex(pat, flags).test(String(s)),
    re_match: (s, pat, flags) => { const m = String(s).match(buildRegex(pat, flags)); if (!m) return null; return { match: m[0], index: m.index, groups: Array.from(m).slice(1) }; },
    re_all: (s, pat, flags) => { const re = buildRegex(pat, (flags || '') + (String(flags || '').includes('g') ? '' : 'g')); const out = []; let m; while ((m = re.exec(String(s))) !== null) { out.push({ match: m[0], index: m.index, groups: Array.from(m).slice(1) }); if (m.index === re.lastIndex) re.lastIndex++; } return out; },
    re_sub: (s, pat, repl, flags) => String(s).replace(buildRegex(pat, (flags || '') + (String(flags || '').includes('g') ? '' : 'g')), String(repl)),
    re_split: (s, pat, flags) => String(s).split(buildRegex(pat, flags)),
    b64_encode: (s) => Buffer.from(String(s), 'utf8').toString('base64'),
    b64_decode: (s) => Buffer.from(String(s), 'base64').toString('utf8'),
    // FNV-1a — a stable 32-bit hash. Stable across runs (unlike a JS object's ordering), which
    // makes it usable for bucketing, caching, and deterministic colour/name pickers.
    hash: (s) => { let h = 0x811c9dc5; const t = String(s); for (let i = 0; i < t.length; i++) { h ^= t.charCodeAt(i); h = Math.imul(h, 0x01000193); } return h >>> 0; },
    to_json: (v, pretty) => { try { return pretty ? JSON.stringify(v, null, 2) : JSON.stringify(v); } catch (e) { return '<circular>'; } },
    from_json: (s) => { try { return JSON.parse(s); } catch (e) { return null; } },

    // =====================================================================================
    // TIME
    // =====================================================================================
    now: () => Date.now(),
    time: () => Date.now() / 1000,
    date_iso: (ms) => new Date(ms === undefined ? Date.now() : ms).toISOString(),
    // Blocking sleep. Scripts need it (polling, rate limits); a frame block should not use it.
    sleep: (sec) => { const end = Date.now() + Math.max(0, Number(sec) * 1000); while (Date.now() < end) { /* spin */ } return null; },

    // =====================================================================================
    // FILES, PROCESS, ENVIRONMENT  (sandbox-gated)
    // =====================================================================================
    read: (p, dflt) => { try { return fs.readFileSync(readable(p), 'utf8'); } catch (e) { if (e && e.axiomCode) throw e; return dflt === undefined ? null : dflt; } },
    read_lines: (p) => { try { return fs.readFileSync(readable(p), 'utf8').split(/\r?\n/); } catch (e) { if (e && e.axiomCode) throw e; return []; } },
    read_json: (p, dflt) => { try { return JSON.parse(fs.readFileSync(readable(p), 'utf8')); } catch (e) { if (e && e.axiomCode) throw e; return dflt === undefined ? null : dflt; } },
    write: (p, s) => { const abs = writable(p); fs.mkdirSync(path.dirname(abs), { recursive: true }); fs.writeFileSync(abs, s === null || s === undefined ? '' : String(s)); return true; },
    write_json: (p, v, pretty) => { const abs = writable(p); fs.mkdirSync(path.dirname(abs), { recursive: true }); fs.writeFileSync(abs, JSON.stringify(v, null, pretty ? 2 : 0)); return true; },
    append: (p, s) => { const abs = writable(p); fs.mkdirSync(path.dirname(abs), { recursive: true }); fs.appendFileSync(abs, s === null || s === undefined ? '' : String(s)); return true; },
    // Named `file_exists` because `exists` is taken by the entity query `?exists(#Tag)`. The
    // query accepts a string and delegates here, so both spellings work.
    file_exists: (p) => { try { return fs.existsSync(path.resolve(p)); } catch (e) { return false; } },
    ls: (p) => { try { return fs.readdirSync(readable(p || '.')); } catch (e) { if (e && e.axiomCode) throw e; return []; } },
    mkdir: (p) => { const abs = writable(p); fs.mkdirSync(abs, { recursive: true }); return true; },
    rm: (p) => { const abs = writable(p); try { fs.rmSync(abs, { recursive: true, force: true }); return true; } catch (e) { return false; } },
    is_dir: (p) => { try { return fs.statSync(path.resolve(p)).isDirectory(); } catch (e) { return false; } },
    path_join: (...parts) => path.join(...parts.map(String)),
    // stdin, for interactive CLIs and for programs fed by a pipe. Returns null at EOF.
    input: (prompt) => {
      if (prompt !== undefined && prompt !== null) process.stdout.write(String(prompt));
      const buf = Buffer.alloc(1);
      let line = '';
      for (;;) {
        let n = 0;
        try { n = fs.readSync(0, buf, 0, 1, null); } catch (e) { break; }
        if (n === 0) break;
        const ch = buf.toString('utf8');
        if (ch === '\n') return line;
        if (ch !== '\r') line += ch;
      }
      return line.length ? line : null;
    },
    read_stdin: () => { try { return fs.readFileSync(0, 'utf8'); } catch (e) { return ''; } },
    args: () => (world.argv || []).slice(),
    env: (name, dflt) => { const v = process.env[String(name)]; return v === undefined ? (dflt === undefined ? null : dflt) : v; },
    eprint: (...vals) => { process.stderr.write(vals.map(v => typeof v === 'string' ? v : rt.stringify(v)).join(' ') + '\n'); return null; },
    exit: (code) => { const c = (Number(code) || 0) | 0; world.exitCode = c; const e = new rt.AxiomError(`exit(${c})`, 'AX-EXIT'); e.__exit = c; throw e; },
    sh: (cmd) => {
      if (world.sandbox && !world.allowExec) fail(`sandbox: running commands is not allowed (pass --allow-exec)`, 'AX-SANDBOX-001');
      const { execSync } = require('child_process');
      try { return { out: execSync(String(cmd), { encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'] }), code: 0 }; }
      catch (e) { return { out: (e.stdout || '') + (e.stderr || ''), code: e.status === undefined ? 1 : e.status }; }
    },

    // =====================================================================================
    // HIGHER-ORDER ENTRY POINTS (function-call forms of the array methods, for `|>` chains)
    // =====================================================================================
    map: withCtx((xs, fn, ctx) => { const a = toArray(xs); const k = keyOf(fn, ctx); return a.map((v, i) => k(v, i)); }),
    filter: withCtx((xs, fn, ctx) => { const p = predOf(fn, ctx); return toArray(xs).filter((v, i) => p(v, i)); }),
    reduce: withCtx((xs, fn, init, ctx) => { const a = toArray(xs); let acc = init; let start = 0; if (acc === undefined) { acc = a[0]; start = 1; } for (let i = start; i < a.length; i++) acc = call(fn, [acc, a[i], i], ctx); return acc === undefined ? null : acc; }),
    each: withCtx((xs, fn, ctx) => { const a = toArray(xs); for (let i = 0; i < a.length; i++) call(fn, [a[i], i], ctx); return null; }),
    find: withCtx((xs, fn, ctx) => { const p = predOf(fn, ctx); const a = toArray(xs); for (let i = 0; i < a.length; i++) if (p(a[i], i)) return a[i]; return null; }),
    apply: withCtx((fn, argList, ctx) => call(fn, toArray(argList), ctx)),
    // Function composition and partial application — the two combinators that pay for
    // themselves immediately in a pipeline.
    compose: withCtx((...rest) => { const ctx = rest.pop(); const fns = rest; return (...xs) => fns.reduceRight((acc, f, i) => i === fns.length - 1 ? call(f, xs, ctx) : call(f, [acc], ctx), null); }),
    partial: withCtx((...rest) => { const ctx = rest.pop(); const fn = rest.shift(); const bound = rest; return (...xs) => call(fn, [...bound, ...xs], ctx); }),
    // `memo` makes exponential recursion linear — the single most valuable four tokens in a
    // language LLMs use for dynamic programming.
    memo: withCtx((fn, ctx) => { const cache = new Map(); return (...xs) => { const k = xs.map(rt.stringify).join('\u0001'); if (cache.has(k)) return cache.get(k); const v = call(fn, xs, ctx); cache.set(k, v); return v; }; }),
    // Assertions that throw (so ^try can catch them) rather than logging — what a test needs.
    check: withCtx((cond, msg, ctx) => { if (!truthy(cond)) fail(msg === undefined ? 'check failed' : rt.stringify(msg), 'AX-CHECK'); return true; }),
    check_eq: withCtx((a, b, msg, ctx) => { if (!rt.equalsVal(a, b) && rt.stringify(a) !== rt.stringify(b)) fail(`${msg === undefined ? 'check_eq failed' : rt.stringify(msg)}: ${rt.stringify(a)} != ${rt.stringify(b)}`, 'AX-CHECK'); return true; }),
  };
}

module.exports = { stdlibIntrinsics, withCtx, makeRng, toArray };
