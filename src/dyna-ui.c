#include "cutils.h"
#include "dyna-nat.h"
#include "dyna-libc.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_UI)

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "core/dyn-ui.h"
#include "dyna-ui-backend.h"

static const uint8_t ui_proggy[] = {
#include "proggy_clean.inc"
};

static const uint8_t ui_inter[] = {
#include "inter.inc"
};

#define UIB_MAX_ITEMS 65536
#define UIB_MAX_COLS 256

typedef struct {
    dyn_ui_t* ui;
    int w, h;
    float scale, lw, lh;
    float soft_insets[4];
    float insets_px[4];
    int touch_mode, touch_done;
    uint8_t* px;
    int present;
    int busy;
    int manual_clock;
    int events;
    int frames;
    int raster_ok;
    uint32_t clear;
    JSValue target_obj;
    JSRuntime* rt;
    const dyn_uib_ops* ops;
    void* be;
    char** strs;
    size_t nstrs, cstrs;
    JSContext* cb_ctx;
    JSValue cb_fn;
    JSValue cb_arr;
    int cb_failed;
    int activated;
    JSObjectTemplate* rect_tpl;
    JSObjectTemplate* table_tpl;
    JSValue dnd_payload;
} uib;

static JSClassID uib_cid, uib_u8_cid, uib_f64_cid, uib_f32_cid;

static int uib_view(JSContext* ctx, uib* u);

static float uib_px(const uib* u, double v)
{
    return (float)(v * (double)u->scale);
}

static void uib_dispose(void* p)
{
    uib* u = p;
    if (!u)
        return;
    if (u->ops)
        u->ops->release(u->be, u->target_obj);
    dyn_ui_free(u->ui);
    dyn_nat_free(u->px);
    dyn_nat_free(u->strs);
    if (u->rt) {
        JS_FreeValueRT(u->rt, u->target_obj);
        JS_FreeObjectTemplateRT(u->rt, u->rect_tpl);
        JS_FreeObjectTemplateRT(u->rt, u->table_tpl);
        JS_FreeValueRT(u->rt, u->dnd_payload);
    }
    dyn_nat_free(u);
}

static JSObjectTemplate* uib_template(JSContext* ctx, const char* const* names, int n)
{
    JSAtom at[8];
    JSObjectTemplate* t = NULL;
    int k, ok = 1;
    for (k = 0; k < n; k++)
        if ((at[k] = JS_NewAtom(ctx, names[k])) == JS_ATOM_NULL)
            ok = 0;
    if (ok)
        t = JS_NewObjectTemplate(ctx, at, (uint32_t)n);
    for (k = 0; k < n; k++)
        if (at[k] != JS_ATOM_NULL)
            JS_FreeAtom(ctx, at[k]);
    return t;
}

static JSValue uib_fill(JSContext* ctx, JSObjectTemplate* t, int n, JSValue* vals)
{
    JSValue o = JS_NewObjectFromTemplate(ctx, t);
    int k;
    for (k = 0; k < n; k++) {
        if (JS_IsException(o))
            JS_FreeValue(ctx, vals[k]);
        else
            JS_SetTemplateCell(ctx, o, (uint32_t)k, vals[k]);
    }
    return o;
}

static void uib_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark)
{
    DynResource* r = JS_GetOpaque(val, uib_cid);
    uib* u = r ? r->native : NULL;
    if (u) {
        JS_MarkValue(rt, u->target_obj, mark);
        JS_MarkObjectTemplate(rt, u->rect_tpl, mark);
        JS_MarkObjectTemplate(rt, u->table_tpl, mark);
    }
}

static uib* uib_get(JSContext* ctx, JSValueConst this_val)
{
    uib* u = dyn_res_native(ctx, this_val, uib_cid);
    if (!u)
        return NULL;
    return u;
}

static uib* uib_in_frame(JSContext* ctx, JSValueConst this_val, const char* who)
{
    uib* u = uib_get(ctx, this_val);
    if (!u)
        return NULL;
    if (!u->busy) {
        JS_ThrowTypeError(ctx, "dyna:ui: %s() is called inside frame(), between the widgets it lays out", who);
        return NULL;
    }
    return u;
}

static int uib_str(JSContext* ctx, JSValueConst v, const char* who, const char** s, size_t* n)
{
    if (!JS_IsString(v)) {
        JS_ThrowTypeError(ctx, "dyna:ui: %s needs a string", who);
        return -1;
    }
    *s = JS_ToCStringLen(ctx, n, v);
    if (!*s)
        return -1;
    if (*n > (1u << 26)) {
        JS_FreeCString(ctx, *s);
        JS_ThrowRangeError(ctx, "dyna:ui: %s is longer than 64 MiB", who);
        return -1;
    }
    return 0;
}

static void uib_bound(char* buf, size_t cap, double x)
{
    if (x == floor(x) && fabs(x) < 1e15)
        snprintf(buf, cap, "%.0f", x);
    else
        snprintf(buf, cap, "%g", x);
}

static int uib_num(JSContext* ctx, JSValueConst v, const char* who, double lo, double hi, double* out)
{
    double d;
    if (!JS_IsNumber(v)) {
        JS_ThrowTypeError(ctx, "dyna:ui: %s must be a number", who);
        return -1;
    }
    if (JS_ToFloat64(ctx, &d, v))
        return -1;
    if (!(d >= lo && d <= hi)) {
        char lb[32], hb[32];
        uib_bound(lb, sizeof(lb), lo);
        uib_bound(hb, sizeof(hb), hi);
        JS_ThrowRangeError(ctx, "dyna:ui: %s must be a finite number in %s..%s", who, lb, hb);
        return -1;
    }
    *out = d;
    return 0;
}

static int uib_value(JSContext* ctx, JSValueConst v, const char* who, double* out)
{
    if (!JS_IsNumber(v)) {
        JS_ThrowTypeError(ctx, "dyna:ui: %s must be a number", who);
        return -1;
    }
    return JS_ToFloat64(ctx, out, v);
}

static int uib_color(JSContext* ctx, JSValueConst v, const char* who, uint32_t* out)
{
    double d;
    if (uib_num(ctx, v, who, 0, 4294967295.0, &d))
        return -1;
    if (d != floor(d)) {
        JS_ThrowRangeError(ctx, "dyna:ui: %s is a 0xRRGGBBAA integer", who);
        return -1;
    }
    *out = (uint32_t)d;
    return 0;
}

static int uib_bool(JSContext* ctx, JSValueConst v, int* out)
{
    int b = JS_ToBool(ctx, v);
    if (b < 0)
        return -1;
    *out = b;
    return 0;
}

static int uib_fn(JSContext* ctx, JSValueConst v, const char* who)
{
    if (!JS_IsFunction(ctx, v)) {
        JS_ThrowTypeError(ctx, "dyna:ui: %s needs a function", who);
        return -1;
    }
    return 0;
}

static int uib_check_opts_m(JSContext* ctx, JSValueConst opts, const char* who, const char* const* keys, int nkeys, uint32_t* present)
{
    JSPropertyEnum* tab;
    uint32_t len, i;
    int j, rc = 0;
    if (present)
        *present = 0;
    if (JS_IsUndefined(opts))
        return 0;
    if (!JS_IsObject(opts) || JS_IsFunction(ctx, opts) || JS_IsArray(ctx, opts)) {
        JS_ThrowTypeError(ctx, "dyna:ui: %s options must be a plain object", who);
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
            if (!strcmp(k, keys[j])) {
                ok = 1;
                if (present && j < 32)
                    *present |= 1u << j;
            }
        if (!ok) {
            JS_ThrowTypeError(ctx, "dyna:ui: %s: unknown option '%s'", who, k);
            rc = -1;
        }
        JS_FreeCString(ctx, k);
    }
    JS_FreePropertyEnum(ctx, tab, len);
    return rc;
}

static int uib_check_opts(JSContext* ctx, JSValueConst opts, const char* who, const char* const* keys, int nkeys)
{
    return uib_check_opts_m(ctx, opts, who, keys, nkeys, NULL);
}

static int uib_get_opt(JSContext* ctx, JSValueConst opts, const char* key, JSValue* out)
{
    *out = JS_UNDEFINED;
    if (!JS_IsObject(opts))
        return 0;
    *out = JS_GetPropertyStr(ctx, opts, key);
    return JS_IsException(*out) ? -1 : 0;
}

static int uib_opt_num(JSContext* ctx, JSValueConst opts, const char* key, const char* who, double lo, double hi, double* out)
{
    JSValue v;
    int rc;
    if (uib_get_opt(ctx, opts, key, &v))
        return -1;
    if (JS_IsUndefined(v))
        return 0;
    rc = uib_num(ctx, v, who, lo, hi, out);
    JS_FreeValue(ctx, v);
    return rc;
}

static int uib_opt_bool(JSContext* ctx, JSValueConst opts, const char* key, int* out)
{
    JSValue v;
    int rc;
    if (uib_get_opt(ctx, opts, key, &v))
        return -1;
    if (JS_IsUndefined(v))
        return 0;
    rc = uib_bool(ctx, v, out);
    JS_FreeValue(ctx, v);
    return rc;
}

static int uib_opt_color(JSContext* ctx, JSValueConst opts, const char* key, const char* who, uint32_t* out)
{
    JSValue v;
    int rc;
    if (uib_get_opt(ctx, opts, key, &v))
        return -1;
    if (JS_IsUndefined(v))
        return 0;
    rc = uib_color(ctx, v, who, out);
    JS_FreeValue(ctx, v);
    return rc;
}

static int uib_opt_str(JSContext* ctx, JSValueConst opts, const char* key, const char* who, const char** s, size_t* n)
{
    JSValue v;
    int rc;
    *s = NULL;
    *n = 0;
    if (uib_get_opt(ctx, opts, key, &v))
        return -1;
    if (JS_IsUndefined(v))
        return 0;
    rc = uib_str(ctx, v, who, s, n);
    JS_FreeValue(ctx, v);
    return rc;
}

static int uib_length(JSContext* ctx, JSValueConst v, int64_t* out)
{
    JSValue l = JS_GetPropertyStr(ctx, v, "length");
    double d;
    int rc;
    if (JS_IsException(l))
        return -1;
    rc = JS_ToFloat64(ctx, &d, l);
    JS_FreeValue(ctx, l);
    if (rc)
        return -1;
    if (!(d >= 0 && d <= 9007199254740991.0) || d != floor(d)) {
        JS_ThrowRangeError(ctx, "dyna:ui: a length that is not a whole number");
        return -1;
    }
    *out = (int64_t)d;
    return 0;
}

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

static double uib_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void uib_density(dyn_ui_theme* t, int touch)
{
    t->min_target = touch ? 44 : 0;
    t->pad_x = touch ? 10 : 6;
    t->pad_y = touch ? 8 : 4;
    t->spacing = touch ? 8 : 4;
}

static int uib_insets_opt(JSContext* ctx, JSValueConst opts, float out[4])
{
    JSValue v;
    int64_t n;
    int k, rc = 0;
    if (uib_get_opt(ctx, opts, "insets", &v))
        return -1;
    if (JS_IsUndefined(v))
        return 0;
    if (!JS_IsArray(ctx, v) || uib_length(ctx, v, &n) || n != 4) {
        JS_FreeValue(ctx, v);
        if (!JS_HasException(ctx))
            JS_ThrowTypeError(ctx, "dyna:ui: Context insets is [left, top, right, bottom]");
        return -1;
    }
    for (k = 0; k < 4 && !rc; k++) {
        JSValue e = JS_GetPropertyUint32(ctx, v, (uint32_t)k);
        double d = 0;
        rc = JS_IsException(e) || uib_num(ctx, e, "an inset", 0, 4096, &d);
        JS_FreeValue(ctx, e);
        out[k] = (float)d;
    }
    JS_FreeValue(ctx, v);
    return rc ? -1 : 0;
}

static JSValue uib_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv)
{
    static const char* const keys[] = { "width", "height", "scale", "insets", "touch", "target", "present", "clock", "clear" };
    JSValueConst opts = argc > 0 ? argv[0] : JS_UNDEFINED;
    double w = 0, h = 0, scale = 1;
    int present = 1, touch = 0, has_scale = 0, has_insets = 0, k;
    uint32_t clear = 0;
    const char* clock_s = NULL;
    size_t clock_n = 0;
    float insets[4] = { 0, 0, 0, 0 };
    JSValue target = JS_UNDEFINED, tv, obj;
    uib* u;
    if (!JS_IsObject(opts))
        return JS_ThrowTypeError(ctx, "dyna:ui: new Context({ width, height }) or new Context({ target })");
    if (uib_check_opts(ctx, opts, "Context", keys, (int)countof(keys)) || uib_opt_num(ctx, opts, "width", "Context width", 1, 16384, &w)
        || uib_opt_num(ctx, opts, "height", "Context height", 1, 16384, &h) || uib_opt_num(ctx, opts, "scale", "Context scale", 0.25, 16, &scale)
        || uib_opt_bool(ctx, opts, "present", &present) || uib_opt_color(ctx, opts, "clear", "Context clear", &clear) || uib_insets_opt(ctx, opts, insets))
        return JS_EXCEPTION;
    if (uib_get_opt(ctx, opts, "scale", &tv))
        return JS_EXCEPTION;
    has_scale = !JS_IsUndefined(tv);
    JS_FreeValue(ctx, tv);
    if (uib_get_opt(ctx, opts, "insets", &tv))
        return JS_EXCEPTION;
    has_insets = !JS_IsUndefined(tv);
    JS_FreeValue(ctx, tv);
    if (uib_get_opt(ctx, opts, "touch", &tv))
        return JS_EXCEPTION;
    if (JS_IsString(tv)) {
        const char* ts = JS_ToCString(ctx, tv);
        int ok = ts && !strcmp(ts, "auto");
        if (ts)
            JS_FreeCString(ctx, ts);
        JS_FreeValue(ctx, tv);
        if (!ok)
            return JS_ThrowTypeError(ctx, "dyna:ui: Context touch is true, false or \"auto\"");
        touch = 2;
    } else if (!JS_IsUndefined(tv)) {
        touch = JS_ToBool(ctx, tv);
        JS_FreeValue(ctx, tv);
    }
    if (uib_opt_str(ctx, opts, "clock", "Context clock", &clock_s, &clock_n))
        return JS_EXCEPTION;
    if (clock_s && strcmp(clock_s, "manual") && strcmp(clock_s, "monotonic")) {
        JS_FreeCString(ctx, clock_s);
        return JS_ThrowTypeError(ctx, "dyna:ui: Context clock is \"monotonic\" or \"manual\"");
    }
    if (uib_get_opt(ctx, opts, "target", &target)) {
        if (clock_s)
            JS_FreeCString(ctx, clock_s);
        return JS_EXCEPTION;
    }
    u = dyn_nat_calloc(1, sizeof(*u));
    if (!u) {
        if (clock_s)
            JS_FreeCString(ctx, clock_s);
        JS_FreeValue(ctx, target);
        return JS_ThrowOutOfMemory(ctx);
    }
    u->manual_clock = clock_s && !strcmp(clock_s, "manual");
    if (clock_s)
        JS_FreeCString(ctx, clock_s);
    u->rt = JS_GetRuntime(ctx);
    u->target_obj = JS_UNDEFINED;
    {
        static const char* const rect_names[4] = { "x", "y", "width", "height" };
        static const char* const table_names[8] = { "selected", "sortColumn", "descending", "column", "activated", "edit", "committed", "cancelled" };
        u->dnd_payload = JS_UNDEFINED;
        u->rect_tpl = uib_template(ctx, rect_names, 4);
        u->table_tpl = uib_template(ctx, table_names, 8);
        if (!u->rect_tpl || !u->table_tpl) {
            uib_dispose(u);
            JS_FreeValue(ctx, target);
            return JS_ThrowOutOfMemory(ctx);
        }
    }
    u->present = present;
    u->touch_mode = touch;
    u->activated = -1;
    u->ui = dyn_ui_new();
    if (!u->ui || dyn_ui_font_add(u->ui, ui_proggy, sizeof(ui_proggy), 0) != 0 || dyn_ui_font_add(u->ui, ui_inter, sizeof(ui_inter), 0) != 1) {
        uib_dispose(u);
        JS_FreeValue(ctx, target);
        return u->ui ? JS_ThrowInternalError(ctx, "dyna:ui: a built-in font did not load") : JS_ThrowOutOfMemory(ctx);
    }
    if (touch == 1)
        uib_density(dyn_ui_theme_of(u->ui), 1);
    u->clear = clear ? clear : dyn_ui_theme_of(u->ui)->bg;
    if (JS_IsUndefined(target)) {
        if (!(w > 0) || !(h > 0)) {
            uib_dispose(u);
            return JS_ThrowTypeError(ctx, "dyna:ui: a Context without a target needs { width, height }");
        }
        if (touch == 2) {
            uib_dispose(u);
            return JS_ThrowTypeError(ctx, "dyna:ui: touch \"auto\" asks the target's platform; a Context without a target takes true or false");
        }
        u->lw = (float)w;
        u->lh = (float)h;
        u->scale = (float)scale;
        memcpy(u->soft_insets, insets, sizeof(insets));
        for (k = 0; k < 4; k++)
            u->insets_px[k] = insets[k] * (float)scale;
        u->w = (int)lround(w * scale);
        u->h = (int)lround(h * scale);
        if (u->w < 1 || u->h < 1 || u->w > 16384 || u->h > 16384) {
            uib_dispose(u);
            return JS_ThrowRangeError(ctx, "dyna:ui: width * scale and height * scale must be 1..16384 pixels");
        }
        dyn_ui_set_clipboard(u->ui, NULL, NULL, NULL);
    } else {
        u->target_obj = target;
        target = JS_UNDEFINED;
        if (w || h || has_scale || has_insets) {
            uib_dispose(u);
            return JS_ThrowTypeError(ctx, "dyna:ui: a Context with a target takes its size, scale and insets from the target");
        }
        u->scale = 1;
#ifdef CONFIG_SDL3
        if (!u->be && !JS_HasException(ctx))
            u->be = dyn_uib_sdl_attach(ctx, u->target_obj, u->ui, &u->ops);
#endif
#ifdef CONFIG_RAYLIB
        if (!u->be && !JS_HasException(ctx))
            u->be = dyn_uib_raylib_attach(ctx, u->target_obj, u->ui, &u->ops);
#endif
        if (!u->be) {
            uib_dispose(u);
            if (!JS_HasException(ctx))
                JS_ThrowTypeError(ctx, "dyna:ui: Context target is a dyna:sdl Renderer or a dyna:raylib Window (this build has %s)",
#if defined(CONFIG_SDL3) && defined(CONFIG_RAYLIB)
                    "both"
#elif defined(CONFIG_SDL3)
                    "dyna:sdl only"
#elif defined(CONFIG_RAYLIB)
                    "dyna:raylib only"
#else
                    "neither; build with CONFIG_SDL3=y or CONFIG_RAYLIB=y"
#endif
                );
            return JS_EXCEPTION;
        }
    }
    if (u->ops && uib_view(ctx, u)) {
        uib_dispose(u);
        return JS_EXCEPTION;
    }
    obj = dyn_res_wrap(ctx, new_target, uib_cid, u, uib_dispose);
    return obj;
}

static void uib_strs_free(JSContext* ctx, uib* u)
{
    size_t i;
    for (i = 0; i < u->nstrs; i++)
        JS_FreeCString(ctx, u->strs[i]);
    u->nstrs = 0;
}

