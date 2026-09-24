// test_v090_general.js — v0.9.0 "general-purpose" feature suite.
//
// Covers the language changes that turn AxiomScript from a game-world DSL into a language you
// can write any program in: scripts (^main), lexical scope and recursion, lambdas and
// closures, pipelines, error handling, pattern matching, records, modules, and the standard
// library. Every test that can assert a VALUE does — a test that only asserts "it compiled"
// catches nothing (that was the weakest part of the pre-0.9 suites).
//
// Run: node test_v090_general.js

const fs = require('fs');
const os = require('os');
const path = require('path');
const { compile } = require('./checker');
const { World } = require('./interpreter');
const { spawnSync } = require('child_process');

let passed = 0, failed = 0;
function ok(label, cond, extra) {
  if (cond) { passed++; console.log(`OK   ${label}`); }
  else { failed++; console.log(`FAIL ${label}${extra === undefined ? '' : ` — ${extra}`}`); }
}
function eq(label, actual, expected) {
  const a = JSON.stringify(actual), e = JSON.stringify(expected);
  ok(label, a === e, `got ${a}, want ${e}`);
}
function section(name) { console.log(`\n--- ${name} ---`); }

// Compile + run a script, returning {world, result, diagnostics, logs}.
function run(src, opts) {
  const r = compile(src, Object.assign({ imports: false }, opts || {}));
  const blocking = r.diagnostics.filter(d => d.severity === 'fatal' || d.severity === 'contract_violation');
  if (blocking.length) {
    throw new Error(`compile failed: ${blocking.map(d => `${d.error_code} L${d.location && d.location.line}: ${d.message_for_human}`).join(' | ')}`);
  }
  const w = new World().loadProgram(r.program, src);
  w._suppressConsole = true;
  const result = w.runMain((opts && opts.argv) || []);
  return { world: w, result, diagnostics: r.diagnostics, logs: w.log.map(l => l.msg).filter(m => m !== undefined) };
}
// Run a one-expression program and return the value ^main returned.
function val(expr, prelude) {
  return run(`${prelude || ''}\n^main:\n  ^return ${expr}\n`).result;
}

// =========================================================================================
section('1. ^main — programs without a game loop');
// =========================================================================================
{
  const r = run(`^main:\n  print("hi")\n  ^return 7\n`);
  eq('1.1 ^main runs and returns', r.result, 7);
  eq('1.2 print() reaches the log', r.logs, ['hi']);
  eq('1.3 exit code follows the return value', r.world.exitCode, 7);

  const one = run(`^main: ^return 1 + 1\n`);
  eq('1.4 single-line ^main body', one.result, 2);

  const expr = run(`^main = 6 * 7\n`);
  ok('1.5 expression-body ^main runs', expr.world.log.length === 0);

  const argv = run(`^main(a):\n  ^return a[1]\n`, { argv: ['x', 'y'] });
  eq('1.6 ^main receives argv', argv.result, 'y');

  const noEntities = run(`^main:\n  ^return 1\n`);
  eq('1.7 a script needs no entities', noEntities.world.entities.length, 0);

  // A program that is only declarations is a library, flagged as an advisory, not an error.
  const lib = compile(`^fn f(x) = x\n`, { imports: false });
  ok('1.8 library file compiles', lib.ok);
  ok('1.9 library file gets the no-entry-point advisory',
    lib.diagnostics.some(d => d.error_code === 'AX-MAIN-001' && d.severity === 'advisory'));
}

// =========================================================================================
section('2. Lexical scope and recursion');
// =========================================================================================
{
  eq('2.1 recursion returns the right value',
    val('fib(20)', '^fn fib(n) = n < 2 ? n : fib(n-1) + fib(n-2)'), 6765);

  // The pre-0.9 failure: a recursive call overwrote the caller's locals, because every
  // assignment wrote to the shared entity. Each frame must be independent.
  const src = `
^fn count_down(n):
  acc = 0
  ?n <= 0:
    ^return 0
  acc = n + count_down(n - 1)
  ^return acc
^main:
  ^return count_down(10)
`;
  eq('2.2 locals survive a recursive call', run(src).result, 55);

  eq('2.3 parameters are assignable (shadowing works)',
    val('bump(5)', '^fn bump(n):\n  n = n + 1\n  ^return n'), 6);

  eq('2.4 a function cannot see its caller locals',
    val('outer()', '^fn inner() = type(secret)\n^fn outer():\n  secret = 42\n  ^return inner()'), 'atom');

  const loopScope = run(`
^main:
  total = 0
  *i in 0..5:
    sq = i * i
    total += sq
  ^return total
`);
  eq('2.5 accumulator survives the loop body scope', loopScope.result, 30);

  eq('2.6 globals are visible inside functions',
    run(`~K: 9\n^fn f() = K * 2\n^main:\n  ^return f()\n`).result, 18);

  eq('2.7 a global can build on an earlier one',
    run(`~A: 3\n~B: A * 4\n^main:\n  ^return B\n`).result, 12);

  eq('2.8 nested loops do not clobber each other', run(`
^main:
  n = 0
  *i in 0..3:
    *i2 in 0..3:
      n += 1
  ^return n
`).result, 9);
}

