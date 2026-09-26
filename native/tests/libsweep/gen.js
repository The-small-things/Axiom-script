// gen.js — every library function called with every argument shape, for difftest.sh: both
// runtimes must print the same result, or raise the same error, for each call — including the
// calls a program should never make (missing arguments, a dict where text is expected), because
// a model learns the language from what those calls do.
//
//   node gen.js OUT.ax
'use strict';
const fs = require('fs');
const path = require('path');
const src = fs.readFileSync(path.join(__dirname, '..', '..', 'checknames.h'), 'utf8');
const names = [...src.match(/CHECK_KNOWN_NAMES\[\] = \{([\s\S]*?)\};/)[1].matchAll(/"([^"]+)"/g)].map((m) => m[1]);
// Side effects, the clock, randomness and the terminal: nothing to compare.
const SKIP = new Set(['exit', 'sleep', 'sh', 'rm', 'mkdir', 'write', 'append', 'write_json', 'input', 'read_stdin',
  'print', 'eprint', 'clock', 'now', 'time', 'date_iso', 'random', 'randomRange', 'randomInt', 'random_int',
  'random_range', 'rand', 'shuffle', 'sample', 'choice', 'pick', 'seed', 'gauss', 'uuid', 'ls', 'read', 'read_lines',
  'read_json', 'env', 'args', 'noise', 'timestamp', 'today', 'perf_now', 'time_ms', 'uid']);
const SHAPES = ['N', 'R', 'S', 'A', 'E', 'D', 'Z', 'T', 'V', 'F', 'B'];
let out = [
  '^fn t(label, f):',
  '  ^try:',
  '    print(label, "=>", f())',
  '  ^catch e:',
  '    print(label, "!!", e.code, e.msg)',
  '',
  '^main:',
  '  N = 5',
  '  R = -2.5',
  '  S = "ab c"',
  '  A = [5, 2]',
  '  E = []',
  '  D = {a: 1}',
  '  Z = null',
  '  T = true',
  '  V = v3(1, 2, 3)',
  '  F = \\x: x',
  '  B = big(5)',
];
const seen = new Set();   // `A, F` comes from two of the shape loops
const call = (n, args) => { if (!seen.has(`${n}(${args})`)) { seen.add(`${n}(${args})`); out.push(`  t("${n}(${args})", \\: ${n}(${args}))`); } };
for (const n of names) {
  if (SKIP.has(n) || !/^[a-z_][a-z0-9_]*$/i.test(n)) continue;
  call(n, '');
  for (const s of SHAPES) call(n, s);
  for (const s of SHAPES) { call(n, `${s}, 2`); call(n, `2, ${s}`); call(n, `${s}, F`); call(n, `A, ${s}`); }
  call(n, 'A, 2, 3');
  call(n, 'S, S, S');
}
// Methods, through each runtime's own method dispatch (and, for a name the receiver type does
// not have, uniform call syntax). A receiver is written fresh for every call, because methods
// like push and set change it.
const METHODS = ['all', 'any', 'capitalize', 'charAt', 'charCodeAt', 'chars', 'clear', 'clone', 'concat', 'contains',
  'count', 'delete', 'each', 'endsWith', 'entries', 'every', 'fill', 'filter', 'find', 'findIndex', 'find_index', 'first',
  'flat', 'flatMap', 'flat_map', 'forEach', 'get', 'group_by', 'has', 'includes', 'indexOf', 'is_empty', 'items', 'join',
  'keys', 'last', 'lastIndexOf', 'len', 'lines', 'lower', 'map', 'map_values', 'match', 'max', 'merge', 'min', 'padEnd',
  'padStart', 'pop', 'push', 'reduce', 'reject', 'repeat', 'replace', 'replace_all', 'reverse', 'set', 'shift', 'slice',
  'some', 'sort', 'sort_by', 'splice', 'split', 'startsWith', 'sum', 'to_int', 'to_num', 'trim', 'trim_end', 'trim_start',
  'uniq', 'unshift', 'upper', 'values', 'words'];
