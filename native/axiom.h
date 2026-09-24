// axiom.h — AxiomScript native runtime (C11)
//
// This is the native implementation of the AxiomScript *language*: the part a program is
// written in (values, functions, closures, control flow, patterns, the standard library).
// It also hosts the engine: entities, frame blocks, physics, collisions, events, tweens and
// queries (engine.c). See native/README.md for what is ported and how it is verified.
//
// Design notes that the rest of the code depends on:
//
//   * The engine half (entities, frame blocks, physics, events) lives in engine.c and shares
//     this value model: vectors, quaternions, transforms and entities are ordinary refcounted
//     heap values, so a Vec3 held in two places is one object, exactly as in the reference.
//   * Values are 16 bytes: a tag plus a union. Numbers are doubles, as in the reference
//     implementation, so arithmetic agrees bit for bit.
//   * Heap objects are reference counted, with an object header shared by every type. The
//     convention, applied everywhere without exception: a function that RETURNS a Value returns
//     an owned reference (+1); a function that TAKES a Value borrows it (the caller still owns
//     it). `ax_release` on every temporary.
//   * Names are interned, so scope lookups compare pointers rather than strings.
//   * The AST lives in a bump arena and is freed in one call at exit — it never changes after
//     parsing, so per-node lifetime tracking would be pure overhead.
//   * Errors (`^throw`, a bad index, a sandbox denial) unwind with setjmp/longjmp; `^try`
//     pushes a handler. Break/continue/return are ordinary return codes, not jumps, because
//     they are common and must stay cheap.
//
// Cycles (a closure capturing the scope that holds it) are not collected. A CLI process exits
// long before that matters, and the alternative — a tracing collector over C stack roots — is
// a large amount of machinery for a language whose programs are short-lived.

#ifndef AXIOM_H
#define AXIOM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <setjmp.h>

#define AX_VERSION "0.9.2"

// WASI has no realpath: there, paths are compared as written.
#ifdef __wasi__
static inline char *ax_realpath(const char *path, char *out) { snprintf(out, 4096, "%s", path); return out; }
#else
#define ax_realpath realpath
#endif

// ============================================================================================
// Values
// ============================================================================================

typedef enum {
  AX_NULL = 0,
  AX_BOOL,
  AX_NUM,
  AX_STR,     // heap: AxStr
  AX_ATOM,    // heap: AxStr, but a distinct type (the language's symbol type)
  AX_ARR,     // heap: AxArr
  AX_DICT,    // heap: AxDict
  AX_FN,      // heap: AxFn   (closure or native function)
  AX_RANGE,   // heap: AxRange
  // engine values
  AX_VEC2,    // heap: AxVec (x, y)
  AX_VEC3,    // heap: AxVec (x, y, z)
  AX_QUAT,    // heap: AxVec (x, y, z, w)
  AX_MAT4,    // heap: AxMat4
  AX_XFORM,   // heap: AxXform — a pose: pos, rot, scl, vel
  AX_TIMER,   // heap: AxTimer — a field declared with a duration (`~cd: 0.5s`)
  AX_SHAPE,   // heap: AxShape — a collider (sphere/box/capsule)
  AX_ENTITY,  // heap: AxEntity
  AX_HOST,    // heap: AxHost — pools, bounded vectors/maps, distributions
} AxType;

typedef struct AxObj AxObj;
typedef struct AxStr AxStr;
typedef struct AxArr AxArr;
typedef struct AxDict AxDict;
typedef struct AxFn AxFn;
typedef struct AxRange AxRange;
typedef struct AxScope AxScope;
typedef struct AxVM AxVM;
typedef struct AxNode AxNode;

typedef struct {
  uint8_t t;
  union {
    double num;
    bool b;
    AxObj *o;
  };
} AxValue;

struct AxObj {
  uint32_t rc;
  uint8_t type;   // AxType of the object (AX_STR … AX_RANGE)
};

struct AxStr {
  AxObj hdr;
  uint32_t len;
  uint32_t hash;
  bool interned;
  char data[];    // NUL-terminated for convenience with C library calls
};

struct AxArr {
  AxObj hdr;
  uint32_t len, cap;
  AxValue *items;
};

// Insertion-ordered dictionary: `entries` preserves insertion order (so keys()/items() match
// the reference implementation), and an open-addressing index makes lookup O(1) once a dict
// grows past a handful of keys — word-frequency counting is a first-class use case.
typedef struct {
  AxStr *key;
  AxValue val;
  bool dead;
} AxDictEntry;

