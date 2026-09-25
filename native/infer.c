// infer.c — `$` distributions, NavMesh3D queries, and !save / !load.
//
// Ports of interpreter.js's Distribution, loadNavGrid/aStar/aStarMultiLayer/smoothPath/
// navRaycast, and the save/load actions with their schema check. The A* keeps the reference's
// binary heap and its exact tie-breaking, so the two runtimes find the same path, not merely a
// path of the same length.
//
// One deliberate difference: a particle-filter distribution draws from the program's seeded
// generator (`seed(n)`), where the reference uses Math.random. An `infer: exact` distribution
// uses no randomness and matches the reference exactly.

#include "axiom.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <sys/stat.h>
#include "jsmath.h"

static double hyp2(double a, double b) { double v[2] = { a, b }; return ax_js_hypot(2, v); }
static double hyp3(double a, double b, double c) { double v[3] = { a, b, c }; return ax_js_hypot(3, v); }
static double clampd(double x, double lo, double hi) { return x > hi ? hi : (x < lo ? lo : x); }
static AxStr *K(const char *s) { return ax_internz(s); }   // interned: the table holds it

static AxValue getv(AxValue obj, const char *key) {
  AxValue out = ax_null();
  if (obj.t == AX_DICT) ax_dict_get((AxDict *)obj.o, K(key), &out);
  return out;
}
static void setv(AxDict *d, const char *key, AxValue v) { ax_dict_set(d, K(key), v); }

// =============================================================================================
// Distributions (class Distribution)
// =============================================================================================

typedef struct { int x, y; double w; } Particle;

typedef struct {
  int w, h;
  char prior[32], infer[32];
  bool exact;
  int nparticles;
  Particle *particles;
  int np;
  double *weights;       // exact mode: h*w
  double *mass;          // particle mode: cached mass grid, or NULL
  AxVM *vm;
} Dist;

enum { H_DIST = 3 };

void ax_dist_free(void *p) {
  Dist *d = p;
  free(d->particles);
  free(d->weights);
  free(d->mass);
  free(d);
}

static double rnd(Dist *d) { return ax_rng_next(d->vm); }

static void reset_prior(Dist *d) {
  free(d->mass);
  d->mass = NULL;
  if (d->exact) {
    double p0 = 1.0 / (d->w * d->h);
    for (int i = 0; i < d->w * d->h; i++) d->weights[i] = p0;
    return;
  }
  d->np = d->nparticles;
  d->particles = realloc(d->particles, sizeof(Particle) * (d->np + 1));
  for (int i = 0; i < d->np; i++) {
    d->particles[i].x = (int)floor(rnd(d) * d->w);
    d->particles[i].y = (int)floor(rnd(d) * d->h);
    d->particles[i].w = 1.0 / d->nparticles;
  }
}

static AxHost *dist_host(Dist *d) {
  AxHost *h = calloc(1, sizeof(AxHost));
  h->hdr.rc = 1;
  h->hdr.type = AX_HOST;
  h->kind = H_DIST;
  h->data = d;
  return h;
}

AxValue ax_dist_new(AxVM *vm, AxNode *field) {
  AxNode *shape = field->a, *prior = field->b, *infer = field->c;
  if (!shape || strcmp(shape->str->data, "Grid") != 0)
    ax_throw(vm, "AX-RUNTIME-000", "this reference interpreter only implements Grid(w,h) distributions; got '%s(...)'", shape ? shape->str->data : "?");
  AxScope *scope = ax_scope_enter(vm, vm->globals, false);
  double args[2] = { 0, 0 };
  for (int i = 0; i < 2 && i < shape->nlist; i++) { AxValue v = ax_eval(vm, shape->list[i]->b, scope); args[i] = ax_to_num(v); ax_release(v); }
  double np = 64;
  if (infer && infer->nlist) { AxValue v = ax_eval(vm, infer->list[0]->b, scope); np = ax_to_num(v); ax_release(v); }
  if (prior) for (int i = 0; i < prior->nlist; i++) { AxValue v = ax_eval(vm, prior->list[i]->b, scope); ax_release(v); }
  ax_scope_exit(vm, scope);
  Dist *d = calloc(1, sizeof(Dist));
  d->vm = vm;
  d->w = (int)round(args[0]);
  d->h = (int)round(args[1]);
  snprintf(d->prior, sizeof d->prior, "%s", prior ? prior->str->data : "Uniform");
  snprintf(d->infer, sizeof d->infer, "%s", infer ? infer->str->data : "particle");
  d->exact = strcmp(d->infer, "exact") == 0;
  d->nparticles = infer && infer->nlist ? (int)round(np) : 64;
  if (d->w < 0) d->w = 0;
  if (d->h < 0) d->h = 0;
  d->weights = d->exact ? calloc((size_t)d->w * d->h + 1, sizeof(double)) : NULL;
  reset_prior(d);
  AxValue v; v.t = AX_HOST; v.o = (AxObj *)dist_host(d);
  return v;
}

static Dist *as_dist(AxValue v) {
  if (v.t != AX_HOST) return NULL;
  AxHost *h = (AxHost *)v.o;
  return h->kind == H_DIST ? h->data : NULL;
}

static double cell_likelihood(int x, int y, AxValue obs, bool has_self, double sx, double sy) {
  double lik = 1;
  AxValue noise = getv(obs, "noise");
  if (noise.t == AX_NUM && has_self) {
    double cx = x + 0.5, cy = y + 0.5;
    double dist = hyp2(cx - sx, cy - sy);
    double predicted = clampd(1 - dist / 8, 0, 1);
    double err = noise.num - predicted;
    lik *= exp(-(err * err) / (2 * 0.18 * 0.18));
  }
  AxValue vision = getv(obs, "vision");
  if (ax_truthy(vision)) {
    AxValue seen = getv(vision, "seenCell");
    bool is_seen = false;
    if (seen.t == AX_DICT) {
      AxValue a = getv(seen, "x"), b = getv(seen, "y");
      is_seen = ax_to_num(a) == x && ax_to_num(b) == y;
      ax_release(a); ax_release(b);
    }
    bool in_visible = false;
    if (!is_seen) {
      AxValue cells = getv(vision, "visibleCells");
      if (cells.t == AX_ARR) {
        AxArr *arr = (AxArr *)cells.o;
        for (uint32_t i = 0; i < arr->len && !in_visible; i++) {
          AxValue a = getv(arr->items[i], "x"), b = getv(arr->items[i], "y");
          in_visible = ax_to_num(a) == x && ax_to_num(b) == y;
          ax_release(a); ax_release(b);
        }
      }
      ax_release(cells);
    }
    if (is_seen) lik *= 40;
    else if (in_visible) lik *= 0.03;
    ax_release(seen);
  }
  ax_release(vision);
  ax_release(noise);
  return lik;
}

