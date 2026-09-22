// lexer.c — indentation- and sigil-aware tokenizer.
//
// Mirrors lexer.js stage for stage, because the two implementations have to agree about what a
// program means, not just about what it computes:
//
//   1. strip `//` comments, respecting string literals and triple-quoted strings
//   2. join continuation lines: a line starting with `->`, a line inside a triple-quoted
//      string, and a line that leaves a bracket open
//   3. emit INDENT/DEDENT from the indent stack, and NEWLINE at each logical line's end
//   4. scan the logical line into tokens
//
// The one deliberate difference is memory: tokens point into an owned copy of the source, and
// decoded string payloads hang off the token, so nothing has to be freed during parsing.

#include "axiom.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>

typedef struct {
  AxTokens *out;
  const char *line;
  int line_len;
  int lineno;
  int pos;
  bool failed;
} Lexer;

static void lex_err(AxTokens *t, int line, const char *msg) {
  if (t->err[0]) return;
  snprintf(t->err, sizeof t->err, "%s", msg);
  t->err_line = line;
}

static AxTok *push_tok(AxTokens *t, AxTokType type, int line, int col) {
  if (t->count == t->cap) {
    t->cap = t->cap ? t->cap * 2 : 256;
    t->toks = realloc(t->toks, sizeof(AxTok) * t->cap);
    if (!t->toks) { fprintf(stderr, "axiom: out of memory\n"); exit(70); }
  }
  AxTok *tok = &t->toks[t->count++];
  memset(tok, 0, sizeof *tok);
  tok->type = type;
  tok->line = line;
  tok->col = col;
  return tok;
}

// ---------------------------------------------------------------------------------------------
// Line assembly
// ---------------------------------------------------------------------------------------------

typedef struct { char *text; int indent; int lineno; } LogicalLine;

// Bracket depth of a line, ignoring brackets inside string literals. A line that leaves one
// open continues onto the next, which is what makes multi-line arrays and dicts legal.
static int bracket_delta(const char *s, int n) {
  int depth = 0;
  bool in_str = false;
  char q = 0;
  for (int i = 0; i < n; i++) {
    char c = s[i];
    if (in_str) {
      if (c == '\\') { i++; continue; }
      if (c == q) in_str = false;
      continue;
    }
    if (c == '"' || c == '\'') { in_str = true; q = c; continue; }
    if (c == '(' || c == '[' || c == '{') depth++;
    else if (c == ')' || c == ']' || c == '}') depth--;
  }
  return depth;
}

// Find a triple-quote opener not inside a single-quoted string; returns the index after it.
static int find_triple_open(const char *s, int n, char *quote) {
  bool in_str = false;
  char q = 0;
  for (int i = 0; i < n; i++) {
    char c = s[i];
    if (in_str) {
      if (c == '\\') { i++; continue; }
      if (c == q) in_str = false;
      continue;
    }
    if (c == '"' || c == '\'') {
      if (i + 2 < n && s[i + 1] == c && s[i + 2] == c) { *quote = c; return i + 3; }
      in_str = true; q = c;
      continue;
    }
  }
  return -1;
}

static int find_triple_close(const char *s, int n, char quote, int from) {
  for (int i = from; i + 2 < n + 1; i++) {
    if (s[i] == '\\') { i++; continue; }
    if (s[i] == quote && s[i + 1] == quote && s[i + 2] == quote) return i;
  }
  return -1;
}

// Strip a `//` comment, honouring string literals and an in-progress triple-quoted string.
static int strip_comment(char *line, int len, bool *in_triple, char *triple_q) {
  bool in_str = false;
  char q = 0;
  for (int i = 0; i < len; i++) {
    char c = line[i];
    if (*in_triple) {
      if (c == *triple_q && i + 2 < len && line[i + 1] == *triple_q && line[i + 2] == *triple_q) {
        *in_triple = false;
        i += 2;
      }
      continue;
    }
    if (in_str) {
      if (c == '\\') { i++; continue; }
      if (c == q) in_str = false;
      continue;
    }
    if (c == '"' || c == '\'') {
      if (i + 2 < len && line[i + 1] == c && line[i + 2] == c) {
        // A triple-quote that does not close on this line opens a multi-line string.
        int close = find_triple_close(line, len, c, i + 3);
        if (close < 0) { *in_triple = true; *triple_q = c; i += 2; continue; }
        i = close + 2;
        continue;
      }
      in_str = true; q = c;
      continue;
    }
    if (c == '/' && i + 1 < len && line[i + 1] == '/') return i;
  }
  return len;
}

