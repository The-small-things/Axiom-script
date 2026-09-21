// AxiomScript Lexer
// Indentation-sensitive tokenizer implementing §1.2 (sigil layer) and Appendix A/B of the spec.
//
// Design decision (documented, since the spec's EBNF is illustrative, not a full grammar):
// Multi-line guarded transition chains (§4.2) are written with continuation lines that begin
// with "->" and are visually indented for readability, but are logically part of the SAME
// statement as the line above. Rather than inventing implicit-line-continuation-inside-brackets
// rules the spec never states, we resolve this at the lexer's line-joining stage: any raw line
// whose first non-whitespace token is "->" is merged onto the previous logical line before
// indentation/tokens are computed at all. This keeps the indent stack simple and matches the
// only concrete multi-line construct the spec actually shows.

const TT = {
  AT: 'AT', TILDE: 'TILDE', DOLLAR: 'DOLLAR', AMP: 'AMP', BANG: 'BANG', BANGBANG: 'BANGBANG',
  QUESTION: 'QUESTION', CARET: 'CARET', HASH: 'HASH', QMARKEQ: 'QMARKEQ',
  ARROW: 'ARROW', TILDEEQ: 'TILDEEQ', TILDEGT: 'TILDEGT', COLONCOLON: 'COLONCOLON',
  QMARKGT: 'QMARKGT', DOTDOT: 'DOTDOT',
  COLON: 'COLON', LPAREN: 'LPAREN', RPAREN: 'RPAREN', LBRACKET: 'LBRACKET', RBRACKET: 'RBRACKET',
  COMMA: 'COMMA', DOT: 'DOT', LBRACE: 'LBRACE', RBRACE: 'RBRACE',
  PLUS: 'PLUS', MINUS: 'MINUS', STAR: 'STAR', SLASH: 'SLASH', PERCENT: 'PERCENT',
  // v0.8.8: compound assignment operators. `+=` etc. save 2 tokens vs `= x +`.
  PLUSEQ: 'PLUSEQ', MINUSEQ: 'MINUSEQ', STAREQ: 'STAREQ', SLASHEQ: 'SLASHEQ', PERCEQ: 'PERCEQ',
  // v0.8.8: increment/decrement. `++x` saves 4 tokens vs `x = x + 1`.
  PLUSPLUS: 'PLUSPLUS', MINUSMINUS: 'MINUSMINUS',
  STARSTAR: 'STARSTAR', // v0.8.11: ** exponentiation
  // v0.8.8: null-coalescing. `a ?? b` saves 4 tokens vs `a != null ? a : b`.
  NULLCOAL: 'NULLCOAL',
  // v0.8.17: null-coalescing assignment. `~x ??= v` saves 5-7 tokens vs `?x == null: x = v`.
  NULLCOALEQ: 'NULLCOALEQ',
  // v0.8.9: logical AND/OR operators.
  ANDAND: 'ANDAND', OROR: 'OROR',
  GT: 'GT', LT: 'LT', GE: 'GE', LE: 'LE', EQEQ: 'EQEQ', NE: 'NE', ASSIGN: 'ASSIGN', PIPE: 'PIPE',
  MIDDOT: 'MIDDOT', CROSS: 'CROSS', COMPOSE: 'COMPOSE',
  SEMICOLON: 'SEMICOLON',
  NUMBER: 'NUMBER', HEXNUM: 'HEXNUM', IDENT: 'IDENT', STRING: 'STRING',
  FSTRING: 'FSTRING', // v0.8.7: f"..." and $"..." interpolation (single token, raw payload kept)
  NEWLINE: 'NEWLINE', INDENT: 'INDENT', DEDENT: 'DEDENT', EOF: 'EOF'
};

function isDigit(c) { return c >= '0' && c <= '9'; }
function isAlpha(c) { return /[A-Za-z_]/.test(c); }
function isAlnum(c) { return /[A-Za-z0-9_]/.test(c); }

