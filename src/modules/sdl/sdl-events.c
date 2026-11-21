#include "sdl-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_SDL3)

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

JSValue sdl_event_obj(JSContext* ctx, const SDL_Event* e)
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

int sdl_events_ready(JSContext* ctx, const char* what)
{
    if (!sdl_on_main(ctx, what))
        return 0;
    if (!(SDL_WasInit(SDL_INIT_EVENTS) & SDL_INIT_EVENTS)) {
        JS_ThrowTypeError(ctx, "dyna:sdl: %s needs init() first (video or events)", what);
        return 0;
    }
    return 1;
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

JSValue sdl_push_event(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
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

#endif
