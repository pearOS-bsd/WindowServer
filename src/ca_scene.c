/* wscarender: the CoreAnimation scene of the commits that carender-stub keeps (ca.bin + ca-ool-<n>-<k>.bin), composed and shown in a window of WindowServer.
 * It is the C port of darwinator/tools/ca/{caparse,ca_scene}.py: decode the command stream of the render server protocol (messages 0x9c42..0x9c45), follow the layers,
 * give every Image its pixels from the out-of-line data, paint the tree.  A client of WindowServer for now; the decoder and the painter (everything above main) are meant to move into the
 * render server of WindowServer (CARenderServer) when it takes the commits itself.
 *
 *   usage: wscarender CADIR [x y [level [w h]]]        (CADIR holds ca.bin and the ca-ool files; reloads when ca.bin grows)
 *
 * Painted: background colour and opacity, Image contents (BGRA8 or RGBA half float, first plane), children on top.  Not yet: transforms, masks, filters, object types 2/3/25/160. */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/stat.h>
#include "ca_scene.h"

/* ---- reader ---- */
typedef struct { const uint8_t *b; size_t n, p; int err; } rd;
static uint8_t u8(rd *r) { if (r->p + 1 > r->n) { r->err = 1; return 0; } return r->b[r->p++]; }
static uint16_t u16(rd *r) { uint16_t v = 0; if (r->p + 2 > r->n) { r->err = 1; return 0; } memcpy(&v, r->b + r->p, 2); r->p += 2; return v; }
static uint32_t u32(rd *r) { uint32_t v = 0; if (r->p + 4 > r->n) { r->err = 1; return 0; } memcpy(&v, r->b + r->p, 4); r->p += 4; return v; }
static uint64_t u64(rd *r) { uint64_t v = 0; if (r->p + 8 > r->n) { r->err = 1; return 0; } memcpy(&v, r->b + r->p, 8); r->p += 8; return v; }
static const uint8_t *raw(rd *r, size_t k) { const uint8_t *p = r->b + r->p; if (k > r->n || r->p + k > r->n) { r->err = 1; return NULL; } r->p += k; return p; }

/* ---- the scene ---- */
#define MAXL 4096
#define MAXS 64
typedef struct shape shape_t;
typedef struct { int n; float *v, *loc; double s[2], e[2]; int atom; } grad_t;	/* a CAGradientLayer: n stops (r g b a + 1 float each, premultiplied), locations, start and end points in unit coordinates, the atom of the type */
typedef struct { int w, h, bpp, stride; uint8_t *px; } img_t;       /* stride: bytes of a row (the Image's row bytes: padded, 192 for 42 pixels) */
typedef struct {
	uint64_t id;
	int used;
	double px_, py_, w, h;
	float col[5];
	int any_col;
	uint64_t subs[MAXS]; int nsubs;
	uint64_t img;		/* id of the Image object, 0: none */
	char cls;			/* the class of the layer by its class object: 0 plain, 'E' CASDFElementLayer (0x2f), 'S' CASDFLayer (0x30), 'B' CABackdropLayer (3) */
	double dd[6]; uint32_t unk[5];	/* the doubles of the record (position, ?, ?, size) and the words between the colour and them */
	double ccenter[4]; int has_ccenter;	/* contentsCenter: the Vector at index 6 of the extension block's objects (Layer::set_contents_center writes ext + 0x30) */
	int32_t shape[4]; int has_shape;	/* the valid part of the Image: (x0, y0, ...) of the shape record */
	float shoff[2], shrad; int has_sh;	/* shadowOffset, shadowRadius */
	float bw, bcol[5];	/* borderWidth and borderColor of the layer (premultiplied, 5 floats like colour) */
	shape_t *pshape;	/* the shape of a CAShapeLayer */
	grad_t *grad;	/* the gradient of a CAGradientLayer */
	float tint[4]; int has_tint;	/* the colour of an alpha-only Image (see texture_tail) */
	float rectA[4], rectB[4]; int has_rects;	/* the two 16 byte records of the extension block (contentsRect / contentsCenter?) */
	float cr;			/* cornerRadius (the record of a layer with an extension block) */
} layer_t;
typedef struct { uint64_t id; img_t im; } imgent_t;
#define MAXSCENES 16
#define MAXROOTS 16
struct scene { layer_t layers[MAXL]; int nlayers; imgent_t images[MAXL]; int nimages; int nskipped; uint32_t ctx; int nroots; uint32_t rootctx[MAXROOTS]; uint64_t rootid[MAXROOTS]; };		/* rootctx/rootid: the root layer of each render context (op 7 of its first commit) */
static struct scene *S, *scenes[MAXSCENES]; static int nscenes;		/* S: the scene that is being fed or painted */
#define L (S->layers)
#define nL (S->nlayers)
#define IM (S->images)
#define nIM (S->nimages)
#define skipped (S->nskipped)
static const char *cadir = ".";
static uint8_t *file_ool(void *ctx, int msg, int desc, size_t *sz)
{
	char nm[512]; snprintf(nm, sizeof nm, "%s/ca-ool-%02d-%d.bin", cadir, msg, desc);
	FILE *f = fopen(nm, "rb"); long n; uint8_t *p;
	(void)ctx;
	if (!f) return NULL;
	fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
	p = malloc(n > 0 ? n : 1);
	if (!p || fread(p, 1, n, f) != (size_t)n) { free(p); fclose(f); return NULL; }
	fclose(f); *sz = (size_t)n; return p;
}
static ca_ool_fn ool_loader = file_ool;
static void *ool_ctx;
void ca_set_dir(const char *d) { cadir = d; }
void ca_set_ool_loader(ca_ool_fn fn, void *ctx) { ool_loader = fn ? fn : file_ool; ool_ctx = ctx; }
int ca_nlayers(void) { int n = 0; for (int i = 0; i < nscenes; i++) n += scenes[i]->nlayers; return n; }
int ca_nimages(void) { int n = 0; for (int i = 0; i < nscenes; i++) n += scenes[i]->nimages; return n; }
int ca_nscenes(void) { return nscenes; }
void ca_scene_info(int i, int *nlayers, int *nimages) { *nlayers = i >= 0 && i < nscenes ? scenes[i]->nlayers : 0; *nimages = i >= 0 && i < nscenes ? scenes[i]->nimages : 0; }
uint32_t ca_scene_ctx(int i) { return i >= 0 && i < nscenes ? scenes[i]->ctx : 0; }

static layer_t *find(uint64_t id) { for (int i = 0; i < nL; i++) if (L[i].used && L[i].id == id) return &L[i]; return NULL; }
static layer_t *get(uint64_t id) { layer_t *l = find(id); if (l) return l; for (int i = 0; i < nL; i++) if (!L[i].used) { l = &L[i]; goto init; } if (nL >= MAXL) return NULL; l = &L[nL++]; init: memset(l, 0, sizeof *l); l->used = 1; l->id = id; return l; }

/* CA_DUMP_IMG=dir: every image that arrives (A8 or BGRA) is written as <dir>/img-<id>-<n>.pgm (A8) or .ppm (BGRA): to see what the render server really got */
static void dump_img(uint64_t id, const img_t *im)
{
	const char *dir = getenv("CA_DUMP_IMG"); static int n; char p[200];
	if (!dir || !im->px || im->w <= 0 || im->h <= 0) return;
	snprintf(p, sizeof p, "%s/img-%llx-%d.%s", dir, (unsigned long long)id, n++, im->bpp == 1 ? "pgm" : "ppm");
	FILE *f = fopen(p, "wb"); if (!f) return;
	fprintf(f, "%s\n%d %d\n255\n", im->bpp == 1 ? "P5" : "P6", im->w, im->h);
	for (int y = 0; y < im->h; y++) for (int x = 0; x < im->w; x++) { const uint8_t *q = im->px + (size_t)y * im->stride + (size_t)x * im->bpp; if (im->bpp == 1) fputc(q[0], f); else { fputc(q[2], f); fputc(q[1], f); fputc(q[0], f); } }
	fclose(f);
	if (im->bpp == 4) {	/* and the alpha channel of a BGRA image as a pgm */
		snprintf(p, sizeof p, "%s/img-%llx-%d.alpha.pgm", dir, (unsigned long long)id, n - 1);
		if ((f = fopen(p, "wb")) != NULL) { fprintf(f, "P5\n%d %d\n255\n", im->w, im->h); for (int y = 0; y < im->h; y++) for (int x = 0; x < im->w; x++) fputc(im->px[(size_t)y * im->stride + (size_t)x * 4 + 3], f); fclose(f); }
	}
}
static void setimg(uint64_t id, img_t *im) { dump_img(id, im); for (int i = 0; i < nIM; i++) if (IM[i].id == id) { free(IM[i].im.px); IM[i].im = *im; return; } if (nIM < MAXL) { IM[nIM].id = id; IM[nIM].im = *im; nIM++; } }
static img_t *getimg(uint64_t id) { for (int i = 0; i < nIM; i++) if (IM[i].id == id) return &IM[i].im; return NULL; }

/* ---- decoder ---- */
struct shape { int nt, np; uint8_t *types; double *pts; float fill[5]; float stroke[5]; double lw; int rule, cap, join; int has_path; };	/* a CAShapeLayer (CA::Render::ShapeLayer 0x33 with a CA::Render::Path 0x26): the path elements (0 move 1 line 2 quad 3 cubic 4 close) and their points, the fill and stroke colours (5 floats like a layer's), the fill rule */
typedef struct { int kind; uint64_t id; int attached; const uint8_t *inl; size_t inl_n; shape_t *pshape; grad_t *grad; int w, h, bpp, stride; uint64_t imgid; char cls; double vec[4]; int nvec; float tint[4]; int has_tint; int32_t shape[4]; int has_shape; } contents_t;	/* kind: 0 none, 1 image, 2 reference */
static int cur_ool_msg;			/* the file number of the message being read, or -1 */
static int ool_desc[256], n_ool;		/* descriptor indexes of its out-of-line data */
static int ool_taken;
static int port_desc[256], n_port, port_taken;	/* the descriptors that are ports (the memory entries of shared pixels), in order */

