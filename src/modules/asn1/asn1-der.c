#include "asn1-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_ASN1)

int der_ident(ab_t* b, int cls, int constructed, uint32_t tag)
{
    uint8_t h[8];
    uint8_t t[8];
    uint32_t v;
    int i, n = 0;

    if (tag < 31) {
        h[0] = (uint8_t)((cls << 6) | (constructed << 5) | (int)tag);
        return ab_write(b, h, 1);
    }
    h[0] = (uint8_t)((cls << 6) | (constructed << 5) | 0x1F);
    v = tag;
    do {
        t[n++] = (uint8_t)(v & 0x7F);
        v >>= 7;
    } while (v);
    for (i = n - 1; i >= 0; i--)
        h[1 + (n - 1 - i)] = (uint8_t)(t[i] | (i ? 0x80 : 0));
    return ab_write(b, h, 1 + (size_t)n);
}

int der_length(ab_t* b, size_t n)
{
    uint8_t h[9];
    uint8_t t[8];
    size_t v = n;
    int k = 0, i;

    if (n < 128) {
        h[0] = (uint8_t)n;
        return ab_write(b, h, 1);
    }
    while (v) {
        t[k++] = (uint8_t)(v & 0xFF);
        v >>= 8;
    }
    h[0] = (uint8_t)(0x80 | k);
    for (i = 0; i < k; i++)
        h[1 + i] = t[k - 1 - i];
    return ab_write(b, h, 1 + (size_t)k);
}

int der_int_bytes(int64_t v, uint8_t* out)
{
    int n, i;

    if (v >= 0) {
        for (n = 1; n < 8; n++)
            if ((uint64_t)v <= (UINT64_C(1) << (8 * n - 1)) - 1)
                break;
    } else {
        for (n = 1; n < 8; n++)
            if (v >= -((int64_t)UINT64_C(1) << (8 * n - 1)))
                break;
    }
    for (i = 0; i < n; i++)
        out[i] = (uint8_t)((uint64_t)v >> (8 * (n - 1 - i)));
    return n;
}

int der_int_from_decimal(JSContext* ctx, const char* s,
    uint8_t* content, int* clen)
{
    uint8_t mag[ASN1_MAX_BIGINT_BYTES];
    uint8_t be[ASN1_MAX_BIGINT_BYTES];
    int neg = 0;
    size_t n = 0, i, m;

    if (*s == '-') {
        neg = 1;
        s++;
    }
    for (; *s; s++) {
        unsigned carry, d = (unsigned)(*s - '0');
        if (d > 9) {
            JS_ThrowTypeError(ctx, "ASN.1: INTEGER BigInt did not stringify "
                                   "to decimal digits");
            return -1;
        }
        carry = d;
        for (i = 0; i < n; i++) {
            unsigned x = (unsigned)mag[i] * 10 + carry;
            mag[i] = (uint8_t)(x & 0xFF);
            carry = x >> 8;
        }
        while (carry) {
            if (n >= ASN1_MAX_BIGINT_BYTES) {
                JS_ThrowRangeError(ctx, "ASN.1: INTEGER BigInt needs more "
                                        "than the %d-byte content cap",
                    ASN1_MAX_BIGINT_BYTES);
                return -1;
            }
            mag[n++] = (uint8_t)(carry & 0xFF);
            carry >>= 8;
        }
    }
    if (n == 0) {
        content[0] = 0x00;
        *clen = 1;
        return 0;
    }
    for (i = 0; i < n; i++)
        be[i] = mag[n - 1 - i];
    if (!neg) {
        m = n + ((be[0] & 0x80) ? 1 : 0);
        if (m > ASN1_MAX_BIGINT_BYTES) {
            JS_ThrowRangeError(ctx, "ASN.1: INTEGER BigInt needs more than the "
                                    "%d-byte content cap",
                ASN1_MAX_BIGINT_BYTES);
            return -1;
        }
        if (m != n)
            content[0] = 0x00;
        memcpy(content + (m - n), be, n);
    } else {
        int fits = (be[0] < 0x80);
        unsigned carry = 1;
        if (!fits && be[0] == 0x80) {
            fits = 1;
            for (i = 1; i < n; i++)
                if (be[i]) {
                    fits = 0;
                    break;
                }
        }
        m = n + (fits ? 0 : 1);
        if (m > ASN1_MAX_BIGINT_BYTES) {
            JS_ThrowRangeError(ctx, "ASN.1: INTEGER BigInt needs more than the "
                                    "%d-byte content cap",
                ASN1_MAX_BIGINT_BYTES);
            return -1;
        }
        memset(content, 0x00, m);
        memcpy(content + (m - n), be, n);
        for (i = 0; i < m; i++)
            content[i] = (uint8_t)~content[i];
        for (i = m; i > 0 && carry; i--) {
            unsigned x = (unsigned)content[i - 1] + carry;
            content[i - 1] = (uint8_t)(x & 0xFF);
            carry = x >> 8;
        }
        if (!(content[0] & 0x80)) {
            JS_ThrowInternalError(ctx, "ASN.1: INTEGER two's complement is "
                                       "not minimal");
            return -1;
        }
    }
    *clen = (int)m;
    return 0;
}

int asn1_der_set_cmp(const void* a, const void* b)
{
    const ab_t *x = (const ab_t*)a, *y = (const ab_t*)b;
    size_t n = x->n < y->n ? x->n : y->n;
    int c = n ? memcmp(x->p, y->p, n) : 0;
    if (c)
        return c;
    return x->n < y->n ? -1 : (x->n > y->n ? 1 : 0);
}

#endif
