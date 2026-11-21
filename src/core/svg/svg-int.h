#ifndef SVG2IMG_INTERNAL_H
#define SVG2IMG_INTERNAL_H

#include "../dyn-svg.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#define SVG2IMG_NEON 1
#elif defined(__SSE2__)
#include <emmintrin.h>
#define SVG2IMG_SSE2 1
#endif

#ifndef SVG_PI
#define SVG_PI 3.14159265358979323846f
#endif

#define SVG2IMG_LIMIT_MAX_NODES          200000
#define SVG2IMG_LIMIT_MAX_DEPTH          1024
#define SVG2IMG_LIMIT_MAX_PATH_POINTS    500000
#define SVG2IMG_LIMIT_MAX_PATH_SUBPATHS  100000
#define SVG2IMG_LIMIT_MAX_GRADIENT_STOPS 256
#define SVG2IMG_LIMIT_MAX_TEXT_LENGTH    100000
#define SVG2IMG_LIMIT_MAX_CLIP_DEPTH     16
#define SVG2IMG_LIMIT_MAX_LAYERS         64
#define SVG2IMG_LIMIT_MAX_USE_EXPANSION  20000
#define SVG2IMG_LIMIT_MAX_PIXELS         (64 * 1024 * 1024)

#if defined(SVG2IMG_ALLOC_TRACE)

void* svg_trace_malloc(size_t n, const char* file, int line);
void* svg_trace_realloc(void* p, size_t n, const char* file, int line);
void* svg_trace_calloc(size_t n, const char* file, int line);
void  svg_trace_free(void* p, const char* file, int line);
void  svg_trace_mark(const char* name);
#define svg_malloc(n)      svg_trace_malloc((n), __FILE__, __LINE__)
#define svg_realloc(p, n)  svg_trace_realloc((p), (n), __FILE__, __LINE__)
#define svg_calloc(n)      svg_trace_calloc((n), __FILE__, __LINE__)
#define svg_mfree(p)       svg_trace_free((p), __FILE__, __LINE__)
#elif defined(SVG2IMG_MIMALLOC)
#include <mimalloc.h>
static inline void* svg_malloc(size_t n) { return mi_malloc(n ? n : 1); }
static inline void* svg_realloc(void* p, size_t n) { return mi_realloc(p, n ? n : 1); }
static inline void* svg_calloc(size_t n) { return mi_calloc(1, n ? n : 1); }
static inline void  svg_mfree(void* p) { mi_free(p); }
#elif defined(SVG2IMG_SLAB)

void* svg_malloc(size_t n);
void* svg_realloc(void* p, size_t n);
void* svg_calloc(size_t n);

void  svg_mfree_slow(void* p);
static inline void svg_mfree(void* p) { if (p) svg_mfree_slow(p); }

void  svg_doc_arena_begin(void);
void  svg_doc_arena_pause(void);
void  svg_doc_arena_reset(void);
#elif defined(SVG2IMG_ALLOC_HOOKS)
typedef void* (*svg_alloc_fn)(size_t);
typedef void* (*svg_realloc_fn)(void*, size_t);
typedef void  (*svg_free_fn)(void*);
void  svg_set_alloc_hooks(svg_alloc_fn alloc, svg_realloc_fn realloc_fn,
                          svg_free_fn free_fn);
void* svg_malloc(size_t n);
void* svg_realloc(void* p, size_t n);
void* svg_calloc(size_t n);
void  svg_mfree(void* p);
#else
static inline void* svg_malloc(size_t n) { return malloc(n ? n : 1); }
static inline void* svg_realloc(void* p, size_t n) { return realloc(p, n ? n : 1); }
static inline void* svg_calloc(size_t n) { return calloc(1, n ? n : 1); }
static inline void  svg_mfree(void* p) { if (p) free(p); }
#endif

void  svg_doc_arena_begin(void);
void  svg_doc_arena_pause(void);
void  svg_doc_arena_reset(void);

