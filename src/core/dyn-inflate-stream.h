#ifndef DYN_INFLATE_STREAM_H
#define DYN_INFLATE_STREAM_H

#include <stddef.h>
#include <stdint.h>

#define DYN_INFS_MORE 0
#define DYN_INFS_END 1
#define DYN_INFS_FULL 2
#define DYN_INFS_BAD (-1)
#define DYN_INFS_OOM (-2)

typedef struct dyn_infs dyn_infs_t;

dyn_infs_t* dyn_infs_new(void);
void dyn_infs_free(dyn_infs_t* z);
int dyn_infs_run(dyn_infs_t* z, const uint8_t* in, size_t n, size_t* used, const uint8_t** out, size_t* outn);
size_t dyn_infs_tail(dyn_infs_t* z, uint8_t* buf);

#endif
