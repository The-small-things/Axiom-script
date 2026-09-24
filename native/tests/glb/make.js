// make.js — write a corpus of .glb files exercising parseGLBMulti's rules, then dump what the
// reference parses from each, for glbcheck.c to compare.
// usage: node make.js DIR [MUTANTS]   → DIR/NNN_name.glb and DIR/js.txt
const fs = require('fs');
const path = require('path');
const { parseGLBMulti } = require('../../../interpreter');
const dir = process.argv[2];

function glb(gltf, bin, opts = {}) {
  let json = Buffer.from(typeof gltf === 'string' ? gltf : JSON.stringify(gltf), 'utf8');
  const padJ = (4 - (json.length % 4)) % 4;
  json = Buffer.concat([json, Buffer.alloc(padJ, opts.nulPad ? 0 : 0x20)]);
  const padB = (4 - (bin.length % 4)) % 4;
  const binP = Buffer.concat([bin, Buffer.alloc(padB)]);
  const chunks = [];
  const jh = Buffer.alloc(8); jh.writeUInt32LE(json.length, 0); jh.writeUInt32LE(0x4E4F534A, 4);
  chunks.push(jh, json);
  if (!opts.noBin) { const bh = Buffer.alloc(8); bh.writeUInt32LE(binP.length, 0); bh.writeUInt32LE(0x004E4942, 4); chunks.push(bh, binP); }
  const body = Buffer.concat(chunks);
  const head = Buffer.alloc(12);
  head.writeUInt32LE(opts.magic || 0x46546C67, 0); head.writeUInt32LE(opts.version || 2, 4);
  head.writeUInt32LE(opts.total !== undefined ? opts.total : 12 + body.length, 8);
  return Buffer.concat([head, body]);
}
const f32 = (xs) => { const b = Buffer.alloc(xs.length * 4); xs.forEach((x, i) => b.writeFloatLE(x, i * 4)); return b; };
const u16 = (xs) => { const b = Buffer.alloc(xs.length * 2); xs.forEach((x, i) => b.writeUInt16LE(x, i * 2)); return b; };
const u32 = (xs) => { const b = Buffer.alloc(xs.length * 4); xs.forEach((x, i) => b.writeUInt32LE(x, i * 4)); return b; };
const u8 = (xs) => Buffer.from(xs);

// A pyramid: 5 positions, 5 normals, 5 uvs, 18 indices (U16).
const P = [0, 1, 0, -1, 0, -1, 1, 0, -1, 1, 0, 1, -1, 0, 1];
const N = [0, 1, 0, -0.7, 0.3, -0.7, 0.7, 0.3, -0.7, 0.7, 0.3, 0.7, -0.7, 0.3, 0.7];
const UV = [0.5, 0, 0, 1, 1, 1, 0, 1, 1, 0.1];
const I = [0, 1, 2, 0, 2, 3, 0, 3, 4, 0, 4, 1, 1, 3, 2, 1, 4, 3];
const bin = Buffer.concat([f32(P), f32(N), f32(UV), u16(I)]);          // 60 + 60 + 40 + 36
const views = [
  { buffer: 0, byteOffset: 0, byteLength: 60 }, { buffer: 0, byteOffset: 60, byteLength: 60 },
  { buffer: 0, byteOffset: 120, byteLength: 40 }, { buffer: 0, byteOffset: 160, byteLength: 36 },
];
const accs = [
  { bufferView: 0, componentType: 5126, count: 5, type: 'VEC3' }, { bufferView: 1, componentType: 5126, count: 5, type: 'VEC3' },
  { bufferView: 2, componentType: 5126, count: 5, type: 'VEC2' }, { bufferView: 3, componentType: 5123, count: 18, type: 'SCALAR' },
];
const base = (extra = {}) => Object.assign({
  asset: { version: '2.0' }, buffers: [{ byteLength: bin.length }], bufferViews: views, accessors: accs,
  meshes: [{ primitives: [{ attributes: { POSITION: 0, NORMAL: 1, TEXCOORD_0: 2 }, indices: 3, material: 0 }] }],
}, extra);
const clone = (x) => JSON.parse(JSON.stringify(x));

