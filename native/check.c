// check.c — the static checker, ported from checker.js: `axiom --check`, and the gate every
// program passes before it runs (a fatal or contract-violation diagnostic stops it, as in
// main.js).
//
// Every check walks the program the way the reference walks its own syntax tree — the same
// statements, in the same order, with the same helpers (stmtExprRoots, walkExpr, walkStmts,
// forEachEntityBlock, forEachBody) — because the diagnostics must come out identical: the same
// codes, locations and messages, in the same order, including the reference's quirks (a call
// nested in a conditional inside a function body is reported twice, because its statement
// list is both expanded by stmtExprRoots and recursed into; an untyped event field "expects
// 'null'"; an entity has no line). Expression nodes in the reference carry no position, so
// every diagnostic below is located at its statement.
//
// Tables that come from the reference (library names, subsystems, infer strategies, versions)
// are generated into checknames.h by tools/gen_checknames.js.

#include "axiom.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <limits.h>
#include <unistd.h>
#include "checknames.h"

bool ax_base64_decode(const char *s, size_t n, uint8_t **out, size_t *outn);

// ---- diagnostics ------------------------------------------------------------------------------

typedef struct {
  const char *code, *severity;
  char *entity, *block;       // NULL → null
  int line, col;              // 0 → null
  const char *rsec, *rtitle;
  char *snippet;              // NULL → null
  char *human, *agent;
  int fixk;                   // 0 null, 1 {kind:"text"}, 2 {kind:"patch"}, 3 a plain string
  char *fix;
  bool autofix;
} CD;

struct AxCheck {
  CD *v;
  int n, cap;
  bool ok;
};

static char *fmt(const char *f, ...) {
  va_list ap;
  va_start(ap, f);
  va_list ap2;
  va_copy(ap2, ap);
  int n = vsnprintf(NULL, 0, f, ap);
  va_end(ap);
  char *s = malloc((size_t)n + 1);
  vsnprintf(s, (size_t)n + 1, f, ap2);
  va_end(ap2);
  return s;
}
static char *sdup(const char *s) { return s ? strdup(s) : NULL; }

typedef struct {
  const char *src;
  const char *filename;
  AxCheck *out;
} Ck;

