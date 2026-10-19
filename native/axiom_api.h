// axiom_api.h — embed AxiomScript in a C program (or, built to WebAssembly, in JavaScript).
//
// One `axiom` is one program: its globals, its world and its output. Instances are independent
// but not thread-safe; use one thread per instance. Everything returns instead of exiting: a
// program's exit(n), a runtime error and a failed compile all come back as return values.
//
//   axiom *ax = axiom_new();
//   int code = axiom_run(ax, "^main:\n  print(1 + 2)\n", "demo.ax");   // prints 3, code 0
//   axiom_free(ax);
//
// Strings and buffers the API returns are the caller's, released with axiom_release. Strings passed in
// are copied (or only read during the call). All JSON uses the formats the command prints:
// axiom_check → `axiom --check --json`, axiom_result → a script's `--json`, axiom_state →
// `--sim N --json`.
//
// An instance starts SANDBOXED: no file reads or writes and no subprocesses until the host
// allows them (axiom_allow_read / axiom_allow_write / axiom_sandbox).

#ifndef AXIOM_API_H
#define AXIOM_API_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct axiom axiom;

// Output from print() and eprint() (and the console copy of !log): stream 1 or 2. Each call
// carries whole lines. Without a writer, output goes to stdout / stderr.
typedef void (*axiom_write_fn)(void *user, int stream, const char *data, size_t len);

// A host function callable from AxiomScript. `args_json` is the JSON array of the arguments.
// Return a JSON value in a string from malloc (the runtime frees it), or NULL for null. To
// raise an AxiomScript error instead (catchable with ^try), call axiom_host_error and return
// NULL.
typedef char *(*axiom_host_fn)(axiom *ax, void *user, const char *args_json);

const char *axiom_version(void);

axiom *axiom_new(void);
void axiom_free(axiom *ax);

void axiom_set_output(axiom *ax, axiom_write_fn fn, void *user);
void axiom_set_args(axiom *ax, int argc, const char *const *argv);   // args()
void axiom_sandbox(axiom *ax, int on);                                // on by default
void axiom_allow_read(axiom *ax, const char *path);                   // a file or directory
void axiom_allow_write(axiom *ax, const char *path);
void axiom_allow_exec(axiom *ax, int on);                             // sh()

// Define (or replace) a global function `name`, callable from then on. 0 on success, 1 for a
// name that is not an identifier.
int axiom_define(axiom *ax, const char *name, axiom_host_fn fn, void *user);
void axiom_host_error(axiom *ax, const char *code, const char *message);

// Static check without running: {"ok": bool, "diagnostics": [...]}. `filename` may be NULL.
char *axiom_check(const char *source, const char *filename);

// Compile and load a program (declarations, globals, entities) without running ^main.
// 0 on success; 1 when it does not compile (the diagnostics are written to stream 2 and kept
// for axiom_error) or an instance already holds a program.
int axiom_load(axiom *ax, const char *source, const char *filename);

// Run the loaded program's ^main. Returns the exit code, as the command would: exit(n), a
// numeric ^main result, 1 for a runtime error (written to stream 2), else 0.
int axiom_main(axiom *ax);

// axiom_load + axiom_main.
int axiom_run(axiom *ax, const char *source, const char *filename);

// The last run's result in `--json` script form: {main_result, log, diagnostics, exit_code}.
char *axiom_result(axiom *ax);

// Simulation: advance `frames` 60 Hz frames with the given input held. Returns 0, or 1 once the
// program has called exit(n) (see axiom_exit_code; further steps do nothing).
void axiom_set_input(axiom *ax, double move_x, double move_y, int jump, int fire);
int axiom_step(axiom *ax, int frames);
char *axiom_state(axiom *ax);      // the `--sim --json` state after the frames stepped so far
int axiom_exit_code(axiom *ax);

// The current frame drawn by the software renderer: width*height RGBA bytes, top row first.
unsigned char *axiom_render(axiom *ax, int width, int height);

// REPL semantics: statements run at once, declarations join the session, a bare expression's
// value is returned (strings quoted; several joined by newlines; NULL when none). Works with or
// without a loaded program. Returns the number of errors (written to stream 2).
int axiom_eval(axiom *ax, const char *code, char **value);

// What went wrong in the last call that failed: "CODE: message" for a runtime error, the
// diagnostics for a failed compile. NULL when the last call succeeded. Owned by the instance,
// valid until the next call.
const char *axiom_error(axiom *ax);

void axiom_release(void *p);

#ifdef __cplusplus
}
#endif

#endif
