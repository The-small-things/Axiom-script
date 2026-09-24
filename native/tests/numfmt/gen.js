// gen.js — doubles and their JavaScript String(x), for numcheck.c.
// usage: node gen.js out.txt N   → lines "<hex bits> <String(x)>"
const fs = require('fs');
const [out, nArg] = process.argv.slice(2);
const N = parseInt(nArg || '100000', 10);
const buf = new DataView(new ArrayBuffer(8));
const lines = [];
let seed = 12345;
const rnd = () => { seed = (seed * 1103515245 + 12345) % 2147483648; return seed / 2147483648; };
const push = (x) => {
  buf.setFloat64(0, x);
  const hex = buf.getBigUint64(0).toString(16).padStart(16, '0');
  lines.push(`${hex} ${String(x)}`);
};
// Boundaries of the notation rules, powers of ten and two, and integers around 2^53.
for (let e = -330; e <= 310; e++) { push(Number(`1e${e}`)); push(Number(`1.5e${e}`)); push(Number(`9.999999999999999e${e}`)); }
for (let e = -1080; e <= 1024; e++) push(Math.pow(2, e));
for (let d = -50; d <= 50; d++) { push(2 ** 53 + d); push(-(2 ** 53) + d); push(2 ** 63 + d * 2048); }
[0.1, 0.2, 0.3, 0.1 + 0.2, 1 / 3, 2 / 3, 5e-324, 1.7976931348623157e308, 123456789012345680000, 1e21, 999999999999999900000,
 0.000001, 0.0000009999999999999999, 1e-7, 4.35, 0.5, -0.5, 1.005, 100, 1e15, 1e16, 1e17, 123e18].forEach(push);
for (let i = 0; i < N; i++) {
  const kind = i % 5;
  if (kind === 0) {                     // any bit pattern (finite ones)
    const hi = Math.floor(rnd() * 0x100000000), lo = Math.floor(rnd() * 0x100000000);
    buf.setUint32(0, hi); buf.setUint32(4, lo);
    const x = buf.getFloat64(0);
    if (Number.isFinite(x)) push(x);
  } else if (kind === 1) push(Math.floor(rnd() * 10 ** Math.floor(rnd() * 22)) * (rnd() < 0.5 ? -1 : 1));   // integers
  else if (kind === 2) push(Number((rnd() * 1000).toFixed(Math.floor(rnd() * 8))));                          // short decimals
  else if (kind === 3) push(rnd() * 10 ** (Math.floor(rnd() * 44) - 22));                                   // any magnitude
  else push(rnd() / (1 + Math.floor(rnd() * 1000)));                                                        // fractions
}
fs.writeFileSync(out, lines.join('\n') + '\n');