static void skip_obj(rd *r, int depth, contents_t *ct);
static void skip_arr(rd *r, int depth, uint64_t *ids, int *nids)
{
	if (!u8(r)) return;
	uint32_t fl = u32(r), n = u32(r);
	if (fl & 1) { for (uint32_t i = 0; i < n && !r->err; i++) { uint64_t v = u64(r); if (ids && *nids < MAXS) ids[(*nids)++] = v; } }
	else for (uint32_t i = 0; i < n && !r->err; i++) skip_obj(r, depth + 1, NULL);
}
static void skip_atom(rd *r) { uint8_t b = u8(r); if (b < 0xfe) return; if (b == 0xfe) { u16(r); return; } uint64_t n = u64(r); raw(r, n); }
static void skip_cf(rd *r, int depth)
{
	uint8_t t = u8(r);
	if (depth > 20) { r->err = 1; return; }
	switch (t) {
	case 0: break;		/* a null CF object */
	case 1: break;
	case 2: if (u8(r)) { uint64_t n = u64(r); raw(r, n); } break;
	case 3: u8(r); u64(r); break;
	case 4: u8(r); break;
	case 5: { uint64_t n = u64(r); if (n != 0xffffffffULL && n != ~0ULL) raw(r, n); break; }
	case 6: { uint64_t n = u64(r); for (uint64_t i = 0; i < n && !r->err; i++) skip_cf(r, depth + 1); break; }
	case 7: { uint64_t n = u64(r); for (uint64_t i = 0; i < n && !r->err; i++) { skip_cf(r, depth + 1); skip_cf(r, depth + 1); } break; }
	default: if (getenv("CA_DEBUG")) fprintf(stderr, "ca_scene: unknown object type %#x at %zu\n", t, r->p - 1); r->err = 1;
	}
}
static void do_layer(rd *r, int depth, uint64_t id);
/* CA::Render::Texture::decode (the end of an Image): colorspace, i32, 4 x i16, shape, i32, i32, and 0x14 bytes when the Image flags say so (decompilation of QuartzCore) */
static int32_t tt_shape[4]; static int tt_has_shape;	/* the bounds of the valid part of an Image (the shape of texture_tail, n == -2) */
static float tt_tint[4]; static int tt_has_tint;	/* the 5 floats at the end of an Image with flag 0x1000: (r, g, b, a, 0), the colour of an alpha-only image (white in the dark appearance, black in the light one) */
static uint32_t tt_a, tt_b; static uint16_t tt_h[4];	/* what texture_tail read: the four 16-bit words and the last two 32-bit words (the size of a Surface) */
static void texture_tail(rd *r, uint32_t image_flags)
{
	uint8_t cs = u8(r);
	switch (cs) {
	case 0: case 3: break;
	case 1: u8(r); break;
	case 2: case 4: skip_cf(r, 0); break;
	default: r->err = 1; return;
	}
	u32(r); for (int i = 0; i < 4; i++) tt_h[i] = u16(r);
	{	/* the shape: -2 bounds (4 x i32), 0 none, -1 all, else a list of n words (3 are the header) */
		int32_t n = (int32_t)u32(r);
		tt_has_shape = 0;
		if (n == -2) { for (int i = 0; i < 4; i++) tt_shape[i] = (int32_t)u32(r); tt_has_shape = 1; }
		else if (n != 0 && n != -1) { if (n < 3 || n > 0x20000) { r->err = 1; return; } raw(r, (size_t)(n - 3) * 4); }
	}
	tt_a = u32(r); tt_b = u32(r);
	tt_has_tint = 0;
	if (image_flags & 0x1000) { const uint8_t *tp = raw(r, 0x14); if (tp) { memcpy(tt_tint, tp, 16); tt_has_tint = 1; } }
}

