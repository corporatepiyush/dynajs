#include "ui-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_UI)

static int uib_size_val(JSContext* ctx, JSValue v, const char* who, dyn_ui_size* sz)
{
    if (JS_IsUndefined(v))
        return 0;
    if (JS_IsNumber(v)) {
        double d;
        if (uib_num(ctx, v, who, 0, 16777216, &d))
            return -1;
        sz->kind = DYN_UI_FIXED;
        sz->value = (float)d;
        return 0;
    }
    if (JS_IsString(v)) {
        size_t n;
        const char* s = JS_ToCStringLen(ctx, &n, v);
        int rc = 0;
        if (!s)
            return -1;
        if (!strcmp(s, "fit"))
            sz->kind = DYN_UI_FIT;
        else if (!strcmp(s, "grow")) {
            sz->kind = DYN_UI_GROW;
            sz->value = 1;
        } else if (n >= 2 && n <= 8 && s[n - 1] == '%') {
            char* end = NULL;
            double p;
            char buf[16];
            memcpy(buf, s, n - 1);
            buf[n - 1] = 0;
            p = strtod(buf, &end);
            if (end != buf + n - 1 || !(p >= 0 && p <= 100) || buf[0] == ' ' || buf[0] == '+' || buf[0] == '-') {
                JS_ThrowRangeError(ctx, "dyna:ui: %s percentage must be 0%%..100%%", who);
                rc = -1;
            } else {
                sz->kind = DYN_UI_PERCENT;
                sz->value = (float)(p / 100);
            }
        } else {
            JS_ThrowTypeError(ctx, "dyna:ui: %s is a pixel count, \"fit\", \"grow\", \"N%%\" or { grow: weight }", who);
            rc = -1;
        }
        JS_FreeCString(ctx, s);
        return rc;
    }
    if (JS_IsObject(v)) {
        static const char* const keys[] = { "grow" };
        JSValue g;
        double d;
        int rc;
        if (uib_check_opts(ctx, v, who, keys, 1))
            return -1;
        g = JS_GetPropertyStr(ctx, v, "grow");
        if (JS_IsException(g))
            return -1;
        rc = uib_num(ctx, g, who, 0, 1e6, &d);
        JS_FreeValue(ctx, g);
        if (rc)
            return -1;
        sz->kind = DYN_UI_GROW;
        sz->value = (float)d;
        return 0;
    }
    JS_ThrowTypeError(ctx, "dyna:ui: %s is a pixel count, \"fit\", \"grow\", \"N%%\" or { grow: weight }", who);
    return -1;
}

static int uib_align(JSContext* ctx, JSValueConst opts, const char* key, uint8_t* out)
{
    JSValue v;
    const char* s;
    int rc = 0;
    if (uib_get_opt(ctx, opts, key, &v))
        return -1;
    if (JS_IsUndefined(v))
        return 0;
    if (!JS_IsString(v)) {
        JS_FreeValue(ctx, v);
        JS_ThrowTypeError(ctx, "dyna:ui: %s is \"start\", \"center\" or \"end\"", key);
        return -1;
    }
    s = JS_ToCString(ctx, v);
    JS_FreeValue(ctx, v);
    if (!s)
        return -1;
    if (!strcmp(s, "start"))
        *out = DYN_UI_START;
    else if (!strcmp(s, "center"))
        *out = DYN_UI_CENTER;
    else if (!strcmp(s, "end"))
        *out = DYN_UI_END;
    else {
        JS_ThrowTypeError(ctx, "dyna:ui: %s is \"start\", \"center\" or \"end\"", key);
        rc = -1;
    }
    JS_FreeCString(ctx, s);
    return rc;
}

static int uib_padding(JSContext* ctx, JSValueConst opts, dyn_ui_layout* l)
{
    JSValue v;
    int rc = 0;
    if (uib_get_opt(ctx, opts, "padding", &v))
        return -1;
    if (JS_IsUndefined(v))
        return 0;
    if (JS_IsNumber(v)) {
        double d;
        rc = uib_num(ctx, v, "padding", 0, 1e6, &d);
        if (!rc)
            l->pad_l = l->pad_t = l->pad_r = l->pad_b = (float)d;
    } else if (JS_IsArray(ctx, v)) {
        int64_t n;
        double p[4] = { 0, 0, 0, 0 };
        int i;
        if (uib_length(ctx, v, &n))
            rc = -1;
        else if (n != 2 && n != 4) {
            JS_ThrowTypeError(ctx, "dyna:ui: padding is a number, [x, y] or [left, top, right, bottom]");
            rc = -1;
        }
        for (i = 0; rc == 0 && i < n; i++) {
            JSValue e = JS_GetPropertyUint32(ctx, v, (uint32_t)i);
            if (JS_IsException(e))
                rc = -1;
            else {
                rc = uib_num(ctx, e, "padding", 0, 1e6, &p[i]);
                JS_FreeValue(ctx, e);
            }
        }
        if (!rc && n == 2) {
            l->pad_l = l->pad_r = (float)p[0];
            l->pad_t = l->pad_b = (float)p[1];
        } else if (!rc) {
            l->pad_l = (float)p[0];
            l->pad_t = (float)p[1];
            l->pad_r = (float)p[2];
            l->pad_b = (float)p[3];
        }
    } else {
        JS_ThrowTypeError(ctx, "dyna:ui: padding is a number, [x, y] or [left, top, right, bottom]");
        rc = -1;
    }
    JS_FreeValue(ctx, v);
    return rc;
}