static const char* uib_keep(JSContext* ctx, uib* u, JSValueConst v, size_t* n)
{
    const char* s;
    if (!JS_IsString(v)) {
        JS_ThrowTypeError(ctx, "dyna:ui: items must be strings");
        return NULL;
    }
    if (u->nstrs == u->cstrs) {
        size_t nc = u->cstrs ? u->cstrs * 2 : 64;
        char** p = dyn_nat_realloc(u->strs, nc * sizeof(char*));
        if (!p) {
            JS_ThrowOutOfMemory(ctx);
            return NULL;
        }
        u->strs = p;
        u->cstrs = nc;
    }
    s = JS_ToCStringLen(ctx, n, v);
    if (s)
        u->strs[u->nstrs++] = (char*)(uintptr_t)s;
    return s;
}

static int uib_items(JSContext* ctx, uib* u, JSValueConst arr, const char* who, const char*** items, size_t** lens, int* count)
{
    int64_t n;
    int i;
    *items = NULL;
    *lens = NULL;
    *count = 0;
    if (!JS_IsArray(ctx, arr)) {
        JS_ThrowTypeError(ctx, "dyna:ui: %s needs an array of strings", who);
        return -1;
    }
    if (uib_length(ctx, arr, &n))
        return -1;
    if (n > UIB_MAX_ITEMS) {
        JS_ThrowRangeError(ctx, "dyna:ui: %s takes at most %d items", who, UIB_MAX_ITEMS);
        return -1;
    }
    if (n == 0)
        return 0;
    *items = dyn_nat_malloc((size_t)n * sizeof(char*));
    *lens = dyn_nat_malloc((size_t)n * sizeof(size_t));
    if (!*items || !*lens) {
        dyn_nat_free(*items);
        dyn_nat_free(*lens);
        *items = NULL;
        *lens = NULL;
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    for (i = 0; i < n; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, arr, (uint32_t)i);
        const char* s;
        if (JS_IsException(e))
            goto fail;
        s = uib_keep(ctx, u, e, &(*lens)[i]);
        JS_FreeValue(ctx, e);
        if (!s)
            goto fail;
        (*items)[i] = s;
    }
    *count = (int)n;
    return 0;
fail:
    dyn_nat_free(*items);
    dyn_nat_free(*lens);
    *items = NULL;
    *lens = NULL;
    return -1;
}

static JSValue uib_ui_error(JSContext* ctx, uib* u)
{
    return JS_ThrowRangeError(ctx, "dyna:ui: %s", dyn_ui_error_text(u->ui));
}

static JSValue uib_call(JSContext* ctx, JSValueConst fn, JSValueConst self)
{
    JSValueConst a[1];
    a[0] = self;
    return JS_Call(ctx, fn, JS_UNDEFINED, 1, a);
}

static int uib_run(JSContext* ctx, JSValueConst fn, JSValueConst self)
{
    JSValue r = uib_call(ctx, fn, self);
    if (JS_IsException(r))
        return -1;
    if (JS_IsObject(r) && JS_PromiseState(ctx, r) >= 0) {
        JS_FreeValue(ctx, r);
        JS_ThrowTypeError(ctx, "dyna:ui: a layout callback is synchronous; an async function would lay its widgets out after the frame ends");
        return -1;
    }
    JS_FreeValue(ctx, r);
    return 0;
}

static int uib_view(JSContext* ctx, uib* u)
{
    float ins[4];
    int k;
    if (u->ops) {
        dyn_uib_view v;
        memset(&v, 0, sizeof(v));
        if (u->ops->begin(u->be, ctx, u->target_obj, u->ui, &v))
            return -1;
        u->w = (int)v.w;
        u->h = (int)v.h;
        u->scale = v.scale >= 0.25f && v.scale <= 16 ? v.scale : 1;
        memcpy(ins, v.insets, sizeof(ins));
        if (u->touch_mode == 2 && !u->touch_done) {
            uib_density(dyn_ui_theme_of(u->ui), v.touch);
            u->touch_done = 1;
        }
    } else {
        for (k = 0; k < 4; k++)
            ins[k] = u->soft_insets[k] * u->scale;
    }
    if (u->w < 1 || u->h < 1 || u->w > 16384 || u->h > 16384) {
        JS_ThrowRangeError(ctx, "dyna:ui: the target is %dx%d pixels; frames are 1..16384 on each side", u->w, u->h);
        return -1;
    }
    for (k = 0; k < 4; k++) {
        float lim = (float)(k % 2 ? u->h : u->w);
        ins[k] = ins[k] > 0 ? (ins[k] < lim ? ins[k] : lim) : 0;
    }
    dyn_ui_set_scale(u->ui, u->scale);
    dyn_ui_set_insets(u->ui, ins[0], ins[1], ins[2], ins[3]);
    memcpy(u->insets_px, ins, sizeof(ins));
    return 0;
}

static JSValue uib_frame(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    uib* u;
    int changed = 0, rc, failed;
    DynResource* res;
    if (argc < 1 || uib_fn(ctx, argv[0], "frame(fn)"))
        return JS_EXCEPTION;
    res = dyn_res_get(ctx, this_val, uib_cid);
    u = res ? res->native : NULL;
    if (!u)
        return JS_EXCEPTION;
    dyn_res_hold(ctx, res);
    if (u->busy)
        return JS_ThrowTypeError(ctx, "dyna:ui: frame() inside frame() is refused");
    if (!u->manual_clock)
        dyn_ui_set_time(u->ui, uib_now());
    if (uib_view(ctx, u))
        return JS_EXCEPTION;
    if (dyn_ui_w_begin(u->ui, (float)u->w, (float)u->h))
        return uib_ui_error(ctx, u);
    u->busy = 1;
    u->events = 0;
    u->activated = -1;
    failed = uib_run(ctx, argv[0], this_val);
    while (dyn_ui_depth(u->ui) > 2)
        dyn_ui_close(u->ui);
    rc = dyn_ui_w_end(u->ui, &changed);
    u->busy = 0;
    if (!dyn_ui_dragging(u->ui) && !JS_IsUndefined(u->dnd_payload)) {
        JS_FreeValue(ctx, u->dnd_payload);
        u->dnd_payload = JS_UNDEFINED;
    }
    uib_strs_free(ctx, u);
    u->frames++;
    if (failed)
        return JS_EXCEPTION;
    if (rc)
        return uib_ui_error(ctx, u);
    if (u->ops) {
        int drawn = u->ops->draw(u->be, ctx, u->target_obj, u->ui, changed, u->present, u->clear);
        if (drawn < 0)
            return JS_EXCEPTION;
    } else if (changed)
        u->raster_ok = 0;
    return JS_NewBool(ctx, changed);
}

static JSValue uib_container(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
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

static JSValue uib_id(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    uib* u;
    const char* s = NULL;
    size_t n = 0;
    double d = 0;
    int failed, isnum = argc > 0 && JS_IsNumber(argv[0]);
    if (argc < 2 || uib_fn(ctx, argv[1], "id(key, fn)"))
        return JS_EXCEPTION;
    if (isnum ? uib_num(ctx, argv[0], "id key", -9007199254740991.0, 9007199254740991.0, &d) : uib_str(ctx, argv[0], "id key", &s, &n))
        return JS_EXCEPTION;
    u = uib_in_frame(ctx, this_val, "id");
    if (!u) {
        if (s)
            JS_FreeCString(ctx, s);
        return JS_EXCEPTION;
    }
    dyn_ui_push_id(u->ui, isnum ? dyn_ui_hash(0x1D, &d, sizeof(d)) : dyn_ui_hash(0, s, n));
    if (s)
        JS_FreeCString(ctx, s);
    failed = uib_run(ctx, argv[1], this_val);
    dyn_ui_pop_id(u->ui);
    return failed ? JS_EXCEPTION : JS_UNDEFINED;
}

static JSValue uib_spacer(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    uib* u;
    double w = 0, h = 0;
    if ((argc > 0 && !JS_IsUndefined(argv[0]) && uib_num(ctx, argv[0], "spacer width", 0, 16777216, &w))
        || (argc > 1 && !JS_IsUndefined(argv[1]) && uib_num(ctx, argv[1], "spacer height", 0, 16777216, &h)))
        return JS_EXCEPTION;
    u = uib_in_frame(ctx, this_val, "spacer");
    if (!u)
        return JS_EXCEPTION;
    dyn_ui_w_spacer(u->ui, uib_px(u, w), uib_px(u, h));
    return JS_UNDEFINED;
}

static JSValue uib_label_like(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    static const char* const who[] = { "label", "heading", "bullet", "separator", "link", "button", "tooltip" };
    uib* u;
    const char* s = NULL;
    size_t n = 0;
    double delay = 0.5;
    int r = 0;
    if (magic == 3 && (argc < 1 || JS_IsUndefined(argv[0]))) {
        u = uib_in_frame(ctx, this_val, who[magic]);
        if (!u)
            return JS_EXCEPTION;
        dyn_ui_w_separator(u->ui, NULL, 0);
        return JS_UNDEFINED;
    }
    if (magic == 6 && argc > 1) {
        static const char* const keys[] = { "delay" };
        if (uib_check_opts(ctx, argv[1], "tooltip", keys, 1) || uib_opt_num(ctx, argv[1], "delay", "tooltip delay", 0, 3600, &delay))
            return JS_EXCEPTION;
    }
    if (uib_str(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, who[magic], &s, &n))
        return JS_EXCEPTION;
    u = uib_in_frame(ctx, this_val, who[magic]);
    if (!u) {
        JS_FreeCString(ctx, s);
        return JS_EXCEPTION;
    }
    switch (magic) {
    case 0:
        dyn_ui_w_label(u->ui, s, n);
        break;
    case 1:
        dyn_ui_w_heading(u->ui, s, n);
        break;
    case 2:
        dyn_ui_w_bullet(u->ui, s, n);
        break;
    case 3:
        dyn_ui_w_separator(u->ui, s, n);
        break;
    case 4:
        r = dyn_ui_w_link(u->ui, s, n);
        break;
    case 5:
        r = dyn_ui_w_button(u->ui, s, n);
        break;
    default:
        dyn_ui_w_tooltip(u->ui, s, n, delay);
        break;
    }
    JS_FreeCString(ctx, s);
    return magic == 4 || magic == 5 ? JS_NewBool(ctx, r) : JS_UNDEFINED;
}

static JSValue uib_text(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    static const char* const keys[] = { "color", "wrap" };
    JSValueConst opts = argc > 1 ? argv[1] : JS_UNDEFINED;
    uib* u;
    const char* s;
    size_t n;
    uint32_t color = 0;
    int wrap = 1;
    if (uib_check_opts(ctx, opts, "text", keys, 2) || uib_opt_color(ctx, opts, "color", "text color", &color) || uib_opt_bool(ctx, opts, "wrap", &wrap)
        || uib_str(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, "text", &s, &n))
        return JS_EXCEPTION;
    u = uib_in_frame(ctx, this_val, "text");
    if (u)
        dyn_ui_w_text(u->ui, s, n, color ? color : dyn_ui_theme_of(u->ui)->text, wrap);
    JS_FreeCString(ctx, s);
    return u ? JS_UNDEFINED : JS_EXCEPTION;
}

static JSValue uib_badge(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    uib* u;
    const char* s;
    size_t n;
    uint32_t color = 0;
    if ((argc > 1 && !JS_IsUndefined(argv[1]) && uib_color(ctx, argv[1], "badge color", &color)) || uib_str(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, "badge", &s, &n))
        return JS_EXCEPTION;
    u = uib_in_frame(ctx, this_val, "badge");
    if (u)
        dyn_ui_w_badge(u->ui, s, n, color);
    JS_FreeCString(ctx, s);
    return u ? JS_UNDEFINED : JS_EXCEPTION;
}

static int uib_handle(JSContext* ctx, uib* u, JSValueConst v, uint32_t* out)
{
    double d;
    int w, h;
    if (uib_num(ctx, v, "image handle", 1, 4095, &d))
        return -1;
    if (d != floor(d) || dyn_ui_image_size(u->ui, (uint32_t)d, &w, &h)) {
        JS_ThrowRangeError(ctx, "dyna:ui: %g is not an image handle from addImage()", d);
        return -1;
    }
    *out = (uint32_t)d;
    return 0;
}

static JSValue uib_image_w(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    uib* u;
    uint32_t img, tint = 0;
    double w = 0, h = 0;
    if ((argc > 1 && !JS_IsUndefined(argv[1]) && uib_num(ctx, argv[1], "image width", 0, 16384, &w))
        || (argc > 2 && !JS_IsUndefined(argv[2]) && uib_num(ctx, argv[2], "image height", 0, 16384, &h))
        || (argc > 3 && !JS_IsUndefined(argv[3]) && uib_color(ctx, argv[3], "image tint", &tint)))
        return JS_EXCEPTION;
    u = uib_in_frame(ctx, this_val, "image");
    if (!u || uib_handle(ctx, u, argc > 0 ? argv[0] : JS_UNDEFINED, &img))
        return JS_EXCEPTION;
    dyn_ui_w_image(u->ui, img, uib_px(u, w), uib_px(u, h), tint);
    return JS_UNDEFINED;
}

static JSValue uib_progress(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    uib* u;
    double f, w = 0;
    if (uib_value(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, "progress", &f)
        || (argc > 1 && !JS_IsUndefined(argv[1]) && uib_num(ctx, argv[1], "progress width", 0, 16384, &w)))
        return JS_EXCEPTION;
    u = uib_in_frame(ctx, this_val, "progress");
    if (!u)
        return JS_EXCEPTION;
    dyn_ui_w_progress(u->ui, f == f ? f : 0, uib_px(u, w));
    return JS_UNDEFINED;
}

static JSValue uib_spinner(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    uib* u;
    double z = 0;
    if (argc > 0 && !JS_IsUndefined(argv[0]) && uib_num(ctx, argv[0], "spinner size", 4, 512, &z))
        return JS_EXCEPTION;
    u = uib_in_frame(ctx, this_val, "spinner");
    if (!u)
        return JS_EXCEPTION;
    dyn_ui_w_spinner(u->ui, uib_px(u, z));
    return JS_UNDEFINED;
}

static JSValue uib_icon_button(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    uib* u;
    const char* s;
    size_t n;
    uint32_t img;
    double z = 0;
    int r;
    if ((argc > 2 && !JS_IsUndefined(argv[2]) && uib_num(ctx, argv[2], "iconButton size", 1, 512, &z))
        || uib_str(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, "iconButton", &s, &n))
        return JS_EXCEPTION;
    u = uib_in_frame(ctx, this_val, "iconButton");
    if (!u || uib_handle(ctx, u, argc > 1 ? argv[1] : JS_UNDEFINED, &img)) {
        JS_FreeCString(ctx, s);
        return JS_EXCEPTION;
    }
    r = dyn_ui_w_icon_button(u->ui, s, n, img, uib_px(u, z));
    JS_FreeCString(ctx, s);
    return JS_NewBool(ctx, r);
}

static JSValue uib_flag(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    static const char* const who[] = { "toggle", "checkbox", "switch", "selectable" };
    uib* u;
    const char* s;
    size_t n;
    int on, r = 0;
    if (argc < 2 || !JS_IsBool(argv[1]))
        return JS_ThrowTypeError(ctx, "dyna:ui: %s(label, value) takes a boolean value", who[magic]);
    on = JS_ToBool(ctx, argv[1]);
    if (uib_str(ctx, argv[0], who[magic], &s, &n))
        return JS_EXCEPTION;
    u = uib_in_frame(ctx, this_val, who[magic]);
    if (!u) {
        JS_FreeCString(ctx, s);
        return JS_EXCEPTION;
    }
    switch (magic) {
    case 0:
        r = dyn_ui_w_toggle(u->ui, s, n, on);
        break;
    case 1:
        r = dyn_ui_w_checkbox(u->ui, s, n, on);
        break;
    case 2:
        r = dyn_ui_w_switch(u->ui, s, n, on);
        break;
    default:
        r = dyn_ui_w_selectable(u->ui, s, n, on);
        break;
    }
    JS_FreeCString(ctx, s);
    return JS_NewBool(ctx, r);
}

static int uib_index(JSContext* ctx, JSValueConst v, const char* who, int* out)
{
    double d;
    if (uib_num(ctx, v, who, -1, UIB_MAX_ITEMS, &d))
        return -1;
    if (d != floor(d)) {
        JS_ThrowRangeError(ctx, "dyna:ui: %s is an integer index", who);
        return -1;
    }
    *out = (int)d;
    return 0;
}

static int uib_note_opts(JSContext* ctx, JSValueConst opts, const char** err, size_t* en, const char** help, size_t* hn)
{
    *err = *help = NULL;
    *en = *hn = 0;
    if (uib_opt_str(ctx, opts, "error", "error", err, en))
        return -1;
    if (uib_opt_str(ctx, opts, "help", "help", help, hn)) {
        if (*err)
            JS_FreeCString(ctx, *err);
        *err = NULL;
        return -1;
    }
    return 0;
}

static void uib_note_free(JSContext* ctx, const char* err, const char* help)
{
    if (err)
        JS_FreeCString(ctx, err);
    if (help)
        JS_FreeCString(ctx, help);
}

static void uib_note_begin(uib* u, size_t en, size_t hn)
{
    if (en || hn) {
        dyn_ui_layout l;
        memset(&l, 0, sizeof(l));
        l.w.kind = DYN_UI_GROW;
        l.w.value = 1;
        l.h.kind = DYN_UI_FIT;
        l.dir = DYN_UI_COLUMN;
        l.gap = roundf(2 * u->scale);
        dyn_ui_open(u->ui, 0, &l, NULL);
    }
    if (en)
        dyn_ui_w_field_error(u->ui, 1);
}

static void uib_note_end(uib* u, const char* err, size_t en, const char* help, size_t hn)
{
    if (en)
        dyn_ui_w_field_error(u->ui, 0);
    if (en || hn) {
        dyn_ui_w_field_note(u->ui, en ? err : help, en ? en : hn, en != 0);
        dyn_ui_close(u->ui);
    }
}

static JSValue uib_choice(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    static const char* const who[] = { "radio", "segmented", "tabs", "combo" };
    uib* u;
    const char* s;
    size_t n, *lens = NULL;
    const char** items = NULL;
    const char *err = NULL, *help = NULL;
    size_t en = 0, hn = 0;
    int sel, count, r, horiz = 0, search = 0;
    if (argc < 3 || uib_index(ctx, argv[2], who[magic], &sel))
        return argc < 3 ? JS_ThrowTypeError(ctx, "dyna:ui: %s(label, items, selected)", who[magic]) : JS_EXCEPTION;
    if (magic == 0 && argc > 3) {
        static const char* const keys[] = { "horizontal" };
        if (uib_check_opts(ctx, argv[3], "radio", keys, 1) || uib_opt_bool(ctx, argv[3], "horizontal", &horiz))
            return JS_EXCEPTION;
    }
    if (magic == 3 && argc > 3) {
        static const char* const keys[] = { "search", "error", "help" };
        if (uib_check_opts(ctx, argv[3], "combo", keys, 3) || uib_opt_bool(ctx, argv[3], "search", &search))
            return JS_EXCEPTION;
    }
    if (uib_str(ctx, argv[0], who[magic], &s, &n))
        return JS_EXCEPTION;
    u = uib_in_frame(ctx, this_val, who[magic]);
    if (!u || uib_items(ctx, u, argv[1], who[magic], &items, &lens, &count)) {
        JS_FreeCString(ctx, s);
        return JS_EXCEPTION;
    }
    if (magic == 3 && argc > 3 && uib_note_opts(ctx, argv[3], &err, &en, &help, &hn)) {
        dyn_nat_free(items);
        dyn_nat_free(lens);
        JS_FreeCString(ctx, s);
        return JS_EXCEPTION;
    }
    if (sel >= count)
        sel = -1;
    switch (magic) {
    case 0:
        r = dyn_ui_w_radio(u->ui, s, n, items, lens, count, sel, horiz);
        break;
    case 1:
        r = dyn_ui_w_segmented(u->ui, s, n, items, lens, count, sel);
        break;
    case 2:
        r = dyn_ui_w_tabs(u->ui, s, n, items, lens, count, sel);
        break;
    default:
        uib_note_begin(u, en, hn);
        r = dyn_ui_w_combo_ex(u->ui, s, n, items, lens, count, sel, search);
        uib_note_end(u, err, en, help, hn);
        uib_note_free(ctx, err, help);
        break;
    }
    dyn_nat_free(items);
    dyn_nat_free(lens);
    JS_FreeCString(ctx, s);
    return JS_NewInt32(ctx, r);
}

static int uib_decimals(JSContext* ctx, JSValueConst opts, int* dec)
{
    double d = *dec;
    if (uib_opt_num(ctx, opts, "decimals", "decimals", 0, 9, &d))
        return -1;
    if (d != floor(d)) {
        JS_ThrowRangeError(ctx, "dyna:ui: decimals is an integer 0..9");
        return -1;
    }
    *dec = (int)d;
    return 0;
}

static JSValue uib_slider(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    static const char* const keys[] = { "step", "decimals", "vertical", "length" };
    JSValueConst opts = argc > 4 ? argv[4] : JS_UNDEFINED;
    uib* u;
    const char* s;
    size_t n;
    double v, lo, hi, step = 0, len = 0, r;
    int dec = 2, vert = 0;
    if (argc < 4)
        return JS_ThrowTypeError(ctx, "dyna:ui: slider(label, value, min, max, opts?)");
    if (uib_value(ctx, argv[1], "slider value", &v) || uib_num(ctx, argv[2], "slider min", -1e300, 1e300, &lo)
        || uib_num(ctx, argv[3], "slider max", -1e300, 1e300, &hi) || uib_check_opts(ctx, opts, "slider", keys, 4)
        || uib_opt_num(ctx, opts, "step", "slider step", 0, 1e300, &step) || uib_decimals(ctx, opts, &dec) || uib_opt_bool(ctx, opts, "vertical", &vert)
        || uib_opt_num(ctx, opts, "length", "slider length", 0, 16384, &len))
        return JS_EXCEPTION;
    if (!(lo < hi))
        return JS_ThrowRangeError(ctx, "dyna:ui: slider min must be below max");
    if (uib_str(ctx, argv[0], "slider", &s, &n))
        return JS_EXCEPTION;
    u = uib_in_frame(ctx, this_val, "slider");
    if (!u) {
        JS_FreeCString(ctx, s);
        return JS_EXCEPTION;
    }
    r = dyn_ui_w_slider(u->ui, s, n, v, lo, hi, step, dec, vert, uib_px(u, len));
    JS_FreeCString(ctx, s);
    return r == v ? JS_DupValue(ctx, argv[1]) : JS_NewFloat64(ctx, r);
}

static JSValue uib_range(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    static const char* const keys[] = { "step", "decimals" };
    JSValueConst opts = argc > 4 ? argv[4] : JS_UNDEFINED;
    uib* u;
    const char* s;
    size_t n;
    double a, b, a0, b0, lo, hi, step = 0;
    int dec = 2;
    int64_t len;
    JSValue e0, e1, out;
    if (argc < 4 || !JS_IsArray(ctx, argv[1]))
        return JS_ThrowTypeError(ctx, "dyna:ui: range(label, [low, high], min, max, opts?)");
    if (uib_length(ctx, argv[1], &len))
        return JS_EXCEPTION;
    if (len != 2)
        return JS_ThrowTypeError(ctx, "dyna:ui: range value is [low, high]");
    e0 = JS_GetPropertyUint32(ctx, argv[1], 0);
    e1 = JS_GetPropertyUint32(ctx, argv[1], 1);
    if (JS_IsException(e0) || JS_IsException(e1) || uib_value(ctx, e0, "range low", &a) || uib_value(ctx, e1, "range high", &b)) {
        JS_FreeValue(ctx, e0);
        JS_FreeValue(ctx, e1);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, e0);
    JS_FreeValue(ctx, e1);
    if (uib_num(ctx, argv[2], "range min", -1e300, 1e300, &lo) || uib_num(ctx, argv[3], "range max", -1e300, 1e300, &hi)
        || uib_check_opts(ctx, opts, "range", keys, 2) || uib_opt_num(ctx, opts, "step", "range step", 0, 1e300, &step) || uib_decimals(ctx, opts, &dec))
        return JS_EXCEPTION;
    if (!(lo < hi))
        return JS_ThrowRangeError(ctx, "dyna:ui: range min must be below max");
    if (uib_str(ctx, argv[0], "range", &s, &n))
        return JS_EXCEPTION;
    u = uib_in_frame(ctx, this_val, "range");
    if (!u) {
        JS_FreeCString(ctx, s);
        return JS_EXCEPTION;
    }
    a0 = a;
    b0 = b;
    dyn_ui_w_range(u->ui, s, n, &a, &b, lo, hi, step, dec);
    JS_FreeCString(ctx, s);
    if (a == a0 && b == b0)
        return JS_DupValue(ctx, argv[1]);
    out = JS_NewArray(ctx);
    if (JS_IsException(out) || JS_DefinePropertyValueUint32(ctx, out, 0, JS_NewFloat64(ctx, a), JS_PROP_C_W_E) < 0
        || JS_DefinePropertyValueUint32(ctx, out, 1, JS_NewFloat64(ctx, b), JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, out);
        return JS_EXCEPTION;
    }
    return out;
}

static JSValue uib_number(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    static const char* const dkeys[] = { "speed", "min", "max", "decimals" };
    static const char* const skeys[] = { "step", "min", "max", "decimals" };
    const char* who = magic ? "spin" : "drag";
    JSValueConst opts = argc > 2 ? argv[2] : JS_UNDEFINED;
    uib* u;
    const char* s;
    size_t n;
    double v, r, speed = 1, step = 1, lo = 0, hi = -1;
    int dec = magic ? 0 : 2;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "dyna:ui: %s(label, value, opts?)", who);
    if (uib_value(ctx, argv[1], who, &v) || uib_check_opts(ctx, opts, who, magic ? skeys : dkeys, 4)
        || uib_opt_num(ctx, opts, magic ? "step" : "speed", magic ? "spin step" : "drag speed", 1e-300, 1e300, magic ? &step : &speed)
        || uib_opt_num(ctx, opts, "min", "min", -1e300, 1e300, &lo) || uib_opt_num(ctx, opts, "max", "max", -1e300, 1e300, &hi)
        || uib_decimals(ctx, opts, &dec))
        return JS_EXCEPTION;
    if (JS_IsObject(opts)) {
        JSValue a, b;
        int has_a, has_b;
        if (uib_get_opt(ctx, opts, "min", &a))
            return JS_EXCEPTION;
        if (uib_get_opt(ctx, opts, "max", &b)) {
            JS_FreeValue(ctx, a);
            return JS_EXCEPTION;
        }
        has_a = !JS_IsUndefined(a);
        has_b = !JS_IsUndefined(b);
        JS_FreeValue(ctx, a);
        JS_FreeValue(ctx, b);
        if (has_a != has_b)
            return JS_ThrowTypeError(ctx, "dyna:ui: %s takes min and max together", who);
        if (has_a && !(lo <= hi))
            return JS_ThrowRangeError(ctx, "dyna:ui: %s min must not exceed max", who);
    }
    if (uib_str(ctx, argv[0], who, &s, &n))
        return JS_EXCEPTION;
    u = uib_in_frame(ctx, this_val, who);
    if (!u) {
        JS_FreeCString(ctx, s);
        return JS_EXCEPTION;
    }
    r = magic ? dyn_ui_w_spin(u->ui, s, n, v, lo, hi, step, dec) : dyn_ui_w_drag(u->ui, s, n, v, speed, lo, hi, dec);
    JS_FreeCString(ctx, s);
    return r == v || (r != r && v != v) ? JS_DupValue(ctx, argv[1]) : JS_NewFloat64(ctx, r);
}

