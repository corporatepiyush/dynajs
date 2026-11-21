#include "sdl-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_SDL3)

#define SDL_MAX_WAV_BYTES (1 << 28)

static void sdl_tex_dispose(void* p)
{
    sdl_tex* t = p;
    JSValue held = t->ren_obj;
    if (t->ren) {
        sdl_tex** pp = &t->ren->texs;
        while (*pp && *pp != t)
            pp = &(*pp)->next;
        if (*pp)
            *pp = t->next;
    }
    if (t->t)
        SDL_DestroyTexture(t->t);
    t->t = NULL;
    t->ren = NULL;
    t->ren_obj = JS_UNDEFINED;
    JS_FreeValueRT(t->rt, held);
    dyn_nat_free(t);
}

void sdl_tex_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark)
{
    DynResource* res = JS_GetOpaque(val, sdl_tex_cid);
    if (res && res->native)
        JS_MarkValue(rt, ((sdl_tex*)res->native)->ren_obj, mark);
}

sdl_tex* sdl_tex_of(JSContext* ctx, JSValueConst v)
{
    sdl_tex* t = dyn_res_native(ctx, v, sdl_tex_cid);
    if (t && !t->t) {
        JS_ThrowTypeError(ctx, "dyna:sdl: the texture's renderer was closed");
        return NULL;
    }
    return t;
}

static const char* const sdl_texture_keys[] = { "target" };

JSValue sdl_texture_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv)
{
    JSValueConst opts = argc > 3 ? argv[3] : JS_UNDEFINED;
    int w, h, target = 0;
    sdl_ren* r;
    sdl_tex* t;
    if (argc < 3)
        return JS_ThrowTypeError(ctx, "dyna:sdl: new Texture(renderer, width, height, opts?)");
    if (sdl_dim(ctx, argv[1], "Texture width", &w) || sdl_dim(ctx, argv[2], "Texture height", &h))
        return JS_EXCEPTION;
    if (sdl_check_opts(ctx, opts, "Texture", sdl_texture_keys, (int)countof(sdl_texture_keys))
        || sdl_opt_bool(ctx, opts, "target", &target))
        return JS_EXCEPTION;
    r = sdl_ren_of(ctx, argv[0]);
    if (!r)
        return JS_EXCEPTION;
    t = dyn_nat_calloc(1, sizeof(*t));
    if (!t)
        return JS_ThrowOutOfMemory(ctx);
    t->rt = JS_GetRuntime(ctx);
    t->ren_obj = JS_UNDEFINED;
    t->t = SDL_CreateTexture(r->r, SDL_PIXELFORMAT_RGBA32, target ? SDL_TEXTUREACCESS_TARGET : SDL_TEXTUREACCESS_STREAMING, w, h);
    if (!t->t) {
        dyn_nat_free(t);
        return sdl_throw(ctx, "SDL_CreateTexture");
    }
    SDL_SetTextureBlendMode(t->t, SDL_BLENDMODE_BLEND);
    t->w = w;
    t->h = h;
    t->target = target;
    t->ren = r;
    t->next = r->texs;
    r->texs = t;
    t->ren_obj = JS_DupValue(ctx, argv[0]);
    return dyn_res_wrap(ctx, new_target, sdl_tex_cid, t, sdl_tex_dispose);
}

JSValue sdl_tex_get(JSContext* ctx, JSValueConst this_val, int magic)
{
    sdl_tex* t = sdl_tex_of(ctx, this_val);
    if (!t)
        return JS_EXCEPTION;
    if (magic == 2)
        return JS_NewBool(ctx, t->target);
    return JS_NewInt32(ctx, magic ? t->h : t->w);
}

