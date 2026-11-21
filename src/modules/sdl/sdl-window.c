#include "sdl-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_SDL3)

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

JSValue sdl_window_state(JSContext* ctx, JSValueConst this_val)
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

JSValue sdl_window_act2(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
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

JSValue sdl_window_flag(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
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

static const char* const sdl_window_keys[] = { "resizable", "hidden", "borderless", "highDensity", "fullscreen", "alwaysOnTop" };

JSValue sdl_window_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv)
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

JSValue sdl_window_get(JSContext* ctx, JSValueConst this_val, int magic)
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

JSValue sdl_window_set_title(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
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

JSValue sdl_window_set2(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
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

JSValue sdl_window_act(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
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

JSValue sdl_window_set_opacity(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
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

JSValue sdl_window_text(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
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

JSValue sdl_window_text_area(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
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

#endif
