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
#define NUM(i) (i < argc ? ax_to_num(args[i]) : 0)
#define DEF_NUM(i, d) (i < argc && args[i].t != AX_NULL ? ax_to_num(args[i]) : (d))

static AxStr *arg_str(AxValue v) { return ax_to_str(v); }   // +1

static AxValue str_take(AxStr *s) { return ax_strv(s); }

static AxValue num_or_null(double d, bool ok) { return ok ? ax_num(d) : ax_null(); }

static int cmp_values_asc(const void *a, const void *b) {
  return ax_compare(*(const AxValue *)a, *(const AxValue *)b);
}

// Sorting with a key or comparator needs the VM, which qsort cannot carry, so the sort is a
// simple merge sort over the array instead. Stable, which also matches the reference.
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

static void merge_sort(SortCtx *c, AxValue *items, uint32_t n, AxValue *tmp) {
  if (n < 2) return;
  uint32_t mid = n / 2;
  merge_sort(c, items, mid, tmp);
  merge_sort(c, items + mid, n - mid, tmp);
  uint32_t i = 0, j = mid, k = 0;
  while (i < mid && j < n) tmp[k++] = (sort_cmp(c, items[j], items[i]) < 0) ? items[j++] : items[i++];
  while (i < mid) tmp[k++] = items[i++];
  while (j < n) tmp[k++] = items[j++];
  memcpy(items, tmp, sizeof(AxValue) * n);
}

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
    AxValue *tmp = malloc(sizeof(AxValue) * out->len);
    merge_sort(&c, out->items, out->len, tmp);
    free(tmp);
  }
  return out;
}

// ---------------------------------------------------------------------------------------------
// JSON
// ---------------------------------------------------------------------------------------------

typedef struct { char *buf; size_t len, cap; } SB;

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

static void json_write(SB *sb, AxValue v, int indent, int depth) {
  char pad[64];
  switch (v.t) {
    case AX_NULL: sb_addz(sb, "null"); return;
    case AX_BOOL: sb_addz(sb, v.b ? "true" : "false"); return;
    case AX_NUM: {
      AxStr *s = ax_to_str(v);
      if (isnan(v.num) || isinf(v.num)) sb_addz(sb, "null");
      else sb_add(sb, s->data, s->len);
      ax_release(ax_strv(s));
      return;
    }
    case AX_STR: case AX_ATOM: json_quote(sb, (AxStr *)v.o); return;
    case AX_ARR: {
      AxArr *a = (AxArr *)v.o;
      if (!a->len) { sb_addz(sb, "[]"); return; }
      sb_addz(sb, "[");
      for (uint32_t i = 0; i < a->len; i++) {
        if (i) sb_addz(sb, ",");
        if (indent) { sb_addz(sb, "\n"); snprintf(pad, sizeof pad, "%*s", indent * (depth + 1), ""); sb_addz(sb, pad); }
        json_write(sb, a->items[i], indent, depth + 1);
      }
      if (indent) { sb_addz(sb, "\n"); snprintf(pad, sizeof pad, "%*s", indent * depth, ""); sb_addz(sb, pad); }
      sb_addz(sb, "]");
      return;
    }
    case AX_DICT: {
      AxDict *d = (AxDict *)v.o;
      if (!d->live && !d->type_tag) { sb_addz(sb, "{}"); return; }
      sb_addz(sb, "{");
      bool first = true;
      if (d->type_tag) {
        // A record writes its type first, as JSON.stringify does with the reference's `__type`.
        if (indent) { sb_addz(sb, "\n"); snprintf(pad, sizeof pad, "%*s", indent * (depth + 1), ""); sb_addz(sb, pad); }
        sb_addz(sb, indent ? "\"__type\": " : "\"__type\":");
        json_quote(sb, d->type_tag);
        first = false;
      }
      for (uint32_t i = 0; i < d->len; i++) {
        if (d->entries[i].dead) continue;
        if (!first) sb_addz(sb, ",");
        first = false;
        if (indent) { sb_addz(sb, "\n"); snprintf(pad, sizeof pad, "%*s", indent * (depth + 1), ""); sb_addz(sb, pad); }
        json_quote(sb, d->entries[i].key);
        sb_addz(sb, indent ? ": " : ":");
        json_write(sb, d->entries[i].val, indent, depth + 1);
      }
      if (indent) { sb_addz(sb, "\n"); snprintf(pad, sizeof pad, "%*s", indent * depth, ""); sb_addz(sb, pad); }
      sb_addz(sb, "}");
      return;
    }
    default: sb_addz(sb, "null"); return;
  }
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
    if (c == '[' && (i == 0 || pat[i - 1] != '\\')) in_class = true;
    else if (c == ']' && in_class) in_class = false;
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
  if (!realpath(path, real)) snprintf(real, sizeof real, "%s", path);
  for (uint32_t i = 0; i < list->len; i++) {
    AxStr *p = (AxStr *)list->items[i].o;
    char allowed[4096];
    if (!realpath(p->data, allowed)) snprintf(allowed, sizeof allowed, "%s", p->data);
    size_t al = strlen(allowed);
    if (strncmp(real, allowed, al) == 0 && (real[al] == '\0' || real[al] == '/')) return true;
  }
  return false;
}

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
  if (!ax_engine_log_msg(vm, msg)) { fwrite(msg->data, 1, msg->len, stdout); fputc('\n', stdout); }
  ax_release(ax_strv(msg));
  return ax_null();
}

