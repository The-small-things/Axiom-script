// interp.c — the evaluator: expressions, statements, calls, patterns, errors.
//
// A tree walker, like the reference implementation, and for the same reason: the language's
// semantics are defined by what this does, and a tree walker is the form in which those
// semantics are readable. The speed comes from the value model (16-byte values, interned names,
// pointer-compare scope lookup) rather than from a bytecode compiler, which would be the next
// step if profiling ever justified the loss in clarity.
//
// Conventions, applied without exception:
//   * `ax_eval` returns an owned value (+1); every caller releases it.
//   * control flow (break/continue/return) is an ordinary return code, because it is common;
//     errors (`^throw`, a bad index) unwind with longjmp, because they are not.
//   * a scope frame is created per call, per loop iteration that captures, and per match arm.

#include "axiom.h"
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <math.h>

// Names compared by pointer on the hot path.
static AxStr *S_null, *S_true, *S_false, *S_args, *S_msg, *S_code, *S_value, *S_type, *S_underscore, *S_it;

static void init_names(void) {
  if (S_null) return;
  S_null = ax_internz("null");
  S_true = ax_internz("true");
  S_false = ax_internz("false");
  S_args = ax_internz("args");
  S_msg = ax_internz("msg");
  S_code = ax_internz("code");
  S_value = ax_internz("value");
  S_type = ax_internz("__type");
  S_underscore = ax_internz("_");
  S_it = ax_internz("it");
}

// ---------------------------------------------------------------------------------------------
// Errors
// ---------------------------------------------------------------------------------------------

void ax_throw_value(AxVM *vm, AxValue v) {
  ax_release(vm->error);
  vm->error = v;
  if (vm->nhandlers > 0) longjmp(vm->handlers[vm->nhandlers - 1], 1);
  // Unhandled: print and stop. The message is the one a model will read, so it names the code.
  AxValue msg;
  const char *text = vm->error_msg;
  if (v.t == AX_DICT && ax_dict_get((AxDict *)v.o, S_msg, &msg)) {
    AxStr *s = ax_to_str(msg);
    fprintf(stderr, "runtime error [%s]: %s\n", vm->error_code[0] ? vm->error_code : "AX-THROW", s->data);
    ax_release(ax_strv(s));
    ax_release(msg);
  } else {
    AxStr *s = ax_to_str(v);
    fprintf(stderr, "runtime error [%s]: %s\n", vm->error_code[0] ? vm->error_code : "AX-THROW", text[0] ? text : s->data);
    ax_release(ax_strv(s));
  }
  exit(1);
}

void ax_throw(AxVM *vm, const char *code, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(vm->error_msg, sizeof vm->error_msg, fmt, ap);
  va_end(ap);
  snprintf(vm->error_code, sizeof vm->error_code, "%s", code);
  AxDict *d = ax_dict_new();
  ax_dict_set(d, S_msg, ax_str_from(vm->error_msg));
  ax_dict_set(d, S_code, ax_str_from(code));
  ax_dict_set(d, S_value, ax_null());
  ax_throw_value(vm, ax_dictv(d));
}

// ---------------------------------------------------------------------------------------------
// Operators
// ---------------------------------------------------------------------------------------------

static AxValue concat_values(AxValue a, AxValue b) {
  AxStr *sa = ax_to_str(a), *sb = ax_to_str(b);
  AxStr *r = ax_str_concat(sa, sb);
  ax_release(ax_strv(sa));
  ax_release(ax_strv(sb));
  return ax_strv(r);
}

static bool value_in(AxVM *vm, AxValue needle, AxValue hay) {
  switch (hay.t) {
    case AX_NULL: return false;
    case AX_STR: {
      AxStr *h = (AxStr *)hay.o;
      AxStr *n = ax_to_str(needle);
      bool found = n->len == 0 || (h->len >= n->len && strstr(h->data, n->data) != NULL);
      ax_release(ax_strv(n));
      return found;
    }
    case AX_ARR: {
      AxArr *a = (AxArr *)hay.o;
      for (uint32_t i = 0; i < a->len; i++) if (ax_equals(a->items[i], needle)) return true;
      return false;
    }
    case AX_DICT: {
      AxStr *k = ax_to_str(needle);
      bool found = ax_dict_has((AxDict *)hay.o, k);
      ax_release(ax_strv(k));
      return found;
    }
    case AX_RANGE: {
      AxRange *r = (AxRange *)hay.o;
      double n = ax_to_num(needle);
      return r->step > 0 ? (n >= r->lo && n < r->hi) : (n <= r->lo && n > r->hi);
    }
    default: return false;
  }
}

static AxValue binary_op(AxVM *vm, int op, AxValue l, AxValue r) {
  switch (op) {
    case OP_ADD:
      if (l.t == AX_STR || r.t == AX_STR) return concat_values(l, r);
      if (l.t == AX_ARR && r.t == AX_ARR) {
        AxArr *a = (AxArr *)l.o, *b = (AxArr *)r.o;
        AxArr *out = ax_arr_new(a->len + b->len);
        for (uint32_t i = 0; i < a->len; i++) ax_arr_push(out, ax_copy(a->items[i]));
        for (uint32_t i = 0; i < b->len; i++) ax_arr_push(out, ax_copy(b->items[i]));
        return ax_arrv(out);
      }
      return ax_num(ax_to_num(l) + ax_to_num(r));
    case OP_SUB: return ax_num(ax_to_num(l) - ax_to_num(r));
    case OP_MUL: return ax_num(ax_to_num(l) * ax_to_num(r));
    case OP_DIV: return ax_num(ax_to_num(l) / ax_to_num(r));
    case OP_MOD: return ax_num(fmod(ax_to_num(l), ax_to_num(r)));
    case OP_POW: return ax_num(pow(ax_to_num(l), ax_to_num(r)));
    case OP_EQ: return ax_bool(ax_equals(l, r));
    case OP_NE: return ax_bool(!ax_equals(l, r));
    case OP_GT: case OP_LT: case OP_GE: case OP_LE: {
      // Numbers compare numerically, strings lexicographically, as in the reference.
      bool numeric = (l.t == AX_NUM || l.t == AX_BOOL || l.t == AX_NULL) && (r.t == AX_NUM || r.t == AX_BOOL || r.t == AX_NULL);
      int c;
      if (numeric) {
        double a = ax_to_num(l), b = ax_to_num(r);
        if (isnan(a) || isnan(b)) return ax_bool(false);
        c = a < b ? -1 : (a > b ? 1 : 0);
      } else {
        c = ax_compare(l, r);
      }
      switch (op) {
        case OP_GT: return ax_bool(c > 0);
        case OP_LT: return ax_bool(c < 0);
        case OP_GE: return ax_bool(c >= 0);
        default: return ax_bool(c <= 0);
      }
    }
    case OP_IN: return ax_bool(value_in(vm, l, r));
    case OP_RANGE: return ax_range(ax_to_num(l), ax_to_num(r), 1);
    default:
      ax_throw(vm, "AX-RUNTIME-OP", "unknown operator");
      return ax_null();
  }
}

