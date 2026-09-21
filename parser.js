// AxiomScript Parser
// Builds an AST from the token stream. Follows Appendix B's grammar sketch, extended in the
// (documented) places where the spec's EBNF was explicitly "minimal"/illustrative:
//   - field lines may pack multiple comma-separated "~x: v" declarations on one line (§4.1 uses this)
//   - "@input:" is a distinct pseudo-entity whose members are bare "name: A | B" bindings, not "~" fields
//   - dist_expr is "shape_call ('~' prior_call)?" (e.g. Grid(10,10) ~ Uniform), not just one call
//   - emit is "^emit" <dotted.path> "(" args ")" (the bare EBNF form doesn't have room for the channel path)
//   - call args may be named ("name: expr") for channel binding-by-parameter-name (§2.3),
//     or a bare predicate ("> 0.3") for the belief.any(>0.3) idiom (§4.2)
//
// v0.4 extensions: ^fn/^proc user functions, ^mix mixins, ^mat materials,
//   ?cond: / ?!: conditional blocks, *cond: / *i in 0..n: loops,
//   ^return, ~break, ~continue, !!assert, entity +Mixin and at pose,
//   ?> raycast and .. range expression operators.
const { tokenize, TT } = require('./lexer');

class ParseError extends Error {
  constructor(msg, tok) {
    super(`Parse error (line ${tok ? tok.line : '?'}): ${msg}`);
    this.line = tok ? tok.line : null;
    this.col = tok ? (tok.col ?? null) : null;
  }
}

// v0.8.3: Names that are always queries (never function calls) when followed by `(`.
// `nearest` is a global query (works on any entity). `path`, `raycast`, `block_cell`,
// `unblock_cell`, `is_blocked` are NavMesh3D queries. `exists` is a global query (v0.8.3).
// When the parser sees `name(args)` in expression position and `name` is in this set, it
// produces a Query node (receiverless) instead of a Call node. This ensures `?query(...)`
// works identically in all syntactic positions: assignment, conditional test, ternary, etc.
const KNOWN_QUERY_NAMES = new Set([
  'nearest',   // global: ?nearest(#Prefab, radius?)
  'exists',    // global: ?exists(#Tag) — v0.8.3
  'path',      // NavMesh3D
  'raycast',   // NavMesh3D
  'block_cell',   // NavMesh3D — v0.8.2
  'unblock_cell', // NavMesh3D — v0.8.2
  'is_blocked',   // NavMesh3D — v0.8.2
]);
// v0.8.10: `dist` is BOTH a regular intrinsic (dist(a, b) = 2-arg distance) AND a query
// (?dist(#Tag) = distance from self to target). We do NOT add it to KNOWN_QUERY_NAMES because
// that would make `dist(a, b)` in expression position produce a Query node (wrong — it should
// be a Call). Instead, ?dist(#Tag) is handled in parsePrimary via the `?` prefix, which
// already produces a Query node when followed by IDENT(.

class Parser {
  constructor(tokens) {
    this.toks = tokens;
    this.pos = 0;
  }
  peek(o = 0) { return this.toks[this.pos + o]; }
  at(type) { return this.peek().type === type; }
  atAny(...types) { return types.includes(this.peek().type); }
  advance() { return this.toks[this.pos++]; }
  expect(type, ctx) {
    if (!this.at(type)) throw new ParseError(`expected ${type}${ctx ? ' ' + ctx : ''}, got ${this.peek().type} (${JSON.stringify(this.peek().value)})`, this.peek());
    return this.advance();
  }
  // Skip stray NEWLINE tokens (blank statement separators)
  skipNewlines() { while (this.at(TT.NEWLINE)) this.advance(); }

  parseProgram() {
    const entities = [];
    const resources = [];
    const events = [];
    const types = [];
    const fns = [];
    const procs = [];
    const mixins = []
    const materials = [];
    let inputBlock = null;
    // v0.8.8: parse-error recovery. Collect errors instead of throwing on the first one. When
    // a top-level decl parse throws, record the error, skip to the next `@` / `^` / `#` / EOF,
    // and continue. This lets the LLM see ALL parse errors in one pass and fix them all at once
    // instead of retrying N times for N errors. The errors are stored on the Parser instance;
    // the `parse()` wrapper (below) attaches them to the Program so the checker can surface them.
    this._parseErrors = this._parseErrors || [];
    this.skipNewlines();
    while (!this.at(TT.EOF)) {
      try {
        // v0.8.8: @input: block is REMOVED (Option B per Phase 9 spec). It was parsed but had
        // no runtime effect — input bindings were stored nowhere. Now it's a fatal parse error
        // with a clear message directing the LLM to the fixed input names. The fixed input
        // contract is: input.move (Vec2), input.jump (bool), input.fire (bool), input.aim (Vec2).
        // Custom bindings are not supported; LLMs should read these names directly in blocks.
        if (this.at(TT.AT) && this.peek(1).type === TT.IDENT && this.peek(1).value === 'input') {
          throw new ParseError(`@input: is not supported — use input.move (Vec2), input.jump (bool), input.fire (bool), input.aim (Vec2) directly in blocks. Custom bindings are not supported.`, this.peek());
        } else if (this.at(TT.AT)) {
          entities.push(this.parseEntity());
        } else if (this.at(TT.HASH)) {
          resources.push(this.parseResourceDecl());
        } else if (this.at(TT.CARET) && this.peek(1).type === TT.IDENT && this.peek(1).value === 'event') {
          events.push(this.parseEventDecl());
        } else if (this.at(TT.CARET) && this.peek(1).type === TT.IDENT && this.peek(1).value === 'type') {
          types.push(this.parseTypeDecl());
        } else if (this.at(TT.CARET) && this.peek(1).type === TT.IDENT && this.peek(1).value === 'fn') {
          fns.push(this.parseFnDecl());
        } else if (this.at(TT.CARET) && this.peek(1).type === TT.IDENT && this.peek(1).value === 'proc') {
          procs.push(this.parseProcDecl());
        } else if (this.at(TT.CARET) && this.peek(1).type === TT.IDENT && this.peek(1).value === 'mix') {
          mixins.push(this.parseMixinDecl());
        } else if (this.at(TT.CARET) && this.peek(1).type === TT.IDENT && this.peek(1).value === 'mat') {
          materials.push(this.parseMaterialDecl());
        } else {
          throw new ParseError(`unexpected top-level token ${this.peek().type}`, this.peek());
        }
      } catch (err) {
        if (err instanceof ParseError) {
          this._parseErrors.push(err);
        } else {
          // v0.8.8: wrap non-ParseError throws (e.g. TypeError from peek() at EOF) into a
          // ParseError so recovery can continue. The original message is preserved.
          this._parseErrors.push(new ParseError(err.message || String(err), this.peek() || { line: null, col: null }));
        }
        // Skip to the next top-level decl start (AT, CARET, HASH) or EOF.
        // Consume the current token first (to avoid infinite loop if we're already AT one).
        // Guard against EOF (peek() returns undefined at EOF — this.at() handles it).
        // v0.8.8 fix: also skip NEWLINE and INDENT/DEDENT tokens — after an error line, the
        // remaining tokens on that line include NEWLINE, and the next entity's INDENT may follow.
        // Without skipping these, the loop stops at the next `@` but the INDENT before it was
        // already consumed by the entity's header parse, so the body doesn't parse correctly.
        if (!this.at(TT.EOF)) this.advance();
        while (!this.at(TT.EOF) && !this.at(TT.AT) && !this.at(TT.CARET) && !this.at(TT.HASH)) {
          this.advance();
        }
      }
      this.skipNewlines();
    }
    return { type: 'Program', entities, inputBlock, resources, events, types, fns, procs, mixins, materials };
  }

  // v0.2 §1.6: "#Mesh3D Terrain: \"assets/terrain.mesh\"" — declares a shared, content-addressed
  // native resource. First IDENT is the resource kind (closed registry, §1.6); second is its name.
  //
  // v0.8.4: `#Mesh3D` (and only `#Mesh3D`) now accepts two additional inline forms alongside the
  // existing path form, so the .glb bytes can live inside the .ax source text instead of needing
  // a separate file on disk:
  //
  //   1. Single-line inline:
  //        #Mesh3D Hero: base64("Z2xURgIAAAA...")
  //      The base64 payload is one STRING token (quoted), so the lexer's existing string scan
  //      handles it. Best for small props / characters where the whole blob fits on one line.
  //
  //   2. Multi-line heredoc:
  //        #Mesh3D Hero: glb:
  //          "Z2xURgIAAAA..."
  //          "AAAA..."
  //      Each indented line is a quoted STRING token; the parser concatenates their values
  //      (no separators). Best for larger meshes where a single 100KB+ line would be unreadable.
  //
  // Both forms produce a ResourceDecl with `path: null` and `inlineB64: <string>`. The existing
  // path form (`#Mesh3D Name: "file.glb"`) is unchanged and produces `path: <string>, inlineB64: null`.
  // The interpreter's loadProgram dispatches on `inlineB64` first; the checker validates the
  // base64 decodes and parses as a real .glb (AX-MESH-001) at compile time.
  parseResourceDecl() {
    const line = this.peek().line, col = this.peek().col;
    this.expect(TT.HASH);
    const kind = this.expect(TT.IDENT).value;
    const name = this.expect(TT.IDENT).value;
    this.expect(TT.COLON);

    // v0.8.4: inline single-line form — `base64("...")`.
    if (this.at(TT.IDENT) && this.peek().value === 'base64') {
      this.advance();
      this.expect(TT.LPAREN);
      const payload = this.expect(TT.STRING).value;
      this.expect(TT.RPAREN);
      this.expect(TT.NEWLINE);
      return { type: 'ResourceDecl', kind, name, path: null, inlineB64: payload, line, col };
    }

    // v0.8.4: heredoc form — `glb:` NEWLINE INDENT (STRING NEWLINE)+ DEDENT.
    // Each STRING line is a chunk of base64; their values are concatenated with no separator.
    if (this.at(TT.IDENT) && this.peek().value === 'glb' && this.peek(1).type === TT.COLON) {
      this.advance();   // 'glb'
      this.advance();   // ':'
      this.expect(TT.NEWLINE);
      this.expect(TT.INDENT);
      let payload = '';
      let chunkCount = 0;
      while (!this.at(TT.DEDENT)) {
        payload += this.expect(TT.STRING).value;
        this.expect(TT.NEWLINE);
        chunkCount++;
      }
      this.expect(TT.DEDENT);
      if (chunkCount === 0) {
        throw new ParseError(`#Mesh3D ${name}: glb: heredoc requires at least one quoted base64 chunk`, this.peek());
      }
      return { type: 'ResourceDecl', kind, name, path: null, inlineB64: payload, line, col };
    }

    // Existing path form — `"file.glb"`.
    const path = this.expect(TT.STRING).value;
    this.expect(TT.NEWLINE);
    return { type: 'ResourceDecl', kind, name, path, inlineB64: null, line, col };
  }

  // Shared by "^event Name:" and (v0.3) "^type Name:" -- both are "KEYWORD Name: NEWLINE INDENT
  // (field:: type '?'? NEWLINE)+ DEDENT". The trailing '?' (v0.3) marks a field optional; only
  // meaningful for events today (§2.3.1) -- a Pool element type's fields (v0.3 §2.1.1) are always
  // required, since a partially-initialized pooled struct isn't a sound thing to hand back from
  // "!spawn(...)".
  parseSchemaFields() {
    this.expect(TT.COLON);
    this.expect(TT.NEWLINE);
    this.expect(TT.INDENT);
    const fields = [];
    while (!this.at(TT.DEDENT)) {
      const fname = this.expect(TT.IDENT).value;
      this.expect(TT.COLONCOLON);
      const ftype = this.parseTypeRef();
      let optional = false;
      if (this.at(TT.QUESTION)) { this.advance(); optional = true; }
      fields.push({ name: fname, ftype, optional });
      this.expect(TT.NEWLINE);
    }
    this.expect(TT.DEDENT);
    return fields;
  }

