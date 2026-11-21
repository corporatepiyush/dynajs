#ifndef DS_RANGE_SET_H
#define DS_RANGE_SET_H

#include <stddef.h>
#include <stdint.h>
#include "ds-common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dyn_rset dyn_rset_t;

dyn_rset_t* dyn_rset_new(void);

void dyn_rset_free(dyn_rset_t* s);

int dyn_rset_add(dyn_rset_t* s, double lo, double hi);

int dyn_rset_remove(dyn_rset_t* s, double lo, double hi);

int dyn_rset_contains(const dyn_rset_t* s, double x);

int dyn_rset_encloses(const dyn_rset_t* s, double lo, double hi);

int dyn_rset_intersects(const dyn_rset_t* s, double lo, double hi);

uint32_t dyn_rset_count(const dyn_rset_t* s);

int dyn_rset_at(const dyn_rset_t* s, uint32_t i, double* lo, double* hi);

void dyn_rset_clear(dyn_rset_t* s);

int dyn_rset_complement(const dyn_rset_t* s, double lo, double hi,
    dyn_rset_t* out);

#ifdef __cplusplus
}
#endif

#endif