// ---------------------------------------------------------------------------------------------
// Sequences
// ---------------------------------------------------------------------------------------------

// An array view of any iterable. Returns a NEW array (+1) except for arrays, which are shared.
AxArr *ax_to_seq(AxVM *vm, AxValue v) {
  switch (v.t) {
    case AX_ARR: ax_retain(v); return (AxArr *)v.o;
    case AX_STR: {
      AxStr *s = (AxStr *)v.o;
      AxArr *a = ax_arr_new(s->len);
      for (uint32_t i = 0; i < s->len; i++) ax_arr_push(a, ax_strv(ax_str_new(s->data + i, 1)));
      return a;
    }
    case AX_RANGE: {
      AxRange *r = (AxRange *)v.o;
      AxArr *a = ax_arr_new(8);
      if (r->step > 0) for (double x = r->lo; x < r->hi; x += r->step) ax_arr_push(a, ax_num(x));
      else for (double x = r->lo; x > r->hi; x += r->step) ax_arr_push(a, ax_num(x));
      return a;
    }
    case AX_DICT: {
      // Iterating a dict yields its values, matching `values(d)`; `items(d)` gives pairs.
      AxDict *d = (AxDict *)v.o;
      AxArr *a = ax_arr_new(d->live);
      for (uint32_t i = 0; i < d->len; i++) {
        if (d->entries[i].dead) continue;
        ax_arr_push(a, ax_copy(d->entries[i].val));
      }
      return a;
    }
    case AX_NULL: return ax_arr_new(0);
    default: {
      AxArr *a = ax_arr_new(1);
      ax_arr_push(a, ax_copy(v));
      return a;
    }
  }
}

AxValue ax_index_get(AxVM *vm, AxValue obj, AxValue idx) {
  switch (obj.t) {
    case AX_ARR: {
      AxArr *a = (AxArr *)obj.o;
      double d = ax_to_num(idx);
      int64_t i = (int64_t)d;
      if (i < 0) i += a->len;                     // negative indices count from the end
      return ax_arr_get(a, i);
    }
    case AX_STR: {
      AxStr *s = (AxStr *)obj.o;
      int64_t i = (int64_t)ax_to_num(idx);
      if (i < 0) i += s->len;
      if (i < 0 || (uint64_t)i >= s->len) return ax_null();
      return ax_strv(ax_str_new(s->data + i, 1));
    }
    case AX_DICT: {
      AxStr *k = ax_to_str(idx);
      AxValue out;
      bool found = ax_dict_get((AxDict *)obj.o, k, &out);
      ax_release(ax_strv(k));
      return found ? out : ax_null();
    }
    case AX_NULL:
      ax_throw(vm, "AX-RUNTIME-INDEX", "cannot index null — check the value with is_null() or ?? first");
      return ax_null();
    default:
      ax_throw(vm, "AX-RUNTIME-INDEX", "cannot index %s", ax_type_name(obj));
      return ax_null();
  }
}

// ---------------------------------------------------------------------------------------------
// Calls
// ---------------------------------------------------------------------------------------------

#define AX_MAX_DEPTH 2500

static AxValue eval_node(AxVM *vm, AxNode *n, AxScope *scope);
static int exec_list(AxVM *vm, AxNode **stmts, int n, AxScope *scope, AxValue *out);

AxValue ax_call(AxVM *vm, AxValue fnv, AxValue *args, int argc) {
  if (fnv.t != AX_FN) {
    ax_throw(vm, "AX-CALL-001", "%s is not callable", ax_type_name(fnv));
    return ax_null();
  }
  AxFn *f = (AxFn *)fnv.o;
  if (f->native) {
    if (argc < f->min_args) {
      ax_throw(vm, "AX-ARITY-001", "%s() needs at least %d argument%s, got %d",
               f->name, f->min_args, f->min_args == 1 ? "" : "s", argc);
    }
    return f->fn(vm, f, args, argc);
  }
  if (++vm->call_depth > AX_MAX_DEPTH) {
    vm->call_depth = 0;
    ax_throw(vm, "AX-DEPTH-001", "recursion depth exceeded %d — add a base case, or rewrite the recursion as a loop", AX_MAX_DEPTH);
  }
  AxScope *s = ax_scope_new(f->scope, true);
  int off = 0;
  if (f->has_bound) {
    if (f->nparams > 0) ax_scope_declare(s, f->params[0], ax_copy(f->bound));
    off = 1;
  }
  for (int i = off; i < f->nparams; i++) {
    int ai = i - off;
    AxValue v = ax_null();
    if (ai < argc && !(args[ai].t == AX_NULL && f->defaults && f->defaults[i])) v = ax_copy(args[ai]);
    else if (f->defaults && f->defaults[i]) v = eval_node(vm, f->defaults[i], s);
    ax_scope_declare(s, f->params[i], v);
  }
  // `args` is always available, which is the language's variadic escape hatch.
  AxArr *all = ax_arr_new(argc);
  for (int i = 0; i < argc; i++) ax_arr_push(all, ax_copy(args[i]));
  ax_scope_declare(s, S_args, ax_arrv(all));

  AxValue result = ax_null();
  if (f->is_expr) {
    result = eval_node(vm, f->body, s);
  } else {
    AxValue ret = ax_null();
    int flow = exec_list(vm, f->body->list, f->body->nlist, s, &ret);
    if (flow == AX_FLOW_RETURN) result = ret;
    else ax_release(ret);
  }
  ax_scope_release(s);
  vm->call_depth--;
  return result;
}

