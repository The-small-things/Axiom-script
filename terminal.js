// terminal.js — AxiomScript v0.8.16 terminal rendering backend
//
// Pure post-process: reads the RGBA pixel buffer that rasterizeFrame() already
// produces and converts it to a character grid (Unicode half-blocks or ASCII
// density chars) with optional 24-bit ANSI color. Does NOT modify the pixel
// buffer or call into render3d.js. The terminal renderer is a "monitor cable"
// in the same sense as SDL — it just displays the framebuffer in a different
// format.
//
// Public API:
//   detectColorSupport() → { mode: 'unicode' | 'ascii', color: boolean }
//   renderToTerminal(pixels, width, height, options) → string
//
// Options:
//   mode:       'unicode' | 'ascii'    (forced; otherwise from detectColorSupport)
//   color:      true | false           (forced; otherwise from detectColorSupport)
//   termWidth:  number                 (default: process.stdout.columns || 80)
//   termHeight: number                 (default: process.stdout.rows || 24)
//   isTTY:      boolean                (default: process.stdout.isTTY)
//               When false, frames are separated by blank lines instead of
//               using the cursor-home escape \033[H.

'use strict';

// --- Color support detection ------------------------------------------------
//
// Detection order (first match wins):
//   1. NO_COLOR env var set             → color: false, mode: 'ascii'
//   2. COLORTERM contains truecolor|24bit → color: true,  mode: 'unicode'
//   3. TERM contains 256color           → color: true,  mode: 'unicode' (256-color quantization applied later)
//   4. Windows + stdout.getColorDepth() → color: depth >= 4
//   5. fallback                          → color: false, mode: 'ascii'
//
function detectColorSupport() {
  // 1. NO_COLOR convention — https://no-color.org
  if (process.env.NO_COLOR !== undefined && process.env.NO_COLOR !== '') {
    return { mode: 'ascii', color: false };
  }
  // 2. 24-bit truecolor
  const colorterm = (process.env.COLORTERM || '').toLowerCase();
  if (colorterm.includes('truecolor') || colorterm.includes('24bit')) {
    return { mode: 'unicode', color: true };
  }
  // 3. 256-color
  const term = (process.env.TERM || '').toLowerCase();
  if (term.includes('256color')) {
    return { mode: 'unicode', color: true };  // color=true; renderer will quantize to xterm-256
  }
  // 4. Windows: probe getColorDepth if available (Node 10+)
  if (process.platform === 'win32' && process.stdout && typeof process.stdout.getColorDepth === 'function') {
    try {
      const depth = process.stdout.getColorDepth();
      if (depth >= 4) return { mode: 'unicode', color: true };
      return { mode: 'ascii', color: false };
    } catch (e) { /* fall through */ }
  }
  // 5. Conservative fallback
  return { mode: 'ascii', color: false };
}

// --- 256-color quantization -------------------------------------------------
//
// Standard xterm-256 palette: 6x6x6 color cube (indices 16-231) + 24 grayscale
// ramp (indices 232-255). We pick the nearest cube color by min Euclidean dist.
//
const CUBE6 = [0, 95, 135, 175, 215, 255];  // standard xterm-256 cube levels
const CUBE_STEPS = CUBE6.length;
function rgbToXterm256(r, g, b) {
  // Find nearest cube index per channel.
  let best = 16;
  let bestDist = Infinity;
  for (let ri = 0; ri < CUBE_STEPS; ri++) {
    for (let gi = 0; gi < CUBE_STEPS; gi++) {
      for (let bi = 0; bi < CUBE_STEPS; bi++) {
        const dr = r - CUBE6[ri];
        const dg = g - CUBE6[gi];
        const db = b - CUBE6[bi];
        const d = dr*dr + dg*dg + db*db;
        if (d < bestDist) { bestDist = d; best = 16 + (ri*36 + gi*6 + bi); }
      }
    }
  }
  // Compare to nearest grayscale ramp entry.
  // Gray ramp: index 232 has RGB(8,8,8), each next +10.
  const grayIdx = Math.round(((r + g + b) / 3 - 8) / 10);
  const grayClamped = Math.max(0, Math.min(23, grayIdx));
  const grayRgb = 8 + grayClamped * 10;
  const gr = r - grayRgb, gg = g - grayRgb, gb = b - grayRgb;
  const grayDist = gr*gr + gg*gg + gb*gb;
  if (grayDist < bestDist) return 232 + grayClamped;
  return best;
}

