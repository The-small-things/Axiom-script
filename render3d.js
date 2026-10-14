// render3d.js — the software rasterizer behind --terminal, --headless and --sdl.
//
// A port of native/render.c, line for line, so that both runtimes draw the same pixels (and
// write the same PNG bytes) for the same world: every frame, the `!mesh(#Res, color: 0xRRGGBB)`
// commands collected from &render blocks are drawn through the first &Camera entity (its pose,
// `~target`, `~fov`) and lit by the first &Light (`~dir`, `~intensity`, `~ambient`). The output
// is an RGBA buffer, which terminal.js turns into terminal frames, savePNG() into files and the
// SDL window into pixels on screen.
//
// Meshes: a #Mesh3D loaded from a .glb is drawn from its own triangles, in its rest pose; any
// other is procedural — a path naming a sphere/ball, a plane/ground/floor, or anything else (a
// box). Arithmetic follows the C code's: doubles throughout, a float32 depth buffer, C's
// truncating integer conversions and its NaN-ignoring fmin/fmax.
//
// Public API (main.js and interpreter.js):
//   rasterizeFrame(world, width, height) → { pixels: Uint8Array (RGBA), width, height }
//   savePNG(pixels, width, height, file) → true on success
//   proceduralMeshForPath(path), loadTexture(path), proceduralTexture(name) → null
//     (descriptors stay as the native runtime builds them: procedural shapes are chosen at
//     draw time from the path, and textures are not sampled)
'use strict';
const fs = require('fs');
const path = require('path');

const FLT_MAX = 3.4028234663852886e38;
const cfmin = (a, b) => (a !== a ? b : b !== b ? a : Math.min(a, b));
const cfmax = (a, b) => (a !== a ? b : b !== b ? a : Math.max(a, b));

const p3 = (x, y, z) => ({ x, y, z });
const padd = (a, b) => p3(a.x + b.x, a.y + b.y, a.z + b.z);
const psub = (a, b) => p3(a.x - b.x, a.y - b.y, a.z - b.z);
const pmul = (a, s) => p3(a.x * s, a.y * s, a.z * s);
const pdot = (a, b) => a.x * b.x + a.y * b.y + a.z * b.z;
const pcross = (a, b) => p3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
function pnorm(a) { const m = Math.sqrt(pdot(a, a)); return m < 1e-12 ? p3(0, 0, 0) : pmul(a, 1 / m); }

const kind = (v) => (v !== null && typeof v === 'object' && v.constructor ? v.constructor.name : '');
function pof(v, dflt) {
  const k = kind(v);
  if (k === 'Vec3') return p3(v.x, v.y, v.z);
  if (k === 'Vec2') return p3(v.x, 0, v.y);
  if (typeof v === 'number') return p3(v, v, v);
  return dflt;
}
function qrotate(q, v) {
  if (kind(q) !== 'Quat') return v;
  const u = p3(q.x, q.y, q.z);
  const t = pmul(pcross(u, v), 2);
  return padd(padd(v, pmul(t, q.w)), pcross(u, t));
}

// ---- meshes ----------------------------------------------------------------------------------

let boxMesh = null, sphereMesh = null, planeMesh = null;

function buildMeshes() {
  if (boxMesh) return;
  const bv = [[-.5, -.5, -.5], [.5, -.5, -.5], [.5, .5, -.5], [-.5, .5, -.5],
              [-.5, -.5, .5], [.5, -.5, .5], [.5, .5, .5], [-.5, .5, .5]];
  const bt = [4, 5, 6, 4, 6, 7, 1, 0, 3, 1, 3, 2, 0, 4, 7, 0, 7, 3,
              5, 1, 2, 5, 2, 6, 3, 7, 6, 3, 6, 2, 0, 1, 5, 0, 5, 4];
  boxMesh = { v: bv.map(([x, y, z]) => p3(x, y, z)), tri: bt, nt: 12 };

  const SL = 16, ST = 10;
  const sv = [], st = [];
  for (let j = 0; j <= ST; j++) for (let i = 0; i <= SL; i++) {
    const th = Math.PI * j / ST, ph = 2 * Math.PI * i / SL;
    sv.push(p3(0.5 * Math.sin(th) * Math.cos(ph), 0.5 * Math.cos(th), 0.5 * Math.sin(th) * Math.sin(ph)));
  }
  for (let j = 0; j < ST; j++) for (let i = 0; i < SL; i++) {
    const a = j * (SL + 1) + i, b = a + 1, c = a + SL + 1, d = c + 1;
    st.push(a, b, c, b, d, c);
  }
  sphereMesh = { v: sv, tri: st, nt: st.length / 3 };

  planeMesh = { v: [p3(-.5, 0, -.5), p3(.5, 0, -.5), p3(.5, 0, .5), p3(-.5, 0, .5)], tri: [0, 2, 1, 0, 3, 2], nt: 2 };
}

