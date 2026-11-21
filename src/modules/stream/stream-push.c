#include "stream-internal.h"
#include "dyna-libc.h"
#include "core/dyn-inflate-stream.h"
#include "core/dyn-hash.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_STREAM)

enum {
    PUSH_RAW,
    PUSH_GZIP,
    PUSH_ZLIB,
    PUSH_BROTLI,
    PUSH_ZSTD,
    PUSH_LZ4,
};

typedef struct {
    int encode;
    int kind;
    int ended;
    int failed;
    int closed;
    codec_enc_t e;
    codec_dec_t d;
    uint32_t adler;
    uint8_t hdr[2];
    int hdr_len;
    uint8_t tail[4];
    int tail_len;
    dyn_infs_t* zs;
    int inf;
    int zph, zbn;
    uint8_t zb[16];
    cbuf_t gh;
    uint32_t crc, isize;
} push_codec_t;

static JSClassID push_codec_class_id;

typedef struct {
    uint8_t* buf;
    size_t len;
} push_piece_t;

typedef struct {
    cbuf_t cat;
    push_piece_t* pv;
    int pn, pcap, split;
} push_out_t;

static void pout_free(push_out_t* o)
{
    int i;
    cbuf_free(&o->cat);
    for (i = 0; i < o->pn; i++)
        free(o->pv[i].buf);
    free(o->pv);
    o->pv = NULL;
    o->pn = o->pcap = 0;
}

static int pout_put(push_out_t* o, const uint8_t* q, size_t k, int more)
{
    uint8_t* b;
    if (!o->split) {
        if (!o->cat.cap) {
            size_t want = k < (size_t)1 << 20 ? k : (size_t)1 << 20;
            if (more && want < (size_t)1 << 18)
                want = (size_t)1 << 18;
            o->cat.buf = (uint8_t*)malloc(want);
            if (!o->cat.buf)
                return -1;
            o->cat.cap = want;
        }
        return cbuf_put(&o->cat, q, k);
    }
    if (o->pn == o->pcap) {
        int nc = o->pcap ? o->pcap * 2 : 4;
        push_piece_t* nv = (push_piece_t*)realloc(o->pv, (size_t)nc * sizeof(*nv));
        if (!nv)
            return -1;
        o->pv = nv;
        o->pcap = nc;
    }
    b = (uint8_t*)malloc(k);
    if (!b)
        return -1;
    memcpy(b, q, k);
    o->pv[o->pn].buf = b;
    o->pv[o->pn++].len = k;
    return 0;
}

static void push_codec_release(push_codec_t* pc)
{
    if (pc->closed)
        return;
    pc->closed = 1;
    if (pc->encode)
        codec_enc_free(&pc->e);
    else
        codec_dec_free(&pc->d);
    dyn_infs_free(pc->zs);
    pc->zs = NULL;
    cbuf_free(&pc->gh);
}

static void push_codec_finalizer(JSRuntime* rt, JSValue val)
{
    push_codec_t* pc = (push_codec_t*)JS_GetOpaque(val, push_codec_class_id);
    (void)rt;
    if (!pc)
        return;
    push_codec_release(pc);
    free(pc);
}

static JSClassDef push_codec_class = {
    "PushCodec",
    .finalizer = push_codec_finalizer,
};

static JSValue push_out(JSContext* ctx, cbuf_t* out)
{
    JSValue ab, u8;
    if (out->len == 0) {
        cbuf_free(out);
        return JS_UNDEFINED;
    }
    if (out->cap - out->len > out->len / 4 && out->cap > 4096) {
        uint8_t* nb = (uint8_t*)realloc(out->buf, out->len);
        if (nb) {
            out->buf = nb;
            out->cap = out->len;
        }
    }
    ab = dyn_nat_ab_take(ctx, out->buf, out->len);
    out->buf = NULL;
    out->len = out->cap = 0;
    if (JS_IsException(ab))
        return ab;
    {
        JSValueConst args[3];
        args[0] = ab;
        args[1] = JS_UNDEFINED;
        args[2] = JS_UNDEFINED;
        u8 = JS_NewTypedArray(ctx, 3, args, JS_TYPED_ARRAY_UINT8);
    }
    JS_FreeValue(ctx, ab);
    return u8;
}