// --- Cell color sampling ----------------------------------------------------
//
// Sample a blockW × blockH pixel region starting at (px, py). Returns the
// average RGB of all non-transparent pixels (alpha >= 128). If no opaque
// pixels are found, returns null (transparent → space char / background).
//
function sampleCellColor(pixels, width, height, px, py, blockW, blockH) {
  let r = 0, g = 0, b = 0, count = 0;
  const xEnd = Math.min(px + blockW, width);
  const yEnd = Math.min(py + blockH, height);
  for (let y = py; y < yEnd; y++) {
    const rowBase = (y * width) * 4;
    for (let x = px; x < xEnd; x++) {
      const i = rowBase + x * 4;
      const a = pixels[i + 3];
      if (a < 128) continue;  // skip transparent
      r += pixels[i];
      g += pixels[i + 1];
      b += pixels[i + 2];
      count++;
    }
  }
  if (count === 0) return null;
  return { r: (r / count) | 0, g: (g / count) | 0, b: (b / count) | 0 };
}

function colorDistance(a, b) {
  if (!a || !b) return 65535;  // force "different" if either is transparent
  const dr = a.r - b.r, dg = a.g - b.g, db = a.b - b.b;
  return dr*dr + dg*dg + db*db;
}

function averageColor(a, b) {
  // Average two colors; if either is null, use the other.
  if (!a) return b;
  if (!b) return a;
  return { r: ((a.r + b.r) / 2) | 0, g: ((a.g + b.g) / 2) | 0, b: ((a.b + b.b) / 2) | 0 };
}

// --- ANSI escape helpers ----------------------------------------------------
const ESC = '\x1b';
function fgTruecolor(r, g, b) { return `${ESC}[38;2;${r};${g};${b}m`; }
function bgTruecolor(r, g, b) { return `${ESC}[48;2;${r};${g};${b}m`; }
function fgXterm256(idx) { return `${ESC}[38;5;${idx}m`; }
function bgXterm256(idx) { return `${ESC}[48;5;${idx}m`; }
const RESET = `${ESC}[0m`;
const CURSOR_HOME = `${ESC}[H`;

