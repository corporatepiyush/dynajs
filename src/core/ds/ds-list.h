#ifndef DS_LIST_H
#define DS_LIST_H

#include <stddef.h>
#include <stdint.h>
#include "ds-common.h"
#include "ds-cell.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dyn_list_node {
    struct dyn_list_node *prev, *next;
    dyn_cell_t value;
} dyn_list_node_t;

typedef struct {
    dyn_list_node_t *head, *tail;
    uint32_t length;
    dyn_cell_free_fn release;
    void* ud;
} dyn_list_t;

void dyn_list_init(dyn_list_t* l, dyn_cell_free_fn release, void* ud);

void dyn_list_free(dyn_list_t* l);

int dyn_list_push_front(dyn_list_t* l, const dyn_cell_t* v);

int dyn_list_push_back(dyn_list_t* l, const dyn_cell_t* v);

int dyn_list_pop_front(dyn_list_t* l, dyn_cell_t* out);

int dyn_list_pop_back(dyn_list_t* l, dyn_cell_t* out);

#ifdef __cplusplus
}
#endif

#endif
