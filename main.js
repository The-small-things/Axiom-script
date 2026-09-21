#!/usr/bin/env node
// main.js — AxiomScript v0.8.17 runtime
//
// Loads and compiles a .ax file, runs the game loop, and renders frames.
//
// Mode 1 (preferred, when SDL is available): opens a 640×480 window via @kmamal/sdl,
//   runs at 60Hz, pushes RGBA pixels via `window.render(width, height, stride, 'rgba32', buffer)`.
//   WASD/Space keyboard input → world.input. ESC quits.
//
// Mode 2 (headless fallback, used in CI / sandboxes without a display):
//   runs N frames, dumps a PNG per `--png-every` frames (default 30) and a final PNG.
//   Input is a recorded sequence or zero (Vec2(0,0), jump=false) by default.
//
// Mode 3 (v0.8.16: terminal rendering, --terminal / -t):
//   runs at 15 fps, renders each frame as Unicode block characters with 24-bit ANSI
//   color directly to stdout. Auto-detects terminal capabilities; falls back to ASCII
//   density characters ( .:-=+#@) when truecolor/Unicode is unavailable.
//   Overrides --headless and --sdl when specified. Default mode when no SDL and no
//   --headless is given: try SDL → terminal → headless PNG.
//
// The rasterizer (render3d.js) "owns the renderer" — SDL/terminal are just "monitor cables".
//
// Usage:
//   node main.js world.ax                  # auto-detect mode (SDL if available, else terminal)
//   node main.js world.ax --terminal 60    # force terminal mode, run 60 frames
//   node main.js world.ax --ascii          # force ASCII density mode (implies --terminal)
//   node main.js world.ax --no-color       # force no-color terminal mode
//   node main.js world.ax --headless 60    # headless, run 60 frames, save final PNG
//   node main.js world.ax --sdl            # force SDL window (errors out if SDL unavailable)
//   node main.js world.ax --png-every 30   # in either mode, save a PNG every 30 frames
//   node main.js world.ax --input script.txt  # headless mode: per-step input from a file
//   node main.js world.ax --json --terminal # terminal renders to stdout during run; JSON dump emitted after final frame
//
// Output: writes PNGs to ./screenshots/frame_NNNNN.png (headless mode only)

const fs = require('fs');
const path = require('path');
const { compile } = require('./checker');
const { World, Vec2 } = require('./interpreter');
const { rasterizeFrame, savePNG } = require('./render3d');
// v0.8.16: terminal rendering backend — converts the RGBA pixel buffer to Unicode
// block characters with 24-bit ANSI color. Pure post-process on rasterizeFrame output.
const { renderToTerminal, detectColorSupport } = require('./terminal');
// v0.8.1: gamepad input adapter — polls @kmamal/sdl controllers (if any connected) and merges
// LeftStick → input.move, ButtonSouth → input.jump, alongside the existing keyboard polling.
const { pollGamepadToInput, mergeKeyboardGamepad } = require('./input_gamepad');

// v0.8.15: serialize AxiomScript runtime values for --json output. Plain numbers/strings/bools
// pass through. Vec3/Vec2/Quat become arrays. Atom becomes {atom: name}. EntityInstance becomes
// its tag name. Everything else falls back to a string representation.
function serializeForJson(v) {
  if (v === null || v === undefined) return null;
  if (typeof v === 'number' || typeof v === 'string' || typeof v === 'boolean') return v;
  if (Array.isArray(v)) return v.map(serializeForJson);
  // Duck-type Vec3/Vec2/Quat by checking for the x/y/z props (avoids importing the classes).
  if (typeof v === 'object') {
    if (v.constructor && v.constructor.name === 'Vec3') return [v.x, v.y, v.z];
    if (v.constructor && v.constructor.name === 'Vec2') return [v.x, v.y];
    if (v.constructor && v.constructor.name === 'Quat') return [v.x, v.y, v.z, v.w];
    if (v.constructor && v.constructor.name === 'Atom') return { atom: v.name };
    if (v.constructor && v.constructor.name === 'EntityInstance') return { entity: v._tagName || v.decl.name };
    if (v.__timer) return { timer: { remaining: v.remaining, total: v.total } };
    if (v.__hud) return { hud: { kind: v.kind, pos: v.pos ? [v.pos.x, v.pos.y, v.pos.z] : null } };
    // Plain object — serialize own enumerable props.
    const out = {};
    for (const [k, val] of Object.entries(v)) {
      try { out[k] = serializeForJson(val); } catch (e) { out[k] = String(val); }
    }
    return out;
  }
  return String(v);
}

