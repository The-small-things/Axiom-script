// value.c — the value model: refcounting, strings, arrays, dicts, ranges, functions.
//
// Two decisions shape everything here.
//
// 1. Numbers are doubles, exactly as in the JavaScript reference. A native implementation that
//    used 64-bit integers would disagree with the reference on overflow, division, and NaN, and
//    a program that behaves differently on the two runtimes is worse than a slower one.
//
// 2. Dictionaries preserve insertion order. `keys()`, `items()` and iteration are observable,
//    programs depend on their order, and the reference implementation inherits JavaScript's
//    insertion order — so the native one has to reproduce it rather than pick its own.

#include "axiom.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

// ---------------------------------------------------------------------------------------------
// Allocation helpers
// ---------------------------------------------------------------------------------------------

static void *xalloc(size_t n) {
  void *p = calloc(1, n);
  if (!p) { fprintf(stderr, "axiom: out of memory\n"); exit(70); }
  return p;
}

static void *xrealloc(void *p, size_t n) {
  void *q = realloc(p, n);
  if (!q) { fprintf(stderr, "axiom: out of memory\n"); exit(70); }
  return q;
}

uint32_t ax_hash_bytes(const char *p, size_t n) {
  // FNV-1a, matching the `hash()` intrinsic so both runtimes agree.
  uint32_t h = 0x811c9dc5u;
  for (size_t i = 0; i < n; i++) { h ^= (uint8_t)p[i]; h *= 0x01000193u; }
  return h;
}

// ---------------------------------------------------------------------------------------------
// Interning
// ---------------------------------------------------------------------------------------------
//
// Every identifier, dictionary key, and field name is interned, so scope lookups and dict
// probes compare pointers instead of bytes. The table owns one reference to each entry and is
// never shrunk; identifiers in a program are a bounded set.

typedef struct {
  AxStr **slots;
  uint32_t cap, len;
} InternTable;

static InternTable g_intern;

static void intern_grow(void) {
  uint32_t ncap = g_intern.cap ? g_intern.cap * 2 : 1024;
  AxStr **ns = xalloc(sizeof(AxStr *) * ncap);
  for (uint32_t i = 0; i < g_intern.cap; i++) {
    AxStr *s = g_intern.slots[i];
    if (!s) continue;
    uint32_t j = s->hash & (ncap - 1);
    while (ns[j]) j = (j + 1) & (ncap - 1);
    ns[j] = s;
  }
  free(g_intern.slots);
  g_intern.slots = ns;
  g_intern.cap = ncap;
}

static AxStr *str_alloc(const char *data, size_t len) {
  AxStr *s = xalloc(sizeof(AxStr) + len + 1);
  s->hdr.rc = 1;
  s->hdr.type = AX_STR;
  s->len = (uint32_t)len;
  if (len && data) memcpy(s->data, data, len);
  s->data[len] = '\0';
  s->hash = ax_hash_bytes(s->data, len);
  s->interned = false;
  return s;
}

AxStr *ax_str_new(const char *data, size_t len) { return str_alloc(data, len); }
AxStr *ax_str_newz(const char *cstr) { return str_alloc(cstr, strlen(cstr)); }

AxStr *ax_intern(const char *data, size_t len) {
  if (!g_intern.cap) intern_grow();
  if (g_intern.len * 4 >= g_intern.cap * 3) intern_grow();
  uint32_t h = ax_hash_bytes(data, len);
  uint32_t i = h & (g_intern.cap - 1);
  while (g_intern.slots[i]) {
    AxStr *s = g_intern.slots[i];
    if (s->hash == h && s->len == len && memcmp(s->data, data, len) == 0) {
      s->hdr.rc++;
      return s;
    }
    i = (i + 1) & (g_intern.cap - 1);
  }
  AxStr *s = str_alloc(data, len);
  s->interned = true;
  s->hdr.rc = 2;   // one reference held by the table, one returned to the caller
  g_intern.slots[i] = s;
  g_intern.len++;
  return s;
}

AxStr *ax_internz(const char *cstr) { return ax_intern(cstr, strlen(cstr)); }

// ---------------------------------------------------------------------------------------------
// Refcounting
// ---------------------------------------------------------------------------------------------

static void obj_free(AxObj *o);

void ax_retain(AxValue v) {
  if (ax_is_obj(v) && v.o) v.o->rc++;
}

