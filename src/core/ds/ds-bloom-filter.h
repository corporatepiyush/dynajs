#ifndef DS_BLOOM_FILTER_H
#define DS_BLOOM_FILTER_H

#include <stddef.h>
#include <stdint.h>
#include "ds-common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint64_t* words;
    uint32_t nbits;
    uint32_t nwords;
    uint32_t k;
} dyn_bloom_t;

int dyn_bloom_init(dyn_bloom_t* b, uint32_t nbits, uint32_t k);

void dyn_bloom_free(dyn_bloom_t* b);

void dyn_bloom_add(dyn_bloom_t* b, const void* key, size_t n);

int dyn_bloom_may_contain(const dyn_bloom_t* b, const void* key, size_t n);

#ifdef __cplusplus
}
#endif

#endif
