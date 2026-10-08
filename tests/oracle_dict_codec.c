#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/core/dyn-dict.h"
#include "../src/core/dyn-codec.h"

static long long g_checks, g_fail;

static void fail(const char *what, const char *detail)
{
    if (g_fail < 20)
        printf("FAIL %-22s %s\n", what, detail);
    g_fail++;
}

static int ref_decode(const uint8_t *const *phrases, const size_t *lens,
                      size_t n, const uint8_t *rec, size_t rlen,
                      uint8_t *out, size_t outcap, size_t *outlen)
{
    size_t pos = 7, o = 0;
    uint64_t raw;
    int used;

    if (rlen < 7 || rec[0] != 'D' || rec[1] != 'T' || rec[2] != 1)
        return -1;
    used = dyn_codec_uvarint(rec + pos, rlen - pos, &raw);
    if (used <= 0)
        return -1;
    pos += (size_t)used;

    while (pos < rlen) {
        uint64_t code;
        used = dyn_codec_uvarint(rec + pos, rlen - pos, &code);
        if (used <= 0)
            return -1;
        pos += (size_t)used;
        if (code == 0) {
            uint64_t rl;
            used = dyn_codec_uvarint(rec + pos, rlen - pos, &rl);
            if (used <= 0)
                return -1;
            pos += (size_t)used;
            if (rl > rlen - pos || o + rl > outcap)
                return -1;
            memcpy(out + o, rec + pos, (size_t)rl);
            o += (size_t)rl;
            pos += (size_t)rl;
        } else {
            size_t idx = (size_t)code - 1;
            if (idx >= n || o + lens[idx] > outcap)
                return -1;
            memcpy(out + o, phrases[idx], lens[idx]);
            o += lens[idx];
        }
    }
    if (o != (size_t)raw)
        return -1;
    *outlen = o;
    return 0;
}

static const char *PHRASES_RPC[] = {
    "\"jsonrpc\":\"2.0\"", "\"method\":", "\"params\":", "\"id\":",
    "\"result\":", "\"error\":", "{\"", "\"}", "\":\"", "\",\"",
};
static const char *PHRASES_OVERLAP[] = {
    "he", "she", "his", "hers", "error", "error_code", "err", "code",
};

static const char *CORPORA[] = {
    "",
    "a",
    "he",
    "ushers",
    "she said hers was an error_code, his err code",
    "{\"jsonrpc\":\"2.0\",\"method\":\"sum\",\"params\":[1,2],\"id\":7}",
    "{\"jsonrpc\":\"2.0\",\"result\":42,\"id\":7}",
    "no phrases appear in this sentence whatsoever",
    "\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"",
    "errorerrorerrorerrorerror_codeerror_codeerrerrerr",
};

typedef struct {
    dyn_dict_t *d;
    const uint8_t **p;
    size_t *l;
    size_t n;
} dict_pair;

static dict_pair build(const char **phrases, size_t n)
{
    dict_pair dp;
    size_t i;
    dp.p = (const uint8_t **)malloc(n * sizeof(*dp.p));
    dp.l = (size_t *)malloc(n * sizeof(*dp.l));
    for (i = 0; i < n; i++) {
        dp.p[i] = (const uint8_t *)phrases[i];
        dp.l[i] = strlen(phrases[i]);
    }
    dp.n = n;
    dp.d = dyn_dict_new(dp.p, dp.l, n);
    if (!dp.d) {
        fprintf(stderr, "dyn_dict_new failed\n");
        exit(2);
    }
    return dp;
}

static void destroy(dict_pair *dp)
{
    dyn_dict_free(dp->d);
    free(dp->p);
    free(dp->l);
}

static void check_corpus(dict_pair *dp, const uint8_t *src, size_t len,
                         const char *label)
{
    dyn_outbuf_t c1 = { NULL, 0, 0 }, r = { NULL, 0, 0 }, c2 = { NULL, 0, 0 };
    uint8_t *refout = NULL;
    size_t reflen = 0;

    g_checks++;
    if (dyn_dict_compress(dp->d, src, len, &c1) != 0) {
        fail("compress", label);
        goto done;
    }
    if (dyn_dict_decompress(dp->d, c1.buf, c1.len, &r) != 0) {
        fail("decompress", label);
        goto done;
    }
    if (r.len != len || (len && memcmp(r.buf, src, len) != 0)) {
        fail("round trip", label);
        goto done;
    }
    if (dyn_dict_compress(dp->d, r.buf, r.len, &c2) != 0) {
        fail("re-compress", label);
        goto done;
    }
    if (c2.len != c1.len || memcmp(c2.buf, c1.buf, c1.len) != 0) {
        fail("re-encode not byte-identical", label);
        goto done;
    }
    refout = (uint8_t *)malloc(len + 16);
    if (ref_decode(dp->p, dp->l, dp->n, c1.buf, c1.len, refout, len + 16,
                   &reflen) != 0) {
        fail("independent decoder rejected a valid record", label);
        goto done;
    }
    if (reflen != len || (len && memcmp(refout, src, len) != 0))
        fail("independent decoder disagrees", label);

done:
    free(c1.buf);
    free(r.buf);
    free(c2.buf);
    free(refout);
}

