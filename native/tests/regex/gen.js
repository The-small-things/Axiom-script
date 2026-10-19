// gen.js — random regular expressions for difftest.sh: patterns from a grammar of every
// construct JavaScript has (and malformed variations of them), random flags, and subjects with
// accented letters, Greek, the case-folding oddities (K, ſ, ß), emoji, line terminators and
// lone surrogates — each through re_test, re_match, re_all, re_sub, re_split and .match. Both
// runtimes must print the same result, or the same error, for every call.
//
//   node gen.js OUT.ax [cases] [seed]
'use strict';
const fs = require('fs');

let seed = Number(process.argv[4] || 20261012) >>> 0;
const rnd = () => {   // mulberry32
  seed = (seed + 0x6d2b79f5) >>> 0;
  let t = seed;
  t = Math.imul(t ^ (t >>> 15), t | 1);
  t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
  return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
};
const pick = (xs) => xs[Math.floor(rnd() * xs.length)];
const chance = (p) => rnd() < p;

const LITERALS = ['a', 'b', 'c', 'x', 'A', 'B', 'k', 'K', 's', 'S', '0', '1', '9', '_', ' ', '-', ',', '.', 'é', 'É', 'ß', 'ẞ',
  'Σ', 'σ', 'ς', 'K', 'ſ', 'İ', 'ı', '😀', '😁', '\u{1D400}', 'ǅ', 'Ω', 'ω', 'Ω', '\n', '\t', 'µ', 'Μ', 'μ'];
const ESCAPES = ['\\d', '\\D', '\\w', '\\W', '\\s', '\\S', '\\n', '\\t', '\\r', '\\f', '\\v', '\\0', '\\x41', '\\x6b', '\\u0041',
  '\\u00e9', '\\u{1F600}', '\\u{61}', '\\uD83D', '\\uDE00', '\\uD83D\\uDE00', '\\cA', '\\cj', '\\c1', '\\c', '\\01', '\\07',
  '\\012', '\\1', '\\2', '\\8', '\\9', '\\k<n>', '\\k<m>', '\\k', '\\p{L}', '\\p{Lu}', '\\P{Ll}', '\\p{Letter}', '\\p{sc=Greek}',
  '\\p{Script=Latn}', '\\p{scx=Grek}', '\\p{Emoji}', '\\p{ASCII}', '\\p{Any}', '\\p{RGI_Emoji}', '\\p{Basic_Emoji}',
  '\\p{Nd}', '\\p{gc=Zs}', '\\p{White_Space}', '\\p{Lowercase}', '\\p{Foo}', '\\p', '\\-', '\\/', '\\.', '\\*', '\\$',
  '\\a', '\\e', '\\_', '\\é', '\\q{ab}', '\\b', '\\B', '\\x4', '\\u12', '\\u{}', '\\u{110000}'];
const CLASSES = ['[abc]', '[^abc]', '[a-z]', '[^a-z]', '[A-Z0-9]', '[\\d-x]', '[a-]', '[-a]', '[\\w\\s]', '[^\\W]', '[Σσ]',
  '[😀-😂]', '[\\u0041-\\u005A]', '[\\b]', '[\\cA]', '[\\c1]', '[\\c]', '[]', '[^]', '[z-a]', '[\\p{Lu}]', '[\\P{L}x]',
  '[\\p{L}-z]', '[k-m]', '[éÉ]', '[ſ]', '[\\u212A]', '[.]', '[$^]', '[\\1]', '[\\08]', '[\\k]', '[\\B]', '[a&&b]',
  '[[a-z]--[aeiou]]', '[\\w&&\\d]', '[\\q{abc|d}]', '[\\q{ab|}x]', '[^\\q{ab}]', '[[^a]]', '[\\p{RGI_Emoji}--\\q{😀}]',
  '[a--b--c]', '[a&&&b]', '[&&a]', '[(]', '[a-c&&b]', '[\\d--5]', '[\\-]', '[a\\-z]', '[\\s&&[^\\n]]', '[[a][b]]', '['];

