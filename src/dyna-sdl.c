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
#define SDL_MAX_VERTICES (1 << 22)
#define SDL_MAX_GET_SAMPLES (1 << 24)
#define SDL_MAX_WAV_BYTES (1 << 28)

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

typedef struct {
    SDL_AudioStream* s;
    int bytes_per_sample;
    int is_float;
    int channels;
    int recording;
} sdl_audio;

static JSClassID sdl_win_cid, sdl_ren_cid, sdl_tex_cid, sdl_audio_cid, sdl_pad_cid;
static JSClassID sdl_f32_cid, sdl_i16_cid, sdl_i32_cid;
static _Atomic int sdl_live_windows, sdl_live_streams;
static pthread_mutex_t sdl_mu = PTHREAD_MUTEX_INITIALIZER;
static SDL_InitFlags sdl_inited;
static int sdl_hints_set;

static const struct {
    const char* name;
    SDL_InitFlags flag;
} sdl_subsystems[] = {
    { "video", SDL_INIT_VIDEO },
    { "audio", SDL_INIT_AUDIO },
    { "events", SDL_INIT_EVENTS },
    { "joystick", SDL_INIT_JOYSTICK },
    { "gamepad", SDL_INIT_GAMEPAD },
};

static JSValue sdl_throw(JSContext* ctx, const char* what)
{
    return JS_ThrowInternalError(ctx, "dyna:sdl: %s failed: %s", what, SDL_GetError());
}

static int sdl_on_main(JSContext* ctx, const char* what)
{
#ifdef __APPLE__
    if (!pthread_main_np()) {
        JS_ThrowTypeError(ctx, "dyna:sdl: %s must run on the main thread, not in a Worker", what);
        return 0;
    }
#endif
    if (!js_std_runtime_is_main(JS_GetRuntime(ctx))) {
        JS_ThrowTypeError(ctx, "dyna:sdl: %s must run on the main thread, not in a Worker", what);
        return 0;
    }
    return 1;
}

static int sdl_need_video(JSContext* ctx, const char* what)
{
    if (!sdl_on_main(ctx, what))
        return 0;
    if (!(SDL_WasInit(SDL_INIT_VIDEO) & SDL_INIT_VIDEO)) {
        JS_ThrowTypeError(ctx, "dyna:sdl: %s needs init([\"video\"]) first", what);
        return 0;
    }
    return 1;
}

static int sdl_dim(JSContext* ctx, JSValueConst v, const char* what, int* out)
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

static int sdl_coord(JSContext* ctx, JSValueConst v, const char* what, float* out)
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

static int sdl_int_in(JSContext* ctx, JSValueConst v, const char* what, int lo, int hi, int* out)
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

static int sdl_opt_bool(JSContext* ctx, JSValueConst opts, const char* key, int* out)
{
    JSValue v;
    int b;
    if (!JS_IsObject(opts))
        return 0;
    v = JS_GetPropertyStr(ctx, opts, key);
    if (JS_IsException(v))
        return -1;
    if (JS_IsUndefined(v))
        return 0;
    b = JS_ToBool(ctx, v);
    JS_FreeValue(ctx, v);
    if (b < 0)
        return -1;
    *out = b;
    return 0;
}

static int sdl_check_opts(JSContext* ctx, JSValueConst opts, const char* who,
    const char* const* keys, int nkeys)
{
    JSPropertyEnum* tab;
    uint32_t len, i;
    int j, rc = 0;
    if (JS_IsUndefined(opts))
        return 0;
    if (!JS_IsObject(opts)) {
        JS_ThrowTypeError(ctx, "dyna:sdl: %s options must be an object", who);
        return -1;
    }
    if (JS_GetOwnPropertyNames(ctx, &tab, &len, opts, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY))
        return -1;
    for (i = 0; i < len && rc == 0; i++) {
        const char* k = JS_AtomToCString(ctx, tab[i].atom);
        int ok = 0;
        if (!k) {
            rc = -1;
            break;
        }
        for (j = 0; j < nkeys; j++)
            if (!strcmp(k, keys[j]))
                ok = 1;
        if (!ok) {
            JS_ThrowTypeError(ctx, "dyna:sdl: %s: unknown option '%s'", who, k);
            rc = -1;
        }
        JS_FreeCString(ctx, k);
    }
    JS_FreePropertyEnum(ctx, tab, len);
    return rc;
}

static void sdl_ren_kill(sdl_ren* r)
{
    sdl_tex* tx;
    for (tx = r->texs; tx; tx = tx->next) {
        if (tx->t)
            SDL_DestroyTexture(tx->t);
        tx->t = NULL;
        tx->ren = NULL;
    }
    r->texs = NULL;
    if (r->r)
        SDL_DestroyRenderer(r->r);
    r->r = NULL;
    if (r->win)
        r->win->ren = NULL;
    r->win = NULL;
}

static void sdl_win_dispose(void* p)
{
    sdl_win* w = p;
    if (w->ren)
        sdl_ren_kill(w->ren);
    if (w->w) {
        SDL_DestroyWindow(w->w);
        atomic_fetch_sub(&sdl_live_windows, 1);
    }
    dyn_nat_free(w);
}

static void sdl_ren_dispose(void* p)
{
    sdl_ren* r = p;
    JSValue held = r->win_obj;
    sdl_ren_kill(r);
    r->win_obj = JS_UNDEFINED;
    JS_FreeValueRT(r->rt, held);
    dyn_nat_free(r);
}

static void sdl_tex_dispose(void* p)
{
    sdl_tex* t = p;
    JSValue held = t->ren_obj;
    if (t->ren) {
        sdl_tex** pp = &t->ren->texs;
        while (*pp && *pp != t)
            pp = &(*pp)->next;
        if (*pp)
            *pp = t->next;
    }
    if (t->t)
        SDL_DestroyTexture(t->t);
    t->t = NULL;
    t->ren = NULL;
    t->ren_obj = JS_UNDEFINED;
    JS_FreeValueRT(t->rt, held);
    dyn_nat_free(t);
}

static void sdl_audio_dispose(void* p)
{
    sdl_audio* a = p;
    if (a->s) {
        SDL_DestroyAudioStream(a->s);
        atomic_fetch_sub(&sdl_live_streams, 1);
    }
    dyn_nat_free(a);
}

static void sdl_ren_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark)
{
    DynResource* res = JS_GetOpaque(val, sdl_ren_cid);
    if (res && res->native)
        JS_MarkValue(rt, ((sdl_ren*)res->native)->win_obj, mark);
}

static void sdl_tex_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark)
{
    DynResource* res = JS_GetOpaque(val, sdl_tex_cid);
    if (res && res->native)
        JS_MarkValue(rt, ((sdl_tex*)res->native)->ren_obj, mark);
}

static sdl_win* sdl_win_of(JSContext* ctx, JSValueConst v)
{
    sdl_win* w = dyn_res_native(ctx, v, sdl_win_cid);
    if (w && !w->w) {
        JS_ThrowTypeError(ctx, "dyna:sdl: the window is closed");
        return NULL;
    }
    return w;
}

static sdl_ren* sdl_ren_of(JSContext* ctx, JSValueConst v)
{
    sdl_ren* r = dyn_res_native(ctx, v, sdl_ren_cid);
    if (r && !r->r) {
        JS_ThrowTypeError(ctx, "dyna:sdl: the renderer's window was closed");
        return NULL;
    }
    return r;
}

static sdl_tex* sdl_tex_of(JSContext* ctx, JSValueConst v)
{
    sdl_tex* t = dyn_res_native(ctx, v, sdl_tex_cid);
    if (t && !t->t) {
        JS_ThrowTypeError(ctx, "dyna:sdl: the texture's renderer was closed");
        return NULL;
    }
    return t;
}

static SDL_InitFlags sdl_flag_named(const char* s)
{
    size_t i;
    for (i = 0; i < countof(sdl_subsystems); i++)
        if (!strcmp(s, sdl_subsystems[i].name))
            return sdl_subsystems[i].flag;
    return 0;
}

typedef struct {
    SDL_Gamepad* g;
    SDL_JoystickID id;
} sdl_pad;

static const char* const sdl_cursor_names[] = { "default", "text", "wait", "crosshair", "progress", "nwse-resize", "nesw-resize",
    "ew-resize", "ns-resize", "move", "not-allowed", "pointer", "nw-resize", "n-resize", "ne-resize", "e-resize", "se-resize",
    "s-resize", "sw-resize", "w-resize" };
static const char* const sdl_present_names[] = { "disabled", "stretch", "letterbox", "overscan", "integer" };
static const char* const sdl_flip_names[] = { "none", "horizontal", "vertical", "both" };
static SDL_Cursor* sdl_cursors[countof(sdl_cursor_names)];

static int sdl_name_index(JSContext* ctx, JSValueConst v, const char* what, const char* const* names, int n, int* out)
{
    const char* s;
    int i;
    if (!JS_IsString(v)) {
        JS_ThrowTypeError(ctx, "dyna:sdl: %s must be a string", what);
        return -1;
    }
    s = JS_ToCString(ctx, v);
    if (!s)
        return -1;
    for (i = 0; i < n; i++)
        if (!strcmp(s, names[i])) {
            JS_FreeCString(ctx, s);
            *out = i;
            return 0;
        }
    JS_ThrowRangeError(ctx, "dyna:sdl: %s '%s' is not one of %s .. %s", what, s, names[0], names[n - 1]);
    JS_FreeCString(ctx, s);
    return -1;
}

static int sdl_text_arg(JSContext* ctx, JSValueConst v, const char* who, const char** out)
{
    size_t n;
    const char* s;
    if (!JS_IsString(v)) {
        JS_ThrowTypeError(ctx, "dyna:sdl: %s needs a string", who);
        return -1;
    }
    s = JS_ToCStringLen(ctx, &n, v);
    if (!s)
        return -1;
    if (dyn_wtf8_has_surrogate(s, n)) {
        JS_FreeCString(ctx, s);
        JS_ThrowTypeError(ctx, "dyna:sdl: %s needs well-formed text (it holds a lone surrogate)", who);
        return -1;
    }
    *out = s;
    return 0;
}

static void sdl_cursors_free(void)
{
    size_t i;
    for (i = 0; i < countof(sdl_cursors); i++)
        if (sdl_cursors[i]) {
            SDL_DestroyCursor(sdl_cursors[i]);
            sdl_cursors[i] = NULL;
        }
}

static JSValue sdl_window_state(JSContext* ctx, JSValueConst this_val)
{
    static const struct {
        const char* k;
        SDL_WindowFlags f;
    } bits[] = {
        { "fullscreen", SDL_WINDOW_FULLSCREEN },
        { "hidden", SDL_WINDOW_HIDDEN },
        { "borderless", SDL_WINDOW_BORDERLESS },
        { "resizable", SDL_WINDOW_RESIZABLE },
        { "minimized", SDL_WINDOW_MINIMIZED },
        { "maximized", SDL_WINDOW_MAXIMIZED },
        { "mouseGrabbed", SDL_WINDOW_MOUSE_GRABBED },
        { "inputFocus", SDL_WINDOW_INPUT_FOCUS },
        { "mouseFocus", SDL_WINDOW_MOUSE_FOCUS },
        { "highDensity", SDL_WINDOW_HIGH_PIXEL_DENSITY },
        { "relativeMouse", SDL_WINDOW_MOUSE_RELATIVE_MODE },
        { "alwaysOnTop", SDL_WINDOW_ALWAYS_ON_TOP },
        { "occluded", SDL_WINDOW_OCCLUDED },
    };
    sdl_win* w = sdl_win_of(ctx, this_val);
    SDL_WindowFlags f;
    JSValue o;
    size_t i;
    if (!w)
        return JS_EXCEPTION;
    f = SDL_GetWindowFlags(w->w);
    o = JS_NewObject(ctx);
    if (JS_IsException(o))
        return o;
    for (i = 0; i < countof(bits); i++)
        if (JS_DefinePropertyValueStr(ctx, o, bits[i].k, JS_NewBool(ctx, (f & bits[i].f) != 0), JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, o);
            return JS_EXCEPTION;
        }
    return o;
}

static JSValue sdl_window_act2(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    sdl_win* w;
    bool ok = true;
    (void)argc;
    (void)argv;
    w = sdl_win_of(ctx, this_val);
    if (!w)
        return JS_EXCEPTION;
    switch (magic) {
    case 0:
        ok = SDL_MaximizeWindow(w->w);
        break;
    case 1:
        ok = SDL_MinimizeWindow(w->w);
        break;
    case 2:
        ok = SDL_RestoreWindow(w->w);
        break;
    case 3:
        ok = SDL_FlashWindow(w->w, SDL_FLASH_BRIEFLY);
        break;
    default:
        ok = SDL_SyncWindow(w->w);
        break;
    }
    if (!ok)
        return sdl_throw(ctx, "a window operation");
    return JS_UNDEFINED;
}

static JSValue sdl_window_flag(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    static const char* const usage[] = { "setResizable(on)", "setBordered(on)", "setAlwaysOnTop(on)", "setMouseGrab(on)",
        "setRelativeMouseMode(on)", "setKeyboardGrab(on)" };
    sdl_win* w;
    bool on, ok = true;
    if (argc < 1 || !JS_IsBool(argv[0]))
        return JS_ThrowTypeError(ctx, "dyna:sdl: %s needs a boolean", usage[magic]);
    on = JS_ToBool(ctx, argv[0]);
    w = sdl_win_of(ctx, this_val);
    if (!w)
        return JS_EXCEPTION;
    switch (magic) {
    case 0:
        ok = SDL_SetWindowResizable(w->w, on);
        break;
    case 1:
        ok = SDL_SetWindowBordered(w->w, on);
        break;
    case 2:
        ok = SDL_SetWindowAlwaysOnTop(w->w, on);
        break;
    case 3:
        ok = SDL_SetWindowMouseGrab(w->w, on);
        break;
    case 4:
        ok = SDL_SetWindowRelativeMouseMode(w->w, on);
        break;
    default:
        ok = SDL_SetWindowKeyboardGrab(w->w, on);
        break;
    }
    if (!ok)
        return sdl_throw(ctx, usage[magic]);
    return JS_UNDEFINED;
}

static JSValue sdl_cursor_fn(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    static const char* const names[] = { "showCursor()", "hideCursor()", "cursorVisible()", "setCursor(name)" };
    int idx = 0;
    bool ok = true;
    (void)this_val;
    if (magic == 3) {
        if (argc < 1)
            return JS_ThrowTypeError(ctx, "dyna:sdl: setCursor(name) needs a cursor name");
        if (sdl_name_index(ctx, argv[0], "setCursor name", sdl_cursor_names, (int)countof(sdl_cursor_names), &idx))
            return JS_EXCEPTION;
    }
    if (!sdl_need_video(ctx, names[magic]))
        return JS_EXCEPTION;
    switch (magic) {
    case 0:
        ok = SDL_ShowCursor();
        break;
    case 1:
        ok = SDL_HideCursor();
        break;
    case 2:
        return JS_NewBool(ctx, SDL_CursorVisible());
    default:
        if (!sdl_cursors[idx])
            sdl_cursors[idx] = SDL_CreateSystemCursor((SDL_SystemCursor)idx);
        if (!sdl_cursors[idx])
            return sdl_throw(ctx, "SDL_CreateSystemCursor");
        ok = SDL_SetCursor(sdl_cursors[idx]);
        break;
    }
    if (!ok)
        return sdl_throw(ctx, names[magic]);
    return JS_UNDEFINED;
}

static JSValue sdl_ren_debug_text(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    float x, y;
    const char* s;
    sdl_ren* r;
    bool ok;
    if (argc < 3)
        return JS_ThrowTypeError(ctx, "dyna:sdl: debugText(x, y, text)");
    if (sdl_coord(ctx, argv[0], "debugText x", &x) || sdl_coord(ctx, argv[1], "debugText y", &y))
        return JS_EXCEPTION;
    r = sdl_ren_of(ctx, this_val);
    if (!r)
        return JS_EXCEPTION;
    if (sdl_text_arg(ctx, argv[2], "debugText(x, y, text)", &s))
        return JS_EXCEPTION;
    ok = SDL_RenderDebugText(r->r, x, y, s);
    JS_FreeCString(ctx, s);
    if (!ok)
        return sdl_throw(ctx, "SDL_RenderDebugText");
    return JS_UNDEFINED;
}

static JSValue sdl_ren_logical(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    int w, h, mode = 2;
    sdl_ren* r;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "dyna:sdl: setLogicalPresentation(width, height, mode?)");
    if (sdl_int_in(ctx, argv[0], "setLogicalPresentation width", 0, SDL_MAX_DIM, &w)
        || sdl_int_in(ctx, argv[1], "setLogicalPresentation height", 0, SDL_MAX_DIM, &h))
        return JS_EXCEPTION;
    if (argc > 2 && !JS_IsUndefined(argv[2])
        && sdl_name_index(ctx, argv[2], "setLogicalPresentation mode", sdl_present_names, (int)countof(sdl_present_names), &mode))
        return JS_EXCEPTION;
    if (mode != 0 && (w == 0 || h == 0))
        return JS_ThrowRangeError(ctx, "dyna:sdl: setLogicalPresentation needs a width and height of at least 1 unless mode is \"disabled\"");
    r = sdl_ren_of(ctx, this_val);
    if (!r)
        return JS_EXCEPTION;
    if (!SDL_SetRenderLogicalPresentation(r->r, w, h, (SDL_RendererLogicalPresentation)mode))
        return sdl_throw(ctx, "SDL_SetRenderLogicalPresentation");
    return JS_UNDEFINED;
}

