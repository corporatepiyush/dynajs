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
#define RL_MAX_SAMPLES (1 << 28)
#define RL_COORD_MAX 1e7

typedef struct {
    const char* name;
    unsigned int value;
} rl_named;

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

static struct {
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
} rl;

static int rl_window_token;
static JSClassID rl_win_cid, rl_tex_cid, rl_rt_cid, rl_img_cid, rl_font_cid, rl_sound_cid, rl_music_cid, rl_stream_cid;
static JSClassID rl_f32_cid;

static const rl_named rl_key_table[] = {
    { "APOSTROPHE", 39 },
    { "COMMA", 44 },
    { "MINUS", 45 },
    { "PERIOD", 46 },
    { "SLASH", 47 },
    { "ZERO", 48 },
    { "ONE", 49 },
    { "TWO", 50 },
    { "THREE", 51 },
    { "FOUR", 52 },
    { "FIVE", 53 },
    { "SIX", 54 },
    { "SEVEN", 55 },
    { "EIGHT", 56 },
    { "NINE", 57 },
    { "SEMICOLON", 59 },
    { "EQUAL", 61 },
    { "A", 65 },
    { "B", 66 },
    { "C", 67 },
    { "D", 68 },
    { "E", 69 },
    { "F", 70 },
    { "G", 71 },
    { "H", 72 },
    { "I", 73 },
    { "J", 74 },
    { "K", 75 },
    { "L", 76 },
    { "M", 77 },
    { "N", 78 },
    { "O", 79 },
    { "P", 80 },
    { "Q", 81 },
    { "R", 82 },
    { "S", 83 },
    { "T", 84 },
    { "U", 85 },
    { "V", 86 },
    { "W", 87 },
    { "X", 88 },
    { "Y", 89 },
    { "Z", 90 },
    { "LEFT_BRACKET", 91 },
    { "BACKSLASH", 92 },
    { "RIGHT_BRACKET", 93 },
    { "GRAVE", 96 },
    { "SPACE", 32 },
    { "ESCAPE", 256 },
    { "ENTER", 257 },
    { "TAB", 258 },
    { "BACKSPACE", 259 },
    { "INSERT", 260 },
    { "DELETE", 261 },
    { "RIGHT", 262 },
    { "LEFT", 263 },
    { "DOWN", 264 },
    { "UP", 265 },
    { "PAGE_UP", 266 },
    { "PAGE_DOWN", 267 },
    { "HOME", 268 },
    { "END", 269 },
    { "CAPS_LOCK", 280 },
    { "SCROLL_LOCK", 281 },
    { "NUM_LOCK", 282 },
    { "PRINT_SCREEN", 283 },
    { "PAUSE", 284 },
    { "F1", 290 },
    { "F2", 291 },
    { "F3", 292 },
    { "F4", 293 },
    { "F5", 294 },
    { "F6", 295 },
    { "F7", 296 },
    { "F8", 297 },
    { "F9", 298 },
    { "F10", 299 },
    { "F11", 300 },
    { "F12", 301 },
    { "LEFT_SHIFT", 340 },
    { "LEFT_CONTROL", 341 },
    { "LEFT_ALT", 342 },
    { "LEFT_SUPER", 343 },
    { "RIGHT_SHIFT", 344 },
    { "RIGHT_CONTROL", 345 },
    { "RIGHT_ALT", 346 },
    { "RIGHT_SUPER", 347 },
    { "KB_MENU", 348 },
    { "KP_0", 320 },
    { "KP_1", 321 },
    { "KP_2", 322 },
    { "KP_3", 323 },
    { "KP_4", 324 },
    { "KP_5", 325 },
    { "KP_6", 326 },
    { "KP_7", 327 },
    { "KP_8", 328 },
    { "KP_9", 329 },
    { "KP_DECIMAL", 330 },
    { "KP_DIVIDE", 331 },
    { "KP_MULTIPLY", 332 },
    { "KP_SUBTRACT", 333 },
    { "KP_ADD", 334 },
    { "KP_ENTER", 335 },
    { "KP_EQUAL", 336 },
    { "BACK", 4 },
    { "MENU", 5 },
    { "VOLUME_UP", 24 },
    { "VOLUME_DOWN", 25 },
};

static const rl_named rl_color_table[] = {
    { "LIGHTGRAY", 0xC8C8C8FFu },
    { "GRAY", 0x828282FFu },
    { "DARKGRAY", 0x505050FFu },
    { "YELLOW", 0xFDF900FFu },
    { "GOLD", 0xFFCB00FFu },
    { "ORANGE", 0xFFA100FFu },
    { "PINK", 0xFF6DC2FFu },
    { "RED", 0xE62937FFu },
    { "MAROON", 0xBE2137FFu },
    { "GREEN", 0x00E430FFu },
    { "LIME", 0x009E2FFFu },
    { "DARKGREEN", 0x00752CFFu },
    { "SKYBLUE", 0x66BFFFFFu },
    { "BLUE", 0x0079F1FFu },
    { "DARKBLUE", 0x0052ACFFu },
    { "PURPLE", 0xC87AFFFFu },
    { "VIOLET", 0x873CBEFFu },
    { "DARKPURPLE", 0x701F7EFFu },
    { "BEIGE", 0xD3B083FFu },
    { "BROWN", 0x7F6A4FFFu },
    { "DARKBROWN", 0x4C3F2FFFu },
    { "WHITE", 0xFFFFFFFFu },
    { "BLACK", 0x000000FFu },
    { "BLANK", 0x00000000u },
    { "MAGENTA", 0xFF00FFFFu },
    { "RAYWHITE", 0xF5F5F5FFu },
};

static const rl_named rl_gamepad_button_table[] = {
    { "UNKNOWN", 0 },
    { "LEFT_FACE_UP", 1 },
    { "LEFT_FACE_RIGHT", 2 },
    { "LEFT_FACE_DOWN", 3 },
    { "LEFT_FACE_LEFT", 4 },
    { "RIGHT_FACE_UP", 5 },
    { "RIGHT_FACE_RIGHT", 6 },
    { "RIGHT_FACE_DOWN", 7 },
    { "RIGHT_FACE_LEFT", 8 },
    { "LEFT_TRIGGER_1", 9 },
    { "LEFT_TRIGGER_2", 10 },
    { "RIGHT_TRIGGER_1", 11 },
    { "RIGHT_TRIGGER_2", 12 },
    { "MIDDLE_LEFT", 13 },
    { "MIDDLE", 14 },
    { "MIDDLE_RIGHT", 15 },
    { "LEFT_THUMB", 16 },
    { "RIGHT_THUMB", 17 },
};

static const rl_named rl_gamepad_axis_table[] = {
    { "LEFT_X", 0 },
    { "LEFT_Y", 1 },
    { "RIGHT_X", 2 },
    { "RIGHT_Y", 3 },
    { "LEFT_TRIGGER", 4 },
    { "RIGHT_TRIGGER", 5 },
};

static const rl_named rl_mouse_button_table[] = {
    { "LEFT", 0 },
    { "RIGHT", 1 },
    { "MIDDLE", 2 },
    { "SIDE", 3 },
    { "EXTRA", 4 },
    { "FORWARD", 5 },
    { "BACK", 6 },
};

static int rl_on_main(JSContext* ctx, const char* what)
{
#ifdef __APPLE__
    if (!pthread_main_np()) {
        JS_ThrowTypeError(ctx, "dyna:raylib: %s must run on the main thread, not in a Worker", what);
        return 0;
    }
#endif
    if (!js_std_runtime_is_main(JS_GetRuntime(ctx))) {
        JS_ThrowTypeError(ctx, "dyna:raylib: %s must run on the main thread, not in a Worker", what);
        return 0;
    }
    return 1;
}

static int rl_ready(JSContext* ctx, const char* what)
{
    if (!rl_on_main(ctx, what))
        return 0;
    if (!rl.open || rl.closing) {
        JS_ThrowTypeError(ctx, "dyna:raylib: %s needs an open Window", what);
        return 0;
    }
    return 1;
}

static int rl_audio_ready(JSContext* ctx, const char* what)
{
    if (!rl_on_main(ctx, what))
        return 0;
    if (!IsAudioDeviceReady()) {
        JS_ThrowTypeError(ctx, "dyna:raylib: %s needs initAudio() first", what);
        return 0;
    }
    return 1;
}

static int rl_num(JSContext* ctx, JSValueConst v, const char* what, float* out)
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

static int rl_num_in(JSContext* ctx, JSValueConst v, const char* what, double lo, double hi, float* out)
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

static int rl_int_in(JSContext* ctx, JSValueConst v, const char* what, int lo, int hi, int* out)
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

static int rl_color(JSContext* ctx, JSValueConst v, const char* what, Color* c)
{
    double d;
    uint32_t u;
    if (JS_ToFloat64(ctx, &d, v))
        return -1;
    if (!(d >= 0 && d <= 4294967295.0) || d != floor(d)) {
        JS_ThrowRangeError(ctx, "dyna:raylib: %s must be a colour 0xRRGGBBAA (an integer 0..0xFFFFFFFF)", what);
        return -1;
    }
    u = (uint32_t)d;
    c->r = (unsigned char)(u >> 24);
    c->g = (unsigned char)(u >> 16);
    c->b = (unsigned char)(u >> 8);
    c->a = (unsigned char)u;
    return 0;
}

static int rl_color_opt(JSContext* ctx, int argc, JSValueConst* argv, int i, const char* what, Color* c)
{
    if (argc <= i || JS_IsUndefined(argv[i])) {
        *c = WHITE;
        return 0;
    }
    return rl_color(ctx, argv[i], what, c);
}

static int rl_args(JSContext* ctx, int argc, JSValueConst* argv, int n, const char* usage)
{
    if (argc < n) {
        JS_ThrowTypeError(ctx, "dyna:raylib: %s", usage);
        return -1;
    }
    return 0;
}

static int rl_nums(JSContext* ctx, JSValueConst* argv, int n, const char* what, float* out)
{
    int i;
    for (i = 0; i < n; i++)
        if (rl_num(ctx, argv[i], what, &out[i]))
            return -1;
    return 0;
}

static int rl_field(JSContext* ctx, JSValueConst obj, const char* key, const char* what, int need, float dflt, float* out)
{
    JSValue v = JS_GetPropertyStr(ctx, obj, key);
    int rc;
    if (JS_IsException(v))
        return -1;
    if (JS_IsUndefined(v)) {
        if (need) {
            JS_ThrowTypeError(ctx, "dyna:raylib: %s needs %s", what, key);
            return -1;
        }
        *out = dflt;
        return 0;
    }
    rc = rl_num(ctx, v, what, out);
    JS_FreeValue(ctx, v);
    return rc;
}

static int rl_rect(JSContext* ctx, JSValueConst v, const char* what, Rectangle* r)
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

static int rl_vec2(JSContext* ctx, JSValueConst obj, const char* key, const char* what, Vector2 dflt, Vector2* out)
{
    JSValue v = JS_GetPropertyStr(ctx, obj, key);
    int rc = 0;
    if (JS_IsException(v))
        return -1;
    if (JS_IsUndefined(v)) {
        *out = dflt;
        return 0;
    }
    if (!JS_IsObject(v)) {
        JS_FreeValue(ctx, v);
        JS_ThrowTypeError(ctx, "dyna:raylib: %s.%s must be { x, y }", what, key);
        return -1;
    }
    if (rl_field(ctx, v, "x", what, 1, 0, &out->x) || rl_field(ctx, v, "y", what, 1, 0, &out->y))
        rc = -1;
    JS_FreeValue(ctx, v);
    return rc;
}

static int rl_vec3(JSContext* ctx, JSValueConst obj, const char* key, const char* what, Vector3 dflt, Vector3* out)
{
    JSValue v = JS_GetPropertyStr(ctx, obj, key);
    int rc = 0;
    if (JS_IsException(v))
        return -1;
    if (JS_IsUndefined(v)) {
        *out = dflt;
        return 0;
    }
    if (!JS_IsObject(v)) {
        JS_FreeValue(ctx, v);
        JS_ThrowTypeError(ctx, "dyna:raylib: %s.%s must be { x, y, z }", what, key);
        return -1;
    }
    if (rl_field(ctx, v, "x", what, 1, 0, &out->x) || rl_field(ctx, v, "y", what, 1, 0, &out->y)
        || rl_field(ctx, v, "z", what, 1, 0, &out->z))
        rc = -1;
    JS_FreeValue(ctx, v);
    return rc;
}

static int rl_check_opts(JSContext* ctx, JSValueConst opts, const char* who, const char* const* keys, int nkeys)
{
    JSPropertyEnum* tab;
    uint32_t len, i;
    int j, rc = 0;
    if (JS_IsUndefined(opts))
        return 0;
    if (!JS_IsObject(opts)) {
        JS_ThrowTypeError(ctx, "dyna:raylib: %s options must be an object", who);
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
            JS_ThrowTypeError(ctx, "dyna:raylib: %s: unknown option '%s'", who, k);
            rc = -1;
        }
        JS_FreeCString(ctx, k);
    }
    JS_FreePropertyEnum(ctx, tab, len);
    return rc;
}

static int rl_opt_bool(JSContext* ctx, JSValueConst opts, const char* key, int* out)
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

static int rl_path(JSContext* ctx, JSValueConst v, const char* who, const char** out)
{
    size_t len;
    const char* s;
    if (!JS_IsString(v)) {
        JS_ThrowTypeError(ctx, "dyna:raylib: %s needs a path string", who);
        return -1;
    }
    s = JS_ToCStringLen(ctx, &len, v);
    if (!s)
        return -1;
    if (strlen(s) != len || len == 0) {
        JS_FreeCString(ctx, s);
        JS_ThrowTypeError(ctx, "dyna:raylib: %s: the path is empty or holds a NUL character", who);
        return -1;
    }
    *out = s;
    return 0;
}

static JSValue rl_xy(JSContext* ctx, double x, double y, const char* kx, const char* ky)
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

static int rl_sync_result(JSContext* ctx, JSValue r, const char* who)
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

static void rl_gpu_link(rl_gpu* g)
{
    g->prev = NULL;
    g->next = rl.gpu;
    if (rl.gpu)
        rl.gpu->prev = g;
    rl.gpu = g;
}

static void rl_gpu_unlink(rl_gpu* g)
{
    if (g->prev)
        g->prev->next = g->next;
    else if (rl.gpu == g)
        rl.gpu = g->next;
    if (g->next)
        g->next->prev = g->prev;
    g->next = g->prev = NULL;
}

static void rl_gpu_unload(rl_gpu* g)
{
    if (!g->alive)
        return;
    if (rl.depth)
        rlDrawRenderBatchActive();
    if (g->kind == RL_TEX)
        UnloadTexture(g->tex);
    else if (g->kind == RL_RT)
        UnloadRenderTexture(g->rt);
    else
        UnloadFont(g->font);
    g->alive = 0;
}

static void rl_snd_link(rl_snd* s)
{
    s->prev = NULL;
    s->next = rl.snd;
    if (rl.snd)
        rl.snd->prev = s;
    rl.snd = s;
}

static void rl_snd_unlink(rl_snd* s)
{
    if (s->prev)
        s->prev->next = s->next;
    else if (rl.snd == s)
        rl.snd = s->next;
    if (s->next)
        s->next->prev = s->prev;
    s->next = s->prev = NULL;
}

static void rl_snd_unload(rl_snd* s)
{
    if (!s->alive)
        return;
    if (s->kind == RL_SOUND)
        UnloadSound(s->sound);
    else if (s->kind == RL_MUSIC)
        UnloadMusicStream(s->music);
    else
        UnloadAudioStream(s->stream);
    s->alive = 0;
}

static void rl_close_now(void)
{
    rl_gpu* g = rl.gpu;
    while (g) {
        rl_gpu* next = g->next;
        rl_gpu_unload(g);
        g->next = g->prev = NULL;
        g = next;
    }
    rl.gpu = NULL;
    if (rl.open)
        CloseWindow();
    rl.open = 0;
    rl.closing = 0;
}

static void rl_after_scope(void)
{
    if (rl.depth == 0 && rl.closing)
        rl_close_now();
}

static void rl_win_dispose(void* p)
{
    (void)p;
    rl.win_obj = JS_UNDEFINED;
    if (rl.depth)
        rl.closing = 1;
    else
        rl_close_now();
}

static void rl_gpu_dispose(void* p)
{
    rl_gpu* g = p;
    JSValue held = g->win_obj;
    JSRuntime* jrt = g->jrt;
    g->win_obj = JS_UNDEFINED;
    if (g == rl.active_rt) {
        g->pending_free = 1;
    } else {
        rl_gpu_unload(g);
        rl_gpu_unlink(g);
        dyn_nat_free(g);
    }
    JS_FreeValueRT(jrt, held);
}

static void rl_snd_dispose(void* p)
{
    rl_snd* s = p;
    rl_snd_unload(s);
    rl_snd_unlink(s);
    dyn_nat_free(s);
}

