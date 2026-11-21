#ifndef DS_LRU_H
#define DS_LRU_H

#include <stddef.h>
#include <stdint.h>
#include "ds-common.h"
#include "ds-cell.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DYN_LRU_MAX_BUCKETS (1u << 24)

typedef struct dyn_lru_node {
    struct dyn_lru_node *prev, *next;
    struct dyn_lru_node* hnext;
    dyn_cell_t value;
    uint64_t hash;
    uint64_t expires;
    size_t klen;
    char key[];
} dyn_lru_node_t;

typedef struct {
    dyn_lru_node_t** buckets;
    dyn_lru_node_t *head, *tail;
    uint64_t default_ttl;
    uint64_t hits, misses, evictions, expired;
    uint32_t n_buckets;
    uint32_t count;
    uint32_t cap;
    dyn_cell_free_fn release;
    void* ud;
} dyn_lru_t;

int dyn_lru_init(dyn_lru_t* L, uint32_t cap, dyn_cell_free_fn release, void* ud);

void dyn_lru_free(dyn_lru_t* L);

uint64_t dyn_lru_now_ms(void);

uint64_t dyn_lru_expiry(uint64_t ttl);

int dyn_lru_expired(const dyn_lru_node_t* n, uint64_t now);

dyn_lru_node_t* dyn_lru_find(const dyn_lru_t* L, const char* key, size_t klen);

void dyn_lru_touch(dyn_lru_t* L, dyn_lru_node_t* n);

void dyn_lru_detach(dyn_lru_t* L, dyn_lru_node_t* n);

int dyn_lru_set(dyn_lru_t* L, const char* key, size_t klen, const dyn_cell_t* v, uint64_t ttl, dyn_lru_node_t** evicted);

void dyn_lru_node_free(dyn_lru_t* L, dyn_lru_node_t* n);

dyn_cell_t dyn_lru_node_take(dyn_lru_node_t* n);

#ifdef __cplusplus
}
#endif

#endif
