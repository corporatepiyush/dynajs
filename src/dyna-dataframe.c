/* dyna:dataframe -- columnar tables over TypedArrays. Columns are discriminated
   by CLASS ID, never by element width: two distinct TypedArray classes can share
   a width and mixing them silently corrupts. Full API: docs/dynajs-guide/API.md. */
#include "dyna-nat.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_DATAFRAME)

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* Local, per the native-module rule: a binding file includes only the public
   dynajs.h (cutils.h's container_of/likely clash with module headers). */
#define countof(x) (sizeof(x) / sizeof((x)[0]))

#define DF_MAX_COLS   1024
#define DF_MAX_GROUPS (1 << 20)   /* group-by cardinality cap */

/* Column element type. The JS side passes a TypedArray; the element width plus
 * an explicit float/int discriminator is all the kernels need. */
typedef enum {
    DF_F64, DF_F32, DF_I32, DF_U32, DF_I16, DF_U16, DF_I8, DF_U8,
    DF_STR      /* dictionary-encoded: codes live in an owned int32 array */
} DFType;

typedef struct {
    char *name;             /* owned */
    DFType type;
    JSValue buffer;         /* owned ref to the ArrayBuffer (numeric cols) */
    uint32_t byte_offset;   /* view offset within the buffer */
    uint32_t length;        /* element count */
    /* DF_STR only: */
    int32_t *codes;         /* owned, `length` entries */
    char **dict;            /* owned, dict_len NUL-terminated strings */
    uint32_t dict_len;
} DFColumn;

typedef struct {
    DFColumn *cols;
    uint32_t ncols;
    uint32_t nrows;
} DataFrame;

/* A column resolved to a live pointer for the duration of ONE native span. */
typedef struct {
    const void *p;
    DFType type;
    uint32_t n;
} DFBound;

static JSClassID dyn_df_class_id;

/* Exact TypedArray discrimination. So the class IDs are captured once at module
   init by making one array of each type and asking the engine, and every column
   is typed by identity rather than by guessing. */
static struct {
    JSClassID id;
    DFType type;
} df_ta_class[8];
static int df_ta_class_count;

static JSClassID df_class_id_of(JSContext *ctx, JSTypedArrayEnum type)
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

static void df_init_ta_classes(JSContext *ctx)
{
    static const struct { JSTypedArrayEnum e; DFType t; } map[] = {
        { JS_TYPED_ARRAY_FLOAT64, DF_F64 }, { JS_TYPED_ARRAY_FLOAT32, DF_F32 },
        { JS_TYPED_ARRAY_INT32,   DF_I32 }, { JS_TYPED_ARRAY_UINT32,  DF_U32 },
        { JS_TYPED_ARRAY_INT16,   DF_I16 }, { JS_TYPED_ARRAY_UINT16,  DF_U16 },
        { JS_TYPED_ARRAY_INT8,    DF_I8  }, { JS_TYPED_ARRAY_UINT8,   DF_U8  },
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

/* 0 and *out set, or -1 if `v` is not a supported TypedArray. Uint8ClampedArray
 * and the BigInt arrays are deliberately absent: clamped has different write
 * semantics and BigInt elements do not fit a double. */
static int df_type_of_value(JSValueConst v, DFType *out)
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

/* ---------------------------------------------------------------- helpers */

static void df_col_free(DFColumn *c)
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

/* The JSValue refs must be released with a runtime, so freeing splits in two:
 * the finalizer drops the buffer refs, this frees the malloc'd parts. */
static void dyn_df_dispose(void *native)
{
    DataFrame *df = native;
    uint32_t i;
    if (!df)
        return;
    for (i = 0; i < df->ncols; i++)
        df_col_free(&df->cols[i]);
    free(df->cols);
    free(df);
}

static void dyn_df_finalizer(JSRuntime *rt, JSValue val)
{
    DataFrame *df = JS_GetOpaque(val, dyn_df_class_id);
    uint32_t i;
    if (df) {
        for (i = 0; i < df->ncols; i++)
            JS_FreeValueRT(rt, df->cols[i].buffer);
        dyn_df_dispose(df);
    }
}

static void dyn_df_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    DataFrame *df = JS_GetOpaque(val, dyn_df_class_id);
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
    switch (t) {
    case DF_F64: return 8;
    case DF_F32: case DF_I32: case DF_U32: case DF_STR: return 4;
    case DF_I16: case DF_U16: return 2;
    default: return 1;
    }
}

/* Column type as the TypedArray name the caller passed in, for error messages.
 * A reduction that refuses a column has to name the type it actually got, or
 * the caller cannot tell which of their columns is wrong. */
static const char *df_type_name(DFType t)
{
    switch (t) {
    case DF_F64: return "Float64Array";
    case DF_F32: return "Float32Array";
    case DF_I32: return "Int32Array";
    case DF_U32: return "Uint32Array";
    case DF_I16: return "Int16Array";
    case DF_U16: return "Uint16Array";
    case DF_I8:  return "Int8Array";
    case DF_U8:  return "Uint8Array";
    default:     return "a dictionary-encoded string column";
    }
}

static JSValue df_to_typed_array(JSContext *ctx, void *p, size_t nbytes,
                                 JSTypedArrayEnum type);
/* Read one element as a double. Kept out of the hot loops on purpose -- the
 * reductions below switch on the type ONCE and then run a typed loop. */
static double df_get(const void *p, DFType t, uint32_t i)
{
    switch (t) {
    case DF_F64: return ((const double *)p)[i];
    case DF_F32: return ((const float *)p)[i];
    case DF_I32: case DF_STR: return ((const int32_t *)p)[i];
    case DF_U32: return ((const uint32_t *)p)[i];
    case DF_I16: return ((const int16_t *)p)[i];
    case DF_U16: return ((const uint16_t *)p)[i];
    case DF_I8: return ((const int8_t *)p)[i];
    default: return ((const uint8_t *)p)[i];
    }
}

static int df_find_col(const DataFrame *df, const char *name)
{
    uint32_t i;
    for (i = 0; i < df->ncols; i++)
        if (strcmp(df->cols[i].name, name) == 0)
            return (int)i;
    return -1;
}

/* Resolve a column to a live pointer. MUST be called after every JS-invoking
   coercion in the method and never cached: user code can detach or resize the
   ArrayBuffer, so the range is re-validated on each bind. */
static int dyn_df_bind(JSContext *ctx, const DataFrame *df, int idx, DFBound *b)
{
    const DFColumn *c = &df->cols[idx];
    uint8_t *base;
    size_t ab_size, need;

    if (c->type == DF_STR) {
        b->p = c->codes;
        b->type = DF_STR;
        b->n = c->length;
        return 0;
    }
    base = JS_GetArrayBuffer(ctx, &ab_size, c->buffer);
    if (!base) {                        /* detached */
        JS_ThrowTypeError(ctx, "column '%s': ArrayBuffer is detached", c->name);
        return -1;
    }
    need = (size_t)c->length * df_elt_size(c->type);
    if (c->byte_offset > ab_size || need > ab_size - c->byte_offset) {
        JS_ThrowRangeError(ctx, "column '%s': view is out of bounds "
                           "(buffer resized?)", c->name);
        return -1;
    }
    b->p = base + c->byte_offset;
    b->type = c->type;
    b->n = c->length;
    return 0;
}

/* Resolve a column argument given as a JS string. Coerces (may run JS) and
 * returns the index, or -1 throwing. */
static int df_col_arg(JSContext *ctx, const DataFrame *df, JSValueConst v)
{
    size_t len;
    const char *s = JS_ToCStringLen(ctx, &len, v);
    int idx;
    if (!s)
        return -1;
    /* A JS string may hold U+0000 and the C form is NUL-terminated, so strcmp
       would match a PREFIX: "secret\0ignored" resolved to "secret". Refuse it
       rather than truncate — a caller that vetted the name vetted all of it. */
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

/* ------------------------------------------------------- mask + reductions */

/* Optional mask argument: a Uint8Array of nrows, or undefined. Returns the
 * bytes (borrowed from the JS buffer, valid for this native span only) or NULL
 * with *ok=0 on error. */
static const uint8_t *df_mask_arg(JSContext *ctx, JSValueConst v,
                                  uint32_t nrows, int *ok)
{
    JSValue buf;
    uint8_t *base;
    size_t off, len, bpe, ab;

    *ok = 1;
    if (JS_IsUndefined(v) || JS_IsNull(v))
        return NULL;
    buf = JS_GetTypedArrayBuffer(ctx, v, &off, &len, &bpe);
    if (JS_IsException(buf)) { *ok = 0; return NULL; }
    if (bpe != 1 || len < nrows) {
        JS_FreeValue(ctx, buf);
        JS_ThrowTypeError(ctx, "mask must be a Uint8Array of at least %u bytes",
                          nrows);
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

enum { DF_SUM, DF_MIN, DF_MAX, DF_MEAN, DF_COUNT };

/* ---- the reduction kernel family ----
   WHAT THE OLD GENERIC LOOP COST, AND WHY. BUT THE SUM WAS NOT THE COST. sum and
   mean PROPAGATE it, because `acc += v` does and the merge tree carries it out. */

/* Every numeric column type, once, split by what the accumulator is: the two
   halves take a different number of accumulators and nothing else differs. */
#define DF_FLOAT_TYPES(X)                                                     \
    X(f64, double,   DF_F64)                                                  \
    X(f32, float,    DF_F32)
#define DF_INT_TYPES(X)                                                       \
    X(i32, int32_t,  DF_I32)                                                  \
    X(u32, uint32_t, DF_U32)                                                  \
    X(i16, int16_t,  DF_I16)                                                  \
    X(u16, uint16_t, DF_U16)                                                  \
    X(i8,  int8_t,   DF_I8)                                                   \
    X(u8,  uint8_t,  DF_U8)
#define DF_NUMERIC_TYPES(X)  DF_FLOAT_TYPES(X) DF_INT_TYPES(X)

/* The list must cover every DFType below DF_STR. */
#define DF_COUNT_ONE(sfx, cty, tag)  + 1
_Static_assert((0 DF_NUMERIC_TYPES(DF_COUNT_ONE)) == DF_STR,
               "DF_NUMERIC_TYPES must list every numeric DFType exactly once");
#undef DF_COUNT_ONE

/* STEP(acc, v) folds one already-loaded value into one accumulator;
   COMBINE(p, q) merges two accumulators. Both are expressions and both
   evaluate their arguments more than once -- only ever applied to locals. */
#define DF_STEP_SUM(acc, v)     ((acc) += (v))
#define DF_STEP_MIN(acc, v)     ((acc) = (v) < (acc) ? (v) : (acc))
#define DF_STEP_MAX(acc, v)     ((acc) = (v) > (acc) ? (v) : (acc))
#define DF_COMBINE_SUM(p, q)    ((p) + (q))
#define DF_COMBINE_MIN(p, q)    ((p) < (q) ? (p) : (q))
#define DF_COMBINE_MAX(p, q)    ((p) > (q) ? (p) : (q))

/* It is not changing. Plain FMIN / vminq_f64 PROPAGATES NaN and would break the
   contract; only the NM ("number") forms are usable here. The value compares
   equal either way, so `===` cannot see it and Object.is can. */
#if defined(__aarch64__) || defined(__arm64__) || defined(_M_ARM64)
#define DF_STEP_FMIN_f64(acc, v)  ((acc) = __builtin_fmin((v), (acc)))
#define DF_STEP_FMAX_f64(acc, v)  ((acc) = __builtin_fmax((v), (acc)))
#define DF_STEP_FMIN_f32(acc, v)  ((acc) = __builtin_fminf((v), (acc)))
#define DF_STEP_FMAX_f32(acc, v)  ((acc) = __builtin_fmaxf((v), (acc)))
#else
#define DF_STEP_FMIN_f64(acc, v)  DF_STEP_MIN(acc, v)
#define DF_STEP_FMAX_f64(acc, v)  DF_STEP_MAX(acc, v)
#define DF_STEP_FMIN_f32(acc, v)  DF_STEP_MIN(acc, v)
#define DF_STEP_FMAX_f32(acc, v)  DF_STEP_MAX(acc, v)
#endif

/* Accumulator type and identity, selected by (aggregate, column type). Two
   levels of indirection so the arguments expand before they are pasted. */
#define DF_ACC__(kind, sfx)     DF_ACC_##kind##_##sfx
#define DF_ACC(kind, sfx)       DF_ACC__(kind, sfx)
#define DF_ID__(kind, sfx)      DF_ID_##kind##_##sfx
#define DF_ID(kind, sfx)        DF_ID__(kind, sfx)

#define DF_ACC_SUM_f64  double
#define DF_ACC_SUM_f32  double
#define DF_ACC_SUM_i32  int64_t
#define DF_ACC_SUM_u32  uint64_t
#define DF_ACC_SUM_i16  int64_t
#define DF_ACC_SUM_u16  uint64_t
#define DF_ACC_SUM_i8   int64_t
#define DF_ACC_SUM_u8   uint64_t
#define DF_ID_SUM_f64   0.0
#define DF_ID_SUM_f32   0.0
#define DF_ID_SUM_i32   0
#define DF_ID_SUM_u32   0
#define DF_ID_SUM_i16   0
#define DF_ID_SUM_u16   0
#define DF_ID_SUM_i8    0
#define DF_ID_SUM_u8    0

#define DF_ACC_MIN_f64  double
#define DF_ACC_MIN_f32  float
#define DF_ACC_MIN_i32  int32_t
#define DF_ACC_MIN_u32  uint32_t
#define DF_ACC_MIN_i16  int16_t
#define DF_ACC_MIN_u16  uint16_t
#define DF_ACC_MIN_i8   int8_t
#define DF_ACC_MIN_u8   uint8_t
#define DF_ID_MIN_f64   INFINITY
#define DF_ID_MIN_f32   INFINITY
#define DF_ID_MIN_i32   INT32_MAX
#define DF_ID_MIN_u32   UINT32_MAX
#define DF_ID_MIN_i16   INT16_MAX
#define DF_ID_MIN_u16   UINT16_MAX
#define DF_ID_MIN_i8    INT8_MAX
#define DF_ID_MIN_u8    UINT8_MAX

#define DF_ACC_MAX_f64  double
#define DF_ACC_MAX_f32  float
#define DF_ACC_MAX_i32  int32_t
#define DF_ACC_MAX_u32  uint32_t
#define DF_ACC_MAX_i16  int16_t
#define DF_ACC_MAX_u16  uint16_t
#define DF_ACC_MAX_i8   int8_t
#define DF_ACC_MAX_u8   uint8_t
#define DF_ID_MAX_f64   (-INFINITY)
#define DF_ID_MAX_f32   (-INFINITY)
#define DF_ID_MAX_i32   INT32_MIN
#define DF_ID_MAX_u32   0
#define DF_ID_MAX_i16   INT16_MIN
#define DF_ID_MAX_u16   0
#define DF_ID_MAX_i8    INT8_MIN
#define DF_ID_MAX_u8    0

/* Change it only with a differential that admits a ULP bound rather than bit-
   equality. At K=8 an f64 fold is 4 chains on NEON and only 2 on AVX2, which
   does not cover a 4-cycle vaddpd at 2/cycle throughput; K=16 gives 8 and 4. */
#define DF_ACCS_FLOAT   16
#define DF_ACCS_INT     1

/* Measured 0.87x / 0.87x / 0.89x / 0.89x at 8 K / 10 K / 1 M / 8 M -- the only
   regression in the table. Instruction density decides this only when the chains
   are already covered. Do not merge these three cases. */
#define DF_NACC_SUM_f64 DF_ACCS_FLOAT
#define DF_NACC_SUM_f32 8

/* The masked fold is instruction-bound (a select and a counter per element)
   where the unmasked one is latency-bound, so it wants fewer chains: at sixteen
   masked f64 sum and product lose 0.86x while unmasked gain. Min/max keep 16. */
#define DF_NACC_MASKED_FLOAT 8

_Static_assert((DF_ACCS_FLOAT & (DF_ACCS_FLOAT - 1)) == 0 &&
               (DF_ACCS_INT   & (DF_ACCS_INT   - 1)) == 0 &&
               (DF_NACC_SUM_f32 & (DF_NACC_SUM_f32 - 1)) == 0 &&
               (DF_NACC_MASKED_FLOAT & (DF_NACC_MASKED_FLOAT - 1)) == 0,
               "accumulator counts must be powers of two: the body bound "
               "n & ~(NACC-1) over-reads the column otherwise");

/* NACC arrives as a named constant, so the paste needs one indirection or it
   would glue the macro's NAME to the prefix instead of its value. */
#define DF_PASTE_(a, b) a##b
#define DF_PASTE(a, b)  DF_PASTE_(a, b)

/* --- the four shapes an NACC-way body is made of ------------------------- */
#define DF_DECL_1(T, I)  T a0 = I;
#define DF_DECL_8(T, I)  T a0 = I, a1 = I, a2 = I, a3 = I,                    \
                           a4 = I, a5 = I, a6 = I, a7 = I;
#define DF_DECL_16(T, I) T a0 = I, a1 = I, a2 = I, a3 = I,                    \
                           a4 = I, a5 = I, a6 = I, a7 = I,                    \
                           a8 = I, a9 = I, a10 = I, a11 = I,                  \
                           a12 = I, a13 = I, a14 = I, a15 = I;
#define DF_MERGE_1(C)    (a0)
#define DF_MERGE_8(C)    C(C(C(a0, a1), C(a2, a3)), C(C(a4, a5), C(a6, a7)))
#define DF_MERGE_16(C)   C(C(C(C(a0, a1),   C(a2, a3)),                       \
                             C(C(a4, a5),   C(a6, a7))),                      \
                           C(C(C(a8, a9),   C(a10, a11)),                     \
                             C(C(a12, a13), C(a14, a15))))

#define DF_FOLD_1(T, S)                                                       \
    { T v0 = (T)x[i]; S(a0, v0); }
#define DF_FOLD_8(T, S)                                                       \
    { T v0 = (T)x[i],     v1 = (T)x[i + 1], v2 = (T)x[i + 2],                 \
        v3 = (T)x[i + 3], v4 = (T)x[i + 4], v5 = (T)x[i + 5],                 \
        v6 = (T)x[i + 6], v7 = (T)x[i + 7];                                   \
      S(a0, v0); S(a1, v1); S(a2, v2); S(a3, v3);                             \
      S(a4, v4); S(a5, v5); S(a6, v6); S(a7, v7); }
#define DF_FOLD_16(T, S)                                                      \
    { T v0 = (T)x[i],       v1 = (T)x[i + 1],   v2 = (T)x[i + 2],             \
        v3 = (T)x[i + 3],   v4 = (T)x[i + 4],   v5 = (T)x[i + 5],             \
        v6 = (T)x[i + 6],   v7 = (T)x[i + 7],   v8 = (T)x[i + 8],             \
        v9 = (T)x[i + 9],   v10 = (T)x[i + 10], v11 = (T)x[i + 11],           \
        v12 = (T)x[i + 12], v13 = (T)x[i + 13], v14 = (T)x[i + 14],           \
        v15 = (T)x[i + 15];                                                   \
      S(a0, v0);   S(a1, v1);   S(a2, v2);   S(a3, v3);                       \
      S(a4, v4);   S(a5, v5);   S(a6, v6);   S(a7, v7);                       \
      S(a8, v8);   S(a9, v9);   S(a10, v10); S(a11, v11);                     \
      S(a12, v12); S(a13, v13); S(a14, v14); S(a15, v15); }

/* One half-width cleanup block between the NACC-wide body and the scalar tail.
   Sizes that are multiples of sixteen -- which is every size in the obvious
   benchmark grid -- cannot see this at all. */
#define DF_CLEAN_1(T, S)                /* lim == n; the tail never runs */
#define DF_CLEAN_8(T, S)                /* at most 7 already */
#define DF_CLEAN_16(T, S)                                                     \
    if (n - i >= 8) { DF_FOLD_8(T, S) i += 8; }
#define DF_MCLEAN_1(T, S, I)
#define DF_MCLEAN_8(T, S, I)
#define DF_MCLEAN_16(T, S, I)                                                 \
    if (n - i >= 8) { DF_MFOLD_8(T, S, I) i += 8; }

/* The masked select stays a ternary. The bit-pattern form of the same trick for
   a floating-point sum is worse than null, at 0.35x, because the punning blocks
   vectorisation of the whole body. */
#define DF_MFOLD_1(T, S, I)                                                   \
    { T v0 = (T)x[i];                                                         \
      v0 = m[i] ? v0 : (T)(I);                                                \
      S(a0, v0);                                                              \
      k += (m[i] != 0); }
#define DF_MFOLD_8(T, S, I)                                                   \
    { T v0 = (T)x[i],     v1 = (T)x[i + 1], v2 = (T)x[i + 2],                 \
        v3 = (T)x[i + 3], v4 = (T)x[i + 4], v5 = (T)x[i + 5],                 \
        v6 = (T)x[i + 6], v7 = (T)x[i + 7];                                   \
      v0 = m[i]     ? v0 : (T)(I); v1 = m[i + 1] ? v1 : (T)(I);               \
      v2 = m[i + 2] ? v2 : (T)(I); v3 = m[i + 3] ? v3 : (T)(I);               \
      v4 = m[i + 4] ? v4 : (T)(I); v5 = m[i + 5] ? v5 : (T)(I);               \
      v6 = m[i + 6] ? v6 : (T)(I); v7 = m[i + 7] ? v7 : (T)(I);               \
      S(a0, v0); S(a1, v1); S(a2, v2); S(a3, v3);                             \
      S(a4, v4); S(a5, v5); S(a6, v6); S(a7, v7);                             \
      k += (m[i] != 0)     + (m[i + 1] != 0) + (m[i + 2] != 0)                \
         + (m[i + 3] != 0) + (m[i + 4] != 0) + (m[i + 5] != 0)                \
         + (m[i + 6] != 0) + (m[i + 7] != 0); }
#define DF_MFOLD_16(T, S, I)                                                  \
    { T v0 = (T)x[i],       v1 = (T)x[i + 1],   v2 = (T)x[i + 2],             \
        v3 = (T)x[i + 3],   v4 = (T)x[i + 4],   v5 = (T)x[i + 5],             \
        v6 = (T)x[i + 6],   v7 = (T)x[i + 7],   v8 = (T)x[i + 8],             \
        v9 = (T)x[i + 9],   v10 = (T)x[i + 10], v11 = (T)x[i + 11],           \
        v12 = (T)x[i + 12], v13 = (T)x[i + 13], v14 = (T)x[i + 14],           \
        v15 = (T)x[i + 15];                                                   \
      v0 = m[i]      ? v0 : (T)(I); v1 = m[i + 1]   ? v1 : (T)(I);            \
      v2 = m[i + 2]  ? v2 : (T)(I); v3 = m[i + 3]   ? v3 : (T)(I);            \
      v4 = m[i + 4]  ? v4 : (T)(I); v5 = m[i + 5]   ? v5 : (T)(I);            \
      v6 = m[i + 6]  ? v6 : (T)(I); v7 = m[i + 7]   ? v7 : (T)(I);            \
      v8 = m[i + 8]  ? v8 : (T)(I); v9 = m[i + 9]   ? v9 : (T)(I);            \
      v10 = m[i + 10] ? v10 : (T)(I); v11 = m[i + 11] ? v11 : (T)(I);         \
      v12 = m[i + 12] ? v12 : (T)(I); v13 = m[i + 13] ? v13 : (T)(I);         \
      v14 = m[i + 14] ? v14 : (T)(I); v15 = m[i + 15] ? v15 : (T)(I);         \
      S(a0, v0);   S(a1, v1);   S(a2, v2);   S(a3, v3);                       \
      S(a4, v4);   S(a5, v5);   S(a6, v6);   S(a7, v7);                       \
      S(a8, v8);   S(a9, v9);   S(a10, v10); S(a11, v11);                     \
      S(a12, v12); S(a13, v13); S(a14, v14); S(a15, v15);                     \
      k += (m[i] != 0)      + (m[i + 1] != 0)  + (m[i + 2] != 0)              \
         + (m[i + 3] != 0)  + (m[i + 4] != 0)  + (m[i + 5] != 0)              \
         + (m[i + 6] != 0)  + (m[i + 7] != 0)  + (m[i + 8] != 0)              \
         + (m[i + 9] != 0)  + (m[i + 10] != 0) + (m[i + 11] != 0)             \
         + (m[i + 12] != 0) + (m[i + 13] != 0) + (m[i + 14] != 0)             \
         + (m[i + 15] != 0); }

/* Generate the unmasked and masked kernels for one (aggregate, column type). A
   three-argument masked kernel cannot express "nothing was selected" and would
   need a second pass over the mask to recover it. */
/* NACC drives the unmasked body, MNACC the masked one. They are separate because
   the two bodies are bound by different things -- see DF_NACC_MASKED_FLOAT. */
#define DF_DEFINE_REDUCE_KM(NACC, MNACC, method, suffix, ctype, acctype, INIT, \
                            STEP, COMBINE)                                    \
static acctype df_##method##_##suffix(const ctype *restrict x, uint32_t n)    \
{                                                                             \
    DF_PASTE(DF_DECL_, NACC)(acctype, INIT)                                   \
    uint32_t i, lim = n & ~(uint32_t)(NACC - 1);                              \
    for (i = 0; i < lim; i += NACC)                                           \
        DF_PASTE(DF_FOLD_, NACC)(acctype, STEP)                               \
    DF_PASTE(DF_CLEAN_, NACC)(acctype, STEP)                                  \
    for (; i < n; i++) {                                                      \
        acctype v = (acctype)x[i];                                            \
        STEP(a0, v);                                                          \
    }                                                                         \
    return DF_PASTE(DF_MERGE_, NACC)(COMBINE);                                \
}                                                                             \
                                                                              \
static acctype df_##method##_masked_##suffix(const ctype *restrict x,         \
                                             const uint8_t *restrict m,       \
                                             uint32_t n, uint32_t *pcount)    \
{                                                                             \
    DF_PASTE(DF_DECL_, MNACC)(acctype, INIT)                                  \
    uint32_t i, k = 0, lim = n & ~(uint32_t)(MNACC - 1);                      \
    for (i = 0; i < lim; i += MNACC)                                          \
        DF_PASTE(DF_MFOLD_, MNACC)(acctype, STEP, INIT)                       \
    DF_PASTE(DF_MCLEAN_, MNACC)(acctype, STEP, INIT)                          \
    for (; i < n; i++) {                                                      \
        acctype v = (acctype)x[i];                                            \
        v = m[i] ? v : (acctype)(INIT);                                       \
        STEP(a0, v);                                                          \
        k += (m[i] != 0);                                                     \
    }                                                                         \
    *pcount = k;                                                              \
    return DF_PASTE(DF_MERGE_, MNACC)(COMBINE);                               \
}

/* Both bodies at the same width. Every existing call site keeps this spelling,
   so the arms that do not want a split are byte-for-byte what they were. */
#define DF_DEFINE_REDUCE_K(NACC, method, suffix, ctype, acctype, INIT, STEP,  \
                           COMBINE)                                           \
    DF_DEFINE_REDUCE_KM(NACC, NACC, method, suffix, ctype, acctype, INIT,     \
                        STEP, COMBINE)

/* The 7-argument spelling: the accumulator count is DF_ACCS_FLOAT, which is what
   any floating-point fold wants. */
#define DF_DEFINE_REDUCE(method, suffix, ctype, acctype, INIT, STEP, COMBINE) \
    DF_DEFINE_REDUCE_K(DF_ACCS_FLOAT, method, suffix, ctype, acctype, INIT,   \
                       STEP, COMBINE)

/* x and m are both restrict and a caller can legally pass one Uint8Array as
   both the column and the mask. That is well defined: restrict is only
   violated by MODIFYING the aliased object, and neither pointer is written. */

#define DF_DEF_SUM_F(sfx, cty, tag)                                           \
    DF_DEFINE_REDUCE_KM(DF_PASTE(DF_NACC_SUM_, sfx), DF_NACC_MASKED_FLOAT,    \
                       sum, sfx, cty,                                         \
                       DF_ACC(SUM, sfx), DF_ID(SUM, sfx), DF_STEP_SUM,        \
                       DF_COMBINE_SUM)
#define DF_DEF_SUM_I(sfx, cty, tag)                                           \
    DF_DEFINE_REDUCE_K(DF_ACCS_INT, sum, sfx, cty, DF_ACC(SUM, sfx),          \
                       DF_ID(SUM, sfx), DF_STEP_SUM, DF_COMBINE_SUM)
DF_FLOAT_TYPES(DF_DEF_SUM_F)
DF_INT_TYPES(DF_DEF_SUM_I)
#undef DF_DEF_SUM_F
#undef DF_DEF_SUM_I

/* The float instantiations take the per-suffix FMIN/FMAX step, which is the ISA-
   split one; the integer instantiations keep the plain comparison, because
   SMIN/SMINV already reduce an integer column in one instruction from a single. */
#define DF_DEF_MIN_F(sfx, cty, tag)                                           \
    DF_DEFINE_REDUCE(min, sfx, cty, DF_ACC(MIN, sfx),                         \
                     DF_ID(MIN, sfx), DF_PASTE(DF_STEP_FMIN_, sfx),           \
                     DF_COMBINE_MIN)
#define DF_DEF_MIN_I(sfx, cty, tag)                                           \
    DF_DEFINE_REDUCE_K(DF_ACCS_INT, min, sfx, cty, DF_ACC(MIN, sfx),          \
                       DF_ID(MIN, sfx), DF_STEP_MIN, DF_COMBINE_MIN)
DF_FLOAT_TYPES(DF_DEF_MIN_F)
DF_INT_TYPES(DF_DEF_MIN_I)
#undef DF_DEF_MIN_F
#undef DF_DEF_MIN_I

#define DF_DEF_MAX_F(sfx, cty, tag)                                           \
    DF_DEFINE_REDUCE(max, sfx, cty, DF_ACC(MAX, sfx),                         \
                     DF_ID(MAX, sfx), DF_PASTE(DF_STEP_FMAX_, sfx),           \
                     DF_COMBINE_MAX)
#define DF_DEF_MAX_I(sfx, cty, tag)                                           \
    DF_DEFINE_REDUCE_K(DF_ACCS_INT, max, sfx, cty, DF_ACC(MAX, sfx),          \
                       DF_ID(MAX, sfx), DF_STEP_MAX, DF_COMBINE_MAX)
DF_FLOAT_TYPES(DF_DEF_MAX_F)
DF_INT_TYPES(DF_DEF_MAX_I)
#undef DF_DEF_MAX_F
#undef DF_DEF_MAX_I

/* One dispatch case. The aggregate comes from `magic` and the kernel from the
   column's type -- which was decided by CLASS ID at construction, never by
   element width -- and both are resolved ONCE per call, never per element. */
#define DF_REDUCE_CASE(method, sfx, cty, tag)                                 \
        case tag:                                                             \
            if (mask)                                                         \
                acc = (double)df_##method##_masked_##sfx((const cty *)b.p,    \
                                                         mask, b.n, &count);  \
            else {                                                            \
                acc = (double)df_##method##_##sfx((const cty *)b.p, b.n);     \
                count = b.n;                                                  \
            }                                                                 \
            break;