static void rl_img_dispose(void* p)
{
    Image* im = p;
    UnloadImage(*im);
    dyn_nat_free(im);
}

static void rl_gpu_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark)
{
    JSClassID id;
    DynResource* r = JS_GetAnyOpaque(val, &id);
    if (r && r->native)
        JS_MarkValue(rt, ((rl_gpu*)r->native)->win_obj, mark);
}

static rl_gpu* rl_gpu_of(JSContext* ctx, JSValueConst v, JSClassID cid)
{
    rl_gpu* g = dyn_res_native(ctx, v, cid);
    if (g && !g->alive) {
        JS_ThrowTypeError(ctx, "dyna:raylib: the Window that owned this resource was closed");
        return NULL;
    }
    return g;
}

static rl_snd* rl_snd_of(JSContext* ctx, JSValueConst v, JSClassID cid)
{
    rl_snd* s = dyn_res_native(ctx, v, cid);
    if (s && !s->alive) {
        JS_ThrowTypeError(ctx, "dyna:raylib: the audio device was closed under this resource");
        return NULL;
    }
    return s;
}

static Image* rl_img_of(JSContext* ctx, JSValueConst v)
{
    return dyn_res_native(ctx, v, rl_img_cid);
}

static int rl_drawable(JSContext* ctx, JSValueConst v, const char* what, Texture2D* tex, int* flip)
{
    JSClassID cid = JS_GetClassID(v);
    rl_gpu* g;
    if (cid != rl_tex_cid && cid != rl_rt_cid) {
        JS_ThrowTypeError(ctx, "dyna:raylib: %s needs a Texture or a RenderTexture", what);
        return -1;
    }
    g = rl_gpu_of(ctx, v, cid);
    if (!g)
        return -1;
    if (g == rl.active_rt) {
        JS_ThrowTypeError(ctx, "dyna:raylib: %s: a RenderTexture cannot be drawn inside its own draw()", what);
        return -1;
    }
    *tex = cid == rl_rt_cid ? g->rt.texture : g->tex;
    *flip = cid == rl_rt_cid;
    return 0;
}

static JSValue rl_wrap_gpu(JSContext* ctx, JSClassID cid, rl_gpu* g)
{
    g->jrt = JS_GetRuntime(ctx);
    g->win_obj = JS_DupValue(ctx, rl.win_obj);
    g->alive = 1;
    rl_gpu_link(g);
    return dyn_res_wrap(ctx, JS_UNDEFINED, cid, g, rl_gpu_dispose);
}

static rl_gpu* rl_gpu_new(JSContext* ctx, int kind)
{
    rl_gpu* g = dyn_nat_calloc(1, sizeof(*g));
    if (!g) {
        JS_ThrowOutOfMemory(ctx);
        return NULL;
    }
    g->kind = kind;
    g->win_obj = JS_UNDEFINED;
    return g;
}

static const char* const rl_window_keys[] = { "hidden", "resizable", "highDensity", "vsync", "msaa", "undecorated", "topmost", "transparent", "fps" };

static JSValue rl_window_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv)
{
    static const unsigned int bits[] = { FLAG_WINDOW_HIDDEN, FLAG_WINDOW_RESIZABLE, FLAG_WINDOW_HIGHDPI, FLAG_VSYNC_HINT,
        FLAG_MSAA_4X_HINT, FLAG_WINDOW_UNDECORATED, FLAG_WINDOW_TOPMOST, FLAG_WINDOW_TRANSPARENT };
    JSValueConst opts = argc > 3 ? argv[3] : JS_UNDEFINED;
    unsigned int flags = 0;
    int w, h, fps = 0, b, i;
    size_t tlen;
    const char* title;
    JSValue obj;
    if (argc < 3 || !JS_IsString(argv[2]))
        return JS_ThrowTypeError(ctx, "dyna:raylib: new Window(width, height, title, opts?)");
    if (rl_int_in(ctx, argv[0], "Window width", 1, RL_MAX_DIM, &w) || rl_int_in(ctx, argv[1], "Window height", 1, RL_MAX_DIM, &h))
        return JS_EXCEPTION;
    if (rl_check_opts(ctx, opts, "Window", rl_window_keys, (int)countof(rl_window_keys)))
        return JS_EXCEPTION;
    for (i = 0; i < (int)countof(bits); i++) {
        b = 0;
        if (rl_opt_bool(ctx, opts, rl_window_keys[i], &b))
            return JS_EXCEPTION;
        if (b)
            flags |= bits[i];
    }
    if (JS_IsObject(opts)) {
        JSValue v = JS_GetPropertyStr(ctx, opts, "fps");
        if (JS_IsException(v))
            return JS_EXCEPTION;
        if (!JS_IsUndefined(v) && rl_int_in(ctx, v, "Window fps", 0, 1000, &fps)) {
            JS_FreeValue(ctx, v);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, v);
    }
    if (!rl_on_main(ctx, "new Window()"))
        return JS_EXCEPTION;
    if (rl.open || rl.closing)
        return JS_ThrowTypeError(ctx, "dyna:raylib: raylib drives one window per process; close the open Window first");
    title = JS_ToCStringLen(ctx, &tlen, argv[2]);
    if (!title)
        return JS_EXCEPTION;
    if (dyn_wtf8_has_surrogate(title, tlen)) {
        JS_FreeCString(ctx, title);
        return JS_ThrowTypeError(ctx, "dyna:raylib: a window title must be well-formed text (it holds a lone surrogate)");
    }
#ifdef __APPLE__
    flags |= FLAG_WINDOW_HIGHDPI;
#endif
    SetConfigFlags(flags);
    InitWindow(w, h, title);
    JS_FreeCString(ctx, title);
    if (!IsWindowReady())
        return JS_ThrowInternalError(ctx, "dyna:raylib: InitWindow failed (is there a display to open a window on?)");
    rl.open = 1;
    rl.closing = 0;
    SetTargetFPS(fps);
    obj = dyn_res_wrap(ctx, new_target, rl_win_cid, &rl_window_token, rl_win_dispose);
    if (!JS_IsException(obj))
        rl.win_obj = obj;
    return obj;
}

static int rl_win_live(JSContext* ctx, JSValueConst this_val)
{
    if (!dyn_res_native(ctx, this_val, rl_win_cid))
        return 0;
    return rl_ready(ctx, "a Window method");
}

static JSValue rl_win_get(JSContext* ctx, JSValueConst this_val, int magic)
{
    Vector2 v;
    if (!rl_win_live(ctx, this_val))
        return JS_EXCEPTION;
    switch (magic) {
    case 0:
        return JS_NewBool(ctx, WindowShouldClose());
    case 1:
        return rl_xy(ctx, GetScreenWidth(), GetScreenHeight(), "width", "height");
    case 2:
        return rl_xy(ctx, GetRenderWidth(), GetRenderHeight(), "width", "height");
    case 3:
        v = GetWindowScaleDPI();
        return rl_xy(ctx, (double)v.x, (double)v.y, "x", "y");
    case 4:
        return JS_NewBool(ctx, IsWindowFocused());
    case 5:
        return JS_NewBool(ctx, IsWindowMinimized());
    case 6:
        return JS_NewInt32(ctx, GetFPS());
    case 7:
        return JS_NewFloat64(ctx, (double)GetFrameTime());
    case 8:
        return JS_NewFloat64(ctx, GetTime());
    default:
        return JS_UNDEFINED;
    }
}

static JSValue rl_win_set_title(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    const char* s;
    size_t n;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "dyna:raylib: setTitle(title) needs a string");
    if (!rl_win_live(ctx, this_val))
        return JS_EXCEPTION;
    s = JS_ToCStringLen(ctx, &n, argv[0]);
    if (!s)
        return JS_EXCEPTION;
    if (dyn_wtf8_has_surrogate(s, n)) {
        JS_FreeCString(ctx, s);
        return JS_ThrowTypeError(ctx, "dyna:raylib: a window title must be well-formed text (it holds a lone surrogate)");
    }
    SetWindowTitle(s);
    JS_FreeCString(ctx, s);
    return JS_UNDEFINED;
}

static JSValue rl_win_set2(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    static const char* const usage[] = { "setSize(width, height)", "setPosition(x, y)" };
    int a, b;
    if (rl_args(ctx, argc, argv, 2, usage[magic]))
        return JS_EXCEPTION;
    if (magic == 0) {
        if (rl_int_in(ctx, argv[0], "width", 1, RL_MAX_DIM, &a) || rl_int_in(ctx, argv[1], "height", 1, RL_MAX_DIM, &b))
            return JS_EXCEPTION;
    } else if (rl_int_in(ctx, argv[0], "x", -RL_MAX_DIM * 4, RL_MAX_DIM * 4, &a)
        || rl_int_in(ctx, argv[1], "y", -RL_MAX_DIM * 4, RL_MAX_DIM * 4, &b)) {
        return JS_EXCEPTION;
    }
    if (!rl_win_live(ctx, this_val))
        return JS_EXCEPTION;
    if (magic == 0)
        SetWindowSize(a, b);
    else
        SetWindowPosition(a, b);
    return JS_UNDEFINED;
}

static JSValue rl_win_set1(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    static const char* const usage[] = { "setTargetFPS(fps)", "setExitKey(key)" };
    int v;
    if (rl_args(ctx, argc, argv, 1, usage[magic]))
        return JS_EXCEPTION;
    if (magic == 0 ? rl_int_in(ctx, argv[0], "setTargetFPS(fps)", 0, 1000, &v) : rl_int_in(ctx, argv[0], "setExitKey(key)", 0, 511, &v))
        return JS_EXCEPTION;
    if (!rl_win_live(ctx, this_val))
        return JS_EXCEPTION;
    if (magic == 0)
        SetTargetFPS(v);
    else
        SetExitKey(v);
    return JS_UNDEFINED;
}