/* ICC matrix/TRC profile (the colourspace of a layer's stream, e.g. the 496-byte "CG Cal RGB" of a commit) to sRGB: the stream's colours are in that space, the display here is sRGB.  Only RGB matrix profiles with 'para' or 'curv' gamma curves (what CoreAnimation writes, measured). */
typedef struct { double m[9]; int ptype; double pp[7]; int ok; } icc_t;
static double be32(const uint8_t *p) { return (double)(uint32_t)(p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]); }
static double s15(const uint8_t *p) { return (double)(int32_t)(p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]) / 65536.0; }
static void icc_parse(const uint8_t *p, size_t n, icc_t *o)
{
	memset(o, 0, sizeof *o);
	if (n < 132 || memcmp(p + 16, "RGB ", 4) || memcmp(p + 20, "XYZ ", 4)) return;
	uint32_t nt = (uint32_t)be32(p + 128); int have = 0; const uint8_t *trc = NULL;
	for (uint32_t k = 0; k < nt && 132 + 12 * (size_t)k + 12 <= n; k++) {
		const uint8_t *t = p + 132 + 12 * k; size_t off = (size_t)be32(t + 4), ln = (size_t)be32(t + 8);
		if (off + ln > n) continue;
		int c = !memcmp(t, "rXYZ", 4) ? 0 : !memcmp(t, "gXYZ", 4) ? 1 : !memcmp(t, "bXYZ", 4) ? 2 : -1;
		if (c >= 0 && ln >= 20) { for (int q = 0; q < 3; q++) o->m[q * 3 + c] = s15(p + off + 8 + 4 * q); have |= 1 << c; }
		if (!memcmp(t, "rTRC", 4) && ln >= 12) trc = p + off;
	}
	if (have != 7 || !trc) return;
	if (!memcmp(trc, "para", 4)) { static const int np_[5] = { 1, 3, 4, 5, 7 }; int ft = trc[8] << 8 | trc[9]; if (ft > 4) return; o->ptype = ft; for (int q = 0; q < np_[ft]; q++) o->pp[q] = s15(trc + 12 + 4 * q); o->ok = 1; }
	else if (!memcmp(trc, "curv", 4) && be32(trc + 8) == 1) { o->ptype = 0; o->pp[0] = (trc[12] << 8 | trc[13]) / 256.0; o->ok = 1; }
}
static double icc_decode(const icc_t *o, double x)	/* encoded -> linear, the parametric curve types 0..4 of the ICC specification */
{
	double g = o->pp[0], a = o->pp[1], b = o->pp[2], c = o->pp[3], d = o->pp[4], e = o->pp[5], f = o->pp[6], ax = x < 0 ? -x : x, y;
	switch (o->ptype) {
	case 0: y = pow(ax, g); break;
	case 1: y = ax >= -b / a ? pow(a * ax + b, g) : 0; break;
	case 2: y = ax >= -b / a ? pow(a * ax + b, g) + c : c; break;
	case 3: y = ax >= d ? pow(a * ax + b, g) : c * ax; break;
	default: y = ax >= d ? pow(a * ax + b, g) + e : c * ax + f; break;
	}
	return x < 0 ? -y : y;
}
static void icc_to_srgb(const icc_t *o, double *rgb)
{
	static const double x2s[9] = { 3.1338561, -1.6168667, -0.4906146, -0.9787684, 1.9161415, 0.0334540, 0.0719453, -0.2289914, 1.4052427 };
	double l[3], xyz[3], out[3];
	for (int i = 0; i < 3; i++) l[i] = icc_decode(o, rgb[i]);
	for (int i = 0; i < 3; i++) xyz[i] = o->m[i * 3] * l[0] + o->m[i * 3 + 1] * l[1] + o->m[i * 3 + 2] * l[2];
	{ static const double d50[3] = { 0.9642, 1.0, 0.8249 }; for (int i = 0; i < 3; i++) { double w = o->m[i * 3] + o->m[i * 3 + 1] + o->m[i * 3 + 2]; if (w > 0.01) xyz[i] *= d50[i] / w; } }	/* the profile's white (R = G = B = 1) maps to the white of the destination (white point compensation) */
	for (int i = 0; i < 3; i++) { double v = x2s[i * 3] * xyz[0] + x2s[i * 3 + 1] * xyz[1] + x2s[i * 3 + 2] * xyz[2]; v = v < 0 ? 0 : v > 1 ? 1 : v; out[i] = v <= 0.0031308 ? 12.92 * v : 1.055 * pow(v, 1 / 2.4) - 0.055; }
	memcpy(rgb, out, sizeof out);
}
static void skip_obj_(rd *r, int depth, contents_t *ct);
static void skip_obj(rd *r, int depth, contents_t *ct)
{
	size_t p0 = r->p; int e0 = r->err;
	skip_obj_(r, depth, ct);
	if (!e0 && r->err && getenv("CA_DEBUG")) fprintf(stderr, "ca_scene: object at %zu (type %#x) failed at %zu (depth %d)\n", p0, p0 < r->n ? r->b[p0] : 0, r->p, depth);
}
static void skip_obj_(rd *r, int depth, contents_t *ct)
{
	if (depth > 40) { r->err = 1; return; }
	uint8_t t = u8(r);
	if (r->err) return;
	switch (t) {
	case 0: break;
	/* an Image is identified by its id AND its generation (the u32 after the id): the render server's ids are recycled (the address of the object), the generation tells a new buffer from the old one that other layers still show: 0x9e37... mixes both into one key */
	case 0xff: { uint64_t i = u64(r); i ^= (uint64_t)u32(r) * 0x9e3779b97f4a7c15ULL; if (ct) { ct->kind = 2; ct->imgid = i; } break; }
	case 0xfe: { uint64_t i = u64(r); i ^= (uint64_t)u32(r) * 0x9e3779b97f4a7c15ULL; contents_t in; memset(&in, 0, sizeof in); skip_obj(r, depth + 1, &in); if (ct) { *ct = in; ct->imgid = i; } break; }
	case 0x1e: do_layer(r, depth, 0); break;		/* a layer inside an object: decoded, not kept */
	case 0x26: {		/* CA::Render::Path::decode (decompiled): i32 flags, 0x20 bytes (bounds), 0x10 bytes, vm_size_t, i32 element count, i32 point count, i32 (curves), the element bytes, point count x 2 doubles */
		uint32_t flags_ = u32(r); (void)flags_; raw(r, 0x20); raw(r, 0x10); u64(r);
		uint32_t nt = u32(r), np = u32(r); u32(r);
		if (r->err || nt >= 0x1000000 || np >= 0x1000000) { r->err = 1; break; }
		const uint8_t *tp = raw(r, nt); const uint8_t *pp = raw(r, (size_t)np * 16);
		if (!r->err && ct && tp && pp) {
			shape_t *sh = calloc(1, sizeof *sh);
			if (sh) { sh->types = malloc(nt ? nt : 1); sh->pts = malloc(np ? (size_t)np * 16 : 1); if (getenv("CA_DEBUG_EXT")) { fprintf(stderr, "ca_scene: path types:"); for (uint32_t q = 0; q < nt; q++) fprintf(stderr, " %d", tp[q]); fprintf(stderr, " | %u points:", np); for (uint32_t q = 0; q < np && q < 24; q++) { double d2[2]; memcpy(d2, pp + (size_t)q * 16, 16); fprintf(stderr, " (%.3f,%.3f)", d2[0], d2[1]); } fprintf(stderr, "\n"); }
			if (sh->types && sh->pts) { memcpy(sh->types, tp, nt); memcpy(sh->pts, pp, (size_t)np * 16); sh->nt = (int)nt; sh->np = (int)np; sh->has_path = 1; ct->pshape = sh; } else { free(sh->types); free(sh->pts); free(sh); } }
		}
		break; }
	case 0x33: {		/* CA::Render::ShapeLayer (decompiled): Object flags, the Path object (0x26), 0x14 bytes (fill colour), an object (0x27), 0x14 bytes (stroke colour), an object (0x27), 5 doubles (line width, miter limit, dash phase, stroke start, stroke end), the dash Vector (0x3e), i8 fill rule (0..1), i8 line cap (0..2), i8 line join (0..2) */
		contents_t pc; memset(&pc, 0, sizeof pc);
		u32(r); skip_obj(r, depth + 1, &pc);
		const uint8_t *fc = raw(r, 0x14); skip_obj(r, depth + 1, NULL);
		const uint8_t *sc = raw(r, 0x14); skip_obj(r, depth + 1, NULL);
		double lw = 0; { const uint8_t *dp = raw(r, 5 * 8); if (dp) memcpy(&lw, dp, 8); }
		skip_obj(r, depth + 1, NULL);
		uint8_t rule = u8(r), cap = u8(r), join = u8(r);
		if (!r->err && pc.pshape) { if (fc) memcpy(pc.pshape->fill, fc, 20); if (sc) memcpy(pc.pshape->stroke, sc, 20); pc.pshape->lw = lw; pc.pshape->rule = rule; pc.pshape->cap = cap; pc.pshape->join = join; if (ct) ct->pshape = pc.pshape; else { free(pc.pshape->types); free(pc.pshape->pts); free(pc.pshape); } }
		else if (pc.pshape) { free(pc.pshape->types); free(pc.pshape->pts); free(pc.pshape); }
		break; }
	case 0x36: { uint32_t n = u32(r); raw(r, n); break; }
	case 0x3e: { uint32_t n = u32(r); const uint8_t *vp = raw(r, 8ULL * n); if (ct && vp && n >= 4) { memcpy(ct->vec, vp, 32); ct->nvec = 4; } break; }		/* CA::Render::Vector: u32 count, then count doubles */
	case 0x34: { uint32_t fl = u32(r); if ((fl & 3) == 0) { uint64_t n = u64(r); if (n != 0xffffffffULL && n != ~0ULL) { const uint8_t *ip = raw(r, n); if (ct && ip) { ct->inl = ip; ct->inl_n = (size_t)n; } } else if (ct) ct->attached = 1; } else { uint64_t a0 = u64(r), a1 = u64(r); if (getenv("CA_DEBUG")) fprintf(stderr, "ca_scene: object 0x34 flags %#x: %llx %llx\n", fl, (unsigned long long)a0, (unsigned long long)a1); if (ct) ct->attached = 2; } break; }
	case 0x12: { uint32_t fl = u32(r); if ((fl >> 8) & 1) skip_atom(r); else { { uint8_t kind = u8(r); if (kind == 0) { skip_cf(r, 0); break; } if (kind != 1) { r->err = 1; break; } } skip_atom(r); skip_obj(r, depth + 1, NULL); skip_arr(r, depth, NULL, NULL); } break; }
	case 0x53: {		/* CA::Render::Surface::encode (decompiled): the pixels of a CABackingStore, an IOSurface or a shared memory; see docs: SwiftUI/docs/EVIDENCE.md, 54th pass */
		uint8_t mode = u8(r); uint32_t w0 = u32(r); int shm = 0;
		if (mode == 1) { skip_obj(r, depth + 1, NULL); if ((w0 >> 21) & 1) u8(r); }
		else {
			uint8_t src; u32(r); src = u8(r);
			if (src == 2) { u64(r); for (int i = 0; i < 4; i++) u32(r); }
			else if (src == 1 || src == 3) shm = 1;		/* the pixels are in the memory the next port descriptor names */
		}
		for (int i = 0; i < 7; i++) u8(r);
		texture_tail(r, 0);
		if (ct) { ct->kind = 1; ct->w = (int)tt_a; ct->h = (int)tt_b; ct->bpp = 4; ct->stride = ((int)tt_a * 4 + 15) & ~15; ct->attached = shm ? 2 : 0; }
		break; }
	case 0x17: {
		uint32_t flags_i = u32(r); uint8_t fmt = u8(r); uint8_t np = u8(r); uint32_t w = u32(r), h = u32(r); uint64_t row0 = 0;
		(void)fmt;
		for (int i = 0; i < np && !r->err; i++) { uint64_t v = u64(r); if (i == 0) row0 = v; }
		if (u8(r)) { contents_t in; memset(&in, 0, sizeof in); skip_obj(r, depth + 1, &in); if (ct) { ct->kind = 1; ct->attached = in.attached; ct->inl = in.inl; ct->inl_n = in.inl_n; } }
		else { uint64_t n = u64(r); if (n != 0xffffffffULL && n != ~0ULL) { const uint8_t *ip = raw(r, n); if (ct && ip) { ct->inl = ip; ct->inl_n = (size_t)n; } } else if (ct) { ct->kind = 1; ct->attached = 1; } }
		texture_tail(r, flags_i);
		if (ct) { ct->kind = 1; ct->w = (int)w; ct->h = (int)h; ct->bpp = !w ? 4 : (row0 < 2ull * w ? 1 : (row0 < 6ull * w ? 4 : 8)); ct->has_tint = tt_has_tint; memcpy(ct->tint, tt_tint, 16); ct->has_shape = tt_has_shape; memcpy(ct->shape, tt_shape, 16); ct->stride = (int)row0;	/* the rows are padded (128 bytes for 33 BGRA pixels, 64 for 34 A8): the pixel size is told by how many row bytes a pixel has at least */ }
		break; }
	case 0x1f: u32(r); u32(r); u8(r); break;
	/* the formats below come from the decompilation of QuartzCore (CA::Render::<Class>::<Class>(Decoder*), REA/Ghidra) */
	case 3: {		/* BackdropLayer: Object flags, vm_size_t, String, 4 x i32, 0x20 bytes, i32, 0x20 bytes, i8 enum */
		if (ct) ct->cls = 'B';
		u32(r); u64(r); skip_obj(r, depth + 1, NULL);
		for (int i = 0; i < 4; i++) u32(r);
		raw(r, 0x20); u32(r); raw(r, 0x20); u8(r); break; }
	case 0x2f: u32(r); u8(r); u8(r); u32(r); u32(r); u32(r); u8(r); if (ct) ct->cls = 'E'; break;		/* SDFElementLayer: Object flags, i8 mode, i8 operation, i32 contentsZeroValueDistance, i32 contentsOneValueDistance, i32 gradientOvalization, i8 hitTestsAsFill (Solarium docs/RENDER-STREAM.md) */
	case 0x30: u32(r); raw(r, 0x70); if (ct) ct->cls = 'S'; break;		/* SDFLayer: Object flags, 0x70 bytes of effect parameters */
	case 0x40: u32(r); u64(r); skip_cf(r, 0); u8(r); break;		/* SeparatedOptions: i32, i64, a CF dictionary, i8 */
	case 7: u32(r); break;		/* Object (QuartzCore decode_object: a bare Object: its flags) */
	case 0x14: {		/* GradientLayer: Object flags, Gradient (vm_size_t n, i32 width = 5, i32 flags, n*5*4 bytes, locations n*4 if flags & 2, interpolations n*0x20-0x20 if flags & 4), 2 x 2 cgfloats, 2 x 2 cgfloats, atom, i32, a texture object if Object flags bit 11, colorspace */
		uint32_t of = u32(r); uint64_t n = u64(r); u32(r); uint32_t fl = u32(r) & 0xff;
		size_t gv = r->p; if (n) { raw(r, 20 * n); if (fl & 2) raw(r, 4 * n); if ((fl & 4) && n > 0) raw(r, 0x20 * n - 0x20); }
		size_t gp = r->p; raw(r, 16); raw(r, 16); skip_atom(r); u32(r);
		if (ct && !r->err && n && n < 4096) {
			grad_t *g = calloc(1, sizeof *g);
			if (g) { g->n = (int)n; g->v = malloc(20 * n); g->loc = fl & 2 ? malloc(4 * n) : NULL; if (g->v) { memcpy(g->v, r->b + gv, 20 * n); if (g->loc) memcpy(g->loc, r->b + gv + 20 * n, 4 * n); memcpy(g->s, r->b + gp, 16); memcpy(g->e, r->b + gp + 16, 16); g->atom = r->b[gp + 32]; ct->grad = g; } else free(g); }
		}
		if ((of >> 8) & 8) skip_obj(r, depth + 1, NULL);
		size_t csp = r->p;
		if (r->p < r->n && r->b[r->p] <= 4) { uint8_t cs = u8(r); switch (cs) { case 0: case 3: break; case 1: u8(r); break; case 2: case 4: skip_cf(r, 0); break; } }		/* the colorspace: absent in the streams seen (the next byte is the 0xfe of a following object) */
		if (ct && ct->grad && !r->err) {		/* the stops are in the colourspace of the stream (an ICC matrix profile): convert them to sRGB */
			for (size_t q = csp; q + 24 < r->p && q + 24 < r->n; q++) if (!memcmp(r->b + q + 4, "appl", 4) && !memcmp(r->b + q + 12, "mntrRGB ", 8)) {
				icc_t ic; size_t isz = (size_t)be32(r->b + q); if (q + isz > r->n) break;
				icc_parse(r->b + q, isz, &ic);
				if (ic.ok) for (int k2 = 0; k2 < ct->grad->n; k2++) { float *c = ct->grad->v + 5 * k2; double a = c[3] > 0 ? c[3] : 1, rgb[3] = { c[0] / a, c[1] / a, c[2] / a }; icc_to_srgb(&ic, rgb); c[0] = (float)(rgb[0] * a); c[1] = (float)(rgb[1] * a); c[2] = (float)(rgb[2] * a); }
				break;
			}
		}
		break; }
	case 0x1d: skip_atom(r); skip_obj(r, depth + 1, NULL); break;		/* KeyValue: atom, any object (no Object flags) */
	case 0x3a: u32(r); u32(r); for (int i = 0; i < 5; i++) u64(r); break;	/* Timing: Object flags, i32, 5 x i64 */
	case 0x3f:		/* WindowLayer: Object flags, vm_size_t, i64, i8 enum */
		u32(r); u64(r); u64(r); u8(r); break;
	case 0x19:		/* ImageQueue::decode: Shmem, i32, (a port: no bytes), i32, i32 */
		skip_obj(r, depth + 1, NULL); u32(r); u32(r); u32(r); break;
	default: r->err = 1;
	}
}
static void do_layer(rd *r, int depth, uint64_t id)
{
	u32(r);
	const uint8_t *pod = raw(r, 0x58);
	contents_t ct; memset(&ct, 0, sizeof ct);
	uint8_t ctype = r->p < r->n ? r->b[r->p] : 0; size_t cpos = r->p;
	skip_obj(r, depth + 1, &ct);
	if (r->err) return;
	if (getenv("CA_DEBUG") && ctype != 0) { double dd[6]; memcpy(dd, pod + 0x28, sizeof dd); if (dd[4] >= 10) fprintf(stderr, "ca_scene: layer %llx %gx%g contents object type %#x (%zu bytes, kind %d)\n", (unsigned long long)id, dd[4], dd[5], ctype, r->p - cpos, ct.kind); }
	uint64_t subs[MAXS]; int ns = 0, hassubs = 0;
	{ size_t p0 = r->p; skip_arr(r, depth, subs, &ns); hassubs = r->p > p0 + 1; }
	u64(r); skip_obj(r, depth + 1, NULL);
	if (r->err) return;
	if (id == 0 || pod == NULL) return;
	layer_t *l = get(id);
	if (!l) return;
	double d[6]; memcpy(d, pod + 0x28, sizeof d);
	float col[5]; memcpy(col, pod, sizeof col);
	if (d[4] > 0 && d[5] > 0) {		/* a record with geometry; a later record that only changes the contents (an implicit animation of "contents": ControlCenter updating its clock) carries a zero block and must not reset the layer */
		l->px_ = d[0]; l->py_ = d[1]; l->w = d[4]; l->h = d[5]; memcpy(l->dd, d, sizeof d); memcpy(l->unk, pod + 0x14, 20);
		memcpy(l->col, col, sizeof col);
		l->any_col = col[0] != 0 || col[1] != 0 || col[2] != 0 || col[3] != 0 || col[4] != 0;
	}
	if (hassubs) { l->nsubs = ns; memcpy(l->subs, subs, sizeof(uint64_t) * ns); }
	if (ct.kind) {
		if (getenv("CA_DEBUG")) fprintf(stderr, "ca_scene: layer %llx contents: kind %d image %llx attached %d %dx%d bpp %d stride %d (ool %d taken %d of %d)\n", (unsigned long long)id, ct.kind, (unsigned long long)ct.imgid, ct.attached, ct.w, ct.h, ct.bpp, ct.stride, cur_ool_msg, ool_taken, n_ool);
		l->img = ct.imgid; l->has_tint = ct.has_tint; memcpy(l->tint, ct.tint, 16); l->has_shape = ct.has_shape; memcpy(l->shape, ct.shape, 16);
		if (ct.has_shape && getenv("CA_DEBUG_EXT")) fprintf(stderr, "ca_scene: layer %llx image %dx%d shape %d %d %d %d\n", (unsigned long long)id, ct.w, ct.h, ct.shape[0], ct.shape[1], ct.shape[2], ct.shape[3]);
		if (ct.kind == 1 && ct.attached == 2 && cur_ool_msg >= 0) {		/* shared memory: the next memory-entry port of the message that carender-stub could map */
			uint8_t *px = NULL; size_t sz = 0;
			while (px == NULL && port_taken < n_port) px = ool_loader(ool_ctx, cur_ool_msg, port_desc[port_taken++], &sz);
			if (px) {
				img_t im; im.w = ct.w; im.h = ct.h; im.bpp = ct.bpp; im.stride = ct.stride > 0 ? ct.stride : ct.w * ct.bpp; im.px = px;
				{	/* a CA::Render::Shmem of an Image starts with a header (u32: 2, 2, data offset 0x100, format, width, height, width, height ...: seen in the memory of SwiftUI's Text, 9 = A8); the pixels follow it */
					const uint32_t *hd = (const uint32_t *)px;
					if (getenv("CA_DEBUG_EXT") && sz >= 0x20) fprintf(stderr, "ca_scene: shm of layer %llx: %zu bytes, header %x %x %x %x %u %u %u %u, record %dx%d stride %d\n", (unsigned long long)id, sz, hd[0], hd[1], hd[2], hd[3], hd[4], hd[5], hd[6], hd[7], ct.w, ct.h, ct.stride);
					if (sz >= 0x20 && hd[0] == 2 && hd[1] == 2 && hd[2] >= 0x20 && hd[2] < sz && hd[6] > 0 && hd[6] <= hd[4] && hd[7] > 0 && hd[7] <= hd[5] && (int)hd[4] <= ct.w + 64 && (int)hd[5] <= ct.h + 64) {
						im.w = (int)hd[6]; im.h = (int)hd[7];		/* the width of the Image record is the width of the buffer (a power of two: 64, 128 for a line of 49 pixels); the header of the memory has the size of the buffer (u32 4, 5) and the size of the picture (u32 6, 7: 64 x 16 buffer, 48 x 16 picture) */
						memmove(px, px + hd[2], sz - hd[2]); sz -= hd[2];
					}
				}
				if (im.h > 0 && sz >= (size_t)im.stride * (im.h - 1) + (size_t)im.w * im.bpp) setimg(ct.imgid, &im); else free(px);
			}
		} else if (ct.kind == 1 && ct.attached == 0 && ct.inl != NULL && ct.inl_n > 0) {		/* the pixels travel inside the commit stream (a small Image: the label of a Text that changed): a copy of them */
			uint8_t *px = malloc(ct.inl_n); size_t sz = ct.inl_n;
			if (px) {
				memcpy(px, ct.inl, sz);
				img_t im; im.w = ct.w; im.h = ct.h; im.bpp = ct.bpp; im.stride = ct.stride > 0 ? ct.stride : ct.w * ct.bpp; im.px = px;
				if (getenv("CA_DEBUG")) fprintf(stderr, "ca_scene: layer %llx inline image %dx%d bpp %d stride %d: %zu bytes\n", (unsigned long long)id, ct.w, ct.h, ct.bpp, ct.stride, sz);
				if (ct.h > 1 && ct.w > 0 && sz > (size_t)im.stride * (ct.h - 1)) {
					size_t last = sz - (size_t)im.stride * (ct.h - 1);
					if (last % (size_t)ct.w == 0 && (last / ct.w == 1 || last / ct.w == 2 || last / ct.w == 4 || last / ct.w == 8)) im.bpp = (int)(last / ct.w);
				}
				if (ct.h > 0 && sz >= (size_t)im.stride * (ct.h - 1) + (size_t)ct.w * im.bpp) setimg(ct.imgid, &im); else free(px);
			}
		} else if (ct.kind == 1 && ct.attached && cur_ool_msg >= 0 && ool_taken < n_ool) {
			size_t sz = 0;
			uint8_t *px = ool_loader(ool_ctx, cur_ool_msg, ool_desc[ool_taken++], &sz);
			if (getenv("CA_DEBUG")) fprintf(stderr, "ca_scene: layer %llx out-of-line image %dx%d bpp %d stride %d: %s, %zu bytes, need %zu\n", (unsigned long long)id, ct.w, ct.h, ct.bpp, ct.stride, px ? "loaded" : "NOT LOADED", sz, (size_t)(ct.stride > 0 ? ct.stride : ct.w * ct.bpp) * (ct.h > 0 ? ct.h - 1 : 0) + (size_t)ct.w * ct.bpp);
			if (px) {
				img_t im; im.w = ct.w; im.h = ct.h; im.bpp = ct.bpp; im.stride = ct.stride > 0 ? ct.stride : ct.w * ct.bpp; im.px = px;
				if (ct.h > 1 && ct.w > 0 && sz > (size_t)im.stride * (ct.h - 1)) {		/* the last row is not padded: its bytes tell the pixel size (66 bytes for 33 pixels: 2 bytes, grey + alpha) */
					size_t last = sz - (size_t)im.stride * (ct.h - 1);
					if (last % (size_t)ct.w == 0 && (last / ct.w == 1 || last / ct.w == 2 || last / ct.w == 4 || last / ct.w == 8)) im.bpp = (int)(last / ct.w);
				}
				if (ct.h > 0 && sz >= (size_t)im.stride * (ct.h - 1) + (size_t)ct.w * im.bpp) setimg(ct.imgid, &im); else free(px);       /* the last row is not padded: 29 * 192 + 168 = 5736 bytes for 42 x 30 */
			}
		}
	}
	{
	uint8_t ext = u8(r);
	contents_t cls0; memset(&cls0, 0, sizeof cls0);
	if (ext) {
		{ contents_t cc6, cx[12]; memset(&cc6, 0, sizeof cc6); memset(cx, 0, sizeof cx);
		if (getenv("CA_DEBUG_EXT") && r->p < r->n) fprintf(stderr, "ca_scene: ext of layer %llx: first object type %#x\n", (unsigned long long)id, r->b[r->p]);
		for (int i = 0; i < 1 + 6 + 1 + 4; i++) { if (getenv("CA_DEBUG_EXT") && r->p < r->n) fprintf(stderr, "ca_scene: ext object %d of layer %llx type %#x\n", i, (unsigned long long)id, r->b[r->p]); skip_obj(r, depth + 1, i == 0 ? &cls0 : &cx[i]); if (i == 6) cc6 = cx[6]; }
		if (getenv("CA_DEBUG_EXT")) for (int i = 1; i < 12; i++) if (cx[i].nvec) fprintf(stderr, "ca_scene: ext object %d of layer %llx is a vector: %g %g %g %g\n", i, (unsigned long long)id, cx[i].vec[0], cx[i].vec[1], cx[i].vec[2], cx[i].vec[3]);
		if (cc6.nvec == 4 && id && !r->err) { layer_t *lq = get(id); if (lq) { memcpy(lq->ccenter, cc6.vec, 32); lq->has_ccenter = 1; } } }
		skip_arr(r, depth, NULL, NULL); skip_arr(r, depth, NULL, NULL);
		for (int i = 0; i < 3; i++) skip_obj(r, depth + 1, NULL);
		skip_arr(r, depth, NULL, NULL);
		skip_obj(r, depth + 1, NULL); skip_obj(r, depth + 1, NULL);
		skip_arr(r, depth, NULL, NULL);
		{ const uint8_t *ra = raw(r, 16), *rb = raw(r, 16); if (ra && rb && id && !r->err) { layer_t *lr = get(id); if (lr) { memcpy(lr->rectA, ra, 16); memcpy(lr->rectB, rb, 16); lr->has_rects = 1; } } }
		u64(r); u64(r); raw(r, 0x14); const uint8_t *sof = raw(r, 8);
		uint32_t srb = u32(r); if (id && !r->err && sof) { layer_t *ls = get(id); if (ls) { memcpy(ls->shoff, sof, 8); memcpy(&ls->shrad, &srb, 4); ls->has_sh = 1; } }	/* shadowOffset (two floats) and shadowRadius, measured: 4 6 / 8 for .shadow(radius: 8, x: 4, y: 6); the shadow opacity is the byte at pod + 0x15 (0xff = 1.0) */
		{ uint32_t crb = u32(r); float crf; memcpy(&crf, &crb, 4); if (id && !r->err) { layer_t *lc = get(id); if (lc) { lc->cr = crf; lc->cls = cls0.cls; if (cls0.grad) { if (lc->grad) { free(lc->grad->v); free(lc->grad->loc); free(lc->grad); } lc->grad = cls0.grad; } if (cls0.pshape) { if (lc->pshape) { free(lc->pshape->types); free(lc->pshape->pts); free(lc->pshape); } lc->pshape = cls0.pshape; } } } } u32(r); uint32_t bwb = u32(r);
		{ const uint8_t *bc = raw(r, 0x14); if (id && !r->err && bc) { layer_t *lb = get(id); if (lb) { memcpy(&lb->bw, &bwb, 4); memcpy(lb->bcol, bc, 20); } } }	/* borderWidth (the second word after the corner radius) and the border colour (5 floats), measured against the stream of a SwiftUI stroke: 4.0 and green */
		for (int i = 0; i < 5; i++) u32(r);
		raw(r, 0x14); for (int i = 0; i < 5; i++) u32(r);
		raw(r, 0x14); u32(r); u64(r); skip_obj(r, depth + 1, NULL); skip_obj(r, depth + 1, NULL); u8(r);
	}
	}
}