// v0.8.8: helpers for triple-quoted string line-joining. These scan a single physical line
// for `"""` or `'''` markers, respecting `\"`/`\'` escapes inside single-line strings so we
// don't false-positive on a `"""` that's actually inside a regular `"..."` string.
//
// findTripleOpen: returns {ch: '"'|"'", afterIdx: number} if the line opens a triple-quoted
// string, or null. `afterIdx` is the index AFTER the opening triple (so the close search can
// start from there). Returns the FIRST triple-open (in case a line has multiple).
function findTripleOpen(line) {
  let i = 0;
  let inStr = false, strCh = null;
  while (i < line.length) {
    const c = line[i];
    if (inStr) {
      if (c === '\\') { i += 2; continue; }  // skip escaped char
      if (c === strCh) { inStr = false; strCh = null; }
      i++; continue;
    }
    if (c === '"' || c === "'") {
      // Check for triple.
      if (line[i + 1] === c && line[i + 2] === c) {
        return { ch: c, afterIdx: i + 2 };  // index of the third quote char
      }
      // Single-quote string start.
      inStr = true; strCh = c;
      i++; continue;
    }
    i++;
  }
  return null;
}

// findTripleClose: returns the index of the FIRST `ch ch ch` sequence at or after `startIdx`,
// or -1 if none. Respects `\\` escapes inside the triple string (so `\"""` doesn't false-close).
function findTripleClose(line, ch, startIdx = 0) {
  let i = startIdx;
  while (i < line.length) {
    if (line[i] === '\\') { i += 2; continue; }  // skip escaped char
    if (line[i] === ch && line[i + 1] === ch && line[i + 2] === ch) {
      return i;  // index of the first quote char of the closing triple
    }
    i++;
  }
  return -1;
}

class LexError extends Error {
  constructor(msg, line, col) { super(`Lex error (line ${line}): ${msg}`); this.line = line; this.col = col ?? null; }
}

