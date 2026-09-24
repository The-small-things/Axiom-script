// pngdiff.js A.png B.png — how many pixels differ between two PNGs written by render.c
// (8-bit RGBA, filter 0 rows).
const fs = require('fs');
const zlib = require('zlib');
function read(file) {
  const b = fs.readFileSync(file);
  let pos = 8, w = 0, h = 0;
  const idat = [];
  while (pos < b.length) {
    const len = b.readUInt32BE(pos), type = b.toString('ascii', pos + 4, pos + 8);
    const data = b.subarray(pos + 8, pos + 8 + len);
    if (type === 'IHDR') { w = data.readUInt32BE(0); h = data.readUInt32BE(4); }
    if (type === 'IDAT') idat.push(data);
    pos += 12 + len;
  }
  const raw = zlib.inflateSync(Buffer.concat(idat));
  const px = Buffer.alloc(w * h * 4);
  for (let y = 0; y < h; y++) raw.copy(px, y * w * 4, y * (w * 4 + 1) + 1, (y + 1) * (w * 4 + 1));
  return { w, h, px };
}
const a = read(process.argv[2]), b = read(process.argv[3]);
let differ = 0;
for (let i = 0; i < a.w * a.h; i++) if (a.px.readUInt32BE(i * 4) !== b.px.readUInt32BE(i * 4)) differ++;
console.log(`${differ}`);
