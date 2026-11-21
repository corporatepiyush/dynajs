#ifndef RANDOM_INTERNAL_H
#define RANDOM_INTERNAL_H

#include "dyna-nat.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_RANDOM)

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "core/dyn-prng.h"

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

extern JSClassID dyn_random_class_id;

JSValue dyn_random_next_u64(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_random_next_u53(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_random_next_float(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_random_next_bounded(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

int dyn_random_arg_real(JSContext* ctx, JSValueConst v, int has_arg,
    double def, double* out, const char* what);

int dyn_random_arg_int(JSContext* ctx, JSValueConst v, int has_arg,
    double def, double lo, double hi,
    double* out, const char* what);

JSValue dyn_random_fill(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

extern _Atomic(JSClassID) dyn_random_cid_f32;

extern _Atomic(JSClassID) dyn_random_cid_f64;

enum { DYN_FILL_NORMAL,
    DYN_FILL_UNIFORM,
    DYN_FILL_EXPONENTIAL };

JSValue dyn_random_fill_dist(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic);

static inline int dyn_random_view_ta(JSContext* ctx, JSValueConst v, JSValue* buf_out,
    size_t* byte_off, size_t* byte_len, size_t* bpe)
{
    JSValue buf = JS_GetTypedArrayBuffer(ctx, v, byte_off, byte_len, bpe);
    if (!JS_IsException(buf)) {
        *buf_out = buf;
        return 1;
    }
    JS_FreeValue(ctx, JS_GetException(ctx));
    return 0;
}

int dyn_random_array_len(JSContext* ctx, JSValueConst arr, double* len);

JSValue dyn_random_shuffle(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_random_sample(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_random_get_state(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_random_set_state(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

#endif

#endif
