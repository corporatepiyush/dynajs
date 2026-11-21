/*
 * test_crc32c_hw.c -- dyn_crc32c against a bitwise reference (poly 0x82F63B78, reflected, init/xorout 0xFFFFFFFF).
 * crc-1: dyn_crc32c is size-tiered: one crc32c lane below 3*64 bytes, then greedy 3-lane rounds of 3*1024, 3*256 and
 * 3*64 bytes whose lane CRCs are combined through byte-sliced shift tables (built once, published with release/acquire;
 * until published a call takes the single lane). Covered here: the RFC 3720 B.4 vectors, every tier boundary
 * (3B-1, 3B, 3B+1) and every sum of tier rounds with odd tails 1..15, all lengths 0..4096, strided lengths to 70000,
 * unaligned starts, and random lengths/offsets/contents. The same file is built a second time with
 * DYN_CRC32C_HW_MIN=SIZE_MAX so the slice-by-8 fallback answers the same vectors, and once for x86_64 (SSE4.2 path,
 * run under Rosetta on Apple silicon when available).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
uint32_t dyn_crc32c(const uint8_t* d, size_t n);
uint32_t dyn_crc32_poly(const uint8_t* d, size_t n, uint32_t poly);
static uint32_t ref_crc32c(const uint8_t* d, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    size_t i;
    int k;
    for (i = 0; i < n; i++) {
        c ^= d[i];
        for (k = 0; k < 8; k++)
            c = (c >> 1) ^ (0x82F63B78u & (0u - (c & 1u)));
    }
    return c ^ 0xFFFFFFFFu;
}
static size_t bad, checked;
static void check(const uint8_t* d, size_t n, const char* what)
{
    uint32_t got = dyn_crc32c(d, n), want = ref_crc32c(d, n);
    checked++;
    if (got != want) {
        if (bad < 8)
            printf("  MISMATCH %s n=%zu got %08x want %08x\n", what, n, got, want);
        bad++;
    }
}
static void vec(const uint8_t* d, size_t n, uint32_t want, const char* name)
{
    uint32_t got = dyn_crc32c(d, n);
    checked++;
    if (got != want || ref_crc32c(d, n) != want || dyn_crc32_poly(d, n, 0x82F63B78u) != want) {
        printf("  VECTOR %s got %08x want %08x\n", name, got, want);
        bad++;
    }
}
static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return (uint32_t)(rng >> 16);
}
int main(void)
{
    static const size_t tiers[] = { 64, 256, 1024 };
    size_t n, t, a, b2, c, k;
    uint8_t* buf = malloc(70000 + 64);
    uint8_t v[48];
    for (n = 0; n < 32; n++)
        v[n] = 0;
    vec(v, 32, 0x8A9136AAu, "rfc3720 32x00");
    for (n = 0; n < 32; n++)
        v[n] = 0xFF;
    vec(v, 32, 0x62A8AB43u, "rfc3720 32xFF");
    for (n = 0; n < 32; n++)
        v[n] = (uint8_t)n;
    vec(v, 32, 0x46DD794Eu, "rfc3720 incrementing");
    for (n = 0; n < 32; n++)
        v[n] = (uint8_t)(31 - n);
    vec(v, 32, 0x113FDB5Cu, "rfc3720 decrementing");
    {
        static const uint8_t pdu[48] = { 0x01, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00, 0x18,
            0x28, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
        vec(pdu, 48, 0xD9963A56u, "rfc3720 iSCSI read PDU");
    }
    vec((const uint8_t*)"123456789", 9, 0xE3069283u, "check 123456789");
    for (n = 0; n < 70000 + 64; n++)
        buf[n] = (uint8_t)(n * 167u + 13u);
    for (n = 0; n <= 4096; n++)
        check(buf, n, "all");
    for (n = 4096; n <= 70000; n += 997)
        check(buf, n, "strided");
    for (n = 1; n < 64; n++)
        check(buf + n, 5000, "unaligned");
    for (t = 0; t < 3; t++)
        for (k = 0; k < 3; k++) {
            size_t base = 3 * tiers[t] - 1 + k;
            size_t tail;
            for (tail = 0; tail < 16; tail++) {
                check(buf, base + tail, "tier boundary");
                check(buf + 1 + tail, base, "tier boundary unaligned");
            }
        }
    for (a = 0; a < 4; a++)
        for (b2 = 0; b2 < 4; b2++)
            for (c = 0; c < 4; c++) {
                size_t base = a * 3 * 1024 + b2 * 3 * 256 + c * 3 * 64;
                size_t tail;
                for (tail = 0; tail < 16; tail++)
                    if (base + tail <= 70000)
                        check(buf, base + tail, "tier sum");
            }
    for (k = 0; k < 3000; k++) {
        size_t off = rnd() % 64, len = rnd() % (k & 1 ? 20000 : 2048);
        size_t i;
        for (i = 0; i < len; i++)
            buf[off + i] = (uint8_t)rnd();
        check(buf + off, len, "random");
    }
    printf("crc32c differential: %zu checked, %zu mismatches\n", checked, bad);
    return bad != 0;
}