void  svg_parallel_run(int n, void (*fn)(void* arg, int worker), void* arg);
int   svg_cpu_count(void);
void  svg_alloc_worker(int on);
size_t svg_scratch_mark(void);
void*  svg_scratch_alloc(size_t n);
void*  svg_scratch_try(size_t n);
void   svg_scratch_release(size_t mark);

static inline char* svg_strdup(const char* s) {
    if (!s) return NULL;
    size_t n = strlen(s);
    char* r = (char*)svg_malloc(n + 1);
    if (r) memcpy(r, s, n + 1);
    return r;
}

static inline char* svg_strndup(const char* s, size_t n) {
    char* r = (char*)svg_malloc(n + 1);
    if (r) { memcpy(r, s, n); r[n] = 0; }
    return r;
}

#if defined(SVG2IMG_ALLOC_TRACE)
char* svg_trace_strdup(const char* s, const char* file, int line);
char* svg_trace_strndup(const char* s, size_t n, const char* file, int line);
#define svg_strdup(s)     svg_trace_strdup((s), __FILE__, __LINE__)
#define svg_strndup(s, n) svg_trace_strndup((s), (n), __FILE__, __LINE__)
#endif

typedef struct { float x, y; } Pt;

typedef struct { float m[6]; } Mat;

static inline Mat mat_identity(void) { Mat m = {{1, 0, 0, 1, 0, 0}}; return m; }

static inline Mat mat_mul(Mat a, Mat b) {
    Mat r;
    r.m[0] = a.m[0] * b.m[0] + a.m[2] * b.m[1];
    r.m[1] = a.m[1] * b.m[0] + a.m[3] * b.m[1];
    r.m[2] = a.m[0] * b.m[2] + a.m[2] * b.m[3];
    r.m[3] = a.m[1] * b.m[2] + a.m[3] * b.m[3];
    r.m[4] = a.m[0] * b.m[4] + a.m[2] * b.m[5] + a.m[4];
    r.m[5] = a.m[1] * b.m[4] + a.m[3] * b.m[5] + a.m[5];
    return r;
}

static inline Pt mat_apply(Mat m, Pt p) {
    Pt r;
    r.x = m.m[0] * p.x + m.m[2] * p.y + m.m[4];
    r.y = m.m[1] * p.x + m.m[3] * p.y + m.m[5];
    return r;
}

static inline Mat mat_translate(float x, float y) { Mat m = {{1, 0, 0, 1, x, y}}; return m; }
static inline Mat mat_scale(float x, float y)     { Mat m = {{x, 0, 0, y, 0, 0}}; return m; }

static inline Mat mat_rotate(float deg) {
    float r = deg * SVG_PI / 180.0f;
    float c = cosf(r), s = sinf(r);
    Mat m = {{c, s, -s, c, 0, 0}};
    return m;
}

static inline Mat mat_skew_x(float deg) {
    float t = tanf(deg * SVG_PI / 180.0f);
    Mat m = {{1, 0, t, 1, 0, 0}};
    return m;
}

static inline float mat_scale_factor(Mat m) {
    return sqrtf(fabsf(m.m[0] * m.m[3] - m.m[1] * m.m[2]));
}

static inline int mat_invert_checked(Mat m, Mat* out) {
    float det = m.m[0] * m.m[3] - m.m[1] * m.m[2];
    if (!isfinite(det) || fabsf(det) < 1e-20f) return 0;
    float id = 1.0f / det;
    Mat r;
    r.m[0] =  m.m[3] * id;
    r.m[1] = -m.m[1] * id;
    r.m[2] = -m.m[2] * id;
    r.m[3] =  m.m[0] * id;
    r.m[4] = (m.m[2] * m.m[5] - m.m[3] * m.m[4]) * id;
    r.m[5] = (m.m[1] * m.m[4] - m.m[0] * m.m[5]) * id;
    for (int i = 0; i < 6; i++)
        if (!isfinite(r.m[i])) return 0;
    if (out) *out = r;
    return 1;
}

static inline Mat mat_invert(Mat m) {
    Mat r = mat_identity();
    (void)mat_invert_checked(m, &r);
    return r;
}

typedef struct { Pt* p; int n; int closed; } SubPath;
typedef struct {
    SubPath* s;
    int n, cap;
    int shared_points;

    void* pts_arena;
    SubPath* s_inline;

} Path;
#define PATH_ONE_BLOCK 2

