// api.c — the embedding API (axiom_api.h) over the runtime the command uses.
//
// Each entry point that runs AxiomScript goes through guarded(): exit(n) and any error nothing
// caught land there instead of ending the host process, and the VM's handler stack, call depth
// and engine context are restored, so the instance stays usable. Each instance parses into its
// own AST arena, freed with it.

#include "axiom.h"
#include "axiom_api.h"
#include "render.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <setjmp.h>

typedef struct {
  char *name;
  axiom_host_fn fn;
  void *user;
} Host;

struct axiom {
  AxVM *vm;
  void *arena;              // this instance's AST
  char *source, *filename;
  AxTokens *toks;           // kept: the AST points into them
  AxParseResult *pr;
  bool loaded, exited;
  int exit_code;
  int frames;               // frames stepped
  AxValue result;           // the last ^main result
  char *error;
  Host *hosts;
  int nhosts;
  bool host_failed;         // axiom_host_error during the current host call
  char host_code[32];
  char *host_msg;
};

const char *axiom_version(void) { return AX_VERSION; }

// ---- errors and output ------------------------------------------------------------------------

static void set_error(axiom *ax, const char *text) {
  free(ax->error);
  ax->error = text ? strdup(text) : NULL;
}

static void out(axiom *ax, int stream, const char *text) {
  if (text && *text) ax_write(ax->vm, stream, text, strlen(text));
}

// The error a guarded call caught: "CODE: message", also written to stream 2 the way the
// command reports an unhandled error.
static void take_error(axiom *ax) {
  AxVM *vm = ax->vm;
  const char *code = vm->error_code[0] ? vm->error_code : "AX-RUNTIME-000";
  char *msg = NULL;
  AxValue m;
  if (vm->error.t == AX_DICT && ax_dict_get((AxDict *)vm->error.o, ax_internz("msg"), &m)) {
    AxStr *s = ax_to_str(m);
    msg = strdup(s->data);
    ax_release(ax_strv(s));
    ax_release(m);
  } else {
    msg = strdup(vm->error_msg);
  }
  size_t n = strlen(code) + strlen(msg) + 32;
  char *text = malloc(n);
  snprintf(text, n, "%s: %s", code, msg);
  set_error(ax, text);
  snprintf(text, n, "runtime error [%s]: %s\n", code, msg);
  out(ax, 2, text);
  free(text);
  free(msg);
  vm->error_code[0] = '\0';
}

// A FILE* that collects into a string, for the printers the command uses.
typedef struct { FILE *f; char *buf; size_t len; } Mem;
static bool mem_open(Mem *m) { m->buf = NULL; m->len = 0; m->f = open_memstream(&m->buf, &m->len); return m->f != NULL; }
static char *mem_close(Mem *m) { fclose(m->f); return m->buf; }

// ---- guarded execution ------------------------------------------------------------------------

enum { RAN = 0, EXITED = 1, THREW = 2 };

static int guarded(axiom *ax, void (*body)(axiom *ax, void *arg), void *arg) {
  AxVM *vm = ax->vm;
  jmp_buf exit_target;
  jmp_buf *saved_exit = vm->exit_jmp;
  int base = vm->nhandlers, depth = vm->call_depth;
  AxCtx ctx = vm->ctx;
  void *saved_arena = ax_arena_swap(ax->arena);
  int live = vm->nlive;
  volatile int outcome = RAN;
  vm->exit_jmp = &exit_target;
  if (setjmp(exit_target)) {
    outcome = EXITED;
  } else if (base >= AX_MAX_HANDLERS) {
    outcome = THREW;
    snprintf(vm->error_code, sizeof vm->error_code, "AX-RUNTIME-000");
    snprintf(vm->error_msg, sizeof vm->error_msg, "too many nested calls into the runtime");
  } else {
    vm->nhandlers = base + 1;
    vm->handler_live[base] = live;
    if (setjmp(vm->handlers[base])) outcome = THREW;
    else body(ax, arg);
  }
  ax_arena_swap(saved_arena);
  ax_unwind_scopes(vm, live);   // exit() leaves its scopes behind
  vm->nhandlers = base;
  vm->call_depth = depth;
  vm->ctx = ctx;
  vm->exit_jmp = saved_exit;
  if (outcome == EXITED) {
    ax->exited = true;
    ax->exit_code = vm->exit_code;
  } else if (outcome == THREW) {
    take_error(ax);
  }
  fflush(stdout);
  return outcome;
}

