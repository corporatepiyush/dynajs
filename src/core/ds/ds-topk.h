#ifndef DS_TOPK_H
#define DS_TOPK_H

#include <stddef.h>
#include <stdint.h>
#include "ds-common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char* key;
    uint32_t klen;
    uint32_t hpos;
    double count;
    double err;
} dyn_topk_slot;

typedef struct {
    uint32_t k, n;
    dyn_topk_slot* s;
    uint32_t* heap;
    dyn_ds_smap idx;
    double total;
} dyn_topk_t;

#define DYN_TOPK_MAX_K (1u << 20)

void dyn_topk_up(dyn_topk_t* t, uint32_t i);

void dyn_topk_free(dyn_topk_t* t);

int dyn_topk_add(dyn_topk_t* t, const char* k, size_t n, double c, char** evicted, size_t* evn);

void dyn_topk_sorted(const dyn_topk_t* t, uint32_t* ord);

int dyn_topk_init(dyn_topk_t* t, uint32_t k);

#ifdef __cplusplus
}
#endif

#endif