// render.c lowercases ASCII letters only, in the first 255 bytes of the path.
function pathHas(p, w) {
  const low = Buffer.from(p, 'utf8').subarray(0, 255).toString('latin1').replace(/[A-Z]/g, (c) => c.toLowerCase());
  return low.includes(w);
}

// A .glb primitive as a mesh (converted once, then reused every frame). Triangles whose indices
// fall outside the vertex list are dropped rather than read out of bounds.
const glbCache = new WeakMap();
function glbMesh(res) {
  let m = glbCache.get(res);
  if (m) return m;
  const verts = res.vertices, idx = res.indices;
  const nv = Math.floor(verts.length / 8);
  const v = [];
  for (let i = 0; i < nv; i++) v.push(p3(verts[i * 8], verts[i * 8 + 1], verts[i * 8 + 2]));
  const tri = [];
  for (let t = 0; t + 2 < idx.length; t += 3) {
    const a = idx[t], b = idx[t + 1], c = idx[t + 2];
    if (a >= nv || b >= nv || c >= nv) continue;
    tri.push(a, b, c);
  }
  m = { v, tri, nt: tri.length / 3 };
  glbCache.set(res, m);
  return m;
}

function meshFor(res) {
  buildMeshes();
  const isRes = res !== null && typeof res === 'object' && !Array.isArray(res);
  if (isRes && res.glb && res.vertices && res.indices) return glbMesh(res);
  let m = boxMesh;
  if (isRes && typeof res.path === 'string') {
    const p = res.path;
    if (pathHas(p, 'sphere') || pathHas(p, 'ball') || pathHas(p, 'orb')) m = sphereMesh;
    else if (pathHas(p, 'plane') || pathHas(p, 'ground') || pathHas(p, 'floor') || pathHas(p, 'quad')) m = planeMesh;
  }
  return m;
}

// ---- frame -----------------------------------------------------------------------------------

function clear(fr) {
  for (let y = 0; y < fr.h; y++) {
    const t = y / (fr.h > 1 ? fr.h - 1 : 1);
    const r = Math.trunc(70 + 110 * t), g = Math.trunc(110 + 90 * t), b = Math.trunc(170 + 60 * t);
    for (let x = 0; x < fr.w; x++) {
      const i = (y * fr.w + x) * 4;
      fr.px[i] = r; fr.px[i + 1] = g; fr.px[i + 2] = b; fr.px[i + 3] = 255;
      fr.z[y * fr.w + x] = FLT_MAX;
    }
  }
}

function project(c, wp) {
  const s = { x: 0, y: 0, z: 0, ok: false };
  const d = psub(wp, c.eye);
  const cz = pdot(d, c.fwd);
  if (cz < c.near) return s;
  const cx = pdot(d, c.right), cy = pdot(d, c.up);
  s.x = (cx / cz * c.f / c.aspect * 0.5 + 0.5) * c.w;
  s.y = (0.5 - cy / cz * c.f * 0.5) * c.h;
  s.z = cz;
  s.ok = true;
  return s;
}

