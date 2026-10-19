// gen.js — write N random inputs (small, medium, huge, tiny) and Node's Math results for them.
const fs = require('fs');
const [inPath, outPath, count] = process.argv.slice(2);
const fns = ['sin','cos','tan','asin','acos','atan','exp','log','log2','log10','sinh','cosh','tanh','cbrt','expm1','log1p','atan2','pow'];
let seed = 12345;
const r = () => { seed = (seed * 1103515245 + 12345) % 2147483648; return seed / 2147483648; };
const ins = [], outs = [];
for (let i = 0; i < +count; i++) {
  const m = i % 6;
  const x = (r() * 2 - 1) * (m == 0 ? 1 : m == 1 ? 10 : m == 2 ? 200 : m == 3 ? 1e6 : m == 4 ? 1e-5 : 1e300 * r());
  const y = (r() * 2 - 1) * (i % 4 == 0 ? 60 : 5);
  ins.push(x.toString() + ' ' + y.toString());
  const e = Math.abs(x) > 800 ? x % 800 : x;
  outs.push(fns.map(f => {
    const a = (f == 'asin' || f == 'acos') ? x / 200 : (f == 'exp' || f == 'sinh' || f == 'cosh' || f == 'expm1') ? e : x;
    return (f == 'atan2' ? Math.atan2(x, y) : f == 'pow' ? Math.pow(Math.abs(x) % 50, y) : Math[f](a)).toString();
  }).join(' '));
}
fs.writeFileSync(inPath, ins.join('\n'));
fs.writeFileSync(outPath, outs.join('\n'));
