/*
 * ac_cand.h -- pinned REFERENCE Aho-Corasick for the bench differential.
 *
 * A pure double-array automaton with the same observable contract as
 * src/core/dyn-ac.h (dyn_*_new/insert/build/run, same emit order, same
 * "first pattern wins" duplicates, same -1 refusal at DYN_AC_MAX_STATES).
 * It exists so `tests/bench_ac.c --diff` can compare the ENGINE's automaton
 * against this independent implementation in one binary over a systematic
 * corpus; the engine's compact materialization was validated against it the
 * same way. It is a bench asset: nothing in src/ links it.
 */
#ifndef AC_CAND_H
#define AC_CAND_H

#include <stddef.h>
#include <stdint.h>

#include "dyn-ac.h"     /* DYN_AC_MAX_STATES, dyn_ac_emit_t */

typedef struct { uint32_t next; uint32_t check; } da_slot;

typedef struct { int32_t out; int32_t out_link; } da_oo;

typedef struct dyn_ac_cand {
    /* placed automaton (steady state) */
    da_slot *tab;       /* transition table: slot = base[s] + byte */
    uint32_t *base;     /* per-state row base; base[0] == 0 always (root) */
    int32_t *fail;      /* suffix links */
    da_oo *oo;          /* out / out_link pairs: one 8 B load per byte */
    size_t n_states, cap_states;
    size_t tab_cap;     /* slot count (power-of-two-ish growth, >= 256) */

    /* trie under construction (freed at build) */
    int32_t *child_head;    /* n_states: head edge of child list */
    uint8_t *e_byte;        /* edges: byte, target, next-sibling */
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

#endif /* AC_CAND_H */
