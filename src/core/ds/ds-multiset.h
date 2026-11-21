#ifndef DS_MULTISET_H
#define DS_MULTISET_H

#include <stddef.h>
#include <stdint.h>
#include "ds-common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dyn_mset dyn_mset_t;

dyn_mset_t* dyn_mset_new(void);

void dyn_mset_free(dyn_mset_t* m);

int dyn_mset_add(dyn_mset_t* m, const char* k, size_t n, int64_t delta,
    uint64_t* out);

int dyn_mset_set_count(dyn_mset_t* m, const char* k, size_t n, uint64_t c);

uint64_t dyn_mset_count(const dyn_mset_t* m, const char* k, size_t n);

uint64_t dyn_mset_total(const dyn_mset_t* m);

uint32_t dyn_mset_distinct(const dyn_mset_t* m);

void dyn_mset_clear(dyn_mset_t* m);

int dyn_mset_at(const dyn_mset_t* m, uint32_t i, const char** k, size_t* n,
    uint64_t* count);

#ifdef __cplusplus
}
#endif

#endif