static void dist_update(Dist *d, AxValue obs, AxValue self_pos) {
  free(d->mass);
  d->mass = NULL;
  bool has_self = ax_truthy(self_pos) || self_pos.t >= AX_VEC2;
  double sx = 0, sy = 0;
  if (self_pos.t == AX_VEC2 || self_pos.t == AX_VEC3) { sx = ax_vecp(self_pos)->x; sy = ax_vecp(self_pos)->y; }
  if (d->exact) {
    double sum = 0;
    for (int y = 0; y < d->h; y++) for (int x = 0; x < d->w; x++) {
      d->weights[y * d->w + x] *= cell_likelihood(x, y, obs, has_self, sx, sy);
      sum += d->weights[y * d->w + x];
    }
    double p0 = 1.0 / (d->w * d->h);
    for (int i = 0; i < d->w * d->h; i++) d->weights[i] = sum < 1e-12 ? p0 : d->weights[i] / sum;
    return;
  }
  static const int dirs[5][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 }, { 0, 0 } };
  for (int i = 0; i < d->np; i++) {
    if (rnd(d) < 0.12) {
      const int *dd = dirs[(int)floor(rnd(d) * 5)];
      d->particles[i].x = (int)clampd(d->particles[i].x + dd[0], 0, d->w - 1);
      d->particles[i].y = (int)clampd(d->particles[i].y + dd[1], 0, d->h - 1);
    }
  }
  for (int i = 0; i < d->np; i++) d->particles[i].w *= cell_likelihood(d->particles[i].x, d->particles[i].y, obs, has_self, sx, sy);
  double sum = 0;
  for (int i = 0; i < d->np; i++) sum += d->particles[i].w;
  for (int i = 0; i < d->np; i++) d->particles[i].w = sum < 1e-12 ? 1.0 / d->nparticles : d->particles[i].w / sum;
  // Systematic resampling.
  int N = d->nparticles;
  if (N == 0 || d->np == 0) return;
  Particle *out = malloc(sizeof(Particle) * N);
  double step = 1.0 / N, u0 = rnd(d) * step;
  int i = 0;
  double c = d->particles[0].w;
  for (int m = 0; m < N; m++) {
    double U = u0 + m * step;
    while (U > c && i < N - 1) { i++; c += d->particles[i].w; }
    out[m].x = d->particles[i].x;
    out[m].y = d->particles[i].y;
    out[m].w = step;
  }
  free(d->particles);
  d->particles = out;
  d->np = N;
}

static const double *mass_grid(Dist *d) {
  if (d->exact) return d->weights;
  if (d->mass) return d->mass;
  d->mass = calloc((size_t)d->w * d->h + 1, sizeof(double));
  for (int i = 0; i < d->np; i++) d->mass[d->particles[i].y * d->w + d->particles[i].x] += d->particles[i].w;
  return d->mass;
}

static AxValue cell(int x, int y) {
  AxDict *c = ax_dict_new();
  setv(c, "x", ax_num(x));
  setv(c, "y", ax_num(y));
  setv(c, "__cell", ax_bool(true));
  return ax_dictv(c);
}

AxValue ax_dist_infer(AxVM *vm, AxValue v, AxStr *op) {
  Dist *d = as_dist(v);
  if (!d) ax_throw(vm, "AX-RUNTIME-000", "'~> %s' used on a non-distribution value", op->data);
  if (!strcmp(op->data, "argmax")) {
    const double *g = mass_grid(d);
    int bx = 0, by = 0;
    double bv = -1;
    for (int y = 0; y < d->h; y++) for (int x = 0; x < d->w; x++) if (g[y * d->w + x] > bv) { bv = g[y * d->w + x]; bx = x; by = y; }
    return cell(bx, by);
  }
  if (!strcmp(op->data, "sample")) {
    double r = rnd(d), acc = 0;
    if (d->exact) {
      for (int y = 0; y < d->h; y++) for (int x = 0; x < d->w; x++) {
        acc += d->weights[y * d->w + x];
        if (r <= acc) return cell(x, y);
      }
      return cell(d->w - 1, d->h - 1);
    }
    for (int i = 0; i < d->np; i++) { acc += d->particles[i].w; if (r <= acc) return cell(d->particles[i].x, d->particles[i].y); }
    return cell(d->particles[d->np - 1].x, d->particles[d->np - 1].y);
  }
  ax_throw(vm, "AX-RUNTIME-000", "unknown infer op '%s' (supported: argmax, sample)", op->data);
  return ax_null();
}

AxValue ax_dist_mass_at(AxVM *vm, AxHost *h, AxValue c) {
  Dist *d = h->data;
  const double *g = mass_grid(d);
  AxValue cx = getv(c, "x"), cy = getv(c, "y");
  double x = round(ax_to_num(cx)), y = round(ax_to_num(cy));
  ax_release(cx); ax_release(cy);
  if (!(y >= 0 && y < d->h && x >= 0 && x < d->w)) return ax_num(0);
  return ax_num(g[(int)y * d->w + (int)x]);
}

bool ax_dist_member(AxVM *vm, AxHost *h, AxStr *prop, AxValue *out) {
  Dist *d = h->data;
  const char *p = prop->data;
  *out = ax_null();
  if (!strcmp(p, "w")) *out = ax_num(d->w);
  else if (!strcmp(p, "h")) *out = ax_num(d->h);
  else if (!strcmp(p, "mode")) *out = ax_str_from(d->exact ? "exact" : "particle");
  else if (!strcmp(p, "numParticles")) *out = ax_num(d->nparticles);
  return true;
}

bool ax_dist_method(AxVM *vm, AxHost *h, AxStr *name, AxValue *args, int argc, AxValue *out) {
  (void)vm;
  if (strcmp(name->data, "any") != 0) return false;
  Dist *d = h->data;
  // `belief.any(>0.3)` — the predicate operator arrives as the argument's `op`; the evaluated
  // arguments carry only the value, so it is recovered by the caller (see ax_dist_any).
  double value = argc ? ax_to_num(args[0]) : 0;
  const double *g = mass_grid(d);
  bool r = false;
  for (int i = 0; i < d->w * d->h && !r; i++) r = g[i] == value;
  *out = ax_bool(r);
  return true;
}

// `dist.any(>0.3)`: needs the operator from the argument node, so the interpreter asks here
// before evaluating the call generically.
bool ax_dist_any(AxVM *vm, AxValue obj, AxNode *call, AxScope *scope, AxValue *out) {
  Dist *d = as_dist(obj);
  if (!d || strcmp(call->str->data, "any") != 0 || call->nlist < 1) return false;
  AxNode *a = call->list[0];
  int op = a->op ? a->op : OP_EQ;
  AxValue v = ax_eval(vm, a->b, scope);
  double value = ax_to_num(v);
  ax_release(v);
  const double *g = mass_grid(d);
  bool r = false;
  for (int i = 0; i < d->w * d->h && !r; i++) {
    double m = g[i];
    switch (op) {
      case OP_GT: r = m > value; break;
      case OP_LT: r = m < value; break;
      case OP_GE: r = m >= value; break;
      case OP_LE: r = m <= value; break;
      case OP_NE: r = m != value; break;
      default: r = m == value; break;
    }
  }
  *out = ax_bool(r);
  return true;
}

