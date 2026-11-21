#ifndef DS_HYPERLOGLOG_H
#define DS_HYPERLOGLOG_H

#include <stddef.h>
#include <stdint.h>
#include "ds-common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dyn_hll dyn_hll_t;

#define DYN_HLL_MIN_PRECISION 4
#define DYN_HLL_MAX_PRECISION 18

dyn_hll_t* dyn_hll_new(uint32_t precision);

void dyn_hll_free(dyn_hll_t* h);

void dyn_hll_add(dyn_hll_t* h, const char* k, size_t n);

void dyn_hll_add_hash(dyn_hll_t* h, uint64_t hash);

double dyn_hll_count(dyn_hll_t* h);

uint32_t dyn_hll_precision(const dyn_hll_t* h);

uint32_t dyn_hll_registers(const dyn_hll_t* h);

int dyn_hll_merge(dyn_hll_t* a, const dyn_hll_t* b);

const uint8_t* dyn_hll_data(const dyn_hll_t* h);

uint8_t* dyn_hll_data_mut(dyn_hll_t* h);

int dyn_hll_regs_valid(const dyn_hll_t* h);
uint8_t* dyn_hll_swap_regs(dyn_hll_t* h, uint8_t* reg);

#ifdef __cplusplus
}
#endif

#endif
