/*
 * dyna:compress -- gzip (RFC 1952) compress/decompress. Self-contained, in-repo,
 * no external deps.
 *
 *   import { gzip, gunzip } from "dyna:compress";
 *   const packed = gzip("hello world".repeat(100)); // str|Uint8Array|ArrayBuffer -> Uint8Array
 *   const bytes  = gunzip(packed);                   // -> Uint8Array
 *   const text   = gunzip(packed, { asString: true });// -> string (UTF-8 decode)
 *
 * Transient plain functions -- no resource, no dispose. The input bytes are
 * copied into a private libc buffer FIRST (fully decoupled from the JS heap),
 * the codec runs entirely in C, then the result is COPIED into a fresh,
 * independent JS value; every C buffer is freed before returning on EVERY path.
 * Nothing native escapes into the JS heap.
 *
 * Codec:
 *   - gzip():   RFC 1952 framing (magic 1f 8b, method 8, mtime=0, OS=ff) around a
 *               real fixed-Huffman DEFLATE stream (LZ77 hash-chain match finding
 *               + RFC 1951 §3.2.6 fixed codes), falling back to *stored*
 *               (uncompressed) blocks when that would not shrink the input so the
 *               output never expands -- always valid DEFLATE that any standard
 *               decoder accepts -- plus the CRC-32 + ISIZE trailer.
 *   - gunzip(): a full RFC 1951 inflate (stored / fixed-Huffman / dynamic-Huffman
 *               blocks) so it decodes the output of the system gzip/zlib, with
 *               RFC 1952 header parsing and CRC-32 + ISIZE trailer validation.
 *
 * Every bit/byte read from untrusted input is bounds-checked against the input
 * length; the output size is capped to reject decompression bombs.
 */
#include "dyna-nat.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_COMPRESS)

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* the pure-C gzip/DEFLATE codec (src/core/dyn-compress.c) */
#include "core/dyn-compress.h"
#include "core/dyn-dict.h"
#include "core/dyn-hash.h"   /* dyn_crc32c: the dictionary id */

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif



/* CRC-32 for the gzip trailer comes from the shared pure-C digest library
 * (src/core/dyn-hash.c) -- same IEEE 802.3 polynomial, same table-free
 * bit-serial form, so there is still no lazily-initialised static to race
 * across worker threads. */

/* ---------- JS input/output boundary --------------------------------------- */

/* Copy the argument's bytes into a fresh libc buffer and release every JS-side
 * handle before returning, so the pointer handed back is decoupled from the JS
 * heap. Returns 0 with *pout (never NULL) / *plen set, or -1 with a pending JS
 * exception. Accepts a string, a typed array, or an ArrayBuffer. */
static int dyn_read_input(JSContext *ctx, JSValueConst val, uint8_t **pout,
                          size_t *plen)
{
    uint8_t *base, *copy;
    size_t off = 0, tlen = 0, ab = 0;
    JSValue buf;

    if (JS_IsString(val)) {
        size_t slen;
        const char *str = JS_ToCStringLen(ctx, &slen, val);
        if (!str)
            return -1;
        copy = (uint8_t *)malloc(slen ? slen : 1);
        if (!copy) {
            JS_FreeCString(ctx, str);
            JS_ThrowOutOfMemory(ctx);
            return -1;
        }
        if (slen)
            memcpy(copy, str, slen);
        JS_FreeCString(ctx, str);
        *pout = copy;
        *plen = slen;
        return 0;
    }

    buf = JS_GetTypedArrayBuffer(ctx, val, &off, &tlen, NULL);
    if (!JS_IsException(buf)) {
        base = JS_GetArrayBuffer(ctx, &ab, buf);
        if (!base) {
            JS_FreeValue(ctx, buf);
            return -1;
        }
        if (off > ab)
            off = ab;
        if (tlen > ab - off)
            tlen = ab - off;
    } else {
        JS_FreeValue(ctx, JS_GetException(ctx)); /* not a typed array */
        base = JS_GetArrayBuffer(ctx, &ab, val);
        if (!base) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            JS_ThrowTypeError(ctx, "dyna:compress: input must be a string, "
                                   "Uint8Array, or ArrayBuffer");
            return -1;
        }
        buf = JS_UNDEFINED; /* bare ArrayBuffer: no reference to release */
        off = 0;
        tlen = ab;
    }

    copy = (uint8_t *)malloc(tlen ? tlen : 1);
    if (!copy) {
        JS_FreeValue(ctx, buf);
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    if (tlen)
        memcpy(copy, base + off, tlen);
    JS_FreeValue(ctx, buf);
    *pout = copy;
    *plen = tlen;
    return 0;
}