static JSValue rl_win_frame(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    JSValue r;
    if (argc < 1 || !JS_IsFunction(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "dyna:raylib: frame(draw) needs a function");
    if (!rl_win_live(ctx, this_val))
        return JS_EXCEPTION;
    if (rl.depth)
        return JS_ThrowTypeError(ctx, "dyna:raylib: frame() cannot run inside frame(), mode2D(), mode3D() or RenderTexture#draw()");
    BeginDrawing();
    rl.in_frame = 1;
    rl.depth++;
    r = JS_Call(ctx, argv[0], JS_UNDEFINED, 0, NULL);
    rl.depth--;
    rl.in_frame = 0;
    EndDrawing();
    rl_after_scope();
    if (rl_sync_result(ctx, r, "frame(draw)"))
        return JS_EXCEPTION;
    return JS_UNDEFINED;
}

static int rl_camera2d(JSContext* ctx, JSValueConst v, Camera2D* c)
{
    float rot = 0, zoom = 1;
    if (!JS_IsObject(v)) {
        JS_ThrowTypeError(ctx, "dyna:raylib: mode2D camera must be { offset?, target?, rotation?, zoom? }");
        return -1;
    }
    if (rl_vec2(ctx, v, "offset", "mode2D camera", (Vector2) { 0, 0 }, &c->offset)
        || rl_vec2(ctx, v, "target", "mode2D camera", (Vector2) { 0, 0 }, &c->target)
        || rl_field(ctx, v, "rotation", "mode2D camera", 0, 0, &rot) || rl_field(ctx, v, "zoom", "mode2D camera", 0, 1, &zoom))
        return -1;
    if (!(zoom > 0)) {
        JS_ThrowRangeError(ctx, "dyna:raylib: mode2D camera zoom must be greater than 0");
        return -1;
    }
    c->rotation = rot;
    c->zoom = zoom;
    return 0;
}

static int rl_camera3d(JSContext* ctx, JSValueConst v, Camera3D* c)
{
    JSValue pv;
    float fovy = 45;
    c->projection = CAMERA_PERSPECTIVE;
    if (!JS_IsObject(v)) {
        JS_ThrowTypeError(ctx, "dyna:raylib: mode3D camera must be { position, target?, up?, fovy?, projection? }");
        return -1;
    }
    if (rl_vec3(ctx, v, "position", "mode3D camera", (Vector3) { 0, 0, 10 }, &c->position)
        || rl_vec3(ctx, v, "target", "mode3D camera", (Vector3) { 0, 0, 0 }, &c->target)
        || rl_vec3(ctx, v, "up", "mode3D camera", (Vector3) { 0, 1, 0 }, &c->up)
        || rl_field(ctx, v, "fovy", "mode3D camera", 0, 45, &fovy))
        return -1;
    if (!(fovy > 0 && fovy < 180)) {
        JS_ThrowRangeError(ctx, "dyna:raylib: mode3D camera fovy must be in (0, 180)");
        return -1;
    }
    c->fovy = fovy;
    pv = JS_GetPropertyStr(ctx, v, "projection");
    if (JS_IsException(pv))
        return -1;
    if (!JS_IsUndefined(pv)) {
        const char* s = JS_IsString(pv) ? JS_ToCString(ctx, pv) : NULL;
        int ok = s && (!strcmp(s, "perspective") || !strcmp(s, "orthographic"));
        if (ok && !strcmp(s, "orthographic"))
            c->projection = CAMERA_ORTHOGRAPHIC;
        if (s)
            JS_FreeCString(ctx, s);
        JS_FreeValue(ctx, pv);
        if (!ok) {
            JS_ThrowTypeError(ctx, "dyna:raylib: mode3D camera projection is \"perspective\" or \"orthographic\"");
            return -1;
        }
    }
    return 0;
}

static JSValue rl_win_mode(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    Camera2D c2;
    Camera3D c3;
    JSValue r;
    const char* who = magic ? "mode3D(camera, draw)" : "mode2D(camera, draw)";
    memset(&c2, 0, sizeof(c2));
    memset(&c3, 0, sizeof(c3));
    if (argc < 2 || !JS_IsFunction(ctx, argv[1]))
        return JS_ThrowTypeError(ctx, "dyna:raylib: %s needs a camera and a function", who);
    if (magic ? rl_camera3d(ctx, argv[0], &c3) : rl_camera2d(ctx, argv[0], &c2))
        return JS_EXCEPTION;
    if (!rl_win_live(ctx, this_val))
        return JS_EXCEPTION;
    if (!rl.in_frame && !rl.in_texture)
        return JS_ThrowTypeError(ctx, "dyna:raylib: %s runs inside frame() or RenderTexture#draw()", who);
    if (rl.in_mode2d || rl.in_mode3d)
        return JS_ThrowTypeError(ctx, "dyna:raylib: %s cannot nest inside mode2D() or mode3D()", who);
    if (magic) {
        BeginMode3D(c3);
        rl.in_mode3d = 1;
    } else {
        BeginMode2D(c2);
        rl.in_mode2d = 1;
    }
    rl.depth++;
    r = JS_Call(ctx, argv[1], JS_UNDEFINED, 0, NULL);
    rl.depth--;
    if (magic) {
        EndMode3D();
        rl.in_mode3d = 0;
    } else {
        EndMode2D();
        rl.in_mode2d = 0;
    }
    rl_after_scope();
    if (rl_sync_result(ctx, r, who))
        return JS_EXCEPTION;
    return JS_UNDEFINED;
}

static JSValue rl_clear(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    Color c;
    (void)this_val;
    if (rl_args(ctx, argc, argv, 1, "clearBackground(color)") || rl_color(ctx, argv[0], "clearBackground color", &c))
        return JS_EXCEPTION;
    if (!rl_ready(ctx, "clearBackground()"))
        return JS_EXCEPTION;
    ClearBackground(c);
    return JS_UNDEFINED;
}

static JSValue rl_draw2d(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    static const int need[] = { 3, 5, 4, 4, 5, 5, 5, 7, 7, 6 };
    static const char* const names[] = { "drawPixel(x, y, color)", "drawLine(x1, y1, x2, y2, color, thick?)",
        "drawCircle(cx, cy, radius, color)", "drawCircleLines(cx, cy, radius, color)", "drawEllipse(cx, cy, rx, ry, color)",
        "drawRectangle(x, y, width, height, color)", "drawRectangleLines(x, y, width, height, color, thick?)",
        "drawRectangleRounded(x, y, width, height, roundness, segments, color)", "drawTriangle(x1, y1, x2, y2, x3, y3, color)",
        "drawPoly(cx, cy, sides, radius, rotation, color)" };
    float f[6] = { 0, 0, 0, 0, 0, 0 }, extra = 1;
    int n = need[magic] - 1, seg = 0, sides = 3;
    Color c;
    (void)this_val;
    if (rl_args(ctx, argc, argv, need[magic], names[magic]))
        return JS_EXCEPTION;
    if (magic == 7) {
        if (rl_nums(ctx, argv, 4, names[magic], f) || rl_num_in(ctx, argv[4], "drawRectangleRounded roundness", 0, 1, &f[4])
            || rl_int_in(ctx, argv[5], "drawRectangleRounded segments", 0, 1024, &seg) || rl_color(ctx, argv[6], "drawRectangleRounded color", &c))
            return JS_EXCEPTION;
    } else if (magic == 9) {
        if (rl_nums(ctx, argv, 2, names[magic], f) || rl_int_in(ctx, argv[2], "drawPoly sides", 3, 4096, &sides)
            || rl_num(ctx, argv[3], names[magic], &f[2]) || rl_num(ctx, argv[4], names[magic], &f[3])
            || rl_color(ctx, argv[5], "drawPoly color", &c))
            return JS_EXCEPTION;
    } else {
        if (rl_nums(ctx, argv, n, names[magic], f) || rl_color(ctx, argv[n], "a draw colour", &c))
            return JS_EXCEPTION;
        if ((magic == 1 || magic == 6) && argc > n + 1 && !JS_IsUndefined(argv[n + 1])
            && rl_num_in(ctx, argv[n + 1], "line thickness", 0, 4096, &extra))
            return JS_EXCEPTION;
    }
    if (!rl_ready(ctx, names[magic]))
        return JS_EXCEPTION;
    switch (magic) {
    case 0:
        DrawPixelV((Vector2) { f[0], f[1] }, c);
        break;
    case 1:
        DrawLineEx((Vector2) { f[0], f[1] }, (Vector2) { f[2], f[3] }, extra, c);
        break;
    case 2:
        DrawCircleV((Vector2) { f[0], f[1] }, f[2], c);
        break;
    case 3:
        DrawCircleLinesV((Vector2) { f[0], f[1] }, f[2], c);
        break;
    case 4:
        DrawEllipse((int)f[0], (int)f[1], f[2], f[3], c);
        break;
    case 5:
        DrawRectangleRec((Rectangle) { f[0], f[1], f[2], f[3] }, c);
        break;
    case 6:
        DrawRectangleLinesEx((Rectangle) { f[0], f[1], f[2], f[3] }, extra, c);
        break;
    case 7:
        DrawRectangleRounded((Rectangle) { f[0], f[1], f[2], f[3] }, f[4], seg, c);
        break;
    case 8:
        DrawTriangle((Vector2) { f[0], f[1] }, (Vector2) { f[2], f[3] }, (Vector2) { f[4], f[5] }, c);
        break;
    case 9:
        DrawPoly((Vector2) { f[0], f[1] }, sides, f[2], f[3], c);
        break;
    }
    return JS_UNDEFINED;
}

static JSValue rl_draw3d(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    static const int need[] = { 7, 7, 5, 6, 7 };
    static const char* const names[] = { "drawCube(x, y, z, width, height, length, color)",
        "drawCubeWires(x, y, z, width, height, length, color)", "drawSphere(x, y, z, radius, color)",
        "drawPlane(x, y, z, width, length, color)", "drawLine3D(x1, y1, z1, x2, y2, z2, color)" };
    float f[6];
    Color c;
    int n = need[magic] - 1;
    (void)this_val;
    if (rl_args(ctx, argc, argv, need[magic], names[magic]) || rl_nums(ctx, argv, n, names[magic], f)
        || rl_color(ctx, argv[n], "a draw colour", &c))
        return JS_EXCEPTION;
    if (!rl_ready(ctx, names[magic]))
        return JS_EXCEPTION;
    switch (magic) {
    case 0:
        DrawCube((Vector3) { f[0], f[1], f[2] }, f[3], f[4], f[5], c);
        break;
    case 1:
        DrawCubeWires((Vector3) { f[0], f[1], f[2] }, f[3], f[4], f[5], c);
        break;
    case 2:
        DrawSphere((Vector3) { f[0], f[1], f[2] }, f[3], c);
        break;
    case 3:
        DrawPlane((Vector3) { f[0], f[1], f[2] }, (Vector2) { f[3], f[4] }, c);
        break;
    case 4:
        DrawLine3D((Vector3) { f[0], f[1], f[2] }, (Vector3) { f[3], f[4], f[5] }, c);
        break;
    }
    return JS_UNDEFINED;
}

static JSValue rl_draw_grid(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    int slices;
    float spacing;
    (void)this_val;
    if (rl_args(ctx, argc, argv, 2, "drawGrid(slices, spacing)") || rl_int_in(ctx, argv[0], "drawGrid slices", 1, 1024, &slices)
        || rl_num_in(ctx, argv[1], "drawGrid spacing", 0, 1e6, &spacing))
        return JS_EXCEPTION;
    if (!rl_ready(ctx, "drawGrid()"))
        return JS_EXCEPTION;
    DrawGrid(slices, spacing);
    return JS_UNDEFINED;
}

static JSValue rl_draw_fps(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    float f[2];
    (void)this_val;
    if (rl_args(ctx, argc, argv, 2, "drawFPS(x, y)") || rl_nums(ctx, argv, 2, "drawFPS(x, y)", f))
        return JS_EXCEPTION;
    if (!rl_ready(ctx, "drawFPS()"))
        return JS_EXCEPTION;
    DrawFPS((int)f[0], (int)f[1]);
    return JS_UNDEFINED;
}

static int rl_font_arg(JSContext* ctx, JSValueConst v, Font* f)
{
    rl_gpu* g;
    if (JS_IsNull(v) || JS_IsUndefined(v)) {
        *f = GetFontDefault();
        return 0;
    }
    if (JS_GetClassID(v) != rl_font_cid) {
        JS_ThrowTypeError(ctx, "dyna:raylib: the font must be a Font or null for the default font");
        return -1;
    }
    g = rl_gpu_of(ctx, v, rl_font_cid);
    if (!g)
        return -1;
    *f = g->font;
    return 0;
}

static JSValue rl_text(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    static const int need[] = { 5, 2, 7, 4 };
    static const char* const names[] = { "drawText(text, x, y, fontSize, color)", "measureText(text, fontSize)",
        "drawTextEx(font, text, x, y, fontSize, spacing, color)", "measureTextEx(font, text, fontSize, spacing)" };
    const char* s;
    float x = 0, y = 0, size = 10, spacing = 0;
    int isize = 10, ti = magic >= 2 ? 1 : 0;
    Color c = WHITE;
    Font font;
    JSValue ret = JS_UNDEFINED;
    (void)this_val;
    if (rl_args(ctx, argc, argv, need[magic], names[magic]))
        return JS_EXCEPTION;
    if (!JS_IsString(argv[ti]))
        return JS_ThrowTypeError(ctx, "dyna:raylib: %s needs a text string", names[magic]);
    switch (magic) {
    case 0:
        if (rl_num(ctx, argv[1], names[0], &x) || rl_num(ctx, argv[2], names[0], &y)
            || rl_int_in(ctx, argv[3], "fontSize", 1, 4096, &isize) || rl_color(ctx, argv[4], "drawText color", &c))
            return JS_EXCEPTION;
        break;
    case 1:
        if (rl_int_in(ctx, argv[1], "fontSize", 1, 4096, &isize))
            return JS_EXCEPTION;
        break;
    case 2:
        if (rl_num(ctx, argv[2], names[2], &x) || rl_num(ctx, argv[3], names[2], &y)
            || rl_num_in(ctx, argv[4], "fontSize", 1, 4096, &size) || rl_num_in(ctx, argv[5], "spacing", -4096, 4096, &spacing)
            || rl_color(ctx, argv[6], "drawTextEx color", &c))
            return JS_EXCEPTION;
        break;
    case 3:
        if (rl_num_in(ctx, argv[2], "fontSize", 1, 4096, &size) || rl_num_in(ctx, argv[3], "spacing", -4096, 4096, &spacing))
            return JS_EXCEPTION;
        break;
    }
    if (!rl_ready(ctx, names[magic]))
        return JS_EXCEPTION;
    if (magic >= 2 && rl_font_arg(ctx, argv[0], &font))
        return JS_EXCEPTION;
    s = JS_ToCString(ctx, argv[ti]);
    if (!s)
        return JS_EXCEPTION;
    switch (magic) {
    case 0:
        DrawText(s, (int)x, (int)y, isize, c);
        break;
    case 1:
        ret = JS_NewInt32(ctx, MeasureText(s, isize));
        break;
    case 2:
        DrawTextEx(font, s, (Vector2) { x, y }, size, spacing, c);
        break;
    case 3: {
        Vector2 m = MeasureTextEx(font, s, size, spacing);
        ret = rl_xy(ctx, (double)m.x, (double)m.y, "width", "height");
        break;
    }
    }
    JS_FreeCString(ctx, s);
    return ret;
}

static JSValue rl_draw_texture(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    float f[2];
    Color tint;
    Texture2D tex;
    int flip;
    (void)this_val;
    if (rl_args(ctx, argc, argv, 3, "drawTexture(texture, x, y, tint?)") || rl_nums(ctx, argv + 1, 2, "drawTexture(texture, x, y, tint?)", f)
        || rl_color_opt(ctx, argc, argv, 3, "drawTexture tint", &tint))
        return JS_EXCEPTION;
    if (!rl_ready(ctx, "drawTexture()") || rl_drawable(ctx, argv[0], "drawTexture()", &tex, &flip))
        return JS_EXCEPTION;
    DrawTexturePro(tex, (Rectangle) { 0, 0, (float)tex.width, flip ? -(float)tex.height : (float)tex.height },
        (Rectangle) { f[0], f[1], (float)tex.width, (float)tex.height }, (Vector2) { 0, 0 }, 0, tint);
    return JS_UNDEFINED;
}

static JSValue rl_draw_texture_pro(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    Rectangle src, dst;
    float ox = 0, oy = 0, rot = 0;
    Color tint;
    Texture2D tex;
    int flip;
    (void)this_val;
    if (rl_args(ctx, argc, argv, 3, "drawTexturePro(texture, src, dst, originX?, originY?, rotation?, tint?)")
        || rl_rect(ctx, argv[1], "drawTexturePro src", &src) || rl_rect(ctx, argv[2], "drawTexturePro dst", &dst)
        || (argc > 3 && !JS_IsUndefined(argv[3]) && rl_num(ctx, argv[3], "drawTexturePro originX", &ox))
        || (argc > 4 && !JS_IsUndefined(argv[4]) && rl_num(ctx, argv[4], "drawTexturePro originY", &oy))
        || (argc > 5 && !JS_IsUndefined(argv[5]) && rl_num(ctx, argv[5], "drawTexturePro rotation", &rot))
        || rl_color_opt(ctx, argc, argv, 6, "drawTexturePro tint", &tint))
        return JS_EXCEPTION;
    if (!rl_ready(ctx, "drawTexturePro()") || rl_drawable(ctx, argv[0], "drawTexturePro()", &tex, &flip))
        return JS_EXCEPTION;
    if (flip) {
        src.y = (float)tex.height - src.y - src.height;
        src.height = -src.height;
    }
    DrawTexturePro(tex, src, dst, (Vector2) { ox, oy }, rot, tint);
    return JS_UNDEFINED;
}

static int rl_dims(JSContext* ctx, JSValueConst wv, JSValueConst hv, const char* who, int* w, int* h)
{
    if (rl_int_in(ctx, wv, "width", 1, RL_MAX_DIM, w) || rl_int_in(ctx, hv, "height", 1, RL_MAX_DIM, h))
        return -1;
    if ((int64_t)*w * *h > RL_MAX_PIXELS) {
        JS_ThrowRangeError(ctx, "dyna:raylib: %s: %dx%d is over the %d-pixel limit", who, *w, *h, RL_MAX_PIXELS);
        return -1;
    }
    return 0;
}

static JSValue rl_texture_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv)
{
    (void)new_target;
    (void)argc;
    (void)argv;
    return JS_ThrowTypeError(ctx, "dyna:raylib: a Texture comes from loadTexture(path) or Image#toTexture()");
}

static JSValue rl_texture_from_image(JSContext* ctx, Image* im)
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

static int rl_load_image_rgba(JSContext* ctx, const char* path, const char* who, Image* out)
{
    Image im = LoadImage(path);
    if (!IsImageValid(im)) {
        JS_ThrowInternalError(ctx, "dyna:raylib: %s: could not load an image from '%s'", who, path);
        return -1;
    }
    if (im.width > RL_MAX_DIM || im.height > RL_MAX_DIM || (int64_t)im.width * im.height > RL_MAX_PIXELS) {
        int w = im.width, h = im.height;
        UnloadImage(im);
        JS_ThrowRangeError(ctx, "dyna:raylib: %s: a %dx%d image is over the size limit", who, w, h);
        return -1;
    }
    if (im.format != PIXELFORMAT_UNCOMPRESSED_R8G8B8A8)
        ImageFormat(&im, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
    if (im.format != PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 || !im.data) {
        UnloadImage(im);
        JS_ThrowInternalError(ctx, "dyna:raylib: %s: could not convert '%s' to RGBA", who, path);
        return -1;
    }
    *out = im;
    return 0;
}

static JSValue rl_load_texture(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    const char* path;
    Image im;
    JSValue t;
    (void)this_val;
    if (rl_args(ctx, argc, argv, 1, "loadTexture(path)") || rl_path(ctx, argv[0], "loadTexture(path)", &path))
        return JS_EXCEPTION;
    if (!rl_ready(ctx, "loadTexture()") || rl_load_image_rgba(ctx, path, "loadTexture", &im)) {
        JS_FreeCString(ctx, path);
        return JS_EXCEPTION;
    }
    JS_FreeCString(ctx, path);
    t = rl_texture_from_image(ctx, &im);
    UnloadImage(im);
    return t;
}

static JSValue rl_gpu_size(JSContext* ctx, JSValueConst this_val, int magic)
{
    JSClassID cid = JS_GetClassID(this_val);
    rl_gpu* g = rl_gpu_of(ctx, this_val, cid == rl_rt_cid ? rl_rt_cid : rl_tex_cid);
    Texture2D t;
    if (!g)
        return JS_EXCEPTION;
    t = g->kind == RL_RT ? g->rt.texture : g->tex;
    return JS_NewInt32(ctx, magic ? t.height : t.width);
}

static JSValue rl_tex_update(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    Rectangle rc = { 0, 0, 0, 0 };
    int have = argc > 1 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1]);
    size_t off, blen, bpe, ab, need;
    JSValue buf;
    uint8_t* base;
    rl_gpu* g;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "dyna:raylib: update(pixels, rect?) needs an RGBA Uint8Array");
    if (have && rl_rect(ctx, argv[1], "update rect", &rc))
        return JS_EXCEPTION;
    if (!rl_ready(ctx, "Texture#update()"))
        return JS_EXCEPTION;
    g = rl_gpu_of(ctx, this_val, rl_tex_cid);
    if (!g)
        return JS_EXCEPTION;
    if (!have)
        rc = (Rectangle) { 0, 0, (float)g->tex.width, (float)g->tex.height };
    if (rc.x != floorf(rc.x) || rc.y != floorf(rc.y) || rc.width != floorf(rc.width) || rc.height != floorf(rc.height)
        || rc.width < 1 || rc.height < 1 || rc.x < 0 || rc.y < 0 || rc.x + rc.width > (float)g->tex.width
        || rc.y + rc.height > (float)g->tex.height)
        return JS_ThrowRangeError(ctx, "dyna:raylib: update rect must be whole pixels inside the %dx%d texture", g->tex.width, g->tex.height);
    buf = JS_GetTypedArrayBuffer(ctx, argv[0], &off, &blen, &bpe);
    if (JS_IsException(buf))
        return JS_EXCEPTION;
    base = JS_GetArrayBuffer(ctx, &ab, buf);
    JS_FreeValue(ctx, buf);
    if (!base)
        return JS_EXCEPTION;
    if (bpe != 1)
        return JS_ThrowTypeError(ctx, "dyna:raylib: update pixels must be a byte view (Uint8Array) of RGBA");
    need = (size_t)rc.width * (size_t)rc.height * 4;
    if (off > ab || blen > ab - off || blen < need)
        return JS_ThrowRangeError(ctx, "dyna:raylib: update needs %zu bytes of RGBA for a %dx%d area, got %zu", need, (int)rc.width, (int)rc.height, blen);
    if (rl.depth)
        rlDrawRenderBatchActive();
    UpdateTextureRec(g->tex, rc, base + off);
    return JS_UNDEFINED;
}

static JSValue rl_gpu_filter(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    static const char* const names[] = { "point", "bilinear", "trilinear" };
    JSClassID cid = JS_GetClassID(this_val);
    const char* s;
    int mode = -1, i;
    rl_gpu* g;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "dyna:raylib: setFilter(\"point\" | \"bilinear\" | \"trilinear\")");
    s = JS_ToCString(ctx, argv[0]);
    if (!s)
        return JS_EXCEPTION;
    for (i = 0; i < 3; i++)
        if (!strcmp(s, names[i]))
            mode = i;
    JS_FreeCString(ctx, s);
    if (mode < 0)
        return JS_ThrowRangeError(ctx, "dyna:raylib: unknown filter (point, bilinear, trilinear)");
    if (!rl_ready(ctx, "setFilter()"))
        return JS_EXCEPTION;
    g = rl_gpu_of(ctx, this_val, cid == rl_rt_cid ? rl_rt_cid : rl_tex_cid);
    if (!g)
        return JS_EXCEPTION;
    SetTextureFilter(g->kind == RL_RT ? g->rt.texture : g->tex, mode);
    return JS_UNDEFINED;
}

