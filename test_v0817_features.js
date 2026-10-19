// test_v0817_features.js — v0.8.17 feature tests
//
// Covers:
//   Stream 1: --term-fps flag (CLI parsing + clamping)
//   Stream 2: sub-pixel ▌▐ half-block mode (terminal.js + --subpixel/-S CLI flag)
//   Stream 3: ??= null-coalescing assignment (lexer + parser + interpreter)
//
// All tests run without spawning a child process except the CLI flag tests,
// which use spawnSync to verify main.js flag parsing.

const { renderToTerminal, detectColorSupport } = require('./terminal');
const { compile } = require('./checker');
const { World } = require('./interpreter');
const { spawnSync } = require('child_process');
const path = require('path');

let pass = 0, fail = 0;
function ok(label, cond) {
  if (cond) { console.log(`OK   ${label}`); pass++; }
  else { console.log(`FAIL ${label}`); fail++; }
}

// --- Helpers ----------------------------------------------------------------

function makeBuffer(width, height, fill) {
  const buf = new Uint8ClampedArray(width * height * 4);
  if (fill) {
    for (let i = 0; i < buf.length; i += 4) {
      buf[i]     = fill[0];
      buf[i + 1] = fill[1];
      buf[i + 2] = fill[2];
      buf[i + 3] = fill[3];
    }
  }
  return buf;
}

function setPixel(buf, width, x, y, rgba) {
  const i = (y * width + x) * 4;
  buf[i]     = rgba[0];
  buf[i + 1] = rgba[1];
  buf[i + 2] = rgba[2];
  buf[i + 3] = rgba[3];
}

// Save/restore env vars so tests don't pollute each other.
function withEnv(overrides, fn) {
  const saved = {};
  for (const k of Object.keys(overrides)) {
    saved[k] = process.env[k];
    if (overrides[k] === undefined) delete process.env[k];
    else process.env[k] = overrides[k];
  }
  try { return fn(); }
  finally {
    for (const k of Object.keys(saved)) {
      if (saved[k] === undefined) delete process.env[k];
      else process.env[k] = saved[k];
    }
  }
}

// ============================================================================
// Stream 1: --term-fps flag
// ============================================================================

console.log('\n--- Stream 1: --term-fps ---');

// 1.1: --term-fps 30 is accepted by flag parser
{
  const result = spawnSync('node', [path.join(__dirname, 'main.js'), '--check', '--eval', '@E\n  ~x: 1', '--terminal', '--term-fps', '30'], {
    encoding: 'utf8',
    env: { ...process.env, NO_COLOR: '' },
    timeout: 5000,
  });
  const stderr = result.stderr || '';
  ok('1.1: --term-fps accepted (no "unknown flag" warning)', !stderr.includes('unknown flag: --term-fps'));
}

// 1.2: --term-fps 0 is clamped to 1 (warning emitted)
{
  const result = spawnSync('node', [path.join(__dirname, 'main.js'), '--terminal', '1', '--eval', '@E\n  ~x: 1', '--term-fps', '0'], {
    encoding: 'utf8',
    env: { ...process.env, NO_COLOR: '' },
    timeout: 5000,
  });
  const stderr = result.stderr || '';
  ok('1.2: --term-fps 0 clamped to 1 (warning emitted)', stderr.includes('clamped to 1'));
}

// 1.3: --term-fps 999 is clamped to 60 (warning emitted)
{
  const result = spawnSync('node', [path.join(__dirname, 'main.js'), '--terminal', '1', '--eval', '@E\n  ~x: 1', '--term-fps', '999'], {
    encoding: 'utf8',
    env: { ...process.env, NO_COLOR: '' },
    timeout: 5000,
  });
  const stderr = result.stderr || '';
  ok('1.3: --term-fps 999 clamped to 60 (warning emitted)', stderr.includes('clamped to 60'));
}

