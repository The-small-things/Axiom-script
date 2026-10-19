// term.c — the terminal display backend, ported from terminal.js.
//
// A pure function from an RGBA pixel buffer to text: Unicode half-blocks (▀ with foreground =
// upper pixel, background = lower) or ASCII density characters, with 24-bit or xterm-256
// colour, and the optional ▌▐ sub-pixel mode. Runs of identical cells share one escape
// sequence. Output is byte-identical to terminal.js for the same buffer and options, which
// tests/term checks.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <math.h>
#include "term.h"

// ---- colour support --------------------------------------------------------------------------

static bool env_has(const char *name, const char *needle) {
  const char *v = getenv(name);
  if (!v) return false;
  char low[256];
  size_t n = strlen(v);
  if (n >= sizeof low) n = sizeof low - 1;
  for (size_t i = 0; i < n; i++) low[i] = (char)((v[i] >= 'A' && v[i] <= 'Z') ? v[i] + 32 : v[i]);
  low[n] = '\0';
  return strstr(low, needle) != NULL;
}

void ax_term_detect(bool *unicode, bool *color) {
  const char *nc = getenv("NO_COLOR");
  if (nc && *nc) { *unicode = false; *color = false; return; }
  if (env_has("COLORTERM", "truecolor") || env_has("COLORTERM", "24bit")) { *unicode = true; *color = true; return; }
  if (env_has("TERM", "256color")) { *unicode = true; *color = true; return; }
  *unicode = false;
  *color = false;
}

// ---- helpers ---------------------------------------------------------------------------------

typedef struct { int r, g, b; bool ok; } Col;

static const int CUBE6[6] = { 0, 95, 135, 175, 215, 255 };

static int to_xterm256(int r, int g, int b) {
  int best = 16;
  double best_d = INFINITY;
  for (int ri = 0; ri < 6; ri++) for (int gi = 0; gi < 6; gi++) for (int bi = 0; bi < 6; bi++) {
    int dr = r - CUBE6[ri], dg = g - CUBE6[gi], db = b - CUBE6[bi];
    double d = dr * dr + dg * dg + db * db;
    if (d < best_d) { best_d = d; best = 16 + (ri * 36 + gi * 6 + bi); }
  }
  // Math.round on a value that may be negative: round half up.
  int gray = (int)floor(((r + g + b) / 3.0 - 8) / 10 + 0.5);
  if (gray < 0) gray = 0;
  if (gray > 23) gray = 23;
  int grgb = 8 + gray * 10;
  int gr = r - grgb, gg = g - grgb, gb = b - grgb;
  if (gr * gr + gg * gg + gb * gb < best_d) return 232 + gray;
  return best;
}

static Col sample(const uint8_t *px, int w, int h, int x0, int y0, int bw, int bh) {
  long r = 0, g = 0, b = 0, n = 0;
  int x1 = x0 + bw < w ? x0 + bw : w, y1 = y0 + bh < h ? y0 + bh : h;
  for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) {
    const uint8_t *p = px + ((size_t)y * w + x) * 4;
    if (p[3] < 128) continue;
    r += p[0]; g += p[1]; b += p[2]; n++;
  }
  Col c = { 0, 0, 0, false };
  if (!n) return c;
  c.r = (int)(r / n); c.g = (int)(g / n); c.b = (int)(b / n); c.ok = true;   // (x / n) | 0
  return c;
}

static int dist2(Col a, Col b) {
  if (!a.ok || !b.ok) return 65535;
  int dr = a.r - b.r, dg = a.g - b.g, db = a.b - b.b;
  return dr * dr + dg * dg + db * db;
}

static Col avg(Col a, Col b) {
  if (!a.ok) return b;
  if (!b.ok) return a;
  Col c = { (a.r + b.r) / 2, (a.g + b.g) / 2, (a.b + b.b) / 2, true };
  return c;
}

typedef struct { char *buf; size_t len, cap; } Out;
static void put(Out *o, const char *s, size_t n) {
  if (o->len + n + 1 > o->cap) {
    while (o->len + n + 1 > o->cap) o->cap = o->cap ? o->cap * 2 : 4096;
    o->buf = realloc(o->buf, o->cap);
  }
  memcpy(o->buf + o->len, s, n);
  o->len += n;
  o->buf[o->len] = '\0';
}
static void puts_(Out *o, const char *s) { put(o, s, strlen(s)); }

// One output cell: a glyph plus optional foreground/background colours.
typedef struct { const char *ch; Col fg, bg; } Cell;

#define GLYPH_SPACE " "
#define GLYPH_FULL "\xE2\x96\x88"    // █
#define GLYPH_UPPER "\xE2\x96\x80"   // ▀
#define GLYPH_LOWER "\xE2\x96\x84"   // ▄
#define GLYPH_LEFT "\xE2\x96\x8C"    // ▌
#define GLYPH_RIGHT "\xE2\x96\x90"   // ▐

static const Col NONE = { 0, 0, 0, false };

static Cell cell_char(Col up, Col lo) {
  Cell c = { GLYPH_SPACE, NONE, NONE };
  if (!up.ok && !lo.ok) return c;
  if (dist2(up, lo) < 30 * 30) { c.ch = GLYPH_FULL; c.fg = avg(up, lo); return c; }
  if (!up.ok) { c.ch = GLYPH_LOWER; c.fg = lo; return c; }
  if (!lo.ok) { c.ch = GLYPH_UPPER; c.fg = up; return c; }
  c.ch = GLYPH_UPPER; c.fg = up; c.bg = lo;
  return c;
}

