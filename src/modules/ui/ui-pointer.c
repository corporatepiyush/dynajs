#include "ui-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_UI)

static int uib_pointer_field(JSContext* ctx, JSValueConst o, const char* k, double lo, double hi, double* out)
{
    return uib_opt_num(ctx, o, k, k, lo, hi, out);
}

JSValue uib_pointer(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    static const char* const keys[] = { "x", "y", "buttons", "wheelX", "wheelY", "time", "kind" };
    JSValueConst o = argc > 0 ? argv[0] : JS_UNDEFINED;
    dyn_ui_pointer p;
    double x, y, b = 0, wx = 0, wy = 0, t = 0;
    int kind;
    uib* u;
    if (!JS_IsObject(o))
        return JS_ThrowTypeError(ctx, "dyna:ui: pointer({ x, y, buttons?, wheelX?, wheelY?, time? })");
    if (uib_check_opts(ctx, o, "pointer", keys, (int)countof(keys)))
        return JS_EXCEPTION;
    u = uib_get(ctx, this_val);
    if (!u)
        return JS_EXCEPTION;
    x = (double)dyn_ui_pointer_state(u->ui)->x / (double)u->scale;
    y = (double)dyn_ui_pointer_state(u->ui)->y / (double)u->scale;
    kind = dyn_ui_pointer_state(u->ui)->kind;
    b = dyn_ui_pointer_state(u->ui)->buttons;
    if (uib_pointer_field(ctx, o, "x", -1e6, 1e6, &x) || uib_pointer_field(ctx, o, "y", -1e6, 1e6, &y) || uib_pointer_field(ctx, o, "buttons", 0, 7, &b)
        || uib_pointer_field(ctx, o, "wheelX", -1000, 1000, &wx) || uib_pointer_field(ctx, o, "wheelY", -1000, 1000, &wy)
        || uib_pointer_field(ctx, o, "time", 0, 1e12, &t))
        return JS_EXCEPTION;
    if (b != floor(b))
        return JS_ThrowRangeError(ctx, "dyna:ui: pointer buttons is a bit mask: 1 left, 2 right, 4 middle");
    {
        const char* ks = NULL;
        size_t kn = 0;
        if (uib_opt_str(ctx, o, "kind", "pointer kind", &ks, &kn))
            return JS_EXCEPTION;
        if (ks) {
            int ok = 1;
            if (!strcmp(ks, "mouse"))
                kind = DYN_UI_POINTER_MOUSE;
            else if (!strcmp(ks, "touch"))
                kind = DYN_UI_POINTER_TOUCH;
            else if (!strcmp(ks, "pen"))
                kind = DYN_UI_POINTER_PEN;
            else
                ok = 0;
            JS_FreeCString(ctx, ks);
            if (!ok)
                return JS_ThrowTypeError(ctx, "dyna:ui: pointer kind is \"mouse\", \"touch\" or \"pen\"");
        }
    }
    u = uib_get(ctx, this_val);
    if (!u)
        return JS_EXCEPTION;
    memset(&p, 0, sizeof(p));
    p.x = uib_px(u, x);
    p.y = uib_px(u, y);
    p.kind = (uint8_t)kind;
    p.buttons = (uint32_t)b;
    p.wheel_x = (float)wx;
    p.wheel_y = (float)wy;
    p.time = t > 0 ? t : dyn_ui_time(u->ui) > 0 ? dyn_ui_time(u->ui)
                                                : uib_now();
    dyn_ui_input_pointer(u->ui, &p);
    return JS_UNDEFINED;
}

static int uib_payload_ok(JSContext* ctx, JSValueConst v)
{
    double d;
    if (JS_IsString(v))
        return 1;
    return JS_IsNumber(v) && !JS_ToFloat64(ctx, &d, v) && isfinite(d);
}

JSValue uib_drag_source(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    static const char* const keys[] = { "label" };
    JSValueConst opts = argc > 1 ? argv[1] : JS_UNDEFINED;
    uib* u;
    const char* lab = NULL;
    size_t ln = 0;
    int r;
    if (argc < 1 || !uib_payload_ok(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "dyna:ui: dragSource(payload) takes a string or a finite number");
    if (JS_IsString(argv[0])) {
        size_t pl = 0;
        const char* ps = JS_ToCStringLen(ctx, &pl, argv[0]);
        if (!ps)
            return JS_EXCEPTION;
        JS_FreeCString(ctx, ps);
        if (!pl)
            return JS_ThrowTypeError(ctx, "dyna:ui: dragSource payload must not be the empty string");
    }
    if (uib_check_opts(ctx, opts, "dragSource", keys, 1) || uib_opt_str(ctx, opts, "label", "dragSource label", &lab, &ln))
        return JS_EXCEPTION;
    u = uib_in_frame(ctx, this_val, "dragSource");
    if (u && !dyn_ui_has_last(u->ui)) {
        JS_ThrowTypeError(ctx, "dyna:ui: dragSource() marks the widget drawn before it, and there is none in this frame");
        u = NULL;
    }
    if (!u) {
        if (lab)
            JS_FreeCString(ctx, lab);
        return JS_EXCEPTION;
    }
    r = dyn_ui_w_drag_source(u->ui, lab ? lab : "", ln);
    if (lab)
        JS_FreeCString(ctx, lab);
    if (r) {
        JS_FreeValue(ctx, u->dnd_payload);
        u->dnd_payload = JS_DupValue(ctx, argv[0]);
    }
    return JS_NewBool(ctx, r);
}

#endif
