// AxiomScript static checker — v0.8.17
//
// This module is the "compiler" referred to in §3: it never throws for a violation it
// recognizes. It walks a successfully-parsed Program and returns an array of Semantic Error
// Payload objects (§3's payload shape table), one per finding, batched (§3 Rule: "batch, don't
// trickle" — every check below runs to completion over the whole program regardless of how many
// prior findings it already produced).
//
// v0.4 adds: function/proc param validation, loop control flow checks, timer type validation,
// collider shape validation, mixin field conflict detection, collection type validation.

const { parse } = require('./parser');
const { NATIVE_SUBSYSTEMS, SUPPORTED_INFER_STRATEGIES } = require('./interpreter');

const RULE = {
  ALLOC: { section: '§2.1.1', title: 'The Zero-Allocation Contract' },
  KERNEL: { section: '§2.3', title: 'Axiom Channels — the D↔S Bridge' },
  NATIVE: { section: '§1.6', title: 'Compiler Pragmas for the Deep Math Core' },
  EVENT: { section: '§2.3.1', title: 'The Broadcast Protocol' },
  INFER: { section: '§2.2', title: 'Inference Strategies' },
  POOL: { section: '§2.1.1', title: 'Pool / Collection Type Validation' },
  VERSION: { section: 'header pragma', title: 'axiom X.Y version pragma' },
  PARSE: { section: 'Appendix B', title: 'Minimal Grammar Sketch' },
  FN: { section: 'v0.4', title: 'User Function / Proc Validation' },
  LOOP: { section: 'v0.4', title: 'Loop Control Flow' },
  MIXIN: { section: 'v0.4', title: 'Mixin Composition' },
};

function makePayload(p) {
  return {
    error_code: p.code,
    severity: p.severity,
    location: { entity: p.entity ?? null, block: p.block ?? null, line: p.line ?? null, col: p.col ?? null },
    violated_rule: p.rule,
    context_snippet: p.snippet ?? null,
    message_for_human: p.human,
    message_for_agent: p.agent,
    suggested_fix: p.fix ?? null,
    auto_fixable: !!p.autoFixable,
  };
}

function sourceLine(source, line) {
  if (!line) return null;
  const lines = source.split('\n');
  return lines[line - 1] !== undefined ? lines[line - 1].trim() : null;
}

// -----------------------------------------------------------------------------
// Generic AST walkers
function walkExpr(node, visit) {
  if (!node || typeof node !== 'object') return;
  visit(node);
  switch (node.type) {
    case 'Member': walkExpr(node.obj, visit); return;
    case 'Index': walkExpr(node.obj, visit); walkExpr(node.index, visit); return;
    case 'Unary': walkExpr(node.expr, visit); return;
    case 'Binary': walkExpr(node.left, visit); walkExpr(node.right, visit); return;
    case 'Ternary': walkExpr(node.cond, visit); walkExpr(node.then, visit); walkExpr(node.else, visit); return;
    // v0.8.8: null-coalescing — walk both sides (either may contain a `~=` or string concat
    // that the zero-alloc check needs to see).
    case 'NullCoalesce': walkExpr(node.left, visit); walkExpr(node.right, visit); return;
    case 'InferExpr': walkExpr(node.dist, visit); return;
    case 'MethodCall': walkExpr(node.obj, visit); node.args.forEach(a => walkExpr(a.value, visit)); return;
    case 'Call': node.args.forEach(a => walkExpr(a.value, visit)); return;
    case 'Query': walkExpr(node.obj, visit); node.args.forEach(a => walkExpr(a.value, visit)); return;
    case 'ArrayLit': node.elements.forEach(e => walkExpr(e, visit)); return;
    case 'DictLit': node.pairs.forEach(p => walkExpr(p.value, visit)); return;
    // v0.8.7: f-string — recurse into each {expr} placeholder so checks (zero-alloc, sealed body,
    // etc.) see what's inside the interpolation. Literal parts have no expr to walk.
    case 'FString': node.parts.forEach(p => { if (p.kind === 'expr') walkExpr(p.node, visit); }); return;
    // v0.8.7: comprehension — the iterable, the filter cond, and the projection expr all need
    // walking (zero-alloc will catch a `~=` inside the comprehension, for example).
    case 'Comprehension': walkExpr(node.iterable, visit); if (node.cond) walkExpr(node.cond, visit); walkExpr(node.expr, visit); return;
    // v0.9.0: lambda bodies, pipelines, and value-calls are ordinary expression trees — walk
    // them so every existing check (zero-alloc, sealed body, undefined function) sees inside.
    case 'Lambda': walkExpr(node.body, visit); return;
    case 'Pipe': walkExpr(node.left, visit); walkExpr(node.right, visit); return;
    case 'CallValue': walkExpr(node.callee, visit); node.args.forEach(a => walkExpr(a.value, visit)); return;
    default: return;
  }
}

function stmtExprRoots(stmt) {
  switch (stmt.type) {
    case 'Assign': return [stmt.value];
    // v0.8.8: IndexAssign — walk the index and value (either may contain a zero-alloc violation).
    case 'IndexAssign': return [stmt.index, stmt.value];
    // v0.8.8: MemberAssign/DeepAssign with compoundOp — walk the value (the read of current is
    // implicit, not an expr root; the value expr is what the LLM wrote).
    case 'MemberAssign': return [stmt.value];
    case 'DeepAssign': return [stmt.value];
    case 'ExprStmt': return [stmt.expr];
    case 'Action': return stmt.args.map(a => a.value);
    case 'Emit': return stmt.args.map(a => a.value);
    case 'Broadcast': {
      const roots = stmt.args.map(a => a.value);
      if (stmt.addressing.radius) roots.push(stmt.addressing.radius);
      if (stmt.addressing.origin) roots.push(stmt.addressing.origin);
      return roots;
    }
    case 'TransitionChain': {
      const roots = [];
      for (const c of stmt.clauses) { if (c.guard) roots.push(c.guard); for (const a of c.args) roots.push(a.value); }
      return roots;
    }
    case 'Assert': return [stmt.expr];
    case 'Return': return [stmt.expr];
    case 'CondBlock': {
      // v0.8.8: walk into the if/else bodies — a `~=` or string concat inside a ?cond: block
      // inside a &physics: block was previously invisible to zero-alloc checking.
      const roots = [stmt.cond, stmt.guard].filter(Boolean);
      if (stmt.ifBody) for (const s of stmt.ifBody) roots.push(...stmtExprRoots(s));
      if (stmt.elseBody) for (const s of stmt.elseBody) roots.push(...stmtExprRoots(s));
      return roots;
    }
    case 'WhileLoop': {
      // v0.8.8: walk loop body — string concat inside `*cond:` inside `&physics:` was invisible.
      const roots = [stmt.cond];
      if (stmt.body) for (const s of stmt.body) roots.push(...stmtExprRoots(s));
      return roots;
    }
    case 'ForLoop': {
      // v0.8.8: walk loop body — same fix as WhileLoop. The iterable is already walked.
      const roots = [stmt.iterable];
      if (stmt.body) for (const s of stmt.body) roots.push(...stmtExprRoots(s));
      return roots;
    }
    // v0.9.0: match arms, try/catch/finally bodies, and throw payloads.
    case 'Match': {
      const roots = [stmt.subject];
      for (const arm of stmt.arms || []) {
        if (arm.patterns) roots.push(...arm.patterns);
        for (const s of arm.body || []) roots.push(...stmtExprRoots(s));
      }
      return roots;
    }
    case 'Try': {
      const roots = [];
      for (const s of stmt.body || []) roots.push(...stmtExprRoots(s));
      for (const s of stmt.catchBody || []) roots.push(...stmtExprRoots(s));
      for (const s of stmt.finallyBody || []) roots.push(...stmtExprRoots(s));
      return roots;
    }
    case 'Throw': return stmt.expr ? [stmt.expr] : [];
    case 'DestructureAssign': return [stmt.value];
    default: return [];
  }
}

function forEachEntityBlock(program, fn) {
  const visit = (entityDecl) => {
    for (const m of entityDecl.members) {
      if (m.type === 'Block') fn(entityDecl, m);
      else if (m.type === 'EntityDecl') visit(m);
    }
  };
  for (const e of program.entities) visit(e);
}

// -----------------------------------------------------------------------------
// §2.1.1 — The Zero-Allocation Contract
const ZERO_ALLOC_BLOCKS = new Set(['physics', 'render', 'on']);
const RESIZE_METHODS = new Set(['push', 'append', 'insert']);

function checkZeroAlloc(program, source) {
  const out = [];
  forEachEntityBlock(program, (entityDecl, block) => {
    if (!ZERO_ALLOC_BLOCKS.has(block.name)) return;
    if (NATIVE_SUBSYSTEMS[entityDecl.base]) return;
    for (const stmt of block.body) {
      // v0.8.7: `!d(...)` debug intrinsic is whitelisted in hot blocks. Its whole purpose
      // is to be a token-efficient way to emit debug logs from &physics/&render/&on without
      // the 15-20 token boilerplate of moving logs to a &tick block. The runtime buffers
      // raw args to a fixed-size ring buffer (no per-call allocation), so the zero-alloc
      // contract is respected at runtime even though the source contains string concat.
      // We skip the entire stmt — no walkExpr, no ~= check — because the debug call's args
      // are evaluated lazily and never escape the ring buffer.
      if (stmt.type === 'Action' && (stmt.name === 'd' || stmt.name === 'print' || stmt.name === 'stop_anim')) continue; // v0.8.15: also whitelist !stop_anim
      if (stmt.type === 'Assign' && stmt.op === '~=') {
        out.push(makePayload({
          code: 'AX-KERNEL-001', severity: 'fatal', entity: entityDecl.name, block: block.name, line: stmt.line, col: stmt.col,
          rule: RULE.KERNEL, snippet: sourceLine(source, stmt.line),
          human: `An S-Kernel update ('~=') was called from a '&${block.name}' block.`,
          agent: `Move this line into a '&tick' block.`,
          fix: { kind: 'text', detail: `Relocate the '~=' line to a '&tick' block.` },
        }));
      }
      for (const root of stmtExprRoots(stmt)) {
        walkExpr(root, (node) => {
          if (node.type === 'InferExpr') {
            out.push(makePayload({
              code: 'AX-KERNEL-001', severity: 'fatal', entity: entityDecl.name, block: block.name, line: stmt.line, col: stmt.col,
              rule: RULE.KERNEL, snippet: sourceLine(source, stmt.line),
              human: `An S-Kernel infer op ('~> ${node.op}') was used inside a '&${block.name}' block.`,
              agent: `Move the '~> ${node.op}' expression into a '&tick' block.`,
              fix: { kind: 'text', detail: `Move the '~> ${node.op}' expression into a '&tick' block.` },
            }));
          }
          if (node.type === 'MethodCall' && RESIZE_METHODS.has(node.method)) {
            out.push(makePayload({
              code: 'AX-ALLOC-002', severity: 'contract_violation', entity: entityDecl.name, block: block.name, line: stmt.line, col: stmt.col,
              rule: RULE.ALLOC, snippet: sourceLine(source, stmt.line),
              human: `Growable-array '.${node.method}(...)' inside a '&${block.name}' block violates the zero-allocation contract.`,
              agent: `Use a fixed-capacity pool or BVec instead.`,
              fix: { kind: 'text', detail: `Replace '.${node.method}(...)' with a Pool/BVec operation.` },
            }));
          }
          if (node.type === 'Binary' && node.op === '+' && (node.left.type === 'StringLit' || node.right.type === 'StringLit')) {
            out.push(makePayload({
              code: 'AX-ALLOC-003', severity: 'contract_violation', entity: entityDecl.name, block: block.name, line: stmt.line, col: stmt.col,
              rule: RULE.ALLOC, snippet: sourceLine(source, stmt.line),
              human: `Runtime string concatenation inside a '&${block.name}' block.`,
              agent: `Emit raw values on a channel instead.`,
              fix: { kind: 'text', detail: `Use '^emit' instead of string concatenation.` },
            }));
          }
          // v0.8.15: f-strings also allocate at runtime — flag in hot blocks
          if (node.type === 'FString') {
            out.push(makePayload({
              code: 'AX-ALLOC-003', severity: 'contract_violation', entity: entityDecl.name, block: block.name, line: stmt.line, col: stmt.col,
              rule: RULE.ALLOC, snippet: sourceLine(source, stmt.line),
              human: `F-string allocation inside a '&${block.name}' block.`,
              agent: `Use !d() for debug logging in hot blocks, or move f-string to &tick.`,
              fix: { kind: 'text', detail: `Use '!d(...)' instead of f-strings in hot blocks.` },
            }));
          }
          // v0.8.15: print() allocates strings — flag in hot blocks
          if (node.type === 'Call' && node.callee === 'print') {
            out.push(makePayload({
              code: 'AX-ALLOC-003', severity: 'contract_violation', entity: entityDecl.name, block: block.name, line: stmt.line, col: stmt.col,
              rule: RULE.ALLOC, snippet: sourceLine(source, stmt.line),
              human: `print() call inside a '&${block.name}' block allocates strings.`,
              agent: `Use !d() for debug logging in hot blocks, or move print() to &tick.`,
              fix: { kind: 'text', detail: `Use '!d(...)' instead of print() in hot blocks.` },
            }));
          }
          if (node.type === 'Call' && node.callee === 'spawn_new') {
            out.push(makePayload({
              code: 'AX-ALLOC-001', severity: 'contract_violation', entity: entityDecl.name, block: block.name, line: stmt.line, col: stmt.col,
              rule: RULE.ALLOC, snippet: sourceLine(source, stmt.line),
              human: `Dynamic heap instantiation inside a '&${block.name}' block.`,
              agent: `Use a Pool/BVec instead.`,
              fix: { kind: 'text', detail: `Use '!spawn(pool, init)' instead.` },
            }));
          }
        });
      }
    }
  });
  return out;
}

