// regex.c — JavaScript's regular expressions, as V8 runs them.
//
// The `re_*` functions take a JavaScript pattern, so the native runtime needs JavaScript's
// regular expressions, not the C library's: ECMAScript 2024 syntax with Annex B's legacy forms
// (octal escapes, a literal `{`, `\c` before a non-letter …) when neither `u` nor `v` is given,
// code points and \p{…} under `u`, class set operations and properties of strings under `v`,
// lookbehind, named groups, lazy quantifiers and backreferences; matching on UTF-16 units,
// with V8's error messages for malformed patterns. The Unicode data — property sets, which
// characters /i treats as one, the emoji sequences — is generated from Node (regex_tables.h).
//
// A pattern is parsed into a tree, compiled to bytecode and run by a backtracking machine with
// an explicit stack (deep inputs never deepen the C stack), whose semantics follow the
// specification's matchers: captures inside a quantified atom reset on every iteration, an
// iteration past the minimum may not match empty, lookarounds are atomic, lookbehind reads
// right to left.
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <limits.h>
#include "axiom.h"
#include "regex_tables.h"

#define INF UINT32_MAX
#define MAXCP 0x10FFFF

// ---------------------------------------------------------------------------------------------
// Character sets: sorted, disjoint ranges of code points (or of UTF-16 units without `u`)
// ---------------------------------------------------------------------------------------------

typedef struct { uint32_t *r; int n, cap; } CSet;   // n ranges: r[2i] .. r[2i + 1]

static void cs_push(CSet *s, uint32_t lo, uint32_t hi) {
  if (s->n == s->cap) { s->cap = s->cap ? s->cap * 2 : 8; s->r = realloc(s->r, sizeof(uint32_t) * 2 * (size_t)s->cap); }
  s->r[2 * s->n] = lo;
  s->r[2 * s->n + 1] = hi;
  s->n++;
}
static int cmp_range(const void *a, const void *b) {
  uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
  return x < y ? -1 : x > y;
}
static void cs_norm(CSet *s) {
  if (s->n < 2) return;
  qsort(s->r, (size_t)s->n, sizeof(uint32_t) * 2, cmp_range);
  int k = 0;
  for (int i = 1; i < s->n; i++) {
    if (s->r[2 * i] <= s->r[2 * k + 1] + 1 && s->r[2 * k + 1] != UINT32_MAX) {
      if (s->r[2 * i + 1] > s->r[2 * k + 1]) s->r[2 * k + 1] = s->r[2 * i + 1];
    } else {
      k++;
      s->r[2 * k] = s->r[2 * i];
      s->r[2 * k + 1] = s->r[2 * i + 1];
    }
  }
  s->n = k + 1;
}
static void cs_add(CSet *s, uint32_t lo, uint32_t hi) { cs_push(s, lo, hi); }
static void cs_add_set(CSet *s, const CSet *t) { for (int i = 0; i < t->n; i++) cs_push(s, t->r[2 * i], t->r[2 * i + 1]); }
static bool cs_has(const CSet *s, uint32_t c) {
  int lo = 0, hi = s->n - 1;
  while (lo <= hi) {
    int m = (lo + hi) / 2;
    if (c < s->r[2 * m]) hi = m - 1;
    else if (c > s->r[2 * m + 1]) lo = m + 1;
    else return true;
  }
  return false;
}
static CSet cs_complement(const CSet *s, uint32_t max) {   // s normalized
  CSet o = {0};
  uint32_t next = 0;
  for (int i = 0; i < s->n; i++) {
    if (s->r[2 * i] > next) cs_push(&o, next, s->r[2 * i] - 1);
    next = s->r[2 * i + 1] + 1;
    if (s->r[2 * i + 1] >= max) { next = max + 1; break; }
  }
  if (next <= max) cs_push(&o, next, max);
  return o;
}
static CSet cs_intersect(const CSet *a, const CSet *b) {   // both normalized
  CSet o = {0};
  int i = 0, j = 0;
  while (i < a->n && j < b->n) {
    uint32_t lo = a->r[2 * i] > b->r[2 * j] ? a->r[2 * i] : b->r[2 * j];
    uint32_t hi = a->r[2 * i + 1] < b->r[2 * j + 1] ? a->r[2 * i + 1] : b->r[2 * j + 1];
    if (lo <= hi) cs_push(&o, lo, hi);
    if (a->r[2 * i + 1] < b->r[2 * j + 1]) i++; else j++;
  }
  return o;
}
static void cs_free(CSet *s) { free(s->r); s->r = NULL; s->n = s->cap = 0; }
static CSet cs_copy(const CSet *s) { CSet o = {0}; cs_add_set(&o, s); return o; }

// A generated set (regex_tables.h).
static CSet table_set(int idx) {
  CSet o = {0};
  const RxSetDef *d = &RX_SETS[idx];
  uint32_t prev = 0;
  for (uint32_t i = 0; i < d->n; i++) {
    uint32_t lo = prev + RX_DELTAS[d->off + 2 * i], hi = lo + RX_DELTAS[d->off + 2 * i + 1];
    cs_push(&o, lo, hi);
    prev = hi;
  }
  return o;
}

// ---------------------------------------------------------------------------------------------
// Case-insensitive equivalence: the classes of characters /i (or /iu) matches with each other
// ---------------------------------------------------------------------------------------------

typedef struct { uint32_t *members; int *start; int nclass; } FoldIndex;   // members grouped by class
static FoldIndex g_fold[2];
static bool g_fold_ready[2];

static const uint32_t (*fold_table(bool uni, int *n))[2] {
  *n = uni ? RX_FOLD_U_N : RX_FOLD_NONU_N;
  return uni ? RX_FOLD_U : RX_FOLD_NONU;
}
// The smallest member of c's class (c itself when it has none).
static uint32_t fold_rep(bool uni, uint32_t c) {
  int n;
  const uint32_t (*t)[2] = fold_table(uni, &n);
  int lo = 0, hi = n - 1;
  while (lo <= hi) {
    int m = (lo + hi) / 2;
    if (c < t[m][0]) hi = m - 1;
    else if (c > t[m][0]) lo = m + 1;
    else return t[m][1];
  }
  return c;
}
static int cmp_pair_rep(const void *a, const void *b) {
  const uint32_t *x = a, *y = b;
  if (x[1] != y[1]) return x[1] < y[1] ? -1 : 1;
  return x[0] < y[0] ? -1 : x[0] > y[0];
}
static FoldIndex *fold_index(bool uni) {
  if (g_fold_ready[uni]) return &g_fold[uni];
  int n;
  const uint32_t (*t)[2] = fold_table(uni, &n);
  uint32_t (*p)[2] = malloc(sizeof(uint32_t) * 2 * (size_t)n);
  memcpy(p, t, sizeof(uint32_t) * 2 * (size_t)n);
  qsort(p, (size_t)n, sizeof(uint32_t) * 2, cmp_pair_rep);
  FoldIndex *f = &g_fold[uni];
  f->members = malloc(sizeof(uint32_t) * (size_t)(2 * n + 1));
  f->start = malloc(sizeof(int) * (size_t)(n + 2));
  int k = 0, c = 0;
  for (int i = 0; i < n; i++) {
    if (i == 0 || p[i][1] != p[i - 1][1]) { f->start[c++] = k; f->members[k++] = p[i][1]; }
    f->members[k++] = p[i][0];
  }
  f->start[c] = k;
  f->nclass = c;
  free(p);
  g_fold_ready[uni] = true;
  return f;
}
// Every character some member of s is equivalent to, added to s (s normalized on return).
static void cs_case_close(CSet *s, bool uni) {
  cs_norm(s);
  FoldIndex *f = fold_index(uni);
  CSet add = {0};
  for (int c = 0; c < f->nclass; c++) {
    bool any = false;
    for (int k = f->start[c]; k < f->start[c + 1] && !any; k++) any = cs_has(s, f->members[k]);
    if (any) for (int k = f->start[c]; k < f->start[c + 1]; k++) cs_push(&add, f->members[k], f->members[k]);
  }
  cs_add_set(s, &add);
  cs_free(&add);
  cs_norm(s);
}

// ---------------------------------------------------------------------------------------------
// The pattern tree
// ---------------------------------------------------------------------------------------------

enum { N_EMPTY, N_CHAR, N_SET, N_BOL, N_EOL, N_WB, N_NWB, N_SEQ, N_ALT, N_GROUP, N_LOOK, N_BREF, N_QUANT };

typedef struct RNode RNode;
struct RNode {
  int t;
  uint32_t c;              // N_CHAR
  int set;                 // N_SET: index into the compiled sets
  RNode **kids;            // N_SEQ, N_ALT
  int nk, capk;
  RNode *kid;              // N_GROUP, N_LOOK, N_QUANT
  int idx;                 // N_GROUP: capture number; N_BREF: group (0 while a name is unresolved)
  uint16_t *name;          // N_BREF by name
  uint32_t namelen;
  bool behind, neg;        // N_LOOK
  uint32_t min, max;       // N_QUANT
  bool greedy;
  int cap_lo, cap_hi;      // N_QUANT: the captures inside, [lo, hi)
  RNode *next_alloc;
};

// A class value under `v`: characters and strings (of code points).
typedef struct { uint32_t *cp; int n; } VStr;
typedef struct { CSet chars; VStr *strs; int nstrs, capstrs; } ClassVal;

typedef struct {
  const uint16_t *p;
  uint32_t n, i;
  bool uni, vmode, icase, multiline, dotall;
  int ncaps;                       // captures so far (group 0 not counted)
  int total_caps;                  // all of them (scanned ahead), -1 until needed
  int has_named;                   // -1 until needed
  uint16_t **names; uint32_t *namelens; int nnames, capnames;   // by capture number - 1
  char *err;
  size_t errsz;
  bool failed;
  RNode *nodes;                    // every node, for freeing
  CSet *sets; int nsets, capsets;
  RNode **brefs; int nbrefs, capbrefs;   // named references to resolve
  int *closed; int nclosed, capclosed;   // named groups closed so far
  int depth;
} P;

