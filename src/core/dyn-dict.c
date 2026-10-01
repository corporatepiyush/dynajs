#include "dyn-dict.h"

#include <stdlib.h>
#include <string.h>

#include "dyn-ac.h"
#include "dyn-codec.h"
#include "dyn-hash.h"

#define DYN_DICT_MAGIC0 'D'
#define DYN_DICT_MAGIC1 'T'
#define DYN_DICT_VERSION 1
#define DYN_DICT_HEADER 7

#define DD_RUN_HDR 2

struct dyn_dict {
    dyn_ac_t* ac;
    uint8_t** phrase;
    size_t* plen;
    size_t n;
    uint32_t id;
    uint32_t* best_len;
    int32_t* best_pat;
    uint32_t *cost_a, *cost_b;
    uint8_t* take;
    size_t scratch_cap;
};

static int dd_ensure(dyn_outbuf_t* o, size_t extra)
{
    if (o->len + extra > o->cap) {
        size_t cap = o->cap ? o->cap : 64;
        uint8_t* nb;
        while (cap < o->len + extra) {
            if (cap > DYN_MAX_OUTPUT)
                return -1;
            cap *= 2;
        }
        nb = (uint8_t*)realloc(o->buf, cap);
        if (!nb)
            return -1;
        o->buf = nb;
        o->cap = cap;
    }
    return 0;
}

static int dd_put(dyn_outbuf_t* o, const uint8_t* p, size_t n)
{
    if (dd_ensure(o, n) < 0)
        return -1;
    memcpy(o->buf + o->len, p, n);
    o->len += n;
    return 0;
}

static int dd_put_varint(dyn_outbuf_t* o, uint64_t v)
{
    uint8_t tmp[DYN_CODEC_VARINT_MAX];
    size_t n = dyn_codec_put_uvarint(v, tmp);
    return dd_put(o, tmp, n);
}

static uint32_t dd_compute_id(const uint8_t* const* phrases, const size_t* lens,
    size_t n)
{
    dyn_outbuf_t canon = { NULL, 0, 0 };
    uint32_t id = 0;
    size_t i;

    for (i = 0; i < n; i++) {
        if (dd_put_varint(&canon, (uint64_t)lens[i]) < 0)
            goto done;
        if (dd_put(&canon, phrases[i], lens[i]) < 0)
            goto done;
    }
    id = dyn_crc32c(canon.buf, canon.len);
done:
    free(canon.buf);
    return id;
}

dyn_dict_t* dyn_dict_new(const uint8_t* const* phrases, const size_t* lens,
    size_t n)
{
    dyn_dict_t* d;
    size_t i;

    if (n == 0 || n > DYN_DICT_MAX_PHRASES)
        return NULL;
    for (i = 0; i < n; i++) {
        if (lens[i] == 0)
            return NULL;
    }

    d = (dyn_dict_t*)calloc(1, sizeof(*d));
    if (!d)
        return NULL;
    d->n = n;
    d->phrase = (uint8_t**)calloc(n, sizeof(*d->phrase));
    d->plen = (size_t*)calloc(n, sizeof(*d->plen));
    if (!d->phrase || !d->plen)
        goto fail;

    for (i = 0; i < n; i++) {
        d->phrase[i] = (uint8_t*)malloc(lens[i]);
        if (!d->phrase[i])
            goto fail;
        memcpy(d->phrase[i], phrases[i], lens[i]);
        d->plen[i] = lens[i];
    }

    d->ac = dyn_ac_new(n);
    if (!d->ac)
        goto fail;
    for (i = 0; i < n; i++) {
        if (dyn_ac_insert(d->ac, d->phrase[i], d->plen[i], (int)i) < 0)
            goto fail;
    }
    if (dyn_ac_build(d->ac) < 0)
        goto fail;

    d->id = dd_compute_id(phrases, lens, n);
    return d;

fail:
    dyn_dict_free(d);
    return NULL;
}