// -----------------------------------------------------------------------------
// §1.6 — Sealed body
function isPlainRead(node) {
  if (!node) return false;
  if (node.type === 'NumberLit' || node.type === 'StringLit' || node.type === 'Ident' || node.type === 'TagRef') return true;
  if (node.type === 'Member') return isPlainRead(node.obj);
  if (node.type === 'Index') return isPlainRead(node.obj) && isPlainRead(node.index);
  return false;
}

function checkSealedBody(program, source) {
  const out = [];
  forEachEntityBlock(program, (entityDecl, block) => {
    const subsystem = NATIVE_SUBSYSTEMS[entityDecl.base];
    // v0.5: only check sealed surface for subsystems that opt in (e.g., Fluid3D)
    // Trait-like subsystems (Body3D, Camera, Audio3D, etc.) allow general statements
    if (!subsystem || !subsystem.sealed) return;
    for (const stmt of block.body) {
      let ok = false;
      let reason = '';
      if (stmt.type === 'Action') {
        ok = subsystem.actions.includes(stmt.name);
        reason = `action '!${stmt.name}(...)' is not in &${entityDecl.base}'s declared surface`;
      } else if (stmt.type === 'Assign' && stmt.op === '=') {
        if (stmt.value.type === 'Query' && subsystem.queries.includes(stmt.value.name)) ok = true;
        else if (isPlainRead(stmt.value)) ok = true;
        reason = `right-hand side is neither a query nor a plain read`;
      } else if (stmt.type === 'ExprStmt' && isPlainRead(stmt.expr)) {
        ok = true;
      } else {
        reason = `statement type '${stmt.type}' is not part of &${entityDecl.base}'s declared surface`;
      }
      if (!ok) {
        out.push(makePayload({
          code: 'AX-NATIVE-001', severity: 'contract_violation', entity: entityDecl.name, block: block.name, line: stmt.line, col: stmt.col,
          rule: RULE.NATIVE, snippet: sourceLine(source, stmt.line),
          human: `Entity '${entityDecl.name}' (&${entityDecl.base}) contains a statement outside its sealed surface.`,
          agent: `${reason}.`,
          fix: { kind: 'text', detail: `Use only declared actions/queries or plain reads.` },
        }));
      }
    }
  });
  return out;
}

// -----------------------------------------------------------------------------
// §2.3.1 — Event schema validation
function staticTypeOf(node) {
  if (node.type === 'NumberLit') return 'number';
  if (node.type === 'StringLit') return 'string';
  return null;
}

// v0.8.4: the runtime auto-fills the `source` field of a broadcast with the broadcasting
// entity when omitted (v0.8.1 convenience, see interpreter.js Broadcast case). To keep
// that convenience working with the v0.8.4 tightening (which now recurses into CondBlock
// and loop bodies), the static checker treats a required `source:: #Entity` field as
// auto-fillable — i.e. it skips the missing-required-field check for it. The user gets
// the convenience (no need to write `source: self` on every ^Hit), but other required
// fields (e.g. `amount:: number`) still get the missing-field diagnostic even when
// nested in control flow.
//
// Note: OPTIONAL fields (declared `field:: type?`) are already skipped via f.optional.
// This special case is only for the required-`source` runtime auto-fill.
function isAutoFillableField(f) {
  return f.optional || (f.name === 'source' && f.ftype === '#Entity');
}

// v0.8.4: recursively yield every statement reachable from a block body, descending into
// CondBlock (ifBody + elseBody), WhileLoop (body), and ForLoop (body). Before v0.8.4, the
// event schema checker only walked direct statements — so a ^Broadcast nested inside
// `?cond: ^Event(...)` or `*for ... ^Event(...)` was never validated, and the missing-
// required-field check (AX-EVENT-001) silently passed. The runtime still auto-fills the
// required `source:: #Entity` field with the broadcasting entity (v0.8.1 convenience,
// preserved via `isAutoFillableField` above), so this tightening only catches genuinely
// missing required fields — never `source:: #Entity`.
function* walkStmts(stmts) {
  for (const stmt of stmts || []) {
    yield stmt;
    if (stmt.type === 'CondBlock') {
      yield* walkStmts(stmt.ifBody);
      if (stmt.elseBody) yield* walkStmts(stmt.elseBody);
    } else if (stmt.type === 'WhileLoop' || stmt.type === 'ForLoop') {
      yield* walkStmts(stmt.body);
    }
  }
}

function checkEvents(program, source) {
  const out = [];
  const schemas = new Map();
  for (const ev of program.events) schemas.set(ev.name, ev);
  // v0.8.2: built-in events that don't require a ^event declaration. `Collide` is fired by
  // stepCollisions whenever two colliders overlap; its payload ({other, normal, depth}) is
  // provided by the runtime, not by a user-declared schema.
  const BUILTIN_EVENTS = new Set(['Collide']);
  forEachEntityBlock(program, (entityDecl, block) => {
    if (block.name === 'on' && block.eventArg && !schemas.has(block.eventArg) && !BUILTIN_EVENTS.has(block.eventArg)) {
      out.push(makePayload({
        code: 'AX-EVENT-001', severity: 'fatal', entity: entityDecl.name, block: block.name, line: block.line, col: block.col,
        rule: RULE.EVENT, snippet: sourceLine(source, block.line),
        human: `'&on(${block.eventArg})' handles an undeclared event.`,
        agent: `Declare '^event ${block.eventArg}:' at the top level, or use a built-in event like 'Collide'.`,
        fix: { kind: 'text', detail: `Add '^event ${block.eventArg}:' with its field list.` },
      }));
    }
    // v0.8.4: walk recursively so ^Broadcast nodes nested in CondBlock/loop bodies are
    // also validated. This closes the gap where `?cond: ^Event(...)` (no required field
    // passed) compiled clean because the missing-field check only saw direct statements.
    for (const stmt of walkStmts(block.body)) {
      if (stmt.type !== 'Broadcast') continue;
      const schema = schemas.get(stmt.name);
      if (!schema) {
        out.push(makePayload({
          code: 'AX-EVENT-001', severity: 'fatal', entity: entityDecl.name, block: block.name, line: stmt.line, col: stmt.col,
          rule: RULE.EVENT, snippet: sourceLine(source, stmt.line),
          human: `'^${stmt.name}(...)' broadcasts an undeclared event.`,
          agent: `Declare '^event ${stmt.name}:'.`,
          fix: { kind: 'text', detail: `Add '^event ${stmt.name}:' at the top level.` },
        }));
        continue;
      }
      const declared = new Map(schema.fields.map(f => [f.name, f]));
      const given = new Set();
      for (const a of stmt.args) {
        if (!a.name) continue;
        given.add(a.name);
        const field = declared.get(a.name);
        if (!field) {
          out.push(makePayload({
            code: 'AX-EVENT-001', severity: 'fatal', entity: entityDecl.name, block: block.name, line: stmt.line, col: stmt.col,
            rule: RULE.EVENT, snippet: sourceLine(source, stmt.line),
            human: `'^${stmt.name}(...)' passes undeclared field '${a.name}'.`,
            agent: `'${a.name}' is not in the schema.`,
            fix: { kind: 'text', detail: `Remove '${a.name}' or add it to the schema.` },
          }));
          continue;
        }
        const actual = staticTypeOf(a.value);
        if (actual && field.ftype !== actual) {
          out.push(makePayload({
            code: 'AX-EVENT-001', severity: 'fatal', entity: entityDecl.name, block: block.name, line: stmt.line, col: stmt.col,
            rule: RULE.EVENT, snippet: sourceLine(source, stmt.line),
            human: `'^${stmt.name}(${a.name}: ...)' type mismatch (expected '${field.ftype}').`,
            agent: `Pass a '${field.ftype}' value.`,
            fix: { kind: 'text', detail: `Change the argument to '${field.ftype}'.` },
          }));
        }
      }
      for (const f of schema.fields) {
        if (isAutoFillableField(f)) continue;
        if (!given.has(f.name)) {
          out.push(makePayload({
            code: 'AX-EVENT-001', severity: 'fatal', entity: entityDecl.name, block: block.name, line: stmt.line, col: stmt.col,
            rule: RULE.EVENT, snippet: sourceLine(source, stmt.line),
            human: `'^${stmt.name}(...)' missing required field '${f.name}'.`,
            agent: `Add '${f.name}: <value>' to the call.`,
            fix: { kind: 'text', detail: `Add '${f.name}: <value>'.` },
          }));
        }
      }
    }
  });
  return out;
}

// -----------------------------------------------------------------------------
// §2.2 — Infer strategy validation
function checkInferStrategies(program, source) {
  const out = [];
  for (const entityDecl of program.entities) {
    for (const m of entityDecl.members) {
      if (m.type !== 'FieldDecl' || m.sigil !== '$' || !m.infer) continue;
      const name = m.infer.callee;
      if (!SUPPORTED_INFER_STRATEGIES.has(name)) {
        out.push(makePayload({
          code: 'AX-INFER-001', severity: 'fatal', entity: entityDecl.name, block: null, line: m.line, col: m.col,
          rule: RULE.INFER,
          human: `'infer: ${name}(...)' is not implemented.`,
          agent: `Use 'infer: exact' or 'infer: particle(N)'.`,
          fix: { kind: 'patch', detail: `Replace with 'infer: exact'.` },
        }));
      }
    }
  }
  return out;
}

// -----------------------------------------------------------------------------
// §2.1.1 — Pool / Collection type validation
function checkPoolTypes(program, source) {
  const out = [];
  const typeDecls = new Map(program.types.map(t => [t.name, t]));
  for (const entityDecl of program.entities) {
    const poolFields = new Map();
    for (const m of entityDecl.members) {
      if (m.type === 'FieldDecl' && m.sigil === '~' && m.value && (m.value.type === 'PoolType' || m.value.type === 'VecType' || m.value.type === 'MapType')) {
        poolFields.set(m.name, m.value);
        if (m.value.type === 'PoolType' && !typeDecls.has(m.value.elementType)) {
          out.push(makePayload({
            code: 'AX-POOL-001', severity: 'contract_violation', entity: entityDecl.name, block: null, line: m.line, col: m.col,
            rule: RULE.POOL,
            human: `&Pool(${m.value.elementType}, ${m.value.capacity}) references undeclared type.`,
            agent: `Declare '^type ${m.value.elementType}:'.`,
            fix: { kind: 'text', detail: `Add '^type ${m.value.elementType}:'.` },
          }));
        }
      }
    }
    if (poolFields.size === 0) continue;
    forEachEntityBlock(program, (ed, block) => {
      if (ed !== entityDecl) return;
      for (const stmt of block.body) {
        if (stmt.type !== 'Action' || stmt.name !== 'spawn') continue;
        const poolArg = stmt.args[0] && stmt.args[0].value;
        const initArg = stmt.args[1] && stmt.args[1].value;
        if (!poolArg || poolArg.type !== 'Ident' || !poolFields.has(poolArg.name)) continue;
        const pool = poolFields.get(poolArg.name);
        const typeDecl = typeDecls.get(pool.elementType);
        if (!typeDecl || !initArg || initArg.type !== 'Call') continue;
        if (initArg.callee !== pool.elementType) {
          out.push(makePayload({
            code: 'AX-POOL-002', severity: 'contract_violation', entity: entityDecl.name, block: block.name, line: stmt.line, col: stmt.col,
            rule: RULE.POOL,
            human: `'!spawn(${poolArg.name}, ${initArg.callee}(...))' type mismatch (expected '${pool.elementType}').`,
            agent: `Use '${pool.elementType}(...)' as the initializer.`,
            fix: { kind: 'text', detail: `Change to '${pool.elementType}(...)'.` },
          }));
        } else if (initArg.args.length !== typeDecl.fields.length) {
          out.push(makePayload({
            code: 'AX-POOL-002', severity: 'contract_violation', entity: entityDecl.name, block: block.name, line: stmt.line, col: stmt.col,
            rule: RULE.POOL,
            human: `'${pool.elementType}(...)' arg count mismatch (${initArg.args.length} vs ${typeDecl.fields.length}).`,
            agent: `Match the declared field count.`,
            fix: { kind: 'text', detail: `Pass ${typeDecl.fields.length} args.` },
          }));
        }
      }
    });
  }
  return out;
}

