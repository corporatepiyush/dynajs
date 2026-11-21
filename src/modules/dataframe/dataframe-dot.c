#include "dataframe-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_DATAFRAME)

#define DF_DEFINE_DOT(name, atype, btype)                                      \
    static double df_dot_##name(const atype* restrict xa,                      \
        const btype* restrict xb,                                              \
        const uint8_t* restrict m, uint32_t n)                                 \
    {                                                                          \
        double a0 = 0, a1 = 0, a2 = 0, a3 = 0, a4 = 0, a5 = 0, a6 = 0, a7 = 0; \
        uint32_t i = 0;                                                        \
        if (m) {                                                               \
            for (; i + 8 <= n; i += 8) {                                       \
                a0 += m[i] ? (double)xa[i] * (double)xb[i] : 0.0;              \
                a1 += m[i + 1] ? (double)xa[i + 1] * (double)xb[i + 1] : 0.0;  \
                a2 += m[i + 2] ? (double)xa[i + 2] * (double)xb[i + 2] : 0.0;  \
                a3 += m[i + 3] ? (double)xa[i + 3] * (double)xb[i + 3] : 0.0;  \
                a4 += m[i + 4] ? (double)xa[i + 4] * (double)xb[i + 4] : 0.0;  \
                a5 += m[i + 5] ? (double)xa[i + 5] * (double)xb[i + 5] : 0.0;  \
                a6 += m[i + 6] ? (double)xa[i + 6] * (double)xb[i + 6] : 0.0;  \
                a7 += m[i + 7] ? (double)xa[i + 7] * (double)xb[i + 7] : 0.0;  \
            }                                                                  \
            for (; i < n; i++)                                                 \
                a0 += m[i] ? (double)xa[i] * (double)xb[i] : 0.0;              \
        } else {                                                               \
            for (; i + 8 <= n; i += 8) {                                       \
                a0 += (double)xa[i] * (double)xb[i];                           \
                a1 += (double)xa[i + 1] * (double)xb[i + 1];                   \
                a2 += (double)xa[i + 2] * (double)xb[i + 2];                   \
                a3 += (double)xa[i + 3] * (double)xb[i + 3];                   \
                a4 += (double)xa[i + 4] * (double)xb[i + 4];                   \
                a5 += (double)xa[i + 5] * (double)xb[i + 5];                   \
                a6 += (double)xa[i + 6] * (double)xb[i + 6];                   \
                a7 += (double)xa[i + 7] * (double)xb[i + 7];                   \
            }                                                                  \
            for (; i < n; i++)                                                 \
                a0 += (double)xa[i] * (double)xb[i];                           \
        }                                                                      \
        return ((a0 + a1) + (a2 + a3)) + ((a4 + a5) + (a6 + a7));              \
    }

#define DF_DOT_BLOCK 128
#define X(suffix, ctype, tag) DF_DEFINE_DOT(suffix##_##suffix, ctype, ctype)

DF_NUMERIC_TYPES(X)

#undef X

DF_DEFINE_DOT(f64_f32, double, float)

DF_DEFINE_DOT(f64_i32, double, int32_t)

DF_DEFINE_DOT(f64_u32, double, uint32_t)

DF_DEFINE_DOT(f64_i16, double, int16_t)

DF_DEFINE_DOT(f64_u16, double, uint16_t)

DF_DEFINE_DOT(f64_i8, double, int8_t)

DF_DEFINE_DOT(f64_u8, double, uint8_t)

static double df_dot_generic(const void* pa, DFType ta,
    const void* pb, DFType tb,
    const uint8_t* restrict m, uint32_t n)
{
    double ba[DF_DOT_BLOCK], bb[DF_DOT_BLOCK], acc = 0;
    uint32_t i = 0;

    while (i < n) {
        uint32_t e = n - i < (uint32_t)DF_DOT_BLOCK ? n - i
                                                    : (uint32_t)DF_DOT_BLOCK;
        df_widen_block(ba, pa, ta, i, e);
        df_widen_block(bb, pb, tb, i, e);
        acc += df_dot_f64_f64(ba, bb, m ? m + i : NULL, e);
        i += e;
    }
    return acc;
}

static double df_dot_dispatch(const void* pa, DFType ta,
    const void* pb, DFType tb,
    const uint8_t* m, uint32_t n)
{
    if (ta == tb) {
        switch ((int)(ta)) {
#define X(suffix, ctype, tag)                               \
    case tag:                                               \
        return df_dot_##suffix##_##suffix((const ctype*)pa, \
            (const ctype*)pb, m, n);
            DF_NUMERIC_TYPES(X)
#undef X
        default:
            break;
        }
    } else if (ta == DF_F64) {
        const double* xa = (const double*)pa;
        switch ((int)(tb)) {
        case DF_F32:
            return df_dot_f64_f32(xa, (const float*)pb, m, n);
        case DF_I32:
            return df_dot_f64_i32(xa, (const int32_t*)pb, m, n);
        case DF_U32:
            return df_dot_f64_u32(xa, (const uint32_t*)pb, m, n);
        case DF_I16:
            return df_dot_f64_i16(xa, (const int16_t*)pb, m, n);
        case DF_U16:
            return df_dot_f64_u16(xa, (const uint16_t*)pb, m, n);
        case DF_I8:
            return df_dot_f64_i8(xa, (const int8_t*)pb, m, n);
        case DF_U8:
            return df_dot_f64_u8(xa, (const uint8_t*)pb, m, n);
        default:
            break;
        }
    }
    return df_dot_generic(pa, ta, pb, tb, m, n);
}

JSValue dyn_df_dot_product(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DataFrame* df;
    DFBound ba, bb;
    const uint8_t* mask;
    const void *pa, *pb;
    DFType ta, tb;
    int ia, ib, ok;
    uint32_t n;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    ia = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (ia < 0)
        return JS_EXCEPTION;
    ib = df_col_arg(ctx, df, argc > 1 ? argv[1] : JS_UNDEFINED);
    if (ib < 0)
        return JS_EXCEPTION;
    mask = df_mask_arg(ctx, argc > 2 ? argv[2] : JS_UNDEFINED, df->nrows, &ok);
    if (!ok)
        return JS_EXCEPTION;

    if (dyn_df_bind(ctx, df, ia, &ba) || dyn_df_bind(ctx, df, ib, &bb))
        return JS_EXCEPTION;
    if (ba.type == DF_STR || bb.type == DF_STR)
        return JS_ThrowTypeError(ctx, "cannot multiply a string column");

    pa = ba.p;
    ta = ba.type;
    pb = bb.p;
    tb = bb.type;
    n = ba.n < bb.n ? ba.n : bb.n;
    if (ta != DF_F64 && tb == DF_F64) {
        const void* tp = pa;
        DFType tt = ta;
        pa = pb;
        ta = tb;
        pb = tp;
        tb = tt;
    }
    return JS_NewFloat64(ctx, df_dot_dispatch(pa, ta, pb, tb, mask, n));
}

#endif