// `belief ~= observe(noise: audio.ambient, vision: v)` — a named argument whose expression is a
// dotted path published with ^emit reads the channel; otherwise it is evaluated.
void ax_dist_observe(AxVM *vm, AxNode *n, AxScope *scope) {
  AxValue target = ax_eval(vm, &(AxNode){ .kind = N_IDENT, .str = n->str }, scope);
  Dist *d = as_dist(target);
  if (!d) {
    ax_release(target);
    ax_throw(vm, "AX-RUNTIME-KERNEL", "'~=' target '%s' is not a distribution", n->str->data);
  }
  AxNode *call = n->a;
  if (call->kind != N_CALL || strcmp(call->str->data, "observe") != 0) {
    ax_release(target);
    ax_throw(vm, "AX-RUNTIME-000", "'~=' currently only supports 'observe(...)' on the right-hand side");
  }
  AxDict *obs = ax_dict_new();
  AxDict *channels = ax_world_channels(vm);
  for (int i = 0; i < call->nlist; i++) {
    AxNode *a = call->list[i];
    if (!a->str) continue;
    // flattenPath: a name or a chain of member reads.
    char path[256] = "";
    AxNode *cur = a->b;
    AxStr *parts[16];
    int np = 0;
    while (cur && cur->kind == N_MEMBER && np < 15) { parts[np++] = cur->str; cur = cur->a; }
    AxValue v = ax_null();
    bool found = false;
    if (cur && cur->kind == N_IDENT && channels) {
      snprintf(path, sizeof path, "%s", cur->str->data);
      for (int k = np - 1; k >= 0; k--) { strncat(path, ".", sizeof path - strlen(path) - 1); strncat(path, parts[k]->data, sizeof path - strlen(path) - 1); }
      AxStr *key = ax_internz(path);
      found = ax_dict_get(channels, key, &v);
      ax_release(ax_strv(key));
    }
    if (!found) v = ax_eval(vm, a->b, scope);
    ax_dict_set(obs, a->str, v);
  }
  AxValue pos = vm->ctx.entity ? ax_entity_pos(vm->ctx.entity) : ax_vec3(0, 0, 0);
  dist_update(d, ax_dictv(obs), pos);
  ax_release(pos);
  ax_release(ax_dictv(obs));
  ax_release(target);
}

// JSON.stringify of a Distribution: its own properties, in construction order.
typedef struct { char *buf; size_t len, cap; } JB;
static void jb(JB *b, const char *s) { ax_str_append(&b->buf, &b->len, &b->cap, s, strlen(s)); }
static void pad(JB *b, int indent, int depth) { if (!indent) return; jb(b, "\n"); for (int i = 0; i < indent * depth; i++) jb(b, " "); }
static void num(JB *b, double d) { char t[64]; if (isnan(d) || isinf(d)) { jb(b, "null"); return; } ax_fmt_num(d, t, sizeof t); jb(b, t); }
static void key(JB *b, const char *k, int indent) { jb(b, "\""); jb(b, k); jb(b, indent ? "\": " : "\":"); }

static void grid_json(JB *b, const double *g, int w, int h, int indent, int depth) {
  if (!h) { jb(b, "[]"); return; }
  jb(b, "[");
  for (int y = 0; y < h; y++) {
    if (y) jb(b, ",");
    pad(b, indent, depth + 1);
    if (!w) { jb(b, "[]"); continue; }
    jb(b, "[");
    for (int x = 0; x < w; x++) { if (x) jb(b, ","); pad(b, indent, depth + 2); num(b, g[y * w + x]); }
    pad(b, indent, depth + 1);
    jb(b, "]");
  }
  pad(b, indent, depth);
  jb(b, "]");
}

void ax_dist_json(AxHost *h, void *jbp, int indent, int depth, bool sim) {
  (void)sim;
  JB *b = jbp;
  Dist *d = h->data;
  jb(b, "{");
  pad(b, indent, depth + 1); key(b, "w", indent); num(b, d->w); jb(b, ",");
  pad(b, indent, depth + 1); key(b, "h", indent); num(b, d->h); jb(b, ",");
  pad(b, indent, depth + 1); key(b, "priorName", indent); jb(b, "\""); jb(b, d->prior); jb(b, "\",");
  pad(b, indent, depth + 1); key(b, "inferName", indent); jb(b, "\""); jb(b, d->infer); jb(b, "\",");
  pad(b, indent, depth + 1); key(b, "mode", indent); jb(b, d->exact ? "\"exact\"," : "\"particle\",");
  pad(b, indent, depth + 1); key(b, "numParticles", indent); num(b, d->nparticles); jb(b, ",");
  pad(b, indent, depth + 1); key(b, "particles", indent);
  if (d->exact || !d->np) jb(b, "[]");
  else {
    jb(b, "[");
    for (int i = 0; i < d->np; i++) {
      if (i) jb(b, ",");
      pad(b, indent, depth + 2);
      jb(b, "{");
      pad(b, indent, depth + 3); key(b, "x", indent); num(b, d->particles[i].x); jb(b, ",");
      pad(b, indent, depth + 3); key(b, "y", indent); num(b, d->particles[i].y); jb(b, ",");
      pad(b, indent, depth + 3); key(b, "w", indent); num(b, d->particles[i].w);
      pad(b, indent, depth + 2);
      jb(b, "}");
    }
    pad(b, indent, depth + 1);
    jb(b, "]");
  }
  jb(b, ",");
  pad(b, indent, depth + 1); key(b, "weights", indent);
  if (d->exact) grid_json(b, d->weights, d->w, d->h, indent, depth + 1); else jb(b, "null");
  jb(b, ",");
  pad(b, indent, depth + 1); key(b, "_massCache", indent);
  if (!d->exact && d->mass) grid_json(b, d->mass, d->w, d->h, indent, depth + 1); else jb(b, "null");
  pad(b, indent, depth);
  jb(b, "}");
}

// =============================================================================================
// NavMesh3D (loadNavGrid / aStar / aStarMultiLayer / smoothPath / navRaycast)
// =============================================================================================

typedef struct { unsigned char *grid; int w, h; double y; } Layer;
typedef struct { int ax, az; double ay; int bx, bz; double by; } Conn;
typedef struct { char *path; bool ok; Layer *layers; int nlayers; Conn *conns; int nconns; } Nav;

static Nav *nav_cache;
static int nav_count;

static char *trim(char *s) {
  while (*s == ' ' || *s == '\t' || *s == '\r') s++;
  size_t n = strlen(s);
  while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r')) s[--n] = '\0';
  return s;
}

static bool parse_grid(char **lines, int nlines, int *i, Layer *L) {
  int w = 0, h = 0;
  if (*i >= nlines || sscanf(lines[*i], "%d %d", &w, &h) != 2 || w <= 0 || h <= 0) return false;
  (*i)++;
  if (*i + h > nlines) return false;
  L->w = w; L->h = h;
  L->grid = calloc((size_t)w * h, 1);
  for (int y = 0; y < h; y++) {
    const char *row = lines[*i + y];
    size_t rl = strlen(row);
    for (int x = 0; x < w; x++) L->grid[y * w + x] = ((size_t)x < rl && (row[x] == '1' || row[x] == '#')) ? 1 : 0;
  }
  *i += h;
  return true;
}