// ---- instances --------------------------------------------------------------------------------

axiom *axiom_new(void) {
  axiom *ax = calloc(1, sizeof *ax);
  if (!ax) return NULL;
  ax->vm = ax_vm_new();
  ax->vm->embed = ax;
  ax->arena = ax_arena_new();
  ax_stdlib_install(ax->vm);
  ax->vm->sandbox = true;
  ax->result = ax_null();
  return ax;
}

void axiom_free(axiom *ax) {
  if (!ax) return;
  ax_release(ax->result);
  ax_engine_free(ax->vm);
  ax_imports_free(ax->vm);
  ax_vm_free(ax->vm);
  if (ax->toks) { ax_tokens_free(ax->toks); free(ax->toks); }
  free(ax->pr);
  ax_arena_free(ax->arena);
  for (int i = 0; i < ax->nhosts; i++) free(ax->hosts[i].name);
  free(ax->hosts);
  free(ax->host_msg);
  free(ax->source);
  free(ax->filename);
  free(ax->error);
  free(ax);
}

void axiom_set_output(axiom *ax, axiom_write_fn fn, void *user) {
  ax->vm->write = fn;
  ax->vm->write_user = user;
}

void axiom_set_args(axiom *ax, int argc, const char *const *argv) {
  AxArr *a = ax->vm->argv;
  while (a->len) { a->len--; ax_release(a->items[a->len]); }
  for (int i = 0; i < argc; i++) ax_arr_push(a, ax_str_from(argv[i]));
}

void axiom_sandbox(axiom *ax, int on) { ax->vm->sandbox = on != 0; }
void axiom_allow_read(axiom *ax, const char *path) { ax_arr_push(ax->vm->allow_read, ax_str_from(path)); }
void axiom_allow_write(axiom *ax, const char *path) { ax_arr_push(ax->vm->allow_write, ax_str_from(path)); }
void axiom_allow_exec(axiom *ax, int on) { ax->vm->allow_exec = on != 0; }

const char *axiom_error(axiom *ax) { return ax->error; }
int axiom_exit_code(axiom *ax) { return ax->exit_code; }
void axiom_release(void *p) { free(p); }

// ---- host functions ---------------------------------------------------------------------------

static AxValue host_call(AxVM *vm, AxFn *self, AxValue *args, int argc) {
  axiom *ax = vm->embed;
  int id = (int)self->bound.num;
  AxArr *a = ax_arr_new((uint32_t)argc);
  for (int i = 0; i < argc; i++) ax_arr_push(a, ax_copy(args[i]));
  char *json = NULL;
  ax_json_write(ax_arrv(a), 0, &json);
  ax_release(ax_arrv(a));
  ax->host_failed = false;
  char *res = ax->hosts[id].fn(ax, ax->hosts[id].user, json);
  free(json);
  if (ax->host_failed) {
    free(res);
    ax->host_failed = false;
    ax_throw(vm, ax->host_code, "%s", ax->host_msg ? ax->host_msg : "host function failed");
  }
  if (!res) return ax_null();
  AxValue v;
  bool ok = ax_json_parse(res, &v);
  free(res);
  if (!ok) ax_throw(vm, "AX-HOST-001", "host function %s returned text that is not JSON", ax->hosts[id].name);
  return v;
}

int axiom_define(axiom *ax, const char *name, axiom_host_fn fn, void *user) {
  if (!name || !(isalpha((unsigned char)name[0]) || name[0] == '_') || !fn) return 1;
  for (const char *p = name; *p; p++) if (!(isalnum((unsigned char)*p) || *p == '_')) return 1;
  int id = -1;
  for (int i = 0; i < ax->nhosts; i++) if (!strcmp(ax->hosts[i].name, name)) id = i;
  if (id < 0) {
    ax->hosts = realloc(ax->hosts, sizeof(Host) * (size_t)(ax->nhosts + 1));
    id = ax->nhosts++;
    ax->hosts[id].name = strdup(name);
  }
  ax->hosts[id].fn = fn;
  ax->hosts[id].user = user;
  AxValue f = ax_native(ax->hosts[id].name, host_call, 0, -1);
  ((AxFn *)f.o)->bound = ax_num(id);
  AxStr *key = ax_internz(name);
  ax_scope_declare(ax->vm->builtins, key, f);
  ax_release(ax_strv(key));
  return 0;
}

