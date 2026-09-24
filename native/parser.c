// parser.c — recursive-descent parser producing an arena-allocated AST.
//
// The grammar is the one in GRAMMAR.md: the language (`^fn`, `^proc`, `^main`, `^type`, `^use`,
// `~globals`, statements, expressions) and the engine (`@entities` with their `~fields` and
// `&blocks`, `^event`, `^mix`, `^mat`, `#resources`, broadcasts, transitions, queries and
// cross-entity writes). Each engine form follows parser.js rule for rule, including its
// one-line shorthands, because a program has to mean the same thing on both runtimes.
//
// The AST is immutable after parsing and lives in a bump arena freed in one call, so nodes
// carry no ownership bookkeeping. Identifier and key strings are interned, which is what makes
// scope lookup a pointer comparison at run time.
//
// Two generic node shapes are reused throughout and worth naming here:
//   * a PAIR node (kind N_BLOCK) carries `str` (a name, may be NULL), `a` (a key expression,
//     may be NULL) and `b` (a value) — used for call arguments and dictionary entries;
//   * a BODY node (kind N_BLOCK) carries a statement list in `list`/`nlist` — used for the
//     else-branch, catch and finally clauses.

#include "axiom.h"
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>

// ---------------------------------------------------------------------------------------------
// Arena
// ---------------------------------------------------------------------------------------------

typedef struct ArenaBlock {
  struct ArenaBlock *next;
  size_t used, cap;
  char data[];
} ArenaBlock;

static ArenaBlock *g_arena = NULL;

static void *arena_alloc(size_t n) {
  n = (n + 15) & ~(size_t)15;
  if (!g_arena || g_arena->used + n > g_arena->cap) {
    size_t cap = n > (size_t)64 * 1024 ? n : (size_t)64 * 1024;
    ArenaBlock *b = calloc(1, sizeof(ArenaBlock) + cap);
    if (!b) { fprintf(stderr, "axiom: out of memory\n"); exit(70); }
    b->cap = cap;
    b->next = g_arena;
    g_arena = b;
  }
  void *p = g_arena->data + g_arena->used;
  g_arena->used += n;
  return p;
}

void ax_ast_free_all(void) {
  while (g_arena) { ArenaBlock *n = g_arena->next; free(g_arena); g_arena = n; }
}

// ---------------------------------------------------------------------------------------------
// Parser state
// ---------------------------------------------------------------------------------------------

typedef struct {
  AxTokens *toks;
  int pos;
  AxParseResult *res;
  jmp_buf bail;
} P;

static AxNode *node(P *p, AxNodeKind kind) {
  AxNode *n = arena_alloc(sizeof(AxNode));
  n->kind = (uint8_t)kind;
  AxTok *t = &p->toks->toks[p->pos < p->toks->count ? p->pos : p->toks->count - 1];
  n->line = t->line;
  n->col = t->col;
  return n;
}

static AxTok *peek(P *p, int off) {
  int i = p->pos + off;
  if (i >= p->toks->count) i = p->toks->count - 1;
  return &p->toks->toks[i];
}

static bool at(P *p, AxTokType t) { return peek(p, 0)->type == t; }
static AxTok *advance(P *p) { return &p->toks->toks[p->pos++]; }

static void perr(P *p, const char *fmt, ...) {
  if (!p->res->err[0]) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(p->res->err, sizeof p->res->err, fmt, ap);
    va_end(ap);
    p->res->err_line = peek(p, 0)->line;
  }
  p->res->nerrors++;
  longjmp(p->bail, 1);
}

static const char *tok_name(AxTokType t) {
  switch (t) {
    case T_EOF: return "end of file";
    case T_NEWLINE: return "end of line";
    case T_INDENT: return "indent";
    case T_DEDENT: return "dedent";
    case T_IDENT: return "a name";
    case T_NUM: return "a number";
    case T_STR: return "a string";
    case T_COLON: return "':'";
    case T_LPAREN: return "'('";
    case T_RPAREN: return "')'";
    case T_RBRACKET: return "']'";
    case T_RBRACE: return "'}'";
    case T_COMMA: return "','";
    default: return "a different token";
  }
}

static AxTok *expect(P *p, AxTokType t, const char *what) {
  if (!at(p, t)) perr(p, "expected %s%s%s, got %s", tok_name(t), what ? " " : "", what ? what : "", tok_name(peek(p, 0)->type));
  return advance(p);
}

static bool at_kw(P *p, const char *kw) {
  AxTok *t = peek(p, 0);
  return t->type == T_IDENT && strcmp(t->payload, kw) == 0;
}

static bool at_kw_off(P *p, int off, const char *kw) {
  AxTok *t = peek(p, off);
  return t->type == T_IDENT && strcmp(t->payload, kw) == 0;
}

static AxStr *tok_name_str(AxTok *t) { return ax_intern(t->payload, t->len); }

static void skip_newlines(P *p) { while (at(p, T_NEWLINE)) advance(p); }

// Growable child lists, arena-backed.
typedef struct { AxNode **items; int n, cap; } NodeList;
static void nl_push(NodeList *l, AxNode *n) {
  if (l->n == l->cap) {
    int ncap = l->cap ? l->cap * 2 : 8;
    AxNode **ni = arena_alloc(sizeof(AxNode *) * ncap);
    if (l->items) memcpy(ni, l->items, sizeof(AxNode *) * l->n);
    l->items = ni;
    l->cap = ncap;
  }
  l->items[l->n++] = n;
}

typedef struct { AxStr **items; int n, cap; } NameList;
static void name_push(NameList *l, AxStr *s) {
  if (l->n == l->cap) {
    int ncap = l->cap ? l->cap * 2 : 8;
    AxStr **ni = arena_alloc(sizeof(AxStr *) * ncap);
    if (l->items) memcpy(ni, l->items, sizeof(AxStr *) * l->n);
    l->items = ni;
    l->cap = ncap;
  }
  l->items[l->n++] = s;
}

// ---------------------------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------------------------

static AxNode *parse_expr(P *p);
static AxNode *parse_ternary(P *p);
static AxNode *parse_tagref(P *p);
static bool is_known_query(AxStr *name);
static AxNode *parse_stmt(P *p);
static void parse_body(P *p, NodeList *out);

// ---------------------------------------------------------------------------------------------
// Expressions
// ---------------------------------------------------------------------------------------------

// A call argument or a dict entry: `str` names it (or NULL), `a` is a computed key (or NULL),
// `b` is the value.
static AxNode *pair_node(P *p, AxStr *name, AxNode *keyExpr, AxNode *value) {
  AxNode *n = node(p, N_BLOCK);
  n->str = name;
  n->a = keyExpr;
  n->b = value;
  return n;
}

static void parse_args(P *p, NodeList *out) {
  expect(p, T_LPAREN, NULL);
  if (!at(p, T_RPAREN)) {
    for (;;) {
      AxStr *name = NULL;
      // A named argument is `ident: value`, but only when the ident is not the whole expression
      // (`f(a)` passes the value of `a`; `f(a: 1)` names it).
      if (at(p, T_IDENT) && peek(p, 1)->type == T_COLON) {
        name = tok_name_str(advance(p));
        advance(p);
      }
      // A bare predicate argument — `belief.any(>0.3)` — carries its operator in `op`.
      int pred = OP_NONE;
      if (!name) {
        switch (peek(p, 0)->type) {
          case T_GT: pred = OP_GT; break;
          case T_LT: pred = OP_LT; break;
          case T_GE: pred = OP_GE; break;
          case T_LE: pred = OP_LE; break;
          case T_EQEQ: pred = OP_EQ; break;
          case T_NE: pred = OP_NE; break;
          default: break;
        }
        if (pred) advance(p);
      }
      AxNode *v = parse_expr(p);
      AxNode *pr = pair_node(p, name, NULL, v);
      pr->op = (uint8_t)pred;
      nl_push(out, pr);
      if (at(p, T_COMMA)) { advance(p); if (at(p, T_RPAREN)) break; continue; }
      break;
    }
  }
  expect(p, T_RPAREN, NULL);
}