static Nav *load_nav(AxVM *vm, AxValue source) {
  if (source.t != AX_STR) return NULL;
  const char *path = ((AxStr *)source.o)->data;
  for (int i = 0; i < nav_count; i++) if (!strcmp(nav_cache[i].path, path)) return nav_cache[i].ok ? &nav_cache[i] : NULL;
  nav_cache = realloc(nav_cache, sizeof(Nav) * (nav_count + 1));
  Nav *nav = &nav_cache[nav_count++];
  memset(nav, 0, sizeof *nav);
  nav->path = strdup(path);
  if (vm->sandbox) return NULL;
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *text = malloc((size_t)n + 1);
  size_t got = fread(text, 1, (size_t)n, f);
  text[got] = '\0';
  fclose(f);
  char **lines = NULL;
  int nlines = 0;
  for (char *save = NULL, *l = strtok_r(text, "\n", &save); l; l = strtok_r(NULL, "\n", &save)) {
    char *t = trim(l);
    // A '#' line is a comment unless it is a grid row (only '#', '.', '0', '1').
    if (!*t || (*t == '#' && strspn(t, "#.01") != strlen(t))) continue;
    lines = realloc(lines, sizeof(char *) * (nlines + 1));
    lines[nlines++] = t;
  }
  if (nlines >= 1) {
    if (!strncmp(lines[0], "layers", 6)) {
      int want = 0;
      if (sscanf(lines[0], "layers %d", &want) == 1 && want > 0) {
        int i = 1;
        while (i < nlines && nav->nlayers < want) {
          int idx;
          double y;
          if (sscanf(lines[i], "layer %d y=%lf", &idx, &y) != 2) { i++; continue; }
          i++;
          if (i >= nlines) break;
          Layer L;
          int save_i = i;
          if (!parse_grid(lines, nlines, &i, &L)) { i = save_i + 1; continue; }
          L.y = y;
          nav->layers = realloc(nav->layers, sizeof(Layer) * (nav->nlayers + 1));
          nav->layers[nav->nlayers++] = L;
        }
        for (; i < nlines; i++) {
          Conn c;
          if (sscanf(lines[i], "connect (%d,%d,%lf) (%d,%d,%lf)", &c.ax, &c.az, &c.ay, &c.bx, &c.bz, &c.by) == 6) {
            nav->conns = realloc(nav->conns, sizeof(Conn) * (nav->nconns + 1));
            nav->conns[nav->nconns++] = c;
          }
        }
        nav->ok = nav->nlayers > 0;
      }
    } else {
      int i = 0;
      Layer L;
      if (parse_grid(lines, nlines, &i, &L)) {
        L.y = 0;
        nav->layers = malloc(sizeof(Layer));
        nav->layers[0] = L;
        nav->nlayers = 1;
        nav->ok = true;
      }
    }
  }
  free(lines);
  // `text` backs nothing after parsing; the grids are copies.
  free(text);
  return nav->ok ? nav : NULL;
}

// The reference's array-backed binary min-heap, comparisons and swaps identical, so ties break
// the same way and the same path comes out.
typedef struct { int li, x, y; double g, f; } HNode;
typedef struct { HNode *d; int n, cap; } Heap;

static void heap_push(Heap *h, HNode node) {
  if (h->n == h->cap) { h->cap = h->cap ? h->cap * 2 : 64; h->d = realloc(h->d, sizeof(HNode) * h->cap); }
  int i = h->n++;
  h->d[i] = node;
  while (i > 0) {
    int parent = (i - 1) >> 1;
    if (h->d[i].f < h->d[parent].f) { HNode t = h->d[i]; h->d[i] = h->d[parent]; h->d[parent] = t; i = parent; }
    else break;
  }
}

static HNode heap_pop(Heap *h) {
  HNode top = h->d[0];
  HNode last = h->d[--h->n];
  if (h->n > 0) {
    h->d[0] = last;
    int i = 0, n = h->n;
    for (;;) {
      int s = i, l = 2 * i + 1, r = 2 * i + 2;
      if (l < n && h->d[l].f < h->d[s].f) s = l;
      if (r < n && h->d[r].f < h->d[s].f) s = r;
      if (s != i) { HNode t = h->d[i]; h->d[i] = h->d[s]; h->d[s] = t; i = s; }
      else break;
    }
  }
  return top;
}

typedef struct { double x, y, z; } P3;

static AxValue smooth_path(P3 *path, int n) {
  AxArr *out = ax_arr_new(n);
  if (n < 3) {
    for (int i = 0; i < n; i++) ax_arr_push(out, ax_vec3(path[i].x, path[i].y, path[i].z));
    return ax_arrv(out);
  }
  P3 *kept = malloc(sizeof(P3) * n);
  int k = 0;
  kept[k++] = path[0];
  for (int i = 1; i < n - 1; i++) {
    P3 a = kept[k - 1], b = path[i], c = path[i + 1];
    double xz = (b.x - a.x) * (c.z - b.z) - (b.z - a.z) * (c.x - b.x);
    bool ych = fabs(b.y - a.y) > 1e-6 || fabs(c.y - b.y) > 1e-6;
    if (fabs(xz) > 1e-6 || ych) kept[k++] = b;
  }
  kept[k++] = path[n - 1];
  for (int i = 0; i < k; i++) ax_arr_push(out, ax_vec3(kept[i].x, kept[i].y, kept[i].z));
  free(kept);
  return ax_arrv(out);
}

static const double DIRS[8][3] = { { 1, 0, 1 }, { -1, 0, 1 }, { 0, 1, 1 }, { 0, -1, 1 }, { 1, 1, 1.414 }, { -1, 1, 1.414 }, { 1, -1, 1.414 }, { -1, -1, 1.414 } };

static double clampi(double v, double lo, double hi) { return fmax(lo, fmin(hi, v)); }

static int closest_layer(Nav *nav, double y) {
  int best = 0;
  double bd = INFINITY;
  for (int i = 0; i < nav->nlayers; i++) { double d = fabs(nav->layers[i].y - y); if (d < bd) { bd = d; best = i; } }
  return best;
}

