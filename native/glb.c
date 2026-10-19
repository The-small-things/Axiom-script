// glb.c — binary glTF 2.0 (.glb) meshes, ported from interpreter.js parseGLBMulti /
// buildPrimitive / decodeAccessor.
//
// A load yields every primitive of every mesh, in declaration order, each as interleaved
// float32 vertices (position xyz, normal xyz, uv xy — the reference's layout, with its defaults
// for a missing normal or uv) and 16-bit indices. What decides whether a load succeeds is ported
// as carefully as the geometry, because it decides which resources a program can name (`#X_0`,
// `#X_1`, …): where the reference throws — a read past the binary chunk, a missing accessors or
// bufferViews table, a negative count, a malformed skin or animation entry — the whole load fails
// here too; where it merely returns null for one accessor or primitive, so does this.
//
// Skins, morph targets and animation clips are validated (they can fail a load) but not kept:
// the native renderer draws the rest pose.

#include "axiom.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

// ---- base64 (Node's Buffer.from(s, 'base64'): standard and URL-safe alphabets, anything else
// skipped, stops at '=') ------------------------------------------------------------------------

bool ax_base64_decode(const char *s, size_t n, uint8_t **out, size_t *outn) {
  uint8_t *buf = malloc(n / 4 * 3 + 4);
  size_t o = 0;
  uint32_t acc = 0;
  int bits = 0;
  for (size_t i = 0; i < n; i++) {
    int c = (unsigned char)s[i], v;
    if (c >= 'A' && c <= 'Z') v = c - 'A';
    else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
    else if (c >= '0' && c <= '9') v = c - '0' + 52;
    else if (c == '+' || c == '-') v = 62;
    else if (c == '/' || c == '_') v = 63;
    else if (c == '=') break;
    else continue;
    acc = (acc << 6) | (uint32_t)v;
    bits += 6;
    if (bits >= 8) { bits -= 8; buf[o++] = (uint8_t)(acc >> bits); }
  }
  *out = buf;
  *outn = o;
  return true;
}

// ---- JSON helpers: JavaScript's property access on a parsed glTF ------------------------------

// obj[key] for a string key. `thrown` is set when obj is null/undefined (a TypeError in JS).
static AxValue jkey(AxValue obj, const char *key, bool *thrown) {
  if (obj.t == AX_NULL) { *thrown = true; return ax_null(); }
  AxValue out = ax_null();
  if (obj.t == AX_DICT) {
    AxStr *k = ax_internz(key);
    if (!ax_dict_get((AxDict *)obj.o, k, &out)) out = ax_null();
    ax_release(ax_strv(k));
  }
  return out;
}

static bool jhas(AxValue obj, const char *key) {
  if (obj.t != AX_DICT) return false;
  AxStr *k = ax_internz(key);
  AxValue v;
  bool has = ax_dict_get((AxDict *)obj.o, k, &v);
  ax_release(ax_strv(k));
  if (has) { bool nn = v.t != AX_NULL; ax_release(v); return nn; }
  return false;
}

// table[index] where index is any JSON value (a number, or a numeric string, as JS allows).
static AxValue jindex(AxValue table, AxValue index, bool *thrown) {
  if (table.t == AX_NULL) { *thrown = true; return ax_null(); }
  if (table.t == AX_ARR) {
    double d = NAN;
    if (index.t == AX_NUM) d = index.num;
    else if (index.t == AX_STR) {
      char *end;
      const char *s = ((AxStr *)index.o)->data;
      d = strtod(s, &end);
      if (end == s || *end) d = NAN;
    }
    if (d >= 0 && d == floor(d) && d < ((AxArr *)table.o)->len) return ax_arr_get((AxArr *)table.o, (int64_t)d);
    return ax_null();
  }
  if (table.t == AX_DICT && (index.t == AX_STR || index.t == AX_NUM)) {
    AxStr *k = ax_to_str(index);
    AxValue out;
    if (!ax_dict_get((AxDict *)table.o, k, &out)) out = ax_null();
    ax_release(ax_strv(k));
    return out;
  }
  return ax_null();
}

static double jnum(AxValue v, double dflt) { return v.t == AX_NUM ? v.num : dflt; }
static bool jstr_is(AxValue v, const char *s) { return v.t == AX_STR && !strcmp(((AxStr *)v.o)->data, s); }

// ---- accessors -------------------------------------------------------------------------------