function tri(fr, a, b, c, r, g, bl) {
  const area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
  if (area >= 0) return;   // back-facing (screen y points down)
  let x0 = Math.floor(cfmin(a.x, cfmin(b.x, c.x))), x1 = Math.ceil(cfmax(a.x, cfmax(b.x, c.x)));
  let y0 = Math.floor(cfmin(a.y, cfmin(b.y, c.y))), y1 = Math.ceil(cfmax(a.y, cfmax(b.y, c.y)));
  if (x0 < 0) x0 = 0;
  if (y0 < 0) y0 = 0;
  if (x1 > fr.w - 1) x1 = fr.w - 1;
  if (y1 > fr.h - 1) y1 = fr.h - 1;
  for (let y = y0; y <= y1; y++) for (let x = x0; x <= x1; x++) {
    const px = x + 0.5, py = y + 0.5;
    let w0 = (b.x - px) * (c.y - py) - (b.y - py) * (c.x - px);
    let w1 = (c.x - px) * (a.y - py) - (c.y - py) * (a.x - px);
    let w2 = (a.x - px) * (b.y - py) - (a.y - py) * (b.x - px);
    if (w0 > 0 || w1 > 0 || w2 > 0) continue;
    w0 /= area; w1 /= area; w2 /= area;
    // Perspective-correct depth: interpolate 1/z.
    const iz = w0 / a.z + w1 / b.z + w2 / c.z;
    const z = Math.fround(1 / iz);
    const i = y * fr.w + x;
    if (z >= fr.z[i]) continue;
    fr.z[i] = z;
    const p = i * 4;
    fr.px[p] = r; fr.px[p + 1] = g; fr.px[p + 2] = bl; fr.px[p + 3] = 255;
  }
}

function firstWithBase(world, base) {
  for (const e of world.entities || []) if (!e._pendingRemove && e.decl && e.decl.base === base) return e;
  return null;
}

// (uint32_t)num & 0xffffff for a number, a clamped 0..1 RGB triple for a vector.
function colorOf(v, dflt) {
  if (typeof v === 'number' && v >= 0) return (Math.trunc(v) % 4294967296) & 0xffffff;
  if (kind(v) === 'Vec3') {
    const r = Math.trunc(cfmax(0, cfmin(1, v.x)) * 255), g = Math.trunc(cfmax(0, cfmin(1, v.y)) * 255), b = Math.trunc(cfmax(0, cfmin(1, v.z)) * 255);
    return (r << 16 | g << 8 | b) >>> 0;
  }
  return dflt;
}

function rasterizeFrame(world, w, h) {
  const fr = { w, h, px: new Uint8Array(w * h * 4), z: new Float32Array(w * h) };
  clear(fr);
  if (!world) return { pixels: fr.px, width: w, height: h };

  const cam = { w, h, aspect: w / h, near: 0.05, eye: p3(0, 6, 12) };
  let fov = 60;
  let target = p3(0, 0, 0);
  const ce = firstWithBase(world, 'Camera');
  if (ce) {
    const pose = ce.locals.get('pose');
    cam.eye = pof(pose ? pose.pos : null, cam.eye);
    const t = ce.get('target');
    const hasTarget = kind(t) === 'Vec3' || kind(t) === 'Vec2';
    if (hasTarget) target = pof(t, target);
    else {
      const rot = ce.get('rot');
      if (rot !== undefined) target = padd(cam.eye, qrotate(rot, p3(0, 0, -1)));
    }
    const fv = ce.get('fov');
    if (typeof fv === 'number' && fv > 1) fov = fv;
  }
  cam.fwd = pnorm(psub(target, cam.eye));
  if (pdot(cam.fwd, cam.fwd) < 1e-12) cam.fwd = p3(0, 0, -1);
  const worldUp = Math.abs(cam.fwd.y) > 0.9999 ? p3(0, 0, -1) : p3(0, 1, 0);
  cam.right = pnorm(pcross(cam.fwd, worldUp));
  cam.up = pcross(cam.right, cam.fwd);
  cam.f = 1 / Math.tan(fov * Math.PI / 360);

  let light = pnorm(p3(-0.4, -1, -0.3));
  let intensity = 1, ambient = 0.35;
  const le = firstWithBase(world, 'Light');
  if (le) {
    let v = le.get('dir');
    if (v !== undefined) light = pnorm(pof(v, light));
    v = le.get('intensity');
    if (typeof v === 'number') intensity = v;
    v = le.get('ambient');
    if (typeof v === 'number') ambient = v;
  }

  for (const cmd of world.drawList || []) {
    if (!cmd || typeof cmd.entity !== 'string') continue;
    const e = world.resolveTag(cmd.entity);
    if (!e) continue;
    const rgb = colorOf(cmd.color, 0xcccccc);
    const wp = e.get('wpose');
    let pos = p3(0, 0, 0), s = p3(1, 1, 1), rot = null;
    if (kind(wp) === 'Transform') {
      pos = pof(wp.pos, pos);
      s = pof(wp.scl, s);
      rot = wp.rot;
    }
    if ('scl' in cmd) s = pof(cmd.scl, s);
    const m = meshFor(cmd.mesh);
    const wv = new Array(m.v.length), screen = new Array(m.v.length);
    for (let k = 0; k < m.v.length; k++) {
      const v = p3(m.v[k].x * s.x, m.v[k].y * s.y, m.v[k].z * s.z);
      wv[k] = padd(qrotate(rot, v), pos);
      screen[k] = project(cam, wv[k]);
    }
    for (let t = 0; t < m.nt; t++) {
      const ia = m.tri[t * 3], ib = m.tri[t * 3 + 1], ic = m.tri[t * 3 + 2];
      if (!screen[ia].ok || !screen[ib].ok || !screen[ic].ok) continue;   // crosses the near plane
      const n = pnorm(pcross(psub(wv[ib], wv[ia]), psub(wv[ic], wv[ia])));
      let lit = ambient + intensity * cfmax(0, -pdot(n, light));
      if (lit > 1.2) lit = 1.2;
      const r = Math.trunc(((rgb >> 16) & 255) * lit), g = Math.trunc(((rgb >> 8) & 255) * lit), b = Math.trunc((rgb & 255) * lit);
      // (uint8_t) of the clamped value: a negative one wraps, as the Uint8Array store does.
      tri(fr, screen[ia], screen[ib], screen[ic], r > 255 ? 255 : r, g > 255 ? 255 : g, b > 255 ? 255 : b);
    }
  }
  return { pixels: fr.px, width: w, height: h };
}