static const char* const uib_box_keys[] = { "width", "height", "minWidth", "maxWidth", "minHeight", "maxHeight", "padding", "gap", "alignX",
    "alignY", "scroll", "fill", "border", "borderWidth", "radius", "id", "x", "y", "float", "z", "wrap" };

static float uib_pxf(const uib* u, float v)
{
    return v * u->scale;
}

static void uib_scale_size(const uib* u, dyn_ui_size* s)
{
    if (s->kind == DYN_UI_FIXED)
        s->value = uib_pxf(u, s->value);
    s->min = uib_pxf(u, s->min);
    s->max = uib_pxf(u, s->max);
}

static void uib_scale_box(const uib* u, dyn_ui_layout* l, dyn_ui_style* s)
{
    uib_scale_size(u, &l->w);
    uib_scale_size(u, &l->h);
    l->pad_l = roundf(uib_pxf(u, l->pad_l));
    l->pad_t = roundf(uib_pxf(u, l->pad_t));
    l->pad_r = roundf(uib_pxf(u, l->pad_r));
    l->pad_b = roundf(uib_pxf(u, l->pad_b));
    if (l->gap > 0)
        l->gap = roundf(uib_pxf(u, l->gap));
    l->float_x = uib_pxf(u, l->float_x);
    l->float_y = uib_pxf(u, l->float_y);
    if (s->border_w > 0)
        s->border_w = fmaxf(1, roundf(uib_pxf(u, s->border_w)));
    s->radius = uib_pxf(u, s->radius);
}

