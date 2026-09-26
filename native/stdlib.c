// stdlib.c — the standard library and method dispatch.
//
// The contract is STDLIB.md: same names, same argument order, same total-where-possible
// behaviour (an empty input returns a neutral value rather than raising). Where the reference
// implementation leans on JavaScript — JSON, regular expressions, number formatting — this file
// implements the same observable behaviour directly, because "call out to the host language"
// is not available here and, more to the point, would make the two runtimes disagree the moment
// the host changed.
//
// Regular expressions are POSIX extended, with the handful of JavaScript escapes a program
// actually uses (\d \w \s and their negations, \b) translated on the way in. That covers the
// patterns in the corpus; anything beyond it fails loudly with the pattern quoted, rather than
// silently matching the wrong thing.

#include "axiom.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "jsmath.h"
#include <time.h>
#include <regex.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <ctype.h>

// ---------------------------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------------------------

#define ARG(i) (i < argc ? args[i] : ax_null())
// A numeric argument. A big is refused as Math.* refuses one; the library functions that do
// arithmetic on their arguments instead refuse it as mixing (no_big).
static const char BIG_CONVERT[] = "Cannot convert a BigInt value to a number";
static const char BIG_MIX[] = "Cannot mix BigInt and other types, use explicit conversions";
static double num_arg(AxVM *vm, AxValue v) {
  if (v.t == AX_BIG) ax_throw(vm, "AX-RUNTIME-000", "%s", BIG_CONVERT);
  return ax_to_num(v);
}
static void no_big(AxVM *vm, AxValue *args, int argc, int used) {   // the first `used` arguments
  for (int i = 0; i < argc && i < used; i++) if (args[i].t == AX_BIG) ax_throw(vm, "AX-RUNTIME-000", "%s", BIG_MIX);
}
#define NUM(i) (i < argc ? num_arg(vm, args[i]) : 0)
#define DEF_NUM(i, d) (i < argc && args[i].t != AX_NULL ? num_arg(vm, args[i]) : (d))
#define INT32(i) ax_to_int32(NUM(i))

static AxStr *arg_str(AxValue v) { return ax_to_str(v); }   // +1

static AxValue str_take(AxStr *s) { return ax_strv(s); }

static AxValue num_or_null(double d, bool ok) { return ok ? ax_num(d) : ax_null(); }

static int cmp_values_asc(const void *a, const void *b) {
  return ax_compare(*(const AxValue *)a, *(const AxValue *)b);
}

// Sorting with a key or comparator needs the VM, which qsort cannot carry.
typedef struct { AxVM *vm; AxValue sel; bool comparator; } SortCtx;

static int sort_cmp(SortCtx *c, AxValue a, AxValue b) {
  if (c->comparator) {
    AxValue args2[2] = { a, b };
    AxValue r = ax_call(c->vm, c->sel, args2, 2);
    double d = ax_to_num(r);
    ax_release(r);
    return d < 0 ? -1 : (d > 0 ? 1 : 0);
  }
  if (c->sel.t == AX_NULL) return ax_compare(a, b);
  AxValue ka = ax_key_apply(c->vm, c->sel, a, 0);
  AxValue kb = ax_key_apply(c->vm, c->sel, b, 0);
  int r = ax_compare(ka, kb);
  ax_release(ka);
  ax_release(kb);
  return r;
}

// V8's Array.prototype.sort, step for step (third_party/v8/builtins/array-sort.tq: TimSort with
// binary insertion for short runs and galloping merges). Any stable sort agrees with it for a
// consistent ordering, but mixed-type keys (booleans among numbers) and hand-written comparators
// need not be consistent, and then only the same comparisons in the same order give the same
// result — and call a side-effecting key or comparator the same number of times.
typedef struct {
  SortCtx *c;
  AxValue *a;
  AxValue *tmp;
  int min_gallop;
  int nruns;
  int32_t base[85], len[85];
} TimSort;

#define TS_CMP(x, y) sort_cmp(ts->c, (x), (y))

static int32_t ts_gallop_left(TimSort *ts, AxValue *arr, AxValue key, int32_t base, int32_t length, int32_t hint) {
  int32_t last = 0, ofs = 1;
  if (TS_CMP(arr[base + hint], key) < 0) {
    int32_t max = length - hint;
    while (ofs < max) {
      if (TS_CMP(arr[base + hint + ofs], key) >= 0) break;
      last = ofs;
      ofs = (ofs << 1) + 1;
      if (ofs <= 0) ofs = max;
    }
    if (ofs > max) ofs = max;
    last += hint;
    ofs += hint;
  } else {
    int32_t max = hint + 1;
    while (ofs < max) {
      if (TS_CMP(arr[base + hint - ofs], key) < 0) break;
      last = ofs;
      ofs = (ofs << 1) + 1;
      if (ofs <= 0) ofs = max;
    }
    if (ofs > max) ofs = max;
    int32_t t = last;
    last = hint - ofs;
    ofs = hint - t;
  }
  last++;
  while (last < ofs) {
    int32_t m = last + ((ofs - last) >> 1);
    if (TS_CMP(arr[base + m], key) < 0) last = m + 1;
    else ofs = m;
  }
  return ofs;
}

static int32_t ts_gallop_right(TimSort *ts, AxValue *arr, AxValue key, int32_t base, int32_t length, int32_t hint) {
  int32_t last = 0, ofs = 1;
  if (TS_CMP(key, arr[base + hint]) < 0) {
    int32_t max = hint + 1;
    while (ofs < max) {
      if (TS_CMP(key, arr[base + hint - ofs]) >= 0) break;
      last = ofs;
      ofs = (ofs << 1) + 1;
      if (ofs <= 0) ofs = max;
    }
    if (ofs > max) ofs = max;
    int32_t t = last;
    last = hint - ofs;
    ofs = hint - t;
  } else {
    int32_t max = length - hint;
    while (ofs < max) {
      if (TS_CMP(key, arr[base + hint + ofs]) < 0) break;
      last = ofs;
      ofs = (ofs << 1) + 1;
      if (ofs <= 0) ofs = max;
    }
    if (ofs > max) ofs = max;
    last += hint;
    ofs += hint;
  }
  last++;
  while (last < ofs) {
    int32_t m = last + ((ofs - last) >> 1);
    if (TS_CMP(key, arr[base + m]) < 0) ofs = m;
    else last = m + 1;
  }
  return ofs;
}

#define TS_COPY(src, si, dst, di, n) memmove(&(dst)[di], &(src)[si], sizeof(AxValue) * (size_t)(n))

static void ts_merge_low(TimSort *ts, int32_t base_a, int32_t len_a, int32_t base_b, int32_t len_b) {
  AxValue *a = ts->a, *t = ts->tmp;
  TS_COPY(a, base_a, t, 0, len_a);
  int32_t dest = base_a, ct = 0, cb = base_b;
  a[dest++] = a[cb++];
  if (--len_b == 0) goto succeed;
  if (len_a == 1) goto copy_b;
  int min_gallop = ts->min_gallop;
  for (;;) {
    int32_t wins_a = 0, wins_b = 0;
    for (;;) {
      if (TS_CMP(a[cb], t[ct]) < 0) {
        a[dest++] = a[cb++];
        ++wins_b; --len_b; wins_a = 0;
        if (len_b == 0) goto succeed;
        if (wins_b >= min_gallop) break;
      } else {
        a[dest++] = t[ct++];
        ++wins_a; --len_a; wins_b = 0;
        if (len_a == 1) goto copy_b;
        if (wins_a >= min_gallop) break;
      }
    }
    ++min_gallop;
    bool first = true;
    while (wins_a >= 7 || wins_b >= 7 || first) {
      first = false;
      min_gallop = min_gallop - 1 > 1 ? min_gallop - 1 : 1;
      ts->min_gallop = min_gallop;
      wins_a = ts_gallop_right(ts, t, a[cb], ct, len_a, 0);
      if (wins_a > 0) {
        TS_COPY(t, ct, a, dest, wins_a);
        dest += wins_a; ct += wins_a; len_a -= wins_a;
        if (len_a == 1) goto copy_b;
        if (len_a == 0) goto succeed;
      }
      a[dest++] = a[cb++];
      if (--len_b == 0) goto succeed;
      wins_b = ts_gallop_left(ts, a, t[ct], cb, len_b, 0);
      if (wins_b > 0) {
        TS_COPY(a, cb, a, dest, wins_b);
        dest += wins_b; cb += wins_b; len_b -= wins_b;
        if (len_b == 0) goto succeed;
      }
      a[dest++] = t[ct++];
      if (--len_a == 1) goto copy_b;
    }
    ++min_gallop;
    ts->min_gallop = min_gallop;
  }
succeed:
  if (len_a > 0) TS_COPY(t, ct, a, dest, len_a);
  return;
copy_b:
  // The last element of run A belongs at the end of the merge.
  TS_COPY(a, cb, a, dest, len_b);
  a[dest + len_b] = t[ct];
}

static void ts_merge_high(TimSort *ts, int32_t base_a, int32_t len_a, int32_t base_b, int32_t len_b) {
  AxValue *a = ts->a, *t = ts->tmp;
  TS_COPY(a, base_b, t, 0, len_b);
  int32_t dest = base_b + len_b - 1, ct = len_b - 1, ca = base_a + len_a - 1;
  a[dest--] = a[ca--];
  if (--len_a == 0) goto succeed;
  if (len_b == 1) goto copy_a;
  int min_gallop = ts->min_gallop;
  for (;;) {
    int32_t wins_a = 0, wins_b = 0;
    for (;;) {
      if (TS_CMP(t[ct], a[ca]) < 0) {
        a[dest--] = a[ca--];
        ++wins_a; --len_a; wins_b = 0;
        if (len_a == 0) goto succeed;
        if (wins_a >= min_gallop) break;
      } else {
        a[dest--] = t[ct--];
        ++wins_b; --len_b; wins_a = 0;
        if (len_b == 1) goto copy_a;
        if (wins_b >= min_gallop) break;
      }
    }
    ++min_gallop;
    bool first = true;
    while (wins_a >= 7 || wins_b >= 7 || first) {
      first = false;
      min_gallop = min_gallop - 1 > 1 ? min_gallop - 1 : 1;
      ts->min_gallop = min_gallop;
      int32_t k = ts_gallop_right(ts, a, t[ct], base_a, len_a, len_a - 1);
      wins_a = len_a - k;
      if (wins_a > 0) {
        dest -= wins_a; ca -= wins_a;
        TS_COPY(a, ca + 1, a, dest + 1, wins_a);
        len_a -= wins_a;
        if (len_a == 0) goto succeed;
      }
      a[dest--] = t[ct--];
      if (--len_b == 1) goto copy_a;
      k = ts_gallop_left(ts, t, a[ca], 0, len_b, len_b - 1);
      wins_b = len_b - k;
      if (wins_b > 0) {
        dest -= wins_b; ct -= wins_b;
        TS_COPY(t, ct + 1, a, dest + 1, wins_b);
        len_b -= wins_b;
        if (len_b == 1) goto copy_a;
        if (len_b == 0) goto succeed;
      }
      a[dest--] = a[ca--];
      if (--len_a == 0) goto succeed;
    }
    ++min_gallop;
    ts->min_gallop = min_gallop;
  }
succeed:
  if (len_b > 0) TS_COPY(t, 0, a, dest - (len_b - 1), len_b);
  return;
copy_a:
  dest -= len_a; ca -= len_a;
  TS_COPY(a, ca + 1, a, dest + 1, len_a);
  a[dest] = t[ct];
}

static void ts_merge_at(TimSort *ts, int i) {
  int32_t base_a = ts->base[i], len_a = ts->len[i], base_b = ts->base[i + 1], len_b = ts->len[i + 1];
  ts->len[i] = len_a + len_b;
  if (i == ts->nruns - 3) { ts->base[i + 1] = ts->base[i + 2]; ts->len[i + 1] = ts->len[i + 2]; }
  ts->nruns--;
  int32_t k = ts_gallop_right(ts, ts->a, ts->a[base_b], base_a, len_a, 0);
  base_a += k;
  len_a -= k;
  if (len_a == 0) return;
  len_b = ts_gallop_left(ts, ts->a, ts->a[base_a + len_a - 1], base_b, len_b, len_b - 1);
  if (len_b == 0) return;
  if (len_a <= len_b) ts_merge_low(ts, base_a, len_a, base_b, len_b);
  else ts_merge_high(ts, base_a, len_a, base_b, len_b);
}

static bool ts_invariant(TimSort *ts, int n) {
  if (n < 2) return true;
  return ts->len[n - 2] > ts->len[n - 1] + ts->len[n];
}

static void ts_merge_collapse(TimSort *ts) {
  while (ts->nruns > 1) {
    int n = ts->nruns - 2;
    if (!ts_invariant(ts, n + 1) || !ts_invariant(ts, n)) {
      if (n > 0 && ts->len[n - 1] < ts->len[n + 1]) --n;
      ts_merge_at(ts, n);
    } else if (ts->len[n] <= ts->len[n + 1]) {
      ts_merge_at(ts, n);
    } else {
      break;
    }
  }
}

static int32_t ts_count_run(TimSort *ts, int32_t low_arg, int32_t high) {
  AxValue *a = ts->a;
  int32_t low = low_arg + 1;
  if (low == high) return 1;
  int32_t run = 2;
  bool descending = TS_CMP(a[low], a[low - 1]) < 0;
  AxValue prev = a[low];
  for (int32_t i = low + 1; i < high; ++i) {
    int order = TS_CMP(a[i], prev);
    if (descending ? order >= 0 : order < 0) break;
    prev = a[i];
    ++run;
  }
  if (descending) {
    for (int32_t i = low_arg, j = low_arg + run - 1; i < j; i++, j--) { AxValue x = a[i]; a[i] = a[j]; a[j] = x; }
  }
  return run;
}

static void ts_binary_insertion(TimSort *ts, int32_t low, int32_t start, int32_t high) {
  AxValue *a = ts->a;
  if (low == start) start++;
  for (; start < high; ++start) {
    int32_t left = low, right = start;
    AxValue pivot = a[start];
    while (left < right) {
      int32_t mid = left + ((right - left) >> 1);
      if (TS_CMP(pivot, a[mid]) < 0) right = mid;
      else left = mid + 1;
    }
    for (int32_t p = start; p > left; --p) a[p] = a[p - 1];
    a[left] = pivot;
  }
}

static void timsort(SortCtx *c, AxValue *items, uint32_t n) {
  if (n < 2) return;
  // As in V8, the sort runs on a work copy that is written back only when it finishes, so a key
  // or comparator that throws leaves the array as it was (and never with a slot twice).
  AxValue *work = malloc(sizeof(AxValue) * n * 2);
  memcpy(work, items, sizeof(AxValue) * n);
  TimSort ts = { c, work, work + n, 7, 0, {0}, {0} };
  int32_t remaining = (int32_t)n, low = 0, min_run;
  {
    int32_t m = remaining, r = 0;
    while (m >= 64) { r |= m & 1; m >>= 1; }
    min_run = m + r;
  }
  while (remaining != 0) {
    int32_t run = ts_count_run(&ts, low, low + remaining);
    if (run < min_run) {
      int32_t forced = min_run < remaining ? min_run : remaining;
      ts_binary_insertion(&ts, low, low + run, low + forced);
      run = forced;
    }
    ts.base[ts.nruns] = low;
    ts.len[ts.nruns] = run;
    ts.nruns++;
    ts_merge_collapse(&ts);
    low += run;
    remaining -= run;
  }
  while (ts.nruns > 1) {
    int k = ts.nruns - 2;
    if (k > 0 && ts.len[k - 1] < ts.len[k + 1]) --k;
    ts_merge_at(&ts, k);
  }
  memcpy(items, work, sizeof(AxValue) * n);
  free(work);
}
#undef TS_CMP
#undef TS_COPY

static AxArr *sorted_copy(AxVM *vm, AxValue seqv, AxValue sel) {
  AxArr *src = ax_to_seq(vm, seqv);
  AxArr *out = ax_arr_new(src->len);
  for (uint32_t i = 0; i < src->len; i++) ax_arr_push(out, ax_copy(src->items[i]));
  ax_release(ax_arrv(src));
  if (out->len > 1) {
    SortCtx c = { vm, sel, false };
    if (sel.t == AX_FN) {
      AxFn *f = (AxFn *)sel.o;
      c.comparator = (!f->native && f->nparams >= 2) || (f->native && f->min_args >= 2);
    }
    timsort(&c, out->items, out->len);
  }
  return out;
}

// ---------------------------------------------------------------------------------------------
// JSON
// ---------------------------------------------------------------------------------------------

typedef struct { char *buf; size_t len, cap; bool bigint; } SB;   // bigint: JSON.stringify would throw

static void sb_add(SB *sb, const char *s, size_t n) {
  if (sb->len + n + 1 > sb->cap) {
    while (sb->len + n + 1 > sb->cap) sb->cap = sb->cap ? sb->cap * 2 : 128;
    sb->buf = realloc(sb->buf, sb->cap);
  }
  memcpy(sb->buf + sb->len, s, n);
  sb->len += n;
  sb->buf[sb->len] = '\0';
}
static void sb_addz(SB *sb, const char *s) { sb_add(sb, s, strlen(s)); }

static void json_quote(SB *sb, AxStr *s) {
  sb_addz(sb, "\"");
  for (uint32_t i = 0; i < s->len; i++) {
    unsigned char c = (unsigned char)s->data[i];
    switch (c) {
      case '"': sb_addz(sb, "\\\""); break;
      case '\\': sb_addz(sb, "\\\\"); break;
      case '\n': sb_addz(sb, "\\n"); break;
      case '\t': sb_addz(sb, "\\t"); break;
      case '\r': sb_addz(sb, "\\r"); break;
      default:
        if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); sb_addz(sb, b); }
        else sb_add(sb, (const char *)&c, 1);
    }
  }
  sb_addz(sb, "\"");
}


typedef struct { const char *p; bool ok; } JP;