int ca_resynced(void) { int n = 0; for (int i = 0; i < nscenes; i++) n += scenes[i]->nskipped; return n; }
static void message(const uint8_t *m, size_t n, int *nmsg)
{
	if (n < 0x1c) return;
	uint32_t bits; memcpy(&bits, m, 4);
	size_t st = 0x1c;
	n_ool = 0; ool_taken = 0; cur_ool_msg = -1; n_port = 0; port_taken = 0;
	if (bits & 0x80000000u) {
		uint32_t nd; memcpy(&nd, m + 0x18, 4);
		for (uint32_t i = 0; i < nd && st + 12 <= n; i++) {
			if (st + 12 <= n && m[st + 11] == 1) { if (n_ool < 256) ool_desc[n_ool++] = (int)i; st += 16; } else { if (m[st + 11] == 0 && n_port < 256) port_desc[n_port++] = (int)i; st += 12; }
		}
		cur_ool_msg = (*nmsg)++;
	}
	uint32_t mid; memcpy(&mid, m + 0x14, 4);
	uint8_t *ool0 = NULL; size_t osz = 0;
	if ((mid == 0x9c43 || mid == 0x9c45) && (bits & 0x80000000u) && n_ool >= 1 && cur_ool_msg >= 0) {	/* a large commit: its command stream is the first out-of-line descriptor, the images follow */
		ool0 = ool_loader(ool_ctx, cur_ool_msg, ool_desc[0], &osz); ool_taken = 1;
		if (getenv("CA_DEBUG")) fprintf(stderr, "ca_scene: large commit %d: stream %zu bytes (%s), %d out-of-line descriptors\n", cur_ool_msg, osz, ool0 ? "loaded" : "NOT LOADED", n_ool);
		if (ool0 == NULL) return;
	}
	rd r = ool0 ? (rd){ ool0, osz, 0, 0 } : (rd){ m + st, n - st, 0, 0 };
	if (getenv("CA_DUMP")) { static int nd; char nm[64]; snprintf(nm, sizeof nm, "%s/ca-stream-%03d.bin", getenv("CA_DUMP"), nd++); FILE *df = fopen(nm, "wb"); if (df) { fwrite(r.b, 1, r.n, df); fclose(df); } }       /* a debugging aid: every command stream into a file */
	u32(&r); uint32_t msg_ctx = u32(&r); u64(&r);
	if (getenv("CA_DEBUG")) fprintf(stderr, "ca_scene: commit of context %u: %zu stream bytes\n", (unsigned)msg_ctx, r.n);
	while (!r.err && r.p < r.n) {
		uint8_t op = u8(&r);
		if (getenv("CA_DEBUG") && getenv("CA_DEBUG")[0] == '2') fprintf(stderr, "ca_scene:   ctx %u op %#x at %zu\n", (unsigned)msg_ctx, op, r.p - 1);
		if (op == 0) break;
		else if (op == 1) { uint64_t id = u64(&r); u32(&r); layer_t *l = find(id); if (l) l->used = 0; }
		else if (op == 2) {
			size_t start = r.p;
			uint64_t id = u64(&r); u32(&r); uint8_t t = r.b[r.p < r.n ? r.p : 0];
			if (t == 0x1e) { r.p++; do_layer(&r, 0, id); } else skip_obj(&r, 0, NULL);
			u32(&r);
			if (r.err) {		/* an object that is not decoded yet (types 3, 25, 63, 160...): go on at the next layer record (02 + id + generation + 0x1e) */
				size_t q;
				for (q = start + 1; q + 14 <= r.n; q++)
					if (r.b[q] == 0x02 && r.b[q + 11] == 0 && r.b[q + 12] == 0 && r.b[q + 13] == 0x1e) break;
				if (q + 14 > r.n) break;
				if (getenv("CA_DEBUG")) { fprintf(stderr, "ca_scene: record at %zu failed at %zu (id %llx), next record at %zu of %zu; type %02x; bytes:", start, r.p, (unsigned long long)id, q, r.n, start + 13 < r.n ? r.b[start + 13] : 0); for (size_t z = start; z < start + 80 && z < r.n; z++) fprintf(stderr, " %02x", r.b[z]); fprintf(stderr, "\n"); }
				r.err = 0; r.p = q; skipped++;
			}
		}
		else if (op == 7) {			/* the root layer of the context */
			uint64_t rid = u64(&r); int k;
			if (getenv("CA_DEBUG")) fprintf(stderr, "ca_scene: context %u: root layer %llx\n", (unsigned)msg_ctx, (unsigned long long)rid);
			for (k = 0; k < S->nroots; k++) if (S->rootctx[k] == msg_ctx) break;
			if (k < MAXROOTS) { S->rootctx[k] = msg_ctx; S->rootid[k] = rid; if (k == S->nroots) S->nroots++; }
		}
		else if (op == 3 || op == 0x1a || op == 0x1c || op == 0x1d || op == 0x1e || op == 0xb || op == 0xc) u64(&r);
		else if (op == 9) { u8(&r); u8(&r); }
		else if (op == 8 || op == 10) skip_cf(&r, 0);
		else if (op == 0xd) u32(&r);
		else if (op == 0x20) { u32(&r); u8(&r); }
		else if (op == 0x25) ;
		else if (op == 4) {			/* add_animation (layer id, an Animation object): not decoded yet (BasicAnimation, Timing, KeyframeAnimation ...); CoreAnimation sends one when a layer's contents change (the implicit "contents" action): go on at the next layer record */
			size_t q;
			for (q = r.p; q + 14 <= r.n; q++)
				if (r.b[q] == 0x02 && r.b[q + 11] == 0 && r.b[q + 12] == 0 && r.b[q + 13] == 0x1e) break;
			if (q + 14 > r.n) break;
			if (getenv("CA_DEBUG")) fprintf(stderr, "ca_scene: animation skipped, next record at %zu of %zu\n", q, r.n);
			r.p = q; skipped++;
		}
		else if (op == 0x1f) u8(&r);
		else break;
	}
	free(ool0);
}