static JSValue push_fail(JSContext* ctx, push_codec_t* pc, cbuf_t* out, const char* err)
{
    pc->failed = 1;
    cbuf_free(out);
    return JS_ThrowTypeError(ctx, "%s", err);
}

static push_codec_t* push_codec_of(JSContext* ctx, JSValueConst obj)
{
    push_codec_t* pc = (push_codec_t*)JS_GetOpaque2(ctx, obj, push_codec_class_id);
    if (!pc)
        return NULL;
    if (pc->closed) {
        JS_ThrowTypeError(ctx, "the codec is closed");
        return NULL;
    }
    if (pc->failed) {
        JS_ThrowTypeError(ctx, "the codec already failed");
        return NULL;
    }
    if (pc->ended) {
        JS_ThrowTypeError(ctx, "the codec already ended");
        return NULL;
    }
    return pc;
}

static int push_take_dec_out(push_codec_t* pc, cbuf_t* out)
{
    if (pc->d.out.len == 0)
        return 0;
    if (pc->kind == PUSH_ZLIB)
        pc->adler = dyn_adler32(pc->adler, pc->d.out.buf, pc->d.out.len);
    if (cbuf_put(out, pc->d.out.buf, pc->d.out.len) < 0)
        return -1;
    pc->d.out.len = 0;
    return 0;
}

static const char push_trailing[] = "trailing data after the end of the compressed stream";

static long push_gz_head(const uint8_t* b, size_t n)
{
    size_t need = 10;
    const uint8_t* z;
    uint8_t flg;
    if ((n > 0 && b[0] != 0x1f) || (n > 1 && b[1] != 0x8b) || (n > 2 && b[2] != 0x08) || (n > 3 && (b[3] & 0xe0)))
        return -1;
    if (n < 10)
        return 0;
    flg = b[3];
    if (flg & 0x04) {
        if (n < 12)
            return 0;
        need += 2 + (size_t)(b[10] | (b[11] << 8));
        if (n < need)
            return 0;
    }
    if (flg & 0x08) {
        z = (const uint8_t*)memchr(b + need, 0, n - need);
        if (!z)
            return 0;
        need = (size_t)(z - b) + 1;
    }
    if (flg & 0x10) {
        z = n > need ? (const uint8_t*)memchr(b + need, 0, n - need) : NULL;
        if (!z)
            return 0;
        need = (size_t)(z - b) + 1;
    }
    if (flg & 0x02)
        need += 2;
    return n < need ? 0 : (long)need;
}

static long push_inf_head(push_codec_t* pc, const uint8_t* b, size_t n, const char** err)
{
    unsigned cmf, flg;
    *err = NULL;
    if (pc->kind == PUSH_GZIP) {
        long h = push_gz_head(b, n);
        if (h < 0)
            *err = "not a gzip member (bad header)";
        return h;
    }
    if (n < 2)
        return 0;
    cmf = b[0];
    flg = b[1];
    if ((cmf & 0x0f) != 8 || (cmf >> 4) > 7 || ((cmf << 8) | flg) % 31 != 0)
        *err = "invalid zlib header";
    else if (flg & 0x20)
        *err = "zlib preset dictionaries are not supported";
    return *err ? -1 : 2;
}

static const char* push_inf_between(push_codec_t* pc, const uint8_t* p, size_t n)
{
    if (!n)
        return NULL;
    if (pc->kind != PUSH_GZIP)
        return push_trailing;
    if (!p[0]) {
        pc->zph = 4;
        return NULL;
    }
    pc->crc = 0;
    pc->isize = 0;
    pc->zbn = 0;
    dyn_infs_free(pc->zs);
    pc->zs = NULL;
    pc->zph = 0;
    return NULL;
}

