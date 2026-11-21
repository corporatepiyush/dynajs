#include "idna.h"

static const unsigned int dyn_idna_utf8_min[5] = {
    0x80,
    0x800,
    0x10000,
    0x00200000,
    0x04000000,
};

static const unsigned char dyn_idna_utf8_mask[5] = {
    0x1f,
    0xf,
    0x7,
    0x3,
    0x1,
};

int dyn_idna_utf8_next(const uint8_t* p, int max_len, const uint8_t** pp)
{
    int l, c, b, i;

    c = *p++;
    if (c < 0x80) {
        *pp = p;
        return c;
    }
    if (c >= 0xc0 && c <= 0xdf)
        l = 1;
    else if (c >= 0xe0 && c <= 0xef)
        l = 2;
    else if (c >= 0xf0 && c <= 0xf7)
        l = 3;
    else if (c >= 0xf8 && c <= 0xfb)
        l = 4;
    else if (c >= 0xfc && c <= 0xfd)
        l = 5;
    else
        return -1;
    if (l > (max_len - 1))
        return -1;
    c &= dyn_idna_utf8_mask[l - 1];
    for (i = 0; i < l; i++) {
        b = *p++;
        if (b < 0x80 || b >= 0xc0)
            return -1;
        c = (c << 6) | (b & 0x3f);
    }
    if (c < (int)dyn_idna_utf8_min[l - 1])
        return -1;
    *pp = p;
    return c;
}
