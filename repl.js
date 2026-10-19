// repl.js — `axiom --repl`: an interactive session, or a scripted one over a pipe.
//
// Built for a model driving a process as much as for a person at a terminal:
//
//   * a complete line runs at once, so a harness can write one line and read the answer;
//   * a bare expression echoes its value (strings quoted, so "1" and 1 differ), and nothing
//     else echoes — no banners, no prompts when stdin is not a terminal;
//   * a line that opens a block (ends in ':'), leaves a bracket or a """ open, or starts an
//     @Entity keeps reading; the block ends at a blank line or at the next unindented line
//     that is not its own continuation (?!, else, elif, ^catch, ^fin);
//   * declarations (^fn ^proc ^type ^use ^event ^mix ^mat @Entity #Resource ~GLOBAL:) join the
//     session; everything else runs as if in ^main, except that its names are globals, so a
//     function declared later sees a variable assigned earlier;
//   * an error prints `error [CODE]: message` to stderr and the session continues.
//
// Commands: :step N (advance the simulation N frames at 60 Hz), :help, :quit (or end of input).

'use strict';
const path = require('path');
const readline = require('readline');
const { parse } = require('./parser');
const { resolveUses } = require('./checker');
const rt = require('./interpreter');

const DECL_RE = /^(\^(fn|proc|type|use|event|mix|mat)\b|@[A-Za-z_]|#[A-Za-z_]\w*\s+[A-Za-z_]|~[A-Za-z_]\w*!?\s*:)/;
const CONT_RE = /^(\?!|else\b|elif\b|\^catch\b|\^fin\b)/;
const HELP = `statements run at once; a bare expression prints its value
blocks (a line ending in ':') end at a blank line
declarations (^fn ^type ~G: @Entity ^use …) join the session
:step N   advance the simulation N frames      :quit   leave`;

// Bracket depth, whether a """ string is still open, and whether the text's last significant
// character (outside strings and // comments) is ':'.
function scan(text) {
  let depth = 0, triple = false, last = '';
  for (let i = 0; i < text.length; i++) {
    const c = text[i];
    if (triple) {
      if (c === '"' && text.startsWith('"""', i)) { triple = false; i += 2; last = '"'; }
      continue;
    }
    if (c === '"' && text.startsWith('"""', i)) { triple = true; i += 2; continue; }
    if (c === '"' || c === "'") {
      const q = c;
      for (i++; i < text.length && text[i] !== q && text[i] !== '\n'; i++) if (text[i] === '\\') i++;
      last = q;
      continue;
    }
    if (c === '/' && text[i + 1] === '/') { while (i < text.length && text[i] !== '\n') i++; i--; continue; }
    if (c === '(' || c === '[' || c === '{') depth++;
    else if (c === ')' || c === ']' || c === '}') depth--;
    if (c !== ' ' && c !== '\t' && c !== '\n' && c !== '\r') last = c;
  }
  return { open: depth > 0 || triple, colon: last === ':' };
}

function startRepl(opts) {
  opts = opts || {};
  const input = opts.input || process.stdin;
  const out = opts.output || process.stdout;
  const err = opts.error || process.stderr;
  const interactive = !!(input.isTTY && out.isTTY);

  // Top-level names are session globals (as in a Python REPL), so a ^fn declared later sees
  // `x = 10` typed earlier.
  const world = new rt.World().loadProgram({ entities: [] }, '');
  const ctx = { ...world.scriptCtx(), scope: world.globalScope };
  const filename = path.join(process.cwd(), '<repl>.ax');
  let seenDiags = 0;

  const fail = (code, msg) => err.write(`error [${code}]: ${msg}\n`);
  const reportFault = (e) => {
    if (e && e.__exit !== undefined) { hardExit(e.__exit); return; }
    const f = rt.makeRuntimeFault(null, { name: 'repl' }, null, e, world);
    fail(f.error_code, e && e.message ? e.message : String(e));
  };
  // Faults recorded rather than thrown (a global initializer, a frame block during :step).
  const flushDiags = () => {
    const ds = world.runtimeDiagnostics;
    for (; seenDiags < ds.length; seenDiags++) {
      const d = ds[seenDiags];
      const raw = /Raw error: "([\s\S]*)"\./.exec(d.message_for_agent || '');
      fail(d.error_code, raw ? raw[1] : d.message_for_human);
    }
  };
  const show = (v) => {
    if (v === null || v === undefined) return;
    out.write((typeof v === 'string' ? JSON.stringify(v) : rt.stringifyFStringVal(v)) + '\n');
  };
  // The wrapper's line numbers would only mislead, so a parse error loses its "(line N)".
  const parseMessage = (e) => String(e && e.message || e).replace(/^Parse error \(line \d+\): /, '');
  const tryParse = (src) => {
    try {
      const program = parse(src);
      return program.parseErrors && program.parseErrors.length ? { error: program.parseErrors[0] } : { program };
    } catch (e) { return { error: e }; }
  };
  const parseOrReport = (src) => {
    const r = tryParse(src);
    if (r.error) { fail('AX-PARSE-000', parseMessage(r.error)); return null; }
    return r.program;
  };

  function declare(src) {
    const program = parseOrReport(src + '\n');
    if (!program) return;
    const bad = resolveUses(program, { filename }).find(d => d.severity === 'fatal');
    if (bad) { fail(bad.error_code, bad.message_for_human); return; }
    try { world.loadProgram(program); } catch (e) { reportFault(e); }
    flushDiags();
  }

  function run(src) {
    const body = src.split('\n').map(l => '  ' + l).join('\n');
    // An expression first (`1 + 2`, `"hi"`, `[1, 2][0]` are not statements); a statement
    // otherwise. Lines that start with a statement sigil are never read as expressions.
    let program = null;
    if (!/^[!^*?~@]/.test(src)) {
      const r = tryParse(`^main:\n  ^return ${body.slice(2)}\n`);
      if (r.program && r.program.main && r.program.main.body.length === 1) program = r.program;
    }
    if (!program) program = parseOrReport(`^main:\n${body}\n`);
    if (!program || !program.main) return;
    const stmts = program.main.body;
    try {
      if (stmts.length === 1 && stmts[0].type === 'ExprStmt') show(rt.evalExpr(stmts[0].expr, ctx));
      else {
        for (const stmt of stmts) {
          const r = rt.execStmtInner(stmt, ctx);
          if (r instanceof rt.ReturnSignal) { show(r.value); break; }
        }
      }
    } catch (e) {
      if (e instanceof rt.ReturnSignal) show(e.value);
      else reportFault(e);
    }
    flushDiags();
  }

  function command(line) {
    const [cmd, arg] = line.trim().split(/\s+/);
    if (cmd === ':q' || cmd === ':quit' || cmd === ':exit') { finish(0); return; }
    if (cmd === ':help' || cmd === ':h') { out.write(HELP + '\n'); return; }
    if (cmd === ':step') {
      const n = Math.max(1, parseInt(arg, 10) || 1);
      try { for (let i = 0; i < n; i++) world.update(1 / 60); } catch (e) { reportFault(e); }
      flushDiags();
      return;
    }
    fail('AX-REPL-001', `unknown command ${cmd} (try :help)`);
  }

  // ---- chunking -----------------------------------------------------------------------------
  let chunk = [];
  let block = false;          // the chunk is a block: it ends at a blank or unindented line
  let done = false;

  function flush() {
    if (!chunk.length) return;
    const src = chunk.join('\n');
    chunk = [];
    block = false;
    if (DECL_RE.test(src)) declare(src); else run(src);
  }

  function feed(line) {
    if (done) return;
    line = line.replace(/\r$/, '');
    const blank = line.trim() === '' || line.trim().startsWith('//');
    if (chunk.length) {
      const st = scan(chunk.join('\n'));
      if (st.open) { chunk.push(line); return settle(); }
      if (blank) { flush(); return; }
      if (/^[ \t]/.test(line) || CONT_RE.test(line)) { chunk.push(line); return settle(); }
      flush();
    }
    if (blank) return;
    if (line.startsWith(':')) { command(line); return; }
    chunk.push(line);
    block = line.startsWith('@');
    settle();
  }

  // Run the chunk now if it is complete and not a block.
  function settle() {
    const st = scan(chunk.join('\n'));
    if (!st.open && scan(chunk[chunk.length - 1]).colon) block = true;
    if (!st.open && !block) flush();
  }

  function hardExit(code) {
    if (done) return;
    done = true;
    chunk = [];
    if (opts.onExit) opts.onExit(code);
    else process.exit(code);
  }
  function finish(code) {
    if (done) return;
    flush();                  // may itself end the session through exit()
    hardExit(code);
  }

  const rl = readline.createInterface({ input, output: interactive ? out : undefined, terminal: interactive });
  const prompt = () => { if (interactive && !done) { rl.setPrompt(chunk.length ? '... ' : '> '); rl.prompt(); } };
  rl.on('line', (line) => { feed(line); prompt(); });
  rl.on('close', () => finish(0));
  prompt();
}

module.exports = { startRepl, scan };
