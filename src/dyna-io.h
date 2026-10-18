#ifndef DYNAJS_IO_H
#define DYNAJS_IO_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

enum {
    DYN_IOBUF_INLINE = 0,
    DYN_IOBUF_HEAP,
    DYN_IOBUF_MMAP
};

#define DYN_IOBUF_INLINE_CAP 88

typedef struct {
    uint8_t* data;
    size_t len;
    size_t rpos;
    size_t cap;
    uint8_t kind;
    uint8_t inln[DYN_IOBUF_INLINE_CAP];
} dyn_iobuf_t;

void dyn_iobuf_init(dyn_iobuf_t* b);

int dyn_iobuf_reserve(dyn_iobuf_t* b, size_t extra);

int dyn_iobuf_append(dyn_iobuf_t* b, const void* src, size_t n);

uint8_t* dyn_iobuf_tail(dyn_iobuf_t* b, size_t n);

int dyn_iobuf_commit(dyn_iobuf_t* b, size_t n);

static inline uint8_t* dyn_iobuf_rdata(const dyn_iobuf_t* b)
{
    return b->data + b->rpos;
}
static inline size_t dyn_iobuf_rlen(const dyn_iobuf_t* b)
{
    return b->len - b->rpos;
}

void dyn_iobuf_consume(dyn_iobuf_t* b, size_t n);

void dyn_iobuf_compact(dyn_iobuf_t* b);

int dyn_iobuf_ensure_nul(dyn_iobuf_t* b);

void dyn_iobuf_reset(dyn_iobuf_t* b);

void dyn_iobuf_free(dyn_iobuf_t* b);

#define DYN_SLURP_NUL 1
#define DYN_SLURP_MMAP 2

#define DYN_MAX_INPUT ((size_t)1 << 30)

int dyn_io_slurp(const char* path, dyn_iobuf_t* out, int flags, size_t maxsize);

int dyn_io_slurp_fd(int fd, dyn_iobuf_t* out, int flags, size_t maxsize);

int dyn_io_read_buf(const char* path, dyn_iobuf_t* out, int flags, size_t maxsize);

int dyn_io_write_whole_atomic(const char* path, const void* data, size_t len,
    int durable);

void dyn_io_advise_seq_read(int fd, off_t size);
size_t dyn_esc_ctrl(char* dst, size_t cap, const char* src);
int dyn_io_preallocate(int fd, off_t size);
int dyn_io_durable_sync(int fd);
int dyn_io_read_whole(const char* path, char** out, size_t* outlen);

int dyn_net_set_nonblock(int fd);
void dyn_net_set_nodelay(int fd);
int dyn_net_send_all(int fd, const void* buf, size_t len);
void dyn_net_set_io_timeout(int fd, long long ms);

#define DYN_NET_CONNECT_RESOLVE 1
#define DYN_NET_CONNECT_TIMEOUT 2
#define DYN_NET_CONNECT_FAILED 3
#define DYN_NET_CONNECT_REFUSED_BY_FILTER 4
int dyn_net_socket_cloexec(int domain, int type, int protocol);
int dyn_net_connect_blocking(const char* host, unsigned short port, long long timeout_ms,
    int (*reject)(int fd, void* udata), void* udata, int* why, int* gai_rc);
int dyn_net_peer_ip(int fd, char* out, size_t out_len, int unmap_v4);

static inline int dyn_ci_equal(const char* a, size_t alen, const char* b)
{
    size_t i;
    for (i = 0; i < alen; i++) {
        int ca = a[i], cb = b[i];
        if (cb == '\0')
            return 0;
        if (ca >= 'A' && ca <= 'Z')
            ca += 32;
        if (cb >= 'A' && cb <= 'Z')
            cb += 32;
        if (ca != cb)
            return 0;
    }
    return b[i] == '\0';
}

void* dyn_trk_malloc(size_t size);
void* dyn_trk_malloc_plumbing(size_t size);
void* dyn_trk_calloc_plumbing(size_t nmemb, size_t size);
void* dyn_trk_realloc_plumbing(void* ptr, size_t size);
void* dyn_trk_calloc(size_t nmemb, size_t size);
void* dyn_trk_realloc(void* ptr, size_t size);
void dyn_trk_free(void* ptr);
char* dyn_trk_strdup(const char* s);
char* dyn_trk_strndup(const char* s, size_t n);
#include <stdatomic.h>
extern _Atomic int dyn_trk_armed;
uint64_t dyn_trk_bytes(void);
void dyn_trk_set_limit(uint64_t limit);
void dyn_trk_set_peer(uint64_t (*exact_bytes)(void));

#endif