const cases = {};
cases.pyramid = glb(base(), bin);
cases.nul_padded_json = glb(base(), bin, { nulPad: true });
cases.no_normals_no_uv = glb(base({ meshes: [{ primitives: [{ attributes: { POSITION: 0 }, indices: 3 }] }] }), bin);
cases.no_indices = glb(base({ meshes: [{ primitives: [{ attributes: { POSITION: 0, NORMAL: 1 } }] }] }), bin);
{ // two meshes, three primitives: U8, U32 and float indices
  const b2 = Buffer.concat([bin, u8([0, 1, 2, 0, 2, 3]), Buffer.alloc(2), u32([0, 3, 4, 0, 4, 1]), f32([1.9, 70000.5, -1, 2])]);
  const v2 = views.concat([{ buffer: 0, byteOffset: 196, byteLength: 6 }, { buffer: 0, byteOffset: 204, byteLength: 24 }, { buffer: 0, byteOffset: 228, byteLength: 16 }]);
  const a2 = accs.concat([{ bufferView: 4, componentType: 5121, count: 6 }, { bufferView: 5, componentType: 5125, count: 6, type: 'SCALAR' }, { bufferView: 6, componentType: 5126, count: 4 }]);
  cases.multi_prim = glb(base({ bufferViews: v2, accessors: a2, meshes: [
    { primitives: [{ attributes: { POSITION: 0 }, indices: 4, material: 1 }, { attributes: { POSITION: 0, TEXCOORD_0: 2 }, indices: 5 }] },
    { primitives: [{ attributes: { POSITION: 0 }, indices: 6, material: 2 }] },
  ] }), b2);
}
cases.oob_read = (() => { const a = clone(accs); a[0].count = 50; return glb(base({ accessors: a }), bin); })();
cases.no_bufferviews = (() => { const g = base(); delete g.bufferViews; return glb(g, bin); })();
cases.no_accessors = (() => { const g = base(); delete g.accessors; return glb(g, bin); })();
cases.other_buffer = (() => { const v = clone(views); v[0].buffer = 1; return glb(base({ bufferViews: v, buffers: [{}, {}] }), bin); })();
cases.missing_normal_accessor = glb(base({ meshes: [{ primitives: [{ attributes: { POSITION: 0, NORMAL: 9 }, indices: 3 }] }] }), bin);
cases.short_normals = (() => { const a = clone(accs); a[1].count = 2; return glb(base({ accessors: a }), bin); })();
cases.fractional_count = (() => { const a = clone(accs); a[3].count = 7.9; return glb(base({ accessors: a }), bin); })();
cases.negative_count = (() => { const a = clone(accs); a[3].count = -3; return glb(base({ accessors: a }), bin); })();
cases.u8_past_end = (() => { const a = accs.concat([{ bufferView: 3, byteOffset: 30, componentType: 5121, count: 12 }]);
  return glb(base({ accessors: a, meshes: [{ primitives: [{ attributes: { POSITION: 0 }, indices: 4 }] }] }), bin); })();
