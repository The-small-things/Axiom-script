// api_js.c — the JavaScript side's entry points into the embedding API, for the reactor build
// (wasm/axiom-lib.wasm). JavaScript cannot hand C a function pointer, so output and host
// functions go through two imports from the "axiom" module, keyed by instance and id; wasm/
// axiom.mjs implements them.

#include "axiom_api.h"
#include <stdint.h>
#include <stdlib.h>

__attribute__((import_module("axiom"), import_name("write")))
extern void js_write(axiom *ax, int stream, const char *data, size_t len);

__attribute__((import_module("axiom"), import_name("host")))
extern char *js_host(axiom *ax, int id, const char *args_json);

static void write_to_js(void *user, int stream, const char *data, size_t len) {
  js_write((axiom *)user, stream, data, len);
}

static char *host_to_js(axiom *ax, void *user, const char *args_json) {
  return js_host(ax, (int)(intptr_t)user, args_json);
}

// An instance whose output goes to JavaScript.
axiom *axiom_js_new(void) {
  axiom *ax = axiom_new();
  if (ax) axiom_set_output(ax, write_to_js, ax);
  return ax;
}

// Host function `name` is JavaScript function number `id` (the instance's own table).
int axiom_js_define(axiom *ax, const char *name, int id) {
  return axiom_define(ax, name, host_to_js, (void *)(intptr_t)id);
}

// Memory for strings JavaScript passes in (and for a host function's result), freed with
// axiom_release.
void *axiom_js_alloc(size_t n) { return malloc(n ? n : 1); }

// axiom_eval with the error count stored at `errors` (so the value can be the return).
char *axiom_js_eval_value(axiom *ax, const char *code, int *errors) {
  char *value = NULL;
  *errors = axiom_eval(ax, code, &value);
  return value;
}
