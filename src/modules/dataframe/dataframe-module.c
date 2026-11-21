#include "dataframe-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_DATAFRAME)

#define DF_DEFINE_MAP(name, suffix, ctype, EXPR)              \
    static void DF_KERNEL(map, name, suffix)(const void* src, \
        double* restrict dst, uint32_t n)                     \
    {                                                         \
        const ctype* x = (const ctype*)src;                   \
        uint32_t i;                                           \
        for (i = 0; i < n; i++) {                             \
            double v = (double)x[i];                          \
            dst[i] = (EXPR);                                  \
        }                                                     \
    }

#define DF_MAP_ONE(suffix, ctype, tag) DF_DEFINE_MAP(DF_OP_NAME, suffix, ctype, DF_OP_EXPR)
#define DF_MAP_ENTRY(suffix, ctype, tag) [tag] = DF_KERNEL(map, DF_OP_NAME, suffix),

static inline double df_round_js(double v)
{
    double f = floor(v);
    double r = (v - f >= 0.5) ? f + 1.0 : f;
    return (r == 0.0) ? v * 0.0 : r;
}

static inline double df_sign_js(double v)
{
    return (v > 0.0) ? 1.0 : ((v < 0.0) ? -1.0 : v);
}

enum { DF_MAP_ABS,
    DF_MAP_ROUND,
    DF_MAP_FLOOR,
    DF_MAP_CEIL,
    DF_MAP_SQRT,
    DF_MAP_LOG,
    DF_MAP_EXP,
    DF_MAP_SIGN,
    DF_MAP_N };

#define DFC_GROUPED_VERB(name)                                              \
    static JSValue name(JSContext* ctx, JSValueConst this_val, int argc,    \
        JSValueConst* argv, int magic)                                      \
    {                                                                       \
        return dfc_grouped(ctx, this_val, argc, argv, magic, name##_dense); \
    }

void df_fmt_double(char* out, size_t cap, double v)
{
    JSDTOATempMem dtoa_mem;
    int n;
    if (v != v) {
        snprintf(out, cap, "NaN");
        return;
    }
    if (isinf(v)) {
        snprintf(out, cap, v < 0 ? "-Infinity" : "Infinity");
        return;
    }
    if (js_dtoa_max_len(v, 10, 0, JS_DTOA_FORMAT_FREE) >= (int)cap) {
        snprintf(out, cap, "%.17g", v);
        return;
    }
    n = js_dtoa(out, v, 10, 0, JS_DTOA_FORMAT_FREE, &dtoa_mem);
    out[n] = 0;
}

JSClassID dyn_df_class_id;

struct df_ta_class_s df_ta_class[8];

int df_ta_class_count;

static JSClassID df_class_id_of(JSContext* ctx, JSTypedArrayEnum type)
{
    JSValueConst args[1];
    JSValue ta, zero;
    JSClassID cid = 0;

    zero = JS_NewInt32(ctx, 0);
    args[0] = zero;
    ta = JS_NewTypedArray(ctx, 1, args, type);
    JS_FreeValue(ctx, zero);
    if (!JS_IsException(ta))
        cid = JS_GetClassID(ta);
    else
        JS_FreeValue(ctx, JS_GetException(ctx));
    JS_FreeValue(ctx, ta);
    return cid;
}

static void df_init_ta_classes(JSContext* ctx)
{
    static const struct {
        JSTypedArrayEnum e;
        DFType t;
    } map[] = {
        { JS_TYPED_ARRAY_FLOAT64, DF_F64 },
        { JS_TYPED_ARRAY_FLOAT32, DF_F32 },
        { JS_TYPED_ARRAY_INT32, DF_I32 },
        { JS_TYPED_ARRAY_UINT32, DF_U32 },
        { JS_TYPED_ARRAY_INT16, DF_I16 },
        { JS_TYPED_ARRAY_UINT16, DF_U16 },
        { JS_TYPED_ARRAY_INT8, DF_I8 },
        { JS_TYPED_ARRAY_UINT8, DF_U8 },
    };
    size_t i;

    df_ta_class_count = 0;
    for (i = 0; i < countof(map); i++) {
        JSClassID cid = df_class_id_of(ctx, map[i].e);
        if (cid) {
            df_ta_class[df_ta_class_count].id = cid;
            df_ta_class[df_ta_class_count].type = map[i].t;
            df_ta_class_count++;
        }
    }
}

static void dyn_df_finalizer(JSRuntime* rt, JSValue val)
{
    DataFrame* df = JS_GetOpaque(val, dyn_df_class_id);
    uint32_t i;
    if (df) {
        for (i = 0; i < df->ncols; i++)
            JS_FreeValueRT(rt, df->cols[i].buffer);
        dyn_df_dispose(df);
    }
}

static void dyn_df_gc_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark_func)
{
    DataFrame* df = JS_GetOpaque(val, dyn_df_class_id);
    uint32_t i;
    if (df)
        for (i = 0; i < df->ncols; i++)
            JS_MarkValue(rt, df->cols[i].buffer, mark_func);
}

static const JSClassDef dyn_df_class = {
    "DataFrame",
    .finalizer = dyn_df_finalizer,
    .gc_mark = dyn_df_gc_mark,
};

size_t df_elt_size(DFType t)
{
    switch ((int)(t)) {
    case DF_F64:
        return 8;
    case DF_F32:
    case DF_I32:
    case DF_U32:
    case DF_STR:
        return 4;
    case DF_I16:
    case DF_U16:
        return 2;
    default:
        return 1;
    }
}

JSValue df_to_typed_array(JSContext* ctx, void* p, size_t nbytes,
    JSTypedArrayEnum type)
{
    JSValueConst args[3];
    JSValue ab, out;

    ab = dyn_nat_ab_take(ctx, p, nbytes);
    if (JS_IsException(ab))
        return ab;
    args[0] = ab;
    args[1] = JS_UNDEFINED;
    args[2] = JS_UNDEFINED;
    out = JS_NewTypedArray(ctx, 3, args, type);
    JS_FreeValue(ctx, ab);
    return out;
}