static int g_nmsg;		/* complex messages fed so far: the number of the out-of-line data of the next one */
void ca_set_msg_number(int n) { g_nmsg = n; }
void ca_reset(void)
{
	for (int k = 0; k < nscenes; k++) {
		for (int i = 0; i < scenes[k]->nimages; i++) free(scenes[k]->images[i].im.px);
		free(scenes[k]); scenes[k] = NULL;
	}
	nscenes = 0; S = NULL; g_nmsg = 0;
}
/* the scene of a context (made when it is first seen) */
static struct scene *scene_for(uint32_t ctx, int *idx)
{
	for (int i = 0; i < nscenes; i++) if (scenes[i]->ctx == ctx) { *idx = i; return scenes[i]; }
	if (nscenes >= MAXSCENES) return NULL;
	struct scene *n = calloc(1, sizeof *n);
	if (!n) return NULL;
	n->ctx = ctx; scenes[nscenes] = n; *idx = nscenes++;
	return n;
}
/* one commit message; returns the index of the scene it belongs to (the context is the second word of its body), -1 when it was not understood */
int ca_feed_key(uint32_t key, const uint8_t *m, size_t n)
{
	int idx = -1;
	if (n < 0x24) return -1;
	if ((S = scene_for(key, &idx)) == NULL) return -1;
	message(m, n, &g_nmsg);
	return idx;
}
int ca_feed(const uint8_t *m, size_t n)
{
	size_t st = 0x1c; uint32_t bits, ctx = 0;
	if (n < 0x24) return -1;
	memcpy(&bits, m, 4);
	if (bits & 0x80000000u) { uint32_t nd; memcpy(&nd, m + 0x18, 4); for (uint32_t i = 0; i < nd && st + 12 <= n; i++) st += m[st + 11] == 1 ? 16 : 12; }
	if (st + 8 > n) return -1;
	memcpy(&ctx, m + st + 4, 4);
	return ca_feed_key(ctx, m, n);
}
int ca_load_file(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f) return -1;
	fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
	uint8_t *d = malloc(sz + 1);
	if (!d || fread(d, 1, sz, f) != (size_t)sz) { fclose(f); free(d); return -1; }
	fclose(f);
	ca_reset();
	for (long p = 0; p + 4 <= sz;) {
		uint32_t n; memcpy(&n, d + p, 4); p += 4;
		if (p + n > (unsigned long)sz) break;
		ca_feed(d + p, n);
		p += n;
	}
	free(d);
	return 0;
}

