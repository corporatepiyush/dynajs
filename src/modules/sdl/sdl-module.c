#include "sdl-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_SDL3)

JSClassID sdl_win_cid, sdl_ren_cid, sdl_tex_cid, sdl_audio_cid, sdl_pad_cid;

JSClassID sdl_f32_cid, sdl_i16_cid, sdl_i32_cid;

_Atomic int sdl_live_windows, sdl_live_streams;

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

int sdl_on_main(JSContext* ctx, const char* what)
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

int sdl_opt_bool(JSContext* ctx, JSValueConst opts, const char* key, int* out)
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

int sdl_check_opts(JSContext* ctx, JSValueConst opts, const char* who,
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

static SDL_InitFlags sdl_flag_named(const char* s)
{
    size_t i;
    for (i = 0; i < countof(sdl_subsystems); i++)
        if (!strcmp(s, sdl_subsystems[i].name))
            return sdl_subsystems[i].flag;
    return 0;
}

static const char* const sdl_cursor_names[] = { "default", "text", "wait", "crosshair", "progress", "nwse-resize", "nesw-resize",
    "ew-resize", "ns-resize", "move", "not-allowed", "pointer", "nw-resize", "n-resize", "ne-resize", "e-resize", "se-resize",
    "s-resize", "sw-resize", "w-resize" };

static SDL_Cursor* sdl_cursors[countof(sdl_cursor_names)];

int sdl_name_index(JSContext* ctx, JSValueConst v, const char* what, const char* const* names, int n, int* out)
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

int sdl_text_arg(JSContext* ctx, JSValueConst v, const char* who, const char** out)
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

_Atomic int sdl_live_pads;

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

int sdl_rect_arg(JSContext* ctx, JSValueConst v, const char* what, float f[4], int* present)
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

int sdl_blend_arg(JSContext* ctx, int argc, JSValueConst* argv, const char* who, SDL_BlendMode* bm)
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

JSValue sdl_samples_array(JSContext* ctx, const uint8_t* bytes, size_t len, int is_float)
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

#endif
