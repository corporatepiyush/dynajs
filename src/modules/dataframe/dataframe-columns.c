#include "dataframe-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_DATAFRAME)

typedef void (*DFCombFn)(const double* a, const void* src,
    double* restrict dst, uint32_t n);

#define DF_DEFINE_COMB(name, suffix, ctype, EXPR)                               \
    static void DF_KERNEL(comb, name, suffix)(const double* a, const void* src, \
        double* restrict dst, uint32_t n)                                       \
    {                                                                           \
        const ctype* y = (const ctype*)src;                                     \
        uint32_t i;                                                             \
        for (i = 0; i < n; i++) {                                               \
            double p = a[i], q = (double)y[i];                                  \
            dst[i] = (EXPR);                                                    \
        }                                                                       \
    }

#define DF_COMB_ONE(suffix, ctype, tag) DF_DEFINE_COMB(DF_OP_NAME, suffix, ctype, DF_OP_EXPR)
#define DF_COMB_ENTRY(suffix, ctype, tag) [tag] = DF_KERNEL(comb, DF_OP_NAME, suffix),

int df_type_of_value(JSValueConst v, DFType* out)
{
    JSClassID cid = JS_GetClassID(v);
    int i;
    for (i = 0; i < df_ta_class_count; i++)
        if (df_ta_class[i].id == cid) {
            *out = df_ta_class[i].type;
            return 0;
        }
    return -1;
}

void df_col_free(DFColumn* c)
{
    uint32_t i;
    free(c->name);
    free(c->codes);
    if (c->dict) {
        for (i = 0; i < c->dict_len; i++)
            free(c->dict[i]);
        free(c->dict);
    }
}

const char* df_type_name(DFType t)
{
    switch ((int)(t)) {
    case DF_F64:
        return "Float64Array";
    case DF_F32:
        return "Float32Array";
    case DF_I32:
        return "Int32Array";
    case DF_U32:
        return "Uint32Array";
    case DF_I16:
        return "Int16Array";
    case DF_U16:
        return "Uint16Array";
    case DF_I8:
        return "Int8Array";
    case DF_U8:
        return "Uint8Array";
    default:
        return "a dictionary-encoded string column";
    }
}

int dyn_df_bind(JSContext* ctx, const DataFrame* df, int idx, DFBound* b)
{
    const DFColumn* c = &df->cols[idx];
    uint8_t* base;
    size_t ab_size, need;

    if (c->type == DF_STR) {
        b->p = c->codes;
        b->type = DF_STR;
        b->n = c->length;
        return 0;
    }
    base = JS_GetArrayBuffer(ctx, &ab_size, c->buffer);
    if (!base) {
        JS_ThrowTypeError(ctx, "column '%s': ArrayBuffer is detached", c->name);
        return -1;
    }
    need = (size_t)c->length * df_elt_size(c->type);
    if (c->byte_offset > ab_size || need > ab_size - c->byte_offset) {
        JS_ThrowRangeError(ctx, "column '%s': view is out of bounds "
                                "(buffer resized?)",
            c->name);
        return -1;
    }
    b->p = base + c->byte_offset;
    b->type = c->type;
    b->n = c->length;
    return 0;
}

int df_col_arg(JSContext* ctx, const DataFrame* df, JSValueConst v)
{
    size_t len;
    const char* s = JS_ToCStringLen(ctx, &len, v);
    int idx;
    if (!s)
        return -1;
    if (strlen(s) != len) {
        JS_FreeCString(ctx, s);
        JS_ThrowRangeError(ctx, "column name contains a NUL character");
        return -1;
    }
    idx = df_find_col(df, s);
    if (idx < 0)
        JS_ThrowRangeError(ctx, "no such column: '%s'", s);
    JS_FreeCString(ctx, s);
    return idx;
}

void df_widen_block(double* restrict dst, const void* src, DFType t,
    uint32_t off, uint32_t n)
{
    uint32_t i;
#define DF_WIDEN(TY)                        \
    do {                                    \
        const TY* s = (const TY*)src + off; \
        for (i = 0; i < n; i++)             \
            dst[i] = (double)s[i];          \
    } while (0)
    switch ((int)(t)) {
    case DF_F64:
        DF_WIDEN(double);
        break;
    case DF_F32:
        DF_WIDEN(float);
        break;
    case DF_I32:
        DF_WIDEN(int32_t);
        break;
    case DF_U32:
        DF_WIDEN(uint32_t);
        break;
    case DF_I16:
        DF_WIDEN(int16_t);
        break;
    case DF_U16:
        DF_WIDEN(uint16_t);
        break;
    case DF_I8:
        DF_WIDEN(int8_t);
        break;
    default:
        DF_WIDEN(uint8_t);
        break;
    }
#undef DF_WIDEN
}

