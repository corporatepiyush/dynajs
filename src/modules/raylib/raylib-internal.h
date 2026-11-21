#ifndef RAYLIB_INTERNAL_H
#define RAYLIB_INTERNAL_H

#include "cutils.h"
#include "dyna-nat.h"
#include "dyna-libc.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_RAYLIB)

#include <raylib.h>
#include <rlgl.h>
#include <math.h>
#include <pthread.h>
#include <string.h>
#define RL_MAX_DIM 16384
#define RL_MAX_PIXELS (1 << 26)
#define RL_COORD_MAX 1e7

enum { RL_TEX,
    RL_RT,
    RL_FONT };

enum { RL_SOUND,
    RL_MUSIC,
    RL_STREAM };

typedef struct rl_gpu rl_gpu;

struct rl_gpu {
    int kind;
    int alive;
    int pending_free;
    Texture2D tex;
    RenderTexture2D rt;
    Font font;
    rl_gpu* next;
    rl_gpu* prev;
    JSRuntime* jrt;
    JSValue win_obj;
};

typedef struct rl_snd rl_snd;

struct rl_snd {
    int kind;
    int alive;
    int channels;
    Sound sound;
    Music music;
    AudioStream stream;
    rl_snd* next;
    rl_snd* prev;
};

typedef struct {
    int open;
    int closing;
    int depth;
    int in_frame;
    int in_texture;
    int in_mode2d;
    int in_mode3d;
    int in_blend;
    int blend_mode;
    int in_scissor;
    rl_gpu* active_rt;
    rl_gpu* gpu;
    rl_snd* snd;
    JSValue win_obj;
} dyn_rl_state_t;

extern dyn_rl_state_t rl;

extern JSClassID rl_win_cid;

extern JSClassID rl_tex_cid;

extern JSClassID rl_rt_cid;

extern JSClassID rl_img_cid;

extern JSClassID rl_font_cid;

extern JSClassID rl_sound_cid;

extern JSClassID rl_music_cid;

extern JSClassID rl_stream_cid;

extern JSClassID rl_f32_cid;

int rl_on_main(JSContext* ctx, const char* what);

static inline int rl_ready(JSContext* ctx, const char* what)
{
    if (!rl_on_main(ctx, what))
        return 0;
    if (!rl.open || rl.closing) {
        JS_ThrowTypeError(ctx, "dyna:raylib: %s needs an open Window", what);
        return 0;
    }
    return 1;
}

int rl_audio_ready(JSContext* ctx, const char* what);

static inline int rl_num(JSContext* ctx, JSValueConst v, const char* what, float* out)
{
    double d;
    if (JS_ToFloat64(ctx, &d, v))
        return -1;
    if (!isfinite(d) || fabs(d) > RL_COORD_MAX) {
        JS_ThrowRangeError(ctx, "dyna:raylib: %s must be a finite number within +-1e7", what);
        return -1;
    }
    *out = (float)d;
    return 0;
}

static inline int rl_num_in(JSContext* ctx, JSValueConst v, const char* what, double lo, double hi, float* out)
{
    double d;
    if (JS_ToFloat64(ctx, &d, v))
        return -1;
    if (!(d >= lo && d <= hi)) {
        JS_ThrowRangeError(ctx, "dyna:raylib: %s must be in %g..%g", what, lo, hi);
        return -1;
    }
    *out = (float)d;
    return 0;
}

static inline int rl_int_in(JSContext* ctx, JSValueConst v, const char* what, int lo, int hi, int* out)
{
    double d;
    if (JS_ToFloat64(ctx, &d, v))
        return -1;
    if (!(d >= lo && d <= hi) || d != floor(d)) {
        JS_ThrowRangeError(ctx, "dyna:raylib: %s must be an integer in %d..%d", what, lo, hi);
        return -1;
    }
    *out = (int)d;
    return 0;
}

int rl_color(JSContext* ctx, JSValueConst v, const char* what, Color* c);

static inline int rl_color_opt(JSContext* ctx, int argc, JSValueConst* argv, int i, const char* what, Color* c)
{
    if (argc <= i || JS_IsUndefined(argv[i])) {
        *c = WHITE;
        return 0;
    }
    return rl_color(ctx, argv[i], what, c);
}

static inline int rl_args(JSContext* ctx, int argc, JSValueConst* argv, int n, const char* usage)
{
    if (argc < n) {
        JS_ThrowTypeError(ctx, "dyna:raylib: %s", usage);
        return -1;
    }
    return 0;
}

static inline int rl_nums(JSContext* ctx, JSValueConst* argv, int n, const char* what, float* out)
{
    int i;
    for (i = 0; i < n; i++)
        if (rl_num(ctx, argv[i], what, &out[i]))
            return -1;
    return 0;
}

int rl_field(JSContext* ctx, JSValueConst obj, const char* key, const char* what, int need, float dflt, float* out);

static inline int rl_rect(JSContext* ctx, JSValueConst v, const char* what, Rectangle* r)
{
    if (!JS_IsObject(v)) {
        JS_ThrowTypeError(ctx, "dyna:raylib: %s must be { x, y, width, height }", what);
        return -1;
    }
    if (rl_field(ctx, v, "x", what, 1, 0, &r->x) || rl_field(ctx, v, "y", what, 1, 0, &r->y)
        || rl_field(ctx, v, "width", what, 1, 0, &r->width) || rl_field(ctx, v, "height", what, 1, 0, &r->height))
        return -1;
    return 0;
}

int rl_path(JSContext* ctx, JSValueConst v, const char* who, const char** out);