void ax_release(AxValue v) {
  if (!ax_is_obj(v) || !v.o) return;
  if (--v.o->rc == 0) obj_free(v.o);
}

static void obj_free(AxObj *o) {
  switch (o->type) {
    case AX_STR: {
      AxStr *s = (AxStr *)o;
      if (s->interned) { s->hdr.rc = 1; return; }   // the intern table keeps it alive
      free(s);
      return;
    }
    case AX_ARR: {
      AxArr *a = (AxArr *)o;
      for (uint32_t i = 0; i < a->len; i++) ax_release(a->items[i]);
      free(a->items);
      free(a);
      return;
    }
    case AX_DICT: {
      AxDict *d = (AxDict *)o;
      for (uint32_t i = 0; i < d->len; i++) {
        if (d->entries[i].dead) continue;
        AxValue kv; kv.t = AX_STR; kv.o = (AxObj *)d->entries[i].key;
        ax_release(kv);
        ax_release(d->entries[i].val);
      }
      free(d->entries);
      free(d->index);
      if (d->type_tag) { AxValue tv; tv.t = AX_STR; tv.o = (AxObj *)d->type_tag; ax_release(tv); }
      free(d);
      return;
    }
    case AX_FN: {
      AxFn *f = (AxFn *)o;
      if (f->entity) { AxValue ev; ev.t = AX_ENTITY; ev.o = (AxObj *)f->entity; ax_release(ev); }
      if (f->native) { if (f->has_bound) ax_release(f->bound); }
      if (!f->native) {
        for (int i = 0; i < f->nparams; i++) {
          AxValue pv; pv.t = AX_STR; pv.o = (AxObj *)f->params[i];
          ax_release(pv);
        }
        free(f->params);
        free(f->defaults);
        if (f->scope) ax_scope_release(f->scope);
        if (f->has_bound) ax_release(f->bound);
      }
      free(f);
      return;
    }
    case AX_RANGE: free(o); return;
    case AX_XFORM: {
      AxXform *x = (AxXform *)o;
      ax_release(x->pos); ax_release(x->rot); ax_release(x->scl); ax_release(x->vel);
      free(x);
      return;
    }
    case AX_SHAPE: {
      AxShape *sh = (AxShape *)o;
      AxValue kv; kv.t = AX_STR; kv.o = (AxObj *)sh->kind;
      ax_release(kv);
      for (int i = 0; i < sh->nparams; i++) ax_release(sh->params[i]);
      free(sh);
      return;
    }
    case AX_ENTITY: {
      AxEntity *e = (AxEntity *)o;
      ax_release(ax_dictv(e->fields));
      ax_release(ax_dictv(e->locals));
      ax_release(e->patrol);
      free(e);
      return;
    }
    case AX_HOST: ax_host_free((AxHost *)o); return;
    default: free(o); return;
  }
}

// ---------------------------------------------------------------------------------------------
// Strings
// ---------------------------------------------------------------------------------------------

AxValue ax_strv(AxStr *s) { AxValue v; v.t = AX_STR; v.o = (AxObj *)s; return v; }
AxValue ax_atom(AxStr *s) { AxValue v; v.t = AX_ATOM; v.o = (AxObj *)s; return v; }
AxValue ax_str_from(const char *cstr) { return ax_strv(ax_str_newz(cstr)); }
AxValue ax_atomz(const char *cstr) { return ax_atom(ax_internz(cstr)); }

AxStr *ax_str_concat(AxStr *a, AxStr *b) {
  AxStr *s = xalloc(sizeof(AxStr) + a->len + b->len + 1);
  s->hdr.rc = 1;
  s->hdr.type = AX_STR;
  s->len = a->len + b->len;
  memcpy(s->data, a->data, a->len);
  memcpy(s->data + a->len, b->data, b->len);
  s->data[s->len] = '\0';
  s->hash = ax_hash_bytes(s->data, s->len);
  return s;
}

bool ax_str_eq(const AxStr *a, const AxStr *b) {
  if (a == b) return true;
  if (!a || !b) return false;
  return a->len == b->len && a->hash == b->hash && memcmp(a->data, b->data, a->len) == 0;
}

// ---------------------------------------------------------------------------------------------
// Arrays
// ---------------------------------------------------------------------------------------------

