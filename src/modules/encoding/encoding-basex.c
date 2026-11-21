#include "encoding-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_ENCODING)

#define BX_MAX_INPUT 4096u

static const char BX_B58[] = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

static int bx_index_table(JSContext* ctx, const char* alpha, size_t n,
    int idx[256])
{
    size_t i;

    if (n < 2 || n > 255) {
        JS_ThrowRangeError(ctx, "BaseX: the alphabet is 2 to 255 characters");
        return -1;
    }
    memset(idx, -1, sizeof(int) * 256);
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)alpha[i];
        if (idx[c] >= 0) {
            JS_ThrowRangeError(ctx, "BaseX: '%c' appears twice in the alphabet, "
                                    "which would corrupt decoding",
                c);
            return -1;
        }
        idx[c] = (int)i;
    }
    return 0;
}

static uint32_t bx_limb_pow(uint32_t base, uint32_t* pk)
{
    uint32_t p = 1, k = 0;
    while ((uint64_t)p * base <= 0xFFFFFFFFu) {
        p *= base;
        k++;
    }
    *pk = k;
    return p;
}

static int bx_encode(const uint8_t* src, size_t n, const char* alpha,
    uint32_t base, uint8_t** out, size_t* out_len)
{
    size_t zeros = 0, len = 0, i, j, m, nd, o;
    uint32_t k, p = bx_limb_pow(base, &k), *limb;
    uint8_t* res;

    while (zeros < n && src[zeros] == 0)
        zeros++;
    m = n - zeros;
    limb = (uint32_t*)malloc((m / 3 + 2) * sizeof(uint32_t));
    if (!limb)
        return -1;
    i = zeros;
    while (i < n) {
        size_t g = (i == zeros && m % 3) ? m % 3 : 3;
        uint64_t carry = 0, mul = (uint64_t)1 << (8 * g);
        for (j = 0; j < g; j++)
            carry = (carry << 8) | src[i + j];
        i += g;
        for (j = 0; j < len; j++) {
            uint64_t v = (uint64_t)limb[j] * mul + carry;
            limb[j] = (uint32_t)(v % p);
            carry = v / p;
        }
        while (carry) {
            limb[len++] = (uint32_t)(carry % p);
            carry /= p;
        }
    }
    nd = 0;
    if (len) {
        uint32_t top = limb[len - 1];
        while (top) {
            nd++;
            top /= base;
        }
        nd += (len - 1) * k;
    }
    res = (uint8_t*)malloc(zeros + nd + 1);
    if (!res) {
        free(limb);
        return -1;
    }
    for (i = 0; i < zeros; i++)
        res[i] = (uint8_t)alpha[0];
    o = zeros + nd;
    for (j = 0; j < len; j++) {
        uint32_t v = limb[j];
        uint32_t d = j + 1 < len ? k : (uint32_t)(nd - (len - 1) * k);
        while (d--) {
            res[--o] = (uint8_t)alpha[v % base];
            v /= base;
        }
    }
    res[zeros + nd] = 0;
    free(limb);
    *out = res;
    *out_len = zeros + nd;
    return 0;
}

static int bx_decode(const char* src, size_t n, const int idx[256],
    uint32_t base, uint8_t** out, size_t* out_len)
{
    size_t zeros = 0, len = 0, i, j, m, nb, o;
    uint32_t k, *limb;
    uint8_t* res;
    char zero_ch = 0;

    bx_limb_pow(base, &k);
    for (i = 0; i < 256; i++)
        if (idx[i] == 0) {
            zero_ch = (char)i;
            break;
        }
    while (zeros < n && src[zeros] == zero_ch)
        zeros++;
    m = n - zeros;
    limb = (uint32_t*)malloc((m / 4 + 2) * sizeof(uint32_t));
    if (!limb)
        return -1;
    i = zeros;
    while (i < n) {
        size_t g = (i == zeros && m % k) ? m % k : k;
        uint64_t carry = 0, mul = 1;
        for (j = 0; j < g; j++) {
            int d = idx[(unsigned char)src[i + j]];
            if (d < 0) {
                free(limb);
                return -2;
            }
            carry = carry * base + (uint32_t)d;
            mul *= base;
        }
        i += g;
        for (j = 0; j < len; j++) {
            uint64_t v = (uint64_t)limb[j] * mul + carry;
            limb[j] = (uint32_t)v;
            carry = v >> 32;
        }
        while (carry) {
            limb[len++] = (uint32_t)carry;
            carry >>= 32;
        }
    }
    nb = 0;
    if (len) {
        uint32_t top = limb[len - 1];
        while (top) {
            nb++;
            top >>= 8;
        }
        nb += (len - 1) * 4;
    }
    res = (uint8_t*)malloc(zeros + nb + 1);
    if (!res) {
        free(limb);
        return -1;
    }
    memset(res, 0, zeros);
    o = zeros + nb;
    for (j = 0; j < len; j++) {
        uint32_t v = limb[j];
        size_t d = j + 1 < len ? 4 : nb - (len - 1) * 4;
        while (d--) {
            res[--o] = (uint8_t)v;
            v >>= 8;
        }
    }
    free(limb);
    *out = res;
    *out_len = zeros + nb;
    return 0;
}

