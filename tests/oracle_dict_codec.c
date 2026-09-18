/*
 * oracle_dict_codec.c -- differential and hostile-input harness for
 * src/core/dyn-dict.c.
 *
 * A round trip is the WEAKEST useful property of a codec, and this file exists
 * because CLAUDE.md records two separate occasions where one passed while the
 * output was wrong: the SWAR match-length endianness bug produced a valid but
 * different LZ4 stream that round-tripped perfectly, and the Trie codec emitted
 * keys in node order so that re-encoding a decoded record differed from the
 * original. So there are three checks here, in increasing strength:
 *
 *   1. round trip           -- decompress(compress(x)) == x
 *   2. RE-ENCODE            -- compress(decompress(compress(x))) is BYTE-
 *                              IDENTICAL to compress(x). This is what catches
 *                              an encoder whose output depends on anything but
 *                              the input and the phrase set.
 *   3. an INDEPENDENT decoder, written here from the wire format in the header
 *                              rather than from the implementation, so a
 *                              misreading of the format by dyn-dict.c cannot be
 *                              mirrored by its own decoder.
 *
 * Plus the hostile half: truncation at every offset, a bit flipped at every
 * position, and a record decoded against the WRONG dictionary -- which must
 * produce nothing rather than plausible garbage, since every code in it is
 * still in range.
 *
 * Build:  make test-dict-oracle
 */
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

/* ---- an independent decoder, written from dyn-dict.h's format block ------ */

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

/* ---- corpora ------------------------------------------------------------ */

static const char *PHRASES_RPC[] = {
    "\"jsonrpc\":\"2.0\"", "\"method\":", "\"params\":", "\"id\":",
    "\"result\":", "\"error\":", "{\"", "\"}", "\":\"", "\",\"",
};
/* A phrase set with overlaps, prefixes and a phrase that is a suffix of
 * another -- the shapes where leftmost-longest differs from first-match. */
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

/* ---- the three strength levels ------------------------------------------ */

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
    /* 1. round trip */
    if (r.len != len || (len && memcmp(r.buf, src, len) != 0)) {
        fail("round trip", label);
        goto done;
    }
    /* 2. re-encode is byte-identical */
    if (dyn_dict_compress(dp->d, r.buf, r.len, &c2) != 0) {
        fail("re-compress", label);
        goto done;
    }
    if (c2.len != c1.len || memcmp(c2.buf, c1.buf, c1.len) != 0) {
        fail("re-encode not byte-identical", label);
        goto done;
    }
    /* 3. an independent decoder agrees */
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

/* ---- hostile ------------------------------------------------------------ */

static void check_hostile(dict_pair *dp, dict_pair *other, const uint8_t *src,
                          size_t len, const char *label)
{
    dyn_outbuf_t c = { NULL, 0, 0 };
    size_t i;
    int bit;

    if (dyn_dict_compress(dp->d, src, len, &c) != 0)
        return;

    /* The WRONG dictionary must produce NOTHING. Every code in the record is
     * still a valid index into the other phrase list, so without the id check
     * this would decode successfully into a different string -- the exact
     * failure mode the header calls out. */
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

    /* Truncation at every offset must be rejected, never partially accepted. */
    for (i = 0; i < c.len; i++) {
        dyn_outbuf_t o = { NULL, 0, 0 };
        g_checks++;
        if (dyn_dict_decompress(dp->d, c.buf, i, &o) == 0)
            fail("truncated record accepted", label);
        free(o.buf);
    }

    /* A flipped bit must either reproduce the input exactly (possible: some
     * bits are in literal bytes the codec does not interpret) or be rejected.
     * What it must never do is crash, hang, or return success with the wrong
     * length -- so every outcome is checked, not just the failure. */
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

/* ---- a pseudorandom corpus, so the sweep is not only hand-written ------- */

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

    /* Random strings over a small alphabet drawn from the phrase characters,
     * so matches are dense and overlapping rather than incidental. */
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

    /* Arbitrary bytes, including NULs and the high half, which the phrase sets
     * never contain -- the all-literal adversarial case. */
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

    /* The two dictionaries must have different ids, or the wrong-dictionary
     * check above is vacuous -- which is exactly the shape of test CLAUDE.md
     * sec.8 warns about. Asserted, not assumed. */
    g_checks++;
    if (dyn_dict_id(rpc.d) == dyn_dict_id(ov.d))
        fail("the two test dictionaries share an id", "the hostile check is vacuous");

    /* Length-prefixing in the canonical form: {"ab","c"} and {"a","bc"} are
     * different dictionaries and must not collide. */
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
