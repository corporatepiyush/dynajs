#ifndef DS_MULTIMAP_H
#define DS_MULTIMAP_H

#include <stddef.h>
#include <stdint.h>
#include "ds-common.h"
#include "ds-cell.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dyn_mmap dyn_mmap_t;

dyn_mmap_t* dyn_mmap_new(void);

void dyn_mmap_free(dyn_mmap_t* m, dyn_cell_free_fn fn, void* ud);

int dyn_mmap_put(dyn_mmap_t* m, const char* k, size_t n, const dyn_cell_t* v);

uint32_t dyn_mmap_count(const dyn_mmap_t* m, const char* k, size_t n);

const dyn_cell_t* dyn_mmap_at(const dyn_mmap_t* m, const char* k, size_t n,
    uint32_t i);

int dyn_mmap_remove_at(dyn_mmap_t* m, const char* k, size_t n, uint32_t i,
    dyn_cell_t* out);

uint32_t dyn_mmap_remove_key(dyn_mmap_t* m, const char* k, size_t n,
    dyn_cell_free_fn fn, void* ud);

uint32_t dyn_mmap_keys(const dyn_mmap_t* m);

uint64_t dyn_mmap_size(const dyn_mmap_t* m);

int dyn_mmap_key_at(const dyn_mmap_t* m, uint32_t i, const char** k, size_t* n,
    uint32_t* count);

const dyn_cell_t* dyn_mmap_cells_at(const dyn_mmap_t* m, uint32_t i,
    uint32_t* count);

const dyn_cell_t* dyn_mmap_cells_for(const dyn_mmap_t* m, const char* k,
    size_t n, uint32_t* count);

#ifdef __cplusplus
}
#endif

#endif