JSValue dyn_b58(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    int idx[256];
    uint8_t* out = NULL;
    size_t out_len = 0;
    JSValue result;

    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "Base58: an argument is required");
    if (bx_index_table(ctx, BX_B58, 58, idx) < 0)
        return JS_EXCEPTION;
    if (magic == 0 || magic == 2) {
        const uint8_t* data;
        const char* owned;
        size_t n;
        uint8_t* payload = NULL;
        int rc;
        if (dyn_enc_bytes(ctx, argv[0], &data, &n, &owned) < 0)
            return JS_EXCEPTION;
        if (n > BX_MAX_INPUT) {
            if (owned)
                JS_FreeCString(ctx, owned);
            return JS_ThrowRangeError(ctx, "Base58: input exceeds %u bytes -- "
                                           "this is a division codec and its cost is quadratic",
                BX_MAX_INPUT);
        }
        if (magic == 2) {
            uint8_t h1[32], h2[32];
            payload = (uint8_t*)malloc(n + 4);
            if (!payload) {
                if (owned)
                    JS_FreeCString(ctx, owned);
                return JS_ThrowOutOfMemory(ctx);
            }
            memcpy(payload, data, n);
            dyn_sha256(data, n, h1);
            dyn_sha256(h1, 32, h2);
            memcpy(payload + n, h2, 4);
            rc = bx_encode(payload, n + 4, BX_B58, 58, &out, &out_len);
            free(payload);
        } else {
            rc = bx_encode(data, n, BX_B58, 58, &out, &out_len);
        }
        if (owned)
            JS_FreeCString(ctx, owned);
        if (rc < 0)
            return JS_ThrowOutOfMemory(ctx);
        result = JS_NewStringLen(ctx, (const char*)out, out_len);
        free(out);
        return result;
    }
    {
        const char* s;
        size_t n;
        int rc;
        if (!JS_IsString(argv[0]))
            return JS_ThrowTypeError(ctx, "Base58Decode(text): text must be a string");
        s = JS_ToCStringLen(ctx, &n, argv[0]);
        if (!s)
            return JS_EXCEPTION;
        if (n > BX_MAX_INPUT) {
            JS_FreeCString(ctx, s);
            return JS_ThrowRangeError(ctx, "Base58Decode: input exceeds %u bytes",
                BX_MAX_INPUT);
        }
        rc = bx_decode(s, n, idx, 58, &out, &out_len);
        JS_FreeCString(ctx, s);
        if (rc == -2)
            return JS_ThrowSyntaxError(ctx, "Base58Decode: not a base58 string");
        if (rc < 0)
            return JS_ThrowOutOfMemory(ctx);
        if (magic == 3) {
            uint8_t h1[32], h2[32];
            if (out_len < 4) {
                free(out);
                return JS_ThrowSyntaxError(ctx,
                    "Base58CheckDecode: too short to carry a checksum");
            }
            dyn_sha256(out, out_len - 4, h1);
            dyn_sha256(h1, 32, h2);
            if (memcmp(h2, out + out_len - 4, 4) != 0) {
                free(out);
                return JS_ThrowSyntaxError(ctx,
                    "Base58CheckDecode: the checksum does not match -- a single "
                    "mistyped character is exactly what it exists to catch");
            }
            out_len -= 4;
        }
        result = dyn_nat_u8_copy(ctx, out, out_len);
        free(out);
        return result;
    }
}

