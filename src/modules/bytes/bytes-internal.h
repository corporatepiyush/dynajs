#ifndef BYTES_INTERNAL_H
#define BYTES_INTERNAL_H

#include "dyna-nat.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_BYTES)

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "core/dyn-simdk.h"

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

extern JSClassID dyn_bh_class_id;

JSValue dyn_bh_backing(JSValueConst v);

int dyn_bytes_view(JSContext* ctx, JSValueConst v, uint8_t** pp, size_t* pn);

int dyn_bytes_needle(JSContext* ctx, JSValueConst v, uint8_t* byte_out,
    const uint8_t** pat, size_t* plen);

#define DYN_BYTES_IDX_UNSET ((size_t)-1)

int dyn_bytes_from_index(JSContext* ctx, JSValueConst v, size_t* out);

JSValue dyn_bytes_index_of(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_bytes_last_index_of(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_bytes_count(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

#define DYN_UTF8_LOSSY_HAS_SIMD

typedef struct {
    JSValue u8;
    int is_ascii;
    int is_valid_utf8;
    int escaped;
} dyn_bh_t;

void dyn_bh_finalizer(JSRuntime* rt, JSValue val);

void dyn_bh_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark_func);

JSValue dyn_bh_wrap(JSContext* ctx, JSValue u8);

dyn_bh_t* dyn_bh_of(JSContext* ctx, JSValueConst v);

JSValue dyn_bh_ctor(JSContext* ctx, JSValueConst new_target, int argc,
    JSValueConst* argv);

JSValue dyn_bh_static(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv, int magic);

JSValue dyn_bh_get(JSContext* ctx, JSValueConst this_val, int magic);

JSValue dyn_bh_slice(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv);

#define DYN_BH_FORWARD(name, fn, maxargs)                                \
    static JSValue name(JSContext* ctx, JSValueConst this_val, int argc, \
        JSValueConst* argv)                                              \
    {                                                                    \
        dyn_bh_t* b = dyn_bh_of(ctx, this_val);                          \
        JSValueConst a[(maxargs) + 1];                                   \
        int i;                                                           \
        if (!b)                                                          \
            return JS_EXCEPTION;                                         \
        a[0] = b->u8;                                                    \
        for (i = 0; i < (maxargs); i++)                                  \
            a[i + 1] = (i < argc) ? argv[i] : JS_UNDEFINED;              \
        return fn(ctx, JS_UNDEFINED, (maxargs) + 1, a);                  \
    }

JSValue dyn_text_index_of_any(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_bh_to_string(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv);

JSValue dyn_text_is_valid_utf8(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_text_latin1_to_utf8(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_text_utf8_to_latin1(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_text_count_utf8(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_text_utf8_to_utf16(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_text_utf16_to_utf8(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_text_is_valid_utf16(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_text_count_utf16(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

typedef struct {
    JSValue str;
    int is_wide;
} dyn_txt_t;

extern JSClassID dyn_txt_class_id;

void dyn_txt_finalizer(JSRuntime* rt, JSValue val);

void dyn_txt_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark_func);

dyn_txt_t* dyn_txt_of(JSContext* ctx, JSValueConst v);

JSValue dyn_txt_ctor(JSContext* ctx, JSValueConst new_target, int argc,
    JSValueConst* argv);

JSValue dyn_txt_m_valid_utf16(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_txt_m_count_utf16_str(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_txt_get(JSContext* ctx, JSValueConst this_val, int magic);

JSValue dyn_txt_to_string(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_txt_to_bytes(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_txt_m_utf16_to_utf8(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

#include "dyna-charsets.h"

JSValue dyn_iconv_exists(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_iconv_decode(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_iconv_encode(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_iconv_list(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

#endif

#endif