#define DF_OP_NAME widen
#define DF_OP_EXPR v

DF_NUMERIC_TYPES(DF_MAP_ONE)

const DFMapFn df_map_widen_tab[DF_STR] = { DF_NUMERIC_TYPES(DF_MAP_ENTRY) };

#undef DF_OP_NAME
#undef DF_OP_EXPR
#define DF_OP_NAME abs
#define DF_OP_EXPR fabs(v)

DF_NUMERIC_TYPES(DF_MAP_ONE)

static const DFMapFn df_map_abs_tab[DF_STR] = { DF_NUMERIC_TYPES(DF_MAP_ENTRY) };

#undef DF_OP_NAME
#undef DF_OP_EXPR
#define DF_OP_NAME round
#define DF_OP_EXPR df_round_js(v)

DF_NUMERIC_TYPES(DF_MAP_ONE)

static const DFMapFn df_map_round_tab[DF_STR] = { DF_NUMERIC_TYPES(DF_MAP_ENTRY) };

#undef DF_OP_NAME
#undef DF_OP_EXPR
#define DF_OP_NAME floor
#define DF_OP_EXPR floor(v)

DF_NUMERIC_TYPES(DF_MAP_ONE)

static const DFMapFn df_map_floor_tab[DF_STR] = { DF_NUMERIC_TYPES(DF_MAP_ENTRY) };

#undef DF_OP_NAME
#undef DF_OP_EXPR
#define DF_OP_NAME ceil
#define DF_OP_EXPR ceil(v)

DF_NUMERIC_TYPES(DF_MAP_ONE)

static const DFMapFn df_map_ceil_tab[DF_STR] = { DF_NUMERIC_TYPES(DF_MAP_ENTRY) };

#undef DF_OP_NAME
#undef DF_OP_EXPR
#define DF_OP_NAME sqrt
#define DF_OP_EXPR sqrt(v)

DF_NUMERIC_TYPES(DF_MAP_ONE)

static const DFMapFn df_map_sqrt_tab[DF_STR] = { DF_NUMERIC_TYPES(DF_MAP_ENTRY) };

#undef DF_OP_NAME
#undef DF_OP_EXPR
#define DF_OP_NAME log
#define DF_OP_EXPR log(v)

DF_NUMERIC_TYPES(DF_MAP_ONE)

static const DFMapFn df_map_log_tab[DF_STR] = { DF_NUMERIC_TYPES(DF_MAP_ENTRY) };

#undef DF_OP_NAME
#undef DF_OP_EXPR
#define DF_OP_NAME exp
#define DF_OP_EXPR exp(v)

DF_NUMERIC_TYPES(DF_MAP_ONE)

static const DFMapFn df_map_exp_tab[DF_STR] = { DF_NUMERIC_TYPES(DF_MAP_ENTRY) };

#undef DF_OP_NAME
#undef DF_OP_EXPR
#define DF_OP_NAME sign
#define DF_OP_EXPR df_sign_js(v)

DF_NUMERIC_TYPES(DF_MAP_ONE)

static const DFMapFn df_map_sign_tab[DF_STR] = { DF_NUMERIC_TYPES(DF_MAP_ENTRY) };

#undef DF_OP_NAME
#undef DF_OP_EXPR
#define DF_OP_NAME addk
#define DF_OP_EXPR v + k0

DF_NUMERIC_TYPES(DF_MAPK_ONE)

static const DFMapKFn df_mapk_addk_tab[DF_STR] = { DF_NUMERIC_TYPES(DF_MAPK_ENTRY) };

#undef DF_OP_NAME
#undef DF_OP_EXPR
#define DF_OP_NAME subk
#define DF_OP_EXPR v - k0

DF_NUMERIC_TYPES(DF_MAPK_ONE)

static const DFMapKFn df_mapk_subk_tab[DF_STR] = { DF_NUMERIC_TYPES(DF_MAPK_ENTRY) };

#undef DF_OP_NAME
#undef DF_OP_EXPR
#define DF_OP_NAME mulk
#define DF_OP_EXPR v* k0

DF_NUMERIC_TYPES(DF_MAPK_ONE)

static const DFMapKFn df_mapk_mulk_tab[DF_STR] = { DF_NUMERIC_TYPES(DF_MAPK_ENTRY) };

#undef DF_OP_NAME
#undef DF_OP_EXPR
#define DF_OP_NAME divk
#define DF_OP_EXPR v / k0

DF_NUMERIC_TYPES(DF_MAPK_ONE)

static const DFMapKFn df_mapk_divk_tab[DF_STR] = { DF_NUMERIC_TYPES(DF_MAPK_ENTRY) };

#undef DF_OP_NAME
#undef DF_OP_EXPR
#define DF_OP_NAME powk
#define DF_OP_EXPR pow(v, k0)

DF_NUMERIC_TYPES(DF_MAPK_ONE)

static const DFMapKFn df_mapk_powk_tab[DF_STR] = { DF_NUMERIC_TYPES(DF_MAPK_ENTRY) };

#undef DF_OP_NAME
#undef DF_OP_EXPR
#define DF_OP_NAME rsubk
#define DF_OP_EXPR k0 - v

DF_NUMERIC_TYPES(DF_MAPK_ONE)

static const DFMapKFn df_mapk_rsubk_tab[DF_STR] = { DF_NUMERIC_TYPES(DF_MAPK_ENTRY) };

#undef DF_OP_NAME
#undef DF_OP_EXPR
#define DF_OP_NAME rdivk
#define DF_OP_EXPR k0 / v

DF_NUMERIC_TYPES(DF_MAPK_ONE)

static const DFMapKFn df_mapk_rdivk_tab[DF_STR] = { DF_NUMERIC_TYPES(DF_MAPK_ENTRY) };

#undef DF_OP_NAME
#undef DF_OP_EXPR

