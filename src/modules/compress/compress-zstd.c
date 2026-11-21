#include "compress-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_COMPRESS)

static const char* const cpr_zstd_keys[] = { "level", "checksum" };

#if DYN_HAVE_ZSTD
static int dyn_zstd_frame_bounds(const uint8_t* src, size_t len, size_t cap,
    uint64_t* pwin, uint64_t* pfcs,
    const char** pwhy)
{
    uint64_t fcs;
    size_t p;
    int fsz, single, fcs_flag, did_flag;
    uint8_t desc;

    if (len < 6) {
        *pwhy = "truncated frame";
        return -1;
    }
    {
        uint32_t magic = (uint32_t)src[0] | ((uint32_t)src[1] << 8) | ((uint32_t)src[2] << 16) | ((uint32_t)src[3] << 24);
        if (magic != 0xFD2FB528u) {
            *pwhy = "bad magic";
            return -1;
        }
    }
    desc = src[4];
    fcs_flag = desc >> 6;
    single = (desc >> 5) & 1;
    did_flag = desc & 3;
    p = 5;
    if (single) {
        *pwin = 0;
    } else {
        uint64_t window;
        int expo, mant;
        if (p >= len) {
            *pwhy = "truncated frame";
            return -1;
        }
        {
            uint8_t wd = src[p++];
            expo = wd >> 3;
            mant = wd & 7;
        }
        window = (uint64_t)1 << (10 + expo);
        window += (window / 8) * (uint64_t)mant;
        if (window > cap) {
            *pwhy = "oversized window";
            return -1;
        }
        *pwin = window;
    }
    if (did_flag == 1) {
        if (p + 1 > len) {
            *pwhy = "truncated frame";
            return -1;
        }
        p += 1;
    } else if (did_flag == 2) {
        if (p + 2 > len) {
            *pwhy = "truncated frame";
            return -1;
        }
        p += 2;
    } else if (did_flag == 3) {
        if (p + 4 > len) {
            *pwhy = "truncated frame";
            return -1;
        }
        p += 4;
    }
    if (fcs_flag == 0)
        fsz = single ? 1 : 0;
    else if (fcs_flag == 1)
        fsz = 2;
    else if (fcs_flag == 2)
        fsz = 4;
    else
        fsz = 8;
    fcs = 0;
    if (fsz) {
        int i;
        if (p + (size_t)fsz > len) {
            *pwhy = "truncated frame";
            return -1;
        }
        for (i = 0; i < fsz; i++)
            fcs |= (uint64_t)src[p + i] << (8 * i);
        if (fsz == 2)
            fcs += 256;
        if (fcs > cap) {
            *pwhy = "declared output exceeds cap";
            return -1;
        }
    }
    *pfcs = fcs;
    return 0;
}

int dyn_zstd_decode(const uint8_t* src, size_t len, size_t cap,
    dyn_outbuf_t* o, const char** pwhy)
{
    ZSTD_DStream* ds;
    ZSTD_inBuffer in;
    uint64_t win, fcs2;
    int err = 0;

    if (dyn_zstd_frame_bounds(src, len, cap, &win, &fcs2, pwhy) < 0) {
        if (*pwhy && strstr(*pwhy, "exceeds cap"))
            o->exceeded = 1;
        return -1;
    }
    (void)win;
    (void)fcs2;
    ds = ZSTD_createDStream();
    if (!ds) {
        *pwhy = "out of memory";
        return -1;
    }
    ZSTD_initDStream(ds);
    in.src = src;
    in.size = len;
    in.pos = 0;
    for (;;) {
        ZSTD_outBuffer out;
        size_t r;
        if (o->len == o->cap) {
            size_t want = cap - o->len > 65536 ? o->len + 65536 : cap + 1;
            if (o->len > cap || dyn_out_grow(o, want, cap + 1) < 0) {
                o->exceeded = 1;
                *pwhy = "output exceeds cap";
                err = 1;
                break;
            }
        }
        out.dst = o->buf + o->len;
        out.size = o->cap - o->len;
        out.pos = 0;
        r = ZSTD_decompressStream(ds, &out, &in);
        o->len += out.pos;
        if (ZSTD_isError(r)) {
            *pwhy = ZSTD_getErrorName(r);
            err = 1;
            break;
        }
        if (r == 0) {
            if (in.pos == in.size)
                break;
            continue;
        }
        if (in.pos == in.size && out.pos == 0) {
            *pwhy = "truncated frame";
            err = 1;
            break;
        }
    }
    ZSTD_freeDStream(ds);
    if (!err && o->len > cap) {
        o->exceeded = 1;
        *pwhy = "output exceeds cap";
        err = 1;
    }
    return err ? -1 : 0;
}