AxArr *ax_arr_new(uint32_t cap) {
  AxArr *a = xalloc(sizeof(AxArr));
  a->hdr.rc = 1;
  a->hdr.type = AX_ARR;
  a->cap = cap;
  if (cap) a->items = xalloc(sizeof(AxValue) * cap);
  return a;
}

AxValue ax_arrv(AxArr *a) { AxValue v; v.t = AX_ARR; v.o = (AxObj *)a; return v; }

void ax_arr_push(AxArr *a, AxValue v) {
  if (a->len == a->cap) {
    a->cap = a->cap ? a->cap * 2 : 8;
    a->items = xrealloc(a->items, sizeof(AxValue) * a->cap);
  }
  a->items[a->len++] = v;
}

AxValue ax_arr_get(AxArr *a, int64_t i) {
  if (i < 0 || (uint64_t)i >= a->len) return ax_null();
  return ax_copy(a->items[i]);
}

void ax_arr_set(AxArr *a, int64_t i, AxValue v) {
  if (i < 0) { ax_release(v); return; }
  while ((uint64_t)i >= a->len) ax_arr_push(a, ax_null());   // grow with nulls, as JS does
  ax_release(a->items[i]);
  a->items[i] = v;
}

// ---------------------------------------------------------------------------------------------
// Dictionaries — insertion-ordered, with a hash index once they grow
// ---------------------------------------------------------------------------------------------

#define DICT_INDEX_MIN 8

AxDict *ax_dict_new(void) {
  AxDict *d = xalloc(sizeof(AxDict));
  d->hdr.rc = 1;
  d->hdr.type = AX_DICT;
  return d;
}

AxValue ax_dictv(AxDict *d) { AxValue v; v.t = AX_DICT; v.o = (AxObj *)d; return v; }

static void dict_reindex(AxDict *d) {
  uint32_t cap = DICT_INDEX_MIN;
  while (cap < d->len * 2) cap *= 2;
  free(d->index);
  d->index = xalloc(sizeof(int32_t) * cap);
  for (uint32_t i = 0; i < cap; i++) d->index[i] = -1;
  d->index_cap = cap;
  for (uint32_t e = 0; e < d->len; e++) {
    if (d->entries[e].dead) continue;
    uint32_t i = d->entries[e].key->hash & (cap - 1);
    while (d->index[i] >= 0) i = (i + 1) & (cap - 1);
    d->index[i] = (int32_t)e;
  }
}

static int32_t dict_find(const AxDict *d, const AxStr *key) {
  if (!d->index) {
    for (uint32_t i = 0; i < d->len; i++) {
      if (!d->entries[i].dead && ax_str_eq(d->entries[i].key, key)) return (int32_t)i;
    }
    return -1;
  }
  uint32_t i = key->hash & (d->index_cap - 1);
  while (d->index[i] >= 0) {
    int32_t e = d->index[i];
    if (!d->entries[e].dead && ax_str_eq(d->entries[e].key, key)) return e;
    i = (i + 1) & (d->index_cap - 1);
  }
  return -1;
}

static bool key_as_index(const AxStr *k, uint32_t *out) {
  if (!k->len || k->len > 10) return false;
  if (k->data[0] == '0' && k->len > 1) return false;   // "01" is not canonical
  uint64_t v = 0;
  for (uint32_t i = 0; i < k->len; i++) {
    if (k->data[i] < '0' || k->data[i] > '9') return false;
    v = v * 10 + (uint64_t)(k->data[i] - '0');
    if (v > 4294967294ull) return false;
  }
  *out = (uint32_t)v;
  return true;
}