// One A* over every layer (a single layer is the common case). Nodes are (layer, x, z); layers
// join through `connect` bridges.
static AxValue astar(Nav *nav, AxValue from, AxValue to) {
  bool multi = nav->nlayers > 1 || nav->nconns > 0;
  double fx = ax_vecp(from)->x, fy = from.t == AX_VEC3 ? ax_vecp(from)->y : 0, fz = from.t == AX_VEC3 ? ax_vecp(from)->z : NAN;
  double tx = ax_vecp(to)->x, ty = to.t == AX_VEC3 ? ax_vecp(to)->y : 0, tz = to.t == AX_VEC3 ? ax_vecp(to)->z : NAN;
  int sl = multi ? closest_layer(nav, fy) : 0, el = multi ? closest_layer(nav, ty) : 0;
  Layer *S = &nav->layers[sl], *E = &nav->layers[el];
  double sxd = clampi(floor(fx), 0, S->w - 1), szd = clampi(floor(fz), 0, S->h - 1);
  double exd = clampi(floor(tx), 0, E->w - 1), ezd = clampi(floor(tz), 0, E->h - 1);
  if (isnan(sxd) || isnan(szd) || isnan(exd) || isnan(ezd)) return ax_null();
  int sx = (int)sxd, sz = (int)szd, ex = (int)exd, ez = (int)ezd;
  if (S->grid[sz * S->w + sx] == 1 || E->grid[ez * E->w + ex] == 1) return ax_null();
  // Per-layer offsets into flat per-cell arrays.
  int *off = malloc(sizeof(int) * (nav->nlayers + 1));
  int total = 0;
  for (int i = 0; i < nav->nlayers; i++) { off[i] = total; total += nav->layers[i].w * nav->layers[i].h; }
  double *g = malloc(sizeof(double) * total);
  bool *has_g = calloc(total, 1);
  int *came = malloc(sizeof(int) * total);
  for (int i = 0; i < total; i++) came[i] = -1;
  // Bridges, per cell: a small adjacency list.
  typedef struct { int li, x, z; double cost; } Br;
  Br **br = calloc(total, sizeof(Br *));
  int *nbr = calloc(total, sizeof(int));
  for (int c = 0; c < nav->nconns; c++) {
    Conn *cn = &nav->conns[c];
    int la = closest_layer(nav, cn->ay), lb = closest_layer(nav, cn->by);
    Layer *A = &nav->layers[la], *B = &nav->layers[lb];
    int ax = (int)clampi(cn->ax, 0, A->w - 1), az = (int)clampi(cn->az, 0, A->h - 1);
    int bx = (int)clampi(cn->bx, 0, B->w - 1), bz = (int)clampi(cn->bz, 0, B->h - 1);
    if (A->grid[az * A->w + ax] == 1 || B->grid[bz * B->w + bx] == 1) continue;
    double cost = hyp3(bx - ax, B->y - A->y, bz - az);
    int ka = off[la] + az * A->w + ax, kb = off[lb] + bz * B->w + bx;
    br[ka] = realloc(br[ka], sizeof(Br) * (nbr[ka] + 1)); br[ka][nbr[ka]++] = (Br){ lb, bx, bz, cost };
    br[kb] = realloc(br[kb], sizeof(Br) * (nbr[kb] + 1)); br[kb][nbr[kb]++] = (Br){ la, ax, az, cost };
  }
  double eY = E->y;
  #define HEUR(li, x, z) (multi ? hyp3(ex + 0.5 - (x) - 0.5, nav->layers[li].y - eY, ez + 0.5 - (z) - 0.5) : hyp2(ex - (x), ez - (z)))
  Heap open = { 0 };
  heap_push(&open, (HNode){ sl, sx, sz, 0, HEUR(sl, sx, sz) });
  g[off[sl] + sz * S->w + sx] = 0;
  has_g[off[sl] + sz * S->w + sx] = true;
  int max_iters = total * 4, iters = 0;
  AxValue result = ax_null();
  while (open.n > 0 && iters < max_iters) {
    iters++;
    HNode cur = heap_pop(&open);
    if (cur.li == el && cur.x == ex && cur.y == ez) {
      P3 *path = NULL;
      int np = 0, cap = 0;
      int cl = cur.li, cx = cur.x, cz = cur.y;
      for (;;) {
        if (np == cap) { cap = cap ? cap * 2 : 32; path = realloc(path, sizeof(P3) * cap); }
        path[np++] = (P3){ cx + 0.5, multi ? nav->layers[cl].y : 0, cz + 0.5 };
        if (cl == sl && cx == sx && cz == sz) break;
        int k = came[off[cl] + cz * nav->layers[cl].w + cx];
        if (k < 0) break;
        int li = 0;
        while (li + 1 < nav->nlayers && off[li + 1] <= k) li++;
        int rel = k - off[li];
        cl = li; cx = rel % nav->layers[li].w; cz = rel / nav->layers[li].w;
      }
      for (int i = 0; i < np / 2; i++) { P3 t = path[i]; path[i] = path[np - 1 - i]; path[np - 1 - i] = t; }
      result = smooth_path(path, np);
      free(path);
      break;
    }
    Layer *L = &nav->layers[cur.li];
    for (int d = 0; d < 8; d++) {
      int dx = (int)DIRS[d][0], dz = (int)DIRS[d][1];
      int nx = cur.x + dx, nz = cur.y + dz;
      if (nx < 0 || nx >= L->w || nz < 0 || nz >= L->h) continue;
      if (L->grid[nz * L->w + nx] == 1) continue;
      if (dx != 0 && dz != 0 && (L->grid[cur.y * L->w + nx] == 1 || L->grid[nz * L->w + cur.x] == 1)) continue;
      double tg = cur.g + DIRS[d][2];
      int k = off[cur.li] + nz * L->w + nx;
      if (has_g[k] && g[k] <= tg) continue;
      g[k] = tg; has_g[k] = true;
      came[k] = off[cur.li] + cur.y * L->w + cur.x;
      heap_push(&open, (HNode){ cur.li, nx, nz, tg, tg + HEUR(cur.li, nx, nz) });
    }
    int ck = off[cur.li] + cur.y * L->w + cur.x;
    for (int b = 0; b < nbr[ck]; b++) {
      Br *e = &br[ck][b];
      double tg = cur.g + e->cost;
      int k = off[e->li] + e->z * nav->layers[e->li].w + e->x;
      if (has_g[k] && g[k] <= tg) continue;
      g[k] = tg; has_g[k] = true;
      came[k] = ck;
      heap_push(&open, (HNode){ e->li, e->x, e->z, tg, tg + HEUR(e->li, e->x, e->z) });
    }
  }
  #undef HEUR
  for (int i = 0; i < total; i++) free(br[i]);
  free(br); free(nbr); free(g); free(has_g); free(came); free(off); free(open.d);
  return result;
}

static int test_cell(Nav *nav, bool multi, int cx, int cz, double y) {
  if (multi) {
    Layer *best = NULL;
    double bd = INFINITY;
    for (int i = 0; i < nav->nlayers; i++) {
      Layer *l = &nav->layers[i];
      if (cx < 0 || cx >= l->w || cz < 0 || cz >= l->h) continue;
      double d = fabs(l->y - y);
      if (d < bd) { bd = d; best = l; }
    }
    return best ? best->grid[cz * best->w + cx] : 0;
  }
  Layer *l = &nav->layers[0];
  if (cx < 0 || cx >= l->w || cz < 0 || cz >= l->h) return -1;
  return l->grid[cz * l->w + cx];
}

static bool in_any(Nav *nav, bool multi, int cx, int cz) {
  if (!multi) { Layer *l = &nav->layers[0]; return !(cx < 0 || cx >= l->w || cz < 0 || cz >= l->h); }
  for (int i = 0; i < nav->nlayers; i++) { Layer *l = &nav->layers[i]; if (cx >= 0 && cx < l->w && cz >= 0 && cz < l->h) return true; }
  return false;
}