// =========================================================================================
section('3. Lambdas, closures, first-class functions');
// =========================================================================================
{
  eq('3.1 lambda applied inline', val('(\\x: x * 3)(4)'), 12);
  eq('3.2 lambda with two params', val('add(2, 3)', '^fn add(a, b) = (\\x, y: x + y)(a, b)'), 5);
  eq('3.2b two-param lambda value', val('(\\x, y: x + y)(2, 3)'), 5);
  eq('3.3 zero-param lambda', val('(\\: 42)()'), 42);
  eq('3.4 => alias for the lambda body', val('(\\x => x + 1)(1)'), 2);

  eq('3.5 .map with a lambda', val('[1,2,3].map(\\x: x * 2)'), [2, 4, 6]);
  eq('3.6 .filter with a lambda', val('[1,2,3,4].filter(\\x: x % 2 == 0)'), [2, 4]);
  eq('3.7 .reduce with a lambda', val('[1,2,3,4].reduce(\\a, b: a + b, 0)'), 10);
  eq('3.8 a ^fn name is a function value', val('[1,2,3].map(sq)', '^fn sq(x) = x * x'), [1, 4, 9]);
  eq('3.9 .sort with a comparator', val('[3,1,2].sort(\\a, b: b - a)'), [3, 2, 1]);
  eq('3.10 .sort_by with a field name', val('[{n: 2}, {n: 1}].sort_by("n").map(\\r: r.n)'), [1, 2]);

  // Closures capture their defining scope, so a returned function still works.
  eq('3.11 closure captures its environment',
    val('make(10)(5)', '^fn make(base) = \\x: x + base'), 15);

  eq('3.12 functions stored in a dict are callable',
    run(`
^main:
  ops = {inc: \\x: x + 1, dbl: \\x: x * 2}
  ^return ops.dbl(ops.inc(3))
`).result, 8);

  eq('3.13 functions stored in a list are callable',
    run(`
^main:
  fs = [\\x: x + 1, \\x: x * 10]
  ^return fs[1](fs[0](1))
`).result, 20);

  eq('3.14 a lambda passed as a parameter is callable by name',
    val('twice(\\x: x + 3, 1)', '^fn twice(f, v) = f(f(v))'), 7);

  // The v0.8 failure mode: .map with anything but a bare ^fn name silently returned a copy.
  eq('3.15 .map never silently no-ops', val('[1,2].map(\\x: 0)'), [0, 0]);

  // Loop bodies reuse one scope frame unless they create a closure; when they do, each
  // iteration must capture its own value.
  eq('3.16 closures in a loop capture the iteration value', run(`
^main:
  fs = []
  *i in 0..3:
    fs.push(\\x: x + i)
  ^return [fs[0](0), fs[1](0), fs[2](0)]
`).result, [0, 1, 2]);
  eq('3.17 a closure outlives the function that made it',
    run(`^fn counter_from(n) = \\d: n + d\n^main:\n  f = counter_from(10)\n  ^return f(1) + f(2)\n`).result, 23);
}

// =========================================================================================
section('4. Pipelines');
// =========================================================================================
{
  eq('4.1 pipe into an intrinsic', val('[1,2,3] |> sum'), 6);
  eq('4.2 pipe into a call inserts the value first', val('[1,2,3] |> map(\\x: x * 2) |> sum'), 12);
  eq('4.3 pipe into a lambda', val('4 |> \\x: x * x'), 16);
  eq('4.4 long chain stays left-to-right',
    val('[3,1,2] |> sorted |> reversed |> first'), 3);
  eq('4.5 pipe into a user fn', val('3 |> sq', '^fn sq(x) = x * x'), 9);
}

// =========================================================================================
section('5. Errors: ^try / ^catch / ^fin / ^throw');
// =========================================================================================
{
  eq('5.1 catch binds a thrown string',
    run(`^main:\n  ^try:\n    ^throw "boom"\n  ^catch e:\n    ^return e.msg\n`).result, 'boom');

  eq('5.2 catch binds a thrown record',
    run(`^main:\n  ^try:\n    ^throw {code: "E_IO", msg: "no file"}\n  ^catch e:\n    ^return e.code\n`).result, 'E_IO');

  eq('5.3 a runtime fault is catchable',
    run(`^main:\n  ^try:\n    x = missing_thing()\n  ^catch e:\n    ^return "caught"\n`).result, 'caught');

  eq('5.4 ^fin runs after the body',
    run(`^main:\n  trace = []\n  ^try:\n    trace.push("body")\n  ^catch e:\n    trace.push("catch")\n  ^fin:\n    trace.push("fin")\n  ^return trace\n`).result, ['body', 'fin']);

  eq('5.5 ^fin runs after a catch',
    run(`^main:\n  trace = []\n  ^try:\n    ^throw "x"\n  ^catch e:\n    trace.push("catch")\n  ^fin:\n    trace.push("fin")\n  ^return trace\n`).result, ['catch', 'fin']);

  eq('5.6 throwing inside a called function unwinds to the caller catch',
    run(`^fn boom():\n  ^throw "inner"\n^main:\n  ^try:\n    boom()\n  ^catch e:\n    ^return e.msg\n`).result, 'inner');

  eq('5.7 a retry loop works',
    run(`
^fn flaky(n):
  ?n < 3:
    ^throw "not yet"
  ^return "ok"
^main:
  attempt = 0
  *attempt < 5:
    ^try:
      r = flaky(attempt)
      ^return attempt
    ^catch e:
      attempt += 1
  ^return -1
`).result, 3);

  // An uncaught throw is a fault, and the value reaches the caller.
  let threw = false;
  try { run(`^main:\n  ^throw "unhandled"\n`); } catch (e) { threw = true; }
  ok('5.8 an uncaught throw propagates out of ^main', threw);

  eq('5.9 check_eq throws on mismatch (usable as a test assertion)',
    run(`^main:\n  ^try:\n    check_eq(1, 2, "nope")\n  ^catch e:\n    ^return e.code\n`).result, 'AX-CHECK');
}

// =========================================================================================
section('6. ?* match');
// =========================================================================================
{
  const dispatch = `
^fn apply(op, a, b):
  ?* op:
    "add": ^return a + b
    "sub": ^return a - b
    "mul", "times": ^return a * b
    _: ^throw f"unknown op {op}"
`;
  eq('6.1 match on a string', val('apply("add", 2, 3)', dispatch), 5);
  eq('6.2 several patterns per arm', val('apply("times", 2, 3)', dispatch), 6);
  eq('6.3 default arm runs',
    run(`${dispatch}\n^main:\n  ^try:\n    apply("nope", 1, 1)\n  ^catch e:\n    ^return e.msg\n`).result, 'unknown op nope');

  eq('6.4 match on numbers',
    run(`^main:\n  r = ""\n  ?* 2:\n    1: r = "one"\n    2: r = "two"\n  ^return r\n`).result, 'two');

  eq('6.5 match on atoms (state machines)',
    run(`^main:\n  s = idle\n  out = ""\n  ?* s:\n    running: out = "go"\n    idle: out = "wait"\n  ^return out\n`).result, 'wait');

  eq('6.6 match falls through with no default and no hit',
    run(`^main:\n  r = "untouched"\n  ?* 9:\n    1: r = "one"\n  ^return r\n`).result, 'untouched');

  eq('6.7 match on a record type',
    run(`^type Leaf: v\n^type Node: l, r\n^main:\n  n = Leaf(3)\n  ?* n:\n    Node: ^return "node"\n    Leaf: ^return "leaf"\n`).result, 'leaf');
}