JSValue sdl_tex_update(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    float f[4];
    int have;
    SDL_Rect rc;
    size_t off, blen, bpe, ab, need;
    JSValue buf;
    uint8_t* base;
    sdl_tex* t;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "dyna:sdl: update(pixels, rect?) needs an RGBA Uint8Array");
    if (sdl_rect_arg(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, "update rect", f, &have))
        return JS_EXCEPTION;
    t = sdl_tex_of(ctx, this_val);
    if (!t)
        return JS_EXCEPTION;
    rc.x = have ? (int)f[0] : 0;
    rc.y = have ? (int)f[1] : 0;
    rc.w = have ? (int)f[2] : t->w;
    rc.h = have ? (int)f[3] : t->h;
    if (rc.w < 1 || rc.h < 1 || rc.x < 0 || rc.y < 0 || rc.x > t->w - rc.w || rc.y > t->h - rc.h)
        return JS_ThrowRangeError(ctx, "dyna:sdl: update rect must lie inside the %dx%d texture", t->w, t->h);
    buf = JS_GetTypedArrayBuffer(ctx, argv[0], &off, &blen, &bpe);
    if (JS_IsException(buf))
        return JS_EXCEPTION;
    base = JS_GetArrayBuffer(ctx, &ab, buf);
    JS_FreeValue(ctx, buf);
    if (!base)
        return JS_EXCEPTION;
    if (bpe != 1)
        return JS_ThrowTypeError(ctx, "dyna:sdl: update pixels must be a byte view (Uint8Array) of RGBA");
    need = (size_t)rc.w * (size_t)rc.h * 4;
    if (off > ab || blen > ab - off || blen < need)
        return JS_ThrowRangeError(ctx, "dyna:sdl: update needs %zu bytes of RGBA for a %dx%d area, got %zu", need, rc.w, rc.h, blen);
    if (!SDL_UpdateTexture(t->t, &rc, base + off, rc.w * 4))
        return sdl_throw(ctx, "SDL_UpdateTexture");
    return JS_UNDEFINED;
}

JSValue sdl_tex_mod(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    sdl_tex* t;
    int v[3] = { 0, 0, 0 }, i, n = magic == 1 ? 3 : 1;
    bool ok = false;
    SDL_BlendMode bm = SDL_BLENDMODE_NONE;
    if (magic == 2) {
        if (sdl_blend_arg(ctx, argc, argv, "setBlendMode", &bm))
            return JS_EXCEPTION;
    } else {
        if (argc < n)
            return JS_ThrowTypeError(ctx, magic ? "dyna:sdl: setColorMod(r, g, b)" : "dyna:sdl: setAlphaMod(alpha)");
        for (i = 0; i < n; i++)
            if (sdl_int_in(ctx, argv[i], "a modulation channel", 0, 255, &v[i]))
                return JS_EXCEPTION;
    }
    t = sdl_tex_of(ctx, this_val);
    if (!t)
        return JS_EXCEPTION;
    if (magic == 0)
        ok = SDL_SetTextureAlphaMod(t->t, (Uint8)v[0]);
    else if (magic == 1)
        ok = SDL_SetTextureColorMod(t->t, (Uint8)v[0], (Uint8)v[1], (Uint8)v[2]);
    else
        ok = SDL_SetTextureBlendMode(t->t, bm);
    if (!ok)
        return sdl_throw(ctx, "a texture modulation");
    return JS_UNDEFINED;
}

static int sdl_path_arg(JSContext* ctx, JSValueConst v, const char* who, const char** out)
{
    size_t len;
    const char* s;
    if (!JS_IsString(v)) {
        JS_ThrowTypeError(ctx, "dyna:sdl: %s needs a path string", who);
        return -1;
    }
    s = JS_ToCStringLen(ctx, &len, v);
    if (!s)
        return -1;
    if (strlen(s) != len) {
        JS_FreeCString(ctx, s);
        JS_ThrowTypeError(ctx, "dyna:sdl: %s: the path holds a NUL character", who);
        return -1;
    }
    *out = s;
    return 0;
}