struct AxDict {
  AxObj hdr;
  AxDictEntry *entries;
  uint32_t len, cap, live;
  int32_t *index;        // open-addressed slots holding entry indices, -1 when empty
  uint32_t index_cap;
  AxStr *type_tag;       // `__type` for records built from a ^type, else NULL
};

// A native function receives its own AxFn, so a native can carry captured state in `bound`
// (which is what `partial`, `compose` and `memo` need to exist at all).
typedef AxValue (*AxNativeFn)(AxVM *vm, AxFn *self, AxValue *args, int argc);

struct AxFn {
  AxObj hdr;
  bool native;
  // native
  AxNativeFn fn;
  const char *name;
  int min_args, max_args;   // -1 for variadic
  // closure (and, for natives, captured state in `bound`)
  AxNode *body;             // expression (lambda) or statement list (declared fn)
  bool is_expr;
  AxStr **params;
  AxNode **defaults;        // parallel to params, NULL where absent
  int nparams;
  AxScope *scope;
  AxValue bound;            // optional captured first argument (partial application)
  bool has_bound;
  struct AxEntity *entity;  // a lambda remembers the entity it was created in
  bool uses_args;           // the body mentions `args`, so a call must bind it
};

struct AxRange {
  AxObj hdr;
  double lo, hi, step;
};

// ---- constructors / refcounting -------------------------------------------------------------

static inline AxValue ax_null(void) { AxValue v; v.t = AX_NULL; v.o = NULL; return v; }
static inline AxValue ax_bool(bool b) { AxValue v; v.t = AX_BOOL; v.b = b; return v; }
static inline AxValue ax_num(double d) { AxValue v; v.t = AX_NUM; v.num = d; return v; }
static inline bool ax_is_obj(AxValue v) { return v.t >= AX_STR; }

void ax_retain(AxValue v);
void ax_release(AxValue v);
static inline AxValue ax_copy(AxValue v) { ax_retain(v); return v; }

// Strings
AxStr *ax_str_new(const char *data, size_t len);
AxStr *ax_str_newz(const char *cstr);
AxStr *ax_intern(const char *data, size_t len);
AxStr *ax_internz(const char *cstr);
AxValue ax_strv(AxStr *s);                 // takes ownership of s
AxValue ax_str_from(const char *cstr);     // copies
AxValue ax_atom(AxStr *s);                 // takes ownership of s
AxValue ax_atomz(const char *cstr);
AxStr *ax_str_concat(AxStr *a, AxStr *b);
bool ax_str_eq(const AxStr *a, const AxStr *b);
uint32_t ax_hash_bytes(const char *p, size_t n);

// Arrays
AxArr *ax_arr_new(uint32_t cap);
AxValue ax_arrv(AxArr *a);
void ax_arr_push(AxArr *a, AxValue v);     // takes ownership of v
AxValue ax_arr_get(AxArr *a, int64_t i);   // borrowed → returns +1
void ax_arr_set(AxArr *a, int64_t i, AxValue v);

// Dicts
AxDict *ax_dict_new(void);
AxValue ax_dictv(AxDict *d);
void ax_dict_set(AxDict *d, AxStr *key, AxValue v);   // borrows key, takes v
bool ax_dict_get(AxDict *d, const AxStr *key, AxValue *out);  // out is +1
bool ax_dict_has(AxDict *d, const AxStr *key);
void ax_dict_del(AxDict *d, const AxStr *key);
uint32_t ax_dict_count(const AxDict *d);

// Functions
AxValue ax_native(const char *name, AxNativeFn fn, int min_args, int max_args);
AxValue ax_fnv(AxFn *f);

// Ranges
AxValue ax_range(double lo, double hi, double step);

// ---- predicates and conversions ---------------------------------------------------------------
bool ax_truthy(AxValue v);
bool ax_equals(AxValue a, AxValue b);          // structural for arrays/dicts, identity otherwise
int ax_compare(AxValue a, AxValue b);          // natural ordering (numbers numeric, else text)
AxStr *ax_to_str(AxValue v);                   // the f-string rendering; returns +1
void ax_fmt_num(double d, char *buf, size_t n); // JavaScript's Number#toString
void ax_str_append(char **buf, size_t *len, size_t *cap, const char *s, size_t n);
const char *ax_type_name(AxValue v);
double ax_to_num(AxValue v);