static int uib_box_opts(JSContext* ctx, JSValueConst opts, const char* who, dyn_ui_layout* l, dyn_ui_style* s, const char** id, size_t* idn)
{
    JSValue v;
    double d;
    *id = NULL;
    *idn = 0;
    if (uib_check_opts(ctx, opts, who, uib_box_keys, (int)countof(uib_box_keys)))
        return -1;
    if (!JS_IsObject(opts))
        return 0;
    if (uib_get_opt(ctx, opts, "width", &v))
        return -1;
    if (uib_size_val(ctx, v, "width", &l->w)) {
        JS_FreeValue(ctx, v);
        return -1;
    }
    JS_FreeValue(ctx, v);
    if (uib_get_opt(ctx, opts, "height", &v))
        return -1;
    if (uib_size_val(ctx, v, "height", &l->h)) {
        JS_FreeValue(ctx, v);
        return -1;
    }
    JS_FreeValue(ctx, v);
    d = (double)l->w.min;
    if (uib_opt_num(ctx, opts, "minWidth", "minWidth", 0, 16777216, &d))
        return -1;
    l->w.min = (float)d;
    d = (double)l->w.max;
    if (uib_opt_num(ctx, opts, "maxWidth", "maxWidth", 0, 16777216, &d))
        return -1;
    l->w.max = (float)d;
    d = (double)l->h.min;
    if (uib_opt_num(ctx, opts, "minHeight", "minHeight", 0, 16777216, &d))
        return -1;
    l->h.min = (float)d;
    d = (double)l->h.max;
    if (uib_opt_num(ctx, opts, "maxHeight", "maxHeight", 0, 16777216, &d))
        return -1;
    l->h.max = (float)d;
    if (uib_padding(ctx, opts, l))
        return -1;
    d = (double)l->gap;
    if (uib_opt_num(ctx, opts, "gap", "gap", 0, 1e6, &d))
        return -1;
    l->gap = (float)d;
    if (uib_align(ctx, opts, "alignX", &l->align_x) || uib_align(ctx, opts, "alignY", &l->align_y))
        return -1;
    if (uib_get_opt(ctx, opts, "scroll", &v))
        return -1;
    if (!JS_IsUndefined(v)) {
        if (JS_IsBool(v)) {
            l->clip_x = l->clip_y = (uint8_t)JS_ToBool(ctx, v);
        } else if (JS_IsString(v)) {
            const char* sc = JS_ToCString(ctx, v);
            int ok = 1;
            if (!sc) {
                JS_FreeValue(ctx, v);
                return -1;
            }
            if (!strcmp(sc, "x"))
                l->clip_x = 1;
            else if (!strcmp(sc, "y"))
                l->clip_y = 1;
            else if (!strcmp(sc, "both"))
                l->clip_x = l->clip_y = 1;
            else
                ok = 0;
            JS_FreeCString(ctx, sc);
            if (!ok) {
                JS_FreeValue(ctx, v);
                JS_ThrowTypeError(ctx, "dyna:ui: scroll is true, \"x\", \"y\" or \"both\"");
                return -1;
            }
        } else {
            JS_FreeValue(ctx, v);
            JS_ThrowTypeError(ctx, "dyna:ui: scroll is true, \"x\", \"y\" or \"both\"");
            return -1;
        }
    }
    JS_FreeValue(ctx, v);
    if (uib_opt_color(ctx, opts, "fill", "fill", &s->fill) || uib_opt_color(ctx, opts, "border", "border", &s->border))
        return -1;
    d = (double)s->border_w;
    if (uib_opt_num(ctx, opts, "borderWidth", "borderWidth", 0, 1e4, &d))
        return -1;
    s->border_w = (float)d;
    if (s->border && !(s->border_w > 0))
        s->border_w = 1;
    d = (double)s->radius;
    if (uib_opt_num(ctx, opts, "radius", "radius", 0, 1e6, &d))
        return -1;
    s->radius = (float)d;
    if (uib_get_opt(ctx, opts, "float", &v))
        return -1;
    if (!JS_IsUndefined(v)) {
        const char* fs = JS_IsString(v) ? JS_ToCString(ctx, v) : NULL;
        int ok = 1;
        if (!fs) {
            JS_FreeValue(ctx, v);
            if (!JS_HasException(ctx))
                JS_ThrowTypeError(ctx, "dyna:ui: float is \"parent\", \"popup\", \"screen\" or \"safe\"");
            return -1;
        }
        if (!strcmp(fs, "parent"))
            l->floating = DYN_UI_FLOAT_PARENT;
        else if (!strcmp(fs, "popup"))
            l->floating = DYN_UI_FLOAT_POPUP;
        else if (!strcmp(fs, "screen"))
            l->floating = DYN_UI_FLOAT_ROOT;
        else if (!strcmp(fs, "safe"))
            l->floating = DYN_UI_FLOAT_SAFE;
        else
            ok = 0;
        JS_FreeCString(ctx, fs);
        if (!ok) {
            JS_FreeValue(ctx, v);
            JS_ThrowTypeError(ctx, "dyna:ui: float is \"parent\", \"popup\", \"screen\" or \"safe\"");
            return -1;
        }
    }
    JS_FreeValue(ctx, v);
    d = (double)l->float_x;
    if (uib_opt_num(ctx, opts, "x", "x", -16777216, 16777216, &d))
        return -1;
    l->float_x = (float)d;
    d = (double)l->float_y;
    if (uib_opt_num(ctx, opts, "y", "y", -16777216, 16777216, &d))
        return -1;
    l->float_y = (float)d;
    d = l->z;
    if (uib_opt_num(ctx, opts, "z", "z", -1000000, 1000000, &d))
        return -1;
    if (d != floor(d)) {
        JS_ThrowRangeError(ctx, "dyna:ui: z is an integer");
        return -1;
    }
    l->z = (int32_t)d;
    {
        int wr = 0;
        if (uib_opt_bool(ctx, opts, "wrap", &wr))
            return -1;
        l->wrap = (uint8_t)wr;
        if (wr && l->dir != DYN_UI_ROW) {
            JS_ThrowTypeError(ctx, "dyna:ui: wrap lays a row out in lines; a column does not wrap");
            return -1;
        }
    }
    return uib_opt_str(ctx, opts, "id", "id", id, idn);
}

