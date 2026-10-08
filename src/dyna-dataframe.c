#include "dyna-nat.h"
#include "cutils.h"
#include "dtoa.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_DATAFRAME)

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <float.h>
#include <math.h>
#include <time.h>
#include <locale.h>
#ifdef _WIN32
#include <process.h>
#include <intrin.h>
#define df_getpid _getpid
#else
#include <unistd.h>
#define df_getpid getpid
#endif
#include <stdatomic.h>
#include "core/dyn-prng.h"

#define countof(x) (sizeof(x) / sizeof((x)[0]))

static int df_clz64(uint64_t v)
{
#if defined(_MSC_VER)
    unsigned long i;
    _BitScanReverse64(&i, v);
    return 63 - (int)i;
#else
    return __builtin_clzll(v);
#endif
}

static void df_fmt_double(char* out, size_t cap, double v)
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

#define DF_MAX_COLS 1024
#define DF_MAX_GROUPS (1 << 20)

typedef enum {
    DF_F64,
    DF_F32,
    DF_I32,
    DF_U32,
    DF_I16,
    DF_U16,
    DF_I8,
    DF_U8,
    DF_STR
} DFType;

typedef struct {
    char* name;
    DFType type;
    JSValue buffer;
    uint32_t byte_offset;
    uint32_t length;
    int32_t* codes;
    char** dict;
    uint32_t dict_len;
    uint32_t dict_cap;
} DFColumn;

typedef struct {
    DFColumn* cols;
    uint32_t ncols;
    uint32_t nrows;
} DataFrame;

typedef struct {
    const void* p;
    DFType type;
    uint32_t n;
} DFBound;

static JSClassID dyn_df_class_id;

static struct {
    JSClassID id;
    DFType type;
} df_ta_class[8];
static int df_ta_class_count;

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

static int df_type_of_value(JSValueConst v, DFType* out)
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

static void df_col_free(DFColumn* c)
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

static void dyn_df_dispose(void* native)
{
    DataFrame* df = native;
    uint32_t i;
    if (!df)
        return;
    for (i = 0; i < df->ncols; i++)
        df_col_free(&df->cols[i]);
    free(df->cols);
    free(df);
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

static size_t df_elt_size(DFType t)
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

static const char* df_type_name(DFType t)
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

static JSValue df_to_typed_array(JSContext* ctx, void* p, size_t nbytes,
    JSTypedArrayEnum type);
static double df_get(const void* p, DFType t, uint32_t i)
{
    switch ((int)(t)) {
    case DF_F64:
        return ((const double*)p)[i];
    case DF_F32:
        return (double)((const float*)p)[i];
    case DF_I32:
    case DF_STR:
        return ((const int32_t*)p)[i];
    case DF_U32:
        return ((const uint32_t*)p)[i];
    case DF_I16:
        return ((const int16_t*)p)[i];
    case DF_U16:
        return ((const uint16_t*)p)[i];
    case DF_I8:
        return ((const int8_t*)p)[i];
    default:
        return ((const uint8_t*)p)[i];
    }
}

static int df_find_col(const DataFrame* df, const char* name)
{
    uint32_t i;
    for (i = 0; i < df->ncols; i++)
        if (strcmp(df->cols[i].name, name) == 0)
            return (int)i;
    return -1;
}

static int dyn_df_bind(JSContext* ctx, const DataFrame* df, int idx, DFBound* b)
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

static int df_col_arg(JSContext* ctx, const DataFrame* df, JSValueConst v)
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

static const uint8_t* df_mask_arg(JSContext* ctx, JSValueConst v,
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

enum { DF_SUM,
    DF_MIN,
    DF_MAX,
    DF_MEAN,
    DF_COUNT };

#define DF_FLOAT_TYPES(X)  \
    X(f64, double, DF_F64) \
    X(f32, float, DF_F32)
#define DF_INT_TYPES(X)      \
    X(i32, int32_t, DF_I32)  \
    X(u32, uint32_t, DF_U32) \
    X(i16, int16_t, DF_I16)  \
    X(u16, uint16_t, DF_U16) \
    X(i8, int8_t, DF_I8)     \
    X(u8, uint8_t, DF_U8)
#define DF_NUMERIC_TYPES(X) DF_FLOAT_TYPES(X) DF_INT_TYPES(X)

#define DF_COUNT_ONE(sfx, cty, tag) +1
_Static_assert((0 DF_NUMERIC_TYPES(DF_COUNT_ONE)) == DF_STR,
    "DF_NUMERIC_TYPES must list every numeric DFType exactly once");
#undef DF_COUNT_ONE

#define DF_STEP_SUM(acc, v) ((acc) += (v))
#define DF_STEP_MIN(acc, v) ((acc) = (v) < (acc) ? (v) : (acc))
#define DF_STEP_MAX(acc, v) ((acc) = (v) > (acc) ? (v) : (acc))
#define DF_COMBINE_SUM(p, q) ((p) + (q))
#define DF_COMBINE_MIN(p, q) ((p) < (q) ? (p) : (q))
#define DF_COMBINE_MAX(p, q) ((p) > (q) ? (p) : (q))

#if defined(__aarch64__) || defined(__arm64__) || defined(_M_ARM64)
#if defined(_MSC_VER)
#define DF_STEP_FMIN_f64(acc, v) ((acc) = fmin((v), (acc)))
#define DF_STEP_FMAX_f64(acc, v) ((acc) = fmax((v), (acc)))
#define DF_STEP_FMIN_f32(acc, v) ((acc) = (float)fmin((double)(v), (double)(acc)))
#define DF_STEP_FMAX_f32(acc, v) ((acc) = (float)fmax((double)(v), (double)(acc)))
#else
#define DF_STEP_FMIN_f64(acc, v) ((acc) = __builtin_fmin((v), (acc)))
#define DF_STEP_FMAX_f64(acc, v) ((acc) = __builtin_fmax((v), (acc)))
#define DF_STEP_FMIN_f32(acc, v) ((acc) = __builtin_fminf((double)(v), (double)(acc)))
#define DF_STEP_FMAX_f32(acc, v) ((acc) = __builtin_fmaxf((double)(v), (double)(acc)))
#endif
#else
#define DF_STEP_FMIN_f64(acc, v) DF_STEP_MIN(acc, v)
#define DF_STEP_FMAX_f64(acc, v) DF_STEP_MAX(acc, v)
#define DF_STEP_FMIN_f32(acc, v) DF_STEP_MIN(acc, v)
#define DF_STEP_FMAX_f32(acc, v) DF_STEP_MAX(acc, v)
#endif

#define DF_ACC__(kind, sfx) DF_ACC_##kind##_##sfx
#define DF_ACC(kind, sfx) DF_ACC__(kind, sfx)
#define DF_ID__(kind, sfx) DF_ID_##kind##_##sfx
#define DF_ID(kind, sfx) DF_ID__(kind, sfx)

#define DF_ACC_SUM_f64 double
#define DF_ACC_SUM_f32 double
#define DF_ACC_SUM_i32 int64_t
#define DF_ACC_SUM_u32 uint64_t
#define DF_ACC_SUM_i16 int64_t
#define DF_ACC_SUM_u16 uint64_t
#define DF_ACC_SUM_i8 int64_t
#define DF_ACC_SUM_u8 uint64_t
#define DF_ID_SUM_f64 0.0
#define DF_ID_SUM_f32 0.0
#define DF_ID_SUM_i32 0
#define DF_ID_SUM_u32 0
#define DF_ID_SUM_i16 0
#define DF_ID_SUM_u16 0
#define DF_ID_SUM_i8 0
#define DF_ID_SUM_u8 0

#define DF_ACC_MIN_f64 double
#define DF_ACC_MIN_f32 float
#define DF_ACC_MIN_i32 int32_t
#define DF_ACC_MIN_u32 uint32_t
#define DF_ACC_MIN_i16 int16_t
#define DF_ACC_MIN_u16 uint16_t
#define DF_ACC_MIN_i8 int8_t
#define DF_ACC_MIN_u8 uint8_t
#define DF_ID_MIN_f64 DYN_INFINITY
#define DF_ID_MIN_f32 INFINITY
#define DF_ID_MIN_i32 INT32_MAX
#define DF_ID_MIN_u32 UINT32_MAX
#define DF_ID_MIN_i16 INT16_MAX
#define DF_ID_MIN_u16 UINT16_MAX
#define DF_ID_MIN_i8 INT8_MAX
#define DF_ID_MIN_u8 UINT8_MAX

#define DF_ACC_MAX_f64 double
#define DF_ACC_MAX_f32 float
#define DF_ACC_MAX_i32 int32_t
#define DF_ACC_MAX_u32 uint32_t
#define DF_ACC_MAX_i16 int16_t
#define DF_ACC_MAX_u16 uint16_t
#define DF_ACC_MAX_i8 int8_t
#define DF_ACC_MAX_u8 uint8_t
#define DF_ID_MAX_f64 (-DYN_INFINITY)
#define DF_ID_MAX_f32 (-INFINITY)
#define DF_ID_MAX_i32 INT32_MIN
#define DF_ID_MAX_u32 0
#define DF_ID_MAX_i16 INT16_MIN
#define DF_ID_MAX_u16 0
#define DF_ID_MAX_i8 INT8_MIN
#define DF_ID_MAX_u8 0

#define DF_ACCS_FLOAT 16
#define DF_ACCS_INT 1

#define DF_NACC_SUM_f64 DF_ACCS_FLOAT
#define DF_NACC_SUM_f32 8

#define DF_NACC_MASKED_FLOAT 8

_Static_assert((DF_ACCS_FLOAT & (DF_ACCS_FLOAT - 1)) == 0 && (DF_ACCS_INT & (DF_ACCS_INT - 1)) == 0 && (DF_NACC_SUM_f32 & (DF_NACC_SUM_f32 - 1)) == 0 && (DF_NACC_MASKED_FLOAT & (DF_NACC_MASKED_FLOAT - 1)) == 0,
    "accumulator counts must be powers of two: the body bound "
    "n & ~(NACC-1) over-reads the column otherwise");

#define DF_PASTE_(a, b) a##b
#define DF_PASTE(a, b) DF_PASTE_(a, b)

#define DF_DECL_1(T, I) T a0 = I;
#define DF_DECL_8(T, I) T a0 = I, a1 = I, a2 = I, a3 = I, \
                          a4 = I, a5 = I, a6 = I, a7 = I;
#define DF_DECL_16(T, I) T a0 = I, a1 = I, a2 = I, a3 = I,   \
                           a4 = I, a5 = I, a6 = I, a7 = I,   \
                           a8 = I, a9 = I, a10 = I, a11 = I, \
                           a12 = I, a13 = I, a14 = I, a15 = I;
#define DF_MERGE_1(C) (a0)
#define DF_MERGE_8(C) C(C(C(a0, a1), C(a2, a3)), C(C(a4, a5), C(a6, a7)))
#define DF_MERGE_16(C) C(C(C(C(a0, a1), C(a2, a3)),    \
                             C(C(a4, a5), C(a6, a7))), \
    C(C(C(a8, a9), C(a10, a11)),                       \
        C(C(a12, a13), C(a14, a15))))

#define DF_FOLD_1(T, S) \
    {                   \
        T v0 = (T)x[i]; \
        S(a0, v0);      \
    }
#define DF_FOLD_8(T, S)                                         \
    {                                                           \
        T v0 = (T)x[i], v1 = (T)x[i + 1], v2 = (T)x[i + 2],     \
          v3 = (T)x[i + 3], v4 = (T)x[i + 4], v5 = (T)x[i + 5], \
          v6 = (T)x[i + 6], v7 = (T)x[i + 7];                   \
        S(a0, v0);                                              \
        S(a1, v1);                                              \
        S(a2, v2);                                              \
        S(a3, v3);                                              \
        S(a4, v4);                                              \
        S(a5, v5);                                              \
        S(a6, v6);                                              \
        S(a7, v7);                                              \
    }
#define DF_FOLD_16(T, S)                                              \
    {                                                                 \
        T v0 = (T)x[i], v1 = (T)x[i + 1], v2 = (T)x[i + 2],           \
          v3 = (T)x[i + 3], v4 = (T)x[i + 4], v5 = (T)x[i + 5],       \
          v6 = (T)x[i + 6], v7 = (T)x[i + 7], v8 = (T)x[i + 8],       \
          v9 = (T)x[i + 9], v10 = (T)x[i + 10], v11 = (T)x[i + 11],   \
          v12 = (T)x[i + 12], v13 = (T)x[i + 13], v14 = (T)x[i + 14], \
          v15 = (T)x[i + 15];                                         \
        S(a0, v0);                                                    \
        S(a1, v1);                                                    \
        S(a2, v2);                                                    \
        S(a3, v3);                                                    \
        S(a4, v4);                                                    \
        S(a5, v5);                                                    \
        S(a6, v6);                                                    \
        S(a7, v7);                                                    \
        S(a8, v8);                                                    \
        S(a9, v9);                                                    \
        S(a10, v10);                                                  \
        S(a11, v11);                                                  \
        S(a12, v12);                                                  \
        S(a13, v13);                                                  \
        S(a14, v14);                                                  \
        S(a15, v15);                                                  \
    }

#define DF_CLEAN_1(T, S)
#define DF_CLEAN_8(T, S)
#define DF_CLEAN_16(T, S) \
    if (n - i >= 8) {     \
        DF_FOLD_8(T, S)   \
        i += 8;           \
    }
#define DF_MCLEAN_1(T, S, I)
#define DF_MCLEAN_8(T, S, I)
#define DF_MCLEAN_16(T, S, I) \
    if (n - i >= 8) {         \
        DF_MFOLD_8(T, S, I)   \
        i += 8;               \
    }

#define DF_MFOLD_1(T, S, I)      \
    {                            \
        T v0 = (T)x[i];          \
        v0 = m[i] ? v0 : (T)(I); \
        S(a0, v0);               \
        k += (m[i] != 0);        \
    }
#define DF_MFOLD_8(T, S, I)                                       \
    {                                                             \
        T v0 = (T)x[i], v1 = (T)x[i + 1], v2 = (T)x[i + 2],       \
          v3 = (T)x[i + 3], v4 = (T)x[i + 4], v5 = (T)x[i + 5],   \
          v6 = (T)x[i + 6], v7 = (T)x[i + 7];                     \
        v0 = m[i] ? v0 : (T)(I);                                  \
        v1 = m[i + 1] ? v1 : (T)(I);                              \
        v2 = m[i + 2] ? v2 : (T)(I);                              \
        v3 = m[i + 3] ? v3 : (T)(I);                              \
        v4 = m[i + 4] ? v4 : (T)(I);                              \
        v5 = m[i + 5] ? v5 : (T)(I);                              \
        v6 = m[i + 6] ? v6 : (T)(I);                              \
        v7 = m[i + 7] ? v7 : (T)(I);                              \
        S(a0, v0);                                                \
        S(a1, v1);                                                \
        S(a2, v2);                                                \
        S(a3, v3);                                                \
        S(a4, v4);                                                \
        S(a5, v5);                                                \
        S(a6, v6);                                                \
        S(a7, v7);                                                \
        k += (m[i] != 0) + (m[i + 1] != 0) + (m[i + 2] != 0)      \
            + (m[i + 3] != 0) + (m[i + 4] != 0) + (m[i + 5] != 0) \
            + (m[i + 6] != 0) + (m[i + 7] != 0);                  \
    }
#define DF_MFOLD_16(T, S, I)                                          \
    {                                                                 \
        T v0 = (T)x[i], v1 = (T)x[i + 1], v2 = (T)x[i + 2],           \
          v3 = (T)x[i + 3], v4 = (T)x[i + 4], v5 = (T)x[i + 5],       \
          v6 = (T)x[i + 6], v7 = (T)x[i + 7], v8 = (T)x[i + 8],       \
          v9 = (T)x[i + 9], v10 = (T)x[i + 10], v11 = (T)x[i + 11],   \
          v12 = (T)x[i + 12], v13 = (T)x[i + 13], v14 = (T)x[i + 14], \
          v15 = (T)x[i + 15];                                         \
        v0 = m[i] ? v0 : (T)(I);                                      \
        v1 = m[i + 1] ? v1 : (T)(I);                                  \
        v2 = m[i + 2] ? v2 : (T)(I);                                  \
        v3 = m[i + 3] ? v3 : (T)(I);                                  \
        v4 = m[i + 4] ? v4 : (T)(I);                                  \
        v5 = m[i + 5] ? v5 : (T)(I);                                  \
        v6 = m[i + 6] ? v6 : (T)(I);                                  \
        v7 = m[i + 7] ? v7 : (T)(I);                                  \
        v8 = m[i + 8] ? v8 : (T)(I);                                  \
        v9 = m[i + 9] ? v9 : (T)(I);                                  \
        v10 = m[i + 10] ? v10 : (T)(I);                               \
        v11 = m[i + 11] ? v11 : (T)(I);                               \
        v12 = m[i + 12] ? v12 : (T)(I);                               \
        v13 = m[i + 13] ? v13 : (T)(I);                               \
        v14 = m[i + 14] ? v14 : (T)(I);                               \
        v15 = m[i + 15] ? v15 : (T)(I);                               \
        S(a0, v0);                                                    \
        S(a1, v1);                                                    \
        S(a2, v2);                                                    \
        S(a3, v3);                                                    \
        S(a4, v4);                                                    \
        S(a5, v5);                                                    \
        S(a6, v6);                                                    \
        S(a7, v7);                                                    \
        S(a8, v8);                                                    \
        S(a9, v9);                                                    \
        S(a10, v10);                                                  \
        S(a11, v11);                                                  \
        S(a12, v12);                                                  \
        S(a13, v13);                                                  \
        S(a14, v14);                                                  \
        S(a15, v15);                                                  \
        k += (m[i] != 0) + (m[i + 1] != 0) + (m[i + 2] != 0)          \
            + (m[i + 3] != 0) + (m[i + 4] != 0) + (m[i + 5] != 0)     \
            + (m[i + 6] != 0) + (m[i + 7] != 0) + (m[i + 8] != 0)     \
            + (m[i + 9] != 0) + (m[i + 10] != 0) + (m[i + 11] != 0)   \
            + (m[i + 12] != 0) + (m[i + 13] != 0) + (m[i + 14] != 0)  \
            + (m[i + 15] != 0);                                       \
    }

#define DF_DEFINE_REDUCE_KM(NACC, MNACC, method, suffix, ctype, acctype, INIT,      \
    STEP, COMBINE)                                                                  \
    static acctype df_##method##_##suffix(const ctype* restrict x, uint32_t n)      \
    {                                                                               \
        DF_PASTE(DF_DECL_, NACC)(acctype, INIT)                                     \
            uint32_t i,                                                             \
            lim = n & ~(uint32_t)(NACC - 1);                                        \
        for (i = 0; i < lim; i += NACC)                                             \
            DF_PASTE(DF_FOLD_, NACC)(acctype, STEP)                                 \
                DF_PASTE(DF_CLEAN_, NACC)(acctype, STEP) for (; i < n; i++)         \
            {                                                                       \
                acctype v = (acctype)x[i];                                          \
                STEP(a0, v);                                                        \
            }                                                                       \
        return DF_PASTE(DF_MERGE_, NACC)(COMBINE);                                  \
    }                                                                               \
                                                                                    \
    static acctype df_##method##_masked_##suffix(const ctype* restrict x,           \
        const uint8_t* restrict m,                                                  \
        uint32_t n, uint32_t* pcount)                                               \
    {                                                                               \
        DF_PASTE(DF_DECL_, MNACC)(acctype, INIT)                                    \
            uint32_t i,                                                             \
            k = 0, lim = n & ~(uint32_t)(MNACC - 1);                                \
        for (i = 0; i < lim; i += MNACC)                                            \
            DF_PASTE(DF_MFOLD_, MNACC)(acctype, STEP, INIT)                         \
                DF_PASTE(DF_MCLEAN_, MNACC)(acctype, STEP, INIT) for (; i < n; i++) \
            {                                                                       \
                acctype v = (acctype)x[i];                                          \
                v = m[i] ? v : (acctype)(INIT);                                     \
                STEP(a0, v);                                                        \
                k += (m[i] != 0);                                                   \
            }                                                                       \
        *pcount = k;                                                                \
        return DF_PASTE(DF_MERGE_, MNACC)(COMBINE);                                 \
    }

#define DF_DEFINE_REDUCE_K(NACC, method, suffix, ctype, acctype, INIT, STEP, \
    COMBINE)                                                                 \
    DF_DEFINE_REDUCE_KM(NACC, NACC, method, suffix, ctype, acctype, INIT,    \
        STEP, COMBINE)

#define DF_DEFINE_REDUCE(method, suffix, ctype, acctype, INIT, STEP, COMBINE) \
    DF_DEFINE_REDUCE_K(DF_ACCS_FLOAT, method, suffix, ctype, acctype, INIT,   \
        STEP, COMBINE)

#define DF_DEF_SUM_F(sfx, cty, tag)                                        \
    DF_DEFINE_REDUCE_KM(DF_PASTE(DF_NACC_SUM_, sfx), DF_NACC_MASKED_FLOAT, \
        sum, sfx, cty,                                                     \
        DF_ACC(SUM, sfx), DF_ID(SUM, sfx), DF_STEP_SUM,                    \
        DF_COMBINE_SUM)
#define DF_DEF_SUM_I(sfx, cty, tag)                                  \
    DF_DEFINE_REDUCE_K(DF_ACCS_INT, sum, sfx, cty, DF_ACC(SUM, sfx), \
        DF_ID(SUM, sfx), DF_STEP_SUM, DF_COMBINE_SUM)
DF_FLOAT_TYPES(DF_DEF_SUM_F)
DF_INT_TYPES(DF_DEF_SUM_I)
#undef DF_DEF_SUM_F
#undef DF_DEF_SUM_I

#define DF_DEF_MIN_F(sfx, cty, tag)                    \
    DF_DEFINE_REDUCE(min, sfx, cty, DF_ACC(MIN, sfx),  \
        DF_ID(MIN, sfx), DF_PASTE(DF_STEP_FMIN_, sfx), \
        DF_COMBINE_MIN)
#define DF_DEF_MIN_I(sfx, cty, tag)                                  \
    DF_DEFINE_REDUCE_K(DF_ACCS_INT, min, sfx, cty, DF_ACC(MIN, sfx), \
        DF_ID(MIN, sfx), DF_STEP_MIN, DF_COMBINE_MIN)
DF_FLOAT_TYPES(DF_DEF_MIN_F)
DF_INT_TYPES(DF_DEF_MIN_I)
#undef DF_DEF_MIN_F
#undef DF_DEF_MIN_I

#define DF_DEF_MAX_F(sfx, cty, tag)                    \
    DF_DEFINE_REDUCE(max, sfx, cty, DF_ACC(MAX, sfx),  \
        DF_ID(MAX, sfx), DF_PASTE(DF_STEP_FMAX_, sfx), \
        DF_COMBINE_MAX)
#define DF_DEF_MAX_I(sfx, cty, tag)                                  \
    DF_DEFINE_REDUCE_K(DF_ACCS_INT, max, sfx, cty, DF_ACC(MAX, sfx), \
        DF_ID(MAX, sfx), DF_STEP_MAX, DF_COMBINE_MAX)
DF_FLOAT_TYPES(DF_DEF_MAX_F)
DF_INT_TYPES(DF_DEF_MAX_I)
#undef DF_DEF_MAX_F
#undef DF_DEF_MAX_I

#define DF_REDUCE_CASE(method, sfx, cty, tag)                         \
    case tag:                                                         \
        if (mask)                                                     \
            acc = (double)df_##method##_masked_##sfx((const cty*)b.p, \
                mask, b.n, &count);                                   \
        else {                                                        \
            acc = (double)df_##method##_##sfx((const cty*)b.p, b.n);  \
            count = b.n;                                              \
        }                                                             \
        break;

static JSValue dyn_df_reduce(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    DataFrame* df;
    DFBound b;
    const uint8_t* mask;
    int idx, ok;
    uint32_t i, count = 0;
    double acc = 0;

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
    if (b.type == DF_STR)
        return JS_ThrowTypeError(ctx, "cannot reduce a string column");

    if (magic == DF_COUNT) {
        if (!mask)
            return JS_NewInt64(ctx, b.n);
        for (i = 0; i < b.n; i++)
            count += (mask[i] != 0);
        return JS_NewInt64(ctx, count);
    }

    switch (magic) {
    case DF_MIN:
        switch ((int)(b.type)) {
#define DF_CASE(sfx, cty, tag) DF_REDUCE_CASE(min, sfx, cty, tag)
            DF_NUMERIC_TYPES(DF_CASE)
#undef DF_CASE
        default:
            return JS_ThrowTypeError(ctx, "cannot reduce a string column");
        }
        break;
    case DF_MAX:
        switch ((int)(b.type)) {
#define DF_CASE(sfx, cty, tag) DF_REDUCE_CASE(max, sfx, cty, tag)
            DF_NUMERIC_TYPES(DF_CASE)
#undef DF_CASE
        default:
            return JS_ThrowTypeError(ctx, "cannot reduce a string column");
        }
        break;
    default:
        switch ((int)(b.type)) {
#define DF_CASE(sfx, cty, tag) DF_REDUCE_CASE(sum, sfx, cty, tag)
            DF_NUMERIC_TYPES(DF_CASE)
#undef DF_CASE
        default:
            return JS_ThrowTypeError(ctx, "cannot reduce a string column");
        }
        break;
    }

    switch (magic) {
    case DF_MIN:
    case DF_MAX:
        return count ? JS_NewFloat64(ctx, acc) : JS_UNDEFINED;
    case DF_MEAN:
        return count ? JS_NewFloat64(ctx, acc / count)
                     : JS_NewFloat64(ctx, DYN_NAN);
    default:
        return JS_NewFloat64(ctx, acc);
    }
}
#define DF_STEP_MUL(acc, v) ((acc) *= (v))
#define DF_STEP_AND(acc, v) ((acc) &= (v))
#define DF_STEP_OR(acc, v) ((acc) |= (v))
#define DF_STEP_XOR(acc, v) ((acc) ^= (v))
#define DF_COMBINE_MUL(p, q) ((p) * (q))
#define DF_COMBINE_AND(p, q) ((p) & (q))
#define DF_COMBINE_OR(p, q) ((p) | (q))
#define DF_COMBINE_XOR(p, q) ((p) ^ (q))

#define X(suffix, ctype, tag)                                \
    DF_DEFINE_REDUCE_KM(DF_ACCS_FLOAT, DF_NACC_MASKED_FLOAT, \
        product, suffix, ctype, double, 1.0,                 \
        DF_STEP_MUL, DF_COMBINE_MUL)
DF_NUMERIC_TYPES(X)
#undef X

#define X(suffix, ctype, tag)                                           \
    DF_DEFINE_REDUCE_K(DF_ACCS_INT, band, suffix, ctype, uint32_t, ~0u, \
        DF_STEP_AND, DF_COMBINE_AND)                                    \
    DF_DEFINE_REDUCE_K(DF_ACCS_INT, bor, suffix, ctype, uint32_t, 0u,   \
        DF_STEP_OR, DF_COMBINE_OR)                                      \
    DF_DEFINE_REDUCE_K(DF_ACCS_INT, bxor, suffix, ctype, uint32_t, 0u,  \
        DF_STEP_XOR, DF_COMBINE_XOR)
DF_INT_TYPES(X)
#undef X

#define DF_DEFINE_VARIANCE(suffix, ctype, tag)                                                                                                                  \
    static double df_variance_##suffix(const ctype* restrict x,                                                                                                 \
        const uint8_t* restrict m, uint32_t n)                                                                                                                  \
    {                                                                                                                                                           \
        double s0 = 0, s1 = 0, s2 = 0, s3 = 0, s4 = 0, s5 = 0, s6 = 0, s7 = 0;                                                                                  \
        double q0 = 0, q1 = 0, q2 = 0, q3 = 0, q4 = 0, q5 = 0, q6 = 0, q7 = 0;                                                                                  \
        double mu, d0, d1, d2, d3, d4, d5, d6, d7;                                                                                                              \
        uint32_t i = 0, k = 0;                                                                                                                                  \
        if (m) {                                                                                                                                                \
            for (; i + 8 <= n; i += 8) {                                                                                                                        \
                s0 += m[i] ? (double)x[i] : 0.0;                                                                                                                \
                s1 += m[i + 1] ? (double)x[i + 1] : 0.0;                                                                                                        \
                s2 += m[i + 2] ? (double)x[i + 2] : 0.0;                                                                                                        \
                s3 += m[i + 3] ? (double)x[i + 3] : 0.0;                                                                                                        \
                s4 += m[i + 4] ? (double)x[i + 4] : 0.0;                                                                                                        \
                s5 += m[i + 5] ? (double)x[i + 5] : 0.0;                                                                                                        \
                s6 += m[i + 6] ? (double)x[i + 6] : 0.0;                                                                                                        \
                s7 += m[i + 7] ? (double)x[i + 7] : 0.0;                                                                                                        \
                k += (m[i] != 0) + (m[i + 1] != 0) + (m[i + 2] != 0) + (m[i + 3] != 0) + (m[i + 4] != 0) + (m[i + 5] != 0) + (m[i + 6] != 0) + (m[i + 7] != 0); \
            }                                                                                                                                                   \
            for (; i < n; i++) {                                                                                                                                \
                s0 += m[i] ? (double)x[i] : 0.0;                                                                                                                \
                k += (m[i] != 0);                                                                                                                               \
            }                                                                                                                                                   \
        } else {                                                                                                                                                \
            for (; i + 8 <= n; i += 8) {                                                                                                                        \
                s0 += (double)x[i];                                                                                                                             \
                s1 += (double)x[i + 1];                                                                                                                         \
                s2 += (double)x[i + 2];                                                                                                                         \
                s3 += (double)x[i + 3];                                                                                                                         \
                s4 += (double)x[i + 4];                                                                                                                         \
                s5 += (double)x[i + 5];                                                                                                                         \
                s6 += (double)x[i + 6];                                                                                                                         \
                s7 += (double)x[i + 7];                                                                                                                         \
            }                                                                                                                                                   \
            for (; i < n; i++)                                                                                                                                  \
                s0 += (double)x[i];                                                                                                                             \
            k = n;                                                                                                                                              \
        }                                                                                                                                                       \
        if (k < 2)                                                                                                                                              \
            return DYN_NAN;                                                                                                                                     \
        mu = (((s0 + s1) + (s2 + s3)) + ((s4 + s5) + (s6 + s7))) / (double)k;                                                                                   \
        i = 0;                                                                                                                                                  \
        if (m) {                                                                                                                                                \
            for (; i + 8 <= n; i += 8) {                                                                                                                        \
                d0 = m[i] ? (double)x[i] - mu : 0.0;                                                                                                            \
                d1 = m[i + 1] ? (double)x[i + 1] - mu : 0.0;                                                                                                    \
                d2 = m[i + 2] ? (double)x[i + 2] - mu : 0.0;                                                                                                    \
                d3 = m[i + 3] ? (double)x[i + 3] - mu : 0.0;                                                                                                    \
                d4 = m[i + 4] ? (double)x[i + 4] - mu : 0.0;                                                                                                    \
                d5 = m[i + 5] ? (double)x[i + 5] - mu : 0.0;                                                                                                    \
                d6 = m[i + 6] ? (double)x[i + 6] - mu : 0.0;                                                                                                    \
                d7 = m[i + 7] ? (double)x[i + 7] - mu : 0.0;                                                                                                    \
                q0 += d0 * d0;                                                                                                                                  \
                q1 += d1 * d1;                                                                                                                                  \
                q2 += d2 * d2;                                                                                                                                  \
                q3 += d3 * d3;                                                                                                                                  \
                q4 += d4 * d4;                                                                                                                                  \
                q5 += d5 * d5;                                                                                                                                  \
                q6 += d6 * d6;                                                                                                                                  \
                q7 += d7 * d7;                                                                                                                                  \
            }                                                                                                                                                   \
            for (; i < n; i++) {                                                                                                                                \
                d0 = m[i] ? (double)x[i] - mu : 0.0;                                                                                                            \
                q0 += d0 * d0;                                                                                                                                  \
            }                                                                                                                                                   \
        } else {                                                                                                                                                \
            for (; i + 8 <= n; i += 8) {                                                                                                                        \
                d0 = (double)x[i] - mu;                                                                                                                         \
                d1 = (double)x[i + 1] - mu;                                                                                                                     \
                d2 = (double)x[i + 2] - mu;                                                                                                                     \
                d3 = (double)x[i + 3] - mu;                                                                                                                     \
                d4 = (double)x[i + 4] - mu;                                                                                                                     \
                d5 = (double)x[i + 5] - mu;                                                                                                                     \
                d6 = (double)x[i + 6] - mu;                                                                                                                     \
                d7 = (double)x[i + 7] - mu;                                                                                                                     \
                q0 += d0 * d0;                                                                                                                                  \
                q1 += d1 * d1;                                                                                                                                  \
                q2 += d2 * d2;                                                                                                                                  \
                q3 += d3 * d3;                                                                                                                                  \
                q4 += d4 * d4;                                                                                                                                  \
                q5 += d5 * d5;                                                                                                                                  \
                q6 += d6 * d6;                                                                                                                                  \
                q7 += d7 * d7;                                                                                                                                  \
            }                                                                                                                                                   \
            for (; i < n; i++) {                                                                                                                                \
                d0 = (double)x[i] - mu;                                                                                                                         \
                q0 += d0 * d0;                                                                                                                                  \
            }                                                                                                                                                   \
        }                                                                                                                                                       \
        return (((q0 + q1) + (q2 + q3)) + ((q4 + q5) + (q6 + q7)))                                                                                              \
            / (double)(k - 1);                                                                                                                                  \
    }
DF_NUMERIC_TYPES(DF_DEFINE_VARIANCE)

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

#define DF_DOT_BLOCK 128

static void df_widen_block(double* restrict dst, const void* src, DFType t,
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

#define DF_ALLANY_BLOCK 4096u

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

#define DF_SWAR_LOW7 0x7f7f7f7f7f7f7f7full
#define DF_SWAR_HIGH 0x8080808080808080ull
#define DF_SWAR_GATHER 0x0002040810204081ull
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define DF_PACK_SWAR 1
#endif

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
enum { DF_VAR_SAMPLE,
    DF_VAR_STDDEV };
enum { DF_BIT_AND,
    DF_BIT_OR,
    DF_BIT_XOR };
enum { DF_ALL,
    DF_ANY };

static JSValue dyn_df_product(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DataFrame* df;
    DFBound b;
    const uint8_t* mask;
    int idx, ok;
    uint32_t count = 0;
    double acc;

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

#define X(suffix, ctype, tag)                                          \
    case tag:                                                          \
        acc = mask                                                     \
            ? df_product_masked_##suffix((const ctype*)b.p, mask, b.n, \
                  &count)                                              \
            : df_product_##suffix((const ctype*)b.p, b.n);             \
        break;
    switch ((int)(b.type)) {
        DF_NUMERIC_TYPES(X)
    default:
        return JS_ThrowTypeError(ctx, "cannot reduce a string column");
    }
#undef X
    (void)count;
    return JS_NewFloat64(ctx, acc);
}

static JSValue dyn_df_variance(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    DataFrame* df;
    DFBound b;
    const uint8_t* mask;
    int idx, ok;
    double v;

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

#define X(suffix, ctype, tag)                                   \
    case tag:                                                   \
        v = df_variance_##suffix((const ctype*)b.p, mask, b.n); \
        break;
    switch ((int)(b.type)) {
        DF_NUMERIC_TYPES(X)
    default:
        return JS_ThrowTypeError(ctx, "cannot reduce a string column");
    }
#undef X
    return JS_NewFloat64(ctx, magic == DF_VAR_STDDEV ? sqrt(v) : v);
}

static JSValue dyn_df_dot_product(JSContext* ctx, JSValueConst this_val,
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

static JSValue dyn_df_bitwise(JSContext* ctx, JSValueConst this_val,
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

static JSValue dyn_df_all_any(JSContext* ctx, JSValueConst this_val,
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

static JSValue dyn_df_bitmask(JSContext* ctx, JSValueConst this_val,
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

enum { DF_GT,
    DF_GE,
    DF_LT,
    DF_LE,
    DF_EQ,
    DF_NE };

static JSValue df_to_typed_array(JSContext* ctx, void* p, size_t nbytes,
    JSTypedArrayEnum type)
{
    JSValueConst args[3];
    JSValue ab, out;

    ab = JS_NewArrayBufferTake(ctx, p, nbytes);
    if (JS_IsException(ab))
        return ab;
    args[0] = ab;
    args[1] = JS_UNDEFINED;
    args[2] = JS_UNDEFINED;
    out = JS_NewTypedArray(ctx, 3, args, type);
    JS_FreeValue(ctx, ab);
    return out;
}

static JSValue dyn_df_compare(JSContext* ctx, JSValueConst this_val,
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
#define DF_CAT_(a, b) a##b
#define DF_CAT(a, b) DF_CAT_(a, b)
#define DF_KERNEL(kind, name, suffix) \
    DF_CAT(DF_CAT(df_, kind), DF_CAT(DF_CAT(_, name), DF_CAT(_, suffix)))

typedef void (*DFMapFn)(const void* src, double* restrict dst, uint32_t n);
typedef void (*DFMapKFn)(const void* src, double* restrict dst, uint32_t n,
    double k0, double k1);
typedef void (*DFMaskFn)(const void* src, uint8_t* restrict dst, uint32_t n,
    double k0, double k1);
typedef void (*DFCombFn)(const double* a, const void* src,
    double* restrict dst, uint32_t n);

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

#define DF_DEFINE_MAPK(name, suffix, ctype, EXPR)              \
    static void DF_KERNEL(mapk, name, suffix)(const void* src, \
        double* restrict dst,                                  \
        uint32_t n, double k0, double k1)                      \
    {                                                          \
        const ctype* x = (const ctype*)src;                    \
        uint32_t i;                                            \
        for (i = 0; i < n; i++) {                              \
            double v = (double)x[i];                           \
            dst[i] = (EXPR);                                   \
        }                                                      \
    }

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

#define DF_MAP_ONE(suffix, ctype, tag) DF_DEFINE_MAP(DF_OP_NAME, suffix, ctype, DF_OP_EXPR)
#define DF_MAPK_ONE(suffix, ctype, tag) DF_DEFINE_MAPK(DF_OP_NAME, suffix, ctype, DF_OP_EXPR)
#define DF_MASK_ONE(suffix, ctype, tag) DF_DEFINE_MASK(DF_OP_NAME, suffix, ctype, DF_OP_EXPR)
#define DF_COMB_ONE(suffix, ctype, tag) DF_DEFINE_COMB(DF_OP_NAME, suffix, ctype, DF_OP_EXPR)
#define DF_MAP_ENTRY(suffix, ctype, tag) [tag] = DF_KERNEL(map, DF_OP_NAME, suffix),
#define DF_MAPK_ENTRY(suffix, ctype, tag) [tag] = DF_KERNEL(mapk, DF_OP_NAME, suffix),
#define DF_MASK_ENTRY(suffix, ctype, tag) [tag] = DF_KERNEL(mask, DF_OP_NAME, suffix),
#define DF_COMB_ENTRY(suffix, ctype, tag) [tag] = DF_KERNEL(comb, DF_OP_NAME, suffix),

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

#define DF_OP_NAME widen
#define DF_OP_EXPR v
DF_NUMERIC_TYPES(DF_MAP_ONE)
static const DFMapFn df_map_widen_tab[DF_STR] = { DF_NUMERIC_TYPES(DF_MAP_ENTRY) };
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

enum { DF_MAP_ABS,
    DF_MAP_ROUND,
    DF_MAP_FLOOR,
    DF_MAP_CEIL,
    DF_MAP_SQRT,
    DF_MAP_LOG,
    DF_MAP_EXP,
    DF_MAP_SIGN,
    DF_MAP_N };

static const DFMapFn* const df_map1_tab[DF_MAP_N] = {
    df_map_abs_tab,
    df_map_round_tab,
    df_map_floor_tab,
    df_map_ceil_tab,
    df_map_sqrt_tab,
    df_map_log_tab,
    df_map_exp_tab,
    df_map_sign_tab,
};
static const char* const df_map1_name[DF_MAP_N] = {
    "ABS",
    "ROUND",
    "FLOOR",
    "CEIL",
    "SQRT",
    "LOG",
    "EXP",
    "SIGN",
};

enum { DF_BIN_ADD,
    DF_BIN_SUB,
    DF_BIN_MUL,
    DF_BIN_DIV,
    DF_BIN_POW,
    DF_BIN_NCOMB,
    DF_BIN_RSUB = DF_BIN_NCOMB,
    DF_BIN_RDIV,
    DF_BIN_N };

static const DFCombFn* const df_comb_tab[DF_BIN_NCOMB] = {
    df_comb_add_tab,
    df_comb_sub_tab,
    df_comb_mul_tab,
    df_comb_div_tab,
    df_comb_pow_tab,
};
static const DFMapKFn* const df_bin_scalar_tab[DF_BIN_N] = {
    df_mapk_addk_tab,
    df_mapk_subk_tab,
    df_mapk_mulk_tab,
    df_mapk_divk_tab,
    df_mapk_powk_tab,
    df_mapk_rsubk_tab,
    df_mapk_rdivk_tab,
};
static const char* const df_bin_name[DF_BIN_N] = {
    "ADD",
    "SUB",
    "MUL",
    "DIV",
    "POW",
    "RSUB",
    "RDIV",
};

_Static_assert(countof(df_map1_tab) == countof(df_map1_name),
    "map table and name table must agree");
_Static_assert(countof(df_bin_scalar_tab) == countof(df_bin_name),
    "binary table and name table must agree");
_Static_assert(DF_BIN_NCOMB < DF_BIN_N,
    "the scalar-only ops must follow the column-capable ones");

static void* df_out_alloc(JSContext* ctx, uint32_t n, size_t esz)
{
    void* p;
    if (esz && n > SIZE_MAX / esz) {
        JS_ThrowOutOfMemory(ctx);
        return NULL;
    }
    p = malloc(n ? (size_t)n * esz : 1);
    if (!p)
        JS_ThrowOutOfMemory(ctx);
    return p;
}

static int df_bind_numeric(JSContext* ctx, const DataFrame* df, int idx,
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

static int df_out_bag_check(JSContext* ctx, JSValueConst o, const char* op)
{
    static const char* const keys[] = { "out" };
    JSPropertyEnum* props = NULL;
    uint32_t nprops = 0, i;
    int j, k, bad = 0;

    if (!JS_IsObject(o))
        return 0;
    if (JS_GetOwnPropertyNames(ctx, &props, &nprops, o,
            JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY))
        return -1;
    for (i = 0; i < nprops && !bad; i++) {
        const char* name = JS_AtomToCString(ctx, props[i].atom);
        if (!name) {
            bad = 1;
            break;
        }
        for (k = 0; k < (int)countof(keys); k++)
            if (strcmp(name, keys[k]) == 0)
                break;
        if (k == (int)countof(keys)) {
            JS_ThrowTypeError(ctx, "%s: unknown option \"%s\" (valid: out)",
                op, name);
            JS_FreeCString(ctx, name);
            bad = 1;
            break;
        }
        JS_FreeCString(ctx, name);
    }
    for (j = 0; j < (int)nprops; j++)
        JS_FreeAtom(ctx, props[j].atom);
    js_free(ctx, props);
    return bad ? -1 : 1;
}

static int df_arg_is_bag(JSContext* ctx, JSValueConst v)
{
    JSPropertyEnum* props = NULL;
    uint32_t nprops = 0, i;
    int isbag = 1;

    if (!JS_IsObject(v) || JS_IsArray(ctx, v))
        return 0;
    if (JS_GetBufferKind(v) != JS_BUFFER_KIND_NONE)
        return 0;
    if (JS_GetOwnPropertyNames(ctx, &props, &nprops, v,
            JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY)
        < 0) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return 0;
    }
    for (i = 0; i < nprops && isbag; i++) {
        const char* name = JS_AtomToCString(ctx, props[i].atom);
        if (name && *name) {
            const char* c;
            int digits = 1;
            for (c = name; *c; c++)
                if (*c < '0' || *c > '9') {
                    digits = 0;
                    break;
                }
            if (digits)
                isbag = 0;
        }
        JS_FreeCString(ctx, name);
    }
    for (i = 0; i < nprops; i++)
        JS_FreeAtom(ctx, props[i].atom);
    js_free(ctx, props);
    return isbag;
}

static int df_is_out_bag(JSContext* ctx, JSValueConst v)
{
    JSPropertyEnum* props = NULL;
    uint32_t nprops = 0, i;
    int has = 0;

    if (!df_arg_is_bag(ctx, v))
        return 0;
    if (JS_GetOwnPropertyNames(ctx, &props, &nprops, v,
            JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY)
        < 0) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return 0;
    }
    for (i = 0; i < nprops && !has; i++) {
        const char* name = JS_AtomToCString(ctx, props[i].atom);
        if (name && strcmp(name, "out") == 0)
            has = 1;
        JS_FreeCString(ctx, name);
    }
    for (i = 0; i < nprops; i++)
        JS_FreeAtom(ctx, props[i].atom);
    js_free(ctx, props);
    return has;
}

static int df_out_opt(JSContext* ctx, const DataFrame* df, JSValueConst v,
    const char* op, int* pout_idx)
{
    JSValue ov;
    int idx, rc;

    rc = df_out_bag_check(ctx, v, op);
    if (rc <= 0)
        return rc;
    ov = JS_GetPropertyStr(ctx, v, "out");
    if (JS_IsException(ov))
        return -1;
    if (!JS_IsString(ov)) {
        JS_FreeValue(ctx, ov);
        JS_ThrowTypeError(ctx, "%s: the \"out\" option names a column, as a "
                               "string",
            op);
        return -1;
    }
    idx = df_col_arg(ctx, df, ov);
    JS_FreeValue(ctx, ov);
    if (idx < 0)
        return -1;
    if (df->cols[idx].type != DF_F64) {
        JS_ThrowTypeError(ctx, "%s {out}: column '%s' is %s; the in-place "
                               "target must be an existing Float64Array column",
            op, df->cols[idx].name,
            df_type_name(df->cols[idx].type));
        return -1;
    }
    if (df->cols[idx].length != df->nrows) {
        JS_ThrowRangeError(ctx, "%s {out}: column '%s' has %u rows, the frame "
                                "has %u",
            op, df->cols[idx].name,
            df->cols[idx].length, df->nrows);
        return -1;
    }
    *pout_idx = idx;
    return 1;
}

static int df_store_out(JSContext* ctx, DataFrame* df, int out_idx,
    const double* dst, uint32_t n)
{
    DFBound b;
    double* p;

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

static uint32_t df_map_span(uint32_t nrows, uint32_t avail, double* dst)
{
    uint32_t n = avail < nrows ? avail : nrows, i;
    for (i = n; i < nrows; i++)
        dst[i] = DYN_NAN;
    return n;
}

static JSValue dyn_df_map1(JSContext* ctx, JSValueConst this_val,
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

static JSValue dyn_df_isna(JSContext* ctx, JSValueConst this_val,
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

static JSValue dyn_df_between(JSContext* ctx, JSValueConst this_val,
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

static JSValue dyn_df_clip(JSContext* ctx, JSValueConst this_val,
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

static JSValue dyn_df_fillna(JSContext* ctx, JSValueConst this_val,
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

static JSValue dyn_df_binary(JSContext* ctx, JSValueConst this_val,
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

static JSValue dyn_df_where(JSContext* ctx, JSValueConst this_val,
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

#define DF_KEYS_ONE(CTYPE)                 \
    do {                                   \
        const CTYPE* x = b->p;             \
        for (i = 0; i < n; i++)            \
            k[i] = (uint32_t)(double)x[i]; \
    } while (0)

static uint32_t* df_keys_u32(const DFBound* b, uint32_t n)
{
    uint32_t *k, i;

    if (!n)
        return NULL;
    k = malloc((size_t)n * sizeof(*k));
    if (!k)
        return NULL;
    switch ((int)(b->type)) {
    case DF_I32:
    case DF_STR:
        DF_KEYS_ONE(int32_t);
        break;
    case DF_U32:
        DF_KEYS_ONE(uint32_t);
        break;
    case DF_I16:
        DF_KEYS_ONE(int16_t);
        break;
    case DF_U16:
        DF_KEYS_ONE(uint16_t);
        break;
    case DF_I8:
        DF_KEYS_ONE(int8_t);
        break;
    case DF_U8:
        DF_KEYS_ONE(uint8_t);
        break;
    default:
        DF_KEYS_ONE(double);
        break;
    }
    return k;
}
#undef DF_KEYS_ONE

static int dfc_group_count(JSContext* ctx, const DataFrame* df, int ki,
    uint32_t* pnkeys, uint32_t* pngroups)
{
    uint32_t ngroups;

    if (df->cols[ki].type == DF_STR) {
        ngroups = df->cols[ki].dict_len;
    } else {
        DFBound b;
        double mx = 0;
        uint32_t i;

        if (dyn_df_bind(ctx, df, ki, &b))
            return -1;
        if (b.type == DF_F64 || b.type == DF_F32) {
            JS_ThrowTypeError(ctx, "group key must be an integer or string "
                                   "column");
            return -1;
        }
        {
            double hi = 0.0;
            int neg = 0;
#define DF_GROUP_RANGE(CTYPE)       \
    do {                            \
        const CTYPE* x = b.p;       \
        CTYPE a = 0, z = 0;         \
        for (i = 0; i < b.n; i++) { \
            CTYPE v = x[i];         \
            if (v < a)              \
                a = v;              \
            if (v > z)              \
                z = v;              \
        }                           \
        if ((double)a < 0.0)        \
            neg = 1;                \
        hi = (double)z;             \
    } while (0)
            switch ((int)(b.type)) {
            case DF_I32:
            case DF_STR:
                DF_GROUP_RANGE(int32_t);
                break;
            case DF_U32:
                DF_GROUP_RANGE(uint32_t);
                break;
            case DF_I16:
                DF_GROUP_RANGE(int16_t);
                break;
            case DF_U16:
                DF_GROUP_RANGE(uint16_t);
                break;
            case DF_I8:
                DF_GROUP_RANGE(int8_t);
                break;
            default:
                DF_GROUP_RANGE(uint8_t);
                break;
            }
#undef DF_GROUP_RANGE
            if (neg) {
                JS_ThrowRangeError(ctx, "negative group key");
                return -1;
            }
            mx = hi;
        }
        if (mx + 1 > (double)DF_MAX_GROUPS) {
            JS_ThrowRangeError(ctx, "too many groups (max %d)", DF_MAX_GROUPS);
            return -1;
        }
        ngroups = b.n ? (uint32_t)mx + 1 : 0;
    }
    *pnkeys = ngroups;
    *pngroups = ngroups ? ngroups : 1;
    return 0;
}

static JSValue dyn_df_group_by_sum(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DataFrame* df;
    DFBound kb, vb;
    const uint8_t* mask;
    int ki, vi, ok;
    uint32_t i, ngroups, nkeys, g;
    double* acc = NULL;
    JSValue keys = JS_UNDEFINED, vals = JS_UNDEFINED, res = JS_UNDEFINED;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    ki = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (ki < 0)
        return JS_EXCEPTION;
    vi = df_col_arg(ctx, df, argc > 1 ? argv[1] : JS_UNDEFINED);
    if (vi < 0)
        return JS_EXCEPTION;
    mask = df_mask_arg(ctx, argc > 2 ? argv[2] : JS_UNDEFINED, df->nrows, &ok);
    if (!ok)
        return JS_EXCEPTION;

    if (dfc_group_count(ctx, df, ki, &nkeys, &ngroups))
        return JS_EXCEPTION;

    acc = calloc(ngroups, sizeof(double));
    if (!acc)
        return JS_ThrowOutOfMemory(ctx);

    if (dyn_df_bind(ctx, df, ki, &kb) || dyn_df_bind(ctx, df, vi, &vb))
        goto fail;
    if (vb.type == DF_STR) {
        JS_ThrowTypeError(ctx, "cannot sum a string column");
        goto fail;
    }

    {
        uint32_t n = kb.n < vb.n ? kb.n : vb.n;
        uint32_t* gk = df_keys_u32(&kb, n);
        if (vb.type == DF_F64) {
            const double* x = vb.p;
            for (i = 0; i < n; i++) {
                if (mask && !mask[i])
                    continue;
                g = gk ? gk[i] : (uint32_t)df_get(kb.p, kb.type, i);
                if (g < ngroups)
                    acc[g] += x[i];
            }
        } else {
            for (i = 0; i < n; i++) {
                if (mask && !mask[i])
                    continue;
                g = gk ? gk[i] : (uint32_t)df_get(kb.p, kb.type, i);
                if (g < ngroups)
                    acc[g] += df_get(vb.p, vb.type, i);
            }
        }
        free(gk);
    }
    vals = df_to_typed_array(ctx, acc, (size_t)nkeys * sizeof(double),
        JS_TYPED_ARRAY_FLOAT64);
    acc = NULL;
    if (JS_IsException(vals))
        goto fail;

    keys = JS_NewArray(ctx);
    if (JS_IsException(keys))
        goto fail;
    for (i = 0; i < nkeys; i++) {
        JSValue k;
        if (df->cols[ki].type == DF_STR)
            k = JS_NewString(ctx, df->cols[ki].dict[i]);
        else
            k = JS_NewInt64(ctx, i);
        if (JS_IsException(k) || JS_DefinePropertyValueUint32(ctx, keys, i, k, JS_PROP_C_W_E) < 0)
            goto fail;
    }

    res = JS_NewObject(ctx);
    if (JS_IsException(res))
        goto fail;
    if (JS_DefinePropertyValueStr(ctx, res, "keys", keys, JS_PROP_C_W_E) < 0) {
        keys = JS_UNDEFINED;
        goto fail;
    }
    keys = JS_UNDEFINED;
    if (JS_DefinePropertyValueStr(ctx, res, "values", vals, JS_PROP_C_W_E) < 0) {
        vals = JS_UNDEFINED;
        goto fail;
    }
    free(acc);
    return res;

fail:
    free(acc);
    JS_FreeValue(ctx, keys);
    JS_FreeValue(ctx, vals);
    JS_FreeValue(ctx, res);
    return JS_EXCEPTION;
}

typedef struct {
    double key;
    uint32_t idx;
} DfoItem;

static int dfo_cmp_asc(const void* pa, const void* pb)
{
    const DfoItem *a = pa, *b = pb;
    int na = isnan(a->key), nb = isnan(b->key);

    if (na | nb) {
        if (na != nb)
            return na - nb;
    } else if (a->key < b->key) {
        return -1;
    } else if (a->key > b->key) {
        return 1;
    }
    return a->idx < b->idx ? -1 : (a->idx > b->idx ? 1 : 0);
}

static int dfo_cmp_desc(const void* pa, const void* pb)
{
    const DfoItem *a = pa, *b = pb;
    int na = isnan(a->key), nb = isnan(b->key);

    if (na | nb) {
        if (na != nb)
            return na - nb;
    } else if (a->key > b->key) {
        return -1;
    } else if (a->key < b->key) {
        return 1;
    }
    return a->idx < b->idx ? -1 : (a->idx > b->idx ? 1 : 0);
}

static inline int dfo_before(const DfoItem* a, const DfoItem* b, int desc)
{
    int na = a->key != a->key, nb = b->key != b->key;

    if (na | nb) {
        if (na != nb)
            return nb;
    } else if (a->key != b->key) {
        return desc ? (a->key > b->key) : (a->key < b->key);
    }
    return a->idx < b->idx;
}

static inline void dfo_swap(DfoItem* a, DfoItem* b)
{
    DfoItem t = *a;
    *a = *b;
    *b = t;
}

static uint32_t dfo_pivot(DfoItem* it, uint32_t n, int desc)
{
    uint32_t m = n >> 1, last = n - 1;

    if (dfo_before(&it[m], &it[0], desc))
        dfo_swap(&it[m], &it[0]);
    if (dfo_before(&it[last], &it[m], desc))
        dfo_swap(&it[last], &it[m]);
    if (dfo_before(&it[m], &it[0], desc))
        dfo_swap(&it[m], &it[0]);
    return m;
}

static uint32_t dfo_partition(DfoItem* it, uint32_t n, uint32_t p, int desc)
{
    DfoItem pivot = it[p];
    uint32_t i = 0, j = n - 1;

    for (;;) {
        while (dfo_before(&it[i], &pivot, desc))
            i++;
        while (dfo_before(&pivot, &it[j], desc))
            j--;
        if (i >= j)
            return j;
        dfo_swap(&it[i], &it[j]);
        i++;
        if (j == 0)
            return 0;
        j--;
    }
}

static void dfo_insertion(DfoItem* it, uint32_t n, int desc)
{
    uint32_t i, j;

    for (i = 1; i < n; i++) {
        DfoItem v = it[i];
        for (j = i; j > 0 && dfo_before(&v, &it[j - 1], desc); j--)
            it[j] = it[j - 1];
        it[j] = v;
    }
}

#define DFO_SMALL 24
static void dfo_sort(DfoItem* it, uint32_t n, int desc, int depth)
{
    while (n > DFO_SMALL) {
        uint32_t p;
        if (depth-- <= 0) {
            qsort(it, n, sizeof(*it), desc ? dfo_cmp_desc : dfo_cmp_asc);
            return;
        }
        p = dfo_partition(it, n, dfo_pivot(it, n, desc), desc);
        dfo_sort(it, p + 1, desc, depth);
        it += p + 1;
        n -= p + 1;
    }
    dfo_insertion(it, n, desc);
}

static void dfo_select(DfoItem* it, uint32_t n, uint32_t k, int desc)
{
    while (n > DFO_SMALL) {
        uint32_t p = dfo_partition(it, n, dfo_pivot(it, n, desc), desc);
        if (k <= p) {
            n = p + 1;
        } else {
            it += p + 1;
            n -= p + 1;
            k -= p + 1;
        }
    }
    dfo_insertion(it, n, desc);
}

#define DFO_GATHER(CTYPE)                     \
    do {                                      \
        const CTYPE* x = b->p;                \
        if (mask) {                           \
            for (i = 0; i < span; i++)        \
                if (mask[i]) {                \
                    it[n].key = (double)x[i]; \
                    it[n].idx = i;            \
                    n++;                      \
                }                             \
        } else {                              \
            for (i = 0; i < span; i++) {      \
                it[i].key = (double)x[i];     \
                it[i].idx = i;                \
            }                                 \
            n = span;                         \
        }                                     \
    } while (0)

static uint32_t dfo_gather(const DFBound* b, const uint8_t* mask,
    uint32_t span, DfoItem* it)
{
    uint32_t i, n = 0;

    switch ((int)(b->type)) {
    case DF_F64:
        DFO_GATHER(double);
        break;
    case DF_F32:
        DFO_GATHER(float);
        break;
    case DF_I32:
        DFO_GATHER(int32_t);
        break;
    case DF_U32:
        DFO_GATHER(uint32_t);
        break;
    case DF_I16:
        DFO_GATHER(int16_t);
        break;
    case DF_U16:
        DFO_GATHER(uint16_t);
        break;
    case DF_I8:
        DFO_GATHER(int8_t);
        break;
    default:
        DFO_GATHER(uint8_t);
        break;
    }
    return n;
}
#undef DFO_GATHER

static inline uint64_t dfo_skey(double v)
{
    uint64_t u;
    memcpy(&u, &v, sizeof(u));
    return (u >> 63) ? ~u : (u | 0x8000000000000000ULL);
}

static inline double dfo_unskey(uint64_t u)
{
    double v;
    uint64_t b = (u >> 63) ? (u & ~0x8000000000000000ULL) : ~u;
    memcpy(&v, &b, sizeof(v));
    return v;
}

static uint32_t dfv_gather(const DFBound* b, const uint8_t* mask,
    uint32_t span, double* a)
{
    uint32_t i, n = 0;

    if (b->type == DF_F64 && !mask) {
        const double* x = b->p;
        for (i = 0; i < span; i++) {
            a[n] = x[i];
            n += (x[i] == x[i]);
        }
        return n;
    }
    for (i = 0; i < span; i++) {
        double v;
        if (mask && !mask[i])
            continue;
        v = df_get(b->p, b->type, i);
        a[n] = v;
        n += (v == v);
    }
    return n;
}

static uint32_t dfv_gather_keys(const DFBound* b, const uint8_t* mask,
    uint32_t span, uint64_t* k,
    uint64_t* pmin, uint64_t* pmax)
{
    uint64_t lo = ~(uint64_t)0, hi = 0;
    uint32_t i, n = 0;

    if (b->type == DF_F64 && !mask) {
        const double* x = b->p;
        for (i = 0; i < span; i++) {
            uint64_t u = dfo_skey(x[i]);
            k[n] = u;
            if (x[i] == x[i]) {
                n++;
                if (u < lo)
                    lo = u;
                if (u > hi)
                    hi = u;
            }
        }
    } else {
        for (i = 0; i < span; i++) {
            double v;
            uint64_t u;
            if (mask && !mask[i])
                continue;
            v = df_get(b->p, b->type, i);
            u = dfo_skey(v);
            k[n] = u;
            if (v == v) {
                n++;
                if (u < lo)
                    lo = u;
                if (u > hi)
                    hi = u;
            }
        }
    }
    *pmin = lo;
    *pmax = hi;
    return n;
}

static void dfv_insertion(double* a, uint32_t n)
{
    uint32_t i, j;

    for (i = 1; i < n; i++) {
        double v = a[i];
        for (j = i; j > 0 && v < a[j - 1]; j--)
            a[j] = a[j - 1];
        a[j] = v;
    }
}

static double dfv_radix_select(uint64_t* k, uint32_t n, uint32_t rank,
    uint64_t kmin, uint64_t kmax)
{
    uint64_t diff = kmin ^ kmax;
    int level;

    if (!diff || n <= 1)
        return dfo_unskey(k[0]);
    level = (63 - df_clz64(diff)) / 8;
    for (; level >= 0 && n > 1; level--) {
        uint32_t hist[256], i, acc = 0, d, o = 0, sh = (uint32_t)level * 8;
        memset(hist, 0, sizeof(hist));
        for (i = 0; i < n; i++)
            hist[(k[i] >> sh) & 0xff]++;
        for (d = 0; d < 255; d++) {
            if (acc + hist[d] > rank)
                break;
            acc += hist[d];
        }
        if (hist[d] == n)
            continue;
        rank -= acc;
        for (i = 0; i < n; i++)
            if (((k[i] >> sh) & 0xff) == d)
                k[o++] = k[i];
        n = o;
    }
    return dfo_unskey(k[0]);
}

static double dfv_next_above(const DFBound* b, const uint8_t* mask,
    uint32_t span, double v, uint32_t rank)
{
    uint32_t i, le = 0;
    double nx = DYN_INFINITY;

    if (b->type == DF_F64 && !mask) {
        const double* x = b->p;
        for (i = 0; i < span; i++) {
            le += (x[i] <= v);
            if (x[i] > v && x[i] < nx)
                nx = x[i];
        }
    } else {
        for (i = 0; i < span; i++) {
            double x;
            if (mask && !mask[i])
                continue;
            x = df_get(b->p, b->type, i);
            le += (x <= v);
            if (x > v && x < nx)
                nx = x;
        }
    }
    return le >= rank + 2 ? v : nx;
}

static void dfv_select(double* a, uint32_t n, uint32_t k)
{
    while (n > DFO_SMALL) {
        double p, t;
        int64_t i = -1, j = (int64_t)n;
        uint32_t m = n >> 1, last = n - 1;

        if (a[m] < a[0]) {
            t = a[m];
            a[m] = a[0];
            a[0] = t;
        }
        if (a[last] < a[m]) {
            t = a[last];
            a[last] = a[m];
            a[m] = t;
        }
        if (a[m] < a[0]) {
            t = a[m];
            a[m] = a[0];
            a[0] = t;
        }
        p = a[m];
        for (;;) {
            do {
                i++;
            } while (a[i] < p);
            do {
                j--;
            } while (a[j] > p);
            if (i >= j)
                break;
            t = a[i];
            a[i] = a[j];
            a[j] = t;
        }
        if (k <= (uint32_t)j) {
            n = (uint32_t)j + 1;
        } else {
            a += (uint32_t)j + 1;
            n -= (uint32_t)j + 1;
            k -= (uint32_t)j + 1;
        }
    }
    dfv_insertion(a, n);
}

#define DFO_RADIX_MIN 2048u

static int dfo_radix(DfoItem* it, uint32_t n, int desc)
{
    uint64_t *ka = NULL, *kb = NULL, *ks, *kd;
    uint32_t *ia = NULL, *ib = NULL, *is, *id;
    DfoItem* nanv = NULL;
    uint32_t (*hist)[256] = NULL, i, p, nn = 0, nz = 0;
    int negzero = 0;

    if (n < DFO_RADIX_MIN)
        return -1;
    ka = malloc((size_t)n * sizeof(*ka));
    kb = malloc((size_t)n * sizeof(*kb));
    ia = malloc((size_t)n * sizeof(*ia));
    ib = malloc((size_t)n * sizeof(*ib));
    hist = calloc(8, sizeof(*hist));
    if (!ka || !kb || !ia || !ib || !hist)
        goto decline;

    for (i = 0; i < n; i++) {
        uint64_t k, raw;
        if (it[i].key != it[i].key) {
            nz++;
            continue;
        }
        memcpy(&raw, &it[i].key, sizeof(raw));
        negzero |= (raw == 0x8000000000000000ULL);
        k = dfo_skey(it[i].key);
        if (desc)
            k = ~k;
        ka[nn] = k;
        ia[nn] = it[i].idx;
        nn++;
        hist[0][k & 0xff]++;
        hist[1][(k >> 8) & 0xff]++;
        hist[2][(k >> 16) & 0xff]++;
        hist[3][(k >> 24) & 0xff]++;
        hist[4][(k >> 32) & 0xff]++;
        hist[5][(k >> 40) & 0xff]++;
        hist[6][(k >> 48) & 0xff]++;
        hist[7][(k >> 56) & 0xff]++;
    }
    if (negzero)
        goto decline;
    if (nz) {
        nanv = malloc((size_t)nz * sizeof(*nanv));
        if (!nanv)
            goto decline;
        nz = 0;
        for (i = 0; i < n; i++)
            if (it[i].key != it[i].key)
                nanv[nz++] = it[i];
    }

    ks = ka;
    kd = kb;
    is = ia;
    id = ib;
    for (p = 0; p < 8; p++) {
        uint32_t *h = hist[p], sum = 0, shift = p * 8;
        if (nn && h[(ks[0] >> shift) & 0xff] == nn)
            continue;
        for (i = 0; i < 256; i++) {
            uint32_t c = h[i];
            h[i] = sum;
            sum += c;
        }
        for (i = 0; i < nn; i++) {
            uint32_t d = h[(ks[i] >> shift) & 0xff]++;
            kd[d] = ks[i];
            id[d] = is[i];
        }
        {
            uint64_t* tk = ks;
            ks = kd;
            kd = tk;
        }
        {
            uint32_t* ti = is;
            is = id;
            id = ti;
        }
    }
    for (i = 0; i < nn; i++) {
        it[i].key = dfo_unskey(desc ? ~ks[i] : ks[i]);
        it[i].idx = is[i];
    }
    for (i = 0; i < nz; i++)
        it[nn + i] = nanv[i];
    free(nanv);
    free(ka);
    free(kb);
    free(ia);
    free(ib);
    free(hist);
    return 0;
decline:
    free(nanv);
    free(ka);
    free(kb);
    free(ia);
    free(ib);
    free(hist);
    return -1;
}

static int dfo_sorted(JSContext* ctx, const DFBound* b, const uint8_t* mask,
    uint32_t nrows, int desc, DfoItem** out, uint32_t* pn)
{
    uint32_t span = b->n < nrows ? b->n : nrows, n;
    DfoItem* it = df_out_alloc(ctx, span, sizeof(DfoItem));

    if (!it)
        return -1;
    n = dfo_gather(b, mask, span, it);
    if (n > 1 && dfo_radix(it, n, desc) != 0) {
        int depth = 2;
        uint32_t t = n;
        while (t >>= 1)
            depth += 2;
        dfo_sort(it, n, desc, depth);
    }
    *out = it;
    *pn = n;
    return 0;
}

static int dfo_gathered(JSContext* ctx, const DFBound* b, const uint8_t* mask,
    uint32_t nrows, DfoItem** out, uint32_t* pn,
    uint32_t* pvalued)
{
    uint32_t span = b->n < nrows ? b->n : nrows, n, i, m = 0;
    DfoItem* it = df_out_alloc(ctx, span, sizeof(DfoItem));

    if (!it)
        return -1;
    n = dfo_gather(b, mask, span, it);
    for (i = 0; i < n; i++)
        m += it[i].key == it[i].key;
    *out = it;
    *pn = n;
    *pvalued = m;
    return 0;
}

static uint32_t dfo_valued(const DfoItem* it, uint32_t n)
{
    while (n > 0 && isnan(it[n - 1].key))
        n--;
    return n;
}

static int dfo_open(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv, int nscalar, const char* op,
    DataFrame** pdf, DFBound* b, double* pscalar,
    const uint8_t** pmask, int* pout_idx)
{
    DataFrame* df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    int idx, ok, slot;
    JSValueConst maskv = JS_UNDEFINED;

    if (!df)
        return -1;
    idx = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (idx < 0)
        return -1;
    if (pout_idx)
        *pout_idx = -1;
    if (nscalar && argc > 1 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1]) && pout_idx && df_is_out_bag(ctx, argv[1])) {
        if (pscalar)
            *pscalar = 0.0;
        if (df_out_opt(ctx, df, argv[1], op, pout_idx) < 0)
            return -1;
    } else if (nscalar && JS_ToFloat64(ctx, pscalar, argc > 1 ? argv[1] : JS_UNDEFINED))
        return -1;
    for (slot = 1 + nscalar; slot < argc; slot++) {
        JSValueConst v = argv[slot];
        if (JS_IsUndefined(v) || JS_IsNull(v))
            continue;
        if (pout_idx && *pout_idx < 0 && df_is_out_bag(ctx, v)) {
            if (df_out_opt(ctx, df, v, op, pout_idx) < 0)
                return -1;
            continue;
        }
        if (JS_IsUndefined(maskv))
            maskv = v;
    }
    *pmask = df_mask_arg(ctx, maskv, df->nrows, &ok);
    if (!ok)
        return -1;
    if (df_bind_numeric(ctx, df, idx, b, op))
        return -1;
    *pdf = df;
    return 0;
}

enum { DFO_SORT,
    DFO_ARGSORT };

static JSValue dyn_df_sort(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    static const char* const dfo_sort_name[2] = { "SORT", "ARG_SORT" };
    DataFrame* df;
    DFBound b;
    const uint8_t* mask;
    DfoItem* it;
    uint32_t n, i;

    if (dfo_open(ctx, this_val, argc, argv, 0, dfo_sort_name[magic],
            &df, &b, NULL, &mask, NULL))
        return JS_EXCEPTION;
    if (dfo_sorted(ctx, &b, mask, df->nrows, 0, &it, &n))
        return JS_EXCEPTION;

    if (magic == DFO_ARGSORT) {
        uint32_t* dst = df_out_alloc(ctx, n, sizeof(uint32_t));
        if (!dst) {
            free(it);
            return JS_EXCEPTION;
        }
        for (i = 0; i < n; i++)
            dst[i] = it[i].idx;
        free(it);
        return df_to_typed_array(ctx, dst, (size_t)n * sizeof(uint32_t),
            JS_TYPED_ARRAY_UINT32);
    } else {
        double* dst = df_out_alloc(ctx, n, sizeof(double));
        if (!dst) {
            free(it);
            return JS_EXCEPTION;
        }
        for (i = 0; i < n; i++)
            dst[i] = it[i].key;
        free(it);
        return df_to_typed_array(ctx, dst, (size_t)n * sizeof(double),
            JS_TYPED_ARRAY_FLOAT64);
    }
}

static JSValue dyn_df_rank(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DataFrame* df;
    DFBound b;
    const uint8_t* mask;
    DfoItem* it;
    double* dst;
    uint32_t nrows, n, m, i, j;

    if (dfo_open(ctx, this_val, argc, argv, 0, "RANK", &df, &b, NULL, &mask, NULL))
        return JS_EXCEPTION;
    nrows = df->nrows;
    if (dfo_sorted(ctx, &b, mask, nrows, 0, &it, &n))
        return JS_EXCEPTION;
    dst = df_out_alloc(ctx, nrows, sizeof(double));
    if (!dst) {
        free(it);
        return JS_EXCEPTION;
    }
    for (i = 0; i < nrows; i++)
        dst[i] = DYN_NAN;

    m = dfo_valued(it, n);
    i = 0;
    while (i < m) {
        double r;
        for (j = i + 1; j < m && it[j].key == it[i].key; j++)
            ;
        r = ((double)(i + 1) + (double)j) * 0.5;
        for (; i < j; i++)
            dst[it[i].idx] = r;
    }
    free(it);
    return df_to_typed_array(ctx, dst, (size_t)nrows * sizeof(double),
        JS_TYPED_ARRAY_FLOAT64);
}

enum { DFO_Q_CONT,
    DFO_Q_PCONT,
    DFO_Q_DISC,
    DFO_Q_MEDIAN };

static double dfq_from_keys(uint64_t* k, uint32_t m, double q, int magic,
    uint64_t kmin, uint64_t kmax,
    const DFBound* b, const uint8_t* mask, uint32_t span)
{
    double pos, frac, lo_v, hi_v;
    uint32_t lo;

    if (m == 0)
        return DYN_NAN;
    if (magic == DFO_Q_DISC) {
        double t = ceil(q * (double)m);
        uint32_t i = t <= 1.0 ? 0 : (uint32_t)t - 1;
        if (i >= m)
            i = m - 1;
        return dfv_radix_select(k, m, i, kmin, kmax);
    }
    pos = q * (double)(m - 1);
    lo = (uint32_t)pos;
    frac = pos - (double)lo;
    lo_v = dfv_radix_select(k, m, lo, kmin, kmax);
    if (frac == 0.0 || lo + 1 >= m)
        return lo_v;
    hi_v = dfv_next_above(b, mask, span, lo_v, lo);
    return lo_v + frac * (hi_v - lo_v);
}

static JSValue dyn_df_quantile(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    static const char* const dfo_q_name[4] = {
        "QUANTILE", "PERCENTILE_CONT", "PERCENTILE_DISC", "MEDIAN"
    };
    DataFrame* df;
    DFBound b;
    const uint8_t* mask;
    double *a, q = 0.5, res;
    uint32_t span, m;

    if (dfo_open(ctx, this_val, argc, argv, magic != DFO_Q_MEDIAN,
            dfo_q_name[magic], &df, &b, &q, &mask, NULL))
        return JS_EXCEPTION;
    if (magic != DFO_Q_MEDIAN && (isnan(q) || q < 0.0 || q > 1.0))
        return JS_ThrowRangeError(ctx, "%s(col, q): q must be in [0, 1], got %g",
            dfo_q_name[magic], q);
    span = b.n < df->nrows ? b.n : df->nrows;
    if (span >= DFO_RADIX_MIN && (magic == DFO_Q_DISC || (q >= 0.25 && q <= 0.75))) {
        uint64_t *k = df_out_alloc(ctx, span, sizeof(uint64_t)), kmin, kmax;
        if (!k)
            return JS_EXCEPTION;
        m = dfv_gather_keys(&b, mask, span, k, &kmin, &kmax);
        res = dfq_from_keys(k, m, q, magic, kmin, kmax, &b, mask, span);
        free(k);
        return m ? JS_NewFloat64(ctx, res) : JS_UNDEFINED;
    }
    a = df_out_alloc(ctx, span ? span : 1, sizeof(double));
    if (!a)
        return JS_EXCEPTION;
    m = dfv_gather(&b, mask, span, a);

    if (m == 0) {
        free(a);
        return JS_UNDEFINED;
    }
    if (magic == DFO_Q_DISC) {
        double t = ceil(q * (double)m);
        uint32_t i = t <= 1.0 ? 0 : (uint32_t)t - 1;
        if (i >= m)
            i = m - 1;
        dfv_select(a, m, i);
        res = a[i];
    } else {
        double pos = q * (double)(m - 1);
        uint32_t lo = (uint32_t)pos;
        double frac = pos - (double)lo;
        dfv_select(a, m, lo);
        if (frac == 0.0 || lo + 1 >= m) {
            res = a[lo];
        } else {
            dfv_select(a + lo + 1, m - lo - 1, 0);
            res = a[lo] + frac * (a[lo + 1] - a[lo]);
        }
    }
    free(a);
    return JS_NewFloat64(ctx, res);
}

enum { DFO_TOP_LARGEST,
    DFO_TOP_SMALLEST };

static JSValue dyn_df_nlargest(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    static const char* const dfo_top_name[2] = { "N_LARGEST", "N_SMALLEST" };
    DataFrame* df;
    DFBound b;
    const uint8_t* mask;
    DfoItem* it;
    double *dst, kd = 0;
    uint32_t n, m, k, i;

    if (dfo_open(ctx, this_val, argc, argv, 1, dfo_top_name[magic],
            &df, &b, &kd, &mask, NULL))
        return JS_EXCEPTION;
    if (isnan(kd) || isinf(kd) || kd < 0.0 || kd != floor(kd))
        return JS_ThrowRangeError(ctx, "%s(col, k): k must be a non-negative "
                                       "integer, got %g",
            dfo_top_name[magic], kd);
    if (dfo_sorted(ctx, &b, mask, df->nrows, magic == DFO_TOP_LARGEST, &it, &n))
        return JS_EXCEPTION;

    m = dfo_valued(it, n);
    k = kd >= (double)m ? m : (uint32_t)kd;
    dst = df_out_alloc(ctx, k, sizeof(double));
    if (!dst) {
        free(it);
        return JS_EXCEPTION;
    }
    for (i = 0; i < k; i++)
        dst[i] = it[i].key;
    free(it);
    return df_to_typed_array(ctx, dst, (size_t)k * sizeof(double),
        JS_TYPED_ARRAY_FLOAT64);
}

enum { DFS_CUMSUM,
    DFS_CUMPROD,
    DFS_CUMMAX,
    DFS_CUMMIN,
    DFS_SHIFT,
    DFS_DIFF,
    DFS_N };

static const char* const dfs_name[] = {
    "CUM_SUM",
    "CUM_PROD",
    "CUM_MAX",
    "CUM_MIN",
    "SHIFT",
    "DIFF",
};
_Static_assert(countof(dfs_name) == DFS_N,
    "every scan magic needs a name: a missing entry is a NULL "
    "passed to %s in a throw, not a compile error");

#define DF_WIDEN_ONE(CTYPE)        \
    do {                           \
        const CTYPE* x = b->p;     \
        for (i = 0; i < n; i++)    \
            dst[i] = (double)x[i]; \
    } while (0)

static void df_widen(const DFBound* b, double* dst, uint32_t n)
{
    uint32_t i;

    if (n == 0)
        return;
    switch ((int)(b->type)) {
    case DF_F64:
        memcpy(dst, b->p, (size_t)n * sizeof(double));
        return;
    case DF_F32:
        DF_WIDEN_ONE(float);
        break;
    case DF_I32:
        DF_WIDEN_ONE(int32_t);
        break;
    case DF_U32:
        DF_WIDEN_ONE(uint32_t);
        break;
    case DF_I16:
        DF_WIDEN_ONE(int16_t);
        break;
    case DF_U16:
        DF_WIDEN_ONE(uint16_t);
        break;
    case DF_I8:
        DF_WIDEN_ONE(int8_t);
        break;
    case DF_U8:
        DF_WIDEN_ONE(uint8_t);
        break;
    default:
        DF_WIDEN_ONE(int32_t);
        break;
    }
}
#undef DF_WIDEN_ONE

static void dfs_widen(const DFBound* b, double* dst, uint32_t n)
{
    df_widen(b, dst, n);
}

#define DFS_SCAN(name, STEP, ID)                           \
    static void dfs_##name(double* v, uint32_t n)          \
    {                                                      \
        double acc = (ID);                                 \
        uint32_t i;                                        \
        for (i = 0; i < n; i++) {                          \
            double x = v[i];                               \
            STEP(acc, x);                                  \
            v[i] = acc;                                    \
        }                                                  \
    }                                                      \
    static void dfs_##name##_masked(double* v, uint32_t n, \
        const uint8_t* mask)                               \
    {                                                      \
        double acc = (ID);                                 \
        uint32_t i;                                        \
        for (i = 0; i < n; i++) {                          \
            double x = mask[i] ? v[i] : (ID);              \
            STEP(acc, x);                                  \
            v[i] = acc;                                    \
        }                                                  \
    }

DFS_SCAN(cumsum, DF_STEP_SUM, 0.0)
DFS_SCAN(cumprod, DF_STEP_MUL, 1.0)
DFS_SCAN(cummax, DF_STEP_MAX, -DYN_INFINITY)
DFS_SCAN(cummin, DF_STEP_MIN, DYN_INFINITY)
#undef DFS_SCAN

static JSValue dyn_df_cumsum(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    DataFrame* df;
    DFBound b;
    const uint8_t* mask;
    double* dst;
    uint32_t n, span;
    int idx, ok;

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
    dst = df_out_alloc(ctx, n, sizeof(double));
    if (!dst)
        return JS_EXCEPTION;
    if (df_bind_numeric(ctx, df, idx, &b, dfs_name[magic])) {
        free(dst);
        return JS_EXCEPTION;
    }
    span = df_map_span(n, b.n, dst);
    dfs_widen(&b, dst, span);
    switch (magic) {
    case DFS_CUMPROD:
        if (mask)
            dfs_cumprod_masked(dst, span, mask);
        else
            dfs_cumprod(dst, span);
        break;
    case DFS_CUMMAX:
        if (mask)
            dfs_cummax_masked(dst, span, mask);
        else
            dfs_cummax(dst, span);
        break;
    case DFS_CUMMIN:
        if (mask)
            dfs_cummin_masked(dst, span, mask);
        else
            dfs_cummin(dst, span);
        break;
    default:
        if (mask)
            dfs_cumsum_masked(dst, span, mask);
        else
            dfs_cumsum(dst, span);
        break;
    }
    return df_to_typed_array(ctx, dst, (size_t)n * sizeof(double),
        JS_TYPED_ARRAY_FLOAT64);
}

static JSValue dyn_df_shift(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    DataFrame* df;
    DFBound b;
    const double* src;
    double periods, *dst, *tmp = NULL;
    uint32_t n, span, i;
    int idx;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    idx = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (idx < 0)
        return JS_EXCEPTION;
    periods = 1.0;
    if (argc > 1 && !JS_IsUndefined(argv[1]) && JS_ToFloat64(ctx, &periods, argv[1]))
        return JS_EXCEPTION;
    if (isnan(periods) || periods != floor(periods))
        return JS_ThrowTypeError(ctx, "%s(col, periods): periods must be an "
                                      "integer",
            dfs_name[magic]);

    n = df->nrows;
    dst = df_out_alloc(ctx, n, sizeof(double));
    if (!dst)
        return JS_EXCEPTION;
    if (df_bind_numeric(ctx, df, idx, &b, dfs_name[magic])) {
        free(dst);
        return JS_EXCEPTION;
    }
    span = df_map_span(n, b.n, dst);
    if (!(fabs(periods) < (double)span)) {
        for (i = 0; i < span; i++)
            dst[i] = DYN_NAN;
        return df_to_typed_array(ctx, dst, (size_t)n * sizeof(double),
            JS_TYPED_ARRAY_FLOAT64);
    }
    if (b.type == DF_F64) {
        src = b.p;
    } else {
        tmp = df_out_alloc(ctx, span, sizeof(double));
        if (!tmp) {
            free(dst);
            return JS_EXCEPTION;
        }
        dfs_widen(&b, tmp, span);
        src = tmp;
    }
    {
        int64_t k = (int64_t)periods, end = (int64_t)span, j;

        int64_t lo = k > 0 ? k : 0;
        int64_t hi = k > 0 ? end : end + k;
        (void)j;
        for (i = 0; i < (uint32_t)lo; i++)
            dst[i] = DYN_NAN;
        for (i = (uint32_t)hi; i < span; i++)
            dst[i] = DYN_NAN;
        if (magic == DFS_DIFF) {
            const double *a = src + lo, *bsrc = src + (lo - k);
            uint32_t m = (uint32_t)(hi - lo);
            double* o = dst + lo;
            for (i = 0; i < m; i++)
                o[i] = a[i] - bsrc[i];
        } else if (hi > lo) {
            memcpy(dst + lo, src + (lo - k),
                (size_t)(hi - lo) * sizeof(double));
        }
    }
    free(tmp);
    return df_to_typed_array(ctx, dst, (size_t)n * sizeof(double),
        JS_TYPED_ARRAY_FLOAT64);
}

#define DFC_EMPTY 0xFFFFFFFFu
#define DFC_MIN_SLOTS 64u

typedef struct {
    uint32_t* slots;
    uint64_t* keys;
    uint32_t* tags;
    uint32_t* counts;
    uint32_t nslots, mask, nent, cap_ent;
} DfcSet;

_Static_assert(DF_MAX_GROUPS > 0 && DF_MAX_GROUPS <= (1 << 29),
    "DF_MAX_GROUPS must leave room for 2x slots in a uint32");

static uint64_t dfc_key(double v)
{
    uint64_t b;

    if (v != v)
        return 0x7ff8000000000000ULL;
    memcpy(&b, &v, sizeof(b));
    if (b == 0x8000000000000000ULL)
        b = 0;
    return b;
}

static double dfc_key_value(uint64_t bits)
{
    double v;
    memcpy(&v, &bits, sizeof(v));
    return v;
}

static atomic_uint_least64_t dfc_hash_seed = 0x9e3779b97f4a7c15ULL;
static inline uint32_t df_str_hash_basis(void)
{
    uint64_t seed = atomic_load_explicit(&dfc_hash_seed, memory_order_relaxed);
    return 2166136261u ^ (uint32_t)(seed ^ (seed >> 32));
}

static void dfc_hash_seed_once(void)
{
    static atomic_flag seeded = ATOMIC_FLAG_INIT;
    uint64_t s;
    if (atomic_flag_test_and_set_explicit(&seeded, memory_order_relaxed))
        return;
    if (dyn_os_entropy(&s, sizeof s) == 0)
        atomic_store_explicit(&dfc_hash_seed, s, memory_order_relaxed);
}

static uint32_t dfc_hash(uint64_t k, uint32_t tag)
{
    uint64_t seed = atomic_load_explicit(&dfc_hash_seed, memory_order_relaxed);
    k ^= seed;
    k ^= (uint64_t)tag * 0x9e3779b97f4a7c15ULL;
    k ^= k >> 33;
    k *= 0xff51afd7ed558ccdULL;
    k ^= k >> 33;
    k *= 0xc4ceb9fe1a85ec53ULL;
    k ^= k >> 33;
    return (uint32_t)k;
}

static void dfc_set_free(DfcSet* s)
{
    free(s->slots);
    free(s->keys);
    free(s->tags);
    free(s->counts);
    memset(s, 0, sizeof(*s));
}

static int dfc_set_grow(JSContext* ctx, DfcSet* s)
{
    uint32_t nslots = s->nslots ? s->nslots << 1 : DFC_MIN_SLOTS;
    uint32_t cap = nslots >> 1;
    uint32_t *slots, *tags, *counts;
    uint64_t* keys;
    uint32_t i;

    if (s->nent >= (uint32_t)DF_MAX_GROUPS) {
        JS_ThrowRangeError(ctx, "too many distinct values (max %d)",
            DF_MAX_GROUPS);
        return -1;
    }
    slots = malloc((size_t)nslots * sizeof(*slots));
    keys = realloc(s->keys, (size_t)cap * sizeof(*keys));
    if (keys)
        s->keys = keys;
    tags = realloc(s->tags, (size_t)cap * sizeof(*tags));
    if (tags)
        s->tags = tags;
    counts = realloc(s->counts, (size_t)cap * sizeof(*counts));
    if (counts)
        s->counts = counts;
    if (!slots || !keys || !tags || !counts) {
        free(slots);
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    memset(slots, 0xFF, (size_t)nslots * sizeof(*slots));
    free(s->slots);
    s->slots = slots;
    s->nslots = nslots;
    s->mask = nslots - 1;
    s->cap_ent = cap;
    for (i = 0; i < s->nent; i++) {
        uint32_t p = dfc_hash(s->keys[i], s->tags[i]) & s->mask;
        while (s->slots[p] != DFC_EMPTY)
            p = (p + 1) & s->mask;
        s->slots[p] = i;
    }
    return 0;
}

static int dfc_set_put(JSContext* ctx, DfcSet* s, uint64_t k, uint32_t tag,
    uint32_t* pent, int* pfresh)
{
    uint32_t p;

    if (s->nent >= s->cap_ent && dfc_set_grow(ctx, s))
        return -1;
    p = dfc_hash(k, tag) & s->mask;
    while (s->slots[p] != DFC_EMPTY) {
        uint32_t e = s->slots[p];
        if (s->keys[e] == k && s->tags[e] == tag) {
            s->counts[e]++;
            if (pent)
                *pent = e;
            if (pfresh)
                *pfresh = 0;
            return 0;
        }
        p = (p + 1) & s->mask;
    }
    s->slots[p] = s->nent;
    s->keys[s->nent] = k;
    s->tags[s->nent] = tag;
    s->counts[s->nent] = 1;
    if (pent)
        *pent = s->nent;
    if (pfresh)
        *pfresh = 1;
    s->nent++;
    return 0;
}

static int dfc_scan(JSContext* ctx, const DataFrame* df, int idx,
    const uint8_t* mask, DfcSet* set)
{
    DFBound b;
    uint32_t i, n;

    if (dyn_df_bind(ctx, df, idx, &b))
        return -1;
    n = b.n < df->nrows ? b.n : df->nrows;
    double *wv = NULL, *own = NULL;
    if (n) {
        if (b.type == DF_F64)
            wv = DYN_UNCONST(b.p);
        else {
            wv = own = malloc((size_t)n * sizeof(double));
            if (wv)
                df_widen(&b, wv, n);
        }
    }
    for (i = 0; i < n; i++) {
        if (mask && !mask[i])
            continue;
        if (dfc_set_put(ctx, set, dfc_key(wv ? wv[i] : df_get(b.p, b.type, i)), 0,
                NULL, NULL)) {
            free(own);
            return -1;
        }
    }
    {
        free(own);
        return 0;
    }
}

static JSValue dfc_key_js(JSContext* ctx, const DFColumn* c, uint64_t bits)
{
    double v = dfc_key_value(bits);

    if (c->type == DF_STR) {
        uint32_t code = (uint32_t)v;
        if (code >= c->dict_len)
            return JS_ThrowRangeError(ctx, "dictionary code %u out of range",
                code);
        return JS_NewString(ctx, c->dict[code]);
    }
    return JS_NewFloat64(ctx, v);
}

static JSValue dfc_keys_array(JSContext* ctx, const DFColumn* c,
    const DfcSet* s, const uint32_t* ord, uint32_t n)
{
    JSValue a = JS_NewArray(ctx);
    uint32_t i;

    if (JS_IsException(a))
        return a;
    for (i = 0; i < n; i++) {
        JSValue k = dfc_key_js(ctx, c, s->keys[ord ? ord[i] : i]);
        if (JS_IsException(k) || JS_DefinePropertyValueUint32(ctx, a, i, k, JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, a);
            return JS_EXCEPTION;
        }
    }
    return a;
}

static JSValue dfc_pair(JSContext* ctx, JSValue keys, JSValue values)
{
    JSValue res;

    if (JS_IsException(keys) || JS_IsException(values)) {
        JS_FreeValue(ctx, keys);
        JS_FreeValue(ctx, values);
        return JS_EXCEPTION;
    }
    res = JS_NewObject(ctx);
    if (JS_IsException(res)) {
        JS_FreeValue(ctx, keys);
        JS_FreeValue(ctx, values);
        return JS_EXCEPTION;
    }
    if (JS_DefinePropertyValueStr(ctx, res, "keys", keys, JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, values);
        JS_FreeValue(ctx, res);
        return JS_EXCEPTION;
    }
    if (JS_DefinePropertyValueStr(ctx, res, "values", values,
            JS_PROP_C_W_E)
        < 0) {
        JS_FreeValue(ctx, res);
        return JS_EXCEPTION;
    }
    return res;
}

static JSValue dyn_df_unique(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DataFrame* df;
    DfcSet set;
    const uint8_t* mask;
    int idx, ok;
    uint32_t i;
    JSValue out;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    idx = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (idx < 0)
        return JS_EXCEPTION;
    mask = df_mask_arg(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, df->nrows, &ok);
    if (!ok)
        return JS_EXCEPTION;

    memset(&set, 0, sizeof(set));
    if (dfc_scan(ctx, df, idx, mask, &set)) {
        dfc_set_free(&set);
        return JS_EXCEPTION;
    }
    if (df->cols[idx].type == DF_STR) {
        out = dfc_keys_array(ctx, &df->cols[idx], &set, NULL, set.nent);
    } else {
        double* dst = df_out_alloc(ctx, set.nent, sizeof(double));
        if (!dst) {
            dfc_set_free(&set);
            return JS_EXCEPTION;
        }
        for (i = 0; i < set.nent; i++)
            dst[i] = dfc_key_value(set.keys[i]);
        out = df_to_typed_array(ctx, dst, (size_t)set.nent * sizeof(double),
            JS_TYPED_ARRAY_FLOAT64);
    }
    dfc_set_free(&set);
    return out;
}

static JSValue dyn_df_nunique(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DataFrame* df;
    DfcSet set;
    const uint8_t* mask;
    int idx, ok;
    uint32_t n;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    idx = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (idx < 0)
        return JS_EXCEPTION;
    mask = df_mask_arg(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, df->nrows, &ok);
    if (!ok)
        return JS_EXCEPTION;

    memset(&set, 0, sizeof(set));
    if (dfc_scan(ctx, df, idx, mask, &set)) {
        dfc_set_free(&set);
        return JS_EXCEPTION;
    }
    n = set.nent;
    dfc_set_free(&set);
    return JS_NewInt64(ctx, n);
}

enum { DFC_VC_ALL,
    DFC_VC_TOPK };

static int dfc_cmp_packed(const void* a, const void* b)
{
    uint64_t x = *(const uint64_t*)a, y = *(const uint64_t*)b;
    return (x > y) - (x < y);
}

static uint32_t* dfc_rank_order(JSContext* ctx, const DfcSet* s)
{
    uint64_t* packed;
    uint32_t* ord;
    uint32_t i;

    ord = df_out_alloc(ctx, s->nent, sizeof(uint32_t));
    if (!ord)
        return NULL;
    packed = df_out_alloc(ctx, s->nent, sizeof(uint64_t));
    if (!packed) {
        free(ord);
        return NULL;
    }
    for (i = 0; i < s->nent; i++)
        packed[i] = ((uint64_t)(0xFFFFFFFFu - s->counts[i]) << 32) | i;
    qsort(packed, s->nent, sizeof(*packed), dfc_cmp_packed);
    for (i = 0; i < s->nent; i++)
        ord[i] = (uint32_t)packed[i];
    free(packed);
    return ord;
}

static JSValue dyn_df_value_counts(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    DataFrame* df;
    DfcSet set;
    const uint8_t* mask;
    uint32_t* ord = NULL;
    double* cnt = NULL;
    int idx, ok, mask_arg = (magic == DFC_VC_TOPK) ? 2 : 1;
    int32_t k = 0;
    uint32_t i, nout;
    JSValue keys, values;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    idx = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (idx < 0)
        return JS_EXCEPTION;
    if (magic == DFC_VC_TOPK) {
        if (argc < 2 || JS_IsUndefined(argv[1]))
            return JS_ThrowTypeError(ctx, "TOP_K(col, k): k is required");
        if (JS_ToInt32(ctx, &k, argv[1]))
            return JS_EXCEPTION;
        if (k < 0)
            return JS_ThrowRangeError(ctx, "TOP_K: k must not be negative");
    }
    mask = df_mask_arg(ctx, argc > mask_arg ? argv[mask_arg] : JS_UNDEFINED,
        df->nrows, &ok);
    if (!ok)
        return JS_EXCEPTION;

    memset(&set, 0, sizeof(set));
    if (dfc_scan(ctx, df, idx, mask, &set))
        goto fail;
    ord = dfc_rank_order(ctx, &set);
    if (!ord)
        goto fail;
    nout = set.nent;
    if (magic == DFC_VC_TOPK && (uint32_t)k < nout)
        nout = (uint32_t)k;
    cnt = df_out_alloc(ctx, nout, sizeof(double));
    if (!cnt)
        goto fail;
    for (i = 0; i < nout; i++)
        cnt[i] = set.counts[ord[i]];

    keys = dfc_keys_array(ctx, &df->cols[idx], &set, ord, nout);
    values = df_to_typed_array(ctx, cnt, (size_t)nout * sizeof(double),
        JS_TYPED_ARRAY_FLOAT64);
    free(ord);
    dfc_set_free(&set);
    return dfc_pair(ctx, keys, values);

fail:
    free(cnt);
    free(ord);
    dfc_set_free(&set);
    return JS_EXCEPTION;
}

static JSValue dyn_df_mode(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DataFrame* df;
    DfcSet set;
    const uint8_t* mask;
    int idx, ok;
    uint32_t i, best = 0, bestc = 0;
    JSValue out;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    idx = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (idx < 0)
        return JS_EXCEPTION;
    mask = df_mask_arg(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, df->nrows, &ok);
    if (!ok)
        return JS_EXCEPTION;

    memset(&set, 0, sizeof(set));
    if (dfc_scan(ctx, df, idx, mask, &set)) {
        dfc_set_free(&set);
        return JS_EXCEPTION;
    }
    for (i = 0; i < set.nent; i++)
        if (set.counts[i] > bestc) {
            bestc = set.counts[i];
            best = i;
        }
    out = set.nent ? dfc_key_js(ctx, &df->cols[idx], set.keys[best])
                   : JS_UNDEFINED;
    dfc_set_free(&set);
    return out;
}

static JSValue dyn_df_drop_duplicates(JSContext* ctx, JSValueConst this_val,
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

enum { DFC_GROUP_ALL,
    DFC_GROUP_UNIQ };

typedef struct {
    double* flat;
    uint32_t *cnt, *off;
    uint32_t nkeys, ngroups, total, n;
} DfcGrouped;

static void dfc_grouped_free(DfcGrouped* G)
{
    free(G->flat);
    free(G->cnt);
    free(G->off);
    memset(G, 0, sizeof(*G));
}

static int dfc_group_gather(JSContext* ctx, DataFrame* df, int ki, int vi,
    const uint8_t* mask, int uniq, DfcGrouped* G)
{
    DFBound kb, vb;
    DfcSet set;
    uint8_t* keep = NULL;
    uint32_t i, g, n, total;

    memset(G, 0, sizeof(*G));
    memset(&set, 0, sizeof(set));
    if (dfc_group_count(ctx, df, ki, &G->nkeys, &G->ngroups))
        return -1;
    if (dyn_df_bind(ctx, df, ki, &kb) || dyn_df_bind(ctx, df, vi, &vb))
        return -1;
    if (vb.type == DF_STR) {
        JS_ThrowTypeError(ctx, "cannot collect %s into a group",
            df_type_name(vb.type));
        return -1;
    }
    n = kb.n < vb.n ? kb.n : vb.n;
    if (n > df->nrows)
        n = df->nrows;
    G->n = n;

    G->cnt = calloc(G->ngroups, sizeof(*G->cnt));
    G->off = malloc((size_t)G->ngroups * sizeof(*G->off));
    if (!G->cnt || !G->off) {
        JS_ThrowOutOfMemory(ctx);
        goto fail;
    }
    if (uniq) {
        keep = calloc(n ? n : 1, 1);
        if (!keep) {
            JS_ThrowOutOfMemory(ctx);
            goto fail;
        }
    }

    for (i = 0; i < n; i++) {
        if (mask && !mask[i])
            continue;
        g = (uint32_t)df_get(kb.p, kb.type, i);
        if (g >= G->ngroups)
            continue;
        if (uniq) {
            int fresh;
            if (dfc_set_put(ctx, &set, dfc_key(df_get(vb.p, vb.type, i)), g,
                    NULL, &fresh))
                goto fail;
            if (!fresh)
                continue;
            keep[i] = 1;
        }
        G->cnt[g]++;
    }

    total = 0;
    for (g = 0; g < G->ngroups; g++) {
        G->off[g] = total;
        total += G->cnt[g];
    }
    G->total = total;
    G->flat = df_out_alloc(ctx, total, sizeof(double));
    if (!G->flat)
        goto fail;

    for (i = 0; i < n; i++) {
        if (mask && !mask[i])
            continue;
        g = (uint32_t)df_get(kb.p, kb.type, i);
        if (g >= G->ngroups)
            continue;
        if (uniq && !keep[i])
            continue;
        G->flat[G->off[g]++] = df_get(vb.p, vb.type, i);
    }
    free(keep);
    dfc_set_free(&set);
    return 0;

fail:
    free(keep);
    dfc_set_free(&set);
    dfc_grouped_free(G);
    return -1;
}

static JSValue dfc_values_array(JSContext* ctx, const DfcGrouped* G)
{
    JSValue values = JS_NewArray(ctx);
    uint32_t g;

    if (JS_IsException(values))
        return values;
    for (g = 0; g < G->nkeys; g++) {
        double* slice = df_out_alloc(ctx, G->cnt[g], sizeof(double));
        JSValue ta;
        if (!slice) {
            JS_FreeValue(ctx, values);
            return JS_EXCEPTION;
        }
        memcpy(slice, G->flat + (G->off[g] - G->cnt[g]),
            (size_t)G->cnt[g] * sizeof(double));
        ta = df_to_typed_array(ctx, slice, (size_t)G->cnt[g] * sizeof(double),
            JS_TYPED_ARRAY_FLOAT64);
        if (JS_IsException(ta) || JS_DefinePropertyValueUint32(ctx, values, g, ta, JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, values);
            return JS_EXCEPTION;
        }
    }
    return values;
}

static JSValue dfc_dense_keys(JSContext* ctx, const DataFrame* df, int ki,
    uint32_t nkeys)
{
    JSValue keys = JS_NewArray(ctx);
    uint32_t g;

    if (JS_IsException(keys))
        return keys;
    for (g = 0; g < nkeys; g++) {
        JSValue k = (df->cols[ki].type == DF_STR)
            ? JS_NewString(ctx, df->cols[ki].dict[g])
            : JS_NewInt64(ctx, g);
        if (JS_IsException(k) || JS_DefinePropertyValueUint32(ctx, keys, g, k, JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, keys);
            return JS_EXCEPTION;
        }
    }
    return keys;
}

static JSValue dyn_df_group_array(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    DataFrame* df;
    DfcGrouped G;
    const uint8_t* mask;
    int ki, vi, ok;
    JSValue keys, values;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    ki = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (ki < 0)
        return JS_EXCEPTION;
    vi = df_col_arg(ctx, df, argc > 1 ? argv[1] : JS_UNDEFINED);
    if (vi < 0)
        return JS_EXCEPTION;
    mask = df_mask_arg(ctx, argc > 2 ? argv[2] : JS_UNDEFINED, df->nrows, &ok);
    if (!ok)
        return JS_EXCEPTION;

    if (dfc_group_gather(ctx, df, ki, vi, mask, magic == DFC_GROUP_UNIQ, &G))
        return JS_EXCEPTION;

    values = dfc_values_array(ctx, &G);
    keys = JS_IsException(values) ? JS_EXCEPTION
                                  : dfc_dense_keys(ctx, df, ki, G.nkeys);
    dfc_grouped_free(&G);
    if (JS_IsException(values) || JS_IsException(keys)) {
        JS_FreeValue(ctx, values);
        JS_FreeValue(ctx, keys);
        return JS_EXCEPTION;
    }
    return dfc_pair(ctx, keys, values);
}

enum { DFC_MOVING_SUM,
    DFC_MOVING_AVG };

#define DFG_ROLL_SLIDE_MIN 256u

static int dfc_moving_blocks(double* v, uint32_t m, uint32_t w, int want_avg)
{
    double* suf = malloc((size_t)w * sizeof(double));
    double* out = malloc((size_t)m * sizeof(double));
    double run = 0.0;
    uint32_t s, i;

    if (!suf || !out) {
        free(suf);
        free(out);
        return -1;
    }
    for (i = 0; i + 1 < w; i++) {
        run += v[i];
        out[i] = want_avg ? run / (double)(i + 1) : run;
    }
    for (s = 0; s < m; s += w) {
        uint32_t end = (s + w < m) ? s + w : m;
        uint32_t k, lo, hi;
        double r = 0.0, pre = 0.0;
        for (k = end; k-- > s;) {
            r += v[k];
            suf[k - s] = r;
        }
        lo = s + w - 1;
        hi = s + 2 * w - 2;
        if (hi >= m)
            hi = m - 1;
        for (k = lo; k <= hi; k++) {
            uint32_t st = k + 1 - w;
            double a;
            if (k >= end)
                pre += v[k];
            a = (st < end ? suf[st - s] : 0.0) + pre;
            out[k] = want_avg ? a / (double)w : a;
        }
    }
    memcpy(v, out, (size_t)m * sizeof(double));
    free(out);
    free(suf);
    return 0;
}

static void dfc_moving_slice(double* v, uint32_t m, uint32_t w, int want_avg)
{
    uint32_t i;

    if (w == 0 || w >= m) {
        double a = 0.0;
        for (i = 0; i < m; i++) {
            a += v[i];
            v[i] = want_avg ? a / (double)(i + 1) : a;
        }
        return;
    }
    if (w == 1)
        return;
    if (w >= DFG_ROLL_SLIDE_MIN && dfc_moving_blocks(v, m, w, want_avg) == 0)
        return;
    for (i = m; i-- > 0;) {
        uint32_t lo = (i + 1 >= w) ? i + 1 - w : 0, j, c = i - lo + 1;
        double a = 0.0;
        for (j = lo; j <= i; j++)
            a += v[j];
        v[i] = want_avg ? a / (double)c : a;
    }
}

static JSValue dyn_df_group_array_moving(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    DataFrame* df;
    DfcGrouped G;
    const uint8_t* mask;
    const char* op = magic == DFC_MOVING_AVG ? "GROUP_ARRAY_MOVING_AVG"
                                             : "GROUP_ARRAY_MOVING_SUM";
    double wd = 0.0;
    uint32_t g, w;
    int ki, vi, ok;
    JSValue keys, values;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    ki = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (ki < 0)
        return JS_EXCEPTION;
    vi = df_col_arg(ctx, df, argc > 1 ? argv[1] : JS_UNDEFINED);
    if (vi < 0)
        return JS_EXCEPTION;
    if (argc > 2 && !JS_IsUndefined(argv[2])) {
        if (JS_ToFloat64(ctx, &wd, argv[2]))
            return JS_EXCEPTION;
        if (!(wd >= 1) || wd != floor(wd) || wd > (double)UINT32_MAX)
            return JS_ThrowRangeError(ctx, "%s: window must be a positive "
                                           "integer, got %g",
                op, wd);
    }
    w = (uint32_t)wd;
    mask = df_mask_arg(ctx, argc > 3 ? argv[3] : JS_UNDEFINED, df->nrows, &ok);
    if (!ok)
        return JS_EXCEPTION;

    if (dfc_group_gather(ctx, df, ki, vi, mask, 0, &G))
        return JS_EXCEPTION;
    for (g = 0; g < G.nkeys; g++)
        dfc_moving_slice(G.flat + (G.off[g] - G.cnt[g]), G.cnt[g], w,
            magic == DFC_MOVING_AVG);

    values = dfc_values_array(ctx, &G);
    keys = JS_IsException(values) ? JS_EXCEPTION
                                  : dfc_dense_keys(ctx, df, ki, G.nkeys);
    dfc_grouped_free(&G);
    if (JS_IsException(values) || JS_IsException(keys)) {
        JS_FreeValue(ctx, values);
        JS_FreeValue(ctx, keys);
        return JS_EXCEPTION;
    }
    return dfc_pair(ctx, keys, values);
}

enum { DFP_HEAD,
    DFP_TAIL };
enum { DFP_FIRST,
    DFP_LAST };
enum { DFP_ARGMIN,
    DFP_ARGMAX };

#define DFP_DEFAULT_N 5u

static int dfp_bind_num(JSContext* ctx, const DataFrame* df, int idx,
    DFBound* b, const char* op)
{
    if (dyn_df_bind(ctx, df, idx, b))
        return -1;
    if (b->type == DF_STR) {
        JS_ThrowTypeError(ctx, "%s: cannot apply to %s", op,
            df_type_name(b->type));
        return -1;
    }
    return 0;
}

static uint32_t dfp_span(uint32_t nrows, uint32_t avail)
{
    return avail < nrows ? avail : nrows;
}

static uint32_t dfp_selected(const uint8_t* mask, uint32_t n)
{
    uint32_t i, k = 0;
    if (!mask)
        return n;
    for (i = 0; i < n; i++)
        k += (mask[i] != 0);
    return k;
}

static uint32_t dfp_first_selected(const uint8_t* mask, uint32_t n)
{
    uint32_t i;
    if (!mask)
        return n ? 0 : n;
    for (i = 0; i < n; i++)
        if (mask[i])
            return i;
    return n;
}

static uint32_t dfp_last_selected(const uint8_t* mask, uint32_t n)
{
    uint32_t i = n;
    if (!mask)
        return n ? n - 1 : n;
    while (i > 0) {
        i--;
        if (mask[i])
            return i;
    }
    return n;
}

static JSValue dyn_df_head(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    DataFrame* df;
    DFBound b;
    const uint8_t* mask;
    const char* op = (magic == DFP_TAIL) ? "TAIL" : "HEAD";
    double want_d = (double)DFP_DEFAULT_N;
    double* dst;
    JSValueConst nv;
    uint32_t nrows, n, want, avail, skip, take, w, i, seen;
    int idx, ok;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    idx = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (idx < 0)
        return JS_EXCEPTION;
    nv = argc > 1 ? argv[1] : JS_UNDEFINED;
    if (!JS_IsUndefined(nv) && !JS_IsNull(nv) && JS_ToFloat64(ctx, &want_d, nv))
        return JS_EXCEPTION;
    if (!(want_d >= 0))
        return JS_ThrowRangeError(ctx, "%s(col, n): n must be a non-negative "
                                       "number, got %g",
            op, want_d);
    nrows = df->nrows;
    want = (want_d >= (double)nrows) ? nrows : (uint32_t)want_d;
    mask = df_mask_arg(ctx, argc > 2 ? argv[2] : JS_UNDEFINED, nrows, &ok);
    if (!ok)
        return JS_EXCEPTION;

    dst = malloc(want ? (size_t)want * sizeof(double) : 1);
    if (!dst)
        return JS_ThrowOutOfMemory(ctx);
    if (dfp_bind_num(ctx, df, idx, &b, op)) {
        free(dst);
        return JS_EXCEPTION;
    }
    n = dfp_span(nrows, b.n);
    avail = dfp_selected(mask, n);
    take = (want < avail) ? want : avail;
    skip = (magic == DFP_TAIL) ? avail - take : 0;

    w = 0;
    if (!mask) {
        for (i = skip; w < take; i++)
            dst[w++] = df_get(b.p, b.type, i);
    } else {
        seen = 0;
        for (i = 0; i < n && w < take; i++) {
            if (!mask[i])
                continue;
            if (seen++ < skip)
                continue;
            dst[w++] = df_get(b.p, b.type, i);
        }
    }
    return df_to_typed_array(ctx, dst, (size_t)take * sizeof(double),
        JS_TYPED_ARRAY_FLOAT64);
}

static JSValue dyn_df_first(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    DataFrame* df;
    DFBound b;
    const uint8_t* mask;
    const char* op = (magic == DFP_LAST) ? "LAST" : "FIRST";
    uint32_t n, i;
    int idx, ok;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    idx = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (idx < 0)
        return JS_EXCEPTION;
    mask = df_mask_arg(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, df->nrows, &ok);
    if (!ok)
        return JS_EXCEPTION;

    if (dfp_bind_num(ctx, df, idx, &b, op))
        return JS_EXCEPTION;
    n = dfp_span(df->nrows, b.n);
    i = (magic == DFP_LAST) ? dfp_last_selected(mask, n)
                            : dfp_first_selected(mask, n);
    if (i >= n)
        return JS_UNDEFINED;
    return JS_NewFloat64(ctx, df_get(b.p, b.type, i));
}

static JSValue dyn_df_argmin(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    DataFrame* df;
    DFBound b;
    const uint8_t* mask;
    const char* op = (magic == DFP_ARGMAX) ? "ARG_MAX" : "ARG_MIN";
    double best = 0, v, *wv = NULL, *own = NULL;
    uint32_t n, i, at = 0;
    int idx, ok, have = 0;
    JSValue res;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    idx = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (idx < 0)
        return JS_EXCEPTION;
    mask = df_mask_arg(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, df->nrows, &ok);
    if (!ok)
        return JS_EXCEPTION;

    if (dfp_bind_num(ctx, df, idx, &b, op))
        return JS_EXCEPTION;
    n = dfp_span(df->nrows, b.n);
    if (n) {
        if (b.type == DF_F64) {
            wv = DYN_UNCONST(b.p);
        } else {
            wv = own = malloc((size_t)n * sizeof(double));
            if (wv)
                df_widen(&b, wv, n);
        }
    }

#define DFP_ARG_SCAN(BETTER)                         \
    do {                                             \
        for (i = 0; i < n; i++) {                    \
            if (mask && !mask[i])                    \
                continue;                            \
            v = wv ? wv[i] : df_get(b.p, b.type, i); \
            if (isnan(v))                            \
                continue;                            \
            if (!have) {                             \
                have = 1;                            \
                best = v;                            \
                at = i;                              \
            } else if (BETTER) {                     \
                best = v;                            \
                at = i;                              \
            }                                        \
        }                                            \
    } while (0)

#define DFP_LANES(BETTER)                                  \
    do {                                                   \
        double m0 = best, m1 = best, m2 = best, m3 = best; \
        uint32_t p0 = at, p1 = at, p2 = at, p3 = at;       \
        for (; i + 3 < n; i += 4) {                        \
            if (wv[i] BETTER m0) {                         \
                m0 = wv[i];                                \
                p0 = i;                                    \
            }                                              \
            if (wv[i + 1] BETTER m1) {                     \
                m1 = wv[i + 1];                            \
                p1 = i + 1;                                \
            }                                              \
            if (wv[i + 2] BETTER m2) {                     \
                m2 = wv[i + 2];                            \
                p2 = i + 2;                                \
            }                                              \
            if (wv[i + 3] BETTER m3) {                     \
                m3 = wv[i + 3];                            \
                p3 = i + 3;                                \
            }                                              \
        }                                                  \
        for (; i < n; i++)                                 \
            if (wv[i] BETTER m0) {                         \
                m0 = wv[i];                                \
                p0 = i;                                    \
            }                                              \
        if (m1 BETTER m0 || (m1 == m0 && p1 < p0)) {       \
            m0 = m1;                                       \
            p0 = p1;                                       \
        }                                                  \
        if (m3 BETTER m2 || (m3 == m2 && p3 < p2)) {       \
            m2 = m3;                                       \
            p2 = p3;                                       \
        }                                                  \
        if (m2 BETTER m0 || (m2 == m0 && p2 < p0)) {       \
            m0 = m2;                                       \
            p0 = p2;                                       \
        }                                                  \
        best = m0;                                         \
        at = p0;                                           \
    } while (0)

    if (wv && !mask) {
        for (i = 0; i < n; i++)
            if (!isnan(wv[i])) {
                have = 1;
                best = wv[i];
                at = i++;
                break;
            }
        if (have) {
            if (magic == DFP_ARGMAX)
                DFP_LANES(>);
            else
                DFP_LANES(<);
        }
    } else if (magic == DFP_ARGMAX)
        DFP_ARG_SCAN(v > best);
    else
        DFP_ARG_SCAN(v < best);
#undef DFP_ARG_SCAN

    res = have ? JS_NewInt64(ctx, at) : JS_UNDEFINED;
    free(own);
    return res;
}

enum { DFM_VARPOP,
    DFM_STDPOP,
    DFM_SKEW,
    DFM_KURT };
enum { DFM_COVPOP,
    DFM_COVSAMP,
    DFM_CORR,
    DFM_SLOPE,
    DFM_INTERCEPT,
    DFM_R2,
    DFM_AVGX,
    DFM_AVGY };

typedef struct {
    double n;
    double sx, sy;
    double mx, my;
    double m2x, m3x, m4x;
    double m2y;
    double cxy;
    double lo, hi;
} DFMoments;

#define DFM_UNROLL_MIN 64u

static void dfm_unrolled(const double* x, const double* y, uint32_t n,
    DFMoments* o)
{
    double s0 = 0, s1 = 0, s2 = 0, s3 = 0, s4 = 0, s5 = 0, s6 = 0, s7 = 0;
    double t0 = 0, t1 = 0, t2 = 0, t3 = 0, t4 = 0, t5 = 0, t6 = 0, t7 = 0;
    double lo0 = DYN_INFINITY, lo1 = DYN_INFINITY, lo2 = DYN_INFINITY, lo3 = DYN_INFINITY;
    double hi0 = -DYN_INFINITY, hi1 = -DYN_INFINITY, hi2 = -DYN_INFINITY, hi3 = -DYN_INFINITY;
    double a0 = 0, a1 = 0, a2 = 0, a3 = 0, b0 = 0, b1 = 0, b2 = 0, b3 = 0;
    double c0 = 0, c1 = 0, c2 = 0, c3 = 0, mx, my = 0.0, sx, sy = 0.0;
    uint32_t i, q = n & ~7u, r = n & ~3u;

    for (i = 0; i < q; i += 8) {
        s0 += x[i];
        s1 += x[i + 1];
        s2 += x[i + 2];
        s3 += x[i + 3];
        s4 += x[i + 4];
        s5 += x[i + 5];
        s6 += x[i + 6];
        s7 += x[i + 7];
    }
    for (; i < n; i++)
        s0 += x[i];
    sx = ((s0 + s1) + (s2 + s3)) + ((s4 + s5) + (s6 + s7));

    for (i = 0; i < r; i += 4) {
        if (x[i] < lo0)
            lo0 = x[i];
        if (x[i] > hi0)
            hi0 = x[i];
        if (x[i + 1] < lo1)
            lo1 = x[i + 1];
        if (x[i + 1] > hi1)
            hi1 = x[i + 1];
        if (x[i + 2] < lo2)
            lo2 = x[i + 2];
        if (x[i + 2] > hi2)
            hi2 = x[i + 2];
        if (x[i + 3] < lo3)
            lo3 = x[i + 3];
        if (x[i + 3] > hi3)
            hi3 = x[i + 3];
    }
    for (; i < n; i++) {
        if (x[i] < lo0)
            lo0 = x[i];
        if (x[i] > hi0)
            hi0 = x[i];
    }
    if (lo1 < lo0)
        lo0 = lo1;
    if (lo3 < lo2)
        lo2 = lo3;
    if (lo2 < lo0)
        lo0 = lo2;
    if (hi1 > hi0)
        hi0 = hi1;
    if (hi3 > hi2)
        hi2 = hi3;
    if (hi2 > hi0)
        hi0 = hi2;

    if (y) {
        for (i = 0; i < q; i += 8) {
            t0 += y[i];
            t1 += y[i + 1];
            t2 += y[i + 2];
            t3 += y[i + 3];
            t4 += y[i + 4];
            t5 += y[i + 5];
            t6 += y[i + 6];
            t7 += y[i + 7];
        }
        for (; i < n; i++)
            t0 += y[i];
        sy = ((t0 + t1) + (t2 + t3)) + ((t4 + t5) + (t6 + t7));
        my = sy / (double)n;
    }
    mx = sx / (double)n;

    if (y) {
        for (i = 0; i < r; i += 4) {
            double d0 = x[i] - mx, d1 = x[i + 1] - mx, d2 = x[i + 2] - mx, d3 = x[i + 3] - mx;
            double e0 = y[i] - my, e1 = y[i + 1] - my, e2 = y[i + 2] - my, e3 = y[i + 3] - my;
            a0 += d0 * d0;
            a1 += d1 * d1;
            a2 += d2 * d2;
            a3 += d3 * d3;
            b0 += e0 * e0;
            b1 += e1 * e1;
            b2 += e2 * e2;
            b3 += e3 * e3;
            c0 += d0 * e0;
            c1 += d1 * e1;
            c2 += d2 * e2;
            c3 += d3 * e3;
        }
        for (; i < n; i++) {
            double d = x[i] - mx, e = y[i] - my;
            a0 += d * d;
            b0 += e * e;
            c0 += d * e;
        }
        o->m2x = (a0 + a1) + (a2 + a3);
        o->m2y = (b0 + b1) + (b2 + b3);
        o->cxy = (c0 + c1) + (c2 + c3);
        o->m3x = DYN_NAN;
        o->m4x = DYN_NAN;
    } else {
        for (i = 0; i < r; i += 4) {
            double d0 = x[i] - mx, d1 = x[i + 1] - mx, d2 = x[i + 2] - mx, d3 = x[i + 3] - mx;
            a0 += d0 * d0;
            a1 += d1 * d1;
            a2 += d2 * d2;
            a3 += d3 * d3;
            b0 += d0 * d0 * d0;
            b1 += d1 * d1 * d1;
            b2 += d2 * d2 * d2;
            b3 += d3 * d3 * d3;
            c0 += d0 * d0 * d0 * d0;
            c1 += d1 * d1 * d1 * d1;
            c2 += d2 * d2 * d2 * d2;
            c3 += d3 * d3 * d3 * d3;
        }
        for (; i < n; i++) {
            double d = x[i] - mx;
            a0 += d * d;
            b0 += d * d * d;
            c0 += d * d * d * d;
        }
        o->m2x = (a0 + a1) + (a2 + a3);
        o->m3x = (b0 + b1) + (b2 + b3);
        o->m4x = (c0 + c1) + (c2 + c3);
        o->m2y = 0.0;
        o->cxy = 0.0;
    }
    o->n = (double)n;
    o->sx = sx;
    o->sy = sy;
    o->mx = mx;
    o->my = my;
    o->lo = lo0;
    o->hi = hi0;
}

static void dfm_moments(const void* px, DFType tx,
    const void* py, DFType ty,
    const uint8_t* m, uint32_t n, DFMoments* o)
{
    double cnt = 0.0, sx = 0.0, sy = 0.0, lo = DYN_INFINITY, hi = -DYN_INFINITY;
    double m2x = 0.0, m3x = 0.0, m4x = 0.0, m2y = 0.0, cxy = 0.0;
    double mx = 0.0, my = 0.0, v, w, d, e;
    uint32_t i;
    double *wx = NULL, *wy = NULL;
    double *own_x = NULL, *own_y = NULL;
    DFBound wb;

    if (n) {
        if (tx == DF_F64) {
            wx = DYN_UNCONST(px);
        } else {
            wx = own_x = malloc((size_t)n * sizeof(double));
            if (wx) {
                wb.p = px;
                wb.type = tx;
                wb.n = n;
                df_widen(&wb, wx, n);
            }
        }
        if (py) {
            if (ty == DF_F64) {
                wy = DYN_UNCONST(py);
            } else {
                wy = own_y = malloc((size_t)n * sizeof(double));
                if (wy) {
                    wb.p = py;
                    wb.type = ty;
                    wb.n = n;
                    df_widen(&wb, wy, n);
                }
            }
            if (!wy) {
                free(own_x);
                wx = own_x = NULL;
            }
        }
    }

#define DFM_X(i) (wx ? wx[i] : df_get(px, tx, i))
#define DFM_Y(i) (wy ? wy[i] : df_get(py, ty, i))

    if (!m && wx && n >= DFM_UNROLL_MIN && (!py || wy)) {
        dfm_unrolled(wx, wy, n, o);
        free(own_x);
        free(own_y);
        return;
    }

    if (py) {
        for (i = 0; i < n; i++) {
            if (m && !m[i])
                continue;
            v = DFM_X(i);
            w = DFM_Y(i);
            cnt += 1.0;
            sx += v;
            sy += w;
            if (v < lo)
                lo = v;
            if (v > hi)
                hi = v;
        }
    } else {
        for (i = 0; i < n; i++) {
            if (m && !m[i])
                continue;
            v = DFM_X(i);
            cnt += 1.0;
            sx += v;
            if (v < lo)
                lo = v;
            if (v > hi)
                hi = v;
        }
    }

    if (cnt > 0.0) {
        mx = sx / cnt;
        my = sy / cnt;
        if (py) {
            m3x = DYN_NAN;
            m4x = DYN_NAN;
            for (i = 0; i < n; i++) {
                if (m && !m[i])
                    continue;
                d = DFM_X(i) - mx;
                e = DFM_Y(i) - my;
                m2x += d * d;
                m2y += e * e;
                cxy += d * e;
            }
        } else {
            for (i = 0; i < n; i++) {
                if (m && !m[i])
                    continue;
                d = DFM_X(i) - mx;
                m2x += d * d;
                m3x += d * d * d;
                m4x += d * d * d * d;
            }
        }
    } else if (py) {
        m3x = DYN_NAN;
        m4x = DYN_NAN;
    }
#undef DFM_X
#undef DFM_Y
    free(own_x);
    free(own_y);

    o->n = cnt;
    o->sx = sx;
    o->sy = sy;
    o->mx = mx;
    o->my = my;
    o->m2x = m2x;
    o->m3x = m3x;
    o->m4x = m4x;
    o->m2y = m2y;
    o->cxy = cxy;
    o->lo = lo;
    o->hi = hi;
}

static double dfm_var_pop(const DFMoments* o)
{
    return o->n >= 1.0 ? o->m2x / o->n : DYN_NAN;
}

static double dfm_skew(const DFMoments* o)
{
    double m2, m3;
    if (!(o->m2x > 0.0))
        return DYN_NAN;
    m2 = o->m2x / o->n;
    m3 = o->m3x / o->n;
    return m3 / (m2 * sqrt(m2));
}

static double dfm_kurt(const DFMoments* o)
{
    double m2, m4;
    if (!(o->m2x > 0.0))
        return DYN_NAN;
    m2 = o->m2x / o->n;
    m4 = o->m4x / o->n;
    return m4 / (m2 * m2) - 3.0;
}

static double dfm_corr(const DFMoments* o)
{
    double r;
    if (!(o->m2x > 0.0) || !(o->m2y > 0.0))
        return DYN_NAN;
    r = o->cxy / (sqrt(o->m2x) * sqrt(o->m2y));
    if (r > 1.0)
        r = 1.0;
    if (r < -1.0)
        r = -1.0;
    return r;
}

static JSValue dyn_df_moments1(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    DataFrame* df;
    DFBound b;
    DFMoments mo;
    const uint8_t* mask;
    int idx, ok;
    double r;

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
    if (b.type == DF_STR)
        return JS_ThrowTypeError(ctx, "cannot reduce a string column");

    dfm_moments(b.p, b.type, NULL, DF_F64, mask, b.n, &mo);
    switch (magic) {
    case DFM_STDPOP:
        r = sqrt(dfm_var_pop(&mo));
        break;
    case DFM_SKEW:
        r = dfm_skew(&mo);
        break;
    case DFM_KURT:
        r = dfm_kurt(&mo);
        break;
    default:
        r = dfm_var_pop(&mo);
        break;
    }
    return JS_NewFloat64(ctx, r);
}

static JSValue dyn_df_moments2(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    static const char* const names[] = {
        "COV_POP", "COV_SAMP", "CORR", "REGR_SLOPE", "REGR_INTERCEPT", "REGR_R2",
        "REGR_AVG_X", "REGR_AVG_Y"
    };
    DataFrame* df;
    DFBound ba, bb;
    const DFBound *bx, *by;
    DFMoments mo;
    const uint8_t* mask;
    int ia, ib, ok, swap;
    uint32_t n;
    double r, rr;

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
    if (ba.type == DF_STR || bb.type == DF_STR) {
        int bad = ba.type == DF_STR ? ia : ib;
        return JS_ThrowTypeError(ctx, "%s: column '%s' is %s; cannot reduce a "
                                      "string column",
            names[magic],
            df->cols[bad].name, df_type_name(DF_STR));
    }

    switch (magic) {
    case DFM_SLOPE:
    case DFM_INTERCEPT:
    case DFM_R2:
    case DFM_AVGX:
    case DFM_AVGY:
        swap = 1;
        break;
    default:
        swap = 0;
        break;
    }
    bx = swap ? &bb : &ba;
    by = swap ? &ba : &bb;

    n = bx->n < by->n ? bx->n : by->n;
    dfm_moments(bx->p, bx->type, by->p, by->type, mask, n, &mo);

    switch (magic) {
    case DFM_COVSAMP:
        r = mo.n >= 2.0 ? mo.cxy / (mo.n - 1.0) : DYN_NAN;
        break;
    case DFM_CORR:
        r = dfm_corr(&mo);
        break;
    case DFM_SLOPE:
        r = mo.m2x > 0.0 ? mo.cxy / mo.m2x : DYN_NAN;
        break;
    case DFM_INTERCEPT:
        r = mo.m2x > 0.0 ? mo.my - (mo.cxy / mo.m2x) * mo.mx : DYN_NAN;
        break;
    case DFM_R2:
        rr = dfm_corr(&mo);
        r = rr * rr;
        if (r > 1.0)
            r = 1.0;
        break;
    case DFM_AVGX:
        r = mo.n >= 1.0 ? mo.mx : DYN_NAN;
        break;
    case DFM_AVGY:
        r = mo.n >= 1.0 ? mo.my : DYN_NAN;
        break;
    default:
        r = mo.n >= 1.0 ? mo.cxy / mo.n : DYN_NAN;
        break;
    }
    return JS_NewFloat64(ctx, r);
}

static JSValue dyn_df_mean_weighted(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DataFrame* df;
    DFBound bv, bw;
    const uint8_t* mask;
    int iv, iw, ok;
    uint32_t i, n;
    double sw = 0.0, swx = 0.0, v, w;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    iv = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (iv < 0)
        return JS_EXCEPTION;
    iw = df_col_arg(ctx, df, argc > 1 ? argv[1] : JS_UNDEFINED);
    if (iw < 0)
        return JS_EXCEPTION;
    mask = df_mask_arg(ctx, argc > 2 ? argv[2] : JS_UNDEFINED, df->nrows, &ok);
    if (!ok)
        return JS_EXCEPTION;

    if (dyn_df_bind(ctx, df, iv, &bv) || dyn_df_bind(ctx, df, iw, &bw))
        return JS_EXCEPTION;
    if (bv.type == DF_STR || bw.type == DF_STR) {
        int bad = bv.type == DF_STR ? iv : iw;
        return JS_ThrowTypeError(ctx, "MEAN_WEIGHTED: column '%s' is %s; cannot "
                                      "reduce a string column",
            df->cols[bad].name,
            df_type_name(DF_STR));
    }

    n = bv.n < bw.n ? bv.n : bw.n;
    for (i = 0; i < n; i++) {
        if (mask && !mask[i])
            continue;
        w = df_get(bw.p, bw.type, i);
        if (w == 0.0)
            continue;
        v = df_get(bv.p, bv.type, i);
        sw += w;
        swx += w * v;
    }
    return JS_NewFloat64(ctx, sw == 0.0 ? DYN_NAN : swx / sw);
}

static int dfm_set(JSContext* ctx, JSValue obj, const char* key, JSValue v)
{
    if (JS_IsException(v))
        return -1;
    return JS_DefinePropertyValueStr(ctx, obj, key, v, JS_PROP_C_W_E);
}

static JSValue dyn_df_describe(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DataFrame* df;
    DFBound b;
    DFMoments mo;
    const uint8_t* mask;
    JSValue res;
    int idx, ok;
    double var;

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
    if (b.type == DF_STR)
        return JS_ThrowTypeError(ctx, "cannot reduce a string column");

    dfm_moments(b.p, b.type, NULL, DF_F64, mask, b.n, &mo);
    res = JS_NewObject(ctx);
    if (JS_IsException(res))
        return res;
    var = mo.n >= 2.0 ? mo.m2x / (mo.n - 1.0) : DYN_NAN;

    if (dfm_set(ctx, res, "count", JS_NewInt64(ctx, (int64_t)mo.n)) < 0 || dfm_set(ctx, res, "sum", JS_NewFloat64(ctx, mo.sx)) < 0 || dfm_set(ctx, res, "mean", JS_NewFloat64(ctx, mo.n >= 1.0 ? mo.mx : DYN_NAN)) < 0 || dfm_set(ctx, res, "min", mo.n >= 1.0 ? JS_NewFloat64(ctx, mo.lo) : JS_UNDEFINED) < 0 || dfm_set(ctx, res, "max", mo.n >= 1.0 ? JS_NewFloat64(ctx, mo.hi) : JS_UNDEFINED) < 0 || dfm_set(ctx, res, "variance", JS_NewFloat64(ctx, var)) < 0 || dfm_set(ctx, res, "stddev", JS_NewFloat64(ctx, sqrt(var))) < 0 || dfm_set(ctx, res, "skew", JS_NewFloat64(ctx, dfm_skew(&mo))) < 0 || dfm_set(ctx, res, "kurtosis", JS_NewFloat64(ctx, dfm_kurt(&mo))) < 0) {
        JS_FreeValue(ctx, res);
        return JS_EXCEPTION;
    }
    return res;
}

enum { DFL_BOOL_AND,
    DFL_BOOL_OR,
    DFL_BOOL_XOR };

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

#define DFL_TC_FLOAT(sfx, cty, tag) DFL_DEFINE_TRUE_COUNT(sfx, cty, DFL_TRUTHY_FLOAT)
#define DFL_TC_INT(sfx, cty, tag) DFL_DEFINE_TRUE_COUNT(sfx, cty, DFL_TRUTHY_INT)
DF_FLOAT_TYPES(DFL_TC_FLOAT)
DF_INT_TYPES(DFL_TC_INT)
#undef DFL_TC_FLOAT
#undef DFL_TC_INT

static JSValue dyn_df_bool_reduce(JSContext* ctx, JSValueConst this_val,
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

#define DFL_SINT_TYPES(X)   \
    X(i32, int32_t, DF_I32) \
    X(i16, int16_t, DF_I16) \
    X(i8, int8_t, DF_I8)
#define DFL_UINT_TYPES(X)    \
    X(u32, uint32_t, DF_U32) \
    X(u16, uint16_t, DF_U16) \
    X(u8, uint8_t, DF_U8)

#define DFL_COUNT_ONE(sfx, cty, tag) +1
_Static_assert((0 DFL_SINT_TYPES(DFL_COUNT_ONE) DFL_UINT_TYPES(DFL_COUNT_ONE))
        == (0 DF_INT_TYPES(DFL_COUNT_ONE)),
    "DFL_SINT_TYPES + DFL_UINT_TYPES must cover DF_INT_TYPES");
#undef DFL_COUNT_ONE

#define DFL_DEFINE_ISUM(sfx, cty, acct)                                          \
    static acct dfl_isum_##sfx(const cty* restrict x, const uint8_t* restrict m, \
        uint32_t n)                                                              \
    {                                                                            \
        acct s = 0;                                                              \
        uint32_t i;                                                              \
        if (m) {                                                                 \
            for (i = 0; i < n; i++) {                                            \
                acct v = (acct)x[i];                                             \
                s += m[i] ? v : (acct)0;                                         \
            }                                                                    \
        } else {                                                                 \
            for (i = 0; i < n; i++)                                              \
                s += (acct)x[i];                                                 \
        }                                                                        \
        return s;                                                                \
    }

#define DFL_ISUM_S(sfx, cty, tag) DFL_DEFINE_ISUM(sfx, cty, int64_t)
#define DFL_ISUM_U(sfx, cty, tag) DFL_DEFINE_ISUM(sfx, cty, uint64_t)
DFL_SINT_TYPES(DFL_ISUM_S)
DFL_UINT_TYPES(DFL_ISUM_U)
#undef DFL_ISUM_S
#undef DFL_ISUM_U

static int dfl_i64_exact_as_double(int64_t s, double d)
{
    if (!(d >= -9223372036854775808.0 && d < 9223372036854775808.0))
        return 0;
    return (int64_t)d == s;
}

static int dfl_u64_exact_as_double(uint64_t s, double d)
{
    if (!(d >= 0.0 && d < 18446744073709551616.0))
        return 0;
    return (uint64_t)d == s;
}

static JSValue dyn_df_sumChecked(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DataFrame* df;
    DFBound b;
    const uint8_t* mask;
    int idx, ok, is_signed = 0;
    int64_t si = 0;
    uint64_t ui = 0;
    double d = 0;

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

    switch ((int)(b.type)) {
#define X(sfx, cty, tag)                                 \
    case tag:                                            \
        si = dfl_isum_##sfx((const cty*)b.p, mask, b.n); \
        is_signed = 1;                                   \
        break;
        DFL_SINT_TYPES(X)
#undef X
#define X(sfx, cty, tag)                                 \
    case tag:                                            \
        ui = dfl_isum_##sfx((const cty*)b.p, mask, b.n); \
        break;
        DFL_UINT_TYPES(X)
#undef X
    default:
        return JS_ThrowTypeError(ctx,
            "SUM_CHECKED: column '%s' is %s; a checked sum is defined only on "
            "integer columns (Int32/Uint32/Int16/Uint16/Int8/Uint8 Array) -- "
            "use sum() for a floating-point column",
            df->cols[idx].name, df_type_name(b.type));
    }

    if (is_signed) {
        d = (double)si;
        if (!dfl_i64_exact_as_double(si, d)) {
            char nb[32];
            df_fmt_double(nb, sizeof nb, d);
            return JS_ThrowRangeError(ctx,
                "SUM_CHECKED: column '%s' totals %lld, which a Number cannot "
                "hold exactly; sum() returns %s",
                df->cols[idx].name, (long long)si, nb);
        }
    } else {
        d = (double)ui;
        if (!dfl_u64_exact_as_double(ui, d)) {
            char nb[32];
            df_fmt_double(nb, sizeof nb, d);
            return JS_ThrowRangeError(ctx,
                "SUM_CHECKED: column '%s' totals %llu, which a Number cannot "
                "hold exactly; sum() returns %s",
                df->cols[idx].name, (unsigned long long)ui, nb);
        }
    }
    return JS_NewFloat64(ctx, d);
}

static const char* dfg_agg_verb(int magic)
{
    switch (magic) {
    case DF_MIN:
        return "take the min of";
    case DF_MAX:
        return "take the max of";
    default:
        return "take the mean of";
    }
}

#define DFG_SCATTER(LOAD, STEP)                                  \
    do {                                                         \
        for (i = 0; i < n; i++) {                                \
            double v;                                            \
            if (mask && !mask[i])                                \
                continue;                                        \
            g = gk ? gk[i] : (uint32_t)df_get(kb.p, kb.type, i); \
            if (g >= ngroups)                                    \
                continue;                                        \
            v = (LOAD);                                          \
            STEP;                                                \
            cnt[g]++;                                            \
        }                                                        \
    } while (0)
#define DFG_SCATTER_BY_TYPE(STEP)                        \
    do {                                                 \
        if (vb.type == DF_F64) {                         \
            const double* x = vb.p;                      \
            DFG_SCATTER(x[i], STEP);                     \
        } else {                                         \
            DFG_SCATTER(df_get(vb.p, vb.type, i), STEP); \
        }                                                \
    } while (0)

static JSValue dyn_df_group_agg(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    DataFrame* df;
    DFBound kb, vb;
    const uint8_t* mask;
    int ki, vi = -1, ok, wants_value = (magic != DF_COUNT);
    uint32_t i, ngroups, nkeys, g;
    double* acc = NULL;
    uint32_t *cnt = NULL, *gk = NULL;
    JSValue keys = JS_UNDEFINED, vals = JS_UNDEFINED, res = JS_UNDEFINED;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    ki = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (ki < 0)
        return JS_EXCEPTION;
    if (wants_value) {
        vi = df_col_arg(ctx, df, argc > 1 ? argv[1] : JS_UNDEFINED);
        if (vi < 0)
            return JS_EXCEPTION;
    } else if (argc > 1 && JS_IsString(argv[1])) {
        return JS_ThrowTypeError(ctx, "GROUP_BY_COUNT(keyCol[, mask]) takes no "
                                      "value column");
    }
    mask = df_mask_arg(ctx, argc > (wants_value ? 2 : 1) ? argv[wants_value ? 2 : 1] : JS_UNDEFINED,
        df->nrows, &ok);
    if (!ok)
        return JS_EXCEPTION;

    if (dfc_group_count(ctx, df, ki, &nkeys, &ngroups))
        return JS_EXCEPTION;

    acc = calloc(ngroups, sizeof(double));
    cnt = calloc(ngroups, sizeof(uint32_t));
    if (!acc || !cnt) {
        JS_ThrowOutOfMemory(ctx);
        goto fail;
    }
    if (magic == DF_MIN || magic == DF_MAX) {
        double seed = (magic == DF_MIN) ? DYN_INFINITY : -DYN_INFINITY;
        for (i = 0; i < ngroups; i++)
            acc[i] = seed;
    }

    if (dyn_df_bind(ctx, df, ki, &kb))
        goto fail;
    if (wants_value) {
        if (dyn_df_bind(ctx, df, vi, &vb))
            goto fail;
        if (vb.type == DF_STR) {
            JS_ThrowTypeError(ctx, "cannot %s a string column",
                dfg_agg_verb(magic));
            goto fail;
        }
    }

    {
        uint32_t n = kb.n;
        if (wants_value && vb.n < n)
            n = vb.n;
        gk = df_keys_u32(&kb, n);
        switch (magic) {
        case DF_COUNT:
            for (i = 0; i < n; i++) {
                if (mask && !mask[i])
                    continue;
                g = gk ? gk[i] : (uint32_t)df_get(kb.p, kb.type, i);
                if (g < ngroups)
                    cnt[g]++;
            }
            break;
        case DF_MIN:
            DFG_SCATTER_BY_TYPE(if (v < acc[g]) acc[g] = v);
            break;
        case DF_MAX:
            DFG_SCATTER_BY_TYPE(if (v > acc[g]) acc[g] = v);
            break;
        default:
            DFG_SCATTER_BY_TYPE(acc[g] += v);
            break;
        }
        free(gk);
        gk = NULL;
    }

    if (magic == DF_COUNT) {
        for (i = 0; i < nkeys; i++)
            acc[i] = (double)cnt[i];
    } else if (magic == DF_MEAN) {
        for (i = 0; i < nkeys; i++)
            acc[i] = cnt[i] ? acc[i] / cnt[i] : DYN_NAN;
    } else {
        for (i = 0; i < nkeys; i++)
            if (!cnt[i])
                acc[i] = DYN_NAN;
    }
    free(cnt);
    cnt = NULL;

    vals = df_to_typed_array(ctx, acc, (size_t)nkeys * sizeof(double),
        JS_TYPED_ARRAY_FLOAT64);
    acc = NULL;
    if (JS_IsException(vals))
        goto fail;

    keys = JS_NewArray(ctx);
    if (JS_IsException(keys))
        goto fail;
    for (i = 0; i < nkeys; i++) {
        JSValue k;
        if (df->cols[ki].type == DF_STR)
            k = JS_NewString(ctx, df->cols[ki].dict[i]);
        else
            k = JS_NewInt64(ctx, i);
        if (JS_IsException(k) || JS_DefinePropertyValueUint32(ctx, keys, i, k, JS_PROP_C_W_E) < 0)
            goto fail;
    }

    res = JS_NewObject(ctx);
    if (JS_IsException(res))
        goto fail;
    if (JS_DefinePropertyValueStr(ctx, res, "keys", keys, JS_PROP_C_W_E) < 0) {
        keys = JS_UNDEFINED;
        goto fail;
    }
    keys = JS_UNDEFINED;
    if (JS_DefinePropertyValueStr(ctx, res, "values", vals, JS_PROP_C_W_E) < 0) {
        vals = JS_UNDEFINED;
        goto fail;
    }
    return res;

fail:
    free(acc);
    free(cnt);
    JS_FreeValue(ctx, keys);
    JS_FreeValue(ctx, vals);
    JS_FreeValue(ctx, res);
    return JS_EXCEPTION;
}
#undef DFG_SCATTER_BY_TYPE
#undef DFG_SCATTER

static const char* dfg_roll_name(int magic)
{
    switch (magic) {
    case DF_MIN:
        return "ROLLING_MIN";
    case DF_MAX:
        return "ROLLING_MAX";
    case DF_MEAN:
        return "ROLLING_MEAN";
    default:
        return "ROLLING_SUM";
    }
}

#define DFG_ROLL(INIT, STEP, FINISH)                     \
    do {                                                 \
        for (i = 0; i < span; i++) {                     \
            double a = (INIT);                           \
            uint32_t c = 0, j;                           \
            if (i < w - 1) {                             \
                dst[i] = DYN_NAN;                        \
                continue;                                \
            }                                            \
            for (j = i - (w - 1); j <= i; j++) {         \
                double v;                                \
                if (mask && !mask[j])                    \
                    continue;                            \
                v = fx ? fx[j] : df_get(b.p, b.type, j); \
                STEP;                                    \
                c++;                                     \
            }                                            \
            dst[i] = (FINISH);                           \
        }                                                \
    } while (0)

static inline double dfg_pick(uint32_t sel, double v)
{
    uint64_t b, m;

    memcpy(&b, &v, sizeof(b));
    m = (uint64_t)0 - (uint64_t)(sel != 0);
    b = (b & m) | (0ULL & ~m);
    memcpy(&v, &b, sizeof(v));
    return v;
}

static int dfg_roll_extreme(const DFBound* b, const uint8_t* mask, double* dst,
    uint32_t span, uint32_t w, int want_min)
{
    uint32_t* dq;
    double *wv, *own;
    uint32_t head = 0, tail = 0, i;

    dq = malloc((size_t)span * sizeof(*dq) + 1);
    if (!dq)
        return -1;
    if (b->type == DF_F64) {
        wv = DYN_UNCONST(b->p);
        own = NULL;
    } else {
        wv = own = malloc((size_t)span * sizeof(*wv) + 1);
        if (!wv) {
            free(dq);
            return -1;
        }
        df_widen(b, wv, span);
    }

    for (i = 0; i < span; i++) {
        double v;
        while (head < tail && dq[head] + w <= i)
            head++;
        if (!(mask && !mask[i])) {
            v = wv[i];
            if (!(v != v)) {
                while (head < tail && (want_min ? (wv[dq[tail - 1]] >= v) : (wv[dq[tail - 1]] <= v)))
                    tail--;
                dq[tail++] = i;
            }
        }
        dst[i] = i < w - 1 ? DYN_NAN
            : head < tail  ? wv[dq[head]]
                           : DYN_NAN;
    }
    free(own);
    free(dq);
    return 0;
}

#define DFG_ROLL_UNROLL 8u

static int dfg_roll_sum(const DFBound* b, double* dst, uint32_t span,
    uint32_t w, int want_mean)
{
    const double* x;
    double* own = NULL;
    uint32_t i;

    if (w < DFG_ROLL_UNROLL || span < w)
        return -1;
    if (b->type == DF_F64) {
        x = b->p;
    } else {
        x = own = malloc((size_t)span * sizeof(double) + 1);
        if (!own)
            return -1;
        df_widen(b, own, span);
    }
    if (w >= DFG_ROLL_SLIDE_MIN) {
        double* suf = malloc((size_t)w * sizeof(double));
        if (suf) {
            uint32_t s;
            for (i = 0; i + 1 < w; i++)
                dst[i] = DYN_NAN;
            for (s = 0; s < span; s += w) {
                uint32_t end = (s + w < span) ? s + w : span;
                uint32_t k, lo, hi;
                double run = 0, pre = 0;
                for (k = end; k-- > s;) {
                    run += x[k];
                    suf[k - s] = run;
                }
                lo = s + w - 1;
                hi = s + 2 * w - 2;
                if (hi >= span)
                    hi = span - 1;
                for (k = lo; k <= hi; k++) {
                    uint32_t st = k + 1 - w;
                    double v;
                    if (k >= end)
                        pre += x[k];
                    v = (st < end ? suf[st - s] : 0.0) + pre;
                    dst[k] = want_mean ? v / (double)w : v;
                }
            }
            free(suf);
            free(own);
            return 0;
        }
    }
    for (i = 0; i + 1 < w; i++)
        dst[i] = DYN_NAN;
    for (i = w - 1; i < span; i++) {
        const double* p = x + (i - (w - 1));
        double a0 = 0, a1 = 0, a2 = 0, a3 = 0, a4 = 0, a5 = 0, a6 = 0, a7 = 0;
        uint32_t j, m = w & ~7u;
        for (j = 0; j < m; j += 8) {
            a0 += p[j];
            a1 += p[j + 1];
            a2 += p[j + 2];
            a3 += p[j + 3];
            a4 += p[j + 4];
            a5 += p[j + 5];
            a6 += p[j + 6];
            a7 += p[j + 7];
        }
        for (; j < w; j++)
            a0 += p[j];
        a0 = ((a0 + a1) + (a2 + a3)) + ((a4 + a5) + (a6 + a7));
        dst[i] = want_mean ? a0 / (double)w : a0;
    }
    free(own);
    return 0;
}

static int dfg_roll_sum_masked(const DFBound* b, const uint8_t* mask,
    double* dst, uint32_t span, uint32_t w,
    int want_mean)
{
    const double* x;
    double *own = NULL, *suf;
    uint32_t i, s, sel = 0;

    if (w < DFG_ROLL_SLIDE_MIN || span < w)
        return -1;
    if (b->type == DF_F64) {
        x = b->p;
    } else {
        x = own = malloc((size_t)span * sizeof(double) + 1);
        if (!own)
            return -1;
        df_widen(b, own, span);
    }
    suf = malloc((size_t)w * sizeof(double));
    if (!suf) {
        free(own);
        return -1;
    }
    for (i = 0; i + 1 < w; i++)
        dst[i] = DYN_NAN;

    for (s = 0; s < span; s += w) {
        uint32_t end = (s + w < span) ? s + w : span;
        uint32_t k, lo, hi, t;
        double run = 0.0, pre = 0.0;
        for (k = end; k-- > s;) {
            run += dfg_pick((uint32_t)(mask[k] != 0), x[k]);
            suf[k - s] = run;
        }
        lo = s + w - 1;
        hi = s + 2 * w - 2;
        if (hi >= span)
            hi = span - 1;
        for (k = lo; k <= hi; k++) {
            uint32_t st = k + 1 - w;
            double v;
            if (k >= w) {
                sel += (uint32_t)(mask[k] != 0) - (uint32_t)(mask[k - w] != 0);
            } else {
                for (t = 0; t <= k; t++)
                    sel += (uint32_t)(mask[t] != 0);
            }
            if (k >= end)
                pre += dfg_pick((uint32_t)(mask[k] != 0), x[k]);
            v = (st < end ? suf[st - s] : 0.0) + pre;
            dst[k] = want_mean ? (sel ? v / (double)sel : DYN_NAN) : v;
        }
    }
    free(suf);
    free(own);
    return 0;
}

static int dfg_roll_sum_sel(const DFBound* b, const uint8_t* mask, double* dst,
    uint32_t span, uint32_t w, int want_mean)
{
    if (!mask)
        return dfg_roll_sum(b, dst, span, w, want_mean);
    return dfg_roll_sum_masked(b, mask, dst, span, w, want_mean);
}

static JSValue dyn_df_rolling(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    DataFrame* df;
    DFBound b;
    const uint8_t* mask;
    const double* fx;
    double wd, *dst;
    uint32_t i, n, span, w;
    int idx, ok, slot, out_idx = -1;
    JSValueConst maskv = JS_UNDEFINED;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    idx = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (idx < 0)
        return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &wd, argc > 1 ? argv[1] : JS_UNDEFINED))
        return JS_EXCEPTION;
    for (slot = 2; slot < argc; slot++) {
        JSValueConst v = argv[slot];
        if (JS_IsUndefined(v) || JS_IsNull(v))
            continue;
        if (out_idx < 0 && df_is_out_bag(ctx, v)) {
            if (df_out_opt(ctx, df, v, dfg_roll_name(magic), &out_idx) < 0)
                return JS_EXCEPTION;
            continue;
        }
        if (JS_IsUndefined(maskv))
            maskv = v;
    }
    mask = df_mask_arg(ctx, maskv, df->nrows, &ok);
    if (!ok)
        return JS_EXCEPTION;

    if (!(wd >= 1) || wd != floor(wd) || wd > (double)UINT32_MAX)
        return JS_ThrowRangeError(ctx, "%s: window must be a positive integer, "
                                       "got %g",
            dfg_roll_name(magic), wd);
    w = (uint32_t)wd;

    n = df->nrows;
    dst = df_out_alloc(ctx, n, sizeof(double));
    if (!dst)
        return JS_EXCEPTION;
    if (df_bind_numeric(ctx, df, idx, &b, dfg_roll_name(magic))) {
        free(dst);
        return JS_EXCEPTION;
    }
    span = df_map_span(n, b.n, dst);

    fx = (b.type == DF_F64) ? b.p : NULL;
    switch (magic) {
    case DF_MIN:
        if (dfg_roll_extreme(&b, mask, dst, span, w, 1) == 0)
            break;
        DFG_ROLL(DYN_NAN, if (v == v && (a != a || v < a)) a = v, c ? a : DYN_NAN);
        break;
    case DF_MAX:
        if (dfg_roll_extreme(&b, mask, dst, span, w, 0) == 0)
            break;
        DFG_ROLL(DYN_NAN, if (v == v && (a != a || v > a)) a = v, c ? a : DYN_NAN);
        break;
    case DF_MEAN:
        if (dfg_roll_sum_sel(&b, mask, dst, span, w, 1) == 0)
            break;
        DFG_ROLL(0.0, a += v, c ? a / c : DYN_NAN);
        break;
    default:
        if (dfg_roll_sum_sel(&b, mask, dst, span, w, 0) == 0)
            break;
        DFG_ROLL(0.0, a += v, c ? a : 0.0);
        break;
    }
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
#undef DFG_ROLL

static JSValue dyn_df_dropna(JSContext* ctx, JSValueConst this_val,
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

#include "core/dyn-ds.h"
#include "core/dyn-hash.h"

#define DFA_HLL_PRECISION 14

#define DFA_TD_COMPRESSION 100.0
#define DFA_TD_MAIN_CAP 400
#define DFA_TD_BUF_CAP 1024
#define DFA_TD_MERGE_CAP (DFA_TD_MAIN_CAP + DFA_TD_BUF_CAP)

#define DFA_SS_COUNTERS_PER_K 8u
#define DFA_SS_MIN_COUNTERS 256u
#define DFA_SS_MAX_COUNTERS 8192u
#define DFA_TOPK_MAX_K 1024

#define DFA_KMV_K 256u

static uint64_t dfa_canon_bits(double v)
{
    uint64_t b;
    if (v != v)
        return 0x7FF8000000000000ULL;
    if (v == 0.0)
        v = 0.0;
    memcpy(&b, &v, sizeof(b));
    return b;
}

static double dfa_bits_value(uint64_t b)
{
    double v;
    memcpy(&v, &b, sizeof(v));
    return v;
}

static uint64_t dfa_hash_bits(uint64_t bits)
{
    return dyn_mix64(bits);
}

static uint64_t dfa_row_hash(const DFColumn* c, const DFBound* b, uint32_t i)
{
    if (b->type == DF_STR) {
        uint32_t code = (uint32_t)((const int32_t*)b->p)[i];
        const char* s;
        if (code >= c->dict_len)
            return 0;
        s = c->dict[code];
        return dyn_xxh64((const uint8_t*)s, strlen(s), 0);
    }
    return dfa_hash_bits(dfa_canon_bits(df_get(b->p, b->type, i)));
}

static int dfa_total_order(double a, double b)
{
    int na = (a != a), nb = (b != b);
    if (na || nb)
        return na - nb;
    if (a < b)
        return -1;
    if (a > b)
        return 1;
    return 0;
}

static JSValue dfa_result_pair(JSContext* ctx, JSValue keys, double* vals,
    uint32_t n)
{
    JSValue values, res;

    if (JS_IsException(keys)) {
        free(vals);
        return JS_EXCEPTION;
    }
    values = df_to_typed_array(ctx, vals, (size_t)n * sizeof(double),
        JS_TYPED_ARRAY_FLOAT64);
    if (JS_IsException(values)) {
        JS_FreeValue(ctx, keys);
        return JS_EXCEPTION;
    }
    res = JS_NewObject(ctx);
    if (JS_IsException(res)) {
        JS_FreeValue(ctx, keys);
        JS_FreeValue(ctx, values);
        return JS_EXCEPTION;
    }
    if (JS_DefinePropertyValueStr(ctx, res, "keys", keys, JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, values);
        JS_FreeValue(ctx, res);
        return JS_EXCEPTION;
    }
    if (JS_DefinePropertyValueStr(ctx, res, "values", values,
            JS_PROP_C_W_E)
        < 0) {
        JS_FreeValue(ctx, res);
        return JS_EXCEPTION;
    }
    return res;
}

static JSValue dfa_exact_distinct_str(JSContext* ctx, const DFColumn* c,
    const DFBound* b, const uint8_t* mask)
{
    const int32_t* codes = b->p;
    uint32_t *seen, i, nwords, distinct = 0;

    nwords = (c->dict_len + 31) / 32;
    seen = calloc(nwords ? nwords : 1, sizeof(uint32_t));
    if (!seen)
        return JS_ThrowOutOfMemory(ctx);
    for (i = 0; i < b->n; i++) {
        uint32_t code, w, bit;
        if (mask && !mask[i])
            continue;
        code = (uint32_t)codes[i];
        if (code >= c->dict_len)
            continue;
        w = code >> 5;
        bit = 1u << (code & 31);
        if (!(seen[w] & bit)) {
            seen[w] |= bit;
            distinct++;
        }
    }
    free(seen);
    return JS_NewInt64(ctx, distinct);
}

static JSValue dyn_df_approxCountDistinct(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DataFrame* df;
    DFBound b;
    const uint8_t* mask;
    dyn_hll_t* hll;
    int idx, ok;
    uint32_t i, any = 0;
    double est;

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
    if (b.type == DF_STR)
        return dfa_exact_distinct_str(ctx, &df->cols[idx], &b, mask);

    hll = dyn_hll_new(DFA_HLL_PRECISION);
    if (!hll)
        return JS_ThrowOutOfMemory(ctx);
    for (i = 0; i < b.n; i++) {
        if (mask && !mask[i])
            continue;
        dyn_hll_add_hash(hll,
            dfa_hash_bits(dfa_canon_bits(df_get(b.p, b.type, i))));
        any = 1;
    }
    est = any ? dyn_hll_count(hll) : 0.0;
    dyn_hll_free(hll);
    return JS_NewFloat64(ctx, est);
}

#define DFA_TD_HALF_PI 1.5707963267948966
#define DFA_TD_NORM (DFA_TD_COMPRESSION / (2.0 * 3.14159265358979323846))

typedef struct {
    double mean, weight;
} dfa_centroid;

typedef struct {
    uint32_t nmain, nbuf;
    double total, vmin, vmax;
    dfa_centroid main[DFA_TD_MAIN_CAP];
    dfa_centroid merge[DFA_TD_MERGE_CAP];
    dfa_centroid buf[DFA_TD_BUF_CAP];
} dfa_tdigest;

static int dfa_cmp_centroid(const void* pa, const void* pb)
{
    double a = ((const dfa_centroid*)pa)->mean;
    double b = ((const dfa_centroid*)pb)->mean;

    return a < b ? -1 : (a > b ? 1 : 0);
}

static double dfa_td_k(double q)
{
    if (q <= 0.0)
        return -DFA_TD_NORM * DFA_TD_HALF_PI;
    if (q >= 1.0)
        return DFA_TD_NORM * DFA_TD_HALF_PI;
    return DFA_TD_NORM * asin(2.0 * q - 1.0);
}

static double dfa_td_q(double k)
{
    double t = k / DFA_TD_NORM;
    if (t <= -DFA_TD_HALF_PI)
        return 0.0;
    if (t >= DFA_TD_HALF_PI)
        return 1.0;
    return (sin(t) + 1.0) / 2.0;
}

static double dfa_td_merge_mean(double ma, double wa, double mb, double wb)
{
    if (ma == mb)
        return ma;
    if (!isfinite(ma) || !isfinite(mb))
        return wa >= wb ? ma : mb;
    return ma + (mb - ma) * (wb / (wa + wb));
}

static void dfa_td_init(dfa_tdigest* t)
{
    t->nmain = 0;
    t->nbuf = 0;
    t->total = 0.0;
    t->vmin = DYN_INFINITY;
    t->vmax = -DYN_INFINITY;
}

static void dfa_td_flush(dfa_tdigest* t)
{
    uint32_t i, j, n = 0, out = 0;
    double w_so_far, q_limit;
    dfa_centroid cur;

    if (t->nbuf == 0)
        return;
    qsort(t->buf, t->nbuf, sizeof(dfa_centroid), dfa_cmp_centroid);

    i = 0;
    j = 0;
    while (i < t->nmain || j < t->nbuf) {
        if (j >= t->nbuf || (i < t->nmain && t->main[i].mean <= t->buf[j].mean))
            t->merge[n++] = t->main[i++];
        else
            t->merge[n++] = t->buf[j++];
    }
    t->nbuf = 0;
    if (n == 0) {
        t->nmain = 0;
        return;
    }

    w_so_far = 0.0;
    q_limit = dfa_td_q(dfa_td_k(0.0) + 1.0);
    cur = t->merge[0];
    for (i = 1; i < n; i++) {
        double proposed = (w_so_far + cur.weight + t->merge[i].weight) / t->total;
        if (proposed <= q_limit || out == DFA_TD_MAIN_CAP - 1) {
            cur.mean = dfa_td_merge_mean(cur.mean, cur.weight,
                t->merge[i].mean, t->merge[i].weight);
            cur.weight += t->merge[i].weight;
        } else {
            t->main[out++] = cur;
            w_so_far += cur.weight;
            q_limit = dfa_td_q(dfa_td_k(w_so_far / t->total) + 1.0);
            cur = t->merge[i];
        }
    }
    t->main[out++] = cur;
    t->nmain = out;
}

static void dfa_td_add_w(dfa_tdigest* t, double v, double w)
{
    if (v != v)
        return;
    if (!(w > 0.0) || w != w)
        return;
    if (v < t->vmin)
        t->vmin = v;
    if (v > t->vmax)
        t->vmax = v;
    t->total += w;
    t->buf[t->nbuf].mean = v;
    t->buf[t->nbuf].weight = w;
    t->nbuf++;
    if (t->nbuf == DFA_TD_BUF_CAP)
        dfa_td_flush(t);
}

static void dfa_td_add(dfa_tdigest* t, double v)
{
    dfa_td_add_w(t, v, 1.0);
}

static double dfa_td_lerp(double left, double right, double frac,
    double lo, double hi)
{
    double d = right - left, v;
    v = (d == 0.0 || !isfinite(d)) ? left : left + frac * d;
    if (v < lo)
        v = lo;
    if (v > hi)
        v = hi;
    return v;
}

static double dfa_td_quantile(dfa_tdigest* t, double q)
{
    uint32_t i;
    double index, w_so_far;

    dfa_td_flush(t);
    if (q <= 0.0)
        return t->vmin;
    if (q >= 1.0)
        return t->vmax;
    if (t->nmain == 1)
        return t->main[0].mean;

    index = q * t->total;
    w_so_far = 0.0;
    for (i = 0; i < t->nmain; i++) {
        double w = t->main[i].weight;
        double centre = w_so_far + w / 2.0;
        if (index <= centre) {
            double left = (i == 0) ? t->vmin : t->main[i - 1].mean;
            double cleft = (i == 0) ? 0.0
                                    : w_so_far - t->main[i - 1].weight / 2.0;
            double span = centre - cleft;
            double frac = span > 0.0 ? (index - cleft) / span : 0.0;
            return dfa_td_lerp(left, t->main[i].mean, frac, t->vmin, t->vmax);
        }
        w_so_far += w;
    }
    {
        dfa_centroid last = t->main[t->nmain - 1];
        double cleft = t->total - last.weight / 2.0;
        double span = t->total - cleft;
        double frac = span > 0.0 ? (index - cleft) / span : 1.0;
        return dfa_td_lerp(last.mean, t->vmax, frac, t->vmin, t->vmax);
    }
}

static JSValue dyn_df_approxPercentile(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DataFrame* df;
    DFBound b;
    const uint8_t* mask;
    dfa_tdigest* t;
    double q = 0, v;
    int idx, ok;
    uint32_t i;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    idx = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (idx < 0)
        return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &q, argc > 1 ? argv[1] : JS_UNDEFINED))
        return JS_EXCEPTION;
    if (!(q >= 0.0 && q <= 1.0))
        return JS_ThrowRangeError(ctx, "q must be a number in [0, 1]");
    mask = df_mask_arg(ctx, argc > 2 ? argv[2] : JS_UNDEFINED, df->nrows, &ok);
    if (!ok)
        return JS_EXCEPTION;

    if (dyn_df_bind(ctx, df, idx, &b))
        return JS_EXCEPTION;
    if (b.type == DF_STR)
        return JS_ThrowTypeError(ctx, "cannot reduce a string column");

    t = malloc(sizeof(*t));
    if (!t)
        return JS_ThrowOutOfMemory(ctx);
    dfa_td_init(t);
    for (i = 0; i < b.n; i++) {
        if (mask && !mask[i])
            continue;
        dfa_td_add(t, df_get(b.p, b.type, i));
    }
    if (t->total <= 0.0) {
        free(t);
        return JS_UNDEFINED;
    }
    v = dfa_td_quantile(t, q);
    free(t);
    return JS_NewFloat64(ctx, v);
}

typedef struct {
    uint64_t bits;
    uint64_t count;
    uint32_t islot;
} dfa_ss_counter;

typedef struct {
    dfa_ss_counter* c;
    uint32_t* idx;
    uint32_t m, n, islots, used;
} dfa_ss;

#define DFA_SS_EMPTY 0xFFFFFFFFu
#define DFA_SS_DEAD 0xFFFFFFFEu

static void dfa_ss_free(dfa_ss* s)
{
    free(s->c);
    free(s->idx);
    s->c = NULL;
    s->idx = NULL;
}

static int dfa_ss_init(dfa_ss* s, uint32_t m)
{
    s->m = m;
    s->n = 0;
    s->used = 0;
    s->islots = 4 * m;
    s->c = malloc((size_t)m * sizeof(*s->c));
    s->idx = malloc((size_t)s->islots * sizeof(uint32_t));
    if (!s->c || !s->idx) {
        dfa_ss_free(s);
        return -1;
    }
    memset(s->idx, 0xFF, (size_t)s->islots * sizeof(uint32_t));
    return 0;
}

static uint32_t dfa_ss_slot(const dfa_ss* s, uint64_t bits, uint64_t h,
    int* found)
{
    uint32_t mask = s->islots - 1;
    uint32_t p = (uint32_t)(h & mask), dead = DFA_SS_EMPTY;

    *found = 0;
    for (;;) {
        uint32_t e = s->idx[p];
        if (e == DFA_SS_EMPTY)
            return dead == DFA_SS_EMPTY ? p : dead;
        if (e == DFA_SS_DEAD) {
            if (dead == DFA_SS_EMPTY)
                dead = p;
        } else if (s->c[e].bits == bits) {
            *found = 1;
            return p;
        }
        p = (p + 1) & mask;
    }
}

static void dfa_ss_reindex(dfa_ss* s)
{
    uint32_t mask = s->islots - 1, i, p;

    memset(s->idx, 0xFF, (size_t)s->islots * sizeof(uint32_t));
    for (i = 0; i < s->n; i++) {
        p = (uint32_t)(dfa_hash_bits(s->c[i].bits) & mask);
        while (s->idx[p] != DFA_SS_EMPTY)
            p = (p + 1) & mask;
        s->idx[p] = i;
        s->c[i].islot = p;
    }
    s->used = s->n;
}

static void dfa_ss_place(dfa_ss* s, uint32_t pos, const dfa_ss_counter* v)
{
    s->c[pos] = *v;
    s->idx[s->c[pos].islot] = pos;
}

static void dfa_ss_sift_up(dfa_ss* s, uint32_t pos)
{
    dfa_ss_counter v = s->c[pos];

    while (pos > 0) {
        uint32_t parent = (pos - 1) / 2;
        if (s->c[parent].count <= v.count)
            break;
        dfa_ss_place(s, pos, &s->c[parent]);
        pos = parent;
    }
    dfa_ss_place(s, pos, &v);
}

static void dfa_ss_sift_down(dfa_ss* s, uint32_t pos)
{
    dfa_ss_counter v = s->c[pos];

    for (;;) {
        uint32_t l = 2 * pos + 1, r = l + 1, sm = pos;
        uint64_t best = v.count;
        if (l < s->n && s->c[l].count < best) {
            sm = l;
            best = s->c[l].count;
        }
        if (r < s->n && s->c[r].count < best) {
            sm = r;
        }
        if (sm == pos)
            break;
        dfa_ss_place(s, pos, &s->c[sm]);
        pos = sm;
    }
    dfa_ss_place(s, pos, &v);
}

static void dfa_ss_add(dfa_ss* s, uint64_t bits)
{
    uint64_t h = dfa_hash_bits(bits);
    int found;
    uint32_t slot = dfa_ss_slot(s, bits, h, &found);

    if (found) {
        uint32_t pos = s->idx[slot];
        s->c[pos].count++;
        dfa_ss_sift_down(s, pos);
        return;
    }
    if (s->n < s->m) {
        dfa_ss_counter v;
        v.bits = bits;
        v.count = 1;
        v.islot = slot;
        if (s->idx[slot] == DFA_SS_EMPTY)
            s->used++;
        s->c[s->n] = v;
        s->idx[slot] = s->n;
        s->n++;
        dfa_ss_sift_up(s, s->n - 1);
    } else {
        s->idx[s->c[0].islot] = DFA_SS_DEAD;
        if (s->idx[slot] == DFA_SS_EMPTY)
            s->used++;
        s->c[0].bits = bits;
        s->c[0].count++;
        s->c[0].islot = slot;
        s->idx[slot] = 0;
        dfa_ss_sift_down(s, 0);
    }
    if (s->used * 2 >= s->islots)
        dfa_ss_reindex(s);
}

static int dfa_ss_cmp(const void* pa, const void* pb)
{
    const dfa_ss_counter *a = pa, *b = pb;

    if (a->count != b->count)
        return a->count > b->count ? -1 : 1;
    return dfa_total_order(dfa_bits_value(a->bits), dfa_bits_value(b->bits));
}

typedef struct {
    uint64_t count;
    uint32_t code;
} dfa_codecount;

static int dfa_cmp_codecount(const void* pa, const void* pb)
{
    const dfa_codecount *a = pa, *b = pb;

    if (a->count != b->count)
        return a->count > b->count ? -1 : 1;
    return a->code < b->code ? -1 : (a->code > b->code ? 1 : 0);
}

static JSValue dfa_exact_topk_str(JSContext* ctx, const DFColumn* c,
    const DFBound* b, const uint8_t* mask,
    uint32_t k)
{
    const int32_t* codes = b->p;
    uint64_t* counts;
    dfa_codecount* rank = NULL;
    double* vals = NULL;
    JSValue keys;
    uint32_t i, nseen = 0, nout;

    counts = calloc(c->dict_len ? c->dict_len : 1, sizeof(uint64_t));
    if (!counts)
        return JS_ThrowOutOfMemory(ctx);
    for (i = 0; i < b->n; i++) {
        uint32_t code;
        if (mask && !mask[i])
            continue;
        code = (uint32_t)codes[i];
        if (code < c->dict_len)
            counts[code]++;
    }
    rank = malloc((size_t)(c->dict_len ? c->dict_len : 1) * sizeof(*rank));
    if (!rank) {
        free(counts);
        return JS_ThrowOutOfMemory(ctx);
    }
    for (i = 0; i < c->dict_len; i++) {
        if (counts[i] == 0)
            continue;
        rank[nseen].count = counts[i];
        rank[nseen].code = i;
        nseen++;
    }
    free(counts);
    qsort(rank, nseen, sizeof(*rank), dfa_cmp_codecount);

    nout = k < nseen ? k : nseen;
    vals = malloc((size_t)(nout ? nout : 1) * sizeof(double));
    if (!vals) {
        free(rank);
        return JS_ThrowOutOfMemory(ctx);
    }
    keys = JS_NewArray(ctx);
    for (i = 0; i < nout && !JS_IsException(keys); i++) {
        JSValue key = JS_NewString(ctx, c->dict[rank[i].code]);
        vals[i] = (double)rank[i].count;
        if (JS_IsException(key) || JS_DefinePropertyValueUint32(ctx, keys, i, key, JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, keys);
            keys = JS_EXCEPTION;
        }
    }
    free(rank);
    return dfa_result_pair(ctx, keys, vals, nout);
}

static JSValue dyn_df_approxTopK(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DataFrame* df;
    DFBound b;
    dfa_ss s;
    const uint8_t* mask;
    double* vals;
    JSValue keys;
    int64_t k64 = 0;
    int idx, ok;
    uint32_t i, k, m, nout;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    idx = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (idx < 0)
        return JS_EXCEPTION;
    if (JS_ToInt64(ctx, &k64, argc > 1 ? argv[1] : JS_UNDEFINED))
        return JS_EXCEPTION;
    if (k64 < 1 || k64 > DFA_TOPK_MAX_K)
        return JS_ThrowRangeError(ctx, "k must be an integer in [1, %d]",
            DFA_TOPK_MAX_K);
    k = (uint32_t)k64;
    mask = df_mask_arg(ctx, argc > 2 ? argv[2] : JS_UNDEFINED, df->nrows, &ok);
    if (!ok)
        return JS_EXCEPTION;

    if (dyn_df_bind(ctx, df, idx, &b))
        return JS_EXCEPTION;
    if (b.type == DF_STR)
        return dfa_exact_topk_str(ctx, &df->cols[idx], &b, mask, k);

    m = k * DFA_SS_COUNTERS_PER_K;
    if (m < DFA_SS_MIN_COUNTERS)
        m = DFA_SS_MIN_COUNTERS;
    if (m > DFA_SS_MAX_COUNTERS)
        m = DFA_SS_MAX_COUNTERS;
    while (m & (m - 1))
        m += m & (uint32_t)(-(int32_t)m);
    if (dfa_ss_init(&s, m))
        return JS_ThrowOutOfMemory(ctx);

    for (i = 0; i < b.n; i++) {
        if (mask && !mask[i])
            continue;
        dfa_ss_add(&s, dfa_canon_bits(df_get(b.p, b.type, i)));
    }
    qsort(s.c, s.n, sizeof(*s.c), dfa_ss_cmp);

    nout = k < s.n ? k : s.n;
    vals = malloc((size_t)(nout ? nout : 1) * sizeof(double));
    if (!vals) {
        dfa_ss_free(&s);
        return JS_ThrowOutOfMemory(ctx);
    }
    keys = JS_NewArray(ctx);
    for (i = 0; i < nout && !JS_IsException(keys); i++) {
        JSValue key = JS_NewFloat64(ctx, dfa_bits_value(s.c[i].bits));
        vals[i] = (double)s.c[i].count;
        if (JS_IsException(key) || JS_DefinePropertyValueUint32(ctx, keys, i, key, JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, keys);
            keys = JS_EXCEPTION;
        }
    }
    dfa_ss_free(&s);
    return dfa_result_pair(ctx, keys, vals, nout);
}

typedef struct {
    uint64_t h[DFA_KMV_K];
    uint32_t n;
} dfa_kmv;

static void dfa_kmv_add(dfa_kmv* s, uint64_t h)
{
    uint32_t i, pos;

    if (s->n == DFA_KMV_K && h >= s->h[0])
        return;
    for (i = 0; i < s->n; i++)
        if (s->h[i] == h)
            return;
    if (s->n < DFA_KMV_K) {
        pos = s->n++;
        s->h[pos] = h;
        while (pos > 0 && s->h[(pos - 1) / 2] < s->h[pos]) {
            uint64_t t = s->h[pos];
            s->h[pos] = s->h[(pos - 1) / 2];
            s->h[(pos - 1) / 2] = t;
            pos = (pos - 1) / 2;
        }
        return;
    }
    s->h[0] = h;
    pos = 0;
    for (;;) {
        uint32_t l = 2 * pos + 1, r = l + 1, big = pos;
        if (l < s->n && s->h[l] > s->h[big])
            big = l;
        if (r < s->n && s->h[r] > s->h[big])
            big = r;
        if (big == pos)
            break;
        {
            uint64_t t = s->h[pos];
            s->h[pos] = s->h[big];
            s->h[big] = t;
        }
        pos = big;
    }
}

static int dfa_cmp_u64(const void* pa, const void* pb)
{
    uint64_t a = *(const uint64_t*)pa, b = *(const uint64_t*)pb;
    return a < b ? -1 : (a > b ? 1 : 0);
}

static double dfa_kmv_jaccard(dfa_kmv* a, dfa_kmv* b)
{
    uint32_t ia = 0, ib = 0, taken = 0, matches = 0;

    qsort(a->h, a->n, sizeof(uint64_t), dfa_cmp_u64);
    qsort(b->h, b->n, sizeof(uint64_t), dfa_cmp_u64);
    while (taken < DFA_KMV_K && (ia < a->n || ib < b->n)) {
        if (ib >= b->n || (ia < a->n && a->h[ia] < b->h[ib])) {
            ia++;
        } else if (ia >= a->n || b->h[ib] < a->h[ia]) {
            ib++;
        } else {
            ia++;
            ib++;
            matches++;
        }
        taken++;
    }
    if (taken == 0)
        return DYN_NAN;
    return (double)matches / (double)taken;
}

static JSValue dyn_df_approxSimilarity(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DataFrame* df;
    DFBound ba, bb;
    const uint8_t* mask;
    dfa_kmv* sk;
    int ia, ib, ok;
    uint32_t i;
    double j;

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

    sk = malloc(2 * sizeof(*sk));
    if (!sk)
        return JS_ThrowOutOfMemory(ctx);
    sk[0].n = 0;
    sk[1].n = 0;
    for (i = 0; i < ba.n; i++) {
        if (mask && !mask[i])
            continue;
        dfa_kmv_add(&sk[0], dfa_row_hash(&df->cols[ia], &ba, i));
    }
    for (i = 0; i < bb.n; i++) {
        if (mask && !mask[i])
            continue;
        dfa_kmv_add(&sk[1], dfa_row_hash(&df->cols[ib], &bb, i));
    }
    j = dfa_kmv_jaccard(&sk[0], &sk[1]);
    free(sk);
    return JS_NewFloat64(ctx, j);
}

enum { DFX_SEM,
    DFX_SKEW_SAMP,
    DFX_KURT_SAMP,
    DFX_COUNT_NULLS,
    DFX_REGR_COUNT,
    DFX_REGR_SXX,
    DFX_REGR_SYY,
    DFX_REGR_SXY };

static double dfx_skew_samp(const DFMoments* o)
{
    double n = o->n, g1;

    if (n < 3.0 || !(o->m2x > 0.0))
        return 0.0;
    g1 = (o->m3x / n) / pow(o->m2x / n, 1.5);
    return g1 * sqrt(n * (n - 1.0)) / (n - 2.0);
}

static double dfx_kurt_samp(const DFMoments* o)
{
    double n = o->n, g2;

    if (n < 4.0 || !(o->m2x > 0.0))
        return 0.0;
    g2 = (o->m4x / n) / ((o->m2x / n) * (o->m2x / n)) - 3.0;
    return ((n + 1.0) * g2 + 6.0) * (n - 1.0) / ((n - 2.0) * (n - 3.0));
}

static JSValue dyn_df_stat1(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    static const char* const nm[4] = { "SEM", "SKEW_SAMP", "KURT_SAMP",
        "COUNT_NULLS" };
    DataFrame* df;
    DFBound b;
    DFMoments mo;
    const uint8_t* mask;
    int idx, ok;
    double r;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    idx = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (idx < 0)
        return JS_EXCEPTION;
    mask = df_mask_arg(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, df->nrows, &ok);
    if (!ok)
        return JS_EXCEPTION;
    if (df_bind_numeric(ctx, df, idx, &b, nm[magic]))
        return JS_EXCEPTION;

    if (magic == DFX_COUNT_NULLS) {
        uint32_t i, span = b.n < df->nrows ? b.n : df->nrows, c = 0;
        for (i = 0; i < span; i++) {
            double v;
            if (mask && !mask[i])
                continue;
            v = df_get(b.p, b.type, i);
            c += (v != v);
        }
        return JS_NewInt64(ctx, (int64_t)c);
    }

    dfm_moments(b.p, b.type, NULL, DF_F64, mask, b.n, &mo);
    switch (magic) {
    case DFX_SKEW_SAMP:
        r = dfx_skew_samp(&mo);
        break;
    case DFX_KURT_SAMP:
        r = dfx_kurt_samp(&mo);
        break;
    default:
        r = mo.n > 1.0 ? sqrt(mo.m2x / (mo.n - 1.0)) / sqrt(mo.n) : DYN_NAN;
        break;
    }
    return JS_NewFloat64(ctx, r);
}

static JSValue dyn_df_regr_sum(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    static const char* const nm[4] = { "REGR_COUNT", "REGR_SXX", "REGR_SYY",
        "REGR_SXY" };
    DataFrame* df;
    DFBound bx, by;
    DFMoments mo;
    const uint8_t* mask;
    int ix, iy, ok;
    uint32_t n;
    double r;
    const char* op = nm[magic - DFX_REGR_COUNT];

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    ix = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (ix < 0)
        return JS_EXCEPTION;
    iy = df_col_arg(ctx, df, argc > 1 ? argv[1] : JS_UNDEFINED);
    if (iy < 0)
        return JS_EXCEPTION;
    mask = df_mask_arg(ctx, argc > 2 ? argv[2] : JS_UNDEFINED, df->nrows, &ok);
    if (!ok)
        return JS_EXCEPTION;
    if (df_bind_numeric(ctx, df, ix, &bx, op) || df_bind_numeric(ctx, df, iy, &by, op))
        return JS_EXCEPTION;

    n = bx.n < by.n ? bx.n : by.n;
    dfm_moments(bx.p, bx.type, by.p, by.type, mask, n, &mo);
    switch (magic) {
    case DFX_REGR_SXX:
        r = mo.m2y;
        break;
    case DFX_REGR_SYY:
        r = mo.m2x;
        break;
    case DFX_REGR_SXY:
        r = mo.cxy;
        break;
    default:
        r = mo.n;
        break;
    }
    return JS_NewFloat64(ctx, r);
}

enum { DFX_MAD,
    DFX_MED_AD };

static JSValue dyn_df_deviation(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    DataFrame* df;
    DFBound b;
    const uint8_t* mask;
    DfoItem* it = NULL;
    uint32_t n, m, i;
    int idx, ok;
    double centre, r;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    idx = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (idx < 0)
        return JS_EXCEPTION;
    mask = df_mask_arg(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, df->nrows, &ok);
    if (!ok)
        return JS_EXCEPTION;
    if (df_bind_numeric(ctx, df, idx, &b,
            magic == DFX_MAD ? "MAD" : "MEDIAN_ABSOLUTE_DEVIATION"))
        return JS_EXCEPTION;
    if (dfo_gathered(ctx, &b, mask, df->nrows, &it, &n, &m))
        return JS_EXCEPTION;
    if (m == 0) {
        free(it);
        return JS_UNDEFINED;
    }

    if (magic == DFX_MAD) {
        double s = 0.0;
        for (i = 0; i < n; i++)
            if (it[i].key == it[i].key)
                s += it[i].key;
        centre = s / (double)m;
        s = 0.0;
        for (i = 0; i < n; i++)
            if (it[i].key == it[i].key)
                s += fabs(it[i].key - centre);
        r = s / (double)m;
    } else {
        dfo_select(it, n, m >> 1, 0);
        centre = it[m >> 1].key;
        if ((m & 1) == 0 && m >= 2) {
            dfo_select(it, n, (m >> 1) - 1, 0);
            centre = (centre + it[(m >> 1) - 1].key) * 0.5;
        }
        for (i = 0; i < n; i++)
            it[i].key = it[i].key == it[i].key ? fabs(it[i].key - centre) : DYN_NAN;
        dfo_select(it, n, m >> 1, 0);
        r = it[m >> 1].key;
        if ((m & 1) == 0 && m >= 2) {
            dfo_select(it, n, (m >> 1) - 1, 0);
            r = (r + it[(m >> 1) - 1].key) * 0.5;
        }
    }
    free(it);
    return JS_NewFloat64(ctx, r);
}

static JSValue dyn_df_entropy(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DataFrame* df;
    DfcSet set;
    const uint8_t* mask;
    uint32_t i, total = 0;
    int idx, ok;
    double h = 0.0;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    idx = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (idx < 0)
        return JS_EXCEPTION;
    mask = df_mask_arg(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, df->nrows, &ok);
    if (!ok)
        return JS_EXCEPTION;

    memset(&set, 0, sizeof(set));
    if (dfc_scan(ctx, df, idx, mask, &set)) {
        dfc_set_free(&set);
        return JS_EXCEPTION;
    }
    for (i = 0; i < set.nent; i++)
        total += set.counts[i];
    if (total) {
        double inv = 1.0 / (double)total;
        for (i = 0; i < set.nent; i++) {
            double pr = (double)set.counts[i] * inv;
            h -= pr * log2(pr);
        }
    }
    dfc_set_free(&set);
    return JS_NewFloat64(ctx, h);
}

enum { DFX_Q_LOW,
    DFX_Q_HIGH };

static JSValue dyn_df_quantile_lh(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    static const char* const nm[2] = { "QUANTILE_EXACT_LOW",
        "QUANTILE_EXACT_HIGH" };
    DataFrame* df;
    DFBound b;
    const uint8_t* mask;
    DfoItem* it = NULL;
    double q = 0.5, pos;
    uint32_t n, m, k;

    if (dfo_open(ctx, this_val, argc, argv, 1, nm[magic], &df, &b, &q, &mask, NULL))
        return JS_EXCEPTION;
    if (isnan(q) || q < 0.0 || q > 1.0)
        return JS_ThrowRangeError(ctx, "%s(col, q): q must be in [0, 1], got %g",
            nm[magic], q);
    if (dfo_gathered(ctx, &b, mask, df->nrows, &it, &n, &m))
        return JS_EXCEPTION;
    if (m == 0) {
        free(it);
        return JS_UNDEFINED;
    }
    pos = q * (double)(m - 1);
    k = magic == DFX_Q_LOW ? (uint32_t)floor(pos) : (uint32_t)ceil(pos);
    if (k >= m)
        k = m - 1;
    dfo_select(it, n, k, 0);
    q = it[k].key;
    free(it);
    return JS_NewFloat64(ctx, q);
}

static JSValue dyn_df_quantiles(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DataFrame* df;
    DFBound b;
    const uint8_t* mask;
    DfoItem* it = NULL;
    JSValue arr, lenv;
    double *qs = NULL, *out = NULL;
    uint32_t n, m, i, nq = 0;
    int idx, ok;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    idx = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (idx < 0)
        return JS_EXCEPTION;

    arr = argc > 1 ? JS_DupValue(ctx, argv[1]) : JS_UNDEFINED;
    if (!JS_IsObject(arr)) {
        JS_FreeValue(ctx, arr);
        return JS_ThrowTypeError(ctx, "QUANTILES(col, qs): qs must be an array");
    }
    lenv = JS_GetPropertyStr(ctx, arr, "length");
    if (JS_IsException(lenv) || JS_ToUint32(ctx, &nq, lenv) < 0) {
        JS_FreeValue(ctx, lenv);
        JS_FreeValue(ctx, arr);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, lenv);
    if (nq > (1u << 20)) {
        JS_FreeValue(ctx, arr);
        return JS_ThrowRangeError(ctx, "QUANTILES: too many quantiles");
    }
    qs = df_out_alloc(ctx, nq, sizeof(double));
    if (!qs) {
        JS_FreeValue(ctx, arr);
        return JS_EXCEPTION;
    }
    for (i = 0; i < nq; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, arr, i);
        if (JS_IsException(e) || JS_ToFloat64(ctx, &qs[i], e) < 0) {
            JS_FreeValue(ctx, e);
            JS_FreeValue(ctx, arr);
            free(qs);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, e);
        if (isnan(qs[i]) || qs[i] < 0.0 || qs[i] > 1.0) {
            double bad = qs[i];
            JS_FreeValue(ctx, arr);
            free(qs);
            return JS_ThrowRangeError(ctx, "QUANTILES: q must be in [0, 1], "
                                           "got %g",
                bad);
        }
    }
    JS_FreeValue(ctx, arr);

    mask = df_mask_arg(ctx, argc > 2 ? argv[2] : JS_UNDEFINED, df->nrows, &ok);
    if (!ok || df_bind_numeric(ctx, df, idx, &b, "QUANTILES")) {
        free(qs);
        return JS_EXCEPTION;
    }
    if (dfo_gathered(ctx, &b, mask, df->nrows, &it, &n, &m)) {
        free(qs);
        return JS_EXCEPTION;
    }
    if (m) {
        int depth = 2;
        uint32_t t = n;
        while (t >>= 1)
            depth += 2;
        dfo_sort(it, n, 0, depth);
    }
    out = df_out_alloc(ctx, nq, sizeof(double));
    if (!out) {
        free(qs);
        free(it);
        return JS_EXCEPTION;
    }
    for (i = 0; i < nq; i++) {
        if (m == 0) {
            out[i] = DYN_NAN;
        } else {
            double pos = qs[i] * (double)(m - 1), frac;
            uint32_t lo = (uint32_t)pos;
            frac = pos - (double)lo;
            out[i] = (frac == 0.0 || lo + 1 >= m)
                ? it[lo].key
                : it[lo].key + frac * (it[lo + 1].key - it[lo].key);
        }
    }
    free(qs);
    free(it);
    return df_to_typed_array(ctx, out, (size_t)nq * sizeof(double),
        JS_TYPED_ARRAY_FLOAT64);
}

static JSValue dyn_df_quantiles_td(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DataFrame* df;
    DFBound b;
    const uint8_t* mask;
    dfa_tdigest* t = NULL;
    JSValue arr, lenv;
    double *qs = NULL, *out = NULL;
    uint32_t i, span, nq = 0;
    int idx, ok;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    idx = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (idx < 0)
        return JS_EXCEPTION;

    arr = argc > 1 ? JS_DupValue(ctx, argv[1]) : JS_UNDEFINED;
    if (!JS_IsObject(arr)) {
        JS_FreeValue(ctx, arr);
        return JS_ThrowTypeError(ctx,
            "QUANTILES_TDIGEST(col, qs): qs must be an array");
    }
    lenv = JS_GetPropertyStr(ctx, arr, "length");
    if (JS_IsException(lenv) || JS_ToUint32(ctx, &nq, lenv) < 0) {
        JS_FreeValue(ctx, lenv);
        JS_FreeValue(ctx, arr);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, lenv);
    if (nq > (1u << 20)) {
        JS_FreeValue(ctx, arr);
        return JS_ThrowRangeError(ctx, "QUANTILES_TDIGEST: too many quantiles");
    }
    qs = df_out_alloc(ctx, nq, sizeof(double));
    if (!qs) {
        JS_FreeValue(ctx, arr);
        return JS_EXCEPTION;
    }
    for (i = 0; i < nq; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, arr, i);
        if (JS_IsException(e) || JS_ToFloat64(ctx, &qs[i], e) < 0) {
            JS_FreeValue(ctx, e);
            JS_FreeValue(ctx, arr);
            free(qs);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, e);
        if (isnan(qs[i]) || qs[i] < 0.0 || qs[i] > 1.0) {
            double bad = qs[i];
            JS_FreeValue(ctx, arr);
            free(qs);
            return JS_ThrowRangeError(ctx,
                "QUANTILES_TDIGEST: q must be in [0, 1], got %g", bad);
        }
    }
    JS_FreeValue(ctx, arr);

    mask = df_mask_arg(ctx, argc > 2 ? argv[2] : JS_UNDEFINED, df->nrows, &ok);
    if (!ok || df_bind_numeric(ctx, df, idx, &b, "QUANTILES_TDIGEST")) {
        free(qs);
        return JS_EXCEPTION;
    }
    t = malloc(sizeof(*t));
    if (!t) {
        free(qs);
        return JS_ThrowOutOfMemory(ctx);
    }
    dfa_td_init(t);
    span = b.n < df->nrows ? b.n : df->nrows;
    for (i = 0; i < span; i++) {
        if (mask && !mask[i])
            continue;
        dfa_td_add(t, df_get(b.p, b.type, i));
    }
    out = df_out_alloc(ctx, nq, sizeof(double));
    if (!out) {
        free(qs);
        free(t);
        return JS_EXCEPTION;
    }
    for (i = 0; i < nq; i++)
        out[i] = t->total > 0.0 ? dfa_td_quantile(t, qs[i]) : DYN_NAN;
    free(qs);
    free(t);
    return df_to_typed_array(ctx, out, (size_t)nq * sizeof(double),
        JS_TYPED_ARRAY_FLOAT64);
}

static JSValue dyn_df_uniq_up_to(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DataFrame* df;
    DFBound b;
    const uint8_t* mask;
    DfoItem* it = NULL;
    double cap = 0.0;
    uint32_t n, m, i, seen = 0, lim;

    if (dfo_open(ctx, this_val, argc, argv, 1, "UNIQ_UP_TO", &df, &b, &cap,
            &mask, NULL))
        return JS_EXCEPTION;
    if (!(cap >= 0.0) || cap != floor(cap) || cap > 65536.0)
        return JS_ThrowRangeError(ctx, "UNIQ_UP_TO(col, n): n must be an "
                                       "integer in [0, 65536], got %g",
            cap);
    lim = (uint32_t)cap;
    {
        uint32_t nslot = 1, probe;
        double* slot;
        uint8_t* used;
        uint32_t span = b.n < df->nrows ? b.n : df->nrows;
        while (nslot < (lim + 1) * 2u)
            nslot <<= 1;
        slot = malloc((size_t)nslot * sizeof(*slot));
        used = calloc(nslot, 1);
        if (!slot || !used) {
            free(slot);
            free(used);
            return JS_ThrowOutOfMemory(ctx);
        }
        int saw_nan = 0;
        for (i = 0; i < span && seen <= lim; i++) {
            double v;
            uint64_t bits;
            if (mask && !mask[i])
                continue;
            v = df_get(b.p, b.type, i);
            if (v != v) {
                if (!saw_nan) {
                    saw_nan = 1;
                    seen++;
                }
                continue;
            }
            if (v == 0.0)
                v = 0.0;
            memcpy(&bits, &v, sizeof(bits));
            bits ^= bits >> 33;
            bits *= 0xff51afd7ed558ccdULL;
            bits ^= bits >> 33;
            probe = (uint32_t)bits & (nslot - 1);
            while (used[probe] && slot[probe] != v)
                probe = (probe + 1) & (nslot - 1);
            if (!used[probe]) {
                used[probe] = 1;
                slot[probe] = v;
                seen++;
            }
        }
        free(slot);
        free(used);
    }
    (void)it;
    (void)n;
    (void)m;
    return JS_NewInt64(ctx, (int64_t)seen);
}

static JSValue dyn_df_histogram(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    DataFrame* df;
    DFBound b;
    const uint8_t* mask;
    double nb = 0.0, lo = DYN_INFINITY, hi = -DYN_INFINITY, w;
    double *edges = NULL, *counts = NULL;
    uint32_t nbins, i, span, kept = 0;
    JSValue res, ev, cv;

    if (dfo_open(ctx, this_val, argc, argv, 1,
            magic ? "HISTOGRAM_NORMALIZED" : "HISTOGRAM",
            &df, &b, &nb, &mask, NULL))
        return JS_EXCEPTION;
    if (!(nb >= 1.0) || nb != floor(nb) || nb > 1048576.0)
        return JS_ThrowRangeError(ctx, "%s(col, bins): bins must be a positive "
                                       "integer, got %g",
            magic ? "HISTOGRAM_NORMALIZED" : "HISTOGRAM",
            nb);
    nbins = (uint32_t)nb;
    span = b.n < df->nrows ? b.n : df->nrows;

    for (i = 0; i < span; i++) {
        double v;
        if (mask && !mask[i])
            continue;
        v = df_get(b.p, b.type, i);
        if (v != v || !isfinite(v))
            continue;
        kept++;
        if (v < lo)
            lo = v;
        if (v > hi)
            hi = v;
    }
    edges = df_out_alloc(ctx, nbins + 1, sizeof(double));
    counts = edges ? df_out_alloc(ctx, nbins, sizeof(double)) : NULL;
    if (!counts) {
        free(edges);
        return JS_EXCEPTION;
    }
    for (i = 0; i < nbins; i++)
        counts[i] = 0.0;
    if (kept == 0) {
        lo = 0.0;
        hi = 1.0;
    } else if (hi <= lo) {
        hi = lo + 1.0;
    }
    w = (hi - lo) / (double)nbins;
    if (!isfinite(w) || w <= 0.0) {
        free(edges);
        free(counts);
        return JS_ThrowRangeError(ctx, "HISTOGRAM: the value range is too wide "
                                       "for finite edges");
    }
    for (i = 0; i <= nbins; i++)
        edges[i] = lo + w * (double)i;

    for (i = 0; i < span; i++) {
        double v;
        uint32_t k;
        if (mask && !mask[i])
            continue;
        v = df_get(b.p, b.type, i);
        if (v != v || !isfinite(v))
            continue;
        {
            double r = (v - lo) / w;
            if (!(r >= 0.0))
                k = 0;
            else if (r >= (double)nbins)
                k = nbins - 1;
            else
                k = (uint32_t)r;
        }
        counts[k] += 1.0;
    }
    if (magic && kept)
        for (i = 0; i < nbins; i++)
            counts[i] /= (double)kept;

    res = JS_NewObject(ctx);
    if (JS_IsException(res)) {
        free(edges);
        free(counts);
        return res;
    }
    ev = df_to_typed_array(ctx, edges, (size_t)(nbins + 1) * sizeof(double),
        JS_TYPED_ARRAY_FLOAT64);
    cv = df_to_typed_array(ctx, counts, (size_t)nbins * sizeof(double),
        JS_TYPED_ARRAY_FLOAT64);
    if (JS_IsException(ev) || JS_IsException(cv) || JS_DefinePropertyValueStr(ctx, res, "edges", ev, JS_PROP_C_W_E) < 0 || JS_DefinePropertyValueStr(ctx, res, "counts", cv, JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, res);
        return JS_EXCEPTION;
    }
    return res;
}

static JSValue dyn_df_ema(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DataFrame* df;
    DFBound b;
    const uint8_t* mask;
    double alpha = 0.0, acc = 0.0, *dst;
    uint32_t i, n, span;
    int seeded = 0, out_idx = -1;

    if (dfo_open(ctx, this_val, argc, argv, 1, "EMA", &df, &b, &alpha, &mask, &out_idx))
        return JS_EXCEPTION;
    if (!(alpha > 0.0) || !(alpha <= 1.0) || isnan(alpha))
        return JS_ThrowRangeError(ctx, "EMA(col, alpha): alpha must be in "
                                       "(0, 1], got %g",
            alpha);
    n = df->nrows;
    dst = df_out_alloc(ctx, n, sizeof(double));
    if (!dst)
        return JS_EXCEPTION;
    span = df_map_span(n, b.n, dst);
    for (i = 0; i < span; i++) {
        double v;
        if (mask && !mask[i]) {
            dst[i] = seeded ? acc : DYN_NAN;
            continue;
        }
        v = df_get(b.p, b.type, i);
        if (!seeded) {
            acc = v;
            seeded = 1;
        } else
            acc = alpha * v + (1.0 - alpha) * acc;
        dst[i] = acc;
    }
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

static JSValue dyn_df_delta_sum(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DataFrame* df;
    DFBound b;
    const uint8_t* mask;
    uint32_t i, span;
    int idx, ok, have = 0;
    double prev = 0.0, s = 0.0;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    idx = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (idx < 0)
        return JS_EXCEPTION;
    mask = df_mask_arg(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, df->nrows, &ok);
    if (!ok)
        return JS_EXCEPTION;
    if (df_bind_numeric(ctx, df, idx, &b, "DELTA_SUM"))
        return JS_EXCEPTION;
    span = b.n < df->nrows ? b.n : df->nrows;
    for (i = 0; i < span; i++) {
        double v;
        if (mask && !mask[i])
            continue;
        v = df_get(b.p, b.type, i);
        if (have && v > prev)
            s += v - prev;
        prev = v;
        have = 1;
    }
    return JS_NewFloat64(ctx, s);
}

enum { DFX_RATE,
    DFX_IRATE };

static JSValue dyn_df_rate(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    const char* op = magic == DFX_IRATE ? "IRATE" : "RATE";
    DataFrame* df;
    DFBound bv, bt;
    const uint8_t* mask;
    uint32_t i, span;
    int iv, it_, ok, have = 0;
    double v0 = 0, t0 = 0, v1 = 0, t1 = 0, pv = 0, pt = 0;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    iv = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (iv < 0)
        return JS_EXCEPTION;
    it_ = df_col_arg(ctx, df, argc > 1 ? argv[1] : JS_UNDEFINED);
    if (it_ < 0)
        return JS_EXCEPTION;
    mask = df_mask_arg(ctx, argc > 2 ? argv[2] : JS_UNDEFINED, df->nrows, &ok);
    if (!ok)
        return JS_EXCEPTION;
    if (df_bind_numeric(ctx, df, iv, &bv, op) || df_bind_numeric(ctx, df, it_, &bt, op))
        return JS_EXCEPTION;

    span = bv.n < bt.n ? bv.n : bt.n;
    if (span > df->nrows)
        span = df->nrows;
    for (i = 0; i < span; i++) {
        double v, t;
        if (mask && !mask[i])
            continue;
        v = df_get(bv.p, bv.type, i);
        t = df_get(bt.p, bt.type, i);
        if (!have) {
            v0 = v;
            t0 = t;
            have = 1;
        }
        pv = v1;
        pt = t1;
        v1 = v;
        t1 = t;
        have++;
    }
    if (have < 3)
        return JS_NewFloat64(ctx, DYN_NAN);
    if (magic == DFX_IRATE)
        return JS_NewFloat64(ctx, (v1 - pv) / (t1 - pt));
    return JS_NewFloat64(ctx, (v1 - v0) / (t1 - t0));
}

enum { DFY_BIT_AND,
    DFY_BIT_OR,
    DFY_BIT_XOR };

static JSValue dyn_df_group_bit(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    static const char* const nm[3] = { "GROUP_BIT_AND", "GROUP_BIT_OR",
        "GROUP_BIT_XOR" };
    DataFrame* df;
    DFBound kb, vb;
    const uint8_t* mask;
    uint32_t *acc = NULL, *cnt = NULL, *gk = NULL;
    double* out = NULL;
    int ki, vi, ok;
    uint32_t i, g, n, nkeys, ngroups;
    JSValue keys = JS_UNDEFINED, vals = JS_UNDEFINED, res;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    ki = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (ki < 0)
        return JS_EXCEPTION;
    vi = df_col_arg(ctx, df, argc > 1 ? argv[1] : JS_UNDEFINED);
    if (vi < 0)
        return JS_EXCEPTION;
    mask = df_mask_arg(ctx, argc > 2 ? argv[2] : JS_UNDEFINED, df->nrows, &ok);
    if (!ok)
        return JS_EXCEPTION;
    if (dfc_group_count(ctx, df, ki, &nkeys, &ngroups))
        return JS_EXCEPTION;
    if (dyn_df_bind(ctx, df, ki, &kb) || dyn_df_bind(ctx, df, vi, &vb))
        return JS_EXCEPTION;
    if (vb.type == DF_STR || vb.type == DF_F64 || vb.type == DF_F32)
        return JS_ThrowTypeError(ctx, "%s: column is %s; a bitwise fold is "
                                      "defined only on integer columns",
            nm[magic],
            df_type_name(vb.type));

    acc = calloc(ngroups, sizeof(*acc));
    cnt = calloc(ngroups, sizeof(*cnt));
    if (!acc || !cnt) {
        free(acc);
        free(cnt);
        return JS_ThrowOutOfMemory(ctx);
    }
    if (magic == DFY_BIT_AND)
        for (i = 0; i < ngroups; i++)
            acc[i] = ~0u;

    n = kb.n < vb.n ? kb.n : vb.n;
    gk = df_keys_u32(&kb, n);
    for (i = 0; i < n; i++) {
        uint32_t v;
        if (mask && !mask[i])
            continue;
        g = gk ? gk[i] : (uint32_t)df_get(kb.p, kb.type, i);
        if (g >= ngroups)
            continue;
        v = (uint32_t)(int64_t)df_get(vb.p, vb.type, i);
        if (magic == DFY_BIT_AND)
            acc[g] &= v;
        else if (magic == DFY_BIT_OR)
            acc[g] |= v;
        else
            acc[g] ^= v;
        cnt[g]++;
    }
    free(gk);

    out = df_out_alloc(ctx, nkeys, sizeof(double));
    if (!out) {
        free(acc);
        free(cnt);
        return JS_EXCEPTION;
    }
    for (i = 0; i < nkeys; i++)
        out[i] = (double)acc[i];
    free(acc);
    free(cnt);

    keys = JS_NewArray(ctx);
    if (JS_IsException(keys)) {
        free(out);
        return JS_EXCEPTION;
    }
    for (i = 0; i < nkeys; i++) {
        JSValue k = (df->cols[ki].type == DF_STR)
            ? JS_NewString(ctx, df->cols[ki].dict[i])
            : JS_NewInt64(ctx, i);
        if (JS_IsException(k) || JS_DefinePropertyValueUint32(ctx, keys, i, k, JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, keys);
            free(out);
            return JS_EXCEPTION;
        }
    }
    vals = df_to_typed_array(ctx, out, (size_t)nkeys * sizeof(double),
        JS_TYPED_ARRAY_FLOAT64);
    if (JS_IsException(vals)) {
        JS_FreeValue(ctx, keys);
        return JS_EXCEPTION;
    }
    (void)res;
    return dfc_pair(ctx, keys, vals);
}

static JSValue dyn_df_corr_matrix(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    DataFrame* df;
    const char* op = magic ? "COV_MATRIX" : "CORR_MATRIX";
    const uint8_t* mask;
    JSValue arr = JS_UNDEFINED, lenv, cols = JS_UNDEFINED, mv, res;
    int *idx = NULL, ok;
    uint32_t* first = NULL;
    double* m = NULL;
    uint32_t nc = 0, i, j;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;

    if (argc > 0 && JS_IsObject(argv[0])) {
        arr = JS_DupValue(ctx, argv[0]);
        lenv = JS_GetPropertyStr(ctx, arr, "length");
        if (JS_IsException(lenv) || JS_ToUint32(ctx, &nc, lenv) < 0) {
            JS_FreeValue(ctx, lenv);
            JS_FreeValue(ctx, arr);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, lenv);
    } else {
        return JS_ThrowTypeError(ctx, "%s(cols): cols must be an array "
                                      "of column names",
            op);
    }
    if (nc == 0 || nc > DF_MAX_COLS) {
        JS_FreeValue(ctx, arr);
        return JS_ThrowRangeError(ctx, "%s: between 1 and %d columns", op,
            DF_MAX_COLS);
    }
    idx = df_out_alloc(ctx, nc, sizeof(int));
    if (!idx) {
        JS_FreeValue(ctx, arr);
        return JS_EXCEPTION;
    }
    for (i = 0; i < nc; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, arr, i);
        idx[i] = JS_IsException(e) ? -1 : df_col_arg(ctx, df, e);
        JS_FreeValue(ctx, e);
        if (idx[i] < 0) {
            JS_FreeValue(ctx, arr);
            free(idx);
            return JS_EXCEPTION;
        }
    }
    mask = df_mask_arg(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, df->nrows, &ok);
    if (!ok) {
        JS_FreeValue(ctx, arr);
        free(idx);
        return JS_EXCEPTION;
    }

    m = df_out_alloc(ctx, nc * nc, sizeof(double));
    first = df_out_alloc(ctx, nc, sizeof(uint32_t));
    if (!m || !first) {
        JS_FreeValue(ctx, arr);
        free(idx);
        free(m);
        free(first);
        return JS_EXCEPTION;
    }
    for (i = 0; i < nc; i++) {
        first[i] = i;
        for (j = 0; j < i; j++)
            if (idx[j] == idx[i]) {
                first[i] = j;
                break;
            }
    }

    for (i = 0; i < nc; i++) {
        for (j = i; j < nc; j++) {
            if (first[i] != i || first[j] != j) {
                double dup = m[first[i] * nc + first[j]];
                m[i * nc + j] = dup;
                m[j * nc + i] = dup;
                continue;
            }
            DFBound bx, by;
            DFMoments mo;
            double r;
            uint32_t n;
            if (df_bind_numeric(ctx, df, idx[i], &bx, op) || df_bind_numeric(ctx, df, idx[j], &by, op)) {
                JS_FreeValue(ctx, arr);
                free(idx);
                free(m);
                free(first);
                return JS_EXCEPTION;
            }
            n = bx.n < by.n ? bx.n : by.n;
            dfm_moments(bx.p, bx.type, by.p, by.type, mask, n, &mo);
            r = magic      ? (mo.n >= 2.0 ? mo.cxy / (mo.n - 1.0) : DYN_NAN)
                : (i == j) ? (mo.m2x > 0.0 ? 1.0 : DYN_NAN)
                           : dfm_corr(&mo);
            m[i * nc + j] = r;
            m[j * nc + i] = r;
        }
    }

    cols = JS_NewArray(ctx);
    for (i = 0; i < nc && !JS_IsException(cols); i++) {
        JSValue s = JS_NewString(ctx, df->cols[idx[i]].name);
        if (JS_IsException(s) || JS_DefinePropertyValueUint32(ctx, cols, i, s, JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, cols);
            cols = JS_EXCEPTION;
        }
    }
    JS_FreeValue(ctx, arr);
    free(idx);
    free(first);
    mv = df_to_typed_array(ctx, m, (size_t)nc * nc * sizeof(double),
        JS_TYPED_ARRAY_FLOAT64);
    res = JS_NewObject(ctx);
    if (JS_IsException(cols) || JS_IsException(mv) || JS_IsException(res)) {
        JS_FreeValue(ctx, cols);
        JS_FreeValue(ctx, mv);
        JS_FreeValue(ctx, res);
        return JS_EXCEPTION;
    }
    if (JS_DefinePropertyValueStr(ctx, res, "columns", cols, JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, res);
        JS_FreeValue(ctx, mv);
        return JS_EXCEPTION;
    }
    if (JS_DefinePropertyValueStr(ctx, res, "matrix", mv, JS_PROP_C_W_E) < 0 || JS_DefinePropertyValueStr(ctx, res, "n", JS_NewInt64(ctx, nc), JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, res);
        return JS_EXCEPTION;
    }
    return res;
}

enum { DFX_ROLL_VAR,
    DFX_ROLL_STD };

#define DFG_ROLL_VAR_WORK_MAX 100000000.0

#define DFG_ROLL_POLL_WORK 4194304u

#define DFG_ROLL_VAR_TOL 1e-10

#define DFG_ROLL_VAR_CADENCE 131072u

#define DFG_ROLL_VAR_XPASS 16u

static uint32_t dfg_roll_win2(const double* x, const uint8_t* mask,
    uint32_t lo, uint32_t hi, double* pm, double* pq,
    double* ps)
{
    double mean = 0.0, m2 = 0.0, dev = 0.0, sum = 0.0, err = 0.0;
    uint32_t c = 0, j;

    for (j = lo; j <= hi; j++) {
        double t, xv;
        if (mask && !mask[j])
            continue;
        xv = x[j];
        t = sum + xv;
        if (fabs(sum) >= fabs(xv))
            err += (sum - t) + xv;
        else
            err += (xv - t) + sum;
        sum = t;
        c++;
    }
    if (c == 0) {
        *pm = 0.0;
        *pq = 0.0;
        *ps = 0.0;
        return 0;
    }
    mean = (sum + err) / (double)c;
    for (j = lo; j <= hi; j++) {
        double d;
        if (mask && !mask[j])
            continue;
        d = x[j] - mean;
        m2 += d * d;
        dev += d;
    }
    *pm = mean;
    *pq = m2;
    *ps = dev;
    return c;
}

#define DFG_ROLL_VAR_SLIDE(SLIDED)                                         \
    do {                                                                   \
        for (i = w; i < span; i++) {                                       \
            uint32_t mi, mo;                                               \
            double din, dout, a, m2, bnd, t2;                              \
            if (SLIDED) {                                                  \
                mi = (uint32_t)(mask[i] != 0);                             \
                mo = (uint32_t)(mask[i - w] != 0);                         \
            } else {                                                       \
                mi = 1;                                                    \
                mo = 1;                                                    \
            }                                                              \
            if (mi && !isfinite(x[i]))                                     \
                nf++;                                                      \
            if (mo && !isfinite(x[i - w])) {                               \
                nf--;                                                      \
                if (nf == 0) {                                             \
                    c = dfg_roll_win2(x, mask, i + 1 - w, i, &K, &Q, &S);  \
                    eq = 0.0;                                              \
                    es = 0.0;                                              \
                    t = 0;                                                 \
                    dst[i] = c < 2 ? DYN_NAN                               \
                                   : (want_std ? sqrt(Q / (double)(c - 1)) \
                                               : Q / (double)(c - 1));     \
                    continue;                                              \
                }                                                          \
            }                                                              \
            if (nf != 0) {                                                 \
                dst[i] = DYN_NAN;                                          \
                continue;                                                  \
            }                                                              \
            din = dfg_pick(mi, x[i] - K);                                  \
            dout = dfg_pick(mo, x[i - w] - K);                             \
            S += din - dout;                                               \
            Q += din * din - dout * dout;                                  \
            c += (int32_t)mi - (int32_t)mo;                                \
            if (c < 2) {                                                   \
                dst[i] = DYN_NAN;                                          \
                continue;                                                  \
            }                                                              \
            a = S / (double)c;                                             \
            m2 = Q - S * a;                                                \
            t2 = din * din;                                                \
            if (dout * dout > t2)                                          \
                t2 = dout * dout;                                          \
                                                                           \
            eq += 4.0 * DBL_EPSILON * (Q > t2 ? Q : t2);                   \
            t2 = fabs(din) > fabs(dout) ? fabs(din) : fabs(dout);          \
            es += 2.0 * DBL_EPSILON * (fabs(S) > t2 ? fabs(S) : t2);       \
            bnd = eq + 2.0 * fabs(S) * es / (double)c                      \
                + 2.0 * DBL_EPSILON * (Q > fabs(S * a) ? Q : fabs(S * a)); \
            t++;                                                           \
            if (t >= cadence || !(m2 >= 0.0)                               \
                || !(bnd <= DFG_ROLL_VAR_TOL * m2)) {                      \
                if (rc * (uint64_t)w * 2u                                  \
                    > (uint64_t)DFG_ROLL_VAR_XPASS * span)                 \
                    goto decline;                                          \
                rc++;                                                      \
                c = dfg_roll_win2(x, mask, i + 1 - w, i, &K, &Q, &S);      \
                eq = 0.0;                                                  \
                t = 0;                                                     \
                es = 0.0;                                                  \
                m2 = Q;                                                    \
            }                                                              \
            dst[i] = want_std ? sqrt(m2 / (double)(c - 1))                 \
                              : m2 / (double)(c - 1);                      \
        }                                                                  \
    } while (0)

static int dfg_roll_var_slide(const DFBound* b, const uint8_t* mask, double* dst,
    uint32_t span, uint32_t w, int want_std)
{
    const double* x;
    double* own = NULL;
    double K = 0.0, S = 0.0, Q = 0.0, eq = 0.0, es = 0.0;
    uint32_t i, c = 0, rc = 0, t = 0, cadence, nf = 0;

    if (w < 2 || span < w)
        return -1;
    if ((double)span * (double)w <= DFG_ROLL_VAR_WORK_MAX)
        return -1;
    if (b->type == DF_F64) {
        x = b->p;
    } else {
        x = own = malloc((size_t)span * sizeof(double) + 1);
        if (!own)
            return -1;
        df_widen(b, own, span);
    }

    cadence = w < DFG_ROLL_VAR_CADENCE ? DFG_ROLL_VAR_CADENCE : w;
    for (i = 0; i + 1 < w; i++)
        dst[i] = DYN_NAN;
    for (i = 0; i < w; i++)
        if ((!mask || mask[i]) && !isfinite(x[i]))
            nf++;
    c = dfg_roll_win2(x, mask, 0, w - 1, &K, &Q, &S);
    if (c < 2 || nf != 0) {
        dst[w - 1] = DYN_NAN;
    } else {
        dst[w - 1] = want_std ? sqrt(Q / (double)(c - 1))
                              : Q / (double)(c - 1);
    }
    if (mask)
        DFG_ROLL_VAR_SLIDE(1);
    else
        DFG_ROLL_VAR_SLIDE(0);
    free(own);
    return 0;
decline:
    free(own);
    return -1;
}

#undef DFG_ROLL_VAR_SLIDE

static JSValue dyn_df_rolling_disp(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    DataFrame* df;
    DFBound b;
    const uint8_t* mask;
    const char* op = magic == DFX_ROLL_STD ? "ROLLING_STD" : "ROLLING_VAR";
    double wd = 0.0, *dst;
    uint32_t i, n, span, w;
    uint64_t poll_work = 0;
    int out_idx = -1;

    if (dfo_open(ctx, this_val, argc, argv, 1, op, &df, &b, &wd, &mask, &out_idx))
        return JS_EXCEPTION;
    if (!(wd >= 1) || wd != floor(wd) || wd > (double)UINT32_MAX)
        return JS_ThrowRangeError(ctx, "%s: window must be a positive integer, "
                                       "got %g",
            op, wd);
    w = (uint32_t)wd;
    n = df->nrows;
    dst = df_out_alloc(ctx, n, sizeof(double));
    if (!dst)
        return JS_EXCEPTION;
    span = df_map_span(n, b.n, dst);

    if (dfg_roll_var_slide(&b, mask, dst, span, w, magic == DFX_ROLL_STD) == 0) {
        if (out_idx < 0)
            return df_to_typed_array(ctx, dst, (size_t)n * sizeof(double),
                JS_TYPED_ARRAY_FLOAT64);
        if (df_store_out(ctx, df, out_idx, dst, n)) {
            free(dst);
            return JS_EXCEPTION;
        }
        free(dst);
        return JS_DupValue(ctx, this_val);
    }

    for (i = 0; i < span; i++) {
        double mean = 0.0, m2 = 0.0;
        uint32_t c = 0, j, lo;
        if (i + 1 < w) {
            dst[i] = DYN_NAN;
            continue;
        }
        poll_work += w;
        if (poll_work >= DFG_ROLL_POLL_WORK) {
            poll_work = 0;
            if (JS_CheckInterrupt(ctx)) {
                free(dst);
                return JS_EXCEPTION;
            }
        }
        lo = i + 1 - w;
        for (j = lo; j <= i; j++) {
            if (mask && !mask[j])
                continue;
            mean += df_get(b.p, b.type, j);
            c++;
        }
        if (c < 2) {
            dst[i] = DYN_NAN;
            continue;
        }
        mean /= (double)c;
        for (j = lo; j <= i; j++) {
            double d;
            if (mask && !mask[j])
                continue;
            d = df_get(b.p, b.type, j) - mean;
            m2 += d * d;
        }
        m2 /= (double)(c - 1);
        dst[i] = magic == DFX_ROLL_STD ? sqrt(m2) : m2;
    }
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

static JSValue dyn_df_pct_change(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DataFrame* df;
    DFBound b;
    const uint8_t* mask;
    double pd = 1.0, *dst;
    uint32_t i, n, span, p;
    int out_idx = -1;

    if (dfo_open(ctx, this_val, argc, argv, 1, "PCT_CHANGE", &df, &b, &pd, &mask, &out_idx))
        return JS_EXCEPTION;
    if (argc < 2 || JS_IsUndefined(argv[1]) || (argc > 1 && df_is_out_bag(ctx, argv[1])))
        pd = 1.0;
    if (!(pd >= 1) || pd != floor(pd) || pd > (double)UINT32_MAX)
        return JS_ThrowRangeError(ctx, "PCT_CHANGE: periods must be a positive "
                                       "integer, got %g",
            pd);
    p = (uint32_t)pd;
    n = df->nrows;
    dst = df_out_alloc(ctx, n, sizeof(double));
    if (!dst)
        return JS_EXCEPTION;
    span = df_map_span(n, b.n, dst);

    for (i = 0; i < span; i++) {
        double prev, cur;
        if (i < p || (mask && (!mask[i] || !mask[i - p]))) {
            dst[i] = DYN_NAN;
            continue;
        }
        prev = df_get(b.p, b.type, i - p);
        cur = df_get(b.p, b.type, i);
        dst[i] = (cur - prev) / prev;
    }
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

static JSValue dyn_df_zscore(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DataFrame* df;
    DFBound b;
    DFMoments mo;
    const uint8_t* mask;
    double sd, *dst;
    uint32_t i, n, span;
    int out_idx = -1;

    if (dfo_open(ctx, this_val, argc, argv, 0, "ZSCORE", &df, &b, NULL, &mask, &out_idx))
        return JS_EXCEPTION;
    n = df->nrows;
    dst = df_out_alloc(ctx, n, sizeof(double));
    if (!dst)
        return JS_EXCEPTION;
    span = df_map_span(n, b.n, dst);
    dfm_moments(b.p, b.type, NULL, DF_F64, mask, b.n, &mo);
    sd = mo.n >= 2.0 ? sqrt(mo.m2x / (mo.n - 1.0)) : DYN_NAN;

    for (i = 0; i < span; i++) {
        if (mask && !mask[i]) {
            dst[i] = DYN_NAN;
            continue;
        }
        dst[i] = (df_get(b.p, b.type, i) - mo.mx) / sd;
    }
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

enum { DFX_DENSE_RANK,
    DFX_PERCENT_RANK };

static JSValue dyn_df_rank_ext(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    DataFrame* df;
    DFBound b;
    const uint8_t* mask;
    DfoItem* it;
    double* dst;
    uint32_t nrows, n, m, i, j, dense = 0;
    const char* op = magic == DFX_PERCENT_RANK ? "PERCENT_RANK" : "DENSE_RANK";

    if (dfo_open(ctx, this_val, argc, argv, 0, op, &df, &b, NULL, &mask, NULL))
        return JS_EXCEPTION;
    nrows = df->nrows;
    if (dfo_sorted(ctx, &b, mask, nrows, 0, &it, &n))
        return JS_EXCEPTION;
    dst = df_out_alloc(ctx, nrows, sizeof(double));
    if (!dst) {
        free(it);
        return JS_EXCEPTION;
    }
    for (i = 0; i < nrows; i++)
        dst[i] = DYN_NAN;

    m = dfo_valued(it, n);
    i = 0;
    while (i < m) {
        double r;
        for (j = i + 1; j < m && it[j].key == it[i].key; j++)
            ;
        dense++;
        r = magic == DFX_PERCENT_RANK
            ? (m > 1 ? (double)i / (double)(m - 1) : 0.0)
            : (double)dense;
        for (; i < j; i++)
            dst[it[i].idx] = r;
    }
    free(it);
    return df_to_typed_array(ctx, dst, (size_t)nrows * sizeof(double),
        JS_TYPED_ARRAY_FLOAT64);
}

static JSValue dyn_df_ntile(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DataFrame* df;
    DFBound b;
    const uint8_t* mask;
    DfoItem* it;
    double kd, *dst;
    uint32_t nrows, n, m, i, k, big, small, cut;

    if (dfo_open(ctx, this_val, argc, argv, 1, "NTILE", &df, &b, &kd, &mask, NULL))
        return JS_EXCEPTION;
    if (!(kd >= 1) || kd != floor(kd) || kd > (double)UINT32_MAX)
        return JS_ThrowRangeError(ctx, "NTILE: buckets must be a positive "
                                       "integer, got %g",
            kd);
    k = (uint32_t)kd;
    nrows = df->nrows;
    if (dfo_sorted(ctx, &b, mask, nrows, 0, &it, &n))
        return JS_EXCEPTION;
    dst = df_out_alloc(ctx, nrows, sizeof(double));
    if (!dst) {
        free(it);
        return JS_EXCEPTION;
    }
    for (i = 0; i < nrows; i++)
        dst[i] = DYN_NAN;

    m = dfo_valued(it, n);
    big = m % k;
    small = m / k;
    cut = big * (small + 1);
    for (i = 0; i < m; i++) {
        uint32_t t = i < cut ? i / (small + 1)
                             : big + (small ? (i - cut) / small : 0);
        dst[it[i].idx] = (double)(t + 1);
    }
    free(it);
    return df_to_typed_array(ctx, dst, (size_t)nrows * sizeof(double),
        JS_TYPED_ARRAY_FLOAT64);
}

static JSValue dyn_df_rank_corr(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DataFrame* df;
    DFBound bx, by;
    DFMoments mo;
    const uint8_t* mask;
    uint8_t* both = NULL;
    DfoItem* it = NULL;
    double *rx = NULL, *ry = NULL, r;
    uint32_t nrows, span, n, m, i, j, pass;
    int ix, iy, ok;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    ix = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (ix < 0)
        return JS_EXCEPTION;
    iy = df_col_arg(ctx, df, argc > 1 ? argv[1] : JS_UNDEFINED);
    if (iy < 0)
        return JS_EXCEPTION;
    mask = df_mask_arg(ctx, argc > 2 ? argv[2] : JS_UNDEFINED, df->nrows, &ok);
    if (!ok)
        return JS_EXCEPTION;
    if (df_bind_numeric(ctx, df, ix, &bx, "RANK_CORR") || df_bind_numeric(ctx, df, iy, &by, "RANK_CORR"))
        return JS_EXCEPTION;

    nrows = df->nrows;
    span = bx.n < by.n ? bx.n : by.n;
    both = df_out_alloc(ctx, nrows, sizeof(uint8_t));
    rx = df_out_alloc(ctx, nrows, sizeof(double));
    ry = df_out_alloc(ctx, nrows, sizeof(double));
    if (!both || !rx || !ry)
        goto oom;
    memset(both, 0, nrows);
    for (i = 0; i < span; i++) {
        double a = df_get(bx.p, bx.type, i), c = df_get(by.p, by.type, i);
        both[i] = (!mask || mask[i]) && a == a && c == c;
    }

    for (pass = 0; pass < 2; pass++) {
        const DFBound* bp = pass ? &by : &bx;
        double* out = pass ? ry : rx;
        if (dfo_sorted(ctx, bp, both, nrows, 0, &it, &n))
            goto oom;
        m = dfo_valued(it, n);
        i = 0;
        while (i < m) {
            double v;
            for (j = i + 1; j < m && it[j].key == it[i].key; j++)
                ;
            v = ((double)(i + 1) + (double)j) * 0.5;
            for (; i < j; i++)
                out[it[i].idx] = v;
        }
        free(it);
        it = NULL;
    }

    dfm_moments(rx, DF_F64, ry, DF_F64, both, span, &mo);
    r = dfm_corr(&mo);
    free(both);
    free(rx);
    free(ry);
    return JS_NewFloat64(ctx, r);
oom:
    free(it);
    free(both);
    free(rx);
    free(ry);
    return JS_EXCEPTION;
}

static JSValue dyn_df_group_concat(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DataFrame* df;
    DFBound b;
    const uint8_t* mask;
    const char* sep = NULL;
    char* buf = NULL;
    size_t len = 0, cap = 0, seplen;
    JSValue out;
    uint32_t i, span;
    int idx, ok, first = 1;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    idx = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (idx < 0)
        return JS_EXCEPTION;
    if (argc > 1 && !JS_IsUndefined(argv[1])) {
        sep = JS_ToCString(ctx, argv[1]);
        if (!sep)
            return JS_EXCEPTION;
    }
    mask = df_mask_arg(ctx, argc > 2 ? argv[2] : JS_UNDEFINED, df->nrows, &ok);
    if (!ok || dyn_df_bind(ctx, df, idx, &b)) {
        if (sep)
            JS_FreeCString(ctx, sep);
        return JS_EXCEPTION;
    }
    seplen = sep ? strlen(sep) : 1;

    span = b.n < df->nrows ? b.n : df->nrows;
    for (i = 0; i < span; i++) {
        char num[40];
        const char* piece;
        size_t plen, need;
        if (mask && !mask[i])
            continue;
        if (b.type == DF_STR) {
            int32_t code = ((const int32_t*)b.p)[i];
            piece = (code >= 0 && (uint32_t)code < df->cols[idx].dict_len)
                ? df->cols[idx].dict[code]
                : "";
            plen = strlen(piece);
        } else {
            double d = df_get(b.p, b.type, i);
            if (d == floor(d) && d > -1e15 && d < 1e15) {
                int64_t iv = (int64_t)d;
                char tmp[24];
                int tn = 0, neg = iv < 0;
                uint64_t u = neg ? (uint64_t)(-iv) : (uint64_t)iv;
                do {
                    tmp[tn++] = (char)('0' + (u % 10));
                    u /= 10;
                } while (u);
                plen = 0;
                if (neg)
                    num[plen++] = '-';
                while (tn)
                    num[plen++] = tmp[--tn];
                piece = num;
            } else {
                df_fmt_double(num, sizeof(num), d);
                plen = strlen(num);
                piece = num;
            }
        }
        need = len + plen + (first ? 0 : seplen);
        if (need > cap) {
            char* nb;
            size_t ncap = cap ? cap * 2 : 256;
            while (ncap < need)
                ncap *= 2;
            nb = realloc(buf, ncap);
            if (!nb) {
                free(buf);
                if (sep)
                    JS_FreeCString(ctx, sep);
                return JS_ThrowOutOfMemory(ctx);
            }
            buf = nb;
            cap = ncap;
        }
        if (!first) {
            memcpy(buf + len, sep ? sep : ",", seplen);
            len += seplen;
        }
        first = 0;
        memcpy(buf + len, piece, plen);
        len += plen;
    }
    if (sep)
        JS_FreeCString(ctx, sep);
    out = JS_NewStringLen(ctx, buf ? buf : "", len);
    free(buf);
    return out;
}

static int dfz_cmp_d(const void* pa, const void* pb)
{
    double a = *(const double*)pa, b = *(const double*)pb;

    if (a != a)
        return b != b ? 0 : 1;
    if (b != b)
        return -1;
    return a < b ? -1 : (a > b ? 1 : 0);
}

enum { DFZ_SORTED,
    DFZ_LAST,
    DFZ_SAMPLE };

static JSValue dyn_df_group_array_v(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    static const char* const nm[3] = { "GROUP_ARRAY_SORTED",
        "GROUP_ARRAY_LAST",
        "GROUP_ARRAY_SAMPLE" };
    DataFrame* df;
    DFBound kb, vb;
    const uint8_t* mask;
    uint32_t *cnt = NULL, *off = NULL, *gk = NULL, *fill = NULL, *head = NULL;
    double *flat = NULL, kd = 0.0;
    int ki, vi, ok;
    uint32_t i, g, n, nkeys, ngroups, cap, total = 0;
    JSValue keys = JS_UNDEFINED, values = JS_UNDEFINED;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    ki = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (ki < 0)
        return JS_EXCEPTION;
    vi = df_col_arg(ctx, df, argc > 1 ? argv[1] : JS_UNDEFINED);
    if (vi < 0)
        return JS_EXCEPTION;
    if (magic != DFZ_SORTED) {
        if (JS_ToFloat64(ctx, &kd, argc > 2 ? argv[2] : JS_UNDEFINED))
            return JS_EXCEPTION;
        if (!(kd >= 1.0) || kd != floor(kd) || kd > 65536.0)
            return JS_ThrowRangeError(ctx, "%s(key, val, k): k must be an "
                                           "integer in [1, 65536], got %g",
                nm[magic], kd);
    }
    mask = df_mask_arg(ctx, argc > (magic == DFZ_SORTED ? 2 : 3) ? argv[magic == DFZ_SORTED ? 2 : 3] : JS_UNDEFINED,
        df->nrows, &ok);
    if (!ok)
        return JS_EXCEPTION;
    if (dfc_group_count(ctx, df, ki, &nkeys, &ngroups))
        return JS_EXCEPTION;
    if (dyn_df_bind(ctx, df, ki, &kb) || dyn_df_bind(ctx, df, vi, &vb))
        return JS_EXCEPTION;
    if (vb.type == DF_STR)
        return JS_ThrowTypeError(ctx, "%s: the value column is %s; only numeric "
                                      "columns can be collected",
            nm[magic],
            df_type_name(vb.type));
    cap = magic == DFZ_SORTED ? 0xffffffffu : (uint32_t)kd;

    cnt = calloc(ngroups, sizeof(*cnt));
    off = calloc(ngroups + 1, sizeof(*off));
    fill = calloc(ngroups, sizeof(*fill));
    head = calloc(ngroups, sizeof(*head));
    if (!cnt || !off || !fill || !head) {
        free(cnt);
        free(off);
        free(fill);
        free(head);
        return JS_ThrowOutOfMemory(ctx);
    }
    n = kb.n < vb.n ? kb.n : vb.n;
    gk = df_keys_u32(&kb, n);

    for (i = 0; i < n; i++) {
        if (mask && !mask[i])
            continue;
        g = gk ? gk[i] : (uint32_t)df_get(kb.p, kb.type, i);
        if (g >= ngroups)
            continue;
        cnt[g]++;
    }
    for (i = 0; i < ngroups; i++) {
        if (cnt[i] > cap)
            cnt[i] = cap;
        off[i + 1] = off[i] + cnt[i];
    }
    total = off[ngroups];
    flat = df_out_alloc(ctx, total ? total : 1, sizeof(double));
    if (!flat) {
        free(cnt);
        free(off);
        free(fill);
        free(head);
        free(gk);
        return JS_EXCEPTION;
    }

    for (i = 0; i < n; i++) {
        double v;
        uint32_t base, have;
        if (mask && !mask[i])
            continue;
        g = gk ? gk[i] : (uint32_t)df_get(kb.p, kb.type, i);
        if (g >= ngroups)
            continue;
        v = df_get(vb.p, vb.type, i);
        base = off[g];
        have = fill[g];
        if (magic == DFZ_LAST && have == cnt[g] && cnt[g]) {
            flat[base + head[g]] = v;
            {
                uint32_t h = head[g] + 1;
                head[g] = h == cnt[g] ? 0 : h;
            }
            continue;
        }
        if (have < cnt[g]) {
            flat[base + have] = v;
            fill[g] = have + 1;
        }
    }
    free(gk);
    if (magic == DFZ_SORTED)
        for (g = 0; g < nkeys; g++)
            if (cnt[g] > 1)
                qsort(&flat[off[g]], cnt[g], sizeof(double), dfz_cmp_d);

    values = JS_NewArray(ctx);
    if (JS_IsException(values)) {
        free(cnt);
        free(off);
        free(fill);
        free(head);
        free(flat);
        return JS_EXCEPTION;
    }
    for (g = 0; g < nkeys; g++) {
        double* chunk = df_out_alloc(ctx, cnt[g] ? cnt[g] : 1, sizeof(double));
        JSValue ta;
        if (!chunk) {
            JS_FreeValue(ctx, values);
            free(cnt);
            free(off);
            free(fill);
            free(head);
            free(flat);
            return JS_EXCEPTION;
        }
        if (magic == DFZ_LAST && head[g]) {
            uint32_t j, idx = head[g];
            for (j = 0; j < cnt[g]; j++) {
                chunk[j] = flat[off[g] + idx];
                if (++idx == cnt[g])
                    idx = 0;
            }
        } else {
            memcpy(chunk, &flat[off[g]], (size_t)cnt[g] * sizeof(double));
        }
        ta = df_to_typed_array(ctx, chunk, (size_t)cnt[g] * sizeof(double),
            JS_TYPED_ARRAY_FLOAT64);
        if (JS_IsException(ta) || JS_DefinePropertyValueUint32(ctx, values, g, ta, JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, values);
            free(cnt);
            free(off);
            free(fill);
            free(head);
            free(flat);
            return JS_EXCEPTION;
        }
    }
    free(cnt);
    free(off);
    free(fill);
    free(head);
    free(flat);

    keys = JS_NewArray(ctx);
    if (JS_IsException(keys)) {
        JS_FreeValue(ctx, values);
        return JS_EXCEPTION;
    }
    for (g = 0; g < nkeys; g++) {
        JSValue k = (df->cols[ki].type == DF_STR)
            ? JS_NewString(ctx, df->cols[ki].dict[g])
            : JS_NewInt64(ctx, g);
        if (JS_IsException(k) || JS_DefinePropertyValueUint32(ctx, keys, g, k, JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, keys);
            JS_FreeValue(ctx, values);
            return JS_EXCEPTION;
        }
    }
    return dfc_pair(ctx, keys, values);
}

typedef struct {
    double key, w;
} DfwPair;

static int dfw_cmp_w(const void* pa, const void* pb)
{
    const DfwPair *a = pa, *b = pb;

    if (a->w != b->w)
        return a->w > b->w ? -1 : 1;
    return a->key < b->key ? -1 : (a->key > b->key ? 1 : 0);
}

static void dfw_sort_k(DfwPair* p, uint32_t n)
{
    while (n > 24) {
        DfwPair pv;
        uint32_t i = 0, j = n - 1, m = n >> 1;
        if (p[m].key < p[0].key) {
            DfwPair t = p[m];
            p[m] = p[0];
            p[0] = t;
        }
        if (p[n - 1].key < p[m].key) {
            DfwPair t = p[n - 1];
            p[n - 1] = p[m];
            p[m] = t;
        }
        if (p[m].key < p[0].key) {
            DfwPair t = p[m];
            p[m] = p[0];
            p[0] = t;
        }
        pv = p[m];
        for (;;) {
            while (p[i].key < pv.key)
                i++;
            while (pv.key < p[j].key)
                j--;
            if (i >= j)
                break;
            {
                DfwPair t = p[i];
                p[i] = p[j];
                p[j] = t;
            }
            i++;
            if (j == 0)
                break;
            j--;
        }
        {
            uint32_t leftn = j + 1, rightn = n - leftn;
            if (leftn < rightn) {
                dfw_sort_k(p, leftn);
                p += leftn;
                n = rightn;
            } else {
                dfw_sort_k(p + leftn, rightn);
                n = leftn;
            }
        }
    }
    {
        uint32_t a, b;
        for (a = 1; a < n; a++) {
            DfwPair v = p[a];
            for (b = a; b > 0 && v.key < p[b - 1].key; b--)
                p[b] = p[b - 1];
            p[b] = v;
        }
    }
}

static JSValue dyn_df_delta_sum_ts(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DataFrame* df;
    DFBound bv, bt;
    const uint8_t* mask;
    DfwPair* p = NULL;
    uint32_t i, span, n = 0;
    int iv, it_, ok;
    double s = 0.0;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    iv = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (iv < 0)
        return JS_EXCEPTION;
    it_ = df_col_arg(ctx, df, argc > 1 ? argv[1] : JS_UNDEFINED);
    if (it_ < 0)
        return JS_EXCEPTION;
    mask = df_mask_arg(ctx, argc > 2 ? argv[2] : JS_UNDEFINED, df->nrows, &ok);
    if (!ok)
        return JS_EXCEPTION;
    if (df_bind_numeric(ctx, df, iv, &bv, "DELTA_SUM_TIMESTAMP") || df_bind_numeric(ctx, df, it_, &bt, "DELTA_SUM_TIMESTAMP"))
        return JS_EXCEPTION;

    span = bv.n < bt.n ? bv.n : bt.n;
    if (span > df->nrows)
        span = df->nrows;
    p = df_out_alloc(ctx, span ? span : 1, sizeof(*p));
    if (!p)
        return JS_EXCEPTION;
    for (i = 0; i < span; i++) {
        double tv;
        if (mask && !mask[i])
            continue;
        tv = df_get(bt.p, bt.type, i);
        if (tv != tv)
            continue;
        p[n].key = tv;
        p[n].w = df_get(bv.p, bv.type, i);
        n++;
    }
    dfw_sort_k(p, n);
    for (i = 1; i < n; i++)
        if (p[i].w > p[i - 1].w)
            s += p[i].w - p[i - 1].w;
    free(p);
    return JS_NewFloat64(ctx, s);
}

static DfwPair* dfw_collect(JSContext* ctx, const DFBound* b, const DFBound* w,
    const uint8_t* mask, uint32_t nrows, uint32_t* pn, const char* op)
{
    uint32_t span = b->n < nrows ? b->n : nrows, i, j, n = 0, o = 0;
    DfwPair* p;

    if (w && w->n < span)
        span = w->n;
    p = df_out_alloc(ctx, span ? span : 1, sizeof(*p));
    if (!p)
        return NULL;
    for (i = 0; i < span; i++) {
        double v, wt;
        if (mask && !mask[i])
            continue;
        v = df_get(b->p, b->type, i);
        if (v != v)
            continue;
        wt = w ? df_get(w->p, w->type, i) : 1.0;
        if (!isfinite(wt)) {
            JS_ThrowRangeError(ctx, "%s: weight column contains non-finite "
                                    "values",
                op);
            free(p);
            return NULL;
        }
        p[n].key = v;
        p[n].w = wt;
        n++;
    }
    dfw_sort_k(p, n);
    for (i = 0; i < n; i = j) {
        double s = 0.0;
        for (j = i; j < n && p[j].key == p[i].key; j++)
            s += p[j].w;
        p[o].key = p[i].key;
        p[o].w = s;
        o++;
    }
    *pn = o;
    return p;
}

enum { DFW_TOPK_W,
    DFW_APPROX_TOP_SUM,
    DFW_ANY_HEAVY };

static JSValue dyn_df_weighted_top(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    static const char* const nm[3] = { "TOP_K_WEIGHTED", "APPROX_TOP_SUM",
        "ANY_HEAVY" };
    DataFrame* df;
    DFBound b, wb;
    const uint8_t* mask;
    DfwPair* p = NULL;
    double kd = 1.0, total = 0.0, *out;
    uint32_t n = 0, i, k, nout;
    int idx, widx = -1, wa = 0, ok, wants_k = (magic != DFW_ANY_HEAVY);
    JSValue keys, vals;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    idx = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (idx < 0)
        return JS_EXCEPTION;
    if (argc > 1 && !JS_IsUndefined(argv[1]) && !JS_IsNumber(argv[1])) {
        widx = df_col_arg(ctx, df, argv[1]);
        if (widx < 0)
            return JS_EXCEPTION;
        wa = 1;
    } else if (argc > 2 && JS_IsUndefined(argv[1])) {
        wa = 1;
    }
    if (wants_k) {
        int ka = wa ? 2 : 1;
        if (JS_ToFloat64(ctx, &kd, argc > ka ? argv[ka] : JS_UNDEFINED))
            return JS_EXCEPTION;
        if (!(kd >= 1.0) || kd != floor(kd) || kd > 65536.0)
            return JS_ThrowRangeError(ctx, "%s: k must be an integer in "
                                           "[1, 65536], got %g",
                nm[magic], kd);
    }
    {
        int ma = wa + (wants_k ? 1 : 0) + 1;
        mask = df_mask_arg(ctx, argc > ma ? argv[ma] : JS_UNDEFINED,
            df->nrows, &ok);
        if (!ok)
            return JS_EXCEPTION;
    }
    if (df_bind_numeric(ctx, df, idx, &b, nm[magic]))
        return JS_EXCEPTION;
    if (widx >= 0 && df_bind_numeric(ctx, df, widx, &wb, nm[magic]))
        return JS_EXCEPTION;

    p = dfw_collect(ctx, &b, widx >= 0 ? &wb : NULL, mask, df->nrows, &n,
        nm[magic]);
    if (!p)
        return JS_EXCEPTION;
    for (i = 0; i < n; i++)
        total += p[i].w;
    qsort(p, n, sizeof(*p), dfw_cmp_w);

    if (magic == DFW_ANY_HEAVY) {
        JSValue r = (n && p[0].w * 2.0 > total)
            ? JS_NewFloat64(ctx, p[0].key)
            : JS_UNDEFINED;
        free(p);
        return r;
    }
    k = (uint32_t)kd;
    nout = n < k ? n : k;
    out = df_out_alloc(ctx, nout ? nout : 1, sizeof(double));
    if (!out) {
        free(p);
        return JS_EXCEPTION;
    }
    keys = JS_NewArray(ctx);
    if (JS_IsException(keys)) {
        free(p);
        free(out);
        return JS_EXCEPTION;
    }
    for (i = 0; i < nout; i++) {
        JSValue kv = JS_NewFloat64(ctx, p[i].key);
        out[i] = p[i].w;
        if (JS_IsException(kv) || JS_DefinePropertyValueUint32(ctx, keys, i, kv, JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, keys);
            free(p);
            free(out);
            return JS_EXCEPTION;
        }
    }
    free(p);
    vals = df_to_typed_array(ctx, out, (size_t)nout * sizeof(double),
        JS_TYPED_ARRAY_FLOAT64);
    return dfc_pair(ctx, keys, vals);
}

static JSValue dyn_df_quantile_weighted(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DataFrame* df;
    DFBound b, wb;
    const uint8_t* mask;
    DfwPair* p = NULL;
    double q = 0.5, total = 0.0, run = 0.0, target, res;
    uint32_t n = 0, i;
    int idx, widx, ok;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    idx = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (idx < 0)
        return JS_EXCEPTION;
    widx = df_col_arg(ctx, df, argc > 1 ? argv[1] : JS_UNDEFINED);
    if (widx < 0)
        return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &q, argc > 2 ? argv[2] : JS_UNDEFINED))
        return JS_EXCEPTION;
    if (isnan(q) || q < 0.0 || q > 1.0)
        return JS_ThrowRangeError(ctx, "QUANTILE_EXACT_WEIGHTED(col, w, q): q "
                                       "must be in [0, 1], got %g",
            q);
    mask = df_mask_arg(ctx, argc > 3 ? argv[3] : JS_UNDEFINED, df->nrows, &ok);
    if (!ok)
        return JS_EXCEPTION;
    if (df_bind_numeric(ctx, df, idx, &b, "QUANTILE_EXACT_WEIGHTED") || df_bind_numeric(ctx, df, widx, &wb, "QUANTILE_EXACT_WEIGHTED"))
        return JS_EXCEPTION;

    p = dfw_collect(ctx, &b, &wb, mask, df->nrows, &n,
        "QUANTILE_EXACT_WEIGHTED");
    if (!p)
        return JS_EXCEPTION;
    if (n == 0) {
        free(p);
        return JS_UNDEFINED;
    }
    for (i = 0; i < n; i++)
        total += p[i].w;
    if (!(total > 0.0)) {
        free(p);
        return JS_UNDEFINED;
    }
    target = q * total;
    res = p[n - 1].key;
    for (i = 0; i < n; i++) {
        run += p[i].w;
        if (run >= target) {
            res = p[i].key;
            break;
        }
    }
    free(p);
    return JS_NewFloat64(ctx, res);
}

static JSValue dyn_df_group_intersect(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DataFrame* df;
    DFBound kb, vb;
    const uint8_t* mask;
    DfcSet vals = { 0 }, pairs = { 0 };
    uint32_t *gk = NULL, *counts = NULL;
    uint8_t* active = NULL;
    double* out = NULL;
    int ki, vi, ok;
    uint32_t i, g, n, nkeys, ngroups, nvals, nout = 0, nactive = 0;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    ki = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (ki < 0)
        return JS_EXCEPTION;
    vi = df_col_arg(ctx, df, argc > 1 ? argv[1] : JS_UNDEFINED);
    if (vi < 0)
        return JS_EXCEPTION;
    mask = df_mask_arg(ctx, argc > 2 ? argv[2] : JS_UNDEFINED, df->nrows, &ok);
    if (!ok)
        return JS_EXCEPTION;
    if (dfc_group_count(ctx, df, ki, &nkeys, &ngroups))
        return JS_EXCEPTION;
    if (dyn_df_bind(ctx, df, ki, &kb))
        return JS_EXCEPTION;
    if (df_bind_numeric(ctx, df, vi, &vb, "GROUP_ARRAY_INTERSECT"))
        return JS_EXCEPTION;

    n = kb.n < vb.n ? kb.n : vb.n;
    counts = calloc(n ? n : 1, sizeof(*counts));
    active = calloc(ngroups ? ngroups : 1, 1);
    if (!counts || !active) {
        JS_ThrowOutOfMemory(ctx);
        goto fail;
    }
    gk = df_keys_u32(&kb, n);
    for (i = 0; i < n; i++) {
        double v;
        uint32_t e, e2;
        int fresh;
        if (mask && !mask[i])
            continue;
        g = gk ? gk[i] : (uint32_t)df_get(kb.p, kb.type, i);
        if (g >= ngroups)
            continue;
        active[g] = 1;
        v = df_get(vb.p, vb.type, i);
        if (v != v)
            continue;
        if (dfc_set_put(ctx, &vals, dfc_key(v), 0, &e, NULL))
            goto fail;
        if (dfc_set_put(ctx, &pairs, dfc_key(v), g, &e2, &fresh))
            goto fail;
        if (fresh)
            counts[e]++;
    }
    for (g = 0; g < nkeys; g++)
        nactive += active[g];
    nvals = vals.nent;
    out = df_out_alloc(ctx, nvals ? nvals : 1, sizeof(double));
    if (!out)
        goto fail;
    if (nactive)
        for (i = 0; i < nvals; i++)
            if (counts[i] == nactive)
                out[nout++] = dfc_key_value(vals.keys[i]);
    if (nout > 1)
        qsort(out, nout, sizeof(*out), dfz_cmp_d);
    dfc_set_free(&vals);
    dfc_set_free(&pairs);
    free(gk);
    free(counts);
    free(active);
    return df_to_typed_array(ctx, out, (size_t)nout * sizeof(double),
        JS_TYPED_ARRAY_FLOAT64);
fail:
    dfc_set_free(&vals);
    dfc_set_free(&pairs);
    free(gk);
    free(counts);
    free(active);
    free(out);
    return JS_EXCEPTION;
}

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

static JSValue dyn_df_get_columns(JSContext* ctx, JSValueConst this_val)
{
    DataFrame* df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    JSValue a;
    uint32_t i;
    if (!df)
        return JS_EXCEPTION;
    a = JS_NewArray(ctx);
    if (JS_IsException(a))
        return a;
    for (i = 0; i < df->ncols; i++) {
        JSValue s = JS_NewString(ctx, df->cols[i].name);
        if (JS_IsException(s) || JS_DefinePropertyValueUint32(ctx, a, i, s, JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, a);
            return JS_EXCEPTION;
        }
    }
    return a;
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

static int df_add_strings(JSContext* ctx, DataFrame* df, const char* name,
    JSValueConst arr, uint32_t n)
{
    DFColumn* c = &df->cols[df->ncols];
    uint32_t i;
    uint32_t *dfh = NULL, dfh_mask = 0;
    size_t dfh_cap = 16;

    memset(c, 0, sizeof(*c));
    c->name = strdup(name);
    c->type = DF_STR;
    c->buffer = JS_UNDEFINED;
    c->length = n;
    c->codes = malloc((size_t)(n ? n : 1) * sizeof(int32_t));
    if (!c->name || !c->codes)
        goto oom;
    while (dfh_cap < (size_t)n * 2 + 8)
        dfh_cap <<= 1;
    dfh = (uint32_t*)malloc(dfh_cap * sizeof(uint32_t));
    if (!dfh)
        goto oom;
    memset(dfh, 0xFF, dfh_cap * sizeof(uint32_t));
    dfh_mask = (uint32_t)(dfh_cap - 1);

    for (i = 0; i < n; i++) {
        JSValue v = JS_GetPropertyUint32(ctx, arr, i);
        const char* s;
        uint32_t k;
        if (JS_IsException(v))
            goto fail;
        s = JS_ToCString(ctx, v);
        JS_FreeValue(ctx, v);
        if (!s)
            goto fail;
        {
            uint32_t h = df_str_hash_basis(), m, probe;
            const char* cp;
            for (cp = s; *cp; cp++) {
                h ^= (unsigned char)*cp;
                h *= 16777619u;
            }
            m = dfh_mask;
            probe = h & m;
            k = c->dict_len;
            while (dfh[probe] != 0xFFFFFFFFu) {
                if (strcmp(c->dict[dfh[probe]], s) == 0) {
                    k = dfh[probe];
                    break;
                }
                probe = (probe + 1) & m;
            }
            if (k == c->dict_len)
                dfh[probe] = c->dict_len;
        }
        if (k == c->dict_len) {
            if (c->dict_len == c->dict_cap) {
                uint32_t nc = c->dict_cap ? c->dict_cap << 1 : 16;
                char** nd = realloc(c->dict, nc * sizeof(char*));
                if (!nd) {
                    JS_FreeCString(ctx, s);
                    goto oom;
                }
                c->dict = nd;
                c->dict_cap = nc;
            }
            c->dict[c->dict_len] = strdup(s);
            if (!c->dict[c->dict_len]) {
                JS_FreeCString(ctx, s);
                goto oom;
            }
            c->dict_len++;
        }
        JS_FreeCString(ctx, s);
        c->codes[i] = (int32_t)k;
    }
    df->ncols++;
    free(dfh);
    return 0;
oom:
    JS_ThrowOutOfMemory(ctx);
fail:
    free(dfh);
    df_col_free(c);
    memset(c, 0, sizeof(*c));
    return -1;
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

typedef struct {
    DataFrame* df;
    uint32_t cap, nrows;
    const char* op;
} DFBuilder;

#define DFB_NO_ROW UINT32_MAX
#define DFB_MAX_ROWS UINT32_MAX

static void dfb_free(JSContext* ctx, DFBuilder* B)
{
    uint32_t i;
    if (!B->df)
        return;
    for (i = 0; i < B->df->ncols; i++)
        JS_FreeValue(ctx, B->df->cols[i].buffer);
    dyn_df_dispose(B->df);
    B->df = NULL;
}

static int dfb_init(JSContext* ctx, DFBuilder* B, const char* op,
    uint32_t ncols, uint64_t nrows)
{
    B->df = NULL;
    B->cap = ncols;
    B->nrows = (uint32_t)nrows;
    B->op = op;
    if (ncols > DF_MAX_COLS)
        return JS_ThrowRangeError(ctx, "%s: too many columns (%u, max %d)",
                   op, ncols, DF_MAX_COLS),
               -1;
    if (nrows > (uint64_t)DFB_MAX_ROWS)
        return JS_ThrowRangeError(ctx, "%s: result would have %llu rows, the "
                                       "limit is %u",
                   op,
                   (unsigned long long)nrows, DFB_MAX_ROWS),
               -1;
    B->df = calloc(1, sizeof(DataFrame));
    if (B->df)
        B->df->cols = calloc(ncols ? ncols : 1, sizeof(DFColumn));
    if (!B->df || !B->df->cols) {
        dfb_free(ctx, B);
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    B->df->nrows = B->nrows;
    return 0;
}

static DFColumn* dfb_slot(JSContext* ctx, DFBuilder* B, const char* name,
    uint32_t n)
{
    DFColumn* c;

    if (!B->df) {
        JS_ThrowInternalError(ctx, "%s: builder used after seal", B->op);
        return NULL;
    }
    if (B->df->ncols >= B->cap) {
        JS_ThrowInternalError(ctx, "%s: more columns than the %u reserved",
            B->op, B->cap);
        return NULL;
    }
    if (n != B->nrows) {
        JS_ThrowRangeError(ctx, "%s: column '%s' has %u rows, the frame has %u",
            B->op, name, n, B->nrows);
        return NULL;
    }
    if (df_find_col(B->df, name) >= 0) {
        JS_ThrowRangeError(ctx, "%s: duplicate output column '%s'", B->op, name);
        return NULL;
    }
    c = &B->df->cols[B->df->ncols];
    memset(c, 0, sizeof(*c));
    c->buffer = JS_UNDEFINED;
    c->length = n;
    c->name = strdup(name);
    if (!c->name) {
        JS_ThrowOutOfMemory(ctx);
        return NULL;
    }
    B->df->ncols++;
    return c;
}

static int dfb_add(JSContext* ctx, DFBuilder* B, const char* name,
    DFType t, void* p, uint32_t n)
{
    DFColumn* c;
    JSValue ab;

    if (t == DF_STR || !p) {
        free(p);
        JS_ThrowInternalError(ctx, "%s: dfb_add('%s') needs a numeric type and "
                                   "a non-NULL buffer",
            B->op, name);
        return -1;
    }
    c = dfb_slot(ctx, B, name, n);
    if (!c) {
        free(p);
        return -1;
    }
    ab = JS_NewArrayBufferCopy(ctx, (const uint8_t*)p,
        (size_t)n * df_elt_size(t));
    free(p);
    if (JS_IsException(ab))
        return -1;
    c->type = t;
    c->buffer = ab;
    c->byte_offset = 0;
    return 0;
}

static int dfb_add_str(JSContext* ctx, DFBuilder* B, const char* name,
    int32_t* codes, char** dict, uint32_t dict_len,
    uint32_t n)
{
    DFColumn* c;
    uint32_t i;

    if (!codes || (dict_len && !dict)) {
        JS_ThrowInternalError(ctx, "%s: dfb_add_str('%s') needs codes and a "
                                   "dictionary",
            B->op, name);
        goto drop;
    }
    c = dfb_slot(ctx, B, name, n);
    if (!c)
        goto drop;
    for (i = 0; i < n; i++)
        if (codes[i] < 0 || (uint32_t)codes[i] >= dict_len) {
            JS_ThrowRangeError(ctx, "%s: column '%s' code %d is outside the "
                                    "dictionary (%u entries)",
                B->op, name,
                codes[i], dict_len);
            goto drop;
        }
    c->type = DF_STR;
    c->codes = codes;
    c->dict = dict;
    c->dict_len = dict_len;
    c->dict_cap = dict_len;
    return 0;
drop:
    free(codes);
    if (dict) {
        for (i = 0; i < dict_len; i++)
            free(dict[i]);
        free(dict);
    }
    return -1;
}

#define DFB_GATHER_ONE(cty)                           \
    do {                                              \
        cty* d = out;                                 \
        for (i = 0; i < n; i++)                       \
            d[i] = (cty)df_get(b.p, b.type, rows[i]); \
    } while (0)

static int dfb_add_gather(JSContext* ctx, DFBuilder* B, const char* name,
    const DataFrame* sdf, int scol, DFType out_type,
    const uint32_t* rows, uint32_t n)
{
    const DFColumn* sc = &sdf->cols[scol];
    DFBound b;
    void* out;
    uint32_t i;

    if (dyn_df_bind(ctx, sdf, scol, &b))
        return -1;
    if (n && !rows) {
        JS_ThrowInternalError(ctx, "%s: gather of '%s' needs a row index array",
            B->op, sc->name);
        return -1;
    }
    if (out_type != b.type && out_type != DF_F64) {
        JS_ThrowTypeError(ctx, "%s: column '%s' is %s; gather it as that type "
                               "or as Float64Array",
            B->op, sc->name,
            df_type_name(b.type));
        return -1;
    }
    if (b.type == DF_STR) {
        int32_t* codes = df_out_alloc(ctx, n, sizeof(int32_t));
        char** dict;
        uint32_t d;
        if (!codes)
            return -1;
        for (i = 0; i < n; i++) {
            if (rows[i] >= b.n) {
                free(codes);
                JS_ThrowRangeError(ctx, "%s: column '%s' row %u is outside the "
                                        "source (%u rows); a string column has no "
                                        "missing value",
                    B->op, sc->name, rows[i],
                    b.n);
                return -1;
            }
            codes[i] = ((const int32_t*)b.p)[rows[i]];
        }
        dict = calloc(sc->dict_len ? sc->dict_len : 1, sizeof(char*));
        if (!dict) {
            free(codes);
            JS_ThrowOutOfMemory(ctx);
            return -1;
        }
        for (d = 0; d < sc->dict_len; d++) {
            dict[d] = strdup(sc->dict[d]);
            if (!dict[d]) {
                while (d--)
                    free(dict[d]);
                free(dict);
                free(codes);
                JS_ThrowOutOfMemory(ctx);
                return -1;
            }
        }
        return dfb_add_str(ctx, B, name, codes, dict, sc->dict_len, n);
    }

    out = df_out_alloc(ctx, n, df_elt_size(out_type));
    if (!out)
        return -1;
    if (out_type == DF_F64) {
        double* d = out;
        for (i = 0; i < n; i++) {
            uint32_t r = rows[i];
            if (r == DFB_NO_ROW) {
                d[i] = DYN_NAN;
                continue;
            }
            if (r >= b.n)
                goto bad_row;
            d[i] = df_get(b.p, b.type, r);
        }
    } else {
        for (i = 0; i < n; i++)
            if (rows[i] >= b.n)
                goto bad_row;
        switch ((int)(out_type)) {
        case DF_F32:
            DFB_GATHER_ONE(float);
            break;
        case DF_I32:
            DFB_GATHER_ONE(int32_t);
            break;
        case DF_U32:
            DFB_GATHER_ONE(uint32_t);
            break;
        case DF_I16:
            DFB_GATHER_ONE(int16_t);
            break;
        case DF_U16:
            DFB_GATHER_ONE(uint16_t);
            break;
        case DF_I8:
            DFB_GATHER_ONE(int8_t);
            break;
        default:
            DFB_GATHER_ONE(uint8_t);
            break;
        }
    }
    return dfb_add(ctx, B, name, out_type, out, n);
bad_row:
    free(out);
    JS_ThrowRangeError(ctx, "%s: column '%s' row %u is outside the source "
                            "(%u rows)",
        B->op, sc->name, rows[i], b.n);
    return -1;
}

static JSValue dfb_seal(JSContext* ctx, DFBuilder* B)
{
    DataFrame* df = B->df;
    JSValue obj;
    uint32_t i;

    if (!df)
        return JS_ThrowInternalError(ctx, "%s: nothing to seal", B->op);
    if (df->ncols != B->cap) {
        JS_ThrowInternalError(ctx, "%s: reserved %u columns, filled %u",
            B->op, B->cap, df->ncols);
        dfb_free(ctx, B);
        return JS_EXCEPTION;
    }
    B->df = NULL;
    obj = dyn_plain_wrap(ctx, JS_UNDEFINED, dyn_df_class_id, df, NULL);
    if (JS_IsException(obj)) {
        for (i = 0; i < df->ncols; i++)
            JS_FreeValue(ctx, df->cols[i].buffer);
        dyn_df_dispose(df);
    }
    return obj;
}

static int dfc_set_find(const DfcSet* s, uint64_t k, uint32_t tag, uint32_t* pent)
{
    uint32_t p;

    if (!s->slots || !s->nent)
        return -1;
    p = dfc_hash(k, tag) & s->mask;
    while (s->slots[p] != DFC_EMPTY) {
        uint32_t e = s->slots[p];
        if (s->keys[e] == k && s->tags[e] == tag) {
            if (pent)
                *pent = e;
            return 0;
        }
        p = (p + 1) & s->mask;
    }
    return -1;
}

typedef struct {
    char** dict;
    uint32_t dict_len, dict_cap;
    uint32_t* slots;
    uint32_t slots_mask;
} DfStrTab;

static void df_strtab_free(DfStrTab* t)
{
    uint32_t i;
    for (i = 0; i < t->dict_len; i++)
        free(t->dict[i]);
    free(t->dict);
    free(t->slots);
    memset(t, 0, sizeof(*t));
}

static int df_strtab_put(JSContext* ctx, DfStrTab* t, const char* s,
    uint32_t* code)
{
    uint32_t h, p, k;

    if (!t->slots) {
        uint32_t ncap = 64;
        t->slots = malloc(ncap * sizeof(*t->slots));
        if (!t->slots) {
            JS_ThrowOutOfMemory(ctx);
            return -1;
        }
        memset(t->slots, 0xFF, ncap * sizeof(*t->slots));
        t->slots_mask = ncap - 1;
        t->dict_cap = 16;
        t->dict = malloc(t->dict_cap * sizeof(*t->dict));
        if (!t->dict) {
            JS_ThrowOutOfMemory(ctx);
            return -1;
        }
    }
    h = df_str_hash_basis();
    for (p = 0; s[p]; p++) {
        h ^= (unsigned char)s[p];
        h *= 16777619u;
    }
    p = h & t->slots_mask;
    while (t->slots[p] != 0xFFFFFFFFu) {
        if (strcmp(t->dict[t->slots[p]], s) == 0) {
            *code = t->slots[p];
            return 0;
        }
        p = (p + 1) & t->slots_mask;
    }
    if ((t->dict_len + 1) * 2 >= t->slots_mask + 1) {
        uint32_t ncap = (t->slots_mask + 1) << 1, i;
        uint32_t* ns = malloc(ncap * sizeof(*ns));
        if (!ns) {
            JS_ThrowOutOfMemory(ctx);
            return -1;
        }
        memset(ns, 0xFF, ncap * sizeof(*ns));
        for (i = 0; i < t->dict_len; i++) {
            uint32_t q = df_str_hash_basis(), c;
            for (c = 0; t->dict[i][c]; c++) {
                q ^= (unsigned char)t->dict[i][c];
                q *= 16777619u;
            }
            q &= ncap - 1;
            while (ns[q] != 0xFFFFFFFFu)
                q = (q + 1) & (ncap - 1);
            ns[q] = i;
        }
        free(t->slots);
        t->slots = ns;
        t->slots_mask = ncap - 1;
        p = h & t->slots_mask;
        while (t->slots[p] != 0xFFFFFFFFu)
            p = (p + 1) & t->slots_mask;
    }
    if (t->dict_len == t->dict_cap) {
        uint32_t nc = t->dict_cap << 1;
        char** nd = realloc(t->dict, nc * sizeof(*nd));
        if (!nd) {
            JS_ThrowOutOfMemory(ctx);
            return -1;
        }
        t->dict = nd;
        t->dict_cap = nc;
    }
    k = t->dict_len;
    t->dict[k] = strdup(s);
    if (!t->dict[k]) {
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    t->dict_len++;
    t->slots[p] = k;
    *code = k;
    return 0;
}

static uint64_t df_splitmix64(uint64_t* st)
{
    uint64_t z = (*st += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

static int df_strtab_find(const DfStrTab* t, const char* s)
{
    uint32_t h, p;

    if (!t->slots)
        return -1;
    h = df_str_hash_basis();
    for (p = 0; s[p]; p++) {
        h ^= (unsigned char)s[p];
        h *= 16777619u;
    }
    p = h & t->slots_mask;
    while (t->slots[p] != 0xFFFFFFFFu) {
        if (strcmp(t->dict[t->slots[p]], s) == 0)
            return (int)t->slots[p];
        p = (p + 1) & t->slots_mask;
    }
    return -1;
}

typedef struct {
    char* p;
    size_t len, cap;
    int failed;
} DfBuf;

static void df_buf_put(DfBuf* b, const char* s, size_t n)
{
    if (b->failed)
        return;
    if (b->len + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 256;
        char* np;
        while (nc < b->len + n + 1)
            nc *= 2;
        np = realloc(b->p, nc);
        if (!np) {
            b->failed = 1;
            return;
        }
        b->p = np;
        b->cap = nc;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = 0;
}

static const char* df_type_short(DFType t)
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

static JSTypedArrayEnum df_type_ta(DFType t)
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

static void* df_col_copy(JSContext* ctx, const DataFrame* df, int idx,
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

static JSValue dyn_df_dtypes(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DataFrame* df;
    JSValue res;
    uint32_t i;

    (void)argc;
    (void)argv;
    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    res = JS_NewObject(ctx);
    if (JS_IsException(res))
        return res;
    for (i = 0; i < df->ncols; i++) {
        if (JS_DefinePropertyValueStr(ctx, res, df->cols[i].name,
                JS_NewString(ctx, df_type_short(df->cols[i].type)),
                JS_PROP_C_W_E)
            < 0) {
            JS_FreeValue(ctx, res);
            return JS_EXCEPTION;
        }
    }
    return res;
}

static JSValue dyn_df_schema(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DataFrame* df;
    JSValue res, item;
    uint32_t i;

    (void)argc;
    (void)argv;
    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    res = JS_NewArray(ctx);
    if (JS_IsException(res))
        return res;
    for (i = 0; i < df->ncols; i++) {
        item = JS_NewObject(ctx);
        if (JS_IsException(item) || JS_DefinePropertyValueStr(ctx, item, "name", JS_NewString(ctx, df->cols[i].name), JS_PROP_C_W_E) < 0 || JS_DefinePropertyValueStr(ctx, item, "type", JS_NewString(ctx, df_type_short(df->cols[i].type)), JS_PROP_C_W_E) < 0 || JS_DefinePropertyValueUint32(ctx, res, i, item, JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, res);
            return JS_EXCEPTION;
        }
    }
    return res;
}

static uint64_t df_col_bytes(const DFColumn* c)
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

static JSValue dyn_df_info(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DataFrame* df;
    JSValue res, dtypes, bytes, total;
    uint64_t sum = 0;
    uint32_t i;

    (void)argc;
    (void)argv;
    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    res = JS_NewObject(ctx);
    if (JS_IsException(res))
        return res;
    dtypes = JS_NewObject(ctx);
    bytes = JS_NewObject(ctx);
    total = JS_NewFloat64(ctx, 0);
    if (JS_IsException(dtypes) || JS_IsException(bytes) || JS_IsException(total))
        goto fail;
    for (i = 0; i < df->ncols; i++) {
        uint64_t b = df_col_bytes(&df->cols[i]);
        sum += b;
        if (JS_DefinePropertyValueStr(ctx, dtypes, df->cols[i].name,
                JS_NewString(ctx, df_type_short(df->cols[i].type)),
                JS_PROP_C_W_E)
                < 0
            || JS_DefinePropertyValueStr(ctx, bytes, df->cols[i].name,
                   JS_NewFloat64(ctx, (double)b),
                   JS_PROP_C_W_E)
                < 0)
            goto fail;
    }
    if (JS_DefinePropertyValueStr(ctx, res, "rows", JS_NewInt64(ctx, df->nrows),
            JS_PROP_C_W_E)
            < 0
        || JS_DefinePropertyValueStr(ctx, res, "cols", JS_NewInt64(ctx, df->ncols),
               JS_PROP_C_W_E)
            < 0)
        goto fail;
    {
        JSValue owned = dtypes;
        dtypes = JS_UNDEFINED;
        if (JS_DefinePropertyValueStr(ctx, res, "dtypes", owned, JS_PROP_C_W_E) < 0)
            goto fail;
    }
    {
        JSValue owned = bytes;
        bytes = JS_UNDEFINED;
        if (JS_DefinePropertyValueStr(ctx, res, "bytes", owned, JS_PROP_C_W_E) < 0)
            goto fail;
    }
    if (JS_DefinePropertyValueStr(ctx, res, "total_bytes",
            JS_NewFloat64(ctx, (double)sum), JS_PROP_C_W_E)
        < 0)
        goto fail;
    JS_FreeValue(ctx, total);
    return res;
fail:
    JS_FreeValue(ctx, dtypes);
    JS_FreeValue(ctx, bytes);
    JS_FreeValue(ctx, total);
    JS_FreeValue(ctx, res);
    return JS_EXCEPTION;
}

static JSValue dyn_df_memory_usage(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DataFrame* df;
    JSValue res, cols, total;
    uint64_t sum = 0;
    uint32_t i;

    (void)argc;
    (void)argv;
    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    res = JS_NewObject(ctx);
    if (JS_IsException(res))
        return res;
    cols = JS_NewObject(ctx);
    if (JS_IsException(cols)) {
        JS_FreeValue(ctx, res);
        return JS_EXCEPTION;
    }
    for (i = 0; i < df->ncols; i++) {
        uint64_t b = df_col_bytes(&df->cols[i]);
        sum += b;
        if (JS_DefinePropertyValueStr(ctx, cols, df->cols[i].name,
                JS_NewFloat64(ctx, (double)b),
                JS_PROP_C_W_E)
            < 0) {
            JS_FreeValue(ctx, cols);
            JS_FreeValue(ctx, res);
            return JS_EXCEPTION;
        }
    }
    total = JS_NewFloat64(ctx, (double)sum);
    if (JS_DefinePropertyValueStr(ctx, res, "columns", cols, JS_PROP_C_W_E) < 0 || JS_DefinePropertyValueStr(ctx, res, "total", total, JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, res);
        return JS_EXCEPTION;
    }
    return res;
}

static JSValue dyn_df_to_columns(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DataFrame* df;
    JSValue res;
    uint32_t i;

    (void)argc;
    (void)argv;
    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    res = JS_NewObject(ctx);
    if (JS_IsException(res))
        return res;
    for (i = 0; i < df->ncols; i++) {
        JSValue v;
        if (df->cols[i].type == DF_STR) {
            uint32_t n, k;
            DFType t;
            int32_t* codes = df_col_copy(ctx, df, (int)i, &n, &t);
            if (!codes) {
                JS_FreeValue(ctx, res);
                return JS_EXCEPTION;
            }
            v = JS_NewArray(ctx);
            if (JS_IsException(v)) {
                free(codes);
                JS_FreeValue(ctx, res);
                return JS_EXCEPTION;
            }
            for (k = 0; k < n; k++) {
                int32_t c = codes[k];
                if (c < 0 || (uint32_t)c >= df->cols[i].dict_len) {
                    JS_FreeValue(ctx, v);
                    free(codes);
                    JS_FreeValue(ctx, res);
                    return JS_ThrowRangeError(ctx, "TO_COLUMNS: column '%s' "
                                                   "code %d out of range",
                        df->cols[i].name, c);
                }
                if (JS_DefinePropertyValueUint32(ctx, v, k,
                        JS_NewString(ctx, df->cols[i].dict[c]),
                        JS_PROP_C_W_E)
                    < 0) {
                    JS_FreeValue(ctx, v);
                    free(codes);
                    JS_FreeValue(ctx, res);
                    return JS_EXCEPTION;
                }
            }
            free(codes);
        } else {
            DFBound b;
            void* out;
            if (dyn_df_bind(ctx, df, (int)i, &b))
                goto fail;
            out = df_out_alloc(ctx, df->nrows ? df->nrows : 1, df_elt_size(b.type));
            if (!out)
                goto fail;
            memcpy(out, b.p, (size_t)df->nrows * df_elt_size(b.type));
            v = df_to_typed_array(ctx, out, (size_t)df->nrows * df_elt_size(b.type),
                df_type_ta(b.type));
            if (JS_IsException(v))
                goto fail;
        }
        if (JS_DefinePropertyValueStr(ctx, res, df->cols[i].name, v,
                JS_PROP_C_W_E)
            < 0) {
            JS_FreeValue(ctx, v);
            goto fail;
        }
    }
    return res;
fail:
    JS_FreeValue(ctx, res);
    return JS_EXCEPTION;
}

static JSValue dyn_df_to_records(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DataFrame* df;
    JSValue res = JS_UNDEFINED, row;
    DFBound* bs = NULL;
    JSAtom* atoms = NULL;
    uint32_t n, c, r, natoms = 0;

    (void)argc;
    (void)argv;
    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    bs = (DFBound*)malloc((df->ncols ? df->ncols : 1) * sizeof(*bs));
    atoms = (JSAtom*)malloc((df->ncols ? df->ncols : 1) * sizeof(*atoms));
    if (!bs || !atoms) {
        JS_ThrowOutOfMemory(ctx);
        goto fail;
    }
    for (c = 0; c < df->ncols; c++) {
        if (dyn_df_bind(ctx, df, (int)c, &bs[c]))
            goto fail;
        atoms[c] = JS_NewAtom(ctx, df->cols[c].name);
        if (atoms[c] == JS_ATOM_NULL)
            goto fail;
        natoms = c + 1;
    }
    res = JS_NewArray(ctx);
    if (JS_IsException(res))
        goto fail;
    n = df->nrows;
    for (r = 0; r < n; r++) {
        row = JS_NewObject(ctx);
        if (JS_IsException(row))
            goto fail;
        for (c = 0; c < df->ncols; c++) {
            JSValue v;
            if (df->cols[c].type == DF_STR) {
                int32_t code = ((const int32_t*)bs[c].p)[r];
                if (code < 0 || (uint32_t)code >= df->cols[c].dict_len) {
                    JS_FreeValue(ctx, row);
                    JS_ThrowRangeError(ctx, "TO_RECORDS: column '%s' "
                                            "code %d out of range",
                        df->cols[c].name, code);
                    goto fail;
                }
                v = JS_NewString(ctx, df->cols[c].dict[code]);
            } else {
                v = JS_NewFloat64(ctx, df_get(bs[c].p, bs[c].type, r));
            }
            if (JS_DefinePropertyValue(ctx, row, atoms[c], v, JS_PROP_C_W_E) < 0) {
                JS_FreeValue(ctx, row);
                goto fail;
            }
        }
        if (JS_DefinePropertyValueUint32(ctx, res, r, row, JS_PROP_C_W_E) < 0)
            goto fail;
    }
    for (c = 0; c < natoms; c++)
        JS_FreeAtom(ctx, atoms[c]);
    free(atoms);
    free(bs);
    return res;
fail:
    for (c = 0; c < natoms; c++)
        JS_FreeAtom(ctx, atoms[c]);
    free(atoms);
    free(bs);
    JS_FreeValue(ctx, res);
    return JS_EXCEPTION;
}

static JSValue dyn_df_to_json(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSValue recs, s;

    (void)argc;
    (void)argv;
    recs = dyn_df_to_records(ctx, this_val, 0, argv);
    if (JS_IsException(recs))
        return JS_EXCEPTION;
    s = JS_JSONStringify(ctx, recs, JS_UNDEFINED, JS_UNDEFINED);
    JS_FreeValue(ctx, recs);
    return s;
}

static void df_csv_field_esc(DfBuf* b, const char* s, int esc_formula)
{
    size_t i, n = strlen(s);
    int need = 0;

    int guard = esc_formula && n && (s[0] == '=' || s[0] == '+' || s[0] == '-' || s[0] == '@' || s[0] == '\t' || s[0] == '\r');

    for (i = 0; i < n; i++)
        if (s[i] == ',' || s[i] == '"' || s[i] == '\r' || s[i] == '\n') {
            need = 1;
            break;
        }
    if (!need) {
        if (guard)
            df_buf_put(b, "'", 1);
        df_buf_put(b, s, n);
        return;
    }
    df_buf_put(b, "\"", 1);
    if (guard)
        df_buf_put(b, "'", 1);
    for (i = 0; i < n; i++) {
        if (s[i] == '"')
            df_buf_put(b, "\"\"", 2);
        else
            df_buf_put(b, s + i, 1);
    }
    df_buf_put(b, "\"", 1);
}

static JSValue dyn_df_to_csv(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DataFrame* df;
    DfBuf b = { 0 };
    DFBound* bs;
    uint32_t r, c, n;
    JSValue res;

    int esc_formula = 0;

    if (argc > 0 && JS_IsObject(argv[0])) {
        JSValue v = JS_GetPropertyStr(ctx, argv[0], "escapeFormulas");
        if (JS_IsException(v))
            return JS_EXCEPTION;
        if (JS_ToBool(ctx, v) == 1)
            esc_formula = 1;
        JS_FreeValue(ctx, v);
    }
    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    for (c = 0; c < df->ncols; c++) {
        if (c)
            df_buf_put(&b, ",", 1);
        df_csv_field_esc(&b, df->cols[c].name, esc_formula);
    }
    df_buf_put(&b, "\n", 1);
    n = df->nrows;
    bs = (DFBound*)malloc((df->ncols ? df->ncols : 1) * sizeof(*bs));
    if (!bs) {
        free(b.p);
        return JS_ThrowOutOfMemory(ctx);
    }
    for (c = 0; c < df->ncols; c++)
        if (dyn_df_bind(ctx, df, (int)c, &bs[c])) {
            free(bs);
            free(b.p);
            return JS_EXCEPTION;
        }
    for (r = 0; r < n; r++) {
        for (c = 0; c < df->ncols; c++) {
            if (c)
                df_buf_put(&b, ",", 1);
            if (df->cols[c].type == DF_STR) {
                int32_t code = ((const int32_t*)bs[c].p)[r];
                if (code < 0 || (uint32_t)code >= df->cols[c].dict_len) {
                    free(bs);
                    free(b.p);
                    return JS_ThrowRangeError(ctx, "TO_CSV: column '%s' code "
                                                   "%d out of range",
                        df->cols[c].name, code);
                }
                df_csv_field_esc(&b, df->cols[c].dict[code], esc_formula);
            } else {
                char tmp[32];
                double v = df_get(bs[c].p, bs[c].type, r);
                if (v != v) {
                    continue;
                } else {
                    df_fmt_double(tmp, sizeof(tmp), v);
                    df_buf_put(&b, tmp, strlen(tmp));
                }
            }
        }
        df_buf_put(&b, "\n", 1);
    }
    free(bs);
    if (b.failed) {
        free(b.p);
        return JS_ThrowOutOfMemory(ctx);
    }
    res = JS_NewStringLen(ctx, b.p ? b.p : "", b.len);
    free(b.p);
    return res;
}

static JSValue dyn_df_from_records(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSValue rows, lenv, res;
    uint32_t nrows, i, c, ncols = 0;
    uint32_t nkeys;
    JSPropertyEnum* tab = NULL;
    const char** names = NULL;
    JSAtom* col_atoms = NULL;
    int* is_num = NULL;
    DFBuilder B = { 0 };

    (void)this_val;
    if (argc < 1 || !JS_IsArray(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "FROM_RECORDS(rows): expected an array of "
                                      "row objects");
    rows = argv[0];
    lenv = JS_GetPropertyStr(ctx, rows, "length");
    if (JS_IsException(lenv))
        return JS_EXCEPTION;
    if (JS_ToUint32(ctx, &nrows, lenv)) {
        JS_FreeValue(ctx, lenv);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, lenv);
    if (nrows == 0)
        return JS_ThrowRangeError(ctx, "FROM_RECORDS(rows): cannot infer a "
                                       "schema from zero rows");

    names = malloc((size_t)DF_MAX_COLS * sizeof(char*));
    col_atoms = malloc((size_t)DF_MAX_COLS * sizeof(JSAtom));
    if (!names || !col_atoms) {
        free(names);
        free(col_atoms);
        return JS_ThrowOutOfMemory(ctx);
    }
    for (i = 0; i < nrows; i++) {
        JSValue row = JS_GetPropertyUint32(ctx, rows, i);
        uint32_t k;
        if (JS_IsException(row)) {
            JS_ThrowOutOfMemory(ctx);
            goto fail;
        }
        if (!JS_IsObject(row) || JS_IsArray(ctx, row)) {
            JS_FreeValue(ctx, row);
            JS_ThrowTypeError(ctx, "FROM_RECORDS(rows): row %u is not a plain "
                                   "object",
                i);
            goto fail;
        }
        if (JS_GetOwnPropertyNames(ctx, &tab, &nkeys, row,
                JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY)
            < 0) {
            JS_FreeValue(ctx, row);
            goto fail;
        }
        for (k = 0; k < nkeys; k++) {
            JSValue nv;
            size_t nlen = 0;
            const char* nm;
            uint32_t j;
            int seen = k < ncols && col_atoms[k] == tab[k].atom;
            for (j = 0; !seen && j < ncols; j++)
                seen = col_atoms[j] == tab[k].atom;
            if (seen)
                continue;
            nv = JS_AtomToValue(ctx, tab[k].atom);
            nm = JS_IsException(nv) ? NULL : JS_ToCStringLen(ctx, &nlen, nv);
            JS_FreeValue(ctx, nv);
            if (!nm) {
                JS_FreeValue(ctx, row);
                goto fail;
            }
            if (strlen(nm) != nlen) {
                JS_FreeCString(ctx, nm);
                JS_FreeValue(ctx, row);
                JS_ThrowRangeError(ctx, "FROM_RECORDS: column name contains a "
                                        "NUL character");
                goto fail;
            }
            {
                if (ncols >= DF_MAX_COLS) {
                    JS_FreeCString(ctx, nm);
                    JS_FreeValue(ctx, row);
                    JS_ThrowRangeError(ctx, "FROM_RECORDS: too many columns "
                                            "(max %d)",
                        DF_MAX_COLS);
                    goto fail;
                }
                names[ncols] = strdup(nm);
                if (!names[ncols]) {
                    JS_FreeCString(ctx, nm);
                    JS_FreeValue(ctx, row);
                    JS_ThrowOutOfMemory(ctx);
                    goto fail;
                }
                col_atoms[ncols] = JS_DupAtom(ctx, tab[k].atom);
                ncols++;
            }
            JS_FreeCString(ctx, nm);
        }
        for (k = 0; k < nkeys; k++)
            JS_FreeAtom(ctx, tab[k].atom);
        js_free(ctx, tab);
        tab = NULL;
        JS_FreeValue(ctx, row);
    }

    is_num = malloc((size_t)(ncols ? ncols : 1) * sizeof(int));
    if (!is_num) {
        JS_ThrowOutOfMemory(ctx);
        goto fail;
    }
    for (c = 0; c < ncols; c++) {
        int allnum = 1;
        for (i = 0; i < nrows; i++) {
            JSValue row = JS_GetPropertyUint32(ctx, rows, i);
            JSValue v;
            if (JS_IsException(row)) {
                JS_ThrowOutOfMemory(ctx);
                goto fail;
            }
            v = JS_GetProperty(ctx, row, col_atoms[c]);
            JS_FreeValue(ctx, row);
            if (JS_IsException(v)) {
                JS_ThrowOutOfMemory(ctx);
                goto fail;
            }
            if (!JS_IsUndefined(v) && !JS_IsNumber(v))
                allnum = 0;
            JS_FreeValue(ctx, v);
            if (!allnum)
                break;
        }
        is_num[c] = allnum;
    }

    if (dfb_init(ctx, &B, "FROM_RECORDS", ncols, nrows))
        goto fail;
    for (c = 0; c < ncols; c++) {
        if (is_num[c]) {
            double* vals = df_out_alloc(ctx, nrows ? nrows : 1, sizeof(double));
            if (!vals)
                goto fail;
            for (i = 0; i < nrows; i++) {
                JSValue row = JS_GetPropertyUint32(ctx, rows, i);
                JSValue v;
                if (JS_IsException(row)) {
                    JS_ThrowOutOfMemory(ctx);
                    free(vals);
                    goto fail;
                }
                v = JS_GetProperty(ctx, row, col_atoms[c]);
                JS_FreeValue(ctx, row);
                if (JS_IsException(v)) {
                    JS_ThrowOutOfMemory(ctx);
                    free(vals);
                    goto fail;
                }
                if (JS_IsUndefined(v)) {
                    vals[i] = DYN_NAN;
                } else if (JS_ToFloat64(ctx, &vals[i], v)) {
                    JS_FreeValue(ctx, v);
                    free(vals);
                    goto fail;
                }
                JS_FreeValue(ctx, v);
            }
            if (dfb_add(ctx, &B, names[c], DF_F64, vals, nrows)) {
                vals = NULL;
                goto fail;
            }
        } else {
            int32_t* codes = df_out_alloc(ctx, nrows ? nrows : 1, sizeof(int32_t));
            DfStrTab strtab = { 0 };
            if (!codes)
                goto fail;
            for (i = 0; i < nrows; i++) {
                JSValue row = JS_GetPropertyUint32(ctx, rows, i);
                JSValue v;
                const char* s;
                uint32_t code;
                if (JS_IsException(row)) {
                    JS_ThrowOutOfMemory(ctx);
                    free(codes);
                    df_strtab_free(&strtab);
                    goto fail;
                }
                v = JS_GetProperty(ctx, row, col_atoms[c]);
                JS_FreeValue(ctx, row);
                if (JS_IsException(v)) {
                    JS_ThrowOutOfMemory(ctx);
                    free(codes);
                    df_strtab_free(&strtab);
                    goto fail;
                }
                if (JS_IsUndefined(v)) {
                    s = "";
                    if (df_strtab_put(ctx, &strtab, s, &code)) {
                        free(codes);
                        df_strtab_free(&strtab);
                        goto fail;
                    }
                } else {
                    s = JS_ToCString(ctx, v);
                    JS_FreeValue(ctx, v);
                    if (!s) {
                        free(codes);
                        df_strtab_free(&strtab);
                        goto fail;
                    }
                    if (df_strtab_put(ctx, &strtab, s, &code)) {
                        JS_FreeCString(ctx, s);
                        free(codes);
                        df_strtab_free(&strtab);
                        goto fail;
                    }
                    JS_FreeCString(ctx, s);
                }
                codes[i] = (int32_t)code;
            }
            if (dfb_add_str(ctx, &B, names[c], codes, strtab.dict, strtab.dict_len,
                    nrows)) {
                strtab.dict = NULL;
                strtab.dict_len = 0;
                df_strtab_free(&strtab);
                goto fail;
            }
            strtab.dict = NULL;
            strtab.dict_len = 0;
            df_strtab_free(&strtab);
        }
    }
    res = dfb_seal(ctx, &B);
    for (i = 0; i < ncols; i++) {
        free(DYN_UNCONST(names[i]));
        JS_FreeAtom(ctx, col_atoms[i]);
    }
    free(col_atoms);
    free(names);
    free(is_num);
    return res;
fail:
    if (tab) {
        uint32_t k;
        for (k = 0; k < nkeys; k++)
            JS_FreeAtom(ctx, tab[k].atom);
        js_free(ctx, tab);
    }
    dfb_free(ctx, &B);
    for (i = 0; i < ncols; i++) {
        free(DYN_UNCONST(names[i]));
        JS_FreeAtom(ctx, col_atoms[i]);
    }
    free(col_atoms);
    free(names);
    free(is_num);
    return JS_EXCEPTION;
}

static JSValue dyn_df_copy(JSContext* ctx, JSValueConst this_val,
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

static JSValue dyn_df_select(JSContext* ctx, JSValueConst this_val,
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

static JSValue dyn_df_drop_columns(JSContext* ctx, JSValueConst this_val,
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

static JSValue dyn_df_rename(JSContext* ctx, JSValueConst this_val,
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

static JSValue dyn_df_filter(JSContext* ctx, JSValueConst this_val,
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
    for (i = 0; i < lim; i++)
        if (mask[i])
            rows[nsel++] = i;
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

static JSValue dyn_df_slice(JSContext* ctx, JSValueConst this_val,
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

static JSValue dyn_df_sample(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DataFrame* df;
    DFBuilder B = { 0 };
    uint32_t *rows = NULL, *shuf = NULL;
    uint32_t i, n, nrows;
    double want = 0;
    uint64_t seed;
    JSValue res;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &want, argc > 0 ? argv[0] : JS_UNDEFINED))
        return JS_EXCEPTION;
    if (!(want >= 0) || !isfinite(want) || want != floor(want))
        return JS_ThrowRangeError(ctx, "SAMPLE(n[, seed]): n must be a "
                                       "non-negative integer, got %g",
            want);
    nrows = df->nrows;
    if (want > (double)nrows)
        return JS_ThrowRangeError(ctx, "SAMPLE(n[, seed]): n (%g) exceeds the "
                                       "row count (%u)",
            want, nrows);
    n = (uint32_t)want;
    if (argc > 1 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
        uint32_t s32;
        if (JS_ToUint32(ctx, &s32, argv[1]))
            return JS_EXCEPTION;
        seed = s32;
    } else {
        seed = 0x9e3779b97f4a7c15ULL ^ (uint64_t)(uintptr_t)&nrows;
        seed ^= (uint64_t)(unsigned)time(NULL) << 17;
        seed ^= (uint64_t)df_getpid();
    }
    rows = df_out_alloc(ctx, n ? n : 1, sizeof(uint32_t));
    shuf = df_out_alloc(ctx, nrows ? nrows : 1, sizeof(uint32_t));
    if (!rows || !shuf) {
        free(rows);
        free(shuf);
        return JS_EXCEPTION;
    }
    for (i = 0; i < nrows; i++)
        shuf[i] = i;
    for (i = 0; i < n; i++) {
        uint32_t j = i + (uint32_t)(df_splitmix64(&seed) % (uint64_t)(nrows - i));
        uint32_t tmp = shuf[i];
        shuf[i] = shuf[j];
        shuf[j] = tmp;
    }
    for (i = 0; i < n; i++)
        rows[i] = shuf[i];
    free(shuf);
    if (dfb_init(ctx, &B, "SAMPLE", df->ncols, n))
        goto fail;
    for (i = 0; i < df->ncols; i++)
        if (dfb_add_gather(ctx, &B, df->cols[i].name, df, (int)i,
                df->cols[i].type, rows, n))
            goto fail;
    free(rows);
    res = dfb_seal(ctx, &B);
    return res;
fail:
    free(rows);
    dfb_free(ctx, &B);
    return JS_EXCEPTION;
}

static JSValue dyn_df_isin(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DataFrame* df;
    DFBound b;
    DfcSet set = { 0 };
    DfStrTab strt = { 0 };
    const char** strvals = NULL;
    uint8_t* dst;
    uint32_t i, n, nvals = 0;
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
        strvals = js_mallocz(ctx, (size_t)(nvals ? nvals : 1) * sizeof(char*));
        if (!strvals) {
            JS_ThrowOutOfMemory(ctx);
            return JS_EXCEPTION;
        }
        for (i = 0; i < nvals; i++) {
            JSValue v = JS_GetPropertyUint32(ctx, argv[1], i);
            const char* s;
            uint32_t code;
            if (JS_IsException(v)) {
                JS_ThrowOutOfMemory(ctx);
                goto fail_str;
            }
            s = JS_ToCString(ctx, v);
            JS_FreeValue(ctx, v);
            if (!s)
                goto fail_str;
            if (df_strtab_put(ctx, &strt, s, &code)) {
                JS_FreeCString(ctx, s);
                goto fail_str;
            }
            strvals[i] = s;
        }
    } else {
        for (i = 0; i < nvals; i++) {
            JSValue v = JS_GetPropertyUint32(ctx, argv[1], i);
            double x;
            if (JS_IsException(v))
                goto fail_num;
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
    if (strvals) {
        for (i = 0; i < nvals; i++)
            if (strvals[i])
                JS_FreeCString(ctx, strvals[i]);
        js_free(ctx, strvals);
    }
    return df_to_typed_array(ctx, dst, n, JS_TYPED_ARRAY_UINT8);
fail_dst:
    free(dst);
fail_both:
    dfc_set_free(&set);
    df_strtab_free(&strt);
fail_str:
    if (strvals) {
        for (i = 0; i < nvals; i++)
            if (strvals[i])
                JS_FreeCString(ctx, strvals[i]);
        js_free(ctx, strvals);
    }
    return JS_EXCEPTION;
fail_num:
    dfc_set_free(&set);
    return JS_EXCEPTION;
}

static JSValue dyn_df_mask(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DataFrame* df;
    DFBuilder B = { 0 };
    const uint8_t* mask;
    uint32_t* rows = NULL;
    uint32_t i, c, n;
    double filld = 0;
    const char* fills = NULL;
    int ok, have_fill = 0;
    JSValue res;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
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
                    if (fills)
                        JS_FreeCString(ctx, fills);
                    return JS_EXCEPTION;
                }
            if (have_fill) {
                const char* fs = fills ? fills : "";
                if (df_strtab_put(ctx, &tab, fs, &fillcode)) {
                    df_strtab_free(&tab);
                    free(rows);
                    dfb_free(ctx, &B);
                    if (fills)
                        JS_FreeCString(ctx, fills);
                    return JS_EXCEPTION;
                }
            }
            for (i = 0; i < n; i++) {
                int32_t oc = ((const int32_t*)b.p)[i];
                if (mask[i]) {
                    codes[i] = oc;
                } else {
                    const char* fs = fills ? fills : "";
                    uint32_t code;
                    if (df_strtab_put(ctx, &tab, fs, &code)) {
                        df_strtab_free(&tab);
                        free(rows);
                        dfb_free(ctx, &B);
                        if (fills)
                            JS_FreeCString(ctx, fills);
                        return JS_EXCEPTION;
                    }
                    codes[i] = (int32_t)code;
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
            for (i = 0; i < n; i++)
                out[i] = mask[i] ? df_get(b.p, b.type, i)
                                 : (have_fill ? filld : DYN_NAN);
            if (dfb_add(ctx, &B, df->cols[c].name, DF_F64, out, n)) {
                out = NULL;
                free(rows);
                dfb_free(ctx, &B);
                return JS_EXCEPTION;
            }
        }
    }
    if (fills)
        JS_FreeCString(ctx, fills);
    free(rows);
    res = dfb_seal(ctx, &B);
    return res;
fail:
    if (fills)
        JS_FreeCString(ctx, fills);
    free(rows);
    dfb_free(ctx, &B);
    return JS_EXCEPTION;
}

enum { DFJ_INNER,
    DFJ_LEFT,
    DFJ_RIGHT,
    DFJ_OUTER };

#define DFJ_OUT_BUDGET ((uint64_t)1024 * 1024 * 1024)

typedef struct {
    uint64_t* keys;
    uint32_t* heads;
    uint32_t* next;
    uint32_t* knext;
    uint32_t* tail;
    uint32_t* row;
    uint32_t* len;
    uint32_t nslots, mask, nent, cap;
} DfJoinIndex;

static void dfj_free(DfJoinIndex* j)
{
    free(j->keys);
    free(j->heads);
    free(j->next);
    free(j->knext);
    free(j->tail);
    free(j->row);
    free(j->len);
    memset(j, 0, sizeof(*j));
}

static int dfj_put(JSContext* ctx, DfJoinIndex* j, uint64_t key, uint32_t row)
{
    uint32_t p, e;

    if (j->nent == j->cap) {
        uint32_t ncap = j->cap ? j->cap << 1 : 64;
        uint64_t* nk = realloc(j->keys, (size_t)ncap * sizeof(*nk));
        uint32_t* nn = realloc(j->next, (size_t)ncap * sizeof(*nn));
        uint32_t* nk2 = realloc(j->knext, (size_t)ncap * sizeof(*nk2));
        uint32_t* nt = realloc(j->tail, (size_t)ncap * sizeof(*nt));
        uint32_t* nr = realloc(j->row, (size_t)ncap * sizeof(*nr));
        uint32_t* nl = realloc(j->len, (size_t)ncap * sizeof(*nl));
        if (nk)
            j->keys = nk;
        if (nn)
            j->next = nn;
        if (nk2)
            j->knext = nk2;
        if (nt)
            j->tail = nt;
        if (nr)
            j->row = nr;
        if (nl)
            j->len = nl;
        if (!nk || !nn || !nk2 || !nt || !nr || !nl) {
            JS_ThrowOutOfMemory(ctx);
            return -1;
        }
        j->cap = ncap;
    }
    if (j->nent * 2 >= j->nslots) {
        uint32_t nslots = j->nslots ? j->nslots << 1 : 64, i;
        uint32_t* heads = malloc((size_t)nslots * sizeof(*heads));
        if (!heads) {
            JS_ThrowOutOfMemory(ctx);
            return -1;
        }
        memset(heads, 0xFF, (size_t)nslots * sizeof(*heads));
        for (i = j->nent; i-- > 0;) {
            uint32_t q = dfc_hash(j->keys[i], 0) & (nslots - 1);
            j->next[i] = (heads[q] == 0xFFFFFFFFu) ? 0xFFFFFFFFu : heads[q];
            heads[q] = i;
        }
        free(j->heads);
        j->heads = heads;
        j->nslots = nslots;
        j->mask = nslots - 1;
    }
    p = dfc_hash(key, 0) & j->mask;
    e = j->heads[p];
    while (e != 0xFFFFFFFFu) {
        if (j->keys[e] == key) {
            uint32_t last = j->tail[e];
            j->knext[last] = j->nent;
            j->tail[e] = j->nent;
            j->knext[j->nent] = 0xFFFFFFFFu;
            j->row[j->nent] = row;
            j->keys[j->nent] = key;
            j->len[e]++;
            j->nent++;
            return 0;
        }
        e = j->next[e];
    }
    j->keys[j->nent] = key;
    j->next[j->nent] = j->heads[p];
    j->knext[j->nent] = 0xFFFFFFFFu;
    j->tail[j->nent] = j->nent;
    j->row[j->nent] = row;
    j->len[j->nent] = 1;
    j->heads[p] = j->nent;
    j->nent++;
    return 0;
}

static uint32_t dfj_find(const DfJoinIndex* j, uint64_t key)
{
    uint32_t p, e;

    if (!j->heads)
        return 0xFFFFFFFFu;
    p = dfc_hash(key, 0) & j->mask;
    e = j->heads[p];
    while (e != 0xFFFFFFFFu) {
        if (j->keys[e] == key)
            return e;
        e = j->next[e];
    }
    return 0xFFFFFFFFu;
}

static int dfj_key_col(JSContext* ctx, const DataFrame* df, int idx,
    const char* op, DFBound* b)
{
    if (dyn_df_bind(ctx, df, idx, b))
        return -1;
    if (b->type == DF_STR || b->type == DF_F64 || b->type == DF_F32) {
        JS_ThrowTypeError(ctx, "%s: key column '%s' is %s; a join key must be "
                               "an integer column",
            op, df->cols[idx].name, df_type_name(b->type));
        return -1;
    }
    return 0;
}

static JSValue dyn_df_join(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char* op = "JOIN";
    DataFrame *df, *other;
    DFBound bl, br;
    DfJoinIndex ridx = { 0 }, lidx = { 0 };
    uint32_t *lrows = NULL, *rrows = NULL, *matched = NULL;
    uint64_t nout = 0, rowcap;
    uint32_t i, lr, rr, e;
    int li2, ri2;
    int how = DFJ_INNER;
    DFBuilder B = { 0 };
    JSValue res;
    uint32_t ncols, c;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    other = dyn_plain_get(ctx, argc > 0 ? argv[0] : JS_UNDEFINED,
        dyn_df_class_id);
    if (!other)
        return JS_EXCEPTION;
    li2 = df_col_arg(ctx, df, argc > 1 ? argv[1] : JS_UNDEFINED);
    if (li2 < 0)
        return JS_EXCEPTION;
    ri2 = df_col_arg(ctx, other, argc > 2 ? argv[2] : JS_UNDEFINED);
    if (ri2 < 0)
        return JS_EXCEPTION;
    if (argc > 3 && !JS_IsUndefined(argv[3])) {
        const char* hows = JS_ToCString(ctx, argv[3]);
        if (!hows)
            return JS_EXCEPTION;
        if (!strcmp(hows, "inner"))
            how = DFJ_INNER;
        else if (!strcmp(hows, "left"))
            how = DFJ_LEFT;
        else if (!strcmp(hows, "right"))
            how = DFJ_RIGHT;
        else if (!strcmp(hows, "outer"))
            how = DFJ_OUTER;
        else {
            JS_FreeCString(ctx, hows);
            return JS_ThrowRangeError(ctx, "%s(..., how): how must be one of "
                                           "inner/left/right/outer",
                op);
        }
        JS_FreeCString(ctx, hows);
    }
    if (dfj_key_col(ctx, df, (int)li2, op, &bl))
        return JS_EXCEPTION;
    if (dfj_key_col(ctx, other, (int)ri2, op, &br))
        return JS_EXCEPTION;

    for (rr = 0; rr < br.n; rr++)
        if (dfj_put(ctx, &ridx, dfc_key(df_get(br.p, br.type, rr)), rr))
            goto fail;
    if (how == DFJ_RIGHT || how == DFJ_OUTER)
        for (lr = 0; lr < bl.n; lr++)
            if (dfj_put(ctx, &lidx, dfc_key(df_get(bl.p, bl.type, lr)), lr))
                goto fail;
    if (how == DFJ_OUTER) {
        matched = calloc(br.n ? br.n : 1, sizeof(uint32_t));
        if (!matched) {
            JS_ThrowOutOfMemory(ctx);
            goto fail;
        }
    }

    ncols = df->ncols + other->ncols + (how == DFJ_OUTER ? 1 : 0);
    rowcap = DFJ_OUT_BUDGET / (8 + (uint64_t)ncols * 8);
    if (rowcap > (uint64_t)DFB_MAX_ROWS)
        rowcap = (uint64_t)DFB_MAX_ROWS;
    {
        uint64_t cap = rowcap;
        nout = 0;
        if (how == DFJ_INNER || how == DFJ_LEFT || how == DFJ_OUTER) {
            for (lr = 0; lr < bl.n; lr++) {
                e = dfj_find(&ridx, dfc_key(df_get(bl.p, bl.type, lr)));
                nout += (e == 0xFFFFFFFFu) ? (uint32_t)(how != DFJ_INNER)
                                           : ridx.len[e];
                if (nout > cap)
                    goto too_many;
            }
        }
        if (how == DFJ_OUTER) {
            for (rr = 0; rr < br.n; rr++)
                if (dfj_find(&lidx, dfc_key(df_get(br.p, br.type, rr)))
                    == 0xFFFFFFFFu) {
                    nout++;
                    if (nout > cap)
                        goto too_many;
                }
        }
        if (how == DFJ_RIGHT) {
            for (rr = 0; rr < br.n; rr++) {
                e = dfj_find(&lidx, dfc_key(df_get(br.p, br.type, rr)));
                nout += (e == 0xFFFFFFFFu) ? 1u : lidx.len[e];
                if (nout > cap)
                    goto too_many;
            }
        }
    }
    goto counted;
too_many:
    JS_ThrowRangeError(ctx, "%s: result would have more than %llu rows "
                            "(%llu columns, about %llu MB); the join key has too "
                            "many matches",
        op, (unsigned long long)rowcap,
        (unsigned long long)ncols,
        (unsigned long long)(DFJ_OUT_BUDGET / (1024 * 1024)));
    goto fail;
counted:;
    lrows = df_out_alloc(ctx, (uint32_t)(nout ? nout : 1), sizeof(uint32_t));
    rrows = df_out_alloc(ctx, (uint32_t)(nout ? nout : 1), sizeof(uint32_t));
    if (!lrows || !rrows)
        goto fail;

    i = 0;
    if (how == DFJ_INNER || how == DFJ_LEFT || how == DFJ_OUTER) {
        for (lr = 0; lr < bl.n; lr++) {
            uint64_t k = dfc_key(df_get(bl.p, bl.type, lr));
            e = dfj_find(&ridx, k);
            if (e == 0xFFFFFFFFu) {
                if (how != DFJ_INNER) {
                    lrows[i] = lr;
                    rrows[i] = DFB_NO_ROW;
                    i++;
                }
            } else {
                for (e = dfj_find(&ridx, k); e != 0xFFFFFFFFu; e = ridx.knext[e]) {
                    lrows[i] = lr;
                    rrows[i] = ridx.row[e];
                    if (matched)
                        matched[ridx.row[e]] = 1;
                    i++;
                }
            }
        }
    }
    if (how == DFJ_OUTER) {
        for (rr = 0; rr < br.n; rr++)
            if (!matched[rr]) {
                lrows[i] = DFB_NO_ROW;
                rrows[i] = rr;
                i++;
            }
    }
    if (how == DFJ_RIGHT) {
        for (rr = 0; rr < br.n; rr++) {
            uint64_t k = dfc_key(df_get(br.p, br.type, rr));
            e = dfj_find(&lidx, k);
            if (e == 0xFFFFFFFFu) {
                lrows[i] = DFB_NO_ROW;
                rrows[i] = rr;
                i++;
            } else {
                for (e = dfj_find(&lidx, k); e != 0xFFFFFFFFu; e = lidx.knext[e]) {
                    lrows[i] = lidx.row[e];
                    rrows[i] = rr;
                    i++;
                }
            }
        }
    }
    (void)i;

    if (dfb_init(ctx, &B, op, ncols, nout))
        goto fail;
    {
        int left_miss = (how == DFJ_RIGHT || how == DFJ_OUTER);
        int right_miss = (how == DFJ_LEFT || how == DFJ_OUTER);
        for (c = 0; c < df->ncols; c++) {
            DFType ot = df->cols[c].type;
            if (left_miss) {
                if (ot == DF_STR) {
                    JS_ThrowRangeError(ctx, "%s: left column '%s' is a string "
                                            "column and a %s join can leave a row "
                                            "without a left value",
                        op, df->cols[c].name,
                        how == DFJ_OUTER ? "outer" : "right");
                    goto fail;
                }
                ot = DF_F64;
            }
            if (dfb_add_gather(ctx, &B, df->cols[c].name, df, (int)c, ot,
                    lrows, (uint32_t)nout))
                goto fail;
        }
        for (c = 0; c < other->ncols; c++) {
            char name[64];
            const char* nm = other->cols[c].name;
            DFType ot = other->cols[c].type;
            if (df_find_col(df, nm) >= 0) {
                int nw = snprintf(name, sizeof(name), "%s_right", nm);
                if (nw < 0 || (size_t)nw >= sizeof(name)) {
                    JS_ThrowRangeError(ctx, "%s: renamed column '%s_right' "
                                            "exceeds the %d character name limit",
                        op, nm, (int)sizeof(name) - 1);
                    goto fail;
                }
                if (df_find_col(df, name) >= 0) {
                    JS_ThrowRangeError(ctx, "%s: right column '%s' collides "
                                            "with the carried column '%s_right'",
                        op, nm, nm);
                    goto fail;
                }
                nm = name;
            }
            if (right_miss) {
                if (ot == DF_STR) {
                    JS_ThrowRangeError(ctx, "%s: right column '%s' is a string "
                                            "column and a %s join can leave a row "
                                            "without a right value",
                        op, other->cols[c].name,
                        how == DFJ_OUTER ? "outer" : "left");
                    goto fail;
                }
                ot = DF_F64;
            }
            if (dfb_add_gather(ctx, &B, nm, other, (int)c, ot, rrows,
                    (uint32_t)nout))
                goto fail;
        }
        if (how == DFJ_OUTER) {
            uint8_t* m = df_out_alloc(ctx, (uint32_t)(nout ? nout : 1), 1);
            uint32_t k;
            if (!m)
                goto fail;
            for (k = 0; k < nout; k++)
                m[k] = (rrows[k] != DFB_NO_ROW) ? 1 : 0;
            if (dfb_add(ctx, &B, "matched", DF_U8, m, (uint32_t)nout)) {
                m = NULL;
                goto fail;
            }
        }
    }
    free(lrows);
    free(rrows);
    free(matched);
    dfj_free(&ridx);
    dfj_free(&lidx);
    res = dfb_seal(ctx, &B);
    return res;
fail:
    free(lrows);
    free(rrows);
    free(matched);
    dfj_free(&ridx);
    dfj_free(&lidx);
    dfb_free(ctx, &B);
    return JS_EXCEPTION;
}

static JSValue dyn_df_asof_join(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char* op = "ASOF_JOIN";
    DataFrame *df, *other;
    DFBound bl, br;
    uint32_t *lrows = NULL, *rrows = NULL;
    uint32_t n, i, lr, rr, rptr = 0;
    uint32_t c;
    int li, ri;
    DFBuilder B = { 0 };
    JSValue res;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    other = dyn_plain_get(ctx, argc > 0 ? argv[0] : JS_UNDEFINED,
        dyn_df_class_id);
    if (!other)
        return JS_EXCEPTION;
    li = df_col_arg(ctx, df, argc > 1 ? argv[1] : JS_UNDEFINED);
    if (li < 0)
        return JS_EXCEPTION;
    ri = df_col_arg(ctx, other, argc > 2 ? argv[2] : JS_UNDEFINED);
    if (ri < 0)
        return JS_EXCEPTION;
    if (dfj_key_col(ctx, df, li, op, &bl))
        return JS_EXCEPTION;
    if (dfj_key_col(ctx, other, ri, op, &br))
        return JS_EXCEPTION;

    for (i = 1; i < bl.n; i++)
        if (df_get(bl.p, bl.type, i) < df_get(bl.p, bl.type, i - 1))
            return JS_ThrowRangeError(ctx, "%s: left time column must be sorted "
                                           "ascending",
                op);
    for (i = 1; i < br.n; i++)
        if (df_get(br.p, br.type, i) < df_get(br.p, br.type, i - 1))
            return JS_ThrowRangeError(ctx, "%s: right time column must be "
                                           "sorted ascending",
                op);
    for (i = 0; i < bl.n; i++)
        if (df_get(bl.p, bl.type, i) != df_get(bl.p, bl.type, i))
            return JS_ThrowRangeError(ctx, "%s: left time column must not "
                                           "contain NaN",
                op);
    for (i = 0; i < br.n; i++)
        if (df_get(br.p, br.type, i) != df_get(br.p, br.type, i))
            return JS_ThrowRangeError(ctx, "%s: right time column must not "
                                           "contain NaN",
                op);

    n = bl.n;
    lrows = df_out_alloc(ctx, n ? n : 1, sizeof(uint32_t));
    rrows = df_out_alloc(ctx, n ? n : 1, sizeof(uint32_t));
    if (!lrows || !rrows)
        goto fail;

    for (lr = 0; lr < n; lr++) {
        double lt = df_get(bl.p, bl.type, lr);
        while (rptr + 1 < br.n && df_get(br.p, br.type, rptr + 1) <= lt)
            rptr++;
        lrows[lr] = lr;
        rrows[lr] = (br.n && df_get(br.p, br.type, rptr) <= lt) ? rptr
                                                                : DFB_NO_ROW;
    }
    (void)rr;

    {
        uint32_t ncols = df->ncols + other->ncols;
        if (dfb_init(ctx, &B, op, ncols, n))
            goto fail;
        for (c = 0; c < df->ncols; c++)
            if (dfb_add_gather(ctx, &B, df->cols[c].name, df, (int)c,
                    df->cols[c].type, lrows, n))
                goto fail;
        for (c = 0; c < other->ncols; c++) {
            char name[64];
            const char* nm = other->cols[c].name;
            DFType ot = other->cols[c].type;
            if (df_find_col(df, nm) >= 0) {
                int nw = snprintf(name, sizeof(name), "%s_right", nm);
                if (nw < 0 || (size_t)nw >= sizeof(name)) {
                    JS_ThrowRangeError(ctx, "%s: renamed column '%s_right' "
                                            "exceeds the %d character name limit",
                        op, nm, (int)sizeof(name) - 1);
                    goto fail;
                }
                nm = name;
            }
            if (ot == DF_STR) {
                JS_ThrowRangeError(ctx, "%s: right column '%s' is a string "
                                        "column and a left row may have no preceding "
                                        "right row",
                    op, other->cols[c].name);
                goto fail;
            }
            ot = DF_F64;
            if (dfb_add_gather(ctx, &B, nm, other, (int)c, ot, rrows, n))
                goto fail;
        }
    }
    free(lrows);
    free(rrows);
    res = dfb_seal(ctx, &B);
    return res;
fail:
    free(lrows);
    free(rrows);
    dfb_free(ctx, &B);
    return JS_EXCEPTION;
}

static JSValue dyn_df_concat(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char* op = "CONCAT";
    DataFrame *df, *other;
    DFBuilder B = { 0 };
    uint32_t c, i;
    uint64_t nout;
    JSValue res;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    other = dyn_plain_get(ctx, argc > 0 ? argv[0] : JS_UNDEFINED,
        dyn_df_class_id);
    if (!other)
        return JS_EXCEPTION;
    if (df->ncols != other->ncols)
        return JS_ThrowRangeError(ctx, "%s: column count mismatch (%u vs %u)",
            op, df->ncols, other->ncols);
    for (c = 0; c < df->ncols; c++)
        if (strcmp(df->cols[c].name, other->cols[c].name) != 0)
            return JS_ThrowRangeError(ctx, "%s: column %u is '%s' on the left "
                                           "and '%s' on the right; the sets must "
                                           "match in order",
                op, c, df->cols[c].name,
                other->cols[c].name);
    nout = (uint64_t)df->nrows + other->nrows;
    if (dfb_init(ctx, &B, op, df->ncols, nout))
        return JS_EXCEPTION;
    {
        uint32_t n = (uint32_t)nout;

        for (c = 0; c < df->ncols; c++) {
            DFType lt = df->cols[c].type, rt = other->cols[c].type;
            if (lt == DF_STR && rt == DF_STR) {
                DfStrTab tab = { 0 };
                int32_t* codes = df_out_alloc(ctx, n ? n : 1, sizeof(int32_t));
                int32_t* remap;
                uint32_t d;
                DFBound bl, br;
                if (!codes)
                    goto fail;
                if (dyn_df_bind(ctx, df, (int)c, &bl) || dyn_df_bind(ctx, other, (int)c, &br)) {
                    free(codes);
                    goto fail;
                }
                for (d = 0; d < df->cols[c].dict_len; d++) {
                    uint32_t code;
                    if (df_strtab_put(ctx, &tab, df->cols[c].dict[d], &code)) {
                        free(codes);
                        df_strtab_free(&tab);
                        goto fail;
                    }
                }
                remap = malloc((size_t)(other->cols[c].dict_len
                                       ? other->cols[c].dict_len
                                       : 1)
                    * sizeof(int32_t));
                if (!remap) {
                    free(codes);
                    df_strtab_free(&tab);
                    JS_ThrowOutOfMemory(ctx);
                    goto fail;
                }
                for (d = 0; d < other->cols[c].dict_len; d++) {
                    uint32_t code;
                    if (df_strtab_put(ctx, &tab, other->cols[c].dict[d], &code)) {
                        free(codes);
                        free(remap);
                        df_strtab_free(&tab);
                        goto fail;
                    }
                    remap[d] = (int32_t)code;
                }
                for (i = 0; i < df->nrows; i++)
                    codes[i] = ((const int32_t*)bl.p)[i];
                for (i = 0; i < other->nrows; i++)
                    codes[df->nrows + i] = remap[((const int32_t*)br.p)[i]];
                free(remap);
                if (dfb_add_str(ctx, &B, df->cols[c].name, codes, tab.dict,
                        tab.dict_len, n)) {
                    tab.dict = NULL;
                    tab.dict_len = 0;
                    df_strtab_free(&tab);
                    goto fail;
                }
                tab.dict = NULL;
                tab.dict_len = 0;
                df_strtab_free(&tab);
            } else if (lt != DF_STR && rt != DF_STR) {
                DFType outt = (lt == rt) ? lt : DF_F64;
                DFBound bl, br;
                void* out;
                if (dyn_df_bind(ctx, df, (int)c, &bl) || dyn_df_bind(ctx, other, (int)c, &br))
                    goto fail;
                out = df_out_alloc(ctx, n ? n : 1, df_elt_size(outt));
                if (!out)
                    goto fail;
                if (outt == DF_F64) {
                    double* d = out;
                    for (i = 0; i < df->nrows; i++)
                        d[i] = df_get(bl.p, bl.type, i);
                    for (i = 0; i < other->nrows; i++)
                        d[df->nrows + i] = df_get(br.p, br.type, i);
                } else {
                    memcpy(out, bl.p, (size_t)df->nrows * df_elt_size(outt));
                    memcpy((uint8_t*)out + (size_t)df->nrows * df_elt_size(outt),
                        br.p, (size_t)other->nrows * df_elt_size(outt));
                }
                if (dfb_add(ctx, &B, df->cols[c].name, outt, out, n)) {
                    out = NULL;
                    goto fail;
                }
            } else {
                JS_ThrowRangeError(ctx, "%s: column '%s' is %s on the left and %s "
                                        "on the right; a string column cannot meet a "
                                        "numeric one",
                    op, df->cols[c].name,
                    df_type_name(lt), df_type_name(rt));
                goto fail;
            }
        }
    }
    res = dfb_seal(ctx, &B);
    return res;
fail:
    dfb_free(ctx, &B);
    return JS_EXCEPTION;
}

enum { DFR_SUM,
    DFR_MEAN,
    DFR_MIN,
    DFR_MAX,
    DFR_COUNT };

static int dfr_agg(const char* s)
{
    if (!strcmp(s, "sum"))
        return DFR_SUM;
    if (!strcmp(s, "mean"))
        return DFR_MEAN;
    if (!strcmp(s, "min"))
        return DFR_MIN;
    if (!strcmp(s, "max"))
        return DFR_MAX;
    if (!strcmp(s, "count"))
        return DFR_COUNT;
    return -1;
}

static JSValue dyn_df_resample(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char* op = "RESAMPLE";
    DataFrame* df;
    DFBound b;
    double *times = NULL, *bstart = NULL, *vals = NULL;
    double interval, t0;
    uint32_t* kidx = NULL;
    uint32_t i, n, nb = 0;
    int ti, agg = DFR_SUM;
    const char* aggs = NULL;
    DFBuilder B = { 0 };
    JSValue res;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    ti = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (ti < 0)
        return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &interval, argc > 1 ? argv[1] : JS_UNDEFINED))
        return JS_EXCEPTION;
    if (!(interval > 0.0) || !isfinite(interval))
        return JS_ThrowRangeError(ctx, "%s(time, interval[, agg]): interval "
                                       "must be finite and positive, got %g",
            op, interval);
    if (argc > 2 && !JS_IsUndefined(argv[2])) {
        aggs = JS_ToCString(ctx, argv[2]);
        if (!aggs)
            return JS_EXCEPTION;
        agg = dfr_agg(aggs);
        JS_FreeCString(ctx, aggs);
        if (agg < 0)
            return JS_ThrowRangeError(ctx, "%s(..., agg): agg must be one of "
                                           "sum/mean/min/max/count",
                op);
    }
    if (df_bind_numeric(ctx, df, ti, &b, op))
        return JS_EXCEPTION;

    n = b.n < df->nrows ? b.n : df->nrows;
    times = df_out_alloc(ctx, n ? n : 1, sizeof(double));
    if (!times)
        return JS_EXCEPTION;
    for (i = 0; i < n; i++)
        times[i] = df_get(b.p, b.type, i);
    for (i = 1; i < n; i++)
        if (times[i] < times[i - 1]) {
            free(times);
            return JS_ThrowRangeError(ctx, "%s: the time column must be sorted "
                                           "ascending",
                op);
        }
    for (i = 0; i < n; i++)
        if (times[i] != times[i]) {
            free(times);
            return JS_ThrowRangeError(ctx, "%s: the time column must not "
                                           "contain NaN",
                op);
        }
    if (n == 0) {
        double* e1 = df_out_alloc(ctx, 1, sizeof(double));
        double* e2 = df_out_alloc(ctx, 1, sizeof(double));
        if (!e1 || !e2) {
            free(e1);
            free(e2);
            goto fail;
        }
        if (dfb_init(ctx, &B, op, 2, 0)) {
            free(e1);
            free(e2);
            goto fail;
        }
        if (dfb_add(ctx, &B, "bucket", DF_F64, e1, 0)) {
            e1 = NULL;
            goto fail;
        }
        if (dfb_add(ctx, &B, "value", DF_F64, e2, 0)) {
            e2 = NULL;
            goto fail;
        }
        res = dfb_seal(ctx, &B);
        free(times);
        return res;
    }
    t0 = floor(times[0] / interval) * interval;

    kidx = df_out_alloc(ctx, n, sizeof(uint32_t));
    if (!kidx)
        goto fail;
    {
        double* bk = df_out_alloc(ctx, n, sizeof(double));
        if (!bk)
            goto fail;
        nb = 0;
        for (i = 0; i < n; i++) {
            double s = t0 + floor((times[i] - t0) / interval) * interval;
            if (nb == 0 || s != bk[nb - 1]) {
                bk[nb] = s;
                nb++;
            }
            kidx[i] = nb - 1;
        }
        bstart = malloc((size_t)(nb ? nb : 1) * sizeof(double));
        if (!bstart) {
            free(bk);
            JS_ThrowOutOfMemory(ctx);
            goto fail;
        }
        memcpy(bstart, bk, (size_t)nb * sizeof(double));
        free(bk);
    }

    vals = calloc((size_t)(nb ? nb : 1), sizeof(double));
    if (!vals)
        goto fail;
    if (agg == DFR_MIN)
        for (i = 0; i < nb; i++)
            vals[i] = DYN_INFINITY;
    if (agg == DFR_MAX)
        for (i = 0; i < nb; i++)
            vals[i] = -DYN_INFINITY;
    for (i = 0; i < n; i++) {
        double v = df_get(b.p, b.type, i);
        uint32_t k = kidx[i];
        switch (agg) {
        case DFR_SUM:
            vals[k] += v;
            break;
        case DFR_MEAN:
            vals[k] += v;
            break;
        case DFR_MIN:
            if (v == v && v < vals[k])
                vals[k] = v;
            break;
        case DFR_MAX:
            if (v == v && v > vals[k])
                vals[k] = v;
            break;
        case DFR_COUNT:
            vals[k] += 1.0;
            break;
        }
    }
    if (agg == DFR_MIN || agg == DFR_MAX) {
        double* cnt = calloc((size_t)(nb ? nb : 1), sizeof(double));
        if (!cnt)
            goto fail;
        for (i = 0; i < n; i++)
            if (df_get(b.p, b.type, i) == df_get(b.p, b.type, i))
                cnt[kidx[i]] += 1.0;
        for (i = 0; i < nb; i++)
            if (cnt[i] == 0)
                vals[i] = DYN_NAN;
        free(cnt);
    }
    if (agg == DFR_MEAN) {
        double* cnt = calloc((size_t)(nb ? nb : 1), sizeof(double));
        if (!cnt)
            goto fail;
        for (i = 0; i < n; i++)
            cnt[kidx[i]] += 1.0;
        for (i = 0; i < nb; i++)
            vals[i] = cnt[i] ? vals[i] / cnt[i] : DYN_NAN;
        free(cnt);
    }
    for (i = 0; i < n; i++) {
        double v = df_get(b.p, b.type, i);
        if (v != v && (agg == DFR_SUM || agg == DFR_MEAN))
            vals[kidx[i]] = DYN_NAN;
    }

    if (dfb_init(ctx, &B, op, 2, nb))
        goto fail;
    if (dfb_add(ctx, &B, "bucket", DF_F64, bstart, nb)) {
        bstart = NULL;
        goto fail;
    }
    if (dfb_add(ctx, &B, "value", DF_F64, vals, nb)) {
        vals = NULL;
        goto fail;
    }
    free(times);
    free(kidx);
    res = dfb_seal(ctx, &B);
    return res;
fail:
    free(times);
    free(bstart);
    free(vals);
    free(kidx);
    dfb_free(ctx, &B);
    return JS_EXCEPTION;
}

#define DFP_CELL_BUDGET ((uint64_t)256 * 1024 * 1024)

static JSValue dyn_df_pivot(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char* op = "PIVOT";
    DataFrame* df;
    DFBound bi, bc, bv;
    DfcSet iset = { 0 }, pset = { 0 };
    uint32_t* ifirst = NULL;
    uint32_t ni = 0, np = 0, n, i, pe;
    int ii, ic, iv, agg = DFR_SUM;
    const char* aggs = NULL;
    char** pnames = NULL;
    double *acc = NULL, *mn = NULL, *mx = NULL, *fst = NULL, *lst = NULL;
    uint32_t* cnt = NULL;
    DFBuilder B = { 0 };
    JSValue res;
    int do_acc, do_cnt, do_mn, do_mx, do_fl;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    ii = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (ii < 0)
        return JS_EXCEPTION;
    ic = df_col_arg(ctx, df, argc > 1 ? argv[1] : JS_UNDEFINED);
    if (ic < 0)
        return JS_EXCEPTION;
    iv = df_col_arg(ctx, df, argc > 2 ? argv[2] : JS_UNDEFINED);
    if (iv < 0)
        return JS_EXCEPTION;
    if (argc > 3 && !JS_IsUndefined(argv[3])) {
        aggs = JS_ToCString(ctx, argv[3]);
        if (!aggs)
            return JS_EXCEPTION;
        if (!strcmp(aggs, "sum"))
            agg = DFR_SUM;
        else if (!strcmp(aggs, "mean"))
            agg = DFR_MEAN;
        else if (!strcmp(aggs, "min"))
            agg = DFR_MIN;
        else if (!strcmp(aggs, "max"))
            agg = DFR_MAX;
        else if (!strcmp(aggs, "count"))
            agg = DFR_COUNT;
        else if (!strcmp(aggs, "first"))
            agg = 5;
        else if (!strcmp(aggs, "last"))
            agg = 6;
        else {
            JS_FreeCString(ctx, aggs);
            return JS_ThrowRangeError(ctx, "%s(..., agg): agg must be one of "
                                           "sum/mean/min/max/count/first/last",
                op);
        }
        JS_FreeCString(ctx, aggs);
    }
    if (df_bind_numeric(ctx, df, iv, &bv, op))
        return JS_EXCEPTION;
    if (dyn_df_bind(ctx, df, ii, &bi))
        return JS_EXCEPTION;
    if (dyn_df_bind(ctx, df, ic, &bc))
        return JS_EXCEPTION;

    n = bi.n < bc.n ? bi.n : bc.n;
    if (bv.n < n)
        n = bv.n;
    if (n > df->nrows)
        n = df->nrows;

    ifirst = df_out_alloc(ctx, (uint32_t)(n ? n : 1), sizeof(uint32_t));
    if (!ifirst)
        goto fail;
    for (i = 0; i < n; i++) {
        uint64_t ikey, pkey;
        uint32_t ie, pe_idx;
        int fr;
        if (bi.type == DF_STR)
            ikey = dfc_key((double)((const int32_t*)bi.p)[i]);
        else
            ikey = dfc_key(df_get(bi.p, bi.type, i));
        if (bc.type == DF_STR)
            pkey = dfc_key((double)((const int32_t*)bc.p)[i]);
        else
            pkey = dfc_key(df_get(bc.p, bc.type, i));
        if (dfc_set_put(ctx, &iset, ikey, 0, &ie, &fr))
            goto fail;
        if (fr) {
            ifirst[ie] = i;
            ni++;
        }
        if (dfc_set_put(ctx, &pset, pkey, 0, &pe_idx, &fr))
            goto fail;
        if (fr)
            np++;
    }
    if (np + 1 > DF_MAX_COLS) {
        JS_ThrowRangeError(ctx, "%s: result would be %u rows x %u columns; "
                                "the max is %u rows and %d columns (DF_MAX_COLS)",
            op, ni, np + 1, DFB_MAX_ROWS, DF_MAX_COLS);
        goto fail;
    }
    {
        uint64_t cells = (uint64_t)ni * (uint64_t)np;
        uint64_t per = 4;
        if (agg == DFR_SUM || agg == DFR_MEAN || agg == DFR_MIN || agg == DFR_MAX)
            per += 8;
        if (agg == 5 || agg == 6)
            per += 16;
        if (cells > DFP_CELL_BUDGET / per) {
            JS_ThrowRangeError(ctx, "%s: %u index values x %u pivot values "
                                    "needs %llu accumulator slots (about %llu MB); "
                                    "the max for this agg is %llu slots (%llu MB). "
                                    "Fewer distinct index values, or GROUP_BY first.",
                op, ni, np, (unsigned long long)cells,
                (unsigned long long)(cells * per / (1024 * 1024)),
                (unsigned long long)(DFP_CELL_BUDGET / per),
                (unsigned long long)(DFP_CELL_BUDGET / (1024 * 1024)));
            goto fail;
        }
    }
    pnames = calloc((size_t)(np ? np : 1), sizeof(char*));
    if (!pnames) {
        JS_ThrowOutOfMemory(ctx);
        goto fail;
    }
    for (pe = 0; pe < np; pe++) {
        uint64_t bits = pset.keys[pe];
        char tmp[32];
        if (bc.type == DF_STR) {
            uint32_t code = (uint32_t)dfc_key_value(bits);
            if (code >= df->cols[ic].dict_len) {
                JS_ThrowRangeError(ctx, "%s: pivot code %u out of range", op,
                    code);
                goto fail;
            }
            pnames[pe] = strdup(df->cols[ic].dict[code]);
        } else {
            df_fmt_double(tmp, sizeof(tmp), dfc_key_value(bits));
            pnames[pe] = strdup(tmp);
        }
        if (!pnames[pe]) {
            JS_ThrowOutOfMemory(ctx);
            goto fail;
        }
        if (strcmp(pnames[pe], df->cols[ii].name) == 0) {
            JS_ThrowRangeError(ctx, "%s: pivot value '%s' collides with the "
                                    "index column name",
                op, pnames[pe]);
            goto fail;
        }
    }

    do_acc = (agg == DFR_SUM || agg == DFR_MEAN);
    do_cnt = 1;
    do_mn = (agg == DFR_MIN);
    do_mx = (agg == DFR_MAX);
    do_fl = (agg == 5 || agg == 6);
    if (do_acc) {
        acc = calloc((size_t)ni * np, sizeof(double));
        if (!acc) {
            JS_ThrowOutOfMemory(ctx);
            goto fail;
        }
    }
    cnt = calloc((size_t)ni * np, sizeof(uint32_t));
    if (!cnt) {
        JS_ThrowOutOfMemory(ctx);
        goto fail;
    }
    if (do_mn) {
        mn = malloc((size_t)ni * np * sizeof(double));
        if (!mn) {
            JS_ThrowOutOfMemory(ctx);
            goto fail;
        }
        for (i = 0; i < (uint64_t)ni * np; i++)
            mn[i] = DYN_INFINITY;
    }
    if (do_mx) {
        mx = malloc((size_t)ni * np * sizeof(double));
        if (!mx) {
            JS_ThrowOutOfMemory(ctx);
            goto fail;
        }
        for (i = 0; i < (uint64_t)ni * np; i++)
            mx[i] = -DYN_INFINITY;
    }
    if (do_fl) {
        fst = malloc((size_t)ni * np * sizeof(double));
        lst = malloc((size_t)ni * np * sizeof(double));
        if (!fst || !lst) {
            JS_ThrowOutOfMemory(ctx);
            goto fail;
        }
        for (i = 0; i < (uint64_t)ni * np; i++) {
            fst[i] = DYN_NAN;
            lst[i] = DYN_NAN;
        }
    }

    for (i = 0; i < n; i++) {
        uint64_t ikey, pkey;
        uint32_t ie, pe_idx;
        double v = df_get(bv.p, bv.type, i);
        if (bi.type == DF_STR)
            ikey = dfc_key((double)((const int32_t*)bi.p)[i]);
        else
            ikey = dfc_key(df_get(bi.p, bi.type, i));
        if (bc.type == DF_STR)
            pkey = dfc_key((double)((const int32_t*)bc.p)[i]);
        else
            pkey = dfc_key(df_get(bc.p, bc.type, i));
        if (dfc_set_find(&iset, ikey, 0, &ie) || dfc_set_find(&pset, pkey, 0, &pe_idx))
            continue;
        {
            uint64_t cell = (uint64_t)ie * np + pe_idx;
            if (do_acc)
                acc[cell] += v;
            if (do_cnt)
                cnt[cell]++;
            if (do_mn && v == v && v < mn[cell])
                mn[cell] = v;
            if (do_mx && v == v && v > mx[cell])
                mx[cell] = v;
            if (do_fl) {
                if (v == v && fst[cell] != fst[cell])
                    fst[cell] = v;
                if (v == v)
                    lst[cell] = v;
            }
        }
    }

    {
        uint32_t* irows = df_out_alloc(ctx, ni ? ni : 1, sizeof(uint32_t));
        uint32_t k, c;
        if (!irows)
            goto fail;
        for (k = 0; k < ni; k++)
            irows[k] = ifirst[k];
        if (dfb_init(ctx, &B, op, np + 1, ni)) {
            free(irows);
            goto fail;
        }
        if (dfb_add_gather(ctx, &B, df->cols[ii].name, df, ii, bi.type,
                irows, ni)) {
            free(irows);
            goto fail;
        }
        free(irows);
        for (pe = 0; pe < np; pe++) {
            double* col = df_out_alloc(ctx, ni ? ni : 1, sizeof(double));
            if (!col)
                goto fail;
            for (k = 0; k < ni; k++) {
                uint64_t cell = (uint64_t)k * np + pe;
                switch (agg) {
                case DFR_SUM:
                    col[k] = cnt[cell] ? acc[cell] : DYN_NAN;
                    break;
                case DFR_MEAN:
                    col[k] = cnt[cell] ? acc[cell] / cnt[cell] : DYN_NAN;
                    break;
                case DFR_MIN:
                    col[k] = (mn[cell] == DYN_INFINITY) ? DYN_NAN : mn[cell];
                    break;
                case DFR_MAX:
                    col[k] = (mx[cell] == -DYN_INFINITY) ? DYN_NAN : mx[cell];
                    break;
                case DFR_COUNT:
                    col[k] = (double)cnt[cell];
                    break;
                default:
                    col[k] = fst[cell];
                    break;
                }
                if (agg == 6)
                    col[k] = lst[cell];
            }
            if (dfb_add(ctx, &B, pnames[pe], DF_F64, col, ni)) {
                col = NULL;
                goto fail;
            }
        }
        (void)c;
    }
    res = dfb_seal(ctx, &B);
    dfc_set_free(&iset);
    dfc_set_free(&pset);
    free(ifirst);
    free(acc);
    free(cnt);
    free(mn);
    free(mx);
    free(fst);
    free(lst);
    for (pe = 0; pe < np; pe++)
        free(pnames[pe]);
    free(pnames);
    return res;
fail:
    dfc_set_free(&iset);
    dfc_set_free(&pset);
    free(ifirst);
    free(acc);
    free(cnt);
    free(mn);
    free(mx);
    free(fst);
    free(lst);
    if (pnames) {
        uint32_t pe_idx;
        for (pe_idx = 0; pe_idx < np; pe_idx++)
            free(pnames[pe_idx]);
        free(pnames);
    }
    dfb_free(ctx, &B);
    return JS_EXCEPTION;
}

static JSValue dyn_df_melt(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char* op = "MELT";
    DataFrame* df;
    DFBound* binds = NULL;
    uint32_t *idxs = NULL, *vvals = NULL, *rows = NULL;
    int32_t* varcodes = NULL;
    uint64_t nout;
    uint32_t i, k, c, nid = 0, nvv = 0, nrows;
    DFBuilder B = { 0 };
    JSValue res;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    if (argc > 0 && !JS_IsUndefined(argv[0]) && !JS_IsNull(argv[0])) {
        if (!JS_IsArray(ctx, argv[0]))
            return JS_ThrowTypeError(ctx, "%s(idVars, valueVars): idVars must "
                                          "be an array of names",
                op);
    }
    if (argc < 2 || !JS_IsArray(ctx, argv[1]))
        return JS_ThrowTypeError(ctx, "%s(idVars, valueVars): valueVars must "
                                      "be an array of names",
            op);
    if (argc > 0 && !JS_IsUndefined(argv[0]) && !JS_IsNull(argv[0])) {
        JSValue lv = JS_GetPropertyStr(ctx, argv[0], "length");
        if (JS_IsException(lv) || JS_ToUint32(ctx, &nid, lv)) {
            JS_FreeValue(ctx, lv);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, lv);
    }
    {
        JSValue lv = JS_GetPropertyStr(ctx, argv[1], "length");
        if (JS_IsException(lv) || JS_ToUint32(ctx, &nvv, lv)) {
            JS_FreeValue(ctx, lv);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, lv);
    }
    if (nvv == 0)
        return JS_ThrowRangeError(ctx, "%s(idVars, valueVars): valueVars "
                                       "cannot be empty",
            op);
    idxs = df_out_alloc(ctx, nid ? nid : 1, sizeof(uint32_t));
    vvals = df_out_alloc(ctx, nvv ? nvv : 1, sizeof(uint32_t));
    if (!idxs || !vvals)
        goto fail;
    for (i = 0; i < nid; i++) {
        JSValue nv = JS_GetPropertyUint32(ctx, argv[0], i);
        int idx;
        if (JS_IsException(nv)) {
            JS_ThrowOutOfMemory(ctx);
            goto fail;
        }
        idx = df_col_arg(ctx, df, nv);
        JS_FreeValue(ctx, nv);
        if (idx < 0)
            goto fail;
        idxs[i] = (uint32_t)idx;
    }
    for (i = 0; i < nvv; i++) {
        JSValue nv = JS_GetPropertyUint32(ctx, argv[1], i);
        int idx;
        if (JS_IsException(nv)) {
            JS_ThrowOutOfMemory(ctx);
            goto fail;
        }
        idx = df_col_arg(ctx, df, nv);
        JS_FreeValue(ctx, nv);
        if (idx < 0)
            goto fail;
        if (df->cols[idx].type == DF_STR) {
            JS_ThrowTypeError(ctx, "%s: valueVar '%s' is a string column; "
                                   "melt needs numeric value columns",
                op, df->cols[idx].name);
            goto fail;
        }
        vvals[i] = (uint32_t)idx;
    }
    for (i = 0; i < nid; i++)
        for (k = 0; k < nvv; k++)
            if (idxs[i] == vvals[k]) {
                JS_ThrowRangeError(ctx, "%s: column '%s' is both an idVar and "
                                        "a valueVar",
                    op, df->cols[idxs[i]].name);
                goto fail;
            }

    nrows = df->nrows;
    nout = (uint64_t)nrows * nvv;
    if (nout > (uint64_t)DFB_MAX_ROWS) {
        JS_ThrowRangeError(ctx, "%s: result would have %llu rows (max %u)",
            op, (unsigned long long)nout, DFB_MAX_ROWS);
        goto fail;
    }
    rows = df_out_alloc(ctx, (uint32_t)(nout ? nout : 1), sizeof(uint32_t));
    varcodes = df_out_alloc(ctx, (uint32_t)(nout ? nout : 1), sizeof(int32_t));
    if (!rows || !varcodes)
        goto fail;
    for (i = 0; i < nout; i++) {
        rows[i] = (uint32_t)(i / nvv);
        varcodes[i] = (int32_t)(i % nvv);
    }

    if (dfb_init(ctx, &B, op, nid + 2, nout))
        goto fail;
    for (c = 0; c < nid; c++)
        if (dfb_add_gather(ctx, &B, df->cols[idxs[c]].name, df, (int)idxs[c],
                df->cols[idxs[c]].type, rows, (uint32_t)nout))
            goto fail;
    {
        DfStrTab tab = { 0 };
        int32_t* codes = df_out_alloc(ctx, (uint32_t)(nout ? nout : 1),
            sizeof(int32_t));
        int32_t* poscode = df_out_alloc(ctx, (uint32_t)(nvv ? nvv : 1),
            sizeof(int32_t));
        char** dict;
        uint32_t code;
        if (!codes || !poscode) {
            free(poscode);
            goto fail;
        }
        for (i = 0; i < nvv; i++) {
            if (df_strtab_put(ctx, &tab, df->cols[vvals[i]].name, &code)) {
                free(poscode);
                goto fail;
            }
            poscode[i] = (int32_t)code;
        }
        for (i = 0; i < nout; i++) {
            codes[i] = poscode[varcodes[i]];
        }
        free(poscode);
        dict = tab.dict;
        if (dfb_add_str(ctx, &B, "variable", codes, dict, tab.dict_len,
                (uint32_t)nout)) {
            tab.dict = NULL;
            tab.dict_len = 0;
            df_strtab_free(&tab);
            goto fail;
        }
        tab.dict = NULL;
        tab.dict_len = 0;
        df_strtab_free(&tab);
    }
    {
        double* vals = df_out_alloc(ctx, (uint32_t)(nout ? nout : 1),
            sizeof(double));
        if (!vals)
            goto fail;
        binds = calloc(nvv, sizeof(DFBound));
        if (!binds) {
            free(vals);
            JS_ThrowOutOfMemory(ctx);
            goto fail;
        }
        for (k = 0; k < nvv; k++)
            if (dyn_df_bind(ctx, df, (int)vvals[k], &binds[k])) {
                free(vals);
                goto fail;
            }
        for (i = 0; i < nout; i++)
            vals[i] = df_get(binds[varcodes[i]].p, binds[varcodes[i]].type,
                rows[i]);
        if (dfb_add(ctx, &B, "value", DF_F64, vals, (uint32_t)nout)) {
            vals = NULL;
            goto fail;
        }
    }
    free(binds);
    free(idxs);
    free(vvals);
    free(rows);
    free(varcodes);
    res = dfb_seal(ctx, &B);
    return res;
fail:
    free(binds);
    free(idxs);
    free(vvals);
    free(rows);
    free(varcodes);
    dfb_free(ctx, &B);
    return JS_EXCEPTION;
}

enum { DFJA_AGG,
    DFJA_OBJ,
    DFJA_AGG_STRICT,
    DFJA_OBJ_STRICT };

static JSValue dfja_value(JSContext* ctx, double v, int strict)
{
    if (strict && (v != v || v > 1.0 / 0.0 || v < -1.0 / 0.0))
        return JS_ThrowRangeError(ctx, "JSON_AGG: a non-finite value cannot "
                                       "be represented in JSON");
    return JS_NewFloat64(ctx, v);
}

static JSValue dfja_build(JSContext* ctx, const DataFrame* df, int ki,
    const double* keys, const double* vals,
    const uint8_t* mask, uint32_t n, int obj_form,
    int strict)
{
    DfcSet set = { 0 };
    double** grows = NULL;
    uint32_t *gcnt = NULL, *gcap = NULL;
    uint32_t i, g, ng = 0;
    JSValue res, arr;
    int isstr = (df->cols[ki].type == DF_STR);

    for (i = 0; i < n; i++) {
        uint32_t e;
        int fr;
        if (mask && !mask[i])
            continue;
        if (dfc_set_put(ctx, &set, dfc_key(keys[i]), 0, &e, &fr))
            goto fail;
    }
    ng = set.nent;
    if (ng == 0) {
        dfc_set_free(&set);
        return JS_NewObject(ctx);
    }
    gcnt = calloc(ng, sizeof(uint32_t));
    gcap = calloc(ng, sizeof(uint32_t));
    grows = calloc(ng, sizeof(double*));
    if (!gcnt || !gcap || !grows) {
        JS_ThrowOutOfMemory(ctx);
        goto fail;
    }
    for (i = 0; i < n; i++) {
        uint32_t e;
        if (mask && !mask[i])
            continue;
        if (dfc_set_find(&set, dfc_key(keys[i]), 0, &e))
            continue;
        gcnt[e]++;
    }
    for (g = 0; g < ng; g++) {
        grows[g] = malloc((size_t)(gcnt[g] ? gcnt[g] : 1) * sizeof(double));
        if (!grows[g]) {
            JS_ThrowOutOfMemory(ctx);
            goto fail;
        }
        gcap[g] = gcnt[g];
        gcnt[g] = 0;
    }
    for (i = 0; i < n; i++) {
        uint32_t e;
        double v = vals[i];
        if (mask && !mask[i])
            continue;
        if (dfc_set_find(&set, dfc_key(keys[i]), 0, &e))
            continue;
        if (strict && (v != v || v > 1.0 / 0.0 || v < -1.0 / 0.0)) {
            JS_ThrowRangeError(ctx, "JSON_AGG: a non-finite value cannot be "
                                    "represented in JSON");
            goto fail;
        }
        grows[e][gcnt[e]++] = v;
    }

    res = JS_NewObject(ctx);
    if (JS_IsException(res))
        goto fail;
    if (obj_form) {
        for (i = 0; i < n; i++) {
            JSValue v;
            char kbuf[64];
            const char* kname;
            double kv = keys[i];
            if (mask && !mask[i])
                continue;
            if (isstr) {
                uint32_t code = (uint32_t)kv;
                if (code >= df->cols[ki].dict_len) {
                    JS_ThrowRangeError(ctx, "JSON_OBJECT_AGG: key code %u out "
                                            "of range",
                        code);
                    JS_FreeValue(ctx, res);
                    goto fail;
                }
                kname = df->cols[ki].dict[code];
            } else {
                df_fmt_double(kbuf, sizeof(kbuf), kv);
                kname = kbuf;
            }
            v = dfja_value(ctx, vals[i], strict);
            if (JS_IsException(v)) {
                JS_FreeValue(ctx, res);
                goto fail;
            }
            if (JS_DefinePropertyValueStr(ctx, res, kname, v, JS_PROP_C_W_E) < 0) {
                JS_FreeValue(ctx, res);
                goto fail;
            }
        }
    } else {
        for (g = 0; g < ng; g++) {
            char kbuf[64];
            const char* kname;
            uint64_t bits = set.keys[g];
            double kv = dfc_key_value(bits);
            uint32_t c;
            if (isstr) {
                uint32_t code = (uint32_t)kv;
                if (code >= df->cols[ki].dict_len) {
                    JS_ThrowRangeError(ctx, "JSON_AGG: key code %u out of "
                                            "range",
                        code);
                    JS_FreeValue(ctx, res);
                    goto fail;
                }
                kname = df->cols[ki].dict[code];
            } else {
                df_fmt_double(kbuf, sizeof(kbuf), kv);
                kname = kbuf;
            }
            arr = JS_NewArray(ctx);
            if (JS_IsException(arr)) {
                JS_FreeValue(ctx, res);
                goto fail;
            }
            for (c = 0; c < gcnt[g]; c++) {
                JSValue v = dfja_value(ctx, grows[g][c], strict);
                if (JS_IsException(v)) {
                    JS_FreeValue(ctx, arr);
                    JS_FreeValue(ctx, res);
                    goto fail;
                }
                if (JS_DefinePropertyValueUint32(ctx, arr, c, v,
                        JS_PROP_C_W_E)
                    < 0) {
                    JS_FreeValue(ctx, arr);
                    JS_FreeValue(ctx, res);
                    goto fail;
                }
            }
            if (JS_DefinePropertyValueStr(ctx, res, kname, arr,
                    JS_PROP_C_W_E)
                < 0) {
                JS_FreeValue(ctx, res);
                goto fail;
            }
        }
    }
    dfc_set_free(&set);
    for (g = 0; g < ng; g++)
        free(grows[g]);
    free(grows);
    free(gcnt);
    free(gcap);
    return res;
fail:
    dfc_set_free(&set);
    if (grows) {
        for (g = 0; g < ng; g++)
            free(grows[g]);
        free(grows);
    }
    free(gcnt);
    free(gcap);
    return JS_EXCEPTION;
}

static JSValue dyn_df_json_agg(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    const char* op = (magic == DFJA_AGG || magic == DFJA_AGG_STRICT)
        ? "JSON_AGG"
        : "JSON_OBJECT_AGG";
    DataFrame* df;
    DFBound bk, bv;
    const uint8_t* mask;
    double *keys = NULL, *vals = NULL;
    uint32_t i, n, avail;
    int ki, vi, ok, strict;
    JSValue res, j;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    ki = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (ki < 0)
        return JS_EXCEPTION;
    vi = df_col_arg(ctx, df, argc > 1 ? argv[1] : JS_UNDEFINED);
    if (vi < 0)
        return JS_EXCEPTION;
    mask = df_mask_arg(ctx, argc > 2 ? argv[2] : JS_UNDEFINED, df->nrows, &ok);
    if (!ok)
        return JS_EXCEPTION;
    strict = (magic == DFJA_AGG_STRICT || magic == DFJA_OBJ_STRICT);

    if (dyn_df_bind(ctx, df, ki, &bk))
        return JS_EXCEPTION;
    if (df_bind_numeric(ctx, df, vi, &bv, op))
        return JS_EXCEPTION;
    avail = bk.n < bv.n ? bk.n : bv.n;
    n = avail < df->nrows ? avail : df->nrows;

    keys = df_out_alloc(ctx, n ? n : 1, sizeof(double));
    vals = df_out_alloc(ctx, n ? n : 1, sizeof(double));
    if (!keys || !vals) {
        free(keys);
        free(vals);
        return JS_EXCEPTION;
    }
    for (i = 0; i < n; i++) {
        if (mask && !mask[i]) {
            keys[i] = 0.0;
            vals[i] = 0.0;
            continue;
        }
        keys[i] = (bk.type == DF_STR) ? (double)((const int32_t*)bk.p)[i]
                                      : df_get(bk.p, bk.type, i);
        vals[i] = df_get(bv.p, bv.type, i);
    }

    res = dfja_build(ctx, df, ki, keys, vals, mask, n,
        magic == DFJA_OBJ || magic == DFJA_OBJ_STRICT, strict);
    free(keys);
    free(vals);
    if (JS_IsException(res))
        return JS_EXCEPTION;
    j = JS_JSONStringify(ctx, res, JS_UNDEFINED, JS_UNDEFINED);
    JS_FreeValue(ctx, res);
    return j;
}

static JSValue dfr_pair(JSContext* ctx, const char* na, JSValue a,
    const char* nb, JSValue b)
{
    JSValue res;

    if (JS_IsException(a) || JS_IsException(b)) {
        JS_FreeValue(ctx, a);
        JS_FreeValue(ctx, b);
        return JS_EXCEPTION;
    }
    res = JS_NewObject(ctx);

    if (JS_IsException(res)) {
        JS_FreeValue(ctx, a);
        JS_FreeValue(ctx, b);
        return JS_EXCEPTION;
    }
    if (JS_DefinePropertyValueStr(ctx, res, na, a, JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, b);
        JS_FreeValue(ctx, res);
        return JS_EXCEPTION;
    }
    if (JS_DefinePropertyValueStr(ctx, res, nb, b, JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, res);
        return JS_EXCEPTION;
    }
    return res;
}

static JSValue dyn_df_bounding_ratio(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DataFrame* df;
    DFBound bx, by;
    const uint8_t* mask;
    uint32_t i, span;
    int ix, iy, ok, have = 0;
    double xlo = 0, xhi = 0, ylo = 0, yhi = 0;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    ix = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (ix < 0)
        return JS_EXCEPTION;
    iy = df_col_arg(ctx, df, argc > 1 ? argv[1] : JS_UNDEFINED);
    if (iy < 0)
        return JS_EXCEPTION;
    mask = df_mask_arg(ctx, argc > 2 ? argv[2] : JS_UNDEFINED, df->nrows, &ok);
    if (!ok)
        return JS_EXCEPTION;
    if (df_bind_numeric(ctx, df, ix, &bx, "BOUNDING_RATIO") || df_bind_numeric(ctx, df, iy, &by, "BOUNDING_RATIO"))
        return JS_EXCEPTION;

    span = bx.n < by.n ? bx.n : by.n;
    if (span > df->nrows)
        span = df->nrows;
    for (i = 0; i < span; i++) {
        double x, y;
        if (mask && !mask[i])
            continue;
        x = df_get(bx.p, bx.type, i);
        y = df_get(by.p, by.type, i);
        if (x != x || y != y)
            continue;
        if (!have || x < xlo) {
            xlo = x;
            ylo = y;
        }
        if (!have || x > xhi) {
            xhi = x;
            yhi = y;
        }
        have = 1;
    }
    if (!have || xhi == xlo)
        return JS_NewFloat64(ctx, DYN_NAN);
    return JS_NewFloat64(ctx, (yhi - ylo) / (xhi - xlo));
}

enum { DFE_AVG,
    DFE_SUM,
    DFE_COUNT,
    DFE_MAX };

static const char* dfe_name(int magic)
{
    switch (magic) {
    case DFE_SUM:
        return "EXPONENTIAL_TIME_DECAYED_SUM";
    case DFE_COUNT:
        return "EXPONENTIAL_TIME_DECAYED_COUNT";
    case DFE_MAX:
        return "EXPONENTIAL_TIME_DECAYED_MAX";
    default:
        return "EXPONENTIAL_TIME_DECAYED_AVG";
    }
}

static JSValue dyn_df_etd_avg(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    DataFrame* df;
    DFBound bv, bt;
    const uint8_t* mask;
    const char* op = dfe_name(magic);
    uint32_t i, span;
    int iv, it_, ok, have = 0, hit = 0;
    double tau = 0, tmax = 0, num = 0, den = 0, best = 0;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    iv = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (iv < 0)
        return JS_EXCEPTION;
    it_ = df_col_arg(ctx, df, argc > 1 ? argv[1] : JS_UNDEFINED);
    if (it_ < 0)
        return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &tau, argc > 2 ? argv[2] : JS_UNDEFINED))
        return JS_EXCEPTION;
    if (!(tau > 0.0))
        return JS_ThrowRangeError(ctx, "%s(value, time, tau): tau must be "
                                       "positive, got %g",
            op, tau);
    mask = df_mask_arg(ctx, argc > 3 ? argv[3] : JS_UNDEFINED, df->nrows, &ok);
    if (!ok)
        return JS_EXCEPTION;
    if (df_bind_numeric(ctx, df, iv, &bv, op) || df_bind_numeric(ctx, df, it_, &bt, op))
        return JS_EXCEPTION;

    span = bv.n < bt.n ? bv.n : bt.n;
    if (span > df->nrows)
        span = df->nrows;
    for (i = 0; i < span; i++) {
        double t;
        if (mask && !mask[i])
            continue;
        t = df_get(bt.p, bt.type, i);
        if (t != t)
            continue;
        if (!have || t > tmax)
            tmax = t;
        have = 1;
    }
    if (!have)
        return JS_UNDEFINED;
    for (i = 0; i < span; i++) {
        double v, t, w;
        if (mask && !mask[i])
            continue;
        v = df_get(bv.p, bv.type, i);
        t = df_get(bt.p, bt.type, i);
        if (v != v || t != t)
            continue;
        w = exp((t - tmax) / tau);
        num += v * w;
        den += w;
        if (!hit || v * w > best)
            best = v * w;
        hit = 1;
    }
    if (!hit)
        return JS_UNDEFINED;
    switch (magic) {
    case DFE_SUM:
        return JS_NewFloat64(ctx, num);
    case DFE_COUNT:
        return JS_NewFloat64(ctx, den);
    case DFE_MAX:
        return JS_NewFloat64(ctx, best);
    default:
        break;
    }
    return den > 0.0 ? JS_NewFloat64(ctx, num / den) : JS_UNDEFINED;
}

static JSValue dyn_df_group_insert_at(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DataFrame* df;
    DFBound bv, bp;
    const uint8_t* mask;
    double *out, sz = 0, fill = 0;
    uint32_t i, span, size;
    int iv, ip, ok;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    iv = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (iv < 0)
        return JS_EXCEPTION;
    ip = df_col_arg(ctx, df, argc > 1 ? argv[1] : JS_UNDEFINED);
    if (ip < 0)
        return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &sz, argc > 2 ? argv[2] : JS_UNDEFINED))
        return JS_EXCEPTION;
    if (argc > 3 && !JS_IsUndefined(argv[3]) && JS_ToFloat64(ctx, &fill, argv[3]))
        return JS_EXCEPTION;
    if (!(sz >= 0.0) || sz != floor(sz) || sz > (double)DF_MAX_GROUPS)
        return JS_ThrowRangeError(ctx, "GROUP_ARRAY_INSERT_AT(value, position, "
                                       "size): size must be an integer in [0, %d], "
                                       "got %g",
            DF_MAX_GROUPS, sz);
    mask = df_mask_arg(ctx, argc > 4 ? argv[4] : JS_UNDEFINED, df->nrows, &ok);
    if (!ok)
        return JS_EXCEPTION;
    if (df_bind_numeric(ctx, df, iv, &bv, "GROUP_ARRAY_INSERT_AT") || df_bind_numeric(ctx, df, ip, &bp, "GROUP_ARRAY_INSERT_AT"))
        return JS_EXCEPTION;

    size = (uint32_t)sz;
    out = df_out_alloc(ctx, size ? size : 1, sizeof(double));
    if (!out)
        return JS_EXCEPTION;
    for (i = 0; i < size; i++)
        out[i] = fill;
    span = bv.n < bp.n ? bv.n : bp.n;
    if (span > df->nrows)
        span = df->nrows;
    for (i = 0; i < span; i++) {
        double pos;
        if (mask && !mask[i])
            continue;
        pos = df_get(bp.p, bp.type, i);
        if (!(pos >= 0.0) || pos != floor(pos) || pos >= (double)size)
            continue;
        out[(uint32_t)pos] = df_get(bv.p, bv.type, i);
    }
    return df_to_typed_array(ctx, out, (size_t)size * sizeof(double),
        JS_TYPED_ARRAY_FLOAT64);
}

#define DF_BITMAP_MAX_BITS (1u << 26)

static JSValue dyn_df_group_bitmap(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DataFrame* df;
    DFBound b;
    const uint8_t* mask;
    uint32_t* bits = NULL;
    uint32_t i, span, nw, count = 0;
    double mx = -1.0;
    int idx, ok;

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
    if (b.type == DF_STR || b.type == DF_F64 || b.type == DF_F32)
        return JS_ThrowTypeError(ctx, "GROUP_BITMAP: column '%s' is %s; a "
                                      "bitmap is defined on integer columns",
            df->cols[idx].name, df_type_name(b.type));

    span = b.n < df->nrows ? b.n : df->nrows;
    for (i = 0; i < span; i++) {
        double v;
        if (mask && !mask[i])
            continue;
        v = df_get(b.p, b.type, i);
        if (v < 0.0)
            return JS_ThrowRangeError(ctx, "GROUP_BITMAP: negative value %g; a "
                                           "bitmap indexes by the value itself",
                v);
        if (v > mx)
            mx = v;
    }
    if (mx < 0.0)
        return JS_NewUint32(ctx, 0);
    if (mx >= (double)DF_BITMAP_MAX_BITS)
        return JS_ThrowRangeError(ctx, "GROUP_BITMAP: value %g exceeds the "
                                       "bitmap range of %u; use N_UNIQUE, which is "
                                       "bounded by the row count",
            mx,
            DF_BITMAP_MAX_BITS);
    nw = ((uint32_t)mx >> 5) + 1;
    bits = calloc(nw, sizeof(*bits));
    if (!bits)
        return JS_ThrowOutOfMemory(ctx);
    for (i = 0; i < span; i++) {
        uint32_t v, w, bit;
        if (mask && !mask[i])
            continue;
        v = (uint32_t)df_get(b.p, b.type, i);
        w = v >> 5;
        bit = 1u << (v & 31);
        count += (bits[w] & bit) == 0;
        bits[w] |= bit;
    }
    free(bits);
    return JS_NewUint32(ctx, count);
}

static JSValue dyn_df_quantile_td_weighted(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    DataFrame* df;
    DFBound b, wb;
    const uint8_t* mask;
    dfa_tdigest* t;
    double q = 0, v;
    uint32_t i, span;
    int idx, widx, ok;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    idx = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (idx < 0)
        return JS_EXCEPTION;
    widx = df_col_arg(ctx, df, argc > 1 ? argv[1] : JS_UNDEFINED);
    if (widx < 0)
        return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &q, argc > 2 ? argv[2] : JS_UNDEFINED))
        return JS_EXCEPTION;
    if (!(q >= 0.0 && q <= 1.0))
        return JS_ThrowRangeError(ctx, "QUANTILE_TDIGEST_WEIGHTED(col, w, q): "
                                       "q must be in [0, 1], got %g",
            q);
    mask = df_mask_arg(ctx, argc > 3 ? argv[3] : JS_UNDEFINED, df->nrows, &ok);
    if (!ok)
        return JS_EXCEPTION;
    if (df_bind_numeric(ctx, df, idx, &b, "QUANTILE_TDIGEST_WEIGHTED") || df_bind_numeric(ctx, df, widx, &wb, "QUANTILE_TDIGEST_WEIGHTED"))
        return JS_EXCEPTION;

    t = malloc(sizeof(*t));
    if (!t)
        return JS_ThrowOutOfMemory(ctx);
    dfa_td_init(t);
    span = b.n < wb.n ? b.n : wb.n;
    if (span > df->nrows)
        span = df->nrows;
    for (i = 0; i < span; i++) {
        if (mask && !mask[i])
            continue;
        dfa_td_add_w(t, df_get(b.p, b.type, i), df_get(wb.p, wb.type, i));
    }
    if (t->total <= 0.0) {
        free(t);
        return JS_UNDEFINED;
    }
    v = dfa_td_quantile(t, q);
    free(t);
    return JS_NewFloat64(ctx, v);
}

typedef struct {
    double lo, hi;
} DfrRange;

static int dfr_cmp(const void* pa, const void* pb)
{
    double a = ((const DfrRange*)pa)->lo, b = ((const DfrRange*)pb)->lo;

    return a < b ? -1 : (a > b ? 1 : 0);
}

enum { DFR_UNION,
    DFR_INTERSECT };

static JSValue dyn_df_range_agg(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    const char* op = magic == DFR_INTERSECT ? "RANGE_INTERSECT_AGG"
                                            : "RANGE_AGG";
    DataFrame* df;
    DFBound ba, bb;
    const uint8_t* mask;
    DfrRange* r = NULL;
    double *ls, *hs;
    uint32_t i, span, n = 0, out = 0;
    int ia, ib, ok;

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
    if (df_bind_numeric(ctx, df, ia, &ba, op) || df_bind_numeric(ctx, df, ib, &bb, op))
        return JS_EXCEPTION;

    span = ba.n < bb.n ? ba.n : bb.n;
    if (span > df->nrows)
        span = df->nrows;
    r = df_out_alloc(ctx, span ? span : 1, sizeof(*r));
    if (!r)
        return JS_EXCEPTION;
    for (i = 0; i < span; i++) {
        double lo, hi;
        if (mask && !mask[i])
            continue;
        lo = df_get(ba.p, ba.type, i);
        hi = df_get(bb.p, bb.type, i);
        if (lo != lo || hi != hi || !(lo < hi))
            continue;
        r[n].lo = lo;
        r[n].hi = hi;
        n++;
    }

    if (magic == DFR_INTERSECT) {
        double lo, hi;
        if (n == 0) {
            free(r);
            return JS_UNDEFINED;
        }
        lo = r[0].lo;
        hi = r[0].hi;
        for (i = 1; i < n; i++) {
            if (r[i].lo > lo)
                lo = r[i].lo;
            if (r[i].hi < hi)
                hi = r[i].hi;
        }
        free(r);
        if (!(lo < hi))
            return JS_UNDEFINED;
        return dfr_pair(ctx, "start", JS_NewFloat64(ctx, lo),
            "end", JS_NewFloat64(ctx, hi));
    }

    qsort(r, n, sizeof(*r), dfr_cmp);
    for (i = 0; i < n; i++) {
        if (out > 0 && r[i].lo <= r[out - 1].hi) {
            if (r[i].hi > r[out - 1].hi)
                r[out - 1].hi = r[i].hi;
            continue;
        }
        r[out++] = r[i];
    }
    ls = df_out_alloc(ctx, out ? out : 1, sizeof(double));
    hs = df_out_alloc(ctx, out ? out : 1, sizeof(double));
    if (!ls || !hs) {
        free(r);
        free(ls);
        free(hs);
        return JS_EXCEPTION;
    }
    for (i = 0; i < out; i++) {
        ls[i] = r[i].lo;
        hs[i] = r[i].hi;
    }
    free(r);
    return dfr_pair(ctx, "starts",
        df_to_typed_array(ctx, ls, (size_t)out * sizeof(double),
            JS_TYPED_ARRAY_FLOAT64),
        "ends",
        df_to_typed_array(ctx, hs, (size_t)out * sizeof(double),
            JS_TYPED_ARRAY_FLOAT64));
}

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

static int dyn_df_init_module(JSContext* ctx, JSModuleDef* m)
{
    JSValue isdf;

    dfc_hash_seed_once();
    df_init_ta_classes(ctx);
    if (dyn_register_plain_class(ctx, m, &dyn_df_class_id, &dyn_df_class,
            dyn_df_proto, countof(dyn_df_proto),
            dyn_df_ctor, "DataFrame")
        < 0)
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
