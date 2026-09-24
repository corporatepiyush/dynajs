/* Base58, Base58Check and a generic BaseX for dyna:encoding (design 26).
   These are DIVISION codecs, not bit-packing ones: the radix does not divide
   256, so encoding is repeated divmod over the whole number and is O(n^2).
   That is correct and fine for the 32-byte inputs these exist for, and the cap
   below is what keeps it from being a denial of service. */

#define BX_MAX_INPUT 4096u              /* O(n^2): the cap IS the defence */

static const char BX_B58[] =
    "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

/* An alphabet with a repeated character silently corrupts DECODING -- the
   second occurrence is unreachable, so a value encodes one way and reads back
   as another. Rejecting it is the only honest option. */
static int bx_index_table(JSContext *ctx, const char *alpha, size_t n,
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
                                    "which would corrupt decoding", c);
            return -1;
        }
        /* int, not int8_t: positions 128-254 wrapped negative, which rejected
           valid characters and defeated the duplicate check above. */
        idx[c] = (int)i;
    }
    return 0;
}

/* Bits per digit, rounded DOWN for sizing an encode and UP for a decode: both
   directions need an over-estimate, and the rounding that gives it differs. */
static uint32_t bx_log2_floor(uint32_t base)
{
    uint32_t b = 0;
    while ((1u << (b + 1)) <= base) b++;
    return b ? b : 1;
}

/* Repeated divmod of the input, treated as one big-endian number. */
static int bx_encode(const uint8_t *src, size_t n, const char *alpha,
                     uint32_t base, uint8_t **out, size_t *out_len)
{
    size_t zeros = 0, size, i, j, len = 0;
    uint8_t *buf;

    while (zeros < n && src[zeros] == 0)
        zeros++;
    /* 8 bits per byte over the bits one digit carries. Hard-coding base 58's
       1.365 ratio here made BaseXEncode(x, "01") ask for a buffer eight times
       too small and report it as out of memory. */
    size = (n - zeros) * 8 / bx_log2_floor(base) + 1;
    buf = (uint8_t *)calloc(size + zeros + 1, 1);
    if (!buf)
        return -1;
    for (i = zeros; i < n; i++) {
        uint32_t carry = src[i];
        for (j = 0; j < len || carry; j++) {
            uint32_t v = (j < len ? (uint32_t)buf[size - 1 - j] : 0) * 256 + carry;
            if (j >= size) { free(buf); return -1; }
            buf[size - 1 - j] = (uint8_t)(v % base);
            carry = v / base;
        }
        if (j > len)
            len = j;
    }
    {   /* every leading zero byte is one leading alphabet[0] character */
        uint8_t *res = (uint8_t *)malloc(zeros + len + 1);
        if (!res) { free(buf); return -1; }
        for (i = 0; i < zeros; i++)
            res[i] = (uint8_t)alpha[0];
        for (i = 0; i < len; i++)
            res[zeros + i] = (uint8_t)alpha[buf[size - len + i]];
        res[zeros + len] = 0;
        free(buf);
        *out = res;
        *out_len = zeros + len;
    }
    return 0;
}

static int bx_decode(const char *src, size_t n, const int idx[256],
                     uint32_t base, uint8_t **out, size_t *out_len)
{
    size_t zeros = 0, size, i, j, len = 0;
    uint8_t *buf, *res;
    char zero_ch = 0;

    for (i = 0; i < 256; i++)
        if (idx[i] == 0) { zero_ch = (char)i; break; }
    while (zeros < n && src[zeros] == zero_ch)
        zeros++;
    size = (n - zeros) * (bx_log2_floor(base) + 1) / 8 + 1;
    buf = (uint8_t *)calloc(size + 1, 1);
    if (!buf)
        return -1;
    for (i = zeros; i < n; i++) {
        int d = idx[(unsigned char)src[i]];
        uint32_t carry;
        if (d < 0) { free(buf); return -2; }   /* not in the alphabet */
        carry = (uint32_t)d;
        for (j = 0; j < len || carry; j++) {
            uint32_t v = (j < len ? (uint32_t)buf[size - 1 - j] : 0) * base + carry;
            if (j >= size) { free(buf); return -1; }
            buf[size - 1 - j] = (uint8_t)(v & 0xFF);
            carry = v >> 8;
        }
        if (j > len)
            len = j;
    }
    res = (uint8_t *)malloc(zeros + len + 1);
    if (!res) { free(buf); return -1; }
    memset(res, 0, zeros);
    memcpy(res + zeros, buf + size - len, len);
    free(buf);
    *out = res;
    *out_len = zeros + len;
    return 0;
}

/* magic 0 = Base58Encode, 1 = Base58Decode, 2 = Base58CheckEncode,
   3 = Base58CheckDecode */
