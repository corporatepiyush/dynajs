/*
 * IPv4/IPv6 addresses and CIDR prefixes, part of dyna:net.
 *
 * Addresses are immutable value handles. `Prefix` is a compiled capability;
 * measured crossover is N=10 (1.33x at N=1000).
 * Full API: see the dyna:* module in dyna-libc.h.
 */
#include "dyna-nat.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_NET)

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>   /* malloc/free: the Prefix capability owns its compiled state */
#include <string.h>

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

/* A parsed address. is4 == 1 => IPv4, octets in b[0..3] (b[4..15] are 0).
 * is4 == 0 => IPv6, all 16 bytes in b[0..15]. A 4-in-6 mapped address is an
 * IPv6 value (is4 == 0) whose b[] is "::ffff:a.b.c.d". */
typedef struct {
    int is4;
    uint8_t b[16];
} NetAddr;

/* ==================================================================== *
 *  parsing                                                              *
 * ==================================================================== */

/* Parse the entire [s,s+n) as a strict dotted-quad IPv4 into out[4].
 * Returns 0 on success, -1 on any malformation. */
static int parse_ipv4(const char *s, size_t n, uint8_t out[4])
{
    size_t i = 0;
    int field;

    for (field = 0; field < 4; field++) {
        unsigned val = 0;
        int digits = 0;
        size_t start = i;
        while (i < n && s[i] >= '0' && s[i] <= '9') {
            val = val * 10u + (unsigned)(s[i] - '0');
            digits++;
            if (digits > 3 || val > 255)
                return -1;
            i++;
        }
        if (digits == 0)
            return -1;                    /* empty field */
        if (digits > 1 && s[start] == '0')
            return -1;                    /* leading zero */
        out[field] = (uint8_t)val;
        if (field < 3) {
            if (i >= n || s[i] != '.')
                return -1;
            i++;                          /* consume '.' */
        }
    }
    return (i == n) ? 0 : -1;             /* reject trailing garbage */
}

/* Parse the entire [s,s+n) as an IPv6 (incl. "::" and embedded IPv4 tail)
 * into out[16]. Returns 0 / -1. */
static int parse_ipv6(const char *s, size_t n, uint8_t out[16])
{
    int i;                  /* byte index into out, 0..16 */
    int ellipsis = -1;      /* out-byte position where "::" was seen, or -1 */

    for (i = 0; i < 16; i++)
        out[i] = 0;

    /* leading "::" */
    if (n >= 2 && s[0] == ':' && s[1] == ':') {
        ellipsis = 0;
        s += 2;
        n -= 2;
        if (n == 0)
            return 0;                     /* "::" == all zeros */
    }

    i = 0;
    while (i < 16) {
        uint32_t acc = 0;
        size_t off = 0;

        while (off < n) {
            char c = s[off];
            int d;
            if (c >= '0' && c <= '9')
                d = c - '0';
            else if (c >= 'a' && c <= 'f')
                d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F')
                d = c - 'A' + 10;
            else
                break;
            acc = (acc << 4) + (uint32_t)d;
            off++;
            if (off > 4)
                return -1;                /* each group has <= 4 digits */
            if (acc > 0xffff)
                return -1;
        }
        if (off == 0)
            return -1;                    /* empty field */

        /* embedded IPv4 tail (e.g. "::ffff:1.2.3.4", "1:2:3:4:5:6:1.2.3.4") */
        if (off < n && s[off] == '.') {
            if (ellipsis < 0 && i != 12)
                return -1;                /* IPv4 must be the final 2 groups */
            if (i + 4 > 16)
                return -1;
            if (parse_ipv4(s, n, out + i) != 0)
                return -1;                /* rest of string must be a full v4 */
            i += 4;
            n = 0;                        /* fully consumed */
            break;
        }

        out[i] = (uint8_t)(acc >> 8);
        out[i + 1] = (uint8_t)(acc & 0xff);
        i += 2;

        s += off;
        n -= off;
        if (n == 0)
            break;

        if (s[0] != ':')
            return -1;                    /* groups joined by ':' only */
        if (n == 1)
            return -1;                    /* colon must be followed by more */
        s += 1;
        n -= 1;

        if (s[0] == ':') {                /* "::" */
            if (ellipsis >= 0)
                return -1;                /* multiple "::" */
            ellipsis = i;
            s += 1;
            n -= 1;
            if (n == 0)
                break;                    /* trailing "::" */
        }
    }

    if (n != 0)
        return -1;                        /* trailing garbage */

    if (i < 16) {
        int nfill = 16 - i;
        int j;
        if (ellipsis < 0)
            return -1;                    /* too short and no "::" */
        for (j = i - 1; j >= ellipsis; j--)
            out[j + nfill] = out[j];
        for (j = ellipsis + nfill - 1; j >= ellipsis; j--)
            out[j] = 0;
    } else if (ellipsis >= 0) {
        return -1;                        /* "::" expanded to zero groups */
    }
    return 0;
}