static inline JSValue rl_xy(JSContext* ctx, double x, double y, const char* kx, const char* ky)
{
    JSValue o = JS_NewObject(ctx);
    if (JS_IsException(o))
        return o;
    if (JS_DefinePropertyValueStr(ctx, o, kx, JS_NewFloat64(ctx, x), JS_PROP_C_W_E) < 0
        || JS_DefinePropertyValueStr(ctx, o, ky, JS_NewFloat64(ctx, y), JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, o);
        return JS_EXCEPTION;
    }
    return o;
}

static inline int rl_sync_result(JSContext* ctx, JSValue r, const char* who)
{
    if (JS_IsException(r))
        return -1;
    if ((int)JS_PromiseState(ctx, r) >= 0) {
        JS_FreeValue(ctx, r);
        JS_ThrowTypeError(ctx, "dyna:raylib: %s: the callback must be synchronous (an async function returns before it draws)", who);
        return -1;
    }
    JS_FreeValue(ctx, r);
    return 0;
}

void rl_gpu_link(rl_gpu* g);

void rl_gpu_unload(rl_gpu* g);

void rl_close_now(void);

static inline void rl_after_scope(void)
{
    if (rl.depth == 0 && rl.closing)
        rl_close_now();
}

void rl_gpu_dispose(void* p);

void rl_gpu_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark);

rl_gpu* rl_gpu_of(JSContext* ctx, JSValueConst v, JSClassID cid);

rl_snd* rl_snd_of(JSContext* ctx, JSValueConst v, JSClassID cid);

static inline JSValue rl_wrap_gpu(JSContext* ctx, JSClassID cid, rl_gpu* g)
{
    g->jrt = JS_GetRuntime(ctx);
    g->win_obj = JS_DupValue(ctx, rl.win_obj);
    g->alive = 1;
    rl_gpu_link(g);
    return dyn_res_wrap(ctx, JS_UNDEFINED, cid, g, rl_gpu_dispose);
}

rl_gpu* rl_gpu_new(JSContext* ctx, int kind);

JSValue rl_window_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv);

JSValue rl_win_get(JSContext* ctx, JSValueConst this_val, int magic);

JSValue rl_win_set_title(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

JSValue rl_win_set2(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic);

JSValue rl_win_set1(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic);

JSValue rl_win_frame(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

int rl_camera2d(JSContext* ctx, JSValueConst v, Camera2D* c);

int rl_camera3d(JSContext* ctx, JSValueConst v, Camera3D* c);

JSValue rl_win_mode(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic);

JSValue rl_draw2d(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic);

JSValue rl_draw_grid(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

JSValue rl_draw_fps(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

JSValue rl_text(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic);

JSValue rl_draw_texture(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

JSValue rl_draw_texture_pro(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

static inline int rl_dims(JSContext* ctx, JSValueConst wv, JSValueConst hv, const char* who, int* w, int* h)
{
    if (rl_int_in(ctx, wv, "width", 1, RL_MAX_DIM, w) || rl_int_in(ctx, hv, "height", 1, RL_MAX_DIM, h))
        return -1;
    if ((int64_t)*w * *h > RL_MAX_PIXELS) {
        JS_ThrowRangeError(ctx, "dyna:raylib: %s: %dx%d is over the %d-pixel limit", who, *w, *h, RL_MAX_PIXELS);
        return -1;
    }
    return 0;
}

static inline JSValue rl_texture_from_image(JSContext* ctx, Image* im)
{
    rl_gpu* g = rl_gpu_new(ctx, RL_TEX);
    if (!g)
        return JS_EXCEPTION;
    g->tex = LoadTextureFromImage(*im);
    if (!IsTextureValid(g->tex)) {
        dyn_nat_free(g);
        return JS_ThrowInternalError(ctx, "dyna:raylib: the GPU refused a %dx%d texture", im->width, im->height);
    }
    return rl_wrap_gpu(ctx, rl_tex_cid, g);
}

JSValue rl_gpu_size(JSContext* ctx, JSValueConst this_val, int magic);

JSValue rl_tex_update(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

JSValue rl_gpu_filter(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

JSValue rl_rt_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv);

JSValue rl_rt_draw(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

JSValue rl_rt_read(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

JSValue rl_image_wrap(JSContext* ctx, JSValueConst new_target, Image im);

JSValue rl_image_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv);

JSValue rl_img_get(JSContext* ctx, JSValueConst this_val, int magic);

JSValue rl_img_pixels(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

JSValue rl_img_export(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

JSValue rl_img_op(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic);

JSValue rl_img_copy(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

JSValue rl_img_to_texture(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

JSValue rl_input0(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic);

JSValue rl_gamepad(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic);

JSValue rl_audio_fn(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic);

JSValue rl_snd_wrap(JSContext* ctx, JSClassID cid, rl_snd* s);

JSValue rl_play(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic);

JSValue rl_snd_get(JSContext* ctx, JSValueConst this_val, int magic);

JSValue rl_snd_set(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic);

JSValue rl_stream_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv);

JSValue rl_stream_update(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

int rl_name_index(JSContext* ctx, JSValueConst v, const char* what, const char* const* names, int n, int* out);

int rl_text_arg(JSContext* ctx, JSValueConst v, const char* who, const char** out);

JSValue rl_win_get2(JSContext* ctx, JSValueConst this_val, int magic);

JSValue rl_win_act(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic);

JSValue rl_win_flag(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic);

JSValue rl_win_set_opacity(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

JSValue rl_win_limit(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic);

JSValue rl_win_scope(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic);

JSValue rl_draw_shape(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic);

JSValue rl_draw_points(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic);

JSValue rl_img_draw(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic);

JSValue rl_img_draw_image(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

JSValue rl_img_draw_text(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

#endif

#endif
