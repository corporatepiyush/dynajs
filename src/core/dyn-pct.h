#ifndef DYNA_CORE_PCT_H
#define DYNA_CORE_PCT_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "dyn-common.h"

typedef void (*dyn_pct_sink)(void* ud, const char* bytes, size_t n);

typedef struct {
    char* p;
    size_t n;
} dyn_pct_buf_t;
static inline void dyn_pct_buf_sink(void* ud, const char* b, size_t k)
{
    dyn_pct_buf_t* o = (dyn_pct_buf_t*)ud;
    memcpy(o->p + o->n, b, k);
    o->n += k;
}

static const uint8_t dyn_pct_class[256] = {
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    1,
    0,
    0,
    0,
    0,
    0,
    1,
    1,
    1,
    7,
    0,
    0,
    7,
    7,
    0,
    7,
    7,
    7,
    7,
    7,
    7,
    7,
    7,
    7,
    7,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    7,
    7,
    7,
    7,
    7,
    7,
    7,
    7,
    7,
    7,
    7,
    7,
    7,
    7,
    7,
    7,
    7,
    7,
    7,
    7,
    7,
    7,
    7,
    7,
    7,
    7,
    0,
    0,
    0,
    0,
    7,
    0,
    7,
    7,
    7,
    7,
    7,
    7,
    7,
    7,
    7,
    7,
    7,
    7,
    7,
    7,
    7,
    7,
    7,
    7,
    7,
    7,
    7,
    7,
    7,
    7,
    7,
    7,
    0,
    0,
    0,
    5,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
};

static inline int dyn_pct_safe_0(unsigned char c)
{
    return dyn_pct_class[c] & 1;
}

static inline int dyn_pct_safe_1(unsigned char c)
{
    return (dyn_pct_class[c] >> 1) & 1;
}

static inline int dyn_pct_safe_2(unsigned char c)
{
    return (dyn_pct_class[c] >> 2) & 1;
}

static inline void dyn_pct_encode_core(void* ud, dyn_pct_sink sink,
    const char* s, size_t n, int form)
{
    static const char HEX[] = "0123456789ABCDEF";
    size_t i;
    unsigned bit = form == 0 ? 0u : (form == 4 ? 1u : 2u);
    i = 0;
    while (i < n) {
        size_t j = i;
        unsigned char c;
        while (j < n && ((dyn_pct_class[(unsigned char)s[j]] >> bit) & 1u))
            j++;
        if (j > i)
            sink(ud, s + i, j - i);
        if (j >= n)
            break;
        c = (unsigned char)s[j];
        if (form && form != 4 && c == ' ') {
            sink(ud, "+", 1);
        } else {
            char trip[3] = { '%', HEX[c >> 4], HEX[c & 0xF] };
            sink(ud, trip, 3);
        }
        i = j + 1;
    }
}

static inline void dyn_pct_decode_core(void* ud, dyn_pct_sink sink,
    const char* s, size_t n, int plus_space)
{
    size_t i;
    for (i = 0; i < n; i++) {
        size_t j = i;
        while (j < n && s[j] != '%' && !(plus_space && s[j] == '+'))
            j++;
        if (j > i) {
            sink(ud, s + i, j - i);
            i = j;
            if (i >= n)
                break;
        }
        if (s[i] == '%' && i + 2 < n) {
            int h = dyn_hexval((unsigned char)s[i + 1]);
            int l = dyn_hexval((unsigned char)s[i + 2]);
            if (h >= 0 && l >= 0) {
                char b = (char)((h << 4) | l);
                sink(ud, &b, 1);
                i += 2;
                continue;
            }
        }
        if (plus_space && s[i] == '+') {
            sink(ud, " ", 1);
        } else {
            sink(ud, &s[i], 1);
        }
    }
}

#endif
