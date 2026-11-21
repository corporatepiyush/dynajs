#ifndef VECK_H
#define VECK_H

#include <stddef.h>
#include <stdint.h>
#include "../ds/ds-bitset.h"
#include "../ds/ds-roaring-bitmap.h"

#define DYN_VEC_NONE 0xFFFFFFFFu
#define DYN_VEC_NOID UINT64_MAX
#define DYN_VEC_MAX_DIM 65536u
#define DYN_VEC_MIN_M 2u
#define DYN_VEC_MAX_M 256u
#define DYN_VEC_MAX_EF 65536u
#define DYN_VEC_MAX_POOL 64u
#define DYN_VEC_BINARY_POOL 12u
#define DYN_VEC_MAX_K 65536u
#define DYN_VEC_MAX_LEVEL 30
#define DYN_VEC_MAX_SIZE 0x7FFFFFF0u
#define DYN_VEC_PERSIST_TYPE 200u
#define DYN_VEC_META_WORDS 16u
#define DYN_VEC_LAYOUT 1u

enum { DYN_VEC_L2,
    DYN_VEC_COSINE,
    DYN_VEC_IP };

enum { DYN_VEC_FLAT,
    DYN_VEC_HNSW };

enum { DYN_VEC_F32,
    DYN_VEC_SQ8,
    DYN_VEC_BINARY };

enum { DYN_VEC_OK = 0,
    DYN_VEC_ENOMEM = -1,
    DYN_VEC_EDUP = -2,
    DYN_VEC_ENONFINITE = -3,
    DYN_VEC_EZERO = -4,
    DYN_VEC_EFULL = -5,
    DYN_VEC_ECORRUPT = -6 };

typedef struct {
    const dyn_roar_t* roar;
    const dyn_bitset_t* bits;
    const uint64_t* ids;
    uint64_t nids;
} dyn_vec_filter_t;

typedef struct {
    double d;
    uint64_t s;
} dyn_vec_hit_t;

typedef struct {
    uint32_t dim, metric, kind, M, M0, efc, efs;
    uint8_t heuristic, exact_fallback;
    uint64_t rng;
    double ml;
    uint32_t n, cap, live;
    float* data;
    uint64_t* ext;
    uint8_t* dead;
    uint64_t* hkey;
    uint32_t* hval;
    uint32_t hcap;
    uint8_t* lvl;
    uint32_t** links;
    uint32_t entry;
    int top;
    uint32_t* vis;
    uint32_t vtag;
    dyn_vec_hit_t *cand, *res, *tmp;
    uint32_t ccap, rcap, tcap;
    float* qbuf;
    uint32_t* lpool;
    uint64_t lwords;
    uint8_t data_borrowed;
    uint32_t quant, cb, words, pool;
    uint8_t keep, trained;
    uint8_t* codes;
    double* corr;
    double *lo, *hi, *mid;
    double scale, msq;
    int8_t* qcbuf;
    const int8_t* qc;
    const uint64_t* qb;
    const float* qf;
    double qcorr;
    int qmode;
} dyn_vec_t;

typedef struct {
    uint32_t dim, metric, kind, M, efc, efs;
    int heuristic, exact_fallback;
    uint64_t seed;
    uint32_t quant;
    int rerank;
    uint32_t pool;
} dyn_vec_params_t;

int dyn_vec_init(dyn_vec_t* v, const dyn_vec_params_t* p);

void dyn_vec_free(dyn_vec_t* v);

int dyn_vec_reserve(dyn_vec_t* v, uint32_t extra);

int dyn_vec_train(dyn_vec_t* v, const float* x, uint32_t nb);

int dyn_vec_check(const dyn_vec_t* v, const float* x);

uint32_t dyn_vec_find(const dyn_vec_t* v, uint64_t id);

int dyn_vec_add(dyn_vec_t* v, uint64_t id, const float* x);

int dyn_vec_remove(dyn_vec_t* v, uint64_t id);

int dyn_vec_search(dyn_vec_t* v, const float* q, uint32_t k, uint32_t ef,
    const dyn_vec_filter_t* f, dyn_vec_hit_t* out, uint32_t* nout);

uint64_t dyn_vec_filter_card(const dyn_vec_filter_t* f);

int dyn_vec_ids_sorted(const uint64_t* ids, uint64_t n);

uint64_t dyn_vec_block_words(const dyn_vec_t* v, uint32_t slot);

void dyn_vec_meta(const dyn_vec_t* v, uint64_t* meta);

typedef struct {
    const uint64_t* meta;
    const float* data;
    uint64_t data_bytes;
    const uint64_t* ext;
    uint64_t ext_bytes;
    const uint8_t* dead;
    uint64_t dead_bytes;
    const uint8_t* lvl;
    uint64_t lvl_bytes;
    const uint32_t* links;
    uint64_t links_bytes;
    const double* qparam;
    uint64_t qparam_bytes;
    const uint8_t* codes;
    uint64_t codes_bytes;
    int borrow_data;
} dyn_vec_image_t;

int dyn_vec_restore(dyn_vec_t* v, const dyn_vec_image_t* img, const char** why);

#endif