/* Copy `len` bytes into a fresh, independent JS Uint8Array over its own buffer. */
static JSValue dyn_bytes_to_uint8(JSContext *ctx, const uint8_t *data, size_t len)
{
    static const uint8_t empty = 0;
    JSValue ab, u8;
    JSValueConst args[3];

    ab = JS_NewArrayBufferCopy(ctx, len ? data : &empty, len);
    if (JS_IsException(ab))
        return ab;
    args[0] = ab;
    args[1] = JS_UNDEFINED;
    args[2] = JS_UNDEFINED;
    u8 = JS_NewTypedArray(ctx, 3, args, JS_TYPED_ARRAY_UINT8);
    JS_FreeValue(ctx, ab); /* u8 keeps its own reference to the buffer */
    return u8;
}

/* ---------- gzip / gunzip -------------------------------------------------- */

static JSValue dyn_gzip(JSContext *ctx, JSValueConst this_val, int argc,
                        JSValueConst *argv)
{
    uint8_t *src = NULL, *out = NULL;
    size_t src_len = 0, out_len = 0;
    JSValue result;

    (void)this_val;

    /* Accept an optional level for signature compatibility; the fixed-Huffman
     * encoder ignores it. Coerce it FIRST (a throwing valueOf strands nothing). */
    if (argc > 1 && JS_IsNumber(argv[1])) {
        int32_t lv;
        if (JS_ToInt32(ctx, &lv, argv[1]))
            return JS_EXCEPTION;
        (void)lv;
    }

    if (dyn_read_input(ctx, argv[0], &src, &src_len) < 0)
        return JS_EXCEPTION;
    if (dyn_gzip_build(src, src_len, &out, &out_len) < 0) {
        free(src);
        return JS_ThrowOutOfMemory(ctx);
    }
    free(src);
    result = dyn_bytes_to_uint8(ctx, out, out_len);
    free(out);
    return result;
}

static JSValue dyn_gunzip(JSContext *ctx, JSValueConst this_val, int argc,
                          JSValueConst *argv)
{
    uint8_t *src = NULL;
    size_t src_len = 0;
    dyn_outbuf_t o = { NULL, 0, 0 };
    int as_string = 0;
    JSValue result;

    (void)this_val;

    /* Optional { asString: true } -> UTF-8 decode to a JS string. Read the
     * (possibly getter-backed) property before touching native buffers. */
    if (argc > 1 && JS_IsObject(argv[1])) {
        JSValue v = JS_GetPropertyStr(ctx, argv[1], "asString");
        if (JS_IsException(v))
            return JS_EXCEPTION;
        as_string = JS_ToBool(ctx, v);
        JS_FreeValue(ctx, v);
        if (as_string < 0)
            return JS_EXCEPTION;
    }

    if (dyn_read_input(ctx, argv[0], &src, &src_len) < 0)
        return JS_EXCEPTION;
    if (dyn_gunzip_decode(src, src_len, &o) < 0) {
        free(src);
        free(o.buf);
        return JS_ThrowTypeError(ctx, "dyna:compress gunzip: invalid gzip data");
    }
    free(src);

    if (as_string)
        result = JS_NewStringLen(ctx, o.len ? (const char *)o.buf : "", o.len);
    else
        result = dyn_bytes_to_uint8(ctx, o.buf, o.len);
    free(o.buf);
    return result;
}


/* ---------- LZ4 ------------------------------------------------------------
 *
 * Two shapes, and they are not interchangeable:
 *
 *   lz4Compress/lz4Decompress  a RAW BLOCK -- no header, no length, no
 *                              checksum. The caller already knows the size and
 *                              the framing. This is what a message bus wants.
 *   lz4Frame/lz4Unframe        the LZ4 FRAME -- magic, descriptor, block sizes
 *                              and an optional content checksum. This is what
 *                              the `lz4` command line reads and writes, and
 *                              tests/test_compress.js checks both directions
 *                              against it rather than round-tripping ourselves.
 */

