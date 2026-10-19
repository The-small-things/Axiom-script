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

// Live keyboard input (kbd.c). Times are seconds on any monotonic clock; a key is "held" until
// the time stored for it.
typedef struct { double up, down, left, right, jump, fire; bool quit; } AxKbd;
bool ax_kbd_enable(void);                       // stdin to raw mode; false if it is not a terminal
void ax_kbd_restore(void);                      // also runs at exit
void ax_kbd_poll(AxKbd *k, double now);         // read whatever keys are waiting
void ax_kbd_feed(AxKbd *k, const unsigned char *bytes, int n, double now);
void ax_kbd_state(const AxKbd *k, double now, double *mx, double *my, bool *jump, bool *fire);

// Keys without waiting (kbd.c; keyboard.js is the reference): poll_key() and keys_down().
bool ax_keys_poll(double now, char out[16]);                // the next key's name, if one waits
int ax_keys_down(double now, char out[][16], int cap);      // the keys held now
void ax_keys_feed(const unsigned char *bytes, int n, double now);
void ax_keys_external(void);                    // a live world's terminal owns stdin
#endif
