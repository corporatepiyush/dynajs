#include "dataframe-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_DATAFRAME)

#define DF_STEP_AND(acc, v) ((acc) &= (v))
#define DF_STEP_OR(acc, v) ((acc) |= (v))
#define DF_STEP_XOR(acc, v) ((acc) ^= (v))
#define DF_COMBINE_AND(p, q) ((p) & (q))
#define DF_COMBINE_OR(p, q) ((p) | (q))
#define DF_COMBINE_XOR(p, q) ((p) ^ (q))
#define DF_ALLANY_BLOCK 4096u
#define DF_SWAR_LOW7 0x7f7f7f7f7f7f7f7full
#define DF_SWAR_HIGH 0x8080808080808080ull
#define DF_SWAR_GATHER 0x0002040810204081ull

typedef void (*DFMaskFn)(const void* src, uint8_t* restrict dst, uint32_t n,
    double k0, double k1);

#define DF_DEFINE_MASK(name, suffix, ctype, EXPR)              \
    static void DF_KERNEL(mask, name, suffix)(const void* src, \
        uint8_t* restrict dst,                                 \
        uint32_t n, double k0, double k1)                      \
    {                                                          \
        const ctype* x = (const ctype*)src;                    \
        uint32_t i;                                            \
        for (i = 0; i < n; i++) {                              \
            double v = (double)x[i];                           \
            dst[i] = (uint8_t)(EXPR);                          \
        }                                                      \
    }

#define DF_MASK_ONE(suffix, ctype, tag) DF_DEFINE_MASK(DF_OP_NAME, suffix, ctype, DF_OP_EXPR)
#define DF_MASK_ENTRY(suffix, ctype, tag) [tag] = DF_KERNEL(mask, DF_OP_NAME, suffix),
#define DFL_TRUTHY_INT(v) ((v) != 0)
#define DFL_TRUTHY_FLOAT(v) ((v) == (v) && (v) != 0)

#define DFL_DEFINE_TRUE_COUNT(sfx, cty, TRUTHY)                             \
    static uint32_t dfl_true_count_##sfx(const cty* restrict x, uint32_t n) \
    {                                                                       \
        uint32_t i, ntrue = 0;                                              \
        for (i = 0; i < n; i++) {                                           \
            cty v = x[i];                                                   \
            ntrue += (uint32_t)(TRUTHY(v) != 0);                            \
        }                                                                   \
        return ntrue;                                                       \
    }                                                                       \
    static uint32_t dfl_true_count_masked_##sfx(const cty* restrict x,      \
        const uint8_t* restrict m,                                          \
        uint32_t n, uint32_t* psel)                                         \
    {                                                                       \
        uint32_t i, ntrue = 0, nsel = 0;                                    \
        for (i = 0; i < n; i++) {                                           \
            cty v = x[i];                                                   \
            int s = (m[i] != 0), t = (TRUTHY(v) != 0);                      \
            nsel += (uint32_t)s;                                            \
            ntrue += (uint32_t)(s & t);                                     \
        }                                                                   \
        *psel = nsel;                                                       \
        return ntrue;                                                       \
    }

const uint8_t* df_mask_arg(JSContext* ctx, JSValueConst v,
    uint32_t nrows, int* ok)
{
    JSValue buf;
    uint8_t* base;
    size_t off, len, bpe, ab;

    *ok = 1;
    if (JS_IsUndefined(v) || JS_IsNull(v))
        return NULL;
    buf = JS_GetTypedArrayBuffer(ctx, v, &off, &len, &bpe);
    if (JS_IsException(buf)) {
        *ok = 0;
        return NULL;
    }
    if (bpe != 1 || len != nrows) {
        JS_FreeValue(ctx, buf);
        JS_ThrowTypeError(ctx, "mask must be a Uint8Array of %u bytes", nrows);
        *ok = 0;
        return NULL;
    }
    if (JS_IsSharedArrayBuffer(buf)) {
        JS_FreeValue(ctx, buf);
        JS_ThrowTypeError(ctx, "mask must not be backed by a SharedArrayBuffer; pass a private copy (new Uint8Array(mask))");
        *ok = 0;
        return NULL;
    }
    base = JS_GetArrayBuffer(ctx, &ab, buf);
    JS_FreeValue(ctx, buf);
    if (!base || off > ab || len > ab - off) {
        JS_ThrowTypeError(ctx, "mask buffer is detached or out of bounds");
        *ok = 0;
        return NULL;
    }
    return base + off;
}

#define X(suffix, ctype, tag)                                           \
    DF_DEFINE_REDUCE_K(DF_ACCS_INT, band, suffix, ctype, uint32_t, ~0u, \
        DF_STEP_AND, DF_COMBINE_AND)                                    \
    DF_DEFINE_REDUCE_K(DF_ACCS_INT, bor, suffix, ctype, uint32_t, 0u,   \
        DF_STEP_OR, DF_COMBINE_OR)                                      \
    DF_DEFINE_REDUCE_K(DF_ACCS_INT, bxor, suffix, ctype, uint32_t, 0u,  \
        DF_STEP_XOR, DF_COMBINE_XOR)

DF_INT_TYPES(X)

#undef X

static int df_any_scan(const uint8_t* restrict m, uint32_t n)
{
    uint8_t a = 0;
    uint32_t i;
    for (i = 0; i < n; i++)
        a |= m[i];
    return a != 0;
}

static int df_all_scan(const uint8_t* restrict m, uint32_t n)
{
    uint8_t z = 0;
    uint32_t i;
    for (i = 0; i < n; i++)
        z |= (uint8_t)(m[i] == 0);
    return z == 0;
}

static int df_any_block(const uint8_t* restrict m, uint32_t n)
{
    uint32_t i = 0;
    while (i < n) {
        uint32_t e = n - i < DF_ALLANY_BLOCK ? n - i : DF_ALLANY_BLOCK;
        if (df_any_scan(m + i, e))
            return 1;
        i += e;
    }
    return 0;
}

