#include "sdl-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_SDL3)

#define SDL_MAX_VERTICES (1 << 22)

void sdl_ren_kill(sdl_ren* r)
{
    sdl_tex* tx;
    for (tx = r->texs; tx; tx = tx->next) {
        if (tx->t)
            SDL_DestroyTexture(tx->t);
        tx->t = NULL;
        tx->ren = NULL;
    }
    r->texs = NULL;
    if (r->r)
        SDL_DestroyRenderer(r->r);
    r->r = NULL;
    if (r->win)
        r->win->ren = NULL;
    r->win = NULL;
}

static void sdl_ren_dispose(void* p)
{
    sdl_ren* r = p;
    JSValue held = r->win_obj;
    sdl_ren_kill(r);
    r->win_obj = JS_UNDEFINED;
    JS_FreeValueRT(r->rt, held);
    dyn_nat_free(r);
}

void sdl_ren_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark)
{
    DynResource* res = JS_GetOpaque(val, sdl_ren_cid);
    if (res && res->native)
        JS_MarkValue(rt, ((sdl_ren*)res->native)->win_obj, mark);
}

sdl_ren* sdl_ren_of(JSContext* ctx, JSValueConst v)
{
    sdl_ren* r = dyn_res_native(ctx, v, sdl_ren_cid);
    if (r && !r->r) {
        JS_ThrowTypeError(ctx, "dyna:sdl: the renderer's window was closed");
        return NULL;
    }
    return r;
}

static const char* const sdl_present_names[] = { "disabled", "stretch", "letterbox", "overscan", "integer" };

static const char* const sdl_flip_names[] = { "none", "horizontal", "vertical", "both" };

JSValue sdl_ren_debug_text(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    float x, y;
    const char* s;
    sdl_ren* r;
    bool ok;
    if (argc < 3)
        return JS_ThrowTypeError(ctx, "dyna:sdl: debugText(x, y, text)");
    if (sdl_coord(ctx, argv[0], "debugText x", &x) || sdl_coord(ctx, argv[1], "debugText y", &y))
        return JS_EXCEPTION;
    r = sdl_ren_of(ctx, this_val);
    if (!r)
        return JS_EXCEPTION;
    if (sdl_text_arg(ctx, argv[2], "debugText(x, y, text)", &s))
        return JS_EXCEPTION;
    ok = SDL_RenderDebugText(r->r, x, y, s);
    JS_FreeCString(ctx, s);
    if (!ok)
        return sdl_throw(ctx, "SDL_RenderDebugText");
    return JS_UNDEFINED;
}

JSValue sdl_ren_logical(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    int w, h, mode = 2;
    sdl_ren* r;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "dyna:sdl: setLogicalPresentation(width, height, mode?)");
    if (sdl_int_in(ctx, argv[0], "setLogicalPresentation width", 0, SDL_MAX_DIM, &w)
        || sdl_int_in(ctx, argv[1], "setLogicalPresentation height", 0, SDL_MAX_DIM, &h))
        return JS_EXCEPTION;
    if (argc > 2 && !JS_IsUndefined(argv[2])
        && sdl_name_index(ctx, argv[2], "setLogicalPresentation mode", sdl_present_names, (int)countof(sdl_present_names), &mode))
        return JS_EXCEPTION;
    if (mode != 0 && (w == 0 || h == 0))
        return JS_ThrowRangeError(ctx, "dyna:sdl: setLogicalPresentation needs a width and height of at least 1 unless mode is \"disabled\"");
    r = sdl_ren_of(ctx, this_val);
    if (!r)
        return JS_EXCEPTION;
    if (!SDL_SetRenderLogicalPresentation(r->r, w, h, (SDL_RendererLogicalPresentation)mode))
        return sdl_throw(ctx, "SDL_SetRenderLogicalPresentation");
    return JS_UNDEFINED;
}

