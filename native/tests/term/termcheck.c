// termcheck.c — render the buffers gen.js wrote and compare with terminal.js's output.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../term.h"

static char *slurp(const char *p, size_t *n) {
  FILE *f = fopen(p, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
  char *b = malloc((size_t)len + 1);
  *n = fread(b, 1, (size_t)len, f);
  b[*n] = '\0';
  fclose(f);
  return b;
}

int main(int argc, char **argv) {
  if (argc < 2) return 2;
  char path[1024];
  snprintf(path, sizeof path, "%s/cases.txt", argv[1]);
  FILE *f = fopen(path, "r");
  if (!f) return 2;
  int buf, w, h, uni, col, sub, tw, th, tty, k = 0, bad = 0;
  while (fscanf(f, "%d %d %d %d %d %d %d %d %d", &buf, &w, &h, &uni, &col, &sub, &tw, &th, &tty) == 9) {
    size_t n;
    snprintf(path, sizeof path, "%s/px%d.rgba", argv[1], buf);
    unsigned char *px = (unsigned char *)slurp(path, &n);
    snprintf(path, sizeof path, "%s/out%d.txt", argv[1], k);
    char *want = slurp(path, &n);
    AxTermOptions o = { uni, col, sub, tw, th, tty };
    char *got = ax_term_render(px, w, h, &o);
    if (strcmp(got, want) != 0) { bad++; fprintf(stderr, "case %d differs (len %zu vs %zu)\n", k, strlen(got), strlen(want)); }
    free(px); free(want); free(got);
    k++;
  }
  printf("%d cases, %d differ\n", k, bad);
  return bad ? 1 : 0;
}
