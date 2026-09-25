// gen.js — a seeded program of big-integer arithmetic for difftest.sh: operands at the 32- and
// 64-bit limb boundaries and random ones up to 80 digits, every operator and comparison, exact
// conversions from doubles, parsing edge cases, and long products and quotients. Both runtimes
// run it; the output must be identical (V8's BigInt is the reference).
//
//   node gen.js OUT.ax [pairs]
'use strict';
const fs = require('fs');
const out = process.argv[2];
const pairs = Number(process.argv[3] || 400);
let seed = 12345;
const rnd = () => { seed = (seed * 1103515245 + 12345) % 2147483648; return seed / 2147483648; };
const num = () => {
  const len = 1 + Math.floor(rnd() * (rnd() < 0.3 ? 5 : 60));
  let s = '';
  for (let i = 0; i < len; i++) s += Math.floor(rnd() * 10);
  s = s.replace(/^0+(?=\d)/, '');
  if (rnd() < 0.2) s = s.replace(/^./, '9') + '0'.repeat(Math.floor(rnd() * 20));
  return (rnd() < 0.4 ? '-' : '') + s;
};
const special = ['0', '1', '-1', '4294967295', '4294967296', '-4294967296', '18446744073709551615',
  '18446744073709551616', '340282366920938463463374607431768211456', '79228162514264337593543950335',
  '9007199254740993', '-9007199254740993'];
let src = '^main:\n';
for (let i = 0; i < pairs; i++) {
  const n = special.length;
  const a = i < n * n ? special[i % n] : num();
  const b = i < n * n ? special[Math.floor(i / n) % n] : num();
  src += `  a = big("${a}")\n  b = big("${b}")\n  print(a + b, a - b, a * b, a < b, a == b, a > b)\n  ?b != big(0):\n    print(a / b, a % b)\n`;
}
for (const d of [0.5, 1e15, 2 ** 53, 2 ** 53 + 2, 1e21, 1.7976931348623157e308, -2.5e-7, 123456789.987, 5e-324]) {
  src += `  print(big(${d}), big(${d}) > ${d}, big(${d}) == big(${d}), big(${d}) < ${d}, float(str(big(${d}))))\n`;
}
for (const s of ['123456789012345678901234567890123456789', '-98765432109876543210987654321', '0xffffffffffffffffffffffff',
  '0b1011', '0o777', ' 12 ', '+5', '1_000', '1.0', '', '-', '0x', '--1']) src += `  print(big("${s}"))\n`;
src += '  f = big(1)\n  *i in range(1, 60):\n    f = f * big(i)\n  print(f, str(f).len())\n';
src += '  print(big(2) ** big(521) - big(1))\n';
src += '  print((big(2) ** big(200)) / (big(3) ** big(50)), (big(2) ** big(200)) % (big(3) ** big(50)))\n';
src += '  x = big("123456789123456789123456789")\n';
src += '  print(x > 1.2345678912345678e26, x < 1.2345678912345679e26, x == 123456789123456789123456789)\n';
fs.writeFileSync(out, src);
