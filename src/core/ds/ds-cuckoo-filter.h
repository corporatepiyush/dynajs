#ifndef DS_CUCKOO_FILTER_H
#define DS_CUCKOO_FILTER_H

#include <stddef.h>
#include <stdint.h>
#include "ds-common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DYN_CUCKOO_SLOTS 4
#define DYN_CUCKOO_MAX_KICKS 500
#define DYN_CUCKOO_MAX_CAPACITY (1u << 26)

typedef struct {
    uint16_t* t;
    uint32_t nb, mask;
    uint32_t count;
    uint16_t victim_fp;
    uint32_t victim_i;
    int has_victim;
    uint64_t rng;
} dyn_cuckoo_t;

void dyn_cuckoo_hash(const dyn_cuckoo_t* c, const char* k, size_t n, uint32_t* i, uint16_t* fp);

int dyn_cuckoo_insert(dyn_cuckoo_t* c, uint32_t i, uint16_t fp);

int dyn_cuckoo_contains(const dyn_cuckoo_t* c, uint32_t i, uint16_t fp);

int dyn_cuckoo_delete(dyn_cuckoo_t* c, uint32_t i, uint16_t fp);

int dyn_cuckoo_init(dyn_cuckoo_t* c, uint32_t nb);

void dyn_cuckoo_free(dyn_cuckoo_t* c);

#ifdef __cplusplus
}
#endif

#endif
