#include "bytes-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_BYTES)

int dyn_bytes_from_index(JSContext* ctx, JSValueConst v, size_t* out)
{
    int64_t from;
    if (JS_IsUndefined(v)) {
        *out = DYN_BYTES_IDX_UNSET;
        return 0;
    }
    if (JS_ToInt64(ctx, &from, v))
        return -1;
    if (from < 0)
        from = 0;
    *out = (size_t)from;
    return 0;
}

static size_t dyn_bytes_last_byte(const uint8_t* text, size_t tlen, uint8_t c)
{
    const uint64_t ones = 0x0101010101010101ULL, highs = 0x8080808080808080ULL;
    const uint64_t pat = ones * c;
    size_t k = tlen;
    while (k >= 8) {
        uint64_t w, x;
        memcpy(&w, text + k - 8, 8);
        x = w ^ pat;
        if ((x - ones) & ~x & highs)
            break;
        k -= 8;
    }
    while (k > 0) {
        k--;
        if (text[k] == c)
            return k;
    }
    return (size_t)-1;
}

static size_t dyn_bytes_last_find(const uint8_t* text, size_t tlen,
    const uint8_t* pat, size_t plen)
{
    size_t lps_small[256];
    uint8_t rp_small[256];
    size_t* lps = lps_small;
    uint8_t* rp = rp_small;
    size_t i, j = 0, found = (size_t)-1;

    if (plen == 0 || tlen < plen)
        return (size_t)-1;
    if (plen == 1)
        return dyn_bytes_last_byte(text, tlen, pat[0]);
    if (plen > countof(lps_small)) {
        lps = (size_t*)dyn_nat_malloc(plen * sizeof(*lps));
        rp = (uint8_t*)dyn_nat_malloc(plen);
        if (!lps || !rp) {
            size_t k = tlen - plen + 1;
            dyn_nat_free(lps);
            dyn_nat_free(rp);
            while (k > 0) {
                k--;
                if (memcmp(text + k, pat, plen) == 0)
                    return k;
            }
            return (size_t)-1;
        }
    }
    for (i = 0; i < plen; i++)
        rp[i] = pat[plen - 1 - i];
    lps[0] = 0;
    for (i = 1; i < plen; i++) {
        size_t len = lps[i - 1];
        while (len && rp[i] != rp[len])
            len = lps[len - 1];
        if (rp[i] == rp[len])
            len++;
        lps[i] = len;
    }
    for (i = tlen; i > 0;) {
        uint8_t c = text[--i];
        while (j && c != rp[j])
            j = lps[j - 1];
        if (c == rp[j])
            j++;
        if (j == plen) {
            found = i;
            break;
        }
    }
    if (lps != lps_small) {
        dyn_nat_free(lps);
        dyn_nat_free(rp);
    }
    return found;
}

JSValue dyn_bytes_index_of(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    uint8_t needle_byte;
    const uint8_t* pat;
    size_t plen;
    uint8_t* buf;
    size_t n, pos, start = 0;
    (void)this_val;

    if (argc < 2)
        return JS_ThrowTypeError(ctx, "indexOf(buf, needle)");
    if (dyn_bytes_from_index(ctx, argv[2], &start))
        return JS_EXCEPTION;
    if (dyn_bytes_needle(ctx, argv[1], &needle_byte, &pat, &plen))
        return JS_EXCEPTION;
    if (dyn_bytes_view(ctx, argv[0], &buf, &n))
        return JS_EXCEPTION;
    if (start == DYN_BYTES_IDX_UNSET)
        start = 0;
    if (start > n)
        start = n;

    if (plen == 0)
        return JS_NewInt64(ctx, (int64_t)start);
    if (plen > n || start > n - plen)
        return JS_NewInt32(ctx, -1);
    pos = simd.strfind(buf + start, n - start, pat, plen);
    return pos == SIZE_MAX ? JS_NewInt32(ctx, -1)
                           : JS_NewInt64(ctx, (int64_t)(pos + start));
}

JSValue dyn_bytes_last_index_of(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    uint8_t needle_byte;
    const uint8_t* pat;
    size_t plen;
    uint8_t* buf;
    size_t n, pos, start = 0, hi;
    (void)this_val;

    if (argc < 2)
        return JS_ThrowTypeError(ctx, "lastIndexOf(buf, needle)");
    if (dyn_bytes_from_index(ctx, argv[2], &start))
        return JS_EXCEPTION;
    if (dyn_bytes_needle(ctx, argv[1], &needle_byte, &pat, &plen))
        return JS_EXCEPTION;
    if (dyn_bytes_view(ctx, argv[0], &buf, &n))
        return JS_EXCEPTION;
    if (start == DYN_BYTES_IDX_UNSET || start > n)
        start = n;

    if (plen == 0)
        return JS_NewInt64(ctx, (int64_t)start);
    if (plen > n)
        return JS_NewInt32(ctx, -1);
    hi = n - plen;
    if (start < hi)
        hi = start;
    pos = dyn_bytes_last_find(buf, hi + plen, pat, plen);
    return pos == (size_t)-1 ? JS_NewInt32(ctx, -1) : JS_NewInt64(ctx, (int64_t)pos);
}

JSValue dyn_bytes_count(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    uint8_t needle_byte;
    const uint8_t* pat;
    size_t plen;
    uint8_t* buf;
    size_t n, scan, cnt, start = 0;
    (void)this_val;

    if (argc < 2)
        return JS_ThrowTypeError(ctx, "count(buf, needle)");
    if (dyn_bytes_from_index(ctx, argv[2], &start))
        return JS_EXCEPTION;
    if (dyn_bytes_needle(ctx, argv[1], &needle_byte, &pat, &plen))
        return JS_EXCEPTION;
    if (dyn_bytes_view(ctx, argv[0], &buf, &n))
        return JS_EXCEPTION;
    if (start == DYN_BYTES_IDX_UNSET)
        start = 0;
    if (start > n)
        start = n;

    if (plen == 0)
        return JS_NewInt64(ctx, (int64_t)(n - start) + 1);
    if (plen > n || start > n - plen)
        return JS_NewInt32(ctx, 0);
    if (plen == 1)
        return JS_NewInt64(ctx, (int64_t)simd.count_u8(buf + start, pat[0], n - start));

    cnt = 0;
    scan = start;
    while (scan <= n - plen) {
        size_t pos = simd.strfind(buf + scan, n - scan, pat, plen);
        if (pos == SIZE_MAX)
            break;
        cnt++;
        scan += pos + plen;
    }
    return JS_NewInt64(ctx, (int64_t)cnt);
}

#endif