/* ---- painter ---- */
static float half(uint16_t h)
{
	uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023, o;
	if (e == 0) { if (m == 0) o = s << 31; else { e = 1; while (!(m & 1024)) { m <<= 1; e--; } m &= 1023; o = (s << 31) | ((e + 112) << 23) | (m << 13); } }
	else if (e == 31) o = (s << 31) | 0x7f800000 | (m << 13);
	else o = (s << 31) | ((e + 112) << 23) | (m << 13);
	float f; memcpy(&f, &o, 4); return f;
}
typedef struct { uint8_t *px; int w, h, stride; } ca_canvas;
static void blend(ca_canvas *win, int x, int y, int r, int g, int b, int a)
{
	if (x < 0 || y < 0 || x >= win->w || y >= win->h || a <= 0) return;
	uint8_t *p = win->px + (size_t)y * win->stride + x * 4;
	if (a > 255) a = 255;
	p[0] = (uint8_t)((p[0] * (255 - a) + b * a) / 255); p[1] = (uint8_t)((p[1] * (255 - a) + g * a) / 255); p[2] = (uint8_t)((p[2] * (255 - a) + r * a) / 255); p[3] = (uint8_t)(a + p[3] * (255 - a) / 255);
}
/* the geometry of a layer as CoreAnimation has it (CA::Render::Layer::compute_frame_offset, decompiled): position (dd[0], dd[1]), bounds origin (dd[2], dd[3]), bounds size (dd[4], dd[5]); the anchor point is (0, 0) when bit 0
 * of the byte at offset 0x33 of the object (the high byte of unk[3]) is set, (0.5, 0.5) otherwise; the frame origin in the parent is position - (anchor * size + bounds origin), and the position of a child is in
 * the bounds space of its parent (the origin of its bounds is the point of the parent's frame origin).  (A layer with an extension block can have another anchor point: not read yet.) */
