#ifndef DYN_DFK_DFK_H
#define DYN_DFK_DFK_H

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

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

enum {
    DFK_OK = 0,
    DFK_E_OOM = -1,
    DFK_E_FLOAT_KEY = -2,
    DFK_E_NEG_KEY = -3,
    DFK_E_TOO_MANY = -4
};

enum {
    DFK_AGG_SUM = 1,
    DFK_AGG_MEAN = 2,
    DFK_AGG_COUNT = 4,
    DFK_AGG_MIN = 8,
    DFK_AGG_MAX = 16
};

typedef struct {
    double* sum;
    double* min;
    double* max;
    uint32_t* cnt;
} dfk_acc_t;

double df_get_call(const void* p, DFType t, uint32_t i);

static inline double df_get(const void* p, DFType t, uint32_t i)
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

typedef struct {
    double n;
    double sx, sy;
    double mx, my;
    double m2x, m3x, m4x;
    double m2y;
    double cxy;
    double lo, hi;
} DfkMoments;

void dfk_widen(const void* p, DFType t, double* dst, uint32_t n);

void dfk_moments(const void* px, DFType tx, const void* py, DFType ty,
    const uint8_t* m, uint32_t n, DfkMoments* o);

typedef struct {
    double key;
    uint32_t idx;
} DfoItem;

#define DFO_SMALL 24

void dfo_sort(DfoItem* it, uint32_t n, int desc, int depth);

void dfo_select(DfoItem* it, uint32_t n, uint32_t k, int desc);

#define DFO_TOP_SEL_RATIO 8u

void dfo_sort_any(DfoItem* it, uint32_t n, int desc);

void dfo_top(DfoItem* it, uint32_t n, uint32_t k, int desc);

#define DFO_PAIR_RADIX_MIN 2048u
#define DFO_SMALL_RADIX_MIN 64u
#define DFO_PAIR_SIZE 16u

int dfo_pair_sort(void* base, uint32_t n, uint32_t min);

void dfo_pair_small_sort(void* base, uint32_t n);

int dfo_f64_sort(double* a, uint32_t n);

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

#define DFO_RADIX_MIN 2048u
#define DFO_RADIX_SORT_MIN 256u
#define DFO_RADIX_PAR_ROWS_PER_WORKER 32768u
#define DFO_RADIX_PAR_MIN_WORKERS 3u

uint32_t dfo_gather(const void* p, DFType t, const uint8_t* mask,
    uint32_t span, DfoItem* it);

uint32_t dfo_valued(const DfoItem* it, uint32_t n);

#define DFK_ROLL_SLIDE_MIN 256u

int dfk_roll_extreme(const void* bp, DFType bt, const uint8_t* mask, double* dst,
    uint32_t span, uint32_t w, int want_min);

int dfk_roll_sum_sel(const void* bp, DFType bt, const uint8_t* mask, double* dst,
    uint32_t span, uint32_t w, int want_mean);

int dfk_group_range(DFType t, const void* p, uint32_t n, uint32_t* pnkeys, uint32_t* pngroups);

uint32_t* dfk_keys_u32(DFType t, const void* p, uint32_t n);

int dfk_group_scan(DFType kt, const void* kp, DFType vt, const void* vp, uint32_t n,
    const uint8_t* mask, uint32_t ngroups, unsigned aggs, const dfk_acc_t* a);

void dfk_group_finish(unsigned agg, const dfk_acc_t* a, uint32_t nkeys, double* out);

uint32_t dfk_keys_present(const int32_t* kp, uint32_t n, uint32_t nkeys, uint8_t* seen);

typedef enum {
    DFK_CSV_AUTO = 0,
    DFK_CSV_F64,
    DFK_CSV_I32,
    DFK_CSV_STR
} DfkCsvKind;

enum {
    DFK_CSV_OK = 0,
    DFK_CSV_E_OOM = -1,
    DFK_CSV_E_QUOTE = -2,
    DFK_CSV_E_GARBAGE = -3,
    DFK_CSV_E_EMPTY = -4,
    DFK_CSV_E_DUP = -5,
    DFK_CSV_E_NOCOL = -6,
    DFK_CSV_E_NUMBER = -7,
    DFK_CSV_E_INT = -8,
    DFK_CSV_E_WIDTH = -9,
    DFK_CSV_E_ROWS = -10
};

typedef struct DfkCsvSpec {
    uint8_t delim, quote;
    int strict;
    uint64_t offset, limit;
    const char* const* columns;
    uint32_t ncolumns;
    const char* const* dtype_names;
    const DfkCsvKind* dtype_kinds;
    uint32_t ndtypes;
    uint64_t seed;
} DfkCsvSpec;

typedef struct {
    const char* name;
    DfkCsvKind kind;
    double* f64;
    int32_t* i32;
    int32_t* codes;
    char** dict;
    uint32_t dict_len;
} DfkCsvCol;

typedef struct DfkCsvOut {
    DfkCsvCol* cols;
    uint32_t ncols;
    uint32_t nrows;
    char** hdr;
    uint32_t nhdr;
    uint64_t err_record;
    char err_text[200];
} DfkCsvOut;

int dfk_csv_read(const uint8_t* buf, size_t len, const DfkCsvSpec* spec, DfkCsvOut* out);

void dfk_csv_free(DfkCsvOut* out);

#endif
