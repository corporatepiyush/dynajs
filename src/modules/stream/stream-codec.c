#include "stream-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_STREAM)

void codec_enc_free(codec_enc_t* e)
{
    dyn_defs_free(e->ds);
    e->ds = NULL;
    cbuf_free(&e->lz4_pend);
#if defined(CONFIG_ZSTD)
    if (e->zc) {
        ZSTD_freeCCtx(e->zc);
        e->zc = NULL;
    }
#endif
    if (e->cs_active) {
        compression_stream_destroy(&e->cs);
        e->cs_active = 0;
    }
}

#define LZ4_ENC_BLOCK (64u * 1024)

static const char* lz4_enc_block(codec_enc_t* e, const uint8_t* in, size_t take, cbuf_t* out)
{
    uint8_t* blk = NULL;
    size_t blk_len = 0;
    uint8_t sz[4];
    uint32_t n;
    int packed;
    if (dyn_lz4_compress(in, take, NULL, 0, e->level, NULL, &blk, &blk_len) < 0) {
        free(blk);
        return "lz4 compress failed";
    }
    packed = blk && blk_len < take;
    n = packed ? (uint32_t)blk_len : ((uint32_t)take | 0x80000000u);
    sz[0] = (uint8_t)n;
    sz[1] = (uint8_t)(n >> 8);
    sz[2] = (uint8_t)(n >> 16);
    sz[3] = (uint8_t)(n >> 24);
    if (cbuf_put(out, sz, 4) < 0 || cbuf_put(out, packed ? blk : in, packed ? blk_len : take) < 0) {
        free(blk);
        return "out of memory";
    }
    free(blk);
    return NULL;
}

int codec_enc_init(codec_enc_t* e, int codec, int level)
{
    memset(e, 0, sizeof(*e));
    e->codec = codec;
    e->crc_reg = 0;
    e->level = level;
#if defined(CONFIG_ZSTD)
    if (codec == DYN_STREAM_CODEC_ZSTD) {
        e->zc = ZSTD_createCCtx();
        if (!e->zc)
            return -1;
        ZSTD_CCtx_setParameter(e->zc, ZSTD_c_compressionLevel, level);
        ZSTD_CCtx_setParameter(e->zc, ZSTD_c_checksumFlag, 1);
    }
#endif
    return 0;
}

