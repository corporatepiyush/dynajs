#ifndef DATAFRAME_INTERNAL_H
#define DATAFRAME_INTERNAL_H

#include "dyna-nat.h"
#include "cutils.h"
#include "dtoa.h"
#include "core/dyn-par.h"
#include "core/dyn-dfk.h"

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

void df_fmt_double(char* out, size_t cap, double v);

#define DF_MAX_COLS 1024

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
    char* out_new;
    uint32_t out_gen;
} DataFrame;

#define DF_OUT_NEW 0x100000
#define DF_OUT_GEN_MASK 0xFFFFF

typedef struct {
    const void* p;
    DFType type;
    uint32_t n;
} DFBound;

extern JSClassID dyn_df_class_id;

int dyn_df_ensure_class(JSContext* ctx);

struct df_ta_class_s {
    JSClassID id;
    DFType type;
};

extern struct df_ta_class_s df_ta_class[8];

extern int df_ta_class_count;

int df_type_of_value(JSValueConst v, DFType* out);

void df_col_free(DFColumn* c);

static inline void dyn_df_dispose(void* native)
{
    DataFrame* df = native;
    uint32_t i;
    if (!df)
        return;
    for (i = 0; i < df->ncols; i++)
        df_col_free(&df->cols[i]);
    free(df->cols);
    free(df->out_new);
    free(df);
}

size_t df_elt_size(DFType t);

const char* df_type_name(DFType t);

JSValue df_to_typed_array(JSContext* ctx, void* p, size_t nbytes,
    JSTypedArrayEnum type);

static inline int df_find_col(const DataFrame* df, const char* name)
{
    uint32_t i;
    for (i = 0; i < df->ncols; i++)
        if (strcmp(df->cols[i].name, name) == 0)
            return (int)i;
    return -1;
}

int dyn_df_bind(JSContext* ctx, const DataFrame* df, int idx, DFBound* b);

int df_col_arg(JSContext* ctx, const DataFrame* df, JSValueConst v);

const uint8_t* df_mask_arg(JSContext* ctx, JSValueConst v,
    uint32_t nrows, int* ok);

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

JSValue dyn_df_reduce(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic);

#define DF_STEP_MUL(acc, v) ((acc) *= (v))

void df_widen_block(double* restrict dst, const void* src, DFType t,
    uint32_t off, uint32_t n);

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define DF_PACK_SWAR 1
#endif

enum { DF_VAR_SAMPLE,
    DF_VAR_STDDEV };

enum { DF_BIT_AND,
    DF_BIT_OR,
    DF_BIT_XOR };

enum { DF_ALL,
    DF_ANY };