#define FAIL(...) do { rx_fail(ps, __VA_ARGS__); return NULL; } while (0)

static void rx_fail(P *ps, const char *msg) {
  if (ps->failed) return;
  ps->failed = true;
  snprintf(ps->err, ps->errsz, "%s", msg);
}

static RNode *mk(P *ps, int t) {
  RNode *n = calloc(1, sizeof *n);
  n->t = t;
  n->next_alloc = ps->nodes;
  ps->nodes = n;
  return n;
}
static void kid_add(RNode *n, RNode *k) {
  if (n->nk == n->capk) { n->capk = n->capk ? n->capk * 2 : 4; n->kids = realloc(n->kids, sizeof(RNode *) * (size_t)n->capk); }
  n->kids[n->nk++] = k;
}
static int add_set(P *ps, CSet s) {
  if (ps->nsets == ps->capsets) { ps->capsets = ps->capsets ? ps->capsets * 2 : 8; ps->sets = realloc(ps->sets, sizeof(CSet) * (size_t)ps->capsets); }
  ps->sets[ps->nsets] = s;
  return ps->nsets++;
}

// --- reading the pattern -------------------------------------------------------------------------

#define END UINT32_MAX
static uint32_t cur(P *ps) { return ps->i < ps->n ? ps->p[ps->i] : END; }
static uint32_t peekn(P *ps, uint32_t k) { return ps->i + k < ps->n ? ps->p[ps->i + k] : END; }
static bool is_hi(uint32_t u) { return u >= 0xD800 && u <= 0xDBFF; }
static bool is_lo(uint32_t u) { return u >= 0xDC00 && u <= 0xDFFF; }
// The next pattern character: a code point under u/v (a pair read as one), else a unit.
static uint32_t cur_char(P *ps, uint32_t *w) {
  uint32_t c = cur(ps);
  *w = 1;
  if (ps->uni && is_hi(c) && is_lo(peekn(ps, 1))) { *w = 2; return 0x10000 + ((c - 0xD800) << 10) + (peekn(ps, 1) - 0xDC00); }
  return c;
}
static int hexval(uint32_t c) {
  if (c >= '0' && c <= '9') return (int)(c - '0');
  if (c >= 'a' && c <= 'f') return (int)(c - 'a' + 10);
  if (c >= 'A' && c <= 'F') return (int)(c - 'A' + 10);
  return -1;
}
static bool is_syntax_char(uint32_t c) { return c < 128 && strchr("^$\\.*+?()[]{}|/", (int)c) && c; }

// ScanForCaptures: how many capturing groups the whole pattern has, and whether any is named.
static void scan_captures(P *ps) {
  int count = 0, named = 0, class_depth = 0;
  for (uint32_t i = 0; i < ps->n; i++) {
    uint16_t c = ps->p[i];
    if (c == '\\') { i++; continue; }
    if (class_depth) {
      if (c == ']') class_depth--;
      else if (c == '[' && ps->vmode) class_depth++;
      continue;
    }
    if (c == '[') { class_depth = 1; continue; }
    if (c != '(') continue;
    if (i + 1 < ps->n && ps->p[i + 1] == '?') {
      if (i + 2 < ps->n && ps->p[i + 2] == '<' && i + 3 < ps->n && ps->p[i + 3] != '=' && ps->p[i + 3] != '!') { count++; named = 1; }
    } else count++;
  }
  ps->total_caps = count;
  ps->has_named = named;
}
static int total_caps(P *ps) { if (ps->total_caps < 0) scan_captures(ps); return ps->total_caps; }
static bool has_named(P *ps) { if (ps->has_named < 0) scan_captures(ps); return ps->has_named > 0; }

// \u escapes (after the "\u"): four hex digits, a pair of them under u, or \u{…} under u. In a
// group name the u forms are always allowed.
static bool parse_unicode_escape(P *ps, uint32_t *out, bool unicode) {
  uint32_t save = ps->i;
  if (cur(ps) == '{' && unicode) {
    ps->i++;
    uint32_t v = 0;
    int digits = 0;
    while (hexval(cur(ps)) >= 0) {
      v = v * 16 + (uint32_t)hexval(cur(ps));
      if (v > MAXCP) { ps->i = save; return false; }
      ps->i++;
      digits++;
    }
    if (!digits || cur(ps) != '}') { ps->i = save; return false; }
    ps->i++;
    *out = v;
    return true;
  }
  uint32_t v = 0;
  for (int k = 0; k < 4; k++) {
    int h = hexval(peekn(ps, (uint32_t)k));
    if (h < 0) { ps->i = save; return false; }
    v = v * 16 + (uint32_t)h;
  }
  ps->i += 4;
  if (unicode && is_hi(v) && cur(ps) == '\\' && peekn(ps, 1) == 'u') {
    uint32_t back = ps->i;
    ps->i += 2;
    uint32_t lo = 0;
    bool ok = true;
    for (int k = 0; k < 4 && ok; k++) {
      int h = hexval(peekn(ps, (uint32_t)k));
      if (h < 0) ok = false; else lo = lo * 16 + (uint32_t)h;
    }
    if (ok && is_lo(lo)) { ps->i += 4; v = 0x10000 + ((v - 0xD800) << 10) + (lo - 0xDC00); }
    else ps->i = back;
  }
  *out = v;
  return true;
}

static bool id_start(uint32_t c) {
  static CSet s;
  static bool ready;
  if (!ready) {
    for (int i = 0; i < RX_BINARY_N; i++) if (!strcmp(RX_BINARY[i].name, "ID_Start")) s = table_set(RX_BINARY[i].set);
    ready = true;
  }
  return c == '$' || c == '_' || cs_has(&s, c);
}
static bool id_continue(uint32_t c) {
  static CSet s;
  static bool ready;
  if (!ready) {
    for (int i = 0; i < RX_BINARY_N; i++) if (!strcmp(RX_BINARY[i].name, "ID_Continue")) s = table_set(RX_BINARY[i].set);
    ready = true;
  }
  return c == '$' || c == 0x200C || c == 0x200D || cs_has(&s, c);
}

// A group name after "<", through the ">": as UTF-16 units into *out.
static bool parse_group_name(P *ps, uint16_t **out, uint32_t *outlen) {
  uint16_t *buf = malloc(sizeof(uint16_t) * (ps->n + 2));
  uint32_t len = 0;
  for (bool first = true;; first = false) {
    uint32_t c = cur(ps);
    if (c == END) { free(buf); return false; }
    if (c == '>' && !first) { ps->i++; break; }
    if (c == '\\') {
      ps->i++;
      if (cur(ps) != 'u') { free(buf); return false; }
      ps->i++;
      if (!parse_unicode_escape(ps, &c, true)) { free(buf); rx_fail(ps, "Invalid Unicode escape"); return false; }
    } else {
      ps->i++;
      // A surrogate pair in the pattern is one character of the name, with or without u.
      if (is_hi(c) && is_lo(cur(ps))) { c = 0x10000 + ((c - 0xD800) << 10) + (cur(ps) - 0xDC00); ps->i++; }
    }
    if (first ? !id_start(c) : !id_continue(c)) { free(buf); return false; }
    if (c >= 0x10000) { buf[len++] = (uint16_t)(0xD800 + ((c - 0x10000) >> 10)); buf[len++] = (uint16_t)(0xDC00 + ((c - 0x10000) & 0x3FF)); }
    else buf[len++] = (uint16_t)c;
  }
  *out = buf;
  *outlen = len;
  return true;
}

// --- properties ---------------------------------------------------------------------------------

static int name_lookup(const RxName *t, int n, const char *name) {
  int lo = 0, hi = n - 1;
  while (lo <= hi) {
    int m = (lo + hi) / 2;
    int c = strcmp(name, t[m].name);
    if (!c) return t[m].set;
    if (c < 0) hi = m - 1; else lo = m + 1;
  }
  return -1;
}
static int script_lookup(const char *name, bool ext) {
  int lo = 0, hi = RX_SCRIPT_N - 1;
  while (lo <= hi) {
    int m = (lo + hi) / 2;
    int c = strcmp(name, RX_SCRIPT[m].name);
    if (!c) return ext ? RX_SCRIPT[m].scx : RX_SCRIPT[m].sc;
    if (c < 0) hi = m - 1; else lo = m + 1;
  }
  return -1;
}