/* fills the path of a shape layer: the path is in the layer's own coordinates (y down inside the flipped views of AppKit, as measured: the check mark of the Toggle runs from the top left to the bottom and up to the top right), flattened (cubics and quadratics in 8 steps), filled with the non-zero or even-odd rule by 4 x 4 samples per pixel.  The colour is premultiplied like the layer's colour. */
static void paint_shape(ca_canvas *win, const shape_t *sh, int bx, int by, int iw, int ih, double op)
{
	enum { MAXE = 4096 };
	static double ex0[MAXE], ey0[MAXE], ex1[MAXE], ey1[MAXE];
	int ne = 0; double cx = 0, cy = 0, sx = 0, sy = 0; size_t pi = 0;
	float a = sh->fill[3]; if (a <= 0 || iw <= 0 || ih <= 0) return;
	for (int e = 0; e < sh->nt && ne < MAXE - 24; e++) {
		int t = sh->types[e]; const double *p = sh->pts + pi * 2;
		int need = t == 1 || t == 2 ? 1 : t == 3 ? 2 : t == 4 ? 3 : 0;	/* the path element codes of the stream: 0 close, 1 move, 2 line, 3 quadratic, 4 cubic (path_counts 0 1 1 2 3: 29 points for the check mark's 14 elements, measured) */
		if (pi + need > (size_t)sh->np) break;
		if (t == 1) { if (ne && (cx != sx || cy != sy)) { ex0[ne] = cx; ey0[ne] = cy; ex1[ne] = sx; ey1[ne] = sy; ne++; } cx = sx = p[0]; cy = sy = p[1]; }
		else if (t == 2) { ex0[ne] = cx; ey0[ne] = cy; ex1[ne] = p[0]; ey1[ne] = p[1]; ne++; cx = p[0]; cy = p[1]; }
		else if (t == 3 || t == 4) {
			for (int k = 1; k <= 8; k++) {
				double u = k / 8.0, v = 1 - u, nx, ny;
				if (t == 3) { nx = v * v * cx + 2 * v * u * p[0] + u * u * p[2]; ny = v * v * cy + 2 * v * u * p[1] + u * u * p[3]; }
				else { nx = v * v * v * cx + 3 * v * v * u * p[0] + 3 * v * u * u * p[2] + u * u * u * p[4]; ny = v * v * v * cy + 3 * v * v * u * p[1] + 3 * v * u * u * p[3] + u * u * u * p[5]; }
				static double px, py; (void)px; (void)py;
				ex0[ne] = k == 1 ? cx : ex1[ne - 1]; ey0[ne] = k == 1 ? cy : ey1[ne - 1]; ex1[ne] = nx; ey1[ne] = ny; ne++;
			}
			cx = p[(need - 1) * 2]; cy = p[(need - 1) * 2 + 1];
		} else if (t == 0) { if (cx != sx || cy != sy) { ex0[ne] = cx; ey0[ne] = cy; ex1[ne] = sx; ey1[ne] = sy; ne++; } cx = sx; cy = sy; }
		pi += need;
	}
	if (ne && (cx != sx || cy != sy)) { ex0[ne] = cx; ey0[ne] = cy; ex1[ne] = sx; ey1[ne] = sy; ne++; }
	double ia = 1.0 / a; int fr = (int)(sh->fill[0] * ia * 255 + 0.5), fg = (int)(sh->fill[1] * ia * 255 + 0.5), fb = (int)(sh->fill[2] * ia * 255 + 0.5);
	if (fr > 255) fr = 255; if (fg > 255) fg = 255; if (fb > 255) fb = 255;
	for (int j = 0; j < ih; j++) for (int i = 0; i < iw; i++) {
		int hit = 0;
		for (int sj = 0; sj < 4; sj++) for (int si = 0; si < 4; si++) {
			double x = i + (si + 0.5) / 4, y = j + (sj + 0.5) / 4; int wn = 0;
			for (int e = 0; e < ne; e++) {
				double y0 = ey0[e], y1 = ey1[e];
				if ((y0 <= y && y1 > y) || (y1 <= y && y0 > y)) { double xc = ex0[e] + (y - y0) * (ex1[e] - ex0[e]) / (y1 - y0); if (xc > x) wn += y1 > y0 ? 1 : -1; }
			}
			if (sh->rule ? (wn & 1) : wn != 0) hit++;
		}
		if (hit) blend(win, bx + i, by + j, fr, fg, fb, (int)(a * op * hit / 16.0 * 255));
	}
}
static void paint_layer_f(ca_canvas *win, uint64_t id, double ox, double oy, double pbx, double pby, double op, int depth, int pflip, double ph);
static void paint_layer(ca_canvas *win, uint64_t id, double ox, double oy, double pbx, double pby, double op, int depth) { paint_layer_f(win, id, ox, oy, pbx, pby, op, depth, 0, 0); }
/* pflip, ph: the parent has geometryFlipped (bit 0x800 of the word at pod offset 8: NSHostingView's backing layer has it, its children measure y from the top) and this height */
static void paint_layer_f(ca_canvas *win, uint64_t id, double ox, double oy, double pbx, double pby, double op, int depth, int pflip, double ph)
{
	layer_t *l = find(id);
	int H = win->h;
	if (!l || depth > 64) return;
	if (getenv("CA_PAINT_DEBUG") && l->nsubs >= 0)	fprintf(stderr, "ca_scene: tree depth %d layer %llx d=%g %g %g %g %g %g unk=%08x %08x %08x %08x %08x nsubs %d cls %c col %g %g %g %g %g any %d cr %g img %d\n", depth, (unsigned long long)id, l->dd[0], l->dd[1], l->dd[2], l->dd[3], l->dd[4], l->dd[5], l->unk[0], l->unk[1], l->unk[2], l->unk[3], l->unk[4], l->nsubs, l->cls ? l->cls : '-', l->col[0], l->col[1], l->col[2], l->col[3], l->col[4], l->any_col, l->cr, l->img != 0);
	if (getenv("CA_PAINT_DEBUG") && depth <= 1) fprintf(stderr, "ca_scene: paint depth %d id %llx at %.1f,%.1f size %.1fx%.1f parent origin %.1f,%.1f nsubs %d\n", depth, (unsigned long long)id, l->px_, l->py_, l->w, l->h, ox, oy, l->nsubs);
	double ax = (l->unk[3] >> 24) & 1 ? 0.0 : 0.5, fx = l->dd[0] - (ax * l->w + l->dd[2]), fy = l->dd[1] - (ax * l->h + l->dd[3]);
	if (pflip && depth > 0) fy = ph - (fy + l->h);
	double x = depth == 0 ? 0 : ox + fx - pbx, y = depth == 0 ? 0 : oy + fy - pby, o = (l->unk[0] & 0xff) / 255.0, a = l->any_col ? l->col[3] : 0.0;	/* measured against the layer tree of the same window on the Mac (SwiftUI/tests/hello, TREE lines): the opacity is the byte at pod + 0x14 (0xff = 1.0; 0x1a = the CAChameleonLayer's 0.1), the colour floats are the background colour PREMULTIPLIED (white at alpha 0.098 is 0.098 0.098 0.098 0.098) and the fifth float is not the opacity (it is 0 for translucent fills, 1 for opaque ones) */
	double op2 = op * (o > 0 ? (o > 1 ? 1 : o) : 0);
	if (l->w > 0 && l->h > 0 && op2 > 0) {
		int bx = (int)x, by = (int)(H - (y + l->h)), iw = (int)l->w, ih = (int)l->h;
		if (l->has_sh && (l->unk[0] >> 8 & 0xff) && a > 0) {	/* the shadow: the layer's rectangle blurred (gaussian, sigma = radius / 2), black, drawn under the layer, before its fill */
			double sg = l->shrad > 0.5 ? l->shrad / 2.0 : 0.5, so = ((l->unk[0] >> 8) & 0xff) / 255.0 * a * op2; int m = (int)(sg * 3 + 1);
			for (int j = -m; j < ih + m; j++) for (int i = -m; i < iw + m; i++) {
				double cx_ = i + 0.5 - l->shoff[0], cy_ = j + 0.5 - l->shoff[1];
				double ax = 0.5 * (erf((cx_ - 0) / (sg * 1.41421356)) - erf((cx_ - iw) / (sg * 1.41421356)));
				double ay = 0.5 * (erf((cy_ - 0) / (sg * 1.41421356)) - erf((cy_ - ih) / (sg * 1.41421356)));
				int al = (int)(so * ax * ay * 255); if (al > 0) blend(win, bx + i, by + j, 0, 0, 0, al);
			}
		}
		if (a > 0) {
			double cr = l->cr > 0 ? l->cr : 0, hm = (l->w < l->h ? l->w : l->h) / 2.0;	/* cornerRadius: the fill is a rounded rectangle (the radius is at most half the short side) */
			if (cr > hm) cr = hm;
			for (int j = 0; j < ih; j++) for (int i = 0; i < iw; i++) {
				double cov = 1.0;
				if (cr > 0.5) {		/* distance of the pixel centre from the corner circle: a one pixel wide edge */
					double px_ = i + 0.5, py_ = j + 0.5, cx_ = px_ < cr ? cr : (px_ > iw - cr ? iw - cr : px_), cy_ = py_ < cr ? cr : (py_ > ih - cr ? ih - cr : py_);
					double dx_ = px_ - cx_, dy_ = py_ - cy_, d_ = dx_ * dx_ + dy_ * dy_;
					if (d_ > 0) { double dist = __builtin_sqrt(d_) - cr + 0.5; cov = dist <= 0 ? 1.0 : (dist >= 1 ? 0.0 : 1.0 - dist); }
				}
				if (cov > 0) { double ia = a > 0 ? 1.0 / a : 0; int fr = (int)(l->col[0] * ia * 255 + 0.5), fg = (int)(l->col[1] * ia * 255 + 0.5), fb = (int)(l->col[2] * ia * 255 + 0.5); blend(win, bx + i, by + j, fr > 255 ? 255 : fr, fg > 255 ? 255 : fg, fb > 255 ? 255 : fb, (int)(a * op2 * cov * 255)); }
			}
		}
		if (l->bw > 0 && l->bcol[3] > 0) {		/* the border: inside the bounds, along the rounded rectangle (signed distance of the pixel centre to its edge), one pixel of anti-aliasing */
			double cr_ = l->cr > 0 ? l->cr : 0, hx = iw / 2.0, hy = ih / 2.0, ia = 1.0 / l->bcol[3];
			if (cr_ > hx) cr_ = hx; if (cr_ > hy) cr_ = hy;
			int br_ = (int)(l->bcol[0] * ia * 255 + 0.5), bg_ = (int)(l->bcol[1] * ia * 255 + 0.5), bb_ = (int)(l->bcol[2] * ia * 255 + 0.5);
			if (br_ > 255) br_ = 255; if (bg_ > 255) bg_ = 255; if (bb_ > 255) bb_ = 255;
			for (int j = 0; j < ih; j++) for (int i = 0; i < iw; i++) {
				double qx = fabs(i + 0.5 - hx) - hx + cr_, qy = fabs(j + 0.5 - hy) - hy + cr_, mx = qx > 0 ? qx : 0, my = qy > 0 ? qy : 0;
				double sd = sqrt(mx * mx + my * my) + (qx > qy ? (qx < 0 ? qx : 0) : (qy < 0 ? qy : 0)) - cr_;
				double c1 = 0.5 - sd, c2 = sd + l->bw + 0.5; c1 = c1 < 0 ? 0 : c1 > 1 ? 1 : c1; c2 = c2 < 0 ? 0 : c2 > 1 ? 1 : c2;
				if (c1 * c2 > 0) blend(win, bx + i, by + j, br_, bg_, bb_, (int)(l->bcol[3] * op2 * c1 * c2 * 255));
			}
		}
		if (l->grad && l->grad->n >= 2 && l->grad->v) {		/* axial gradient, measured: stops are r g b a (premultiplied) + 1 float; start/end are unit coordinates */
			grad_t *g = l->grad; double sx = g->s[0] * iw, sy = (1 - g->s[1]) * ih, dx = (g->e[0] - g->s[0]) * iw, dy = -(g->e[1] - g->s[1]) * ih, dd = dx * dx + dy * dy;
			for (int j = 0; j < ih && dd > 0; j++) for (int i = 0; i < iw; i++) {
				double t = ((i + 0.5 - sx) * dx + (j + 0.5 - sy) * dy) / dd; t = t < 0 ? 0 : t > 1 ? 1 : t;
				int k = 0; while (k < g->n - 2 && g->loc && t > g->loc[k + 1]) k++;
				double l0 = g->loc ? g->loc[k] : (double)k / (g->n - 1), l1 = g->loc ? g->loc[k + 1] : (double)(k + 1) / (g->n - 1), u = l1 > l0 ? (t - l0) / (l1 - l0) : 0; u = u < 0 ? 0 : u > 1 ? 1 : u;
				const float *c0 = g->v + 5 * k, *c1 = g->v + 5 * (k + 1); double c[4]; for (int q = 0; q < 4; q++) c[q] = c0[q] + (c1[q] - c0[q]) * u;
				if (c[3] > 0) { int rr = (int)(c[0] / c[3] * 255 + 0.5), gg = (int)(c[1] / c[3] * 255 + 0.5), bb = (int)(c[2] / c[3] * 255 + 0.5); rr = rr > 255 ? 255 : rr; gg = gg > 255 ? 255 : gg; bb = bb > 255 ? 255 : bb; blend(win, bx + i, by + j, rr, gg, bb, (int)(c[3] * op2 * 255)); }
			}
		}
		if (l->pshape && l->pshape->has_path) paint_shape(win, l->pshape, bx, by, iw, ih, op2);
		img_t *im = l->img ? getimg(l->img) : NULL;
		if (im && getenv("CA_PAINT_DEBUG")) fprintf(stderr, "ca_scene: image of layer %llx painted at %d,%d size %dx%d opacity %.2f (canvas %dx%d, depth %d)\n", (unsigned long long)id, bx, by, iw, ih, op2, win->w, H, depth);
		if (im && im->bpp == 2 && getenv("CA_PAINT_DEBUG")) fprintf(stderr, "ca_scene: ga8 layer %llx image %dx%d in layer %gx%g at %g,%g ccenter %d\n", (unsigned long long)id, im->w, im->h, l->w, l->h, x, y, l->has_ccenter);
		if (im && im->bpp == 1 && getenv("CA_PAINT_DEBUG")) fprintf(stderr, "ca_scene: mask layer %llx %dx%d col %g %g %g %g %g any %d ext_cls %c\n", (unsigned long long)id, im->w, im->h, l->col[0], l->col[1], l->col[2], l->col[3], l->col[4], l->any_col, l->cls ? l->cls : '-');
		if (im && l->has_ccenter && getenv("CA_PAINT_DEBUG")) fprintf(stderr, "ca_scene: contentsCenter of layer %llx img %dx%d: %g %g %g %g\n", (unsigned long long)id, im->w, im->h, l->ccenter[0], l->ccenter[1], l->ccenter[2], l->ccenter[3]);
		if (im && l->has_rects && getenv("CA_PAINT_DEBUG")) fprintf(stderr, "ca_scene: rects of layer %llx img %dx%d: A %g %g %g %g  B %g %g %g %g\n", (unsigned long long)id, im->w, im->h, l->rectA[0], l->rectA[1], l->rectA[2], l->rectA[3], l->rectB[0], l->rectB[1], l->rectB[2], l->rectB[3]);
		if (im && im->w > 0 && im->h > 0)
			for (int j = 0; j < ih; j++) for (int i = 0; i < iw; i++) {
				int sx = (int)((i + 0.5) * im->w / iw), sy = (int)((j + 0.5) * im->h / ih), r, g, b, al;
				if (im->bpp == 1 && !l->has_ccenter && (im->w > iw || im->h > ih) && im->w <= iw + 40 && im->h <= ih + 40) { sx = i; sy = j; }	/* an alpha-only text image is larger than its layer (the buffer of CoreAnimation is padded: 64 wide for a 48 wide line): drawn at its own size from the top left, clipped to the layer, not squeezed into it */
				if (l->has_ccenter && (l->ccenter[0] > 0 || l->ccenter[1] > 0 || l->ccenter[2] < 1 || l->ccenter[3] < 1) && l->ccenter[2] > l->ccenter[0] && l->ccenter[3] > l->ccenter[1]) {		/* 9-slice: the corners keep their size, the centre of the image stretches */
					double sc = im->h > ih ? (double)((im->h + ih - 1) / ih) : 1.0, cx0 = l->ccenter[0] * im->w, cw_ = (l->ccenter[2] - l->ccenter[0]) * im->w, cy0 = l->ccenter[1] * im->h, ch_ = (l->ccenter[3] - l->ccenter[1]) * im->h;	/* the vector of the 27 x 27 button image is 0.444 0.444 0.556 0.556: (x0, y0, x1, y1) */
					double dl = cx0 / sc, dr = (im->w - cx0 - cw_) / sc, dt = cy0 / sc, db = (im->h - cy0 - ch_) / sc;
					if (iw - dl - dr > 0 && ih - dt - db > 0) {
						sx = i < dl ? (int)(i * sc) : (i >= iw - dr ? (int)(im->w - (iw - i) * sc) : (int)(cx0 + (i - dl) * cw_ / (iw - dl - dr)));
						sy = j < dt ? (int)(j * sc) : (j >= ih - db ? (int)(im->h - (ih - j) * sc) : (int)(cy0 + (j - dt) * ch_ / (ih - dt - db)));
					}
				}
				if (sx >= im->w) sx = im->w - 1;
				if (sy >= im->h) sy = im->h - 1;
				if (im->bpp == 1) {		/* an alpha-only image (the glyphs of CoreUI, format 80AL / A8): a mask, tinted with the colour of the layer (black when it has none) */
					al = im->px[(size_t)sy * im->stride + sx];
					if (l->has_tint) { r = (int)(l->tint[0] * 255 + 0.5); g = (int)(l->tint[1] * 255 + 0.5); b = (int)(l->tint[2] * 255 + 0.5); if (r > 255) r = 255; if (g > 255) g = 255; if (b > 255) b = 255; al = (int)(al * (l->tint[3] > 1 ? 1 : l->tint[3])); }
					else { r = l->any_col ? (int)(l->col[0] * 255) : 0; g = l->any_col ? (int)(l->col[1] * 255) : 0; b = l->any_col ? (int)(l->col[2] * 255) : 0; }
				} else if (im->bpp == 2) {		/* grey + alpha (CoreUI GA8) */
					const uint8_t *s = im->px + (size_t)sy * im->stride + (size_t)sx * 2;
					r = g = b = s[0]; al = s[1];
				} else if (im->bpp == 8) {
					const uint16_t *s = (const uint16_t *)(im->px + (size_t)sy * im->stride) + (size_t)sx * 4;
					float fr = half(s[0]), fg = half(s[1]), fb = half(s[2]), fa = half(s[3]);
					#define CL(v) ((v) != (v) ? 0 : (v) < 0 ? 0 : (v) > 1 ? 255 : (int)((v) * 255))
					r = CL(fr); g = CL(fg); b = CL(fb); al = CL(fa);
				} else {
					const uint8_t *s = im->px + (size_t)sy * im->stride + (size_t)sx * 4;
					b = s[0]; g = s[1]; r = s[2]; al = s[3];
				}
				blend(win, bx + i, by + j, r, g, b, (int)(al * op2));
			}
	}
	for (int i = 0; i < l->nsubs; i++) paint_layer_f(win, l->subs[i], x, y, l->dd[2], l->dd[3], op2 > 0 ? op2 : op, depth + 1, (l->unk[2] & 0x800) != 0, l->h);
}
static void paint_scene(ca_canvas *win)
{
	uint64_t roots[MAXL]; int nr = 0;
	for (int i = 0; i < nL; i++) {
		int child = 0;
		if (!L[i].used) continue;
		for (int k = 0; k < nL && !child; k++) if (L[k].used) for (int s = 0; s < L[k].nsubs; s++) if (L[k].subs[s] == L[i].id) { child = 1; break; }
		if (!child && nr < MAXL) roots[nr++] = L[i].id;
	}
	for (int i = 0; i < nr; i++) for (int k = i + 1; k < nr; k++) {		/* the biggest root first: the back */
		layer_t *a = find(roots[i]), *b = find(roots[k]);
		if (b->w * b->h > a->w * a->h) { uint64_t t = roots[i]; roots[i] = roots[k]; roots[k] = t; }
	}
	for (int i = 0; i < nr; i++) paint_layer(win, roots[i], 0, 0, 0, 0, 1.0, 0);
}
/* The glass of a context: the rectangles of the CASDFElementLayers (the shapes of the Liquid Glass of macOS 26: NSGlassEffectView / SwiftUI glassEffect put them under a CABackdropLayer with the glassBackground filter), in the window of the context (origin at the top left), with their corner radius. */
static void glass_walk(uint64_t id, double ox, double oy, int H, ca_glass *out, int max, int *n, int depth)
{
	layer_t *l = find(id); if (!l || depth > 64) return;
	double x = ox + l->px_ - l->w / 2, y = oy + l->py_ - l->h / 2;
	if (l->cls == 'E' && l->w > 3 && l->h > 3 && *n < max) { out[*n].x = (int)(x + 0.5); out[*n].y = (int)(H - (y + l->h) + 0.5); out[*n].w = (int)(l->w + 0.5); out[*n].h = (int)(l->h + 0.5); out[*n].radius = (int)(l->cr + 0.5); out[*n].mode = (l->w < 160 || l->h < 160) ? 10 : 9; (*n)++; }
	for (int i = 0; i < l->nsubs; i++) glass_walk(l->subs[i], x, y, H, out, max, n, depth + 1);
}
int ca_scene_glass(int scene, int root, int height, ca_glass *out, int max)
{
	int n = 0; if (scene < 0 || scene >= nscenes || root < 0 || root >= scenes[scene]->nroots) return 0;
	S = scenes[scene]; glass_walk(S->rootid[root], 0, 0, height, out, max, &n, 0); return n;
}