/* Read {level, dict} from an options object. Both are coerced BEFORE anything
 * native is resolved or allocated (CLAUDE.md section 8). */
static int dyn_lz4_opts(JSContext *ctx, JSValueConst opts, int *plevel,
                        uint8_t **pdict, size_t *pdict_len)
{
    *plevel = 1;
    *pdict = NULL;
    *pdict_len = 0;
    if (!JS_IsObject(opts))
        return 0;
    {
        JSValue v = JS_GetPropertyStr(ctx, opts, "level");
        if (JS_IsException(v))
            return -1;
        if (!JS_IsUndefined(v)) {
            int32_t lv;
            if (JS_ToInt32(ctx, &lv, v)) {
                JS_FreeValue(ctx, v);
                return -1;
            }
            if (lv < 1 || lv > 12) {
                JS_FreeValue(ctx, v);
                JS_ThrowRangeError(ctx, "level must be 1..12");
                return -1;
            }
            *plevel = (int)lv;
        }
        JS_FreeValue(ctx, v);
    }
    {
        JSValue v = JS_GetPropertyStr(ctx, opts, "dict");
        if (JS_IsException(v))
            return -1;
        if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
            if (dyn_read_input(ctx, v, pdict, pdict_len) < 0) {
                JS_FreeValue(ctx, v);
                return -1;
            }
        }
        JS_FreeValue(ctx, v);
    }
    return 0;
}

static JSValue dyn_lz4_compress_js(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    uint8_t *src = NULL, *dict = NULL, *out = NULL;
    size_t src_len = 0, dict_len = 0, out_len = 0;
    int level;
    JSValue result;

    (void)this_val;
    if (dyn_lz4_opts(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, &level, &dict,
                     &dict_len) < 0)
        return JS_EXCEPTION;
    if (dyn_read_input(ctx, argv[0], &src, &src_len) < 0) {
        free(dict);
        return JS_EXCEPTION;
    }
    if (dyn_lz4_compress(src, src_len, dict, dict_len, level, NULL,
                         &out, &out_len) < 0) {
        free(src);
        free(dict);
        return JS_ThrowOutOfMemory(ctx);
    }
    free(src);
    free(dict);
    result = dyn_bytes_to_uint8(ctx, out, out_len);
    free(out);
    return result;
}

static JSValue dyn_lz4_decompress_js(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    uint8_t *src = NULL, *dict = NULL;
    size_t src_len = 0, dict_len = 0;
    dyn_outbuf_t o = { NULL, 0, 0 };
    int level;
    JSValue result;

    (void)this_val;
    if (dyn_lz4_opts(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, &level, &dict,
                     &dict_len) < 0)
        return JS_EXCEPTION;
    if (dyn_read_input(ctx, argv[0], &src, &src_len) < 0) {
        free(dict);
        return JS_EXCEPTION;
    }
    if (dyn_lz4_decompress(src, src_len, dict, dict_len, &o) < 0) {
        free(src);
        free(dict);
        free(o.buf);
        return JS_ThrowTypeError(ctx, "dyna:compress lz4Decompress: invalid LZ4 block");
    }
    free(src);
    free(dict);
    result = dyn_bytes_to_uint8(ctx, o.buf, o.len);
    free(o.buf);
    return result;
}

static JSValue dyn_lz4_frame_js(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    uint8_t *src = NULL, *dict = NULL, *out = NULL;
    size_t src_len = 0, dict_len = 0, out_len = 0;
    int level, checksum = 1;
    JSValue result;

    (void)this_val;
    if (argc > 1 && JS_IsObject(argv[1])) {
        JSValue v = JS_GetPropertyStr(ctx, argv[1], "checksum");
        if (JS_IsException(v))
            return JS_EXCEPTION;
        if (!JS_IsUndefined(v))
            checksum = JS_ToBool(ctx, v);
        JS_FreeValue(ctx, v);
    }
    if (dyn_lz4_opts(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, &level, &dict,
                     &dict_len) < 0)
        return JS_EXCEPTION;
    free(dict);            /* the frame format has no prefix-dictionary form */
    if (dyn_read_input(ctx, argv[0], &src, &src_len) < 0)
        return JS_EXCEPTION;
    if (dyn_lz4_frame_build(src, src_len, level, checksum, NULL,
                            &out, &out_len) < 0) {
        free(src);
        return JS_ThrowOutOfMemory(ctx);
    }
    free(src);
    result = dyn_bytes_to_uint8(ctx, out, out_len);
    free(out);
    return result;
}