static JSValue sdl_ren_coords(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    const char* who = magic ? "renderToWindow(x, y)" : "windowToRender(x, y)";
    float x, y, ox = 0, oy = 0;
    sdl_ren* r;
    bool ok;
    JSValue o;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "dyna:sdl: %s", who);
    if (sdl_coord(ctx, argv[0], who, &x) || sdl_coord(ctx, argv[1], who, &y))
        return JS_EXCEPTION;
    r = sdl_ren_of(ctx, this_val);
    if (!r)
        return JS_EXCEPTION;
    ok = magic ? SDL_RenderCoordinatesToWindow(r->r, x, y, &ox, &oy) : SDL_RenderCoordinatesFromWindow(r->r, x, y, &ox, &oy);
    if (!ok)
        return sdl_throw(ctx, who);
    o = JS_NewObject(ctx);
    if (JS_IsException(o))
        return o;
    if (JS_DefinePropertyValueStr(ctx, o, "x", JS_NewFloat64(ctx, (double)ox), JS_PROP_C_W_E) < 0
        || JS_DefinePropertyValueStr(ctx, o, "y", JS_NewFloat64(ctx, (double)oy), JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, o);
        return JS_EXCEPTION;
    }
    return o;
}

static const char* const sdl_draw_tex_keys[] = { "angle", "center", "flip" };

static int sdl_draw_tex_opts(JSContext* ctx, JSValueConst opts, double* angle, SDL_FPoint* center, int* has_center, SDL_FlipMode* flip)
{
    JSValue v;
    int idx = 0;
    *angle = 0;
    *has_center = 0;
    *flip = SDL_FLIP_NONE;
    if (sdl_check_opts(ctx, opts, "drawTexture", sdl_draw_tex_keys, (int)countof(sdl_draw_tex_keys)))
        return -1;
    v = JS_GetPropertyStr(ctx, opts, "angle");
    if (JS_IsException(v))
        return -1;
    if (!JS_IsUndefined(v)) {
        int rc = JS_ToFloat64(ctx, angle, v);
        JS_FreeValue(ctx, v);
        if (rc)
            return -1;
        if (!isfinite(*angle) || fabs(*angle) > 1e7) {
            JS_ThrowRangeError(ctx, "dyna:sdl: drawTexture angle must be a finite number of degrees within +-1e7");
            return -1;
        }
    }
    v = JS_GetPropertyStr(ctx, opts, "center");
    if (JS_IsException(v))
        return -1;
    if (!JS_IsUndefined(v)) {
        JSValue cx, cy;
        int rc;
        if (!JS_IsObject(v)) {
            JS_FreeValue(ctx, v);
            JS_ThrowTypeError(ctx, "dyna:sdl: drawTexture center must be { x, y }");
            return -1;
        }
        cx = JS_GetPropertyStr(ctx, v, "x");
        cy = JS_IsException(cx) ? JS_EXCEPTION : JS_GetPropertyStr(ctx, v, "y");
        JS_FreeValue(ctx, v);
        rc = JS_IsException(cx) || JS_IsException(cy) || sdl_coord(ctx, cx, "drawTexture center", &center->x)
            || sdl_coord(ctx, cy, "drawTexture center", &center->y);
        JS_FreeValue(ctx, cx);
        JS_FreeValue(ctx, cy);
        if (rc)
            return -1;
        *has_center = 1;
    }
    v = JS_GetPropertyStr(ctx, opts, "flip");
    if (JS_IsException(v))
        return -1;
    if (!JS_IsUndefined(v)) {
        int rc = sdl_name_index(ctx, v, "drawTexture flip", sdl_flip_names, (int)countof(sdl_flip_names), &idx);
        JS_FreeValue(ctx, v);
        if (rc)
            return -1;
        *flip = idx == 3 ? SDL_FLIP_HORIZONTAL_AND_VERTICAL : idx == 2 ? SDL_FLIP_VERTICAL
            : idx == 1                                                 ? SDL_FLIP_HORIZONTAL
                                                                       : SDL_FLIP_NONE;
    }
    return 0;
}

static _Atomic int sdl_live_pads;

static void sdl_pad_dispose(void* p)
{
    sdl_pad* g = p;
    if (g->g) {
        SDL_CloseGamepad(g->g);
        atomic_fetch_sub(&sdl_live_pads, 1);
    }
    dyn_nat_free(g);
}

static int sdl_need_pad(JSContext* ctx, const char* what)
{
    if (!sdl_on_main(ctx, what))
        return 0;
    if (!(SDL_WasInit(SDL_INIT_GAMEPAD) & SDL_INIT_GAMEPAD)) {
        JS_ThrowTypeError(ctx, "dyna:sdl: %s needs init([\"gamepad\"]) first", what);
        return 0;
    }
    return 1;
}

static int sdl_joystick_id(JSContext* ctx, JSValueConst v, const char* what, SDL_JoystickID* out)
{
    double d;
    if (JS_ToFloat64(ctx, &d, v))
        return -1;
    if (!(d >= 1 && d <= 4294967295.0) || d != floor(d)) {
        JS_ThrowRangeError(ctx, "dyna:sdl: %s must be a gamepad id from gamepads() (an integer 1..0xFFFFFFFF)", what);
        return -1;
    }
    *out = (SDL_JoystickID)d;
    return 0;
}

static JSValue sdl_gamepads(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    SDL_JoystickID* ids;
    int n = 0, i;
    JSValue arr;
    (void)this_val;
    (void)argc;
    (void)argv;
    if (!sdl_need_pad(ctx, "gamepads()"))
        return JS_EXCEPTION;
    ids = SDL_GetGamepads(&n);
    if (!ids)
        return sdl_throw(ctx, "SDL_GetGamepads");
    arr = JS_NewArray(ctx);
    for (i = 0; i < n && !JS_IsException(arr); i++)
        if (JS_DefinePropertyValueUint32(ctx, arr, (uint32_t)i, JS_NewFloat64(ctx, (double)ids[i]), JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, arr);
            arr = JS_EXCEPTION;
        }
    SDL_free(ids);
    return arr;
}

static JSValue sdl_virtual_pad(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    SDL_VirtualJoystickDesc desc;
    SDL_JoystickID id = 0;
    const char* name = NULL;
    (void)this_val;
    if (magic) {
        if (argc < 1)
            return JS_ThrowTypeError(ctx, "dyna:sdl: detachVirtualGamepad(id) needs the id attachVirtualGamepad() returned");
        if (sdl_joystick_id(ctx, argv[0], "detachVirtualGamepad id", &id))
            return JS_EXCEPTION;
    } else if (argc > 0 && !JS_IsUndefined(argv[0]) && sdl_text_arg(ctx, argv[0], "attachVirtualGamepad(name?)", &name)) {
        return JS_EXCEPTION;
    }
    if (!sdl_need_pad(ctx, magic ? "detachVirtualGamepad()" : "attachVirtualGamepad()")) {
        if (name)
            JS_FreeCString(ctx, name);
        return JS_EXCEPTION;
    }
    if (magic) {
        if (!SDL_IsJoystickVirtual(id))
            return JS_ThrowRangeError(ctx, "dyna:sdl: detachVirtualGamepad: %u is not a virtual gamepad", (unsigned)id);
        if (!SDL_DetachVirtualJoystick(id))
            return sdl_throw(ctx, "SDL_DetachVirtualJoystick");
        return JS_UNDEFINED;
    }
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.button_mask = (Uint32)((1ull << SDL_GAMEPAD_BUTTON_COUNT) - 1);
    desc.axis_mask = (Uint32)((1ull << SDL_GAMEPAD_AXIS_COUNT) - 1);
    desc.name = name ? name : "dyna:sdl virtual gamepad";
    id = SDL_AttachVirtualJoystick(&desc);
    if (name)
        JS_FreeCString(ctx, name);
    if (!id)
        return sdl_throw(ctx, "SDL_AttachVirtualJoystick");
    return JS_NewFloat64(ctx, (double)id);
}

static JSValue sdl_pad_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv)
{
    SDL_JoystickID id;
    sdl_pad* g;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "dyna:sdl: new Gamepad(id) needs an id from gamepads()");
    if (sdl_joystick_id(ctx, argv[0], "Gamepad id", &id))
        return JS_EXCEPTION;
    if (!sdl_need_pad(ctx, "new Gamepad()"))
        return JS_EXCEPTION;
    if (!SDL_IsGamepad(id))
        return JS_ThrowRangeError(ctx, "dyna:sdl: Gamepad: %u is not a connected gamepad", (unsigned)id);
    g = dyn_nat_calloc(1, sizeof(*g));
    if (!g)
        return JS_ThrowOutOfMemory(ctx);
    g->g = SDL_OpenGamepad(id);
    if (!g->g) {
        dyn_nat_free(g);
        return sdl_throw(ctx, "SDL_OpenGamepad");
    }
    g->id = id;
    atomic_fetch_add(&sdl_live_pads, 1);
    return dyn_res_wrap(ctx, new_target, sdl_pad_cid, g, sdl_pad_dispose);
}

static sdl_pad* sdl_pad_of(JSContext* ctx, JSValueConst v)
{
    return dyn_res_native(ctx, v, sdl_pad_cid);
}

static JSValue sdl_pad_get(JSContext* ctx, JSValueConst this_val, int magic)
{
    sdl_pad* g = sdl_pad_of(ctx, this_val);
    const char* s;
    if (!g)
        return JS_EXCEPTION;
    switch (magic) {
    case 0:
        return JS_NewFloat64(ctx, (double)g->id);
    case 1:
        s = SDL_GetGamepadName(g->g);
        return JS_NewString(ctx, s ? s : "");
    case 2:
        s = SDL_GetGamepadStringForType(SDL_GetGamepadType(g->g));
        return JS_NewString(ctx, s ? s : "unknown");
    case 3:
        return JS_NewBool(ctx, SDL_GamepadConnected(g->g));
    default:
        return JS_NewBool(ctx, SDL_IsJoystickVirtual(g->id));
    }
}

static int sdl_pad_part(JSContext* ctx, JSValueConst v, int axis, const char* who, int* out)
{
    const char* s;
    int idx;
    if (!JS_IsString(v)) {
        JS_ThrowTypeError(ctx, "dyna:sdl: %s takes a %s name (%s)", who, axis ? "axis" : "button",
            axis ? "leftx, lefty, rightx, righty, lefttrigger, righttrigger" : "a, b, x, y, back, guide, start, dpup, ...");
        return -1;
    }
    s = JS_ToCString(ctx, v);
    if (!s)
        return -1;
    idx = axis ? (int)SDL_GetGamepadAxisFromString(s) : (int)SDL_GetGamepadButtonFromString(s);
    if (idx < 0) {
        JS_ThrowRangeError(ctx, "dyna:sdl: %s: unknown %s '%s'", who, axis ? "axis" : "button", s);
        JS_FreeCString(ctx, s);
        return -1;
    }
    JS_FreeCString(ctx, s);
    *out = idx;
    return 0;
}

static JSValue sdl_pad_read(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    const char* who = magic ? "axis(name)" : "button(name)";
    sdl_pad* g;
    int idx;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "dyna:sdl: %s needs a name", who);
    if (sdl_pad_part(ctx, argv[0], magic, who, &idx))
        return JS_EXCEPTION;
    g = sdl_pad_of(ctx, this_val);
    if (!g)
        return JS_EXCEPTION;
    if (!sdl_need_pad(ctx, who))
        return JS_EXCEPTION;
    if (!magic)
        return JS_NewBool(ctx, SDL_GetGamepadButton(g->g, (SDL_GamepadButton)idx));
    {
        double v = (double)SDL_GetGamepadAxis(g->g, (SDL_GamepadAxis)idx) / 32767.0;
        return JS_NewFloat64(ctx, v < -1 ? -1 : v);
    }
}

static JSValue sdl_pad_rumble(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    double lo, hi;
    int ms;
    sdl_pad* g;
    if (argc < 3)
        return JS_ThrowTypeError(ctx, "dyna:sdl: rumble(low, high, ms)");
    if (JS_ToFloat64(ctx, &lo, argv[0]) || JS_ToFloat64(ctx, &hi, argv[1]))
        return JS_EXCEPTION;
    if (!(lo >= 0 && lo <= 1) || !(hi >= 0 && hi <= 1))
        return JS_ThrowRangeError(ctx, "dyna:sdl: rumble low and high are intensities in 0..1");
    if (sdl_int_in(ctx, argv[2], "rumble ms", 0, 60000, &ms))
        return JS_EXCEPTION;
    g = sdl_pad_of(ctx, this_val);
    if (!g)
        return JS_EXCEPTION;
    if (!sdl_need_pad(ctx, "rumble()"))
        return JS_EXCEPTION;
    return JS_NewBool(ctx, SDL_RumbleGamepad(g->g, (Uint16)(lo * 65535.0), (Uint16)(hi * 65535.0), (Uint32)ms));
}

static JSValue sdl_pad_set_virtual(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    const char* who = magic ? "setVirtualAxis(name, value)" : "setVirtualButton(name, down)";
    sdl_pad* g;
    int idx, down = 0;
    double v = 0;
    bool ok;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "dyna:sdl: %s", who);
    if (sdl_pad_part(ctx, argv[0], magic, who, &idx))
        return JS_EXCEPTION;
    if (magic) {
        if (JS_ToFloat64(ctx, &v, argv[1]))
            return JS_EXCEPTION;
        if (!(v >= -1 && v <= 1))
            return JS_ThrowRangeError(ctx, "dyna:sdl: setVirtualAxis value must be in -1..1");
    } else {
        if (!JS_IsBool(argv[1]))
            return JS_ThrowTypeError(ctx, "dyna:sdl: setVirtualButton(name, down) needs a boolean");
        down = JS_ToBool(ctx, argv[1]);
    }
    g = sdl_pad_of(ctx, this_val);
    if (!g)
        return JS_EXCEPTION;
    if (!sdl_need_pad(ctx, who))
        return JS_EXCEPTION;
    if (!SDL_IsJoystickVirtual(g->id))
        return JS_ThrowTypeError(ctx, "dyna:sdl: %s works on a gamepad from attachVirtualGamepad() only", who);
    ok = magic ? SDL_SetJoystickVirtualAxis(SDL_GetGamepadJoystick(g->g), idx, (Sint16)(v * 32767.0))
               : SDL_SetJoystickVirtualButton(SDL_GetGamepadJoystick(g->g), idx, down);
    if (!ok)
        return sdl_throw(ctx, who);
    SDL_UpdateGamepads();
    return JS_UNDEFINED;
}

static JSValue sdl_sysinfo(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    static const char* const power[] = { "error", "unknown", "battery", "no-battery", "charging", "charged" };
    static const char* const theme[] = { "unknown", "light", "dark" };
    JSValue o;
    (void)this_val;
    (void)argc;
    (void)argv;
    switch (magic) {
    case 0: {
        int secs = -1, pct = -1;
        SDL_PowerState st = SDL_GetPowerInfo(&secs, &pct);
        int i = (int)st + 1;
        o = JS_NewObject(ctx);
        if (JS_IsException(o))
            return o;
        if (JS_DefinePropertyValueStr(ctx, o, "state", JS_NewString(ctx, i >= 0 && i < (int)countof(power) ? power[i] : "unknown"), JS_PROP_C_W_E) < 0
            || JS_DefinePropertyValueStr(ctx, o, "seconds", secs < 0 ? JS_NULL : JS_NewInt32(ctx, secs), JS_PROP_C_W_E) < 0
            || JS_DefinePropertyValueStr(ctx, o, "percent", pct < 0 ? JS_NULL : JS_NewInt32(ctx, pct), JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, o);
            return JS_EXCEPTION;
        }
        return o;
    }
    case 1: {
        int t = (int)SDL_GetSystemTheme();
        return JS_NewString(ctx, t >= 0 && t < (int)countof(theme) ? theme[t] : "unknown");
    }
    case 2: {
        int n = 0, i;
        SDL_Locale** loc = SDL_GetPreferredLocales(&n);
        JSValue arr = JS_NewArray(ctx);
        for (i = 0; loc && i < n && !JS_IsException(arr); i++) {
            char buf[64];
            if (loc[i]->country)
                snprintf(buf, sizeof(buf), "%s-%s", loc[i]->language, loc[i]->country);
            else
                snprintf(buf, sizeof(buf), "%s", loc[i]->language);
            if (JS_DefinePropertyValueUint32(ctx, arr, (uint32_t)i, JS_NewString(ctx, buf), JS_PROP_C_W_E) < 0) {
                JS_FreeValue(ctx, arr);
                arr = JS_EXCEPTION;
            }
        }
        SDL_free(loc);
        return arr;
    }
    case 3:
        return JS_NewInt32(ctx, SDL_GetNumLogicalCPUCores());
    case 4:
        return JS_NewInt32(ctx, SDL_GetSystemRAM());
    default: {
        const char* p = SDL_GetBasePath();
        return p ? JS_NewString(ctx, p) : JS_NULL;
    }
    }
}