const char* codec_enc_feed(codec_enc_t* e, const uint8_t* in,
    size_t in_len, int finalize, cbuf_t* out)
{
    if (e->finalized)
        return "stream already finished";
    e->finalized = finalize;

    if (e->codec == DYN_STREAM_CODEC_GZIP && !e->header_emitted && (in_len || finalize)) {
        static const uint8_t hdr[10] = { 0x1f, 0x8b, 0x08, 0x00, 0, 0, 0, 0, 0x00, 0xff };
        if (cbuf_put(out, hdr, sizeof(hdr)) < 0)
            return "out of memory";
        e->header_emitted = 1;
    }

    if (in_len && e->codec == DYN_STREAM_CODEC_GZIP) {
        e->crc_reg = dyn_crc32_update(e->crc_reg, in, in_len);
        e->isize += (uint32_t)in_len;
    }

    switch (e->codec) {
#if defined(CONFIG_ZSTD)
    case DYN_STREAM_CODEC_ZSTD: {
        ZSTD_inBuffer zin = { in, in_len, 0 };
        for (;;) {
            size_t need = 64 * 1024;
            ZSTD_outBuffer zout;
            size_t rc;

            if (cbuf_reserve(out, need) < 0)
                return "out of memory";
            zout.dst = out->buf + out->len;
            zout.size = need;
            zout.pos = 0;
            rc = ZSTD_compressStream2(e->zc, &zout, &zin,
                finalize ? ZSTD_e_end : ZSTD_e_continue);
            if (ZSTD_isError(rc))
                return ZSTD_getErrorName(rc);
            out->len += zout.pos;
            if (zin.pos >= zin.size && (!finalize || rc == 0))
                break;
            if (zout.pos == 0 && zin.pos >= zin.size && finalize && rc != 0)
                continue;
        }
        return NULL;
    }
#endif
    case DYN_STREAM_CODEC_DEFLATE:
    case DYN_STREAM_CODEC_GZIP: {
        if (!e->ds && !(e->ds = dyn_defs_new(e->level > 0 && e->level <= 9 ? e->level : 6)))
            return "out of memory";
        if (dyn_defs_run(e->ds, in, in_len, finalize, &out->buf, &out->len, &out->cap) < 0)
            return "out of memory";
        if (finalize && e->codec == DYN_STREAM_CODEC_GZIP) {
            uint32_t crc = e->crc_reg, isz = e->isize;
            uint8_t t[8];
            t[0] = (uint8_t)crc;
            t[1] = (uint8_t)(crc >> 8);
            t[2] = (uint8_t)(crc >> 16);
            t[3] = (uint8_t)(crc >> 24);
            t[4] = (uint8_t)isz;
            t[5] = (uint8_t)(isz >> 8);
            t[6] = (uint8_t)(isz >> 16);
            t[7] = (uint8_t)(isz >> 24);
            if (cbuf_put(out, t, 8) < 0)
                return "out of memory";
        }
        return NULL;
    }
    case DYN_STREAM_CODEC_BROTLI: {
        if (!e->cs_active) {
            if (compression_stream_init(&e->cs, COMPRESSION_STREAM_ENCODE,
                    COMPRESSION_BROTLI)
                != COMPRESSION_STATUS_OK)
                return "compression stream init failed";
            e->cs_active = 1;
        }
        e->cs.src_ptr = in;
        e->cs.src_size = in_len;
        for (;;) {
            compression_status st;
            size_t produced;

            if (cbuf_reserve(out, 64 * 1024) < 0)
                return "out of memory";
            e->cs.dst_ptr = out->buf + out->len;
            e->cs.dst_size = 64 * 1024;
            st = compression_stream_process(&e->cs,
                finalize ? COMPRESSION_STREAM_FINALIZE : 0);
            produced = 64 * 1024 - e->cs.dst_size;
            out->len += produced;
            if (st == COMPRESSION_STATUS_END)
                break;
            if (st == COMPRESSION_STATUS_ERROR)
                return "compression stream failed";
            if (produced == 0 && e->cs.src_size == 0 && !finalize)
                break;
        }
        return NULL;
    }
    case DYN_STREAM_CODEC_LZ4: {
        if (!e->header_emitted) {
            uint8_t hdr[7];
            hdr[0] = 0x04;
            hdr[1] = 0x22;
            hdr[2] = 0x4D;
            hdr[3] = 0x18;
            hdr[4] = 0x60;
            hdr[5] = 0x40;
            hdr[6] = (uint8_t)((dyn_xxh32(hdr + 4, 2, 0) >> 8) & 0xff);
            if (cbuf_put(out, hdr, sizeof(hdr)) < 0)
                return "out of memory";
            e->header_emitted = 1;
        }
        {
            const char* err;
            size_t off = 0;
            if (e->lz4_pend.len && in_len) {
                size_t take = LZ4_ENC_BLOCK - e->lz4_pend.len;
                if (take > in_len)
                    take = in_len;
                if (cbuf_put(&e->lz4_pend, in, take) < 0)
                    return "out of memory";
                off = take;
            }
            if (e->lz4_pend.len == LZ4_ENC_BLOCK || (finalize && e->lz4_pend.len)) {
                err = lz4_enc_block(e, e->lz4_pend.buf, e->lz4_pend.len, out);
                if (err)
                    return err;
                e->lz4_pend.len = 0;
            }
            while (in_len - off >= LZ4_ENC_BLOCK || (finalize && off < in_len)) {
                size_t take = in_len - off < LZ4_ENC_BLOCK ? in_len - off : LZ4_ENC_BLOCK;
                err = lz4_enc_block(e, in + off, take, out);
                if (err)
                    return err;
                off += take;
            }
            if (off < in_len && cbuf_put(&e->lz4_pend, in + off, in_len - off) < 0)
                return "out of memory";
        }
        if (finalize) {
            static const uint8_t endmark[4] = { 0, 0, 0, 0 };
            if (cbuf_put(out, endmark, 4) < 0)
                return "out of memory";
        }
        return NULL;
    }
    default:
        return "unsupported codec";
    }
}

