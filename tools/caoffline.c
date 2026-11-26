/* caoffline: paints the CoreAnimation scene of a captured ca.bin (+ ca-ool files, CARENDER_TRACE=1 of carender-stub) into a PPM, without WindowServer: the test bench of src/ca_scene.c.
 *   caoffline CADIR out.ppm [w h]      (CA_PAINT_DEBUG=1 prints the layer tree; CA_SCENE=s paints only scene s, CA_ROOT=s,i only root i of scene s) */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "ca_scene.h"
int main(int argc, char **argv)
{
	if (argc < 3) { fprintf(stderr, "usage: caoffline CADIR out.ppm [w h]\n"); return 1; }
	int w = argc > 4 ? atoi(argv[3]) : 1920, h = argc > 4 ? atoi(argv[4]) : 1080;
	char path[512]; snprintf(path, sizeof path, "%s/ca.bin", argv[1]);
	ca_set_dir(argv[1]);
	if (ca_load_file(path) != 0) { fprintf(stderr, "caoffline: cannot load %s\n", path); return 1; }
	uint8_t *px = calloc((size_t)w * h, 4);
	/* transparent canvas: what the scene leaves empty is written grey */
	const char *rt = getenv("CA_ROOT");
	const char *sc = getenv("CA_SCENE");
	if (sc) ca_paint_scene(atoi(sc), px, w, h, w * 4);
	else if (rt) ca_paint_root(atoi(rt), atoi(strchr(rt, ',') ? strchr(rt, ',') + 1 : "0"), px, w, h, w * 4);
	else ca_paint(px, w, h, w * 4);
	FILE *f = fopen(argv[2], "wb"); if (!f) return 1;
	fprintf(f, "P6\n%d %d\n255\n", w, h);
	for (int i = 0; i < w * h; i++) { const uint8_t *p = px + 4 * i; int a = p[3]; fputc(p[2] + (255 - a) * 128 / 255, f); fputc(p[1] + (255 - a) * 128 / 255, f); fputc(p[0] + (255 - a) * 128 / 255, f); }
	fclose(f);
	fprintf(stderr, "caoffline: %d layers, %d images, %d records resynchronised\n", ca_nlayers(), ca_nimages(), ca_resynced());
	return 0;
}