typedef struct { float r, g, b, a; } Color;
typedef struct { float off; Color c; } Stop;

enum {
    PAINT_NONE = 0,
    PAINT_COLOR = 1,
    PAINT_LINEAR = 2,
    PAINT_RADIAL = 3,
    PAINT_CURRENT_COLOR = 4,
    PAINT_REF = 5,
    PAINT_PATTERN = 6
};

enum { UNITS_BBOX = 0, UNITS_USER = 1 };
enum { SPREAD_PAD = 0, SPREAD_REFLECT = 1, SPREAD_REPEAT = 2 };

enum {
    SPEC_NONE         = 0u,
    SPEC_FILL         = 1u << 0,
    SPEC_STROKE       = 1u << 1,
    SPEC_FONT_SIZE    = 1u << 2,
    SPEC_FONT_WEIGHT  = 1u << 3,
    SPEC_FONT_STYLE   = 1u << 4,
    SPEC_TEXT_ANCHOR  = 1u << 5,
    SPEC_LETTER_SP    = 1u << 6,
    SPEC_VISIBILITY   = 1u << 7,
    SPEC_STROKE_WIDTH = 1u << 8,
    SPEC_LINECAP      = 1u << 9,
    SPEC_LINEJOIN     = 1u << 10,
    SPEC_MITERLIMIT   = 1u << 11,
    SPEC_FILL_RULE    = 1u << 12,
    SPEC_CLIP_RULE    = 1u << 13,
    SPEC_FILL_OPACITY = 1u << 14,
    SPEC_STROKE_OPACITY = 1u << 15,
    SPEC_DASH         = 1u << 16,
    SPEC_DASH_OFFSET  = 1u << 17
};

typedef struct Paint {
    int    type;
    Color  color;
    int    units;
    float  x1, y1, x2, y2;
    float  cx, cy, r, fx, fy;
    Stop*  stops;
    int    nstops;
    int    spread;
    Mat    tm;
    char*  ref;
    void*  pat;
    int    linear;
    int    fb_type;

    Color  fb_color;
} Paint;

typedef struct FontFace FontFace;

typedef struct {
    char*  text;
    float  x, y;
    float  fontsize;
    float  letterspacing;
    int    bold, italic, anchor;
    Paint  fill;

    Paint  stroke;
    float  stroke_width;
    int    linecap, linejoin;
    float  miterlimit;
    float* dash;
    int    ndash;
    float  dashoff;

    float  textlen;
    int    length_kind;
    int    writing;
    float  baseline;
    float* rot;
    int    nrot;

    float* gx, * gy, * gdx, * gdy;
    int    ngpos;
    const FontFace* font;
    char*  path;
    Path*  refpath;
    char*  path_ref;
    float  start_offset;
    int    start_is_pct;
} TextChunk;

typedef struct Node Node;
typedef struct Canvas Canvas;
typedef struct Mask Mask;

enum {
    FE_BLEND = 0, FE_COLOR_MATRIX, FE_COMPONENT_TRANSFER, FE_COMPOSITE,
    FE_CONVOLVE_MATRIX, FE_DIFFUSE_LIGHTING, FE_DISPLACEMENT_MAP,
    FE_DISTANT_LIGHT, FE_FLOOD, FE_GAUSSIAN_BLUR, FE_IMAGE, FE_MERGE,
    FE_MERGE_NODE, FE_MORPHOLOGY, FE_OFFSET, FE_POINT_LIGHT,
    FE_SPECULAR_LIGHTING, FE_SPOT_LIGHT, FE_TILE, FE_TURBULENCE,
    FE_FUNC_R, FE_FUNC_G, FE_FUNC_B, FE_FUNC_A
};

enum { FE_CM_MATRIX = 0, FE_CM_SATURATE, FE_CM_HUE_ROTATE,
       FE_CM_LUMINANCE_TO_ALPHA };
enum { FE_CT_IDENTITY = 0, FE_CT_TABLE, FE_CT_DISCRETE, FE_CT_LINEAR,
       FE_CT_GAMMA };