// `{expr:spec}`: the spec follows the last top-level ':' when it parses as one, nothing but
// the expression precedes it, and the colon cannot belong to an open ternary or a lambda.
// Returns the offset of that colon within [s, s+n), or -1. (Mirrors parser.js splitFormatSpec.)
static int format_colon(const char *s, int n, char *spec, size_t specn) {
  int depth = 0, last = -1;
  char quote = 0;
  for (int i = 0; i < n; i++) {
    char c = s[i];
    if (quote) { if (c == '\\') i++; else if (c == quote) quote = 0; continue; }
    if (c == '"' || c == '\'') { quote = c; continue; }
    if (c == '(' || c == '[' || c == '{') depth++;
    else if (c == ')' || c == ']' || c == '}') depth--;
    else if (c == ':' && depth == 0) last = i;
  }
  if (last <= 0) return -1;
  int sl = n - last - 1;
  if (sl < 0 || (size_t)sl >= specn) return -1;
  memcpy(spec, s + last + 1, sl);
  spec[sl] = '\0';
  if (sl > 0 && !ax_is_format_spec(spec)) return -1;          // `{x:}` is an empty spec: plain `{x}`
  char before = s[last - 1];
  if (before == ' ' || before == '\t' || before == '\n' || before == '\r') return -1;
  int q = 0, colons = 0;
  bool nonblank = false;
  depth = 0; quote = 0;
  for (int i = 0; i < last; i++) {
    char c = s[i];
    if (c != ' ' && c != '\t') nonblank = true;
    if (quote) { if (c == '\\') i++; else if (c == quote) quote = 0; continue; }
    if (c == '"' || c == '\'') { quote = c; continue; }
    if (c == '(' || c == '[' || c == '{') depth++;
    else if (c == ')' || c == ']' || c == '}') depth--;
    else if (depth == 0 && c == '\\') return -1;
    else if (depth == 0 && c == ':') colons++;
    else if (depth == 0 && c == '?') {
      char nx = i + 1 < last ? s[i + 1] : 0, pv = i > 0 ? s[i - 1] : 0;
      if (nx != '?' && pv != '?' && nx != '>' && nx != '!' && nx != '.') q++;
    }
  }
  if (q > colons || !nonblank) return -1;
  return last;
}

// f-string payload → a list of parts. Literal parts carry flag=true.
static AxNode *parse_fstring(P *p, AxTok *tok) {
  AxNode *n = node(p, N_FSTR);
  NodeList parts = {0};
  const char *s = tok->payload;
  int len = (int)tok->len;
  char *buf = arena_alloc(len + 1);
  int bl = 0;
  for (int i = 0; i < len; ) {
    if (s[i] == '{' && i + 1 < len && s[i + 1] == '{') { buf[bl++] = '{'; i += 2; continue; }
    if (s[i] == '}' && i + 1 < len && s[i + 1] == '}') { buf[bl++] = '}'; i += 2; continue; }
    if (s[i] == '\\' && i + 1 < len) {
      // Escapes in the literal parts of an f-string are decoded here: the lexer keeps the
      // payload verbatim so `\{` can be told from a placeholder, which leaves the rest to us.
      char e = s[i + 1];
      char out;
      switch (e) {
        case 'n': out = '\n'; break;
        case 't': out = '\t'; break;
        case 'r': out = '\r'; break;
        case '0': out = '\0'; break;
        case '\\': out = '\\'; break;
        case '"': out = '"'; break;
        case '\'': out = '\''; break;
        case '{': out = '{'; break;
        case '}': out = '}'; break;
        default: buf[bl++] = '\\'; buf[bl++] = e; i += 2; continue;
      }
      buf[bl++] = out;
      i += 2;
      continue;
    }
    if (s[i] == '{') {
      if (bl) {
        AxNode *lit = node(p, N_STR);
        lit->str = ax_str_new(buf, bl);
        lit->flag = true;
        nl_push(&parts, lit);
        bl = 0;
      }
      int depth = 1, j = i + 1;
      bool in_str = false;
      char q = 0;
      while (j < len && depth > 0) {
        char c = s[j];
        if (in_str) { if (c == '\\') j++; else if (c == q) in_str = false; j++; continue; }
        if (c == '"' || c == '\'') { in_str = true; q = c; j++; continue; }
        if (c == '{') depth++;
        else if (c == '}') depth--;
        if (depth == 0) break;
        j++;
      }
      if (depth != 0) perr(p, "unterminated { } in an f-string");
      // Parse the placeholder by tokenizing it on its own.
      int flen = j - (i + 1);
      char spec[128];
      int colon = format_colon(s + i + 1, flen, spec, sizeof spec);
      if (colon >= 0) flen = colon;
      // Trimmed, as parser.js trims: `{ x }` is `{x}`, not an indented line.
      const char *fs = s + i + 1;
      while (flen > 0 && (*fs == ' ' || *fs == '\t' || *fs == '\n' || *fs == '\r')) { fs++; flen--; }
      while (flen > 0 && (fs[flen - 1] == ' ' || fs[flen - 1] == '\t' || fs[flen - 1] == '\n' || fs[flen - 1] == '\r')) flen--;
      char *frag = arena_alloc(flen + 2);
      memcpy(frag, fs, flen);
      frag[flen] = '\n';
      frag[flen + 1] = '\0';
      AxTokens *sub = arena_alloc(sizeof(AxTokens));
      if (!ax_tokenize(frag, sub)) perr(p, "f-string placeholder: %s", sub->err);
      P sp = { .toks = sub, .pos = 0, .res = p->res };
      if (setjmp(sp.bail) == 0) {
        AxNode *e = parse_expr(&sp);
        while (sp.pos < sub->count && (sub->toks[sp.pos].type == T_NEWLINE || sub->toks[sp.pos].type == T_INDENT || sub->toks[sp.pos].type == T_DEDENT)) sp.pos++;
        if (sp.pos < sub->count && sub->toks[sp.pos].type != T_EOF) {
          sub->src[strcspn(sub->src, "\n")] = '\0';
          perr(p, "f-string placeholder '{%s}' has trailing tokens (only a single expression is allowed)", sub->src);
        }
        e->flag = false;
        if (colon >= 0 && spec[0]) {
          AxNode *f = node(p, N_FMT);
          f->a = e;
          f->str = ax_str_new(spec, strlen(spec));
          e = f;
        }
        nl_push(&parts, e);
      } else {
        perr(p, "invalid expression inside an f-string");
      }
      i = j + 1;
      continue;
    }
    buf[bl++] = s[i++];
  }
  if (bl) {
    AxNode *lit = node(p, N_STR);
    lit->str = ax_str_new(buf, bl);
    lit->flag = true;
    nl_push(&parts, lit);
  }
  n->list = parts.items;
  n->nlist = parts.n;
  return n;
}

// `#Tag.a.b` — a reference to an entity (or resource), with an optional member path.
static AxNode *parse_tagref(P *p) {
  expect(p, T_HASH, NULL);
  AxNode *n = node(p, N_TAGREF);
  NameList path = {0};
  name_push(&path, tok_name_str(expect(p, T_IDENT, "after '#'")));
  while (at(p, T_DOT) && peek(p, 1)->type == T_IDENT) { advance(p); name_push(&path, tok_name_str(advance(p))); }
  n->names = path.items;
  n->nnames = path.n;
  return n;
}

// Names that parse as queries even without the leading `?` (parser.js KNOWN_QUERY_NAMES), so
// `nearest(#Enemy)` and `?nearest(#Enemy)` are the same thing in every position.
static bool is_known_query(AxStr *name) {
  static const char *known[] = { "nearest", "exists", "path", "raycast", "block_cell", "unblock_cell", "is_blocked" };
  for (size_t i = 0; i < sizeof known / sizeof *known; i++) if (strcmp(name->data, known[i]) == 0) return true;
  return false;
}