static JSValue sdl_init(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    SDL_InitFlags flags = 0;
    (void)this_val;
    if (argc < 1 || JS_IsUndefined(argv[0])) {
        flags = SDL_INIT_VIDEO;
    } else {
        int64_t n, i;
        if (!JS_IsArray(ctx, argv[0]))
            return JS_ThrowTypeError(ctx, "dyna:sdl: init(subsystems) takes an array of names");
        {
            JSValue lv = JS_GetPropertyStr(ctx, argv[0], "length");
            int lr;
            if (JS_IsException(lv))
                return JS_EXCEPTION;
            lr = JS_ToInt64(ctx, &n, lv);
            JS_FreeValue(ctx, lv);
            if (lr)
                return JS_EXCEPTION;
        }
        if (n > (int64_t)countof(sdl_subsystems))
            return JS_ThrowRangeError(ctx, "dyna:sdl: init: at most %d subsystem names", (int)countof(sdl_subsystems));
        for (i = 0; i < n; i++) {
            JSValue e = JS_GetPropertyUint32(ctx, argv[0], (uint32_t)i);
            const char* s;
            SDL_InitFlags f;
            if (JS_IsException(e))
                return JS_EXCEPTION;
            if (!JS_IsString(e)) {
                JS_FreeValue(ctx, e);
                return JS_ThrowTypeError(ctx, "dyna:sdl: init: subsystem names are strings (video, audio, events, joystick, gamepad)");
            }
            s = JS_ToCString(ctx, e);
            JS_FreeValue(ctx, e);
            if (!s)
                return JS_EXCEPTION;
            f = sdl_flag_named(s);
            if (!f) {
                JS_ThrowRangeError(ctx, "dyna:sdl: init: unknown subsystem '%s' (video, audio, events, joystick, gamepad)", s);
                JS_FreeCString(ctx, s);
                return JS_EXCEPTION;
            }
            JS_FreeCString(ctx, s);
            flags |= f;
        }
    }
    if ((flags & SDL_INIT_VIDEO) && !sdl_on_main(ctx, "init([\"video\"])"))
        return JS_EXCEPTION;
    {
        bool ok = true;
        pthread_mutex_lock(&sdl_mu);
        if (!sdl_hints_set) {
            SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
            sdl_hints_set = 1;
        }
        if (flags & ~sdl_inited) {
            ok = SDL_InitSubSystem(flags & ~sdl_inited);
            if (ok)
                sdl_inited |= flags;
        }
        pthread_mutex_unlock(&sdl_mu);
        if (!ok)
            return sdl_throw(ctx, "SDL_InitSubSystem");
    }
    return JS_UNDEFINED;
}

static JSValue sdl_quit(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)this_val;
    (void)argc;
    (void)argv;
    if (atomic_load(&sdl_live_windows) || atomic_load(&sdl_live_streams) || atomic_load(&sdl_live_pads))
        return JS_ThrowTypeError(ctx, "dyna:sdl: quit() while %d window(s), %d audio stream(s) and %d gamepad(s) are open; close them first",
            atomic_load(&sdl_live_windows), atomic_load(&sdl_live_streams), atomic_load(&sdl_live_pads));
    sdl_cursors_free();
    pthread_mutex_lock(&sdl_mu);
    if (sdl_inited) {
        SDL_QuitSubSystem(sdl_inited);
        sdl_inited = 0;
    }
    pthread_mutex_unlock(&sdl_mu);
    return JS_UNDEFINED;
}

static JSValue sdl_was_init(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    JSValue arr = JS_NewArray(ctx);
    SDL_InitFlags got = SDL_WasInit(0);
    uint32_t n = 0;
    size_t i;
    (void)this_val;
    (void)argc;
    (void)argv;
    if (JS_IsException(arr))
        return arr;
    for (i = 0; i < countof(sdl_subsystems); i++)
        if ((got & sdl_subsystems[i].flag) == sdl_subsystems[i].flag && JS_DefinePropertyValueUint32(ctx, arr, n++, JS_NewString(ctx, sdl_subsystems[i].name), JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, arr);
            return JS_EXCEPTION;
        }
    return arr;
}

static JSValue sdl_version(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    int v = SDL_GetVersion();
    JSValue o = JS_NewObject(ctx);
    (void)this_val;
    (void)argc;
    (void)argv;
    if (JS_IsException(o))
        return o;
    if (JS_DefinePropertyValueStr(ctx, o, "major", JS_NewInt32(ctx, SDL_VERSIONNUM_MAJOR(v)), JS_PROP_C_W_E) < 0
        || JS_DefinePropertyValueStr(ctx, o, "minor", JS_NewInt32(ctx, SDL_VERSIONNUM_MINOR(v)), JS_PROP_C_W_E) < 0
        || JS_DefinePropertyValueStr(ctx, o, "patch", JS_NewInt32(ctx, SDL_VERSIONNUM_MICRO(v)), JS_PROP_C_W_E) < 0
        || JS_DefinePropertyValueStr(ctx, o, "revision", JS_NewString(ctx, SDL_GetRevision()), JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, o);
        return JS_EXCEPTION;
    }
    return o;
}

static JSValue sdl_str_fn(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    const char* s = NULL;
    (void)this_val;
    (void)argc;
    (void)argv;
    switch (magic) {
    case 0:
        s = SDL_GetPlatform();
        break;
    case 1:
        s = SDL_GetError();
        break;
    case 2:
        s = SDL_GetCurrentVideoDriver();
        break;
    case 3:
        s = SDL_GetCurrentAudioDriver();
        break;
    }
    return s ? JS_NewString(ctx, s) : JS_NULL;
}

static JSValue sdl_drivers(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    JSValue arr = JS_NewArray(ctx);
    int i, n = magic ? SDL_GetNumAudioDrivers() : SDL_GetNumVideoDrivers();
    (void)this_val;
    (void)argc;
    (void)argv;
    if (JS_IsException(arr))
        return arr;
    for (i = 0; i < n; i++) {
        const char* s = magic ? SDL_GetAudioDriver(i) : SDL_GetVideoDriver(i);
        if (s && JS_DefinePropertyValueUint32(ctx, arr, (uint32_t)i, JS_NewString(ctx, s), JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, arr);
            return JS_EXCEPTION;
        }
    }
    return arr;
}

static JSValue sdl_ticks(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)this_val;
    (void)argc;
    (void)argv;
    return JS_NewFloat64(ctx, (double)SDL_GetTicks());
}

static int sdl_set_int(JSContext* ctx, JSValue o, const char* k, double v)
{
    return JS_DefinePropertyValueStr(ctx, o, k, JS_NewFloat64(ctx, v), JS_PROP_C_W_E);
}

static JSValue sdl_displays(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    int n = 0, i;
    SDL_DisplayID* ids;
    JSValue arr;
    (void)this_val;
    (void)argc;
    (void)argv;
    if (!sdl_need_video(ctx, "displays()"))
        return JS_EXCEPTION;
    ids = SDL_GetDisplays(&n);
    if (!ids)
        return sdl_throw(ctx, "SDL_GetDisplays");
    arr = JS_NewArray(ctx);
    for (i = 0; i < n && !JS_IsException(arr); i++) {
        SDL_Rect b = { 0, 0, 0, 0 };
        const SDL_DisplayMode* m = SDL_GetCurrentDisplayMode(ids[i]);
        const char* name = SDL_GetDisplayName(ids[i]);
        JSValue o = JS_NewObject(ctx);
        SDL_GetDisplayBounds(ids[i], &b);
        if (JS_IsException(o)
            || sdl_set_int(ctx, o, "id", ids[i]) < 0
            || JS_DefinePropertyValueStr(ctx, o, "name", name ? JS_NewString(ctx, name) : JS_NULL, JS_PROP_C_W_E) < 0
            || sdl_set_int(ctx, o, "x", b.x) < 0 || sdl_set_int(ctx, o, "y", b.y) < 0
            || sdl_set_int(ctx, o, "width", b.w) < 0 || sdl_set_int(ctx, o, "height", b.h) < 0
            || sdl_set_int(ctx, o, "refreshRate", m ? (double)m->refresh_rate : 0) < 0
            || sdl_set_int(ctx, o, "pixelDensity", m ? (double)m->pixel_density : 1) < 0
            || JS_DefinePropertyValueUint32(ctx, arr, (uint32_t)i, o, JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, arr);
            arr = JS_EXCEPTION;
        }
    }
    SDL_free(ids);
    return arr;
}

static const char* sdl_event_name(Uint32 t)
{
    switch (t) {
    case SDL_EVENT_QUIT:
        return "quit";
    case SDL_EVENT_KEY_DOWN:
        return "keydown";
    case SDL_EVENT_KEY_UP:
        return "keyup";
    case SDL_EVENT_TEXT_INPUT:
        return "textinput";
    case SDL_EVENT_TEXT_EDITING:
        return "textediting";
    case SDL_EVENT_DROP_FILE:
        return "dropfile";
    case SDL_EVENT_MOUSE_MOTION:
        return "mousemotion";
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        return "mousebuttondown";
    case SDL_EVENT_MOUSE_BUTTON_UP:
        return "mousebuttonup";
    case SDL_EVENT_MOUSE_WHEEL:
        return "mousewheel";
    case SDL_EVENT_WINDOW_SHOWN:
        return "windowshown";
    case SDL_EVENT_WINDOW_HIDDEN:
        return "windowhidden";
    case SDL_EVENT_WINDOW_MOVED:
        return "windowmoved";
    case SDL_EVENT_WINDOW_RESIZED:
        return "windowresized";
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
        return "windowfocusgained";
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        return "windowfocuslost";
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        return "windowclose";
    case SDL_EVENT_WINDOW_EXPOSED:
        return "windowexposed";
    case SDL_EVENT_WINDOW_MINIMIZED:
        return "windowminimized";
    case SDL_EVENT_WINDOW_MAXIMIZED:
        return "windowmaximized";
    case SDL_EVENT_WINDOW_RESTORED:
        return "windowrestored";
    case SDL_EVENT_WINDOW_MOUSE_ENTER:
        return "windowmouseenter";
    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
        return "windowmouseleave";
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        return "windowpixelsizechanged";
    case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
        return "windowdisplayscalechanged";
    case SDL_EVENT_FINGER_DOWN:
        return "fingerdown";
    case SDL_EVENT_FINGER_UP:
        return "fingerup";
    case SDL_EVENT_FINGER_MOTION:
        return "fingermotion";
    case SDL_EVENT_FINGER_CANCELED:
        return "fingercanceled";
    case SDL_EVENT_GAMEPAD_ADDED:
        return "gamepadadded";
    case SDL_EVENT_GAMEPAD_REMOVED:
        return "gamepadremoved";
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        return "gamepadbuttondown";
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
        return "gamepadbuttonup";
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        return "gamepadaxis";
    default:
        return t >= SDL_EVENT_USER && t <= SDL_EVENT_LAST ? "user" : "other";
    }
}

static JSValue sdl_event_obj(JSContext* ctx, const SDL_Event* e)
{
    JSValue o = JS_NewObject(ctx);
    int bad = 0;
    if (JS_IsException(o))
        return o;
    bad |= JS_DefinePropertyValueStr(ctx, o, "type", JS_NewString(ctx, sdl_event_name(e->type)), JS_PROP_C_W_E) < 0;
    bad |= sdl_set_int(ctx, o, "timestamp", (double)e->common.timestamp / 1e6) < 0;
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        const char* kn = SDL_GetKeyName(e->key.key);
        bad |= sdl_set_int(ctx, o, "windowId", e->key.windowID) < 0;
        bad |= sdl_set_int(ctx, o, "key", e->key.key) < 0;
        bad |= JS_DefinePropertyValueStr(ctx, o, "keyName", JS_NewString(ctx, kn ? kn : ""), JS_PROP_C_W_E) < 0;
        bad |= sdl_set_int(ctx, o, "scancode", e->key.scancode) < 0;
        bad |= sdl_set_int(ctx, o, "mod", e->key.mod) < 0;
        bad |= JS_DefinePropertyValueStr(ctx, o, "repeat", JS_NewBool(ctx, e->key.repeat), JS_PROP_C_W_E) < 0;
        break;
    }
    case SDL_EVENT_TEXT_INPUT:
        bad |= sdl_set_int(ctx, o, "windowId", e->text.windowID) < 0;
        bad |= JS_DefinePropertyValueStr(ctx, o, "text", JS_NewString(ctx, e->text.text ? e->text.text : ""), JS_PROP_C_W_E) < 0;
        break;
    case SDL_EVENT_TEXT_EDITING:
        bad |= sdl_set_int(ctx, o, "windowId", e->edit.windowID) < 0;
        bad |= JS_DefinePropertyValueStr(ctx, o, "text", JS_NewString(ctx, e->edit.text ? e->edit.text : ""), JS_PROP_C_W_E) < 0;
        bad |= sdl_set_int(ctx, o, "start", e->edit.start) < 0 || sdl_set_int(ctx, o, "length", e->edit.length) < 0;
        break;
    case SDL_EVENT_DROP_FILE:
        bad |= sdl_set_int(ctx, o, "windowId", e->drop.windowID) < 0;
        bad |= JS_DefinePropertyValueStr(ctx, o, "file", JS_NewString(ctx, e->drop.data ? e->drop.data : ""), JS_PROP_C_W_E) < 0;
        break;
    case SDL_EVENT_MOUSE_MOTION:
        bad |= sdl_set_int(ctx, o, "windowId", e->motion.windowID) < 0;
        bad |= sdl_set_int(ctx, o, "x", (double)e->motion.x) < 0 || sdl_set_int(ctx, o, "y", (double)e->motion.y) < 0;
        bad |= sdl_set_int(ctx, o, "xrel", (double)e->motion.xrel) < 0 || sdl_set_int(ctx, o, "yrel", (double)e->motion.yrel) < 0;
        bad |= sdl_set_int(ctx, o, "buttons", e->motion.state) < 0;
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        bad |= sdl_set_int(ctx, o, "windowId", e->button.windowID) < 0;
        bad |= sdl_set_int(ctx, o, "button", e->button.button) < 0;
        bad |= sdl_set_int(ctx, o, "clicks", e->button.clicks) < 0;
        bad |= sdl_set_int(ctx, o, "x", (double)e->button.x) < 0 || sdl_set_int(ctx, o, "y", (double)e->button.y) < 0;
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        bad |= sdl_set_int(ctx, o, "windowId", e->wheel.windowID) < 0;
        bad |= sdl_set_int(ctx, o, "x", (double)e->wheel.x) < 0 || sdl_set_int(ctx, o, "y", (double)e->wheel.y) < 0;
        break;
    case SDL_EVENT_FINGER_DOWN:
    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_MOTION:
    case SDL_EVENT_FINGER_CANCELED:
        bad |= sdl_set_int(ctx, o, "windowId", e->tfinger.windowID) < 0;
        bad |= sdl_set_int(ctx, o, "touchId", (double)e->tfinger.touchID) < 0 || sdl_set_int(ctx, o, "fingerId", (double)e->tfinger.fingerID) < 0;
        bad |= sdl_set_int(ctx, o, "x", (double)e->tfinger.x) < 0 || sdl_set_int(ctx, o, "y", (double)e->tfinger.y) < 0;
        bad |= sdl_set_int(ctx, o, "dx", (double)e->tfinger.dx) < 0 || sdl_set_int(ctx, o, "dy", (double)e->tfinger.dy) < 0;
        bad |= sdl_set_int(ctx, o, "pressure", (double)e->tfinger.pressure) < 0;
        break;
    case SDL_EVENT_GAMEPAD_ADDED:
    case SDL_EVENT_GAMEPAD_REMOVED:
        bad |= sdl_set_int(ctx, o, "which", (double)e->gdevice.which) < 0;
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        const char* bn = SDL_GetGamepadStringForButton((SDL_GamepadButton)e->gbutton.button);
        bad |= sdl_set_int(ctx, o, "which", (double)e->gbutton.which) < 0;
        bad |= JS_DefinePropertyValueStr(ctx, o, "button", JS_NewString(ctx, bn ? bn : ""), JS_PROP_C_W_E) < 0;
        break;
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        const char* an = SDL_GetGamepadStringForAxis((SDL_GamepadAxis)e->gaxis.axis);
        double v = (double)e->gaxis.value / 32767.0;
        bad |= sdl_set_int(ctx, o, "which", (double)e->gaxis.which) < 0;
        bad |= JS_DefinePropertyValueStr(ctx, o, "axis", JS_NewString(ctx, an ? an : ""), JS_PROP_C_W_E) < 0;
        bad |= JS_DefinePropertyValueStr(ctx, o, "value", JS_NewFloat64(ctx, v < -1 ? -1 : v), JS_PROP_C_W_E) < 0;
        break;
    }
    default:
        if (e->type >= SDL_EVENT_WINDOW_FIRST && e->type <= SDL_EVENT_WINDOW_LAST) {
            bad |= sdl_set_int(ctx, o, "windowId", e->window.windowID) < 0;
            bad |= sdl_set_int(ctx, o, "data1", e->window.data1) < 0 || sdl_set_int(ctx, o, "data2", e->window.data2) < 0;
        } else if (e->type >= SDL_EVENT_USER && e->type <= SDL_EVENT_LAST) {
            bad |= sdl_set_int(ctx, o, "code", e->user.code) < 0;
        }
        break;
    }
    if (bad) {
        JS_FreeValue(ctx, o);
        return JS_EXCEPTION;
    }
    return o;
}

