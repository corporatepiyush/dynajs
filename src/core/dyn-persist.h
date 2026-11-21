#ifndef DYN_PERSIST_H
#define DYN_PERSIST_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DYN_PERSIST_VERSION 1u
#define DYN_PERSIST_SUPER 64u
#define DYN_PERSIST_HDR 160u
#define DYN_PERSIST_SEC 40u
#define DYN_PERSIST_OV 16u
#define DYN_PERSIST_MAX_SECTIONS 4096u
#define DYN_PERSIST_MAX_OVERLAYS (1u << 24)
#define DYN_PERSIST_NPARAMS 8
#define DYN_PERSIST_PAGE 16384u
#define DYN_PERSIST_ALIGN 64u
#define DYN_PERSIST_MAX_ELEM 65536u
#define DYN_PERSIST_ENDIAN_TAG 0x01020304u

#define DYN_PERSIST_S_PAGE 1u
#define DYN_PERSIST_S_KNOWN 1u

#define DYN_PERSIST_OK 0
#define DYN_PERSIST_E_IO -1
#define DYN_PERSIST_E_NOMEM -2
#define DYN_PERSIST_E_MAGIC -3
#define DYN_PERSIST_E_VERSION -4
#define DYN_PERSIST_E_ENDIAN -5
#define DYN_PERSIST_E_TRUNCATED -6
#define DYN_PERSIST_E_CORRUPT -7
#define DYN_PERSIST_E_CHECKSUM -8
#define DYN_PERSIST_E_TYPE -9
#define DYN_PERSIST_E_ARG -10
#define DYN_PERSIST_E_TOO_LARGE -11
#define DYN_PERSIST_E_MISSING -12

#define DYN_PERSIST_MODE_COPY 0
#define DYN_PERSIST_MODE_MMAP 1

typedef struct {
    int code;
    int sys_errno;
    char msg[240];
} dyn_persist_err;

typedef struct {
    uint32_t kind, tag, elem, flags;
    const void* p;
    size_t boff;
    uint64_t len, off;
    uint32_t crc;
    const uint64_t* dirty;
    uint32_t dirty_shift;
} dyn_persist_sect;

typedef struct dyn_persist_w {
    uint8_t* buf;
    size_t len, cap;
    uint8_t* xbuf;
    size_t xcap;
    dyn_persist_sect* s;
    uint32_t ns, scap;
    void* iov;
    uint32_t iovcap;
    int open;
    int err;
    uint32_t type_id, flags;
    uint64_t count;
    uint64_t lineage, gen;
    uint64_t params[DYN_PERSIST_NPARAMS];
    uint64_t file_size;
    uint32_t slot_size;
    size_t hdr_off, hdr_len;
} dyn_persist_w;

typedef struct {
    int fsync;
    int incremental;
    int wrote_incremental;
    uint64_t bytes_written;
    uint64_t file_size;
} dyn_persist_save_opt;

typedef struct {
    uint32_t kind, tag, elem, flags;
    uint64_t off, len;
    uint32_t crc;
    uint32_t noverlay;
    const uint8_t* p;
} dyn_persist_rsect;

typedef struct {
    uint32_t sect, chunk;
    uint64_t src;
} dyn_persist_ov;

typedef struct dyn_persist_r {
    uint8_t* base;
    size_t size;
    int mode;
    int fd;
    uint32_t type_id, flags, nsect, page, slot, slot_size;
    uint64_t count, lineage, gen, file_size, base_end;
    uint64_t params[DYN_PERSIST_NPARAMS];
    dyn_persist_rsect* s;
    dyn_persist_ov* ov;
    uint32_t nov;
    int refs;
} dyn_persist_r;

void dyn_persist_w_init(dyn_persist_w* w);
void dyn_persist_w_reset(dyn_persist_w* w);
void dyn_persist_w_free(dyn_persist_w* w);
dyn_persist_w* dyn_persist_w_thread(void);

int dyn_persist_w_begin(dyn_persist_w* w, uint32_t type_id, uint64_t count);
void dyn_persist_w_param(dyn_persist_w* w, unsigned i, uint64_t v);
int dyn_persist_w_ref(dyn_persist_w* w, uint32_t kind, uint32_t tag,
    uint32_t elem, uint32_t flags, const void* p, uint64_t len);
int dyn_persist_w_ref_dirty(dyn_persist_w* w, uint32_t kind, uint32_t tag,
    uint32_t elem, uint32_t flags, const void* p, uint64_t len,
    const uint64_t* dirty, uint32_t dirty_shift);
int dyn_persist_w_open(dyn_persist_w* w, uint32_t kind, uint32_t tag,
    uint32_t elem, uint32_t flags);
int dyn_persist_w_put(dyn_persist_w* w, const void* p, size_t n);
int dyn_persist_w_close(dyn_persist_w* w);

int dyn_persist_w_layout(dyn_persist_w* w);
int dyn_persist_w_image(dyn_persist_w* w, uint8_t** out, size_t* len);
int dyn_persist_save(dyn_persist_w* w, const char* path,
    dyn_persist_save_opt* o, dyn_persist_err* e);

int dyn_persist_validate(const uint8_t* buf, size_t size, int verify,
    uint32_t expect_type, dyn_persist_r* r, dyn_persist_err* e);
void dyn_persist_r_clear(dyn_persist_r* r);
int dyn_persist_open(const char* path, int mode, int verify,
    uint32_t expect_type, dyn_persist_r** out, dyn_persist_err* e);
int dyn_persist_from_bytes(const uint8_t* buf, size_t size, int verify,
    uint32_t expect_type, dyn_persist_r** out, dyn_persist_err* e);
void dyn_persist_retain(dyn_persist_r* r);
void dyn_persist_release(dyn_persist_r* r);
const dyn_persist_rsect* dyn_persist_find(const dyn_persist_r* r,
    uint32_t kind, uint32_t tag);
int dyn_persist_map_section(const dyn_persist_r* r,
    const dyn_persist_rsect* s, void* addr, size_t cap);
size_t dyn_persist_host_page(void);

uint64_t dyn_persist_chunks(uint64_t len, uint32_t shift);
void dyn_persist_chunk_diff(const void* p, uint64_t len, uint32_t shift,
    uint64_t* hashes, int valid, uint64_t* dirty);

static inline void dyn_persist_dirty_mark(uint64_t* bits, uint32_t shift,
    uint64_t byte_off)
{
    uint64_t c = byte_off >> shift;
    bits[c >> 6] |= (uint64_t)1 << (c & 63);
}

static inline void dyn_persist_dirty_mark_range(uint64_t* bits, uint32_t shift,
    uint64_t byte_off, uint64_t len)
{
    uint64_t c, last;
    if (!len)
        return;
    last = (byte_off + len - 1) >> shift;
    for (c = byte_off >> shift; c <= last; c++)
        bits[c >> 6] |= (uint64_t)1 << (c & 63);
}

const char* dyn_persist_strerror(int code);
const char* dyn_persist_code_name(int code);

#ifdef DYN_PERSIST_FAILPOINTS
extern int dyn_persist_fp_countdown;
extern int dyn_persist_fp_torn;
extern int dyn_persist_fp_hits;
extern int dyn_persist_fp_powerloss;
extern int dyn_persist_fp_fullsyncs;
#endif

#ifdef __cplusplus
}
#endif

#endif
