// ustr.c — text as the reference sees it.
//
// A JavaScript string is a sequence of UTF-16 code units: lengths, indices, slices, ordering and
// case all work on units, so `len("a😀b")` is 4 and `"😀"[0]` is half of a surrogate pair. The
// native runtime keeps text as UTF-8, which keeps output and the C library cheap, and gives every
// string operation the reference's unit semantics on top of it:
//
//  * Storage is WTF-8. A unit sequence can hold a lone surrogate (`chr(0xD83D)`, `"😀"[0]`),
//    which UTF-8 cannot; it is stored in the 3-byte generalized form (ED A0..BF xx). A high
//    surrogate followed by a low one is always stored as the one 4-byte character: every string
//    is normalized when it is made (ax_wtf8_normalize), so equal unit sequences are equal bytes
//    and hash alike, and joining "\uD83D" to "\uDE00" gives "😀".
//  * ASCII text — nearly all of it — is flagged when a string is made, and every operation on
//    it stays on bytes, which are its units.
//  * Text leaving the runtime (output, files, commands, base64, hashes of bytes) is UTF-8 with
//    each lone surrogate written as U+FFFD, as Node writes it; text entering it is decoded as
//    Node decodes UTF-8, each ill-formed sequence becoming U+FFFD (ax_utf8_repair).
//  * Case mapping is String.prototype.toUpperCase/toLowerCase: the full Unicode mappings,
//    including the ones that change length (ß → SS) and the final-sigma rule, from tables
//    generated out of Node itself (tools/gen_unicode.js → unicode.h).
#include <stdlib.h>
#include <string.h>
#include "axiom.h"
#include "unicode.h"

#define LONE(b, i, n) ((b)[i] == 0xED && (i) + 2 < (n) && (b)[(i) + 1] >= 0xA0)

// ---------------------------------------------------------------------------------------------
// Code points in WTF-8
// ---------------------------------------------------------------------------------------------

// The code point at p (n bytes remain) and its length in bytes. Surrogates (ED A0..BF xx) read
// as themselves. An ill-formed byte, which the runtime never stores but a host could hand over,
// reads as U+FFFD.
static inline int next_cp(const uint8_t *p, size_t n, uint32_t *cp) {
  uint8_t c = p[0];
  if (c < 0x80) { *cp = c; return 1; }
  int need = c >= 0xC2 && c <= 0xDF ? 1 : c >= 0xE0 && c <= 0xEF ? 2 : c >= 0xF0 && c <= 0xF4 ? 3 : 0;
  if (!need || (size_t)need + 1 > n) { *cp = 0xFFFD; return 1; }
  uint32_t v = c & (need == 1 ? 0x1F : need == 2 ? 0x0F : 0x07);
  for (int k = 1; k <= need; k++) {
    if ((p[k] & 0xC0) != 0x80) { *cp = 0xFFFD; return 1; }
    v = (v << 6) | (p[k] & 0x3F);
  }
  *cp = v;
  return need + 1;
}

int ax_utf8_next(const char *p, size_t n, uint32_t *cp) { return next_cp((const uint8_t *)p, n, cp); }

static inline int put_cp(uint8_t *o, uint32_t cp) {
  if (cp < 0x80) { o[0] = (uint8_t)cp; return 1; }
  if (cp < 0x800) { o[0] = (uint8_t)(0xC0 | (cp >> 6)); o[1] = (uint8_t)(0x80 | (cp & 0x3F)); return 2; }
  if (cp < 0x10000) {
    o[0] = (uint8_t)(0xE0 | (cp >> 12)); o[1] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F)); o[2] = (uint8_t)(0x80 | (cp & 0x3F));
    return 3;
  }
  o[0] = (uint8_t)(0xF0 | (cp >> 18)); o[1] = (uint8_t)(0x80 | ((cp >> 12) & 0x3F));
  o[2] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F)); o[3] = (uint8_t)(0x80 | (cp & 0x3F));
  return 4;
}