static JSValue dyn_df_reduce(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv, int magic)
{
    DataFrame *df;
    DFBound b;
    const uint8_t *mask;
    int idx, ok;
    uint32_t i, count = 0;
    double acc = 0;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    /* coerce EVERY argument before binding any column pointer: ToCString and
       the typed-array accessors can run user JS (valueOf/Proxy) that detaches
       the very buffer we are about to alias. */
    idx = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (idx < 0)
        return JS_EXCEPTION;
    mask = df_mask_arg(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, df->nrows, &ok);
    if (!ok)
        return JS_EXCEPTION;

    /* no JS may run from here to the end of the loop. */
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

    /* Two switches, both loop-invariant: the aggregate picks the kernel family
       and the column type picks the instance. */
    switch (magic) {
    case DF_MIN:
        switch (b.type) {
#define DF_CASE(sfx, cty, tag) DF_REDUCE_CASE(min, sfx, cty, tag)
        DF_NUMERIC_TYPES(DF_CASE)
#undef DF_CASE
        default:
            return JS_ThrowTypeError(ctx, "cannot reduce a string column");
        }
        break;
    case DF_MAX:
        switch (b.type) {
#define DF_CASE(sfx, cty, tag) DF_REDUCE_CASE(max, sfx, cty, tag)
        DF_NUMERIC_TYPES(DF_CASE)
#undef DF_CASE
        default:
            return JS_ThrowTypeError(ctx, "cannot reduce a string column");
        }
        break;
    default:                    /* DF_SUM and DF_MEAN share the sum kernels */
        switch (b.type) {
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
    case DF_MAX:  return count ? JS_NewFloat64(ctx, acc) : JS_UNDEFINED;
    case DF_MEAN: return count ? JS_NewFloat64(ctx, acc / count)
                               : JS_NewFloat64(ctx, NAN);
    default:      return JS_NewFloat64(ctx, acc);
    }
}
/* The seven-argument spelling means DF_ACCS_FLOAT accumulators. Taking the
   default here is silent -- it compiles and it is correct. */

/* ------------------------------------------------------ shared step macros */

/* They are not interchangeable: passing an expression-form STEP compiles to a
   statement with no effect, which computes nothing and is caught only by
   -Wunused-value. Same split as Batch 1's DF_STEP_SUM / DF_COMBINE_SUM. */
#define DF_STEP_MUL(acc, v)     ((acc) *= (v))
#define DF_STEP_AND(acc, v)     ((acc) &= (v))
#define DF_STEP_OR(acc, v)      ((acc) |= (v))
#define DF_STEP_XOR(acc, v)     ((acc) ^= (v))
#define DF_COMBINE_MUL(p, q)    ((p) * (q))
#define DF_COMBINE_AND(p, q)    ((p) & (q))
#define DF_COMBINE_OR(p, q)     ((p) | (q))
#define DF_COMBINE_XOR(p, q)    ((p) ^ (q))

/* ---- B.2 product ----
   ------------------------------------------------------------- B.2 product
   Accumulated in DOUBLE for every column type, including the integer ones. */
#define X(suffix, ctype, tag)                                                 \
    DF_DEFINE_REDUCE_KM(DF_ACCS_FLOAT, DF_NACC_MASKED_FLOAT,                  \
                        product, suffix, ctype, double, 1.0,                  \
                        DF_STEP_MUL, DF_COMBINE_MUL)
DF_NUMERIC_TYPES(X)
#undef X

/* ---- B.3 bitwise ----
   ------------------------------------------------------------- B.3 bitwise
   Integer columns only; a float column is refused by name rather than coerced. */
#define X(suffix, ctype, tag)                                                 \
    DF_DEFINE_REDUCE_K(DF_ACCS_INT, band, suffix, ctype, uint32_t, ~0u,       \
                       DF_STEP_AND, DF_COMBINE_AND)                           \
    DF_DEFINE_REDUCE_K(DF_ACCS_INT, bor,  suffix, ctype, uint32_t, 0u,        \
                       DF_STEP_OR,  DF_COMBINE_OR)                            \
    DF_DEFINE_REDUCE_K(DF_ACCS_INT, bxor, suffix, ctype, uint32_t, 0u,        \
                       DF_STEP_XOR, DF_COMBINE_XOR)
DF_INT_TYPES(X)
#undef X

/* ---- B.4 variance ----
   TWO-PASS, not Welford. Sample variance, n-1 divisor. Fewer than two selected
   rows -> NaN. */
#define DF_DEFINE_VARIANCE(suffix, ctype, tag)                                \
static double df_variance_##suffix(const ctype *restrict x,                   \
                                   const uint8_t *restrict m, uint32_t n)     \
{                                                                             \
    double s0 = 0, s1 = 0, s2 = 0, s3 = 0, s4 = 0, s5 = 0, s6 = 0, s7 = 0;    \
    double q0 = 0, q1 = 0, q2 = 0, q3 = 0, q4 = 0, q5 = 0, q6 = 0, q7 = 0;    \
    double mu, d0, d1, d2, d3, d4, d5, d6, d7;                                \
    uint32_t i = 0, k = 0;                                                    \
    if (m) {                                                                  \
        for (; i + 8 <= n; i += 8) {                                          \
            s0 += m[i]     ? (double)x[i]     : 0.0;                          \
            s1 += m[i + 1] ? (double)x[i + 1] : 0.0;                          \
            s2 += m[i + 2] ? (double)x[i + 2] : 0.0;                          \
            s3 += m[i + 3] ? (double)x[i + 3] : 0.0;                          \
            s4 += m[i + 4] ? (double)x[i + 4] : 0.0;                          \
            s5 += m[i + 5] ? (double)x[i + 5] : 0.0;                          \
            s6 += m[i + 6] ? (double)x[i + 6] : 0.0;                          \
            s7 += m[i + 7] ? (double)x[i + 7] : 0.0;                          \
            k  += (m[i]     != 0) + (m[i + 1] != 0) +                         \
                  (m[i + 2] != 0) + (m[i + 3] != 0) +                         \
                  (m[i + 4] != 0) + (m[i + 5] != 0) +                         \
                  (m[i + 6] != 0) + (m[i + 7] != 0);                          \
        }                                                                     \
        for (; i < n; i++) {                                                  \
            s0 += m[i] ? (double)x[i] : 0.0;                                  \
            k  += (m[i] != 0);                                                \
        }                                                                     \
    } else {                                                                  \
        for (; i + 8 <= n; i += 8) {                                          \
            s0 += (double)x[i];     s1 += (double)x[i + 1];                   \
            s2 += (double)x[i + 2]; s3 += (double)x[i + 3];                   \
            s4 += (double)x[i + 4]; s5 += (double)x[i + 5];                   \
            s6 += (double)x[i + 6]; s7 += (double)x[i + 7];                   \
        }                                                                     \
        for (; i < n; i++)                                                    \
            s0 += (double)x[i];                                               \
        k = n;                                                                \
    }                                                                         \
    if (k < 2)                                                                \
        return NAN;                                                           \
    mu = (((s0 + s1) + (s2 + s3)) + ((s4 + s5) + (s6 + s7))) / (double)k;     \
    i = 0;                                                                    \
    if (m) {                                                                  \
        for (; i + 8 <= n; i += 8) {                                          \
            d0 = m[i]     ? (double)x[i]     - mu : 0.0;                      \
            d1 = m[i + 1] ? (double)x[i + 1] - mu : 0.0;                      \
            d2 = m[i + 2] ? (double)x[i + 2] - mu : 0.0;                      \
            d3 = m[i + 3] ? (double)x[i + 3] - mu : 0.0;                      \
            d4 = m[i + 4] ? (double)x[i + 4] - mu : 0.0;                      \
            d5 = m[i + 5] ? (double)x[i + 5] - mu : 0.0;                      \
            d6 = m[i + 6] ? (double)x[i + 6] - mu : 0.0;                      \
            d7 = m[i + 7] ? (double)x[i + 7] - mu : 0.0;                      \
            q0 += d0 * d0; q1 += d1 * d1; q2 += d2 * d2; q3 += d3 * d3;       \
            q4 += d4 * d4; q5 += d5 * d5; q6 += d6 * d6; q7 += d7 * d7;       \
        }                                                                     \
        for (; i < n; i++) {                                                  \
            d0 = m[i] ? (double)x[i] - mu : 0.0;                              \
            q0 += d0 * d0;                                                    \
        }                                                                     \
    } else {                                                                  \
        for (; i + 8 <= n; i += 8) {                                          \
            d0 = (double)x[i]     - mu; d1 = (double)x[i + 1] - mu;           \
            d2 = (double)x[i + 2] - mu; d3 = (double)x[i + 3] - mu;           \
            d4 = (double)x[i + 4] - mu; d5 = (double)x[i + 5] - mu;           \
            d6 = (double)x[i + 6] - mu; d7 = (double)x[i + 7] - mu;           \
            q0 += d0 * d0; q1 += d1 * d1; q2 += d2 * d2; q3 += d3 * d3;       \
            q4 += d4 * d4; q5 += d5 * d5; q6 += d6 * d6; q7 += d7 * d7;       \
        }                                                                     \
        for (; i < n; i++) {                                                  \
            d0 = (double)x[i] - mu;                                           \
            q0 += d0 * d0;                                                    \
        }                                                                     \
    }                                                                         \
    return (((q0 + q1) + (q2 + q3)) + ((q4 + q5) + (q6 + q7)))                \
           / (double)(k - 1);                                                 \
}
DF_NUMERIC_TYPES(DF_DEFINE_VARIANCE)

/* ---- B.5 DOT_PRODUCT ----
   FIFTEEN specialised kernels, not sixty-four: the eight same-type diagonals
   plus Float64 paired with each of the other seven. */
#define DF_DEFINE_DOT(name, atype, btype)                                     \
static double df_dot_##name(const atype *restrict xa,                         \
                            const btype *restrict xb,                         \
                            const uint8_t *restrict m, uint32_t n)            \
{                                                                             \
    double a0 = 0, a1 = 0, a2 = 0, a3 = 0, a4 = 0, a5 = 0, a6 = 0, a7 = 0;    \
    uint32_t i = 0;                                                           \
    if (m) {                                                                  \
        for (; i + 8 <= n; i += 8) {                                          \
            a0 += m[i]     ? (double)xa[i]     * (double)xb[i]     : 0.0;     \
            a1 += m[i + 1] ? (double)xa[i + 1] * (double)xb[i + 1] : 0.0;     \
            a2 += m[i + 2] ? (double)xa[i + 2] * (double)xb[i + 2] : 0.0;     \
            a3 += m[i + 3] ? (double)xa[i + 3] * (double)xb[i + 3] : 0.0;     \
            a4 += m[i + 4] ? (double)xa[i + 4] * (double)xb[i + 4] : 0.0;     \
            a5 += m[i + 5] ? (double)xa[i + 5] * (double)xb[i + 5] : 0.0;     \
            a6 += m[i + 6] ? (double)xa[i + 6] * (double)xb[i + 6] : 0.0;     \
            a7 += m[i + 7] ? (double)xa[i + 7] * (double)xb[i + 7] : 0.0;     \
        }                                                                     \
        for (; i < n; i++)                                                    \
            a0 += m[i] ? (double)xa[i] * (double)xb[i] : 0.0;                 \
    } else {                                                                  \
        for (; i + 8 <= n; i += 8) {                                          \
            a0 += (double)xa[i]     * (double)xb[i];                          \
            a1 += (double)xa[i + 1] * (double)xb[i + 1];                      \
            a2 += (double)xa[i + 2] * (double)xb[i + 2];                      \
            a3 += (double)xa[i + 3] * (double)xb[i + 3];                      \
            a4 += (double)xa[i + 4] * (double)xb[i + 4];                      \
            a5 += (double)xa[i + 5] * (double)xb[i + 5];                      \
            a6 += (double)xa[i + 6] * (double)xb[i + 6];                      \
            a7 += (double)xa[i + 7] * (double)xb[i + 7];                      \
        }                                                                     \
        for (; i < n; i++)                                                    \
            a0 += (double)xa[i] * (double)xb[i];                              \
    }                                                                         \
    return ((a0 + a1) + (a2 + a3)) + ((a4 + a5) + (a6 + a7));                 \
}
#define X(suffix, ctype, tag) DF_DEFINE_DOT(suffix##_##suffix, ctype, ctype)
DF_NUMERIC_TYPES(X)
#undef X
DF_DEFINE_DOT(f64_f32, double, float)
DF_DEFINE_DOT(f64_i32, double, int32_t)
DF_DEFINE_DOT(f64_u32, double, uint32_t)
DF_DEFINE_DOT(f64_i16, double, int16_t)
DF_DEFINE_DOT(f64_u16, double, uint16_t)
DF_DEFINE_DOT(f64_i8,  double, int8_t)
DF_DEFINE_DOT(f64_u8,  double, uint8_t)

/* 128 doubles = 1 KB per side, 2 KB of stack for the pair. Large enough that
   the type switch below costs ~0.02 ns/elem, small enough to stay in L1. */
#define DF_DOT_BLOCK 128

static void df_widen_block(double *restrict dst, const void *src, DFType t,
                           uint32_t off, uint32_t n)
{
    uint32_t i;
#define DF_WIDEN(TY)                                                          \
    do {                                                                      \
        const TY *s = (const TY *)src + off;                                  \
        for (i = 0; i < n; i++) dst[i] = (double)s[i];                        \
    } while (0)
    switch (t) {
    case DF_F64: DF_WIDEN(double);   break;
    case DF_F32: DF_WIDEN(float);    break;
    case DF_I32: DF_WIDEN(int32_t);  break;
    case DF_U32: DF_WIDEN(uint32_t); break;
    case DF_I16: DF_WIDEN(int16_t);  break;
    case DF_U16: DF_WIDEN(uint16_t); break;
    case DF_I8:  DF_WIDEN(int8_t);   break;
    default:     DF_WIDEN(uint8_t);  break;
    }
#undef DF_WIDEN
}

/* Fallback for mixed narrow pairs. Sums block by block, so the addition order
   differs from the specialised kernels -- already inside this module's stated
   reassociation tolerance. */
static double df_dot_generic(const void *pa, DFType ta,
                             const void *pb, DFType tb,
                             const uint8_t *restrict m, uint32_t n)
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

static double df_dot_dispatch(const void *pa, DFType ta,
                              const void *pb, DFType tb,
                              const uint8_t *m, uint32_t n)
{
    if (ta == tb) {
        switch (ta) {
#define X(suffix, ctype, tag)                                                 \
        case tag: return df_dot_##suffix##_##suffix((const ctype *)pa,        \
                                                    (const ctype *)pb, m, n);
        DF_NUMERIC_TYPES(X)
#undef X
        default: break;
        }
    } else if (ta == DF_F64) {
        const double *xa = (const double *)pa;
        switch (tb) {
        case DF_F32: return df_dot_f64_f32(xa, (const float *)pb, m, n);
        case DF_I32: return df_dot_f64_i32(xa, (const int32_t *)pb, m, n);
        case DF_U32: return df_dot_f64_u32(xa, (const uint32_t *)pb, m, n);
        case DF_I16: return df_dot_f64_i16(xa, (const int16_t *)pb, m, n);
        case DF_U16: return df_dot_f64_u16(xa, (const uint16_t *)pb, m, n);
        case DF_I8:  return df_dot_f64_i8(xa, (const int8_t *)pb, m, n);
        case DF_U8:  return df_dot_f64_u8(xa, (const uint8_t *)pb, m, n);
        default: break;
        }
    }
    return df_dot_generic(pa, ta, pb, tb, m, n);
}

/* ---- B.6 all and any ----
   Measured at 1M rows: `all` over an all-true mask (the adversarial case, the
   bypass never fires) costs 0.0155 flat / 0.0157 blocked / 0.391 early. */
#define DF_ALLANY_BLOCK 4096u

static int df_any_scan(const uint8_t *restrict m, uint32_t n)
{
    uint8_t a = 0;
    uint32_t i;
    for (i = 0; i < n; i++)
        a |= m[i];
    return a != 0;
}

static int df_all_scan(const uint8_t *restrict m, uint32_t n)
{
    uint8_t z = 0;
    uint32_t i;
    /* accumulate "a zero was seen". ANDing the raw bytes would be wrong: a
       mask byte of 2 is true, and 1 & 2 is 0. */
    for (i = 0; i < n; i++)
        z |= (uint8_t)(m[i] == 0);
    return z == 0;
}


static int df_any_block(const uint8_t *restrict m, uint32_t n)
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

static int df_all_block(const uint8_t *restrict m, uint32_t n)
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

/* ---- B.7 bitmask ----
   The result is a Uint32Array of ceil(nrows / 32) words; bits past nrows in the
   last word are zero. Deliberately NOT a single JS number. */
#define DF_SWAR_LOW7   0x7f7f7f7f7f7f7f7full
#define DF_SWAR_HIGH   0x8080808080808080ull
#define DF_SWAR_GATHER 0x0002040810204081ull
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define DF_PACK_SWAR 1
#endif

static void df_pack_bits(uint32_t *restrict dst, const uint8_t *restrict m,
                         uint32_t n)
{
    uint32_t i = 0;
#ifdef DF_PACK_SWAR
    for (; i + 32 <= n; i += 32) {
        uint64_t v0, v1, v2, v3;
        /* memcpy, not a pointer cast: the cast is UB and traps on
           strict-alignment targets. It compiles to the same load. */
        memcpy(&v0, m + i,      8);
        memcpy(&v1, m + i + 8,  8);
        memcpy(&v2, m + i + 16, 8);
        memcpy(&v3, m + i + 24, 8);
        v0 = (((v0 & DF_SWAR_LOW7) + DF_SWAR_LOW7) | v0) & DF_SWAR_HIGH;
        v1 = (((v1 & DF_SWAR_LOW7) + DF_SWAR_LOW7) | v1) & DF_SWAR_HIGH;
        v2 = (((v2 & DF_SWAR_LOW7) + DF_SWAR_LOW7) | v2) & DF_SWAR_HIGH;
        v3 = (((v3 & DF_SWAR_LOW7) + DF_SWAR_LOW7) | v3) & DF_SWAR_HIGH;
        dst[i >> 5] = (uint32_t)(((v0 * DF_SWAR_GATHER) >> 56)        |
                                 ((v1 * DF_SWAR_GATHER) >> 56) <<  8  |
                                 ((v2 * DF_SWAR_GATHER) >> 56) << 16  |
                                 ((v3 * DF_SWAR_GATHER) >> 56) << 24);
    }
#endif
    /* tail, and the whole function on a big-endian host. ORs in, so the
       destination must arrive zeroed -- it comes from calloc. */
    for (; i < n; i++)
        dst[i >> 5] |= (uint32_t)(m[i] != 0) << (i & 31);
}
/* These entry points sit AFTER the comparisons section rather than before it,
   which is where they otherwise belong: dyn_df_bitmask calls df_to_typed_array,
   declared there. */

enum { DF_VAR_SAMPLE, DF_VAR_STDDEV };
enum { DF_BIT_AND, DF_BIT_OR, DF_BIT_XOR };
enum { DF_ALL, DF_ANY };

/* product(col[, mask]) -> Number. Identity 1, so an empty column or a mask that
   selects nothing returns 1, exactly as `[].reduce((a, b) => a * b, 1)` does. */
static JSValue dyn_df_product(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    DataFrame *df;
    DFBound b;
    const uint8_t *mask;
    int idx, ok;
    uint32_t count = 0;    /* Batch 1's masked kernels report it; unused here */
    double acc;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    /* coerce EVERY argument before binding any column pointer. */
    idx = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (idx < 0)
        return JS_EXCEPTION;
    mask = df_mask_arg(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, df->nrows, &ok);
    if (!ok)
        return JS_EXCEPTION;

    /* no JS may run from here to the end of the loop. */
    if (dyn_df_bind(ctx, df, idx, &b))
        return JS_EXCEPTION;

#define X(suffix, ctype, tag)                                                 \
    case tag:                                                                 \
        acc = mask                                                            \
            ? df_product_masked_##suffix((const ctype *)b.p, mask, b.n,       \
                                         &count)                              \
            : df_product_##suffix((const ctype *)b.p, b.n);                   \
        break;
    switch (b.type) {
    DF_NUMERIC_TYPES(X)
    default:
        return JS_ThrowTypeError(ctx, "cannot reduce a string column");
    }
#undef X
    (void)count;
    return JS_NewFloat64(ctx, acc);
}

/* variance(col[, mask]) / stddev(col[, mask]) -> Number.
   Sample variance, n-1 divisor; fewer than two selected rows gives NaN. */
static JSValue dyn_df_variance(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv, int magic)
{
    DataFrame *df;
    DFBound b;
    const uint8_t *mask;
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

#define X(suffix, ctype, tag)                                                 \
    case tag:                                                                 \
        v = df_variance_##suffix((const ctype *)b.p, mask, b.n);              \
        break;
    switch (b.type) {
    DF_NUMERIC_TYPES(X)
    default:
        return JS_ThrowTypeError(ctx, "cannot reduce a string column");
    }
#undef X
    /* sqrt(NaN) is NaN, so the n < 2 answer carries through unchanged. */
    return JS_NewFloat64(ctx, magic == DF_VAR_STDDEV ? sqrt(v) : v);
}

/* DOT_PRODUCT(colA, colB[, mask]) -> Number. */
static JSValue dyn_df_dot_product(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    DataFrame *df;
    DFBound ba, bb;
    const uint8_t *mask;
    const void *pa, *pb;
    DFType ta, tb;
    int ia, ib, ok;
    uint32_t n;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    /* BOTH column names are coerced before EITHER column is bound: ToCString
       runs user JS that can detach the buffer the first bind aliased. */
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

    pa = ba.p; ta = ba.type;
    pb = bb.p; tb = bb.type;
    n = ba.n < bb.n ? ba.n : bb.n;
    if (ta != DF_F64 && tb == DF_F64) {
        const void *tp = pa;
        DFType tt = ta;
        pa = pb; ta = tb;
        pb = tp; tb = tt;
    }
    return JS_NewFloat64(ctx, df_dot_dispatch(pa, ta, pb, tb, mask, n));
}

/* BITWISE_AND/Or/Xor(col[, mask]) -> Number. An empty column, or a mask
   selecting nothing, returns the identity: -1 for AND on a signed column
   (4294967295 on an unsigned one), 0 for OR and XOR. */
static JSValue dyn_df_bitwise(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv, int magic)
{
    static const char *const names[] = { "BITWISE_AND", "BITWISE_OR",
                                         "BITWISE_XOR" };
    DataFrame *df;
    DFBound b;
    const uint8_t *mask;
    int idx, ok;
    uint32_t count = 0;    /* Batch 1's masked kernels report it; unused here */
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

/* One switch both dispatches and refuses, so the "is this an integer column"
   predicate exists once and cannot drift from the kernels DF_INT_TYPES generated
   for it. */
#define DF_BIT_BY_OP(suffix, ctype)                                           \
    do {                                                                      \
        const ctype *x = (const ctype *)b.p;                                  \
        switch (magic) {                                                      \
        case DF_BIT_AND:                                                      \
            acc = mask ? df_band_masked_##suffix(x, mask, b.n, &count)        \
                       : df_band_##suffix(x, b.n);                            \
            break;                                                            \
        case DF_BIT_OR:                                                       \
            acc = mask ? df_bor_masked_##suffix(x, mask, b.n, &count)         \
                       : df_bor_##suffix(x, b.n);                             \
            break;                                                            \
        default:                                                              \
            acc = mask ? df_bxor_masked_##suffix(x, mask, b.n, &count)        \
                       : df_bxor_##suffix(x, b.n);                            \
            break;                                                            \
        }                                                                     \
    } while (0)
#define X(suffix, ctype, tag) case tag: DF_BIT_BY_OP(suffix, ctype); break;
    switch (b.type) {
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

    /* AND's identity is all-ones at the COLUMN's width, not at 32 bits: an
       empty Uint8Array answers 255. OR and XOR cannot exceed the width, so the
       mask is a no-op for them. */
    switch (b.type) {
    case DF_U8:  acc &= 0xffu;   break;
    case DF_U16: acc &= 0xffffu; break;
    default: break;
    }

    /* An unsigned column returns the unsigned value, which JS's own bitwise
       operators cannot express -- they coerce to int32 -- but which is what a
       Uint32Array column means. */
    switch (b.type) {
    case DF_I32: case DF_I16: case DF_I8:
        return JS_NewInt32(ctx, (int32_t)acc);
    default:
        return JS_NewUint32(ctx, acc);
    }
}

/* all(mask) / any(mask) -> Boolean. Vacuous over zero rows: all -> true,
   any -> false. Any nonzero byte counts as true, as everywhere else here. */
static JSValue dyn_df_all_any(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv, int magic)
{
    DataFrame *df;
    const uint8_t *mask;
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
    /* Blocked scan only: it matches the best of a pure scan and an early exit
       at both extremes, so the crossover lives in DF_ALLANY_BLOCK. */
    n = df->nrows;
    res = (magic & 1) == DF_ANY ? df_any_block(mask, n) : df_all_block(mask, n);
    return JS_NewBool(ctx, res);
}

/* bitmask(mask) -> Uint32Array of ceil(rows/32) words, LSB first. */
static JSValue dyn_df_bitmask(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    DataFrame *df;
    const uint8_t *mask;
    uint32_t *dst;
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
                                 "bytes is required", df->nrows);

    n = df->nrows;
    nwords = (n + 31u) >> 5;
    /* calloc rather than malloc + memset: the packing loop writes whole words
       but ORs into the last, partial one, and fresh pages arrive zeroed. */
    dst = calloc(nwords ? nwords : 1, sizeof(uint32_t));
    if (!dst)
        return JS_ThrowOutOfMemory(ctx);
    df_pack_bits(dst, mask, n);
    /* frees dst either way; no JS-visible allocation happened while the mask
       pointer was held. */
    return df_to_typed_array(ctx, dst, (size_t)nwords * sizeof(uint32_t),
                             JS_TYPED_ARRAY_UINT32);
}

/* ------------------------------------------------------------ comparisons */

enum { DF_GT, DF_GE, DF_LT, DF_LE, DF_EQ, DF_NE };

/* Wrap a native buffer as a fresh TypedArray (copying it in). This is the only
 * way a result leaves the module: nothing native ever escapes into a JS value.
 * Frees `p` either way. */
static JSValue df_to_typed_array(JSContext *ctx, void *p, size_t nbytes,
                                 JSTypedArrayEnum type)
{
    JSValueConst args[3];
    JSValue ab, out;

    ab = JS_NewArrayBufferCopy(ctx, (const uint8_t *)p, nbytes);
    free(p);
    if (JS_IsException(ab))
        return ab;
    args[0] = ab;
    args[1] = JS_UNDEFINED;
    args[2] = JS_UNDEFINED;
    out = JS_NewTypedArray(ctx, 3, args, type);
    JS_FreeValue(ctx, ab);
    return out;
}

/* Build a Uint8Array mask of rows satisfying `col <op> value`. One pass, no
 * JS values produced per row -- which is the entire difference from the JS
 * loop it replaces. */
static JSValue dyn_df_compare(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv, int magic)
{
    DataFrame *df;
    DFBound b;
    double thr;
    int idx;
    uint32_t i, n;
    uint8_t *dst;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    idx = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (idx < 0)
        return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &thr, argc > 1 ? argv[1] : JS_UNDEFINED))
        return JS_EXCEPTION;

    /* Compute into a native buffer and only then build the JS result: that way
       no JS-visible allocation (which can trigger GC and, through a finalizer,
       user code) happens while a column pointer is held. */
    n = df->nrows;
    dst = malloc(n ? n : 1);
    if (!dst)
        return JS_ThrowOutOfMemory(ctx);
    if (dyn_df_bind(ctx, df, idx, &b)) {
        free(dst);
        return JS_EXCEPTION;
    }
    /* A string column holds DICTIONARY CODES, so comparing it numerically
       answers about the encoding, not the data: ["10","200","30"] > 1 read
       [0,0,1]. Refuse, and name the column the caller meant to build. */
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

/* Compare in DOUBLE, not in the column's type. The cast was also undefined
   behaviour -- C17 6.3.1.4: a floating value that cannot be represented in the
   integer type. */
#define DF_CMP_LOOP(TY, OP)                                                   \
    do {                                                                      \
        const TY *x = b.p;                                                    \
        for (i = 0; i < b.n; i++) dst[i] = (uint8_t)((double)x[i] OP thr);     \
    } while (0)
#define DF_CMP_BY_OP(TY)                                                      \
    do {                                                                      \
        switch (magic) {                                                      \
        case DF_GT: DF_CMP_LOOP(TY, >);  break;                               \
        case DF_GE: DF_CMP_LOOP(TY, >=); break;                               \
        case DF_LT: DF_CMP_LOOP(TY, <);  break;                               \
        case DF_LE: DF_CMP_LOOP(TY, <=); break;                               \
        case DF_EQ: DF_CMP_LOOP(TY, ==); break;                               \
        default:    DF_CMP_LOOP(TY, !=); break;                               \
        }                                                                     \
    } while (0)

    switch (b.type) {
    case DF_F64: DF_CMP_BY_OP(double); break;
    case DF_F32: DF_CMP_BY_OP(float); break;
    case DF_I32: case DF_STR: DF_CMP_BY_OP(int32_t); break;
    case DF_U32: DF_CMP_BY_OP(uint32_t); break;
    case DF_I16: DF_CMP_BY_OP(int16_t); break;
    case DF_U16: DF_CMP_BY_OP(uint16_t); break;
    case DF_I8:  DF_CMP_BY_OP(int8_t); break;
    default:     DF_CMP_BY_OP(uint8_t); break;
    }
#undef DF_CMP_BY_OP
#undef DF_CMP_LOOP
    return df_to_typed_array(ctx, dst, n, JS_TYPED_ARRAY_UINT8);
}
/* ---------------------------------------------------- element-wise maps */

/* - integer division by zero and INT32_MIN / -1 do not exist in double (they are
   +-Infinity and NaN), so no kernel can trap and none relies on -fwrapv. -
   abs(INT32_MIN) is 2147483648, which does not fit an int32. */

/* Batch 1 owns this list; the fallback exists only so this section builds if it
 * lands first. Delete the whole #ifndef once DF_NUMERIC_TYPES is in the file. */

/* Paste through one extra expansion, because ## suppresses expansion of its own
 * operands and the op name arrives as a macro. */
#define DF_CAT_(a, b)   a##b
#define DF_CAT(a, b)    DF_CAT_(a, b)
#define DF_KERNEL(kind, name, suffix)                                         \
    DF_CAT(DF_CAT(df_, kind), DF_CAT(DF_CAT(_, name), DF_CAT(_, suffix)))

typedef void (*DFMapFn)(const void *src, double *restrict dst, uint32_t n);
typedef void (*DFMapKFn)(const void *src, double *restrict dst, uint32_t n,
                         double k0, double k1);
typedef void (*DFMaskFn)(const void *src, uint8_t *restrict dst, uint32_t n,
                         double k0, double k1);
typedef void (*DFCombFn)(const double *a, const void *src,
                         double *restrict dst, uint32_t n);

/* `restrict` is on the OUTPUT pointer only. It is unambiguously true (the output
   is a fresh malloc) and it is sufficient: it tells the compiler a store through
   dst cannot alias anything read through src, which is the whole hazard. */
#define DF_DEFINE_MAP(name, suffix, ctype, EXPR)                              \
    static void DF_KERNEL(map, name, suffix)(const void *src,                 \
                                             double *restrict dst, uint32_t n)\
    {                                                                         \
        const ctype *x = (const ctype *)src;                                  \
        uint32_t i;                                                           \
        for (i = 0; i < n; i++) {                                             \
            double v = (double)x[i];                                          \
            dst[i] = (EXPR);                                                  \
        }                                                                     \
    }

#define DF_DEFINE_MAPK(name, suffix, ctype, EXPR)                             \
    static void DF_KERNEL(mapk, name, suffix)(const void *src,                \
                                              double *restrict dst,           \
                                              uint32_t n, double k0, double k1)\
    {                                                                         \
        const ctype *x = (const ctype *)src;                                  \
        uint32_t i;                                                           \
        for (i = 0; i < n; i++) {                                             \
            double v = (double)x[i];                                          \
            dst[i] = (EXPR);                                                  \
        }                                                                     \
    }

#define DF_DEFINE_MASK(name, suffix, ctype, EXPR)                             \
    static void DF_KERNEL(mask, name, suffix)(const void *src,                \
                                              uint8_t *restrict dst,          \
                                              uint32_t n, double k0, double k1)\
    {                                                                         \
        const ctype *x = (const ctype *)src;                                  \
        uint32_t i;                                                           \
        for (i = 0; i < n; i++) {                                             \
            double v = (double)x[i];                                          \
            dst[i] = (uint8_t)(EXPR);                                         \
        }                                                                     \
    }

#define DF_DEFINE_COMB(name, suffix, ctype, EXPR)                             \
    static void DF_KERNEL(comb, name, suffix)(const double *a, const void *src,\
                                              double *restrict dst, uint32_t n)\
    {                                                                         \
        const ctype *y = (const ctype *)src;                                  \
        uint32_t i;                                                           \
        for (i = 0; i < n; i++) {                                             \
            double p = a[i], q = (double)y[i];                                \
            dst[i] = (EXPR);                                                  \
        }                                                                     \
    }

/* One instantiation per numeric column type, plus the matching dispatch entry.
 * DF_OP_NAME / DF_OP_EXPR exist only between the #define and the #undef that
 * bracket each op below. */
#define DF_MAP_ONE(suffix, ctype, tag)   DF_DEFINE_MAP(DF_OP_NAME, suffix, ctype, DF_OP_EXPR)
#define DF_MAPK_ONE(suffix, ctype, tag)  DF_DEFINE_MAPK(DF_OP_NAME, suffix, ctype, DF_OP_EXPR)
#define DF_MASK_ONE(suffix, ctype, tag)  DF_DEFINE_MASK(DF_OP_NAME, suffix, ctype, DF_OP_EXPR)
#define DF_COMB_ONE(suffix, ctype, tag)  DF_DEFINE_COMB(DF_OP_NAME, suffix, ctype, DF_OP_EXPR)
#define DF_MAP_ENTRY(suffix, ctype, tag)   [tag] = DF_KERNEL(map,  DF_OP_NAME, suffix),
#define DF_MAPK_ENTRY(suffix, ctype, tag)  [tag] = DF_KERNEL(mapk, DF_OP_NAME, suffix),
#define DF_MASK_ENTRY(suffix, ctype, tag)  [tag] = DF_KERNEL(mask, DF_OP_NAME, suffix),
#define DF_COMB_ENTRY(suffix, ctype, tag)  [tag] = DF_KERNEL(comb, DF_OP_NAME, suffix),

/* DF_STR never reaches a table: every entry point rejects a string column first. */

/* Math.round is half toward +INFINITY; C round() is half AWAY FROM ZERO, so
   round(-1.5) is -1 in JS and -2 in C. The `v - f >= 0.5` form is also correct
   at 0.49999999999999994, where the textbook floor(v + 0.5) rounds up to 1. */
static inline double df_round_js(double v)
{
    double f = floor(v);
    double r = (v - f >= 0.5) ? f + 1.0 : f;
    return (r == 0.0) ? v * 0.0 : r;
}

/* Math.sign: NaN -> NaN, -0 -> -0, +0 -> +0. The usual branchless
 * `(v > 0) - (v < 0)` loses both of those. */
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

/* Scalar right-hand operand. */
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
#define DF_OP_EXPR v * k0
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

/* NaN is REPLACED, everything else is copied. On an integer column the test is
 * statically false and the kernel folds to the plain widening copy -- the
 * compiler applies the bypass, nothing here special-cases it. */
#define DF_OP_NAME fillna
#define DF_OP_EXPR (v != v) ? k0 : v
DF_NUMERIC_TYPES(DF_MAPK_ONE)
static const DFMapKFn df_mapk_fillna_tab[DF_STR] = { DF_NUMERIC_TYPES(DF_MAPK_ENTRY) };
#undef DF_OP_NAME
#undef DF_OP_EXPR

/* NaN PROPAGATES: both comparisons are false, so it falls through unchanged --
 * which is why this is not written with fmin/fmax, where fmin(NaN, x) is x and
 * a NaN would silently become a bound. -0 in range is preserved. */
#define DF_OP_NAME clip
#define DF_OP_EXPR (v < k0) ? k0 : ((v > k1) ? k1 : v)
DF_NUMERIC_TYPES(DF_MAPK_ONE)
static const DFMapKFn df_mapk_clip_tab[DF_STR] = { DF_NUMERIC_TYPES(DF_MAPK_ENTRY) };
#undef DF_OP_NAME
#undef DF_OP_EXPR

/* That is the bypass of section 3, found by the compiler because the summary
   (the column type) is already loop-invariant. */
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

/* Inclusive at both ends, like Series.between. NaN compares false against
 * everything, so a NaN element and a NaN bound both select nothing. */
