// wasmtest.mjs — the WebAssembly build against the native one.
//
//   node tests/api/wasmtest.mjs            (after `make wasm`; run from native/)
//
// 1. The JavaScript API (wasm/axiom.mjs) does what apitest.c checks of the C API.
// 2. Every script in tests/ prints the same and exits the same through the API as through the
//    native command (programs that touch files are left out: the library build has none).
// 3. Every engine test steps to the same state (`load`, `main`, `step`, `state` against
//    `--sim N --json`).
// 4. The command build (axiom.wasm under node:wasi) matches the native command, files included:
//    the scripts, every engine test's `--sim --json`, `--check --json` on the checker corpus,
//    and the REPL transcripts.
import { loadAxiom } from '../../wasm/axiom.mjs';
import { execFileSync, spawnSync } from 'node:child_process';
import { readFileSync, readdirSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = dirname(fileURLToPath(import.meta.url));
const NATIVE_DIR = join(HERE, '..', '..');
const NATIVE = join(NATIVE_DIR, 'axiom');
process.chdir(NATIVE_DIR);   // engine tests name their .nav files relative to native/

let pass = 0, fail = 0;
function expect(ok, what, detail) {
  if (ok) { pass++; return; }
  fail++;
  console.log(`FAIL ${what}`);
  if (detail) console.log(String(detail).split('\n').slice(0, 12).map((l) => '  ' + l).join('\n'));
}

function native(args) {
  const r = spawnSync(NATIVE, args, { encoding: 'utf8' });
  return { out: r.stdout, code: r.status };
}

const axiom = await loadAxiom();

// ---- 1. the API ----------------------------------------------------------------------------
{
  const out = [];
  const ax = axiom.create({ onOutput: (s, t) => out.push([s, t]) });
  let calls = 0;
  ax.define('js_sum', (...xs) => { calls++; return { total: xs.flat().reduce((a, b) => a + b, 0) }; });
  ax.define('js_fail', () => { throw Object.assign(new Error('the host said no'), { code: 'E_HOST' }); });
  const code = ax.run([
    '^main:',
    '  print(js_sum(1, 2, 3.5).total, [4, 5].js_sum().total)',
    '  eprint("err")',
    '  ^try:',
    '    js_fail()',
    '  ^catch e:',
    '    print("caught", e.code, e.msg)',
    '  exit(3)',
    '  print("never")',
  ].join('\n') + '\n', 'host.ax');
  expect(code === 3 && ax.exitCode === 3, 'api: exit(3) returns 3', code);
  expect(JSON.stringify(out) === JSON.stringify([[1, '6.5 9\n'], [2, 'err\n'], [1, 'caught E_HOST the host said no\n']]), 'api: output streams, host calls, host errors', JSON.stringify(out));
  expect(calls === 2, 'api: host called twice');
  const res = ax.result();
  expect(res.exit_code === 3 && res.main_result === null, 'api: result()', JSON.stringify(res));
  ax.free();

  const bad = axiom.create({ onOutput: () => {} });
  expect(bad.run('^main:\n  x = null\n  print(x.y)\n', 'bad.ax') === 1 && /^AX-/.test(bad.error), 'api: runtime error → 1 and error()', bad.error);
  bad.free();

  const broken = axiom.create({ onOutput: () => {} });
  expect(!broken.load('^main:\n  print((\n') && /AX-PARSE-000/.test(broken.error), 'api: load() refuses a program that does not compile');
  broken.free();

  const r = axiom.create({ onOutput: () => {} });
  expect(r.eval('x = 20').value === null, 'api: eval statement');
  expect(r.eval('x * 2 + 2').value === '42', 'api: eval expression');
  expect(r.eval('^fn sq(n) = n * n\nsq(x)\n"s"').value === '400\n"s"', 'api: eval declarations and several values');
  expect(r.eval('nope(1)').errors === 1 && r.eval('x').value === '20', 'api: eval error, session survives');
  r.free();

  const w = axiom.create({ onOutput: () => {} });
  expect(w.load('@M\n  ~x: 0\n  ~j: 0\n  &physics:\n    x += input.move.x\n    ?input.jump: j += 1\n'), 'api: load a world');
  w.setInput({ x: 1, jump: true });
  w.step(30);
  w.setInput({});
  w.step(30);
  const st = w.state();
  expect(st.frames_run === 60 && st.entities[0].fields.x === 30 && st.entities[0].fields.j === 30, 'api: input and stepping', JSON.stringify(st.entities[0]));
  const px = w.render(64, 48);
  expect(px && px.length === 64 * 48 * 4, 'api: render');
  w.free();

  const sb = axiom.create({ onOutput: (s, t) => out.push(t) });
  out.length = 0;
  sb.run('^main:\n  ^try:\n    read("x.txt")\n  ^catch e:\n    print(e.code)\n  ^try:\n    sh("ls")\n  ^catch e:\n    print(e.code)\n');
  expect(out.join('') === 'AX-SANDBOX-001\nAX-SANDBOX-001\n', 'api: sandboxed by default', out.join(''));
  sb.free();

  const args = axiom.create({ onOutput: (s, t) => out.push(t), args: ['one', 'two'] });
  out.length = 0;
  args.run('^main(a):\n  print(a, args())\n');
  expect(out.join('') === '[one,two] [one,two]\n', 'api: args', out.join(''));
  args.free();

  expect(axiom.check('^main:\n  print(1)\n').ok === true && axiom.check('^main:\n  print((\n').ok === false, 'api: check');

  let ok = true;
  for (let i = 0; i < 200 && ok; i++) {
    const t = [];
    const a = axiom.create({ onOutput: (s, x) => t.push(x) });
    ok = a.run('^fn f(n) = n * 2\n^main:\n  print([1, 2, 3].map(f).sum())\n') === 0 && t.join('') === '12\n';
    a.free();
  }
  expect(ok, 'api: 200 instances created, run and freed');
}

// ---- 2. scripts through the API ------------------------------------------------------------
const USES_FILES = /\b(read|write|append|read_lines|read_json|write_json|ls|mkdir|rm|file_exists|exists|is_dir|input|read_stdin|env)\s*\(|\^use\b/;
const scripts = readdirSync('tests').filter((f) => f.endsWith('.ax')).map((f) => join('tests', f))
  .concat(readdirSync('tests/json').filter((f) => f.endsWith('.ax')).map((f) => join('tests/json', f)));
let nscripts = 0;
for (const f of scripts) {
  const src = readFileSync(f, 'utf8');
  if (USES_FILES.test(src)) continue;
  nscripts++;
  const out = [];
  const ax = axiom.create({ onOutput: (s, t) => { if (s === 1) out.push(t); } });
  const code = ax.run(src, f);
  ax.free();
  const n = native(f.includes('json') ? [f, '--run'] : [f]);
  expect(out.join('') === n.out && code === n.code, `script ${f} (wasm exit ${code}, native ${n.code})`, diff(n.out, out.join('')));
}

// ---- 3. worlds through the API -------------------------------------------------------------
const FRAMES = 120;
let nworlds = 0;
for (const f of readdirSync('tests/engine').filter((x) => x.endsWith('.ax')).map((x) => join('tests/engine', x))) {
  const src = readFileSync(f, 'utf8');
  if (USES_FILES.test(src) || /\.(glb|nav)\b/.test(src) || /!save|!load/.test(src)) continue;
  nworlds++;
  const ax = axiom.create({ onOutput: () => {}, sandbox: false });
  const loaded = ax.load(src, f);
  if (loaded) ax.main();
  if (!ax.exitCode) ax.step(FRAMES);
  const st = loaded ? ax.state() : null;
  ax.free();
  const n = native([f, '--sim', String(FRAMES), '--json']);
  let same = false;
  try { same = JSON.stringify(st) === JSON.stringify(JSON.parse(n.out)); } catch { same = false; }
  expect(same, `world ${f} after ${FRAMES} frames`, diff(n.out, JSON.stringify(st, null, 2)));
}

// ---- 4. the command build ------------------------------------------------------------------
let ncommand = 0;
for (const f of scripts) {
  ncommand++;
  const extra = f.includes('json') ? ['--run'] : [];
  const w = spawnSync(process.execPath, [join('wasm', 'axiom-wasi.js'), f, ...extra], { encoding: 'utf8' });
  const n = native([f, ...extra]);
  expect(w.stdout === n.out && w.status === n.code, `command ${f} (wasm exit ${w.status}, native ${n.code})`, diff(n.out, w.stdout) + w.stderr);
}

const wasmCmd = (args, input) => spawnSync(process.execPath, [join('wasm', 'axiom-wasi.js'), ...args], { encoding: 'utf8', input });
for (const f of readdirSync('tests/engine').filter((x) => x.endsWith('.ax')).map((x) => join('tests/engine', x))) {
  ncommand++;
  const args = [f, '--sim', String(FRAMES), '--json'];
  const w = wasmCmd(args), n = native(args);
  expect(w.stdout === n.out && w.status === n.code, `command ${f} --sim ${FRAMES} --json`, diff(n.out, w.stdout) + w.stderr);
}
for (const f of readdirSync('tests/check').filter((x) => x.endsWith('.ax')).map((x) => join('tests/check', x))) {
  ncommand++;
  const w = wasmCmd([f, '--check', '--json']), n = native([f, '--check', '--json']);
  expect(w.stdout === n.out && w.status === n.code, `command ${f} --check --json`, diff(n.out, w.stdout) + w.stderr);
}
for (const f of readdirSync('tests/repl').filter((x) => x.endsWith('.txt')).map((x) => join('tests/repl', x))) {
  ncommand++;
  const input = readFileSync(f, 'utf8');
  const w = wasmCmd(['--repl'], input);
  const n = spawnSync(NATIVE, ['--repl'], { encoding: 'utf8', input });
  expect(w.stdout === n.stdout && w.status === n.status, `command --repl < ${f}`, diff(n.stdout, w.stdout) + w.stderr);
}

console.log(`${pass} passed, ${fail} failed (API checks; ${nscripts} scripts and ${nworlds} worlds through the API; ${ncommand} programs through the command build)`);
process.exit(fail ? 1 : 0);

function diff(a, b) {
  const x = (a ?? '').split('\n'), y = (b ?? '').split('\n');
  for (let i = 0; i < Math.max(x.length, y.length); i++) {
    if (x[i] !== y[i]) return `line ${i + 1}:\n  native: ${x[i]}\n  wasm:   ${y[i]}`;
  }
  return '';
}