const DFMapFn* const df_map1_tab[DF_MAP_N] = {
    df_map_abs_tab,
    df_map_round_tab,
    df_map_floor_tab,
    df_map_ceil_tab,
    df_map_sqrt_tab,
    df_map_log_tab,
    df_map_exp_tab,
    df_map_sign_tab,
};

const char* const df_map1_name[DF_MAP_N] = {
    "ABS",
    "ROUND",
    "FLOOR",
    "CEIL",
    "SQRT",
    "LOG",
    "EXP",
    "SIGN",
};

const DFMapKFn* const df_bin_scalar_tab[DF_BIN_N] = {
    df_mapk_addk_tab,
    df_mapk_subk_tab,
    df_mapk_mulk_tab,
    df_mapk_divk_tab,
    df_mapk_powk_tab,
    df_mapk_rsubk_tab,
    df_mapk_rdivk_tab,
};

const char* const df_bin_name[DF_BIN_N] = {
    "ADD",
    "SUB",
    "MUL",
    "DIV",
    "POW",
    "RSUB",
    "RDIV",
};

int df_store_out(JSContext* ctx, DataFrame* df, int out_idx,
    const double* dst, uint32_t n)
{
    DFBound b;
    double* p;

    if (out_idx >= DF_OUT_NEW) {
        DFColumn* nc;
        JSValue ab;
        if (!df->out_new || (uint32_t)(out_idx & DF_OUT_GEN_MASK) != (df->out_gen & DF_OUT_GEN_MASK)
            || df_find_col(df, df->out_new) >= 0 || n != df->nrows || df->ncols >= DF_MAX_COLS) {
            JS_ThrowRangeError(ctx, "out column '%s' changed during the operation",
                df->out_new ? df->out_new : "");
            return -1;
        }
        nc = realloc(df->cols, ((size_t)df->ncols + 1) * sizeof(*nc));
        if (!nc) {
            JS_ThrowOutOfMemory(ctx);
            return -1;
        }
        df->cols = nc;
        ab = JS_NewArrayBufferCopy(ctx, (const uint8_t*)dst, (size_t)n * sizeof(double));
        if (JS_IsException(ab))
            return -1;
        nc = &df->cols[df->ncols];
        memset(nc, 0, sizeof(*nc));
        nc->name = df->out_new;
        df->out_new = NULL;
        nc->type = DF_F64;
        nc->buffer = ab;
        nc->byte_offset = 0;
        nc->length = n;
        df->ncols++;
        return 0;
    }
    if (dyn_df_bind(ctx, df, out_idx, &b))
        return -1;
    if (b.type != DF_F64 || b.n != n) {
        JS_ThrowRangeError(ctx, "out column '%s' changed during the operation",
            df->cols[out_idx].name);
        return -1;
    }
    p = DYN_UNCONST(b.p);
    memcpy(p, dst, (size_t)n * sizeof(*p));
    return 0;
}

const char* const dfs_name[] = {
    "CUM_SUM",
    "CUM_PROD",
    "CUM_MAX",
    "CUM_MIN",
    "SHIFT",
    "DIFF",
};

atomic_uint_least64_t dfc_hash_seed = 0x9e3779b97f4a7c15ULL;

static JSValue dyn_df_get_nrows(JSContext* ctx, JSValueConst this_val)
{
    DataFrame* df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    return df ? JS_NewInt64(ctx, df->nrows) : JS_EXCEPTION;
}

static JSValue dyn_df_get_ncols(JSContext* ctx, JSValueConst this_val)
{
    DataFrame* df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    return df ? JS_NewInt64(ctx, df->ncols) : JS_EXCEPTION;
}

static int df_has_codes(JSContext* ctx, JSValueConst v)
{
    JSAtom a = JS_NewAtom(ctx, "codes");
    int r = a == JS_ATOM_NULL ? 0 : JS_HasProperty(ctx, v, a);
    JS_FreeAtom(ctx, a);
    return r > 0;
}