// Joins every high surrogate that is followed by a low one into the 4-byte character, in place;
// returns the new length (never longer).
size_t ax_wtf8_normalize(char *buf, size_t n) {
  uint8_t *b = (uint8_t *)buf;
  const uint8_t *e = memchr(b, 0xED, n);
  if (!e) return n;
  size_t j = (size_t)(e - b);
  for (size_t i = j; i < n;) {
    if (i + 6 <= n && b[i] == 0xED && b[i + 1] >= 0xA0 && b[i + 1] <= 0xAF && b[i + 3] == 0xED && b[i + 4] >= 0xB0 && b[i + 4] <= 0xBF) {
      uint32_t hi = 0xD000 | ((uint32_t)(b[i + 1] & 0x3F) << 6) | (b[i + 2] & 0x3F);
      uint32_t lo = 0xD000 | ((uint32_t)(b[i + 4] & 0x3F) << 6) | (b[i + 5] & 0x3F);
      j += (size_t)put_cp(b + j, 0x10000 + ((hi - 0xD800) << 10) + (lo - 0xDC00));
      i += 6;
      continue;
    }
    b[j++] = b[i++];
  }
  return j;
}

bool ax_bytes_ascii(const char *p, size_t n) {
  const uint8_t *b = (const uint8_t *)p;
  size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    uint64_t w;
    memcpy(&w, b + i, 8);
    if (w & 0x8080808080808080ULL) return false;
  }
  for (; i < n; i++) if (b[i] & 0x80) return false;
  return true;
}

// ---------------------------------------------------------------------------------------------
// Units
// ---------------------------------------------------------------------------------------------

uint32_t ax_u16_count(const char *p, size_t n) {
  const uint8_t *b = (const uint8_t *)p;
  uint32_t u = 0;
  for (size_t i = 0; i < n;) {
    uint32_t cp;
    i += (size_t)next_cp(b + i, n - i, &cp);
    u += cp >= 0x10000 ? 2 : 1;
  }
  return u;
}

// A long non-ASCII string keeps, from its first use by index, where every 64th unit falls: a
// lookup then walks at most one stretch instead of the whole string, so `s[i]` in a loop stays
// linear. The string is immutable, so the index never goes stale.
#define IX_STEP 64
struct AxStrIndex {
  uint32_t ulen, n;
  uint32_t *byte, *unit;   // [k]: the code point holding unit k * IX_STEP starts at byte[k], unit[k]
};

static struct AxStrIndex *str_index(const AxStr *cs) {
  AxStr *s = (AxStr *)cs;
  if (s->ix) return s->ix;
  if (s->ascii || s->len < 256) return NULL;
  struct AxStrIndex *ix = malloc(sizeof *ix);
  uint32_t cap = s->len / IX_STEP + 2;
  ix->byte = malloc(sizeof(uint32_t) * cap);
  ix->unit = malloc(sizeof(uint32_t) * cap);
  ix->n = 0;
  const uint8_t *b = (const uint8_t *)s->data;
  uint32_t u = 0;
  for (size_t k = 0; k < s->len;) {
    uint32_t cp;
    int w = next_cp(b + k, s->len - k, &cp);
    uint32_t units = cp >= 0x10000 ? 2 : 1;
    while ((uint64_t)ix->n * IX_STEP < (uint64_t)u + units) { ix->byte[ix->n] = (uint32_t)k; ix->unit[ix->n] = u; ix->n++; }
    u += units;
    k += (size_t)w;
  }
  ix->ulen = u;
  s->ix = ix;
  return ix;
}

void ax_str_index_free(AxStr *s) {
  free(s->ix->byte);
  free(s->ix->unit);
  free(s->ix);
  s->ix = NULL;
}

// Where to start walking toward unit i: a code point boundary at or before it.
static void walk_start(const AxStr *s, uint32_t i, size_t *byte, uint32_t *unit) {
  struct AxStrIndex *ix = str_index(s);
  *byte = 0;
  *unit = 0;
  if (ix && ix->n) {
    uint32_t k = i / IX_STEP;
    if (k >= ix->n) k = ix->n - 1;
    *byte = ix->byte[k];
    *unit = ix->unit[k];
  }
}

uint32_t ax_str_ulen(const AxStr *s) {
  if (s->ascii) return s->len;
  struct AxStrIndex *ix = str_index(s);
  return ix ? ix->ulen : ax_u16_count(s->data, s->len);
}

// Units of bytes [0, off).
uint32_t ax_str_units_before(const AxStr *s, size_t off) {
  if (s->ascii) return (uint32_t)off;
  if (off > s->len) off = s->len;
  struct AxStrIndex *ix = str_index(s);
  if (!ix || !ix->n) return ax_u16_count(s->data, off);
  uint32_t lo = 0, hi = ix->n - 1;   // the last mark at or before off
  while (lo < hi) {
    uint32_t m = (lo + hi + 1) / 2;
    if (ix->byte[m] <= off) lo = m; else hi = m - 1;
  }
  return ix->unit[lo] + ax_u16_count(s->data + ix->byte[lo], off - ix->byte[lo]);
}