void axiom_host_error(axiom *ax, const char *code, const char *message) {
  ax->host_failed = true;
  snprintf(ax->host_code, sizeof ax->host_code, "%s", code && *code ? code : "AX-HOST-002");
  free(ax->host_msg);
  ax->host_msg = strdup(message ? message : "host function failed");
}

// ---- checking and loading ---------------------------------------------------------------------

typedef struct { AxTokens *toks; AxParseResult *pr; AxCheck *chk; } Compiled;

static Compiled compile(const char *source, const char *filename) {
  Compiled c;
  c.toks = calloc(1, sizeof *c.toks);
  c.pr = calloc(1, sizeof *c.pr);
  bool lexed = ax_tokenize(source, c.toks);
  bool parsed = lexed && ax_parse(c.toks, c.pr);
  c.chk = parsed ? ax_check(c.pr->program, source, filename, c.toks->version, NULL, 0, 0)
                 : ax_check(NULL, source, filename, NULL, lexed ? c.pr->err : c.toks->err,
                            lexed ? c.pr->err_line : c.toks->err_line, lexed ? c.pr->err_col : 0);
  return c;
}

char *axiom_check(const char *source, const char *filename) {
  void *arena = ax_arena_new();
  void *saved = ax_arena_swap(arena);
  Compiled c = compile(source, filename);
  Mem m;
  char *json = NULL;
  if (mem_open(&m)) {
    ax_check_print_json(c.chk, m.f);
    json = mem_close(&m);
  }
  ax_check_free(c.chk);
  ax_tokens_free(c.toks);
  free(c.toks);
  free(c.pr);
  ax_arena_swap(saved);
  ax_arena_free(arena);
  return json;
}

static void load_body(axiom *ax, void *arg) {
  (void)arg;
  AxVM *vm = ax->vm;
  ax_engine_init(vm, ax->source);
  ax_program_declare(vm, ax->pr->program);
  ax_engine_load(vm, ax->pr->program, ax->source);
  ax_engine_set_version(vm, ax->toks->version);
}

int axiom_load(axiom *ax, const char *source, const char *filename) {
  set_error(ax, NULL);
  if (ax->loaded || ax->toks) {
    set_error(ax, "AX-API-001: this instance already holds a program (use a new instance)");
    out(ax, 2, "axiom: this instance already holds a program (use a new instance)\n");
    return 1;
  }
  ax->source = strdup(source ? source : "");
  ax->filename = filename ? strdup(filename) : NULL;
  void *saved = ax_arena_swap(ax->arena);
  Compiled c = compile(ax->source, ax->filename);
  ax->toks = c.toks;
  ax->pr = c.pr;
  bool ok = ax_check_ok(c.chk);
  if (!ok) {
    Mem m;
    if (mem_open(&m)) {
      fputs("compile failed:\n", m.f);
      ax_check_print(c.chk, m.f);
      char *text = mem_close(&m);
      out(ax, 2, text);
      size_t n = strlen(text);
      while (n && text[n - 1] == '\n') text[--n] = '\0';
      set_error(ax, text);
      free(text);
    }
  }
  ax_check_free(c.chk);
  char err[512] = { 0 };
  if (ok) {
    if (ax->filename) ax_imports_seed(ax->vm, ax->filename);
    if (!ax_imports_resolve(ax->vm, ax->pr->program, ax->filename, ax->vm->sandbox, err, sizeof err)) {
      ok = false;
      char text[700];
      snprintf(text, sizeof text, "%s: %s\n", ax->filename ? ax->filename : "<eval>", err);
      out(ax, 2, text);
      snprintf(text, sizeof text, "AX-USE-001: %s", err);
      set_error(ax, text);
    }
  }
  ax_arena_swap(saved);
  if (!ok) return 1;
  if (guarded(ax, load_body, NULL) != RAN) return 1;
  ax->loaded = true;
  return 0;
}

// ---- running ------------------------------------------------------------------------------------