// 1.4: --term-fps is NOT consumed as a frame-count arg when it follows --terminal
{
  // `--terminal --term-fps 30 5` — --terminal sees --term-fps (non-numeric), sets frames=60 (default).
  // Then --term-fps 30 is parsed. Then 5 is a stray positional (ignored since world.ax is positional).
  // The fps should be 30, frames should be 60 (default, since --terminal didn't get a numeric arg).
  const result = spawnSync('node', [path.join(__dirname, 'main.js'), '--eval', '@E\n  ~x: 1', '--terminal', '--term-fps', '30', '5'], {
    encoding: 'utf8',
    env: { ...process.env, NO_COLOR: '' },
    timeout: 5000,
  });
  const stderr = result.stderr || '';
  // The Mode line should show "30 fps target" (from --term-fps), NOT "5 fps" (which would mean
  // --term-fps ate the 5 as its value, leaving --terminal without a frame count... actually that
  // wouldn't produce "5 fps" either. The real risk is --terminal eating "30" as frames. Verify
  // the fps is 30 and frames is 60.)
  ok('1.4: --term-fps not consumed as frame count (fps=30 in Mode line)', stderr.includes('30 fps target'));
}

// 1.5: --term-fps with no arg emits a warning
{
  const result = spawnSync('node', [path.join(__dirname, 'main.js'), '--check', '--eval', '@E\n  ~x: 1', '--terminal', '--term-fps'], {
    encoding: 'utf8',
    env: { ...process.env, NO_COLOR: '' },
    timeout: 5000,
  });
  const stderr = result.stderr || '';
  ok('1.5: --term-fps with no arg warns', stderr.includes('--term-fps requires a numeric argument'));
}

// ============================================================================
// Stream 2: Sub-pixel ▌▐ half-block mode
// ============================================================================

console.log('\n--- Stream 2: sub-pixel ▌▐ ---');

// 2.1: subpixel: true output contains ▌ or ▐
{
  // 4×4 frame: left half (cols 0-1) red, right half (cols 2-3) blue.
  // With termWidth=2, termHeight=2: effectiveTermW=1, cellW=floor(4/1)=4, subCellW=2.
  // Cell 0 samples left (cols 0-1, red) and right (cols 2-3, blue). High horizontal contrast → ▌▐.
  const buf = makeBuffer(4, 4, [255, 0, 0, 255]);
  for (let y = 0; y < 4; y++) {
    for (let x = 2; x < 4; x++) setPixel(buf, 4, x, y, [0, 0, 255, 255]);
  }
  const out = withEnv({ COLORTERM: 'truecolor', NO_COLOR: undefined },
    () => renderToTerminal(buf, 4, 4, { mode: 'unicode', color: true, termWidth: 2, termHeight: 2, isTTY: false, subpixel: true }));
  ok('2.1: subpixel output contains ▌ (U+258C) or ▐ (U+2590)',
     out.includes('\u258C') || out.includes('\u2590'));
}

// 2.2: sub-pixel left-right split: left=red, right=blue → red fg + blue fg appear
{
  // Same setup as 2.1: 4×4, left red, right blue, termWidth=2 (effectiveTermW=1).
  const buf = makeBuffer(4, 4, [255, 0, 0, 255]);
  for (let y = 0; y < 4; y++) {
    for (let x = 2; x < 4; x++) setPixel(buf, 4, x, y, [0, 0, 255, 255]);
  }
  const out = withEnv({ COLORTERM: 'truecolor', NO_COLOR: undefined },
    () => renderToTerminal(buf, 4, 4, { mode: 'unicode', color: true, termWidth: 2, termHeight: 2, isTTY: false, subpixel: true }));
  // Red (255;0;0) and blue (0;0;255) should both appear as fg colors.
  ok('2.2: subpixel split has red fg (255;0;0)', out.includes('38;2;255;0;0'));
  ok('2.2b: subpixel split has blue fg (0;0;255)', out.includes('38;2;0;0;255'));
}

