/* glass_gpu: the Liquid Glass of a shape drawn with QuartzCore's own shaders (sdf_filter_vert_lpf + glass_background_sdf_lpf of QuartzCore's
 * default.metallib) through pear-metal-helper (Metal client/pmwc), the way the Mac's render server draws a glassBackground filter
 * (CA::OGL::GlassBackgroundFilter::render, decompiled: Solarium docs/CONTROLCENTER-GLASS.md has every key, default and uniform).
 * The backdrop goes as a mipmapped texture (the shader samples it with an explicit level for its blur); the result is drawn over the backdrop
 * with premultiplied blending.  0 when the helper or the library is not there: the compositor's own material is drawn instead. */
#include "ws.h"
#include "pmwc.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int state;		/* 0 not tried, 1 ready, -1 not available */
static uint64_t pso, pso_srgb;

static int glass_init(void)
{
	if (state) return state > 0;
	state = -1;
	if (getenv("WS_GLASS_GPU") && !strcmp(getenv("WS_GLASS_GPU"), "0")) return 0;
	if (pmwc_connect(NULL) != 0) return 0;
	const char *path = getenv("WS_QC_METALLIB") ? getenv("WS_QC_METALLIB") : "/System/Library/Frameworks/QuartzCore.framework/Versions/A/Resources/default.metallib";
	uint64_t lib = pmwc_library_file(path);
	uint64_t vf = lib ? pmwc_function(lib, "sdf_filter_vert_lpf") : 0, ff = lib ? pmwc_function(lib, "glass_background_sdf_lpf") : 0;
	if (!vf || !ff) { fprintf(stderr, "WindowServer: glass shaders not available (%s)\n", path); return 0; }
	uint8_t q[sizeof(pmw_render_desc) + 4 + 3 * 16 + 4 + 16]; memset(q, 0, sizeof q);
	pmw_render_desc *rd = (pmw_render_desc *)q;
	rd->vertex_fn = vf; rd->fragment_fn = ff; rd->samples = 1;
	rd->color[0] = (pmw_blend){ 80 /* BGRA8Unorm */, 1, 1 /* one */, 5 /* one minus source alpha */, 0, 1, 5, 0, 0xf };
	uint32_t *w = (uint32_t *)(q + sizeof *rd);
	uint32_t vd[] = { 3, 0, 31, 0, 0,  1, 29, 16, 0,  2, 29, 24, 0,  1, 0, 32, 1, 1 };	/* position float4 @0, sdf_uv float2 @16, src_uv float2 @24; buffer 0, stride 32 */
	memcpy(w, vd, sizeof vd);
	uint8_t *a = NULL; uint32_t n = 0;
	if (pmwc_call(PMW_RENDER_PSO_NEW, q, sizeof q, &a, &n) == 0 && n >= 8) memcpy(&pso, a, 8);
	else fprintf(stderr, "WindowServer: glass pipeline: %.*s\n", (int)n, a ? (char *)a : "");
	free(a);
	if (!pso) return 0;
	if (getenv("WS_GLASS_BLEND") && !atoi(getenv("WS_GLASS_BLEND"))) {		/* WS_GLASS_BLEND=0: the shader's output replaces the pixels (experiment) */
		rd->color[0].blending = 0; uint64_t p2 = 0; a = NULL; n = 0;
		if (pmwc_call(PMW_RENDER_PSO_NEW, q, sizeof q, &a, &n) == 0 && n >= 8) memcpy(&p2, a, 8);
		free(a); if (p2) pso = p2;
	}
	rd->color[0].format = 81;
	a = NULL; n = 0;
	if (pmwc_call(PMW_RENDER_PSO_NEW, q, sizeof q, &a, &n) == 0 && n >= 8) memcpy(&pso_srgb, a, 8);
	free(a);
	state = 1;
	fprintf(stderr, "WindowServer: Liquid Glass through QuartzCore's shaders\n");
	return 1;
}