static JSValue dyn_lz4_unframe_js(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    uint8_t *src = NULL;
    size_t src_len = 0;
    dyn_outbuf_t o = { NULL, 0, 0 };
    int as_string = 0;
    JSValue result;

    (void)this_val;
    if (argc > 1 && JS_IsObject(argv[1])) {
        JSValue v = JS_GetPropertyStr(ctx, argv[1], "asString");
        if (JS_IsException(v))
            return JS_EXCEPTION;
        as_string = JS_ToBool(ctx, v);
        JS_FreeValue(ctx, v);
    }
    if (dyn_read_input(ctx, argv[0], &src, &src_len) < 0)
        return JS_EXCEPTION;
    if (dyn_lz4_frame_decode(src, src_len, &o) < 0) {
        free(src);
        free(o.buf);
        return JS_ThrowTypeError(ctx, "dyna:compress lz4Unframe: invalid LZ4 frame");
    }
    free(src);
    if (as_string)
        result = JS_NewStringLen(ctx, o.len ? (const char *)o.buf : "", o.len);
    else
        result = dyn_bytes_to_uint8(ctx, o.buf, o.len);
    free(o.buf);
    return result;
}

/* ---------- class Compressor (W8.5) ----------------------------------------
 *
 * A compiled capability: the configuration goes in the constructor, the data
 * goes in the method, and the match-finder scratch -- 64 KiB of hash heads plus
 * four bytes per input byte, which the free functions malloc and initialise on
 * EVERY call -- is owned by the instance and reused. That is the whole reason
 * the class exists; the crossover is published in API.md and measured by
 * tests/bench_compress.js.
 *
 * The input is type-checked rather than coerced, so no user JS runs inside a
 * call: there is no busy flag because there is no way to re-enter one.
 *
 * A configured dictionary changes the RECORD, not just the encoder: a
 * dictionary-compressed payload is prefixed with the 4-byte CRC-32C of the
 * dictionary, and decompressing with a different one throws instead of
 * returning plausible garbage. The raw LZ4 block carries no dictionary
 * identity, so without that stamp a mismatch is silent corruption.
 */

typedef struct {
    dyn_comp_ctx_t cx;
    uint8_t *dict;
    size_t dict_len;
    uint32_t dict_id;
    int level;
    int algo;            /* 0 = gzip, 1 = lz4 block, 2 = lz4 frame */
    int checksum;
} dyn_compressor_t;

static JSClassID dyn_compressor_class_id;

static void dyn_compressor_dispose(void *native)
{
    dyn_compressor_t *c = (dyn_compressor_t *)native;
    if (!c)
        return;
    dyn_comp_ctx_free(&c->cx);
    free(c->dict);
    free(c);
}

static const JSClassDef dyn_compressor_class = {
    "Compressor", .finalizer = dyn_res_finalizer
};