JSValue sdl_load_texture(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    const char* path;
    SDL_Surface *s, *c;
    sdl_ren* r;
    sdl_tex* t;
    (void)this_val;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "dyna:sdl: loadTexture(renderer, path)");
    if (sdl_path_arg(ctx, argv[1], "loadTexture(renderer, path)", &path))
        return JS_EXCEPTION;
    r = sdl_ren_of(ctx, argv[0]);
    if (!r) {
        JS_FreeCString(ctx, path);
        return JS_EXCEPTION;
    }
    s = SDL_LoadSurface(path);
    JS_FreeCString(ctx, path);
    if (!s)
        return sdl_throw(ctx, "SDL_LoadSurface");
    if (s->w < 1 || s->h < 1 || s->w > SDL_MAX_DIM || s->h > SDL_MAX_DIM) {
        int sw = s->w, sh = s->h;
        SDL_DestroySurface(s);
        return JS_ThrowRangeError(ctx, "dyna:sdl: loadTexture: a %dx%d image is outside 1..%d per side", sw, sh, SDL_MAX_DIM);
    }
    c = SDL_ConvertSurface(s, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(s);
    if (!c)
        return sdl_throw(ctx, "SDL_ConvertSurface");
    t = dyn_nat_calloc(1, sizeof(*t));
    if (!t) {
        SDL_DestroySurface(c);
        return JS_ThrowOutOfMemory(ctx);
    }
    t->rt = JS_GetRuntime(ctx);
    t->ren_obj = JS_UNDEFINED;
    t->t = SDL_CreateTexture(r->r, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, c->w, c->h);
    if (!t->t) {
        SDL_DestroySurface(c);
        dyn_nat_free(t);
        return sdl_throw(ctx, "SDL_CreateTexture");
    }
    if (!SDL_UpdateTexture(t->t, NULL, c->pixels, c->pitch)) {
        SDL_DestroyTexture(t->t);
        SDL_DestroySurface(c);
        dyn_nat_free(t);
        return sdl_throw(ctx, "SDL_UpdateTexture");
    }
    SDL_SetTextureBlendMode(t->t, SDL_BLENDMODE_BLEND);
    t->w = c->w;
    t->h = c->h;
    SDL_DestroySurface(c);
    t->ren = r;
    t->next = r->texs;
    r->texs = t;
    t->ren_obj = JS_DupValue(ctx, argv[0]);
    return dyn_res_wrap(ctx, JS_UNDEFINED, sdl_tex_cid, t, sdl_tex_dispose);
}

JSValue sdl_load_wav(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    const char* path;
    SDL_AudioSpec src, dst;
    Uint8 *raw = NULL, *cvt = NULL;
    Uint32 rawlen = 0;
    int cvtlen = 0;
    JSValue o, samples;
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "dyna:sdl: loadWAV(path)");
    if (sdl_path_arg(ctx, argv[0], "loadWAV(path)", &path))
        return JS_EXCEPTION;
    if (!SDL_LoadWAV(path, &src, &raw, &rawlen)) {
        JS_FreeCString(ctx, path);
        return sdl_throw(ctx, "SDL_LoadWAV");
    }
    JS_FreeCString(ctx, path);
    if (rawlen > SDL_MAX_WAV_BYTES) {
        SDL_free(raw);
        return JS_ThrowRangeError(ctx, "dyna:sdl: loadWAV: the decoded audio exceeds %d bytes", SDL_MAX_WAV_BYTES);
    }
    dst = src;
    dst.format = SDL_AUDIO_F32;
    if (!SDL_ConvertAudioSamples(&src, raw, (int)rawlen, &dst, &cvt, &cvtlen)) {
        SDL_free(raw);
        return sdl_throw(ctx, "SDL_ConvertAudioSamples");
    }
    SDL_free(raw);
    samples = sdl_samples_array(ctx, cvt, (size_t)cvtlen - (size_t)cvtlen % 4, 1);
    SDL_free(cvt);
    if (JS_IsException(samples))
        return samples;
    o = JS_NewObject(ctx);
    if (JS_IsException(o) || sdl_set_int(ctx, o, "freq", src.freq) < 0 || sdl_set_int(ctx, o, "channels", src.channels) < 0) {
        JS_FreeValue(ctx, o);
        JS_FreeValue(ctx, samples);
        return JS_EXCEPTION;
    }
    if (JS_DefinePropertyValueStr(ctx, o, "samples", samples, JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, o);
        return JS_EXCEPTION;
    }
    return o;
}

#endif
