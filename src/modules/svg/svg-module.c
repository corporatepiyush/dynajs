#include "dyna-nat.h"
#include "core/dyn-svg.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_SVG)

#define SVG_MAX_SIDE 65536.0

typedef struct {
    char* data;
    size_t len;
    svg2img_options opts;
    int png;
    int rc;
    uint8_t* out;
    size_t out_len;
    int w, h;
} SvgJob;

static void svg_job_free(SvgJob* j)
{
    free(j->data);
    free(j->out);
    free(j);
}

static JSValue svg_u8_take(JSContext* ctx, uint8_t* p, size_t n)
{
    JSValue ab = dyn_nat_ab_take(ctx, p, n), u8;
    JSValueConst args[3];
    if (JS_IsException(ab))
        return ab;
    args[0] = ab;
    args[1] = JS_UNDEFINED;
    args[2] = JS_UNDEFINED;
    u8 = JS_NewTypedArray(ctx, 3, args, JS_TYPED_ARRAY_UINT8);
    JS_FreeValue(ctx, ab);
    return u8;
}

static JSValue svg_result(JSContext* ctx, SvgJob* j)
{
    JSValue data, obj;
    uint8_t* out = j->out;
    switch (j->rc) {
    case SVG2IMG_OK:
        break;
    case SVG2IMG_ERR_FORMAT:
        return JS_ThrowTypeError(ctx, "dyna:svg: input is not an SVG document");
    case SVG2IMG_ERR_LIMIT:
        return JS_ThrowRangeError(ctx, "dyna:svg: a resource limit was exceeded (image too large or document too complex)");
    case SVG2IMG_ERR_NOMEM:
        return JS_ThrowOutOfMemory(ctx);
    default:
        return JS_ThrowInternalError(ctx, "dyna:svg: render failed (code %d)", j->rc);
    }
    j->out = NULL;
    data = svg_u8_take(ctx, out, j->out_len);
    if (JS_IsException(data) || j->png)
        return data;
    obj = JS_NewObject(ctx);
    if (JS_IsException(obj)) {
        JS_FreeValue(ctx, data);
        return obj;
    }
    if (JS_DefinePropertyValueStr(ctx, obj, "width", JS_NewInt32(ctx, j->w), JS_PROP_C_W_E) < 0
        || JS_DefinePropertyValueStr(ctx, obj, "height", JS_NewInt32(ctx, j->h), JS_PROP_C_W_E) < 0
        || JS_DefinePropertyValueStr(ctx, obj, "data", data, JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    return obj;
}

static JSValue svg_entry(JSContext* ctx, void* arg)
{
    SvgJob* j = (SvgJob*)arg;
    JSValue res;
    j->rc = DYN_OFFLOAD_CPU(dyn_svg_render(j->data, j->len, &j->opts, j->png, &j->out, &j->out_len, &j->w, &j->h));
    res = svg_result(ctx, j);
    svg_job_free(j);
    return res;
}

static int svg_input(JSContext* ctx, JSValueConst v, SvgJob* j)
{
    if (JS_IsString(v)) {
        size_t n;
        const char* s = JS_ToCStringLen(ctx, &n, v);
        if (!s)
            return -1;
        j->data = (char*)malloc(n + 1);
        if (!j->data) {
            JS_FreeCString(ctx, s);
            JS_ThrowOutOfMemory(ctx);
            return -1;
        }
        memcpy(j->data, s, n);
        j->data[n] = 0;
        j->len = n;
        JS_FreeCString(ctx, s);
        return 0;
    }
    if (JS_IsObject(v)) {
        size_t off = 0, blen = 0, bpe = 0, abn = 0;
        JSValue ab = JS_GetTypedArrayBuffer(ctx, v, &off, &blen, &bpe);
        uint8_t* p;
        if (JS_IsException(ab)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
        } else {
            p = JS_GetArrayBuffer(ctx, &abn, ab);
            JS_FreeValue(ctx, ab);
            if (bpe == 1 && (p || !blen) && off + blen <= abn) {
                j->data = (char*)malloc(blen + 1);
                if (!j->data) {
                    JS_ThrowOutOfMemory(ctx);
                    return -1;
                }
                if (blen)
                    memcpy(j->data, p + off, blen);
                j->data[blen] = 0;
                j->len = blen;
                return 0;
            }
            if (!p)
                JS_FreeValue(ctx, JS_GetException(ctx));
        }
    }
    JS_ThrowTypeError(ctx, "dyna:svg: input must be a string or a Uint8Array");
    return -1;
}

static int svg_side(JSContext* ctx, JSValueConst o, const char* k, int* out)
{
    JSValue v = JS_GetPropertyStr(ctx, o, k);
    double d;
    if (JS_IsException(v))
        return -1;
    if (JS_IsUndefined(v))
        return 0;
    if (!JS_IsNumber(v)) {
        JS_FreeValue(ctx, v);
        JS_ThrowTypeError(ctx, "dyna:svg: options.%s must be a number", k);
        return -1;
    }
    JS_ToFloat64(ctx, &d, v);
    if (!(d >= 1 && d <= SVG_MAX_SIDE) || d != floor(d)) {
        JS_ThrowRangeError(ctx, "dyna:svg: options.%s must be an integer in 1..65536", k);
        return -1;
    }
    *out = (int)d;
    return 0;
}

static int svg_hex(const char* s, size_t n, unsigned* out)
{
    unsigned v = 0;
    size_t i;
    if (n && s[0] == '#') {
        s++;
        n--;
    }
    if (n != 6 && n != 8)
        return -1;
    for (i = 0; i < n; i++) {
        int c = (unsigned char)s[i], d;
        if (c >= '0' && c <= '9')
            d = c - '0';
        else if (c >= 'a' && c <= 'f')
            d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F')
            d = c - 'A' + 10;
        else
            return -1;
        v = (v << 4) | (unsigned)d;
    }
    *out = n == 6 ? (v << 8) | 0xFFu : v;
    return 0;
}

static const char* const svg_opt_keys[] = { "width", "height", "scale", "background" };

static int svg_options(JSContext* ctx, JSValueConst o, svg2img_options* so)
{
    JSValue v;
    double d;
    memset(so, 0, sizeof(*so));
    so->scale = 1.0f;
    so->aa = 4;
    if (JS_IsUndefined(o) || JS_IsNull(o))
        return 0;
    if (!JS_IsObject(o)) {
        JS_ThrowTypeError(ctx, "dyna:svg: options must be an object");
        return -1;
    }
    if (dyn_opts_strict(ctx, o, svg_opt_keys, (int)(sizeof(svg_opt_keys) / sizeof(svg_opt_keys[0]))) < 0)
        return -1;
    if (svg_side(ctx, o, "width", &so->width) || svg_side(ctx, o, "height", &so->height))
        return -1;
    v = JS_GetPropertyStr(ctx, o, "scale");
    if (JS_IsException(v))
        return -1;
    if (!JS_IsUndefined(v)) {
        if (!JS_IsNumber(v)) {
            JS_FreeValue(ctx, v);
            JS_ThrowTypeError(ctx, "dyna:svg: options.scale must be a number");
            return -1;
        }
        JS_ToFloat64(ctx, &d, v);
        if (!(d > 0 && d <= SVG_MAX_SIDE)) {
            JS_ThrowRangeError(ctx, "dyna:svg: options.scale must be > 0 and <= 65536");
            return -1;
        }
        so->scale = (float)d;
    }
    v = JS_GetPropertyStr(ctx, o, "background");
    if (JS_IsException(v))
        return -1;
    if (JS_IsUndefined(v))
        return 0;
    if (JS_IsNumber(v)) {
        JS_ToFloat64(ctx, &d, v);
        if (!(d >= 0 && d <= 4294967295.0) || d != floor(d)) {
            JS_ThrowRangeError(ctx, "dyna:svg: options.background must be a 0xRRGGBBAA integer");
            return -1;
        }
        so->background = (unsigned)d;
    } else if (JS_IsString(v)) {
        size_t n;
        const char* s = JS_ToCStringLen(ctx, &n, v);
        int bad;
        JS_FreeValue(ctx, v);
        if (!s)
            return -1;
        bad = svg_hex(s, n, &so->background);
        JS_FreeCString(ctx, s);
        if (bad) {
            JS_ThrowTypeError(ctx, "dyna:svg: options.background must be \"#RRGGBB\" or \"#RRGGBBAA\"");
            return -1;
        }
    } else {
        JS_FreeValue(ctx, v);
        JS_ThrowTypeError(ctx, "dyna:svg: options.background must be a string or a number");
        return -1;
    }
    so->has_background = 1;
    return 0;
}

static JSValue svg_render(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv, int magic)
{
    JSValue funcs[2], promise, e, r;
    SvgJob* j;
    (void)this_val;
    j = (SvgJob*)calloc(1, sizeof(*j));
    if (!j)
        return JS_ThrowOutOfMemory(ctx);
    j->png = magic;
    if (svg_input(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, j) == 0
        && svg_options(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, &j->opts) == 0) {
        promise = dyn_fiber_start(ctx, svg_entry, j);
        if (JS_IsException(promise))
            svg_job_free(j);
        return promise;
    }
    svg_job_free(j);
    e = JS_GetException(ctx);
    promise = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(promise)) {
        JS_FreeValue(ctx, e);
        return promise;
    }
    r = JS_Call(ctx, funcs[1], JS_UNDEFINED, 1, (JSValueConst*)&e);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, e);
    JS_FreeValue(ctx, funcs[0]);
    JS_FreeValue(ctx, funcs[1]);
    return promise;
}

static const JSCFunctionListEntry svg_funcs[] = {
    JS_CFUNC_MAGIC_DEF("render", 2, svg_render, 1),
    JS_CFUNC_MAGIC_DEF("renderRGBA", 2, svg_render, 0),
};

static int svg_init_module(JSContext* ctx, JSModuleDef* m)
{
    return JS_SetModuleExportList(ctx, m, svg_funcs, (int)(sizeof(svg_funcs) / sizeof(svg_funcs[0])));
}

int js_nat_init_svg(JSContext* ctx)
{
    JSModuleDef* m = JS_NewCModule(ctx, "dyna:svg", svg_init_module);
    if (!m)
        return -1;
    return JS_AddModuleExportList(ctx, m, svg_funcs, (int)(sizeof(svg_funcs) / sizeof(svg_funcs[0])));
}

#endif
