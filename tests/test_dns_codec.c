#include "dyn-dns.h"

#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { \
    printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

int main(void)
{
    uint8_t buf[1024];
    char name[DYN_DNS_MAX_NAME + 1];
    int n;

    setvbuf(stdout, NULL, _IOLBF, 0);

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
        uint8_t m[64];
        memset(m, 0, sizeof(m));
        m[0] = 3; memcpy(m + 1, "www", 3);
        m[4] = 7; memcpy(m + 5, "example", 7);
        m[12] = 3; memcpy(m + 13, "com", 3);
        m[16] = 0;
        m[17] = 2; memcpy(m + 18, "ns", 2);
        m[20] = 0xc0; m[21] = 4;
        n = dyn_dns_name_decode(m, 22, 17, name, sizeof(name));
        CHECK(n == 22, "a legal pointer consumed %d, want 22", n);
        CHECK(strcmp(name, "ns.example.com") == 0,
              "pointer-compressed name decoded as '%s'", name);
    }

    {
        uint8_t m[8];
        memset(m, 0, sizeof(m));
        m[0] = 0xc0; m[1] = 0;
        n = dyn_dns_name_decode(m, 2, 0, name, sizeof(name));
        CHECK(n == DYN_DNS_E_LOOP, "self-pointer gave %d (%s), want E_LOOP",
              n, dyn_dns_strerror(n));
    }

    {
        uint8_t m[8];
        memset(m, 0, sizeof(m));
        m[0] = 0xc0; m[1] = 2;
        m[2] = 0xc0; m[3] = 0;
        n = dyn_dns_name_decode(m, 4, 0, name, sizeof(name));
        CHECK(n == DYN_DNS_E_LOOP, "a->b->a gave %d (%s), want E_LOOP",
              n, dyn_dns_strerror(n));
        n = dyn_dns_name_decode(m, 4, 2, name, sizeof(name));
        CHECK(n == DYN_DNS_E_LOOP, "entering the cycle at b gave %d", n);
    }

    {
        uint8_t m[16];
        memset(m, 0, sizeof(m));
        m[0] = 0xc0; m[1] = 8;
        m[8] = 1; m[9] = 'a'; m[10] = 0;
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
                  "is large, got %d (%s)", n, dyn_dns_strerror(n));
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
        m[0] = 1; m[1] = 'a'; m[2] = 0;
        m[3] = 0; m[4] = 1;
        m[5] = 0; m[6] = 1;
        m[7] = m[8] = m[9] = m[10] = 0;
        m[11] = 0xff; m[12] = 0xff;
        off = 0;
        CHECK(dyn_dns_rr_decode(m, 20, &off, &rr) == DYN_DNS_E_SHORT,
              "a forged RDLENGTH must be refused");
        m[11] = 0; m[12] = 4;
        m[13] = 93; m[14] = 184; m[15] = 216; m[16] = 34;
        off = 0;
        CHECK(dyn_dns_rr_decode(m, 17, &off, &rr) == DYN_DNS_OK, "legal RR");
        CHECK(rr.type == DYN_DNS_T_A && rr.rdlen == 4, "type/rdlen");
        CHECK(rr.rdata && rr.rdata[0] == 93, "rdata borrows the message");
        CHECK(off == 17, "RR consumed to %zu", off);
    }

    if (fails == 0) printf("test_dns_codec: all tests passed\n");
    else printf("test_dns_codec: %d FAILED\n", fails);
    return fails != 0;
}
