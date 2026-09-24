/* SHA-3, Keccak-256 and SHAKE for dyna:hash (design 26).
   ONE permutation, four rates and two padding bytes: FIPS 202 appends 0x06 and
   original Keccak appends 0x01, and that single byte is the whole difference
   between SHA3-256 and the Keccak-256 that Ethereum uses. Getting it wrong
   produces a perfectly good hash of the wrong function. Full API: see the module header. */

#define K3_ROUNDS 24

static const uint64_t K3_RC[K3_ROUNDS] = {
    0x0000000000000001ULL, 0x0000000000008082ULL, 0x800000000000808aULL,
    0x8000000080008000ULL, 0x000000000000808bULL, 0x0000000080000001ULL,
    0x8000000080008081ULL, 0x8000000000008009ULL, 0x000000000000008aULL,
    0x0000000000000088ULL, 0x0000000080008009ULL, 0x000000008000000aULL,
    0x000000008000808bULL, 0x800000000000008bULL, 0x8000000000008089ULL,
    0x8000000000008003ULL, 0x8000000000008002ULL, 0x8000000000000080ULL,
    0x000000000000800aULL, 0x800000008000000aULL, 0x8000000080008081ULL,
    0x8000000000008080ULL, 0x0000000080000001ULL, 0x8000000080008008ULL,
};

static const int K3_ROT[25] = {
     0,  1, 62, 28, 27,
    36, 44,  6, 55, 20,
     3, 10, 43, 25, 39,
    41, 45, 15, 21,  8,
    18,  2, 61, 56, 14,
};

static uint64_t k3_rotl(uint64_t x, int n)
{
    return n ? ((x << n) | (x >> (64 - n))) : x;
}

/* Keccak-f[1600] over a 5x5 lane array, lanes indexed x + 5*y. */
static void k3_permute(uint64_t a[25])
{
    int r, x, y;

    for (r = 0; r < K3_ROUNDS; r++) {
        uint64_t c[5], d[5], b[25];
        for (x = 0; x < 5; x++)
            c[x] = a[x] ^ a[x + 5] ^ a[x + 10] ^ a[x + 15] ^ a[x + 20];
        for (x = 0; x < 5; x++)
            d[x] = c[(x + 4) % 5] ^ k3_rotl(c[(x + 1) % 5], 1);
        for (x = 0; x < 5; x++)
            for (y = 0; y < 5; y++)
                a[x + 5 * y] ^= d[x];
        /* rho and pi in one pass: lane (x,y) moves to (y, 2x+3y). */
        for (x = 0; x < 5; x++)
            for (y = 0; y < 5; y++)
                b[y + 5 * ((2 * x + 3 * y) % 5)] =
                    k3_rotl(a[x + 5 * y], K3_ROT[x + 5 * y]);
        for (x = 0; x < 5; x++)
            for (y = 0; y < 5; y++)
                a[x + 5 * y] = b[x + 5 * y]
                             ^ ((~b[(x + 1) % 5 + 5 * y]) & b[(x + 2) % 5 + 5 * y]);
        a[0] ^= K3_RC[r];
    }
}

/* Absorb, pad, squeeze. `rate` is in bytes; `pad` is 0x06 for SHA-3 and 0x01
   for original Keccak; SHAKE uses 0x1f. */
static void k3_hash(const uint8_t *data, size_t len, size_t rate, uint8_t pad,
                    uint8_t *out, size_t out_len)
{
    uint64_t a[25];
    uint8_t block[200];
    size_t i, k;

    memset(a, 0, sizeof a);
    while (len >= rate) {
        for (i = 0; i < rate / 8; i++) {
            uint64_t v = 0;
            for (k = 0; k < 8; k++)
                v |= (uint64_t)data[i * 8 + k] << (8 * k);   /* little-endian */
            a[i] ^= v;
        }
        k3_permute(a);
        data += rate;
        len -= rate;
    }
    memset(block, 0, rate);
    memcpy(block, data, len);
    block[len] = pad;
    block[rate - 1] |= 0x80;             /* the final bit of the pad10*1 rule */
    for (i = 0; i < rate / 8; i++) {
        uint64_t v = 0;
        for (k = 0; k < 8; k++)
            v |= (uint64_t)block[i * 8 + k] << (8 * k);
        a[i] ^= v;
    }
    k3_permute(a);
    while (out_len) {
        size_t take = out_len < rate ? out_len : rate;
        for (i = 0; i < take; i++)
            out[i] = (uint8_t)(a[i / 8] >> (8 * (i % 8)));
        out += take;
        out_len -= take;
        if (out_len)
            k3_permute(a);              /* squeeze another block */
    }
}

/* magic: 0 SHA3-224, 1 SHA3-256, 2 SHA3-384, 3 SHA3-512, 4 Keccak-256,
   5 SHAKE128, 6 SHAKE256; +8 for the hex form */
