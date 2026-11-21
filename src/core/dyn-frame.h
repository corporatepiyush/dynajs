#ifndef DYN_FRAME_H
#define DYN_FRAME_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DYN_FRAME_OK 0
#define DYN_FRAME_ETOOLONG (-1)
#define DYN_FRAME_ERANGE (-2)
#define DYN_FRAME_ESHORT (-3)
#define DYN_FRAME_ESTRIP (-4)
#define DYN_FRAME_ENOMEM (-5)
#define DYN_FRAME_EINVAL (-6)
#define DYN_FRAME_ENEG (-7)
#define DYN_FRAME_EOVER (-8)

#define DYN_FRAME_MAX_LENGTH (((uint64_t)1) << 53)
#define DYN_FRAME_MAX_DELIMS 64
#define DYN_FRAME_MAX_DELIM_LEN 4096

typedef struct dyn_frame dyn_frame_t;

typedef void (*dyn_frame_sink)(void* ud, int ev, const uint8_t* p, uint64_t n);

typedef struct {
    uint64_t offset;
    int width;
    int little_endian;
    int64_t adjustment;
    uint64_t strip;
    uint64_t max_frame;
    int fail_fast;
    int stop_on_error;
    int reject_unsafe;
    int resume_after_error;
} dyn_frame_length_cfg;

typedef struct {
    const uint8_t* const* delims;
    const size_t* lens;
    size_t count;
    int strip_delimiter;
    uint64_t max_frame;
    int fail_fast;
    int stop_on_error;
    int hold_overlapping;
    int resume_after_error;
} dyn_frame_delim_cfg;

dyn_frame_t* dyn_frame_new_length(const dyn_frame_length_cfg* cfg, int* err);
dyn_frame_t* dyn_frame_new_delim(const dyn_frame_delim_cfg* cfg, int* err);
dyn_frame_t* dyn_frame_new_fixed(uint64_t frame_len, int* err);
int dyn_frame_feed(dyn_frame_t* d, const uint8_t* data, size_t n, dyn_frame_sink sink, void* ud);
int dyn_frame_finish(dyn_frame_t* d, dyn_frame_sink sink, void* ud);
int dyn_frame_error(const dyn_frame_t* d);
int dyn_frame_line_mode(const dyn_frame_t* d);
int dyn_frame_stalled(const dyn_frame_t* d);
size_t dyn_frame_buffered(const dyn_frame_t* d);
uint64_t dyn_frame_copied(const dyn_frame_t* d);
const char* dyn_frame_strerror(int err);
int dyn_frame_describe(const dyn_frame_t* d, int ev, uint64_t v, char* out, size_t cap);
void dyn_frame_free(dyn_frame_t* d);

#ifdef __cplusplus
}
#endif

#endif