static const char* push_inf_feed(push_codec_t* pc, const uint8_t* p, size_t n, push_out_t* out)
{
    cbuf_t held = { 0 };
    const char* e = NULL;
    for (;;) {
        if (pc->zph == 0) {
            long h;
            if (pc->kind == PUSH_RAW) {
                pc->zph = 1;
                continue;
            }
            if (pc->gh.len) {
                if (n && cbuf_put(&pc->gh, p, n) < 0) {
                    e = "out of memory";
                    break;
                }
                h = push_inf_head(pc, pc->gh.buf, pc->gh.len, &e);
                if (h <= 0)
                    break;
                cbuf_free(&held);
                held = pc->gh;
                memset(&pc->gh, 0, sizeof pc->gh);
                p = held.buf + h;
                n = held.len - (size_t)h;
            } else {
                h = push_inf_head(pc, p, n, &e);
                if (h < 0)
                    break;
                if (!h) {
                    if (n && cbuf_put(&pc->gh, p, n) < 0)
                        e = "out of memory";
                    break;
                }
                p += h;
                n -= (size_t)h;
            }
            pc->zph = 1;
        }
        if (pc->zph == 1) {
            const uint8_t* q;
            size_t used, k;
            int rc;
            if (!pc->zs && !(pc->zs = dyn_infs_new())) {
                e = "out of memory";
                break;
            }
            do {
                rc = dyn_infs_run(pc->zs, p, n, &used, &q, &k);
                if (k) {
                    if (pout_put(out, q, k, rc == DYN_INFS_FULL) < 0) {
                        rc = DYN_INFS_OOM;
                        break;
                    }
                    if (pc->kind == PUSH_ZLIB)
                        pc->adler = dyn_adler32(pc->adler, q, k);
                    else if (pc->kind == PUSH_GZIP) {
                        pc->crc = dyn_crc32_update(pc->crc, q, k);
                        pc->isize += (uint32_t)k;
                    }
                }
                p += used;
                n -= used;
            } while (rc == DYN_INFS_FULL);
            if (rc == DYN_INFS_OOM)
                e = "out of memory";
            else if (rc < 0)
                e = "malformed deflate stream";
            if (rc != DYN_INFS_END)
                break;
            pc->zbn = (int)dyn_infs_tail(pc->zs, pc->zb);
            pc->zph = 2;
        }
        if (pc->zph == 2) {
            int need = pc->kind == PUSH_ZLIB ? 4 : pc->kind == PUSH_GZIP ? 8 : 0;
            const uint8_t* t = pc->zb;
            uint8_t extra[16];
            int nx;
            while (pc->zbn < need && n) {
                pc->zb[pc->zbn++] = *p++;
                n--;
            }
            if (pc->zbn < need)
                break;
            if (pc->kind == PUSH_ZLIB && (((uint32_t)t[0] << 24) | ((uint32_t)t[1] << 16) | ((uint32_t)t[2] << 8) | (uint32_t)t[3]) != pc->adler) {
                e = "zlib Adler-32 mismatch";
                break;
            }
            if (pc->kind == PUSH_GZIP) {
                uint32_t c = (uint32_t)t[0] | ((uint32_t)t[1] << 8) | ((uint32_t)t[2] << 16) | ((uint32_t)t[3] << 24);
                uint32_t s = (uint32_t)t[4] | ((uint32_t)t[5] << 8) | ((uint32_t)t[6] << 16) | ((uint32_t)t[7] << 24);
                if (c != pc->crc) {
                    e = "gzip member CRC-32 mismatch";
                    break;
                }
                if (s != pc->isize) {
                    e = "gzip member length (ISIZE) mismatch";
                    break;
                }
            }
            nx = pc->zbn - need;
            memcpy(extra, pc->zb + need, (size_t)nx);
            pc->zph = 3;
            if (nx) {
                e = push_inf_between(pc, extra, (size_t)nx);
                if (e || pc->zph != 0)
                    break;
                if (cbuf_put(&pc->gh, extra, (size_t)nx) < 0) {
                    e = "out of memory";
                    break;
                }
                continue;
            }
        }
        if (pc->zph == 3) {
            e = push_inf_between(pc, p, n);
            if (e || pc->zph != 0)
                break;
            continue;
        }
        break;
    }
    cbuf_free(&held);
    return e;
}

static const char* push_inf_end(push_codec_t* pc)
{
    if (pc->zph >= 3)
        return NULL;
    return pc->kind == PUSH_GZIP ? "truncated compressed stream (no gzip trailer)"
        : pc->kind == PUSH_ZLIB  ? "truncated zlib stream"
                                 : "truncated compressed stream";
}