// -----------------------------------------------------------------------------
// v0.4 — User function/proc param validation
function checkFunctions(program, source) {
  const out = [];
  // Check for duplicate fn/proc names
  const allNames = new Map();
  for (const fn of program.fns || []) {
    if (allNames.has(fn.name)) {
      out.push(makePayload({
        code: 'AX-FN-001', severity: 'fatal', line: fn.line, col: fn.col,
        rule: RULE.FN,
        human: `Duplicate function/proc name '${fn.name}'.`,
        agent: `Rename one of the '${fn.name}' declarations.`,
        fix: null,
      }));
    }
    allNames.set(fn.name, fn);
  }
  for (const p of program.procs || []) {
    if (allNames.has(p.name)) {
      out.push(makePayload({
        code: 'AX-FN-001', severity: 'fatal', line: p.line, col: p.col,
        rule: RULE.FN,
        human: `Duplicate function/proc name '${p.name}'.`,
        agent: `Rename one of the '${p.name}' declarations.`,
        fix: null,
      }));
    }
    allNames.set(p.name, p);
  }
  // v0.8.15: check for unreachable code after ^return in fn/proc bodies
  for (const fn of [...(program.fns || []), ...(program.procs || [])]) {
    const body = fn.body || [];
    for (let i = 0; i < body.length - 1; i++) {
      if (body[i].type === 'Return') {
        out.push(makePayload({
          code: 'AX-FN-002', severity: 'advisory', line: body[i + 1].line, col: body[i + 1].col,
          rule: RULE.FN, snippet: sourceLine(source, body[i + 1].line),
          human: `Unreachable code after ^return in ${fn.name}.`,
          agent: `Code after ^return is never executed. Remove it or move the ^return.`,
          fix: null, autoFixable: false,
        }));
        break; // only report the first unreachable stmt
      }
    }
  }
  // v0.8.15: check for compile-time division by zero
  forEachEntityBlock(program, (entityDecl, block) => {
    for (const stmt of block.body) {
      for (const root of stmtExprRoots(stmt)) {
        walkExpr(root, (node) => {
          // v0.8.15: also check % (modulo) by zero — same NaN risk
          if (node.type === 'Binary' && (node.op === '/' || node.op === '%') &&
              node.right && node.right.type === 'NumberLit' && node.right.value === 0) {
            out.push(makePayload({
              code: 'AX-DIV-001', severity: 'advisory', entity: entityDecl.name, block: block.name,
              line: stmt.line, col: stmt.col, rule: RULE.ALLOC, snippet: sourceLine(source, stmt.line),
              human: `${node.op === '/' ? 'Division' : 'Modulo'} by zero (literal 0 as divisor).`,
              agent: `The right-hand side of '${node.op}' is the literal 0. This produces Infinity or NaN at runtime. Variable-based div-by-zero requires data-flow analysis (out of scope).`,
              fix: null, autoFixable: false,
            }));
          }
        });
      }
    }
  });
  return out;
}

// -----------------------------------------------------------------------------
// v0.4 — Loop control flow: break/continue must be inside a loop
function checkLoopControl(program, source) {
  const out = [];
  function checkStmts(stmts, inLoop, entityName, blockName) {
    for (const stmt of stmts) {
      if (stmt.type === 'Break' || stmt.type === 'Continue') {
        if (!inLoop) {
          out.push(makePayload({
            code: 'AX-LOOP-001', severity: 'fatal', entity: entityName, block: blockName, line: stmt.line, col: stmt.col,
            rule: RULE.LOOP,
            human: `'~${stmt.type === 'Break' ? 'break' : 'continue'}' outside a loop.`,
            agent: `Move this into a '*cond:' or '*i in 0..n:' loop.`,
            fix: null,
          }));
        }
      }
      if (stmt.type === 'CondBlock') {
        checkStmts(stmt.ifBody, inLoop, entityName, blockName);
        if (stmt.elseBody) checkStmts(stmt.elseBody, inLoop, entityName, blockName);
      }
      if (stmt.type === 'WhileLoop' || stmt.type === 'ForLoop') {
        checkStmts(stmt.body, true, entityName, blockName);
      }
    }
  }
  forEachEntityBlock(program, (entityDecl, block) => {
    checkStmts(block.body, false, entityDecl.name, block.name);
  });
  // Also check fn/proc bodies
  for (const fn of program.fns || []) checkStmts(fn.body, false, null, `fn ${fn.name}`);
  for (const p of program.procs || []) checkStmts(p.body, false, null, `proc ${p.name}`);
  return out;
}

// -----------------------------------------------------------------------------
// v0.4 — Mixin validation: check that referenced mixins exist
function checkMixins(program, source) {
  const out = [];
  const mixinNames = new Set((program.mixins || []).map(m => m.name));
  const mixinFields = new Map(); // mixinName -> Map<fieldName, value-AST>
  for (const m of program.mixins || []) mixinFields.set(m.name, collectFieldDefaults(m.members));
  for (const e of program.entities) {
    for (const mName of e.mixins || []) {
      if (!mixinNames.has(mName)) {
        out.push(makePayload({
          code: 'AX-MIXIN-001', severity: 'fatal', entity: e.name, line: e.line, col: e.col,
          rule: RULE.MIXIN,
          human: `Entity '${e.name}' includes undefined mixin '+${mName}'.`,
          agent: `Declare '^mix ${mName}:' at the top level, or fix the name.`,
          fix: { kind: 'text', detail: `Add '^mix ${mName}:' or fix the reference.` },
        }));
        continue;
      }
      // v0.5.1: detect when an entity overrides a mixin field with a DIFFERENT default value.
      // Shadowing is allowed (the spec relies on it), but a silent default change is usually a typo,
      // so we surface it as advisory. Equality is structural on NumberLit / StringLit only; complex
      // expressions are skipped (treated as "intentional override") to keep the check cheap.
      //
      // v0.8.7: AX-MIXIN-002 advisory emission REMOVED. The `~field!:` sigil already signals
      // intent (the LLM explicitly marked the override as intentional), so the advisory is
      // redundant — it costs more tokens in retry cycles than the `!` costs upfront. The
      // `!` parsing stays in interpreter.js (the AST has `noWarn: true`), but the checker
      // no longer emits AX-MIXIN-002 regardless of whether `!` is present. This makes
      // `~hp: 60` and `~hp!: 60` behave identically from the checker's perspective — both
      // are valid overrides, no advisory. The `!` form is still documented as the preferred
      // form (it signals intent to human/LLM readers), but it's no longer required to
      // suppress an advisory.
      //
      // The field-collection and comparison helpers (collectFieldDefaults, fieldDefaultsDiffer)
      // are kept for potential future use; the advisory emission itself is gone.
    }
  }
  return out;
}

function collectFieldDefaults(members) {
  const out = new Map();
  for (const m of members || []) {
    if (m.type === 'FieldDecl' && m.sigil === '~') out.set(m.name, { value: m.value || null, noWarn: !!m.noWarn });
  }
  return out;
}

function fieldDefaultsDiffer(a, b) {
  // a, b are {value, noWarn} entries from collectFieldDefaults.
  if (!a && !b) return false;
  if (!a || !b) return true;
  const av = a.value, bv = b.value;
  if (!av && !bv) return false;
  if (!av || !bv) return true;
  if (av.type === 'NumberLit' && bv.type === 'NumberLit') return av.value !== bv.value;
  if (av.type === 'StringLit' && bv.type === 'StringLit') return av.value !== bv.value;
  return false; // complex exprs: assume intentional
}

// -----------------------------------------------------------------------------
// v0.4 — Version pragma
const KNOWN_VERSIONS = new Set(['0.1', '0.2', '0.3', '0.4', '0.5', '0.5.1', '0.6', '0.6.1', '0.7', '0.8', '0.8.1', '0.8.2', '0.8.3', '0.8.4', '0.8.5', '0.8.6', '0.8.7', '0.8.8', '0.8.9', '0.8.10', '0.8.11', '0.8.12', '0.8.13', '0.8.15', '0.8.16', '0.8.17', '0.9', '0.9.0', '0.9.1', '0.9.2']);
function checkVersionPragma(program, source) {
  if (!program.version) return [];
  if (KNOWN_VERSIONS.has(program.version)) return [];
  return [makePayload({
    code: 'AX-VERSION-001', severity: 'advisory', entity: null, block: null, line: 1, col: 1,
    rule: RULE.VERSION, snippet: `axiom ${program.version}`,
    human: `Version '${program.version}' not recognized by this toolchain.`,
    agent: `Known: ${[...KNOWN_VERSIONS].join(', ')}.`,
    fix: null,
  })];
}

// v0.8.1: check for unknown block names. The interpreter only executes blocks named
// physics, render, tick, on. Blocks with other names parse fine but silently do nothing.
// This is a common LLM mistake — writing &init: or &update: instead of &physics:.
// v0.8.1 (enhanced): for each unknown block, compute the Levenshtein distance to each known
// block name. If the closest match has distance ≤ 3 (covers single-typo + a few transpositions
// or omissions), the diagnostic's `suggested_fix` recommends it explicitly. This catches
// `&phyiscs:` → `physics`, `&phsyics:` → `physics`, `&rendering:` → `render`, `&ticks:` → `tick`.
const KNOWN_BLOCKS = new Set(['physics', 'render', 'tick', 'on']);
const KNOWN_BLOCKS_LIST = ['physics', 'render', 'tick', 'on'];

// Classic Levenshtein edit distance (insertions, deletions, substitutions). Returns a non-
// negative integer. Case-insensitive (we lower-case both strings) so `&Physics:` matches
// `physics` at distance 0.
function levenshtein(a, b) {
  a = a.toLowerCase(); b = b.toLowerCase();
  const m = a.length, n = b.length;
  if (m === 0) return n;
  if (n === 0) return m;
  const prev = new Array(n + 1);
  const cur = new Array(n + 1);
  for (let j = 0; j <= n; j++) prev[j] = j;
  for (let i = 1; i <= m; i++) {
    cur[0] = i;
    for (let j = 1; j <= n; j++) {
      const cost = a[i - 1] === b[j - 1] ? 0 : 1;
      cur[j] = Math.min(prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost);
    }
    for (let j = 0; j <= n; j++) prev[j] = cur[j];
  }
  return prev[n];
}

// Find the closest known block name to `name` by Levenshtein distance. Returns the name
// (string) if the closest match is within `maxDist` (default 3); otherwise returns null.
function closestBlockName(name, maxDist = 3) {
  let bestName = null, bestDist = Infinity;
  for (const k of KNOWN_BLOCKS_LIST) {
    const d = levenshtein(name, k);
    if (d < bestDist) { bestDist = d; bestName = k; }
  }
  return bestDist <= maxDist ? bestName : null;
}

// v0.8.3: detect function-only intrinsics used as actions (!bar, !sphere, !v3, etc.).
// These compile fine but fail at runtime with AX-RUNTIME-ACTION. The checker catches them
// statically so the user sees a clear compile-time error instead of a runtime crash.
// `bar` is the canonical case: it's a HUD descriptor function only valid in ~hud: field
// declarations, but nothing stopped users from writing `!bar(...)` in a &render: block.
const FUNCTION_ONLY_INTRINSICS = new Set([
  'v2', 'v3', 'q', 'euler', 'm4', 'persp', 'ortho', 'lookat', 'aabb',
  'clamp', 'dist', 'sphere', 'box', 'capsule', 'patrol_point', 'bar',
  'vision_cells', 'cell_to_world',
]);
function checkFunctionOnlyInActionPosition(program, source) {
  const out = [];
  forEachEntityBlock(program, (entityDecl, block) => {
    for (const stmt of block.body) {
      if (stmt.type !== 'Action') continue;
      if (FUNCTION_ONLY_INTRINSICS.has(stmt.name)) {
        out.push(makePayload({
          code: 'AX-ACTION-001', severity: 'fatal', entity: entityDecl.name, block: block.name, line: stmt.line, col: stmt.col,
          rule: { section: 'v0.8.3', title: 'Function used as action' },
          snippet: sourceLine(source, stmt.line),
          human: `'!${stmt.name}(...)' is a function, not an action — it can't be used with the '!' sigil.`,
          agent: `'${stmt.name}' is an intrinsic function (valid in expressions like ~field: ${stmt.name}(...)). Only actions (play, mesh, save, spawn, etc.) can follow '!'. To draw a HUD bar, declare it as ~hud: bar(...); to create a collider, declare it as ~hit: ${stmt.name}(...).`,
          fix: { kind: 'text', detail: `Remove the '!' or move '${stmt.name}(...)' into a field declaration (e.g. ~hud: bar(...)).` },
        }));
      }
    }
  });
  return out;
}

