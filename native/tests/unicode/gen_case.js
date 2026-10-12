// gen_case.js — every code point through upper() and lower() (and len and hash of the result),
// in blocks of 256, for difftest.sh: the native case tables and their lookup must give exactly
// what String.prototype.toUpperCase/toLowerCase give. Then final sigma in the contexts that
// decide it.
//
//   node gen_case.js OUT.ax
'use strict';
const fs = require('fs');
// Characters a string literal cannot hold as they are: quotes, backslashes, braces (f-string
// syntax elsewhere), line breaks and other controls, and surrogates (UTF-8 cannot encode them).
const skip = (c) => c < 0x20 || c === 0x22 || c === 0x5c || c === 0x7b || c === 0x7d || c === 0x7f ||
  (c >= 0x80 && c < 0xa0) || c === 0x2028 || c === 0x2029 || (c >= 0xd800 && c <= 0xdfff);
const out = ['^main:'];
for (let base = 0; base < 0x110000; base += 256) {
  let s = '';
  for (let c = base; c < base + 256; c++) if (!skip(c)) s += String.fromCodePoint(c);
  out.push(`  b = "${s}"`);
  out.push(`  u = b.upper()`, `  l = b.lower()`);
  out.push(`  print("${base.toString(16)}", len(u), hash(u), len(l), hash(l))`);
}
// Σ is final after a cased letter (past case-ignorable ones) when no cased letter follows.
const sigma = ['Σ', 'ΑΣ', 'ΑΣ.', 'ΑΣΑ', "Α'Σ", "ΑΣ'Α", 'Α.Σ Σ', '1Σ', 'ΑΣ́', 'ΆΣ', 'ΣΣΣ', 'ΑΣΣ', 'aΣb Σa aΣ',
  'ΑΣ😀', '😀Σ', 'ΣΑΣ. Σ', 'ΑΣ­', 'ΑΣ­Α', 'ἈΣ', 'ΩΣ ΣΩ', 'ǅΣ', 'ⅰΣ', 'ªΣ', 'Σª'];
for (const s of sigma) out.push(`  print("${s}".lower(), lower_all(["${s}"]))`);
out.push('', '^fn lower_all(xs) = map(xs, \\s: s.lower() + "|" + title(s) + "|" + capitalize(s))');
fs.writeFileSync(process.argv[2], out.join('\n') + '\n');
