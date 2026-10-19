// gen.js — random key streams for difftest.sh: escape sequences (complete, cut off, unknown),
// control bytes, UTF-8 (well-formed or not) and printable characters, written as DIR/0.in …
// Both runtimes must decode each into the same key names.
//
//   node gen.js DIR [count] [seed]
'use strict';
const fs = require('fs');
const path = require('path');

let seed = Number(process.argv[4] || 20261014) >>> 0;
const rnd = () => {   // mulberry32
  seed = (seed + 0x6d2b79f5) >>> 0;
  let t = seed;
  t = Math.imul(t ^ (t >>> 15), t | 1);
  t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
  return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
};
const SEQ = '[O~;0123456789ABCDHF';
const count = Number(process.argv[3] || 40);
for (let k = 0; k < count; k++) {
  const bytes = [];
  const n = Math.floor(rnd() * 600);
  for (let i = 0; i < n; i++) {
    const r = rnd();
    if (r < 0.15) bytes.push(0x1b);
    else if (r < 0.25) bytes.push(SEQ.charCodeAt(Math.floor(rnd() * SEQ.length)));
    else if (r < 0.4) bytes.push(Math.floor(rnd() * 32));
    else if (r < 0.6) bytes.push(0x80 + Math.floor(rnd() * 128));
    else bytes.push(32 + Math.floor(rnd() * 96));
  }
  fs.writeFileSync(path.join(process.argv[2], `${k}.in`), Buffer.from(bytes));
}