static AxNode *parse_primary(P *p) {
  AxTok *t = peek(p, 0);
  switch (t->type) {
    case T_NUM: {
      advance(p);
      AxNode *n = node(p, N_NUM);
      n->num = t->num;
      // `2v` (uniform vector) and other engine units are not part of the native language;
      // `3f` is just a float, and a duration suffix is carried but unused here.
      if (t->unit[0]) n->str = ax_internz(t->unit);
      return n;
    }
    case T_STR: {
      advance(p);
      AxNode *n = node(p, N_STR);
      n->str = ax_str_new(t->payload, t->len);
      return n;
    }
    case T_FSTR: advance(p); return parse_fstring(p, t);
    case T_BACKSLASH: {
      // Lambda: \x: expr  /  \a, b => expr  /  \: expr
      advance(p);
      AxNode *n = node(p, N_LAMBDA);
      NameList names = {0};
      if (!at(p, T_COLON) && !at(p, T_FATARROW)) {
        name_push(&names, tok_name_str(expect(p, T_IDENT, "as a lambda parameter")));
        while (at(p, T_COMMA)) { advance(p); name_push(&names, tok_name_str(expect(p, T_IDENT, "as a lambda parameter"))); }
      }
      if (at(p, T_FATARROW)) advance(p);
      else expect(p, T_COLON, "before the lambda body");
      n->names = names.items;
      n->nnames = names.n;
      n->a = parse_ternary(p);   // not parse_expr: `\x: x |> f` pipes the lambda's RESULT
      n->flag = true;
      return n;
    }
    case T_LBRACKET: {
      advance(p);
      AxNode *n = node(p, N_ARRAY);
      NodeList items = {0};
      if (at(p, T_RBRACKET)) { advance(p); n->list = items.items; n->nlist = 0; return n; }
      AxNode *first = parse_expr(p);
      if (at_kw(p, "for")) {
        // Comprehension: [expr for v (, v2) in iterable (if cond)]
        advance(p);
        AxNode *c = node(p, N_COMPREHENSION);
        c->a = first;
        NameList vars = {0};
        name_push(&vars, tok_name_str(expect(p, T_IDENT, "as a comprehension variable")));
        while (at(p, T_COMMA)) { advance(p); name_push(&vars, tok_name_str(expect(p, T_IDENT, "as a comprehension variable"))); }
        if (!at_kw(p, "in")) perr(p, "expected 'in' after the comprehension variable");
        advance(p);
        c->b = parse_expr(p);
        if (at_kw(p, "if")) { advance(p); c->c = parse_expr(p); }
        expect(p, T_RBRACKET, "to close the comprehension");
        c->names = vars.items;
        c->nnames = vars.n;
        return c;
      }
      nl_push(&items, first);
      while (at(p, T_COMMA)) {
        advance(p);
        if (at(p, T_RBRACKET)) break;   // trailing comma
        nl_push(&items, parse_expr(p));
      }
      expect(p, T_RBRACKET, "to close the array");
      n->list = items.items;
      n->nlist = items.n;
      return n;
    }
    case T_LBRACE: {
      advance(p);
      AxNode *n = node(p, N_DICT);
      NodeList entries = {0};
      if (!at(p, T_RBRACE)) {
        for (;;) {
          if (at(p, T_STR)) {
            AxTok *k = advance(p);
            expect(p, T_COLON, "after a dictionary key");
            nl_push(&entries, pair_node(p, ax_intern(k->payload, k->len), NULL, parse_expr(p)));
          } else if (at(p, T_LBRACKET)) {
            advance(p);
            AxNode *ke = parse_expr(p);
            expect(p, T_RBRACKET, "after a computed key");
            expect(p, T_COLON, "after a dictionary key");
            nl_push(&entries, pair_node(p, NULL, ke, parse_expr(p)));
          } else {
            AxTok *k = expect(p, T_IDENT, "as a dictionary key");
            AxStr *key = tok_name_str(k);
            if (at(p, T_COLON)) {
              advance(p);
              nl_push(&entries, pair_node(p, key, NULL, parse_expr(p)));
            } else {
              // Shorthand {x} → {x: x}
              AxNode *id = node(p, N_IDENT);
              id->str = key;
              nl_push(&entries, pair_node(p, key, NULL, id));
            }
          }
          if (at(p, T_COMMA)) { advance(p); if (at(p, T_RBRACE)) break; continue; }
          break;
        }
      }
      expect(p, T_RBRACE, "to close the dictionary");
      n->list = entries.items;
      n->nlist = entries.n;
      return n;
    }
    case T_LPAREN: {
      advance(p);
      AxNode *e = parse_expr(p);
      expect(p, T_RPAREN, "to close the group");
      return e;
    }
    case T_QUESTION: {
      advance(p);
      // `?!#Tag` — shorthand for `?exists(#Tag)`.
      if (at(p, T_BANG) && peek(p, 1)->type == T_HASH) {
        advance(p);
        AxNode *q = node(p, N_QUERY);
        q->str = ax_internz("exists");
        NodeList args = {0};
        nl_push(&args, pair_node(p, NULL, NULL, parse_tagref(p)));
        q->list = args.items;
        q->nlist = args.n;
        return q;
      }
      // `?name(args)` — a query on the running entity (or a global query).
      if (at(p, T_IDENT) && peek(p, 1)->type == T_LPAREN) {
        AxNode *q = node(p, N_QUERY);
        q->str = tok_name_str(advance(p));
        NodeList args = {0};
        parse_args(p, &args);
        q->list = args.items;
        q->nlist = args.n;
        return q;
      }
      // Prefix ternary: ? cond : then : else
      AxNode *n = node(p, N_TERNARY);
      n->a = parse_expr(p);
      expect(p, T_COLON, "in the prefix ternary");
      n->b = parse_expr(p);
      expect(p, T_COLON, "in the prefix ternary");
      n->c = parse_expr(p);
      return n;
    }
    case T_IDENT: {
      advance(p);
      AxNode *n = node(p, N_IDENT);
      n->str = tok_name_str(t);
      return n;
    }
    case T_DOLLAR: {
      advance(p);
      AxTok *id = expect(p, T_IDENT, "after '$'");
      AxNode *n = node(p, N_IDENT);
      n->str = tok_name_str(id);
      return n;
    }
    case T_HASH: return parse_tagref(p);
    default:
      perr(p, "unexpected %s in an expression", tok_name(t->type));
      return NULL;
  }
}

static AxNode *parse_postfix(P *p) {
  AxNode *e = parse_primary(p);
  for (;;) {
    if (at(p, T_DOT)) {
      advance(p);
      AxTok *prop = expect(p, T_IDENT, "after '.'");
      if (at(p, T_LPAREN)) {
        AxNode *n = node(p, N_METHOD);
        n->a = e;
        n->str = tok_name_str(prop);
        NodeList args = {0};
        parse_args(p, &args);
        n->list = args.items;
        n->nlist = args.n;
        e = n;
      } else {
        AxNode *n = node(p, N_MEMBER);
        n->a = e;
        n->str = tok_name_str(prop);
        e = n;
      }
      continue;
    }
    if (at(p, T_LBRACKET)) {
      advance(p);
      AxNode *n = node(p, N_INDEX);
      n->a = e;
      n->b = parse_expr(p);
      expect(p, T_RBRACKET, "to close the index");
      e = n;
      continue;
    }
    if (at(p, T_LPAREN)) {
      NodeList args = {0};
      parse_args(p, &args);
      AxNode *n;
      if (e->kind == N_IDENT && is_known_query(e->str)) {
        n = node(p, N_QUERY);
        n->str = e->str;
      } else if (e->kind == N_IDENT) {
        n = node(p, N_CALL);
        n->str = e->str;
      } else {
        n = node(p, N_CALLV);
        n->a = e;
      }
      n->list = args.items;
      n->nlist = args.n;
      e = n;
      continue;
    }
    break;
  }
  return e;
}

static AxNode *unary_node(P *p, int op, AxNode *a) {
  AxNode *n = node(p, N_UNARY);
  n->op = (uint8_t)op;
  n->a = a;
  return n;
}

static AxNode *parse_unary(P *p) {
  if (at(p, T_BANG)) { advance(p); return unary_node(p, OP_NOT, parse_unary(p)); }
  if (at(p, T_MINUS)) { advance(p); return unary_node(p, OP_NEG, parse_unary(p)); }
  return parse_postfix(p);
}

static AxNode *bin_node(P *p, int op, AxNode *a, AxNode *b) {
  AxNode *n = node(p, N_BINARY);
  n->op = (uint8_t)op;
  n->a = a;
  n->b = b;
  return n;
}

static AxNode *parse_exponent(P *p) {
  AxNode *left = parse_unary(p);
  if (at(p, T_STARSTAR)) { advance(p); return bin_node(p, OP_POW, left, parse_exponent(p)); }  // right-assoc
  return left;
}

static AxNode *parse_multiplicative(P *p) {
  AxNode *left = parse_exponent(p);
  for (;;) {
    int op = 0;
    if (at(p, T_STAR)) op = OP_MUL;
    else if (at(p, T_SLASH)) op = OP_DIV;
    else if (at(p, T_PERCENT)) op = OP_MOD;
    else if (at(p, T_MIDDOT)) op = OP_DOT;
    else if (at(p, T_CROSS)) op = OP_CROSS;
    else break;
    advance(p);
    left = bin_node(p, op, left, parse_exponent(p));
  }
  return left;
}

static AxNode *parse_additive(P *p) {
  AxNode *left = parse_multiplicative(p);
  for (;;) {
    int op = 0;
    if (at(p, T_PLUS)) op = OP_ADD;
    else if (at(p, T_MINUS)) op = OP_SUB;
    else if (at(p, T_DOTDOT)) op = OP_RANGE;
    else break;
    advance(p);
    left = bin_node(p, op, left, parse_multiplicative(p));
  }
  return left;
}

static AxNode *parse_comparison(P *p) {
  AxNode *left = parse_additive(p);
  for (;;) {
    int op = 0;
    if (at(p, T_GT)) op = OP_GT;
    else if (at(p, T_LT)) op = OP_LT;
    else if (at(p, T_GE)) op = OP_GE;
    else if (at(p, T_LE)) op = OP_LE;
    else if (at(p, T_EQEQ)) op = OP_EQ;
    else if (at(p, T_NE)) op = OP_NE;
    else if (at(p, T_QMARKGT)) op = OP_RAY;
    else if (at_kw(p, "in")) op = OP_IN;
    else if (at(p, T_BANG) && at_kw_off(p, 1, "in")) {
      advance(p); advance(p);
      left = unary_node(p, OP_NOT, bin_node(p, OP_IN, left, parse_additive(p)));
      continue;
    }
    else break;
    advance(p);
    left = bin_node(p, op, left, parse_additive(p));
  }
  return left;
}

// `dist ~> argmax` — draw a concrete value from a distribution.
static AxNode *parse_infer(P *p) {
  AxNode *left = parse_comparison(p);
  if (at(p, T_TILDEGT)) {
    advance(p);
    AxNode *n = node(p, N_INFER);
    n->a = left;
    n->str = tok_name_str(expect(p, T_IDENT, "after '~>'"));
    left = n;
  }
  return left;
}

static AxNode *parse_and(P *p) {
  AxNode *left = parse_infer(p);
  while (at(p, T_ANDAND)) { advance(p); left = bin_node(p, OP_AND, left, parse_infer(p)); }
  return left;
}