  // v0.2 §2.3.1: "^event Damage: amount:: number \n source:: #Entity" — a broadcast schema.
  parseEventDecl() {
    const line = this.peek().line, col = this.peek().col;
    this.expect(TT.CARET);
    this.expect(TT.IDENT); // 'event' keyword
    const name = this.expect(TT.IDENT).value;
    const fields = this.parseSchemaFields();
    return { type: 'EventDecl', name, fields, line, col };
  }

  // v0.3 §2.1.1: "^type Bullet: pos:: v2 \n vel:: v2" — a lightweight, positional-field data
  // shape, declared so "&Pool(Bullet, 64)" and "!spawn(bullets, Bullet(position, facing*20))"
  // reference something real instead of an undeclared name the checker can't validate against.
  parseTypeDecl() {
    const line = this.peek().line, col = this.peek().col;
    this.expect(TT.CARET);
    this.expect(TT.IDENT); // 'type' keyword
    const name = this.expect(TT.IDENT).value;
    const fields = this.parseSchemaFields();
    return { type: 'TypeDecl', name, fields, line, col };
  }

  // v0.4: "^fn name(a, b -> v3):" — user-defined pure function. Params are bare names;
  // return type after -> is optional (can appear after last param inside parens, or after parens).
  // v0.8.7: "^fn name(a, b) = expr" — expression-body shorthand. After the params, if the next
  //   token is `=`, parse a single expression and use it as an implicit `^return expr` body.
  //   This saves 2-3 tokens per short function (no `:` NEWLINE INDENT `^return` NEWLINE DEDENT).
  //   Same AST as the block form (FnDecl with body = [Return(expr)]).
  parseFnDecl() {
    const line = this.peek().line, col = this.peek().col;
    this.expect(TT.CARET);
    this.expect(TT.IDENT); // 'fn'
    const name = this.expect(TT.IDENT).value;
    const params = [];
    let retType = null;
    if (this.at(TT.LPAREN)) {
      this.advance();
      if (!this.at(TT.RPAREN)) {
        params.push(this.expect(TT.IDENT).value);
        while (this.at(TT.COMMA)) {
          this.advance();
          // v0.4 fix: -> type annotation on last param inside parens
          if (this.at(TT.ARROW)) {
            this.advance();
            retType = this.parseTypeRef();
            break;
          }
          params.push(this.expect(TT.IDENT).value);
        }
        // v0.4 fix: -> can also appear after last param without trailing comma
        if (!retType && this.at(TT.ARROW)) {
          this.advance();
          retType = this.parseTypeRef();
        }
      }
      this.expect(TT.RPAREN);
      // Also support -> type after closing paren
      if (!retType && this.at(TT.ARROW)) {
        this.advance();
        retType = this.parseTypeRef();
      }
    }
    // v0.8.7: expression-body shorthand — `^fn name(args) = expr`
    if (this.at(TT.ASSIGN)) {
      this.advance();
      const expr = this.parseExpr();
      this.expect(TT.NEWLINE);
      const body = [{ type: 'Return', expr, line, col }];
      return { type: 'FnDecl', name, params, retType, body, line, col };
    }
    this.expect(TT.COLON);
    this.expect(TT.NEWLINE);
    this.expect(TT.INDENT);
    const body = [];
    while (!this.at(TT.DEDENT)) { body.push(this.parseStmt()); this.skipNewlines(); }
    this.expect(TT.DEDENT);
    return { type: 'FnDecl', name, params, retType, body, line, col };
  }

  // v0.4: "^proc name(a, b):" — imperative procedure. No return type. Can call !actions.
  parseProcDecl() {
    const line = this.peek().line, col = this.peek().col;
    this.expect(TT.CARET);
    this.expect(TT.IDENT); // 'proc'
    const name = this.expect(TT.IDENT).value;
    const params = [];
    if (this.at(TT.LPAREN)) {
      this.advance();
      if (!this.at(TT.RPAREN)) {
        params.push(this.expect(TT.IDENT).value);
        while (this.at(TT.COMMA)) { this.advance(); params.push(this.expect(TT.IDENT).value); }
      }
      this.expect(TT.RPAREN);
    }
    // v0.8.9: expression-body shorthand — `^proc name(args) = expr`
    if (this.at(TT.ASSIGN)) {
      this.advance();
      const expr = this.parseExpr();
      this.expect(TT.NEWLINE);
      // Wrap as ExprStmt — proc expression body doesn't auto-return (unlike ^fn).
      const body = [{ type: 'ExprStmt', expr, line, col }];
      return { type: 'ProcDecl', name, params, body, line, col };
    }
    this.expect(TT.COLON);
    this.expect(TT.NEWLINE);
    this.expect(TT.INDENT);
    const body = [];
    while (!this.at(TT.DEDENT)) { body.push(this.parseStmt()); this.skipNewlines(); }
    this.expect(TT.DEDENT);
    return { type: 'ProcDecl', name, params, body, line, col };
  }

  // v0.4: "^mix HP: ~hp: 100, ~max: 100" — mixin declaration. Contains field decls and blocks.
  parseMixinDecl() {
    const line = this.peek().line, col = this.peek().col;
    this.expect(TT.CARET);
    this.expect(TT.IDENT); // 'mix'
    const name = this.expect(TT.IDENT).value;
    this.expect(TT.COLON);
    // v0.8.1: support inline mixin — `^mix Stats: ~hp: 100` on one line.
    // If the next token is NOT a NEWLINE, parse fields/blocks on the same line.
    if (!this.at(TT.NEWLINE)) {
      // Inline: parse one or more comma-separated fields on the same line.
      const members = [];
      // First field must have a sigil.
      if (this.at(TT.TILDE) || this.at(TT.DOLLAR)) {
        const sigil = this.at(TT.TILDE) ? '~' : '$';
        // v0.8.7: parseField may return an array (multi-name shorthand); normalize.
        members.push(...[].concat(this.parseField()));
        while (this.at(TT.COMMA)) {
          this.advance();
          // v0.8.1: sigil-less continuation.
          if (this.at(TT.IDENT)) {
            const fname = this.advance().value;
            // v0.8.2: `field!: value` suppresses AX-MIXIN-002 advisory for this field.
            let fnoWarn = false;
            if (this.at(TT.BANG)) { this.advance(); fnoWarn = true; }
            let fval = null;
            if (this.at(TT.COLON)) { this.advance(); fval = this.at(TT.AMP) ? this.parsePoolType() : this.parseExpr(); }
            members.push({ type: 'FieldDecl', sigil, name: fname, value: fval, noWarn: fnoWarn, line: this.peek().line, col: this.peek().col });
          } else {
            members.push(...[].concat(this.parseField()));
          }
        }
        this.expect(TT.NEWLINE);
        return { type: 'MixinDecl', name, members, line, col };
      }
      throw new ParseError(`expected ~ or $ for inline mixin field`, this.peek());
    }
    // Multi-line: indented block.
    this.expect(TT.NEWLINE);
    this.expect(TT.INDENT);
    const members = [];
    while (!this.at(TT.DEDENT)) {
      if (this.at(TT.AMP) && this.isBlockAhead()) {
        members.push(this.parseBlock());
      } else if (this.at(TT.TILDE) || this.at(TT.DOLLAR)) {
        const sigil = this.at(TT.TILDE) ? '~' : '$';
        const fields = [].concat(this.parseField());
        while (this.at(TT.COMMA)) {
          this.advance();
          // v0.8.12: guard against `at` keyword being consumed as a field name in comma-continuation
          if (this.at(TT.IDENT) && this.peek().value === 'at') break;
          if (this.at(TT.IDENT)) {
            const fname = this.advance().value;
            let fnoWarn = false;
            if (this.at(TT.BANG)) { this.advance(); fnoWarn = true; }
            let fval = null;
            if (this.at(TT.COLON)) { this.advance(); fval = this.at(TT.AMP) ? this.parsePoolType() : this.parseExpr(); }
            fields.push({ type: 'FieldDecl', sigil, name: fname, value: fval, noWarn: fnoWarn, line: this.peek().line, col: this.peek().col });
          } else {
            fields.push(...[].concat(this.parseField()));
          }
        }
        this.expect(TT.NEWLINE);
        members.push(...fields);
      } else {
        throw new ParseError(`unexpected token in mixin body: ${this.peek().type}`, this.peek());
      }
      this.skipNewlines();
    }
    this.expect(TT.DEDENT);
    return { type: 'MixinDecl', name, members, line, col };
  }

  // v0.4: "^mat M1: albedo: #Tex, rough: 0.5" — material declaration. Key-value properties.
  // Supports both inline (comma-separated on header line) and indented block styles.
  // v0.8.6 (Bug #9 doc audit): the colon after the name is now OPTIONAL, consistent with
  // entity headers (where `@Tag &Base` works without a colon). The AXIOM_REF.md PBR material
  // example uses `^mat Wall` (no colon) with an indented block — that form now works.
  parseMaterialDecl() {
    const line = this.peek().line, col = this.peek().col;
    this.expect(TT.CARET);
    this.expect(TT.IDENT); // 'mat'
    const name = this.expect(TT.IDENT).value;
    // Colon is optional — both `^mat M: prop: val` and `^mat M\n  prop: val` work.
    if (this.at(TT.COLON)) this.advance();
    // v0.4 fix: check if properties start on the same line (inline style)
    if (this.at(TT.NEWLINE)) {
      this.advance();
      this.expect(TT.INDENT);
      const props = [];
      while (!this.at(TT.DEDENT)) {
        const pName = this.expect(TT.IDENT).value;
        this.expect(TT.COLON);
        const pVal = this.parseExpr();
        props.push({ name: pName, value: pVal });
        if (this.at(TT.COMMA)) { this.advance(); continue; }
        this.expect(TT.NEWLINE);
      }
      this.expect(TT.DEDENT);
      return { type: 'MaterialDecl', name, props, line, col };
    }
    // Inline style: properties on same line as colon
    const props = [];
    while (true) {
      const pName = this.expect(TT.IDENT).value;
      this.expect(TT.COLON);
      const pVal = this.parseExpr();
      props.push({ name: pName, value: pVal });
      if (!this.at(TT.COMMA)) break;
      this.advance(); // consume comma
    }
    this.expect(TT.NEWLINE);
    return { type: 'MaterialDecl', name, props, line, col };
  }

  // A type reference in a schema is either a bare IDENT ("number") or a tag reference
  // ("#Entity") — the latter needed for fields like "source:: #Entity".
  parseTypeRef() {
    if (this.at(TT.HASH)) { this.advance(); return '#' + this.expect(TT.IDENT).value; }
    return this.expect(TT.IDENT).value;
  }

  parseInputBlock() {
    this.expect(TT.AT);
    this.expect(TT.IDENT); // 'input'
    this.expect(TT.COLON);
    this.expect(TT.NEWLINE);
    this.expect(TT.INDENT);
    const bindings = [];
    while (!this.at(TT.DEDENT)) {
      const name = this.expect(TT.IDENT).value;
      this.expect(TT.COLON);
      const sources = [this.expect(TT.IDENT).value];
      while (this.at(TT.PIPE)) { this.advance(); sources.push(this.expect(TT.IDENT).value); }
      bindings.push({ name, sources });
      this.expect(TT.NEWLINE);
    }
    this.expect(TT.DEDENT);
    return { type: 'InputBlock', bindings };
  }