static int df_all_block(const uint8_t* restrict m, uint32_t n)
{
    uint32_t i = 0;
    while (i < n) {
        uint32_t e = n - i < DF_ALLANY_BLOCK ? n - i : DF_ALLANY_BLOCK;
        if (!df_all_scan(m + i, e))
            return 0;
        i += e;
    }
    return 1;
}

static void df_pack_bits(uint32_t* restrict dst, const uint8_t* restrict m,
    uint32_t n)
{
    uint32_t i = 0;
#ifdef DF_PACK_SWAR
    for (; i + 32 <= n; i += 32) {
        uint64_t v0, v1, v2, v3;
        memcpy(&v0, m + i, 8);
        memcpy(&v1, m + i + 8, 8);
        memcpy(&v2, m + i + 16, 8);
        memcpy(&v3, m + i + 24, 8);
        v0 = (((v0 & DF_SWAR_LOW7) + DF_SWAR_LOW7) | v0) & DF_SWAR_HIGH;
        v1 = (((v1 & DF_SWAR_LOW7) + DF_SWAR_LOW7) | v1) & DF_SWAR_HIGH;
        v2 = (((v2 & DF_SWAR_LOW7) + DF_SWAR_LOW7) | v2) & DF_SWAR_HIGH;
        v3 = (((v3 & DF_SWAR_LOW7) + DF_SWAR_LOW7) | v3) & DF_SWAR_HIGH;
        dst[i >> 5] = (uint32_t)(((v0 * DF_SWAR_GATHER) >> 56) | ((v1 * DF_SWAR_GATHER) >> 56) << 8 | ((v2 * DF_SWAR_GATHER) >> 56) << 16 | ((v3 * DF_SWAR_GATHER) >> 56) << 24);
    }
#endif
    for (; i < n; i++)
        dst[i >> 5] |= (uint32_t)(m[i] != 0) << (i & 31);
}

JSValue dyn_df_bitwise(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    static const char* const names[] = { "BITWISE_AND", "BITWISE_OR",
        "BITWISE_XOR" };
    DataFrame* df;
    DFBound b;
    const uint8_t* mask;
    int idx, ok;
    uint32_t count = 0;
    uint32_t acc;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    idx = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (idx < 0)
        return JS_EXCEPTION;
    mask = df_mask_arg(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, df->nrows, &ok);
    if (!ok)
        return JS_EXCEPTION;

    if (dyn_df_bind(ctx, df, idx, &b))
        return JS_EXCEPTION;

#define DF_BIT_BY_OP(suffix, ctype)                                    \
    do {                                                               \
        const ctype* x = (const ctype*)b.p;                            \
        switch (magic) {                                               \
        case DF_BIT_AND:                                               \
            acc = mask ? df_band_masked_##suffix(x, mask, b.n, &count) \
                       : df_band_##suffix(x, b.n);                     \
            break;                                                     \
        case DF_BIT_OR:                                                \
            acc = mask ? df_bor_masked_##suffix(x, mask, b.n, &count)  \
                       : df_bor_##suffix(x, b.n);                      \
            break;                                                     \
        default:                                                       \
            acc = mask ? df_bxor_masked_##suffix(x, mask, b.n, &count) \
                       : df_bxor_##suffix(x, b.n);                     \
            break;                                                     \
        }                                                              \
    } while (0)
#define X(suffix, ctype, tag)        \
    case tag:                        \
        DF_BIT_BY_OP(suffix, ctype); \
        break;
    switch ((int)(b.type)) {
        DF_INT_TYPES(X)
    default:
        return JS_ThrowTypeError(ctx,
            "%s: column '%s' is %s; a bitwise reduction is defined only on "
            "integer columns (Int32/Uint32/Int16/Uint16/Int8/Uint8 Array)",
            names[magic], df->cols[idx].name, df_type_name(b.type));
    }
#undef X
#undef DF_BIT_BY_OP
    (void)count;

    switch ((int)(b.type)) {
    case DF_U8:
        acc &= 0xffu;
        break;
    case DF_U16:
        acc &= 0xffffu;
        break;
    default:
        break;
    }

    switch ((int)(b.type)) {
    case DF_I32:
    case DF_I16:
    case DF_I8:
        return JS_NewInt32(ctx, (int32_t)acc);
    default:
        return JS_NewUint32(ctx, acc);
    }
}

JSValue dyn_df_all_any(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    DataFrame* df;
    const uint8_t* mask;
    int ok, res;
    uint32_t n;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    mask = df_mask_arg(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, df->nrows, &ok);
    if (!ok)
        return JS_EXCEPTION;
    if (!mask)
        return JS_ThrowTypeError(ctx, "%s(mask): a Uint8Array mask of %u bytes "
                                      "is required",
            (magic & 1) == DF_ANY ? "ANY" : "ALL",
            df->nrows);
    n = df->nrows;
    res = (magic & 1) == DF_ANY ? df_any_block(mask, n) : df_all_block(mask, n);
    return JS_NewBool(ctx, res);
}

JSValue dyn_df_bitmask(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DataFrame* df;
    const uint8_t* mask;
    uint32_t* dst;
    uint32_t n, nwords;
    int ok;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    mask = df_mask_arg(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, df->nrows, &ok);
    if (!ok)
        return JS_EXCEPTION;
    if (!mask)
        return JS_ThrowTypeError(ctx, "BITMASK(mask): a Uint8Array mask of %u "
                                      "bytes is required",
            df->nrows);

    n = df->nrows;
    nwords = (n + 31u) >> 5;
    dst = calloc(nwords ? nwords : 1, sizeof(uint32_t));
    if (!dst)
        return JS_ThrowOutOfMemory(ctx);
    df_pack_bits(dst, mask, n);
    return df_to_typed_array(ctx, dst, (size_t)nwords * sizeof(uint32_t),
        JS_TYPED_ARRAY_UINT32);
}