static int sdl_events_ready(JSContext* ctx, const char* what)
{
    if (!sdl_on_main(ctx, what))
        return 0;
    if (!(SDL_WasInit(SDL_INIT_EVENTS) & SDL_INIT_EVENTS)) {
        JS_ThrowTypeError(ctx, "dyna:sdl: %s needs init() first (video or events)", what);
        return 0;
    }
    return 1;
}

static JSValue sdl_poll_event(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    SDL_Event e;
    (void)this_val;
    (void)argc;
    (void)argv;
    if (!sdl_events_ready(ctx, "pollEvent()"))
        return JS_EXCEPTION;
    if (!SDL_PollEvent(&e))
        return JS_NULL;
    return sdl_event_obj(ctx, &e);
}

static JSValue sdl_poll_events(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    SDL_Event e;
    int max = 256, n = 0;
    JSValue arr;
    (void)this_val;
    if (argc > 0 && !JS_IsUndefined(argv[0]) && sdl_int_in(ctx, argv[0], "pollEvents(max)", 1, 65536, &max))
        return JS_EXCEPTION;
    if (!sdl_events_ready(ctx, "pollEvents()"))
        return JS_EXCEPTION;
    arr = JS_NewArray(ctx);
    while (n < max && !JS_IsException(arr) && SDL_PollEvent(&e)) {
        JSValue o = sdl_event_obj(ctx, &e);
        if (JS_IsException(o) || JS_DefinePropertyValueUint32(ctx, arr, (uint32_t)n++, o, JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, arr);
            arr = JS_EXCEPTION;
        }
    }
    return arr;
}

static const char* const sdl_push_names[] = { "user", "quit", "keydown", "keyup", "mousemotion", "mousebuttondown",
    "mousebuttonup", "mousewheel", "fingerdown", "fingerup", "fingermotion", "fingercanceled" };
static const Uint32 sdl_push_types[] = { SDL_EVENT_USER, SDL_EVENT_QUIT, SDL_EVENT_KEY_DOWN, SDL_EVENT_KEY_UP,
    SDL_EVENT_MOUSE_MOTION, SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_EVENT_MOUSE_BUTTON_UP, SDL_EVENT_MOUSE_WHEEL,
    SDL_EVENT_FINGER_DOWN, SDL_EVENT_FINGER_UP, SDL_EVENT_FINGER_MOTION, SDL_EVENT_FINGER_CANCELED };
static const char* const sdl_push_keys[] = { "type", "code", "windowId", "key", "scancode", "mod", "repeat", "x", "y",
    "xrel", "yrel", "buttons", "button", "clicks", "touchId", "fingerId", "dx", "dy", "pressure" };

static const uint32_t sdl_push_allowed[] = { 0x7u, 0x1u, 0x7du, 0x7du, 0xf85u, 0x3185u, 0x3185u, 0x185u,
    0x7c185u, 0x7c185u, 0x7c185u, 0x7c185u };

static JSValue sdl_push_own(JSContext* ctx, JSValueConst ev, const char* name)
{
    JSPropertyDescriptor desc;
    JSAtom atom = JS_NewAtom(ctx, name);
    int r;
    if (atom == JS_ATOM_NULL)
        return JS_EXCEPTION;
    r = JS_GetOwnProperty(ctx, &desc, ev, atom);
    if (r < 0) {
        JS_FreeAtom(ctx, atom);
        return JS_EXCEPTION;
    }
    if (r == 0) {
        JS_FreeAtom(ctx, atom);
        return JS_UNDEFINED;
    }
    JS_FreeValue(ctx, desc.value);
    JS_FreeValue(ctx, desc.getter);
    JS_FreeValue(ctx, desc.setter);
    if (!(desc.flags & JS_PROP_ENUMERABLE)) {
        JS_FreeAtom(ctx, atom);
        return JS_UNDEFINED;
    }
    {
        JSValue v = JS_GetProperty(ctx, ev, atom);
        JS_FreeAtom(ctx, atom);
        return v;
    }
}

static int sdl_push_fields_fit(JSContext* ctx, JSValueConst ev, int kind)
{
    int i;
    for (i = 1; i < (int)countof(sdl_push_keys); i++) {
        JSValue v;
        if (sdl_push_allowed[kind] & (1u << i))
            continue;
        v = sdl_push_own(ctx, ev, sdl_push_keys[i]);
        if (JS_IsException(v))
            return -1;
        if (!JS_IsUndefined(v)) {
            JS_FreeValue(ctx, v);
            JS_ThrowTypeError(ctx, "dyna:sdl: pushEvent: '%s' is not a field of a %s event", sdl_push_keys[i], sdl_push_names[kind]);
            return -1;
        }
    }
    return 0;
}

static int sdl_push_field(JSContext* ctx, JSValueConst ev, const char* name, double lo, double hi, int integral,
    double* out)
{
    JSValue v = sdl_push_own(ctx, ev, name);
    double d;
    if (JS_IsException(v))
        return -1;
    if (JS_IsUndefined(v))
        return 0;
    if (JS_ToFloat64(ctx, &d, v)) {
        JS_FreeValue(ctx, v);
        return -1;
    }
    JS_FreeValue(ctx, v);
    if (!(d >= lo && d <= hi) || (integral && d != floor(d))) {
        JS_ThrowRangeError(ctx, "dyna:sdl: pushEvent %s must be %s in %.17g..%.17g", name,
            integral ? "an integer" : "a number", lo, hi);
        return -1;
    }
    *out = d;
    return 0;
}

static JSValue sdl_push_event(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    SDL_Event e;
    JSValue tv, rv;
    const char* t;
    double code = 0, win = 0, key = 0, sc = 0, mod = 0, x = 0, y = 0, xr = 0, yr = 0, bs = 0, b = 1, cl = 1;
    double tid = 1, fid = 1, dx = 0, dy = 0, pr = 1;
    int kind = -1, k, rep = 0;
    (void)this_val;
    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "dyna:sdl: pushEvent({ type, ... }) takes an event object");
    if (sdl_check_opts(ctx, argv[0], "pushEvent", sdl_push_keys, (int)countof(sdl_push_keys)))
        return JS_EXCEPTION;
    tv = sdl_push_own(ctx, argv[0], "type");
    if (JS_IsException(tv))
        return JS_EXCEPTION;
    if (!JS_IsString(tv)) {
        JS_FreeValue(ctx, tv);
        return JS_ThrowTypeError(ctx, "dyna:sdl: pushEvent: type must be a string (user, quit, keydown, keyup, mousemotion, mousebuttondown, mousebuttonup, mousewheel, fingerdown, fingerup, fingermotion, fingercanceled)");
    }
    t = JS_ToCString(ctx, tv);
    JS_FreeValue(ctx, tv);
    if (!t)
        return JS_EXCEPTION;
    for (k = 0; k < (int)countof(sdl_push_names); k++)
        if (!strcmp(t, sdl_push_names[k]))
            kind = k;
    if (kind < 0) {
        JS_ThrowRangeError(ctx, "dyna:sdl: pushEvent: unknown type '%s'", t);
        JS_FreeCString(ctx, t);
        return JS_EXCEPTION;
    }
    JS_FreeCString(ctx, t);
    if (sdl_push_fields_fit(ctx, argv[0], kind))
        return JS_EXCEPTION;
    if (sdl_push_field(ctx, argv[0], "code", INT32_MIN, INT32_MAX, 1, &code)
        || sdl_push_field(ctx, argv[0], "windowId", 0, 4294967295.0, 1, &win)
        || sdl_push_field(ctx, argv[0], "key", 0, 4294967295.0, 1, &key)
        || sdl_push_field(ctx, argv[0], "scancode", 0, SDL_SCANCODE_COUNT - 1, 1, &sc)
        || sdl_push_field(ctx, argv[0], "mod", 0, 65535, 1, &mod)
        || sdl_push_field(ctx, argv[0], "x", -1e7, 1e7, 0, &x)
        || sdl_push_field(ctx, argv[0], "y", -1e7, 1e7, 0, &y)
        || sdl_push_field(ctx, argv[0], "xrel", -1e7, 1e7, 0, &xr)
        || sdl_push_field(ctx, argv[0], "yrel", -1e7, 1e7, 0, &yr)
        || sdl_push_field(ctx, argv[0], "buttons", 0, 31, 1, &bs)
        || sdl_push_field(ctx, argv[0], "button", 1, 5, 1, &b)
        || sdl_push_field(ctx, argv[0], "clicks", 0, 255, 1, &cl)
        || sdl_push_field(ctx, argv[0], "touchId", 1, 9007199254740991.0, 1, &tid)
        || sdl_push_field(ctx, argv[0], "fingerId", 1, 9007199254740991.0, 1, &fid)
        || sdl_push_field(ctx, argv[0], "dx", -1, 1, 0, &dx)
        || sdl_push_field(ctx, argv[0], "dy", -1, 1, 0, &dy)
        || sdl_push_field(ctx, argv[0], "pressure", 0, 1, 0, &pr))
        return JS_EXCEPTION;
    rv = sdl_push_own(ctx, argv[0], "repeat");
    if (JS_IsException(rv))
        return JS_EXCEPTION;
    if (!JS_IsUndefined(rv) && !JS_IsBool(rv)) {
        JS_FreeValue(ctx, rv);
        return JS_ThrowTypeError(ctx, "dyna:sdl: pushEvent repeat must be a boolean");
    }
    rep = JS_ToBool(ctx, rv);
    JS_FreeValue(ctx, rv);
    if (kind >= 8 && kind <= 11) {
        double fx = x, fy = y;
        if (!(fx >= 0 && fx <= 1) || !(fy >= 0 && fy <= 1))
            return JS_ThrowRangeError(ctx, "dyna:sdl: pushEvent finger x and y are 0..1 of the window");
    }
    if (!(SDL_WasInit(SDL_INIT_EVENTS) & SDL_INIT_EVENTS))
        return JS_ThrowTypeError(ctx, "dyna:sdl: pushEvent() needs init() first (video or events)");
    SDL_zero(e);
    e.type = sdl_push_types[kind];
    e.common.timestamp = SDL_GetTicksNS();
    switch (kind) {
    case 0:
        e.user.code = (Sint32)code;
        e.user.windowID = (SDL_WindowID)win;
        break;
    case 2:
    case 3:
        e.key.windowID = (SDL_WindowID)win;
        e.key.key = (SDL_Keycode)key;
        e.key.scancode = (SDL_Scancode)sc;
        e.key.mod = (SDL_Keymod)mod;
        e.key.repeat = rep != 0;
        e.key.down = kind == 2;
        break;
    case 4:
        e.motion.windowID = (SDL_WindowID)win;
        e.motion.x = (float)x;
        e.motion.y = (float)y;
        e.motion.xrel = (float)xr;
        e.motion.yrel = (float)yr;
        e.motion.state = (SDL_MouseButtonFlags)bs;
        break;
    case 5:
    case 6:
        e.button.windowID = (SDL_WindowID)win;
        e.button.button = (Uint8)b;
        e.button.clicks = (Uint8)cl;
        e.button.down = kind == 5;
        e.button.x = (float)x;
        e.button.y = (float)y;
        break;
    case 7:
        e.wheel.windowID = (SDL_WindowID)win;
        e.wheel.x = (float)x;
        e.wheel.y = (float)y;
        e.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
        break;
    case 8:
    case 9:
    case 10:
    case 11:
        e.tfinger.windowID = (SDL_WindowID)win;
        e.tfinger.touchID = (SDL_TouchID)tid;
        e.tfinger.fingerID = (SDL_FingerID)fid;
        e.tfinger.x = (float)x;
        e.tfinger.y = (float)y;
        e.tfinger.dx = (float)dx;
        e.tfinger.dy = (float)dy;
        e.tfinger.pressure = (float)pr;
        break;
    default:
        break;
    }
    if (!SDL_PushEvent(&e))
        return sdl_throw(ctx, "SDL_PushEvent");
    return JS_UNDEFINED;
}

static JSValue sdl_key_name(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    double d;
    const char* s;
    (void)this_val;
    if (argc < 1 || JS_ToFloat64(ctx, &d, argv[0]))
        return argc < 1 ? JS_ThrowTypeError(ctx, "dyna:sdl: keyName(key) needs a keycode") : JS_EXCEPTION;
    if (!(d >= 0 && d <= UINT32_MAX) || d != floor(d))
        return JS_ThrowRangeError(ctx, "dyna:sdl: keyName: the keycode must be a non-negative 32-bit integer");
    s = SDL_GetKeyName((SDL_Keycode)d);
    if (s && !strcmp(s, "\xEF\xBF\xBD") && d != 0xFFFD)
        s = "";
    return JS_NewString(ctx, s ? s : "");
}

static JSValue sdl_key_from_name(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    const char* s;
    SDL_Keycode k;
    (void)this_val;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "dyna:sdl: keyFromName(name) needs a string");
    s = JS_ToCString(ctx, argv[0]);
    if (!s)
        return JS_EXCEPTION;
    k = SDL_GetKeyFromName(s);
    JS_FreeCString(ctx, s);
    return JS_NewFloat64(ctx, (double)k);
}

static JSValue sdl_mouse_state(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    float x = 0, y = 0;
    SDL_MouseButtonFlags b;
    JSValue o;
    (void)this_val;
    (void)argc;
    (void)argv;
    if (!sdl_need_video(ctx, "mouseState()"))
        return JS_EXCEPTION;
    b = SDL_GetMouseState(&x, &y);
    o = JS_NewObject(ctx);
    if (JS_IsException(o))
        return o;
    if (sdl_set_int(ctx, o, "x", (double)x) < 0 || sdl_set_int(ctx, o, "y", (double)y) < 0 || sdl_set_int(ctx, o, "buttons", b) < 0) {
        JS_FreeValue(ctx, o);
        return JS_EXCEPTION;
    }
    return o;
}

static JSValue sdl_set_hint(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    const char *k, *v;
    bool ok;
    (void)this_val;
    if (argc < 2 || !JS_IsString(argv[0]) || !JS_IsString(argv[1]))
        return JS_ThrowTypeError(ctx, "dyna:sdl: setHint(name, value) takes two strings");
    k = JS_ToCString(ctx, argv[0]);
    if (!k)
        return JS_EXCEPTION;
    v = JS_ToCString(ctx, argv[1]);
    if (!v) {
        JS_FreeCString(ctx, k);
        return JS_EXCEPTION;
    }
    ok = SDL_SetHint(k, v);
    JS_FreeCString(ctx, k);
    JS_FreeCString(ctx, v);
    return JS_NewBool(ctx, ok);
}

static JSValue sdl_clipboard(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    (void)this_val;
    if (!sdl_need_video(ctx, "the clipboard"))
        return JS_EXCEPTION;
    if (magic == 0) {
        char* s = SDL_GetClipboardText();
        JSValue r = JS_NewString(ctx, s ? s : "");
        SDL_free(s);
        return r;
    }
    if (magic == 1)
        return JS_NewBool(ctx, SDL_HasClipboardText());
    {
        const char* s;
        size_t n;
        bool ok;
        if (argc < 1 || !JS_IsString(argv[0]))
            return JS_ThrowTypeError(ctx, "dyna:sdl: setClipboardText(text) needs a string");
        s = JS_ToCStringLen(ctx, &n, argv[0]);
        if (!s)
            return JS_EXCEPTION;
        if (dyn_wtf8_has_surrogate(s, n)) {
            JS_FreeCString(ctx, s);
            return JS_ThrowTypeError(ctx, "dyna:sdl: clipboard text must be well-formed (it holds a lone surrogate)");
        }
        ok = SDL_SetClipboardText(s);
        JS_FreeCString(ctx, s);
        if (!ok)
            return sdl_throw(ctx, "SDL_SetClipboardText");
        return JS_UNDEFINED;
    }
}

static const char* const sdl_window_keys[] = { "resizable", "hidden", "borderless", "highDensity", "fullscreen", "alwaysOnTop" };