// 2.3: sub-pixel halves effective width — verify by checking that a buffer with a sharp
// vertical color boundary produces DIFFERENT output in sub-pixel vs non-sub-pixel mode.
// In sub-pixel mode, the boundary cell can show BOTH colors (▌▐); in non-sub-pixel mode,
// the boundary cell shows only one color (averaged).
{
  // 8×2 frame: left 4 cols red, right 4 cols blue. termWidth=2, termHeight=1.
  // Non-subpixel: 2 cells, each 4px wide. Cell 0 = all red, cell 1 = all blue. Output: ▀▀ with red fg, blue fg.
  // Sub-pixel: effectiveTermW=1, cellW=8, subCellW=4. Cell 0 samples left (red) and right (blue).
  //   High horizontal contrast → ▌▐ (left=red fg, right=blue fg). Output: ▌▐.
  const buf = makeBuffer(8, 2, [255, 0, 0, 255]);
  for (let y = 0; y < 2; y++) {
    for (let x = 4; x < 8; x++) setPixel(buf, 8, x, y, [0, 0, 255, 255]);
  }
  const outNonSub = withEnv({ COLORTERM: 'truecolor', NO_COLOR: undefined },
    () => renderToTerminal(buf, 8, 2, { mode: 'unicode', color: true, termWidth: 2, termHeight: 1, isTTY: false, subpixel: false }));
  const outSub = withEnv({ COLORTERM: 'truecolor', NO_COLOR: undefined },
    () => renderToTerminal(buf, 8, 2, { mode: 'unicode', color: true, termWidth: 2, termHeight: 1, isTTY: false, subpixel: true }));
  // Sub-pixel should emit ▌ or ▐; non-sub-pixel should not (it uses ▀ or █).
  const subHasHalfBlock = outSub.includes('\u258C') || outSub.includes('\u2590');
  const nonSubHasHalfBlock = outNonSub.includes('\u258C') || outNonSub.includes('\u2590');
  ok('2.3: subpixel emits ▌▐ for boundary cell', subHasHalfBlock);
  ok('2.3b: non-subpixel does NOT emit ▌▐', !nonSubHasHalfBlock);
}

// 2.4: --subpixel flag implies --terminal
{
  const result = spawnSync('node', [path.join(__dirname, 'main.js'), '--check', '--eval', '@E\n  ~x: 1', '--subpixel'], {
    encoding: 'utf8',
    env: { ...process.env, NO_COLOR: '' },
    timeout: 5000,
  });
  const stderr = result.stderr || '';
  ok('2.4: --subpixel accepted (no "unknown flag" warning)', !stderr.includes('unknown flag: --subpixel'));
}

// 2.5: -S short flag works
{
  const result = spawnSync('node', [path.join(__dirname, 'main.js'), '--check', '--eval', '@E\n  ~x: 1', '-S'], {
    encoding: 'utf8',
    env: { ...process.env, NO_COLOR: '' },
    timeout: 5000,
  });
  const stderr = result.stderr || '';
  ok('2.5: -S short flag accepted (no "unknown flag" warning)', !stderr.includes('unknown flag: -S'));
}

// 2.6: sub-pixel + ASCII mode emits a warning
{
  const result = spawnSync('node', [path.join(__dirname, 'main.js'), '--terminal', '1', '--eval', '@E\n  ~x: 1', '--ascii', '--subpixel'], {
    encoding: 'utf8',
    env: { ...process.env, NO_COLOR: '' },
    timeout: 5000,
  });
  const stderr = result.stderr || '';
  ok('2.6: --ascii --subpixel emits warning', stderr.includes('--subpixel ignored in ASCII mode'));
}

// 2.7: performance — sub-pixel 160×120 → 40×40 in < 25ms
{
  const buf = makeBuffer(160, 120, [100, 100, 100, 255]);
  // Warm up.
  withEnv({ COLORTERM: 'truecolor', NO_COLOR: undefined },
    () => renderToTerminal(buf, 160, 120, { mode: 'unicode', color: true, termWidth: 80, termHeight: 40, isTTY: false, subpixel: true }));
  withEnv({ COLORTERM: 'truecolor', NO_COLOR: undefined },
    () => renderToTerminal(buf, 160, 120, { mode: 'unicode', color: true, termWidth: 80, termHeight: 40, isTTY: false, subpixel: true }));
  // Measure.
  const t0 = process.hrtime.bigint();
  withEnv({ COLORTERM: 'truecolor', NO_COLOR: undefined },
    () => renderToTerminal(buf, 160, 120, { mode: 'unicode', color: true, termWidth: 80, termHeight: 40, isTTY: false, subpixel: true }));
  const t1 = process.hrtime.bigint();
  const ms = Number(t1 - t0) / 1_000_000;
  ok(`2.7: subpixel perf ${ms.toFixed(2)}ms < 25ms budget`, ms < 25);
}