static const char* push_dec_feed(push_codec_t* pc, const uint8_t* p, size_t n, push_out_t* po)
{
    const char* err;
    cbuf_t* out = &po->cat;
    if (pc->inf)
        return push_inf_feed(pc, p, n, po);
    if (pc->d.done && n)
        return "trailing data after the end of the compressed stream";
    if (pc->kind == PUSH_ZLIB) {
        while (pc->hdr_len < 2 && n) {
            pc->hdr[pc->hdr_len++] = *p++;
            n--;
            if (pc->hdr_len == 2) {
                unsigned cmf = pc->hdr[0], flg = pc->hdr[1];
                if ((cmf & 0x0f) != 8 || (cmf >> 4) > 7 || ((cmf << 8) | flg) % 31 != 0)
                    return "invalid zlib header";
                if (flg & 0x20)
                    return "zlib preset dictionaries are not supported";
            }
        }
        if (!n)
            return NULL;
        if ((size_t)pc->tail_len + n <= 4) {
            memcpy(pc->tail + pc->tail_len, p, n);
            pc->tail_len += (int)n;
            return NULL;
        }
        {
            size_t total = (size_t)pc->tail_len + n;
            size_t feed = total - 4;
            size_t from_tail = feed < (size_t)pc->tail_len ? feed : (size_t)pc->tail_len;
            uint8_t keep[4];
            size_t k;
            if (from_tail && cbuf_put(&pc->d.in, pc->tail, from_tail) < 0)
                return "out of memory";
            if (feed > from_tail && cbuf_put(&pc->d.in, p, feed - from_tail) < 0)
                return "out of memory";
            for (k = 0; k < 4; k++) {
                size_t idx = feed + k;
                keep[k] = idx < (size_t)pc->tail_len ? pc->tail[idx] : p[idx - (size_t)pc->tail_len];
            }
            memcpy(pc->tail, keep, 4);
            pc->tail_len = 4;
        }
    } else if (n && cbuf_put(&pc->d.in, p, n) < 0) {
        return "out of memory";
    }
    err = codec_dec_run(&pc->d, 0, pc->kind == PUSH_GZIP ? 8 : 0);
    if (err)
        return err;
    if (push_take_dec_out(pc, out) < 0)
        return "out of memory";
    if ((pc->d.done && pc->d.in.len + (pc->kind == PUSH_ZLIB ? (size_t)pc->tail_len : 0) > (pc->kind == PUSH_ZLIB ? 4u : pc->kind == PUSH_GZIP ? 8u : 0u))
        || (pc->kind == PUSH_GZIP && pc->d.phase == 2 && pc->d.in.len > 8))
        return push_trailing;
    return NULL;
}

static JSValue push_codec_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    const char* name;
    push_codec_t* pc;
    JSValue obj;
    int kind, codec, level;

    (void)new_target;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "PushCodec(codec, encode) needs two arguments");
    name = JS_ToCString(ctx, argv[0]);
    if (!name)
        return JS_EXCEPTION;
    if (!strcmp(name, "deflate"))
        kind = PUSH_RAW;
    else if (!strcmp(name, "gzip"))
        kind = PUSH_GZIP;
    else if (!strcmp(name, "zlib"))
        kind = PUSH_ZLIB;
#if defined(DYN_STREAM_HAVE_BROTLI)
    else if (!strcmp(name, "brotli"))
        kind = PUSH_BROTLI;
#endif
#if defined(CONFIG_ZSTD)
    else if (!strcmp(name, "zstd"))
        kind = PUSH_ZSTD;
#endif
    else if (!strcmp(name, "lz4"))
        kind = PUSH_LZ4;
    else {
        JS_FreeCString(ctx, name);
        return JS_ThrowTypeError(ctx, "PushCodec: unknown codec");
    }
    JS_FreeCString(ctx, name);
    codec = kind == PUSH_GZIP ? DYN_STREAM_CODEC_GZIP : kind == PUSH_BROTLI ? DYN_STREAM_CODEC_BROTLI : kind == PUSH_ZSTD ? DYN_STREAM_CODEC_ZSTD : kind == PUSH_LZ4 ? DYN_STREAM_CODEC_LZ4 : DYN_STREAM_CODEC_DEFLATE;
    level = kind == PUSH_LZ4 ? 1 : 6;