uint16_t *ax_u16_decode(const char *p, size_t n, uint32_t *out_n) {
  uint16_t *u = malloc(sizeof(uint16_t) * (n + 1));
  const uint8_t *b = (const uint8_t *)p;
  uint32_t k = 0;
  for (size_t i = 0; i < n;) {
    uint32_t cp;
    i += (size_t)next_cp(b + i, n - i, &cp);
    if (cp >= 0x10000) { cp -= 0x10000; u[k++] = (uint16_t)(0xD800 + (cp >> 10)); u[k++] = (uint16_t)(0xDC00 + (cp & 0x3FF)); }
    else u[k++] = (uint16_t)cp;
  }
  *out_n = k;
  return u;
}

uint16_t *ax_str_units(const AxStr *s, uint32_t *out_n) { return ax_u16_decode(s->data, s->len, out_n); }

AxStr *ax_str_from_u16(const uint16_t *u, size_t n) {
  uint8_t *o = malloc(n * 3 + 1);
  size_t j = 0;
  for (size_t i = 0; i < n; i++) {
    uint32_t c = u[i];
    if (c >= 0xD800 && c <= 0xDBFF && i + 1 < n && u[i + 1] >= 0xDC00 && u[i + 1] <= 0xDFFF) {
      c = 0x10000 + ((c - 0xD800) << 10) + (u[i + 1] - 0xDC00);
      i++;
    }
    j += (size_t)put_cp(o + j, c);
  }
  AxStr *s = ax_str_new((const char *)o, j);
  free(o);
  return s;
}

// The unit at index i, or -1 past the end.
int32_t ax_str_unit_at(const AxStr *s, uint32_t i) {
  if (s->ascii) return i < s->len ? (unsigned char)s->data[i] : -1;
  const uint8_t *b = (const uint8_t *)s->data;
  size_t k;
  uint32_t u;
  walk_start(s, i, &k, &u);
  while (k < s->len) {
    uint32_t cp;
    k += (size_t)next_cp(b + k, s->len - k, &cp);
    if (cp >= 0x10000) {
      if (i == u) return 0xD800 + ((cp - 0x10000) >> 10);
      if (i == u + 1) return 0xDC00 + ((cp - 0x10000) & 0x3FF);
      u += 2;
    } else {
      if (i == u) return (int32_t)cp;
      u++;
    }
  }
  return -1;
}

// Units [a, b) as a new string (a <= b). A cut through a surrogate pair keeps the half inside,
// as a lone surrogate, which is what the reference's slice does.
AxStr *ax_str_unit_slice(const AxStr *s, uint32_t a, uint32_t b) {
  if (s->ascii) {
    if (a > s->len) a = s->len;
    if (b > s->len) b = s->len;
    return ax_str_new(s->data + a, b > a ? b - a : 0);
  }
  const uint8_t *p = (const uint8_t *)s->data;
  size_t cap = (size_t)(b > a ? b - a : 0) * 3 + 4;   // a unit is at most 3 bytes (a pair, 4 for 2)
  if (cap > s->len + 4) cap = s->len + 4;
  uint8_t *o = malloc(cap);
  size_t j = 0, k;
  uint32_t u;
  walk_start(s, a, &k, &u);
  while (k < s->len && u < b) {
    uint32_t cp;
    int w = next_cp(p + k, s->len - k, &cp);
    if (cp >= 0x10000) {
      bool hi = u >= a && u < b, lo = u + 1 >= a && u + 1 < b;
      if (hi && lo) { memcpy(o + j, p + k, 4); j += 4; }
      else if (hi) j += (size_t)put_cp(o + j, 0xD800 + ((cp - 0x10000) >> 10));
      else if (lo) j += (size_t)put_cp(o + j, 0xDC00 + ((cp - 0x10000) & 0x3FF));
      u += 2;
    } else {
      if (u >= a) { memcpy(o + j, p + k, (size_t)w); j += (size_t)w; }
      u++;
    }
    k += (size_t)w;
  }
  AxStr *r = ax_str_new((const char *)o, j);
  free(o);
  return r;
}