// ---------------------------------------------------------------------------------------------
// Token scanning for one logical line
// ---------------------------------------------------------------------------------------------

static bool is_ident_start(char c) { return isalpha((unsigned char)c) || c == '_'; }
static bool is_ident_char(char c) { return isalnum((unsigned char)c) || c == '_'; }

// Decode escapes into a freshly allocated buffer, as the reference lexer does: unknown escapes
// are preserved verbatim rather than silently dropped, so the author can see what they wrote.
static char *decode_escapes(const char *s, int n, int *out_len) {
  char *buf = malloc(n + 1);
  int j = 0;
  for (int i = 0; i < n; i++) {
    if (s[i] != '\\' || i + 1 >= n) { buf[j++] = s[i]; continue; }
    char nx = s[++i];
    switch (nx) {
      case 'n': buf[j++] = '\n'; break;
      case 't': buf[j++] = '\t'; break;
      case 'r': buf[j++] = '\r'; break;
      case '0': buf[j++] = '\0'; break;
      case '\\': buf[j++] = '\\'; break;
      case '"': buf[j++] = '"'; break;
      case '\'': buf[j++] = '\''; break;
      case '{': buf[j++] = '{'; break;
      case '}': buf[j++] = '}'; break;
      default: buf[j++] = '\\'; buf[j++] = nx; break;
    }
  }
  buf[j] = '\0';
  *out_len = j;
  return buf;
}

