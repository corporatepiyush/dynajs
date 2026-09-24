/* dyn-serial -- see dyn-serial.h for the format and the threat model. */
#include "dyn-serial.h"
#include "dyn-hash.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------ writer ---- */

void dyn_ser_init(dyn_ser_t *w)
{
    w->buf = NULL;
    w->len = w->cap = 0;
    w->err = 0;
}

void dyn_ser_free(dyn_ser_t *w)
{
    free(w->buf);
    dyn_ser_init(w);
}

uint8_t *dyn_ser_take(dyn_ser_t *w, size_t *len)
{
    uint8_t *b = w->buf;
    *len = w->len;
    w->buf = NULL;
    w->len = w->cap = 0;
    return b;
}

static int ser_grow(dyn_ser_t *w, size_t need)
{
    size_t nc = w->cap ? w->cap : 256;
    uint8_t *nb;
    if (w->err)
        return -1;
    if (w->len + need <= w->cap)
        return 0;
    while (nc < w->len + need) {
        if (nc > (size_t)1 << 40) {
            w->err = 1;
            return -1;
        }
        nc *= 2;
    }
    nb = (uint8_t *)realloc(w->buf, nc);
    if (!nb) {
        w->err = 1;
        return -1;
    }
    w->buf = nb;
    w->cap = nc;
    return 0;
}

int dyn_ser_raw(dyn_ser_t *w, const void *p, size_t n)
{
    if (ser_grow(w, n) < 0)
        return -1;
    if (n)
        memcpy(w->buf + w->len, p, n);
    w->len += n;
    return 0;
}

/* Little-endian by definition of the format, byte by byte so the host's own
 * byte order never leaks into the file. */
static int ser_le(dyn_ser_t *w, uint64_t v, int nbytes)
{
    uint8_t b[8];
    int i;
    for (i = 0; i < nbytes; i++)
        b[i] = (uint8_t)(v >> (i * 8));
    return dyn_ser_raw(w, b, (size_t)nbytes);
}

int dyn_ser_u8(dyn_ser_t *w, uint8_t v)   { return ser_le(w, v, 1); }
int dyn_ser_u16(dyn_ser_t *w, uint16_t v) { return ser_le(w, v, 2); }
int dyn_ser_u32(dyn_ser_t *w, uint32_t v) { return ser_le(w, v, 4); }
int dyn_ser_u64(dyn_ser_t *w, uint64_t v) { return ser_le(w, v, 8); }
int dyn_ser_i64(dyn_ser_t *w, int64_t v)  { return ser_le(w, (uint64_t)v, 8); }

int dyn_ser_f64(dyn_ser_t *w, double v)
{
    uint64_t bits;
    memcpy(&bits, &v, 8);        /* IEEE-754 bit pattern, NaN payload and all */
    return ser_le(w, bits, 8);
}

int dyn_ser_blob(dyn_ser_t *w, const void *p, size_t n)
{
    if (n > 0xFFFFFFFFu) {
        w->err = 1;
        return -1;
    }
    if (dyn_ser_u32(w, (uint32_t)n) < 0)
        return -1;
    return dyn_ser_raw(w, p, n);
}

size_t dyn_varint_len(uint64_t v)
{
    size_t k = 1;
    while (v >>= 7)
        k++;
    return k;
}

int dyn_ser_uvarint(dyn_ser_t *w, uint64_t v)
{
    do {
        uint8_t b = (uint8_t)(v & 0x7f);
        v >>= 7;
        if (v)
            b |= 0x80;
        if (dyn_ser_u8(w, b) < 0)
            return -1;
    } while (v);
    return 0;
}

int dyn_ser_svarint(dyn_ser_t *w, int64_t v)
{
    return dyn_ser_uvarint(w, ((uint64_t)v << 1) ^ (uint64_t)(v >> 63));
}

int dyn_de_uvarint(dyn_de_t *r, uint64_t *out)
{
    uint64_t v = 0;
    int shift = 0;
    for (;;) {
        uint8_t b = dyn_de_u8(r);
        if (!dyn_de_ok(r))
            return -1;
        v |= (uint64_t)(b & 0x7f) << shift;
        if (!(b & 0x80))
            break;
        shift += 7;
        if (shift >= 64)
            return -1;
    }
    *out = v;
    return 0;
}

int dyn_de_svarint(dyn_de_t *r, int64_t *out)
{
    uint64_t z;
    if (dyn_de_uvarint(r, &z) < 0)
        return -1;
    *out = (int64_t)(z >> 1) ^ -(int64_t)(z & 1);
    return 0;
}

int dyn_ser_begin(dyn_ser_t *w, uint16_t type_id, uint32_t flags)
{
    static const uint8_t magic[4] = {
        DYN_SER_MAGIC0, DYN_SER_MAGIC1, DYN_SER_MAGIC2, DYN_SER_MAGIC3
    };
    if (dyn_ser_raw(w, magic, 4) < 0 ||
        dyn_ser_u16(w, DYN_SER_VERSION) < 0 ||
        dyn_ser_u16(w, type_id) < 0 ||
        dyn_ser_u32(w, flags) < 0 ||
        dyn_ser_u64(w, 0) < 0)          /* patched by dyn_ser_finish */
        return -1;
    return 0;
}

int dyn_ser_finish(dyn_ser_t *w)
{
    uint64_t payload;
    uint32_t crc;
    int i;

    if (w->err || w->len < DYN_SER_HEADER) {
        w->err = 1;
        return -1;
    }
    payload = (uint64_t)(w->len - DYN_SER_HEADER);
    for (i = 0; i < 8; i++)
        w->buf[12 + i] = (uint8_t)(payload >> (i * 8));
    crc = dyn_crc32c(w->buf, w->len);
    return dyn_ser_u32(w, crc);
}