void codec_dec_free(codec_dec_t* d)
{
#if defined(CONFIG_ZSTD)
    if (d->zd) {
        ZSTD_freeDStream(d->zd);
        d->zd = NULL;
    }
#endif
    if (d->cs_active) {
        compression_stream_destroy(&d->cs);
        d->cs_active = 0;
    }
    cbuf_free(&d->in);
    cbuf_free(&d->out);
}

int codec_dec_init(codec_dec_t* d, int codec)
{
    memset(d, 0, sizeof(*d));
    d->codec = codec;
    d->crc_reg = 0;
#if defined(CONFIG_ZSTD)
    if (codec == DYN_STREAM_CODEC_ZSTD) {
        d->zd = ZSTD_createDStream();
        if (!d->zd)
            return -1;
    }
#endif
    return 0;
}

void dec_take(cbuf_t* in, size_t n)
{
    in->len -= n;
    if (in->len)
        memmove(in->buf, in->buf + n, in->len);
}

static int gzip_parse_header(codec_dec_t* d, const char** perr)
{
    cbuf_t* in = &d->in;
    size_t need = 10;
    uint8_t flg;

    if (in->len < need)
        return 0;
    if (in->buf[0] != 0x1f || in->buf[1] != 0x8b || in->buf[2] != 0x08) {
        *perr = "not a gzip member (bad magic)";
        return -1;
    }
    flg = in->buf[3];
    if (flg & 0x04) {
        if (in->len < need + 2)
            return 0;
        need += 2 + (size_t)(in->buf[10] | (in->buf[11] << 8));
        if (in->len < need)
            return 0;
    }
    if (flg & 0x08) {
        const uint8_t* z = (const uint8_t*)memchr(in->buf + need, 0,
            in->len - need);
        if (!z)
            return 0;
        need = (size_t)(z - in->buf) + 1;
    }
    if (flg & 0x10) {
        const uint8_t* z = (const uint8_t*)memchr(in->buf + need, 0,
            in->len - need);
        if (!z)
            return 0;
        need = (size_t)(z - in->buf) + 1;
    }
    if (flg & 0x02)
        need += 2;
    if (in->len < need)
        return 0;
    dec_take(in, need);
    d->phase = 1;
    return 1;
}