// \p{…} / \P{…} after the "p": a set (and, under v, the strings of a property of strings).
// Returns false for a name V8 does not accept.
static bool parse_property(P *ps, bool negate, ClassVal *out) {
  if (cur(ps) != '{') return false;
  ps->i++;
  char name[64], value[64];
  size_t nl = 0, vl = 0;
  bool has_value = false;
  for (;;) {
    uint32_t c = cur(ps);
    if (c == END) return false;
    ps->i++;
    if (c == '}') break;
    if (c == '=' && !has_value) { has_value = true; continue; }
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) return false;
    if (has_value) { if (vl + 1 >= sizeof value) return false; value[vl++] = (char)c; }
    else { if (nl + 1 >= sizeof name) return false; name[nl++] = (char)c; }
  }
  name[nl] = value[vl] = '\0';
  int set = -1;
  if (has_value) {
    if (!vl || !nl) return false;
    if (!strcmp(name, "General_Category") || !strcmp(name, "gc")) set = name_lookup(RX_GC, RX_GC_N, value);
    else if (!strcmp(name, "Script") || !strcmp(name, "sc")) set = script_lookup(value, false);
    else if (!strcmp(name, "Script_Extensions") || !strcmp(name, "scx")) set = script_lookup(value, true);
    if (set < 0) return false;
  } else {
    if (!nl) return false;
    set = name_lookup(RX_GC, RX_GC_N, name);
    if (set < 0) set = name_lookup(RX_BINARY, RX_BINARY_N, name);
    if (set < 0 && ps->vmode && !negate) {
      // A property of strings (v only, never negated): RGI_Emoji is all of them.
      bool all = !strcmp(name, "RGI_Emoji");
      bool found = false;
      for (int k = 0; k < RX_STRING_PROPS_N; k++) {
        const RxStringProp *sp = &RX_STRING_PROPS[k];
        if (!all && strcmp(name, sp->name)) continue;
        found = true;
        CSet t = table_set(sp->set);
        cs_add_set(&out->chars, &t);
        cs_free(&t);
        for (uint32_t j = 0; j < sp->n; j++) {
          const char *s = sp->strings[j];
          VStr v = { malloc(sizeof(uint32_t) * (strlen(s) + 1)), 0 };
          for (size_t b = 0, len = strlen(s); b < len;) {
            uint32_t cp;
            b += (size_t)ax_utf8_next(s + b, len - b, &cp);
            v.cp[v.n++] = cp;
          }
          if (out->nstrs == out->capstrs) { out->capstrs = out->capstrs ? out->capstrs * 2 : 64; out->strs = realloc(out->strs, sizeof(VStr) * (size_t)out->capstrs); }
          out->strs[out->nstrs++] = v;
        }
      }
      if (found) { cs_norm(&out->chars); return true; }
    }
    if (set < 0) return false;
  }
  CSet s = table_set(set);
  if (negate) {
    // Under v with i the complement is taken among case-folded characters: of the closed set.
    if (ps->vmode && ps->icase) cs_case_close(&s, true);
    CSet c = cs_complement(&s, MAXCP);
    cs_free(&s);
    s = c;
  }
  cs_add_set(&out->chars, &s);
  cs_free(&s);
  return true;
}

// --- escapes -------------------------------------------------------------------------------------

static void class_escape_set(P *ps, uint32_t e, CSet *out) {
  CSet s = {0};
  switch (e | 0x20) {
    case 'd': cs_add(&s, '0', '9'); break;
    case 's':
      cs_add(&s, 9, 13); cs_add(&s, 32, 32); cs_add(&s, 0xA0, 0xA0); cs_add(&s, 0x1680, 0x1680);
      cs_add(&s, 0x2000, 0x200A); cs_add(&s, 0x2028, 0x2029); cs_add(&s, 0x202F, 0x202F); cs_add(&s, 0x205F, 0x205F);
      cs_add(&s, 0x3000, 0x3000); cs_add(&s, 0xFEFF, 0xFEFF);
      break;
    case 'w':
      cs_add(&s, '0', '9'); cs_add(&s, 'A', 'Z'); cs_add(&s, '_', '_'); cs_add(&s, 'a', 'z');
      // Under u (or v) with i, the characters that fold to word characters are word characters.
      if (ps->uni && ps->icase) { cs_add(&s, 0x17F, 0x17F); cs_add(&s, 0x212A, 0x212A); }
      break;
  }
  cs_norm(&s);
  if (e >= 'A' && e <= 'Z') {
    if (ps->vmode && ps->icase) cs_case_close(&s, true);
    CSet c = cs_complement(&s, ps->uni ? MAXCP : 0xFFFF);
    cs_free(&s);
    s = c;
  }
  cs_add_set(out, &s);
  cs_free(&s);
}

// Legacy octal (Annex B), after the backslash, first digit current.
static uint32_t parse_octal(P *ps) {
  uint32_t v = cur(ps) - '0';
  ps->i++;
  if (cur(ps) >= '0' && cur(ps) <= '7') {
    v = v * 8 + (cur(ps) - '0');
    ps->i++;
    if (v < 32 && cur(ps) >= '0' && cur(ps) <= '7') { v = v * 8 + (cur(ps) - '0'); ps->i++; }
  }
  return v;
}

// A CharacterEscape after the backslash (current is the escape letter): the character, or
// false (with the error set) when the escape is invalid. `in_class` picks V8's message.
static bool parse_char_escape(P *ps, uint32_t *out, bool in_class) {
  uint32_t e = cur(ps);
  switch (e) {
    case 'f': ps->i++; *out = '\f'; return true;
    case 'n': ps->i++; *out = '\n'; return true;
    case 'r': ps->i++; *out = '\r'; return true;
    case 't': ps->i++; *out = '\t'; return true;
    case 'v': ps->i++; *out = '\v'; return true;
    case 'x': {
      ps->i++;
      int a = hexval(cur(ps)), b = hexval(peekn(ps, 1));
      if (a >= 0 && b >= 0) { ps->i += 2; *out = (uint32_t)(a * 16 + b); return true; }
      if (ps->uni) { rx_fail(ps, "Invalid escape"); return false; }
      *out = 'x';
      return true;
    }
    case 'u': {
      ps->i++;
      if (parse_unicode_escape(ps, out, ps->uni)) return true;
      if (ps->uni) { rx_fail(ps, "Invalid Unicode escape"); return false; }
      *out = 'u';
      return true;
    }
    case '0':
      if (!(peekn(ps, 1) >= '0' && peekn(ps, 1) <= '9')) { ps->i++; *out = 0; return true; }
      if (ps->uni) { rx_fail(ps, in_class ? "Invalid class escape" : "Invalid decimal escape"); return false; }
      *out = parse_octal(ps);
      return true;
    default: break;
  }
  if (ps->uni) {
    uint32_t w;
    uint32_t c = cur_char(ps, &w);
    if (is_syntax_char(c) || (in_class && c == '-')) { ps->i += w; *out = c; return true; }
    rx_fail(ps, "Invalid escape");
    return false;
  }
  ps->i++;
  *out = e;   // an identity escape
  return true;
}

// --- classes -------------------------------------------------------------------------------------

static void cv_free(ClassVal *v) {
  cs_free(&v->chars);
  for (int i = 0; i < v->nstrs; i++) free(v->strs[i].cp);
  free(v->strs);
  memset(v, 0, sizeof *v);
}
static void cv_add_str(ClassVal *v, const uint32_t *cp, int n) {
  if (n == 1) { cs_add(&v->chars, cp[0], cp[0]); return; }
  for (int i = 0; i < v->nstrs; i++) if (v->strs[i].n == n && !memcmp(v->strs[i].cp, cp, sizeof(uint32_t) * (size_t)n)) return;
  if (v->nstrs == v->capstrs) { v->capstrs = v->capstrs ? v->capstrs * 2 : 8; v->strs = realloc(v->strs, sizeof(VStr) * (size_t)v->capstrs); }
  VStr s = { malloc(sizeof(uint32_t) * (size_t)(n + 1)), n };
  if (n) memcpy(s.cp, cp, sizeof(uint32_t) * (size_t)n);
  v->strs[v->nstrs++] = s;
}
static bool cv_has_str(const ClassVal *v, const VStr *s) {
  if (s->n == 1) return cs_has(&v->chars, s->cp[0]);
  for (int i = 0; i < v->nstrs; i++) if (v->strs[i].n == s->n && !memcmp(v->strs[i].cp, s->cp, sizeof(uint32_t) * (size_t)s->n)) return true;
  return false;
}
// Under i, compare and store strings in folded form, and close the characters.
static void cv_fold(P *ps, ClassVal *v) {
  if (!ps->icase) { cs_norm(&v->chars); return; }
  cs_case_close(&v->chars, ps->uni);
  for (int i = 0; i < v->nstrs; i++) for (int k = 0; k < v->strs[i].n; k++) v->strs[i].cp[k] = fold_rep(ps->uni, v->strs[i].cp[k]);
}

// A class atom outside v: a character, or a class escape's set. Returns 0 error, 1 character,
// 2 set (into *set).
static int parse_class_atom(P *ps, uint32_t *ch, CSet *set) {
  uint32_t c = cur(ps);
  if (c == '\\') {
    ps->i++;
    uint32_t e = cur(ps);
    if (e == END) { rx_fail(ps, "\\ at end of pattern"); return 0; }
    switch (e) {
      case 'd': case 'D': case 's': case 'S': case 'w': case 'W':
        ps->i++;
        class_escape_set(ps, e, set);
        return 2;
      case 'p': case 'P':
        if (ps->uni) {
          ps->i++;
          ClassVal v = {0};
          if (!parse_property(ps, e == 'P', &v)) { cv_free(&v); rx_fail(ps, "Invalid property name in character class"); return 0; }
          cs_add_set(set, &v.chars);
          cv_free(&v);
          return 2;
        }
        break;
      case 'b': ps->i++; *ch = 8; return 1;
      case '-': if (ps->uni) { ps->i++; *ch = '-'; return 1; } break;
      case 'c': {
        uint32_t l = peekn(ps, 1);
        if ((l >= 'a' && l <= 'z') || (l >= 'A' && l <= 'Z') || (!ps->uni && ((l >= '0' && l <= '9') || l == '_'))) {
          ps->i += 2;
          *ch = l & 0x1F;
          return 1;
        }
        if (ps->uni) { rx_fail(ps, "Invalid Unicode escape"); return 0; }
        *ch = '\\';   // the backslash is a character; the c is read next
        return 1;
      }
      case 'k':
        if (ps->uni || has_named(ps)) { rx_fail(ps, "Invalid escape"); return 0; }
        break;
      default:
        if (e >= '1' && e <= '9') {
          if (ps->uni) { rx_fail(ps, e >= '8' ? "Invalid escape" : "Invalid class escape"); return 0; }
          if (e >= '8') { ps->i++; *ch = e; return 1; }
          *ch = parse_octal(ps);
          return 1;
        }
        break;
    }
    if (!parse_char_escape(ps, ch, true)) return 0;
    return 1;
  }
  uint32_t w;
  *ch = cur_char(ps, &w);
  ps->i += w;
  return 1;
}

static bool parse_class_v(P *ps, ClassVal *out);