// =========================================================================================
section('7. Records (^type constructors)');
// =========================================================================================
{
  eq('7.1 positional construction',
    run(`^type P: x:: number, y:: number\n^main:\n  p = P(1, 2)\n  ^return p.y\n`).result, 2);
  eq('7.2 named construction',
    run(`^type P: x, y\n^main:\n  p = P(y: 5, x: 1)\n  ^return p.x + p.y\n`).result, 6);
  eq('7.3 omitted fields are null',
    run(`^type P: x, y\n^main:\n  p = P(1)\n  ^return is_null(p.y)\n`).result, true);
  eq('7.4 type() reports the record name',
    run(`^type P: x\n^main:\n  ^return type(P(1))\n`).result, 'P');
  eq('7.5 len() ignores the type tag',
    run(`^type P: x, y\n^main:\n  ^return len(P(1, 2))\n`).result, 2);
  eq('7.6 single-line ^type with untyped fields parses',
    run(`^type V: a, b, c\n^main:\n  ^return V(1,2,3).c\n`).result, 3);
}

// =========================================================================================
section('8. Collections, dicts, destructuring');
// =========================================================================================
{
  eq('8.1 dict literal with a string key', val('{"a-b": 7}["a-b"]'), 7);
  eq('8.2 computed key', val('{[1 + 1]: "two"}["2"]'), 'two');
  eq('8.3 shorthand {x} packs a local',
    run(`^main:\n  x = 5\n  d = {x}\n  ^return d.x\n`).result, 5);
  eq('8.4 trailing comma tolerated', val('len({a: 1, b: 2,})'), 2);

  eq('8.5 multi-var loop over items()',
    run(`^main:\n  d = {a: 1, b: 2}\n  t = 0\n  *k, v in items(d):\n    t += v\n  ^return t\n`).result, 3);
  eq('8.6 multi-var loop over zip()',
    run(`^main:\n  out = []\n  *a, b in zip([1,2], [10,20]):\n    out.push(a * b)\n  ^return out\n`).result, [10, 40]);
  eq('8.7 loop over a string iterates characters',
    run(`^main:\n  n = 0\n  *c in "abc":\n    n += 1\n  ^return n\n`).result, 3);
  eq('8.8 range with a step', val('[x for x in range(0, 10, 3)]'), [0, 3, 6, 9]);
  eq('8.9 range counting down', val('[x for x in range(3, 0, -1)]'), [3, 2, 1]);
  eq('8.10 dict .map_values', val('{a: 1, b: 2}.map_values(\\v: v * 10).b'), 20);
  eq('8.11 dict .items round-trips through dict()', val('len(dict(items({a: 1, b: 2})))'), 2);
  eq('8.12 group_by', val('len(keys(group_by([1,2,3,4], \\n: n % 2)))'), 2);
  eq('8.13 array .len()', val('[1,2,3].len()'), 3);
  // v0.9.0: structural equality — tuples and records compare by value, which is what makes
  // them usable as match patterns and as members of a set.
  eq('8.14 arrays compare structurally', val('[[1,2] == [1,2], [1,2] == [2,1]]'), [true, false]);
  eq('8.15 dicts compare structurally', val('{a: 1} == {a: 1}'), true);
  eq('8.16 nested structures compare', val('{a: [1, {b: 2}]} == {a: [1, {b: 2}]}'), true);
  eq('8.17 a tuple works as a match pattern',
    run(`^main:\n  ?* [1, 0]:\n    [0, 0]: ^return "zero"\n    [1, 0]: ^return "one-zero"\n`).result, 'one-zero');
  eq('8.18 entities still compare by identity (not structurally)',
    run(`^main:\n  ^return {a: 1} == {a: 2}\n`).result, false);
}