#if defined(CONFIG_ZSTD)
    if (kind == PUSH_ZSTD)
        level = ZSTD_defaultCLevel();
#endif
    pc = (push_codec_t*)calloc(1, sizeof(*pc));
    if (!pc)
        return JS_ThrowOutOfMemory(ctx);
    pc->encode = JS_ToBool(ctx, argv[1]);
    pc->kind = kind;
    pc->adler = 1;
    if (pc->encode ? codec_enc_init(&pc->e, codec, level) < 0 : codec_dec_init(&pc->d, codec) < 0) {
        free(pc);
        return JS_ThrowOutOfMemory(ctx);
    }
    pc->inf = !pc->encode && kind <= PUSH_ZLIB;
    obj = JS_NewObjectClass(ctx, (int)push_codec_class_id);
    if (JS_IsException(obj)) {
        pc->closed = 0;
        push_codec_release(pc);
        free(pc);
        return obj;
    }
    JS_SetOpaque(obj, pc);
    return obj;
}

static const char* push_state_err(push_codec_t* pc)
{
    if (!pc)
        return "the codec is closed";
    if (pc->closed)
        return "the codec is closed";
    if (pc->failed)
        return "the codec already failed";
    if (pc->ended)
        return "the codec already ended";
    return NULL;
}

static const char* push_step_push(push_codec_t* pc, const uint8_t* p, size_t n, push_out_t* po)
{
    if (pc->encode) {
        if (pc->kind == PUSH_ZLIB && !pc->hdr_len && n) {
            static const uint8_t zh[2] = { 0x78, 0x9c };
            if (cbuf_put(&po->cat, zh, 2) < 0)
                return "out of memory";
            pc->hdr_len = 2;
        }
        if (pc->kind == PUSH_ZLIB && n)
            pc->adler = dyn_adler32(pc->adler, p, n);
        if (!n)
            return NULL;
        return codec_enc_feed(&pc->e, p, n, 0, &po->cat);
    }
    return push_dec_feed(pc, p, n, po);
}

static const char* push_step_end(push_codec_t* pc, push_out_t* po)
{
    const char* err;
    cbuf_t* out = &po->cat;
    pc->ended = 1;
    if (pc->encode) {
        if (pc->kind == PUSH_ZLIB && !pc->hdr_len) {
            static const uint8_t zh[2] = { 0x78, 0x9c };
            if (cbuf_put(out, zh, 2) < 0)
                return "out of memory";
            pc->hdr_len = 2;
        }
        err = codec_enc_feed(&pc->e, NULL, 0, 1, out);
        if (err)
            return err;
        if (pc->kind == PUSH_ZLIB) {
            uint8_t t[4];
            t[0] = (uint8_t)(pc->adler >> 24);
            t[1] = (uint8_t)(pc->adler >> 16);
            t[2] = (uint8_t)(pc->adler >> 8);
            t[3] = (uint8_t)pc->adler;
            if (cbuf_put(out, t, 4) < 0)
                return "out of memory";
        }
    } else if (pc->inf) {
        err = push_inf_end(pc);
        if (err)
            return err;
    } else {
        if (pc->kind == PUSH_ZLIB && (pc->hdr_len < 2 || pc->tail_len < 4))
            return "truncated zlib stream";
        err = codec_dec_finish(&pc->d);
        if (err)
            return err;
        if (push_take_dec_out(pc, out) < 0)
            return "out of memory";
        if (pc->kind == PUSH_ZLIB) {
            uint32_t want = ((uint32_t)pc->tail[0] << 24) | ((uint32_t)pc->tail[1] << 16) | ((uint32_t)pc->tail[2] << 8) | (uint32_t)pc->tail[3];
            if (want != pc->adler)
                return "zlib Adler-32 mismatch";
        }
    }
    push_codec_release(pc);
    return NULL;
}

const char dyn_push_emit_failed[] = "emit failed";

