#include <stdlib.h>
#include "dd.h"

void dd_arena_init(dd_arena* a, dd_db* db)
{
    memset(a, 0, sizeof *a);
    a->db = db;
}

static dd_chunk* dd_chunk_new(dd_arena* a, size_t cap)
{
    dd_chunk* c = calloc(1, sizeof *c);
    if (!c)
        return NULL;
    c->data = malloc(cap);
    if (!c->data) {
        free(c);
        return NULL;
    }
    c->owns_data = 1;
    c->cap = cap;
    c->next = a->head;
    if (a->head)
        a->head->prev = c;
    a->head = c;
    a->nchunks++;
    return c;
}

static void dd_chunk_unlink(dd_arena* a, dd_chunk* c)
{
    if (c->prev)
        c->prev->next = c->next;
    else
        a->head = c->next;
    if (c->next)
        c->next->prev = c->prev;
    c->next = c->prev = NULL;
    a->nchunks--;
    if (a->cur == c)
        a->cur = NULL;
}

const uint8_t* dd_arena_put(dd_arena* a, const uint8_t* p, size_t n, dd_chunk** out_chunk)
{
    dd_chunk* c = a->cur;
    uint8_t* dst;
    if (n > DD_CHUNK_SIZE / 4) {
        c = dd_chunk_new(a, n);
        if (!c)
            return NULL;
    } else if (!c || c->cap - c->used < n) {
        c = dd_chunk_new(a, DD_CHUNK_SIZE);
        if (!c)
            return NULL;
        if (a->cur && a->cur->live == 0) {
            dd_chunk* old = a->cur;
            dd_chunk_unlink(a, old);
            dd_db_retire(a->db, old);
        }
        a->cur = c;
    }
    dst = c->data + c->used;
    memcpy(dst, p, n);
    c->used += n;
    c->live += n;
    a->bytes += n;
    *out_chunk = c;
    return dst;
}

dd_chunk* dd_arena_adopt(dd_arena* a, uint8_t* data, size_t n)
{
    dd_chunk* c = calloc(1, sizeof *c);
    if (!c)
        return NULL;
    c->data = data;
    c->owns_data = 1;
    c->cap = n;
    c->used = n;
    c->live = n;
    c->next = a->head;
    if (a->head)
        a->head->prev = c;
    a->head = c;
    a->nchunks++;
    a->bytes += n;
    return c;
}

void dd_arena_release(dd_arena* a, dd_chunk* c, size_t n)
{
    c->live -= n;
    a->bytes -= n;
    if (c->live == 0 && c != a->cur) {
        dd_chunk_unlink(a, c);
        dd_db_retire(a->db, c);
    }
}

void dd_arena_drop_all(dd_arena* a)
{
    while (a->head) {
        dd_chunk* c = a->head;
        dd_chunk_unlink(a, c);
        dd_db_retire(a->db, c);
    }
    a->cur = NULL;
    a->bytes = 0;
}