static JSValue rl_rt_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv)
{
    int w, h;
    rl_gpu* g;
    (void)new_target;
    if (rl_args(ctx, argc, argv, 2, "new RenderTexture(width, height)") || rl_dims(ctx, argv[0], argv[1], "RenderTexture", &w, &h))
        return JS_EXCEPTION;
    if (!rl_ready(ctx, "new RenderTexture()"))
        return JS_EXCEPTION;
    g = rl_gpu_new(ctx, RL_RT);
    if (!g)
        return JS_EXCEPTION;
    if (rl.depth)
        rlDrawRenderBatchActive();
    g->rt = LoadRenderTexture(w, h);
    if (!IsRenderTextureValid(g->rt)) {
        dyn_nat_free(g);
        return JS_ThrowInternalError(ctx, "dyna:raylib: the GPU refused a %dx%d render texture", w, h);
    }
    return rl_wrap_gpu(ctx, rl_rt_cid, g);
}

static JSValue rl_rt_draw(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    rl_gpu* g;
    JSValue r;
    if (argc < 1 || !JS_IsFunction(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "dyna:raylib: draw(fn) needs a function");
    if (!rl_ready(ctx, "RenderTexture#draw()"))
        return JS_EXCEPTION;
    g = rl_gpu_of(ctx, this_val, rl_rt_cid);
    if (!g)
        return JS_EXCEPTION;
    if (rl.in_texture || rl.in_mode2d || rl.in_mode3d)
        return JS_ThrowTypeError(ctx, "dyna:raylib: RenderTexture#draw() cannot run inside another draw(), mode2D() or mode3D()");
    BeginTextureMode(g->rt);
    rl.in_texture = 1;
    rl.active_rt = g;
    rl.depth++;
    r = JS_Call(ctx, argv[0], JS_UNDEFINED, 0, NULL);
    rl.depth--;
    rl.active_rt = NULL;
    rl.in_texture = 0;
    EndTextureMode();
    if (g->pending_free) {
        rl_gpu_unload(g);
        rl_gpu_unlink(g);
        dyn_nat_free(g);
    }
    rl_after_scope();
    if (rl_sync_result(ctx, r, "RenderTexture#draw(fn)"))
        return JS_EXCEPTION;
    return JS_UNDEFINED;
}

static JSValue rl_rgba_object(JSContext* ctx, int w, int h, const uint8_t* px)
{
    JSValue o = JS_NewObject(ctx), u8;
    if (JS_IsException(o))
        return o;
    u8 = dyn_nat_u8_copy(ctx, px, (size_t)w * (size_t)h * 4);
    if (JS_IsException(u8) || JS_DefinePropertyValueStr(ctx, o, "width", JS_NewInt32(ctx, w), JS_PROP_C_W_E) < 0
        || JS_DefinePropertyValueStr(ctx, o, "height", JS_NewInt32(ctx, h), JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, o);
        JS_FreeValue(ctx, u8);
        return JS_EXCEPTION;
    }
    if (JS_DefinePropertyValueStr(ctx, o, "pixels", u8, JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, o);
        return JS_EXCEPTION;
    }
    return o;
}

static JSValue rl_rt_read(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    rl_gpu* g;
    Image im;
    JSValue o;
    (void)argc;
    (void)argv;
    if (!rl_ready(ctx, "RenderTexture#readPixels()"))
        return JS_EXCEPTION;
    g = rl_gpu_of(ctx, this_val, rl_rt_cid);
    if (!g)
        return JS_EXCEPTION;
    if (g == rl.active_rt)
        return JS_ThrowTypeError(ctx, "dyna:raylib: readPixels() cannot run inside the same RenderTexture's draw()");
    if (rl.depth)
        rlDrawRenderBatchActive();
    im = LoadImageFromTexture(g->rt.texture);
    if (!IsImageValid(im))
        return JS_ThrowInternalError(ctx, "dyna:raylib: could not read the render texture back");
    if (im.format != PIXELFORMAT_UNCOMPRESSED_R8G8B8A8)
        ImageFormat(&im, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
    ImageFlipVertical(&im);
    o = rl_rgba_object(ctx, im.width, im.height, im.data);
    UnloadImage(im);
    return o;
}

static JSValue rl_image_wrap(JSContext* ctx, JSValueConst new_target, Image im)
{
    Image* p = dyn_nat_malloc(sizeof(*p));
    if (!p) {
        UnloadImage(im);
        return JS_ThrowOutOfMemory(ctx);
    }
    *p = im;
    return dyn_res_wrap(ctx, new_target, rl_img_cid, p, rl_img_dispose);
}

static JSValue rl_image_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv)
{
    int w, h;
    Color c = BLANK;
    const uint8_t* px = NULL;
    size_t off = 0, blen = 0, bpe = 0, ab = 0;
    Image im;
    if (rl_args(ctx, argc, argv, 2, "new Image(width, height, fill?)") || rl_dims(ctx, argv[0], argv[1], "Image", &w, &h))
        return JS_EXCEPTION;
    if (argc > 2 && !JS_IsUndefined(argv[2])) {
        if (JS_IsNumber(argv[2])) {
            if (rl_color(ctx, argv[2], "Image fill", &c))
                return JS_EXCEPTION;
        } else {
            JSValue buf = JS_GetTypedArrayBuffer(ctx, argv[2], &off, &blen, &bpe);
            uint8_t* base;
            if (JS_IsException(buf))
                return JS_EXCEPTION;
            base = JS_GetArrayBuffer(ctx, &ab, buf);
            JS_FreeValue(ctx, buf);
            if (!base)
                return JS_EXCEPTION;
            if (bpe != 1)
                return JS_ThrowTypeError(ctx, "dyna:raylib: Image fill is a colour or a Uint8Array of RGBA");
            if (off > ab || blen > ab - off || blen != (size_t)w * (size_t)h * 4)
                return JS_ThrowRangeError(ctx, "dyna:raylib: Image pixels must be exactly %zu bytes of RGBA for %dx%d, got %zu",
                    (size_t)w * (size_t)h * 4, w, h, blen);
            px = base + off;
        }
    }
    im = GenImageColor(w, h, c);
    if (!im.data)
        return JS_ThrowOutOfMemory(ctx);
    if (px)
        memcpy(im.data, px, (size_t)w * (size_t)h * 4);
    return rl_image_wrap(ctx, new_target, im);
}

static JSValue rl_load_image(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    const char* path;
    Image im;
    (void)this_val;
    if (rl_args(ctx, argc, argv, 1, "loadImage(path)") || rl_path(ctx, argv[0], "loadImage(path)", &path))
        return JS_EXCEPTION;
    if (rl_load_image_rgba(ctx, path, "loadImage", &im)) {
        JS_FreeCString(ctx, path);
        return JS_EXCEPTION;
    }
    JS_FreeCString(ctx, path);
    return rl_image_wrap(ctx, JS_UNDEFINED, im);
}

static JSValue rl_img_get(JSContext* ctx, JSValueConst this_val, int magic)
{
    Image* im = rl_img_of(ctx, this_val);
    if (!im)
        return JS_EXCEPTION;
    return JS_NewInt32(ctx, magic ? im->height : im->width);
}

static JSValue rl_img_pixels(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    Image* im = rl_img_of(ctx, this_val);
    (void)argc;
    (void)argv;
    if (!im)
        return JS_EXCEPTION;
    return dyn_nat_u8_copy(ctx, im->data, (size_t)im->width * (size_t)im->height * 4);
}

static JSValue rl_img_export(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    const char* path;
    Image* im;
    int ok;
    if (rl_args(ctx, argc, argv, 1, "export(path)") || rl_path(ctx, argv[0], "export(path)", &path))
        return JS_EXCEPTION;
    im = rl_img_of(ctx, this_val);
    if (!im) {
        JS_FreeCString(ctx, path);
        return JS_EXCEPTION;
    }
    ok = ExportImage(*im, path);
    JS_FreeCString(ctx, path);
    if (!ok)
        return JS_ThrowInternalError(ctx, "dyna:raylib: export failed (the extension picks the format: .png, .bmp, .qoi, .tga)");
    return JS_UNDEFINED;
}

static JSValue rl_img_op(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    int w = 0, h = 0, x = 0, y = 0;
    Image* im;
    if (magic == 0) {
        if (rl_args(ctx, argc, argv, 2, "resize(width, height)") || rl_dims(ctx, argv[0], argv[1], "resize", &w, &h))
            return JS_EXCEPTION;
    } else if (magic == 1) {
        if (rl_args(ctx, argc, argv, 4, "crop(x, y, width, height)") || rl_int_in(ctx, argv[0], "crop x", 0, RL_MAX_DIM - 1, &x)
            || rl_int_in(ctx, argv[1], "crop y", 0, RL_MAX_DIM - 1, &y) || rl_dims(ctx, argv[2], argv[3], "crop", &w, &h))
            return JS_EXCEPTION;
    }
    im = rl_img_of(ctx, this_val);
    if (!im)
        return JS_EXCEPTION;
    switch (magic) {
    case 0:
        ImageResize(im, w, h);
        if (!im->data || im->width != w || im->height != h)
            return JS_ThrowInternalError(ctx, "dyna:raylib: resize to %dx%d failed", w, h);
        break;
    case 1:
        if (x + w > im->width || y + h > im->height)
            return JS_ThrowRangeError(ctx, "dyna:raylib: crop must lie inside the %dx%d image", im->width, im->height);
        ImageCrop(im, (Rectangle) { (float)x, (float)y, (float)w, (float)h });
        break;
    case 2:
        ImageFlipVertical(im);
        break;
    case 3:
        ImageFlipHorizontal(im);
        break;
    }
    return JS_UNDEFINED;
}

static JSValue rl_img_copy(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    Image* im = rl_img_of(ctx, this_val);
    Image c;
    (void)argc;
    (void)argv;
    if (!im)
        return JS_EXCEPTION;
    c = ImageCopy(*im);
    if (!c.data)
        return JS_ThrowOutOfMemory(ctx);
    return rl_image_wrap(ctx, JS_UNDEFINED, c);
}

static JSValue rl_img_to_texture(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    Image* im;
    (void)argc;
    (void)argv;
    if (!rl_ready(ctx, "Image#toTexture()"))
        return JS_EXCEPTION;
    im = rl_img_of(ctx, this_val);
    if (!im)
        return JS_EXCEPTION;
    return rl_texture_from_image(ctx, im);
}

static JSValue rl_font_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv)
{
    (void)new_target;
    (void)argc;
    (void)argv;
    return JS_ThrowTypeError(ctx, "dyna:raylib: a Font comes from loadFont(path, size?)");
}

static JSValue rl_load_font(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    const char* path;
    int size = 32;
    rl_gpu* g;
    (void)this_val;
    if (rl_args(ctx, argc, argv, 1, "loadFont(path, size?)")
        || (argc > 1 && !JS_IsUndefined(argv[1]) && rl_int_in(ctx, argv[1], "loadFont size", 1, 1024, &size))
        || rl_path(ctx, argv[0], "loadFont(path, size?)", &path))
        return JS_EXCEPTION;
    if (!rl_ready(ctx, "loadFont()")) {
        JS_FreeCString(ctx, path);
        return JS_EXCEPTION;
    }
    g = rl_gpu_new(ctx, RL_FONT);
    if (!g) {
        JS_FreeCString(ctx, path);
        return JS_EXCEPTION;
    }
    if (rl.depth)
        rlDrawRenderBatchActive();
    g->font = LoadFontEx(path, size, NULL, 0);
    if (!IsFontValid(g->font) || g->font.texture.id == GetFontDefault().texture.id) {
        dyn_nat_free(g);
        JS_ThrowInternalError(ctx, "dyna:raylib: could not load a font from '%s'", path);
        JS_FreeCString(ctx, path);
        return JS_EXCEPTION;
    }
    JS_FreeCString(ctx, path);
    return rl_wrap_gpu(ctx, rl_font_cid, g);
}

static JSValue rl_font_get(JSContext* ctx, JSValueConst this_val, int magic)
{
    rl_gpu* g = rl_gpu_of(ctx, this_val, rl_font_cid);
    if (!g)
        return JS_EXCEPTION;
    return JS_NewInt32(ctx, magic ? g->font.glyphCount : g->font.baseSize);
}

static JSValue rl_key(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    static const char* const names[] = { "isKeyDown(key)", "isKeyPressed(key)", "isKeyReleased(key)" };
    int k;
    bool b;
    (void)this_val;
    if (rl_args(ctx, argc, argv, 1, names[magic]) || rl_int_in(ctx, argv[0], "a key code", 0, 511, &k))
        return JS_EXCEPTION;
    if (!rl_ready(ctx, names[magic]))
        return JS_EXCEPTION;
    b = magic == 0 ? IsKeyDown(k) : magic == 1 ? IsKeyPressed(k)
                                               : IsKeyReleased(k);
    return JS_NewBool(ctx, b);
}

static JSValue rl_mouse_button(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    static const char* const names[] = { "isMouseButtonDown(button)", "isMouseButtonPressed(button)", "isMouseButtonReleased(button)" };
    int k;
    bool b;
    (void)this_val;
    if (rl_args(ctx, argc, argv, 1, names[magic]) || rl_int_in(ctx, argv[0], "a mouse button", 0, 6, &k))
        return JS_EXCEPTION;
    if (!rl_ready(ctx, names[magic]))
        return JS_EXCEPTION;
    b = magic == 0 ? IsMouseButtonDown(k) : magic == 1 ? IsMouseButtonPressed(k)
                                                       : IsMouseButtonReleased(k);
    return JS_NewBool(ctx, b);
}