static JSValue dyn_b58(JSContext *ctx, JSValueConst this_val,
                       int argc, JSValueConst *argv, int magic)
{
    int idx[256];
    uint8_t *out = NULL;
    size_t out_len = 0;
    JSValue result;

    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "Base58: an argument is required");
    if (bx_index_table(ctx, BX_B58, 58, idx) < 0)
        return JS_EXCEPTION;
    if (magic == 0 || magic == 2) {
        const uint8_t *data;
        const char *owned;
        size_t n;
        uint8_t *payload = NULL;
        int rc;
        if (dyn_enc_bytes(ctx, argv[0], &data, &n, &owned) < 0)
            return JS_EXCEPTION;
        if (n > BX_MAX_INPUT) {
            if (owned) JS_FreeCString(ctx, owned);
            return JS_ThrowRangeError(ctx, "Base58: input exceeds %u bytes -- "
                "this is a division codec and its cost is quadratic", BX_MAX_INPUT);
        }
        if (magic == 2) {
            uint8_t h1[32], h2[32];
            payload = (uint8_t *)malloc(n + 4);
            if (!payload) {
                if (owned) JS_FreeCString(ctx, owned);
                return JS_ThrowOutOfMemory(ctx);
            }
            memcpy(payload, data, n);
            dyn_sha256(data, n, h1);
            dyn_sha256(h1, 32, h2);
            memcpy(payload + n, h2, 4);  /* the checksum is the first 4 bytes
                                            of the DOUBLE hash, per the spec */
            rc = bx_encode(payload, n + 4, BX_B58, 58, &out, &out_len);
            free(payload);
        } else {
            rc = bx_encode(data, n, BX_B58, 58, &out, &out_len);
        }
        if (owned) JS_FreeCString(ctx, owned);
        if (rc < 0)
            return JS_ThrowOutOfMemory(ctx);
        result = JS_NewStringLen(ctx, (const char *)out, out_len);
        free(out);
        return result;
    }
    {
        const char *s;
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
        result = dyn_enc_new_u8array(ctx, out, out_len);
        free(out);
        return result;
    }
}

/* ----: Base58DecodeInto / Base58CheckDecodeInto ---------------------
 *
 * The SAME decode-and-verify flow as dyn_b58's magics 1/3 (bx_decode, then
 * the double-SHA256 checksum strip for the check form), writing the payload
 * into a caller-owned buffer. The division core still allocates its own
 * workspace -- an Into form cannot remove that -- so what this saves is the
 * Uint8Array allocation and wrap, and what it adds is a copy the caller
 * controls. `out` must hold at least text.length bytes (a static bound: 58 <
 * 256, so n digits decode to at most n bytes, leading zeros included), which
 * is checked BEFORE the decode -- keeping the no-partial-write property the
 * cap-computed Into forms get. Returns the bytes written. */