static JSValue uib_edit(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    static const char* const keys[] = { "hint", "maxLength", "width", "password", "readOnly", "rows", "error", "help" };
    static const char* const who[] = { "textField", "textArea", "search" };
    const char *err = NULL, *help = NULL;
    size_t en = 0, hn2 = 0;
    JSValueConst opts = argc > 2 ? argv[2] : JS_UNDEFINED;
    uib* u;
    const char *s = NULL, *t = NULL, *hint = NULL, *out;
    size_t n, tn, hn = 0, on;
    double maxlen = 0, width = 0, rows = 4;
    int pw = 0, ro = 0, flags, events = 0;
    uint32_t present;
    JSValue ret;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "dyna:ui: %s(label, text, opts?)", who[magic]);
    if (uib_check_opts_m(ctx, opts, who[magic], keys, (int)countof(keys), &present) || uib_opt_num(ctx, opts, "maxLength", "maxLength", 0, 16777216, &maxlen)
        || uib_opt_num(ctx, opts, "width", "width", 0, 16384, &width) || uib_opt_bool(ctx, opts, "password", &pw) || uib_opt_bool(ctx, opts, "readOnly", &ro)
        || uib_opt_num(ctx, opts, "rows", "rows", 1, 1000, &rows))
        return JS_EXCEPTION;
    if (uib_opt_str(ctx, opts, "hint", "hint", &hint, &hn))
        return JS_EXCEPTION;
    if (uib_str(ctx, argv[0], who[magic], &s, &n) || uib_str(ctx, argv[1], "text", &t, &tn)) {
        if (s)
            JS_FreeCString(ctx, s);
        if (hint)
            JS_FreeCString(ctx, hint);
        return JS_EXCEPTION;
    }
    u = uib_in_frame(ctx, this_val, who[magic]);
    if (!u) {
        JS_FreeCString(ctx, s);
        JS_FreeCString(ctx, t);
        if (hint)
            JS_FreeCString(ctx, hint);
        return JS_EXCEPTION;
    }
    flags = (pw ? DYN_UI_EDIT_PASSWORD : 0) | (ro ? DYN_UI_EDIT_READONLY : 0) | (magic == 1 ? DYN_UI_EDIT_MULTILINE : 0);
    if (uib_note_opts(ctx, present & 0xC0u ? opts : JS_UNDEFINED, &err, &en, &help, &hn2)) {
        JS_FreeCString(ctx, s);
        JS_FreeCString(ctx, t);
        if (hint)
            JS_FreeCString(ctx, hint);
        return JS_EXCEPTION;
    }
    uib_note_begin(u, en, hn2);
    if (magic == 2) {
        out = dyn_ui_w_search(u->ui, s, n, t, tn, hint ? hint : "Search", hint ? hn : 6, uib_px(u, width), &on, &events);
    } else {
        out = dyn_ui_w_edit(u->ui, s, n, t, tn, flags, (int)maxlen, hint, hn, (float)rows, uib_px(u, width), &on, &events);
    }
    uib_note_end(u, err, en, help, hn2);
    uib_note_free(ctx, err, help);
    u->events = events;
    if (out == t || (on == tn && !memcmp(out, t, tn)))
        ret = JS_DupValue(ctx, argv[1]);
    else
        ret = JS_NewStringLen(ctx, out, on);
    JS_FreeCString(ctx, s);
    JS_FreeCString(ctx, t);
    if (hint)
        JS_FreeCString(ctx, hint);
    return ret;
}

static const char* uib_item_fn(void* ud, int i, size_t* n)
{
    uib* u = ud;
    JSContext* ctx = u->cb_ctx;
    JSValue e;
    const char* s;
    if (u->cb_failed)
        return NULL;
    e = JS_GetPropertyUint32(ctx, u->cb_arr, (uint32_t)i);
    if (JS_IsException(e)) {
        u->cb_failed = 1;
        return NULL;
    }
    s = uib_keep(ctx, u, e, n);
    JS_FreeValue(ctx, e);
    if (!s)
        u->cb_failed = 1;
    return s;
}

static const char* uib_cell_fn(void* ud, int row, int col, size_t* n)
{
    uib* u = ud;
    JSContext* ctx = u->cb_ctx;
    JSValueConst a[2];
    JSValue r;
    const char* s;
    if (u->cb_failed)
        return NULL;
    a[0] = JS_NewInt32(ctx, row);
    a[1] = JS_NewInt32(ctx, col);
    r = JS_Call(ctx, u->cb_fn, JS_UNDEFINED, 2, a);
    if (JS_IsException(r)) {
        u->cb_failed = 1;
        return NULL;
    }
    if (!JS_IsString(r)) {
        JSValue str = JS_ToString(ctx, r);
        JS_FreeValue(ctx, r);
        if (JS_IsException(str)) {
            u->cb_failed = 1;
            return NULL;
        }
        r = str;
    }
    s = uib_keep(ctx, u, r, n);
    JS_FreeValue(ctx, r);
    if (!s)
        u->cb_failed = 1;
    return s;
}

static JSValue uib_list(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    static const char* const keys[] = { "rows", "follow" };
    JSValueConst opts = argc > (magic ? 2 : 3) ? argv[magic ? 2 : 3] : JS_UNDEFINED;
    const char* who = magic ? "log" : "list";
    uib* u;
    const char* s;
    size_t n;
    int64_t count;
    int sel = -1, follow = magic, r, act;
    double rows = 8;
    if (argc < 2 || !JS_IsArray(ctx, argv[1]))
        return JS_ThrowTypeError(ctx, "dyna:ui: %s(label, items%s, opts?) takes an array of strings", who, magic ? "" : ", selected");
    if ((!magic && (argc < 3 || uib_index(ctx, argv[2], "list selected", &sel))) || uib_check_opts(ctx, opts, who, keys, 2)
        || uib_opt_num(ctx, opts, "rows", "rows", 1, 1000, &rows) || uib_opt_bool(ctx, opts, "follow", &follow) || uib_length(ctx, argv[1], &count))
        return JS_EXCEPTION;
    if (count > 1000000)
        return JS_ThrowRangeError(ctx, "dyna:ui: %s holds at most 1000000 rows", who);
    if (uib_str(ctx, argv[0], who, &s, &n))
        return JS_EXCEPTION;
    u = uib_in_frame(ctx, this_val, who);
    if (!u) {
        JS_FreeCString(ctx, s);
        return JS_EXCEPTION;
    }
    u->cb_ctx = ctx;
    u->cb_arr = argv[1];
    u->cb_failed = 0;
    r = dyn_ui_w_list(u->ui, s, n, (int)count, uib_item_fn, u, sel, (int)rows, !magic, follow, &act);
    JS_FreeCString(ctx, s);
    u->cb_arr = JS_UNDEFINED;
    if (u->cb_failed)
        return JS_EXCEPTION;
    u->activated = act;
    return magic ? JS_UNDEFINED : JS_NewInt32(ctx, r);
}

static JSValue uib_color_w(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    uib* u;
    const char* s;
    size_t n;
    uint32_t c, r;
    if (argc < 2 || uib_color(ctx, argv[1], "color value", &c) || uib_str(ctx, argv[0], "color", &s, &n))
        return argc < 2 ? JS_ThrowTypeError(ctx, "dyna:ui: color(label, 0xRRGGBBAA)") : JS_EXCEPTION;
    u = uib_in_frame(ctx, this_val, "color");
    if (!u) {
        JS_FreeCString(ctx, s);
        return JS_EXCEPTION;
    }
    r = dyn_ui_w_color(u->ui, s, n, c);
    JS_FreeCString(ctx, s);
    return r == c ? JS_DupValue(ctx, argv[1]) : JS_NewUint32(ctx, r);
}

static JSValue uib_table(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    static const char* const keys[] = { "columns", "rows", "cell", "visibleRows", "selected", "sort", "selectedColumn", "edit" };
    static const char* const ckeys[] = { "name", "width" };
    static const char* const skeys[] = { "column", "descending" };
    JSValueConst opts = argc > 1 ? argv[1] : JS_UNDEFINED;
    uib* u;
    const char* s;
    size_t n;
    const char* heads[UIB_MAX_COLS];
    size_t hlens[UIB_MAX_COLS];
    float widths[UIB_MAX_COLS];
    double rows = 0, vis = 10, sel = -1, scol = -1, ccol = -2;
    int sdesc = 0, ncols = 0, i, r;
    uint32_t present;
    const char* edit = NULL;
    size_t edit_n = 0;
    int64_t nc;
    JSValue cols = JS_UNDEFINED, cell = JS_UNDEFINED, sort = JS_UNDEFINED;
    if (!JS_IsObject(opts))
        return JS_ThrowTypeError(ctx, "dyna:ui: table(label, { columns, rows, cell, ... })");
    if (uib_check_opts_m(ctx, opts, "table", keys, (int)countof(keys), &present) || uib_opt_num(ctx, opts, "rows", "table rows", 0, 1000000, &rows)
        || uib_opt_num(ctx, opts, "visibleRows", "table visibleRows", 1, 1000, &vis) || uib_opt_num(ctx, opts, "selected", "table selected", -1, 1000000, &sel)
        || ((present & 0x40u) && uib_opt_num(ctx, opts, "selectedColumn", "table selectedColumn", -1, UIB_MAX_COLS, &ccol)))
        return JS_EXCEPTION;
    if (rows != floor(rows) || sel != floor(sel) || vis != floor(vis) || ccol != floor(ccol))
        return JS_ThrowRangeError(ctx, "dyna:ui: table rows, visibleRows, selected and selectedColumn are integers");
    u = uib_in_frame(ctx, this_val, "table");
    if (!u)
        return JS_EXCEPTION;

    if (uib_get_opt(ctx, opts, "sort", &sort))
        return JS_EXCEPTION;
    if (!JS_IsUndefined(sort)) {
        int rc = uib_check_opts(ctx, sort, "table sort", skeys, 2) || uib_opt_num(ctx, sort, "column", "sort column", -1, UIB_MAX_COLS, &scol)
            || uib_opt_bool(ctx, sort, "descending", &sdesc);
        JS_FreeValue(ctx, sort);
        if (rc)
            return JS_EXCEPTION;
        if (scol != floor(scol))
            return JS_ThrowRangeError(ctx, "dyna:ui: sort column is an integer");
    }
    if (uib_get_opt(ctx, opts, "cell", &cell))
        return JS_EXCEPTION;
    if (!JS_IsFunction(ctx, cell)) {
        JS_FreeValue(ctx, cell);
        return JS_ThrowTypeError(ctx, "dyna:ui: table cell is a function (row, column) => string");
    }
    if (uib_get_opt(ctx, opts, "columns", &cols) || !JS_IsArray(ctx, cols) || uib_length(ctx, cols, &nc) || nc < 1 || nc > UIB_MAX_COLS) {
        JS_FreeValue(ctx, cell);
        JS_FreeValue(ctx, cols);
        if (!JS_HasException(ctx))
            JS_ThrowTypeError(ctx, "dyna:ui: table columns is an array of 1..%d { name, width? }", UIB_MAX_COLS);
        return JS_EXCEPTION;
    }
    for (i = 0; i < nc; i++) {
        JSValue c = JS_GetPropertyUint32(ctx, cols, (uint32_t)i), nm;
        double w = 0;
        int bad;
        if (JS_IsException(c))
            goto fail;
        bad = uib_check_opts(ctx, c, "table column", ckeys, 2) || !JS_IsObject(c) || uib_opt_num(ctx, c, "width", "column width", 0, 4000, &w);
        if (!bad) {
            nm = JS_GetPropertyStr(ctx, c, "name");
            heads[i] = JS_IsException(nm) ? NULL : uib_keep(ctx, u, nm, &hlens[i]);
            JS_FreeValue(ctx, nm);
            bad = !heads[i];
        }
        JS_FreeValue(ctx, c);
        if (bad) {
            if (!JS_HasException(ctx))
                JS_ThrowTypeError(ctx, "dyna:ui: a table column is { name: string, width?: number }");
            goto fail;
        }
        widths[i] = uib_px(u, w);
        ncols++;
    }
    JS_FreeValue(ctx, cols);
    cols = JS_UNDEFINED;
    if ((present & 0x80u) && uib_opt_str(ctx, opts, "edit", "table edit", &edit, &edit_n))
        goto fail;
    if (edit && ccol < -1) {
        JS_ThrowTypeError(ctx, "dyna:ui: table edit needs selectedColumn (it edits the selected cell)");
        goto fail;
    }
    if (uib_str(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, "table", &s, &n))
        goto fail;
    {
        dyn_ui_table_state st;
        memset(&st, 0, sizeof(st));
        st.selected = (int)sel;
        st.column = ccol < -1 ? -1 : (int)ccol;
        st.cells = ccol >= -1;
        st.sort_col = scol < ncols ? (int)scol : -1;
        st.sort_desc = sdesc;
        st.edit = edit;
        st.edit_n = edit_n;
        u->cb_ctx = ctx;
        u->cb_fn = cell;
        u->cb_failed = 0;
        r = dyn_ui_w_table_ex(u->ui, s, n, heads, hlens, widths, ncols, (int)rows, uib_cell_fn, u, (int)vis, &st);
        u->cb_fn = JS_UNDEFINED;
        JS_FreeCString(ctx, s);
        JS_FreeValue(ctx, cell);
        if (st.activated)
            u->activated = r;
        if (u->cb_failed) {
            if (edit)
                JS_FreeCString(ctx, edit);
            return JS_EXCEPTION;
        }
        {
            JSValue v[8];
            v[0] = JS_NewInt32(ctx, r);
            v[1] = JS_NewInt32(ctx, st.sort_col);
            v[2] = JS_NewBool(ctx, st.sort_desc);
            v[3] = JS_NewInt32(ctx, st.column);
            v[4] = JS_NewBool(ctx, st.activated);
            v[5] = st.edit_out ? JS_NewStringLen(ctx, st.edit_out, st.edit_out_n) : JS_UNDEFINED;
            v[6] = JS_NewBool(ctx, st.committed);
            v[7] = JS_NewBool(ctx, st.cancelled);
            if (edit)
                JS_FreeCString(ctx, edit);
            return uib_fill(ctx, u->table_tpl, 8, v);
        }
    }
fail:
    if (edit)
        JS_FreeCString(ctx, edit);
    JS_FreeValue(ctx, cell);
    JS_FreeValue(ctx, cols);
    return JS_EXCEPTION;
}