// sourceLine(source, line): the line, trimmed; NULL when there is none.
static char *source_line(Ck *c, int line) {
  if (line <= 0 || !c->src) return NULL;
  const char *p = c->src;
  for (int ln = 1; ln < line; ln++) { p = strchr(p, '\n'); if (!p) return NULL; p++; }
  const char *end = strchr(p, '\n');
  size_t n = end ? (size_t)(end - p) : strlen(p);
  while (n && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\v' || *p == '\f')) { p++; n--; }
  while (n && (p[n - 1] == ' ' || p[n - 1] == '\t' || p[n - 1] == '\r' || p[n - 1] == '\v' || p[n - 1] == '\f')) n--;
  char *s = malloc(n + 1);
  memcpy(s, p, n);
  s[n] = '\0';
  return s;
}

static CD *emit(Ck *c, const char *code, const char *sev) {
  AxCheck *o = c->out;
  if (o->n == o->cap) { o->cap = o->cap ? o->cap * 2 : 16; o->v = realloc(o->v, sizeof(CD) * o->cap); }
  CD *d = &o->v[o->n++];
  memset(d, 0, sizeof *d);
  d->code = code;
  d->severity = sev;
  return d;
}

// ---- the program as the reference sees it ----------------------------------------------------

typedef struct { AxNode **v; int n, cap; } NV;
static void nv_push(NV *l, AxNode *n) {
  if (l->n == l->cap) { l->cap = l->cap ? l->cap * 2 : 8; l->v = realloc(l->v, sizeof(AxNode *) * l->cap); }
  l->v[l->n++] = n;
}

typedef struct {
  NV entities, events, types, fns, procs, mixins, materials, globals, resources, uses;
  AxNode *main;
  const char *version;
} Prog;

static void prog_add(Prog *pg, AxNode *d) {
  switch (d->kind) {
    case N_ENTITY: nv_push(&pg->entities, d); break;
    case N_EVENT: nv_push(&pg->events, d); break;
    case N_TYPE: nv_push(&pg->types, d); break;
    case N_FN: nv_push(d->op == 1 ? &pg->procs : &pg->fns, d); break;
    case N_MIXIN: nv_push(&pg->mixins, d); break;
    case N_MATERIAL: nv_push(&pg->materials, d); break;
    case N_GLOBAL: nv_push(&pg->globals, d); break;
    case N_RESOURCE: nv_push(&pg->resources, d); break;
    case N_USE: nv_push(&pg->uses, d); break;
    case N_MAIN: pg->main = d; break;
    default: break;
  }
}

static bool streq(const AxStr *a, const char *b) { return a && b && strcmp(a->data, b) == 0; }
static const char *S(const AxStr *s) { return s ? s->data : NULL; }

static bool in_list(const char *const *xs, const char *s) {
  if (!s) return false;
  for (; *xs; xs++) if (!strcmp(*xs, s)) return true;
  return false;
}

static const CheckSubsystem *subsystem(const AxStr *base) {
  if (!base) return NULL;
  for (int i = 0; i < CHECK_SUBSYSTEMS_N; i++) if (!strcmp(CHECK_SUBSYSTEMS[i].name, base->data)) return &CHECK_SUBSYSTEMS[i];
  return NULL;
}

static bool known_name(const char *s) {
  for (int i = 0; i < CHECK_KNOWN_NAMES_N; i++) if (!strcmp(CHECK_KNOWN_NAMES[i], s)) return true;
  return false;
}

// An ordered set of names (JavaScript's Set: insertion order, no duplicates).
typedef struct { const char **v; int n, cap; } SSet;
static bool ss_has(const SSet *s, const char *x) {
  if (!x) return false;
  for (int i = 0; i < s->n; i++) if (!strcmp(s->v[i], x)) return true;
  return false;
}
static void ss_add(SSet *s, const char *x) {
  if (!x || ss_has(s, x)) return;
  if (s->n == s->cap) { s->cap = s->cap ? s->cap * 2 : 16; s->v = realloc(s->v, sizeof(char *) * s->cap); }
  s->v[s->n++] = x;
}
static void ss_free(SSet *s) { free(s->v); s->v = NULL; s->n = s->cap = 0; }

// Levenshtein distance, case-insensitive (checker.js levenshtein).
static int lev(const char *a, const char *b) {
  int m = (int)strlen(a), n = (int)strlen(b);
  if (m == 0) return n;
  if (n == 0) return m;
  int *prev = malloc(sizeof(int) * (n + 1)), *cur = malloc(sizeof(int) * (n + 1));
  for (int j = 0; j <= n; j++) prev[j] = j;
  for (int i = 1; i <= m; i++) {
    cur[0] = i;
    for (int j = 1; j <= n; j++) {
      char x = a[i - 1], y = b[j - 1];
      if (x >= 'A' && x <= 'Z') x += 32;
      if (y >= 'A' && y <= 'Z') y += 32;
      int cost = x == y ? 0 : 1;
      int v = prev[j] + 1;
      if (cur[j - 1] + 1 < v) v = cur[j - 1] + 1;
      if (prev[j - 1] + cost < v) v = prev[j - 1] + cost;
      cur[j] = v;
    }
    memcpy(prev, cur, sizeof(int) * (n + 1));
  }
  int r = prev[n];
  free(prev); free(cur);
  return r;
}

// ---- the reference's node vocabulary ---------------------------------------------------------

// Statement type names as parser.js spells them (they appear in one message).
static const char *stmt_type(const AxNode *st) {
  switch (st->kind) {
    case N_ASSIGN: return "Assign";
    case N_DESTRUCTURE: return "DestructureAssign";
    case N_MEMBER_ASSIGN:
      if (st->str2) return st->nnames == 1 ? "MemberAssign" : "DeepAssign";
      return st->nnames == 2 ? "MemberAssign" : "DeepAssign";
    case N_INDEX_ASSIGN: return "IndexAssign";
    case N_EXPRSTMT: return "ExprStmt";
    case N_IF: return "CondBlock";
    case N_WHILE: return "WhileLoop";
    case N_FOR: return "ForLoop";
    case N_BREAK: return "Break";
    case N_CONTINUE: return "Continue";
    case N_RETURN: return "Return";
    case N_TRY: return "Try";
    case N_THROW: return "Throw";
    case N_ASSERT: return "Assert";
    case N_MATCH: return "Match";
    case N_ACTION: return "Action";
    case N_BROADCAST: return "Broadcast";
    case N_EMIT: return "Emit";
    case N_TRANSITION: return "TransitionChain";
    default: return "Unknown";
  }
}

static const char *expr_type(const AxNode *n) {
  switch (n->kind) {
    case N_NUM: return "NumberLit";
    case N_STR: return "StringLit";
    case N_FSTR: return "FString";
    case N_IDENT: return "Ident";
    case N_ARRAY: return "ArrayLit";
    case N_DICT: return "DictLit";
    case N_LAMBDA: return "Lambda";
    case N_CALL: return "Call";
    case N_CALLV: return "CallValue";
    case N_METHOD: return "MethodCall";
    case N_MEMBER: return "Member";
    case N_INDEX: return "Index";
    case N_BINARY: return n->op == OP_COALESCE ? "NullCoalesce" : "Binary";
    case N_UNARY: return "Unary";
    case N_TERNARY: return "Ternary";
    case N_PIPE: return "Pipe";
    case N_COMPREHENSION: return "Comprehension";
    case N_QUERY: return "Query";
    case N_INFER: return "InferExpr";
    case N_TAGREF: return "TagRef";
    default: return "Unknown";
  }
}

// An argument node's value (call arguments are `name: value` pairs).
static AxNode *argv_of(AxNode *a) { return a ? a->b : NULL; }

// walkExpr(node, visit)
typedef void (*Visit)(AxNode *n, void *ctx);
static void walk_expr(AxNode *n, Visit visit, void *ctx) {
  if (!n) return;
  if (n->kind == N_FMT) { walk_expr(n->a, visit, ctx); return; }
  visit(n, ctx);
  switch (n->kind) {
    case N_MEMBER: walk_expr(n->a, visit, ctx); return;
    case N_INDEX: walk_expr(n->a, visit, ctx); walk_expr(n->b, visit, ctx); return;
    case N_UNARY: walk_expr(n->a, visit, ctx); return;
    case N_BINARY: walk_expr(n->a, visit, ctx); walk_expr(n->b, visit, ctx); return;
    case N_TERNARY: walk_expr(n->a, visit, ctx); walk_expr(n->b, visit, ctx); walk_expr(n->c, visit, ctx); return;
    case N_INFER: walk_expr(n->a, visit, ctx); return;
    case N_METHOD: walk_expr(n->a, visit, ctx); for (int i = 0; i < n->nlist; i++) walk_expr(argv_of(n->list[i]), visit, ctx); return;
    case N_CALL: for (int i = 0; i < n->nlist; i++) walk_expr(argv_of(n->list[i]), visit, ctx); return;
    case N_QUERY: walk_expr(n->a, visit, ctx); for (int i = 0; i < n->nlist; i++) walk_expr(argv_of(n->list[i]), visit, ctx); return;
    case N_ARRAY: for (int i = 0; i < n->nlist; i++) walk_expr(n->list[i], visit, ctx); return;
    case N_DICT: for (int i = 0; i < n->nlist; i++) walk_expr(n->list[i]->b, visit, ctx); return;
    case N_FSTR:
      for (int i = 0; i < n->nlist; i++) {
        AxNode *part = n->list[i];
        if (part->kind == N_STR && part->flag) continue;   // a literal part
        walk_expr(part, visit, ctx);
      }
      return;
    case N_COMPREHENSION: walk_expr(n->b, visit, ctx); if (n->c) walk_expr(n->c, visit, ctx); walk_expr(n->a, visit, ctx); return;
    case N_LAMBDA: walk_expr(n->a, visit, ctx); return;
    case N_PIPE: walk_expr(n->a, visit, ctx); walk_expr(n->b, visit, ctx); return;
    case N_CALLV: walk_expr(n->a, visit, ctx); for (int i = 0; i < n->nlist; i++) walk_expr(argv_of(n->list[i]), visit, ctx); return;
    default: return;
  }
}

// stmtExprRoots(stmt)
static void roots_of(AxNode *st, NV *out) {
  switch (st->kind) {
    case N_ASSIGN: nv_push(out, st->a); return;
    case N_INDEX_ASSIGN: nv_push(out, st->a); nv_push(out, st->b); return;
    case N_MEMBER_ASSIGN: nv_push(out, st->a); return;
    case N_EXPRSTMT: nv_push(out, st->a); return;
    case N_ACTION: case N_EMIT: for (int i = 0; i < st->nlist; i++) nv_push(out, argv_of(st->list[i])); return;
    case N_BROADCAST:
      for (int i = 0; i < st->nlist; i++) nv_push(out, argv_of(st->list[i]));
      if (st->a) nv_push(out, st->a);
      if (st->b) nv_push(out, st->b);
      return;
    case N_TRANSITION:
      for (int i = 0; i < st->nlist; i++) {
        AxNode *cl = st->list[i];
        if (cl->a) nv_push(out, cl->a);
        for (int k = 0; k < cl->nlist; k++) nv_push(out, argv_of(cl->list[k]));
      }
      return;
    case N_ASSERT: nv_push(out, st->a); return;
    case N_RETURN: nv_push(out, st->a); return;
    case N_IF:
      nv_push(out, st->a);
      if (st->b) nv_push(out, st->b);
      for (int i = 0; i < st->nlist; i++) roots_of(st->list[i], out);
      if (st->c) for (int i = 0; i < st->c->nlist; i++) roots_of(st->c->list[i], out);
      return;
    case N_WHILE: case N_FOR:
      nv_push(out, st->a);
      for (int i = 0; i < st->nlist; i++) roots_of(st->list[i], out);
      return;
    case N_MATCH:
      nv_push(out, st->a);
      for (int i = 0; i < st->narms; i++) {
        AxArm *arm = &st->arms[i];
        for (int k = 0; k < arm->npatterns; k++) nv_push(out, arm->patterns[k]);
        for (int k = 0; k < arm->nbody; k++) roots_of(arm->body[k], out);
      }
      return;
    case N_TRY:
      for (int i = 0; i < st->nlist; i++) roots_of(st->list[i], out);
      if (st->c) for (int i = 0; i < st->c->nlist; i++) roots_of(st->c->list[i], out);
      if (st->b) for (int i = 0; i < st->b->nlist; i++) roots_of(st->b->list[i], out);
      return;
    case N_THROW: if (st->a) nv_push(out, st->a); return;
    case N_DESTRUCTURE: nv_push(out, st->a); return;
    default: return;
  }
}

static void walk_roots(AxNode *st, Visit visit, void *ctx) {
  NV r = { 0 };
  roots_of(st, &r);
  for (int i = 0; i < r.n; i++) walk_expr(r.v[i], visit, ctx);
  free(r.v);
}

// The statement lists a statement holds, in the reference's key order (body, ifBody, elseBody,
// catchBody, finallyBody), then its match arms' bodies.
typedef void (*ListFn)(AxNode **stmts, int n, void *ctx);
static void each_sublist(AxNode *st, ListFn f, void *ctx) {
  switch (st->kind) {
    case N_WHILE: case N_FOR: f(st->list, st->nlist, ctx); break;
    case N_IF: f(st->list, st->nlist, ctx); if (st->c) f(st->c->list, st->c->nlist, ctx); break;
    case N_TRY: f(st->list, st->nlist, ctx); if (st->c) f(st->c->list, st->c->nlist, ctx); if (st->b) f(st->b->list, st->b->nlist, ctx); break;
    case N_MATCH: for (int i = 0; i < st->narms; i++) f(st->arms[i].body, st->arms[i].nbody, ctx); break;
    default: break;
  }
}

// walkStmts: every statement, descending into conditionals and loops only.
typedef void (*StmtFn)(AxNode *st, void *ctx);
static void walk_stmts(AxNode **stmts, int n, StmtFn f, void *ctx) {
  for (int i = 0; i < n; i++) {
    AxNode *st = stmts[i];
    f(st, ctx);
    if (st->kind == N_IF) {
      walk_stmts(st->list, st->nlist, f, ctx);
      if (st->c) walk_stmts(st->c->list, st->c->nlist, f, ctx);
    } else if (st->kind == N_WHILE || st->kind == N_FOR) {
      walk_stmts(st->list, st->nlist, f, ctx);
    }
  }
}

// forEachEntityBlock: every &block of every entity, nested entities included, in member order.
typedef void (*BlockFn)(AxNode *ent, AxNode *block, void *ctx);
static void entity_blocks(AxNode *ent, BlockFn f, void *ctx) {
  for (int i = 0; i < ent->nlist; i++) {
    AxNode *m = ent->list[i];
    if (m->kind == N_EBLOCK) f(ent, m, ctx);
    else if (m->kind == N_ENTITY) entity_blocks(m, f, ctx);
  }
}
static void for_each_block(Prog *pg, BlockFn f, void *ctx) {
  for (int i = 0; i < pg->entities.n; i++) entity_blocks(pg->entities.v[i], f, ctx);
}

static const char *event_arg(AxNode *block) { return (streq(block->str, "on") && block->str2) ? block->str2->data : NULL; }

// ---- §2.1.1 — the zero-allocation contract ---------------------------------------------------

typedef struct { Ck *c; AxNode *ent, *block, *st; } ZA;

static CD *emit_at(Ck *c, const char *code, const char *sev, AxNode *ent, AxNode *block, AxNode *st) {
  CD *d = emit(c, code, sev);
  d->entity = ent ? sdup(S(ent->str)) : NULL;
  d->block = block ? sdup(S(block->str)) : NULL;
  d->line = st ? st->line : 0;
  d->col = st ? st->col : 0;
  d->snippet = st ? source_line(c, st->line) : NULL;
  return d;
}

static void za_visit(AxNode *n, void *vctx) {
  ZA *z = vctx;
  const char *bn = z->block->str->data;
  if (n->kind == N_INFER) {
    CD *d = emit_at(z->c, "AX-KERNEL-001", "fatal", z->ent, z->block, z->st);
    d->rsec = "§2.3"; d->rtitle = "Axiom Channels — the D↔S Bridge";
    d->human = fmt("An S-Kernel infer op ('~> %s') was used inside a '&%s' block.", S(n->str), bn);
    d->agent = fmt("Move the '~> %s' expression into a '&tick' block.", S(n->str));
    d->fixk = 1; d->fix = fmt("Move the '~> %s' expression into a '&tick' block.", S(n->str));
  }
  if (n->kind == N_METHOD && (streq(n->str, "push") || streq(n->str, "append") || streq(n->str, "insert"))) {
    CD *d = emit_at(z->c, "AX-ALLOC-002", "contract_violation", z->ent, z->block, z->st);
    d->rsec = "§2.1.1"; d->rtitle = "The Zero-Allocation Contract";
    d->human = fmt("Growable-array '.%s(...)' inside a '&%s' block violates the zero-allocation contract.", S(n->str), bn);
    d->agent = fmt("Use a fixed-capacity pool or BVec instead.");
    d->fixk = 1; d->fix = fmt("Replace '.%s(...)' with a Pool/BVec operation.", S(n->str));
  }
  if (n->kind == N_BINARY && n->op == OP_ADD && ((n->a && n->a->kind == N_STR) || (n->b && n->b->kind == N_STR))) {
    CD *d = emit_at(z->c, "AX-ALLOC-003", "contract_violation", z->ent, z->block, z->st);
    d->rsec = "§2.1.1"; d->rtitle = "The Zero-Allocation Contract";
    d->human = fmt("Runtime string concatenation inside a '&%s' block.", bn);
    d->agent = fmt("Emit raw values on a channel instead.");
    d->fixk = 1; d->fix = fmt("Use '^emit' instead of string concatenation.");
  }
  if (n->kind == N_FSTR) {
    CD *d = emit_at(z->c, "AX-ALLOC-003", "contract_violation", z->ent, z->block, z->st);
    d->rsec = "§2.1.1"; d->rtitle = "The Zero-Allocation Contract";
    d->human = fmt("F-string allocation inside a '&%s' block.", bn);
    d->agent = fmt("Use !d() for debug logging in hot blocks, or move f-string to &tick.");
    d->fixk = 1; d->fix = fmt("Use '!d(...)' instead of f-strings in hot blocks.");
  }
  if (n->kind == N_CALL && streq(n->str, "print")) {
    CD *d = emit_at(z->c, "AX-ALLOC-003", "contract_violation", z->ent, z->block, z->st);
    d->rsec = "§2.1.1"; d->rtitle = "The Zero-Allocation Contract";
    d->human = fmt("print() call inside a '&%s' block allocates strings.", bn);
    d->agent = fmt("Use !d() for debug logging in hot blocks, or move print() to &tick.");
    d->fixk = 1; d->fix = fmt("Use '!d(...)' instead of print() in hot blocks.");
  }
  if (n->kind == N_CALL && streq(n->str, "spawn_new")) {
    CD *d = emit_at(z->c, "AX-ALLOC-001", "contract_violation", z->ent, z->block, z->st);
    d->rsec = "§2.1.1"; d->rtitle = "The Zero-Allocation Contract";
    d->human = fmt("Dynamic heap instantiation inside a '&%s' block.", bn);
    d->agent = fmt("Use a Pool/BVec instead.");
    d->fixk = 1; d->fix = fmt("Use '!spawn(pool, init)' instead.");
  }
}

static void za_block(AxNode *ent, AxNode *block, void *vctx) {
  Ck *c = vctx;
  if (!streq(block->str, "physics") && !streq(block->str, "render") && !streq(block->str, "on")) return;
  if (subsystem(ent->str2)) return;
  for (int i = 0; i < block->nlist; i++) {
    AxNode *st = block->list[i];
    if (st->kind == N_ACTION && (streq(st->str, "d") || streq(st->str, "print") || streq(st->str, "stop_anim"))) continue;
    if (st->kind == N_ASSIGN && st->op == OP_OBSERVE) {
      CD *d = emit_at(c, "AX-KERNEL-001", "fatal", ent, block, st);
      d->rsec = "§2.3"; d->rtitle = "Axiom Channels — the D↔S Bridge";
      d->human = fmt("An S-Kernel update ('~=') was called from a '&%s' block.", block->str->data);
      d->agent = fmt("Move this line into a '&tick' block.");
      d->fixk = 1; d->fix = fmt("Relocate the '~=' line to a '&tick' block.");
    }
    ZA z = { c, ent, block, st };
    walk_roots(st, za_visit, &z);
  }
}

// ---- §1.6 — the sealed body ------------------------------------------------------------------

static bool plain_read(AxNode *n) {
  if (!n) return false;
  if (n->kind == N_NUM || n->kind == N_STR || n->kind == N_IDENT || n->kind == N_TAGREF) return true;
  if (n->kind == N_MEMBER) return plain_read(n->a);
  if (n->kind == N_INDEX) return plain_read(n->a) && plain_read(n->b);
  return false;
}

static void sealed_block(AxNode *ent, AxNode *block, void *vctx) {
  Ck *c = vctx;
  const CheckSubsystem *sub = subsystem(ent->str2);
  if (!sub || !sub->sealed) return;
  for (int i = 0; i < block->nlist; i++) {
    AxNode *st = block->list[i];
    bool ok = false;
    char *reason = NULL;
    if (st->kind == N_ACTION) {
      ok = in_list(sub->actions, S(st->str));
      reason = fmt("action '!%s(...)' is not in &%s's declared surface", S(st->str), S(ent->str2));
    } else if (st->kind == N_ASSIGN && st->op != OP_OBSERVE) {
      if (st->a && st->a->kind == N_QUERY && in_list(sub->queries, S(st->a->str))) ok = true;
      else if (plain_read(st->a)) ok = true;
      reason = fmt("right-hand side is neither a query nor a plain read");
    } else if (st->kind == N_EXPRSTMT && plain_read(st->a)) {
      ok = true;
    } else {
      reason = fmt("statement type '%s' is not part of &%s's declared surface", stmt_type(st), S(ent->str2));
    }
    if (!ok) {
      CD *d = emit_at(c, "AX-NATIVE-001", "contract_violation", ent, block, st);
      d->rsec = "§1.6"; d->rtitle = "Compiler Pragmas for the Deep Math Core";
      d->human = fmt("Entity '%s' (&%s) contains a statement outside its sealed surface.", S(ent->str), S(ent->str2));
      d->agent = fmt("%s.", reason ? reason : "");
      d->fixk = 1; d->fix = fmt("Use only declared actions/queries or plain reads.");
    }
    free(reason);
  }
}

// ---- §2.3.1 — event schemas ------------------------------------------------------------------

typedef struct { Ck *c; Prog *pg; AxNode *ent, *block; } EvCtx;

static AxNode *find_event(Prog *pg, const char *name) {
  AxNode *found = NULL;
  for (int i = 0; i < pg->events.n; i++) if (streq(pg->events.v[i]->str, name)) found = pg->events.v[i];   // last wins
  return found;
}

static const char *ftype_str(AxNode *f) { return f->str2 ? f->str2->data : "null"; }

static void ev_stmt(AxNode *st, void *vctx) {
  EvCtx *e = vctx;
  if (st->kind != N_BROADCAST) return;
  const char *name = S(st->str);
  AxNode *schema = find_event(e->pg, name);
  if (!schema) {
    CD *d = emit_at(e->c, "AX-EVENT-001", "fatal", e->ent, e->block, st);
    d->rsec = "§2.3.1"; d->rtitle = "The Broadcast Protocol";
    d->human = fmt("'^%s(...)' broadcasts an undeclared event.", name);
    d->agent = fmt("Declare '^event %s:'.", name);
    d->fixk = 1; d->fix = fmt("Add '^event %s:' at the top level.", name);
    return;
  }
  SSet given = { 0 };
  for (int i = 0; i < st->nlist; i++) {
    AxNode *a = st->list[i];
    if (!a->str) continue;
    ss_add(&given, a->str->data);
    AxNode *field = NULL;
    for (int k = 0; k < schema->nlist; k++) if (ax_str_eq(schema->list[k]->str, a->str)) field = schema->list[k];   // last wins
    if (!field) {
      CD *d = emit_at(e->c, "AX-EVENT-001", "fatal", e->ent, e->block, st);
      d->rsec = "§2.3.1"; d->rtitle = "The Broadcast Protocol";
      d->human = fmt("'^%s(...)' passes undeclared field '%s'.", name, a->str->data);
      d->agent = fmt("'%s' is not in the schema.", a->str->data);
      d->fixk = 1; d->fix = fmt("Remove '%s' or add it to the schema.", a->str->data);
      continue;
    }
    AxNode *v = a->b;
    const char *actual = v && v->kind == N_NUM ? "number" : v && v->kind == N_STR ? "string" : NULL;
    if (actual && strcmp(ftype_str(field), actual) != 0) {
      CD *d = emit_at(e->c, "AX-EVENT-001", "fatal", e->ent, e->block, st);
      d->rsec = "§2.3.1"; d->rtitle = "The Broadcast Protocol";
      d->human = fmt("'^%s(%s: ...)' type mismatch (expected '%s').", name, a->str->data, ftype_str(field));
      d->agent = fmt("Pass a '%s' value.", ftype_str(field));
      d->fixk = 1; d->fix = fmt("Change the argument to '%s'.", ftype_str(field));
    }
  }
  for (int k = 0; k < schema->nlist; k++) {
    AxNode *f = schema->list[k];
    bool autofill = f->flag || (streq(f->str, "source") && f->str2 && !strcmp(f->str2->data, "#Entity"));
    if (autofill) continue;
    if (!ss_has(&given, f->str->data)) {
      CD *d = emit_at(e->c, "AX-EVENT-001", "fatal", e->ent, e->block, st);
      d->rsec = "§2.3.1"; d->rtitle = "The Broadcast Protocol";
      d->human = fmt("'^%s(...)' missing required field '%s'.", name, f->str->data);
      d->agent = fmt("Add '%s: <value>' to the call.", f->str->data);
      d->fixk = 1; d->fix = fmt("Add '%s: <value>'.", f->str->data);
    }
  }
  ss_free(&given);
}

static void ev_block(AxNode *ent, AxNode *block, void *vctx) {
  EvCtx *e = vctx;
  const char *arg = event_arg(block);
  if (arg && !find_event(e->pg, arg) && strcmp(arg, "Collide") != 0) {
    CD *d = emit(e->c, "AX-EVENT-001", "fatal");
    d->entity = sdup(S(ent->str)); d->block = sdup(S(block->str));
    d->line = block->line; d->col = block->col;
    d->snippet = source_line(e->c, block->line);
    d->rsec = "§2.3.1"; d->rtitle = "The Broadcast Protocol";
    d->human = fmt("'&on(%s)' handles an undeclared event.", arg);
    d->agent = fmt("Declare '^event %s:' at the top level, or use a built-in event like 'Collide'.", arg);
    d->fixk = 1; d->fix = fmt("Add '^event %s:' with its field list.", arg);
  }
  e->ent = ent;
  e->block = block;
  walk_stmts(block->list, block->nlist, ev_stmt, e);
}

// ---- §2.2 — infer strategies, and pools ------------------------------------------------------

static void check_infer(Ck *c, Prog *pg) {
  for (int i = 0; i < pg->entities.n; i++) {
    AxNode *ent = pg->entities.v[i];
    for (int k = 0; k < ent->nlist; k++) {
      AxNode *m = ent->list[k];
      if (m->kind != N_FIELD || m->op != 1 || !m->c) continue;
      const char *name = S(m->c->str);
      if (!in_list(CHECK_INFER_STRATEGIES, name)) {
        CD *d = emit(c, "AX-INFER-001", "fatal");
        d->entity = sdup(S(ent->str));
        d->line = m->line; d->col = m->col;
        d->rsec = "§2.2"; d->rtitle = "Inference Strategies";
        d->human = fmt("'infer: %s(...)' is not implemented.", name);
        d->agent = fmt("Use 'infer: exact' or 'infer: particle(N)'.");
        d->fixk = 2; d->fix = fmt("Replace with 'infer: exact'.");
      }
    }
  }
}

static AxNode *find_type(Prog *pg, const char *name) {
  AxNode *found = NULL;
  for (int i = 0; i < pg->types.n; i++) if (streq(pg->types.v[i]->str, name)) found = pg->types.v[i];   // last wins
  return found;
}

typedef struct { Ck *c; Prog *pg; AxNode *ent; NV *pools; } PoolCtx;

static AxNode *pool_field(NV *pools, const char *name) {
  AxNode *found = NULL;
  for (int i = 0; i < pools->n; i++) if (streq(pools->v[i]->str, name)) found = pools->v[i];
  return found;
}

static void pool_block(AxNode *ed, AxNode *block, void *vctx) {
  PoolCtx *pc = vctx;
  if (ed != pc->ent) return;
  for (int i = 0; i < block->nlist; i++) {
    AxNode *st = block->list[i];
    if (st->kind != N_ACTION || !streq(st->str, "spawn")) continue;
    AxNode *pool_arg = st->nlist > 0 ? st->list[0]->b : NULL;
    AxNode *init = st->nlist > 1 ? st->list[1]->b : NULL;
    if (!pool_arg || pool_arg->kind != N_IDENT) continue;
    AxNode *pf = pool_field(pc->pools, pool_arg->str->data);
    if (!pf) continue;
    AxNode *pool = pf->a;
    AxNode *td = find_type(pc->pg, S(pool->str2));
    if (!td || !init || init->kind != N_CALL) continue;
    if (!ax_str_eq(init->str, pool->str2)) {
      CD *d = emit(pc->c, "AX-POOL-002", "contract_violation");
      d->entity = sdup(S(pc->ent->str)); d->block = sdup(S(block->str));
      d->line = st->line; d->col = st->col;
      d->rsec = "§2.1.1"; d->rtitle = "Pool / Collection Type Validation";
      d->human = fmt("'!spawn(%s, %s(...))' type mismatch (expected '%s').", pool_arg->str->data, S(init->str), S(pool->str2));
      d->agent = fmt("Use '%s(...)' as the initializer.", S(pool->str2));
      d->fixk = 1; d->fix = fmt("Change to '%s(...)'.", S(pool->str2));
    } else if (init->nlist != td->nnames) {
      CD *d = emit(pc->c, "AX-POOL-002", "contract_violation");
      d->entity = sdup(S(pc->ent->str)); d->block = sdup(S(block->str));
      d->line = st->line; d->col = st->col;
      d->rsec = "§2.1.1"; d->rtitle = "Pool / Collection Type Validation";
      d->human = fmt("'%s(...)' arg count mismatch (%d vs %d).", S(pool->str2), init->nlist, td->nnames);
      d->agent = fmt("Match the declared field count.");
      d->fixk = 1; d->fix = fmt("Pass %d args.", td->nnames);
    }
  }
}

static void check_pools(Ck *c, Prog *pg) {
  for (int i = 0; i < pg->entities.n; i++) {
    AxNode *ent = pg->entities.v[i];
    NV pools = { 0 };
    for (int k = 0; k < ent->nlist; k++) {
      AxNode *m = ent->list[k];
      if (m->kind != N_FIELD || m->op != 0 || !m->a || m->a->kind != N_POOLTYPE) continue;
      nv_push(&pools, m);
      if (streq(m->a->str, "Pool") && !find_type(pg, S(m->a->str2))) {
        char cap[64];
        ax_fmt_num(m->a->num, cap, sizeof cap);
        CD *d = emit(c, "AX-POOL-001", "contract_violation");
        d->entity = sdup(S(ent->str));
        d->line = m->line; d->col = m->col;
        d->rsec = "§2.1.1"; d->rtitle = "Pool / Collection Type Validation";
        d->human = fmt("&Pool(%s, %s) references undeclared type.", S(m->a->str2), cap);
        d->agent = fmt("Declare '^type %s:'.", S(m->a->str2));
        d->fixk = 1; d->fix = fmt("Add '^type %s:'.", S(m->a->str2));
      }
    }
    if (pools.n) {
      PoolCtx pc = { c, pg, ent, &pools };
      for_each_block(pg, pool_block, &pc);
    }
    free(pools.v);
  }
}

// ---- v0.4 — functions, loop control, mixins --------------------------------------------------

typedef struct { Ck *c; AxNode *ent, *block, *st; } DivCtx;

static void div_visit(AxNode *n, void *vctx) {
  DivCtx *dc = vctx;
  if (n->kind == N_BINARY && (n->op == OP_DIV || n->op == OP_MOD) && n->b && n->b->kind == N_NUM && n->b->num == 0) {
    CD *d = emit_at(dc->c, "AX-DIV-001", "advisory", dc->ent, dc->block, dc->st);
    d->rsec = "§2.1.1"; d->rtitle = "The Zero-Allocation Contract";
    d->human = fmt("%s by zero (literal 0 as divisor).", n->op == OP_DIV ? "Division" : "Modulo");
    d->agent = fmt("The right-hand side of '%s' is the literal 0. This produces Infinity or NaN at runtime. Variable-based div-by-zero requires data-flow analysis (out of scope).", n->op == OP_DIV ? "/" : "%");
  }
}

static void div_block(AxNode *ent, AxNode *block, void *vctx) {
  for (int i = 0; i < block->nlist; i++) {
    DivCtx dc = { vctx, ent, block, block->list[i] };
    walk_roots(block->list[i], div_visit, &dc);
  }
}

static void check_functions(Ck *c, Prog *pg) {
  SSet all = { 0 };
  for (int pass = 0; pass < 2; pass++) {
    NV *l = pass ? &pg->procs : &pg->fns;
    for (int i = 0; i < l->n; i++) {
      AxNode *fn = l->v[i];
      if (ss_has(&all, S(fn->str))) {
        CD *d = emit(c, "AX-FN-001", "fatal");
        d->line = fn->line; d->col = fn->col;
        d->rsec = "v0.4"; d->rtitle = "User Function / Proc Validation";
        d->human = fmt("Duplicate function/proc name '%s'.", S(fn->str));
        d->agent = fmt("Rename one of the '%s' declarations.", S(fn->str));
      }
      ss_add(&all, S(fn->str));
    }
  }
  ss_free(&all);
  for (int pass = 0; pass < 2; pass++) {
    NV *l = pass ? &pg->procs : &pg->fns;
    for (int i = 0; i < l->n; i++) {
      AxNode *fn = l->v[i];
      for (int k = 0; k < fn->nlist - 1; k++) {
        if (fn->list[k]->kind == N_RETURN) {
          AxNode *next = fn->list[k + 1];
          CD *d = emit(c, "AX-FN-002", "advisory");
          d->line = next->line; d->col = next->col;
          d->snippet = source_line(c, next->line);
          d->rsec = "v0.4"; d->rtitle = "User Function / Proc Validation";
          d->human = fmt("Unreachable code after ^return in %s.", S(fn->str));
          d->agent = fmt("Code after ^return is never executed. Remove it or move the ^return.");
          break;
        }
      }
    }
  }
  for_each_block(pg, div_block, c);
}

static void loop_stmts(Ck *c, AxNode **stmts, int n, bool in_loop, const char *entity, const char *block) {
  for (int i = 0; i < n; i++) {
    AxNode *st = stmts[i];
    if ((st->kind == N_BREAK || st->kind == N_CONTINUE) && !in_loop) {
      CD *d = emit(c, "AX-LOOP-001", "fatal");
      d->entity = sdup(entity); d->block = sdup(block);
      d->line = st->line; d->col = st->col;
      d->rsec = "v0.4"; d->rtitle = "Loop Control Flow";
      d->human = fmt("'~%s' outside a loop.", st->kind == N_BREAK ? "break" : "continue");
      d->agent = fmt("Move this into a '*cond:' or '*i in 0..n:' loop.");
    }
    if (st->kind == N_IF) {
      loop_stmts(c, st->list, st->nlist, in_loop, entity, block);
      if (st->c) loop_stmts(c, st->c->list, st->c->nlist, in_loop, entity, block);
    }
    if (st->kind == N_WHILE || st->kind == N_FOR) loop_stmts(c, st->list, st->nlist, true, entity, block);
  }
}

static void loop_block(AxNode *ent, AxNode *block, void *vctx) {
  loop_stmts(vctx, block->list, block->nlist, false, S(ent->str), S(block->str));
}

static void check_loops(Ck *c, Prog *pg) {
  for_each_block(pg, loop_block, c);
  for (int i = 0; i < pg->fns.n; i++) {
    char *label = fmt("fn %s", S(pg->fns.v[i]->str));
    loop_stmts(c, pg->fns.v[i]->list, pg->fns.v[i]->nlist, false, NULL, label);
    free(label);
  }
  for (int i = 0; i < pg->procs.n; i++) {
    char *label = fmt("proc %s", S(pg->procs.v[i]->str));
    loop_stmts(c, pg->procs.v[i]->list, pg->procs.v[i]->nlist, false, NULL, label);
    free(label);
  }
}

static AxNode *find_mixin(Prog *pg, const AxStr *name) {
  for (int i = 0; i < pg->mixins.n; i++) if (ax_str_eq(pg->mixins.v[i]->str, name)) return pg->mixins.v[i];
  return NULL;
}

static void check_mixins(Ck *c, Prog *pg) {
  for (int i = 0; i < pg->entities.n; i++) {
    AxNode *e = pg->entities.v[i];
    for (int k = 0; k < e->nnames; k++) {
      if (find_mixin(pg, e->names[k])) continue;
      CD *d = emit(c, "AX-MIXIN-001", "fatal");
      d->entity = sdup(S(e->str));
      d->rsec = "v0.4"; d->rtitle = "Mixin Composition";
      d->human = fmt("Entity '%s' includes undefined mixin '+%s'.", S(e->str), e->names[k]->data);
      d->agent = fmt("Declare '^mix %s:' at the top level, or fix the name.", e->names[k]->data);
      d->fixk = 1; d->fix = fmt("Add '^mix %s:' or fix the reference.", e->names[k]->data);
    }
  }
}

// ---- v0.8.1 – v0.8.4 -------------------------------------------------------------------------

static const char *const KNOWN_BLOCKS[] = { "physics", "render", "tick", "on", NULL };

static void unknown_block(AxNode *ent, AxNode *block, void *vctx) {
  Ck *c = vctx;
  const char *name = S(block->str);
  if (in_list(KNOWN_BLOCKS, name)) return;
  const char *best = NULL;
  int bd = INT_MAX;
  for (int i = 0; KNOWN_BLOCKS[i]; i++) { int d = lev(name, KNOWN_BLOCKS[i]); if (d < bd) { bd = d; best = KNOWN_BLOCKS[i]; } }
  const char *sugg = bd <= 3 ? best : NULL;
  CD *d = emit(c, "AX-BLOCK-001", "advisory");
  d->entity = sdup(S(ent->str)); d->block = sdup(name);
  d->line = block->line; d->col = block->col;
  d->snippet = source_line(c, block->line);
  d->rsec = "runtime"; d->rtitle = "Unknown Block";
  if (sugg) {
    d->human = fmt("Block '&%s:' is not a recognized block name and will never execute. Did you mean '&%s:'?", name, sugg);
    d->agent = fmt("The interpreter only runs blocks named: physics, render, tick, on. Other block names parse but are dead code. Closest match: '%s' (Levenshtein distance %d).", sugg, lev(name, sugg));
    d->fixk = 3; d->fix = fmt("Rename to '&%s:'. Recognized blocks: physics (60Hz deterministic), render (draw), tick(Nhz) (AI/cognition), on(Event) (event handlers).", sugg);
  } else {
    d->human = fmt("Block '&%s:' is not a recognized block name and will never execute.", name);
    d->agent = fmt("The interpreter only runs blocks named: physics, render, tick, on. Other block names parse but are dead code. No close match found.");
    d->fixk = 3; d->fix = fmt("Use &physics: for deterministic 60Hz logic, &tick(Nhz): for AI/cognition, &render: for drawing, &on(Event): for event handlers.");
  }
}

static const char *const FUNCTION_ONLY[] = { "v2", "v3", "q", "euler", "m4", "persp", "ortho", "lookat", "aabb",
  "clamp", "dist", "sphere", "box", "capsule", "patrol_point", "bar", "vision_cells", "cell_to_world", NULL };

static void fn_only_block(AxNode *ent, AxNode *block, void *vctx) {
  Ck *c = vctx;
  for (int i = 0; i < block->nlist; i++) {
    AxNode *st = block->list[i];
    if (st->kind != N_ACTION || !in_list(FUNCTION_ONLY, S(st->str))) continue;
    const char *nm = S(st->str);
    CD *d = emit_at(c, "AX-ACTION-001", "fatal", ent, block, st);
    d->rsec = "v0.8.3"; d->rtitle = "Function used as action";
    d->human = fmt("'!%s(...)' is a function, not an action — it can't be used with the '!' sigil.", nm);
    d->agent = fmt("'%s' is an intrinsic function (valid in expressions like ~field: %s(...)). Only actions (play, mesh, save, spawn, etc.) can follow '!'. To draw a HUD bar, declare it as ~hud: bar(...); to create a collider, declare it as ~hit: %s(...).", nm, nm, nm);
    d->fixk = 1; d->fix = fmt("Remove the '!' or move '%s(...)' into a field declaration (e.g. ~hud: bar(...)).", nm);
  }
}

typedef struct { Ck *c; SSet *handled, *declared; AxNode *ent, *block; } BcCtx;

static void handled_events(AxNode *ent, SSet *out) {
  for (int i = 0; i < ent->nlist; i++) {
    AxNode *m = ent->list[i];
    if (m->kind == N_EBLOCK && event_arg(m)) ss_add(out, event_arg(m));
    else if (m->kind == N_ENTITY) handled_events(m, out);
  }
}

static void bc_stmt(AxNode *st, void *vctx) {
  BcCtx *b = vctx;
  if (st->kind != N_BROADCAST || streq(st->str, "Collide")) return;
  const char *nm = S(st->str);
  if (ss_has(b->handled, nm) || ss_has(b->declared, nm)) return;
  CD *d = emit_at(b->c, "AX-BROADCAST-001", "advisory", b->ent, b->block, st);
  d->rsec = "v0.8.3"; d->rtitle = "Broadcast with no listeners";
  d->human = fmt("'^%s(...)' is broadcast but no entity has an '&on(%s):' handler — this is a silent no-op.", nm, nm);
  d->agent = fmt("Either add an &on(%s): block to the intended receiver, or declare '^event %s:' if the handler will be added later. Without a handler, the broadcast does nothing and produces no error.", nm, nm);
  d->fixk = 1; d->fix = fmt("Add '&on(%s):' to the entity that should receive this event.", nm);
}

static void bc_block(AxNode *ent, AxNode *block, void *vctx) {
  BcCtx *b = vctx;
  b->ent = ent;
  b->block = block;
  walk_stmts(block->list, block->nlist, bc_stmt, b);
}

static void check_broadcasts(Ck *c, Prog *pg) {
  SSet handled = { 0 }, declared = { 0 };
  for (int i = 0; i < pg->entities.n; i++) handled_events(pg->entities.v[i], &handled);
  for (int i = 0; i < pg->events.n; i++) ss_add(&declared, S(pg->events.v[i]->str));
  BcCtx b = { c, &handled, &declared, NULL, NULL };
  for_each_block(pg, bc_block, &b);
  ss_free(&handled);
  ss_free(&declared);
}

static void check_version(Ck *c, Prog *pg) {
  if (!pg->version || in_list(CHECK_KNOWN_VERSIONS, pg->version)) return;
  CD *d = emit(c, "AX-VERSION-001", "advisory");
  d->line = 1; d->col = 1;
  d->snippet = fmt("axiom %s", pg->version);
  d->rsec = "header pragma"; d->rtitle = "axiom X.Y version pragma";
  d->human = fmt("Version '%s' not recognized by this toolchain.", pg->version);
  size_t cap = 64, len = 0;
  char *known = malloc(cap);
  known[0] = '\0';
  for (int i = 0; CHECK_KNOWN_VERSIONS[i]; i++) {
    size_t k = strlen(CHECK_KNOWN_VERSIONS[i]);
    while (len + k + 3 > cap) { cap *= 2; known = realloc(known, cap); }
    if (i) { memcpy(known + len, ", ", 2); len += 2; }
    memcpy(known + len, CHECK_KNOWN_VERSIONS[i], k + 1);
    len += k;
  }
  d->agent = fmt("Known: %s.", known);
  free(known);
}

static void check_inline_meshes(Ck *c, Prog *pg) {
  for (int i = 0; i < pg->resources.n; i++) {
    AxNode *r = pg->resources.v[i];
    if (!r->b) continue;
    const char *kind = S(r->str), *name = S(r->str2);
    if (strcmp(kind, "Mesh3D") != 0) {
      CD *d = emit(c, "AX-MESH-001", "contract_violation");
      d->line = r->line; d->col = r->col; d->snippet = source_line(c, r->line);
      d->rsec = "v0.8.4"; d->rtitle = "Inline base64 mesh on non-Mesh3D resource";
      d->human = fmt("#%s %s: base64(...) / glb: heredoc is only supported on #Mesh3D resources.", kind, name);
      d->agent = fmt("Inline base64 mesh payloads are only meaningful for #Mesh3D (binary glTF). For other resource kinds (Texture, NavMesh3D source, etc.) use the path form: #%s %s: \"file.ext\".", kind, name);
      d->fixk = 1; d->fix = fmt("Use the path form: #%s %s: \"file.ext\"", kind, name);
      continue;
    }
    uint8_t *buf = NULL;
    size_t n = 0;
    ax_base64_decode(r->b->str->data, r->b->str->len, &buf, &n);
    if (n < 12) {
      CD *d = emit(c, "AX-MESH-001", "contract_violation");
      d->line = r->line; d->col = r->col; d->snippet = source_line(c, r->line);
      d->rsec = "v0.8.4"; d->rtitle = "Inline base64 mesh too short";
      d->human = fmt("#Mesh3D %s: inline payload is only %zu bytes — too short for a GLB header (minimum 12).", name, n);
      d->agent = fmt("A valid GLB starts with a 12-byte header (magic 'glTF' + version + total length). The decoded payload is %zu bytes, which can't even fit the header. The base64 string was likely truncated.", n);
      d->fixk = 1; d->fix = fmt("Re-encode the full .glb file — the current payload is truncated.");
      free(buf);
      continue;
    }
    uint32_t magic = (uint32_t)buf[0] | (uint32_t)buf[1] << 8 | (uint32_t)buf[2] << 16 | (uint32_t)buf[3] << 24;
    uint32_t version = (uint32_t)buf[4] | (uint32_t)buf[5] << 8 | (uint32_t)buf[6] << 16 | (uint32_t)buf[7] << 24;
    if (magic != 0x46546C67u) {
      CD *d = emit(c, "AX-MESH-001", "contract_violation");
      d->line = r->line; d->col = r->col; d->snippet = source_line(c, r->line);
      d->rsec = "v0.8.4"; d->rtitle = "Inline base64 mesh wrong magic";
      d->human = fmt("#Mesh3D %s: inline payload is not a GLB (magic 0x%x ≠ 0x46546c67 'glTF').", name, magic);
      d->agent = fmt("The first 4 bytes of a GLB file must be the ASCII string 'glTF' (little-endian 0x46546c67). The decoded payload starts with 0x%x — this is not a GLB. If you embedded a .gltf (JSON) or .obj file, convert it to .glb first; AxiomScript only loads binary glTF.", magic);
      d->fixk = 1; d->fix = fmt("Convert the source asset to .glb (e.g. `gltf-pipeline -i hero.gltf -o hero.glb`) and re-embed.");
      free(buf);
      continue;
    }
    if (version != 2) {
      CD *d = emit(c, "AX-MESH-001", "contract_violation");
      d->line = r->line; d->col = r->col; d->snippet = source_line(c, r->line);
      d->rsec = "v0.8.4"; d->rtitle = "Inline base64 mesh wrong glTF version";
      d->human = fmt("#Mesh3D %s: inline payload is glTF version %u, only version 2 is supported.", name, version);
      d->agent = fmt("AxiomScript's parseGLBMulti only handles glTF 2.0 (the current Khronos spec). Version 1 .glb files are extinct in practice; this usually means the .glb was written by a non-standard tool.");
      d->fixk = 1; d->fix = fmt("Re-export the asset as glTF 2.0 from any modern DCC (Blender, Maya, etc.).");
      free(buf);
      continue;
    }
    int np = 0;
    AxGlbPrim *prims = ax_glb_parse(buf, n, &np);
    if (!prims) {
      CD *d = emit(c, "AX-MESH-001", "contract_violation");
      d->line = r->line; d->col = r->col; d->snippet = source_line(c, r->line);
      d->rsec = "v0.8.4"; d->rtitle = "Inline base64 mesh parse failure";
      d->human = fmt("#Mesh3D %s: inline payload has valid GLB magic/version but failed to parse (no meshes, corrupt JSON chunk, or missing BIN chunk).", name);
      d->agent = fmt("The header is valid but the content is structurally broken. parseGLBMulti returned null. Common causes: (1) the JSON chunk doesn't parse (truncated, missing closing brace); (2) the BIN chunk is missing or shorter than the accessors declare; (3) gltf.meshes is empty or missing. Open the .glb in a validator (e.g. https://github.com/KhronosGroup/glTF-Validator) before re-embedding.");
      d->fixk = 1; d->fix = fmt("Validate the source .glb with glTF-Validator, fix any errors, and re-embed.");
      free(buf);
      continue;
    }
    ax_glb_free(prims, np);
    if (n > 1000000) {
      char mb[64], lower[256];
      snprintf(mb, sizeof mb, "%.1f", (double)n / 1000000);
      size_t k = 0;
      for (; name[k] && k + 1 < sizeof lower; k++) lower[k] = (char)((name[k] >= 'A' && name[k] <= 'Z') ? name[k] + 32 : name[k]);
      lower[k] = '\0';
      CD *d = emit(c, "AX-MESH-001", "advisory");
      d->line = r->line; d->col = r->col; d->snippet = source_line(c, r->line);
      d->rsec = "v0.8.4"; d->rtitle = "Inline base64 mesh size advisory";
      d->human = fmt("#Mesh3D %s: inline payload is %s MB — consider using the file-based form for large meshes.", name, mb);
      d->agent = fmt("Inline base64 bloats the .ax source by ~1.33× the binary size (base64 encoding overhead) and makes the source harder to edit/diff. For environment meshes or detailed characters, use the path form `#Mesh3D %s: \"%s.glb\"` and ship the .glb alongside the .ax. Inline is best for small props and characters (< 1 MB).", name, lower);
      d->fixk = 1; d->fix = fmt("Move the .glb to a separate file and use `#Mesh3D %s: \"%s.glb\"`.", name, lower);
    }
    free(buf);
  }
}

typedef struct { const char *tag; SSet fields; } TagFields;
typedef struct { Ck *c; TagFields *tf; int ntf; AxNode *ent, *block; } XwCtx;

static void tag_fields(AxNode *ent, TagFields **tf, int *n) {
  SSet fields = { 0 };
  for (int i = 0; i < ent->nlist; i++) if (ent->list[i]->kind == N_FIELD && ent->list[i]->op == 0) ss_add(&fields, S(ent->list[i]->str));
  int at = -1;
  for (int i = 0; i < *n; i++) if (!strcmp((*tf)[i].tag, S(ent->str))) at = i;   // Map#set: a later one replaces
  if (at < 0) { *tf = realloc(*tf, sizeof(TagFields) * (*n + 1)); at = (*n)++; (*tf)[at].tag = S(ent->str); }
  else ss_free(&(*tf)[at].fields);
  (*tf)[at].fields = fields;
  for (int i = 0; i < ent->nlist; i++) if (ent->list[i]->kind == N_ENTITY) tag_fields(ent->list[i], tf, n);
}

static const char *const RESERVED_XWRITE[] = { "pos", "rot", "scl", "vel", "pose", NULL };

static void xw_stmt(AxNode *st, void *vctx) {
  XwCtx *x = vctx;
  if (st->kind != N_MEMBER_ASSIGN || !st->str2) return;
  SSet *fields = NULL;
  for (int i = 0; i < x->ntf; i++) if (!strcmp(x->tf[i].tag, st->str2->data)) fields = &x->tf[i].fields;
  if (!fields) return;
  const char *tag = st->str2->data, *first = S(st->names[0]);
  if (ss_has(fields, first) || in_list(RESERVED_XWRITE, first)) return;
  CD *d = emit_at(x->c, "AX-XWRITE-001", "advisory", x->ent, x->block, st);
  d->rsec = "v0.8.4"; d->rtitle = "Cross-entity write to undeclared field";
  if (st->nnames == 1) {
    d->human = fmt("'#%s.%s = ...' writes to an undeclared field on @%s — it will silently land in @%s's locals as a dead key nothing reads.", tag, first, tag, tag);
    d->agent = fmt("@%s has no ~%s field declared, and '%s' is not a reserved transform-property name (pos/rot/scl/vel/pose). The write executes (no crash) but the value goes into @%s.locals, which nothing reads — the visible state of @%s is unchanged. This is the same class of silent-correctness bug as v0.8.3 bugs #3/#4/#6/#7. Either declare ~%s on @%s, fix the typo, or use one of the reserved names (pos/rot/scl/vel route to the live pose Transform; pose replaces the whole Transform).", tag, first, first, tag, tag, first, tag);
    d->fixk = 1; d->fix = fmt("Add `~%s: <default>` to @%s's body, or fix the field name typo.", first, tag);
  } else {
    size_t cap = 64, len = 0;
    char *path = malloc(cap);
    path[0] = '\0';
    for (int i = 0; i < st->nnames; i++) {
      size_t k = st->names[i]->len;
      while (len + k + 2 > cap) { cap *= 2; path = realloc(path, cap); }
      if (i) path[len++] = '.';
      memcpy(path + len, st->names[i]->data, k + 1);
      len += k;
    }
    d->human = fmt("'#%s.%s = ...' writes through an undeclared field on @%s — the path walk will dereference undefined at runtime.", tag, path, tag);
    d->agent = fmt("@%s has no ~%s field, and '%s' is not a reserved transform-property name (pos/rot/scl/vel/pose). The cross-entity deep write walks obj.get('%s') → undefined, then tries to dereference undefined.%s → AX-RUNTIME-MEMBER. Declare ~%s on @%s or fix the path.", tag, first, first, first, S(st->names[1]), first, tag);
    d->fixk = 1; d->fix = fmt("Add `~%s: <default>` to @%s's body, or fix the path.", first, tag);
    free(path);
  }
}

static void xw_block(AxNode *ent, AxNode *block, void *vctx) {
  XwCtx *x = vctx;
  x->ent = ent;
  x->block = block;
  walk_stmts(block->list, block->nlist, xw_stmt, x);
}

static void check_xwrites(Ck *c, Prog *pg) {
  TagFields *tf = NULL;
  int ntf = 0;
  for (int i = 0; i < pg->entities.n; i++) tag_fields(pg->entities.v[i], &tf, &ntf);
  XwCtx x = { c, tf, ntf, NULL, NULL };
  for_each_block(pg, xw_block, &x);
  for (int i = 0; i < ntf; i++) ss_free(&tf[i].fields);
  free(tf);
}

// ---- v0.8.8 — undefined functions in entity blocks, query arguments --------------------------

typedef struct { Ck *c; SSet *declared, *bound, *pool; AxNode *ent, *block, *st; } UfCtx;

static void lambda_params(AxNode *n, void *vctx) {
  SSet *bound = vctx;
  if (n->kind == N_LAMBDA) for (int i = 0; i < n->nnames; i++) ss_add(bound, S(n->names[i]));
}

static void collect_block_bound(AxNode **stmts, int n, SSet *bound);
static void cbb_list(AxNode **stmts, int n, void *ctx) { collect_block_bound(stmts, n, ctx); }
static void collect_block_bound(AxNode **stmts, int n, SSet *bound) {
  for (int i = 0; i < n; i++) {
    AxNode *st = stmts[i];
    if (st->kind == N_ASSIGN && st->str) ss_add(bound, S(st->str));
    if (st->kind == N_DESTRUCTURE) for (int k = 0; k < st->nnames; k++) ss_add(bound, S(st->names[k]));
    if (st->kind == N_FOR) for (int k = 0; k < st->nnames; k++) ss_add(bound, S(st->names[k]));
    if (st->kind == N_TRY && st->str) ss_add(bound, S(st->str));
    each_sublist(st, cbb_list, bound);
    walk_roots(st, lambda_params, bound);
  }
}

static void uf_visit(AxNode *n, void *vctx) {
  UfCtx *u = vctx;
  if (n->kind != N_CALL || !n->str) return;
  const char *callee = n->str->data;
  if (known_name(callee) || ss_has(u->declared, callee) || ss_has(u->bound, callee)) return;
  if (!strcmp(callee, "observe")) return;
  const char *best = NULL;
  int bd = INT_MAX;
  for (int i = 0; i < u->pool->n; i++) { int d = lev(callee, u->pool->v[i]); if (d < bd) { bd = d; best = u->pool->v[i]; } }
  const char *sugg = bd <= 3 ? best : NULL;
  CD *d = emit_at(u->c, "AX-UNDEF-FN-001", "advisory", u->ent, u->block, u->st);
  d->rsec = "v0.8.8"; d->rtitle = "Undefined Function";
  if (sugg) {
    d->human = fmt("Call to undefined function '%s(...)'. Did you mean '%s'?", callee, sugg);
    d->agent = fmt("'%s' is not a declared ^fn/^proc and not a known intrinsic. Closest match: '%s' (Levenshtein distance %d). Declare it with '^fn %s(args):' or fix the spelling.", callee, sugg, bd, callee);
    d->fixk = 1; d->fix = fmt("Replace '%s' with '%s'.", callee, sugg);
  } else {
    d->human = fmt("Call to undefined function '%s(...)'.", callee);
    d->agent = fmt("'%s' is not a declared ^fn/^proc and not a known intrinsic. Declare it with '^fn %s(args):' or fix the spelling. Known intrinsics: v3, v2, q, euler, lookat, dist, clamp, abs, min, max, floor, sin, cos, PI, random, etc.", callee, callee);
  }
}

static void uf_block(AxNode *ent, AxNode *block, void *vctx) {
  UfCtx *u = vctx;
  SSet bound = { 0 };
  for (int i = 0; i < ent->nlist; i++) if (ent->list[i]->kind == N_FIELD) ss_add(&bound, S(ent->list[i]->str));
  collect_block_bound(block->list, block->nlist, &bound);
  UfCtx cu = *u;
  cu.bound = &bound;
  cu.ent = ent;
  cu.block = block;
  for (int i = 0; i < block->nlist; i++) {
    cu.st = block->list[i];
    walk_roots(block->list[i], uf_visit, &cu);
  }
  ss_free(&bound);
}

static void declared_callables(Prog *pg, SSet *out) {
  for (int i = 0; i < pg->fns.n; i++) ss_add(out, S(pg->fns.v[i]->str));
  for (int i = 0; i < pg->procs.n; i++) ss_add(out, S(pg->procs.v[i]->str));
  for (int i = 0; i < pg->types.n; i++) ss_add(out, S(pg->types.v[i]->str));
}

static void check_undefined_fns(Ck *c, Prog *pg) {
  SSet declared = { 0 };
  declared_callables(pg, &declared);
  // The suggestion pool: the known names, then the declared ones (an array, so a name may
  // appear twice, exactly as [...KNOWN_INTRINSIC_NAMES, ...declaredFns]).
  SSet pool = { 0 };
  pool.cap = CHECK_KNOWN_NAMES_N + declared.n + 1;
  pool.v = malloc(sizeof(char *) * pool.cap);
  for (int i = 0; i < CHECK_KNOWN_NAMES_N; i++) pool.v[pool.n++] = CHECK_KNOWN_NAMES[i];
  for (int i = 0; i < declared.n; i++) pool.v[pool.n++] = declared.v[i];
  UfCtx u = { c, &declared, NULL, &pool, NULL, NULL, NULL };
  for_each_block(pg, uf_block, &u);
  free(pool.v);
  ss_free(&declared);
}

typedef struct { Ck *c; AxNode *ent, *block, *st; } QaCtx;

static void qa_visit(AxNode *n, void *vctx) {
  QaCtx *q = vctx;
  if (n->kind != N_QUERY || !(streq(n->str, "nearest") || streq(n->str, "exists"))) return;
  if (n->nlist < 1 || !n->list[0]->b) return;
  AxNode *first = n->list[0]->b;
  if (first->kind == N_TAGREF) return;
  const char *got = first->kind == N_STR ? "string literal" : first->kind == N_IDENT ? "identifier" : first->kind == N_NUM ? "number literal" : expr_type(first);
  CD *d = emit_at(q->c, "AX-QUERY-001", "advisory", q->ent, q->block, q->st);
  d->rsec = "v0.8.8"; d->rtitle = "Query Argument";
  d->human = fmt("First argument to '?%s(...)' must be a #TagRef, got %s.", S(n->str), got);
  d->agent = fmt("'?%s' looks up an entity by tag — use '#Player' instead of '\"Player\"' or a bare identifier. The tag must be declared with '@Player ...' at the top level.", S(n->str));
  d->fixk = 1; d->fix = fmt("Replace the first argument with a #TagRef (e.g. '#Player').");
}

static void qa_block(AxNode *ent, AxNode *block, void *vctx) {
  for (int i = 0; i < block->nlist; i++) {
    QaCtx q = { vctx, ent, block, block->list[i] };
    walk_roots(block->list[i], qa_visit, &q);
  }
}

// ---- v0.9.0 — entry point, undefined functions in function bodies ----------------------------

static void check_entry(Ck *c, Prog *pg) {
  if (pg->main || pg->entities.n) return;
  if (!pg->fns.n && !pg->procs.n && !pg->types.n) return;
  CD *d = emit(c, "AX-MAIN-001", "advisory");
  d->line = 1; d->col = 1;
  d->snippet = source_line(c, 1);
  d->rsec = "v0.9.0"; d->rtitle = "No entry point";
  d->human = fmt("This program declares no ^main and no entities, so running it does nothing.");
  d->agent = fmt("Add '^main:' with the statements to run, or declare an @Entity with a &tick/&physics block. This is expected for a library file that another program imports with ^use.");
  d->fixk = 1; d->fix = fmt("Add a '^main:' block, or import this file from one that has one.");
}

typedef struct { Ck *c; SSet *declared, *bound; const char *label; AxNode *st; } UbCtx;

static void collect_fn_bound(AxNode **stmts, int n, SSet *bound);
static void cfb_list(AxNode **stmts, int n, void *ctx) { collect_fn_bound(stmts, n, ctx); }
static void collect_fn_bound(AxNode **stmts, int n, SSet *bound) {
  for (int i = 0; i < n; i++) {
    AxNode *st = stmts[i];
    if (st->kind == N_ASSIGN && st->str) ss_add(bound, S(st->str));
    if (st->kind == N_DESTRUCTURE) for (int k = 0; k < st->nnames; k++) ss_add(bound, S(st->names[k]));
    if (st->kind == N_FOR) for (int k = 0; k < st->nnames; k++) ss_add(bound, S(st->names[k]));
    if (st->kind == N_TRY && st->str) ss_add(bound, S(st->str));
    each_sublist(st, cfb_list, bound);
  }
}

static void ub_visit(AxNode *n, void *vctx) {
  UbCtx *u = vctx;
  if (n->kind == N_LAMBDA) { for (int i = 0; i < n->nnames; i++) ss_add(u->bound, S(n->names[i])); return; }
  if (n->kind != N_CALL || !n->str) return;
  const char *callee = n->str->data;
  if (known_name(callee) || ss_has(u->declared, callee) || ss_has(u->bound, callee)) return;
  CD *d = emit(u->c, "AX-UNDEF-FN-001", "advisory");
  d->block = sdup(u->label);
  d->line = u->st->line; d->col = u->st->col;
  d->snippet = source_line(u->c, u->st->line);
  d->rsec = "v0.9.0"; d->rtitle = "Undefined Function";
  d->human = fmt("Call to undefined function '%s(...)' in %s.", callee, u->label);
  d->agent = fmt("'%s' is not a declared ^fn/^proc/^type, not a local holding a function, and not a standard-library name. Declare it, or check the spelling against the standard library (see STDLIB.md).", callee);
}

static void ub_walk(AxNode **stmts, int n, void *vctx);
static void ub_walk(AxNode **stmts, int n, void *vctx) {
  UbCtx *u = vctx;
  for (int i = 0; i < n; i++) {
    u->st = stmts[i];
    walk_roots(stmts[i], ub_visit, u);
    each_sublist(stmts[i], ub_walk, u);
  }
}

static void ub_scan(Ck *c, SSet *declared, AxNode *decl, const char *label) {
  SSet bound = { 0 };
  for (int i = 0; i < decl->nnames; i++) ss_add(&bound, S(decl->names[i]));
  ss_add(&bound, "args");
  collect_fn_bound(decl->list, decl->nlist, &bound);
  UbCtx u = { c, declared, &bound, label, NULL };
  ub_walk(decl->list, decl->nlist, &u);
  ss_free(&bound);
}

static void check_undefined_in_bodies(Ck *c, Prog *pg) {
  SSet declared = { 0 };
  declared_callables(pg, &declared);
  for (int i = 0; i < pg->fns.n; i++) { char *l = fmt("^fn %s", S(pg->fns.v[i]->str)); ub_scan(c, &declared, pg->fns.v[i], l); free(l); }
  for (int i = 0; i < pg->procs.n; i++) { char *l = fmt("^proc %s", S(pg->procs.v[i]->str)); ub_scan(c, &declared, pg->procs.v[i], l); free(l); }
  if (pg->main) ub_scan(c, &declared, pg->main, "^main");
  ss_free(&declared);
}

// ---- v0.9.1 — the static safety net ----------------------------------------------------------

// forEachBody: every statement list — entity blocks, then ^fn, ^proc, ^main — with its label and
// the names in scope around it.
typedef struct {
  AxNode **body; int nbody;
  const char *label;
  AxNode *ent, *block, *decl;
  SSet *scope;
  SSet *types;
} Body;
typedef void (*BodyFn)(Body *b, void *ctx);

typedef struct { Prog *pg; BodyFn f; void *ctx; SSet *types; } FebCtx;

static void feb_block(AxNode *ent, AxNode *block, void *vctx) {
  FebCtx *fe = vctx;
  SSet scope = { 0 };
  for (int i = 0; i < ent->nlist; i++) if (ent->list[i]->kind == N_FIELD) ss_add(&scope, S(ent->list[i]->str));
  for (int k = 0; k < ent->nnames; k++) {
    AxNode *mix = find_mixin(fe->pg, ent->names[k]);
    if (mix) for (int i = 0; i < mix->nlist; i++) if (mix->list[i]->kind == N_FIELD) ss_add(&scope, S(mix->list[i]->str));
  }
  if (event_arg(block)) {
    AxNode *ev = NULL;
    for (int i = 0; i < fe->pg->events.n; i++) if (streq(fe->pg->events.v[i]->str, event_arg(block))) { ev = fe->pg->events.v[i]; break; }
    if (ev) for (int i = 0; i < ev->nlist; i++) ss_add(&scope, S(ev->list[i]->str));
    ss_add(&scope, "source");
  }
  char *label = fmt("&%s", S(block->str));
  Body b = { block->list, block->nlist, label, ent, block, NULL, &scope, fe->types };
  fe->f(&b, fe->ctx);
  free(label);
  ss_free(&scope);
}

static void for_each_body(Prog *pg, BodyFn f, void *ctx) {
  SSet types = { 0 };
  for (int i = 0; i < pg->types.n; i++) ss_add(&types, S(pg->types.v[i]->str));
  FebCtx fe = { pg, f, ctx, &types };
  for_each_block(pg, feb_block, &fe);
  for (int pass = 0; pass < 3; pass++) {
    NV *l = pass == 0 ? &pg->fns : &pg->procs;
    int n = pass == 2 ? (pg->main ? 1 : 0) : l->n;
    for (int i = 0; i < n; i++) {
      AxNode *decl = pass == 2 ? pg->main : l->v[i];
      SSet scope = { 0 };
      for (int k = 0; k < decl->nnames; k++) ss_add(&scope, S(decl->names[k]));
      char *label = pass == 2 ? fmt("^main") : fmt("%s %s", pass == 0 ? "^fn" : "^proc", S(decl->str));
      Body b = { decl->list, decl->nlist, label, NULL, NULL, decl, &scope, &types };
      f(&b, ctx);
      free(label);
      ss_free(&scope);
    }
  }
  ss_free(&types);
}

static const char *body_entity(Body *b) { return b->ent ? S(b->ent->str) : NULL; }
static const char *body_block(Body *b) { return b->block ? S(b->block->str) : b->label; }

// A generic recursion: visit each statement's expression roots, then its statement lists.
typedef void (*RootsFn)(AxNode *st, void *ctx);
typedef struct { RootsFn f; void *ctx; } RecCtx;
static void rec_list(AxNode **stmts, int n, void *vctx) {
  RecCtx *r = vctx;
  for (int i = 0; i < n; i++) {
    r->f(stmts[i], r->ctx);
    each_sublist(stmts[i], rec_list, r);
  }
}
static void recurse(AxNode **stmts, int n, RootsFn f, void *ctx) { RecCtx r = { f, ctx }; rec_list(stmts, n, &r); }

// --- AX-ARITY-001
static bool reads_args_visit_found;
static void ra_visit(AxNode *n, void *vctx) { (void)vctx; if (n->kind == N_IDENT && streq(n->str, "args")) reads_args_visit_found = true; }
static void ra_stmt(AxNode *st, void *vctx) { walk_roots(st, ra_visit, vctx); }
static bool body_reads_args(AxNode *decl) {
  reads_args_visit_found = false;
  recurse(decl->list, decl->nlist, ra_stmt, NULL);
  return reads_args_visit_found;
}

typedef struct { Ck *c; Prog *pg; Body *b; AxNode *st; } ArCtx;

static AxNode *find_sig(Prog *pg, const char *name) {
  AxNode *found = NULL;
  for (int i = 0; i < pg->fns.n; i++) if (streq(pg->fns.v[i]->str, name)) found = pg->fns.v[i];
  for (int i = 0; i < pg->procs.n; i++) if (streq(pg->procs.v[i]->str, name)) found = pg->procs.v[i];
  return found;
}

static void arity_report(ArCtx *a, const char *name, int given, AxNode *at) {
  AxNode *sig = find_sig(a->pg, name);
  int total = sig->nnames, required = 0;
  for (int i = 0; i < total; i++) if (!(sig->defaults && sig->defaults[i])) required++;
  if (given >= required && given <= total) return;
  if (given > total && body_reads_args(sig)) return;
  int line = at && at->line ? at->line : sig->line;
  char shape[64];
  if (required == total) snprintf(shape, sizeof shape, "%d", total);
  else snprintf(shape, sizeof shape, "%d–%d", required, total);
  CD *d = emit(a->c, "AX-ARITY-001", "advisory");
  d->entity = sdup(body_entity(a->b));
  d->block = sdup(body_block(a->b));
  d->line = line;
  d->col = at ? at->col : 0;
  d->snippet = source_line(a->c, line);
  d->rsec = "v0.9.1"; d->rtitle = "Wrong number of arguments";
  char given_s[32];
  if (given == 1) snprintf(given_s, sizeof given_s, "1 was");
  else snprintf(given_s, sizeof given_s, "%d were", given);
  d->human = fmt("'%s(...)' takes %s argument%s, but %s given.", name, shape, required == 1 && total == 1 ? "" : "s", given_s);
  size_t cap = 64, len = 0;
  char *params = malloc(cap);
  params[0] = '\0';
  for (int i = 0; i < total; i++) {
    size_t k = sig->names[i]->len;
    while (len + k + 3 > cap) { cap *= 2; params = realloc(params, cap); }
    if (i) { memcpy(params + len, ", ", 2); len += 2; }
    memcpy(params + len, sig->names[i]->data, k + 1);
    len += k;
  }
  const char *next = given >= 0 && given < total ? sig->names[given]->data : "x";
  d->agent = fmt("%s %s(%s) expects %s argument(s); this call passes %d. Missing arguments bind to null (or their default) and extra ones are dropped, so the failure will surface far from here. Fix the call, or give the parameter a default (`%s = <value>`) if it is meant to be optional.", sig->op == 1 ? "^proc" : "^fn", name, params, shape, given, next);
  free(params);
}

static void ar_visit(AxNode *n, void *vctx) {
  ArCtx *a = vctx;
  if (n->kind == N_CALL && n->str && find_sig(a->pg, n->str->data)) arity_report(a, n->str->data, n->nlist, a->st);
}

static void ar_stmt(AxNode *st, void *vctx) {
  ArCtx *a = vctx;
  if (st->kind == N_ACTION && st->str && find_sig(a->pg, st->str->data)) arity_report(a, st->str->data, st->nlist, st);
  a->st = st;
  walk_roots(st, ar_visit, a);
}

static void arity_body(Body *b, void *vctx) {
  ArCtx a = *(ArCtx *)vctx;
  a.b = b;
  recurse(b->body, b->nbody, ar_stmt, &a);
}

static void check_arity(Ck *c, Prog *pg) {
  if (!pg->fns.n && !pg->procs.n) return;
  ArCtx a = { c, pg, NULL, NULL };
  for_each_body(pg, arity_body, &a);
}

// --- AX-FIELD-001
typedef struct { const char *var; const char *type; } VT;   // type NULL: not certain
typedef struct { Ck *c; Prog *pg; Body *b; VT *vt; int nvt; AxNode *st; } RfCtx;

static int vt_find(RfCtx *r, const char *var) { for (int i = 0; i < r->nvt; i++) if (!strcmp(r->vt[i].var, var)) return i; return -1; }
static void vt_set(RfCtx *r, const char *var, const char *type) {
  int i = vt_find(r, var);
  if (i < 0) { r->vt = realloc(r->vt, sizeof(VT) * (r->nvt + 1)); i = r->nvt++; r->vt[i].var = var; }
  r->vt[i].type = type;
}

static void rf_note_list(AxNode **stmts, int n, void *vctx);
static void rf_note_list(AxNode **stmts, int n, void *vctx) {
  RfCtx *r = vctx;
  for (int i = 0; i < n; i++) {
    AxNode *st = stmts[i];
    if (st->kind == N_ASSIGN && st->str) {
      AxNode *v = st->a;
      const char *t = (v && v->kind == N_CALL && v->str && find_type(r->pg, v->str->data)) ? v->str->data : NULL;
      if (vt_find(r, st->str->data) >= 0) vt_set(r, st->str->data, NULL);
      else vt_set(r, st->str->data, t);
    }
    if (st->kind == N_DESTRUCTURE) for (int k = 0; k < st->nnames; k++) vt_set(r, st->names[k]->data, NULL);
    if (st->kind == N_FOR) for (int k = 0; k < st->nnames; k++) vt_set(r, st->names[k]->data, NULL);
    each_sublist(st, rf_note_list, r);
  }
}

static void rf_visit(AxNode *n, void *vctx) {
  RfCtx *r = vctx;
  if (n->kind != N_MEMBER && n->kind != N_METHOD) return;
  if (!n->a || n->a->kind != N_IDENT) return;
  int i = vt_find(r, n->a->str->data);
  if (i < 0 || !r->vt[i].type) return;
  const char *tname = r->vt[i].type;
  AxNode *td = find_type(r->pg, tname);
  SSet fields = { 0 };
  for (int k = 0; k < td->nnames; k++) ss_add(&fields, S(td->names[k]));
  const char *prop = S(n->str);
  if (ss_has(&fields, prop) || n->kind == N_METHOD) { ss_free(&fields); return; }
  const char *best = NULL;
  int bd = INT_MAX;
  for (int k = 0; k < fields.n; k++) { int dd = lev(prop, fields.v[k]); if (dd < bd) { bd = dd; best = fields.v[k]; } }
  CD *d = emit(r->c, "AX-FIELD-001", "advisory");
  d->entity = sdup(body_entity(r->b));
  d->block = sdup(body_block(r->b));
  d->line = r->st->line; d->col = r->st->col;
  d->snippet = source_line(r->c, r->st->line);
  d->rsec = "v0.9.1"; d->rtitle = "Unknown field on a record";
  char *tail = bd <= 3 ? fmt(" — did you mean '%s'?", best) : fmt(".");
  d->human = fmt("'%s' is a %s, which has no field '%s'%s", n->a->str->data, tname, prop, tail);
  free(tail);
  size_t cap = 64, len = 0;
  char *known = malloc(cap);
  known[0] = '\0';
  for (int k = 0; k < fields.n; k++) {
    size_t kl = strlen(fields.v[k]);
    while (len + kl + 3 > cap) { cap *= 2; known = realloc(known, cap); }
    if (k) { memcpy(known + len, ", ", 2); len += 2; }
    memcpy(known + len, fields.v[k], kl + 1);
    len += kl;
  }
  d->agent = fmt("^type %s declares: %s. Reading '%s' yields null, which will surface as a confusing failure later. Fix the field name, or add '%s' to the ^type declaration.", tname, known, prop, prop);
  free(known);
  if (bd <= 3) { d->fixk = 1; d->fix = fmt("Replace '.%s' with '.%s'.", prop, best); }
  ss_free(&fields);
}

static void rf_stmt(AxNode *st, void *vctx) {
  RfCtx *r = vctx;
  r->st = st;
  walk_roots(st, rf_visit, r);
}

static void fields_body(Body *b, void *vctx) {
  RfCtx r = *(RfCtx *)vctx;
  r.b = b;
  r.vt = NULL;
  r.nvt = 0;
  rf_note_list(b->body, b->nbody, &r);
  recurse(b->body, b->nbody, rf_stmt, &r);
  free(r.vt);
}

static void check_record_fields(Ck *c, Prog *pg) {
  if (!pg->types.n) return;
  RfCtx r = { c, pg, NULL, NULL, 0, NULL };
  for_each_body(pg, fields_body, &r);
}

// --- AX-UNDEF-VAR-001
static const char *const IMPLICIT_NAMES[] = { "dt", "self", "null", "true", "false", "input", "args", "it",
  "pose", "wpose", "pos", "vel", "rot", "scl", "position", "velocity", "facing", NULL };

static void pattern_bindings(AxNode *pat, SSet *types, SSet *out, bool top) {
  if (!pat) return;
  switch (pat->kind) {
    case N_IDENT:
      if (streq(pat->str, "_") || top) return;
      if (ss_has(types, S(pat->str))) return;
      ss_add(out, S(pat->str));
      return;
    case N_CALL:
      if (!ss_has(types, S(pat->str))) return;
      for (int i = 0; i < pat->nlist; i++) if (pat->list[i]->b) pattern_bindings(pat->list[i]->b, types, out, false);
      return;
    case N_ARRAY: for (int i = 0; i < pat->nlist; i++) pattern_bindings(pat->list[i], types, out, false); return;
    case N_DICT: for (int i = 0; i < pat->nlist; i++) pattern_bindings(pat->list[i]->b, types, out, false); return;
    default: return;
  }
}

static void bound_visit(AxNode *n, void *vctx) {
  SSet *out = vctx;
  if (n->kind == N_LAMBDA) for (int i = 0; i < n->nnames; i++) ss_add(out, S(n->names[i]));
  if (n->kind == N_COMPREHENSION) for (int i = 0; i < n->nnames; i++) ss_add(out, S(n->names[i]));
}

static void collect_bound_names(AxNode **stmts, int n, SSet *types, SSet *out) {
  for (int i = 0; i < n; i++) {
    AxNode *st = stmts[i];
    if (st->kind == N_ASSIGN && st->str) ss_add(out, S(st->str));
    if (st->kind == N_DESTRUCTURE) for (int k = 0; k < st->nnames; k++) ss_add(out, S(st->names[k]));
    if (st->kind == N_FOR) for (int k = 0; k < st->nnames; k++) ss_add(out, S(st->names[k]));
    if (st->kind == N_TRY && st->str) ss_add(out, S(st->str));
    if (st->kind == N_MEMBER_ASSIGN) {
      // MemberAssign.obj (a tag write has none) / DeepAssign.path[0] (a tag write's too)
      if (!st->str2 || st->nnames >= 2) ss_add(out, S(st->names[0]));
    }
    if (st->kind == N_INDEX_ASSIGN && st->str) ss_add(out, S(st->str));
    switch (st->kind) {
      case N_WHILE: case N_FOR: collect_bound_names(st->list, st->nlist, types, out); break;
      case N_IF: collect_bound_names(st->list, st->nlist, types, out); if (st->c) collect_bound_names(st->c->list, st->c->nlist, types, out); break;
      case N_TRY:
        collect_bound_names(st->list, st->nlist, types, out);
        if (st->c) collect_bound_names(st->c->list, st->c->nlist, types, out);
        if (st->b) collect_bound_names(st->b->list, st->b->nlist, types, out);
        break;
      default: break;
    }
    for (int k = 0; k < st->narms && st->kind == N_MATCH; k++) {
      AxArm *arm = &st->arms[k];
      for (int p = 0; p < arm->npatterns; p++) pattern_bindings(arm->patterns[p], types, out, true);
      if (arm->guard && arm->npatterns == 1 && arm->patterns[0]->kind == N_IDENT && !streq(arm->patterns[0]->str, "_")
          && !ss_has(types, S(arm->patterns[0]->str))) ss_add(out, S(arm->patterns[0]->str));
      collect_bound_names(arm->body, arm->nbody, types, out);
    }
    walk_roots(st, bound_visit, out);
  }
}

typedef struct { const char *name; AxNode *node, *st; } Suspect;
typedef struct { SSet *known; Suspect *v; int n; AxNode *st; } UnCtx;

static void un_note(UnCtx *u, AxNode *n) {
  if (!n || n->kind != N_IDENT || n->op == 1) return;
  if (ss_has(u->known, S(n->str))) return;
  for (int i = 0; i < u->n; i++) if (!strcmp(u->v[i].name, S(n->str))) return;
  u->v = realloc(u->v, sizeof(Suspect) * (u->n + 1));
  u->v[u->n].name = S(n->str);
  u->v[u->n].node = n;
  u->v[u->n].st = u->st;
  u->n++;
}

static void un_visit(AxNode *n, void *vctx) {
  UnCtx *u = vctx;
  if (n->kind == N_BINARY && (n->op == OP_ADD || n->op == OP_SUB || n->op == OP_MUL || n->op == OP_DIV || n->op == OP_MOD ||
                              n->op == OP_POW || n->op == OP_GT || n->op == OP_LT || n->op == OP_GE || n->op == OP_LE)) {
    un_note(u, n->a);
    un_note(u, n->b);
  } else if (n->kind == N_UNARY && n->op == OP_NEG) un_note(u, n->a);
  else if (n->kind == N_INDEX) un_note(u, n->a);
  else if (n->kind == N_MEMBER || n->kind == N_METHOD) un_note(u, n->a);
}

static void un_stmt(AxNode *st, void *vctx) {
  UnCtx *u = vctx;
  u->st = st;
  walk_roots(st, un_visit, u);
}

typedef struct { Ck *c; SSet *globals; } UnnCtx;

static void unknown_body(Body *b, void *vctx) {
  UnnCtx *un = vctx;
  SSet known = { 0 };
  for (int i = 0; i < b->scope->n; i++) ss_add(&known, b->scope->v[i]);
  for (int i = 0; i < un->globals->n; i++) ss_add(&known, un->globals->v[i]);
  for (int i = 0; IMPLICIT_NAMES[i]; i++) ss_add(&known, IMPLICIT_NAMES[i]);
  for (int i = 0; i < CHECK_KNOWN_NAMES_N; i++) ss_add(&known, CHECK_KNOWN_NAMES[i]);
  collect_bound_names(b->body, b->nbody, b->types, &known);
  UnCtx u = { &known, NULL, 0, NULL };
  recurse(b->body, b->nbody, un_stmt, &u);
  for (int i = 0; i < u.n; i++) {
    const char *name = u.v[i].name;
    const char *best = NULL;
    int bd = INT_MAX;
    for (int k = 0; k < known.n; k++) { int d = lev(name, known.v[k]); if (d < bd) { bd = d; best = known.v[k]; } }
    AxNode *st = u.v[i].st;
    CD *d = emit(un->c, "AX-UNDEF-VAR-001", "advisory");
    d->entity = sdup(body_entity(b));
    d->block = sdup(body_block(b));
    d->line = st->line; d->col = st->col;
    d->snippet = source_line(un->c, st->line);
    d->rsec = "v0.9.1"; d->rtitle = "Unknown name used as a value";
    char *hint = bd <= 3 ? fmt(" (did you mean '%s'?)", best) : fmt("%s", "");
    char *hint2 = bd <= 3 ? fmt(" (closest known name: '%s')", best) : fmt("%s", "");
    d->human = fmt("'%s' is not declared anywhere%s — used like this it becomes the atom `%s`, not a value.", name, hint, name);
    d->agent = fmt("An identifier that is not a parameter, local, loop variable, field, global, or standard-library name evaluates to an ATOM (the language's symbol type, as in `state = idle`). Here '%s' is used in arithmetic, an ordering comparison, an index, or a member access, where an atom can never be right — arithmetic on it yields NaN and member access yields null. Declare '%s', or correct the spelling%s.", name, name, hint2);
    free(hint); free(hint2);
    if (bd <= 3) { d->fixk = 1; d->fix = fmt("Replace '%s' with '%s'.", name, best); }
  }
  free(u.v);
  ss_free(&known);
}

static void check_unknown_names(Ck *c, Prog *pg) {
  SSet globals = { 0 };
  for (int i = 0; i < pg->globals.n; i++) for (int k = 0; k < pg->globals.v[i]->nnames; k++) ss_add(&globals, S(pg->globals.v[i]->names[k]));
  for (int i = 0; i < pg->fns.n; i++) ss_add(&globals, S(pg->fns.v[i]->str));
  for (int i = 0; i < pg->procs.n; i++) ss_add(&globals, S(pg->procs.v[i]->str));
  for (int i = 0; i < pg->types.n; i++) ss_add(&globals, S(pg->types.v[i]->str));
  for (int i = 0; i < pg->materials.n; i++) ss_add(&globals, S(pg->materials.v[i]->str));
  UnnCtx un = { c, &globals };
  for_each_body(pg, unknown_body, &un);
  ss_free(&globals);
}

// --- AX-TYPE-001
static const char *type_alias(const char *t) {
  static const char *const map[][2] = { { "number", "number" }, { "num", "number" }, { "int", "number" }, { "float", "number" },
    { "string", "string" }, { "str", "string" }, { "text", "string" }, { "bool", "bool" }, { "array", "array" }, { "list", "array" },
    { "dict", "dict" }, { "map", "dict" }, { "fn", "fn" } };
  if (!t) return NULL;
  for (size_t i = 0; i < sizeof map / sizeof map[0]; i++) if (!strcmp(map[i][0], t)) return map[i][1];
  return NULL;
}

static const char *literal_kind(AxNode *n) {
  if (!n) return NULL;
  if (n->kind == N_IDENT && (streq(n->str, "true") || streq(n->str, "false"))) return "bool";
  switch (n->kind) {
    case N_NUM: return "number";
    case N_STR: case N_FSTR: return "string";
    case N_ARRAY: return "array";
    case N_DICT: return "dict";
    case N_LAMBDA: return "fn";
    default: return NULL;
  }
}

static void type_report(Ck *c, const char *declared, const char *kind, char *what_h, char *what_a, int line, int col, const char *entity, const char *block) {
  CD *d = emit(c, "AX-TYPE-001", "advisory");
  d->entity = sdup(entity);
  d->block = sdup(block);
  d->line = line; d->col = col;
  d->snippet = source_line(c, line);
  d->rsec = "v0.9.1"; d->rtitle = "Literal contradicts a declared type";
  d->human = fmt("%s is declared `%s` but the value here is a %s.", what_h, declared, kind);
  d->agent = fmt("%s Either pass a %s, change the declared type, or drop the annotation — annotations are documentation, so the runtime will not stop this, but the mismatch is almost always a real mistake.", what_a, declared);
  free(what_h);
  free(what_a);
}

typedef struct { Ck *c; Prog *pg; Body *b; AxNode *st; } LtCtx;

static void lt_visit(AxNode *n, void *vctx) {
  LtCtx *l = vctx;
  if (n->kind != N_CALL || !n->str) return;
  AxNode *td = find_type(l->pg, n->str->data);
  if (!td) return;
  int pos = 0;
  for (int i = 0; i < n->nlist; i++) {
    AxNode *a = n->list[i];
    if (!a->b) continue;
    int fi = -1;
    if (a->str) { for (int k = 0; k < td->nnames; k++) if (ax_str_eq(td->names[k], a->str)) { fi = k; break; } }
    else { if (pos < td->nnames) fi = pos; pos++; }
    if (fi < 0) continue;
    AxNode *ft = fi < td->nlist ? td->list[fi] : NULL;
    if (!ft || !ft->str) continue;
    const char *want = type_alias(ft->str->data), *kind = literal_kind(a->b);
    if (!want || !kind || !strcmp(want, kind)) continue;
    type_report(l->c, ft->str->data, kind, fmt("Field '%s' of %s", td->names[fi]->data, n->str->data),
                fmt("^type %s declares %s:: %s, and this call passes a %s literal.", n->str->data, td->names[fi]->data, ft->str->data, kind),
                l->st->line, l->st->col, body_entity(l->b), body_block(l->b));
  }
}

static void lt_stmt(AxNode *st, void *vctx) {
  LtCtx *l = vctx;
  l->st = st;
  walk_roots(st, lt_visit, l);
}

static void literal_body(Body *b, void *vctx) {
  LtCtx l = *(LtCtx *)vctx;
  l.b = b;
  recurse(b->body, b->nbody, lt_stmt, &l);
}

typedef struct { Ck *c; AxNode *fn; const char *want; } RtCtx;
static void rt_stmt(AxNode *st, void *vctx) {
  RtCtx *r = vctx;
  if (st->kind != N_RETURN || !st->a) return;
  const char *kind = literal_kind(st->a);
  if (!kind || !strcmp(kind, r->want)) return;
  const char *name = S(r->fn->str), *ret = S(r->fn->str2);
  char *block = fmt("^fn %s", name);
  type_report(r->c, ret, kind, fmt("The return value of ^fn %s", name),
              fmt("^fn %s is declared -> %s, but this ^return yields a %s literal.", name, ret, kind),
              st->line, st->col, NULL, block);
  free(block);
}

static void check_literal_types(Ck *c, Prog *pg) {
  LtCtx l = { c, pg, NULL, NULL };
  for_each_body(pg, literal_body, &l);
  for (int i = 0; i < pg->fns.n; i++) {
    AxNode *fn = pg->fns.v[i];
    if (!fn->str2) continue;
    const char *want = type_alias(fn->str2->data);
    if (!want) continue;
    RtCtx r = { c, fn, want };
    recurse(fn->list, fn->nlist, rt_stmt, &r);
  }
}

// ---- ^use (checker.js resolveUses) -----------------------------------------------------------

static const char *decl_key(AxNode *d, char *buf, size_t n) {
  if (d->kind == N_RESOURCE) return S(d->str2);
  if (d->kind == N_GLOBAL) {
    size_t len = 0;
    buf[0] = '\0';
    for (int i = 0; i < d->nnames; i++) len += (size_t)snprintf(buf + len, len < n ? n - len : 0, "%s%s", i ? "," : "", d->names[i]->data);
    return buf;
  }
  return S(d->str);
}

static NV *prog_list(Prog *pg, AxNode *d) {
  switch (d->kind) {
    case N_FN: return d->op == 1 ? &pg->procs : &pg->fns;
    case N_TYPE: return &pg->types;
    case N_EVENT: return &pg->events;
    case N_MIXIN: return &pg->mixins;
    case N_MATERIAL: return &pg->materials;
    case N_ENTITY: return &pg->entities;
    case N_RESOURCE: return &pg->resources;
    case N_GLOBAL: return &pg->globals;
    default: return NULL;
  }
}

typedef struct { char **v; int n; } Visited;

static bool visited_has(Visited *v, const char *p) { for (int i = 0; i < v->n; i++) if (!strcmp(v->v[i], p)) return true; return false; }
static void visited_add(Visited *v, const char *p) { v->v = realloc(v->v, sizeof(char *) * (v->n + 1)); v->v[v->n++] = strdup(p); }

// path.resolve(dir, rel), lexically.
static char *resolve_path(const char *dir, const char *rel) {
  char buf[PATH_MAX * 2];
  if (rel[0] == '/') snprintf(buf, sizeof buf, "%s", rel);
  else snprintf(buf, sizeof buf, "%s/%s", dir, rel);
  // normalise . and ..
  char *parts[512];
  int np = 0;
  char *save = NULL;
  for (char *t = strtok_r(buf, "/", &save); t; t = strtok_r(NULL, "/", &save)) {
    if (!strcmp(t, ".") || !*t) continue;
    if (!strcmp(t, "..")) { if (np) np--; continue; }
    if (np < 512) parts[np++] = t;
  }
  size_t len = 0;
  char *out = malloc(PATH_MAX * 2);
  out[0] = '\0';
  for (int i = 0; i < np; i++) len += (size_t)snprintf(out + len, PATH_MAX * 2 - len, "/%s", parts[i]);
  if (!np) snprintf(out, PATH_MAX * 2, "/");
  return out;
}

static char *dir_of(const char *path) {
  char *d = strdup(path);
  char *s = strrchr(d, '/');
  if (s == d) d[1] = '\0';
  else if (s) *s = '\0';
  return d;
}

static char *read_all(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  size_t cap = 4096, n = 0;
  char *b = malloc(cap);
  size_t got;
  while ((got = fread(b + n, 1, cap - n - 1, f)) > 0) { n += got; if (n + 1 >= cap) { cap *= 2; b = realloc(b, cap); } }
  b[n] = '\0';
  fclose(f);
  return b;
}

static void merge_decls(Ck *c, Prog *root, AxNode *sub, const char *from) {
  // In the reference's list order: fns, procs, types, events, mixins, materials, entities,
  // resources, globals — each checked against what the program already has.
  static const int kinds[] = { N_FN, N_FN, N_TYPE, N_EVENT, N_MIXIN, N_MATERIAL, N_ENTITY, N_RESOURCE, N_GLOBAL };
  for (int li = 0; li < 9; li++) {
    for (int i = 0; i < sub->nlist; i++) {
      AxNode *d = sub->list[i];
      if (d->kind != kinds[li]) continue;
      if (d->kind == N_FN && (li == 0) != (d->op == 0)) continue;
      NV *l = prog_list(root, d);
      char b1[1024], b2[1024];
      const char *key = decl_key(d, b1, sizeof b1);
      bool have = false;
      for (int k = 0; k < l->n && !have; k++) { const char *other = decl_key(l->v[k], b2, sizeof b2); have = key && other && !strcmp(key, other); }
      if (have) {
        CD *dd = emit(c, "AX-USE-002", "advisory");
        dd->line = d->kind == N_ENTITY ? 0 : d->line;
        dd->col = d->kind == N_ENTITY ? 0 : d->col;
        dd->rsec = "v0.9.0"; dd->rtitle = "Imported declaration shadowed";
        dd->human = fmt("'%s' is declared both locally and in '%s' — the local declaration wins.", key, from);
        dd->agent = fmt("The import '%s' declares '%s', which this file also declares. AxiomScript imports share one flat namespace and the importing file takes precedence. Rename one of them if the shadowing was not intentional.", from, key);
        continue;
      }
      nv_push(l, d);
    }
  }
}

static void use_visit(Ck *c, Prog *root, AxNode **uses, int nuses, const char *dir, Visited *visited) {
  for (int u = 0; u < nuses; u++) {
    AxNode *use = uses[u];
    for (int p = 0; p < use->nlist; p++) {
      const char *rel = use->list[p]->str->data;
      char *resolved = resolve_path(dir, rel);
      if (access(resolved, F_OK) != 0) {
        char *alt = fmt("%s.ax", resolved);
        if (access(alt, F_OK) == 0) { free(resolved); resolved = alt; } else free(alt);
      }
      if (visited_has(visited, resolved)) { free(resolved); continue; }
      if (access(resolved, F_OK) != 0) {
        CD *d = emit(c, "AX-USE-001", "fatal");
        d->line = use->line; d->col = use->col;
        d->rsec = "v0.9.0"; d->rtitle = "Import not found";
        d->human = fmt("^use \"%s\" — no such file (looked in %s).", rel, dir);
        d->agent = fmt("The import path is resolved relative to the importing file's directory, and '.ax' is appended if the literal path does not exist. Create '%s' or fix the path.", rel);
        free(resolved);
        continue;
      }
      visited_add(visited, resolved);
      char *src = read_all(resolved);
      if (!src) {
        CD *d = emit(c, "AX-USE-001", "fatal");
        d->line = use->line; d->col = use->col;
        d->rsec = "v0.9.0"; d->rtitle = "Import unreadable";
        d->human = fmt("^use \"%s\" — could not read the file.", rel);
        d->agent = fmt("Check permissions on '%s'.", resolved);
        free(resolved);
        continue;
      }
      AxTokens *toks = calloc(1, sizeof *toks);
      AxParseResult *pr = calloc(1, sizeof *pr);
      bool ok = ax_tokenize(src, toks);
      if (ok) ok = ax_parse(toks, pr);
      if (!ok) {
        CD *d = emit(c, "AX-USE-003", "fatal");
        d->line = use->line; d->col = use->col;
        d->rsec = "v0.9.0"; d->rtitle = "Import failed to parse";
        int eline = toks->err[0] ? toks->err_line : pr->err_line;
        d->human = fmt("^use \"%s\" (line %d): %s", rel, eline, toks->err[0] ? toks->err : pr->err);
        d->agent = fmt("Fix the syntax error inside '%s' at its line %d.", rel, eline);
        free(resolved);
        continue;
      }
      NV subuses = { 0 };
      bool has_main = false;
      for (int i = 0; i < pr->program->nlist; i++) {
        if (pr->program->list[i]->kind == N_USE) nv_push(&subuses, pr->program->list[i]);
        if (pr->program->list[i]->kind == N_MAIN) has_main = true;
      }
      char *subdir = dir_of(resolved);
      use_visit(c, root, subuses.v, subuses.n, subdir, visited);   // depth first
      free(subdir);
      free(subuses.v);
      merge_decls(c, root, pr->program, rel);
      if (has_main) {
        CD *d = emit(c, "AX-USE-004", "advisory");
        d->line = use->line; d->col = use->col;
        d->rsec = "v0.9.0"; d->rtitle = "Imported ^main ignored";
        d->human = fmt("'%s' declares a ^main — only the entry file's ^main runs.", rel);
        d->agent = fmt("A ^main in an imported file is ignored. Move the shared code into a ^fn/^proc so both programs can call it.");
      }
      free(resolved);
    }
  }
}

// ---- entry point ---------------------------------------------------------------------------

AxCheck *ax_check(AxNode *program, const char *source, const char *filename, const char *version,
                  const char *parse_err, int parse_line, int parse_col) {
  AxCheck *out = calloc(1, sizeof *out);
  Ck c = { source, filename, out };
  if (parse_err) {
    CD *d = emit(&c, "AX-PARSE-000", "fatal");
    d->line = parse_line; d->col = parse_col;
    d->snippet = source_line(&c, parse_line);
    d->rsec = "Appendix B"; d->rtitle = "Minimal Grammar Sketch";
    d->human = fmt("Parse error: %s", parse_err);
    d->agent = fmt("Fix syntax at or near this line.");
    out->ok = false;
    return out;
  }
  Prog pg = { 0 };
  pg.version = version;
  for (int i = 0; i < program->nlist; i++) prog_add(&pg, program->list[i]);
  // Imports first, so an imported function counts as declared everywhere.
  if (pg.uses.n) {
    char *dir;
    Visited visited = { 0 };
    if (filename) {
      char cwd[PATH_MAX];
      if (!getcwd(cwd, sizeof cwd)) snprintf(cwd, sizeof cwd, ".");
      char *abs = resolve_path(cwd, filename);
      visited_add(&visited, abs);
      dir = dir_of(abs);
      free(abs);
    } else {
      char cwd[PATH_MAX];
      if (!getcwd(cwd, sizeof cwd)) snprintf(cwd, sizeof cwd, ".");
      dir = strdup(cwd);
    }
    use_visit(&c, &pg, pg.uses.v, pg.uses.n, dir, &visited);
    free(dir);
    for (int i = 0; i < visited.n; i++) free(visited.v[i]);
    free(visited.v);
  }
  for_each_block(&pg, za_block, &c);
  for_each_block(&pg, sealed_block, &c);
  EvCtx ev = { &c, &pg, NULL, NULL };
  for_each_block(&pg, ev_block, &ev);
  check_infer(&c, &pg);
  check_pools(&c, &pg);
  check_functions(&c, &pg);
  check_loops(&c, &pg);
  check_mixins(&c, &pg);
  for_each_block(&pg, unknown_block, &c);
  for_each_block(&pg, fn_only_block, &c);
  check_broadcasts(&c, &pg);
  check_version(&c, &pg);
  check_inline_meshes(&c, &pg);
  check_xwrites(&c, &pg);
  check_undefined_fns(&c, &pg);
  for_each_block(&pg, qa_block, &c);
  check_entry(&c, &pg);
  check_undefined_in_bodies(&c, &pg);
  check_arity(&c, &pg);
  check_record_fields(&c, &pg);
  check_unknown_names(&c, &pg);
  check_literal_types(&c, &pg);
  out->ok = true;
  for (int i = 0; i < out->n; i++) if (!strcmp(out->v[i].severity, "fatal") || !strcmp(out->v[i].severity, "contract_violation")) out->ok = false;
  free(pg.entities.v); free(pg.events.v); free(pg.types.v); free(pg.fns.v); free(pg.procs.v);
  free(pg.mixins.v); free(pg.materials.v); free(pg.globals.v); free(pg.resources.v); free(pg.uses.v);
  return out;
}

bool ax_check_ok(const AxCheck *c) { return c->ok; }
int ax_check_count(const AxCheck *c) { return c->n; }
int ax_check_blocking(const AxCheck *c) {
  int n = 0;
  for (int i = 0; i < c->n; i++) if (!strcmp(c->v[i].severity, "fatal") || !strcmp(c->v[i].severity, "contract_violation")) n++;
  return n;
}

// `  [CODE] (severity) Lline:col message` — main.js's format (a missing position prints null).
void ax_check_print(const AxCheck *c, FILE *out) {
  for (int i = 0; i < c->n; i++) {
    const CD *d = &c->v[i];
    char line[16], col[16];
    if (d->line) snprintf(line, sizeof line, "%d", d->line); else snprintf(line, sizeof line, "null");
    if (d->col) snprintf(col, sizeof col, "%d", d->col); else snprintf(col, sizeof col, "null");
    fprintf(out, "  [%s] (%s) L%s:%s %s\n", d->code, d->severity, line, col, d->human);
  }
}

static void jstr(FILE *o, const char *s) {
  if (!s) { fputs("null", o); return; }
  fputc('"', o);
  for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
    switch (*p) {
      case '"': fputs("\\\"", o); break;
      case '\\': fputs("\\\\", o); break;
      case '\n': fputs("\\n", o); break;
      case '\r': fputs("\\r", o); break;
      case '\t': fputs("\\t", o); break;
      case '\b': fputs("\\b", o); break;
      case '\f': fputs("\\f", o); break;
      default:
        if (*p < 0x20) fprintf(o, "\\u%04x", *p);
        else fputc(*p, o);
    }
  }
  fputc('"', o);
}

