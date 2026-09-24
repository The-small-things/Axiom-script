// kbd.c — live keyboard input for terminal mode.
//
// A terminal reports key presses, never releases, so "held" is inferred: a movement key counts
// as held for HOLD_MOVE seconds after its last press, which the terminal's auto-repeat keeps
// renewing while the key is down (the gap before auto-repeat starts is 250–500 ms on common
// terminals, hence the length). Jump and fire are taps, held long enough for at least one
// 60 Hz physics step to see them. Pressing a direction cancels its opposite, so reversing
// is immediate.
//
//   W A S D, arrow keys   input.move     (x: right, y: forward)
//   space                 input.jump
//   F, Enter              input.fire
//   q, Esc, Ctrl-C        quit

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <termios.h>
#include "term.h"

#define HOLD_MOVE 0.35
#define HOLD_TAP 0.12

void ax_kbd_feed(AxKbd *k, const unsigned char *b, int n, double now) {
  for (int i = 0; i < n; i++) {
    unsigned char c = b[i];
    if (c == 0x1b) {
      // An escape sequence (arrows: ESC [ A..D, or ESC O A..D) or a lone Esc.
      if (i + 2 < n && (b[i + 1] == '[' || b[i + 1] == 'O')) {
        unsigned char d = b[i + 2];
        i += 2;
        if (d == 'A') { k->up = now + HOLD_MOVE; k->down = 0; }
        else if (d == 'B') { k->down = now + HOLD_MOVE; k->up = 0; }
        else if (d == 'C') { k->right = now + HOLD_MOVE; k->left = 0; }
        else if (d == 'D') { k->left = now + HOLD_MOVE; k->right = 0; }
        else {
          // Some other sequence (F-keys, Home…): skip to its final byte.
          while (i + 1 < n && !(b[i] >= 0x40 && b[i] <= 0x7e)) i++;
        }
        continue;
      }
      k->quit = true;
      continue;
    }
    switch (c) {
      case 'w': case 'W': k->up = now + HOLD_MOVE; k->down = 0; break;
      case 's': case 'S': k->down = now + HOLD_MOVE; k->up = 0; break;
      case 'a': case 'A': k->left = now + HOLD_MOVE; k->right = 0; break;
      case 'd': case 'D': k->right = now + HOLD_MOVE; k->left = 0; break;
      case ' ': k->jump = now + HOLD_TAP; break;
      case 'f': case 'F': case '\r': case '\n': k->fire = now + HOLD_TAP; break;
      case 'q': case 'Q': case 0x03: case 0x04: k->quit = true; break;
      default: break;
    }
  }
}

void ax_kbd_state(const AxKbd *k, double now, double *mx, double *my, bool *jump, bool *fire) {
  *mx = (k->right > now ? 1 : 0) - (k->left > now ? 1 : 0);
  *my = (k->up > now ? 1 : 0) - (k->down > now ? 1 : 0);
  *jump = k->jump > now;
  *fire = k->fire > now;
}

// ---- the terminal ----------------------------------------------------------------------------

static struct termios saved;
static bool raw_on;
static volatile sig_atomic_t interrupted;

static void on_signal(int sig) { (void)sig; interrupted = 1; }

void ax_kbd_restore(void) {
  if (!raw_on) return;
  tcsetattr(STDIN_FILENO, TCSANOW, &saved);
  raw_on = false;
  fputs("\x1b[0m\x1b[?25h\n", stdout);   // attributes off, cursor back
  fflush(stdout);
}

bool ax_kbd_enable(void) {
  if (!isatty(STDIN_FILENO)) return false;
  if (tcgetattr(STDIN_FILENO, &saved) != 0) return false;
  struct termios t = saved;
  t.c_lflag &= ~(ICANON | ECHO);   // byte at a time, unechoed; ISIG stays, so Ctrl-C signals
  t.c_cc[VMIN] = 0;                // read() returns at once with whatever is there
  t.c_cc[VTIME] = 0;
  if (tcsetattr(STDIN_FILENO, TCSANOW, &t) != 0) return false;
  raw_on = true;
  atexit(ax_kbd_restore);
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = on_signal;
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGTERM, &sa, NULL);
  sigaction(SIGHUP, &sa, NULL);
  return true;
}

void ax_kbd_poll(AxKbd *k, double now) {
  unsigned char buf[256];
  ssize_t n;
  while ((n = read(STDIN_FILENO, buf, sizeof buf)) > 0) ax_kbd_feed(k, buf, (int)n, now);
  if (interrupted) k->quit = true;
}