static void jp_ws(JP *j) { while (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r') j->p++; }

static AxValue json_read(JP *j) {
  jp_ws(j);
  char c = *j->p;
  if (c == '\0') { j->ok = false; return ax_null(); }
  if (c == 'n' && strncmp(j->p, "null", 4) == 0) { j->p += 4; return ax_null(); }
  if (c == 't' && strncmp(j->p, "true", 4) == 0) { j->p += 4; return ax_bool(true); }
  if (c == 'f' && strncmp(j->p, "false", 5) == 0) { j->p += 5; return ax_bool(false); }
  if (c == '"') {
    // JSON.parse's strings: no raw control characters, only the defined escapes, \u with four
    // hex digits (a surrogate pair becomes one code point).
    j->p++;
    SB sb = {0};
    sb_add(&sb, "", 0);
    while (*j->p && *j->p != '"') {
      if ((unsigned char)*j->p < 0x20) { j->ok = false; free(sb.buf); return ax_null(); }
      if (*j->p == '\\') {
        j->p++;
        char e = *j->p++;
        switch (e) {
          case 'n': sb_addz(&sb, "\n"); break;
          case 't': sb_addz(&sb, "\t"); break;
          case 'r': sb_addz(&sb, "\r"); break;
          case 'b': sb_addz(&sb, "\b"); break;
          case 'f': sb_addz(&sb, "\f"); break;
          case '"': case '\\': case '/': sb_add(&sb, &e, 1); break;
          case 'u': {
            unsigned cp = 0;
            for (int k = 0; k < 4; k++) {
              char h = j->p[k];
              int v = h >= '0' && h <= '9' ? h - '0' : h >= 'a' && h <= 'f' ? h - 'a' + 10 : h >= 'A' && h <= 'F' ? h - 'A' + 10 : -1;
              if (v < 0) { j->ok = false; free(sb.buf); return ax_null(); }
              cp = cp * 16 + (unsigned)v;
            }
            j->p += 4;
            if (cp >= 0xD800 && cp < 0xDC00 && j->p[0] == '\\' && j->p[1] == 'u') {
              unsigned lo = 0;
              bool hex = true;
              for (int k = 0; k < 4; k++) {
                char h = j->p[2 + k];
                int v = h >= '0' && h <= '9' ? h - '0' : h >= 'a' && h <= 'f' ? h - 'a' + 10 : h >= 'A' && h <= 'F' ? h - 'A' + 10 : -1;
                if (v < 0) { hex = false; break; }
                lo = lo * 16 + (unsigned)v;
              }
              if (hex && lo >= 0xDC00 && lo < 0xE000) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); j->p += 6; }
            }
            char b[4];
            int nb;
            if (cp < 0x80) { b[0] = (char)cp; nb = 1; }
            else if (cp < 0x800) { b[0] = (char)(0xC0 | (cp >> 6)); b[1] = (char)(0x80 | (cp & 0x3F)); nb = 2; }
            else if (cp < 0x10000) { b[0] = (char)(0xE0 | (cp >> 12)); b[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); b[2] = (char)(0x80 | (cp & 0x3F)); nb = 3; }
            else { b[0] = (char)(0xF0 | (cp >> 18)); b[1] = (char)(0x80 | ((cp >> 12) & 0x3F)); b[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); b[3] = (char)(0x80 | (cp & 0x3F)); nb = 4; }
            sb_add(&sb, b, (size_t)nb);
            break;
          }
          default: j->ok = false; free(sb.buf); return ax_null();
        }
        continue;
      }
      sb_add(&sb, j->p, 1);
      j->p++;
    }
    if (*j->p != '"') { j->ok = false; free(sb.buf); return ax_null(); }
    j->p++;
    AxValue v = ax_strv(ax_str_new(sb.buf ? sb.buf : "", sb.len));
    free(sb.buf);
    return v;
  }
  if (c == '[') {
    j->p++;
    AxArr *a = ax_arr_new(4);
    jp_ws(j);
    if (*j->p == ']') { j->p++; return ax_arrv(a); }
    for (;;) {
      ax_arr_push(a, json_read(j));
      if (!j->ok) break;
      jp_ws(j);
      if (*j->p == ',') { j->p++; continue; }
      if (*j->p == ']') { j->p++; break; }
      j->ok = false;
      break;
    }
    return ax_arrv(a);
  }
  if (c == '{') {
    j->p++;
    AxDict *d = ax_dict_new();
    jp_ws(j);
    if (*j->p == '}') { j->p++; return ax_dictv(d); }
    for (;;) {
      jp_ws(j);
      AxValue k = json_read(j);
      if (!j->ok || k.t != AX_STR) { j->ok = false; ax_release(k); break; }
      jp_ws(j);
      if (*j->p != ':') { j->ok = false; ax_release(k); break; }
      j->p++;
      AxValue v = json_read(j);
      AxStr *key = ax_intern(((AxStr *)k.o)->data, ((AxStr *)k.o)->len);
      if (strcmp(key->data, "__type") == 0 && v.t == AX_STR && !d->type_tag) {
        // `"__type": "P"` is how a record serializes; reading it back makes the record again.
        d->type_tag = ax_intern(((AxStr *)v.o)->data, ((AxStr *)v.o)->len);
        ax_release(v);
      } else {
        ax_dict_set(d, key, v);
      }
      ax_release(ax_strv(key));
      ax_release(k);
      if (!j->ok) break;
      jp_ws(j);
      if (*j->p == ',') { j->p++; continue; }
      if (*j->p == '}') { j->p++; break; }
      j->ok = false;
      break;
    }
    return ax_dictv(d);
  }
  // A JSON number: -?(0|[1-9]\d*)(\.\d+)?([eE][+-]?\d+)? — no Infinity, hex, '+' or '.5'.
  const char *q = j->p;
  if (*q == '-') q++;
  if (*q == '0') q++;
  else if (*q >= '1' && *q <= '9') { while (*q >= '0' && *q <= '9') q++; }
  else { j->ok = false; return ax_null(); }
  if (*q == '.') {
    q++;
    if (!(*q >= '0' && *q <= '9')) { j->ok = false; return ax_null(); }
    while (*q >= '0' && *q <= '9') q++;
  }
  if (*q == 'e' || *q == 'E') {
    q++;
    if (*q == '+' || *q == '-') q++;
    if (!(*q >= '0' && *q <= '9')) { j->ok = false; return ax_null(); }
    while (*q >= '0' && *q <= '9') q++;
  }
  double d = strtod(j->p, NULL);
  j->p = q;
  return ax_num(d);
}

// ---------------------------------------------------------------------------------------------
// Regular expressions
// ---------------------------------------------------------------------------------------------
//
// Translate the JavaScript-flavoured escapes a program is likely to write into POSIX ERE.
// Anything else passes through unchanged, so a genuinely POSIX pattern still works.
static char *re_translate(const char *pat) {
  size_t n = strlen(pat);
  char *out = malloc(n * 12 + 1);
  size_t j = 0;
  bool in_class = false;
  for (size_t i = 0; i < n; i++) {
    char c = pat[i];
    // JavaScript's [] matches nothing and [^] anything; POSIX has neither (a C string holds
    // no NUL byte, so a class of every other byte stands in).
    if (c == '[' && !in_class && (i == 0 || pat[i - 1] != '\\')) {
      const char *cls = pat[i + 1] == ']' ? "[^\x01-\xff]" : (pat[i + 1] == '^' && pat[i + 2] == ']') ? "[\x01-\xff]" : NULL;
      if (cls) { size_t cl = strlen(cls); memcpy(out + j, cls, cl); j += cl; i += pat[i + 1] == ']' ? 1 : 2; continue; }
    }
    if (c == '[' && (i == 0 || pat[i - 1] != '\\')) in_class = true;
    else if (c == ']' && in_class) in_class = false;
    // A brace that does not make a quantifier ({n}, {n,}, {n,m} after something to repeat) is
    // a literal in JavaScript; POSIX would reject it.
    if (c == '{' && !in_class) {
      size_t k = i + 1;
      bool digits = false;
      while (pat[k] >= '0' && pat[k] <= '9') { k++; digits = true; }
      if (digits && pat[k] == ',') { k++; while (pat[k] >= '0' && pat[k] <= '9') k++; }
      bool quant = digits && pat[k] == '}' && j > 0 && out[j - 1] != '(' && out[j - 1] != '|';
      if (!quant) { out[j++] = '\\'; out[j++] = '{'; continue; }
    }
    if (c == '\\' && i + 1 < n) {
      char e = pat[++i];
      const char *rep = NULL;
      switch (e) {
        case 'd': rep = in_class ? "0-9" : "[0-9]"; break;
        case 'D': rep = in_class ? "^0-9" : "[^0-9]"; break;
        case 'w': rep = in_class ? "A-Za-z0-9_" : "[A-Za-z0-9_]"; break;
        case 'W': rep = in_class ? "^A-Za-z0-9_" : "[^A-Za-z0-9_]"; break;
        case 's': rep = in_class ? " \t\n\r\f\v" : "[ \t\n\r\f\v]"; break;
        case 'S': rep = in_class ? "^ \t\n\r\f\v" : "[^ \t\n\r\f\v]"; break;
        case 'n': rep = "\n"; break;
        case 't': rep = "\t"; break;
        default: out[j++] = '\\'; out[j++] = e; continue;
      }
      size_t rl = strlen(rep);
      memcpy(out + j, rep, rl);
      j += rl;
      continue;
    }
    out[j++] = c;
  }
  out[j] = '\0';
  return out;
}

// A regex's flags, as new RegExp checks them: letters from "dgimsuyv", none twice (re_all and
// re_sub add "g" first). Only "i" changes what POSIX matching does.
static bool re_flags(AxVM *vm, AxValue patv, AxValue flagsv, bool add_g) {
  AxStr *f = flagsv.t == AX_NULL ? ax_str_newz("") : ax_to_str(flagsv);
  char buf[64];
  snprintf(buf, sizeof buf, "%s%s", f->data, add_g && !strchr(f->data, 'g') ? "g" : "");
  bool seen[128] = { 0 }, ok = strlen(f->data) < 60;
  for (const char *c = buf; *c && ok; c++) {
    ok = strchr("dgimsuyv", *c) && !seen[(unsigned char)*c];
    if (ok) seen[(unsigned char)*c] = true;
  }
  ax_release(ax_strv(f));
  if (!ok) {
    AxStr *p = ax_to_str(patv);
    char patbuf[256];
    snprintf(patbuf, sizeof patbuf, "%s", p->data);
    ax_release(ax_strv(p));
    ax_throw(vm, "AX-REGEX", "bad regular expression /%s/: Invalid flags supplied to RegExp constructor '%s'", patbuf, buf);
  }
  return seen['i'];
}

static bool re_build(AxVM *vm, AxValue patv, regex_t *re, bool icase) {
  AxStr *p = ax_to_str(patv);
  char *translated = re_translate(p->data);
  int flags = REG_EXTENDED | (icase ? REG_ICASE : 0);
  int rc = regcomp(re, translated, flags);
  free(translated);
  if (rc != 0) {
    char buf[256];
    regerror(rc, re, buf, sizeof buf);
    char patbuf[256];
    snprintf(patbuf, sizeof patbuf, "%s", p->data);
    ax_release(ax_strv(p));
    ax_throw(vm, "AX-REGEX", "bad regular expression /%s/: %s", patbuf, buf);
    return false;
  }
  ax_release(ax_strv(p));
  return true;
}

static AxValue re_match_record(const char *subject, regmatch_t *m, int ngroups) {
  AxDict *d = ax_dict_new();
  AxStr *k_match = ax_internz("match"), *k_index = ax_internz("index"), *k_groups = ax_internz("groups");
  ax_dict_set(d, k_match, ax_strv(ax_str_new(subject + m[0].rm_so, m[0].rm_eo - m[0].rm_so)));
  ax_dict_set(d, k_index, ax_num(m[0].rm_so));
  AxArr *groups = ax_arr_new(ngroups);
  for (int g = 1; g <= ngroups; g++) {
    if (m[g].rm_so < 0) ax_arr_push(groups, ax_null());
    else ax_arr_push(groups, ax_strv(ax_str_new(subject + m[g].rm_so, m[g].rm_eo - m[g].rm_so)));
  }
  ax_dict_set(d, k_groups, ax_arrv(groups));
  ax_release(ax_strv(k_match)); ax_release(ax_strv(k_index)); ax_release(ax_strv(k_groups));
  return ax_dictv(d);
}

// ---------------------------------------------------------------------------------------------
// Sandbox
// ---------------------------------------------------------------------------------------------

static bool path_allowed(AxArr *list, const char *path) {
  char real[4096];
  if (!ax_realpath(path, real)) snprintf(real, sizeof real, "%s", path);
  for (uint32_t i = 0; i < list->len; i++) {
    AxStr *p = (AxStr *)list->items[i].o;
    char allowed[4096];
    if (!ax_realpath(p->data, allowed)) snprintf(allowed, sizeof allowed, "%s", p->data);
    size_t al = strlen(allowed);
    if (strncmp(real, allowed, al) == 0 && (real[al] == '\0' || real[al] == '/')) return true;
  }
  return false;
}

bool ax_sandbox_can_read(AxVM *vm, const char *path) { return !vm->sandbox || path_allowed(vm->allow_read, path); }

static void check_read(AxVM *vm, const char *path) {
  if (!vm->sandbox) return;
  if (path_allowed(vm->allow_read, path)) return;
  ax_throw(vm, "AX-SANDBOX-001", "sandbox: reading '%s' is not allowed (pass --allow-read %s)", path, path);
}

static void check_write(AxVM *vm, const char *path) {
  if (!vm->sandbox) return;
  if (path_allowed(vm->allow_write, path)) return;
  ax_throw(vm, "AX-SANDBOX-001", "sandbox: writing '%s' is not allowed (pass --allow-write %s)", path, path);
}

// ---------------------------------------------------------------------------------------------
// Native functions
// ---------------------------------------------------------------------------------------------

#define NATIVE(name) static AxValue name(AxVM *vm, AxFn *self, AxValue *args, int argc)

NATIVE(n_print) {
  // Every argument is rendered first, then the line is written once: into the engine's log
  // (the `log` of --json output) when a world is running, and to stdout unless --json owns it.
  SB sb = {0};
  sb_addz(&sb, "");
  for (int i = 0; i < argc; i++) {
    if (i) sb_addz(&sb, " ");
    AxStr *s = ax_to_str(args[i]);
    sb_add(&sb, s->data, s->len);
    ax_release(ax_strv(s));
  }
  AxStr *msg = ax_str_new(sb.buf, sb.len);
  free(sb.buf);
  if (!ax_engine_log_msg(vm, msg)) ax_write_line(vm, 1, msg->data, msg->len);
  ax_release(ax_strv(msg));
  return ax_null();
}

NATIVE(n_eprint) {
  SB sb = {0};
  sb_addz(&sb, "");
  for (int i = 0; i < argc; i++) {
    if (i) sb_addz(&sb, " ");
    AxStr *s = ax_to_str(args[i]);
    sb_add(&sb, s->data, s->len);
    ax_release(ax_strv(s));
  }
  fflush(stdout);
  ax_write_line(vm, 2, sb.buf, sb.len);
  free(sb.buf);
  return ax_null();
}

NATIVE(n_len) {
  // Strings, arrays, dicts (a record's fields), ranges, containers and vectors (components);
  // anything else — a number, an atom, a function — has no length.
  AxValue v = ARG(0);
  switch (v.t) {
    case AX_STR: return ax_num(((AxStr *)v.o)->len);
    case AX_VEC2: return ax_num(2);
    case AX_VEC3: return ax_num(3);
    case AX_QUAT: return ax_num(4);
    case AX_ARR: return ax_num(((AxArr *)v.o)->len);
    case AX_DICT: return ax_num(ax_dict_count((AxDict *)v.o));
    case AX_HOST: return ax_num(ax_host_len(v));
    case AX_RANGE: {
      AxRange *r = (AxRange *)v.o;
      double n = ceil((r->hi - r->lo) / r->step);
      return ax_num(n > 0 ? n : 0);
    }
    default: return ax_num(0);
  }
}

// The bounds are read as Number(x) reads them (a big included), as the reference's Range does.
#define RNUM(i) ax_to_num(args[i])
NATIVE(n_range) {
  if (argc == 1) return ax_range(0, RNUM(0), 1);
  if (argc >= 3) return ax_range(RNUM(0), RNUM(1), RNUM(2));
  return ax_range(RNUM(0), RNUM(1), 1);
}

NATIVE(n_str) { AxStr *s = ax_to_str(ARG(0)); return str_take(s); }
NATIVE(n_type) {
  AxValue v = ARG(0);
  if (v.t == AX_DICT && ((AxDict *)v.o)->type_tag) {
    AxStr *t = ((AxDict *)v.o)->type_tag;
    ax_retain(ax_strv(t));
    return ax_strv(t);
  }
  return ax_str_from(ax_type_name(v));
}
NATIVE(n_num) { double d = ax_to_num(ARG(0)); return ax_num(isnan(d) ? 0 : d); }

// JavaScript's Number(v) where it differs from ax_to_num: an array is its one element (or 0
// when empty), read as text.
static double js_number(AxValue v) {
  if (v.t != AX_ARR) return ax_to_num(v);
  AxArr *a = (AxArr *)v.o;
  if (a->len == 0) return 0;
  if (a->len > 1) return NAN;
  AxValue e = a->items[0];
  if (e.t == AX_NULL) return 0;
  if (e.t == AX_NUM || e.t == AX_STR || e.t == AX_ARR) return js_number(e);
  return NAN;
}

// big(v): BigInt(v.trim()) for text, BigInt(Math.trunc(Number(v))) otherwise; 0 when that fails.
NATIVE(n_big) {
  AxValue v = ARG(0);
  if (v.t == AX_STR) {
    AxStr *s = (AxStr *)v.o;
    AxBig *b = ax_big_parse(s->data, s->len);
    return ax_bigv(b ? b : ax_big_from_int(0));
  }
  double d = argc ? js_number(v) : NAN;
  return ax_bigv(isfinite(d) ? ax_big_from_double(d) : ax_big_from_int(0));
}
NATIVE(n_is_big) { return ax_bool(ARG(0).t == AX_BIG); }
// parseInt(text, radix): leading white space and a sign, "0x" when the radix is 16 or not
// given, then the longest run of digits; NaN when there are none. Exactly rounded.
static double js_parse_int(const char *s, size_t len, int radix) {
  size_t i = 0, w;
  while (i < len && (w = ax_js_space(s, i, len))) i += w;
  bool neg = false;
  if (i < len && (s[i] == '+' || s[i] == '-')) { neg = s[i] == '-'; i++; }
  bool strip = true;
  if (radix != 0) {
    if (radix < 2 || radix > 36) return NAN;
    if (radix != 16) strip = false;
  } else radix = 10;
  if (strip && i + 1 < len && s[i] == '0' && (s[i + 1] == 'x' || s[i + 1] == 'X')) { i += 2; radix = 16; }
  size_t start = i;
  for (; i < len; i++) {
    char c = s[i];
    int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'z' ? c - 'a' + 10 : c >= 'A' && c <= 'Z' ? c - 'A' + 10 : 99;
    if (d >= radix) break;
  }
  if (i == start) return NAN;
  AxBig *b = ax_big_parse_digits(s + start, i - start, (unsigned)radix);
  double v = ax_big_to_double(b);
  free(b);
  return neg ? -v : v;
}

// parseFloat(text): the longest prefix that is a decimal literal (or Infinity); NaN otherwise.
static double js_parse_float(const char *s, size_t len) {
  size_t i = 0, w;
  while (i < len && (w = ax_js_space(s, i, len))) i += w;
  size_t start = i;
  if (i < len && (s[i] == '+' || s[i] == '-')) i++;
  if (len - i >= 8 && !strncmp(s + i, "Infinity", 8)) return s[start] == '-' ? -INFINITY : INFINITY;
  size_t digits = 0;
  while (i < len && s[i] >= '0' && s[i] <= '9') { i++; digits++; }
  if (i < len && s[i] == '.') {
    size_t j = i + 1, frac = 0;
    while (j < len && s[j] >= '0' && s[j] <= '9') { j++; frac++; }
    if (digits || frac) { i = j; digits += frac; }
  }
  if (!digits) return NAN;
  if (i < len && (s[i] == 'e' || s[i] == 'E')) {
    size_t j = i + 1;
    if (j < len && (s[j] == '+' || s[j] == '-')) j++;
    size_t exp = 0;
    while (j < len && s[j] >= '0' && s[j] <= '9') { j++; exp++; }
    if (exp) i = j;
  }
  char *text = malloc(i - start + 1);
  memcpy(text, s + start, i - start);
  text[i - start] = '\0';
  double v = strtod(text, NULL);
  free(text);
  return v;
}

