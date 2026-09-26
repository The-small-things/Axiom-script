// compare.js — pair the two runtimes' sweep outputs by call and list the calls that differ.
//
//   node compare.js js.txt native.txt [--summary] [--expect N]    exit 1 when anything differs,
//                                                   or when either side printed other than N calls
'use strict';
const fs = require('fs');
const argv = process.argv.slice(2);
const [jsFile, cFile] = argv;
const flag = argv.includes('--summary') ? '--summary' : null;
const expect = argv.includes('--expect') ? Number(argv[argv.indexOf('--expect') + 1]) : null;
// Each result starts with `name(args) => ` or `name(args) !! `; a result may run over lines.
function parse(file) {
  const out = new Map();
  let key = null;
  for (const line of fs.readFileSync(file, 'latin1').split('\n')) {
    const m = /^([a-zA-Z_][a-zA-Z0-9_.]*\([^)]*\)) (=>|!!) /.exec(line);
    if (m) { key = m[1]; out.set(key, line.slice(m[1].length + 1)); }
    else if (key && line !== '') out.set(key, out.get(key) + '\n' + line);
  }
  return out;
}
const js = parse(jsFile), c = parse(cFile);
const diffs = [];
for (const [k, v] of js) if (c.get(k) !== v) diffs.push([k, v, c.get(k)]);
for (const k of c.keys()) if (!js.has(k)) diffs.push([k, undefined, c.get(k)]);
if (flag === '--summary') {
  const by = new Map();
  for (const d of diffs) { const f = d[0].split('(')[0]; by.set(f, (by.get(f) || 0) + 1); }
  console.log(`${diffs.length} calls differ in ${by.size} functions (${js.size} calls)`);
  console.log([...by].sort((a, b) => b[1] - a[1]).map(([f, n]) => `${f}:${n}`).join(' '));
} else {
  for (const [k, a, b] of diffs) console.log(`${k}\n  js:     ${a}\n  native: ${b}`);
  console.log(`${diffs.length} of ${js.size} calls differ`);
}
// A runtime that stopped partway (a crash, a hang cut short) prints fewer calls than were made.
if (expect !== null && (js.size !== expect || c.size !== expect)) {
  console.log(`expected ${expect} calls, got ${js.size} (js) and ${c.size} (native)`);
  process.exit(1);
}
process.exit(diffs.length ? 1 : 0);
