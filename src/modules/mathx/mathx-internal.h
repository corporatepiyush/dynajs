#ifndef MATHX_INTERNAL_H
#define MATHX_INTERNAL_H

#include "dyna-nat.h"
#include "cutils.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_MATHX)

#include <stdint.h>
#include <float.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#define _REENTRANT
#include <math.h>
#include <limits.h>
#include "core/dyn-mathx.h"

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

int dyn_mathx_bigint_fits_i64(JSContext* ctx, JSValueConst v);

int dyn_mathx_bigint_mag_u64(JSContext* ctx, JSValueConst v,
    uint64_t* out, int* pneg);

int dyn_mathx_to_int64(JSContext* ctx, JSValueConst v, int64_t* out);

#define DYN_MATHX_LIMB_BASE 1000000000u

typedef struct {
    JSContext* ctx;
    uint32_t* limb;
    size_t len, cap;
} DynBigDec;

int dyn_bigdec_init(JSContext* ctx, DynBigDec* b, uint64_t v);

void dyn_bigdec_free(DynBigDec* b);

int dyn_bigdec_mul_small(DynBigDec* b, uint32_t factor);

JSValue dyn_mathx_is_prime(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

typedef struct {
    const double* p;
    JSValueConst arr;
    uint32_t len;
} mxst_src;

int mxst_src_of(JSContext* ctx, JSValueConst v, mxst_src* s);

static inline int mxst_elem(JSContext* ctx, JSValueConst arr, uint32_t i,
    double* out);

extern _Atomic(JSClassID) mxst_cid_f64;

JSClassID mxst_capture_f64_cid(JSContext* ctx);

static inline int mxst_elem(JSContext* ctx, JSValueConst arr, uint32_t i,
    double* out)
{
    JSValue* fv;
    uint32_t fc;
    JSValue e;
    if (JS_GetFastArray(ctx, arr, &fv, &fc) && i < fc) {
        int tag = JS_VALUE_GET_TAG(fv[i]);
        if (tag == JS_TAG_INT) {
            *out = (double)JS_VALUE_GET_INT(fv[i]);
            return 0;
        }
        if (JS_TAG_IS_FLOAT64(tag)) {
            *out = JS_VALUE_GET_FLOAT64(fv[i]);
            return 0;
        }
    }
    e = JS_GetPropertyUint32(ctx, arr, i);
    if (JS_IsException(e) || JS_ToFloat64(ctx, out, e)) {
        JS_FreeValue(ctx, e);
        return -1;
    }
    JS_FreeValue(ctx, e);
    return 0;
}

int dyn_mathx_tests_register(JSContext* ctx, JSValue stats);
int dyn_mathx_signal_register(JSContext* ctx, JSValue sig);
int dyn_mathx_geo_register(JSContext* ctx, JSValue geo);
int mxt_vec(JSContext* ctx, JSValueConst v, const char* fn, const char* what, double** out, uint32_t* n);
JSValue mxt_f64(JSContext* ctx, double* p, uint32_t n);
int mxt_opts_check(JSContext* ctx, JSValueConst opts, const char* fn, const char* const* keys, int nkeys);
int mxt_get(JSContext* ctx, JSValueConst opts, const char* key, JSValue* out);
int mxt_enum(JSContext* ctx, JSValueConst opts, const char* fn, const char* key, const char* const* names, int nn, int def, int* out);
int mxt_bool(JSContext* ctx, JSValueConst opts, const char* fn, const char* key, int def, int* out);
int mxt_num(JSContext* ctx, JSValueConst opts, const char* fn, const char* key, double def, double* out);
JSValue dyn_stats_sum(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_stats_mean(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_stats_var(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic);

JSValue dyn_stats_quantile(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int median);

JSValue dyn_stats_minmax(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int ismax);

JSValue dyn_stats_cov(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_stats_corr(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

#include "core/dyn-bits.h"

int mxbits_read8(JSContext* ctx, JSValueConst v, uint64_t* out);

int mxbits_read16(JSContext* ctx, JSValueConst v, uint64_t* out);

int mxbits_read32(JSContext* ctx, JSValueConst v, uint64_t* out);

int mxbits_read64(JSContext* ctx, JSValueConst v, uint64_t* out);

JSValue mxbits_reverse64(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue mxbits_reverse_bytes16(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue mxbits_reverse_bytes32(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue mxbits_reverse_bytes64(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue mxbits_rotate_left64(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

static inline uint64_t mxbits_rotate_right(uint64_t x, int k, int width)
{
    unsigned s = (unsigned)k & (unsigned)(width - 1);
    if (!s)
        return x;
    x = (x >> s) | (x << (width - s));
    return width == 64 ? x : (x & ((((uint64_t)1) << width) - 1));
}

JSValue mxbits_rotate_right64(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue mxbits_add32(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue mxbits_add64(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue mxbits_sub32(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue mxbits_sub64(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue mxbits_mul32(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue mxbits_mul64(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue mxbits_div32(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue mxbits_div64(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue mxbits_rem32(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue mxbits_rem64(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

#include "core/dyn-exprk.h"

extern JSClassID dyn_expr_class_id;

extern const JSClassDef dyn_expr_class;

JSValue dyn_expr_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv);

extern const JSCFunctionListEntry dyn_expr_proto[2];

int mx_f64_view(JSContext* ctx, JSValueConst v, const char* what,
    double** pp, size_t* pn);

int mx_cholesky(const double* A, double* L, size_t n, size_t* bi,
    size_t* bj, double* pivot);

#endif

#endif
