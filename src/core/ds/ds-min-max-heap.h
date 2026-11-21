#ifndef DS_MIN_MAX_HEAP_H
#define DS_MIN_MAX_HEAP_H

#include <stddef.h>
#include <stdint.h>
#include "ds-common.h"
#include "ds-cell.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dyn_mmheap dyn_mmheap_t;

dyn_mmheap_t* dyn_mmheap_new(void);

void dyn_mmheap_free(dyn_mmheap_t* h, dyn_cell_free_fn fn, void* ud);

int dyn_mmheap_push(dyn_mmheap_t* h, double pri, const dyn_cell_t* v);

int dyn_mmheap_pop_min(dyn_mmheap_t* h, double* pri, dyn_cell_t* out);

int dyn_mmheap_pop_max(dyn_mmheap_t* h, double* pri, dyn_cell_t* out);

int dyn_mmheap_peek_min(const dyn_mmheap_t* h, double* pri,
    const dyn_cell_t** v);

int dyn_mmheap_peek_max(const dyn_mmheap_t* h, double* pri,
    const dyn_cell_t** v);

uint32_t dyn_mmheap_size(const dyn_mmheap_t* h);

const dyn_cell_t* dyn_mmheap_cell_at(const dyn_mmheap_t* h, uint32_t i);

double dyn_mmheap_pri_at(const dyn_mmheap_t* h, uint32_t i);

#ifdef __cplusplus
}
#endif

#endif