int dyn_zstd_encode(const uint8_t* src, size_t len, int level, int checksum,
    uint8_t** pout, size_t* pout_len)
{
    size_t bound = ZSTD_compressBound(len);
    ZSTD_CCtx* zc;
    uint8_t* out;
    size_t r;

    out = (uint8_t*)malloc(bound);
    zc = out ? ZSTD_createCCtx() : NULL;
    if (!zc) {
        free(out);
        return -1;
    }
    ZSTD_CCtx_setParameter(zc, ZSTD_c_compressionLevel, level);
    ZSTD_CCtx_setParameter(zc, ZSTD_c_checksumFlag, checksum);
    r = ZSTD_compress2(zc, out, bound, src, len);
    ZSTD_freeCCtx(zc);
    if (ZSTD_isError(r)) {
        free(out);
        return -1;
    }
    *pout = out;
    *pout_len = r;
    return 0;
}
#endif

JSValue dyn_zstd_js(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    (void)this_val;
#if DYN_HAVE_ZSTD
    uint8_t *src = NULL, *out = NULL;
    size_t src_len = 0, out_len = 0;
    int level = 3, checksum = 1;
    JSValue result;

    if (argc > 1 && JS_IsObject(argv[1])) {
        if (dyn_opts_strict(ctx, argv[1], cpr_zstd_keys, 2))
            return JS_EXCEPTION;
        JSValue v = JS_GetPropertyStr(ctx, argv[1], "checksum");
        if (JS_IsException(v))
            return JS_EXCEPTION;
        if (!JS_IsUndefined(v)) {
            if (!JS_IsBool(v)) {
                JS_FreeValue(ctx, v);
                return JS_ThrowTypeError(ctx, "zstd checksum must be a boolean");
            }
            checksum = JS_ToBool(ctx, v);
        }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[1], "level");
        if (JS_IsException(v))
            return JS_EXCEPTION;
        if (!JS_IsUndefined(v)) {
            int32_t lv;
            if (JS_ToInt32(ctx, &lv, v)) {
                JS_FreeValue(ctx, v);
                return JS_EXCEPTION;
            }
            if (lv < 1 || lv > 22) {
                JS_FreeValue(ctx, v);
                return JS_ThrowRangeError(ctx, "zstd level must be 1..22");
            }
            level = (int)lv;
        }
        JS_FreeValue(ctx, v);
    }
    if (dyn_read_input(ctx, argv[0], &src, &src_len) < 0)
        return JS_EXCEPTION;
    if (DYN_OFFLOAD_CPU(dyn_zstd_encode(src, src_len, level, checksum, &out, &out_len)) < 0) {
        free(src);
        return JS_ThrowOutOfMemory(ctx);
    }
    free(src);
    result = dyn_bytes_to_uint8(ctx, out, out_len);
    free(out);
    return result;
#else
    return JS_ThrowTypeError(ctx,
        "dyna:compress zstd: support not compiled in (build with libzstd)");
#endif
}

JSValue dyn_unzstd_js(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    (void)this_val;
#if DYN_HAVE_ZSTD
    uint8_t* src = NULL;
    size_t src_len = 0;
    dyn_outbuf_t o = { NULL, 0, 0 };
    int as_string = 0;
    const char* why = NULL;
    JSValue result;

    if (argc > 1 && JS_IsObject(argv[1])) {
        if (dyn_opts_strict(ctx, argv[1], cpr_asstring_keys, 2))
            return JS_EXCEPTION;
        JSValue v = JS_GetPropertyStr(ctx, argv[1], "asString");
        if (JS_IsException(v))
            return JS_EXCEPTION;
        as_string = JS_ToBool(ctx, v);
        JS_FreeValue(ctx, v);
        if (as_string < 0)
            return JS_EXCEPTION;
    }
    if (dyn_cpr_maxout(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, &o) < 0)
        return JS_EXCEPTION;
    if (dyn_read_input(ctx, argv[0], &src, &src_len) < 0)
        return JS_EXCEPTION;
    if (DYN_OFFLOAD_CPU(dyn_zstd_decode(src, src_len, dyn_cpr_cap(&o), &o, &why)) < 0) {
        free(src);
        free(o.buf);
        if (o.exceeded && why && strstr(why, "declared"))
            return JS_ThrowRangeError(ctx,
                "dyna:compress unzstd: zstd: the frame's declared output "
                "exceeds maxOutputBytes (%llu)",
                (unsigned long long)dyn_cpr_cap(&o));
        if (o.exceeded)
            return dyn_cpr_cap_error(ctx, &o, "dyna:compress unzstd");
        return JS_ThrowTypeError(ctx, "dyna:compress unzstd: zstd: %s",
            why ? why : "invalid zstd data");
    }
    free(src);
    if (as_string)
        result = JS_NewStringLen(ctx, o.len ? (const char*)o.buf : "", o.len);
    else
        result = dyn_bytes_to_uint8(ctx, o.buf, o.len);
    free(o.buf);
    return result;
#else
    (void)argc;
    (void)argv;
    return JS_ThrowTypeError(ctx,
        "dyna:compress zstd: support not compiled in (build with libzstd)");
#endif
}

#endif