// =========================================================================================
section('9. Standard library');
// =========================================================================================
{
  eq('9.1 sum/mean/median', val('[sum([1,2,3]), mean([1,2,3]), median([3,1,2])]'), [6, 2, 2]);
  eq('9.2 stdev of a constant list is 0', val('stdev([5,5,5])'), 0);
  eq('9.3 gcd/lcm', val('[gcd(12, 18), lcm(4, 6)]'), [6, 12]);
  eq('9.4 mod is the mathematical modulo', val('[mod(-1, 5), -1 % 5]'), [4, -1]);
  eq('9.5 is_prime / primes', val('[is_prime(97), len(primes(30))]'), [true, 10]);
  eq('9.6 fact / comb', val('[fact(5), comb(5, 2)]'), [120, 10]);
  eq('9.7 sorted is numeric, not lexicographic', val('sorted([10, 9, 100])'), [9, 10, 100]);
  eq('9.8 sort_by with a key lambda', val('sort_by([{a: 3}, {a: 1}], \\r: r.a).map(\\r: r.a)'), [1, 3]);
  eq('9.9 min_by / max_by', val('[min_by([3,1,2], \\v: v), max_by([3,1,2], \\v: v)]'), [1, 3]);
  eq('9.10 uniq preserves order', val('uniq([3,1,3,2,1])'), [3, 1, 2]);
  eq('9.11 zip / enumerate', val('[len(zip([1,2],[3,4])), enumerate(["a"])[0][0]]'), [2, 0]);
  eq('9.12 chunk / flatten', val('[chunk([1,2,3,4,5], 2).len(), flatten([[1,[2]],[3]])]'), [3, [1, 2, 3]]);
  eq('9.13 set algebra', val('[union([1,2],[2,3]), intersect([1,2],[2,3]), difference([1,2],[2])]'), [[1, 2, 3], [2], [1]]);
  eq('9.14 partition', val('partition([1,2,3,4], \\n: n % 2 == 0)'), [[2, 4], [1, 3]]);
  eq('9.15 count with a value and with a predicate', val('[count([1,1,2], 1), count([1,2,3], \\n: n > 1)]'), [2, 2]);
  eq('9.16 any / all', val('[any([0, 3], \\n: n > 2), all([1,2], \\n: n > 0)]'), [true, true]);

  eq('9.17 string helpers', val('[ord("A"), chr(66), capitalize("abc"), "a,b".split(",")]'), [65, 'B', 'Abc', ['a', 'b']]);
  eq('9.18 lines / words', val('["a\\nb".lines().len(), words("  a  b ")]'), [2, ['a', 'b']]);
  eq('9.19 regex test/match/sub', val('[re_test("a1", "\\\\d"), re_sub("a1b2", "\\\\d", "#"), re_all("a1b2", "\\\\d").len()]'), [true, 'a#b#', 2]);
  eq('9.20 regex capture groups', val('re_match("2024-06", "(\\\\d+)-(\\\\d+)").groups'), ['2024', '06']);
  eq('9.21 json round-trip', val('from_json(to_json({a: [1, 2]})).a'), [1, 2]);
  eq('9.22 base64 round-trip', val('b64_decode(b64_encode("hi"))'), 'hi');
  eq('9.23 hash is stable', val('hash("abc") == hash("abc")'), true);

  eq('9.24 seeded rng is reproducible',
    run(`^main:\n  seed(7)\n  a = [random_int(1, 100), random_int(1, 100)]\n  seed(7)\n  b = [random_int(1, 100), random_int(1, 100)]\n  ^return a[0] == b[0] && a[1] == b[1]\n`).result, true);
  eq('9.25 shuffle keeps the elements',
    run(`^main:\n  seed(1)\n  ^return sorted(shuffle([1,2,3,4]))\n`).result, [1, 2, 3, 4]);

  eq('9.26 memo makes repeated calls cheap',
    run(`
^fn slow(n) = n * 2
^main:
  f = memo(slow)
  ^return f(21) + f(21)
`).result, 84);
  eq('9.27 partial application', val('partial(\\a, b: a + b, 10)(5)'), 15);
  eq('9.28 apply spreads an argument list', val('apply(\\a, b: a * b, [3, 4])'), 12);
  eq('9.29 bigint arithmetic beats float64', val('str(big("9007199254740993") + big(2))'), '9007199254740995');
  eq('9.30 grid builds a 2D array', val('grid(2, 3, 0)'), [[0, 0, 0], [0, 0, 0]]);
  eq('9.31 transpose', val('transpose([[1,2],[3,4]])'), [[1, 3], [2, 4]]);
  eq('9.32 to_hex / parse_int', val('[to_hex(255), parse_int("ff", 16)]'), ['0xff', 255]);
}

// =========================================================================================
section('10. Files, argv, environment (and the sandbox)');
// =========================================================================================
{
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'axiom-'));
  const file = path.join(dir, 'data.json').replace(/\\/g, '/');
  const txt = path.join(dir, 'out.txt').replace(/\\/g, '/');

  eq('10.1 write then read a file',
    run(`^main:\n  write("${txt}", "hello")\n  ^return read("${txt}")\n`).result, 'hello');
  eq('10.2 write_json then read_json',
    run(`^main:\n  write_json("${file}", {n: 42})\n  ^return read_json("${file}").n\n`).result, 42);
  eq('10.3 append', run(`^main:\n  append("${txt}", "!")\n  ^return read("${txt}")\n`).result, 'hello!');
  eq('10.4 exists / ls', run(`^main:\n  ^return [exists("${txt}"), len(ls("${dir}")) >= 2]\n`).result, [true, true]);
  eq('10.5 read of a missing file returns the default',
    run(`^main:\n  ^return read("${dir}/nope.txt", "fallback")\n`).result, 'fallback');
  eq('10.6 args() reflects the program argv',
    run(`^main:\n  ^return args()\n`, { argv: ['a', 'b'] }).result, ['a', 'b']);
  eq('10.7 env() with a default', run(`^main:\n  ^return env("AXIOM_TEST_NOPE", "dflt")\n`).result, 'dflt');

  // Sandbox: a write outside the allowed paths is refused, and the refusal is catchable.
  const r = compile(`^main:\n  ^try:\n    write("${txt}", "x")\n  ^catch e:\n    ^return e.code\n`, { imports: false });
  const w = new World();
  w.setSandbox({ allowReadPaths: [], allowWritePaths: [] });
  w.loadProgram(r.program, '');
  w._suppressConsole = true;
  eq('10.8 sandbox denies an unlisted write', w.runMain([]), 'AX-SANDBOX-001');

  const w2 = new World();
  w2.setSandbox({ allowWritePaths: [dir] });
  w2.loadProgram(compile(`^main:\n  write("${txt}", "y")\n  ^return "wrote"\n`, { imports: false }).program, '');
  w2._suppressConsole = true;
  eq('10.9 sandbox allows an --allow-write path', w2.runMain([]), 'wrote');

  fs.rmSync(dir, { recursive: true, force: true });
}