// String comparison as JavaScript's < compares: unit by unit. Code point order (which plain byte
// order gives) differs from it only where a character past U+FFFF meets one in U+E000..U+FFFF.
int ax_str_cmp(const AxStr *a, const AxStr *b) {
  size_t n = a->len < b->len ? a->len : b->len;
  size_t i = 0;
  while (i < n && a->data[i] == b->data[i]) i++;
  if (i == n) return a->len < b->len ? -1 : (a->len > b->len ? 1 : 0);
  if (a->ascii || b->ascii || ((uint8_t)a->data[i] < 0xEE && (uint8_t)b->data[i] < 0xEE)) {
    // Below the first byte that can start U+E000.. or a 4-byte character, byte order is unit order.
    return (uint8_t)a->data[i] < (uint8_t)b->data[i] ? -1 : 1;
  }
  // Back up to the start of the characters that differ and compare their units.
  while (i > 0 && ((uint8_t)a->data[i] & 0xC0) == 0x80) i--;
  uint32_t ca, cb;
  next_cp((const uint8_t *)a->data + i, a->len - i, &ca);
  next_cp((const uint8_t *)b->data + i, b->len - i, &cb);
  uint32_t ua = ca >= 0x10000 ? 0xD800 + ((ca - 0x10000) >> 10) : ca;
  uint32_t ub = cb >= 0x10000 ? 0xD800 + ((cb - 0x10000) >> 10) : cb;
  if (ua == ub) { ua = 0xDC00 + ((ca - 0x10000) & 0x3FF); ub = 0xDC00 + ((cb - 0x10000) & 0x3FF); }
  return ua < ub ? -1 : (ua > ub ? 1 : 0);
}

// ---------------------------------------------------------------------------------------------
// Crossing the boundary
// ---------------------------------------------------------------------------------------------

bool ax_bytes_have_lone(const char *p, size_t n) {
  const uint8_t *b = (const uint8_t *)p;
  for (const uint8_t *e = memchr(b, 0xED, n); e; e = memchr(e + 1, 0xED, n - (size_t)(e + 1 - b))) {
    if ((size_t)(e - b) + 1 < n && e[1] >= 0xA0) return true;
    if ((size_t)(e + 1 - b) >= n) break;
  }
  return false;
}

// Bytes as Node writes a string: UTF-8, each lone surrogate as U+FFFD. Returns p itself when
// nothing changes, else a malloc'd copy (*owned set).
const char *ax_utf8_out(const char *p, size_t n, size_t *out_n, bool *owned) {
  *owned = false;
  *out_n = n;
  if (!ax_bytes_have_lone(p, n)) return p;
  const uint8_t *b = (const uint8_t *)p;
  char *o = malloc(n + 1);
  size_t j = 0;
  for (size_t i = 0; i < n;) {
    if (LONE(b, i, n)) { memcpy(o + j, "\xEF\xBF\xBD", 3); j += 3; i += 3; continue; }
    o[j++] = (char)b[i++];
  }
  o[j] = '\0';
  *out_n = j;
  *owned = true;
  return o;
}

// Bytes as Node's toString('utf8') reads them (the WHATWG decoder): each ill-formed sequence —
// the longest prefix of one that could still have been valid — becomes one U+FFFD. Surrogates
// encoded in UTF-8 are ill-formed too, so nothing from outside can make a lone surrogate.
AxStr *ax_utf8_repair(const char *p, size_t n) {
  const uint8_t *b = (const uint8_t *)p;
  if (ax_bytes_ascii(p, n)) return ax_str_new(p, n);
  char *out = malloc(n * 3 + 1);
  size_t j = 0;
  for (size_t i = 0; i < n;) {
    uint8_t c = b[i];
    if (c < 0x80) { out[j++] = (char)c; i++; continue; }
    int need = c >= 0xC2 && c <= 0xDF ? 1 : c >= 0xE0 && c <= 0xEF ? 2 : c >= 0xF0 && c <= 0xF4 ? 3 : 0;
    uint8_t lo = 0x80, hi = 0xBF;
    if (c == 0xE0) lo = 0xA0; else if (c == 0xED) hi = 0x9F; else if (c == 0xF0) lo = 0x90; else if (c == 0xF4) hi = 0x8F;
    size_t k = i + 1;
    int got = 0;
    while (need && got < need && k < n) {
      uint8_t d = b[k];
      if (got == 0 ? (d < lo || d > hi) : (d < 0x80 || d > 0xBF)) break;
      k++;
      got++;
    }
    if (need && got == need) { memcpy(out + j, b + i, k - i); j += k - i; i = k; continue; }
    memcpy(out + j, "\xEF\xBF\xBD", 3);
    j += 3;
    i = need ? k : i + 1;
  }
  AxStr *s = ax_str_new(out, j);
  free(out);
  return s;
}

