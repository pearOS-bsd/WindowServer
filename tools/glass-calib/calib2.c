#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ws.h"
/* ./calib2 in.raw w h out.raw x,y,w,h,r,mode ... : several shapes in one call (a panel and its controls) */
int main(int c, char **v) {
	int w = atoi(v[2]), h = atoi(v[3]); ws_surface *s = ws_surface_new(w, h); FILE *f = fopen(v[1], "rb"); for (int y = 0; y < h; y++) if (fread(s->px + (size_t)y * s->stride, 1, w * 4, f) != (size_t)w * 4) return 1; fclose(f);
	ws_glass g[16]; int n = 0; for (int i = 5; i < c && n < 16; i++) { int a[6]; if (sscanf(v[i], "%d,%d,%d,%d,%d,%d", a, a + 1, a + 2, a + 3, a + 4, a + 5) == 6) g[n++] = (ws_glass){ { a[0], a[1], a[2], a[3] }, a[4], a[5] }; }
	ws_glass_regions(s, g, n, 0, 0);
	f = fopen(v[4], "wb"); for (int y = 0; y < h; y++) fwrite(s->px + (size_t)y * s->stride, 1, w * 4, f); fclose(f); return 0; }