// --- Main render function ---------------------------------------------------
//
// Output layout:
//   - If isTTY: starts with \033[H (cursor home) so each frame overwrites the previous.
//   - Each terminal row ends with \n.
//   - Each frame ends with RESET + \n (so the shell prompt doesn't pick up the last color).
//   - If NOT isTTY: no \033[H, no ANSI escapes (when color=false), frames are
//     just rows of characters separated by blank lines.
//
// v0.8.17: sub-pixel mode (options.subpixel = true). When enabled, each logical cell
// samples a 2×2 pixel block (TL, TR, BL, BR) and emits TWO terminal columns side-by-side.
// The LEFT terminal column represents the LEFT pixel column (TL top, BL bottom) — uses
// the existing ▀/█/space half-block logic. The RIGHT column does the same for TR/BR.
// This doubles horizontal resolution at the cost of halving terminal width.
// Sub-pixel + ASCII mode: silently ignored (ASCII can't do half-blocks).
//
function renderToTerminal(pixels, width, height, options) {
  if (!options) options = {};
  const mode = options.mode === 'ascii' ? 'ascii' : 'unicode';
  const color = !!options.color;
  const isTTY = options.isTTY !== undefined ? !!options.isTTY : !!process.stdout.isTTY;
  const subpixel = !!options.subpixel && mode === 'unicode';  // ignored in ASCII mode
  // Determine if we should use 256-color quantization. We do this only when
  // COLORTERM did not say truecolor — detected by sniffing TERM for 256color.
  const isTruecolor = (process.env.COLORTERM || '').toLowerCase().includes('truecolor') ||
                      (process.env.COLORTERM || '').toLowerCase().includes('24bit') ||
                      process.platform === 'win32';
  const use256 = color && !isTruecolor;

  // Terminal dimensions (cells).
  const termW = options.termWidth || (process.stdout.columns || 80);
  const termH = options.termHeight || (process.stdout.rows || 24);
  // Each cell is 1 char wide and represents 2 pixel rows (half-block doubling).
  // So we need termW × (termH * 2) pixels from the source buffer.
  // In sub-pixel mode, each logical cell is 2 terminal columns wide, so the
  // effective number of logical columns is floor(termW / 2).
  const effectiveTermW = subpixel ? Math.floor(termW / 2) : termW;
  // Compute the source-pixel block size per cell.
  const cellW = Math.max(1, Math.floor(width / effectiveTermW));
  const cellH = Math.max(1, Math.floor(height / (termH * 2)));

  // In sub-pixel mode, each cell's source block is split into LEFT and RIGHT halves
  // (each cellW/2 pixels wide). The LEFT half drives the left terminal column;
  // the RIGHT half drives the right terminal column.
  const subCellW = subpixel ? Math.max(1, Math.floor(cellW / 2)) : cellW;

  // Build a 2D grid of cell color pairs per cell.
  // In non-subpixel mode: each cell has {upper, lower}.
  // In sub-pixel mode: each cell has {leftUpper, leftLower, rightUpper, rightLower}.
  // Grid dimensions: cols × rows, where cols <= effectiveTermW and rows <= termH.
  const cols = Math.min(effectiveTermW, Math.floor(width / cellW));
  const rows = Math.min(termH, Math.floor(height / (cellH * 2)));

  // Pre-compute per-cell color pairs.
  const cells = new Array(cols * rows);
  for (let row = 0; row < rows; row++) {
    for (let col = 0; col < cols; col++) {
      const px = col * cellW;
      const pyUpper = row * cellH * 2;
      const pyLower = pyUpper + cellH;
      if (subpixel) {
        // Sample 4 sub-regions: left-upper, left-lower, right-upper, right-lower.
        const leftUpper = sampleCellColor(pixels, width, height, px, pyUpper, subCellW, cellH);
        const leftLower = sampleCellColor(pixels, width, height, px, pyLower, subCellW, cellH);
        const rightUpper = sampleCellColor(pixels, width, height, px + subCellW, pyUpper, subCellW, cellH);
        const rightLower = sampleCellColor(pixels, width, height, px + subCellW, pyLower, subCellW, cellH);
        cells[row * cols + col] = { leftUpper, leftLower, rightUpper, rightLower };
      } else {
        const upper = sampleCellColor(pixels, width, height, px, pyUpper, cellW, cellH);
        const lower = sampleCellColor(pixels, width, height, px, pyLower, cellW, cellH);
        cells[row * cols + col] = { upper, lower };
      }
    }
  }

  // Render the grid to a string.
  // ASCII mode: 1 char per cell (luminance-mapped). Sub-pixel is ignored.
  // Unicode mode: 1 char per cell (half-block or full-block), grouped runs.
  //   Sub-pixel mode: 2 chars per cell (left column + right column).
  const parts = [];
  if (isTTY) parts.push(CURSOR_HOME);

  if (mode === 'ascii') {
    const density = ' .:-=+#@';
    for (let row = 0; row < rows; row++) {
      let line = '';
      for (let col = 0; col < cols; col++) {
        const cell = cells[row * cols + col];
        // ASCII mode uses the average of upper+lower (or whichever is opaque).
        const c = averageColor(cell.upper, cell.lower);
        if (!c) { line += ' '; continue; }
        const lum = 0.299 * c.r + 0.587 * c.g + 0.114 * c.b;
        const pct = lum / 255;
        const idx = Math.min(density.length - 1, Math.floor(pct * density.length));
        line += density[idx];
      }
      parts.push(line + '\n');
    }
    if (color) parts.push(RESET);
    return parts.join('');
  }

  // Unicode mode with optional color.
  // For each row, emit one string. Group consecutive cells with identical
  // fg+bg color pairs into a single escape + run of chars.
  // v0.8.17: in sub-pixel mode, each cell emits TWO chars (left + right column).
  //   Per-cell heuristic: sample 4 regions (TL, TR, BL, BR).
  //   - If horizontal contrast > vertical contrast: emit ▌▐
  //     (left char ▌ with fg=avg(TL,BL); right char ▐ with fg=avg(TR,BR)).
  //     This sacrifices vertical resolution for horizontal detail.
  //   - Else: emit ▀▀ (left char ▀ with fg=TL/bg=BL; right char ▀ with fg=TR/bg=BR).
  //     This preserves vertical resolution (existing ▀ logic per column).
  const SPACE = ' ';
  const FULL = '\u2588';   // █
  const UPPER = '\u2580';  // ▀ — fg=upper, bg=lower
  const LOWER = '\u2584';  // ▄ — fg=lower, bg=upper
  // v0.8.17: sub-pixel left/right half-blocks.
  // ▌ (U+258C) — left half block: left half of cell is fg color, right half is bg (or default).
  // ▐ (U+2590) — right half block: right half of cell is fg color, left half is bg (or default).
  // When emitted as a pair ▌▐ with fg=leftColor on ▌ and fg=rightColor on ▐, the pair
  // shows leftColor in the left column and rightColor in the right column.
  const LEFT_HALF = '\u258C';   // ▌
  const RIGHT_HALF = '\u2590';  // ▐

  // Helper: given (upper, lower) color pair, compute (char, fg, bg, fgKey, bgKey).
  // Used for both non-subpixel cells and sub-pixel sub-columns (vertical split mode).
  function cellToChar(upper, lower) {
    if (!upper && !lower) {
      return { ch: SPACE, fg: null, bg: null, fgKey: '', bgKey: '' };
    }
    if (colorDistance(upper, lower) < 30 * 30) {
      const avg = averageColor(upper, lower);
      return { ch: FULL, fg: avg, bg: null, fgKey: `${avg.r},${avg.g},${avg.b}`, bgKey: '' };
    }
    if (!upper) {
      return { ch: LOWER, fg: lower, bg: null, fgKey: lower ? `${lower.r},${lower.g},${lower.b}` : '', bgKey: '' };
    }
    if (!lower) {
      return { ch: UPPER, fg: upper, bg: null, fgKey: `${upper.r},${upper.g},${upper.b}`, bgKey: '' };
    }
    return {
      ch: UPPER, fg: upper, bg: lower,
      fgKey: `${upper.r},${upper.g},${upper.b}`,
      bgKey: `${lower.r},${lower.g},${lower.b}`,
    };
  }

  // v0.8.17: helper for sub-pixel horizontal split mode.
  // Given (leftColor, rightColor), compute (char, fg, bg, fgKey, bgKey) for the LEFT
  // terminal column. The LEFT column emits ▌ with fg=leftColor. If leftColor is null
  // (transparent), emit SPACE. The RIGHT column emits ▐ with fg=rightColor (handled
  // separately by the caller — this function only computes the LEFT char).
  function leftHalfChar(leftColor, rightColor) {
    if (!leftColor && !rightColor) {
      return { ch: SPACE, fg: null, bg: null, fgKey: '', bgKey: '' };
    }
    if (!leftColor) {
      // Left is transparent — emit ▐ (right half) so the right color fills the right half.
      // Actually for the LEFT column, we want to show nothing on the left half. Emit SPACE.
      return { ch: SPACE, fg: null, bg: null, fgKey: '', bgKey: '' };
    }
    return {
      ch: LEFT_HALF, fg: leftColor, bg: null,
      fgKey: `${leftColor.r},${leftColor.g},${leftColor.b}`, bgKey: '',
    };
  }
  function rightHalfChar(leftColor, rightColor) {
    if (!leftColor && !rightColor) {
      return { ch: SPACE, fg: null, bg: null, fgKey: '', bgKey: '' };
    }
    if (!rightColor) {
      // Right is transparent — emit ▌ (left half) so the left color fills the left half.
      // Actually for the RIGHT column, we want to show nothing on the right half. Emit SPACE.
      return { ch: SPACE, fg: null, bg: null, fgKey: '', bgKey: '' };
    }
    return {
      ch: RIGHT_HALF, fg: rightColor, bg: null,
      fgKey: `${rightColor.r},${rightColor.g},${rightColor.b}`, bgKey: '',
    };
  }

  for (let row = 0; row < rows; row++) {
    let line = '';
    let runChar = '';
    let runCount = 0;
    let runFg = null;  // {r,g,b} or null (no fg)
    let runBg = null;  // {r,g,b} or null (no bg)
    let runFgKey = '';
    let runBgKey = '';

    function flushRun() {
      if (runCount === 0) return;
      let prefix = '';
      if (color) {
        if (runFg !== null) {
          prefix += use256
            ? fgXterm256(rgbToXterm256(runFg.r, runFg.g, runFg.b))
            : fgTruecolor(runFg.r, runFg.g, runFg.b);
        }
        if (runBg !== null) {
          prefix += use256
            ? bgXterm256(rgbToXterm256(runBg.r, runBg.g, runBg.b))
            : bgTruecolor(runBg.r, runBg.g, runBg.b);
        }
      }
      line += prefix + runChar.repeat(runCount);
      if (color && (runFg !== null || runBg !== null)) line += RESET;
      runCount = 0;
      runChar = '';
      runFg = null;
      runBg = null;
      runFgKey = '';
      runBgKey = '';
    }

    function emitChar(ch, fg, bg, fgKey, bgKey) {
      if (ch === runChar && fgKey === runFgKey && bgKey === runBgKey) {
        runCount++;
      } else {
        flushRun();
        runChar = ch;
        runCount = 1;
        runFg = fg;
        runBg = bg;
        runFgKey = fgKey;
        runBgKey = bgKey;
      }
    }

    for (let col = 0; col < cols; col++) {
      const cell = cells[row * cols + col];
      if (subpixel) {
        // v0.8.17: sub-pixel mode. Sample 4 regions already in cell: {leftUpper, leftLower, rightUpper, rightLower}.
        // Compute horizontal vs vertical contrast to decide split mode.
        const leftAvg = averageColor(cell.leftUpper, cell.leftLower);
        const rightAvg = averageColor(cell.rightUpper, cell.rightLower);
        const topAvg = averageColor(cell.leftUpper, cell.rightUpper);
        const botAvg = averageColor(cell.leftLower, cell.rightLower);
        const hContrast = colorDistance(leftAvg, rightAvg);
        const vContrast = colorDistance(topAvg, botAvg);
        if (hContrast > vContrast && (leftAvg || rightAvg)) {
          // Horizontal split: emit ▌▐ — left char shows left color, right char shows right color.
          const lc = leftHalfChar(leftAvg, rightAvg);
          const rc = rightHalfChar(leftAvg, rightAvg);
          emitChar(lc.ch, lc.fg, lc.bg, lc.fgKey, lc.bgKey);
          emitChar(rc.ch, rc.fg, rc.bg, rc.fgKey, rc.bgKey);
        } else {
          // Vertical split: emit ▀▀ — each column uses existing ▀ logic on its (upper, lower) pair.
          const lc = cellToChar(cell.leftUpper, cell.leftLower);
          const rc = cellToChar(cell.rightUpper, cell.rightLower);
          emitChar(lc.ch, lc.fg, lc.bg, lc.fgKey, lc.bgKey);
          emitChar(rc.ch, rc.fg, rc.bg, rc.fgKey, rc.bgKey);
        }
      } else {
        const upper = cell.upper;
        const lower = cell.lower;
        const { ch, fg, bg, fgKey, bgKey } = cellToChar(upper, lower);
        emitChar(ch, fg, bg, fgKey, bgKey);
      }
    }
    flushRun();
    parts.push(line + '\n');
  }
  if (color) parts.push(RESET);
  return parts.join('');
}

module.exports = { renderToTerminal, detectColorSupport };