// =========================================================================================
section('11. Modules (^use)');
// =========================================================================================
{
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'axiom-mod-'));
  fs.writeFileSync(path.join(dir, 'mathlib.ax'), `~TWO: 2\n^fn triple(x) = x * 3\n`);
  fs.writeFileSync(path.join(dir, 'util.ax'), `^use "mathlib.ax"\n^fn sextuple(x) = triple(x) * TWO\n`);
  const mainPath = path.join(dir, 'app.ax');
  fs.writeFileSync(mainPath, `^use "util.ax"\n^main:\n  ^return sextuple(2)\n`);

  const r = compile(fs.readFileSync(mainPath, 'utf8'), { filename: mainPath });
  ok('11.1 a program with imports compiles', r.ok, JSON.stringify(r.diagnostics.map(d => d.message_for_human)));
  const w = new World().loadProgram(r.program, '');
  w._suppressConsole = true;
  eq('11.2 transitive imports resolve', w.runMain([]), 12);

  const missing = compile(`^use "nope.ax"\n^main:\n  ^return 1\n`, { filename: mainPath });
  ok('11.3 a missing import is fatal',
    !missing.ok && missing.diagnostics.some(d => d.error_code === 'AX-USE-001'));

  // A cycle must terminate rather than recurse forever.
  fs.writeFileSync(path.join(dir, 'a.ax'), `^use "b.ax"\n^fn fa() = 1\n`);
  fs.writeFileSync(path.join(dir, 'b.ax'), `^use "a.ax"\n^fn fb() = 2\n`);
  const cyc = path.join(dir, 'cyc.ax');
  fs.writeFileSync(cyc, `^use "a.ax"\n^main:\n  ^return fa() + fb()\n`);
  const rc = compile(fs.readFileSync(cyc, 'utf8'), { filename: cyc });
  ok('11.4 an import cycle terminates', rc.ok);
  const wc = new World().loadProgram(rc.program, '');
  wc._suppressConsole = true;
  eq('11.5 both sides of the cycle are usable', wc.runMain([]), 3);

  fs.rmSync(dir, { recursive: true, force: true });
}

// =========================================================================================
section('12. Backward compatibility — the game runtime is unchanged');
// =========================================================================================
{
  const src = `
^event Hit:
  damage:: number
  source:: #Entity
^proc heal(n):
  hp += n
^fn double(x) = x * 2
@Player &Body3D
  ~hp: 50
  ~score: 0
  ~items: [1, 2, 3]
  &tick(10hz):
    total = 0
    *v in items:
      total += double(v)
    score = total
    !heal(1)
  &on(Hit):
    hp -= damage
`;
  const r = compile(src, { imports: false });
  ok('12.1 a v0.8-style program still compiles', r.ok);
  const w = new World().loadProgram(r.program, src);
  for (let i = 0; i < 12; i++) w.update(1 / 60);
  const p = w.tags.get('Player');
  eq('12.2 entity fields still accumulate in a tick block', p.get('score'), 12);
  ok('12.3 a ^proc still mutates the caller entity field', p.get('hp') > 50);
  w.deliverBroadcast('Hit', { damage: 10, source: p }, p, { mode: 'all' });
  ok('12.4 event handlers still fire', p.get('hp') < 60 + 10);
  eq('12.5 no runtime faults', w.runtimeDiagnostics.map(d => d.error_code), []);

  // Frame-local scratch state written without a declaration still lands on the entity.
  const src2 = `
@A
  ~n: 0
  &tick(60hz):
    scratch = 5
    n = scratch
`;
  const r2 = compile(src2, { imports: false });
  const w2 = new World().loadProgram(r2.program, src2);
  for (let i = 0; i < 4; i++) w2.update(1 / 60);
  eq('12.6 undeclared names in a block still write to the entity', w2.tags.get('A').get('n'), 5);

  // Hot blocks keep an iteration cap; scripts do not.
  const hot = `
@B
  &tick(60hz):
    i = 0
    *i < 999999999:
      i += 1
`;
  const rh = compile(hot, { imports: false });
  const wh = new World().loadProgram(rh.program, hot);
  wh.update(1 / 60);
  ok('12.7 a runaway loop in a hot block is stopped, not hung',
    wh.runtimeDiagnostics.some(d => d.error_code === 'AX-LOOP-002'));

  eq('12.8 a script loop runs past the old 10k cap',
    run(`^main:\n  n = 0\n  *i in 0..100000:\n    n += 1\n  ^return n\n`).result, 100000);
}

// =========================================================================================
section('13. Diagnostics');
// =========================================================================================
{
  const r = compile(`^main:\n  x = nosuchfunction(1)\n`, { imports: false });
  ok('13.1 an undefined call in ^main is flagged',
    r.diagnostics.some(d => d.error_code === 'AX-UNDEF-FN-001'));

  const r2 = compile(`^main:\n  f = \\x: x + 1\n  ^return f(1)\n`, { imports: false });
  ok('13.2 calling a local closure is NOT flagged',
    !r2.diagnostics.some(d => d.error_code === 'AX-UNDEF-FN-001'), JSON.stringify(r2.diagnostics.map(d => d.message_for_human)));

  const r3 = compile(`^main:\n  ^return sum([1,2])\n`, { imports: false });
  ok('13.3 standard-library calls are NOT flagged',
    !r3.diagnostics.some(d => d.error_code === 'AX-UNDEF-FN-001'));

  const r4 = compile(`^fn f(x):\n  ^return g(x)\n^fn g(x) = x\n^main:\n  ^return f(1)\n`, { imports: false });
  ok('13.4 forward references between functions are fine',
    !r4.diagnostics.some(d => d.error_code === 'AX-UNDEF-FN-001'));

  const r5 = compile(`^main:\n  ^catch e:\n    ^return 1\n`, { imports: false });
  ok('13.5 ^catch without ^try is a parse error',
    r5.diagnostics.some(d => d.error_code === 'AX-PARSE-000'));

  const r6 = compile(`axiom 0.9\n^main:\n  ^return 1\n`, { imports: false });
  ok('13.6 the 0.9 version pragma is recognized',
    !r6.diagnostics.some(d => d.error_code === 'AX-VERSION-001'));

  ok('13.7 recursion without a base case reports a depth error, not a crash',
    (() => {
      try { run(`^fn loop_forever(n) = loop_forever(n + 1)\n^main:\n  ^return loop_forever(0)\n`); return false; }
      catch (e) { return /depth/i.test(e.message); }
    })());
}