// KEY ORDER.
//
// The reference implementation stores dictionaries as JavaScript objects, and JavaScript's own
// key order is observable to every program that prints one or iterates it: array-index keys
// (canonical non-negative integers) come first in ascending numeric order, and every other key
// follows in insertion order. `group_by(xs, \n: n % 2)` builds exactly such a dictionary, so a
// native runtime that used plain insertion order would print different output for the same
// program.
//
// Rather than sorting on every iteration, the entries array is kept in that order as keys are
// added: a string key appends (the common case, O(1)); an integer-like key is placed among the
// leading integer-like keys, which costs a shift but is rare.
void ax_dict_set(AxDict *d, AxStr *key, AxValue v) {
  int32_t e = dict_find(d, key);
  if (e >= 0) {
    ax_release(d->entries[e].val);
    d->entries[e].val = v;
    return;
  }
  if (d->len == d->cap) {
    d->cap = d->cap ? d->cap * 2 : 8;
    d->entries = xrealloc(d->entries, sizeof(AxDictEntry) * d->cap);
    memset(d->entries + d->len, 0, sizeof(AxDictEntry) * (d->cap - d->len));
  }
  AxValue kv; kv.t = AX_STR; kv.o = (AxObj *)key;
  ax_retain(kv);

  uint32_t idx_val;
  uint32_t pos = d->len;
  bool shifted = false;
  if (key_as_index(key, &idx_val)) {
    pos = 0;
    while (pos < d->len) {
      if (d->entries[pos].dead) { pos++; continue; }
      uint32_t other;
      if (!key_as_index(d->entries[pos].key, &other)) break;   // string keys start here
      if (other > idx_val) break;
      pos++;
    }
    if (pos < d->len) {
      memmove(d->entries + pos + 1, d->entries + pos, sizeof(AxDictEntry) * (d->len - pos));
      shifted = true;
    }
  }
  d->entries[pos].key = key;
  d->entries[pos].val = v;
  d->entries[pos].dead = false;
  d->len++;
  d->live++;
  if (shifted) { dict_reindex(d); return; }   // every index after `pos` moved
  if (d->len >= DICT_INDEX_MIN && (!d->index || d->len * 2 > d->index_cap)) dict_reindex(d);
  else if (d->index) {
    uint32_t i = key->hash & (d->index_cap - 1);
    while (d->index[i] >= 0) i = (i + 1) & (d->index_cap - 1);
    d->index[i] = (int32_t)pos;
  }
}

bool ax_dict_get(AxDict *d, const AxStr *key, AxValue *out) {
  int32_t e = dict_find(d, key);
  if (e < 0) return false;
  *out = ax_copy(d->entries[e].val);
  return true;
}

bool ax_dict_has(AxDict *d, const AxStr *key) { return dict_find(d, key) >= 0; }

void ax_dict_del(AxDict *d, const AxStr *key) {
  int32_t e = dict_find(d, key);
  if (e < 0) return;
  AxValue kv; kv.t = AX_STR; kv.o = (AxObj *)d->entries[e].key;
  ax_release(kv);
  ax_release(d->entries[e].val);
  d->entries[e].dead = true;
  d->entries[e].key = NULL;
  d->entries[e].val = ax_null();
  d->live--;
  if (d->index) dict_reindex(d);
}

uint32_t ax_dict_count(const AxDict *d) { return d->live; }

// ---------------------------------------------------------------------------------------------
// Functions and ranges
// ---------------------------------------------------------------------------------------------

AxValue ax_native(const char *name, AxNativeFn fn, int min_args, int max_args) {
  AxFn *f = xalloc(sizeof(AxFn));
  f->hdr.rc = 1;
  f->hdr.type = AX_FN;
  f->native = true;
  f->fn = fn;
  f->name = name;
  f->min_args = min_args;
  f->max_args = max_args;
  AxValue v; v.t = AX_FN; v.o = (AxObj *)f;
  return v;
}

AxValue ax_fnv(AxFn *f) { AxValue v; v.t = AX_FN; v.o = (AxObj *)f; return v; }

AxValue ax_range(double lo, double hi, double step) {
  AxRange *r = xalloc(sizeof(AxRange));
  r->hdr.rc = 1;
  r->hdr.type = AX_RANGE;
  r->lo = lo; r->hi = hi;
  r->step = (step == 0 || isnan(step)) ? 1 : step;
  AxValue v; v.t = AX_RANGE; v.o = (AxObj *)r;
  return v;
}

bool ax_is_callable(AxValue v) { return v.t == AX_FN; }

// ---------------------------------------------------------------------------------------------
// Predicates, comparison, rendering
// ---------------------------------------------------------------------------------------------

bool ax_truthy(AxValue v) {
  switch (v.t) {
    case AX_NULL: return false;
    case AX_BOOL: return v.b;
    case AX_NUM:  return v.num != 0 && !isnan(v.num);
    case AX_STR:  return ((AxStr *)v.o)->len > 0;
    case AX_ARR:  return ((AxArr *)v.o)->len > 0;
    // A vector is "true" when it is meaningfully non-zero, as in the reference — which is what
    // makes `?input.move:` read as "is the stick being pushed".
    case AX_VEC2: { AxVec *q = (AxVec *)v.o; return hypot(q->x, q->y) > 0.1; }
    case AX_VEC3: { AxVec *q = (AxVec *)v.o; return sqrt(q->x * q->x + q->y * q->y + q->z * q->z) > 0.1; }
    default:      return true;   // atoms, dicts, functions, ranges, engine objects
  }
}