JSValue sdl_ren_coords(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    const char* who = magic ? "renderToWindow(x, y)" : "windowToRender(x, y)";
    float x, y, ox = 0, oy = 0;
    sdl_ren* r;
    bool ok;
    JSValue o;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "dyna:sdl: %s", who);
    if (sdl_coord(ctx, argv[0], who, &x) || sdl_coord(ctx, argv[1], who, &y))
        return JS_EXCEPTION;
    r = sdl_ren_of(ctx, this_val);
    if (!r)
        return JS_EXCEPTION;
    ok = magic ? SDL_RenderCoordinatesToWindow(r->r, x, y, &ox, &oy) : SDL_RenderCoordinatesFromWindow(r->r, x, y, &ox, &oy);
    if (!ok)
        return sdl_throw(ctx, who);
    o = JS_NewObject(ctx);
    if (JS_IsException(o))
        return o;
    if (JS_DefinePropertyValueStr(ctx, o, "x", JS_NewFloat64(ctx, (double)ox), JS_PROP_C_W_E) < 0
        || JS_DefinePropertyValueStr(ctx, o, "y", JS_NewFloat64(ctx, (double)oy), JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, o);
        return JS_EXCEPTION;
    }
    return o;
}

static const char* const sdl_draw_tex_keys[] = { "angle", "center", "flip" };

static int sdl_draw_tex_opts(JSContext* ctx, JSValueConst opts, double* angle, SDL_FPoint* center, int* has_center, SDL_FlipMode* flip)
{
    JSValue v;
    int idx = 0;
    *angle = 0;
    *has_center = 0;
    *flip = SDL_FLIP_NONE;
    if (sdl_check_opts(ctx, opts, "drawTexture", sdl_draw_tex_keys, (int)countof(sdl_draw_tex_keys)))
        return -1;
    v = JS_GetPropertyStr(ctx, opts, "angle");
    if (JS_IsException(v))
        return -1;
    if (!JS_IsUndefined(v)) {
        int rc = JS_ToFloat64(ctx, angle, v);
        JS_FreeValue(ctx, v);
        if (rc)
            return -1;
        if (!isfinite(*angle) || fabs(*angle) > 1e7) {
            JS_ThrowRangeError(ctx, "dyna:sdl: drawTexture angle must be a finite number of degrees within +-1e7");
            return -1;
        }
    }
    v = JS_GetPropertyStr(ctx, opts, "center");
    if (JS_IsException(v))
        return -1;
    if (!JS_IsUndefined(v)) {
        JSValue cx, cy;
        int rc;
        if (!JS_IsObject(v)) {
            JS_FreeValue(ctx, v);
            JS_ThrowTypeError(ctx, "dyna:sdl: drawTexture center must be { x, y }");
            return -1;
        }
        cx = JS_GetPropertyStr(ctx, v, "x");
        cy = JS_IsException(cx) ? JS_EXCEPTION : JS_GetPropertyStr(ctx, v, "y");
        JS_FreeValue(ctx, v);
        rc = JS_IsException(cx) || JS_IsException(cy) || sdl_coord(ctx, cx, "drawTexture center", &center->x)
            || sdl_coord(ctx, cy, "drawTexture center", &center->y);
        JS_FreeValue(ctx, cx);
        JS_FreeValue(ctx, cy);
        if (rc)
            return -1;
        *has_center = 1;
    }
    v = JS_GetPropertyStr(ctx, opts, "flip");
    if (JS_IsException(v))
        return -1;
    if (!JS_IsUndefined(v)) {
        int rc = sdl_name_index(ctx, v, "drawTexture flip", sdl_flip_names, (int)countof(sdl_flip_names), &idx);
        JS_FreeValue(ctx, v);
        if (rc)
            return -1;
        *flip = idx == 3 ? SDL_FLIP_HORIZONTAL_AND_VERTICAL : idx == 2 ? SDL_FLIP_VERTICAL
            : idx == 1                                                 ? SDL_FLIP_HORIZONTAL
                                                                       : SDL_FLIP_NONE;
    }
    return 0;
}

