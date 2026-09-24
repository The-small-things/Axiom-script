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
//   axiom world.ax --sim N       step a simulation N frames headless (60 Hz physics)
//   axiom world.ax --sim N --json   … and print the final state as JSON (main.js's format)
//   axiom world.ax --run         run ^main only, even when the program declares entities
//
// A program with a ^main and entities runs ^main first (as setup) and then the frame loop.

#include "axiom.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <limits.h>
#include <ctype.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <setjmp.h>
#include "term.h"
#include "render.h"

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

// For the REPL: each declaration chunk resolves its imports against one shared set, so a
// library is loaded once per session.
bool ax_imports_resolve(AxNode *program, const char *from_path, char *err, size_t errn) {
  static ImportSet session;
  return resolve_imports(program, from_path, &session, err, errn);
}

static void usage(void) {
  printf(
    "AxiomScript %s (native)\n"
    "\n"
    "usage: axiom <file.ax> [options] [-- program args]\n"
    "\n"
    "  --eval '<source>'   run inline source\n"
    "  --stdin             read the program from standard input\n"
    "  --check             parse and check only; exit 1 on an error\n"
    "  --repl              interactive session: statements run as typed, expressions echo\n"
    "  -- a b c            arguments for the program, readable with args()\n"
    "\n"
    "  --sim N             step a simulation N frames headless, no renderer\n"
    "  --json              with --sim (or a script), print the final state as JSON\n"
    "  --input FILE        one line of WASD/space per frame, applied to input.move/jump\n"
    "  --run               run ^main and exit, even when the program declares entities\n"
    "\n"
    "  --terminal [N], -t  draw N frames in the terminal (the default with entities)\n"
    "  --ascii, -A         ASCII density characters instead of half-blocks\n"
    "  --no-color, -C      no ANSI colour\n"
    "  --subpixel, -S      ▌▐ half-cells: double horizontal resolution\n"
    "  --term-fps N        terminal frame rate (default 15, 1..60)\n"
    "  --headless N        step N frames and save PNGs to screenshots/ (see --png-every)\n"
    "  --png-every N       with --headless, save every Nth frame (default 30)\n"
    "  --width N, --height N   render resolution\n"
    "\n"
    "  --sandbox           deny file writes, subprocesses, and unlisted reads\n"
    "  --allow-read PATH   permit reads under PATH\n"
    "  --allow-write PATH  permit writes under PATH\n"
    "  --allow-exec        permit sh()\n"
    "\n"
    "  --version           print the version\n"
    "  --help              print this message\n", AX_VERSION);
}