typedef struct {
  const uint8_t *bin;
  size_t binlen;
  AxValue gltf;
  bool thrown;           // the reference would have thrown: the whole load fails
} Ctx;

typedef struct { double *v; size_t n; bool ok; } Data;   // ok=false: the accessor decodes to null

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

// decodeAccessor(gltf, binChunk, accessor)
static Data decode(Ctx *c, AxValue acc) {
  Data d = { NULL, 0, false };
  if (c->thrown) return d;
  if (acc.t == AX_NULL) { c->thrown = true; return d; }        // accessor.bufferView of undefined
  AxValue bvi = jkey(acc, "bufferView", &c->thrown);
  AxValue views = jkey(c->gltf, "bufferViews", &c->thrown);
  AxValue bv = jindex(views, bvi, &c->thrown);
  ax_release(bvi); ax_release(views);
  if (c->thrown || bv.t == AX_NULL) { ax_release(bv); return d; }
  AxValue bufi = jkey(bv, "buffer", &c->thrown);
  AxValue buffers = jkey(c->gltf, "buffers", &c->thrown);
  AxValue buf = jindex(buffers, bufi, &c->thrown);
  ax_release(buffers);
  bool is0 = bufi.t == AX_NUM && bufi.num == 0;
  ax_release(bufi);
  if (c->thrown || buf.t == AX_NULL || !is0) { ax_release(buf); ax_release(bv); return d; }
  ax_release(buf);
  AxValue bo = jkey(bv, "byteOffset", &c->thrown), ao = jkey(acc, "byteOffset", &c->thrown);
  AxValue cnt = jkey(acc, "count", &c->thrown), ct = jkey(acc, "componentType", &c->thrown), ty = jkey(acc, "type", &c->thrown);
  ax_release(bv);
  // (bufferView.byteOffset || 0) + (accessor.byteOffset || 0): a non-number offset makes every
  // read throw in the reference.
  bool bad_off = (bo.t != AX_NULL && bo.t != AX_NUM && ax_truthy(bo)) || (ao.t != AX_NULL && ao.t != AX_NUM && ax_truthy(ao));
  double off = jnum(bo, 0) + jnum(ao, 0);
  int comps = jstr_is(ty, "VEC3") ? 3 : jstr_is(ty, "VEC2") ? 2 : jstr_is(ty, "VEC4") ? 4 : jstr_is(ty, "MAT4") ? 16 : jstr_is(ty, "MAT3") ? 9 : 1;
  double count = cnt.t == AX_NUM ? cnt.num : NAN;
  int type = ct.t == AX_NUM ? (int)ct.num : 0;
  if (ct.t == AX_NUM && ct.num != floor(ct.num)) type = 0;
  ax_release(bo); ax_release(ao); ax_release(cnt); ax_release(ct); ax_release(ty);
  int size = type == 5126 ? 4 : type == 5123 ? 2 : type == 5121 ? 1 : type == 5125 ? 4 : 0;
  if (!size) return d;                                          // an unknown component type: null
  // new TypedArray(count * components): ToIndex — NaN is 0, a fraction truncates, a negative or
  // enormous length throws.
  double len = count * comps;
  if (isnan(len)) len = 0;
  len = trunc(len);
  if (len < 0 || len > 1e9) { c->thrown = true; return d; }
  size_t n = (size_t)len;
  d.v = malloc(sizeof(double) * (n ? n : 1));
  d.n = n;
  d.ok = true;
  if (type == 5121) {
    // Bytes are read by indexing (binChunk[offset + i]), which never throws: a position outside
    // the chunk, or not an integer, reads undefined, stored as 0.
    for (size_t i = 0; i < n; i++) {
      double at = off + (double)i;
      d.v[i] = (!bad_off && at >= 0 && at == floor(at) && at < (double)c->binlen) ? c->bin[(size_t)at] : 0;
    }
    return d;
  }
  // readFloatLE / readUInt16LE / readUInt32LE throw on a position outside the chunk or not an
  // integer.
  if (n && (bad_off || off != floor(off) || off < 0 || off + (double)n * size > (double)c->binlen)) {
    free(d.v);
    d.v = NULL; d.n = 0; d.ok = false;
    c->thrown = true;
    return d;
  }
  const uint8_t *p = c->bin + (size_t)off;
  for (size_t i = 0; i < n; i++) {
    if (type == 5126) { uint32_t u = rd32(p + i * 4); float f; memcpy(&f, &u, 4); d.v[i] = f; }
    else if (type == 5123) d.v[i] = (double)(p[i * 2] | p[i * 2 + 1] << 8);
    else d.v[i] = (double)rd32(p + i * 4);
  }
  return d;
}

