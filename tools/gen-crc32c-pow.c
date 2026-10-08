#include <stdio.h>
#include <stdint.h>

#define POLY 0x82F63B78u

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
