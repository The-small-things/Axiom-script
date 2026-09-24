// glbcheck.c — dump what ax_glb_parse reads from each .glb given, in make.js's format.
#include "../../axiom.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void ax_fmt_num(double d, char *buf, size_t n);
void ax_json_write(AxValue v, int indent, char **out);

int main(int argc, char **argv) {
  for (int a = 1; a < argc; a++) {
    FILE *f = fopen(argv[a], "rb");
    if (!f) return 2;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)n);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) return 2;
    fclose(f);
    const char *base = strrchr(argv[a], '/');
    printf("== %s\n", base ? base + 1 : argv[a]);
    int np = 0;
    AxGlbPrim *prims = ax_glb_parse(buf, (size_t)n, &np);
    if (!prims) { printf("FAIL\n"); free(buf); continue; }
    for (int k = 0; k < np; k++) {
      char *mat = NULL;
      ax_json_write(prims[k].material, 0, &mat);
      printf("prim %d verts %d idx %d material %s\n", k, prims[k].nverts, prims[k].nidx, mat);
      free(mat);
      char num[64];
      for (int i = 0; i < prims[k].nverts * 8; i++) { ax_fmt_num(prims[k].verts[i], num, sizeof num); printf(i ? " %s" : "%s", num); }
      printf("\n");
      for (int i = 0; i < prims[k].nidx; i++) printf(i ? " %d" : "%d", prims[k].idx[i]);
      printf("\n");
    }
    ax_glb_free(prims, np);
    free(buf);
  }
  return 0;
}
