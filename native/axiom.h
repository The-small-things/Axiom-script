// axiom.h — AxiomScript native runtime (C11)
//
// This is the native implementation of the AxiomScript *language*: the part a program is
// written in (values, functions, closures, control flow, patterns, the standard library).
// The engine half — entities, frame blocks, the rasterizer, navmesh, audio — still lives in
// the JavaScript reference implementation; see native/README.md for the split and the reason.
//
// Design notes that the rest of the code depends on:
//
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

#define AX_VERSION "0.9.1"

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
} AxNodeKind;

typedef struct AxArm AxArm;

struct AxNode {
  uint8_t kind;
  uint8_t op;          // binary/unary operator, compound-assign op, …
  int line, col;
  double num;
  AxStr *str;          // identifier / literal / field name (interned where it is a name)
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
};

typedef struct {
  AxNode *program;
  char err[512];
  int err_line;
  int nerrors;
} AxParseResult;

bool ax_parse(AxTokens *toks, AxParseResult *out);
void ax_ast_free_all(void);   // frees the whole AST arena

// ============================================================================================
// Runtime
// ============================================================================================

struct AxScope {
  AxObj hdr;
  AxScope *parent;
  // Small open-addressed map keyed by interned name pointers.
  AxStr **keys;
  AxValue *vals;
  uint32_t cap, len;
  bool fn_root;
};

AxScope *ax_scope_new(AxScope *parent, bool fn_root);
void ax_scope_release(AxScope *s);
bool ax_scope_lookup(AxScope *s, AxStr *name, AxValue *out);   // out is +1
bool ax_scope_lookup_local(AxScope *s, AxStr *name, AxValue *out);   // one frame only
bool ax_scope_set_existing(AxScope *s, AxStr *name, AxValue v);
void ax_scope_declare(AxScope *s, AxStr *name, AxValue v);

#define AX_MAX_HANDLERS 64

struct AxVM {
  AxScope *globals;
  AxDict *types;        // ^type name → array of field names
  AxDict *fns;          // ^fn/^proc name → AX_FN value
  AxArr *argv;          // program arguments
  int exit_code;
  bool exiting;
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
};

AxVM *ax_vm_new(void);
void ax_vm_free(AxVM *vm);
void ax_stdlib_install(AxVM *vm);

// Raise a runtime error. Never returns.
void ax_throw(AxVM *vm, const char *code, const char *fmt, ...);
void ax_throw_value(AxVM *vm, AxValue v);

// Execution
AxValue ax_eval(AxVM *vm, AxNode *n, AxScope *scope);              // returns +1
int ax_exec(AxVM *vm, AxNode *stmt, AxScope *scope, AxValue *out); // returns AX_FLOW_*
AxValue ax_call(AxVM *vm, AxValue fn, AxValue *args, int argc);    // returns +1
bool ax_run_program(AxVM *vm, AxNode *program, AxArr *argv, AxValue *result);

enum { AX_FLOW_NORMAL = 0, AX_FLOW_BREAK, AX_FLOW_CONTINUE, AX_FLOW_RETURN };

// Shared helpers used by the standard library.
AxValue ax_method_call(AxVM *vm, AxValue obj, AxStr *name, AxValue *args, int argc);
AxValue ax_index_get(AxVM *vm, AxValue obj, AxValue idx);
AxArr *ax_to_seq(AxVM *vm, AxValue v);   // array view of any iterable; returns +1
AxValue ax_key_apply(AxVM *vm, AxValue sel, AxValue item, double index);
bool ax_is_callable(AxValue v);

#endif // AXIOM_H