// A selector may be a function, a field name, or absent — the library's uniform callback rule.
AxValue ax_key_apply(AxVM *vm, AxValue sel, AxValue item, double index) {
  if (sel.t == AX_NULL) return ax_copy(item);
  if (sel.t == AX_FN) {
    AxValue args[2] = { item, ax_num(index) };
    return ax_call(vm, sel, args, 2);
  }
  if (sel.t == AX_STR || sel.t == AX_ATOM) {
    if (item.t == AX_DICT) {
      AxValue out;
      if (ax_dict_get((AxDict *)item.o, (AxStr *)sel.o, &out)) return out;
      return ax_null();
    }
    return ax_null();
  }
  return ax_copy(sel);
}

// ---------------------------------------------------------------------------------------------
// Assignment
// ---------------------------------------------------------------------------------------------

static AxScope *fn_frame(AxScope *s) {
  while (s && !s->fn_root && s->parent) s = s->parent;
  return s;
}

static void write_var(AxVM *vm, AxScope *scope, AxStr *name, AxValue v, bool declare_local) {
  if (declare_local) { ax_scope_declare(scope, name, v); return; }
  if (ax_scope_set_existing(scope, name, v)) return;
  ax_scope_declare(fn_frame(scope), name, v);
}

static AxValue read_var(AxVM *vm, AxScope *scope, AxStr *name) {
  AxValue out;
  if (ax_scope_lookup(scope, name, &out)) return out;
  return ax_null();
}

// ---------------------------------------------------------------------------------------------
// Pattern matching
// ---------------------------------------------------------------------------------------------

typedef struct { AxStr *name; AxValue val; } Binding;
typedef struct { Binding *items; int n, cap; } Bindings;

static void bind_push(Bindings *b, AxStr *name, AxValue v) {
  if (b->n == b->cap) { b->cap = b->cap ? b->cap * 2 : 8; b->items = realloc(b->items, sizeof(Binding) * b->cap); }
  b->items[b->n].name = name;
  b->items[b->n].val = v;
  b->n++;
}

static void bind_free(Bindings *b) {
  for (int i = 0; i < b->n; i++) ax_release(b->items[i].val);
  free(b->items);
  b->items = NULL;
  b->n = b->cap = 0;
}

static bool is_type_name(AxVM *vm, AxStr *name) { return ax_dict_has(vm->types, name); }

// Inside a pattern a bare name binds; `_` matches without binding; a declared ^type name
// matches by type; everything else is an expression compared by value. See GRAMMAR.md.
static bool match_inner(AxVM *vm, AxValue subject, AxNode *pat, AxScope *scope, Bindings *out) {
  switch (pat->kind) {
    case N_IDENT: {
      if (pat->str == S_underscore) return true;
      if (is_type_name(vm, pat->str)) {
        if (subject.t != AX_DICT) return false;
        AxDict *d = (AxDict *)subject.o;
        return d->type_tag && ax_str_eq(d->type_tag, pat->str);
      }
      bind_push(out, pat->str, ax_copy(subject));
      return true;
    }
    case N_CALL: {
      AxValue fields;
      if (!ax_dict_get(vm->types, pat->str, &fields)) {
        AxValue v = eval_node(vm, pat, scope);
        bool eq = ax_equals(subject, v);
        ax_release(v);
        return eq;
      }
      AxArr *fnames = (AxArr *)fields.o;
      bool ok = true;
      if (subject.t != AX_DICT) ok = false;
      if (ok) {
        AxDict *d = (AxDict *)subject.o;
        if (d->type_tag && !ax_str_eq(d->type_tag, pat->str)) ok = false;
        int pos = 0;
        for (int i = 0; ok && i < pat->nlist; i++) {
          AxNode *arg = pat->list[i];
          AxStr *field = NULL;
          if (arg->str) field = arg->str;
          else if (pos < (int)fnames->len) field = (AxStr *)fnames->items[pos++].o;
          if (!field) { ok = false; break; }
          AxValue fv;
          if (!ax_dict_get(d, field, &fv)) fv = ax_null();
          ok = match_inner(vm, fv, arg->b, scope, out);
          ax_release(fv);
        }
      }
      ax_release(fields);
      return ok;
    }
    case N_ARRAY: {
      if (subject.t != AX_ARR) return false;
      AxArr *a = (AxArr *)subject.o;
      if ((int)a->len != pat->nlist) return false;
      for (int i = 0; i < pat->nlist; i++) {
        if (!match_inner(vm, a->items[i], pat->list[i], scope, out)) return false;
      }
      return true;
    }
    case N_DICT: {
      if (subject.t != AX_DICT) return false;
      AxDict *d = (AxDict *)subject.o;
      for (int i = 0; i < pat->nlist; i++) {
        AxNode *e = pat->list[i];
        AxStr *key = e->str;
        AxValue computed = ax_null();
        if (!key && e->a) { computed = eval_node(vm, e->a, scope); key = ax_to_str(computed); }
        AxValue fv;
        bool present = key && ax_dict_get(d, key, &fv);
        if (!key || !present) {
          if (computed.t != AX_NULL) ax_release(computed);
          return false;
        }
        bool ok = match_inner(vm, fv, e->b, scope, out);
        ax_release(fv);
        if (computed.t != AX_NULL) ax_release(computed);
        if (!ok) return false;
      }
      return true;
    }
    default: {
      AxValue v = eval_node(vm, pat, scope);
      bool eq = ax_equals(subject, v);
      ax_release(v);
      return eq;
    }
  }
}

// At the top level of an arm a bare name COMPARES (the atom idiom), except in a guarded arm,
// where it captures the subject.
static bool match_top(AxVM *vm, AxValue subject, AxNode *pat, AxScope *scope, Bindings *out, bool guarded) {
  if (pat->kind == N_IDENT) {
    if (pat->str == S_underscore) return true;
    if (is_type_name(vm, pat->str)) {
      if (subject.t != AX_DICT) return false;
      AxDict *d = (AxDict *)subject.o;
      return d->type_tag && ax_str_eq(d->type_tag, pat->str);
    }
    if (guarded) { bind_push(out, pat->str, ax_copy(subject)); return true; }
    AxValue v = eval_node(vm, pat, scope);
    bool eq = ax_equals(subject, v);
    ax_release(v);
    return eq;
  }
  return match_inner(vm, subject, pat, scope, out);
}

