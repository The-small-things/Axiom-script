// numcheck.c — ax_fmt_num against JavaScript's String(x) on the values gen.js wrote.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
void ax_fmt_num(double d, char *buf, size_t n);

int main(int argc, char **argv) {
  FILE *f = fopen(argv[1], "r");
  if (!f) return 2;
  char line[256], got[64];
  long n = 0, bad = 0;
  while (fgets(line, sizeof line, f)) {
    char *sp = strchr(line, ' ');
    if (!sp) continue;
    *sp = '\0';
    char *want = sp + 1;
    want[strcspn(want, "\n")] = '\0';
    uint64_t bits = strtoull(line, NULL, 16);
    double d;
    memcpy(&d, &bits, 8);
    ax_fmt_num(d, got, sizeof got);
    n++;
    if (strcmp(got, want)) { if (bad++ < 10) printf("  %s: js %s, native %s\n", line, want, got); }
  }
  printf("%ld values, %ld differ\n", n, bad);
  return bad ? 1 : 0;
}