enum { FE_COMP_OVER = 0, FE_COMP_IN, FE_COMP_OUT, FE_COMP_ATOP, FE_COMP_XOR,
       FE_COMP_ARITH };
enum { FE_BLEND_NORMAL = 0, FE_BLEND_MULTIPLY, FE_BLEND_SCREEN,
       FE_BLEND_DARKEN, FE_BLEND_LIGHTEN };
enum { FE_TURB_FRACTAL = 0, FE_TURB_TURBULENCE };
enum { FE_LIGHT_DISTANT = 0, FE_LIGHT_POINT, FE_LIGHT_SPOT };
enum { FE_CHAN_R = 0, FE_CHAN_G, FE_CHAN_B, FE_CHAN_A };

typedef struct FilterPrim {
    int   type;
    char* in;
    char* in2;
    char* result;

    float num[8];

    float sub[4];
    int   sub_mask;
    float values[32];
    int   nvalues;
    float table[16];
    int   ntable;

    int   ct_type[4];
    float ct_slope[4], ct_intercept[4], ct_amplitude[4], ct_exponent[4],
          ct_offset[4];
    float ct_table[4][16];
    int   ct_ntable[4];

    int   order_x, order_y, edge_mode;
    int   preserve_alpha;
    float target_x, target_y;
    float divisor, bias;
    float kernel[64];
    int   nkernel;

    Color light_color;
    float surface_scale, diffuse_constant, specular_constant, specular_exponent;
    int   light_type;
    float lx, ly, lz;
    float azimuth, elevation;
    float points_at_x, points_at_y, points_at_z;
    float limiting_cone_angle;

    int   turb_type, stitch;
    float base_freq_x, base_freq_y;
    int   octaves, seed;

    char* href;
    Node* ref_node;
    unsigned char* px;
    int   pw, ph;

    char* merge_in[8];
    int   nmerge;
} FilterPrim;

enum {
    PAT_SPEC_UNITS        = 1u << 0,
    PAT_SPEC_CONTENT_UNITS = 1u << 1,
    PAT_SPEC_TM           = 1u << 2,
    PAT_SPEC_X            = 1u << 3,
    PAT_SPEC_Y            = 1u << 4,
    PAT_SPEC_W            = 1u << 5,
    PAT_SPEC_H            = 1u << 6,
    PAT_SPEC_VB           = 1u << 7,
    PAT_SPEC_PAR          = 1u << 8
};

enum {
    GRAD_SPEC_UNITS  = 1u << 0,
    GRAD_SPEC_SPREAD = 1u << 1,
    GRAD_SPEC_TM     = 1u << 2,
    GRAD_SPEC_CI     = 1u << 3,
    GRAD_SPEC_X1     = 1u << 4,
    GRAD_SPEC_Y1     = 1u << 5,
    GRAD_SPEC_X2     = 1u << 6,
    GRAD_SPEC_Y2     = 1u << 7,
    GRAD_SPEC_CX     = 1u << 8,
    GRAD_SPEC_CY     = 1u << 9,
    GRAD_SPEC_R      = 1u << 10,
    GRAD_SPEC_FX     = 1u << 11,
    GRAD_SPEC_FY     = 1u << 12,
    GRAD_SPEC_STOPS  = 1u << 13
};

enum {
    NODE_GROUP = 0,
    NODE_SHAPE = 1,
    NODE_TEXT = 2,
    NODE_CLIPDEF = 3,
    NODE_GRADDEF = 4,
    NODE_IGNORE = 5,
    NODE_SYMBOL = 6,
    NODE_TRANSPARENT = 7,

    NODE_PATTERNDEF = 8,
    NODE_MASKDEF = 9,
    NODE_MARKERDEF = 10,
    NODE_IMAGE = 11,
    NODE_FILTERDEF = 12
};

struct Node {
    int   type;
    char* id;
    Mat   tm;
    float opacity;
    Node** kids;
    int   nkids;
    int   capkids;

    unsigned ext_gen;
    int   ext_ok;
    float ext_y0, ext_y1;
    int   ext_fidx;