static JSValue rl_input0(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    static const char* const names[] = { "getKeyPressed()", "getCharPressed()", "mousePosition()", "mouseDelta()", "mouseWheel()",
        "getGamepadButtonPressed()", "mouseWheelV()" };
    Vector2 v;
    (void)this_val;
    (void)argc;
    (void)argv;
    if (!rl_ready(ctx, names[magic]))
        return JS_EXCEPTION;
    switch (magic) {
    case 0:
        return JS_NewInt32(ctx, GetKeyPressed());
    case 1: {
        int cp = GetCharPressed();
        char buf[5];
        int n = 0;
        if (cp <= 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
            return JS_NewString(ctx, "");
        if (cp < 0x80)
            buf[n++] = (char)cp;
        else if (cp < 0x800) {
            buf[n++] = (char)(0xC0 | (cp >> 6));
            buf[n++] = (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            buf[n++] = (char)(0xE0 | (cp >> 12));
            buf[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            buf[n++] = (char)(0x80 | (cp & 0x3F));
        } else {
            buf[n++] = (char)(0xF0 | (cp >> 18));
            buf[n++] = (char)(0x80 | ((cp >> 12) & 0x3F));
            buf[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            buf[n++] = (char)(0x80 | (cp & 0x3F));
        }
        return JS_NewStringLen(ctx, buf, (size_t)n);
    }
    case 2:
        v = GetMousePosition();
        return rl_xy(ctx, (double)v.x, (double)v.y, "x", "y");
    case 3:
        v = GetMouseDelta();
        return rl_xy(ctx, (double)v.x, (double)v.y, "x", "y");
    case 4:
        return JS_NewFloat64(ctx, (double)GetMouseWheelMove());
    case 5:
        return JS_NewInt32(ctx, GetGamepadButtonPressed());
    default:
        v = GetMouseWheelMoveV();
        return rl_xy(ctx, (double)v.x, (double)v.y, "x", "y");
    }
}

static JSValue rl_gamepad(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    static const char* const names[] = { "isGamepadAvailable(gamepad)", "gamepadName(gamepad)", "gamepadAxis(gamepad, axis)",
        "isGamepadButtonDown(gamepad, button)", "isGamepadButtonPressed(gamepad, button)", "isGamepadButtonReleased(gamepad, button)",
        "gamepadAxisCount(gamepad)", "setGamepadVibration(gamepad, leftMotor, rightMotor, seconds)" };
    static const int need[] = { 1, 1, 2, 2, 2, 2, 1, 4 };
    int pad, k = 0;
    float m[3] = { 0, 0, 0 };
    const char* s;
    (void)this_val;
    if (rl_args(ctx, argc, argv, need[magic], names[magic]) || rl_int_in(ctx, argv[0], "a gamepad index", 0, 3, &pad))
        return JS_EXCEPTION;
    if (magic == 2 && rl_int_in(ctx, argv[1], "a gamepad axis", 0, 5, &k))
        return JS_EXCEPTION;
    if (magic >= 3 && magic <= 5 && rl_int_in(ctx, argv[1], "a gamepad button", 0, 17, &k))
        return JS_EXCEPTION;
    if (magic == 7
        && (rl_num_in(ctx, argv[1], "setGamepadVibration leftMotor", 0, 1, &m[0]) || rl_num_in(ctx, argv[2], "setGamepadVibration rightMotor", 0, 1, &m[1])
            || rl_num_in(ctx, argv[3], "setGamepadVibration seconds", 0, 60, &m[2])))
        return JS_EXCEPTION;
    if (!rl_ready(ctx, names[magic]))
        return JS_EXCEPTION;
    switch (magic) {
    case 0:
        return JS_NewBool(ctx, IsGamepadAvailable(pad));
    case 1:
        s = IsGamepadAvailable(pad) ? GetGamepadName(pad) : NULL;
        return s ? JS_NewString(ctx, s) : JS_NULL;
    case 2:
        return JS_NewFloat64(ctx, (double)GetGamepadAxisMovement(pad, k));
    case 3:
        return JS_NewBool(ctx, IsGamepadButtonDown(pad, k));
    case 4:
        return JS_NewBool(ctx, IsGamepadButtonPressed(pad, k));
    case 5:
        return JS_NewBool(ctx, IsGamepadButtonReleased(pad, k));
    case 6:
        return JS_NewInt32(ctx, IsGamepadAvailable(pad) ? GetGamepadAxisCount(pad) : 0);
    default:
        SetGamepadVibration(pad, m[0], m[1], m[2]);
        return JS_UNDEFINED;
    }
}

static JSValue rl_audio_fn(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    float v = 1;
    rl_snd* s;
    (void)this_val;
    if (magic == 3 && (rl_args(ctx, argc, argv, 1, "setMasterVolume(volume)") || rl_num_in(ctx, argv[0], "setMasterVolume volume", 0, 1, &v)))
        return JS_EXCEPTION;
    if (!rl_on_main(ctx, "the audio device"))
        return JS_EXCEPTION;
    switch (magic) {
    case 0:
        if (!IsAudioDeviceReady())
            InitAudioDevice();
        if (!IsAudioDeviceReady())
            return JS_ThrowInternalError(ctx, "dyna:raylib: initAudio() found no audio device");
        return JS_UNDEFINED;
    case 1:
        s = rl.snd;
        while (s) {
            rl_snd* next = s->next;
            rl_snd_unload(s);
            s->next = s->prev = NULL;
            s = next;
        }
        rl.snd = NULL;
        if (IsAudioDeviceReady())
            CloseAudioDevice();
        return JS_UNDEFINED;
    case 2:
        return JS_NewBool(ctx, IsAudioDeviceReady());
    default:
        if (!rl_audio_ready(ctx, "setMasterVolume()"))
            return JS_EXCEPTION;
        SetMasterVolume(v);
        return JS_UNDEFINED;
    }
}

static JSValue rl_snd_wrap(JSContext* ctx, JSClassID cid, rl_snd* s)
{
    s->alive = 1;
    rl_snd_link(s);
    return dyn_res_wrap(ctx, JS_UNDEFINED, cid, s, rl_snd_dispose);
}

static JSValue rl_load_audio(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    const char* who = magic ? "loadMusic(path)" : "loadSound(path)";
    const char* path;
    rl_snd* s;
    int ok;
    (void)this_val;
    if (rl_args(ctx, argc, argv, 1, who) || rl_path(ctx, argv[0], who, &path))
        return JS_EXCEPTION;
    if (!rl_audio_ready(ctx, who)) {
        JS_FreeCString(ctx, path);
        return JS_EXCEPTION;
    }
    s = dyn_nat_calloc(1, sizeof(*s));
    if (!s) {
        JS_FreeCString(ctx, path);
        return JS_ThrowOutOfMemory(ctx);
    }
    s->kind = magic ? RL_MUSIC : RL_SOUND;
    if (magic) {
        s->music = LoadMusicStream(path);
        ok = IsMusicValid(s->music);
    } else {
        s->sound = LoadSound(path);
        ok = IsSoundValid(s->sound);
    }
    if (!ok) {
        dyn_nat_free(s);
        JS_ThrowInternalError(ctx, "dyna:raylib: %s: could not load audio from '%s'", who, path);
        JS_FreeCString(ctx, path);
        return JS_EXCEPTION;
    }
    JS_FreeCString(ctx, path);
    return rl_snd_wrap(ctx, magic ? rl_music_cid : rl_sound_cid, s);
}

static JSValue rl_play(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    JSClassID cid = JS_GetClassID(this_val);
    rl_snd* s;
    (void)argc;
    (void)argv;
    if (!rl_audio_ready(ctx, "an audio method"))
        return JS_EXCEPTION;
    s = rl_snd_of(ctx, this_val, cid == rl_music_cid ? rl_music_cid : cid == rl_stream_cid ? rl_stream_cid
                                                                                           : rl_sound_cid);
    if (!s)
        return JS_EXCEPTION;
    switch (s->kind * 4 + magic) {
    case RL_SOUND * 4 + 0:
        PlaySound(s->sound);
        break;
    case RL_SOUND * 4 + 1:
        StopSound(s->sound);
        break;
    case RL_SOUND * 4 + 2:
        PauseSound(s->sound);
        break;
    case RL_SOUND * 4 + 3:
        ResumeSound(s->sound);
        break;
    case RL_MUSIC * 4 + 0:
        PlayMusicStream(s->music);
        break;
    case RL_MUSIC * 4 + 1:
        StopMusicStream(s->music);
        break;
    case RL_MUSIC * 4 + 2:
        PauseMusicStream(s->music);
        break;
    case RL_MUSIC * 4 + 3:
        ResumeMusicStream(s->music);
        break;
    case RL_STREAM * 4 + 0:
        PlayAudioStream(s->stream);
        break;
    case RL_STREAM * 4 + 1:
        StopAudioStream(s->stream);
        break;
    case RL_STREAM * 4 + 2:
        PauseAudioStream(s->stream);
        break;
    case RL_STREAM * 4 + 3:
        ResumeAudioStream(s->stream);
        break;
    }
    return JS_UNDEFINED;
}

static JSValue rl_snd_get(JSContext* ctx, JSValueConst this_val, int magic)
{
    JSClassID cid = JS_GetClassID(this_val);
    rl_snd* s;
    if (!rl_audio_ready(ctx, "an audio property"))
        return JS_EXCEPTION;
    s = rl_snd_of(ctx, this_val, cid == rl_music_cid ? rl_music_cid : cid == rl_stream_cid ? rl_stream_cid
                                                                                           : rl_sound_cid);
    if (!s)
        return JS_EXCEPTION;
    switch (magic) {
    case 0:
        return JS_NewBool(ctx, s->kind == RL_SOUND ? IsSoundPlaying(s->sound) : s->kind == RL_MUSIC ? IsMusicStreamPlaying(s->music)
                                                                                                    : IsAudioStreamPlaying(s->stream));
    case 1:
        return JS_NewFloat64(ctx, (double)GetMusicTimeLength(s->music));
    case 2:
        return JS_NewFloat64(ctx, (double)GetMusicTimePlayed(s->music));
    default:
        return JS_NewBool(ctx, IsAudioStreamProcessed(s->stream));
    }
}

static JSValue rl_snd_set(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    static const char* const names[] = { "setVolume(volume)", "setPitch(pitch)", "setPan(pan)", "seek(seconds)", "setLooping(on)" };
    static const double lo[] = { 0, 0.01, 0, 0, 0 }, hi[] = { 1, 16, 1, 1e7, 1 };
    JSClassID cid = JS_GetClassID(this_val);
    float v = 0;
    int on = 0;
    rl_snd* s;
    if (rl_args(ctx, argc, argv, 1, names[magic]))
        return JS_EXCEPTION;
    if (magic == 4) {
        if (!JS_IsBool(argv[0]))
            return JS_ThrowTypeError(ctx, "dyna:raylib: setLooping(on) needs a boolean");
        on = JS_ToBool(ctx, argv[0]);
    } else if (rl_num_in(ctx, argv[0], names[magic], lo[magic], hi[magic], &v)) {
        return JS_EXCEPTION;
    }
    if (!rl_audio_ready(ctx, names[magic]))
        return JS_EXCEPTION;
    s = rl_snd_of(ctx, this_val, cid == rl_music_cid ? rl_music_cid : cid == rl_stream_cid ? rl_stream_cid
                                                                                           : rl_sound_cid);
    if (!s)
        return JS_EXCEPTION;
    switch (s->kind * 8 + magic) {
    case RL_SOUND * 8 + 0:
        SetSoundVolume(s->sound, v);
        break;
    case RL_SOUND * 8 + 1:
        SetSoundPitch(s->sound, v);
        break;
    case RL_SOUND * 8 + 2:
        SetSoundPan(s->sound, v);
        break;
    case RL_MUSIC * 8 + 0:
        SetMusicVolume(s->music, v);
        break;
    case RL_MUSIC * 8 + 1:
        SetMusicPitch(s->music, v);
        break;
    case RL_MUSIC * 8 + 2:
        SetMusicPan(s->music, v);
        break;
    case RL_MUSIC * 8 + 3:
        if (v > GetMusicTimeLength(s->music))
            return JS_ThrowRangeError(ctx, "dyna:raylib: seek past the end of the music");
        SeekMusicStream(s->music, v);
        break;
    case RL_MUSIC * 8 + 4:
        s->music.looping = on;
        break;
    case RL_STREAM * 8 + 0:
        SetAudioStreamVolume(s->stream, v);
        break;
    case RL_STREAM * 8 + 1:
        SetAudioStreamPitch(s->stream, v);
        break;
    case RL_STREAM * 8 + 2:
        SetAudioStreamPan(s->stream, v);
        break;
    default:
        return JS_ThrowTypeError(ctx, "dyna:raylib: %s does not apply to this kind of audio", names[magic]);
    }
    return JS_UNDEFINED;
}

static JSValue rl_music_update(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    rl_snd* s;
    (void)argc;
    (void)argv;
    if (!rl_audio_ready(ctx, "Music#update()"))
        return JS_EXCEPTION;
    s = rl_snd_of(ctx, this_val, rl_music_cid);
    if (!s)
        return JS_EXCEPTION;
    UpdateMusicStream(s->music);
    return JS_UNDEFINED;
}

static JSValue rl_sound_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv)
{
    (void)new_target;
    (void)argc;
    (void)argv;
    return JS_ThrowTypeError(ctx, "dyna:raylib: Sound and Music come from loadSound(path) and loadMusic(path)");
}

static JSValue rl_stream_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv)
{
    int rate, ch;
    rl_snd* s;
    JSValue obj;
    if (rl_args(ctx, argc, argv, 2, "new AudioStream(sampleRate, channels)") || rl_int_in(ctx, argv[0], "AudioStream sampleRate", 8000, 192000, &rate)
        || rl_int_in(ctx, argv[1], "AudioStream channels", 1, 2, &ch))
        return JS_EXCEPTION;
    if (!rl_audio_ready(ctx, "new AudioStream()"))
        return JS_EXCEPTION;
    s = dyn_nat_calloc(1, sizeof(*s));
    if (!s)
        return JS_ThrowOutOfMemory(ctx);
    s->kind = RL_STREAM;
    s->channels = ch;
    s->stream = LoadAudioStream((unsigned)rate, 32, (unsigned)ch);
    if (!IsAudioStreamValid(s->stream)) {
        dyn_nat_free(s);
        return JS_ThrowInternalError(ctx, "dyna:raylib: the audio device refused a %d Hz %d-channel stream", rate, ch);
    }
    s->alive = 1;
    rl_snd_link(s);
    obj = dyn_res_wrap(ctx, new_target, rl_stream_cid, s, rl_snd_dispose);
    return obj;
}

static JSValue rl_stream_update(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    size_t off, blen, bpe, ab;
    JSValue buf;
    uint8_t* base;
    rl_snd* s;
    if (argc < 1 || JS_GetClassID(argv[0]) != rl_f32_cid)
        return JS_ThrowTypeError(ctx, "dyna:raylib: update(samples) needs a Float32Array of interleaved samples");
    buf = JS_GetTypedArrayBuffer(ctx, argv[0], &off, &blen, &bpe);
    if (JS_IsException(buf))
        return JS_EXCEPTION;
    base = JS_GetArrayBuffer(ctx, &ab, buf);
    JS_FreeValue(ctx, buf);
    if (!base)
        return JS_EXCEPTION;
    if (!rl_audio_ready(ctx, "AudioStream#update()"))
        return JS_EXCEPTION;
    s = rl_snd_of(ctx, this_val, rl_stream_cid);
    if (!s)
        return JS_EXCEPTION;
    if (off > ab || blen > ab - off || (blen / 4) % (size_t)s->channels || blen / 4 / (size_t)s->channels > INT32_MAX)
        return JS_ThrowRangeError(ctx, "dyna:raylib: update needs whole frames of %d interleaved samples", s->channels);
    if (!IsAudioStreamProcessed(s->stream))
        return JS_ThrowTypeError(ctx, "dyna:raylib: the stream has not played its last buffer yet (check processed first)");
    if (blen)
        UpdateAudioStream(s->stream, base + off, (int)(blen / 4 / (size_t)s->channels));
    return JS_UNDEFINED;
}

static JSValue rl_load_wave(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    const char* path;
    Wave w;
    float* samples;
    size_t n;
    JSValue o, ab, ta;
    JSValueConst targs[3];
    (void)this_val;
    if (rl_args(ctx, argc, argv, 1, "loadWave(path)") || rl_path(ctx, argv[0], "loadWave(path)", &path))
        return JS_EXCEPTION;
    w = LoadWave(path);
    if (!IsWaveValid(w)) {
        JS_ThrowInternalError(ctx, "dyna:raylib: loadWave: could not load audio from '%s'", path);
        JS_FreeCString(ctx, path);
        return JS_EXCEPTION;
    }
    JS_FreeCString(ctx, path);
    n = (size_t)w.frameCount * w.channels;
    if (n > RL_MAX_SAMPLES) {
        UnloadWave(w);
        return JS_ThrowRangeError(ctx, "dyna:raylib: loadWave: %zu samples is over the %d-sample limit", n, RL_MAX_SAMPLES);
    }
    samples = LoadWaveSamples(w);
    if (!samples) {
        UnloadWave(w);
        return JS_ThrowOutOfMemory(ctx);
    }
    ab = JS_NewArrayBufferCopy(ctx, (const uint8_t*)samples, n * sizeof(float));
    UnloadWaveSamples(samples);
    if (JS_IsException(ab)) {
        UnloadWave(w);
        return ab;
    }
    targs[0] = ab;
    targs[1] = JS_NewInt32(ctx, 0);
    targs[2] = JS_NewFloat64(ctx, (double)n);
    ta = JS_NewTypedArray(ctx, 3, targs, JS_TYPED_ARRAY_FLOAT32);
    JS_FreeValue(ctx, ab);
    if (JS_IsException(ta)) {
        UnloadWave(w);
        return ta;
    }
    o = JS_NewObject(ctx);
    if (JS_IsException(o) || JS_DefinePropertyValueStr(ctx, o, "sampleRate", JS_NewInt32(ctx, (int)w.sampleRate), JS_PROP_C_W_E) < 0
        || JS_DefinePropertyValueStr(ctx, o, "channels", JS_NewInt32(ctx, (int)w.channels), JS_PROP_C_W_E) < 0) {
        UnloadWave(w);
        JS_FreeValue(ctx, o);
        JS_FreeValue(ctx, ta);
        return JS_EXCEPTION;
    }
    UnloadWave(w);
    if (JS_DefinePropertyValueStr(ctx, o, "samples", ta, JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, o);
        return JS_EXCEPTION;
    }
    return o;
}

static JSValue rl_version(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)this_val;
    (void)argc;
    (void)argv;
    return JS_NewString(ctx, RAYLIB_VERSION);
}

static JSValue rl_set_log_level(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    static const char* const names[] = { "all", "trace", "debug", "info", "warning", "error", "fatal", "none" };
    const char* s;
    int i, lvl = -1;
    (void)this_val;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "dyna:raylib: setLogLevel(\"all\" | \"trace\" | \"debug\" | \"info\" | \"warning\" | \"error\" | \"fatal\" | \"none\")");
    s = JS_ToCString(ctx, argv[0]);
    if (!s)
        return JS_EXCEPTION;
    for (i = 0; i < (int)countof(names); i++)
        if (!strcmp(s, names[i]))
            lvl = i;
    JS_FreeCString(ctx, s);
    if (lvl < 0)
        return JS_ThrowRangeError(ctx, "dyna:raylib: unknown log level (all, trace, debug, info, warning, error, fatal, none)");
    SetTraceLogLevel(lvl);
    return JS_UNDEFINED;
}