function tokenize(source) {
  const rawLines = source.replace(/\r\n/g, '\n').split('\n');

  // v0.3: an optional top-of-file version pragma, "axiom X.Y" on its own line, recorded but not
  // sliced out of the line count -- every later line number stays correct relative to the
  // original file, since we skip pushing this one raw line as a logical line rather than
  // removing it from the source string before line-counting begins.
  let version = null;

  // v0.8.1: strip //-to-end-of-line comments from each raw line BEFORE any other processing.
  // This runs before the trim() + "->" join pass so comment-only lines become blank (and are
  // skipped by the blank-line check), and inline comments are removed without affecting the
  // line's indent or tokenization. String literals ("..." and '...') are respected — a //
  // inside a string is part of the string, not a comment. The strip preserves the original
  // line length up to the comment so column numbers in error messages stay correct (the
  // trailing whitespace from the removed comment doesn't affect any token positions).
  // v0.8.8 fix: track triple-quoted string state across lines. A `//` inside a triple-quoted
  // string ("""...""" or '''...''') is part of the string content, NOT a comment. Without this,
  // `stripComment` would truncate the line at `//`, corrupting the triple-quoted string's content.
  let _inTripleStr = false, _tripleStrCh = null;
  const stripComment = (line) => {
    let inStr = false, strCh = null;
    for (let i = 0; i < line.length; i++) {
      const c = line[i];
      if (_inTripleStr) {
        if (c === _tripleStrCh && line[i + 1] === _tripleStrCh && line[i + 2] === _tripleStrCh) {
          _inTripleStr = false; _tripleStrCh = null;
          i += 2; continue;
        }
        continue;
      }
      if ((c === '"' || c === "'") && line[i + 1] === c && line[i + 2] === c) {
        _inTripleStr = true; _tripleStrCh = c;
        i += 2; continue;
      }
      if (inStr) {
        if (c === '\\') { i++; continue; }
        if (c === strCh) { inStr = false; strCh = null; }
        continue;
      }
      if (c === '"' || c === "'") { inStr = true; strCh = c; continue; }
      if (c === '/' && line[i + 1] === '/') {
        return line.slice(0, i);
      }
    }
    return line;
  };

  // Pass 1: join "->"-led continuation lines onto the previous logical line.
  // v0.8.8: also join lines that are inside a triple-quoted string ("""...""" or '''...''').
  // A triple-quoted string can span multiple physical lines; without this joining, each line
  // inside the string would be treated as a separate logical line (and fail to parse, since
  // the content looks like random tokens). We track "inside triple string" state across lines.
  const logicalLines = []; // { indent, text, lineNo }
  let inTripleStr = false;       // v0.8.8: true when inside a """ or ''' that hasn't closed
  let tripleStrCh = null;        // '"' or "'"
  let tripleStartLine = 0;       // for error messages
  let tripleAccum = '';          // accumulated text of the multi-line string's logical line
  let tripleIndent = 0;          // indent of the line where the triple string started
  for (let i = 0; i < rawLines.length; i++) {
    const raw = stripComment(rawLines[i]);
    const trimmed = raw.trim();
    if (inTripleStr) {
      // We're inside a multi-line triple-quoted string. Append this line to the accumulator.
      // Look for the closing triple-quote in this line.
      const closeIdx = findTripleClose(raw, tripleStrCh);
      if (closeIdx >= 0) {
        // Found the close. Append up to and including the close, then close the triple state.
        tripleAccum += '\n' + raw.slice(0, closeIdx + 3);
        logicalLines.push({ indent: tripleIndent, text: tripleAccum.trim(), lineNo: tripleStartLine });
        inTripleStr = false;
        tripleStrCh = null;
        tripleAccum = '';
      } else {
        // No close yet — append the whole line.
        tripleAccum += '\n' + raw;
      }
      continue;
    }
    if (trimmed.length === 0) continue; // blank lines (or comment-only lines) never affect indentation
    if (version === null && logicalLines.length === 0 && /^axiom\s+[0-9]+(\.[0-9]+)*\s*$/.test(trimmed)) {
      version = trimmed.replace(/^axiom\s+/, '').trim();
      continue;
    }
    if (trimmed.startsWith('->') && logicalLines.length > 0) {
      logicalLines[logicalLines.length - 1].text += '  ' + trimmed;
      continue;
    }
    // v0.8.8: check if this line OPENS a triple-quoted string that doesn't close on the same line.
    const tripleOpen = findTripleOpen(raw);
    if (tripleOpen) {
      // The line contains `"""` or `'''`. Check if the matching close is on the SAME line.
      // Start the close search immediately AFTER the opening triple (at afterIdx + 1, where
      // afterIdx is the index of the third quote char). This handles `""""""` (empty triple
      // string) correctly: open at 0-2, close search starts at 3, finds close at 3-5.
      const closeIdx = findTripleClose(raw, tripleOpen.ch, tripleOpen.afterIdx + 1);
      if (closeIdx >= 0) {
        // Single-line triple-quoted string — treat as a normal logical line.
        const indent = raw.match(/^[ ]*/)[0].length;
        logicalLines.push({ indent, text: trimmed, lineNo: i + 1 });
      } else {
        // Multi-line triple-quoted string — start accumulating.
        inTripleStr = true;
        tripleStrCh = tripleOpen.ch;
        tripleStartLine = i + 1;
        tripleIndent = raw.match(/^[ ]*/)[0].length;
        tripleAccum = raw;
      }
      continue;
    }
    const indent = raw.match(/^[ ]*/)[0].length;
    logicalLines.push({ indent, text: trimmed, lineNo: i + 1 });
  }
  // v0.8.8: if we're still in a triple string at EOF, it's an unterminated string error.
  // The tokenizer will report it when it tries to lex the accumulated text.

  const tokens = [];
  const indentStack = [0];

  for (const line of logicalLines) {
    if (line.indent > indentStack[indentStack.length - 1]) {
      indentStack.push(line.indent);
      tokens.push({ type: TT.INDENT, line: line.lineNo });
    } else {
      while (line.indent < indentStack[indentStack.length - 1]) {
        indentStack.pop();
        tokens.push({ type: TT.DEDENT, line: line.lineNo });
      }
    }
    tokenizeLine(line.text, line.lineNo, tokens);
    tokens.push({ type: TT.NEWLINE, line: line.lineNo });
  }
  while (indentStack.length > 1) {
    indentStack.pop();
    tokens.push({ type: TT.DEDENT, line: logicalLines.length ? logicalLines[logicalLines.length - 1].lineNo : 0 });
  }
  tokens.push({ type: TT.EOF, line: logicalLines.length ? logicalLines[logicalLines.length - 1].lineNo + 1 : 1 });
  return { tokens, version };
}

