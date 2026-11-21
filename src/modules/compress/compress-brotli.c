#include "compress-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_COMPRESS)

#if DYN_HAVE_BROTLI
int dyn_brotli_decode(const uint8_t* src, size_t len, size_t cap,
    dyn_outbuf_t* o, const char** pwhy)
{
#if defined(__APPLE__)
    compression_stream s;
    uint8_t chunk[65536] DYN_IO_SCRATCH;
    int err = 0;

    if (compression_stream_init(&s, COMPRESSION_STREAM_DECODE,
            COMPRESSION_BROTLI)
        != COMPRESSION_STATUS_OK) {
        *pwhy = "brotli init failed";
        return -1;
    }
    s.src_ptr = src;
    s.src_size = len;
    for (;;) {
        size_t produced;
        compression_status st;
        s.dst_ptr = chunk;
        s.dst_size = sizeof chunk;
        st = compression_stream_process(&s, COMPRESSION_STREAM_FINALIZE);
        produced = sizeof chunk - s.dst_size;
        if (produced) {
            if (dyn_out_grow(o, o->len + produced, cap) < 0) {
                *pwhy = "output exceeds cap";
                err = 1;
                break;
            }
            memcpy(o->buf + o->len, chunk, produced);
            o->len += produced;
        }
        if (st == COMPRESSION_STATUS_END) {
            if (s.src_size != 0) {
                *pwhy = "bytes follow the end of the brotli stream";
                err = 1;
            }
            break;
        }
        if (st == COMPRESSION_STATUS_ERROR) {
            *pwhy = "malformed brotli stream";
            err = 1;
            break;
        }
        if (st == COMPRESSION_STATUS_OK && s.src_size == 0 && s.dst_size != 0) {
            *pwhy = "truncated brotli stream";
            err = 1;
            break;
        }
    }
    compression_stream_destroy(&s);
    return err ? -1 : 0;
#else
    BrotliDecoderState* st = BrotliDecoderCreateInstance(NULL, NULL, NULL);
    const uint8_t* in = src;
    size_t avail_in = len;
    int err = 0;

    if (!st) {
        *pwhy = "out of memory";
        return -1;
    }
    for (;;) {
        uint8_t* dst;
        size_t avail_out;
        BrotliDecoderResult r;
        if (o->len == o->cap) {
            size_t want = cap - o->len > 65536 ? o->len + 65536 : cap + 1;
            if (o->len > cap || dyn_out_grow(o, want, cap + 1) < 0) {
                o->exceeded = 1;
                *pwhy = "output exceeds cap";
                err = 1;
                break;
            }
        }
        dst = o->buf + o->len;
        avail_out = o->cap - o->len;
        r = BrotliDecoderDecompressStream(st, &avail_in, &in, &avail_out,
            &dst, NULL);
        o->len = (size_t)(dst - o->buf);
        if (r == BROTLI_DECODER_RESULT_SUCCESS) {
            if (avail_in != 0) {
                *pwhy = "bytes follow the end of the brotli stream";
                err = 1;
            } else if (o->len > cap) {
                o->exceeded = 1;
                *pwhy = "output exceeds cap";
                err = 1;
            }
            break;
        }
        if (r == BROTLI_DECODER_RESULT_ERROR) {
            *pwhy = "malformed brotli stream";
            err = 1;
            break;
        }
        if (r == BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT && avail_in == 0) {
            *pwhy = "truncated brotli stream";
            err = 1;
            break;
        }
    }
    BrotliDecoderDestroyInstance(st);
    return err ? -1 : 0;
#endif
}

int dyn_brotli_encode(const uint8_t* src, size_t len, int level,
    uint8_t** pout, size_t* pout_len)
{
#if defined(__APPLE__)
    compression_stream s;
    uint8_t chunk[65536] DYN_IO_SCRATCH;
    dyn_outbuf_t o = { NULL, 0, 0 };
    int err = 0;

    (void)level;
    if (compression_stream_init(&s, COMPRESSION_STREAM_ENCODE,
            COMPRESSION_BROTLI)
        != COMPRESSION_STATUS_OK)
        return -1;
    s.src_ptr = src;
    s.src_size = len;
    for (;;) {
        size_t produced;
        compression_status st;
        s.dst_ptr = chunk;
        s.dst_size = sizeof chunk;
        st = compression_stream_process(&s, COMPRESSION_STREAM_FINALIZE);
        produced = sizeof chunk - s.dst_size;
        if (produced) {
            if (dyn_out_grow(&o, o.len + produced, (size_t)-1) < 0) {
                err = 1;
                break;
            }
            memcpy(o.buf + o.len, chunk, produced);
            o.len += produced;
        }
        if (st == COMPRESSION_STATUS_END)
            break;
        if (st == COMPRESSION_STATUS_ERROR) {
            err = 1;
            break;
        }
    }
    compression_stream_destroy(&s);
    if (err) {
        free(o.buf);
        return -1;
    }
    *pout = o.buf;
    *pout_len = o.len;
    return 0;
#else
    size_t bound = BrotliEncoderMaxCompressedSize(len);
    uint8_t* out;
    size_t olen;

    if (bound == 0)
        bound = 1;
    out = (uint8_t*)malloc(bound);
    if (!out)
        return -1;
    olen = bound;
    if (!BrotliEncoderCompress(level, BROTLI_DEFAULT_WINDOW,
            BROTLI_MODE_GENERIC, len, src, &olen, out)) {
        free(out);
        return -1;
    }
    *pout = out;
    *pout_len = olen;
    return 0;
#endif
}
#endif

JSValue dyn_brotli_js(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    (void)this_val;
#if DYN_HAVE_BROTLI
    uint8_t *src = NULL, *out = NULL;
    size_t src_len = 0, out_len = 0;
    int level = 5;
    JSValue result;

    if (argc > 1 && JS_IsObject(argv[1])) {
        if (dyn_opts_strict(ctx, argv[1], cpr_level_keys, 1))
            return JS_EXCEPTION;
        JSValue v = JS_GetPropertyStr(ctx, argv[1], "level");
        if (JS_IsException(v))
            return JS_EXCEPTION;
        if (!JS_IsUndefined(v)) {
            int32_t lv;
            if (JS_ToInt32(ctx, &lv, v)) {
                JS_FreeValue(ctx, v);
                return JS_EXCEPTION;
            }
            if (lv < 0 || lv > 11) {
                JS_FreeValue(ctx, v);
                return JS_ThrowRangeError(ctx, "brotli level must be 0..11");
            }
            level = (int)lv;
        }
        JS_FreeValue(ctx, v);
    }
    if (dyn_read_input(ctx, argv[0], &src, &src_len) < 0)
        return JS_EXCEPTION;
    if (DYN_OFFLOAD_CPU(dyn_brotli_encode(src, src_len, level, &out, &out_len)) < 0) {
        free(src);
        return JS_ThrowOutOfMemory(ctx);
    }
    free(src);
    result = dyn_bytes_to_uint8(ctx, out, out_len);
    free(out);
    return result;
#else
    (void)argc;
    (void)argv;
    return JS_ThrowTypeError(ctx,
        "dyna:compress brotli: support not compiled in");
#endif
}

#endif
