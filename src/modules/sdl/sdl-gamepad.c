#include "sdl-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_SDL3)

typedef struct {
    SDL_Gamepad* g;
    SDL_JoystickID id;
} sdl_pad;

static void sdl_pad_dispose(void* p)
{
    sdl_pad* g = p;
    if (g->g) {
        SDL_CloseGamepad(g->g);
        atomic_fetch_sub(&sdl_live_pads, 1);
    }
    dyn_nat_free(g);
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

JSValue sdl_virtual_pad(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
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

JSValue sdl_pad_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv)
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

JSValue sdl_pad_get(JSContext* ctx, JSValueConst this_val, int magic)
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

JSValue sdl_pad_read(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
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

JSValue sdl_pad_rumble(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
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

JSValue sdl_pad_set_virtual(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
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

#endif
