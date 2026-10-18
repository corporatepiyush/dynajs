#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "cutils.h"

void pstrcpy(char* buf, int buf_size, const char* str)
{
    int c;
    char* q = buf;

    if (buf_size <= 0)
        return;

    for (;;) {
        c = *str++;
        if (c == 0 || q >= buf + buf_size - 1)
            break;
        *q++ = c;
    }
    *q = '\0';
}

char* pstrcat(char* buf, int buf_size, const char* s)
{
    int len;
    len = strlen(buf);
    if (len < buf_size)
        pstrcpy(buf + len, buf_size - len, s);
    return buf;
}

int strstart(const char* str, const char* val, const char** ptr)
{
    const char *p, *q;
    p = str;
    q = val;
    while (*q != '\0') {
        if (*p != *q)
            return 0;
        p++;
        q++;
    }
    if (ptr)
        *ptr = p;
    return 1;
}

int has_suffix(const char* str, const char* suffix)
{
    size_t len = strlen(str);
    size_t slen = strlen(suffix);
    return (len >= slen && !memcmp(str + len - slen, suffix, slen));
}

static void* dbuf_default_realloc(void* opaque, void* ptr, size_t size)
{
    if (size == 0) {
        free(ptr);
        return NULL;
    }
    return realloc(ptr, size);
}

void dbuf_init2(DynBuf* s, void* opaque, DynBufReallocFunc* realloc_func)
{
    memset(s, 0, sizeof(*s));
    if (!realloc_func)
        realloc_func = dbuf_default_realloc;
    s->opaque = opaque;
    s->realloc_func = realloc_func;
}

void dbuf_init(DynBuf* s)
{
    dbuf_init2(s, NULL, NULL);
}

int dbuf_claim(DynBuf* s, size_t len)
{
    size_t new_size, size;
    uint8_t* new_buf;
    new_size = s->size + len;
    if (new_size < len)
        return -1;
    if (new_size > s->allocated_size) {
        if (s->error)
            return -1;
        size = s->allocated_size + (s->allocated_size / 2);
        if (size < s->allocated_size)
            return -1;
        if (size > new_size)
            new_size = size;
        if (new_size < 64)
            new_size = 64;
        new_buf = s->realloc_func(s->opaque, s->buf, new_size);
        if (!new_buf) {
            s->error = TRUE;
            return -1;
        }
        s->buf = new_buf;
        s->allocated_size = new_size;
    }
    return 0;
}

int dbuf_put(DynBuf* s, const uint8_t* data, size_t len)
{
    if (unlikely((s->allocated_size - s->size) < len)) {
        if (dbuf_claim(s, len))
            return -1;
    }
    memcpy_no_ub(s->buf + s->size, data, len);
    s->size += len;
    return 0;
}

int dbuf_put_self(DynBuf* s, size_t offset, size_t len)
{
    if (unlikely((s->allocated_size - s->size) < len)) {
        if (dbuf_claim(s, len))
            return -1;
    }
    memcpy(s->buf + s->size, s->buf + offset, len);
    s->size += len;
    return 0;
}

int __dbuf_putc(DynBuf* s, uint8_t c)
{
    return dbuf_put(s, &c, 1);
}

int __dbuf_put_u16(DynBuf* s, uint16_t val)
{
    return dbuf_put(s, (uint8_t*)&val, 2);
}

int __dbuf_put_u32(DynBuf* s, uint32_t val)
{
    return dbuf_put(s, (uint8_t*)&val, 4);
}

int __dbuf_put_u64(DynBuf* s, uint64_t val)
{
    return dbuf_put(s, (uint8_t*)&val, 8);
}

int dbuf_putstr(DynBuf* s, const char* str)
{
    return dbuf_put(s, (const uint8_t*)str, strlen(str));
}

int __attribute__((format(printf, 2, 3))) dbuf_printf(DynBuf* s,
    const char* fmt, ...)
{
    va_list ap;
    char buf[128];
    int len;

    va_start(ap, fmt);
    len = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (len < 0)
        return -1;
    if (len < sizeof(buf)) {
        return dbuf_put(s, (uint8_t*)buf, len);
    } else {
        if (dbuf_claim(s, len + 1))
            return -1;
        va_start(ap, fmt);
        vsnprintf((char*)(s->buf + s->size), s->allocated_size - s->size,
            fmt, ap);
        va_end(ap);
        s->size += len;
    }
    return 0;
}

