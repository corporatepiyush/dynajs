/*
 * test_resp_codec.c -- src/core/dyn-resp.c, the RESP2/RESP3 scanner, reader and command encoder of dyna:redis.
 * Fixed checks: per-type wire vectors, limits, error codes, a 200000-field integer sweep against an __int128 oracle.
 * Seeded property tests (tests/lib/dyn-fuzzkit.h + tests/lib/dyn-resp-model.h, shared with src/fuzz/fuzz_net.c):
 * DYN_FUZZ_SEED replays a run, DYN_FUZZ_ITERS (50 = deep) multiplies every loop, DYN_FUZZ_ONLY=case selects.
 *  [resp-tree] random RESP3 trees (every type, attributes, nulls, depth chains past DYN_RESP_MAX_DEPTH, a trailing
 *              partial value) vs the model: verdict, consumed, resumed scan over growing prefixes, every proper prefix
 *              INCOMPLETE, the reader's items in pre-order, verdict under a random maxbulk;
 *              mutants: depth limit off by one, map count not doubled, bulk CRLF check dropped, resume loses position.
 *  [resp-raw]  mutated and truncated trees: one-shot vs resumed scan, prefix closure, the reader's item walk ends
 *              where the scanner ended; mutant: attribute's trailing value not counted (also red in [resp-tree]).
 *  [resp-stream] RESP3 streamed strings and aggregates: verdicts, canonical form, depth 31/32 (an empty 32nd stream is
 *              legal like a fixed *0), count and chunk-total caps at the edge, resumable chunk state; the model streamifies
 *              every valid tree and checks canon(stream) == tree, prefixes, resume and maxbulk verdict parity; canon output
 *              may exceed its input from 10000 elements up (*10000 vs *?+.), bounded by n + n/1024 + 16 (KAT 9999/10000/1e5);
 *              mutants: map parity off, stream count cap off, chunk total cap off, attribute stream without its value,
 *              '.' accepted in a fixed frame, streamed flag not reset, empty stream refused at depth 32, cut header
 *              counted twice, canon map count not halved, stream depth off by one, canon room = n.
 *  [resp-cmd]  random binary argv through cmd_size/cmd_encode: exact need, one byte short refused, round trip;
 *              mutant: cmd_size digit count off by one.
 */
#include "dyn-resp.h"
#include "../lib/dyn-resp-model.h"

#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c, ...)            \
    do {                         \
        if (!(c)) {              \
            printf("FAIL: ");    \
            printf(__VA_ARGS__); \
            printf("\n");        \
            fails++;             \
        }                        \
    } while (0)

#define countof(a) (sizeof(a) / sizeof((a)[0]))
static unsigned long long rnd(unsigned long long* s)
{
    *s ^= *s << 13;
    *s ^= *s >> 7;
    *s ^= *s << 17;
    return *s;
}

#define SCAN(s, cap, out) \
    dyn_resp_scan((const uint8_t*)(s), strlen(s), (cap), (out))

static fk_rng_t fk_g;
static rm_msg_t rm_m;

static void prop_tree(void)
{
    long it, N = (long)fk_iters(3000);
    rm_src_t s = { NULL, 0, &fk_g };
    fk_g = fk_stream("resp-tree");
    for (it = 0; it < N; it++) {
        const char* e;
        rm_gen(&s, &rm_m);
        e = rm_check_tree(&rm_m, &s);
        FK_CHECK(e == NULL, "%s (item %ld, %zu bytes)", e, it, rm_m.len);
    }
}

static void prop_raw(void)
{
    long it, N = (long)fk_iters(3000);
    rm_src_t s = { NULL, 0, &fk_g };
    fk_g = fk_stream("resp-raw");
    for (it = 0; it < N; it++) {
        const char* e;
        size_t n, mb = fk_chance(&fk_g, 25) ? 1 + fk_below(&fk_g, 64) : 0;
        rm_gen(&s, &rm_m);
        n = rm_m.len + rm_m.tail;
        fk_mutate(&fk_g, rm_m.buf, n, fk_below(&fk_g, 4));
        if (fk_chance(&fk_g, 20))
            n -= fk_below(&fk_g, (uint32_t)n);
        e = rm_check_raw(rm_m.buf, n, mb, &s);
        FK_CHECK(e == NULL, "%s (item %ld, %zu bytes, maxbulk %zu)", e, it, n, mb);
    }
}

static void prop_cmd(void)
{
    long it, N = (long)fk_iters(1000);
    rm_src_t s = { NULL, 0, &fk_g };
    fk_g = fk_stream("resp-cmd");
    for (it = 0; it < N; it++) {
        const char* e = rm_check_cmd(&s);
        FK_CHECK(e == NULL, "%s (item %ld)", e, it);
    }
}

typedef struct {
    const char* wire;
    size_t len, maxbulk;
    int rc;
    const char* canon;
    size_t clen;
} rs_case_t;

#define RS(w, mb, rc, c) { w, sizeof(w) - 1, mb, rc, c, sizeof(c) - 1 }
#define RSE(w, mb, rc) { w, sizeof(w) - 1, mb, rc, NULL, 0 }

static void rs_nest(char* o, const char* open, const char* close, int d)
{
    int i;
    o[0] = 0;
    for (i = 0; i < d; i++)
        strcat(o, open);
    strcat(o, ":7\r\n");
    for (i = 0; i < d; i++)
        strcat(o, close);
}

