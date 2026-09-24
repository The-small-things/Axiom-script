// repl.c — `axiom --repl`, the same session as repl.js.
//
// A complete line runs at once (so a harness can write a line and read the answer); a line that
// opens a block (ends in ':'), leaves a bracket or a """ open, or starts an @Entity keeps
// reading until a blank line or the next unindented line that is not its own continuation
// (?!, else, elif, ^catch, ^fin). A bare expression echoes its value — strings quoted — and
// nothing else echoes. Declarations join the session; everything else runs as if in ^main,
// except that its names are globals, so a function declared later sees them. Errors print `error [CODE]: message` to stderr and the session goes
// on. Commands: :step N, :help, :quit.

#include "axiom.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <setjmp.h>

bool ax_imports_resolve(AxNode *program, const char *from_path, char *err, size_t errn);

static AxVM *VM;
static AxScope *SCOPE;
static int seen_diags;
static bool interactive;
static jmp_buf exit_target;

typedef struct { char *buf; size_t len, cap; } Buf;
static void buf_add(Buf *b, const char *s, size_t n) {
  if (b->len + n + 1 > b->cap) {
    while (b->len + n + 1 > b->cap) b->cap = b->cap ? b->cap * 2 : 256;
    b->buf = realloc(b->buf, b->cap);
  }
  memcpy(b->buf + b->len, s, n);
  b->len += n;
  b->buf[b->len] = '\0';
}
static void buf_str(Buf *b, const char *s) { buf_add(b, s, strlen(s)); }

static void fail(const char *code, const char *msg) {
  fflush(stdout);
  fprintf(stderr, "error [%s]: %s\n", code, msg);
  fflush(stderr);
}

// Bracket depth > 0 or an open """ → `open`; `colon` when the last significant character
// outside strings and // comments is ':'. (repl.js scan)
static void scan(const char *t, bool *open, bool *colon) {
  int depth = 0;
  bool triple = false;
  char last = 0;
  size_t n = strlen(t);
  for (size_t i = 0; i < n; i++) {
    char c = t[i];
    if (triple) {
      if (c == '"' && !strncmp(t + i, "\"\"\"", 3)) { triple = false; i += 2; last = '"'; }
      continue;
    }
    if (c == '"' && !strncmp(t + i, "\"\"\"", 3)) { triple = true; i += 2; continue; }
    if (c == '"' || c == '\'') {
      char q = c;
      for (i++; i < n && t[i] != q && t[i] != '\n'; i++) if (t[i] == '\\') i++;
      last = q;
      continue;
    }
    if (c == '/' && i + 1 < n && t[i + 1] == '/') { while (i < n && t[i] != '\n') i++; i--; continue; }
    if (c == '(' || c == '[' || c == '{') depth++;
    else if (c == ')' || c == ']' || c == '}') depth--;
    if (c != ' ' && c != '\t' && c != '\n' && c != '\r') last = c;
  }
  *open = depth > 0 || triple;
  *colon = last == ':';
}

static bool word_at(const char *s, const char *w) {
  size_t n = strlen(w);
  return !strncmp(s, w, n) && !(isalnum((unsigned char)s[n]) || s[n] == '_');
}

static bool is_decl(const char *s) {
  if (s[0] == '^') {
    static const char *kw[] = { "fn", "proc", "type", "use", "event", "mix", "mat" };
    for (size_t i = 0; i < sizeof kw / sizeof kw[0]; i++) if (word_at(s + 1, kw[i])) return true;
    return false;
  }
  if (s[0] == '@') return isalpha((unsigned char)s[1]) || s[1] == '_';
  if (s[0] == '#') {
    const char *p = s + 1;
    if (!(isalpha((unsigned char)*p) || *p == '_')) return false;
    while (isalnum((unsigned char)*p) || *p == '_') p++;
    if (*p != ' ' && *p != '\t') return false;
    while (*p == ' ' || *p == '\t') p++;
    return isalpha((unsigned char)*p) || *p == '_';
  }
  if (s[0] == '~') {
    const char *p = s + 1;
    if (!(isalpha((unsigned char)*p) || *p == '_')) return false;
    while (isalnum((unsigned char)*p) || *p == '_') p++;
    if (*p == '!') p++;
    while (*p == ' ' || *p == '\t') p++;
    return *p == ':';
  }
  return false;
}

