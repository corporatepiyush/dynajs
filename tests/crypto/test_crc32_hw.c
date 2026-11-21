/*
 * test_crc32_hw.c -- dyn_crc32 and dyn_crc32_update (IEEE 802.3, reflected poly 0xEDB88320, init/xorout 0xFFFFFFFF)
 * against a bitwise reference. crc-2: on aarch64 the hardware path is size-tiered like dyn_crc32c (crc-1): one crc32
 * lane below 3*64 bytes, then greedy 3-lane rounds of 3*1024, 3*256 and 3*64 bytes combined through byte-sliced shift
 * tables (built once, published with release/acquire; a call before publication takes the single lane). The
 * streaming entry dyn_crc32_update starts from any prior CRC, so it is checked by chaining updates over random splits.
 * Covered: the check value and short-string vectors, every tier boundary (3B-1, 3B, 3B+1) and every sum of tier rounds
 * with odd tails 1..15, all lengths 0..4096, strided lengths to 70000, unaligned starts, random cases. The file is also
 * built with DYN_CRC32C_HW_MIN=SIZE_MAX (the slice-by-8 path, the only one on x86) and for x86_64 under Rosetta.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../lib/dyn-xalloc.h"
uint32_t dyn_crc32(const uint8_t* d, size_t n);
uint32_t dyn_crc32_update(uint32_t crc, const uint8_t* d, size_t n);
static uint32_t ref_crc32(const uint8_t* d, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    size_t i;
    int k;
    for (i = 0; i < n; i++) {
        c ^= d[i];
        for (k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return c ^ 0xFFFFFFFFu;
}
static size_t bad, checked;
static void check(const uint8_t* d, size_t n, const char* what)
{
    uint32_t want = ref_crc32(d, n), got = dyn_crc32(d, n), up = dyn_crc32_update(0, d, n);
    checked++;
    if (got != want || up != want) {
        if (bad < 8)
            printf("  MISMATCH %s n=%zu got %08x update %08x want %08x\n", what, n, got, up, want);
        bad++;
    }
}
static void vec(const char* s, uint32_t want)
{
    size_t n = strlen(s);
    uint32_t got = dyn_crc32((const uint8_t*)s, n);
    checked++;
    if (got != want || ref_crc32((const uint8_t*)s, n) != want) {
        printf("  VECTOR \"%s\" got %08x want %08x\n", s, got, want);
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
    uint8_t* buf = xmalloc(70000 + 64);
    vec("", 0x00000000u);
    vec("a", 0xE8B7BE43u);
    vec("abc", 0x352441C2u);
    vec("123456789", 0xCBF43926u);
    vec("The quick brown fox jumps over the lazy dog", 0x414FA339u);
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
            size_t base = 3 * tiers[t] - 1 + k, tail;
            for (tail = 0; tail < 16; tail++) {
                check(buf, base + tail, "tier boundary");
                check(buf + 1 + tail, base, "tier boundary unaligned");
            }
        }
    for (a = 0; a < 4; a++)
        for (b2 = 0; b2 < 4; b2++)
            for (c = 0; c < 4; c++) {
                size_t base = a * 3 * 1024 + b2 * 3 * 256 + c * 3 * 64, tail;
                for (tail = 0; tail < 16; tail++)
                    if (base + tail <= 70000)
                        check(buf, base + tail, "tier sum");
            }
    for (k = 0; k < 3000; k++) {
        size_t off = rnd() % 64, len = rnd() % (k & 1 ? 20000 : 2048), i;
        for (i = 0; i < len; i++)
            buf[off + i] = (uint8_t)rnd();
        check(buf + off, len, "random");
    }
    for (k = 0; k < 2000; k++) {
        size_t len = rnd() % 20000, pos = 0;
        uint32_t crc = 0;
        for (n = 0; n < len; n++)
            buf[n] = (uint8_t)rnd();
        while (pos < len) {
            size_t step = 1 + rnd() % (k & 1 ? 4000 : 300);
            if (step > len - pos)
                step = len - pos;
            crc = dyn_crc32_update(crc, buf + pos, step);
            pos += step;
        }
        checked++;
        if (crc != ref_crc32(buf, len)) {
            if (bad < 8)
                printf("  MISMATCH streaming len=%zu got %08x want %08x\n", len, crc, ref_crc32(buf, len));
            bad++;
        }
    }
    printf("crc32 differential: %zu checked, %zu mismatches\n", checked, bad);
    return bad != 0;
}
