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
#include <unistd.h>
#include "term.h"
#ifndef __wasi__
#include <signal.h>
#include <termios.h>
#endif

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

#ifdef __wasi__
// WASI has no terminal control: there is no live keyboard, and a run takes its input from
// --input (or the embedding host's axiom_set_input).
void ax_kbd_restore(void) {}
bool ax_kbd_enable(void) { return false; }
void ax_kbd_poll(AxKbd *k, double now) { (void)k; (void)now; }
#else
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
  ax_keys_external();
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
  while ((n = read(STDIN_FILENO, buf, sizeof buf)) > 0) {
    ax_kbd_feed(k, buf, (int)n, now);
    ax_keys_feed(buf, (int)n, now);   // the same keys, for poll_key() and keys_down()
  }
  if (interrupted) k->quit = true;
}
#endif

// ---- keys without waiting: poll_key() and keys_down() ----------------------------------------
// keyboard.js is the reference: the same decoding of bytes into key names, the same queue and
// the same hold time. Keys come from stdin, read without blocking (a terminal switched to
// unechoed byte-at-a-time mode, a pipe, a file), or, while a world runs live in the terminal,
// from the bytes ax_kbd_poll() reads.

#define KEYS_HOLD 0.35
#define KEYS_QUEUE 256
#define KEYS_HELD 64

typedef struct { char name[16]; double until; } KeyHeld;
static struct {
  int source;                 // 0 not opened, 1 terminal, 2 pipe, 3 file, 4 none, 5 external
  bool eof, last_cr;
  unsigned char *pending;      // bytes read but not yet decoded
  int npending, cappending;
  char queue[KEYS_QUEUE][16];
  int qhead, qlen;
  KeyHeld held[KEYS_HELD];    // in order of last press, oldest first
  int nheld;
} K;

static void keys_press(const char *name, double now) {
  if (K.qlen == KEYS_QUEUE) { K.qhead = (K.qhead + 1) % KEYS_QUEUE; K.qlen--; }
  snprintf(K.queue[(K.qhead + K.qlen) % KEYS_QUEUE], 16, "%s", name);
  K.qlen++;
  for (int i = 0; i < K.nheld; i++) if (!strcmp(K.held[i].name, name)) {
    memmove(&K.held[i], &K.held[i + 1], sizeof(KeyHeld) * (size_t)(K.nheld - i - 1));
    K.nheld--;
    break;
  }
  if (K.nheld == KEYS_HELD) { memmove(&K.held[0], &K.held[1], sizeof(KeyHeld) * (KEYS_HELD - 1)); K.nheld--; }
  snprintf(K.held[K.nheld].name, 16, "%s", name);
  K.held[K.nheld++].until = now + KEYS_HOLD;
}

static const char *csi_name(unsigned char fin, const unsigned char *param, int nparam) {
  switch (fin) {
    case 'A': return "up";
    case 'B': return "down";
    case 'C': return "right";
    case 'D': return "left";
    case 'H': return "home";
    case 'F': return "end";
    case '~': {
      // parseInt of the parameter bytes: leading whitespace, a sign, then digits.
      int i = 0, sign = 1;
      long v = 0;
      bool any = false;
      while (i < nparam && (param[i] == ' ' || param[i] == '\t' || param[i] == '\n' || param[i] == '\r' || param[i] == '\f' || param[i] == '\v' || param[i] == 0xa0)) i++;
      if (i < nparam && (param[i] == '+' || param[i] == '-')) { if (param[i] == '-') sign = -1; i++; }
      while (i < nparam && param[i] >= '0' && param[i] <= '9') { if (v < 1000) v = v * 10 + (param[i] - '0'); any = true; i++; }
      if (!any) return NULL;
      v *= sign;
      static const char *const tilde[] = { NULL, "home", "insert", "delete", "end", "pageup", "pagedown", "home", "end" };
      return v >= 1 && v <= 8 ? tilde[v] : NULL;
    }
    default: return NULL;
  }
}

