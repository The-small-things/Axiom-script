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