void dbuf_free(DynBuf* s)
{
    if (s->buf) {
        s->realloc_func(s->opaque, s->buf, 0);
    }
    memset(s, 0, sizeof(*s));
}

int unicode_to_utf8(uint8_t* buf, unsigned int c)
{
    uint8_t* q = buf;

    if (c < 0x80) {
        *q++ = c;
    } else {
        if (c < 0x800) {
            *q++ = (c >> 6) | 0xc0;
        } else {
            if (c < 0x10000) {
                *q++ = (c >> 12) | 0xe0;
            } else {
                if (c < 0x00200000) {
                    *q++ = (c >> 18) | 0xf0;
                } else {
                    if (c < 0x04000000) {
                        *q++ = (c >> 24) | 0xf8;
                    } else if (c < 0x80000000) {
                        *q++ = (c >> 30) | 0xfc;
                        *q++ = ((c >> 24) & 0x3f) | 0x80;
                    } else {
                        return 0;
                    }
                    *q++ = ((c >> 18) & 0x3f) | 0x80;
                }
                *q++ = ((c >> 12) & 0x3f) | 0x80;
            }
            *q++ = ((c >> 6) & 0x3f) | 0x80;
        }
        *q++ = (c & 0x3f) | 0x80;
    }
    return q - buf;
}

static const unsigned int utf8_min_code[5] = {
    0x80,
    0x800,
    0x10000,
    0x00200000,
    0x04000000,
};

static const unsigned char utf8_first_code_mask[5] = {
    0x1f,
    0xf,
    0x7,
    0x3,
    0x1,
};

int unicode_from_utf8(const uint8_t* p, int max_len, const uint8_t** pp)
{
    int l, c, b, i;

    c = *p++;
    if (c < 0x80) {
        *pp = p;
        return c;
    }
    switch (c) {
    case 0xc0:
    case 0xc1:
    case 0xc2:
    case 0xc3:
    case 0xc4:
    case 0xc5:
    case 0xc6:
    case 0xc7:
    case 0xc8:
    case 0xc9:
    case 0xca:
    case 0xcb:
    case 0xcc:
    case 0xcd:
    case 0xce:
    case 0xcf:
    case 0xd0:
    case 0xd1:
    case 0xd2:
    case 0xd3:
    case 0xd4:
    case 0xd5:
    case 0xd6:
    case 0xd7:
    case 0xd8:
    case 0xd9:
    case 0xda:
    case 0xdb:
    case 0xdc:
    case 0xdd:
    case 0xde:
    case 0xdf:
        l = 1;
        break;
    case 0xe0:
    case 0xe1:
    case 0xe2:
    case 0xe3:
    case 0xe4:
    case 0xe5:
    case 0xe6:
    case 0xe7:
    case 0xe8:
    case 0xe9:
    case 0xea:
    case 0xeb:
    case 0xec:
    case 0xed:
    case 0xee:
    case 0xef:
        l = 2;
        break;
    case 0xf0:
    case 0xf1:
    case 0xf2:
    case 0xf3:
    case 0xf4:
    case 0xf5:
    case 0xf6:
    case 0xf7:
        l = 3;
        break;
    case 0xf8:
    case 0xf9:
    case 0xfa:
    case 0xfb:
        l = 4;
        break;
    case 0xfc:
    case 0xfd:
        l = 5;
        break;
    default:
        return -1;
    }
    if (l > (max_len - 1))
        return -1;
    c &= utf8_first_code_mask[l - 1];
    for (i = 0; i < l; i++) {
        b = *p++;
        if (b < 0x80 || b >= 0xc0)
            return -1;
        c = (c << 6) | (b & 0x3f);
    }
    if (c < utf8_min_code[l - 1])
        return -1;
    *pp = p;
    return c;
}

#if 0

#if defined(__EMSCRIPTEN__) || defined(__ANDROID__)

static void *rqsort_arg;
static int (*rqsort_cmp)(const void *, const void *, void *);

static int rqsort_cmp2(const void *p1, const void *p2)
{
    return rqsort_cmp(p1, p2, rqsort_arg);
}

void rqsort(void *base, size_t nmemb, size_t size,
            int (*cmp)(const void *, const void *, void *),
            void *arg)
{
    rqsort_arg = arg;
    rqsort_cmp = cmp;
    qsort(base, nmemb, size, rqsort_cmp2);
}

