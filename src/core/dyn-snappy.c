/*
 * dyn-snappy -- the Snappy compressed format (google/snappy format
 * description, 2011-10-05). PURE C: no JSValue, no JSContext.
 *
 *   encode: literal + copy(2-byte-offset) greedy encoder with a hash table.
 *           Only the 2-byte-offset copy form is emitted (offsets <= 65535);
 *           matches longer than 64 are split. The decoder accepts ALL FOUR
 *           element types (literal, copy1, copy2, copy4), so any conforming
 *           stream -- including system snappy output -- decodes.
 *   decode: full format, every read bounds-checked, output capped at `cap`
 *           and the declared length enforced. UNTRUSTED SURFACE.
 */
#include <stdlib.h>
#include <string.h>

#include "dyn-compress.h"
#include "dyn-snappy.h"

/* Grow `o` to hold `extra` more bytes. Returns 0 on success; on failure or
 * when o->len + extra would exceed `cap`, returns -1 with o->buf untouched. */
static int sn_grow(dyn_outbuf_t *o, size_t extra, size_t cap)
{
    size_t need;
    if (extra > cap - o->len)
        return -1;
    need = o->len + extra;
    if (need <= o->cap)
        return 0;
    {
        size_t nc = o->cap ? o->cap : 256;
        uint8_t *nb;
        while (nc < need) {
            if (nc > cap / 2) {
                nc = cap;
                break;
            }
            nc *= 2;
        }
        if (nc < need)
            nc = need;
        nb = (uint8_t *)realloc(o->buf, nc);
        if (!nb)
            return -1;
        o->buf = nb;
        o->cap = nc;
    }
    return 0;
}

/* Decode a little-endian base-128 varint (preamble / length). Returns 0 with
 * *pv set, or -1 on a malformed varint. */
static int sn_varint(const uint8_t *src, size_t len, size_t *pp, uint32_t *pv)
{
    uint32_t v = 0;
    int shift = 0;
    while (*pp < len && shift < 35) {
        uint8_t b = src[(*pp)++];
        v |= (uint32_t)(b & 0x7f) << shift;
        if (!(b & 0x80)) {
            *pv = v;
            return 0;
        }
        shift += 7;
    }
    return -1;
}

int dyn_snappy_decompress(const uint8_t *src, size_t len, size_t cap,
                          dyn_outbuf_t *o)
{
    size_t p = 0;
    uint32_t declared;

    if (sn_varint(src, len, &p, &declared) < 0)
        return -1;                  /* truncated preamble */
    if ((uint64_t)declared > cap)
        return -1;                  /* bomb: declared length exceeds the cap */

    while (p < len) {
        uint8_t tag = src[p++];
        unsigned type = tag & 3;

        if (type == 0) {            /* literal */
            uint32_t llen = tag >> 2;
            if (llen >= 60) {       /* 60..63: 1..4 length bytes follow */
                unsigned nb = (unsigned)(llen - 59);
                uint32_t x = 0;
                unsigned i;
                if (p + nb > len)
                    return -1;
                for (i = 0; i < nb; i++)
                    x |= (uint32_t)src[p + i] << (8 * i);
                p += nb;
                llen = x;           /* len - 1 */
            }
            {
                uint64_t need = (uint64_t)llen + 1;
                if (need > (uint64_t)(len - p))
                    return -1;      /* literal runs past the end */
                if (sn_grow(o, (size_t)need, cap) < 0)
                    return -1;
                memcpy(o->buf + o->len, src + p, (size_t)need);
                o->len += (size_t)need;
                p += (size_t)need;
            }
        } else {                    /* copy */
            uint32_t llen, off;
            uint32_t i;

            if (type == 1) {        /* copy, 1-byte offset */
                if (p >= len)
                    return -1;
                llen = 4 + ((tag >> 2) & 7);            /* 4..11 */
                off = ((uint32_t)(tag >> 5) << 8) | src[p++];
            } else if (type == 2) { /* copy, 2-byte offset */
                if (p + 2 > len)
                    return -1;
                llen = (tag >> 2) + 1;                  /* 1..64 */
                off = (uint32_t)src[p] | ((uint32_t)src[p + 1] << 8);
                p += 2;
            } else {                /* copy, 4-byte offset */
                if (p + 4 > len)
                    return -1;
                llen = (tag >> 2) + 1;
                off = (uint32_t)src[p] | ((uint32_t)src[p + 1] << 8) |
                      ((uint32_t)src[p + 2] << 16) | ((uint32_t)src[p + 3] << 24);
                p += 4;
            }
            if (off == 0 || off > o->len)
                return -1;          /* offset past the start of output */
            if (sn_grow(o, llen, cap) < 0)
                return -1;
            /* copies may overlap (offset < length): RLE. Copy byte-by-byte
             * only when they do, memcpy otherwise. */
            if (off >= llen) {
                memcpy(o->buf + o->len, o->buf + o->len - off, llen);
            } else {
                for (i = 0; i < llen; i++)
                    o->buf[o->len + i] = o->buf[o->len + i - off];
            }
            o->len += llen;
        }
    }
    if (o->len != declared)
        return -1;                  /* stream shorter/longer than declared */
    return 0;
}

