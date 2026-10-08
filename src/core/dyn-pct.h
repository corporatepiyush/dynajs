#ifndef DYNA_CORE_PCT_H
#define DYNA_CORE_PCT_H

#include <stddef.h>
#include <string.h>

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

static inline int dyn_pct_safe_0(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
        || (c >= '0' && c <= '9') || c == '*' || c == '-' || c == '.'
        || c == '_' || c == '~' || c == '!' || c == '\'' || c == '(' || c == ')';
}

static inline int dyn_pct_safe_1(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
        || (c >= '0' && c <= '9') || c == '*' || c == '-' || c == '.' || c == '_';
}

static inline int dyn_pct_safe_2(unsigned char c)
{
    return dyn_pct_safe_1(c) || c == '~';
}

static inline void dyn_pct_encode_core(void* ud, dyn_pct_sink sink,
    const char* s, size_t n, int form)
{
    static const char HEX[] = "0123456789ABCDEF";
    size_t i;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        int safe = form == 0 ? dyn_pct_safe_0(c)
                             : (form == 4 ? dyn_pct_safe_1(c)
                                          : dyn_pct_safe_2(c));
        if (safe) {
            sink(ud, &s[i], 1);
        } else if (form && form != 4 && c == ' ') {

            sink(ud, "+", 1);
        } else {
            char trip[3] = { '%', HEX[c >> 4], HEX[c & 0xF] };
            sink(ud, trip, 3);
        }
    }
}

static inline int dyn_pct_hexval(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static inline void dyn_pct_decode_core(void* ud, dyn_pct_sink sink,
    const char* s, size_t n, int plus_space)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (s[i] == '%' && i + 2 < n) {
            int h = dyn_pct_hexval((unsigned char)s[i + 1]);
            int l = dyn_pct_hexval((unsigned char)s[i + 2]);
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
