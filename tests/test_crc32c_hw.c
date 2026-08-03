/* test_crc32c_hw.c -- the hardware CRC32C path against the DEFINITION.
 *
 * dyn_crc32c dispatches to an ISA instruction (SSE4.2 on x86-64, the ARMv8
 * CRC32 extension on arm64) above DYN_CRC32C_HW_MIN bytes and to a slice-by-8
 * table below it. Both must equal the bit-serial definition, which is what
 * this file implements as the reference -- not the table, because a reference
 * that shares code with the thing under test proves nothing.
 *
 * Covers every length 0..4096 (so both sides of the gate and of the 3*64
 * three-chain threshold), a stride past 64 KB, and unaligned starts, which
 * the three-way split makes no alignment assumption about.
 *
 * PROVED TO FIRE: xoring 1 into the second recombination step gives 4035
 * mismatches, the first at n=192 = 3*64, exactly where the split begins. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
uint32_t dyn_crc32c(const uint8_t *d, size_t n);
uint32_t dyn_crc32_poly(const uint8_t *d, size_t n, uint32_t poly);
/* the table path, reachable through dyn_crc32_poly only when hw is off, so
 * re-implement the reference here: bit-serial, the definition itself. */
static uint32_t ref_crc32c(const uint8_t *d, size_t n)
{
    uint32_t c = 0xFFFFFFFFu; size_t i; int k;
    for (i = 0; i < n; i++) {
        c ^= d[i];
        for (k = 0; k < 8; k++) c = (c >> 1) ^ (0x82F63B78u & (0u - (c & 1u)));
    }
    return c ^ 0xFFFFFFFFu;
}
int main(void)
{
    size_t n, bad = 0, checked = 0;
    uint8_t *b = malloc(70000);
    for (n = 0; n < 70000; n++) b[n] = (uint8_t)(n * 167u + 13u);
    for (n = 0; n <= 4096; n++) {
        if (dyn_crc32c(b, n) != ref_crc32c(b, n)) { if (bad < 5) printf("  MISMATCH at n=%zu\n", n); bad++; }
        checked++;
    }
    /* the sizes the three-chain path actually uses */
    for (n = 4096; n <= 70000; n += 997) {
        if (dyn_crc32c(b, n) != ref_crc32c(b, n)) { if (bad < 5) printf("  MISMATCH at n=%zu\n", n); bad++; }
        checked++;
    }
    /* unaligned starts: the three-chain split assumes nothing about alignment */
    for (n = 1; n < 64; n++) {
        if (dyn_crc32c(b + n, 5000) != ref_crc32c(b + n, 5000)) { bad++; }
        checked++;
    }
    printf("crc32c differential: %zu checked, %zu mismatches\n", checked, bad);
    return bad != 0;
}