#define DF_OP_NAME add
#define DF_OP_EXPR p + q

DF_NUMERIC_TYPES(DF_COMB_ONE)

static const DFCombFn df_comb_add_tab[DF_STR] = { DF_NUMERIC_TYPES(DF_COMB_ENTRY) };

#undef DF_OP_NAME
#undef DF_OP_EXPR
#define DF_OP_NAME sub
#define DF_OP_EXPR p - q

DF_NUMERIC_TYPES(DF_COMB_ONE)

static const DFCombFn df_comb_sub_tab[DF_STR] = { DF_NUMERIC_TYPES(DF_COMB_ENTRY) };

#undef DF_OP_NAME
#undef DF_OP_EXPR
#define DF_OP_NAME mul
#define DF_OP_EXPR p* q

DF_NUMERIC_TYPES(DF_COMB_ONE)

static const DFCombFn df_comb_mul_tab[DF_STR] = { DF_NUMERIC_TYPES(DF_COMB_ENTRY) };

#undef DF_OP_NAME
#undef DF_OP_EXPR
#define DF_OP_NAME div
#define DF_OP_EXPR p / q

DF_NUMERIC_TYPES(DF_COMB_ONE)

static const DFCombFn df_comb_div_tab[DF_STR] = { DF_NUMERIC_TYPES(DF_COMB_ENTRY) };

#undef DF_OP_NAME
#undef DF_OP_EXPR
#define DF_OP_NAME pow
#define DF_OP_EXPR pow(p, q)

DF_NUMERIC_TYPES(DF_COMB_ONE)

static const DFCombFn df_comb_pow_tab[DF_STR] = { DF_NUMERIC_TYPES(DF_COMB_ENTRY) };

#undef DF_OP_NAME
#undef DF_OP_EXPR

static const DFCombFn* const df_comb_tab[DF_BIN_NCOMB] = {
    df_comb_add_tab,
    df_comb_sub_tab,
    df_comb_mul_tab,
    df_comb_div_tab,
    df_comb_pow_tab,
};

int df_bind_numeric(JSContext* ctx, const DataFrame* df, int idx,
    DFBound* b, const char* op)
{
    if (dyn_df_bind(ctx, df, idx, b))
        return -1;
    if (b->type == DF_STR) {
        JS_ThrowTypeError(ctx, "%s: cannot apply to a string column", op);
        return -1;
    }
    return 0;
}

