/* test_dns_codec -- RFC 1035 vectors plus seeded properties over tests/lib/dyn-dns-model.h: dns-gen (pointer-compressed
 * messages decode to their recorded names and records), dns-raw (mutated and truncated messages match the reference
 * decoder; error leaves *off unchanged), dns-enc (encode, build_query, begin_response, add_answer round trip).
 * Fixed here: names over 255 wire octets (3.1) were accepted up to 257; begin_response dropped OPCODE (4.1.1);
 * skip_questions advanced *off before failing. DYN_FUZZ_SEED / DYN_FUZZ_ITERS / DYN_FUZZ_ONLY as in dyn-fuzzkit.h. */
#include "../lib/dyn-dns-model.h"

#include <stdio.h>
#include <string.h>

#define CHECK(c, ...) FK_CHECK(c, __VA_ARGS__)

static dm_msg_t g_m;

static void prop_gen(void)
{
    long it, N = (long)fk_iters(4000);
    fk_rng_t r = fk_stream("dns-gen");
    dm_src_t s = { NULL, 0, &r };
    for (it = 0; it < N; it++) {
        const char* e;
        dm_gen(&s, &g_m);
        e = dm_check_gen(&g_m);
        FK_CHECK(e == NULL, "%s (item %ld, %zu bytes)", e, it, g_m.len);
    }
}

static void prop_raw(void)
{
    long it, N = (long)fk_iters(4000);
    fk_rng_t r = fk_stream("dns-raw");
    dm_src_t s = { NULL, 0, &r };
    for (it = 0; it < N; it++) {
        const char* e;
        size_t n;
        dm_gen(&s, &g_m);
        if (g_m.full)
            continue;
        n = g_m.len;
        if (fk_chance(&r, 50))
            fk_mutate(&r, g_m.buf, n, fk_below(&r, 4));
        else
            dm_mutate(&s, g_m.buf, n);
        if (fk_chance(&r, 25))
            n = fk_below(&r, (uint32_t)n + 1);
        e = dm_check_raw(g_m.buf, n, &s);
        FK_CHECK(e == NULL, "%s (item %ld, %zu bytes)", e, it, n);
    }
}

static void prop_enc(void)
{
    long it, N = (long)fk_iters(6000);
    fk_rng_t r = fk_stream("dns-enc");
    dm_src_t s = { NULL, 0, &r };
    for (it = 0; it < N; it++) {
        const char* e = dm_check_enc(&s);
        FK_CHECK(e == NULL, "%s (item %ld)", e, it);
    }
}