static int df_add_numeric(JSContext* ctx, DataFrame* df, const char* name,
    JSValueConst ta)
{
    JSValue buf;
    size_t off, len, bpe;
    DFColumn* c;
    DFType type;

    if (df_type_of_value(ta, &type) < 0) {
        JS_ThrowTypeError(ctx, "column '%s': expected Float64/Float32/Int32/"
                               "Uint32/Int16/Uint16/Int8/Uint8 Array or string[]",
            name);
        return -1;
    }
    buf = JS_GetTypedArrayBuffer(ctx, ta, &off, &len, &bpe);
    if (JS_IsException(buf))
        return -1;
    if (JS_IsSharedArrayBuffer(buf)) {
        size_t shared_size;
        uint8_t* shared_base = JS_GetArrayBuffer(ctx, &shared_size, buf);
        JSValue snapshot;
        if (!shared_base || off > shared_size || len > shared_size - off) {
            JS_FreeValue(ctx, buf);
            JS_ThrowTypeError(ctx, "column '%s': buffer is detached or out of bounds", name);
            return -1;
        }
        snapshot = JS_NewArrayBufferCopy(ctx, shared_base + off, len);
        JS_FreeValue(ctx, buf);
        if (JS_IsException(snapshot))
            return -1;
        buf = snapshot;
        off = 0;
    }
    c = &df->cols[df->ncols];
    memset(c, 0, sizeof(*c));
    c->name = strdup(name);
    if (!c->name) {
        JS_FreeValue(ctx, buf);
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    c->type = type;
    c->buffer = buf;
    c->byte_offset = (uint32_t)off;
    c->length = (uint32_t)(len / bpe);
    df->ncols++;
    return 0;
}

static JSValue dyn_df_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    DataFrame* df;
    JSPropertyEnum* tab = NULL;
    uint32_t ntab = 0, i;
    JSValue obj;

    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "DataFrame(columns): expected an object "
                                      "mapping column name -> TypedArray | string[]");
    if (JS_GetOwnPropertyNames(ctx, &tab, &ntab, argv[0],
            JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY)
        < 0)
        return JS_EXCEPTION;
    if (ntab > DF_MAX_COLS) {
        JS_FreePropertyEnum(ctx, tab, ntab);
        return JS_ThrowRangeError(ctx, "too many columns (max %d)", DF_MAX_COLS);
    }

    df = calloc(1, sizeof(*df));
    if (!df) {
        JS_FreePropertyEnum(ctx, tab, ntab);
        return JS_ThrowOutOfMemory(ctx);
    }
    df->cols = calloc(ntab ? ntab : 1, sizeof(DFColumn));
    if (!df->cols) {
        free(df);
        JS_FreePropertyEnum(ctx, tab, ntab);
        return JS_ThrowOutOfMemory(ctx);
    }

    for (i = 0; i < ntab; i++) {
        JSValue v = JS_GetProperty(ctx, argv[0], tab[i].atom);
        JSValue nv = JS_AtomToValue(ctx, tab[i].atom);
        size_t nlen = 0;
        const char* name = JS_IsException(nv) ? NULL
                                              : JS_ToCStringLen(ctx, &nlen, nv);
        int rc;
        JS_FreeValue(ctx, nv);
        if (JS_IsException(v) || !name) {
            JS_FreeValue(ctx, v);
            if (name)
                JS_FreeCString(ctx, name);
            goto fail;
        }
        if (strlen(name) != nlen) {
            JS_FreeValue(ctx, v);
            JS_FreeCString(ctx, name);
            JS_ThrowRangeError(ctx, "column name contains a NUL character");
            goto fail;
        }
        if (JS_IsArray(ctx, v)) {
            JSValue lv = JS_GetPropertyStr(ctx, v, "length");
            uint32_t n = 0;
            if (JS_IsException(lv) || JS_ToUint32(ctx, &n, lv)) {
                JS_FreeValue(ctx, lv);
                JS_FreeValue(ctx, v);
                JS_FreeCString(ctx, name);
                goto fail;
            }
            JS_FreeValue(ctx, lv);
            rc = df_add_strings(ctx, df, name, v, n);
        } else if (JS_IsObject(v) && !JS_IsFunction(ctx, v) && JS_GetBufferKind(v) == JS_BUFFER_KIND_NONE && df_has_codes(ctx, v)) {
            rc = df_add_dict(ctx, df, name, v);
        } else {
            rc = df_add_numeric(ctx, df, name, v);
        }
        JS_FreeValue(ctx, v);
        JS_FreeCString(ctx, name);
        if (rc < 0)
            goto fail;
        if (i == 0)
            df->nrows = df->cols[0].length;
        else if (df->cols[df->ncols - 1].length != df->nrows) {
            JS_ThrowRangeError(ctx, "all columns must have the same length "
                                    "(%u), got %u",
                df->nrows,
                df->cols[df->ncols - 1].length);
            goto fail;
        }
    }

    JS_FreePropertyEnum(ctx, tab, ntab);
    tab = NULL;

    obj = dyn_plain_wrap(ctx, new_target, dyn_df_class_id, df, NULL);
    if (JS_IsException(obj)) {
        for (i = 0; i < df->ncols; i++)
            JS_FreeValue(ctx, df->cols[i].buffer);
        dyn_df_dispose(df);
    }
    return obj;

fail:
    if (tab)
        JS_FreePropertyEnum(ctx, tab, ntab);
    for (i = 0; i < df->ncols; i++)
        JS_FreeValue(ctx, df->cols[i].buffer);
    dyn_df_dispose(df);
    return JS_EXCEPTION;
}

DFC_GROUPED_VERB(dyn_df_group_array)

DFC_GROUPED_VERB(dyn_df_group_array_moving)

DFC_GROUPED_VERB(dyn_df_group_agg)

DFC_GROUPED_VERB(dyn_df_group_bit)

DFC_GROUPED_VERB(dyn_df_group_array_v)