JSValue dyn_df_map1(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    DataFrame* df;
    DFBound b;
    double* dst;
    uint32_t n, span;
    int idx, out_idx = -1;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    idx = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (idx < 0)
        return JS_EXCEPTION;
    if (argc > 1 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
        if (df_out_opt(ctx, df, argv[1], df_map1_name[magic], &out_idx) < 0)
            return JS_EXCEPTION;
    }

    n = df->nrows;
    dst = df_out_alloc(ctx, n, sizeof(double));
    if (!dst)
        return JS_EXCEPTION;
    if (df_bind_numeric(ctx, df, idx, &b, df_map1_name[magic])) {
        free(dst);
        return JS_EXCEPTION;
    }
    span = df_map_span(n, b.n, dst);
    df_map1_tab[magic][b.type](b.p, dst, span);
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

JSValue dyn_df_binary(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    DataFrame* df;
    DFBound a, b;
    JSValueConst rhs;
    double k = 0, *dst;
    uint32_t n, span;
    int ia, ib = -1, rhs_is_col, out_idx = -1;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    ia = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (ia < 0)
        return JS_EXCEPTION;
    rhs = argc > 1 ? argv[1] : JS_UNDEFINED;
    rhs_is_col = JS_IsString(rhs);
    if (argc > 1 && df_is_out_bag(ctx, rhs))
        return JS_ThrowTypeError(ctx,
            "%s: the right operand must be a number or a column name; the "
            "in-place form is %s(col, operand, {out: \"column\"})",
            df_bin_name[magic], df_bin_name[magic]);
    if (rhs_is_col) {
        if (magic >= DF_BIN_NCOMB)
            return JS_ThrowTypeError(ctx, "%s: the right operand must be a "
                                          "number; for two columns use %s with them "
                                          "swapped",
                df_bin_name[magic],
                magic == DF_BIN_RSUB ? "SUB" : "DIV");
        ib = df_col_arg(ctx, df, rhs);
        if (ib < 0)
            return JS_EXCEPTION;
    } else if (JS_ToFloat64(ctx, &k, rhs)) {
        return JS_EXCEPTION;
    }
    if (argc > 2 && !JS_IsUndefined(argv[2]) && !JS_IsNull(argv[2])) {
        if (df_out_opt(ctx, df, argv[2], df_bin_name[magic], &out_idx) < 0)
            return JS_EXCEPTION;
    }

    n = df->nrows;
    dst = df_out_alloc(ctx, n, sizeof(double));
    if (!dst)
        return JS_EXCEPTION;
    if (df_bind_numeric(ctx, df, ia, &a, df_bin_name[magic]))
        goto fail;
    if (!rhs_is_col) {
        span = df_map_span(n, a.n, dst);
        df_bin_scalar_tab[magic][a.type](a.p, dst, span, k, 0.0);
    } else {
        if (df_bind_numeric(ctx, df, ib, &b, df_bin_name[magic]))
            goto fail;
        span = df_map_span(n, a.n < b.n ? a.n : b.n, dst);
        if (a.type == DF_F64) {
            df_comb_tab[magic][b.type]((const double*)a.p, b.p, dst, span);
        } else {
            double* tmp = df_out_alloc(ctx, span, sizeof(double));
            if (!tmp)
                goto fail;
            df_map_widen_tab[a.type](a.p, tmp, span);
            df_comb_tab[magic][b.type](tmp, b.p, dst, span);
            free(tmp);
        }
    }
    if (out_idx >= 0) {
        if (df_store_out(ctx, df, out_idx, dst, n))
            goto fail;
        free(dst);
        return JS_DupValue(ctx, this_val);
    }
    return df_to_typed_array(ctx, dst, (size_t)n * sizeof(double),
        JS_TYPED_ARRAY_FLOAT64);
fail:
    free(dst);
    return JS_EXCEPTION;
}

void df_widen(const DFBound* b, double* dst, uint32_t n)
{
    dfk_widen(b->p, b->type, dst, n);
}

const char* df_type_short(DFType t)
{
    switch ((int)(t)) {
    case DF_F64:
        return "f64";
    case DF_F32:
        return "f32";
    case DF_I32:
        return "i32";
    case DF_U32:
        return "u32";
    case DF_I16:
        return "i16";
    case DF_U16:
        return "u16";
    case DF_I8:
        return "i8";
    case DF_U8:
        return "u8";
    default:
        return "str";
    }
}

JSTypedArrayEnum df_type_ta(DFType t)
{
    switch ((int)(t)) {
    case DF_F64:
        return JS_TYPED_ARRAY_FLOAT64;
    case DF_F32:
        return JS_TYPED_ARRAY_FLOAT32;
    case DF_I32:
        return JS_TYPED_ARRAY_INT32;
    case DF_U32:
        return JS_TYPED_ARRAY_UINT32;
    case DF_I16:
        return JS_TYPED_ARRAY_INT16;
    case DF_U16:
        return JS_TYPED_ARRAY_UINT16;
    case DF_I8:
        return JS_TYPED_ARRAY_INT8;
    default:
        return JS_TYPED_ARRAY_UINT8;
    }
}

void* df_col_copy(JSContext* ctx, const DataFrame* df, int idx,
    uint32_t* n_out, DFType* type_out)
{
    DFBound b;
    void* out;
    uint32_t i, n = df->nrows;

    if (dyn_df_bind(ctx, df, idx, &b))
        return NULL;
    if (b.type == DF_STR) {
        int32_t* codes = df_out_alloc(ctx, n ? n : 1, sizeof(int32_t));
        if (!codes)
            return NULL;
        for (i = 0; i < n; i++)
            codes[i] = ((const int32_t*)b.p)[i];
        *n_out = n;
        *type_out = DF_STR;
        return codes;
    }
    out = df_out_alloc(ctx, n ? n : 1, sizeof(double));
    if (!out)
        return NULL;
    for (i = 0; i < n; i++)
        ((double*)out)[i] = df_get(b.p, b.type, i);
    *n_out = n;
    *type_out = DF_F64;
    return out;
}

uint64_t df_col_bytes(const DFColumn* c)
{
    uint64_t n = (uint64_t)c->length * df_elt_size(c->type);
    uint32_t i;

    if (c->type == DF_STR) {
        n = (uint64_t)c->length * sizeof(int32_t);
        for (i = 0; i < c->dict_len; i++)
            n += (uint64_t)strlen(c->dict[i]) + 1;
        n += (uint64_t)c->dict_len * sizeof(char*);
    }
    return n;
}

JSValue dyn_df_copy(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DataFrame* df;
    DFBuilder B = { 0 };
    uint32_t *rows = NULL, i;
    JSValue res;

    (void)argc;
    (void)argv;
    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    if (dfb_init(ctx, &B, "COPY", df->ncols, df->nrows))
        return JS_EXCEPTION;
    rows = df_out_alloc(ctx, df->nrows ? df->nrows : 1, sizeof(uint32_t));
    if (!rows) {
        dfb_free(ctx, &B);
        return JS_EXCEPTION;
    }
    for (i = 0; i < df->nrows; i++)
        rows[i] = i;
    for (i = 0; i < df->ncols; i++)
        if (dfb_add_gather(ctx, &B, df->cols[i].name, df, (int)i,
                df->cols[i].type, rows, df->nrows)) {
            free(rows);
            dfb_free(ctx, &B);
            return JS_EXCEPTION;
        }
    free(rows);
    res = dfb_seal(ctx, &B);
    return res;
}

JSValue dyn_df_rename(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DataFrame* df;
    DFBuilder B = { 0 };
    uint32_t *rows = NULL, i;
    const char** newname = NULL;
    JSValue res;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "RENAME(map): expected an object mapping "
                                      "old -> new column names");
    newname = malloc((size_t)(df->ncols ? df->ncols : 1) * sizeof(char*));
    if (!newname) {
        JS_ThrowOutOfMemory(ctx);
        return JS_EXCEPTION;
    }
    for (i = 0; i < df->ncols; i++)
        newname[i] = NULL;
    {
        JSPropertyEnum* tab = NULL;
        uint32_t ntab, k;
        if (JS_GetOwnPropertyNames(ctx, &tab, &ntab, argv[0],
                JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY)
            < 0)
            goto fail;
        for (k = 0; k < ntab; k++) {
            JSValue nv = JS_AtomToValue(ctx, tab[k].atom);
            JSValue tv = JS_GetProperty(ctx, argv[0], tab[k].atom);
            size_t nlen = 0, tlen = 0;
            const char* on = JS_IsException(nv) ? NULL
                                                : JS_ToCStringLen(ctx, &nlen, nv);
            const char* nn = JS_IsException(tv) ? NULL
                                                : JS_ToCStringLen(ctx, &tlen, tv);
            int idx, j;
            JS_FreeValue(ctx, nv);
            JS_FreeValue(ctx, tv);
            if (!on || !nn) {
                if (on)
                    JS_FreeCString(ctx, on);
                if (nn)
                    JS_FreeCString(ctx, nn);
                goto fail_tab;
            }
            if (strlen(on) != nlen || strlen(nn) != tlen) {
                JS_ThrowRangeError(ctx, "RENAME: a name contains a NUL character");
                JS_FreeCString(ctx, on);
                JS_FreeCString(ctx, nn);
                goto fail_tab;
            }
            idx = df_find_col(df, on);
            if (idx < 0) {
                JS_ThrowRangeError(ctx, "RENAME: no such column: '%s'", on);
                JS_FreeCString(ctx, on);
                JS_FreeCString(ctx, nn);
                goto fail_tab;
            }
            JS_FreeCString(ctx, on);
            if (newname[idx]) {
                JS_ThrowRangeError(ctx, "RENAME: column '%s' renamed twice",
                    df->cols[idx].name);
                JS_FreeCString(ctx, nn);
                goto fail_tab;
            }
            for (j = 0; j < (int)df->ncols; j++)
                if (j != idx && !newname[j] && strcmp(df->cols[j].name, nn) == 0) {
                    JS_ThrowRangeError(ctx, "RENAME: target '%s' collides with "
                                            "the unrenamed column '%s'",
                        nn, df->cols[j].name);
                    JS_FreeCString(ctx, nn);
                    goto fail_tab;
                }
            newname[idx] = nn;
        }
        for (k = 0; k < ntab; k++)
            JS_FreeAtom(ctx, tab[k].atom);
        js_free(ctx, tab);
        goto fail_norm;
    fail_tab:
        for (k = 0; k < ntab; k++)
            JS_FreeAtom(ctx, tab[k].atom);
        js_free(ctx, tab);
        goto fail;
    }
fail_norm:
    rows = df_out_alloc(ctx, df->nrows ? df->nrows : 1, sizeof(uint32_t));
    if (!rows)
        goto fail;
    for (i = 0; i < df->nrows; i++)
        rows[i] = i;
    if (dfb_init(ctx, &B, "RENAME", df->ncols, df->nrows))
        goto fail;
    for (i = 0; i < df->ncols; i++) {
        const char* nm = newname[i] ? newname[i] : df->cols[i].name;
        if (dfb_add_gather(ctx, &B, nm, df, (int)i, df->cols[i].type, rows,
                df->nrows))
            goto fail;
    }
    for (i = 0; i < df->ncols; i++)
        if (newname[i]) {
            JS_FreeCString(ctx, newname[i]);
            newname[i] = NULL;
        }
    free(rows);
    free(newname);
    res = dfb_seal(ctx, &B);
    return res;
fail:
    for (i = 0; i < df->ncols; i++)
        if (newname[i])
            JS_FreeCString(ctx, newname[i]);
    free(rows);
    free(newname);
    dfb_free(ctx, &B);
    return JS_EXCEPTION;
}

#endif