// =========================================================================================
section('14. CLI');
// =========================================================================================
{
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'axiom-cli-'));
  const f = path.join(dir, 'p.ax');
  fs.writeFileSync(f, `^main(a):\n  print("argv:", len(a))\n  ^return 3\n`);
  const node = process.execPath;
  const mainJs = path.join(__dirname, 'main.js');

  const out = spawnSync(node, [mainJs, f, '--', 'x', 'y'], { encoding: 'utf8' });
  ok('14.1 a script runs with no renderer present', out.stdout.includes('argv: 2'), out.stdout + out.stderr);
  eq('14.2 ^main return value becomes the exit code', out.status, 3);

  const chk = spawnSync(node, [mainJs, f, '--check'], { encoding: 'utf8' });
  eq('14.3 --check exits 0 on a clean program', chk.status, 0);

  const bad = path.join(dir, 'bad.ax');
  fs.writeFileSync(bad, `^main:\n  ^return (\n`);
  const badChk = spawnSync(node, [mainJs, bad, '--check'], { encoding: 'utf8' });
  eq('14.4 --check exits 1 on a broken program', badChk.status, 1);

  const ev = spawnSync(node, [mainJs, '--eval', '^main:\n  print(sum([1,2,3]))\n'], { encoding: 'utf8' });
  ok('14.5 --eval runs an inline script', ev.stdout.includes('6'), ev.stdout + ev.stderr);

  const help = spawnSync(node, [mainJs, '--help'], { encoding: 'utf8' });
  ok('14.7 --help lists the flags', help.status === 0 && help.stdout.includes('--run') && help.stdout.includes('--sandbox'));

  const ver = spawnSync(node, [mainJs, '--version'], { encoding: 'utf8' });
  ok('14.8 --version prints the version', ver.stdout.trim() === require('./package.json').version, ver.stdout);

  const js = spawnSync(node, [mainJs, f, '--json'], { encoding: 'utf8' });
  ok('14.6 --json emits the main result', /"main_result": 3/.test(js.stdout), js.stdout + js.stderr);

  fs.rmSync(dir, { recursive: true, force: true });
}

// =========================================================================================
section('15. Destructuring, membership, default parameters');
// =========================================================================================
{
  eq('15.1 destructure an array', run(`^main:\n  a, b = [1, 2]\n  ^return a + b\n`).result, 3);
  eq('15.2 destructure a multi-value return', run(`^main:\n  q, r = divmod(17, 5)\n  ^return [q, r]\n`).result, [3, 2]);
  eq('15.3 destructure a record by field name', run(`^main:\n  x, y = {x: 4, y: 5}\n  ^return y - x\n`).result, 1);
  eq('15.4 destructure does not break a plain expression statement',
    run(`^main:\n  xs = []\n  xs.push(1, 2)\n  ^return xs\n`).result, [1, 2]);

  eq('15.5 in on an array', val('3 in [1,2,3]'), true);
  eq('15.6 in on a string', val('"ell" in "hello"'), true);
  eq('15.7 in on dict keys', val('"a" in {a: 1}'), true);
  eq('15.8 in on a range', val('[5 in range(0, 10), 15 in range(0, 10)]'), [true, false]);
  eq('15.9 !in negates', val('9 !in [1,2]'), true);
  eq('15.10 in works inside a condition',
    run(`^main:\n  ?"x" in ["x"]:\n    ^return "yes"\n  ^return "no"\n`).result, 'yes');
  eq('15.11 a for loop over a range still parses with in as an operator',
    run(`^main:\n  n = 0\n  *i in 0..3:\n    n += i\n  ^return n\n`).result, 3);

  eq('15.12 default parameters fill in', val('box(99)', '^fn box(v, lo = 0, hi = 10) = min(max(v, lo), hi)'), 10);
  eq('15.13 defaults are overridable', val('box(5, 0, 3)', '^fn box(v, lo = 0, hi = 10) = min(max(v, lo), hi)'), 3);
  eq('15.14 a default may reference a global',
    run(`~D: 7\n^fn f(x = D) = x\n^main:\n  ^return f()\n`).result, 7);
  eq('15.15 defaults work on a ^proc', run(`^proc tag(name, sep = ":"):\n  ^return name + sep\n^main:\n  ^return tag("a")\n`).result, 'a:');
}

// =========================================================================================
section('16. Examples run (integration)');
// =========================================================================================
{
  // The examples are the language's worked documentation: if one of them stops running, the
  // documentation is wrong, which for an LLM-facing language is as bad as a broken feature.
  const node = process.execPath;
  const mainJs = path.join(__dirname, 'main.js');
  const ex = (name, args) => spawnSync(node, [mainJs, path.join(__dirname, 'examples', name), '--no-restack', ...(args || [])], { encoding: 'utf8' });

  const fb = ex('fizzbuzz.ax');
  ok('16.1 fizzbuzz.ax', fb.status === 0 && fb.stdout.includes('FizzBuzz') && fb.stdout.includes('Fizz\n4'), fb.stdout + fb.stderr);

  const st = ex('stats.ax');
  ok('16.2 stats.ax', st.status === 0 && /median\s+10\.50/.test(st.stdout), st.stdout + st.stderr);

  const wc = ex('wordcount.ax', ['--', path.join(__dirname, 'examples', 'calc.ax'), '3']);
  ok('16.3 wordcount.ax', wc.status === 0 && wc.stdout.trim().split('\n').length === 3, wc.stdout + wc.stderr);

  const lf = ex('life.ax', ['--', '3']);
  ok('16.4 life.ax', lf.status === 0 && (lf.stdout.match(/gen \d/g) || []).length === 3, lf.stderr);

  const ca = ex('calc.ax');
  ok('16.5 calc.ax self-check', ca.status === 0 && ca.stdout.includes('1 + 2 * 3 = 7'), ca.stdout + ca.stderr);

  const ca2 = ex('calc.ax', ['--', '2 * (3 + 4) - 10 / 5']);
  ok('16.6 calc.ax evaluates an expression', ca2.stdout.trim().endsWith('12'), ca2.stdout + ca2.stderr);

  const ca3 = ex('calc.ax', ['--', '1 / 0']);
  ok('16.7 calc.ax reports a caught error', ca3.status === 1 && /division by zero/.test(ca3.stderr), ca3.stdout + ca3.stderr);

  const sim = ex('sim.ax', ['--sim', '400', '--json']);
  ok('16.8 sim.ax steps headless with no renderer', sim.status === 0 && /"tag": "Mouse"/.test(sim.stdout), sim.stderr);
}