    Path* path;
    int   marker_ok;
    int   crisp;
    Paint fill, stroke;
    float sw;
    int   linecap;
    int   linejoin;
    float miterlimit;
    float* dash;
    int   ndash;
    float dashoff;
    int   fillrule;
    float fillop, strokeop;
    int   visible;

    char* text;
    TextChunk* chunks;
    int nchunks;
    int capchunks;
    float fontsize;
    float letterspacing;
    int   bold, italic;
    int   anchor;
    float vw, vh;
    float x, y;

    Node* clip;
    char* clip_ref;
    int   clip_units;
    int   clip_rule;

    int hidden_subtree;

    char* use_ref;
    float use_x, use_y;
    float use_w, use_h;

    unsigned spec;

    unsigned grad_spec;
    unsigned grad_pct;
    float    grad_vw, grad_vh;

    struct { int w, h; Mat tm; int valid; } vp;

    int   vp_clip;
    float vp_x, vp_y, vp_w, vp_h;
    Mat   vp_matrix;
    int   vp_matrix_valid;

    float use_vb[4]; int use_vb_valid;
    int   use_par_none, use_par_slice, use_par_xa, use_par_ya;
    float use_vp_x, use_vp_y;
    float use_vp_w, use_vp_h;
    int   use_vp_has_size;
    float symbol_vb[4];
    int   symbol_vb_valid;

    float symbol_w, symbol_h;
    int   symbol_has_w, symbol_has_h;
    int   symbol_par_none, symbol_par_slice, symbol_par_xa, symbol_par_ya;

    char*  image_href;
    float  img_x, img_y, img_w, img_h;
    int    img_par_none, img_par_slice, img_par_xa, img_par_ya;
    unsigned char* img_px;
    int    img_iw, img_ih;
    const FontFace* font;
    FontFace* font_owned;

    Node* marker_start, * marker_mid, * marker_end;
    char* marker_start_ref, * marker_mid_ref, * marker_end_ref;
    int   mk_units;
    float mk_w, mk_h, mk_refx, mk_refy;
    int   mk_orient;
    float mk_angle;
    float mk_vb[4]; int mk_vb_valid;
    int   mk_par_none, mk_par_slice, mk_par_xa, mk_par_ya;

    Node* mask;
    char* mask_ref;
    int   mask_units;
    int   mask_content_units;
    float mask_x, mask_y, mask_w, mask_h;

    int   pat_units;
    int   pat_content_units;
    float pat_x, pat_y, pat_w, pat_h;
    Mat   pat_tm;
    float pat_vb[4]; int pat_vb_valid;
    int   pat_par_none, pat_par_slice, pat_par_xa, pat_par_ya;

    unsigned pat_spec, pat_pct;
    float pat_vw, pat_vh;
    Node* pat_content;

    Node* filter;
    char* filter_ref;
    int   filt_units;
    int   filt_prim_units;
    float filt_x, filt_y, filt_w, filt_h;
    int   filt_linear;
    FilterPrim* prims;
    int   nprims;
    char* filt_href;
};

struct Canvas {
    int w, h;
    float* px;

    unsigned char* scratch;
    size_t scratch_cap, scratch_used;
    int scratch_depth;

    int wy0, wy1;

    int py0, ph;
};

static inline float* canvas_at(const Canvas* c, int x, int y) {
    return c->px + ((size_t)(y - c->py0) * (size_t)c->w + (size_t)x) * 4;
}
struct Mask   { int x0, y0, x1, y1; float* m; int empty; };

typedef struct XMLNode {
    char*  name;
    char** attrs;
    int    nattrs, capattrs;
    struct XMLNode** kids;
    int    nkids, capkids;
    char*  text;
    struct XMLNode* parent;
} XMLNode;

XMLNode*    svg_xml_parse(const char* data, size_t len);
void        xml_free(XMLNode* n);
const char* xml_attr(const XMLNode* n, const char* name);
XMLNode*    xml_child(const XMLNode* n, const char* name);