static void scan_line(AxTokens *t, const char *line, int len, int lineno) {
  int i = 0;
  while (i < len) {
    char c = line[i];
    if (c == ' ' || c == '\t') { i++; continue; }
    int col = i + 1;

    // f-strings: f"..." or $"...", one token carrying the raw payload.
    if ((c == 'f' || c == '$') && i + 1 < len && line[i + 1] == '"') {
      int j = i + 2;
      int depth = 0;
      bool in_inner = false;
      char inner_q = 0;
      while (j < len) {
        char d = line[j];
        if (d == '\\' && j + 1 < len) { j += 2; continue; }
        if (in_inner) { if (d == inner_q) in_inner = false; j++; continue; }
        if (depth > 0 && (d == '"' || d == '\'')) { in_inner = true; inner_q = d; j++; continue; }
        if (d == '{') depth++;
        else if (d == '}') { if (depth > 0) depth--; }
        else if (d == '"' && depth == 0) break;
        j++;
      }
      if (j >= len) { lex_err(t, lineno, "unterminated f-string"); return; }
      AxTok *tok = push_tok(t, T_FSTR, lineno, col);
      tok->payload = malloc(j - (i + 2) + 1);
      memcpy(tok->payload, line + i + 2, j - (i + 2));
      tok->payload[j - (i + 2)] = '\0';
      tok->len = (uint32_t)(j - (i + 2));
      i = j + 1;
      continue;
    }

    // Strings, including the triple-quoted form (already joined into this logical line).
    if (c == '"' || c == '\'') {
      char q = c;
      if (i + 2 < len && line[i + 1] == q && line[i + 2] == q) {
        int close = find_triple_close(line, len, q, i + 3);
        if (close < 0) { lex_err(t, lineno, "unterminated triple-quoted string"); return; }
        int raw_len = close - (i + 3);
        int dec_len = 0;
        char *dec = decode_escapes(line + i + 3, raw_len, &dec_len);
        // Dedent: strip the common leading whitespace of the non-empty lines after the first.
        int min_indent = 1 << 30;
        for (int p = 0; p < dec_len; p++) {
          if (p == 0 || dec[p - 1] == '\n') {
            int k = p, sp = 0;
            while (k < dec_len && dec[k] == ' ') { sp++; k++; }
            if (k < dec_len && dec[k] != '\n' && !(p == 0)) { if (sp < min_indent) min_indent = sp; }
          }
        }
        if (min_indent == (1 << 30)) min_indent = 0;
        char *outb = malloc(dec_len + 1);
        int oj = 0;
        for (int p = 0; p < dec_len; ) {
          int line_start = p;
          while (p < dec_len && dec[p] != '\n') p++;
          int line_end = p;
          int skip = 0;
          if (line_start != 0) { while (skip < min_indent && line_start + skip < line_end && dec[line_start + skip] == ' ') skip++; }
          for (int k = line_start + skip; k < line_end; k++) outb[oj++] = dec[k];
          if (p < dec_len) { outb[oj++] = '\n'; p++; }
        }
        outb[oj] = '\0';
        // Trim one leading and one trailing newline, matching the reference's block-string feel.
        char *start = outb;
        int final_len = oj;
        if (final_len && start[0] == '\n') { start++; final_len--; }
        while (final_len && (start[final_len - 1] == '\n' || start[final_len - 1] == ' ')) final_len--;
        AxTok *tok = push_tok(t, T_STR, lineno, col);
        tok->payload = malloc(final_len + 1);
        memcpy(tok->payload, start, final_len);
        tok->payload[final_len] = '\0';
        tok->len = (uint32_t)final_len;
        free(outb);
        free(dec);
        i = close + 3;
        continue;
      }
      int j = i + 1;
      while (j < len && line[j] != q) { if (line[j] == '\\') j++; j++; }
      if (j >= len) { lex_err(t, lineno, "unterminated string literal"); return; }
      int dec_len = 0;
      char *dec = decode_escapes(line + i + 1, j - i - 1, &dec_len);
      AxTok *tok = push_tok(t, T_STR, lineno, col);
      tok->payload = dec;
      tok->len = (uint32_t)dec_len;
      i = j + 1;
      continue;
    }

    // Numbers, including 0x hex and a unit suffix (3s, 10hz, 3f, 2v).
    if (isdigit((unsigned char)c) || (c == '.' && i + 1 < len && isdigit((unsigned char)line[i + 1]))) {
      int j = i;
      double val = 0;
      if (c == '0' && i + 1 < len && (line[i + 1] == 'x' || line[i + 1] == 'X')) {
        j = i + 2;
        unsigned long long hv = 0;
        while (j < len && isxdigit((unsigned char)line[j])) {
          char d = line[j];
          hv = hv * 16 + (isdigit((unsigned char)d) ? d - '0' : (tolower(d) - 'a' + 10));
          j++;
        }
        val = (double)hv;
      } else {
        // At most ONE decimal point, and a '.' only counts when a digit follows it — otherwise
        // `0..5` (a range) would be swallowed whole as a malformed number.
        while (j < len && isdigit((unsigned char)line[j])) j++;
        if (j < len && line[j] == '.' && j + 1 < len && isdigit((unsigned char)line[j + 1])) {
          j++;
          while (j < len && isdigit((unsigned char)line[j])) j++;
        }
        if (j < len && (line[j] == 'e' || line[j] == 'E')) {
          int k = j + 1;
          if (k < len && (line[k] == '+' || line[k] == '-')) k++;
          if (k < len && isdigit((unsigned char)line[k])) { j = k; while (j < len && isdigit((unsigned char)line[j])) j++; }
        }
        char buf[64];
        int n = j - i < 63 ? j - i : 63;
        memcpy(buf, line + i, n);
        buf[n] = '\0';
        val = strtod(buf, NULL);
      }
      AxTok *tok = push_tok(t, T_NUM, lineno, col);
      tok->num = val;
      // Unit suffix: letters immediately after the digits (3s, 10hz, 2v, 3f).
      int u = 0;
      while (j < len && isalpha((unsigned char)line[j]) && u < 7) tok->unit[u++] = line[j++];
      tok->unit[u] = '\0';
      i = j;
      continue;
    }

    // Identifiers. The text is copied into the token: the logical-line buffers are freed as
    // soon as scanning finishes, so a token may not point into them.
    if (is_ident_start(c)) {
      int j = i;
      while (j < len && is_ident_char(line[j])) j++;
      AxTok *tok = push_tok(t, T_IDENT, lineno, col);
      tok->payload = malloc(j - i + 1);
      memcpy(tok->payload, line + i, j - i);
      tok->payload[j - i] = '\0';
      tok->start = tok->payload;
      tok->len = (uint32_t)(j - i);
      i = j;
      continue;
    }

    // Multi-character operators, longest first.
    #define TWO(a, b, tt) if (c == a && i + 1 < len && line[i + 1] == b) { push_tok(t, tt, lineno, col); i += 2; continue; }
    if (c == '?' && i + 2 < len && line[i + 1] == '?' && line[i + 2] == '=') { push_tok(t, T_NULLCOALEQ, lineno, col); i += 3; continue; }
    // `?!` is the else sigil only when followed by ':'; otherwise it is `?` then `!`.
    if (c == '?' && i + 1 < len && line[i + 1] == '!') {
      if (i + 2 < len && line[i + 2] == ':') { push_tok(t, T_QMARKEQ, lineno, col); i += 2; continue; }
      push_tok(t, T_QUESTION, lineno, col); i += 1; continue;
    }
    TWO('?', '?', T_NULLCOAL)
    TWO('&', '&', T_ANDAND)
    TWO('|', '|', T_OROR)
    TWO('|', '>', T_PIPEGT)
    TWO('=', '>', T_FATARROW)
    TWO('=', '=', T_EQEQ)
    TWO('!', '=', T_NE)
    TWO('!', '!', T_BANGBANG)
    TWO('>', '=', T_GE)
    TWO('<', '=', T_LE)
    TWO('-', '>', T_ARROW)
    TWO('~', '>', T_TILDEGT)
    TWO(':', ':', T_COLONCOLON)
    TWO('.', '.', T_DOTDOT)
    TWO('+', '=', T_PLUSEQ)
    TWO('-', '=', T_MINUSEQ)
    TWO('*', '*', T_STARSTAR)
    TWO('*', '=', T_STAREQ)
    TWO('/', '=', T_SLASHEQ)
    TWO('%', '=', T_PERCEQ)
    TWO('+', '+', T_PLUSPLUS)
    TWO('-', '-', T_MINUSMINUS)
    #undef TWO

    AxTokType tt;
    switch (c) {
      case '@': tt = T_AT; break;
      case '~': tt = T_TILDE; break;
      case '$': tt = T_DOLLAR; break;
      case '&': tt = T_AMP; break;
      case '!': tt = T_BANG; break;
      case '?': tt = T_QUESTION; break;
      case '^': tt = T_CARET; break;
      case '#': tt = T_HASH; break;
      case ':': tt = T_COLON; break;
      case '(': tt = T_LPAREN; break;
      case ')': tt = T_RPAREN; break;
      case '[': tt = T_LBRACKET; break;
      case ']': tt = T_RBRACKET; break;
      case ',': tt = T_COMMA; break;
      case '.': tt = T_DOT; break;
      case '{': tt = T_LBRACE; break;
      case '}': tt = T_RBRACE; break;
      case ';': tt = T_SEMI; break;
      case '+': tt = T_PLUS; break;
      case '-': tt = T_MINUS; break;
      case '*': tt = T_STAR; break;
      case '/': tt = T_SLASH; break;
      case '%': tt = T_PERCENT; break;
      case '>': tt = T_GT; break;
      case '<': tt = T_LT; break;
      case '=': tt = T_ASSIGN; break;
      case '|': tt = T_PIPE; break;
      case '\\': tt = T_BACKSLASH; break;
      default: {
        char msg[64];
        snprintf(msg, sizeof msg, "unexpected character '%c'", c);
        lex_err(t, lineno, msg);
        return;
      }
    }
    push_tok(t, tt, lineno, col);
    i++;
  }
}

