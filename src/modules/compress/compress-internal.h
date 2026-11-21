#ifndef COMPRESS_INTERNAL_H
#define COMPRESS_INTERNAL_H

#include "dyna-nat.h"
#include "core/dyn-common.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_COMPRESS)

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "core/dyn-compress.h"
#include "core/dyn-dict.h"
#include "core/dyn-hash.h"
#include "core/dyn-arck.h"

#if defined(CONFIG_ZSTD)
#include <zstd.h>
#endif

#if defined(__APPLE__)
#include <compression.h>
#elif defined(CONFIG_BROTLI)
#include <brotli/decode.h>
#include <brotli/encode.h>
#endif

#include "core/dyn-snappy.h"

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

int dyn_read_input(JSContext* ctx, JSValueConst val, uint8_t** pout,
    size_t* plen);

JSValue dyn_bytes_to_uint8(JSContext* ctx, const uint8_t* data, size_t len);

extern const char* const cpr_level_keys[1];

extern const char* const cpr_asstring_keys[2];

int dyn_cpr_maxout(JSContext* ctx, JSValueConst opts, dyn_outbuf_t* o);

static inline size_t dyn_cpr_cap(const dyn_outbuf_t* o)
{
    return o->limit && o->limit < DYN_MAX_OUTPUT ? o->limit : DYN_MAX_OUTPUT;
}

static inline JSValue dyn_cpr_cap_error(JSContext* ctx, const dyn_outbuf_t* o,
    const char* what)
{
    return JS_ThrowRangeError(ctx,
        "%s: decompressed output exceeds maxOutputBytes (%llu)",
        what, (unsigned long long)o->limit);
}

int dyn_lz4_opts(JSContext* ctx, JSValueConst opts, int level_hi,
    int* plevel, uint8_t** pdict, size_t* pdict_len,
    int clamp_level);

JSValue dyn_lz4_compress_js(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_lz4_decompress_js(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_lz4_frame_js(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_lz4_unframe_js(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

#if defined(CONFIG_ZSTD)
#define DYN_HAVE_ZSTD 1
#else
#define DYN_HAVE_ZSTD 0
#endif

#if defined(__APPLE__) || defined(CONFIG_BROTLI)
#define DYN_HAVE_BROTLI 1
#else
#define DYN_HAVE_BROTLI 0
#endif

int dyn_out_grow(dyn_outbuf_t* o, size_t need, size_t cap);

#if DYN_HAVE_ZSTD
int dyn_zstd_decode(const uint8_t* src, size_t len, size_t cap,
    dyn_outbuf_t* o, const char** pwhy);

int dyn_zstd_encode(const uint8_t* src, size_t len, int level, int checksum,
    uint8_t** pout, size_t* pout_len);
#endif

#if DYN_HAVE_BROTLI
int dyn_brotli_decode(const uint8_t* src, size_t len, size_t cap,
    dyn_outbuf_t* o, const char** pwhy);

int dyn_brotli_encode(const uint8_t* src, size_t len, int level,
    uint8_t** pout, size_t* pout_len);
#endif

#if !DYN_HAVE_ZSTD
static int dyn_zstd_encode(const uint8_t* src, size_t len, int level, int checksum,
    uint8_t** pout, size_t* pout_len)
{
    (void)src;
    (void)len;
    (void)level;
    (void)checksum;
    (void)pout;
    (void)pout_len;
    return -1;
}
static int dyn_zstd_decode(const uint8_t* src, size_t len, size_t cap,
    dyn_outbuf_t* o, const char** pwhy)
{
    (void)src;
    (void)len;
    (void)cap;
    (void)o;
    *pwhy = "zstd support not compiled in";
    return -1;
}
#endif

#if !DYN_HAVE_BROTLI
static int dyn_brotli_encode(const uint8_t* src, size_t len, int level,
    uint8_t** pout, size_t* pout_len)
{
    (void)src;
    (void)len;
    (void)level;
    (void)pout;
    (void)pout_len;
    return -1;
}
static int dyn_brotli_decode(const uint8_t* src, size_t len, size_t cap,
    dyn_outbuf_t* o, const char** pwhy)
{
    (void)src;
    (void)len;
    (void)cap;
    (void)o;
    *pwhy = "brotli support not compiled in";
    return -1;
}
#endif

JSValue dyn_zstd_js(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv);

JSValue dyn_unzstd_js(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv);

JSValue dyn_brotli_js(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv);

extern JSClassID dyn_compressor_class_id;

JSValue dyn_compressor_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv);

JSValue dyn_compressor_compress(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_compressor_decompress(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_compressor_get_algo(JSContext* ctx, JSValueConst this_val);

JSValue dyn_compressor_get_dictid(JSContext* ctx, JSValueConst this_val);

extern JSClassID dyn_jsdict_class_id;

JSValue dyn_jsdict_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv);

JSValue dyn_jsdict_run(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv, int magic);

JSValue dyn_jsdict_get_id(JSContext* ctx, JSValueConst this_val);

JSValue dyn_jsdict_get_size(JSContext* ctx, JSValueConst this_val);

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

JSValue dyn_tar_read(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic);

JSValue dyn_tar_pack(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_zip_read(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic);

JSValue dyn_zip_pack(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_deflate_js(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_inflate_js(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_zip_read_at(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_zip_extract_all(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

#endif

#endif