const char* codec_dec_run(codec_dec_t* d, int finalize, size_t reserve)
{
    size_t limit;
    if (d->failed)
        return "decoder already failed";
    d->progressed = 0;

    switch (d->codec) {
#if defined(CONFIG_ZSTD)
    case DYN_STREAM_CODEC_ZSTD: {
        size_t consumed = 0;
        for (;;) {
            size_t need = 64 * 1024;
            ZSTD_outBuffer zout;
            ZSTD_inBuffer zin;
            size_t rc;

            if (cbuf_reserve(&d->out, need) < 0)
                return "out of memory";
            zin.src = d->in.buf + consumed;
            zin.size = d->in.len - consumed;
            zin.pos = 0;
            zout.dst = d->out.buf + d->out.len;
            zout.size = need;
            zout.pos = 0;
            rc = ZSTD_decompressStream(d->zd, &zout, &zin);
            if (ZSTD_isError(rc))
                return ZSTD_getErrorName(rc);
            d->out.len += zout.pos;
            consumed += zin.pos;
            if (zout.pos > 0 || zin.pos > 0)
                d->progressed = 1;
            if (rc == 0) {
                d->done = 1;
                if (consumed < d->in.len)
                    return "trailing garbage after zstd frame";
                break;
            }
            if (zout.pos == 0 && zin.pos < zin.size)
                break;
            if (zout.pos == 0 && zin.pos >= zin.size) {
                ZSTD_inBuffer empty = { NULL, 0, 0 };
                zin.src = NULL;
                zin.size = 0;
                zin.pos = 0;
                rc = ZSTD_decompressStream(d->zd, &zout, &empty);
                if (ZSTD_isError(rc))
                    return ZSTD_getErrorName(rc);
                d->out.len += zout.pos;
                if (zout.pos == 0)
                    break;
            }
        }
        if (consumed)
            dec_take(&d->in, consumed);
        return NULL;
    }
#endif
    case DYN_STREAM_CODEC_GZIP: {
        for (;;) {
            if (d->phase == 0) {
                const char* err;
                int r = gzip_parse_header(d, &err);
                if (r < 0)
                    return err;
                if (r == 0)
                    return NULL;
                if (!d->cs_active) {
                    if (compression_stream_init(&d->cs,
                            COMPRESSION_STREAM_DECODE,
                            COMPRESSION_ZLIB)
                        != COMPRESSION_STATUS_OK)
                        return "compression stream init failed";
                    d->cs_active = 1;
                }
                d->cs.src_ptr = d->in.buf;
                d->cs.src_size = d->in.len;
                d->phase = 1;
            }
            if (d->phase == 1) {
                limit = d->in.len > reserve ? d->in.len - reserve : 0;
                d->cs.src_ptr = d->in.buf;
                d->cs.src_size = limit;
                for (;;) {
                    compression_status st;
                    size_t produced;

                    if (cbuf_reserve(&d->out, 64 * 1024) < 0)
                        return "out of memory";
                    d->cs.dst_ptr = d->out.buf + d->out.len;
                    d->cs.dst_size = 64 * 1024;
                    st = compression_stream_process(&d->cs,
                        finalize ? COMPRESSION_STREAM_FINALIZE : 0);
                    produced = 64 * 1024 - d->cs.dst_size;
                    d->out.len += produced;
                    d->crc_reg = dyn_crc32_update(d->crc_reg,
                        d->out.buf + (d->out.len - produced),
                        produced);
                    d->isize += (uint32_t)produced;
                    if (st == COMPRESSION_STATUS_END) {
                        d->progressed = produced > 0 || d->cs.src_size < limit;
                        dec_take(&d->in,
                            (size_t)(d->cs.src_ptr - d->in.buf));
                        d->phase = 2;
                        break;
                    }
                    if (st == COMPRESSION_STATUS_ERROR)
                        return "malformed deflate stream in gzip member";
                    if (d->cs.src_size == 0) {
                        size_t used = (size_t)(d->cs.src_ptr - d->in.buf);
                        if (used) {
                            dec_take(&d->in, used);
                            d->progressed = 1;
                        }
                        if (produced)
                            d->progressed = 1;
                        return NULL;
                    }
                    if (produced == 0) {
                        return finalize
                            ? "truncated compressed stream (no gzip trailer)"
                            : "malformed deflate stream in gzip member";
                    }
                }
            }
            if (d->phase == 2) {
                if (!d->gz_have_want)
                    return NULL;
                if (d->crc_reg != d->gz_want_crc)
                    return "gzip member CRC-32 mismatch";
                if (d->isize != d->gz_want_isize)
                    return "gzip member length (ISIZE) mismatch";
                if (d->cs_active) {
                    compression_stream_destroy(&d->cs);
                    d->cs_active = 0;
                }
                if (d->in.len > (reserve > 8 ? reserve : 8))
                    return "trailing garbage after gzip member";
                d->done = 1;
                return NULL;
            }
        }
    }
    case DYN_STREAM_CODEC_LZ4: {
        cbuf_t* in = &d->in;
        for (;;) {
            if (d->phase == 0) {
                size_t hdr = 6;
                uint8_t flg;

                if (in->len < hdr)
                    return NULL;
                if (in->buf[0] != 0x04 || in->buf[1] != 0x22 || in->buf[2] != 0x4D || in->buf[3] != 0x18)
                    return "not an LZ4 frame (bad magic)";
                flg = in->buf[4];
                if (((flg >> 6) & 3) != 1)
                    return "unsupported LZ4 frame version";
                if (flg & 0x08)
                    hdr += 8;
                d->lz4_content_checksum = (flg >> 2) & 1;
                {
                    static const uint32_t bmax[8] = {
                        0, 0, 0, 0, 64u * 1024, 256u * 1024,
                        1024u * 1024, 4u * 1024 * 1024
                    };
                    uint32_t code = ((uint32_t)in->buf[5] >> 4) & 7;
                    if (code < 4)
                        return "invalid LZ4 block-max size";
                    d->lz4_block_max = bmax[code];
                }
                if (in->len < hdr + 1)
                    return NULL;
                {
                    uint8_t want = (uint8_t)((dyn_xxh32(in->buf + 4, hdr - 4, 0)
                                                 >> 8)
                        & 0xff);
                    if (in->buf[hdr] != want)
                        return "LZ4 frame descriptor checksum mismatch";
                }
                dec_take(in, hdr + 1);
                if (d->lz4_content_checksum) {
                    dyn_xxh32_init(&d->lz4_xxh, 0);
                }
                d->phase = 1;
                continue;
            }
            if (d->phase == 1) {
                uint32_t bsize;

                if (in->len < 4)
                    return NULL;
                bsize = (uint32_t)in->buf[0] | ((uint32_t)in->buf[1] << 8) | ((uint32_t)in->buf[2] << 16) | ((uint32_t)in->buf[3] << 24);
                if (bsize == 0) {
                    if (in->len < (d->lz4_content_checksum ? 8u : 4u))
                        return NULL;
                    dec_take(in, 4);
                    if (d->lz4_content_checksum) {
                        uint32_t want;
                        want = (uint32_t)in->buf[0] | ((uint32_t)in->buf[1] << 8) | ((uint32_t)in->buf[2] << 16) | ((uint32_t)in->buf[3] << 24);
                        dec_take(in, 4);
                        if (dyn_xxh32_digest(&d->lz4_xxh) != want)
                            return "LZ4 content checksum mismatch";
                    }
                    if (in->len)
                        return "trailing garbage after LZ4 frame";
                    d->done = 1;
                    return NULL;
                }
                {
                    int stored = (bsize & 0x80000000u) != 0;
                    size_t blen = bsize & 0x7FFFFFFFu;

                    if (d->lz4_block_max && blen > d->lz4_block_max)
                        return "LZ4 block exceeds the declared block-max size";
                    if (in->len < 4 + blen)
                        return NULL;
                    if (d->out.len + blen > DYN_MAX_OUTPUT)
                        return "LZ4 output exceeds the 1 GiB stream limit";
                    dec_take(in, 4);
                    if (stored) {
                        if (cbuf_put(&d->out, in->buf, blen) < 0)
                            return "out of memory";
                        if (d->lz4_content_checksum)
                            dyn_xxh32_update(&d->lz4_xxh, in->buf, blen);
                    } else {
                        dyn_outbuf_t o;
                        memset(&o, 0, sizeof(o));
                        if (dyn_lz4_decompress(in->buf, blen, NULL, 0, &o) < 0) {
                            free(o.buf);
                            return "invalid LZ4 block";
                        }
                        if (d->out.len + o.len > DYN_MAX_OUTPUT) {
                            free(o.buf);
                            return "LZ4 output exceeds the 1 GiB stream limit";
                        }
                        if (cbuf_reserve(&d->out, o.len) < 0) {
                            free(o.buf);
                            return "out of memory";
                        }
                        memcpy(d->out.buf + d->out.len, o.buf, o.len);
                        d->out.len += o.len;
                        if (d->lz4_content_checksum)
                            dyn_xxh32_update(&d->lz4_xxh, o.buf, o.len);
                        free(o.buf);
                    }
                    dec_take(in, blen);
                    d->progressed = 1;
                }
                continue;
            }
            return NULL;
        }
    }
    default:
        if (d->codec == DYN_STREAM_CODEC_DEFLATE || d->codec == DYN_STREAM_CODEC_BROTLI) {
            if (d->done)
                return NULL;
            if (!d->cs_active) {
                compression_algorithm algo = d->codec == DYN_STREAM_CODEC_BROTLI ? COMPRESSION_BROTLI
                                                                                 : COMPRESSION_ZLIB;
                if (compression_stream_init(&d->cs,
                        COMPRESSION_STREAM_DECODE,
                        algo)
                    != COMPRESSION_STATUS_OK)
                    return "compression stream init failed";
                d->cs_active = 1;
            }
            d->cs.src_ptr = d->in.buf;
            d->cs.src_size = d->in.len;
            for (;;) {
                compression_status st;
                size_t produced;

                if (cbuf_reserve(&d->out, 64 * 1024) < 0)
                    return "out of memory";
                d->cs.dst_ptr = d->out.buf + d->out.len;
                d->cs.dst_size = 64 * 1024;
                st = compression_stream_process(&d->cs, 0);
                produced = 64 * 1024 - d->cs.dst_size;
                d->out.len += produced;
                if (st == COMPRESSION_STATUS_ERROR)
                    return "malformed compressed stream";
                if (st == COMPRESSION_STATUS_END) {
                    d->done = 1;
                    break;
                }
                if (produced == 0 && d->cs.src_size == 0)
                    break;
            }
            {
                size_t used = (size_t)(d->cs.src_ptr - d->in.buf);
                if (used)
                    dec_take(&d->in, used);
            }
            return NULL;
        }
        return "unsupported codec";
    }
}