// --- Argument parsing ---
// v0.8.15: added --check (parse-only, no World creation), --json (structured state output),
// --eval 'source' (inline source instead of file), --stdin (read source from stdin).
// These support LLM codegen pipelines and IDE/CI integration.
const args = process.argv.slice(2);
const flags = {};
let inlineSource = null;
let positionalFile = null;
for (let i = 0; i < args.length; i++) {
  const a = args[i];
  // v0.8.17: accept both --long and -short flags. Previously only --long flags were parsed;
  // -t/-A/-C were listed in the conditionals but never matched (a.startsWith('--') excluded them).
  if (a.startsWith('--') || /^-[a-zA-Z]+$/.test(a)) {
    if (a === '--headless') { flags.headless = true; flags.frames = parseInt(args[++i], 10) || 60; }
    else if (a === '--sdl') { flags.sdl = true; }
    // v0.8.16: terminal rendering backend. --terminal/-t overrides --headless and --sdl.
    // Optional frame count arg matches --headless convention (parseInt(args[++i]) || 60).
    // If the next arg is non-numeric (e.g. a filename), don't consume it.
    else if (a === '--terminal' || a === '-t') {
      flags.terminal = true;
      const next = args[i + 1];
      if (next !== undefined && /^\d+$/.test(next)) { flags.frames = parseInt(args[++i], 10); }
      else { flags.frames = 60; }
    }
    // v0.8.16: --ascii/-A forces ASCII density mode (implies --terminal). Pure flag, no arg.
    else if (a === '--ascii' || a === '-A') { flags.terminal = true; flags.ascii = true; }
    // v0.8.16: --no-color/-C forces no-color mode (overrides COLORTERM=truecolor detection). Pure flag, no arg.
    else if (a === '--no-color' || a === '-C') { flags.noColor = true; }
    // v0.8.17: --subpixel/-S — enable sub-pixel half-block rendering (▌▐). Implies --terminal.
    // Each logical cell becomes 2 terminal columns, doubling horizontal resolution.
    // Silently ignored in ASCII mode (ASCII can't do half-blocks — a warning is emitted at render time).
    else if (a === '--subpixel' || a === '-S') { flags.terminal = true; flags.subpixel = true; }
    // v0.8.17: --term-fps N — tune terminal animation frame rate. Default 15; clamped to [1, 60].
    // Useful for fast terminals (kitty, alacritty → 30+ fps) and slow SSH (→ 5 fps).
    // The flag is purely additive — without it, terminal mode uses 15 fps (backward compat).
    else if (a === '--term-fps') {
      const n = parseInt(args[++i], 10);
      if (isNaN(n)) { console.warn('--term-fps requires a numeric argument; ignoring.'); }
      else { flags.termFps = n; }
    }
    else if (a === '--png-every') { flags.pngEvery = parseInt(args[++i], 10) || 30; }
    else if (a === '--width') { flags.width = parseInt(args[++i], 10) || 640; }
    else if (a === '--height') { flags.height = parseInt(args[++i], 10) || 480; }
    else if (a === '--input') { flags.inputScript = args[++i]; }
    // v0.8.7: sandbox mode.
    else if (a === '--sandbox') { flags.sandbox = true; }
    else if (a === '--allow-net') { flags.allowNet = true; }
    else if (a === '--allow-read') { flags.allowReadPaths = flags.allowReadPaths || []; flags.allowReadPaths.push(args[++i]); }
    // v0.8.15: --check (parse-only, no World creation). For linting/CI/IDE. Exits 0 if no
    // fatal/contract_violation diagnostics, 1 otherwise. Diagnostics go to stderr.
    else if (a === '--check') { flags.check = true; }
    // v0.8.15: --json (structured state output). After running --headless N frames, output a
    // JSON object to stdout with entities, log, diagnostics, frames_run, sim_time. No PNG output.
    else if (a === '--json') { flags.json = true; }
    // v0.8.15: --eval 'source' — inline source. Skips file reading. Useful for LLM codegen
    // pipelines that would otherwise write to a temp file.
    else if (a === '--eval') { inlineSource = args[++i]; if (!inlineSource) { console.error('--eval requires a source string argument'); process.exit(1); } }
    // v0.8.15: --stdin — read source from stdin. If neither --eval nor a file arg nor --stdin
    // is given, defaults to 'world.ax' (backward compat).
    else if (a === '--stdin') { flags.stdin = true; }
    else console.warn(`unknown flag: ${a}`);
  } else {
    // First non-flag argument is the .ax file (backward compat).
    if (!positionalFile) positionalFile = a;
  }
}
const WIDTH = flags.width || 640;
const HEIGHT = flags.height || 480;
const PNG_EVERY = flags.pngEvery || 30;
const SHOT_DIR = path.join(__dirname, 'screenshots');