  // v0.8.7: helper to disambiguate `&` in entity header position. Returns 'block' if the `&`
  // starts a one-liner block (i.e. has `(` for frequency or `:` followed by a non-NEWLINE body),
  // 'base' otherwise (bare `&Name`, `&Name:`, `&Name;`, `&Name NEWLINE`).
  // The check is purely lexical — we look at the tokens AHEAD without consuming.
  headerAmpIsBlock() {
    // Current token is AMP. Peek ahead: AMP IDENT ( ... or AMP IDENT : non-NEWLINE.
    if (this.peek().type !== TT.AMP) return false;
    const next = this.peek(1);
    if (!next || next.type !== TT.IDENT) return false;
    const after = this.peek(2);
    if (!after) return false;
    if (after.type === TT.LPAREN) return true; // &tick(10hz): ...
    if (after.type === TT.COLON) {
      // `&Name:` — block only if `:` is followed by a non-NEWLINE token (one-liner body).
      const body = this.peek(3);
      if (body && body.type !== TT.NEWLINE && body.type !== TT.EOF && body.type !== TT.SEMICOLON) return true;
      return false;
    }
    return false;
  }

  parseEntity() {
    const line = this.peek().line, col = this.peek().col;
    this.expect(TT.AT);
    const name = this.expect(TT.IDENT).value;
    let base = null;
    const mixins = [];
    let initPose = null;
    const inlineMembers = [];

    // v0.8.7: restructured header into a loop that accepts `;` as a separator between header
    // elements (base types, mixins, fields, one-liner blocks). The order is now flexible —
    // `@Enemy ~hp:50 +Stats; &Body3D; &physics: vel.y -= 9.8*dt` works (field, mixin, base,
    // one-liner block, all on one line). Existing forms (newline-separated) still work — the
    // loop simply breaks on NEWLINE instead of `;`.
    //
    // Backward compat: every existing form (`@E &Base`, `@E &Base +Mix`, `@E at v3(...) &Base`,
    // `@E &Base ~field: val`, etc.) produces the same AST. The loop just adds `;` as another
    // way to separate header elements on the same physical line.
    let headerDone = false;
    while (!headerDone) {
      if (this.at(TT.SEMICOLON)) { this.advance(); continue; }
      if (this.at(TT.AMP)) {
        if (this.headerAmpIsBlock()) {
          // One-liner block on the header line.
          inlineMembers.push(this.parseBlock());
          continue;
        }
        // Base type.
        this.advance();
        base = this.expect(TT.IDENT).value;
        continue;
      }
      if (this.at(TT.PLUS)) {
        this.advance();
        mixins.push(this.expect(TT.IDENT).value);
        // v0.8.7 (2C): comma-separated mixins — `+M1, M2, M3`.
        while (this.at(TT.COMMA)) { this.advance(); mixins.push(this.expect(TT.IDENT).value); }
        continue;
      }
      if (this.at(TT.TILDE) || this.at(TT.DOLLAR)) {
        // Inline field(s) — parseField handles multi-name and multi-value forms.
        inlineMembers.push(...[].concat(this.parseField()));
        // After a field, allow comma-separated additional fields (sigil-less continuation).
        while (this.at(TT.COMMA)) {
          this.advance();
          // v0.8.12: guard against `at` keyword being consumed as a field name in comma-continuation
          if (this.at(TT.IDENT) && this.peek().value === 'at') break;
          if (this.at(TT.IDENT)) {
            // Sigil-less continuation: inherit sigil from preceding field.
            // v0.8.11 fix: was hardcoded to '~', dropping the '$' sigil from $-field continuations.
            const prevSigil = inlineMembers.length > 0 ? inlineMembers[inlineMembers.length - 1].sigil : '~';
            const fname = this.advance().value;
            let noWarn = false;
            if (this.at(TT.BANG)) { this.advance(); noWarn = true; }
            let value = null;
            if (this.at(TT.COLON)) {
              this.advance();
              value = this.at(TT.AMP) ? this.parsePoolType() : this.parseExpr();
            }
            inlineMembers.push({ type: 'FieldDecl', sigil: prevSigil, name: fname, value, noWarn, line: this.peek().line, col: this.peek().col });
          } else {
            inlineMembers.push(...[].concat(this.parseField()));
          }
        }
        continue;
      }
      if (this.at(TT.IDENT) && this.peek().value === 'at') {
        this.advance();
        initPose = this.parseExpr();
        continue;
      }
      if (this.at(TT.COLON)) {
        // Optional header colon — consume and continue (no-op).
        this.advance();
        continue;
      }
      // Anything else (NEWLINE, EOF, etc.) — header is done.
      headerDone = true;
    }
    // v0.8.7: NEWLINE is optional here — if the last header element was a one-liner block,
    // the block's stmt parser already consumed the trailing NEWLINE. Making it optional also
    // handles EOF (entity at end of file with no trailing newline) and DEDENT (entity inside
    // a nested context, though that's rare).
    if (this.at(TT.NEWLINE)) this.advance();
    // v0.8.6 (Bug #9): when inline fields are present on the header line, the entity may have
    // NO indented body. In that case, the next token is DEDENT or EOF (or another top-level
    // @/^/# declaration), not INDENT. We make the INDENT + body optional when inlineMembers
    // is non-empty.
    const members = [...inlineMembers];
    if (this.at(TT.INDENT)) {
      this.advance();
      while (!this.at(TT.DEDENT)) {
        members.push(...this.parseMemberLine());
      }
      this.expect(TT.DEDENT);
    } else if (inlineMembers.length === 0) {
      // No inline fields AND no INDENT — the entity has no body at all. This was always
      // an error (entities need at least one member or an indented body to be useful),
      // but we give a clearer message than "expected INDENT, got X".
      throw new ParseError(`entity '@${name}' has no body (expected INDENT after header)`, this.peek());
    }
    return { type: 'EntityDecl', name, base, mixins, initPose, members };
  }

  // A single physical line inside an entity body: may hold 1+ comma-separated field decls,
  // a scheduled block, or a nested entity.
  parseMemberLine() {
    if (this.at(TT.AT)) {
      const e = this.parseEntity();
      this.skipNewlines();
      return [e];
    }
    if (this.at(TT.AMP) && this.isBlockAhead()) {
      const b = this.parseBlock();
      return [b];
    }
    if (this.at(TT.TILDE) || this.at(TT.DOLLAR)) {
      const sigil = this.at(TT.TILDE) ? '~' : '$';
      const fields = [].concat(this.parseField());
      while (this.at(TT.COMMA)) {
        this.advance();
        // v0.8.1: sigil-less continuation — after a comma, if the next token is an IDENT
        // (not ~ or $), inherit the sigil from the first field in the list. This lets
        // `~speed: 8, mass: 1, cd: 0s` instead of `~speed: 8, ~mass: 1, ~cd: 0s`.
        if (this.at(TT.IDENT)) {
          const name = this.advance().value;
          let noWarn = false;
          if (this.at(TT.BANG)) { this.advance(); noWarn = true; }
          let value = null;
          if (this.at(TT.COLON)) {
            this.advance();
            value = this.at(TT.AMP) ? this.parsePoolType() : this.parseExpr();
          }
          fields.push({ type: 'FieldDecl', sigil, name, value, noWarn, line: this.peek().line, col: this.peek().col });
        } else {
          fields.push(...[].concat(this.parseField()));
        }
      }
      this.expect(TT.NEWLINE);
      return fields;
    }
    throw new ParseError(`unexpected token starting a member: ${this.peek().type}`, this.peek());
  }

  // Disambiguate "&physics:" (a block) from a hypothetical bare "&Base" reference — in practice
  // '&' inside a member position always starts a scheduled block, since base-type binding only
  // happens right after the entity header.
  isBlockAhead() { return true; }

  // v0.8.7: parseField may return EITHER a single FieldDecl OR an array of FieldDecls (when the
  // multi-name shorthand `~a,b,c: value` is used). All callers normalize via `[].concat(...)`.
  // The multi-name form is detected by peeking after the first name: if `,` follows (before `:` or
  // `!`), we collect more names until `:`, then assign the same value to all of them. This saves
  // LLMs 2-3 tokens per group of related fields (e.g. `~x,y,z: 0` vs three separate `~x: 0`
  // lines). The `!` override marker (existing `~field!:`) is NOT supported in multi-name mode —
  // each field in a multi-name group is a fresh declaration, not an override, so the `!` wouldn't
  // apply uniformly. (If you need per-field overrides, write them on separate lines.)
  parseField() {
    const line = this.peek().line, col = this.peek().col;
    if (this.at(TT.TILDE)) {
      this.advance();
      const name = this.expect(TT.IDENT).value;
      // v0.8.7: multi-name detection — `~a,b,c: value`.
      if (this.at(TT.COMMA)) {
        const names = [name];
        while (this.at(TT.COMMA)) {
          this.advance();
          names.push(this.expect(TT.IDENT).value);
        }
        // Expect `: value` (required for multi-name — there's no useful "default null x3" case
        // since the LLM could just write `~a: 0, b: 0, c: 0` if they wanted different values).
        // Actually the spec says "If no value, init all to null/0." — so `: value` is optional.
        let value = null;
        if (this.at(TT.COLON)) {
          this.advance();
          value = this.at(TT.AMP) ? this.parsePoolType() : this.parseExpr();
        }
        // Emit one FieldDecl per name, all sharing the same value AST node. (Sharing the AST
        // node is fine — eval is read-only on the node.)
        return names.map(n => ({ type: 'FieldDecl', sigil: '~', name: n, value, noWarn: false, line, col }));
      }
      // v0.8.2: `~field!: value` — the `!` after the name marks this field as intentionally
      // overriding a mixin default, suppressing the AX-MIXIN-002 advisory. Useful when you
      // have several enemy archetypes that vary ~hp from a shared ^mix Stats: ~hp: 100.
      let noWarn = false;
      if (this.at(TT.BANG)) { this.advance(); noWarn = true; }
      let value = null;
      if (this.at(TT.COLON)) {
        this.advance();
        value = this.at(TT.AMP) ? this.parsePoolType() : this.parseExpr();
      }
      return { type: 'FieldDecl', sigil: '~', name, value, noWarn, line, col };
    }
    if (this.at(TT.DOLLAR)) {
      this.advance();
      const name = this.expect(TT.IDENT).value;
      this.expect(TT.COLON);
      const dist = this.parseDistExpr();
      let infer = null;
      if (this.at(TT.IDENT) && this.peek().value === 'infer') {
        this.advance(); this.expect(TT.COLON);
        infer = this.parseCallLike();
      }
      return { type: 'FieldDecl', sigil: '$', name, dist, infer, line, col };
    }
    throw new ParseError('expected a field', this.peek());
  }

  // v0.2 §2.1.1: "~bullets: &Pool(Bullet, 64)" — the one sanctioned dynamic-count container
  // usable inside a "&" block. Reuses the "&" glyph (engine-native, hard-real-time) rather than
  // introducing a new sigil for containers.
  parsePoolType() {
    this.expect(TT.AMP);
    const kw = this.expect(TT.IDENT).value;
    if (kw !== 'Pool' && kw !== 'Vec' && kw !== 'Map') {
      throw new ParseError(`'&${kw}' is not a recognized field-type constructor (only '&Pool', '&Vec', '&Map' are defined)`, this.peek());
    }
    this.expect(TT.LPAREN);
    if (kw === 'Map') {
      const keyType = this.expect(TT.IDENT).value;
      this.expect(TT.COMMA);
      const valType = this.expect(TT.IDENT).value;
      this.expect(TT.COMMA);
      const capacity = this.expect(TT.NUMBER).value;
      this.expect(TT.RPAREN);
      return { type: 'MapType', keyType, valType, capacity };
    }
    const elementType = this.expect(TT.IDENT).value;
    this.expect(TT.COMMA);
    const capacity = this.expect(TT.NUMBER).value;
    this.expect(TT.RPAREN);
    return { type: kw === 'Pool' ? 'PoolType' : 'VecType', elementType, capacity };
  }

  parseDistExpr() {
    const shape = this.parseCallLike();
    let prior = null;
    if (this.at(TT.TILDE)) { this.advance(); prior = this.parseCallLike(); }
    return { type: 'DistExpr', shape, prior };
  }