static AxNode *parse_or(P *p) {
  AxNode *left = parse_and(p);
  while (at(p, T_OROR)) { advance(p); left = bin_node(p, OP_OR, left, parse_and(p)); }
  return left;
}

static AxNode *parse_nullcoal(P *p) {
  AxNode *left = parse_or(p);
  while (at(p, T_NULLCOAL)) { advance(p); left = bin_node(p, OP_COALESCE, left, parse_or(p)); }
  return left;
}

static AxNode *parse_ternary(P *p) {
  AxNode *cond = parse_nullcoal(p);
  if (!at(p, T_QUESTION)) return cond;
  advance(p);
  AxNode *n = node(p, N_TERNARY);
  n->a = cond;
  n->b = parse_ternary(p);            // right-associative, so `a ? 1 : b ? 2 : 3` chains
  // `obj ?name(args)` with no ':' is a query with an explicit receiver.
  if (!at(p, T_COLON) && (n->b->kind == N_CALL || (n->b->kind == N_QUERY && !n->b->a))) {
    AxNode *q = n->b;
    q->kind = N_QUERY;
    q->a = cond;
    return q;
  }
  expect(p, T_COLON, "in the ternary");
  n->c = parse_ternary(p);
  return n;
}

static AxNode *parse_expr(P *p) {
  AxNode *left = parse_ternary(p);
  while (at(p, T_PIPEGT)) {
    advance(p);
    AxNode *n = node(p, N_PIPE);
    n->a = left;
    n->b = parse_ternary(p);
    left = n;
  }
  return left;
}

// ---------------------------------------------------------------------------------------------
// Statements
// ---------------------------------------------------------------------------------------------

// A body is either one inline statement or an indented block.
static void parse_body(P *p, NodeList *out) {
  if (!at(p, T_NEWLINE)) { nl_push(out, parse_stmt(p)); return; }
  advance(p);
  expect(p, T_INDENT, "to start a block");
  while (!at(p, T_DEDENT) && !at(p, T_EOF)) {
    nl_push(out, parse_stmt(p));
    skip_newlines(p);
  }
  expect(p, T_DEDENT, "to end a block");
}

static AxNode *body_node(P *p, NodeList *l) {
  AxNode *n = node(p, N_BLOCK);
  n->list = l->items;
  n->nlist = l->n;
  return n;
}

static void end_stmt(P *p) {
  if (at(p, T_SEMI)) { advance(p); return; }
  if (at(p, T_NEWLINE)) { advance(p); return; }
  if (at(p, T_DEDENT) || at(p, T_EOF)) return;
  perr(p, "expected the end of the statement, got %s", tok_name(peek(p, 0)->type));
}

static int compound_op_of(AxTokType t) {
  switch (t) {
    case T_PLUSEQ: return OP_ADD;
    case T_MINUSEQ: return OP_SUB;
    case T_STAREQ: return OP_MUL;
    case T_SLASHEQ: return OP_DIV;
    case T_PERCEQ: return OP_MOD;
    case T_NULLCOALEQ: return OP_COALESCE;
    default: return OP_NONE;
  }
}

static bool at_assign_op(P *p) {
  AxTokType t = peek(p, 0)->type;
  return t == T_ASSIGN || compound_op_of(t) != OP_NONE;
}

static AxNode *exists_query(P *p) {
  AxNode *q = node(p, N_QUERY);
  q->str = ax_internz("exists");
  NodeList args = {0};
  nl_push(&args, pair_node(p, NULL, NULL, parse_tagref(p)));
  q->list = args.items;
  q->nlist = args.n;
  return q;
}

static bool starts_stmt_only(P *p) {
  AxTokType t = peek(p, 0)->type;
  if (t == T_BANG || t == T_CARET || t == T_PLUSPLUS || t == T_MINUSMINUS || t == T_TILDE || t == T_HASH || t == T_DOLLAR) return true;
  return false;
}

static void parse_indented_into(P *p, NodeList *out) {
  expect(p, T_INDENT, "to start a block");
  while (!at(p, T_DEDENT) && !at(p, T_EOF)) {
    nl_push(out, parse_stmt(p));
    skip_newlines(p);
  }
  expect(p, T_DEDENT, "to end a block");
}

static AxNode *parse_cond(P *p) {
  // ?cond: body  [ ?!: else | elif … | else: … ]
  AxNode *n = node(p, N_IF);
  bool sigil = at(p, T_QUESTION);
  advance(p);                            // `?`, `if` or `elif`
  if (sigil && at(p, T_BANG) && peek(p, 1)->type == T_HASH) {
    advance(p);                          // `?!#Tag:` is the exists-shorthand
    n->a = exists_query(p);
  } else {
    n->a = parse_expr(p);
  }
  expect(p, T_COLON, "after the condition");
  NodeList body = {0};
  if (at(p, T_NEWLINE)) {
    parse_body(p, &body);
  } else if (starts_stmt_only(p)) {
    // `?c: !act()` followed by an indented block: the block continues the body.
    nl_push(&body, parse_stmt(p));
    if (at(p, T_INDENT)) parse_indented_into(p, &body);
  } else {
    // `?c: expr` alone on the line is an expression statement; `?c: guard` followed by an
    // indented block ANDs the guard with the condition (parser.js); anything else is a
    // statement, so back up and parse it as one.
    int start = p->pos;
    jmp_buf saved;
    memcpy(saved, p->bail, sizeof saved);
    AxNode *e = NULL;
    if (setjmp(p->bail) == 0) e = parse_expr(p);
    else { p->res->err[0] = '\0'; p->res->nerrors--; e = NULL; }
    memcpy(p->bail, saved, sizeof saved);
    if (e && at(p, T_NEWLINE)) {
      advance(p);
      if (at(p, T_INDENT)) {
        n->b = e;
        parse_indented_into(p, &body);
      } else {
        AxNode *es = node(p, N_EXPRSTMT);
        es->a = e;
        nl_push(&body, es);
      }
    } else {
      p->pos = start;
      nl_push(&body, parse_stmt(p));
    }
  }
  n->list = body.items;
  n->nlist = body.n;
  // Else / elif. A DEDENT may separate the block from its else clause.
  int save = p->pos;
  skip_newlines(p);
  if (at(p, T_QMARKEQ) || at_kw(p, "else")) {
    advance(p);
    expect(p, T_COLON, "after else");
    NodeList eb = {0};
    parse_body(p, &eb);
    n->c = body_node(p, &eb);
    return n;
  }
  if (at_kw(p, "elif")) {
    NodeList eb = {0};
    nl_push(&eb, parse_cond(p));
    n->c = body_node(p, &eb);
    return n;
  }
  p->pos = save;
  return n;
}

static AxNode *parse_match(P *p) {
  AxNode *n = node(p, N_MATCH);
  expect(p, T_QUESTION, NULL);
  expect(p, T_STAR, NULL);
  n->a = parse_expr(p);
  expect(p, T_COLON, "after the match subject");
  expect(p, T_NEWLINE, "after the match subject");
  expect(p, T_INDENT, "to start the match arms");
  AxArm *arms = NULL;
  int narms = 0, cap = 0;
  bool saw_default = false;
  while (!at(p, T_DEDENT) && !at(p, T_EOF)) {
    if (narms == cap) {
      cap = cap ? cap * 2 : 8;
      AxArm *na = arena_alloc(sizeof(AxArm) * cap);
      if (arms) memcpy(na, arms, sizeof(AxArm) * narms);
      arms = na;
    }
    AxArm *arm = &arms[narms++];
    memset(arm, 0, sizeof *arm);
    bool is_default = at(p, T_IDENT) && strcmp(peek(p, 0)->payload, "_") == 0
                      && (peek(p, 1)->type == T_COLON || (peek(p, 1)->type == T_IDENT && strcmp(peek(p, 1)->payload, "if") == 0));
    if (is_default) {
      advance(p);
    } else {
      if (saw_default) perr(p, "the `_` default arm must be last in a ?* match");
      NodeList pats = {0};
      nl_push(&pats, parse_expr(p));
      while (at(p, T_COMMA)) { advance(p); nl_push(&pats, parse_expr(p)); }
      arm->patterns = pats.items;
      arm->npatterns = pats.n;
    }
    if (at_kw(p, "if")) { advance(p); arm->guard = parse_expr(p); }
    if (is_default && !arm->guard) saw_default = true;
    expect(p, T_COLON, "after the match pattern");
    NodeList body = {0};
    parse_body(p, &body);
    arm->body = body.items;
    arm->nbody = body.n;
    skip_newlines(p);
  }
  expect(p, T_DEDENT, "to end the match");
  n->arms = arms;
  n->narms = narms;
  return n;
}