/* ------------------------------------------------------------ reader ---- */

const char *dyn_de_strerror(int code)
{
    switch (code) {
    case DYN_DE_OK:          return "ok";
    case DYN_DE_BAD_MAGIC:   return "not a DYNS record";
    case DYN_DE_BAD_VERSION: return "unsupported DYNS format version";
    case DYN_DE_TRUNCATED:   return "truncated DYNS record";
    case DYN_DE_BAD_CRC:     return "DYNS checksum mismatch";
    case DYN_DE_TOO_LARGE:   return "DYNS payload exceeds maxBytes";
    default:                 return "invalid DYNS record";
    }
}

static uint64_t de_le(const uint8_t *p, int nbytes)
{
    uint64_t v = 0;
    int i;
    for (i = 0; i < nbytes; i++)
        v |= (uint64_t)p[i] << (i * 8);
    return v;
}

int dyn_de_open(dyn_de_t *r, const uint8_t *buf, size_t len,
                uint16_t *type_id, uint32_t *flags, size_t max_payload)
{
    uint64_t payload;
    uint32_t want, got;

    r->p = NULL;
    r->len = r->pos = 0;
    r->err = 1;

    if (!max_payload)
        max_payload = DYN_SER_DEFAULT_MAX;
    if (len < DYN_SER_HEADER + DYN_SER_TRAILER)
        return DYN_DE_TRUNCATED;
    if (buf[0] != DYN_SER_MAGIC0 || buf[1] != DYN_SER_MAGIC1 ||
        buf[2] != DYN_SER_MAGIC2 || buf[3] != DYN_SER_MAGIC3)
        return DYN_DE_BAD_MAGIC;
    if (de_le(buf + 4, 2) != DYN_SER_VERSION)
        return DYN_DE_BAD_VERSION;

    payload = de_le(buf + 12, 8);
    if (payload > (uint64_t)max_payload)
        return DYN_DE_TOO_LARGE;
    /* The length must describe THIS buffer exactly. A record that claims less
     * than it carries is as suspect as one that claims more. */
    if (payload != (uint64_t)len - DYN_SER_HEADER - DYN_SER_TRAILER)
        return DYN_DE_TRUNCATED;

    /* Checked before a single payload byte is interpreted. */
    want = (uint32_t)de_le(buf + len - DYN_SER_TRAILER, 4);
    got = dyn_crc32c(buf, len - DYN_SER_TRAILER);
    if (want != got)
        return DYN_DE_BAD_CRC;

    if (type_id)
        *type_id = (uint16_t)de_le(buf + 6, 2);
    if (flags)
        *flags = (uint32_t)de_le(buf + 8, 4);
    r->p = buf + DYN_SER_HEADER;
    r->len = (size_t)payload;
    r->pos = 0;
    r->err = 0;
    return DYN_DE_OK;
}

/* The one place the cursor moves. Everything else goes through it. */
static const uint8_t *de_take(dyn_de_t *r, size_t n)
{
    const uint8_t *p;
    if (r->err || n > r->len - r->pos) {
        r->err = 1;
        return NULL;
    }
    p = r->p + r->pos;
    r->pos += n;
    return p;
}

uint8_t dyn_de_u8(dyn_de_t *r)
{
    const uint8_t *p = de_take(r, 1);
    return p ? p[0] : 0;
}

uint16_t dyn_de_u16(dyn_de_t *r)
{
    const uint8_t *p = de_take(r, 2);
    return p ? (uint16_t)de_le(p, 2) : 0;
}

uint32_t dyn_de_u32(dyn_de_t *r)
{
    const uint8_t *p = de_take(r, 4);
    return p ? (uint32_t)de_le(p, 4) : 0;
}

uint64_t dyn_de_u64(dyn_de_t *r)
{
    const uint8_t *p = de_take(r, 8);
    return p ? de_le(p, 8) : 0;
}

int64_t dyn_de_i64(dyn_de_t *r)
{
    return (int64_t)dyn_de_u64(r);
}

double dyn_de_f64(dyn_de_t *r)
{
    uint64_t bits = dyn_de_u64(r);
    double v;
    memcpy(&v, &bits, 8);
    return v;
}

const uint8_t *dyn_de_raw(dyn_de_t *r, size_t n)
{
    return de_take(r, n);
}

const char *dyn_de_blob(dyn_de_t *r, size_t *n)
{
    uint32_t len = dyn_de_u32(r);
    const uint8_t *p;
    if (r->err || len > r->len - r->pos) {
        r->err = 1;
        *n = 0;
        return NULL;
    }
    p = de_take(r, len);
    *n = p ? len : 0;
    /* A zero-length blob is legal and must not read as an error, so return a
     * non-NULL pointer into the payload rather than NULL. */
    return p ? (const char *)p : (r->err ? NULL : (const char *)r->p);
}

int dyn_de_count(dyn_de_t *r, uint32_t *out, size_t elem_size)
{
    uint32_t n = dyn_de_u32(r);
    *out = 0;
    if (r->err)
        return -1;
    if (elem_size) {
        /* The whole point: refuse a count the remaining bytes cannot hold,
         * before the caller allocates anything for it. */
        if ((uint64_t)n * elem_size > (uint64_t)(r->len - r->pos)) {
            r->err = 1;
            return -1;
        }
    } else if (n > r->len - r->pos) {
        /* A nested structure needs at least one byte per element. */
        r->err = 1;
        return -1;
    }
    *out = n;
    return 0;
}