#define DF_OP_NAME between
#define DF_OP_EXPR v >= k0 && v <= k1
DF_NUMERIC_TYPES(DF_MASK_ONE)
static const DFMaskFn df_mask_between_tab[DF_STR] = { DF_NUMERIC_TYPES(DF_MASK_ENTRY) };
#undef DF_OP_NAME
#undef DF_OP_EXPR

/* Column OP column. The LEFT operand is always f64 -- a non-f64 left column is
 * widened into scratch once by the caller -- so the kernel is specialised on
 * the RIGHT type only: 5 ops x 8 types, not 5 x 64. */
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
#define DF_OP_EXPR p * q
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

/* where(): a branchless select, with BOTH sides loaded unconditionally.
   Branchless is chosen because its cost does not depend on the data: it is flat
   at 0.396 across every mask, where the branchy form ranges 0.30 to 0.42. */
static void df_where_cc(const uint8_t *m, const double *a, const double *b,
                        double *restrict dst, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++) {
        double p = a[i], q = b[i];
        dst[i] = m[i] != 0 ? p : q;
    }
}

static void df_where_cs(const uint8_t *m, const double *a, double kb,
                        double *restrict dst, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++) {
        double p = a[i];
        dst[i] = m[i] != 0 ? p : kb;
    }
}

static void df_where_sc(const uint8_t *m, double ka, const double *b,
                        double *restrict dst, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++) {
        double q = b[i];
        dst[i] = m[i] != 0 ? ka : q;
    }
}

static void df_where_ss(const uint8_t *m, double ka, double kb,
                        double *restrict dst, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++)
        dst[i] = m[i] != 0 ? ka : kb;
}

/* -------------------------------------------------- map entry points */

enum { DF_MAP_ABS, DF_MAP_ROUND, DF_MAP_FLOOR, DF_MAP_CEIL,
       DF_MAP_SQRT, DF_MAP_LOG, DF_MAP_EXP, DF_MAP_SIGN, DF_MAP_N };

static const DFMapFn *const df_map1_tab[DF_MAP_N] = {
    df_map_abs_tab,  df_map_round_tab, df_map_floor_tab, df_map_ceil_tab,
    df_map_sqrt_tab, df_map_log_tab,   df_map_exp_tab,   df_map_sign_tab,
};
static const char *const df_map1_name[DF_MAP_N] = {
    "ABS", "ROUND", "FLOOR", "CEIL", "SQRT", "LOG", "EXP", "SIGN",
};

/* Column ops. The five that accept a column on the right come first, so the
 * combine table can be exactly that long and an out-of-range magic is a
 * compile error rather than a call through NULL. */
enum { DF_BIN_ADD, DF_BIN_SUB, DF_BIN_MUL, DF_BIN_DIV, DF_BIN_POW,
       DF_BIN_NCOMB,
       DF_BIN_RSUB = DF_BIN_NCOMB, DF_BIN_RDIV, DF_BIN_N };

static const DFCombFn *const df_comb_tab[DF_BIN_NCOMB] = {
    df_comb_add_tab, df_comb_sub_tab, df_comb_mul_tab, df_comb_div_tab,
    df_comb_pow_tab,
};
static const DFMapKFn *const df_bin_scalar_tab[DF_BIN_N] = {
    df_mapk_addk_tab,  df_mapk_subk_tab,  df_mapk_mulk_tab, df_mapk_divk_tab,
    df_mapk_powk_tab,  df_mapk_rsubk_tab, df_mapk_rdivk_tab,
};
static const char *const df_bin_name[DF_BIN_N] = {
    "ADD", "SUB", "MUL", "DIV", "POW", "RSUB", "RDIV",
};

_Static_assert(countof(df_map1_tab) == countof(df_map1_name),
               "map table and name table must agree");
_Static_assert(countof(df_bin_scalar_tab) == countof(df_bin_name),
               "binary table and name table must agree");
_Static_assert(DF_BIN_NCOMB < DF_BIN_N,
               "the scalar-only ops must follow the column-capable ones");

/* Allocate a native output buffer. malloc, never a JS allocation: a JS-visible
 * allocation can collect, a finalizer can run user code, and user code can
 * detach a column this call is about to read. */
static void *df_out_alloc(JSContext *ctx, uint32_t n, size_t esz)
{
    void *p = malloc(n ? (size_t)n * esz : 1);
    if (!p)
        JS_ThrowOutOfMemory(ctx);
    return p;
}

/* Bind a column and refuse a string one: a dictionary code is not a number the
 * caller stored, so arithmetic on it would be a plausible wrong answer. */
static int df_bind_numeric(JSContext *ctx, const DataFrame *df, int idx,
                           DFBound *b, const char *op)
{
    if (dyn_df_bind(ctx, df, idx, b))
        return -1;
    if (b->type == DF_STR) {
        JS_ThrowTypeError(ctx, "%s: cannot apply to a string column", op);
        return -1;
    }
    return 0;
}

/* How many rows the kernel may write, filling any it cannot with NaN. The result
   is always `nrows` long so it lines up with the frame it came from and can be
   handed straight back to `new DataFrame`. */
static uint32_t df_map_span(uint32_t nrows, uint32_t avail, double *dst)
{
    uint32_t n = avail < nrows ? avail : nrows, i;
    for (i = n; i < nrows; i++)
        dst[i] = NAN;
    return n;
}

/* abs/round/floor/ceil/sqrt/log/exp/sign -> Float64Array */
static JSValue dyn_df_map1(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv, int magic)
{
    DataFrame *df;
    DFBound b;
    double *dst;
    uint32_t n, span;
    int idx;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    idx = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (idx < 0)
        return JS_EXCEPTION;

    n = df->nrows;
    dst = df_out_alloc(ctx, n, sizeof(double));
    if (!dst)
        return JS_EXCEPTION;
    /* no JS may run from here to the end of the kernel. */
    if (df_bind_numeric(ctx, df, idx, &b, df_map1_name[magic])) {
        free(dst);
        return JS_EXCEPTION;
    }
    span = df_map_span(n, b.n, dst);
    df_map1_tab[magic][b.type](b.p, dst, span);
    return df_to_typed_array(ctx, dst, (size_t)n * sizeof(double),
                             JS_TYPED_ARRAY_FLOAT64);
}

/* isna/notna -> Uint8Array, in the same one-byte-per-row form as gt/ge/... */
static JSValue dyn_df_isna(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv, int magic)
{
    DataFrame *df;
    DFBound b;
    uint8_t *dst;
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

/* between(col, lo, hi) -> Uint8Array. Inclusive both ends. An inverted range
 * selects nothing, which is a meaningful answer -- unlike clip, where it would
 * produce values outside the range, so only clip refuses it. */
static JSValue dyn_df_between(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    DataFrame *df;
    DFBound b;
    uint8_t *dst;
    double lo, hi;
    uint32_t n;
    int idx;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    idx = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (idx < 0)
        return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &lo, argc > 1 ? argv[1] : JS_UNDEFINED) ||
        JS_ToFloat64(ctx, &hi, argc > 2 ? argv[2] : JS_UNDEFINED))
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

/* clip(col, lo, hi) -> Float64Array. NaN elements pass through unchanged. */
static JSValue dyn_df_clip(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    DataFrame *df;
    DFBound b;
    double lo, hi, *dst;
    uint32_t n, span;
    int idx;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    idx = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (idx < 0)
        return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &lo, argc > 1 ? argv[1] : JS_UNDEFINED) ||
        JS_ToFloat64(ctx, &hi, argc > 2 ? argv[2] : JS_UNDEFINED))
        return JS_EXCEPTION;
    /* Checked ONCE per call, not per element. A NaN bound would silently turn
       clip into a copy (every comparison against NaN is false) and an inverted
       range would return values outside it -- clip(3, 5, 1) is 5. */
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
    return df_to_typed_array(ctx, dst, (size_t)n * sizeof(double),
                             JS_TYPED_ARRAY_FLOAT64);
}

/* fillna(col, value) -> Float64Array. A NaN `value` is legal and is a no-op. */
static JSValue dyn_df_fillna(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    DataFrame *df;
    DFBound b;
    double fill, *dst;
    uint32_t n, span;
    int idx;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    idx = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (idx < 0)
        return JS_EXCEPTION;
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
    return df_to_typed_array(ctx, dst, (size_t)n * sizeof(double),
                             JS_TYPED_ARRAY_FLOAT64);
}

/* rsub and rdiv exist because subtraction and division are not commutative and
   `100 - col` has no cheap spelling otherwise; they take a number only, since
   `rsub(a, b)` on two columns is just `sub(b, a)`. */
static JSValue dyn_df_binary(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv, int magic)
{
    DataFrame *df;
    DFBound a, b;
    JSValueConst rhs;
    double k = 0, *dst;
    uint32_t n, span;
    int ia, ib = -1, rhs_is_col;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    /* coerce EVERY argument before binding any column pointer. */
    ia = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (ia < 0)
        return JS_EXCEPTION;
    rhs = argc > 1 ? argv[1] : JS_UNDEFINED;
    rhs_is_col = JS_IsString(rhs);
    if (rhs_is_col) {
        if (magic >= DF_BIN_NCOMB)
            return JS_ThrowTypeError(ctx, "%s: the right operand must be a "
                                     "number; for two columns use %s with them "
                                     "swapped", df_bin_name[magic],
                                     magic == DF_BIN_RSUB ? "SUB" : "DIV");
        ib = df_col_arg(ctx, df, rhs);
        if (ib < 0)
            return JS_EXCEPTION;
    } else if (JS_ToFloat64(ctx, &k, rhs)) {
        return JS_EXCEPTION;
    }

    n = df->nrows;
    dst = df_out_alloc(ctx, n, sizeof(double));
    if (!dst)
        return JS_EXCEPTION;
    /* no JS may run from here to the end of the kernel. */
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
            df_comb_tab[magic][b.type]((const double *)a.p, b.p, dst, span);
        } else {
            /* Widen the left operand once rather than emitting a kernel for
               all 64 type PAIRS. An f64 left operand -- which is what any
               column a map produced is -- takes the one-pass path above. */
            double *tmp = df_out_alloc(ctx, span, sizeof(double));
            if (!tmp)
                goto fail;
            df_map_widen_tab[a.type](a.p, tmp, span);
            df_comb_tab[magic][b.type](tmp, b.p, dst, span);
            free(tmp);
        }
    }
    return df_to_typed_array(ctx, dst, (size_t)n * sizeof(double),
                             JS_TYPED_ARRAY_FLOAT64);
 fail:
    free(dst);
    return JS_EXCEPTION;
}

/* where(mask, a, b) -> Float64Array: a where the mask byte is nonzero, else b.
 * Each of `a` and `b` is a column name or a number. */
static JSValue dyn_df_where(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    DataFrame *df;
    DFBound ba, bb;
    const uint8_t *mask;
    const double *pa = NULL, *pb = NULL;
    double ka = 0, kb = 0, *dst = NULL, *wa = NULL, *wb = NULL;
    uint32_t n, span, avail;
    int ia = -1, ib = -1, ok;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    /* The operands are coerced BEFORE the mask pointer is taken. */
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
    /* no JS may run from here to the end of the kernel. */
    avail = n;
    if (ia >= 0) {
        if (df_bind_numeric(ctx, df, ia, &ba, "WHERE"))
            goto fail;
        if (ba.n < avail) avail = ba.n;
    }
    if (ib >= 0) {
        if (df_bind_numeric(ctx, df, ib, &bb, "WHERE"))
            goto fail;
        if (bb.n < avail) avail = bb.n;
    }
    span = df_map_span(n, avail, dst);
    /* A non-f64 operand is widened once into scratch; the select itself is one
       f64 kernel per (column, scalar) shape rather than one per type pair. */
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
    if (pa && pb)   df_where_cc(mask, pa, pb, dst, span);
    else if (pa)    df_where_cs(mask, pa, kb, dst, span);
    else if (pb)    df_where_sc(mask, ka, pb, dst, span);
    else            df_where_ss(mask, ka, kb, dst, span);
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

/* ---------------------------------------------------------------- group by */

/* Group keys as uint32 with the type switch hoisted out of the loop. Keys are
   integers or dictionary codes, so the narrowing is exact, and a negative key
   was refused before this runs. NULL means "not worth it": caller uses df_get. */
#define DF_KEYS_ONE(CTYPE)                                                    \
    do { const CTYPE *x = b->p;                                               \
         for (i = 0; i < n; i++) k[i] = (uint32_t)(double)x[i]; } while (0)

static uint32_t *df_keys_u32(const DFBound *b, uint32_t n)
{
    uint32_t *k, i;

    if (!n)
        return NULL;
    k = malloc((size_t)n * sizeof(*k));
    if (!k)
        return NULL;
    switch (b->type) {
    case DF_I32: case DF_STR: DF_KEYS_ONE(int32_t);  break;
    case DF_U32:              DF_KEYS_ONE(uint32_t); break;
    case DF_I16:              DF_KEYS_ONE(int16_t);  break;
    case DF_U16:              DF_KEYS_ONE(uint16_t); break;
    case DF_I8:               DF_KEYS_ONE(int8_t);   break;
    case DF_U8:               DF_KEYS_ONE(uint8_t);  break;
    default:                  DF_KEYS_ONE(double);   break;
    }
    return k;
}
#undef DF_KEYS_ONE

/* *pnkeys is how many groups the caller is told about; *pngroups is that clamped
   to at least one, because calloc(0) may return NULL and that would read as
   exhaustion. */
static int dfc_group_count(JSContext *ctx, const DataFrame *df, int ki,
                           uint32_t *pnkeys, uint32_t *pngroups)
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
        for (i = 0; i < b.n; i++) {
            double v = df_get(b.p, b.type, i);
            if (v < 0) {
                JS_ThrowRangeError(ctx, "negative group key");
                return -1;
            }
            if (v > mx)
                mx = v;
        }
        if (mx + 1 > (double)DF_MAX_GROUPS) {
            JS_ThrowRangeError(ctx, "too many groups (max %d)", DF_MAX_GROUPS);
            return -1;
        }
        /* No rows means no groups. Deriving the count from the maximum alone
           reports one group for an empty column, where the string path -- which
           reads a dictionary length -- correctly reports none. */
        ngroups = b.n ? (uint32_t)mx + 1 : 0;
    }
    *pnkeys = ngroups;
    *pngroups = ngroups ? ngroups : 1;
    return 0;
}

/* That is the whole reason string columns are dictionary-encoded on
   construction. */
