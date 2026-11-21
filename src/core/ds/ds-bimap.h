#ifndef DS_BIMAP_H
#define DS_BIMAP_H

#include <stddef.h>
#include <stdint.h>
#include "ds-common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dyn_bimap dyn_bimap_t;

dyn_bimap_t* dyn_bimap_new(void);

void dyn_bimap_free(dyn_bimap_t* b);

#define DYN_BIMAP_VALUE_TAKEN 1

int dyn_bimap_put(dyn_bimap_t* b, const char* k, size_t kn,
    const char* v, size_t vn, int force);

const char* dyn_bimap_get(const dyn_bimap_t* b, const char* k, size_t kn,
    size_t* vn);

const char* dyn_bimap_key_for(const dyn_bimap_t* b, const char* v, size_t vn,
    size_t* kn);

int dyn_bimap_remove(dyn_bimap_t* b, const char* k, size_t kn);

int dyn_bimap_remove_value(dyn_bimap_t* b, const char* v, size_t vn);

uint32_t dyn_bimap_size(const dyn_bimap_t* b);

void dyn_bimap_clear(dyn_bimap_t* b);

int dyn_bimap_at(const dyn_bimap_t* b, uint32_t i, const char** k, size_t* kn,
    const char** v, size_t* vn);

#ifdef __cplusplus
}
#endif

#endif