// ---------------------------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------------------------

bool ax_tokenize(const char *src, AxTokens *out) {
  memset(out, 0, sizeof *out);
  size_t n = strlen(src);
  out->src = malloc(n + 1);
  memcpy(out->src, src, n + 1);

  // Split into physical lines (in place).
  LogicalLine *lines = NULL;
  int nlines = 0, cap_lines = 0;
  bool in_triple = false;
  char triple_q = 0;
  int bracket_depth = 0;
  char *p = out->src;
  int lineno = 0;
  char *pending = NULL;          // accumulating triple-quoted string or bracket continuation
  size_t pending_len = 0;
  int pending_line = 0, pending_indent = 0;

  while (true) {
    char *nl = strchr(p, '\n');
    int plen = nl ? (int)(nl - p) : (int)strlen(p);
    lineno++;
    char *raw = p;
    if (plen && raw[plen - 1] == '\r') plen--;

    // Comment stripping happens before anything else, exactly as in the reference.
    bool was_in_triple = in_triple;
    int content_len = strip_comment(raw, plen, &in_triple, &triple_q);

    int indent = 0;
    while (indent < content_len && raw[indent] == ' ') indent++;
    const char *body = raw + indent;
    int body_len = content_len - indent;
    while (body_len > 0 && (body[body_len - 1] == ' ' || body[body_len - 1] == '\t')) body_len--;

    if (was_in_triple || pending) {
      // Continuation of a multi-line string or a bracketed literal: append verbatim.
      size_t add = (size_t)(was_in_triple ? plen : body_len);
      const char *addp = was_in_triple ? raw : body;
      pending = realloc(pending, pending_len + add + 2);
      pending[pending_len++] = was_in_triple ? '\n' : ' ';
      memcpy(pending + pending_len, addp, add);
      pending_len += add;
      pending[pending_len] = '\0';
      bool still_open = in_triple;
      if (!still_open) {
        bracket_depth += bracket_delta(addp, (int)add);
        if (bracket_depth < 0) bracket_depth = 0;
        still_open = bracket_depth > 0;
      }
      if (!still_open) {
        if (nlines == cap_lines) { cap_lines = cap_lines ? cap_lines * 2 : 64; lines = realloc(lines, sizeof(LogicalLine) * cap_lines); }
        lines[nlines].text = pending;
        lines[nlines].indent = pending_indent;
        lines[nlines].lineno = pending_line;
        nlines++;
        pending = NULL;
        pending_len = 0;
      }
      if (!nl) break;
      p = nl + 1;
      continue;
    }

    if (body_len > 0) {
      // Optional `axiom X.Y` pragma on the first logical line.
      if (!out->version && nlines == 0 && body_len > 6 && strncmp(body, "axiom ", 6) == 0) {
        bool numeric = true;
        for (int i = 6; i < body_len; i++) if (!isdigit((unsigned char)body[i]) && body[i] != '.' && body[i] != ' ') numeric = false;
        if (numeric) {
          out->version = malloc(body_len - 5);
          memcpy(out->version, body + 6, body_len - 6);
          out->version[body_len - 6] = '\0';
          if (!nl) break;
          p = nl + 1;
          continue;
        }
      }
      // A line beginning with `->` continues the previous one (guarded transition chains).
      if (body_len >= 2 && body[0] == '-' && body[1] == '>' && nlines > 0) {
        LogicalLine *prev = &lines[nlines - 1];
        size_t plen2 = strlen(prev->text);
        prev->text = realloc(prev->text, plen2 + body_len + 3);
        prev->text[plen2] = ' ';
        memcpy(prev->text + plen2 + 1, body, body_len);
        prev->text[plen2 + 1 + body_len] = '\0';
        if (!nl) break;
        p = nl + 1;
        continue;
      }
      int delta = in_triple ? 1 : bracket_delta(body, body_len);
      if (in_triple || delta > 0) {
        pending = malloc(body_len + 1);
        memcpy(pending, body, body_len);
        pending[body_len] = '\0';
        pending_len = body_len;
        pending_line = lineno;
        pending_indent = indent;
        bracket_depth = in_triple ? 0 : delta;
      } else {
        if (nlines == cap_lines) { cap_lines = cap_lines ? cap_lines * 2 : 64; lines = realloc(lines, sizeof(LogicalLine) * cap_lines); }
        char *txt = malloc(body_len + 1);
        memcpy(txt, body, body_len);
        txt[body_len] = '\0';
        lines[nlines].text = txt;
        lines[nlines].indent = indent;
        lines[nlines].lineno = lineno;
        nlines++;
      }
    }
    if (!nl) break;
    p = nl + 1;
  }
  if (pending) {
    if (nlines == cap_lines) { cap_lines = cap_lines ? cap_lines * 2 : 64; lines = realloc(lines, sizeof(LogicalLine) * cap_lines); }
    lines[nlines].text = pending;
    lines[nlines].indent = pending_indent;
    lines[nlines].lineno = pending_line;
    nlines++;
  }

  // Indent stack → INDENT/DEDENT, then scan each logical line.
  int stack[128];
  int sp = 0;
  stack[0] = 0;
  for (int i = 0; i < nlines; i++) {
    int ind = lines[i].indent;
    if (ind > stack[sp]) {
      if (sp + 1 >= 128) { lex_err(out, lines[i].lineno, "indentation nested too deeply"); break; }
      stack[++sp] = ind;
      push_tok(out, T_INDENT, lines[i].lineno, 1);
    } else {
      while (sp > 0 && ind < stack[sp]) { sp--; push_tok(out, T_DEDENT, lines[i].lineno, 1); }
    }
    scan_line(out, lines[i].text, (int)strlen(lines[i].text), lines[i].lineno);
    if (out->err[0]) break;
    push_tok(out, T_NEWLINE, lines[i].lineno, 1);
  }
  int last_line = nlines ? lines[nlines - 1].lineno : 1;
  while (sp > 0) { sp--; push_tok(out, T_DEDENT, last_line, 1); }
  push_tok(out, T_EOF, last_line + 1, 1);

  // The token stream holds its own copies of every payload, so the line buffers go now.
  for (int i = 0; i < nlines; i++) free(lines[i].text);
  free(lines);
  return out->err[0] == '\0';
}

void ax_tokens_free(AxTokens *t) {
  for (int i = 0; i < t->count; i++) free(t->toks[i].payload);
  free(t->toks);
  free(t->src);
  free(t->version);
  memset(t, 0, sizeof *t);
}