static JSValue sdl_window_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv)
{
    const char* title;
    size_t tlen;
    int w, h, b;
    SDL_WindowFlags flags = 0;
    sdl_win* nw;
    JSValueConst opts = argc > 3 ? argv[3] : JS_UNDEFINED;
    static const SDL_WindowFlags bits[] = { SDL_WINDOW_RESIZABLE, SDL_WINDOW_HIDDEN, SDL_WINDOW_BORDERLESS,
        SDL_WINDOW_HIGH_PIXEL_DENSITY, SDL_WINDOW_FULLSCREEN, SDL_WINDOW_ALWAYS_ON_TOP };
    size_t i;
    if (argc < 3 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "dyna:sdl: new Window(title, width, height, opts?)");
    if (sdl_dim(ctx, argv[1], "Window width", &w) || sdl_dim(ctx, argv[2], "Window height", &h))
        return JS_EXCEPTION;
    if (sdl_check_opts(ctx, opts, "Window", sdl_window_keys, (int)countof(sdl_window_keys)))
        return JS_EXCEPTION;
    for (i = 0; i < countof(sdl_window_keys); i++) {
        b = 0;
        if (sdl_opt_bool(ctx, opts, sdl_window_keys[i], &b))
            return JS_EXCEPTION;
        if (b)
            flags |= bits[i];
    }
    if (!sdl_need_video(ctx, "new Window()"))
        return JS_EXCEPTION;
    title = JS_ToCStringLen(ctx, &tlen, argv[0]);
    if (!title)
        return JS_EXCEPTION;
    if (dyn_wtf8_has_surrogate(title, tlen)) {
        JS_FreeCString(ctx, title);
        return JS_ThrowTypeError(ctx, "dyna:sdl: a window title must be well-formed text (it holds a lone surrogate)");
    }
    nw = dyn_nat_calloc(1, sizeof(*nw));
    if (!nw) {
        JS_FreeCString(ctx, title);
        return JS_ThrowOutOfMemory(ctx);
    }
    nw->w = SDL_CreateWindow(title, w, h, flags);
    JS_FreeCString(ctx, title);
    if (!nw->w) {
        dyn_nat_free(nw);
        return sdl_throw(ctx, "SDL_CreateWindow");
    }
    atomic_fetch_add(&sdl_live_windows, 1);
    return dyn_res_wrap(ctx, new_target, sdl_win_cid, nw, sdl_win_dispose);
}

static JSValue sdl_window_get(JSContext* ctx, JSValueConst this_val, int magic)
{
    sdl_win* w = sdl_win_of(ctx, this_val);
    int a = 0, b = 0;
    JSValue o;
    const char* s;
    if (!w)
        return JS_EXCEPTION;
    switch (magic) {
    case 0:
        return JS_NewFloat64(ctx, (double)SDL_GetWindowID(w->w));
    case 1:
        s = SDL_GetWindowTitle(w->w);
        return JS_NewString(ctx, s ? s : "");
    case 2:
        SDL_GetWindowSize(w->w, &a, &b);
        break;
    case 3:
        SDL_GetWindowSizeInPixels(w->w, &a, &b);
        break;
    case 4:
        SDL_GetWindowPosition(w->w, &a, &b);
        o = JS_NewObject(ctx);
        if (JS_IsException(o))
            return o;
        if (sdl_set_int(ctx, o, "x", a) < 0 || sdl_set_int(ctx, o, "y", b) < 0) {
            JS_FreeValue(ctx, o);
            return JS_EXCEPTION;
        }
        return o;
    case 5: {
        float sc = SDL_GetWindowDisplayScale(w->w);
        if (!(sc > 0))
            return sdl_throw(ctx, "SDL_GetWindowDisplayScale");
        return JS_NewFloat64(ctx, (double)sc);
    }
    case 6:
        return JS_NewBool(ctx, SDL_TextInputActive(w->w));
    default:
        return JS_UNDEFINED;
    }
    o = JS_NewObject(ctx);
    if (JS_IsException(o))
        return o;
    if (sdl_set_int(ctx, o, "width", a) < 0 || sdl_set_int(ctx, o, "height", b) < 0) {
        JS_FreeValue(ctx, o);
        return JS_EXCEPTION;
    }
    return o;
}

static JSValue sdl_window_set_title(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    const char* s;
    size_t n;
    sdl_win* w;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "dyna:sdl: setTitle(title) needs a string");
    s = JS_ToCStringLen(ctx, &n, argv[0]);
    if (!s)
        return JS_EXCEPTION;
    if (dyn_wtf8_has_surrogate(s, n)) {
        JS_FreeCString(ctx, s);
        return JS_ThrowTypeError(ctx, "dyna:sdl: a window title must be well-formed text (it holds a lone surrogate)");
    }
    w = sdl_win_of(ctx, this_val);
    if (!w || !SDL_SetWindowTitle(w->w, s)) {
        JS_FreeCString(ctx, s);
        return w ? sdl_throw(ctx, "SDL_SetWindowTitle") : JS_EXCEPTION;
    }
    JS_FreeCString(ctx, s);
    return JS_UNDEFINED;
}

static JSValue sdl_window_set2(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    static const char* const usage[] = { "setSize(width, height)", "setPosition(x, y)", "setMinimumSize(width, height)",
        "setMaximumSize(width, height)" };
    static const char* const calls[] = { "SDL_SetWindowSize", "SDL_SetWindowPosition", "SDL_SetWindowMinimumSize",
        "SDL_SetWindowMaximumSize" };
    int a, b;
    sdl_win* w;
    bool ok = false;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "dyna:sdl: %s", usage[magic]);
    if (magic == 0) {
        if (sdl_dim(ctx, argv[0], "width", &a) || sdl_dim(ctx, argv[1], "height", &b))
            return JS_EXCEPTION;
    } else if (magic == 1) {
        if (sdl_int_in(ctx, argv[0], "x", -SDL_MAX_DIM * 4, SDL_MAX_DIM * 4, &a)
            || sdl_int_in(ctx, argv[1], "y", -SDL_MAX_DIM * 4, SDL_MAX_DIM * 4, &b))
            return JS_EXCEPTION;
    } else if (sdl_int_in(ctx, argv[0], "width", 0, SDL_MAX_DIM, &a) || sdl_int_in(ctx, argv[1], "height", 0, SDL_MAX_DIM, &b)) {
        return JS_EXCEPTION;
    }
    w = sdl_win_of(ctx, this_val);
    if (!w)
        return JS_EXCEPTION;
    switch (magic) {
    case 0:
        ok = SDL_SetWindowSize(w->w, a, b);
        break;
    case 1:
        ok = SDL_SetWindowPosition(w->w, a, b);
        break;
    case 2:
        ok = SDL_SetWindowMinimumSize(w->w, a, b);
        break;
    case 3:
        ok = SDL_SetWindowMaximumSize(w->w, a, b);
        break;
    }
    if (!ok)
        return sdl_throw(ctx, calls[magic]);
    SDL_SyncWindow(w->w);
    return JS_UNDEFINED;
}

static JSValue sdl_window_act(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    sdl_win* w;
    int b = 0;
    bool ok = true;
    if (magic == 3) {
        if (argc < 1 || !JS_IsBool(argv[0]))
            return JS_ThrowTypeError(ctx, "dyna:sdl: setFullscreen(on) needs a boolean");
        b = JS_ToBool(ctx, argv[0]);
    }
    w = sdl_win_of(ctx, this_val);
    if (!w)
        return JS_EXCEPTION;
    switch (magic) {
    case 0:
        ok = SDL_ShowWindow(w->w);
        break;
    case 1:
        ok = SDL_HideWindow(w->w);
        break;
    case 2:
        ok = SDL_RaiseWindow(w->w);
        break;
    case 3:
        ok = SDL_SetWindowFullscreen(w->w, b);
        break;
    }
    if (!ok)
        return sdl_throw(ctx, "a window operation");
    return JS_UNDEFINED;
}

static const char* const sdl_renderer_keys[] = { "driver", "vsync" };

static JSValue sdl_renderer_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv)
{
    JSValueConst opts = argc > 1 ? argv[1] : JS_UNDEFINED;
    char driver[64] = "";
    int vsync = 0;
    sdl_win* w;
    sdl_ren* r;
    JSValue obj;
    if (sdl_check_opts(ctx, opts, "Renderer", sdl_renderer_keys, (int)countof(sdl_renderer_keys)))
        return JS_EXCEPTION;
    if (JS_IsObject(opts)) {
        JSValue dv = JS_GetPropertyStr(ctx, opts, "driver");
        if (JS_IsException(dv))
            return JS_EXCEPTION;
        if (!JS_IsUndefined(dv)) {
            const char* s;
            if (!JS_IsString(dv)) {
                JS_FreeValue(ctx, dv);
                return JS_ThrowTypeError(ctx, "dyna:sdl: Renderer driver must be a string");
            }
            s = JS_ToCString(ctx, dv);
            JS_FreeValue(ctx, dv);
            if (!s)
                return JS_EXCEPTION;
            {
                size_t dl = strlen(s);
                if (dl >= sizeof(driver)) {
                    JS_FreeCString(ctx, s);
                    return JS_ThrowRangeError(ctx, "dyna:sdl: Renderer driver name is too long");
                }
                memcpy(driver, s, dl + 1);
            }
            JS_FreeCString(ctx, s);
        }
        if (sdl_opt_bool(ctx, opts, "vsync", &vsync))
            return JS_EXCEPTION;
    }
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "dyna:sdl: new Renderer(window, opts?)");
    w = sdl_win_of(ctx, argv[0]);
    if (!w)
        return JS_EXCEPTION;
    if (w->ren)
        return JS_ThrowTypeError(ctx, "dyna:sdl: this window already has a Renderer (SDL allows one per window)");
    r = dyn_nat_calloc(1, sizeof(*r));
    if (!r)
        return JS_ThrowOutOfMemory(ctx);
    r->rt = JS_GetRuntime(ctx);
    r->win_obj = JS_UNDEFINED;
    r->r = SDL_CreateRenderer(w->w, driver[0] ? driver : NULL);
    if (!r->r) {
        dyn_nat_free(r);
        return sdl_throw(ctx, "SDL_CreateRenderer");
    }
    if (vsync)
        SDL_SetRenderVSync(r->r, 1);
    r->win = w;
    w->ren = r;
    r->win_obj = JS_DupValue(ctx, argv[0]);
    obj = dyn_res_wrap(ctx, new_target, sdl_ren_cid, r, sdl_ren_dispose);
    return obj;
}

static int sdl_color(JSContext* ctx, int argc, JSValueConst* argv, Uint8 c[4])
{
    int i, v;
    if (argc < 3)
        return JS_ThrowTypeError(ctx, "dyna:sdl: a colour is (r, g, b, a = 255)"), -1;
    for (i = 0; i < 4; i++) {
        if (i == 3 && (argc < 4 || JS_IsUndefined(argv[3]))) {
            c[3] = 255;
            break;
        }
        if (sdl_int_in(ctx, argv[i], "a colour channel", 0, 255, &v))
            return -1;
        c[i] = (Uint8)v;
    }
    return 0;
}

static JSValue sdl_ren_color(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    Uint8 c[4];
    sdl_ren* r;
    if (sdl_color(ctx, argc, argv, c))
        return JS_EXCEPTION;
    r = sdl_ren_of(ctx, this_val);
    if (!r)
        return JS_EXCEPTION;
    if (!SDL_SetRenderDrawColor(r->r, c[0], c[1], c[2], c[3]))
        return sdl_throw(ctx, "SDL_SetRenderDrawColor");
    return JS_UNDEFINED;
}

static JSValue sdl_ren_simple(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    sdl_ren* r = sdl_ren_of(ctx, this_val);
    bool ok;
    (void)argc;
    (void)argv;
    if (!r)
        return JS_EXCEPTION;
    ok = magic == 0 ? SDL_RenderClear(r->r) : SDL_RenderPresent(r->r);
    if (!ok)
        return sdl_throw(ctx, magic == 0 ? "SDL_RenderClear" : "SDL_RenderPresent");
    return JS_UNDEFINED;
}

static JSValue sdl_ren_draw(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    static const int need[] = { 4, 4, 4, 2 };
    static const char* const names[] = { "fillRect(x, y, width, height)", "drawRect(x, y, width, height)",
        "drawLine(x1, y1, x2, y2)", "drawPoint(x, y)" };
    float f[4];
    int i;
    sdl_ren* r;
    bool ok = false;
    if (argc < need[magic])
        return JS_ThrowTypeError(ctx, "dyna:sdl: %s", names[magic]);
    for (i = 0; i < need[magic]; i++)
        if (sdl_coord(ctx, argv[i], names[magic], &f[i]))
            return JS_EXCEPTION;
    r = sdl_ren_of(ctx, this_val);
    if (!r)
        return JS_EXCEPTION;
    switch (magic) {
    case 0:
    case 1: {
        SDL_FRect rc = { f[0], f[1], f[2], f[3] };
        ok = magic == 0 ? SDL_RenderFillRect(r->r, &rc) : SDL_RenderRect(r->r, &rc);
        break;
    }
    case 2:
        ok = SDL_RenderLine(r->r, f[0], f[1], f[2], f[3]);
        break;
    case 3:
        ok = SDL_RenderPoint(r->r, f[0], f[1]);
        break;
    }
    if (!ok)
        return sdl_throw(ctx, names[magic]);
    return JS_UNDEFINED;
}

static int sdl_rect_arg(JSContext* ctx, JSValueConst v, const char* what, float f[4], int* present)
{
    static const char* const k[] = { "x", "y", "width", "height" };
    int i;
    *present = 0;
    if (JS_IsUndefined(v) || JS_IsNull(v))
        return 0;
    if (!JS_IsObject(v)) {
        JS_ThrowTypeError(ctx, "dyna:sdl: %s must be { x, y, width, height }", what);
        return -1;
    }
    for (i = 0; i < 4; i++) {
        JSValue e = JS_GetPropertyStr(ctx, v, k[i]);
        int rc;
        if (JS_IsException(e))
            return -1;
        if (JS_IsUndefined(e)) {
            JS_ThrowTypeError(ctx, "dyna:sdl: %s needs %s", what, k[i]);
            return -1;
        }
        rc = sdl_coord(ctx, e, what, &f[i]);
        JS_FreeValue(ctx, e);
        if (rc)
            return -1;
    }
    if (f[2] < 0 || f[3] < 0) {
        JS_ThrowRangeError(ctx, "dyna:sdl: %s width and height must not be negative", what);
        return -1;
    }
    *present = 1;
    return 0;
}

static JSValue sdl_ren_read_pixels(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    float f[4];
    int have, row;
    SDL_Rect rc;
    SDL_Surface *s, *c;
    sdl_ren* r;
    uint8_t* buf;
    size_t len;
    JSValue o, u8;
    if (sdl_rect_arg(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, "readPixels(rect)", f, &have))
        return JS_EXCEPTION;
    r = sdl_ren_of(ctx, this_val);
    if (!r)
        return JS_EXCEPTION;
    rc.x = (int)f[0];
    rc.y = (int)f[1];
    rc.w = (int)f[2];
    rc.h = (int)f[3];
    s = SDL_RenderReadPixels(r->r, have ? &rc : NULL);
    if (!s)
        return sdl_throw(ctx, "SDL_RenderReadPixels");
    c = SDL_ConvertSurface(s, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(s);
    if (!c)
        return sdl_throw(ctx, "SDL_ConvertSurface");
    len = (size_t)c->w * (size_t)c->h * 4;
    buf = dyn_nat_malloc(len ? len : 1);
    if (!buf) {
        SDL_DestroySurface(c);
        return JS_ThrowOutOfMemory(ctx);
    }
    for (row = 0; row < c->h; row++)
        memcpy(buf + (size_t)row * (size_t)c->w * 4, (const uint8_t*)c->pixels + (size_t)row * (size_t)c->pitch, (size_t)c->w * 4);
    o = JS_NewObject(ctx);
    u8 = dyn_nat_u8_copy(ctx, buf, len);
    dyn_nat_free(buf);
    if (JS_IsException(o) || JS_IsException(u8)
        || sdl_set_int(ctx, o, "width", c->w) < 0 || sdl_set_int(ctx, o, "height", c->h) < 0) {
        SDL_DestroySurface(c);
        JS_FreeValue(ctx, o);
        JS_FreeValue(ctx, u8);
        return JS_EXCEPTION;
    }
    SDL_DestroySurface(c);
    if (JS_DefinePropertyValueStr(ctx, o, "pixels", u8, JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, o);
        return JS_EXCEPTION;
    }
    return o;
}

static JSValue sdl_ren_get(JSContext* ctx, JSValueConst this_val, int magic)
{
    sdl_ren* r = sdl_ren_of(ctx, this_val);
    int w = 0, h = 0;
    JSValue o;
    if (!r)
        return JS_EXCEPTION;
    if (magic == 0) {
        const char* s = SDL_GetRendererName(r->r);
        return JS_NewString(ctx, s ? s : "");
    }
    if (!SDL_GetCurrentRenderOutputSize(r->r, &w, &h))
        return sdl_throw(ctx, "SDL_GetCurrentRenderOutputSize");
    o = JS_NewObject(ctx);
    if (JS_IsException(o))
        return o;
    if (sdl_set_int(ctx, o, "width", w) < 0 || sdl_set_int(ctx, o, "height", h) < 0) {
        JS_FreeValue(ctx, o);
        return JS_EXCEPTION;
    }
    return o;
}

static JSValue sdl_ren_draw_texture(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    float sf[4], df[4];
    int hs, hd, has_opts = argc > 3 && !JS_IsUndefined(argv[3]), has_center = 0;
    double angle = 0;
    SDL_FPoint center = { 0, 0 };
    SDL_FlipMode flip = SDL_FLIP_NONE;
    sdl_ren* r;
    sdl_tex* t;
    SDL_FRect src, dst;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "dyna:sdl: drawTexture(texture, src?, dst?, { angle?, center?, flip? })");
    if (sdl_rect_arg(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, "drawTexture src", sf, &hs)
        || sdl_rect_arg(ctx, argc > 2 ? argv[2] : JS_UNDEFINED, "drawTexture dst", df, &hd)
        || (has_opts && sdl_draw_tex_opts(ctx, argv[3], &angle, &center, &has_center, &flip)))
        return JS_EXCEPTION;
    r = sdl_ren_of(ctx, this_val);
    if (!r)
        return JS_EXCEPTION;
    t = sdl_tex_of(ctx, argv[0]);
    if (!t)
        return JS_EXCEPTION;
    if (t->ren != r)
        return JS_ThrowTypeError(ctx, "dyna:sdl: drawTexture: the texture belongs to another renderer");
    src = (SDL_FRect) { sf[0], sf[1], sf[2], sf[3] };
    dst = (SDL_FRect) { df[0], df[1], df[2], df[3] };
    if (has_opts) {
        if (!SDL_RenderTextureRotated(r->r, t->t, hs ? &src : NULL, hd ? &dst : NULL, angle, has_center ? &center : NULL, flip))
            return sdl_throw(ctx, "SDL_RenderTextureRotated");
        return JS_UNDEFINED;
    }
    if (!SDL_RenderTexture(r->r, t->t, hs ? &src : NULL, hd ? &dst : NULL))
        return sdl_throw(ctx, "SDL_RenderTexture");
    return JS_UNDEFINED;
}

