// main.c — the `axiom` command.
//
// Deliberately the same surface as the JavaScript CLI for everything the native build supports,
// so a program and its invocation move between the two runtimes unchanged:
//
//   axiom prog.ax [-- args…]     run it
//   axiom --eval 'source'        run inline source
//   axiom --stdin                read the program from standard input
//   axiom --check                parse only; exit 1 on an error
//   axiom --sandbox …            deny file writes and subprocesses unless explicitly allowed
//
// Anything belonging to the engine half (entities, frame blocks, rendering) is refused by the
// parser with a message naming the JavaScript runtime, rather than being silently ignored.

#include "axiom.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <limits.h>

static char *read_file(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *buf = malloc((size_t)n + 1);
  size_t got = fread(buf, 1, (size_t)n, f);
  buf[got] = '\0';
  fclose(f);
  return buf;
}

static char *read_stdin_all(void) {
  size_t cap = 65536, len = 0;
  char *buf = malloc(cap);
  size_t got;
  while ((got = fread(buf + len, 1, cap - len - 1, stdin)) > 0) {
    len += got;
    if (len + 1 >= cap) { cap *= 2; buf = realloc(buf, cap); }
  }
  buf[len] = '\0';
  return buf;
}

// ---------------------------------------------------------------------------------------------
// `^use "lib.ax"` — include-once module resolution.
//
// The same rule as the reference implementation: paths resolve relative to the importing file,
// `.ax` is appended when the literal path is absent, each resolved path is visited at most once
// (so diamonds and cycles terminate), and a declaration the importing file already makes wins.
// Imported declarations are spliced into the program before it runs, which is why a function in
// a library is visible to the whole program regardless of where the ^use appears.
// ---------------------------------------------------------------------------------------------

#define MAX_IMPORTS 128

typedef struct {
  char *paths[MAX_IMPORTS];
  int count;
} ImportSet;

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
    if (d->str) {
      for (int j = 0; j < dst->nlist && !clash; j++) {
        AxNode *e = dst->list[j];
        clash = e->str && e->kind == d->kind && ax_str_eq(e->str, d->str);
      }
    }
    if (clash) continue;                      // the importing file wins
    merged[n++] = d;
  }
  dst->list = merged;
  dst->nlist = n;
}

static bool resolve_imports(AxNode *program, const char *from_path, ImportSet *seen, char *err, size_t errn) {
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
      if (!realpath(resolved, real)) snprintf(real, sizeof real, "%s", resolved);
      if (import_seen(seen, real)) continue;
      if (seen->count >= MAX_IMPORTS) { snprintf(err, errn, "too many imports"); return false; }
      seen->paths[seen->count++] = strdup(real);

      char *src = read_file(real);
      if (!src) { snprintf(err, errn, "^use \"%.200s\": no such file (looked in %.200s)", rel, dir); return false; }
      AxTokens *toks = calloc(1, sizeof(AxTokens));
      if (!ax_tokenize(src, toks)) { snprintf(err, errn, "^use \"%.120s\" (line %d): %.240s", rel, toks->err_line, toks->err); return false; }
      AxParseResult *pr = calloc(1, sizeof(AxParseResult));
      if (!ax_parse(toks, pr)) { snprintf(err, errn, "^use \"%.120s\" (line %d): %.240s", rel, pr->err_line, pr->err); return false; }
      // Depth first, so a library's own imports are available to it.
      if (!resolve_imports(pr->program, real, seen, err, errn)) return false;
      splice_program(program, pr->program);
    }
  }
  return true;
}

static void usage(void) {
  printf(
    "AxiomScript %s (native) — the language runtime, without the engine.\n"
    "\n"
    "usage: axiom <file.ax> [options] [-- program args]\n"
    "\n"
    "  --eval '<source>'   run inline source\n"
    "  --stdin             read the program from standard input\n"
    "  --check             parse and check only; exit 1 on an error\n"
    "  -- a b c            arguments for the program, readable with args()\n"
    "\n"
    "  --sandbox           deny file writes, subprocesses, and unlisted reads\n"
    "  --allow-read PATH   permit reads under PATH\n"
    "  --allow-write PATH  permit writes under PATH\n"
    "  --allow-exec        permit sh()\n"
    "\n"
    "  --version           print the version\n"
    "  --help              print this message\n"
    "\n"
    "Entities, frame blocks and rendering are hosted by the JavaScript runtime:\n"
    "  node main.js world.ax\n", AX_VERSION);
}

