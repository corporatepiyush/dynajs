#include "netipk.h"

static size_t fmt_dec8(char* dst, unsigned v)
{
    size_t k = 0;
    if (v >= 100) {
        dst[k++] = (char)('0' + v / 100);
        v %= 100;
        dst[k++] = (char)('0' + v / 10);
        dst[k++] = (char)('0' + v % 10);
    } else if (v >= 10) {
        dst[k++] = (char)('0' + v / 10);
        dst[k++] = (char)('0' + v % 10);
    } else {
        dst[k++] = (char)('0' + v);
    }
    return k;
}

static size_t fmt_v4(char* dst, const uint8_t oct[4])
{
    size_t k = 0;
    int i;
    for (i = 0; i < 4; i++) {
        if (i)
            dst[k++] = '.';
        k += fmt_dec8(dst + k, oct[i]);
    }
    return k;
}

static size_t fmt_hex16(char* dst, unsigned v)
{
    static const char hx[] = "0123456789abcdef";
    size_t w = v > 0xfff ? 4 : v > 0xff ? 3
        : v > 0xf                       ? 2
                                        : 1,
           k;
    for (k = 0; k < w; k++)
        dst[k] = hx[(v >> (4 * (w - 1 - k))) & 0xf];
    return w;
}

size_t netipk_format_addr(const NetAddr* a, char* dst)
{
    unsigned g[8];
    int best_start = -1, best_len = 0, i;
    size_t o = 0;

    if (a->is4)
        return fmt_v4(dst, a->b);

    if (netipk_is_4in6(a)) {
        memcpy(dst, "::ffff:", 7);
        return 7 + fmt_v4(dst + 7, a->b + 12);
    }

    for (i = 0; i < 8; i++)
        g[i] = ((unsigned)a->b[2 * i] << 8) | a->b[2 * i + 1];

    for (i = 0; i < 8;) {
        if (g[i] == 0) {
            int j = i;
            while (j < 8 && g[j] == 0)
                j++;
            if (j - i > best_len) {
                best_len = j - i;
                best_start = i;
            }
            i = j;
        } else {
            i++;
        }
    }
    if (best_len < 2)
        best_start = -1;

    for (i = 0; i < 8; i++) {
        if (i == best_start) {
            dst[o++] = ':';
            dst[o++] = ':';
            i = best_start + best_len;
            if (i >= 8)
                break;
        } else if (i > 0) {
            dst[o++] = ':';
        }
        o += fmt_hex16(dst + o, g[i]);
    }
    return o;
}