static const char* const sdl_texture_keys[] = { "target" };

static JSValue sdl_texture_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv)
{
    JSValueConst opts = argc > 3 ? argv[3] : JS_UNDEFINED;
    int w, h, target = 0;
    sdl_ren* r;
    sdl_tex* t;
    if (argc < 3)
        return JS_ThrowTypeError(ctx, "dyna:sdl: new Texture(renderer, width, height, opts?)");
    if (sdl_dim(ctx, argv[1], "Texture width", &w) || sdl_dim(ctx, argv[2], "Texture height", &h))
        return JS_EXCEPTION;
    if (sdl_check_opts(ctx, opts, "Texture", sdl_texture_keys, (int)countof(sdl_texture_keys))
        || sdl_opt_bool(ctx, opts, "target", &target))
        return JS_EXCEPTION;
    r = sdl_ren_of(ctx, argv[0]);
    if (!r)
        return JS_EXCEPTION;
    t = dyn_nat_calloc(1, sizeof(*t));
    if (!t)
        return JS_ThrowOutOfMemory(ctx);
    t->rt = JS_GetRuntime(ctx);
    t->ren_obj = JS_UNDEFINED;
    t->t = SDL_CreateTexture(r->r, SDL_PIXELFORMAT_RGBA32, target ? SDL_TEXTUREACCESS_TARGET : SDL_TEXTUREACCESS_STREAMING, w, h);
    if (!t->t) {
        dyn_nat_free(t);
        return sdl_throw(ctx, "SDL_CreateTexture");
    }
    SDL_SetTextureBlendMode(t->t, SDL_BLENDMODE_BLEND);
    t->w = w;
    t->h = h;
    t->target = target;
    t->ren = r;
    t->next = r->texs;
    r->texs = t;
    t->ren_obj = JS_DupValue(ctx, argv[0]);
    return dyn_res_wrap(ctx, new_target, sdl_tex_cid, t, sdl_tex_dispose);
}

static JSValue sdl_tex_get(JSContext* ctx, JSValueConst this_val, int magic)
{
    sdl_tex* t = sdl_tex_of(ctx, this_val);
    if (!t)
        return JS_EXCEPTION;
    if (magic == 2)
        return JS_NewBool(ctx, t->target);
    return JS_NewInt32(ctx, magic ? t->h : t->w);
}

static JSValue sdl_tex_update(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    float f[4];
    int have;
    SDL_Rect rc;
    size_t off, blen, bpe, ab, need;
    JSValue buf;
    uint8_t* base;
    sdl_tex* t;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "dyna:sdl: update(pixels, rect?) needs an RGBA Uint8Array");
    if (sdl_rect_arg(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, "update rect", f, &have))
        return JS_EXCEPTION;
    t = sdl_tex_of(ctx, this_val);
    if (!t)
        return JS_EXCEPTION;
    rc.x = have ? (int)f[0] : 0;
    rc.y = have ? (int)f[1] : 0;
    rc.w = have ? (int)f[2] : t->w;
    rc.h = have ? (int)f[3] : t->h;
    if (rc.w < 1 || rc.h < 1 || rc.x < 0 || rc.y < 0 || rc.x > t->w - rc.w || rc.y > t->h - rc.h)
        return JS_ThrowRangeError(ctx, "dyna:sdl: update rect must lie inside the %dx%d texture", t->w, t->h);
    buf = JS_GetTypedArrayBuffer(ctx, argv[0], &off, &blen, &bpe);
    if (JS_IsException(buf))
        return JS_EXCEPTION;
    base = JS_GetArrayBuffer(ctx, &ab, buf);
    JS_FreeValue(ctx, buf);
    if (!base)
        return JS_EXCEPTION;
    if (bpe != 1)
        return JS_ThrowTypeError(ctx, "dyna:sdl: update pixels must be a byte view (Uint8Array) of RGBA");
    need = (size_t)rc.w * (size_t)rc.h * 4;
    if (off > ab || blen > ab - off || blen < need)
        return JS_ThrowRangeError(ctx, "dyna:sdl: update needs %zu bytes of RGBA for a %dx%d area, got %zu", need, rc.w, rc.h, blen);
    if (!SDL_UpdateTexture(t->t, &rc, base + off, rc.w * 4))
        return sdl_throw(ctx, "SDL_UpdateTexture");
    return JS_UNDEFINED;
}

static int sdl_blend_arg(JSContext* ctx, int argc, JSValueConst* argv, const char* who, SDL_BlendMode* bm)
{
    static const struct {
        const char* name;
        SDL_BlendMode mode;
    } modes[] = {
        { "none", SDL_BLENDMODE_NONE },
        { "blend", SDL_BLENDMODE_BLEND },
        { "add", SDL_BLENDMODE_ADD },
        { "mod", SDL_BLENDMODE_MOD },
        { "mul", SDL_BLENDMODE_MUL },
    };
    const char* s;
    size_t i;
    if (argc < 1 || !JS_IsString(argv[0])) {
        JS_ThrowTypeError(ctx, "dyna:sdl: %s(\"none\" | \"blend\" | \"add\" | \"mod\" | \"mul\")", who);
        return -1;
    }
    s = JS_ToCString(ctx, argv[0]);
    if (!s)
        return -1;
    for (i = 0; i < countof(modes); i++)
        if (!strcmp(s, modes[i].name)) {
            *bm = modes[i].mode;
            JS_FreeCString(ctx, s);
            return 0;
        }
    JS_FreeCString(ctx, s);
    JS_ThrowRangeError(ctx, "dyna:sdl: unknown blend mode (none, blend, add, mod, mul)");
    return -1;
}

static JSValue sdl_tex_mod(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    sdl_tex* t;
    int v[3] = { 0, 0, 0 }, i, n = magic == 1 ? 3 : 1;
    bool ok = false;
    SDL_BlendMode bm = SDL_BLENDMODE_NONE;
    if (magic == 2) {
        if (sdl_blend_arg(ctx, argc, argv, "setBlendMode", &bm))
            return JS_EXCEPTION;
    } else {
        if (argc < n)
            return JS_ThrowTypeError(ctx, magic ? "dyna:sdl: setColorMod(r, g, b)" : "dyna:sdl: setAlphaMod(alpha)");
        for (i = 0; i < n; i++)
            if (sdl_int_in(ctx, argv[i], "a modulation channel", 0, 255, &v[i]))
                return JS_EXCEPTION;
    }
    t = sdl_tex_of(ctx, this_val);
    if (!t)
        return JS_EXCEPTION;
    if (magic == 0)
        ok = SDL_SetTextureAlphaMod(t->t, (Uint8)v[0]);
    else if (magic == 1)
        ok = SDL_SetTextureColorMod(t->t, (Uint8)v[0], (Uint8)v[1], (Uint8)v[2]);
    else
        ok = SDL_SetTextureBlendMode(t->t, bm);
    if (!ok)
        return sdl_throw(ctx, "a texture modulation");
    return JS_UNDEFINED;
}

static const char* const sdl_audio_keys[] = { "freq", "channels", "format", "recording", "device" };

static JSValue sdl_audio_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv)
{
    JSValueConst opts = argc > 0 ? argv[0] : JS_UNDEFINED;
    SDL_AudioSpec spec;
    int freq = 48000, ch = 2, is_float = 1, recording = 0;
    SDL_AudioDeviceID dev = 0;
    sdl_audio* a;
    JSValue v;
    if (!JS_IsObject(opts) && !JS_IsUndefined(opts))
        return JS_ThrowTypeError(ctx, "dyna:sdl: new AudioStream({ freq?, channels?, format?, recording?, device? })");
    if (sdl_check_opts(ctx, opts, "AudioStream", sdl_audio_keys, (int)countof(sdl_audio_keys)))
        return JS_EXCEPTION;
    if (JS_IsObject(opts)) {
        v = JS_GetPropertyStr(ctx, opts, "freq");
        if (JS_IsException(v))
            return JS_EXCEPTION;
        if (!JS_IsUndefined(v) && sdl_int_in(ctx, v, "AudioStream freq", 8000, 384000, &freq)) {
            JS_FreeValue(ctx, v);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, opts, "channels");
        if (JS_IsException(v))
            return JS_EXCEPTION;
        if (!JS_IsUndefined(v) && sdl_int_in(ctx, v, "AudioStream channels", 1, 8, &ch)) {
            JS_FreeValue(ctx, v);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, opts, "format");
        if (JS_IsException(v))
            return JS_EXCEPTION;
        if (!JS_IsUndefined(v)) {
            const char* s;
            if (!JS_IsString(v)) {
                JS_FreeValue(ctx, v);
                return JS_ThrowTypeError(ctx, "dyna:sdl: AudioStream format is \"f32\" or \"s16\"");
            }
            s = JS_ToCString(ctx, v);
            JS_FreeValue(ctx, v);
            if (!s)
                return JS_EXCEPTION;
            if (!strcmp(s, "f32"))
                is_float = 1;
            else if (!strcmp(s, "s16"))
                is_float = 0;
            else {
                JS_FreeCString(ctx, s);
                return JS_ThrowRangeError(ctx, "dyna:sdl: AudioStream format is \"f32\" or \"s16\"");
            }
            JS_FreeCString(ctx, s);
        }
        if (sdl_opt_bool(ctx, opts, "recording", &recording))
            return JS_EXCEPTION;
        v = JS_GetPropertyStr(ctx, opts, "device");
        if (JS_IsException(v))
            return JS_EXCEPTION;
        if (!JS_IsUndefined(v)) {
            double d;
            int rc = JS_ToFloat64(ctx, &d, v);
            JS_FreeValue(ctx, v);
            if (rc)
                return JS_EXCEPTION;
            if (!(d >= 1 && d <= 4294967295.0) || d != floor(d))
                return JS_ThrowRangeError(ctx, "dyna:sdl: AudioStream device must be an id from audioDevices()");
            dev = (SDL_AudioDeviceID)d;
        }
    }
    if (!(SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO))
        return JS_ThrowTypeError(ctx, "dyna:sdl: new AudioStream() needs init([\"audio\"]) first");
    if (dev && !SDL_GetAudioDeviceName(dev))
        return JS_ThrowRangeError(ctx, "dyna:sdl: AudioStream: no audio device has id %u (see audioDevices())", (unsigned)dev);
    if (dev && SDL_IsAudioDevicePlayback(dev) == !!recording)
        return JS_ThrowTypeError(ctx, "dyna:sdl: AudioStream device %u is a %s device but recording is %s", (unsigned)dev,
            recording ? "playback" : "recording", recording ? "true" : "false");
    a = dyn_nat_calloc(1, sizeof(*a));
    if (!a)
        return JS_ThrowOutOfMemory(ctx);
    spec.format = is_float ? SDL_AUDIO_F32 : SDL_AUDIO_S16;
    spec.channels = ch;
    spec.freq = freq;
    if (!dev)
        dev = recording ? SDL_AUDIO_DEVICE_DEFAULT_RECORDING : SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
    a->s = SDL_OpenAudioDeviceStream(dev, &spec, NULL, NULL);
    if (!a->s) {
        dyn_nat_free(a);
        return sdl_throw(ctx, "SDL_OpenAudioDeviceStream");
    }
    a->is_float = is_float;
    a->bytes_per_sample = is_float ? 4 : 2;
    a->channels = ch;
    a->recording = recording;
    atomic_fetch_add(&sdl_live_streams, 1);
    return dyn_res_wrap(ctx, new_target, sdl_audio_cid, a, sdl_audio_dispose);
}

static JSValue sdl_audio_put(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    sdl_audio* a;
    JSClassID cid;
    size_t off, blen, bpe, ab;
    JSValue buf;
    uint8_t* base;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "dyna:sdl: put(samples) needs a Float32Array (f32) or Int16Array (s16)");
    a = dyn_res_native(ctx, this_val, sdl_audio_cid);
    if (!a)
        return JS_EXCEPTION;
    if (a->recording)
        return JS_ThrowTypeError(ctx, "dyna:sdl: put() feeds a playback stream; a recording stream is read with get()");
    cid = JS_GetClassID(argv[0]);
    if (cid != (a->is_float ? sdl_f32_cid : sdl_i16_cid))
        return JS_ThrowTypeError(ctx, "dyna:sdl: this stream's format is %s, so put() takes a %s",
            a->is_float ? "f32" : "s16", a->is_float ? "Float32Array" : "Int16Array");
    buf = JS_GetTypedArrayBuffer(ctx, argv[0], &off, &blen, &bpe);
    if (JS_IsException(buf))
        return JS_EXCEPTION;
    base = JS_GetArrayBuffer(ctx, &ab, buf);
    JS_FreeValue(ctx, buf);
    if (!base)
        return JS_EXCEPTION;
    if (off > ab || blen > ab - off || blen > INT32_MAX)
        return JS_ThrowRangeError(ctx, "dyna:sdl: put: the samples do not fit one call");
    if (blen && !SDL_PutAudioStreamData(a->s, base + off, (int)blen))
        return sdl_throw(ctx, "SDL_PutAudioStreamData");
    return JS_UNDEFINED;
}

static JSValue sdl_audio_act(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    sdl_audio* a = dyn_res_native(ctx, this_val, sdl_audio_cid);
    bool ok = false;
    (void)argc;
    (void)argv;
    if (!a)
        return JS_EXCEPTION;
    switch (magic) {
    case 0:
        ok = SDL_ResumeAudioStreamDevice(a->s);
        break;
    case 1:
        ok = SDL_PauseAudioStreamDevice(a->s);
        break;
    case 2:
        ok = SDL_ClearAudioStream(a->s);
        break;
    case 3:
        ok = SDL_FlushAudioStream(a->s);
        break;
    }
    if (!ok)
        return sdl_throw(ctx, "an audio stream operation");
    return JS_UNDEFINED;
}

static JSValue sdl_audio_queued(JSContext* ctx, JSValueConst this_val)
{
    sdl_audio* a = dyn_res_native(ctx, this_val, sdl_audio_cid);
    int n;
    if (!a)
        return JS_EXCEPTION;
    n = SDL_GetAudioStreamQueued(a->s);
    if (n < 0)
        return sdl_throw(ctx, "SDL_GetAudioStreamQueued");
    return JS_NewInt32(ctx, n);
}

static JSValue sdl_audio_set_gain(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    double g;
    sdl_audio* a;
    if (argc < 1 || JS_ToFloat64(ctx, &g, argv[0]))
        return argc < 1 ? JS_ThrowTypeError(ctx, "dyna:sdl: setGain(gain)") : JS_EXCEPTION;
    if (!(g >= 0 && g <= 16))
        return JS_ThrowRangeError(ctx, "dyna:sdl: setGain: gain must be in 0..16");
    a = dyn_res_native(ctx, this_val, sdl_audio_cid);
    if (!a)
        return JS_EXCEPTION;
    if (!SDL_SetAudioStreamGain(a->s, (float)g))
        return sdl_throw(ctx, "SDL_SetAudioStreamGain");
    return JS_UNDEFINED;
}

static int sdl_typed_view(JSContext* ctx, JSValueConst v, JSClassID cid, size_t elem, const char* what, void** p, size_t* n)
{
    size_t off, blen, bpe, ab;
    JSValue buf;
    uint8_t* base;
    if (JS_GetClassID(v) != cid) {
        JS_ThrowTypeError(ctx, "dyna:sdl: %s", what);
        return -1;
    }
    buf = JS_GetTypedArrayBuffer(ctx, v, &off, &blen, &bpe);
    if (JS_IsException(buf))
        return -1;
    base = JS_GetArrayBuffer(ctx, &ab, buf);
    JS_FreeValue(ctx, buf);
    if (!base)
        return -1;
    if (off > ab || blen > ab - off) {
        JS_ThrowRangeError(ctx, "dyna:sdl: %s: the view lies outside its buffer", what);
        return -1;
    }
    *p = base + off;
    *n = blen / elem;
    return 0;
}

