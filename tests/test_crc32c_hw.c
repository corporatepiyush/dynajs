#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
uint32_t dyn_crc32c(const uint8_t *d, size_t n);
uint32_t dyn_crc32_poly(const uint8_t *d, size_t n, uint32_t poly);
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
    for (n = 4096; n <= 70000; n += 997) {
        if (dyn_crc32c(b, n) != ref_crc32c(b, n)) { if (bad < 5) printf("  MISMATCH at n=%zu\n", n); bad++; }
        checked++;
    }
    for (n = 1; n < 64; n++) {
        if (dyn_crc32c(b + n, 5000) != ref_crc32c(b + n, 5000)) { bad++; }
        checked++;
    }
    printf("crc32c differential: %zu checked, %zu mismatches\n", checked, bad);
    return bad != 0;
}