// Column is tracked per-token, 1-based, relative to the (possibly "->"-joined) logical line's
// own text -- the same caveat "line" already has for a joined multi-line transition chain (every
// token on a continuation line reports the *opening* line's number, and now also a column
// relative to the joined text rather than the original physical line). Good enough to disambiguate
// findings on a packed line like "~speed: 6, ~accel: 15"; not a promise of pixel-perfect IDE columns.
function tokenizeLine(text, lineNo, out) {
  let i = 0;
  const n = text.length;

  while (i < n) {
    const c = text[i];
    if (c === ' ' || c === '\t') { i++; continue; }
    const colStart = i + 1;
    const push = (type, value) => out.push({ type, value, line: lineNo, col: colStart });

    // v0.8.7: f-string detection — `f"..."` and `$"..."` lex as a single FSTRING token.
    // The `f` / `$` must be immediately followed by `"` (no whitespace). The raw payload
    // (text between the quotes) is kept verbatim; the parser splits it on `{` / `}` (with
    // `{{` / `}}` escapes) and recursively parses each `{expr}` placeholder as an expression.
    // The `prefix` field records which form was used so the AST can preserve the distinction
    // (cosmetic — both forms produce identical evaluation semantics).
    //
    // Important: this check runs BEFORE the isAlpha scan so `f"..."` is recognized even though
    // `f` is also a valid IDENT start. The isAlpha scan would otherwise consume `f` alone (since
    // `"` is not alnum), making `f"..."` lex as IDENT(f) + STRING(...) — two tokens instead of
    // one. Intercepting here keeps it one token, which is the whole point (saves the LLM 1 token
    // vs the `f` + STRING form).
    //
    // Brace / quote handling: while scanning the payload, we track `{` `}` depth so a `"` inside
    // a `{...}` placeholder does NOT terminate the f-string. This lets LLMs write
    // `f"x={cond ? 'a' : 'b'}"` (single quotes inside the placeholder) or even
    // `f"x={cond ? "a" : "b"}"` (double quotes — supported because we enter "string-inside-
    // placeholder" mode when we see `"` or `'` between `{` and `}`). Outside placeholders, the
    // first unescaped `"` terminates the f-string.
    if ((c === 'f' || c === '$') && text[i + 1] === '"') {
      const prefix = c;
      let j = i + 2; let s = '';
      let braceDepth = 0;
      let inStr = false, strCh = null;
      // v0.8.8: escape handling inside the f-string payload. We process escapes in the LITERAL
      // text portions (outside placeholders). Inside placeholders, escapes are handled by the
      // sub-tokenizer when the placeholder expression is recursively parsed — so we copy
      // placeholder text verbatim and let the sub-parser deal with it.
      //
      // For literal text: `\n` → newline, `\t` → tab, `\\` → backslash, `\"` → quote (does NOT
      // end the f-string — same as regular strings), `\'` → single quote, `\{` → literal {
      // (prevents starting a placeholder), `\}` → literal }, `\r`/`\0` similar. Unknown escapes
      // kept verbatim.
      while (j < n) {
        const cj = text[j];
        if (inStr) {
          // Inside a string literal within a placeholder — copy verbatim, but respect `\"`/`\'`
          // so the string's closing quote isn't misdetected.
          if (cj === '\\') {
            s += cj; s += (text[j + 1] !== undefined ? text[j + 1] : ''); j += 2; continue;
          }
          s += cj;
          if (cj === strCh) { inStr = false; strCh = null; }
          j++; continue;
        }
        if (cj === '"' && braceDepth === 0) {
          // End of f-string (only at brace depth 0 — inside a placeholder, `"` starts a string).
          break;
        }
        // v0.8.8: escape handling in literal text (braceDepth === 0).
        // NOTE: `\{` and `\}` are NOT processed here — they're left as `\{`/`\}` in the payload
        // so parseFString can recognize them as literal braces (vs. `{expr}` placeholders).
        // All other escapes (`\n`, `\t`, `\\`, `\"`, etc.) ARE processed here.
        if (cj === '\\' && braceDepth === 0) {
          const next = text[j + 1];
          if (next === undefined) throw new LexError(`unterminated escape in f-string`, lineNo, colStart);
          switch (next) {
            case 'n': s += '\n'; j += 2; break;
            case 't': s += '\t'; j += 2; break;
            case 'r': s += '\r'; j += 2; break;
            case '0': s += '\0'; j += 2; break;
            case '\\': s += '\\'; j += 2; break;
            case '"': s += '"'; j += 2; break;
            case "'": s += "'"; j += 2; break;
            // `\{` and `\}` kept verbatim — parseFString handles them as literal-brace escapes.
            case '{': s += '\\{'; j += 2; break;
            case '}': s += '\\}'; j += 2; break;
            default: s += '\\' + next; j += 2; break;
          }
          continue;
        }
        if (cj === "'" || cj === '"') {
          // Inside a placeholder, a quote starts a string literal — track it so the matching
          // close quote doesn't get misinterpreted.
          if (braceDepth > 0) { inStr = true; strCh = cj; }
          s += cj; j++; continue;
        }
        if (cj === '{') { braceDepth++; s += cj; j++; continue; }
        if (cj === '}') {
          if (braceDepth > 0) braceDepth--;
          s += cj; j++; continue;
        }
        s += cj; j++;
      }
      if (j >= n) throw new LexError(`unterminated f-string literal`, lineNo, colStart);
      out.push({ type: TT.FSTRING, value: s, prefix, line: lineNo, col: colStart });
      i = j + 1; continue;
    }

    // Two/three-char operators first
    if (c === '-' && text[i + 1] === '>') { push(TT.ARROW); i += 2; continue; }
    if (c === '~' && text[i + 1] === '=') { push(TT.TILDEEQ); i += 2; continue; }
    if (c === '~' && text[i + 1] === '>') { push(TT.TILDEGT); i += 2; continue; }
    if (c === ':' && text[i + 1] === ':') { push(TT.COLONCOLON); i += 2; continue; }
    if (c === '?' && text[i + 1] === '>') { push(TT.QMARKGT); i += 2; continue; }
    if (c === '?' && text[i + 1] === '!') {
      // v0.8.8: `?!#Tag` is the ?exists shorthand. Don't combine `?!` into QMARKEQ when
      // followed by `#` — leave as separate `?` `!` `#` tokens so the parser can recognize
      // the ?!# pattern in parsePrimary. QMARKEQ is only for `?!:` (the else-clause sigil),
      // which is always followed by `:`, not `#`.
      if (text[i + 2] === '#') {
        push(TT.QUESTION); i++;
        continue;
      }
      push(TT.QMARKEQ); i += 2; continue;
    }
    // v0.8.8: null-coalescing `??` — must check before `?` (single char) and before `?!`.
    // Note: `?!` is checked above (QMARKEQ); `??` is a distinct token. Check `??` here so it
    // doesn't get mis-tokenized as `?` + `?` (which would be two QUESTION tokens).
    // v0.8.17: `??=` — check before `??` so the `=` is consumed into the token.
    if (c === '?' && text[i + 1] === '?' && text[i + 2] === '=') { push(TT.NULLCOALEQ); i += 3; continue; }
    if (c === '?' && text[i + 1] === '?') { push(TT.NULLCOAL); i += 2; continue; }
    // v0.8.9: logical AND/OR operators. `&&` and `||` — check before single-char `&` and `|`.
    if (c === '&' && text[i + 1] === '&') { push(TT.ANDAND); i += 2; continue; }
    if (c === '|' && text[i + 1] === '|') { push(TT.OROR); i += 2; continue; }
    if (c === '.' && text[i + 1] === '.') { push(TT.DOTDOT); i += 2; continue; }
    if (c === '>' && text[i + 1] === '=') { push(TT.GE); i += 2; continue; }
    if (c === '<' && text[i + 1] === '=') { push(TT.LE); i += 2; continue; }
    if (c === '=' && text[i + 1] === '=') { push(TT.EQEQ); i += 2; continue; }
    if (c === '!' && text[i + 1] === '=') { push(TT.NE); i += 2; continue; }
    if (c === '!' && text[i + 1] === '!') { push(TT.BANGBANG); i += 2; continue; }
    // v0.8.8: compound assignment operators — `+=`, `-=`, `*=`, `/=`, `%=`.
    // Must be checked BEFORE the single-char `+`/`-`/`*`/`/`/`%` and before `++`/`--`.
    if (c === '+' && text[i + 1] === '=') { push(TT.PLUSEQ); i += 2; continue; }
    if (c === '-' && text[i + 1] === '=') { push(TT.MINUSEQ); i += 2; continue; }
    // v0.8.11: ** exponentiation — must be checked before *= and single *.
    if (c === '*' && text[i + 1] === '*') { push(TT.STARSTAR); i += 2; continue; }
    if (c === '*' && text[i + 1] === '=') { push(TT.STAREQ); i += 2; continue; }
    if (c === '/' && text[i + 1] === '=') { push(TT.SLASHEQ); i += 2; continue; }
    if (c === '%' && text[i + 1] === '=') { push(TT.PERCEQ); i += 2; continue; }
    // v0.8.8: increment/decrement — `++`, `--`. Checked BEFORE single-char `+`/`-`.
    if (c === '+' && text[i + 1] === '+') { push(TT.PLUSPLUS); i += 2; continue; }
    if (c === '-' && text[i + 1] === '-') { push(TT.MINUSMINUS); i += 2; continue; }

    // Single-char sigils / operators
    switch (c) {
      case '@': push(TT.AT); i++; continue;
      case '~': push(TT.TILDE); i++; continue;
      case '$': push(TT.DOLLAR); i++; continue;
      case '&': push(TT.AMP); i++; continue;
      case '!': push(TT.BANG); i++; continue;
      case '?': push(TT.QUESTION); i++; continue;
      case '^': push(TT.CARET); i++; continue;
      case '#': push(TT.HASH); i++; continue;
      case ':': push(TT.COLON); i++; continue;
      case '(': push(TT.LPAREN); i++; continue;
      case ')': push(TT.RPAREN); i++; continue;
      case '[': push(TT.LBRACKET); i++; continue;
      case ']': push(TT.RBRACKET); i++; continue;
      case ',': push(TT.COMMA); i++; continue;
      case '{': push(TT.LBRACE); i++; continue;
      case '}': push(TT.RBRACE); i++; continue;
      case '.':
        if (isDigit(text[i + 1])) break; // fall through to number scan
        push(TT.DOT); i++; continue;
      case '+': push(TT.PLUS); i++; continue;
      case '-': push(TT.MINUS); i++; continue;
      case '*': push(TT.STAR); i++; continue;
      case '/': push(TT.SLASH); i++; continue;
      case '%': push(TT.PERCENT); i++; continue;  // v0.8.8: modulo operator
      case '>': push(TT.GT); i++; continue;
      case '<': push(TT.LT); i++; continue;
      case '=': push(TT.ASSIGN); i++; continue;
      case '|': push(TT.PIPE); i++; continue;
      case ';': push(TT.SEMICOLON); i++; continue; // v0.8.7: inline separator (entity header / single-line block)
      case '\u00B7': push(TT.MIDDOT); i++; continue; // ·
      case '\u00D7': push(TT.CROSS); i++; continue;  // ×
      case '\u2218': push(TT.COMPOSE); i++; continue; // ∘
      case "'":
      case '"': {
        // v0.8.8: string escape sequences. Inside a string, `\` introduces an escape:
        //   \n → newline, \t → tab, \\ → backslash, \" → quote (doesn't end string),
        //   \' → single quote, \{ → literal { (for f-strings), \} → literal },
        //   \r → carriage return, \0 → null byte.
        // Any other `\x` is preserved as-is (both chars kept) — don't silently drop unknown
        // escapes; let the LLM see what it wrote. A `\` at end of string (no next char) is an
        // error (unterminated escape).
        //
        // v0.8.8: triple-quoted strings — `"""..."""` or `'''...'''`. Scan until the matching
        // triple-quote. Newlines within are preserved. Common leading whitespace (dedent) is
        // stripped based on the first non-empty line's indentation. Escape sequences still apply.
        // The triple-quote form is for multi-line content (dialog, HUD text) that would otherwise
        // require string concatenation (blocked in hot blocks).
        const quote = c;
        // Check for triple-quote.
        if (text[i + 1] === quote && text[i + 2] === quote) {
          let j = i + 3; let raw = '';
          while (j < n) {
            if (text[j] === quote && text[j + 1] === quote && text[j + 2] === quote) {
              j += 3; break;
            }
            if (text[j] === '\\') {
              const next = text[j + 1];
              if (next === undefined) throw new LexError(`unterminated escape in triple-quoted string`, lineNo, colStart);
              switch (next) {
                case 'n': raw += '\n'; j += 2; break;
                case 't': raw += '\t'; j += 2; break;
                case 'r': raw += '\r'; j += 2; break;
                case '0': raw += '\0'; j += 2; break;
                case '\\': raw += '\\'; j += 2; break;
                case '"': raw += '"'; j += 2; break;
                case "'": raw += "'"; j += 2; break;
                case '{': raw += '{'; j += 2; break;
                case '}': raw += '}'; j += 2; break;
                default: raw += '\\' + next; j += 2; break;
              }
            } else {
              raw += text[j]; j++;
            }
          }
          if (j > n || (j - 3 < n && text[j - 3] !== quote)) {
            // Didn't find the closing triple-quote.
            if (j >= n && (n < 3 || text.slice(n - 3) !== quote.repeat(3))) {
              throw new LexError(`unterminated triple-quoted string literal`, lineNo, colStart);
            }
          }
          // Dedent: find the minimum leading whitespace across all non-empty lines (after the
          // first newline), and strip that amount from each line. This lets LLMs write:
          //   ~msg: """
          //     line 1
          //     line 2
          //   """
          // and get "line 1\nline 2" (no leading whitespace).
          const lines = raw.split('\n');
          if (lines.length > 1) {
            let minIndent = Infinity;
            for (let k = 1; k < lines.length; k++) {
              const line = lines[k];
              if (line.trim().length === 0) continue;  // skip blank lines
              const indent = line.match(/^[ ]*/)[0].length;
              if (indent < minIndent) minIndent = indent;
            }
            if (minIndent === Infinity) minIndent = 0;
            // Strip the common indent from lines 1..N (line 0 is the text after the opening """).
            // Also strip a leading newline if line 0 is empty (common case: """\n  text\n""").
            for (let k = 1; k < lines.length; k++) {
              lines[k] = lines[k].slice(minIndent);
            }
            // If line 0 (the text immediately after """) is empty or whitespace-only, drop it
            // (the content starts on the next line).
            if (lines[0].trim().length === 0) {
              lines.shift();
            }
            raw = lines.join('\n');
            // Strip a single trailing newline if present (the closing """ is usually on its
            // own line, which produces a trailing \n before the """).
            if (raw.endsWith('\n')) raw = raw.slice(0, -1);
          }
          push(TT.STRING, raw); i = j; continue;
        }
        // Single-line string (original logic).
        let j = i + 1; let s = '';
        while (j < n && text[j] !== quote) {
          if (text[j] === '\\') {
            const next = text[j + 1];
            if (next === undefined) throw new LexError(`unterminated escape sequence at end of string`, lineNo, colStart);
            switch (next) {
              case 'n': s += '\n'; j += 2; break;
              case 't': s += '\t'; j += 2; break;
              case 'r': s += '\r'; j += 2; break;
              case '0': s += '\0'; j += 2; break;
              case '\\': s += '\\'; j += 2; break;
              case '"': s += '"'; j += 2; break;
              case "'": s += "'"; j += 2; break;
              case '{': s += '{'; j += 2; break;
              case '}': s += '}'; j += 2; break;
              default: s += '\\' + next; j += 2; break;  // keep unknown escape verbatim
            }
          } else {
            s += text[j]; j++;
          }
        }
        if (j >= n) throw new LexError(`unterminated string literal`, lineNo, colStart);
        push(TT.STRING, s); i = j + 1; continue;
      }
    }

    if (isDigit(c)) {
      // v0.5: hex literal 0x...
      if (c === '0' && (text[i + 1] === 'x' || text[i + 1] === 'X')) {
        let j = i + 2; let s = '0x';
        while (j < n && /[0-9a-fA-F]/.test(text[j])) { s += text[j]; j++; }
        if (j === i + 2) throw new LexError(`invalid hex literal`, lineNo, colStart);
        out.push({ type: TT.HEXNUM, value: parseInt(s, 16), line: lineNo, col: colStart });
        i = j; continue;
      }
      let j = i; let s = '';
      while (j < n && isDigit(text[j])) { s += text[j]; j++; }
      if (text[j] === '.' && isDigit(text[j + 1])) {
        s += '.'; j++;
        while (j < n && isDigit(text[j])) { s += text[j]; j++; }
      }
      let unit = null;
      if (isAlpha(text[j])) {
        let u = '';
        while (j < n && isAlnum(text[j])) { u += text[j]; j++; }
        unit = u;
      }
      out.push({ type: TT.NUMBER, value: parseFloat(s), unit, line: lineNo, col: colStart });
      i = j; continue;
    }

    if (isAlpha(c)) {
      let j = i; let s = '';
      while (j < n && isAlnum(text[j])) { s += text[j]; j++; }
      push(TT.IDENT, s); i = j; continue;
    }

    throw new LexError(`unexpected character '${c}'`, lineNo, colStart);
  }
}

module.exports = { tokenize, TT, LexError };