static JSValue uib_plot(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    static const char* const keys[] = { "min", "max", "height" };
    const char* who = magic ? "bars" : "plot";
    JSValueConst opts = argc > 2 ? argv[2] : JS_UNDEFINED;
    uib* u;
    const char* s;
    size_t n;
    double lo = 0, hi = 0, h = 80, *v = NULL;
    int64_t count, i;
    if (argc < 2 || uib_check_opts(ctx, opts, who, keys, 3) || uib_opt_num(ctx, opts, "min", "min", -1e300, 1e300, &lo)
        || uib_opt_num(ctx, opts, "max", "max", -1e300, 1e300, &hi) || uib_opt_num(ctx, opts, "height", "height", 8, 4096, &h))
        return argc < 2 ? JS_ThrowTypeError(ctx, "dyna:ui: %s(label, values, opts?)", who) : JS_EXCEPTION;
    if (uib_length(ctx, argv[1], &count))
        return JS_EXCEPTION;
    if (!JS_IsObject(argv[1]) || count > (magic ? 4096 : 1000000))
        return JS_ThrowRangeError(ctx, "dyna:ui: %s values is an array or typed array of at most %d numbers", who, magic ? 4096 : 1000000);
    v = dyn_nat_malloc((size_t)(count ? count : 1) * sizeof(double));
    if (!v)
        return JS_ThrowOutOfMemory(ctx);
    if (JS_GetClassID(argv[1]) == uib_f64_cid || JS_GetClassID(argv[1]) == uib_f32_cid) {
        size_t off, len, bpe, blen;
        JSValue buf = JS_GetTypedArrayBuffer(ctx, argv[1], &off, &len, &bpe);
        const uint8_t* base;
        if (JS_IsException(buf)) {
            dyn_nat_free(v);
            return JS_EXCEPTION;
        }
        base = JS_GetArrayBuffer(ctx, &blen, buf);
        JS_FreeValue(ctx, buf);
        if (!base || off > blen || len > blen - off || len / bpe < (size_t)count) {
            dyn_nat_free(v);
            return JS_ThrowTypeError(ctx, "dyna:ui: %s values: the typed array is detached or lies outside its buffer", who);
        }
        for (i = 0; i < count; i++) {
            if (bpe == 8)
                memcpy(&v[i], base + off + (size_t)i * 8, 8);
            else {
                float f;
                memcpy(&f, base + off + (size_t)i * 4, 4);
                v[i] = (double)f;
            }
        }
        count = -count - 1;
    }
    for (i = 0; i < count; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, argv[1], (uint32_t)i);
        int rc = JS_IsException(e) || uib_value(ctx, e, "a plotted value", &v[i]);
        JS_FreeValue(ctx, e);
        if (rc) {
            dyn_nat_free(v);
            return JS_EXCEPTION;
        }
    }
    if (count < 0)
        count = -count - 1;
    if (uib_str(ctx, argv[0], who, &s, &n)) {
        dyn_nat_free(v);
        return JS_EXCEPTION;
    }
    u = uib_in_frame(ctx, this_val, who);
    if (u)
        dyn_ui_w_plot(u->ui, s, n, v, (int)count, lo, hi, uib_px(u, h), magic);
    JS_FreeCString(ctx, s);
    dyn_nat_free(v);
    return u ? JS_UNDEFINED : JS_EXCEPTION;
}

enum { UIB_PROPERTY,
    UIB_GROUP,
    UIB_PANEL,
    UIB_SCROLL,
    UIB_MENUBAR,
    UIB_TOOLBAR,
    UIB_STATUSBAR };

