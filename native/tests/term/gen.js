// gen.js — render deterministic pixel buffers with terminal.js; termcheck.c renders the same
// buffers natively and compares byte for byte.
const fs = require('fs');
const path = require('path');
const { renderToTerminal } = require(path.resolve(__dirname, '../../../terminal.js'));
const dir = process.argv[2];
let seed = 99;
const r = () => { seed = (seed * 1103515245 + 12345) % 2147483648; return seed / 2147483648; };
const cases = [];
const sizes = [[160, 120], [64, 48], [81, 37]];
const opts = [
  { mode: 'unicode', color: true }, { mode: 'unicode', color: false }, { mode: 'ascii', color: false },
  { mode: 'ascii', color: true }, { mode: 'unicode', color: true, subpixel: true },
];
let n = 0;
for (const [w, h] of sizes) {
  const px = new Uint8Array(w * h * 4);
  // Blocks of flat colour (so runs merge), gradients, and transparent holes.
  for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) {
    const i = (y * w + x) * 4;
    const block = ((x >> 3) + (y >> 3)) % 4;
    if (block === 0) { px[i] = 30; px[i + 1] = 120; px[i + 2] = 200; }
    else if (block === 1) { px[i] = x * 3 % 256; px[i + 1] = y * 5 % 256; px[i + 2] = 90; }
    else if (block === 2) { px[i] = Math.floor(r() * 256); px[i + 1] = Math.floor(r() * 256); px[i + 2] = Math.floor(r() * 256); }
    else { px[i] = 250; px[i + 1] = 250; px[i + 2] = 250; }
    px[i + 3] = (x * 7 + y * 3) % 23 === 0 ? 0 : 255;
  }
  fs.writeFileSync(path.join(dir, `px${n}.rgba`), Buffer.from(px));
  for (const o of opts) for (const tw of [80, 40]) {
    const full = Object.assign({ termWidth: tw, termHeight: 30, isTTY: tw === 80 }, o);
    const out = renderToTerminal(px, w, h, full);
    const k = cases.length;
    fs.writeFileSync(path.join(dir, `out${k}.txt`), out);
    cases.push(`${n} ${w} ${h} ${o.mode === 'unicode' ? 1 : 0} ${o.color ? 1 : 0} ${o.subpixel ? 1 : 0} ${tw} 30 ${tw === 80 ? 1 : 0}`);
  }
  n++;
}
fs.writeFileSync(path.join(dir, 'cases.txt'), cases.join('\n') + '\n');