static AxValue nav_raycast(Nav *nav, AxValue origin, AxValue dir, double max_dist) {
  if (origin.t != AX_VEC3 || dir.t != AX_VEC3) return ax_null();
  AxVec *o = ax_vecp(origin), *dv = ax_vecp(dir);
  double dx = dv->x, dz = dv->z, dmag = hyp2(dx, dz);
  if (dmag < 1e-9) return ax_null();
  double ndx = dx / dmag, ndz = dz / dmag;
  bool multi = nav->nlayers > 1;
  int cx = (int)floor(o->x), cz = (int)floor(o->z);
  if (!in_any(nav, multi, cx, cz)) return ax_null();
  if (test_cell(nav, multi, cx, cz, o->y) == 1) return ax_vec3(o->x, o->y, o->z);
  int stepX = ndx > 0 ? 1 : ndx < 0 ? -1 : 0, stepZ = ndz > 0 ? 1 : ndz < 0 ? -1 : 0;
  double tMaxX, tMaxZ, tDX, tDZ;
  if (stepX) { double nb = stepX > 0 ? cx + 1 : cx; tMaxX = (nb - o->x) / ndx; tDX = fabs(1 / ndx); } else { tMaxX = INFINITY; tDX = INFINITY; }
  if (stepZ) { double nb = stepZ > 0 ? cz + 1 : cz; tMaxZ = (nb - o->z) / ndz; tDZ = fabs(1 / ndz); } else { tMaxZ = INFINITY; tDZ = INFINITY; }
  double t = 0, maxT = max_dist / dmag;
  int total = 0;
  for (int i = 0; i < nav->nlayers; i++) total += nav->layers[i].w * nav->layers[i].h;
  int max_iters = multi ? total * 2 : nav->layers[0].w * nav->layers[0].h * 2;
  for (int it = 0; it < max_iters; it++) {
    if (tMaxX < tMaxZ) { cx += stepX; t = tMaxX; tMaxX += tDX; }
    else { cz += stepZ; t = tMaxZ; tMaxZ += tDZ; }
    if (t > maxT) return ax_null();
    double wt = t / dmag;
    double cy = o->y + dv->y * wt;
    if (!in_any(nav, multi, cx, cz)) return ax_null();
    if (test_cell(nav, multi, cx, cz, cy) == 1) return ax_vec3(o->x + dv->x * wt, o->y + dv->y * wt, o->z + dv->z * wt);
  }
  return ax_null();
}

AxValue ax_nav_query(AxVM *vm, AxEntity *navent, AxStr *name, AxValue *args, int argc) {
  AxValue src;
  if (!ax_dict_get(navent->fields, K("source"), &src)) src = ax_null();
  Nav *nav = load_nav(vm, src);
  bool has_src = ax_truthy(src);
  ax_release(src);
  const char *q = name->data;
  AxValue a0 = argc > 0 ? args[0] : ax_null(), a1 = argc > 1 ? args[1] : ax_null();
  if (!strcmp(q, "path")) {
    if (has_src && nav && (a0.t == AX_VEC3 || a0.t == AX_VEC2) && (a1.t == AX_VEC3 || a1.t == AX_VEC2)) {
      AxValue p = astar(nav, a0, a1);
      if (p.t != AX_NULL) return p;
    }
    AxArr *out = ax_arr_new(2);
    ax_arr_push(out, ax_copy(a0));
    ax_arr_push(out, ax_copy(a1));
    return ax_arrv(out);
  }
  if (!strcmp(q, "raycast")) {
    double md = (argc > 2 && args[2].t == AX_NUM) ? args[2].num : 100;
    if (!has_src || !nav) return ax_null();
    return nav_raycast(nav, a0, a1, md);
  }
  bool isb = !strcmp(q, "is_blocked");
  if (!has_src || !nav) return isb ? ax_bool(false) : ax_null();
  Layer *layer = &nav->layers[0];
  if (nav->nlayers > 1 && a0.t == AX_VEC3) layer = &nav->layers[closest_layer(nav, ax_vecp(a0)->y)];
  double cx, cz;
  if (a0.t == AX_NUM && a1.t == AX_NUM) { cx = a0.num; cz = a1.num; }
  else if (a0.t == AX_VEC3) { cx = ax_vecp(a0)->x; cz = ax_vecp(a0)->z; }
  else return isb ? ax_bool(false) : ax_null();
  cx = floor(cx); cz = floor(cz);
  if (cx < 0 || cx >= layer->w || cz < 0 || cz >= layer->h) return isb ? ax_bool(false) : ax_null();
  int idx = (int)cz * layer->w + (int)cx;
  if (!strcmp(q, "block_cell")) { layer->grid[idx] = 1; return ax_null(); }
  if (!strcmp(q, "unblock_cell")) { layer->grid[idx] = 0; return ax_null(); }
  return ax_bool(layer->grid[idx] == 1);
}

// =============================================================================================
// !save / !load
// =============================================================================================

static int cmp_str(const void *a, const void *b) {
  return strcmp((*(AxStr *const *)a)->data, (*(AxStr *const *)b)->data);
}

// {axiomVersion, entities: {name: [sorted field names]}} — the fingerprint a save carries.
static AxValue compute_schema(AxVM *vm) {
  AxEntity **ents;
  int n = ax_world_entities(vm, &ents);
  AxDict *entities = ax_dict_new();
  for (int i = 0; i < n; i++) {
    AxEntity *e = ents[i];
    if (e->nosave || e->pending_remove) continue;
    AxStr **names = malloc(sizeof(AxStr *) * (e->fields->len + 1));
    int k = 0;
    for (uint32_t j = 0; j < e->fields->len; j++) if (!e->fields->entries[j].dead) names[k++] = e->fields->entries[j].key;
    qsort(names, k, sizeof(AxStr *), cmp_str);
    AxArr *arr = ax_arr_new(k);
    for (int j = 0; j < k; j++) { ax_retain(ax_strv(names[j])); ax_arr_push(arr, ax_strv(names[j])); }
    free(names);
    ax_dict_set(entities, e->name, ax_arrv(arr));
  }
  AxDict *s = ax_dict_new();
  const char *v = ax_world_version(vm);
  setv(s, "axiomVersion", ax_str_from(v ? v : "unknown"));
  setv(s, "entities", ax_dictv(entities));
  return ax_dictv(s);
}

static bool arr_has(AxValue arr, AxStr *s) {
  if (arr.t != AX_ARR) return false;
  AxArr *a = (AxArr *)arr.o;
  for (uint32_t i = 0; i < a->len; i++) if (a->items[i].t == AX_STR && ax_str_eq((AxStr *)a->items[i].o, s)) return true;
  return false;
}

static AxStr **sorted_keys(AxDict *d, int *n) {
  AxStr **k = malloc(sizeof(AxStr *) * (d->len + 1));
  *n = 0;
  for (uint32_t i = 0; i < d->len; i++) if (!d->entries[i].dead) k[(*n)++] = d->entries[i].key;
  qsort(k, *n, sizeof(AxStr *), cmp_str);
  return k;
}