static void jint(FILE *o, int v) { if (v) fprintf(o, "%d", v); else fputs("null", o); }

// JSON.stringify({ ok, diagnostics }, null, 2)
void ax_check_print_json(const AxCheck *c, FILE *o) {
  fprintf(o, "{\n  \"ok\": %s,\n  \"diagnostics\": ", c->ok ? "true" : "false");
  if (!c->n) { fputs("[]\n}\n", o); return; }
  fputs("[\n", o);
  for (int i = 0; i < c->n; i++) {
    const CD *d = &c->v[i];
    fputs("    {\n      \"error_code\": ", o); jstr(o, d->code);
    fputs(",\n      \"severity\": ", o); jstr(o, d->severity);
    fputs(",\n      \"location\": {\n        \"entity\": ", o); jstr(o, d->entity);
    fputs(",\n        \"block\": ", o); jstr(o, d->block);
    fputs(",\n        \"line\": ", o); jint(o, d->line);
    fputs(",\n        \"col\": ", o); jint(o, d->col);
    fputs("\n      },\n      \"violated_rule\": {\n        \"section\": ", o); jstr(o, d->rsec);
    fputs(",\n        \"title\": ", o); jstr(o, d->rtitle);
    fputs("\n      },\n      \"context_snippet\": ", o); jstr(o, d->snippet);
    fputs(",\n      \"message_for_human\": ", o); jstr(o, d->human);
    fputs(",\n      \"message_for_agent\": ", o); jstr(o, d->agent);
    fputs(",\n      \"suggested_fix\": ", o);
    if (d->fixk == 1 || d->fixk == 2) {
      fputs("{\n        \"kind\": ", o); jstr(o, d->fixk == 1 ? "text" : "patch");
      fputs(",\n        \"detail\": ", o); jstr(o, d->fix);
      fputs("\n      }", o);
    } else if (d->fixk == 3) jstr(o, d->fix);
    else fputs("null", o);
    fprintf(o, ",\n      \"auto_fixable\": %s\n    }%s\n", d->autofix ? "true" : "false", i + 1 < c->n ? "," : "");
  }
  fputs("  ]\n}\n", o);
}

void ax_check_free(AxCheck *c) {
  if (!c) return;
  for (int i = 0; i < c->n; i++) {
    CD *d = &c->v[i];
    free(d->entity); free(d->block); free(d->snippet); free(d->human); free(d->agent); free(d->fix);
  }
  free(c->v);
  free(c);
}
