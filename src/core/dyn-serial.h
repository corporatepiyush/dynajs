/*
 * dyn-serial -- the one binary envelope, and the bounds-checked reader for it.
 *
 * ONE format, shared by dyna:structures and dyna:ml, so there is one thing to
 * version and one reader to fuzz:
 *
 *   "DYNS" | u16 version | u16 type_id | u32 flags | u64 payload_len
 *          | payload | u32 crc32c(everything before it)
 *
 * All integers are little-endian, assembled byte by byte rather than punned, so
 * a file written on one host reads on another. Doubles go out as their IEEE-754
 * bit pattern for the same reason.
 *
 * ---- The reader is an UNTRUSTED-INPUT surface ----
 *
 * CLAUDE.md section 7: "JS_ReadObject is the top untrusted surface and a whole
 * bug class lives there." A `deserializeFromFile` makes a file exactly that, so
 * the reader is built to make the bug class unrepresentable rather than to be
 * carefully audited:
 *
 *   - a cursor that can only move forward and only within bounds. Every read
 *     past the end sets a sticky error and returns a zero, so a caller that
 *     forgets one check gets zeros, never a wild pointer;
 *   - dyn_de_count() is the ONLY way to read a length that will drive an
 *     allocation. It refuses any count whose elements cannot possibly fit in
 *     the bytes that remain -- the check happens BEFORE the allocation, which
 *     is the specific mistake that makes "every *_count is read before its
 *     array is allocated" a bug class in the bytecode reader;
 *   - the CRC is verified over the whole record before any payload byte is
 *     interpreted;
 *   - a caller-supplied hard cap on payload_len.
 */
#ifndef DYN_SERIAL_H
#define DYN_SERIAL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DYN_SER_MAGIC0 'D'
#define DYN_SER_MAGIC1 'Y'
#define DYN_SER_MAGIC2 'N'
#define DYN_SER_MAGIC3 'S'
#define DYN_SER_VERSION 1u
#define DYN_SER_HEADER 20u    /* magic 4 + ver 2 + type 2 + flags 4 + len 8 */
#define DYN_SER_TRAILER 4u    /* crc32c */

/* Default hard cap on a record's payload. A caller may lower it; nothing may
 * raise it past what its own buffer can hold. */
#define DYN_SER_DEFAULT_MAX (1u << 28)

/* ------------------------------------------------------------ writer ---- */

typedef struct {
    uint8_t *buf;
    size_t len, cap;
    int err;          /* sticky: 1 once any append failed to allocate */
} dyn_ser_t;

void dyn_ser_init(dyn_ser_t *w);
void dyn_ser_free(dyn_ser_t *w);
/* Hand the buffer to the caller, who must free() it. Leaves `w` empty. */
uint8_t *dyn_ser_take(dyn_ser_t *w, size_t *len);

int dyn_ser_raw(dyn_ser_t *w, const void *p, size_t n);
int dyn_ser_u8(dyn_ser_t *w, uint8_t v);
int dyn_ser_u16(dyn_ser_t *w, uint16_t v);
int dyn_ser_u32(dyn_ser_t *w, uint32_t v);
int dyn_ser_u64(dyn_ser_t *w, uint64_t v);
int dyn_ser_i64(dyn_ser_t *w, int64_t v);
int dyn_ser_f64(dyn_ser_t *w, double v);
/* u32 length followed by the bytes. */
int dyn_ser_blob(dyn_ser_t *w, const void *p, size_t n);
/* LEB128. Small values cost one byte, which is what makes a stream of counts,
 * indices and deltas worth writing at all. */
int dyn_ser_uvarint(dyn_ser_t *w, uint64_t v);
/* Zigzag, so a small NEGATIVE delta is also one byte. */
int dyn_ser_svarint(dyn_ser_t *w, int64_t v);
/* Bytes a value would occupy, for choosing between encodings by size. */
size_t dyn_varint_len(uint64_t v);

/* Write the header with a placeholder length. */
int dyn_ser_begin(dyn_ser_t *w, uint16_t type_id, uint32_t flags);
/* Backpatch payload_len and append the CRC. After this the buffer is a
 * complete record. */
int dyn_ser_finish(dyn_ser_t *w);

/* ------------------------------------------------------------ reader ---- */

typedef struct {
    const uint8_t *p;
    size_t len;        /* payload length, NOT the whole record */
    size_t pos;
    int err;           /* sticky: 1 once any read went out of bounds */
} dyn_de_t;

#define DYN_DE_OK           0
#define DYN_DE_BAD_MAGIC   -1
#define DYN_DE_BAD_VERSION -2
#define DYN_DE_TRUNCATED   -3
#define DYN_DE_BAD_CRC     -4
#define DYN_DE_TOO_LARGE   -5

/* Validate a whole record and position `r` at the start of its payload.
 * `max_payload` of 0 means DYN_SER_DEFAULT_MAX. Returns DYN_DE_*. */
int dyn_de_open(dyn_de_t *r, const uint8_t *buf, size_t len,
                uint16_t *type_id, uint32_t *flags, size_t max_payload);
const char *dyn_de_strerror(int code);

/* Every accessor returns 0/0.0/NULL and sets `err` on underrun. */
uint8_t dyn_de_u8(dyn_de_t *r);
uint16_t dyn_de_u16(dyn_de_t *r);
uint32_t dyn_de_u32(dyn_de_t *r);
uint64_t dyn_de_u64(dyn_de_t *r);
int64_t dyn_de_i64(dyn_de_t *r);
double dyn_de_f64(dyn_de_t *r);
/* Borrowed pointer into the payload, valid for the reader's lifetime. */
const uint8_t *dyn_de_raw(dyn_de_t *r, size_t n);
/* Refuse a continuation run longer than a u64 holds, so a forged payload can
 * neither spin nor overflow. Returns -1 and sets `err` on underrun. */
int dyn_de_uvarint(dyn_de_t *r, uint64_t *out);
int dyn_de_svarint(dyn_de_t *r, int64_t *out);
const char *dyn_de_blob(dyn_de_t *r, size_t *n);

/* Read a count that will drive an allocation of `count * elem_size` bytes.
 * Fails -- setting `err` and returning 0 -- if those bytes cannot possibly be
 * present in what remains, so no caller can allocate on a forged length.
 * `elem_size` of 0 means the count does not index the payload directly (a
 * nested structure follows); then only an absolute sanity cap applies. */
int dyn_de_count(dyn_de_t *r, uint32_t *out, size_t elem_size);

static inline int dyn_de_ok(const dyn_de_t *r) { return !r->err; }
/* Bytes not yet consumed. */
static inline size_t dyn_de_left(const dyn_de_t *r)
{
    return r->err ? 0 : r->len - r->pos;
}

#ifdef __cplusplus
}
#endif

#endif /* DYN_SERIAL_H */