static void data_free(Data *d) { free(d->v); d->v = NULL; }

// accessors[index] (a missing accessor is `undefined`, which decode() treats as a throw)
static AxValue accessor(Ctx *c, AxValue index) {
  AxValue accs = jkey(c->gltf, "accessors", &c->thrown);
  AxValue a = jindex(accs, index, &c->thrown);
  ax_release(accs);
  return a;
}

// Uint16Array element assignment: ToUint16.
static uint16_t to_u16(double x) {
  if (!isfinite(x)) return 0;
  double t = fmod(trunc(x), 65536.0);
  if (t < 0) t += 65536.0;
  return (uint16_t)t;
}

static float f32(double x) { return (float)x; }   // Float32Array element assignment

// buildPrimitive(gltf, binChunk, prim) → false when the reference returns null for it.
static bool build(Ctx *c, AxValue prim, AxGlbPrim *out) {
  memset(out, 0, sizeof *out);
  out->material = ax_null();
  if (prim.t == AX_NULL) { c->thrown = true; return false; }    // prim.attributes of undefined
  AxValue attrs = jkey(prim, "attributes", &c->thrown);
  if (!ax_truthy(attrs)) { ax_release(attrs); attrs = ax_dictv(ax_dict_new()); }   // `|| {}`
  AxValue ipos = jkey(attrs, "POSITION", &c->thrown);
  AxValue pos_acc = accessor(c, ipos);
  ax_release(ipos);
  AxValue norm_acc = ax_null(), uv_acc = ax_null(), idx_acc = ax_null(), j_acc = ax_null(), w_acc = ax_null();
  bool has_norm = jhas(attrs, "NORMAL"), has_uv = jhas(attrs, "TEXCOORD_0"), has_idx = jhas(prim, "indices");
  bool has_j = jhas(attrs, "JOINTS_0"), has_w = jhas(attrs, "WEIGHTS_0");
  if (has_norm) { AxValue i = jkey(attrs, "NORMAL", &c->thrown); norm_acc = accessor(c, i); ax_release(i); }
  if (has_uv) { AxValue i = jkey(attrs, "TEXCOORD_0", &c->thrown); uv_acc = accessor(c, i); ax_release(i); }
  if (has_idx) { AxValue i = jkey(prim, "indices", &c->thrown); idx_acc = accessor(c, i); ax_release(i); }
  if (has_j) { AxValue i = jkey(attrs, "JOINTS_0", &c->thrown); j_acc = accessor(c, i); ax_release(i); }
  if (has_w) { AxValue i = jkey(attrs, "WEIGHTS_0", &c->thrown); w_acc = accessor(c, i); ax_release(i); }
  bool ok = false;
  Data pos = { 0 }, nrm = { 0 }, uv = { 0 }, idx = { 0 }, jd = { 0 }, wd = { 0 };
  if (!c->thrown && pos_acc.t != AX_NULL) {
    // The reference decodes every accessor before it looks at any of them.
    // (An index naming no accessor is `undefined`, which the reference skips rather than decodes.)
    pos = decode(c, pos_acc);
    if (norm_acc.t != AX_NULL) nrm = decode(c, norm_acc);
    if (uv_acc.t != AX_NULL) uv = decode(c, uv_acc);
    if (idx_acc.t != AX_NULL) idx = decode(c, idx_acc);
    if (j_acc.t != AX_NULL) jd = decode(c, j_acc);
    if (w_acc.t != AX_NULL) wd = decode(c, w_acc);
    if (!c->thrown && pos.ok) {
      size_t nv = pos.n / 3;
      out->nverts = (int)nv;
      out->verts = malloc(sizeof(float) * 8 * (nv ? nv : 1));
      for (size_t i = 0; i < nv; i++) {
        float *v = out->verts + i * 8;
        v[0] = f32(pos.v[i * 3]); v[1] = f32(pos.v[i * 3 + 1]); v[2] = f32(pos.v[i * 3 + 2]);
        if (nrm.ok) {
          for (int k = 0; k < 3; k++) v[3 + k] = i * 3 + k < nrm.n ? f32(nrm.v[i * 3 + k]) : NAN;
        } else { v[3] = 0; v[4] = 1; v[5] = 0; }
        if (uv.ok) {
          for (int k = 0; k < 2; k++) v[6 + k] = i * 2 + k < uv.n ? f32(uv.v[i * 2 + k]) : NAN;
        } else { v[6] = 0; v[7] = 0; }
      }
      if (idx.ok) {
        out->nidx = (int)idx.n;
        out->idx = malloc(sizeof(uint16_t) * (idx.n ? idx.n : 1));
        for (size_t i = 0; i < idx.n; i++) out->idx[i] = to_u16(idx.v[i]);
      } else {
        out->nidx = (int)nv;
        out->idx = malloc(sizeof(uint16_t) * (nv ? nv : 1));
        for (size_t i = 0; i < nv; i++) out->idx[i] = to_u16((double)i);
      }
      AxValue mat;
      if (prim.t == AX_DICT && ax_dict_get((AxDict *)prim.o, ax_internz("material"), &mat)) out->material = mat;
      // Morph targets are decoded (a bad one fails the load) but not kept.
      AxValue targets = jkey(prim, "targets", &c->thrown);
      if (targets.t == AX_ARR) {
        AxArr *ta = (AxArr *)targets.o;
        for (uint32_t t = 0; t < ta->len && !c->thrown; t++) {
          AxValue tv = ta->items[t];
          if (tv.t == AX_NULL) { c->thrown = true; break; }
          static const char *keys[2] = { "POSITION", "NORMAL" };
          for (int k = 0; k < 2 && !c->thrown; k++) {
            if (!jhas(tv, keys[k])) continue;
            AxValue i = jkey(tv, keys[k], &c->thrown);
            AxValue a = accessor(c, i);
            Data dd = decode(c, a);
            data_free(&dd);
            ax_release(a); ax_release(i);
          }
        }
      }
      ax_release(targets);
      ok = !c->thrown;
      if (!ok) { free(out->verts); free(out->idx); out->verts = NULL; out->idx = NULL; ax_release(out->material); out->material = ax_null(); }
    }
  }
  data_free(&pos); data_free(&nrm); data_free(&uv); data_free(&idx); data_free(&jd); data_free(&wd);
  ax_release(pos_acc); ax_release(norm_acc); ax_release(uv_acc); ax_release(idx_acc); ax_release(j_acc); ax_release(w_acc);
  ax_release(attrs);
  return ok;
}