static JSValue uib_wrap(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
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

static JSValue uib_window(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    static const char* const keys[] = { "x", "y", "width", "height", "closable", "collapsible", "resizable", "fixed" };
    JSValueConst opts = argc > 2 ? argv[1] : JS_UNDEFINED, fn = argc > 2 ? argv[2] : argc > 1 ? argv[1]
                                                                                              : JS_UNDEFINED;
    uib* u;
    const char* s;
    size_t n;
    double x = 40, y = 40, w = 300, h = 200;
    int cl = 0, co = 1, re = 1, fx = 0, flags, closed = 0, failed = 0;
    dyn_ui_window_frame fr;
    if (uib_fn(ctx, fn, "window") || uib_check_opts(ctx, opts, "window", keys, (int)countof(keys)) || uib_opt_num(ctx, opts, "x", "x", -16384, 16384, &x)
        || uib_opt_num(ctx, opts, "y", "y", -16384, 16384, &y) || uib_opt_num(ctx, opts, "width", "width", 80, 16384, &w)
        || uib_opt_num(ctx, opts, "height", "height", 20, 16384, &h) || uib_opt_bool(ctx, opts, "closable", &cl)
        || uib_opt_bool(ctx, opts, "collapsible", &co) || uib_opt_bool(ctx, opts, "resizable", &re) || uib_opt_bool(ctx, opts, "fixed", &fx))
        return JS_EXCEPTION;
    if (uib_str(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, "window", &s, &n))
        return JS_EXCEPTION;
    u = uib_in_frame(ctx, this_val, "window");
    if (!u) {
        JS_FreeCString(ctx, s);
        return JS_EXCEPTION;
    }
    flags = (cl ? DYN_UI_WIN_CLOSABLE : 0) | (co ? DYN_UI_WIN_COLLAPSIBLE : 0) | (re ? DYN_UI_WIN_RESIZABLE : 0) | (fx ? DYN_UI_WIN_FIXED : 0);
    if (dyn_ui_w_window_begin(u->ui, s, n, uib_px(u, x), uib_px(u, y), uib_px(u, w), uib_px(u, h), flags, &closed, &fr)) {
        dyn_ui_push_id(u->ui, fr.id);
        failed = uib_run(ctx, fn, this_val);
        dyn_ui_pop_id(u->ui);
    }
    dyn_ui_w_window_end(u->ui, &fr);
    JS_FreeCString(ctx, s);
    return failed ? JS_EXCEPTION : JS_NewBool(ctx, !closed);
}

static JSValue uib_collapsing(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    uib* u;
    const char* s;
    size_t n;
    int open = 0, r;
    if ((argc > 1 && uib_bool(ctx, argv[1], &open)) || uib_str(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, "collapsing", &s, &n))
        return JS_EXCEPTION;
    u = uib_in_frame(ctx, this_val, "collapsing");
    if (!u) {
        JS_FreeCString(ctx, s);
        return JS_EXCEPTION;
    }
    r = dyn_ui_w_collapsing(u->ui, s, n, open);
    JS_FreeCString(ctx, s);
    return JS_NewBool(ctx, r);
}

static JSValue uib_tree(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    static const char* const keys[] = { "open", "leaf" };
    JSValueConst opts = JS_UNDEFINED, fn = JS_UNDEFINED;
    uib* u;
    const char* s;
    size_t n;
    int open = 0, leaf = 0, r, failed = 0, k;
    for (k = 1; k < argc && k < 3; k++) {
        if (JS_IsFunction(ctx, argv[k]))
            fn = argv[k];
        else if (!JS_IsUndefined(argv[k]))
            opts = argv[k];
    }
    if (uib_check_opts(ctx, opts, "tree", keys, 2) || uib_opt_bool(ctx, opts, "open", &open) || uib_opt_bool(ctx, opts, "leaf", &leaf)
        || uib_str(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, "tree", &s, &n))
        return JS_EXCEPTION;
    u = uib_in_frame(ctx, this_val, "tree");
    if (!u) {
        JS_FreeCString(ctx, s);
        return JS_EXCEPTION;
    }
    r = dyn_ui_w_tree_begin(u->ui, s, n, open, leaf || JS_IsUndefined(fn));
    JS_FreeCString(ctx, s);
    if (r) {
        failed = uib_run(ctx, fn, this_val);
        dyn_ui_w_tree_end(u->ui);
    }
    return failed ? JS_EXCEPTION : JS_NewBool(ctx, r);
}

enum { UIB_OPEN_POPUP,
    UIB_OPEN_MODAL,
    UIB_CLOSE_POPUP };

static JSValue uib_popup_cmd(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    static const char* const who[] = { "openPopup", "openModal", "closePopup" };
    uib* u;
    const char* s = NULL;
    size_t n = 0;
    if (magic != UIB_CLOSE_POPUP && uib_str(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, who[magic], &s, &n))
        return JS_EXCEPTION;
    u = uib_in_frame(ctx, this_val, who[magic]);
    if (u) {
        if (magic == UIB_OPEN_POPUP)
            dyn_ui_w_open_popup(u->ui, s, n);
        else if (magic == UIB_OPEN_MODAL)
            dyn_ui_w_open_modal(u->ui, s, n);
        else
            dyn_ui_w_close_popup(u->ui);
    }
    if (s)
        JS_FreeCString(ctx, s);
    return u ? JS_UNDEFINED : JS_EXCEPTION;
}

enum { UIB_POPUP,
    UIB_CONTEXT,
    UIB_MODAL,
    UIB_MENU };

static JSValue uib_popup(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    static const char* const who[] = { "popup", "contextMenu", "modal", "menu" };
    static const char* const mkeys[] = { "width" };
    JSValueConst fn = argc > 1 ? argv[argc > 2 ? 2 : 1] : JS_UNDEFINED, opts = argc > 2 ? argv[1] : JS_UNDEFINED;
    uib* u;
    const char* s;
    size_t n;
    double w = 0;
    int depth = 0, open, failed = 0;
    dyn_ui_menu_frame mf;
    if (uib_fn(ctx, fn, who[magic]))
        return JS_EXCEPTION;
    if (magic == UIB_MODAL && (uib_check_opts(ctx, opts, "modal", mkeys, 1) || uib_opt_num(ctx, opts, "width", "modal width", 0, 16384, &w)))
        return JS_EXCEPTION;
    if (uib_str(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, who[magic], &s, &n))
        return JS_EXCEPTION;
    u = uib_in_frame(ctx, this_val, who[magic]);
    if (!u) {
        JS_FreeCString(ctx, s);
        return JS_EXCEPTION;
    }
    switch (magic) {
    case UIB_POPUP:
        open = dyn_ui_w_popup_begin(u->ui, s, n, &depth);
        break;
    case UIB_CONTEXT:
        open = dyn_ui_w_context_begin(u->ui, s, n, &depth);
        break;
    case UIB_MODAL:
        open = dyn_ui_w_modal_begin(u->ui, s, n, uib_px(u, w), &depth);
        break;
    default:
        open = dyn_ui_w_menu_begin(u->ui, s, n, &mf);
        break;
    }
    JS_FreeCString(ctx, s);
    if (!open)
        return JS_FALSE;
    failed = uib_run(ctx, fn, this_val);
    if (magic == UIB_MODAL)
        dyn_ui_w_modal_end(u->ui, depth);
    else if (magic == UIB_MENU)
        dyn_ui_w_menu_end(u->ui, &mf);
    else
        dyn_ui_w_popup_end(u->ui, depth);
    return failed ? JS_EXCEPTION : JS_TRUE;
}

static JSValue uib_menu_item(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    static const char* const keys[] = { "shortcut", "checked", "enabled" };
    JSValueConst opts = argc > 1 ? argv[1] : JS_UNDEFINED;
    uib* u;
    const char *s, *key = NULL;
    size_t n, kn = 0;
    int checked = -1, enabled = 1, r;
    JSValue cv;
    if (uib_check_opts(ctx, opts, "menuItem", keys, 3) || uib_opt_bool(ctx, opts, "enabled", &enabled) || uib_get_opt(ctx, opts, "checked", &cv))
        return JS_EXCEPTION;
    if (!JS_IsUndefined(cv)) {
        checked = JS_ToBool(ctx, cv);
        JS_FreeValue(ctx, cv);
    }
    if (uib_opt_str(ctx, opts, "shortcut", "shortcut", &key, &kn))
        return JS_EXCEPTION;
    if (uib_str(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, "menuItem", &s, &n)) {
        if (key)
            JS_FreeCString(ctx, key);
        return JS_EXCEPTION;
    }
    u = uib_in_frame(ctx, this_val, "menuItem");
    r = u ? dyn_ui_w_menu_item(u->ui, s, n, key, kn, checked, enabled) : 0;
    JS_FreeCString(ctx, s);
    if (key)
        JS_FreeCString(ctx, key);
    return u ? JS_NewBool(ctx, r) : JS_EXCEPTION;
}

static JSValue uib_split(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    static const char* const keys[] = { "vertical" };
    uib* u;
    const char* s;
    size_t n;
    double ratio;
    int vert = 0, failed;
    float r;
    dyn_ui_split_frame fr;
    JSValueConst opts = argc > 4 ? argv[2] : JS_UNDEFINED, fa = argv[argc > 4 ? 3 : 2], fb = argv[argc > 4 ? 4 : 3];
    if (argc < 4)
        return JS_ThrowTypeError(ctx, "dyna:ui: split(label, ratio, opts?, first, second)");
    if (uib_num(ctx, argv[1], "split ratio", 0, 1, &ratio) || uib_check_opts(ctx, opts, "split", keys, 1) || uib_opt_bool(ctx, opts, "vertical", &vert)
        || uib_fn(ctx, fa, "split first") || uib_fn(ctx, fb, "split second") || uib_str(ctx, argv[0], "split", &s, &n))
        return JS_EXCEPTION;
    u = uib_in_frame(ctx, this_val, "split");
    if (!u) {
        JS_FreeCString(ctx, s);
        return JS_EXCEPTION;
    }
    r = dyn_ui_w_split_begin(u->ui, s, n, (float)ratio, vert, &fr);
    JS_FreeCString(ctx, s);
    failed = uib_run(ctx, fa, this_val);
    dyn_ui_w_split_next(u->ui, &fr);
    if (!failed)
        failed = uib_run(ctx, fb, this_val);
    dyn_ui_w_split_end(u->ui);
    if (failed)
        return JS_EXCEPTION;
    return (float)ratio == r ? JS_DupValue(ctx, argv[1]) : JS_NewFloat64(ctx, (double)r);
}

static int uib_pointer_field(JSContext* ctx, JSValueConst o, const char* k, double lo, double hi, double* out)
{
    return uib_opt_num(ctx, o, k, k, lo, hi, out);
}

static JSValue uib_pointer(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
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

static const struct {
    const char* name;
    int key;
} uib_keys[] = {
    { "left", DYN_UI_KEY_LEFT },
    { "right", DYN_UI_KEY_RIGHT },
    { "up", DYN_UI_KEY_UP },
    { "down", DYN_UI_KEY_DOWN },
    { "home", DYN_UI_KEY_HOME },
    { "end", DYN_UI_KEY_END },
    { "backspace", DYN_UI_KEY_BACKSPACE },
    { "delete", DYN_UI_KEY_DELETE },
    { "return", DYN_UI_KEY_RETURN },
    { "tab", DYN_UI_KEY_TAB },
    { "escape", DYN_UI_KEY_ESCAPE },
    { "pageup", DYN_UI_KEY_PAGEUP },
    { "pagedown", DYN_UI_KEY_PAGEDOWN },
    { "a", DYN_UI_KEY_A },
    { "c", DYN_UI_KEY_C },
    { "v", DYN_UI_KEY_V },
    { "x", DYN_UI_KEY_X },
    { "y", DYN_UI_KEY_Y },
    { "z", DYN_UI_KEY_Z },
};

static int uib_mods(JSContext* ctx, JSValueConst v, int* out)
{
    static const struct {
        const char* name;
        int bit;
    } m[] = { { "shift", DYN_UI_MOD_SHIFT }, { "ctrl", DYN_UI_MOD_CTRL }, { "alt", DYN_UI_MOD_ALT }, { "super", DYN_UI_MOD_SUPER } };
    static const char* const keys[] = { "shift", "ctrl", "alt", "super" };
    size_t i;
    *out = 0;
    if (JS_IsUndefined(v))
        return 0;
    if (uib_check_opts(ctx, v, "key modifiers", keys, 4))
        return -1;
    for (i = 0; i < countof(m); i++) {
        int b = 0;
        if (uib_opt_bool(ctx, v, m[i].name, &b))
            return -1;
        if (b)
            *out |= m[i].bit;
    }
    return 0;
}

static JSValue uib_key(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    uib* u;
    const char* s;
    size_t n, i;
    int key = 0, down = 1, mods;
    if ((argc > 1 && uib_bool(ctx, argv[1], &down)) || uib_mods(ctx, argc > 2 ? argv[2] : JS_UNDEFINED, &mods)
        || uib_str(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, "key", &s, &n))
        return JS_EXCEPTION;
    for (i = 0; i < countof(uib_keys); i++)
        if (!strcmp(s, uib_keys[i].name))
            key = uib_keys[i].key;
    if (!key) {
        JSValue e = JS_ThrowRangeError(ctx, "dyna:ui: key '%s' is not one of left right up down home end backspace delete return tab escape pageup pagedown a c v x y z", s);
        JS_FreeCString(ctx, s);
        return e;
    }
    JS_FreeCString(ctx, s);
    u = uib_get(ctx, this_val);
    if (!u)
        return JS_EXCEPTION;
    dyn_ui_input_key(u->ui, key, down, mods);
    return JS_UNDEFINED;
}

static JSValue uib_type(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    uib* u;
    const char* s;
    size_t n;
    if (uib_str(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, "type", &s, &n))
        return JS_EXCEPTION;
    u = uib_get(ctx, this_val);
    if (u)
        dyn_ui_input_text(u->ui, s, n);
    JS_FreeCString(ctx, s);
    return u ? JS_UNDEFINED : JS_EXCEPTION;
}

static JSValue uib_set_time(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    uib* u;
    double t;
    if (uib_num(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, "setTime", 0, 1e12, &t))
        return JS_EXCEPTION;
    u = uib_get(ctx, this_val);
    if (!u)
        return JS_EXCEPTION;
    if (!u->manual_clock)
        return JS_ThrowTypeError(ctx, "dyna:ui: setTime() needs a Context made with { clock: \"manual\" }");
    dyn_ui_set_time(u->ui, t);
    return JS_UNDEFINED;
}

static JSValue uib_pixels(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    uib* u = uib_get(ctx, this_val);
    size_t bytes;
    (void)argc;
    (void)argv;
    if (!u)
        return JS_EXCEPTION;
    if (u->busy)
        return JS_ThrowTypeError(ctx, "dyna:ui: pixels() reads a finished frame; call it after frame() returns");
    if (!u->frames)
        return JS_ThrowTypeError(ctx, "dyna:ui: pixels() before the first frame()");
    bytes = (size_t)u->w * (size_t)u->h * 4;
    if (!u->px || !u->raster_ok) {
        if (!u->px) {
            u->px = dyn_nat_malloc(bytes);
            if (!u->px)
                return JS_ThrowOutOfMemory(ctx);
        }
        dyn_ui_raster(u->ui, u->px, u->w, u->h, (size_t)u->w * 4, u->clear);
        u->raster_ok = 1;
    }
    return dyn_nat_u8_copy(ctx, u->px, bytes);
}

static const struct {
    const char* name;
    size_t off;
    int color;
    double lo, hi;
} uib_theme_fields[] = {
#define UIB_TF(n, f, c, lo, hi) { n, offsetof(dyn_ui_theme, f), c, lo, hi }
    UIB_TF("fontSize", font_px, 0, 1, 512),
    UIB_TF("padX", pad_x, 0, 0, 256),
    UIB_TF("padY", pad_y, 0, 0, 256),
    UIB_TF("spacing", spacing, 0, 0, 256),
    UIB_TF("windowPad", window_pad, 0, 0, 256),
    UIB_TF("indent", indent, 0, 0, 256),
    UIB_TF("radius", radius, 0, 0, 256),
    UIB_TF("borderWidth", border_w, 0, 0, 64),
    UIB_TF("labelWidth", label_w, 0, 0, 4096),
    UIB_TF("scrollbarWidth", scrollbar_w, 0, 0, 64),
    UIB_TF("minTarget", min_target, 0, 0, 256),
    UIB_TF("dragSlop", drag_slop, 0, 0, 256),
    UIB_TF("longPress", long_press, 0, 0.05, 10),
    UIB_TF("textScale", text_scale, 0, 0.25, 8),
    UIB_TF("text", text, 1, 0, 0),
    UIB_TF("textDim", text_dim, 1, 0, 0),
    UIB_TF("onAccent", on_accent, 1, 0, 0),
    UIB_TF("bg", bg, 1, 0, 0),
    UIB_TF("panel", panel, 1, 0, 0),
    UIB_TF("frame", frame, 1, 0, 0),
    UIB_TF("frameHot", frame_hot, 1, 0, 0),
    UIB_TF("frameActive", frame_active, 1, 0, 0),
    UIB_TF("button", button, 1, 0, 0),
    UIB_TF("buttonHot", button_hot, 1, 0, 0),
    UIB_TF("buttonActive", button_active, 1, 0, 0),
    UIB_TF("accent", accent, 1, 0, 0),
    UIB_TF("accentHot", accent_hot, 1, 0, 0),
    UIB_TF("border", border, 1, 0, 0),
    UIB_TF("title", title, 1, 0, 0),
    UIB_TF("titleActive", title_active, 1, 0, 0),
    UIB_TF("selection", selection, 1, 0, 0),
    UIB_TF("popup", popup, 1, 0, 0),
    UIB_TF("shade", shade, 1, 0, 0),
    UIB_TF("tooltip", tooltip, 1, 0, 0),
    UIB_TF("link", link, 1, 0, 0),
    UIB_TF("scrollbar", scrollbar, 1, 0, 0),
    UIB_TF("scrollbarHot", scrollbar_hot, 1, 0, 0),
    UIB_TF("error", error, 1, 0, 0),
#undef UIB_TF
};

static JSValue uib_theme(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    uib* u;
    dyn_ui_theme next;
    size_t i;
    if (argc > 0 && !JS_IsUndefined(argv[0])) {
        const char* keys[countof(uib_theme_fields) + 1];
        double font = -2;
        for (i = 0; i < countof(uib_theme_fields); i++)
            keys[i] = uib_theme_fields[i].name;
        keys[i] = "font";
        if (uib_check_opts(ctx, argv[0], "theme", keys, (int)countof(keys)))
            return JS_EXCEPTION;
        u = uib_get(ctx, this_val);
        if (!u)
            return JS_EXCEPTION;
        next = *dyn_ui_theme_of(u->ui);
        for (i = 0; i < countof(uib_theme_fields); i++) {
            char* f = (char*)&next + uib_theme_fields[i].off;
            if (uib_theme_fields[i].color) {
                uint32_t c = *(uint32_t*)(void*)f;
                if (uib_opt_color(ctx, argv[0], uib_theme_fields[i].name, uib_theme_fields[i].name, &c))
                    return JS_EXCEPTION;
                memcpy(f, &c, sizeof(c));
            } else {
                double d = (double)*(float*)(void*)f;
                if (uib_opt_num(ctx, argv[0], uib_theme_fields[i].name, uib_theme_fields[i].name, uib_theme_fields[i].lo, uib_theme_fields[i].hi, &d))
                    return JS_EXCEPTION;
                *(float*)(void*)f = (float)d;
            }
        }
        if (uib_opt_num(ctx, argv[0], "font", "theme font", -1, 63, &font))
            return JS_EXCEPTION;
        u = uib_get(ctx, this_val);
        if (!u)
            return JS_EXCEPTION;
        if (u->busy)
            return JS_ThrowTypeError(ctx, "dyna:ui: theme() changes take effect between frames; call it outside frame()");
        if (font >= -1) {
            if (font != floor(font) || (int)font >= dyn_ui_font_count(u->ui))
                return JS_ThrowRangeError(ctx, "dyna:ui: theme font %g is not a font index from loadFont()", font);
            next.font = (int)font;
        }
        *dyn_ui_theme_of(u->ui) = next;
        return JS_UNDEFINED;
    }
    u = uib_get(ctx, this_val);
    if (!u)
        return JS_EXCEPTION;
    {
        JSValue o = JS_NewObject(ctx);
        const dyn_ui_theme* t = dyn_ui_theme_of(u->ui);
        if (JS_IsException(o))
            return o;
        for (i = 0; i < countof(uib_theme_fields); i++) {
            const char* f = (const char*)t + uib_theme_fields[i].off;
            JSValue v = uib_theme_fields[i].color ? JS_NewUint32(ctx, *(const uint32_t*)(const void*)f) : JS_NewFloat64(ctx, (double)*(const float*)(const void*)f);
            if (JS_DefinePropertyValueStr(ctx, o, uib_theme_fields[i].name, v, JS_PROP_C_W_E) < 0) {
                JS_FreeValue(ctx, o);
                return JS_EXCEPTION;
            }
        }
        if (JS_DefinePropertyValueStr(ctx, o, "font", JS_NewInt32(ctx, t->font), JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, o);
            return JS_EXCEPTION;
        }
        return o;
    }
}

static int uib_bytes_rw(JSContext* ctx, JSValueConst v, const char* who, uint8_t** p, size_t* n)
{
    size_t off, len, bpe, blen;
    JSValue buf;
    uint8_t* base;
    if (!uib_u8_cid || JS_GetClassID(v) != uib_u8_cid) {
        JS_ThrowTypeError(ctx, "dyna:ui: %s takes a Uint8Array", who);
        return -1;
    }
    buf = JS_GetTypedArrayBuffer(ctx, v, &off, &len, &bpe);
    if (JS_IsException(buf))
        return -1;
    base = JS_GetArrayBuffer(ctx, &blen, buf);
    JS_FreeValue(ctx, buf);
    if (!base) {
        JS_ThrowTypeError(ctx, "dyna:ui: %s: the Uint8Array's buffer is detached", who);
        return -1;
    }
    if (off > blen || len > blen - off) {
        JS_ThrowRangeError(ctx, "dyna:ui: %s: the view lies outside its buffer", who);
        return -1;
    }
    *p = base + off;
    *n = len;
    return 0;
}

static int uib_bytes(JSContext* ctx, JSValueConst v, const char* who, const uint8_t** p, size_t* n)
{
    uint8_t* q = NULL;
    int r = uib_bytes_rw(ctx, v, who, &q, n);
    *p = q;
    return r;
}

static JSValue uib_pixels_into(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    uib* u;
    uint8_t* dst = NULL;
    size_t n = 0, bytes;
    if (uib_bytes_rw(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, "pixelsInto", &dst, &n))
        return JS_EXCEPTION;
    u = uib_get(ctx, this_val);
    if (!u)
        return JS_EXCEPTION;
    if (u->busy)
        return JS_ThrowTypeError(ctx, "dyna:ui: pixelsInto() reads a finished frame; call it after frame() returns");
    if (!u->frames)
        return JS_ThrowTypeError(ctx, "dyna:ui: pixelsInto() before the first frame()");
    bytes = (size_t)u->w * (size_t)u->h * 4;
    if (n != bytes)
        return JS_ThrowRangeError(ctx, "dyna:ui: pixelsInto() takes a Uint8Array of pixelWidth * pixelHeight * 4 = %zu bytes, not %zu", bytes, n);
    if (u->px && u->raster_ok)
        memcpy(dst, u->px, bytes);
    else
        dyn_ui_raster(u->ui, dst, u->w, u->h, (size_t)u->w * 4, u->clear);
    return JS_DupValue(ctx, argv[0]);
}

static JSValue uib_load_font(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    uib* u;
    const uint8_t* p = NULL;
    size_t n = 0;
    uint8_t* owned = NULL;
    int idx;
    JSValueConst src = argc > 0 ? argv[0] : JS_UNDEFINED;
    if (JS_IsString(src)) {
        size_t pl;
        const char* path = JS_ToCStringLen(ctx, &pl, src);
        FILE* f;
        long sz;
        if (!path)
            return JS_EXCEPTION;
        if (strlen(path) != pl) {
            JS_FreeCString(ctx, path);
            return JS_ThrowTypeError(ctx, "dyna:ui: loadFont: a path holds no NUL character");
        }
        f = fopen(path, "rb");
        if (!f) {
            JSValue e = JS_ThrowTypeError(ctx, "dyna:ui: loadFont: cannot open %s", path);
            JS_FreeCString(ctx, path);
            return e;
        }
        JS_FreeCString(ctx, path);
        if (fseek(f, 0, SEEK_END) || (sz = ftell(f)) < 0 || sz > (64L << 20) || fseek(f, 0, SEEK_SET)) {
            fclose(f);
            return JS_ThrowRangeError(ctx, "dyna:ui: loadFont: the file is unreadable or over 64 MiB");
        }
        owned = dyn_nat_malloc(sz ? (size_t)sz : 1);
        if (!owned) {
            fclose(f);
            return JS_ThrowOutOfMemory(ctx);
        }
        if (fread(owned, 1, (size_t)sz, f) != (size_t)sz) {
            fclose(f);
            dyn_nat_free(owned);
            return JS_ThrowTypeError(ctx, "dyna:ui: loadFont: short read");
        }
        fclose(f);
        p = owned;
        n = (size_t)sz;
    } else if (uib_bytes(ctx, src, "loadFont", &p, &n))
        return JS_EXCEPTION;
    u = uib_get(ctx, this_val);
    if (!u) {
        dyn_nat_free(owned);
        return JS_EXCEPTION;
    }
    if (owned == NULL && uib_bytes(ctx, src, "loadFont", &p, &n))
        return JS_EXCEPTION;
    idx = dyn_ui_font_add(u->ui, p, n, 1);
    dyn_nat_free(owned);
    if (idx < 0)
        return JS_ThrowTypeError(ctx, "dyna:ui: loadFont: not a TrueType/OpenType font whose tables lie inside the file (or 64 fonts are loaded)");
    return JS_NewInt32(ctx, idx);
}

static JSValue uib_add_image(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    uib* u;
    const uint8_t* p;
    size_t n;
    double w = 0, h = 0, hd = 0;
    int r;
    if (magic == 0) {
        if (argc < 3 || uib_num(ctx, argv[1], "image width", 1, 16384, &w) || uib_num(ctx, argv[2], "image height", 1, 16384, &h))
            return argc < 3 ? JS_ThrowTypeError(ctx, "dyna:ui: addImage(rgba, width, height)") : JS_EXCEPTION;
        if (w != floor(w) || h != floor(h) || w * h > (double)(1 << 26))
            return JS_ThrowRangeError(ctx, "dyna:ui: image size is whole pixels, at most 2^26 of them");
    } else if (argc < 2 || uib_num(ctx, argv[0], "image handle", 1, 4095, &hd))
        return argc < 2 ? JS_ThrowTypeError(ctx, "dyna:ui: updateImage(handle, rgba)") : JS_EXCEPTION;
    if (uib_bytes(ctx, argv[magic ? 1 : 0], magic ? "updateImage" : "addImage", &p, &n))
        return JS_EXCEPTION;
    u = uib_get(ctx, this_val);
    if (!u)
        return JS_EXCEPTION;
    if (uib_bytes(ctx, argv[magic ? 1 : 0], magic ? "updateImage" : "addImage", &p, &n))
        return JS_EXCEPTION;
    if (magic == 0) {
        if (n != (size_t)w * (size_t)h * 4)
            return JS_ThrowRangeError(ctx, "dyna:ui: addImage needs width * height * 4 bytes of RGBA (%zu given, %zu needed)", n, (size_t)w * (size_t)h * 4);
        r = dyn_ui_image_add(u->ui, (int)w, (int)h, p);
        if (r < 0)
            return JS_ThrowRangeError(ctx, "dyna:ui: addImage: out of image slots (4095) or memory");
        return JS_NewInt32(ctx, r);
    }
    {
        int iw, ih;
        if (hd != floor(hd) || dyn_ui_image_size(u->ui, (uint32_t)hd, &iw, &ih))
            return JS_ThrowRangeError(ctx, "dyna:ui: %g is not an image handle from addImage()", hd);
        if (n != (size_t)iw * (size_t)ih * 4)
            return JS_ThrowRangeError(ctx, "dyna:ui: updateImage needs %zu bytes for this %dx%d image, %zu given", (size_t)iw * (size_t)ih * 4, iw, ih, n);
        dyn_ui_image_update(u->ui, (uint32_t)hd, p);
        u->raster_ok = 0;
    }
    return JS_UNDEFINED;
}

static JSValue uib_remove_image(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    uib* u;
    double hd;
    int iw, ih;
    if (uib_num(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, "image handle", 1, 4095, &hd))
        return JS_EXCEPTION;
    u = uib_get(ctx, this_val);
    if (!u)
        return JS_EXCEPTION;
    if (u->busy)
        return JS_ThrowTypeError(ctx, "dyna:ui: removeImage() inside frame() is refused; the frame may still draw it");
    if (hd != floor(hd) || dyn_ui_image_size(u->ui, (uint32_t)hd, &iw, &ih))
        return JS_ThrowRangeError(ctx, "dyna:ui: %g is not an image handle from addImage()", hd);
    dyn_ui_image_remove(u->ui, (uint32_t)hd);
    return JS_UNDEFINED;
}

static JSValue uib_rect_obj(JSContext* ctx, uib* u, const dyn_ui_rect* r)
{
    JSValue o;
    double s = (double)u->scale;
    if (u->rect_tpl) {
        JSValue v[4];
        v[0] = JS_NewFloat64(ctx, (double)r->x / s);
        v[1] = JS_NewFloat64(ctx, (double)r->y / s);
        v[2] = JS_NewFloat64(ctx, (double)r->w / s);
        v[3] = JS_NewFloat64(ctx, (double)r->h / s);
        return uib_fill(ctx, u->rect_tpl, 4, v);
    }
    o = JS_NewObject(ctx);
    if (JS_IsException(o))
        return o;
    if (JS_DefinePropertyValueStr(ctx, o, "x", JS_NewFloat64(ctx, (double)r->x / s), JS_PROP_C_W_E) < 0
        || JS_DefinePropertyValueStr(ctx, o, "y", JS_NewFloat64(ctx, (double)r->y / s), JS_PROP_C_W_E) < 0
        || JS_DefinePropertyValueStr(ctx, o, "width", JS_NewFloat64(ctx, (double)r->w / s), JS_PROP_C_W_E) < 0
        || JS_DefinePropertyValueStr(ctx, o, "height", JS_NewFloat64(ctx, (double)r->h / s), JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, o);
        return JS_EXCEPTION;
    }
    return o;
}

static JSValue uib_getter(JSContext* ctx, JSValueConst this_val, int magic)
{
    uib* u = uib_get(ctx, this_val);
    if (!u)
        return JS_EXCEPTION;
    switch (magic) {
    case 0:
        return JS_NewFloat64(ctx, (double)u->w / (double)u->scale);
    case 1:
        return JS_NewFloat64(ctx, (double)u->h / (double)u->scale);
    case 2: {
        double t = dyn_ui_next_wake(u->ui) - dyn_ui_time(u->ui);
        return JS_NewFloat64(ctx, t < 0 ? 0 : t);
    }
    case 3:
        return JS_NewBool(ctx, (u->events & DYN_UI_EDIT_SUBMITTED) != 0);
    case 4:
        return JS_NewInt32(ctx, u->activated);
    case 5:
        return JS_NewBool(ctx, (u->events & DYN_UI_EDIT_FOCUSED) != 0);
    case 6:
        return JS_NewInt32(ctx, u->w);
    case 7:
        return JS_NewInt32(ctx, u->h);
    case 8:
        return JS_NewFloat64(ctx, (double)u->scale);
    case 9: {
        double lw = (double)u->w / (double)u->scale;
        return JS_NewString(ctx, lw < 600 ? "compact" : lw < 840 ? "medium"
                                                                 : "expanded");
    }
    case 10: {
        dyn_ui_rect r;
        if (!dyn_ui_text_input(u->ui, &r))
            return JS_NULL;
        return uib_rect_obj(ctx, u, &r);
    }
    case 12:
        return dyn_ui_dragging(u->ui) && !JS_IsUndefined(u->dnd_payload) ? JS_DupValue(ctx, u->dnd_payload) : JS_NULL;
    default: {
        JSValue a = JS_NewArray(ctx);
        int k;
        for (k = 0; k < 4 && !JS_IsException(a); k++)
            if (JS_DefinePropertyValueUint32(ctx, a, (uint32_t)k, JS_NewFloat64(ctx, (double)u->insets_px[k] / (double)u->scale), JS_PROP_C_W_E) < 0) {
                JS_FreeValue(ctx, a);
                return JS_EXCEPTION;
            }
        return a;
    }
    }
}

static JSValue uib_last_rect(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    uib* u = uib_get(ctx, this_val);
    dyn_ui_rect r;
    double item = -2, col = -1;
    if (!u)
        return JS_EXCEPTION;
    if ((argc > 0 && !JS_IsUndefined(argv[0]) && uib_num(ctx, argv[0], "lastRect item", -1, 2147483647.0, &item))
        || (argc > 1 && !JS_IsUndefined(argv[1]) && uib_num(ctx, argv[1], "lastRect column", 0, UIB_MAX_COLS, &col)))
        return JS_EXCEPTION;
    if (item != floor(item) || col != floor(col))
        return JS_ThrowRangeError(ctx, "dyna:ui: lastRect item and column are integers");
    if (col >= 0 && item < -1)
        return JS_ThrowTypeError(ctx, "dyna:ui: lastRect(item, column) needs the item (row) too");
    if (item == -1 && col < 0)
        return JS_ThrowRangeError(ctx, "dyna:ui: lastRect item -1 is a table header and needs its column");
    if (item >= -1 ? !dyn_ui_last_item_box(u->ui, (int)item, (int)col, &r) : !dyn_ui_last_box(u->ui, &r))
        return JS_NULL;
    return uib_rect_obj(ctx, u, &r);
}

static JSValue uib_chip(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    uib* u;
    const char* s;
    size_t n;
    int on, r;
    if (argc < 2 || !JS_IsBool(argv[1]))
        return JS_ThrowTypeError(ctx, "dyna:ui: chip(label, selected) takes a boolean");
    on = JS_ToBool(ctx, argv[1]);
    if (uib_str(ctx, argv[0], "chip", &s, &n))
        return JS_EXCEPTION;
    u = uib_in_frame(ctx, this_val, "chip");
    r = u ? dyn_ui_w_chip(u->ui, s, n, on) : 0;
    JS_FreeCString(ctx, s);
    return u ? JS_NewBool(ctx, r) : JS_EXCEPTION;
}

static JSValue uib_card(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    static const char* const keys[] = { "width" };
    const char* who = magic ? "appBar" : "card";
    JSValueConst opts = argc > 2 ? argv[1] : JS_UNDEFINED, fn = argc > 2 ? argv[2] : argc > 1 ? argv[1]
                                                                                              : JS_UNDEFINED;
    uib* u;
    const char* s;
    size_t n;
    double w = 0;
    int failed;
    if (uib_fn(ctx, fn, who) || (!magic && (uib_check_opts(ctx, opts, "card", keys, 1) || uib_opt_num(ctx, opts, "width", "card width", 0, 16384, &w)))
        || uib_str(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, who, &s, &n))
        return JS_EXCEPTION;
    u = uib_in_frame(ctx, this_val, who);
    if (!u) {
        JS_FreeCString(ctx, s);
        return JS_EXCEPTION;
    }
    if (magic)
        dyn_ui_w_app_bar_begin(u->ui, s, n);
    else
        dyn_ui_w_card_begin(u->ui, s, n, uib_px(u, w));
    JS_FreeCString(ctx, s);
    failed = uib_run(ctx, fn, this_val);
    if (magic)
        dyn_ui_w_bar_end(u->ui);
    else
        dyn_ui_w_card_end(u->ui);
    return failed ? JS_EXCEPTION : JS_UNDEFINED;
}

static int uib_nav_items(JSContext* ctx, uib* u, JSValueConst arr, const char* who, const char*** pitems, size_t** plens, uint32_t** picons, int* pcount)
{
    const char** items;
    size_t* lens;
    uint32_t* icons;
    int64_t len;
    int i, count = 0;
    *pitems = NULL;
    *plens = NULL;
    *picons = NULL;
    *pcount = 0;
    if (!JS_IsArray(ctx, arr)) {
        JS_ThrowTypeError(ctx, "dyna:ui: %s items are strings or { label, icon }", who);
        return -1;
    }
    if (uib_length(ctx, arr, &len))
        return -1;
    if (len > 64) {
        JS_ThrowRangeError(ctx, "dyna:ui: %s holds at most 64 destinations", who);
        return -1;
    }
    items = dyn_nat_calloc((size_t)(len ? len : 1), sizeof(char*));
    lens = dyn_nat_calloc((size_t)(len ? len : 1), sizeof(size_t));
    icons = dyn_nat_calloc((size_t)(len ? len : 1), sizeof(uint32_t));
    if (!items || !lens || !icons) {
        JS_ThrowOutOfMemory(ctx);
        goto fail;
    }
    for (i = 0; i < len; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, arr, (uint32_t)i), lab;
        if (JS_IsException(e))
            goto fail;
        if (JS_IsString(e)) {
            items[i] = uib_keep(ctx, u, e, &lens[i]);
        } else if (JS_IsObject(e)) {
            static const char* const keys[] = { "label", "icon" };
            JSValue ic;
            if (uib_check_opts(ctx, e, who, keys, 2)) {
                JS_FreeValue(ctx, e);
                goto fail;
            }
            lab = JS_GetPropertyStr(ctx, e, "label");
            items[i] = JS_IsException(lab) ? NULL : uib_keep(ctx, u, lab, &lens[i]);
            JS_FreeValue(ctx, lab);
            ic = JS_GetPropertyStr(ctx, e, "icon");
            if (items[i] && !JS_IsUndefined(ic) && uib_handle(ctx, u, ic, &icons[i]))
                items[i] = NULL;
            JS_FreeValue(ctx, ic);
        } else
            JS_ThrowTypeError(ctx, "dyna:ui: %s items are strings or { label, icon }", who);
        JS_FreeValue(ctx, e);
        if (!items[i])
            goto fail;
        count++;
    }
    *pitems = items;
    *plens = lens;
    *picons = icons;
    *pcount = count;
    return 0;
fail:
    dyn_nat_free(items);
    dyn_nat_free(lens);
    dyn_nat_free(icons);
    return -1;
}

static JSValue uib_nav(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    const char* who = magic ? "navRail" : "navBar";
    uib* u;
    const char* s;
    size_t n, *lens = NULL;
    const char** items = NULL;
    uint32_t* icons = NULL;
    int sel, r, count = 0;
    if (argc < 3 || !JS_IsArray(ctx, argv[1]) || uib_index(ctx, argv[2], who, &sel))
        return JS_HasException(ctx) ? JS_EXCEPTION : JS_ThrowTypeError(ctx, "dyna:ui: %s(label, items, selected); items are strings or { label, icon }", who);
    if (uib_str(ctx, argv[0], who, &s, &n))
        return JS_EXCEPTION;
    u = uib_in_frame(ctx, this_val, who);
    if (!u || uib_nav_items(ctx, u, argv[1], who, &items, &lens, &icons, &count)) {
        JS_FreeCString(ctx, s);
        return JS_EXCEPTION;
    }
    r = dyn_ui_w_nav(u->ui, s, n, items, lens, icons, count, sel < count ? sel : -1, magic);
    dyn_nat_free(items);
    dyn_nat_free(lens);
    dyn_nat_free(icons);
    JS_FreeCString(ctx, s);
    return JS_NewInt32(ctx, r);
}

static JSValue uib_scaffold(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    uib* u;
    const char* s;
    size_t n, *lens = NULL;
    const char** items = NULL;
    uint32_t* icons = NULL;
    int sel, r, count = 0, failed;
    if (argc < 4 || uib_index(ctx, argv[2], "scaffold", &sel) || uib_fn(ctx, argv[3], "scaffold"))
        return JS_HasException(ctx) ? JS_EXCEPTION : JS_ThrowTypeError(ctx, "dyna:ui: scaffold(label, items, selected, fn)");
    if (uib_str(ctx, argv[0], "scaffold", &s, &n))
        return JS_EXCEPTION;
    u = uib_in_frame(ctx, this_val, "scaffold");
    if (!u || uib_nav_items(ctx, u, argv[1], "scaffold", &items, &lens, &icons, &count)) {
        JS_FreeCString(ctx, s);
        return JS_EXCEPTION;
    }
    if (sel >= count)
        sel = -1;
    dyn_ui_w_scaffold_begin(u->ui, s, n, items, lens, icons, count, sel);
    failed = uib_run(ctx, argv[3], this_val);
    r = dyn_ui_w_scaffold_end(u->ui, s, n, items, lens, icons, count, sel);
    dyn_nat_free(items);
    dyn_nat_free(lens);
    dyn_nat_free(icons);
    JS_FreeCString(ctx, s);
    return failed ? JS_EXCEPTION : JS_NewInt32(ctx, r);
}

enum { UIB_OPEN_DRAWER,
    UIB_OPEN_SHEET };

static JSValue uib_open_overlay(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    static const char* const who[] = { "openDrawer", "openSheet" };
    uib* u;
    const char* s;
    size_t n;
    if (uib_str(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, who[magic], &s, &n))
        return JS_EXCEPTION;
    u = uib_in_frame(ctx, this_val, who[magic]);
    if (u)
        dyn_ui_w_open_overlay(u->ui, s, n);
    JS_FreeCString(ctx, s);
    return u ? JS_UNDEFINED : JS_EXCEPTION;
}

static JSValue uib_overlay(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    static const char* const keys[] = { "side", "width" };
    const char* who = magic ? "sheet" : "drawer";
    JSValueConst opts = argc > 2 ? argv[1] : JS_UNDEFINED, fn = argc > 2 ? argv[2] : argc > 1 ? argv[1]
                                                                                              : JS_UNDEFINED;
    uib* u;
    const char *s, *side = NULL;
    size_t n, sn = 0;
    double w = 0;
    int right = 0, depth = 0, open, failed = 0;
    if (uib_fn(ctx, fn, who))
        return JS_EXCEPTION;
    if (!magic) {
        if (uib_check_opts(ctx, opts, "drawer", keys, 2) || uib_opt_num(ctx, opts, "width", "drawer width", 0, 16384, &w)
            || uib_opt_str(ctx, opts, "side", "drawer side", &side, &sn))
            return JS_EXCEPTION;
        if (side) {
            int ok = !strcmp(side, "left") || !strcmp(side, "right");
            right = !strcmp(side, "right");
            JS_FreeCString(ctx, side);
            if (!ok)
                return JS_ThrowTypeError(ctx, "dyna:ui: drawer side is \"left\" or \"right\"");
        }
    } else if (!JS_IsUndefined(opts) && uib_check_opts(ctx, opts, "sheet", keys, 0))
        return JS_EXCEPTION;
    if (uib_str(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, who, &s, &n))
        return JS_EXCEPTION;
    u = uib_in_frame(ctx, this_val, who);
    if (!u) {
        JS_FreeCString(ctx, s);
        return JS_EXCEPTION;
    }
    open = magic ? dyn_ui_w_sheet_begin(u->ui, s, n, &depth) : dyn_ui_w_drawer_begin(u->ui, s, n, right, uib_px(u, w), &depth);
    JS_FreeCString(ctx, s);
    if (!open)
        return JS_FALSE;
    failed = uib_run(ctx, fn, this_val);
    dyn_ui_w_overlay_end(u->ui, depth);
    return failed ? JS_EXCEPTION : JS_TRUE;
}

static JSValue uib_toast(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    static const char* const keys[] = { "duration" };
    JSValueConst opts = argc > 1 ? argv[1] : JS_UNDEFINED;
    uib* u;
    const char* s;
    size_t n;
    double d = 3;
    if (uib_check_opts(ctx, opts, "toast", keys, 1) || uib_opt_num(ctx, opts, "duration", "toast duration", 0.1, 3600, &d)
        || uib_str(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, "toast", &s, &n))
        return JS_EXCEPTION;
    u = uib_get(ctx, this_val);
    if (u && !u->manual_clock && !u->busy)
        dyn_ui_set_time(u->ui, uib_now());
    if (u && dyn_ui_w_toast(u->ui, s, n, d)) {
        JS_FreeCString(ctx, s);
        return JS_ThrowOutOfMemory(ctx);
    }
    JS_FreeCString(ctx, s);
    return u ? JS_UNDEFINED : JS_EXCEPTION;
}

static JSValue uib_fab(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    static const char* const keys[] = { "icon" };
    JSValueConst opts = argc > 1 ? argv[1] : JS_UNDEFINED;
    uib* u;
    const char* s;
    size_t n;
    uint32_t icon = 0;
    JSValue iv = JS_UNDEFINED;
    int r;
    if (uib_check_opts(ctx, opts, "fab", keys, 1) || uib_get_opt(ctx, opts, "icon", &iv) || uib_str(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, "fab", &s, &n)) {
        JS_FreeValue(ctx, iv);
        return JS_EXCEPTION;
    }
    u = uib_in_frame(ctx, this_val, "fab");
    if (!u || (!JS_IsUndefined(iv) && uib_handle(ctx, u, iv, &icon))) {
        JS_FreeValue(ctx, iv);
        JS_FreeCString(ctx, s);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, iv);
    r = dyn_ui_w_fab(u->ui, s, n, icon);
    JS_FreeCString(ctx, s);
    return JS_NewBool(ctx, r);
}

enum { UIB_BREADCRUMBS,
    UIB_STEPPER };

static JSValue uib_seq(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    const char* who = magic ? "stepper" : "breadcrumbs";
    uib* u;
    const char* s;
    size_t n, *lens = NULL;
    const char** items = NULL;
    int count, cur = 0, r;
    if (argc < 2 || (magic && (argc < 3 || uib_index(ctx, argv[2], "stepper current", &cur))))
        return JS_HasException(ctx) ? JS_EXCEPTION : JS_ThrowTypeError(ctx, "dyna:ui: %s(label, items%s)", who, magic ? ", current" : "");
    if (uib_str(ctx, argv[0], who, &s, &n))
        return JS_EXCEPTION;
    u = uib_in_frame(ctx, this_val, who);
    if (!u || uib_items(ctx, u, argv[1], who, &items, &lens, &count)) {
        JS_FreeCString(ctx, s);
        return JS_EXCEPTION;
    }
    r = magic ? dyn_ui_w_stepper(u->ui, s, n, items, lens, count, cur) : dyn_ui_w_breadcrumbs(u->ui, s, n, items, lens, count);
    dyn_nat_free(items);
    dyn_nat_free(lens);
    JS_FreeCString(ctx, s);
    return JS_NewInt32(ctx, r);
}

static JSValue uib_pagination(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    uib* u;
    const char* s;
    size_t n;
    double page, pages;
    int r;
    if (argc < 3 || uib_num(ctx, argv[1], "pagination page", 0, 1e9, &page) || uib_num(ctx, argv[2], "pagination pages", 1, 1e9, &pages))
        return JS_HasException(ctx) ? JS_EXCEPTION : JS_ThrowTypeError(ctx, "dyna:ui: pagination(label, page, pages)");
    if (page != floor(page) || pages != floor(pages))
        return JS_ThrowRangeError(ctx, "dyna:ui: pagination page and pages are integers");
    if (uib_str(ctx, argv[0], "pagination", &s, &n))
        return JS_EXCEPTION;
    u = uib_in_frame(ctx, this_val, "pagination");
    r = u ? dyn_ui_w_pagination(u->ui, s, n, (int)page, (int)pages) : 0;
    JS_FreeCString(ctx, s);
    return u ? JS_NewInt32(ctx, r) : JS_EXCEPTION;
}

static JSValue uib_rating(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    static const char* const keys[] = { "max" };
    JSValueConst opts = argc > 2 ? argv[2] : JS_UNDEFINED;
    uib* u;
    const char* s;
    size_t n;
    double v, max = 5;
    int r;
    if (argc < 2 || uib_num(ctx, argv[1], "rating value", 0, 20, &v) || uib_check_opts(ctx, opts, "rating", keys, 1)
        || uib_opt_num(ctx, opts, "max", "rating max", 1, 20, &max))
        return JS_HasException(ctx) ? JS_EXCEPTION : JS_ThrowTypeError(ctx, "dyna:ui: rating(label, value, opts?)");
    if (v != floor(v) || max != floor(max))
        return JS_ThrowRangeError(ctx, "dyna:ui: rating value and max are integers");
    if (uib_str(ctx, argv[0], "rating", &s, &n))
        return JS_EXCEPTION;
    u = uib_in_frame(ctx, this_val, "rating");
    r = u ? dyn_ui_w_rating(u->ui, s, n, (int)v, (int)max) : 0;
    JS_FreeCString(ctx, s);
    return !u ? JS_EXCEPTION : r == (int)v ? JS_DupValue(ctx, argv[1])
                                           : JS_NewInt32(ctx, r);
}

static int uib_date_field(JSContext* ctx, JSValueConst o, const char* k, double lo, double hi, int* out)
{
    JSValue v = JS_GetPropertyStr(ctx, o, k);
    double d;
    int rc;
    if (JS_IsException(v))
        return -1;
    rc = uib_num(ctx, v, k, lo, hi, &d);
    JS_FreeValue(ctx, v);
    if (rc)
        return -1;
    if (d != floor(d)) {
        JS_ThrowRangeError(ctx, "dyna:ui: date %s is an integer", k);
        return -1;
    }
    *out = (int)d;
    return 0;
}

static JSValue uib_date(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    static const char* const keys[] = { "firstDay" };
    static const char* const dkeys[] = { "year", "month", "day" };
    JSValueConst opts = argc > 2 ? argv[2] : JS_UNDEFINED;
    uib* u;
    const char* s;
    size_t n;
    int y, m, d, first = 1, changed;
    double fd = 1;
    JSValue o;
    if (argc < 2 || !JS_IsObject(argv[1]) || uib_check_opts(ctx, argv[1], "datePicker date", dkeys, 3) || uib_date_field(ctx, argv[1], "year", 1, 9999, &y)
        || uib_date_field(ctx, argv[1], "month", 1, 12, &m) || uib_date_field(ctx, argv[1], "day", 1, 31, &d) || uib_check_opts(ctx, opts, "datePicker", keys, 1)
        || uib_opt_num(ctx, opts, "firstDay", "datePicker firstDay", 0, 6, &fd))
        return JS_HasException(ctx) ? JS_EXCEPTION : JS_ThrowTypeError(ctx, "dyna:ui: datePicker(label, { year, month, day }, opts?)");
    first = (int)fd;
    if (uib_str(ctx, argv[0], "datePicker", &s, &n))
        return JS_EXCEPTION;
    u = uib_in_frame(ctx, this_val, "datePicker");
    if (!u) {
        JS_FreeCString(ctx, s);
        return JS_EXCEPTION;
    }
    changed = dyn_ui_w_date(u->ui, s, n, &y, &m, &d, first);
    JS_FreeCString(ctx, s);
    if (!changed)
        return JS_DupValue(ctx, argv[1]);
    o = JS_NewObject(ctx);
    if (JS_IsException(o) || JS_DefinePropertyValueStr(ctx, o, "year", JS_NewInt32(ctx, y), JS_PROP_C_W_E) < 0
        || JS_DefinePropertyValueStr(ctx, o, "month", JS_NewInt32(ctx, m), JS_PROP_C_W_E) < 0
        || JS_DefinePropertyValueStr(ctx, o, "day", JS_NewInt32(ctx, d), JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, o);
        return JS_EXCEPTION;
    }
    return o;
}

static JSValue uib_avatar(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    static const char* const keys[] = { "image", "size" };
    JSValueConst opts = argc > 1 ? argv[1] : JS_UNDEFINED;
    uib* u;
    const char* s;
    size_t n;
    double z = 0;
    uint32_t img = 0;
    JSValue iv = JS_UNDEFINED;
    if (uib_check_opts(ctx, opts, "avatar", keys, 2) || uib_opt_num(ctx, opts, "size", "avatar size", 4, 1024, &z) || uib_get_opt(ctx, opts, "image", &iv)
        || uib_str(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, "avatar", &s, &n)) {
        JS_FreeValue(ctx, iv);
        return JS_EXCEPTION;
    }
    u = uib_in_frame(ctx, this_val, "avatar");
    if (!u || (!JS_IsUndefined(iv) && uib_handle(ctx, u, iv, &img))) {
        JS_FreeValue(ctx, iv);
        JS_FreeCString(ctx, s);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, iv);
    dyn_ui_w_avatar(u->ui, s, n, img, uib_px(u, z));
    JS_FreeCString(ctx, s);
    return JS_UNDEFINED;
}

static JSValue uib_skeleton(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    uib* u;
    double w = 0, h = 0;
    if ((argc > 0 && !JS_IsUndefined(argv[0]) && uib_num(ctx, argv[0], "skeleton width", 0, 16384, &w))
        || (argc > 1 && !JS_IsUndefined(argv[1]) && uib_num(ctx, argv[1], "skeleton height", 0, 16384, &h)))
        return JS_EXCEPTION;
    u = uib_in_frame(ctx, this_val, "skeleton");
    if (!u)
        return JS_EXCEPTION;
    dyn_ui_w_skeleton(u->ui, uib_px(u, w), uib_px(u, h));
    return JS_UNDEFINED;
}

static JSValue uib_composition(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    uib* u;
    const char* s;
    size_t n;
    double cur = 0;
    if ((argc > 1 && uib_num(ctx, argv[1], "composition cursor", 0, 4096, &cur)) || uib_str(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, "composition", &s, &n))
        return JS_EXCEPTION;
    u = uib_get(ctx, this_val);
    if (u)
        dyn_ui_input_composition(u->ui, s, n, (int)cur);
    JS_FreeCString(ctx, s);
    return u ? JS_UNDEFINED : JS_EXCEPTION;
}

static JSValue uib_resize(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    static const char* const keys[] = { "scale", "insets" };
    uib* u;
    double w, h, scale = -1;
    float ins[4] = { 0, 0, 0, 0 };
    JSValueConst opts = argc > 2 ? argv[2] : JS_UNDEFINED;
    JSValue iv;
    int has_ins, k;
    if (argc < 2 || uib_num(ctx, argv[0], "width", 1, 16384, &w) || uib_num(ctx, argv[1], "height", 1, 16384, &h) || uib_check_opts(ctx, opts, "resize", keys, 2)
        || uib_opt_num(ctx, opts, "scale", "scale", 0.25, 16, &scale) || uib_insets_opt(ctx, opts, ins) || uib_get_opt(ctx, opts, "insets", &iv))
        return JS_HasException(ctx) ? JS_EXCEPTION : JS_ThrowTypeError(ctx, "dyna:ui: resize(width, height, { scale?, insets? })");
    has_ins = !JS_IsUndefined(iv);
    JS_FreeValue(ctx, iv);
    u = uib_get(ctx, this_val);
    if (!u)
        return JS_EXCEPTION;
    if (u->ops)
        return JS_ThrowTypeError(ctx, "dyna:ui: resize() is for a Context without a target; a window's size comes from the window");
    if (u->busy)
        return JS_ThrowTypeError(ctx, "dyna:ui: resize() inside frame() is refused");
    if (scale < 0)
        scale = (double)u->scale;
    if (lround(w * scale) < 1 || lround(h * scale) < 1 || lround(w * scale) > 16384 || lround(h * scale) > 16384)
        return JS_ThrowRangeError(ctx, "dyna:ui: width * scale and height * scale must be 1..16384 pixels");
    u->lw = (float)w;
    u->lh = (float)h;
    u->scale = (float)scale;
    u->w = (int)lround(w * scale);
    u->h = (int)lround(h * scale);
    if (has_ins)
        memcpy(u->soft_insets, ins, sizeof(ins));
    for (k = 0; k < 4; k++)
        u->insets_px[k] = u->soft_insets[k] * u->scale;
    dyn_nat_free(u->px);
    u->px = NULL;
    u->raster_ok = 0;
    return JS_UNDEFINED;
}

static JSValue uib_idle(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    uib* u;
    double t = 1e9, wake;
    if (argc > 0 && !JS_IsUndefined(argv[0]) && uib_num(ctx, argv[0], "idle timeout", 0, 1e9, &t))
        return JS_EXCEPTION;
    u = uib_get(ctx, this_val);
    if (!u)
        return JS_EXCEPTION;
    if (u->busy)
        return JS_ThrowTypeError(ctx, "dyna:ui: idle() inside frame() is refused");
    wake = dyn_ui_next_wake(u->ui) - (u->manual_clock ? dyn_ui_time(u->ui) : uib_now());
    if (wake < t)
        t = wake < 0 ? 0 : wake;
    if (u->ops)
        u->ops->idle(u->be, t);
    return JS_NewFloat64(ctx, t);
}

static JSValue uib_geometry(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    uib* u = uib_get(ctx, this_val);
    const dyn_ui_gvertex* v;
    const uint32_t* idx;
    const dyn_ui_batch* b;
    size_t nv, ni, nb, i;
    JSValue o, arr, fv, iv;
    float* f;
    (void)argc;
    (void)argv;
    if (!u)
        return JS_EXCEPTION;
    if (u->busy || !u->frames)
        return JS_ThrowTypeError(ctx, "dyna:ui: geometry() reads a finished frame");
    if (dyn_ui_tessellate(u->ui, &v, &nv, &idx, &ni, &b, &nb))
        return JS_ThrowOutOfMemory(ctx);
    f = dyn_nat_malloc((nv ? nv : 1) * 8 * sizeof(float));
    if (!f)
        return JS_ThrowOutOfMemory(ctx);
    for (i = 0; i < nv; i++) {
        f[i * 8] = v[i].x;
        f[i * 8 + 1] = v[i].y;
        f[i * 8 + 2] = (float)(v[i].color >> 24) / 255.0f;
        f[i * 8 + 3] = (float)((v[i].color >> 16) & 0xFFu) / 255.0f;
        f[i * 8 + 4] = (float)((v[i].color >> 8) & 0xFFu) / 255.0f;
        f[i * 8 + 5] = (float)(v[i].color & 0xFFu) / 255.0f;
        f[i * 8 + 6] = v[i].u;
        f[i * 8 + 7] = v[i].v;
    }
    {
        JSValue ab = JS_NewArrayBufferCopy(ctx, (const uint8_t*)f, nv * 8 * sizeof(float)), args[3];
        dyn_nat_free(f);
        if (JS_IsException(ab))
            return ab;
        args[0] = ab;
        args[1] = JS_NewInt32(ctx, 0);
        args[2] = JS_NewInt64(ctx, (int64_t)nv * 8);
        fv = JS_NewTypedArray(ctx, 3, args, JS_TYPED_ARRAY_FLOAT32);
        JS_FreeValue(ctx, ab);
        if (JS_IsException(fv))
            return fv;
        ab = JS_NewArrayBufferCopy(ctx, (const uint8_t*)idx, ni * sizeof(uint32_t));
        if (JS_IsException(ab)) {
            JS_FreeValue(ctx, fv);
            return ab;
        }
        args[0] = ab;
        args[2] = JS_NewInt64(ctx, (int64_t)ni);
        iv = JS_NewTypedArray(ctx, 3, args, JS_TYPED_ARRAY_UINT32);
        JS_FreeValue(ctx, ab);
        if (JS_IsException(iv)) {
            JS_FreeValue(ctx, fv);
            return iv;
        }
    }
    o = JS_NewObject(ctx);
    arr = JS_NewArray(ctx);
    if (JS_IsException(o) || JS_IsException(arr) || JS_DefinePropertyValueStr(ctx, o, "vertices", fv, JS_PROP_C_W_E) < 0
        || JS_DefinePropertyValueStr(ctx, o, "indices", iv, JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, o);
        JS_FreeValue(ctx, arr);
        return JS_EXCEPTION;
    }
    for (i = 0; i < nb; i++) {
        JSValue bo = JS_NewObject(ctx);
        dyn_ui_rect r = b[i].clip;
        if (JS_IsException(bo)
            || JS_DefinePropertyValueStr(ctx, bo, "texture", b[i].texture == DYN_UI_TEX_ATLAS ? JS_NewString(ctx, "atlas") : JS_NewUint32(ctx, b[i].texture),
                   JS_PROP_C_W_E)
                < 0
            || JS_DefinePropertyValueStr(ctx, bo, "first", JS_NewUint32(ctx, b[i].first), JS_PROP_C_W_E) < 0
            || JS_DefinePropertyValueStr(ctx, bo, "count", JS_NewUint32(ctx, b[i].count), JS_PROP_C_W_E) < 0
            || JS_DefinePropertyValueStr(ctx, bo, "clip", uib_rect_obj(ctx, &(uib) { .scale = 1 }, &r), JS_PROP_C_W_E) < 0
            || JS_DefinePropertyValueUint32(ctx, arr, (uint32_t)i, bo, JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, o);
            JS_FreeValue(ctx, arr);
            return JS_EXCEPTION;
        }
    }
    if (JS_DefinePropertyValueStr(ctx, o, "batches", arr, JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, o);
        return JS_EXCEPTION;
    }
    return o;
}

static JSValue uib_atlas_pixels(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    uib* u = uib_get(ctx, this_val);
    dyn_ui_atlas_view a;
    JSValue o, px;
    (void)argc;
    (void)argv;
    if (!u)
        return JS_EXCEPTION;
    a = dyn_ui_atlas(u->ui);
    px = dyn_nat_u8_copy(ctx, a.pixels, (size_t)a.width * (size_t)a.height);
    if (JS_IsException(px))
        return px;
    o = JS_NewObject(ctx);
    if (JS_IsException(o) || JS_DefinePropertyValueStr(ctx, o, "width", JS_NewInt32(ctx, a.width), JS_PROP_C_W_E) < 0
        || JS_DefinePropertyValueStr(ctx, o, "height", JS_NewInt32(ctx, a.height), JS_PROP_C_W_E) < 0
        || JS_DefinePropertyValueStr(ctx, o, "version", JS_NewUint32(ctx, a.version), JS_PROP_C_W_E) < 0
        || JS_DefinePropertyValueStr(ctx, o, "alpha", px, JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, o);
        return JS_EXCEPTION;
    }
    return o;
}

static JSValue uib_stats(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    uib* u = uib_get(ctx, this_val);
    dyn_ui_stats_t s;
    JSValue o;
    (void)argc;
    (void)argv;
    if (!u)
        return JS_EXCEPTION;
    dyn_ui_stats(u->ui, &s);
    o = JS_NewObject(ctx);
    if (JS_IsException(o) || JS_DefinePropertyValueStr(ctx, o, "elements", JS_NewUint32(ctx, s.elements), JS_PROP_C_W_E) < 0
        || JS_DefinePropertyValueStr(ctx, o, "textBytes", JS_NewUint32(ctx, s.text_bytes), JS_PROP_C_W_E) < 0
        || JS_DefinePropertyValueStr(ctx, o, "commands", JS_NewUint32(ctx, s.commands), JS_PROP_C_W_E) < 0
        || JS_DefinePropertyValueStr(ctx, o, "glyphQuads", JS_NewUint32(ctx, s.quads), JS_PROP_C_W_E) < 0
        || JS_DefinePropertyValueStr(ctx, o, "vertices", JS_NewUint32(ctx, s.vertices), JS_PROP_C_W_E) < 0
        || JS_DefinePropertyValueStr(ctx, o, "hitBoxes", JS_NewUint32(ctx, s.hits), JS_PROP_C_W_E) < 0
        || JS_DefinePropertyValueStr(ctx, o, "textRuns", JS_NewUint32(ctx, s.runs), JS_PROP_C_W_E) < 0
        || JS_DefinePropertyValueStr(ctx, o, "runRecords", JS_NewUint32(ctx, s.run_records), JS_PROP_C_W_E) < 0
        || JS_DefinePropertyValueStr(ctx, o, "glyphs", JS_NewUint32(ctx, s.glyphs), JS_PROP_C_W_E) < 0
        || JS_DefinePropertyValueStr(ctx, o, "atlasHeight", JS_NewUint32(ctx, s.atlas_height), JS_PROP_C_W_E) < 0
        || JS_DefinePropertyValueStr(ctx, o, "frames", JS_NewFloat64(ctx, (double)s.frames), JS_PROP_C_W_E) < 0
        || JS_DefinePropertyValueStr(ctx, o, "reusedFrames", JS_NewFloat64(ctx, (double)s.reused), JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, o);
        return JS_EXCEPTION;
    }
    return o;
}

static int uib_payload_ok(JSContext* ctx, JSValueConst v)
{
    double d;
    if (JS_IsString(v))
        return 1;
    return JS_IsNumber(v) && !JS_ToFloat64(ctx, &d, v) && isfinite(d);
}

static JSValue uib_drag_source(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
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

static JSValue uib_drop_target(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    uib* u = uib_in_frame(ctx, this_val, "dropTarget");
    (void)argc;
    (void)argv;
    if (!u)
        return JS_EXCEPTION;
    if (!dyn_ui_has_last(u->ui))
        return JS_ThrowTypeError(ctx, "dyna:ui: dropTarget() marks the widget drawn before it, and there is none in this frame");
    if (dyn_ui_w_drop_target(u->ui) == 2 && !JS_IsUndefined(u->dnd_payload))
        return JS_DupValue(ctx, u->dnd_payload);
    return JS_NULL;
}

static JSValue uib_autocomplete(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    static const char* const keys[] = { "hint", "width", "error", "help" };
    JSValueConst opts = argc > 3 ? argv[3] : JS_UNDEFINED;
    uib* u;
    const char *s = NULL, *t = NULL, *hint = NULL, *err = NULL, *help = NULL, *out;
    size_t n, tn, hn = 0, en = 0, hpn = 0, on = 0, *lens = NULL;
    const char** items = NULL;
    double width = 0;
    int count = 0, picked = -1;
    JSValue ret = JS_EXCEPTION;
    if (argc < 3)
        return JS_ThrowTypeError(ctx, "dyna:ui: autocomplete(label, text, suggestions, opts?)");
    if (uib_check_opts(ctx, opts, "autocomplete", keys, 4) || uib_opt_num(ctx, opts, "width", "width", 0, 16384, &width))
        return JS_EXCEPTION;
    if (uib_str(ctx, argv[0], "autocomplete", &s, &n))
        return JS_EXCEPTION;
    if (uib_str(ctx, argv[1], "text", &t, &tn)) {
        JS_FreeCString(ctx, s);
        return JS_EXCEPTION;
    }
    u = uib_in_frame(ctx, this_val, "autocomplete");
    if (!u || uib_items(ctx, u, argv[2], "autocomplete suggestions", &items, &lens, &count))
        goto done;
    if (uib_opt_str(ctx, opts, "hint", "hint", &hint, &hn) || uib_note_opts(ctx, opts, &err, &en, &help, &hpn))
        goto done;
    uib_note_begin(u, en, hpn);
    out = dyn_ui_w_autocomplete(u->ui, s, n, t, tn, items, lens, count, hint, hn, uib_px(u, width), &on, &picked);
    uib_note_end(u, err, en, help, hpn);
    if (picked < 0 && (out == t || (on == tn && !memcmp(out, t, tn))))
        ret = JS_DupValue(ctx, argv[1]);
    else
        ret = JS_NewStringLen(ctx, out, on);
done:
    dyn_nat_free(items);
    dyn_nat_free(lens);
    uib_note_free(ctx, err, help);
    if (hint)
        JS_FreeCString(ctx, hint);
    JS_FreeCString(ctx, s);
    JS_FreeCString(ctx, t);
    return ret;
}

static JSValue uib_time_picker(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    static const char* const keys[] = { "step", "h12", "error", "help" };
    JSValueConst opts = argc > 2 ? argv[2] : JS_UNDEFINED;
    uib* u;
    const char *s, *err = NULL, *help = NULL;
    size_t n, en = 0, hn = 0;
    double hour, minute, step = 1;
    int h12 = 0, hi, mi, changed;
    JSValue hv, mv, o;
    if (argc < 2 || !JS_IsObject(argv[1]))
        return JS_ThrowTypeError(ctx, "dyna:ui: timePicker(label, { hour, minute }, opts?)");
    if (uib_check_opts(ctx, opts, "timePicker", keys, 4) || uib_opt_num(ctx, opts, "step", "timePicker step", 1, 60, &step) || uib_opt_bool(ctx, opts, "h12", &h12))
        return JS_EXCEPTION;
    if (step != floor(step))
        return JS_ThrowRangeError(ctx, "dyna:ui: timePicker step is an integer 1..60");
    if (60 % (int)step)
        return JS_ThrowRangeError(ctx, "dyna:ui: timePicker step must divide 60 (1, 2, 3, 4, 5, 6, 10, 12, 15, 20, 30 or 60)");
    hv = JS_GetPropertyStr(ctx, argv[1], "hour");
    if (JS_IsException(hv))
        return JS_EXCEPTION;
    mv = JS_GetPropertyStr(ctx, argv[1], "minute");
    if (JS_IsException(mv)) {
        JS_FreeValue(ctx, hv);
        return JS_EXCEPTION;
    }
    if (uib_num(ctx, hv, "hour", 0, 23, &hour) || uib_num(ctx, mv, "minute", 0, 59, &minute)) {
        JS_FreeValue(ctx, hv);
        JS_FreeValue(ctx, mv);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, hv);
    JS_FreeValue(ctx, mv);
    if (hour != floor(hour) || minute != floor(minute))
        return JS_ThrowRangeError(ctx, "dyna:ui: timePicker hour and minute are integers");
    if (uib_str(ctx, argv[0], "timePicker", &s, &n))
        return JS_EXCEPTION;
    u = uib_in_frame(ctx, this_val, "timePicker");
    if (!u || uib_note_opts(ctx, opts, &err, &en, &help, &hn)) {
        JS_FreeCString(ctx, s);
        return JS_EXCEPTION;
    }
    hi = (int)hour;
    mi = (int)minute;
    uib_note_begin(u, en, hn);
    changed = dyn_ui_w_time(u->ui, s, n, &hi, &mi, (int)step, h12 ? DYN_UI_TIME_H12 : 0);
    uib_note_end(u, err, en, help, hn);
    uib_note_free(ctx, err, help);
    JS_FreeCString(ctx, s);
    if (!changed)
        return JS_DupValue(ctx, argv[1]);
    o = JS_NewObject(ctx);
    if (JS_IsException(o))
        return o;
    if (JS_DefinePropertyValueStr(ctx, o, "hour", JS_NewInt32(ctx, hi), JS_PROP_C_W_E) < 0
        || JS_DefinePropertyValueStr(ctx, o, "minute", JS_NewInt32(ctx, mi), JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, o);
        return JS_EXCEPTION;
    }
    return o;
}

static JSValue uib_tag_input(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    static const char* const keys[] = { "hint", "error", "help" };
    JSValueConst opts = argc > 2 ? argv[2] : JS_UNDEFINED;
    uib* u;
    const char *s, *hint = NULL, *err = NULL, *help = NULL, *add = NULL;
    size_t n, hn = 0, en = 0, hpn = 0, add_n = 0, *lens = NULL;
    const char** tags = NULL;
    int count = 0, removed, k, o;
    JSValue ret = JS_EXCEPTION;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "dyna:ui: tagInput(label, tags, opts?)");
    if (uib_check_opts(ctx, opts, "tagInput", keys, 3) || uib_str(ctx, argv[0], "tagInput", &s, &n))
        return JS_EXCEPTION;
    u = uib_in_frame(ctx, this_val, "tagInput");
    if (!u || uib_items(ctx, u, argv[1], "tagInput", &tags, &lens, &count))
        goto done;
    if (uib_opt_str(ctx, opts, "hint", "hint", &hint, &hn) || uib_note_opts(ctx, opts, &err, &en, &help, &hpn))
        goto done;
    uib_note_begin(u, en, hpn);
    removed = dyn_ui_w_tags(u->ui, s, n, tags, lens, count, hint, hn, &add, &add_n);
    uib_note_end(u, err, en, help, hpn);
    if (removed < 0 && !add) {
        ret = JS_DupValue(ctx, argv[1]);
        goto done;
    }
    ret = JS_NewArray(ctx);
    for (k = 0, o = 0; k < count && !JS_IsException(ret); k++) {
        if (k == removed)
            continue;
        if (JS_DefinePropertyValueUint32(ctx, ret, (uint32_t)o++, JS_NewStringLen(ctx, tags[k], lens[k]), JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, ret);
            ret = JS_EXCEPTION;
        }
    }
    for (k = 0; add && (size_t)k < add_n && !JS_IsException(ret);) {
        size_t l = (unsigned char)add[k];
        if (JS_DefinePropertyValueUint32(ctx, ret, (uint32_t)o++, JS_NewStringLen(ctx, add + k + 1, l), JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, ret);
            ret = JS_EXCEPTION;
        }
        k += (int)(1 + l);
    }
done:
    dyn_nat_free(tags);
    dyn_nat_free(lens);
    uib_note_free(ctx, err, help);
    if (hint)
        JS_FreeCString(ctx, hint);
    JS_FreeCString(ctx, s);
    return ret;
}

static JSValue uib_accordion(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    uib* u;
    const char* s;
    size_t n, *lens = NULL;
    const char** items = NULL;
    int open, count = 0, k, out, failed = 0;
    if (argc < 4 || uib_index(ctx, argv[2], "accordion open", &open) || uib_fn(ctx, argv[3], "accordion"))
        return JS_HasException(ctx) ? JS_EXCEPTION : JS_ThrowTypeError(ctx, "dyna:ui: accordion(label, sections, open, fn)");
    if (uib_str(ctx, argv[0], "accordion", &s, &n))
        return JS_EXCEPTION;
    u = uib_in_frame(ctx, this_val, "accordion");
    if (!u || uib_items(ctx, u, argv[1], "accordion", &items, &lens, &count)) {
        JS_FreeCString(ctx, s);
        return JS_EXCEPTION;
    }
    if (open >= count)
        open = -1;
    out = open;
    dyn_ui_w_accordion_begin(u->ui, s, n);
    for (k = 0; k < count; k++) {
        if (dyn_ui_w_accordion_item(u->ui, k, items[k], lens[k], k == open))
            out = k == open ? -1 : k;
        if (k == open && !failed) {
            JSValueConst a[2];
            JSValue r;
            a[0] = this_val;
            a[1] = JS_NewInt32(ctx, k);
            dyn_ui_w_accordion_section_begin(u->ui);
            r = JS_Call(ctx, argv[3], JS_UNDEFINED, 2, a);
            dyn_ui_w_accordion_section_end(u->ui);
            if (JS_IsException(r))
                failed = 1;
            JS_FreeValue(ctx, r);
        }
    }
    dyn_ui_w_accordion_end(u->ui);
    dyn_nat_free(items);
    dyn_nat_free(lens);
    JS_FreeCString(ctx, s);
    return failed ? JS_EXCEPTION : JS_NewInt32(ctx, out);
}

static const JSCFunctionListEntry uib_proto[] = {
    JS_CFUNC_DEF("frame", 1, uib_frame),
    JS_CFUNC_DEF("pixels", 0, uib_pixels),
    JS_CFUNC_DEF("theme", 1, uib_theme),
    JS_CFUNC_DEF("loadFont", 1, uib_load_font),
    JS_CFUNC_MAGIC_DEF("addImage", 3, uib_add_image, 0),
    JS_CFUNC_MAGIC_DEF("updateImage", 2, uib_add_image, 1),
    JS_CFUNC_DEF("removeImage", 1, uib_remove_image),
    JS_CFUNC_DEF("pointer", 1, uib_pointer),
    JS_CFUNC_DEF("key", 3, uib_key),
    JS_CFUNC_DEF("type", 1, uib_type),
    JS_CFUNC_DEF("setTime", 1, uib_set_time),
    JS_CGETSET_MAGIC_DEF("width", uib_getter, NULL, 0),
    JS_CGETSET_MAGIC_DEF("height", uib_getter, NULL, 1),
    JS_CGETSET_MAGIC_DEF("wakeIn", uib_getter, NULL, 2),
    JS_CGETSET_MAGIC_DEF("submitted", uib_getter, NULL, 3),
    JS_CGETSET_MAGIC_DEF("activated", uib_getter, NULL, 4),
    JS_CGETSET_MAGIC_DEF("editing", uib_getter, NULL, 5),
    JS_CGETSET_MAGIC_DEF("pixelWidth", uib_getter, NULL, 6),
    JS_CGETSET_MAGIC_DEF("pixelHeight", uib_getter, NULL, 7),
    JS_CGETSET_MAGIC_DEF("scale", uib_getter, NULL, 8),
    JS_CGETSET_MAGIC_DEF("sizeClass", uib_getter, NULL, 9),
    JS_CGETSET_MAGIC_DEF("textInput", uib_getter, NULL, 10),
    JS_CGETSET_MAGIC_DEF("insets", uib_getter, NULL, 11),
    JS_CFUNC_MAGIC_DEF("row", 2, uib_container, 0),
    JS_CFUNC_MAGIC_DEF("column", 2, uib_container, 1),
    JS_CFUNC_MAGIC_DEF("box", 2, uib_container, 2),
    JS_CFUNC_DEF("id", 2, uib_id),
    JS_CFUNC_DEF("spacer", 2, uib_spacer),
    JS_CFUNC_MAGIC_DEF("label", 1, uib_label_like, 0),
    JS_CFUNC_MAGIC_DEF("heading", 1, uib_label_like, 1),
    JS_CFUNC_MAGIC_DEF("bullet", 1, uib_label_like, 2),
    JS_CFUNC_MAGIC_DEF("separator", 1, uib_label_like, 3),
    JS_CFUNC_MAGIC_DEF("link", 1, uib_label_like, 4),
    JS_CFUNC_MAGIC_DEF("button", 1, uib_label_like, 5),
    JS_CFUNC_MAGIC_DEF("tooltip", 2, uib_label_like, 6),
    JS_CFUNC_DEF("text", 2, uib_text),
    JS_CFUNC_DEF("badge", 2, uib_badge),
    JS_CFUNC_DEF("image", 4, uib_image_w),
    JS_CFUNC_DEF("progress", 2, uib_progress),
    JS_CFUNC_DEF("spinner", 1, uib_spinner),
    JS_CFUNC_DEF("iconButton", 3, uib_icon_button),
    JS_CFUNC_MAGIC_DEF("toggle", 2, uib_flag, 0),
    JS_CFUNC_MAGIC_DEF("checkbox", 2, uib_flag, 1),
    JS_CFUNC_MAGIC_DEF("switch", 2, uib_flag, 2),
    JS_CFUNC_MAGIC_DEF("selectable", 2, uib_flag, 3),
    JS_CFUNC_MAGIC_DEF("radio", 4, uib_choice, 0),
    JS_CFUNC_MAGIC_DEF("segmented", 3, uib_choice, 1),
    JS_CFUNC_MAGIC_DEF("tabs", 3, uib_choice, 2),
    JS_CFUNC_MAGIC_DEF("combo", 3, uib_choice, 3),
    JS_CFUNC_DEF("slider", 5, uib_slider),
    JS_CFUNC_DEF("range", 5, uib_range),
    JS_CFUNC_MAGIC_DEF("drag", 3, uib_number, 0),
    JS_CFUNC_MAGIC_DEF("spin", 3, uib_number, 1),
    JS_CFUNC_MAGIC_DEF("textField", 3, uib_edit, 0),
    JS_CFUNC_MAGIC_DEF("textArea", 3, uib_edit, 1),
    JS_CFUNC_MAGIC_DEF("search", 3, uib_edit, 2),
    JS_CFUNC_MAGIC_DEF("list", 4, uib_list, 0),
    JS_CFUNC_MAGIC_DEF("log", 3, uib_list, 1),
    JS_CFUNC_DEF("color", 2, uib_color_w),
    JS_CFUNC_DEF("table", 2, uib_table),
    JS_CFUNC_MAGIC_DEF("plot", 3, uib_plot, 0),
    JS_CFUNC_MAGIC_DEF("bars", 3, uib_plot, 1),
    JS_CFUNC_MAGIC_DEF("property", 2, uib_wrap, UIB_PROPERTY),
    JS_CFUNC_MAGIC_DEF("group", 2, uib_wrap, UIB_GROUP),
    JS_CFUNC_MAGIC_DEF("panel", 3, uib_wrap, UIB_PANEL),
    JS_CFUNC_MAGIC_DEF("scroll", 3, uib_wrap, UIB_SCROLL),
    JS_CFUNC_MAGIC_DEF("menubar", 1, uib_wrap, UIB_MENUBAR),
    JS_CFUNC_MAGIC_DEF("toolbar", 1, uib_wrap, UIB_TOOLBAR),
    JS_CFUNC_MAGIC_DEF("statusbar", 1, uib_wrap, UIB_STATUSBAR),
    JS_CFUNC_DEF("window", 3, uib_window),
    JS_CFUNC_DEF("collapsing", 2, uib_collapsing),
    JS_CFUNC_DEF("tree", 3, uib_tree),
    JS_CFUNC_MAGIC_DEF("openPopup", 1, uib_popup_cmd, UIB_OPEN_POPUP),
    JS_CFUNC_MAGIC_DEF("openModal", 1, uib_popup_cmd, UIB_OPEN_MODAL),
    JS_CFUNC_MAGIC_DEF("closePopup", 0, uib_popup_cmd, UIB_CLOSE_POPUP),
    JS_CFUNC_MAGIC_DEF("popup", 2, uib_popup, UIB_POPUP),
    JS_CFUNC_MAGIC_DEF("contextMenu", 2, uib_popup, UIB_CONTEXT),
    JS_CFUNC_MAGIC_DEF("modal", 3, uib_popup, UIB_MODAL),
    JS_CFUNC_MAGIC_DEF("menu", 2, uib_popup, UIB_MENU),
    JS_CFUNC_DEF("menuItem", 2, uib_menu_item),
    JS_CFUNC_DEF("split", 5, uib_split),
    JS_CFUNC_MAGIC_DEF("wrap", 2, uib_container, 3),
    JS_CFUNC_DEF("chip", 2, uib_chip),
    JS_CFUNC_MAGIC_DEF("card", 3, uib_card, 0),
    JS_CFUNC_MAGIC_DEF("appBar", 2, uib_card, 1),
    JS_CFUNC_MAGIC_DEF("navBar", 3, uib_nav, 0),
    JS_CFUNC_MAGIC_DEF("navRail", 3, uib_nav, 1),
    JS_CFUNC_MAGIC_DEF("openDrawer", 1, uib_open_overlay, UIB_OPEN_DRAWER),
    JS_CFUNC_MAGIC_DEF("openSheet", 1, uib_open_overlay, UIB_OPEN_SHEET),
    JS_CFUNC_MAGIC_DEF("drawer", 3, uib_overlay, 0),
    JS_CFUNC_MAGIC_DEF("sheet", 2, uib_overlay, 1),
    JS_CFUNC_DEF("toast", 2, uib_toast),
    JS_CFUNC_DEF("fab", 2, uib_fab),
    JS_CFUNC_MAGIC_DEF("breadcrumbs", 2, uib_seq, UIB_BREADCRUMBS),
    JS_CFUNC_MAGIC_DEF("stepper", 3, uib_seq, UIB_STEPPER),
    JS_CFUNC_DEF("pagination", 3, uib_pagination),
    JS_CFUNC_DEF("rating", 3, uib_rating),
    JS_CFUNC_DEF("datePicker", 3, uib_date),
    JS_CFUNC_DEF("avatar", 2, uib_avatar),
    JS_CFUNC_DEF("skeleton", 2, uib_skeleton),
    JS_CFUNC_DEF("composition", 2, uib_composition),
    JS_CFUNC_DEF("resize", 3, uib_resize),
    JS_CFUNC_DEF("idle", 1, uib_idle),
    JS_CFUNC_DEF("geometry", 0, uib_geometry),
    JS_CFUNC_DEF("atlas", 0, uib_atlas_pixels),
    JS_CFUNC_DEF("stats", 0, uib_stats),
    JS_CFUNC_DEF("lastRect", 0, uib_last_rect),
    JS_CFUNC_DEF("dragSource", 2, uib_drag_source),
    JS_CFUNC_DEF("dropTarget", 0, uib_drop_target),
    JS_CGETSET_MAGIC_DEF("dragging", uib_getter, NULL, 12),
    JS_CFUNC_DEF("autocomplete", 4, uib_autocomplete),
    JS_CFUNC_DEF("timePicker", 3, uib_time_picker),
    JS_CFUNC_DEF("tagInput", 3, uib_tag_input),
    JS_CFUNC_DEF("accordion", 4, uib_accordion),
    JS_CFUNC_DEF("scaffold", 4, uib_scaffold),
    JS_CFUNC_DEF("pixelsInto", 1, uib_pixels_into),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "Context", JS_PROP_CONFIGURABLE),
};