static JSValue dyn_df_group_by_sum(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    DataFrame *df;
    DFBound kb, vb;
    const uint8_t *mask;
    int ki, vi, ok;
    uint32_t i, ngroups, nkeys, g;
    double *acc = NULL;
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
        uint32_t *gk = df_keys_u32(&kb, n);
        if (vb.type == DF_F64) {
            const double *x = vb.p;
            for (i = 0; i < n; i++) {
                if (mask && !mask[i]) continue;
                g = gk ? gk[i] : (uint32_t)df_get(kb.p, kb.type, i);
                if (g < ngroups) acc[g] += x[i];
            }
        } else {
            for (i = 0; i < n; i++) {
                if (mask && !mask[i]) continue;
                g = gk ? gk[i] : (uint32_t)df_get(kb.p, kb.type, i);
                if (g < ngroups) acc[g] += df_get(vb.p, vb.type, i);
            }
        }
        free(gk);
    }
    /* the accumulator becomes the result buffer; nothing native escapes. */
    vals = df_to_typed_array(ctx, acc, (size_t)nkeys * sizeof(double),
                             JS_TYPED_ARRAY_FLOAT64);
    acc = NULL;
    if (JS_IsException(vals))
        goto fail;

    /* keys: the dictionary strings, or the integer codes. */
    keys = JS_NewArray(ctx);
    if (JS_IsException(keys))
        goto fail;
    for (i = 0; i < nkeys; i++) {
        JSValue k;
        if (df->cols[ki].type == DF_STR)
            k = JS_NewString(ctx, df->cols[ki].dict[i]);
        else
            k = JS_NewInt64(ctx, i);
        if (JS_IsException(k) ||
            JS_DefinePropertyValueUint32(ctx, keys, i, k, JS_PROP_C_W_E) < 0)
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


/* ==== generated family g1 ====. */

/* ---- ordering and quantiles ----
   pandas returns NaN for the median of an empty series; the divergence is
   chosen, not overlooked, so do not "fix" it. - A string column is refused. /. */

typedef struct {
    double   key;
    uint32_t idx;               /* row in the FRAME, not in the selection */
} DfoItem;

/* The comparators. Two properties of these are load-bearing. NaN IS PLACED, NOT
   COMPARED. NaN sorts LAST in BOTH directions, because it is a missing value and
   not a large one -- nobody asking for the largest rows wants NaN at the top. */
static int dfo_cmp_asc(const void *pa, const void *pb)
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

static int dfo_cmp_desc(const void *pa, const void *pb)
{
    const DfoItem *a = pa, *b = pb;
    int na = isnan(a->key), nb = isnan(b->key);

    if (na | nb) {
        if (na != nb)
            return na - nb;             /* still last, not first */
    } else if (a->key > b->key) {
        return -1;
    } else if (a->key < b->key) {
        return 1;
    }
    return a->idx < b->idx ? -1 : (a->idx > b->idx ? 1 : 0);
}

/* dfo_cmp_asc/desc as an inline predicate: no call per comparison. NaN last,
   ties by row index -- that tiebreak makes the order total, which is what lets
   the unstable algorithms below produce the stable answer. */
static inline int dfo_before(const DfoItem *a, const DfoItem *b, int desc)
{
    int na = a->key != a->key, nb = b->key != b->key;

    if (na | nb) {
        if (na != nb)
            return nb;                  /* the non-NaN one comes first */
    } else if (a->key != b->key) {
        return desc ? (a->key > b->key) : (a->key < b->key);
    }
    return a->idx < b->idx;
}

static inline void dfo_swap(DfoItem *a, DfoItem *b)
{
    DfoItem t = *a; *a = *b; *b = t;
}

/* Median of three: a sorted or reversed column would otherwise be quadratic. */
static uint32_t dfo_pivot(DfoItem *it, uint32_t n, int desc)
{
    uint32_t m = n >> 1, last = n - 1;

    if (dfo_before(&it[m], &it[0], desc))    dfo_swap(&it[m], &it[0]);
    if (dfo_before(&it[last], &it[m], desc)) dfo_swap(&it[last], &it[m]);
    if (dfo_before(&it[m], &it[0], desc))    dfo_swap(&it[m], &it[0]);
    return m;
}

/* Hoare partition around it[p]; returns the split point. */
static uint32_t dfo_partition(DfoItem *it, uint32_t n, uint32_t p, int desc)
{
    DfoItem pivot = it[p];
    uint32_t i = 0, j = n - 1;

    for (;;) {
        while (dfo_before(&it[i], &pivot, desc)) i++;
        while (dfo_before(&pivot, &it[j], desc)) j--;
        if (i >= j)
            return j;
        dfo_swap(&it[i], &it[j]);
        i++;
        if (j == 0)
            return 0;
        j--;
    }
}

static void dfo_insertion(DfoItem *it, uint32_t n, int desc)
{
    uint32_t i, j;

    for (i = 1; i < n; i++) {
        DfoItem v = it[i];
        for (j = i; j > 0 && dfo_before(&v, &it[j - 1], desc); j--)
            it[j] = it[j - 1];
        it[j] = v;
    }
}

/* Introsort: inline compare, insertion sort under a cutoff, qsort past the
   depth limit rather than degrading. */
#define DFO_SMALL 24
static void dfo_sort(DfoItem *it, uint32_t n, int desc, int depth)
{
    while (n > DFO_SMALL) {
        uint32_t p;
        if (depth-- <= 0) {
            qsort(it, n, sizeof(*it), desc ? dfo_cmp_desc : dfo_cmp_asc);
            return;
        }
        p = dfo_partition(it, n, dfo_pivot(it, n, desc), desc);
        dfo_sort(it, p + 1, desc, depth);       /* left */
        it += p + 1;
        n  -= p + 1;                            /* tail-recurse right */
    }
    dfo_insertion(it, n, desc);
}

/* Quickselect: k-th order statistic at it[k] in O(n). Same total order as
   dfo_sort, so the element landing at k is the one a full sort would place. */
static void dfo_select(DfoItem *it, uint32_t n, uint32_t k, int desc)
{
    while (n > DFO_SMALL) {
        uint32_t p = dfo_partition(it, n, dfo_pivot(it, n, desc), desc);
        if (k <= p) {
            n = p + 1;
        } else {
            it += p + 1;
            n  -= p + 1;
            k  -= p + 1;
        }
    }
    dfo_insertion(it, n, desc);
}

/* Selected rows as (value, row) pairs, type switch hoisted out of the loop.
   `span` clamps to the bound length: a well-formed frame cannot have a short
   column, so that is defence against an uninitialised read, not a code path. */
#define DFO_GATHER(CTYPE)                                                     \
    do {                                                                      \
        const CTYPE *x = b->p;                                                \
        if (mask) {                                                           \
            for (i = 0; i < span; i++)                                        \
                if (mask[i]) { it[n].key = (double)x[i]; it[n].idx = i; n++; }\
        } else {                                                              \
            for (i = 0; i < span; i++) { it[i].key = (double)x[i];            \
                                         it[i].idx = i; }                     \
            n = span;                                                         \
        }                                                                     \
    } while (0)

static uint32_t dfo_gather(const DFBound *b, const uint8_t *mask,
                           uint32_t span, DfoItem *it)
{
    uint32_t i, n = 0;

    switch (b->type) {
    case DF_F64: DFO_GATHER(double);   break;
    case DF_F32: DFO_GATHER(float);    break;
    case DF_I32: DFO_GATHER(int32_t);  break;
    case DF_U32: DFO_GATHER(uint32_t); break;
    case DF_I16: DFO_GATHER(int16_t);  break;
    case DF_U16: DFO_GATHER(uint16_t); break;
    case DF_I8:  DFO_GATHER(int8_t);   break;
    default:     DFO_GATHER(uint8_t);  break;
    }
    return n;
}
#undef DFO_GATHER

/* Doubles order as unsigned integers once negatives are inverted whole and
   positives get their sign bit set. The map is a bijection on bit patterns, so
   the value comes back exactly; NaN has no place in it and is split out. */
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

/* MEDIAN and the quantiles want a value at a rank, never a row, so carrying the
   row doubles the bytes moved and the swap cost for nothing. The input must be
   NaN-free: a NaN loses every comparison and the scans would run off. */
static uint32_t dfv_gather(const DFBound *b, const uint8_t *mask,
                           uint32_t span, double *a)
{
    uint32_t i, n = 0;

    if (b->type == DF_F64 && !mask) {
        const double *x = b->p;
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

/* Same filter as dfv_gather, but emitting the order-preserving integer form so
   the histogram can index on a byte. -0.0 is left faithful: canonicalising it
   would change the value MEDIAN returns for an all -0.0 column. */
static uint32_t dfv_gather_keys(const DFBound *b, const uint8_t *mask,
                                uint32_t span, uint64_t *k,
                                uint64_t *pmin, uint64_t *pmax)
{
    uint64_t lo = ~(uint64_t)0, hi = 0;
    uint32_t i, n = 0;

    if (b->type == DF_F64 && !mask) {
        const double *x = b->p;
        for (i = 0; i < span; i++) {
            uint64_t u = dfo_skey(x[i]);
            k[n] = u;
            if (x[i] == x[i]) {
                n++;
                if (u < lo) lo = u;
                if (u > hi) hi = u;
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
                if (u < lo) lo = u;
                if (u > hi) hi = u;
            }
        }
    }
    *pmin = lo;
    *pmax = hi;
    return n;
}

static void dfv_insertion(double *a, uint32_t n)
{
    uint32_t i, j;

    for (i = 1; i < n; i++) {
        double v = a[i];
        for (j = i; j > 0 && v < a[j - 1]; j--)
            a[j] = a[j - 1];
        a[j] = v;
    }
}

/* MSD radix select: a branchless histogram narrows to one byte bucket, so the
   next level sees n/256. Keys must be NaN-free. */
static double dfv_radix_select(uint64_t *k, uint32_t n, uint32_t rank,
                               uint64_t kmin, uint64_t kmax)
{
    uint64_t diff = kmin ^ kmax;
    int level;

    if (!diff || n <= 1)
        return dfo_unskey(k[0]);
    /* Bytes the range shares cannot split anything, and integer-valued columns
       share several: start where the keys first differ. */
    level = (63 - __builtin_clzll(diff)) / 8;
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
            continue;               /* nothing narrowed: the copy would be a no-op */
        rank -= acc;
        for (i = 0; i < n; i++)
            if (((k[i] >> sh) & 0xff) == d)
                k[o++] = k[i];
        n = o;
    }
    return dfo_unskey(k[0]);
}

/* The order statistic one rank above `v`: `v` again where it repeats past that
   rank, otherwise the smallest value strictly greater. One pass over the
   column, against a second full select plus a copy of the key array. */
static double dfv_next_above(const DFBound *b, const uint8_t *mask,
                             uint32_t span, double v, uint32_t rank)
{
    uint32_t i, le = 0;
    double nx = INFINITY;

    if (b->type == DF_F64 && !mask) {
        const double *x = b->p;
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

/* Median-of-three quickselect, kept for the short inputs radix select cannot
   amortise its histogram over. The pivot is a value present in the array,
   which is what keeps the two scans in bounds without a sentinel. */
static void dfv_select(double *a, uint32_t n, uint32_t k)
{
    while (n > DFO_SMALL) {
        double p, t;
        int64_t i = -1, j = (int64_t)n;
        uint32_t m = n >> 1, last = n - 1;

        if (a[m] < a[0])    { t = a[m]; a[m] = a[0]; a[0] = t; }
        if (a[last] < a[m]) { t = a[last]; a[last] = a[m]; a[m] = t; }
        if (a[m] < a[0])    { t = a[m]; a[m] = a[0]; a[0] = t; }
        p = a[m];
        for (;;) {
            do { i++; } while (a[i] < p);
            do { j--; } while (a[j] > p);
            if (i >= j)
                break;
            t = a[i]; a[i] = a[j]; a[j] = t;
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

/* Stable 8-bit LSD radix over (key, row) as parallel arrays; stability is what
   supplies dfo_before's tie-by-row. Declines (-1) on allocation failure and on
   -0.0, which the sortable key orders by sign where dfo_before ties by row. */
static int dfo_radix(DfoItem *it, uint32_t n, int desc)
{
    uint64_t *ka = NULL, *kb = NULL, *ks, *kd;
    uint32_t *ia = NULL, *ib = NULL, *is, *id;
    DfoItem *nanv = NULL;
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
        hist[0][k & 0xff]++;         hist[1][(k >> 8) & 0xff]++;
        hist[2][(k >> 16) & 0xff]++; hist[3][(k >> 24) & 0xff]++;
        hist[4][(k >> 32) & 0xff]++; hist[5][(k >> 40) & 0xff]++;
        hist[6][(k >> 48) & 0xff]++; hist[7][(k >> 56) & 0xff]++;
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

    ks = ka; kd = kb; is = ia; id = ib;
    for (p = 0; p < 8; p++) {
        uint32_t *h = hist[p], sum = 0, shift = p * 8;
        if (nn && h[(ks[0] >> shift) & 0xff] == nn)
            continue;               /* one bucket holds everything: identity */
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
        { uint64_t *tk = ks; ks = kd; kd = tk; }
        { uint32_t *ti = is; is = id; id = ti; }
    }
    for (i = 0; i < nn; i++) {
        it[i].key = dfo_unskey(desc ? ~ks[i] : ks[i]);
        it[i].idx = is[i];
    }
    for (i = 0; i < nz; i++)
        it[nn + i] = nanv[i];
    free(nanv); free(ka); free(kb); free(ia); free(ib); free(hist);
    return 0;
decline:
    free(nanv); free(ka); free(kb); free(ia); free(ib); free(hist);
    return -1;
}

static int dfo_sorted(JSContext *ctx, const DFBound *b, const uint8_t *mask,
                      uint32_t nrows, int desc, DfoItem **out, uint32_t *pn)
{
    uint32_t span = b->n < nrows ? b->n : nrows, n;
    DfoItem *it = df_out_alloc(ctx, span, sizeof(DfoItem));

    if (!it)
        return -1;
    n = dfo_gather(b, mask, span, it);
    if (n > 1 && dfo_radix(it, n, desc) != 0) {
        int depth = 2;
        uint32_t t = n;
        while (t >>= 1) depth += 2;             /* 2*floor(log2 n) + 2 */
        dfo_sort(it, n, desc, depth);
    }
    *out = it;
    *pn = n;
    return 0;
}

/* Gather unordered, plus the non-NaN count. dfo_valued() cannot supply it: that
   reads a NaN suffix, which only exists once the array is sorted. */
static int dfo_gathered(JSContext *ctx, const DFBound *b, const uint8_t *mask,
                        uint32_t nrows, DfoItem **out, uint32_t *pn,
                        uint32_t *pvalued)
{
    uint32_t span = b->n < nrows ? b->n : nrows, n, i, m = 0;
    DfoItem *it = df_out_alloc(ctx, span, sizeof(DfoItem));

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

/* Length of the non-NaN prefix. NaN is a suffix in both directions, so this is
 * a backwards scan and not a filter. */
static uint32_t dfo_valued(const DfoItem *it, uint32_t n)
{
    while (n > 0 && isnan(it[n - 1].key))
        n--;
    return n;
}

/* Left to right, so a caller's valueOf hooks fire in argument order. `nscalar`
   is 0 or 1 and puts the mask at argv[1 + nscalar]. */
static int dfo_open(JSContext *ctx, JSValueConst this_val, int argc,
                    JSValueConst *argv, int nscalar, const char *op,
                    DataFrame **pdf, DFBound *b, double *pscalar,
                    const uint8_t **pmask)
{
    DataFrame *df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    int idx, ok;

    if (!df)
        return -1;
    idx = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (idx < 0)
        return -1;
    if (nscalar && JS_ToFloat64(ctx, pscalar, argc > 1 ? argv[1] : JS_UNDEFINED))
        return -1;
    *pmask = df_mask_arg(ctx, argc > 1 + nscalar ? argv[1 + nscalar]
                                                 : JS_UNDEFINED,
                         df->nrows, &ok);
    if (!ok)
        return -1;
    /* no JS may run from here to the end of the kernel. */
    if (df_bind_numeric(ctx, df, idx, b, op))
        return -1;
    *pdf = df;
    return 0;
}

enum { DFO_SORT, DFO_ARGSORT };

/* sort(col, mask?) -> Float64Array of the selected values, ascending
   argsort(col, mask?) -> Uint32Array of the selected FRAME ROW indices Both are
   PERMUTATIONS of the selection, so both keep the NaN rows -- at the end. */
static JSValue dyn_df_sort(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv, int magic)
{
    static const char *const dfo_sort_name[2] = { "SORT", "ARG_SORT" };
    DataFrame *df;
    DFBound b;
    const uint8_t *mask;
    DfoItem *it;
    uint32_t n, i;

    if (dfo_open(ctx, this_val, argc, argv, 0, dfo_sort_name[magic],
                 &df, &b, NULL, &mask))
        return JS_EXCEPTION;
    if (dfo_sorted(ctx, &b, mask, df->nrows, 0, &it, &n))
        return JS_EXCEPTION;

    if (magic == DFO_ARGSORT) {
        uint32_t *dst = df_out_alloc(ctx, n, sizeof(uint32_t));
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
        double *dst = df_out_alloc(ctx, n, sizeof(double));
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

/* Average is not a convention borrowed from elsewhere: it is the only tie rule
   whose column sum is invariant at n(n+1)/2 whatever the ties are, which is what
   lets a rank column compose with the sum and mean this module already ships. */
static JSValue dyn_df_rank(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    DataFrame *df;
    DFBound b;
    const uint8_t *mask;
    DfoItem *it;
    double *dst;
    uint32_t nrows, n, m, i, j;

    if (dfo_open(ctx, this_val, argc, argv, 0, "RANK", &df, &b, NULL, &mask))
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
        dst[i] = NAN;

    m = dfo_valued(it, n);
    i = 0;
    while (i < m) {
        double r;
        /* exact equality is right here: both keys are non-NaN, and -0.0 == 0.0
           is the grouping we want -- they are the same number. */
        for (j = i + 1; j < m && it[j].key == it[i].key; j++)
            ;
        r = ((double)(i + 1) + (double)j) * 0.5;   /* mean of ranks i+1 .. j */
        for (; i < j; i++)
            dst[it[i].idx] = r;
    }
    free(it);
    return df_to_typed_array(ctx, dst, (size_t)nrows * sizeof(double),
                             JS_TYPED_ARRAY_FLOAT64);
}

enum { DFO_Q_CONT, DFO_Q_PCONT, DFO_Q_DISC, DFO_Q_MEDIAN };

/* CONT interpolates linearly between the two neighbouring order statistics
   (numpy's and pandas' default, R type 7, SQL PERCENTILE_CONT), so its answer
   need not be a value the column contains. */
/* The rank arithmetic below must stay identical to the double path's, or the
   two sides of DFO_RADIX_MIN answer differently on the same column. */
static double dfq_from_keys(uint64_t *k, uint32_t m, double q, int magic,
                            uint64_t kmin, uint64_t kmax,
                            const DFBound *b, const uint8_t *mask, uint32_t span)
{
    double pos, frac, lo_v, hi_v;
    uint32_t lo;

    if (m == 0)
        return NAN;
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
    /* The select compacted k, so the neighbouring statistic comes from the
       column rather than from a second descent over a copy of the keys. */
    hi_v = dfv_next_above(b, mask, span, lo_v, lo);
    return lo_v + frac * (hi_v - lo_v);
}

static JSValue dyn_df_quantile(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv, int magic)
{
    static const char *const dfo_q_name[4] = {
        "QUANTILE", "PERCENTILE_CONT", "PERCENTILE_DISC", "MEDIAN"
    };
    DataFrame *df;
    DFBound b;
    const uint8_t *mask;
    double *a, q = 0.5, res;
    uint32_t span, m;

    if (dfo_open(ctx, this_val, argc, argv, magic != DFO_Q_MEDIAN,
                 dfo_q_name[magic], &df, &b, &q, &mask))
        return JS_EXCEPTION;
    if (magic != DFO_Q_MEDIAN && (isnan(q) || q < 0.0 || q > 1.0))
        return JS_ThrowRangeError(ctx, "%s(col, q): q must be in [0, 1], got %g",
                                  dfo_q_name[magic], q);
    /* Select, do not sort: a quantile reads one position, or two when it
       interpolates. Same total order, so the answer is identical. */
    span = b.n < df->nrows ? b.n : df->nrows;
    /* Radix select is a fixed ~3 passes; quickselect's neighbour select scans
       only the tail past the rank, which is short exactly when the rank is
       extreme. So the radix path takes middle ranks only. */
    if (span >= DFO_RADIX_MIN &&
        (magic == DFO_Q_DISC || (q >= 0.25 && q <= 0.75))) {
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

    if (m == 0) {                       /* as min and max do on empty */
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
        /* The frac == 0 arm is LOAD-BEARING, not a fast path: the interpolation
           below evaluates a[lo] + 0 * (a[lo+1] - a[lo]), and 0 * Infinity is
           NaN. Do not fold this branch away. */
        dfv_select(a, m, lo);
        if (frac == 0.0 || lo + 1 >= m) {
            res = a[lo];
        } else {
            /* The tail past lo is unordered, so a[lo+1] is not yet the next
               order statistic; selecting position 0 of it makes it so. */
            dfv_select(a + lo + 1, m - lo - 1, 0);
            res = a[lo] + frac * (a[lo + 1] - a[lo]);
        }
    }
    free(a);
    return JS_NewFloat64(ctx, res);
}

enum { DFO_TOP_LARGEST, DFO_TOP_SMALLEST };

/* (sort and argsort keep them, because those are permutations.) k must be a non-
   negative integer. A fractional, negative or NaN k is refused rather than
   truncated; a k larger than the selection is fine and clamps. */
static JSValue dyn_df_nlargest(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv, int magic)
{
    static const char *const dfo_top_name[2] = { "N_LARGEST", "N_SMALLEST" };
    DataFrame *df;
    DFBound b;
    const uint8_t *mask;
    DfoItem *it;
    double *dst, kd = 0;
    uint32_t n, m, k, i;

    if (dfo_open(ctx, this_val, argc, argv, 1, dfo_top_name[magic],
                 &df, &b, &kd, &mask))
        return JS_EXCEPTION;
    if (isnan(kd) || isinf(kd) || kd < 0.0 || kd != floor(kd))
        return JS_ThrowRangeError(ctx, "%s(col, k): k must be a non-negative "
                                  "integer, got %g", dfo_top_name[magic], kd);
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

/* ==== generated family g2 ====. */

/* ---- scans ----
   Nothing about the value contract is decided here; the one new thing is that
   the intermediate state is written out instead of discarded. Matches min/max. */

enum { DFS_CUMSUM, DFS_CUMPROD, DFS_CUMMAX, DFS_CUMMIN, DFS_SHIFT, DFS_DIFF,
       DFS_N };

/* Named for the error messages: a refusal has to say which method refused. */
static const char *const dfs_name[] = {
    "CUM_SUM", "CUM_PROD", "CUM_MAX", "CUM_MIN", "SHIFT", "DIFF",
};
_Static_assert(countof(dfs_name) == DFS_N,
               "every scan magic needs a name: a missing entry is a NULL "
               "passed to %s in a throw, not a compile error");

/* Widen a bound column into `dst` as doubles. Not df_map_widen_tab: this is one
   pass over the whole column either way, and the scans want the source in the
   destination buffer so the fold can run in place. */
/* Column -> contiguous doubles, type switch hoisted out of the loop. Only worth
   it when the column is read more than once; a single-pass fold pays more in
   traffic than it saves. Callers gate on dtype: f64 is already doubles. */
#define DF_WIDEN_ONE(CTYPE)                                                   \
    do {                                                                      \
        const CTYPE *x = b->p;                                                \
        for (i = 0; i < n; i++) dst[i] = (double)x[i];                        \
    } while (0)

static void df_widen(const DFBound *b, double *dst, uint32_t n)
{
    uint32_t i;

    if (n == 0)
        return;                 /* a zero-length view's base need not be valid */
    switch (b->type) {
    case DF_F64: memcpy(dst, b->p, (size_t)n * sizeof(double)); return;
    case DF_F32: DF_WIDEN_ONE(float);    break;
    case DF_I32: DF_WIDEN_ONE(int32_t);  break;
    case DF_U32: DF_WIDEN_ONE(uint32_t); break;
    case DF_I16: DF_WIDEN_ONE(int16_t);  break;
    case DF_U16: DF_WIDEN_ONE(uint16_t); break;
    case DF_I8:  DF_WIDEN_ONE(int8_t);   break;
    case DF_U8:  DF_WIDEN_ONE(uint8_t);  break;
    /* DF_STR carries int32 dictionary codes, NOT bytes: reaching this through a
       uint8_t default read three quarters of every code as the next one's. */
    default:     DF_WIDEN_ONE(int32_t);  break;
    }
}
#undef DF_WIDEN_ONE

static void dfs_widen(const DFBound *b, double *dst, uint32_t n)
{
    df_widen(b, dst, n);
}

/* The four folds, masked and unmasked, generated from the reduction's own STEP
   macros so the two cannot drift. STEP evaluates its second argument more than
   once, so the element is bound to a local first. */
#define DFS_SCAN(name, STEP, ID)                                              \
    static void dfs_##name(double *v, uint32_t n)                             \
    {                                                                         \
        double acc = (ID);                                                    \
        uint32_t i;                                                           \
        for (i = 0; i < n; i++) {                                             \
            double x = v[i];                                                  \
            STEP(acc, x);                                                     \
            v[i] = acc;                                                       \
        }                                                                     \
    }                                                                         \
    static void dfs_##name##_masked(double *v, uint32_t n,                    \
                                    const uint8_t *mask)                      \
    {                                                                         \
        double acc = (ID);                                                    \
        uint32_t i;                                                           \
        for (i = 0; i < n; i++) {                                             \
            double x = mask[i] ? v[i] : (ID);                                 \
            STEP(acc, x);                                                     \
            v[i] = acc;                                                       \
        }                                                                     \
    }

DFS_SCAN(cumsum,  DF_STEP_SUM, 0.0)
DFS_SCAN(cumprod, DF_STEP_MUL, 1.0)
DFS_SCAN(cummax,  DF_STEP_MAX, -INFINITY)
DFS_SCAN(cummin,  DF_STEP_MIN, INFINITY)
#undef DFS_SCAN

/* cumsum/cumprod/cummax/cummin(col[, mask]) -> Float64Array */
static JSValue dyn_df_cumsum(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv, int magic)
{
    DataFrame *df;
    DFBound b;
    const uint8_t *mask;
    double *dst;
    uint32_t n, span;
    int idx, ok;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    /* the column name is coerced (running JS) before the mask pointer is
       taken, so a detach from inside that coercion is seen by df_mask_arg. */
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
    /* no JS may run from here to the end of the kernel. */
    if (df_bind_numeric(ctx, df, idx, &b, dfs_name[magic])) {
        free(dst);
        return JS_EXCEPTION;
    }
    span = df_map_span(n, b.n, dst);
    dfs_widen(&b, dst, span);
    switch (magic) {
    case DFS_CUMPROD:
        if (mask) dfs_cumprod_masked(dst, span, mask);
        else      dfs_cumprod(dst, span);
        break;
    case DFS_CUMMAX:
        if (mask) dfs_cummax_masked(dst, span, mask);
        else      dfs_cummax(dst, span);
        break;
    case DFS_CUMMIN:
        if (mask) dfs_cummin_masked(dst, span, mask);
        else      dfs_cummin(dst, span);
        break;
    default:
        if (mask) dfs_cumsum_masked(dst, span, mask);
        else      dfs_cumsum(dst, span);
        break;
    }
    return df_to_typed_array(ctx, dst, (size_t)n * sizeof(double),
                             JS_TYPED_ARRAY_FLOAT64);
}

/* shift(col[, periods]) -> Float64Array: out[i] = col[i - periods]. Those slots
   are NaN and never 0: zero is a plausible wrong answer for diff -- it reads as
   "no change" -- and df_map_span already uses NaN for a row it cannot compute. */
static JSValue dyn_df_shift(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv, int magic)
{
    DataFrame *df;
    DFBound b;
    const double *src;
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
    if (argc > 1 && !JS_IsUndefined(argv[1]) &&
        JS_ToFloat64(ctx, &periods, argv[1]))
        return JS_EXCEPTION;
    /* Checked ONCE per call. A fractional period has no meaning and truncating
       it silently would answer a question the caller did not ask; a NaN one
       would compare false against every bound and fall through to a copy. */
    if (isnan(periods) || periods != floor(periods))
        return JS_ThrowTypeError(ctx, "%s(col, periods): periods must be an "
                                 "integer", dfs_name[magic]);

    n = df->nrows;
    dst = df_out_alloc(ctx, n, sizeof(double));
    if (!dst)
        return JS_EXCEPTION;
    /* no JS may run from here to the end of the kernel. */
    if (df_bind_numeric(ctx, df, idx, &b, dfs_name[magic])) {
        free(dst);
        return JS_EXCEPTION;
    }
    span = df_map_span(n, b.n, dst);
    /* Every source index out of range, so the whole answer is NaN -- for diff
       too, since x - NaN is NaN. Taken before the cast below, which is what
       makes an absurd period safe rather than undefined. */
    if (!(fabs(periods) < (double)span)) {
        for (i = 0; i < span; i++)
            dst[i] = NAN;
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
        /* |periods| < span <= UINT32_MAX, so this is exact. */
        int64_t k = (int64_t)periods, end = (int64_t)span, j;

        /* Three regions, so no arm carries the bounds test: the out-of-range
           head or tail is NaN, and the middle is a copy or subtract the
           compiler can widen into a memcpy. */
        int64_t lo = k > 0 ? k : 0;             /* first i with j in range */
        int64_t hi = k > 0 ? end : end + k;     /* one past the last */
        (void)j;
        for (i = 0; i < (uint32_t)lo; i++)
            dst[i] = NAN;
        for (i = (uint32_t)hi; i < span; i++)
            dst[i] = NAN;
        if (magic == DFS_DIFF) {
            const double *a = src + lo, *bsrc = src + (lo - k);
            uint32_t m = (uint32_t)(hi - lo);
            double *o = dst + lo;
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

/* ==== generated family g3 ====. */

/* ---- cardinality and set ops ----
   Every one of them is "how many DISTINCT values", so they all run over one
   open-addressed set and differ only in what they read out of it. /. */

#define DFC_EMPTY     0xFFFFFFFFu
#define DFC_MIN_SLOTS 64u

/* Open-addressed, linear probing, one entry per distinct (tag, value) pair.
   Callers that are not grouping pass 0 and pay one compare against a constant. */
typedef struct {
    uint32_t *slots;        /* slot -> entry index, or DFC_EMPTY */
    uint64_t *keys;         /* entry -> canonical value bits */
    uint32_t *tags;         /* entry -> group index (0 when ungrouped) */
    uint32_t *counts;       /* entry -> occurrences */
    uint32_t nslots, mask, nent, cap_ent;
} DfcSet;

/* The set doubles its slot array up to twice the entry cap, so the cap has to
 * leave room for that in a uint32 slot index. Violating it would silently wrap
 * and corrupt the probe rather than fail. */
_Static_assert(DF_MAX_GROUPS > 0 && DF_MAX_GROUPS <= (1 << 29),
               "DF_MAX_GROUPS must leave room for 2x slots in a uint32");

/* SameValueZero as 64 bits. memcpy into the integer rather than a pointer cast
   -- the cast is undefined behaviour and traps on strict-alignment targets. */
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

/* murmur3's fmix64, and the mix is LOAD-BEARING rather than an optimisation. */
static uint32_t dfc_hash(uint64_t k, uint32_t tag)
{
    k ^= (uint64_t)tag * 0x9e3779b97f4a7c15ULL;
    k ^= k >> 33;
    k *= 0xff51afd7ed558ccdULL;
    k ^= k >> 33;
    k *= 0xc4ceb9fe1a85ec53ULL;
    k ^= k >> 33;
    return (uint32_t)k;
}

static void dfc_set_free(DfcSet *s)
{
    free(s->slots);
    free(s->keys);
    free(s->tags);
    free(s->counts);
    memset(s, 0, sizeof(*s));
}

/* Double the table and rehash. */
static int dfc_set_grow(JSContext *ctx, DfcSet *s)
{
    uint32_t nslots = s->nslots ? s->nslots << 1 : DFC_MIN_SLOTS;
    uint32_t cap = nslots >> 1;             /* load factor 1/2 */
    uint32_t *slots, *tags, *counts;
    uint64_t *keys;
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

/* Insert or find. *pent is the entry index, *pfresh is 1 only on the first
 * occurrence of the pair -- which is what DROP_DUPLICATES and GROUP_UNIQ_ARRAY
 * are asking. Either may be NULL. Returns 0, or -1 having thrown. */
static int dfc_set_put(JSContext *ctx, DfcSet *s, uint64_t k, uint32_t tag,
                       uint32_t *pent, int *pfresh)
{
    uint32_t p;

    if (s->nent >= s->cap_ent && dfc_set_grow(ctx, s))
        return -1;
    p = dfc_hash(k, tag) & s->mask;
    while (s->slots[p] != DFC_EMPTY) {
        uint32_t e = s->slots[p];
        if (s->keys[e] == k && s->tags[e] == tag) {
            s->counts[e]++;
            if (pent) *pent = e;
            if (pfresh) *pfresh = 0;
            return 0;
        }
        p = (p + 1) & s->mask;
    }
    s->slots[p] = s->nent;
    s->keys[s->nent] = k;
    s->tags[s->nent] = tag;
    s->counts[s->nent] = 1;
    if (pent) *pent = s->nent;
    if (pfresh) *pfresh = 1;
    s->nent++;
    return 0;
}

/* Bind column `idx` and fold every unmasked row into `set`. Binds, so the caller
   must have finished every coercion first and no JS may run until this returns.
   Returns 0, or -1 having thrown. */
static int dfc_scan(JSContext *ctx, const DataFrame *df, int idx,
                    const uint8_t *mask, DfcSet *set)
{
    DFBound b;
    uint32_t i, n;

    if (dyn_df_bind(ctx, df, idx, &b))
        return -1;
    n = b.n < df->nrows ? b.n : df->nrows;
    /* borrow for f64, widen narrower elements: one type switch per call */
    double *wv = NULL, *own = NULL;
    if (n) {
        if (b.type == DF_F64) wv = (double *)b.p;
        else { wv = own = malloc((size_t)n * sizeof(double));
               if (wv) df_widen(&b, wv, n); }
    }
    for (i = 0; i < n; i++) {
        if (mask && !mask[i])
            continue;
        if (dfc_set_put(ctx, set, dfc_key(wv ? wv[i] : df_get(b.p, b.type, i)), 0,
                        NULL, NULL))
            { free(own); return -1; }
    }
    { free(own); return 0; }
}

/* One key as the value the caller stored: the dictionary string for a string
 * column, the number otherwise. */
static JSValue dfc_key_js(JSContext *ctx, const DFColumn *c, uint64_t bits)
{
    double v = dfc_key_value(bits);

    if (c->type == DF_STR) {
        uint32_t code = (uint32_t)v;
        if (code >= c->dict_len)    /* codes index the dictionary by
                                       construction; a miss means the column
                                       was mutated under us */
            return JS_ThrowRangeError(ctx, "dictionary code %u out of range",
                                      code);
        return JS_NewString(ctx, c->dict[code]);
    }
    return JS_NewFloat64(ctx, v);
}

/* Keys as a JS Array in the order given by `ord` (entry order when NULL). An
 * Array and not a typed array because a string column's keys are strings --
 * the same shape GROUP_BY_SUM's `keys` already has. */
static JSValue dfc_keys_array(JSContext *ctx, const DFColumn *c,
                              const DfcSet *s, const uint32_t *ord, uint32_t n)
{
    JSValue a = JS_NewArray(ctx);
    uint32_t i;

    if (JS_IsException(a))
        return a;
    for (i = 0; i < n; i++) {
        JSValue k = dfc_key_js(ctx, c, s->keys[ord ? ord[i] : i]);
        if (JS_IsException(k) ||
            JS_DefinePropertyValueUint32(ctx, a, i, k, JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, a);
            return JS_EXCEPTION;
        }
    }
    return a;
}

/* { keys, values } -- the module's pair shape. Consumes both arguments on
 * every path, including when either is already an exception. */
static JSValue dfc_pair(JSContext *ctx, JSValue keys, JSValue values)
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
    /* JS_DefinePropertyValueStr consumes the value even when it fails, so a
       failed define must not be followed by a free of the same value. */
    if (JS_DefinePropertyValueStr(ctx, res, "keys", keys, JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, values);
        JS_FreeValue(ctx, res);
        return JS_EXCEPTION;
    }
    if (JS_DefinePropertyValueStr(ctx, res, "values", values,
                                  JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, res);
        return JS_EXCEPTION;
    }
    return res;
}

/* unique(col[, mask]) -> Float64Array, or an Array of strings for a string
 * column. First-seen order. */
static JSValue dyn_df_unique(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    DataFrame *df;
    DfcSet set;
    const uint8_t *mask;
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

    /* no JS may run from here until the scan is finished. */
    memset(&set, 0, sizeof(set));
    if (dfc_scan(ctx, df, idx, mask, &set)) {
        dfc_set_free(&set);
        return JS_EXCEPTION;
    }
    if (df->cols[idx].type == DF_STR) {
        out = dfc_keys_array(ctx, &df->cols[idx], &set, NULL, set.nent);
    } else {
        double *dst = df_out_alloc(ctx, set.nent, sizeof(double));
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

/* nunique(col[, mask]) -> Number. Distinct values PRESENT: for a string column
 * that is the number of codes the selected rows carry, which is dict_len only
 * when nothing is masked out. */
static JSValue dyn_df_nunique(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    DataFrame *df;
    DfcSet set;
    const uint8_t *mask;
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

enum { DFC_VC_ALL, DFC_VC_TOPK };

/* Ascending on a packed (~count, entry) word IS descending on count with first
   appearance breaking ties, which is the whole ordering contract in one
   comparison. */
static int dfc_cmp_packed(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static uint32_t *dfc_rank_order(JSContext *ctx, const DfcSet *s)
{
    uint64_t *packed;
    uint32_t *ord;
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

/* It is the frequency question (the Space-Saving family), not the magnitude one
   -- nlargest/nsmallest answer that and live elsewhere. */
static JSValue dyn_df_value_counts(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv, int magic)
{
    DataFrame *df;
    DfcSet set;
    const uint8_t *mask;
    uint32_t *ord = NULL;
    double *cnt = NULL;
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
        /* Refuse a missing k rather than defaulting it: coercing undefined
           gives 0, so a default would silently return an empty table and look
           like a column with nothing in it. */
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

/* mode(col[, mask]) -> the most frequent value, or undefined when nothing is
   selected -- undefined rather than NaN, because a mode is an order statistic
   and follows min/max, not mean. */
static JSValue dyn_df_mode(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    DataFrame *df;
    DfcSet set;
    const uint8_t *mask;
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

/* A mask composes with everything else here -- sum(c, m), count(c, m),
   GROUP_BY_SUM(k, v, m) -- and is nrows long so it lines up with the frame; a
   compacted column changes the row count and can never be handed back. */
static JSValue dyn_df_drop_duplicates(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    DataFrame *df;
    DFBound b;
    DfcSet set;
    const uint8_t *mask;
    uint8_t *dst;
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


enum { DFC_GROUP_ALL, DFC_GROUP_UNIQ };

/* Values gathered per group and flattened. Group g owns
   [off[g] - cnt[g], off[g]) in `flat` once dfc_group_gather has filled it. */
typedef struct {
    double *flat;
    uint32_t *cnt, *off;
    uint32_t nkeys, ngroups, total, n;
} DfcGrouped;

static void dfc_grouped_free(DfcGrouped *G)
{
    free(G->flat);
    free(G->cnt);
    free(G->off);
    memset(G, 0, sizeof(*G));
}

/* Two passes: count per group, prefix-sum into offsets, then scatter. `uniq`
   keeps only the FIRST occurrence of each (group, value) pair. Runs no JS, so
   every caller must have coerced its arguments before calling. */
static int dfc_group_gather(JSContext *ctx, DataFrame *df, int ki, int vi,
                            const uint8_t *mask, int uniq, DfcGrouped *G)
{
    DFBound kb, vb;
    DfcSet set;
    uint8_t *keep = NULL;
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

    /* pass 1: how many values each group receives. For the uniq variant the
       per-row answer is kept so pass 2 needs no second probe. */
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

    /* pass 2: scatter. off[g] advances as group g fills, so afterwards it is
       one past that group's last element. */
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

/* One Float64Array per group, in key order. JS-visible allocation, so it runs
   only after every gather pass is done. */
static JSValue dfc_values_array(JSContext *ctx, const DfcGrouped *G)
{
    JSValue values = JS_NewArray(ctx);
    uint32_t g;

    if (JS_IsException(values))
        return values;
    for (g = 0; g < G->nkeys; g++) {
        double *slice = df_out_alloc(ctx, G->cnt[g], sizeof(double));
        JSValue ta;
        if (!slice) {
            JS_FreeValue(ctx, values);
            return JS_EXCEPTION;
        }
        memcpy(slice, G->flat + (G->off[g] - G->cnt[g]),
               (size_t)G->cnt[g] * sizeof(double));
        ta = df_to_typed_array(ctx, slice, (size_t)G->cnt[g] * sizeof(double),
                               JS_TYPED_ARRAY_FLOAT64);
        if (JS_IsException(ta) ||
            JS_DefinePropertyValueUint32(ctx, values, g, ta,
                                         JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, values);
            return JS_EXCEPTION;
        }
    }
    return values;
}

/* Keys for the DENSE-code grouping (dfc_group_count assigns 0..nkeys-1); the
   hash-set grouping has its own dfc_keys_array above. A string key column
   yields its dictionary strings, a numeric one its codes. */
static JSValue dfc_dense_keys(JSContext *ctx, const DataFrame *df, int ki,
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
        if (JS_IsException(k) ||
            JS_DefinePropertyValueUint32(ctx, keys, g, k, JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, keys);
            return JS_EXCEPTION;
        }
    }
    return keys;
}

/* An empty group is a zero-length Float64Array, never a hole and never
   undefined, so values[i].length is always safe. Summing a group's array
   reproduces GROUP_BY_SUM's value for that group. */
static JSValue dyn_df_group_array(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv, int magic)
{
    DataFrame *df;
    DfcGrouped G;
    const uint8_t *mask;
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

    /* no JS may run from here to the end of the gather. */
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

enum { DFC_MOVING_SUM, DFC_MOVING_AVG };

/* Above this window size the O(n*w) re-sum loses to an O(n) block-decomposed
   sum, whose association (and therefore last-ULP values) differs: small windows
   keep the exact path so their pinned values hold. Shared by dfc_moving_blocks
   and dfg_roll_sum -- one threshold, so the two cannot disagree. */
#define DFG_ROLL_SLIDE_MIN 256u

/* Block decomposition, as dfg_roll_sum: each full window is suffix(previous
   block) + prefix(current), so every element is added exactly twice and none
   is ever subtracted. O(m) where the re-sum below is O(m*w) -- measured 756 ms
   for one group of 40k at w=m-1, growing 4x per doubling. -1 means it declined
   and the caller re-sums. Associates differently, hence the gate. */
static int dfc_moving_blocks(double *v, uint32_t m, uint32_t w, int want_avg)
{
    double *suf = malloc((size_t)w * sizeof(double));
    double *out = malloc((size_t)m * sizeof(double));
    double run = 0.0;
    uint32_t s, i;

    if (!suf || !out) {
        free(suf);
        free(out);
        return -1;
    }
    /* windows before the first full one are PARTIAL -- [0, i], not absent */
    for (i = 0; i + 1 < w; i++) {
        run += v[i];
        out[i] = want_avg ? run / (double)(i + 1) : run;
    }
    for (s = 0; s < m; s += w) {
        uint32_t end = (s + w < m) ? s + w : m;
        uint32_t k, lo, hi;
        double r = 0.0, pre = 0.0;
        for (k = end; k-- > s; ) {
            r += v[k];
            suf[k - s] = r;
        }
        lo = s + w - 1;
        hi = s + 2 * w - 2;
        if (hi >= m)
            hi = m - 1;
        for (k = lo; k <= hi; k++) {
            uint32_t st = k + 1 - w;            /* window is [st, k], w wide */
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

/* Rewrite one group's slice in place with its moving aggregate. `w` is the
   window; 0 means expanding. Every window ends at i and reads only indices
   <= i, which is what lets both forms run in place. */
static void dfc_moving_slice(double *v, uint32_t m, uint32_t w, int want_avg)
{
    uint32_t i;

    if (w == 0 || w >= m) {
        /* Expanding: purely additive, so a running accumulator IS the
           definition here -- nothing is ever subtracted -- and it is O(m)
           rather than the O(m*w) the fixed window below has to pay. */
        double a = 0.0;
        for (i = 0; i < m; i++) {
            a += v[i];
            v[i] = want_avg ? a / (double)(i + 1) : a;
        }
        return;
    }
    if (w == 1)
        return;                 /* each window is one element, sum and mean */
    if (w >= DFG_ROLL_SLIDE_MIN && dfc_moving_blocks(v, m, w, want_avg) == 0)
        return;
    /* Fixed window, re-summed. Subtracting the element that leaves is the O(m)
       form this module already rejected: a large value leaving cancels the
       accumulator to a FINITE wrong answer, so no check catches it.
       Backwards, so v[i] is still ORIGINAL when the window ending at i reads it. */
    for (i = m; i-- > 0; ) {
        uint32_t lo = (i + 1 >= w) ? i + 1 - w : 0, j, c = i - lo + 1;
        double a = 0.0;
        for (j = lo; j <= i; j++)
            a += v[j];
        v[i] = want_avg ? a / (double)c : a;
    }
}

/* GROUP_ARRAY_MOVING_SUM / _AVG(key, val[, w][, mask]) -> { keys, values }.
   AVG divides by what CONTRIBUTED, as ROLLING_MEAN does and unlike ClickHouse,
   which divides by the window and so ramps up from a smaller first value. */
static JSValue dyn_df_group_array_moving(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv, int magic)
{
    DataFrame *df;
    DfcGrouped G;
    const uint8_t *mask;
    const char *op = magic == DFC_MOVING_AVG ? "GROUP_ARRAY_MOVING_AVG"
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
    /* an omitted window is EXPANDING, which is what ClickHouse's one-argument
       groupArrayMovingSum computes; a given one must still be a real window. */
    if (argc > 2 && !JS_IsUndefined(argv[2])) {
        if (JS_ToFloat64(ctx, &wd, argv[2]))
            return JS_EXCEPTION;
        if (!(wd >= 1) || wd != floor(wd) || wd > (double)UINT32_MAX)
            return JS_ThrowRangeError(ctx, "%s: window must be a positive "
                                      "integer, got %g", op, wd);
    }
    w = (uint32_t)wd;
    mask = df_mask_arg(ctx, argc > 3 ? argv[3] : JS_UNDEFINED, df->nrows, &ok);
    if (!ok)
        return JS_EXCEPTION;

    /* no JS may run from here to the end of the rewrite. */
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

/* ==== generated family g4 ====. */

/* ---- positional accessors (G.4) ----
   argmin must agree with min and argmax with max: where argmin returns an index
   i, col[i] is exactly what min returned, for the same column and the same mask. */

enum { DFP_HEAD, DFP_TAIL };
enum { DFP_FIRST, DFP_LAST };
enum { DFP_ARGMIN, DFP_ARGMAX };

/* Rows head/tail return when the caller omits n: the peek-sized default, so
   `df.head(col)` is a glance rather than a copy of the column. An n that IS
   given is clamped to the frame and never silently truncated below it. */
#define DFP_DEFAULT_N 5u

/* Bind a column and refuse a dictionary-encoded one. */
static int dfp_bind_num(JSContext *ctx, const DataFrame *df, int idx,
                        DFBound *b, const char *op)
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

/* Rows the frame and the bound column agree on. */
static uint32_t dfp_span(uint32_t nrows, uint32_t avail)
{
    return avail < nrows ? avail : nrows;
}

/* How many of the first `n` rows the mask selects. A NULL mask selects all of
   them, which is what df_mask_arg's NULL return already means everywhere else
   in this file. */
static uint32_t dfp_selected(const uint8_t *mask, uint32_t n)
{
    uint32_t i, k = 0;
    if (!mask)
        return n;
    for (i = 0; i < n; i++)
        k += (mask[i] != 0);
    return k;
}

/* Row index of the first selected row, or `n` when none is. */
static uint32_t dfp_first_selected(const uint8_t *mask, uint32_t n)
{
    uint32_t i;
    if (!mask)
        return n ? 0 : n;
    for (i = 0; i < n; i++)
        if (mask[i])
            return i;
    return n;
}

/* Row index of the last selected row, or `n` when none is. */
static uint32_t dfp_last_selected(const uint8_t *mask, uint32_t n)
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

/* head(col[, n[, mask]]) / tail(col[, n[, mask]]) -> Float64Array. n is clamped
   to the frame rather than refused, because "give me twenty rows" from a five-
   row frame has an obvious right answer. */
static JSValue dyn_df_head(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv, int magic)
{
    DataFrame *df;
    DFBound b;
    const uint8_t *mask;
    const char *op = (magic == DFP_TAIL) ? "TAIL" : "HEAD";
    double want_d = (double)DFP_DEFAULT_N;
    double *dst;
    JSValueConst nv;
    uint32_t nrows, n, want, avail, skip, take, w, i, seen;
    int idx, ok;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    /* coerce EVERY argument before binding any column pointer: ToCString and
       ToFloat64 both run user JS that can detach the buffer we are about to
       alias. n is a number, so the hook it reaches is valueOf, not toString. */
    idx = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (idx < 0)
        return JS_EXCEPTION;
    nv = argc > 1 ? argv[1] : JS_UNDEFINED;
    if (!JS_IsUndefined(nv) && !JS_IsNull(nv) && JS_ToFloat64(ctx, &want_d, nv))
        return JS_EXCEPTION;
    if (!(want_d >= 0))                 /* also catches NaN */
        return JS_ThrowRangeError(ctx, "%s(col, n): n must be a non-negative "
                                  "number, got %g", op, want_d);
    nrows = df->nrows;
    /* >= as a double, so +Infinity clamps here instead of converting. */
    want = (want_d >= (double)nrows) ? nrows : (uint32_t)want_d;
    mask = df_mask_arg(ctx, argc > 2 ? argv[2] : JS_UNDEFINED, nrows, &ok);
    if (!ok)
        return JS_EXCEPTION;

    /* malloc, never a JS-visible allocation: the latter can collect, a
       finalizer can run user code, and user code can detach the column. */
    dst = malloc(want ? (size_t)want * sizeof(double) : 1);
    if (!dst)
        return JS_ThrowOutOfMemory(ctx);
    /* no JS may run from here to df_to_typed_array. */
    if (dfp_bind_num(ctx, df, idx, &b, op)) {
        free(dst);
        return JS_EXCEPTION;
    }
    n = dfp_span(nrows, b.n);
    avail = dfp_selected(mask, n);
    take = (want < avail) ? want : avail;
    skip = (magic == DFP_TAIL) ? avail - take : 0;

    /* Two loops, not one with a test in it: the unmasked arm is then provably
       a contiguous run rather than merely measured as one. */
    w = 0;
    if (!mask) {
        for (i = skip; w < take; i++)           /* skip + take <= n */
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

/* first(col[, mask]) / last(col[, mask]) -> Number, or undefined. The value of
   the first (last) SELECTED row. It cannot be confused with a NaN value, which
   is why it is undefined and not NaN. */
static JSValue dyn_df_first(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv, int magic)
{
    DataFrame *df;
    DFBound b;
    const uint8_t *mask;
    const char *op = (magic == DFP_LAST) ? "LAST" : "FIRST";
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

    /* no JS may run from here to the return. */
    if (dfp_bind_num(ctx, df, idx, &b, op))
        return JS_EXCEPTION;
    n = dfp_span(df->nrows, b.n);
    i = (magic == DFP_LAST) ? dfp_last_selected(mask, n)
                            : dfp_first_selected(mask, n);
    if (i >= n)
        return JS_UNDEFINED;
    return JS_NewFloat64(ctx, df_get(b.p, b.type, i));
}

/* argmin(col[, mask]) / argmax(col[, mask]) -> Number, or undefined. Ties go to
   the FIRST occurrence, which a strict compare gives for free once the scan
   seeds on the first candidate rather than on a sentinel. */
static JSValue dyn_df_argmin(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv, int magic)
{
    DataFrame *df;
    DFBound b;
    const uint8_t *mask;
    const char *op = (magic == DFP_ARGMAX) ? "ARG_MAX" : "ARG_MIN";
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

    /* no JS may run from here to the return. */
    if (dfp_bind_num(ctx, df, idx, &b, op))
        return JS_EXCEPTION;
    n = dfp_span(df->nrows, b.n);
    /* borrow for f64, widen narrower elements: one type switch per call */
    if (n) {
        if (b.type == DF_F64) {
            wv = (double *)b.p;
        } else {
            wv = own = malloc((size_t)n * sizeof(double));
            if (wv)
                df_widen(&b, wv, n);
        }
    }

/* THE SEED, AND WHY IT IS NOT A SENTINEL. A scan seeded with the identity and
   taking `v < best` never fires there -- +Infinity is not less than +Infinity --
   so it would answer `undefined` for a column whose minimum plainly exists. */
#define DFP_ARG_SCAN(BETTER)                                                  \
    do {                                                                      \
        for (i = 0; i < n; i++) {                                             \
            if (mask && !mask[i])                                             \
                continue;                                                     \
            v = wv ? wv[i] : df_get(b.p, b.type, i);                          \
            if (isnan(v))                                                     \
                continue;                     /* as min does; see the head */ \
            if (!have) { have = 1; best = v; at = i; }   /* SEED, not INIT */ \
            else if (BETTER) { best = v; at = i; }                            \
        }                                                                     \
    } while (0)

/* Four independent lanes: one lane is a four-cycle loop-carried chain on
   `best`. Each keeps its own first occurrence and the merge breaks equal
   values by the lower row, so ties still go to the first. */
#define DFP_LANES(BETTER)                                                     \
    do {                                                                      \
        double m0 = best, m1 = best, m2 = best, m3 = best;                    \
        uint32_t p0 = at, p1 = at, p2 = at, p3 = at;                          \
        for (; i + 3 < n; i += 4) {                                           \
            if (wv[i]     BETTER m0) { m0 = wv[i];     p0 = i;     }          \
            if (wv[i + 1] BETTER m1) { m1 = wv[i + 1]; p1 = i + 1; }          \
            if (wv[i + 2] BETTER m2) { m2 = wv[i + 2]; p2 = i + 2; }          \
            if (wv[i + 3] BETTER m3) { m3 = wv[i + 3]; p3 = i + 3; }          \
        }                                                                     \
        for (; i < n; i++)                                                    \
            if (wv[i] BETTER m0) { m0 = wv[i]; p0 = i; }                      \
        if (m1 BETTER m0 || (m1 == m0 && p1 < p0)) { m0 = m1; p0 = p1; }      \
        if (m3 BETTER m2 || (m3 == m2 && p3 < p2)) { m2 = m3; p2 = p3; }      \
        if (m2 BETTER m0 || (m2 == m0 && p2 < p0)) { m0 = m2; p0 = p2; }      \
        best = m0; at = p0;                                                   \
    } while (0)

    /* Seed on the first value present; a NaN then loses every comparison, so
       the scans below need no NaN test of their own. */
    if (wv && !mask) {
        for (i = 0; i < n; i++)
            if (!isnan(wv[i])) { have = 1; best = wv[i]; at = i++; break; }
        if (have) {
            if (magic == DFP_ARGMAX) DFP_LANES(>);
            else                     DFP_LANES(<);
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

/* ==== generated family g5 ====. */

/* Everything below is one contiguous block. Registration rows: register.txt.
   ==========================================================================. */

/* ---- B.x moments, correlation, regression ----
   ------------------------------------------ B.x moments, correlation,
   regression FOURTEEN methods, ONE loop pair. /. */

enum { DFM_VARPOP, DFM_STDPOP, DFM_SKEW, DFM_KURT };
enum { DFM_COVPOP, DFM_COVSAMP, DFM_CORR, DFM_SLOPE, DFM_INTERCEPT, DFM_R2,
       DFM_AVGX, DFM_AVGY };

/* The running quantities, all CENTRED except the raw sums and the extremes. NaN
   rather than a stale zero so that reading them from the wrong path produces a
   NaN instead of a plausible skewness. */
typedef struct {
    double n;                 /* SELECTED rows; a double because every use divides */
    double sx, sy;            /* raw sums (pass 1) */
    double mx, my;            /* means */
    double m2x, m3x, m4x;     /* S((x-mx)^k), k = 2,3,4 -- one-column path only */
    double m2y;               /* S((y-my)^2) */
    double cxy;               /* S((x-mx)(y-my)) */
    double lo, hi;            /* min/max of x, NaN-IGNORING; +/-inf when n == 0 */
} DFMoments;

/* Fill `o` from one column (py == NULL) or two paired columns. */
#define DFM_UNROLL_MIN 64u

/* Eight sum lanes and four extremum lanes; min/max are exact under any lane
   split, the sums are not, which is what DFM_UNROLL_MIN gates. */
static void dfm_unrolled(const double *x, const double *y, uint32_t n,
                         DFMoments *o)
{
    double s0=0,s1=0,s2=0,s3=0,s4=0,s5=0,s6=0,s7=0;
    double t0=0,t1=0,t2=0,t3=0,t4=0,t5=0,t6=0,t7=0;
    double lo0=INFINITY,lo1=INFINITY,lo2=INFINITY,lo3=INFINITY;
    double hi0=-INFINITY,hi1=-INFINITY,hi2=-INFINITY,hi3=-INFINITY;
    double a0=0,a1=0,a2=0,a3=0, b0=0,b1=0,b2=0,b3=0;
    double c0=0,c1=0,c2=0,c3=0, mx, my = 0.0, sx, sy = 0.0;
    uint32_t i, q = n & ~7u, r = n & ~3u;

    for (i = 0; i < q; i += 8) {
        s0+=x[i]; s1+=x[i+1]; s2+=x[i+2]; s3+=x[i+3];
        s4+=x[i+4]; s5+=x[i+5]; s6+=x[i+6]; s7+=x[i+7];
    }
    for (; i < n; i++)
        s0 += x[i];
    sx = ((s0+s1)+(s2+s3))+((s4+s5)+(s6+s7));

    for (i = 0; i < r; i += 4) {
        if (x[i]   < lo0) lo0 = x[i];    if (x[i]   > hi0) hi0 = x[i];
        if (x[i+1] < lo1) lo1 = x[i+1];  if (x[i+1] > hi1) hi1 = x[i+1];
        if (x[i+2] < lo2) lo2 = x[i+2];  if (x[i+2] > hi2) hi2 = x[i+2];
        if (x[i+3] < lo3) lo3 = x[i+3];  if (x[i+3] > hi3) hi3 = x[i+3];
    }
    for (; i < n; i++) {
        if (x[i] < lo0) lo0 = x[i];
        if (x[i] > hi0) hi0 = x[i];
    }
    if (lo1 < lo0) lo0 = lo1;  if (lo3 < lo2) lo2 = lo3;  if (lo2 < lo0) lo0 = lo2;
    if (hi1 > hi0) hi0 = hi1;  if (hi3 > hi2) hi2 = hi3;  if (hi2 > hi0) hi0 = hi2;

    if (y) {
        for (i = 0; i < q; i += 8) {
            t0+=y[i]; t1+=y[i+1]; t2+=y[i+2]; t3+=y[i+3];
            t4+=y[i+4]; t5+=y[i+5]; t6+=y[i+6]; t7+=y[i+7];
        }
        for (; i < n; i++)
            t0 += y[i];
        sy = ((t0+t1)+(t2+t3))+((t4+t5)+(t6+t7));
        my = sy / (double)n;
    }
    mx = sx / (double)n;

    if (y) {
        for (i = 0; i < r; i += 4) {
            double d0=x[i]-mx, d1=x[i+1]-mx, d2=x[i+2]-mx, d3=x[i+3]-mx;
            double e0=y[i]-my, e1=y[i+1]-my, e2=y[i+2]-my, e3=y[i+3]-my;
            a0+=d0*d0; a1+=d1*d1; a2+=d2*d2; a3+=d3*d3;
            b0+=e0*e0; b1+=e1*e1; b2+=e2*e2; b3+=e3*e3;
            c0+=d0*e0; c1+=d1*e1; c2+=d2*e2; c3+=d3*e3;
        }
        for (; i < n; i++) {
            double d = x[i]-mx, e = y[i]-my;
            a0+=d*d; b0+=e*e; c0+=d*e;
        }
        o->m2x = (a0+a1)+(a2+a3);
        o->m2y = (b0+b1)+(b2+b3);
        o->cxy = (c0+c1)+(c2+c3);
        o->m3x = NAN;
        o->m4x = NAN;
    } else {
        /* Do NOT name `d * d`: that defeats FMA contraction and moves the
           variance by a ULP, which describe() pins against variance(). */
        for (i = 0; i < r; i += 4) {
            double d0=x[i]-mx, d1=x[i+1]-mx, d2=x[i+2]-mx, d3=x[i+3]-mx;
            a0+=d0*d0;     a1+=d1*d1;     a2+=d2*d2;     a3+=d3*d3;
            b0+=d0*d0*d0;  b1+=d1*d1*d1;  b2+=d2*d2*d2;  b3+=d3*d3*d3;
            c0+=d0*d0*d0*d0; c1+=d1*d1*d1*d1;
            c2+=d2*d2*d2*d2; c3+=d3*d3*d3*d3;
        }
        for (; i < n; i++) {
            double d = x[i]-mx;
            a0+=d*d; b0+=d*d*d; c0+=d*d*d*d;
        }
        o->m2x = (a0+a1)+(a2+a3);
        o->m3x = (b0+b1)+(b2+b3);
        o->m4x = (c0+c1)+(c2+c3);
        o->m2y = 0.0;
        o->cxy = 0.0;
    }
    o->n = (double)n;
    o->sx = sx;  o->sy = sy;
    o->mx = mx;  o->my = my;
    o->lo = lo0; o->hi = hi0;
}

static void dfm_moments(const void *px, DFType tx,
                        const void *py, DFType ty,
                        const uint8_t *m, uint32_t n, DFMoments *o)
{
    double cnt = 0.0, sx = 0.0, sy = 0.0, lo = INFINITY, hi = -INFINITY;
    double m2x = 0.0, m3x = 0.0, m4x = 0.0, m2y = 0.0, cxy = 0.0;
    double mx = 0.0, my = 0.0, v, w, d, e;
    uint32_t i;
    /* Widened copies so both passes read flat doubles. Optional: on allocation
       failure the df_get path below gives the identical answer. */
    double *wx = NULL, *wy = NULL;
    double *own_x = NULL, *own_y = NULL;   /* only these are freed */
    DFBound wb;

    /* Borrow for f64, widen only narrower elements: copying an f64 column is a
       third pass of traffic for a switch that was already predicted (0.94x). */
    if (n) {
        if (tx == DF_F64) {
            wx = (double *)px;          /* borrowed, not owned */
        } else {
            wx = own_x = malloc((size_t)n * sizeof(double));
            if (wx) {
                wb.p = px; wb.type = tx; wb.n = n;
                df_widen(&wb, wx, n);
            }
        }
        if (py) {
            if (ty == DF_F64) {
                wy = (double *)py;
            } else {
                wy = own_y = malloc((size_t)n * sizeof(double));
                if (wy) {
                    wb.p = py; wb.type = ty; wb.n = n;
                    df_widen(&wb, wy, n);
                }
            }
            if (!wy) { free(own_x); wx = own_x = NULL; }
        }
    }

/* Same value either way; only the cost differs. */
#define DFM_X(i)  (wx ? wx[i] : df_get(px, tx, i))
#define DFM_Y(i)  (wy ? wy[i] : df_get(py, ty, i))

    /* Unmasked flat doubles: the serial FP chains were the whole cost, so both
       passes run with independent accumulators. Gated at DFM_UNROLL_MIN so short
       columns keep the left-to-right association their pinned values use. */
    if (!m && wx && n >= DFM_UNROLL_MIN && (!py || wy)) {
        dfm_unrolled(wx, wy, n, o);
        free(own_x);
        free(own_y);
        return;
    }

    /* pass 1: count, raw sums, NaN-ignoring extremes of x. */
    if (py) {
        for (i = 0; i < n; i++) {
            if (m && !m[i])
                continue;
            v = DFM_X(i);
            w = DFM_Y(i);
            cnt += 1.0;
            sx += v;
            sy += w;
            if (v < lo) lo = v;
            if (v > hi) hi = v;
        }
    } else {
        for (i = 0; i < n; i++) {
            if (m && !m[i])
                continue;
            v = DFM_X(i);
            cnt += 1.0;
            sx += v;
            if (v < lo) lo = v;
            if (v > hi) hi = v;
        }
    }

    /* pass 2: centred powers. Skipped entirely at n == 0, where every consumer
       gates on n or on m2 being positive and never reads them. */
    if (cnt > 0.0) {
        mx = sx / cnt;
        my = sy / cnt;
        if (py) {
            m3x = NAN;
            m4x = NAN;
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
            /* Do NOT hoist `d * d` into a named local to reuse: that defeats
               FMA contraction, costs a ULP, and describe().variance is pinned
               equal to variance(). */
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
        m3x = NAN;
        m4x = NAN;
    }
#undef DFM_X
#undef DFM_Y
    free(own_x);
    free(own_y);

    o->n = cnt;
    o->sx = sx;   o->sy = sy;
    o->mx = mx;   o->my = my;
    o->m2x = m2x; o->m3x = m3x; o->m4x = m4x;
    o->m2y = m2y; o->cxy = cxy;
    o->lo = lo;   o->hi = hi;
}

/* POPULATION variance: n >= 1, because the population variance of a single
   observation is 0 and not undefined. The SAMPLE variance above returns NaN
   below TWO rows. */
static double dfm_var_pop(const DFMoments *o)
{
    return o->n >= 1.0 ? o->m2x / o->n : NAN;
}

/* Every gate below is spelled `!(q > 0)` rather than `q == 0`. */

/* Population (moment) skewness g1 = m3 / m2^(3/2), NOT the sample-adjusted G1.
   scipy.stats.skew defaults to the same choice; pandas .skew() does not, and
   returns G1. */
static double dfm_skew(const DFMoments *o)
{
    double m2, m3;
    if (!(o->m2x > 0.0))
        return NAN;
    m2 = o->m2x / o->n;
    m3 = o->m3x / o->n;
    return m3 / (m2 * sqrt(m2));
}

/* EXCESS kurtosis: m4/m2^2 - 3, so a normal distribution reads 0 and the SIGN
   carries the meaning (heavy tails positive, light tails negative). The raw
   fourth standardised moment is `kurtosis + 3` for anyone who wants it. */
static double dfm_kurt(const DFMoments *o)
{
    double m2, m4;
    if (!(o->m2x > 0.0))
        return NAN;
    m2 = o->m2x / o->n;
    m4 = o->m4x / o->n;
    return m4 / (m2 * m2) - 3.0;
}

/* Pearson correlation. A constant column is ordinary data that any filter can
   produce. Returning 0 would be the plausible wrong answer: it asserts
   "uncorrelated" where the answer is undefined. */
static double dfm_corr(const DFMoments *o)
{
    double r;
    if (!(o->m2x > 0.0) || !(o->m2y > 0.0))
        return NAN;
    /* sqrt each side BEFORE multiplying: sqrt(a)*sqrt(b) cannot overflow where
       a*b can, and an overflow there would divide by infinity and return 0 --
       "uncorrelated" for data that is perfectly correlated. */
    r = o->cxy / (sqrt(o->m2x) * sqrt(o->m2y));
    /* Cauchy-Schwarz bounds |r| at 1; floating point does not know that, and a
       caller passing 1.0000000000000002 to Math.acos gets NaN. */
    if (r > 1.0)  r = 1.0;
    if (r < -1.0) r = -1.0;
    return r;
}

/* VARIANCE_POP / STDDEV_POP / skew / kurtosis (col[, mask]) -> Number. */
static JSValue dyn_df_moments1(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv, int magic)
{
    DataFrame *df;
    DFBound b;
    DFMoments mo;
    const uint8_t *mask;
    int idx, ok;
    double r;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    /* coerce EVERY argument before binding any column pointer: ToCString and
       the typed-array accessors can run user JS that detaches the buffer we
       are about to alias. */
    idx = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (idx < 0)
        return JS_EXCEPTION;
    mask = df_mask_arg(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, df->nrows, &ok);
    if (!ok)
        return JS_EXCEPTION;

    /* no JS may run from here to the end of the loops. */
    if (dyn_df_bind(ctx, df, idx, &b))
        return JS_EXCEPTION;
    if (b.type == DF_STR)
        return JS_ThrowTypeError(ctx, "cannot reduce a string column");

    dfm_moments(b.p, b.type, NULL, DF_F64, mask, b.n, &mo);
    switch (magic) {
    case DFM_STDPOP: r = sqrt(dfm_var_pop(&mo)); break;
    case DFM_SKEW:   r = dfm_skew(&mo);          break;
    case DFM_KURT:   r = dfm_kurt(&mo);          break;
    default:         r = dfm_var_pop(&mo);       break;
    }
    /* sqrt(NaN) is NaN, so the n == 0 answer carries through unchanged. */
    return JS_NewFloat64(ctx, r);
}

/* It is also the only reading under which REGR_AVG_X and REGR_AVG_Y need to
   exist as separate names at all. cov and corr are symmetric and do not care. */
static JSValue dyn_df_moments2(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv, int magic)
{
    static const char *const names[] = {
        "COV_POP", "COV_SAMP", "CORR", "REGR_SLOPE", "REGR_INTERCEPT", "REGR_R2",
        "REGR_AVG_X", "REGR_AVG_Y"
    };
    DataFrame *df;
    DFBound ba, bb;
    const DFBound *bx, *by;
    DFMoments mo;
    const uint8_t *mask;
    int ia, ib, ok, swap;
    uint32_t n;
    double r, rr;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    /* BOTH names are coerced before EITHER column is bound: ToCString runs user
       JS that can detach the buffer the first bind aliased. */
    ia = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (ia < 0)
        return JS_EXCEPTION;
    ib = df_col_arg(ctx, df, argc > 1 ? argv[1] : JS_UNDEFINED);
    if (ib < 0)
        return JS_EXCEPTION;
    mask = df_mask_arg(ctx, argc > 2 ? argv[2] : JS_UNDEFINED, df->nrows, &ok);
    if (!ok)
        return JS_EXCEPTION;

    /* no JS may run from here to the end of the loops. */
    if (dyn_df_bind(ctx, df, ia, &ba) || dyn_df_bind(ctx, df, ib, &bb))
        return JS_EXCEPTION;
    /* Refused in the ARGUMENT's terms, not the role's: the caller has to be told
       which of the two columns they passed is the string one. */
    if (ba.type == DF_STR || bb.type == DF_STR) {
        int bad = ba.type == DF_STR ? ia : ib;
        return JS_ThrowTypeError(ctx, "%s: column '%s' is %s; cannot reduce a "
                                 "string column", names[magic],
                                 df->cols[bad].name, df_type_name(DF_STR));
    }

    /* Bind the two columns to their ROLES exactly once, here, rather than
       leaving each formula below to remember which argument it wanted. */
    switch (magic) {
    case DFM_SLOPE: case DFM_INTERCEPT: case DFM_R2:
    case DFM_AVGX:  case DFM_AVGY:
        swap = 1;               /* SQL order: (Y, X) */
        break;
    default:
        swap = 0;               /* symmetric: (A, B) */
        break;
    }
    bx = swap ? &bb : &ba;
    by = swap ? &ba : &bb;

    /* The constructor already forces every column to nrows, so these are equal;
       taking the minimum anyway matches DOT_PRODUCT and keeps the loop bound
       correct if that ever stops being true. */
    n = bx->n < by->n ? bx->n : by->n;
    dfm_moments(bx->p, bx->type, by->p, by->type, mask, n, &mo);

    switch (magic) {
    case DFM_COVSAMP:
        r = mo.n >= 2.0 ? mo.cxy / (mo.n - 1.0) : NAN;
        break;
    case DFM_CORR:
        r = dfm_corr(&mo);
        break;
    case DFM_SLOPE:
        r = mo.m2x > 0.0 ? mo.cxy / mo.m2x : NAN;
        break;
    case DFM_INTERCEPT:
        r = mo.m2x > 0.0 ? mo.my - (mo.cxy / mo.m2x) * mo.mx : NAN;
        break;
    case DFM_R2:
        /* SQL's REGR_R2 special-cases a constant Y to 1; that is deliberately
           not followed, because it would break the identity and 0/0 is honestly
           NaN. */
        rr = dfm_corr(&mo);
        r = rr * rr;
        if (r > 1.0)
            r = 1.0;
        break;
    case DFM_AVGX:
        r = mo.n >= 1.0 ? mo.mx : NAN;
        break;
    case DFM_AVGY:
        r = mo.n >= 1.0 ? mo.my : NAN;
        break;
    default:                    /* DFM_COVPOP: /n, defined from one row up */
        r = mo.n >= 1.0 ? mo.cxy / mo.n : NAN;
        break;
    }
    return JS_NewFloat64(ctx, r);
}

/* MEAN_WEIGHTED(valueCol, weightCol[, mask]) -> Number. The one method here that
   is NOT a moment: it needs S(w) and S(w*x), which are not centred quantities,
   so it gets its own single pass. There is no cancellation to defend against. */
static JSValue dyn_df_mean_weighted(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    DataFrame *df;
    DFBound bv, bw;
    const uint8_t *mask;
    int iv, iw, ok;
    uint32_t i, n;
    double sw = 0.0, swx = 0.0, v, w;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    /* both names coerced before either column is bound. */
    iv = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (iv < 0)
        return JS_EXCEPTION;
    iw = df_col_arg(ctx, df, argc > 1 ? argv[1] : JS_UNDEFINED);
    if (iw < 0)
        return JS_EXCEPTION;
    mask = df_mask_arg(ctx, argc > 2 ? argv[2] : JS_UNDEFINED, df->nrows, &ok);
    if (!ok)
        return JS_EXCEPTION;

    /* no JS may run from here to the end of the loop. */
    if (dyn_df_bind(ctx, df, iv, &bv) || dyn_df_bind(ctx, df, iw, &bw))
        return JS_EXCEPTION;
    if (bv.type == DF_STR || bw.type == DF_STR) {
        int bad = bv.type == DF_STR ? iv : iw;
        return JS_ThrowTypeError(ctx, "MEAN_WEIGHTED: column '%s' is %s; cannot "
                                 "reduce a string column", df->cols[bad].name,
                                 df_type_name(DF_STR));
    }

    n = bv.n < bw.n ? bv.n : bw.n;
    for (i = 0; i < n; i++) {
        if (mask && !mask[i])
            continue;
        w = df_get(bw.p, bw.type, i);
        if (w == 0.0)           /* a zero weight is not in the input set */
            continue;
        v = df_get(bv.p, bv.type, i);
        sw += w;
        swx += w * v;
    }
    return JS_NewFloat64(ctx, sw == 0.0 ? NAN : swx / sw);
}

/* Define one property, consuming `v` exactly as JS_DefinePropertyValueStr does.
   Returns < 0 on failure. */
static int dfm_set(JSContext *ctx, JSValue obj, const char *key, JSValue v)
{
    if (JS_IsException(v))
        return -1;
    return JS_DefinePropertyValueStr(ctx, obj, key, v, JS_PROP_C_W_E);
}

/* Mirrored rather than quietly improved: if that is ever changed, describe must
   be changed with it and the test will say so; - sum is 0 on an empty selection
   and mean is NaN, matching sum()/mean(). */
static JSValue dyn_df_describe(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    DataFrame *df;
    DFBound b;
    DFMoments mo;
    const uint8_t *mask;
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

    /* no JS may run from here to the end of the loops. */
    if (dyn_df_bind(ctx, df, idx, &b))
        return JS_EXCEPTION;
    if (b.type == DF_STR)
        return JS_ThrowTypeError(ctx, "cannot reduce a string column");

    dfm_moments(b.p, b.type, NULL, DF_F64, mask, b.n, &mo);
    /* every JS value below is built AFTER the loops, so the object allocation
       cannot run a finalizer while a column pointer is live. */
    res = JS_NewObject(ctx);
    if (JS_IsException(res))
        return res;
    var = mo.n >= 2.0 ? mo.m2x / (mo.n - 1.0) : NAN;

    if (dfm_set(ctx, res, "count", JS_NewInt64(ctx, (int64_t)mo.n)) < 0 ||
        dfm_set(ctx, res, "sum", JS_NewFloat64(ctx, mo.sx)) < 0 ||
        dfm_set(ctx, res, "mean",
                JS_NewFloat64(ctx, mo.n >= 1.0 ? mo.mx : NAN)) < 0 ||
        dfm_set(ctx, res, "min",
                mo.n >= 1.0 ? JS_NewFloat64(ctx, mo.lo) : JS_UNDEFINED) < 0 ||
        dfm_set(ctx, res, "max",
                mo.n >= 1.0 ? JS_NewFloat64(ctx, mo.hi) : JS_UNDEFINED) < 0 ||
        dfm_set(ctx, res, "variance", JS_NewFloat64(ctx, var)) < 0 ||
        dfm_set(ctx, res, "stddev", JS_NewFloat64(ctx, sqrt(var))) < 0 ||
        dfm_set(ctx, res, "skew", JS_NewFloat64(ctx, dfm_skew(&mo))) < 0 ||
        dfm_set(ctx, res, "kurtosis", JS_NewFloat64(ctx, dfm_kurt(&mo))) < 0) {
        JS_FreeValue(ctx, res);
        return JS_EXCEPTION;
    }
    return res;
}

/* ==================== end GEN-5 block ====================================. */

/* ==== generated family g6 ====. */

/* ---- logical reductions ----
   Eight-bit only, so no element it sees can be a NaN or a -0, and the mask
   argument is REQUIRED. TRUTHINESS IS JS TRUTHINESS, AND IT IS NOT `v != 0`. /. */

enum { DFL_BOOL_AND, DFL_BOOL_OR, DFL_BOOL_XOR };

/* The two predicates. Applied only to a local, like the DF_STEP_* family
   above, because each spells its argument more than once. */
#define DFL_TRUTHY_INT(v)    ((v) != 0)
#define DFL_TRUTHY_FLOAT(v)  ((v) == (v) && (v) != 0)

/* `restrict` here is the file's convention and costs nothing: both pointers are
   read-only, so a caller passing a u8 column as its own mask (legal --
   df_mask_arg accepts any Uint8Array) violates nothing. */
#define DFL_DEFINE_TRUE_COUNT(sfx, cty, TRUTHY)                               \
static uint32_t dfl_true_count_##sfx(const cty *restrict x, uint32_t n)       \
{                                                                             \
    uint32_t i, ntrue = 0;                                                    \
    for (i = 0; i < n; i++) {                                                 \
        cty v = x[i];                                                         \
        ntrue += (uint32_t)(TRUTHY(v) != 0);                                  \
    }                                                                         \
    return ntrue;                                                             \
}                                                                             \
static uint32_t dfl_true_count_masked_##sfx(const cty *restrict x,            \
                                            const uint8_t *restrict m,        \
                                            uint32_t n, uint32_t *psel)       \
{                                                                             \
    uint32_t i, ntrue = 0, nsel = 0;                                          \
    for (i = 0; i < n; i++) {                                                 \
        cty v = x[i];                                                         \
        int s = (m[i] != 0), t = (TRUTHY(v) != 0);                            \
        nsel += (uint32_t)s;                                                  \
        ntrue += (uint32_t)(s & t);                                           \
    }                                                                         \
    *psel = nsel;                                                             \
    return ntrue;                                                             \
}

#define DFL_TC_FLOAT(sfx, cty, tag) DFL_DEFINE_TRUE_COUNT(sfx, cty, DFL_TRUTHY_FLOAT)
#define DFL_TC_INT(sfx, cty, tag)   DFL_DEFINE_TRUE_COUNT(sfx, cty, DFL_TRUTHY_INT)
DF_FLOAT_TYPES(DFL_TC_FLOAT)
DF_INT_TYPES(DFL_TC_INT)
#undef DFL_TC_FLOAT
#undef DFL_TC_INT

/* BOOL_AND/BOOL_OR/BOOL_XOR(col[, mask]) -> Boolean. Vacuous over zero selected
   rows: BOOL_AND true, BOOL_OR false, BOOL_XOR false. */
static JSValue dyn_df_bool_reduce(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv, int magic)
{
    static const char *const names[] = { "BOOL_AND", "BOOL_OR", "BOOL_XOR" };
    DataFrame *df;
    DFBound b;
    const uint8_t *mask;
    int idx, ok;
    uint32_t ntrue = 0, nsel = 0;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    /* coerce EVERY argument before binding any column pointer: ToCString and
       the typed-array accessors run user JS that can detach this buffer. */
    idx = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (idx < 0)
        return JS_EXCEPTION;
    mask = df_mask_arg(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, df->nrows, &ok);
    if (!ok)
        return JS_EXCEPTION;

    /* no JS may run from here to the end of the loop. */
    if (dyn_df_bind(ctx, df, idx, &b))
        return JS_EXCEPTION;

#define X(sfx, cty, tag)                                                      \
    case tag:                                                                 \
        if (mask)                                                             \
            ntrue = dfl_true_count_masked_##sfx((const cty *)b.p, mask, b.n,  \
                                                &nsel);                       \
        else {                                                                \
            ntrue = dfl_true_count_##sfx((const cty *)b.p, b.n);              \
            nsel = b.n;                                                       \
        }                                                                     \
        break;
    switch (b.type) {
    DF_NUMERIC_TYPES(X)
    default:
        return JS_ThrowTypeError(ctx,
            "%s: column '%s' is %s; a logical reduction folds the TRUTHINESS "
            "of the stored values, and a dictionary code is not one",
            names[magic], df->cols[idx].name, df_type_name(b.type));
    }
#undef X

    switch (magic) {
    case DFL_BOOL_AND: return JS_NewBool(ctx, ntrue == nsel);
    case DFL_BOOL_OR:  return JS_NewBool(ctx, ntrue != 0);
    default:           return JS_NewBool(ctx, (ntrue & 1u) != 0);
    }
}

/* ---- SUM_CHECKED ----
   WHAT IS NOT BROKEN, so that nobody adds a check that cannot fire: the int64
   accumulator itself cannot overflow. THE TEST IS A ROUND TRIP, NOT A MAGNITUDE. */

/* Signed and unsigned integer columns need different accumulators, and the split
   is not cosmetic: the largest Uint32Array sum is (2^32-1)^2, which exceeds
   INT64_MAX. Together these two lists must cover DF_INT_TYPES exactly. */
#define DFL_SINT_TYPES(X)                                                     \
    X(i32, int32_t,  DF_I32)                                                  \
    X(i16, int16_t,  DF_I16)                                                  \
    X(i8,  int8_t,   DF_I8)
#define DFL_UINT_TYPES(X)                                                     \
    X(u32, uint32_t, DF_U32)                                                  \
    X(u16, uint16_t, DF_U16)                                                  \
    X(u8,  uint8_t,  DF_U8)

#define DFL_COUNT_ONE(sfx, cty, tag)  + 1
_Static_assert((0 DFL_SINT_TYPES(DFL_COUNT_ONE) DFL_UINT_TYPES(DFL_COUNT_ONE))
               == (0 DF_INT_TYPES(DFL_COUNT_ONE)),
               "DFL_SINT_TYPES + DFL_UINT_TYPES must cover DF_INT_TYPES");
#undef DFL_COUNT_ONE

/* Exact integer sum of the selected rows. A masked-out row folds in the
   identity 0 rather than branching, matching the file's masked convention. */
#define DFL_DEFINE_ISUM(sfx, cty, acct)                                       \
static acct dfl_isum_##sfx(const cty *restrict x, const uint8_t *restrict m,  \
                           uint32_t n)                                        \
{                                                                             \
    acct s = 0;                                                               \
    uint32_t i;                                                               \
    if (m) {                                                                  \
        for (i = 0; i < n; i++) {                                             \
            acct v = (acct)x[i];                                              \
            s += m[i] ? v : (acct)0;                                          \
        }                                                                     \
    } else {                                                                  \
        for (i = 0; i < n; i++)                                               \
            s += (acct)x[i];                                                  \
    }                                                                         \
    return s;                                                                 \
}

#define DFL_ISUM_S(sfx, cty, tag)  DFL_DEFINE_ISUM(sfx, cty, int64_t)
#define DFL_ISUM_U(sfx, cty, tag)  DFL_DEFINE_ISUM(sfx, cty, uint64_t)
DFL_SINT_TYPES(DFL_ISUM_S)
DFL_UINT_TYPES(DFL_ISUM_U)
#undef DFL_ISUM_S
#undef DFL_ISUM_U

/* Does `d`, which is (double)s, convert back to s exactly? Both bounds are
   exactly representable doubles. */
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

/* SUM_CHECKED(col[, mask]) -> Number. The same value sum() returns, except that
   it refuses to return a rounded one. Integer columns only; an empty column or a
   mask selecting nothing gives 0, which is exact. */
static JSValue dyn_df_sumChecked(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    DataFrame *df;
    DFBound b;
    const uint8_t *mask;
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

    /* no JS may run from here to the end of the loop. */
    if (dyn_df_bind(ctx, df, idx, &b))
        return JS_EXCEPTION;

    /* One switch both dispatches and refuses, so "is this an integer column"
       exists once and cannot drift from the kernels these lists generated. A
       float column is named, not summed. */
    switch (b.type) {
#define X(sfx, cty, tag)                                                      \
    case tag:                                                                 \
        si = dfl_isum_##sfx((const cty *)b.p, mask, b.n);                     \
        is_signed = 1;                                                        \
        break;
    DFL_SINT_TYPES(X)
#undef X
#define X(sfx, cty, tag)                                                      \
    case tag:                                                                 \
        ui = dfl_isum_##sfx((const cty *)b.p, mask, b.n);                     \
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
        if (!dfl_i64_exact_as_double(si, d))
            return JS_ThrowRangeError(ctx,
                "SUM_CHECKED: column '%s' totals %lld, which a Number cannot "
                "hold exactly; sum() returns %.17g",
                df->cols[idx].name, (long long)si, d);
    } else {
        d = (double)ui;
        if (!dfl_u64_exact_as_double(ui, d))
            return JS_ThrowRangeError(ctx,
                "SUM_CHECKED: column '%s' totals %llu, which a Number cannot "
                "hold exactly; sum() returns %.17g",
                df->cols[idx].name, (unsigned long long)ui, d);
    }
    return JS_NewFloat64(ctx, d);
}

/* ==== generated family g7 ====. */

/* ------------------------------------------------ grouped aggregates (G7) */

/* They are rows in the prototype table, not code. KEY HANDLING IS
   dyn_df_group_by_sum's, VERBATIM. GROUP_BY_SUM keeps its 0 for an empty group,
   because sum is the one aggregate whose identity the scalar path does publish. */

/* The aggregate as it appears in the string-column refusal. Named after the
   AGGREGATE, not the method, because MIN_MAP and GROUP_BY_MIN are the same call. */
static const char *dfg_agg_verb(int magic)
{
    switch (magic) {
    case DF_MIN:  return "take the min of";
    case DF_MAX:  return "take the max of";
    default:      return "take the mean of";
    }
}

/* One scatter pass. LOAD reads the value column -- specialised on DF_F64 exactly
   as dyn_df_group_by_sum's inner loop is, everything else through df_get -- and
   STEP folds it into the group's accumulator. */
#define DFG_SCATTER(LOAD, STEP)                                               \
    do {                                                                      \
        for (i = 0; i < n; i++) {                                             \
            double v;                                                         \
            if (mask && !mask[i]) continue;                                   \
            g = gk ? gk[i] : (uint32_t)df_get(kb.p, kb.type, i);              \
            if (g >= ngroups) continue;                                       \
            v = (LOAD);                                                       \
            STEP;                                                             \
            cnt[g]++;                                                         \
        }                                                                     \
    } while (0)
#define DFG_SCATTER_BY_TYPE(STEP)                                             \
    do {                                                                      \
        if (vb.type == DF_F64) {                                              \
            const double *x = vb.p;                                           \
            DFG_SCATTER(x[i], STEP);                                          \
        } else {                                                              \
            DFG_SCATTER(df_get(vb.p, vb.type, i), STEP);                      \
        }                                                                     \
    } while (0)

static JSValue dyn_df_group_agg(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv, int magic)
{
    DataFrame *df;
    DFBound kb, vb;
    const uint8_t *mask;
    int ki, vi = -1, ok, wants_value = (magic != DF_COUNT);
    uint32_t i, ngroups, nkeys, g;
    double *acc = NULL;
    uint32_t *cnt = NULL, *gk = NULL;
    JSValue keys = JS_UNDEFINED, vals = JS_UNDEFINED, res = JS_UNDEFINED;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    /* coerce EVERY argument before binding any column pointer: ToCString can
       run user JS that detaches the very buffer we are about to alias. */
    ki = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (ki < 0)
        return JS_EXCEPTION;
    if (wants_value) {
        vi = df_col_arg(ctx, df, argc > 1 ? argv[1] : JS_UNDEFINED);
        if (vi < 0)
            return JS_EXCEPTION;
    } else if (argc > 1 && JS_IsString(argv[1])) {
        /* a type test, not a coercion: nothing runs. Reinterpreting the name as
           a mask would fail with "mask must be a Uint8Array", which names the
           wrong mistake. */
        return JS_ThrowTypeError(ctx, "GROUP_BY_COUNT(keyCol[, mask]) takes no "
                                 "value column");
    }
    mask = df_mask_arg(ctx, argc > (wants_value ? 2 : 1)
                            ? argv[wants_value ? 2 : 1] : JS_UNDEFINED,
                       df->nrows, &ok);
    if (!ok)
        return JS_EXCEPTION;

    /* One counter for every grouped method: this was a second copy of the same
       arithmetic, and it drifted -- an empty integer key reported one group
       where the string path correctly reported none. */
    if (dfc_group_count(ctx, df, ki, &nkeys, &ngroups))
        return JS_EXCEPTION;

    acc = calloc(ngroups, sizeof(double));
    cnt = calloc(ngroups, sizeof(uint32_t));
    if (!acc || !cnt) {
        JS_ThrowOutOfMemory(ctx);
        goto fail;
    }
    if (magic == DF_MIN || magic == DF_MAX) {
        double seed = (magic == DF_MIN) ? INFINITY : -INFINITY;
        for (i = 0; i < ngroups; i++)   /* the whole allocation, not nkeys */
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

    /* no JS may run from here to the end of the scatter. */
    {
        uint32_t n = kb.n;
        if (wants_value && vb.n < n)
            n = vb.n;
        gk = df_keys_u32(&kb, n);
        switch (magic) {
        case DF_COUNT:
            for (i = 0; i < n; i++) {
                if (mask && !mask[i]) continue;
                g = gk ? gk[i] : (uint32_t)df_get(kb.p, kb.type, i);
                if (g < ngroups) cnt[g]++;
            }
            break;
        case DF_MIN: DFG_SCATTER_BY_TYPE(if (v < acc[g]) acc[g] = v); break;
        case DF_MAX: DFG_SCATTER_BY_TYPE(if (v > acc[g]) acc[g] = v); break;
        default:     DFG_SCATTER_BY_TYPE(acc[g] += v); break;   /* DF_MEAN */
        }
        free(gk);
        gk = NULL;
    }

    /* Finalise in place; the accumulator becomes the result buffer. A group with
       no contributing row at all (cnt == 0) is NaN, because the scalar reduce
       answers that with `undefined` and a Float64Array cannot hold one. */
    if (magic == DF_COUNT) {
        for (i = 0; i < nkeys; i++) acc[i] = (double)cnt[i];
    } else if (magic == DF_MEAN) {
        for (i = 0; i < nkeys; i++) acc[i] = cnt[i] ? acc[i] / cnt[i] : NAN;
    } else {
        for (i = 0; i < nkeys; i++) if (!cnt[i]) acc[i] = NAN;
    }
    free(cnt);
    cnt = NULL;

    vals = df_to_typed_array(ctx, acc, (size_t)nkeys * sizeof(double),
                             JS_TYPED_ARRAY_FLOAT64);
    acc = NULL;
    if (JS_IsException(vals))
        goto fail;

    /* keys: the dictionary strings, or the integer codes. */
    keys = JS_NewArray(ctx);
    if (JS_IsException(keys))
        goto fail;
    for (i = 0; i < nkeys; i++) {
        JSValue k;
        if (df->cols[ki].type == DF_STR)
            k = JS_NewString(ctx, df->cols[ki].dict[i]);
        else
            k = JS_NewInt64(ctx, i);
        if (JS_IsException(k) ||
            JS_DefinePropertyValueUint32(ctx, keys, i, k, JS_PROP_C_W_E) < 0)
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

/* --------------------------------------------------- rolling windows (G7) */

/* It is the baseline an incremental or deque-based version has to beat, and
   without it that version cannot be proved. A window may therefore contribute
   fewer than w values, and ROLLING_MEAN divides by what contributed, never by w. */

static const char *dfg_roll_name(int magic)
{
    switch (magic) {
    case DF_MIN:  return "ROLLING_MIN";
    case DF_MAX:  return "ROLLING_MAX";
    case DF_MEAN: return "ROLLING_MEAN";
    default:      return "ROLLING_SUM";
    }
}

/* INIT seeds the window accumulator, STEP folds one value in, FINISH turns it
   into the output -- and FINISH reads `c` in every arm including sum's, where
   `c ? a : 0.0` is the identity spelled out rather than a set-and-never-read. */
#define DFG_ROLL(INIT, STEP, FINISH)                                          \
    do {                                                                      \
        for (i = 0; i < span; i++) {                                          \
            double a = (INIT);                                                \
            uint32_t c = 0, j;                                                \
            if (i < w - 1) { dst[i] = NAN; continue; }                        \
            for (j = i - (w - 1); j <= i; j++) {                              \
                double v;                                                     \
                if (mask && !mask[j]) continue;                               \
                v = df_get(b.p, b.type, j);                                   \
                STEP;                                                         \
                c++;                                                          \
            }                                                                 \
            dst[i] = (FINISH);                                                \
        }                                                                     \
    } while (0)

/* Monotonic deque: each index pushed and popped once, so O(n) not O(n*w). Exact
   because min/max SELECT an element -- sum/mean cannot use this, they would have
   to subtract and drift. Returns -1 if it declined; caller falls back. */
static int dfg_roll_extreme(const DFBound *b, const uint8_t *mask, double *dst,
                            uint32_t span, uint32_t w, int want_min)
{
    uint32_t *dq;
    double *wv, *own;
    uint32_t head = 0, tail = 0, i, sel = 0;
    double seed = want_min ? INFINITY : -INFINITY;

    dq = malloc((size_t)span * sizeof(*dq) + 1);
    if (!dq)
        return -1;
    /* The deque reads a value three times per element, so the type switch would
       be paid three times over. */
    if (b->type == DF_F64) {
        wv = (double *)b->p;            /* already doubles: borrow, do not copy */
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
        /* the row leaving the window on the left, if any. */
        if (i >= w && !(mask && !mask[i - w]))
            sel--;
        /* drop indices that have fallen out of the left edge. */
        while (head < tail && dq[head] + w <= i)
            head++;
        if (!(mask && !mask[i])) {
            sel++;
            v = wv[i];
            if (!(v != v)) {                    /* NaN never enters */
                while (head < tail && (want_min ? (wv[dq[tail - 1]] >= v)
                                                : (wv[dq[tail - 1]] <= v)))
                    tail--;
                dq[tail++] = i;
            }
        }
        /* sel != 0 with an empty deque means selected rows that were all NaN:
           that yields the seed, as scalar min does. Nothing selected yields NaN. */
        dst[i] = i < w - 1  ? NAN
               : head < tail ? wv[dq[head]]
               : sel         ? seed
               : NAN;
    }
    free(own);
    free(dq);
    return 0;
}

/* Unmasked window sum with independent accumulators; `a += v` is w add
   latencies per output. Gated so short windows keep the left-to-right
   association their pinned values use. -1 means the generic macro runs. */
#define DFG_ROLL_UNROLL 8u

/* DFG_ROLL_SLIDE_MIN is defined above dfc_moving_blocks: both windowed sums
   use one threshold, and a second copy would drift from it. */

static int dfg_roll_sum(const DFBound *b, double *dst, uint32_t span,
                        uint32_t w, int want_mean)
{
    const double *x;
    double *own = NULL;
    uint32_t i;

    if (w < DFG_ROLL_UNROLL || span < w)
        return -1;
    if (b->type == DF_F64) {
        x = b->p;                       /* already doubles: borrow, do not copy */
    } else {
        x = own = malloc((size_t)span * sizeof(double) + 1);
        if (!own)
            return -1;
        df_widen(b, own, span);
    }
    if (w >= DFG_ROLL_SLIDE_MIN) {
        /* Block decomposition, NOT a subtractive running sum. Each window is
           suffix(previous block) + prefix(current), so every element is added
           exactly twice and nothing is ever subtracted -- O(n) with the
           numerics of two contiguous sums. `acc += x[i] - x[i-w]` is O(n) too
           and silently wrong: when 1e308 leaves a window of 1s the running sum
           cancels to 0, which is FINITE, so no overflow check can catch it.
           Non-finite input needs no special case here either: a window holding
           an Inf reports Inf, and one that does not is unaffected. */
        double *suf = malloc((size_t)w * sizeof(double));
        if (suf) {
            uint32_t s;
            for (i = 0; i + 1 < w; i++)
                dst[i] = NAN;
            for (s = 0; s < span; s += w) {
                uint32_t end = (s + w < span) ? s + w : span;
                uint32_t k, lo, hi;
                double run = 0, pre = 0;
                for (k = end; k-- > s; ) {       /* suffix sums of this block */
                    run += x[k];
                    suf[k - s] = run;
                }
                lo = s + w - 1;
                hi = s + 2 * w - 2;
                if (hi >= span)
                    hi = span - 1;
                for (k = lo; k <= hi; k++) {
                    uint32_t st = k + 1 - w;     /* window is [st, k] */
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
        dst[i] = NAN;
    for (i = w - 1; i < span; i++) {
        const double *p = x + (i - (w - 1));
        double a0 = 0, a1 = 0, a2 = 0, a3 = 0, a4 = 0, a5 = 0, a6 = 0, a7 = 0;
        uint32_t j, m = w & ~7u;
        for (j = 0; j < m; j += 8) {
            a0 += p[j];     a1 += p[j + 1]; a2 += p[j + 2]; a3 += p[j + 3];
            a4 += p[j + 4]; a5 += p[j + 5]; a6 += p[j + 6]; a7 += p[j + 7];
        }
        for (; j < w; j++)
            a0 += p[j];
        a0 = ((a0 + a1) + (a2 + a3)) + ((a4 + a5) + (a6 + a7));
        dst[i] = want_mean ? a0 / (double)w : a0;
    }
    free(own);
    return 0;
}

static JSValue dyn_df_rolling(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv, int magic)
{
    DataFrame *df;
    DFBound b;
    const uint8_t *mask;
    double wd, *dst;
    uint32_t i, n, span, w;
    int idx, ok;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    idx = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (idx < 0)
        return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &wd, argc > 1 ? argv[1] : JS_UNDEFINED))
        return JS_EXCEPTION;
    mask = df_mask_arg(ctx, argc > 2 ? argv[2] : JS_UNDEFINED, df->nrows, &ok);
    if (!ok)
        return JS_EXCEPTION;

    /* Validated in DOUBLE and once per call. A window LONGER than the column is
       not an error -- it simply never fills. */
    if (!(wd >= 1) || wd != floor(wd) || wd > (double)UINT32_MAX)
        return JS_ThrowRangeError(ctx, "%s: window must be a positive integer, "
                                  "got %g", dfg_roll_name(magic), wd);
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

    /* no JS may run from here to the end of the loop. */
    switch (magic) {
    case DF_MIN:
        if (dfg_roll_extreme(&b, mask, dst, span, w, 1) == 0) break;
        DFG_ROLL(INFINITY, if (v < a) a = v, c ? a : NAN); break;
    case DF_MAX:
        if (dfg_roll_extreme(&b, mask, dst, span, w, 0) == 0) break;
        DFG_ROLL(-INFINITY, if (v > a) a = v, c ? a : NAN); break;
    case DF_MEAN:
        if (!mask && dfg_roll_sum(&b, dst, span, w, 1) == 0) break;
        DFG_ROLL(0.0, a += v, c ? a / c : NAN); break;
    default:
        if (!mask && dfg_roll_sum(&b, dst, span, w, 0) == 0) break;
        DFG_ROLL(0.0, a += v, c ? a : 0.0); break;      /* DF_SUM */
    }
    return df_to_typed_array(ctx, dst, (size_t)n * sizeof(double),
                             JS_TYPED_ARRAY_FLOAT64);
}
#undef DFG_ROLL

/* ---------------------------------------------------------- dropna (G7) */

/* A mask composes with every reduction here; a compacted column changes the row
   count, so it cannot be handed back to the frame it came from, and one returned
   array could not compact the other columns anyway. */
static JSValue dyn_df_dropna(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    DataFrame *df;
    DFBound b;
    uint8_t *dst;
    int *sel;
    uint32_t i, k, nsel = 0, n;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    n = df->nrows;
    sel = df_out_alloc(ctx, (uint32_t)(argc > 0 ? argc : (int)df->ncols),
                       sizeof(int));
    if (!sel)
        return JS_EXCEPTION;

    /* Resolve EVERY name before binding anything: ToCString runs user JS that
       can detach a buffer a later column would be read through. */
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
        /* Only a float column can hold a NaN, so an integer one is not scanned
           at all -- isnan() over it is false by construction, not by luck. */
        /* Branchless: the conditional store was a per-element branch on data
           the predictor cannot learn, and it blocked vectorisation. */
        if (b.type == DF_F64) {
            const double *x = b.p;
            for (i = 0; i < lim; i++) dst[i] &= (uint8_t)(x[i] == x[i]);
        } else if (b.type == DF_F32) {
            const float *x = b.p;
            for (i = 0; i < lim; i++) dst[i] &= (uint8_t)(x[i] == x[i]);
        }
        /* a row the column does not reach has no value there: missing. */
        for (i = lim; i < n; i++)
            dst[i] = 0;
    }
    free(sel);
    return df_to_typed_array(ctx, dst, n, JS_TYPED_ARRAY_UINT8);
}

/* ==== generated family g8 ====. */

/* ---- approximate sketches ----
   ------------------------------------------------------ approximate sketches
   Four methods that are ALLOWED TO BE WRONG. This binds that one. /. */
#include "core/dyn-ds.h"      /* dyn_hll_t -- the HyperLogLog is already here */
#include "core/dyn-hash.h"    /* dyn_mix64, dyn_xxh64 */

/* Every parameter here trades memory for accuracy, so each one is named, and the
   error it buys is on the line next to it. */
#define DFA_HLL_PRECISION   14

/* t-digest compression. Shipped bound: |rank(returned value) - q| <= 0.01. */
#define DFA_TD_COMPRESSION  100.0
/* k1 compresses to ~2*delta centroids; 400 is that with slack, and the merge
 * loop below has a hard valve so it can never be exceeded even if that bound is
 * wrong. 1024 buffered values amortise the sort over the merge. */
#define DFA_TD_MAIN_CAP     400
#define DFA_TD_BUF_CAP      1024
#define DFA_TD_MERGE_CAP    (DFA_TD_MAIN_CAP + DFA_TD_BUF_CAP)

/* The guarantee is a function of m alone -- an estimate is never below the truth
   and never above it by more than the smallest count in the summary, which is at
   most n/m, so every item with true frequency above n/m is present. */
#define DFA_SS_COUNTERS_PER_K   8u
#define DFA_SS_MIN_COUNTERS     256u
#define DFA_SS_MAX_COUNTERS     8192u
#define DFA_TOPK_MAX_K          1024

/* MinHash sketch size. Standard error is sqrt(J(1-J)/k) <= 0.5/sqrt(256) =
 * 3.1%; shipped bound 0.10. Below 256 distinct values in the union the sketch
 * holds the whole union and the answer is EXACT. */
#define DFA_KMV_K           256u

/* ---- hashing, once ----
   There is deliberately no memcpy of an integer into a byte buffer here, because
   that would reintroduce the endianness question this avoids. /. */
static uint64_t dfa_canon_bits(double v)
{
    uint64_t b;
    if (v != v)                 /* every NaN payload collapses to ONE value */
        return 0x7FF8000000000000ULL;
    if (v == 0.0)               /* -0.0 == 0.0, so this catches both signs */
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

/* The hash of row i's VALUE. A string column hashes the dictionary entry's
 * BYTES, so the number 5 and the string "5" are correctly different values and
 * a numeric column compared against a string one is correctly dissimilar. */
static uint64_t dfa_row_hash(const DFColumn *c, const DFBound *b, uint32_t i)
{
    if (b->type == DF_STR) {
        uint32_t code = (uint32_t)((const int32_t *)b->p)[i];
        const char *s;
        if (code >= c->dict_len)
            return 0;           /* unreachable: a code is an index the
                                   constructor produced. A bounds check and not
                                   an assertion, because the failure would be an
                                   out-of-bounds read, not a wrong number. */
        s = c->dict[code];
        return dyn_xxh64((const uint8_t *)s, strlen(s), 0);
    }
    return dfa_hash_bits(dfa_canon_bits(df_get(b->p, b->type, i)));
}

/* A total order over doubles, NaN last, for the deterministic tie-break in
 * APPROX_TOP_K. Plain < is not one: NaN compares false against everything, so a
 * comparator built on it makes qsort's output depend on the input order. */
static int dfa_total_order(double a, double b)
{
    int na = (a != a), nb = (b != b);
    if (na || nb)
        return na - nb;
    if (a < b) return -1;
    if (a > b) return 1;
    return 0;                   /* +0 and -0 were canonicalised away already */
}

/* {keys, values}: the shape GROUP_BY_SUM returns and the shape the EXACT TOP_K
   returns, so an approximate and an exact answer are directly diffable. */
static JSValue dfa_result_pair(JSContext *ctx, JSValue keys, double *vals,
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
    /* JS_DefinePropertyValue* consumes its value even when it fails, so a
       failed define must not be followed by a free of the same value. */
    if (JS_DefinePropertyValueStr(ctx, res, "keys", keys, JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, values);
        JS_FreeValue(ctx, res);
        return JS_EXCEPTION;
    }
    if (JS_DefinePropertyValueStr(ctx, res, "values", values,
                                  JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, res);
        return JS_EXCEPTION;
    }
    return res;
}

/* ------------------------------------------------- APPROX_COUNT_DISTINCT */

/* Error zero satisfies any bound the method advertises. This must agree with
   nunique() on the same column BY CONSTRUCTION, and that identity is a test
   rather than a comment. */
static JSValue dfa_exact_distinct_str(JSContext *ctx, const DFColumn *c,
                                      const DFBound *b, const uint8_t *mask)
{
    const int32_t *codes = b->p;
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

/* APPROX_COUNT_DISTINCT(column[, mask]) -> Number The estimate is returned
   UNROUNDED. The exact string path returns an integer because it is one. */
static JSValue dyn_df_approxCountDistinct(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv)
{
    DataFrame *df;
    DFBound b;
    const uint8_t *mask;
    dyn_hll_t *hll;
    int idx, ok;
    uint32_t i, any = 0;
    double est;

    df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    if (!df)
        return JS_EXCEPTION;
    /* coerce every argument before binding any column pointer: both of these
       can run user JS that detaches the buffer about to be aliased. */
    idx = df_col_arg(ctx, df, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (idx < 0)
        return JS_EXCEPTION;
    mask = df_mask_arg(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, df->nrows, &ok);
    if (!ok)
        return JS_EXCEPTION;

    /* no JS may run from here to the end of the loop. */
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
    /* An empty or fully masked column is 0, said here rather than inherited
       from the estimator's formula, so the contract cannot move under us. */
    est = any ? dyn_hll_count(hll) : 0.0;
    dyn_hll_free(hll);
    return JS_NewFloat64(ctx, est);
}

/* ---- APPROX_PERCENTILE ----
   ---------------------------------------------------- APPROX_PERCENTILE Merging
   t-digest (Dunning & Ertl, "Computing Extremely Accurate Quantiles Using. */
#define DFA_TD_HALF_PI  1.5707963267948966
#define DFA_TD_NORM     (DFA_TD_COMPRESSION / (2.0 * 3.14159265358979323846))

typedef struct { double mean, weight; } dfa_centroid;

typedef struct {
    uint32_t nmain, nbuf;
    double total, vmin, vmax;
    dfa_centroid main[DFA_TD_MAIN_CAP];
    dfa_centroid merge[DFA_TD_MERGE_CAP];
    dfa_centroid buf[DFA_TD_BUF_CAP];
} dfa_tdigest;

static int dfa_cmp_centroid(const void *pa, const void *pb)
{
    double a = ((const dfa_centroid *)pa)->mean;
    double b = ((const dfa_centroid *)pb)->mean;

    return a < b ? -1 : (a > b ? 1 : 0);
}

static double dfa_td_k(double q)
{
    if (q <= 0.0) return -DFA_TD_NORM * DFA_TD_HALF_PI;
    if (q >= 1.0) return  DFA_TD_NORM * DFA_TD_HALF_PI;
    return DFA_TD_NORM * asin(2.0 * q - 1.0);
}

static double dfa_td_q(double k)
{
    double t = k / DFA_TD_NORM;
    if (t <= -DFA_TD_HALF_PI) return 0.0;
    if (t >=  DFA_TD_HALF_PI) return 1.0;
    return (sin(t) + 1.0) / 2.0;
}

/* Weighted mean of two centroids, defended against the infinities a column may
   legally hold: (+inf) - (+inf) is NaN, and ONE NaN centroid mean makes every
   quantile NaN -- a plausible wrong answer, not a crash. */
static double dfa_td_merge_mean(double ma, double wa, double mb, double wb)
{
    if (ma == mb)
        return ma;
    if (!isfinite(ma) || !isfinite(mb))
        return wa >= wb ? ma : mb;
    return ma + (mb - ma) * (wb / (wa + wb));
}

static void dfa_td_init(dfa_tdigest *t)
{
    t->nmain = 0;
    t->nbuf = 0;
    t->total = 0.0;
    t->vmin = INFINITY;
    t->vmax = -INFINITY;
}

static void dfa_td_flush(dfa_tdigest *t)
{
    uint32_t i, j, n = 0, out = 0;
    double w_so_far, q_limit;
    dfa_centroid cur;

    if (t->nbuf == 0)
        return;
    qsort(t->buf, t->nbuf, sizeof(dfa_centroid), dfa_cmp_centroid);

    /* two sorted runs (existing centroids, buffered singletons) into one. */
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

    /* Absorb while the span stays inside one k-step. `out == CAP - 1` is a hard
       valve: at that point everything remaining folds into the last centroid,
       so the array cannot overflow even if the ~2*delta bound were wrong. */
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

/* Buffered points carry a weight so the weighted quantile shares this digest.
   A non-finite or non-positive weight contributes nothing rather than poisoning
   the running total. */
static void dfa_td_add_w(dfa_tdigest *t, double v, double w)
{
    if (v != v)                 /* a percentile is an order statistic and NaN is
                                   not ordered, so it is SKIPPED -- the same
                                   choice min and max already make */
        return;
    if (!(w > 0.0) || w != w)
        return;
    if (v < t->vmin) t->vmin = v;
    if (v > t->vmax) t->vmax = v;
    t->total += w;
    t->buf[t->nbuf].mean = v;
    t->buf[t->nbuf].weight = w;
    t->nbuf++;
    if (t->nbuf == DFA_TD_BUF_CAP)
        dfa_td_flush(t);
}

static void dfa_td_add(dfa_tdigest *t, double v)
{
    dfa_td_add_w(t, v, 1.0);
}

/* Interpolate, then clamp into the observed range: an estimate must never report
   a value the column does not bracket. */
static double dfa_td_lerp(double left, double right, double frac,
                          double lo, double hi)
{
    double d = right - left, v;
    v = (d == 0.0 || !isfinite(d)) ? left : left + frac * d;
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return v;
}

static double dfa_td_quantile(dfa_tdigest *t, double q)
{
    uint32_t i;
    double index, w_so_far;

    dfa_td_flush(t);
    if (q <= 0.0) return t->vmin;       /* EXACT: tracked, not interpolated */
    if (q >= 1.0) return t->vmax;
    if (t->nmain == 1) return t->main[0].mean;

    index = q * t->total;
    w_so_far = 0.0;
    for (i = 0; i < t->nmain; i++) {
        double w = t->main[i].weight;
        double centre = w_so_far + w / 2.0;
        if (index <= centre) {
            double left  = (i == 0) ? t->vmin : t->main[i - 1].mean;
            double cleft = (i == 0) ? 0.0
                                    : w_so_far - t->main[i - 1].weight / 2.0;
            double span  = centre - cleft;
            double frac  = span > 0.0 ? (index - cleft) / span : 0.0;
            return dfa_td_lerp(left, t->main[i].mean, frac, t->vmin, t->vmax);
        }
        w_so_far += w;
    }
    {
        dfa_centroid last = t->main[t->nmain - 1];
        double cleft = t->total - last.weight / 2.0;
        double span  = t->total - cleft;
        double frac  = span > 0.0 ? (index - cleft) / span : 1.0;
        return dfa_td_lerp(last.mean, t->vmax, frac, t->vmin, t->vmax);
    }
}

/* APPROX_PERCENTILE(column, q[, mask]) -> Number, or undefined for no values.
   undefined rather than NaN for an empty column, because that is what min and
   max already return for one and a percentile is the same kind of question. */
static JSValue dyn_df_approxPercentile(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    DataFrame *df;
    DFBound b;
    const uint8_t *mask;
    dfa_tdigest *t;
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
    /* NaN fails this too: `!(q >= 0 && q <= 1)` and not `q < 0 || q > 1`. One
       spelling of the argument, in [0, 1]; a percentage is declined rather than
       guessed at, because 0.95 and 95 would both be plausible. */
    if (!(q >= 0.0 && q <= 1.0))
        return JS_ThrowRangeError(ctx, "q must be a number in [0, 1]");
    mask = df_mask_arg(ctx, argc > 2 ? argv[2] : JS_UNDEFINED, df->nrows, &ok);
    if (!ok)
        return JS_EXCEPTION;

    if (dyn_df_bind(ctx, df, idx, &b))
        return JS_EXCEPTION;
    if (b.type == DF_STR)
        return JS_ThrowTypeError(ctx, "cannot reduce a string column");

    t = malloc(sizeof(*t));     /* ~37 KB: too large for a stack frame */
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

/* ---- APPROX_TOP_K ----
   --------------------------------------------------------- APPROX_TOP_K Space-
   Saving (Metwally, Agrawal, El Abbadi 2005). */
typedef struct {
    uint64_t bits;      /* the canonical value, as bits */
    uint64_t count;     /* the estimate for this value */
    uint32_t islot;     /* this counter's slot in idx[], so a swap is O(1) */
} dfa_ss_counter;

typedef struct {
    dfa_ss_counter *c;
    uint32_t *idx;
    uint32_t m, n, islots, used;
} dfa_ss;

#define DFA_SS_EMPTY 0xFFFFFFFFu
#define DFA_SS_DEAD  0xFFFFFFFEu

static void dfa_ss_free(dfa_ss *s)
{
    free(s->c);
    free(s->idx);
    s->c = NULL;
    s->idx = NULL;
}

static int dfa_ss_init(dfa_ss *s, uint32_t m)
{
    s->m = m;
    s->n = 0;
    s->used = 0;
    s->islots = 4 * m;          /* m is a power of two, so this is one too */
    s->c = malloc((size_t)m * sizeof(*s->c));
    s->idx = malloc((size_t)s->islots * sizeof(uint32_t));
    if (!s->c || !s->idx) {
        dfa_ss_free(s);
        return -1;
    }
    memset(s->idx, 0xFF, (size_t)s->islots * sizeof(uint32_t));
    return 0;
}

/* The slot for `bits`: the one holding it if present, else where it would go.
 * Tombstones are stepped over on a lookup but remembered as an insertion point,
 * so a table churning through evictions does not lengthen its probes. */
static uint32_t dfa_ss_slot(const dfa_ss *s, uint64_t bits, uint64_t h,
                            int *found)
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

/* One tombstone accumulates per eviction. */
static void dfa_ss_reindex(dfa_ss *s)
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

static void dfa_ss_place(dfa_ss *s, uint32_t pos, const dfa_ss_counter *v)
{
    s->c[pos] = *v;
    s->idx[s->c[pos].islot] = pos;
}

static void dfa_ss_sift_up(dfa_ss *s, uint32_t pos)
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

static void dfa_ss_sift_down(dfa_ss *s, uint32_t pos)
{
    dfa_ss_counter v = s->c[pos];

    for (;;) {
        uint32_t l = 2 * pos + 1, r = l + 1, sm = pos;
        uint64_t best = v.count;
        if (l < s->n && s->c[l].count < best) { sm = l; best = s->c[l].count; }
        if (r < s->n && s->c[r].count < best) { sm = r; }
        if (sm == pos)
            break;
        dfa_ss_place(s, pos, &s->c[sm]);
        pos = sm;
    }
    dfa_ss_place(s, pos, &v);
}

/* The hash is derived here rather than passed in, so the value that indexes the
 * table and the value stored in the counter cannot drift apart -- dfa_ss_reindex
 * recomputes it from the bits and would otherwise have to agree with a caller. */
static void dfa_ss_add(dfa_ss *s, uint64_t bits)
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
            s->used++;          /* reusing a tombstone does not add occupancy */
        s->c[s->n] = v;
        s->idx[slot] = s->n;
        s->n++;
        dfa_ss_sift_up(s, s->n - 1);
    } else {
        /* Full. dfa_ss_slot returned an EMPTY or DEAD slot, never the victim's
           own (that one holds a different, present key), so marking the victim
           dead cannot invalidate it. */
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

static int dfa_ss_cmp(const void *pa, const void *pb)
{
    const dfa_ss_counter *a = pa, *b = pb;

    if (a->count != b->count)
        return a->count > b->count ? -1 : 1;         /* count DESCENDING */
    /* Ties break on the value, ascending, NaN last. Without a total order here
       the reported order would depend on the row order, and two runs over the
       same data could disagree. */
    return dfa_total_order(dfa_bits_value(a->bits), dfa_bits_value(b->bits));
}

/* EXACT for a dictionary-encoded column, for the same reason GROUP_BY_SUM is:
   the codes index a counter directly, so there is nothing to estimate. Memory is
   bounded by the frame's own dictionary. */
typedef struct { uint64_t count; uint32_t code; } dfa_codecount;

static int dfa_cmp_codecount(const void *pa, const void *pb)
{
    const dfa_codecount *a = pa, *b = pb;

    if (a->count != b->count)
        return a->count > b->count ? -1 : 1;
    return a->code < b->code ? -1 : (a->code > b->code ? 1 : 0);
}

static JSValue dfa_exact_topk_str(JSContext *ctx, const DFColumn *c,
                                  const DFBound *b, const uint8_t *mask,
                                  uint32_t k)
{
    const int32_t *codes = b->p;
    uint64_t *counts;
    dfa_codecount *rank = NULL;
    double *vals = NULL;
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
            continue;           /* absent under the mask, or absent outright */
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
        if (JS_IsException(key) ||
            JS_DefinePropertyValueUint32(ctx, keys, i, key,
                                         JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, keys);
            keys = JS_EXCEPTION;
        }
    }
    free(rank);
    return dfa_result_pair(ctx, keys, vals, nout);
}

/* APPROX_TOP_K(column, k[, mask]) -> { keys, values } The k most FREQUENT
   values, not the k largest. Same shape as GROUP_BY_SUM and as the exact TOP_K,
   so the two are substitutable and diffable. */
static JSValue dyn_df_approxTopK(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    DataFrame *df;
    DFBound b;
    dfa_ss s;
    const uint8_t *mask;
    double *vals;
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
    /* No default k. The counter array is sized from it, so the memory this call
       takes is always a number the caller stated. */
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
    if (m < DFA_SS_MIN_COUNTERS) m = DFA_SS_MIN_COUNTERS;
    if (m > DFA_SS_MAX_COUNTERS) m = DFA_SS_MAX_COUNTERS;
    while (m & (m - 1))                 /* round up to a power of two: the
                                           index mask depends on it */
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
        if (JS_IsException(key) ||
            JS_DefinePropertyValueUint32(ctx, keys, i, key,
                                         JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, keys);
            keys = JS_EXCEPTION;
        }
    }
    dfa_ss_free(&s);
    return dfa_result_pair(ctx, keys, vals, nout);
}

/* ---- APPROX_SIMILARITY ----
   Bottom-k rather than k independent permutations because it has the same
   1/sqrt(k) accuracy for one hash per row instead of k, and because it degrades. */
typedef struct {
    uint64_t h[DFA_KMV_K];      /* max-heap: h[0] is the largest kept */
    uint32_t n;
} dfa_kmv;

static void dfa_kmv_add(dfa_kmv *s, uint64_t h)
{
    uint32_t i, pos;

    /* The cheap summary test that makes this O(1) for almost every row of a
       high-cardinality column: anything at or above the current largest cannot
       be among the k smallest, and needs no membership scan. */
    if (s->n == DFA_KMV_K && h >= s->h[0])
        return;
    for (i = 0; i < s->n; i++)
        if (s->h[i] == h)
            return;             /* the sketch holds DISTINCT hashes; a repeat
                                   must not take a second slot, or a column of
                                   one value would look like k of them */
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
        if (l < s->n && s->h[l] > s->h[big]) big = l;
        if (r < s->n && s->h[r] > s->h[big]) big = r;
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

static int dfa_cmp_u64(const void *pa, const void *pb)
{
    uint64_t a = *(const uint64_t *)pa, b = *(const uint64_t *)pb;
    return a < b ? -1 : (a > b ? 1 : 0);
}

/* Both sketches are sorted (they are finished being written to) and walked as
   one merge: each step consumes one member of the union in ascending order, so
   the first k steps ARE the k smallest hashes of the union, and `matches` counts. */
static double dfa_kmv_jaccard(dfa_kmv *a, dfa_kmv *b)
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
    if (taken == 0)             /* both empty: 0/0 is not 0 and not 1 */
        return NAN;
    return (double)matches / (double)taken;
}

/* Two empty sets give NaN, which is the same answer mean gives for an empty
   column: 0/0 is undefined, and claiming 1 ("identical") or 0 ("disjoint") would
   assert something never observed. One empty and one not is exactly 0. */
static JSValue dyn_df_approxSimilarity(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    DataFrame *df;
    DFBound ba, bb;
    const uint8_t *mask;
    dfa_kmv *sk;
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


/* ============================== additional aggregates ==================== */

enum { DFX_SEM, DFX_SKEW_SAMP, DFX_KURT_SAMP, DFX_COUNT_NULLS,
       DFX_REGR_COUNT, DFX_REGR_SXX, DFX_REGR_SYY, DFX_REGR_SXY };

/* Sample skew/kurtosis from the population forms dfm_moments already gives.
   Undefined below n=3 and n=4 respectively; 0 there rather than a division by a
   negative, matching what the population forms return on a degenerate column. */
static double dfx_skew_samp(const DFMoments *o)
{
    double n = o->n, g1;

    if (n < 3.0 || !(o->m2x > 0.0))
        return 0.0;
    g1 = (o->m3x / n) / pow(o->m2x / n, 1.5);
    return g1 * sqrt(n * (n - 1.0)) / (n - 2.0);
}

static double dfx_kurt_samp(const DFMoments *o)
{
    double n = o->n, g2;

    if (n < 4.0 || !(o->m2x > 0.0))
        return 0.0;
    g2 = (o->m4x / n) / ((o->m2x / n) * (o->m2x / n)) - 3.0;
    return ((n + 1.0) * g2 + 6.0) * (n - 1.0) / ((n - 2.0) * (n - 3.0));
}

/* One column: SEM, SKEW_SAMP, KURT_SAMP, COUNT_NULLS. */
static JSValue dyn_df_stat1(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv, int magic)
{
    static const char *const nm[4] = { "SEM", "SKEW_SAMP", "KURT_SAMP",
                                       "COUNT_NULLS" };
    DataFrame *df;
    DFBound b;
    DFMoments mo;
    const uint8_t *mask;
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
    case DFX_SKEW_SAMP: r = dfx_skew_samp(&mo); break;
    case DFX_KURT_SAMP: r = dfx_kurt_samp(&mo); break;
    default:
        /* standard error of the mean: the SAMPLE stddev over sqrt(n) */
        r = mo.n > 1.0 ? sqrt(mo.m2x / (mo.n - 1.0)) / sqrt(mo.n) : NAN;
        break;
    }
    return JS_NewFloat64(ctx, r);
}

/* Two columns: the raw regression sums the family already computes. */
static JSValue dyn_df_regr_sum(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv, int magic)
{
    static const char *const nm[4] = { "REGR_COUNT", "REGR_SXX", "REGR_SYY",
                                       "REGR_SXY" };
    DataFrame *df;
    DFBound bx, by;
    DFMoments mo;
    const uint8_t *mask;
    int ix, iy, ok;
    uint32_t n;
    double r;
    const char *op = nm[magic - DFX_REGR_COUNT];

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
    if (df_bind_numeric(ctx, df, ix, &bx, op) ||
        df_bind_numeric(ctx, df, iy, &by, op))
        return JS_EXCEPTION;

    n = bx.n < by.n ? bx.n : by.n;
    dfm_moments(bx.p, bx.type, by.p, by.type, mask, n, &mo);
    switch (magic) {
    /* REGR_*(y, x): the FIRST argument is the dependent variable, so the
       INDEPENDENT one is dfm_moments' y. SXX is therefore m2y, not m2x --
       swapping them still compiles and makes SXY/SXX stop being the slope. */
    case DFX_REGR_SXX: r = mo.m2y; break;
    case DFX_REGR_SYY: r = mo.m2x; break;
    case DFX_REGR_SXY: r = mo.cxy; break;
    default:           r = mo.n;   break;
    }
    return JS_NewFloat64(ctx, r);
}

/* MAD is the MEAN of |x - mean|; MEDIAN_ABSOLUTE_DEVIATION is the MEDIAN of
   |x - median|. Different centre, different fold, so they are two methods and
   not one with a flag -- conflating them is the usual bug. */
enum { DFX_MAD, DFX_MED_AD };

static JSValue dyn_df_deviation(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv, int magic)
{
    DataFrame *df;
    DFBound b;
    const uint8_t *mask;
    DfoItem *it = NULL;
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
            it[i].key = it[i].key == it[i].key ? fabs(it[i].key - centre) : NAN;
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

/* Shannon entropy in BITS over the empirical value distribution. Sorting makes
   the run lengths adjacent, so the frequencies need no hash table. */
static JSValue dyn_df_entropy(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    DataFrame *df;
    DfcSet set;
    const uint8_t *mask;
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

    /* The frequency table VALUE_COUNTS already builds is exactly the
       distribution this needs, and it costs a hash pass rather than a sort. */
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

/* QUANTILE_EXACT_LOW / _HIGH: the order statistic below or above the position,
   never interpolated, so both return a value present in the column. */
enum { DFX_Q_LOW, DFX_Q_HIGH };

static JSValue dyn_df_quantile_lh(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv, int magic)
{
    static const char *const nm[2] = { "QUANTILE_EXACT_LOW",
                                       "QUANTILE_EXACT_HIGH" };
    DataFrame *df;
    DFBound b;
    const uint8_t *mask;
    DfoItem *it = NULL;
    double q = 0.5, pos;
    uint32_t n, m, k;

    if (dfo_open(ctx, this_val, argc, argv, 1, nm[magic], &df, &b, &q, &mask))
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

/* QUANTILES(col, [q, ...][, mask]) -> Float64Array. One gather and ONE sort for
   the whole set: asking for five quantiles separately costs five partitions. */
static JSValue dyn_df_quantiles(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    DataFrame *df;
    DFBound b;
    const uint8_t *mask;
    DfoItem *it = NULL;
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
            double bad = qs[i];         /* copy BEFORE the free: the message reads it */
            JS_FreeValue(ctx, arr);
            free(qs);
            return JS_ThrowRangeError(ctx, "QUANTILES: q must be in [0, 1], "
                                      "got %g", bad);
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
    /* SELECT each position instead of sorting: k selects are O(k*n) against
       O(n log n). Ascending order narrows each successive select; `ord` carries
       the caller's order back so [0.9, 0.1] still answers [p90, p10]. */
    if (m) {
        uint32_t *ord = df_out_alloc(ctx, nq ? nq : 1, sizeof(uint32_t)), a, c;
        if (!ord) {
            free(qs);
            free(it);
            return JS_EXCEPTION;
        }
        for (a = 0; a < nq; a++)
            ord[a] = a;
        for (a = 1; a < nq; a++) {          /* insertion sort: nq is tiny */
            uint32_t key = ord[a];
            for (c = a; c > 0 && qs[ord[c - 1]] > qs[key]; c--)
                ord[c] = ord[c - 1];
            ord[c] = key;
        }
        for (a = 0; a < nq; a++) {
            double pos = qs[ord[a]] * (double)(m - 1);
            uint32_t lo = (uint32_t)pos;
            dfo_select(it, n, lo, 0);
            if (pos != (double)lo && lo + 1 < m)
                dfo_select(it + lo + 1, n - lo - 1, 0, 0);
        }
        free(ord);
    }
    out = df_out_alloc(ctx, nq, sizeof(double));
    if (!out) {
        free(qs);
        free(it);
        return JS_EXCEPTION;
    }
    for (i = 0; i < nq; i++) {
        if (m == 0) {
            out[i] = NAN;
        } else {
            double pos = qs[i] * (double)(m - 1), frac;
            uint32_t lo = (uint32_t)pos;
            frac = pos - (double)lo;
            /* the frac == 0 arm is load-bearing: 0 * Infinity is NaN. */
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

/* QUANTILES_TDIGEST(col, qs[, mask]): many APPROXIMATE quantiles off ONE digest.
   QUANTILES sorts and answers exactly; this builds a bounded digest once and
   reads each q from it, so the memory is fixed and the cost is one pass. */
static JSValue dyn_df_quantiles_td(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    DataFrame *df;
    DFBound b;
    const uint8_t *mask;
    dfa_tdigest *t = NULL;
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
    t = malloc(sizeof(*t));             /* ~37 KB: too large for a stack frame */
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
        out[i] = t->total > 0.0 ? dfa_td_quantile(t, qs[i]) : NAN;
    free(qs);
    free(t);
    return df_to_typed_array(ctx, out, (size_t)nq * sizeof(double),
                             JS_TYPED_ARRAY_FLOAT64);
}


/* UNIQ_UP_TO(col, n[, mask]) -> exact distinct count, or n+1 meaning "more than
   n". Bounded work: it stops the moment the threshold is passed. */
static JSValue dyn_df_uniq_up_to(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    DataFrame *df;
    DFBound b;
    const uint8_t *mask;
    DfoItem *it = NULL;
    double cap = 0.0;
    uint32_t n, m, i, seen = 0, lim;

    if (dfo_open(ctx, this_val, argc, argv, 1, "UNIQ_UP_TO", &df, &b, &cap,
                 &mask))
        return JS_EXCEPTION;
    if (!(cap >= 0.0) || cap != floor(cap) || cap > 65536.0)
        return JS_ThrowRangeError(ctx, "UNIQ_UP_TO(col, n): n must be an "
                                  "integer in [0, 65536], got %g", cap);
    lim = (uint32_t)cap;
    /* BOUNDED, which is the whole promise: a linear probe over at most lim+1
       distinct values, abandoned the moment the cap is passed. Sorting first
       would cost O(n log n) on a column whose answer is "more than ten". */
    {
        uint32_t nslot = 1, probe;
        double *slot;
        uint8_t *used;
        uint32_t span = b.n < df->nrows ? b.n : df->nrows;
        while (nslot < (lim + 1) * 2u) nslot <<= 1;
        slot = malloc((size_t)nslot * sizeof(*slot));
        used = calloc(nslot, 1);
        if (!slot || !used) {
            free(slot); free(used);
            return JS_ThrowOutOfMemory(ctx);
        }
        int saw_nan = 0;
        for (i = 0; i < span && seen <= lim; i++) {
            double v;
            uint64_t bits;
            if (mask && !mask[i])
                continue;
            v = df_get(b.p, b.type, i);
            /* SameValueZero, matching N_UNIQUE: NaN is ONE distinct value, not
               a skipped row, and -0 collapses into +0. */
            if (v != v) {
                if (!saw_nan) { saw_nan = 1; seen++; }
                continue;
            }
            if (v == 0.0) v = 0.0;
            memcpy(&bits, &v, sizeof(bits));
            bits ^= bits >> 33; bits *= 0xff51afd7ed558ccdULL; bits ^= bits >> 33;
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
    (void)it; (void)n; (void)m;
    return JS_NewInt64(ctx, (int64_t)seen);
}

/* HISTOGRAM(col, bins[, mask]) -> { edges, counts }. Equal-width bins over the
   observed range; the top edge is inclusive so the maximum is not lost. */
static JSValue dyn_df_histogram(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv, int magic)
{
    DataFrame *df;
    DFBound b;
    const uint8_t *mask;
    double nb = 0.0, lo = INFINITY, hi = -INFINITY, w;
    double *edges = NULL, *counts = NULL;
    uint32_t nbins, i, span, kept = 0;
    JSValue res, ev, cv;

    if (dfo_open(ctx, this_val, argc, argv, 1,
                 magic ? "HISTOGRAM_NORMALIZED" : "HISTOGRAM",
                 &df, &b, &nb, &mask))
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
        if (v != v)
            continue;
        kept++;
        if (v < lo) lo = v;
        if (v > hi) hi = v;
    }
    edges  = df_out_alloc(ctx, nbins + 1, sizeof(double));
    counts = edges ? df_out_alloc(ctx, nbins, sizeof(double)) : NULL;
    if (!counts) {
        free(edges);
        return JS_EXCEPTION;
    }
    for (i = 0; i < nbins; i++)
        counts[i] = 0.0;
    /* a degenerate range would divide by zero; widen it so every value lands in
       bin 0 rather than producing NaN edges. */
    if (kept == 0) { lo = 0.0; hi = 1.0; }
    else if (hi <= lo) { hi = lo + 1.0; }
    w = (hi - lo) / (double)nbins;
    for (i = 0; i <= nbins; i++)
        edges[i] = lo + w * (double)i;

    for (i = 0; i < span; i++) {
        double v;
        uint32_t k;
        if (mask && !mask[i])
            continue;
        v = df_get(b.p, b.type, i);
        if (v != v)
            continue;
        k = (uint32_t)((v - lo) / w);
        if (k >= nbins)
            k = nbins - 1;          /* the top edge is inclusive */
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
    if (JS_IsException(ev) || JS_IsException(cv) ||
        JS_DefinePropertyValueStr(ctx, res, "edges", ev, JS_PROP_C_W_E) < 0 ||
        JS_DefinePropertyValueStr(ctx, res, "counts", cv, JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, res);
        return JS_EXCEPTION;
    }
    return res;
}

/* EMA(col, alpha[, mask]) -> Float64Array, always `rows` long. A masked-out row
   carries the running value forward, matching the cumulative scans. */
static JSValue dyn_df_ema(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv)
{
    DataFrame *df;
    DFBound b;
    const uint8_t *mask;
    double alpha = 0.0, acc = 0.0, *dst;
    uint32_t i, n, span;
    int seeded = 0;

    if (dfo_open(ctx, this_val, argc, argv, 1, "EMA", &df, &b, &alpha, &mask))
        return JS_EXCEPTION;
    if (!(alpha > 0.0) || !(alpha <= 1.0) || isnan(alpha))
        return JS_ThrowRangeError(ctx, "EMA(col, alpha): alpha must be in "
                                  "(0, 1], got %g", alpha);
    n = df->nrows;
    dst = df_out_alloc(ctx, n, sizeof(double));
    if (!dst)
        return JS_EXCEPTION;
    span = df_map_span(n, b.n, dst);
    for (i = 0; i < span; i++) {
        double v;
        if (mask && !mask[i]) {
            dst[i] = seeded ? acc : NAN;
            continue;
        }
        v = df_get(b.p, b.type, i);
        if (!seeded) { acc = v; seeded = 1; }
        else acc = alpha * v + (1.0 - alpha) * acc;
        dst[i] = acc;
    }
    return df_to_typed_array(ctx, dst, (size_t)n * sizeof(double),
                             JS_TYPED_ARRAY_FLOAT64);
}

/* DELTA_SUM(col[, mask]) -> the sum of POSITIVE consecutive differences, the
   monotonic-increase total a counter accumulates across resets. */
static JSValue dyn_df_delta_sum(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    DataFrame *df;
    DFBound b;
    const uint8_t *mask;
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

/* RATE(valueCol, timeCol[, mask]) -> change per unit time across the whole
   selection. IRATE uses only the LAST two selected rows, so it reacts to the
   most recent interval rather than averaging over the window. */
enum { DFX_RATE, DFX_IRATE };

static JSValue dyn_df_rate(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv, int magic)
{
    const char *op = magic == DFX_IRATE ? "IRATE" : "RATE";
    DataFrame *df;
    DFBound bv, bt;
    const uint8_t *mask;
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
    if (df_bind_numeric(ctx, df, iv, &bv, op) ||
        df_bind_numeric(ctx, df, it_, &bt, op))
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
        if (!have) { v0 = v; t0 = t; have = 1; }
        pv = v1; pt = t1;
        v1 = v; t1 = t;
        have++;
    }
    if (have < 3)                       /* fewer than two selected rows */
        return JS_NewFloat64(ctx, NAN);
    if (magic == DFX_IRATE)
        return JS_NewFloat64(ctx, (v1 - pv) / (t1 - pt));
    return JS_NewFloat64(ctx, (v1 - v0) / (t1 - t0));
}

/* ---- grouped bitwise, sorted collection, weighted top-k, correlation matrix ---- */

enum { DFY_BIT_AND, DFY_BIT_OR, DFY_BIT_XOR };

/* GROUP_BIT_AND/OR/XOR(keyCol, valueCol[, mask]) -> { keys, values }. Integer
   value columns only, folded in uint32 exactly as the scalar BITWISE_* do, so
   the empty-group identity is the same one those publish. */
static JSValue dyn_df_group_bit(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv, int magic)
{
    static const char *const nm[3] = { "GROUP_BIT_AND", "GROUP_BIT_OR",
                                       "GROUP_BIT_XOR" };
    DataFrame *df;
    DFBound kb, vb;
    const uint8_t *mask;
    uint32_t *acc = NULL, *cnt = NULL, *gk = NULL;
    double *out = NULL;
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
                                 "defined only on integer columns", nm[magic],
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
        if (magic == DFY_BIT_AND)      acc[g] &= v;
        else if (magic == DFY_BIT_OR)  acc[g] |= v;
        else                           acc[g] ^= v;
        cnt[g]++;
    }
    free(gk);

    out = df_out_alloc(ctx, nkeys, sizeof(double));
    if (!out) {
        free(acc);
        free(cnt);
        return JS_EXCEPTION;
    }
    /* An untouched group keeps the identity, matching the scalar reductions on
       an empty column: ~0 for AND, 0 for OR and XOR. */
    for (i = 0; i < nkeys; i++)
        out[i] = (double)(int32_t)acc[i];
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
        if (JS_IsException(k) ||
            JS_DefinePropertyValueUint32(ctx, keys, i, k, JS_PROP_C_W_E) < 0) {
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

/* CORR_MATRIX / COV_MATRIX([col, ...][, mask]) -> { columns, matrix, n }. The
   matrix is n*n row-major; every pair goes through the same dfm_moments CORR
   and COV_SAMP use, so a cell cannot disagree with the pairwise call. */
static JSValue dyn_df_corr_matrix(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv, int magic)
{
    DataFrame *df;
    const char *op = magic ? "COV_MATRIX" : "CORR_MATRIX";
    const uint8_t *mask;
    JSValue arr = JS_UNDEFINED, lenv, cols = JS_UNDEFINED, mv, res;
    int *idx = NULL, ok;
    uint32_t *first = NULL;
    double *m = NULL;
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
                                 "of column names", op);
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
    /* resolve every name BEFORE binding anything: each one can run user JS */
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
    /* First position naming each column. A name repeated 1024 times over a
       2-column frame is 524288 computations of the SAME pair -- measured 128 s.
       Duplicates copy from their first occurrence instead. */
    for (i = 0; i < nc; i++) {
        first[i] = i;
        for (j = 0; j < i; j++)
            if (idx[j] == idx[i]) {
                first[i] = j;
                break;
            }
    }

    /* Both loops start ON the diagonal. CORR's used to be a literal 1.0, which
       disagreed with CORR(c,c) = NaN for a zero-variance column -- the one cell
       that never went through the moments the rest are computed from. */
    for (i = 0; i < nc; i++) {
        for (j = i; j < nc; j++) {
            /* first[x] <= x, so the source cell is always already written. */
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
            if (df_bind_numeric(ctx, df, idx[i], &bx, op) ||
                df_bind_numeric(ctx, df, idx[j], &by, op)) {
                JS_FreeValue(ctx, arr);
                free(idx);
                free(m);
                free(first);
                return JS_EXCEPTION;
            }
            n = bx.n < by.n ? bx.n : by.n;
            dfm_moments(bx.p, bx.type, by.p, by.type, mask, n, &mo);
            /* dfm_corr, not a local formula: sqrt(a*b) and sqrt(a)*sqrt(b) round
               differently, so an inline copy makes a cell disagree with CORR by
               a ULP -- which the doc promises it cannot. */
            /* On the diagonal dfm_corr computes S/(sqrt(S)*sqrt(S)), which is
               not exactly 1 for every S — so pin 1.0 where a variance exists
               and let the NaN through where it does not. */
            r = magic ? (mo.n >= 2.0 ? mo.cxy / (mo.n - 1.0) : NAN)
              : (i == j) ? (mo.m2x > 0.0 ? 1.0 : NAN)
              : dfm_corr(&mo);
            m[i * nc + j] = r;
            m[j * nc + i] = r;      /* symmetric by construction, not by luck */
        }
    }

    cols = JS_NewArray(ctx);
    for (i = 0; i < nc && !JS_IsException(cols); i++) {
        JSValue s = JS_NewString(ctx, df->cols[idx[i]].name);
        if (JS_IsException(s) ||
            JS_DefinePropertyValueUint32(ctx, cols, i, s, JS_PROP_C_W_E) < 0) {
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
    if (JS_IsException(cols) || JS_IsException(mv) || JS_IsException(res) ||
        JS_DefinePropertyValueStr(ctx, res, "columns", cols, JS_PROP_C_W_E) < 0 ||
        JS_DefinePropertyValueStr(ctx, res, "matrix", mv, JS_PROP_C_W_E) < 0 ||
        JS_DefinePropertyValueStr(ctx, res, "n", JS_NewInt64(ctx, nc),
                                  JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, res);
        return JS_EXCEPTION;
    }
    return res;
}

/* ------------------------------------------- dispersion, rank, change (G8) */

enum { DFX_ROLL_VAR, DFX_ROLL_STD };

/* ROLLING_VAR / ROLLING_STD(col, w[, mask]) -> Float64Array. Sample (ddof=1),
   as pandas rolling().var(). Two passes per window, never a subtractive sum of
   squares: that cancels catastrophically when the mean is far from zero. */
static JSValue dyn_df_rolling_disp(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv, int magic)
{
    DataFrame *df;
    DFBound b;
    const uint8_t *mask;
    const char *op = magic == DFX_ROLL_STD ? "ROLLING_STD" : "ROLLING_VAR";
    double wd, *dst;
    uint32_t i, n, span, w;

    if (dfo_open(ctx, this_val, argc, argv, 1, op, &df, &b, &wd, &mask))
        return JS_EXCEPTION;
    if (!(wd >= 1) || wd != floor(wd) || wd > (double)UINT32_MAX)
        return JS_ThrowRangeError(ctx, "%s: window must be a positive integer, "
                                  "got %g", op, wd);
    w = (uint32_t)wd;
    n = df->nrows;
    dst = df_out_alloc(ctx, n, sizeof(double));
    if (!dst)
        return JS_EXCEPTION;
    span = df_map_span(n, b.n, dst);

    /* NaN PROPAGATES, as VARIANCE and ROLLING_MEAN do -- variance is in the sum
       family, not the min/max family that ignores it. A window with fewer than
       two selected rows has no sample variance and yields NaN. */
    for (i = 0; i < span; i++) {
        double mean = 0.0, m2 = 0.0;
        uint32_t c = 0, j, lo;
        if (i + 1 < w) {
            dst[i] = NAN;
            continue;
        }
        lo = i + 1 - w;
        for (j = lo; j <= i; j++) {
            if (mask && !mask[j])
                continue;
            mean += df_get(b.p, b.type, j);
            c++;
        }
        if (c < 2) {
            dst[i] = NAN;
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
    return df_to_typed_array(ctx, dst, (size_t)n * sizeof(double),
                             JS_TYPED_ARRAY_FLOAT64);
}

/* PCT_CHANGE(col[, periods][, mask]) -> Float64Array of (x[i]-x[i-p])/x[i-p].
   DIFF gives the absolute delta; this is the relative one, and it is NOT
   DIFF/SHIFT composed -- that pays two output buffers and two passes. */
static JSValue dyn_df_pct_change(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    DataFrame *df;
    DFBound b;
    const uint8_t *mask;
    double pd = 1.0, *dst;
    uint32_t i, n, span, p;

    if (dfo_open(ctx, this_val, argc, argv, 1, "PCT_CHANGE", &df, &b, &pd, &mask))
        return JS_EXCEPTION;
    if (argc < 2 || JS_IsUndefined(argv[1]))
        pd = 1.0;
    if (!(pd >= 1) || pd != floor(pd) || pd > (double)UINT32_MAX)
        return JS_ThrowRangeError(ctx, "PCT_CHANGE: periods must be a positive "
                                  "integer, got %g", pd);
    p = (uint32_t)pd;
    n = df->nrows;
    dst = df_out_alloc(ctx, n, sizeof(double));
    if (!dst)
        return JS_EXCEPTION;
    span = df_map_span(n, b.n, dst);

    for (i = 0; i < span; i++) {
        double prev, cur;
        if (i < p || (mask && (!mask[i] || !mask[i - p]))) {
            dst[i] = NAN;
            continue;
        }
        prev = df_get(b.p, b.type, i - p);
        cur = df_get(b.p, b.type, i);
        /* prev == 0 gives +/-Inf, which is the honest answer for a relative
           change from nothing; 0/0 is NaN and stays NaN. */
        dst[i] = (cur - prev) / prev;
    }
    return df_to_typed_array(ctx, dst, (size_t)n * sizeof(double),
                             JS_TYPED_ARRAY_FLOAT64);
}

/* ZSCORE(col[, mask]) -> Float64Array of (x - mean) / sample stddev. Sample,
   not population, so it composes with STDDEV rather than STDDEV_POP.
   Unselected and NaN rows are NaN; a constant column is all NaN, not 0. */
static JSValue dyn_df_zscore(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    DataFrame *df;
    DFBound b;
    DFMoments mo;
    const uint8_t *mask;
    double sd, *dst;
    uint32_t i, n, span;

    if (dfo_open(ctx, this_val, argc, argv, 0, "ZSCORE", &df, &b, NULL, &mask))
        return JS_EXCEPTION;
    n = df->nrows;
    dst = df_out_alloc(ctx, n, sizeof(double));
    if (!dst)
        return JS_EXCEPTION;
    span = df_map_span(n, b.n, dst);
    dfm_moments(b.p, b.type, NULL, DF_F64, mask, b.n, &mo);
    sd = mo.n >= 2.0 ? sqrt(mo.m2x / (mo.n - 1.0)) : NAN;

    for (i = 0; i < span; i++) {
        if (mask && !mask[i]) {
            dst[i] = NAN;
            continue;
        }
        dst[i] = (df_get(b.p, b.type, i) - mo.mx) / sd;
    }
    return df_to_typed_array(ctx, dst, (size_t)n * sizeof(double),
                             JS_TYPED_ARRAY_FLOAT64);
}

enum { DFX_DENSE_RANK, DFX_PERCENT_RANK };

/* DENSE_RANK counts DISTINCT values (no gap after a tie); PERCENT_RANK uses the
   MINIMUM rank, normalised. RANK averages instead -- the only rule whose column
   sum is invariant. Verified against SQLite's window functions. NaN stays NaN. */
static JSValue dyn_df_rank_ext(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv, int magic)
{
    DataFrame *df;
    DFBound b;
    const uint8_t *mask;
    DfoItem *it;
    double *dst;
    uint32_t nrows, n, m, i, j, dense = 0;
    const char *op = magic == DFX_PERCENT_RANK ? "PERCENT_RANK" : "DENSE_RANK";

    if (dfo_open(ctx, this_val, argc, argv, 0, op, &df, &b, NULL, &mask))
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
        dst[i] = NAN;

    m = dfo_valued(it, n);
    i = 0;
    while (i < m) {
        double r;
        for (j = i + 1; j < m && it[j].key == it[i].key; j++)
            ;
        dense++;
        /* PERCENT_RANK is (minrank-1)/(m-1); a single valued row has no spread,
           so SQL defines it as 0 rather than dividing by zero. */
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

/* NTILE(col, buckets[, mask]) -> Float64Array of 1..buckets. SQL's rule: the
   first m%buckets tiles take one extra row, so sizes differ by at most one.
   Ties are NOT kept together -- that is SQL NTILE, not a quantile bucket. */
static JSValue dyn_df_ntile(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    DataFrame *df;
    DFBound b;
    const uint8_t *mask;
    DfoItem *it;
    double kd, *dst;
    uint32_t nrows, n, m, i, k, big, small, cut;

    if (dfo_open(ctx, this_val, argc, argv, 1, "NTILE", &df, &b, &kd, &mask))
        return JS_EXCEPTION;
    if (!(kd >= 1) || kd != floor(kd) || kd > (double)UINT32_MAX)
        return JS_ThrowRangeError(ctx, "NTILE: buckets must be a positive "
                                  "integer, got %g", kd);
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
        dst[i] = NAN;

    m = dfo_valued(it, n);
    big = m % k;                        /* tiles that take one extra row */
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

/* RANK_CORR(x, y[, mask]) -> Number. Spearman: Pearson over the AVERAGE ranks,
   which is the definition that stays correct with ties -- the 1-6d^2/n(n^2-1)
   shortcut is only equal to it when there are none. */
static JSValue dyn_df_rank_corr(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    DataFrame *df;
    DFBound bx, by;
    DFMoments mo;
    const uint8_t *mask;
    uint8_t *both = NULL;
    DfoItem *it = NULL;
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
    /* no JS may run from here on. */
    if (df_bind_numeric(ctx, df, ix, &bx, "RANK_CORR") ||
        df_bind_numeric(ctx, df, iy, &by, "RANK_CORR"))
        return JS_EXCEPTION;

    nrows = df->nrows;
    span = bx.n < by.n ? bx.n : by.n;
    both = df_out_alloc(ctx, nrows, sizeof(uint8_t));
    rx = df_out_alloc(ctx, nrows, sizeof(double));
    ry = df_out_alloc(ctx, nrows, sizeof(double));
    if (!both || !rx || !ry)
        goto oom;
    /* PAIRWISE selection: a row missing in either column is out of BOTH
       rankings, or the two rank vectors would not be over the same rows. */
    for (i = 0; i < span; i++) {
        double a = df_get(bx.p, bx.type, i), c = df_get(by.p, by.type, i);
        both[i] = (!mask || mask[i]) && a == a && c == c;
    }

    for (pass = 0; pass < 2; pass++) {
        const DFBound *bp = pass ? &by : &bx;
        double *out = pass ? ry : rx;
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

    /* span, not nrows: both[] is only filled over span, and df_out_alloc does
       not zero. They are equal today (the constructor refuses ragged columns),
       so this keeps the guard above consistent rather than half-applied. */
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

/* ---- string fold, sorted and sampled collection, weighted order statistics ---- */

/* GROUP_CONCAT(col[, sep][, mask]) -> String. Values are joined in row order;
   a string column joins its dictionary strings, a numeric one its numbers. */
static JSValue dyn_df_group_concat(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    DataFrame *df;
    DFBound b;
    const uint8_t *mask;
    const char *sep = NULL;
    char *buf = NULL;
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
        if (sep) JS_FreeCString(ctx, sep);
        return JS_EXCEPTION;
    }
    seplen = sep ? strlen(sep) : 1;

    span = b.n < df->nrows ? b.n : df->nrows;
    for (i = 0; i < span; i++) {
        char num[40];
        const char *piece;
        size_t plen, need;
        if (mask && !mask[i])
            continue;
        if (b.type == DF_STR) {
            int32_t code = ((const int32_t *)b.p)[i];
            piece = (code >= 0 && (uint32_t)code < df->cols[idx].dict_len)
                  ? df->cols[idx].dict[code] : "";
            plen = strlen(piece);
        } else {
            double d = df_get(b.p, b.type, i);
            /* snprintf dominates this loop, and a column of whole numbers is the
               common case, so integers get a digit loop and only the rest pay. */
            if (d == floor(d) && d > -1e15 && d < 1e15) {
                int64_t iv = (int64_t)d;
                char tmp[24];
                int tn = 0, neg = iv < 0;
                uint64_t u = neg ? (uint64_t)(-iv) : (uint64_t)iv;
                do { tmp[tn++] = (char)('0' + (u % 10)); u /= 10; } while (u);
                plen = 0;
                if (neg) num[plen++] = '-';
                while (tn) num[plen++] = tmp[--tn];
                piece = num;
            } else {
                int w = snprintf(num, sizeof(num), "%.17g", d);
                plen = w < 0 ? 0 : (size_t)w >= sizeof(num) ? sizeof(num)-1 : (size_t)w;
                piece = num;
            }
        }
        need = len + plen + (first ? 0 : seplen);
        if (need > cap) {
            char *nb;
            size_t ncap = cap ? cap * 2 : 256;
            while (ncap < need) ncap *= 2;
            nb = realloc(buf, ncap);
            if (!nb) {
                free(buf);
                if (sep) JS_FreeCString(ctx, sep);
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

/* Ascending doubles, NaN last -- the order the rest of this file uses. */
static int dfz_cmp_d(const void *pa, const void *pb)
{
    double a = *(const double *)pa, b = *(const double *)pb;

    if (a != a) return b != b ? 0 : 1;
    if (b != b) return -1;
    return a < b ? -1 : (a > b ? 1 : 0);
}

enum { DFZ_SORTED, DFZ_LAST, DFZ_SAMPLE };

/* GROUP_ARRAY_SORTED / _LAST / _SAMPLE(keyCol, valueCol, k[, mask]).
   SORTED sorts each group ascending; LAST keeps the last k rows seen; SAMPLE
   takes a deterministic every-nth stride, so a rerun gives the same rows. */
static JSValue dyn_df_group_array_v(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv, int magic)
{
    static const char *const nm[3] = { "GROUP_ARRAY_SORTED",
                                       "GROUP_ARRAY_LAST",
                                       "GROUP_ARRAY_SAMPLE" };
    DataFrame *df;
    DFBound kb, vb;
    const uint8_t *mask;
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
    mask = df_mask_arg(ctx, argc > (magic == DFZ_SORTED ? 2 : 3)
                       ? argv[magic == DFZ_SORTED ? 2 : 3] : JS_UNDEFINED,
                       df->nrows, &ok);
    if (!ok)
        return JS_EXCEPTION;
    if (dfc_group_count(ctx, df, ki, &nkeys, &ngroups))
        return JS_EXCEPTION;
    if (dyn_df_bind(ctx, df, ki, &kb) || dyn_df_bind(ctx, df, vi, &vb))
        return JS_EXCEPTION;
    if (vb.type == DF_STR)
        return JS_ThrowTypeError(ctx, "%s: the value column is %s; only numeric "
                                 "columns can be collected", nm[magic],
                                 df_type_name(vb.type));
    cap = magic == DFZ_SORTED ? 0xffffffffu : (uint32_t)kd;

    cnt = calloc(ngroups, sizeof(*cnt));
    off = calloc(ngroups + 1, sizeof(*off));
    fill = calloc(ngroups, sizeof(*fill));
    head = calloc(ngroups, sizeof(*head));
    if (!cnt || !off || !fill || !head) {
        free(cnt); free(off); free(fill); free(head);
        return JS_ThrowOutOfMemory(ctx);
    }
    n = kb.n < vb.n ? kb.n : vb.n;
    gk = df_keys_u32(&kb, n);

    /* pass 1: how many rows each group keeps, capped */
    for (i = 0; i < n; i++) {
        if (mask && !mask[i]) continue;
        g = gk ? gk[i] : (uint32_t)df_get(kb.p, kb.type, i);
        if (g >= ngroups) continue;
        cnt[g]++;
    }
    for (i = 0; i < ngroups; i++) {
        if (cnt[i] > cap) cnt[i] = cap;
        off[i + 1] = off[i] + cnt[i];
    }
    total = off[ngroups];
    flat = df_out_alloc(ctx, total ? total : 1, sizeof(double));
    if (!flat) {
        free(cnt); free(off); free(fill); free(head); free(gk);
        return JS_EXCEPTION;
    }

    /* pass 2: place. LAST keeps a rolling window so the final k survive; SAMPLE
       strides deterministically; SORTED takes everything and orders it after. */
    for (i = 0; i < n; i++) {
        double v;
        uint32_t base, have;
        if (mask && !mask[i]) continue;
        g = gk ? gk[i] : (uint32_t)df_get(kb.p, kb.type, i);
        if (g >= ngroups) continue;
        v = df_get(vb.p, vb.type, i);
        base = off[g];
        have = fill[g];
        if (magic == DFZ_LAST && have == cnt[g] && cnt[g]) {
            /* Circular window: the oldest slot is overwritten and the head
               advances -- a memmove of the whole window per row was O(n*k). */
            flat[base + head[g]] = v;
            head[g] = (head[g] + 1) % cnt[g];
            continue;
        }
        if (have < cnt[g]) {
            flat[base + have] = v;
            fill[g] = have + 1;
        }
    }
    free(gk);
    /* qsort per group, not insertion sort: one big group makes the latter
       quadratic, and a thousand rows in a group is ordinary. */
    if (magic == DFZ_SORTED)
        for (g = 0; g < nkeys; g++)
            if (cnt[g] > 1)
                qsort(&flat[off[g]], cnt[g], sizeof(double), dfz_cmp_d);

    values = JS_NewArray(ctx);
    if (JS_IsException(values)) {
        free(cnt); free(off); free(fill); free(head); free(flat);
        return JS_EXCEPTION;
    }
    for (g = 0; g < nkeys; g++) {
        double *chunk = df_out_alloc(ctx, cnt[g] ? cnt[g] : 1, sizeof(double));
        JSValue ta;
        if (!chunk) {
            JS_FreeValue(ctx, values);
            free(cnt); free(off); free(fill); free(head); free(flat);
            return JS_EXCEPTION;
        }
        if (magic == DFZ_LAST && head[g]) {
            /* unwrap the circular window into arrival order */
            uint32_t j;
            for (j = 0; j < cnt[g]; j++)
                chunk[j] = flat[off[g] + (head[g] + j) % cnt[g]];
        } else {
            memcpy(chunk, &flat[off[g]], (size_t)cnt[g] * sizeof(double));
        }
        ta = df_to_typed_array(ctx, chunk, (size_t)cnt[g] * sizeof(double),
                               JS_TYPED_ARRAY_FLOAT64);
        if (JS_IsException(ta) ||
            JS_DefinePropertyValueUint32(ctx, values, g, ta, JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, values);
            free(cnt); free(off); free(fill); free(head); free(flat);
            return JS_EXCEPTION;
        }
    }
    free(cnt); free(off); free(fill); free(head); free(flat);

    keys = JS_NewArray(ctx);
    if (JS_IsException(keys)) {
        JS_FreeValue(ctx, values);
        return JS_EXCEPTION;
    }
    for (g = 0; g < nkeys; g++) {
        JSValue k = (df->cols[ki].type == DF_STR)
                    ? JS_NewString(ctx, df->cols[ki].dict[g])
                    : JS_NewInt64(ctx, g);
        if (JS_IsException(k) ||
            JS_DefinePropertyValueUint32(ctx, keys, g, k, JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, keys);
            JS_FreeValue(ctx, values);
            return JS_EXCEPTION;
        }
    }
    return dfc_pair(ctx, keys, values);
}

/* ---- weighted frequency, heavy hitter, weighted quantile, group intersection ---- */

typedef struct { double key, w; } DfwPair;

static int dfw_cmp_w(const void *pa, const void *pb)
{
    const DfwPair *a = pa, *b = pb;

    if (a->w != b->w)
        return a->w > b->w ? -1 : 1;      /* descending by weight */
    return a->key < b->key ? -1 : (a->key > b->key ? 1 : 0);
}

/* Ascending by key with an INLINE comparison, the same shape dfo_sort uses:
   qsort costs an indirect call per comparison, which dominates at a million
   rows. NaN never reaches here -- dfw_collect drops it. */
static void dfw_sort_k(DfwPair *p, uint32_t n)
{
    while (n > 24) {
        DfwPair pv;
        uint32_t i = 0, j = n - 1, m = n >> 1;
        if (p[m].key < p[0].key)     { DfwPair t = p[m]; p[m] = p[0]; p[0] = t; }
        if (p[n-1].key < p[m].key)   { DfwPair t = p[n-1]; p[n-1] = p[m]; p[m] = t; }
        if (p[m].key < p[0].key)     { DfwPair t = p[m]; p[m] = p[0]; p[0] = t; }
        pv = p[m];
        for (;;) {
            while (p[i].key < pv.key) i++;
            while (pv.key < p[j].key) j--;
            if (i >= j) break;
            { DfwPair t = p[i]; p[i] = p[j]; p[j] = t; }
            i++;
            if (j == 0) break;
            j--;
        }
        dfw_sort_k(p, j + 1);
        p += j + 1;
        n -= j + 1;
    }
    {
        uint32_t a, b;
        for (a = 1; a < n; a++) {
            DfwPair v = p[a];
            for (b = a; b > 0 && v.key < p[b-1].key; b--)
                p[b] = p[b-1];
            p[b] = v;
        }
    }
}

/* DELTA_SUM_TIMESTAMP(valueCol, timeCol[, mask]): DELTA_SUM in TIMESTAMP order,
   not row order. Rows are ordered by the time column first, so out-of-order
   input still sums the positive value-deltas along the timeline. */
static JSValue dyn_df_delta_sum_ts(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    DataFrame *df;
    DFBound bv, bt;
    const uint8_t *mask;
    DfwPair *p = NULL;
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
    if (df_bind_numeric(ctx, df, iv, &bv, "DELTA_SUM_TIMESTAMP") ||
        df_bind_numeric(ctx, df, it_, &bt, "DELTA_SUM_TIMESTAMP"))
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
        if (tv != tv)                   /* a row with no time has no place */
            continue;
        p[n].key = tv;
        p[n].w = df_get(bv.p, bv.type, i);
        n++;
    }
    dfw_sort_k(p, n);               /* by timestamp; NaN already dropped */
    for (i = 1; i < n; i++)
        if (p[i].w > p[i - 1].w)
            s += p[i].w - p[i - 1].w;
    free(p);
    return JS_NewFloat64(ctx, s);
}

/* Selected rows collapsed to (value, summed weight), one entry per distinct
   value, sorted by value. A NULL weight column weighs every row one, which is
   plain frequency. NaN is dropped: it is not a value to rank. */
static DfwPair *dfw_collect(JSContext *ctx, const DFBound *b, const DFBound *w,
                            const uint8_t *mask, uint32_t nrows, uint32_t *pn)
{
    uint32_t span = b->n < nrows ? b->n : nrows, i, j, n = 0, o = 0;
    DfwPair *p;

    if (w && w->n < span)
        span = w->n;
    p = df_out_alloc(ctx, span ? span : 1, sizeof(*p));
    if (!p)
        return NULL;
    for (i = 0; i < span; i++) {
        double v;
        if (mask && !mask[i])
            continue;
        v = df_get(b->p, b->type, i);
        if (v != v)
            continue;
        p[n].key = v;
        p[n].w = w ? df_get(w->p, w->type, i) : 1.0;
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

enum { DFW_TOPK_W, DFW_APPROX_TOP_SUM, DFW_ANY_HEAVY };

/* TOP_K_WEIGHTED and APPROX_TOP_SUM rank by SUMMED WEIGHT, not count; the
   second is the same answer under the name the sketch API uses. ANY_HEAVY
   returns the value holding strictly more than half the weight, or undefined. */
static JSValue dyn_df_weighted_top(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv, int magic)
{
    static const char *const nm[3] = { "TOP_K_WEIGHTED", "APPROX_TOP_SUM",
                                       "ANY_HEAVY" };
    DataFrame *df;
    DFBound b, wb;
    const uint8_t *mask;
    DfwPair *p = NULL;
    double kd = 1.0, total = 0.0, *out;
    uint32_t n = 0, i, k, nout;
    int idx, widx = -1, ok, wants_k = (magic != DFW_ANY_HEAVY);
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
    }
    if (wants_k) {
        int ka = widx >= 0 ? 2 : 1;
        if (JS_ToFloat64(ctx, &kd, argc > ka ? argv[ka] : JS_UNDEFINED))
            return JS_EXCEPTION;
        if (!(kd >= 1.0) || kd != floor(kd) || kd > 65536.0)
            return JS_ThrowRangeError(ctx, "%s: k must be an integer in "
                                      "[1, 65536], got %g", nm[magic], kd);
    }
    {
        int ma = (widx >= 0 ? 1 : 0) + (wants_k ? 1 : 0) + 1;
        mask = df_mask_arg(ctx, argc > ma ? argv[ma] : JS_UNDEFINED,
                           df->nrows, &ok);
        if (!ok)
            return JS_EXCEPTION;
    }
    if (df_bind_numeric(ctx, df, idx, &b, nm[magic]))
        return JS_EXCEPTION;
    if (widx >= 0 && df_bind_numeric(ctx, df, widx, &wb, nm[magic]))
        return JS_EXCEPTION;

    p = dfw_collect(ctx, &b, widx >= 0 ? &wb : NULL, mask, df->nrows, &n);
    if (!p)
        return JS_EXCEPTION;
    for (i = 0; i < n; i++)
        total += p[i].w;
    qsort(p, n, sizeof(*p), dfw_cmp_w);

    if (magic == DFW_ANY_HEAVY) {
        JSValue r = (n && p[0].w * 2.0 > total)
                  ? JS_NewFloat64(ctx, p[0].key) : JS_UNDEFINED;
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
        if (JS_IsException(kv) ||
            JS_DefinePropertyValueUint32(ctx, keys, i, kv, JS_PROP_C_W_E) < 0) {
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

/* QUANTILE_EXACT_WEIGHTED(col, weightCol, q[, mask]): the value at which the
   cumulative weight first reaches q of the total, so a row weighing three
   counts as three rows rather than one. */
static JSValue dyn_df_quantile_weighted(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv)
{
    DataFrame *df;
    DFBound b, wb;
    const uint8_t *mask;
    DfwPair *p = NULL;
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
                                  "must be in [0, 1], got %g", q);
    mask = df_mask_arg(ctx, argc > 3 ? argv[3] : JS_UNDEFINED, df->nrows, &ok);
    if (!ok)
        return JS_EXCEPTION;
    if (df_bind_numeric(ctx, df, idx, &b, "QUANTILE_EXACT_WEIGHTED") ||
        df_bind_numeric(ctx, df, widx, &wb, "QUANTILE_EXACT_WEIGHTED"))
        return JS_EXCEPTION;

    p = dfw_collect(ctx, &b, &wb, mask, df->nrows, &n);
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
        return JS_UNDEFINED;            /* no weight: no weighted quantile */
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

/* GROUP_ARRAY_INTERSECT(keyCol, valueCol[, mask]): the values present in EVERY
   group. A repeat inside one group must not advance the counter, so the last
   group seen is remembered per value. */
static JSValue dyn_df_group_intersect(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    DataFrame *df;
    DFBound kb, vb;
    const uint8_t *mask;
    DfwPair *p = NULL;
    uint32_t *gk = NULL, *seen = NULL, *lastg = NULL;
    double *out = NULL;
    int ki, vi, ok;
    uint32_t i, g, n, nkeys, ngroups, np = 0, nout = 0;

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

    p = dfw_collect(ctx, &vb, NULL, mask, df->nrows, &np);
    if (!p)
        return JS_EXCEPTION;
    out   = df_out_alloc(ctx, np ? np : 1, sizeof(double));
    seen  = calloc(np ? np : 1, sizeof(*seen));
    lastg = malloc((size_t)(np ? np : 1) * sizeof(*lastg));
    if (!out || !seen || !lastg) {
        free(p); free(out); free(seen); free(lastg);
        return JS_EXCEPTION;
    }
    for (i = 0; i < np; i++)
        lastg[i] = 0xffffffffu;

    n = kb.n < vb.n ? kb.n : vb.n;
    gk = df_keys_u32(&kb, n);
    for (i = 0; i < n; i++) {
        double v;
        uint32_t lo = 0, hi = np, mid;
        if (mask && !mask[i])
            continue;
        g = gk ? gk[i] : (uint32_t)df_get(kb.p, kb.type, i);
        if (g >= ngroups)
            continue;
        v = df_get(vb.p, vb.type, i);
        if (v != v)
            continue;
        while (lo < hi) {               /* the pair table is sorted by value */
            mid = lo + (hi - lo) / 2;
            if (p[mid].key < v) lo = mid + 1;
            else hi = mid;
        }
        if (lo < np && p[lo].key == v && lastg[lo] != g) {
            lastg[lo] = g;
            seen[lo]++;
        }
    }
    free(gk);
    free(lastg);
    for (i = 0; i < np; i++)
        if (seen[i] == nkeys)
            out[nout++] = p[i].key;
    free(p);
    free(seen);
    return df_to_typed_array(ctx, out, (size_t)nout * sizeof(double),
                             JS_TYPED_ARRAY_FLOAT64);
}

/* ---------------------------------------------------------------- accessors */

static JSValue dyn_df_get_nrows(JSContext *ctx, JSValueConst this_val)
{
    DataFrame *df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    return df ? JS_NewInt64(ctx, df->nrows) : JS_EXCEPTION;
}

static JSValue dyn_df_get_ncols(JSContext *ctx, JSValueConst this_val)
{
    DataFrame *df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    return df ? JS_NewInt64(ctx, df->ncols) : JS_EXCEPTION;
}

static JSValue dyn_df_get_columns(JSContext *ctx, JSValueConst this_val)
{
    DataFrame *df = dyn_plain_get(ctx, this_val, dyn_df_class_id);
    JSValue a;
    uint32_t i;
    if (!df)
        return JS_EXCEPTION;
    a = JS_NewArray(ctx);
    if (JS_IsException(a))
        return a;
    for (i = 0; i < df->ncols; i++) {
        JSValue s = JS_NewString(ctx, df->cols[i].name);
        if (JS_IsException(s) ||
            JS_DefinePropertyValueUint32(ctx, a, i, s, JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, a);
            return JS_EXCEPTION;
        }
    }
    return a;
}

/* ------------------------------------------------------------- constructor */

/* Add a numeric column aliasing a TypedArray's buffer. */
static int df_add_numeric(JSContext *ctx, DataFrame *df, const char *name,
                          JSValueConst ta)
{
    JSValue buf;
    size_t off, len, bpe;
    DFColumn *c;
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
    c = &df->cols[df->ncols];
    memset(c, 0, sizeof(*c));
    c->name = strdup(name);
    if (!c->name) {
        JS_FreeValue(ctx, buf);
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    c->type = type;
    c->buffer = buf;                 /* takes the ref */
    c->byte_offset = (uint32_t)off;
    c->length = (uint32_t)(len / bpe);
    df->ncols++;
    return 0;
}

/* Add a string column, dictionary-encoded. This is the ONE place the module
 * copies input data: the codes and the distinct strings are owned. */
static int df_add_strings(JSContext *ctx, DataFrame *df, const char *name,
                          JSValueConst arr, uint32_t n)
{
    DFColumn *c = &df->cols[df->ncols];
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
    /* Scratch dictionary index, discarded before returning. Power of two at
       least 2n so the open-addressed probe stays short. */
    while (dfh_cap < (size_t)n * 2 + 8) dfh_cap <<= 1;
    dfh = (uint32_t *)malloc(dfh_cap * sizeof(uint32_t));
    if (!dfh)
        goto oom;
    memset(dfh, 0xFF, dfh_cap * sizeof(uint32_t));
    dfh_mask = (uint32_t)(dfh_cap - 1);

    for (i = 0; i < n; i++) {
        JSValue v = JS_GetPropertyUint32(ctx, arr, i);
        const char *s;
        uint32_t k;
        if (JS_IsException(v))
            goto fail;
        s = JS_ToCString(ctx, v);
        JS_FreeValue(ctx, v);
        if (!s)
            goto fail;
        /* Hash probe instead of a linear strcmp scan of the whole dictionary.
           The scan was O(n * dict_len): a million rows over ten thousand
           distinct values is ~1e10 string comparisons inside a constructor. */
        {
            uint32_t h = 2166136261u, m, probe;
            const char *cp;
            for (cp = s; *cp; cp++) { h ^= (unsigned char)*cp; h *= 16777619u; }
            m = dfh_mask;
            probe = h & m;
            k = c->dict_len;
            while (dfh[probe] != 0xFFFFFFFFu) {
                if (strcmp(c->dict[dfh[probe]], s) == 0) { k = dfh[probe]; break; }
                probe = (probe + 1) & m;
            }
            if (k == c->dict_len)
                dfh[probe] = c->dict_len;   /* claim the slot for the new entry */
        }
        if (k == c->dict_len) {
            char **nd = realloc(c->dict, (c->dict_len + 1) * sizeof(char *));
            if (!nd) { JS_FreeCString(ctx, s); goto oom; }
            c->dict = nd;
            c->dict[c->dict_len] = strdup(s);
            if (!c->dict[c->dict_len]) { JS_FreeCString(ctx, s); goto oom; }
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

static JSValue dyn_df_ctor(JSContext *ctx, JSValueConst new_target,
                           int argc, JSValueConst *argv)
{
    DataFrame *df;
    JSPropertyEnum *tab = NULL;
    uint32_t ntab = 0, i;
    JSValue obj;

    (void)new_target;
    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "DataFrame(columns): expected an object "
                                 "mapping column name -> TypedArray | string[]");
    if (JS_GetOwnPropertyNames(ctx, &tab, &ntab, argv[0],
                               JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0)
        return JS_EXCEPTION;
    if (ntab > DF_MAX_COLS) {
        js_free(ctx, tab);
        return JS_ThrowRangeError(ctx, "too many columns (max %d)", DF_MAX_COLS);
    }

    df = calloc(1, sizeof(*df));
    if (!df) {
        js_free(ctx, tab);
        return JS_ThrowOutOfMemory(ctx);
    }
    df->cols = calloc(ntab ? ntab : 1, sizeof(DFColumn));
    if (!df->cols) {
        free(df);
        js_free(ctx, tab);
        return JS_ThrowOutOfMemory(ctx);
    }

    for (i = 0; i < ntab; i++) {
        JSValue v = JS_GetProperty(ctx, argv[0], tab[i].atom);
        JSValue nv = JS_AtomToValue(ctx, tab[i].atom);
        size_t nlen = 0;
        const char *name = JS_IsException(nv) ? NULL
                                              : JS_ToCStringLen(ctx, &nlen, nv);
        int rc;
        JS_FreeValue(ctx, nv);
        if (JS_IsException(v) || !name) {
            JS_FreeValue(ctx, v);
            if (name) JS_FreeCString(ctx, name);
            goto fail;
        }
        /* A key may hold U+0000 and the C name is NUL-terminated, so two
           distinct keys collapse to one: {"a\0b","a\0c"} gave COLUMNS
           ["a","a"] and the second column was unreachable forever. */
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
                JS_FreeValue(ctx, lv); JS_FreeValue(ctx, v);
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
                               "(%u), got %u", df->nrows,
                               df->cols[df->ncols - 1].length);
            goto fail;
        }
    }

    for (i = 0; i < ntab; i++)
        JS_FreeAtom(ctx, tab[i].atom);
    js_free(ctx, tab);

    obj = dyn_plain_wrap(ctx, dyn_df_class_id, df, NULL);
    if (JS_IsException(obj)) {
        /* dyn_plain_wrap with a NULL dispose does not free on failure. */
        for (i = 0; i < df->ncols; i++)
            JS_FreeValue(ctx, df->cols[i].buffer);
        dyn_df_dispose(df);
    }
    return obj;

 fail:
    for (i = 0; i < ntab; i++)
        JS_FreeAtom(ctx, tab[i].atom);
    js_free(ctx, tab);
    for (i = 0; i < df->ncols; i++)
        JS_FreeValue(ctx, df->cols[i].buffer);
    dyn_df_dispose(df);
    return JS_EXCEPTION;
}

/* ============================ final surface: 7 methods =================== */

/* Two named typed arrays, the shape RANGE_AGG returns. dfc_pair's keys/values
   would misname both halves. */
static JSValue dfr_pair(JSContext *ctx, const char *na, JSValue a,
                        const char *nb, JSValue b)
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

/* BOUNDING_RATIO(x, y[, mask]): the slope of the line joining the LEFTMOST and
   RIGHTMOST points, chosen by x value. RATE looks the same but reads the first
   and last ROWS, so the two disagree on any column that is not sorted by x. */
static JSValue dyn_df_bounding_ratio(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    DataFrame *df;
    DFBound bx, by;
    const uint8_t *mask;
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
    if (df_bind_numeric(ctx, df, ix, &bx, "BOUNDING_RATIO") ||
        df_bind_numeric(ctx, df, iy, &by, "BOUNDING_RATIO"))
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
        if (x != x || y != y)           /* an unordered x has no side */
            continue;
        if (!have || x < xlo) { xlo = x; ylo = y; }
        if (!have || x > xhi) { xhi = x; yhi = y; }
        have = 1;
    }
    if (!have || xhi == xlo)            /* one point, or a vertical line */
        return JS_NewFloat64(ctx, NAN);
    return JS_NewFloat64(ctx, (yhi - ylo) / (xhi - xlo));
}

enum { DFE_AVG, DFE_SUM, DFE_COUNT, DFE_MAX };

static const char *dfe_name(int magic)
{
    switch (magic) {
    case DFE_SUM:   return "EXPONENTIAL_TIME_DECAYED_SUM";
    case DFE_COUNT: return "EXPONENTIAL_TIME_DECAYED_COUNT";
    case DFE_MAX:   return "EXPONENTIAL_TIME_DECAYED_MAX";
    default:        return "EXPONENTIAL_TIME_DECAYED_AVG";
    }
}

/* All four weight by w = exp(-(tMax - t) / tau): AVG is num/den, SUM is num,
   COUNT is den (the decayed row count, which is why it ignores the value), and
   MAX is the largest weighted value. Relative to the LATEST selected time, so
   the exponent stays non-positive and the sum finite for any timestamps. */
static JSValue dyn_df_etd_avg(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv, int magic)
{
    DataFrame *df;
    DFBound bv, bt;
    const uint8_t *mask;
    const char *op = dfe_name(magic);
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
                                  "positive, got %g", op, tau);
    mask = df_mask_arg(ctx, argc > 3 ? argv[3] : JS_UNDEFINED, df->nrows, &ok);
    if (!ok)
        return JS_EXCEPTION;
    if (df_bind_numeric(ctx, df, iv, &bv, op) ||
        df_bind_numeric(ctx, df, it_, &bt, op))
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
        if (!have || t > tmax) tmax = t;
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
    case DFE_SUM:   return JS_NewFloat64(ctx, num);
    case DFE_COUNT: return JS_NewFloat64(ctx, den);
    case DFE_MAX:   return JS_NewFloat64(ctx, best);
    default: break;
    }
    return den > 0.0 ? JS_NewFloat64(ctx, num / den) : JS_UNDEFINED;
}

/* A dense array of `size`, each row writing at its own position. A later row
   overwrites an earlier one; positions outside are dropped, never grown into. */
static JSValue dyn_df_group_insert_at(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    DataFrame *df;
    DFBound bv, bp;
    const uint8_t *mask;
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
    /* Both scalars are coerced BEFORE the columns are bound: a valueOf hook can
       detach the buffer, and a pointer taken first would be stale. */
    if (JS_ToFloat64(ctx, &sz, argc > 2 ? argv[2] : JS_UNDEFINED))
        return JS_EXCEPTION;
    if (argc > 3 && !JS_IsUndefined(argv[3]) &&
        JS_ToFloat64(ctx, &fill, argv[3]))
        return JS_EXCEPTION;
    if (!(sz >= 0.0) || sz != floor(sz) || sz > (double)DF_MAX_GROUPS)
        return JS_ThrowRangeError(ctx, "GROUP_ARRAY_INSERT_AT(value, position, "
                                  "size): size must be an integer in [0, %d], "
                                  "got %g", DF_MAX_GROUPS, sz);
    mask = df_mask_arg(ctx, argc > 4 ? argv[4] : JS_UNDEFINED, df->nrows, &ok);
    if (!ok)
        return JS_EXCEPTION;
    if (df_bind_numeric(ctx, df, iv, &bv, "GROUP_ARRAY_INSERT_AT") ||
        df_bind_numeric(ctx, df, ip, &bp, "GROUP_ARRAY_INSERT_AT"))
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

#define DF_BITMAP_MAX_BITS (1u << 26)   /* 8 MB of bitmap */

/* Distinct non-negative integers, one bit per value. Bounded by the value
   RANGE where N_UNIQUE is bounded by the ROW COUNT, so past the cap it refuses
   and names N_UNIQUE rather than allocating what the column asks for. */
static JSValue dyn_df_group_bitmap(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    DataFrame *df;
    DFBound b;
    const uint8_t *mask;
    uint32_t *bits = NULL;
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
                                      "bitmap indexes by the value itself", v);
        if (v > mx)
            mx = v;
    }
    if (mx < 0.0)
        return JS_NewUint32(ctx, 0);
    if (mx >= (double)DF_BITMAP_MAX_BITS)
        return JS_ThrowRangeError(ctx, "GROUP_BITMAP: value %g exceeds the "
                                  "bitmap range of %u; use N_UNIQUE, which is "
                                  "bounded by the row count", mx,
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

/* QUANTILE_TDIGEST_WEIGHTED(col, weightCol, q[, mask]): the approximate
   quantile of the weighted distribution, in bounded memory. The exact form is
   QUANTILE_EXACT_WEIGHTED, which sorts and therefore costs by row count. */
static JSValue dyn_df_quantile_td_weighted(JSContext *ctx, JSValueConst this_val,
                                           int argc, JSValueConst *argv)
{
    DataFrame *df;
    DFBound b, wb;
    const uint8_t *mask;
    dfa_tdigest *t;
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
                                  "q must be in [0, 1], got %g", q);
    mask = df_mask_arg(ctx, argc > 3 ? argv[3] : JS_UNDEFINED, df->nrows, &ok);
    if (!ok)
        return JS_EXCEPTION;
    if (df_bind_numeric(ctx, df, idx, &b, "QUANTILE_TDIGEST_WEIGHTED") ||
        df_bind_numeric(ctx, df, widx, &wb, "QUANTILE_TDIGEST_WEIGHTED"))
        return JS_EXCEPTION;

    t = malloc(sizeof(*t));             /* too large for a stack frame */
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

typedef struct { double lo, hi; } DfrRange;

static int dfr_cmp(const void *pa, const void *pb)
{
    double a = ((const DfrRange *)pa)->lo, b = ((const DfrRange *)pb)->lo;

    return a < b ? -1 : (a > b ? 1 : 0);
}

enum { DFR_UNION, DFR_INTERSECT };

/* Half-open ranges merged into their union, or the interval common to all.
   [1,2) and [2,3) join, covering [1,3) with no gap; an empty or inverted range
   covers nothing and is dropped by both. */
static JSValue dyn_df_range_agg(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv, int magic)
{
    const char *op = magic == DFR_INTERSECT ? "RANGE_INTERSECT_AGG"
                                            : "RANGE_AGG";
    DataFrame *df;
    DFBound ba, bb;
    const uint8_t *mask;
    DfrRange *r = NULL;
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
    if (df_bind_numeric(ctx, df, ia, &ba, op) ||
        df_bind_numeric(ctx, df, ib, &bb, op))
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
            continue;                   /* empty or inverted: covers nothing */
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
            if (r[i].lo > lo) lo = r[i].lo;
            if (r[i].hi < hi) hi = r[i].hi;
        }
        free(r);
        if (!(lo < hi))
            return JS_UNDEFINED;        /* they do not all overlap */
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
        free(r); free(ls); free(hs);
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
    /* Strategy variants for the all/any crossover measurement only. Not
       documented, not part of the API; delete these four rows, and the
       _scan/_early kernels with them, once the crossover is pinned. */
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
    /* g1 */
    JS_CFUNC_MAGIC_DEF("SORT", 2, dyn_df_sort, DFO_SORT),
    JS_CFUNC_MAGIC_DEF("ARG_SORT", 2, dyn_df_sort, DFO_ARGSORT),
    JS_CFUNC_DEF("RANK", 2, dyn_df_rank),
    JS_CFUNC_MAGIC_DEF("QUANTILE", 3, dyn_df_quantile, DFO_Q_CONT),
    JS_CFUNC_MAGIC_DEF("PERCENTILE_CONT", 3, dyn_df_quantile, DFO_Q_PCONT),
    JS_CFUNC_MAGIC_DEF("PERCENTILE_DISC", 3, dyn_df_quantile, DFO_Q_DISC),
    JS_CFUNC_MAGIC_DEF("MEDIAN", 2, dyn_df_quantile, DFO_Q_MEDIAN),
    JS_CFUNC_MAGIC_DEF("N_LARGEST", 3, dyn_df_nlargest, DFO_TOP_LARGEST),
    JS_CFUNC_MAGIC_DEF("N_SMALLEST", 3, dyn_df_nlargest, DFO_TOP_SMALLEST),
    /* g2 */
    JS_CFUNC_MAGIC_DEF("CUM_SUM", 2, dyn_df_cumsum, DFS_CUMSUM),
    JS_CFUNC_MAGIC_DEF("CUM_PROD", 2, dyn_df_cumsum, DFS_CUMPROD),
    JS_CFUNC_MAGIC_DEF("CUM_MAX", 2, dyn_df_cumsum, DFS_CUMMAX),
    JS_CFUNC_MAGIC_DEF("CUM_MIN", 2, dyn_df_cumsum, DFS_CUMMIN),
    JS_CFUNC_MAGIC_DEF("SHIFT", 2, dyn_df_shift, DFS_SHIFT),
    JS_CFUNC_MAGIC_DEF("DIFF", 2, dyn_df_shift, DFS_DIFF),
    /* g3 */
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
    /* g4 */
    JS_CFUNC_MAGIC_DEF("HEAD", 3, dyn_df_head, DFP_HEAD),
    JS_CFUNC_MAGIC_DEF("TAIL", 3, dyn_df_head, DFP_TAIL),
    JS_CFUNC_MAGIC_DEF("FIRST", 2, dyn_df_first, DFP_FIRST),
    JS_CFUNC_MAGIC_DEF("LAST", 2, dyn_df_first, DFP_LAST),
    JS_CFUNC_MAGIC_DEF("ARG_MIN", 2, dyn_df_argmin, DFP_ARGMIN),
    JS_CFUNC_MAGIC_DEF("ARG_MAX", 2, dyn_df_argmin, DFP_ARGMAX),
    /* g5 */
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
    /* g6 */
    JS_CFUNC_DEF("SUM_CHECKED", 2, dyn_df_sumChecked),
    JS_CFUNC_MAGIC_DEF("BOOL_AND", 2, dyn_df_bool_reduce, DFL_BOOL_AND),
    JS_CFUNC_MAGIC_DEF("BOOL_OR", 2, dyn_df_bool_reduce, DFL_BOOL_OR),
    JS_CFUNC_MAGIC_DEF("BOOL_XOR", 2, dyn_df_bool_reduce, DFL_BOOL_XOR),
    /* g7 */
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
    /* g8 */
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
};

static int dyn_df_init_module(JSContext *ctx, JSModuleDef *m)
{
    df_init_ta_classes(ctx);
    return dyn_register_plain_class(ctx, m, &dyn_df_class_id, &dyn_df_class,
                                    dyn_df_proto, countof(dyn_df_proto),
                                    dyn_df_ctor, "DataFrame");
}

int js_nat_init_dataframe(JSContext *ctx)
{
    JSModuleDef *m = JS_NewCModule(ctx, "dyna:dataframe", dyn_df_init_module);
    if (!m)
        return -1;
    JS_AddModuleExport(ctx, m, "DataFrame");
    return 0;
}

#endif /* CONFIG_NATIVE_MODULES && CONFIG_NATIVE_MODULE_DATAFRAME */