// 2.8: sub-pixel with vertical contrast uses ▀ (not ▌▐)
{
  // 4×4 frame: top half (rows 0-1) red, bottom half (rows 2-3) blue.
  // High vertical contrast, low horizontal contrast → should use ▀▀ (vertical split).
  const buf = makeBuffer(4, 4, [255, 0, 0, 255]);
  for (let y = 2; y < 4; y++) {
    for (let x = 0; x < 4; x++) setPixel(buf, 4, x, y, [0, 0, 255, 255]);
  }
  const out = withEnv({ COLORTERM: 'truecolor', NO_COLOR: undefined },
    () => renderToTerminal(buf, 4, 4, { mode: 'unicode', color: true, termWidth: 2, termHeight: 1, isTTY: false, subpixel: true }));
  // Should contain ▀ (U+2580) — vertical split mode.
  ok('2.8: subpixel vertical contrast uses ▀', out.includes('\u2580'));
}

// ============================================================================
// Stream 3: ??= null-coalescing assignment
// ============================================================================

console.log('\n--- Stream 3: ??= operator ---');

// 3.1: ~hp ??= 100 on entity with no ~hp declared → hp becomes 100
{
  const src = `@E
  ~first: 0
  &tick(10hz):
    ?first == 0:
      hp ??= 100
      first = 1`;
  const r = compile(src);
  ok('3.1: ??= compiles', r.ok);
  if (r.ok) {
    const w = new World().loadProgram(r.program, src);
    w.update(0.2);
    // hp is in locals (not fields) because it wasn't declared with ~hp:.
    ok('3.1: ??= sets undeclared field to 100', w.tags.get('E').get('hp') === 100);
  }
}

// 3.2: ~hp ??= 100 on entity with ~hp: 50 → hp stays 50
{
  const src = `@E
  ~hp: 50
  ~first: 0
  &tick(10hz):
    ?first == 0:
      hp ??= 100
      first = 1`;
  const r = compile(src);
  ok('3.2: ??= compiles with existing field', r.ok);
  if (r.ok) {
    const w = new World().loadProgram(r.program, src);
    w.update(0.2);
    ok('3.2: ??= does NOT overwrite non-null value (hp stays 50)', w.tags.get('E').fields.get('hp') === 50);
  }
}

// 3.3: ~hp ??= 100 on entity with ~hp: null → hp becomes 100
{
  const src = `@E
  ~hp: null
  ~first: 0
  &tick(10hz):
    ?first == 0:
      hp ??= 100
      first = 1`;
  const r = compile(src);
  ok('3.3: ??= compiles with null field', r.ok);
  if (r.ok) {
    const w = new World().loadProgram(r.program, src);
    w.update(0.2);
    ok('3.3: ??= overwrites null (hp becomes 100)', w.tags.get('E').fields.get('hp') === 100);
  }
}

// 3.4: ??= with complex expression (function call)
{
  const src = `^fn deflt():
  ^return 42
@E
  ~hp: null
  ~first: 0
  &tick(10hz):
    ?first == 0:
      hp ??= deflt()
      first = 1`;
  const r = compile(src);
  ok('3.4: ??= with fn call compiles', r.ok);
  if (r.ok) {
    const w = new World().loadProgram(r.program, src);
    w.update(0.2);
    ok('3.4: ??= evaluates function call (hp=42)', w.tags.get('E').fields.get('hp') === 42);
  }
}

// 3.5: #Tag.score ??= 0 cross-entity — if score is null, sets it to 0
{
  const src = `@E
  ~score: null
@T
  ~first: 0
  &tick(10hz):
    ?first == 0:
      #E.score ??= 7
      first = 1`;
  const r = compile(src);
  ok('3.5: cross-entity ??= compiles', r.ok);
  if (r.ok) {
    const w = new World().loadProgram(r.program, src);
    w.update(0.2);
    ok('3.5: cross-entity ??= sets null to value (score=7)', w.tags.get('E').fields.get('score') === 7);
  }
}

