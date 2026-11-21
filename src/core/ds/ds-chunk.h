#ifndef DS_CHUNK_H
#define DS_CHUNK_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "ds-internal.h"

#define DYN_CHUNK_NIL UINT32_MAX

typedef struct {
    uint8_t* c0;
    uint8_t** ch;
    size_t esz;
    uint32_t nch, tcap, len, cap, shift, first, step, freeh;
} dyn_chunk_t;

static inline void dyn_chunk_init(dyn_chunk_t* a, size_t esz, uint32_t shift, uint32_t first, uint32_t step)
{
    memset(a, 0, sizeof(*a));
    a->esz = esz;
    a->shift = shift;
    a->first = first;
    a->step = step;
    a->freeh = DYN_CHUNK_NIL;
}

static inline uint8_t* dyn_chunk_at(const dyn_chunk_t* a, uint32_t id)
{
    uint32_t k = id >> a->shift;
    return (k ? a->ch[k] : a->c0) + (size_t)(id & ((1u << a->shift) - 1)) * a->esz;
}

static inline uint32_t dyn_chunk_alloc(dyn_chunk_t* a)
{
    uint32_t id = a->freeh;
    if (id != DYN_CHUNK_NIL) {
        memcpy(&a->freeh, dyn_chunk_at(a, id), sizeof(uint32_t));
        return id;
    }
    id = a->len;
    if (id >= 0xF0000000u)
        return DYN_CHUNK_NIL;
    if (id == a->cap) {
        uint32_t full = 1u << a->shift, k = a->cap >> a->shift;
        size_t ents = a->cap < full ? (a->cap ? (size_t)a->cap + (a->cap < a->step ? a->cap : a->step) : a->first) : full;
        uint8_t* c;
        if (ents > full)
            ents = full;
        if (k == a->nch) {
            if (k == a->tcap) {
                uint32_t nc = a->tcap ? (a->tcap > 0x7FFFFFFFu ? 0xF0000000u : a->tcap * 2) : 4;
                uint8_t** nt = (uint8_t**)DYN_DS_REALLOC(a->ch, sizeof(uint8_t*) * nc);
                if (!nt)
                    return DYN_CHUNK_NIL;
                a->ch = nt;
                a->tcap = nc;
            }
            a->ch[k] = NULL;
            a->nch = k + 1;
        }
        c = (uint8_t*)DYN_DS_REALLOC(a->ch[k], ents * a->esz);
        if (!c)
            return DYN_CHUNK_NIL;
        a->ch[k] = c;
        if (!k)
            a->c0 = c;
        a->cap = k ? a->cap + full : (uint32_t)ents;
    }
    a->len++;
    return id;
}

static inline void dyn_chunk_release(dyn_chunk_t* a, uint32_t id)
{
    memcpy(dyn_chunk_at(a, id), &a->freeh, sizeof(uint32_t));
    a->freeh = id;
}

static inline size_t dyn_chunk_bytes(const dyn_chunk_t* a)
{
    return (size_t)a->cap * a->esz + (size_t)a->tcap * sizeof(uint8_t*);
}

static inline void dyn_chunk_destroy(dyn_chunk_t* a)
{
    uint32_t k;
    for (k = 0; k < a->nch; k++)
        DYN_DS_FREE(a->ch[k]);
    DYN_DS_FREE(a->ch);
    dyn_chunk_init(a, a->esz, a->shift, a->first, a->step);
}

#endif