#endif

#else

typedef void (*exchange_f)(void* a, void* b, size_t size);
typedef int (*cmp_f)(const void*, const void*, void* opaque);

static void exchange_bytes(void* a, void* b, size_t size)
{
    uint8_t* ap = (uint8_t*)a;
    uint8_t* bp = (uint8_t*)b;

    while (size-- != 0) {
        uint8_t t = *ap;
        *ap++ = *bp;
        *bp++ = t;
    }
}

static void exchange_one_byte(void* a, void* b, size_t size)
{
    uint8_t* ap = (uint8_t*)a;
    uint8_t* bp = (uint8_t*)b;
    uint8_t t = *ap;
    *ap = *bp;
    *bp = t;
}

static void exchange_int16s(void* a, void* b, size_t size)
{
    uint16_t* ap = (uint16_t*)a;
    uint16_t* bp = (uint16_t*)b;

    for (size /= sizeof(uint16_t); size-- != 0;) {
        uint16_t t = *ap;
        *ap++ = *bp;
        *bp++ = t;
    }
}

static void exchange_one_int16(void* a, void* b, size_t size)
{
    uint16_t* ap = (uint16_t*)a;
    uint16_t* bp = (uint16_t*)b;
    uint16_t t = *ap;
    *ap = *bp;
    *bp = t;
}

static void exchange_int32s(void* a, void* b, size_t size)
{
    uint32_t* ap = (uint32_t*)a;
    uint32_t* bp = (uint32_t*)b;

    for (size /= sizeof(uint32_t); size-- != 0;) {
        uint32_t t = *ap;
        *ap++ = *bp;
        *bp++ = t;
    }
}

static void exchange_one_int32(void* a, void* b, size_t size)
{
    uint32_t* ap = (uint32_t*)a;
    uint32_t* bp = (uint32_t*)b;
    uint32_t t = *ap;
    *ap = *bp;
    *bp = t;
}

static void exchange_int64s(void* a, void* b, size_t size)
{
    uint64_t* ap = (uint64_t*)a;
    uint64_t* bp = (uint64_t*)b;

    for (size /= sizeof(uint64_t); size-- != 0;) {
        uint64_t t = *ap;
        *ap++ = *bp;
        *bp++ = t;
    }
}

static void exchange_one_int64(void* a, void* b, size_t size)
{
    uint64_t* ap = (uint64_t*)a;
    uint64_t* bp = (uint64_t*)b;
    uint64_t t = *ap;
    *ap = *bp;
    *bp = t;
}

static void exchange_int128s(void* a, void* b, size_t size)
{
    uint64_t* ap = (uint64_t*)a;
    uint64_t* bp = (uint64_t*)b;

    for (size /= sizeof(uint64_t) * 2; size-- != 0; ap += 2, bp += 2) {
        uint64_t t = ap[0];
        uint64_t u = ap[1];
        ap[0] = bp[0];
        ap[1] = bp[1];
        bp[0] = t;
        bp[1] = u;
    }
}

static void exchange_one_int128(void* a, void* b, size_t size)
{
    uint64_t* ap = (uint64_t*)a;
    uint64_t* bp = (uint64_t*)b;
    uint64_t t = ap[0];
    uint64_t u = ap[1];
    ap[0] = bp[0];
    ap[1] = bp[1];
    bp[0] = t;
    bp[1] = u;
}

static inline exchange_f exchange_func(const void* base, size_t size)
{
    switch (((uintptr_t)base | (uintptr_t)size) & 15) {
    case 0:
        if (size == sizeof(uint64_t) * 2)
            return exchange_one_int128;
        else
            return exchange_int128s;
    case 8:
        if (size == sizeof(uint64_t))
            return exchange_one_int64;
        else
            return exchange_int64s;
    case 4:
    case 12:
        if (size == sizeof(uint32_t))
            return exchange_one_int32;
        else
            return exchange_int32s;
    case 2:
    case 6:
    case 10:
    case 14:
        if (size == sizeof(uint16_t))
            return exchange_one_int16;
        else
            return exchange_int16s;
    default:
        if (size == 1)
            return exchange_one_byte;
        else
            return exchange_bytes;
    }
}

