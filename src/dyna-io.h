/*
 * dyna-io -- shared disk/network I/O substrate for the whole engine.
 *
 * This is ENGINE-CORE infrastructure (always compiled and linked, independent of
 * CONFIG_NATIVE_MODULES): dynajs itself slurps source/module/bytecode files
 * through here before parsing, and every dyna:* module that touches disk or a
 * socket (file, csv, http, compress, ...) shares this one buffer type
 * and these primitives instead of rolling its own.
 *
 * Two design goals drive the shapes here:
 *   1. Avoid CPU-cache misses -- hot metadata is packed at the front of one
 *      cache line, and small payloads live INLINE in the struct (no separate
 *      allocation, no pointer-chase to reach the bytes).
 *   2. Avoid cross-boundary copies -- a whole file can be an mmap VIEW handed
 *      straight to the parser (no read()-into-heap copy), and a front-consuming
 *      reader advances a cursor instead of memmove-ing the remainder.
 *
 * Everything here is plain C over fds/paths with no JSContext dependency, and
 * uses libc malloc/realloc/free (thread-safe), so it is safe to call from the
 * HTTP worker/reactor threads as well as the JS thread.
 */
#ifndef DYNAJS_IO_H
#define DYNAJS_IO_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h> /* off_t */

/* ==================================================================== *
 *  dyn_iobuf_t -- the one growable I/O buffer (uint8_t*)                *
 * ==================================================================== *
 *
 * Layout: valid bytes are data[rpos .. len); [0,rpos) are consumed, [len,cap)
 * is spare capacity. `kind` records how `data` was obtained so teardown frees
 * it correctly (and so a slurped file can be a zero-copy mmap view).
 *
 * Invariants: rpos <= len <= cap. `data` points either at the inline array or a
 * heap/mmap block. NEVER cache `data` across a reserve/append/consume-compact --
 * growth or an inline->heap transition moves it. A dyn_iobuf_t owning inline
 * data must not be relocated by value (its `data` would dangle); embed it in a
 * stable struct and pass it by pointer.
 */
enum {
    DYN_IOBUF_INLINE = 0, /* data == inln[]                      */
    DYN_IOBUF_HEAP,       /* data is a malloc'd block            */
    DYN_IOBUF_MMAP        /* data is an mmap view (read-only)    */
};

/* Inline capacity chosen so sizeof(dyn_iobuf_t) stays compact (~128B, <=2 lines)
 * while absorbing tiny payloads (small HTTP status lines, short fields, empty or
 * near-empty files) without any heap allocation. */
#define DYN_IOBUF_INLINE_CAP 88

typedef struct {
    uint8_t *data; /* base of the current storage                       */
    size_t   len;  /* end of valid data / write position                */
    size_t   rpos; /* read cursor; front bytes [0,rpos) are consumed    */
    size_t   cap;  /* capacity of *data                                 */
    uint8_t  kind; /* DYN_IOBUF_*                                       */
    uint8_t  inln[DYN_IOBUF_INLINE_CAP];
} dyn_iobuf_t;

/* Initialise an empty buffer backed by its inline storage. (Zeroing the struct
 * then calling this, or memset+init, is fine; init also fixes up `data`.) */
void dyn_iobuf_init(dyn_iobuf_t *b);

/* Ensure at least `extra` more bytes fit past `len` (transitions inline->heap or
 * mmap->heap and grows as needed). Returns 0, or -1 on OOM. */
int dyn_iobuf_reserve(dyn_iobuf_t *b, size_t extra);

/* Append `n` bytes from `src`. Returns 0 or -1 (OOM). */
int dyn_iobuf_append(dyn_iobuf_t *b, const void *src, size_t n);

/* Reserve room for `n` bytes at the tail and return a writable pointer to it
 * WITHOUT advancing len (for read()/recv() straight into the buffer). Returns
 * NULL on OOM. Follow a successful I/O with dyn_iobuf_commit(actual). */
uint8_t *dyn_iobuf_tail(dyn_iobuf_t *b, size_t n);

/* Advance `len` by `n` bytes just written into the tail. */
void dyn_iobuf_commit(dyn_iobuf_t *b, size_t n);

/* Readable view of the unconsumed bytes. */
static inline uint8_t *dyn_iobuf_rdata(const dyn_iobuf_t *b)
{
    return b->data + b->rpos;
}
static inline size_t dyn_iobuf_rlen(const dyn_iobuf_t *b)
{
    return b->len - b->rpos;
}

/* Advance the read cursor by `n` consumed bytes (no memmove). Caller must not
 * consume past rlen(). */
void dyn_iobuf_consume(dyn_iobuf_t *b, size_t n);

/* Drop the consumed prefix: move [rpos,len) to the base and set rpos=0. Call
 * before a large tail reserve to keep capacity bounded. */
void dyn_iobuf_compact(dyn_iobuf_t *b);