static void stream_cases(void)
{
    static const rs_case_t C[] = {
        RS("*?\r\n:1\r\n:2\r\n.\r\n", 0, DYN_RESP_OK, "*2\r\n:1\r\n:2\r\n"),
        RS("*?\r\n.\r\n", 0, DYN_RESP_OK, "*0\r\n"),
        RS("~?\r\n+a\r\n.\r\n", 0, DYN_RESP_OK, "~1\r\n+a\r\n"),
        RS("%?\r\n+a\r\n:1\r\n+b\r\n:2\r\n.\r\n", 0, DYN_RESP_OK, "%2\r\n+a\r\n:1\r\n+b\r\n:2\r\n"),
        RS("|?\r\n+k\r\n:0\r\n.\r\n:7\r\n", 0, DYN_RESP_OK, "|1\r\n+k\r\n:0\r\n:7\r\n"),
        RS("|?\r\n.\r\n*?\r\n.\r\n", 0, DYN_RESP_OK, "|0\r\n*0\r\n"),
        RS("$?\r\n;4\r\nHell\r\n;5\r\no wor\r\n;1\r\nd\r\n;0\r\n", 0, DYN_RESP_OK, "$10\r\nHello word\r\n"),
        RS("$?\r\n;0\r\n", 0, DYN_RESP_OK, "$0\r\n\r\n"),
        RS("$?\r\n;3\r\na\0b\r\n;0\r\n", 0, DYN_RESP_OK, "$3\r\na\0b\r\n"),
        RS("$?\r\n;2\r\n\r\n\r\n;0\r\n", 0, DYN_RESP_OK, "$2\r\n\r\n\r\n"),
        RS("*?\r\n*?\r\n$?\r\n;2\r\nab\r\n;0\r\n.\r\n%?\r\n.\r\n$-1\r\n_\r\n.\r\n", 0, DYN_RESP_OK, "*4\r\n*1\r\n$2\r\nab\r\n%0\r\n$-1\r\n_\r\n"),
        RS("*2\r\n$?\r\n;1\r\nx\r\n;0\r\n:5\r\n", 0, DYN_RESP_OK, "*2\r\n$1\r\nx\r\n:5\r\n"),
        RS(">2\r\n+m\r\n*?\r\n.\r\n", 0, DYN_RESP_OK, ">2\r\n+m\r\n*0\r\n"),
        RS("*?\r\n:1\r\n:2\r\n:3\r\n.\r\n", 9, DYN_RESP_OK, "*3\r\n:1\r\n:2\r\n:3\r\n"),
        RSE("*?\r\n:1\r\n:2\r\n:3\r\n:4\r\n.\r\n", 9, DYN_RESP_E_COUNT),
        RSE("%?\r\n:1\r\n:2\r\n:3\r\n:4\r\n.\r\n", 9, DYN_RESP_E_COUNT),
        RS("$?\r\n;3\r\nabc\r\n;2\r\nde\r\n;0\r\n", 5, DYN_RESP_OK, "$5\r\nabcde\r\n"),
        RSE("$?\r\n;3\r\nabc\r\n;3\r\ndef\r\n;0\r\n", 5, DYN_RESP_E_TOOBIG),
        RSE("$?\r\n;6\r\nabcdef\r\n;0\r\n", 5, DYN_RESP_E_TOOBIG),
        RSE("%?\r\n+a\r\n.\r\n", 0, DYN_RESP_E_SYNTAX),
        RSE("|?\r\n+a\r\n.\r\n:1\r\n", 0, DYN_RESP_E_SYNTAX),
        RSE(".\r\n", 0, DYN_RESP_E_TYPE),
        RSE("*2\r\n:1\r\n.\r\n", 0, DYN_RESP_E_TYPE),
        RSE("*?\r\n.x\r\n", 0, DYN_RESP_E_SYNTAX),
        RSE("$?\r\n;-1\r\n", 0, DYN_RESP_E_SYNTAX),
        RSE("$?\r\nabc\r\n", 0, DYN_RESP_E_SYNTAX),
        RSE("$?\r\n;3\r\nabcX\r\n;0\r\n", 0, DYN_RESP_E_SYNTAX),
        RSE("$?\r\n;\r\n", 0, DYN_RESP_E_SYNTAX),
        RSE("$?\r\n.\r\n", 0, DYN_RESP_E_SYNTAX),
        RSE(">?\r\n.\r\n", 0, DYN_RESP_E_SYNTAX),
        RSE("!?\r\n;0\r\n", 0, DYN_RESP_E_SYNTAX),
        RSE("=?\r\n;0\r\n", 0, DYN_RESP_E_SYNTAX),
        RSE("*??\r\n.\r\n", 0, DYN_RESP_E_SYNTAX),
        RSE("$??\r\n;0\r\n", 0, DYN_RESP_E_SYNTAX),
        RSE("*?\r\n", 0, DYN_RESP_INCOMPLETE),
        RSE("*?\r\n:1\r\n.\r", 0, DYN_RESP_INCOMPLETE),
        RSE("$?\r\n;0\r", 0, DYN_RESP_INCOMPLETE),
        RSE("$?\r\n;4\r\nab", 0, DYN_RESP_INCOMPLETE),
        RSE("|?\r\n.\r\n", 0, DYN_RESP_INCOMPLETE),
    };
    static const char* const EMPTY[] = { "*?\r\n.\r\n", "%?\r\n.\r\n", "~?\r\n.\r\n", "*0\r\n", "|?\r\n.\r\n", "*?\r\n:1\r\n.\r\n" };
    static char w[4096], w2[4096];
    static uint8_t cb[8192];
    size_t k, used, cl;
    int d, rc, e2;
    dyn_resp_scan_t st;

    for (k = 0; k < sizeof C / sizeof C[0]; k++) {
        used = 7;
        rc = dyn_resp_scan((const uint8_t*)C[k].wire, C[k].len, C[k].maxbulk, &used);
        FK_CHECK(rc == C[k].rc, "stream case %zu: rc %d, want %d", k, rc, C[k].rc);
        if (rc != DYN_RESP_OK) {
            FK_CHECK(used == 0, "stream case %zu: a failed scan reported %zu consumed", k, used);
            continue;
        }
        FK_CHECK(used == C[k].len, "stream case %zu: consumed %zu of %zu", k, used, C[k].len);
        rc = dyn_resp_canon((const uint8_t*)C[k].wire, used, cb, sizeof cb, &cl);
        FK_CHECK(rc == DYN_RESP_OK && cl == C[k].clen && !memcmp(cb, C[k].canon, cl), "stream case %zu: canon differs", k);
        rc = dyn_resp_canon((const uint8_t*)C[k].wire, used, cb, dyn_resp_canon_cap(used) - 1, &cl);
        FK_CHECK(rc == DYN_RESP_E_SYNTAX, "stream case %zu: canon accepted a buffer one byte short of canon_cap", k);
    }

    for (d = 30; d <= 33; d++) {
        int i;
        rs_nest(w, "*?\r\n", ".\r\n", d);
        rc = SCAN(w, 0, &used);
        FK_CHECK(rc == (d <= 31 ? DYN_RESP_OK : DYN_RESP_E_DEPTH), "%d nested *? streams: rc %d", d, rc);
        rs_nest(w2, "|?\r\n.\r\n", "", d);
        rc = SCAN(w2, 0, &used);
        FK_CHECK(rc == (d <= 31 ? DYN_RESP_OK : DYN_RESP_E_DEPTH), "%d streamed attributes: rc %d (fixed |0 chain limit is 31)", d, rc);
        w[0] = 0;
        for (i = 0; i < d; i++)
            strcat(w, "*?\r\n:1\r\n");
        strcat(w, ":7\r\n");
        for (i = 0; i < d; i++)
            strcat(w, ".\r\n");
        rc = SCAN(w, 0, &used);
        FK_CHECK(rc == (d <= 31 ? DYN_RESP_OK : DYN_RESP_E_DEPTH), "%d streams each with an element first: rc %d", d, rc);
        if (rc == DYN_RESP_OK) {
            rc = dyn_resp_canon((const uint8_t*)w, used, cb, sizeof cb, &cl);
            FK_CHECK(rc == DYN_RESP_OK && cl < used && dyn_resp_scan(cb, cl, 0, &k) == DYN_RESP_OK && k == cl, "%d nested streams: canon does not rescan", d);
        }
    }

    for (e2 = 0; e2 < 6; e2++) {
        size_t L, cut;
        int i, want2 = e2 < 4 ? DYN_RESP_OK : DYN_RESP_E_DEPTH;
        w[0] = 0;
        for (i = 0; i < 31; i++)
            strcat(w, "*?\r\n");
        strcat(w, EMPTY[e2]);
        for (i = 0; i < 31; i++)
            strcat(w, ".\r\n");
        L = strlen(w);
        rc = SCAN(w, 0, &used);
        FK_CHECK(rc == want2 && (rc != DYN_RESP_OK || used == L), "31 streams around %s: rc %d (a 32nd container is legal only when empty, as with fixed *0)", EMPTY[e2], rc);
        if (rc != DYN_RESP_OK)
            continue;
        FK_CHECK(dyn_resp_canon((const uint8_t*)w, L, cb, sizeof cb, &cl) == DYN_RESP_OK && dyn_resp_scan(cb, cl, 0, &k) == DYN_RESP_OK && k == cl, "empty stream at max depth: canon does not rescan");
        for (cut = 124; cut < 131; cut++) {
            dyn_resp_scan_init(&st);
            rc = dyn_resp_scan_resume(&st, (const uint8_t*)w, cut, 0, &used);
            FK_CHECK(rc == DYN_RESP_INCOMPLETE, "empty stream at max depth cut at %zu: rc %d", cut, rc);
            rc = dyn_resp_scan_resume(&st, (const uint8_t*)w, L, 0, &used);
            FK_CHECK(rc == DYN_RESP_OK && used == L, "empty stream at max depth resumed from %zu: rc %d", cut, rc);
        }
    }

    {
        size_t L, cut;
        int i;
        w[0] = 0;
        for (i = 0; i < 30; i++)
            strcat(w, "*?\r\n");
        strcat(w, "%?\r\n+k\r\n*?\r\n.\r\n");
        for (i = 0; i < 31; i++)
            strcat(w, ".\r\n");
        L = strlen(w);
        for (cut = 128; cut < 135; cut++) {
            dyn_resp_scan_init(&st);
            rc = dyn_resp_scan_resume(&st, (const uint8_t*)w, cut, 0, &used);
            FK_CHECK(rc == DYN_RESP_INCOMPLETE, "map value = empty stream at max depth cut at %zu: rc %d", cut, rc);
            rc = dyn_resp_scan_resume(&st, (const uint8_t*)w, L, 0, &used);
            FK_CHECK(rc == DYN_RESP_OK && used == L, "map value = empty stream at max depth resumed from %zu: rc %d (a cut must not count the header twice)", cut, rc);
        }
    }

    dyn_resp_scan_init(&st);
    rc = dyn_resp_scan_resume(&st, (const uint8_t*)"*?\r\n.\r\n", 7, 0, &used);
    FK_CHECK(rc == DYN_RESP_OK && st.streamed == 1, "streamed flag not set after a streamed message");
    rc = dyn_resp_scan_resume(&st, (const uint8_t*)"+OK\r\n", 5, 0, &used);
    FK_CHECK(rc == DYN_RESP_OK && st.streamed == 0, "streamed flag leaked into the next fixed message");
    rc = dyn_resp_scan_resume(&st, (const uint8_t*)"$?\r\n;1", 6, 0, &used);
    FK_CHECK(rc == DYN_RESP_INCOMPLETE && st.chunk == 1, "a cut chunk header must leave the scanner in chunk mode");
    rc = dyn_resp_scan_resume(&st, (const uint8_t*)"$?\r\n;1\r\nz\r\n;0\r\n", 15, 0, &used);
    FK_CHECK(rc == DYN_RESP_OK && used == 15 && st.streamed == 1, "resumed chunked string: rc %d used %zu", rc, used);
    dyn_resp_scan_init(&st);
    rc = dyn_resp_scan_resume(&st, (const uint8_t*)"$?\r\n;9", 6, 4, &used);
    rc = dyn_resp_scan_resume(&st, (const uint8_t*)"$?\r\n;9\r\n", 8, 4, &used);
    FK_CHECK(rc == DYN_RESP_E_TOOBIG && st.chunk == 0 && st.sp == 0 && st.pos == 0, "an over-cap chunk must reset the scanner, rc %d", rc);

    {
        size_t big = 3u * 20000u, n2;
        char* m = (char*)malloc(4 + 4 * (big + 1) + 3 + 1);
        FK_CHECK(m != NULL, "alloc");
        if (m) {
            memcpy(m, "*?\r\n", 4);
            n2 = 4;
            for (k = 0; k < big / 3 + 1; k++) {
                memcpy(m + n2, ":1\r\n", 4);
                n2 += 4;
            }
            memcpy(m + n2, ".\r\n", 3);
            n2 += 3;
            FK_CHECK(dyn_resp_scan((const uint8_t*)m, n2, big, &used) == DYN_RESP_E_COUNT, "a stream one element past maxbulk/3 must be E_COUNT");
            FK_CHECK(dyn_resp_scan((const uint8_t*)m, n2 - 7, big, &used) == DYN_RESP_INCOMPLETE, "a stream exactly at maxbulk/3 elements with no end must be INCOMPLETE");
            memcpy(m + n2 - 7, ".\r\n", 3);
            FK_CHECK(dyn_resp_scan((const uint8_t*)m, n2 - 4, big, &used) == DYN_RESP_OK && used == n2 - 4, "a stream exactly at maxbulk/3 elements must scan");
            free(m);
        }
    }
    {
        static const size_t KN[] = { 9999, 10000, 100000 };
        size_t q;
        for (q = 0; q < 3; q++) {
            size_t kk = KN[q], sl = 4, fl, j, ol = 0, cc;
            char* sm = (char*)malloc(4 * kk + 16);
            char* fm = (char*)malloc(4 * kk + 32);
            uint8_t* ob;
            FK_CHECK(sm && fm, "alloc");
            if (!sm || !fm) {
                free(sm);
                free(fm);
                continue;
            }
            memcpy(sm, "*?\r\n", 4);
            fl = (size_t)sprintf(fm, "*%zu\r\n", kk);
            for (j = 0; j < kk; j++) {
                memcpy(sm + sl, ":1\r\n", 4);
                memcpy(fm + fl, ":1\r\n", 4);
                sl += 4;
                fl += 4;
            }
            memcpy(sm + sl, ".\r\n", 3);
            sl += 3;
            cc = dyn_resp_canon_cap(sl);
            ob = (uint8_t*)malloc(cc);
            rc = ob ? dyn_resp_canon((const uint8_t*)sm, sl, ob, cc, &ol) : -99;
            FK_CHECK(rc == DYN_RESP_OK && ol == fl && !memcmp(ob, fm, fl), "KAT %zu-element stream: canon rc %d len %zu, fixed %zu (streamed input %zu)", kk, rc, ol, fl, sl);
            free(ob);
            free(sm);
            free(fm);
        }
    }
    FK_CHECK(dyn_resp_canon_cap(0) == 52 && dyn_resp_canon_cap((size_t)-1) == 0, "canon_cap edges");
    FK_CHECK(dyn_resp_canon(NULL, 1, cb, sizeof cb, &cl) == DYN_RESP_E_SYNTAX && dyn_resp_canon(cb, 0, cb, sizeof cb, &cl) == DYN_RESP_E_SYNTAX, "canon argument guards");
}