static bool equals_depth(AxValue a, AxValue b, int depth) {
  if (a.t != b.t) return false;
  switch (a.t) {
    case AX_NULL: return true;
    case AX_BOOL: return a.b == b.b;
    case AX_NUM:  return a.num == b.num;
    case AX_STR:
    case AX_ATOM: return ax_str_eq((AxStr *)a.o, (AxStr *)b.o);
    case AX_FN:   return a.o == b.o;
    case AX_VEC2: { AxVec *x = (AxVec *)a.o, *y = (AxVec *)b.o; return x->x == y->x && x->y == y->y; }
    case AX_VEC3: { AxVec *x = (AxVec *)a.o, *y = (AxVec *)b.o; return x->x == y->x && x->y == y->y && x->z == y->z; }
    case AX_QUAT: { AxVec *x = (AxVec *)a.o, *y = (AxVec *)b.o; return x->x == y->x && x->y == y->y && x->z == y->z && x->w == y->w; }
    case AX_MAT4: case AX_XFORM: case AX_TIMER: case AX_SHAPE: case AX_ENTITY: case AX_HOST:
      return a.o == b.o;
    case AX_RANGE: {
      AxRange *x = (AxRange *)a.o, *y = (AxRange *)b.o;
      return x->lo == y->lo && x->hi == y->hi && x->step == y->step;
    }
    case AX_ARR: {
      // Structural, like the reference implementation: [1,2] == [1,2].
      if (a.o == b.o) return true;
      if (depth > 32) return false;
      AxArr *x = (AxArr *)a.o, *y = (AxArr *)b.o;
      if (x->len != y->len) return false;
      for (uint32_t i = 0; i < x->len; i++) if (!equals_depth(x->items[i], y->items[i], depth + 1)) return false;
      return true;
    }
    case AX_DICT: {
      if (a.o == b.o) return true;
      if (depth > 32) return false;
      AxDict *x = (AxDict *)a.o, *y = (AxDict *)b.o;
      if (ax_dict_count(x) != ax_dict_count(y)) return false;
      for (uint32_t i = 0; i < x->len; i++) {
        if (x->entries[i].dead) continue;
        AxValue ov;
        if (!ax_dict_get(y, x->entries[i].key, &ov)) return false;
        bool same = equals_depth(x->entries[i].val, ov, depth + 1);
        ax_release(ov);
        if (!same) return false;
      }
      return true;
    }
    default: return false;
  }
}

bool ax_equals(AxValue a, AxValue b) { return equals_depth(a, b, 0); }

int ax_compare(AxValue a, AxValue b) {
  if (a.t == AX_NUM && b.t == AX_NUM) return a.num < b.num ? -1 : (a.num > b.num ? 1 : 0);
  if (a.t == AX_BOOL || b.t == AX_BOOL) {
    int x = (a.t == AX_BOOL && a.b) ? 1 : 0, y = (b.t == AX_BOOL && b.b) ? 1 : 0;
    return x - y;
  }
  AxStr *sa = ax_to_str(a), *sb = ax_to_str(b);
  int c = strcmp(sa->data, sb->data);
  ax_release(ax_strv(sa));
  ax_release(ax_strv(sb));
  return c < 0 ? -1 : (c > 0 ? 1 : 0);
}