static bool is_cont(const char *s) {
  return !strncmp(s, "?!", 2) || word_at(s, "else") || word_at(s, "elif") || word_at(s, "^catch") || word_at(s, "^fin");
}

// Faults recorded rather than thrown: a global initializer, a frame block during :step.
static void flush_diags(void) {
  int n = ax_engine_diag_count(VM);
  for (; seen_diags < n; seen_diags++) {
    const char *code, *msg;
    if (ax_engine_diag_at(VM, seen_diags, &code, &msg)) fail(code, msg);
  }
}

static void report_error(void) {
  const char *code = VM->error_code[0] ? VM->error_code : "AX-RUNTIME-000";
  AxValue msg;
  if (VM->error.t == AX_DICT && ax_dict_get((AxDict *)VM->error.o, ax_internz("msg"), &msg)) {
    AxStr *s = ax_to_str(msg);
    fail(code, s->data);
    ax_release(ax_strv(s));
    ax_release(msg);
  } else {
    fail(code, VM->error_msg);
  }
  VM->error_code[0] = '\0';
}

static void show(AxValue v) {
  if (v.t == AX_NULL) return;
  if (v.t == AX_STR) {
    char *j = NULL;
    ax_json_write(v, 0, &j);
    printf("%s\n", j);
    free(j);
  } else {
    AxStr *s = ax_to_str(v);
    printf("%s\n", s->data);
    ax_release(ax_strv(s));
  }
  fflush(stdout);
}

// Parse `src`; NULL (after reporting, when `report`) on an error.
static AxNode *parse_src(const char *src, bool report) {
  AxTokens *toks = calloc(1, sizeof *toks);       // kept: the AST points into them
  AxParseResult *pr = calloc(1, sizeof *pr);
  if (!ax_tokenize(src, toks)) { if (report) fail("AX-PARSE-000", toks->err); return NULL; }
  if (!ax_parse(toks, pr)) { if (report) fail("AX-PARSE-000", pr->err); return NULL; }
  return pr->program;
}

static AxNode *find_main(AxNode *program) {
  for (int i = 0; i < program->nlist; i++) if (program->list[i]->kind == N_MAIN) return program->list[i];
  return NULL;
}

static void declare(const char *src) {
  Buf b = { 0 };
  buf_str(&b, src);
  buf_str(&b, "\n");
  AxNode *program = parse_src(b.buf, true);
  if (!program) return;
  char err[512] = { 0 };
  if (!ax_imports_resolve(program, "./<repl>.ax", err, sizeof err)) { fail("AX-USE-001", err); return; }
  ax_program_declare(VM, program);   // functions, types; a faulting global is recorded
  ax_engine_load(VM, program, NULL);
  flush_diags();
}

// Run the statements of ^main `m` in the session scope.
static void exec_main(AxNode *m, bool expr) {
  AxCtx saved = VM->ctx;
  VM->ctx.entity = NULL;
  VM->ctx.in_fn = true;
  VM->ctx.hot = false;
  VM->ctx.dt = 0;
  int hidx = VM->nhandlers++;
  int saved_depth = VM->call_depth;
  if (setjmp(VM->handlers[hidx]) == 0) {
    if (expr && m->nlist == 1 && m->list[0]->kind == N_EXPRSTMT) {
      AxValue v = ax_eval(VM, m->list[0]->a, SCOPE);
      show(v);
      ax_release(v);
    } else {
      for (int i = 0; i < m->nlist; i++) {
        AxValue out = ax_null();
        int flow = ax_exec(VM, m->list[i], SCOPE, &out);
        if (flow == AX_FLOW_RETURN) { show(out); ax_release(out); break; }
        ax_release(out);
      }
    }
    VM->nhandlers = hidx;
  } else {
    VM->nhandlers = hidx;
    VM->call_depth = saved_depth;
    report_error();
  }
  VM->ctx = saved;
  flush_diags();
}