// ---------------------------------------------------------------------------------------------
// Characters
// ---------------------------------------------------------------------------------------------

// JavaScript's white space and line terminators: what trim() removes and \s matches.
bool ax_unit_is_space(uint32_t u) {
  return (u >= 0x09 && u <= 0x0D) || u == 0x20 || u == 0xA0 || u == 0x1680 || (u >= 0x2000 && u <= 0x200A) ||
         u == 0x2028 || u == 0x2029 || u == 0x202F || u == 0x205F || u == 0x3000 || u == 0xFEFF;
}

static bool in_ranges(const UcRange *r, int n, uint32_t cp) {
  int lo = 0, hi = n - 1;
  while (lo <= hi) {
    int m = (lo + hi) / 2;
    if (cp < r[m].lo) hi = m - 1;
    else if (cp > r[m].hi) lo = m + 1;
    else return true;
  }
  return false;
}

// cp's case mapping into out (up to 3 code points); returns how many.
static int map_case(uint32_t cp, bool upper, uint32_t out[3]) {
  const UcMulti *mt = upper ? UC_UPPER_MULTI : UC_LOWER_MULTI;
  int mn = upper ? UC_UPPER_MULTI_N : UC_LOWER_MULTI_N;
  int lo = 0, hi = mn - 1;
  while (lo <= hi) {
    int m = (lo + hi) / 2;
    if (cp < mt[m].cp) hi = m - 1;
    else if (cp > mt[m].cp) lo = m + 1;
    else { for (int k = 0; k < mt[m].n; k++) out[k] = mt[m].out[k]; return mt[m].n; }
  }
  const UcRun *rt = upper ? UC_UPPER_RUNS : UC_LOWER_RUNS;
  int rn = upper ? UC_UPPER_RUNS_N : UC_LOWER_RUNS_N;
  lo = 0;
  hi = rn - 1;
  while (lo <= hi) {
    int m = (lo + hi) / 2;
    if (cp < rt[m].start) hi = m - 1;
    else if (cp > rt[m].end) lo = m + 1;
    else {
      out[0] = (cp - rt[m].start) % rt[m].step == 0 ? (uint32_t)((int32_t)cp + rt[m].delta) : cp;
      return 1;
    }
  }
  out[0] = cp;
  return 1;
}

// Σ lowercases to final ς when a cased letter precedes it (past any case-ignorable ones) and
// none follows it (likewise) — Unicode's Final_Sigma condition, as ICU applies it.
static bool final_sigma(const uint32_t *cps, size_t n, size_t i) {
  bool before = false;
  for (size_t j = i; j-- > 0;) {
    if (in_ranges(UC_CASED, UC_CASED_N, cps[j])) { before = true; break; }
    if (!in_ranges(UC_CASE_IGNORABLE, UC_CASE_IGNORABLE_N, cps[j])) break;
  }
  if (!before) return false;
  for (size_t j = i + 1; j < n; j++) {
    if (in_ranges(UC_CASED, UC_CASED_N, cps[j])) return false;
    if (!in_ranges(UC_CASE_IGNORABLE, UC_CASE_IGNORABLE_N, cps[j])) break;
  }
  return true;
}

// String.prototype.toUpperCase / toLowerCase.
AxStr *ax_str_case(const AxStr *s, bool upper) {
  if (s->ascii) {
    AxStr *o = ax_str_new(s->data, s->len);
    for (uint32_t i = 0; i < o->len; i++) {
      char c = o->data[i];
      o->data[i] = upper ? (c >= 'a' && c <= 'z' ? (char)(c - 32) : c) : (c >= 'A' && c <= 'Z' ? (char)(c + 32) : c);
    }
    o->hash = ax_hash_bytes(o->data, o->len);
    return o;
  }
  // Code points (a lone surrogate stands for itself), mapped one by one.
  const uint8_t *b = (const uint8_t *)s->data;
  uint32_t *cps = malloc(sizeof(uint32_t) * (s->len + 1));
  size_t n = 0;
  for (size_t k = 0; k < s->len;) k += (size_t)next_cp(b + k, s->len - k, &cps[n++]);
  uint8_t *o = malloc(s->len * 3 + 4);   // a mapping at most triples a character's bytes
  size_t j = 0;
  for (size_t i = 0; i < n; i++) {
    uint32_t m[3];
    int c;
    if (!upper && cps[i] == 0x3A3) { m[0] = final_sigma(cps, n, i) ? 0x3C2 : 0x3C3; c = 1; }
    else c = map_case(cps[i], upper, m);
    for (int k = 0; k < c; k++) j += (size_t)put_cp(o + j, m[k]);
  }
  AxStr *r = ax_str_new((const char *)o, j);
  free(o);
  free(cps);
  return r;
}

