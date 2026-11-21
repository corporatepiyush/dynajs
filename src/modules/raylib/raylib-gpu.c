#include "raylib-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_RAYLIB)

void rl_gpu_link(rl_gpu* g)
{
    g->prev = NULL;
    g->next = rl.gpu;
    if (rl.gpu)
        rl.gpu->prev = g;
    rl.gpu = g;
}

static void rl_gpu_unlink(rl_gpu* g)
{
    if (g->prev)
        g->prev->next = g->next;
    else if (rl.gpu == g)
        rl.gpu = g->next;
    if (g->next)
        g->next->prev = g->prev;
    g->next = g->prev = NULL;
}

void rl_gpu_unload(rl_gpu* g)
{
    if (!g->alive)
        return;
    if (rl.depth)
        rlDrawRenderBatchActive();
    if (g->kind == RL_TEX)
        UnloadTexture(g->tex);
    else if (g->kind == RL_RT)
        UnloadRenderTexture(g->rt);
    else
        UnloadFont(g->font);
    g->alive = 0;
}

void rl_gpu_dispose(void* p)
{
    rl_gpu* g = p;
    JSValue held = g->win_obj;
    JSRuntime* jrt = g->jrt;
    g->win_obj = JS_UNDEFINED;
    if (g == rl.active_rt) {
        g->pending_free = 1;
    } else {
        rl_gpu_unload(g);
        rl_gpu_unlink(g);
        dyn_nat_free(g);
    }
    JS_FreeValueRT(jrt, held);
}

void rl_gpu_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark)
{
    JSClassID id;
    DynResource* r = JS_GetAnyOpaque(val, &id);
    if (r && r->native)
        JS_MarkValue(rt, ((rl_gpu*)r->native)->win_obj, mark);
}

rl_gpu* rl_gpu_of(JSContext* ctx, JSValueConst v, JSClassID cid)
{
    rl_gpu* g = dyn_res_native(ctx, v, cid);
    if (g && !g->alive) {
        JS_ThrowTypeError(ctx, "dyna:raylib: the Window that owned this resource was closed");
        return NULL;
    }
    return g;
}

rl_gpu* rl_gpu_new(JSContext* ctx, int kind)
{
    rl_gpu* g = dyn_nat_calloc(1, sizeof(*g));
    if (!g) {
        JS_ThrowOutOfMemory(ctx);
        return NULL;
    }
    g->kind = kind;
    g->win_obj = JS_UNDEFINED;
    return g;
}

JSValue rl_gpu_size(JSContext* ctx, JSValueConst this_val, int magic)
{
    JSClassID cid = JS_GetClassID(this_val);
    rl_gpu* g = rl_gpu_of(ctx, this_val, cid == rl_rt_cid ? rl_rt_cid : rl_tex_cid);
    Texture2D t;
    if (!g)
        return JS_EXCEPTION;
    t = g->kind == RL_RT ? g->rt.texture : g->tex;
    return JS_NewInt32(ctx, magic ? t.height : t.width);
}

JSValue rl_tex_update(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    Rectangle rc = { 0, 0, 0, 0 };
    int have = argc > 1 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1]);
    size_t off, blen, bpe, ab, need;
    JSValue buf;
    uint8_t* base;
    rl_gpu* g;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "dyna:raylib: update(pixels, rect?) needs an RGBA Uint8Array");
    if (have && rl_rect(ctx, argv[1], "update rect", &rc))
        return JS_EXCEPTION;
    if (!rl_ready(ctx, "Texture#update()"))
        return JS_EXCEPTION;
    g = rl_gpu_of(ctx, this_val, rl_tex_cid);
    if (!g)
        return JS_EXCEPTION;
    if (!have)
        rc = (Rectangle) { 0, 0, (float)g->tex.width, (float)g->tex.height };
    if (rc.x != floorf(rc.x) || rc.y != floorf(rc.y) || rc.width != floorf(rc.width) || rc.height != floorf(rc.height)
        || rc.width < 1 || rc.height < 1 || rc.x < 0 || rc.y < 0 || rc.x + rc.width > (float)g->tex.width
        || rc.y + rc.height > (float)g->tex.height)
        return JS_ThrowRangeError(ctx, "dyna:raylib: update rect must be whole pixels inside the %dx%d texture", g->tex.width, g->tex.height);
    buf = JS_GetTypedArrayBuffer(ctx, argv[0], &off, &blen, &bpe);
    if (JS_IsException(buf))
        return JS_EXCEPTION;
    base = JS_GetArrayBuffer(ctx, &ab, buf);
    JS_FreeValue(ctx, buf);
    if (!base)
        return JS_EXCEPTION;
    if (bpe != 1)
        return JS_ThrowTypeError(ctx, "dyna:raylib: update pixels must be a byte view (Uint8Array) of RGBA");
    need = (size_t)rc.width * (size_t)rc.height * 4;
    if (off > ab || blen > ab - off || blen < need)
        return JS_ThrowRangeError(ctx, "dyna:raylib: update needs %zu bytes of RGBA for a %dx%d area, got %zu", need, (int)rc.width, (int)rc.height, blen);
    if (rl.depth)
        rlDrawRenderBatchActive();
    UpdateTextureRec(g->tex, rc, base + off);
    return JS_UNDEFINED;
}

