/* test_resp_codec.c -- the RESP codec, driven adversarially.
 *
 * "The server" is whatever answered on that port, so the cases that matter are
 * the hostile ones: element counts and bulk lengths that are allocation
 * instructions, nesting that would drive a recursive decoder off its stack,
 * bare LFs that would split one reply into two, and a value containing CRLF
 * that must NOT become a second command. Every refusal is paired with the
 * legal shape it resembles, so a blanket rejection cannot pass for a check.
 */
#include "dyn-resp.h"

#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { \
    printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

#define SCAN(s, cap, out) \
    dyn_resp_scan((const uint8_t *)(s), strlen(s), (cap), (out))

int main(void)
{
    size_t used;
    int rc;

    setvbuf(stdout, NULL, _IOLBF, 0);

    /* ---- 1. every scalar shape, RESP2 and RESP3 ---- */
    {
        static const struct { const char *wire; size_t len; int type; } ok[] = {
            { "+OK\r\n",            5,  DYN_RESP_SIMPLE  },
            { "-ERR nope\r\n",     11,  DYN_RESP_ERROR   },
            { ":1234\r\n",          7,  DYN_RESP_INT     },
            { ":-9\r\n",            5,  DYN_RESP_INT     },
            { "$5\r\nhello\r\n",   11,  DYN_RESP_BULK    },
            { "$0\r\n\r\n",         6,  DYN_RESP_BULK    },
            { "$-1\r\n",            5,  DYN_RESP_BULK    },
            { "*-1\r\n",            5,  DYN_RESP_ARRAY   },
            { "*0\r\n",             4,  DYN_RESP_ARRAY   },
            { "_\r\n",              3,  DYN_RESP_NULL    },
            { "#t\r\n",             4,  DYN_RESP_BOOL    },
            { "#f\r\n",             4,  DYN_RESP_BOOL    },
            { ",3.25\r\n",          7,  DYN_RESP_DOUBLE  },
            { ",inf\r\n",           6,  DYN_RESP_DOUBLE  },
            { ",-inf\r\n",          7,  DYN_RESP_DOUBLE  },
            { "(3492890328409238\r\n", 19, DYN_RESP_BIGNUM },
            { "!4\r\noops\r\n",    10,  DYN_RESP_BLOBERR },
            { "=8\r\ntxt:abcd\r\n", 14, DYN_RESP_VERB    },
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
            dyn_resp_reader_init(&r, (const uint8_t *)ok[i].wire, ok[i].len);
            CHECK(dyn_resp_next(&r, &it) == DYN_RESP_OK, "read case %zu", i);
            CHECK(it.type == ok[i].type, "case %zu type %c, want %c",
                  i, it.type, ok[i].type);
        }
    }

    /* ---- 2. values decode to the right values, not merely to the right shape ---- */
    {
        dyn_resp_reader_t r;
        dyn_resp_item_t it;
        dyn_resp_reader_init(&r, (const uint8_t *)"$5\r\nhe\r\no\r\n", 11);
        CHECK(dyn_resp_next(&r, &it) == DYN_RESP_OK, "bulk with an embedded CRLF");
        CHECK(it.slen == 5 && memcmp(it.str, "he\r\no", 5) == 0,
              "a length-prefixed bulk carries CRLF as DATA");
        CHECK(r.pos == 11, "and consumed the whole thing");

        dyn_resp_reader_init(&r, (const uint8_t *)":-9\r\n", 5);
        dyn_resp_next(&r, &it);
        CHECK(it.ival == -9, "negative integer decoded as %lld", (long long)it.ival);

        dyn_resp_reader_init(&r, (const uint8_t *)",3.25\r\n", 7);
        dyn_resp_next(&r, &it);
        CHECK(it.dval == 3.25, "double decoded as %g", it.dval);

        dyn_resp_reader_init(&r, (const uint8_t *)",inf\r\n", 6);
        dyn_resp_next(&r, &it);
        CHECK(it.dval > 1e308, "inf decoded as %g", it.dval);

        dyn_resp_reader_init(&r, (const uint8_t *)"#f\r\n", 4);
        dyn_resp_next(&r, &it);
        CHECK(it.ival == 0, "#f is false");

        dyn_resp_reader_init(&r, (const uint8_t *)"$-1\r\n", 5);
        dyn_resp_next(&r, &it);
        CHECK(it.isnull && it.str == NULL, "a null bulk is null, not empty");

        dyn_resp_reader_init(&r, (const uint8_t *)"$0\r\n\r\n", 6);
        dyn_resp_next(&r, &it);
        CHECK(!it.isnull && it.slen == 0, "an EMPTY bulk is not null");
    }

    /* ---- 3. aggregates, including a map's pair count ---- */
    {
        dyn_resp_reader_t r;
        dyn_resp_item_t it;
        const char *m = "%2\r\n+a\r\n:1\r\n+b\r\n:2\r\n";
        rc = SCAN(m, 0, &used);
        CHECK(rc == DYN_RESP_OK && used == strlen(m),
              "a 2-pair map scans whole: %s, %zu", dyn_resp_strerror(rc), used);
        dyn_resp_reader_init(&r, (const uint8_t *)m, strlen(m));
        dyn_resp_next(&r, &it);
        CHECK(it.count == 2, "map reports PAIRS (%lld), not values",
              (long long)it.count);

        CHECK(SCAN("~2\r\n:1\r\n:2\r\n", 0, &used) == DYN_RESP_OK, "set");
        CHECK(SCAN(">3\r\n$7\r\nmessage\r\n$2\r\nch\r\n$2\r\nhi\r\n", 0, &used)
              == DYN_RESP_OK, "push (a RESP3 pub/sub delivery)");
        CHECK(SCAN("*2\r\n*1\r\n:1\r\n*1\r\n:2\r\n", 0, &used) == DYN_RESP_OK,
              "nested arrays");
    }

    /* ---- 4. EVERY PROPER PREFIX MUST BE INCOMPLETE ----
     * The single strongest check on a streaming parser: a partial TCP read
     * must never be read as a complete reply, and must never be an error. */
    {
        static const char *msgs[] = {
            "+OK\r\n", "$5\r\nhello\r\n", "*2\r\n$3\r\nfoo\r\n:7\r\n",
            "%1\r\n+k\r\n*2\r\n:1\r\n:2\r\n", ">3\r\n$7\r\nmessage\r\n$1\r\na\r\n$1\r\nb\r\n",
        };
        size_t i, k;
        for (i = 0; i < sizeof(msgs) / sizeof(msgs[0]); i++) {
            size_t n = strlen(msgs[i]);
            for (k = 0; k < n; k++) {
                rc = dyn_resp_scan((const uint8_t *)msgs[i], k, 0, &used);
                CHECK(rc == DYN_RESP_INCOMPLETE,
                      "prefix %zu of msg %zu gave %s, want INCOMPLETE",
                      k, i, dyn_resp_strerror(rc));
            }
            rc = dyn_resp_scan((const uint8_t *)msgs[i], n, 0, &used);
            CHECK(rc == DYN_RESP_OK && used == n, "the whole of msg %zu", i);
        }
    }

    /* ---- 5. two replies back to back: consume exactly the first ---- */
    {
        const char *two = "+OK\r\n:42\r\n";
        rc = SCAN(two, 0, &used);
        CHECK(rc == DYN_RESP_OK && used == 5,
              "pipelined replies must not be merged, consumed %zu", used);
        rc = dyn_resp_scan((const uint8_t *)two + 5, 5, 0, &used);
        CHECK(rc == DYN_RESP_OK && used == 5, "and the second parses alone");
    }

    /* ---- 6. allocation bombs ---- */
    {
        rc = SCAN("*2000000000\r\n", 0, &used);
        CHECK(rc == DYN_RESP_E_COUNT, "a 2e9-element array gave %s",
              dyn_resp_strerror(rc));
        rc = SCAN("%2000000000\r\n", 0, &used);
        CHECK(rc == DYN_RESP_E_COUNT, "a 2e9-PAIR map gave %s",
              dyn_resp_strerror(rc));
        /* and the legal shape at the same place still scans, so the bound is
         * not simply refusing every count */
        CHECK(SCAN("*2\r\n:1\r\n:2\r\n", 0, &used) == DYN_RESP_OK,
              "a small array must still pass");

        rc = SCAN("$99999999999\r\n", 0, &used);
        CHECK(rc == DYN_RESP_E_TOOBIG, "a 100 GB bulk gave %s",
              dyn_resp_strerror(rc));
        rc = SCAN("$99999999999999999999999999\r\n", 0, &used);
        CHECK(rc == DYN_RESP_E_TOOBIG, "a length that OVERFLOWS int64 gave %s",
              dyn_resp_strerror(rc));
        /* the cap is honoured as given, not only at its default */
        rc = SCAN("$100\r\n", 64, &used);
        CHECK(rc == DYN_RESP_E_TOOBIG, "maxbulk=64 must refuse a 100-byte bulk");
        rc = SCAN("$4\r\nabcd\r\n", 64, &used);
        CHECK(rc == DYN_RESP_OK, "and still accept one under it");
    }

    /* ---- 7. depth bomb: a recursive decoder's stack ---- */
    {
        char deep[4 * 64];
        size_t i, w = 0;
        for (i = 0; i < 40; i++) { memcpy(deep + w, "*1\r\n", 4); w += 4; }
        rc = dyn_resp_scan((const uint8_t *)deep, w, 0, &used);
        CHECK(rc == DYN_RESP_E_DEPTH, "40 levels of nesting gave %s",
              dyn_resp_strerror(rc));
        /* one level under the limit must still be accepted */
        w = 0;
        for (i = 0; i < DYN_RESP_MAX_DEPTH - 1; i++)
            { memcpy(deep + w, "*1\r\n", 4); w += 4; }
        memcpy(deep + w, ":1\r\n", 4); w += 4;
        rc = dyn_resp_scan((const uint8_t *)deep, w, 0, &used);
        CHECK(rc == DYN_RESP_OK, "%d levels must pass, got %s",
              DYN_RESP_MAX_DEPTH - 1, dyn_resp_strerror(rc));
    }

    /* ---- 8. terminator and type-byte abuse ---- */
    {
        rc = dyn_resp_scan((const uint8_t *)"+OK\n", 4, 0, &used);
        CHECK(rc == DYN_RESP_E_SYNTAX, "a BARE LF must not terminate a line");
        rc = dyn_resp_scan((const uint8_t *)"+OK\rX", 5, 0, &used);
        CHECK(rc == DYN_RESP_E_SYNTAX, "CR not followed by LF");
        rc = dyn_resp_scan((const uint8_t *)"@1\r\n", 4, 0, &used);
        CHECK(rc == DYN_RESP_E_TYPE, "an unknown type byte gave %s",
              dyn_resp_strerror(rc));
        rc = dyn_resp_scan((const uint8_t *)"$-2\r\n", 5, 0, &used);
        CHECK(rc == DYN_RESP_E_SYNTAX, "only $-1 is null; $-2 is not");
        rc = dyn_resp_scan((const uint8_t *)"*-2\r\n", 5, 0, &used);
        CHECK(rc == DYN_RESP_E_SYNTAX, "only *-1 is a null array");
        rc = dyn_resp_scan((const uint8_t *)"%-1\r\n", 5, 0, &used);
        CHECK(rc == DYN_RESP_E_SYNTAX, "a map has no null form");
        rc = dyn_resp_scan((const uint8_t *)":12x4\r\n", 7, 0, &used);
        CHECK(rc == DYN_RESP_E_SYNTAX, "a non-numeric integer");
        rc = dyn_resp_scan((const uint8_t *)":\r\n", 3, 0, &used);
        CHECK(rc == DYN_RESP_E_SYNTAX, "an EMPTY integer");
        rc = dyn_resp_scan((const uint8_t *)"#x\r\n", 4, 0, &used);
        CHECK(rc == DYN_RESP_E_SYNTAX, "a boolean that is neither t nor f");
        rc = dyn_resp_scan((const uint8_t *)"_x\r\n", 4, 0, &used);
        CHECK(rc == DYN_RESP_E_SYNTAX, "RESP3 null carries no payload");
        /* a line with no CRLF at all must be bounded, not hunted forever */
        {
            static char runaway[DYN_RESP_MAX_LINE + 64];
            memset(runaway, 'a', sizeof(runaway));
            runaway[0] = '+';
            rc = dyn_resp_scan((const uint8_t *)runaway, sizeof(runaway), 0, &used);
            CHECK(rc == DYN_RESP_E_TOOBIG,
                  "a line longer than the cap must be refused, got %s",
                  dyn_resp_strerror(rc));
        }
    }

    /* ---- 9. an attribute is metadata, NOT the reply ----
     * Stopping after it would hand the caller the attribute and leave the real
     * reply in the buffer -- every later command answered one out of step. */
    {
        const char *w = "|1\r\n+ttl\r\n:60\r\n+OK\r\n";
        rc = SCAN(w, 0, &used);
        CHECK(rc == DYN_RESP_OK && used == strlen(w),
              "attribute+reply must consume BOTH: %s, %zu of %zu",
              dyn_resp_strerror(rc), used, strlen(w));
        /* an attribute alone is incomplete: the reply it decorates is missing */
        rc = dyn_resp_scan((const uint8_t *)w, 14, 0, &used);
        CHECK(rc == DYN_RESP_INCOMPLETE, "an attribute with no reply after it");
    }

    /* ---- 10. command encoding: exact bytes, and NO injection ---- */
    {
        uint8_t out[256];
        const char *argv[3] = { "SET", "key", "value" };
        size_t need;
        int n = dyn_resp_cmd_encode(out, sizeof(out), 3, argv, NULL, &need);
        const char *want = "*3\r\n$3\r\nSET\r\n$3\r\nkey\r\n$5\r\nvalue\r\n";
        CHECK(n == (int)strlen(want), "encode wrote %d, want %zu", n, strlen(want));
        CHECK(n > 0 && memcmp(out, want, (size_t)n) == 0, "exact wire bytes");
        CHECK(need == strlen(want), "size prediction must match what was written");
        CHECK(dyn_resp_cmd_size(3, argv, NULL) == strlen(want),
              "and dyn_resp_cmd_size must agree with the encoder");

        /* THE INJECTION CASE. A value carrying a complete second command must
         * come out as one argument of one command. If the encoder ever used
         * the inline form, this scans as two. */
        {
            const char *evil[3] = { "SET", "k", "v\r\n*1\r\n$8\r\nFLUSHALL\r\n" };
            uint8_t buf[256];
            size_t elen[3] = { (size_t)-1, (size_t)-1, 21 };
            dyn_resp_reader_t r;
            dyn_resp_item_t it;
            int m = dyn_resp_cmd_encode(buf, sizeof(buf), 3, evil, elen, NULL);
            CHECK(m > 0, "encode the hostile value");
            rc = dyn_resp_scan(buf, (size_t)m, 0, &used);
            CHECK(rc == DYN_RESP_OK && used == (size_t)m,
                  "the hostile command must scan as exactly ONE value "
                  "(%s, %zu of %d)", dyn_resp_strerror(rc), used, m);
            dyn_resp_reader_init(&r, buf, (size_t)m);
            dyn_resp_next(&r, &it);
            CHECK(it.type == DYN_RESP_ARRAY && it.count == 3,
                  "one array of 3, got type %c count %lld",
                  it.type, (long long)it.count);
            dyn_resp_next(&r, &it); dyn_resp_next(&r, &it); dyn_resp_next(&r, &it);
            CHECK(it.slen == 21 && memcmp(it.str, "v\r\n*1\r\n$8\r\nFLUSHALL\r\n", 21) == 0,
                  "FLUSHALL stayed inside the third ARGUMENT");
        }

        /* an embedded NUL is data too, once a length is supplied */
        {
            const char *nul[2] = { "SET", "a\0b" };
            size_t nlen[2] = { 3, 3 };
            uint8_t buf[64];
            int m = dyn_resp_cmd_encode(buf, sizeof(buf), 2, nul, nlen, NULL);
            CHECK(m > 0 && memcmp(buf + 13, "$3\r\na\0b\r\n", 9) == 0,
                  "an embedded NUL survives encoding");
        }

        /* refusing beats overrunning */
        CHECK(dyn_resp_cmd_encode(out, 8, 3, argv, NULL, &need) == DYN_RESP_E_TOOBIG,
              "a short output buffer must be refused");
        CHECK(need == strlen(want), "and must still report what was needed");
        CHECK(dyn_resp_cmd_encode(out, sizeof(out), 0, argv, NULL, NULL)
              == DYN_RESP_E_SYNTAX, "a command with no name");
    }

    /* ---- 10b. the double parse is LOCALE-INDEPENDENT and fully consuming ----
     * strtod and sscanf("%lf") read LC_NUMERIC for the radix character, so a
     * comma locale would parse ",3,25" as 3 and stop. Nothing here calls
     * setlocale today, which is exactly why the bug would appear later and far
     * from its cause. These cases pin the grammar RESP3 actually defines. */
    {
        static const struct { const char *wire; int ok; double want; } d[] = {
            { ",3.25\r\n",     1,  3.25    },
            { ",-0.5\r\n",     1, -0.5     },
            { ",10\r\n",       1,  10.0    },
            { ",1e3\r\n",      1,  1000.0  },
            { ",1.5E-2\r\n",   1,  0.015   },
            { ",+2.5\r\n",     1,  2.5     },
            { ",3,25\r\n",     0,  0       },  /* comma radix: NOT a number */
            { ",1.2.3\r\n",    0,  0       },
            { ",0x10\r\n",     0,  0       },  /* strtod would take this */
            { ",1e\r\n",       0,  0       },
            { ", 1.0\r\n",     0,  0       },  /* leading space */
            { ",1.0junk\r\n",  0,  0       },
            { ",.\r\n",        0,  0       },
            /* EXACTNESS, not just grammar. Every case above is a small value
             * that any parser gets right, so the table proved the grammar and
             * nothing about the arithmetic -- the alphabet it was written with,
             * not the domain. A digit accumulator (`v = v*10 + d`, then scale)
             * passes all of them and still returned 1.0000000000000002e+300 for
             * 1e300 against a real Redis 8, and 1.7976931348623145e+308 for
             * DBL_MAX. These are the cases that separate the two. */
            { ",1e300\r\n",    1,  1e300   },
            { ",-1e300\r\n",   1, -1e300   },
            { ",1e-300\r\n",   1,  1e-300  },
            { ",1.7976931348623157e308\r\n", 1, 1.7976931348623157e308 },
            { ",2.2250738585072014e-308\r\n", 1, 2.2250738585072014e-308 },
            { ",5e-324\r\n",   1,  5e-324  },   /* the smallest denormal */
            { ",3.141592653589793\r\n", 1, 3.141592653589793 },
            { ",0.1\r\n",      1,  0.1     },
            { ",9007199254740993\r\n", 1, 9007199254740992.0 },
            { ",1234567890123456789\r\n", 1, 1234567890123456768.0 },
        };
        size_t i;
        for (i = 0; i < sizeof(d) / sizeof(d[0]); i++) {
            dyn_resp_reader_t r;
            dyn_resp_item_t it;
            size_t n = strlen(d[i].wire);
            int rc2;
            dyn_resp_reader_init(&r, (const uint8_t *)d[i].wire, n);
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

    /* ---- 11. a TLS endpoint is named, not reported as gibberish ---- */
    {
        const uint8_t hs[5] = { 0x16, 0x03, 0x01, 0x00, 0x50 };
        const uint8_t al[5] = { 0x15, 0x03, 0x03, 0x00, 0x02 };
        CHECK(dyn_resp_looks_like_tls(hs, 5), "a TLS handshake record");
        CHECK(dyn_resp_looks_like_tls(al, 5), "a TLS alert record");
        CHECK(!dyn_resp_looks_like_tls((const uint8_t *)"+OK\r\n", 5),
              "and a normal reply is not mistaken for one");
        CHECK(!dyn_resp_looks_like_tls(hs, 2), "nor is a 2-byte fragment");
    }

    if (fails == 0) printf("test_resp_codec: all tests passed\n");
    else printf("test_resp_codec: %d FAILED\n", fails);
    return fails != 0;
}
