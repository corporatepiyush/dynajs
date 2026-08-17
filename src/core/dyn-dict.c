/*
 * dyn-dict -- token-substitution dictionary codec, pure C. See dyn-dict.h for
 * the wire format and the threat model.
 *
 * The encoder is NOT greedy, and that is a measured decision rather than a
 * refinement. Aho-Corasick reports hits as they END, so one pass fills a
 * per-position table of "the longest phrase starting here"; a greedy walk over
 * that table then made this codec EXPAND its own best case, because taking a
 * short match blocks a longer one starting one byte later and greedy can never
 * come back for it. The second pass is therefore a two-state dynamic program
 * that is allowed to DECLINE a match -- see the parse for the states and the
 * numbers.
 *
 * Either way the encoding is a function of the phrase SET and the input alone,
 * never of the order Aho-Corasick happened to emit, which is what makes the
 * output reproducible across platforms and therefore checkable by a byte hash
 * rather than only by a round trip.
 */
#include "dyn-dict.h"

#include <stdlib.h>
#include <string.h>

#include "dyn-ac.h"
#include "dyn-codec.h"
#include "dyn-hash.h"

#define DYN_DICT_MAGIC0 'D'
#define DYN_DICT_MAGIC1 'T'
#define DYN_DICT_VERSION 1
#define DYN_DICT_HEADER 7   /* magic(2) + version(1) + id(4) */

/* Cost of OPENING a literal run: the 0 marker varint plus the run-length
 * varint. Exact below 128 bytes, one low beyond -- see the parse. */
#define DD_RUN_HDR 2

struct dyn_dict {
    dyn_ac_t *ac;
    uint8_t **phrase;
    size_t *plen;
    size_t n;
    uint32_t id;
    /* Encoder scratch, owned across calls so a hot loop does not malloc per
     * record. Grown on demand and never shrunk -- the same reasoning as the
     * Compressor's head table. */
    uint32_t *best_len;
    int32_t *best_pat;
    uint32_t *cost_a, *cost_b;   /* the two DP states, len+1 entries */
    uint8_t *take;               /* 1 where the DP chose the phrase at this position */
    size_t scratch_cap;
};

/* ---- output helpers ----------------------------------------------------- */

static int dd_ensure(dyn_outbuf_t *o, size_t extra)
{
    if (o->len + extra > o->cap) {
        size_t cap = o->cap ? o->cap : 64;
        uint8_t *nb;
        while (cap < o->len + extra) {
            if (cap > DYN_MAX_OUTPUT)
                return -1;
            cap *= 2;
        }
        nb = (uint8_t *)realloc(o->buf, cap);
        if (!nb)
            return -1;
        o->buf = nb;
        o->cap = cap;
    }
    return 0;
}

static int dd_put(dyn_outbuf_t *o, const uint8_t *p, size_t n)
{
    if (dd_ensure(o, n) < 0)
        return -1;
    memcpy(o->buf + o->len, p, n);
    o->len += n;
    return 0;
}

static int dd_put_varint(dyn_outbuf_t *o, uint64_t v)
{
    uint8_t tmp[DYN_CODEC_VARINT_MAX];
    size_t n = dyn_codec_put_uvarint(v, tmp);
    return dd_put(o, tmp, n);
}

/* ---- construction ------------------------------------------------------- */

/* The canonical encoding whose CRC-32C is the dictionary id: varint(len) then
 * the bytes, per phrase, in order. Length-prefixed rather than concatenated,
 * so {"ab","c"} and {"a","bc"} are different dictionaries with different ids --
 * which they are, and a plain concatenation would say otherwise. */