static AxNode *parse_loop(P *p) {
  expect(p, T_STAR, NULL);
  // `*v in iterable:` / `*a, b in iterable:` / `*cond:`
  bool is_for = false;
  if (at(p, T_IDENT)) {
    if (at_kw_off(p, 1, "in")) is_for = true;
    else if (peek(p, 1)->type == T_COMMA) {
      int i = 1;
      while (peek(p, i)->type == T_COMMA && peek(p, i + 1)->type == T_IDENT) i += 2;
      if (peek(p, i)->type == T_IDENT && strcmp(peek(p, i)->payload, "in") == 0) is_for = true;
    }
  }
  if (is_for) {
    AxNode *n = node(p, N_FOR);
    NameList vars = {0};
    name_push(&vars, tok_name_str(advance(p)));
    while (at(p, T_COMMA)) { advance(p); name_push(&vars, tok_name_str(expect(p, T_IDENT, "as a loop variable"))); }
    advance(p);   // `in`
    n->a = parse_expr(p);
    expect(p, T_COLON, "after the loop header");
    NodeList body = {0};
    parse_body(p, &body);
    n->names = vars.items;
    n->nnames = vars.n;
    n->list = body.items;
    n->nlist = body.n;
    return n;
  }
  AxNode *n = node(p, N_WHILE);
  n->a = parse_expr(p);
  expect(p, T_COLON, "after the loop condition");
  NodeList body = {0};
  parse_body(p, &body);
  n->list = body.items;
  n->nlist = body.n;
  return n;
}

static AxNode *parse_try(P *p) {
  AxNode *n = node(p, N_TRY);
  advance(p);   // ^
  advance(p);   // try
  expect(p, T_COLON, "after ^try");
  NodeList body = {0};
  parse_body(p, &body);
  n->list = body.items;
  n->nlist = body.n;
  skip_newlines(p);
  if (at(p, T_CARET) && at_kw_off(p, 1, "catch")) {
    advance(p); advance(p);
    if (at(p, T_IDENT)) n->str = tok_name_str(advance(p));
    expect(p, T_COLON, "after ^catch");
    NodeList cb = {0};
    parse_body(p, &cb);
    n->c = body_node(p, &cb);
    skip_newlines(p);
  }
  if (at(p, T_CARET) && at_kw_off(p, 1, "fin")) {
    advance(p); advance(p);
    expect(p, T_COLON, "after ^fin");
    NodeList fb = {0};
    parse_body(p, &fb);
    n->b = body_node(p, &fb);
  }
  if (!n->c && !n->b) perr(p, "^try: needs a ^catch: or ^fin: clause");
  return n;
}

static AxNode *parse_stmt_inner(P *p);

// A statement is located at its first token (parser.js), which is where a runtime fault in it
// is reported.
static AxNode *parse_stmt(P *p) {
  AxTok *t = peek(p, 0);
  int line = t->line, col = t->col;
  AxNode *n = parse_stmt_inner(p);
  if (n) { n->line = line; n->col = col; }
  return n;
}

static AxNode *parse_stmt_inner(P *p) {
  // !!assert
  if (at(p, T_BANGBANG)) {
    advance(p);
    AxNode *n = node(p, N_ASSERT);
    n->a = parse_expr(p);
    end_stmt(p);
    return n;
  }
  // !action(...) — an engine action, or a call to a ^proc/^fn/library function whose result
  // is not wanted.
  if (at(p, T_BANG)) {
    AxNode *n = node(p, N_ACTION);
    advance(p);
    n->str = tok_name_str(expect(p, T_IDENT, "after '!'"));
    NodeList args = {0};
    if (at(p, T_LPAREN)) parse_args(p, &args);
    n->list = args.items;
    n->nlist = args.n;
    end_stmt(p);
    return n;
  }
  // ++x / --x
  if (at(p, T_PLUSPLUS) || at(p, T_MINUSMINUS)) {
    int op = at(p, T_PLUSPLUS) ? OP_ADD : OP_SUB;
    advance(p);
    AxTok *name = expect(p, T_IDENT, "after '++'");
    AxNode *one = node(p, N_NUM);
    one->num = 1;
    if (at(p, T_DOT) || at(p, T_LBRACKET)) {
      // ++obj.field / ++arr[i]
      NameList path = {0};
      name_push(&path, tok_name_str(name));
      if (at(p, T_LBRACKET)) {
        advance(p);
        AxNode *n = node(p, N_INDEX_ASSIGN);
        n->str = tok_name_str(name);
        n->a = parse_expr(p);
        expect(p, T_RBRACKET, NULL);
        n->b = one;
        n->op = (uint8_t)op;
        end_stmt(p);
        return n;
      }
      while (at(p, T_DOT)) { advance(p); name_push(&path, tok_name_str(expect(p, T_IDENT, "in the path"))); }
      AxNode *n = node(p, N_MEMBER_ASSIGN);
      n->names = path.items;
      n->nnames = path.n;
      n->a = one;
      n->op = (uint8_t)op;
      end_stmt(p);
      return n;
    }
    AxNode *n = node(p, N_ASSIGN);
    n->str = tok_name_str(name);
    n->a = one;
    n->op = (uint8_t)op;
    end_stmt(p);
    return n;
  }
  if (at(p, T_CARET)) {
    if (at_kw_off(p, 1, "return")) {
      advance(p); advance(p);
      AxNode *n = node(p, N_RETURN);
      if (!at(p, T_NEWLINE) && !at(p, T_SEMI) && !at(p, T_DEDENT) && !at(p, T_EOF)) n->a = parse_expr(p);
      end_stmt(p);
      return n;
    }
    if (at_kw_off(p, 1, "throw")) {
      advance(p); advance(p);
      AxNode *n = node(p, N_THROW);
      if (!at(p, T_NEWLINE) && !at(p, T_SEMI)) n->a = parse_expr(p);
      end_stmt(p);
      return n;
    }
    if (at_kw_off(p, 1, "try")) return parse_try(p);
    if (at_kw_off(p, 1, "catch") || at_kw_off(p, 1, "fin")) perr(p, "^%s without a preceding ^try:", peek(p, 1)->payload);
    if (at_kw_off(p, 1, "emit")) {
      // ^emit a.b(args) — publish on a channel.
      AxNode *n = node(p, N_EMIT);
      advance(p); advance(p);
      NameList path = {0};
      name_push(&path, tok_name_str(expect(p, T_IDENT, "as a channel name")));
      while (at(p, T_DOT)) { advance(p); name_push(&path, tok_name_str(expect(p, T_IDENT, "in the channel path"))); }
      NodeList args = {0};
      parse_args(p, &args);
      n->names = path.items;
      n->nnames = path.n;
      n->list = args.items;
      n->nlist = args.n;
      end_stmt(p);
      return n;
    }
    // ^Event(args) [to #Tag | to(#Tag) | within(r[, origin: e])] — a broadcast.
    AxNode *n = node(p, N_BROADCAST);
    advance(p);
    n->str = tok_name_str(expect(p, T_IDENT, "as the event name"));
    NodeList args = {0};
    parse_args(p, &args);
    n->list = args.items;
    n->nlist = args.n;
    if (at_kw(p, "to")) {
      advance(p);
      bool paren = at(p, T_LPAREN);
      if (paren) advance(p);
      expect(p, T_HASH, "before the broadcast target");
      n->str2 = tok_name_str(expect(p, T_IDENT, "as the broadcast target"));
      if (paren) expect(p, T_RPAREN, NULL);
      n->op = 1;
    } else if (at_kw(p, "within")) {
      advance(p);
      expect(p, T_LPAREN, NULL);
      n->a = parse_expr(p);
      if (at(p, T_COMMA)) {
        advance(p);
        expect(p, T_IDENT, "('origin')");
        expect(p, T_COLON, NULL);
        n->b = parse_expr(p);
      }
      expect(p, T_RPAREN, NULL);
      n->op = 2;
    }
    end_stmt(p);
    return n;
  }
  // ?* match
  if (at(p, T_QUESTION) && peek(p, 1)->type == T_STAR) return parse_match(p);
  // ?cond: / if cond:
  if (at(p, T_QUESTION) || at_kw(p, "if")) return parse_cond(p);
  if (at(p, T_QMARKEQ)) perr(p, "?! (else) without a preceding ?cond:");
  if (at_kw(p, "elif") || at_kw(p, "else")) perr(p, "%s without a preceding if/?cond:", peek(p, 0)->payload);
  // loops
  if (at(p, T_STAR)) return parse_loop(p);
  // ~break / ~continue / ~name: value
  if (at(p, T_TILDE) && peek(p, 1)->type == T_IDENT) {
    const char *kw = peek(p, 1)->payload;
    if (strcmp(kw, "break") == 0 || strcmp(kw, "continue") == 0) {
      advance(p); advance(p);
      AxNode *n = node(p, kw[0] == 'b' ? N_BREAK : N_CONTINUE);
      end_stmt(p);
      return n;
    }
    // `~name: value` (declare/assign a local) or `~name += value`
    if (peek(p, 2)->type == T_COLON || compound_op_of(peek(p, 2)->type) != OP_NONE) {
      advance(p);
      AxTok *name = advance(p);
      AxNode *n = node(p, N_ASSIGN);
      n->str = tok_name_str(name);
      n->flag = true;   // declare in the current scope
      if (at(p, T_COLON)) advance(p);
      else n->op = (uint8_t)compound_op_of(advance(p)->type);
      n->a = parse_expr(p);
      end_stmt(p);
      return n;
    }
  }
  if (at(p, T_AT)) perr(p, "an @entity is declared at the top level, not inside a block");
  if (at(p, T_AMP)) perr(p, "an &block belongs directly inside an @entity, not inside another block");

  // `#Tag.field = v` (and op=) — a write to another entity's field.
  if (at(p, T_HASH)) {
    AxNode *n = node(p, N_MEMBER_ASSIGN);
    advance(p);
    n->str2 = tok_name_str(expect(p, T_IDENT, "after '#'"));
    NameList path = {0};
    while (at(p, T_DOT)) { advance(p); name_push(&path, tok_name_str(expect(p, T_IDENT, "in the field path"))); }
    if (!path.n) perr(p, "expected .field after #%s in an assignment", n->str2->data);
    if (!at_assign_op(p)) perr(p, "expected = after #%s.%s (cross-entity writes must assign)", n->str2->data, path.items[0]->data);
    AxTokType opt = advance(p)->type;
    n->op = (uint8_t)(opt == T_ASSIGN ? OP_NONE : compound_op_of(opt));
    n->names = path.items;
    n->nnames = path.n;
    n->a = parse_expr(p);
    end_stmt(p);
    return n;
  }
  // `$belief ~= observe(...)` / `$x = …` — the sigil is decoration on an ordinary name.
  if (at(p, T_DOLLAR) && peek(p, 1)->type == T_IDENT) advance(p);

  // `state -> chase if near -> idle` — a guarded transition chain.
  if (at(p, T_IDENT) && peek(p, 1)->type == T_ARROW) {
    AxNode *n = node(p, N_TRANSITION);
    n->str = tok_name_str(advance(p));
    NodeList clauses = {0};
    while (at(p, T_ARROW)) {
      advance(p);
      AxNode *c = node(p, N_BLOCK);
      c->str = tok_name_str(expect(p, T_IDENT, "as a transition target"));
      NodeList args = {0};
      if (at(p, T_LPAREN)) parse_args(p, &args);
      c->list = args.items;
      c->nlist = args.n;
      if (at_kw(p, "if")) { advance(p); c->a = parse_expr(p); }
      nl_push(&clauses, c);
    }
    n->list = clauses.items;
    n->nlist = clauses.n;
    end_stmt(p);
    return n;
  }
  // `belief ~= observe(...)`
  if (at(p, T_IDENT) && peek(p, 1)->type == T_TILDEEQ) {
    AxNode *n = node(p, N_ASSIGN);
    n->str = tok_name_str(advance(p));
    advance(p);
    n->op = OP_OBSERVE;
    n->a = parse_expr(p);
    end_stmt(p);
    return n;
  }

  // Assignments and expression statements.
  if (at(p, T_IDENT)) {
    // Destructuring: a, b = expr
    if (peek(p, 1)->type == T_COMMA) {
      int save = p->pos;
      NameList names = {0};
      name_push(&names, tok_name_str(advance(p)));
      bool ok = true;
      while (at(p, T_COMMA)) {
        advance(p);
        if (!at(p, T_IDENT)) { ok = false; break; }
        name_push(&names, tok_name_str(advance(p)));
      }
      if (ok && at(p, T_ASSIGN)) {
        advance(p);
        AxNode *n = node(p, N_DESTRUCTURE);
        n->names = names.items;
        n->nnames = names.n;
        n->a = parse_expr(p);
        end_stmt(p);
        return n;
      }
      p->pos = save;
    }
    // name = / name op= / name.path = / name[i] =
    if (at_assign_op(p) == false && peek(p, 1)->type == T_DOT) {
      int save = p->pos;
      NameList path = {0};
      name_push(&path, tok_name_str(advance(p)));
      while (at(p, T_DOT) && peek(p, 1)->type == T_IDENT) { advance(p); name_push(&path, tok_name_str(advance(p))); }
      if (at_assign_op(p)) {
        AxTokType opt = advance(p)->type;
        AxNode *n = node(p, N_MEMBER_ASSIGN);
        n->names = path.items;
        n->nnames = path.n;
        n->op = (uint8_t)(opt == T_ASSIGN ? OP_NONE : compound_op_of(opt));
        n->a = parse_expr(p);
        end_stmt(p);
        return n;
      }
      p->pos = save;
    }
    if (peek(p, 1)->type == T_LBRACKET) {
      int save = p->pos;
      AxStr *obj = tok_name_str(advance(p));
      advance(p);   // [
      AxNode *idx = parse_expr(p);
      if (at(p, T_RBRACKET)) {
        advance(p);
        if (at_assign_op(p)) {
          AxTokType opt = advance(p)->type;
          AxNode *n = node(p, N_INDEX_ASSIGN);
          n->str = obj;
          n->a = idx;
          n->op = (uint8_t)(opt == T_ASSIGN ? OP_NONE : compound_op_of(opt));
          n->b = parse_expr(p);
          end_stmt(p);
          return n;
        }
      }
      p->pos = save;
    }
    if (at_assign_op(p) == false && peek(p, 1)->type != T_DOT && peek(p, 1)->type != T_LBRACKET) {
      // fallthrough to the plain-assignment check below
    }
    if (peek(p, 1)->type == T_ASSIGN || compound_op_of(peek(p, 1)->type) != OP_NONE) {
      AxStr *target = tok_name_str(advance(p));
      AxTokType opt = advance(p)->type;
      AxNode *n = node(p, N_ASSIGN);
      n->str = target;
      n->op = (uint8_t)(opt == T_ASSIGN ? OP_NONE : compound_op_of(opt));
      n->a = parse_expr(p);
      end_stmt(p);
      return n;
    }
  }
  AxNode *n = node(p, N_EXPRSTMT);
  n->a = parse_expr(p);
  end_stmt(p);
  return n;
}