int main(int argc, char **argv) {
  // static: exit() longjmps back into main, and these must survive it.
  static const char *path = NULL;
  static char *source = NULL;
  static bool check_only = false, use_stdin = false;
  static bool sandbox = false, allow_exec = false;
  static const char *allow_read[64];
  static int n_allow_read = 0;
  static const char *allow_write[64];
  static int n_allow_write = 0;
  static char **prog_args = NULL;
  static int n_prog_args = 0;
  static bool sim = false, json = false, run_only = false, repl = false;
  static bool terminal = false, headless = false, ascii = false, no_color = false, subpixel = false;
  static int frames = 60, term_fps = 15, png_every = 30, width = 0, height = 0;
  static const char *input_path = NULL;

  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) { usage(); return 0; }
    if (strcmp(a, "--version") == 0 || strcmp(a, "-v") == 0) { printf("%s\n", AX_VERSION); return 0; }
    if (strcmp(a, "--check") == 0) { check_only = true; continue; }
    if (strcmp(a, "--repl") == 0) { repl = true; continue; }
    if (strcmp(a, "--stdin") == 0) { use_stdin = true; continue; }
    if (strcmp(a, "--eval") == 0 && i + 1 < argc) { source = strdup(argv[++i]); continue; }
    if (strcmp(a, "--sandbox") == 0) { sandbox = true; continue; }
    if (strcmp(a, "--allow-exec") == 0) { allow_exec = true; continue; }
    if (strcmp(a, "--allow-read") == 0 && i + 1 < argc) { if (n_allow_read < 64) allow_read[n_allow_read++] = argv[++i]; continue; }
    if (strcmp(a, "--allow-write") == 0 && i + 1 < argc) { if (n_allow_write < 64) allow_write[n_allow_write++] = argv[++i]; continue; }
    if (strcmp(a, "--sim") == 0) { sim = true; if (i + 1 < argc) { frames = atoi(argv[++i]); if (frames <= 0) frames = 60; } continue; }
    if (strcmp(a, "--json") == 0) { json = true; continue; }
    if (strcmp(a, "--terminal") == 0 || strcmp(a, "-t") == 0) {
      terminal = true;
      if (i + 1 < argc && argv[i + 1][0] && strspn(argv[i + 1], "0123456789") == strlen(argv[i + 1])) frames = atoi(argv[++i]);
      continue;
    }
    if (strcmp(a, "--headless") == 0) { headless = true; if (i + 1 < argc) { frames = atoi(argv[++i]); if (frames <= 0) frames = 60; } continue; }
    if (strcmp(a, "--ascii") == 0 || strcmp(a, "-A") == 0) { terminal = true; ascii = true; continue; }
    if (strcmp(a, "--no-color") == 0 || strcmp(a, "-C") == 0) { no_color = true; continue; }
    if (strcmp(a, "--subpixel") == 0 || strcmp(a, "-S") == 0) { terminal = true; subpixel = true; continue; }
    if (strcmp(a, "--term-fps") == 0 && i + 1 < argc) { term_fps = atoi(argv[++i]); continue; }
    if (strcmp(a, "--png-every") == 0 && i + 1 < argc) { png_every = atoi(argv[++i]); if (png_every <= 0) png_every = 30; continue; }
    if (strcmp(a, "--width") == 0 && i + 1 < argc) { width = atoi(argv[++i]); continue; }
    if (strcmp(a, "--height") == 0 && i + 1 < argc) { height = atoi(argv[++i]); continue; }
    if (strcmp(a, "--run") == 0 || strcmp(a, "-r") == 0) { run_only = true; continue; }
    if (strcmp(a, "--input") == 0 && i + 1 < argc) { input_path = argv[++i]; continue; }
    if (strcmp(a, "--no-restack") == 0) continue;   // accepted for command-line parity with main.js
    if (strcmp(a, "--") == 0) { prog_args = &argv[i + 1]; n_prog_args = argc - i - 1; break; }
    if (a[0] == '-' && a[1] == '-') { fprintf(stderr, "axiom: unknown flag %s (try --help)\n", a); return 2; }
    if (!path) path = a;
  }

  if (repl) {
    AxVM *vm = ax_vm_new();
    ax_stdlib_install(vm);
    vm->sandbox = sandbox;
    vm->allow_exec = allow_exec;
    for (int i = 0; i < n_allow_read; i++) ax_arr_push(vm->allow_read, ax_str_from(allow_read[i]));
    for (int i = 0; i < n_allow_write; i++) ax_arr_push(vm->allow_write, ax_str_from(allow_write[i]));
    for (int i = 0; i < n_prog_args; i++) ax_arr_push(vm->argv, ax_str_from(prog_args[i]));
    return ax_repl(vm);
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

  const char *label = path ? path : (use_stdin ? "<stdin>" : "<eval>");
  // exit(n) lands here from wherever it is called. What is printed depends on the phase the
  // program was in, exactly as main.js: during ^main, the script result; during a --sim run,
  // the state dump; otherwise just the diagnostics.
  static jmp_buf exit_target;
  static volatile int phase = 0;          // 0 loading, 1 ^main, 2 frame loop
  static volatile int frames_done = 0;
  static volatile bool drawing = false;
  vm->exit_jmp = &exit_target;
  if (setjmp(exit_target)) {
    int code = vm->exit_code;
    fflush(stdout);
    if (drawing && isatty(STDOUT_FILENO)) fputs("\x1b[0m", stdout);
    if (phase == 1 && json) ax_engine_print_script_json(vm, ax_null(), code, stdout);
    else if (phase == 2 && sim && json) ax_engine_print_json(vm, frames_done, stdout);
    else if (!json) ax_engine_print_diags(vm, stderr);
    fflush(stdout);
    return code & 0xff;
  }

  ax_engine_init(vm, source);
  if (json) ax_engine_quiet(vm, true);
  ax_program_declare(vm, pr.program);
  bool has_entities = ax_engine_load(vm, pr.program, source);
  ax_engine_set_version(vm, toks.version);
  int n_entities = ax_engine_entity_count(vm);
  bool has_main = false;
  for (int i = 0; i < pr.program->nlist; i++) if (pr.program->list[i]->kind == N_MAIN) has_main = true;
  (void)has_entities;

  // SCRIPT MODE (main.js): ^main runs first; the frame loop follows only when there are
  // entities to simulate and --run was not given.
  if (has_main) {
    bool loop_follows = !run_only && n_entities > 0;
    if (!json && loop_follows) printf("Loaded %s: running ^main, then %d entities\n", label, n_entities);
    AxValue result = ax_null();
    phase = 1;
    bool ok = ax_run_main(vm, pr.program, vm->argv, &result);
    if (!ok) {
      // A fault in ^main ends the program, reported at the statement it escaped from.
      fflush(stdout);
      if (json) ax_engine_print_script_json(vm, ax_null(), 1, stdout);
      else {
        char code[32], msg[512];
        int line = ax_engine_last_fault(vm, code, sizeof code, msg, sizeof msg);
        if (line) fprintf(stderr, "%s:%d: runtime error [%s]: %s\n", label, line, code, msg);
        else fprintf(stderr, "%s:?: runtime error [%s]: %s\n", label, code, msg);
        ax_engine_print_diags(vm, stderr);
      }
      fflush(stdout);
      return 1;
    }
    int code = vm->exit_code;
    if (result.t == AX_NUM) code = ax_to_int32(result.num);
    if (!loop_follows) {
      if (json) ax_engine_print_script_json(vm, result, code, stdout);
      else {
        fflush(stdout);
        ax_engine_print_diags(vm, stderr);
      }
      ax_release(result);
      fflush(stdout);
      return code & 0xff;
    }
    ax_release(result);
  } else if (run_only) {
    fprintf(stderr, "--run needs a '^main:' entry point; %s has none.\n", label);
    return 1;
  } else if (!n_entities && !sim) {
    fprintf(stderr, "axiom: %s declares no ^main and no entities, so running it does nothing.\n", label);
    return 0;
  }
  phase = 2;
  if (!json && !has_main) {
    printf("Loaded %s: ", label);
    ax_engine_summary(vm, stdout);
    printf(", version %s\n", toks.version ? toks.version : "(none)");
  }

  // The frame loop: --sim steps it with no rendering at all; --headless renders to PNG files;
  // otherwise (as in main.js when there is no SDL window) it draws in the terminal.
  bool draw_terminal = !sim && !headless;
  drawing = draw_terminal;
  (void)terminal;
  AxTermOptions topt = { 0 };
  int rw = width > 0 ? width : (draw_terminal ? 160 : 640), rh = height > 0 ? height : (draw_terminal ? 120 : 480);
  if (draw_terminal) {
    bool uni, col;
    ax_term_detect(&uni, &col);
    topt.unicode = ascii ? false : uni;
    topt.color = no_color ? false : col;
    topt.subpixel = subpixel && !ascii;
    struct winsize ws;
    int cols = 80, lines = 24;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col) { cols = ws.ws_col; lines = ws.ws_row; }
    topt.term_width = cols < 80 ? cols : 80;
    topt.term_height = lines - 2 < 40 ? lines - 2 : 40;
    topt.is_tty = isatty(STDOUT_FILENO);
    if (term_fps < 1) term_fps = 1;
    if (term_fps > 60) term_fps = 60;
    if (!json) {
      fprintf(stderr, "\nMode: terminal @ %dx%d (%s%s, %s).\n", rw, rh, topt.unicode ? "unicode" : "ASCII", topt.subpixel ? "+subpixel" : "", topt.color ? "color" : "no color");
      fprintf(stderr, "Running %d frames @ %d fps target.\n", frames, term_fps);
    }
  } else if (headless && !json) {
    printf("\nMode: headless.\nRunning %d frames @ %dx%d, saving PNG every %d frames.\n", frames, rw, rh, png_every);
  }
  char **input_lines = NULL;
  int n_input = 0;
  if (input_path) {
    char *text = read_file(input_path);
    if (text) {
      for (char *line = strtok(text, "\n"); line; line = strtok(NULL, "\n")) {
        while (*line == ' ' || *line == '\t' || *line == '\r') line++;
        size_t L = strlen(line);
        while (L && (line[L - 1] == ' ' || line[L - 1] == '\r' || line[L - 1] == '\t')) line[--L] = '\0';
        if (!L || line[0] == '#') continue;
        input_lines = realloc(input_lines, sizeof(char *) * (n_input + 1));
        input_lines[n_input++] = line;
      }
    }
  }
  for (int f = 0; f < frames; f++) {
    double mx = 0, my = 0;
    bool jump = false;
    if (f < n_input) {
      for (const char *c = input_lines[f]; *c; c++) {
        char u = (char)toupper((unsigned char)*c);
        if (u == 'W') my += 1; else if (u == 'S') my -= 1;
        else if (u == 'A') mx -= 1; else if (u == 'D') mx += 1;
        else if (u == ' ') jump = true;
      }
    }
    ax_engine_set_input(vm, mx, my, jump);
    frames_done = f + 1;
    ax_engine_update(vm, 1.0 / 60);
    if (draw_terminal) {
      uint8_t *px = ax_render_frame(vm, rw, rh);
      char *text = ax_term_render(px, rw, rh, &topt);
      fputs(text, stdout);
      fflush(stdout);
      free(text);
      free(px);
      struct timespec ts = { 0, (long)(1e9 / term_fps) };
      nanosleep(&ts, NULL);
    } else if (headless && ((f + 1) % png_every == 0 || f == frames - 1)) {
      uint8_t *px = ax_render_frame(vm, rw, rh);
      mkdir("screenshots", 0777);
      char name[64];
      snprintf(name, sizeof name, "screenshots/frame_%05d.png", f + 1);
      if (ax_write_png(name, px, rw, rh) && !json) printf("  frame %d → %s\n", f + 1, name);
      free(px);
    }
  }
  if (draw_terminal) fputs("\x1b[0m\n", stdout);
  if (!json) ax_engine_print_diags(vm, stderr);
  else ax_engine_print_json(vm, frames, stdout);
  fflush(stdout);
  return ax_engine_diag_count(vm) ? 1 : 0;
}