// =========================================================================================
section('17. Documentation coverage');
// =========================================================================================
{
  // A standard-library function that is not in STDLIB.md does not exist as far as a model
  // writing AxiomScript is concerned, so drift between the runtime and the reference is a
  // real defect — this test fails the build for it.
  const doc = fs.readFileSync(path.join(__dirname, 'STDLIB.md'), 'utf8');
  const names = Object.keys(new World().intrinsics);
  const undocumented = names.filter(n => !new RegExp(`\\b${n.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')}\\b`).test(doc));
  ok('17.1 every standard-library name is documented in STDLIB.md',
    undocumented.length === 0, `missing: ${undocumented.join(', ')}`);

  const grammar = fs.readFileSync(path.join(__dirname, 'GRAMMAR.md'), 'utf8');
  const newSyntax = ['^main', '^use', '^try', '^catch', '^throw', '?*', 'Lambda', '|>'];
  const ungrammared = newSyntax.filter(t => !grammar.includes(t));
  ok('17.2 GRAMMAR.md covers the v0.9.0 syntax', ungrammared.length === 0, `missing: ${ungrammared.join(', ')}`);

  const readme = fs.readFileSync(path.join(__dirname, 'README.md'), 'utf8');
  ok('17.3 README.md documents the entry point and the script mode',
    readme.includes('^main') && readme.includes('--run'));
}

// =========================================================================================
section('18. Pattern matching with bindings (v0.9.1)');
// =========================================================================================
{
  const tree = `
^type Add: l, r
^type Mul: l, r
^type Num: v
^fn ev(n):
  ?* n:
    Add(a, b): ^return ev(a) + ev(b)
    Mul(a, b): ^return ev(a) * ev(b)
    Num(x): ^return x
    _: ^throw "bad node"
`;
  eq('18.1 record pattern binds its fields',
    run(`${tree}\n^main:\n  ^return ev(Add(Num(2), Mul(Num(3), Num(4))))\n`).result, 14);
  eq('18.2 nested patterns match structurally',
    run(`${tree}\n^main:\n  ?* Add(Num(1), Num(2)):\n    Add(Num(a), Num(b)): ^return a + b\n    _: ^return -1\n`).result, 3);
  eq('18.3 a literal inside a pattern still compares',
    run(`^type P: x, y\n^main:\n  ?* P(1, 9):\n    P(2, y): ^return "no"\n    P(1, y): ^return y\n    _: ^return -1\n`).result, 9);
  eq('18.4 named arguments in a pattern pick fields by name',
    run(`^type P: x, y\n^main:\n  ?* P(4, 5):\n    P(y: b): ^return b\n`).result, 5);
  eq('18.5 array pattern binds elements',
    run(`^main:\n  ?* [1, 2, 3]:\n    [a, b, c]: ^return a * 100 + b * 10 + c\n`).result, 123);
  eq('18.6 array pattern requires the same length',
    run(`^main:\n  ?* [1, 2]:\n    [a, b, c]: ^return "three"\n    [a, b]: ^return "two"\n`).result, 'two');
  eq('18.7 dict pattern binds by key and ignores extra keys',
    run(`^main:\n  ?* {name: "ada", age: 36, city: "london"}:\n    {name, age}: ^return f"{name} {age}"\n`).result, 'ada 36');
  eq('18.8 `_` inside a pattern matches without binding',
    run(`^type P: x, y\n^main:\n  ?* P(7, 8):\n    P(_, b): ^return b\n`).result, 8);
  eq('18.9 a guard sees the pattern bindings',
    run(`^type P: x, y\n^main:\n  ?* P(1, 2):\n    P(a, b) if a > b: ^return "gt"\n    P(a, b) if a < b: ^return "lt"\n    _: ^return "eq"\n`).result, 'lt');
  eq('18.10 a guarded bare name captures the subject',
    run(`^main:\n  ?* 42:\n    n if n > 100: ^return "huge"\n    n if n > 10: ^return n * 2\n    _: ^return 0\n`).result, 84);
  eq('18.11 an unguarded bare name still compares as an atom (v0.8 idiom)',
    run(`^main:\n  s = idle\n  ?* s:\n    running: ^return "go"\n    idle: ^return "wait"\n`).result, 'wait');
  eq('18.12 `_ if` is a guarded catch-all, not the final default',
    run(`^main:\n  ?* 3:\n    _ if 1 > 2: ^return "never"\n    3: ^return "three"\n`).result, 'three');
  eq('18.13 a failed arm leaves no bindings behind',
    run(`^type P: x\n^main:\n  a = "outer"\n  ?* P(1):\n    P(a) if a > 5: ^return "no"\n    _: ^return a\n`).result, 'outer');
  eq('18.14 a type pattern still matches by type alone',
    run(`^type Leaf: v\n^type Node: l, r\n^main:\n  ?* Leaf(1):\n    Node: ^return "node"\n    Leaf: ^return "leaf"\n`).result, 'leaf');
  // Patterns work on the values a real program has in hand, not just on literals.
  eq('18.15 pattern matching drives a recursive walk',
    run(`
^type Leaf: v
^type Pair: a, b
^fn total(n):
  ?* n:
    Pair(x, y): ^return total(x) + total(y)
    Leaf(v): ^return v
    _: ^return 0
^main:
  t = Pair(Leaf(1), Pair(Leaf(2), Pair(Leaf(3), Leaf(4))))
  ^return total(t)
`).result, 10);
}

