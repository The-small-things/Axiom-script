// mutate.js — deterministic line-level mutants of the .ax corpus, for comparing the checkers:
// deleted, duplicated, re-indented and swapped lines, and renamed identifiers.
// usage: node mutate.js OUTDIR N FILE...
const fs = require('fs');
const path = require('path');
const [out, nArg, ...files] = process.argv.slice(2);
let s = 99;
const rand = () => { s = (s * 1103515245 + 12345) % 2147483648; return s / 2147483648; };
const pick = (xs) => xs[Math.floor(rand() * xs.length)];
for (let k = 0; k < parseInt(nArg, 10); k++) {
  const lines = fs.readFileSync(pick(files), 'utf8').split('\n');
  for (let e = 1 + Math.floor(rand() * 2); e > 0 && lines.length > 1; e--) {
    const i = Math.floor(rand() * lines.length), r = rand();
    if (r < 0.3) lines.splice(i, 1);
    else if (r < 0.5) lines.splice(i, 0, pick(lines));
    else if (r < 0.65) lines[i] = '  ' + lines[i];
    else if (r < 0.8) lines[i] = lines[i].startsWith('  ') ? lines[i].slice(2) : lines[i];
    else if (r < 0.9) { const j = Math.floor(rand() * lines.length); [lines[i], lines[j]] = [lines[j], lines[i]]; }
    else {
      const words = lines[i].split(/[^A-Za-z0-9_]+/).filter(w => /^[A-Za-z_]\w*$/.test(w));
      if (words.length) { const w = pick(words); lines[i] = lines[i].replace(w, w.slice(0, -1) + 'q'); }
    }
  }
  fs.writeFileSync(path.join(out, `m${String(k).padStart(4, '0')}.ax`), lines.join('\n'));
}