// compareSaveSchemas: the first difference, as (kind, detail) — or false when they match.
static bool compare_schemas(AxValue saved, AxValue current, char *kind, char *msg, size_t msgn, const char *slot) {
  AxValue sv = getv(saved, "axiomVersion"), cv = getv(current, "axiomVersion");
  bool diff_version = !ax_equals(sv, cv);
  if (diff_version) {
    AxStr *a = ax_to_str(sv), *b = ax_to_str(cv);
    strcpy(kind, "version");
    snprintf(msg, msgn, "!load(\"%s\"): save was written with axiom version '%s' but the current program is version '%s'.", slot, a->data, b->data);
    ax_release(ax_strv(a)); ax_release(ax_strv(b));
  }
  ax_release(sv); ax_release(cv);
  if (diff_version) return true;
  AxValue se = getv(saved, "entities"), ce = getv(current, "entities");
  bool found = false;
  if (se.t == AX_DICT && ce.t == AX_DICT) {
    int ns, nc;
    AxStr **sk = sorted_keys((AxDict *)se.o, &ns), **ck = sorted_keys((AxDict *)ce.o, &nc);
    for (int i = 0; i < ns && !found; i++) if (!ax_dict_has((AxDict *)ce.o, sk[i])) {
      strcpy(kind, "entity_removed");
      snprintf(msg, msgn, "!load(\"%s\"): save references entity '%s' which no longer exists in the current program.", slot, sk[i]->data);
      found = true;
    }
    for (int i = 0; i < nc && !found; i++) if (!ax_dict_has((AxDict *)se.o, ck[i])) {
      strcpy(kind, "entity_added");
      snprintf(msg, msgn, "!load(\"%s\"): current program has entity '%s' which is not in the save (will keep its default field values).", slot, ck[i]->data);
      found = true;
    }
    for (int i = 0; i < ns && !found; i++) {
      AxValue sf, cf;
      ax_dict_get((AxDict *)se.o, sk[i], &sf);
      ax_dict_get((AxDict *)ce.o, sk[i], &cf);
      if (sf.t == AX_ARR) for (uint32_t j = 0; j < ((AxArr *)sf.o)->len && !found; j++) {
        AxStr *f = (AxStr *)((AxArr *)sf.o)->items[j].o;
        if (!arr_has(cf, f)) {
          strcpy(kind, "field_removed");
          snprintf(msg, msgn, "!load(\"%s\"): save has field '~%s' on entity '%s' which no longer exists in the current program.", slot, f->data, sk[i]->data);
          found = true;
        }
      }
      if (cf.t == AX_ARR) for (uint32_t j = 0; j < ((AxArr *)cf.o)->len && !found; j++) {
        AxStr *f = (AxStr *)((AxArr *)cf.o)->items[j].o;
        if (!arr_has(sf, f)) {
          strcpy(kind, "field_added");
          snprintf(msg, msgn, "!load(\"%s\"): entity '%s' has field '~%s' in the current program which is not in the save (will keep its default value).", slot, sk[i]->data, f->data);
          found = true;
        }
      }
      ax_release(sf); ax_release(cf);
    }
    free(sk); free(ck);
  }
  ax_release(se); ax_release(ce);
  return found;
}

static AxValue v3_tagged(const char *tag, AxValue v) {
  AxDict *d = ax_dict_new();
  setv(d, tag, ax_bool(true));
  setv(d, "x", ax_num(ax_vecp(v)->x));
  setv(d, "y", ax_num(ax_vecp(v)->y));
  if (v.t == AX_VEC3) setv(d, "z", ax_num(ax_vecp(v)->z));
  return ax_dictv(d);
}

static AxValue nums(AxValue v, int n) {
  AxArr *a = ax_arr_new(n);
  AxVec *p = (v.t >= AX_VEC2 && v.t <= AX_QUAT) ? ax_vecp(v) : NULL;
  double xs[4] = { p ? p->x : NAN, p ? p->y : NAN, p ? p->z : NAN, p ? p->w : NAN };
  for (int i = 0; i < n; i++) ax_arr_push(a, ax_num(xs[i]));
  return ax_arrv(a);
}

// Fields are kept by reference (as in the reference: a later mutation of a saved array shows up
// in the save); a pose is snapshotted to plain numbers.
static AxValue snapshot(AxVM *vm) {
  AxEntity **ents;
  int n = ax_world_entities(vm, &ents);
  AxDict *state = ax_dict_new();
  for (int i = 0; i < n; i++) {
    AxEntity *e = ents[i];
    if (e->nosave || e->pending_remove) continue;
    AxDict *fields = ax_dict_new(), *locals = ax_dict_new();
    for (uint32_t j = 0; j < e->fields->len; j++) {
      if (e->fields->entries[j].dead) continue;
      ax_dict_set(fields, e->fields->entries[j].key, ax_copy(e->fields->entries[j].val));
    }
    for (uint32_t j = 0; j < e->locals->len; j++) {
      if (e->locals->entries[j].dead) continue;
      AxValue v = e->locals->entries[j].val;
      AxValue s;
      if (v.t == AX_XFORM) {
        AxXform *x = (AxXform *)v.o;
        AxDict *t = ax_dict_new();
        setv(t, "__transform", ax_bool(true));
        setv(t, "pos", nums(x->pos, 3));
        setv(t, "rot", nums(x->rot, 4));
        setv(t, "scl", nums(x->scl, 3));
        setv(t, "vel", nums(x->vel, 3));
        s = ax_dictv(t);
      } else if (v.t == AX_VEC2) s = v3_tagged("__v2", v);
      else if (v.t == AX_VEC3) s = v3_tagged("__v3", v);
      else if (v.t == AX_ATOM) { AxDict *t = ax_dict_new(); setv(t, "__atom", ax_bool(true)); setv(t, "name", ax_strv((AxStr *)v.o)); ax_retain(v); s = ax_dictv(t); }
      else s = ax_copy(v);
      ax_dict_set(locals, e->locals->entries[j].key, s);
    }
    AxDict *es = ax_dict_new();
    setv(es, "fields", ax_dictv(fields));
    setv(es, "locals", ax_dictv(locals));
    ax_dict_set(state, e->name, ax_dictv(es));
  }
  AxDict *wrapped = ax_dict_new();
  setv(wrapped, "__axiomSchema", compute_schema(vm));
  setv(wrapped, "entities", ax_dictv(state));
  return ax_dictv(wrapped);
}

static bool contains_entity(AxValue v, int depth) {
  if (depth > 32) return false;
  if (v.t == AX_ENTITY || v.t == AX_FN || v.t == AX_BIG) return true;   // JSON.stringify would throw — skip the file
  if (v.t == AX_ARR) { AxArr *a = (AxArr *)v.o; for (uint32_t i = 0; i < a->len; i++) if (contains_entity(a->items[i], depth + 1)) return true; }
  if (v.t == AX_DICT) { AxDict *d = (AxDict *)v.o; for (uint32_t i = 0; i < d->len; i++) if (!d->entries[i].dead && contains_entity(d->entries[i].val, depth + 1)) return true; }
  return false;
}