JSValue dyn_df_product(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_df_variance(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic);

JSValue dyn_df_dot_product(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_df_bitwise(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic);

JSValue dyn_df_all_any(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic);

JSValue dyn_df_bitmask(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

enum { DF_GT,
    DF_GE,
    DF_LT,
    DF_LE,
    DF_EQ,
    DF_NE };

JSValue dyn_df_compare(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic);

#define DF_CAT_(a, b) a##b
#define DF_CAT(a, b) DF_CAT_(a, b)

#define DF_KERNEL(kind, name, suffix) \
    DF_CAT(DF_CAT(df_, kind), DF_CAT(DF_CAT(_, name), DF_CAT(_, suffix)))

typedef void (*DFMapFn)(const void* src, double* restrict dst, uint32_t n);

typedef void (*DFMapKFn)(const void* src, double* restrict dst, uint32_t n,
    double k0, double k1);

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

#define DF_MAPK_ONE(suffix, ctype, tag) DF_DEFINE_MAPK(DF_OP_NAME, suffix, ctype, DF_OP_EXPR)
#define DF_MAPK_ENTRY(suffix, ctype, tag) [tag] = DF_KERNEL(mapk, DF_OP_NAME, suffix),

extern const DFMapFn df_map_widen_tab[8];

extern const DFMapFn* const df_map1_tab[8];

extern const char* const df_map1_name[8];

enum { DF_BIN_ADD,
    DF_BIN_SUB,
    DF_BIN_MUL,
    DF_BIN_DIV,
    DF_BIN_POW,
    DF_BIN_NCOMB,
    DF_BIN_RSUB = DF_BIN_NCOMB,
    DF_BIN_RDIV,
    DF_BIN_N };

extern const DFMapKFn* const df_bin_scalar_tab[7];

extern const char* const df_bin_name[7];

_Static_assert(countof(df_map1_tab) == countof(df_map1_name),
    "map table and name table must agree");

_Static_assert(countof(df_bin_scalar_tab) == countof(df_bin_name),
    "binary table and name table must agree");

_Static_assert(DF_BIN_NCOMB < DF_BIN_N,
    "the scalar-only ops must follow the column-capable ones");

void* df_out_alloc(JSContext* ctx, uint32_t n, size_t esz);

int df_bind_numeric(JSContext* ctx, const DataFrame* df, int idx,
    DFBound* b, const char* op);

int df_is_out_bag(JSContext* ctx, JSValueConst v);

int df_out_opt(JSContext* ctx, const DataFrame* df, JSValueConst v,
    const char* op, int* pout_idx);

int df_store_out(JSContext* ctx, DataFrame* df, int out_idx,
    const double* dst, uint32_t n);

static inline uint32_t df_map_span(uint32_t nrows, uint32_t avail, double* dst)
{
    uint32_t n = avail < nrows ? avail : nrows, i;
    for (i = n; i < nrows; i++)
        dst[i] = DYN_NAN;
    return n;
}

JSValue dyn_df_map1(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic);

JSValue dyn_df_isna(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic);

JSValue dyn_df_between(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_df_clip(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_df_fillna(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_df_binary(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic);

JSValue dyn_df_where(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

int dfc_group_count(JSContext* ctx, const DataFrame* df, int ki,
    uint32_t* pnkeys, uint32_t* pngroups);

int dfo_sorted(JSContext* ctx, const DFBound* b, const uint8_t* mask,
    uint32_t nrows, int desc, DfoItem** out, uint32_t* pn);

int dfo_gathered(JSContext* ctx, const DFBound* b, const uint8_t* mask,
    uint32_t nrows, DfoItem** out, uint32_t* pn,
    uint32_t* pvalued);

int dfo_open(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv, int nscalar, const char* op,
    DataFrame** pdf, DFBound* b, double* pscalar,
    const uint8_t** pmask, int* pout_idx);

enum { DFO_SORT,
    DFO_ARGSORT };

JSValue dyn_df_sort(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic);

JSValue dyn_df_rank(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

enum { DFO_Q_CONT,
    DFO_Q_PCONT,
    DFO_Q_DISC,
    DFO_Q_MEDIAN };

JSValue dyn_df_quantile(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic);

enum { DFO_TOP_LARGEST,
    DFO_TOP_SMALLEST };

JSValue dyn_df_nlargest(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic);

enum { DFS_CUMSUM,
    DFS_CUMPROD,
    DFS_CUMMAX,
    DFS_CUMMIN,
    DFS_SHIFT,
    DFS_DIFF,
    DFS_N };

extern const char* const dfs_name[6];

_Static_assert(countof(dfs_name) == DFS_N,
    "every scan magic needs a name: a missing entry is a NULL "
    "passed to %s in a throw, not a compile error");

void df_widen(const DFBound* b, double* dst, uint32_t n);

JSValue dyn_df_cumsum(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic);

JSValue dyn_df_shift(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic);

typedef struct {
    uint32_t* slots;
    uint64_t* keys;
    uint32_t* tags;
    uint32_t* counts;
    uint32_t nslots, mask, nent, cap_ent;
} DfcSet;

_Static_assert(DF_MAX_GROUPS > 0 && DF_MAX_GROUPS <= (1 << 29),
    "DF_MAX_GROUPS must leave room for 2x slots in a uint32");

uint64_t dfc_key(double v);

double dfc_key_value(uint64_t bits);

extern atomic_uint_least64_t dfc_hash_seed;

void dfc_hash_seed_once(void);

uint32_t dfc_hash(uint64_t k, uint32_t tag);

void dfc_set_free(DfcSet* s);

int dfc_set_put(JSContext* ctx, DfcSet* s, uint64_t k, uint32_t tag,
    uint32_t* pent, int* pfresh);

int dfc_scan(JSContext* ctx, const DataFrame* df, int idx,
    const uint8_t* mask, DfcSet* set);

JSValue dfc_key_js(JSContext* ctx, const DFColumn* c, uint64_t bits);

JSValue dfc_keys_array(JSContext* ctx, const DFColumn* c,
    const DfcSet* s, const uint32_t* ord, uint32_t n);

JSValue dfc_pair(JSContext* ctx, JSValue keys, JSValue values);

JSValue dyn_df_unique(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_df_nunique(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

enum { DFC_VC_ALL,
    DFC_VC_TOPK };

uint32_t* dfc_rank_order(JSContext* ctx, const DfcSet* s);

JSValue dyn_df_value_counts(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic);

JSValue dyn_df_mode(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_df_drop_duplicates(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

enum { DFC_GROUP_ALL,
    DFC_GROUP_UNIQ };

typedef struct {
    double* flat;
    uint32_t *cnt, *off;
    uint32_t nkeys, ngroups, total, n;
} DfcGrouped;

void dfc_grouped_free(DfcGrouped* G);

int dfc_group_gather(JSContext* ctx, DataFrame* df, int ki, int vi,
    const uint8_t* mask, int uniq, DfcGrouped* G);

JSValue dfc_values_array(JSContext* ctx, const DfcGrouped* G);

JSValue dfc_dense_keys(JSContext* ctx, const DataFrame* df, int ki,
    uint32_t nkeys);

JSValue dyn_df_group_array_dense(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic);

enum { DFC_MOVING_SUM,
    DFC_MOVING_AVG };

#define DFG_ROLL_SLIDE_MIN DFK_ROLL_SLIDE_MIN

void dfc_moving_slice(double* v, uint32_t m, uint32_t w, int want_avg);

JSValue dyn_df_group_array_moving_dense(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic);

enum { DFP_HEAD,
    DFP_TAIL };

enum { DFP_FIRST,
    DFP_LAST };

enum { DFP_ARGMIN,
    DFP_ARGMAX };

static inline int dfp_bind_num(JSContext* ctx, const DataFrame* df, int idx,
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

static inline uint32_t dfp_span(uint32_t nrows, uint32_t avail)
{
    return avail < nrows ? avail : nrows;
}

JSValue dyn_df_head(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic);

JSValue dyn_df_first(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic);

JSValue dyn_df_argmin(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic);

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

typedef DfkMoments DFMoments;

static inline void dfm_moments(const void* px, DFType tx,
    const void* py, DFType ty,
    const uint8_t* m, uint32_t n, DFMoments* o)
{
    dfk_moments(px, tx, py, ty, m, n, o);
}

double dfm_skew(const DFMoments* o);

double dfm_kurt(const DFMoments* o);

JSValue dyn_df_moments1(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic);

JSValue dyn_df_moments2(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic);

JSValue dyn_df_mean_weighted(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

int dfm_set(JSContext* ctx, JSValue obj, const char* key, JSValue v);

JSValue dyn_df_describe(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

enum { DFL_BOOL_AND,
    DFL_BOOL_OR,
    DFL_BOOL_XOR };

JSValue dyn_df_bool_reduce(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic);

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

JSValue dyn_df_sumChecked(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

const char* dfg_agg_verb(int magic);

JSValue dyn_df_group_agg_dense(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic);

const char* dfg_roll_name(int magic);

static inline int dfg_roll_extreme(const DFBound* b, const uint8_t* mask, double* dst,
    uint32_t span, uint32_t w, int want_min)
{
    return dfk_roll_extreme(b->p, b->type, mask, dst, span, w, want_min);
}

static inline int dfg_roll_sum_sel(const DFBound* b, const uint8_t* mask, double* dst,
    uint32_t span, uint32_t w, int want_mean)
{
    return dfk_roll_sum_sel(b->p, b->type, mask, dst, span, w, want_mean);
}

JSValue dyn_df_rolling(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic);

JSValue dyn_df_dropna(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

#include "core/dyn-ds.h"
#include "core/dyn-hash.h"
#define DFA_TD_MAIN_CAP 400
#define DFA_TD_BUF_CAP 1024
#define DFA_TD_MERGE_CAP (DFA_TD_MAIN_CAP + DFA_TD_BUF_CAP)
#define DFA_KMV_K 256u

uint64_t dfa_canon_bits(double v);

double dfa_bits_value(uint64_t b);

uint64_t dfa_hash_bits(uint64_t bits);

uint64_t dfa_row_hash(const DFColumn* c, const DFBound* b, uint32_t i);

JSValue dfa_result_pair(JSContext* ctx, JSValue keys, double* vals,
    uint32_t n);

JSValue dfa_exact_distinct_str(JSContext* ctx, const DFColumn* c,
    const DFBound* b, const uint8_t* mask);

JSValue dyn_df_approxCountDistinct(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

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

void dfa_td_init(dfa_tdigest* t);

void dfa_td_add_w(dfa_tdigest* t, double v, double w);

void dfa_td_add(dfa_tdigest* t, double v);

double dfa_td_quantile(dfa_tdigest* t, double q);

JSValue dyn_df_approxPercentile(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

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

void dfa_ss_free(dfa_ss* s);

int dfa_ss_init(dfa_ss* s, uint32_t m);

void dfa_ss_add(dfa_ss* s, uint64_t bits);

int dfa_ss_cmp(const void* pa, const void* pb);

JSValue dfa_exact_topk_str(JSContext* ctx, const DFColumn* c,
    const DFBound* b, const uint8_t* mask,
    uint32_t k);

JSValue dyn_df_approxTopK(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

typedef struct {
    uint64_t h[DFA_KMV_K];
    uint32_t n;
} dfa_kmv;

void dfa_kmv_add(dfa_kmv* s, uint64_t h);

double dfa_kmv_jaccard(dfa_kmv* a, dfa_kmv* b);

JSValue dyn_df_approxSimilarity(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

enum { DFX_SEM,
    DFX_SKEW_SAMP,
    DFX_KURT_SAMP,
    DFX_COUNT_NULLS,
    DFX_REGR_COUNT,
    DFX_REGR_SXX,
    DFX_REGR_SYY,
    DFX_REGR_SXY };

JSValue dyn_df_stat1(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic);

JSValue dyn_df_regr_sum(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic);

enum { DFX_MAD,
    DFX_MED_AD };

JSValue dyn_df_deviation(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic);

JSValue dyn_df_entropy(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

enum { DFX_Q_LOW,
    DFX_Q_HIGH };

JSValue dyn_df_quantile_lh(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic);

JSValue dyn_df_quantiles(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_df_quantiles_td(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_df_uniq_up_to(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_df_histogram(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic);

JSValue dyn_df_ema(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_df_delta_sum(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

enum { DFX_RATE,
    DFX_IRATE };

JSValue dyn_df_rate(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic);

enum { DFY_BIT_AND,
    DFY_BIT_OR,
    DFY_BIT_XOR };

JSValue dyn_df_group_bit_dense(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic);

JSValue dyn_df_corr_matrix(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic);

enum { DFX_ROLL_VAR,
    DFX_ROLL_STD };

uint32_t dfg_roll_win2(const double* x, const uint8_t* mask,
    uint32_t lo, uint32_t hi, double* pm, double* pq,
    double* ps);

int dfg_roll_var_slide(const DFBound* b, const uint8_t* mask, double* dst,
    uint32_t span, uint32_t w, int want_std);

JSValue dyn_df_rolling_disp(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic);

JSValue dyn_df_pct_change(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_df_zscore(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

enum { DFX_DENSE_RANK,
    DFX_PERCENT_RANK };

JSValue dyn_df_rank_ext(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic);

JSValue dyn_df_ntile(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_df_rank_corr(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_df_group_concat(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

enum { DFZ_SORTED,
    DFZ_LAST,
    DFZ_SAMPLE };

JSValue dyn_df_group_array_v_dense(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic);

typedef struct {
    double key, w;
} DfwPair;

void dfw_sort_k(DfwPair* p, uint32_t n);
int dfw_sort_stable(DfwPair* p, uint32_t n);

_Static_assert(sizeof(DfwPair) == DFO_PAIR_SIZE && offsetof(DfwPair, key) == 0
        && sizeof(dfa_centroid) == DFO_PAIR_SIZE && offsetof(dfa_centroid, mean) == 0,
    "dfo_pair_sort sorts 16-byte records keyed by the leading double");

JSValue dyn_df_delta_sum_ts(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

DfwPair* dfw_collect(JSContext* ctx, const DFBound* b, const DFBound* w,
    const uint8_t* mask, uint32_t nrows, uint32_t* pn, const char* op);

enum { DFW_TOPK_W,
    DFW_APPROX_TOP_SUM,
    DFW_ANY_HEAVY };

JSValue dyn_df_weighted_top(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic);

JSValue dyn_df_quantile_weighted(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_df_group_intersect(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_df_get_columns(JSContext* ctx, JSValueConst this_val);

int df_add_dict(JSContext* ctx, DataFrame* df, const char* name, JSValueConst obj);

int df_add_strings(JSContext* ctx, DataFrame* df, const char* name,
    JSValueConst arr, uint32_t n);

typedef struct {
    DataFrame* df;
    uint32_t cap, nrows;
    const char* op;
    uint32_t* name_slots;
    uint32_t name_mask;
} DFBuilder;

#define DFB_NO_ROW UINT32_MAX
#define DFB_MAX_ROWS UINT32_MAX

void dfb_free(JSContext* ctx, DFBuilder* B);

int dfb_init(JSContext* ctx, DFBuilder* B, const char* op,
    uint32_t ncols, uint64_t nrows);

int dfb_add(JSContext* ctx, DFBuilder* B, const char* name,
    DFType t, void* p, uint32_t n);

int dfb_add_str(JSContext* ctx, DFBuilder* B, const char* name,
    int32_t* codes, char** dict, uint32_t dict_len,
    uint32_t n);

int dfb_add_gather(JSContext* ctx, DFBuilder* B, const char* name,
    const DataFrame* sdf, int scol, DFType out_type,
    const uint32_t* rows, uint32_t n);

JSValue dfb_seal(JSContext* ctx, DFBuilder* B);

int dfc_set_find(const DfcSet* s, uint64_t k, uint32_t tag, uint32_t* pent);

typedef struct {
    char** dict;
    uint32_t dict_len, dict_cap;
    uint32_t* slots;
    uint32_t slots_mask;
} DfStrTab;

void df_strtab_free(DfStrTab* t);

int df_strtab_put(JSContext* ctx, DfStrTab* t, const char* s,
    uint32_t* code);

int df_strtab_find(const DfStrTab* t, const char* s);

const char* df_type_short(DFType t);

JSTypedArrayEnum df_type_ta(DFType t);

void* df_col_copy(JSContext* ctx, const DataFrame* df, int idx,
    uint32_t* n_out, DFType* type_out);

JSValue dyn_df_dtypes(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_df_schema(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

uint64_t df_col_bytes(const DFColumn* c);

JSValue dyn_df_info(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_df_memory_usage(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_df_to_columns(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_df_to_records(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_df_to_json(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_df_to_csv(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_df_from_records(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_df_copy(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_df_select(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_df_drop_columns(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_df_rename(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_df_filter(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_df_slice(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_df_sample(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_df_isin(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_df_mask(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_df_join(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_df_asof_join(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_df_concat(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

enum { DFR_SUM,
    DFR_MEAN,
    DFR_MIN,
    DFR_MAX,
    DFR_COUNT };

JSValue dyn_df_resample(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_df_pivot(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_df_melt(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

enum { DFJA_AGG,
    DFJA_OBJ,
    DFJA_AGG_STRICT,
    DFJA_OBJ_STRICT };

JSValue dfja_build(JSContext* ctx, const DataFrame* df, int ki,
    const double* keys, const double* vals,
    const uint8_t* mask, uint32_t n, int obj_form,
    int strict);

JSValue dyn_df_json_agg(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic);

JSValue dyn_df_bounding_ratio(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

enum { DFE_AVG,
    DFE_SUM,
    DFE_COUNT,
    DFE_MAX };

JSValue dyn_df_etd_avg(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic);

JSValue dyn_df_group_insert_at(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_df_group_bitmap(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_df_quantile_td_weighted(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

enum { DFR_UNION,
    DFR_INTERSECT };

JSValue dyn_df_range_agg(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic);

typedef JSValue dfc_group_fn(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv, int magic);

JSValue dfc_grouped(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv, int magic, dfc_group_fn* fn);

JSValue dyn_df_group_by_agg(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_df_group_by_sum(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

#endif

#endif