static const char* const sdl_renderer_keys[] = { "driver", "vsync" };

JSValue sdl_renderer_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv)
{
    JSValueConst opts = argc > 1 ? argv[1] : JS_UNDEFINED;
    char driver[64] = "";
    int vsync = 0;
    sdl_win* w;
    sdl_ren* r;
    JSValue obj;
    if (sdl_check_opts(ctx, opts, "Renderer", sdl_renderer_keys, (int)countof(sdl_renderer_keys)))
        return JS_EXCEPTION;
    if (JS_IsObject(opts)) {
        JSValue dv = JS_GetPropertyStr(ctx, opts, "driver");
        if (JS_IsException(dv))
            return JS_EXCEPTION;
        if (!JS_IsUndefined(dv)) {
            const char* s;
            if (!JS_IsString(dv)) {
                JS_FreeValue(ctx, dv);
                return JS_ThrowTypeError(ctx, "dyna:sdl: Renderer driver must be a string");
            }
            s = JS_ToCString(ctx, dv);
            JS_FreeValue(ctx, dv);
            if (!s)
                return JS_EXCEPTION;
            {
                size_t dl = strlen(s);
                if (dl >= sizeof(driver)) {
                    JS_FreeCString(ctx, s);
                    return JS_ThrowRangeError(ctx, "dyna:sdl: Renderer driver name is too long");
                }
                memcpy(driver, s, dl + 1);
            }
            JS_FreeCString(ctx, s);
        }
        if (sdl_opt_bool(ctx, opts, "vsync", &vsync))
            return JS_EXCEPTION;
    }
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "dyna:sdl: new Renderer(window, opts?)");
    w = sdl_win_of(ctx, argv[0]);
    if (!w)
        return JS_EXCEPTION;
    if (w->ren)
        return JS_ThrowTypeError(ctx, "dyna:sdl: this window already has a Renderer (SDL allows one per window)");
    r = dyn_nat_calloc(1, sizeof(*r));
    if (!r)
        return JS_ThrowOutOfMemory(ctx);
    r->rt = JS_GetRuntime(ctx);
    r->win_obj = JS_UNDEFINED;
    r->r = SDL_CreateRenderer(w->w, driver[0] ? driver : NULL);
    if (!r->r) {
        dyn_nat_free(r);
        return sdl_throw(ctx, "SDL_CreateRenderer");
    }
    if (vsync)
        SDL_SetRenderVSync(r->r, 1);
    r->win = w;
    w->ren = r;
    r->win_obj = JS_DupValue(ctx, argv[0]);
    obj = dyn_res_wrap(ctx, new_target, sdl_ren_cid, r, sdl_ren_dispose);
    return obj;
}

static int sdl_color(JSContext* ctx, int argc, JSValueConst* argv, Uint8 c[4])
{
    int i, v;
    if (argc < 3)
        return JS_ThrowTypeError(ctx, "dyna:sdl: a colour is (r, g, b, a = 255)"), -1;
    for (i = 0; i < 4; i++) {
        if (i == 3 && (argc < 4 || JS_IsUndefined(argv[3]))) {
            c[3] = 255;
            break;
        }
        if (sdl_int_in(ctx, argv[i], "a colour channel", 0, 255, &v))
            return -1;
        c[i] = (Uint8)v;
    }
    return 0;
}

JSValue sdl_ren_color(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    Uint8 c[4];
    sdl_ren* r;
    if (sdl_color(ctx, argc, argv, c))
        return JS_EXCEPTION;
    r = sdl_ren_of(ctx, this_val);
    if (!r)
        return JS_EXCEPTION;
    if (!SDL_SetRenderDrawColor(r->r, c[0], c[1], c[2], c[3]))
        return sdl_throw(ctx, "SDL_SetRenderDrawColor");
    return JS_UNDEFINED;
}