static uint16_t half_of(float f)
{
	uint32_t x; memcpy(&x, &f, 4);
	uint32_t s = (x >> 16) & 0x8000; int e = (int)((x >> 23) & 0xff) - 127 + 15; uint32_t m = x & 0x7fffff;
	if (((x >> 23) & 0xff) == 0 || e <= 0) return (uint16_t)s;
	if (e >= 31) return (uint16_t)(s | 0x7c00);
	return (uint16_t)(s | ((uint32_t)e << 10) | (m >> 13));
}

/* the value of a key (0x14d + i), or its default (GlassBackgroundFilter::render) */
static float K(const ws_glass *g, int key, float d) { int i = key - WS_GLASS_KEY0; return g->set[i] ? g->p[i][0] : d; }
static void KC(const ws_glass *g, int key, float c[4]) { int i = key - WS_GLASS_KEY0; for (int k = 0; k < 4; k++) c[k] = g->set[i] ? g->p[i][k] : 0; }
static float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

/* CA::ColorMatrix::set_ycc_composite(white, black, saturation, tint): RGBfromYCC * S * C * YCCfromRGB (BT.709), times (1 - tint.a), tint added to the bias column */
static void ycc_composite(float white, float black, float sat, const float tint[4], float out[4][5])
{
	static const float Y[5][5] = { { 0.2126f, 0.7152f, 0.0722f, 0, 0 }, { -0.11457f, -0.38543f, 0.5f, 0, 0.5f }, { 0.5f, -0.45415f, -0.04585f, 0, 0.5f }, { 0, 0, 0, 1, 0 }, { 0, 0, 0, 0, 1 } };
	static const float R[5][5] = { { 1, 0, 1.5748f, 0, -0.7874f }, { 1, -0.18733f, -0.46812f, 0, 0.32773f }, { 1, 1.8556f, 0, 0, -0.9278f }, { 0, 0, 0, 1, 0 }, { 0, 0, 0, 0, 1 } };
	float C[5][5] = { { white - black, 0, 0, 0, black }, { 0, 1, 0, 0, 0 }, { 0, 0, 1, 0, 0 }, { 0, 0, 0, 1, 0 }, { 0, 0, 0, 0, 1 } };
	float S[5][5] = { { 1, 0, 0, 0, 0 }, { 0, sat, 0, 0, 0.5f - 0.5f * sat }, { 0, 0, sat, 0, 0.5f - 0.5f * sat }, { 0, 0, 0, 1, 0 }, { 0, 0, 0, 0, 1 } };
	float t1[5][5], t2[5][5], t3[5][5];
#define MUL(A, B, O) for (int i = 0; i < 5; i++) for (int j = 0; j < 5; j++) { float v = 0; for (int k = 0; k < 5; k++) v += A[i][k] * B[k][j]; O[i][j] = v; }
	MUL(C, Y, t1); MUL(S, t1, t2); MUL(R, t2, t3);
#undef MUL
	for (int i = 0; i < 4; i++) { for (int j = 0; j < 5; j++) out[i][j] = t3[i][j] * (1 - tint[3]); out[i][4] += tint[i]; }
}
static void put_cm(uint8_t *dst, const float m[4][5]) { for (int k = 0; k < 3; k++) { uint16_t h[4] = { half_of(m[k][0]), half_of(m[k][1]), half_of(m[k][2]), half_of(m[k][4]) }; memcpy(dst + 8 * k, h, 8); } }
static void putf(uint8_t *u, int off, float v) { memcpy(u + off, &v, 4); }