// ---------------------------------------------------------------------------------------------
// Expression evaluation
// ---------------------------------------------------------------------------------------------

static AxValue make_closure(AxVM *vm, AxNode *n, AxScope *scope, bool is_expr) {
  AxFn *f = calloc(1, sizeof(AxFn));
  f->hdr.rc = 1;
  f->hdr.type = AX_FN;
  f->native = false;
  f->is_expr = is_expr;
  f->nparams = n->nnames;
  f->name = n->str ? n->str->data : "lambda";
  if (n->nnames) {
    f->params = calloc(n->nnames, sizeof(AxStr *));
    f->defaults = calloc(n->nnames, sizeof(AxNode *));
    for (int i = 0; i < n->nnames; i++) {
      f->params[i] = n->names[i];
      ax_retain(ax_strv(n->names[i]));
      if (n->defaults) f->defaults[i] = n->defaults[i];
    }
  }
  if (is_expr) {
    f->body = n->a;
  } else {
    // A statement body is stored as a BODY node so the call path has one shape to handle.
    AxNode *body = calloc(1, sizeof(AxNode));   // outlives the AST arena intentionally: freed with the fn
    body->kind = N_BLOCK;
    body->list = n->list;
    body->nlist = n->nlist;
    f->body = body;
  }
  f->scope = scope;
  if (scope) scope->hdr.rc++;
  return ax_fnv(f);
}

static AxValue eval_call_named(AxVM *vm, AxNode *n, AxScope *scope);

static AxValue eval_node(AxVM *vm, AxNode *n, AxScope *scope) {
  switch (n->kind) {
    case N_NUM: return ax_num(n->num);
    case N_STR: { ax_retain(ax_strv(n->str)); return ax_strv(n->str); }
    case N_FSTR: {
      AxStr *acc = ax_str_new("", 0);
      for (int i = 0; i < n->nlist; i++) {
        AxNode *part = n->list[i];
        AxStr *piece;
        if (part->flag) { piece = part->str; ax_retain(ax_strv(piece)); }
        else { AxValue v = eval_node(vm, part, scope); piece = ax_to_str(v); ax_release(v); }
        AxStr *next = ax_str_concat(acc, piece);
        ax_release(ax_strv(acc));
        ax_release(ax_strv(piece));
        acc = next;
      }
      return ax_strv(acc);
    }
    case N_IDENT: {
      AxValue out;
      if (ax_scope_lookup(scope, n->str, &out)) return out;
      if (n->str == S_null) return ax_null();
      if (n->str == S_true) return ax_bool(true);
      if (n->str == S_false) return ax_bool(false);
      // An unbound name is an atom — the language's symbol type (`state = idle`).
      ax_retain(ax_strv(n->str));
      return ax_atom(n->str);
    }
    case N_ARRAY: {
      AxArr *a = ax_arr_new(n->nlist);
      for (int i = 0; i < n->nlist; i++) ax_arr_push(a, eval_node(vm, n->list[i], scope));
      return ax_arrv(a);
    }
    case N_DICT: {
      AxDict *d = ax_dict_new();
      for (int i = 0; i < n->nlist; i++) {
        AxNode *e = n->list[i];
        AxStr *key;
        if (e->str) { key = e->str; ax_retain(ax_strv(key)); }
        else { AxValue kv = eval_node(vm, e->a, scope); key = ax_to_str(kv); ax_release(kv); }
        ax_dict_set(d, key, eval_node(vm, e->b, scope));
        ax_release(ax_strv(key));
      }
      return ax_dictv(d);
    }
    case N_LAMBDA: return make_closure(vm, n, scope, true);
    case N_BINARY: {
      if (n->op == OP_AND) {
        AxValue l = eval_node(vm, n->a, scope);
        if (!ax_truthy(l)) return l;
        ax_release(l);
        return eval_node(vm, n->b, scope);
      }
      if (n->op == OP_OR) {
        AxValue l = eval_node(vm, n->a, scope);
        if (ax_truthy(l)) return l;
        ax_release(l);
        return eval_node(vm, n->b, scope);
      }
      if (n->op == OP_COALESCE) {
        AxValue l = eval_node(vm, n->a, scope);
        if (l.t != AX_NULL) return l;
        ax_release(l);
        return eval_node(vm, n->b, scope);
      }
      AxValue l = eval_node(vm, n->a, scope);
      AxValue r = eval_node(vm, n->b, scope);
      AxValue res = binary_op(vm, n->op, l, r);
      ax_release(l);
      ax_release(r);
      return res;
    }
    case N_UNARY: {
      AxValue v = eval_node(vm, n->a, scope);
      AxValue res;
      if (n->op == OP_NOT) res = ax_bool(!ax_truthy(v));
      else res = ax_num(-ax_to_num(v));
      ax_release(v);
      return res;
    }
    case N_TERNARY: {
      AxValue c = eval_node(vm, n->a, scope);
      bool t = ax_truthy(c);
      ax_release(c);
      return eval_node(vm, t ? n->b : n->c, scope);
    }
    case N_MEMBER: {
      AxValue obj = eval_node(vm, n->a, scope);
      AxValue out = ax_null();
      if (obj.t == AX_DICT) {
        if (!ax_dict_get((AxDict *)obj.o, n->str, &out)) out = ax_null();
      } else if (obj.t == AX_STR && strcmp(n->str->data, "length") == 0) {
        out = ax_num(((AxStr *)obj.o)->len);
      } else if (obj.t == AX_ARR && strcmp(n->str->data, "length") == 0) {
        out = ax_num(((AxArr *)obj.o)->len);
      } else if (obj.t == AX_NULL) {
        ax_release(obj);
        ax_throw(vm, "AX-RUNTIME-MEMBER", "cannot read '.%s' of null", n->str->data);
      }
      ax_release(obj);
      return out;
    }
    case N_INDEX: {
      AxValue obj = eval_node(vm, n->a, scope);
      AxValue idx = eval_node(vm, n->b, scope);
      AxValue out = ax_index_get(vm, obj, idx);
      ax_release(obj);
      ax_release(idx);
      return out;
    }
    case N_METHOD: {
      AxValue obj = eval_node(vm, n->a, scope);
      AxValue *args = n->nlist ? calloc(n->nlist, sizeof(AxValue)) : NULL;
      for (int i = 0; i < n->nlist; i++) args[i] = eval_node(vm, n->list[i]->b, scope);
      AxValue out = ax_method_call(vm, obj, n->str, args, n->nlist);
      for (int i = 0; i < n->nlist; i++) ax_release(args[i]);
      free(args);
      ax_release(obj);
      return out;
    }
    case N_CALL: return eval_call_named(vm, n, scope);
    case N_CALLV: {
      AxValue fn = eval_node(vm, n->a, scope);
      AxValue *args = n->nlist ? calloc(n->nlist, sizeof(AxValue)) : NULL;
      for (int i = 0; i < n->nlist; i++) args[i] = eval_node(vm, n->list[i]->b, scope);
      AxValue out = ax_call(vm, fn, args, n->nlist);
      for (int i = 0; i < n->nlist; i++) ax_release(args[i]);
      free(args);
      ax_release(fn);
      return out;
    }
    case N_PIPE: {
      AxValue val = eval_node(vm, n->a, scope);
      AxNode *r = n->b;
      if (r->kind == N_CALL || r->kind == N_METHOD) {
        // The piped value becomes the FIRST argument of the written call.
        int argc = r->nlist + 1;
        AxValue *args = calloc(argc, sizeof(AxValue));
        args[0] = val;
        for (int i = 0; i < r->nlist; i++) args[i + 1] = eval_node(vm, r->list[i]->b, scope);
        AxValue out;
        if (r->kind == N_CALL) {
          AxValue fn;
          if (!ax_scope_lookup(scope, r->str, &fn)) {
            for (int i = 0; i < argc; i++) ax_release(args[i]);
            free(args);
            ax_throw(vm, "AX-RUNTIME-FUNC", "unknown function '%s(...)'", r->str->data);
          }
          out = ax_call(vm, fn, args, argc);
          ax_release(fn);
        } else {
          AxValue obj = eval_node(vm, r->a, scope);
          out = ax_method_call(vm, obj, r->str, args, argc);
          ax_release(obj);
        }
        for (int i = 0; i < argc; i++) ax_release(args[i]);
        free(args);
        return out;
      }
      AxValue fn = eval_node(vm, r, scope);
      AxValue out = ax_call(vm, fn, &val, 1);
      ax_release(fn);
      ax_release(val);
      return out;
    }
    case N_COMPREHENSION: {
      AxValue iter = eval_node(vm, n->b, scope);
      AxArr *seq = ax_to_seq(vm, iter);
      ax_release(iter);
      AxArr *out = ax_arr_new(seq->len);
      AxScope *inner = ax_scope_new(scope, false);
      for (uint32_t i = 0; i < seq->len; i++) {
        if (n->nnames == 1) {
          ax_scope_declare(inner, n->names[0], ax_copy(seq->items[i]));
        } else {
          AxArr *el = ax_to_seq(vm, seq->items[i]);
          for (int v = 0; v < n->nnames; v++) ax_scope_declare(inner, n->names[v], ax_arr_get(el, v));
          ax_release(ax_arrv(el));
        }
        if (n->c) {
          AxValue c = eval_node(vm, n->c, inner);
          bool keep = ax_truthy(c);
          ax_release(c);
          if (!keep) continue;
        }
        ax_arr_push(out, eval_node(vm, n->a, inner));
      }
      ax_scope_release(inner);
      ax_release(ax_arrv(seq));
      return ax_arrv(out);
    }
    default:
      ax_throw(vm, "AX-RUNTIME", "cannot evaluate this expression");
      return ax_null();
  }
}