// A class: "[" … "]" (the "[" is current). Into a set index, or an alternation for v strings.
static RNode *parse_class(P *ps) {
  ps->i++;
  bool negated = false;
  if (cur(ps) == '^') { negated = true; ps->i++; }
  ClassVal v = {0};
  if (ps->vmode) {
    if (!parse_class_v(ps, &v)) { cv_free(&v); return NULL; }
    if (negated && v.nstrs) { cv_free(&v); FAIL("Negated character class may contain strings"); }
  } else {
    while (cur(ps) != END && cur(ps) != ']') {
      uint32_t c1 = 0, c2 = 0;
      CSet s1 = {0}, s2 = {0};
      int k1 = parse_class_atom(ps, &c1, &s1);
      if (!k1) { cs_free(&s1); cv_free(&v); return NULL; }
      if (cur(ps) == '-') {
        ps->i++;
        if (cur(ps) == END) { if (k1 == 2) cs_add_set(&v.chars, &s1); else cs_add(&v.chars, c1, c1); cs_free(&s1); break; }
        if (cur(ps) == ']') {
          if (k1 == 2) cs_add_set(&v.chars, &s1); else cs_add(&v.chars, c1, c1);
          cs_add(&v.chars, '-', '-');
          cs_free(&s1);
          break;
        }
        int k2 = parse_class_atom(ps, &c2, &s2);
        if (!k2) { cs_free(&s1); cs_free(&s2); cv_free(&v); return NULL; }
        if (k1 == 2 || k2 == 2) {
          if (ps->uni) { cs_free(&s1); cs_free(&s2); cv_free(&v); FAIL("Invalid character class"); }
          if (k1 == 2) cs_add_set(&v.chars, &s1); else cs_add(&v.chars, c1, c1);
          cs_add(&v.chars, '-', '-');
          if (k2 == 2) cs_add_set(&v.chars, &s2); else cs_add(&v.chars, c2, c2);
        } else {
          if (c1 > c2) { cs_free(&s1); cs_free(&s2); cv_free(&v); FAIL("Range out of order in character class"); }
          cs_add(&v.chars, c1, c2);
        }
        cs_free(&s1);
        cs_free(&s2);
        continue;
      }
      if (k1 == 2) cs_add_set(&v.chars, &s1); else cs_add(&v.chars, c1, c1);
      cs_free(&s1);
    }
    if (cur(ps) == END) { cv_free(&v); FAIL("Unterminated character class"); }
    ps->i++;
    cs_norm(&v.chars);
  }
  cv_fold(ps, &v);
  if (negated) {
    CSet c = cs_complement(&v.chars, ps->uni ? MAXCP : 0xFFFF);
    cs_free(&v.chars);
    v.chars = c;
  }
  RNode *setnode = mk(ps, N_SET);
  setnode->set = add_set(ps, cs_copy(&v.chars));
  if (!v.nstrs) { cv_free(&v); return setnode; }
  // Strings longest first, then the characters, then the empty string (the shortest).
  RNode *alt = mk(ps, N_ALT);
  bool has_empty = false;
  for (int maxlen = 0;; maxlen = 0) {
    for (int i = 0; i < v.nstrs; i++) if (v.strs[i].cp && v.strs[i].n > maxlen) maxlen = v.strs[i].n;
    int best = -1;
    for (int i = 0; i < v.nstrs; i++) if (v.strs[i].cp && v.strs[i].n == maxlen) { best = i; break; }
    if (best < 0) break;
    if (maxlen == 0) { has_empty = true; free(v.strs[best].cp); v.strs[best].cp = NULL; continue; }
    VStr *s = &v.strs[best];
    RNode *seq = mk(ps, N_SEQ);
    for (int k = 0; k < s->n; k++) {
      RNode *ch;
      if (ps->icase) {
        CSet cs = {0};
        cs_add(&cs, s->cp[k], s->cp[k]);
        cs_case_close(&cs, ps->uni);
        ch = mk(ps, N_SET);
        ch->set = add_set(ps, cs);
      } else {
        ch = mk(ps, N_CHAR);
        ch->c = s->cp[k];
      }
      kid_add(seq, ch);
    }
    kid_add(alt, seq);
    free(s->cp);
    s->cp = NULL;
  }
  kid_add(alt, setnode);
  if (has_empty) kid_add(alt, mk(ps, N_EMPTY));
  cv_free(&v);
  return alt;
}

// --- v classes (ClassSetExpression) ----------------------------------------------------------------

static bool is_set_syntax(uint32_t c) { return c < 128 && c && strchr("()[]{}/-\\|", (int)c); }
static bool is_reserved_punct(uint32_t c) { return c < 128 && c && strchr("&-!#%,:;<=>@`~", (int)c); }
static bool is_reserved_double(P *ps) {
  uint32_t a = cur(ps), b = peekn(ps, 1);
  return a == b && a < 128 && a && strchr("&!#$%*+,.:;<=>?@^`~", (int)a);
}

// A ClassSetCharacter: returns false with the error set.
static bool parse_set_char(P *ps, uint32_t *out) {
  uint32_t c = cur(ps);
  if (c == END) { rx_fail(ps, "Unterminated character class"); return false; }
  if (c == '\\') {
    ps->i++;
    uint32_t e = cur(ps);
    if (e == END) { rx_fail(ps, "\\ at end of pattern"); return false; }
    if (e == 'b') { ps->i++; *out = 8; return true; }
    if (is_reserved_punct(e)) { ps->i++; *out = e; return true; }
    if (e == 'c') {
      uint32_t l = peekn(ps, 1);
      if ((l >= 'a' && l <= 'z') || (l >= 'A' && l <= 'Z')) { ps->i += 2; *out = l & 0x1F; return true; }
      rx_fail(ps, "Invalid Unicode escape");
      return false;
    }
    if (e >= '1' && e <= '9') { rx_fail(ps, e >= '8' ? "Invalid escape" : "Invalid class escape"); return false; }
    return parse_char_escape(ps, out, true);
  }
  if (is_reserved_double(ps)) { rx_fail(ps, "Invalid set operation in character class"); return false; }
  if (is_set_syntax(c)) { rx_fail(ps, "Invalid character in character class"); return false; }
  uint32_t w;
  *out = cur_char(ps, &w);
  ps->i += w;
  return true;
}

// A ClassSetOperand into *v: a nested class, \q{…}, a class escape or property, or a character.
// Returns 0 on error, 1 for a single character (in *ch), 2 for anything else.
static int parse_set_operand(P *ps, ClassVal *v, uint32_t *ch) {
  uint32_t c = cur(ps);
  if (c == '[') {
    ps->i++;
    bool neg = false;
    if (cur(ps) == '^') { neg = true; ps->i++; }
    ClassVal inner = {0};
    if (!parse_class_v(ps, &inner)) { cv_free(&inner); return 0; }
    if (neg) {
      if (inner.nstrs) { cv_free(&inner); rx_fail(ps, "Negated character class may contain strings"); return 0; }
      cv_fold(ps, &inner);
      CSet comp = cs_complement(&inner.chars, MAXCP);
      cs_free(&inner.chars);
      inner.chars = comp;
    }
    cs_add_set(&v->chars, &inner.chars);
    for (int i = 0; i < inner.nstrs; i++) cv_add_str(v, inner.strs[i].cp, inner.strs[i].n);
    cv_free(&inner);
    return 2;
  }
  if (c == '\\') {
    uint32_t e = peekn(ps, 1);
    if (e == 'd' || e == 'D' || e == 's' || e == 'S' || e == 'w' || e == 'W') {
      ps->i += 2;
      class_escape_set(ps, e, &v->chars);
      return 2;
    }
    if (e == 'p' || e == 'P') {
      ps->i += 2;
      if (!parse_property(ps, e == 'P', v)) { rx_fail(ps, "Invalid property name in character class"); return 0; }
      return 2;
    }
    if (e == 'q') {
      ps->i += 2;
      if (cur(ps) != '{') { rx_fail(ps, "Invalid escape"); return 0; }
      ps->i++;
      uint32_t *buf = malloc(sizeof(uint32_t) * (ps->n + 1));
      int n = 0;
      for (;;) {
        if (cur(ps) == END) { free(buf); rx_fail(ps, "Unterminated character class"); return 0; }
        if (cur(ps) == '}' || cur(ps) == '|') {
          cv_add_str(v, buf, n);
          n = 0;
          if (cur(ps) == '}') { ps->i++; break; }
          ps->i++;
          continue;
        }
        uint32_t x;
        if (!parse_set_char(ps, &x)) { free(buf); return 0; }
        buf[n++] = x;
      }
      free(buf);
      return 2;
    }
  }
  if (!parse_set_char(ps, ch)) return 0;
  return 1;
}