static JSValue rl_rgba(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    int c[4] = { 0, 0, 0, 255 }, i;
    (void)this_val;
    if (rl_args(ctx, argc, argv, 3, "rgba(r, g, b, a?)"))
        return JS_EXCEPTION;
    for (i = 0; i < 4; i++) {
        if (i == 3 && (argc < 4 || JS_IsUndefined(argv[3])))
            break;
        if (rl_int_in(ctx, argv[i], "a colour channel", 0, 255, &c[i]))
            return JS_EXCEPTION;
    }
    return JS_NewFloat64(ctx, (double)(((uint32_t)c[0] << 24) | ((uint32_t)c[1] << 16) | ((uint32_t)c[2] << 8) | (uint32_t)c[3]));
}

static JSValue rl_table_object(JSContext* ctx, const rl_named* t, int n)
{
    JSValue o = JS_NewObject(ctx);
    int i;
    if (JS_IsException(o))
        return o;
    for (i = 0; i < n; i++)
        if (JS_DefinePropertyValueStr(ctx, o, t[i].name, JS_NewFloat64(ctx, (double)t[i].value), JS_PROP_ENUMERABLE) < 0) {
            JS_FreeValue(ctx, o);
            return JS_EXCEPTION;
        }
    if (JS_PreventExtensions(ctx, o) < 0) {
        JS_FreeValue(ctx, o);
        return JS_EXCEPTION;
    }
    return o;
}

#define RL_MAX_POINTS 65536

static const char* const rl_cursor_names[] = { "default", "arrow", "ibeam", "crosshair", "pointer", "resize-ew", "resize-ns",
    "resize-nwse", "resize-nesw", "resize-all", "not-allowed" };
static const char* const rl_blend_names[] = { "alpha", "additive", "multiplied", "add-colors", "subtract-colors", "alpha-premultiply" };