const char* codec_dec_finish(codec_dec_t* d)
{
    if (d->failed)
        return "decoder already failed";
    if (d->done)
        return (d->codec == DYN_STREAM_CODEC_DEFLATE || d->codec == DYN_STREAM_CODEC_BROTLI) && d->in.len ? "trailing garbage after compressed stream" : NULL;
    switch (d->codec) {
    case DYN_STREAM_CODEC_DEFLATE:
    case DYN_STREAM_CODEC_BROTLI: {
        if (!d->cs_active)
            return "truncated compressed stream (no data)";
        for (;;) {
            compression_status st;
            size_t produced;

            if (cbuf_reserve(&d->out, 64 * 1024) < 0)
                return "out of memory";
            d->cs.dst_ptr = d->out.buf + d->out.len;
            d->cs.dst_size = 64 * 1024;
            st = compression_stream_process(&d->cs,
                COMPRESSION_STREAM_FINALIZE);
            produced = 64 * 1024 - d->cs.dst_size;
            d->out.len += produced;
            if (st == COMPRESSION_STATUS_END) {
                if (d->cs.src_size)
                    return "trailing garbage after compressed stream";
                d->done = 1;
                return NULL;
            }
            if (st == COMPRESSION_STATUS_ERROR)
                return "truncated or malformed compressed stream";
            if (produced == 0)
                return "truncated compressed stream";
        }
    }
    case DYN_STREAM_CODEC_GZIP:
        if (d->in.len >= 8) {
            const uint8_t* t = d->in.buf + d->in.len - 8;
            d->gz_want_crc = (uint32_t)t[0] | ((uint32_t)t[1] << 8) | ((uint32_t)t[2] << 16) | ((uint32_t)t[3] << 24);
            d->gz_want_isize = (uint32_t)t[4] | ((uint32_t)t[5] << 8) | ((uint32_t)t[6] << 16) | ((uint32_t)t[7] << 24);
            d->gz_have_want = 1;
        }
        if (d->phase == 2) {
            if (!d->gz_have_want)
                return "truncated compressed stream (no gzip trailer)";
            if (d->crc_reg != d->gz_want_crc)
                return "gzip member CRC-32 mismatch";
            if (d->isize != d->gz_want_isize)
                return "gzip member length (ISIZE) mismatch";
            if (d->cs_active) {
                compression_stream_destroy(&d->cs);
                d->cs_active = 0;
            }
            d->done = 1;
            return NULL;
        }
        for (;;) {
            const char* err = codec_dec_run(d, 1, 0);
            if (err)
                return err;
            if (d->done)
                return NULL;
            if (!d->progressed)
                return "truncated compressed stream (no gzip trailer)";
        }
    case DYN_STREAM_CODEC_ZSTD:
    case DYN_STREAM_CODEC_LZ4:
        for (;;) {
            const char* err = codec_dec_run(d, 0, 0);
            if (err)
                return err;
            if (d->done)
                return NULL;
            if (!d->progressed)
                return "truncated compressed stream (no end mark)";
        }
    default:
        return "unsupported codec";
    }
}

#endif