static JSValue dyn_b58_decode_into(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv, int magic)
{
    const char *s;
    size_t n, on;
    int idx[256];
    uint8_t *dec = NULL, *out;
    size_t dec_len = 0;
    int rc;
    (void)this_val; (void)argc;

    if (argc < 2 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx,
            "Base58DecodeInto(text, out): a string and a byte view are required");
    s = JS_ToCStringLen(ctx, &n, argv[0]);
    if (!s)
        return JS_EXCEPTION;
    if (dyn_enc_view(ctx, argv[1], &out, &on)) {
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
    if (magic) {                    /* Base58CheckDecodeInto */
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

/* ----: Base58EncodeInto / Base58CheckEncodeInto ---------------------
 *
 * The ENCODE mirror, and unlike the decode side it is ZERO-ALLOCATION: the
 * caller's `out` is its own workspace. Every leading zero byte becomes
 * alpha[0] at the front; the remaining bytes are folded into a little-endian
 * base-58 digit run (values 0..57) parked at out[zeros..], using exactly
 * bx_encode's digit arithmetic (v = d*256 + carry; d = v % 58; carry = v/58)
 * over each input byte in order -- then the digit run is reversed in place
 * and mapped through the alphabet. The leading-'1' run is counted over the
 * WHOLE payload (data, then the checksum for the check form), the same run
 * bx_encode counts over its concatenated buffer. Same digits, same output
 * as Base58Encode/Base58CheckEncode; the differential in
 * tests/test_encoding_encode_into.js holds the two spellings byte-identical
 * over adversarial inputs.
 *
 * The division cost is bx_encode's O(n^2) and its cap applies. `out` must
 * hold (n*8)/5+1 bytes (the payload bound; ((n+4)*8)/5+1 for the check
 * form), checked BEFORE anything is written -- the same no-partial-write
 * property the whole Into family keeps. An empty input encodes to zero
 * bytes on the plain form; the check form of an empty input encodes its
 * 4-byte checksum. Returns the bytes written. */

/* Fold one input byte into the little-endian base-58 digit run. 0 ok. */
static int dyn_b58e_fold(uint8_t *d, size_t *dlen, size_t dcap, uint8_t b)
{
    uint32_t carry = b;
    size_t j, len = *dlen;

    for (j = 0; j < len || carry; j++) {
        uint32_t v = (j < len ? (uint32_t)d[j] : 0) * 256 + carry;
        if (j >= dcap)
            return -1;   /* unreachable: the (n*8)/5+1 bound holds */
        d[j] = (uint8_t)(v % 58);
        carry = v / 58;
    }
    if (j > len)
        *dlen = j;
    return 0;
}

/* magic 0 = Base58EncodeInto, 1 = Base58CheckEncodeInto */
static JSValue dyn_b58_encode_into(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv, int magic)
{
    const uint8_t *data;
    size_t n, on, need, zeros = 0, len = 0, i, payload;
    const char *owned;
    uint8_t *out;
    uint8_t cks[4] = { 0 };     /* the check form's checksum suffix */
    (void)this_val; (void)argc;

    if (argc < 2)
        return JS_ThrowTypeError(ctx,
            "Base58EncodeInto(data, out): a byte input and a byte view are required");
    if (dyn_enc_bytes(ctx, argv[0], &data, &n, &owned))
        return JS_EXCEPTION;
    if (n > BX_MAX_INPUT) {
        if (owned) JS_FreeCString(ctx, owned);
        return JS_ThrowRangeError(ctx, "Base58EncodeInto: input exceeds %u bytes "
            "-- this is a division codec and its cost is quadratic", BX_MAX_INPUT);
    }
    payload = magic ? n + 4 : n;
    need = payload ? (payload * 8) / 5 + 1 : 0;
    if (dyn_enc_view(ctx, argv[1], &out, &on)) {
        if (owned) JS_FreeCString(ctx, owned);
        return JS_EXCEPTION;
    }
    if (on < need) {
        if (owned) JS_FreeCString(ctx, owned);
        return JS_ThrowRangeError(ctx,
            "encodeInto: output needs at least %lu bytes, got %lu",
            (unsigned long)need, (unsigned long)on);
    }
    if (dyn_enc_no_alias(ctx, data, n, out, on, "Base58EncodeInto")) {
        if (owned) JS_FreeCString(ctx, owned);
        return JS_EXCEPTION;
    }
    if (magic) {                    /* Base58Check: the double-SHA256 suffix */
        uint8_t h1[32], h2[32];
        dyn_sha256(data, n, h1);
        dyn_sha256(h1, 32, h2);
        memcpy(cks, h2, 4);  /* the checksum is the first 4 bytes
                                of the DOUBLE hash, per the spec */
    }
    /* Leading zero bytes are one alpha[0] character each, counted over the
       WHOLE payload: the data, then the checksum for the check form. The
       one-shot counts this run over data+checksum concatenated, so counting
       it over `data` alone dropped the checksum's own leading zeros -- an
       all-zero input whose checksum began 0x00 encoded one '1' too few, and
       the decoder read the missing zero byte back as a checksum failure.
       Only an all-zero data can expose checksum bytes to the leading run
       (the first nonzero data byte ends it), which is the case below. */
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
    for (i = 0; i < len / 2; i++) { /* reverse the digit run in place ... */
        uint8_t t = out[zeros + i];
        out[zeros + i] = out[zeros + len - 1 - i];
        out[zeros + len - 1 - i] = t;
    }
    for (i = 0; i < len; i++)      /* ... and map it through the alphabet */
        out[zeros + i] = (uint8_t)BX_B58[out[zeros + i]];
    for (i = 0; i < zeros; i++)
        out[i] = (uint8_t)BX_B58[0];
    if (owned)
        JS_FreeCString(ctx, owned);
    return JS_NewInt64(ctx, (int64_t)(zeros + len));

 oom:
    if (owned)
        JS_FreeCString(ctx, owned);
    return JS_ThrowOutOfMemory(ctx);   /* the capacity bound makes this OOM */
}

/* magic 0 = encode, 1 = decode */
static JSValue dyn_basex(JSContext *ctx, JSValueConst this_val,
                         int argc, JSValueConst *argv, int magic)
{
    int idx[256];
    const char *alpha;
    size_t alen;
    uint8_t *out = NULL;
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
        const uint8_t *data;
        const char *owned;
        size_t n;
        if (dyn_enc_bytes(ctx, argv[0], &data, &n, &owned) < 0) {
            JS_FreeCString(ctx, alpha);
            return JS_EXCEPTION;
        }
        if (n > BX_MAX_INPUT) {
            if (owned) JS_FreeCString(ctx, owned);
            JS_FreeCString(ctx, alpha);
            return JS_ThrowRangeError(ctx, "BaseXEncode: input exceeds %u bytes",
                                      BX_MAX_INPUT);
        }
        rc = bx_encode(data, n, alpha, (uint32_t)alen, &out, &out_len);
        if (owned) JS_FreeCString(ctx, owned);
        JS_FreeCString(ctx, alpha);
        if (rc < 0)
            return JS_ThrowOutOfMemory(ctx);
        result = JS_NewStringLen(ctx, (const char *)out, out_len);
        free(out);
        return result;
    }
    {
        const char *s;
        size_t n;
        if (!JS_IsString(argv[0])) {
            JS_FreeCString(ctx, alpha);
            return JS_ThrowTypeError(ctx, "BaseXDecode(text, alphabet): text "
                                          "must be a string");
        }
        s = JS_ToCStringLen(ctx, &n, argv[0]);
        if (!s) { JS_FreeCString(ctx, alpha); return JS_EXCEPTION; }
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
        result = dyn_enc_new_u8array(ctx, out, out_len);
        free(out);
        return result;
    }
}