static JSValue dyn_compressor_ctor(JSContext *ctx, JSValueConst new_target,
                                   int argc, JSValueConst *argv)
{
    dyn_compressor_t *c;
    uint8_t *dict = NULL;
    size_t dict_len = 0;
    int level = 1, algo = 1, checksum = 1;

    (void)new_target;
    /* Every option is coerced to a C local before anything is allocated. */
    if (argc > 0 && JS_IsObject(argv[0])) {
        JSValue v = JS_GetPropertyStr(ctx, argv[0], "algo");
        if (JS_IsException(v))
            return JS_EXCEPTION;
        if (!JS_IsUndefined(v)) {
            const char *name = JS_ToCString(ctx, v);
            if (!name) {
                JS_FreeValue(ctx, v);
                return JS_EXCEPTION;
            }
            if (!strcmp(name, "gzip")) algo = 0;
            else if (!strcmp(name, "lz4")) algo = 1;
            else if (!strcmp(name, "lz4frame")) algo = 2;
            else {
                JS_FreeCString(ctx, name);
                JS_FreeValue(ctx, v);
                return JS_ThrowTypeError(ctx, "algo must be \"gzip\", \"lz4\" or \"lz4frame\"");
            }
            JS_FreeCString(ctx, name);
        }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[0], "checksum");
        if (JS_IsException(v))
            return JS_EXCEPTION;
        if (!JS_IsUndefined(v))
            checksum = JS_ToBool(ctx, v);
        JS_FreeValue(ctx, v);
        if (dyn_lz4_opts(ctx, argv[0], &level, &dict, &dict_len) < 0)
            return JS_EXCEPTION;
    } else if (argc > 0 && !JS_IsUndefined(argv[0])) {
        return JS_ThrowTypeError(ctx, "expected an options object");
    }
    if (dict_len && algo != 1) {
        free(dict);
        return JS_ThrowTypeError(ctx,
            "a dictionary applies to algo \"lz4\" only");
    }

    c = (dyn_compressor_t *)calloc(1, sizeof(*c));
    if (!c) {
        free(dict);
        return JS_ThrowOutOfMemory(ctx);
    }
    /* The scratch is built HERE, not lazily on the first call: a lazy build is
     * a hidden write and the reason to have a constructor at all. */
    if (dyn_comp_ctx_init(&c->cx) < 0) {
        free(dict);
        free(c);
        return JS_ThrowOutOfMemory(ctx);
    }
    c->dict = dict;
    c->dict_len = dict_len;
    c->dict_id = dict_len ? dyn_crc32c(dict, dict_len) : 0;
    c->level = level;
    c->algo = algo;
    c->checksum = checksum;
    return dyn_res_wrap(ctx, dyn_compressor_class_id, c, dyn_compressor_dispose);
}

static JSValue dyn_compressor_compress(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    dyn_compressor_t *c;
    uint8_t *src = NULL, *out = NULL;
    size_t src_len = 0, out_len = 0;
    JSValue result;
    int rc;

    (void)argc;
    /* Coerce the argument BEFORE resolving the handle: a valueOf/toString that
     * calls close() would otherwise free the scratch under us. */
    if (dyn_read_input(ctx, argv[0], &src, &src_len) < 0)
        return JS_EXCEPTION;
    c = (dyn_compressor_t *)dyn_res_native(ctx, this_val, dyn_compressor_class_id);
    if (!c) {
        free(src);
        return JS_EXCEPTION;
    }
    /* No busy flag, and that is a measured conclusion rather than an omission:
     * dyn_read_input TYPE-CHECKS its argument instead of coercing it, so no
     * user JS can run inside this call and the reused scratch cannot be
     * observed half-written. A flag here would be a bypass that never fires.
     * tests/test_lz4.js pins the reason. */
    if (c->algo == 0)
        rc = dyn_gzip_build_ctx(src, src_len, &c->cx, &out, &out_len);
    else if (c->algo == 2)
        rc = dyn_lz4_frame_build(src, src_len, c->level, c->checksum, &c->cx,
                                 &out, &out_len);
    else
        rc = dyn_lz4_compress(src, src_len, c->dict, c->dict_len, c->level,
                              &c->cx, &out, &out_len);
    free(src);
    if (rc < 0)
        return JS_ThrowOutOfMemory(ctx);
    if (c->dict_len) {
        /* stamp the dictionary identity ahead of the block */
        uint8_t *stamped = (uint8_t *)malloc(out_len + 4);
        if (!stamped) {
            free(out);
            return JS_ThrowOutOfMemory(ctx);
        }
        stamped[0] = (uint8_t)(c->dict_id & 0xff);
        stamped[1] = (uint8_t)((c->dict_id >> 8) & 0xff);
        stamped[2] = (uint8_t)((c->dict_id >> 16) & 0xff);
        stamped[3] = (uint8_t)((c->dict_id >> 24) & 0xff);
        memcpy(stamped + 4, out, out_len);
        free(out);
        out = stamped;
        out_len += 4;
    }
    result = dyn_bytes_to_uint8(ctx, out, out_len);
    free(out);
    return result;
}