// v0.8.3: detect broadcasts to events that have zero &on(Event) handlers in the entire
// program. This is a static check (entity types and their handlers are known at compile time).
// Silent no-ops are one of the worst failure modes for AI iteration — they don't error, so
// nothing prompts a second look. This advisory surfaces "you broadcast ^Foo but nobody listens."
// Built-in events (Collide) are excluded — they're delivered by the runtime, not by user code.
const BUILTIN_BROADCAST_EVENTS = new Set(['Collide']);
function checkBroadcastNoListeners(program, source) {
  const out = [];
  // Collect all &on(Event) handler event names across all entities.
  const handledEvents = new Set();
  const visit = (entityDecl) => {
    for (const m of entityDecl.members) {
      if (m.type === 'Block' && m.name === 'on' && m.eventArg) handledEvents.add(m.eventArg);
      else if (m.type === 'EntityDecl') visit(m);
    }
  };
  for (const e of program.entities) visit(e);
  // Also collect declared ^event names (those are always "handled" — someone might add a
  // handler later, and the event is part of the public schema). This prevents false positives
  // for events that are declared but not yet handled.
  const declaredEvents = new Set();
  for (const ev of program.events || []) declaredEvents.add(ev.name);
  // Scan all broadcast statements.
  forEachEntityBlock(program, (entityDecl, block) => {
    // v0.8.15: use walkStmts to find broadcasts in nested blocks (was only block.body)
    for (const stmt of walkStmts(block.body)) {
      if (stmt.type !== 'Broadcast') continue;
      if (BUILTIN_BROADCAST_EVENTS.has(stmt.name)) continue;
      // v0.8.3: only fire if NO entity has &on(Event) for this event. Declared-but-unhandled
      // events ARE still silent no-ops at runtime — the declaration just means "I intend to
      // send this," not "someone handles it." The advisory is still useful for catching the
      // common case of "I forgot to add the handler."
      // v0.8.15 fix: use declaredEvents set (was dead code — built but never checked).
      if (!handledEvents.has(stmt.name) && !declaredEvents.has(stmt.name)) {
        out.push(makePayload({
          code: 'AX-BROADCAST-001', severity: 'advisory', entity: entityDecl.name, block: block.name, line: stmt.line, col: stmt.col,
          rule: { section: 'v0.8.3', title: 'Broadcast with no listeners' },
          snippet: sourceLine(source, stmt.line),
          human: `'^${stmt.name}(...)' is broadcast but no entity has an '&on(${stmt.name}):' handler — this is a silent no-op.`,
          agent: `Either add an &on(${stmt.name}): block to the intended receiver, or declare '^event ${stmt.name}:' if the handler will be added later. Without a handler, the broadcast does nothing and produces no error.`,
          fix: { kind: 'text', detail: `Add '&on(${stmt.name}):' to the entity that should receive this event.` },
        }));
      }
    }
  });
  return out;
}

function checkUnknownBlocks(program, source) {
  const out = [];
  forEachEntityBlock(program, (entityDecl, block) => {
    if (KNOWN_BLOCKS.has(block.name)) return;
    // v0.8.1 (enhanced): suggest the closest valid block name if it looks like a typo.
    const suggestion = closestBlockName(block.name);
    const human = suggestion
      ? `Block '&${block.name}:' is not a recognized block name and will never execute. Did you mean '&${suggestion}:'?`
      : `Block '&${block.name}:' is not a recognized block name and will never execute.`;
    const fix = suggestion
      ? `Rename to '&${suggestion}:'. Recognized blocks: physics (60Hz deterministic), render (draw), tick(Nhz) (AI/cognition), on(Event) (event handlers).`
      : `Use &physics: for deterministic 60Hz logic, &tick(Nhz): for AI/cognition, &render: for drawing, &on(Event): for event handlers.`;
    out.push(makePayload({
      code: 'AX-BLOCK-001', severity: 'advisory', entity: entityDecl.name, block: block.name, line: block.line, col: block.col,
      rule: { section: 'runtime', title: 'Unknown Block' },
      snippet: sourceLine(source, block.line),
      human,
      agent: `The interpreter only runs blocks named: physics, render, tick, on. Other block names parse but are dead code. ${suggestion ? `Closest match: '${suggestion}' (Levenshtein distance ${levenshtein(block.name, suggestion)}).` : 'No close match found.'}`,
      fix,
      autoFixable: false,
    }));
  });
  return out;
}

// v0.8.8: detect calls to undeclared functions/intrinsics at compile time.
// Previously, calling `abx(x)` (typo of `abs`) compiled clean and failed at runtime with
// AX-RUNTIME-FUNC — wasting a full LLM retry cycle. Now we walk all Call nodes, collect the
// callee names, and check against: (1) all declared ^fn/^proc names, (2) a hardcoded set of
// known intrinsic names (mirroring defaultIntrinsics in interpreter.js). If the callee is not
// in either set, emit AX-UNDEF-FN-001 advisory with a suggestion (closest match by Levenshtein).
//
// Note: MethodCall nodes (obj.method()) are NOT checked here — the receiver's type isn't known
// at compile time, and method names vary by type (Vec3.mag, BVec.push, etc.). Only free
// function calls (name(args)) are checked.
// v0.9.0: the intrinsic list is DERIVED from the runtime (engine intrinsics + the standard
// library) instead of being retyped here. A hand-maintained copy drifted every release and
// produced false "undefined function" advisories for functions that existed — the single
// most expensive kind of checker bug, because the LLM then rewrites working code.
const RUNTIME_INTRINSIC_NAMES = (() => {
  try {
    const { World } = require('./interpreter');
    return Object.keys(new World().intrinsics);
  } catch (e) {
    return [];
  }
})();

const KNOWN_INTRINSIC_NAMES = new Set([
  ...RUNTIME_INTRINSIC_NAMES,
  // Original intrinsics:
  'v2', 'v3', 'q', 'euler', 'm4', 'persp', 'ortho', 'lookat', 'aabb', 'clamp', 'dist',
  'sphere', 'box', 'capsule', 'vision_cells', 'cell_to_world', 'bar',
  // v0.8.15: general-purpose intrinsics
  'json_parse', 'json_stringify', 'int', 'float', 'str', 'type', 'is_null', 'is_number', 'is_string', 'is_array', 'clock',
  'wrap', 'fract', 'smoothstep', 'hypot', 'trunc', 'cbrt', 'log1p',
  'band', 'bor', 'bxor', 'bnot', 'shl', 'shr', 'v2dir',
  // v0.8.15: callable print(), len(), range()
  'print', 'len', 'range',
  // v0.8.8 math intrinsics:
  'abs', 'floor', 'ceil', 'round', 'sqrt', 'sin', 'cos', 'tan', 'asin', 'acos', 'atan',
  'log', 'log10', 'log2', 'exp', 'sign', 'min', 'max', 'pow', 'atan2',
  'random', 'randomRange', 'randomInt', 'PI', 'TAU', 'E',
  'deg2rad', 'rad2deg', 'lerp', 'map_range',
  // v0.8.8 Vec3 shorthands:
  'v3x', 'v3y', 'v3z', 'v3xz',
  // v0.8.1 special:
  'patrol_point',
  // Observe is a special form handled in callFunction — not a real intrinsic, but recognized.
  'observe',
  // Atoms used as function-call args (eased-motion names) — these are atoms, not functions,
  // but LLMs sometimes write them in call position. Don't flag them.
  'linear', 'in', 'out', 'inout', 'bounce', 'elastic',
  // Built-in query names (treated as functions in some syntactic positions):
  'nearest', 'exists', 'path', 'raycast', 'block_cell', 'unblock_cell', 'is_blocked',
]);

function checkUndefinedFunctions(program, source) {
  const out = [];
  // Collect declared ^fn, ^proc, and ^type names. ^type names are used as constructors in
  // !spawn(bullets, Bullet(pos, vel)) — the call looks like a function call but is actually a
  // struct initializer. We treat them as "declared" so we don't emit false positives.
  const declaredFns = new Set();
  for (const fn of program.fns || []) declaredFns.add(fn.name);
  for (const p of program.procs || []) declaredFns.add(p.name);
  for (const t of program.types || []) declaredFns.add(t.name);
  // Build a combined suggestion pool: intrinsics + declared fns.
  const suggestionPool = [...KNOWN_INTRINSIC_NAMES, ...declaredFns];

  // Walk every Call node in every block of every entity.
  forEachEntityBlock(program, (entityDecl, block) => {
    // v0.9.0: names bound inside the block may hold a function (`h = \x: x + 1` then `h(2)`,
    // a handler pulled out of a dict, a loop variable over a list of callbacks). Collecting
    // them keeps this check from flagging calls that are perfectly valid — a false "undefined
    // function" costs a full rewrite cycle, which is worse than the miss it prevents.
    const boundNames = new Set(entityDecl.members.filter(m => m.type === 'FieldDecl').map(m => m.name));
    const collect = (stmts) => {
      for (const st of stmts || []) {
        if (!st || typeof st !== 'object') continue;
        if (st.type === 'Assign' && st.target) boundNames.add(st.target);
        if (st.type === 'DestructureAssign') for (const n of st.names || []) boundNames.add(n);
        if (st.type === 'ForLoop') { if (st.varName) boundNames.add(st.varName); for (const v of st.vars || []) boundNames.add(v); }
        if (st.type === 'Try' && st.catchVar) boundNames.add(st.catchVar);
        for (const key of ['body', 'ifBody', 'elseBody', 'catchBody', 'finallyBody']) {
          if (Array.isArray(st[key])) collect(st[key]);
        }
        for (const arm of st.arms || []) collect(arm.body);
        for (const root of stmtExprRoots(st)) {
          walkExpr(root, (n) => { if (n.type === 'Lambda') for (const pn of n.params || []) boundNames.add(pn); });
        }
      }
    };
    collect(block.body);
    for (const stmt of block.body) {
      for (const root of stmtExprRoots(stmt)) {
        walkExpr(root, (node) => {
          if (node.type !== 'Call') return;
          const callee = node.callee;
          if (!callee) return;
          // Skip if it's a known intrinsic, a declared fn, or a name bound in this block.
          if (KNOWN_INTRINSIC_NAMES.has(callee) || declaredFns.has(callee) || boundNames.has(callee)) return;
          // Skip `observe` (special form) and shape/prior calls in $-field decls (those are
          // validated by checkInferStrategies, not here).
          if (callee === 'observe') return;
          // Find closest match for suggestion.
          let bestSugg = null, bestDist = Infinity;
          for (const name of suggestionPool) {
            const d = levenshtein(callee, name);
            if (d < bestDist) { bestDist = d; bestSugg = name; }
          }
          // Only suggest if distance ≤ 3 (otherwise the suggestion is likely useless).
          const suggestion = bestDist <= 3 ? bestSugg : null;
          const human = suggestion
            ? `Call to undefined function '${callee}(...)'. Did you mean '${suggestion}'?`
            : `Call to undefined function '${callee}(...)'.`;
          const agent = suggestion
            ? `'${callee}' is not a declared ^fn/^proc and not a known intrinsic. Closest match: '${suggestion}' (Levenshtein distance ${bestDist}). Declare it with '^fn ${callee}(args):' or fix the spelling.`
            : `'${callee}' is not a declared ^fn/^proc and not a known intrinsic. Declare it with '^fn ${callee}(args):' or fix the spelling. Known intrinsics: v3, v2, q, euler, lookat, dist, clamp, abs, min, max, floor, sin, cos, PI, random, etc.`;
          out.push(makePayload({
            code: 'AX-UNDEF-FN-001', severity: 'advisory', entity: entityDecl.name, block: block.name,
            line: node.line || stmt.line, col: node.col || stmt.col,
            rule: { section: 'v0.8.8', title: 'Undefined Function' },
            snippet: sourceLine(source, stmt.line),
            human,
            agent,
            fix: suggestion ? { kind: 'text', detail: `Replace '${callee}' with '${suggestion}'.` } : null,
            autoFixable: false,
          }));
        });
      }
    }
  });
  return out;
}