JSValue dyn_df_compare(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    DataFrame* df;
    DFBound b;
    double thr;
    int idx;
    uint32_t i, n;
    uint8_t* dst;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    idx = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (idx < 0)
        return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &thr, argc > 1 ? argv[1] : JS_UNDEFINED))
        return JS_EXCEPTION;

    n = df->nrows;
    dst = malloc(n ? n : 1);
    if (!dst)
        return JS_ThrowOutOfMemory(ctx);
    if (dyn_df_bind(ctx, df, idx, &b)) {
        free(dst);
        return JS_EXCEPTION;
    }
    if (b.type == DF_STR) {
        free(dst);
        return JS_ThrowTypeError(ctx,
            "comparison: column '%s' is a string column; a numeric comparison "
            "would compare dictionary codes. Build it as a TypedArray "
            "(Float64Array/Int32Array/...) to compare values",
            df->cols[idx].name);
    }
    if (b.n > n)
        b.n = n;
    memset(dst, 0, n);

#define DF_CMP_LOOP(TY, OP)                          \
    do {                                             \
        const TY* x = b.p;                           \
        for (i = 0; i < b.n; i++)                    \
            dst[i] = (uint8_t)((double)x[i] OP thr); \
    } while (0)
#define DF_CMP_BY_OP(TY)         \
    do {                         \
        switch (magic) {         \
        case DF_GT:              \
            DF_CMP_LOOP(TY, >);  \
            break;               \
        case DF_GE:              \
            DF_CMP_LOOP(TY, >=); \
            break;               \
        case DF_LT:              \
            DF_CMP_LOOP(TY, <);  \
            break;               \
        case DF_LE:              \
            DF_CMP_LOOP(TY, <=); \
            break;               \
        case DF_EQ:              \
            DF_CMP_LOOP(TY, ==); \
            break;               \
        default:                 \
            DF_CMP_LOOP(TY, !=); \
            break;               \
        }                        \
    } while (0)

    switch ((int)(b.type)) {
    case DF_F64:
        DF_CMP_BY_OP(double);
        break;
    case DF_F32:
        DF_CMP_BY_OP(float);
        break;
    case DF_I32:
    case DF_STR:
        DF_CMP_BY_OP(int32_t);
        break;
    case DF_U32:
        DF_CMP_BY_OP(uint32_t);
        break;
    case DF_I16:
        DF_CMP_BY_OP(int16_t);
        break;
    case DF_U16:
        DF_CMP_BY_OP(uint16_t);
        break;
    case DF_I8:
        DF_CMP_BY_OP(int8_t);
        break;
    default:
        DF_CMP_BY_OP(uint8_t);
        break;
    }
#undef DF_CMP_BY_OP
#undef DF_CMP_LOOP
    return df_to_typed_array(ctx, dst, n, JS_TYPED_ARRAY_UINT8);
}

#define DF_OP_NAME fillna
#define DF_OP_EXPR (v != v) ? k0 : v

DF_NUMERIC_TYPES(DF_MAPK_ONE)

static const DFMapKFn df_mapk_fillna_tab[DF_STR] = { DF_NUMERIC_TYPES(DF_MAPK_ENTRY) };

#undef DF_OP_NAME
#undef DF_OP_EXPR
#define DF_OP_NAME clip
#define DF_OP_EXPR (v < k0) ? k0 : ((v > k1) ? k1 : v)

DF_NUMERIC_TYPES(DF_MAPK_ONE)

static const DFMapKFn df_mapk_clip_tab[DF_STR] = { DF_NUMERIC_TYPES(DF_MAPK_ENTRY) };

#undef DF_OP_NAME
#undef DF_OP_EXPR
#define DF_OP_NAME isna
#define DF_OP_EXPR v != v

DF_NUMERIC_TYPES(DF_MASK_ONE)

static const DFMaskFn df_mask_isna_tab[DF_STR] = { DF_NUMERIC_TYPES(DF_MASK_ENTRY) };

#undef DF_OP_NAME
#undef DF_OP_EXPR
#define DF_OP_NAME notna
#define DF_OP_EXPR v == v

DF_NUMERIC_TYPES(DF_MASK_ONE)

static const DFMaskFn df_mask_notna_tab[DF_STR] = { DF_NUMERIC_TYPES(DF_MASK_ENTRY) };

#undef DF_OP_NAME
#undef DF_OP_EXPR
#define DF_OP_NAME between
#define DF_OP_EXPR v >= k0&& v <= k1

DF_NUMERIC_TYPES(DF_MASK_ONE)

static const DFMaskFn df_mask_between_tab[DF_STR] = { DF_NUMERIC_TYPES(DF_MASK_ENTRY) };

#undef DF_OP_NAME
#undef DF_OP_EXPR

static void df_where_cc(const uint8_t* m, const double* a, const double* b,
    double* restrict dst, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++) {
        double p = a[i], q = b[i];
        dst[i] = m[i] != 0 ? p : q;
    }
}

static void df_where_cs(const uint8_t* m, const double* a, double kb,
    double* restrict dst, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++) {
        double p = a[i];
        dst[i] = m[i] != 0 ? p : kb;
    }
}

static void df_where_sc(const uint8_t* m, double ka, const double* b,
    double* restrict dst, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++) {
        double q = b[i];
        dst[i] = m[i] != 0 ? ka : q;
    }
}

static void df_where_ss(const uint8_t* m, double ka, double kb,
    double* restrict dst, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++)
        dst[i] = m[i] != 0 ? ka : kb;
}

JSValue dyn_df_isna(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    DataFrame* df;
    DFBound b;
    uint8_t* dst;
    uint32_t n;
    int idx;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    idx = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (idx < 0)
        return JS_EXCEPTION;

    n = df->nrows;
    dst = df_out_alloc(ctx, n, 1);
    if (!dst)
        return JS_EXCEPTION;
    if (df_bind_numeric(ctx, df, idx, &b, magic ? "NOT_NA" : "IS_NA")) {
        free(dst);
        return JS_EXCEPTION;
    }
    memset(dst, 0, n);
    (magic ? df_mask_notna_tab : df_mask_isna_tab)[b.type](
        b.p, dst, b.n < n ? b.n : n, 0.0, 0.0);
    return df_to_typed_array(ctx, dst, n, JS_TYPED_ARRAY_UINT8);
}

