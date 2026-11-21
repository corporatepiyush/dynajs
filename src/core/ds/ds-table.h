#ifndef DS_TABLE_H
#define DS_TABLE_H

#include <stddef.h>
#include <stdint.h>
#include "ds-common.h"
#include "ds-cell.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dyn_table dyn_table_t;

dyn_table_t* dyn_table_new(void);

void dyn_table_free(dyn_table_t* t, dyn_cell_free_fn fn, void* ud);

int dyn_table_put(dyn_table_t* t, const char* r, size_t rn,
    const char* c, size_t cn, const dyn_cell_t* v,
    dyn_cell_t* old);

const dyn_cell_t* dyn_table_get(const dyn_table_t* t, const char* r, size_t rn,
    const char* c, size_t cn);

int dyn_table_remove(dyn_table_t* t, const char* r, size_t rn,
    const char* c, size_t cn, dyn_cell_t* out);

uint32_t dyn_table_size(const dyn_table_t* t);

uint32_t dyn_table_slice_first(dyn_table_t* t, const char* k, size_t n,
    int by_column);

uint32_t dyn_table_slice_next(const dyn_table_t* t, uint32_t i, int by_column);

int dyn_table_at(const dyn_table_t* t, uint32_t i, const char** r, size_t* rn,
    const char** c, size_t* cn, const dyn_cell_t** v);

#ifdef __cplusplus
}
#endif

#endif
