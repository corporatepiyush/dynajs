#ifndef DYN_COMPRESS_H
#define DYN_COMPRESS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DYN_MAX_OUTPUT ((size_t)1 << 30)

typedef struct {
    uint8_t* buf;
    size_t len;
    size_t cap;
    size_t limit;
    int exceeded;
} dyn_outbuf_t;

int dyn_gzip_build(const uint8_t* src, size_t src_len,
    uint8_t** out, size_t* out_len);

typedef struct {
    int32_t* head;
    uint16_t* prev;
    size_t head_cap;
    size_t prev_cap;
    uint32_t base;
} dyn_comp_ctx_t;

int dyn_comp_ctx_init(dyn_comp_ctx_t* c);
void dyn_comp_ctx_free(dyn_comp_ctx_t* c);

int dyn_gzip_build_ctx(const uint8_t* src, size_t src_len, int level,
    dyn_comp_ctx_t* cx, uint8_t** out, size_t* out_len);

int dyn_gunzip_decode(const uint8_t* src, size_t len, dyn_outbuf_t* o);

int dyn_raw_deflate(const uint8_t* src, size_t src_len, int level,
    dyn_comp_ctx_t* cx, uint8_t** out, size_t* out_len);
int dyn_raw_inflate(const uint8_t* src, size_t len, dyn_outbuf_t* o);

int dyn_lz4_compress(const uint8_t* src, size_t len,
    const uint8_t* dict, size_t dict_len, int level,
    dyn_comp_ctx_t* cx, uint8_t** pout, size_t* pout_len);

int dyn_lz4_decompress(const uint8_t* src, size_t len,
    const uint8_t* dict, size_t dict_len, dyn_outbuf_t* o);

int dyn_lz4_frame_build(const uint8_t* src, size_t len, int level,
    int content_checksum, dyn_comp_ctx_t* cx,
    uint8_t** pout, size_t* pout_len);

int dyn_lz4_frame_decode(const uint8_t* src, size_t len, dyn_outbuf_t* o);

#define DYN_LZ4F_OK 0
#define DYN_LZ4F_ERR_TRUNC (-1)
#define DYN_LZ4F_ERR_MAGIC (-2)
#define DYN_LZ4F_ERR_VERSION (-3)
#define DYN_LZ4F_ERR_BLOCK_MAX (-4)
#define DYN_LZ4F_ERR_LINKED (-5)
#define DYN_LZ4F_ERR_HDR_SUM (-6)
#define DYN_LZ4F_ERR_BLOCK_CAP (-7)
#define DYN_LZ4F_ERR_BLOCK (-8)
#define DYN_LZ4F_ERR_BLOCK_SUM (-9)
#define DYN_LZ4F_ERR_CONT_SUM (-10)
#define DYN_LZ4F_ERR_CONT_SIZE (-11)
#define DYN_LZ4F_ERR_OOM (-12)
#define DYN_LZ4F_ERR_TRAILING (-13)

const char* dyn_lz4_frame_reason(int rc);

#ifdef __cplusplus
}
#endif

#endif
