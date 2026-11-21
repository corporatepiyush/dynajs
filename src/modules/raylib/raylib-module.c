#include "raylib-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_RAYLIB)

#define RL_MAX_SAMPLES (1 << 28)

typedef struct {
    const char* name;
    unsigned int value;
} rl_named;

dyn_rl_state_t rl;

JSClassID rl_win_cid, rl_tex_cid, rl_rt_cid, rl_img_cid, rl_font_cid, rl_sound_cid, rl_music_cid, rl_stream_cid;

JSClassID rl_f32_cid;

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

int rl_on_main(JSContext* ctx, const char* what)
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

int rl_color(JSContext* ctx, JSValueConst v, const char* what, Color* c)
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

int rl_field(JSContext* ctx, JSValueConst obj, const char* key, const char* what, int need, float dflt, float* out)
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

int rl_path(JSContext* ctx, JSValueConst v, const char* who, const char** out)
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

void rl_close_now(void)
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

int rl_camera2d(JSContext* ctx, JSValueConst v, Camera2D* c)
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

int rl_camera3d(JSContext* ctx, JSValueConst v, Camera3D* c)
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

static JSValue rl_texture_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv)
{
    (void)new_target;
    (void)argc;
    (void)argv;
    return JS_ThrowTypeError(ctx, "dyna:raylib: a Texture comes from loadTexture(path) or Image#toTexture()");
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

static const char* const rl_cursor_names[] = { "default", "arrow", "ibeam", "crosshair", "pointer", "resize-ew", "resize-ns",
    "resize-nwse", "resize-nesw", "resize-all", "not-allowed" };

int rl_name_index(JSContext* ctx, JSValueConst v, const char* what, const char* const* names, int n, int* out)
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

#endif
