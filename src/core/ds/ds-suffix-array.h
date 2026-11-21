#ifndef DS_SUFFIX_ARRAY_H
#define DS_SUFFIX_ARRAY_H

#include <stddef.h>
#include <stdint.h>
#include "ds-common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dyn_sa dyn_sa_t;

dyn_sa_t* dyn_sa_build(const uint8_t* text, size_t n);

void dyn_sa_free(dyn_sa_t* sa);

int dyn_sa_bucket(dyn_sa_t* sa);

size_t dyn_sa_len(const dyn_sa_t* sa);

const uint32_t* dyn_sa_array(const dyn_sa_t* sa);

const uint8_t* dyn_sa_text(const dyn_sa_t* sa);

const uint32_t* dyn_sa_lcp(dyn_sa_t* sa);

size_t dyn_sa_find(const dyn_sa_t* sa, const uint8_t* pat, size_t m, size_t* first);

size_t dyn_sa_longest_repeat(dyn_sa_t* sa, size_t* pos);

#ifdef __cplusplus
}
#endif

#endif