static int sdl_path_arg(JSContext* ctx, JSValueConst v, const char* who, const char** out)
{
    size_t len;
    const char* s;
    if (!JS_IsString(v)) {
        JS_ThrowTypeError(ctx, "dyna:sdl: %s needs a path string", who);
        return -1;
    }
    s = JS_ToCStringLen(ctx, &len, v);
    if (!s)
        return -1;
    if (strlen(s) != len) {
        JS_FreeCString(ctx, s);
        JS_ThrowTypeError(ctx, "dyna:sdl: %s: the path holds a NUL character", who);
        return -1;
    }
    *out = s;
    return 0;
}

static JSValue sdl_samples_array(JSContext* ctx, const uint8_t* bytes, size_t len, int is_float)
{
    JSValue ab = JS_NewArrayBufferCopy(ctx, bytes, len), ta;
    JSValueConst args[3];
    if (JS_IsException(ab))
        return ab;
    args[0] = ab;
    args[1] = JS_NewInt32(ctx, 0);
    args[2] = JS_NewFloat64(ctx, (double)(len / (is_float ? 4 : 2)));
    ta = JS_NewTypedArray(ctx, 3, args, is_float ? JS_TYPED_ARRAY_FLOAT32 : JS_TYPED_ARRAY_INT16);
    JS_FreeValue(ctx, ab);
    return ta;
}

static JSValue sdl_window_set_opacity(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    double o;
    sdl_win* w;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "dyna:sdl: setOpacity(opacity)");
    if (JS_ToFloat64(ctx, &o, argv[0]))
        return JS_EXCEPTION;
    if (!(o >= 0 && o <= 1))
        return JS_ThrowRangeError(ctx, "dyna:sdl: setOpacity: opacity must be in 0..1");
    w = sdl_win_of(ctx, this_val);
    if (!w)
        return JS_EXCEPTION;
    if (!SDL_SetWindowOpacity(w->w, (float)o))
        return sdl_throw(ctx, "SDL_SetWindowOpacity");
    return JS_UNDEFINED;
}

static JSValue sdl_window_text(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    sdl_win* w = sdl_win_of(ctx, this_val);
    (void)argc;
    (void)argv;
    if (!w)
        return JS_EXCEPTION;
    if (!(magic == 0 ? SDL_StartTextInput(w->w) : SDL_StopTextInput(w->w)))
        return sdl_throw(ctx, magic == 0 ? "SDL_StartTextInput" : "SDL_StopTextInput");
    return JS_UNDEFINED;
}

static JSValue sdl_window_text_area(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    float f[4];
    int have, cursor = 0;
    SDL_Rect rc;
    sdl_win* w;
    if (sdl_rect_arg(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, "setTextInputArea(rect)", f, &have))
        return JS_EXCEPTION;
    if (argc > 1 && !JS_IsUndefined(argv[1]) && sdl_int_in(ctx, argv[1], "setTextInputArea cursor", 0, SDL_MAX_DIM * 4, &cursor))
        return JS_EXCEPTION;
    w = sdl_win_of(ctx, this_val);
    if (!w)
        return JS_EXCEPTION;
    rc = (SDL_Rect) { (int)f[0], (int)f[1], (int)f[2], (int)f[3] };
    if (!SDL_SetTextInputArea(w->w, have ? &rc : NULL, cursor))
        return sdl_throw(ctx, "SDL_SetTextInputArea");
    return JS_UNDEFINED;
}

static JSValue sdl_ren_set_target(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    JSValueConst v = argc > 0 ? argv[0] : JS_UNDEFINED;
    sdl_tex* t = NULL;
    sdl_ren* r;
    if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
        t = sdl_tex_of(ctx, v);
        if (!t)
            return JS_EXCEPTION;
        if (!t->target)
            return JS_ThrowTypeError(ctx, "dyna:sdl: setTarget: the texture was not created with { target: true }");
    }
    r = sdl_ren_of(ctx, this_val);
    if (!r)
        return JS_EXCEPTION;
    if (t && t->ren != r)
        return JS_ThrowTypeError(ctx, "dyna:sdl: setTarget: the texture belongs to another renderer");
    if (!SDL_SetRenderTarget(r->r, t ? t->t : NULL))
        return sdl_throw(ctx, "SDL_SetRenderTarget");
    return JS_UNDEFINED;
}

static JSValue sdl_ren_rect_state(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    float f[4];
    int have;
    SDL_Rect rc;
    sdl_ren* r;
    bool ok;
    if (sdl_rect_arg(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, magic ? "setViewport(rect)" : "setClipRect(rect)", f, &have))
        return JS_EXCEPTION;
    r = sdl_ren_of(ctx, this_val);
    if (!r)
        return JS_EXCEPTION;
    rc = (SDL_Rect) { (int)f[0], (int)f[1], (int)f[2], (int)f[3] };
    ok = magic ? SDL_SetRenderViewport(r->r, have ? &rc : NULL) : SDL_SetRenderClipRect(r->r, have ? &rc : NULL);
    if (!ok)
        return sdl_throw(ctx, magic ? "SDL_SetRenderViewport" : "SDL_SetRenderClipRect");
    return JS_UNDEFINED;
}

static JSValue sdl_ren_set_scale(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    double s[2];
    int i;
    sdl_ren* r;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "dyna:sdl: setScale(scaleX, scaleY)");
    for (i = 0; i < 2; i++) {
        if (JS_ToFloat64(ctx, &s[i], argv[i]))
            return JS_EXCEPTION;
        if (!(s[i] > 0 && s[i] <= 1024))
            return JS_ThrowRangeError(ctx, "dyna:sdl: setScale factors must be in (0, 1024]");
    }
    r = sdl_ren_of(ctx, this_val);
    if (!r)
        return JS_EXCEPTION;
    if (!SDL_SetRenderScale(r->r, (float)s[0], (float)s[1]))
        return sdl_throw(ctx, "SDL_SetRenderScale");
    return JS_UNDEFINED;
}

static JSValue sdl_ren_set_blend(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    SDL_BlendMode bm = SDL_BLENDMODE_NONE;
    sdl_ren* r;
    if (sdl_blend_arg(ctx, argc, argv, "setBlendMode", &bm))
        return JS_EXCEPTION;
    r = sdl_ren_of(ctx, this_val);
    if (!r)
        return JS_EXCEPTION;
    if (!SDL_SetRenderDrawBlendMode(r->r, bm))
        return sdl_throw(ctx, "SDL_SetRenderDrawBlendMode");
    return JS_UNDEFINED;
}

static JSValue sdl_ren_set_vsync(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    sdl_ren* r;
    int on;
    if (argc < 1 || !JS_IsBool(argv[0]))
        return JS_ThrowTypeError(ctx, "dyna:sdl: setVSync(on) needs a boolean");
    on = JS_ToBool(ctx, argv[0]);
    r = sdl_ren_of(ctx, this_val);
    if (!r)
        return JS_EXCEPTION;
    if (!SDL_SetRenderVSync(r->r, on ? 1 : SDL_RENDERER_VSYNC_DISABLED))
        return sdl_throw(ctx, "SDL_SetRenderVSync");
    return JS_UNDEFINED;
}

static JSValue sdl_ren_geometry(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    float* v = NULL;
    int32_t* idx = NULL;
    size_t nf = 0, ni = 0, nv, i, count;
    sdl_ren* r;
    sdl_tex* t = NULL;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "dyna:sdl: geometry(vertices, indices?, texture?)");
    if (sdl_typed_view(ctx, argv[0], sdl_f32_cid, 4, "geometry vertices must be a Float32Array of x, y, r, g, b, a, u, v per vertex", (void**)&v, &nf))
        return JS_EXCEPTION;
    if (nf % 8)
        return JS_ThrowRangeError(ctx, "dyna:sdl: geometry vertices hold 8 floats per vertex, got %zu floats", nf);
    nv = nf / 8;
    if (nv > SDL_MAX_VERTICES)
        return JS_ThrowRangeError(ctx, "dyna:sdl: geometry takes at most %d vertices", SDL_MAX_VERTICES);
    for (i = 0; i < nf; i++)
        if (!isfinite(v[i]) || fabsf(v[i]) > 1e7f)
            return JS_ThrowRangeError(ctx, "dyna:sdl: geometry vertex %zu holds a value that is not a finite number within +-1e7", i / 8);
    if (argc > 1 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
        if (sdl_typed_view(ctx, argv[1], sdl_i32_cid, 4, "geometry indices must be an Int32Array", (void**)&idx, &ni))
            return JS_EXCEPTION;
        if (ni > (size_t)SDL_MAX_VERTICES * 3)
            return JS_ThrowRangeError(ctx, "dyna:sdl: geometry takes at most %d indices", SDL_MAX_VERTICES * 3);
        for (i = 0; i < ni; i++)
            if (idx[i] < 0 || (size_t)idx[i] >= nv)
                return JS_ThrowRangeError(ctx, "dyna:sdl: geometry index %zu is %d, outside the %zu vertices", i, (int)idx[i], nv);
    }
    count = idx ? ni : nv;
    if (count % 3)
        return JS_ThrowRangeError(ctx, "dyna:sdl: geometry draws triangles, so the %s count must be a multiple of 3", idx ? "index" : "vertex");
    if (argc > 2 && !JS_IsUndefined(argv[2]) && !JS_IsNull(argv[2])) {
        t = sdl_tex_of(ctx, argv[2]);
        if (!t)
            return JS_EXCEPTION;
    }
    r = sdl_ren_of(ctx, this_val);
    if (!r)
        return JS_EXCEPTION;
    if (t && t->ren != r)
        return JS_ThrowTypeError(ctx, "dyna:sdl: geometry: the texture belongs to another renderer");
    if (!count)
        return JS_UNDEFINED;
    if (!SDL_RenderGeometryRaw(r->r, t ? t->t : NULL, v, 32, (const SDL_FColor*)(void*)(v + 2), 32, v + 6, 32,
            (int)nv, idx, idx ? (int)ni : 0, 4))
        return sdl_throw(ctx, "SDL_RenderGeometryRaw");
    return JS_UNDEFINED;
}

static JSValue sdl_load_texture(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    const char* path;
    SDL_Surface *s, *c;
    sdl_ren* r;
    sdl_tex* t;
    (void)this_val;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "dyna:sdl: loadTexture(renderer, path)");
    if (sdl_path_arg(ctx, argv[1], "loadTexture(renderer, path)", &path))
        return JS_EXCEPTION;
    r = sdl_ren_of(ctx, argv[0]);
    if (!r) {
        JS_FreeCString(ctx, path);
        return JS_EXCEPTION;
    }
    s = SDL_LoadSurface(path);
    JS_FreeCString(ctx, path);
    if (!s)
        return sdl_throw(ctx, "SDL_LoadSurface");
    if (s->w < 1 || s->h < 1 || s->w > SDL_MAX_DIM || s->h > SDL_MAX_DIM) {
        int sw = s->w, sh = s->h;
        SDL_DestroySurface(s);
        return JS_ThrowRangeError(ctx, "dyna:sdl: loadTexture: a %dx%d image is outside 1..%d per side", sw, sh, SDL_MAX_DIM);
    }
    c = SDL_ConvertSurface(s, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(s);
    if (!c)
        return sdl_throw(ctx, "SDL_ConvertSurface");
    t = dyn_nat_calloc(1, sizeof(*t));
    if (!t) {
        SDL_DestroySurface(c);
        return JS_ThrowOutOfMemory(ctx);
    }
    t->rt = JS_GetRuntime(ctx);
    t->ren_obj = JS_UNDEFINED;
    t->t = SDL_CreateTexture(r->r, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, c->w, c->h);
    if (!t->t) {
        SDL_DestroySurface(c);
        dyn_nat_free(t);
        return sdl_throw(ctx, "SDL_CreateTexture");
    }
    if (!SDL_UpdateTexture(t->t, NULL, c->pixels, c->pitch)) {
        SDL_DestroyTexture(t->t);
        SDL_DestroySurface(c);
        dyn_nat_free(t);
        return sdl_throw(ctx, "SDL_UpdateTexture");
    }
    SDL_SetTextureBlendMode(t->t, SDL_BLENDMODE_BLEND);
    t->w = c->w;
    t->h = c->h;
    SDL_DestroySurface(c);
    t->ren = r;
    t->next = r->texs;
    r->texs = t;
    t->ren_obj = JS_DupValue(ctx, argv[0]);
    return dyn_res_wrap(ctx, JS_UNDEFINED, sdl_tex_cid, t, sdl_tex_dispose);
}

static JSValue sdl_audio_get(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    sdl_audio* a;
    int max = SDL_MAX_GET_SAMPLES, avail, n, got, frame;
    uint8_t* buf;
    JSValue ta;
    if (argc > 0 && !JS_IsUndefined(argv[0]) && sdl_int_in(ctx, argv[0], "get(maxSamples)", 1, SDL_MAX_GET_SAMPLES, &max))
        return JS_EXCEPTION;
    a = dyn_res_native(ctx, this_val, sdl_audio_cid);
    if (!a)
        return JS_EXCEPTION;
    if (!a->recording)
        return JS_ThrowTypeError(ctx, "dyna:sdl: get() reads a recording stream; a playback stream is fed with put()");
    avail = SDL_GetAudioStreamAvailable(a->s);
    if (avail < 0)
        return sdl_throw(ctx, "SDL_GetAudioStreamAvailable");
    frame = a->channels;
    n = avail / a->bytes_per_sample;
    if (n > max)
        n = max;
    n -= n % frame;
    buf = dyn_nat_malloc(n ? (size_t)n * (size_t)a->bytes_per_sample : 1);
    if (!buf)
        return JS_ThrowOutOfMemory(ctx);
    got = n ? SDL_GetAudioStreamData(a->s, buf, n * a->bytes_per_sample) : 0;
    if (got < 0) {
        dyn_nat_free(buf);
        return sdl_throw(ctx, "SDL_GetAudioStreamData");
    }
    ta = sdl_samples_array(ctx, buf, (size_t)got - (size_t)got % (size_t)a->bytes_per_sample, a->is_float);
    dyn_nat_free(buf);
    return ta;
}

static JSValue sdl_audio_flag(JSContext* ctx, JSValueConst this_val, int magic)
{
    sdl_audio* a = dyn_res_native(ctx, this_val, sdl_audio_cid);
    int n;
    if (!a)
        return JS_EXCEPTION;
    if (magic == 1)
        return JS_NewBool(ctx, a->recording);
    n = SDL_GetAudioStreamAvailable(a->s);
    if (n < 0)
        return sdl_throw(ctx, "SDL_GetAudioStreamAvailable");
    return JS_NewInt32(ctx, n);
}

static JSValue sdl_audio_devices(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    static const char* const keys[] = { "recording" };
    JSValueConst opts = argc > 0 ? argv[0] : JS_UNDEFINED;
    int recording = 0, n = 0, i;
    SDL_AudioDeviceID* ids;
    JSValue arr;
    (void)this_val;
    if (sdl_check_opts(ctx, opts, "audioDevices", keys, (int)countof(keys)) || sdl_opt_bool(ctx, opts, "recording", &recording))
        return JS_EXCEPTION;
    if (!(SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO))
        return JS_ThrowTypeError(ctx, "dyna:sdl: audioDevices() needs init([\"audio\"]) first");
    ids = recording ? SDL_GetAudioRecordingDevices(&n) : SDL_GetAudioPlaybackDevices(&n);
    if (!ids)
        return sdl_throw(ctx, recording ? "SDL_GetAudioRecordingDevices" : "SDL_GetAudioPlaybackDevices");
    arr = JS_NewArray(ctx);
    for (i = 0; i < n && !JS_IsException(arr); i++) {
        const char* name = SDL_GetAudioDeviceName(ids[i]);
        JSValue o = JS_NewObject(ctx);
        if (JS_IsException(o)) {
            JS_FreeValue(ctx, arr);
            arr = JS_EXCEPTION;
            break;
        }
        if (sdl_set_int(ctx, o, "id", (double)ids[i]) < 0
            || JS_DefinePropertyValueStr(ctx, o, "name", JS_NewString(ctx, name ? name : ""), JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, o);
            JS_FreeValue(ctx, arr);
            arr = JS_EXCEPTION;
            break;
        }
        if (JS_DefinePropertyValueUint32(ctx, arr, (uint32_t)i, o, JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, arr);
            arr = JS_EXCEPTION;
        }
    }
    SDL_free(ids);
    return arr;
}