// ---------------------------------------------------------------------------------------------
// Declarations
// ---------------------------------------------------------------------------------------------

static void parse_params(P *p, NameList *names, NodeList *defaults) {
  if (!at(p, T_LPAREN)) return;
  advance(p);
  if (at(p, T_RPAREN)) { advance(p); return; }
  for (;;) {
    if (at(p, T_ARROW)) { advance(p); /* return type */ if (at(p, T_HASH)) advance(p); expect(p, T_IDENT, "as a return type"); break; }
    AxTok *nm = expect(p, T_IDENT, "as a parameter");
    name_push(names, tok_name_str(nm));
    AxNode *def = NULL;
    if (at(p, T_ASSIGN)) { advance(p); def = parse_ternary(p); }
    nl_push(defaults, def);
    if (at(p, T_COMMA)) { advance(p); continue; }
    break;
  }
  if (at(p, T_ARROW)) { advance(p); if (at(p, T_HASH)) advance(p); expect(p, T_IDENT, "as a return type"); }
  expect(p, T_RPAREN, "to close the parameter list");
  if (at(p, T_ARROW)) { advance(p); if (at(p, T_HASH)) advance(p); expect(p, T_IDENT, "as a return type"); }
}

static AxNode *parse_fn(P *p, bool is_main) {
  AxNode *n = node(p, is_main ? N_MAIN : N_FN);
  advance(p);   // ^
  advance(p);   // fn / proc / main
  if (!is_main) n->str = tok_name_str(expect(p, T_IDENT, "as the function name"));
  NameList names = {0};
  NodeList defaults = {0};
  parse_params(p, &names, &defaults);
  n->names = names.items;
  n->nnames = names.n;
  n->defaults = defaults.items;
  if (at(p, T_ASSIGN)) {
    // Expression body: `^fn f(x) = expr`
    advance(p);
    AxNode *ret = node(p, N_RETURN);
    ret->a = parse_expr(p);
    end_stmt(p);
    NodeList body = {0};
    nl_push(&body, ret);
    n->list = body.items;
    n->nlist = body.n;
    return n;
  }
  expect(p, T_COLON, "after the function header");
  NodeList body = {0};
  parse_body(p, &body);
  n->list = body.items;
  n->nlist = body.n;
  return n;
}

static AxNode *parse_type(P *p) {
  AxNode *n = node(p, N_TYPE);
  advance(p);   // ^
  advance(p);   // type
  n->str = tok_name_str(expect(p, T_IDENT, "as the type name"));
  expect(p, T_COLON, "after the type name");
  NameList names = {0};
  bool inline_form = !at(p, T_NEWLINE);
  if (!inline_form) { advance(p); expect(p, T_INDENT, "to start the field list"); }
  for (;;) {
    if (!inline_form && (at(p, T_DEDENT) || at(p, T_EOF))) break;
    AxTok *f = expect(p, T_IDENT, "as a field name");
    name_push(&names, tok_name_str(f));
    if (at(p, T_COLONCOLON)) { advance(p); if (at(p, T_HASH)) advance(p); expect(p, T_IDENT, "as a field type"); }
    if (at(p, T_QUESTION)) advance(p);
    if (at(p, T_COMMA)) { advance(p); continue; }
    if (inline_form) break;
    expect(p, T_NEWLINE, "after a field");
  }
  if (inline_form) end_stmt(p);
  else { expect(p, T_DEDENT, "to end the field list"); }
  n->names = names.items;
  n->nnames = names.n;
  return n;
}