typedef struct {
  Out *o;
  bool color, use256;
  const char *ch;
  int count;
  Col fg, bg;
} Run;

static bool same(Col a, Col b) { return a.ok == b.ok && (!a.ok || (a.r == b.r && a.g == b.g && a.b == b.b)); }

static void flush(Run *r) {
  if (!r->count) return;
  char esc[64];
  if (r->color) {
    if (r->fg.ok) {
      if (r->use256) snprintf(esc, sizeof esc, "\x1b[38;5;%dm", to_xterm256(r->fg.r, r->fg.g, r->fg.b));
      else snprintf(esc, sizeof esc, "\x1b[38;2;%d;%d;%dm", r->fg.r, r->fg.g, r->fg.b);
      puts_(r->o, esc);
    }
    if (r->bg.ok) {
      if (r->use256) snprintf(esc, sizeof esc, "\x1b[48;5;%dm", to_xterm256(r->bg.r, r->bg.g, r->bg.b));
      else snprintf(esc, sizeof esc, "\x1b[48;2;%d;%d;%dm", r->bg.r, r->bg.g, r->bg.b);
      puts_(r->o, esc);
    }
  }
  for (int i = 0; i < r->count; i++) puts_(r->o, r->ch);
  if (r->color && (r->fg.ok || r->bg.ok)) puts_(r->o, "\x1b[0m");
  r->count = 0;
  r->ch = "";
  r->fg = NONE;
  r->bg = NONE;
}

static void emit(Run *r, Cell c) {
  if (r->count && strcmp(c.ch, r->ch) == 0 && same(c.fg, r->fg) && same(c.bg, r->bg)) { r->count++; return; }
  flush(r);
  r->ch = c.ch;
  r->count = 1;
  r->fg = c.fg;
  r->bg = c.bg;
}

char *ax_term_render(const uint8_t *px, int width, int height, const AxTermOptions *opt) {
  bool unicode = opt->unicode;
  bool color = opt->color;
  bool subpixel = opt->subpixel && unicode;
  bool truecolor = env_has("COLORTERM", "truecolor") || env_has("COLORTERM", "24bit");
  bool use256 = color && !truecolor;
  int termW = opt->term_width > 0 ? opt->term_width : 80;
  int termH = opt->term_height > 0 ? opt->term_height : 24;
  int effW = subpixel ? termW / 2 : termW;
  int cellW = width / effW; if (cellW < 1) cellW = 1;
  int cellH = height / (termH * 2); if (cellH < 1) cellH = 1;
  int subW = subpixel ? (cellW / 2 < 1 ? 1 : cellW / 2) : cellW;
  int cols = width / cellW < effW ? width / cellW : effW;
  int rows = height / (cellH * 2) < termH ? height / (cellH * 2) : termH;

  Out o = { 0 };
  puts_(&o, "");
  if (opt->is_tty) puts_(&o, "\x1b[H");
  if (!unicode) {
    static const char density[] = " .:-=+#@";
    for (int row = 0; row < rows; row++) {
      for (int col = 0; col < cols; col++) {
        int x = col * cellW, yu = row * cellH * 2, yl = yu + cellH;
        Col c = avg(sample(px, width, height, x, yu, cellW, cellH), sample(px, width, height, x, yl, cellW, cellH));
        if (!c.ok) { puts_(&o, " "); continue; }
        double lum = 0.299 * c.r + 0.587 * c.g + 0.114 * c.b;
        int idx = (int)floor(lum / 255 * 8);
        if (idx > 7) idx = 7;
        put(&o, &density[idx], 1);
      }
      puts_(&o, "\n");
    }
    if (color) puts_(&o, "\x1b[0m");
    return o.buf;
  }
  for (int row = 0; row < rows; row++) {
    Run run = { &o, color, use256, "", 0, NONE, NONE };
    for (int col = 0; col < cols; col++) {
      int x = col * cellW, yu = row * cellH * 2, yl = yu + cellH;
      if (subpixel) {
        Col lu = sample(px, width, height, x, yu, subW, cellH), ll = sample(px, width, height, x, yl, subW, cellH);
        Col ru = sample(px, width, height, x + subW, yu, subW, cellH), rl = sample(px, width, height, x + subW, yl, subW, cellH);
        Col la = avg(lu, ll), ra = avg(ru, rl), ta = avg(lu, ru), ba = avg(ll, rl);
        int hc = dist2(la, ra), vc = dist2(ta, ba);
        if (hc > vc && (la.ok || ra.ok)) {
          Cell lc = { GLYPH_SPACE, NONE, NONE }, rc = { GLYPH_SPACE, NONE, NONE };
          if (la.ok) { lc.ch = GLYPH_LEFT; lc.fg = la; }
          if (ra.ok) { rc.ch = GLYPH_RIGHT; rc.fg = ra; }
          emit(&run, lc);
          emit(&run, rc);
        } else {
          emit(&run, cell_char(lu, ll));
          emit(&run, cell_char(ru, rl));
        }
      } else {
        emit(&run, cell_char(sample(px, width, height, x, yu, cellW, cellH), sample(px, width, height, x, yl, cellW, cellH)));
      }
    }
    flush(&run);
    puts_(&o, "\n");
  }
  if (color) puts_(&o, "\x1b[0m");
  return o.buf;
}