// ClassContents under v, after "[" (and "^"), through the "]".
static bool parse_class_v(P *ps, ClassVal *out) {
  if (++ps->depth > 400) { rx_fail(ps, "Regular expression too large"); return false; }
  if (cur(ps) == ']') { ps->i++; ps->depth--; return true; }
  // The first operand decides union, intersection or subtraction.
  ClassVal first = {0};
  uint32_t ch;
  int k = parse_set_operand(ps, &first, &ch);
  if (!k) { cv_free(&first); return false; }
  bool is_op = cur(ps) == '&' && peekn(ps, 1) == '&';
  bool is_sub = cur(ps) == '-' && peekn(ps, 1) == '-';
  if (is_op || is_sub) {
    if (k == 1) cs_add(&first.chars, ch, ch);
    cv_fold(ps, &first);
    ClassVal acc = first;
    while (cur(ps) != ']') {
      if (cur(ps) == END) { cv_free(&acc); rx_fail(ps, "Unterminated character class"); return false; }
      bool amp = cur(ps) == '&' && peekn(ps, 1) == '&', dash = cur(ps) == '-' && peekn(ps, 1) == '-';
      if (!(is_op ? amp : dash)) { cv_free(&acc); rx_fail(ps, "Invalid set operation in character class"); return false; }
      ps->i += 2;
      if (is_op && cur(ps) == '&') { cv_free(&acc); rx_fail(ps, "Invalid character in character class"); return false; }
      ClassVal rhs = {0};
      uint32_t c2;
      int k2 = parse_set_operand(ps, &rhs, &c2);
      if (!k2) { cv_free(&acc); cv_free(&rhs); return false; }
      if (k2 == 1) cs_add(&rhs.chars, c2, c2);
      cv_fold(ps, &rhs);
      ClassVal res = {0};
      if (is_op) {
        res.chars = cs_intersect(&acc.chars, &rhs.chars);
        for (int i = 0; i < acc.nstrs; i++) if (cv_has_str(&rhs, &acc.strs[i])) cv_add_str(&res, acc.strs[i].cp, acc.strs[i].n);
      } else {
        CSet comp = cs_complement(&rhs.chars, MAXCP);
        res.chars = cs_intersect(&acc.chars, &comp);
        cs_free(&comp);
        for (int i = 0; i < acc.nstrs; i++) if (!cv_has_str(&rhs, &acc.strs[i])) cv_add_str(&res, acc.strs[i].cp, acc.strs[i].n);
      }
      cv_free(&acc);
      cv_free(&rhs);
      acc = res;
    }
    ps->i++;
    cs_add_set(&out->chars, &acc.chars);
    for (int i = 0; i < acc.nstrs; i++) cv_add_str(out, acc.strs[i].cp, acc.strs[i].n);
    cv_free(&acc);
    cs_norm(&out->chars);
    ps->depth--;
    return true;
  }
  // A union of operands and ranges.
  ClassVal acc = {0};
  for (;;) {
    if (k == 2 && cur(ps) == '-' && peekn(ps, 1) != '-') { cv_free(&first); cv_free(&acc); rx_fail(ps, "Invalid character class"); return false; }
    if (k == 1) {
      if (cur(ps) == '-' && peekn(ps, 1) != '-') {
        ps->i++;
        uint32_t c0 = cur(ps), c1 = peekn(ps, 1);
        if (c0 == '[' || (c0 == '\\' && c1 < 128 && c1 && strchr("dDsSwWpPq", (int)c1))) {
          cv_free(&first); cv_free(&acc); rx_fail(ps, "Invalid character class"); return false;
        }
        uint32_t hi;
        if (!parse_set_char(ps, &hi)) { cv_free(&first); cv_free(&acc); return false; }
        if (ch > hi) { cv_free(&first); cv_free(&acc); rx_fail(ps, "Range out of order in character class"); return false; }
        cs_add(&acc.chars, ch, hi);
      } else cs_add(&acc.chars, ch, ch);
    } else {
      cs_add_set(&acc.chars, &first.chars);
      for (int i = 0; i < first.nstrs; i++) cv_add_str(&acc, first.strs[i].cp, first.strs[i].n);
    }
    cv_free(&first);
    if (cur(ps) == ']') { ps->i++; break; }
    if (cur(ps) == END) { cv_free(&acc); rx_fail(ps, "Unterminated character class"); return false; }
    if ((cur(ps) == '&' && peekn(ps, 1) == '&') || (cur(ps) == '-' && peekn(ps, 1) == '-')) {
      cv_free(&acc);
      rx_fail(ps, "Invalid set operation in character class");
      return false;
    }
    k = parse_set_operand(ps, &first, &ch);
    if (!k) { cv_free(&first); cv_free(&acc); return false; }
  }
  cv_fold(ps, &acc);
  cs_add_set(&out->chars, &acc.chars);
  for (int i = 0; i < acc.nstrs; i++) cv_add_str(out, acc.strs[i].cp, acc.strs[i].n);
  cv_free(&acc);
  cs_norm(&out->chars);
  ps->depth--;
  return true;
}

// --- the disjunction ---------------------------------------------------------------------------------

static RNode *char_node(P *ps, uint32_t c) {
  if (ps->icase) {
    CSet s = {0};
    cs_add(&s, c, c);
    cs_case_close(&s, ps->uni);
    if (s.n > 1 || s.r[0] != s.r[1]) {
      RNode *n = mk(ps, N_SET);
      n->set = add_set(ps, s);
      return n;
    }
    cs_free(&s);
  }
  RNode *n = mk(ps, N_CHAR);
  n->c = c;
  return n;
}

// {n}, {n,}, {n,m} at the current "{": true with the bounds (the position past "}"), else false
// with the position unchanged.
static bool parse_interval(P *ps, uint32_t *min, uint32_t *max) {
  uint32_t save = ps->i;
  ps->i++;
  if (!(cur(ps) >= '0' && cur(ps) <= '9')) { ps->i = save; return false; }
  uint64_t a = 0;
  while (cur(ps) >= '0' && cur(ps) <= '9') { a = a * 10 + (cur(ps) - '0'); if (a > INT_MAX) a = INT_MAX; ps->i++; }
  uint64_t b = a;
  if (cur(ps) == ',') {
    ps->i++;
    if (cur(ps) == '}') b = INF;
    else {
      if (!(cur(ps) >= '0' && cur(ps) <= '9')) { ps->i = save; return false; }
      b = 0;
      while (cur(ps) >= '0' && cur(ps) <= '9') { b = b * 10 + (cur(ps) - '0'); if (b > INT_MAX) b = INT_MAX; ps->i++; }
    }
  }
  if (cur(ps) != '}') { ps->i = save; return false; }
  ps->i++;
  *min = (uint32_t)a;
  *max = b >= INT_MAX && b != a ? INF : (uint32_t)b;
  if (b == INF) *max = INF;
  return true;
}

static RNode *parse_disjunction(P *ps);

// Is the next thing a quantifier? (used where an atom is expected)
static bool at_quantifier(P *ps) {
  uint32_t c = cur(ps);
  if (c == '*' || c == '+' || c == '?') return true;
  if (c == '{') { uint32_t a, b, save = ps->i; bool q = parse_interval(ps, &a, &b); ps->i = save; return q; }
  return false;
}