NATIVE(n_eprint) {
  for (int i = 0; i < argc; i++) {
    if (i) fputc(' ', stderr);
    AxStr *s = ax_to_str(args[i]);
    fwrite(s->data, 1, s->len, stderr);
    ax_release(ax_strv(s));
  }
  fputc('\n', stderr);
  return ax_null();
}

NATIVE(n_len) {
  AxValue v = ARG(0);
  switch (v.t) {
    case AX_STR: case AX_ATOM: return ax_num(((AxStr *)v.o)->len);
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

NATIVE(n_range) {
  if (argc == 1) return ax_range(0, NUM(0), 1);
  if (argc >= 3) return ax_range(NUM(0), NUM(1), NUM(2));
  return ax_range(NUM(0), NUM(1), 1);
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
NATIVE(n_int) {
  AxStr *s = arg_str(ARG(0));
  char *end = NULL;
  long v = strtol(s->data, &end, 10);
  bool ok = end != s->data;
  ax_release(ax_strv(s));
  return num_or_null((double)v, ok);
}
NATIVE(n_float) {
  AxStr *s = arg_str(ARG(0));
  char *end = NULL;
  double v = strtod(s->data, &end);
  bool ok = end != s->data;
  ax_release(ax_strv(s));
  return num_or_null(v, ok);
}
NATIVE(n_parse_int) {
  AxStr *s = arg_str(ARG(0));
  int base = argc > 1 ? (int)NUM(1) : 10;
  char *end = NULL;
  long v = strtol(s->data, &end, base);
  bool ok = end != s->data;
  ax_release(ax_strv(s));
  return num_or_null((double)v, ok);
}

NATIVE(n_is_null) { return ax_bool(ARG(0).t == AX_NULL); }
NATIVE(n_is_number) { return ax_bool(ARG(0).t == AX_NUM); }
NATIVE(n_is_string) { return ax_bool(ARG(0).t == AX_STR); }
NATIVE(n_is_array) { return ax_bool(ARG(0).t == AX_ARR); }
NATIVE(n_is_dict) { return ax_bool(ARG(0).t == AX_DICT); }
NATIVE(n_is_bool) { return ax_bool(ARG(0).t == AX_BOOL); }
NATIVE(n_is_fn) { return ax_bool(ARG(0).t == AX_FN); }
NATIVE(n_is_int) { double d = NUM(0); return ax_bool(ARG(0).t == AX_NUM && d == floor(d) && isfinite(d)); }
NATIVE(n_is_nan) { return ax_bool(isnan(NUM(0))); }
NATIVE(n_is_finite) { return ax_bool(isfinite(NUM(0))); }
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
MATH1(n_sign, (x > 0) - (x < 0))
MATH1(n_fract, x - floor(x))
MATH1(n_clamp01, x < 0 ? 0 : (x > 1 ? 1 : x))
MATH1(n_deg2rad, x * M_PI / 180.0)
MATH1(n_rad2deg, x * 180.0 / M_PI)
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

NATIVE(n_min) {
  if (argc == 1 && args[0].t == AX_ARR) {
    AxArr *a = (AxArr *)args[0].o;
    if (!a->len) return ax_null();
    double best = ax_to_num(a->items[0]);
    for (uint32_t i = 1; i < a->len; i++) { double d = ax_to_num(a->items[i]); if (d < best) best = d; }
    return ax_num(best);
  }
  double best = INFINITY;
  for (int i = 0; i < argc; i++) { double d = ax_to_num(args[i]); if (d < best) best = d; }
  return ax_num(best);
}
NATIVE(n_max) {
  if (argc == 1 && args[0].t == AX_ARR) {
    AxArr *a = (AxArr *)args[0].o;
    if (!a->len) return ax_null();
    double best = ax_to_num(a->items[0]);
    for (uint32_t i = 1; i < a->len; i++) { double d = ax_to_num(a->items[i]); if (d > best) best = d; }
    return ax_num(best);
  }
  double best = -INFINITY;
  for (int i = 0; i < argc; i++) { double d = ax_to_num(args[i]); if (d > best) best = d; }
  return ax_num(best);
}
NATIVE(n_clamp) { double x = NUM(0), lo = NUM(1), hi = NUM(2); return ax_num(x < lo ? lo : (x > hi ? hi : x)); }
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
NATIVE(n_mod) { double a = NUM(0), b = NUM(1); if (b == 0) return ax_num(0); return ax_num(fmod(fmod(a, b) + b, b)); }
NATIVE(n_divmod) {
  double a = NUM(0), b = NUM(1);
  AxArr *out = ax_arr_new(2);
  if (b == 0) { ax_arr_push(out, ax_num(0)); ax_arr_push(out, ax_num(0)); return ax_arrv(out); }
  ax_arr_push(out, ax_num(floor(a / b)));
  ax_arr_push(out, ax_num(fmod(fmod(a, b) + b, b)));
  return ax_arrv(out);
}
NATIVE(n_gcd) {
  long a = (long)fabs(NUM(0)), b = (long)fabs(NUM(1));
  while (b) { long t = b; b = a % b; a = t; }
  return ax_num((double)a);
}
NATIVE(n_lcm) {
  long a = (long)fabs(NUM(0)), b = (long)fabs(NUM(1));
  if (!a || !b) return ax_num(0);
  long x = a, y = b;
  while (y) { long t = y; y = x % y; x = t; }
  return ax_num((double)((a / x) * b));
}
NATIVE(n_fact) {
  long n = (long)floor(NUM(0));
  if (n < 0) return ax_num(0);
  double r = 1;
  for (long i = 2; i <= n; i++) r *= (double)i;
  return ax_num(r);
}
NATIVE(n_comb) {
  long n = (long)floor(NUM(0)), k = (long)floor(NUM(1));
  if (k < 0 || k > n) return ax_num(0);
  if (k > n - k) k = n - k;
  double r = 1;
  for (long i = 0; i < k; i++) r = r * (double)(n - i) / (double)(i + 1);
  return ax_num(floor(r + 0.5));
}
NATIVE(n_perm) {
  long n = (long)floor(NUM(0)), k = (long)floor(NUM(1));
  if (k < 0 || k > n) return ax_num(0);
  double r = 1;
  for (long i = 0; i < k; i++) r *= (double)(n - i);
  return ax_num(r);
}
NATIVE(n_is_prime) {
  long n = (long)floor(NUM(0));
  if (n < 2) return ax_bool(false);
  if (n % 2 == 0) return ax_bool(n == 2);
  for (long i = 3; i * i <= n; i += 2) if (n % i == 0) return ax_bool(false);
  return ax_bool(true);
}
NATIVE(n_primes) {
  long limit = (long)floor(NUM(0));
  AxArr *out = ax_arr_new(16);
  if (limit < 2) return ax_arrv(out);
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
  double x = NUM(0), step = NUM(1);
  if (step == 0) return ax_num(x);
  return ax_num(floor(x / step + 0.5) * step);
}
NATIVE(n_to_fixed) {
  double x = NUM(0);
  int digits = argc > 1 ? (int)NUM(1) : 2;
  char buf[64];
  snprintf(buf, sizeof buf, "%.*f", digits, x);
  return ax_str_from(buf);
}
NATIVE(n_to_hex) { char b[32]; snprintf(b, sizeof b, "0x%x", (unsigned)(long)NUM(0)); return ax_str_from(b); }
NATIVE(n_to_bin) {
  unsigned v = (unsigned)(long)NUM(0);
  char b[40];
  int j = 0;
  b[j++] = '0'; b[j++] = 'b';
  if (!v) b[j++] = '0';
  else { int started = 0; for (int i = 31; i >= 0; i--) { int bit = (v >> i) & 1; if (bit) started = 1; if (started) b[j++] = (char)('0' + bit); } }
  b[j] = '\0';
  return ax_str_from(b);
}
NATIVE(n_to_base) {
  long v = (long)trunc(NUM(0));
  int base = (int)NUM(1);
  if (base < 2) base = 2;
  if (base > 36) base = 36;
  char b[80];
  int j = 0;
  bool neg = v < 0;
  unsigned long uv = neg ? (unsigned long)(-v) : (unsigned long)v;
  if (!uv) b[j++] = '0';
  while (uv) { int d = (int)(uv % base); b[j++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); uv /= base; }
  if (neg) b[j++] = '-';
  b[j] = '\0';
  for (int i = 0, k = j - 1; i < k; i++, k--) { char t = b[i]; b[i] = b[k]; b[k] = t; }
  return ax_str_from(b);
}
NATIVE(n_band) { return ax_num((double)(((long)NUM(0)) & ((long)NUM(1)))); }
NATIVE(n_bor) { return ax_num((double)(((long)NUM(0)) | ((long)NUM(1)))); }
NATIVE(n_bxor) { return ax_num((double)(((long)NUM(0)) ^ ((long)NUM(1)))); }
NATIVE(n_bnot) { return ax_num((double)(~(long)NUM(0))); }
NATIVE(n_shl) { return ax_num((double)(((long)NUM(0)) << ((long)NUM(1)))); }
NATIVE(n_shr) { return ax_num((double)(((long)NUM(0)) >> ((long)NUM(1)))); }

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
    t *= ax_to_num(k);
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
    t += ax_to_num(k);
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
  int64_t n = (int64_t)NUM(1);
  AxArr *out = ax_arr_new(n > 0 ? (uint32_t)n : 0);
  for (int64_t i = 0; i < n && i < (int64_t)a->len; i++) ax_arr_push(out, ax_copy(a->items[i]));
  ax_release(ax_arrv(a));
  return ax_arrv(out);
}
NATIVE(n_drop) {
  AxArr *a = ax_to_seq(vm, ARG(0));
  int64_t n = (int64_t)NUM(1);
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
  AxArr *a = ax_to_seq(vm, ARG(0));
  for (uint32_t i = 0; i < a->len; i++) {
    AxValue r = ax_key_apply(vm, ARG(1), a->items[i], i);
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
  if (argc < 2) n = a->len;
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
NATIVE(n_uniq) {
  AxArr *a = ax_to_seq(vm, ARG(0));
  AxArr *out = ax_arr_new(a->len);
  AxArr *keys = ax_arr_new(a->len);
  for (uint32_t i = 0; i < a->len; i++) {
    AxValue k = ax_key_apply(vm, ARG(1), a->items[i], i);
    bool seen = false;
    for (uint32_t j = 0; j < keys->len && !seen; j++) seen = ax_equals(keys->items[j], k);
    if (!seen) { ax_arr_push(keys, ax_copy(k)); ax_arr_push(out, ax_copy(a->items[i])); }
    ax_release(k);
  }
  ax_release(ax_arrv(keys));
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
  int64_t size = (int64_t)NUM(1);
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
  int64_t size = (int64_t)NUM(1);
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
  int depth = argc > 1 ? (int)NUM(1) : -1;
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
  AxArr *a = ax_to_seq(vm, ARG(0)), *b = ax_to_seq(vm, ARG(1));
  AxArr *out = ax_arr_new(a->len + b->len);
  for (uint32_t i = 0; i < a->len; i++) {
    bool seen = false;
    for (uint32_t j = 0; j < out->len && !seen; j++) seen = ax_equals(out->items[j], a->items[i]);
    if (!seen) ax_arr_push(out, ax_copy(a->items[i]));
  }
  for (uint32_t i = 0; i < b->len; i++) {
    bool seen = false;
    for (uint32_t j = 0; j < out->len && !seen; j++) seen = ax_equals(out->items[j], b->items[i]);
    if (!seen) ax_arr_push(out, ax_copy(b->items[i]));
  }
  ax_release(ax_arrv(a));
  ax_release(ax_arrv(b));
  return ax_arrv(out);
}
NATIVE(n_intersect) {
  AxArr *a = ax_to_seq(vm, ARG(0)), *b = ax_to_seq(vm, ARG(1));
  AxArr *out = ax_arr_new(4);
  for (uint32_t i = 0; i < a->len; i++) {
    bool inb = false;
    for (uint32_t j = 0; j < b->len && !inb; j++) inb = ax_equals(a->items[i], b->items[j]);
    bool dup = false;
    for (uint32_t j = 0; j < out->len && !dup; j++) dup = ax_equals(out->items[j], a->items[i]);
    if (inb && !dup) ax_arr_push(out, ax_copy(a->items[i]));
  }
  ax_release(ax_arrv(a));
  ax_release(ax_arrv(b));
  return ax_arrv(out);
}
NATIVE(n_difference) {
  AxArr *a = ax_to_seq(vm, ARG(0)), *b = ax_to_seq(vm, ARG(1));
  AxArr *out = ax_arr_new(4);
  for (uint32_t i = 0; i < a->len; i++) {
    bool inb = false;
    for (uint32_t j = 0; j < b->len && !inb; j++) inb = ax_equals(a->items[i], b->items[j]);
    if (!inb) ax_arr_push(out, ax_copy(a->items[i]));
  }
  ax_release(ax_arrv(a));
  ax_release(ax_arrv(b));
  return ax_arrv(out);
}
NATIVE(n_grid) {
  int64_t rows = (int64_t)NUM(0), cols = (int64_t)NUM(1);
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
  AxArr *m = ax_to_seq(vm, ARG(0));
  uint32_t cols = 0;
  for (uint32_t i = 0; i < m->len; i++) if (m->items[i].t == AX_ARR && ((AxArr *)m->items[i].o)->len > cols) cols = ((AxArr *)m->items[i].o)->len;
  AxArr *out = ax_arr_new(cols);
  for (uint32_t c = 0; c < cols; c++) {
    AxArr *row = ax_arr_new(m->len);
    for (uint32_t r = 0; r < m->len; r++) {
      AxValue cell = ax_null();
      if (m->items[r].t == AX_ARR) cell = ax_arr_get((AxArr *)m->items[r].o, c);
      ax_arr_push(row, cell);
    }
    ax_arr_push(out, ax_arrv(row));
  }
  ax_release(ax_arrv(m));
  return ax_arrv(out);
}

// --- dicts ------------------------------------------------------------------------------------

NATIVE(n_keys) {
  AxValue v = ARG(0);
  AxArr *out = ax_arr_new(4);
  if (v.t == AX_DICT) {
    AxDict *d = (AxDict *)v.o;
    for (uint32_t i = 0; i < d->len; i++) {
      if (d->entries[i].dead) continue;
      ax_retain(ax_strv(d->entries[i].key));
      ax_arr_push(out, ax_strv(d->entries[i].key));
    }
  }
  return ax_arrv(out);
}
NATIVE(n_values) {
  AxValue v = ARG(0);
  AxArr *out = ax_arr_new(4);
  if (v.t == AX_DICT) {
    AxDict *d = (AxDict *)v.o;
    for (uint32_t i = 0; i < d->len; i++) {
      if (d->entries[i].dead) continue;
      ax_arr_push(out, ax_copy(d->entries[i].val));
    }
  }
  return ax_arrv(out);
}
NATIVE(n_items) {
  AxValue v = ARG(0);
  AxArr *out = ax_arr_new(4);
  if (v.t == AX_DICT) {
    AxDict *d = (AxDict *)v.o;
    for (uint32_t i = 0; i < d->len; i++) {
      if (d->entries[i].dead) continue;
      AxArr *pair = ax_arr_new(2);
      ax_retain(ax_strv(d->entries[i].key));
      ax_arr_push(pair, ax_strv(d->entries[i].key));
      ax_arr_push(pair, ax_copy(d->entries[i].val));
      ax_arr_push(out, ax_arrv(pair));
    }
  }
  return ax_arrv(out);
}
NATIVE(n_dict) {
  AxArr *pairs = ax_to_seq(vm, ARG(0));
  AxDict *d = ax_dict_new();
  for (uint32_t i = 0; i < pairs->len; i++) {
    AxArr *p = ax_to_seq(vm, pairs->items[i]);
    if (p->len >= 1) {
      AxStr *ks = ax_to_str(p->items[0]);
      AxStr *key = ax_intern(ks->data, ks->len);
      ax_dict_set(d, key, p->len > 1 ? ax_copy(p->items[1]) : ax_null());
      ax_release(ax_strv(key));
      ax_release(ax_strv(ks));
    }
    ax_release(ax_arrv(p));
  }
  ax_release(ax_arrv(pairs));
  return ax_dictv(d);
}
NATIVE(n_merge) {
  AxDict *out = ax_dict_new();
  for (int i = 0; i < argc; i++) {
    if (args[i].t != AX_DICT) continue;
    AxDict *d = (AxDict *)args[i].o;
    for (uint32_t e = 0; e < d->len; e++) {
      if (d->entries[e].dead) continue;
      ax_dict_set(out, d->entries[e].key, ax_copy(d->entries[e].val));
    }
  }
  return ax_dictv(out);
}
NATIVE(n_has_key) {
  if (ARG(0).t != AX_DICT) return ax_bool(false);
  AxStr *k = ax_to_str(ARG(1));
  bool has = ax_dict_has((AxDict *)args[0].o, k);
  ax_release(ax_strv(k));
  return ax_bool(has);
}
NATIVE(n_pick_keys) {
  AxDict *out = ax_dict_new();
  if (ARG(0).t == AX_DICT) {
    AxArr *ks = ax_to_seq(vm, ARG(1));
    for (uint32_t i = 0; i < ks->len; i++) {
      AxStr *k = ax_to_str(ks->items[i]);
      AxStr *key = ax_intern(k->data, k->len);
      AxValue v;
      if (ax_dict_get((AxDict *)args[0].o, key, &v)) ax_dict_set(out, key, v);
      ax_release(ax_strv(key));
      ax_release(ax_strv(k));
    }
    ax_release(ax_arrv(ks));
  }
  return ax_dictv(out);
}
NATIVE(n_omit_keys) {
  AxDict *out = ax_dict_new();
  if (ARG(0).t == AX_DICT) {
    AxDict *d = (AxDict *)args[0].o;
    AxArr *ks = ax_to_seq(vm, ARG(1));
    for (uint32_t e = 0; e < d->len; e++) {
      if (d->entries[e].dead) continue;
      bool drop = false;
      for (uint32_t i = 0; i < ks->len && !drop; i++) {
        AxStr *k = ax_to_str(ks->items[i]);
        drop = ax_str_eq(k, d->entries[e].key);
        ax_release(ax_strv(k));
      }
      if (!drop) ax_dict_set(out, d->entries[e].key, ax_copy(d->entries[e].val));
    }
    ax_release(ax_arrv(ks));
  }
  return ax_dictv(out);
}
NATIVE(n_invert) {
  AxDict *out = ax_dict_new();
  if (ARG(0).t == AX_DICT) {
    AxDict *d = (AxDict *)args[0].o;
    for (uint32_t e = 0; e < d->len; e++) {
      if (d->entries[e].dead) continue;
      AxStr *vs = ax_to_str(d->entries[e].val);
      AxStr *key = ax_intern(vs->data, vs->len);
      ax_retain(ax_strv(d->entries[e].key));
      ax_dict_set(out, key, ax_strv(d->entries[e].key));
      ax_release(ax_strv(key));
      ax_release(ax_strv(vs));
    }
  }
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
NATIVE(n_deep_eq) { return ax_bool(ax_equals(ARG(0), ARG(1))); }

// --- strings ----------------------------------------------------------------------------------

NATIVE(n_ord) {
  AxStr *s = arg_str(ARG(0));
  double v = s->len ? (unsigned char)s->data[0] : 0;
  ax_release(ax_strv(s));
  return ax_num(v);
}
NATIVE(n_chr) {
  char c = (char)(int)NUM(0);
  return ax_strv(ax_str_new(&c, 1));
}
NATIVE(n_lines) {
  AxStr *s = arg_str(ARG(0));
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
  AxStr *s = arg_str(ARG(0));
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
  AxStr *s = arg_str(ARG(0));
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
  AxStr *out = ax_str_new(s->data, s->len);
  bool boundary = true;
  for (uint32_t i = 0; i < out->len; i++) {
    if (isspace((unsigned char)out->data[i])) { boundary = true; continue; }
    out->data[i] = (char)(boundary ? toupper((unsigned char)out->data[i]) : tolower((unsigned char)out->data[i]));
    boundary = false;
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
NATIVE(n_b64_decode) {
  AxStr *s = arg_str(ARG(0));
  int rev[256];
  for (int i = 0; i < 256; i++) rev[i] = -1;
  for (int i = 0; i < 64; i++) rev[(unsigned char)B64[i]] = i;
  char *out = malloc(s->len + 1);
  size_t j = 0;
  unsigned buf = 0;
  int bits = 0;
  for (uint32_t i = 0; i < s->len; i++) {
    int d = rev[(unsigned char)s->data[i]];
    if (d < 0) continue;
    buf = (buf << 6) | (unsigned)d;
    bits += 6;
    if (bits >= 8) { bits -= 8; out[j++] = (char)((buf >> bits) & 0xFF); }
  }
  out[j] = '\0';
  AxValue r = ax_strv(ax_str_new(out, j));
  free(out);
  ax_release(ax_strv(s));
  return r;
}
NATIVE(n_to_json) {
  SB sb = {0};
  sb_add(&sb, "", 0);
  json_write(&sb, ARG(0), (argc > 1 && ax_truthy(args[1])) ? 2 : 0, 0);
  AxValue r = ax_strv(ax_str_new(sb.buf, sb.len));
  free(sb.buf);
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
  bool icase = argc > 2 && ax_truthy(args[2]);
  re_build(vm, ARG(1), &re, icase);
  bool m = regexec(&re, s->data, 0, NULL, 0) == 0;
  regfree(&re);
  ax_release(ax_strv(s));
  return ax_bool(m);
}
NATIVE(n_re_match) {
  AxStr *s = arg_str(ARG(0));
  regex_t re;
  re_build(vm, ARG(1), &re, argc > 2 && ax_truthy(args[2]));
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
  re_build(vm, ARG(1), &re, argc > 2 && ax_truthy(args[2]));
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
  re_build(vm, ARG(1), &re, argc > 3 && ax_truthy(args[3]));
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
  re_build(vm, ARG(1), &re, argc > 2 && ax_truthy(args[2]));
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
    fwrite(p->data, 1, p->len, stdout);
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
  FILE *pipe = popen(cmd->data, "r");
  int status = -1;
  if (pipe) {
    char buf[4096];
    size_t got;
    while ((got = fread(buf, 1, sizeof buf, pipe)) > 0) sb_add(&sb, buf, got);
    status = pclose(pipe);
    if (status != -1) status = WEXITSTATUS(status);
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
      AxStr *from = argc ? ax_to_str(args[0]) : ax_str_newz("");
      AxStr *to = argc > 1 ? ax_to_str(args[1]) : ax_str_newz("");
      SB sb = {0};
      sb_add(&sb, "", 0);
      bool all = name_is(name, "replace_all");
      bool done = false;
      for (uint32_t i = 0; i < s->len; ) {
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
      AxStr *p = argc ? ax_to_str(args[0]) : ax_str_newz("");
      bool r = p->len <= s->len && memcmp(s->data, p->data, p->len) == 0;
      ax_release(ax_strv(p));
      return ax_bool(r);
    }
    if (name_is(name, "endsWith")) {
      AxStr *p = argc ? ax_to_str(args[0]) : ax_str_newz("");
      bool r = p->len <= s->len && memcmp(s->data + s->len - p->len, p->data, p->len) == 0;
      ax_release(ax_strv(p));
      return ax_bool(r);
    }
    if (name_is(name, "includes") || name_is(name, "contains")) {
      AxStr *p = argc ? ax_to_str(args[0]) : ax_str_newz("");
      bool r = p->len == 0 || strstr(s->data, p->data) != NULL;
      ax_release(ax_strv(p));
      return ax_bool(r);
    }
    if (name_is(name, "indexOf")) {
      AxStr *p = argc ? ax_to_str(args[0]) : ax_str_newz("");
      const char *hit = p->len ? strstr(s->data, p->data) : s->data;
      double idx = hit ? (double)(hit - s->data) : -1;
      ax_release(ax_strv(p));
      return ax_num(idx);
    }
    if (name_is(name, "count")) {
      AxStr *p = argc ? ax_to_str(args[0]) : ax_str_newz("");
      double n = 0;
      if (p->len) for (uint32_t i = 0; i + p->len <= s->len; ) { if (memcmp(s->data + i, p->data, p->len) == 0) { n++; i += p->len; } else i++; }
      ax_release(ax_strv(p));
      return ax_num(n);
    }
    if (name_is(name, "repeat")) {
      int64_t n = argc ? (int64_t)ax_to_num(args[0]) : 0;
      if (n < 0) n = 0;
      SB sb = {0};
      sb_add(&sb, "", 0);
      for (int64_t i = 0; i < n; i++) sb_add(&sb, s->data, s->len);
      AxValue r = ax_strv(ax_str_new(sb.buf, sb.len));
      free(sb.buf);
      return r;
    }
    if (name_is(name, "slice")) {
      int64_t a = argc ? (int64_t)ax_to_num(args[0]) : 0;
      int64_t b = argc > 1 && args[1].t != AX_NULL ? (int64_t)ax_to_num(args[1]) : (int64_t)s->len;
      if (a < 0) a += s->len;
      if (b < 0) b += s->len;
      if (a < 0) a = 0;
      if (b > (int64_t)s->len) b = s->len;
      if (b < a) b = a;
      return ax_strv(ax_str_new(s->data + a, (size_t)(b - a)));
    }
    if (name_is(name, "padStart") || name_is(name, "padEnd")) {
      int64_t want = argc ? (int64_t)ax_to_num(args[0]) : 0;
      AxStr *padc = argc > 1 ? ax_to_str(args[1]) : ax_str_newz(" ");
      SB sb = {0};
      sb_add(&sb, "", 0);
      int64_t need = want - (int64_t)s->len;
      if (need < 0) need = 0;
      if (name_is(name, "padStart")) { for (int64_t i = 0; i < need; i++) sb_add(&sb, padc->len ? padc->data : " ", 1); sb_add(&sb, s->data, s->len); }
      else { sb_add(&sb, s->data, s->len); for (int64_t i = 0; i < need; i++) sb_add(&sb, padc->len ? padc->data : " ", 1); }
      AxValue r = ax_strv(ax_str_new(sb.buf, sb.len));
      free(sb.buf);
      ax_release(ax_strv(padc));
      return r;
    }
    if (name_is(name, "charAt")) {
      int64_t i = argc ? (int64_t)ax_to_num(args[0]) : 0;
      if (i < 0 || i >= (int64_t)s->len) return ax_str_from("");
      return ax_strv(ax_str_new(s->data + i, 1));
    }
    if (name_is(name, "charCodeAt")) {
      int64_t i = argc ? (int64_t)ax_to_num(args[0]) : 0;
      if (i < 0 || i >= (int64_t)s->len) return ax_null();
      return ax_num((unsigned char)s->data[i]);
    }
    if (name_is(name, "to_int") || name_is(name, "to_num")) {
      char *end = NULL;
      double d = name_is(name, "to_int") ? (double)strtol(s->data, &end, argc ? (int)ax_to_num(args[0]) : 10) : strtod(s->data, &end);
      return end == s->data ? ax_null() : ax_num(d);
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
      int64_t s0 = argc ? (int64_t)ax_to_num(args[0]) : 0;
      int64_t e0 = argc > 1 && args[1].t != AX_NULL ? (int64_t)ax_to_num(args[1]) : (int64_t)a->len;
      if (s0 < 0) s0 += a->len;
      if (e0 < 0) e0 += a->len;
      if (s0 < 0) s0 = 0;
      if (e0 > (int64_t)a->len) e0 = a->len;
      AxArr *out = ax_arr_new(4);
      for (int64_t i = s0; i < e0; i++) ax_arr_push(out, ax_copy(a->items[i]));
      return ax_arrv(out);
    }
    if (name_is(name, "includes") || name_is(name, "contains")) {
      for (uint32_t i = 0; i < a->len; i++) if (argc && ax_equals(a->items[i], args[0])) return ax_bool(true);
      return ax_bool(false);
    }
    if (name_is(name, "indexOf")) {
      for (uint32_t i = 0; i < a->len; i++) if (argc && ax_equals(a->items[i], args[0])) return ax_num(i);
      return ax_num(-1);
    }
    if (name_is(name, "reverse")) {
      for (uint32_t i = 0, j = a->len ? a->len - 1 : 0; i < j; i++, j--) { AxValue t = a->items[i]; a->items[i] = a->items[j]; a->items[j] = t; }
      ax_retain(obj);
      return obj;
    }
    if (name_is(name, "sort")) {
      AxValue sel = argc ? args[0] : ax_null();
      SortCtx c = { vm, sel, false };
      if (sel.t == AX_FN) {
        AxFn *f = (AxFn *)sel.o;
        c.comparator = (!f->native && f->nparams >= 2) || (f->native && f->min_args >= 2);
      }
      if (a->len > 1) {
        AxValue *tmp = malloc(sizeof(AxValue) * a->len);
        merge_sort(&c, a->items, a->len, tmp);
        free(tmp);
      }
      ax_retain(obj);
      return obj;
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
    if (name_is(name, "find_index")) return n_find_index(vm, NULL, fargs, fargc);
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
    if (name_is(name, "has")) {
      AxStr *k = argc ? ax_to_str(args[0]) : ax_str_newz("");
      bool r = ax_dict_has(d, k);
      ax_release(ax_strv(k));
      return ax_bool(r);
    }
    if (name_is(name, "get")) {
      AxStr *k = argc ? ax_to_str(args[0]) : ax_str_newz("");
      AxValue v;
      bool found = ax_dict_get(d, k, &v);
      ax_release(ax_strv(k));
      if (found) return v;
      return argc > 1 ? ax_copy(args[1]) : ax_null();
    }
    if (name_is(name, "set")) {
      AxStr *ks = argc ? ax_to_str(args[0]) : ax_str_newz("");
      AxStr *k = ax_intern(ks->data, ks->len);
      ax_dict_set(d, k, argc > 1 ? ax_copy(args[1]) : ax_null());
      ax_release(ax_strv(k));
      ax_release(ax_strv(ks));
      return argc > 1 ? ax_copy(args[1]) : ax_null();
    }
    if (name_is(name, "delete")) {
      AxStr *k = argc ? ax_to_str(args[0]) : ax_str_newz("");
      ax_dict_del(d, k);
      ax_release(ax_strv(k));
      return ax_null();
    }
    if (name_is(name, "map_values")) {
      AxDict *out = ax_dict_new();
      for (uint32_t i = 0; i < d->len; i++) {
        if (d->entries[i].dead) continue;
        AxValue kargs[2] = { d->entries[i].val, ax_strv(d->entries[i].key) };
        ax_retain(kargs[1]);
        AxValue mapped = argc ? ax_call(vm, args[0], kargs, 2) : ax_copy(d->entries[i].val);
        ax_release(kargs[1]);
        ax_dict_set(out, d->entries[i].key, mapped);
      }
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