// int(s) is parseInt(s, 10) and float(s) parseFloat(s) of the text (a non-string as it
// displays): NaN when there is no number.
NATIVE(n_int) {
  AxStr *s = ax_to_str(ARG(0));
  double v = js_parse_int(s->data, s->len, 10);
  ax_release(ax_strv(s));
  return ax_num(v);
}
NATIVE(n_float) {
  AxStr *s = ax_to_str(ARG(0));
  double v = js_parse_float(s->data, s->len);
  ax_release(ax_strv(s));
  return ax_num(v);
}
// parse_int(s, radix): parseInt(String(s).trim(), radix || 10), null for NaN.
NATIVE(n_parse_int) {
  AxStr *s = ax_to_str(ARG(0));
  double r = argc > 1 ? NUM(1) : 0;
  int radix = (r == 0 || isnan(r)) ? 10 : ax_to_int32(r);
  double v = js_parse_int(s->data, s->len, radix);
  ax_release(ax_strv(s));
  return num_or_null(v, !isnan(v));
}

NATIVE(n_is_null) { return ax_bool(ARG(0).t == AX_NULL); }
NATIVE(n_is_number) { return ax_bool(ARG(0).t == AX_NUM); }
NATIVE(n_is_string) { return ax_bool(ARG(0).t == AX_STR); }
NATIVE(n_is_array) { return ax_bool(ARG(0).t == AX_ARR); }
NATIVE(n_is_dict) { return ax_bool(ARG(0).t == AX_DICT); }
NATIVE(n_is_bool) { return ax_bool(ARG(0).t == AX_BOOL); }
NATIVE(n_is_fn) { return ax_bool(ARG(0).t == AX_FN); }
// Number.isInteger / isNaN / isFinite: true only of numbers.
NATIVE(n_is_int) { AxValue v = ARG(0); return ax_bool(v.t == AX_NUM && v.num == floor(v.num) && isfinite(v.num)); }
NATIVE(n_is_nan) { AxValue v = ARG(0); return ax_bool(v.t == AX_NUM && isnan(v.num)); }
NATIVE(n_is_finite) { AxValue v = ARG(0); return ax_bool(v.t == AX_NUM && isfinite(v.num)); }
NATIVE(n_is_empty) {
  AxValue v = ARG(0);
  switch (v.t) {
    case AX_NULL: return ax_bool(true);
    case AX_STR: return ax_bool(((AxStr *)v.o)->len == 0);
    case AX_ARR: return ax_bool(((AxArr *)v.o)->len == 0);
    case AX_DICT: return ax_bool(ax_dict_count((AxDict *)v.o) == 0);
    default: return ax_bool(false);
  }
}

// --- maths ------------------------------------------------------------------------------------
#define MATH1(fname, expr) NATIVE(fname) { double x = NUM(0); (void)x; return ax_num(expr); }
MATH1(n_abs, fabs(x))
MATH1(n_floor, floor(x))
MATH1(n_ceil, ceil(x))
MATH1(n_sqrt, sqrt(x))
MATH1(n_cbrt, cbrt(x))
MATH1(n_sin, sin(x))
MATH1(n_cos, cos(x))
MATH1(n_tan, tan(x))
MATH1(n_asin, asin(x))
MATH1(n_acos, acos(x))
MATH1(n_atan, atan(x))
MATH1(n_sinh, sinh(x))
MATH1(n_cosh, cosh(x))
MATH1(n_tanh, tanh(x))
MATH1(n_exp, exp(x))
MATH1(n_log, log(x))
MATH1(n_log2, log2(x))
MATH1(n_log10, log10(x))
MATH1(n_log1p, log1p(x))
MATH1(n_trunc, trunc(x))
MATH1(n_sign, isnan(x) ? NAN : x == 0 ? x : (x > 0) - (x < 0))   // Math.sign: NaN and -0 kept
MATH1(n_fract, x - floor(x))
MATH1(n_clamp01, x < 0 ? 0 : (x > 1 ? 1 : x))
NATIVE(n_deg2rad) { no_big(vm, args, argc, 1); return ax_num(NUM(0) * M_PI / 180.0); }
NATIVE(n_rad2deg) { no_big(vm, args, argc, 1); return ax_num(NUM(0) * 180.0 / M_PI); }
MATH1(n_isqrt, floor(sqrt(x < 0 ? 0 : x)))

NATIVE(n_round) {
  // JavaScript rounds .5 toward +Infinity; C's round() rounds away from zero. The difference
  // shows up on negative halves, so match JavaScript.
  double x = NUM(0);
  return ax_num(floor(x + 0.5));
}
NATIVE(n_pow) { return ax_num(pow(NUM(0), NUM(1))); }
NATIVE(n_atan2) { return ax_num(atan2(NUM(0), NUM(1))); }
NATIVE(n_hypot) { return ax_num(hypot(NUM(0), NUM(1))); }
NATIVE(n_pi) { return ax_num(M_PI); }
NATIVE(n_tau) { return ax_num(2 * M_PI); }
NATIVE(n_e) { return ax_num(M_E); }
NATIVE(n_inf) { return ax_num(INFINITY); }
NATIVE(n_nan) { return ax_num(NAN); }