  // IDENT "(" args ")"?  — used for shape/prior/infer-strategy calls
  parseCallLike() {
    const name = this.expect(TT.IDENT).value;
    let args = [];
    if (this.at(TT.LPAREN)) { args = this.parseArgs(); }
    return { type: 'Call', callee: name, args };
  }

  parseBlock() {
    const line = this.peek().line, col = this.peek().col;
    this.expect(TT.AMP);
    const name = this.expect(TT.IDENT).value;
    let freq = null, freqUnit = null, eventArg = null;
    if (this.at(TT.LPAREN)) {
      this.advance();
      if (this.at(TT.NUMBER)) {
        const num = this.expect(TT.NUMBER);
        freq = num.value; freqUnit = num.unit;
      } else {
        // v0.2 §2.3.1: "&on(Damage):" — an event name instead of a frequency.
        eventArg = this.expect(TT.IDENT).value;
      }
      this.expect(TT.RPAREN);
    }
    this.expect(TT.COLON);
    // v0.8.1: single-line block — if the next token is NOT a NEWLINE, parse a single
    // statement on the same line. Mirrors the `?cond: stmt` form already supported by
    // parseCondBlock. This lets `&render: !mesh(#X, color: 0xffcc44)` instead of a
    // 2-line block + indent + dedent.
    if (this.at(TT.NEWLINE)) {
      this.advance();
      this.expect(TT.INDENT);
      const body = [];
      while (!this.at(TT.DEDENT)) {
        body.push(this.parseStmt());
        this.skipNewlines();
      }
      this.expect(TT.DEDENT);
      return { type: 'Block', name, freq, freqUnit, eventArg, body, line, col };
    }
    // v0.8.7: single-line block with `;`-separated statements — `&physics: stmt1; stmt2; stmt3`.
    // Parse one stmt, then while `;` follows, parse more stmts. The block body is the list.
    // This lets LLMs collapse multi-statement blocks onto one line: `&physics: cd = cd - dt; ?cd <= 0: cd = shoot_cd`
    //
    // Implementation: stmt parsers (parseAssignOrExprStmt, parseAction, etc.) end with
    // `this.expect(TT.NEWLINE)`. To make them accept `;` as a terminator, we MUTATE the token
    // stream in-place: every SEMICOLON between the current position and the next real NEWLINE
    // is replaced with a NEWLINE token (same line/col). This way, the stmt parsers see NEWLINE
    // and consume it happily; the block parser then checks if there are more stmts to parse
    // by looking at the next token (which is the start of the next stmt, or a real NEWLINE
    // if the block body is done).
    //
    // The mutation is safe because: (1) we only mutate forward from the current position,
    // never backward; (2) once mutated, the token is consumed by the stmt parser and never
    // re-examined; (3) the mutation persists, but that's fine — the `;` was only meaningful
    // as a separator within this block body, and after parsing, it's gone.
    //
    // IMPORTANT: this means once a one-liner block starts on the entity header line,
    // everything after it on the same physical line is part of the block body (not the entity
    // header). To add more entity header elements after a one-liner block, use a NEWLINE.
    // This is consistent with how `;` works in statement position generally.
    const mutStart = this.pos;
    let mutEnd = mutStart;
    while (mutEnd < this.toks.length &&
           this.toks[mutEnd].type !== TT.NEWLINE &&
           this.toks[mutEnd].type !== TT.EOF &&
           this.toks[mutEnd].type !== TT.DEDENT) {
      if (this.toks[mutEnd].type === TT.SEMICOLON) {
        this.toks[mutEnd] = { type: TT.NEWLINE, line: this.toks[mutEnd].line, col: this.toks[mutEnd].col };
      }
      mutEnd++;
    }
    const body = [this.parseStmt()];
    // parseStmt consumed the (possibly mutated) NEWLINE. Continue parsing stmts until we hit
    // a real NEWLINE / EOF / DEDENT (end of single-line block body) OR a token that starts a
    // new entity/block/decl (AMP, AT, CARET, HASH) — those belong to the enclosing scope, not
    // this block's body. Without the AMP/AT/CARET/HASH check, `&physics: x = 1\n  &render: ...`
    // would try to parse `&render:` as a second stmt of `&physics:`, which fails.
    // v0.8.7 fix: ALSO terminate on INDENT — INDENT means the next line is indented (part of
    // the entity body, not this block's single-line body). Without this, `&tick: stmt\n  ~x: 1`
    // would try to parse the INDENT+`~x: 1` as a second stmt of the block, which fails.
    // v0.8.9: added TT.TILDE and TT.DOLLAR — single-line block body should not consume
    // next-line field declarations starting with ~ or $.
    // v0.8.9: added TT.TILDE and TT.DOLLAR — single-line block body should not consume
    // next-line field declarations starting with ~ or $.
    // v0.8.10: QUESTION and STAR are NOT needed here — the NEWLINE before ?cond: or *loop:
    // on the next line already terminates this loop. Adding them breaks ; -separated multi-statement
    // single-line blocks like `&physics: a = 1; ?cond: b = 2` (the ? after ; gets wrongly stopped).
    while (!this.atAny(TT.NEWLINE, TT.EOF, TT.DEDENT, TT.AMP, TT.AT, TT.CARET, TT.HASH, TT.INDENT, TT.TILDE, TT.DOLLAR)) {
      body.push(this.parseStmt());
    }
    return { type: 'Block', name, freq, freqUnit, eventArg, body, line, col };
  }

  parseStmt() {
    // v0.4: !!expr — assert (double-bang)
    // v0.5.1 fix: lexer emits BANGBANG as a single token (lexer.js line 113), so handle both forms.
    if (this.at(TT.BANGBANG)) {
      const line = this.peek().line, col = this.peek().col;
      this.advance();
      const expr = this.parseExpr();
      this.expect(TT.NEWLINE);
      return { type: 'Assert', expr, line, col };
    }
    if (this.at(TT.BANG) && this.peek(1).type === TT.BANG) {
      const line = this.peek().line, col = this.peek().col;
      this.advance(); this.advance(); // consume !!
      const expr = this.parseExpr();
      this.expect(TT.NEWLINE);
      return { type: 'Assert', expr, line, col };
    }
    if (this.at(TT.BANG)) return this.parseAction();
    // v0.8.8: prefix increment/decrement — `++x` / `--x`. These are STATEMENT forms (not
    // expressions) — they mutate the target by ±1 and produce no value. Support prefix only
    // (`++x`, not `x++`) — prefix is sufficient for LLM use cases (counters, ammo) and simpler
    // to implement (no need to track "post" semantics where the old value is returned).
    // Target can be: plain IDENT (`++score`), member access (`++pose.pos.x`), or index
    // (`++arr[0]`). The interpreter handles each via the existing Assign/MemberAssign/IndexAssign
    // paths with compoundOp='+' or '-' and value=NumberLit(1).
    if (this.at(TT.PLUSPLUS) || this.at(TT.MINUSMINUS)) {
      const line = this.peek().line, col = this.peek().col;
      const op = this.at(TT.PLUSPLUS) ? '+' : '-';
      this.advance();  // consume ++/--
      // Parse the target. Reuse parseAssignOrExprStmt's target-parsing logic by dispatching to it
      // and intercepting the result. But that's complex — simpler to inline the three target forms.
      const nameTok = this.expect(TT.IDENT);
      // Member path: `++a.b.c`
      if (this.at(TT.DOT)) {
        const parts = [nameTok.value];
        while (this.at(TT.DOT)) {
          this.advance();
          parts.push(this.expect(TT.IDENT).value);
        }
        this.expect(TT.NEWLINE);
        const value = { type: 'NumberLit', value: 1 };
        if (parts.length > 2) {
          return { type: 'DeepAssign', path: parts, value, compoundOp: op, line, col };
        }
        return { type: 'MemberAssign', obj: parts[0], prop: parts[1], value, compoundOp: op, line, col };
      }
      // Index: `++arr[i]`
      if (this.at(TT.LBRACKET)) {
        this.advance();
        const indexExpr = this.parseExpr();
        this.expect(TT.RBRACKET);
        this.expect(TT.NEWLINE);
        const value = { type: 'NumberLit', value: 1 };
        return { type: 'IndexAssign', obj: nameTok.value, index: indexExpr, value, compoundOp: op, line, col };
      }
      // Plain variable: `++x`
      this.expect(TT.NEWLINE);
      const value = { type: 'NumberLit', value: 1 };
      return { type: 'Assign', sigil: null, target: nameTok.value, op: '=', value, compoundOp: op, line, col };
    }
    if (this.at(TT.CARET)) {
      if (this.peek(1).type === TT.IDENT && this.peek(1).value === 'emit') return this.parseEmit();
      // v0.4: ^return expr
      if (this.peek(1).type === TT.IDENT && this.peek(1).value === 'return') {
        const line = this.peek().line, col = this.peek().col;
        this.advance(); // ^
        this.advance(); // return
        // v0.8.9: void return — ^return without expression. For early-exit from procs.
        let expr = null;
        if (!this.at(TT.NEWLINE) && !this.at(TT.SEMICOLON)) {
          expr = this.parseExpr();
        }
        if (this.at(TT.SEMICOLON)) this.advance();
        else this.expect(TT.NEWLINE);
        return { type: 'Return', expr, line, col };
      }
      return this.parseBroadcast();
    }
    // v0.4: ?cond: / ?!: conditional block
    // v0.5.1 fix: lexer emits ?! as a single QMARKEQ token; surface it cleanly here.
    // v0.8.7: `if` is an alias for `?` at statement start. `elif`/`else` without a preceding
    // conditional are errors (same as `?!:` without `?cond:`).
    if (this.at(TT.QMARKEQ)) {
      throw new ParseError('?! (else) without preceding ?cond:', this.peek());
    }
    if (this.atKeyword('elif') || this.atKeyword('else')) {
      throw new ParseError(`${this.peek().value} without preceding if/?cond:`, this.peek());
    }
    if (this.at(TT.QUESTION)) {
      // v0.8.9: `?!#Tag:` is the exists-shorthand in conditional position. The lexer emits
      // `?!#` as QUESTION + BANG + HASH (not QMARKEQ) so we can detect it here.
      if (this.peek(1).type === TT.BANG && this.peek(2).type === TT.HASH) {
        return this.parseCondBlock();
      }
      if (this.peek(1).type === TT.BANG) {
        throw new ParseError('?! (else) without preceding ?cond:', this.peek());
      }
      return this.parseCondBlock();
    }
    if (this.atKeyword('if')) {
      return this.parseCondBlock();
    }
    // v0.4: *cond: (while) / *i in 0..n: (for)
    if (this.at(TT.STAR)) return this.parseLoop();
    // v0.4: ~break / ~continue
    if (this.at(TT.TILDE) && this.peek(1).type === TT.IDENT) {
      const v = this.peek(1).value;
      if (v === 'break' || v === 'continue') {
        const line = this.peek().line, col = this.peek().col;
        this.advance(); // ~
        this.advance(); // break/continue
        this.expect(TT.NEWLINE);
        return { type: v === 'break' ? 'Break' : 'Continue', line, col };
      }
    }
    // transition_chain starts with an lvalue identifier followed eventually by "->"
    if (this.at(TT.IDENT) && this.peek(1).type === TT.ARROW) return this.parseTransitionChain();
    if (this.at(TT.DOLLAR) || this.at(TT.IDENT)) return this.parseAssignOrExprStmt();
    // v0.8.2: #Tag.field = expr — cross-entity field write. The statement starts with HASH
    // (a TagRef), followed by .field paths, then = and an expression. This is the write
    // counterpart to reading #Tag.field in an expression. Semantics: the write executes in
    // the WRITER's tick/physics step and directly mutates the target entity's field. There's
    // no ownership tracking — if two entities write the same target in the same step, the
    // last writer wins (matching the implicit ordering of &physics blocks by entity index).
    if (this.at(TT.HASH)) return this.parseCrossEntityAssign();
    // v0.4: ~ at statement level that isn't break/continue — could be a ~field assignment
    if (this.at(TT.TILDE)) {
      return this.parseAssignOrExprStmt();
    }
    throw new ParseError(`unexpected statement token ${this.peek().type}`, this.peek());
  }