// ============================================================================================
// Lexer
// ============================================================================================

typedef enum {
  T_EOF = 0, T_NEWLINE, T_INDENT, T_DEDENT,
  T_NUM, T_STR, T_FSTR, T_IDENT,
  T_AT, T_TILDE, T_DOLLAR, T_AMP, T_BANG, T_BANGBANG, T_QUESTION, T_CARET, T_HASH,
  T_QMARKEQ,            // ?!  (else), only when followed by ':'
  T_ARROW,              // ->
  T_TILDEGT,            // ~>
  T_COLONCOLON,         // ::
  T_DOTDOT,             // ..
  T_COLON, T_LPAREN, T_RPAREN, T_LBRACKET, T_RBRACKET, T_COMMA, T_DOT,
  T_LBRACE, T_RBRACE, T_SEMI,
  T_PLUS, T_MINUS, T_STAR, T_SLASH, T_PERCENT, T_STARSTAR,
  T_PLUSEQ, T_MINUSEQ, T_STAREQ, T_SLASHEQ, T_PERCEQ, T_NULLCOALEQ,
  T_PLUSPLUS, T_MINUSMINUS,
  T_NULLCOAL, T_ANDAND, T_OROR,
  T_GT, T_LT, T_GE, T_LE, T_EQEQ, T_NE, T_ASSIGN, T_PIPE, T_PIPEGT,
  T_BACKSLASH, T_FATARROW,
  T_MIDDOT, T_CROSS,    // · (dot product)  × (cross product)
  T_QMARKGT,            // ?>  (raycast)
  T_TILDEEQ,            // ~=  (observe into a distribution)
} AxTokType;

typedef struct {
  AxTokType type;
  const char *start;   // into the (owned) source buffer, or an allocated payload for strings
  uint32_t len;
  double num;
  char unit[8];        // numeric literal suffix: f, v, s, ms, hz …
  int line, col;
  char *payload;       // decoded string/f-string contents (owned by the token list)
} AxTok;

typedef struct {
  AxTok *toks;
  int count, cap;
  char *src;           // owned copy of the source
  char *version;       // `axiom X.Y` pragma, or NULL
  char err[256];       // non-empty when tokenizing failed
  int err_line;
} AxTokens;

bool ax_tokenize(const char *src, AxTokens *out);
void ax_tokens_free(AxTokens *t);

// ============================================================================================
// AST
// ============================================================================================

typedef enum {
  // expressions
  N_NUM, N_STR, N_FSTR, N_IDENT, N_ATOMLIT,
  N_ARRAY, N_DICT, N_LAMBDA, N_CALL, N_CALLV, N_METHOD, N_MEMBER, N_INDEX,
  N_BINARY, N_UNARY, N_TERNARY, N_PIPE, N_COMPREHENSION, N_VALUE,
  // statements
  N_ASSIGN, N_DESTRUCTURE, N_MEMBER_ASSIGN, N_INDEX_ASSIGN, N_EXPRSTMT,
  N_IF, N_MATCH, N_WHILE, N_FOR, N_BREAK, N_CONTINUE, N_RETURN,
  N_TRY, N_THROW, N_ASSERT, N_BLOCK,
  // declarations
  N_FN, N_MAIN, N_TYPE, N_GLOBAL, N_USE, N_PROGRAM,
  // engine: expressions
  N_TAGREF,      // #Tag.a.b            names = path
  N_QUERY,       // ?name(args)         str = name, a = receiver or NULL, list = args
  N_INFER,       // dist ~> op          a = dist, str = op
  // engine: statements
  N_ACTION,      // !name(args)         str = name, list = args
  N_BROADCAST,   // ^Ev(args) to/within str = event, list = args, op = mode, str2 = target, a = radius, b = origin
  N_EMIT,        // ^emit a.b(args)     names = path, list = args
  N_TRANSITION,  // s -> t(args) if g   str = subject, list = clauses (str = target, list = args, a = guard)
  // engine: declarations
  N_ENTITY,      // @Name               str = name, str2 = base, names = mixins, a = initial pose, list = members
  N_FIELD,       // ~name: value        str = name, a = value, op = 0 '~' / 1 '$', b = pool type; $: a = shape, b = prior, c = infer
  N_EBLOCK,      // &name(freq|Event):  str = name, num = freq, str2 = event, list = body
  N_EVENT,       // ^event Name: f, g?  str = name, list = fields (N_IDENT, flag = optional)
  N_MIXIN,       // ^mix Name: members  str = name, list = members
  N_MATERIAL,    // ^mat Name: k: v     str = name, list = pairs
  N_RESOURCE,    // #Kind Name: "path"  str = kind, str2 = name, a = path (N_STR) or NULL, b = inline base64 (N_STR)
  N_FMT,         // {expr:spec} in an f-string   a = expr, str = spec
  N_POOLTYPE,    // &Pool(T, n)         str = Pool/Vec/Map, str2 = element (or key) type, names[0] = value type, num = capacity
} AxNodeKind;