/* position and size of the root layer of a root of a scene (the content of the window that its context is bound to: the layer's position is the window's origin in the coordinates of the screen's top-left corner, as far as known) */
int ca_scene_root_geom(int scene, int i, double *x, double *y, double *w, double *h)
{
	layer_t *l;
	if (scene < 0 || scene >= nscenes || i < 0 || i >= scenes[scene]->nroots) return 0;
	S = scenes[scene];
	if ((l = find(S->rootid[i])) == NULL) return 0;
	*x = l->px_; *y = l->py_; *w = l->w; *h = l->h;
	return 1;
}
int ca_scene_nroots(int scene) { return scene >= 0 && scene < nscenes ? scenes[scene]->nroots : 0; }
uint32_t ca_scene_root_ctx(int scene, int i) { return scene >= 0 && scene < nscenes && i >= 0 && i < scenes[scene]->nroots ? scenes[scene]->rootctx[i] : 0; }
/* the tree of one context (its root layer) on a transparent canvas: the content of the window of that context */
void ca_paint_root(int scene, int i, uint8_t *bgra, int w_, int h_, int stride)
{
	ca_canvas cv = { bgra, w_, h_, stride };
	for (int j = 0; j < h_; j++) memset(bgra + (size_t)j * stride, 0, (size_t)w_ * 4);
	if (scene < 0 || scene >= nscenes || i < 0 || i >= scenes[scene]->nroots) return;
	S = scenes[scene];
	if (getenv("CA_PAINT_DEBUG")) {	/* what the layers of the scene carry */
		int ncol = 0, nvis = 0, nimg = 0, nsized = 0, ncls[3] = { 0, 0, 0 };
		for (int i = 0; i < nL; i++) if (L[i].used) { if (L[i].w > 0 && L[i].h > 0) nsized++; if (L[i].any_col) { ncol++; if (L[i].col[3] > 0 && L[i].col[4] > 0) nvis++; } if (L[i].cls == 'E') ncls[0]++; else if (L[i].cls == 'S') ncls[1]++; else if (L[i].cls == 'B') ncls[2]++;
			if (L[i].img) { nimg++; img_t *im = getimg(L[i].img); size_t nzb = 0; if (im && im->px) for (size_t q = 0; q < (size_t)im->stride * im->h; q++) if (im->px[q]) nzb++; fprintf(stderr, "ca_scene:   layer %llx cls %c %.0fx%.0f at %.0f,%.0f image %s %dx%d bpp %d stride %d, %zu non-zero bytes\n", (unsigned long long)L[i].id, L[i].cls ? L[i].cls : '-', L[i].w, L[i].h, L[i].px_, L[i].py_, im ? "found" : "MISSING", im ? im->w : 0, im ? im->h : 0, im ? im->bpp : 0, im ? im->stride : 0, nzb); } }
		fprintf(stderr, "ca_scene: scene layers %d: %d sized, %d with a colour (%d visible), %d with an image, %d SDF element, %d SDF, %d backdrop\n", nL, nsized, ncol, nvis, nimg, ncls[0], ncls[1], ncls[2]);
	}
	paint_layer(&cv, S->rootid[i], 0, 0, 0, 0, 1.0, 0);
}
/* every scene over a dark background (opaque): for a viewer that shows the whole stream in one picture */
void ca_paint(uint8_t *bgra, int w_, int h_, int stride)
{
	ca_canvas cv = { bgra, w_, h_, stride };
	for (int j = 0; j < h_; j++) for (int i = 0; i < w_; i++) { uint8_t *p = bgra + (size_t)j * stride + i * 4; p[0] = 30; p[1] = 28; p[2] = 28; p[3] = 255; }
	for (int k = 0; k < nscenes; k++) { S = scenes[k]; paint_scene(&cv); }
}
/* one scene on a transparent canvas (premultiplied BGRA): the content of a window of WindowServer */
void ca_paint_scene(int idx, uint8_t *bgra, int w_, int h_, int stride)
{
	ca_canvas cv = { bgra, w_, h_, stride };
	for (int j = 0; j < h_; j++) memset(bgra + (size_t)j * stride, 0, (size_t)w_ * 4);
	if (idx < 0 || idx >= nscenes) return;
	S = scenes[idx]; paint_scene(&cv);
}