  // v0.8.2: parse `#Tag.field = expr` (or `#Tag.field.subfield = expr` for deep member assign).
  // The TagRef resolves to an entity at runtime; the field path is applied to that entity.
  // This is a statement (not an expression) — it produces no value.
  parseCrossEntityAssign() {
    const line = this.peek().line, col = this.peek().col;
    this.expect(TT.HASH);
    const tagParts = [this.expect(TT.IDENT).value];
    // Collect .field.path — the target field on the tagged entity.
    const fieldPath = [];
    while (this.at(TT.DOT)) {
      this.advance();
      fieldPath.push(this.expect(TT.IDENT).value);
    }
    if (fieldPath.length === 0) {
      throw new ParseError(`expected .field after #${tagParts[0]} in assignment statement`, this.peek());
    }
    // v0.8.12: support compound assignment operators on cross-entity writes
    // v0.8.17: also support ??= (NULLCOALEQ) — compoundOp '??' triggers null-coalescing assign.
    if (!this.atAny(TT.ASSIGN, TT.PLUSEQ, TT.MINUSEQ, TT.STAREQ, TT.SLASHEQ, TT.PERCEQ, TT.NULLCOALEQ)) {
      throw new ParseError(`expected = after #${tagParts[0]}.${fieldPath.join('.')} (cross-entity writes must assign)`, this.peek());
    }
    const opTok = this.advance().type;
    const compoundOp = opTok === TT.ASSIGN ? null
                     : opTok === TT.PLUSEQ ? '+'
                     : opTok === TT.MINUSEQ ? '-'
                     : opTok === TT.STAREQ ? '*'
                     : opTok === TT.SLASHEQ ? '/'
                     : opTok === TT.PERCEQ ? '%'
                     : opTok === TT.NULLCOALEQ ? '??' : null;
    const value = this.parseExpr();
    this.expect(TT.NEWLINE);
    // Reuse DeepAssign with a `tag` field so execStmt can dispatch to cross-entity write logic.
    if (fieldPath.length > 1) {
      return { type: 'DeepAssign', tag: tagParts[0], path: fieldPath, value, compoundOp, line, col };
    }
    return { type: 'MemberAssign', tag: tagParts[0], obj: null, prop: fieldPath[0], value, compoundOp, line, col };
  }

  // v0.4: ?cond: body [?!: else_body]
  // v0.5: also supports ?cond: guard NEWLINE INDENT body (guard is ANDed with cond)
  //         and single-line ?cond: stmt (no indent block)
  // v0.8.7: helper — is the current token an IDENT with the given keyword value?
  // Used to recognize `if`/`elif`/`else`/`for`/`in` as contextual keywords (they're still
  // legal field/variable names in other positions — only statement-start makes them keywords).
  atKeyword(kw) { return this.at(TT.IDENT) && this.peek().value === kw; }

  parseCondBlock() {
    const line = this.peek().line, col = this.peek().col;
    // v0.8.7: accept both `?` and `if` as the conditional starter. Both produce the same
    // CondBlock AST — they're syntactic alternatives for the same semantics. LLMs can use
    // whichever they emit correctly; the goal is correct output, not syntax purity.
    // `elif` is also accepted here as a starter — when parseCondBlock is called recursively
    // from the elif handler below, the current token is `elif` (not `?` or `if`). Treating
    // `elif` as `if` here lets the recursive call work without duplicating the cond/body/else
    // parsing logic. The AST is identical — `elif cond: body` is sugar for `else: if cond: body`.
    let cond;
    if (this.at(TT.QUESTION)) {
      this.advance();
      // v0.8.9: `?!#Tag:` — exists-shorthand in conditional position.
      if (this.at(TT.BANG) && this.peek(1).type === TT.HASH) {
        this.advance(); // consume !
        this.advance(); // consume #
        const parts = [this.expect(TT.IDENT).value];
        while (this.at(TT.DOT)) { this.advance(); parts.push(this.expect(TT.IDENT).value); }
        cond = { type: 'Query', obj: null, name: 'exists', args: [{ value: { type: 'TagRef', path: parts } }] };
        this.expect(TT.COLON);
        // Skip the normal cond parse — go directly to body parsing
        let guard = null;
        let ifBody;
        if (this.at(TT.NEWLINE)) {
          this.advance(); this.expect(TT.INDENT); ifBody = [];
          while (!this.at(TT.DEDENT)) { ifBody.push(this.parseStmt()); this.skipNewlines(); }
          this.expect(TT.DEDENT);
        } else {
          // Single-line or guard+block
          let ifBody_stmt = null;
          if (this.at(TT.BANG) || this.at(TT.CARET) || this.at(TT.PLUSPLUS) || this.at(TT.MINUSMINUS) || (this.at(TT.TILDE) && (this.peek(1).type === TT.IDENT && (this.peek(1).value === 'break' || this.peek(1).value === 'continue')))) {
            ifBody_stmt = this.parseStmt();
          }
          if (ifBody_stmt) {
            // v0.8.13 fix: parseStmt() already consumed the trailing NEWLINE. Check INDENT directly.
            if (this.at(TT.INDENT)) { this.expect(TT.INDENT); guard = null; ifBody = [ifBody_stmt]; while (!this.at(TT.DEDENT)) { ifBody.push(this.parseStmt()); this.skipNewlines(); } this.expect(TT.DEDENT); }
            else { ifBody = [ifBody_stmt]; }
          } else {
            const exprStartPos = this.pos;
            const expr = this.parseExpr();
            if (this.at(TT.NEWLINE)) { this.advance(); if (this.at(TT.INDENT)) { guard = expr; this.expect(TT.INDENT); ifBody = []; while (!this.at(TT.DEDENT)) { ifBody.push(this.parseStmt()); this.skipNewlines(); } this.expect(TT.DEDENT); } else { ifBody = [{ type: 'ExprStmt', expr, line, col }]; } }
            else { this.pos = exprStartPos; ifBody = [this.parseStmt()]; }
          }
        }
        // Parse else clause — v0.8.11: single-line support (same pattern as normal cond path)
        let elseBody = null;
        if (this.at(TT.QMARKEQ)) { this.advance(); this.expect(TT.COLON); if (this.at(TT.NEWLINE)) { this.advance(); this.expect(TT.INDENT); elseBody = []; while (!this.at(TT.DEDENT)) { elseBody.push(this.parseStmt()); this.skipNewlines(); } this.expect(TT.DEDENT); } else { elseBody = [this.parseStmt()]; } }
        else if (this.at(TT.QUESTION) && this.peek(1).type === TT.BANG) { this.advance(); this.advance(); this.expect(TT.COLON); if (this.at(TT.NEWLINE)) { this.advance(); this.expect(TT.INDENT); elseBody = []; while (!this.at(TT.DEDENT)) { elseBody.push(this.parseStmt()); this.skipNewlines(); } this.expect(TT.DEDENT); } else { elseBody = [this.parseStmt()]; } }
        else if (this.atKeyword('else')) { this.advance(); this.expect(TT.COLON); if (this.at(TT.NEWLINE)) { this.advance(); this.expect(TT.INDENT); elseBody = []; while (!this.at(TT.DEDENT)) { elseBody.push(this.parseStmt()); this.skipNewlines(); } this.expect(TT.DEDENT); } else { elseBody = [this.parseStmt()]; } }
        else if (this.atKeyword('elif')) { elseBody = [this.parseCondBlock()]; }
        return { type: 'CondBlock', cond, guard, ifBody, elseBody, line, col };
      }
    } else if (this.atKeyword('if') || this.atKeyword('elif')) {
      this.advance();
    } else {
      throw new ParseError(`expected '?' or 'if' to start a conditional`, this.peek());
    }
    // Use parseNullCoalesce (v0.8.9: ?? binds tighter than ternary, same level as infer here)
    // v0.8.10 fix: use parseOr (above &&/||/?? in precedence) so conditions can use logical ops.
    // Was parseInfer() which sits below &&/||/?? — `?x && y:` crashed with "expected COLON, got ANDAND".
    cond = this.parseOr();
    this.expect(TT.COLON);
    let guard = null;
    let ifBody;

    if (this.at(TT.NEWLINE)) {
      // Standard block: ?cond:\n  body
      this.advance();
      this.expect(TT.INDENT);
      ifBody = [];
      while (!this.at(TT.DEDENT)) { ifBody.push(this.parseStmt()); this.skipNewlines(); }
      this.expect(TT.DEDENT);
    } else {
      // v0.5: single-line or guard+block
      // v0.8.2: if the body starts with `!` (an action) or `^` (a broadcast/emit) or `~` (break/
      // continue), parse it as a STATEMENT, not an expression. Without this, `?cond: !mesh(...)`
      // would parse `mesh(...)` as a function-call expression and throw at runtime
      // (AX-RUNTIME-FUNC: unknown function 'mesh'). Single-line `?cond: !action(...)` now works
      // the same way single-line `&block: !action(...)` does.
      let ifBody_stmt = null;
      // v0.8.9: added PLUSPLUS and MINUSMINUS — `?cond: ++score` should parse as a statement.
      if (this.at(TT.BANG) || this.at(TT.CARET) || this.at(TT.PLUSPLUS) || this.at(TT.MINUSMINUS) || (this.at(TT.TILDE) && (this.peek(1).type === TT.IDENT && (this.peek(1).value === 'break' || this.peek(1).value === 'continue')))) {
        ifBody_stmt = this.parseStmt();
      }
      if (ifBody_stmt) {
        // We parsed a single statement. parseStmt() consumed the trailing NEWLINE.
        // If the next token is INDENT, it's guard+block syntax.
        if (this.at(TT.INDENT)) {
          this.expect(TT.INDENT);
          guard = null;
          ifBody = [ifBody_stmt];
          while (!this.at(TT.DEDENT)) { ifBody.push(this.parseStmt()); this.skipNewlines(); }
          this.expect(TT.DEDENT);
        } else {
          ifBody = [ifBody_stmt];
        }
      } else {
        const exprStartPos = this.pos;
        const expr = this.parseExpr();
        if (this.at(TT.NEWLINE)) {
          this.advance();
          if (this.at(TT.INDENT)) {
            // Guard syntax: ?cond: guard\n  body
            guard = expr;
            this.expect(TT.INDENT);
            ifBody = [];
            while (!this.at(TT.DEDENT)) { ifBody.push(this.parseStmt()); this.skipNewlines(); }
            this.expect(TT.DEDENT);
          } else {
            // Single-line conditional: ?cond: expr\n
            ifBody = [{ type: 'ExprStmt', expr, line, col }];
          }
        } else {
          // v0.8.9 fix: backtrack to BEFORE parseExpr (not just 1 token) so the full
          // expression (e.g. `pose.pos.x`) is re-parsed as part of a statement.
          this.pos = exprStartPos;
          ifBody = [this.parseStmt()];
        }
      }
    }
    let elseBody = null;
    // v0.8.7: else clause accepts three forms:
    //   - `?!:` (existing sigil form)
    //   - `else:` (keyword alias)
    //   - `elif cond:` (chained else-if — desugars to elseBody = [nested CondBlock])
    // All three produce the same CondBlock AST. `elif cond: body` is sugar for
    // `?!: ?cond: body` — the else body contains a nested conditional.
    if (this.at(TT.QMARKEQ)) {
      this.advance();
      this.expect(TT.COLON);
      if (this.at(TT.NEWLINE)) {
        // Multi-line else (existing)
        this.advance(); this.expect(TT.INDENT); elseBody = [];
        while (!this.at(TT.DEDENT)) { elseBody.push(this.parseStmt()); this.skipNewlines(); }
        this.expect(TT.DEDENT);
      } else {
        // v0.8.9: single-line else — ?!: stmt
        elseBody = [this.parseStmt()];
      }
    } else if (this.at(TT.QUESTION) && this.peek(1).type === TT.BANG) {
      this.advance(); // ?
      this.advance(); // !
      this.expect(TT.COLON);
      if (this.at(TT.NEWLINE)) {
        this.advance(); this.expect(TT.INDENT); elseBody = [];
        while (!this.at(TT.DEDENT)) { elseBody.push(this.parseStmt()); this.skipNewlines(); }
        this.expect(TT.DEDENT);
      } else {
        elseBody = [this.parseStmt()];
      }
    } else if (this.atKeyword('else')) {
      this.advance();
      this.expect(TT.COLON);
      if (this.at(TT.NEWLINE)) {
        this.advance(); this.expect(TT.INDENT); elseBody = [];
        while (!this.at(TT.DEDENT)) { elseBody.push(this.parseStmt()); this.skipNewlines(); }
        this.expect(TT.DEDENT);
      } else {
        elseBody = [this.parseStmt()];
      }
    } else if (this.atKeyword('elif')) {
      // v0.8.7: `elif cond: body` — parse a nested CondBlock as the else body.
      // The nested CondBlock handles its own `elif`/`else` chain recursively.
      elseBody = [this.parseCondBlock()];
    }
    return { type: 'CondBlock', cond, guard, ifBody, elseBody, line, col };
  }