void dyn_dict_free(dyn_dict_t* d)
{
    size_t i;
    if (!d)
        return;
    if (d->phrase) {
        for (i = 0; i < d->n; i++)
            free(d->phrase[i]);
        free(d->phrase);
    }
    free(d->plen);
    dyn_ac_free(d->ac);
    free(d->best_len);
    free(d->best_pat);
    free(d->cost_a);
    free(d->cost_b);
    free(d->take);
    free(d);
}

uint32_t dyn_dict_id(const dyn_dict_t* d) { return d ? d->id : 0; }
size_t dyn_dict_count(const dyn_dict_t* d) { return d ? d->n : 0; }

typedef struct {
    dyn_dict_t* d;
    uint32_t* best_len;
    int32_t* best_pat;
} dd_sink_t;

static int dd_emit(void* ud, int pat, size_t end_byte)
{
    dd_sink_t* s = (dd_sink_t*)ud;
    size_t len = s->d->plen[pat];
    size_t start = end_byte - len;
    if ((uint32_t)len > s->best_len[start]) {
        s->best_len[start] = (uint32_t)len;
        s->best_pat[start] = pat;
    }
    return 0;
}

static int dd_scratch(dyn_dict_t* d, size_t need)
{
    if (need > d->scratch_cap) {
        uint32_t* bl = (uint32_t*)realloc(d->best_len, need * sizeof(uint32_t));
        int32_t* bp;
        if (!bl)
            return -1;
        d->best_len = bl;
        bp = (int32_t*)realloc(d->best_pat, need * sizeof(int32_t));
        if (!bp)
            return -1;
        d->best_pat = bp;
        {
            uint32_t* ca = (uint32_t*)realloc(d->cost_a, (need + 1) * sizeof(uint32_t));
            uint32_t* cb;
            uint8_t* tk;
            if (!ca)
                return -1;
            d->cost_a = ca;
            cb = (uint32_t*)realloc(d->cost_b, (need + 1) * sizeof(uint32_t));
            if (!cb)
                return -1;
            d->cost_b = cb;
            tk = (uint8_t*)realloc(d->take, need + 1);
            if (!tk)
                return -1;
            d->take = tk;
        }
        d->scratch_cap = need;
    }
    memset(d->best_len, 0, need * sizeof(uint32_t));
    return 0;
}