typedef struct AxArm AxArm;

struct AxNode {
  uint8_t kind;
  uint8_t op;          // binary/unary operator, compound-assign op, …
  int line, col;
  double num;
  AxStr *str;          // identifier / literal / field name (interned where it is a name)
  AxStr *str2;         // a second name (entity base, event argument, broadcast target, …)
  AxNode *a, *b, *c;   // operands / subject / condition …
  AxNode **list;       // children (statements, elements, arguments)
  int nlist;
  AxStr **names;       // parameters, destructuring targets, loop variables
  int nnames;
  AxNode **defaults;   // parameter defaults (parallel to names)
  AxArm *arms;         // match arms
  int narms;
  bool flag;           // node-specific: is_expr lambda, named argument, …
};

struct AxArm {
  AxNode **patterns;   // NULL for `_`
  int npatterns;
  AxNode *guard;
  AxNode **body;
  int nbody;
};

// Binary/unary operator codes (also used for compound assignment).
enum {
  OP_NONE = 0, OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD, OP_POW,
  OP_GT, OP_LT, OP_GE, OP_LE, OP_EQ, OP_NE,
  OP_AND, OP_OR, OP_COALESCE, OP_IN, OP_RANGE, OP_NOT, OP_NEG,
  OP_DOT, OP_CROSS, OP_RAY,   // ·  ×  ?>
  OP_OBSERVE,                 // ~=  (statement-level, distributions)
};

typedef struct {
  AxNode *program;
  char err[512];
  int err_line, err_col;
  int nerrors;
} AxParseResult;

bool ax_parse(AxTokens *toks, AxParseResult *out);
void ax_ast_free_all(void);   // frees the whole AST arena
void *ax_arena_new(void);            // an AST arena of its own (the embedding API: one per instance)
void *ax_arena_swap(void *arena);    // make `arena` current (NULL: the default); returns the old
void ax_arena_free(void *arena);

// ============================================================================================
// Runtime
// ============================================================================================

struct AxScope {
  AxObj hdr;
  AxScope *parent;
  bool builtin;         // the library frame: readable, never assigned through
  // Small open-addressed map keyed by interned name pointers. The first AX_SCOPE_INLINE
  // bindings live in the frame itself — a call's parameters need no further allocation.
  AxStr **keys;
  AxValue *vals;
  uint32_t cap, len;
  bool fn_root;
  AxStr *ikeys[8];
  AxValue ivals[8];
};
#define AX_SCOPE_INLINE 8

AxScope *ax_scope_new(AxScope *parent, bool fn_root);
void ax_scope_release(AxScope *s);
void ax_scope_clear(AxScope *s);     // drop every binding (breaks function ↔ scope cycles)
bool ax_scope_lookup(AxScope *s, AxStr *name, AxValue *out);   // out is +1
bool ax_scope_lookup_local(AxScope *s, AxStr *name, AxValue *out);   // one frame only
bool ax_scope_set_existing(AxScope *s, AxStr *name, AxValue v);
void ax_scope_declare(AxScope *s, AxStr *name, AxValue v);

#define AX_MAX_HANDLERS 64

struct AxEntity;
struct AxWorld;

// What the evaluator is currently running on behalf of: an entity's frame block, a function
// called from one, or a script. It decides how a bare name resolves and where an assignment
// lands, which is the one place the engine and the language meet.
typedef struct {
  struct AxEntity *entity;   // the running entity, or NULL in a script
  double dt;                 // the block's timestep (0 outside a block)
  AxDict *payload;           // the event being handled by an &on block, or NULL
  AxStr *block;              // "physics", "tick", "on", "render", or NULL
  bool in_fn;                // inside a function body (assignments default to locals)
  bool hot;                  // inside a frame block (loops are budgeted)
} AxCtx;

