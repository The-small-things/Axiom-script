// imports.c — `^use "lib.ax"`: include-once module resolution, shared by the command, the REPL
// and the embedding API.

#include "axiom.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <limits.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

// The same rule as the reference implementation: paths resolve relative to the importing file,
// `.ax` is appended when the literal path is absent, each resolved path is visited at most once
// (so diamonds and cycles terminate), and a declaration the importing file already makes wins.
// Imported declarations are spliced into the program before it runs, which is why a function in
// a library is visible to the whole program regardless of where the ^use appears.

#define MAX_IMPORTS 128

typedef struct {
  char *paths[MAX_IMPORTS];
  int count;
} ImportSet;

static char *read_file(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (n < 0) { fclose(f); return NULL; }
  char *buf = malloc((size_t)n + 1);
  size_t got = fread(buf, 1, (size_t)n, f);
  buf[got] = '\0';
  fclose(f);
  return buf;
}

static bool import_seen(ImportSet *set, const char *path) {
  for (int i = 0; i < set->count; i++) if (strcmp(set->paths[i], path) == 0) return true;
  return false;
}

static void dirname_of(const char *path, char *out, size_t n) {
  const char *slash = strrchr(path, '/');
  if (!slash) { snprintf(out, n, "."); return; }
  size_t len = (size_t)(slash - path);
  if (len >= n) len = n - 1;
  memcpy(out, path, len);
  out[len] = '\0';
}

// What a declaration is called: a resource by its name (not its kind), a global by its first
// name, everything else by its own name.
static const AxStr *decl_name(const AxNode *d) {
  if (d->kind == N_RESOURCE) return d->str2;
  if (d->kind == N_GLOBAL) return d->nnames ? d->names[0] : NULL;
  return d->str;
}

// Appends every declaration of `src` that `dst` does not already declare.
static void splice_program(AxNode *dst, AxNode *src) {
  int extra = src->nlist;
  if (!extra) return;
  AxNode **merged = malloc(sizeof(AxNode *) * (size_t)(dst->nlist + extra));
  int n = 0;
  for (int i = 0; i < dst->nlist; i++) merged[n++] = dst->list[i];
  for (int i = 0; i < extra; i++) {
    AxNode *d = src->list[i];
    if (d->kind == N_MAIN) continue;          // only the entry file's ^main runs
    if (d->kind == N_USE) continue;           // already resolved
    bool clash = false;
    const AxStr *dn = decl_name(d);
    if (dn) {
      for (int j = 0; j < dst->nlist && !clash; j++) {
        AxNode *e = dst->list[j];
        const AxStr *en = decl_name(e);
        clash = en && e->kind == d->kind && (e->kind != N_FN || e->op == d->op) && ax_str_eq(en, dn);
      }
    }
    if (clash) continue;                      // the importing file wins
    merged[n++] = d;
  }
  dst->list = merged;
  dst->nlist = n;
}

static bool resolve_imports(AxVM *vm, AxNode *program, const char *from_path, ImportSet *seen, bool sandboxed, char *err, size_t errn) {
  char dir[4096];
  dirname_of(from_path ? from_path : "./x", dir, sizeof dir);
  // The list grows as imports are spliced in, so take the count up front.
  int original = program->nlist;
  for (int i = 0; i < original; i++) {
    AxNode *d = program->list[i];
    if (d->kind != N_USE) continue;
    for (int p = 0; p < d->nlist; p++) {
      const char *rel = d->list[p]->str->data;
      // Paths are bounded well below the buffer: a longer one is a mistake worth reporting
      // rather than silently truncating into a different file.
      size_t dlen = strlen(dir), rlen = strlen(rel);
      if (dlen + rlen + 8 >= 2048) { snprintf(err, errn, "^use path is too long"); return false; }
      char resolved[2048];
      memcpy(resolved, dir, dlen);
      resolved[dlen] = '/';
      memcpy(resolved + dlen + 1, rel, rlen);
      resolved[dlen + 1 + rlen] = '\0';
      if (access(resolved, R_OK) != 0) {
        // Try the same path with '.ax' appended — the bound above leaves room for it.
        memcpy(resolved + dlen + 1 + rlen, ".ax", 4);
        if (access(resolved, R_OK) != 0) resolved[dlen + 1 + rlen] = '\0';
      }
      char real[PATH_MAX];
      if (!ax_realpath(resolved, real)) snprintf(real, sizeof real, "%s", resolved);
      if (import_seen(seen, real)) continue;
      if (sandboxed && !ax_sandbox_can_read(vm, real)) { snprintf(err, errn, "sandbox: reading '%.200s' is not allowed", rel); return false; }
      if (seen->count >= MAX_IMPORTS) { snprintf(err, errn, "too many imports"); return false; }
      seen->paths[seen->count++] = strdup(real);

      char *src = read_file(real);
      if (!src) { snprintf(err, errn, "^use \"%.200s\": no such file (looked in %.200s)", rel, dir); return false; }
      AxTokens *toks = calloc(1, sizeof(AxTokens));
      if (!ax_tokenize(src, toks)) { snprintf(err, errn, "^use \"%.120s\" (line %d): %.240s", rel, toks->err_line, toks->err); return false; }
      AxParseResult *pr = calloc(1, sizeof(AxParseResult));
      if (!ax_parse(toks, pr)) { snprintf(err, errn, "^use \"%.120s\" (line %d): %.240s", rel, pr->err_line, pr->err); return false; }
      // Depth first, so a library's own imports are available to it.
      if (!resolve_imports(vm, pr->program, real, seen, sandboxed, err, errn)) return false;
      splice_program(program, pr->program);
    }
  }
  return true;
}

// One import set per VM: the REPL resolves each declaration chunk against it, so a library is
// loaded once per session; the command seeds it with the entry file.
static ImportSet *set_of(AxVM *vm) {
  if (!vm->imports) vm->imports = calloc(1, sizeof(ImportSet));
  return vm->imports;
}

void ax_imports_seed(AxVM *vm, const char *path) {
  ImportSet *set = set_of(vm);
  char real[4096];
  if (ax_realpath(path, real) && set->count < MAX_IMPORTS && !import_seen(set, real)) set->paths[set->count++] = strdup(real);
}

bool ax_imports_resolve(AxVM *vm, AxNode *program, const char *from_path, bool sandboxed, char *err, size_t errn) {
  return resolve_imports(vm, program, from_path, set_of(vm), sandboxed, err, errn);
}

void ax_imports_free(AxVM *vm) {
  ImportSet *set = vm->imports;
  if (!set) return;
  for (int i = 0; i < set->count; i++) free(set->paths[i]);
  free(set);
  vm->imports = NULL;
}