static const struct {
    const char* name;
    uint32_t dark, light;
} uib_palettes[] = {
    { "text", 0xE6E6E6FFu, 0x1F2328FFu },
    { "textDim", 0x9AA0A6FFu, 0x5F6670FFu },
    { "onAccent", 0xFFFFFFFFu, 0xFFFFFFFFu },
    { "bg", 0x202428FFu, 0xF6F7F9FFu },
    { "panel", 0x262B30FFu, 0xFFFFFFFFu },
    { "frame", 0x15181BFFu, 0xFFFFFFFFu },
    { "frameHot", 0x1B1F23FFu, 0xF0F2F5FFu },
    { "frameActive", 0x101214FFu, 0xE6E9EEFFu },
    { "button", 0x3A4048FFu, 0xE9ECF0FFu },
    { "buttonHot", 0x4A525CFFu, 0xDDE2E8FFu },
    { "buttonActive", 0x2F343AFFu, 0xCFD5DDFFu },
    { "accent", 0x3D7EFFFFu, 0x2F6FEBFFu },
    { "accentHot", 0x5A92FFFFu, 0x4A84F0FFu },
    { "border", 0x50565EFFu, 0xC9CFD6FFu },
    { "title", 0x2C3238FFu, 0xECEFF3FFu },
    { "titleActive", 0x34506EFFu, 0xD6E4FBFFu },
    { "selection", 0x3D7EFF66u, 0x2F6FEB40u },
    { "popup", 0x2A2F35FFu, 0xFFFFFFFFu },
    { "shade", 0x00000080u, 0x00000059u },
    { "tooltip", 0x111315F0u, 0xFFFFFFF5u },
    { "link", 0x6FA8FFFFu, 0x1F5FD6FFu },
    { "scrollbar", 0x5A616A99u, 0x8A93A066u },
    { "scrollbarHot", 0x7A828CCCu, 0x6B7480AAu },
    { "error", 0xF85149FFu, 0xCF222EFFu },
};

