// apitest.c — drives every entry point of the embedding API (axiom_api.h) as a C host would.
// Build and run: make apitest. Prints one line per check and exits 1 if any failed.

#include "axiom_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures, checks;

static void expect(int ok, const char *what) {
  checks++;
  if (!ok) failures++;
  printf("%s %s\n", ok ? "OK  " : "FAIL", what);
}

// Output capture: stream 1 and stream 2 into separate buffers.
typedef struct { char out[8192], err[8192]; } Cap;
static void capture(void *user, int stream, const char *data, size_t len) {
  Cap *c = user;
  char *b = stream == 2 ? c->err : c->out;
  size_t n = strlen(b);
  if (n + len >= sizeof c->out) len = sizeof c->out - n - 1;
  memcpy(b + n, data, len);
  b[n + len] = '\0';
}

static axiom *fresh(Cap *c) {
  memset(c, 0, sizeof *c);
  axiom *ax = axiom_new();
  axiom_set_output(ax, capture, c);
  return ax;
}

// A host function: the sum of its numeric arguments.
static char *host_sum(axiom *ax, void *user, const char *args) {
  (void)ax;
  int *calls = user;
  (*calls)++;
  double total = 0;
  for (const char *p = args; *p; ) {
    char *end;
    double d = strtod(p, &end);
    if (end != p) { total += d; p = end; } else p++;
  }
  char *out = malloc(64);
  snprintf(out, 64, "{\"total\": %g, \"args\": %s}", total, args);
  return out;
}

static char *host_fail(axiom *ax, void *user, const char *args) {
  (void)user; (void)args;
  axiom_host_error(ax, "E_HOST", "the host said no");
  return NULL;
}

static char *host_bad(axiom *ax, void *user, const char *args) {
  (void)ax; (void)user; (void)args;
  return strdup("{not json");
}

