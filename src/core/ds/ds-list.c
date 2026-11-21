#include "ds-internal.h"
#include "ds-list.h"

void dyn_list_init(dyn_list_t* l, dyn_cell_free_fn release, void* ud)
{
    l->head = l->tail = NULL;
    l->length = 0;
    l->release = release;
    l->ud = ud;
}

void dyn_list_free(dyn_list_t* l)
{
    dyn_list_node_t *n, *next;
    for (n = l->head; n; n = next) {
        next = n->next;
        if (l->release)
            l->release(l->ud, &n->value);
        DYN_DS_FREE(n);
    }
    l->head = l->tail = NULL;
    l->length = 0;
}

int dyn_list_push_front(dyn_list_t* l, const dyn_cell_t* v)
{
    dyn_list_node_t* n = (dyn_list_node_t*)DYN_DS_MALLOC(sizeof(*n));
    if (!n)
        return -1;
    n->value = *v;
    n->prev = NULL;
    n->next = l->head;
    if (l->head)
        l->head->prev = n;
    else
        l->tail = n;
    l->head = n;
    l->length++;
    return 0;
}

int dyn_list_push_back(dyn_list_t* l, const dyn_cell_t* v)
{
    dyn_list_node_t* n = (dyn_list_node_t*)DYN_DS_MALLOC(sizeof(*n));
    if (!n)
        return -1;
    n->value = *v;
    n->next = NULL;
    n->prev = l->tail;
    if (l->tail)
        l->tail->next = n;
    else
        l->head = n;
    l->tail = n;
    l->length++;
    return 0;
}

int dyn_list_pop_front(dyn_list_t* l, dyn_cell_t* out)
{
    dyn_list_node_t* n = l->head;
    if (!n)
        return -1;
    l->head = n->next;
    if (l->head)
        l->head->prev = NULL;
    else
        l->tail = NULL;
    l->length--;
    *out = n->value;
    DYN_DS_FREE(n);
    return 0;
}

int dyn_list_pop_back(dyn_list_t* l, dyn_cell_t* out)
{
    dyn_list_node_t* n = l->tail;
    if (!n)
        return -1;
    l->tail = n->prev;
    if (l->tail)
        l->tail->next = NULL;
    else
        l->head = NULL;
    l->length--;
    *out = n->value;
    DYN_DS_FREE(n);
    return 0;
}