static void heapsortx(void* base, size_t nmemb, size_t size, cmp_f cmp, void* opaque)
{
    uint8_t* basep = (uint8_t*)base;
    size_t i, n, c, r;
    exchange_f swap = exchange_func(base, size);

    if (nmemb > 1) {
        i = (nmemb / 2) * size;
        n = nmemb * size;

        while (i > 0) {
            i -= size;
            for (r = i; (c = r * 2 + size) < n; r = c) {
                if (c < n - size && cmp(basep + c, basep + c + size, opaque) <= 0)
                    c += size;
                if (cmp(basep + r, basep + c, opaque) > 0)
                    break;
                swap(basep + r, basep + c, size);
            }
        }
        for (i = n - size; i > 0; i -= size) {
            swap(basep, basep + i, size);

            for (r = 0; (c = r * 2 + size) < i; r = c) {
                if (c < i - size && cmp(basep + c, basep + c + size, opaque) <= 0)
                    c += size;
                if (cmp(basep + r, basep + c, opaque) > 0)
                    break;
                swap(basep + r, basep + c, size);
            }
        }
    }
}

static inline void* med3(void* a, void* b, void* c, cmp_f cmp, void* opaque)
{
    return cmp(a, b, opaque) < 0 ? (cmp(b, c, opaque) < 0 ? b : (cmp(a, c, opaque) < 0 ? c : a)) : (cmp(b, c, opaque) > 0 ? b : (cmp(a, c, opaque) < 0 ? a : c));
}

void rqsort(void* base, size_t nmemb, size_t size, cmp_f cmp, void* opaque)
{
    struct {
        uint8_t* base;
        size_t count;
        int depth;
    } stack[50], *sp = stack;
    uint8_t *ptr, *pi, *pj, *plt, *pgt, *top, *m;
    size_t m4, i, lt, gt, span, span2;
    int c, depth;
    exchange_f swap = exchange_func(base, size);
    exchange_f swap_block = exchange_func(base, size | 128);

    if (nmemb < 2 || size <= 0)
        return;

    sp->base = (uint8_t*)base;
    sp->count = nmemb;
    sp->depth = 0;
    sp++;

    while (sp > stack) {
        sp--;
        ptr = sp->base;
        nmemb = sp->count;
        depth = sp->depth;

        while (nmemb > 6) {
            if (++depth > 50) {
                heapsortx(ptr, nmemb, size, cmp, opaque);
                nmemb = 0;
                break;
            }
            m4 = (nmemb >> 2) * size;
            m = med3(ptr + m4, ptr + 2 * m4, ptr + 3 * m4, cmp, opaque);
            swap(ptr, m, size);
            i = lt = 1;
            pi = plt = ptr + size;
            gt = nmemb;
            pj = pgt = top = ptr + nmemb * size;
            for (;;) {
                while (pi < pj && (c = cmp(ptr, pi, opaque)) >= 0) {
                    if (c == 0) {
                        swap(plt, pi, size);
                        lt++;
                        plt += size;
                    }
                    i++;
                    pi += size;
                }
                while (pi < (pj -= size) && (c = cmp(ptr, pj, opaque)) <= 0) {
                    if (c == 0) {
                        gt--;
                        pgt -= size;
                        swap(pgt, pj, size);
                    }
                }
                if (pi >= pj)
                    break;
                swap(pi, pj, size);
                i++;
                pi += size;
            }
            span = plt - ptr;
            span2 = pi - plt;
            lt = i - lt;
            if (span > span2)
                span = span2;
            swap_block(ptr, pi - span, span);
            span = top - pgt;
            span2 = pgt - pi;
            pgt = top - span2;
            gt = nmemb - (gt - i);
            if (span > span2)
                span = span2;
            swap_block(pi, top - span, span);

            if (lt > nmemb - gt) {
                sp->base = ptr;
                sp->count = lt;
                sp->depth = depth;
                sp++;
                ptr = pgt;
                nmemb -= gt;
            } else {
                sp->base = pgt;
                sp->count = nmemb - gt;
                sp->depth = depth;
                sp++;
                nmemb = lt;
            }
        }
        for (pi = ptr + size, top = ptr + nmemb * size; pi < top; pi += size) {
            for (pj = pi; pj > ptr && cmp(pj - size, pj, opaque) > 0; pj -= size)
                swap(pj, pj - size, size);
        }
    }
}

#endif