// ---- PNG -------------------------------------------------------------------------------------
// Uncompressed ("stored") deflate, byte for byte what render.c writes.

let crcTable = null;
function crc(buf) {
  if (!crcTable) {
    crcTable = new Uint32Array(256);
    for (let n = 0; n < 256; n++) {
      let c = n;
      for (let k = 0; k < 8; k++) c = (c & 1) ? (0xedb88320 ^ (c >>> 1)) >>> 0 : c >>> 1;
      crcTable[n] = c;
    }
  }
  let c = 0xffffffff;
  for (let i = 0; i < buf.length; i++) c = (crcTable[(c ^ buf[i]) & 255] ^ (c >>> 8)) >>> 0;
  return (c ^ 0xffffffff) >>> 0;
}
function chunk(type, data) {
  const len = Buffer.alloc(4);
  len.writeUInt32BE(data.length);
  const body = Buffer.concat([Buffer.from(type, 'ascii'), data]);
  const c = Buffer.alloc(4);
  c.writeUInt32BE(crc(body));
  return Buffer.concat([len, body, c]);
}
function encodePNG(rgba, w, h) {
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(w, 0);
  ihdr.writeUInt32BE(h, 4);
  ihdr[8] = 8; ihdr[9] = 6;
  const stride = w * 4 + 1;
  const raw = Buffer.alloc(h * stride);
  for (let y = 0; y < h; y++) Buffer.from(rgba.buffer, rgba.byteOffset + y * w * 4, w * 4).copy(raw, y * stride + 1);
  const parts = [Buffer.from([0x78, 0x01])];
  let s1 = 1, s2 = 0;
  for (let off = 0; off < raw.length || off === 0;) {
    const n = Math.min(65535, raw.length - off);
    parts.push(Buffer.from([off + n >= raw.length ? 1 : 0, n & 255, n >> 8, ~n & 255, (~n >> 8) & 255]));
    parts.push(raw.subarray(off, off + n));
    for (let i = 0; i < n; i++) { s1 = (s1 + raw[off + i]) % 65521; s2 = (s2 + s1) % 65521; }
    off += n;
    if (!n) break;
  }
  const adler = Buffer.alloc(4);
  adler.writeUInt32BE(((s2 << 16) | s1) >>> 0);
  parts.push(adler);
  return Buffer.concat([
    Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]),
    chunk('IHDR', ihdr),
    chunk('IDAT', Buffer.concat(parts)),
    chunk('IEND', Buffer.alloc(0)),
  ]);
}
function savePNG(pixels, w, h, file) {
  try {
    fs.mkdirSync(path.dirname(file), { recursive: true });
    fs.writeFileSync(file, encodePNG(pixels, w, h));
    return true;
  } catch (e) { return false; }
}

module.exports = {
  rasterizeFrame,
  savePNG,
  encodePNG,
  proceduralMeshForPath: () => null,
  loadTexture: () => null,
  proceduralTexture: () => null,
};