// Math.min / Math.max: NaN if any argument is NaN, and -0 below +0.
static double js_min2(double a, double b) {
  if (isnan(a) || isnan(b)) return NAN;
  if (a == 0 && b == 0) return signbit(a) ? a : b;
  return a < b ? a : b;
}
static double js_max2(double a, double b) {
  if (isnan(a) || isnan(b)) return NAN;
  if (a == 0 && b == 0) return signbit(a) ? b : a;
  return a > b ? a : b;
}
static AxValue min_max(AxVM *vm, AxValue *args, int argc, bool max) {
  AxValue *xs = args;
  int n = argc;
  if (argc == 1 && args[0].t == AX_ARR) {
    AxArr *a = (AxArr *)args[0].o;
    if (!a->len) return ax_null();
    xs = a->items;
    n = (int)a->len;
  }
  double best = max ? -INFINITY : INFINITY;
  for (int i = 0; i < n; i++) {
    double d = num_arg(vm, xs[i]);
    best = max ? js_max2(best, d) : js_min2(best, d);
  }
  return ax_num(best);
}
NATIVE(n_min) { return min_max(vm, args, argc, false); }
NATIVE(n_max) { return min_max(vm, args, argc, true); }
NATIVE(n_clamp) { return ax_num(js_max2(NUM(1), js_min2(NUM(2), NUM(0)))); }   // Math.max(lo, Math.min(hi, x))
NATIVE(n_lerp) { double a = NUM(0), b = NUM(1), t = NUM(2); return ax_num(a + (b - a) * t); }
NATIVE(n_inv_lerp) { double a = NUM(0), b = NUM(1), v = NUM(2); return ax_num(a == b ? 0 : (v - a) / (b - a)); }
NATIVE(n_map_range) {
  double v = NUM(0), il = NUM(1), ih = NUM(2), ol = NUM(3), oh = NUM(4);
  if (ih == il) return ax_num(ol);
  return ax_num(ol + (v - il) / (ih - il) * (oh - ol));
}
NATIVE(n_smoothstep) {
  double lo = NUM(0), hi = NUM(1), x = NUM(2);
  if (hi == lo) return ax_num(x >= hi ? 1 : 0);
  double t = (x - lo) / (hi - lo);
  if (t < 0) t = 0;
  if (t > 1) t = 1;
  return ax_num(t * t * (3 - 2 * t));
}
NATIVE(n_wrap) {
  double x = NUM(0), lo = NUM(1), hi = NUM(2);
  if (hi == lo) return ax_num(lo);
  double r = hi - lo;
  return ax_num(lo + fmod(fmod(x - lo, r) + r, r));
}
NATIVE(n_mod) { no_big(vm, args, argc, 2); double a = NUM(0), b = NUM(1); if (b == 0) return ax_num(0); return ax_num(fmod(fmod(a, b) + b, b)); }
NATIVE(n_divmod) {
  no_big(vm, args, argc, 2);
  double a = NUM(0), b = NUM(1);
  AxArr *out = ax_arr_new(2);
  if (b == 0) { ax_arr_push(out, ax_num(0)); ax_arr_push(out, ax_num(0)); return ax_arrv(out); }
  ax_arr_push(out, ax_num(floor(a / b)));
  ax_arr_push(out, ax_num(fmod(fmod(a, b) + b, b)));
  return ax_arrv(out);
}
NATIVE(n_gcd) {
  // Math.abs(a | 0): 32-bit, as in the reference.
  no_big(vm, args, argc, 2);
  int64_t a = llabs((int64_t)INT32(0)), b = llabs((int64_t)INT32(1));
  while (b) { int64_t t = b; b = a % b; a = t; }
  return ax_num((double)a);
}
NATIVE(n_lcm) {
  // Math.abs(a | 0) and the same for b, as the reference: NaN and fractions go through ToInt32.
  no_big(vm, args, argc, 2);
  int64_t a = llabs((int64_t)INT32(0)), b = llabs((int64_t)INT32(1));
  if (!a || !b) return ax_num(0);
  int64_t x = a, y = b;
  while (y) { int64_t t = y; y = x % y; x = t; }
  return ax_num((double)(a / x) * (double)b);
}
NATIVE(n_fact) {
  double x = floor(NUM(0));
  if (isnan(x)) return ax_num(NAN);
  if (x < 0) return ax_num(0);
  if (x > 170) return ax_num(INFINITY);   // past 170! a double is Infinity anyway
  long n = (long)x;
  double r = 1;
  for (long i = 2; i <= n; i++) r *= (double)i;
  return ax_num(r);
}
NATIVE(n_comb) {
  double nx = floor(NUM(0)), kx = floor(NUM(1));
  if (isnan(nx) || isnan(kx)) return ax_num(NAN);
  if (kx < 0 || kx > nx) return ax_num(0);
  long n = (long)fmin(nx, 1e15), k = (long)kx;
  if (k > n - k) k = n - k;
  double r = 1;
  for (long i = 0; i < k; i++) r = r * (double)(n - i) / (double)(i + 1);
  return ax_num(floor(r + 0.5));
}
NATIVE(n_perm) {
  double nx = floor(NUM(0)), kx = floor(NUM(1));
  if (isnan(nx) || isnan(kx)) return ax_num(NAN);
  if (kx < 0 || kx > nx) return ax_num(0);
  long n = (long)fmin(nx, 1e15), k = (long)kx;
  double r = 1;
  for (long i = 0; i < k; i++) r *= (double)(n - i);
  return ax_num(r);
}
NATIVE(n_is_prime) {
  double x = floor(NUM(0));
  if (!(x >= 2) || !isfinite(x)) return ax_bool(false);
  long n = (long)x;
  if (n % 2 == 0) return ax_bool(n == 2);
  for (long i = 3; i * i <= n; i += 2) if (n % i == 0) return ax_bool(false);
  return ax_bool(true);
}
NATIVE(n_primes) {
  double lx = floor(NUM(0));
  AxArr *out = ax_arr_new(16);
  if (!(lx >= 2)) return ax_arrv(out);
  if (lx > 1e8) ax_throw(vm, "AX-RUNTIME-000", "primes() limit %g is too large", lx);
  long limit = (long)lx;
  char *sieve = calloc(limit + 1, 1);
  for (long i = 2; i <= limit; i++) {
    if (sieve[i]) continue;
    ax_arr_push(out, ax_num((double)i));
    for (long j = i * i; j <= limit; j += i) sieve[j] = 1;
  }
  free(sieve);
  return ax_arrv(out);
}
NATIVE(n_round_to) {
  no_big(vm, args, argc, 2);
  double x = NUM(0), step = NUM(1);
  if (step == 0) return ax_num(x);
  return ax_num(floor(x / step + 0.5) * step);
}
// Number(x).toFixed(d): ties round up in magnitude, on the exact value, and 1e21 or more is
// written as String(x) — the f-string `{x:.Nf}` code does exactly this.
NATIVE(n_to_fixed) {
  double x = ax_to_num(ARG(0));
  double d = argc > 1 ? NUM(1) : 2;   // digits === undefined ? 2 : digits (null is 0)
  d = isnan(d) ? 0 : trunc(d);
  if (d < 0 || d > 100) ax_throw(vm, "AX-RUNTIME-000", "toFixed() digits argument must be between 0 and 100");
  char spec[16];
  snprintf(spec, sizeof spec, ".%df", (int)d);
  return ax_strv(ax_format_spec(ax_num(x), spec));
}
NATIVE(n_to_hex) { no_big(vm, args, argc, 1); char b[32]; snprintf(b, sizeof b, "0x%x", (unsigned)(uint32_t)INT32(0)); return ax_str_from(b); }
NATIVE(n_to_bin) {
  no_big(vm, args, argc, 1);
  unsigned v = (unsigned)(uint32_t)INT32(0);   // n >>> 0
  char b[40];
  int j = 0;
  b[j++] = '0'; b[j++] = 'b';
  if (!v) b[j++] = '0';
  else { int started = 0; for (int i = 31; i >= 0; i--) { int bit = (v >> i) & 1; if (bit) started = 1; if (started) b[j++] = (char)('0' + bit); } }
  b[j] = '\0';
  return ax_str_from(b);
}
// Math.trunc(n).toString(radix): exact digits for any double, "NaN"/"Infinity" as JavaScript
// writes them, and radix 10 in Number's own notation (1e21 is "1e+21").
NATIVE(n_to_base) {
  if (argc > 1 && args[1].t == AX_BIG) ax_throw(vm, "AX-RUNTIME-000", "%s", BIG_MIX);   // base | 0
  double x = trunc(NUM(0));
  int base = INT32(1);
  if (base < 2) base = 2;
  if (base > 36) base = 36;
  if (!isfinite(x) || base == 10) return ax_strv(ax_to_str(ax_num(x)));
  AxBig *b = ax_big_from_double(x);
  bool neg = b->neg;
  size_t cap = (size_t)b->n * 32 + 3, j = 0;
  char *buf = malloc(cap);
  AxBig *radix = ax_big_from_int(base);
  b->neg = false;
  while (!ax_big_is_zero(b)) {
    AxBig *q, *r;
    ax_big_divmod(b, radix, &q, &r);
    int d = r->n ? (int)r->d[0] : 0;
    buf[j++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
    free(r);
    free(b);
    b = q;
  }
  free(b);
  free(radix);
  if (!j) buf[j++] = '0';
  if (neg) buf[j++] = '-';
  for (size_t a = 0, z = j - 1; a < z; a++, z--) { char t = buf[a]; buf[a] = buf[z]; buf[z] = t; }
  AxValue out = ax_strv(ax_str_new(buf, j));
  free(buf);
  return out;
}
// JavaScript's 32-bit operators: operands through ToInt32, shift counts taken mod 32.
NATIVE(n_band) { no_big(vm, args, argc, 2); return ax_num((double)(INT32(0) & INT32(1))); }
NATIVE(n_bor) { no_big(vm, args, argc, 2); return ax_num((double)(INT32(0) | INT32(1))); }
NATIVE(n_bxor) { no_big(vm, args, argc, 2); return ax_num((double)(INT32(0) ^ INT32(1))); }
NATIVE(n_bnot) { no_big(vm, args, argc, 1); return ax_num((double)(~INT32(0))); }
NATIVE(n_shl) { no_big(vm, args, argc, 2); return ax_num((double)(int32_t)((uint32_t)INT32(0) << ((uint32_t)INT32(1) & 31))); }
NATIVE(n_shr) { no_big(vm, args, argc, 2); return ax_num((double)(INT32(0) >> ((uint32_t)INT32(1) & 31))); }

// --- statistics -------------------------------------------------------------------------------

NATIVE(n_sum) {
  AxArr *a = ax_to_seq(vm, ARG(0));
  AxValue sel = ARG(1);
  double t = 0;
  for (uint32_t i = 0; i < a->len; i++) {
    AxValue k = ax_key_apply(vm, sel, a->items[i], i);
    double d = ax_to_num(k);
    ax_release(k);
    if (!isnan(d)) t += d;
  }
  ax_release(ax_arrv(a));
  return ax_num(t);
}
NATIVE(n_prod) {
  AxArr *a = ax_to_seq(vm, ARG(0));
  AxValue sel = ARG(1);
  double t = 1;
  for (uint32_t i = 0; i < a->len; i++) {
    AxValue k = ax_key_apply(vm, sel, a->items[i], i);
    double d = ax_to_num(k);
    t *= isnan(d) ? 0 : d;   // Number(v) || 0
    ax_release(k);
  }
  ax_release(ax_arrv(a));
  return ax_num(t);
}
NATIVE(n_mean) {
  AxArr *a = ax_to_seq(vm, ARG(0));
  AxValue sel = ARG(1);
  if (!a->len) { ax_release(ax_arrv(a)); return ax_num(0); }
  double t = 0;
  for (uint32_t i = 0; i < a->len; i++) {
    AxValue k = ax_key_apply(vm, sel, a->items[i], i);
    double d = ax_to_num(k);
    t += isnan(d) ? 0 : d;   // Number(v) || 0
    ax_release(k);
  }
  double r = t / a->len;
  ax_release(ax_arrv(a));
  return ax_num(r);
}
NATIVE(n_median) {
  AxArr *a = ax_to_seq(vm, ARG(0));
  uint32_t n = a->len;
  if (!n) { ax_release(ax_arrv(a)); return ax_num(0); }
  double *v = malloc(sizeof(double) * n);
  for (uint32_t i = 0; i < n; i++) v[i] = ax_to_num(a->items[i]);
  ax_release(ax_arrv(a));
  for (uint32_t i = 1; i < n; i++) { double x = v[i]; uint32_t j = i; while (j && v[j - 1] > x) { v[j] = v[j - 1]; j--; } v[j] = x; }
  double r = (n % 2) ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
  free(v);
  return ax_num(r);
}
NATIVE(n_variance_impl) {
  AxArr *a = ax_to_seq(vm, ARG(0));
  uint32_t n = a->len;
  if (n < 2) { ax_release(ax_arrv(a)); return ax_num(0); }
  double m = 0;
  for (uint32_t i = 0; i < n; i++) m += ax_to_num(a->items[i]);
  m /= n;
  double s = 0;
  for (uint32_t i = 0; i < n; i++) { double d = ax_to_num(a->items[i]) - m; s += d * d; }
  ax_release(ax_arrv(a));
  return ax_num(s / n);
}
NATIVE(n_stdev) {
  AxValue v = n_variance_impl(vm, self, args, argc);
  double d = sqrt(ax_to_num(v));
  ax_release(v);
  return ax_num(d);
}
NATIVE(n_mode) {
  AxArr *a = ax_to_seq(vm, ARG(0));
  if (!a->len) { ax_release(ax_arrv(a)); return ax_null(); }
  AxValue best = ax_copy(a->items[0]);
  int bestc = 0;
  for (uint32_t i = 0; i < a->len; i++) {
    int c = 0;
    for (uint32_t j = 0; j < a->len; j++) if (ax_equals(a->items[i], a->items[j])) c++;
    if (c > bestc) { bestc = c; ax_release(best); best = ax_copy(a->items[i]); }
  }
  ax_release(ax_arrv(a));
  return best;
}

// --- collections ------------------------------------------------------------------------------

NATIVE(n_sorted) {
  AxArr *out = sorted_copy(vm, ARG(0), ARG(1));
  return ax_arrv(out);
}
NATIVE(n_sort_by) { return ax_arrv(sorted_copy(vm, ARG(0), ARG(1))); }
NATIVE(n_reversed) {
  AxArr *a = ax_to_seq(vm, ARG(0));
  AxArr *out = ax_arr_new(a->len);
  for (int64_t i = (int64_t)a->len - 1; i >= 0; i--) ax_arr_push(out, ax_copy(a->items[i]));
  ax_release(ax_arrv(a));
  return ax_arrv(out);
}
NATIVE(n_take) {
  AxArr *a = ax_to_seq(vm, ARG(0));
  int64_t n = ax_js_int(NUM(1));
  AxArr *out = ax_arr_new(n > 0 ? (uint32_t)n : 0);
  for (int64_t i = 0; i < n && i < (int64_t)a->len; i++) ax_arr_push(out, ax_copy(a->items[i]));
  ax_release(ax_arrv(a));
  return ax_arrv(out);
}
NATIVE(n_drop) {
  AxArr *a = ax_to_seq(vm, ARG(0));
  int64_t n = ax_js_int(NUM(1));
  AxArr *out = ax_arr_new(4);
  for (int64_t i = n < 0 ? 0 : n; i < (int64_t)a->len; i++) ax_arr_push(out, ax_copy(a->items[i]));
  ax_release(ax_arrv(a));
  return ax_arrv(out);
}
NATIVE(n_first) {
  AxArr *a = ax_to_seq(vm, ARG(0));
  AxValue r = a->len ? ax_copy(a->items[0]) : (argc > 1 ? ax_copy(args[1]) : ax_null());
  ax_release(ax_arrv(a));
  return r;
}
NATIVE(n_last) {
  AxArr *a = ax_to_seq(vm, ARG(0));
  AxValue r = a->len ? ax_copy(a->items[a->len - 1]) : (argc > 1 ? ax_copy(args[1]) : ax_null());
  ax_release(ax_arrv(a));
  return r;
}
NATIVE(n_map) {
  AxArr *a = ax_to_seq(vm, ARG(0));
  AxArr *out = ax_arr_new(a->len);
  for (uint32_t i = 0; i < a->len; i++) ax_arr_push(out, ax_key_apply(vm, ARG(1), a->items[i], i));
  ax_release(ax_arrv(a));
  return ax_arrv(out);
}
NATIVE(n_filter) {
  AxArr *a = ax_to_seq(vm, ARG(0));
  AxArr *out = ax_arr_new(4);
  for (uint32_t i = 0; i < a->len; i++) {
    AxValue k = ax_key_apply(vm, ARG(1), a->items[i], i);
    bool keep = ax_truthy(k);
    ax_release(k);
    if (keep) ax_arr_push(out, ax_copy(a->items[i]));
  }
  ax_release(ax_arrv(a));
  return ax_arrv(out);
}
NATIVE(n_reduce) {
  AxArr *a = ax_to_seq(vm, ARG(0));
  AxValue acc;
  uint32_t start = 0;
  if (argc > 2) acc = ax_copy(args[2]);
  else { acc = a->len ? ax_copy(a->items[0]) : ax_null(); start = 1; }
  for (uint32_t i = start; i < a->len; i++) {
    AxValue call_args[3] = { acc, a->items[i], ax_num(i) };
    AxValue next = ax_call(vm, ARG(1), call_args, 3);
    ax_release(acc);
    acc = next;
  }
  ax_release(ax_arrv(a));
  return acc;
}
NATIVE(n_each) {
  // A callback, called as any function value is (a string names a function).
  AxArr *a = ax_to_seq(vm, ARG(0));
  for (uint32_t i = 0; i < a->len; i++) {
    AxValue cargs[2] = { a->items[i], ax_num(i) };
    AxValue r = ax_call(vm, ARG(1), cargs, 2);
    ax_release(r);
  }
  ax_release(ax_arrv(a));
  return ax_null();
}
NATIVE(n_find) {
  AxArr *a = ax_to_seq(vm, ARG(0));
  AxValue found = ax_null();
  for (uint32_t i = 0; i < a->len; i++) {
    AxValue k = ax_key_apply(vm, ARG(1), a->items[i], i);
    bool hit = ax_truthy(k);
    ax_release(k);
    if (hit) { found = ax_copy(a->items[i]); break; }
  }
  ax_release(ax_arrv(a));
  return found;
}
NATIVE(n_find_index) {
  AxArr *a = ax_to_seq(vm, ARG(0));
  double idx = -1;
  for (uint32_t i = 0; i < a->len; i++) {
    AxValue k = ax_key_apply(vm, ARG(1), a->items[i], i);
    bool hit = ax_truthy(k);
    ax_release(k);
    if (hit) { idx = i; break; }
  }
  ax_release(ax_arrv(a));
  return ax_num(idx);
}
NATIVE(n_any) {
  AxArr *a = ax_to_seq(vm, ARG(0));
  bool found = false;
  for (uint32_t i = 0; i < a->len && !found; i++) {
    AxValue k = ax_key_apply(vm, ARG(1), a->items[i], i);
    found = ax_truthy(k);
    ax_release(k);
  }
  ax_release(ax_arrv(a));
  return ax_bool(found);
}
NATIVE(n_all) {
  AxArr *a = ax_to_seq(vm, ARG(0));
  bool all = true;
  for (uint32_t i = 0; i < a->len && all; i++) {
    AxValue k = ax_key_apply(vm, ARG(1), a->items[i], i);
    all = ax_truthy(k);
    ax_release(k);
  }
  ax_release(ax_arrv(a));
  return ax_bool(all);
}
NATIVE(n_count) {
  AxArr *a = ax_to_seq(vm, ARG(0));
  double n = 0;
  if (argc < 2 || args[1].t == AX_NULL) n = a->len;
  else if (args[1].t == AX_FN) {
    for (uint32_t i = 0; i < a->len; i++) {
      AxValue k = ax_key_apply(vm, args[1], a->items[i], i);
      if (ax_truthy(k)) n++;
      ax_release(k);
    }
  } else {
    for (uint32_t i = 0; i < a->len; i++) if (ax_equals(a->items[i], args[1])) n++;
  }
  ax_release(ax_arrv(a));
  return ax_num(n);
}
// uniq, union, intersect and difference tell values apart by how they display, as the
// reference does (its sets are keyed by that text): 1 and "1" are the same element.
static bool text_set_add(AxDict *set, AxValue v) {   // false when already present
  AxStr *k = ax_to_str(v);
  bool fresh = !ax_dict_has(set, k);
  if (fresh) ax_dict_set(set, k, ax_null());
  ax_release(ax_strv(k));
  return fresh;
}
static bool text_set_has(AxDict *set, AxValue v) {
  AxStr *k = ax_to_str(v);
  bool has = ax_dict_has(set, k);
  ax_release(ax_strv(k));
  return has;
}

NATIVE(n_uniq) {
  AxArr *a = ax_to_seq(vm, ARG(0));
  AxArr *out = ax_arr_new(a->len);
  AxDict *seen = ax_dict_new();
  for (uint32_t i = 0; i < a->len; i++) {
    AxValue k = ax_key_apply(vm, ARG(1), a->items[i], i);
    if (text_set_add(seen, k)) ax_arr_push(out, ax_copy(a->items[i]));
    ax_release(k);
  }
  ax_release(ax_dictv(seen));
  ax_release(ax_arrv(a));
  return ax_arrv(out);
}
NATIVE(n_zip) {
  uint32_t minlen = 0;
  AxArr **seqs = calloc(argc, sizeof(AxArr *));
  for (int i = 0; i < argc; i++) {
    seqs[i] = ax_to_seq(vm, args[i]);
    if (i == 0 || seqs[i]->len < minlen) minlen = seqs[i]->len;
  }
  AxArr *out = ax_arr_new(minlen);
  for (uint32_t i = 0; i < minlen; i++) {
    AxArr *row = ax_arr_new(argc);
    for (int s = 0; s < argc; s++) ax_arr_push(row, ax_copy(seqs[s]->items[i]));
    ax_arr_push(out, ax_arrv(row));
  }
  for (int i = 0; i < argc; i++) ax_release(ax_arrv(seqs[i]));
  free(seqs);
  return ax_arrv(out);
}
// unzip([[a, 1], [b, 2]]) → [[a, b], [1, 2]]: as many rows as the longest pair, null where a
// pair is short.
NATIVE(n_unzip) {
  AxArr *a = ax_to_seq(vm, ARG(0));
  AxArr **rows = calloc(a->len ? a->len : 1, sizeof(AxArr *));
  uint32_t n = 0;
  for (uint32_t i = 0; i < a->len; i++) { rows[i] = ax_to_seq(vm, a->items[i]); if (rows[i]->len > n) n = rows[i]->len; }
  AxArr *out = ax_arr_new(n);
  for (uint32_t k = 0; k < n; k++) {
    AxArr *col = ax_arr_new(a->len);
    for (uint32_t i = 0; i < a->len; i++) ax_arr_push(col, k < rows[i]->len ? ax_copy(rows[i]->items[k]) : ax_null());
    ax_arr_push(out, ax_arrv(col));
  }
  for (uint32_t i = 0; i < a->len; i++) ax_release(ax_arrv(rows[i]));
  free(rows);
  ax_release(ax_arrv(a));
  return ax_arrv(out);
}

NATIVE(n_enumerate) {
  AxArr *a = ax_to_seq(vm, ARG(0));
  AxArr *out = ax_arr_new(a->len);
  for (uint32_t i = 0; i < a->len; i++) {
    AxArr *pair = ax_arr_new(2);
    ax_arr_push(pair, ax_num(i));
    ax_arr_push(pair, ax_copy(a->items[i]));
    ax_arr_push(out, ax_arrv(pair));
  }
  ax_release(ax_arrv(a));
  return ax_arrv(out);
}
NATIVE(n_chunk) {
  AxArr *a = ax_to_seq(vm, ARG(0));
  int64_t size = ax_js_int(NUM(1));
  if (size < 1) size = 1;
  AxArr *out = ax_arr_new(4);
  for (uint32_t i = 0; i < a->len; i += size) {
    AxArr *part = ax_arr_new(size);
    for (int64_t j = 0; j < size && i + j < a->len; j++) ax_arr_push(part, ax_copy(a->items[i + j]));
    ax_arr_push(out, ax_arrv(part));
  }
  ax_release(ax_arrv(a));
  return ax_arrv(out);
}
NATIVE(n_windows) {
  AxArr *a = ax_to_seq(vm, ARG(0));
  int64_t size = ax_js_int(NUM(1));
  if (size < 1) size = 1;
  AxArr *out = ax_arr_new(4);
  for (int64_t i = 0; i + size <= (int64_t)a->len; i++) {
    AxArr *part = ax_arr_new(size);
    for (int64_t j = 0; j < size; j++) ax_arr_push(part, ax_copy(a->items[i + j]));
    ax_arr_push(out, ax_arrv(part));
  }
  ax_release(ax_arrv(a));
  return ax_arrv(out);
}
static void flatten_into(AxVM *vm, AxArr *out, AxValue v, int depth) {
  if (v.t == AX_ARR && depth != 0) {
    AxArr *a = (AxArr *)v.o;
    for (uint32_t i = 0; i < a->len; i++) flatten_into(vm, out, a->items[i], depth - 1);
    return;
  }
  ax_arr_push(out, ax_copy(v));
}
NATIVE(n_flatten) {
  AxArr *out = ax_arr_new(8);
  int depth = argc > 1 ? (int)fmin(ax_js_int(NUM(1)), 1 << 20) : -1;
  AxArr *a = ax_to_seq(vm, ARG(0));
  for (uint32_t i = 0; i < a->len; i++) flatten_into(vm, out, a->items[i], depth);
  ax_release(ax_arrv(a));
  return ax_arrv(out);
}
NATIVE(n_partition) {
  AxArr *a = ax_to_seq(vm, ARG(0));
  AxArr *yes = ax_arr_new(4), *no = ax_arr_new(4);
  for (uint32_t i = 0; i < a->len; i++) {
    AxValue k = ax_key_apply(vm, ARG(1), a->items[i], i);
    bool hit = ax_truthy(k);
    ax_release(k);
    ax_arr_push(hit ? yes : no, ax_copy(a->items[i]));
  }
  ax_release(ax_arrv(a));
  AxArr *out = ax_arr_new(2);
  ax_arr_push(out, ax_arrv(yes));
  ax_arr_push(out, ax_arrv(no));
  return ax_arrv(out);
}
NATIVE(n_group_by) {
  AxArr *a = ax_to_seq(vm, ARG(0));
  AxDict *d = ax_dict_new();
  for (uint32_t i = 0; i < a->len; i++) {
    AxValue k = ax_key_apply(vm, ARG(1), a->items[i], i);
    AxStr *ks = ax_to_str(k);
    AxStr *key = ax_intern(ks->data, ks->len);
    AxValue bucket;
    if (!ax_dict_get(d, key, &bucket)) { bucket = ax_arrv(ax_arr_new(4)); ax_dict_set(d, key, ax_copy(bucket)); }
    ax_arr_push((AxArr *)bucket.o, ax_copy(a->items[i]));
    ax_release(bucket);
    ax_release(ax_strv(key));
    ax_release(ax_strv(ks));
    ax_release(k);
  }
  ax_release(ax_arrv(a));
  return ax_dictv(d);
}
NATIVE(n_count_by) {
  AxArr *a = ax_to_seq(vm, ARG(0));
  AxDict *d = ax_dict_new();
  for (uint32_t i = 0; i < a->len; i++) {
    AxValue k = ax_key_apply(vm, ARG(1), a->items[i], i);
    AxStr *ks = ax_to_str(k);
    AxStr *key = ax_intern(ks->data, ks->len);
    AxValue cur;
    double c = ax_dict_get(d, key, &cur) ? ax_to_num(cur) : 0;
    if (c) ax_release(cur);
    ax_dict_set(d, key, ax_num(c + 1));
    ax_release(ax_strv(key));
    ax_release(ax_strv(ks));
    ax_release(k);
  }
  ax_release(ax_arrv(a));
  return ax_dictv(d);
}
NATIVE(n_min_by) {
  AxArr *a = ax_to_seq(vm, ARG(0));
  AxValue best = ax_null(), bestk = ax_null();
  for (uint32_t i = 0; i < a->len; i++) {
    AxValue k = ax_key_apply(vm, ARG(1), a->items[i], i);
    if (i == 0 || ax_compare(k, bestk) < 0) { ax_release(best); ax_release(bestk); best = ax_copy(a->items[i]); bestk = ax_copy(k); }
    ax_release(k);
  }
  ax_release(bestk);
  ax_release(ax_arrv(a));
  return best;
}
NATIVE(n_max_by) {
  AxArr *a = ax_to_seq(vm, ARG(0));
  AxValue best = ax_null(), bestk = ax_null();
  for (uint32_t i = 0; i < a->len; i++) {
    AxValue k = ax_key_apply(vm, ARG(1), a->items[i], i);
    if (i == 0 || ax_compare(k, bestk) > 0) { ax_release(best); ax_release(bestk); best = ax_copy(a->items[i]); bestk = ax_copy(k); }
    ax_release(k);
  }
  ax_release(bestk);
  ax_release(ax_arrv(a));
  return best;
}
NATIVE(n_union) {
  // Every element of a (duplicates kept, as the reference copies it), then what b adds.
  AxArr *a = ax_to_seq(vm, ARG(0)), *b = ax_to_seq(vm, ARG(1));
  AxArr *out = ax_arr_new(a->len + b->len);
  AxDict *seen = ax_dict_new();
  for (uint32_t i = 0; i < a->len; i++) { text_set_add(seen, a->items[i]); ax_arr_push(out, ax_copy(a->items[i])); }
  for (uint32_t i = 0; i < b->len; i++) if (text_set_add(seen, b->items[i])) ax_arr_push(out, ax_copy(b->items[i]));
  ax_release(ax_dictv(seen));
  ax_release(ax_arrv(a));
  ax_release(ax_arrv(b));
  return ax_arrv(out);
}
NATIVE(n_intersect) {
  AxArr *a = ax_to_seq(vm, ARG(0)), *b = ax_to_seq(vm, ARG(1));
  AxArr *out = ax_arr_new(4);
  AxDict *inb = ax_dict_new(), *added = ax_dict_new();
  for (uint32_t j = 0; j < b->len; j++) text_set_add(inb, b->items[j]);
  for (uint32_t i = 0; i < a->len; i++)
    if (text_set_has(inb, a->items[i]) && text_set_add(added, a->items[i])) ax_arr_push(out, ax_copy(a->items[i]));
  ax_release(ax_dictv(inb));
  ax_release(ax_dictv(added));
  ax_release(ax_arrv(a));
  ax_release(ax_arrv(b));
  return ax_arrv(out);
}
NATIVE(n_difference) {
  AxArr *a = ax_to_seq(vm, ARG(0)), *b = ax_to_seq(vm, ARG(1));
  AxArr *out = ax_arr_new(4);
  AxDict *inb = ax_dict_new();
  for (uint32_t j = 0; j < b->len; j++) text_set_add(inb, b->items[j]);
  for (uint32_t i = 0; i < a->len; i++) if (!text_set_has(inb, a->items[i])) ax_arr_push(out, ax_copy(a->items[i]));
  ax_release(ax_dictv(inb));
  ax_release(ax_arrv(a));
  ax_release(ax_arrv(b));
  return ax_arrv(out);
}
NATIVE(n_grid) {
  int64_t rows = ax_js_int(ax_to_num(ARG(0))), cols = ax_js_int(ax_to_num(ARG(1)));   // Array.from({length})
  AxValue fill = ARG(2);
  AxArr *out = ax_arr_new(rows > 0 ? rows : 0);
  for (int64_t r = 0; r < rows; r++) {
    AxArr *row = ax_arr_new(cols > 0 ? cols : 0);
    for (int64_t c = 0; c < cols; c++) {
      if (fill.t == AX_FN) {
        AxValue cargs[2] = { ax_num((double)r), ax_num((double)c) };
        ax_arr_push(row, ax_call(vm, fill, cargs, 2));
      } else {
        ax_arr_push(row, fill.t == AX_NULL ? ax_num(0) : ax_copy(fill));
      }
    }
    ax_arr_push(out, ax_arrv(row));
  }
  return ax_arrv(out);
}
NATIVE(n_transpose) {
  // Rows are whatever the library iterates (a number is a row of one); short rows pad with null.
  AxArr *m = ax_to_seq(vm, ARG(0));
  AxArr **rows = malloc(sizeof(AxArr *) * (m->len ? m->len : 1));
  uint32_t cols = 0;
  for (uint32_t i = 0; i < m->len; i++) { rows[i] = ax_to_seq(vm, m->items[i]); if (rows[i]->len > cols) cols = rows[i]->len; }
  AxArr *out = ax_arr_new(cols);
  for (uint32_t c = 0; c < cols; c++) {
    AxArr *row = ax_arr_new(m->len);
    for (uint32_t r = 0; r < m->len; r++) ax_arr_push(row, ax_arr_get(rows[r], c));
    ax_arr_push(out, ax_arrv(row));
  }
  for (uint32_t i = 0; i < m->len; i++) ax_release(ax_arrv(rows[i]));
  free(rows);
  ax_release(ax_arrv(m));
  return ax_arrv(out);
}

// --- dicts ------------------------------------------------------------------------------------

// The key/value view of a value, for the dict functions: a dict or record as it is, an array by
// index ("0", "1", …), a vector by component; anything else is empty. Returns +1.
static AxDict *dict_view(AxValue v) {
  if (v.t == AX_DICT) { ax_retain(v); return (AxDict *)v.o; }
  AxDict *d = ax_dict_new();
  if (v.t == AX_ARR) {
    AxArr *a = (AxArr *)v.o;
    for (uint32_t i = 0; i < a->len; i++) {
      char k[16];
      snprintf(k, sizeof k, "%u", i);
      AxStr *key = ax_internz(k);
      ax_dict_set(d, key, ax_copy(a->items[i]));
      ax_release(ax_strv(key));
    }
  } else if (v.t == AX_VEC2 || v.t == AX_VEC3 || v.t == AX_QUAT) {
    AxVec *p = ax_vecp(v);
    const char *names[4] = { "x", "y", "z", "w" };
    double parts[4] = { p->x, p->y, p->z, p->w };
    int n = v.t == AX_VEC2 ? 2 : v.t == AX_VEC3 ? 3 : 4;
    for (int i = 0; i < n; i++) {
      AxStr *key = ax_internz(names[i]);
      ax_dict_set(d, key, ax_num(parts[i]));
      ax_release(ax_strv(key));
    }
  }
  return d;
}
#define DICT_EACH(d, i) for (uint32_t i = 0; i < (d)->len; i++) if (!(d)->entries[i].dead)

NATIVE(n_keys) {
  AxDict *d = dict_view(ARG(0));
  AxArr *out = ax_arr_new(4);
  DICT_EACH(d, i) { ax_retain(ax_strv(d->entries[i].key)); ax_arr_push(out, ax_strv(d->entries[i].key)); }
  ax_release(ax_dictv(d));
  return ax_arrv(out);
}
NATIVE(n_values) {
  AxDict *d = dict_view(ARG(0));
  AxArr *out = ax_arr_new(4);
  DICT_EACH(d, i) ax_arr_push(out, ax_copy(d->entries[i].val));
  ax_release(ax_dictv(d));
  return ax_arrv(out);
}
NATIVE(n_items) {
  AxDict *d = dict_view(ARG(0));
  AxArr *out = ax_arr_new(4);
  DICT_EACH(d, i) {
    AxArr *pair = ax_arr_new(2);
    ax_retain(ax_strv(d->entries[i].key));
    ax_arr_push(pair, ax_strv(d->entries[i].key));
    ax_arr_push(pair, ax_copy(d->entries[i].val));
    ax_arr_push(out, ax_arrv(pair));
  }
  ax_release(ax_dictv(d));
  return ax_arrv(out);
}
NATIVE(n_dict) {
  AxArr *pairs = ax_to_seq(vm, ARG(0));
  AxDict *d = ax_dict_new();
  for (uint32_t i = 0; i < pairs->len; i++) {
    AxArr *p = ax_to_seq(vm, pairs->items[i]);
    AxStr *ks = ax_to_str(p->len ? p->items[0] : ax_null());
    AxStr *key = ax_intern(ks->data, ks->len);
    ax_dict_set(d, key, p->len > 1 ? ax_copy(p->items[1]) : ax_null());
    ax_release(ax_strv(key));
    ax_release(ax_strv(ks));
    ax_release(ax_arrv(p));
  }
  ax_release(ax_arrv(pairs));
  return ax_dictv(d);
}
NATIVE(n_merge) {
  // A plain dict: records merge their fields, not their type.
  AxDict *out = ax_dict_new();
  for (int a = 0; a < argc; a++) {
    AxDict *d = dict_view(args[a]);
    DICT_EACH(d, e) ax_dict_set(out, d->entries[e].key, ax_copy(d->entries[e].val));
    ax_release(ax_dictv(d));
  }
  return ax_dictv(out);
}
NATIVE(n_has_key) {
  AxDict *d = dict_view(ARG(0));
  AxStr *k = ax_to_str(ARG(1));
  bool has = ax_dict_has(d, k);
  ax_release(ax_strv(k));
  ax_release(ax_dictv(d));
  return ax_bool(has);
}
NATIVE(n_pick_keys) {
  AxDict *src = dict_view(ARG(0));
  AxDict *out = ax_dict_new();
  AxArr *ks = ax_to_seq(vm, ARG(1));
  for (uint32_t i = 0; i < ks->len; i++) {
    AxStr *k = ax_to_str(ks->items[i]);
    AxStr *key = ax_intern(k->data, k->len);
    AxValue v;
    if (ax_dict_get(src, key, &v)) ax_dict_set(out, key, v);
    ax_release(ax_strv(key));
    ax_release(ax_strv(k));
  }
  ax_release(ax_arrv(ks));
  ax_release(ax_dictv(src));
  return ax_dictv(out);
}
NATIVE(n_omit_keys) {
  AxDict *d = dict_view(ARG(0));
  AxDict *out = ax_dict_new();
  AxDict *drop = ax_dict_new();
  AxArr *ks = ax_to_seq(vm, ARG(1));
  for (uint32_t i = 0; i < ks->len; i++) {
    AxStr *k = ax_to_str(ks->items[i]);
    ax_dict_set(drop, k, ax_null());
    ax_release(ax_strv(k));
  }
  DICT_EACH(d, e) if (!ax_dict_has(drop, d->entries[e].key)) ax_dict_set(out, d->entries[e].key, ax_copy(d->entries[e].val));
  ax_release(ax_arrv(ks));
  ax_release(ax_dictv(drop));
  ax_release(ax_dictv(d));
  return ax_dictv(out);
}
NATIVE(n_invert) {
  AxDict *d = dict_view(ARG(0));
  AxDict *out = ax_dict_new();
  DICT_EACH(d, e) {
    AxStr *vs = ax_to_str(d->entries[e].val);
    AxStr *key = ax_intern(vs->data, vs->len);
    ax_retain(ax_strv(d->entries[e].key));
    ax_dict_set(out, key, ax_strv(d->entries[e].key));
    ax_release(ax_strv(key));
    ax_release(ax_strv(vs));
  }
  ax_release(ax_dictv(d));
  return ax_dictv(out);
}
static AxValue deep_clone(AxValue v) {
  switch (v.t) {
    case AX_ARR: {
      AxArr *a = (AxArr *)v.o;
      AxArr *out = ax_arr_new(a->len);
      for (uint32_t i = 0; i < a->len; i++) ax_arr_push(out, deep_clone(a->items[i]));
      return ax_arrv(out);
    }
    case AX_DICT: {
      AxDict *d = (AxDict *)v.o;
      AxDict *out = ax_dict_new();
      if (d->type_tag) { out->type_tag = d->type_tag; ax_retain(ax_strv(d->type_tag)); }
      for (uint32_t i = 0; i < d->len; i++) {
        if (d->entries[i].dead) continue;
        ax_dict_set(out, d->entries[i].key, deep_clone(d->entries[i].val));
      }
      return ax_dictv(out);
    }
    default: return ax_copy(v);
  }
}
NATIVE(n_clone) { return deep_clone(ARG(0)); }
// JSON.stringify(a) === JSON.stringify(b); where that throws (a big inside), a === b.
NATIVE(n_deep_eq) {
  AxValue a = ARG(0), b = ARG(1);
  // A function has no JSON at all (undefined), so two functions compare equal and a function
  // equals nothing else.
  if (a.t == AX_FN || b.t == AX_FN) return ax_bool(a.t == b.t);
  char *ja = NULL, *jb = NULL;
  bool oka = ax_json_write_checked(a, 0, &ja), okb = ax_json_write_checked(b, 0, &jb);
  bool eq;
  if (oka && okb) eq = strcmp(ja, jb) == 0;
  else if (a.t != b.t) eq = false;
  else if (a.t == AX_BIG) eq = ax_big_cmp((AxBig *)a.o, (AxBig *)b.o) == 0;
  else if (a.t == AX_NUM) eq = a.num == b.num;
  else if (a.t == AX_BOOL) eq = a.b == b.b;
  else if (a.t == AX_STR) eq = ax_str_eq((AxStr *)a.o, (AxStr *)b.o);
  else eq = a.o == b.o;
  free(ja);
  free(jb);
  return ax_bool(eq);
}

// --- strings ----------------------------------------------------------------------------------

// String(c).charCodeAt(0): the first UTF-16 code unit (NaN for ""), from UTF-8 text.
NATIVE(n_ord) {
  AxStr *s = arg_str(ARG(0));
  const unsigned char *p = (const unsigned char *)s->data;
  double v = NAN;
  if (s->len) {
    uint32_t cp = p[0];
    int extra = cp >= 0xF0 ? 3 : cp >= 0xE0 ? 2 : cp >= 0xC0 ? 1 : 0;
    if (extra && (uint32_t)extra < s->len) {
      cp &= 0x3F >> extra;
      for (int i = 1; i <= extra; i++) cp = (cp << 6) | (p[i] & 0x3F);
    }
    v = cp > 0xFFFF ? 0xD800 + ((cp - 0x10000) >> 10) : cp;
  }
  ax_release(ax_strv(s));
  return ax_num(v);
}
// String.fromCharCode(n): one UTF-16 code unit (ToUint16), written as UTF-8; a lone surrogate
// comes out as U+FFFD, which is what printing it shows.
NATIVE(n_chr) {
  uint32_t u = (uint32_t)ax_to_int32(NUM(0)) & 0xFFFF;
  if (u >= 0xD800 && u <= 0xDFFF) u = 0xFFFD;
  char b[4];
  size_t n;
  if (u < 0x80) { b[0] = (char)u; n = 1; }
  else if (u < 0x800) { b[0] = (char)(0xC0 | (u >> 6)); b[1] = (char)(0x80 | (u & 0x3F)); n = 2; }
  else { b[0] = (char)(0xE0 | (u >> 12)); b[1] = (char)(0x80 | ((u >> 6) & 0x3F)); b[2] = (char)(0x80 | (u & 0x3F)); n = 3; }
  return ax_strv(ax_str_new(b, n));
}
NATIVE(n_lines) {
  AxStr *s = ARG(0).t == AX_NULL ? ax_str_newz("") : arg_str(ARG(0));
  AxArr *out = ax_arr_new(4);
  uint32_t start = 0;
  for (uint32_t i = 0; i <= s->len; i++) {
    if (i == s->len || s->data[i] == '\n') {
      uint32_t end = i;
      if (end > start && s->data[end - 1] == '\r') end--;
      ax_arr_push(out, ax_strv(ax_str_new(s->data + start, end - start)));
      start = i + 1;
    }
  }
  ax_release(ax_strv(s));
  return ax_arrv(out);
}
NATIVE(n_words) {
  AxStr *s = ARG(0).t == AX_NULL ? ax_str_newz("") : arg_str(ARG(0));
  AxArr *out = ax_arr_new(4);
  uint32_t i = 0;
  while (i < s->len) {
    while (i < s->len && isspace((unsigned char)s->data[i])) i++;
    uint32_t start = i;
    while (i < s->len && !isspace((unsigned char)s->data[i])) i++;
    if (i > start) ax_arr_push(out, ax_strv(ax_str_new(s->data + start, i - start)));
  }
  ax_release(ax_strv(s));
  return ax_arrv(out);
}
NATIVE(n_chars) {
  AxStr *s = ARG(0).t == AX_NULL ? ax_str_newz("") : arg_str(ARG(0));
  AxArr *out = ax_arr_new(s->len);
  for (uint32_t i = 0; i < s->len; i++) ax_arr_push(out, ax_strv(ax_str_new(s->data + i, 1)));
  ax_release(ax_strv(s));
  return ax_arrv(out);
}
NATIVE(n_capitalize) {
  AxStr *s = arg_str(ARG(0));
  AxStr *out = ax_str_new(s->data, s->len);
  if (out->len) out->data[0] = (char)toupper((unsigned char)out->data[0]);
  ax_release(ax_strv(s));
  return ax_strv(out);
}
NATIVE(n_title) {
  AxStr *s = arg_str(ARG(0));
  // The reference's s.replace(/\w\S*/g, …): each run from a word character to the next white
  // space is capitalized — the first character up, the rest down.
  AxStr *out = ax_str_new(s->data, s->len);
  for (uint32_t i = 0; i < out->len;) {
    unsigned char c = (unsigned char)out->data[i];
    if (!(isalnum(c) || c == '_')) { i++; continue; }
    out->data[i] = (char)toupper(c);
    for (i++; i < out->len && !isspace((unsigned char)out->data[i]); i++) out->data[i] = (char)tolower((unsigned char)out->data[i]);
  }
  ax_release(ax_strv(s));
  return ax_strv(out);
}
NATIVE(n_reverse_str) {
  AxStr *s = arg_str(ARG(0));
  AxStr *out = ax_str_new(s->data, s->len);
  for (uint32_t i = 0, j = out->len ? out->len - 1 : 0; i < j; i++, j--) { char t = out->data[i]; out->data[i] = out->data[j]; out->data[j] = t; }
  ax_release(ax_strv(s));
  return ax_strv(out);
}
NATIVE(n_hash) {
  AxStr *s = arg_str(ARG(0));
  uint32_t h = ax_hash_bytes(s->data, s->len);
  ax_release(ax_strv(s));
  return ax_num((double)h);
}
static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
NATIVE(n_b64_encode) {
  AxStr *s = arg_str(ARG(0));
  size_t olen = 4 * ((s->len + 2) / 3);
  char *out = malloc(olen + 1);
  size_t j = 0;
  for (size_t i = 0; i < s->len; i += 3) {
    unsigned v = (unsigned char)s->data[i] << 16;
    if (i + 1 < s->len) v |= (unsigned char)s->data[i + 1] << 8;
    if (i + 2 < s->len) v |= (unsigned char)s->data[i + 2];
    out[j++] = B64[(v >> 18) & 63];
    out[j++] = B64[(v >> 12) & 63];
    out[j++] = i + 1 < s->len ? B64[(v >> 6) & 63] : '=';
    out[j++] = i + 2 < s->len ? B64[v & 63] : '=';
  }
  out[j] = '\0';
  AxValue r = ax_strv(ax_str_new(out, j));
  free(out);
  ax_release(ax_strv(s));
  return r;
}
// Bytes as Node's toString('utf8') reads them (the WHATWG decoder): each ill-formed sequence —
// the longest prefix of one that could still have been valid — becomes one U+FFFD.
static AxStr *utf8_repair(const uint8_t *b, size_t n) {
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

NATIVE(n_b64_decode) {
  AxStr *s = arg_str(ARG(0));
  uint8_t *bytes = NULL;
  size_t nb = 0;
  if (!ax_base64_decode(s->data, s->len, &bytes, &nb)) { bytes = NULL; nb = 0; }
  AxStr *r = utf8_repair(bytes ? bytes : (const uint8_t *)"", nb);
  free(bytes);
  ax_release(ax_strv(s));
  return ax_strv(r);
}
// JSON.stringify(v, null, pretty ? 2 : undefined) through the one JSON writer the runtime has
// (vectors are {"x":…}, functions left out); a big cannot be written, which the reference
// reports as this marker.
NATIVE(n_to_json) {
  char *json = NULL;
  bool ok = ax_json_write_checked(ARG(0), argc > 1 && ax_truthy(args[1]) ? 2 : 0, &json);
  AxValue r = ok ? ax_str_from(json) : ax_str_from("<circular>");
  free(json);
  return r;
}
// Parse JSON text into a value (dicts for objects); false when it is not valid JSON.
// JSON.parse: one value, then nothing but whitespace.
bool ax_json_parse(const char *text, AxValue *out) {
  JP j = { text, true };
  *out = json_read(&j);
  if (j.ok) {
    while (*j.p == ' ' || *j.p == '\t' || *j.p == '\n' || *j.p == '\r') j.p++;
    if (*j.p) j.ok = false;
  }
  if (!j.ok) { ax_release(*out); *out = ax_null(); }
  return j.ok;
}

NATIVE(n_from_json) {
  AxStr *s = arg_str(ARG(0));
  AxValue v;
  bool ok = ax_json_parse(s->data, &v);
  ax_release(ax_strv(s));
  return ok ? v : ax_null();
}

// --- regex ------------------------------------------------------------------------------------

NATIVE(n_re_test) {
  AxStr *s = arg_str(ARG(0));
  regex_t re;
  bool icase = re_flags(vm, ARG(1), ARG(2), false);
  re_build(vm, ARG(1), &re, icase);
  bool m = regexec(&re, s->data, 0, NULL, 0) == 0;
  regfree(&re);
  ax_release(ax_strv(s));
  return ax_bool(m);
}
NATIVE(n_re_match) {
  AxStr *s = arg_str(ARG(0));
  regex_t re;
  re_build(vm, ARG(1), &re, re_flags(vm, ARG(1), ARG(2), false));
  regmatch_t m[16];
  AxValue out = ax_null();
  if (regexec(&re, s->data, 16, m, 0) == 0) out = re_match_record(s->data, m, (int)re.re_nsub);
  regfree(&re);
  ax_release(ax_strv(s));
  return out;
}
NATIVE(n_re_all) {
  AxStr *s = arg_str(ARG(0));
  regex_t re;
  re_build(vm, ARG(1), &re, re_flags(vm, ARG(1), ARG(2), true));
  AxArr *out = ax_arr_new(4);
  regmatch_t m[16];
  const char *p = s->data;
  size_t off = 0;
  while (off <= s->len && regexec(&re, p + off, 16, m, off ? REG_NOTBOL : 0) == 0) {
    regmatch_t shifted[16];
    for (int g = 0; g < 16; g++) {
      shifted[g].rm_so = m[g].rm_so < 0 ? -1 : m[g].rm_so + (regoff_t)off;
      shifted[g].rm_eo = m[g].rm_eo < 0 ? -1 : m[g].rm_eo + (regoff_t)off;
    }
    ax_arr_push(out, re_match_record(s->data, shifted, (int)re.re_nsub));
    size_t adv = (size_t)(m[0].rm_eo > m[0].rm_so ? m[0].rm_eo : m[0].rm_so + 1);
    off += adv;
    if (adv == 0) break;
  }
  regfree(&re);
  ax_release(ax_strv(s));
  return ax_arrv(out);
}
NATIVE(n_re_sub) {
  AxStr *s = arg_str(ARG(0));
  AxStr *rep = arg_str(ARG(2));
  regex_t re;
  re_build(vm, ARG(1), &re, re_flags(vm, ARG(1), ARG(3), true));
  SB sb = {0};
  sb_add(&sb, "", 0);
  regmatch_t m[16];
  size_t off = 0;
  while (off <= s->len && regexec(&re, s->data + off, 16, m, off ? REG_NOTBOL : 0) == 0) {
    sb_add(&sb, s->data + off, (size_t)m[0].rm_so);
    // `$1`-style group references in the replacement.
    for (uint32_t i = 0; i < rep->len; i++) {
      if (rep->data[i] == '$' && i + 1 < rep->len && isdigit((unsigned char)rep->data[i + 1])) {
        int g = rep->data[i + 1] - '0';
        if (g <= (int)re.re_nsub && m[g].rm_so >= 0) sb_add(&sb, s->data + off + m[g].rm_so, (size_t)(m[g].rm_eo - m[g].rm_so));
        i++;
        continue;
      }
      sb_add(&sb, rep->data + i, 1);
    }
    size_t adv = (size_t)(m[0].rm_eo > m[0].rm_so ? m[0].rm_eo : m[0].rm_so + 1);
    if (m[0].rm_eo == m[0].rm_so && off + (size_t)m[0].rm_so < s->len) sb_add(&sb, s->data + off + m[0].rm_so, 1);
    off += adv;
    if (adv == 0) break;
  }
  if (off <= s->len) sb_add(&sb, s->data + off, s->len - off);
  regfree(&re);
  AxValue r = ax_strv(ax_str_new(sb.buf, sb.len));
  free(sb.buf);
  ax_release(ax_strv(s));
  ax_release(ax_strv(rep));
  return r;
}
NATIVE(n_re_split) {
  AxStr *s = arg_str(ARG(0));
  regex_t re;
  re_build(vm, ARG(1), &re, re_flags(vm, ARG(1), ARG(2), false));
  AxArr *out = ax_arr_new(4);
  regmatch_t m[1];
  size_t off = 0;
  while (off <= s->len && regexec(&re, s->data + off, 1, m, off ? REG_NOTBOL : 0) == 0 && m[0].rm_eo > m[0].rm_so) {
    ax_arr_push(out, ax_strv(ax_str_new(s->data + off, (size_t)m[0].rm_so)));
    off += (size_t)m[0].rm_eo;
  }
  ax_arr_push(out, ax_strv(ax_str_new(s->data + off, s->len - off)));
  regfree(&re);
  ax_release(ax_strv(s));
  return ax_arrv(out);
}

// --- randomness -------------------------------------------------------------------------------

double ax_rng_next(AxVM *vm);
static double rng_next(AxVM *vm) { return ax_rng_next(vm); }
double ax_rng_next(AxVM *vm) {
  // mulberry32, the same generator the reference uses, so a seeded run matches.
  uint32_t a = (vm->rng_state += 0x6D2B79F5u);
  uint32_t t = a;
  t = (t ^ (t >> 15)) * (1u | t);
  t ^= t + (t ^ (t >> 7)) * (61u | t);
  return (double)((t ^ (t >> 14)) >> 0) / 4294967296.0;
}
NATIVE(n_seed) { vm->rng_state = (uint32_t)NUM(0); return ax_num(NUM(0)); }
NATIVE(n_random) { return ax_num(rng_next(vm)); }
NATIVE(n_random_range) { double lo = NUM(0), hi = NUM(1); return ax_num(lo + rng_next(vm) * (hi - lo)); }
NATIVE(n_random_int) { double lo = NUM(0), hi = NUM(1); return ax_num(floor(lo + rng_next(vm) * (hi - lo + 1))); }
NATIVE(n_pick) {
  AxArr *a = ax_to_seq(vm, ARG(0));
  AxValue r = a->len ? ax_copy(a->items[(size_t)(rng_next(vm) * a->len)]) : ax_null();
  ax_release(ax_arrv(a));
  return r;
}
NATIVE(n_shuffle) {
  AxArr *src = ax_to_seq(vm, ARG(0));
  AxArr *out = ax_arr_new(src->len);
  for (uint32_t i = 0; i < src->len; i++) ax_arr_push(out, ax_copy(src->items[i]));
  ax_release(ax_arrv(src));
  for (int64_t i = (int64_t)out->len - 1; i > 0; i--) {
    int64_t j = (int64_t)(rng_next(vm) * (double)(i + 1));
    AxValue t = out->items[i];
    out->items[i] = out->items[j];
    out->items[j] = t;
  }
  return ax_arrv(out);
}
NATIVE(n_gauss) {
  double mu = DEF_NUM(0, 0), sigma = DEF_NUM(1, 1);
  double u = 1 - rng_next(vm), v = rng_next(vm);
  return ax_num(mu + sigma * sqrt(-2 * log(u)) * cos(2 * M_PI * v));
}
NATIVE(n_uuid) {
  char buf[40];
  const char *hex = "0123456789abcdef";
  int j = 0;
  for (int i = 0; i < 36; i++) {
    if (i == 8 || i == 13 || i == 18 || i == 23) { buf[j++] = '-'; continue; }
    if (i == 14) { buf[j++] = '4'; continue; }
    int r = (int)(rng_next(vm) * 16);
    if (i == 19) r = (r & 0x3) | 0x8;
    buf[j++] = hex[r];
  }
  buf[j] = '\0';
  return ax_str_from(buf);
}

// --- time -------------------------------------------------------------------------------------

NATIVE(n_now) { struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); return ax_num((double)ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6); }
NATIVE(n_time) { struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); return ax_num((double)ts.tv_sec + ts.tv_nsec / 1e9); }
NATIVE(n_clock) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ax_num((double)ts.tv_sec + ts.tv_nsec / 1e9); }
NATIVE(n_date_iso) {
  double ms = argc > 0 ? NUM(0) : 0;
  time_t secs;
  if (argc > 0) secs = (time_t)(ms / 1000.0);
  else { struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); secs = ts.tv_sec; ms = (double)ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6; }
  struct tm tmv;
  gmtime_r(&secs, &tmv);
  char buf[64];
  int millis = (int)fmod(ms, 1000.0);
  snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
           tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec, millis);
  return ax_str_from(buf);
}
NATIVE(n_sleep) {
  double secs = NUM(0);
  if (secs > 0) usleep((useconds_t)(secs * 1e6));
  return ax_null();
}

