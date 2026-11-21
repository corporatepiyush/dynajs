#ifndef DS_COMMON_H
#define DS_COMMON_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void dyn_ds_hash_seed(uint64_t seed0, uint64_t seed1);

uint64_t dyn_ds_xxh64(const void* data, size_t len, uint64_t seed);

typedef struct dyn_ds_ops {
    size_t size;
    uint64_t (*hash)(const void* elem, void* ud);
    int (*eq)(const void* a, const void* b, void* ud);
    int (*cmp)(const void* a, const void* b, void* ud);
    void (*release)(void* elem, void* ud);
    void (*copy)(void* dst, const void* src, void* ud);
    void* ud;
} dyn_ds_ops;

#define DYN_DS_FIXED_SEED 0x5DB1E995C3A5C85CULL
#define DYN_DS_MAX_ITEMS (1u << 28)

typedef struct {
    uint64_t h;
    const char* k;
    uint32_t n;
    uint32_t used;
    uintptr_t v;
} dyn_ds_sent;

typedef struct {
    dyn_ds_sent* e;
    uint32_t cap, n;
} dyn_ds_smap;

typedef struct {
    uint32_t k;
    uint32_t used;
    uint32_t v;
} dyn_ds_ient;

typedef struct {
    dyn_ds_ient* e;
    uint32_t cap, n;
    uint64_t seed;
} dyn_ds_imap;

typedef int (*ii_tok_fn)(void* ud, const char* tok, size_t n);

int dyn_ds_cmp_f64(const void* a, const void* b, void* ud);

uint64_t dyn_ds_strhash(const char* p, size_t n);

void dyn_ds_smap_free(dyn_ds_smap* m);

dyn_ds_sent* dyn_ds_smap_find(const dyn_ds_smap* m, const char* k, size_t n, uint64_t h);

dyn_ds_sent* dyn_ds_smap_insert(dyn_ds_smap* m, const char* k, size_t n, uint64_t h, uintptr_t v);

void dyn_ds_smap_remove(dyn_ds_smap* m, dyn_ds_sent* s);

void dyn_ds_imap_init(dyn_ds_imap* m);

void dyn_ds_imap_free(dyn_ds_imap* m);

dyn_ds_ient* dyn_ds_imap_find(const dyn_ds_imap* m, uint32_t k);

uint8_t* dyn_ds_bytes_dup(const char* p, size_t n);

enum { DYN_SEG_SUM,
    DYN_SEG_MIN,
    DYN_SEG_MAX };

#ifdef __cplusplus
}
#endif

#endif
