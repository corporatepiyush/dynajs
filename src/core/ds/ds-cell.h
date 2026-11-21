#ifndef DS_CELL_H
#define DS_CELL_H

#include <stddef.h>
#include <stdint.h>
#include "ds-common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint64_t w[2];
} dyn_cell_t;

typedef void (*dyn_cell_free_fn)(void* ud, const dyn_cell_t* cell);

typedef void (*dyn_cell_dup_fn)(void* ud, const dyn_cell_t* in,
    dyn_cell_t* out);

#ifdef __cplusplus
}
#endif

#endif