// --- Determine source text ---
// v0.8.15: source can come from (1) --eval 'string', (2) --stdin, or (3) a file argument.
// Priority: --eval > --stdin > file. If none, default to 'world.ax' (backward compat).
let src;
let sourceLabel;
if (inlineSource !== null) {
  src = inlineSource;
  sourceLabel = '<eval>';
} else if (flags.stdin) {
  // Read all of stdin synchronously. Simplest approach: use fs.readFileSync(0).
  try {
    src = fs.readFileSync(0, 'utf8');
  } catch (e) {
    console.error(`--stdin: failed to read from stdin: ${e.message}`);
    process.exit(1);
  }
  sourceLabel = '<stdin>';
} else {
  const axFile = positionalFile || 'world.ax';
  if (!fs.existsSync(axFile)) {
    console.error(`axiom file not found: ${axFile}`);
    process.exit(1);
  }
  src = fs.readFileSync(axFile, 'utf8');
  sourceLabel = axFile;
}

// --- Compile ---
const r = compile(src);

// --- --check mode: print diagnostics, exit 0/1, no World creation ---
if (flags.check) {
  if (r.diagnostics.length > 0) {
    for (const d of r.diagnostics) {
      console.error(`  [${d.error_code}] (${d.severity}) L${d.location?.line}:${d.location?.col} ${d.message_for_human}`);
    }
  }
  if (r.ok) {
    console.error(`OK: ${sourceLabel} compiles clean (${r.diagnostics.length} advisory diagnostics).`);
    process.exit(0);
  } else {
    console.error(`FAIL: ${sourceLabel} has ${r.diagnostics.filter(d => d.severity === 'fatal' || d.severity === 'contract_violation').length} blocking diagnostic(s).`);
    process.exit(1);
  }
}

if (!r.ok) {
  console.error(`compile failed:`);
  for (const d of r.diagnostics) {
    console.error(`  [${d.error_code}] (${d.severity}) L${d.location?.line}:${d.location?.col} ${d.message_for_human}`);
  }
  process.exit(1);
}
const w = new World().loadProgram(r.program, src);
// v0.8.15: suppress console.log from !log/!print when --json is active (would corrupt JSON output)
if (flags.json) w._suppressConsole = true;
// v0.8.7: activate sandbox mode if --sandbox was passed.
if (flags.sandbox) {
  w.setSandbox({
    allowNet: !!flags.allowNet,
    allowReadPaths: flags.allowReadPaths || [],
  });
  // In --json mode, suppress the sandbox log line (it would corrupt JSON output).
  if (!flags.json) {
    console.log(`Sandbox mode ACTIVE: !play stubbed, !save/!load in-memory only, network ${flags.allowNet ? 'allowed' : 'blocked'}, fs reads ${flags.allowReadPaths ? 'limited to: ' + flags.allowReadPaths.join(', ') : 'blocked'}`);
  }
}
// In --json mode, suppress the "Loaded ..." log line (it would corrupt JSON output).
if (!flags.json) {
  console.log(`Loaded ${sourceLabel}: ${w.entities.length} entities (${w.entities.map(e => e._tagName).join(', ')}), version ${r.program.version || '(none)'}`);
}