static JSValue dyn_sha3(JSContext *ctx, JSValueConst this_val,
                        int argc, JSValueConst *argv, int magic)
{
    static const struct { size_t rate, out; uint8_t pad; const char *name; } K[] = {
        { 144, 28, 0x06, "SHA3_224" }, { 136, 32, 0x06, "SHA3_256" },
        { 104, 48, 0x06, "SHA3_384" }, {  72, 64, 0x06, "SHA3_512" },
        { 136, 32, 0x01, "Keccak256" },
        { 168,  0, 0x1f, "SHAKE128" }, { 136,  0, 0x1f, "SHAKE256" },
    };
    int kind = magic & 7, hex = (magic & 8) != 0;
    const uint8_t *data;
    const char *owned;
    size_t n, out_len = K[kind].out;
    uint8_t *out;
    JSValue result;

    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "%s(data): data is required", K[kind].name);
    if (out_len == 0) {                 /* SHAKE: the caller says how much */
        int64_t want = 32;
        if (argc > 1 && !JS_IsUndefined(argv[1])) {
            if (JS_ToInt64(ctx, &want, argv[1]) < 0)
                return JS_EXCEPTION;
            if (want < 1 || want > (1 << 20))
                return JS_ThrowRangeError(ctx,
                    "%s(data, length): length is 1 to 1048576 bytes", K[kind].name);
        }
        out_len = (size_t)want;
    }
    if (dyn_crypto_data(ctx, argv[0], &data, &n, &owned) < 0)
        return JS_EXCEPTION;
    out = (uint8_t *)malloc(out_len);
    if (!out) {
        if (owned) JS_FreeCString(ctx, owned);
        return JS_ThrowOutOfMemory(ctx);
    }
    k3_hash(data, n, K[kind].rate, K[kind].pad, out, out_len);
    if (owned)
        JS_FreeCString(ctx, owned);
    if (hex) {
        char *h = (char *)malloc(out_len * 2 + 1);
        size_t i;
        if (!h) { free(out); return JS_ThrowOutOfMemory(ctx); }
        for (i = 0; i < out_len; i++) {
            h[i * 2] = "0123456789abcdef"[out[i] >> 4];
            h[i * 2 + 1] = "0123456789abcdef"[out[i] & 15];
        }
        result = JS_NewStringLen(ctx, h, out_len * 2);
        free(h);
    } else {
        result = dyn_crypto_u8array(ctx, out, out_len);
    }
    free(out);
    return result;
}

/* ---- streaming sponge (the Hasher engine's SHA-3/SHAKE/Keccak half) -----
 * Same permutation and padding as k3_hash, held across update() calls: full
 * rate-blocks are XORed into the state and permuted as they arrive, the
 * buffer keeps only the partial tail. digest() runs on a COPY (a plain
 * struct copy, the dyn_hash_ctx_t snapshot rule), pads the copy and squeezes,
 * so the original stays resumable -- Hasher.digest() semantics for free. */
#define K3_MAX_RATE 168   /* SHAKE128; the other rates are 136/104/72 */

typedef struct {
    uint64_t a[25];
    uint8_t  buf[K3_MAX_RATE];
    size_t   buflen;   /* always < rate after update() */
    size_t   rate;
    uint8_t  pad;
} k3_stream_t;

static void k3_stream_init(k3_stream_t *s, size_t rate, uint8_t pad)
{
    memset(s, 0, sizeof *s);
    s->rate = rate;
    s->pad = pad;
}

static void k3_stream_absorb(k3_stream_t *s, const uint8_t *block)
{
    size_t i, k;
    for (i = 0; i < s->rate / 8; i++) {
        uint64_t v = 0;
        for (k = 0; k < 8; k++)
            v |= (uint64_t)block[i * 8 + k] << (8 * k);   /* little-endian */
        s->a[i] ^= v;
    }
    k3_permute(s->a);
}

static void k3_stream_update(k3_stream_t *s, const uint8_t *p, size_t n)
{
    while (n) {
        if (s->buflen == 0 && n >= s->rate) {
            k3_stream_absorb(s, p);
            p += s->rate;
            n -= s->rate;
        } else {
            size_t take = s->rate - s->buflen;
            if (take > n)
                take = n;
            memcpy(s->buf + s->buflen, p, take);
            s->buflen += take;
            p += take;
            n -= take;
            if (s->buflen == s->rate) {   /* not the tail: more input came */
                k3_stream_absorb(s, s->buf);
                s->buflen = 0;
            }
        }
    }
}

/* Pad and squeeze a COPY; the caller's state is untouched. */
static void k3_stream_digest(const k3_stream_t *s0, uint8_t *out, size_t out_len)
{
    k3_stream_t s = *s0;
    uint8_t block[K3_MAX_RATE];
    memset(block, 0, s.rate);
    memcpy(block, s.buf, s.buflen);
    block[s.buflen] = s.pad;
    block[s.rate - 1] |= 0x80;            /* pad10*1's final bit */
    k3_stream_absorb(&s, block);
    dyn_wipe(block, sizeof block);        /* held input bytes: unelidable */
    while (out_len) {
        size_t take = out_len < s.rate ? out_len : s.rate;
        size_t i;
        for (i = 0; i < take; i++)
            out[i] = (uint8_t)(s.a[i / 8] >> (8 * (i % 8)));
        out += take;
        out_len -= take;
        if (out_len)
            k3_permute(s.a);              /* squeeze another block */
    }
}