// parseGLBSkin / parseGLBAnimations, for their failure modes only.
static void check_skin_and_animations(Ctx *c) {
  AxValue skins = jkey(c->gltf, "skins", &c->thrown);
  if (skins.t == AX_ARR && ((AxArr *)skins.o)->len > 0) {
    AxValue s0 = ((AxArr *)skins.o)->items[0];
    if (s0.t == AX_NULL) c->thrown = true;
    else if (jhas(s0, "inverseBindMatrices")) {
      AxValue i = jkey(s0, "inverseBindMatrices", &c->thrown);
      AxValue a = accessor(c, i);
      if (a.t != AX_NULL) { Data d = decode(c, a); data_free(&d); }
      ax_release(a); ax_release(i);
    }
  }
  ax_release(skins);
  AxValue anims = jkey(c->gltf, "animations", &c->thrown);
  if (anims.t == AX_ARR) {
    AxArr *aa = (AxArr *)anims.o;
    for (uint32_t k = 0; k < aa->len && !c->thrown; k++) {
      AxValue an = aa->items[k];
      if (an.t == AX_NULL) { c->thrown = true; break; }
      AxValue chans = jkey(an, "channels", &c->thrown);
      AxValue samplers = jkey(an, "samplers", &c->thrown);
      if (chans.t == AX_ARR) {
        AxArr *ca = (AxArr *)chans.o;
        for (uint32_t m = 0; m < ca->len && !c->thrown; m++) {
          AxValue ch = ca->items[m];
          if (ch.t == AX_NULL) { c->thrown = true; break; }
          AxValue si = jkey(ch, "sampler", &c->thrown);
          AxValue sm = samplers.t == AX_NULL ? ax_null() : jindex(samplers, si, &c->thrown);
          ax_release(si);
          if (sm.t == AX_NULL) continue;
          AxValue ii = jkey(sm, "input", &c->thrown), oi = jkey(sm, "output", &c->thrown);
          AxValue ia = accessor(c, ii), oa = accessor(c, oi);
          if (ia.t != AX_NULL && oa.t != AX_NULL) {
            Data t = decode(c, ia), v = decode(c, oa);
            if (t.ok && v.ok) {
              AxValue target = jkey(ch, "target", &c->thrown);   // ch.target.node
              if (target.t == AX_NULL) c->thrown = true;
              ax_release(target);
            }
            data_free(&t); data_free(&v);
          }
          ax_release(ia); ax_release(oa); ax_release(ii); ax_release(oi); ax_release(sm);
        }
      }
      ax_release(chans); ax_release(samplers);
    }
  }
  ax_release(anims);
}

