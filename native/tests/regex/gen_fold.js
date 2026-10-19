// gen_fold.js — case-insensitive matching across Unicode, for difftest.sh: every character
// that has a case partner, and random ranges of them, as a pattern under i, iu and iv, run
// over a subject holding all of those characters; both runtimes must find the same matches.
//
//   node gen_fold.js OUT.ax [ranges] [seed]
'use strict';
const fs = require('fs');

let seed = Number(process.argv[4] || 7) >>> 0;
const rnd = () => {
  seed = (seed + 0x6d2b79f5) >>> 0;
  let t = seed;
  t = Math.imul(t ^ (t >>> 15), t | 1);
  t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
  return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
};

// The characters with a case partner (or a case mapping) anywhere in Unicode.
const cased = [];
for (let c = 0; c < 0x110000; c++) {
  if (c >= 0xd800 && c <= 0xdfff) continue;
  const s = String.fromCodePoint(c);
  if (s.toUpperCase() !== s || s.toLowerCase() !== s || /\p{CWCF}/u.test(s)) cased.push(c);
}
const subject = cased.map((c) => String.fromCodePoint(c)).join('');
const esc = (c, uni) => (c < 0x10000 ? `\\u${c.toString(16).padStart(4, '0')}` : uni ? `\\u{${c.toString(16)}}` : String.fromCodePoint(c));

const out = ['^main:'];
// The subject as literal text, in pieces the lexer takes comfortably.
out.push('  parts = []');
for (let i = 0; i < subject.length; i += 400) {
  let piece = subject.slice(i, i + 400);
  // Keep surrogate pairs whole across the cut.
  if (/[\ud800-\udbff]$/.test(piece)) { piece = subject.slice(i, i + 401); i++; }
  out.push(`  parts.push(${JSON.stringify(piece).replace(/\\u([0-9a-f]{4})/g, (_, h) => `" + chr(${parseInt(h, 16)}) + "`)})`);
}
out.push('  s = parts.join("")');
out.push('  print(len(s))');
out.push('^fn found(s, p, f) = hash(to_json(map(re_all(s, p, f), \\m: m.index)))');
const FLAGS = ['i', 'iu', 'iv'];
let n = 0;
for (const c of cased) {
  for (const f of FLAGS) {
    if (c >= 0x10000 && f === 'i') continue;
    out.push(`  print("c${n++}", found(s, ${JSON.stringify(esc(c, f !== 'i'))}, "${f}"))`);
  }
}
const ranges = Number(process.argv[3] || 600);
for (let k = 0; k < ranges; k++) {
  const f = FLAGS[Math.floor(rnd() * 3)];
  let lo = cased[Math.floor(rnd() * cased.length)];
  if (f === 'i' && lo >= 0x10000) lo = cased[Math.floor(rnd() * 900)];
  const hi = Math.min(lo + Math.floor(rnd() * (rnd() < 0.3 ? 2000 : 60)), f === 'i' ? 0xffff : 0x10ffff);
  const neg = rnd() < 0.3 ? '^' : '';
  const p = `[${neg}${esc(lo, f !== 'i')}-${esc(hi, f !== 'i')}]`;
  out.push(`  print("r${k}", found(s, ${JSON.stringify(p)}, "${f}"))`);
}
// Turn the lines into the shape the main block needs: found() is declared after ^main.
const main = out.filter((l) => !l.startsWith('^fn'));
const fn = out.filter((l) => l.startsWith('^fn'));
fs.writeFileSync(process.argv[2], main.join('\n') + '\n\n' + fn.join('\n') + '\n');