/* Guarantee data[len] is a readable '\0' (grows by one byte if needed) so the
 * span can be handed to a NUL-relying C consumer. Returns 0 or -1. */
int dyn_iobuf_ensure_nul(dyn_iobuf_t *b);

/* Empty the buffer (len=rpos=0) but keep the allocation for reuse. */
void dyn_iobuf_reset(dyn_iobuf_t *b);

/* Release heap/mmap backing and re-arm the inline storage. Idempotent. */
void dyn_iobuf_free(dyn_iobuf_t *b);

/* ==================================================================== *
 *  Whole-file I/O                                                       *
 * ==================================================================== */

/* dyn_io_slurp flags. */
#define DYN_SLURP_NUL    1 /* guarantee a readable '\0' at data[len]      */
#define DYN_SLURP_NOMMAP 2 /* force a heap read (never mmap the file)     */

/* Hard ceiling for a single whole-file read into memory. A file whose st_size
 * exceeds this -- or a stream that grows past it -- is refused with
 * errno=EFBIG instead of driving an unbounded malloc/mmap on the caller's
 * thread (which would OOM rather than raise a clean error). 1 GiB is the same
 * ceiling the decompressors use (DYN_MAX_OUTPUT). p1b-2: the slurp family
 * previously trusted st_size verbatim, so a hostile/corrupt size (a sparse
 * file up to 2^63) became an uncapped allocation. */
#define DYN_MAX_INPUT ((size_t)1 << 30)

/* Read the entire file at `path` into `out` (which must be inited or zeroed).
 * Picks the cheapest path: for a large file it mmaps a zero-copy read-only view
 * (DYN_IOBUF_MMAP); otherwise an advise-hinted sequential heap read. With
 * DYN_SLURP_NUL, mmap is used only when a trailing NUL can be guaranteed by the
 * page tail, else it falls back to a heap read that appends the NUL. On return
 * dyn_iobuf_rlen(out) is the file size. Returns 0, or -1 (errno set). Caller
 * dyn_iobuf_free()s `out`.
 *
 * `maxsize` bounds the read: st_size > maxsize fails with errno=EFBIG BEFORE
 * any allocation, and a pseudo-file reporting st_size==0 is read to EOF (its
 * growth is also bounded by maxsize). Pass DYN_MAX_INPUT for the default.
 *
 * mmap is an OPT-IN fast path for USER-requested whole-file reads: a mapped file
 * truncated or hit by an I/O error under us faults as SIGBUS. The engine's own
 * loader must therefore NEVER use this -- it reads via dyn_io_read_buf below. */
int dyn_io_slurp(const char *path, dyn_iobuf_t *out, int flags, size_t maxsize);

/* Same, from an ALREADY-OPEN fd, which it takes ownership of and closes.
 * The way to slurp a file whose path must not be resolved by name. */
int dyn_io_slurp_fd(int fd, dyn_iobuf_t *out, int flags, size_t maxsize);

/* Heap-only whole-file read into `out` (never mmap; SIGBUS-safe). This is the
 * reader the engine's internal source/module loader uses before parsing. Same
 * flags/return/ownership contract as dyn_io_slurp minus the mmap path. */
int dyn_io_read_buf(const char *path, dyn_iobuf_t *out, int flags, size_t maxsize);

/* Atomically replace `path` with `data`/`len` via a sibling temp file + rename.
 * When `durable` is nonzero the temp file is flushed to stable storage before
 * the rename (F_FULLFSYNC on macOS, fdatasync on Linux). Returns 0 or -1. */
int dyn_io_write_whole_atomic(const char *path, const void *data, size_t len,
                              int durable);

/* ==================================================================== *
 *  Lower-level disk primitives (best facility per platform)            *
 * ==================================================================== */

/* Hint sequential access on `fd` and warm read-ahead. */
void dyn_io_advise_seq_read(int fd, off_t size);
/* Best-effort reserve `size` bytes of backing store (anti-fragmentation). */
int dyn_io_preallocate(int fd, off_t size);
/* Durably flush `fd` to stable storage (real platter flush on macOS). */
int dyn_io_durable_sync(int fd);
/* Read the whole file at `path` into a fresh malloc'd buffer (caller free()s).
 * Retained for callers that want a raw char* (io_uring on Linux when built). */
int dyn_io_read_whole(const char *path, char **out, size_t *outlen);

/* ==================================================================== *
 *  Network primitives (shared by every socket module)                  *
 * ==================================================================== */

/* Put `fd` into non-blocking mode. Returns 0 or -1. */
int dyn_net_set_nonblock(int fd);
/* Disable Nagle on a TCP `fd` (best-effort; harmless on non-TCP fds). */
void dyn_net_set_nodelay(int fd);
/* Write all `len` bytes to `fd`, retrying short/interrupted writes. 0 or -1. */
int dyn_net_send_all(int fd, const void *buf, size_t len);

#endif /* DYNAJS_IO_H */
