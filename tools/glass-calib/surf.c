#include <stdlib.h>
#include <sys/mman.h>
#include "ws.h"
ws_surface *ws_surface_new(int w, int h) { ws_surface *s = calloc(1, sizeof *s); if (!s || w <= 0 || h <= 0) { free(s); return NULL; } s->w = w; s->h = h; s->stride = (size_t)w * 4; s->px = calloc((size_t)h, s->stride); return s; }
void ws_surface_free(ws_surface *s) { if (!s) return; free(s->px); free(s); }