struct AxVM {
  AxScope *builtins;    // the standard library and declared functions (read-only frame)
  AxScope *globals;     // program globals (`~NAME: v`), a child of builtins
  AxDict *types;        // ^type name → array of field names
  AxDict *fns;          // ^fn/^proc name → AX_FN value
  AxArr *argv;          // program arguments
  int exit_code;
  jmp_buf *exit_jmp;    // where exit() lands (the command's top level); NULL → exit the process
  // error handling
  jmp_buf handlers[AX_MAX_HANDLERS];
  int nhandlers;
  AxValue error;        // the value being thrown
  char error_msg[512];
  char error_code[32];
  // sandbox
  bool sandbox, allow_exec;
  AxArr *allow_read, *allow_write;
  // misc
  uint32_t rng_state;
  int call_depth;
  const char *source_path;
  // engine
  AxCtx ctx;
  struct AxWorld *world;   // NULL until a program with entities is loaded
  // output of print(), eprint() and the console copy of !log (stream 1 or 2); NULL → stdio
  void (*write)(void *user, int stream, const char *data, size_t len);
  void *write_user;
  void *embed;             // the embedding API's handle (api.c), or NULL
  int repl_seen;           // diagnostics a REPL session has already reported
  void *imports;           // files ^use has loaded (imports.c)
  // Scopes live on a stack, so an error unwinding past their C frames (longjmp) still
  // releases them: a throw releases every scope above its handler's mark.
  AxScope **live;
  int nlive, caplive;
  int handler_live[AX_MAX_HANDLERS];
};

void ax_live_grow(AxVM *vm);
void ax_unwind_scopes(AxVM *vm, int mark);
static inline AxScope *ax_scope_enter(AxVM *vm, AxScope *parent, bool fn_root) {
  AxScope *s = ax_scope_new(parent, fn_root);
  if (vm->nlive == vm->caplive) ax_live_grow(vm);
  vm->live[vm->nlive++] = s;
  return s;
}
static inline void ax_scope_exit(AxVM *vm, AxScope *s) {   // s is the innermost live scope
  vm->nlive--;
  ax_scope_release(s);
}
static inline int ax_handler_push(AxVM *vm) {
  int h = vm->nhandlers++;
  vm->handler_live[h] = vm->nlive;
  return h;
}

AxVM *ax_vm_new(void);
void ax_vm_free(AxVM *vm);
void ax_stdlib_install(AxVM *vm);

// Program output, through vm->write when an embedding host set one.
void ax_write(AxVM *vm, int stream, const char *data, size_t len);
void ax_write_line(AxVM *vm, int stream, const char *data, size_t len);   // appends '\n'

// Raise a runtime error. Never returns.
void ax_throw(AxVM *vm, const char *code, const char *fmt, ...);
void ax_throw_value(AxVM *vm, AxValue v);

// Execution
AxValue ax_eval(AxVM *vm, AxNode *n, AxScope *scope);              // returns +1
int ax_exec(AxVM *vm, AxNode *stmt, AxScope *scope, AxValue *out); // returns AX_FLOW_*
AxValue ax_call(AxVM *vm, AxValue fn, AxValue *args, int argc);    // returns +1
bool ax_run_program(AxVM *vm, AxNode *program, AxArr *argv, AxValue *result);
void ax_program_declare(AxVM *vm, AxNode *program);   // functions, types, globals
bool ax_run_main(AxVM *vm, AxNode *program, AxArr *argv, AxValue *result);   // false: ^main faulted (recorded)
int ax_to_int32(double x);                            // JavaScript's ToInt32 (`x | 0`)

enum { AX_FLOW_NORMAL = 0, AX_FLOW_BREAK, AX_FLOW_CONTINUE, AX_FLOW_RETURN };

// Shared helpers used by the standard library.
AxValue ax_method_call(AxVM *vm, AxValue obj, AxStr *name, AxValue *args, int argc);
AxValue ax_index_get(AxVM *vm, AxValue obj, AxValue idx);
AxArr *ax_to_seq(AxVM *vm, AxValue v);   // array view of any iterable; returns +1
AxValue ax_key_apply(AxVM *vm, AxValue sel, AxValue item, double index);
bool ax_is_callable(AxValue v);
AxValue ax_binary_op(AxVM *vm, int op, AxValue l, AxValue r);
AxValue ax_member_get(AxVM *vm, AxValue obj, AxStr *prop);
void ax_json_write(AxValue v, int indent, char **out);   // JSON.stringify-compatible; *out is malloc'd