// =========================================================================================
section('19. The static safety net (v0.9.1)');
// =========================================================================================
{
  const diags = (src) => compile(src, { imports: false }).diagnostics;
  const has = (src, code) => diags(src).some(d => d.error_code === code);

  ok('19.1 too few arguments is reported',
    has(`^fn f(a, b) = a + b\n^main:\n  ^return f(1)\n`, 'AX-ARITY-001'));
  ok('19.2 too many arguments is reported',
    has(`^fn f(a) = a\n^main:\n  ^return f(1, 2)\n`, 'AX-ARITY-001'));
  ok('19.3 a default parameter makes the argument optional',
    !has(`^fn f(a, b = 2) = a + b\n^main:\n  ^return f(1)\n`, 'AX-ARITY-001'));
  ok('19.4 a function that reads args is variadic and not flagged',
    !has(`^fn f(a):\n  ^return len(args)\n^main:\n  ^return f(1, 2, 3)\n`, 'AX-ARITY-001'));
  ok('19.5 arity is checked in action position too',
    has(`^proc p(a, b):\n  print(a)\n@E\n  ~x: 0\n  &tick(10hz):\n    !p(1)\n`, 'AX-ARITY-001'));

  ok('19.6 an unknown field on a record is reported',
    has(`^type P: x, y\n^main:\n  p = P(1, 2)\n  ^return p.z\n`, 'AX-FIELD-001'));
  ok('19.7 a declared field is not reported',
    !has(`^type P: x, y\n^main:\n  p = P(1, 2)\n  ^return p.y\n`, 'AX-FIELD-001'));
  ok('19.8 a reassigned variable is not tracked (no false positive)',
    !has(`^type P: x\n^main:\n  p = P(1)\n  p = {anything: 1}\n  ^return p.anything\n`, 'AX-FIELD-001'));

  ok('19.9 a mistyped variable used in arithmetic is reported',
    has(`^main:\n  health = 100\n  ^return helth - 10\n`, 'AX-UNDEF-VAR-001'));
  ok('19.10 the suggestion names the intended variable',
    diags(`^main:\n  health = 100\n  ^return helth - 10\n`).some(d => d.message_for_human.includes("health")));
  ok('19.11 an atom compared with == is NOT reported',
    !has(`^main:\n  state = idle\n  ?state == idle:\n    ^return 1\n`, 'AX-UNDEF-VAR-001'));
  ok('19.12 an atom passed as an argument is NOT reported',
    !has(`@E\n  ~x: 0\n  &tick(10hz):\n    !play(step)\n`, 'AX-UNDEF-VAR-001'));
  ok('19.13 entity fields, mixin fields and event payloads are known names',
    !has(`^event Hit:\n  damage:: number\n^mix M:\n  ~armor: 1\n@E +M\n  ~hp: 10\n  &on(Hit):\n    hp -= damage * armor\n`, 'AX-UNDEF-VAR-001'));
  ok('19.14 pattern bindings are known names',
    !has(`^type P: x, y\n^main:\n  ?* P(1, 2):\n    P(a, b): ^return a + b\n`, 'AX-UNDEF-VAR-001'));
  ok('19.15 lambda and comprehension variables are known names',
    !has(`^main:\n  ^return [n * 2 for n in range(0, 3)].map(\\v: v - 1)\n`, 'AX-UNDEF-VAR-001'));

  ok('19.16 a literal contradicting a declared field type is reported',
    has(`^type P: x:: number\n^main:\n  ^return P("s").x\n`, 'AX-TYPE-001'));
  ok('19.17 a literal contradicting a declared return type is reported',
    has(`^fn f() -> number = "s"\n^main:\n  ^return f()\n`, 'AX-TYPE-001'));
  ok('19.18 a matching literal is not reported',
    !has(`^type P: x:: number\n^main:\n  ^return P(1).x\n`, 'AX-TYPE-001'));
  ok('19.19 an unannotated field is not reported',
    !has(`^type P: x\n^main:\n  ^return P("s").x\n`, 'AX-TYPE-001'));

  // The safety net must not fire on the shipped examples — they are the corpus that proves it.
  for (const name of ['fizzbuzz', 'stats', 'wordcount', 'life', 'calc', 'sim']) {
    const src = fs.readFileSync(path.join(__dirname, 'examples', `${name}.ax`), 'utf8');
    const ds = compile(src, { filename: path.join(__dirname, 'examples', `${name}.ax`) }).diagnostics;
    ok(`19.20.${name} examples/${name}.ax compiles with no diagnostics at all`,
      ds.length === 0, ds.map(d => `${d.error_code} L${d.location && d.location.line}: ${d.message_for_human}`).join(' | '));
  }
}

// =========================================================================================
section('20. f-string format specs (v0.9.3)');
// =========================================================================================
{
  const f = (body, prelude) => val(`f"${body}"`, prelude);
  eq('20.1 fixed precision', f('{3.14159:.2f}'), '3.14');
  eq('20.2 width and alignment', f('[{42:>5}] [{"ab":<4}] [{"ab":^6}] [{"x":*>3}]'), '[   42] [ab  ] [  ab  ] [**x]');
  eq('20.3 zero padding keeps the sign in front', f('{-3.5:08.2f}'), '-0003.50');
  eq('20.4 thousands separator', f('{1234567.891:,.2f}'), '1,234,567.89');
  eq('20.5 percent', f('{0.256:.1%}'), '25.6%');
  eq('20.6 integer bases', f('{255:x} {255:X} {5:b} {8:o}'), 'ff FF 101 10');
  eq('20.7 explicit sign', f('{3:+d} {-7:=+6}'), '+3 -    7');
  eq('20.8 exponent', f('{1234.5:.2e}'), '1.23e+3');
  eq('20.9 rounds like toFixed (half away from zero on the exact value)', f('{2.5:.0f} {1.005:.2f} {1.25:.1f}'), '3 1.00 1.3');
  eq('20.10 string precision truncates by characters', f('{"naïve":.3}'), 'naï');
  eq('20.11 a ternary colon is not a spec', f('{1 > 0 ? "yes" : "no"}'), 'yes');
  eq('20.12 a spaced colon is not a spec', f('{1 ? 2 :3}'), '2');
  eq('20.13 an empty spec is the plain rendering', f('{7:}'), '7');
  eq('20.14 non-numbers are padded as displayed', f('[{[1, 2]:>7}] [{idle:>5}]'), '[  [1,2]] [ idle]');
  eq('20.15 a spec inside a lambda body', val('[1, 2].map(\\v: f"{v:03d}")'), ['001', '002']);
  ok('20.16 a placeholder with two expressions is a parse error',
    compile(`^main:\n  ^return f"{1 2}"\n`, { imports: false }).diagnostics.some(d => d.severity === 'fatal'));
}

console.log(`\n${passed} passed, ${failed} failed`);
process.exit(failed === 0 ? 0 : 1);