JSValue sdl_ren_simple(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    sdl_ren* r = sdl_ren_of(ctx, this_val);
    bool ok;
    (void)argc;
    (void)argv;
    if (!r)
        return JS_EXCEPTION;
    ok = magic == 0 ? SDL_RenderClear(r->r) : SDL_RenderPresent(r->r);
    if (!ok)
        return sdl_throw(ctx, magic == 0 ? "SDL_RenderClear" : "SDL_RenderPresent");
    return JS_UNDEFINED;
}

JSValue sdl_ren_draw(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    static const int need[] = { 4, 4, 4, 2 };
    static const char* const names[] = { "fillRect(x, y, width, height)", "drawRect(x, y, width, height)",
        "drawLine(x1, y1, x2, y2)", "drawPoint(x, y)" };
    float f[4];
    int i;
    sdl_ren* r;
    bool ok = false;
    if (argc < need[magic])
        return JS_ThrowTypeError(ctx, "dyna:sdl: %s", names[magic]);
    for (i = 0; i < need[magic]; i++)
        if (sdl_coord(ctx, argv[i], names[magic], &f[i]))
            return JS_EXCEPTION;
    r = sdl_ren_of(ctx, this_val);
    if (!r)
        return JS_EXCEPTION;
    switch (magic) {
    case 0:
    case 1: {
        SDL_FRect rc = { f[0], f[1], f[2], f[3] };
        ok = magic == 0 ? SDL_RenderFillRect(r->r, &rc) : SDL_RenderRect(r->r, &rc);
        break;
    }
    case 2:
        ok = SDL_RenderLine(r->r, f[0], f[1], f[2], f[3]);
        break;
    case 3:
        ok = SDL_RenderPoint(r->r, f[0], f[1]);
        break;
    }
    if (!ok)
        return sdl_throw(ctx, names[magic]);
    return JS_UNDEFINED;
}

