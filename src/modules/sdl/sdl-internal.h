#ifndef SDL_INTERNAL_H
#define SDL_INTERNAL_H

#include "cutils.h"
#include "dyna-nat.h"
#include "dyna-libc.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_SDL3)

#include <SDL3/SDL.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#define SDL_MAX_DIM 16384

typedef struct sdl_tex sdl_tex;

typedef struct sdl_ren sdl_ren;

typedef struct {
    SDL_Window* w;
    sdl_ren* ren;
} sdl_win;

struct sdl_ren {
    SDL_Renderer* r;
    sdl_win* win;
    sdl_tex* texs;
    JSRuntime* rt;
    JSValue win_obj;
};

struct sdl_tex {
    SDL_Texture* t;
    sdl_ren* ren;
    sdl_tex* next;
    int w, h;
    int target;
    JSRuntime* rt;
    JSValue ren_obj;
};

extern JSClassID sdl_win_cid;

extern JSClassID sdl_ren_cid;

extern JSClassID sdl_tex_cid;

extern JSClassID sdl_audio_cid;

extern JSClassID sdl_pad_cid;

extern JSClassID sdl_f32_cid;

extern JSClassID sdl_i16_cid;

extern JSClassID sdl_i32_cid;

extern _Atomic(int) sdl_live_windows;

extern _Atomic(int) sdl_live_streams;

static inline JSValue sdl_throw(JSContext* ctx, const char* what)
{
    return JS_ThrowInternalError(ctx, "dyna:sdl: %s failed: %s", what, SDL_GetError());
}

int sdl_on_main(JSContext* ctx, const char* what);

static inline int sdl_need_video(JSContext* ctx, const char* what)
{
    if (!sdl_on_main(ctx, what))
        return 0;
    if (!(SDL_WasInit(SDL_INIT_VIDEO) & SDL_INIT_VIDEO)) {
        JS_ThrowTypeError(ctx, "dyna:sdl: %s needs init([\"video\"]) first", what);
        return 0;
    }
    return 1;
}

static inline int sdl_dim(JSContext* ctx, JSValueConst v, const char* what, int* out)
{
    double d;
    if (JS_ToFloat64(ctx, &d, v))
        return -1;
    if (!(d >= 1 && d <= SDL_MAX_DIM) || d != floor(d)) {
        JS_ThrowRangeError(ctx, "dyna:sdl: %s must be an integer in 1..%d", what, SDL_MAX_DIM);
        return -1;
    }
    *out = (int)d;
    return 0;
}

static inline int sdl_coord(JSContext* ctx, JSValueConst v, const char* what, float* out)
{
    double d;
    if (JS_ToFloat64(ctx, &d, v))
        return -1;
    if (!isfinite(d) || fabs(d) > 1e7) {
        JS_ThrowRangeError(ctx, "dyna:sdl: %s must be a finite number within +-1e7", what);
        return -1;
    }
    *out = (float)d;
    return 0;
}

static inline int sdl_int_in(JSContext* ctx, JSValueConst v, const char* what, int lo, int hi, int* out)
{
    double d;
    if (JS_ToFloat64(ctx, &d, v))
        return -1;
    if (!(d >= lo && d <= hi) || d != floor(d)) {
        JS_ThrowRangeError(ctx, "dyna:sdl: %s must be an integer in %d..%d", what, lo, hi);
        return -1;
    }
    *out = (int)d;
    return 0;
}

int sdl_opt_bool(JSContext* ctx, JSValueConst opts, const char* key, int* out);

int sdl_check_opts(JSContext* ctx, JSValueConst opts, const char* who,
    const char* const* keys, int nkeys);

void sdl_ren_kill(sdl_ren* r);

void sdl_ren_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark);

void sdl_tex_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark);

static inline sdl_win* sdl_win_of(JSContext* ctx, JSValueConst v)
{
    sdl_win* w = dyn_res_native(ctx, v, sdl_win_cid);
    if (w && !w->w) {
        JS_ThrowTypeError(ctx, "dyna:sdl: the window is closed");
        return NULL;
    }
    return w;
}