static void main_body(axiom *ax, void *arg) {
  AxVM *vm = ax->vm;
  int *code = arg;
  AxValue result = ax_null();
  if (!ax_run_main(vm, ax->pr->program, vm->argv, &result)) {
    // A fault in ^main ends the program, reported at the statement it escaped from.
    char ecode[32], msg[512], text[700], where[64];
    int line = ax_engine_last_fault(vm, ecode, sizeof ecode, msg, sizeof msg);
    if (line) snprintf(where, sizeof where, "%d", line); else snprintf(where, sizeof where, "?");
    snprintf(text, sizeof text, "%s:%s: runtime error [%s]: %s\n", ax->filename ? ax->filename : "<eval>", where, ecode, msg);
    out(ax, 2, text);
    snprintf(text, sizeof text, "%s: %s", ecode, msg);
    set_error(ax, text);
    *code = 1;
    return;
  }
  *code = vm->exit_code;
  if (result.t == AX_NUM) *code = ax_to_int32(result.num);
  ax_release(ax->result);
  ax->result = result;
}

int axiom_main(axiom *ax) {
  set_error(ax, NULL);
  if (!ax->loaded) {
    set_error(ax, "AX-API-002: no program is loaded");
    return 1;
  }
  if (ax->exited) return ax->exit_code;
  bool has_main = false;
  for (int i = 0; i < ax->pr->program->nlist; i++) if (ax->pr->program->list[i]->kind == N_MAIN) has_main = true;
  if (!has_main) return 0;
  int code = 0;
  ax_release(ax->result);
  ax->result = ax_null();
  int outcome = guarded(ax, main_body, &code);
  if (outcome == EXITED) return ax->exit_code;
  if (outcome == THREW) code = 1;
  ax->exit_code = code;
  return code;
}

int axiom_run(axiom *ax, const char *source, const char *filename) {
  if (axiom_load(ax, source, filename)) {
    ax->exit_code = ax->exited ? ax->exit_code : 1;
    return ax->exit_code;
  }
  return axiom_main(ax);
}

char *axiom_result(axiom *ax) {
  if (!ax->vm->world) ax_engine_init(ax->vm, ax->source);
  Mem m;
  if (!mem_open(&m)) return NULL;
  ax_engine_print_script_json(ax->vm, ax->result, ax->exit_code, m.f);
  return mem_close(&m);
}

// ---- simulation -------------------------------------------------------------------------------

void axiom_set_input(axiom *ax, double move_x, double move_y, int jump, int fire) {
  if (!ax->vm->world) ax_engine_init(ax->vm, ax->source);
  ax_engine_set_input(ax->vm, move_x, move_y, jump != 0, fire != 0);
}

static void step_body(axiom *ax, void *arg) {
  int frames = *(int *)arg;
  for (int i = 0; i < frames; i++) {
    ax->frames++;
    ax_engine_update(ax->vm, 1.0 / 60);
  }
}

int axiom_step(axiom *ax, int frames) {
  set_error(ax, NULL);
  if (ax->exited) return 1;
  if (!ax->vm->world) ax_engine_init(ax->vm, ax->source);
  return guarded(ax, step_body, &frames) == EXITED ? 1 : 0;
}

char *axiom_state(axiom *ax) {
  if (!ax->vm->world) ax_engine_init(ax->vm, ax->source);
  Mem m;
  if (!mem_open(&m)) return NULL;
  ax_engine_print_json(ax->vm, ax->frames, m.f);
  return mem_close(&m);
}

unsigned char *axiom_render(axiom *ax, int width, int height) {
  if (width <= 0 || height <= 0 || width > 8192 || height > 8192) return NULL;
  if (!ax->vm->world) ax_engine_init(ax->vm, ax->source);
  return ax_render_frame(ax->vm, width, height);
}

// ---- REPL ---------------------------------------------------------------------------------------

typedef struct { const char *code; char **value; int errors; } EvalArgs;

static void eval_body(axiom *ax, void *arg) {
  EvalArgs *e = arg;
  e->errors = ax_repl_eval(ax->vm, e->code, e->value);
}

int axiom_eval(axiom *ax, const char *code, char **value) {
  set_error(ax, NULL);
  if (value) *value = NULL;
  if (ax->exited) return 0;
  EvalArgs e = { code ? code : "", value, 0 };
  int outcome = guarded(ax, eval_body, &e);
  if (outcome == THREW) return 1;
  if (e.errors && !ax->error) set_error(ax, ax_repl_error());
  return e.errors;
}