static void restore(AxVM *vm, AxValue state) {
  if (state.t != AX_DICT) return;
  AxEntity **ents;
  int n = ax_world_entities(vm, &ents);
  for (int i = 0; i < n; i++) {
    AxEntity *e = ents[i];
    AxValue s;
    if (!ax_dict_get((AxDict *)state.o, e->name, &s)) continue;
    AxValue fields = getv(s, "fields"), locals = getv(s, "locals");
    if (fields.t == AX_DICT) {
      AxDict *f = (AxDict *)fields.o;
      for (uint32_t j = 0; j < f->len; j++) {
        if (f->entries[j].dead) continue;
        AxValue v = f->entries[j].val;
        AxValue flag = getv(v, "__timer");
        if (ax_truthy(flag)) {
          AxValue rem = getv(v, "remaining"), total = getv(v, "total"), unit = getv(v, "unit");
          AxStr *us = unit.t == AX_STR ? (AxStr *)unit.o : NULL;
          AxValue t = ax_timer_new(ax_to_num(rem), us ? us->data : "s");
          ((AxTimer *)t.o)->total = ax_to_num(total);
          ax_dict_set(e->fields, f->entries[j].key, t);
          ax_release(rem); ax_release(total); ax_release(unit);
        } else {
          ax_dict_set(e->fields, f->entries[j].key, ax_copy(v));
        }
        ax_release(flag);
      }
    }
    if (locals.t == AX_DICT) {
      AxDict *l = (AxDict *)locals.o;
      for (uint32_t j = 0; j < l->len; j++) {
        if (l->entries[j].dead) continue;
        AxValue v = l->entries[j].val;
        AxValue r;
        AxValue tf = getv(v, "__transform"), v2 = getv(v, "__v2"), v3 = getv(v, "__v3"), at = getv(v, "__atom");
        if (ax_truthy(tf)) {
          r = ax_xform_new();
          AxXform *x = (AxXform *)r.o;
          const char *keys[4] = { "pos", "rot", "scl", "vel" };
          AxValue *slots[4] = { &x->pos, &x->rot, &x->scl, &x->vel };
          for (int k = 0; k < 4; k++) {
            AxValue arr = getv(v, keys[k]);
            double c[4] = { 0, 0, 0, 0 };
            if (arr.t == AX_ARR) for (uint32_t q = 0; q < ((AxArr *)arr.o)->len && q < 4; q++) c[q] = ax_to_num(((AxArr *)arr.o)->items[q]);
            ax_release(*slots[k]);
            *slots[k] = k == 1 ? ax_quat(c[0], c[1], c[2], c[3]) : ax_vec3(c[0], c[1], c[2]);
            ax_release(arr);
          }
          x->entity = e;
        } else if (ax_truthy(v2)) {
          AxValue a = getv(v, "x"), b = getv(v, "y");
          r = ax_vec2(ax_to_num(a), ax_to_num(b));
          ax_release(a); ax_release(b);
        } else if (ax_truthy(v3)) {
          AxValue a = getv(v, "x"), b = getv(v, "y"), c = getv(v, "z");
          r = ax_vec3(ax_to_num(a), ax_to_num(b), ax_to_num(c));
          ax_release(a); ax_release(b); ax_release(c);
        } else if (ax_truthy(at)) {
          AxValue nm = getv(v, "name");
          AxStr *s2 = ax_to_str(nm);
          r = ax_atom(ax_intern(s2->data, s2->len));
          ax_release(ax_strv(s2));
          ax_release(nm);
        } else {
          r = ax_copy(v);
        }
        ax_release(tf); ax_release(v2); ax_release(v3); ax_release(at);
        ax_dict_set(e->locals, l->entries[j].key, r);
      }
    }
    ax_release(fields); ax_release(locals); ax_release(s);
  }
}

void ax_save_action(AxVM *vm, AxNode *n, AxScope *scope, bool load) {
  AxValue slotv = n->nlist ? ax_eval(vm, n->list[0]->b, scope) : ax_str_from("default");
  AxStr *slot = ax_to_str(slotv);
  ax_release(slotv);
  AxDict *slots = ax_world_saves(vm);
  AxStr *key = ax_intern(slot->data, slot->len);
  char path[1024];
  snprintf(path, sizeof path, "saves/%s.json", slot->data);
  if (!load) {
    AxValue wrapped = snapshot(vm);
    ax_dict_set(slots, key, ax_copy(wrapped));
    if (!vm->sandbox) {
      // Best effort, like the reference: saves/<slot>.json, pretty-printed. The directory is
      // made first, so it exists even when the state cannot be written as JSON.
      mkdir("saves", 0777);
      if (!contains_entity(wrapped, 0)) {
        char *json = NULL;
        ax_json_write(wrapped, 2, &json);
        FILE *f = fopen(path, "wb");
        if (f) { fputs(json, f); fclose(f); }
        free(json);
      }
    }
    ax_release(wrapped);
  } else {
    AxValue wrapped;
    bool have = ax_dict_get(slots, key, &wrapped);
    if (!have && !vm->sandbox) {
      FILE *f = fopen(path, "rb");
      if (f) {
        fseek(f, 0, SEEK_END);
        long len = ftell(f);
        fseek(f, 0, SEEK_SET);
        char *text = malloc((size_t)len + 1);
        size_t got = fread(text, 1, (size_t)len, f);
        text[got] = '\0';
        fclose(f);
        if (ax_json_parse(text, &wrapped)) { have = true; ax_dict_set(slots, key, ax_copy(wrapped)); }
        free(text);
      }
    }
    if (have) {
      AxValue schema = getv(wrapped, "__axiomSchema");
      AxValue state;
      bool go = true;
      if (ax_truthy(schema)) {
        AxValue current = compute_schema(vm);
        char kind[32], msg[512];
        if (compare_schemas(schema, current, kind, msg, sizeof msg, slot->data)) {
          bool advisory = !strcmp(kind, "entity_added") || !strcmp(kind, "field_added");
          ax_world_diag(vm, vm->ctx.entity, vm->ctx.block, "AX-SAVE-001", advisory ? "advisory" : "fatal", "Save/Load Schema Mismatch", msg);
          if (!advisory) go = false;
        }
        ax_release(current);
        state = getv(wrapped, "entities");
      } else {
        char msg[512];
        snprintf(msg, sizeof msg, "!load(\"%s\"): Save file has no schema stamp (pre-v0.8.1 format). Loading without verification.", slot->data);
        ax_world_diag(vm, vm->ctx.entity, vm->ctx.block, "AX-SAVE-001", "advisory", "Save/Load Schema Mismatch", msg);
        state = ax_copy(wrapped);
      }
      if (go) restore(vm, state);
      ax_release(state);
      ax_release(schema);
      ax_release(wrapped);
    }
  }
  ax_release(ax_strv(key));
  ax_release(ax_strv(slot));
}