// --- files and process --------------------------------------------------------------------------

static char *read_whole(const char *path, size_t *out_len) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (n < 0) { fclose(f); return NULL; }
  char *buf = malloc((size_t)n + 1);
  size_t got = fread(buf, 1, (size_t)n, f);
  fclose(f);
  buf[got] = '\0';
  *out_len = got;
  return buf;
}

NATIVE(n_read) {
  AxStr *p = arg_str(ARG(0));
  check_read(vm, p->data);
  size_t len = 0;
  char *buf = read_whole(p->data, &len);
  ax_release(ax_strv(p));
  if (!buf) return argc > 1 ? ax_copy(args[1]) : ax_null();
  AxValue v = ax_strv(ax_str_new(buf, len));
  free(buf);
  return v;
}
NATIVE(n_read_lines) {
  AxValue text = n_read(vm, self, args, argc < 1 ? 0 : 1);
  if (text.t != AX_STR) { ax_release(text); return ax_arrv(ax_arr_new(0)); }
  AxValue one[1] = { text };
  AxValue r = n_lines(vm, self, one, 1);
  ax_release(text);
  return r;
}
NATIVE(n_read_json) {
  AxValue text = n_read(vm, self, args, 1);
  if (text.t != AX_STR) { ax_release(text); return argc > 1 ? ax_copy(args[1]) : ax_null(); }
  AxValue one[1] = { text };
  AxValue r = n_from_json(vm, self, one, 1);
  ax_release(text);
  return r;
}
static bool mkdir_parents(const char *path) {
  char tmp[4096];
  snprintf(tmp, sizeof tmp, "%s", path);
  char *slash = strrchr(tmp, '/');
  if (!slash) return true;
  *slash = '\0';
  char part[4096];
  size_t n = strlen(tmp);
  for (size_t i = 1; i <= n; i++) {
    if (tmp[i] == '/' || i == n) {
      memcpy(part, tmp, i);
      part[i] = '\0';
      mkdir(part, 0777);
    }
  }
  return true;
}
NATIVE(n_write) {
  AxStr *p = arg_str(ARG(0));
  check_write(vm, p->data);
  AxStr *data = arg_str(ARG(1));
  mkdir_parents(p->data);
  FILE *f = fopen(p->data, "wb");
  bool ok = f != NULL;
  if (f) { fwrite(data->data, 1, data->len, f); fclose(f); }
  ax_release(ax_strv(p));
  ax_release(ax_strv(data));
  return ax_bool(ok);
}
NATIVE(n_append) {
  AxStr *p = arg_str(ARG(0));
  check_write(vm, p->data);
  AxStr *data = arg_str(ARG(1));
  mkdir_parents(p->data);
  FILE *f = fopen(p->data, "ab");
  bool ok = f != NULL;
  if (f) { fwrite(data->data, 1, data->len, f); fclose(f); }
  ax_release(ax_strv(p));
  ax_release(ax_strv(data));
  return ax_bool(ok);
}
NATIVE(n_write_json) {
  AxValue jargs[2] = { ARG(1), argc > 2 ? args[2] : ax_null() };
  AxValue text = n_to_json(vm, self, jargs, 2);
  AxValue wargs[2] = { ARG(0), text };
  AxValue r = n_write(vm, self, wargs, 2);
  ax_release(text);
  return r;
}
NATIVE(n_file_exists) {
  AxStr *p = arg_str(ARG(0));
  bool ok = access(p->data, F_OK) == 0;
  ax_release(ax_strv(p));
  return ax_bool(ok);
}
NATIVE(n_is_dir) {
  AxStr *p = arg_str(ARG(0));
  struct stat st;
  bool ok = stat(p->data, &st) == 0 && S_ISDIR(st.st_mode);
  ax_release(ax_strv(p));
  return ax_bool(ok);
}
NATIVE(n_ls) {
  AxStr *p = argc ? arg_str(ARG(0)) : ax_str_newz(".");
  check_read(vm, p->data);
  AxArr *out = ax_arr_new(8);
  DIR *d = opendir(p->data);
  if (d) {
    struct dirent *e;
    while ((e = readdir(d))) {
      if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
      ax_arr_push(out, ax_str_from(e->d_name));
    }
    closedir(d);
  }
  ax_release(ax_strv(p));
  // Directory order is filesystem-dependent; sort so both runtimes agree.
  if (out->len > 1) qsort(out->items, out->len, sizeof(AxValue), cmp_values_asc);
  return ax_arrv(out);
}
NATIVE(n_mkdir) {
  AxStr *p = arg_str(ARG(0));
  check_write(vm, p->data);
  char tmp[4096];
  snprintf(tmp, sizeof tmp, "%s/", p->data);
  mkdir_parents(tmp);
  mkdir(p->data, 0777);
  ax_release(ax_strv(p));
  return ax_bool(true);
}
NATIVE(n_rm) {
  AxStr *p = arg_str(ARG(0));
  check_write(vm, p->data);
  bool ok = remove(p->data) == 0;
  ax_release(ax_strv(p));
  return ax_bool(ok);
}
NATIVE(n_path_join) {
  SB sb = {0};
  sb_add(&sb, "", 0);
  for (int i = 0; i < argc; i++) {
    AxStr *s = arg_str(args[i]);
    if (sb.len && sb.buf[sb.len - 1] != '/' && s->len && s->data[0] != '/') sb_addz(&sb, "/");
    sb_add(&sb, s->data, s->len);
    ax_release(ax_strv(s));
  }
  AxValue r = ax_strv(ax_str_new(sb.buf, sb.len));
  free(sb.buf);
  return r;
}
NATIVE(n_input) {
  if (argc > 0 && args[0].t != AX_NULL) {
    AxStr *p = arg_str(args[0]);
    ax_write(vm, 1, p->data, p->len);
    fflush(stdout);
    ax_release(ax_strv(p));
  }
  char *line = NULL;
  size_t cap = 0;
  ssize_t n = getline(&line, &cap, stdin);
  if (n < 0) { free(line); return ax_null(); }
  while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) n--;
  AxValue v = ax_strv(ax_str_new(line, (size_t)n));
  free(line);
  return v;
}
NATIVE(n_read_stdin) {
  SB sb = {0};
  sb_add(&sb, "", 0);
  char buf[4096];
  size_t got;
  while ((got = fread(buf, 1, sizeof buf, stdin)) > 0) sb_add(&sb, buf, got);
  AxValue v = ax_strv(ax_str_new(sb.buf, sb.len));
  free(sb.buf);
  return v;
}
NATIVE(n_args) { ax_retain(ax_arrv(vm->argv)); return ax_arrv(vm->argv); }
NATIVE(n_env) {
  AxStr *name = arg_str(ARG(0));
  const char *v = getenv(name->data);
  ax_release(ax_strv(name));
  if (!v) return argc > 1 ? ax_copy(args[1]) : ax_null();
  return ax_str_from(v);
}
NATIVE(n_exit) {
  int code = ax_to_int32(NUM(0));
  vm->exit_code = code;
  // exit() ends the program wherever it is called: not catchable by ^try, not a fault. The
  // command's top level lands here to flush output and, with --json, print the result.
  if (vm->exit_jmp) longjmp(*vm->exit_jmp, 1);
  fflush(stdout);
  exit(code);
}
NATIVE(n_sh) {
  if (vm->sandbox && !vm->allow_exec) ax_throw(vm, "AX-SANDBOX-001", "sandbox: running commands is not allowed (pass --allow-exec)");
  AxStr *cmd = arg_str(ARG(0));
  SB sb = {0};
  sb_add(&sb, "", 0);
#ifdef __wasi__
  // WebAssembly has no processes to start.
  ax_release(ax_strv(cmd));
  free(sb.buf);
  ax_throw(vm, "AX-SANDBOX-001", "sandbox: running commands is not available in the WebAssembly build");
  FILE *pipe = NULL;
#else
  FILE *pipe = popen(cmd->data, "r");
#endif
  int status = -1;
  if (pipe) {
    char buf[4096];
    size_t got;
    while ((got = fread(buf, 1, sizeof buf, pipe)) > 0) sb_add(&sb, buf, got);
#ifndef __wasi__
    status = pclose(pipe);
    if (status != -1) status = WEXITSTATUS(status);
#endif
  }
  ax_release(ax_strv(cmd));
  AxDict *d = ax_dict_new();
  AxStr *k_out = ax_internz("out"), *k_code = ax_internz("code");
  ax_dict_set(d, k_out, ax_strv(ax_str_new(sb.buf, sb.len)));
  ax_dict_set(d, k_code, ax_num(status));
  ax_release(ax_strv(k_out));
  ax_release(ax_strv(k_code));
  free(sb.buf);
  return ax_dictv(d);
}

