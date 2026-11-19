#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ws.h"
/* ./calib in.raw w h x y rw rh radius mode out.raw : BGRA premultiplied raw in, the glass applied, raw out */
int main(int c, char **v) {
	int w = atoi(v[2]), h = atoi(v[3]); ws_surface *s = ws_surface_new(w, h); FILE *f = fopen(v[1], "rb"); for (int y = 0; y < h; y++) if (fread(s->px + (size_t)y * s->stride, 1, w * 4, f) != (size_t)w * 4) return 1; fclose(f);
	ws_glass g = { { atoi(v[4]), atoi(v[5]), atoi(v[6]), atoi(v[7]) }, atoi(v[8]), atoi(v[9]) }; ws_glass_regions(s, &g, 1, 0, 0);
	f = fopen(v[10], "wb"); for (int y = 0; y < h; y++) fwrite(s->px + (size_t)y * s->stride, 1, w * 4, f); fclose(f); return 0; }