int main(void) {
  Cap c;
  printf("axiom %s\n", axiom_version());

  // Run a script; output goes to the writer, whole lines per call.
  axiom *ax = fresh(&c);
  int code = axiom_run(ax, "^main:\n  print(\"hi\", 1 + 2)\n  eprint(\"to stderr\")\n", "hello.ax");
  expect(code == 0, "run: exit code 0");
  expect(!strcmp(c.out, "hi 3\n"), "run: print reaches stream 1");
  expect(!strcmp(c.err, "to stderr\n"), "run: eprint reaches stream 2");
  expect(axiom_error(ax) == NULL, "run: no error");
  char *res = axiom_result(ax);
  expect(res && strstr(res, "\"exit_code\": 0"), "result: script --json shape");
  axiom_release(res);
  axiom_free(ax);

  // exit(n) ends the program, not the host.
  ax = fresh(&c);
  code = axiom_run(ax, "^main:\n  print(\"a\")\n  exit(3)\n  print(\"b\")\n", NULL);
  expect(code == 3 && axiom_exit_code(ax) == 3, "exit(3): returns 3, host keeps running");
  expect(!strcmp(c.out, "a\n"), "exit(3): nothing after it runs");
  axiom_free(ax);

  // A numeric ^main result is the exit code, as in the command.
  ax = fresh(&c);
  code = axiom_run(ax, "^main:\n  ^return 7\n", NULL);
  expect(code == 7, "^return 7 from ^main: exit code 7");
  res = axiom_result(ax);
  expect(res && strstr(res, "\"main_result\": 7"), "result: main_result 7");
  axiom_release(res);
  axiom_free(ax);

  // A runtime error: exit code 1, located, and in axiom_error.
  ax = fresh(&c);
  code = axiom_run(ax, "^main:\n  x = null\n  print(x.y.z)\n", "bad.ax");
  expect(code == 1, "runtime error: exit code 1");
  expect(strstr(c.err, "bad.ax:3: runtime error [") != NULL, "runtime error: reported at its line");
  expect(axiom_error(ax) && strncmp(axiom_error(ax), "AX-", 3) == 0, "runtime error: axiom_error names the code");
  axiom_free(ax);

  // A program that does not compile never runs.
  ax = fresh(&c);
  code = axiom_run(ax, "^main:\n  print(\"never\"\n", "broken.ax");
  expect(code == 1, "compile failure: exit code 1");
  expect(!strstr(c.out, "never") && strstr(c.err, "compile failed:"), "compile failure: diagnostics on stream 2");
  expect(axiom_error(ax) && strstr(axiom_error(ax), "AX-PARSE-000"), "compile failure: axiom_error has the diagnostics");
  axiom_free(ax);

  // Static check.
  char *chk = axiom_check("^main:\n  print(1)\n", "ok.ax");
  expect(chk && strstr(chk, "\"ok\": true"), "check: clean program");
  axiom_release(chk);
  chk = axiom_check("^main:\n  print((\n", NULL);
  expect(chk && strstr(chk, "\"ok\": false") && strstr(chk, "AX-PARSE-000"), "check: parse error");
  axiom_release(chk);

  // Host functions: JSON in, JSON out, errors catchable.
  ax = fresh(&c);
  int calls = 0;
  expect(axiom_define(ax, "host_sum", host_sum, &calls) == 0, "define: host_sum");
  expect(axiom_define(ax, "not a name", host_sum, &calls) == 1, "define: rejects a non-identifier");
  axiom_define(ax, "host_fail", host_fail, NULL);
  axiom_define(ax, "host_bad", host_bad, NULL);
  code = axiom_run(ax,
    "^main:\n"
    "  r = host_sum(1, 2, 3.5)\n"
    "  print(r.total, len(r.args))\n"
    "  print([4, 5].host_sum().total)\n"
    "  ^try:\n"
    "    host_fail()\n"
    "  ^catch e:\n"
    "    print(\"caught\", e.code, e.msg)\n"
    "  ^try:\n"
    "    host_bad()\n"
    "  ^catch e:\n"
    "    print(\"caught\", e.code)\n", "host.ax");
  expect(code == 0, "host: program ran");
  expect(strstr(c.out, "6.5 3\n") != NULL, "host: arguments and result cross as JSON");
  expect(strstr(c.out, "9\n") != NULL, "host: callable as a method (uniform call syntax)");
  expect(strstr(c.out, "caught E_HOST the host said no\n") != NULL, "host: axiom_host_error raises a catchable error");
  expect(strstr(c.out, "caught AX-HOST-001\n") != NULL, "host: invalid JSON is an AX-HOST-001 error");
  expect(calls == 2, "host: called twice");
  axiom_free(ax);

  // REPL semantics.
  ax = fresh(&c);
  char *v = NULL;
  int errs = axiom_eval(ax, "x = 20\n", &v);
  expect(errs == 0 && v == NULL, "eval: a statement echoes nothing");
  errs = axiom_eval(ax, "x * 2 + 2", &v);
  expect(errs == 0 && v && !strcmp(v, "42"), "eval: an expression returns its value");
  axiom_release(v);
  errs = axiom_eval(ax, "^fn sq(n) = n * n\nsq(x)\n\"s\"", &v);
  expect(errs == 0 && v && !strcmp(v, "400\n\"s\""), "eval: declarations join; several values, strings quoted");
  axiom_release(v);
  errs = axiom_eval(ax, "nope(1)", &v);
  expect(errs == 1 && v == NULL && axiom_error(ax) && strstr(c.err, "error ["), "eval: an error is counted and reported");
  errs = axiom_eval(ax, "x", &v);
  expect(errs == 0 && v && !strcmp(v, "20"), "eval: the session survives an error");
  axiom_release(v);
  axiom_free(ax);

  // Simulation: load, step with input, state, render.
  ax = fresh(&c);
  const char *world =
    "@Mover\n"
    "  ~x: 0\n"
    "  ~jumps: 0\n"
    "  &physics:\n"
    "    x += input.move.x\n"
    "    ?input.jump: jumps += 1\n"
    "@Ball &Body3D at v3(0, 5, 0)\n"
    "  ~mass: 1\n"
    "  ~hit: sphere(0.5)\n";
  expect(axiom_load(ax, world, "world.ax") == 0, "load: an entity program");
  expect(axiom_main(ax) == 0, "main: none declared, nothing to run");
  axiom_set_input(ax, 1, 0, 1, 0);
  expect(axiom_step(ax, 30) == 0, "step: 30 frames");
  axiom_set_input(ax, 0, 0, 0, 0);
  axiom_step(ax, 30);
  char *st = axiom_state(ax);
  expect(st && strstr(st, "\"frames_run\": 60"), "state: counts the frames stepped");
  expect(st && strstr(st, "\"x\": 30") && strstr(st, "\"jumps\": 30"), "state: input drove the program");
  axiom_release(st);
  unsigned char *px = axiom_render(ax, 64, 48);
  int lit = 0;
  for (int i = 0; px && i < 64 * 48 * 4; i += 4) if (px[i] || px[i + 1] || px[i + 2]) lit++;
  expect(px && lit > 0, "render: RGBA frame");
  axiom_release(px);
  expect(axiom_load(ax, world, "again.ax") == 1, "load: one program per instance");
  axiom_free(ax);

  // exit() from a frame block stops stepping.
  ax = fresh(&c);
  axiom_load(ax, "@A\n  ~n: 0\n  &tick(60hz):\n    n += 1\n    ?n == 3: exit(4)\n", NULL);
  int stopped = axiom_step(ax, 10);
  expect(stopped == 1 && axiom_exit_code(ax) == 4, "step: exit(4) in a frame block stops the run");
  expect(axiom_step(ax, 10) == 1, "step: nothing runs after exit");
  axiom_free(ax);

  // The sandbox is on by default.
  ax = fresh(&c);
  code = axiom_run(ax, "^main:\n  ^try:\n    read(\"/etc/hostname\")\n  ^catch e:\n    print(e.code)\n  ^try:\n    sh(\"true\")\n  ^catch e:\n    print(e.code)\n", NULL);
  expect(code == 0 && !strcmp(c.out, "AX-SANDBOX-001\nAX-SANDBOX-001\n"), "sandbox: reads and commands denied by default");
  axiom_free(ax);
  ax = fresh(&c);
  axiom_allow_read(ax, "/etc/hostname");
  code = axiom_run(ax, "^main:\n  print(len(read(\"/etc/hostname\")) >= 0)\n", NULL);
  expect(code == 0 && !strcmp(c.out, "true\n"), "sandbox: axiom_allow_read opens one path");
  axiom_free(ax);

  // Arguments.
  ax = fresh(&c);
  const char *argv[] = { "one", "two" };
  axiom_set_args(ax, 2, argv);
  axiom_run(ax, "^main(a):\n  print(a, args())\n", NULL);
  expect(!strcmp(c.out, "[one,two] [one,two]\n"), "args: ^main(a) and args()");
  axiom_free(ax);

  // Instances are independent, and many can come and go.
  Cap c2;
  axiom *a1 = fresh(&c), *a2 = fresh(&c2);
  axiom_eval(a1, "g = 1", NULL);
  axiom_eval(a2, "g = 2", NULL);
  char *v1 = NULL, *v2 = NULL;
  axiom_eval(a1, "g", &v1);
  axiom_eval(a2, "g", &v2);
  expect(v1 && v2 && !strcmp(v1, "1") && !strcmp(v2, "2"), "instances: separate globals");
  axiom_release(v1);
  axiom_release(v2);
  axiom_free(a1);
  axiom_free(a2);
  int ok = 1;
  for (int i = 0; i < 300 && ok; i++) {
    ax = fresh(&c);
    ok = axiom_run(ax, "^fn f(n) = n * 2\n^main:\n  xs = [1, 2, 3].map(f)\n  print(xs.sum())\n", NULL) == 0 && !strcmp(c.out, "12\n");
    if (!ok) printf("  iteration %d: out=%s err=%s\n", i, c.out, c.err);
    axiom_free(ax);
  }
  expect(ok, "instances: 300 created, run and freed");

  printf("%d/%d checks passed\n", checks - failures, checks);
  return failures ? 1 : 0;
}
