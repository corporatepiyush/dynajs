#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dyna-simd-kernels.h"

static size_t ref_find(const uint8_t *p, size_t n, const uint8_t *bm)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (bm[p[i] >> 3] & (uint8_t)(1u << (p[i] & 7))) return i;
    return SIZE_MAX;
}

int main(void)
{
    static uint8_t buf[1024];
    uint8_t bm[32];
    unsigned long cases = 0, fails = 0;
    int t;

    simd_init();
#if defined(__x86_64__)
    __builtin_cpu_init();
    printf("#SB isa: sse4.2=%d avx2=%d avx512f=%d\n",
           __builtin_cpu_supports("sse4.2"), __builtin_cpu_supports("avx2"),
           __builtin_cpu_supports("avx512f"));
#elif defined(__aarch64__) || defined(__ARM_NEON)
    printf("#SB isa: neon\n");
#else
    printf("#SB isa: scalar\n");
#endif
    for (t = 0; t < 40; t++) {
        unsigned seed = (unsigned)t * 2654435761u + 1u;
        size_t i, n, off;
        memset(bm, 0, sizeof bm);
        for (i = 0; i < 256; i++) {
            seed = seed * 1103515245u + 12345u;
            if ((seed >> 16) % 100u < (unsigned)(t * 3 + 1))
                bm[i >> 3] |= (uint8_t)(1u << (i & 7));
        }
        for (i = 0; i < sizeof buf; i++) {
            seed = seed * 1103515245u + 12345u;
            buf[i] = (uint8_t)(seed >> 24);
        }
        for (off = 0; off < 40; off++) {
            for (n = 0; n <= 200; n++) {
                size_t got, want;
                if (off + n > sizeof buf) break;
                got  = simd.find_bitmap(buf + off, n, bm);
                want = ref_find(buf + off, n, bm);
                cases++;
                if (got != want) {
                    if (fails < 8)
                        printf("MISMATCH t=%d off=%zu n=%zu got=%zu want=%zu\n",
                               t, off, n, got, want);
                    fails++;
                }
            }
        }
        memset(bm, 0xff, sizeof bm);
        cases++; if (simd.find_bitmap(buf, 200, bm) != 0) fails++;
        memset(bm, 0x00, sizeof bm);
        cases++; if (simd.find_bitmap(buf, 200, bm) != SIZE_MAX) fails++;
    }
    printf("#SB find_bitmap: %lu cases, %lu mismatches\n", cases, fails);
    return fails ? 1 : 0;
}