JSValue sdl_ren_read_pixels(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    float f[4];
    int have, row;
    SDL_Rect rc;
    SDL_Surface *s, *c;
    sdl_ren* r;
    uint8_t* buf;
    size_t len;
    JSValue o, u8;
    if (sdl_rect_arg(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, "readPixels(rect)", f, &have))
        return JS_EXCEPTION;
    r = sdl_ren_of(ctx, this_val);
    if (!r)
        return JS_EXCEPTION;
    rc.x = (int)f[0];
    rc.y = (int)f[1];
    rc.w = (int)f[2];
    rc.h = (int)f[3];
    s = SDL_RenderReadPixels(r->r, have ? &rc : NULL);
    if (!s)
        return sdl_throw(ctx, "SDL_RenderReadPixels");
    c = SDL_ConvertSurface(s, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(s);
    if (!c)
        return sdl_throw(ctx, "SDL_ConvertSurface");
    len = (size_t)c->w * (size_t)c->h * 4;
    buf = dyn_nat_malloc(len ? len : 1);
    if (!buf) {
        SDL_DestroySurface(c);
        return JS_ThrowOutOfMemory(ctx);
    }
    for (row = 0; row < c->h; row++)
        memcpy(buf + (size_t)row * (size_t)c->w * 4, (const uint8_t*)c->pixels + (size_t)row * (size_t)c->pitch, (size_t)c->w * 4);
    o = JS_NewObject(ctx);
    u8 = dyn_nat_u8_copy(ctx, buf, len);
    dyn_nat_free(buf);
    if (JS_IsException(o) || JS_IsException(u8)
        || sdl_set_int(ctx, o, "width", c->w) < 0 || sdl_set_int(ctx, o, "height", c->h) < 0) {
        SDL_DestroySurface(c);
        JS_FreeValue(ctx, o);
        JS_FreeValue(ctx, u8);
        return JS_EXCEPTION;
    }
    SDL_DestroySurface(c);
    if (JS_DefinePropertyValueStr(ctx, o, "pixels", u8, JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, o);
        return JS_EXCEPTION;
    }
    return o;
}

JSValue sdl_ren_get(JSContext* ctx, JSValueConst this_val, int magic)
{
    sdl_ren* r = sdl_ren_of(ctx, this_val);
    int w = 0, h = 0;
    JSValue o;
    if (!r)
        return JS_EXCEPTION;
    if (magic == 0) {
        const char* s = SDL_GetRendererName(r->r);
        return JS_NewString(ctx, s ? s : "");
    }
    if (!SDL_GetCurrentRenderOutputSize(r->r, &w, &h))
        return sdl_throw(ctx, "SDL_GetCurrentRenderOutputSize");
    o = JS_NewObject(ctx);
    if (JS_IsException(o))
        return o;
    if (sdl_set_int(ctx, o, "width", w) < 0 || sdl_set_int(ctx, o, "height", h) < 0) {
        JS_FreeValue(ctx, o);
        return JS_EXCEPTION;
    }
    return o;
}

JSValue sdl_ren_draw_texture(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    float sf[4], df[4];
    int hs, hd, has_opts = argc > 3 && !JS_IsUndefined(argv[3]), has_center = 0;
    double angle = 0;
    SDL_FPoint center = { 0, 0 };
    SDL_FlipMode flip = SDL_FLIP_NONE;
    sdl_ren* r;
    sdl_tex* t;
    SDL_FRect src, dst;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "dyna:sdl: drawTexture(texture, src?, dst?, { angle?, center?, flip? })");
    if (sdl_rect_arg(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, "drawTexture src", sf, &hs)
        || sdl_rect_arg(ctx, argc > 2 ? argv[2] : JS_UNDEFINED, "drawTexture dst", df, &hd)
        || (has_opts && sdl_draw_tex_opts(ctx, argv[3], &angle, &center, &has_center, &flip)))
        return JS_EXCEPTION;
    r = sdl_ren_of(ctx, this_val);
    if (!r)
        return JS_EXCEPTION;
    t = sdl_tex_of(ctx, argv[0]);
    if (!t)
        return JS_EXCEPTION;
    if (t->ren != r)
        return JS_ThrowTypeError(ctx, "dyna:sdl: drawTexture: the texture belongs to another renderer");
    src = (SDL_FRect) { sf[0], sf[1], sf[2], sf[3] };
    dst = (SDL_FRect) { df[0], df[1], df[2], df[3] };
    if (has_opts) {
        if (!SDL_RenderTextureRotated(r->r, t->t, hs ? &src : NULL, hd ? &dst : NULL, angle, has_center ? &center : NULL, flip))
            return sdl_throw(ctx, "SDL_RenderTextureRotated");
        return JS_UNDEFINED;
    }
    if (!SDL_RenderTexture(r->r, t->t, hs ? &src : NULL, hd ? &dst : NULL))
        return sdl_throw(ctx, "SDL_RenderTexture");
    return JS_UNDEFINED;
}

static int sdl_typed_view(JSContext* ctx, JSValueConst v, JSClassID cid, size_t elem, const char* what, void** p, size_t* n)
{
    size_t off, blen, bpe, ab;
    JSValue buf;
    uint8_t* base;
    if (JS_GetClassID(v) != cid) {
        JS_ThrowTypeError(ctx, "dyna:sdl: %s", what);
        return -1;
    }
    buf = JS_GetTypedArrayBuffer(ctx, v, &off, &blen, &bpe);
    if (JS_IsException(buf))
        return -1;
    base = JS_GetArrayBuffer(ctx, &ab, buf);
    JS_FreeValue(ctx, buf);
    if (!base)
        return -1;
    if (off > ab || blen > ab - off) {
        JS_ThrowRangeError(ctx, "dyna:sdl: %s: the view lies outside its buffer", what);
        return -1;
    }
    *p = base + off;
    *n = blen / elem;
    return 0;
}