int main(void)
{
    uint8_t buf[1024];
    char name[DYN_DNS_MAX_NAME + 1];
    int n;

    setvbuf(stdout, NULL, _IOLBF, 0);
    fk_init("test_dns_codec");

    {
        n = dyn_dns_name_encode("example.com", buf, sizeof(buf));
        CHECK(n == 13, "encode('example.com') wrote %d, want 13", n);
        CHECK(buf[0] == 7 && memcmp(buf + 1, "example", 7) == 0, "first label");
        CHECK(buf[8] == 3 && memcmp(buf + 9, "com", 3) == 0, "second label");
        CHECK(buf[12] == 0, "root label terminates the name");

        n = dyn_dns_name_decode(buf, 13, 0, name, sizeof(name));
        CHECK(n == 13, "decode consumed %d, want 13", n);
        CHECK(strcmp(name, "example.com") == 0, "decoded '%s'", name);
    }

    {
        /* names follow the 12-byte header (RFC 1035 4.1.4); the pointer
           targets the MIDDLE of "www.example.com", at "example" */
        uint8_t m[64];
        const int H = DYN_DNS_HDR_LEN;
        memset(m, 0, sizeof(m));
        m[H + 0] = 3;
        memcpy(m + H + 1, "www", 3);
        m[H + 4] = 7;
        memcpy(m + H + 5, "example", 7);
        m[H + 12] = 3;
        memcpy(m + H + 13, "com", 3);
        m[H + 16] = 0;
        m[H + 17] = 2;
        memcpy(m + H + 18, "ns", 2);
        m[H + 20] = 0xc0;
        m[H + 21] = (uint8_t)(H + 4);
        n = dyn_dns_name_decode(m, H + 22, H + 17, name, sizeof(name));
        CHECK(n == H + 22, "a legal pointer consumed %d, want %d", n, H + 22);
        CHECK(strcmp(name, "ns.example.com") == 0,
            "pointer-compressed name decoded as '%s'", name);

        m[H + 21] = 4;
        n = dyn_dns_name_decode(m, H + 22, H + 17, name, sizeof(name));
        CHECK(n == DYN_DNS_E_LOOP,
            "a pointer INTO the header gave %d (%s), want E_LOOP", n,
            dyn_dns_strerror(n));
        m[H + 21] = (uint8_t)(H + 17);
        n = dyn_dns_name_decode(m, H + 22, H + 17, name, sizeof(name));
        CHECK(n == DYN_DNS_E_LOOP,
            "a pointer to its own name's start gave %d, want E_LOOP", n);
    }

    {
        uint8_t m[8];
        memset(m, 0, sizeof(m));
        m[0] = 0xc0;
        m[1] = 0;
        n = dyn_dns_name_decode(m, 2, 0, name, sizeof(name));
        CHECK(n == DYN_DNS_E_LOOP, "self-pointer gave %d (%s), want E_LOOP",
            n, dyn_dns_strerror(n));
    }

    {
        uint8_t m[8];
        memset(m, 0, sizeof(m));
        m[0] = 0xc0;
        m[1] = 2;
        m[2] = 0xc0;
        m[3] = 0;
        n = dyn_dns_name_decode(m, 4, 0, name, sizeof(name));
        CHECK(n == DYN_DNS_E_LOOP, "a->b->a gave %d (%s), want E_LOOP",
            n, dyn_dns_strerror(n));
        n = dyn_dns_name_decode(m, 4, 2, name, sizeof(name));
        CHECK(n == DYN_DNS_E_LOOP, "entering the cycle at b gave %d", n);
    }

    {
        uint8_t m[16];
        memset(m, 0, sizeof(m));
        m[0] = 0xc0;
        m[1] = 8;
        m[8] = 1;
        m[9] = 'a';
        m[10] = 0;
        n = dyn_dns_name_decode(m, 11, 0, name, sizeof(name));
        CHECK(n == DYN_DNS_E_LOOP, "a FORWARD pointer must be refused, got %d", n);
    }

    {
        uint8_t m[600];
        size_t i, prev = 0;
        memset(m, 0, sizeof(m));
        for (i = 0; i < 8; i++) {
            size_t at = i * 64;
            m[at] = 60;
            memset(m + at + 1, 'a' + (int)i, 60);
            if (i == 0) {
                m[at + 61] = 0;
            } else {
                m[at + 61] = (uint8_t)(0xc0 | ((prev >> 8) & 0x3f));
                m[at + 62] = (uint8_t)(prev & 0xff);
            }
            prev = at;
        }
        n = dyn_dns_name_decode(m, sizeof(m), prev, name, sizeof(name));
        CHECK(n == DYN_DNS_E_NAME || n == DYN_DNS_E_LOOP,
            "an expanding pointer chain must be refused, got %d (%s)",
            n, dyn_dns_strerror(n));

        {
            char big_out[4096];
            n = dyn_dns_name_decode(m, sizeof(m), prev, big_out, sizeof(big_out));
            CHECK(n == DYN_DNS_E_NAME || n == DYN_DNS_E_LOOP,
                "the 255-octet RFC cap must hold even when the output buffer "
                "is large, got %d (%s)",
                n, dyn_dns_strerror(n));
        }
    }

    {
        uint8_t m[8];
        memset(m, 0, sizeof(m));
        m[0] = 0x40;
        n = dyn_dns_name_decode(m, 2, 0, name, sizeof(name));
        CHECK(n == DYN_DNS_E_FORMAT, "reserved bits 01 gave %d", n);
        m[0] = 0x80;
        n = dyn_dns_name_decode(m, 2, 0, name, sizeof(name));
        CHECK(n == DYN_DNS_E_FORMAT, "reserved bits 10 gave %d", n);
    }

    {
        uint8_t m[8];
        memset(m, 0, sizeof(m));
        m[0] = 40;
        n = dyn_dns_name_decode(m, 4, 0, name, sizeof(name));
        CHECK(n == DYN_DNS_E_SHORT, "over-long label gave %d", n);
    }

    {
        char big[80];
        memset(big, 'x', sizeof(big) - 1);
        big[sizeof(big) - 1] = 0;
        CHECK(dyn_dns_name_encode(big, buf, sizeof(buf)) == DYN_DNS_E_NAME,
            "a label over 63 octets must be refused");
        CHECK(dyn_dns_name_encode("a..b", buf, sizeof(buf)) == DYN_DNS_E_FORMAT,
            "an empty label must be refused");
        CHECK(dyn_dns_name_encode("example.com", buf, 4) == DYN_DNS_E_SHORT,
            "a short output buffer must be refused, never overrun");
    }

    {
        dyn_dns_hdr_t h;
        size_t off;
        n = dyn_dns_build_query(0x1234, "example.com", DYN_DNS_T_A,
            buf, sizeof(buf));
        CHECK(n == 12 + 13 + 4, "query length %d", n);
        CHECK(dyn_dns_hdr_decode(buf, (size_t)n, &h) == DYN_DNS_OK, "hdr decode");
        CHECK(h.id == 0x1234, "id round trip");
        CHECK(h.qdcount == 1 && h.ancount == 0, "counts");
        CHECK((h.flags & 0x0100) != 0, "RD must be set on a recursive query");
        off = DYN_DNS_HDR_LEN;
        CHECK(dyn_dns_skip_questions(buf, (size_t)n, &off, h.qdcount) == DYN_DNS_OK,
            "skip questions");
        CHECK(off == (size_t)n, "questions ended at %zu, message is %d", off, n);
    }

    {
        uint8_t m[64];
        size_t off;
        dyn_dns_rr_t rr;
        memset(m, 0, sizeof(m));
        m[0] = 1;
        m[1] = 'a';
        m[2] = 0;
        m[3] = 0;
        m[4] = 1;
        m[5] = 0;
        m[6] = 1;
        m[7] = m[8] = m[9] = m[10] = 0;
        m[11] = 0xff;
        m[12] = 0xff;
        off = 0;
        CHECK(dyn_dns_rr_decode(m, 20, &off, &rr) == DYN_DNS_E_SHORT,
            "a forged RDLENGTH must be refused");
        m[11] = 0;
        m[12] = 4;
        m[13] = 93;
        m[14] = 184;
        m[15] = 216;
        m[16] = 34;
        off = 0;
        CHECK(dyn_dns_rr_decode(m, 17, &off, &rr) == DYN_DNS_OK, "legal RR");
        CHECK(rr.type == DYN_DNS_T_A && rr.rdlen == 4, "type/rdlen");
        CHECK(rr.rdata && rr.rdata[0] == 93, "rdata borrows the message");
        CHECK(off == 17, "RR consumed to %zu", off);
    }

    {
        uint8_t m[300];
        size_t i, off;
        memset(m, 0, sizeof m);
        for (i = 0; i < 4; i++) {
            m[12 + i * 64] = 63;
            memset(m + 13 + i * 64, (int)('a' + i), 63);
        }
        m[12 + 256] = 0;
        n = dyn_dns_name_decode(m, sizeof m, 12, name, sizeof(name));
        CHECK(n == DYN_DNS_E_NAME, "a 257-octet wire name gave %d, want E_NAME (RFC 1035 3.1)", n);
        m[12 + 192] = 61;
        m[12 + 192 + 62] = 0;
        n = dyn_dns_name_decode(m, sizeof m, 12, name, sizeof(name));
        CHECK(n == 12 + 255 && strlen(name) == 253, "a 255-octet wire name gave %d (len %zu), want %d", n, strlen(name), 12 + 255);
        m[12 + 192] = 62;
        m[12 + 192 + 62] = 0x61;
        m[12 + 192 + 63] = 0;
        n = dyn_dns_name_decode(m, sizeof m, 12, name, sizeof(name));
        CHECK(n == DYN_DNS_E_NAME, "a 256-octet wire name gave %d, want E_NAME", n);
        n = dyn_dns_build_query(7, "a.b", DYN_DNS_T_A, buf, sizeof(buf));
        off = 12;
        CHECK(dyn_dns_skip_questions(buf, (size_t)n - 1, &off, 1) == DYN_DNS_E_SHORT && off == 12,
            "skip_questions on a question missing a QCLASS byte must fail and leave *off at 12, got %zu", off);
        off = 12;
        CHECK(dyn_dns_skip_questions(buf, (size_t)n, &off, 2) == DYN_DNS_E_SHORT && off == 12,
            "a failing second question must leave *off untouched, got %zu", off);
        buf[2] = (uint8_t)(buf[2] | 0x20);
        n = dyn_dns_begin_response(buf, (size_t)n, 4, m, sizeof m);
        CHECK(n > 0 && m[2] == 0xa1 && m[3] == 0x84, "a response must copy OPCODE 4 and RD (RFC 1035 4.1.1): %02x %02x", m[2], m[3]);
    }

    if (fk_only("dns-gen"))
        prop_gen();
    if (fk_only("dns-raw"))
        prop_raw();
    if (fk_only("dns-enc"))
        prop_enc();
    return fk_done("test_dns_codec");
}
