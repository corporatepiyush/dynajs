#ifndef DS_INTERNER_H
#define DS_INTERNER_H

#include <stddef.h>
#include <stdint.h>
#include "ds-common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DYN_INTERNER_CHUNK (64u * 1024u)

typedef struct dyn_interner_chunk {
    struct dyn_interner_chunk* next;
    size_t used, cap;
    char data[];
} dyn_interner_chunk;

typedef struct {
    const char* p;
    uint32_t len;
} dyn_interner_str;

typedef struct {
    dyn_interner_chunk* chunks;
    dyn_interner_str* s;
    uint32_t n, cap;
    dyn_ds_smap idx;
    uint64_t bytes;
} dyn_interner_t;

int64_t dyn_interner_intern(dyn_interner_t* t, const char* p, size_t n, int create);

void dyn_interner_free(dyn_interner_t* t);

#ifdef __cplusplus
}
#endif

#endif