static JSValue dyn_compressor_decompress(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv)
{
    dyn_compressor_t *c;
    uint8_t *src = NULL;
    size_t src_len = 0, off = 0;
    dyn_outbuf_t o = { NULL, 0, 0 };
    JSValue result;
    int rc;

    (void)argc;
    if (dyn_read_input(ctx, argv[0], &src, &src_len) < 0)
        return JS_EXCEPTION;
    c = (dyn_compressor_t *)dyn_res_native(ctx, this_val, dyn_compressor_class_id);
    if (!c) {
        free(src);
        return JS_EXCEPTION;
    }
    if (c->dict_len) {
        uint32_t id;
        if (src_len < 4) {
            free(src);
            return JS_ThrowTypeError(ctx, "record is too short to carry a dictionary id");
        }
        id = (uint32_t)src[0] | ((uint32_t)src[1] << 8) |
             ((uint32_t)src[2] << 16) | ((uint32_t)src[3] << 24);
        if (id != c->dict_id) {
            free(src);
            return JS_ThrowTypeError(ctx,
                "dictionary mismatch: record was written with a different dictionary");
        }
        off = 4;
    }
    if (c->algo == 0)
        rc = dyn_gunzip_decode(src + off, src_len - off, &o);
    else if (c->algo == 2)
        rc = dyn_lz4_frame_decode(src + off, src_len - off, &o);
    else
        rc = dyn_lz4_decompress(src + off, src_len - off, c->dict, c->dict_len, &o);
    free(src);
    if (rc < 0) {
        free(o.buf);
        return JS_ThrowTypeError(ctx, "Compressor.decompress: invalid record");
    }
    result = dyn_bytes_to_uint8(ctx, o.buf, o.len);
    free(o.buf);
    return result;
}

static JSValue dyn_compressor_get_algo(JSContext *ctx, JSValueConst this_val)
{
    dyn_compressor_t *c = (dyn_compressor_t *)dyn_res_native(ctx, this_val,
                                                dyn_compressor_class_id);
    if (!c)
        return JS_EXCEPTION;
    return JS_NewString(ctx, c->algo == 0 ? "gzip" :
                             (c->algo == 2 ? "lz4frame" : "lz4"));
}

static JSValue dyn_compressor_get_dictid(JSContext *ctx, JSValueConst this_val)
{
    dyn_compressor_t *c = (dyn_compressor_t *)dyn_res_native(ctx, this_val,
                                                dyn_compressor_class_id);
    if (!c)
        return JS_EXCEPTION;
    return c->dict_len ? JS_NewUint32(ctx, c->dict_id) : JS_NULL;
}

static const JSCFunctionListEntry dyn_compressor_proto[] = {
    JS_CFUNC_DEF("compress", 1, dyn_compressor_compress),
    JS_CFUNC_DEF("decompress", 1, dyn_compressor_decompress),
    JS_CGETSET_DEF("algo", dyn_compressor_get_algo, NULL),
    JS_CGETSET_DEF("dictId", dyn_compressor_get_dictid, NULL),
};

/* ---------- module registration -------------------------------------------- */


/* ==================================================================== *
 *  Dictionary -- the token-substitution codec (W8.4b)
 *
 *  A compiled capability in the strict sense: the configuration is the
 *  PHRASE LIST, the compiled artefact is an Aho-Corasick automaton over it,
 *  and one instance is reused across unbounded inputs. It is the second of
 *  the two dictionary mechanisms and it does not subsume the first --
 *  `new Compressor({dict})` seeds an LZ77 window and wins when the payload
 *  resembles a known block; this replaces known PHRASES with codes and wins
 *  where the payload is far too short for LZ77 to have built a window.
 *
 *  Its crossover is published with its LOSING row, per CLAUDE.md sec.4:
 *  a templated record shrinks hard, and bytes containing none of the
 *  phrases become one literal run plus a header, which is an EXPANSION.
 *  Both rows stay in the bench.
 *
 *  No busy flag, for the third recorded time and the same measured reason
 *  as Compressor and Hasher: dyn_read_input type-checks its argument rather
 *  than coercing it, so no user JS can run inside a call and a flag would
 *  be a bypass that never fires. The test pins the reason rather than the
 *  absence.
 * ==================================================================== */

typedef struct {
    dyn_dict_t *d;
} dyn_jsdict_t;