// A call by name: a local holding a function, then a declared function, then a ^type
// constructor. Innermost binding wins, so a parameter named `map` shadows the library.
// A call resolves to the nearest binding that is actually CALLABLE, skipping over a local of
// the same name that holds data. It is what the reference implementation does, and it is the
// forgiving reading: `^fn step(grid)` shadowing the library's `grid()` should not stop the
// function from calling it.
static bool lookup_callable(AxScope *scope, AxStr *name, AxValue *out, bool *found_any) {
  *found_any = false;
  for (AxScope *p = scope; p; p = p->parent) {
    AxValue v;
    if (!ax_scope_lookup_local(p, name, &v)) continue;
    *found_any = true;
    if (v.t == AX_FN) { *out = v; return true; }
    ax_release(v);
  }
  return false;
}

static AxValue eval_call_named(AxVM *vm, AxNode *n, AxScope *scope) {
  AxValue fn;
  bool found_any = false;
  if (lookup_callable(scope, n->str, &fn, &found_any)) {
    if (fn.t == AX_FN) {
      AxValue *args = n->nlist ? calloc(n->nlist, sizeof(AxValue)) : NULL;
      for (int i = 0; i < n->nlist; i++) args[i] = eval_node(vm, n->list[i]->b, scope);
      AxValue out = ax_call(vm, fn, args, n->nlist);
      for (int i = 0; i < n->nlist; i++) ax_release(args[i]);
      free(args);
      ax_release(fn);
      return out;
    }
    const char *tn = ax_type_name(fn);
    ax_release(fn);
    ax_throw(vm, "AX-CALL-001", "'%s' holds %s, which is not callable", n->str->data, tn);
  }
  // Record constructor.
  AxValue fields;
  if (ax_dict_get(vm->types, n->str, &fields)) {
    AxArr *fnames = (AxArr *)fields.o;
    AxDict *d = ax_dict_new();
    d->type_tag = n->str;
    ax_retain(ax_strv(n->str));
    for (uint32_t i = 0; i < fnames->len; i++) ax_dict_set(d, (AxStr *)fnames->items[i].o, ax_null());
    int pos = 0;
    for (int i = 0; i < n->nlist; i++) {
      AxNode *arg = n->list[i];
      AxValue v = eval_node(vm, arg->b, scope);
      AxStr *field = arg->str;
      if (!field) {
        if (pos < (int)fnames->len) field = (AxStr *)fnames->items[pos++].o;
        else { ax_release(v); continue; }
      }
      ax_dict_set(d, field, v);
    }
    ax_release(fields);
    return ax_dictv(d);
  }
  if (found_any) {
    AxValue v;
    ax_scope_lookup(scope, n->str, &v);
    const char *tn = ax_type_name(v);
    ax_release(v);
    ax_throw(vm, "AX-CALL-001", "'%s' holds %s, which is not callable", n->str->data, tn);
  }
  ax_throw(vm, "AX-RUNTIME-FUNC", "unknown function '%s(...)'", n->str->data);
  return ax_null();
}