static const JSCFunctionListEntry dyn_df_proto[] = {
    JS_CGETSET_DEF("ROWS", dyn_df_get_nrows, NULL),
    JS_CGETSET_DEF("COLS", dyn_df_get_ncols, NULL),
    JS_CGETSET_DEF("COLUMNS", dyn_df_get_columns, NULL),
    JS_CFUNC_MAGIC_DEF("SUM", 2, dyn_df_reduce, DF_SUM),
    JS_CFUNC_MAGIC_DEF("MIN", 2, dyn_df_reduce, DF_MIN),
    JS_CFUNC_MAGIC_DEF("MAX", 2, dyn_df_reduce, DF_MAX),
    JS_CFUNC_MAGIC_DEF("MEAN", 2, dyn_df_reduce, DF_MEAN),
    JS_CFUNC_MAGIC_DEF("COUNT", 2, dyn_df_reduce, DF_COUNT),
    JS_CFUNC_DEF("PRODUCT", 2, dyn_df_product),
    JS_CFUNC_DEF("DOT_PRODUCT", 3, dyn_df_dot_product),
    JS_CFUNC_MAGIC_DEF("VARIANCE", 2, dyn_df_variance, DF_VAR_SAMPLE),
    JS_CFUNC_MAGIC_DEF("STDDEV", 2, dyn_df_variance, DF_VAR_STDDEV),
    JS_CFUNC_MAGIC_DEF("BITWISE_AND", 2, dyn_df_bitwise, DF_BIT_AND),
    JS_CFUNC_MAGIC_DEF("BITWISE_OR", 2, dyn_df_bitwise, DF_BIT_OR),
    JS_CFUNC_MAGIC_DEF("BITWISE_XOR", 2, dyn_df_bitwise, DF_BIT_XOR),
    JS_CFUNC_MAGIC_DEF("ALL", 1, dyn_df_all_any, DF_ALL),
    JS_CFUNC_MAGIC_DEF("ANY", 1, dyn_df_all_any, DF_ANY),
    JS_CFUNC_DEF("BITMASK", 1, dyn_df_bitmask),
    JS_CFUNC_MAGIC_DEF("GT", 2, dyn_df_compare, DF_GT),
    JS_CFUNC_MAGIC_DEF("GE", 2, dyn_df_compare, DF_GE),
    JS_CFUNC_MAGIC_DEF("LT", 2, dyn_df_compare, DF_LT),
    JS_CFUNC_MAGIC_DEF("LE", 2, dyn_df_compare, DF_LE),
    JS_CFUNC_MAGIC_DEF("EQ", 2, dyn_df_compare, DF_EQ),
    JS_CFUNC_MAGIC_DEF("NE", 2, dyn_df_compare, DF_NE),
    JS_CFUNC_MAGIC_DEF("ABS", 1, dyn_df_map1, DF_MAP_ABS),
    JS_CFUNC_MAGIC_DEF("ROUND", 1, dyn_df_map1, DF_MAP_ROUND),
    JS_CFUNC_MAGIC_DEF("FLOOR", 1, dyn_df_map1, DF_MAP_FLOOR),
    JS_CFUNC_MAGIC_DEF("CEIL", 1, dyn_df_map1, DF_MAP_CEIL),
    JS_CFUNC_MAGIC_DEF("SQRT", 1, dyn_df_map1, DF_MAP_SQRT),
    JS_CFUNC_MAGIC_DEF("LOG", 1, dyn_df_map1, DF_MAP_LOG),
    JS_CFUNC_MAGIC_DEF("EXP", 1, dyn_df_map1, DF_MAP_EXP),
    JS_CFUNC_MAGIC_DEF("SIGN", 1, dyn_df_map1, DF_MAP_SIGN),
    JS_CFUNC_MAGIC_DEF("IS_NA", 1, dyn_df_isna, 0),
    JS_CFUNC_MAGIC_DEF("NOT_NA", 1, dyn_df_isna, 1),
    JS_CFUNC_DEF("BETWEEN", 3, dyn_df_between),
    JS_CFUNC_DEF("CLIP", 3, dyn_df_clip),
    JS_CFUNC_DEF("FILL_NA", 2, dyn_df_fillna),
    JS_CFUNC_MAGIC_DEF("ADD", 2, dyn_df_binary, DF_BIN_ADD),
    JS_CFUNC_MAGIC_DEF("SUB", 2, dyn_df_binary, DF_BIN_SUB),
    JS_CFUNC_MAGIC_DEF("MUL", 2, dyn_df_binary, DF_BIN_MUL),
    JS_CFUNC_MAGIC_DEF("DIV", 2, dyn_df_binary, DF_BIN_DIV),
    JS_CFUNC_MAGIC_DEF("POW", 2, dyn_df_binary, DF_BIN_POW),
    JS_CFUNC_MAGIC_DEF("RSUB", 2, dyn_df_binary, DF_BIN_RSUB),
    JS_CFUNC_MAGIC_DEF("RDIV", 2, dyn_df_binary, DF_BIN_RDIV),
    JS_CFUNC_DEF("WHERE", 3, dyn_df_where),
    JS_CFUNC_DEF("GROUP_BY_SUM", 3, dyn_df_group_by_sum),
    JS_CFUNC_DEF("GROUP_BY_AGG", 3, dyn_df_group_by_agg),
    JS_CFUNC_MAGIC_DEF("SORT", 2, dyn_df_sort, DFO_SORT),
    JS_CFUNC_MAGIC_DEF("ARG_SORT", 2, dyn_df_sort, DFO_ARGSORT),
    JS_CFUNC_DEF("RANK", 2, dyn_df_rank),
    JS_CFUNC_MAGIC_DEF("QUANTILE", 3, dyn_df_quantile, DFO_Q_CONT),
    JS_CFUNC_MAGIC_DEF("PERCENTILE_CONT", 3, dyn_df_quantile, DFO_Q_PCONT),
    JS_CFUNC_MAGIC_DEF("PERCENTILE_DISC", 3, dyn_df_quantile, DFO_Q_DISC),
    JS_CFUNC_MAGIC_DEF("MEDIAN", 2, dyn_df_quantile, DFO_Q_MEDIAN),
    JS_CFUNC_MAGIC_DEF("N_LARGEST", 3, dyn_df_nlargest, DFO_TOP_LARGEST),
    JS_CFUNC_MAGIC_DEF("N_SMALLEST", 3, dyn_df_nlargest, DFO_TOP_SMALLEST),
    JS_CFUNC_MAGIC_DEF("CUM_SUM", 2, dyn_df_cumsum, DFS_CUMSUM),
    JS_CFUNC_MAGIC_DEF("CUM_PROD", 2, dyn_df_cumsum, DFS_CUMPROD),
    JS_CFUNC_MAGIC_DEF("CUM_MAX", 2, dyn_df_cumsum, DFS_CUMMAX),
    JS_CFUNC_MAGIC_DEF("CUM_MIN", 2, dyn_df_cumsum, DFS_CUMMIN),
    JS_CFUNC_MAGIC_DEF("SHIFT", 2, dyn_df_shift, DFS_SHIFT),
    JS_CFUNC_MAGIC_DEF("DIFF", 2, dyn_df_shift, DFS_DIFF),
    JS_CFUNC_DEF("UNIQUE", 2, dyn_df_unique),
    JS_CFUNC_DEF("N_UNIQUE", 2, dyn_df_nunique),
    JS_CFUNC_MAGIC_DEF("VALUE_COUNTS", 2, dyn_df_value_counts, DFC_VC_ALL),
    JS_CFUNC_MAGIC_DEF("TOP_K", 3, dyn_df_value_counts, DFC_VC_TOPK),
    JS_CFUNC_DEF("MODE", 2, dyn_df_mode),
    JS_CFUNC_DEF("DROP_DUPLICATES", 2, dyn_df_drop_duplicates),
    JS_CFUNC_MAGIC_DEF("GROUP_ARRAY", 3, dyn_df_group_array, DFC_GROUP_ALL),
    JS_CFUNC_MAGIC_DEF("GROUP_UNIQ_ARRAY", 3, dyn_df_group_array, DFC_GROUP_UNIQ),
    JS_CFUNC_MAGIC_DEF("GROUP_ARRAY_MOVING_SUM", 4, dyn_df_group_array_moving,
        DFC_MOVING_SUM),
    JS_CFUNC_MAGIC_DEF("GROUP_ARRAY_MOVING_AVG", 4, dyn_df_group_array_moving,
        DFC_MOVING_AVG),
    JS_CFUNC_MAGIC_DEF("HEAD", 3, dyn_df_head, DFP_HEAD),
    JS_CFUNC_MAGIC_DEF("TAIL", 3, dyn_df_head, DFP_TAIL),
    JS_CFUNC_MAGIC_DEF("FIRST", 2, dyn_df_first, DFP_FIRST),
    JS_CFUNC_MAGIC_DEF("LAST", 2, dyn_df_first, DFP_LAST),
    JS_CFUNC_MAGIC_DEF("ARG_MIN", 2, dyn_df_argmin, DFP_ARGMIN),
    JS_CFUNC_MAGIC_DEF("ARG_MAX", 2, dyn_df_argmin, DFP_ARGMAX),
    JS_CFUNC_MAGIC_DEF("VARIANCE_POP", 2, dyn_df_moments1, DFM_VARPOP),
    JS_CFUNC_MAGIC_DEF("STDDEV_POP", 2, dyn_df_moments1, DFM_STDPOP),
    JS_CFUNC_MAGIC_DEF("SKEW", 2, dyn_df_moments1, DFM_SKEW),
    JS_CFUNC_MAGIC_DEF("KURTOSIS", 2, dyn_df_moments1, DFM_KURT),
    JS_CFUNC_MAGIC_DEF("COV_POP", 3, dyn_df_moments2, DFM_COVPOP),
    JS_CFUNC_MAGIC_DEF("COV_SAMP", 3, dyn_df_moments2, DFM_COVSAMP),
    JS_CFUNC_MAGIC_DEF("CORR", 3, dyn_df_moments2, DFM_CORR),
    JS_CFUNC_MAGIC_DEF("REGR_SLOPE", 3, dyn_df_moments2, DFM_SLOPE),
    JS_CFUNC_MAGIC_DEF("REGR_INTERCEPT", 3, dyn_df_moments2, DFM_INTERCEPT),
    JS_CFUNC_MAGIC_DEF("REGR_R2", 3, dyn_df_moments2, DFM_R2),
    JS_CFUNC_MAGIC_DEF("REGR_AVG_X", 3, dyn_df_moments2, DFM_AVGX),
    JS_CFUNC_MAGIC_DEF("REGR_AVG_Y", 3, dyn_df_moments2, DFM_AVGY),
    JS_CFUNC_DEF("MEAN_WEIGHTED", 3, dyn_df_mean_weighted),
    JS_CFUNC_DEF("DESCRIBE", 2, dyn_df_describe),
    JS_CFUNC_DEF("SUM_CHECKED", 2, dyn_df_sumChecked),
    JS_CFUNC_MAGIC_DEF("BOOL_AND", 2, dyn_df_bool_reduce, DFL_BOOL_AND),
    JS_CFUNC_MAGIC_DEF("BOOL_OR", 2, dyn_df_bool_reduce, DFL_BOOL_OR),
    JS_CFUNC_MAGIC_DEF("BOOL_XOR", 2, dyn_df_bool_reduce, DFL_BOOL_XOR),
    JS_CFUNC_MAGIC_DEF("GROUP_BY_MIN", 3, dyn_df_group_agg, DF_MIN),
    JS_CFUNC_MAGIC_DEF("GROUP_BY_MAX", 3, dyn_df_group_agg, DF_MAX),
    JS_CFUNC_MAGIC_DEF("GROUP_BY_MEAN", 3, dyn_df_group_agg, DF_MEAN),
    JS_CFUNC_MAGIC_DEF("GROUP_BY_COUNT", 2, dyn_df_group_agg, DF_COUNT),
    JS_CFUNC_DEF("SUM_MAP", 3, dyn_df_group_by_sum),
    JS_CFUNC_MAGIC_DEF("MIN_MAP", 3, dyn_df_group_agg, DF_MIN),
    JS_CFUNC_MAGIC_DEF("MAX_MAP", 3, dyn_df_group_agg, DF_MAX),
    JS_CFUNC_MAGIC_DEF("ROLLING_SUM", 3, dyn_df_rolling, DF_SUM),
    JS_CFUNC_MAGIC_DEF("ROLLING_MEAN", 3, dyn_df_rolling, DF_MEAN),
    JS_CFUNC_MAGIC_DEF("ROLLING_MIN", 3, dyn_df_rolling, DF_MIN),
    JS_CFUNC_MAGIC_DEF("ROLLING_MAX", 3, dyn_df_rolling, DF_MAX),
    JS_CFUNC_DEF("DROP_NA", 0, dyn_df_dropna),
    JS_CFUNC_DEF("APPROX_COUNT_DISTINCT", 2, dyn_df_approxCountDistinct),
    JS_CFUNC_DEF("APPROX_PERCENTILE", 3, dyn_df_approxPercentile),
    JS_CFUNC_DEF("APPROX_TOP_K", 3, dyn_df_approxTopK),
    JS_CFUNC_DEF("APPROX_SIMILARITY", 3, dyn_df_approxSimilarity),
    JS_CFUNC_MAGIC_DEF("SEM", 2, dyn_df_stat1, DFX_SEM),
    JS_CFUNC_MAGIC_DEF("SKEW_SAMP", 2, dyn_df_stat1, DFX_SKEW_SAMP),
    JS_CFUNC_MAGIC_DEF("KURT_SAMP", 2, dyn_df_stat1, DFX_KURT_SAMP),
    JS_CFUNC_MAGIC_DEF("COUNT_NULLS", 2, dyn_df_stat1, DFX_COUNT_NULLS),
    JS_CFUNC_MAGIC_DEF("REGR_COUNT", 3, dyn_df_regr_sum, DFX_REGR_COUNT),
    JS_CFUNC_MAGIC_DEF("REGR_SXX", 3, dyn_df_regr_sum, DFX_REGR_SXX),
    JS_CFUNC_MAGIC_DEF("REGR_SYY", 3, dyn_df_regr_sum, DFX_REGR_SYY),
    JS_CFUNC_MAGIC_DEF("REGR_SXY", 3, dyn_df_regr_sum, DFX_REGR_SXY),
    JS_CFUNC_MAGIC_DEF("MAD", 2, dyn_df_deviation, DFX_MAD),
    JS_CFUNC_MAGIC_DEF("MEDIAN_ABSOLUTE_DEVIATION", 2, dyn_df_deviation, DFX_MED_AD),
    JS_CFUNC_DEF("ENTROPY", 2, dyn_df_entropy),
    JS_CFUNC_MAGIC_DEF("QUANTILE_EXACT_LOW", 3, dyn_df_quantile_lh, DFX_Q_LOW),
    JS_CFUNC_MAGIC_DEF("QUANTILE_EXACT_HIGH", 3, dyn_df_quantile_lh, DFX_Q_HIGH),
    JS_CFUNC_DEF("QUANTILES", 3, dyn_df_quantiles),
    JS_CFUNC_DEF("QUANTILES_TDIGEST", 3, dyn_df_quantiles_td),
    JS_CFUNC_DEF("UNIQ_UP_TO", 3, dyn_df_uniq_up_to),
    JS_CFUNC_MAGIC_DEF("HISTOGRAM", 3, dyn_df_histogram, 0),
    JS_CFUNC_MAGIC_DEF("HISTOGRAM_NORMALIZED", 3, dyn_df_histogram, 1),
    JS_CFUNC_DEF("EMA", 3, dyn_df_ema),
    JS_CFUNC_DEF("DELTA_SUM", 2, dyn_df_delta_sum),
    JS_CFUNC_DEF("DELTA_SUM_TIMESTAMP", 3, dyn_df_delta_sum_ts),
    JS_CFUNC_DEF("BOUNDING_RATIO", 3, dyn_df_bounding_ratio),
    JS_CFUNC_MAGIC_DEF("EXPONENTIAL_TIME_DECAYED_AVG", 4, dyn_df_etd_avg, DFE_AVG),
    JS_CFUNC_MAGIC_DEF("EXPONENTIAL_TIME_DECAYED_SUM", 4, dyn_df_etd_avg, DFE_SUM),
    JS_CFUNC_MAGIC_DEF("EXPONENTIAL_TIME_DECAYED_COUNT", 4, dyn_df_etd_avg, DFE_COUNT),
    JS_CFUNC_MAGIC_DEF("EXPONENTIAL_TIME_DECAYED_MAX", 4, dyn_df_etd_avg, DFE_MAX),
    JS_CFUNC_DEF("GROUP_ARRAY_INSERT_AT", 5, dyn_df_group_insert_at),
    JS_CFUNC_DEF("GROUP_BITMAP", 2, dyn_df_group_bitmap),
    JS_CFUNC_DEF("QUANTILE_TDIGEST_WEIGHTED", 4, dyn_df_quantile_td_weighted),
    JS_CFUNC_MAGIC_DEF("RANGE_AGG", 3, dyn_df_range_agg, DFR_UNION),
    JS_CFUNC_MAGIC_DEF("RANGE_INTERSECT_AGG", 3, dyn_df_range_agg, DFR_INTERSECT),
    JS_CFUNC_MAGIC_DEF("RATE", 3, dyn_df_rate, DFX_RATE),
    JS_CFUNC_MAGIC_DEF("IRATE", 3, dyn_df_rate, DFX_IRATE),
    JS_CFUNC_MAGIC_DEF("GROUP_BIT_AND", 3, dyn_df_group_bit, DFY_BIT_AND),
    JS_CFUNC_MAGIC_DEF("GROUP_BIT_OR", 3, dyn_df_group_bit, DFY_BIT_OR),
    JS_CFUNC_MAGIC_DEF("GROUP_BIT_XOR", 3, dyn_df_group_bit, DFY_BIT_XOR),
    JS_CFUNC_MAGIC_DEF("CORR_MATRIX", 2, dyn_df_corr_matrix, 0),
    JS_CFUNC_MAGIC_DEF("COV_MATRIX", 2, dyn_df_corr_matrix, 1),
    JS_CFUNC_MAGIC_DEF("ROLLING_VAR", 3, dyn_df_rolling_disp, DFX_ROLL_VAR),
    JS_CFUNC_MAGIC_DEF("ROLLING_STD", 3, dyn_df_rolling_disp, DFX_ROLL_STD),
    JS_CFUNC_DEF("PCT_CHANGE", 3, dyn_df_pct_change),
    JS_CFUNC_DEF("ZSCORE", 2, dyn_df_zscore),
    JS_CFUNC_MAGIC_DEF("DENSE_RANK", 2, dyn_df_rank_ext, DFX_DENSE_RANK),
    JS_CFUNC_MAGIC_DEF("PERCENT_RANK", 2, dyn_df_rank_ext, DFX_PERCENT_RANK),
    JS_CFUNC_DEF("NTILE", 3, dyn_df_ntile),
    JS_CFUNC_DEF("RANK_CORR", 3, dyn_df_rank_corr),
    JS_CFUNC_DEF("GROUP_CONCAT", 3, dyn_df_group_concat),
    JS_CFUNC_MAGIC_DEF("GROUP_ARRAY_SORTED", 3, dyn_df_group_array_v, DFZ_SORTED),
    JS_CFUNC_MAGIC_DEF("GROUP_ARRAY_LAST", 4, dyn_df_group_array_v, DFZ_LAST),
    JS_CFUNC_MAGIC_DEF("GROUP_ARRAY_SAMPLE", 4, dyn_df_group_array_v, DFZ_SAMPLE),
    JS_CFUNC_MAGIC_DEF("TOP_K_WEIGHTED", 4, dyn_df_weighted_top, DFW_TOPK_W),
    JS_CFUNC_MAGIC_DEF("APPROX_TOP_SUM", 4, dyn_df_weighted_top, DFW_APPROX_TOP_SUM),
    JS_CFUNC_MAGIC_DEF("ANY_HEAVY", 3, dyn_df_weighted_top, DFW_ANY_HEAVY),
    JS_CFUNC_DEF("QUANTILE_EXACT_WEIGHTED", 4, dyn_df_quantile_weighted),
    JS_CFUNC_DEF("GROUP_ARRAY_INTERSECT", 3, dyn_df_group_intersect),
    JS_CFUNC_DEF("DTYPES", 0, dyn_df_dtypes),
    JS_CFUNC_DEF("SCHEMA", 0, dyn_df_schema),
    JS_CFUNC_DEF("INFO", 0, dyn_df_info),
    JS_CFUNC_DEF("MEMORY_USAGE", 0, dyn_df_memory_usage),
    JS_CFUNC_DEF("TO_COLUMNS", 0, dyn_df_to_columns),
    JS_CFUNC_DEF("TO_RECORDS", 0, dyn_df_to_records),
    JS_CFUNC_DEF("TO_JSON", 0, dyn_df_to_json),
    JS_CFUNC_DEF("TO_CSV", 0, dyn_df_to_csv),
    JS_CFUNC_DEF("FROM_RECORDS", 1, dyn_df_from_records),
    JS_CFUNC_DEF("COPY", 0, dyn_df_copy),
    JS_CFUNC_DEF("SELECT", 1, dyn_df_select),
    JS_CFUNC_DEF("DROP_COLUMNS", 1, dyn_df_drop_columns),
    JS_CFUNC_DEF("RENAME", 1, dyn_df_rename),
    JS_CFUNC_DEF("FILTER", 1, dyn_df_filter),
    JS_CFUNC_DEF("SLICE", 2, dyn_df_slice),
    JS_CFUNC_DEF("SAMPLE", 2, dyn_df_sample),
    JS_CFUNC_DEF("ISIN", 2, dyn_df_isin),
    JS_CFUNC_DEF("MASK", 2, dyn_df_mask),
    JS_CFUNC_DEF("JOIN", 4, dyn_df_join),
    JS_CFUNC_DEF("ASOF_JOIN", 3, dyn_df_asof_join),
    JS_CFUNC_DEF("CONCAT", 1, dyn_df_concat),
    JS_CFUNC_DEF("RESAMPLE", 3, dyn_df_resample),
    JS_CFUNC_DEF("PIVOT", 4, dyn_df_pivot),
    JS_CFUNC_DEF("MELT", 2, dyn_df_melt),
    JS_CFUNC_MAGIC_DEF("JSON_AGG", 3, dyn_df_json_agg, DFJA_AGG),
    JS_CFUNC_MAGIC_DEF("JSON_OBJECT_AGG", 3, dyn_df_json_agg, DFJA_OBJ),
    JS_CFUNC_MAGIC_DEF("JSON_AGG_STRICT", 3, dyn_df_json_agg, DFJA_AGG_STRICT),
    JS_CFUNC_MAGIC_DEF("JSON_OBJECT_AGG_STRICT", 3, dyn_df_json_agg, DFJA_OBJ_STRICT),
};