/* Dispatch on the first '.'/':'/'%': '.' first => IPv4, ':' first => IPv6,
 * '%' => a zone-bearing IPv6. */
static int parse_addr(const char *s, size_t n, NetAddr *a)
{
    size_t i;

    memset(a->b, 0, sizeof(a->b));
    for (i = 0; i < n; i++) {
        char c = s[i];
        if (c == '.') {
            a->is4 = 1;
            return parse_ipv4(s, n, a->b);
        }
        if (c == ':') {
            a->is4 = 0;
            return parse_ipv6(s, n, a->b);
        }
        if (c == '%')
            return -1;                    /* zone unsupported / missing addr */
    }
    return -1;                            /* no '.' or ':' => not an IP */
}

/* Parse "addr/bits" into *a and *bits. 0 / -1. */
static int parse_prefix(const char *s, size_t n, NetAddr *a, int *bits)
{
    long slash = -1;
    const char *bs;
    size_t bn, i;
    int v, maxb;

    for (i = n; i-- > 0; ) {              /* last '/' */
        if (s[i] == '/') {
            slash = (long)i;
            break;
        }
    }
    if (slash < 0)
        return -1;
    if (parse_addr(s, (size_t)slash, a) != 0)
        return -1;

    bs = s + slash + 1;
    bn = n - (size_t)slash - 1;
    if (bn == 0 || bn > 3)
        return -1;
    if (bn > 1 && (bs[0] < '1' || bs[0] > '9'))
        return -1;                        /* leading zero / sign / non-digit */
    v = 0;
    for (i = 0; i < bn; i++) {
        if (bs[i] < '0' || bs[i] > '9')
            return -1;
        v = v * 10 + (bs[i] - '0');
    }
    maxb = a->is4 ? 32 : 128;
    if (v > maxb)
        return -1;
    *bits = v;
    return 0;
}

/* ==================================================================== *
 *  4-in-6 handling + canonical formatting (RFC 5952)                    *
 * ==================================================================== */

/* True iff `a` is an IPv4-mapped IPv6 address ("::ffff:0:0/96"). */
static int is_4in6(const NetAddr *a)
{
    int i;
    if (a->is4)
        return 0;
    for (i = 0; i < 10; i++)
        if (a->b[i])
            return 0;
    return a->b[10] == 0xff && a->b[11] == 0xff;
}

/* Return `a` with a 4-in-6 mapped address collapsed to its IPv4 form. */
static NetAddr net_unmap(const NetAddr *a)
{
    NetAddr u = *a;
    if (is_4in6(a)) {
        memset(u.b, 0, sizeof(u.b));
        u.is4 = 1;
        u.b[0] = a->b[12];
        u.b[1] = a->b[13];
        u.b[2] = a->b[14];
        u.b[3] = a->b[15];
    }
    return u;
}

static size_t fmt_dec8(char *dst, unsigned v)   /* v in 0..255 */
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

static size_t fmt_v4(char *dst, const uint8_t oct[4])
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

static size_t fmt_hex16(char *dst, unsigned v)  /* v in 0..0xffff, lowercase */
{
    static const char hx[] = "0123456789abcdef";
    char tmp[4];
    int k = 0, j;
    if (v == 0) {
        dst[0] = '0';
        return 1;
    }
    while (v) {
        tmp[k++] = hx[v & 0xf];
        v >>= 4;
    }
    for (j = 0; j < k; j++)
        dst[j] = tmp[k - 1 - j];
    return (size_t)k;
}

