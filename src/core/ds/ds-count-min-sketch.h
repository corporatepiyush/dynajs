#ifndef DS_COUNT_MIN_SKETCH_H
#define DS_COUNT_MIN_SKETCH_H

#include <stddef.h>
#include <stdint.h>
#include "ds-common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dyn_cms dyn_cms_t;

dyn_cms_t* dyn_cms_new(uint32_t width, uint32_t depth);

void dyn_cms_free(dyn_cms_t* s);

void dyn_cms_add(dyn_cms_t* s, const char* k, size_t n, uint64_t count);

uint64_t dyn_cms_count(const dyn_cms_t* s, const char* k, size_t n);

uint64_t dyn_cms_total(const dyn_cms_t* s);

uint32_t dyn_cms_width(const dyn_cms_t* s);

uint32_t dyn_cms_depth(const dyn_cms_t* s);

int dyn_cms_merge(dyn_cms_t* a, const dyn_cms_t* b);

const uint64_t* dyn_cms_counters(const dyn_cms_t* s);

uint64_t* dyn_cms_counters_mut(dyn_cms_t* s);

void dyn_cms_set_total(dyn_cms_t* s, uint64_t total);
uint64_t* dyn_cms_swap_counters(dyn_cms_t* s, uint64_t* c);

#ifdef __cplusplus
}
#endif

#endif
