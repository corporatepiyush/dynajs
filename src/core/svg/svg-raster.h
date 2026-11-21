#ifndef RASTER_INTERNAL_H
#define RASTER_INTERNAL_H

#include "svg-int.h"

#ifndef SVG_MIN
#define SVG_MIN(a,b) ((a) < (b) ? (a) : (b))
#define SVG_MAX(a,b) ((a) > (b) ? (a) : (b))
#endif

#if defined(_MSC_VER) && !defined(__clang__)
#define SVG_THREADLOCAL __declspec(thread)
#else
#define SVG_THREADLOCAL _Thread_local
#endif

int svg_f2i(float v, int lo, int hi, int nan_val);

static inline int svg_f2i_clamped(float v, int lo, int hi) {
    float c = fminf(fmaxf(v, (float)lo), (float)hi);
    return (int)c;
}

void*  canvas_scratch_alloc(Canvas* c, size_t n);
size_t canvas_scratch_mark(const Canvas* c);
void   canvas_scratch_reset(Canvas* c, size_t mark);

void blend(Canvas* c, int x, int y, Color s, float a);
void blend_row_var(Canvas* c, int x0, int y, int n, const Color* restrict col,
                   const float* restrict cov, float k);
void blend_row_const(Canvas* c, int x0, int y, int n, Color s, const float* alpha);
Canvas* canvas_create_rows(int w, int h, int ry0, int ry1);
void composite_layer(Canvas* c, int dx, int dy, const float* src,
                     int lw, int lh, int sy0, int sh, float op);

Mask* mask_alloc(int x0, int y0, int x1, int y1);
void  mask_free(Mask* m);
float mask_get(const Mask* m, int x, int y);

const float* mask_row(const Mask* m, int y, int* x0, int* x1);
Mask* mask_shift(const Mask* m, int x0, int y0, int x1, int y1);
Mask* mask_translate(const Mask* m, int dx, int dy);
Mask* mask_intersect(const Mask* a, const Mask* b);

#define GRAD_LUT_N 1024
#define GRAD_LUT_MASK (GRAD_LUT_N - 1)

typedef struct {
    int type;
    int spread;
    int normalized;
    float bx0, by0, bw, bh, bw_inv, bh_inv;
    float x1, y1, dx, dy, invlen2;
    float cx, cy, r, fx, fy;
    float rdx, rdy, rlen, r_inv;
    int   has_inv;
    Mat   inv;
    Color lut[GRAD_LUT_N];
} Grad;

void  grad_build(Grad* g, const Paint* p, const float bbox[4], Mat world);
Color grad_eval(const Grad* g, float x, float y);
void  grad_eval_row(const Grad* g, int x0, int n, float y, Color* restrict out);

typedef struct {
    int   ok;
    int   w, h;
    float* px;
    float ox, oy;
    float tw, th;
    float scale;
    Mat   inv;
    Mat   inv_pt;
    int   has_inv;
    int   use_pt;
} Pattern;

int   pattern_build(Pattern* out, const void* patnode, const float bbox[4],
                    Mat world, int aa);
Color pattern_eval(const Pattern* p, float x, float y);
void  pattern_eval_row(const Pattern* p, int x0, int n, float y, Color* restrict out);
void  pattern_free(Pattern* p);
void  render_subtree(Canvas* c, const Node* n, Mat world, int aa);
void  render_children(Canvas* c, const Node* n, Mat world, float opacity,
                      const Mask* clip, int aa);
void  render_shape_into(Canvas* c, const Node* n, Mat world, int aa,
                        int want_stroke);
void  render_one(Canvas* c, const Node* n, Mat world, int aa);

void fill_path(Canvas* c, const Path* path, int rule, const Paint* paint,
               float opacity, const Mask* clip, const float bounds[4],
               const float grad_bbox[4], int aa, Mat world);
Path* build_stroke(const Path* path, float w, int cap, int join,
                   float miterlimit, const float* dash, int ndash, float dashoff);

void render_shape(Canvas* c, const Node* n, Mat world, float opacity,
                  const Mask* clip, int aa);
void render_image(Canvas* c, const Node* n, Mat world, float opacity,
                  const Mask* clip, int aa);
void render_text(Canvas* c, const Node* n, Mat world, float opacity,
                 const Mask* clip, int aa);

Mask* make_clip_mask(const Node* clipdef, Mat world, const float* obj_bbox,
                     int W, int H, int wy0, int wy1, int aa);
Mask* make_rect_mask(const Node* n, Mat w, int W, int H, int aa);
int rect_mask_covers_canvas(const Node* n, Mat w, int W, int H);
Mask* make_mask_from_node(const Node* mn, Mat world, const float* bbox,
                          int W, int H, int wy0, int wy1, int aa);
int node_bbox_world(const Node* n, Mat full_w, float out[4]);

typedef struct { int w, h; float* px; } FBuf;

FBuf* fbuf_create(int w, int h);
void  fbuf_free(FBuf* b);
FBuf* fbuf_copy(const FBuf* b);

typedef struct NamedBuf {
    char* name;
    FBuf  fb;
    int   sx, sy, sw, sh;
} NamedBuf;

typedef struct FilterCtx {
    FBuf  src;
    FBuf  alpha;
    NamedBuf* bufs;
    int   n, cap;
    int   last;
    const float* bbox;
    int   has_bbox;
    float bw, bh;
    int   prim_units_bbox;
    int   linear;
    Mat   world;
    int   aa;
    size_t bytes, max_bytes;
    int   failed;

    int   last_sx, last_sy, last_sw, last_sh;
    int   ox, oy;
    const Node* src_node;
    FBuf  transparent;
    int   has_transparent;
    FBuf  fill_paint, stroke_paint;
    int   has_fill_paint, has_stroke_paint;
} FilterCtx;

FBuf* filter_execute(const Node* fdef, const Node* src_node, const FBuf* source,
                     const float bbox[4], int has_bbox, Mat world, int aa,
                     size_t max_bytes, int origin_x, int origin_y);
FBuf* fe_run(FilterCtx* fc, const FilterPrim* p);
FBuf* fe_run_generic(FilterCtx* fc, const FilterPrim* p);
size_t fb_bytes(int w, int h);
void  fe_release(FilterCtx* fc, FBuf* b);

void  image_blit_rgba8(Canvas* dst, int dx, int dy, int dw, int dh,
                       const unsigned char* src, int sw, int sh);
void  image_blit_fbuf(FBuf* dst, int dx, int dy, int dw, int dh,
                      const unsigned char* src, int sw, int sh);

const FBuf* fe_input(FilterCtx* fc, const char* name);
const NamedBuf* fe_lookup(FilterCtx* fc, const char* name);
FBuf* fe_new(FilterCtx* fc, int w, int h);
float fe_len(FilterCtx* fc, float v, int axis);
int   fe_chan(const FBuf* b, int x, int y, int c);
void  fe_premul(const FBuf* in, float* out);
void  fe_unpremul(const float* in, FBuf* out);
void  fe_srgb_to_linear(FBuf* b);
void  fe_linear_to_srgb(FBuf* b);
Color fe_tolinear(Color c);
Color fe_tosrgb(Color c);

#endif
