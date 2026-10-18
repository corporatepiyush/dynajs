#ifndef DYN_DICT_H
#define DYN_DICT_H

#include <stddef.h>
#include <stdint.h>

#include "dyn-compress.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DYN_DICT_MAX_PHRASES 65535

typedef struct dyn_dict dyn_dict_t;

dyn_dict_t* dyn_dict_new(const uint8_t* const* phrases, const size_t* lens,
    size_t n);

void dyn_dict_free(dyn_dict_t* d);

uint32_t dyn_dict_id(const dyn_dict_t* d);

size_t dyn_dict_count(const dyn_dict_t* d);

int dyn_dict_compress(dyn_dict_t* d, const uint8_t* src, size_t len,
    dyn_outbuf_t* o);

int dyn_dict_decompress(const dyn_dict_t* d, const uint8_t* src, size_t len,
    dyn_outbuf_t* o);

int dyn_dict_record_id(const uint8_t* src, size_t len, uint32_t* id);

#ifdef __cplusplus
}
#endif

#endif