static AxNode *parse_global(P *p) {
  AxNode *n = node(p, N_GLOBAL);
  advance(p);   // ~
  NameList names = {0};
  name_push(&names, tok_name_str(expect(p, T_IDENT, "as a global name")));
  while (at(p, T_COMMA)) { advance(p); name_push(&names, tok_name_str(expect(p, T_IDENT, "as a global name"))); }
  expect(p, T_COLON, "after the global name");
  n->names = names.items;
  n->nnames = names.n;
  n->a = parse_expr(p);
  end_stmt(p);
  return n;
}

static AxNode *parse_use(P *p) {
  AxNode *n = node(p, N_USE);
  advance(p);   // ^
  advance(p);   // use
  NodeList paths = {0};
  for (;;) {
    AxTok *s = expect(p, T_STR, "as an import path");
    AxNode *lit = node(p, N_STR);
    lit->str = ax_str_new(s->payload, s->len);
    nl_push(&paths, lit);
    if (at(p, T_COMMA)) { advance(p); continue; }
    break;
  }
  end_stmt(p);
  n->list = paths.items;
  n->nlist = paths.n;
  return n;
}

// ---------------------------------------------------------------------------------------------
// Engine declarations — each follows the rule of the same name in parser.js.
// ---------------------------------------------------------------------------------------------

// `IDENT (args)?` — the shape/prior/infer calls of a `$` distribution field.
static AxNode *parse_call_like(P *p) {
  AxNode *n = node(p, N_CALL);
  n->str = tok_name_str(expect(p, T_IDENT, "as a constructor name"));
  NodeList args = {0};
  if (at(p, T_LPAREN)) parse_args(p, &args);
  n->list = args.items;
  n->nlist = args.n;
  return n;
}

// `&Pool(T, n)`, `&Vec(T, n)`, `&Map(K, V, n)` — the bounded containers a field may hold.
static AxNode *parse_pool_type(P *p) {
  expect(p, T_AMP, NULL);
  AxNode *n = node(p, N_POOLTYPE);
  AxTok *kw = expect(p, T_IDENT, "after '&'");
  n->str = tok_name_str(kw);
  if (strcmp(kw->payload, "Pool") && strcmp(kw->payload, "Vec") && strcmp(kw->payload, "Map"))
    perr(p, "'&%s' is not a recognized field-type constructor (only '&Pool', '&Vec', '&Map' are defined)", kw->payload);
  expect(p, T_LPAREN, NULL);
  n->str2 = tok_name_str(expect(p, T_IDENT, "as the element type"));
  expect(p, T_COMMA, NULL);
  if (strcmp(kw->payload, "Map") == 0) {
    NameList vt = {0};
    name_push(&vt, tok_name_str(expect(p, T_IDENT, "as the value type")));
    n->names = vt.items;
    n->nnames = vt.n;
    expect(p, T_COMMA, NULL);
  }
  n->num = expect(p, T_NUM, "as the capacity")->num;
  expect(p, T_RPAREN, NULL);
  return n;
}

static AxNode *field_node(P *p, AxStr *name, int sigil, AxNode *value) {
  AxNode *f = node(p, N_FIELD);
  f->str = name;
  f->op = (uint8_t)sigil;
  f->a = value;
  return f;
}

static AxNode *field_value(P *p) {
  if (!at(p, T_COLON)) return NULL;
  advance(p);
  return at(p, T_AMP) ? parse_pool_type(p) : parse_expr(p);
}

// One `~field` (or the multi-name `~a,b,c: v`) or `$belief: Grid(…) ~ Prior infer: s(n)`.
static void parse_field(P *p, NodeList *out) {
  if (at(p, T_TILDE)) {
    advance(p);
    AxStr *name = tok_name_str(expect(p, T_IDENT, "as a field name"));
    if (at(p, T_COMMA) && peek(p, 1)->type == T_IDENT && !at_kw_off(p, 1, "at")) {
      // `~a,b,c: v` only when the names run straight into the colon.
      int i = 1;
      while (peek(p, i)->type == T_COMMA && peek(p, i + 1)->type == T_IDENT) i += 2;
      if (peek(p, i)->type == T_COLON || peek(p, i)->type == T_NEWLINE) {
        NameList names = {0};
        name_push(&names, name);
        while (at(p, T_COMMA)) { advance(p); name_push(&names, tok_name_str(expect(p, T_IDENT, "as a field name"))); }
        AxNode *value = field_value(p);
        for (int k = 0; k < names.n; k++) nl_push(out, field_node(p, names.items[k], 0, value));
        return;
      }
    }
    if (at(p, T_BANG)) advance(p);       // `~hp!: 50` overrides a mixin default on purpose
    nl_push(out, field_node(p, name, 0, field_value(p)));
    return;
  }
  if (at(p, T_DOLLAR)) {
    advance(p);
    AxNode *f = field_node(p, tok_name_str(expect(p, T_IDENT, "as a field name")), 1, NULL);
    expect(p, T_COLON, "after the distribution name");
    f->a = parse_call_like(p);
    if (at(p, T_TILDE)) { advance(p); f->b = parse_call_like(p); }
    if (at_kw(p, "infer")) { advance(p); expect(p, T_COLON, NULL); f->c = parse_call_like(p); }
    nl_push(out, f);
    return;
  }
  perr(p, "expected a field");
}

// A field followed by comma continuations: `~speed: 8, mass: 1, cd: 0s` — a bare name after
// the comma inherits the sigil of the field before it.
static void parse_fields(P *p, NodeList *out) {
  parse_field(p, out);
  while (at(p, T_COMMA)) {
    advance(p);
    if (at_kw(p, "at")) break;
    if (at(p, T_IDENT)) {
      int sigil = out->n ? out->items[out->n - 1]->op : 0;
      AxStr *name = tok_name_str(advance(p));
      if (at(p, T_BANG)) advance(p);
      nl_push(out, field_node(p, name, sigil, field_value(p)));
    } else {
      parse_field(p, out);
    }
  }
}

// `&name:` / `&tick(10hz):` / `&on(Event):` — a scheduled block, indented or on one line with
// `;` between statements.
static AxNode *parse_eblock(P *p) {
  AxNode *n = node(p, N_EBLOCK);
  expect(p, T_AMP, NULL);
  n->str = tok_name_str(expect(p, T_IDENT, "as a block name"));
  if (at(p, T_LPAREN)) {
    advance(p);
    if (at(p, T_NUM)) n->num = advance(p)->num;
    else n->str2 = tok_name_str(expect(p, T_IDENT, "as a frequency or an event name"));
    expect(p, T_RPAREN, NULL);
  }
  expect(p, T_COLON, "after the block header");
  NodeList body = {0};
  if (at(p, T_NEWLINE)) {
    advance(p);
    parse_indented_into(p, &body);
  } else {
    nl_push(&body, parse_stmt(p));
    while (p->pos > 0 && p->toks->toks[p->pos - 1].type == T_SEMI
           && !at(p, T_NEWLINE) && !at(p, T_EOF) && !at(p, T_DEDENT) && !at(p, T_INDENT)) {
      nl_push(&body, parse_stmt(p));
    }
  }
  n->list = body.items;
  n->nlist = body.n;
  return n;
}

static AxNode *parse_entity(P *p);

// One line of an entity body: fields, a block, or a nested entity.
static void parse_member_line(P *p, NodeList *out) {
  if (at(p, T_AT)) { parse_entity(p); skip_newlines(p); return; }   // nested: parsed, not instantiated
  if (at(p, T_AMP)) { nl_push(out, parse_eblock(p)); return; }
  if (at(p, T_TILDE) || at(p, T_DOLLAR)) {
    parse_fields(p, out);
    end_stmt(p);
    return;
  }
  perr(p, "unexpected %s starting an entity member (expected ~field, $field, &block or @entity)", tok_name(peek(p, 0)->type));
}

// `&Name` right after `@Entity` is the base type — unless it is a one-line block
// (`&tick(10hz): …` or `&physics: stmt`).
static bool header_amp_is_block(P *p) {
  if (peek(p, 1)->type != T_IDENT) return false;
  AxTokType after = peek(p, 2)->type;
  if (after == T_LPAREN) return true;
  if (after == T_COLON) {
    AxTokType body = peek(p, 3)->type;
    return body != T_NEWLINE && body != T_EOF && body != T_SEMI;
  }
  return false;
}

static AxNode *parse_entity(P *p) {
  AxNode *n = node(p, N_ENTITY);
  expect(p, T_AT, NULL);
  n->str = tok_name_str(expect(p, T_IDENT, "as the entity name"));
  NodeList members = {0};
  NameList mixins = {0};
  for (;;) {
    if (at(p, T_SEMI)) { advance(p); continue; }
    if (at(p, T_AMP)) {
      if (header_amp_is_block(p)) { nl_push(&members, parse_eblock(p)); continue; }
      advance(p);
      n->str2 = tok_name_str(expect(p, T_IDENT, "as the base type"));
      continue;
    }
    if (at(p, T_PLUS)) {
      advance(p);
      name_push(&mixins, tok_name_str(expect(p, T_IDENT, "as a mixin name")));
      while (at(p, T_COMMA)) { advance(p); name_push(&mixins, tok_name_str(expect(p, T_IDENT, "as a mixin name"))); }
      continue;
    }
    if (at(p, T_TILDE) || at(p, T_DOLLAR)) { parse_fields(p, &members); continue; }
    if (at_kw(p, "at")) { advance(p); n->a = parse_expr(p); continue; }
    if (at(p, T_COLON)) { advance(p); continue; }
    break;
  }
  if (at(p, T_NEWLINE)) advance(p);
  if (at(p, T_INDENT)) {
    advance(p);
    while (!at(p, T_DEDENT) && !at(p, T_EOF)) { parse_member_line(p, &members); skip_newlines(p); }
    expect(p, T_DEDENT, "to end the entity");
  } else if (!members.n) {
    perr(p, "entity '@%s' has no body (expected INDENT after header)", n->str->data);
  }
  n->list = members.items;
  n->nlist = members.n;
  n->names = mixins.items;
  n->nnames = mixins.n;
  return n;
}