static void keys_decode(double now) {
  const unsigned char *b = K.pending;
  int n = K.npending, i = 0;
  char one[16];
  while (i < n) {
    unsigned char c = b[i];
    if (c == 0x1b) {
      K.last_cr = false;
      if (i + 1 < n && (b[i + 1] == '[' || b[i + 1] == 'O')) {
        int j = i + 2;
        while (j < n && !(b[j] >= 0x40 && b[j] <= 0x7e)) j++;
        if (j >= n) { i = n; break; }   // cut off: dropped
        const char *name = csi_name(b[j], b + i + 2, j - i - 2);
        if (name) keys_press(name, now);
        i = j + 1;
        continue;
      }
      keys_press("escape", now);
      i++;
      continue;
    }
    if (c == 0x0a && K.last_cr) { K.last_cr = false; i++; continue; }   // CR LF is one enter
    K.last_cr = c == 0x0d;
    if (c == 0x0d || c == 0x0a) { keys_press("enter", now); i++; continue; }
    if (c == 0x09) { keys_press("tab", now); i++; continue; }
    if (c == 0x7f || c == 0x08) { keys_press("backspace", now); i++; continue; }
    if (c == 0x20) { keys_press("space", now); i++; continue; }
    if (c < 0x20) {
      if (c == 0) keys_press("ctrl+space", now);
      else { snprintf(one, sizeof one, "ctrl+%c", c <= 0x1a ? c + 0x60 : c + 0x40); keys_press(one, now); }
      i++;
      continue;
    }
    if (c < 0x80) { one[0] = (char)c; one[1] = '\0'; keys_press(one, now); i++; continue; }
    // UTF-8: a well-formed sequence is one key; anything else is U+FFFD, one byte at a time.
    int len = c >= 0xf0 && c <= 0xf4 ? 4 : c >= 0xe0 ? (c <= 0xef ? 3 : 0) : c >= 0xc2 ? 2 : 0;
    long cp = -1;
    if (len && i + len <= n) {
      cp = len == 2 ? c & 0x1f : len == 3 ? c & 0x0f : c & 0x07;
      for (int k = 1; k < len; k++) {
        if ((b[i + k] & 0xc0) != 0x80) { cp = -1; break; }
        cp = (cp << 6) | (b[i + k] & 0x3f);
      }
      if (cp >= 0 && ((len == 3 && (cp < 0x800 || (cp >= 0xd800 && cp <= 0xdfff))) || (len == 4 && (cp < 0x10000 || cp > 0x10ffff)))) cp = -1;
    }
    if (cp < 0) { keys_press("\xef\xbf\xbd", now); i++; continue; }
    memcpy(one, b + i, (size_t)len);
    one[len] = '\0';
    keys_press(one, now);
    i += len;
  }
  if (i) memmove(K.pending, K.pending + i, (size_t)(n - i));
  K.npending = n - i;
}

static void keys_add(const unsigned char *bytes, int n) {
  if (K.npending + n > K.cappending) {
    K.cappending = (K.npending + n) * 2;
    K.pending = realloc(K.pending, (size_t)K.cappending);
  }
  memcpy(K.pending + K.npending, bytes, (size_t)n);
  K.npending += n;
}

void ax_keys_feed(const unsigned char *bytes, int n, double now) {
  keys_add(bytes, n);
  keys_decode(now);
}

void ax_keys_external(void) { K.source = 5; }

#ifdef __wasi__
static void keys_read(double now) { (void)now; if (!K.source) K.source = 4; }
#else
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
static struct termios keys_saved;
static int keys_saved_flags = -1;

static void keys_restore(void) {
  if (K.source == 1) tcsetattr(STDIN_FILENO, TCSANOW, &keys_saved);
  if (K.source == 2 && keys_saved_flags >= 0) fcntl(STDIN_FILENO, F_SETFL, keys_saved_flags);
}
static void keys_on_signal(int sig) {
  keys_restore();
  signal(sig, SIG_DFL);
  raise(sig);
}

static void keys_open(void) {
  struct stat st;
  if (fstat(STDIN_FILENO, &st) != 0) { K.source = 4; return; }
  if (isatty(STDIN_FILENO)) {
    if (tcgetattr(STDIN_FILENO, &keys_saved) != 0) { K.source = 4; return; }
    struct termios t = keys_saved;
    t.c_lflag &= ~(ICANON | ECHO);   // byte at a time, unechoed; ISIG stays, so Ctrl-C interrupts
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &t) != 0) { K.source = 4; return; }
    K.source = 1;
    atexit(keys_restore);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = keys_on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
  } else if (S_ISFIFO(st.st_mode) || S_ISSOCK(st.st_mode)) {
    keys_saved_flags = fcntl(STDIN_FILENO, F_GETFL);
    if (keys_saved_flags < 0 || fcntl(STDIN_FILENO, F_SETFL, keys_saved_flags | O_NONBLOCK) != 0) { K.source = 4; return; }
    K.source = 2;
    atexit(keys_restore);
  } else if (S_ISREG(st.st_mode)) {
    K.source = 3;
  } else {
    K.source = 4;
  }
}

static void keys_read(double now) {
  if (!K.source) keys_open();
  if (K.source >= 4 || K.eof) return;
  unsigned char buf[256];
  for (;;) {
    ssize_t n = read(STDIN_FILENO, buf, sizeof buf);
    if (n < 0) break;                                   // EAGAIN: nothing waiting
    if (n == 0) { if (K.source != 1) K.eof = true; break; }
    keys_add(buf, (int)n);
    if (n < (ssize_t)sizeof buf) break;
  }
  keys_decode(now);
}
#endif

bool ax_keys_poll(double now, char out[16]) {
  keys_read(now);
  if (!K.qlen) return false;
  memcpy(out, K.queue[K.qhead], 16);
  K.qhead = (K.qhead + 1) % KEYS_QUEUE;
  K.qlen--;
  return true;
}

int ax_keys_down(double now, char out[][16], int cap) {
  keys_read(now);
  int n = 0, kept = 0;
  for (int i = 0; i < K.nheld; i++) {
    if (K.held[i].until > now) {
      if (n < cap) memcpy(out[n++], K.held[i].name, 16);
      K.held[kept++] = K.held[i];
    }
  }
  K.nheld = kept;
  return n;
}