/* ---- encoder ------------------------------------------------------------ */

#define SN_HASH_BITS 16
#define SN_HASH_SIZE (1u << SN_HASH_BITS)

static uint32_t sn_hash(const uint8_t *p)
{
    uint32_t h = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                 ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    return (h * 0x1e35a7bdu) >> (32 - SN_HASH_BITS);
}

/* Write the preamble varint of `v`; returns bytes written (1..5). */
static size_t sn_write_varint(uint8_t *out, uint32_t v)
{
    size_t n = 0;
    while (v >= 0x80) {
        out[n++] = (uint8_t)(v | 0x80);
        v >>= 7;
    }
    out[n++] = (uint8_t)v;
    return n;
}

/* Flush `lit` (litn bytes, 0 < litn <= 256) as one literal element. */
static int sn_flush_lit(uint8_t *out, size_t cap, size_t *po, uint8_t *lit,
                        size_t litn)
{
    size_t o = *po;
    if (litn <= 60) {
        if (o + 1 + litn > cap)
            return -1;
        out[o++] = (uint8_t)((litn - 1) << 2);
    } else {
        uint32_t x = (uint32_t)(litn - 1);
        unsigned nb = 1;
        while ((x >> (8 * nb)) != 0)
            nb++;
        if (o + 1 + nb + litn > cap)
            return -1;
        out[o++] = (uint8_t)((59 + nb) << 2);
        while (nb--) {
            out[o++] = (uint8_t)(x & 0xff);
            x >>= 8;
        }
    }
    memcpy(out + o, lit, litn);
    o += litn;
    *po = o;
    return 0;
}

int dyn_snappy_compress(const uint8_t *src, size_t len,
                        uint8_t **pout, size_t *pout_len)
{
    /* Worst case: every byte a separate literal run is capped at 256B, and
     * the literal tag overhead is ~1 byte per 60 bytes, so 1.02x is a
     * generous bound. */
    size_t cap = len + len / 50 + 16;
    uint8_t *out;
    uint32_t *tab;
    size_t o = 0, i = 0;
    uint8_t lit[256];
    size_t litn = 0;

    out = (uint8_t *)malloc(cap);
    if (!out)
        return -1;
    tab = (uint32_t *)calloc(SN_HASH_SIZE, sizeof(*tab));
    if (!tab) {
        free(out);
        return -1;
    }
    o = sn_write_varint(out, (uint32_t)len);

    while (i < len) {
        uint32_t cand = 0;
        if (i + 4 <= len) {
            uint32_t h = sn_hash(src + i);
            cand = tab[h];                  /* position + 1, or 0 = empty */
        }
        if (cand && i >= cand && (i - cand) <= 65534 &&
            memcmp(src + i, src + (size_t)(cand - 1), 4) == 0) {
            uint32_t off = (uint32_t)(i - cand + 1);    /* 1..65535 */
            uint32_t mlen = 4;
            while (i + mlen < len && src[i + mlen] == src[i + mlen - off])
                mlen++;
            if (litn && sn_flush_lit(out, cap, &o, lit, litn) < 0)
                goto fail;
            litn = 0;
            while (mlen) {
                uint32_t n = mlen > 64 ? 64 : mlen;
                if (o + 3 > cap)
                    goto fail;
                out[o++] = (uint8_t)((n - 1) << 2) | 2; /* copy, 2B offset */
                out[o++] = (uint8_t)(off & 0xff);
                out[o++] = (uint8_t)((off >> 8) & 0xff);
                i += n;
                mlen -= n;
            }
        } else {
            lit[litn++] = src[i];
            if (litn == sizeof lit) {
                if (sn_flush_lit(out, cap, &o, lit, litn) < 0)
                    goto fail;
                litn = 0;
            }
            i++;
        }
        if (i + 4 <= len)
            tab[sn_hash(src + i)] = (uint32_t)(i + 1);
    }
    if (litn && sn_flush_lit(out, cap, &o, lit, litn) < 0)
        goto fail;
    free(tab);
    *pout = out;
    *pout_len = o;
    return 0;

fail:
    free(tab);
    free(out);
    return -1;
}