const char* dyn_push_codec_step(JSValueConst obj, const uint8_t* p, size_t n, int end, dyn_push_emit_fn emit, void* ud)
{
    push_codec_t* pc = (push_codec_t*)JS_GetOpaque(obj, push_codec_class_id);
    push_out_t po;
    const char* err = push_state_err(pc);
    int i, keep;
    if (err)
        return err;
    memset(&po, 0, sizeof po);
    po.split = 1;
    err = end ? push_step_end(pc, &po) : push_step_push(pc, p, n, &po);
    keep = !err || (err == push_trailing && (po.pn || po.cat.len));
    if (err)
        pc->failed = 1;
    if (keep) {
        for (i = 0; i < po.pn; i++) {
            uint8_t* b = po.pv[i].buf;
            po.pv[i].buf = NULL;
            if (emit(ud, b, po.pv[i].len) < 0) {
                pout_free(&po);
                return dyn_push_emit_failed;
            }
        }
        if (po.cat.len) {
            uint8_t* b = po.cat.buf;
            size_t k = po.cat.len;
            if (po.cat.cap - k > k / 4 && po.cat.cap > 4096) {
                uint8_t* nb = (uint8_t*)realloc(b, k);
                if (nb)
                    b = nb;
            }
            po.cat.buf = NULL;
            po.cat.len = po.cat.cap = 0;
            if (emit(ud, b, k) < 0) {
                pout_free(&po);
                return dyn_push_emit_failed;
            }
        }
    }
    pout_free(&po);
    return err;
}

static JSValue push_codec_push(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    push_codec_t* pc = push_codec_of(ctx, this_val);
    push_out_t po;
    uint8_t* p;
    size_t n = 0;
    const char* err;

    if (!pc)
        return JS_EXCEPTION;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "push(bytes) needs a Uint8Array");
    p = stream_view_bytes(ctx, argv[0], &n, 1);
    if (!p && JS_HasException(ctx))
        return JS_EXCEPTION;
    memset(&po, 0, sizeof po);
    err = push_step_push(pc, p, n, &po);
    if (err == push_trailing && po.cat.len) {
        JSValue o = push_out(ctx, &po.cat), e;
        pc->failed = 1;
        if (JS_IsException(o))
            return o;
        JS_ThrowTypeError(ctx, "%s", err);
        e = JS_GetException(ctx);
        JS_DefinePropertyValueStr(ctx, e, "output", o, JS_PROP_C_W_E);
        return JS_Throw(ctx, e);
    }
    if (err)
        return push_fail(ctx, pc, &po.cat, err);
    return push_out(ctx, &po.cat);
}

static JSValue push_codec_end(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    push_codec_t* pc = push_codec_of(ctx, this_val);
    push_out_t po;
    const char* err;

    (void)argc;
    (void)argv;
    if (!pc)
        return JS_EXCEPTION;
    memset(&po, 0, sizeof po);
    err = push_step_end(pc, &po);
    if (err)
        return push_fail(ctx, pc, &po.cat, err);
    return push_out(ctx, &po.cat);
}

