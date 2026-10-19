// render.h — software rasterizer and PNG writer (see render.c).
#ifndef AXIOM_RENDER_H
#define AXIOM_RENDER_H
#include <stdint.h>
#include <stdbool.h>
struct AxVM;
uint8_t *ax_render_frame(struct AxVM *vm, int width, int height);   // RGBA, malloc'd
bool ax_write_png(const char *path, const uint8_t *rgba, int width, int height);
#endif