JSValue uib_container(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    uib* u;
    dyn_ui_layout l;
    dyn_ui_style s;
    const char* id = NULL;
    size_t idn = 0;
    JSValueConst opts = argc > 1 ? argv[0] : JS_UNDEFINED, fn = argc > 1 ? argv[1] : argc > 0 ? argv[0]
                                                                                              : JS_UNDEFINED;
    static const char* const who[] = { "row", "column", "box", "wrap" };
    int failed;
    memset(&l, 0, sizeof(l));
    memset(&s, 0, sizeof(s));
    l.dir = magic == 0 || magic == 3 ? DYN_UI_ROW : DYN_UI_COLUMN;
    l.wrap = magic == 3;
    l.w.kind = l.h.kind = DYN_UI_FIT;
    if (magic != 2) {
        l.gap = -1;
        l.align_y = magic == 0 || magic == 3 ? DYN_UI_CENTER : DYN_UI_START;
    }
    if (uib_fn(ctx, fn, who[magic]) || uib_box_opts(ctx, opts, who[magic], &l, &s, &id, &idn))
        return JS_EXCEPTION;
    u = uib_in_frame(ctx, this_val, who[magic]);
    if (!u) {
        if (id)
            JS_FreeCString(ctx, id);
        return JS_EXCEPTION;
    }
    uib_scale_box(u, &l, &s);
    if (l.gap < 0)
        l.gap = magic == 0 ? dyn_ui_resolved(u->ui)->pad_x : dyn_ui_resolved(u->ui)->spacing;
    if (magic == 3) {
        l.w.kind = l.w.kind == DYN_UI_FIT ? DYN_UI_GROW : l.w.kind;
        if (l.w.kind == DYN_UI_GROW && !(l.w.value > 0))
            l.w.value = 1;
    }
    {
        dyn_ui_id key = id ? dyn_ui_id_of(u->ui, id, idn) : 0;
        if (id)
            JS_FreeCString(ctx, id);
        if (!key && (l.clip_x || l.clip_y))
            return JS_ThrowTypeError(ctx, "dyna:ui: a %s that scrolls needs an id (its scroll offset is kept under it)", who[magic]);
        l.interactive = key != 0;
        dyn_ui_open(u->ui, key, &l, &s);
        if (key)
            dyn_ui_push_id(u->ui, key);
        failed = uib_run(ctx, fn, this_val);
        if (key)
            dyn_ui_pop_id(u->ui);
        dyn_ui_close(u->ui);
        if (key)
            dyn_ui_set_last(u->ui, key);
    }
    return failed ? JS_EXCEPTION : JS_UNDEFINED;
}

JSValue uib_wrap(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    static const char* const who[] = { "property", "group", "panel", "scroll", "menubar", "toolbar", "statusbar" };
    static const char* const pkeys[] = { "width", "height", "border" };
    uib* u;
    const char* s = NULL;
    size_t n = 0;
    int nolabel = magic >= UIB_MENUBAR, has_opts = magic == UIB_PANEL || magic == UIB_SCROLL;
    JSValueConst fn = argc > (nolabel ? 0 : has_opts ? 2
                                                     : 1)
        ? argv[nolabel ? 0 : has_opts ? 2
                                      : 1]
        : JS_UNDEFINED;
    JSValueConst opts = has_opts && argc > 1 ? argv[1] : JS_UNDEFINED;
    double w = 0, h = 0;
    int border = magic == UIB_PANEL, failed;
    if (has_opts && argc == 2) {
        fn = argv[1];
        opts = JS_UNDEFINED;
    }
    if (uib_fn(ctx, fn, who[magic]))
        return JS_EXCEPTION;
    if (has_opts && (uib_check_opts(ctx, opts, who[magic], pkeys, 3) || uib_opt_num(ctx, opts, "width", "width", 0, 16384, &w) || uib_opt_num(ctx, opts, "height", "height", 0, 16384, &h) || uib_opt_bool(ctx, opts, "border", &border)))
        return JS_EXCEPTION;
    if (!nolabel && uib_str(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, who[magic], &s, &n))
        return JS_EXCEPTION;
    u = uib_in_frame(ctx, this_val, who[magic]);
    if (!u) {
        if (s)
            JS_FreeCString(ctx, s);
        return JS_EXCEPTION;
    }
    switch (magic) {
    case UIB_PROPERTY:
        dyn_ui_w_property_begin(u->ui, s, n);
        break;
    case UIB_GROUP:
        dyn_ui_w_group_begin(u->ui, s, n);
        break;
    case UIB_PANEL:
    case UIB_SCROLL:
        dyn_ui_w_panel_begin(u->ui, s, n, uib_px(u, w), uib_px(u, h), border);
        break;
    case UIB_MENUBAR:
        dyn_ui_w_menubar_begin(u->ui);
        break;
    case UIB_TOOLBAR:
        dyn_ui_w_toolbar_begin(u->ui);
        break;
    default:
        dyn_ui_w_statusbar_begin(u->ui);
        break;
    }
    if (s)
        JS_FreeCString(ctx, s);
    failed = uib_run(ctx, fn, this_val);
    switch (magic) {
    case UIB_PROPERTY:
        dyn_ui_w_property_end(u->ui);
        break;
    case UIB_GROUP:
        dyn_ui_w_group_end(u->ui);
        break;
    case UIB_PANEL:
    case UIB_SCROLL:
        dyn_ui_w_panel_end(u->ui);
        break;
    case UIB_MENUBAR:
        dyn_ui_w_menubar_end(u->ui);
        break;
    default:
        dyn_ui_w_bar_end(u->ui);
        break;
    }
    return failed ? JS_EXCEPTION : JS_UNDEFINED;
}

#endif
