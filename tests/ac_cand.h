#ifndef AC_CAND_H
#define AC_CAND_H

#include <stddef.h>
#include <stdint.h>

#include "dyn-ac.h"

typedef struct { uint32_t next; uint32_t check; } da_slot;

typedef struct { int32_t out; int32_t out_link; } da_oo;

typedef struct dyn_ac_cand {
    da_slot *tab;
    uint32_t *base;
    int32_t *fail;
    da_oo *oo;
    size_t n_states, cap_states;
    size_t tab_cap;

    int32_t *child_head;
    uint8_t *e_byte;
    int32_t *e_target;
    int32_t *e_next;
    size_t n_edges, cap_edges;

    size_t *plen;
    size_t n_pat;
} dyn_ac_cand_t;

dyn_ac_cand_t *dyn_da_new(size_t n_pat);
void dyn_da_free(dyn_ac_cand_t *a);
int dyn_da_insert(dyn_ac_cand_t *a, const uint8_t *p, size_t len, int idx);
int dyn_da_build(dyn_ac_cand_t *a);
int dyn_da_run(const dyn_ac_cand_t *a, const uint8_t *t, size_t tlen,
               dyn_ac_emit_t emit, void *ud);
size_t dyn_da_states(const dyn_ac_cand_t *a);
size_t dyn_da_bytes(const dyn_ac_cand_t *a);

#endif