static JSValue push_codec_close(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    push_codec_t* pc = (push_codec_t*)JS_GetOpaque2(ctx, this_val, push_codec_class_id);
    (void)argc;
    (void)argv;
    if (!pc)
        return JS_EXCEPTION;
    push_codec_release(pc);
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry push_codec_proto_funcs[] = {
    JS_CFUNC_DEF("push", 1, push_codec_push),
    JS_CFUNC_DEF("end", 0, push_codec_end),
    JS_CFUNC_DEF("close", 0, push_codec_close),
};

#define WEB_PIPE_BATCH 64

static void web_pipe_settle(JSContext* ctx, JSValueConst fn, JSValue v)
{
    JSValue r = JS_IsException(v) ? JS_EXCEPTION : JS_Call(ctx, fn, JS_UNDEFINED, 1, (JSValueConst*)&v);
    JS_FreeValue(ctx, v);
    if (JS_IsException(r))
        stream_drop_exc(ctx);
    else
        JS_FreeValue(ctx, r);
}

static int web_pipe_fill(JSContext* ctx, stream_source_t* s, uint8_t* dst, size_t room, size_t* pn)
{
    ssize_t r;

    if (s->kind == STREAM_SRC_BYTES) {
        uint64_t left = (uint64_t)s->len - s->off;
        size_t n = (uint64_t)room < left ? room : (size_t)left;
        if (n)
            memcpy(dst, s->buf + s->off, n);
        *pn = n;
    } else {
        if (s->fd < 0) {
            int fd = open(s->path, O_RDONLY | O_CLOEXEC);
            if (fd < 0) {
                stream_errno_throw(ctx, errno, "fromFile", s->path);
                return -1;
            }
            s->fd = fd;
        }
        do
            r = read(s->fd, dst, room);
        while (r < 0 && errno == EINTR);
        if (r < 0) {
            stream_errno_throw(ctx, errno, "read", s->path);
            return -1;
        }
        *pn = (size_t)r;
    }
    s->off += *pn;
    return 0;
}

static int web_pipe_direct(JSContext* ctx, stream_source_t* s, stream_sink_t* w, uint8_t* scratch, size_t* pn)
{
    size_t done = 0;

    if (web_pipe_fill(ctx, s, scratch, DYN_STREAM_CHUNK, pn) < 0)
        return -1;
    if (*pn > 0 && w->len > 0 && stream_sink_flush_native(w) < 0)
        goto fail;
    while (done < *pn) {
        ssize_t r = write(w->fd, scratch + done, *pn - done);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            goto fail;
        }
        done += (size_t)r;
    }
    w->total += *pn;
    return 0;
fail:
    w->sticky_errno = errno;
    stream_errno_throw(ctx, w->sticky_errno, "write", w->path);
    return -1;
}

static int web_pipe_step(JSContext* ctx, stream_source_t* s, stream_sink_t* w, uint8_t* scratch, size_t* pn)
{
    size_t room;

    if (w->sticky_errno) {
        stream_errno_throw(ctx, w->sticky_errno, "write", w->path);
        return -1;
    }
    if (stream_sink_open(w, ctx) < 0)
        return -1;
    if (w->len == w->cap && stream_sink_flush_native(w) < 0) {
        w->sticky_errno = errno;
        stream_errno_throw(ctx, w->sticky_errno, "write", w->path);
        return -1;
    }
    if (scratch)
        return web_pipe_direct(ctx, s, w, scratch, pn);
    room = (size_t)(w->cap - w->len);
    if (web_pipe_fill(ctx, s, w->buf + w->len, room, pn) < 0)
        return -1;
    w->len += (unsigned)*pn;
    w->total += *pn;
    if (w->len == w->cap && stream_sink_flush_native(w) < 0) {
        w->sticky_errno = errno;
        stream_errno_throw(ctx, w->sticky_errno, "write", w->path);
        return -1;
    }
    return 0;
}

static JSValue web_pipe_job(JSContext* ctx, int argc, JSValueConst* argv)
{
    DynResource* rs;
    DynResource* rw;
    double total = 0;
    size_t n = 1;
    int i;
    uint8_t* scratch = NULL;
    JSValue next[5];

    (void)argc;
    if (JS_ToFloat64(ctx, &total, argv[4]) < 0)
        goto fail;
    rs = dyn_res_get(ctx, argv[0], stream_source_class_id);
    if (!rs)
        goto fail;
    rw = dyn_res_get(ctx, argv[1], stream_sink_class_id);
    if (!rw)
        goto fail;
    if (((stream_sink_t*)rw->native)->cap < DYN_STREAM_CHUNK) {
        scratch = malloc(DYN_STREAM_CHUNK);
        if (!scratch) {
            JS_ThrowOutOfMemory(ctx);
            goto fail;
        }
    }
    for (i = 0; i < WEB_PIPE_BATCH && n > 0; i++) {
        if (web_pipe_step(ctx, (stream_source_t*)rs->native, (stream_sink_t*)rw->native, scratch, &n) < 0)
            goto fail;
        total += (double)n;
    }
    free(scratch);
    scratch = NULL;
    if (n == 0) {
        web_pipe_settle(ctx, argv[2], JS_NewFloat64(ctx, total));
        return JS_UNDEFINED;
    }
    next[0] = argv[0];
    next[1] = argv[1];
    next[2] = argv[2];
    next[3] = argv[3];
    next[4] = JS_NewFloat64(ctx, total);
    if (JS_EnqueueJob(ctx, web_pipe_job, 5, (JSValueConst*)next) < 0)
        goto fail;
    return JS_UNDEFINED;
fail:
    free(scratch);
    web_pipe_settle(ctx, argv[3], JS_GetException(ctx));
    return JS_UNDEFINED;
}