JSValue sdl_ren_set_target(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    JSValueConst v = argc > 0 ? argv[0] : JS_UNDEFINED;
    sdl_tex* t = NULL;
    sdl_ren* r;
    if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
        t = sdl_tex_of(ctx, v);
        if (!t)
            return JS_EXCEPTION;
        if (!t->target)
            return JS_ThrowTypeError(ctx, "dyna:sdl: setTarget: the texture was not created with { target: true }");
    }
    r = sdl_ren_of(ctx, this_val);
    if (!r)
        return JS_EXCEPTION;
    if (t && t->ren != r)
        return JS_ThrowTypeError(ctx, "dyna:sdl: setTarget: the texture belongs to another renderer");
    if (!SDL_SetRenderTarget(r->r, t ? t->t : NULL))
        return sdl_throw(ctx, "SDL_SetRenderTarget");
    return JS_UNDEFINED;
}

JSValue sdl_ren_rect_state(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    float f[4];
    int have;
    SDL_Rect rc;
    sdl_ren* r;
    bool ok;
    if (sdl_rect_arg(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, magic ? "setViewport(rect)" : "setClipRect(rect)", f, &have))
        return JS_EXCEPTION;
    r = sdl_ren_of(ctx, this_val);
    if (!r)
        return JS_EXCEPTION;
    rc = (SDL_Rect) { (int)f[0], (int)f[1], (int)f[2], (int)f[3] };
    ok = magic ? SDL_SetRenderViewport(r->r, have ? &rc : NULL) : SDL_SetRenderClipRect(r->r, have ? &rc : NULL);
    if (!ok)
        return sdl_throw(ctx, magic ? "SDL_SetRenderViewport" : "SDL_SetRenderClipRect");
    return JS_UNDEFINED;
}

JSValue sdl_ren_set_scale(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    double s[2];
    int i;
    sdl_ren* r;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "dyna:sdl: setScale(scaleX, scaleY)");
    for (i = 0; i < 2; i++) {
        if (JS_ToFloat64(ctx, &s[i], argv[i]))
            return JS_EXCEPTION;
        if (!(s[i] > 0 && s[i] <= 1024))
            return JS_ThrowRangeError(ctx, "dyna:sdl: setScale factors must be in (0, 1024]");
    }
    r = sdl_ren_of(ctx, this_val);
    if (!r)
        return JS_EXCEPTION;
    if (!SDL_SetRenderScale(r->r, (float)s[0], (float)s[1]))
        return sdl_throw(ctx, "SDL_SetRenderScale");
    return JS_UNDEFINED;
}

JSValue sdl_ren_set_blend(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    SDL_BlendMode bm = SDL_BLENDMODE_NONE;
    sdl_ren* r;
    if (sdl_blend_arg(ctx, argc, argv, "setBlendMode", &bm))
        return JS_EXCEPTION;
    r = sdl_ren_of(ctx, this_val);
    if (!r)
        return JS_EXCEPTION;
    if (!SDL_SetRenderDrawBlendMode(r->r, bm))
        return sdl_throw(ctx, "SDL_SetRenderDrawBlendMode");
    return JS_UNDEFINED;
}

JSValue sdl_ren_set_vsync(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    sdl_ren* r;
    int on;
    if (argc < 1 || !JS_IsBool(argv[0]))
        return JS_ThrowTypeError(ctx, "dyna:sdl: setVSync(on) needs a boolean");
    on = JS_ToBool(ctx, argv[0]);
    r = sdl_ren_of(ctx, this_val);
    if (!r)
        return JS_EXCEPTION;
    if (!SDL_SetRenderVSync(r->r, on ? 1 : SDL_RENDERER_VSYNC_DISABLED))
        return sdl_throw(ctx, "SDL_SetRenderVSync");
    return JS_UNDEFINED;
}