cases.unknown_component = (() => { const a = clone(accs); a[1].componentType = 5122; return glb(base({ accessors: a }), bin); })();
cases.string_index = glb(base({ meshes: [{ primitives: [{ attributes: { POSITION: '0' }, indices: '3' }] }] }), bin);
cases.skin_and_animation = (() => {
  const b2 = Buffer.concat([bin, f32([1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1]), f32([0, 1]), f32([0, 0, 0, 0, 1, 0])]);
  const v2 = views.concat([{ buffer: 0, byteOffset: 196, byteLength: 64 }, { buffer: 0, byteOffset: 260, byteLength: 8 }, { buffer: 0, byteOffset: 268, byteLength: 24 }]);
  const a2 = accs.concat([{ bufferView: 4, componentType: 5126, count: 1, type: 'MAT4' }, { bufferView: 5, componentType: 5126, count: 2 }, { bufferView: 6, componentType: 5126, count: 2, type: 'VEC3' }]);
  return glb(base({ bufferViews: v2, accessors: a2, skins: [{ joints: [0], inverseBindMatrices: 4 }],
    animations: [{ name: 'bob', samplers: [{ input: 5, output: 6 }], channels: [{ sampler: 0, target: { node: 0, path: 'translation' } }] }] }), b2);
})();
cases.animation_without_target = (() => {
  const g = JSON.parse(JSON.stringify(base({ animations: [{ samplers: [{ input: 0, output: 1 }], channels: [{ sampler: 0 }] }] })));
  return glb(g, bin);
})();
cases.null_skin = glb(base({ skins: [null] }), bin);
cases.morph_missing_accessor = glb(base({ meshes: [{ primitives: [{ attributes: { POSITION: 0 }, targets: [{ POSITION: 12 }] }] }] }), bin);
cases.morph_ok = glb(base({ meshes: [{ primitives: [{ attributes: { POSITION: 0 }, targets: [{ POSITION: 1, NORMAL: 1 }] }] }] }), bin);
cases.bad_magic = glb(base(), bin, { magic: 0x12345678 });
cases.version_1 = glb(base(), bin, { version: 1 });
cases.total_too_long = glb(base(), bin, { total: 100000 });
cases.no_bin_chunk = glb(base(), bin, { noBin: true });
cases.bad_json = glb('{"meshes": [', bin);
cases.json_trailing_garbage = glb(JSON.stringify(base()) + ' x', bin);
cases.no_meshes = glb(base({ meshes: [] }), bin);
cases.mesh_without_primitives = glb(base({ meshes: [{}, { primitives: [{ attributes: { POSITION: 0 } }] }] }), bin);

// Mutants: truncations, byte flips, and edits inside the JSON chunk, from a fixed seed — each
// must load, or fail to, exactly as in the reference.
const fuzz = parseInt(process.argv[3] || '0', 10);
const names = Object.keys(cases);
let rs = 11;
const rand = () => { rs = (rs * 1103515245 + 12345) % 2147483648; return rs / 2147483648; };
for (let k = 0; k < fuzz; k++) {
  const b = Buffer.from(cases[names[Math.floor(rand() * names.length)]]);
  let m = b;
  for (let e = 1 + Math.floor(rand() * 4); e > 0; e--) {
    const r = rand();
    if (r < 0.2 && m.length > 12) m = m.subarray(0, 12 + Math.floor(rand() * (m.length - 12)));
    else if (r < 0.6 && m.length >= 20) {
      const jl = m.readUInt32LE(12);
      if (jl > 0 && 20 + jl <= m.length) m[20 + Math.floor(rand() * jl)] = '0123456789,:{}[]"xnul '.charCodeAt(Math.floor(rand() * 23));
    } else if (m.length) m[Math.floor(rand() * m.length)] = Math.floor(rand() * 256);
  }
  cases[`mutant_${k}`] = Buffer.from(m);
}

const out = [];
Object.keys(cases).forEach((name, i) => {
  const file = `${String(i).padStart(3, '0')}_${name}.glb`;
  fs.writeFileSync(path.join(dir, file), cases[name]);
  let prims = null;
  try { prims = parseGLBMulti(cases[name]); } catch (e) { prims = null; }
  out.push(`== ${file}`);
  if (!prims || !prims.length) { out.push('FAIL'); return; }
  prims.forEach((p, k) => {
    out.push(`prim ${k} verts ${p.vertices.length / 8} idx ${p.indices.length} material ${JSON.stringify(p.material)}`);
    out.push(Array.from(p.vertices).map(String).join(' '));
    out.push(Array.from(p.indices).map(String).join(' '));
  });
});
fs.writeFileSync(path.join(dir, 'js.txt'), out.join('\n') + '\n');