// v0.8.8: validate ?nearest/#path first argument is a TagRef.
// `?nearest("Player")` compiles clean but fails at runtime requiring `#Tag`. This wastes LLM
// tokens. Walk Query nodes and check the first argument's AST type.
function checkQueryArgs(program, source) {
  const out = [];
  const TAG_REQUIRED_QUERIES = new Set(['nearest', 'exists']);
  // `path`/`raycast`/etc. accept either #Tag (cross-entity form) or a Vec3 (same-entity form).
  // Only `nearest` and `exists` STRICTLY require a #Tag.
  forEachEntityBlock(program, (entityDecl, block) => {
    for (const stmt of block.body) {
      for (const root of stmtExprRoots(stmt)) {
        walkExpr(root, (node) => {
          if (node.type !== 'Query') return;
          if (!TAG_REQUIRED_QUERIES.has(node.name)) return;
          const firstArg = node.args[0];
          if (!firstArg || !firstArg.value) return;
          if (firstArg.value.type === 'TagRef') return;  // OK
          // Got a non-TagRef first arg — emit advisory.
          const gotDesc = firstArg.value.type === 'StringLit' ? 'string literal'
                        : firstArg.value.type === 'Ident' ? 'identifier'
                        : firstArg.value.type === 'NumberLit' ? 'number literal'
                        : firstArg.value.type;
          out.push(makePayload({
            code: 'AX-QUERY-001', severity: 'advisory', entity: entityDecl.name, block: block.name,
            line: node.line || stmt.line, col: node.col || stmt.col,
            rule: { section: 'v0.8.8', title: 'Query Argument' },
            snippet: sourceLine(source, stmt.line),
            human: `First argument to '?${node.name}(...)' must be a #TagRef, got ${gotDesc}.`,
            agent: `'?${node.name}' looks up an entity by tag — use '#Player' instead of '"Player"' or a bare identifier. The tag must be declared with '@Player ...' at the top level.`,
            fix: { kind: 'text', detail: `Replace the first argument with a #TagRef (e.g. '#Player').` },
            autoFixable: false,
          }));
        });
      }
    }
  });
  return out;
}

// v0.8.4 (#1): validate inline #Mesh3D base64 payloads at compile time.
//
// The interpreter's loadProgram calls parseGLBMulti(Buffer) on inline base64 blobs at runtime.
// If the blob is corrupt (truncated base64, wrong magic, wrong glTF version, unparseable JSON,
// no meshes), parseGLBMulti returns null and the entity silently falls back to a procedural
// unit box — the same way a bad FILE path currently does. That fallback is correct for
// "file missing" (the user might be running with a different asset directory), but WRONG for
// "your embedded bytes are garbage": the whole point of inlining is that the bytes are part
// of the source text, so there's no "wrong directory" excuse. A corrupt inline blob is a
// compile-time error, not a runtime fallback.
//
// This check decodes the base64, runs the same magic/version/length pre-flight that
// parseGLBMulti does, and — if those pass — actually calls parseGLBMulti to make sure the
// JSON chunk parses, the BIN chunk is present, and at least one primitive builds. If any of
// those fail, emit AX-MESH-001 (contract_violation — fatal, blocks compilation) with the
// specific failure reason in `message_for_agent`. The interpreter's runtime fallback path
// becomes unreachable in practice; it stays as a defensive guard.
function checkInlineMeshes(program, source) {
  const out = [];
  // parseGLBMulti is a pure function on a Buffer — no fs, no side effects. Safe to call from
  // the checker. The require is lazy so the checker doesn't pull in the whole interpreter at
  // module-load time (which would create a cycle: interpreter requires render3d, which
  // requires... etc.).
  const { parseGLBMulti } = require('./interpreter');
  for (const r of program.resources || []) {
    if (!r.inlineB64) continue;
    if (r.kind !== 'Mesh3D') {
      // base64(...) / glb: heredoc only makes sense for Mesh3D. The parser currently only
      // produces inlineB64 for Mesh3D (other resource kinds always use the path form), but
      // guard against future parser changes anyway.
      out.push(makePayload({
        code: 'AX-MESH-001', severity: 'contract_violation', entity: null, block: null, line: r.line, col: r.col,
        rule: { section: 'v0.8.4', title: 'Inline base64 mesh on non-Mesh3D resource' },
        snippet: sourceLine(source, r.line),
        human: `#${r.kind} ${r.name}: base64(...) / glb: heredoc is only supported on #Mesh3D resources.`,
        agent: `Inline base64 mesh payloads are only meaningful for #Mesh3D (binary glTF). For other resource kinds (Texture, NavMesh3D source, etc.) use the path form: #${r.kind} ${r.name}: "file.ext".`,
        fix: { kind: 'text', detail: `Use the path form: #${r.kind} ${r.name}: "file.ext"` },
      }));
      continue;
    }
    // 1. Base64 decode — Buffer.from throws on invalid alphabet, returns empty/short on truncated.
    let buf;
    try {
      buf = Buffer.from(r.inlineB64, 'base64');
    } catch (e) {
      out.push(makePayload({
        code: 'AX-MESH-001', severity: 'contract_violation', entity: null, block: null, line: r.line, col: r.col,
        rule: { section: 'v0.8.4', title: 'Inline base64 mesh decode failure' },
        snippet: sourceLine(source, r.line),
        human: `#Mesh3D ${r.name}: inline base64 payload failed to decode (${e.message}).`,
        agent: `The base64 string inside base64(...) or the glb: heredoc is not valid base64. Common causes: missing padding (=), non-base64 characters, or a copy-paste that truncated the blob. Raw error: "${e.message}".`,
        fix: { kind: 'text', detail: `Re-encode the .glb bytes with a standard base64 encoder (e.g. \`base64 -w0 hero.glb\`) and paste the result verbatim.` },
      }));
      continue;
    }
    // 2. GLB magic + version + length pre-flight (mirrors parseGLBMulti's header checks).
    if (buf.length < 12) {
      out.push(makePayload({
        code: 'AX-MESH-001', severity: 'contract_violation', entity: null, block: null, line: r.line, col: r.col,
        rule: { section: 'v0.8.4', title: 'Inline base64 mesh too short' },
        snippet: sourceLine(source, r.line),
        human: `#Mesh3D ${r.name}: inline payload is only ${buf.length} bytes — too short for a GLB header (minimum 12).`,
        agent: `A valid GLB starts with a 12-byte header (magic 'glTF' + version + total length). The decoded payload is ${buf.length} bytes, which can't even fit the header. The base64 string was likely truncated.`,
        fix: { kind: 'text', detail: `Re-encode the full .glb file — the current payload is truncated.` },
      }));
      continue;
    }
    const magic = buf.readUInt32LE(0);
    if (magic !== 0x46546C67) {
      out.push(makePayload({
        code: 'AX-MESH-001', severity: 'contract_violation', entity: null, block: null, line: r.line, col: r.col,
        rule: { section: 'v0.8.4', title: 'Inline base64 mesh wrong magic' },
        snippet: sourceLine(source, r.line),
        human: `#Mesh3D ${r.name}: inline payload is not a GLB (magic 0x${magic.toString(16)} ≠ 0x46546c67 'glTF').`,
        agent: `The first 4 bytes of a GLB file must be the ASCII string 'glTF' (little-endian 0x46546c67). The decoded payload starts with 0x${magic.toString(16)} — this is not a GLB. If you embedded a .gltf (JSON) or .obj file, convert it to .glb first; AxiomScript only loads binary glTF.`,
        fix: { kind: 'text', detail: `Convert the source asset to .glb (e.g. \`gltf-pipeline -i hero.gltf -o hero.glb\`) and re-embed.` },
      }));
      continue;
    }
    const version = buf.readUInt32LE(4);
    if (version !== 2) {
      out.push(makePayload({
        code: 'AX-MESH-001', severity: 'contract_violation', entity: null, block: null, line: r.line, col: r.col,
        rule: { section: 'v0.8.4', title: 'Inline base64 mesh wrong glTF version' },
        snippet: sourceLine(source, r.line),
        human: `#Mesh3D ${r.name}: inline payload is glTF version ${version}, only version 2 is supported.`,
        agent: `AxiomScript's parseGLBMulti only handles glTF 2.0 (the current Khronos spec). Version 1 .glb files are extinct in practice; this usually means the .glb was written by a non-standard tool.`,
        fix: { kind: 'text', detail: `Re-export the asset as glTF 2.0 from any modern DCC (Blender, Maya, etc.).` },
      }));
      continue;
    }
    // 3. Full parseGLBMulti — catches corrupt JSON chunks, missing BIN chunk, no meshes,
    // broken accessors, etc. parseGLBMulti is defensive: it returns null on any structural
    // problem rather than throwing, so we just check the return value.
    let prims = null;
    try { prims = parseGLBMulti(buf); } catch (e) { prims = null; }
    if (!prims || prims.length === 0) {
      out.push(makePayload({
        code: 'AX-MESH-001', severity: 'contract_violation', entity: null, block: null, line: r.line, col: r.col,
        rule: { section: 'v0.8.4', title: 'Inline base64 mesh parse failure' },
        snippet: sourceLine(source, r.line),
        human: `#Mesh3D ${r.name}: inline payload has valid GLB magic/version but failed to parse (no meshes, corrupt JSON chunk, or missing BIN chunk).`,
        agent: `The header is valid but the content is structurally broken. parseGLBMulti returned null. Common causes: (1) the JSON chunk doesn't parse (truncated, missing closing brace); (2) the BIN chunk is missing or shorter than the accessors declare; (3) gltf.meshes is empty or missing. Open the .glb in a validator (e.g. https://github.com/KhronosGroup/glTF-Validator) before re-embedding.`,
        fix: { kind: 'text', detail: `Validate the source .glb with glTF-Validator, fix any errors, and re-embed.` },
      }));
      continue;
    }
    // 4. Soft size advisory: if the decoded payload exceeds ~1 MB, warn that the .ax source
    // will be considerably bloated and recommend the path form for large meshes. This is a
    // advisory (non-fatal) — the inline form works fine for any size, it just makes the .ax
    // source harder to edit and slower to parse/lex.
    if (buf.length > 1_000_000) {
      out.push(makePayload({
        code: 'AX-MESH-001', severity: 'advisory', entity: null, block: null, line: r.line, col: r.col,
        rule: { section: 'v0.8.4', title: 'Inline base64 mesh size advisory' },
        snippet: sourceLine(source, r.line),
        human: `#Mesh3D ${r.name}: inline payload is ${(buf.length / 1_000_000).toFixed(1)} MB — consider using the file-based form for large meshes.`,
        agent: `Inline base64 bloats the .ax source by ~1.33× the binary size (base64 encoding overhead) and makes the source harder to edit/diff. For environment meshes or detailed characters, use the path form \`#Mesh3D ${r.name}: "${r.name.toLowerCase()}.glb"\` and ship the .glb alongside the .ax. Inline is best for small props and characters (< 1 MB).`,
        fix: { kind: 'text', detail: `Move the .glb to a separate file and use \`#Mesh3D ${r.name}: "${r.name.toLowerCase()}.glb"\`.` },
      }));
    }
  }
  return out;
}