AxValue ax_eval(AxVM *vm, AxNode *n, AxScope *scope) { return eval_node(vm, n, scope); }

// ---------------------------------------------------------------------------------------------
// Statements
// ---------------------------------------------------------------------------------------------

static int exec_stmt(AxVM *vm, AxNode *n, AxScope *scope, AxValue *out);

static int exec_list(AxVM *vm, AxNode **stmts, int count, AxScope *scope, AxValue *out) {
  for (int i = 0; i < count; i++) {
    int flow = exec_stmt(vm, stmts[i], scope, out);
    if (flow != AX_FLOW_NORMAL) return flow;
  }
  return AX_FLOW_NORMAL;
}

// Does this statement list create a closure? If not, one scope frame can be reused across loop
// iterations instead of allocated per iteration.
static bool creates_closure(AxNode *n) {
  if (!n) return false;
  if (n->kind == N_LAMBDA) return true;
  if (creates_closure(n->a) || creates_closure(n->b) || creates_closure(n->c)) return true;
  for (int i = 0; i < n->nlist; i++) if (creates_closure(n->list[i])) return true;
  for (int i = 0; i < n->narms; i++) {
    AxArm *arm = &n->arms[i];
    if (creates_closure(arm->guard)) return true;
    for (int j = 0; j < arm->nbody; j++) if (creates_closure(arm->body[j])) return true;
    for (int j = 0; j < arm->npatterns; j++) if (creates_closure(arm->patterns[j])) return true;
  }
  return false;
}

static bool body_creates_closure(AxNode *n) {
  if (n->op == 200) return n->flag;    // cached (op 200 is never a real operator)
  bool found = false;
  for (int i = 0; i < n->nlist && !found; i++) found = creates_closure(n->list[i]);
  n->op = 200;
  n->flag = found;
  return found;
}

static void assign_member_path(AxVM *vm, AxNode *n, AxScope *scope, AxValue value) {
  // names[0].names[1]… = value. Walks dicts; anything else is an error naming the real type.
  AxValue obj = read_var(vm, scope, n->names[0]);
  for (int i = 1; i < n->nnames - 1; i++) {
    if (obj.t != AX_DICT) {
      ax_release(obj);
      ax_release(value);
      ax_throw(vm, "AX-RUNTIME-MEMBER", "cannot assign through '%s' — it is not a dict", n->names[i]->data);
    }
    AxValue next;
    if (!ax_dict_get((AxDict *)obj.o, n->names[i], &next)) next = ax_null();
    ax_release(obj);
    obj = next;
  }
  if (obj.t != AX_DICT) {
    const char *tn = ax_type_name(obj);
    ax_release(obj);
    ax_release(value);
    ax_throw(vm, "AX-RUNTIME-MEMBER", "cannot assign '.%s' on %s", n->names[n->nnames - 1]->data, tn);
  }
  AxStr *last = n->names[n->nnames - 1];
  if (n->op != OP_NONE) {
    AxValue cur;
    if (!ax_dict_get((AxDict *)obj.o, last, &cur)) cur = ax_null();
    if (n->op == OP_COALESCE) {
      if (cur.t != AX_NULL) { ax_release(cur); ax_release(value); ax_release(obj); return; }
      ax_release(cur);
    } else {
      AxValue combined = binary_op(vm, n->op, cur, value);
      ax_release(cur);
      ax_release(value);
      value = combined;
    }
  }
  ax_dict_set((AxDict *)obj.o, last, value);
  ax_release(obj);
}