static JSValue uib_presets(JSContext* ctx, int which)
{
    JSValue o = JS_NewObject(ctx);
    size_t i;
    if (JS_IsException(o))
        return o;
    if (which < 2) {
        for (i = 0; i < countof(uib_palettes); i++)
            if (JS_DefinePropertyValueStr(ctx, o, uib_palettes[i].name, JS_NewUint32(ctx, which ? uib_palettes[i].light : uib_palettes[i].dark), JS_PROP_ENUMERABLE) < 0)
                goto fail;
    } else {
        dyn_ui_theme t;
        uib_density(&t, which == 3);
        if (JS_DefinePropertyValueStr(ctx, o, "minTarget", JS_NewFloat64(ctx, (double)t.min_target), JS_PROP_ENUMERABLE) < 0
            || JS_DefinePropertyValueStr(ctx, o, "padX", JS_NewFloat64(ctx, (double)t.pad_x), JS_PROP_ENUMERABLE) < 0
            || JS_DefinePropertyValueStr(ctx, o, "padY", JS_NewFloat64(ctx, (double)t.pad_y), JS_PROP_ENUMERABLE) < 0
            || JS_DefinePropertyValueStr(ctx, o, "spacing", JS_NewFloat64(ctx, (double)t.spacing), JS_PROP_ENUMERABLE) < 0)
            goto fail;
    }
    return o;
fail:
    JS_FreeValue(ctx, o);
    return JS_EXCEPTION;
}

