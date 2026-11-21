#ifndef ENCODING_INTERNAL_H
#define ENCODING_INTERNAL_H

#include "dyna-nat.h"
#include "core/dyn-common.h"
#include "cutils.h"
#include "core/dyn-hash.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_ENCODING)

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "core/dyn-simdk.h"
#include "core/dyn-codec.h"

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

int dyn_enc_view(JSContext* ctx, JSValueConst v, uint8_t** pp, size_t* pn);

int dyn_enc_bytes(JSContext* ctx, JSValueConst v, const uint8_t** data,
    size_t* len, const char** owned);

JSValue dyn_enc_hex_encode(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_enc_hex_decode(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_enc_base64_encode(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_enc_base64_decode(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_enc_base64url_encode(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_enc_base64url_decode(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_enc_base32_encode(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic);

JSValue dyn_enc_base32_decode(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic);

JSValue dyn_enc_base85_encode(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_enc_base85_decode(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

static inline int dyn_enc_view_w(JSContext* ctx, JSValueConst v, uint8_t** pp, size_t* pn)
{
    if (JS_CheckWritableBuffer(ctx, v) < 0)
        return -1;
    return dyn_enc_view(ctx, v, pp, pn);
}

int dyn_enc_into_out_as(JSContext* ctx, JSValueConst out, size_t need,
    const char* what, uint8_t** pp, size_t* pn);

static inline int dyn_enc_into_out(JSContext* ctx, JSValueConst out, size_t need,
    uint8_t** pp, size_t* pn)
{
    return dyn_enc_into_out_as(ctx, out, need, "decodeInto", pp, pn);
}

int dyn_enc_no_alias(JSContext* ctx, const uint8_t* data, size_t n,
    const uint8_t* out, size_t on, const char* what);

JSValue dyn_enc_hex_decode_into(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_enc_base64_decode_into(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_enc_base64url_decode_into(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_enc_base32_decode_into(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv,
    int magic);

JSValue dyn_enc_base85_decode_into(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_enc_hex_encode_into(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_enc_base64_encode_into(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_enc_base64url_encode_into(JSContext* ctx,
    JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_enc_base32_encode_into(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv,
    int magic);

JSValue dyn_enc_base85_encode_into(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_enc_detect_encoding(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_b58(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic);

JSValue dyn_b58_decode_into(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic);

JSValue dyn_b58_encode_into(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic);

JSValue dyn_basex(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic);

#include "core/dyn-sb.h"

typedef struct {
    uint8_t* p;
    size_t n, cap;
    int oom;
} dyn_buf_t;

void dyn_buf_init(dyn_buf_t* b);

void dyn_buf_free(dyn_buf_t* b);

void dyn_buf_put(dyn_buf_t* b, uint8_t c);

void dyn_buf_write(dyn_buf_t* b, const uint8_t* p, size_t n);

void dyn_buf_putc(dyn_buf_t* b, uint32_t cp);

JSValue dyn_json5_parse(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_stringify(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic);

extern JSClassID dyn_jp_class_id;

extern const JSClassDef dyn_jp_class;

JSValue dyn_jp_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv);

extern const JSCFunctionListEntry dyn_jp_proto[3];

JSValue dyn_qr_encode(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic);

#endif

#endif