static JSValue dyn_df_is_dataframe(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    (void)this_val;
    return JS_NewBool(ctx, argc > 0 && JS_GetOpaque(argv[0], dyn_df_class_id) != NULL);
}

int dyn_df_ensure_class(JSContext* ctx)
{
    JSRuntime* rt = JS_GetRuntime(ctx);
    JSValue proto, ctor;

    JS_NewClassID(&dyn_df_class_id);
    if (!JS_IsRegisteredClass(rt, dyn_df_class_id) && JS_NewClass(rt, dyn_df_class_id, &dyn_df_class) < 0)
        return -1;
    proto = JS_GetClassProto(ctx, dyn_df_class_id);
    if (JS_IsObject(proto)) {
        JS_FreeValue(ctx, proto);
        return 0;
    }
    JS_FreeValue(ctx, proto);
    df_init_ta_classes(ctx);
    proto = JS_NewObject(ctx);
    if (JS_IsException(proto))
        return -1;
    JS_SetPropertyFunctionList(ctx, proto, dyn_df_proto, countof(dyn_df_proto));
    JS_SetClassProto(ctx, dyn_df_class_id, proto);
    ctor = JS_NewCFunction2(ctx, dyn_df_ctor, "DataFrame", 0, JS_CFUNC_constructor, 0);
    if (JS_IsException(ctor))
        return -1;
    JS_SetConstructor(ctx, ctor, proto);
    JS_FreeValue(ctx, ctor);
    return 0;
}