static void run(const char *src) {
  Buf body = { 0 };
  // Indent every line of the chunk under a ^main.
  for (const char *p = src; *p; ) {
    const char *nl = strchr(p, '\n');
    size_t n = nl ? (size_t)(nl - p) : strlen(p);
    buf_str(&body, "  ");
    buf_add(&body, p, n);
    buf_str(&body, "\n");
    p += n + (nl ? 1 : 0);
  }
  // An expression first (`1 + 2`, `"hi"` are not statements), then statements. Lines that
  // start with a statement sigil are never read as expressions.
  if (!strchr("!^*?~@", src[0])) {
    Buf e = { 0 };
    buf_str(&e, "^main:\n  ^return ");
    buf_str(&e, body.buf + 2);
    AxNode *program = parse_src(e.buf, false);
    AxNode *m = program ? find_main(program) : NULL;
    if (m && m->nlist == 1) { exec_main(m, false); free(e.buf); free(body.buf); return; }
    free(e.buf);
  }
  Buf s = { 0 };
  buf_str(&s, "^main:\n");
  buf_str(&s, body.buf);
  AxNode *program = parse_src(s.buf, true);
  AxNode *m = program ? find_main(program) : NULL;
  if (m) exec_main(m, true);
  free(s.buf);
  free(body.buf);
}

static const char *HELP =
  "statements run at once; a bare expression prints its value\n"
  "blocks (a line ending in ':') end at a blank line\n"
  "declarations (^fn ^type ~G: @Entity ^use ...) join the session\n"
  ":step N   advance the simulation N frames      :quit   leave\n";

// ---- chunking --------------------------------------------------------------------------------

static Buf chunk;
static bool block;

static void flush_chunk(void) {
  if (!chunk.len) return;
  char *src = strdup(chunk.buf);
  chunk.len = 0;
  chunk.buf[0] = '\0';
  block = false;
  if (is_decl(src)) declare(src); else run(src);
  free(src);
}

static const char *last_line(void) {
  const char *nl = strrchr(chunk.buf, '\n');
  return nl ? nl + 1 : chunk.buf;
}

static void settle(void) {
  bool open, colon, lopen;
  scan(chunk.buf, &open, &colon);
  bool lcolon;
  scan(last_line(), &lopen, &lcolon);
  if (!open && lcolon) block = true;
  if (!open && !block) flush_chunk();
}

static void push_line(const char *line) {
  if (chunk.len) buf_str(&chunk, "\n");
  buf_str(&chunk, line);
}

// false → the session ends
static bool command(const char *line) {
  char cmd[32] = { 0 };
  long arg = 0;
  sscanf(line, "%31s %ld", cmd, &arg);
  if (!strcmp(cmd, ":q") || !strcmp(cmd, ":quit") || !strcmp(cmd, ":exit")) return false;
  if (!strcmp(cmd, ":help") || !strcmp(cmd, ":h")) { fputs(HELP, stdout); fflush(stdout); return true; }
  if (!strcmp(cmd, ":step")) {
    if (arg < 1) arg = 1;
    int hidx = VM->nhandlers++;
    if (setjmp(VM->handlers[hidx]) == 0) {
      for (long i = 0; i < arg; i++) ax_engine_update(VM, 1.0 / 60);
      VM->nhandlers = hidx;
    } else {
      VM->nhandlers = hidx;
      report_error();
    }
    flush_diags();
    return true;
  }
  char msg[96];
  snprintf(msg, sizeof msg, "unknown command %s (try :help)", cmd);
  fail("AX-REPL-001", msg);
  return true;
}

static bool feed(char *line) {
  size_t n = strlen(line);
  while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
  const char *t = line;
  while (*t == ' ' || *t == '\t') t++;
  bool blank = !*t || !strncmp(t, "//", 2);
  if (chunk.len) {
    bool open, colon;
    scan(chunk.buf, &open, &colon);
    if (open) { push_line(line); settle(); return true; }
    if (blank) { flush_chunk(); return true; }
    if (line[0] == ' ' || line[0] == '\t' || is_cont(line)) { push_line(line); settle(); return true; }
    flush_chunk();
  }
  if (blank) return true;
  if (line[0] == ':') return command(line);
  push_line(line);
  block = line[0] == '@';
  settle();
  return true;
}

int ax_repl(AxVM *vm) {
  VM = vm;
  interactive = isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
  ax_engine_init(vm, NULL);
  SCOPE = vm->globals;   // top-level names are session globals, visible to later ^fn bodies
  vm->exit_jmp = &exit_target;
  if (setjmp(exit_target)) {
    fflush(stdout);
    return vm->exit_code & 0xff;
  }
  char *line = NULL;
  size_t cap = 0;
  for (;;) {
    if (interactive) { fputs(chunk.len ? "... " : "> ", stdout); fflush(stdout); }
    if (getline(&line, &cap, stdin) < 0) break;
    if (!feed(line)) break;
  }
  flush_chunk();
  free(line);
  fflush(stdout);
  return 0;
}