static JSValue web_native_pipe(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DynResource* rs;
    DynResource* rw;
    JSValue funcs[2], promise, args[5];
    int rc;

    (void)this_val;
    if (argc < 2 || !JS_IsObject(argv[0]) || !JS_IsObject(argv[1]))
        return JS_UNDEFINED;
    rs = JS_GetOpaque(argv[0], stream_source_class_id);
    rw = JS_GetOpaque(argv[1], stream_sink_class_id);
    if (!rs || !rw || rs->closed || rw->closed)
        return JS_UNDEFINED;
    promise = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(promise))
        return promise;
    args[0] = argv[0];
    args[1] = argv[1];
    args[2] = funcs[0];
    args[3] = funcs[1];
    args[4] = JS_NewFloat64(ctx, 0);
    rc = JS_EnqueueJob(ctx, web_pipe_job, 5, (JSValueConst*)args);
    JS_FreeValue(ctx, funcs[0]);
    JS_FreeValue(ctx, funcs[1]);
    if (rc < 0) {
        JS_FreeValue(ctx, promise);
        return JS_EXCEPTION;
    }
    return promise;
}

JSValue stream_web_bridge(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    static const char* const names[] = { "toReadableStream", "toWritableStream" };

    (void)this_val;
    if (magic >= 4 && magic <= 6) {
        JSValue a[2];
        a[0] = JS_NewInt32(ctx, magic - 4);
        a[1] = JS_TRUE;
        return js_streams_call(ctx, "stdio", 2, (JSValueConst*)a);
    }
    if (magic < 0 || magic >= (int)countof(names))
        return JS_ThrowInternalError(ctx, "stream: bad bridge index");
    return js_streams_call(ctx, names[magic], argc, argv);
}

int dyn_stream_web_natives(JSContext* ctx, JSValueConst natives)
{
    JSRuntime* rt = JS_GetRuntime(ctx);
    JSValue proto, ctor, pipe;

    JS_NewClassID(&push_codec_class_id);
    if (!JS_IsRegisteredClass(rt, push_codec_class_id)) {
        if (JS_NewClass(rt, push_codec_class_id, &push_codec_class) < 0)
            return -1;
    }
    proto = JS_NewObject(ctx);
    if (JS_IsException(proto))
        return -1;
    if (JS_SetPropertyFunctionList(ctx, proto, push_codec_proto_funcs,
            countof(push_codec_proto_funcs))
        < 0) {
        JS_FreeValue(ctx, proto);
        return -1;
    }
    JS_SetClassProto(ctx, push_codec_class_id, JS_DupValue(ctx, proto));
    ctor = JS_NewCFunction2(ctx, push_codec_ctor, "PushCodec", 2,
        JS_CFUNC_constructor, 0);
    if (JS_IsException(ctor)) {
        JS_FreeValue(ctx, proto);
        return -1;
    }
    if (JS_SetConstructor(ctx, ctor, proto) < 0) {
        JS_FreeValue(ctx, ctor);
        JS_FreeValue(ctx, proto);
        return -1;
    }
    JS_FreeValue(ctx, proto);
    if (JS_DefinePropertyValueStr(ctx, natives, "PushCodec", ctor, JS_PROP_C_W_E) < 0)
        return -1;
    pipe = JS_NewCFunction(ctx, stream_pipe, "pipe", 2);
    if (JS_IsException(pipe))
        return -1;
    if (JS_DefinePropertyValueStr(ctx, natives, "fastPipe", pipe, JS_PROP_C_W_E) < 0)
        return -1;
    pipe = JS_NewCFunction(ctx, web_native_pipe, "nativePipe", 2);
    if (JS_IsException(pipe))
        return -1;
    if (JS_DefinePropertyValueStr(ctx, natives, "nativePipe", pipe, JS_PROP_C_W_E) < 0)
        return -1;
    return 0;
}

#endif