// Number formatting has to match JavaScript's, or the two runtimes print different output for
// the same program: shortest round-tripping decimal, no trailing ".0", exponent form only at
// the same thresholds.
static void fmt_num(double d, char *buf, size_t n) {
  if (isnan(d)) { snprintf(buf, n, "NaN"); return; }
  if (isinf(d)) { snprintf(buf, n, d > 0 ? "Infinity" : "-Infinity"); return; }
  if (d == 0) { snprintf(buf, n, "%s", (1 / d < 0) ? "0" : "0"); return; }
  double a = fabs(d);
  if (a >= 1e21 || (a < 1e-6 && a > 0)) {
    // JavaScript switches to exponent notation outside [1e-6, 1e21).
    for (int prec = 1; prec <= 17; prec++) {
      snprintf(buf, n, "%.*e", prec - 1, d);
      if (strtod(buf, NULL) == d) break;
    }
    // Normalise "1.5e+21" → "1.5e+21" (JS prints e+21 as well), strip zero padding.
    char *e = strchr(buf, 'e');
    if (e) {
      char mant[48], expo[16];
      int mant_len = (int)(e - buf);
      if (mant_len > (int)sizeof mant - 1) mant_len = (int)sizeof mant - 1;
      memcpy(mant, buf, (size_t)mant_len);
      mant[mant_len] = '\0';
      int ev = atoi(e + 1);
      snprintf(expo, sizeof expo, "e%c%d", ev < 0 ? '-' : '+', ev < 0 ? -ev : ev);
      snprintf(buf, n, "%s%s", mant, expo);
    }
    return;
  }
  // Integers print without a decimal point or an exponent, which is what JavaScript does for
  // everything below 1e21 — and what a program's output is compared against.
  if (d == floor(d) && fabs(d) < 1e21) { snprintf(buf, n, "%.0f", d); return; }
  for (int prec = 1; prec <= 17; prec++) {
    snprintf(buf, n, "%.*g", prec, d);
    if (strchr(buf, 'e')) continue;      // %g reaches for exponent form far too eagerly
    if (strtod(buf, NULL) == d) return;
  }
  snprintf(buf, n, "%.17g", d);
}

void ax_fmt_num(double d, char *buf, size_t n) { fmt_num(d, buf, n); }

void ax_str_append(char **buf, size_t *len, size_t *cap, const char *s, size_t n);
static void str_append(char **buf, size_t *len, size_t *cap, const char *s, size_t n) {
  if (*len + n + 1 > *cap) {
    while (*len + n + 1 > *cap) *cap = *cap ? *cap * 2 : 64;
    *buf = xrealloc(*buf, *cap);
  }
  memcpy(*buf + *len, s, n);
  *len += n;
  (*buf)[*len] = '\0';
}

static void render(AxValue v, char **buf, size_t *len, size_t *cap, int depth) {
  char tmp[64];
  switch (v.t) {
    case AX_NULL: str_append(buf, len, cap, "null", 4); return;
    case AX_BOOL: str_append(buf, len, cap, v.b ? "true" : "false", v.b ? 4 : 5); return;
    case AX_NUM: fmt_num(v.num, tmp, sizeof tmp); str_append(buf, len, cap, tmp, strlen(tmp)); return;
    case AX_STR:
    case AX_ATOM: { AxStr *s = (AxStr *)v.o; str_append(buf, len, cap, s->data, s->len); return; }
    case AX_ARR: {
      if (depth > 16) { str_append(buf, len, cap, "[...]", 5); return; }
      AxArr *a = (AxArr *)v.o;
      str_append(buf, len, cap, "[", 1);
      for (uint32_t i = 0; i < a->len; i++) {
        if (i) str_append(buf, len, cap, ",", 1);
        render(a->items[i], buf, len, cap, depth + 1);
      }
      str_append(buf, len, cap, "]", 1);
      return;
    }
    case AX_DICT: {
      // A dict shows as its JSON (JSON.stringify's, `__type` first for a record); a resource
      // or draw descriptor ({kind, name, …}) shows as its name.
      AxDict *d = (AxDict *)v.o;
      AxStr *kk = ax_internz("kind"), *kn = ax_internz("name");
      AxValue kind, name;
      bool hk = ax_dict_get(d, kk, &kind), hn = ax_dict_get(d, kn, &name);
      ax_release(ax_strv(kk)); ax_release(ax_strv(kn));
      if (hk && hn && kind.t == AX_STR && name.t == AX_STR) {
        AxStr *ns = (AxStr *)name.o;
        str_append(buf, len, cap, ns->data, ns->len);
        ax_release(kind); ax_release(name);
        return;
      }
      if (hk) ax_release(kind);
      if (hn) ax_release(name);
      char *json = NULL;
      ax_json_write(v, 0, &json);
      str_append(buf, len, cap, json, strlen(json));
      free(json);
      return;
    }
    case AX_FN: {
      AxFn *f = (AxFn *)v.o;
      const char *nm = f->name ? f->name : "fn";
      str_append(buf, len, cap, "<fn ", 4);
      str_append(buf, len, cap, nm, strlen(nm));
      str_append(buf, len, cap, ">", 1);
      return;
    }
    case AX_RANGE: {
      AxRange *r = (AxRange *)v.o;
      fmt_num(r->lo, tmp, sizeof tmp); str_append(buf, len, cap, tmp, strlen(tmp));
      str_append(buf, len, cap, "..", 2);
      fmt_num(r->hi, tmp, sizeof tmp); str_append(buf, len, cap, tmp, strlen(tmp));
      return;
    }
    default: ax_render_engine(v, buf, len, cap); return;
  }
}