  // v0.4: *cond: (while) or *i in 0..n: (for-range) or *item in pool: (for-each)
  parseLoop() {
    const line = this.peek().line, col = this.peek().col;
    this.expect(TT.STAR); // consume *
    // Disambiguate: *IDENT in expr (for) vs *expr (while)
    if (this.at(TT.IDENT) && this.peek(1).type === TT.IDENT && this.peek(1).value === 'in') {
      const varName = this.advance().value; // loop variable
      this.advance(); // consume 'in'
      const iterable = this.parseExpr();
      this.expect(TT.COLON);
      let body;
      if (this.at(TT.NEWLINE)) {
        // Multi-line body (existing)
        this.advance(); this.expect(TT.INDENT); body = [];
        while (!this.at(TT.DEDENT)) { body.push(this.parseStmt()); this.skipNewlines(); }
        this.expect(TT.DEDENT);
      } else {
        // v0.8.9: single-line loop body
        body = [this.parseStmt()];
      }
      return { type: 'ForLoop', varName, iterable, body, line, col };
    }
    // While loop
    const cond = this.parseExpr();
    this.expect(TT.COLON);
    let body;
    if (this.at(TT.NEWLINE)) {
      this.advance(); this.expect(TT.INDENT); body = [];
      while (!this.at(TT.DEDENT)) { body.push(this.parseStmt()); this.skipNewlines(); }
      this.expect(TT.DEDENT);
    } else {
      // v0.8.9: single-line while loop body
      body = [this.parseStmt()];
    }
    return { type: 'WhileLoop', cond, body, line, col };
  }

  // Disambiguates "target = expr" / "target ~= expr" from a bare expression used as a statement
  // (e.g. "targets.push(newTarget)" -- §2.1.1's own illustrative rejected-construct example is
  // written exactly this way, with no assignment, so the grammar needs to admit it as parseable
  // even though it's expected to be a compile-time contract violation). Only a bare, un-suffixed
  // IDENT can be an assignment target; anything else backtracks and parses as an ExprStmt.
  parseAssignOrExprStmt() {
    const line = this.peek().line, col = this.peek().col;
    const startPos = this.pos;
    let sigil = null;
    if (this.at(TT.DOLLAR)) { sigil = '$'; this.advance(); }
    // v0.8.10: ~field += expr — compound reassignment to a ~ field inside a block body.
    // Also handles -=, *=, /=, %=. v0.8.17: also handles ??= (compoundOp '??').
    if (this.at(TT.TILDE) && this.peek(1).type === TT.IDENT &&
        (this.peek(2).type === TT.COLON ||
         [TT.PLUSEQ, TT.MINUSEQ, TT.STAREQ, TT.SLASHEQ, TT.PERCEQ, TT.NULLCOALEQ].includes(this.peek(2).type))) {
      this.advance(); // ~
      const nameTok = this.expect(TT.IDENT);
      const opTok = this.advance().type;
      if (opTok === TT.COLON) {
        // Regular ~field: expr (existing)
        const value = this.parseExpr();
        this.expect(TT.NEWLINE);
        return { type: 'Assign', sigil: '~', target: nameTok.value, op: '=', value, line, col };
      }
      // Compound: ~field += expr (or -=, *=, /=, %=, ??=)
      const compoundOp = opTok === TT.PLUSEQ ? '+' : opTok === TT.MINUSEQ ? '-' : opTok === TT.STAREQ ? '*' : opTok === TT.SLASHEQ ? '/' : opTok === TT.PERCEQ ? '%' : opTok === TT.NULLCOALEQ ? '??' : null;
      const value = this.parseExpr();
      this.expect(TT.NEWLINE);
      return { type: 'Assign', sigil: '~', target: nameTok.value, op: '=', value, compoundOp, line, col };
    }
    const nameTok = this.at(TT.IDENT) ? this.expect(TT.IDENT) : null;
    // v0.4 fix: support obj.field = expr member assignment
    if (nameTok && this.at(TT.DOT) && this.peek(1).type === TT.IDENT) {
      // v0.5: support deep member assignment a.b.c = expr
      const parts = [nameTok.value];
      while (this.at(TT.DOT)) {
        this.advance();
        parts.push(this.expect(TT.IDENT).value);
      }
      // v0.8.8: compound assignment on member path — `obj.field += val` etc.
      // Desugars to `obj.field = obj.field OP val` (same as plain compound assign).
      // v0.8.17: also supports ??= (compoundOp '??') for null-coalescing member assign.
      if (this.atAny(TT.ASSIGN, TT.PLUSEQ, TT.MINUSEQ, TT.STAREQ, TT.SLASHEQ, TT.PERCEQ, TT.NULLCOALEQ)) {
        const opTok = this.advance().type;
        const value = this.parseExpr();
        this.expect(TT.NEWLINE);
        // For `=` (plain assign), no compoundOp — just write the value.
        // For `+=` etc., set compoundOp so the interpreter computes `current OP value`.
        // For `??=`, compoundOp '??' triggers null-coalescing (write only if current is null/undefined).
        const compoundOp = opTok === TT.ASSIGN ? null
                         : opTok === TT.PLUSEQ ? '+'
                         : opTok === TT.MINUSEQ ? '-'
                         : opTok === TT.STAREQ ? '*'
                         : opTok === TT.SLASHEQ ? '/'
                         : opTok === TT.PERCEQ ? '%'
                         : opTok === TT.NULLCOALEQ ? '??' : null;
        if (parts.length > 2) {
          return { type: 'DeepAssign', path: parts, value, compoundOp, sigil, line, col };
        }
        return { type: 'MemberAssign', obj: parts[0], prop: parts[1], value, compoundOp, sigil, line, col };
      }
      // Not an assignment — backtrack and parse as expression
      this.pos = startPos;
      const expr = this.parseExpr();
      this.expect(TT.NEWLINE);
      return { type: 'ExprStmt', expr, line, col };
    }
    // v0.8.8: array element compound assign — `arr[i] += val`. Parse the index, then check
    // for compound assign. Falls through to expression reparsing if not an assignment.
    // v0.8.17: also supports `arr[i] ??= val`.
    if (nameTok && this.at(TT.LBRACKET)) {
      // Backtrack-safe: snapshot pos, parse `arr[idx]`, check for assign.
      const savePos = this.pos;
      this.advance(); // consume [
      const indexExpr = this.parseExpr();
      if (this.at(TT.RBRACKET)) {
        this.advance();
        if (this.atAny(TT.ASSIGN, TT.PLUSEQ, TT.MINUSEQ, TT.STAREQ, TT.SLASHEQ, TT.PERCEQ, TT.NULLCOALEQ)) {
          const opTok = this.advance().type;
          const value = this.parseExpr();
          this.expect(TT.NEWLINE);
          const compoundOp = opTok === TT.ASSIGN ? null
                           : opTok === TT.PLUSEQ ? '+'
                           : opTok === TT.MINUSEQ ? '-'
                           : opTok === TT.STAREQ ? '*'
                           : opTok === TT.SLASHEQ ? '/'
                           : opTok === TT.PERCEQ ? '%'
                           : opTok === TT.NULLCOALEQ ? '??' : null;
          return { type: 'IndexAssign', obj: nameTok.value, index: indexExpr, value, compoundOp, line, col };
        }
      }
      // Not an index assign — backtrack and reparse as expression.
      this.pos = savePos;
    }
    if (nameTok && (this.at(TT.ASSIGN) || this.at(TT.TILDEEQ) || this.atAny(TT.PLUSEQ, TT.MINUSEQ, TT.STAREQ, TT.SLASHEQ, TT.PERCEQ, TT.NULLCOALEQ))) {
      // v0.8.8: compound assignment on plain variable — `x += val` desugars to `x = x OP val`.
      // The interpreter reads the current value, applies the op, and writes back.
      // v0.8.17: also handles `x ??= val` (compoundOp '??' — null-coalescing assign).
      let op, compoundOp = null;
      if (this.at(TT.ASSIGN)) op = '=';
      else if (this.at(TT.TILDEEQ)) op = '~=';
      else {
        op = '=';
        const t = this.peek().type;
        compoundOp = t === TT.PLUSEQ ? '+' : t === TT.MINUSEQ ? '-' : t === TT.STAREQ ? '*' : t === TT.SLASHEQ ? '/' : t === TT.PERCEQ ? '%' : t === TT.NULLCOALEQ ? '??' : null;
      }
      this.advance();
      const value = this.parseExpr();
      this.expect(TT.NEWLINE);
      return { type: 'Assign', sigil, target: nameTok.value, op, value, compoundOp, line, col };
    }
    this.pos = startPos; // not an assignment -- reparse the same tokens as a general expression
    const expr = this.parseExpr();
    this.expect(TT.NEWLINE);
    return { type: 'ExprStmt', expr, line, col };
  }

  parseAction() {
    const line = this.peek().line, col = this.peek().col;
    this.expect(TT.BANG);
    const name = this.expect(TT.IDENT).value;
    const args = this.at(TT.LPAREN) ? this.parseArgs() : [];
    this.expect(TT.NEWLINE);
    return { type: 'Action', name, args, line, col };
  }

  parseEmit() {
    const line = this.peek().line, col = this.peek().col;
    this.expect(TT.CARET);
    this.expect(TT.IDENT); // 'emit' keyword
    const pathParts = [this.expect(TT.IDENT).value];
    while (this.at(TT.DOT)) { this.advance(); pathParts.push(this.expect(TT.IDENT).value); }
    const args = this.parseArgs();
    this.expect(TT.NEWLINE);
    return { type: 'Emit', path: pathParts, args, line, col };
  }