static void check_hostile(dict_pair *dp, dict_pair *other, const uint8_t *src,
                          size_t len, const char *label)
{
    dyn_outbuf_t c = { NULL, 0, 0 };
    size_t i;
    int bit;

    if (dyn_dict_compress(dp->d, src, len, &c) != 0)
        return;

    {
        dyn_outbuf_t o = { NULL, 0, 0 };
        g_checks++;
        if (dyn_dict_decompress(other->d, c.buf, c.len, &o) == 0) {
            fail("wrong dictionary ACCEPTED", label);
        } else if (o.len != 0) {
            fail("wrong dictionary emitted bytes before failing", label);
        }
        free(o.buf);
    }

    for (i = 0; i < c.len; i++) {
        dyn_outbuf_t o = { NULL, 0, 0 };
        g_checks++;
        if (dyn_dict_decompress(dp->d, c.buf, i, &o) == 0)
            fail("truncated record accepted", label);
        free(o.buf);
    }

    for (i = 0; i < c.len && i < 400; i++) {
        for (bit = 0; bit < 8; bit++) {
            dyn_outbuf_t o = { NULL, 0, 0 };
            int rc;
            c.buf[i] ^= (uint8_t)(1 << bit);
            rc = dyn_dict_decompress(dp->d, c.buf, c.len, &o);
            g_checks++;
            if (rc == 0 && o.len > len + 64)
                fail("flipped bit produced an oversized output", label);
            free(o.buf);
            c.buf[i] ^= (uint8_t)(1 << bit);
        }
    }
    free(c.buf);
}

static uint64_t rng_state = 0x9e3779b97f4a7c15ULL;
static uint64_t rng(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

int main(void)
{
    dict_pair rpc = build(PHRASES_RPC, sizeof(PHRASES_RPC) / sizeof(*PHRASES_RPC));
    dict_pair ov = build(PHRASES_OVERLAP,
                         sizeof(PHRASES_OVERLAP) / sizeof(*PHRASES_OVERLAP));
    size_t i;

    for (i = 0; i < sizeof(CORPORA) / sizeof(*CORPORA); i++) {
        const uint8_t *s = (const uint8_t *)CORPORA[i];
        check_corpus(&rpc, s, strlen(CORPORA[i]), CORPORA[i]);
        check_corpus(&ov, s, strlen(CORPORA[i]), CORPORA[i]);
    }

    {
        static const char AL[] = "hesirc_ode\"{},:0123 ";
        uint8_t buf[512];
        int trial;
        for (trial = 0; trial < 4000; trial++) {
            size_t n = (size_t)(rng() % 300);
            size_t k;
            for (k = 0; k < n; k++)
                buf[k] = (uint8_t)AL[rng() % (sizeof(AL) - 1)];
            check_corpus(&rpc, buf, n, "random");
            check_corpus(&ov, buf, n, "random");
        }
    }

    {
        uint8_t buf[256];
        int trial;
        for (trial = 0; trial < 500; trial++) {
            size_t n = (size_t)(rng() % 200);
            size_t k;
            for (k = 0; k < n; k++)
                buf[k] = (uint8_t)(rng() & 0xff);
            check_corpus(&rpc, buf, n, "raw bytes");
        }
    }

    for (i = 0; i < sizeof(CORPORA) / sizeof(*CORPORA); i++) {
        const uint8_t *s = (const uint8_t *)CORPORA[i];
        check_hostile(&rpc, &ov, s, strlen(CORPORA[i]), CORPORA[i]);
    }

    g_checks++;
    if (dyn_dict_id(rpc.d) == dyn_dict_id(ov.d))
        fail("the two test dictionaries share an id", "the hostile check is vacuous");

    {
        const char *A[] = { "ab", "c" };
        const char *B[] = { "a", "bc" };
        dict_pair pa = build(A, 2), pb = build(B, 2);
        g_checks++;
        if (dyn_dict_id(pa.d) == dyn_dict_id(pb.d))
            fail("id collision", "{ab,c} and {a,bc} hash the same");
        destroy(&pa);
        destroy(&pb);
    }

    destroy(&rpc);
    destroy(&ov);

    printf("oracle_dict_codec: %lld checks, %lld failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