JSValue dyn_b58_decode_into(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    const char* s;
    size_t n, on;
    int idx[256];
    uint8_t *dec = NULL, *out;
    size_t dec_len = 0;
    int rc;
    (void)this_val;
    (void)argc;

    if (argc < 2 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx,
            "Base58DecodeInto(text, out): a string and a byte view are required");
    s = JS_ToCStringLen(ctx, &n, argv[0]);
    if (!s)
        return JS_EXCEPTION;
    if (dyn_enc_view_w(ctx, argv[1], &out, &on)) {
        JS_FreeCString(ctx, s);
        return JS_EXCEPTION;
    }
    if (on < n) {
        JS_FreeCString(ctx, s);
        return JS_ThrowRangeError(ctx,
            "Base58DecodeInto: output needs at least %lu bytes, got %lu",
            (unsigned long)n, (unsigned long)on);
    }
    if (n > BX_MAX_INPUT) {
        JS_FreeCString(ctx, s);
        return JS_ThrowRangeError(ctx, "Base58DecodeInto: input exceeds %u bytes",
            BX_MAX_INPUT);
    }
    if (bx_index_table(ctx, BX_B58, 58, idx) < 0) {
        JS_FreeCString(ctx, s);
        return JS_EXCEPTION;
    }
    rc = bx_decode(s, n, idx, 58, &dec, &dec_len);
    JS_FreeCString(ctx, s);
    if (rc == -2)
        return JS_ThrowSyntaxError(ctx, "Base58DecodeInto: not a base58 string");
    if (rc < 0)
        return JS_ThrowOutOfMemory(ctx);
    if (magic) {
        uint8_t h1[32], h2[32];
        if (dec_len < 4) {
            free(dec);
            return JS_ThrowSyntaxError(ctx,
                "Base58CheckDecodeInto: too short to carry a checksum");
        }
        dyn_sha256(dec, dec_len - 4, h1);
        dyn_sha256(h1, 32, h2);
        if (memcmp(h2, dec + dec_len - 4, 4) != 0) {
            free(dec);
            return JS_ThrowSyntaxError(ctx,
                "Base58CheckDecodeInto: the checksum does not match -- a single "
                "mistyped character is exactly what it exists to catch");
        }
        dec_len -= 4;
    }
    memcpy(out, dec, dec_len);
    free(dec);
    return JS_NewInt64(ctx, (int64_t)dec_len);
}

static int dyn_b58e_fold(uint8_t* d, size_t* dlen, size_t dcap, uint8_t b)
{
    uint32_t carry = b;
    size_t j, len = *dlen;

    for (j = 0; j < len || carry; j++) {
        uint32_t v = (j < len ? (uint32_t)d[j] : 0) * 256 + carry;
        if (j >= dcap)
            return -1;
        d[j] = (uint8_t)(v % 58);
        carry = v / 58;
    }
    if (j > len)
        *dlen = j;
    return 0;
}

JSValue dyn_b58_encode_into(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    const uint8_t* data;
    size_t n, on, need, zeros = 0, len = 0, i, payload;
    const char* owned;
    uint8_t* out;
    uint8_t cks[4] = { 0 };
    (void)this_val;
    (void)argc;

    if (argc < 2)
        return JS_ThrowTypeError(ctx,
            "Base58EncodeInto(data, out): a byte input and a byte view are required");
    if (dyn_enc_bytes(ctx, argv[0], &data, &n, &owned))
        return JS_EXCEPTION;
    if (n > BX_MAX_INPUT) {
        if (owned)
            JS_FreeCString(ctx, owned);
        return JS_ThrowRangeError(ctx, "Base58EncodeInto: input exceeds %u bytes "
                                       "-- this is a division codec and its cost is quadratic",
            BX_MAX_INPUT);
    }
    payload = magic ? n + 4 : n;
    need = payload ? (payload * 8) / 5 + 1 : 0;
    if (dyn_enc_view_w(ctx, argv[1], &out, &on)) {
        if (owned)
            JS_FreeCString(ctx, owned);
        return JS_EXCEPTION;
    }
    if (on < need) {
        if (owned)
            JS_FreeCString(ctx, owned);
        return JS_ThrowRangeError(ctx,
            "encodeInto: output needs at least %lu bytes, got %lu",
            (unsigned long)need, (unsigned long)on);
    }
    if (dyn_enc_no_alias(ctx, data, n, out, on, "Base58EncodeInto")) {
        if (owned)
            JS_FreeCString(ctx, owned);
        return JS_EXCEPTION;
    }
    if (magic) {
        uint8_t h1[32], h2[32];
        dyn_sha256(data, n, h1);
        dyn_sha256(h1, 32, h2);
        memcpy(cks, h2, 4);
    }
    while (zeros < n && data[zeros] == 0)
        zeros++;
    if (magic && zeros == n) {
        size_t k = 0;
        while (k < 4 && cks[k] == 0) {
            zeros++;
            k++;
        }
    }
    for (i = zeros; i < n; i++)
        if (dyn_b58e_fold(out + zeros, &len, need - zeros, data[i]))
            goto oom;
    if (magic) {
        for (i = 0; i < 4; i++)
            if (dyn_b58e_fold(out + zeros, &len, need - zeros, cks[i]))
                goto oom;
    }
    for (i = 0; i < len / 2; i++) {
        uint8_t t = out[zeros + i];
        out[zeros + i] = out[zeros + len - 1 - i];
        out[zeros + len - 1 - i] = t;
    }
    for (i = 0; i < len; i++)
        out[zeros + i] = (uint8_t)BX_B58[out[zeros + i]];
    for (i = 0; i < zeros; i++)
        out[i] = (uint8_t)BX_B58[0];
    if (owned)
        JS_FreeCString(ctx, owned);
    return JS_NewInt64(ctx, (int64_t)(zeros + len));

oom:
    if (owned)
        JS_FreeCString(ctx, owned);
    return JS_ThrowOutOfMemory(ctx);
}

