/* gen-crc32c-pow.c -- regenerate the crc32c_pow[] table in src/core/dyn-hash.c.
 *
 *   cc -O2 -o /tmp/gen tools/gen-crc32c-pow.c && /tmp/gen
 *
 * The table is x^(2^i) mod P for i = 0..63, in GF(2)[x]/P with P the
 * Castagnoli polynomial in reversed (LSB-first) form. crc32c_shift() uses it
 * to advance a partial CRC over the bytes that followed it, which is what
 * recombines the three independent chains in crc32c_hw().
 *
 * It is a compile-time table rather than a lazily-filled static because
 * dyn_crc32c runs on any JS thread and a lazy fill is a data race -- writing
 * identical bytes from two threads is still a race (CLAUDE.md sec.6, sec.12).
 *
 * Nothing trusts this generator on its own: tests/test_crc32c_hw.c checks the
 * CRCs the table produces against the bit-serial definition, so a wrong entry
 * fails `make test-crc32c-hw` rather than shipping.
 */
#include <stdio.h>
#include <stdint.h>

/* Must match DYN_CRC32C_POLY in src/core/dyn-hash.h. */
#define POLY 0x82F63B78u

/* Carry-less multiply mod POLY, the same routine as crc32c_gf_mul(). */
static uint32_t gf_mul(uint32_t a, uint32_t b)
{
    uint32_t r = 0;
    int i;
    for (i = 0; i < 32; i++) {
        if (b & 0x80000000u)
            r ^= a;
        b <<= 1;
        a = (a >> 1) ^ (POLY & (0u - (a & 1u)));
    }
    return r;
}

int main(void)
{
    uint32_t pw[64], c = 0x80000000u;
    int i;

    c = (c >> 1) ^ (POLY & (0u - (c & 1u)));
    pw[0] = c;
    for (i = 1; i < 64; i++)
        pw[i] = gf_mul(pw[i - 1], pw[i - 1]);

    printf("static const uint32_t crc32c_pow[64] = {");
    for (i = 0; i < 64; i++) {
        if (i % 6 == 0)
            printf("\n   ");
        printf(" 0x%08Xu,", pw[i]);
    }
    printf("\n};\n");
    return 0;
}