static JSClassID dyn_jsdict_class_id;

static void dyn_jsdict_dispose(void *native)
{
    dyn_jsdict_t *j = (dyn_jsdict_t *)native;
    if (!j)
        return;
    dyn_dict_free(j->d);
    free(j);
}

static const JSClassDef dyn_jsdict_class = {
    "Dictionary", .finalizer = dyn_res_finalizer,
};

static JSValue dyn_jsdict_ctor(JSContext *ctx, JSValueConst new_target,
                               int argc, JSValueConst *argv)
{
    dyn_jsdict_t *j;
    const uint8_t **phrases = NULL;
    size_t *lens = NULL;
    const char **owned = NULL;
    int64_t n = 0;
    int64_t i;
    int n_owned = 0;
    JSValue ret = JS_EXCEPTION;

    (void)new_target;
    if (argc < 1 || !JS_IsArray(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "new Dictionary(phrases[])");
    {
        JSValue lv = JS_GetPropertyStr(ctx, argv[0], "length");
        if (JS_IsException(lv))
            return JS_EXCEPTION;
        if (JS_ToInt64(ctx, &n, lv)) {
            JS_FreeValue(ctx, lv);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, lv);
    }
    if (n < 1)
        return JS_ThrowRangeError(ctx, "at least one phrase is required");
    if (n > DYN_DICT_MAX_PHRASES)
        return JS_ThrowRangeError(ctx, "at most %d phrases", DYN_DICT_MAX_PHRASES);

    /* Every phrase is materialised BEFORE the automaton is built: reading
     * element i can run a getter, and a half-built automaton must never be
     * observable (CLAUDE.md sec.8). */
    phrases = (const uint8_t **)calloc((size_t)n, sizeof(*phrases));
    lens = (size_t *)calloc((size_t)n, sizeof(*lens));
    owned = (const char **)calloc((size_t)n, sizeof(*owned));
    if (!phrases || !lens || !owned) {
        JS_ThrowOutOfMemory(ctx);
        goto done;
    }
    for (i = 0; i < n; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, argv[0], (uint32_t)i);
        size_t sl;
        const char *cs;
        if (JS_IsException(e))
            goto done;
        cs = JS_ToCStringLen(ctx, &sl, e);
        JS_FreeValue(ctx, e);
        if (!cs)
            goto done;
        owned[n_owned++] = cs;
        if (sl == 0) {
            JS_ThrowRangeError(ctx, "an empty phrase matches everywhere and "
                                    "encodes nothing; it is not a phrase");
            goto done;
        }
        phrases[i] = (const uint8_t *)cs;
        lens[i] = sl;
    }

    j = (dyn_jsdict_t *)calloc(1, sizeof(*j));
    if (!j) {
        JS_ThrowOutOfMemory(ctx);
        goto done;
    }
    j->d = dyn_dict_new(phrases, lens, (size_t)n);
    if (!j->d) {
        free(j);
        JS_ThrowOutOfMemory(ctx);
        goto done;
    }
    ret = dyn_res_wrap(ctx, dyn_jsdict_class_id, j, dyn_jsdict_dispose);

done:
    for (i = 0; i < n_owned; i++)
        JS_FreeCString(ctx, owned[i]);
    free(owned);
    free(phrases);
    free(lens);
    return ret;
}

static JSValue dyn_jsdict_run(JSContext *ctx, JSValueConst this_val, int argc,
                              JSValueConst *argv, int magic)
{
    dyn_jsdict_t *j;
    uint8_t *src = NULL;
    size_t src_len = 0;
    dyn_outbuf_t o = { NULL, 0, 0 };
    JSValue result;
    int rc;

    (void)argc;
    /* Coerce (type-check) first, resolve second -- the standing rule. */
    if (dyn_read_input(ctx, argv[0], &src, &src_len) < 0)
        return JS_EXCEPTION;
    j = (dyn_jsdict_t *)dyn_res_native(ctx, this_val, dyn_jsdict_class_id);
    if (!j) {
        free(src);
        return JS_EXCEPTION;
    }
    rc = magic ? dyn_dict_decompress(j->d, src, src_len, &o)
               : dyn_dict_compress(j->d, src, src_len, &o);
    free(src);
    if (rc < 0) {
        free(o.buf);
        return magic
            ? JS_ThrowTypeError(ctx, "not a record from this dictionary, or "
                                     "the record is malformed")
            : JS_ThrowOutOfMemory(ctx);
    }
    result = dyn_bytes_to_uint8(ctx, o.buf, o.len);
    free(o.buf);
    return result;
}