Path* path_parse(const char* d);
Path* path_parse_lim(const char* d, const svg2img_limits* lim, int* limit_hit);
void  path_free(Path* p);
void  path_transform(Path* p, Mat m);
void  path_bbox(const Path* p, float out[4]);
Path* path_copy(const Path* p);
Path* path_copy_xform(const Path* p, const Mat* m);
size_t path_block_size(const Path* p);
Path* path_copy_xform_at(const Path* p, const Mat* m, void* mem);
void  path_bbox_xform(const Path* p, Mat m, float out[4]);

typedef struct Style {
    Paint fill, stroke;
    float stroke_width;
    int   linecap, linejoin;
    float miterlimit;
    int   fillrule;
    int   cliprule;
    float fillop, strokeop;
    Color color;

    Color stop_color;
    float stop_opacity;

    Color flood_color;
    float flood_opacity;
    Color lighting_color;
    float fontsize;
    int   bold, italic;
    int   anchor;
    float letterspacing;
    int   display_none;
    int   visibility_hidden;
    unsigned spec;
    float dashoff;
    float* dash;
    int   ndash;
    int   saw_fill;
    int   saw_stroke;
    char* marker_start;
    char* marker_mid;
    char* marker_end;
    char* filter;
    int   crisp;
    float textlen;
    int   length_kind;
    int   writing;
    float baseline;
    float* rot;
    int   nrot;
} Style;

typedef struct IdMap IdMap;

typedef struct CssRule CssRule;
typedef struct Ctx {
    float       vw, vh, em;
    IdMap*      ids;
    const svg2img_limits* lim;
    int         nodes;
    int         limit_hit;
    CssRule*    rules;
    int         nrules, caprules;
    const char* base_path;
    int         aa;
    const FontFace* font;

    const Style* inherit_from;

    const XMLNode* css_last;
    struct CssIndex* css_index;
    int         css_nlast;
    int         css_last_idx[128];

    int         has_root_view;
    Mat         root_view;
    int         root_w, root_h;
} Ctx;

Style  style_default(void);
void   style_copy(Style* dst, const Style* src);
void   style_free(Style* s);
void   style_apply_decls(Ctx* ctx, Style* st, const char* decls);

int    style_is_presentation_attr(const char* name);
int    style_get_decl(const char* decls, const char* key, char* out, size_t cap);

int   color_parse(const char* s, Color* out);
float length_parse(const char* s, float vw, float vh, float em, int axis, float def);
float path_length(const Path* p);

typedef struct { float* xy; int n, cap; int* cont; int nc, ccap; } PtList;
FontFace* font_face_load(const char* path);
void      font_face_free(FontFace* f);
int       font_face_glyph_index(const FontFace* f, unsigned ch);
float     font_face_advance(const FontFace* f, int gid);
int       font_face_contours(const FontFace* f, int gid, float size,
                             PtList* out, int* ncontours);
float     font_face_metrics(const FontFace* f, float* ascent, float* descent);

unsigned char* png_decode_rgba(const unsigned char* d, size_t n,
                               int* out_w, int* out_h);
unsigned char* jpeg_decode_rgba(const unsigned char* d, size_t n,
                                int* out_w, int* out_h);

#define IMG_LIMIT (-1)
int   image_load_any(const char* href, const char* base_path, int aa,
                     const svg2img_limits* lim,
                     unsigned char** out, int* out_w, int* out_h);

unsigned char* img_buf_retain(unsigned char* px);
void  img_buf_release(unsigned char* px);

void  image_budget_reset(void);

Node* svg_node_new(int type);
int   svg_resolve(Node* n, IdMap* ids, const svg2img_limits* lim);
Node* svg_parse(const char* data, size_t len, const svg2img_options* opts,
                int* limit_hit);
void  svg_free(Node* n);
void  limits_resolve(const svg2img_limits* in, svg2img_limits* out);

Canvas* canvas_create(int w, int h);
void    canvas_free(Canvas* c);
void    canvas_clear(Canvas* c, Color col);
void    render_document(Canvas* c, Node* root, Mat view_tm,
                        const svg2img_options* opts, const svg2img_limits* lim);


int   font_glyph(int ch, unsigned char rows[7]);

#endif