int dyn_dict_compress(dyn_dict_t* d, const uint8_t* src, size_t len,
    dyn_outbuf_t* o)
{
    dd_sink_t sink;
    uint8_t hdr[DYN_DICT_HEADER];
    size_t i, lit_start;

    if (!d)
        return -1;

    hdr[0] = DYN_DICT_MAGIC0;
    hdr[1] = DYN_DICT_MAGIC1;
    hdr[2] = DYN_DICT_VERSION;
    hdr[3] = (uint8_t)(d->id);
    hdr[4] = (uint8_t)(d->id >> 8);
    hdr[5] = (uint8_t)(d->id >> 16);
    hdr[6] = (uint8_t)(d->id >> 24);
    if (dd_put(o, hdr, sizeof(hdr)) < 0)
        return -1;
    if (dd_put_varint(o, (uint64_t)len) < 0)
        return -1;
    if (len == 0)
        return 0;

    if (dd_scratch(d, len) < 0)
        return -1;
    sink.d = d;
    sink.best_len = d->best_len;
    sink.best_pat = d->best_pat;
    dyn_ac_run(d->ac, src, len, dd_emit, &sink);

    {
        uint32_t *A = d->cost_a, *B = d->cost_b;
        uint8_t* take = d->take;
        size_t k;

        A[len] = 0;
        B[len] = 0;
        for (k = len; k-- > 0;) {
            uint32_t bl = d->best_len[k];
            uint32_t phrase = UINT32_MAX;
            uint32_t a, b;

            if (bl) {
                uint8_t tmp[DYN_CODEC_VARINT_MAX];
                uint32_t code_cost = (uint32_t)dyn_codec_put_uvarint((uint64_t)d->best_pat[k] + 1,
                    tmp);
                uint32_t rest = A[k + bl];
                if (rest != UINT32_MAX)
                    phrase = code_cost + rest;
            }
            a = (B[k + 1] == UINT32_MAX) ? UINT32_MAX
                                         : DD_RUN_HDR + 1 + B[k + 1];
            b = (B[k + 1] == UINT32_MAX) ? UINT32_MAX : 1 + B[k + 1];

            if (phrase <= a) {
                A[k] = phrase;
                take[k] = 1;
            } else {
                A[k] = a;
                take[k] = 0;
            }
            B[k] = (phrase < b) ? phrase : b;
        }

        i = 0;
        lit_start = 0;
        while (i < len) {
            if (!take[i] || d->best_len[i] == 0) {
                i++;
                continue;
            }
            if (i > lit_start) {
                if (dd_put_varint(o, 0) < 0 || dd_put_varint(o, (uint64_t)(i - lit_start)) < 0 || dd_put(o, src + lit_start, i - lit_start) < 0)
                    return -1;
            }
            if (dd_put_varint(o, (uint64_t)d->best_pat[i] + 1) < 0)
                return -1;
            i += d->best_len[i];
            lit_start = i;
        }
    }
    if (len > lit_start) {
        if (dd_put_varint(o, 0) < 0 || dd_put_varint(o, (uint64_t)(len - lit_start)) < 0 || dd_put(o, src + lit_start, len - lit_start) < 0)
            return -1;
    }
    return 0;
}

int dyn_dict_record_id(const uint8_t* src, size_t len, uint32_t* id)
{
    if (len < DYN_DICT_HEADER || src[0] != DYN_DICT_MAGIC0 || src[1] != DYN_DICT_MAGIC1 || src[2] != DYN_DICT_VERSION)
        return -1;
    if (id)
        *id = (uint32_t)src[3] | ((uint32_t)src[4] << 8) | ((uint32_t)src[5] << 16) | ((uint32_t)src[6] << 24);
    return 0;
}

int dyn_dict_decompress(const dyn_dict_t* d, const uint8_t* src, size_t len,
    dyn_outbuf_t* o)
{
    uint32_t rid;
    size_t pos = DYN_DICT_HEADER;
    uint64_t raw_len;
    int used;

    if (!d)
        return -1;
    if (dyn_dict_record_id(src, len, &rid) < 0)
        return -1;
    if (rid != d->id)
        return -1;

    used = dyn_codec_uvarint(src + pos, len - pos, &raw_len);
    if (used <= 0 || raw_len > DYN_MAX_OUTPUT)
        return -1;
    pos += (size_t)used;

    if (raw_len && dd_ensure(o, (size_t)raw_len) < 0)
        return -1;

    while (pos < len) {
        uint64_t code;
        used = dyn_codec_uvarint(src + pos, len - pos, &code);
        if (used <= 0)
            return -1;
        pos += (size_t)used;

        if (code == 0) {
            uint64_t rl;
            used = dyn_codec_uvarint(src + pos, len - pos, &rl);
            if (used <= 0)
                return -1;
            pos += (size_t)used;
            if (rl > (uint64_t)(len - pos))
                return -1;
            if (o->len + rl > DYN_MAX_OUTPUT)
                return -1;
            if (dd_put(o, src + pos, (size_t)rl) < 0)
                return -1;
            pos += (size_t)rl;
        } else {
            size_t idx = (size_t)(code - 1);
            if (code > (uint64_t)d->n)
                return -1;
            if (o->len + d->plen[idx] > DYN_MAX_OUTPUT)
                return -1;
            if (dd_put(o, d->phrase[idx], d->plen[idx]) < 0)
                return -1;
        }
    }

    if (o->len != (size_t)raw_len)
        return -1;
    return 0;
}