// One term: an assertion, or an atom with an optional quantifier. NULL with the error set, or a
// node; *done is set at "|", ")" or the end.
static RNode *parse_term(P *ps) {
  uint32_t c = cur(ps);
  RNode *atom = NULL;
  int kind = 0;   // 0 plain atom, 1 assertion (no quantifier), 2 lookahead, 3 lookbehind
  switch (c) {
    case '^': ps->i++; atom = mk(ps, N_BOL); kind = 1; break;
    case '$': ps->i++; atom = mk(ps, N_EOL); kind = 1; break;
    case '.': {
      ps->i++;
      CSet s = {0};
      if (ps->dotall) cs_add(&s, 0, ps->uni ? MAXCP : 0xFFFF);
      else {
        cs_add(&s, 0, 9); cs_add(&s, 11, 12); cs_add(&s, 14, 0x2027); cs_add(&s, 0x202A, ps->uni ? MAXCP : 0xFFFF);
      }
      atom = mk(ps, N_SET);
      atom->set = add_set(ps, s);
      break;
    }
    case '*': case '+': case '?': FAIL("Nothing to repeat");
    case '{': {
      uint32_t a, b;
      uint32_t save = ps->i;
      if (parse_interval(ps, &a, &b)) { ps->i = save; FAIL("Nothing to repeat"); }
      if (ps->uni) FAIL("Lone quantifier brackets");
      ps->i++;
      atom = char_node(ps, '{');
      break;
    }
    case '}': case ']':
      if (ps->uni) FAIL("Lone quantifier brackets");
      ps->i++;
      atom = char_node(ps, c);
      break;
    case '[':
      atom = parse_class(ps);
      if (!atom) return NULL;
      break;
    case '(': {
      ps->i++;
      if (cur(ps) == '?') {
        uint32_t k = peekn(ps, 1);
        if (k == ':') {
          ps->i += 2;
          atom = parse_disjunction(ps);
          if (!atom) return NULL;
        } else if (k == '=' || k == '!') {
          ps->i += 2;
          RNode *body = parse_disjunction(ps);
          if (!body) return NULL;
          atom = mk(ps, N_LOOK);
          atom->kid = body;
          atom->neg = k == '!';
          kind = 2;
        } else if (k == '<' && (peekn(ps, 2) == '=' || peekn(ps, 2) == '!')) {
          bool neg = peekn(ps, 2) == '!';
          ps->i += 3;
          RNode *body = parse_disjunction(ps);
          if (!body) return NULL;
          atom = mk(ps, N_LOOK);
          atom->kid = body;
          atom->neg = neg;
          atom->behind = true;
          kind = 3;
        } else if (k == '<') {
          ps->i += 2;
          uint16_t *name;
          uint32_t len;
          if (!parse_group_name(ps, &name, &len)) FAIL("Invalid capture group name");
          int idx = ++ps->ncaps;
          if (ps->nnames < idx) {
            if (idx > ps->capnames) {
              ps->capnames = idx * 2 + 4;
              ps->names = realloc(ps->names, sizeof(uint16_t *) * (size_t)ps->capnames);
              ps->namelens = realloc(ps->namelens, sizeof(uint32_t) * (size_t)ps->capnames);
            }
            while (ps->nnames < idx) { ps->names[ps->nnames] = NULL; ps->namelens[ps->nnames] = 0; ps->nnames++; }
          }
          ps->names[idx - 1] = name;
          ps->namelens[idx - 1] = len;
          RNode *body = parse_disjunction(ps);
          if (!body) return NULL;
          if (cur(ps) != ')') FAIL("Unterminated group");
          // As V8: a name is checked against the groups already closed when its own closes.
          for (int i = 0; i < ps->nclosed; i++) {
            int g = ps->closed[i];
            if (ps->namelens[g - 1] == len && !memcmp(ps->names[g - 1], name, len * 2)) FAIL("Duplicate capture group name");
          }
          if (ps->nclosed == ps->capclosed) { ps->capclosed = ps->capclosed ? ps->capclosed * 2 : 8; ps->closed = realloc(ps->closed, sizeof(int) * (size_t)ps->capclosed); }
          ps->closed[ps->nclosed++] = idx;
          atom = mk(ps, N_GROUP);
          atom->kid = body;
          atom->idx = idx;
        } else {
          FAIL("Invalid group");
        }
      } else {
        int idx = ++ps->ncaps;
        RNode *body = parse_disjunction(ps);
        if (!body) return NULL;
        atom = mk(ps, N_GROUP);
        atom->kid = body;
        atom->idx = idx;
      }
      if (cur(ps) != ')') FAIL("Unterminated group");
      ps->i++;
      break;
    }
    case '\\': {
      ps->i++;
      uint32_t e = cur(ps);
      if (e == END) FAIL("\\ at end of pattern");
      if (e == 'b' || e == 'B') { ps->i++; atom = mk(ps, e == 'b' ? N_WB : N_NWB); kind = 1; break; }
      if (e >= '1' && e <= '9') {
        uint32_t save = ps->i;
        uint64_t v = 0;
        while (cur(ps) >= '0' && cur(ps) <= '9') { v = v * 10 + (cur(ps) - '0'); if (v > INT_MAX) v = INT_MAX; ps->i++; }
        if (v <= (uint64_t)total_caps(ps)) {
          atom = mk(ps, N_BREF);
          atom->idx = (int)v;
          break;
        }
        ps->i = save;
        if (ps->uni) FAIL("Invalid escape");
        if (e >= '8') { ps->i++; atom = char_node(ps, e); break; }
        atom = char_node(ps, parse_octal(ps));
        break;
      }
      if (e == 'k') {
        if (ps->uni || has_named(ps)) {
          ps->i++;
          if (cur(ps) != '<') FAIL("Invalid named reference");
          ps->i++;
          uint16_t *name;
          uint32_t len;
          if (!parse_group_name(ps, &name, &len)) FAIL("Invalid capture group name");
          atom = mk(ps, N_BREF);
          atom->name = name;
          atom->namelen = len;
          if (ps->nbrefs == ps->capbrefs) { ps->capbrefs = ps->capbrefs ? ps->capbrefs * 2 : 4; ps->brefs = realloc(ps->brefs, sizeof(RNode *) * (size_t)ps->capbrefs); }
          ps->brefs[ps->nbrefs++] = atom;
          break;
        }
        ps->i++;
        atom = char_node(ps, 'k');
        break;
      }
      if (e == 'd' || e == 'D' || e == 's' || e == 'S' || e == 'w' || e == 'W') {
        ps->i++;
        CSet s = {0};
        class_escape_set(ps, e, &s);
        cs_norm(&s);
        if (ps->icase) cs_case_close(&s, ps->uni);
        atom = mk(ps, N_SET);
        atom->set = add_set(ps, s);
        break;
      }
      if ((e == 'p' || e == 'P') && ps->uni) {
        ps->i++;
        ClassVal v = {0};
        if (!parse_property(ps, e == 'P', &v)) { cv_free(&v); FAIL("Invalid property name"); }
        cs_norm(&v.chars);
        if (v.nstrs) {
          // A property of strings outside a class: as the class [\p{…}].
          cv_fold(ps, &v);
          RNode *alt = mk(ps, N_ALT);
          // longest strings first
          for (;;) {
            int best = -1;
            for (int i = 0; i < v.nstrs; i++) if (v.strs[i].cp && (best < 0 || v.strs[i].n > v.strs[best].n)) best = i;
            if (best < 0) break;
            RNode *seq = mk(ps, N_SEQ);
            for (int k = 0; k < v.strs[best].n; k++) kid_add(seq, char_node(ps, v.strs[best].cp[k]));
            kid_add(alt, seq);
            free(v.strs[best].cp);
            v.strs[best].cp = NULL;
          }
          RNode *sn = mk(ps, N_SET);
          sn->set = add_set(ps, cs_copy(&v.chars));
          kid_add(alt, sn);
          cv_free(&v);
          atom = alt;
          break;
        }
        if (ps->icase) cs_case_close(&v.chars, ps->uni);
        atom = mk(ps, N_SET);
        atom->set = add_set(ps, cs_copy(&v.chars));
        cv_free(&v);
        break;
      }
      if (e == 'c') {
        uint32_t l = peekn(ps, 1);
        if ((l >= 'a' && l <= 'z') || (l >= 'A' && l <= 'Z')) { ps->i += 2; atom = char_node(ps, l & 0x1F); break; }
        if (ps->uni) FAIL("Invalid Unicode escape");
        atom = char_node(ps, '\\');   // the backslash alone; "c" is read next
        break;
      }
      uint32_t ch;
      if (!parse_char_escape(ps, &ch, false)) return NULL;
      atom = char_node(ps, ch);
      break;
    }
    default: {
      uint32_t w;
      uint32_t ch = cur_char(ps, &w);
      ps->i += w;
      atom = char_node(ps, ch);
      break;
    }
  }
  if (kind == 1) {
    if (at_quantifier(ps)) FAIL("Nothing to repeat");
    return atom;
  }
  // A quantifier.
  uint32_t min, max;
  c = cur(ps);
  if (c == '*') { min = 0; max = INF; ps->i++; }
  else if (c == '+') { min = 1; max = INF; ps->i++; }
  else if (c == '?') { min = 0; max = 1; ps->i++; }
  else if (c == '{') {
    if (!parse_interval(ps, &min, &max)) {
      if (ps->uni) FAIL("Incomplete quantifier");
      return atom;
    }
    if (max != INF && min > max) FAIL("numbers out of order in {} quantifier");
  } else return atom;
  bool greedy = true;
  if (cur(ps) == '?') { greedy = false; ps->i++; }
  if (kind == 3 || (kind == 2 && ps->uni)) FAIL("Invalid quantifier");
  RNode *q = mk(ps, N_QUANT);
  q->kid = atom;
  q->min = min;
  q->max = max;
  q->greedy = greedy;
  return q;
}

static RNode *parse_alternative(P *ps) {
  RNode *seq = mk(ps, N_SEQ);
  while (cur(ps) != END && cur(ps) != '|' && cur(ps) != ')') {
    int caps_before = ps->ncaps;
    RNode *t = parse_term(ps);
    if (!t) return NULL;
    if (t->t == N_QUANT) { t->cap_lo = caps_before + 1; t->cap_hi = ps->ncaps + 1; }
    kid_add(seq, t);
  }
  return seq;
}

static RNode *parse_disjunction(P *ps) {
  if (++ps->depth > 1000) FAIL("Regular expression too large");
  RNode *first = parse_alternative(ps);
  if (!first) return NULL;
  if (cur(ps) != '|') { ps->depth--; return first; }
  RNode *alt = mk(ps, N_ALT);
  kid_add(alt, first);
  while (cur(ps) == '|') {
    ps->i++;
    RNode *a = parse_alternative(ps);
    if (!a) return NULL;
    kid_add(alt, a);
  }
  ps->depth--;
  return alt;
}

// ---------------------------------------------------------------------------------------------
// Bytecode
// ---------------------------------------------------------------------------------------------

enum {
  OP_CHAR, OP_CHAR_B, OP_SET, OP_SET_B, OP_BOL, OP_EOL, OP_WB, OP_NWB,
  OP_SPLIT_NEXT, OP_SPLIT_GOTO, OP_GOTO, OP_SAVE, OP_RESET, OP_BREF, OP_BREF_B,
  OP_LOOK, OP_LOOK_END, OP_PUSH_I, OP_DROP, OP_LOOP, OP_PUSH_POS, OP_CHECK_ADV, OP_MATCH,
};

typedef struct { int32_t *code; int n, cap; } Code;
static int emit(Code *c, int32_t v) {
  if (c->n == c->cap) { c->cap = c->cap ? c->cap * 2 : 64; c->code = realloc(c->code, sizeof(int32_t) * (size_t)c->cap); }
  c->code[c->n] = v;
  return c->n++;
}

typedef struct {
  CSet set;
  uint64_t ascii[2];   // a bitmap of the set below 128
} RSet;

struct AxRegex {
  int32_t *code;
  int ncode;
  RSet *sets;
  int nsets;
  int ncaps;                   // including group 0
  uint16_t **names;            // [group - 1], NULL when unnamed
  uint32_t *namelens;
  bool global, sticky, uni, icase, multiline, dotall, has_indices, vmode;
  bool word_extra;             // \b sees ſ and K as word characters (u/v with i)
};

static bool can_be_empty(RNode *n) {
  switch (n->t) {
    case N_CHAR: case N_SET: return false;
    case N_SEQ: for (int i = 0; i < n->nk; i++) if (!can_be_empty(n->kids[i])) return false; return true;
    case N_ALT: for (int i = 0; i < n->nk; i++) if (can_be_empty(n->kids[i])) return true; return false;
    case N_GROUP: return can_be_empty(n->kid);
    case N_QUANT: return n->min == 0 || can_be_empty(n->kid);
    default: return true;
  }
}

static void compile(Code *c, RNode *n, bool back);