static int dyn_df_init_module(JSContext* ctx, JSModuleDef* m)
{
    JSValue isdf, proto, ctor;

    dfc_hash_seed_once();
    if (dyn_df_ensure_class(ctx) < 0)
        return -1;
    proto = JS_GetClassProto(ctx, dyn_df_class_id);
    ctor = JS_GetPropertyStr(ctx, proto, "constructor");
    JS_FreeValue(ctx, proto);
    if (!JS_IsFunction(ctx, ctor)) {
        JS_FreeValue(ctx, ctor);
        return -1;
    }
    if (JS_SetModuleExport(ctx, m, "DataFrame", ctor) < 0)
        return -1;
    isdf = JS_NewCFunction(ctx, dyn_df_is_dataframe, "isDataFrame", 1);
    if (JS_IsException(isdf))
        return -1;
    if (JS_SetModuleExport(ctx, m, "isDataFrame", isdf) < 0)
        return -1;
    return 0;
}

int js_nat_init_dataframe(JSContext* ctx)
{
    JSModuleDef* m = JS_NewCModule(ctx, "dyna:dataframe", dyn_df_init_module);
    if (!m)
        return -1;
    JS_AddModuleExport(ctx, m, "DataFrame");
    JS_AddModuleExport(ctx, m, "isDataFrame");
    return 0;
}

#endif