JSValue dyn_df_between(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DataFrame* df;
    DFBound b;
    uint8_t* dst;
    double lo, hi;
    uint32_t n;
    int idx;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    idx = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (idx < 0)
        return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &lo, argc > 1 ? argv[1] : JS_UNDEFINED) || JS_ToFloat64(ctx, &hi, argc > 2 ? argv[2] : JS_UNDEFINED))
        return JS_EXCEPTION;

    n = df->nrows;
    dst = df_out_alloc(ctx, n, 1);
    if (!dst)
        return JS_EXCEPTION;
    if (df_bind_numeric(ctx, df, idx, &b, "BETWEEN")) {
        free(dst);
        return JS_EXCEPTION;
    }
    memset(dst, 0, n);
    df_mask_between_tab[b.type](b.p, dst, b.n < n ? b.n : n, lo, hi);
    return df_to_typed_array(ctx, dst, n, JS_TYPED_ARRAY_UINT8);
}

JSValue dyn_df_clip(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DataFrame* df;
    DFBound b;
    double lo, hi, *dst;
    uint32_t n, span;
    int idx, out_idx = -1;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    idx = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (idx < 0)
        return JS_EXCEPTION;
    if (argc > 3 && !JS_IsUndefined(argv[3]) && !JS_IsNull(argv[3])) {
        if (df_out_opt(ctx, df, argv[3], "CLIP", &out_idx) < 0)
            return JS_EXCEPTION;
    }
    if (JS_ToFloat64(ctx, &lo, argc > 1 ? argv[1] : JS_UNDEFINED) || JS_ToFloat64(ctx, &hi, argc > 2 ? argv[2] : JS_UNDEFINED))
        return JS_EXCEPTION;
    if (isnan(lo) || isnan(hi))
        return JS_ThrowTypeError(ctx, "CLIP(col, lo, hi): bounds must not be NaN");
    if (lo > hi)
        return JS_ThrowRangeError(ctx, "CLIP(col, lo, hi): lo (%g) exceeds hi (%g)",
            lo, hi);

    n = df->nrows;
    dst = df_out_alloc(ctx, n, sizeof(double));
    if (!dst)
        return JS_EXCEPTION;
    if (df_bind_numeric(ctx, df, idx, &b, "CLIP")) {
        free(dst);
        return JS_EXCEPTION;
    }
    span = df_map_span(n, b.n, dst);
    df_mapk_clip_tab[b.type](b.p, dst, span, lo, hi);
    if (out_idx >= 0) {
        if (df_store_out(ctx, df, out_idx, dst, n)) {
            free(dst);
            return JS_EXCEPTION;
        }
        free(dst);
        return JS_DupValue(ctx, this_val);
    }
    return df_to_typed_array(ctx, dst, (size_t)n * sizeof(double),
        JS_TYPED_ARRAY_FLOAT64);
}

JSValue dyn_df_fillna(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DataFrame* df;
    DFBound b;
    double fill, *dst;
    uint32_t n, span;
    int idx, out_idx = -1;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    idx = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (idx < 0)
        return JS_EXCEPTION;
    if (argc > 2 && !JS_IsUndefined(argv[2]) && !JS_IsNull(argv[2])) {
        if (df_out_opt(ctx, df, argv[2], "FILL_NA", &out_idx) < 0)
            return JS_EXCEPTION;
    }
    if (argc > 1 && df_is_out_bag(ctx, argv[1]))
        return JS_ThrowTypeError(ctx,
            "FILL_NA: the fill value must be a number; the in-place form is "
            "FILL_NA(col, value, {out: \"column\"})");
    if (JS_ToFloat64(ctx, &fill, argc > 1 ? argv[1] : JS_UNDEFINED))
        return JS_EXCEPTION;

    n = df->nrows;
    dst = df_out_alloc(ctx, n, sizeof(double));
    if (!dst)
        return JS_EXCEPTION;
    if (df_bind_numeric(ctx, df, idx, &b, "FILL_NA")) {
        free(dst);
        return JS_EXCEPTION;
    }
    span = df_map_span(n, b.n, dst);
    df_mapk_fillna_tab[b.type](b.p, dst, span, fill, 0.0);
    if (out_idx >= 0) {
        if (df_store_out(ctx, df, out_idx, dst, n)) {
            free(dst);
            return JS_EXCEPTION;
        }
        free(dst);
        return JS_DupValue(ctx, this_val);
    }
    return df_to_typed_array(ctx, dst, (size_t)n * sizeof(double),
        JS_TYPED_ARRAY_FLOAT64);
}

JSValue dyn_df_where(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DataFrame* df;
    DFBound ba, bb;
    const uint8_t* mask;
    const double *pa = NULL, *pb = NULL;
    double ka = 0, kb = 0, *dst = NULL, *wa = NULL, *wb = NULL;
    uint32_t n, span, avail;
    int ia = -1, ib = -1, ok;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    if (JS_IsString(argc > 1 ? argv[1] : JS_UNDEFINED)) {
        ia = df_col_arg(ctx, df, argv[1]);
        if (ia < 0)
            return JS_EXCEPTION;
    } else if (JS_ToFloat64(ctx, &ka, argc > 1 ? argv[1] : JS_UNDEFINED)) {
        return JS_EXCEPTION;
    }
    if (JS_IsString(argc > 2 ? argv[2] : JS_UNDEFINED)) {
        ib = df_col_arg(ctx, df, argv[2]);
        if (ib < 0)
            return JS_EXCEPTION;
    } else if (JS_ToFloat64(ctx, &kb, argc > 2 ? argv[2] : JS_UNDEFINED)) {
        return JS_EXCEPTION;
    }
    mask = df_mask_arg(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, df->nrows, &ok);
    if (!ok)
        return JS_EXCEPTION;
    if (!mask)
        return JS_ThrowTypeError(ctx, "WHERE(mask, a, b): mask is required");

    n = df->nrows;
    dst = df_out_alloc(ctx, n, sizeof(double));
    if (!dst)
        return JS_EXCEPTION;
    avail = n;
    if (ia >= 0) {
        if (df_bind_numeric(ctx, df, ia, &ba, "WHERE"))
            goto fail;
        if (ba.n < avail)
            avail = ba.n;
    }
    if (ib >= 0) {
        if (df_bind_numeric(ctx, df, ib, &bb, "WHERE"))
            goto fail;
        if (bb.n < avail)
            avail = bb.n;
    }
    span = df_map_span(n, avail, dst);
    if (ia >= 0) {
        if (ba.type == DF_F64) {
            pa = ba.p;
        } else {
            wa = df_out_alloc(ctx, span, sizeof(double));
            if (!wa)
                goto fail;
            df_map_widen_tab[ba.type](ba.p, wa, span);
            pa = wa;
        }
    }
    if (ib >= 0) {
        if (bb.type == DF_F64) {
            pb = bb.p;
        } else {
            wb = df_out_alloc(ctx, span, sizeof(double));
            if (!wb)
                goto fail;
            df_map_widen_tab[bb.type](bb.p, wb, span);
            pb = wb;
        }
    }
    if (pa && pb)
        df_where_cc(mask, pa, pb, dst, span);
    else if (pa)
        df_where_cs(mask, pa, kb, dst, span);
    else if (pb)
        df_where_sc(mask, ka, pb, dst, span);
    else
        df_where_ss(mask, ka, kb, dst, span);
    free(wa);
    free(wb);
    return df_to_typed_array(ctx, dst, (size_t)n * sizeof(double),
        JS_TYPED_ARRAY_FLOAT64);
fail:
    free(wa);
    free(wb);
    free(dst);
    return JS_EXCEPTION;
}