// ============================================================================================
// Engine values
// ============================================================================================

typedef struct { AxObj hdr; double x, y, z, w; } AxVec;           // AX_VEC2 / AX_VEC3 / AX_QUAT
typedef struct { AxObj hdr; double d[16]; } AxMat4;               // column-major, as the reference
typedef struct AxXform {
  AxObj hdr;
  AxValue pos, rot, scl, vel;      // usually Vec3/Quat/Vec3/Vec3 — assignable, so any value
  struct AxEntity *entity;         // the entity whose pose this is (weak), for parenting
} AxXform;
typedef struct { AxObj hdr; double remaining, total; char unit[4]; } AxTimer;
typedef struct { AxObj hdr; AxStr *kind; AxValue params[2]; int nparams; } AxShape;

// Host objects the engine owns whose behaviour lives in engine.c (pools, bounded containers,
// distributions). `kind` selects the implementation; `free` releases the payload.
typedef struct AxHost {
  AxObj hdr;
  int kind;
  void *data;
  void (*free)(struct AxHost *);
} AxHost;

typedef struct AxEntity {
  AxObj hdr;
  AxStr *name;           // decl name; a spawned copy is `Name_N`
  AxStr *prefab;         // the declaration it came from (`Name` for a spawned copy too)
  AxStr *base;           // `&Base`, or NULL
  AxNode **members;      // composed members (mixins first, then the entity's own)
  int nmembers;
  AxDict *fields;        // declared `~fields`, in declaration order
  AxDict *locals;        // everything else assigned in a block (and `pose`)
  bool nosave, pending_remove;
  struct AxWorld *world;
  AxValue patrol;        // patrol_point(): the current target, re-drawn every 3 s
  double patrol_t0;
} AxEntity;

AxValue ax_vec2(double x, double y);
AxValue ax_vec3(double x, double y, double z);
AxValue ax_quat(double x, double y, double z, double w);
AxValue ax_mat4_new(const double *d);        // NULL → zeros
AxValue ax_xform_new(void);                  // identity pose
AxValue ax_timer_new(double remaining, const char *unit);
static inline AxVec *ax_vecp(AxValue v) { return (AxVec *)v.o; }
static inline bool ax_is_vec(AxValue v) { return v.t == AX_VEC2 || v.t == AX_VEC3; }

// engine.c
void ax_engine_install(AxVM *vm);                          // engine intrinsics (v3, sphere, …)
void ax_engine_init(AxVM *vm, const char *source);   // the world, before globals run (they may log)
void ax_engine_free(AxVM *vm);                       // the world, for an embedding host
bool ax_engine_load(AxVM *vm, AxNode *program, const char *source);   // true if it declares entities
void ax_engine_fault(AxVM *vm, const char *block, AxNode *stmt);      // record the pending error
int  ax_engine_last_fault(AxVM *vm, char *code, size_t coden, char *msg, size_t msgn);   // → its line
void ax_engine_print_script_json(AxVM *vm, AxValue result, int code, FILE *out);
bool ax_engine_diag_at(AxVM *vm, int i, const char **code, const char **msg);   // raw message
int  ax_repl(AxVM *vm);                                                          // repl.c
int  ax_repl_eval(AxVM *vm, const char *code, char **echo);                  // → errors reported
const char *ax_repl_error(void);                                             // the latest, or NULL

// ^use (imports.c): splice imported declarations into `program`. `sandboxed` denies files the
// VM's allow-read list does not cover.
void ax_imports_seed(AxVM *vm, const char *path);   // the entry file counts as loaded
bool ax_imports_resolve(AxVM *vm, AxNode *program, const char *from_path, bool sandboxed, char *err, size_t errn);
void ax_imports_free(AxVM *vm);
bool ax_sandbox_can_read(AxVM *vm, const char *path);

// check.c — the static checker (checker.js). A parse error, when given, is the only diagnostic.
typedef struct AxCheck AxCheck;
AxCheck *ax_check(AxNode *program, const char *source, const char *filename, const char *version,
                  const char *parse_err, int parse_line, int parse_col);
bool ax_check_ok(const AxCheck *c);            // nothing fatal or contract-violating
int  ax_check_count(const AxCheck *c);
int  ax_check_blocking(const AxCheck *c);
void ax_check_print(const AxCheck *c, FILE *out);        // `  [CODE] (severity) Lline:col message`
void ax_check_print_json(const AxCheck *c, FILE *out);   // {ok, diagnostics}
void ax_check_free(AxCheck *c);

