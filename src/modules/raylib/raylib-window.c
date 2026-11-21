#include "raylib-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_RAYLIB)

static int rl_window_token;

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

static void rl_win_dispose(void* p)
{
    (void)p;
    rl.win_obj = JS_UNDEFINED;
    if (rl.depth)
        rl.closing = 1;
    else
        rl_close_now();
}

static const char* const rl_window_keys[] = { "hidden", "resizable", "highDensity", "vsync", "msaa", "undecorated", "topmost", "transparent", "fps" };

JSValue rl_window_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv)
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

JSValue rl_win_get(JSContext* ctx, JSValueConst this_val, int magic)
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

JSValue rl_win_set_title(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
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

JSValue rl_win_set2(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
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

JSValue rl_win_set1(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
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

JSValue rl_win_frame(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
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

JSValue rl_win_mode(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
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

static const char* const rl_blend_names[] = { "alpha", "additive", "multiplied", "add-colors", "subtract-colors", "alpha-premultiply" };

JSValue rl_win_get2(JSContext* ctx, JSValueConst this_val, int magic)
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

JSValue rl_win_act(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
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

JSValue rl_win_flag(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
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

JSValue rl_win_set_opacity(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    float o;
    if (rl_args(ctx, argc, argv, 1, "setOpacity(opacity)") || rl_num_in(ctx, argv[0], "setOpacity opacity", 0, 1, &o))
        return JS_EXCEPTION;
    if (!rl_win_live(ctx, this_val))
        return JS_EXCEPTION;
    SetWindowOpacity(o);
    return JS_UNDEFINED;
}

JSValue rl_win_limit(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
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

JSValue rl_win_scope(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
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

int dyn_raylib_window_alive(JSValueConst v)
{
    DynResource* res = JS_GetOpaque(v, rl_win_cid);
    return res && !res->closed && rl.open && !rl.closing;
}

#endif