static int uib_init_module(JSContext* ctx, JSModuleDef* m)
{
    JSValue themes, dens;
    static const JSClassDef def = { "Context", .finalizer = dyn_res_finalizer, .gc_mark = uib_mark };
    JSValueConst args[1];
    JSValue ta;
    args[0] = JS_NewInt32(ctx, 0);
    ta = JS_NewTypedArray(ctx, 1, args, JS_TYPED_ARRAY_UINT8);
    if (JS_IsException(ta))
        return -1;
    uib_u8_cid = JS_GetClassID(ta);
    JS_FreeValue(ctx, ta);
    ta = JS_NewTypedArray(ctx, 1, args, JS_TYPED_ARRAY_FLOAT64);
    if (JS_IsException(ta))
        return -1;
    uib_f64_cid = JS_GetClassID(ta);
    JS_FreeValue(ctx, ta);
    ta = JS_NewTypedArray(ctx, 1, args, JS_TYPED_ARRAY_FLOAT32);
    if (JS_IsException(ta))
        return -1;
    uib_f32_cid = JS_GetClassID(ta);
    JS_FreeValue(ctx, ta);
    themes = JS_NewObject(ctx);
    dens = JS_NewObject(ctx);
    if (JS_IsException(themes) || JS_IsException(dens) || JS_DefinePropertyValueStr(ctx, themes, "dark", uib_presets(ctx, 0), JS_PROP_ENUMERABLE) < 0
        || JS_DefinePropertyValueStr(ctx, themes, "light", uib_presets(ctx, 1), JS_PROP_ENUMERABLE) < 0
        || JS_DefinePropertyValueStr(ctx, dens, "compact", uib_presets(ctx, 2), JS_PROP_ENUMERABLE) < 0
        || JS_DefinePropertyValueStr(ctx, dens, "touch", uib_presets(ctx, 3), JS_PROP_ENUMERABLE) < 0) {
        JS_FreeValue(ctx, themes);
        JS_FreeValue(ctx, dens);
        return -1;
    }
    if (JS_SetModuleExport(ctx, m, "themes", themes) < 0 || JS_SetModuleExport(ctx, m, "densities", dens) < 0)
        return -1;
    return dyn_register_class(ctx, m, &uib_cid, &def, uib_proto, countof(uib_proto), uib_ctor, "Context");
}

int js_nat_init_ui(JSContext* ctx)
{
    JSModuleDef* m = JS_NewCModule(ctx, "dyna:ui", uib_init_module);
    if (!m)
        return -1;
    if (JS_AddModuleExport(ctx, m, "themes") < 0 || JS_AddModuleExport(ctx, m, "densities") < 0)
        return -1;
    return JS_AddModuleExport(ctx, m, "Context");
}

#endif