// 3.6: #Tag.score ??= 0 cross-entity — if score is 42, stays 42
{
  const src = `@E
  ~score: 42
@T
  ~first: 0
  &tick(10hz):
    ?first == 0:
      #E.score ??= 0
      first = 1`;
  const r = compile(src);
  ok('3.6: cross-entity ??= compiles with existing value', r.ok);
  if (r.ok) {
    const w = new World().loadProgram(r.program, src);
    w.update(0.2);
    ok('3.6: cross-entity ??= preserves non-null (score=42)', w.tags.get('E').fields.get('score') === 42);
  }
}

// 3.7: ??= in a &tick block (not just &physics)
{
  // Note: World.update() runs &physics BEFORE &tick. So &physics fires first (hp null → set to 99),
  // then &tick sees hp is non-null and skips. The test verifies ??= works correctly across
  // multiple block types — the final value should be 99 (from &physics), not 100 (from &tick),
  // because &physics ran first and the &tick ??= correctly saw the non-null value.
  const src = `@E
  ~hp: null
  ~first: 0
  &tick(10hz):
    ?first == 0:
      hp ??= 100
      first = 1
  &physics:
    ?first == 0:
      hp ??= 99
      first = 1`;
  const r = compile(src);
  ok('3.7: ??= in &tick compiles', r.ok);
  if (r.ok) {
    const w = new World().loadProgram(r.program, src);
    w.update(0.2);
    // &physics runs first (hp=99), then &tick sees hp non-null and skips. Final hp=99.
    ok('3.7: ??= works in &tick (hp=99 from &physics, &tick ??= correctly skipped)', w.tags.get('E').get('hp') === 99);
  }
}

// 3.8: ??= compiles clean (no diagnostics on valid code)
{
  const src = `@E
  ~hp: null
  ~first: 0
  &tick(10hz):
    ?first == 0:
      hp ??= 100
      first = 1`;
  const r = compile(src);
  const blocking = r.diagnostics.filter(d => d.severity === 'fatal' || d.severity === 'contract_violation');
  ok('3.8: ??= produces no blocking diagnostics', r.ok && blocking.length === 0);
}

// 3.9: ??= with 0 (falsy but not null) — should NOT overwrite
{
  const src = `@E
  ~count: 0
  ~first: 0
  &tick(10hz):
    ?first == 0:
      count ??= 99
      first = 1`;
  const r = compile(src);
  ok('3.9: ??= with 0 compiles', r.ok);
  if (r.ok) {
    const w = new World().loadProgram(r.program, src);
    w.update(0.2);
    ok('3.9: ??= does NOT overwrite 0 (falsy but not null) — count stays 0', w.tags.get('E').fields.get('count') === 0);
  }
}

// 3.10: ??= with false (falsy but not null) — should NOT overwrite
{
  const src = `@E
  ~flag: false
  ~first: 0
  &tick(10hz):
    ?first == 0:
      flag ??= true
      first = 1`;
  const r = compile(src);
  ok('3.10: ??= with false compiles', r.ok);
  if (r.ok) {
    const w = new World().loadProgram(r.program, src);
    w.update(0.2);
    ok('3.10: ??= does NOT overwrite false (falsy but not null) — flag stays false', w.tags.get('E').fields.get('flag') === false);
  }
}

// 3.11: ~field ??= value (with ~ sigil, inside block body)
{
  const src = `@E
  ~hp: null
  ~first: 0
  &tick(10hz):
    ?first == 0:
      ~hp ??= 100
      first = 1`;
  const r = compile(src);
  ok('3.11: ~hp ??= val compiles (sigil form)', r.ok);
  if (r.ok) {
    const w = new World().loadProgram(r.program, src);
    w.update(0.2);
    ok('3.11: ~hp ??= sets null to 100', w.tags.get('E').fields.get('hp') === 100);
  }
}

// 3.12: lexer smoke test — ??= is a single token, ?? is separate
{
  const { tokenize, TT } = require('./lexer');
  const { tokens } = tokenize('@E\n  ~a ??= 1\n  ~b ?? c\n');
  const types = tokens.map(t => t.type).filter(t => !['NEWLINE','INDENT','DEDENT','EOF'].includes(t));
  ok('3.12: lexer emits NULLCOALEQ for ??=', types.includes('NULLCOALEQ'));
  ok('3.12b: lexer still emits NULLCOAL for ??', types.includes('NULLCOAL'));
}

console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail === 0 ? 0 : 1);