static void compile_quant(Code *c, RNode *n, bool back) {
  if (n->max == 0) return;
  bool check = can_be_empty(n->kid);
  bool reset = n->cap_hi > n->cap_lo;
  #define ITER() do { if (reset) { emit(c, OP_RESET); emit(c, n->cap_lo); emit(c, n->cap_hi); } compile(c, n->kid, back); } while (0)
  // The mandatory iterations.
  if (n->min == 1) ITER();
  else if (n->min > 1) {
    emit(c, OP_PUSH_I); emit(c, (int32_t)n->min);
    int top = c->n;
    ITER();
    emit(c, OP_LOOP); emit(c, top);
    emit(c, OP_DROP);
  }
  if (n->max == n->min) return;
  // The optional ones: each may not match empty.
  int split_op = n->greedy ? OP_SPLIT_NEXT : OP_SPLIT_GOTO;
  if (n->max == INF) {
    int top = emit(c, split_op);
    int patch = emit(c, 0);
    if (check) emit(c, OP_PUSH_POS);
    ITER();
    if (check) emit(c, OP_CHECK_ADV);
    emit(c, OP_GOTO); emit(c, top);
    c->code[patch] = c->n;
  } else if (n->max - n->min == 1) {
    emit(c, split_op);
    int patch = emit(c, 0);
    if (check) emit(c, OP_PUSH_POS);
    ITER();
    if (check) emit(c, OP_CHECK_ADV);
    c->code[patch] = c->n;
  } else {
    emit(c, OP_PUSH_I); emit(c, (int32_t)(n->max - n->min));
    int top = emit(c, split_op);
    int patch = emit(c, 0);
    if (check) emit(c, OP_PUSH_POS);
    ITER();
    if (check) emit(c, OP_CHECK_ADV);
    emit(c, OP_LOOP); emit(c, top);
    c->code[patch] = c->n;
    emit(c, OP_DROP);
  }
  #undef ITER
}

static void compile(Code *c, RNode *n, bool back) {
  switch (n->t) {
    case N_EMPTY: break;
    case N_CHAR: emit(c, back ? OP_CHAR_B : OP_CHAR); emit(c, (int32_t)n->c); break;
    case N_SET: emit(c, back ? OP_SET_B : OP_SET); emit(c, n->set); break;
    case N_BOL: emit(c, OP_BOL); break;
    case N_EOL: emit(c, OP_EOL); break;
    case N_WB: emit(c, OP_WB); break;
    case N_NWB: emit(c, OP_NWB); break;
    case N_SEQ:
      if (back) for (int i = n->nk - 1; i >= 0; i--) compile(c, n->kids[i], back);
      else for (int i = 0; i < n->nk; i++) compile(c, n->kids[i], back);
      break;
    case N_ALT: {
      // split→next alt; alt0; goto end; split→next; alt1; goto end; …; altN
      int *ends = malloc(sizeof(int) * (size_t)n->nk);
      int ne = 0;
      for (int i = 0; i < n->nk; i++) {
        int patch = -1;
        if (i < n->nk - 1) { emit(c, OP_SPLIT_NEXT); patch = emit(c, 0); }
        compile(c, n->kids[i], back);
        if (i < n->nk - 1) { emit(c, OP_GOTO); ends[ne++] = emit(c, 0); c->code[patch] = c->n; }
      }
      for (int i = 0; i < ne; i++) c->code[ends[i]] = c->n;
      free(ends);
      break;
    }
    case N_GROUP:
      emit(c, OP_SAVE); emit(c, 2 * n->idx + (back ? 1 : 0));
      compile(c, n->kid, back);
      emit(c, OP_SAVE); emit(c, 2 * n->idx + (back ? 0 : 1));
      break;
    case N_LOOK: {
      emit(c, OP_LOOK);
      emit(c, n->neg);
      int patch = emit(c, 0);
      compile(c, n->kid, n->behind);
      emit(c, OP_LOOK_END);
      c->code[patch] = c->n;
      break;
    }
    case N_BREF: emit(c, back ? OP_BREF_B : OP_BREF); emit(c, n->idx); break;
    case N_QUANT: compile_quant(c, n, back); break;
  }
}

// ---------------------------------------------------------------------------------------------
// Compiling a pattern
// ---------------------------------------------------------------------------------------------

AxRegex *ax_regex_compile(const uint16_t *pat, uint32_t n, const char *flags, char *err, size_t errsz) {
  P ps = {0};
  ps.p = pat;
  ps.n = n;
  ps.err = err;
  ps.errsz = errsz;
  ps.total_caps = -1;
  ps.has_named = -1;
  AxRegex *re = calloc(1, sizeof *re);
  for (const char *f = flags; *f; f++) {
    switch (*f) {
      case 'g': re->global = true; break;
      case 'y': re->sticky = true; break;
      case 'u': re->uni = true; break;
      case 'v': re->vmode = true; break;
      case 'i': re->icase = true; break;
      case 'm': re->multiline = true; break;
      case 's': re->dotall = true; break;
      case 'd': re->has_indices = true; break;
    }
  }
  ps.uni = re->uni || re->vmode;
  ps.vmode = re->vmode;
  ps.icase = re->icase;
  ps.multiline = re->multiline;
  ps.dotall = re->dotall;
  RNode *root = parse_disjunction(&ps);
  if (root && cur(&ps) == ')') { rx_fail(&ps, "Unmatched ')'"); root = NULL; }
  if (root && !ps.failed) {
    for (int i = 0; i < ps.nbrefs; i++) {
      RNode *b = ps.brefs[i];
      for (int g = 0; g < ps.nnames; g++) if (ps.names[g] && ps.namelens[g] == b->namelen && !memcmp(ps.names[g], b->name, b->namelen * 2)) b->idx = g + 1;
      if (!b->idx) { rx_fail(&ps, "Invalid named capture referenced"); root = NULL; break; }
    }
  }
  bool ok = root && !ps.failed;
  if (ok) {
    Code c = {0};
    emit(&c, OP_SAVE); emit(&c, 0);
    compile(&c, root, false);
    emit(&c, OP_SAVE); emit(&c, 1);
    emit(&c, OP_MATCH);
    re->code = c.code;
    re->ncode = c.n;
    re->ncaps = ps.ncaps + 1;
    re->nsets = ps.nsets;
    re->sets = calloc((size_t)(ps.nsets ? ps.nsets : 1), sizeof(RSet));
    for (int i = 0; i < ps.nsets; i++) {
      cs_norm(&ps.sets[i]);
      re->sets[i].set = ps.sets[i];
      for (uint32_t ch = 0; ch < 128; ch++) if (cs_has(&ps.sets[i], ch)) re->sets[i].ascii[ch >> 6] |= 1ULL << (ch & 63);
    }
    re->names = calloc((size_t)re->ncaps, sizeof(uint16_t *));
    re->namelens = calloc((size_t)re->ncaps, sizeof(uint32_t));
    for (int g = 0; g < ps.nnames; g++) { re->names[g] = ps.names[g]; re->namelens[g] = ps.namelens[g]; }
    re->word_extra = (re->uni || re->vmode) && re->icase;
  } else {
    for (int i = 0; i < ps.nsets; i++) cs_free(&ps.sets[i]);
    for (int g = 0; g < ps.nnames; g++) free(ps.names[g]);
  }
  free(ps.sets);
  free(ps.names);
  free(ps.namelens);
  for (RNode *x = ps.nodes, *nx; x; x = nx) { nx = x->next_alloc; free(x->kids); free(x->name); free(x); }
  free(ps.brefs);
  free(ps.closed);
  if (!ok) { free(re); return NULL; }
  return re;
}

void ax_regex_free(AxRegex *re) {
  if (!re) return;
  free(re->code);
  for (int i = 0; i < re->nsets; i++) cs_free(&re->sets[i].set);
  free(re->sets);
  for (int g = 0; g < re->ncaps; g++) free(re->names[g]);
  free(re->names);
  free(re->namelens);
  free(re);
}

int ax_regex_ncaps(const AxRegex *re) { return re->ncaps; }
bool ax_regex_global(const AxRegex *re) { return re->global; }
bool ax_regex_sticky(const AxRegex *re) { return re->sticky; }
bool ax_regex_unicode(const AxRegex *re) { return re->uni || re->vmode; }
bool ax_regex_u_flag(const AxRegex *re) { return re->uni; }
const uint16_t *ax_regex_group_name(const AxRegex *re, int g, uint32_t *len) {
  if (g < 1 || g >= re->ncaps || !re->names[g - 1]) return NULL;
  *len = re->namelens[g - 1];
  return re->names[g - 1];
}

// ---------------------------------------------------------------------------------------------
// The matcher
// ---------------------------------------------------------------------------------------------

typedef struct {
  const AxRegex *re;
  const uint16_t *s;
  uint32_t n;
  bool uni;
  int32_t *caps;        // 2 * ncaps
  int32_t *aux; int naux, capaux;
  int32_t *bt; size_t nbt, capbt;   // backtrack frames
  int ncw;              // capture words
} M;

static void aux_push(M *m, int32_t v) {
  if (m->naux == m->capaux) { m->capaux = m->capaux ? m->capaux * 2 : 32; m->aux = realloc(m->aux, sizeof(int32_t) * (size_t)m->capaux); }
  m->aux[m->naux++] = v;
}
// A frame: captures, the aux stack, then [naux, pos, pc, kind] on top.
static void bt_push(M *m, int kind, int pc, int32_t pos) {
  size_t need = (size_t)m->ncw + (size_t)m->naux + 4;
  if (m->nbt + need > m->capbt) {
    while (m->nbt + need > m->capbt) m->capbt = m->capbt ? m->capbt * 2 : 1024;
    m->bt = realloc(m->bt, sizeof(int32_t) * m->capbt);
  }
  int32_t *f = m->bt + m->nbt;
  memcpy(f, m->caps, sizeof(int32_t) * (size_t)m->ncw);
  if (m->naux) memcpy(f + m->ncw, m->aux, sizeof(int32_t) * (size_t)m->naux);
  f += m->ncw + m->naux;
  f[0] = m->naux;
  f[1] = pos;
  f[2] = pc;
  f[3] = kind;
  m->nbt += need;
}
// Pops the top frame into the machine; returns its kind (-1 when there is none).
static int bt_pop(M *m, int *pc, int32_t *pos) {
  if (!m->nbt) return -1;
  int32_t *top = m->bt + m->nbt - 4;
  int naux = top[0];
  *pos = top[1];
  *pc = top[2];
  int kind = top[3];
  size_t size = (size_t)m->ncw + (size_t)naux + 4;
  int32_t *f = m->bt + m->nbt - size;
  memcpy(m->caps, f, sizeof(int32_t) * (size_t)m->ncw);
  if (naux > m->capaux) { m->capaux = naux; m->aux = realloc(m->aux, sizeof(int32_t) * (size_t)m->capaux); }
  if (naux) memcpy(m->aux, f + m->ncw, sizeof(int32_t) * (size_t)naux);
  m->naux = naux;
  m->nbt -= size;
  return kind;
}