// `^event Name: amount:: number, source:: #Entity?` — the schema of a broadcast. A trailing
// `?` marks a field optional (omitted → null rather than defaulted).
static AxNode *parse_event(P *p) {
  AxNode *n = node(p, N_EVENT);
  advance(p);   // ^
  advance(p);   // event
  n->str = tok_name_str(expect(p, T_IDENT, "as the event name"));
  expect(p, T_COLON, "after the event name");
  NodeList fields = {0};
  bool inline_form = !at(p, T_NEWLINE);
  if (!inline_form) { advance(p); expect(p, T_INDENT, "to start the field list"); }
  for (;;) {
    if (!inline_form && (at(p, T_DEDENT) || at(p, T_EOF))) break;
    AxNode *f = node(p, N_IDENT);
    f->str = tok_name_str(expect(p, T_IDENT, "as a field name"));
    if (at(p, T_COLONCOLON)) { advance(p); if (at(p, T_HASH)) advance(p); expect(p, T_IDENT, "as a field type"); }
    if (at(p, T_QUESTION)) { advance(p); f->flag = true; }
    nl_push(&fields, f);
    if (at(p, T_COMMA)) { advance(p); continue; }
    if (inline_form) break;
    expect(p, T_NEWLINE, "after a field");
  }
  if (inline_form) end_stmt(p);
  else expect(p, T_DEDENT, "to end the field list");
  n->list = fields.items;
  n->nlist = fields.n;
  return n;
}

// `^mix Stats: ~hp: 100, max: 100` or an indented body of fields and blocks.
static AxNode *parse_mixin(P *p) {
  AxNode *n = node(p, N_MIXIN);
  advance(p);   // ^
  advance(p);   // mix
  n->str = tok_name_str(expect(p, T_IDENT, "as the mixin name"));
  expect(p, T_COLON, "after the mixin name");
  NodeList members = {0};
  if (!at(p, T_NEWLINE)) {
    if (!at(p, T_TILDE) && !at(p, T_DOLLAR)) perr(p, "expected ~ or $ for inline mixin field");
    parse_fields(p, &members);
    end_stmt(p);
  } else {
    advance(p);
    expect(p, T_INDENT, "to start the mixin body");
    while (!at(p, T_DEDENT) && !at(p, T_EOF)) {
      if (at(p, T_AMP)) nl_push(&members, parse_eblock(p));
      else if (at(p, T_TILDE) || at(p, T_DOLLAR)) { parse_fields(p, &members); end_stmt(p); }
      else perr(p, "unexpected %s in mixin body", tok_name(peek(p, 0)->type));
      skip_newlines(p);
    }
    expect(p, T_DEDENT, "to end the mixin");
  }
  n->list = members.items;
  n->nlist = members.n;
  return n;
}

// `^mat Wall: albedo: #Tex, rough: 0.5` — the colon after the name is optional.
static AxNode *parse_material(P *p) {
  AxNode *n = node(p, N_MATERIAL);
  advance(p);   // ^
  advance(p);   // mat
  n->str = tok_name_str(expect(p, T_IDENT, "as the material name"));
  if (at(p, T_COLON)) advance(p);
  NodeList props = {0};
  bool block = at(p, T_NEWLINE);
  if (block) { advance(p); expect(p, T_INDENT, "to start the material body"); }
  for (;;) {
    if (block && (at(p, T_DEDENT) || at(p, T_EOF))) break;
    AxStr *name = tok_name_str(expect(p, T_IDENT, "as a material property"));
    expect(p, T_COLON, "after a material property");
    nl_push(&props, pair_node(p, name, NULL, parse_expr(p)));
    if (at(p, T_COMMA)) { advance(p); continue; }
    if (!block) break;
    expect(p, T_NEWLINE, "after a material property");
  }
  if (block) expect(p, T_DEDENT, "to end the material");
  else end_stmt(p);
  n->list = props.items;
  n->nlist = props.n;
  return n;
}

// `#Mesh3D Name: "file.glb"`, `#Mesh3D Name: base64("…")`, or a `glb:` heredoc of quoted chunks.
static AxNode *parse_resource(P *p) {
  AxNode *n = node(p, N_RESOURCE);
  expect(p, T_HASH, NULL);
  n->str = tok_name_str(expect(p, T_IDENT, "as the resource kind"));
  n->str2 = tok_name_str(expect(p, T_IDENT, "as the resource name"));
  expect(p, T_COLON, "after the resource name");
  if (at_kw(p, "base64") && peek(p, 1)->type == T_LPAREN) {
    advance(p); advance(p);
    AxTok *t = expect(p, T_STR, "as the base64 payload");
    n->b = node(p, N_STR);
    n->b->str = ax_str_new(t->payload, t->len);
    expect(p, T_RPAREN, NULL);
    end_stmt(p);
    return n;
  }
  if (at_kw(p, "glb") && peek(p, 1)->type == T_COLON) {
    advance(p); advance(p);
    expect(p, T_NEWLINE, NULL);
    expect(p, T_INDENT, "to start the glb heredoc");
    char *buf = NULL;
    size_t len = 0, cap = 0;
    int chunks = 0;
    while (!at(p, T_DEDENT) && !at(p, T_EOF)) {
      AxTok *t = expect(p, T_STR, "as a base64 chunk");
      ax_str_append(&buf, &len, &cap, t->payload, t->len);
      expect(p, T_NEWLINE, NULL);
      chunks++;
    }
    expect(p, T_DEDENT, NULL);
    if (!chunks) perr(p, "#%s %s: glb: heredoc requires at least one quoted base64 chunk", n->str->data, n->str2->data);
    n->b = node(p, N_STR);
    n->b->str = ax_str_new(buf, len);
    free(buf);
    return n;
  }
  AxTok *t = expect(p, T_STR, "as the resource path");
  n->a = node(p, N_STR);
  n->a->str = ax_str_new(t->payload, t->len);
  end_stmt(p);
  return n;
}

bool ax_parse(AxTokens *toks, AxParseResult *out) {
  memset(out, 0, sizeof *out);
  P p = { .toks = toks, .pos = 0, .res = out };
  AxNode *prog = NULL;
  NodeList decls = {0};
  if (setjmp(p.bail) != 0) {
    out->program = NULL;
    return false;
  }
  prog = node(&p, N_PROGRAM);
  skip_newlines(&p);
  while (!at(&p, T_EOF)) {
    if (at(&p, T_CARET) && peek(&p, 1)->type == T_IDENT) {
      const char *kw = peek(&p, 1)->payload;
      if (strcmp(kw, "fn") == 0 || strcmp(kw, "proc") == 0) { nl_push(&decls, parse_fn(&p, false)); skip_newlines(&p); continue; }
      if (strcmp(kw, "main") == 0) { nl_push(&decls, parse_fn(&p, true)); skip_newlines(&p); continue; }
      if (strcmp(kw, "type") == 0) { nl_push(&decls, parse_type(&p)); skip_newlines(&p); continue; }
      if (strcmp(kw, "use") == 0) { nl_push(&decls, parse_use(&p)); skip_newlines(&p); continue; }
      if (strcmp(kw, "event") == 0) { nl_push(&decls, parse_event(&p)); skip_newlines(&p); continue; }
      if (strcmp(kw, "mix") == 0) { nl_push(&decls, parse_mixin(&p)); skip_newlines(&p); continue; }
      if (strcmp(kw, "mat") == 0) { nl_push(&decls, parse_material(&p)); skip_newlines(&p); continue; }
      perr(&p, "unknown declaration '^%s'", kw);
    }
    if (at(&p, T_TILDE) && peek(&p, 1)->type == T_IDENT) { nl_push(&decls, parse_global(&p)); skip_newlines(&p); continue; }
    if (at(&p, T_AT) && at_kw_off(&p, 1, "input"))
      perr(&p, "@input: is not supported — use input.move (Vec2), input.jump (bool), input.fire (bool), input.aim (Vec2) directly in blocks");
    if (at(&p, T_AT)) { nl_push(&decls, parse_entity(&p)); skip_newlines(&p); continue; }
    if (at(&p, T_HASH)) { nl_push(&decls, parse_resource(&p)); skip_newlines(&p); continue; }
    perr(&p, "unexpected %s at the top level", tok_name(peek(&p, 0)->type));
  }
  prog->list = decls.items;
  prog->nlist = decls.n;
  out->program = prog;
  return true;
}
