/* ca_scene: decoder + painter of the CoreAnimation commit stream of the render server protocol (messages 0x9c42..0x9c45).
 * One scene per process for now.  Out-of-line data (Image pixels) is read from <dir>/ca-ool-<n>-<k>.bin, where n numbers the complex messages and k is the descriptor. */
#ifndef CA_SCENE_H
#define CA_SCENE_H
#include <stdint.h>
#include <stddef.h>
typedef uint8_t *(*ca_ool_fn)(void *ctx, int msg, int desc, size_t *size);	/* the out-of-line data of descriptor desc of the msg-th complex message: malloc'd, or NULL */
void ca_set_dir(const char *dir);
void ca_set_ool_loader(ca_ool_fn fn, void *ctx);	/* default: files in the directory */
void ca_reset(void);
void ca_set_msg_number(int n);	/* the number of the next complex commit (the key of its out-of-line data), when the sender numbers them */
int ca_feed(const uint8_t *msg, size_t n);		/* one commit message (as the render server received it, from the header on); returns the scene (one for each context) it went to, -1 if not understood */
int ca_feed_key(uint32_t key, const uint8_t *msg, size_t n);
void ca_scene_drop(int i);		/* the client of scene i is gone: its layers and images are freed, the slot is reused */	/* the same, with the key of the scene given (the pid of the client) */
typedef struct { int x, y, w, h, radius, mode; const float (*p)[4]; const unsigned char *set; float src_blur; const float *under_m; uint64_t backdrop_id; } ca_glass;	/* p / set: the glassBackground filter's keys (0x14d + i) of the backdrop that lies under the shape, NULL if none */	/* a glass shape: a rounded rectangle in the window (top left origin), mode 1 regular */
int ca_scene_glass(int scene, int root, int height, ca_glass *out, int max);	/* the CASDFElementLayers of a root, for the glass of its window */
int ca_nscenes(void);
int ca_scene_root_geom(int scene, int root, double *x, double *y, double *w, double *h);	/* position / size of the root layer */
int ca_scene_nroots(int scene);			/* the render contexts of a scene that have a root layer */
uint32_t ca_scene_root_ctx(int scene, int i);	/* the context id of the i-th root */
void ca_paint_root(int scene, int i, uint8_t *bgra, int w, int h, int stride);	/* the tree of one context on a transparent canvas */
uint32_t ca_scene_ctx(int scene);
void ca_scene_info(int scene, int *nlayers, int *nimages);
void ca_paint_scene(int scene, uint8_t *bgra, int w, int h, int stride);	/* one scene on a transparent canvas (premultiplied BGRA) */
int ca_load_file(const char *path);		/* the whole stream (u32 length + message, repeated): rebuilds the scene; 0 on success */
void ca_paint(uint8_t *bgra, int w, int h, int stride);
int ca_nlayers(void);
int ca_nimages(void);
int ca_resynced(void);
/* image queues (CAImageQueue: CAMetalLayer ...): the render server names the shared memory of a queue that a layer shows (the msg-th message's descriptor desc, capacity slots) to the side that has it mapped,
 * which then reports the newest image of the queue: ca_queue_frame with its IOSurface id (mapped through /dev/iosurface); returns the scene to repaint or -1 */
typedef void (*ca_watch_fn)(int msg, int desc, int capacity, int port_desc);	/* port_desc: the descriptor of the queue's client port (-1: none) */
void ca_set_queue_watcher(ca_watch_fn fn);
int ca_queue_frame(int msg, int desc, uint32_t surface);
#endif