static JSValue dyn_jsdict_get_id(JSContext *ctx, JSValueConst this_val)
{
    dyn_jsdict_t *j = (dyn_jsdict_t *)dyn_res_native(ctx, this_val,
                                                     dyn_jsdict_class_id);
    if (!j)
        return JS_EXCEPTION;
    return JS_NewUint32(ctx, dyn_dict_id(j->d));
}

static JSValue dyn_jsdict_get_size(JSContext *ctx, JSValueConst this_val)
{
    dyn_jsdict_t *j = (dyn_jsdict_t *)dyn_res_native(ctx, this_val,
                                                     dyn_jsdict_class_id);
    if (!j)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)dyn_dict_count(j->d));
}

static const JSCFunctionListEntry dyn_jsdict_proto[] = {
    JS_CFUNC_MAGIC_DEF("compress", 1, dyn_jsdict_run, 0),
    JS_CFUNC_MAGIC_DEF("decompress", 1, dyn_jsdict_run, 1),
    JS_CGETSET_DEF("id", dyn_jsdict_get_id, NULL),
    JS_CGETSET_DEF("size", dyn_jsdict_get_size, NULL),
};

/* tar and zip: container formats over the codecs above. */
#include "dyna-archive.inc.c"

static const JSCFunctionListEntry dyn_compress_funcs[] = {
    JS_CFUNC_DEF("gzip", 1, dyn_gzip),
    JS_CFUNC_DEF("gunzip", 1, dyn_gunzip),
    JS_CFUNC_DEF("lz4Compress", 2, dyn_lz4_compress_js),
    JS_CFUNC_DEF("lz4Decompress", 2, dyn_lz4_decompress_js),
    JS_CFUNC_DEF("lz4Frame", 2, dyn_lz4_frame_js),
    JS_CFUNC_DEF("lz4Unframe", 2, dyn_lz4_unframe_js),
    JS_CFUNC_MAGIC_DEF("TarList", 2, dyn_tar_read, 0),
    JS_CFUNC_MAGIC_DEF("TarExtract", 2, dyn_tar_read, 1),
    JS_CFUNC_DEF("TarPack", 1, dyn_tar_pack),
    JS_CFUNC_MAGIC_DEF("ZipList", 2, dyn_zip_read, 0),
    JS_CFUNC_MAGIC_DEF("ZipRead", 3, dyn_zip_read, 1),
    JS_CFUNC_DEF("ZipPack", 2, dyn_zip_pack),
};

static int dyn_compress_init_module(JSContext *ctx, JSModuleDef *m)
{
    if (JS_SetModuleExportList(ctx, m, dyn_compress_funcs,
                               countof(dyn_compress_funcs)) < 0)
        return -1;
    if (dyn_register_class(ctx, m, &dyn_jsdict_class_id, &dyn_jsdict_class,
                           dyn_jsdict_proto, (int)countof(dyn_jsdict_proto),
                           dyn_jsdict_ctor, "Dictionary") < 0)
        return -1;
    return dyn_register_class(ctx, m, &dyn_compressor_class_id,
                              &dyn_compressor_class, dyn_compressor_proto,
                              countof(dyn_compressor_proto),
                              dyn_compressor_ctor, "Compressor");
}

int js_nat_init_compress(JSContext *ctx)
{
    JSModuleDef *m = JS_NewCModule(ctx, "dyna:compress", dyn_compress_init_module);
    if (!m)
        return -1;
    if (JS_AddModuleExportList(ctx, m, dyn_compress_funcs,
                               countof(dyn_compress_funcs)) < 0)
        return -1;
    if (JS_AddModuleExport(ctx, m, "Dictionary") < 0)
        return -1;
    return JS_AddModuleExport(ctx, m, "Compressor");
}

#endif /* CONFIG_NATIVE_MODULES && CONFIG_NATIVE_MODULE_COMPRESS */