const RECEIVERS = { N: '(5)', R: '(-2.5)', S: '"ab c"', A: '[5, 2]', E: '[]', D: '{a: 1}', T: '(true)', V: 'v3(1, 2, 3)', B: 'big(5)' };
const MARGS = ['', '2', 'F', 'S', '2, 3', 'F, 2', 'A', '"a", 2'];
for (const m of METHODS) {
  for (const [label, recv] of Object.entries(RECEIVERS)) {
    for (const a of MARGS) out.push(`  t("${label}.${m}(${a.replace(/"/g, "'")})", \\: ${recv}.${m}(${a}))`);
  }
}
// Field-name selectors over mixed elements: a dict's own entry, a vector's member, and null for
// everything else (a string's length or an array's index is not a field).
out.push('  P = [{hp: 3, n: "x"}, {n: "y"}, v3(3, 0, 4), v2(1, 2), "abc", [1, 2], null, 7, (true)]');
out.push('  PD = {a: P[0], b: P[2], c: "abc"}');
const SELECTOR_FNS = ['map', 'filter', 'sort_by', 'group_by', 'count_by', 'min_by', 'max_by', 'sum', 'uniq', 'partition',
  'find', 'find_index', 'any', 'all', 'mean', 'prod'];
const SELECTOR_METHODS = ['map', 'filter', 'reject', 'sort_by', 'group_by', 'min', 'max', 'sum', 'uniq', 'find',
  'find_index', 'findIndex', 'every', 'some', 'count', 'sort'];
const FIELDS = ['hp', 'n', 'x', 'z', 'mag', 'norm', 'xy', 'length', 'len', '0', '__type', 'missing'];
for (const f of FIELDS) {
  for (const n of SELECTOR_FNS) out.push(`  t("${n}(P, '${f}')", \\: ${n}(P, "${f}"))`);
  for (const m of SELECTOR_METHODS) out.push(`  t("P.${m}('${f}')", \\: P.${m}("${f}"))`);
  out.push(`  t("PD.map_values('${f}')", \\: PD.map_values("${f}"))`);
}
// Orderings that are not consistent (a boolean against numbers and text, a comparator that is not
// a valid ordering) come out however the sort algorithm meets them, so both runtimes sort with
// the same algorithm; long arrays reach its galloping merges.
out.push('  M = [true, 2, false, 1, "a", 0, "", null, [1], {a: 1}, -1, v2(1, 2), false, "b", 3, true]');
out.push('  L = map(range(400), \\i: [true, false, (i * 37) % 101, str(i % 13), null, i % 7 == 0][i % 6])');
for (const [label, expr] of [['sorted(M)', 'sorted(M)'], ['sort_by(M, F)', 'sort_by(M, F)'], ['M.sort()', 'M.clone().sort()'],
  ['min_by(M, F)', 'min_by(M, F)'], ['max_by(M, F)', 'max_by(M, F)'], ['M.min()', 'M.min()'], ['M.max()', 'M.max()'],
  ['sorted(L)', 'sorted(L)'], ['sort_by(L, F)', 'sort_by(L, F)'], ['L.sort()', 'L.clone().sort()'],
  ['sorted(L, cmp)', 'sorted(map(range(500), \\i: (i * 7919) % 1009), \\a, b: (a % 10) - (b % 7))'],
  ['L.sort(cmp)', 'map(range(300), \\i: (i * 31) % 97).sort(\\a, b: (a % 3) - (b % 7))'],
  ['sorted(L, bool)', 'sorted(map(range(50), \\i: (i * 31) % 97), \\a, b: a > b)'],
  // A comparator's result is read as a number, whatever it returns.
  ['A.sort(text)', '[3, 1, 2].sort(\\a, b: str(a - b))'], ['sorted(A, text)', 'sorted([3, 1, 2], \\a, b: str(a - b))'],
  ['A.sort(array)', '[3, 1, 2].sort(\\a, b: [a - b])'], ['sorted(A, null)', 'sorted([3, 1, 2], \\a, b: null)']]) {
  out.push(`  t("${label}", \\: ${expr})`);
}
fs.writeFileSync(process.argv[2], out.join('\n') + '\n');