/* Write the RFC 5952 canonical text of `a` into `dst` (>= 48 bytes suffice;
 * longest output is "ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff" = 39 bytes).
 * Returns the byte length. No NUL is written. */
static size_t format_addr(const NetAddr *a, char *dst)
{
    unsigned g[8];
    int best_start = -1, best_len = 0, i;
    size_t o = 0;

    if (a->is4)
        return fmt_v4(dst, a->b);

    if (is_4in6(a)) {
        memcpy(dst, "::ffff:", 7);
        return 7 + fmt_v4(dst + 7, a->b + 12);
    }

    for (i = 0; i < 8; i++)
        g[i] = ((unsigned)a->b[2 * i] << 8) | a->b[2 * i + 1];

    /* longest run of zero groups; strict '>' keeps the leftmost on ties */
    for (i = 0; i < 8; ) {
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
        best_start = -1;                  /* never compress a single group */

    for (i = 0; i < 8; i++) {
        if (i == best_start) {
            dst[o++] = ':';
            dst[o++] = ':';
            i = best_start + best_len;    /* jump past the compressed run */
            if (i >= 8)
                break;
        } else if (i > 0) {
            dst[o++] = ':';
        }
        o += fmt_hex16(dst + o, g[i]);
    }
    return o;
}

/* ==================================================================== *
 *  classification (one predicate per address property)                  *
 * ==================================================================== */

static int net_is_loopback(const NetAddr *a0)
{
    NetAddr a = net_unmap(a0);
    int i;
    if (a.is4)
        return a.b[0] == 127;
    for (i = 0; i < 15; i++)
        if (a.b[i])
            return 0;
    return a.b[15] == 1;                  /* ::1 */
}

static int net_is_private(const NetAddr *a0)
{
    NetAddr a = net_unmap(a0);
    if (a.is4)
        return a.b[0] == 10 ||
               (a.b[0] == 172 && (a.b[1] & 0xf0) == 16) ||
               (a.b[0] == 192 && a.b[1] == 168);
    return (a.b[0] & 0xfe) == 0xfc;       /* fc00::/7 */
}

static int net_is_multicast(const NetAddr *a0)
{
    NetAddr a = net_unmap(a0);
    if (a.is4)
        return (a.b[0] & 0xf0) == 0xe0;   /* 224.0.0.0/4 */
    return a.b[0] == 0xff;                /* ff00::/8 */
}

static int net_is_ll_unicast(const NetAddr *a0)
{
    NetAddr a = net_unmap(a0);
    if (a.is4)
        return a.b[0] == 169 && a.b[1] == 254;         /* 169.254.0.0/16 */
    return a.b[0] == 0xfe && (a.b[1] & 0xc0) == 0x80;  /* fe80::/10 */
}

static int net_is_ll_multicast(const NetAddr *a0)
{
    NetAddr a = net_unmap(a0);
    if (a.is4)
        return a.b[0] == 224 && a.b[1] == 0 && a.b[2] == 0; /* 224.0.0.0/24 */
    return a.b[0] == 0xff && (a.b[1] & 0x0f) == 0x02;       /* ff02::/16 */
}

static int net_is_unspecified(const NetAddr *a0)
{
    int i;               /* NB: deliberately NOT unmapped, unlike the above */
    if (a0->is4)
        return a0->b[0] == 0 && a0->b[1] == 0 &&
               a0->b[2] == 0 && a0->b[3] == 0;
    for (i = 0; i < 16; i++)
        if (a0->b[i])
            return 0;
    return 1;
}

static int net_is_global_unicast(const NetAddr *a0)
{
    NetAddr a = net_unmap(a0);
    int i;
    if (a.is4) {
        if (a.b[0] == 0 && a.b[1] == 0 && a.b[2] == 0 && a.b[3] == 0)
            return 0;                     /* 0.0.0.0 */
        if (a.b[0] == 255 && a.b[1] == 255 && a.b[2] == 255 && a.b[3] == 255)
            return 0;                     /* 255.255.255.255 */
    } else {
        for (i = 0; i < 16; i++)
            if (a.b[i])
                break;
        if (i == 16)
            return 0;                     /* :: (IPv6 unspecified) */
    }
    return !net_is_loopback(&a) && !net_is_multicast(&a) &&
           !net_is_ll_unicast(&a);
}

/* ==================================================================== *
 *  prefix operations                                                    *
 * ==================================================================== */

static void mask_addr(NetAddr *a, int bits)
{
    int nbytes = a->is4 ? 4 : 16;
    int full = bits / 8, rem = bits % 8, i;
    if (rem) {
        a->b[full] &= (uint8_t)(0xff << (8 - rem));
        full++;
    }
    for (i = full; i < nbytes; i++)
        a->b[i] = 0;
}

static int prefix_contains(const NetAddr *p, int bits, const NetAddr *ip)
{
    int full = bits / 8, rem = bits % 8, i;
    if (p->is4 != ip->is4)
        return 0;                         /* families must match exactly */
    for (i = 0; i < full; i++)
        if (p->b[i] != ip->b[i])
            return 0;
    if (rem) {
        uint8_t m = (uint8_t)(0xff << (8 - rem));
        if ((p->b[full] & m) != (ip->b[full] & m))
            return 0;
    }
    return 1;
}

static int net_compare(const NetAddr *x, const NetAddr *y)
{
    int c, nbytes;
    if (x->is4 != y->is4)
        return x->is4 ? -1 : 1;           /* v4 (32 bits) sorts before v6 */
    nbytes = x->is4 ? 4 : 16;
    c = memcmp(x->b, y->b, (size_t)nbytes);
    return (c > 0) - (c < 0);
}

/* ==================================================================== *
 *  JS boundary                                                          *
 * ==================================================================== */

/* Fresh Uint8Array copying `data[0..len)` (never aliases native memory). */
static JSValue netip_u8(JSContext *ctx, const uint8_t *data, size_t len)
{
    static const uint8_t zero_stub = 0;
    JSValue ab, out;
    JSValueConst ta_args[3];

    ab = JS_NewArrayBufferCopy(ctx, len ? data : &zero_stub, len);
    if (JS_IsException(ab))
        return ab;
    ta_args[0] = ab;
    ta_args[1] = JS_UNDEFINED;
    ta_args[2] = JS_UNDEFINED;
    out = JS_NewTypedArray(ctx, 3, ta_args, JS_TYPED_ARRAY_UINT8);
    JS_FreeValue(ctx, ab);
    return out;
}

/* Coerce argv[0] to an owned C string, or throw TypeError. */
static const char *netip_arg_str(JSContext *ctx, int argc, JSValueConst *argv,
                                 size_t *plen)
{
    if (argc < 1 || !JS_IsString(argv[0])) {
        JS_ThrowTypeError(ctx, "dyna:net: argument must be a string");
        return NULL;
    }
    return JS_ToCStringLen(ctx, plen, argv[0]);
}

/* parseAddr(str) -> { is4, is6, bytes, string } | throw */
static JSValue dyn_netip_parse_addr(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    const char *s;
    size_t n;
    NetAddr a;
    JSValue obj, u8, str;
    char buf[48];
    size_t blen;
    int nbytes, err;

    (void)this_val;
    s = netip_arg_str(ctx, argc, argv, &n);
    if (!s)
        return JS_EXCEPTION;
    if (parse_addr(s, n, &a) != 0) {
        JS_FreeCString(ctx, s);
        return JS_ThrowTypeError(ctx, "dyna:net: invalid IP address");
    }
    JS_FreeCString(ctx, s);

    obj = JS_NewObject(ctx);
    if (JS_IsException(obj))
        return obj;
    nbytes = a.is4 ? 4 : 16;
    u8 = netip_u8(ctx, a.b, (size_t)nbytes);
    if (JS_IsException(u8)) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    blen = format_addr(&a, buf);
    str = JS_NewStringLen(ctx, buf, blen);
    if (JS_IsException(str)) {
        JS_FreeValue(ctx, u8);
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    /* Call every define unconditionally so the two heap values (u8, str) are
     * always consumed (JS_DefinePropertyValueStr frees its value even on
     * failure); the JS_NewBool values are immediates and cannot leak. */
    err  = JS_DefinePropertyValueStr(ctx, obj, "is4",
                                     JS_NewBool(ctx, a.is4), JS_PROP_C_W_E) < 0;
    err |= JS_DefinePropertyValueStr(ctx, obj, "is6",
                                     JS_NewBool(ctx, !a.is4), JS_PROP_C_W_E) < 0;
    err |= JS_DefinePropertyValueStr(ctx, obj, "bytes", u8,
                                     JS_PROP_C_W_E) < 0;
    err |= JS_DefinePropertyValueStr(ctx, obj, "string", str,
                                     JS_PROP_C_W_E) < 0;
    if (err) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    return obj;
}

/* parsePrefix(str) -> { addr, bits } | throw */
static JSValue dyn_netip_parse_prefix(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    const char *s;
    size_t n;
    NetAddr a;
    int bits, err;
    JSValue obj, str;
    char buf[48];
    size_t blen;

    (void)this_val;
    s = netip_arg_str(ctx, argc, argv, &n);
    if (!s)
        return JS_EXCEPTION;
    if (parse_prefix(s, n, &a, &bits) != 0) {
        JS_FreeCString(ctx, s);
        return JS_ThrowTypeError(ctx, "dyna:net: invalid CIDR prefix");
    }
    JS_FreeCString(ctx, s);

    obj = JS_NewObject(ctx);
    if (JS_IsException(obj))
        return obj;
    blen = format_addr(&a, buf);
    str = JS_NewStringLen(ctx, buf, blen);
    if (JS_IsException(str)) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    err  = JS_DefinePropertyValueStr(ctx, obj, "addr", str, JS_PROP_C_W_E) < 0;
    err |= JS_DefinePropertyValueStr(ctx, obj, "bits",
                                     JS_NewInt32(ctx, bits), JS_PROP_C_W_E) < 0;
    if (err) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    return obj;
}

/* contains(prefixStr, addrStr) -> bool | throw */
static JSValue dyn_netip_contains(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    const char *ps, *as;
    size_t pn, an;
    NetAddr prefix, ip;
    int bits, ok;

    (void)this_val;
    if (argc < 2 || !JS_IsString(argv[0]) || !JS_IsString(argv[1]))
        return JS_ThrowTypeError(ctx,
            "dyna:net: contains(prefix, addr) expects two strings");
    ps = JS_ToCStringLen(ctx, &pn, argv[0]);
    if (!ps)
        return JS_EXCEPTION;
    as = JS_ToCStringLen(ctx, &an, argv[1]);
    if (!as) {
        JS_FreeCString(ctx, ps);
        return JS_EXCEPTION;
    }
    ok = parse_prefix(ps, pn, &prefix, &bits) == 0 &&
         parse_addr(as, an, &ip) == 0;
    JS_FreeCString(ctx, ps);
    JS_FreeCString(ctx, as);
    if (!ok)
        return JS_ThrowTypeError(ctx,
            "dyna:net: invalid CIDR prefix or address");
    return JS_NewBool(ctx, prefix_contains(&prefix, bits, &ip));
}

/* masked(prefixStr) -> canonical network address string | throw */
static JSValue dyn_netip_masked(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    const char *s;
    size_t n, blen;
    NetAddr a;
    int bits;
    char buf[48];

    (void)this_val;
    s = netip_arg_str(ctx, argc, argv, &n);
    if (!s)
        return JS_EXCEPTION;
    if (parse_prefix(s, n, &a, &bits) != 0) {
        JS_FreeCString(ctx, s);
        return JS_ThrowTypeError(ctx, "dyna:net: invalid CIDR prefix");
    }
    JS_FreeCString(ctx, s);
    mask_addr(&a, bits);
    blen = format_addr(&a, buf);
    return JS_NewStringLen(ctx, buf, blen);
}

/* canonical(str) -> RFC 5952 canonical text | throw */
static JSValue dyn_netip_canonical(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    const char *s;
    size_t n, blen;
    NetAddr a;
    char buf[48];

    (void)this_val;
    s = netip_arg_str(ctx, argc, argv, &n);
    if (!s)
        return JS_EXCEPTION;
    if (parse_addr(s, n, &a) != 0) {
        JS_FreeCString(ctx, s);
        return JS_ThrowTypeError(ctx, "dyna:net: invalid IP address");
    }
    JS_FreeCString(ctx, s);
    blen = format_addr(&a, buf);
    return JS_NewStringLen(ctx, buf, blen);
}

/* isValid(str) -> bool (never throws) */
static JSValue dyn_netip_is_valid(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    const char *s;
    size_t n;
    NetAddr a;
    int ok;

    (void)this_val;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_NewBool(ctx, 0);
    s = JS_ToCStringLen(ctx, &n, argv[0]);
    if (!s) {
        JS_FreeValue(ctx, JS_GetException(ctx));   /* swallow OOM; report false */
        return JS_NewBool(ctx, 0);
    }
    ok = parse_addr(s, n, &a) == 0;
    JS_FreeCString(ctx, s);
    return JS_NewBool(ctx, ok);
}

/* compareAddr(a, b) -> -1 | 0 | 1 | throw */
static JSValue dyn_netip_compare(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    const char *as, *bs;
    size_t an, bn;
    NetAddr x, y;
    int ok;

    (void)this_val;
    if (argc < 2 || !JS_IsString(argv[0]) || !JS_IsString(argv[1]))
        return JS_ThrowTypeError(ctx,
            "dyna:net: compareAddr(a, b) expects two strings");
    as = JS_ToCStringLen(ctx, &an, argv[0]);
    if (!as)
        return JS_EXCEPTION;
    bs = JS_ToCStringLen(ctx, &bn, argv[1]);
    if (!bs) {
        JS_FreeCString(ctx, as);
        return JS_EXCEPTION;
    }
    ok = parse_addr(as, an, &x) == 0 && parse_addr(bs, bn, &y) == 0;
    JS_FreeCString(ctx, as);
    JS_FreeCString(ctx, bs);
    if (!ok)
        return JS_ThrowTypeError(ctx, "dyna:net: invalid IP address");
    return JS_NewInt32(ctx, net_compare(&x, &y));
}

enum {
    CLS_LOOPBACK, CLS_PRIVATE, CLS_MULTICAST, CLS_UNSPECIFIED,
    CLS_LL_UNICAST, CLS_GLOBAL_UNICAST, CLS_LL_MULTICAST
};

/* is<Class>(addrStr) -> bool | throw. magic selects the predicate. */
static JSValue dyn_netip_classify(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv, int magic)
{
    const char *s;
    size_t n;
    NetAddr a;
    int r;

    (void)this_val;
    s = netip_arg_str(ctx, argc, argv, &n);
    if (!s)
        return JS_EXCEPTION;
    if (parse_addr(s, n, &a) != 0) {
        JS_FreeCString(ctx, s);
        return JS_ThrowTypeError(ctx, "dyna:net: invalid IP address");
    }
    JS_FreeCString(ctx, s);

    switch (magic) {
    case CLS_LOOPBACK:       r = net_is_loopback(&a); break;
    case CLS_PRIVATE:        r = net_is_private(&a); break;
    case CLS_MULTICAST:      r = net_is_multicast(&a); break;
    case CLS_UNSPECIFIED:    r = net_is_unspecified(&a); break;
    case CLS_LL_UNICAST:     r = net_is_ll_unicast(&a); break;
    case CLS_GLOBAL_UNICAST: r = net_is_global_unicast(&a); break;
    case CLS_LL_MULTICAST:   r = net_is_ll_multicast(&a); break;
    default:                 r = 0; break;
    }
    return JS_NewBool(ctx, r);
}

/* ---------- module registration ---------- */

/* ==================================================================== *
 *  class Prefix -- a COMPILED CIDR: parse once, test many addresses     *
 * ==================================================================== *
 *
 * The free contains(prefix, addr) runs parse_prefix() on the CIDR string every
 * call. An ACL, a firewall rule and a rate limiter all ask "is this address in
 * this prefix?" once per request against a prefix that never changes, so that
 * parse is pure repeated work -- the same shape as recompiling a RegExp per
 * match.
 *
 * new Prefix("10.0.0.0/8") parses and masks once; .contains(addr) is then a
 * masked compare. Plain GC object, read-only compiled state, freely reusable,
 * no busy flag.
 */

typedef struct {
    NetAddr net;    /* already masked to `bits` at construction */
    int bits;
} dyn_prefix_t;

static JSClassID dyn_prefix_class_id;

static void dyn_prefix_dispose(void *native) { free(native); }

static void dyn_prefix_finalizer(JSRuntime *rt, JSValue val)
{
    (void)rt;
    dyn_prefix_dispose(JS_GetOpaque(val, dyn_prefix_class_id));
}

static const JSClassDef dyn_prefix_class = {
    "Prefix",
    .finalizer = dyn_prefix_finalizer,
};

static JSValue dyn_prefix_ctor(JSContext *ctx, JSValueConst new_target,
                               int argc, JSValueConst *argv)
{
    const char *s;
    size_t n;
    NetAddr a;
    int bits;
    dyn_prefix_t *p;

    (void)new_target;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "Prefix(cidr) requires a CIDR string");
    s = JS_ToCStringLen(ctx, &n, argv[0]);
    if (!s)
        return JS_EXCEPTION;
    if (parse_prefix(s, n, &a, &bits) != 0) {
        JS_FreeCString(ctx, s);
        return JS_ThrowTypeError(ctx, "dyna:net: invalid CIDR prefix");
    }
    JS_FreeCString(ctx, s);

    p = (dyn_prefix_t *)malloc(sizeof(*p));
    if (!p)
        return JS_ThrowOutOfMemory(ctx);
    /* Mask at construction: every later contains() then compares against an
     * already-canonical network address. */
    mask_addr(&a, bits);
    p->net = a;
    p->bits = bits;
    return dyn_plain_wrap(ctx, dyn_prefix_class_id, p, dyn_prefix_dispose);
}

/* .contains(addr) -> boolean. An unparseable address is `false`, matching the
 * free contains()'s "not in this prefix" answer for anything that is not an
 * address in the same family. */
static JSValue dyn_prefix_contains(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    const char *s;
    size_t n;
    NetAddr ip;
    dyn_prefix_t *p;
    int ok;

    (void)argc;
    s = JS_ToCStringLen(ctx, &n, argv[0]);   /* coerce before resolving */
    if (!s)
        return JS_EXCEPTION;
    p = (dyn_prefix_t *)dyn_plain_get(ctx, this_val, dyn_prefix_class_id);
    if (!p) {
        JS_FreeCString(ctx, s);
        return JS_EXCEPTION;
    }
    ok = parse_addr(s, n, &ip) == 0 && prefix_contains(&p->net, p->bits, &ip);
    JS_FreeCString(ctx, s);
    return JS_NewBool(ctx, ok);
}

/* .overlaps(other) -> boolean. Two prefixes overlap iff the shorter one
 * contains the other's network address -- containment is decided entirely by
 * the leading min(bits) bits. */
static JSValue dyn_prefix_overlaps(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    dyn_prefix_t *a, *b;
    int nbits;

    (void)argc;
    b = (dyn_prefix_t *)JS_GetOpaque2(ctx, argv[0], dyn_prefix_class_id);
    if (!b)
        return JS_EXCEPTION;
    a = (dyn_prefix_t *)dyn_plain_get(ctx, this_val, dyn_prefix_class_id);
    if (!a)
        return JS_EXCEPTION;
    if (a->net.is4 != b->net.is4)
        return JS_NewBool(ctx, 0);   /* different families never overlap */
    nbits = a->bits < b->bits ? a->bits : b->bits;
    return JS_NewBool(ctx, prefix_contains(&a->net, nbits, &b->net));
}

static JSValue dyn_prefix_get_masked(JSContext *ctx, JSValueConst this_val)
{
    dyn_prefix_t *p = (dyn_prefix_t *)dyn_plain_get(ctx, this_val,
                                                    dyn_prefix_class_id);
    char buf[48];
    size_t blen;
    if (!p)
        return JS_EXCEPTION;
    blen = format_addr(&p->net, buf);
    return JS_NewStringLen(ctx, buf, blen);
}

static JSValue dyn_prefix_get_bits(JSContext *ctx, JSValueConst this_val)
{
    dyn_prefix_t *p = (dyn_prefix_t *)dyn_plain_get(ctx, this_val,
                                                    dyn_prefix_class_id);
    if (!p)
        return JS_EXCEPTION;
    return JS_NewInt32(ctx, p->bits);
}

static JSValue dyn_prefix_get_is4(JSContext *ctx, JSValueConst this_val)
{
    dyn_prefix_t *p = (dyn_prefix_t *)dyn_plain_get(ctx, this_val,
                                                    dyn_prefix_class_id);
    if (!p)
        return JS_EXCEPTION;
    return JS_NewBool(ctx, p->net.is4);
}

static const JSCFunctionListEntry dyn_prefix_proto[] = {
    JS_CFUNC_DEF("contains", 1, dyn_prefix_contains),
    JS_CFUNC_DEF("overlaps", 1, dyn_prefix_overlaps),
    JS_CGETSET_DEF("masked", dyn_prefix_get_masked, NULL),
    JS_CGETSET_DEF("bits", dyn_prefix_get_bits, NULL),
    JS_CGETSET_DEF("isIPv4", dyn_prefix_get_is4, NULL),
};

static const JSCFunctionListEntry dyn_netip_funcs[] = {
    JS_CFUNC_DEF("parseAddr", 1, dyn_netip_parse_addr),
    JS_CFUNC_DEF("parsePrefix", 1, dyn_netip_parse_prefix),
    JS_CFUNC_DEF("contains", 2, dyn_netip_contains),
    JS_CFUNC_DEF("masked", 1, dyn_netip_masked),
    JS_CFUNC_DEF("canonical", 1, dyn_netip_canonical),
    JS_CFUNC_DEF("isValid", 1, dyn_netip_is_valid),
    JS_CFUNC_DEF("compareAddr", 2, dyn_netip_compare),
    JS_CFUNC_MAGIC_DEF("isLoopback", 1, dyn_netip_classify, CLS_LOOPBACK),
    JS_CFUNC_MAGIC_DEF("isPrivate", 1, dyn_netip_classify, CLS_PRIVATE),
    JS_CFUNC_MAGIC_DEF("isMulticast", 1, dyn_netip_classify, CLS_MULTICAST),
    JS_CFUNC_MAGIC_DEF("isUnspecified", 1, dyn_netip_classify, CLS_UNSPECIFIED),
    JS_CFUNC_MAGIC_DEF("isLinkLocalUnicast", 1, dyn_netip_classify,
                       CLS_LL_UNICAST),
    JS_CFUNC_MAGIC_DEF("isGlobalUnicast", 1, dyn_netip_classify,
                       CLS_GLOBAL_UNICAST),
    JS_CFUNC_MAGIC_DEF("isLinkLocalMulticast", 1, dyn_netip_classify,
                       CLS_LL_MULTICAST),
};

int dyn_netip_register(JSContext *ctx, JSModuleDef *m)
{
    if (dyn_register_plain_class(ctx, m, &dyn_prefix_class_id, &dyn_prefix_class,
                                 dyn_prefix_proto, countof(dyn_prefix_proto),
                                 dyn_prefix_ctor, "Prefix") < 0)
        return -1;
    return JS_SetModuleExportList(ctx, m, dyn_netip_funcs,
                                  countof(dyn_netip_funcs));
}

int dyn_netip_add_exports(JSContext *ctx, JSModuleDef *m)
{
    JS_AddModuleExport(ctx, m, "Prefix");
    return JS_AddModuleExportList(ctx, m, dyn_netip_funcs,
                                  countof(dyn_netip_funcs));
}

#endif /* CONFIG_NATIVE_MODULES && CONFIG_NATIVE_MODULE_NET */
