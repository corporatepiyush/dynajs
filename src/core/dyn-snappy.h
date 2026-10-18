#ifndef DYN_SNAPPY_H
#define DYN_SNAPPY_H

#include <stddef.h>
#include <stdint.h>

#include "dyn-compress.h"

#ifdef __cplusplus
extern "C" {
#endif

int dyn_snappy_compress(const uint8_t* src, size_t len,
    uint8_t** pout, size_t* pout_len);

int dyn_snappy_decompress(const uint8_t* src, size_t len, size_t cap,
    dyn_outbuf_t* o);

#ifdef __cplusplus
}
#endif

#endif