JSValue dyn_basex(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    int idx[256];
    const char* alpha;
    size_t alen;
    uint8_t* out = NULL;
    size_t out_len = 0;
    JSValue result;
    int rc;

    (void)this_val;
    if (argc < 2 || !JS_IsString(argv[1]))
        return JS_ThrowTypeError(ctx,
            "BaseX%s(value, alphabet): an alphabet is required",
            magic ? "Decode" : "Encode");
    alpha = JS_ToCStringLen(ctx, &alen, argv[1]);
    if (!alpha)
        return JS_EXCEPTION;
    if (bx_index_table(ctx, alpha, alen, idx) < 0) {
        JS_FreeCString(ctx, alpha);
        return JS_EXCEPTION;
    }
    if (magic == 0) {
        const uint8_t* data;
        const char* owned;
        size_t n;
        if (dyn_enc_bytes(ctx, argv[0], &data, &n, &owned) < 0) {
            JS_FreeCString(ctx, alpha);
            return JS_EXCEPTION;
        }
        if (n > BX_MAX_INPUT) {
            if (owned)
                JS_FreeCString(ctx, owned);
            JS_FreeCString(ctx, alpha);
            return JS_ThrowRangeError(ctx, "BaseXEncode: input exceeds %u bytes",
                BX_MAX_INPUT);
        }
        rc = bx_encode(data, n, alpha, (uint32_t)alen, &out, &out_len);
        if (owned)
            JS_FreeCString(ctx, owned);
        JS_FreeCString(ctx, alpha);
        if (rc < 0)
            return JS_ThrowOutOfMemory(ctx);
        result = JS_NewStringLen(ctx, (const char*)out, out_len);
        free(out);
        return result;
    }
    {
        const char* s;
        size_t n;
        if (!JS_IsString(argv[0])) {
            JS_FreeCString(ctx, alpha);
            return JS_ThrowTypeError(ctx, "BaseXDecode(text, alphabet): text "
                                          "must be a string");
        }
        s = JS_ToCStringLen(ctx, &n, argv[0]);
        if (!s) {
            JS_FreeCString(ctx, alpha);
            return JS_EXCEPTION;
        }
        if (n > BX_MAX_INPUT) {
            JS_FreeCString(ctx, s);
            JS_FreeCString(ctx, alpha);
            return JS_ThrowRangeError(ctx, "BaseXDecode: input exceeds %u bytes",
                BX_MAX_INPUT);
        }
        rc = bx_decode(s, n, idx, (uint32_t)alen, &out, &out_len);
        JS_FreeCString(ctx, s);
        JS_FreeCString(ctx, alpha);
        if (rc == -2)
            return JS_ThrowSyntaxError(ctx,
                "BaseXDecode: a character outside the alphabet");
        if (rc < 0)
            return JS_ThrowOutOfMemory(ctx);
        result = dyn_nat_u8_copy(ctx, out, out_len);
        free(out);
        return result;
    }
}

#endif