JSValue sdl_ren_geometry(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    float* v = NULL;
    int32_t* idx = NULL;
    size_t nf = 0, ni = 0, nv, i, count;
    sdl_ren* r;
    sdl_tex* t = NULL;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "dyna:sdl: geometry(vertices, indices?, texture?)");
    if (sdl_typed_view(ctx, argv[0], sdl_f32_cid, 4, "geometry vertices must be a Float32Array of x, y, r, g, b, a, u, v per vertex", (void**)&v, &nf))
        return JS_EXCEPTION;
    if (nf % 8)
        return JS_ThrowRangeError(ctx, "dyna:sdl: geometry vertices hold 8 floats per vertex, got %zu floats", nf);
    nv = nf / 8;
    if (nv > SDL_MAX_VERTICES)
        return JS_ThrowRangeError(ctx, "dyna:sdl: geometry takes at most %d vertices", SDL_MAX_VERTICES);
    for (i = 0; i < nf; i++)
        if (!isfinite(v[i]) || fabsf(v[i]) > 1e7f)
            return JS_ThrowRangeError(ctx, "dyna:sdl: geometry vertex %zu holds a value that is not a finite number within +-1e7", i / 8);
    if (argc > 1 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
        if (sdl_typed_view(ctx, argv[1], sdl_i32_cid, 4, "geometry indices must be an Int32Array", (void**)&idx, &ni))
            return JS_EXCEPTION;
        if (ni > (size_t)SDL_MAX_VERTICES * 3)
            return JS_ThrowRangeError(ctx, "dyna:sdl: geometry takes at most %d indices", SDL_MAX_VERTICES * 3);
        for (i = 0; i < ni; i++)
            if (idx[i] < 0 || (size_t)idx[i] >= nv)
                return JS_ThrowRangeError(ctx, "dyna:sdl: geometry index %zu is %d, outside the %zu vertices", i, (int)idx[i], nv);
    }
    count = idx ? ni : nv;
    if (count % 3)
        return JS_ThrowRangeError(ctx, "dyna:sdl: geometry draws triangles, so the %s count must be a multiple of 3", idx ? "index" : "vertex");
    if (argc > 2 && !JS_IsUndefined(argv[2]) && !JS_IsNull(argv[2])) {
        t = sdl_tex_of(ctx, argv[2]);
        if (!t)
            return JS_EXCEPTION;
    }
    r = sdl_ren_of(ctx, this_val);
    if (!r)
        return JS_EXCEPTION;
    if (t && t->ren != r)
        return JS_ThrowTypeError(ctx, "dyna:sdl: geometry: the texture belongs to another renderer");
    if (!count)
        return JS_UNDEFINED;
    if (!SDL_RenderGeometryRaw(r->r, t ? t->t : NULL, v, 32, (const SDL_FColor*)(void*)(v + 2), 32, v + 6, 32,
            (int)nv, idx, idx ? (int)ni : 0, 4))
        return sdl_throw(ctx, "SDL_RenderGeometryRaw");
    return JS_UNDEFINED;
}

int dyn_sdl_renderer_parts(JSContext* ctx, JSValueConst v, void** renderer, void** window)
{
    sdl_ren* r;
    if (JS_GetClassID(v) != sdl_ren_cid)
        return 0;
    r = sdl_ren_of(ctx, v);
    if (!r)
        return -1;
    if (!r->win || !r->win->w) {
        JS_ThrowTypeError(ctx, "dyna:sdl: the renderer's window was closed");
        return -1;
    }
    *renderer = r->r;
    *window = r->win->w;
    return 1;
}

int dyn_sdl_renderer_alive(JSValueConst v)
{
    DynResource* res = JS_GetOpaque(v, sdl_ren_cid);
    sdl_ren* r = res && !res->closed ? res->native : NULL;
    return r && r->r && r->win && r->win->w;
}

#endif