JSValue dyn_df_drop_duplicates(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DataFrame* df;
    DFBound b;
    DfcSet set;
    const uint8_t* mask;
    uint8_t* dst;
    int idx, ok, fresh;
    uint32_t i, n, scan;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    idx = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (idx < 0)
        return JS_EXCEPTION;
    mask = df_mask_arg(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, df->nrows, &ok);
    if (!ok)
        return JS_EXCEPTION;

    n = df->nrows;
    dst = df_out_alloc(ctx, n, 1);
    if (!dst)
        return JS_EXCEPTION;
    memset(dst, 0, n ? n : 1);

    memset(&set, 0, sizeof(set));
    if (dyn_df_bind(ctx, df, idx, &b))
        goto fail;
    scan = b.n < n ? b.n : n;
    for (i = 0; i < scan; i++) {
        if (mask && !mask[i])
            continue;
        if (dfc_set_put(ctx, &set, dfc_key(df_get(b.p, b.type, i)), 0,
                NULL, &fresh))
            goto fail;
        dst[i] = (uint8_t)fresh;
    }
    dfc_set_free(&set);
    return df_to_typed_array(ctx, dst, n, JS_TYPED_ARRAY_UINT8);

fail:
    dfc_set_free(&set);
    free(dst);
    return JS_EXCEPTION;
}

#define DFL_TC_FLOAT(sfx, cty, tag) DFL_DEFINE_TRUE_COUNT(sfx, cty, DFL_TRUTHY_FLOAT)
#define DFL_TC_INT(sfx, cty, tag) DFL_DEFINE_TRUE_COUNT(sfx, cty, DFL_TRUTHY_INT)

DF_FLOAT_TYPES(DFL_TC_FLOAT)

DF_INT_TYPES(DFL_TC_INT)

#undef DFL_TC_FLOAT
#undef DFL_TC_INT

JSValue dyn_df_bool_reduce(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    static const char* const names[] = { "BOOL_AND", "BOOL_OR", "BOOL_XOR" };
    DataFrame* df;
    DFBound b;
    const uint8_t* mask;
    int idx, ok;
    uint32_t ntrue = 0, nsel = 0;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    idx = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (idx < 0)
        return JS_EXCEPTION;
    mask = df_mask_arg(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, df->nrows, &ok);
    if (!ok)
        return JS_EXCEPTION;

    if (dyn_df_bind(ctx, df, idx, &b))
        return JS_EXCEPTION;

#define X(sfx, cty, tag)                                                    \
    case tag:                                                               \
        if (mask)                                                           \
            ntrue = dfl_true_count_masked_##sfx((const cty*)b.p, mask, b.n, \
                &nsel);                                                     \
        else {                                                              \
            ntrue = dfl_true_count_##sfx((const cty*)b.p, b.n);             \
            nsel = b.n;                                                     \
        }                                                                   \
        break;
    switch ((int)(b.type)) {
        DF_NUMERIC_TYPES(X)
    default:
        return JS_ThrowTypeError(ctx,
            "%s: column '%s' is %s; a logical reduction folds the TRUTHINESS "
            "of the stored values, and a dictionary code is not one",
            names[magic], df->cols[idx].name, df_type_name(b.type));
    }
#undef X

    switch (magic) {
    case DFL_BOOL_AND:
        return JS_NewBool(ctx, ntrue == nsel);
    case DFL_BOOL_OR:
        return JS_NewBool(ctx, ntrue != 0);
    default:
        return JS_NewBool(ctx, (ntrue & 1u) != 0);
    }
}

JSValue dyn_df_dropna(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DataFrame* df;
    DFBound b;
    uint8_t* dst;
    int* sel;
    uint32_t i, k, nsel = 0, n;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    n = df->nrows;
    sel = df_out_alloc(ctx, (uint32_t)(argc > 0 ? argc : (int)df->ncols),
        sizeof(int));
    if (!sel)
        return JS_EXCEPTION;

    if (argc > 0) {
        for (k = 0; k < (uint32_t)argc; k++) {
            int idx = df_col_arg(ctx, df, argv[k]);
            if (idx < 0) {
                free(sel);
                return JS_EXCEPTION;
            }
            if (df->cols[idx].type == DF_STR) {
                JSValue e = JS_ThrowTypeError(ctx, "dropna: column '%s' is %s "
                                                   "and cannot hold NaN",
                    df->cols[idx].name,
                    df_type_name(DF_STR));
                free(sel);
                return e;
            }
            sel[nsel++] = idx;
        }
    } else {
        for (k = 0; k < df->ncols; k++)
            if (df->cols[k].type != DF_STR)
                sel[nsel++] = (int)k;
    }

    dst = df_out_alloc(ctx, n, 1);
    if (!dst) {
        free(sel);
        return JS_EXCEPTION;
    }
    memset(dst, 1, n);
    for (k = 0; k < nsel; k++) {
        uint32_t lim;
        if (dyn_df_bind(ctx, df, sel[k], &b)) {
            free(sel);
            free(dst);
            return JS_EXCEPTION;
        }
        lim = b.n < n ? b.n : n;
        if (b.type == DF_F64) {
            const double* x = b.p;
            for (i = 0; i < lim; i++)
                dst[i] &= (uint8_t)(x[i] == x[i]);
        } else if (b.type == DF_F32) {
            const float* x = b.p;
            for (i = 0; i < lim; i++)
                dst[i] &= (uint8_t)(x[i] == x[i]);
        }
        for (i = lim; i < n; i++)
            dst[i] = 0;
    }
    free(sel);
    return df_to_typed_array(ctx, dst, n, JS_TYPED_ARRAY_UINT8);
}