static JSValue sdl_load_wav(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    const char* path;
    SDL_AudioSpec src, dst;
    Uint8 *raw = NULL, *cvt = NULL;
    Uint32 rawlen = 0;
    int cvtlen = 0;
    JSValue o, samples;
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "dyna:sdl: loadWAV(path)");
    if (sdl_path_arg(ctx, argv[0], "loadWAV(path)", &path))
        return JS_EXCEPTION;
    if (!SDL_LoadWAV(path, &src, &raw, &rawlen)) {
        JS_FreeCString(ctx, path);
        return sdl_throw(ctx, "SDL_LoadWAV");
    }
    JS_FreeCString(ctx, path);
    if (rawlen > SDL_MAX_WAV_BYTES) {
        SDL_free(raw);
        return JS_ThrowRangeError(ctx, "dyna:sdl: loadWAV: the decoded audio exceeds %d bytes", SDL_MAX_WAV_BYTES);
    }
    dst = src;
    dst.format = SDL_AUDIO_F32;
    if (!SDL_ConvertAudioSamples(&src, raw, (int)rawlen, &dst, &cvt, &cvtlen)) {
        SDL_free(raw);
        return sdl_throw(ctx, "SDL_ConvertAudioSamples");
    }
    SDL_free(raw);
    samples = sdl_samples_array(ctx, cvt, (size_t)cvtlen - (size_t)cvtlen % 4, 1);
    SDL_free(cvt);
    if (JS_IsException(samples))
        return samples;
    o = JS_NewObject(ctx);
    if (JS_IsException(o) || sdl_set_int(ctx, o, "freq", src.freq) < 0 || sdl_set_int(ctx, o, "channels", src.channels) < 0) {
        JS_FreeValue(ctx, o);
        JS_FreeValue(ctx, samples);
        return JS_EXCEPTION;
    }
    if (JS_DefinePropertyValueStr(ctx, o, "samples", samples, JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, o);
        return JS_EXCEPTION;
    }
    return o;
}

static const JSCFunctionListEntry sdl_window_proto[] = {
    JS_CGETSET_MAGIC_DEF("id", sdl_window_get, NULL, 0),
    JS_CGETSET_MAGIC_DEF("title", sdl_window_get, NULL, 1),
    JS_CGETSET_MAGIC_DEF("size", sdl_window_get, NULL, 2),
    JS_CGETSET_MAGIC_DEF("pixelSize", sdl_window_get, NULL, 3),
    JS_CGETSET_MAGIC_DEF("position", sdl_window_get, NULL, 4),
    JS_CFUNC_DEF("setTitle", 1, sdl_window_set_title),
    JS_CFUNC_MAGIC_DEF("setSize", 2, sdl_window_set2, 0),
    JS_CFUNC_MAGIC_DEF("setPosition", 2, sdl_window_set2, 1),
    JS_CFUNC_MAGIC_DEF("show", 0, sdl_window_act, 0),
    JS_CFUNC_MAGIC_DEF("hide", 0, sdl_window_act, 1),
    JS_CFUNC_MAGIC_DEF("raise", 0, sdl_window_act, 2),
    JS_CFUNC_MAGIC_DEF("setFullscreen", 1, sdl_window_act, 3),
    JS_CGETSET_MAGIC_DEF("displayScale", sdl_window_get, NULL, 5),
    JS_CGETSET_MAGIC_DEF("textInputActive", sdl_window_get, NULL, 6),
    JS_CFUNC_MAGIC_DEF("setMinimumSize", 2, sdl_window_set2, 2),
    JS_CFUNC_MAGIC_DEF("setMaximumSize", 2, sdl_window_set2, 3),
    JS_CFUNC_DEF("setOpacity", 1, sdl_window_set_opacity),
    JS_CFUNC_MAGIC_DEF("startTextInput", 0, sdl_window_text, 0),
    JS_CFUNC_MAGIC_DEF("stopTextInput", 0, sdl_window_text, 1),
    JS_CFUNC_DEF("setTextInputArea", 2, sdl_window_text_area),
    JS_CGETSET_DEF("state", sdl_window_state, NULL),
    JS_CFUNC_MAGIC_DEF("maximize", 0, sdl_window_act2, 0),
    JS_CFUNC_MAGIC_DEF("minimize", 0, sdl_window_act2, 1),
    JS_CFUNC_MAGIC_DEF("restore", 0, sdl_window_act2, 2),
    JS_CFUNC_MAGIC_DEF("flash", 0, sdl_window_act2, 3),
    JS_CFUNC_MAGIC_DEF("sync", 0, sdl_window_act2, 4),
    JS_CFUNC_MAGIC_DEF("setResizable", 1, sdl_window_flag, 0),
    JS_CFUNC_MAGIC_DEF("setBordered", 1, sdl_window_flag, 1),
    JS_CFUNC_MAGIC_DEF("setAlwaysOnTop", 1, sdl_window_flag, 2),
    JS_CFUNC_MAGIC_DEF("setMouseGrab", 1, sdl_window_flag, 3),
    JS_CFUNC_MAGIC_DEF("setRelativeMouseMode", 1, sdl_window_flag, 4),
    JS_CFUNC_MAGIC_DEF("setKeyboardGrab", 1, sdl_window_flag, 5),
};

static const JSCFunctionListEntry sdl_renderer_proto[] = {
    JS_CGETSET_MAGIC_DEF("name", sdl_ren_get, NULL, 0),
    JS_CGETSET_MAGIC_DEF("outputSize", sdl_ren_get, NULL, 1),
    JS_CFUNC_DEF("setDrawColor", 4, sdl_ren_color),
    JS_CFUNC_MAGIC_DEF("clear", 0, sdl_ren_simple, 0),
    JS_CFUNC_MAGIC_DEF("present", 0, sdl_ren_simple, 1),
    JS_CFUNC_MAGIC_DEF("fillRect", 4, sdl_ren_draw, 0),
    JS_CFUNC_MAGIC_DEF("drawRect", 4, sdl_ren_draw, 1),
    JS_CFUNC_MAGIC_DEF("drawLine", 4, sdl_ren_draw, 2),
    JS_CFUNC_MAGIC_DEF("drawPoint", 2, sdl_ren_draw, 3),
    JS_CFUNC_DEF("drawTexture", 3, sdl_ren_draw_texture),
    JS_CFUNC_DEF("readPixels", 1, sdl_ren_read_pixels),
    JS_CFUNC_DEF("setTarget", 1, sdl_ren_set_target),
    JS_CFUNC_MAGIC_DEF("setClipRect", 1, sdl_ren_rect_state, 0),
    JS_CFUNC_MAGIC_DEF("setViewport", 1, sdl_ren_rect_state, 1),
    JS_CFUNC_DEF("setScale", 2, sdl_ren_set_scale),
    JS_CFUNC_DEF("setBlendMode", 1, sdl_ren_set_blend),
    JS_CFUNC_DEF("setVSync", 1, sdl_ren_set_vsync),
    JS_CFUNC_DEF("geometry", 3, sdl_ren_geometry),
    JS_CFUNC_DEF("debugText", 3, sdl_ren_debug_text),
    JS_CFUNC_DEF("setLogicalPresentation", 2, sdl_ren_logical),
    JS_CFUNC_MAGIC_DEF("windowToRender", 2, sdl_ren_coords, 0),
    JS_CFUNC_MAGIC_DEF("renderToWindow", 2, sdl_ren_coords, 1),
};

static const JSCFunctionListEntry sdl_texture_proto[] = {
    JS_CGETSET_MAGIC_DEF("width", sdl_tex_get, NULL, 0),
    JS_CGETSET_MAGIC_DEF("height", sdl_tex_get, NULL, 1),
    JS_CGETSET_MAGIC_DEF("target", sdl_tex_get, NULL, 2),
    JS_CFUNC_DEF("update", 2, sdl_tex_update),
    JS_CFUNC_MAGIC_DEF("setAlphaMod", 1, sdl_tex_mod, 0),
    JS_CFUNC_MAGIC_DEF("setColorMod", 3, sdl_tex_mod, 1),
    JS_CFUNC_MAGIC_DEF("setBlendMode", 1, sdl_tex_mod, 2),
};

static const JSCFunctionListEntry sdl_audio_proto[] = {
    JS_CGETSET_DEF("queued", sdl_audio_queued, NULL),
    JS_CGETSET_MAGIC_DEF("available", sdl_audio_flag, NULL, 0),
    JS_CGETSET_MAGIC_DEF("recording", sdl_audio_flag, NULL, 1),
    JS_CFUNC_DEF("get", 1, sdl_audio_get),
    JS_CFUNC_DEF("put", 1, sdl_audio_put),
    JS_CFUNC_MAGIC_DEF("resume", 0, sdl_audio_act, 0),
    JS_CFUNC_MAGIC_DEF("pause", 0, sdl_audio_act, 1),
    JS_CFUNC_MAGIC_DEF("clear", 0, sdl_audio_act, 2),
    JS_CFUNC_MAGIC_DEF("flush", 0, sdl_audio_act, 3),
    JS_CFUNC_DEF("setGain", 1, sdl_audio_set_gain),
};

static const JSCFunctionListEntry sdl_pad_proto[] = {
    JS_CGETSET_MAGIC_DEF("id", sdl_pad_get, NULL, 0),
    JS_CGETSET_MAGIC_DEF("name", sdl_pad_get, NULL, 1),
    JS_CGETSET_MAGIC_DEF("type", sdl_pad_get, NULL, 2),
    JS_CGETSET_MAGIC_DEF("connected", sdl_pad_get, NULL, 3),
    JS_CGETSET_MAGIC_DEF("virtual", sdl_pad_get, NULL, 4),
    JS_CFUNC_MAGIC_DEF("button", 1, sdl_pad_read, 0),
    JS_CFUNC_MAGIC_DEF("axis", 1, sdl_pad_read, 1),
    JS_CFUNC_DEF("rumble", 3, sdl_pad_rumble),
    JS_CFUNC_MAGIC_DEF("setVirtualButton", 2, sdl_pad_set_virtual, 0),
    JS_CFUNC_MAGIC_DEF("setVirtualAxis", 2, sdl_pad_set_virtual, 1),
};

static const JSCFunctionListEntry sdl_funcs[] = {
    JS_CFUNC_MAGIC_DEF("showCursor", 0, sdl_cursor_fn, 0),
    JS_CFUNC_MAGIC_DEF("hideCursor", 0, sdl_cursor_fn, 1),
    JS_CFUNC_MAGIC_DEF("cursorVisible", 0, sdl_cursor_fn, 2),
    JS_CFUNC_MAGIC_DEF("setCursor", 1, sdl_cursor_fn, 3),
    JS_CFUNC_DEF("gamepads", 0, sdl_gamepads),
    JS_CFUNC_MAGIC_DEF("attachVirtualGamepad", 0, sdl_virtual_pad, 0),
    JS_CFUNC_MAGIC_DEF("detachVirtualGamepad", 1, sdl_virtual_pad, 1),
    JS_CFUNC_MAGIC_DEF("powerInfo", 0, sdl_sysinfo, 0),
    JS_CFUNC_MAGIC_DEF("systemTheme", 0, sdl_sysinfo, 1),
    JS_CFUNC_MAGIC_DEF("preferredLocales", 0, sdl_sysinfo, 2),
    JS_CFUNC_MAGIC_DEF("cpuCount", 0, sdl_sysinfo, 3),
    JS_CFUNC_MAGIC_DEF("systemRAM", 0, sdl_sysinfo, 4),
    JS_CFUNC_MAGIC_DEF("basePath", 0, sdl_sysinfo, 5),
    JS_CFUNC_DEF("init", 1, sdl_init),
    JS_CFUNC_DEF("quit", 0, sdl_quit),
    JS_CFUNC_DEF("wasInit", 0, sdl_was_init),
    JS_CFUNC_DEF("version", 0, sdl_version),
    JS_CFUNC_MAGIC_DEF("platform", 0, sdl_str_fn, 0),
    JS_CFUNC_MAGIC_DEF("getError", 0, sdl_str_fn, 1),
    JS_CFUNC_MAGIC_DEF("currentVideoDriver", 0, sdl_str_fn, 2),
    JS_CFUNC_MAGIC_DEF("currentAudioDriver", 0, sdl_str_fn, 3),
    JS_CFUNC_MAGIC_DEF("videoDrivers", 0, sdl_drivers, 0),
    JS_CFUNC_MAGIC_DEF("audioDrivers", 0, sdl_drivers, 1),
    JS_CFUNC_DEF("ticks", 0, sdl_ticks),
    JS_CFUNC_DEF("displays", 0, sdl_displays),
    JS_CFUNC_DEF("pollEvent", 0, sdl_poll_event),
    JS_CFUNC_DEF("pollEvents", 1, sdl_poll_events),
    JS_CFUNC_DEF("pushEvent", 1, sdl_push_event),
    JS_CFUNC_DEF("keyName", 1, sdl_key_name),
    JS_CFUNC_DEF("keyFromName", 1, sdl_key_from_name),
    JS_CFUNC_DEF("mouseState", 0, sdl_mouse_state),
    JS_CFUNC_DEF("setHint", 2, sdl_set_hint),
    JS_CFUNC_MAGIC_DEF("getClipboardText", 0, sdl_clipboard, 0),
    JS_CFUNC_MAGIC_DEF("hasClipboardText", 0, sdl_clipboard, 1),
    JS_CFUNC_MAGIC_DEF("setClipboardText", 1, sdl_clipboard, 2),
    JS_CFUNC_DEF("audioDevices", 1, sdl_audio_devices),
    JS_CFUNC_DEF("loadWAV", 1, sdl_load_wav),
    JS_CFUNC_DEF("loadTexture", 2, sdl_load_texture),
};

static JSClassID sdl_capture_cid(JSContext* ctx, JSTypedArrayEnum type)
{
    JSValueConst args[1];
    JSValue ta;
    JSClassID cid = 0;
    args[0] = JS_NewInt32(ctx, 0);
    ta = JS_NewTypedArray(ctx, 1, args, type);
    if (!JS_IsException(ta))
        cid = JS_GetClassID(ta);
    JS_FreeValue(ctx, ta);
    return cid;
}

static int sdl_init_module(JSContext* ctx, JSModuleDef* m)
{
    static const JSClassDef win_def = { "Window", .finalizer = dyn_res_finalizer };
    static const JSClassDef ren_def = { "Renderer", .finalizer = dyn_res_finalizer, .gc_mark = sdl_ren_mark };
    static const JSClassDef tex_def = { "Texture", .finalizer = dyn_res_finalizer, .gc_mark = sdl_tex_mark };
    static const JSClassDef audio_def = { "AudioStream", .finalizer = dyn_res_finalizer };
    static const JSClassDef pad_def = { "Gamepad", .finalizer = dyn_res_finalizer };
    sdl_f32_cid = sdl_capture_cid(ctx, JS_TYPED_ARRAY_FLOAT32);
    sdl_i16_cid = sdl_capture_cid(ctx, JS_TYPED_ARRAY_INT16);
    sdl_i32_cid = sdl_capture_cid(ctx, JS_TYPED_ARRAY_INT32);
    if (dyn_register_class(ctx, m, &sdl_win_cid, &win_def, sdl_window_proto, countof(sdl_window_proto), sdl_window_ctor, "Window")
        || dyn_register_class(ctx, m, &sdl_ren_cid, &ren_def, sdl_renderer_proto, countof(sdl_renderer_proto), sdl_renderer_ctor, "Renderer")
        || dyn_register_class(ctx, m, &sdl_tex_cid, &tex_def, sdl_texture_proto, countof(sdl_texture_proto), sdl_texture_ctor, "Texture")
        || dyn_register_class(ctx, m, &sdl_audio_cid, &audio_def, sdl_audio_proto, countof(sdl_audio_proto), sdl_audio_ctor, "AudioStream")
        || dyn_register_class(ctx, m, &sdl_pad_cid, &pad_def, sdl_pad_proto, countof(sdl_pad_proto), sdl_pad_ctor, "Gamepad"))
        return -1;
    return JS_SetModuleExportList(ctx, m, sdl_funcs, countof(sdl_funcs));
}

int js_nat_init_sdl(JSContext* ctx)
{
    JSModuleDef* m = JS_NewCModule(ctx, "dyna:sdl", sdl_init_module);
    if (!m)
        return -1;
    JS_AddModuleExport(ctx, m, "Window");
    JS_AddModuleExport(ctx, m, "Renderer");
    JS_AddModuleExport(ctx, m, "Texture");
    JS_AddModuleExport(ctx, m, "AudioStream");
    JS_AddModuleExport(ctx, m, "Gamepad");
    return JS_AddModuleExportList(ctx, m, sdl_funcs, countof(sdl_funcs));
}

int dyn_sdl_renderer_parts(JSContext* ctx, JSValueConst v, void** renderer, void** window)
{
    sdl_ren* r;
    if (JS_GetClassID(v) != sdl_ren_cid)
        return 0;
    r = sdl_ren_of(ctx, v);
    if (!r)
        return -1;
    if (!r->win || !r->win->w) {
        JS_ThrowTypeError(ctx, "dyna:sdl: the renderer's window was closed");
        return -1;
    }
    *renderer = r->r;
    *window = r->win->w;
    return 1;
}

int dyn_sdl_renderer_alive(JSValueConst v)
{
    DynResource* res = JS_GetOpaque(v, sdl_ren_cid);
    sdl_ren* r = res && !res->closed ? res->native : NULL;
    return r && r->r && r->win && r->win->w;
}

#endif