sdl_ren* sdl_ren_of(JSContext* ctx, JSValueConst v);

sdl_tex* sdl_tex_of(JSContext* ctx, JSValueConst v);

int sdl_name_index(JSContext* ctx, JSValueConst v, const char* what, const char* const* names, int n, int* out);

int sdl_text_arg(JSContext* ctx, JSValueConst v, const char* who, const char** out);

JSValue sdl_window_state(JSContext* ctx, JSValueConst this_val);

JSValue sdl_window_act2(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic);

JSValue sdl_window_flag(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic);

JSValue sdl_ren_debug_text(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

JSValue sdl_ren_logical(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

JSValue sdl_ren_coords(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic);

extern _Atomic(int) sdl_live_pads;

static inline int sdl_need_pad(JSContext* ctx, const char* what)
{
    if (!sdl_on_main(ctx, what))
        return 0;
    if (!(SDL_WasInit(SDL_INIT_GAMEPAD) & SDL_INIT_GAMEPAD)) {
        JS_ThrowTypeError(ctx, "dyna:sdl: %s needs init([\"gamepad\"]) first", what);
        return 0;
    }
    return 1;
}

JSValue sdl_virtual_pad(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic);

JSValue sdl_pad_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv);

JSValue sdl_pad_get(JSContext* ctx, JSValueConst this_val, int magic);

JSValue sdl_pad_read(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic);

JSValue sdl_pad_rumble(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

JSValue sdl_pad_set_virtual(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic);

static inline int sdl_set_int(JSContext* ctx, JSValue o, const char* k, double v)
{
    return JS_DefinePropertyValueStr(ctx, o, k, JS_NewFloat64(ctx, v), JS_PROP_C_W_E);
}

JSValue sdl_event_obj(JSContext* ctx, const SDL_Event* e);

int sdl_events_ready(JSContext* ctx, const char* what);

JSValue sdl_push_event(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

JSValue sdl_window_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv);

JSValue sdl_window_get(JSContext* ctx, JSValueConst this_val, int magic);

JSValue sdl_window_set_title(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

JSValue sdl_window_set2(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic);

JSValue sdl_window_act(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic);

JSValue sdl_renderer_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv);

JSValue sdl_ren_color(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

JSValue sdl_ren_simple(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic);

JSValue sdl_ren_draw(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic);

int sdl_rect_arg(JSContext* ctx, JSValueConst v, const char* what, float f[4], int* present);

JSValue sdl_ren_read_pixels(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

JSValue sdl_ren_get(JSContext* ctx, JSValueConst this_val, int magic);

JSValue sdl_ren_draw_texture(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

JSValue sdl_texture_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv);

JSValue sdl_tex_get(JSContext* ctx, JSValueConst this_val, int magic);

JSValue sdl_tex_update(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

int sdl_blend_arg(JSContext* ctx, int argc, JSValueConst* argv, const char* who, SDL_BlendMode* bm);

JSValue sdl_tex_mod(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic);

JSValue sdl_audio_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv);

JSValue sdl_audio_put(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

JSValue sdl_audio_act(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic);

JSValue sdl_audio_queued(JSContext* ctx, JSValueConst this_val);

JSValue sdl_audio_set_gain(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

JSValue sdl_samples_array(JSContext* ctx, const uint8_t* bytes, size_t len, int is_float);

JSValue sdl_window_set_opacity(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

JSValue sdl_window_text(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic);

JSValue sdl_window_text_area(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

JSValue sdl_ren_set_target(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

JSValue sdl_ren_rect_state(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic);

JSValue sdl_ren_set_scale(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

JSValue sdl_ren_set_blend(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

JSValue sdl_ren_set_vsync(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

JSValue sdl_ren_geometry(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

JSValue sdl_load_texture(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

JSValue sdl_audio_get(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

JSValue sdl_audio_flag(JSContext* ctx, JSValueConst this_val, int magic);

JSValue sdl_audio_devices(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

JSValue sdl_load_wav(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

#endif

#endif
