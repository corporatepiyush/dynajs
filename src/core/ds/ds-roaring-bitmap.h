#ifndef DS_ROARING_BITMAP_H
#define DS_ROARING_BITMAP_H

#include <stddef.h>
#include <stdint.h>
#include "ds-common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DYN_ROAR_ARR 1
#define DYN_ROAR_BMP 2
#define DYN_ROAR_FULL 3
#define DYN_ROAR_ARR_MAX 4096u
#define DYN_ROAR_WORDS 1024
#define DYN_ROAR_AND 0
#define DYN_ROAR_OR 1
#define DYN_ROAR_XOR 2
#define DYN_ROAR_ANDNOT 3

typedef struct {
    uint16_t key;
    uint8_t type;
    uint32_t card;
    uint32_t cap;
    void* data;
} dyn_roar_ct;

typedef struct {
    dyn_roar_ct* c;
    uint32_t n, cap;
} dyn_roar_t;

void dyn_roar_clear(dyn_roar_t* r);

int dyn_roar_reserve(dyn_roar_t* r, uint32_t need);

uint32_t dyn_roar_popcnt(const uint64_t* w);

int dyn_roar_add(dyn_roar_t* r, uint32_t x);

int dyn_roar_remove(dyn_roar_t* r, uint32_t x);

int dyn_roar_has(const dyn_roar_t* r, uint32_t x);

int dyn_roar_range(dyn_roar_t* r, uint64_t lo, uint64_t hi, int add);

uint64_t dyn_roar_card(const dyn_roar_t* r);

int dyn_roar_op(dyn_roar_t* a, const dyn_roar_t* b, int op);

int dyn_roar_or_many(dyn_roar_t* out, const dyn_roar_t* const* in, uint32_t n);

uint64_t dyn_roar_op_card(const dyn_roar_t* a, const dyn_roar_t* b, int op);

uint64_t dyn_roar_rank(const dyn_roar_t* r, uint32_t x);

int dyn_roar_select(const dyn_roar_t* r, uint64_t i, uint32_t* out);

void dyn_roar_fill(const dyn_roar_t* r, uint32_t* out);

int dyn_roar_clone(dyn_roar_t* dst, const dyn_roar_t* src);

int dyn_roar_u32_cmp(const void* a, const void* b);

#ifdef __cplusplus
}
#endif

#endif
