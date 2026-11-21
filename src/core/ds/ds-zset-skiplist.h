#ifndef DS_ZSET_SKIPLIST_H
#define DS_ZSET_SKIPLIST_H

#include <stddef.h>
#include <stdint.h>
#include "ds-common.h"
#include "ds-cell.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DYN_ZSL_MAX 32

typedef struct dyn_zsl_node dyn_zsl_node;

typedef struct {
    dyn_zsl_node* fwd;
    uint32_t span;
} dyn_zsl_level;

struct dyn_zsl_node {
    double score;
    dyn_cell_t val;
    dyn_zsl_node* back;
    char* key;
    uint32_t klen;
    uint32_t lvl;
    dyn_zsl_level lv[];
};

typedef struct {
    dyn_cell_free_fn release;
    void* ud;
    dyn_zsl_node* head;
    dyn_zsl_node* tail;
    uint32_t len;
    int level;
    uint64_t rng;
    dyn_ds_smap idx;
} dyn_zsl_t;

int dyn_zsl_cmp(const dyn_zsl_node* x, double s, const char* k, uint32_t n);

int dyn_zsl_init(dyn_zsl_t* s, dyn_cell_free_fn release, void* ud);

void dyn_zsl_link(dyn_zsl_t* s, dyn_zsl_node* x);

void dyn_zsl_unlink(dyn_zsl_t* s, dyn_zsl_node* x);

uint32_t dyn_zsl_rank(const dyn_zsl_t* s, const dyn_zsl_node* x);

dyn_zsl_node* dyn_zsl_at(const dyn_zsl_t* s, uint32_t rank);

dyn_zsl_node* dyn_zsl_lt(const dyn_zsl_t* s, double sc, const char* k, uint32_t n, int le, uint32_t* prank);

dyn_zsl_node* dyn_zsl_first_ge_score(const dyn_zsl_t* s, double sc, uint32_t* prank);

dyn_zsl_node* dyn_zsl_last_le_score(const dyn_zsl_t* s, double sc, uint32_t* prank);

dyn_zsl_node* dyn_zsl_find(const dyn_zsl_t* s, const char* k, size_t n);

void dyn_zsl_drop(dyn_zsl_t* s, dyn_zsl_node* x);

int dyn_zsl_insert(dyn_zsl_t* s, double score, const char* k, size_t n, const dyn_cell_t* val, dyn_zsl_node** out);

void dyn_zsl_clear(dyn_zsl_t* s);

void dyn_zsl_destroy(dyn_zsl_t* s);

#ifdef __cplusplus
}
#endif

#endif