function atom(depth) {
  const r = rnd();
  if (r < 0.32) return pick(LITERALS);
  if (r < 0.52) return pick(ESCAPES);
  if (r < 0.64) return pick(CLASSES);
  if (r < 0.69) return '.';
  if (r < 0.73) return pick(['^', '$']);
  if (depth > 2) return pick(LITERALS);
  const body = disjunction(depth + 1);
  const g = rnd();
  if (g < 0.35) return `(${body})`;
  if (g < 0.5) return `(?:${body})`;
  if (g < 0.62) return `(?<${pick(['n', 'm', 'n', 'é', '$x', '1a', 'n'])}>${body})`;
  if (g < 0.72) return `(?=${body})`;
  if (g < 0.78) return `(?!${body})`;
  if (g < 0.88) return `(?<=${body})`;
  if (g < 0.94) return `(?<!${body})`;
  return pick([`(?${body})`, `(${body}`, `${body})`, `(?<${body})`, `(?i:${body})`]);
}
function quantifier() {
  const q = pick(['*', '+', '?', '{2}', '{0,2}', '{1,}', '{2,1}', '{,2}', '{1', '{x}', '{0}', '{3,5}']);
  return q + (chance(0.3) ? '?' : '');
}
function term(depth) {
  let a = atom(depth);
  if (chance(0.35)) a += quantifier();
  if (chance(0.03)) a += quantifier();
  return a;
}
function alternative(depth) {
  let s = '';
  const n = 1 + Math.floor(rnd() * (depth ? 3 : 4));
  for (let i = 0; i < n; i++) s += term(depth);
  return s;
}
function disjunction(depth) {
  let s = alternative(depth);
  while (chance(0.2)) s += '|' + alternative(depth);
  return s;
}
function flags() {
  let f = '';
  for (const c of ['g', 'i', 'm', 's', 'y', 'd']) if (chance(c === 'i' ? 0.4 : 0.2)) f += c;
  const u = rnd();
  if (u < 0.3) f += 'u';
  else if (u < 0.45) f += 'v';
  else if (u < 0.47) f += 'uv';
  if (chance(0.02)) f += pick(['x', 'gg', 'U']);
  return f;
}
const SUBJECT_CHARS = ['a', 'b', 'c', 'x', 'A', 'B', 'k', 'K', 's', 'S', '0', '1', '9', '_', ' ', '-', '.', 'é', 'É', 'ß',
  'Σ', 'σ', 'ς', 'K', 'ſ', 'İ', 'i', 'ı', '😀', '😁', '😂', '\u{1D400}', '\n', '\r', ' ', ' ', '\t',
  '\uD83D', '\uDE00', 'ab', 'abc', 'aba', 'Ω', 'Ω', 'µ', 'μ', '🇫🇷', '👍🏽', '#️⃣', '1️⃣'];
function subject() {
  let s = '';
  const n = Math.floor(rnd() * 10);
  for (let i = 0; i < n; i++) s += pick(SUBJECT_CHARS);
  return s;
}
const REPLACEMENTS = ['-', '[$&]', '<$1>', '$2$1', '$$', "$'", '$`', '$<n>', '$<m>|$<', '$0', '$01', '$10', '$9', '', 'é😀'];

// An AxiomScript expression for a JavaScript string: literal text where the lexer allows it,
// chr() for what a source file cannot hold (lone surrogates, control characters).
function lit(s) {
  const parts = [];
  let cur = '';
  for (let i = 0; i < s.length; i++) {
    const c = s.charCodeAt(i);
    const isPair = c >= 0xd800 && c <= 0xdbff && i + 1 < s.length && s.charCodeAt(i + 1) >= 0xdc00 && s.charCodeAt(i + 1) <= 0xdfff;
    if (isPair) { cur += s[i] + s[i + 1]; i++; continue; }
    if ((c >= 0xd800 && c <= 0xdfff) || (c < 0x20 && c !== 10 && c !== 9 && c !== 13) || c === 0x2028 || c === 0x2029 || c === 0x7f) {
      parts.push(JSON.stringify(cur).slice(1, -1));
      parts.push(`" + chr(${c}) + "`);
      cur = '';
      continue;
    }
    cur += s[i];
  }
  parts.push(JSON.stringify(cur).slice(1, -1));
  // JSON's escapes are AxiomScript's: \\ \" \n \t \r (it writes \uXXXX only for the characters
  // split out above).
  return '"' + parts.join('') + '"';
}

const cases = Number(process.argv[3] || 2500);
const out = [
  '^fn t(label, f):',
  '  ^try:',
  '    print(label, "=>", f())',
  '  ^catch e:',
  '    print(label, "!!", e.code, e.msg)',
  '',
  '^main:',
];
// Malformed variations: characters of the syntax inserted, deleted or swapped at random.
const SYNTAX = '()[]{}|\\^$*+?.-,<>=!:kpPqux&0123456789';
function mutate(p) {
  const n = 1 + Math.floor(rnd() * 3);
  for (let i = 0; i < n; i++) {
    const at = Math.floor(rnd() * (p.length + 1));
    const r = rnd();
    if (r < 0.45) p = p.slice(0, at) + pick([...SYNTAX]) + p.slice(at);
    else if (r < 0.8) p = p.slice(0, at) + p.slice(at + 1);
    else p = p.slice(0, at) + pick([...SYNTAX]) + p.slice(at + 1);
  }
  return p;
}
for (let k = 0; k < cases; k++) {
  let p = disjunction(0);
  if (chance(0.3)) p = mutate(p);
  const f = flags();
  const s = subject();
  const P = lit(p), F = lit(f), S = lit(s);
  out.push(`  p = ${P}`, `  f = ${F}`, `  s = ${S}`);
  out.push(`  t("c${k}(test)", \\: re_test(s, p, f))`);
  out.push(`  t("c${k}(match)", \\: re_match(s, p, f))`);
  out.push(`  t("c${k}(all)", \\: re_all(s, p, f))`);
  out.push(`  t("c${k}(sub)", \\: re_sub(s, p, ${lit(pick(REPLACEMENTS))}, f))`);
  out.push(`  t("c${k}(split)", \\: re_split(s, p, f))`);
  if (k % 4 === 0) out.push(`  t("c${k}(method)", \\: s.match(p))`);
}
fs.writeFileSync(process.argv[2], out.join('\n') + '\n');