// v0.8.4 (#2 / Bug #8): detect cross-entity writes (`#Tag.field = expr`) where `field` is
// neither a declared ~field on the target entity NOR a reserved transform-property name
// (pos/rot/scl/vel/pose). Before this check, such writes silently landed in `Tag.locals` as
// a dead key — the writer thought it was mutating the target, the target never saw it, no
// diagnostic fired. The same class of silent-correctness bug as #3/#4/#6/#7 from earlier
// rounds (compile clean, run with zero runtime diagnostics, do nothing).
//
// The fix routes pos/rot/scl/vel through `EntityInstance.set` to the live `pose` Transform
// (see interpreter.js Bug #8 fix). This check catches everything ELSE: typos like
// `#Target.socre = 5` (instead of `score`), or writes to read-only reserved names like
// `#Tag.wpose = ...` (computed from parent chain — not writable).
//
// The check is advisory (non-fatal): `compile().ok` stays true. The runtime still writes to
// locals for backward compat (in case some .ax file depends on the silent-local behavior —
// unlikely, but breaking it would be a separate breaking change). The advisory surfaces the
// problem so the user notices immediately, instead of debugging "why doesn't this work".
const RESERVED_XWRITE_PROPS = new Set(['pos', 'rot', 'scl', 'vel', 'pose']);
function checkCrossEntityWrites(program, source) {
  const out = [];
  // Build map: tag name → Set of declared ~field names. Statically-declared entities only —
  // dynamically-spawned entities (via !spawn) aren't in the map and are skipped (the check
  // can't reason about them; the runtime still applies the reserved-name routing).
  const tagFields = new Map();
  const visit = (entityDecl) => {
    const fields = new Set();
    for (const m of entityDecl.members) {
      if (m.type === 'FieldDecl' && m.sigil === '~') fields.add(m.name);
      // v0.8.2: `~field!: value` is a noWarn override marker — still a declared field.
      if (m.type === 'FieldDecl' && m.sigil === '~' && m.noWarn) fields.add(m.name);
    }
    tagFields.set(entityDecl.name, fields);
    for (const m of entityDecl.members) {
      if (m.type === 'EntityDecl') visit(m);
    }
  };
  for (const e of program.entities) visit(e);

  forEachEntityBlock(program, (entityDecl, block) => {
    for (const stmt of walkStmts(block.body)) {
      // MemberAssign with stmt.tag set = single-level cross-entity write (#Tag.field = ...).
      if (stmt.type === 'MemberAssign' && stmt.tag) {
        const targetFields = tagFields.get(stmt.tag);
        if (!targetFields) continue; // tag not statically known — skip
        if (targetFields.has(stmt.prop)) continue; // declared field — OK
        if (RESERVED_XWRITE_PROPS.has(stmt.prop)) continue; // reserved (now routed to pose) — OK
        out.push(makePayload({
          code: 'AX-XWRITE-001', severity: 'advisory', entity: entityDecl.name, block: block.name, line: stmt.line, col: stmt.col,
          rule: { section: 'v0.8.4', title: 'Cross-entity write to undeclared field' },
          snippet: sourceLine(source, stmt.line),
          human: `'#${stmt.tag}.${stmt.prop} = ...' writes to an undeclared field on @${stmt.tag} — it will silently land in @${stmt.tag}'s locals as a dead key nothing reads.`,
          agent: `@${stmt.tag} has no ~${stmt.prop} field declared, and '${stmt.prop}' is not a reserved transform-property name (pos/rot/scl/vel/pose). The write executes (no crash) but the value goes into @${stmt.tag}.locals, which nothing reads — the visible state of @${stmt.tag} is unchanged. This is the same class of silent-correctness bug as v0.8.3 bugs #3/#4/#6/#7. Either declare ~${stmt.prop} on @${stmt.tag}, fix the typo, or use one of the reserved names (pos/rot/scl/vel route to the live pose Transform; pose replaces the whole Transform).`,
          fix: { kind: 'text', detail: `Add \`~${stmt.prop}: <default>\` to @${stmt.tag}'s body, or fix the field name typo.` },
        }));
      }
      // DeepAssign with stmt.tag set = multi-level cross-entity write (#Tag.field.path = ...).
      // The FIRST path element is what's looked up on the target entity; if it's not declared
      // and not reserved, the subsequent path walk dereferences `undefined` and throws at
      // runtime (AX-RUNTIME-MEMBER). Catch it statically here.
      if (stmt.type === 'DeepAssign' && stmt.tag && stmt.path.length >= 1) {
        const targetFields = tagFields.get(stmt.tag);
        if (!targetFields) continue;
        const first = stmt.path[0];
        if (targetFields.has(first)) continue;
        if (RESERVED_XWRITE_PROPS.has(first)) continue;
        out.push(makePayload({
          code: 'AX-XWRITE-001', severity: 'advisory', entity: entityDecl.name, block: block.name, line: stmt.line, col: stmt.col,
          rule: { section: 'v0.8.4', title: 'Cross-entity write to undeclared field' },
          snippet: sourceLine(source, stmt.line),
          human: `'#${stmt.tag}.${stmt.path.join('.')} = ...' writes through an undeclared field on @${stmt.tag} — the path walk will dereference undefined at runtime.`,
          agent: `@${stmt.tag} has no ~${first} field, and '${first}' is not a reserved transform-property name (pos/rot/scl/vel/pose). The cross-entity deep write walks obj.get('${first}') → undefined, then tries to dereference undefined.${stmt.path[1] || '…'} → AX-RUNTIME-MEMBER. Declare ~${first} on @${stmt.tag} or fix the path.`,
          fix: { kind: 'text', detail: `Add \`~${first}: <default>\` to @${stmt.tag}'s body, or fix the path.` },
        }));
      }
    }
  });
  return out;
}

// -----------------------------------------------------------------------------
// v0.9.1 — THE STATIC SAFETY NET
//
// Everything in this section exists for one reason: an error the checker reports costs the
// model a line of context, while an error that survives to runtime costs an entire
// generate-run-fail-regenerate cycle. These passes catch the four mistakes that were still
// getting through — a call with the wrong number of arguments, a field name that does not
// exist on a record, a mistyped variable (which the language silently turns into an atom),
// and a literal that contradicts a declared type.
//
// All four are ADVISORY. Each has a conservative trigger: they fire only where the code cannot
// be right, never where it is merely unusual, because a false positive makes a model rewrite
// working code — strictly worse than the miss it would have prevented.

// Names a pattern binds. Mirrors matchPatternInner in interpreter.js: inside a pattern a bare
// name binds, `_` binds nothing, a declared ^type name matches by type, and literals compare.
function patternBindings(patNode, typeNames, out, topLevel) {
  if (!patNode || typeof patNode !== 'object') return out;
  switch (patNode.type) {
    case 'Ident':
      if (patNode.name === '_' || topLevel) return out;   // top level compares, never binds
      if (typeNames.has(patNode.name)) return out;
      out.add(patNode.name);
      return out;
    case 'Call':
      if (!typeNames.has(patNode.callee)) return out;
      for (const a of patNode.args || []) if (a && a.value) patternBindings(a.value, typeNames, out, false);
      return out;
    case 'ArrayLit':
      for (const e of patNode.elements || []) patternBindings(e, typeNames, out, false);
      return out;
    case 'DictLit':
      for (const pr of patNode.pairs || []) patternBindings(pr.value, typeNames, out, false);
      return out;
    default:
      return out;
  }
}

// Every name a statement list binds, wherever it binds it. Flow-insensitive on purpose: a name
// assigned anywhere in the body counts as known everywhere in it, so the checks below never
// depend on statement order (which they would get wrong for loops and early returns).
function collectBoundNames(stmts, typeNames, out) {
  for (const st of stmts || []) {
    if (!st || typeof st !== 'object') continue;
    if (st.type === 'Assign' && st.target) out.add(st.target);
    if (st.type === 'DestructureAssign') for (const n of st.names || []) out.add(n);
    if (st.type === 'ForLoop') { if (st.varName) out.add(st.varName); for (const v of st.vars || []) out.add(v); }
    if (st.type === 'Try' && st.catchVar) out.add(st.catchVar);
    if (st.type === 'MemberAssign' && st.obj) out.add(st.obj);
    if (st.type === 'DeepAssign' && st.path && st.path.length) out.add(st.path[0]);
    if (st.type === 'IndexAssign' && st.obj) out.add(st.obj);
    for (const key of ['body', 'ifBody', 'elseBody', 'catchBody', 'finallyBody']) {
      if (Array.isArray(st[key])) collectBoundNames(st[key], typeNames, out);
    }
    for (const arm of st.arms || []) {
      for (const pat of arm.patterns || []) patternBindings(pat, typeNames, out, true);
      if (arm.guard && arm.patterns && arm.patterns.length === 1 && arm.patterns[0].type === 'Ident'
          && arm.patterns[0].name !== '_' && !typeNames.has(arm.patterns[0].name)) {
        out.add(arm.patterns[0].name);       // guarded capture: `n if n > 10:`
      }
      collectBoundNames(arm.body, typeNames, out);
    }
    for (const root of stmtExprRoots(st)) {
      walkExpr(root, (n) => {
        if (n.type === 'Lambda') for (const pn of n.params || []) out.add(pn);
        if (n.type === 'Comprehension') for (const v of n.vars || []) out.add(v);
      });
    }
  }
  return out;
}

// Signature of every callable the program declares, for the arity check.
function declaredSignatures(program) {
  const sigs = new Map();
  for (const fn of program.fns || []) sigs.set(fn.name, { kind: '^fn', params: fn.params || [], defaults: fn.defaults || {}, decl: fn });
  for (const pr of program.procs || []) sigs.set(pr.name, { kind: '^proc', params: pr.params || [], defaults: pr.defaults || {}, decl: pr });
  return sigs;
}

// Walk every statement list in the program — entity blocks, functions, procedures, and ^main —
// handing each to `fn` with a label and the names in scope around it.
function forEachBody(program, fn) {
  const typeNames = new Set((program.types || []).map(t => t.name));
  forEachEntityBlock(program, (entityDecl, block) => {
    const scopeNames = new Set();
    for (const m of entityDecl.members || []) if (m.type === 'FieldDecl') scopeNames.add(m.name);
    // Mixin fields land on the entity at composition time, so they are in scope here too.
    for (const inc of entityDecl.mixins || []) {
      const mix = (program.mixins || []).find(m => m.name === inc);
      for (const m of (mix && mix.members) || []) if (m.type === 'FieldDecl') scopeNames.add(m.name);
    }
    // An `&on(Event)` block reads the event's payload fields as bare names.
    if (block.name === 'on' && block.eventArg) {
      const ev = (program.events || []).find(e => e.name === block.eventArg);
      for (const f of (ev && ev.fields) || []) scopeNames.add(f.name);
      scopeNames.add('source');
    }
    fn({ body: block.body, label: `&${block.name}`, entity: entityDecl, block, scopeNames, typeNames, inEntity: true });
  });
  for (const f of program.fns || []) fn({ body: f.body, label: `^fn ${f.name}`, decl: f, scopeNames: new Set(f.params || []), typeNames, inEntity: false });
  for (const p of program.procs || []) fn({ body: p.body, label: `^proc ${p.name}`, decl: p, scopeNames: new Set(p.params || []), typeNames, inEntity: false });
  if (program.main) fn({ body: program.main.body, label: '^main', decl: program.main, scopeNames: new Set(program.main.params || []), typeNames, inEntity: false });
}

// --- AX-ARITY-001: a call with the wrong number of arguments -------------------------------
// Calling `^fn f(a, b)` with one argument binds `b` to null and fails somewhere else entirely;
// calling it with three silently drops the third. Both are always bugs, and both are invisible
// at runtime until the wrong value propagates.
function checkArity(program, source) {
  const out = [];
  const sigs = declaredSignatures(program);
  if (!sigs.size) return out;
  forEachBody(program, ({ body, label, entity, block }) => {
    const visit = (stmts) => {
      for (const st of stmts || []) {
        // `!name(args)` in statement position calls a ^proc/^fn too.
        if (st.type === 'Action' && sigs.has(st.name)) report(st.name, (st.args || []).length, st);
        for (const root of stmtExprRoots(st)) {
          walkExpr(root, (node) => {
            if (node.type === 'Call' && sigs.has(node.callee)) report(node.callee, (node.args || []).length, node, st);
          });
        }
        for (const key of ['body', 'ifBody', 'elseBody', 'catchBody', 'finallyBody']) if (Array.isArray(st[key])) visit(st[key]);
        for (const arm of st.arms || []) visit(arm.body);
      }
    };
    const report = (name, given, node, st) => {
      const sig = sigs.get(name);
      const required = sig.params.filter(p => !(p in sig.defaults)).length;
      const total = sig.params.length;
      if (given >= required && given <= total) return;
      // A function that reads `args` is deliberately variadic — never flag it.
      if (given > total && bodyReadsArgs(sig.decl)) return;
      const line = (node && node.line) || (st && st.line) || (sig.decl && sig.decl.line);
      const shape = required === total ? `${total}` : `${required}–${total}`;
      out.push(makePayload({
        code: 'AX-ARITY-001', severity: 'advisory', entity: entity ? entity.name : null, block: block ? block.name : label,
        line, col: (node && node.col) || (st && st.col) || null,
        rule: { section: 'v0.9.1', title: 'Wrong number of arguments' },
        snippet: sourceLine(source, line),
        human: `'${name}(...)' takes ${shape} argument${required === 1 && total === 1 ? '' : 's'}, but ${given === 1 ? '1 was' : `${given} were`} given.`,
        agent: `${sig.kind} ${name}(${sig.params.join(', ')}) expects ${shape} argument(s); this call passes ${given}. Missing arguments bind to null (or their default) and extra ones are dropped, so the failure will surface far from here. Fix the call, or give the parameter a default (\`${sig.params[given] || 'x'} = <value>\`) if it is meant to be optional.`,
        fix: null, autoFixable: false,
      }));
    };
    visit(body);
  });
  return out;
}

// True when a function reads its `args` array — the language's variadic escape hatch.
function bodyReadsArgs(decl) {
  if (!decl || !decl.body) return false;
  let found = false;
  const visit = (stmts) => {
    for (const st of stmts || []) {
      for (const root of stmtExprRoots(st)) walkExpr(root, (n) => { if (n.type === 'Ident' && n.name === 'args') found = true; });
      for (const key of ['body', 'ifBody', 'elseBody', 'catchBody', 'finallyBody']) if (Array.isArray(st[key])) visit(st[key]);
      for (const arm of st.arms || []) visit(arm.body);
    }
  };
  visit(decl.body);
  return found;
}