int main(void)
{
    size_t used;
    int rc;

    setvbuf(stdout, NULL, _IOLBF, 0);
    fk_init("test_resp_codec");

    {
        static const struct {
            const char* wire;
            size_t len;
            int type;
        } ok[] = {
            { "+OK\r\n", 5, DYN_RESP_SIMPLE },
            { "-ERR nope\r\n", 11, DYN_RESP_ERROR },
            { ":1234\r\n", 7, DYN_RESP_INT },
            { ":-9\r\n", 5, DYN_RESP_INT },
            { "$5\r\nhello\r\n", 11, DYN_RESP_BULK },
            { "$0\r\n\r\n", 6, DYN_RESP_BULK },
            { "$-1\r\n", 5, DYN_RESP_BULK },
            { "*-1\r\n", 5, DYN_RESP_ARRAY },
            { "*0\r\n", 4, DYN_RESP_ARRAY },
            { "_\r\n", 3, DYN_RESP_NULL },
            { "#t\r\n", 4, DYN_RESP_BOOL },
            { "#f\r\n", 4, DYN_RESP_BOOL },
            { ",3.25\r\n", 7, DYN_RESP_DOUBLE },
            { ",inf\r\n", 6, DYN_RESP_DOUBLE },
            { ",-inf\r\n", 7, DYN_RESP_DOUBLE },
            { "(3492890328409238\r\n", 19, DYN_RESP_BIGNUM },
            { "!4\r\noops\r\n", 10, DYN_RESP_BLOBERR },
            { "=8\r\ntxt:abcd\r\n", 14, DYN_RESP_VERB },
        };
        size_t i;
        for (i = 0; i < sizeof(ok) / sizeof(ok[0]); i++) {
            dyn_resp_reader_t r;
            dyn_resp_item_t it;
            rc = SCAN(ok[i].wire, 0, &used);
            CHECK(rc == DYN_RESP_OK, "scan(%s) = %s",
                ok[i].type == DYN_RESP_NULL ? "_" : ok[i].wire,
                dyn_resp_strerror(rc));
            CHECK(used == ok[i].len, "case %zu consumed %zu, want %zu",
                i, used, ok[i].len);
            dyn_resp_reader_init(&r, (const uint8_t*)ok[i].wire, ok[i].len);
            CHECK(dyn_resp_next(&r, &it) == DYN_RESP_OK, "read case %zu", i);
            CHECK(it.type == ok[i].type, "case %zu type %c, want %c",
                i, it.type, ok[i].type);
        }
    }

    {
        dyn_resp_reader_t r;
        dyn_resp_item_t it;
        dyn_resp_reader_init(&r, (const uint8_t*)"$5\r\nhe\r\no\r\n", 11);
        CHECK(dyn_resp_next(&r, &it) == DYN_RESP_OK, "bulk with an embedded CRLF");
        CHECK(it.slen == 5 && memcmp(it.str, "he\r\no", 5) == 0,
            "a length-prefixed bulk carries CRLF as DATA");
        CHECK(r.pos == 11, "and consumed the whole thing");

        dyn_resp_reader_init(&r, (const uint8_t*)":-9\r\n", 5);
        dyn_resp_next(&r, &it);
        CHECK(it.ival == -9, "negative integer decoded as %lld", (long long)it.ival);

        dyn_resp_reader_init(&r, (const uint8_t*)",3.25\r\n", 7);
        dyn_resp_next(&r, &it);
        CHECK(it.dval == 3.25, "double decoded as %g", it.dval);

        dyn_resp_reader_init(&r, (const uint8_t*)",inf\r\n", 6);
        dyn_resp_next(&r, &it);
        CHECK(it.dval > 1e308, "inf decoded as %g", it.dval);

        dyn_resp_reader_init(&r, (const uint8_t*)"#f\r\n", 4);
        dyn_resp_next(&r, &it);
        CHECK(it.ival == 0, "#f is false");

        dyn_resp_reader_init(&r, (const uint8_t*)"$-1\r\n", 5);
        dyn_resp_next(&r, &it);
        CHECK(it.isnull && it.str == NULL, "a null bulk is null, not empty");

        dyn_resp_reader_init(&r, (const uint8_t*)"$0\r\n\r\n", 6);
        dyn_resp_next(&r, &it);
        CHECK(!it.isnull && it.slen == 0, "an EMPTY bulk is not null");
    }

    {
        dyn_resp_reader_t r;
        dyn_resp_item_t it;
        const char* m = "%2\r\n+a\r\n:1\r\n+b\r\n:2\r\n";
        rc = SCAN(m, 0, &used);
        CHECK(rc == DYN_RESP_OK && used == strlen(m),
            "a 2-pair map scans whole: %s, %zu", dyn_resp_strerror(rc), used);
        dyn_resp_reader_init(&r, (const uint8_t*)m, strlen(m));
        dyn_resp_next(&r, &it);
        CHECK(it.count == 2, "map reports PAIRS (%lld), not values",
            (long long)it.count);

        CHECK(SCAN("~2\r\n:1\r\n:2\r\n", 0, &used) == DYN_RESP_OK, "set");
        CHECK(SCAN(">3\r\n$7\r\nmessage\r\n$2\r\nch\r\n$2\r\nhi\r\n", 0, &used)
                == DYN_RESP_OK,
            "push (a RESP3 pub/sub delivery)");
        CHECK(SCAN("*2\r\n*1\r\n:1\r\n*1\r\n:2\r\n", 0, &used) == DYN_RESP_OK,
            "nested arrays");
    }

    {
        static const char* msgs[] = {
            "+OK\r\n",
            "$5\r\nhello\r\n",
            "*2\r\n$3\r\nfoo\r\n:7\r\n",
            "%1\r\n+k\r\n*2\r\n:1\r\n:2\r\n",
            ">3\r\n$7\r\nmessage\r\n$1\r\na\r\n$1\r\nb\r\n",
        };
        size_t i, k;
        for (i = 0; i < sizeof(msgs) / sizeof(msgs[0]); i++) {
            size_t n = strlen(msgs[i]);
            for (k = 0; k < n; k++) {
                rc = dyn_resp_scan((const uint8_t*)msgs[i], k, 0, &used);
                CHECK(rc == DYN_RESP_INCOMPLETE,
                    "prefix %zu of msg %zu gave %s, want INCOMPLETE",
                    k, i, dyn_resp_strerror(rc));
            }
            rc = dyn_resp_scan((const uint8_t*)msgs[i], n, 0, &used);
            CHECK(rc == DYN_RESP_OK && used == n, "the whole of msg %zu", i);
        }
    }

    {
        const char* two = "+OK\r\n:42\r\n";
        rc = SCAN(two, 0, &used);
        CHECK(rc == DYN_RESP_OK && used == 5,
            "pipelined replies must not be merged, consumed %zu", used);
        rc = dyn_resp_scan((const uint8_t*)two + 5, 5, 0, &used);
        CHECK(rc == DYN_RESP_OK && used == 5, "and the second parses alone");
    }

    {
        rc = SCAN("*2000000000\r\n", 0, &used);
        CHECK(rc == DYN_RESP_E_COUNT, "a 2e9-element array gave %s",
            dyn_resp_strerror(rc));
        rc = SCAN("%2000000000\r\n", 0, &used);
        CHECK(rc == DYN_RESP_E_COUNT, "a 2e9-PAIR map gave %s",
            dyn_resp_strerror(rc));
        CHECK(SCAN("*2\r\n:1\r\n:2\r\n", 0, &used) == DYN_RESP_OK,
            "a small array must still pass");

        rc = SCAN("$99999999999\r\n", 0, &used);
        CHECK(rc == DYN_RESP_E_TOOBIG, "a 100 GB bulk gave %s",
            dyn_resp_strerror(rc));
        rc = SCAN("$99999999999999999999999999\r\n", 0, &used);
        CHECK(rc == DYN_RESP_E_INT, "a length that OVERFLOWS int64 gave %s",
            dyn_resp_strerror(rc));
        rc = SCAN("$100\r\n", 64, &used);
        CHECK(rc == DYN_RESP_E_TOOBIG, "maxbulk=64 must refuse a 100-byte bulk");
        rc = SCAN("$4\r\nabcd\r\n", 64, &used);
        CHECK(rc == DYN_RESP_OK, "and still accept one under it");
    }

    {
        char deep[4 * 64];
        size_t i, w = 0;
        for (i = 0; i < 40; i++) {
            memcpy(deep + w, "*1\r\n", 4);
            w += 4;
        }
        rc = dyn_resp_scan((const uint8_t*)deep, w, 0, &used);
        CHECK(rc == DYN_RESP_E_DEPTH, "40 levels of nesting gave %s",
            dyn_resp_strerror(rc));
        w = 0;
        for (i = 0; i < DYN_RESP_MAX_DEPTH - 1; i++) {
            memcpy(deep + w, "*1\r\n", 4);
            w += 4;
        }
        memcpy(deep + w, ":1\r\n", 4);
        w += 4;
        rc = dyn_resp_scan((const uint8_t*)deep, w, 0, &used);
        CHECK(rc == DYN_RESP_OK, "%d levels must pass, got %s",
            DYN_RESP_MAX_DEPTH - 1, dyn_resp_strerror(rc));
    }

    {
        rc = dyn_resp_scan((const uint8_t*)"+OK\n", 4, 0, &used);
        CHECK(rc == DYN_RESP_E_SYNTAX, "a BARE LF must not terminate a line");
        rc = dyn_resp_scan((const uint8_t*)"+OK\rX", 5, 0, &used);
        CHECK(rc == DYN_RESP_E_SYNTAX, "CR not followed by LF");
        rc = dyn_resp_scan((const uint8_t*)"@1\r\n", 4, 0, &used);
        CHECK(rc == DYN_RESP_E_TYPE, "an unknown type byte gave %s",
            dyn_resp_strerror(rc));
        rc = dyn_resp_scan((const uint8_t*)"$-2\r\n", 5, 0, &used);
        CHECK(rc == DYN_RESP_E_SYNTAX, "only $-1 is null; $-2 is not");
        rc = dyn_resp_scan((const uint8_t*)"*-2\r\n", 5, 0, &used);
        CHECK(rc == DYN_RESP_E_SYNTAX, "only *-1 is a null array");
        rc = dyn_resp_scan((const uint8_t*)"%-1\r\n", 5, 0, &used);
        CHECK(rc == DYN_RESP_E_SYNTAX, "a map has no null form");
        rc = dyn_resp_scan((const uint8_t*)":12x4\r\n", 7, 0, &used);
        CHECK(rc == DYN_RESP_E_SYNTAX, "a non-numeric integer");
        rc = dyn_resp_scan((const uint8_t*)":\r\n", 3, 0, &used);
        CHECK(rc == DYN_RESP_E_SYNTAX, "an EMPTY integer");
        rc = dyn_resp_scan((const uint8_t*)"#x\r\n", 4, 0, &used);
        CHECK(rc == DYN_RESP_E_SYNTAX, "a boolean that is neither t nor f");
        rc = dyn_resp_scan((const uint8_t*)"_x\r\n", 4, 0, &used);
        CHECK(rc == DYN_RESP_E_SYNTAX, "RESP3 null carries no payload");
        {
            static char runaway[DYN_RESP_MAX_LINE + 64];
            memset(runaway, 'a', sizeof(runaway));
            runaway[0] = '+';
            rc = dyn_resp_scan((const uint8_t*)runaway, sizeof(runaway), 0, &used);
            CHECK(rc == DYN_RESP_E_TOOBIG,
                "a line longer than the cap must be refused, got %s",
                dyn_resp_strerror(rc));
        }
    }

    {
        const char* w = "|1\r\n+ttl\r\n:60\r\n+OK\r\n";
        rc = SCAN(w, 0, &used);
        CHECK(rc == DYN_RESP_OK && used == strlen(w),
            "attribute+reply must consume BOTH: %s, %zu of %zu",
            dyn_resp_strerror(rc), used, strlen(w));
        rc = dyn_resp_scan((const uint8_t*)w, 14, 0, &used);
        CHECK(rc == DYN_RESP_INCOMPLETE, "an attribute with no reply after it");
    }

    {
        uint8_t out[256];
        const char* argv[3] = { "SET", "key", "value" };
        size_t need;
        int n = dyn_resp_cmd_encode(out, sizeof(out), 3, argv, NULL, &need);
        const char* want = "*3\r\n$3\r\nSET\r\n$3\r\nkey\r\n$5\r\nvalue\r\n";
        CHECK(n == (int)strlen(want), "encode wrote %d, want %zu", n, strlen(want));
        CHECK(n > 0 && memcmp(out, want, (size_t)n) == 0, "exact wire bytes");
        CHECK(need == strlen(want), "size prediction must match what was written");
        CHECK(dyn_resp_cmd_size(3, argv, NULL) == strlen(want),
            "and dyn_resp_cmd_size must agree with the encoder");

        {
            const char* evil[3] = { "SET", "k", "v\r\n*1\r\n$8\r\nFLUSHALL\r\n" };
            uint8_t buf[256];
            size_t elen[3] = { (size_t)-1, (size_t)-1, 21 };
            dyn_resp_reader_t r;
            dyn_resp_item_t it;
            int m = dyn_resp_cmd_encode(buf, sizeof(buf), 3, evil, elen, NULL);
            CHECK(m > 0, "encode the hostile value");
            rc = dyn_resp_scan(buf, (size_t)m, 0, &used);
            CHECK(rc == DYN_RESP_OK && used == (size_t)m,
                "the hostile command must scan as exactly ONE value "
                "(%s, %zu of %d)",
                dyn_resp_strerror(rc), used, m);
            dyn_resp_reader_init(&r, buf, (size_t)m);
            dyn_resp_next(&r, &it);
            CHECK(it.type == DYN_RESP_ARRAY && it.count == 3,
                "one array of 3, got type %c count %lld",
                it.type, (long long)it.count);
            dyn_resp_next(&r, &it);
            dyn_resp_next(&r, &it);
            dyn_resp_next(&r, &it);
            CHECK(it.slen == 21 && memcmp(it.str, "v\r\n*1\r\n$8\r\nFLUSHALL\r\n", 21) == 0,
                "FLUSHALL stayed inside the third ARGUMENT");
        }

        {
            const char* nul[2] = { "SET", "a\0b" };
            size_t nlen[2] = { 3, 3 };
            uint8_t buf[64];
            int m = dyn_resp_cmd_encode(buf, sizeof(buf), 2, nul, nlen, NULL);
            CHECK(m > 0 && memcmp(buf + 13, "$3\r\na\0b\r\n", 9) == 0,
                "an embedded NUL survives encoding");
        }

        CHECK(dyn_resp_cmd_encode(out, 8, 3, argv, NULL, &need) == DYN_RESP_E_TOOBIG,
            "a short output buffer must be refused");
        CHECK(need == strlen(want), "and must still report what was needed");
        CHECK(dyn_resp_cmd_encode(out, sizeof(out), 0, argv, NULL, NULL)
                == DYN_RESP_E_SYNTAX,
            "a command with no name");
    }

    {
        static const struct {
            const char* wire;
            int ok;
            double want;
        } d[] = {
            { ",3.25\r\n", 1, 3.25 },
            { ",-0.5\r\n", 1, -0.5 },
            { ",10\r\n", 1, 10.0 },
            { ",1e3\r\n", 1, 1000.0 },
            { ",1.5E-2\r\n", 1, 0.015 },
            { ",+2.5\r\n", 1, 2.5 },
            { ",3,25\r\n", 0, 0 },
            { ",1.2.3\r\n", 0, 0 },
            { ",0x10\r\n", 0, 0 },
            { ",1e\r\n", 0, 0 },
            { ", 1.0\r\n", 0, 0 },
            { ",1.0junk\r\n", 0, 0 },
            { ",.\r\n", 0, 0 },
            { ",1e300\r\n", 1, 1e300 },
            { ",-1e300\r\n", 1, -1e300 },
            { ",1e-300\r\n", 1, 1e-300 },
            { ",1.7976931348623157e308\r\n", 1, 1.7976931348623157e308 },
            { ",2.2250738585072014e-308\r\n", 1, 2.2250738585072014e-308 },
            { ",5e-324\r\n", 1, 5e-324 },
            { ",3.141592653589793\r\n", 1, 3.141592653589793 },
            { ",0.1\r\n", 1, 0.1 },
            { ",9007199254740993\r\n", 1, 9007199254740992.0 },
            { ",1234567890123456789\r\n", 1, 1234567890123456768.0 },
        };
        size_t i;
        for (i = 0; i < sizeof(d) / sizeof(d[0]); i++) {
            dyn_resp_reader_t r;
            dyn_resp_item_t it;
            size_t n = strlen(d[i].wire);
            int rc2;
            dyn_resp_reader_init(&r, (const uint8_t*)d[i].wire, n);
            rc2 = dyn_resp_next(&r, &it);
            if (d[i].ok) {
                CHECK(rc2 == DYN_RESP_OK, "'%s' must parse (%s)", d[i].wire,
                    dyn_resp_strerror(rc2));
                CHECK(rc2 != DYN_RESP_OK || it.dval == d[i].want,
                    "'%s' -> %.10g, want %.10g", d[i].wire, it.dval, d[i].want);
            } else {
                CHECK(rc2 != DYN_RESP_OK,
                    "'%s' must be REFUSED, got %.10g", d[i].wire, it.dval);
            }
        }
    }

    {
        static const struct {
            const char* wire;
            int64_t want;
            int over;
        } edge[] = {
            { ":0\r\n", 0, 0 },
            { ":-0\r\n", 0, 0 },
            { ":+0\r\n", 0, 0 },
            { ":1\r\n", 1, 0 },
            { ":-1\r\n", -1, 0 },
            { ":+1\r\n", 1, 0 },
            { ":000000000000000000000000001\r\n", 1, 0 },
            { ":-000000000000000000000000001\r\n", -1, 0 },
            { ":7\r\n", 7, 0 },
            { ":-7\r\n", -7, 0 },
            { ":2147483647\r\n", INT32_MAX, 0 },
            { ":-2147483648\r\n", -INT32_MAX - 1, 0 },
            { ":-2147483649\r\n", -2147483649LL, 0 },
            { ":4294967295\r\n", 4294967295LL, 0 },
            { ":-4294967296\r\n", -4294967296LL, 0 },
            { ":4294967296\r\n", 4294967296LL, 0 },
            { ":-4294967297\r\n", -4294967297LL, 0 },
            { ":9007199254740992\r\n", 9007199254740992LL, 0 },
            { ":9007199254740993\r\n", 9007199254740993LL, 0 },
            { ":-9007199254740993\r\n", -9007199254740993LL, 0 },
            { ":9223372036854775806\r\n", INT64_MAX - 1, 0 },
            { ":9223372036854775807\r\n", INT64_MAX, 0 },
            { ":9223372036854775808\r\n", 0, 1 },
            { ":9223372036854775809\r\n", 0, 1 },
            { ":18446744073709551615\r\n", 0, 1 },
            { ":18446744073709551616\r\n", 0, 1 },
            { ":-9223372036854775807\r\n", INT64_MIN + 1, 0 },
            { ":-9223372036854775808\r\n", INT64_MIN, 0 },
            { ":-9223372036854775809\r\n", 0, 1 },
            { ":-18446744073709551615\r\n", 0, 1 },
            { ":-18446744073709551616\r\n", 0, 1 },
            { ":-99999999999999999999999\r\n", 0, 1 },
            { ":-00000000000000000009223372036854775809\r\n", 0, 1 },
        };
        size_t i;
        for (i = 0; i < sizeof(edge) / sizeof(edge[0]); i++) {
            dyn_resp_reader_t r;
            dyn_resp_item_t it;
            rc = SCAN(edge[i].wire, 0, &used);
            if (edge[i].over) {
                CHECK(rc == DYN_RESP_E_INT, "scan(%s) must be out of range, gave %s",
                    edge[i].wire, dyn_resp_strerror(rc));
                dyn_resp_reader_init(&r, (const uint8_t*)edge[i].wire,
                    strlen(edge[i].wire));
                CHECK(dyn_resp_next(&r, &it) == DYN_RESP_E_INT,
                    "read(%s) must refuse too", edge[i].wire);
                continue;
            }
            CHECK(rc == DYN_RESP_OK, "scan(%s) = %s", edge[i].wire,
                dyn_resp_strerror(rc));
            dyn_resp_reader_init(&r, (const uint8_t*)edge[i].wire,
                strlen(edge[i].wire));
            CHECK(dyn_resp_next(&r, &it) == DYN_RESP_OK, "read(%s)", edge[i].wire);
            CHECK(it.type == DYN_RESP_INT && it.ival == edge[i].want,
                "%s decoded to %lld, want %lld", edge[i].wire,
                (long long)it.ival, (long long)edge[i].want);
        }

        CHECK(SCAN("$-9223372036854775808\r\n", 0, &used) == DYN_RESP_E_SYNTAX,
            "$-INT64_MIN is a length, and not the only negative one that is legal");
        CHECK(SCAN("*-9223372036854775808\r\n", 0, &used) == DYN_RESP_E_SYNTAX,
            "*-INT64_MIN likewise");
        CHECK(SCAN("$9223372036854775808\r\n", 0, &used) == DYN_RESP_E_INT,
            "a 2^63 bulk length does not fit an int64 at all");
        CHECK(SCAN("*9223372036854775808\r\n", 0, &used) == DYN_RESP_E_INT,
            "and 2^63 as an element count likewise");
        CHECK(SCAN("$9223372036854775807\r\n", 0, &used) == DYN_RESP_E_TOOBIG,
            "INT64_MAX as a length IS representable, so maxbulk is what refuses it");

        {
            static char run[DYN_RESP_MAX_LINE + 128];
            size_t n = 0;
            run[n++] = ':';
            run[n++] = '-';
            memset(run + n, '0', 40000);
            n += 40000;
            run[n++] = '1';
            run[n++] = '\r';
            run[n++] = '\n';
            rc = dyn_resp_scan((const uint8_t*)run, n, 0, &used);
            CHECK(rc == DYN_RESP_OK && used == n,
                "40k leading zeros then 1 is -1, not a bomb: %s",
                dyn_resp_strerror(rc));
        }
        {
            static char run[DYN_RESP_MAX_LINE + 128];
            size_t n = 0;
            run[n++] = ':';
            memset(run + n, '9', 200);
            n += 200;
            run[n++] = '\r';
            run[n++] = '\n';
            rc = dyn_resp_scan((const uint8_t*)run, n, 0, &used);
            CHECK(rc == DYN_RESP_E_INT, "200 nines is out of range: %s",
                dyn_resp_strerror(rc));
            n = 0;
            run[n++] = ':';
            memset(run + n, '9', DYN_RESP_MAX_LINE + 16);
            n += DYN_RESP_MAX_LINE + 16;
            rc = dyn_resp_scan((const uint8_t*)run, n, 0, &used);
            CHECK(rc == DYN_RESP_E_TOOBIG, "a 64k+ digit line hits the LINE cap: %s",
                dyn_resp_strerror(rc));
        }

        CHECK(SCAN(": \r\n", 0, &used) == DYN_RESP_E_SYNTAX, "an embedded space");
        CHECK(SCAN(":1 2\r\n", 0, &used) == DYN_RESP_E_SYNTAX, "an embedded space");
        CHECK(SCAN(":1\t2\r\n", 0, &used) == DYN_RESP_E_SYNTAX, "an embedded tab");
        CHECK(SCAN(":--1\r\n", 0, &used) == DYN_RESP_E_SYNTAX, "a doubled sign");
        CHECK(SCAN(":+-1\r\n", 0, &used) == DYN_RESP_E_SYNTAX, "sign then sign");
        CHECK(SCAN(":-\r\n", 0, &used) == DYN_RESP_E_SYNTAX, "a sign with no digits");
        CHECK(SCAN(":+\r\n", 0, &used) == DYN_RESP_E_SYNTAX, "likewise");
        CHECK(SCAN(":0x10\r\n", 0, &used) == DYN_RESP_E_SYNTAX, "hex is not RESP");
        CHECK(SCAN(":1_000\r\n", 0, &used) == DYN_RESP_E_SYNTAX, "separators are not RESP");
        CHECK(SCAN(":١٢٣\r\n", 0, &used) == DYN_RESP_E_SYNTAX, "non-ASCII digits");
    }

    {
        char chain[64 * 64];
        size_t i, w;
        for (i = 0; i < DYN_RESP_MAX_DEPTH - 1; i++) {
            for (w = 0; w < i; w++)
                memcpy(chain + w * 15, "|1\r\n$1\r\nk\r\n:0\r\n", 15);
            w = i * 15;
            memcpy(chain + w, ":7\r\n", 4);
            w += 4;
            rc = dyn_resp_scan((const uint8_t*)chain, w, 0, &used);
            CHECK(rc == DYN_RESP_OK, "a chain of %zu attributes must scan: %s",
                i, dyn_resp_strerror(rc));
            CHECK(used == w, "and consume all %zu octets, got %zu", w, used);
        }
        {
            size_t n = DYN_RESP_MAX_DEPTH;
            for (w = 0; w < n; w++)
                memcpy(chain + w * 15, "|1\r\n$1\r\nk\r\n:0\r\n", 15);
            w = n * 15;
            memcpy(chain + w, ":7\r\n", 4);
            w += 4;
            rc = dyn_resp_scan((const uint8_t*)chain, w, 0, &used);
            CHECK(rc == DYN_RESP_E_DEPTH,
                "the %dth attribute must be refused BY THE SCAN, got %s",
                DYN_RESP_MAX_DEPTH, dyn_resp_strerror(rc));
        }
        {
            size_t n = 40;
            for (w = 0; w < n; w++)
                memcpy(chain + w * 4, "|0\r\n", 4);
            w = n * 4;
            memcpy(chain + w, ":7\r\n", 4);
            w += 4;
            rc = dyn_resp_scan((const uint8_t*)chain, w, 0, &used);
            CHECK(rc == DYN_RESP_E_DEPTH, "40 empty attributes: %s",
                dyn_resp_strerror(rc));
            n = DYN_RESP_MAX_DEPTH - 1;
            for (w = 0; w < n; w++)
                memcpy(chain + w * 4, "|0\r\n", 4);
            w = n * 4;
            memcpy(chain + w, ":7\r\n", 4);
            w += 4;
            rc = dyn_resp_scan((const uint8_t*)chain, w, 0, &used);
            CHECK(rc == DYN_RESP_OK, "31 empty attributes must scan: %s",
                dyn_resp_strerror(rc));
        }
        {
            const char* in = "*1\r\n|0\r\n:1\r\n";
            rc = SCAN(in, 0, &used);
            CHECK(rc == DYN_RESP_OK && used == strlen(in),
                "an attribute inside an array owes its value: %s, %zu of %zu",
                dyn_resp_strerror(rc), used, strlen(in));
            in = "*2\r\n:1\r\n|1\r\n+a\r\n+b\r\n+v\r\n";
            rc = SCAN(in, 0, &used);
            CHECK(rc == DYN_RESP_OK && used == strlen(in),
                "a non-leading attribute owes its value: %s, %zu of %zu",
                dyn_resp_strerror(rc), used, strlen(in));
            in = "*2\r\n:1\r\n|1\r\n+a\r\n+b\r\n";
            rc = SCAN(in, 0, &used);
            CHECK(rc == DYN_RESP_INCOMPLETE,
                "a non-leading attribute with no value is incomplete: %s",
                dyn_resp_strerror(rc));
        }
        {
            size_t a, d;
            for (a = 0; a <= 3; a++) {
                for (d = 0; d + a <= (size_t)(DYN_RESP_MAX_DEPTH - 1); d++) {
                    size_t w2 = 0, k2;
                    for (k2 = 0; k2 < a; k2++)
                        memcpy(chain + w2, "|0\r\n", 4), w2 += 4;
                    for (k2 = 0; k2 < d; k2++)
                        memcpy(chain + w2, "*1\r\n", 4), w2 += 4;
                    memcpy(chain + w2, ":7\r\n", 4);
                    w2 += 4;
                    rc = dyn_resp_scan((const uint8_t*)chain, w2, 0, &used);
                    CHECK(rc == DYN_RESP_OK,
                        "%zu attributes then %zu containers is %zu deep and must scan: %s",
                        a, d, a + d, dyn_resp_strerror(rc));
                }
                {
                    size_t d2 = (size_t)(DYN_RESP_MAX_DEPTH - 1 - (int)a) + 1;
                    size_t w2 = 0, k2;
                    for (k2 = 0; k2 < a; k2++)
                        memcpy(chain + w2, "|0\r\n", 4), w2 += 4;
                    for (k2 = 0; k2 < d2; k2++)
                        memcpy(chain + w2, "*1\r\n", 4), w2 += 4;
                    memcpy(chain + w2, ":7\r\n", 4);
                    w2 += 4;
                    rc = dyn_resp_scan((const uint8_t*)chain, w2, 0, &used);
                    CHECK(rc == DYN_RESP_E_DEPTH,
                        "%zu attributes then %zu containers is %zu deep and must be "
                        "refused BY THE SCAN, got %s",
                        a, d2, a + d2,
                        dyn_resp_strerror(rc));
                }
            }
        }
        {
            size_t n = DYN_RESP_MAX_DEPTH - 1, one = n * 4 + 4;
            for (w = 0; w < n; w++)
                memcpy(chain + w * 4, "|0\r\n", 4);
            memcpy(chain + n * 4, ":7\r\n", 4);
            memcpy(chain + one, chain, one);
            rc = dyn_resp_scan((const uint8_t*)chain, one, 0, &used);
            CHECK(rc == DYN_RESP_OK && used == one,
                "the first 31-attribute reply: %s, %zu", dyn_resp_strerror(rc), used);
            rc = dyn_resp_scan((const uint8_t*)chain + used, one * 2 - used, 0, &used);
            CHECK(rc == DYN_RESP_OK && used == one,
                "and the second, whose allowance must be its own: %s, %zu",
                dyn_resp_strerror(rc), used);
        }
    }

    {
        const uint8_t hs[5] = { 0x16, 0x03, 0x01, 0x00, 0x50 };
        const uint8_t al[5] = { 0x15, 0x03, 0x03, 0x00, 0x02 };
        CHECK(dyn_resp_looks_like_tls(hs, 5), "a TLS handshake record");
        CHECK(dyn_resp_looks_like_tls(al, 5), "a TLS alert record");
        CHECK(!dyn_resp_looks_like_tls((const uint8_t*)"+OK\r\n", 5),
            "and a normal reply is not mistaken for one");
        CHECK(!dyn_resp_looks_like_tls(hs, 2), "nor is a 2-byte fragment");
    }

    {
        static char buf[8 * 128];
        size_t a, c, w2, k2;
        for (a = 0; a <= 6; a++) {
            for (c = 0; c <= 40; c++) {
                int deep = (int)(a + c);
                w2 = 0;
                for (k2 = 0; k2 < a; k2++) {
                    memcpy(buf + w2, "|1\r\n$1\r\nk\r\n:0\r\n", 15);
                    w2 += 15;
                }
                for (k2 = 0; k2 < c; k2++) {
                    memcpy(buf + w2, "*1\r\n", 4);
                    w2 += 4;
                }
                memcpy(buf + w2, ":7\r\n", 4);
                w2 += 4;
                rc = dyn_resp_scan((const uint8_t*)buf, w2, 0, &used);
                if (deep < DYN_RESP_MAX_DEPTH) {
                    CHECK(rc == DYN_RESP_OK && used == w2,
                        "%zu attr + %zu cont = %d deep must be ACCEPTED, got %s "
                        "(%zu of %zu)",
                        a, c, deep, dyn_resp_strerror(rc), used, w2);
                    {
                        dyn_resp_reader_t rd;
                        dyn_resp_item_t it;
                        int steps = 0;
                        dyn_resp_reader_init(&rd, (const uint8_t*)buf, w2);
                        while (dyn_resp_next(&rd, &it) == DYN_RESP_OK) {
                            steps++;
                            if (steps > 500)
                                break;
                        }
                        CHECK(rd.pos == w2,
                            "%zu attr + %zu cont: the reader stopped at %zu of %zu",
                            a, c, rd.pos, w2);
                    }
                } else {
                    CHECK(rc == DYN_RESP_E_DEPTH,
                        "%zu attr + %zu cont = %d deep must be REFUSED by the "
                        "scan, got %s",
                        a, c, deep, dyn_resp_strerror(rc));
                }
            }
        }
    }

    {
        unsigned long long st = 0x243F6A8885A308D3ULL;
        int iter, bad = 0;
        const __int128 TWO63 = (__int128)1 << 63;
        for (iter = 0; iter < 200000; iter++) {
            char digits[48], wire[64];
            __int128 mag = 0;
            int over = 0, k, len, neg, sign = 0, w = 0;
            int64_t want;
            len = 1 + (int)(rnd(&st) % 40);
            neg = (int)(rnd(&st) % 2);
            for (k = 0; k < len; k++) {
                int d;
                st ^= st << 13;
                st ^= st >> 7;
                st ^= st << 17;
                d = (int)(st % 10);
                digits[k] = (char)('0' + d);
            }
            digits[len] = '\0';
            for (k = 0; k < len; k++) {
                int d = digits[k] - '0';
                if (mag > (TWO63 * 10 - d) / 10) {
                    over = 1;
                    break;
                }
                mag = mag * 10 + d;
            }
            wire[w++] = ':';
            if (neg)
                wire[w++] = '-';
            else if (iter % 7 == 0) {
                wire[w++] = '+';
                sign = 1;
            }
            (void)sign;
            memcpy(wire + w, digits, (size_t)len);
            w += len;
            wire[w++] = '\r';
            wire[w++] = '\n';
            wire[w] = '\0';

            rc = dyn_resp_scan((const uint8_t*)wire, (size_t)w, 0, &used);
            if (over || (!neg && mag > (__int128)INT64_MAX) || (neg && mag > TWO63)) {
                if (rc != DYN_RESP_E_INT && bad++ < 5)
                    CHECK(0, "scan(%s) = %s, want out of range", wire,
                        dyn_resp_strerror(rc));
            } else if (rc != DYN_RESP_OK) {
                if (bad++ < 5)
                    CHECK(0, "scan(%s) = %s, want ok", wire, dyn_resp_strerror(rc));
            } else {
                dyn_resp_reader_t rd;
                dyn_resp_item_t it;
                want = neg ? -(int64_t)mag : (int64_t)mag;
                dyn_resp_reader_init(&rd, (const uint8_t*)wire, (size_t)w);
                if (dyn_resp_next(&rd, &it) != DYN_RESP_OK || it.ival != want) {
                    if (bad++ < 5)
                        CHECK(0, "read(%s) gave %lld, want %lld (seed 0x%llx)",
                            wire, (long long)it.ival, (long long)want,
                            0x243F6A8885A308D3ULL);
                }
            }
        }
        CHECK(bad == 0, "%d of 200000 random integer fields disagreed with the "
                        "__int128 oracle (seed 0x243F6A8885A308D3)",
            bad);
    }

    if (fk_only("resp-stream"))
        stream_cases();
    if (fk_only("resp-tree"))
        prop_tree();
    if (fk_only("resp-raw"))
        prop_raw();
    if (fk_only("resp-cmd"))
        prop_cmd();
    fk_fails += fails;
    return fk_done("test_resp_codec");
}
