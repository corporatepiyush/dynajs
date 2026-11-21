#ifndef DS_PATRICIA_H
#define DS_PATRICIA_H

#include <stddef.h>
#include <stdint.h>
#include "ds-common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dyn_patricia dyn_patricia_t;

typedef int (*dyn_patricia_visit)(const uint8_t* key, uint32_t bits, void* val, void* ud);

dyn_patricia_t* dyn_patricia_new(const dyn_ds_ops* val);

void dyn_patricia_free(dyn_patricia_t* t);

size_t dyn_patricia_size(const dyn_patricia_t* t);

int dyn_patricia_put(dyn_patricia_t* t, const uint8_t* key, uint32_t bits, const void* val);

typedef struct {
    void** path;
    uint32_t n, cap;
    uint8_t* key;
    uint32_t kbits, kcap;
} dyn_patricia_cursor;

int dyn_patricia_put_cursor(dyn_patricia_t* t, dyn_patricia_cursor* cur, const uint8_t* key, uint32_t bits, const void* val);

void dyn_patricia_cursor_free(dyn_patricia_cursor* cur);

void* dyn_patricia_get(const dyn_patricia_t* t, const uint8_t* key, uint32_t bits);

void* dyn_patricia_longest_prefix(const dyn_patricia_t* t, const uint8_t* key, uint32_t bits, uint32_t* matched_bits);

int dyn_patricia_remove(dyn_patricia_t* t, const uint8_t* key, uint32_t bits);

int dyn_patricia_walk(const dyn_patricia_t* t, const uint8_t* prefix, uint32_t bits, dyn_patricia_visit fn, void* ud);

#ifdef __cplusplus
}
#endif

#endif