// --- functional -------------------------------------------------------------------------------

NATIVE(n_apply) {
  AxArr *a = ax_to_seq(vm, ARG(1));
  AxValue r = ax_call(vm, ARG(0), a->items, (int)a->len);
  ax_release(ax_arrv(a));
  return r;
}

// `partial`, `compose` and `memo` return natives that carry their captured state in `self->bound`.
static AxValue make_bound_native(const char *name, AxNativeFn fn, AxValue bound) {
  AxValue v = ax_native(name, fn, 0, -1);
  AxFn *f = (AxFn *)v.o;
  f->bound = bound;
  f->has_bound = true;
  return v;
}

NATIVE(n_partial_call) {
  AxArr *cap = (AxArr *)self->bound.o;   // [fn, bound args…]
  int extra = (int)cap->len - 1;
  AxValue *all = calloc(extra + argc, sizeof(AxValue));
  for (int i = 0; i < extra; i++) all[i] = cap->items[i + 1];
  for (int i = 0; i < argc; i++) all[extra + i] = args[i];
  AxValue r = ax_call(vm, cap->items[0], all, extra + argc);
  free(all);
  return r;
}
NATIVE(n_partial) {
  AxArr *cap = ax_arr_new(argc);
  for (int i = 0; i < argc; i++) ax_arr_push(cap, ax_copy(args[i]));
  return make_bound_native("partial", n_partial_call, ax_arrv(cap));
}

NATIVE(n_compose_call) {
  AxArr *fns = (AxArr *)self->bound.o;
  if (!fns->len) return ax_null();
  AxValue acc = ax_call(vm, fns->items[fns->len - 1], args, argc);
  for (int64_t i = (int64_t)fns->len - 2; i >= 0; i--) {
    AxValue next = ax_call(vm, fns->items[i], &acc, 1);
    ax_release(acc);
    acc = next;
  }
  return acc;
}
NATIVE(n_compose) {
  AxArr *fns = ax_arr_new(argc);
  for (int i = 0; i < argc; i++) ax_arr_push(fns, ax_copy(args[i]));
  return make_bound_native("compose", n_compose_call, ax_arrv(fns));
}

NATIVE(n_memo_call) {
  AxArr *cap = (AxArr *)self->bound.o;   // [fn, cache dict]
  SB sb = {0};
  sb_add(&sb, "", 0);
  for (int i = 0; i < argc; i++) {
    AxStr *s = ax_to_str(args[i]);
    sb_add(&sb, s->data, s->len);
    sb_addz(&sb, "\x01");
    ax_release(ax_strv(s));
  }
  AxStr *key = ax_intern(sb.buf, sb.len);
  free(sb.buf);
  AxDict *cache = (AxDict *)cap->items[1].o;
  AxValue hit;
  if (ax_dict_get(cache, key, &hit)) { ax_release(ax_strv(key)); return hit; }
  AxValue r = ax_call(vm, cap->items[0], args, argc);
  ax_dict_set(cache, key, ax_copy(r));
  ax_release(ax_strv(key));
  return r;
}
NATIVE(n_memo) {
  AxArr *cap = ax_arr_new(2);
  ax_arr_push(cap, ax_copy(ARG(0)));
  ax_arr_push(cap, ax_dictv(ax_dict_new()));
  return make_bound_native("memo", n_memo_call, ax_arrv(cap));
}

NATIVE(n_check) {
  if (ax_truthy(ARG(0))) return ax_bool(true);
  AxStr *msg = argc > 1 ? arg_str(args[1]) : ax_str_newz("check failed");
  char buf[400];
  snprintf(buf, sizeof buf, "%s", msg->data);
  ax_release(ax_strv(msg));
  ax_throw(vm, "AX-CHECK", "%s", buf);
  return ax_null();
}
NATIVE(n_check_eq) {
  if (ax_equals(ARG(0), ARG(1))) return ax_bool(true);
  AxStr *a = ax_to_str(ARG(0)), *b = ax_to_str(ARG(1));
  AxStr *msg = argc > 2 ? arg_str(args[2]) : ax_str_newz("check_eq failed");
  char buf[400];
  snprintf(buf, sizeof buf, "%s: %s != %s", msg->data, a->data, b->data);
  ax_release(ax_strv(a));
  ax_release(ax_strv(b));
  ax_release(ax_strv(msg));
  ax_throw(vm, "AX-CHECK", "%s", buf);
  return ax_null();
}

// ---------------------------------------------------------------------------------------------
// Method dispatch
// ---------------------------------------------------------------------------------------------

static bool name_is(AxStr *n, const char *s) { return strcmp(n->data, s) == 0; }