AxGlbPrim *ax_glb_parse(const uint8_t *buf, size_t n, int *nprims) {
  *nprims = 0;
  if (n < 12) return NULL;
  if (rd32(buf) != 0x46546C67u || rd32(buf + 4) != 2) return NULL;
  uint64_t total = rd32(buf + 8);
  if (total > n) return NULL;
  uint64_t pos = 12;
  const uint8_t *json = NULL, *bin = NULL;
  size_t jlen = 0, blen = 0;
  bool have_json = false, have_bin = false;
  while (pos + 8 <= total) {
    uint64_t clen = rd32(buf + pos), ctype = rd32(buf + pos + 4);
    uint64_t start = pos + 8, end = start + clen;
    if (start > n) start = n;
    if (end > n) end = n;                                        // Buffer#slice clamps
    if (ctype == 0x4E4F534Au) { json = buf + start; jlen = (size_t)(end - start); have_json = true; }
    else if (ctype == 0x004E4942u) { bin = buf + start; blen = (size_t)(end - start); have_bin = true; }
    pos += 8 + clen;
    while (pos % 4 != 0 && pos < total) pos++;
  }
  if (!have_json || !jlen || !have_bin) return NULL;             // an empty JSON string is falsy
  // Trailing NULs and whitespace are padding some writers leave in the JSON chunk.
  while (jlen && (json[jlen - 1] == 0 || json[jlen - 1] == ' ' || json[jlen - 1] == '\t' || json[jlen - 1] == '\n' ||
                  json[jlen - 1] == '\r' || json[jlen - 1] == '\v' || json[jlen - 1] == '\f')) jlen--;
  char *text = malloc(jlen + 1);
  memcpy(text, json, jlen);
  text[jlen] = '\0';
  if (strlen(text) != jlen) { free(text); return NULL; }        // an interior NUL: JSON.parse fails
  Ctx c = { bin, blen, ax_null(), false };
  bool parsed = ax_json_parse(text, &c.gltf);
  free(text);
  if (!parsed) return NULL;
  AxGlbPrim *prims = NULL;
  int np = 0;
  AxValue meshes = jkey(c.gltf, "meshes", &c.thrown);
  if (meshes.t == AX_ARR && ((AxArr *)meshes.o)->len > 0) {
    AxArr *ma = (AxArr *)meshes.o;
    for (uint32_t m = 0; m < ma->len && !c.thrown; m++) {
      AxValue mesh = ma->items[m];
      if (mesh.t == AX_NULL) { c.thrown = true; break; }
      AxValue ps = jkey(mesh, "primitives", &c.thrown);
      if (ps.t == AX_ARR) {
        AxArr *pa = (AxArr *)ps.o;
        for (uint32_t k = 0; k < pa->len && !c.thrown; k++) {
          AxGlbPrim p;
          if (build(&c, pa->items[k], &p)) {
            prims = realloc(prims, sizeof(AxGlbPrim) * (np + 1));
            prims[np++] = p;
          }
        }
      }
      ax_release(ps);
    }
  }
  ax_release(meshes);
  if (!c.thrown && np > 0) check_skin_and_animations(&c);
  ax_release(c.gltf);
  if (c.thrown || np == 0) {
    ax_glb_free(prims, np);
    return NULL;
  }
  *nprims = np;
  return prims;
}

void ax_glb_free(AxGlbPrim *prims, int n) {
  if (!prims) return;
  for (int i = 0; i < n; i++) {
    free(prims[i].verts);
    free(prims[i].idx);
    ax_release(prims[i].material);
  }
  free(prims);
}