static int exec_stmt(AxVM *vm, AxNode *n, AxScope *scope, AxValue *out) {
  switch (n->kind) {
    case N_EXPRSTMT: {
      AxValue v = eval_node(vm, n->a, scope);
      ax_release(v);
      return AX_FLOW_NORMAL;
    }
    case N_ASSIGN: {
      AxValue v = eval_node(vm, n->a, scope);
      if (n->op != OP_NONE) {
        AxValue cur = read_var(vm, scope, n->str);
        if (n->op == OP_COALESCE) {
          if (cur.t != AX_NULL) { ax_release(cur); ax_release(v); return AX_FLOW_NORMAL; }
          ax_release(cur);
        } else {
          AxValue combined = binary_op(vm, n->op, cur, v);
          ax_release(cur);
          ax_release(v);
          v = combined;
        }
      }
      write_var(vm, scope, n->str, v, n->flag);
      return AX_FLOW_NORMAL;
    }
    case N_DESTRUCTURE: {
      AxValue v = eval_node(vm, n->a, scope);
      for (int i = 0; i < n->nnames; i++) {
        AxValue part = ax_null();
        if (v.t == AX_ARR) part = ax_arr_get((AxArr *)v.o, i);
        else if (v.t == AX_DICT) {
          AxDict *d = (AxDict *)v.o;
          if (!ax_dict_get(d, n->names[i], &part)) {
            // Fall back to positional order for a dict without that key.
            int seen = 0;
            for (uint32_t e = 0; e < d->len; e++) {
              if (d->entries[e].dead) continue;
              if (seen++ == i) { part = ax_copy(d->entries[e].val); break; }
            }
          }
        }
        write_var(vm, scope, n->names[i], part, false);
      }
      ax_release(v);
      return AX_FLOW_NORMAL;
    }
    case N_MEMBER_ASSIGN: {
      AxValue v = eval_node(vm, n->a, scope);
      assign_member_path(vm, n, scope, v);
      return AX_FLOW_NORMAL;
    }
    case N_INDEX_ASSIGN: {
      AxValue target = read_var(vm, scope, n->str);
      AxValue idx = eval_node(vm, n->a, scope);
      AxValue v = eval_node(vm, n->b, scope);
      if (n->op != OP_NONE) {
        AxValue cur = ax_index_get(vm, target, idx);
        if (n->op == OP_COALESCE) {
          if (cur.t != AX_NULL) { ax_release(cur); ax_release(v); ax_release(idx); ax_release(target); return AX_FLOW_NORMAL; }
          ax_release(cur);
        } else {
          AxValue combined = binary_op(vm, n->op, cur, v);
          ax_release(cur);
          ax_release(v);
          v = combined;
        }
      }
      if (target.t == AX_ARR) {
        double d = ax_to_num(idx);
        int64_t i = (int64_t)d;
        if (i < 0) i += ((AxArr *)target.o)->len;
        ax_arr_set((AxArr *)target.o, i, v);
      } else if (target.t == AX_DICT) {
        AxStr *k = ax_to_str(idx);
        ax_dict_set((AxDict *)target.o, k, v);
        ax_release(ax_strv(k));
      } else {
        const char *tn = ax_type_name(target);
        ax_release(v); ax_release(idx); ax_release(target);
        ax_throw(vm, "AX-RUNTIME-INDEX", "cannot assign into %s by index — '%s' is not an array or dict", tn, n->str->data);
      }
      ax_release(idx);
      ax_release(target);
      return AX_FLOW_NORMAL;
    }
    case N_IF: {
      AxValue c = eval_node(vm, n->a, scope);
      bool t = ax_truthy(c);
      ax_release(c);
      AxScope *inner = ax_scope_new(scope, false);
      int flow = AX_FLOW_NORMAL;
      if (t) flow = exec_list(vm, n->list, n->nlist, inner, out);
      else if (n->c) flow = exec_list(vm, n->c->list, n->c->nlist, inner, out);
      ax_scope_release(inner);
      return flow;
    }
    case N_MATCH: {
      AxValue subject = eval_node(vm, n->a, scope);
      for (int i = 0; i < n->narms; i++) {
        AxArm *arm = &n->arms[i];
        Bindings binds = {0};
        bool hit = false;
        if (!arm->patterns) hit = true;
        else {
          for (int j = 0; j < arm->npatterns; j++) {
            bind_free(&binds);
            if (match_top(vm, subject, arm->patterns[j], scope, &binds, arm->guard != NULL)) { hit = true; break; }
          }
        }
        if (!hit) { bind_free(&binds); continue; }
        AxScope *arm_scope = ax_scope_new(scope, false);
        for (int b = 0; b < binds.n; b++) ax_scope_declare(arm_scope, binds.items[b].name, ax_copy(binds.items[b].val));
        bind_free(&binds);
        if (arm->guard) {
          AxValue g = eval_node(vm, arm->guard, arm_scope);
          bool pass = ax_truthy(g);
          ax_release(g);
          if (!pass) { ax_scope_release(arm_scope); continue; }
        }
        int flow = exec_list(vm, arm->body, arm->nbody, arm_scope, out);
        ax_scope_release(arm_scope);
        ax_release(subject);
        return flow;
      }
      ax_release(subject);
      return AX_FLOW_NORMAL;
    }
    case N_WHILE: {
      bool fresh = body_creates_closure(n);
      AxScope *inner = ax_scope_new(scope, false);
      for (;;) {
        AxValue c = eval_node(vm, n->a, scope);
        bool t = ax_truthy(c);
        ax_release(c);
        if (!t) break;
        if (fresh) { ax_scope_release(inner); inner = ax_scope_new(scope, false); }
        int flow = exec_list(vm, n->list, n->nlist, inner, out);
        if (flow == AX_FLOW_BREAK) break;
        if (flow == AX_FLOW_RETURN) { ax_scope_release(inner); return flow; }
      }
      ax_scope_release(inner);
      return AX_FLOW_NORMAL;
    }
    case N_FOR: {
      AxValue iter = eval_node(vm, n->a, scope);
      bool dict_pairs = (iter.t == AX_DICT && n->nnames > 1);
      AxArr *seq;
      if (dict_pairs) {
        AxDict *d = (AxDict *)iter.o;
        seq = ax_arr_new(d->live);
        for (uint32_t i = 0; i < d->len; i++) {
          if (d->entries[i].dead) continue;
          AxArr *pair = ax_arr_new(2);
          ax_retain(ax_strv(d->entries[i].key));
          ax_arr_push(pair, ax_strv(d->entries[i].key));
          ax_arr_push(pair, ax_copy(d->entries[i].val));
          ax_arr_push(seq, ax_arrv(pair));
        }
      } else if (iter.t == AX_DICT) {
        // Iterating a dict with one variable yields its KEYS, as the reference does.
        AxDict *d = (AxDict *)iter.o;
        seq = ax_arr_new(d->live);
        for (uint32_t i = 0; i < d->len; i++) {
          if (d->entries[i].dead) continue;
          ax_retain(ax_strv(d->entries[i].key));
          ax_arr_push(seq, ax_strv(d->entries[i].key));
        }
      } else {
        seq = ax_to_seq(vm, iter);
      }
      ax_release(iter);
      bool fresh = body_creates_closure(n);
      AxScope *inner = ax_scope_new(scope, false);
      int flow = AX_FLOW_NORMAL;
      for (uint32_t i = 0; i < seq->len; i++) {
        if (fresh) { ax_scope_release(inner); inner = ax_scope_new(scope, false); }
        if (n->nnames == 1) {
          ax_scope_declare(inner, n->names[0], ax_copy(seq->items[i]));
        } else {
          AxArr *el = ax_to_seq(vm, seq->items[i]);
          for (int v = 0; v < n->nnames; v++) ax_scope_declare(inner, n->names[v], ax_arr_get(el, v));
          ax_release(ax_arrv(el));
        }
        int f = exec_list(vm, n->list, n->nlist, inner, out);
        if (f == AX_FLOW_BREAK) break;
        if (f == AX_FLOW_RETURN) { flow = f; break; }
      }
      ax_scope_release(inner);
      ax_release(ax_arrv(seq));
      return flow;
    }
    case N_BREAK: return AX_FLOW_BREAK;
    case N_CONTINUE: return AX_FLOW_CONTINUE;
    case N_RETURN: {
      ax_release(*out);
      *out = n->a ? eval_node(vm, n->a, scope) : ax_null();
      return AX_FLOW_RETURN;
    }
    case N_THROW: {
      AxValue v = n->a ? eval_node(vm, n->a, scope) : ax_null();
      if (v.t == AX_DICT) {
        AxValue code;
        if (ax_dict_get((AxDict *)v.o, S_code, &code)) {
          AxStr *cs = ax_to_str(code);
          snprintf(vm->error_code, sizeof vm->error_code, "%s", cs->data);
          ax_release(ax_strv(cs));
          ax_release(code);
        }
        AxValue msg;
        if (ax_dict_get((AxDict *)v.o, S_msg, &msg)) {
          AxStr *ms = ax_to_str(msg);
          snprintf(vm->error_msg, sizeof vm->error_msg, "%s", ms->data);
          ax_release(ax_strv(ms));
          ax_release(msg);
        }
        ax_throw_value(vm, v);
      }
      AxStr *s = ax_to_str(v);
      snprintf(vm->error_msg, sizeof vm->error_msg, "%s", s->data);
      snprintf(vm->error_code, sizeof vm->error_code, "AX-THROW");
      AxDict *d = ax_dict_new();
      ax_dict_set(d, S_msg, ax_strv(s));
      ax_dict_set(d, S_code, ax_str_from("AX-THROW"));
      ax_dict_set(d, S_value, v);
      ax_throw_value(vm, ax_dictv(d));
      return AX_FLOW_NORMAL;
    }
    case N_TRY: {
      volatile int flow = AX_FLOW_NORMAL;
      if (vm->nhandlers >= AX_MAX_HANDLERS) ax_throw(vm, "AX-TRY", "too many nested ^try blocks");
      int hidx = vm->nhandlers++;
      int saved_depth = vm->call_depth;
      if (setjmp(vm->handlers[hidx]) == 0) {
        AxScope *inner = ax_scope_new(scope, false);
        flow = exec_list(vm, n->list, n->nlist, inner, out);
        ax_scope_release(inner);
        vm->nhandlers--;
      } else {
        // An error unwound to here: the handler is already popped by the throw path's caller.
        vm->nhandlers = hidx;
        vm->call_depth = saved_depth;
        if (n->c) {
          AxScope *cscope = ax_scope_new(scope, false);
          if (n->str) ax_scope_declare(cscope, n->str, ax_copy(vm->error));
          flow = exec_list(vm, n->c->list, n->c->nlist, cscope, out);
          ax_scope_release(cscope);
        } else {
          if (n->b) {
            AxScope *fscope = ax_scope_new(scope, false);
            exec_list(vm, n->b->list, n->b->nlist, fscope, out);
            ax_scope_release(fscope);
          }
          ax_throw_value(vm, ax_copy(vm->error));   // no catch clause: keep unwinding
        }
      }
      if (n->b) {
        AxScope *fscope = ax_scope_new(scope, false);
        AxValue ignored = ax_null();
        int fflow = exec_list(vm, n->b->list, n->b->nlist, fscope, &ignored);
        ax_release(ignored);
        ax_scope_release(fscope);
        if (fflow != AX_FLOW_NORMAL) return fflow;
      }
      return flow;
    }
    case N_ASSERT: {
      AxValue v = eval_node(vm, n->a, scope);
      bool ok = ax_truthy(v);
      ax_release(v);
      if (!ok) ax_throw(vm, "AX-ASSERT", "assertion failed on line %d", n->line);
      return AX_FLOW_NORMAL;
    }
    case N_BLOCK: return exec_list(vm, n->list, n->nlist, scope, out);
    default: {
      AxValue v = eval_node(vm, n, scope);
      ax_release(v);
      return AX_FLOW_NORMAL;
    }
  }
}