int ws_glass_gpu(ws_surface *img, const ws_glass *g, int ox, int oy)
{
	if (!g->has_params || !glass_init()) return 0;
	int sx = ox + g->rect.x, sy = oy + g->rect.y, sw = g->rect.w, sh = g->rect.h, M = getenv("WS_GLASS_MARGIN") ? atoi(getenv("WS_GLASS_MARGIN")) : 160;	/* the backdrop around the shape: the blurred levels reach far (a narrow margin repeats its edge columns: stripes) */
	int x0 = sx - M < 0 ? 0 : sx - M, y0 = sy - M < 0 ? 0 : sy - M, x1 = sx + sw + M > img->w ? img->w : sx + sw + M, y1 = sy + sh + M > img->h ? img->h : sy + sh + M;
	int W = x1 - x0, H = y1 - y0;
	if (W < 8 || H < 8) { if (getenv("WS_GLASS_DEBUG")) fprintf(stderr, "glass_gpu: region %dx%d too small (origin %d,%d rect %d,%d %dx%d, image %dx%d)\n", W, H, ox, oy, g->rect.x, g->rect.y, g->rect.w, g->rect.h, img->w, img->h); return 0; }
	int levels = 1; while ((W >> levels) > 0 || (H >> levels) > 0) levels++;

	/* the backdrop with its mipmaps (2 x 2 averages) */
	uint32_t fmt = getenv("WS_GLASS_SRGB") && atoi(getenv("WS_GLASS_SRGB")) ? 81 /* BGRA8Unorm_sRGB: the shader works on linear values */ : 80;
	pmw_texture_desc td = { 2, fmt, 1 | 4, 0, (uint64_t)W, (uint64_t)H, 1, (uint64_t)levels, 1, 1, 0 };
	uint64_t src = pmwc_texture(&td);
	pmw_texture_desc tdd = { 2, fmt, 1 | 4, 0, (uint64_t)W, (uint64_t)H, 1, 1, 1, 1, 0 };
	uint64_t dst = pmwc_texture(&tdd);
	if (!src || !dst) { if (getenv("WS_GLASS_DEBUG")) fprintf(stderr, "glass_gpu: textures %dx%d levels %d: %llx %llx\n", W, H, levels, (unsigned long long)src, (unsigned long long)dst); pmwc_release(src); pmwc_release(dst); return 0; }
	uint8_t *lv = malloc((size_t)W * H * 4), *nx = malloc((size_t)W * H * 4);
	for (int y = 0; y < H; y++) memcpy(lv + (size_t)y * W * 4, img->px + (size_t)(y0 + y) * img->stride + (size_t)x0 * 4, (size_t)W * 4);
	if (g->has_under && !(getenv("WS_GLASS_NO_UNDER") && *getenv("WS_GLASS_NO_UNDER"))) {		/* the tile's vibrantColorMatrix fill, drawn before the glass on the Mac: M (4 x 5) on the backdrop inside the shape, the result added (premultiplied, alpha 0 adds) */
		float R2 = (float)(g->radius > 0 ? g->radius : 0);
		for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
			float px_ = x0 + x + 0.5f - sx, py_ = y0 + y + 0.5f - sy;
			if (px_ < 0 || py_ < 0 || px_ > sw || py_ > sh) continue;
			float cx_ = px_ < R2 ? R2 : px_ > sw - R2 ? sw - R2 : px_, cy_ = py_ < R2 ? R2 : py_ > sh - R2 ? sh - R2 : py_, dx_ = px_ - cx_, dy_ = py_ - cy_;
			float d_ = sqrtf(dx_ * dx_ + dy_ * dy_) - R2 + 0.5f, cov = d_ <= 0 ? 1 : d_ >= 1 ? 0 : 1 - d_;
			if (cov <= 0) continue;
			uint8_t *p = lv + ((size_t)y * W + x) * 4; float c[4] = { p[2] / 255.0f, p[1] / 255.0f, p[0] / 255.0f, p[3] / 255.0f }, o[4];
			for (int k = 0; k < 4; k++) { const float *m = g->under_m + 5 * k; o[k] = m[0] * c[0] + m[1] * c[1] + m[2] * c[2] + m[3] * c[3] + m[4]; o[k] = o[k] < 0 ? 0 : o[k] > 1 ? 1 : o[k]; }
			/* the strength: QuartzCore reduces a vibrant fill under glass (vibrancy_strength_reduction, inlined in prepare_layer_image: not decompiled yet); 0.3 is measured
			 * against the Mac over identical backdrops (without it -6, with all of it +14 per channel) */
			static float K = -1; if (K < 0) K = getenv("WS_GLASS_UNDER_K") ? (float)atof(getenv("WS_GLASS_UNDER_K")) : 0.3f;
			for (int k = 0; k < 3; k++) { float v = c[k] + K * (o[k] * cov - c[k] * o[3] * cov); c[k] = v > 1 ? 1 : v < 0 ? 0 : v; }
			p[2] = (uint8_t)(c[0] * 255 + 0.5f); p[1] = (uint8_t)(c[1] * 255 + 0.5f); p[0] = (uint8_t)(c[2] * 255 + 0.5f);
		}
	}
	pmwc_texture_replace(dst, (uint32_t)W, (uint32_t)H, lv, (uint64_t)W * 4);	/* the target: the backdrop as it is */
	if (g->src_blur > 0) {		/* the glass reads its container's gaussianBlur backdrop (inputRadius; sigma = radius / WS_GLASS_BLUR_DIV, 2 by default) */
		float div = getenv("WS_GLASS_BLUR_DIV") ? (float)atof(getenv("WS_GLASS_BLUR_DIV")) : 2.0f;
		ws_surface t = { 0 }; t.w = W; t.h = H; t.stride = (size_t)W * 4; t.px = lv;
		ws_blur_region(&t, (ws_rect){ 0, 0, W, H }, (ws_rect){ 0, 0, W, H }, g->src_blur / div);
	}
	pmwc_texture_replace(src, (uint32_t)W, (uint32_t)H, lv, (uint64_t)W * 4);
	int lw = W, lh = H;
	for (int l = 1; l < levels; l++) {
		int nw = lw > 1 ? lw / 2 : 1, nh = lh > 1 ? lh / 2 : 1;
		/* a 4 x 4 tent (1 3 3 1) around each 2 x 2 block: smoother levels than a plain average (the shader blends between levels for its blur; QuartzCore makes its levels with its own downsample shaders) */
		for (int y = 0; y < nh; y++) for (int x = 0; x < nw; x++) for (int c = 0; c < 4; c++) {
			static const int wt[4] = { 1, 3, 3, 1 }; int acc = 0;
			for (int j = 0; j < 4; j++) { int yy = 2 * y - 1 + j; yy = yy < 0 ? 0 : yy >= lh ? lh - 1 : yy;
				for (int i = 0; i < 4; i++) { int xx = 2 * x - 1 + i; xx = xx < 0 ? 0 : xx >= lw ? lw - 1 : xx; acc += wt[i] * wt[j] * lv[(yy * lw + xx) * 4 + c]; } }
			nx[(y * nw + x) * 4 + c] = (uint8_t)((acc + 32) / 64);
		}
		{ uint8_t *t = lv; lv = nx; nx = t; } lw = nw; lh = nh;
		size_t hd = 8 + sizeof(pmw_region) + 8 + 16; uint8_t *q = malloc(hd + (size_t)lw * lh * 4);
		pmw_region rg = { 0, 0, 0, (uint64_t)lw, (uint64_t)lh, 1 }; uint32_t ls[2] = { (uint32_t)l, 0 }; uint64_t bb[2] = { (uint64_t)lw * 4, 0 };
		memcpy(q, &src, 8); memcpy(q + 8, &rg, sizeof rg); memcpy(q + 8 + sizeof rg, ls, 8); memcpy(q + 16 + sizeof rg, bb, 16); memcpy(q + hd, lv, (size_t)lw * lh * 4);
		pmwc_call(PMW_TEXTURE_REPLACE, q, (uint32_t)(hd + (size_t)lw * lh * 4), NULL, NULL); free(q);
	}

	/* the uniforms (Solarium docs/CONTROLCENTER-GLASS.md): SdfFragmentUniforms (48) + GlassBackgroundUniforms (272); scale 1 (a point is a pixel here) */
	uint8_t u[320]; memset(u, 0, sizeof u);
	float R = (float)(g->radius > 0 ? g->radius : 0.01f), hx = sw / 2.0f, hy = sh / 2.0f;
	float ax = 2.8915570f - 1.4457785f * 2 * hx / (1.528665f * R), ay = 2.8915570f - 1.4457785f * 2 * hy / (1.528665f * R);
	float sdf[12] = { hx, hy, 4, 0,  1, 0, 0, 1,  ax < 0 ? 0 : ax > 1 ? 1 : ax, ay < 0 ? 0 : ay > 1 ? 1 : ay, R, 0 };	/* arg2.z = R: supercircle_sdf scales it by 1.528665 itself (its IR) */
	memcpy(u, sdf, sizeof sdf);
	uint8_t *G = u + 48;
	float dm[4] = { 1.0f / W, 0, 0, 1.0f / H }; memcpy(G, dm, 16);
	float inner = K(g, 0x182, -150), outer = K(g, 0x18b, 100);
	putf(G, 16, inner); putf(G, 20, 1 / K(g, 0x183, 60)); putf(G, 24, outer); putf(G, 28, 1 / K(g, 0x18c, 50));
	putf(G, 32, K(g, 0x195, -11)); putf(G, 36, K(g, 0x196, -3));
	putf(G, 40, 1.6f * K(g, 0x164, 30)); putf(G, 44, 1.6f * (K(g, 0x157, 1) > 0 ? K(g, 0x14e, 100) * 0.5f : 0));
	putf(G, 48, K(g, 0x14d, 400)); putf(G, 52, 1 / K(g, 0x156, 500)); putf(G, 56, K(g, 0x1a1, 200)); putf(G, 60, 1 / K(g, 0x1aa, 250));
	{ int i = 0x1ab - WS_GLASS_KEY0; float so[2] = { (g->set[i] ? g->p[i][0] : 0) / W, -(g->set[i] ? g->p[i][1] : 10) / H }; memcpy(G + 64, so, 8); }
	putf(G, 72, 1.6f * (K(g, 0x1ac, 1) > 0 ? K(g, 0x1a2, 25) * 0.5f : 0)); putf(G, 76, 1 / K(g, 0x1ad, 25));
	float tint[4], cm[4][5];
	KC(g, 0x177, tint); ycc_composite(K(g, 0x179, 1), K(g, 0x176, 0), K(g, 0x178, 1), tint, cm); put_cm(G + 80, cm);
	KC(g, 0x150, tint); ycc_composite(K(g, 0x152, 1), K(g, 0x14f, 0), K(g, 0x151, 1), tint, cm); put_cm(G + 104, cm);
	KC(g, 0x1a6, tint); { float k19f = K(g, 0x19f, 0); if (k19f > 0) tint[3] += k19f; }	/* + key 0x19f * (1 - t), t = 0 for a display without EDR headroom */
	ycc_composite(K(g, 0x1a8, 1), K(g, 0x1a5, 0), K(g, 0x1a7, 1), tint, cm); put_cm(G + 128, cm);
	putf(G, 152, K(g, 0x1ae, 1)); putf(G, 156, tint[3]);
	float a = K(g, 0x15f, 1), b = K(g, 0x160, 0.1f), c = K(g, 0x161, 0.1f), d = K(g, 0x162, 0.4f);
	putf(G, 160, a); putf(G, 164, a - b); putf(G, 168, b - c); putf(G, 172, c - d);
	putf(G, 176, K(g, 0x15a, -450)); putf(G, 180, K(g, 0x15b, -3)); putf(G, 184, K(g, 0x15c, -0.0f)); putf(G, 188, K(g, 0x15d, 0));
	putf(G, 192, K(g, 0x154, -400)); putf(G, 196, K(g, 0x155, -42));
	putf(G, 200, clamp01(K(g, 0x157, 0.2f))); putf(G, 204, clamp01(K(g, 0x17a, 1)));
	{ float bd[2]; if (K(g, 0x153, 0) != 0) { bd[0] = 1; bd[1] = 0; } else { bd[0] = -1; bd[1] = 1; } memcpy(G + 208, bd, 8); }
	putf(G, 216, K(g, 0x1a9, -50)); putf(G, 220, clamp01(K(g, 0x1ac, 1))); putf(G, 224, K(g, 0x199, 0.75f));
	putf(G, 228, K(g, 0x19d, 0) != 0 ? 1.0f : 0.0f); putf(G, 232, K(g, 0x19b, -2.5f)); putf(G, 236, K(g, 0x19c, -1.5f));
	putf(G, 240, 1.0f); putf(G, 244, K(g, 0x167, 0)); putf(G, 248, K(g, 0x19e, 0.97f)); putf(G, 252, 1.0f);
	putf(G, 256, (a != b || b != c || c != d || inner != 0 || outer != 0) ? 1.0f : 0.0f);
	float edr = 1;
	if (getenv("WS_GLASS_DEBUG")) {		/* the keys and what they became, once per shape size */
		static int done; if (done++ < 2) {
			fprintf(stderr, "glass_gpu: shape %dx%d r %d region %dx%d levels %d\n", sw, sh, g->radius, W, H, levels);
			for (int i = 0; i < WS_GLASS_KEYS; i++) if (g->set[i]) fprintf(stderr, "  key %#x = %g %g %g %g\n", WS_GLASS_KEY0 + i, g->p[i][0], g->p[i][1], g->p[i][2], g->p[i][3]);
			for (int o2 = 0; o2 < 272; o2 += 16) { float f[4]; memcpy(f, G + o2, 16); fprintf(stderr, "  u%3d: %g %g %g %g\n", o2, f[0], f[1], f[2], f[3]); }
		}
	}

	/* a quad over the region: clip-space position, sdf_uv = points from the shape's centre, src_uv in the backdrop texture */
	float cx = sx + hx - x0, cy = sy + hy - y0;
	float px[6] = { 0, (float)W, 0, (float)W, (float)W, 0 }, py[6] = { 0, 0, (float)H, 0, (float)H, (float)H }, vx[6][8];
	int flip = getenv("WS_GLASS_FLIP") ? atoi(getenv("WS_GLASS_FLIP")) : 0;
	for (int i = 0; i < 6; i++) { vx[i][0] = px[i] / W * 2 - 1; vx[i][1] = 1 - py[i] / H * 2; vx[i][2] = 0; vx[i][3] = 1;
		vx[i][4] = px[i] - cx; vx[i][5] = py[i] - cy; vx[i][6] = px[i] / W; vx[i][7] = flip ? 1 - py[i] / H : py[i] / H; }
	float mvp[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 }, texmat[4] = { 1, 1, 0, 0 };

	pmwc_list L = { 0 };
	pmw_render_pass rp; memset(&rp, 0, sizeof rp);
	rp.color[0].tex = dst; rp.color[0].load = 1; rp.color[0].store = 1;
	pmwc_cmd(&L, PMWC_RENDER_BEGIN, &rp, sizeof rp);
	pmwc_cmd(&L, PMWC_SET_RENDER_PSO, fmt == 81 ? &pso_srgb : &pso, 8);