// A malloc'd, NUL-terminated buffer of bytes from outside, decoded as Node decodes UTF-8 (see
// ax_utf8_repair): returned as it is when it is already well-formed, else replaced (and freed).
char *ax_text_repair(char *buf, size_t *n) {
  size_t len = n ? *n : strlen(buf);
  if (ax_bytes_ascii(buf, len)) return buf;
  AxStr *s = ax_utf8_repair(buf, len);
  if (s->len == len && memcmp(s->data, buf, len) == 0) { ax_release(ax_strv(s)); return buf; }
  char *out = malloc(s->len + 1);
  memcpy(out, s->data, s->len + 1);
  if (n) *n = s->len;
  ax_release(ax_strv(s));
  free(buf);
  return out;
}

// ---------------------------------------------------------------------------------------------
// Operations on units
// ---------------------------------------------------------------------------------------------

// Searching by bytes finds exactly what a search by units finds unless the needle holds a lone
// surrogate, which can match half of a pair in the haystack.
bool ax_str_byte_searchable(const AxStr *n) { return n->ascii || !ax_bytes_have_lone(n->data, n->len); }

int64_t ax_u16_find(const uint16_t *h, uint32_t hn, const uint16_t *n, uint32_t nn, uint32_t from) {
  if (nn == 0) return from <= hn ? from : hn;
  for (uint32_t i = from; i + nn <= hn; i++) if (h[i] == n[0] && memcmp(h + i, n, nn * sizeof(uint16_t)) == 0) return i;
  return -1;
}

// String.prototype.indexOf(n): the unit index of the first match, or -1.
int64_t ax_str_find(const AxStr *h, const AxStr *n) {
  if (ax_str_byte_searchable(n)) {
    if (!n->len) return 0;
    if (n->len > h->len) return -1;
    for (uint32_t i = 0; i + n->len <= h->len; i++)
      if (h->data[i] == n->data[0] && memcmp(h->data + i, n->data, n->len) == 0) return ax_str_units_before(h, i);
    return -1;
  }
  uint32_t hn, nn;
  uint16_t *hu = ax_str_units(h, &hn), *nu = ax_str_units(n, &nn);
  int64_t r = ax_u16_find(hu, hn, nu, nn, 0);
  free(hu);
  free(nu);
  return r;
}

// Each unit as a one-unit string: split(""), chars(), iteration — a pair comes apart into its
// two halves, as in the reference.
AxArr *ax_str_unit_strings(const AxStr *s) {
  if (s->ascii) {
    AxArr *a = ax_arr_new(s->len);
    for (uint32_t i = 0; i < s->len; i++) ax_arr_push(a, ax_strv(ax_str_new(s->data + i, 1)));
    return a;
  }
  uint32_t n;
  uint16_t *u = ax_str_units(s, &n);
  AxArr *a = ax_arr_new(n);
  for (uint32_t i = 0; i < n; i++) ax_arr_push(a, ax_strv(ax_str_from_u16(u + i, 1)));
  free(u);
  return a;
}

// The bounds of p[0, n) without JavaScript white space at either end (trim, parseInt, Number).
void ax_trim_bytes(const char *p, size_t n, bool start, bool end, size_t *a_out, size_t *b_out) {
  const uint8_t *b = (const uint8_t *)p;
  size_t a = 0, z = n;
  if (start) {
    while (a < z) {
      uint32_t cp;
      int w = next_cp(b + a, z - a, &cp);
      if (!ax_unit_is_space(cp)) break;
      a += (size_t)w;
    }
  }
  if (end) {
    while (z > a) {
      size_t k = z - 1;
      while (k > a && (b[k] & 0xC0) == 0x80) k--;
      uint32_t cp;
      next_cp(b + k, z - k, &cp);
      if (!ax_unit_is_space(cp)) break;
      z = k;
    }
  }
  *a_out = a;
  *b_out = z;
}

AxStr *ax_str_trim(const AxStr *s, bool start, bool end) {
  size_t a, b;
  ax_trim_bytes(s->data, s->len, start, end, &a, &b);
  return ax_str_new(s->data + a, b - a);
}