static uint32_t dd_compute_id(const uint8_t *const *phrases, const size_t *lens,
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

dyn_dict_t *dyn_dict_new(const uint8_t *const *phrases, const size_t *lens,
                         size_t n)
{
    dyn_dict_t *d;
    size_t i;

    if (n == 0 || n > DYN_DICT_MAX_PHRASES)
        return NULL;
    for (i = 0; i < n; i++) {
        if (lens[i] == 0)
            return NULL; /* matches everywhere and encodes nothing */
    }

    d = (dyn_dict_t *)calloc(1, sizeof(*d));
    if (!d)
        return NULL;
    d->n = n;
    d->phrase = (uint8_t **)calloc(n, sizeof(*d->phrase));
    d->plen = (size_t *)calloc(n, sizeof(*d->plen));
    if (!d->phrase || !d->plen)
        goto fail;

    for (i = 0; i < n; i++) {
        d->phrase[i] = (uint8_t *)malloc(lens[i]);
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

void dyn_dict_free(dyn_dict_t *d)
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

uint32_t dyn_dict_id(const dyn_dict_t *d) { return d ? d->id : 0; }
size_t dyn_dict_count(const dyn_dict_t *d) { return d ? d->n : 0; }

/* ---- encode ------------------------------------------------------------- */

typedef struct {
    dyn_dict_t *d;
    uint32_t *best_len;
    int32_t *best_pat;
} dd_sink_t;

/* Aho-Corasick reports the END of a hit; the encoder wants the START. Record
 * the LONGEST phrase beginning at each position -- longest, not first, because
 * a dictionary containing both "error" and "error_code" should spend one code
 * on the longer one rather than a code plus five literals. */
static int dd_emit(void *ud, int pat, size_t end_byte)
{
    dd_sink_t *s = (dd_sink_t *)ud;
    size_t len = s->d->plen[pat];
    size_t start = end_byte - len;
    if ((uint32_t)len > s->best_len[start]) {
        s->best_len[start] = (uint32_t)len;
        s->best_pat[start] = pat;
    }
    return 0;
}

static int dd_scratch(dyn_dict_t *d, size_t need)
{
    if (need > d->scratch_cap) {
        uint32_t *bl = (uint32_t *)realloc(d->best_len, need * sizeof(uint32_t));
        int32_t *bp;
        if (!bl)
            return -1;
        d->best_len = bl;
        bp = (int32_t *)realloc(d->best_pat, need * sizeof(int32_t));
        if (!bp)
            return -1;
        d->best_pat = bp;
        {
            uint32_t *ca = (uint32_t *)realloc(d->cost_a, (need + 1) * sizeof(uint32_t));
            uint32_t *cb;
            uint8_t *tk;
            if (!ca)
                return -1;
            d->cost_a = ca;
            cb = (uint32_t *)realloc(d->cost_b, (need + 1) * sizeof(uint32_t));
            if (!cb)
                return -1;
            d->cost_b = cb;
            tk = (uint8_t *)realloc(d->take, need + 1);
            if (!tk)
                return -1;
            d->take = tk;
        }
        d->scratch_cap = need;
    }
    /* This one IS cleared per call, unlike the Compressor's head table: the
     * epoch trick there works because a stale entry is detectable by comparing
     * against a monotonically advancing base. Here the value stored is a
     * LENGTH, which has no room for a base, and the alternative -- a parallel
     * epoch array -- costs the same memory traffic as the memset it saves.
     * Measured rather than assumed; the crossover bench is what decides
     * whether this matters at the record sizes a dictionary is for. */
    memset(d->best_len, 0, need * sizeof(uint32_t));
    return 0;
}

int dyn_dict_compress(dyn_dict_t *d, const uint8_t *src, size_t len,
                      dyn_outbuf_t *o)
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

    /* THE PARSE IS A DP, NOT A GREEDY WALK, and that is not a refinement --
     * greedy made this codec EXPAND its own best case. On
     *     {"jsonrpc":"2.0","method":...
     * with a phrase list containing both `{"` and `"jsonrpc":"2.0"`, greedy
     * takes the 2-byte match at position 0 and thereby steps over the 15-byte
     * match at position 1, which it can never come back for. Measured: 54
     * bytes in, 61 out. A capability that loses on the input it exists for is
     * a tax, so the parse has to be able to decline a match.
     *
     * Two states, because the cost of a literal byte depends on whether a run
     * is already open:
     *   A[i] = cheapest encoding of src[i..n) with NO run open
     *   B[i] = cheapest encoding of src[i..n) with a run already open
     * A run costs DD_RUN_HDR to open (the 0 marker plus its length varint) and
     * one byte per literal after that. A phrase costs its code varint and
     * closes any run. The header estimate is exact for runs under 128 bytes
     * and one byte low beyond that, which can only ever make the parse
     * slightly prefer a long run -- it cannot make the output invalid, and the
     * decoder neither knows nor cares how the parse was chosen. */
    {
        uint32_t *A = d->cost_a, *B = d->cost_b;
        uint8_t *take = d->take;
        size_t k;

        A[len] = 0;
        B[len] = 0;
        for (k = len; k-- > 0;) {
            uint32_t bl = d->best_len[k];
            uint32_t phrase = UINT32_MAX;
            uint32_t a, b;

            if (bl) {
                uint8_t tmp[DYN_CODEC_VARINT_MAX];
                uint32_t code_cost =
                    (uint32_t)dyn_codec_put_uvarint((uint64_t)d->best_pat[k] + 1,
                                                    tmp);
                uint32_t rest = A[k + bl];
                if (rest != UINT32_MAX)
                    phrase = code_cost + rest;
            }
            /* open a run and spend one byte on this literal */
            a = (B[k + 1] == UINT32_MAX) ? UINT32_MAX
                                         : DD_RUN_HDR + 1 + B[k + 1];
            /* continue an open run */
            b = (B[k + 1] == UINT32_MAX) ? UINT32_MAX : 1 + B[k + 1];

            if (phrase <= a) { A[k] = phrase; take[k] = 1; }
            else             { A[k] = a;      take[k] = 0; }
            B[k] = (phrase < b) ? phrase : b;
        }

        /* Walk the choices A made. `take[k]` is only consulted at a position
         * the walk actually reaches, which is exactly where A was the state. */
        i = 0;
        lit_start = 0;
        while (i < len) {
            if (!take[i] || d->best_len[i] == 0) {
                i++;
                continue;
            }
            if (i > lit_start) {
                if (dd_put_varint(o, 0) < 0 ||
                    dd_put_varint(o, (uint64_t)(i - lit_start)) < 0 ||
                    dd_put(o, src + lit_start, i - lit_start) < 0)
                    return -1;
            }
            if (dd_put_varint(o, (uint64_t)d->best_pat[i] + 1) < 0)
                return -1;
            i += d->best_len[i];
            lit_start = i;
        }
    }
    if (len > lit_start) {
        if (dd_put_varint(o, 0) < 0 ||
            dd_put_varint(o, (uint64_t)(len - lit_start)) < 0 ||
            dd_put(o, src + lit_start, len - lit_start) < 0)
            return -1;
    }
    return 0;
}

/* ---- decode (untrusted) ------------------------------------------------- */

int dyn_dict_record_id(const uint8_t *src, size_t len, uint32_t *id)
{
    if (len < DYN_DICT_HEADER || src[0] != DYN_DICT_MAGIC0 ||
        src[1] != DYN_DICT_MAGIC1 || src[2] != DYN_DICT_VERSION)
        return -1;
    if (id)
        *id = (uint32_t)src[3] | ((uint32_t)src[4] << 8) |
              ((uint32_t)src[5] << 16) | ((uint32_t)src[6] << 24);
    return 0;
}

int dyn_dict_decompress(const dyn_dict_t *d, const uint8_t *src, size_t len,
                        dyn_outbuf_t *o)
{
    uint32_t rid;
    size_t pos = DYN_DICT_HEADER;
    uint64_t raw_len;
    int used;

    if (!d)
        return -1;
    if (dyn_dict_record_id(src, len, &rid) < 0)
        return -1;
    /* Checked BEFORE a single byte is emitted. A record built against another
     * phrase list decodes to plausible garbage otherwise, because every code in
     * it is still in range -- so the failure would be silent. */
    if (rid != d->id)
        return -1;

    used = dyn_codec_uvarint(src + pos, len - pos, &raw_len);
    if (used <= 0 || raw_len > DYN_MAX_OUTPUT)
        return -1;
    pos += (size_t)used;

    /* Presize from the declared length -- but keep checking as we go, because
     * the declaration is attacker-controlled and is a hint, never a bound. */
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
                return -1;                       /* run past the input */
            if (o->len + rl > DYN_MAX_OUTPUT)
                return -1;
            if (dd_put(o, src + pos, (size_t)rl) < 0)
                return -1;
            pos += (size_t)rl;
        } else {
            size_t idx = (size_t)(code - 1);
            if (code > (uint64_t)d->n)
                return -1;                       /* code out of range */
            if (o->len + d->plen[idx] > DYN_MAX_OUTPUT)
                return -1;
            if (dd_put(o, d->phrase[idx], d->plen[idx]) < 0)
                return -1;
        }
    }

    /* The declared length is verified against what was actually produced. A
     * record that lies about its size is malformed, and saying so is better
     * than handing back a short buffer that looks complete. */
    if (o->len != (size_t)raw_len)
        return -1;
    return 0;
}
