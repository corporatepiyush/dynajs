#include "stream-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_STREAM) && !defined(DYN_STREAM_HAVE_BROTLI)

static int dyn_cs_grow(uint8_t** buf, size_t* cap, size_t need)
{
    size_t nc = *cap ? *cap : 4096;
    uint8_t* nb;
    if (need <= *cap)
        return 0;
    while (nc < need) {
        if (nc > SIZE_MAX / 2)
            return -1;
        nc *= 2;
    }
    nb = (uint8_t*)realloc(*buf, nc);
    if (!nb)
        return -1;
    *buf = nb;
    *cap = nc;
    return 0;
}

compression_status compression_stream_init(compression_stream* cs,
    compression_stream_operation op, compression_algorithm algo)
{
    dyn_cs_state_t* st;
    if (algo != COMPRESSION_ZLIB)
        return COMPRESSION_STATUS_ERROR;
    st = (dyn_cs_state_t*)calloc(1, sizeof(*st));
    if (!st)
        return COMPRESSION_STATUS_ERROR;
    st->op = (int)op;
    if (op == COMPRESSION_STREAM_ENCODE) {
        if (dyn_comp_ctx_init(&st->ctx) != 0) {
            free(st);
            return COMPRESSION_STATUS_ERROR;
        }
        st->ctx_ready = 1;
    }
    memset(cs, 0, sizeof(*cs));
    cs->state = st;
    return COMPRESSION_STATUS_OK;
}

void compression_stream_destroy(compression_stream* cs)
{
    dyn_cs_state_t* st = cs->state;
    if (!st)
        return;
    if (st->ctx_ready)
        dyn_comp_ctx_free(&st->ctx);
    free(st->held);
    free(st->ready);
    free(st);
    cs->state = NULL;
}

static int dyn_cs_ready_put(dyn_cs_state_t* st, const uint8_t* data, size_t n)
{
    if (n == 0)
        return 0;
    if (st->ready_len > SIZE_MAX - n || dyn_cs_grow(&st->ready, &st->ready_cap, st->ready_len + n) < 0)
        return -1;
    memcpy(st->ready + st->ready_len, data, n);
    st->ready_len += n;
    return 0;
}

static compression_status dyn_cs_drain(compression_stream* cs, int at_end)
{
    dyn_cs_state_t* st = cs->state;
    size_t left = st->ready_len - st->ready_off;
    size_t n = left < cs->dst_size ? left : cs->dst_size;
    if (n) {
        memcpy(cs->dst_ptr, st->ready + st->ready_off, n);
        cs->dst_ptr += n;
        cs->dst_size -= n;
        st->ready_off += n;
    }
    if (st->ready_off == st->ready_len) {
        st->ready_off = st->ready_len = 0;
        if (at_end)
            return COMPRESSION_STATUS_END;
    }
    return COMPRESSION_STATUS_OK;
}

static compression_status dyn_cs_encode(compression_stream* cs, int finalize)
{
    dyn_cs_state_t* st = cs->state;
    if (cs->src_size) {
        uint8_t* part = NULL;
        size_t part_len = 0;
        if (dyn_raw_deflate_part(cs->src_ptr, cs->src_size, DYN_CS_LEVEL, &st->ctx, &part, &part_len) != 0)
            return COMPRESSION_STATUS_ERROR;
        if (dyn_cs_ready_put(st, part, part_len) < 0) {
            free(part);
            return COMPRESSION_STATUS_ERROR;
        }
        free(part);
        cs->src_ptr += cs->src_size;
        cs->src_size = 0;
    }
    if (finalize && !st->ended) {
        static const uint8_t last_block[5] = { 0x01, 0x00, 0x00, 0xff, 0xff };
        if (dyn_cs_ready_put(st, last_block, sizeof last_block) < 0)
            return COMPRESSION_STATUS_ERROR;
        st->ended = 1;
    }
    return dyn_cs_drain(cs, st->ended);
}

static compression_status dyn_cs_decode(compression_stream* cs, int finalize)
{
    dyn_cs_state_t* st = cs->state;
    if (st->ended)
        return dyn_cs_drain(cs, 1);
    if (!finalize) {
        if (cs->src_size) {
            if (st->held_len > SIZE_MAX - cs->src_size
                || dyn_cs_grow(&st->held, &st->held_cap, st->held_len + cs->src_size) < 0)
                return COMPRESSION_STATUS_ERROR;
            memcpy(st->held + st->held_len, cs->src_ptr, cs->src_size);
            st->held_len += cs->src_size;
            cs->src_ptr += cs->src_size;
            cs->src_size = 0;
        }
        return COMPRESSION_STATUS_OK;
    }
    {
        dyn_outbuf_t o;
        size_t used = 0, total = st->held_len + cs->src_size;
        const uint8_t* all = cs->src_ptr;
        int rc;

        if (st->held_len) {
            if (total < st->held_len || dyn_cs_grow(&st->held, &st->held_cap, total ? total : 1) < 0)
                return COMPRESSION_STATUS_ERROR;
            if (cs->src_size)
                memcpy(st->held + st->held_len, cs->src_ptr, cs->src_size);
            all = st->held;
        }
        memset(&o, 0, sizeof(o));
        o.limit = DYN_CS_MAX_OUTPUT;
        rc = total ? dyn_raw_inflate_prefix(all, total, &o, &used) : -1;
        if (rc != 0 || used < st->held_len) {
            free(o.buf);
            return COMPRESSION_STATUS_ERROR;
        }
        free(st->ready);
        st->ready = o.buf;
        st->ready_len = o.len;
        st->ready_cap = o.cap;
        st->ready_off = 0;
        cs->src_ptr += used - st->held_len;
        cs->src_size -= used - st->held_len;
        st->held_len = 0;
        st->ended = 1;
    }
    return dyn_cs_drain(cs, 1);
}

compression_status compression_stream_process(compression_stream* cs, int flags)
{
    dyn_cs_state_t* st = cs->state;
    int finalize = (flags & COMPRESSION_STREAM_FINALIZE) != 0;
    if (!st)
        return COMPRESSION_STATUS_ERROR;
    return st->op == COMPRESSION_STREAM_ENCODE ? dyn_cs_encode(cs, finalize)
                                               : dyn_cs_decode(cs, finalize);
}

#endif
