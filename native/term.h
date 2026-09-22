// term.h — terminal display backend (see term.c).
#ifndef AXIOM_TERM_H
#define AXIOM_TERM_H
#include <stdbool.h>
#include <stdint.h>

typedef struct {
  bool unicode;       // half-blocks; false → ASCII density characters
  bool color;         // ANSI colour (24-bit, or xterm-256 unless COLORTERM says truecolor)
  bool subpixel;      // ▌▐ halves: double horizontal resolution (unicode only)
  int term_width, term_height;
  bool is_tty;        // prefix each frame with cursor-home
} AxTermOptions;

void ax_term_detect(bool *unicode, bool *color);
char *ax_term_render(const uint8_t *rgba, int width, int height, const AxTermOptions *opt);   // malloc'd
#endif