// glb.c — binary glTF meshes. Vertices are interleaved float32 (pos xyz, normal xyz, uv xy).
typedef struct { float *verts; int nverts; uint16_t *idx; int nidx; AxValue material; } AxGlbPrim;
AxGlbPrim *ax_glb_parse(const uint8_t *buf, size_t n, int *nprims);   // NULL when the load fails
void ax_glb_free(AxGlbPrim *prims, int n);
bool ax_base64_decode(const char *s, size_t n, uint8_t **out, size_t *outn);
// The geometry registered for a #Mesh3D resource (engine.c), or NULL.
const AxGlbPrim *ax_world_mesh(AxVM *vm, AxStr *resource_name);
void ax_engine_update(AxVM *vm, double dt);
int  ax_engine_entity_count(AxVM *vm);
void ax_engine_summary(AxVM *vm, FILE *out);               // "N entities (A, B)"
void ax_engine_print_json(AxVM *vm, int frames, FILE *out);
int  ax_engine_diag_count(AxVM *vm);
int  ax_engine_fatal_count(AxVM *vm);                      // faults only, not advisories
void ax_engine_print_diags(AxVM *vm, FILE *out);
void ax_engine_log(AxVM *vm, const char *msg);             // print()/!log land here as well
bool ax_engine_log_msg(AxVM *vm, AxStr *msg);              // true → the console copy is suppressed
void ax_engine_quiet(AxVM *vm, bool quiet);                // --json: keep stdout for the JSON
void ax_engine_print_log_json(AxVM *vm, FILE *out, int depth);
AxArr *ax_host_seq(AxValue v);                             // iteration order of a container
double ax_host_len(AxValue v);
bool ax_host_contains(AxVM *vm, AxValue hay, AxValue needle);
AxValue ax_host_index(AxVM *vm, AxValue obj, AxValue idx);
bool ax_json_parse(const char *text, AxValue *out);
AxStr *ax_format_spec(AxValue v, const char *spec);        // f-string {x:spec}
bool ax_is_format_spec(const char *spec);
double ax_rng_next(AxVM *vm);                              // the seeded generator random() uses
double ax_js_hypot(int n, const double *vals);
AxValue ax_entity_pos(AxEntity *e);
struct AxXform *ax_entity_pose(AxEntity *e);
int ax_world_entities(AxVM *vm, AxEntity ***out);
AxDict *ax_world_channels(AxVM *vm);
AxDict *ax_world_saves(AxVM *vm);
const char *ax_world_version(AxVM *vm);
void ax_engine_set_version(AxVM *vm, const char *v);
void ax_world_diag(AxVM *vm, AxEntity *e, AxStr *block, const char *code, const char *severity, const char *title, const char *human);
void ax_engine_set_input(AxVM *vm, double mx, double my, bool jump, bool fire);

// Hooks the interpreter calls when it meets an engine construct.
AxValue ax_engine_eval(AxVM *vm, AxNode *n, AxScope *scope);            // N_TAGREF, N_QUERY, N_INFER
int  ax_engine_exec(AxVM *vm, AxNode *n, AxScope *scope, AxValue *out); // N_ACTION, N_BROADCAST, …
bool ax_engine_binary(AxVM *vm, int op, AxValue l, AxValue r, AxValue *out);  // vector arithmetic
bool ax_engine_member(AxVM *vm, AxValue obj, AxStr *prop, AxValue *out);
bool ax_engine_method(AxVM *vm, AxValue obj, AxStr *name, AxValue *args, int argc, AxValue *out);
bool ax_engine_set_member(AxVM *vm, AxValue obj, AxStr *prop, AxValue v);   // takes v
bool ax_entity_get(AxEntity *e, AxStr *name, AxValue *out);                 // out is +1
void ax_entity_set(AxEntity *e, AxStr *name, AxValue v);                    // takes v
void ax_render_engine(AxValue v, char **buf, size_t *len, size_t *cap);     // f-string form
AxValue ax_engine_input(AxVM *vm);                                          // the `input` record
AxValue ax_engine_resolve_tag(AxVM *vm, AxStr *tag);                        // live entity or null
int ax_engine_index_assign(AxVM *vm, AxValue target, AxValue idx, AxValue v, int op);  // takes v
void ax_host_free(AxHost *h);

#endif // AXIOM_H