JSValue rl_gpu_filter(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    static const char* const names[] = { "point", "bilinear", "trilinear" };
    JSClassID cid = JS_GetClassID(this_val);
    const char* s;
    int mode = -1, i;
    rl_gpu* g;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "dyna:raylib: setFilter(\"point\" | \"bilinear\" | \"trilinear\")");
    s = JS_ToCString(ctx, argv[0]);
    if (!s)
        return JS_EXCEPTION;
    for (i = 0; i < 3; i++)
        if (!strcmp(s, names[i]))
            mode = i;
    JS_FreeCString(ctx, s);
    if (mode < 0)
        return JS_ThrowRangeError(ctx, "dyna:raylib: unknown filter (point, bilinear, trilinear)");
    if (!rl_ready(ctx, "setFilter()"))
        return JS_EXCEPTION;
    g = rl_gpu_of(ctx, this_val, cid == rl_rt_cid ? rl_rt_cid : rl_tex_cid);
    if (!g)
        return JS_EXCEPTION;
    SetTextureFilter(g->kind == RL_RT ? g->rt.texture : g->tex, mode);
    return JS_UNDEFINED;
}

JSValue rl_rt_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv)
{
    int w, h;
    rl_gpu* g;
    (void)new_target;
    if (rl_args(ctx, argc, argv, 2, "new RenderTexture(width, height)") || rl_dims(ctx, argv[0], argv[1], "RenderTexture", &w, &h))
        return JS_EXCEPTION;
    if (!rl_ready(ctx, "new RenderTexture()"))
        return JS_EXCEPTION;
    g = rl_gpu_new(ctx, RL_RT);
    if (!g)
        return JS_EXCEPTION;
    if (rl.depth)
        rlDrawRenderBatchActive();
    g->rt = LoadRenderTexture(w, h);
    if (!IsRenderTextureValid(g->rt)) {
        dyn_nat_free(g);
        return JS_ThrowInternalError(ctx, "dyna:raylib: the GPU refused a %dx%d render texture", w, h);
    }
    return rl_wrap_gpu(ctx, rl_rt_cid, g);
}

JSValue rl_rt_draw(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    rl_gpu* g;
    JSValue r;
    if (argc < 1 || !JS_IsFunction(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "dyna:raylib: draw(fn) needs a function");
    if (!rl_ready(ctx, "RenderTexture#draw()"))
        return JS_EXCEPTION;
    g = rl_gpu_of(ctx, this_val, rl_rt_cid);
    if (!g)
        return JS_EXCEPTION;
    if (rl.in_texture || rl.in_mode2d || rl.in_mode3d)
        return JS_ThrowTypeError(ctx, "dyna:raylib: RenderTexture#draw() cannot run inside another draw(), mode2D() or mode3D()");
    BeginTextureMode(g->rt);
    rl.in_texture = 1;
    rl.active_rt = g;
    rl.depth++;
    r = JS_Call(ctx, argv[0], JS_UNDEFINED, 0, NULL);
    rl.depth--;
    rl.active_rt = NULL;
    rl.in_texture = 0;
    EndTextureMode();
    if (g->pending_free) {
        rl_gpu_unload(g);
        rl_gpu_unlink(g);
        dyn_nat_free(g);
    }
    rl_after_scope();
    if (rl_sync_result(ctx, r, "RenderTexture#draw(fn)"))
        return JS_EXCEPTION;
    return JS_UNDEFINED;
}

static JSValue rl_rgba_object(JSContext* ctx, int w, int h, const uint8_t* px)
{
    JSValue o = JS_NewObject(ctx), u8;
    if (JS_IsException(o))
        return o;
    u8 = dyn_nat_u8_copy(ctx, px, (size_t)w * (size_t)h * 4);
    if (JS_IsException(u8) || JS_DefinePropertyValueStr(ctx, o, "width", JS_NewInt32(ctx, w), JS_PROP_C_W_E) < 0
        || JS_DefinePropertyValueStr(ctx, o, "height", JS_NewInt32(ctx, h), JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, o);
        JS_FreeValue(ctx, u8);
        return JS_EXCEPTION;
    }
    if (JS_DefinePropertyValueStr(ctx, o, "pixels", u8, JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, o);
        return JS_EXCEPTION;
    }
    return o;
}

JSValue rl_rt_read(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    rl_gpu* g;
    Image im;
    JSValue o;
    (void)argc;
    (void)argv;
    if (!rl_ready(ctx, "RenderTexture#readPixels()"))
        return JS_EXCEPTION;
    g = rl_gpu_of(ctx, this_val, rl_rt_cid);
    if (!g)
        return JS_EXCEPTION;
    if (g == rl.active_rt)
        return JS_ThrowTypeError(ctx, "dyna:raylib: readPixels() cannot run inside the same RenderTexture's draw()");
    if (rl.depth)
        rlDrawRenderBatchActive();
    im = LoadImageFromTexture(g->rt.texture);
    if (!IsImageValid(im))
        return JS_ThrowInternalError(ctx, "dyna:raylib: could not read the render texture back");
    if (im.format != PIXELFORMAT_UNCOMPRESSED_R8G8B8A8)
        ImageFormat(&im, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
    ImageFlipVertical(&im);
    o = rl_rgba_object(ctx, im.width, im.height, im.data);
    UnloadImage(im);
    return o;
}

#endif