// --- Input script (for headless mode) ---
// Each line of the input file is "WASD" or "W" or "WD" or "SPACE" applied for one update step.
// Lines that start with `#` are comments. Empty lines = no input for that step.
let inputSteps = [];
if (flags.inputScript && fs.existsSync(flags.inputScript)) {
  inputSteps = fs.readFileSync(flags.inputScript, 'utf8')
    .split('\n')
    .map(l => l.trim())
    .filter(l => l.length > 0 && !l.startsWith('#'));
}

function applyInput(step) {
  if (!step) { w.input.move.x = 0; w.input.move.y = 0; w.input.jump = false; return; }
  let x = 0, y = 0, jump = false;
  for (const c of step.toUpperCase()) {
    if (c === 'W') y += 1;
    else if (c === 'S') y -= 1;
    else if (c === 'A') x -= 1;
    else if (c === 'D') x += 1;
    else if (c === ' ') jump = true;
  }
  w.input.move.x = x;
  w.input.move.y = y;
  w.input.jump = jump;
}

// --- Frame output ---
function ensureShotDir() { if (!fs.existsSync(SHOT_DIR)) fs.mkdirSync(SHOT_DIR, { recursive: true }); }

function dumpFrame(frameNum) {
  ensureShotDir();
  // v0.8.15 fix: removed w.stepRender(1/60) — w.update() already calls stepRender internally.
  // Calling it again here caused &render: blocks, animations, and tweens to run 2x on PNG frames.
  // The draw list is already populated by w.update(); rasterizeFrame reads it directly.
  const { pixels } = rasterizeFrame(w, WIDTH, HEIGHT);
  const fname = `frame_${String(frameNum).padStart(5, '0')}.png`;
  const fpath = path.join(SHOT_DIR, fname);
  savePNG(pixels, WIDTH, HEIGHT, fpath);
  console.log(`  frame ${frameNum} → ${fpath}`);
  return fpath;
}

// --- Mode detection ---
// The ONLY allowed external dep per the v0.6 spec is @kmamal/sdl. If it's installed and the
// platform has a display, we open a window and stream pixels via `window.render(...)`. If
// anything fails (no display, native binding, etc.), we fall back to headless PNG dump.
function trySDL() {
  try {
    const sdl = require('@kmamal/sdl');
    // Sanity-check the API surface we need.
    if (!sdl || !sdl.video || typeof sdl.video.createWindow !== 'function') return null;
    return sdl;
  } catch (e) {
    return null;
  }
}

const sdl = trySDL();
// v0.8.16: HEADLESS is now only true when --headless is explicitly passed (or when both
// SDL and --terminal are unavailable AND we somehow fell through to this branch — which
// shouldn't happen because TERMINAL takes precedence). The terminal branch below handles
// the no-SDL-no-headless case.
const HEADLESS = flags.headless || (!sdl && !flags.sdl && !flags.terminal);
if (flags.sdl && !sdl) {
  console.error(`--sdl requested but @kmamal/sdl could not be loaded: ${sdl === null ? 'require failed' : 'bad API surface'}`);
  process.exit(1);
}

// v0.8.16: terminal mode resolution. --terminal takes priority over both --headless and --sdl.
// When no render-mode flag is explicitly given AND SDL is unavailable, fall back to terminal
// mode (rather than headless PNG dump) so the user sees live animation on any tty.
const TERMINAL = flags.terminal || (!flags.headless && !flags.sdl && !sdl);
if (flags.terminal && (flags.headless || flags.sdl)) {
  // Explicit --terminal wins; warn that --headless/--sdl is being ignored.
  if (!flags.json) console.warn('--terminal overrides --headless/--sdl');
}