// A method's selector is checked before any element is seen, as the reference builds it first:
// null (or none), a field name, or something callable — `[].map(2)` is an error, not [].
static void method_selector(AxVM *vm, AxValue sel) {
  if (sel.t == AX_NULL || sel.t == AX_STR || sel.t == AX_ATOM || sel.t == AX_FN) return;
  ax_throw(vm, "AX-CALL-001", "value of type %s is not callable", ax_type_name(sel));
}
// The selector applied to a dict entry: (value, key).
static AxValue selector_apply_kv(AxVM *vm, AxValue sel, AxValue v, AxStr *k) {
  if (sel.t == AX_NULL) return ax_copy(v);
  bool is_fn = sel.t == AX_FN;
  if (!is_fn && sel.t == AX_ATOM) {
    AxValue f;
    if (ax_scope_lookup(vm->builtins, (AxStr *)sel.o, &f)) { is_fn = f.t == AX_FN; ax_release(f); }
  }
  if (is_fn) {
    AxValue kv = ax_strv(k);
    AxValue cargs[2] = { v, kv };
    return ax_call(vm, sel, cargs, 2);
  }
  return ax_key_apply(vm, sel, v, 0);
}
// JavaScript's === (includes: SameValueZero, where NaN equals NaN): values by value, containers
// by identity — [[1]].includes([1]) is false.
static bool js_same(AxValue a, AxValue b, bool nan_equal) {
  if (a.t != b.t) return false;
  switch (a.t) {
    case AX_NULL: return true;
    case AX_BOOL: return a.b == b.b;
    case AX_NUM: return a.num == b.num || (nan_equal && isnan(a.num) && isnan(b.num));
    case AX_STR: return ax_str_eq((AxStr *)a.o, (AxStr *)b.o);
    case AX_BIG: return ax_big_cmp((AxBig *)a.o, (AxBig *)b.o) == 0;
    default: return a.o == b.o;
  }
}
// Missing arguments of a string method read as JavaScript's undefined does: "undefined".
static AxStr *method_text(AxValue *args, int argc, int i) {
  return i < argc ? ax_to_str(args[i]) : ax_str_newz("undefined");
}

AxValue ax_method_call(AxVM *vm, AxValue obj, AxStr *name, AxValue *args, int argc) {
  // A range takes the array methods, as the array it stands for: range(0, n).map(f).
  if (obj.t == AX_RANGE) {
    AxArr *seq = ax_to_seq(vm, obj);
    AxValue r = ax_method_call(vm, ax_arrv(seq), name, args, argc);
    ax_release(ax_arrv(seq));
    return r;
  }
  // A field holding a function is a method — how objects are written without a class construct.
  if (obj.t == AX_DICT) {
    AxValue member;
    if (ax_dict_get((AxDict *)obj.o, name, &member)) {
      if (member.t == AX_FN) {
        AxValue r = ax_call(vm, member, args, argc);
        ax_release(member);
        return r;
      }
      ax_release(member);
    }
  }

  if (obj.t == AX_STR) {
    AxStr *s = (AxStr *)obj.o;
    if (name_is(name, "len")) return ax_num(s->len);
    if (name_is(name, "upper") || name_is(name, "lower")) {
      AxStr *out = ax_str_new(s->data, s->len);
      bool up = name_is(name, "upper");
      for (uint32_t i = 0; i < out->len; i++) out->data[i] = (char)(up ? toupper((unsigned char)out->data[i]) : tolower((unsigned char)out->data[i]));
      return ax_strv(out);
    }
    if (name_is(name, "trim") || name_is(name, "trim_start") || name_is(name, "trim_end")) {
      uint32_t a = 0, b = s->len;
      if (!name_is(name, "trim_end")) while (a < b && isspace((unsigned char)s->data[a])) a++;
      if (!name_is(name, "trim_start")) while (b > a && isspace((unsigned char)s->data[b - 1])) b--;
      return ax_strv(ax_str_new(s->data + a, b - a));
    }
    if (name_is(name, "split") && argc == 0) {
      AxArr *out = ax_arr_new(1);   // split() with no separator: the whole string
      ax_retain(obj);
      ax_arr_push(out, obj);
      return ax_arrv(out);
    }
    if (name_is(name, "match")) {
      // String.prototype.match with a pattern given as text: the first match, as re_match.
      AxValue margs[2] = { obj, argc ? args[0] : ax_str_from("") };
      AxValue r = n_re_match(vm, NULL, margs, 2);
      if (!argc) ax_release(margs[1]);
      return r;
    }
    if (name_is(name, "split")) {
      AxStr *sep = argc ? ax_to_str(args[0]) : ax_str_newz("");
      AxArr *out = ax_arr_new(4);
      if (!sep->len) {
        for (uint32_t i = 0; i < s->len; i++) ax_arr_push(out, ax_strv(ax_str_new(s->data + i, 1)));
      } else {
        uint32_t start = 0;
        for (uint32_t i = 0; i + sep->len <= s->len; ) {
          if (memcmp(s->data + i, sep->data, sep->len) == 0) {
            ax_arr_push(out, ax_strv(ax_str_new(s->data + start, i - start)));
            i += sep->len;
            start = i;
            continue;
          }
          i++;
        }
        ax_arr_push(out, ax_strv(ax_str_new(s->data + start, s->len - start)));
      }
      ax_release(ax_strv(sep));
      return ax_arrv(out);
    }
    if (name_is(name, "replace") || name_is(name, "replace_all")) {
      AxStr *from = method_text(args, argc, 0);
      AxStr *to = argc > 1 && args[1].t != AX_NULL ? ax_to_str(args[1]) : ax_str_newz("");
      SB sb = {0};
      sb_add(&sb, "", 0);
      bool all = name_is(name, "replace_all");
      bool done = false;
      // An empty pattern matches before the first character (replace) or between every two
      // (replace_all, which is split("").join(to)).
      if (!from->len && !all) sb_add(&sb, to->data, to->len);
      for (uint32_t i = 0; i < s->len; ) {
        if (!from->len && all && i) sb_add(&sb, to->data, to->len);
        if (from->len && !(done && !all) && i + from->len <= s->len && memcmp(s->data + i, from->data, from->len) == 0) {
          sb_add(&sb, to->data, to->len);
          i += from->len;
          done = true;
          continue;
        }
        sb_add(&sb, s->data + i, 1);
        i++;
      }
      AxValue r = ax_strv(ax_str_new(sb.buf, sb.len));
      free(sb.buf);
      ax_release(ax_strv(from));
      ax_release(ax_strv(to));
      return r;
    }
    if (name_is(name, "startsWith")) {
      AxStr *p = method_text(args, argc, 0);
      bool r = p->len <= s->len && memcmp(s->data, p->data, p->len) == 0;
      ax_release(ax_strv(p));
      return ax_bool(r);
    }
    if (name_is(name, "endsWith")) {
      AxStr *p = method_text(args, argc, 0);
      bool r = p->len <= s->len && memcmp(s->data + s->len - p->len, p->data, p->len) == 0;
      ax_release(ax_strv(p));
      return ax_bool(r);
    }
    if (name_is(name, "includes") || name_is(name, "contains")) {
      AxStr *p = method_text(args, argc, 0);
      bool r = p->len == 0 || strstr(s->data, p->data) != NULL;
      ax_release(ax_strv(p));
      return ax_bool(r);
    }
    if (name_is(name, "indexOf")) {
      AxStr *p = method_text(args, argc, 0);
      const char *hit = p->len ? strstr(s->data, p->data) : s->data;
      double idx = hit ? (double)(hit - s->data) : -1;
      ax_release(ax_strv(p));
      return ax_num(idx);
    }
    if (name_is(name, "count")) {
      AxStr *p = method_text(args, argc, 0);
      double n = 0;
      if (p->len) for (uint32_t i = 0; i + p->len <= s->len; ) { if (memcmp(s->data + i, p->data, p->len) == 0) { n++; i += p->len; } else i++; }
      ax_release(ax_strv(p));
      return ax_num(n);
    }
    if (name_is(name, "repeat")) {
      double c = argc ? ax_to_num(args[0]) : 0;
      if (isnan(c)) c = 0;
      if (c < 0 || isinf(c)) {
        AxStr *cs = ax_to_str(ax_num(c));
        char msg[64];
        snprintf(msg, sizeof msg, "Invalid count value: %s", cs->data);
        ax_release(ax_strv(cs));
        ax_throw(vm, "AX-RUNTIME-000", "%s", msg);
      }
      int64_t n = ax_js_int(c);
      if (s->len && (double)n * s->len > 536870888.0) ax_throw(vm, "AX-RUNTIME-000", "Invalid string length");
      SB sb = {0};
      sb_add(&sb, "", 0);
      for (int64_t i = 0; i < n; i++) sb_add(&sb, s->data, s->len);
      AxValue r = ax_strv(ax_str_new(sb.buf, sb.len));
      free(sb.buf);
      return r;
    }
    if (name_is(name, "slice")) {
      int64_t a = argc ? ax_js_int(ax_to_num(args[0])) : 0;
      int64_t b = argc > 1 && args[1].t != AX_NULL ? ax_js_int(ax_to_num(args[1])) : (int64_t)s->len;
      if (a < 0) a += s->len;
      if (b < 0) b += s->len;
      if (a < 0) a = 0;
      if (b > (int64_t)s->len) b = s->len;
      if (b < a) b = a;
      return ax_strv(ax_str_new(s->data + a, (size_t)(b - a)));
    }
    if (name_is(name, "padStart") || name_is(name, "padEnd")) {
      // String.prototype.padStart/padEnd: the fill (a space when absent or empty) repeats and is
      // cut to fit; the target is a length (NaN and negatives are 0).
      int64_t want = argc ? ax_js_int(ax_to_num(args[0])) : 0;
      AxStr *fill = (argc > 1 && args[1].t != AX_NULL) ? ax_to_str(args[1]) : ax_str_newz(" ");
      if (!fill->len) { ax_release(ax_strv(fill)); fill = ax_str_newz(" "); }
      if (want > 536870888) ax_throw(vm, "AX-RUNTIME-000", "Invalid string length");
      int64_t need = want - (int64_t)s->len;
      if (need < 0) need = 0;
      SB sb = {0};
      sb_add(&sb, "", 0);
      if (!name_is(name, "padStart")) sb_add(&sb, s->data, s->len);
      for (int64_t k = 0; k < need; k++) sb_add(&sb, fill->data + (k % fill->len), 1);
      if (name_is(name, "padStart")) sb_add(&sb, s->data, s->len);
      AxValue r = ax_strv(ax_str_new(sb.buf, sb.len));
      free(sb.buf);
      ax_release(ax_strv(fill));
      return r;
    }
    if (name_is(name, "charAt")) {
      int64_t i = argc ? ax_js_int(ax_to_num(args[0])) : 0;
      if (i < 0 || i >= (int64_t)s->len) return ax_str_from("");
      return ax_strv(ax_str_new(s->data + i, 1));
    }
    if (name_is(name, "charCodeAt")) {
      int64_t i = argc ? ax_js_int(ax_to_num(args[0])) : 0;
      if (i < 0 || i >= (int64_t)s->len) return ax_num(NAN);
      return ax_num((unsigned char)s->data[i]);
    }
    if (name_is(name, "to_int") || name_is(name, "to_num")) {
      // parseInt(s.trim(), radix || 10) / Number(s.trim()), null for NaN.
      double d;
      if (name_is(name, "to_int")) {
        double r = argc ? ax_to_num(args[0]) : 0;
        d = js_parse_int(s->data, s->len, (r == 0 || isnan(r)) ? 10 : ax_to_int32(r));
      } else d = ax_to_num(obj);
      return isnan(d) ? ax_null() : ax_num(d);
    }
    if (name_is(name, "is_empty")) return ax_bool(s->len == 0);
    // Function forms that read naturally as methods.
    AxValue self_args[4];
    self_args[0] = obj;
    for (int i = 0; i < argc && i < 3; i++) self_args[i + 1] = args[i];
    if (name_is(name, "lines")) return n_lines(vm, NULL, self_args, 1);
    if (name_is(name, "words")) return n_words(vm, NULL, self_args, 1);
    if (name_is(name, "chars")) return n_chars(vm, NULL, self_args, 1);
    if (name_is(name, "capitalize")) return n_capitalize(vm, NULL, self_args, 1);
    if (name_is(name, "reverse")) return n_reverse_str(vm, NULL, self_args, 1);
  }

  if (obj.t == AX_ARR) {
    AxArr *a = (AxArr *)obj.o;
    if (name_is(name, "push") || name_is(name, "append")) {
      for (int i = 0; i < argc; i++) ax_arr_push(a, ax_copy(args[i]));
      return ax_num(a->len);
    }
    if (name_is(name, "pop")) {
      if (!a->len) return ax_null();
      AxValue v = a->items[--a->len];
      return v;   // ownership transfers to the caller
    }
    if (name_is(name, "shift")) {
      if (!a->len) return ax_null();
      AxValue v = a->items[0];
      memmove(a->items, a->items + 1, sizeof(AxValue) * (a->len - 1));
      a->len--;
      return v;
    }
    if (name_is(name, "unshift")) {
      for (int i = argc - 1; i >= 0; i--) {
        ax_arr_push(a, ax_null());
        memmove(a->items + 1, a->items, sizeof(AxValue) * (a->len - 1));
        a->items[0] = ax_copy(args[i]);
      }
      return ax_num(a->len);
    }
    if (name_is(name, "len")) return ax_num(a->len);
    if (name_is(name, "first")) return a->len ? ax_copy(a->items[0]) : ax_null();
    if (name_is(name, "last")) return a->len ? ax_copy(a->items[a->len - 1]) : ax_null();
    if (name_is(name, "clear")) { for (uint32_t i = 0; i < a->len; i++) ax_release(a->items[i]); a->len = 0; ax_retain(obj); return obj; }
    if (name_is(name, "join")) {
      AxStr *sep = argc ? ax_to_str(args[0]) : ax_str_newz(",");
      SB sb = {0};
      sb_add(&sb, "", 0);
      for (uint32_t i = 0; i < a->len; i++) {
        if (i) sb_add(&sb, sep->data, sep->len);
        if (a->items[i].t == AX_NULL) continue;
        AxStr *s = ax_to_str(a->items[i]);
        sb_add(&sb, s->data, s->len);
        ax_release(ax_strv(s));
      }
      AxValue r = ax_strv(ax_str_new(sb.buf, sb.len));
      free(sb.buf);
      ax_release(ax_strv(sep));
      return r;
    }
    if (name_is(name, "slice")) {
      int64_t s0 = argc ? ax_js_int(ax_to_num(args[0])) : 0;
      int64_t e0 = argc > 1 && args[1].t != AX_NULL ? ax_js_int(ax_to_num(args[1])) : (int64_t)a->len;
      if (s0 < 0) s0 += a->len;
      if (e0 < 0) e0 += a->len;
      if (s0 < 0) s0 = 0;
      if (e0 > (int64_t)a->len) e0 = a->len;
      AxArr *out = ax_arr_new(4);
      for (int64_t i = s0; i < e0; i++) ax_arr_push(out, ax_copy(a->items[i]));
      return ax_arrv(out);
    }
    // includes/indexOf/lastIndexOf compare as JavaScript does: by value for numbers, text and
    // bigs, by identity for containers (a missing argument looks for null).
    AxValue needle = argc ? args[0] : ax_null();
    if (name_is(name, "includes") || name_is(name, "contains")) {
      for (uint32_t i = 0; i < a->len; i++) if (js_same(a->items[i], needle, true)) return ax_bool(true);
      return ax_bool(false);
    }
    if (name_is(name, "indexOf")) {
      for (uint32_t i = 0; i < a->len; i++) if (js_same(a->items[i], needle, false)) return ax_num(i);
      return ax_num(-1);
    }
    if (name_is(name, "lastIndexOf")) {
      for (uint32_t i = a->len; i-- > 0;) if (js_same(a->items[i], needle, false)) return ax_num(i);
      return ax_num(-1);
    }
    if (name_is(name, "keys") || name_is(name, "values") || name_is(name, "entries")) {
      AxValue one[1] = { obj };
      return name_is(name, "keys") ? n_keys(vm, NULL, one, 1) : name_is(name, "values") ? n_values(vm, NULL, one, 1) : n_items(vm, NULL, one, 1);
    }
    if (name_is(name, "fill")) {
      // Array.prototype.fill(value, start, end): relative indices, the array itself returned.
      int64_t n = a->len;
      int64_t st = argc > 1 ? ax_js_int(ax_to_num(args[1])) : 0, en = argc > 2 ? ax_js_int(ax_to_num(args[2])) : n;
      if (st < 0) st = st + n < 0 ? 0 : st + n; else if (st > n) st = n;
      if (en < 0) en = en + n < 0 ? 0 : en + n; else if (en > n) en = n;
      for (int64_t i = st; i < en; i++) { ax_release(a->items[i]); a->items[i] = argc ? ax_copy(args[0]) : ax_null(); }
      ax_retain(obj);
      return obj;
    }
    if (name_is(name, "splice")) {
      // splice(start, count, …items): the removed elements; count is always given (a missing one
      // is 0, as the reference passes it through).
      int64_t n = a->len;
      int64_t st = argc ? ax_js_int(ax_to_num(args[0])) : 0;
      if (st < 0) st = st + n < 0 ? 0 : st + n; else if (st > n) st = n;
      int64_t dc = argc > 1 ? ax_js_int(ax_to_num(args[1])) : 0;
      if (dc < 0) dc = 0;
      if (dc > n - st) dc = n - st;
      AxArr *removed = ax_arr_new((uint32_t)dc);
      for (int64_t i = 0; i < dc; i++) ax_arr_push(removed, a->items[st + i]);   // ownership moves
      int ins = argc > 2 ? argc - 2 : 0;
      AxArr *rest = ax_arr_new((uint32_t)(n - st - dc));
      for (int64_t i = st + dc; i < n; i++) ax_arr_push(rest, a->items[i]);
      a->len = (uint32_t)st;
      for (int i = 0; i < ins; i++) ax_arr_push(a, ax_copy(args[2 + i]));
      for (uint32_t i = 0; i < rest->len; i++) ax_arr_push(a, rest->items[i]);
      rest->len = 0;
      ax_release(ax_arrv(rest));
      return ax_arrv(removed);
    }
    if (name_is(name, "reverse")) {
      for (uint32_t i = 0, j = a->len ? a->len - 1 : 0; i < j; i++, j--) { AxValue t = a->items[i]; a->items[i] = a->items[j]; a->items[j] = t; }
      ax_retain(obj);
      return obj;
    }
    if (name_is(name, "sort")) {
      AxValue sel = argc ? args[0] : ax_null();
      method_selector(vm, sel);
      SortCtx c = { vm, sel, false };
      if (sel.t == AX_FN) {
        AxFn *f = (AxFn *)sel.o;
        c.comparator = (!f->native && f->nparams >= 2) || (f->native && f->min_args >= 2);
      }
      if (a->len > 1) {
        timsort(&c, a->items, a->len);
      }
      ax_retain(obj);
      return obj;
    }
    // Methods that take a selector check it before looking at any element.
    static const char *const selector_methods[] = { "map", "filter", "reject", "find", "find_index", "findIndex", "each",
      "forEach", "every", "all", "some", "any", "flat_map", "flatMap", "sum", "min", "max", "sort_by", "group_by", "uniq", NULL };
    for (int i = 0; selector_methods[i]; i++) if (name_is(name, selector_methods[i])) { method_selector(vm, argc ? args[0] : ax_null()); break; }
    if (name_is(name, "each") || name_is(name, "forEach")) {
      AxValue sel = argc ? args[0] : ax_null();
      for (uint32_t i = 0; i < a->len; i++) ax_release(ax_key_apply(vm, sel, a->items[i], i));
      return ax_null();
    }
    if (name_is(name, "count")) {
      // No argument: the length; a function: how many it accepts; anything else, null
      // included: how many equal it.
      if (!argc) return ax_num(a->len);
      AxValue fn;
      bool callable = args[0].t == AX_FN || (args[0].t == AX_ATOM && ax_scope_lookup(vm->builtins, (AxStr *)args[0].o, &fn) && (ax_release(fn), fn.t == AX_FN));
      double n = 0;
      for (uint32_t i = 0; i < a->len; i++) {
        if (callable) { AxValue k = ax_key_apply(vm, args[0], a->items[i], i); n += ax_truthy(k); ax_release(k); }
        else n += ax_equals(a->items[i], args[0]);
      }
      return ax_num(n);
    }
    // Everything else routes to the function form with the array as the first argument.
    AxValue fargs[4];
    fargs[0] = obj;
    for (int i = 0; i < argc && i < 3; i++) fargs[i + 1] = args[i];
    int fargc = argc + 1 > 4 ? 4 : argc + 1;
    if (name_is(name, "map")) return n_map(vm, NULL, fargs, fargc);
    if (name_is(name, "filter")) return n_filter(vm, NULL, fargs, fargc);
    if (name_is(name, "reject")) {
      AxValue kept = n_filter(vm, NULL, fargs, fargc);
      AxArr *k = (AxArr *)kept.o;
      AxArr *out = ax_arr_new(4);
      for (uint32_t i = 0; i < a->len; i++) {
        bool in_kept = false;
        for (uint32_t j = 0; j < k->len && !in_kept; j++) in_kept = (k->items[j].o == a->items[i].o) && (k->items[j].t == a->items[i].t);
        if (!in_kept) ax_arr_push(out, ax_copy(a->items[i]));
      }
      ax_release(kept);
      return ax_arrv(out);
    }
    if (name_is(name, "reduce")) return n_reduce(vm, NULL, fargs, fargc);
    if (name_is(name, "find")) return n_find(vm, NULL, fargs, fargc);
    if (name_is(name, "find_index") || name_is(name, "findIndex")) return n_find_index(vm, NULL, fargs, fargc);
    if (name_is(name, "forEach") || name_is(name, "each")) return n_each(vm, NULL, fargs, fargc);
    if (name_is(name, "every") || name_is(name, "all")) return n_all(vm, NULL, fargs, fargc);
    if (name_is(name, "some") || name_is(name, "any")) return n_any(vm, NULL, fargs, fargc);
    if (name_is(name, "count")) return n_count(vm, NULL, fargs, fargc);
    if (name_is(name, "sum")) return n_sum(vm, NULL, fargs, fargc);
    if (name_is(name, "min")) return n_min_by(vm, NULL, fargs, fargc);
    if (name_is(name, "max")) return n_max_by(vm, NULL, fargs, fargc);
    if (name_is(name, "sort_by")) return n_sort_by(vm, NULL, fargs, fargc);
    if (name_is(name, "group_by")) return n_group_by(vm, NULL, fargs, fargc);
    if (name_is(name, "uniq")) return n_uniq(vm, NULL, fargs, fargc);
    if (name_is(name, "flat") || name_is(name, "flatten")) return n_flatten(vm, NULL, fargs, fargc);
    if (name_is(name, "flat_map") || name_is(name, "flatMap")) {
      AxValue mapped = n_map(vm, NULL, fargs, fargc);
      AxValue one[1] = { mapped };
      AxValue r = n_flatten(vm, NULL, one, 1);
      ax_release(mapped);
      return r;
    }
    if (name_is(name, "concat")) {
      AxArr *out = ax_arr_new(a->len + 4);
      for (uint32_t i = 0; i < a->len; i++) ax_arr_push(out, ax_copy(a->items[i]));
      for (int i = 0; i < argc; i++) {
        if (args[i].t == AX_ARR) { AxArr *b = (AxArr *)args[i].o; for (uint32_t j = 0; j < b->len; j++) ax_arr_push(out, ax_copy(b->items[j])); }
        else ax_arr_push(out, ax_copy(args[i]));
      }
      return ax_arrv(out);
    }
  }

  if (obj.t == AX_DICT) {
    AxDict *d = (AxDict *)obj.o;
    AxValue fargs[4];
    fargs[0] = obj;
    for (int i = 0; i < argc && i < 3; i++) fargs[i + 1] = args[i];
    int fargc = argc + 1 > 4 ? 4 : argc + 1;
    if (name_is(name, "keys")) return n_keys(vm, NULL, fargs, 1);
    if (name_is(name, "values")) return n_values(vm, NULL, fargs, 1);
    if (name_is(name, "items") || name_is(name, "entries")) return n_items(vm, NULL, fargs, 1);
    if (name_is(name, "len")) return ax_num(ax_dict_count(d));
    if (name_is(name, "is_empty")) return ax_bool(ax_dict_count(d) == 0);
    if (name_is(name, "merge")) return n_merge(vm, NULL, fargs, fargc);
    if (name_is(name, "clone")) return deep_clone(obj);
    // Keys as d[k] reads them; a missing key is null.
    if (name_is(name, "has")) {
      AxStr *k = ax_to_str(argc ? args[0] : ax_null());
      bool r = ax_dict_has(d, k);
      ax_release(ax_strv(k));
      return ax_bool(r);
    }
    if (name_is(name, "get")) {
      AxStr *k = ax_to_str(argc ? args[0] : ax_null());
      AxValue v;
      bool found = ax_dict_get(d, k, &v);
      ax_release(ax_strv(k));
      if (found) return v;
      return argc > 1 ? ax_copy(args[1]) : ax_null();
    }
    if (name_is(name, "set")) {
      AxStr *ks = ax_to_str(argc ? args[0] : ax_null());
      AxStr *k = ax_intern(ks->data, ks->len);
      ax_dict_set(d, k, argc > 1 ? ax_copy(args[1]) : ax_null());
      ax_release(ax_strv(k));
      ax_release(ax_strv(ks));
      return argc > 1 ? ax_copy(args[1]) : ax_null();
    }
    if (name_is(name, "delete")) {
      AxStr *k = ax_to_str(argc ? args[0] : ax_null());
      ax_dict_del(d, k);
      ax_release(ax_strv(k));
      return ax_null();
    }
    // The higher-order dict methods take a selector over (value, key), as the array ones do.
    if (name_is(name, "map_values") || name_is(name, "filter") || name_is(name, "each") || name_is(name, "forEach")) {
      AxValue sel = argc ? args[0] : ax_null();
      method_selector(vm, sel);
      bool each = name_is(name, "each") || name_is(name, "forEach"), filter = name_is(name, "filter");
      AxDict *out = ax_dict_new();
      for (uint32_t i = 0; i < d->len; i++) {
        if (d->entries[i].dead) continue;
        AxValue r = selector_apply_kv(vm, sel, d->entries[i].val, d->entries[i].key);
        if (filter) { if (ax_truthy(r)) ax_dict_set(out, d->entries[i].key, ax_copy(d->entries[i].val)); ax_release(r); }
        else if (each) ax_release(r);
        else ax_dict_set(out, d->entries[i].key, r);
      }
      if (each) { ax_release(ax_dictv(out)); return ax_null(); }
      return ax_dictv(out);
    }
  }

  // Uniform call syntax: with no method of that name, `x.f(a, b)` is `f(x, a, b)` for any
  // declared ^fn/^proc or library function (both live in the builtins frame).
  AxValue fn;
  if (ax_scope_lookup_local(vm->builtins, name, &fn)) {
    if (fn.t == AX_FN) {
      AxValue stack[9];
      AxValue *argv = argc + 1 <= 9 ? stack : malloc(sizeof(AxValue) * (size_t)(argc + 1));
      argv[0] = obj;
      for (int i = 0; i < argc; i++) argv[i + 1] = args[i];
      AxValue r = ax_call(vm, fn, argv, argc + 1);
      if (argv != stack) free(argv);
      ax_release(fn);
      return r;
    }
    ax_release(fn);
  }
  ax_throw(vm, "AX-RUNTIME-METHOD", "no method '.%s(...)' on %s", name->data, ax_type_name(obj));
  return ax_null();
}