AxStr *ax_to_str(AxValue v) {
  if (v.t == AX_STR) { ax_retain(v); return (AxStr *)v.o; }
  char *buf = NULL;
  size_t len = 0, cap = 0;
  str_append(&buf, &len, &cap, "", 0);
  render(v, &buf, &len, &cap, 0);
  AxStr *s = ax_str_new(buf, len);
  free(buf);
  return s;
}

const char *ax_type_name(AxValue v) {
  switch (v.t) {
    case AX_NULL: return "null";
    case AX_BOOL: return "bool";
    case AX_NUM: return "number";
    case AX_STR: return "string";
    case AX_ATOM: return "atom";
    case AX_ARR: return "array";
    case AX_DICT: return "dict";
    case AX_FN: return "fn";
    case AX_RANGE: return "range";
    case AX_VEC2: return "vec2";
    case AX_VEC3: return "vec3";
    case AX_QUAT: return "quat";
    case AX_MAT4: return "mat4";
    case AX_XFORM: return "transform";
    case AX_ENTITY: return "entity";
    default: return "dict";   // timers, colliders, pools: plain objects in the reference
  }
}

double ax_to_num(AxValue v) {
  switch (v.t) {
    case AX_NUM: return v.num;
    case AX_BOOL: return v.b ? 1 : 0;
    case AX_NULL: return 0;
    case AX_TIMER: return ((AxTimer *)v.o)->remaining;   // a timer reads as its remaining time
    case AX_STR: {
      AxStr *s = (AxStr *)v.o;
      char *end = NULL;
      double d = strtod(s->data, &end);
      if (end == s->data) return NAN;
      while (*end == ' ' || *end == '\t' || *end == '\n') end++;
      return *end ? NAN : d;
    }
    default: return NAN;
  }
}

// ---------------------------------------------------------------------------------------------
// Scopes
// ---------------------------------------------------------------------------------------------
//
// A scope is a small open-addressed map keyed by interned name pointers, so a lookup is a hash
// of a pointer and a pointer comparison. Frames are usually tiny (a couple of parameters and
// locals), which is why the table starts at 8 slots and never shrinks.

AxScope *ax_scope_new(AxScope *parent, bool fn_root) {
  AxScope *s = xalloc(sizeof(AxScope));
  s->hdr.rc = 1;
  s->hdr.type = 200;   // not a user-visible value type
  s->parent = parent;
  if (parent) parent->hdr.rc++;
  s->fn_root = fn_root;
  return s;
}

void ax_scope_release(AxScope *s) {
  if (!s) return;
  if (--s->hdr.rc > 0) return;
  for (uint32_t i = 0; i < s->cap; i++) {
    if (!s->keys[i]) continue;
    AxValue kv; kv.t = AX_STR; kv.o = (AxObj *)s->keys[i];
    ax_release(kv);
    ax_release(s->vals[i]);
  }
  free(s->keys);
  free(s->vals);
  if (s->parent) ax_scope_release(s->parent);
  free(s);
}

static void scope_grow(AxScope *s) {
  uint32_t ncap = s->cap ? s->cap * 2 : 8;
  AxStr **nk = xalloc(sizeof(AxStr *) * ncap);
  AxValue *nv = xalloc(sizeof(AxValue) * ncap);
  for (uint32_t i = 0; i < s->cap; i++) {
    if (!s->keys[i]) continue;
    uint32_t j = (uint32_t)(((uintptr_t)s->keys[i] >> 4) & (ncap - 1));
    while (nk[j]) j = (j + 1) & (ncap - 1);
    nk[j] = s->keys[i];
    nv[j] = s->vals[i];
  }
  free(s->keys);
  free(s->vals);
  s->keys = nk;
  s->vals = nv;
  s->cap = ncap;
}

