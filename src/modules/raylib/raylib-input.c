#include "raylib-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_RAYLIB)

JSValue rl_input0(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
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

JSValue rl_gamepad(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
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

#endif