int ax_exec(AxVM *vm, AxNode *stmt, AxScope *scope, AxValue *out) { return exec_stmt(vm, stmt, scope, out); }

// ---------------------------------------------------------------------------------------------
// Program
// ---------------------------------------------------------------------------------------------

bool ax_run_program(AxVM *vm, AxNode *program, AxArr *argv, AxValue *result) {
  init_names();
  AxNode *main_decl = NULL;
  // Declarations first, in one pass: functions and types are visible to each other regardless
  // of the order they appear in, which is what lets a program read top-down.
  for (int i = 0; i < program->nlist; i++) {
    AxNode *d = program->list[i];
    if (d->kind == N_FN) {
      AxValue fn = make_closure(vm, d, vm->globals, false);
      ax_scope_declare(vm->globals, d->str, fn);
    } else if (d->kind == N_TYPE) {
      AxArr *fields = ax_arr_new(d->nnames);
      for (int f = 0; f < d->nnames; f++) {
        ax_retain(ax_strv(d->names[f]));
        ax_arr_push(fields, ax_strv(d->names[f]));
      }
      ax_dict_set(vm->types, d->str, ax_arrv(fields));
    } else if (d->kind == N_MAIN) {
      main_decl = d;
    }
  }
  // Globals, in declaration order, so one may build on another.
  for (int i = 0; i < program->nlist; i++) {
    AxNode *d = program->list[i];
    if (d->kind != N_GLOBAL) continue;
    AxValue v = eval_node(vm, d->a, vm->globals);
    for (int k = 0; k < d->nnames; k++) ax_scope_declare(vm->globals, d->names[k], ax_copy(v));
    ax_release(v);
  }
  if (!main_decl) {
    *result = ax_null();
    return false;
  }
  AxScope *s = ax_scope_new(vm->globals, true);
  if (main_decl->nnames > 0) {
    ax_retain(ax_arrv(argv));
    ax_scope_declare(s, main_decl->names[0], ax_arrv(argv));
  }
  ax_retain(ax_arrv(argv));
  ax_scope_declare(s, S_args, ax_arrv(argv));
  AxValue ret = ax_null();
  int flow = exec_list(vm, main_decl->list, main_decl->nlist, s, &ret);
  (void)flow;
  ax_scope_release(s);
  *result = ret;
  return true;
}

AxVM *ax_vm_new(void) {
  init_names();
  AxVM *vm = calloc(1, sizeof(AxVM));
  vm->globals = ax_scope_new(NULL, true);
  vm->types = ax_dict_new();
  vm->fns = ax_dict_new();
  vm->argv = ax_arr_new(0);
  vm->allow_read = ax_arr_new(0);
  vm->allow_write = ax_arr_new(0);
  vm->error = ax_null();
  vm->rng_state = 0x9e3779b9u;
  return vm;
}

void ax_vm_free(AxVM *vm) {
  if (!vm) return;
  ax_release(vm->error);
  ax_release(ax_dictv(vm->types));
  ax_release(ax_dictv(vm->fns));
  ax_release(ax_arrv(vm->argv));
  ax_release(ax_arrv(vm->allow_read));
  ax_release(ax_arrv(vm->allow_write));
  ax_scope_release(vm->globals);
  free(vm);
}