  // v0.2 §2.3.1: "^Damage(amount: 15) within(4)" / "^Damage(amount: 15) to #Player" — a typed,
  // discrete, one-shot event broadcast, addressed globally (default), to a tagged entity, or to
  // everything within a radius of the emitter (or an explicit origin).
  parseBroadcast() {
    const line = this.peek().line, col = this.peek().col;
    this.expect(TT.CARET);
    const name = this.expect(TT.IDENT).value;
    const args = this.parseArgs();
    let addressing = { mode: 'global' };
    if (this.at(TT.IDENT) && this.peek().value === 'to') {
      this.advance();
      // v0.8.2: accept both `to #Target` (bare) and `to(#Target)` (parenthesized) forms.
      // The bare form is the original v0.2 syntax; the parenthesized form is a natural
      // analogy with `within(radius)` that users guess by symmetry. Both resolve identically.
      let hasParen = false;
      if (this.at(TT.LPAREN)) { this.advance(); hasParen = true; }
      this.expect(TT.HASH);
      const target = this.expect(TT.IDENT).value;
      if (hasParen) this.expect(TT.RPAREN);
      addressing = { mode: 'to', target };
    } else if (this.at(TT.IDENT) && this.peek().value === 'within') {
      this.advance();
      this.expect(TT.LPAREN);
      const radius = this.parseExpr();
      let origin = null;
      if (this.at(TT.COMMA)) {
        this.advance();
        this.expect(TT.IDENT); // 'origin' keyword
        this.expect(TT.COLON);
        origin = this.parseExpr();
      }
      this.expect(TT.RPAREN);
      addressing = { mode: 'within', radius, origin };
    }
    this.expect(TT.NEWLINE);
    return { type: 'Broadcast', name, args, addressing, line, col };
  }

  parseTransitionChain() {
    const line = this.peek().line, col = this.peek().col;
    const subject = this.expect(TT.IDENT).value;
    const clauses = [];
    while (this.at(TT.ARROW)) {
      this.advance();
      const targetTok = this.expect(TT.IDENT);
      const target = targetTok.value;
      let args = [];
      if (this.at(TT.LPAREN)) args = this.parseArgs();
      let guard = null;
      if (this.at(TT.IDENT) && this.peek().value === 'if') {
        this.advance();
        guard = this.parseExpr();
      }
      clauses.push({ target, args, guard });
    }
    this.expect(TT.NEWLINE);
    return { type: 'TransitionChain', subject, clauses, line, col };
  }

  parseArgs() {
    this.expect(TT.LPAREN);
    const args = [];
    if (!this.at(TT.RPAREN)) {
      args.push(this.parseArg());
      while (this.at(TT.COMMA)) { this.advance(); if (this.at(TT.RPAREN)) break; args.push(this.parseArg()); } // v0.8.12: trailing comma support
    }
    this.expect(TT.RPAREN);
    return args;
  }

  parseArg() {
    // named arg: IDENT ':' expr   (used for observe(noise: audio.ambient, ...))
    if (this.at(TT.IDENT) && this.peek(1).type === TT.COLON) {
      const name = this.advance().value;
      this.advance(); // colon
      const value = this.parseExpr();
      return { name, value };
    }
    // bare predicate arg: ('>' | '<' | '>=' | '<=' | '==') expr   (used for belief.any(>0.3))
    if (this.atAny(TT.GT, TT.LT, TT.GE, TT.LE, TT.EQEQ, TT.NE)) {
      const opTok = this.advance();
      const opMap = { GT: '>', LT: '<', GE: '>=', LE: '<=', EQEQ: '==', NE: '!=' };
      const value = this.parseExpr();
      return { predicateOp: opMap[opTok.type], value };
    }
    return { value: this.parseExpr() };
  }

  // ---------- expressions ----------
  // v0.8.9: precedence is now: ternary (lowest) -> nullCoalesce -> infer -> ...
  // ?? binds TIGHTER than ternary (matching C#/JS/TS/Swift), so `x ?? 0 ? a : b`
  // parses as `(x ?? 0) ? a : b`, not `x ?? (0 ? a : b)`.
  parseExpr() { return this.parseTernary(); }

  // v0.8.8: null-coalescing `a ?? b` — returns `a` if not null/undefined, else `b`.
  // v0.8.9: moved BELOW ternary (tighter) — `a ?? b ?? c` chains left-associatively.
  parseNullCoalesce() {
    let left = this.parseOr();
    while (this.at(TT.NULLCOAL)) {
      this.advance();
      const right = this.parseOr();
      left = { type: 'NullCoalesce', left, right };
    }
    return left;
  }

  // v0.8.9: logical OR — `a || b`. Short-circuits: if `a` is truthy, returns `a` without
  // evaluating `b`. Sits below ?? and above && in precedence (matching JS/C#).
  parseOr() {
    let left = this.parseAnd();
    while (this.at(TT.OROR)) {
      this.advance();
      const right = this.parseAnd();
      left = { type: 'Binary', op: '||', left, right };
    }
    return left;
  }

  // v0.8.9: logical AND — `a && b`. Short-circuits: if `a` is falsy, returns `a` without
  // evaluating `b`. Sits above || and below ?? in precedence.
  parseAnd() {
    let left = this.parseInfer();
    while (this.at(TT.ANDAND)) {
      this.advance();
      const right = this.parseInfer();
      left = { type: 'Binary', op: '&&', left, right };
    }
    return left;
  }

  parseTernary() {
    // v0.8.9: cond is parseNullCoalesce (?? binds tighter than ternary)
    const cond = this.parseNullCoalesce();
    if (this.at(TT.QUESTION)) {
      this.advance();
      const branch = this.parseNullCoalesce();
      if (this.at(TT.COLON)) {
        this.advance();
        const elze = this.parseNullCoalesce();
        return { type: 'Ternary', cond, then: branch, else: elze };
      }
      // v0.8.3: `obj ?name(args)` query form. The branch can be a Call (traditional) or a
      // Query (v0.8.3 — when `name` is a known query name, parsePostfix now produces a Query
      // node instead of a Call). In the Query case, attach the receiver (`cond`) as `obj`.
      if (branch.type === 'Call') {
        return { type: 'Query', obj: cond, name: branch.callee, args: branch.args };
      }
      if (branch.type === 'Query' && branch.obj === null) {
        return { type: 'Query', obj: cond, name: branch.name, args: branch.args };
      }
      throw new ParseError(`expected ':' to complete a ternary, or a bare 'name(args)' query after '?'`, this.peek());
    }
    return cond;
  }

  // "~>" (Infer op, §1.2/§2.2): draws/derives a concrete value from a distribution, e.g.
  // "$belief ~> argmax" or "$belief ~> sample". Sits on the RHS of an ordinary "=" assignment,
  // so it needs to live in the expression grammar rather than only at the statement level
  // (unlike "~=", which the spec only ever shows as a statement-level assignment operator).
  parseInfer() {
    let left = this.parseComparison();
    if (this.at(TT.TILDEGT)) {
      this.advance();
      const op = this.expect(TT.IDENT).value;
      left = { type: 'InferExpr', dist: left, op };
    }
    return left;
  }

  parseComparison() {
    let left = this.parseAdditive();
    while (this.atAny(TT.GT, TT.LT, TT.GE, TT.LE, TT.EQEQ, TT.NE, TT.QMARKGT)) {
      const opTok = this.advance();
      const opMap = { GT: '>', LT: '<', GE: '>=', LE: '<=', EQEQ: '==', NE: '!=', QMARKGT: '?>' };
      const right = this.parseAdditive();
      left = { type: 'Binary', op: opMap[opTok.type], left, right };
    }
    return left;
  }

  parseAdditive() {
    let left = this.parseMultiplicative();
    while (this.atAny(TT.PLUS, TT.MINUS, TT.DOTDOT)) {
      const t = this.advance().type;
      const op = t === TT.PLUS ? '+' : t === TT.MINUS ? '-' : '..';
      const right = this.parseMultiplicative();
      left = { type: 'Binary', op, left, right };
    }
    return left;
  }

  parseMultiplicative() {
    // v0.8.11: ** (exponentiation) sits between multiplicative and unary, right-associative.
    let left = this.parseExponent();
    while (this.atAny(TT.STAR, TT.SLASH, TT.PERCENT, TT.MIDDOT, TT.CROSS, TT.COMPOSE)) {
      const t = this.advance().type;
      const op = t === TT.STAR ? '*' : t === TT.SLASH ? '/' : t === TT.PERCENT ? '%' : t === TT.MIDDOT ? '\u00B7' : t === TT.CROSS ? '\u00D7' : '\u2218';
      const right = this.parseExponent();
      left = { type: 'Binary', op, left, right };
    }
    return left;
  }

  // v0.8.11: exponentiation — `a ** b`, right-associative (a ** b ** c = a ** (b ** c)).
  parseExponent() {
    let left = this.parseUnary();
    if (this.at(TT.STARSTAR)) {
      this.advance();
      const right = this.parseExponent(); // right-recursive
      return { type: 'Binary', op: '**', left, right };
    }
    return left;
  }

  parseUnary() {
    // v0.8.9: handle !! (double-bang / assert) in expression position. The lexer emits !! as
    // a single BANGBANG token. `!!expr` in expression position is a double-negation: !(!expr).
    if (this.at(TT.BANGBANG)) {
      this.advance();
      return { type: 'Unary', op: '!', expr: { type: 'Unary', op: '!', expr: this.parseUnary() } };
    }
    if (this.at(TT.BANG)) { this.advance(); return { type: 'Unary', op: '!', expr: this.parseUnary() }; }
    if (this.at(TT.MINUS)) { this.advance(); return { type: 'Unary', op: '-', expr: this.parseUnary() }; }
    return this.parsePostfix();
  }

  parsePostfix() {
    let expr = this.parsePrimary();
    for (;;) {
      if (this.at(TT.DOT)) {
        this.advance();
        const prop = this.expect(TT.IDENT).value;
        if (this.at(TT.LPAREN)) {
          const args = this.parseArgs();
          expr = { type: 'MethodCall', obj: expr, method: prop, args };
        } else {
          expr = { type: 'Member', obj: expr, prop };
        }
      } else if (this.at(TT.LBRACKET)) {
        this.advance();
        const index = this.parseExpr();
        this.expect(TT.RBRACKET);
        expr = { type: 'Index', obj: expr, index };
      } else if (this.at(TT.LPAREN) && expr.type === 'Ident') {
        const args = this.parseArgs();
        // v0.8.3: if the callee is a known query name, produce a Query node (receiverless)
        // instead of a Call. This fixes the inconsistency where `?nearest(#X)` worked in
        // assignment position (`x = ?nearest(...)`) but failed in conditional/ternary position
        // (`?nearest(...) != null:`) — in the latter, the outer `?` is the conditional sigil
        // and `nearest(...)` was parsed as a function Call, failing at runtime with
        // AX-RUNTIME-FUNC. Now `nearest(...)`, `path(...)`, `raycast(...)`, etc. are
        // recognized as queries regardless of syntactic position.
        if (KNOWN_QUERY_NAMES.has(expr.name)) {
          expr = { type: 'Query', obj: null, name: expr.name, args };
        } else {
          expr = { type: 'Call', callee: expr.name, args };
        }
      } else {
        break;
      }
    }
    return expr;
  }