static int rl_name_index(JSContext* ctx, JSValueConst v, const char* what, const char* const* names, int n, int* out)
{
    const char* s;
    int i;
    if (!JS_IsString(v)) {
        JS_ThrowTypeError(ctx, "dyna:raylib: %s must be a string", what);
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
    JS_ThrowRangeError(ctx, "dyna:raylib: %s '%s' is not one of %s .. %s", what, s, names[0], names[n - 1]);
    JS_FreeCString(ctx, s);
    return -1;
}

static int rl_points(JSContext* ctx, JSValueConst v, const char* who, int min, const Vector2** pts, int* n)
{
    size_t off, blen, bpe, ab, i, cnt;
    const float* f;
    uint8_t* base;
    JSValue buf;
    if (JS_GetClassID(v) != rl_f32_cid) {
        JS_ThrowTypeError(ctx, "dyna:raylib: %s takes a Float32Array of x, y pairs", who);
        return -1;
    }
    buf = JS_GetTypedArrayBuffer(ctx, v, &off, &blen, &bpe);
    if (JS_IsException(buf))
        return -1;
    base = JS_GetArrayBuffer(ctx, &ab, buf);
    JS_FreeValue(ctx, buf);
    if (!base)
        return -1;
    if (off > ab || blen > ab - off || blen % (2 * sizeof(float))) {
        JS_ThrowRangeError(ctx, "dyna:raylib: %s needs an even number of coordinates (x, y pairs)", who);
        return -1;
    }
    cnt = blen / (2 * sizeof(float));
    if (cnt < (size_t)min || cnt > RL_MAX_POINTS) {
        JS_ThrowRangeError(ctx, "dyna:raylib: %s takes %d to %d points, got %zu", who, min, RL_MAX_POINTS, cnt);
        return -1;
    }
    f = (const float*)(base + off);
    for (i = 0; i < cnt * 2; i++)
        if (!isfinite(f[i]) || fabsf(f[i]) > (float)RL_COORD_MAX) {
            JS_ThrowRangeError(ctx, "dyna:raylib: %s coordinate %zu must be a finite number within +-1e7", who, i);
            return -1;
        }
    *pts = (const Vector2*)f;
    *n = (int)cnt;
    return 0;
}

static int rl_text_arg(JSContext* ctx, JSValueConst v, const char* who, const char** out)
{
    size_t n;
    const char* s;
    if (!JS_IsString(v)) {
        JS_ThrowTypeError(ctx, "dyna:raylib: %s needs a text string", who);
        return -1;
    }
    s = JS_ToCStringLen(ctx, &n, v);
    if (!s)
        return -1;
    if (dyn_wtf8_has_surrogate(s, n)) {
        JS_FreeCString(ctx, s);
        JS_ThrowTypeError(ctx, "dyna:raylib: %s needs well-formed text (it holds a lone surrogate)", who);
        return -1;
    }
    *out = s;
    return 0;
}

static JSValue rl_win_get2(JSContext* ctx, JSValueConst this_val, int magic)
{
    Vector2 v;
    if (!rl_win_live(ctx, this_val))
        return JS_EXCEPTION;
    switch (magic) {
    case 0:
        return JS_NewBool(ctx, IsWindowFullscreen());
    case 1:
        return JS_NewBool(ctx, IsWindowMaximized());
    case 2:
        return JS_NewBool(ctx, IsWindowResized());
    case 3:
        v = GetWindowPosition();
        return rl_xy(ctx, (double)v.x, (double)v.y, "x", "y");
    case 4:
        return JS_NewBool(ctx, IsWindowHidden());
    default:
        return JS_NewInt32(ctx, GetCurrentMonitor());
    }
}

static JSValue rl_win_act(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    (void)argc;
    (void)argv;
    if (!rl_win_live(ctx, this_val))
        return JS_EXCEPTION;
    if (rl.depth)
        return JS_ThrowTypeError(ctx, "dyna:raylib: window state changes run outside frame() and RenderTexture#draw()");
    switch (magic) {
    case 0:
        ToggleFullscreen();
        break;
    case 1:
        ToggleBorderlessWindowed();
        break;
    case 2:
        MaximizeWindow();
        break;
    case 3:
        MinimizeWindow();
        break;
    case 4:
        RestoreWindow();
        break;
    default:
        SetWindowFocused();
        break;
    }
    return JS_UNDEFINED;
}

static JSValue rl_win_flag(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    static const char* const usage[] = { "setResizable(on)", "setTopmost(on)", "setDecorated(on)", "setAlwaysRun(on)",
        "setMousePassthrough(on)" };
    static const unsigned int flag[] = { FLAG_WINDOW_RESIZABLE, FLAG_WINDOW_TOPMOST, FLAG_WINDOW_UNDECORATED, FLAG_WINDOW_ALWAYS_RUN,
        FLAG_WINDOW_MOUSE_PASSTHROUGH };
    int on;
    if (argc < 1 || !JS_IsBool(argv[0]))
        return JS_ThrowTypeError(ctx, "dyna:raylib: %s needs a boolean", usage[magic]);
    on = JS_ToBool(ctx, argv[0]);
    if (magic == 2)
        on = !on;
    if (!rl_win_live(ctx, this_val))
        return JS_EXCEPTION;
    if (rl.depth)
        return JS_ThrowTypeError(ctx, "dyna:raylib: %s runs outside frame() and RenderTexture#draw()", usage[magic]);
    if (on)
        SetWindowState(flag[magic]);
    else
        ClearWindowState(flag[magic]);
    return JS_UNDEFINED;
}

static JSValue rl_win_set_opacity(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    float o;
    if (rl_args(ctx, argc, argv, 1, "setOpacity(opacity)") || rl_num_in(ctx, argv[0], "setOpacity opacity", 0, 1, &o))
        return JS_EXCEPTION;
    if (!rl_win_live(ctx, this_val))
        return JS_EXCEPTION;
    SetWindowOpacity(o);
    return JS_UNDEFINED;
}

static JSValue rl_win_limit(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    static const char* const usage[] = { "setMinSize(width, height)", "setMaxSize(width, height)" };
    int w, h;
    if (rl_args(ctx, argc, argv, 2, usage[magic]) || rl_dims(ctx, argv[0], argv[1], usage[magic], &w, &h))
        return JS_EXCEPTION;
    if (!rl_win_live(ctx, this_val))
        return JS_EXCEPTION;
    if (magic)
        SetWindowMaxSize(w, h);
    else
        SetWindowMinSize(w, h);
    return JS_UNDEFINED;
}

static JSValue rl_win_scope(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    const char* who = magic ? "scissor(x, y, width, height, draw)" : "blendMode(mode, draw)";
    int fi = magic ? 4 : 1, mode = 0, r[4] = { 0, 0, 0, 0 }, i;
    JSValue res;
    if (argc <= fi || !JS_IsFunction(ctx, argv[fi]))
        return JS_ThrowTypeError(ctx, "dyna:raylib: %s needs a function last", who);
    if (magic) {
        for (i = 0; i < 4; i++)
            if (rl_int_in(ctx, argv[i], i < 2 ? "scissor position" : "scissor size", i < 2 ? -RL_MAX_DIM * 4 : 0, RL_MAX_DIM * 4, &r[i]))
                return JS_EXCEPTION;
    } else if (rl_name_index(ctx, argv[0], "blendMode mode", rl_blend_names, (int)countof(rl_blend_names), &mode)) {
        return JS_EXCEPTION;
    }
    if (!rl_win_live(ctx, this_val))
        return JS_EXCEPTION;
    if (!rl.in_frame && !rl.in_texture)
        return JS_ThrowTypeError(ctx, "dyna:raylib: %s runs inside frame() or RenderTexture#draw()", who);
    if (magic ? rl.in_scissor : rl.in_blend)
        return JS_ThrowTypeError(ctx, "dyna:raylib: %s cannot nest inside another %s", who, magic ? "scissor()" : "blendMode()");
    if (magic) {
        BeginScissorMode(r[0], r[1], r[2], r[3]);
        rl.in_scissor = 1;
    } else {
        BeginBlendMode(mode);
        rl.in_blend = 1;
        rl.blend_mode = mode;
    }
    rl.depth++;
    res = JS_Call(ctx, argv[fi], JS_UNDEFINED, 0, NULL);
    rl.depth--;
    if (magic) {
        EndScissorMode();
        rl.in_scissor = 0;
    } else {
        EndBlendMode();
        rl.in_blend = 0;
    }
    rl_after_scope();
    if (rl_sync_result(ctx, res, who))
        return JS_EXCEPTION;
    return JS_UNDEFINED;
}

static JSValue rl_draw_shape(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    static const int need[] = { 8, 7, 8 };
    static const char* const names[] = { "drawRing(cx, cy, innerRadius, outerRadius, startAngle, endAngle, segments, color)",
        "drawCircleSector(cx, cy, radius, startAngle, endAngle, segments, color)",
        "drawRectangleGradient(x, y, width, height, topLeft, bottomLeft, topRight, bottomRight)" };
    float f[6];
    int seg = 0;
    Color c[4];
    (void)this_val;
    if (rl_args(ctx, argc, argv, need[magic], names[magic]))
        return JS_EXCEPTION;
    switch (magic) {
    case 0:
        if (rl_nums(ctx, argv, 2, names[0], f) || rl_num_in(ctx, argv[2], "drawRing innerRadius", 0, RL_COORD_MAX, &f[2])
            || rl_num_in(ctx, argv[3], "drawRing outerRadius", 0, RL_COORD_MAX, &f[3]) || rl_num(ctx, argv[4], names[0], &f[4])
            || rl_num(ctx, argv[5], names[0], &f[5]) || rl_int_in(ctx, argv[6], "drawRing segments", 0, 4096, &seg)
            || rl_color(ctx, argv[7], "drawRing color", &c[0]))
            return JS_EXCEPTION;
        break;
    case 1:
        if (rl_nums(ctx, argv, 2, names[1], f) || rl_num_in(ctx, argv[2], "drawCircleSector radius", 0, RL_COORD_MAX, &f[2])
            || rl_num(ctx, argv[3], names[1], &f[3]) || rl_num(ctx, argv[4], names[1], &f[4])
            || rl_int_in(ctx, argv[5], "drawCircleSector segments", 0, 4096, &seg) || rl_color(ctx, argv[6], "drawCircleSector color", &c[0]))
            return JS_EXCEPTION;
        break;
    default:
        if (rl_nums(ctx, argv, 4, names[2], f) || rl_color(ctx, argv[4], "drawRectangleGradient topLeft", &c[0])
            || rl_color(ctx, argv[5], "drawRectangleGradient bottomLeft", &c[1]) || rl_color(ctx, argv[6], "drawRectangleGradient topRight", &c[2])
            || rl_color(ctx, argv[7], "drawRectangleGradient bottomRight", &c[3]))
            return JS_EXCEPTION;
        break;
    }
    if (!rl_ready(ctx, names[magic]))
        return JS_EXCEPTION;
    switch (magic) {
    case 0:
        DrawRing((Vector2) { f[0], f[1] }, f[2], f[3], f[4], f[5], seg, c[0]);
        break;
    case 1:
        DrawCircleSector((Vector2) { f[0], f[1] }, f[2], f[3], f[4], seg, c[0]);
        break;
    default:
        DrawRectangleGradientEx((Rectangle) { f[0], f[1], f[2], f[3] }, c[0], c[1], c[3], c[2]);
        break;
    }
    return JS_UNDEFINED;
}

static JSValue rl_draw_points(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    static const char* const names[] = { "drawLineStrip(points, color, thick?)", "drawTriangleFan(points, color)" };
    const Vector2* p;
    int n;
    float thick = 0;
    Color c;
    (void)this_val;
    if (rl_args(ctx, argc, argv, 2, names[magic]) || rl_color(ctx, argv[1], "a draw colour", &c))
        return JS_EXCEPTION;
    if (!magic && argc > 2 && !JS_IsUndefined(argv[2]) && rl_num_in(ctx, argv[2], "line thickness", 0, 4096, &thick))
        return JS_EXCEPTION;
    if (!rl_ready(ctx, names[magic]))
        return JS_EXCEPTION;
    if (rl_points(ctx, argv[0], names[magic], magic ? 3 : 2, &p, &n))
        return JS_EXCEPTION;
    if (magic)
        DrawTriangleFan(p, n, c);
    else if (thick > 0)
        DrawSplineLinear(p, n, thick, c);
    else
        DrawLineStrip(p, n, c);
    return JS_UNDEFINED;
}

static JSValue rl_camera_xform(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    static const char* const names[] = { "screenToWorld2D(x, y, camera)", "worldToScreen2D(x, y, camera)",
        "worldToScreen(x, y, z, camera)" };
    float f[3];
    int n = magic == 2 ? 3 : 2;
    Camera2D c2;
    Camera3D c3;
    Vector2 v;
    (void)this_val;
    memset(&c2, 0, sizeof(c2));
    memset(&c3, 0, sizeof(c3));
    if (rl_args(ctx, argc, argv, n + 1, names[magic]) || rl_nums(ctx, argv, n, names[magic], f)
        || (magic == 2 ? rl_camera3d(ctx, argv[3], &c3) : rl_camera2d(ctx, argv[2], &c2)))
        return JS_EXCEPTION;
    switch (magic) {
    case 0:
        v = GetScreenToWorld2D((Vector2) { f[0], f[1] }, c2);
        break;
    case 1:
        v = GetWorldToScreen2D((Vector2) { f[0], f[1] }, c2);
        break;
    default:
        if (!rl_ready(ctx, names[2]))
            return JS_EXCEPTION;
        v = GetWorldToScreen((Vector3) { f[0], f[1], f[2] }, c3);
        break;
    }
    return rl_xy(ctx, (double)v.x, (double)v.y, "x", "y");
}

static JSValue rl_cursor(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    static const char* const names[] = { "showCursor()", "hideCursor()", "enableCursor()", "disableCursor()", "isCursorHidden()",
        "isCursorOnScreen()", "setMouseCursor(cursor)" };
    int cur = 0;
    (void)this_val;
    if (magic == 6 && (rl_args(ctx, argc, argv, 1, names[6]) || rl_name_index(ctx, argv[0], "setMouseCursor cursor", rl_cursor_names, (int)countof(rl_cursor_names), &cur)))
        return JS_EXCEPTION;
    if (!rl_ready(ctx, names[magic]))
        return JS_EXCEPTION;
    switch (magic) {
    case 0:
        ShowCursor();
        break;
    case 1:
        HideCursor();
        break;
    case 2:
        EnableCursor();
        break;
    case 3:
        DisableCursor();
        break;
    case 4:
        return JS_NewBool(ctx, IsCursorHidden());
    case 5:
        return JS_NewBool(ctx, IsCursorOnScreen());
    default:
        SetMouseCursor(cur);
        break;
    }
    return JS_UNDEFINED;
}

static JSValue rl_clipboard(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    const char* s = NULL;
    (void)this_val;
    if (magic && (rl_args(ctx, argc, argv, 1, "setClipboardText(text)") || rl_text_arg(ctx, argv[0], "setClipboardText(text)", &s)))
        return JS_EXCEPTION;
    if (!rl_ready(ctx, magic ? "setClipboardText()" : "getClipboardText()")) {
        if (s)
            JS_FreeCString(ctx, s);
        return JS_EXCEPTION;
    }
    if (magic) {
        SetClipboardText(s);
        JS_FreeCString(ctx, s);
        return JS_UNDEFINED;
    }
    s = GetClipboardText();
    return JS_NewString(ctx, s ? s : "");
}

static JSValue rl_monitors(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    JSValue arr;
    int i, n;
    (void)this_val;
    (void)argc;
    (void)argv;
    if (!rl_ready(ctx, "monitors()"))
        return JS_EXCEPTION;
    n = GetMonitorCount();
    arr = JS_NewArray(ctx);
    if (JS_IsException(arr))
        return arr;
    for (i = 0; i < n; i++) {
        const char* name = GetMonitorName(i);
        Vector2 p = GetMonitorPosition(i);
        JSValue o = JS_NewObject(ctx);
        int bad = JS_IsException(o);
        bad = bad || JS_DefinePropertyValueStr(ctx, o, "name", JS_NewString(ctx, name ? name : ""), JS_PROP_C_W_E) < 0
            || JS_DefinePropertyValueStr(ctx, o, "x", JS_NewFloat64(ctx, (double)p.x), JS_PROP_C_W_E) < 0
            || JS_DefinePropertyValueStr(ctx, o, "y", JS_NewFloat64(ctx, (double)p.y), JS_PROP_C_W_E) < 0
            || JS_DefinePropertyValueStr(ctx, o, "width", JS_NewInt32(ctx, GetMonitorWidth(i)), JS_PROP_C_W_E) < 0
            || JS_DefinePropertyValueStr(ctx, o, "height", JS_NewInt32(ctx, GetMonitorHeight(i)), JS_PROP_C_W_E) < 0
            || JS_DefinePropertyValueStr(ctx, o, "physicalWidth", JS_NewInt32(ctx, GetMonitorPhysicalWidth(i)), JS_PROP_C_W_E) < 0
            || JS_DefinePropertyValueStr(ctx, o, "physicalHeight", JS_NewInt32(ctx, GetMonitorPhysicalHeight(i)), JS_PROP_C_W_E) < 0
            || JS_DefinePropertyValueStr(ctx, o, "refreshRate", JS_NewInt32(ctx, GetMonitorRefreshRate(i)), JS_PROP_C_W_E) < 0;
        if (bad || JS_DefinePropertyValueUint32(ctx, arr, (uint32_t)i, o, JS_PROP_C_W_E) < 0) {
            if (bad && !JS_IsException(o))
                JS_FreeValue(ctx, o);
            JS_FreeValue(ctx, arr);
            return JS_EXCEPTION;
        }
    }
    return arr;
}

static JSValue rl_touch(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    int i = 0;
    Vector2 p;
    JSValue o;
    (void)this_val;
    if (magic && (rl_args(ctx, argc, argv, 1, "touchPoint(index)") || rl_int_in(ctx, argv[0], "touchPoint index", 0, 7, &i)))
        return JS_EXCEPTION;
    if (!rl_ready(ctx, magic ? "touchPoint()" : "touchCount()"))
        return JS_EXCEPTION;
    if (!magic)
        return JS_NewInt32(ctx, GetTouchPointCount());
    if (i >= GetTouchPointCount())
        return JS_NULL;
    p = GetTouchPosition(i);
    o = rl_xy(ctx, (double)p.x, (double)p.y, "x", "y");
    if (!JS_IsException(o) && JS_DefinePropertyValueStr(ctx, o, "id", JS_NewInt32(ctx, GetTouchPointId(i)), JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, o);
        return JS_EXCEPTION;
    }
    return o;
}

static JSValue rl_gesture(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    static const char* const names[] = { "tap", "doubletap", "hold", "drag", "swipe-right", "swipe-left", "swipe-up", "swipe-down",
        "pinch-in", "pinch-out" };
    JSValue o, d, p;
    Vector2 v;
    int g, i;
    (void)this_val;
    (void)argc;
    (void)argv;
    if (!rl_ready(ctx, magic ? "gestureDetails()" : "gesture()"))
        return JS_EXCEPTION;
    if (!magic) {
        g = GetGestureDetected();
        for (i = 0; i < (int)countof(names); i++)
            if (g == (1 << i))
                return JS_NewString(ctx, names[i]);
        return JS_NewString(ctx, "none");
    }
    o = JS_NewObject(ctx);
    if (JS_IsException(o))
        return o;
    v = GetGestureDragVector();
    d = rl_xy(ctx, (double)v.x, (double)v.y, "x", "y");
    v = GetGesturePinchVector();
    p = rl_xy(ctx, (double)v.x, (double)v.y, "x", "y");
    if (JS_DefinePropertyValueStr(ctx, o, "holdDuration", JS_NewFloat64(ctx, (double)GetGestureHoldDuration()), JS_PROP_C_W_E) < 0
        || JS_DefinePropertyValueStr(ctx, o, "drag", d, JS_PROP_C_W_E) < 0
        || JS_DefinePropertyValueStr(ctx, o, "dragAngle", JS_NewFloat64(ctx, (double)GetGestureDragAngle()), JS_PROP_C_W_E) < 0
        || JS_DefinePropertyValueStr(ctx, o, "pinch", p, JS_PROP_C_W_E) < 0
        || JS_DefinePropertyValueStr(ctx, o, "pinchAngle", JS_NewFloat64(ctx, (double)GetGesturePinchAngle()), JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, o);
        return JS_EXCEPTION;
    }
    return o;
}

static JSValue rl_img_draw(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    static const int need[] = { 1, 3, 5, 5, 5, 4, 4, 7 };
    static const char* const names[] = { "clear(color)", "drawPixel(x, y, color)", "drawLine(x1, y1, x2, y2, color, thick?)",
        "drawRectangle(x, y, width, height, color)", "drawRectangleLines(x, y, width, height, color, thick?)",
        "drawCircle(cx, cy, radius, color)", "drawCircleLines(cx, cy, radius, color)", "drawTriangle(x1, y1, x2, y2, x3, y3, color)" };
    const int lim = RL_MAX_DIM * 4;
    int v[6] = { 0, 0, 0, 0, 0, 0 }, n = need[magic] - 1, thick = 1, i;
    Color c;
    Image* im;
    if (rl_args(ctx, argc, argv, need[magic], names[magic]))
        return JS_EXCEPTION;
    for (i = 0; i < n; i++) {
        int is_size = ((magic == 3 || magic == 4) && i >= 2) || ((magic == 5 || magic == 6) && i == 2);
        if (rl_int_in(ctx, argv[i], is_size ? "a size" : "a coordinate", is_size ? 0 : -lim, lim, &v[i]))
            return JS_EXCEPTION;
    }
    if (rl_color(ctx, argv[n], "a draw colour", &c))
        return JS_EXCEPTION;
    if ((magic == 2 || magic == 4) && argc > n + 1 && !JS_IsUndefined(argv[n + 1])
        && rl_int_in(ctx, argv[n + 1], "line thickness", 1, 4096, &thick))
        return JS_EXCEPTION;
    im = rl_img_of(ctx, this_val);
    if (!im)
        return JS_EXCEPTION;
    switch (magic) {
    case 0:
        ImageClearBackground(im, c);
        break;
    case 1:
        ImageDrawPixel(im, v[0], v[1], c);
        break;
    case 2:
        if (thick > 1)
            ImageDrawLineEx(im, (Vector2) { (float)v[0], (float)v[1] }, (Vector2) { (float)v[2], (float)v[3] }, thick, c);
        else
            ImageDrawLine(im, v[0], v[1], v[2], v[3], c);
        break;
    case 3:
        ImageDrawRectangle(im, v[0], v[1], v[2], v[3], c);
        break;
    case 4:
        ImageDrawRectangleLinesEx(im, (Rectangle) { (float)v[0], (float)v[1], (float)v[2], (float)v[3] }, thick, c);
        break;
    case 5:
        ImageDrawCircle(im, v[0], v[1], v[2], c);
        break;
    case 6:
        ImageDrawCircleLines(im, v[0], v[1], v[2], c);
        break;
    default:
        ImageDrawTriangle(im, (Vector2) { (float)v[0], (float)v[1] }, (Vector2) { (float)v[2], (float)v[3] },
            (Vector2) { (float)v[4], (float)v[5] }, c);
        break;
    }
    return JS_UNDEFINED;
}

static JSValue rl_img_draw_image(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    const int lim = RL_MAX_DIM * 4;
    int x, y;
    Color tint;
    Image *dst, *src, tmp;
    if (rl_args(ctx, argc, argv, 3, "drawImage(source, x, y, tint?)") || rl_int_in(ctx, argv[1], "drawImage x", -lim, lim, &x)
        || rl_int_in(ctx, argv[2], "drawImage y", -lim, lim, &y) || rl_color_opt(ctx, argc, argv, 3, "drawImage tint", &tint))
        return JS_EXCEPTION;
    if (JS_GetClassID(argv[0]) != rl_img_cid)
        return JS_ThrowTypeError(ctx, "dyna:raylib: drawImage source must be an Image");
    dst = rl_img_of(ctx, this_val);
    if (!dst)
        return JS_EXCEPTION;
    src = rl_img_of(ctx, argv[0]);
    if (!src)
        return JS_EXCEPTION;
    if (src != dst) {
        ImageDrawImage(dst, *src, x, y, tint);
        return JS_UNDEFINED;
    }
    tmp = ImageCopy(*src);
    if (!tmp.data)
        return JS_ThrowOutOfMemory(ctx);
    ImageDrawImage(dst, tmp, x, y, tint);
    UnloadImage(tmp);
    return JS_UNDEFINED;
}

static JSValue rl_img_draw_text(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    const int lim = RL_MAX_DIM * 4;
    int x, y, size;
    const char* s;
    Color c;
    Image* im;
    if (rl_args(ctx, argc, argv, 5, "drawText(text, x, y, fontSize, color)"))
        return JS_EXCEPTION;
    if (!JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "dyna:raylib: Image#drawText needs a text string");
    if (rl_int_in(ctx, argv[1], "drawText x", -lim, lim, &x) || rl_int_in(ctx, argv[2], "drawText y", -lim, lim, &y)
        || rl_int_in(ctx, argv[3], "fontSize", 1, 4096, &size) || rl_color(ctx, argv[4], "drawText color", &c))
        return JS_EXCEPTION;
    if (!rl_ready(ctx, "Image#drawText() (the default font)"))
        return JS_EXCEPTION;
    im = rl_img_of(ctx, this_val);
    if (!im)
        return JS_EXCEPTION;
    if (rl_text_arg(ctx, argv[0], "Image#drawText", &s))
        return JS_EXCEPTION;
    ImageDrawText(im, s, x, y, size, c);
    JS_FreeCString(ctx, s);
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry rl_window_proto[] = {
    JS_CGETSET_MAGIC_DEF("shouldClose", rl_win_get, NULL, 0),
    JS_CGETSET_MAGIC_DEF("size", rl_win_get, NULL, 1),
    JS_CGETSET_MAGIC_DEF("renderSize", rl_win_get, NULL, 2),
    JS_CGETSET_MAGIC_DEF("scaleDPI", rl_win_get, NULL, 3),
    JS_CGETSET_MAGIC_DEF("focused", rl_win_get, NULL, 4),
    JS_CGETSET_MAGIC_DEF("minimized", rl_win_get, NULL, 5),
    JS_CGETSET_MAGIC_DEF("fps", rl_win_get, NULL, 6),
    JS_CGETSET_MAGIC_DEF("frameTime", rl_win_get, NULL, 7),
    JS_CGETSET_MAGIC_DEF("time", rl_win_get, NULL, 8),
    JS_CFUNC_DEF("setTitle", 1, rl_win_set_title),
    JS_CFUNC_MAGIC_DEF("setSize", 2, rl_win_set2, 0),
    JS_CFUNC_MAGIC_DEF("setPosition", 2, rl_win_set2, 1),
    JS_CFUNC_MAGIC_DEF("setTargetFPS", 1, rl_win_set1, 0),
    JS_CFUNC_MAGIC_DEF("setExitKey", 1, rl_win_set1, 1),
    JS_CFUNC_DEF("frame", 1, rl_win_frame),
    JS_CFUNC_MAGIC_DEF("mode2D", 2, rl_win_mode, 0),
    JS_CFUNC_MAGIC_DEF("mode3D", 2, rl_win_mode, 1),
    JS_CFUNC_MAGIC_DEF("blendMode", 2, rl_win_scope, 0),
    JS_CFUNC_MAGIC_DEF("scissor", 5, rl_win_scope, 1),
    JS_CGETSET_MAGIC_DEF("fullscreen", rl_win_get2, NULL, 0),
    JS_CGETSET_MAGIC_DEF("maximized", rl_win_get2, NULL, 1),
    JS_CGETSET_MAGIC_DEF("resized", rl_win_get2, NULL, 2),
    JS_CGETSET_MAGIC_DEF("position", rl_win_get2, NULL, 3),
    JS_CGETSET_MAGIC_DEF("hidden", rl_win_get2, NULL, 4),
    JS_CGETSET_MAGIC_DEF("monitor", rl_win_get2, NULL, 5),
    JS_CFUNC_MAGIC_DEF("toggleFullscreen", 0, rl_win_act, 0),
    JS_CFUNC_MAGIC_DEF("toggleBorderless", 0, rl_win_act, 1),
    JS_CFUNC_MAGIC_DEF("maximize", 0, rl_win_act, 2),
    JS_CFUNC_MAGIC_DEF("minimize", 0, rl_win_act, 3),
    JS_CFUNC_MAGIC_DEF("restore", 0, rl_win_act, 4),
    JS_CFUNC_MAGIC_DEF("focus", 0, rl_win_act, 5),
    JS_CFUNC_MAGIC_DEF("setResizable", 1, rl_win_flag, 0),
    JS_CFUNC_MAGIC_DEF("setTopmost", 1, rl_win_flag, 1),
    JS_CFUNC_MAGIC_DEF("setDecorated", 1, rl_win_flag, 2),
    JS_CFUNC_MAGIC_DEF("setAlwaysRun", 1, rl_win_flag, 3),
    JS_CFUNC_MAGIC_DEF("setMousePassthrough", 1, rl_win_flag, 4),
    JS_CFUNC_DEF("setOpacity", 1, rl_win_set_opacity),
    JS_CFUNC_MAGIC_DEF("setMinSize", 2, rl_win_limit, 0),
    JS_CFUNC_MAGIC_DEF("setMaxSize", 2, rl_win_limit, 1),
};

static const JSCFunctionListEntry rl_texture_proto[] = {
    JS_CGETSET_MAGIC_DEF("width", rl_gpu_size, NULL, 0),
    JS_CGETSET_MAGIC_DEF("height", rl_gpu_size, NULL, 1),
    JS_CFUNC_DEF("update", 2, rl_tex_update),
    JS_CFUNC_DEF("setFilter", 1, rl_gpu_filter),
};

static const JSCFunctionListEntry rl_rt_proto[] = {
    JS_CGETSET_MAGIC_DEF("width", rl_gpu_size, NULL, 0),
    JS_CGETSET_MAGIC_DEF("height", rl_gpu_size, NULL, 1),
    JS_CFUNC_DEF("draw", 1, rl_rt_draw),
    JS_CFUNC_DEF("readPixels", 0, rl_rt_read),
    JS_CFUNC_DEF("setFilter", 1, rl_gpu_filter),
};

static const JSCFunctionListEntry rl_image_proto[] = {
    JS_CGETSET_MAGIC_DEF("width", rl_img_get, NULL, 0),
    JS_CGETSET_MAGIC_DEF("height", rl_img_get, NULL, 1),
    JS_CFUNC_DEF("pixels", 0, rl_img_pixels),
    JS_CFUNC_DEF("export", 1, rl_img_export),
    JS_CFUNC_MAGIC_DEF("resize", 2, rl_img_op, 0),
    JS_CFUNC_MAGIC_DEF("crop", 4, rl_img_op, 1),
    JS_CFUNC_MAGIC_DEF("flipVertical", 0, rl_img_op, 2),
    JS_CFUNC_MAGIC_DEF("flipHorizontal", 0, rl_img_op, 3),
    JS_CFUNC_DEF("copy", 0, rl_img_copy),
    JS_CFUNC_DEF("toTexture", 0, rl_img_to_texture),
    JS_CFUNC_MAGIC_DEF("clear", 1, rl_img_draw, 0),
    JS_CFUNC_MAGIC_DEF("drawPixel", 3, rl_img_draw, 1),
    JS_CFUNC_MAGIC_DEF("drawLine", 5, rl_img_draw, 2),
    JS_CFUNC_MAGIC_DEF("drawRectangle", 5, rl_img_draw, 3),
    JS_CFUNC_MAGIC_DEF("drawRectangleLines", 5, rl_img_draw, 4),
    JS_CFUNC_MAGIC_DEF("drawCircle", 4, rl_img_draw, 5),
    JS_CFUNC_MAGIC_DEF("drawCircleLines", 4, rl_img_draw, 6),
    JS_CFUNC_MAGIC_DEF("drawTriangle", 7, rl_img_draw, 7),
    JS_CFUNC_DEF("drawImage", 3, rl_img_draw_image),
    JS_CFUNC_DEF("drawText", 5, rl_img_draw_text),
};

static const JSCFunctionListEntry rl_font_proto[] = {
    JS_CGETSET_MAGIC_DEF("baseSize", rl_font_get, NULL, 0),
    JS_CGETSET_MAGIC_DEF("glyphCount", rl_font_get, NULL, 1),
};

static const JSCFunctionListEntry rl_sound_proto[] = {
    JS_CGETSET_MAGIC_DEF("playing", rl_snd_get, NULL, 0),
    JS_CFUNC_MAGIC_DEF("play", 0, rl_play, 0),
    JS_CFUNC_MAGIC_DEF("stop", 0, rl_play, 1),
    JS_CFUNC_MAGIC_DEF("pause", 0, rl_play, 2),
    JS_CFUNC_MAGIC_DEF("resume", 0, rl_play, 3),
    JS_CFUNC_MAGIC_DEF("setVolume", 1, rl_snd_set, 0),
    JS_CFUNC_MAGIC_DEF("setPitch", 1, rl_snd_set, 1),
    JS_CFUNC_MAGIC_DEF("setPan", 1, rl_snd_set, 2),
};

static const JSCFunctionListEntry rl_music_proto[] = {
    JS_CGETSET_MAGIC_DEF("playing", rl_snd_get, NULL, 0),
    JS_CGETSET_MAGIC_DEF("length", rl_snd_get, NULL, 1),
    JS_CGETSET_MAGIC_DEF("played", rl_snd_get, NULL, 2),
    JS_CFUNC_MAGIC_DEF("play", 0, rl_play, 0),
    JS_CFUNC_MAGIC_DEF("stop", 0, rl_play, 1),
    JS_CFUNC_MAGIC_DEF("pause", 0, rl_play, 2),
    JS_CFUNC_MAGIC_DEF("resume", 0, rl_play, 3),
    JS_CFUNC_DEF("update", 0, rl_music_update),
    JS_CFUNC_MAGIC_DEF("setVolume", 1, rl_snd_set, 0),
    JS_CFUNC_MAGIC_DEF("setPitch", 1, rl_snd_set, 1),
    JS_CFUNC_MAGIC_DEF("setPan", 1, rl_snd_set, 2),
    JS_CFUNC_MAGIC_DEF("seek", 1, rl_snd_set, 3),
    JS_CFUNC_MAGIC_DEF("setLooping", 1, rl_snd_set, 4),
};

static const JSCFunctionListEntry rl_stream_proto[] = {
    JS_CGETSET_MAGIC_DEF("playing", rl_snd_get, NULL, 0),
    JS_CGETSET_MAGIC_DEF("processed", rl_snd_get, NULL, 3),
    JS_CFUNC_MAGIC_DEF("play", 0, rl_play, 0),
    JS_CFUNC_MAGIC_DEF("stop", 0, rl_play, 1),
    JS_CFUNC_MAGIC_DEF("pause", 0, rl_play, 2),
    JS_CFUNC_MAGIC_DEF("resume", 0, rl_play, 3),
    JS_CFUNC_DEF("update", 1, rl_stream_update),
    JS_CFUNC_MAGIC_DEF("setVolume", 1, rl_snd_set, 0),
    JS_CFUNC_MAGIC_DEF("setPitch", 1, rl_snd_set, 1),
    JS_CFUNC_MAGIC_DEF("setPan", 1, rl_snd_set, 2),
};

static const JSCFunctionListEntry rl_funcs[] = {
    JS_CFUNC_DEF("version", 0, rl_version),
    JS_CFUNC_DEF("setLogLevel", 1, rl_set_log_level),
    JS_CFUNC_DEF("rgba", 4, rl_rgba),
    JS_CFUNC_MAGIC_DEF("initAudio", 0, rl_audio_fn, 0),
    JS_CFUNC_MAGIC_DEF("closeAudio", 0, rl_audio_fn, 1),
    JS_CFUNC_MAGIC_DEF("audioReady", 0, rl_audio_fn, 2),
    JS_CFUNC_MAGIC_DEF("setMasterVolume", 1, rl_audio_fn, 3),
    JS_CFUNC_DEF("clearBackground", 1, rl_clear),
    JS_CFUNC_MAGIC_DEF("drawPixel", 3, rl_draw2d, 0),
    JS_CFUNC_MAGIC_DEF("drawLine", 6, rl_draw2d, 1),
    JS_CFUNC_MAGIC_DEF("drawCircle", 4, rl_draw2d, 2),
    JS_CFUNC_MAGIC_DEF("drawCircleLines", 4, rl_draw2d, 3),
    JS_CFUNC_MAGIC_DEF("drawEllipse", 5, rl_draw2d, 4),
    JS_CFUNC_MAGIC_DEF("drawRectangle", 5, rl_draw2d, 5),
    JS_CFUNC_MAGIC_DEF("drawRectangleLines", 6, rl_draw2d, 6),
    JS_CFUNC_MAGIC_DEF("drawRectangleRounded", 7, rl_draw2d, 7),
    JS_CFUNC_MAGIC_DEF("drawTriangle", 7, rl_draw2d, 8),
    JS_CFUNC_MAGIC_DEF("drawPoly", 6, rl_draw2d, 9),
    JS_CFUNC_MAGIC_DEF("drawCube", 7, rl_draw3d, 0),
    JS_CFUNC_MAGIC_DEF("drawCubeWires", 7, rl_draw3d, 1),
    JS_CFUNC_MAGIC_DEF("drawSphere", 5, rl_draw3d, 2),
    JS_CFUNC_MAGIC_DEF("drawPlane", 6, rl_draw3d, 3),
    JS_CFUNC_MAGIC_DEF("drawLine3D", 7, rl_draw3d, 4),
    JS_CFUNC_DEF("drawGrid", 2, rl_draw_grid),
    JS_CFUNC_DEF("drawFPS", 2, rl_draw_fps),
    JS_CFUNC_MAGIC_DEF("drawText", 5, rl_text, 0),
    JS_CFUNC_MAGIC_DEF("measureText", 2, rl_text, 1),
    JS_CFUNC_MAGIC_DEF("drawTextEx", 7, rl_text, 2),
    JS_CFUNC_MAGIC_DEF("measureTextEx", 4, rl_text, 3),
    JS_CFUNC_DEF("drawTexture", 4, rl_draw_texture),
    JS_CFUNC_DEF("drawTexturePro", 7, rl_draw_texture_pro),
    JS_CFUNC_DEF("loadTexture", 1, rl_load_texture),
    JS_CFUNC_DEF("loadImage", 1, rl_load_image),
    JS_CFUNC_DEF("loadFont", 2, rl_load_font),
    JS_CFUNC_MAGIC_DEF("loadSound", 1, rl_load_audio, 0),
    JS_CFUNC_MAGIC_DEF("loadMusic", 1, rl_load_audio, 1),
    JS_CFUNC_DEF("loadWave", 1, rl_load_wave),
    JS_CFUNC_MAGIC_DEF("isKeyDown", 1, rl_key, 0),
    JS_CFUNC_MAGIC_DEF("isKeyPressed", 1, rl_key, 1),
    JS_CFUNC_MAGIC_DEF("isKeyReleased", 1, rl_key, 2),
    JS_CFUNC_MAGIC_DEF("getKeyPressed", 0, rl_input0, 0),
    JS_CFUNC_MAGIC_DEF("getCharPressed", 0, rl_input0, 1),
    JS_CFUNC_MAGIC_DEF("mousePosition", 0, rl_input0, 2),
    JS_CFUNC_MAGIC_DEF("mouseDelta", 0, rl_input0, 3),
    JS_CFUNC_MAGIC_DEF("mouseWheel", 0, rl_input0, 4),
    JS_CFUNC_MAGIC_DEF("isMouseButtonDown", 1, rl_mouse_button, 0),
    JS_CFUNC_MAGIC_DEF("isMouseButtonPressed", 1, rl_mouse_button, 1),
    JS_CFUNC_MAGIC_DEF("isMouseButtonReleased", 1, rl_mouse_button, 2),
    JS_CFUNC_MAGIC_DEF("isGamepadAvailable", 1, rl_gamepad, 0),
    JS_CFUNC_MAGIC_DEF("gamepadName", 1, rl_gamepad, 1),
    JS_CFUNC_MAGIC_DEF("gamepadAxis", 2, rl_gamepad, 2),
    JS_CFUNC_MAGIC_DEF("isGamepadButtonDown", 2, rl_gamepad, 3),
    JS_CFUNC_MAGIC_DEF("isGamepadButtonPressed", 2, rl_gamepad, 4),
    JS_CFUNC_MAGIC_DEF("isGamepadButtonReleased", 2, rl_gamepad, 5),
    JS_CFUNC_MAGIC_DEF("gamepadAxisCount", 1, rl_gamepad, 6),
    JS_CFUNC_MAGIC_DEF("setGamepadVibration", 4, rl_gamepad, 7),
    JS_CFUNC_MAGIC_DEF("getGamepadButtonPressed", 0, rl_input0, 5),
    JS_CFUNC_MAGIC_DEF("mouseWheelV", 0, rl_input0, 6),
    JS_CFUNC_MAGIC_DEF("drawRing", 8, rl_draw_shape, 0),
    JS_CFUNC_MAGIC_DEF("drawCircleSector", 7, rl_draw_shape, 1),
    JS_CFUNC_MAGIC_DEF("drawRectangleGradient", 8, rl_draw_shape, 2),
    JS_CFUNC_MAGIC_DEF("drawLineStrip", 2, rl_draw_points, 0),
    JS_CFUNC_MAGIC_DEF("drawTriangleFan", 2, rl_draw_points, 1),
    JS_CFUNC_MAGIC_DEF("screenToWorld2D", 3, rl_camera_xform, 0),
    JS_CFUNC_MAGIC_DEF("worldToScreen2D", 3, rl_camera_xform, 1),
    JS_CFUNC_MAGIC_DEF("worldToScreen", 4, rl_camera_xform, 2),
    JS_CFUNC_MAGIC_DEF("showCursor", 0, rl_cursor, 0),
    JS_CFUNC_MAGIC_DEF("hideCursor", 0, rl_cursor, 1),
    JS_CFUNC_MAGIC_DEF("enableCursor", 0, rl_cursor, 2),
    JS_CFUNC_MAGIC_DEF("disableCursor", 0, rl_cursor, 3),
    JS_CFUNC_MAGIC_DEF("isCursorHidden", 0, rl_cursor, 4),
    JS_CFUNC_MAGIC_DEF("isCursorOnScreen", 0, rl_cursor, 5),
    JS_CFUNC_MAGIC_DEF("setMouseCursor", 1, rl_cursor, 6),
    JS_CFUNC_MAGIC_DEF("getClipboardText", 0, rl_clipboard, 0),
    JS_CFUNC_MAGIC_DEF("setClipboardText", 1, rl_clipboard, 1),
    JS_CFUNC_DEF("monitors", 0, rl_monitors),
    JS_CFUNC_MAGIC_DEF("touchCount", 0, rl_touch, 0),
    JS_CFUNC_MAGIC_DEF("touchPoint", 1, rl_touch, 1),
    JS_CFUNC_MAGIC_DEF("gesture", 0, rl_gesture, 0),
    JS_CFUNC_MAGIC_DEF("gestureDetails", 0, rl_gesture, 1),
};

static JSClassID rl_capture_cid(JSContext* ctx, JSTypedArrayEnum type)
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

static int rl_init_module(JSContext* ctx, JSModuleDef* m)
{
    static const JSClassDef win_def = { "Window", .finalizer = dyn_res_finalizer };
    static const JSClassDef tex_def = { "Texture", .finalizer = dyn_res_finalizer, .gc_mark = rl_gpu_mark };
    static const JSClassDef rt_def = { "RenderTexture", .finalizer = dyn_res_finalizer, .gc_mark = rl_gpu_mark };
    static const JSClassDef img_def = { "Image", .finalizer = dyn_res_finalizer };
    static const JSClassDef font_def = { "Font", .finalizer = dyn_res_finalizer, .gc_mark = rl_gpu_mark };
    static const JSClassDef sound_def = { "Sound", .finalizer = dyn_res_finalizer };
    static const JSClassDef music_def = { "Music", .finalizer = dyn_res_finalizer };
    static const JSClassDef stream_def = { "AudioStream", .finalizer = dyn_res_finalizer };
    static const struct {
        const char* name;
        const rl_named* t;
        int n;
    } tables[] = {
        { "colors", rl_color_table, (int)countof(rl_color_table) },
        { "keys", rl_key_table, (int)countof(rl_key_table) },
        { "mouseButtons", rl_mouse_button_table, (int)countof(rl_mouse_button_table) },
        { "gamepadButtons", rl_gamepad_button_table, (int)countof(rl_gamepad_button_table) },
        { "gamepadAxes", rl_gamepad_axis_table, (int)countof(rl_gamepad_axis_table) },
    };
    size_t i;
    if (!rl.open)
        rl.win_obj = JS_UNDEFINED;
    rl_f32_cid = rl_capture_cid(ctx, JS_TYPED_ARRAY_FLOAT32);
    SetTraceLogLevel(LOG_WARNING);
    if (dyn_register_class(ctx, m, &rl_win_cid, &win_def, rl_window_proto, countof(rl_window_proto), rl_window_ctor, "Window")
        || dyn_register_class(ctx, m, &rl_tex_cid, &tex_def, rl_texture_proto, countof(rl_texture_proto), rl_texture_ctor, "Texture")
        || dyn_register_class(ctx, m, &rl_rt_cid, &rt_def, rl_rt_proto, countof(rl_rt_proto), rl_rt_ctor, "RenderTexture")
        || dyn_register_class(ctx, m, &rl_img_cid, &img_def, rl_image_proto, countof(rl_image_proto), rl_image_ctor, "Image")
        || dyn_register_class(ctx, m, &rl_font_cid, &font_def, rl_font_proto, countof(rl_font_proto), rl_font_ctor, "Font")
        || dyn_register_class(ctx, m, &rl_sound_cid, &sound_def, rl_sound_proto, countof(rl_sound_proto), rl_sound_ctor, "Sound")
        || dyn_register_class(ctx, m, &rl_music_cid, &music_def, rl_music_proto, countof(rl_music_proto), rl_sound_ctor, "Music")
        || dyn_register_class(ctx, m, &rl_stream_cid, &stream_def, rl_stream_proto, countof(rl_stream_proto), rl_stream_ctor, "AudioStream"))
        return -1;
    for (i = 0; i < countof(tables); i++) {
        JSValue o = rl_table_object(ctx, tables[i].t, tables[i].n);
        if (JS_IsException(o) || JS_SetModuleExport(ctx, m, tables[i].name, o) < 0)
            return -1;
    }
    return JS_SetModuleExportList(ctx, m, rl_funcs, countof(rl_funcs));
}

int js_nat_init_raylib(JSContext* ctx)
{
    static const char* const names[] = { "Window", "Texture", "RenderTexture", "Image", "Font", "Sound", "Music", "AudioStream",
        "colors", "keys", "mouseButtons", "gamepadButtons", "gamepadAxes" };
    JSModuleDef* m = JS_NewCModule(ctx, "dyna:raylib", rl_init_module);
    size_t i;
    if (!m)
        return -1;
    for (i = 0; i < countof(names); i++)
        if (JS_AddModuleExport(ctx, m, names[i]) < 0)
            return -1;
    return JS_AddModuleExportList(ctx, m, rl_funcs, countof(rl_funcs));
}

int dyn_raylib_blend_mode(void)
{
    return rl.in_blend ? rl.blend_mode : BLEND_ALPHA;
}

int dyn_raylib_texture_height(void)
{
    return rl.in_texture && rl.active_rt ? rl.active_rt->rt.texture.height : 0;
}

int dyn_raylib_target(JSContext* ctx, JSValueConst v, int* in_frame)
{
    DynResource* res = JS_GetOpaque(v, rl_win_cid);
    if (JS_GetClassID(v) != rl_win_cid)
        return 0;
    if (!rl.open || rl.closing || !res || res->closed) {
        JS_ThrowTypeError(ctx, "dyna:raylib: the Window is closed");
        return -1;
    }
    *in_frame = rl.in_frame || rl.in_texture;
    return 1;
}

int dyn_raylib_window_alive(JSValueConst v)
{
    DynResource* res = JS_GetOpaque(v, rl_win_cid);
    return res && !res->closed && rl.open && !rl.closing;
}

#endif