static int32_t scope_slot(AxScope *s, AxStr *name) {
  if (!s->cap) return -1;
  uint32_t i = (uint32_t)(((uintptr_t)name >> 4) & (s->cap - 1));
  uint32_t probes = 0;
  while (s->keys[i]) {
    if (s->keys[i] == name || ax_str_eq(s->keys[i], name)) return (int32_t)i;
    i = (i + 1) & (s->cap - 1);
    if (++probes > s->cap) break;
  }
  return -1;
}

// Look in ONE frame, without walking the chain — used by call resolution, which needs to step
// frame by frame to find the nearest callable binding.
bool ax_scope_lookup_local(AxScope *s, AxStr *name, AxValue *out) {
  int32_t i = scope_slot(s, name);
  if (i < 0) return false;
  *out = ax_copy(s->vals[i]);
  return true;
}

bool ax_scope_lookup(AxScope *s, AxStr *name, AxValue *out) {
  for (AxScope *p = s; p; p = p->parent) {
    int32_t i = scope_slot(p, name);
    if (i >= 0) { *out = ax_copy(p->vals[i]); return true; }
  }
  return false;
}

bool ax_scope_set_existing(AxScope *s, AxStr *name, AxValue v) {
  for (AxScope *p = s; p; p = p->parent) {
    if (p->builtin) break;   // the library is read-only: assigning `sum = 3` makes a new binding
    int32_t i = scope_slot(p, name);
    if (i >= 0) { ax_release(p->vals[i]); p->vals[i] = v; return true; }
  }
  return false;
}

void ax_scope_declare(AxScope *s, AxStr *name, AxValue v) {
  int32_t i = scope_slot(s, name);
  if (i >= 0) { ax_release(s->vals[i]); s->vals[i] = v; return; }
  if (s->len * 4 >= s->cap * 3) scope_grow(s);
  uint32_t j = (uint32_t)(((uintptr_t)name >> 4) & (s->cap - 1));
  while (s->keys[j]) j = (j + 1) & (s->cap - 1);
  AxValue kv; kv.t = AX_STR; kv.o = (AxObj *)name;
  ax_retain(kv);
  s->keys[j] = name;
  s->vals[j] = v;
  s->len++;
}

void ax_str_append(char **buf, size_t *len, size_t *cap, const char *s, size_t n) { str_append(buf, len, cap, s, n); }

// ---------------------------------------------------------------------------------------------
// Engine value constructors
// ---------------------------------------------------------------------------------------------

static AxValue vec_new(uint8_t t, double x, double y, double z, double w) {
  AxVec *v = xalloc(sizeof(AxVec));
  v->hdr.rc = 1;
  v->hdr.type = t;
  v->x = x; v->y = y; v->z = z; v->w = w;
  AxValue out; out.t = t; out.o = (AxObj *)v;
  return out;
}
AxValue ax_vec2(double x, double y) { return vec_new(AX_VEC2, x, y, 0, 0); }
AxValue ax_vec3(double x, double y, double z) { return vec_new(AX_VEC3, x, y, z, 0); }
AxValue ax_quat(double x, double y, double z, double w) { return vec_new(AX_QUAT, x, y, z, w); }

AxValue ax_mat4_new(const double *d) {
  AxMat4 *m = xalloc(sizeof(AxMat4));
  m->hdr.rc = 1;
  m->hdr.type = AX_MAT4;
  if (d) memcpy(m->d, d, sizeof m->d);
  AxValue out; out.t = AX_MAT4; out.o = (AxObj *)m;
  return out;
}

AxValue ax_xform_new(void) {
  AxXform *x = xalloc(sizeof(AxXform));
  x->hdr.rc = 1;
  x->hdr.type = AX_XFORM;
  x->pos = ax_vec3(0, 0, 0);
  x->rot = ax_quat(0, 0, 0, 1);
  x->scl = ax_vec3(1, 1, 1);
  x->vel = ax_vec3(0, 0, 0);
  AxValue out; out.t = AX_XFORM; out.o = (AxObj *)x;
  return out;
}

AxValue ax_timer_new(double remaining, const char *unit) {
  AxTimer *t = xalloc(sizeof(AxTimer));
  t->hdr.rc = 1;
  t->hdr.type = AX_TIMER;
  t->remaining = remaining;
  t->total = remaining;
  snprintf(t->unit, sizeof t->unit, "%s", unit ? unit : "s");
  AxValue out; out.t = AX_TIMER; out.o = (AxObj *)t;
  return out;
}