// --- AX-FIELD-001: a field that the record type does not declare ---------------------------
// Only fires for a local assigned exactly once from a `^type` constructor and never reassigned,
// so the variable's shape is certain. `p = Point(1, 2)` then `p.z` is a guaranteed null.
function checkRecordFields(program, source) {
  const out = [];
  const types = new Map((program.types || []).map(t => [t.name, new Set((t.fields || []).map(f => f.name))]));
  if (!types.size) return out;
  forEachBody(program, ({ body, label, entity, block, typeNames }) => {
    const assignedType = new Map();   // var -> type name  (null marks "not certain")
    const noteAssign = (stmts) => {
      for (const st of stmts || []) {
        if (st.type === 'Assign' && st.target) {
          const v = st.value;
          const t = (v && v.type === 'Call' && types.has(v.callee)) ? v.callee : null;
          if (assignedType.has(st.target)) assignedType.set(st.target, null);   // reassigned: give up
          else assignedType.set(st.target, t);
        }
        if (st.type === 'DestructureAssign') for (const n of st.names || []) assignedType.set(n, null);
        if (st.type === 'ForLoop') { if (st.varName) assignedType.set(st.varName, null); for (const v of st.vars || []) assignedType.set(v, null); }
        for (const key of ['body', 'ifBody', 'elseBody', 'catchBody', 'finallyBody']) if (Array.isArray(st[key])) noteAssign(st[key]);
        for (const arm of st.arms || []) noteAssign(arm.body);
      }
    };
    noteAssign(body);
    const visit = (stmts) => {
      for (const st of stmts || []) {
        for (const root of stmtExprRoots(st)) {
          walkExpr(root, (node) => {
            if (node.type !== 'Member' && node.type !== 'MethodCall') return;
            if (!node.obj || node.obj.type !== 'Ident') return;
            const tname = assignedType.get(node.obj.name);
            if (!tname) return;
            const fields = types.get(tname);
            const prop = node.type === 'Member' ? node.prop : node.method;
            if (fields.has(prop)) return;
            if (node.type === 'MethodCall') return;      // a field may hold a function; don't guess
            const line = node.line || st.line;
            const known = [...fields];
            let best = null, bestD = Infinity;
            for (const f of known) { const d = levenshtein(prop, f); if (d < bestD) { bestD = d; best = f; } }
            out.push(makePayload({
              code: 'AX-FIELD-001', severity: 'advisory', entity: entity ? entity.name : null, block: block ? block.name : label,
              line, col: node.col || st.col,
              rule: { section: 'v0.9.1', title: 'Unknown field on a record' },
              snippet: sourceLine(source, line),
              human: `'${node.obj.name}' is a ${tname}, which has no field '${prop}'${bestD <= 3 ? ` — did you mean '${best}'?` : '.'}`,
              agent: `^type ${tname} declares: ${known.join(', ')}. Reading '${prop}' yields null, which will surface as a confusing failure later. Fix the field name, or add '${prop}' to the ^type declaration.`,
              fix: bestD <= 3 ? { kind: 'text', detail: `Replace '.${prop}' with '.${best}'.` } : null,
              autoFixable: false,
            }));
          });
        }
        for (const key of ['body', 'ifBody', 'elseBody', 'catchBody', 'finallyBody']) if (Array.isArray(st[key])) visit(st[key]);
        for (const arm of st.arms || []) visit(arm.body);
      }
    };
    visit(body);
  });
  return out;
}

// --- AX-UNDEF-VAR-001: a name that is nothing, used where an atom cannot be meant -----------
// An unbound identifier evaluates to an ATOM (`state = idle` is the language's enum idiom), so
// a mistyped variable name has always been silently legal: `helth - 10` produces NaN, not an
// error. This pass restores the diagnostic without giving up atoms, by reporting only the
// positions where an atom is definitionally wrong — arithmetic, ordering comparisons, indexing,
// and member access.
const ATOM_HOSTILE_OPS = new Set(['+', '-', '*', '/', '%', '**', '>', '<', '>=', '<=']);
const IMPLICIT_NAMES = new Set([
  'dt', 'self', 'null', 'true', 'false', 'input', 'args', 'it',
  'pose', 'wpose', 'pos', 'vel', 'rot', 'scl', 'position', 'velocity', 'facing',
]);
function checkUnknownNames(program, source) {
  const out = [];
  const globalNames = new Set();
  for (const g of program.globals || []) for (const n of g.names || []) globalNames.add(n);
  for (const f of program.fns || []) globalNames.add(f.name);
  for (const p of program.procs || []) globalNames.add(p.name);
  for (const t of program.types || []) globalNames.add(t.name);
  for (const m of program.materials || []) globalNames.add(m.name);

  forEachBody(program, ({ body, label, entity, block, scopeNames, typeNames }) => {
    const known = new Set([...scopeNames, ...globalNames, ...IMPLICIT_NAMES, ...KNOWN_INTRINSIC_NAMES]);
    collectBoundNames(body, typeNames, known);
    const suspect = new Map();   // name -> first node seen in a hostile position
    const note = (node, st) => {
      if (!node || node.type !== 'Ident' || node.sigil === '$') return;
      if (known.has(node.name)) return;
      if (!suspect.has(node.name)) suspect.set(node.name, { node, st });
    };
    const visit = (stmts) => {
      for (const st of stmts || []) {
        for (const root of stmtExprRoots(st)) {
          walkExpr(root, (node) => {
            if (node.type === 'Binary' && ATOM_HOSTILE_OPS.has(node.op)) { note(node.left, st); note(node.right, st); }
            else if (node.type === 'Unary' && node.op === '-') note(node.expr, st);
            else if (node.type === 'Index') note(node.obj, st);
            else if (node.type === 'Member' || node.type === 'MethodCall') note(node.obj, st);
          });
        }
        for (const key of ['body', 'ifBody', 'elseBody', 'catchBody', 'finallyBody']) if (Array.isArray(st[key])) visit(st[key]);
        for (const arm of st.arms || []) visit(arm.body);
      }
    };
    visit(body);
    for (const [name, { node, st }] of suspect) {
      let best = null, bestD = Infinity;
      for (const cand of known) { const d = levenshtein(name, cand); if (d < bestD) { bestD = d; best = cand; } }
      const line = node.line || st.line;
      out.push(makePayload({
        code: 'AX-UNDEF-VAR-001', severity: 'advisory', entity: entity ? entity.name : null, block: block ? block.name : label,
        line, col: node.col || st.col,
        rule: { section: 'v0.9.1', title: 'Unknown name used as a value' },
        snippet: sourceLine(source, line),
        human: `'${name}' is not declared anywhere${bestD <= 3 ? ` (did you mean '${best}'?)` : ''} — used like this it becomes the atom \`${name}\`, not a value.`,
        agent: `An identifier that is not a parameter, local, loop variable, field, global, or standard-library name evaluates to an ATOM (the language's symbol type, as in \`state = idle\`). Here '${name}' is used in arithmetic, an ordering comparison, an index, or a member access, where an atom can never be right — arithmetic on it yields NaN and member access yields null. Declare '${name}', or correct the spelling${bestD <= 3 ? ` (closest known name: '${best}')` : ''}.`,
        fix: bestD <= 3 ? { kind: 'text', detail: `Replace '${name}' with '${best}'.` } : null,
        autoFixable: false,
      }));
    }
  });
  return out;
}

// --- AX-TYPE-001: a literal that contradicts a declared type -------------------------------
// The language is dynamically typed and the annotations on ^type fields and ^fn returns are
// documentation. Where BOTH sides are known statically — a literal against a declared type —
// checking costs nothing and cannot produce a false positive.
const LITERAL_KINDS = { NumberLit: 'number', StringLit: 'string', FString: 'string', ArrayLit: 'array', DictLit: 'dict', Lambda: 'fn' };
const TYPE_ALIASES = { number: 'number', num: 'number', int: 'number', float: 'number', string: 'string', str: 'string', text: 'string', bool: 'bool', array: 'array', list: 'array', dict: 'dict', map: 'dict', fn: 'fn' };
function literalKind(node) {
  if (!node) return null;
  if (node.type === 'Ident' && (node.name === 'true' || node.name === 'false')) return 'bool';
  return LITERAL_KINDS[node.type] || null;
}
function checkLiteralTypes(program, source) {
  const out = [];
  const types = new Map((program.types || []).map(t => [t.name, t]));
  const report = (declared, node, kind, whatHuman, whatAgent, line, col, entity, block) => {
    out.push(makePayload({
      code: 'AX-TYPE-001', severity: 'advisory', entity, block, line, col,
      rule: { section: 'v0.9.1', title: 'Literal contradicts a declared type' },
      snippet: sourceLine(source, line),
      human: `${whatHuman} is declared \`${declared}\` but the value here is a ${kind}.`,
      agent: `${whatAgent} Either pass a ${declared}, change the declared type, or drop the annotation — annotations are documentation, so the runtime will not stop this, but the mismatch is almost always a real mistake.`,
      fix: null, autoFixable: false,
    }));
  };
  // Record construction: P(1, "two") against ^type P: x:: number, y:: number
  forEachBody(program, ({ body, entity, block, label }) => {
    const visit = (stmts) => {
      for (const st of stmts || []) {
        for (const root of stmtExprRoots(st)) {
          walkExpr(root, (node) => {
            if (node.type !== 'Call' || !types.has(node.callee)) return;
            const decl = types.get(node.callee);
            let pos = 0;
            for (const a of node.args || []) {
              if (!a || !a.value) continue;
              const field = a.name ? (decl.fields || []).find(f => f.name === a.name) : (decl.fields || [])[pos++];
              if (!field || !field.ftype) continue;
              const want = TYPE_ALIASES[String(field.ftype)];
              const kind = literalKind(a.value);
              if (!want || !kind || want === kind) continue;
              report(field.ftype, a.value, kind, `Field '${field.name}' of ${node.callee}`,
                `^type ${node.callee} declares ${field.name}:: ${field.ftype}, and this call passes a ${kind} literal.`,
                node.line || st.line, node.col || st.col, entity ? entity.name : null, block ? block.name : label);
            }
          });
        }
        for (const key of ['body', 'ifBody', 'elseBody', 'catchBody', 'finallyBody']) if (Array.isArray(st[key])) visit(st[key]);
        for (const arm of st.arms || []) visit(arm.body);
      }
    };
    visit(body);
  });
  // Declared return type against a literal `^return`.
  for (const fn of program.fns || []) {
    if (!fn.retType) continue;
    const want = TYPE_ALIASES[String(fn.retType)];
    if (!want) continue;
    const visit = (stmts) => {
      for (const st of stmts || []) {
        if (st.type === 'Return' && st.expr) {
          const kind = literalKind(st.expr);
          if (kind && kind !== want) {
            report(fn.retType, st.expr, kind, `The return value of ^fn ${fn.name}`,
              `^fn ${fn.name} is declared -> ${fn.retType}, but this ^return yields a ${kind} literal.`,
              st.line, st.col, null, `^fn ${fn.name}`);
          }
        }
        for (const key of ['body', 'ifBody', 'elseBody', 'catchBody', 'finallyBody']) if (Array.isArray(st[key])) visit(st[key]);
        for (const arm of st.arms || []) visit(arm.body);
      }
    };
    visit(fn.body);
  }
  return out;
}

