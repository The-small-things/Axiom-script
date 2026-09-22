// render.c — a small software rasterizer for the native engine.
//
// The JavaScript runtime delegates drawing to render3d.js, which is not part of this code base,
// so there is no reference to port here; this is new code, written to the same contract:
// every frame, `!mesh(#Res, color: 0xRRGGBB)` commands collected from &render blocks are drawn
// through the first &Camera entity (its pose, `~target`, `~fov`) and lit by the first &Light
// (`~dir`, `~intensity`, `~ambient`). The output is an RGBA buffer, which term.c turns into
// terminal frames and write_png() into files.
//
// Meshes are procedural: a #Mesh3D whose path names a sphere/ball, a plane/ground/floor, or
// anything else (a box). Loading .glb geometry is not part of the native build.

#include "axiom.h"
#include "render.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <float.h>

typedef struct { double x, y, z; } P;
static P p3(double x, double y, double z) { P p = { x, y, z }; return p; }
static P padd(P a, P b) { return p3(a.x + b.x, a.y + b.y, a.z + b.z); }
static P psub(P a, P b) { return p3(a.x - b.x, a.y - b.y, a.z - b.z); }
static P pmul(P a, double s) { return p3(a.x * s, a.y * s, a.z * s); }
static double pdot(P a, P b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static P pcross(P a, P b) { return p3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
static P pnorm(P a) { double m = sqrt(pdot(a, a)); return m < 1e-12 ? p3(0, 0, 0) : pmul(a, 1 / m); }
static P pof(AxValue v, P dflt) {
  if (v.t == AX_VEC3) return p3(ax_vecp(v)->x, ax_vecp(v)->y, ax_vecp(v)->z);
  if (v.t == AX_VEC2) return p3(ax_vecp(v)->x, 0, ax_vecp(v)->y);
  if (v.t == AX_NUM) return p3(v.num, v.num, v.num);
  return dflt;
}
static P qrotate(AxValue q, P v) {
  if (q.t != AX_QUAT) return v;
  AxVec *r = ax_vecp(q);
  P u = p3(r->x, r->y, r->z);
  P t = pmul(pcross(u, v), 2);
  return padd(padd(v, pmul(t, r->w)), pcross(u, t));
}

// ---- meshes ----------------------------------------------------------------------------------

typedef struct { P *v; int nv; int *tri; int nt; } Mesh;

static Mesh box_mesh, sphere_mesh, plane_mesh;

static void build_meshes(void) {
  if (box_mesh.nv) return;
  static const double bv[8][3] = { { -.5, -.5, -.5 }, { .5, -.5, -.5 }, { .5, .5, -.5 }, { -.5, .5, -.5 },
                                   { -.5, -.5, .5 }, { .5, -.5, .5 }, { .5, .5, .5 }, { -.5, .5, .5 } };
  static const int bt[12][3] = { { 4, 5, 6 }, { 4, 6, 7 }, { 1, 0, 3 }, { 1, 3, 2 }, { 0, 4, 7 }, { 0, 7, 3 },
                                 { 5, 1, 2 }, { 5, 2, 6 }, { 3, 7, 6 }, { 3, 6, 2 }, { 0, 1, 5 }, { 0, 5, 4 } };
  box_mesh.v = malloc(sizeof(P) * 8);
  for (int i = 0; i < 8; i++) box_mesh.v[i] = p3(bv[i][0], bv[i][1], bv[i][2]);
  box_mesh.nv = 8;
  box_mesh.tri = malloc(sizeof(int) * 36);
  memcpy(box_mesh.tri, bt, sizeof bt);
  box_mesh.nt = 12;

  const int SL = 16, ST = 10;
  sphere_mesh.nv = (SL + 1) * (ST + 1);
  sphere_mesh.v = malloc(sizeof(P) * sphere_mesh.nv);
  for (int j = 0; j <= ST; j++) for (int i = 0; i <= SL; i++) {
    double th = M_PI * j / ST, ph = 2 * M_PI * i / SL;
    sphere_mesh.v[j * (SL + 1) + i] = p3(0.5 * sin(th) * cos(ph), 0.5 * cos(th), 0.5 * sin(th) * sin(ph));
  }
  sphere_mesh.tri = malloc(sizeof(int) * SL * ST * 6);
  for (int j = 0; j < ST; j++) for (int i = 0; i < SL; i++) {
    int a = j * (SL + 1) + i, b = a + 1, c = a + SL + 1, d = c + 1;
    int *t = &sphere_mesh.tri[sphere_mesh.nt * 3];
    t[0] = a; t[1] = b; t[2] = c;
    t[3] = b; t[4] = d; t[5] = c;
    sphere_mesh.nt += 2;
  }

  plane_mesh.v = malloc(sizeof(P) * 4);
  plane_mesh.v[0] = p3(-.5, 0, -.5); plane_mesh.v[1] = p3(.5, 0, -.5);
  plane_mesh.v[2] = p3(.5, 0, .5);   plane_mesh.v[3] = p3(-.5, 0, .5);
  plane_mesh.nv = 4;
  plane_mesh.tri = malloc(sizeof(int) * 6);
  int pt[6] = { 0, 2, 1, 0, 3, 2 };
  memcpy(plane_mesh.tri, pt, sizeof pt);
  plane_mesh.nt = 2;
}

static bool path_has(const char *path, const char *w) {
  char low[256];
  size_t n = strlen(path);
  if (n >= sizeof low) n = sizeof low - 1;
  for (size_t i = 0; i < n; i++) low[i] = (char)((path[i] >= 'A' && path[i] <= 'Z') ? path[i] + 32 : path[i]);
  low[n] = '\0';
  return strstr(low, w) != NULL;
}

static Mesh *mesh_for(AxValue res) {
  build_meshes();
  AxValue path = ax_null();
  if (res.t == AX_DICT) ax_dict_get((AxDict *)res.o, ax_internz("path"), &path);
  Mesh *m = &box_mesh;
  if (path.t == AX_STR) {
    const char *p = ((AxStr *)path.o)->data;
    if (path_has(p, "sphere") || path_has(p, "ball") || path_has(p, "orb")) m = &sphere_mesh;
    else if (path_has(p, "plane") || path_has(p, "ground") || path_has(p, "floor") || path_has(p, "quad")) m = &plane_mesh;
  }
  ax_release(path);
  return m;
}

// ---- frame -----------------------------------------------------------------------------------

typedef struct { int w, h; uint8_t *px; float *z; } Frame;

static void clear(Frame *f) {
  for (int y = 0; y < f->h; y++) {
    double t = (double)y / (f->h > 1 ? f->h - 1 : 1);
    uint8_t r = (uint8_t)(70 + 110 * t), g = (uint8_t)(110 + 90 * t), b = (uint8_t)(170 + 60 * t);
    for (int x = 0; x < f->w; x++) {
      uint8_t *p = f->px + ((size_t)y * f->w + x) * 4;
      p[0] = r; p[1] = g; p[2] = b; p[3] = 255;
      f->z[(size_t)y * f->w + x] = FLT_MAX;
    }
  }
}

typedef struct { double x, y, z; bool ok; } SP;   // screen position, camera-space depth

typedef struct {
  P eye, fwd, right, up;
  double f, aspect, near;
  int w, h;
} Cam;

static SP project(Cam *c, P wp) {
  SP s = { 0, 0, 0, false };
  P d = psub(wp, c->eye);
  double cz = pdot(d, c->fwd);
  if (cz < c->near) return s;
  double cx = pdot(d, c->right), cy = pdot(d, c->up);
  s.x = (cx / cz * c->f / c->aspect * 0.5 + 0.5) * c->w;
  s.y = (0.5 - cy / cz * c->f * 0.5) * c->h;
  s.z = cz;
  s.ok = true;
  return s;
}

static void tri(Frame *fr, SP a, SP b, SP c, uint8_t r, uint8_t g, uint8_t bl) {
  double area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
  if (area >= 0) return;   // back-facing (screen y points down)
  int x0 = (int)floor(fmin(a.x, fmin(b.x, c.x))), x1 = (int)ceil(fmax(a.x, fmax(b.x, c.x)));
  int y0 = (int)floor(fmin(a.y, fmin(b.y, c.y))), y1 = (int)ceil(fmax(a.y, fmax(b.y, c.y)));
  if (x0 < 0) x0 = 0;
  if (y0 < 0) y0 = 0;
  if (x1 > fr->w - 1) x1 = fr->w - 1;
  if (y1 > fr->h - 1) y1 = fr->h - 1;
  for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) {
    double px = x + 0.5, py = y + 0.5;
    double w0 = (b.x - px) * (c.y - py) - (b.y - py) * (c.x - px);
    double w1 = (c.x - px) * (a.y - py) - (c.y - py) * (a.x - px);
    double w2 = (a.x - px) * (b.y - py) - (a.y - py) * (b.x - px);
    if (w0 > 0 || w1 > 0 || w2 > 0) continue;
    w0 /= area; w1 /= area; w2 /= area;
    // Perspective-correct depth: interpolate 1/z.
    double iz = w0 / a.z + w1 / b.z + w2 / c.z;
    float z = (float)(1 / iz);
    size_t i = (size_t)y * fr->w + x;
    if (z >= fr->z[i]) continue;
    fr->z[i] = z;
    uint8_t *p = fr->px + i * 4;
    p[0] = r; p[1] = g; p[2] = bl; p[3] = 255;
  }
}

static bool field(AxEntity *e, const char *name, AxValue *out) { return ax_entity_get(e, ax_internz(name), out); }

static AxEntity *first_with_base(AxVM *vm, const char *base) {
  AxEntity **ents;
  int n = ax_world_entities(vm, &ents);
  for (int i = 0; i < n; i++) if (!ents[i]->pending_remove && ents[i]->base && !strcmp(ents[i]->base->data, base)) return ents[i];
  return NULL;
}

static uint32_t color_of(AxValue v, uint32_t dflt) {
  if (v.t == AX_NUM && v.num >= 0) return (uint32_t)v.num & 0xffffff;
  if (v.t == AX_VEC3) {
    AxVec *c = ax_vecp(v);
    int r = (int)(fmax(0, fmin(1, c->x)) * 255), g = (int)(fmax(0, fmin(1, c->y)) * 255), b = (int)(fmax(0, fmin(1, c->z)) * 255);
    return (uint32_t)(r << 16 | g << 8 | b);
  }
  return dflt;
}

uint8_t *ax_render_frame(AxVM *vm, int w, int h) {
  Frame fr = { w, h, malloc((size_t)w * h * 4), malloc(sizeof(float) * (size_t)w * h) };
  clear(&fr);
  if (!vm->world) { free(fr.z); return fr.px; }

  Cam cam;
  cam.w = w; cam.h = h;
  cam.aspect = (double)w / h;
  cam.near = 0.05;
  double fov = 60;
  cam.eye = p3(0, 6, 12);
  P target = p3(0, 0, 0);
  AxEntity *ce = first_with_base(vm, "Camera");
  if (ce) {
    AxValue pos = ax_entity_pos(ce), t, fv, rot;
    cam.eye = pof(pos, cam.eye);
    ax_release(pos);
    bool has_target = field(ce, "target", &t) && (t.t == AX_VEC3 || t.t == AX_VEC2);
    if (has_target) target = pof(t, target);
    else if (field(ce, "rot", &rot)) { target = padd(cam.eye, qrotate(rot, p3(0, 0, -1))); ax_release(rot); }
    if (has_target) ax_release(t);
    if (field(ce, "fov", &fv)) { if (fv.t == AX_NUM && fv.num > 1) fov = fv.num; ax_release(fv); }
  }
  cam.fwd = pnorm(psub(target, cam.eye));
  if (pdot(cam.fwd, cam.fwd) < 1e-12) cam.fwd = p3(0, 0, -1);
  P worldUp = fabs(cam.fwd.y) > 0.9999 ? p3(0, 0, -1) : p3(0, 1, 0);
  cam.right = pnorm(pcross(cam.fwd, worldUp));
  cam.up = pcross(cam.right, cam.fwd);
  cam.f = 1 / tan(fov * M_PI / 360);

  P light = pnorm(p3(-0.4, -1, -0.3));
  double intensity = 1, ambient = 0.35;
  AxEntity *le = first_with_base(vm, "Light");
  if (le) {
    AxValue v;
    if (field(le, "dir", &v)) { light = pnorm(pof(v, light)); ax_release(v); }
    if (field(le, "intensity", &v)) { if (v.t == AX_NUM) intensity = v.num; ax_release(v); }
    if (field(le, "ambient", &v)) { if (v.t == AX_NUM) ambient = v.num; ax_release(v); }
  }

  extern AxArr *ax_world_draw_list(AxVM *vm);
  AxArr *dl = ax_world_draw_list(vm);
  for (uint32_t i = 0; dl && i < dl->len; i++) {
    AxDict *cmd = (AxDict *)dl->items[i].o;
    AxValue ename, mesh, color, scl;
    if (!ax_dict_get(cmd, ax_internz("entity"), &ename)) continue;
    AxValue ev = ax_engine_resolve_tag(vm, (AxStr *)ename.o);
    ax_release(ename);
    if (ev.t != AX_ENTITY) continue;
    AxEntity *e = (AxEntity *)ev.o;
    if (!ax_dict_get(cmd, ax_internz("mesh"), &mesh)) mesh = ax_null();
    if (!ax_dict_get(cmd, ax_internz("color"), &color)) color = ax_null();
    uint32_t rgb = color_of(color, 0xcccccc);
    AxValue wp;
    ax_entity_get(e, ax_internz("wpose"), &wp);
    P pos = p3(0, 0, 0), s = p3(1, 1, 1);
    AxValue rot = ax_null();
    if (wp.t == AX_XFORM) {
      AxXform *x = (AxXform *)wp.o;
      pos = pof(x->pos, pos);
      s = pof(x->scl, s);
      rot = x->rot;
    }
    if (ax_dict_get(cmd, ax_internz("scl"), &scl)) { s = pof(scl, s); ax_release(scl); }
    Mesh *m = mesh_for(mesh);
    P *world = malloc(sizeof(P) * m->nv);
    SP *screen = malloc(sizeof(SP) * m->nv);
    for (int k = 0; k < m->nv; k++) {
      P v = p3(m->v[k].x * s.x, m->v[k].y * s.y, m->v[k].z * s.z);
      world[k] = padd(qrotate(rot, v), pos);
      screen[k] = project(&cam, world[k]);
    }
    for (int t = 0; t < m->nt; t++) {
      int ia = m->tri[t * 3], ib = m->tri[t * 3 + 1], ic = m->tri[t * 3 + 2];
      if (!screen[ia].ok || !screen[ib].ok || !screen[ic].ok) continue;   // crosses the near plane
      P n = pnorm(pcross(psub(world[ib], world[ia]), psub(world[ic], world[ia])));
      double lit = ambient + intensity * fmax(0, -pdot(n, light));
      if (lit > 1.2) lit = 1.2;
      int r = (int)(((rgb >> 16) & 255) * lit), g = (int)(((rgb >> 8) & 255) * lit), b = (int)((rgb & 255) * lit);
      tri(&fr, screen[ia], screen[ib], screen[ic], (uint8_t)(r > 255 ? 255 : r), (uint8_t)(g > 255 ? 255 : g), (uint8_t)(b > 255 ? 255 : b));
    }
    free(world);
    free(screen);
    ax_release(wp);
    ax_release(mesh);
    ax_release(color);
    ax_release(ev);
  }
  free(fr.z);
  return fr.px;
}

// ---- PNG -------------------------------------------------------------------------------------
// Uncompressed ("stored") deflate keeps the writer dependency-free and a few dozen lines long;
// every PNG reader accepts it.

static uint32_t crc_table[256];
static void crc_init(void) {
  if (crc_table[1]) return;
  for (uint32_t n = 0; n < 256; n++) {
    uint32_t c = n;
    for (int k = 0; k < 8; k++) c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
    crc_table[n] = c;
  }
}
static uint32_t crc(const uint8_t *b, size_t n, uint32_t c) {
  c ^= 0xffffffffu;
  for (size_t i = 0; i < n; i++) c = crc_table[(c ^ b[i]) & 255] ^ (c >> 8);
  return c ^ 0xffffffffu;
}
static void be32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

static void chunk(FILE *f, const char *type, const uint8_t *data, uint32_t n) {
  uint8_t len[4], cb[4];
  be32(len, n);
  fwrite(len, 1, 4, f);
  uint8_t *tmp = malloc(n + 4);
  memcpy(tmp, type, 4);
  if (n) memcpy(tmp + 4, data, n);
  fwrite(tmp, 1, n + 4, f);
  be32(cb, crc(tmp, n + 4, 0));
  fwrite(cb, 1, 4, f);
  free(tmp);
}

bool ax_write_png(const char *path, const uint8_t *rgba, int w, int h) {
  crc_init();
  FILE *f = fopen(path, "wb");
  if (!f) return false;
  static const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
  fwrite(sig, 1, 8, f);
  uint8_t ihdr[13];
  be32(ihdr, (uint32_t)w);
  be32(ihdr + 4, (uint32_t)h);
  ihdr[8] = 8; ihdr[9] = 6; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
  chunk(f, "IHDR", ihdr, 13);
  size_t raw_n = (size_t)h * (w * 4 + 1);
  uint8_t *raw = malloc(raw_n);
  for (int y = 0; y < h; y++) {
    raw[(size_t)y * (w * 4 + 1)] = 0;
    memcpy(raw + (size_t)y * (w * 4 + 1) + 1, rgba + (size_t)y * w * 4, (size_t)w * 4);
  }
  size_t blocks = raw_n / 65535 + 1;
  size_t z_n = 2 + raw_n + blocks * 5 + 4;
  uint8_t *z = malloc(z_n), *p = z;
  *p++ = 0x78; *p++ = 0x01;
  uint32_t s1 = 1, s2 = 0;
  for (size_t off = 0; off < raw_n || off == 0;) {
    size_t n = raw_n - off > 65535 ? 65535 : raw_n - off;
    *p++ = (off + n >= raw_n) ? 1 : 0;
    *p++ = n & 255; *p++ = n >> 8; *p++ = ~n & 255; *p++ = (~n >> 8) & 255;
    memcpy(p, raw + off, n);
    for (size_t i = 0; i < n; i++) { s1 = (s1 + raw[off + i]) % 65521; s2 = (s2 + s1) % 65521; }
    p += n;
    off += n;
    if (!n) break;
  }
  be32(p, (s2 << 16) | s1);
  p += 4;
  chunk(f, "IDAT", z, (uint32_t)(p - z));
  chunk(f, "IEND", NULL, 0);
  fclose(f);
  free(raw);
  free(z);
  return true;
}