#define BYTES(stage, idx, ptr, len) { uint8_t b[8 + (len)]; uint32_t s2[2] = { stage, idx }; memcpy(b, s2, 8); memcpy(b + 8, ptr, len); pmwc_cmd(&L, PMWC_SET_BYTES, b, (uint16_t)sizeof b); }
	BYTES(1, 0, vx, sizeof vx); BYTES(1, 2, mvp, 64); BYTES(1, 3, texmat, 16); BYTES(2, 1, u, 320); BYTES(2, 6, &edr, 4);
#undef BYTES
	{ uint8_t b[16]; uint32_t s2[2] = { 2, 3 }; memcpy(b, s2, 8); memcpy(b + 8, &src, 8); pmwc_cmd(&L, PMWC_SET_TEXTURE, b, sizeof b); }
	{ uint64_t dr[5]; uint32_t pr[2] = { 3, 0 }; memcpy(dr, pr, 8); dr[1] = 0; dr[2] = 6; dr[3] = 1; dr[4] = 0; pmwc_cmd(&L, PMWC_DRAW, dr, sizeof dr); }
	pmwc_cmd(&L, PMWC_END, NULL, 0);
	int ok = pmwc_commit_wait(&L) == 0;
	if (!ok && getenv("WS_GLASS_DEBUG")) fprintf(stderr, "glass_gpu: the command buffer failed\n");
	free(L.b);
	if (ok && pmwc_texture_get(dst, (uint32_t)W, (uint32_t)H, nx, (uint64_t)W * 4) == 0)
		for (int y = 0; y < H; y++) memcpy(img->px + (size_t)(y0 + y) * img->stride + (size_t)x0 * 4, nx + (size_t)y * W * 4, (size_t)W * 4);
	free(lv); free(nx);
	pmwc_release(src); pmwc_release(dst);
	return ok;
}