// -----------------------------------------------------------------------------
// v0.9.0 — `^use "lib.ax"` module resolution.
//
// Imports are textual and include-once: every top-level declaration of the imported file is
// added to the importing program unless a declaration of that name already exists locally
// (local wins, and the shadowing is reported as an advisory). Cycles terminate because each
// resolved absolute path is visited at most once.
//
// Why textual rather than namespaced: a namespace qualifier costs two tokens at every call
// site, and the programs this language is written for are small. A flat namespace plus a
// duplicate-name diagnostic buys the same safety for zero tokens.
function resolveUses(program, opts) {
  const fs = require('fs');
  const path = require('path');
  const out = [];
  if (!program || !(program.uses || []).length) return out;
  const baseDir = opts && opts.filename ? path.dirname(path.resolve(opts.filename)) : process.cwd();
  const visited = new Set();
  if (opts && opts.filename) visited.add(path.resolve(opts.filename));

  const mergeDecls = (target, source, fromPath) => {
    const lists = [
      ['fns', (d) => d.name], ['procs', (d) => d.name], ['types', (d) => d.name],
      ['events', (d) => d.name], ['mixins', (d) => d.name], ['materials', (d) => d.name],
      ['entities', (d) => d.name], ['resources', (d) => d.name],
      // v0.9.0: a library's `~NAME: value` globals come across too — a constants module is
      // one of the most useful things to import.
      ['globals', (d) => (d.names || []).join(',')],
    ];
    for (const [key, nameOf] of lists) {
      target[key] = target[key] || [];
      const have = new Set(target[key].map(nameOf));
      for (const decl of source[key] || []) {
        const nm = nameOf(decl);
        if (have.has(nm)) {
          out.push(makePayload({
            code: 'AX-USE-002', severity: 'advisory', entity: null, block: null,
            line: decl.line || null, col: decl.col || null,
            rule: { section: 'v0.9.0', title: 'Imported declaration shadowed' },
            snippet: null,
            human: `'${nm}' is declared both locally and in '${fromPath}' — the local declaration wins.`,
            agent: `The import '${fromPath}' declares '${nm}', which this file also declares. AxiomScript imports share one flat namespace and the importing file takes precedence. Rename one of them if the shadowing was not intentional.`,
            fix: null, autoFixable: false,
          }));
          continue;
        }
        have.add(nm);
        decl._fromFile = decl._fromFile || fromPath;
        target[key].push(decl);
      }
    }
  };

  const visit = (prog, dir) => {
    for (const u of prog.uses || []) {
      for (const rel of u.paths) {
        let resolved = path.resolve(dir, rel);
        if (!fs.existsSync(resolved) && fs.existsSync(resolved + '.ax')) resolved += '.ax';
        if (visited.has(resolved)) continue;
        if (!fs.existsSync(resolved)) {
          out.push(makePayload({
            code: 'AX-USE-001', severity: 'fatal', entity: null, block: null, line: u.line, col: u.col,
            rule: { section: 'v0.9.0', title: 'Import not found' },
            snippet: null,
            human: `^use "${rel}" — no such file (looked in ${dir}).`,
            agent: `The import path is resolved relative to the importing file's directory, and '.ax' is appended if the literal path does not exist. Create '${rel}' or fix the path.`,
            fix: null, autoFixable: false,
          }));
          continue;
        }
        visited.add(resolved);
        let subSrc, sub;
        try { subSrc = fs.readFileSync(resolved, 'utf8'); } catch (e) {
          out.push(makePayload({
            code: 'AX-USE-001', severity: 'fatal', entity: null, block: null, line: u.line, col: u.col,
            rule: { section: 'v0.9.0', title: 'Import unreadable' }, snippet: null,
            human: `^use "${rel}" — could not read the file: ${e.message}`,
            agent: `Check permissions on '${resolved}'.`, fix: null, autoFixable: false,
          }));
          continue;
        }
        try { sub = parse(subSrc); } catch (e) {
          out.push(makePayload({
            code: 'AX-USE-003', severity: 'fatal', entity: null, block: null, line: u.line, col: u.col,
            rule: { section: 'v0.9.0', title: 'Import failed to parse' }, snippet: null,
            human: `^use "${rel}" — the imported file has a parse error: ${e.message}`,
            agent: `Fix the syntax error inside '${rel}' (its own line numbers apply), then re-check this file.`,
            fix: null, autoFixable: false,
          }));
          continue;
        }
        for (const perr of sub.parseErrors || []) {
          out.push(makePayload({
            code: 'AX-USE-003', severity: 'fatal', entity: null, block: null, line: u.line, col: u.col,
            rule: { section: 'v0.9.0', title: 'Import failed to parse' }, snippet: null,
            human: `^use "${rel}" (line ${perr.line}): ${perr.message}`,
            agent: `Fix the syntax error inside '${rel}' at its line ${perr.line}.`,
            fix: null, autoFixable: false,
          }));
        }
        // Depth first, so a library's own imports are available to it.
        visit(sub, path.dirname(resolved));
        mergeDecls(program, sub, rel);
        if (sub.main) {
          out.push(makePayload({
            code: 'AX-USE-004', severity: 'advisory', entity: null, block: null, line: u.line, col: u.col,
            rule: { section: 'v0.9.0', title: 'Imported ^main ignored' }, snippet: null,
            human: `'${rel}' declares a ^main — only the entry file's ^main runs.`,
            agent: `A ^main in an imported file is ignored. Move the shared code into a ^fn/^proc so both programs can call it.`,
            fix: null, autoFixable: false,
          }));
        }
      }
    }
  };
  visit(program, baseDir);
  return out;
}

// v0.9.0 — a program must have something to run: a ^main (script) or at least one entity
// (simulation). A file that declares only functions is a library, which is legitimate when it
// is imported, so this is an advisory rather than an error.
function checkEntryPoint(program, source) {
  if (program.main) return [];
  if ((program.entities || []).length) return [];
  const hasDecls = (program.fns || []).length || (program.procs || []).length || (program.types || []).length;
  if (!hasDecls) return [];
  return [makePayload({
    code: 'AX-MAIN-001', severity: 'advisory', entity: null, block: null, line: 1, col: 1,
    rule: { section: 'v0.9.0', title: 'No entry point' },
    snippet: sourceLine(source, 1),
    human: `This program declares no ^main and no entities, so running it does nothing.`,
    agent: `Add '^main:' with the statements to run, or declare an @Entity with a &tick/&physics block. This is expected for a library file that another program imports with ^use.`,
    fix: { kind: 'text', detail: `Add a '^main:' block, or import this file from one that has one.` },
    autoFixable: false,
  })];
}

// v0.9.0 — undefined-function detection extended to ^fn/^proc/^main bodies. Parameters and
// locals holding callables are excluded, so a callback invoked by name is never flagged.
function checkUndefinedFunctionsInBodies(program, source) {
  const out = [];
  const declared = new Set();
  for (const fn of program.fns || []) declared.add(fn.name);
  for (const pr of program.procs || []) declared.add(pr.name);
  for (const t of program.types || []) declared.add(t.name);

  const scan = (decl, label) => {
    // Names bound inside the body: parameters, assignment targets, loop variables, lambda
    // parameters, catch variables. Any of these may hold a function.
    const bound = new Set(decl.params || []);
    bound.add('args');
    const collectBindings = (stmts) => {
      for (const st of stmts || []) {
        if (!st || typeof st !== 'object') continue;
        if (st.type === 'Assign' && st.target) bound.add(st.target);
        if (st.type === 'DestructureAssign') for (const n of st.names || []) bound.add(n);
        if (st.type === 'ForLoop') { if (st.varName) bound.add(st.varName); for (const v of st.vars || []) bound.add(v); }
        if (st.type === 'Try' && st.catchVar) bound.add(st.catchVar);
        for (const key of ['body', 'ifBody', 'elseBody', 'catchBody', 'finallyBody']) {
          if (Array.isArray(st[key])) collectBindings(st[key]);
        }
        for (const arm of st.arms || []) collectBindings(arm.body);
      }
    };
    collectBindings(decl.body);
    const walkStmts = (stmts) => {
      for (const st of stmts || []) {
        for (const root of stmtExprRoots(st)) {
          walkExpr(root, (node) => {
            if (node.type === 'Lambda') { for (const pn of node.params || []) bound.add(pn); return; }
            if (node.type !== 'Call' || !node.callee) return;
            if (KNOWN_INTRINSIC_NAMES.has(node.callee) || declared.has(node.callee) || bound.has(node.callee)) return;
            out.push(makePayload({
              code: 'AX-UNDEF-FN-001', severity: 'advisory', entity: null, block: label,
              line: node.line || st.line, col: node.col || st.col,
              rule: { section: 'v0.9.0', title: 'Undefined Function' },
              snippet: sourceLine(source, st.line),
              human: `Call to undefined function '${node.callee}(...)' in ${label}.`,
              agent: `'${node.callee}' is not a declared ^fn/^proc/^type, not a local holding a function, and not a standard-library name. Declare it, or check the spelling against the standard library (see STDLIB.md).`,
              fix: null, autoFixable: false,
            }));
          });
        }
        for (const key of ['body', 'ifBody', 'elseBody', 'catchBody', 'finallyBody']) {
          if (Array.isArray(st[key])) walkStmts(st[key]);
        }
        for (const arm of st.arms || []) walkStmts(arm.body);
      }
    };
    walkStmts(decl.body);
  };

  for (const fn of program.fns || []) scan(fn, `^fn ${fn.name}`);
  for (const pr of program.procs || []) scan(pr, `^proc ${pr.name}`);
  if (program.main) scan(program.main, '^main');
  return out;
}

// -----------------------------------------------------------------------------
function parseErrorPayload(err, source) {
  const line = err && err.line ? err.line : null;
  const col = err && err.col ? err.col : null;
  return makePayload({
    code: 'AX-PARSE-000', severity: 'fatal', entity: null, block: null, line, col,
    rule: RULE.PARSE, snippet: sourceLine(source, line),
    human: `Parse error: ${err.message}`,
    agent: `Fix syntax at or near this line.`,
    fix: null, autoFixable: false,
  });
}

// §3 Rule ("batch, don't trickle")
// v0.9.0: `compile(source, opts)`.
//   opts.filename   — path of the source, used to resolve `^use` imports (and reported in
//                     diagnostics). Without it, imports resolve relative to the process cwd.
//   opts.imports    — set false to skip import resolution (used by tools that only want to
//                     check one file in isolation).
function compile(source, opts) {
  opts = opts || {};
  let program;
  try {
    program = parse(source);
  } catch (err) {
    return { ok: false, program: null, diagnostics: [parseErrorPayload(err, source)] };
  }
  // Imports are spliced in BEFORE the semantic passes so that a function defined in an
  // imported file counts as declared everywhere it is used.
  const useDiagnostics = opts.imports === false ? [] : resolveUses(program, opts);
  // v0.8.8: surface recovered parse errors as diagnostics. The parser now collects errors
  // instead of throwing on the first one — program.parseErrors is an array of ParseError
  // instances. Each becomes an AX-PARSE-000 diagnostic. The program is still returned
  // (partially parsed) so semantic checks can run on the valid portions.
  const parseErrorPayloads = (program.parseErrors || []).map(err => parseErrorPayload(err, source));
  const diagnostics = [
    // v0.8.8: parse errors first (recovery mode — these were collected during parsing, not thrown).
    ...parseErrorPayloads,
    // v0.9.0: import resolution problems rank with parse errors — nothing downstream is
    // meaningful if a module is missing.
    ...useDiagnostics,
    ...checkZeroAlloc(program, source),
    ...checkSealedBody(program, source),
    ...checkEvents(program, source),
    ...checkInferStrategies(program, source),
    ...checkPoolTypes(program, source),
    ...checkFunctions(program, source),
    ...checkLoopControl(program, source),
    ...checkMixins(program, source),
    ...checkUnknownBlocks(program, source),
    ...checkFunctionOnlyInActionPosition(program, source),
    ...checkBroadcastNoListeners(program, source),
    ...checkVersionPragma(program, source),
    // v0.8.4: validate inline #Mesh3D base64 payloads (AX-MESH-001) and cross-entity writes
    // to undeclared fields (AX-XWRITE-001). Both run last because they're additive and don't
    // gate other checks — a corrupt inline mesh shouldn't suppress the rest of the diagnostics.
    ...checkInlineMeshes(program, source),
    ...checkCrossEntityWrites(program, source),
    // v0.8.8: undefined function detection (AX-UNDEF-FN-001) and query arg validation
    // (AX-QUERY-001). Both are advisory — they don't block compilation but surface typos before
    // they cost a runtime retry cycle.
    ...checkUndefinedFunctions(program, source),
    ...checkQueryArgs(program, source),
    // v0.9.0: script-shaped programs — entry point and function-body call checking.
    ...checkEntryPoint(program, source),
    ...checkUndefinedFunctionsInBodies(program, source),
    // v0.9.1: the static safety net — wrong arity, unknown record field, mistyped name,
    // literal against a declared type.
    ...checkArity(program, source),
    ...checkRecordFields(program, source),
    ...checkUnknownNames(program, source),
    ...checkLiteralTypes(program, source),
  ];
  const blocking = diagnostics.some(d => d.severity === 'fatal' || d.severity === 'contract_violation');
  return { ok: !blocking, program, diagnostics };
}

module.exports = {
  compile, checkZeroAlloc, checkSealedBody, checkEvents,
  checkInferStrategies, checkPoolTypes, checkVersionPragma,
  checkFunctions, checkLoopControl, checkMixins, checkUnknownBlocks,
  checkFunctionOnlyInActionPosition, checkBroadcastNoListeners,
  // v0.8.4: inline #Mesh3D validation + cross-entity write advisory
  checkInlineMeshes, checkCrossEntityWrites,
  // v0.8.8: undefined function detection + query arg validation
  checkUndefinedFunctions, checkQueryArgs, KNOWN_INTRINSIC_NAMES,
  // v0.9.0: module resolution, entry-point and function-body checks
  resolveUses, checkEntryPoint, checkUndefinedFunctionsInBodies,
  // v0.9.1: static safety net
  checkArity, checkRecordFields, checkUnknownNames, checkLiteralTypes,
  patternBindings, collectBoundNames, forEachBody,
  RESERVED_XWRITE_PROPS,
  makePayload, RULE,
  // v0.8.1: Levenshtein distance + closest-block-name helper for AX-BLOCK-001 diagnostics
  levenshtein, closestBlockName,
};