if (TERMINAL) {
  // v0.8.16: Terminal rendering mode.
  // Renders each frame as Unicode block characters with 24-bit ANSI color directly to stdout.
  // Auto-detects terminal capabilities (truecolor, 256-color, no color) via env vars.
  // Falls back to ASCII density characters when Unicode is unavailable or --ascii is passed.
  // 15 fps target (physics still simulates at 60 Hz). Default resolution: 160×120.
  const TERM_WIDTH = flags.width || 160;
  const TERM_HEIGHT = flags.height || 120;
  // v0.8.17: --subpixel + --ascii warning. Sub-pixel uses ▌▐ half-blocks which ASCII mode
  // can't render. Emit a warning so the user knows the flag is being ignored.
  if (flags.subpixel && flags.ascii && !flags.json) {
    console.warn('--subpixel ignored in ASCII mode (half-block characters unavailable).');
  }
  if (!flags.json) {
    const modeLabel = flags.ascii ? 'ASCII' : (detectColorSupport().mode + (flags.subpixel ? '+subpixel' : ''));
    console.error(`\nMode: terminal @ ${TERM_WIDTH}×${TERM_HEIGHT} (${modeLabel}, ${flags.noColor ? 'no color' : (detectColorSupport().color ? 'color' : 'no color')}).`);
    const fpsPreview = flags.termFps !== undefined ? Math.max(1, Math.min(60, flags.termFps)) : 15;
    console.error(`Running ${flags.frames || 60} frames @ ${fpsPreview} fps target.`);
  }
  const colorSupport = detectColorSupport();
  const termOptions = {
    mode: flags.ascii ? 'ascii' : colorSupport.mode,
    color: flags.noColor ? false : colorSupport.color,
    // v0.8.17: sub-pixel mode (▌▐ left/right half-blocks). Ignored when mode === 'ascii'.
    subpixel: !!flags.subpixel && !flags.ascii,
    // Use a smaller terminal viewport than the actual stdout size so the render fits.
    termWidth: Math.min(process.stdout.columns || 80, 80),
    termHeight: Math.min((process.stdout.rows || 24) - 2, 40),  // leave 2 rows for prompt
    isTTY: process.stdout.isTTY,
  };
  const framesRun = flags.frames || 60;
  // v0.8.17: --term-fps flag. Default 15; clamped to [1, 60]. Warns if clamped.
  // Without the flag, behavior is unchanged (15 fps — backward compat with v0.8.16).
  let TARGET_FPS = flags.termFps !== undefined ? flags.termFps : 15;
  if (TARGET_FPS < 1) { console.warn(`--term-fps ${TARGET_FPS} below minimum; clamped to 1.`); TARGET_FPS = 1; }
  if (TARGET_FPS > 60) { console.warn(`--term-fps ${TARGET_FPS} above maximum; clamped to 60.`); TARGET_FPS = 60; }
  const FRAME_MS = 1000 / TARGET_FPS;
  let frameNum = 0;
  function terminalLoop() {
    if (frameNum >= framesRun) {
      // Final frame: reset attributes + newline so the shell prompt doesn't pick up the last color.
      process.stdout.write('\x1b[0m\n');
      if (flags.json) {
        // v0.8.16: --json + --terminal interaction. During the run, terminal frames went to
        // stdout. After the final frame resets the terminal, the JSON state dump is emitted
        // to stdout (so it doesn't interleave with the animation).
        const state = {
          entities: w.entities.map(e => {
            const pose = e.locals.get('pose');
            const fields = {};
            for (const [k, v] of e.fields) fields[k] = serializeForJson(v);
            const poseObj = pose ? {
              pos: pose.pos ? [pose.pos.x, pose.pos.y, pose.pos.z] : null,
              vel: pose.vel ? [pose.vel.x, pose.vel.y, pose.vel.z] : null,
              rot: pose.rot ? [pose.rot.x, pose.rot.y, pose.rot.z, pose.rot.w] : null,
              scl: pose.scl ? [pose.scl.x, pose.scl.y, pose.scl.z] : null,
            } : null;
            return {
              name: e._tagName || e.decl.name,
              base: e.decl.base || null,
              alive: !e._despawned,
              pos: poseObj ? poseObj.pos : null,
              pose: poseObj,
              fields,
            };
          }),
          log: w.log.map(l => ({
            type: l.type,
            msg: l.msg !== undefined ? l.msg : (l.snd !== undefined ? l.snd : null),
            entity: l.entity || null,
            block: l.block || null,
            t: l.t !== undefined ? l.t : null,
          })),
          diagnostics: w.runtimeDiagnostics.map(d => ({
            code: d.error_code,
            severity: d.severity,
            message: d.message_for_human,
            line: d.location?.line || null,
            entity: d.location?.entity || null,
            block: d.location?.block || null,
          })),
          compile_diagnostics: r.diagnostics.map(d => ({
            code: d.error_code,
            severity: d.severity,
            message: d.message_for_human,
            line: d.location?.line || null,
          })),
          frames_run: framesRun,
          sim_time: w._simTime,
          version: r.program.version || null,
        };
        process.stdout.write(JSON.stringify(state, null, 2) + '\n');
      }
      process.exit(0);
      return;
    }
    applyInput(inputSteps[frameNum] || '');
    // Simulate at 60 Hz physics. Terminal renders at 15 fps but physics stays smooth.
    w.update(1 / 60);
    const { pixels } = rasterizeFrame(w, TERM_WIDTH, TERM_HEIGHT);
    const frame = renderToTerminal(pixels, TERM_WIDTH, TERM_HEIGHT, termOptions);
    process.stdout.write(frame);
    frameNum++;
    setTimeout(terminalLoop, FRAME_MS);
  }
  terminalLoop();
} else if (HEADLESS) {
  // v0.8.15: --json mode suppresses all human-readable output and emits a JSON object at the end.
  // The JSON includes entity state, log entries, diagnostics, frames_run, and sim_time.
  // Used by ALMS (AxiomScript LLM Management System) and other tooling that needs structured
  // state output for inspection/serialization.
  if (!flags.json) {
    console.log(`\nMode: headless ${sdl ? '(SDL installed but display not available / forced via --headless)' : '(no @kmamal/sdl available; install with `npm install @kmamal/sdl` for windowed mode)'}.`);
    console.log(`Running ${flags.frames || 60} frames @ ${WIDTH}×${HEIGHT}, saving PNG every ${PNG_EVERY} frames.`);
  }
  const framesRun = flags.frames || 60;
  for (let f = 0; f < framesRun; f++) {
    applyInput(inputSteps[f] || '');
    w.update(1 / 60);
    // v0.8.15: in --json mode, skip PNG dumping (the JSON output has no PNG field; PNGs are
    // for human/visual debugging only).
    if (!flags.json && (f % PNG_EVERY === 0 || f === framesRun - 1)) dumpFrame(f);
  }
  if (flags.json) {
    // Emit JSON state to stdout. Vec3/Vec2/Quat/Atom are serialized via a replacer.
    const state = {
      entities: w.entities.map(e => {
        const pose = e.locals.get('pose');
        const fields = {};
        for (const [k, v] of e.fields) {
          fields[k] = serializeForJson(v);
        }
        // v0.8.15: include full pose (pos, vel, rot, scl) in JSON output. ALMS needs this
        // for state inspection — previously only `pos` was serialized as a flat array, and
        // vel/rot/scl were invisible. Now all four Transform components are included.
        const poseObj = pose ? {
          pos: pose.pos ? [pose.pos.x, pose.pos.y, pose.pos.z] : null,
          vel: pose.vel ? [pose.vel.x, pose.vel.y, pose.vel.z] : null,
          rot: pose.rot ? [pose.rot.x, pose.rot.y, pose.rot.z, pose.rot.w] : null,
          scl: pose.scl ? [pose.scl.x, pose.scl.y, pose.scl.z] : null,
        } : null;
        return {
          name: e._tagName || e.decl.name,
          base: e.decl.base || null,
          alive: !e._despawned,
          pos: poseObj ? poseObj.pos : null,  // backward compat: flat pos array
          pose: poseObj,                       // v0.8.15: full pose object
          fields,
        };
      }),
      log: w.log.map(l => ({
        type: l.type,
        msg: l.msg !== undefined ? l.msg : (l.snd !== undefined ? l.snd : null),
        entity: l.entity || null,
        block: l.block || null,
        t: l.t !== undefined ? l.t : null,
      })),
      diagnostics: w.runtimeDiagnostics.map(d => ({
        code: d.error_code,
        severity: d.severity,
        message: d.message_for_human,
        line: d.location?.line || null,
        entity: d.location?.entity || null,
        block: d.location?.block || null,
      })),
      compile_diagnostics: r.diagnostics.map(d => ({
        code: d.error_code,
        severity: d.severity,
        message: d.message_for_human,
        line: d.location?.line || null,
      })),
      frames_run: framesRun,
      sim_time: w._simTime,
      version: r.program.version || null,
    };
    process.stdout.write(JSON.stringify(state, null, 2) + '\n');
    process.exit(0);
  }
  // Final state report (human-readable)
  console.log(`\nFinal entity state:`);
  for (const e of w.entities) {
    const pose = e.locals.get('pose');
    if (!pose) continue;
    const px = pose.pos && pose.pos.x !== undefined ? pose.pos.x.toFixed(2) : '?';
    const py = pose.pos && pose.pos.y !== undefined ? pose.pos.y.toFixed(2) : '?';
    const pz = pose.pos && pose.pos.z !== undefined ? pose.pos.z.toFixed(2) : '?';
    console.log(`  ${e._tagName}: pos=(${px},${py},${pz})`);
    for (const [k, v] of e.fields) {
      if (typeof v === 'number') console.log(`    ~${k}: ${v}`);
      else if (v && v.__timer) console.log(`    ~${k}: timer(remaining=${v.remaining.toFixed(2)})`);
      else if (v && v.name) console.log(`    ~${k}: atom(${v.name})`);
    }
  }
  console.log(`\nRuntime diagnostics: ${w.runtimeDiagnostics.length}`);
  console.log(`Log events: ${w.log.length} (!play=${w.log.filter(l => l.type === 'play').length}, !music=${w.log.filter(l => l.type === 'music').length}, !dbg_line=${w.log.filter(l => l.type === 'dbg_line').length})`);
  console.log(`\nDone. PNGs in: ${SHOT_DIR}`);
  process.exit(0);
} else {
  // SDL windowed mode using @kmamal/sdl's actual API:
  //   window = sdl.video.createWindow({ title, width, height })
  //   window.render(width, height, stride, format, buffer) — pushes RGBA pixels
  //   window.on('keyDown'/'keyUp'/'close', handler) — events
  //   window.destroy() / .destroyGently() — close
  console.log(`\nMode: SDL window @ ${WIDTH}×${HEIGHT}. WASD moves input, Space jumps, ESC quits.`);
  const window = sdl.video.createWindow({ title: `AxiomScript v0.8.17 — ${path.basename(sourceLabel)}`, width: WIDTH, height: HEIGHT });
  let frameNum = 0;
  let running = true;
  const keyState = new Set();
  // @kmamal/sdl's SCANCODE enum is at sdl.keyboard.SCANCODE; keyDown events carry `scancode`.
  const SCAN = sdl.keyboard.SCANCODE;
  window.on('keyDown', (e) => { keyState.add(e.scancode); if (e.scancode === SCAN.ESCAPE) running = false; });
  window.on('keyUp',   (e) => { keyState.delete(e.scancode); });
  window.on('close',  () => { running = false; });
  // v0.8.1: subscribe to controller connect/disconnect events if the SDL binding exposes them.
  // We track connected controllers in a Set so we can poll them each frame. The polling itself
  // is done by `pollGamepadToInput(sdl, prevJump)` which probes the SDL controller API surface
  // defensively — never throws, returns a zero-state result if no controllers are connected.
  let prevJumpPressed = false;
  if (sdl.controller) {
    if (typeof sdl.controller.on === 'function') {
      try {
        sdl.controller.on('connect', (ctrl) => {
          console.log(`[gamepad] controller connected: ${ctrl && ctrl.name ? ctrl.name : '(unnamed)'}`);
        });
        sdl.controller.on('disconnect', (ctrl) => {
          console.log(`[gamepad] controller disconnected: ${ctrl && ctrl.name ? ctrl.name : '(unnamed)'}`);
        });
      } catch (e) { /* event subscription is best-effort */ }
    }
    // Some @kmamal/sdl versions auto-open connected controllers on first poll; others need an
    // explicit open. We try `sdl.controller.openAll()` if it exists, otherwise rely on auto-open.
    if (typeof sdl.controller.openAll === 'function') {
      try { sdl.controller.openAll(); } catch (e) { /* best-effort */ }
    }
  }

  const TARGET_DT_NS = 16_666_666n; // 60 Hz
  let lastTime = process.hrtime.bigint();

  function loop() {
    if (!running) { try { window.destroy(); } catch (e) {} process.exit(0); return; }
    // Input: WASD → input.move (Vec2 x=right, y=forward), Space → jump
    let x = 0, y = 0, jump = false;
    if (keyState.has(SCAN.W)) y += 1;
    if (keyState.has(SCAN.S)) y -= 1;
    if (keyState.has(SCAN.A)) x -= 1;
    if (keyState.has(SCAN.D)) x += 1;
    if (keyState.has(SCAN.SPACE)) jump = true;
    // v0.8.1: poll gamepads. If a controller is connected and the keyboard is idle this frame,
    // the controller's LeftStick axes drive input.move and ButtonSouth drives input.jump.
    const gp = pollGamepadToInput(sdl, prevJumpPressed);
    const merged = mergeKeyboardGamepad({ x, y }, jump, gp);
    w.input.move.x = merged.x; w.input.move.y = merged.y; w.input.jump = merged.jump;
    prevJumpPressed = merged.jump;
    // Step the world + render. v0.8.15 fix: World.update() already calls stepRender()
    // internally (after stepPhysics + stepCognition). The previous code called stepRender
    // a SECOND time on the next line, which caused every &render: block, animation clip,
    // and HUD draw to run twice per frame — and Tweens advanced at 2× speed. The standalone
    // stepRender call is now removed. The headless branch (below) is unaffected: it calls
    // dumpFrame() which calls w.stepRender(1/60) once per dumped frame, matching the
    // headless convention of "render only when saving a PNG".
    w.update(1 / 60);
    const { pixels } = rasterizeFrame(w, WIDTH, HEIGHT);
    // Push pixels to the SDL window. The rasterizer outputs RGBA (Uint8ClampedArray). SDL accepts
    // a Node Buffer; we copy the pixels into a Buffer (zero-copy if the array's underlying buffer
    // is already an ArrayBuffer, which Uint8ClampedArray is).
    const buf = Buffer.from(pixels.buffer, pixels.byteOffset, pixels.byteLength);
    window.render(WIDTH, HEIGHT, WIDTH * 4, 'rgba32', buf);
    frameNum++;
    if (frameNum % PNG_EVERY === 0) dumpFrame(frameNum);
    // Frame pacing
    const now = process.hrtime.bigint();
    const dt = now - lastTime;
    if (dt < TARGET_DT_NS) {
      setTimeout(loop, Number(TARGET_DT_NS - dt) / 1_000_000);
    } else {
      setImmediate(loop);
    }
    lastTime = now;
  }
  loop();
}