// ---------------------------------------------------------------------------------------------
// Installation
// ---------------------------------------------------------------------------------------------

static void def(AxVM *vm, const char *name, AxNativeFn fn, int min_args, int max_args) {
  AxStr *key = ax_internz(name);
  ax_scope_declare(vm->builtins, key, ax_native(name, fn, min_args, max_args));
  ax_release(ax_strv(key));
}

void ax_stdlib_install(AxVM *vm) {
  def(vm, "print", n_print, 0, -1);
  def(vm, "eprint", n_eprint, 0, -1);
  def(vm, "len", n_len, 1, 1);
  def(vm, "range", n_range, 1, 3);
  def(vm, "str", n_str, 1, 1);
  def(vm, "type", n_type, 1, 1);
  def(vm, "num", n_num, 1, 1);
  def(vm, "int", n_int, 1, 1);
  def(vm, "float", n_float, 1, 1);
  def(vm, "parse_int", n_parse_int, 1, 2);

  def(vm, "is_null", n_is_null, 1, 1);
  def(vm, "is_number", n_is_number, 1, 1);
  def(vm, "is_string", n_is_string, 1, 1);
  def(vm, "is_array", n_is_array, 1, 1);
  def(vm, "is_dict", n_is_dict, 1, 1);
  def(vm, "is_bool", n_is_bool, 1, 1);
  def(vm, "is_fn", n_is_fn, 1, 1);
  def(vm, "is_int", n_is_int, 1, 1);
  def(vm, "is_nan", n_is_nan, 1, 1);
  def(vm, "is_finite", n_is_finite, 1, 1);
  def(vm, "is_empty", n_is_empty, 1, 1);

  def(vm, "abs", n_abs, 1, 1);      def(vm, "floor", n_floor, 1, 1);
  def(vm, "ceil", n_ceil, 1, 1);    def(vm, "round", n_round, 1, 1);
  def(vm, "sqrt", n_sqrt, 1, 1);    def(vm, "cbrt", n_cbrt, 1, 1);
  def(vm, "sin", n_sin, 1, 1);      def(vm, "cos", n_cos, 1, 1);
  def(vm, "tan", n_tan, 1, 1);      def(vm, "asin", n_asin, 1, 1);
  def(vm, "acos", n_acos, 1, 1);    def(vm, "atan", n_atan, 1, 1);
  def(vm, "sinh", n_sinh, 1, 1);    def(vm, "cosh", n_cosh, 1, 1);
  def(vm, "tanh", n_tanh, 1, 1);    def(vm, "exp", n_exp, 1, 1);
  def(vm, "log", n_log, 1, 1);      def(vm, "ln", n_log, 1, 1);
  def(vm, "log2", n_log2, 1, 1);    def(vm, "log10", n_log10, 1, 1);
  def(vm, "log1p", n_log1p, 1, 1);  def(vm, "trunc", n_trunc, 1, 1);
  def(vm, "sign", n_sign, 1, 1);    def(vm, "fract", n_fract, 1, 1);
  def(vm, "clamp01", n_clamp01, 1, 1);
  def(vm, "deg2rad", n_deg2rad, 1, 1);
  def(vm, "rad2deg", n_rad2deg, 1, 1);
  def(vm, "isqrt", n_isqrt, 1, 1);
  def(vm, "pow", n_pow, 2, 2);      def(vm, "atan2", n_atan2, 2, 2);
  def(vm, "hypot", n_hypot, 2, 2);
  def(vm, "PI", n_pi, 0, 0);        def(vm, "TAU", n_tau, 0, 0);
  def(vm, "E", n_e, 0, 0);          def(vm, "inf", n_inf, 0, 0);
  def(vm, "nan", n_nan, 0, 0);
  def(vm, "min", n_min, 1, -1);     def(vm, "max", n_max, 1, -1);
  def(vm, "clamp", n_clamp, 3, 3);  def(vm, "lerp", n_lerp, 3, 3);
  def(vm, "inv_lerp", n_inv_lerp, 3, 3);
  def(vm, "map_range", n_map_range, 5, 5);
  def(vm, "smoothstep", n_smoothstep, 3, 3);
  def(vm, "wrap", n_wrap, 3, 3);
  def(vm, "mod", n_mod, 2, 2);      def(vm, "divmod", n_divmod, 2, 2);
  def(vm, "gcd", n_gcd, 2, 2);      def(vm, "lcm", n_lcm, 2, 2);
  def(vm, "fact", n_fact, 1, 1);    def(vm, "comb", n_comb, 2, 2);
  def(vm, "perm", n_perm, 2, 2);    def(vm, "is_prime", n_is_prime, 1, 1);
  def(vm, "primes", n_primes, 1, 1);
  def(vm, "round_to", n_round_to, 2, 2);
  def(vm, "to_fixed", n_to_fixed, 1, 2);
  def(vm, "to_hex", n_to_hex, 1, 1);
  def(vm, "to_bin", n_to_bin, 1, 1);
  def(vm, "to_base", n_to_base, 2, 2);
  def(vm, "band", n_band, 2, 2);    def(vm, "bor", n_bor, 2, 2);
  def(vm, "bxor", n_bxor, 2, 2);    def(vm, "bnot", n_bnot, 1, 1);
  def(vm, "shl", n_shl, 2, 2);      def(vm, "shr", n_shr, 2, 2);

  def(vm, "sum", n_sum, 1, 2);      def(vm, "prod", n_prod, 1, 2);
  def(vm, "mean", n_mean, 1, 2);    def(vm, "median", n_median, 1, 1);
  def(vm, "variance", n_variance_impl, 1, 1);
  def(vm, "stdev", n_stdev, 1, 1);  def(vm, "mode", n_mode, 1, 1);

  def(vm, "sorted", n_sorted, 1, 2);      def(vm, "sort_by", n_sort_by, 2, 2);
  def(vm, "sort", n_sorted, 1, 2);        // the name a program reaches for first; a copy, as sorted
  def(vm, "reversed", n_reversed, 1, 1);  def(vm, "take", n_take, 2, 2);
  def(vm, "drop", n_drop, 2, 2);          def(vm, "first", n_first, 1, 2);
  def(vm, "last", n_last, 1, 2);          def(vm, "map", n_map, 2, 2);
  def(vm, "filter", n_filter, 2, 2);      def(vm, "reduce", n_reduce, 2, 3);
  def(vm, "each", n_each, 2, 2);          def(vm, "find", n_find, 2, 2);
  def(vm, "find_index", n_find_index, 2, 2);
  def(vm, "any", n_any, 1, 2);            def(vm, "all", n_all, 1, 2);
  def(vm, "count", n_count, 1, 2);        def(vm, "uniq", n_uniq, 1, 2);
  def(vm, "zip", n_zip, 1, -1);           def(vm, "enumerate", n_enumerate, 1, 1);
  def(vm, "unzip", n_unzip, 1, 1);
  def(vm, "chunk", n_chunk, 2, 2);        def(vm, "windows", n_windows, 2, 2);
  def(vm, "flatten", n_flatten, 1, 2);    def(vm, "partition", n_partition, 2, 2);
  def(vm, "group_by", n_group_by, 2, 2);  def(vm, "count_by", n_count_by, 2, 2);
  def(vm, "min_by", n_min_by, 2, 2);      def(vm, "max_by", n_max_by, 2, 2);
  def(vm, "union", n_union, 2, 2);        def(vm, "intersect", n_intersect, 2, 2);
  def(vm, "difference", n_difference, 2, 2);
  def(vm, "grid", n_grid, 2, 3);          def(vm, "transpose", n_transpose, 1, 1);

  def(vm, "keys", n_keys, 1, 1);          def(vm, "values", n_values, 1, 1);
  def(vm, "items", n_items, 1, 1);        def(vm, "dict", n_dict, 1, 1);
  def(vm, "merge", n_merge, 1, -1);       def(vm, "has_key", n_has_key, 2, 2);
  def(vm, "pick_keys", n_pick_keys, 2, 2);
  def(vm, "omit_keys", n_omit_keys, 2, 2);
  def(vm, "invert", n_invert, 1, 1);      def(vm, "clone", n_clone, 1, 1);
  def(vm, "deep_eq", n_deep_eq, 2, 2);

  def(vm, "ord", n_ord, 1, 1);            def(vm, "chr", n_chr, 1, 1);
  def(vm, "lines", n_lines, 1, 1);        def(vm, "words", n_words, 1, 1);
  def(vm, "chars", n_chars, 1, 1);        def(vm, "capitalize", n_capitalize, 1, 1);
  def(vm, "title", n_title, 1, 1);        def(vm, "reverse_str", n_reverse_str, 1, 1);
  def(vm, "hash", n_hash, 1, 1);
  def(vm, "b64_encode", n_b64_encode, 1, 1);
  def(vm, "b64_decode", n_b64_decode, 1, 1);
  def(vm, "big", n_big, 1, 1);            def(vm, "is_big", n_is_big, 1, 1);
  def(vm, "to_json", n_to_json, 1, 2);    def(vm, "from_json", n_from_json, 1, 1);
  def(vm, "json_stringify", n_to_json, 1, 2);
  def(vm, "json_parse", n_from_json, 1, 1);

  def(vm, "re_test", n_re_test, 2, 3);    def(vm, "re_match", n_re_match, 2, 3);
  def(vm, "re_all", n_re_all, 2, 3);      def(vm, "re_sub", n_re_sub, 3, 4);
  def(vm, "re_split", n_re_split, 2, 3);

  def(vm, "seed", n_seed, 1, 1);          def(vm, "random", n_random, 0, 0);
  def(vm, "random_range", n_random_range, 2, 2);
  def(vm, "randomRange", n_random_range, 2, 2);
  def(vm, "random_int", n_random_int, 2, 2);
  def(vm, "randomInt", n_random_int, 2, 2);
  def(vm, "pick", n_pick, 1, 1);          def(vm, "shuffle", n_shuffle, 1, 1);
  def(vm, "gauss", n_gauss, 0, 2);        def(vm, "uuid", n_uuid, 0, 0);

  def(vm, "now", n_now, 0, 0);            def(vm, "time", n_time, 0, 0);
  def(vm, "clock", n_clock, 0, 0);        def(vm, "date_iso", n_date_iso, 0, 1);
  def(vm, "sleep", n_sleep, 1, 1);

  def(vm, "read", n_read, 1, 2);          def(vm, "read_lines", n_read_lines, 1, 1);
  def(vm, "read_json", n_read_json, 1, 2);
  def(vm, "write", n_write, 2, 2);        def(vm, "write_json", n_write_json, 2, 3);
  def(vm, "append", n_append, 2, 2);      def(vm, "file_exists", n_file_exists, 1, 1);
  def(vm, "exists", n_file_exists, 1, 1);
  def(vm, "is_dir", n_is_dir, 1, 1);      def(vm, "ls", n_ls, 0, 1);
  def(vm, "mkdir", n_mkdir, 1, 1);        def(vm, "rm", n_rm, 1, 1);
  def(vm, "path_join", n_path_join, 1, -1);
  def(vm, "input", n_input, 0, 1);        def(vm, "read_stdin", n_read_stdin, 0, 0);
  def(vm, "args", n_args, 0, 0);          def(vm, "env", n_env, 1, 2);
  def(vm, "exit", n_exit, 0, 1);          def(vm, "sh", n_sh, 1, 1);

  def(vm, "apply", n_apply, 2, 2);        def(vm, "partial", n_partial, 1, -1);
  def(vm, "compose", n_compose, 1, -1);   def(vm, "memo", n_memo, 1, 1);
  def(vm, "check", n_check, 1, 2);        def(vm, "check_eq", n_check_eq, 2, 3);
  // Engine intrinsics last: where a name is in both (hypot), the engine's V8-exact version wins.
  ax_engine_install(vm);
}
