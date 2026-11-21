#ifndef DS_INDEXED_HEAP_H
#define DS_INDEXED_HEAP_H

#include <stddef.h>
#include <stdint.h>
#include "ds-common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    double p;
    uint32_t id;
} dyn_iheap_ent;

typedef struct {
    dyn_iheap_ent* h;
    uint32_t n, cap;
    int max;
    dyn_ds_imap pos;
} dyn_iheap_t;

int dyn_iheap_push(dyn_iheap_t* q, uint32_t id, double p);

void dyn_iheap_remove_at(dyn_iheap_t* q, uint32_t i);

void dyn_iheap_init(dyn_iheap_t* q, int max);

void dyn_iheap_free(dyn_iheap_t* q);

#ifdef __cplusplus
}
#endif

#endif