int main(int argc, char **argv) {
  const char *path = NULL;
  char *source = NULL;
  bool check_only = false, use_stdin = false;
  bool sandbox = false, allow_exec = false;
  const char *allow_read[64];
  int n_allow_read = 0;
  const char *allow_write[64];
  int n_allow_write = 0;
  char **prog_args = NULL;
  int n_prog_args = 0;

  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) { usage(); return 0; }
    if (strcmp(a, "--version") == 0 || strcmp(a, "-v") == 0) { printf("%s\n", AX_VERSION); return 0; }
    if (strcmp(a, "--check") == 0) { check_only = true; continue; }
    if (strcmp(a, "--stdin") == 0) { use_stdin = true; continue; }
    if (strcmp(a, "--eval") == 0 && i + 1 < argc) { source = strdup(argv[++i]); continue; }
    if (strcmp(a, "--sandbox") == 0) { sandbox = true; continue; }
    if (strcmp(a, "--allow-exec") == 0) { allow_exec = true; continue; }
    if (strcmp(a, "--allow-read") == 0 && i + 1 < argc) { if (n_allow_read < 64) allow_read[n_allow_read++] = argv[++i]; continue; }
    if (strcmp(a, "--allow-write") == 0 && i + 1 < argc) { if (n_allow_write < 64) allow_write[n_allow_write++] = argv[++i]; continue; }
    if (strcmp(a, "--") == 0) { prog_args = &argv[i + 1]; n_prog_args = argc - i - 1; break; }
    if (a[0] == '-' && a[1] == '-') { fprintf(stderr, "axiom: unknown flag %s (try --help)\n", a); return 2; }
    if (!path) path = a;
  }

  if (!source) {
    if (use_stdin) source = read_stdin_all();
    else if (path) {
      source = read_file(path);
      if (!source) { fprintf(stderr, "axiom: cannot read %s\n", path); return 2; }
    } else { usage(); return 2; }
  }

  AxTokens toks;
  if (!ax_tokenize(source, &toks)) {
    fprintf(stderr, "%s:%d: %s\n", path ? path : "<eval>", toks.err_line, toks.err);
    return 1;
  }
  AxParseResult pr;
  if (!ax_parse(&toks, &pr)) {
    fprintf(stderr, "%s:%d: %s\n", path ? path : "<eval>", pr.err_line, pr.err);
    return 1;
  }
  ImportSet imports = {0};
  if (path) {
    char real[4096];
    if (realpath(path, real)) imports.paths[imports.count++] = strdup(real);
  }
  char imp_err[512] = {0};
  if (!resolve_imports(pr.program, path, &imports, imp_err, sizeof imp_err)) {
    fprintf(stderr, "%s: %s\n", path ? path : "<eval>", imp_err);
    return 1;
  }
  if (check_only) {
    fprintf(stderr, "OK: %s parses clean.\n", path ? path : "<eval>");
    return 0;
  }

  AxVM *vm = ax_vm_new();
  ax_stdlib_install(vm);
  vm->sandbox = sandbox;
  vm->allow_exec = allow_exec;
  for (int i = 0; i < n_allow_read; i++) ax_arr_push(vm->allow_read, ax_str_from(allow_read[i]));
  for (int i = 0; i < n_allow_write; i++) ax_arr_push(vm->allow_write, ax_str_from(allow_write[i]));
  for (int i = 0; i < n_prog_args; i++) ax_arr_push(vm->argv, ax_str_from(prog_args[i]));
  vm->source_path = path;

  AxValue result = ax_null();
  bool had_main = ax_run_program(vm, pr.program, vm->argv, &result);
  if (!had_main) {
    fprintf(stderr, "axiom: %s declares no ^main, so running it does nothing.\n", path ? path : "<eval>");
    ax_release(result);
    return 0;
  }
  int code = vm->exit_code;
  if (result.t == AX_NUM) code = (int)result.num;
  ax_release(result);
  fflush(stdout);
  // The VM and AST are torn down only when it is free to do so: at exit the OS reclaims
  // everything, and walking the heap first would just make start-up-to-answer slower.
  return code;
}
