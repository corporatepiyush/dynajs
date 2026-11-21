#ifndef DS_CONSISTENT_HASH_H
#define DS_CONSISTENT_HASH_H

#include <stddef.h>
#include <stdint.h>
#include "ds-common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DYN_CHASH_MAX_REPLICAS 1000u
#define DYN_CHASH_MAX_WEIGHT 1000u
#define DYN_CHASH_MAX_POINTS (1u << 22)

typedef struct {
    char* name;
    uint32_t len;
    uint32_t weight;
} dyn_chash_node;

typedef struct {
    uint64_t h;
    uint32_t node;
} dyn_chash_pt;

typedef struct {
    dyn_chash_node* nodes;
    uint32_t nn, ncap;
    dyn_chash_pt* ring;
    uint32_t nr;
    uint32_t replicas;
    dyn_ds_smap idx;
    uint32_t* jt;
    uint32_t jbits;
    uint8_t jon, jdirty;
} dyn_chash_t;

uint32_t dyn_chash_locate(const dyn_chash_t* c, uint64_t h);

uint32_t dyn_chash_find(dyn_chash_t* c, uint64_t h);

void dyn_chash_free(dyn_chash_t* c);

int dyn_chash_set_node(dyn_chash_t* c, const char* name, size_t n, uint32_t weight);

int dyn_chash_append_node(dyn_chash_t* c, const char* name, size_t n, uint32_t weight);

int dyn_chash_commit(dyn_chash_t* c);

int dyn_chash_remove_node(dyn_chash_t* c, const char* name, size_t n);

void dyn_chash_init(dyn_chash_t* c, uint32_t replicas);

#ifdef __cplusplus
}
#endif

#endif
