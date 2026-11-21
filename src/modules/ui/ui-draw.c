#include "ui-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_UI)

JSValue uib_pixels(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
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

JSValue uib_pixels_into(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
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

JSValue uib_geometry(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
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

#endif