static bool set_has(const RSet *s, uint32_t c) {
  if (c < 128) return (s->ascii[c >> 6] >> (c & 63)) & 1;
  return cs_has(&s->set, c);
}

// Under u or v, a position between the two halves of a pair holds no character in either
// direction (V8 guards every surrogate it reads so), though assertions still look at the halves.
static bool mid_pair(M *m, int32_t pos) {
  return m->uni && pos > 0 && (uint32_t)pos < m->n && is_lo(m->s[pos]) && is_hi(m->s[pos - 1]);
}
// The character at pos going forward (its width in *w), or -1 at the end.
static int32_t next_char(M *m, int32_t pos, int *w) {
  if ((uint32_t)pos >= m->n || mid_pair(m, pos)) return -1;
  uint32_t c = m->s[pos];
  *w = 1;
  if (m->uni && is_hi(c) && (uint32_t)pos + 1 < m->n && is_lo(m->s[pos + 1])) { *w = 2; return (int32_t)(0x10000 + ((c - 0xD800) << 10) + (m->s[pos + 1] - 0xDC00)); }
  return (int32_t)c;
}
static int32_t prev_char(M *m, int32_t pos, int *w) {
  if (pos <= 0 || mid_pair(m, pos)) return -1;
  uint32_t c = m->s[pos - 1];
  *w = 1;
  if (m->uni && is_lo(c) && pos >= 2 && is_hi(m->s[pos - 2])) { *w = 2; return (int32_t)(0x10000 + ((m->s[pos - 2] - 0xD800) << 10) + (c - 0xDC00)); }
  return (int32_t)c;
}
static bool is_line_term(uint32_t c) { return c == '\n' || c == '\r' || c == 0x2028 || c == 0x2029; }
static bool is_word(const AxRegex *re, int32_t c) {
  if (c < 0) return false;
  if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_') return true;
  return re->word_extra && (c == 0x17F || c == 0x212A);
}
static uint32_t canon(const AxRegex *re, uint32_t c) { return re->icase ? fold_rep(re->uni || re->vmode, c) : c; }

// Runs the program from pos; true on a match (captures in m->caps).
static bool run(M *m, int32_t start) {
  const AxRegex *re = m->re;
  const int32_t *code = re->code;
  int pc = 0;
  int32_t pos = start;
  for (int i = 0; i < m->ncw; i++) m->caps[i] = -1;
  m->naux = 0;
  m->nbt = 0;
  for (;;) {
    bool fail = false;
    switch (code[pc]) {
      case OP_CHAR: {
        int w;
        int32_t ch = next_char(m, pos, &w);
        if (ch < 0 || (uint32_t)ch != (uint32_t)code[pc + 1]) { fail = true; break; }
        pos += w;
        pc += 2;
        break;
      }
      case OP_CHAR_B: {
        int w;
        int32_t ch = prev_char(m, pos, &w);
        if (ch < 0 || (uint32_t)ch != (uint32_t)code[pc + 1]) { fail = true; break; }
        pos -= w;
        pc += 2;
        break;
      }
      case OP_SET: {
        int w;
        int32_t ch = next_char(m, pos, &w);
        if (ch < 0 || !set_has(&re->sets[code[pc + 1]], (uint32_t)ch)) { fail = true; break; }
        pos += w;
        pc += 2;
        break;
      }
      case OP_SET_B: {
        int w;
        int32_t ch = prev_char(m, pos, &w);
        if (ch < 0 || !set_has(&re->sets[code[pc + 1]], (uint32_t)ch)) { fail = true; break; }
        pos -= w;
        pc += 2;
        break;
      }
      case OP_BOL:
        if (pos == 0 || (re->multiline && is_line_term(m->s[pos - 1]))) pc++;
        else fail = true;
        break;
      case OP_EOL:
        if ((uint32_t)pos == m->n || (re->multiline && is_line_term(m->s[pos]))) pc++;
        else fail = true;
        break;
      case OP_WB: case OP_NWB: {
        bool a = pos > 0 && is_word(re, m->s[pos - 1]);
        bool b = (uint32_t)pos < m->n && is_word(re, m->s[pos]);
        if ((a != b) == (code[pc] == OP_WB)) pc++;
        else fail = true;
        break;
      }
      case OP_SPLIT_NEXT:   // try the next instruction first; the target on backtrack
        bt_push(m, 0, code[pc + 1], pos);
        pc += 2;
        break;
      case OP_SPLIT_GOTO:   // try the target first
        bt_push(m, 0, pc + 2, pos);
        pc = code[pc + 1];
        break;
      case OP_GOTO: pc = code[pc + 1]; break;
      case OP_SAVE: m->caps[code[pc + 1]] = pos; pc += 2; break;
      case OP_RESET:
        for (int g = code[pc + 1]; g < code[pc + 2]; g++) m->caps[2 * g] = m->caps[2 * g + 1] = -1;
        pc += 3;
        break;
      case OP_BREF: case OP_BREF_B: {
        int g = code[pc + 1];
        int32_t a = m->caps[2 * g], b = m->caps[2 * g + 1];
        pc += 2;
        if (a < 0 || b < 0) break;   // a group that did not take part matches empty
        int32_t len = b - a;
        bool back = code[pc - 2] == OP_BREF_B;
        int32_t from = back ? pos - len : pos;
        if (from < 0 || (uint32_t)(from + len) > m->n) { fail = true; break; }
        if (!re->icase) {
          if (memcmp(m->s + from, m->s + a, sizeof(uint16_t) * (size_t)len)) fail = true;
        } else {
          // Character by character, each folded.
          int32_t i = 0;
          while (i < len) {
            int w1 = 1, w2 = 1;
            uint32_t c1 = m->s[a + i], c2 = m->s[from + i];
            if (m->uni && is_hi(c1) && i + 1 < len && is_lo(m->s[a + i + 1])) { c1 = 0x10000 + ((c1 - 0xD800) << 10) + (m->s[a + i + 1] - 0xDC00); w1 = 2; }
            if (m->uni && is_hi(c2) && i + 1 < len && is_lo(m->s[from + i + 1])) { c2 = 0x10000 + ((c2 - 0xD800) << 10) + (m->s[from + i + 1] - 0xDC00); w2 = 2; }
            if (w1 != w2 || canon(re, c1) != canon(re, c2)) { fail = true; break; }
            i += w1;
          }
        }
        if (!fail) pos = back ? from : from + len;
        break;
      }
      case OP_LOOK:
        // A boundary frame: popped by failure inside the body, it means the body failed.
        bt_push(m, code[pc + 1] ? 2 : 1, code[pc + 2], pos);
        aux_push(m, (int32_t)(m->nbt));
        aux_push(m, pos);
        pc += 3;
        break;
      case OP_LOOK_END: {
        // The body matched: drop everything it could backtrack into (it is atomic).
        int32_t at = m->aux[--m->naux];
        size_t height = (size_t)m->aux[--m->naux];
        // The boundary frame sits just below `height`; read its kind and continuation.
        int32_t *top = m->bt + height - 4;
        int kind = top[3];
        int cont = top[2];
        size_t size = (size_t)m->ncw + (size_t)top[0] + 4;
        if (kind == 1) {   // positive: keep the captures, return to where it started
          m->nbt = height - size;
          pos = at;
          pc = cont;
        } else {           // negative: the body matching is a failure
          m->nbt = height - size;
          fail = true;
        }
        break;
      }
      case OP_PUSH_I: aux_push(m, code[pc + 1]); pc += 2; break;
      case OP_DROP: m->naux--; pc++; break;
      case OP_LOOP:
        if (--m->aux[m->naux - 1] != 0) pc = code[pc + 1];
        else pc += 2;
        break;
      case OP_PUSH_POS: aux_push(m, pos); pc++; break;
      case OP_CHECK_ADV:
        if (m->aux[--m->naux] == pos) fail = true;
        else pc++;
        break;
      case OP_MATCH: return true;
    }
    if (!fail) continue;
    // Backtrack.
    for (;;) {
      int kind = bt_pop(m, &pc, &pos);
      if (kind < 0) return false;
      if (kind == 0) break;          // an alternative
      if (kind == 1) continue;       // a positive lookaround whose body failed: keep failing
      break;                         // a negative one whose body failed: it succeeds (state restored)
    }
  }
}

// RegExpBuiltinExec's matching loop from lastIndex `start`: 1 and the captures on a match, 0
// when there is none. caps holds 2 * ncaps entries (-1 for a group that did not take part).
int ax_regex_exec(AxRegex *re, const uint16_t *s, uint32_t n, uint32_t start, int32_t *caps) {
  M m = {0};
  m.re = re;
  m.s = s;
  m.n = n;
  m.uni = re->uni || re->vmode;
  m.ncw = 2 * re->ncaps;
  m.caps = caps;
  // As V8 runs it (which differs from the specification's code point steps): the search moves
  // one unit at a time; at a start between the halves of a pair it first tries the pair's start
  // ("optionally step back to the lead surrogate"), then the position itself.
  int r = 0;
  if (m.uni && mid_pair(&m, (int32_t)start) && run(&m, (int32_t)start - 1)) r = 1;
  for (uint32_t pos = start; !r && pos <= n; pos++) {
    if (run(&m, (int32_t)pos)) { r = 1; break; }
    if (re->sticky) break;
  }
  free(m.aux);
  free(m.bt);
  return r;
}