  // v0.8.7: parse an FSTRING token into an FString AST node. The raw payload is split on `{` `}`,
  // with `{{` → literal `{` and `}}` → literal `}`. Each `{expr}` placeholder is recursively
  // tokenized + parsed via a fresh Parser instance (so the expression grammar is fully available
  // inside placeholders — method calls, ternaries, arithmetic, nested f-strings, etc.).
  //
  // The result is an FString node with a `parts` array of {kind: 'lit', text} | {kind: 'expr', node}
  // entries. At eval time, each part is rendered to a string (literals verbatim, expressions via
  // String(value)) and concatenated. This is the token-efficient equivalent of building a chain
  // of `+` concatenations — `f"x={x}"` is 1 fewer token than `"x=" + x` and 2 fewer than
  // `"x=" + String(x)`.
  //
  // Brace nesting: `{a + {b: 1}.b}` would be ambiguous (the inner `{` could start a dict literal
  // or a nested placeholder). We resolve this by counting brace depth — the placeholder ends at
  // the first `}` that brings depth to 0. To include a literal `}` inside an expression, escape
  // it as `}}` (rare; usually you'd use a string literal `'.'` inside the placeholder instead).
  parseFString(tok) {
    const raw = tok.value;
    const parts = [];
    let i = 0;
    const n = raw.length;
    let buf = '';
    while (i < n) {
      const c = raw[i];
      if (c === '{' && raw[i + 1] === '{') { buf += '{'; i += 2; continue; }
      if (c === '}' && raw[i + 1] === '}') { buf += '}'; i += 2; continue; }
      // v0.8.8: `\{` and `\}` escape sequences (lexer keeps them verbatim in the payload so we
      // can distinguish them from placeholder starts). These are alternatives to `{{`/`}}` —
      // both produce a literal brace. `\{` is preferred when the literal brace is adjacent to
      // an actual placeholder (e.g. `$"\{x\} = {x}"` → "{x} = 5"), since `{{x}}` would be
      // ambiguous (is it a literal `{x}` or a placeholder of a placeholder?).
      if (c === '\\' && raw[i + 1] === '{') { buf += '{'; i += 2; continue; }
      if (c === '\\' && raw[i + 1] === '}') { buf += '}'; i += 2; continue; }
      if (c === '{') {
        // Flush buffered literal text (if any) as a part.
        if (buf.length > 0) { parts.push({ kind: 'lit', text: buf }); buf = ''; }
        // Scan to the matching `}` at depth 0.
        // v0.8.8 fix: track string literals inside the expression so `}` inside '...' or "..."
        // does NOT decrement brace depth. Without this, `f"x={'hel}lo'}"` would close the
        // placeholder at the `}` inside the string `'hel}lo'`, producing exprText=`'hel` (broken).
        let depth = 1; let j = i + 1; let exprText = '';
        let inStr = false, strCh = null;
        while (j < n && depth > 0) {
          const cj = raw[j];
          if (inStr) {
            // Inside a string literal within the expression — copy verbatim, respect escapes.
            if (cj === '\\') { exprText += cj; exprText += (raw[j + 1] !== undefined ? raw[j + 1] : ''); j += 2; continue; }
            exprText += cj;
            if (cj === strCh) { inStr = false; strCh = null; }
            j++; continue;
          }
          if (cj === "'" || cj === '"') { inStr = true; strCh = cj; exprText += cj; j++; continue; }
          if (cj === '{') { depth++; exprText += cj; j++; continue; }
          if (cj === '}') { depth--; if (depth === 0) { j++; break; } exprText += cj; j++; continue; }
          exprText += cj; j++;
        }
        if (depth !== 0) throw new ParseError(`unterminated '{' in f-string (no matching '}')`, tok);
        // Recursively tokenize + parse the placeholder expression.
        const trimmed = exprText.trim();
        if (trimmed.length === 0) throw new ParseError(`empty '{}' placeholder in f-string (use '{{' for a literal brace)`, tok);
        let exprNode;
        try {
          const { tokens: subTokens } = tokenize(trimmed);
          // Keep only "real" tokens (drop NEWLINE / INDENT / DEDENT). Keep EOF so the sub-parser's
          // peek() always has a sentinel — without EOF, parsePrimary runs past the end and crashes
          // on `this.peek().type` returning undefined.
          const cleaned = subTokens.filter(t => t.type !== TT.NEWLINE && t.type !== TT.INDENT && t.type !== TT.DEDENT);
          const subParser = new Parser(cleaned);
          exprNode = subParser.parseExpr();
          // If there are leftover tokens (besides EOF), the placeholder contained a statement,
          // not an expression.
          if (subParser.pos < cleaned.length - 1) {
            throw new ParseError(`f-string placeholder '{${trimmed}}' has trailing tokens (only a single expression is allowed)`, tok);
          }
        } catch (e) {
          if (e instanceof ParseError) throw new ParseError(`f-string placeholder '{${trimmed}}' failed to parse: ${e.message}`, tok);
          throw e;
        }
        parts.push({ kind: 'expr', node: exprNode });
        i = j;
        continue;
      }
      buf += c; i++;
    }
    if (buf.length > 0) parts.push({ kind: 'lit', text: buf });
    return { type: 'FString', parts, prefix: tok.prefix, line: tok.line, col: tok.col };
  }

  parsePrimary() {
    const tok = this.peek();
    if (tok.type === TT.NUMBER) { this.advance(); return { type: 'NumberLit', value: tok.value, unit: tok.unit }; }
    // v0.5: hex number literal (0xffcc44)
    if (tok.type === TT.HEXNUM) { this.advance(); return { type: 'NumberLit', value: tok.value }; }
    if (tok.type === TT.STRING) { this.advance(); return { type: 'StringLit', value: tok.value }; }
    // v0.8.7: f-string interpolation — f"..." and $"..." lex as a single FSTRING token.
    // The raw payload is split on { } (with {{ }} escapes for literal braces); each {expr}
    // is recursively parsed via a sub-tokenize + sub-parse of the expression text.
    if (tok.type === TT.FSTRING) { this.advance(); return this.parseFString(tok); }
    if (tok.type === TT.HASH) {
      this.advance();
      const parts = [this.expect(TT.IDENT).value];
      while (this.at(TT.DOT)) { this.advance(); parts.push(this.expect(TT.IDENT).value); }
      return { type: 'TagRef', path: parts };
    }
    if (tok.type === TT.DOLLAR) {
      this.advance();
      const name = this.expect(TT.IDENT).value;
      return { type: 'Ident', name, sigil: '$' };
    }
    // v0.4: array literal [a, b, c]
    // v0.8.7: array comprehension — [expr for var in iter (if cond)?]
    //   `[n*n for n in nums if n%2==0]` desugars to a Comprehension node; eval iterates `iter`,
    //   binds `var` (or var1, var2 for multi-var), filters by `cond`, and collects `expr` results.
    //   Multi-var form `[a+b for a,b in pairs]` destructures each element (assumed array/tuple).
    //   The `for` keyword is recognized only after the first expression; otherwise it's a plain
    //   array literal. This means `[for, while, in]` (a list of three idents) still parses as
    //   ArrayLit — `for` only triggers comprehension mode when it follows an expression.
    if (tok.type === TT.LBRACKET) {
      this.advance();
      if (this.at(TT.RBRACKET)) { this.advance(); return { type: 'ArrayLit', elements: [] }; }
      const firstExpr = this.parseExpr();
      // v0.8.7: comprehension detection — `for` keyword after first expr.
      if (this.at(TT.IDENT) && this.peek().value === 'for') {
        this.advance(); // consume 'for'
        // Parse var(s): `var` or `v1, v2, v3` (multi-var destructuring).
        const vars = [this.expect(TT.IDENT).value];
        while (this.at(TT.COMMA)) {
          this.advance();
          vars.push(this.expect(TT.IDENT).value);
        }
        // Expect `in`.
        if (!(this.at(TT.IDENT) && this.peek().value === 'in')) {
          throw new ParseError(`expected 'in' after comprehension variable(s)`, this.peek());
        }
        this.advance(); // consume 'in'
        const iterable = this.parseExpr();
        // Optional `if cond`.
        let cond = null;
        if (this.at(TT.IDENT) && this.peek().value === 'if') {
          this.advance();
          cond = this.parseExpr();
        }
        this.expect(TT.RBRACKET);
        return { type: 'Comprehension', expr: firstExpr, vars, iterable, cond, line: tok.line, col: tok.col };
      }
      // Plain array literal.
      const elements = [firstExpr];
      while (this.at(TT.COMMA)) { this.advance(); elements.push(this.parseExpr()); }
      this.expect(TT.RBRACKET);
      return { type: 'ArrayLit', elements };
    }
    // v0.4: dict literal {k: v, k2: v2}
    if (tok.type === TT.LBRACE) {
      this.advance();
      const pairs = [];
      if (!this.at(TT.RBRACE)) {
        const k = this.expect(TT.IDENT).value;
        this.expect(TT.COLON);
        const v = this.parseExpr();
        pairs.push({ key: k, value: v });
        while (this.at(TT.COMMA)) { this.advance(); const kk = this.expect(TT.IDENT).value; this.expect(TT.COLON); const vv = this.parseExpr(); pairs.push({ key: kk, value: vv }); }
      }
      this.expect(TT.RBRACE);
      return { type: 'DictLit', pairs };
    }
    if (tok.type === TT.LPAREN) {
      this.advance();
      const e = this.parseExpr();
      this.expect(TT.RPAREN);
      return e;
    }
    // v0.5.1: receiverless query — `?name(args)` as a primary. The receiver is implicit
    // (ctx.entity at runtime). Matches world.ax's `p = ?path(from, to)` usage.
    // Runtime evalExpr() guards against node.obj === null.
    //
    // v0.8.7: prefix ternary — `? cond : then : else`. Disambiguation from the query form:
    //   - `? IDENT (` → query primary (existing behavior).
    //   - `? <anything else>` → prefix ternary. The cond is parsed as an expression, then
    //     `:` then `:` separate the three operands. This is a NEW form distinct from the
    //     existing suffix ternary `cond ? a : b` (which also still works). Both forms produce
    //     the same Ternary AST node — they're syntactic alternatives for the same semantics.
    //     The prefix form is preferred when the cond is long (the leading `?` makes the
    //     ternary intent visible at the start of the expression); the suffix form is preferred
    //     when cond is short (matches C/Python/JS conventions LLMs already know).
    if (tok.type === TT.QUESTION) {
      this.advance();
      // v0.8.8: `?!#Tag` shorthand for `?exists(#Tag)`. When `?` is followed by `!` and `#`,
      // parse as an Exists query. This is unambiguous because `?!` at expression position
      // would otherwise be a parse error (?! is the else-clause sigil, only valid after a
      // conditional block, not in expression position). Saves 1 token vs `?exists(#Tag)`.
      if (this.at(TT.BANG) && this.peek(1).type === TT.HASH) {
        this.advance();  // consume !
        this.advance();  // consume #
        const parts = [this.expect(TT.IDENT).value];
        while (this.at(TT.DOT)) { this.advance(); parts.push(this.expect(TT.IDENT).value); }
        // Produce a Query node with name='exists' and a synthetic TagRef arg — same AST as
        // `?exists(#Tag)`, so the interpreter needs no changes.
        const tagRef = { type: 'TagRef', path: parts, line: tok.line, col: tok.col };
        return { type: 'Query', obj: null, name: 'exists', args: [{ value: tagRef }], line: tok.line, col: tok.col };
      }
      // Query form: `?name(args)` — only when next is IDENT and the token after is `(`.
      if (this.at(TT.IDENT) && this.peek(1).type === TT.LPAREN) {
        const qname = this.expect(TT.IDENT).value;
        const args = this.parseArgs();
        return { type: 'Query', obj: null, name: qname, args };
      }
      // Prefix ternary: `? cond : then : else`.
      const cond = this.parseExpr();
      this.expect(TT.COLON);
      const then = this.parseExpr();
      this.expect(TT.COLON);
      const elze = this.parseExpr();
      return { type: 'Ternary', cond, then, else: elze, line: tok.line, col: tok.col };
    }
    if (tok.type === TT.IDENT) {
      this.advance();
      return { type: 'Ident', name: tok.value };
    }
    throw new ParseError(`unexpected token in expression: ${tok.type}`, tok);
  }
}

function parse(source) {
  const { tokens, version } = tokenize(source);
  const p = new Parser(tokens);
  const program = p.parseProgram();
  program.version = version; // v0.3: from an optional "axiom X.Y" top-of-file pragma, else null
  // v0.8.8: attach collected parse errors (recovery mode) so the checker can surface them as
  // diagnostics alongside the semantic checks. If there are NO errors, this is an empty array
  // (no behavior change for clean programs). If there ARE errors, the program still parses
  // (partially) — the LLM gets all errors in one pass.
  program.parseErrors = p._parseErrors || [];
  return program;
}

module.exports = { parse, Parser, ParseError };