JSValue dyn_df_select(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DataFrame* df;
    DFBuilder B = { 0 };
    uint32_t *rows = NULL, *idxs = NULL;
    uint32_t i, nsel;
    JSValue res;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    if (argc < 1 || !JS_IsArray(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "SELECT(names[]): expected an array of "
                                      "column names");
    {
        JSValue lv = JS_GetPropertyStr(ctx, argv[0], "length");
        if (JS_IsException(lv) || JS_ToUint32(ctx, &nsel, lv)) {
            JS_FreeValue(ctx, lv);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, lv);
    }
    idxs = df_out_alloc(ctx, nsel ? nsel : 1, sizeof(uint32_t));
    if (!idxs)
        return JS_EXCEPTION;
    for (i = 0; i < nsel; i++) {
        JSValue nv = JS_GetPropertyUint32(ctx, argv[0], i);
        int idx;
        uint32_t j;
        if (JS_IsException(nv)) {
            free(idxs);
            return JS_ThrowOutOfMemory(ctx);
        }
        idx = df_col_arg(ctx, df, nv);
        JS_FreeValue(ctx, nv);
        if (idx < 0) {
            free(idxs);
            return JS_EXCEPTION;
        }
        for (j = 0; j < i; j++)
            if (idxs[j] == (uint32_t)idx) {
                JS_ThrowRangeError(ctx, "SELECT: column '%s' listed twice",
                    df->cols[idx].name);
                free(idxs);
                return JS_EXCEPTION;
            }
        idxs[i] = (uint32_t)idx;
    }
    if (dfb_init(ctx, &B, "SELECT", nsel, df->nrows)) {
        free(idxs);
        return JS_EXCEPTION;
    }
    rows = df_out_alloc(ctx, df->nrows ? df->nrows : 1, sizeof(uint32_t));
    if (!rows) {
        free(idxs);
        dfb_free(ctx, &B);
        return JS_EXCEPTION;
    }
    for (i = 0; i < df->nrows; i++)
        rows[i] = i;
    for (i = 0; i < nsel; i++) {
        int c = (int)idxs[i];
        if (dfb_add_gather(ctx, &B, df->cols[c].name, df, c, df->cols[c].type,
                rows, df->nrows))
            goto fail;
    }
    free(rows);
    free(idxs);
    res = dfb_seal(ctx, &B);
    return res;
fail:
    free(rows);
    free(idxs);
    dfb_free(ctx, &B);
    return JS_EXCEPTION;
}

JSValue dyn_df_drop_columns(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DataFrame* df;
    DFBuilder B = { 0 };
    uint32_t *rows = NULL, *drop = NULL;
    uint32_t i, j, ndrop;
    JSValue res;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    if (argc < 1 || !JS_IsArray(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "DROP_COLUMNS(names[]): expected an "
                                      "array of column names");
    {
        JSValue lv = JS_GetPropertyStr(ctx, argv[0], "length");
        if (JS_IsException(lv) || JS_ToUint32(ctx, &ndrop, lv)) {
            JS_FreeValue(ctx, lv);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, lv);
    }
    drop = df_out_alloc(ctx, ndrop ? ndrop : 1, sizeof(uint32_t));
    if (!drop)
        return JS_EXCEPTION;
    for (i = 0; i < ndrop; i++) {
        JSValue nv = JS_GetPropertyUint32(ctx, argv[0], i);
        int idx;
        uint32_t k;
        if (JS_IsException(nv)) {
            free(drop);
            return JS_ThrowOutOfMemory(ctx);
        }
        idx = df_col_arg(ctx, df, nv);
        JS_FreeValue(ctx, nv);
        if (idx < 0) {
            free(drop);
            return JS_EXCEPTION;
        }
        for (k = 0; k < i; k++)
            if (drop[k] == (uint32_t)idx) {
                JS_ThrowRangeError(ctx, "DROP_COLUMNS: column '%s' listed twice",
                    df->cols[idx].name);
                free(drop);
                return JS_EXCEPTION;
            }
        drop[i] = (uint32_t)idx;
    }
    rows = df_out_alloc(ctx, df->nrows ? df->nrows : 1, sizeof(uint32_t));
    if (!rows) {
        free(drop);
        return JS_EXCEPTION;
    }
    for (i = 0; i < df->nrows; i++)
        rows[i] = i;
    if (dfb_init(ctx, &B, "DROP_COLUMNS", df->ncols - ndrop, df->nrows)) {
        free(rows);
        free(drop);
        return JS_EXCEPTION;
    }
    for (i = 0; i < df->ncols; i++) {
        int todrop = 0;
        for (j = 0; j < ndrop; j++)
            if (drop[j] == i) {
                todrop = 1;
                break;
            }
        if (todrop)
            continue;
        if (dfb_add_gather(ctx, &B, df->cols[i].name, df, (int)i,
                df->cols[i].type, rows, df->nrows)) {
            free(rows);
            free(drop);
            dfb_free(ctx, &B);
            return JS_EXCEPTION;
        }
    }
    free(rows);
    free(drop);
    res = dfb_seal(ctx, &B);
    return res;
}

JSValue dyn_df_filter(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DataFrame* df;
    DFBuilder B = { 0 };
    const uint8_t* mask;
    uint32_t* rows = NULL;
    uint32_t i, nsel = 0, lim;
    int ok;
    JSValue res;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    mask = df_mask_arg(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, df->nrows, &ok);
    if (!ok)
        return JS_EXCEPTION;
    if (!mask)
        return JS_ThrowTypeError(ctx, "FILTER(mask): mask is required");
    rows = df_out_alloc(ctx, df->nrows ? df->nrows : 1, sizeof(uint32_t));
    if (!rows)
        return JS_EXCEPTION;
    lim = df->nrows;
    for (i = 0; i < lim; i++) {
        rows[nsel] = i;
        nsel += mask[i] != 0;
    }
    if (dfb_init(ctx, &B, "FILTER", df->ncols, nsel))
        goto fail;
    for (i = 0; i < df->ncols; i++)
        if (dfb_add_gather(ctx, &B, df->cols[i].name, df, (int)i,
                df->cols[i].type, rows, nsel))
            goto fail;
    free(rows);
    res = dfb_seal(ctx, &B);
    return res;
fail:
    free(rows);
    dfb_free(ctx, &B);
    return JS_EXCEPTION;
}

JSValue dyn_df_slice(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DataFrame* df;
    DFBuilder B = { 0 };
    uint32_t* rows = NULL;
    uint32_t i, nrows, start, end, nsel;
    double ds = 0, de = 0;
    JSValue res;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    if (argc > 0 && JS_ToFloat64(ctx, &ds, argv[0]))
        return JS_EXCEPTION;
    if (argc > 1 && !JS_IsUndefined(argv[1]) && JS_ToFloat64(ctx, &de, argv[1]))
        return JS_EXCEPTION;
    nrows = df->nrows;
    if (isnan(ds))
        ds = 0;
    if (isnan(de))
        de = 0;
    ds = ds < 0 ? ceil(ds) : floor(ds);
    de = de < 0 ? ceil(de) : floor(de);
    start = (ds < 0) ? (ds < -(double)nrows ? 0 : (uint32_t)(nrows + ds))
                     : (ds >= (double)nrows ? nrows : (uint32_t)ds);
    if (argc <= 1 || JS_IsUndefined(argv[1]))
        end = nrows;
    else
        end = (de < 0) ? (de < -(double)nrows ? 0 : (uint32_t)(nrows + de))
                       : (de >= (double)nrows ? nrows : (uint32_t)de);
    nsel = (end > start) ? end - start : 0;
    rows = df_out_alloc(ctx, nsel ? nsel : 1, sizeof(uint32_t));
    if (!rows)
        return JS_EXCEPTION;
    for (i = 0; i < nsel; i++)
        rows[i] = start + i;
    if (dfb_init(ctx, &B, "SLICE", df->ncols, nsel))
        goto fail;
    for (i = 0; i < df->ncols; i++)
        if (dfb_add_gather(ctx, &B, df->cols[i].name, df, (int)i,
                df->cols[i].type, rows, nsel))
            goto fail;
    free(rows);
    res = dfb_seal(ctx, &B);
    return res;
fail:
    free(rows);
    dfb_free(ctx, &B);
    return JS_EXCEPTION;
}

JSValue dyn_df_isin(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DataFrame* df;
    DFBound b;
    DfcSet set = { 0 };
    DfStrTab strt = { 0 };
    const char** strvals = NULL;
    uint8_t* dst;
    uint32_t i, n, nvals = 0, nstr = 0, scap = 0;
    int idx;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    idx = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (idx < 0)
        return JS_EXCEPTION;
    if (argc < 2 || !JS_IsArray(ctx, argv[1]))
        return JS_ThrowTypeError(ctx, "ISIN(col, values[]): expected an array "
                                      "of values");
    {
        JSValue lv = JS_GetPropertyStr(ctx, argv[1], "length");
        if (JS_IsException(lv) || JS_ToUint32(ctx, &nvals, lv)) {
            JS_FreeValue(ctx, lv);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, lv);
    }
    if (df->cols[idx].type == DF_STR) {
        for (i = 0; i < nvals; i++) {
            JSValue v = JS_GetPropertyUint32(ctx, argv[1], i);
            const char* s;
            uint32_t code;
            if (JS_IsException(v))
                goto fail_str;
            if (JS_IsUndefined(v)) {
                JS_ThrowTypeError(ctx, "ISIN: values[%u] is missing (a hole or undefined)", (unsigned)i);
                goto fail_str;
            }
            s = JS_ToCString(ctx, v);
            JS_FreeValue(ctx, v);
            if (!s)
                goto fail_str;
            if (nstr == scap) {
                uint32_t ncap = scap ? scap * 2 : 8;
                const char** grown = js_realloc(ctx, strvals, (size_t)ncap * sizeof(char*));
                if (!grown) {
                    JS_FreeCString(ctx, s);
                    goto fail_str;
                }
                strvals = grown;
                scap = ncap;
            }
            if (df_strtab_put(ctx, &strt, s, &code)) {
                JS_FreeCString(ctx, s);
                goto fail_str;
            }
            strvals[nstr++] = s;
        }
    } else {
        for (i = 0; i < nvals; i++) {
            JSValue v = JS_GetPropertyUint32(ctx, argv[1], i);
            double x;
            if (JS_IsException(v))
                goto fail_num;
            if (JS_IsUndefined(v)) {
                JS_ThrowTypeError(ctx, "ISIN: values[%u] is missing (a hole or undefined)", (unsigned)i);
                goto fail_num;
            }
            if (JS_ToFloat64(ctx, &x, v)) {
                JS_FreeValue(ctx, v);
                goto fail_num;
            }
            JS_FreeValue(ctx, v);
            if (dfc_set_put(ctx, &set, dfc_key(x), 0, NULL, NULL))
                goto fail_num;
        }
    }

    n = df->nrows;
    dst = df_out_alloc(ctx, n ? n : 1, 1);
    if (!dst)
        goto fail_both;
    if (dyn_df_bind(ctx, df, idx, &b))
        goto fail_dst;
    if (b.type == DF_STR) {
        const int32_t* codes = b.p;
        for (i = 0; i < n; i++) {
            int32_t code = codes[i];
            dst[i] = (code >= 0 && (uint32_t)code < df->cols[idx].dict_len && df_strtab_find(&strt, df->cols[idx].dict[code]) >= 0) ? 1 : 0;
        }
    } else {
        for (i = 0; i < n; i++)
            dst[i] = dfc_set_find(&set, dfc_key(df_get(b.p, b.type, i)), 0,
                         NULL)
                == 0;
    }
    dfc_set_free(&set);
    df_strtab_free(&strt);
    for (i = 0; i < nstr; i++)
        JS_FreeCString(ctx, strvals[i]);
    js_free(ctx, strvals);
    return df_to_typed_array(ctx, dst, n, JS_TYPED_ARRAY_UINT8);
fail_dst:
    free(dst);
fail_both:
    dfc_set_free(&set);
fail_str:
    df_strtab_free(&strt);
    for (i = 0; i < nstr; i++)
        JS_FreeCString(ctx, strvals[i]);
    js_free(ctx, strvals);
    return JS_EXCEPTION;
fail_num:
    dfc_set_free(&set);
    return JS_EXCEPTION;
}

static JSValue df_mask_run(JSContext* ctx, DataFrame* df, int argc,
    JSValueConst* argv, const char* fills, double filld, int have_fill)
{
    DFBuilder B = { 0 };
    const uint8_t* mask;
    uint32_t* rows = NULL;
    uint32_t i, c, n;
    int ok;
    JSValue res;

    mask = df_mask_arg(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, df->nrows, &ok);
    if (!ok)
        return JS_EXCEPTION;
    if (!mask)
        return JS_ThrowTypeError(ctx, "MASK(mask[, fill]): mask is required");
    n = df->nrows;
    rows = df_out_alloc(ctx, n ? n : 1, sizeof(uint32_t));
    if (!rows)
        goto fail;
    for (i = 0; i < n; i++)
        rows[i] = i;
    if (dfb_init(ctx, &B, "MASK", df->ncols, n))
        goto fail;
    for (c = 0; c < df->ncols; c++) {
        if (df->cols[c].type == DF_STR) {
            DFBound b;
            int32_t* codes;
            DfStrTab tab = { 0 };
            uint32_t fillcode = 0;
            if (dyn_df_bind(ctx, df, (int)c, &b)) {
                free(rows);
                dfb_free(ctx, &B);
                df_strtab_free(&tab);
                return JS_EXCEPTION;
            }
            codes = df_out_alloc(ctx, n ? n : 1, sizeof(int32_t));
            if (!codes) {
                df_strtab_free(&tab);
                free(rows);
                dfb_free(ctx, &B);
                return JS_EXCEPTION;
            }
            for (i = 0; i < df->cols[c].dict_len; i++)
                if (df_strtab_put(ctx, &tab, df->cols[c].dict[i], &fillcode)) {
                    df_strtab_free(&tab);
                    free(rows);
                    dfb_free(ctx, &B);
                    return JS_EXCEPTION;
                }
            if (have_fill) {
                const char* fs = fills ? fills : "";
                if (df_strtab_put(ctx, &tab, fs, &fillcode)) {
                    df_strtab_free(&tab);
                    free(rows);
                    dfb_free(ctx, &B);
                    return JS_EXCEPTION;
                }
            }
            {
                uint32_t miss_code = 0;
                int miss_known = 0;
                for (i = 0; i < n; i++) {
                    if (mask[i]) {
                        codes[i] = ((const int32_t*)b.p)[i];
                        continue;
                    }
                    if (!miss_known) {
                        if (df_strtab_put(ctx, &tab, fills ? fills : "", &miss_code)) {
                            df_strtab_free(&tab);
                            free(rows);
                            dfb_free(ctx, &B);
                            return JS_EXCEPTION;
                        }
                        miss_known = 1;
                    }
                    codes[i] = (int32_t)miss_code;
                }
            }
            if (dfb_add_str(ctx, &B, df->cols[c].name, codes, tab.dict,
                    tab.dict_len, n)) {
                tab.dict = NULL;
                tab.dict_len = 0;
                df_strtab_free(&tab);
                free(rows);
                dfb_free(ctx, &B);
                return JS_EXCEPTION;
            }
            tab.dict = NULL;
            tab.dict_len = 0;
            df_strtab_free(&tab);
        } else {
            DFBound b;
            double* out;
            if (dyn_df_bind(ctx, df, (int)c, &b)) {
                free(rows);
                dfb_free(ctx, &B);
                return JS_EXCEPTION;
            }
            out = df_out_alloc(ctx, n ? n : 1, sizeof(double));
            if (!out) {
                free(rows);
                dfb_free(ctx, &B);
                return JS_EXCEPTION;
            }
            {
                double fv = have_fill ? filld : DYN_NAN;
                uint32_t span = b.n < n ? b.n : n;
                df_widen(&b, out, span);
                for (i = 0; i < span; i++)
                    out[i] = mask[i] ? out[i] : fv;
                for (; i < n; i++)
                    out[i] = mask[i] ? df_get(b.p, b.type, i) : fv;
            }
            if (dfb_add(ctx, &B, df->cols[c].name, DF_F64, out, n)) {
                out = NULL;
                free(rows);
                dfb_free(ctx, &B);
                return JS_EXCEPTION;
            }
        }
    }
    free(rows);
    res = dfb_seal(ctx, &B);
    return res;
fail:
    free(rows);
    dfb_free(ctx, &B);
    return JS_EXCEPTION;
}

JSValue dyn_df_mask(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DataFrame* df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    double filld = 0;
    const char* fills = NULL;
    int have_fill = 0;
    JSValue res;

    if (!df)
        return JS_EXCEPTION;
    if (argc > 1 && !JS_IsUndefined(argv[1])) {
        if (JS_IsString(argv[1])) {
            size_t sl;
            fills = JS_ToCStringLen(ctx, &sl, argv[1]);
            if (!fills)
                return JS_EXCEPTION;
        } else if (JS_ToFloat64(ctx, &filld, argv[1])) {
            return JS_EXCEPTION;
        }
        have_fill = 1;
    }
    res = df_mask_run(ctx, df, argc, argv, fills, filld, have_fill);
    if (fills)
        JS_FreeCString(ctx, fills);
    return res;
}

#endif
