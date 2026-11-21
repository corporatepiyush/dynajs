#include "crypto-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_CRYPTO)

#define HSHK_MD 0
#define HSHK_K3 1
#define HSHK_BL2B 2
#define HSHK_BL2S 3
#define HSHK_BL3 4

typedef struct {
    uint64_t h[8];
    uint64_t t;
    uint8_t buf[128];
    size_t buflen;
    size_t outlen;
} hsh_bl2b_t;

typedef struct {
    uint32_t h[8];
    uint64_t t;
    uint8_t buf[64];
    size_t buflen;
    size_t outlen;
} hsh_bl2s_t;

typedef struct {
    int kind;
    size_t digest_size;
    const char* name;
    union {
        dyn_hash_ctx_t md;
        k3_stream_t k3;
        hsh_bl2b_t bl2b;
        hsh_bl2s_t bl2s;
        bl3_hasher_t bl3;
    } u;
} dyn_hasher_t;

typedef struct {
    const char* name;
    int kind;
    size_t rate;
    uint8_t pad;
    size_t def_out, max_out;
    int fixed;
} hsh_extra_algo_t;

#define XXH3_SECRET_SIZE 192
#define XXH_PRIME64_1 0x9E3779B185EBCA87ULL
#define XXH_PRIME64_2 0xC2B2AE3D27D4EB4FULL
#define XXH_PRIME64_3 0x165667B19E3779F9ULL
#define XXH_PRIME64_4 0x85EBCA77C2B2AE63ULL
#define XXH_PRIME64_5 0x27D4EB2F165667C5ULL
#define XXH_PRIME32_1 0x9E3779B1U
#define XXH_PRIME32_2 0x85EBCA77U
#define XXH_PRIME32_3 0xC2B2AE3DU
#define XXH_PRIME_MX1 0x165667919E3779F9ULL
#define XXH_PRIME_MX2 0x9FB21C651E98DF25ULL

static inline uint64_t xxh3_load64(const uint8_t* p)
{
    uint64_t v;
    memcpy(&v, p, 8);
    return v;
}

static inline uint32_t xxh3_load32(const uint8_t* p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static inline void xxh3_store64(uint8_t* p, uint64_t v)
{
    memcpy(p, &v, 8);
}

static int dyn_crypto_hash_run(const dyn_hash_algo_t* a, const uint8_t* data,
    size_t len, uint8_t* digest)
{
    dyn_hash_ctx_t c;
    dyn_hash_init(&c, a);
    dyn_hash_update(&c, data, len);
    dyn_hash_final(&c, digest);
    return 0;
}

JSValue dyn_crypto_hash(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv, int magic)
{
    const dyn_hash_algo_t* a = dyn_hash_algo_by_id(magic);
    const uint8_t* data;
    size_t len;
    const char* owned;
    uint8_t* hold;
    int live;
    uint8_t digest[DYN_HASH_MAX_DIGEST];
    (void)this_val;
    (void)argc;

    if (dyn_crypto_data(ctx, argv[0], &data, &len, &owned))
        return JS_EXCEPTION;
    if (dyn_crypto_private(&live, &data, len, owned, &hold) < 0)
        return JS_ThrowOutOfMemory(ctx);
    (void)DYN_OFFLOAD_CPU_IF(live, dyn_crypto_hash_run(a, data, len, digest));
    if (hold)
        free(hold);
    if (owned)
        JS_FreeCString(ctx, owned);
    return dyn_nat_u8_copy(ctx, digest, a->digest_size);
}

JSValue dyn_crypto_hash_hex(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv, int magic)
{
    const dyn_hash_algo_t* a = dyn_hash_algo_by_id(magic);
    const uint8_t* data;
    size_t len;
    const char* owned;
    uint8_t* hold;
    int live;
    uint8_t digest[DYN_HASH_MAX_DIGEST];
    (void)this_val;
    (void)argc;

    if (dyn_crypto_data(ctx, argv[0], &data, &len, &owned))
        return JS_EXCEPTION;
    if (dyn_crypto_private(&live, &data, len, owned, &hold) < 0)
        return JS_ThrowOutOfMemory(ctx);
    (void)DYN_OFFLOAD_CPU_IF(live, dyn_crypto_hash_run(a, data, len, digest));
    if (hold)
        free(hold);
    if (owned)
        JS_FreeCString(ctx, owned);
    return dyn_crypto_hex(ctx, digest, a->digest_size);
}

static const uint64_t dyn_cdc_gear[256] = {
    0x1ac046dda8e86e2aull,
    0xbe2c3b00b1d348c8ull,
    0x9b1a66a95412ff75ull,
    0xc448c2b1f05f7e4cull,
    0xc111ca6b8f6e73c4ull,
    0xb54861920d05b01dull,
    0x8d61500f4a7bbe16ull,
    0x5e0c25471f89e02eull,
    0x48105a3d28f0e221ull,
    0x2169f8846b637746ull,
    0x3d628782e0c0d863ull,
    0xa5ddb2216078aa40ull,
    0xc8119d17f0571101ull,
    0x98e2e2eb8f33280full,
    0x8cd1e28860679cc4ull,
    0x9dca6189c923aef3ull,
    0x9d8d3071ba4f04c4ull,
    0x5d395ada34220c26ull,
    0xe6de42a441a1e28eull,
    0x308fbf68cc864f59ull,
    0x216a3c81332862f9ull,
    0xbaceca0a77f3132eull,
    0xdf2a2215339ca69cull,
    0x3e4c11a103a5d859ull,
    0x6d0f173ffec5f603ull,
    0x0bf4bc630d193bb6ull,
    0x5f76c4ad104b57fdull,
    0x99ca459f4e93f651ull,
    0x4751799d68cf88a0ull,
    0xa6b1639e3b42b61cull,
    0x278b01031924ea35ull,
    0x430253eb7e993605ull,
    0x5f4e14147961f2e8ull,
    0x52aead5ef08ac45full,
    0x583dca09af910274ull,
    0x4a8b9d4b576480cbull,
    0xbee913dc4ef28b44ull,
    0x7de79c7a57af8587ull,
    0x1ecf42b9e34cd874ull,
    0x38adac4ab1f3aad1ull,
    0x80ff3025878a34b8ull,
    0xf10a8816c7ac2d95ull,
    0xeff8dc4b1fa1c5d4ull,
    0x0b0ebe1144fe022full,
    0x4d46a271e58e80a2ull,
    0x09cd31f10075274full,
    0xa82f74eaa55bc441ull,
    0x497f6541631d47a4ull,
    0x888b7ede7346db17ull,
    0x256147dc71c784e0ull,
    0x8a5d6ed77045cd6cull,
    0xa9fc0986de332f0bull,
    0x2f597787e8c75c47ull,
    0x3648fb06e09eefe8ull,
    0xceac1655a16aee55ull,
    0x614c72624b61148dull,
    0x4cbdd6aec064c0f0ull,
    0x6620e70990008130ull,
    0x0f7c12bf3c7e6fc3ull,
    0x33a8b131d6275b9bull,
    0xfa11bd2037c759caull,
    0x720ddad5e616729aull,
    0xf7d65a62aa36f6cdull,
    0x79c452ac75db451dull,
    0xb67b17d3a1221ec5ull,
    0xa121663523494b41ull,
    0xb0299b3ec41c4cedull,
    0x6fc29450adcad869ull,
    0x47e9b8ec3fc8cbb7ull,
    0x62fdc189d1af50f0ull,
    0xe2a4894d230c71c5ull,
    0x2b29e84f96f10a17ull,
    0x6a06d8f31cc8127bull,
    0xd2cff0ec00d51e42ull,
    0x53a34f9751fa14dbull,
    0x5527bdf3764839bdull,
    0x5b2b498aa588f2d2ull,
    0x036c60fb15914351ull,
    0x796dff2c504ae68cull,
    0xa0b68b3deb4a26eeull,
    0x538d384072828564ull,
    0x5c8365c92d8e618eull,
    0xadcbd6468938043eull,
    0xa62e0a7bfd3c7a87ull,
    0xf94882172a2802d2ull,
    0xe1460d5af30b3df4ull,
    0x875af97cf2a77a1eull,
    0xcd4ced68dc5d03feull,
    0x34b85bbb2ed2cbb8ull,
    0x14382eba487c2a39ull,
    0x1bf2b642ec0d725eull,
    0x3180c22f85fd4a6eull,
    0x6287e68c688b0a6aull,
    0xc781dbd269c1579bull,
    0x967fba740d8851eeull,
    0x8bcb6289f451eab1ull,
    0xb00af395b957706aull,
    0xd66f731a7ebc0d9aull,
    0x0753e0b1e260c0ffull,
    0x9123b3fc244c22f0ull,
    0xea18df1333df68c7ull,
    0x9eec6b6e47ee4d7full,
    0xfb67ca727d5a7eecull,
    0xff8b16c00c21c99eull,
    0x358784cdb4cb66ecull,
    0x03216b3236e1a9f0ull,
    0xb04c2b63efd0ff13ull,
    0x7c706fdd841f7fdeull,
    0x7d73537d5868a02aull,
    0x79d2f0856b8f869bull,
    0x3ed8cd3a1f18f1dcull,
    0xa63e972135a79123ull,
    0xbae6b248ea01376full,
    0xc6a62efd6e07e935ull,
    0x95bd020eb8287729ull,
    0xddc64b8aa63f411bull,
    0xe3b876db230a4b8cull,
    0xfc2662a03a990c51ull,
    0xc4164ab8549560b2ull,
    0x03661ab91fdc46cfull,
    0x407d681d863d005eull,
    0x748cad2bdea25f24ull,
    0xa6af3a8fbbe02591ull,
    0x4fe003a7ae850547ull,
    0x016d512803fe9519ull,
    0xd3c80ba79b797d64ull,
    0x519a33023219d39full,
    0xa9b8738fd7958fcaull,
    0xb068afbcd3e6cfacull,
    0x12d82d1c233b6a89ull,
    0x52ff395050d637efull,
    0x0b9289abd111c12bull,
    0x280a50d348204e9dull,
    0xc3e4bfbbb3b183f7ull,
    0x460ac41c779fb804ull,
    0x50a570f9e185ec4bull,
    0x3f4da17a82d062a7ull,
    0xd09ec8514e2854b2ull,
    0xd693ad5620641415ull,
    0xa7b39dbe6975c0caull,
    0xa0d0f63f4d9aef1aull,
    0x15af0cbc4969c7d5ull,
    0x278011eaab5c3f0eull,
    0x5e1cf19380ce0c38ull,
    0xb1ba4d9029a2956dull,
    0x73f08e7440c16206ull,
    0x6f9b01ffb859822eull,
    0x5a11189a2b6728e2ull,
    0xa8558b99a4170496ull,
    0x7f2f938318e74c32ull,
    0xbea616a7fd5e3bc4ull,
    0xdbfeafdd8425000dull,
    0x38c230df150c847full,
    0x17ec72a519accd61ull,
    0x036fa2fbc835b4f6ull,
    0x3f4902d125ddcaeeull,
    0xc9dc1fec3a0ac22full,
    0x4fc8d70c9ee4d990ull,
    0xaae8a531b1c93da2ull,
    0xe1fa0e077e0cec8cull,
    0x90356a76ca9c574bull,
    0x2a26cc7a2879d838ull,
    0xcf4ed251a2ae162bull,
    0x098b973c62c609eaull,
    0x1be77277ef4b9126ull,
    0x2acb7cac64d26155ull,
    0xd876dbe01e1e90acull,
    0x51ad90e39ff2711dull,
    0x56c2dbc758d198b0ull,
    0x1f4e0301f8842f44ull,
    0x708969745130b1a1ull,
    0x9a4311b95a6a991dull,
    0x9afcede497e4ddb6ull,
    0xcf3169e617e9ca2dull,
    0x1b4ecbbf8e54cf3dull,
    0x5e9ce5d535be41b4ull,
    0xe7faa5baf8248ea5ull,
    0x3675637ace70bdceull,
    0xd980d9032ec07c88ull,
    0xec6e37a873ecf8b1ull,
    0xf9d4074f810c18dbull,
    0xb60a4b86daa6ef2aull,
    0x4e899a8f297395dbull,
    0x7165c4bd2470cda3ull,
    0x8253b43083c02137ull,
    0x3e025a61ee7fd941ull,
    0x322e76006c21fe35ull,
    0x0ad2377d2e13ed73ull,
    0x46c5cca798eb198eull,
    0x0f73c7b0b88be5a0ull,
    0x9bdbeb2841204b09ull,
    0x4d196436aae8e99bull,
    0x7f3bba1f8a36d062ull,
    0xe65247c253ec319full,
    0x536ec5f02d4e4335ull,
    0x13a17a653a4e29abull,
    0x6eb9f62ff9e69bcdull,
    0x9be0c43eee73606bull,
    0x42aa9b137474a26aull,
    0x38d992c2b7969b10ull,
    0x00584830af6dcb06ull,
    0x21fbd546ca9dc7b4ull,
    0x613143aef10f037eull,
    0x249018dd3524b6ebull,
    0x625f5025eb78a5dbull,
    0x89dffc140591ea45ull,
    0xeabe2cb345bb7fa9ull,
    0xb3d74fdd70015b81ull,
    0xd31bf6ac6e6eff00ull,
    0xffa32024d7e7a05eull,
    0x32675789370b11c1ull,
    0x26cf04b6940262d0ull,
    0x7016e72357d61660ull,
    0x25818a6720cebd3full,
    0xdb731160b31e0635ull,
    0x380407a507c37907ull,
    0xcadf246dd50299f4ull,
    0xbf8f0f184d6c4a16ull,
    0x38119a0902b7a6d0ull,
    0x06ac8fe2ec3606b2ull,
    0x7abc00c02cc859ccull,
    0xf93819575bbf449eull,
    0x2d9dc57e43f28641ull,
    0xea5df4a5436eaf2full,
    0xcab3b92f92d36e8bull,
    0x211bcfa592b9e1bfull,
    0x67ae1da4c7d43427ull,
    0xad700ad7ccaea894ull,
    0x2b107d3d815d86d8ull,
    0x0010b23e14c8bef3ull,
    0x2b1d0f1d75d26f7bull,
    0x3b4ff56c622e7f43ull,
    0x6cacaa7ec6e2f69eull,
    0xf134b52034eb99ddull,
    0x9a2f4c1d1b73a531ull,
    0xf3e4ad23b672706dull,
    0x5c39b33babb430d6ull,
    0xb3c783a4732b3fd5ull,
    0xefd45192ceb437adull,
    0x7d16c00ff3817bc1ull,
    0xf69003865fca895eull,
    0xbd83805faee0202eull,
    0x398c44e739df0decull,
    0x7b190c1260f2583eull,
    0xf33479f42bf6780cull,
    0x1e4b54e22fbe719dull,
    0x03d1f2ee77632020ull,
    0x2a7414b98717fdc8ull,
    0x8534a1646babf432ull,
    0x55af162af065b106ull,
    0x47cdbd2911f272e8ull,
    0x7d9f49a5d5fce2e7ull,
    0x0196fe50064dbca7ull,
    0x69c325a23ab5755full,
    0xb9cabfd1de7de997ull,
    0x869756f713a06d5eull,
};

static int dyn_cdc_opt(JSContext* ctx, JSValueConst opts, const char* key,
    double lo, double hi, uint32_t* out)
{
    JSValue v = JS_GetPropertyStr(ctx, opts, key);
    double d;
    if (JS_IsException(v))
        return -1;
    if (JS_IsUndefined(v))
        return 0;
    if (JS_ToFloat64(ctx, &d, v)) {
        JS_FreeValue(ctx, v);
        return -1;
    }
    JS_FreeValue(ctx, v);
    if (!(d >= lo && d <= hi) || d != floor(d)) {
        JS_ThrowRangeError(ctx, "CDCChunks: %s must be an integer in [%.0f, %.0f]", key, lo, hi);
        return -1;
    }
    *out = (uint32_t)d;
    return 0;
}

static uint32_t dyn_cdc_cut(const uint8_t* p, uint32_t n, uint32_t min, uint32_t avg,
    uint32_t max, uint64_t mask_s, uint64_t mask_l)
{
    uint64_t h = 0;
    uint32_t i, normal;
    if (n <= min)
        return n;
    if (n > max)
        n = max;
    normal = avg < n ? avg : n;
    for (i = min; i < normal; i++) {
        h = (h << 1) + dyn_cdc_gear[p[i]];
        if (!(h & mask_s))
            return i + 1;
    }
    for (; i < n; i++) {
        h = (h << 1) + dyn_cdc_gear[p[i]];
        if (!(h & mask_l))
            return i + 1;
    }
    return n;
}

JSValue dyn_hash_cdc(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    static const char* const keys[] = { "min", "avg", "max" };
    uint32_t min = 2048, avg = 8192, max = 65536, *cuts = NULL;
    const uint8_t* data;
    const char* owned = NULL;
    size_t len, ncuts = 0, cap, pos = 0;
    int bits;
    uint64_t mask_s, mask_l;
    JSValue ab, out, ta[3];
    (void)this_val;

    if (argc > 1 && !JS_IsUndefined(argv[1])) {
        if (!JS_IsObject(argv[1]))
            return JS_ThrowTypeError(ctx, "CDCChunks: options must be an object");
        if (dyn_opts_strict(ctx, argv[1], keys, 3) < 0
            || dyn_cdc_opt(ctx, argv[1], "min", 64, 1 << 26, &min) < 0
            || dyn_cdc_opt(ctx, argv[1], "avg", 256, 1 << 26, &avg) < 0
            || dyn_cdc_opt(ctx, argv[1], "max", 256, 1 << 28, &max) < 0)
            return JS_EXCEPTION;
    }
    if (avg & (avg - 1))
        return JS_ThrowRangeError(ctx, "CDCChunks: avg must be a power of two");
    if (!(min < avg && avg < max))
        return JS_ThrowRangeError(ctx, "CDCChunks: need min < avg < max");
    for (bits = 0; (1u << bits) < avg; bits++)
        ;
    mask_s = ~(uint64_t)0 << (64 - (bits + 2));
    mask_l = ~(uint64_t)0 << (64 - (bits - 2));
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "CDCChunks(data[, options]): data is required");
    if (dyn_crypto_data(ctx, argv[0], &data, &len, &owned))
        return JS_EXCEPTION;
    if (len > 0xffffffffu) {
        if (owned)
            JS_FreeCString(ctx, owned);
        return JS_ThrowRangeError(ctx, "CDCChunks: data exceeds 4 GiB");
    }
    cap = len / min + 2;
    cuts = (uint32_t*)malloc(cap * sizeof(uint32_t));
    if (!cuts) {
        if (owned)
            JS_FreeCString(ctx, owned);
        return JS_ThrowOutOfMemory(ctx);
    }
    while (pos < len) {
        pos += dyn_cdc_cut(data + pos, (uint32_t)(len - pos), min, avg, max, mask_s, mask_l);
        cuts[ncuts++] = (uint32_t)pos;
    }
    if (owned)
        JS_FreeCString(ctx, owned);
    ab = JS_NewArrayBufferCopy(ctx, (const uint8_t*)cuts, ncuts * sizeof(uint32_t));
    free(cuts);
    if (JS_IsException(ab))
        return ab;
    ta[0] = ab;
    ta[1] = JS_NewInt32(ctx, 0);
    ta[2] = JS_NewInt64(ctx, (int64_t)ncuts);
    out = JS_NewTypedArray(ctx, 3, ta, JS_TYPED_ARRAY_UINT32);
    JS_FreeValue(ctx, ab);
    return out;
}

static void hsh_bl2b_init(hsh_bl2b_t* s, size_t outlen)
{
    memset(s, 0, sizeof *s);
    memcpy(s->h, BL2B_IV, sizeof s->h);
    s->h[0] ^= 0x01010000ULL ^ (uint64_t)outlen;
    s->outlen = outlen;
}

static void hsh_bl2b_update(hsh_bl2b_t* s, const uint8_t* in, size_t n)
{
    if (!n)
        return;
    if (s->buflen + n > 128) {
        size_t fill = 128 - s->buflen;
        memcpy(s->buf + s->buflen, in, fill);
        s->t += 128;
        bl2b_compress(s->h, s->buf, s->t, 0);
        s->buflen = 0;
        in += fill;
        n -= fill;
        while (n > 128) {
            s->t += 128;
            bl2b_compress(s->h, in, s->t, 0);
            in += 128;
            n -= 128;
        }
    }
    memcpy(s->buf + s->buflen, in, n);
    s->buflen += n;
}

static void hsh_bl2b_digest(const hsh_bl2b_t* s0, uint8_t* out)
{
    hsh_bl2b_t s = *s0;
    uint8_t block[128];
    size_t i;
    memset(block, 0, sizeof block);
    memcpy(block, s.buf, s.buflen);
    s.t += s.buflen;
    bl2b_compress(s.h, block, s.t, 1);
    for (i = 0; i < s.outlen; i++)
        out[i] = (uint8_t)(s.h[i / 8] >> (8 * (i % 8)));
    dyn_wipe(block, sizeof block);
}

static void hsh_bl2s_init(hsh_bl2s_t* s, size_t outlen)
{
    memset(s, 0, sizeof *s);
    memcpy(s->h, BL3_IV, sizeof s->h);
    s->h[0] ^= 0x01010000u ^ (uint32_t)outlen;
    s->outlen = outlen;
}

static void hsh_bl2s_update(hsh_bl2s_t* s, const uint8_t* in, size_t n)
{
    if (!n)
        return;
    if (s->buflen + n > 64) {
        size_t fill = 64 - s->buflen;
        memcpy(s->buf + s->buflen, in, fill);
        s->t += 64;
        bl2s_compress(s->h, s->buf, s->t, 0);
        s->buflen = 0;
        in += fill;
        n -= fill;
        while (n > 64) {
            s->t += 64;
            bl2s_compress(s->h, in, s->t, 0);
            in += 64;
            n -= 64;
        }
    }
    memcpy(s->buf + s->buflen, in, n);
    s->buflen += n;
}

static void hsh_bl2s_digest(const hsh_bl2s_t* s0, uint8_t* out)
{
    hsh_bl2s_t s = *s0;
    uint8_t block[64];
    size_t i;
    memset(block, 0, sizeof block);
    memcpy(block, s.buf, s.buflen);
    s.t += s.buflen;
    bl2s_compress(s.h, block, s.t, 1);
    for (i = 0; i < s.outlen; i++)
        out[i] = (uint8_t)(s.h[i / 4] >> (8 * (i % 4)));
    dyn_wipe(block, sizeof block);
}

static const hsh_extra_algo_t hsh_extra_algos[] = {
    { "sha3_224", HSHK_K3, 144, 0x06, 28, 28, 1 },
    { "sha3_256", HSHK_K3, 136, 0x06, 32, 32, 1 },
    { "sha3_384", HSHK_K3, 104, 0x06, 48, 48, 1 },
    { "sha3_512", HSHK_K3, 72, 0x06, 64, 64, 1 },
    { "keccak256", HSHK_K3, 136, 0x01, 32, 32, 1 },
    { "shake128", HSHK_K3, 168, 0x1f, 32, 1u << 20, 0 },
    { "shake256", HSHK_K3, 136, 0x1f, 32, 1u << 20, 0 },
    { "blake2b", HSHK_BL2B, 0, 0, 64, 64, 0 },
    { "blake2s", HSHK_BL2S, 0, 0, 32, 32, 0 },
    { "blake3", HSHK_BL3, 0, 0, 32, 1u << 20, 0 },
};

static int hasher_algo_init(dyn_hasher_t* h, const char* name, int64_t optlen,
    int has_len, JSContext* ctx)
{
    const dyn_hash_algo_t* md = dyn_hash_algo_by_name(name);
    size_t def, max;
    int i;

    memset(h, 0, sizeof *h);
    if (md) {
        h->kind = HSHK_MD;
        h->name = md->name;
        h->digest_size = md->digest_size;
        if (has_len) {
            JS_ThrowRangeError(ctx,
                "Hasher: %s has a fixed %u-byte digest; no length option",
                md->name, (unsigned)md->digest_size);
            return -2;
        }
        dyn_hash_init(&h->u.md, md);
        return 0;
    }
    for (i = 0; i < (int)countof(hsh_extra_algos); i++) {
        const hsh_extra_algo_t* a = &hsh_extra_algos[i];
        if (strcmp(name, a->name))
            continue;
        def = a->def_out;
        max = a->max_out;
        h->kind = a->kind;
        h->name = a->name;
        h->digest_size = def;
        if (has_len) {
            if (a->fixed) {
                JS_ThrowRangeError(ctx,
                    "Hasher: %s has a fixed %zu-byte digest; no length option",
                    a->name, def);
                return -2;
            }
            if (optlen < 1 || (size_t)optlen > max) {
                JS_ThrowRangeError(ctx,
                    "Hasher: %s length option is 1..%zu bytes", a->name, max);
                return -2;
            }
            h->digest_size = (size_t)optlen;
        }
        switch (h->kind) {
        case HSHK_K3:
            k3_stream_init(&h->u.k3, a->rate, a->pad);
            break;
        case HSHK_BL2B:
            hsh_bl2b_init(&h->u.bl2b, h->digest_size);
            break;
        case HSHK_BL2S:
            hsh_bl2s_init(&h->u.bl2s, h->digest_size);
            break;
        default:
            bl3_init(&h->u.bl3);
            break;
        }
        return 0;
    }
    return -1;
}

static void hasher_algo_reset(dyn_hasher_t* h)
{
    switch (h->kind) {
    case HSHK_MD: {
        const dyn_hash_algo_t* md = dyn_hash_algo_by_name(h->name);
        if (md)
            dyn_hash_init(&h->u.md, md);
        break;
    }
    case HSHK_K3:
        k3_stream_init(&h->u.k3, h->u.k3.rate, h->u.k3.pad);
        break;
    case HSHK_BL2B:
        hsh_bl2b_init(&h->u.bl2b, h->u.bl2b.outlen);
        break;
    case HSHK_BL2S:
        hsh_bl2s_init(&h->u.bl2s, h->u.bl2s.outlen);
        break;
    default:
        bl3_init(&h->u.bl3);
        break;
    }
}

static void hasher_algo_digest(const dyn_hasher_t* h, uint8_t* out)
{
    dyn_hasher_t copy = *h;
    switch (copy.kind) {
    case HSHK_MD:
        dyn_hash_final(&copy.u.md, out);
        break;
    case HSHK_K3:
        k3_stream_digest(&copy.u.k3, out, copy.digest_size);
        break;
    case HSHK_BL2B:
        hsh_bl2b_digest(&copy.u.bl2b, out);
        break;
    case HSHK_BL2S:
        hsh_bl2s_digest(&copy.u.bl2s, out);
        break;
    default:
        bl3_final(&copy.u.bl3, out, copy.digest_size);
        break;
    }
}

static JSClassID dyn_hasher_class_id;

static void dyn_hasher_dispose(void* native)
{
    free(native);
}

static const JSClassDef dyn_hasher_class = {
    "Hasher",
    .finalizer = dyn_res_finalizer,
};

static JSValue dyn_hasher_ctor(JSContext* ctx, JSValueConst new_target, int argc,
    JSValueConst* argv)
{
    const char* name;
    dyn_hasher_t* h;
    int64_t optlen = -1;
    int has_len = 0;

    if (argc < 1 || JS_IsUndefined(argv[0]))
        return JS_ThrowTypeError(ctx, "Hasher(algorithm) requires an algorithm name");
    if (argc > 1 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
        JSValue v = JS_GetPropertyStr(ctx, argv[1], "length");
        if (JS_IsException(v))
            return JS_EXCEPTION;
        if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
            has_len = 1;
            if (JS_ToInt64(ctx, &optlen, v)) {
                JS_FreeValue(ctx, v);
                return JS_EXCEPTION;
            }
        }
        JS_FreeValue(ctx, v);
    }
    name = JS_ToCString(ctx, argv[0]);
    if (!name)
        return JS_EXCEPTION;

    h = (dyn_hasher_t*)malloc(sizeof(*h));
    if (!h) {
        JS_FreeCString(ctx, name);
        return JS_ThrowOutOfMemory(ctx);
    }
    {
        int rc = hasher_algo_init(h, name, optlen, has_len, ctx);
        JS_FreeCString(ctx, name);
        if (rc) {
            free(h);
            if (rc == -2)
                return JS_EXCEPTION;
            return JS_ThrowTypeError(ctx,
                "Hasher: unknown algorithm (md5|sha1|sha224|sha256|sha384|sha512|"
                "sha3_224|sha3_256|sha3_384|sha3_512|keccak256|shake128|shake256|"
                "blake2b|blake2s|blake3)");
        }
    }
    return dyn_res_wrap(ctx, new_target, dyn_hasher_class_id, h, dyn_hasher_dispose);
}

static JSValue dyn_hasher_update(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    const uint8_t* data;
    size_t len;
    const char* owned;
    dyn_hasher_t* h;
    (void)argc;

    if (dyn_crypto_data(ctx, argv[0], &data, &len, &owned))
        return JS_EXCEPTION;
    h = (dyn_hasher_t*)dyn_res_native(ctx, this_val, dyn_hasher_class_id);
    if (!h) {
        if (owned)
            JS_FreeCString(ctx, owned);
        return JS_EXCEPTION;
    }
    switch (h->kind) {
    case HSHK_MD:
        dyn_hash_update(&h->u.md, data, len);
        break;
    case HSHK_K3:
        k3_stream_update(&h->u.k3, data, len);
        break;
    case HSHK_BL2B:
        hsh_bl2b_update(&h->u.bl2b, data, len);
        break;
    case HSHK_BL2S:
        hsh_bl2s_update(&h->u.bl2s, data, len);
        break;
    default:
        bl3_update(&h->u.bl3, data, len);
        break;
    }
    if (owned)
        JS_FreeCString(ctx, owned);
    return JS_DupValue(ctx, this_val);
}

static JSValue dyn_hasher_digest(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    dyn_hasher_t* h;
    uint8_t stackbuf[200];
    uint8_t* digest = stackbuf;
    JSValue ret;
    (void)argc;
    (void)argv;

    h = (dyn_hasher_t*)dyn_res_native(ctx, this_val, dyn_hasher_class_id);
    if (!h)
        return JS_EXCEPTION;
    if (h->digest_size > sizeof(stackbuf)) {
        digest = (uint8_t*)malloc(h->digest_size);
        if (!digest)
            return JS_ThrowOutOfMemory(ctx);
    }
    hasher_algo_digest(h, digest);
    ret = dyn_nat_u8_copy(ctx, digest, h->digest_size);
    if (digest != stackbuf) {
        dyn_wipe(digest, h->digest_size);
        free(digest);
    }
    return ret;
}

static JSValue dyn_hasher_digest_hex(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    dyn_hasher_t* h;
    uint8_t stackbuf[200];
    uint8_t* digest = stackbuf;
    JSValue ret;
    (void)argc;
    (void)argv;

    h = (dyn_hasher_t*)dyn_res_native(ctx, this_val, dyn_hasher_class_id);
    if (!h)
        return JS_EXCEPTION;
    if (h->digest_size > sizeof(stackbuf)) {
        digest = (uint8_t*)malloc(h->digest_size);
        if (!digest)
            return JS_ThrowOutOfMemory(ctx);
    }
    hasher_algo_digest(h, digest);
    ret = dyn_crypto_hex(ctx, digest, h->digest_size);
    if (digest != stackbuf) {
        dyn_wipe(digest, h->digest_size);
        free(digest);
    }
    return ret;
}

static JSValue dyn_hasher_digest_into(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_hasher_t* h;
    size_t off, blen, bpe, ab_size, dsize;
    uint8_t* base;
    JSValue ab;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "digestInto(buf) requires a Uint8Array");
    if (JS_CheckWritableBuffer(ctx, argv[0]) < 0)
        return JS_EXCEPTION;
    ab = JS_GetTypedArrayBuffer(ctx, argv[0], &off, &blen, &bpe);
    if (JS_IsException(ab))
        return ab;
    base = JS_GetArrayBuffer(ctx, &ab_size, ab);
    JS_FreeValue(ctx, ab);
    if (!base)
        return JS_EXCEPTION;
    if (off > ab_size || blen > ab_size - off) {
        JS_ThrowRangeError(ctx, "typed array out of bounds");
        return JS_EXCEPTION;
    }

    h = (dyn_hasher_t*)dyn_res_native(ctx, this_val, dyn_hasher_class_id);
    if (!h)
        return JS_EXCEPTION;
    dsize = h->digest_size;
    if (blen < dsize) {
        JS_ThrowRangeError(ctx, "digestInto: buffer byteLength %zu is smaller "
                                "than the digest size %zu",
            blen, dsize);
        return JS_EXCEPTION;
    }
    hasher_algo_digest(h, base + off);
    return JS_NewInt64(ctx, (int64_t)dsize);
}

static JSValue dyn_hasher_reset(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    dyn_hasher_t* h;
    (void)argc;
    (void)argv;

    h = (dyn_hasher_t*)dyn_res_native(ctx, this_val, dyn_hasher_class_id);
    if (!h)
        return JS_EXCEPTION;
    hasher_algo_reset(h);
    return JS_DupValue(ctx, this_val);
}

static JSValue dyn_hasher_get_algorithm(JSContext* ctx, JSValueConst this_val)
{
    dyn_hasher_t* h = (dyn_hasher_t*)dyn_res_native(ctx, this_val, dyn_hasher_class_id);
    if (!h)
        return JS_EXCEPTION;
    return JS_NewString(ctx, h->name);
}

static JSValue dyn_hasher_get_digest_size(JSContext* ctx, JSValueConst this_val)
{
    dyn_hasher_t* h = (dyn_hasher_t*)dyn_res_native(ctx, this_val, dyn_hasher_class_id);
    if (!h)
        return JS_EXCEPTION;
    return JS_NewInt32(ctx, (int32_t)h->digest_size);
}

static const JSCFunctionListEntry dyn_hasher_proto[] = {
    JS_CFUNC_DEF("update", 1, dyn_hasher_update),
    JS_CFUNC_DEF("digest", 0, dyn_hasher_digest),
    JS_CFUNC_DEF("digestHex", 0, dyn_hasher_digest_hex),
    JS_CFUNC_DEF("digestInto", 1, dyn_hasher_digest_into),
    JS_CFUNC_DEF("reset", 0, dyn_hasher_reset),
    JS_CGETSET_DEF("algorithm", dyn_hasher_get_algorithm, NULL),
    JS_CGETSET_DEF("digestSize", dyn_hasher_get_digest_size, NULL),
};

static const uint8_t xxh3_ksecret[XXH3_SECRET_SIZE] = {
    0xb8,
    0xfe,
    0x6c,
    0x39,
    0x23,
    0xa4,
    0x4b,
    0xbe,
    0x7c,
    0x01,
    0x81,
    0x2c,
    0xf7,
    0x21,
    0xad,
    0x1c,
    0xde,
    0xd4,
    0x6d,
    0xe9,
    0x83,
    0x90,
    0x97,
    0xdb,
    0x72,
    0x40,
    0xa4,
    0xa4,
    0xb7,
    0xb3,
    0x67,
    0x1f,
    0xcb,
    0x79,
    0xe6,
    0x4e,
    0xcc,
    0xc0,
    0xe5,
    0x78,
    0x82,
    0x5a,
    0xd0,
    0x7d,
    0xcc,
    0xff,
    0x72,
    0x21,
    0xb8,
    0x08,
    0x46,
    0x74,
    0xf7,
    0x43,
    0x24,
    0x8e,
    0xe0,
    0x35,
    0x90,
    0xe6,
    0x81,
    0x3a,
    0x26,
    0x4c,
    0x3c,
    0x28,
    0x52,
    0xbb,
    0x91,
    0xc3,
    0x00,
    0xcb,
    0x88,
    0xd0,
    0x65,
    0x8b,
    0x1b,
    0x53,
    0x2e,
    0xa3,
    0x71,
    0x64,
    0x48,
    0x97,
    0xa2,
    0x0d,
    0xf9,
    0x4e,
    0x38,
    0x19,
    0xef,
    0x46,
    0xa9,
    0xde,
    0xac,
    0xd8,
    0xa8,
    0xfa,
    0x76,
    0x3f,
    0xe3,
    0x9c,
    0x34,
    0x3f,
    0xf9,
    0xdc,
    0xbb,
    0xc7,
    0xc7,
    0x0b,
    0x4f,
    0x1d,
    0x8a,
    0x51,
    0xe0,
    0x4b,
    0xcd,
    0xb4,
    0x59,
    0x31,
    0xc8,
    0x9f,
    0x7e,
    0xc9,
    0xd9,
    0x78,
    0x73,
    0x64,
    0xea,
    0xc5,
    0xac,
    0x83,
    0x34,
    0xd3,
    0xeb,
    0xc3,
    0xc5,
    0x81,
    0xa0,
    0xff,
    0xfa,
    0x13,
    0x63,
    0xeb,
    0x17,
    0x0d,
    0xdd,
    0x51,
    0xb7,
    0xf0,
    0xda,
    0x49,
    0xd3,
    0x16,
    0x55,
    0x26,
    0x29,
    0xd4,
    0x68,
    0x9e,
    0x2b,
    0x16,
    0xbe,
    0x58,
    0x7d,
    0x47,
    0xa1,
    0xfc,
    0x8f,
    0xf8,
    0xb8,
    0xd1,
    0x7a,
    0xd0,
    0x31,
    0xce,
    0x45,
    0xcb,
    0x3a,
    0x8f,
    0x95,
    0x16,
    0x04,
    0x28,
    0xaf,
    0xd7,
    0xfb,
    0xca,
    0xbb,
    0x4b,
    0x40,
    0x7e,
};

static uint64_t xxh3_mul128_fold(uint64_t a, uint64_t b)
{
    uint64_t a0 = (uint32_t)a, a1 = a >> 32;
    uint64_t b0 = (uint32_t)b, b1 = b >> 32;
    uint64_t lo = a0 * b0;
    uint64_t m1 = a1 * b0, m2 = a0 * b1;
    uint64_t mid = (m1 & 0xFFFFFFFFULL) + (m2 & 0xFFFFFFFFULL) + (lo >> 32);
    uint64_t hi = a1 * b1 + (m1 >> 32) + (m2 >> 32) + (mid >> 32);
    uint64_t mlo = (lo & 0xFFFFFFFFULL) | ((mid & 0xFFFFFFFFULL) << 32);
    return mlo ^ hi;
}

static uint64_t xxh3_avalanche(uint64_t h)
{
    h ^= h >> 37;
    h *= XXH_PRIME_MX1;
    h ^= h >> 32;
    return h;
}

static uint64_t xxh3_avalanche64(uint64_t h)
{
    h ^= h >> 33;
    h *= XXH_PRIME64_2;
    h ^= h >> 29;
    h *= XXH_PRIME64_3;
    h ^= h >> 32;
    return h;
}

static uint64_t xxh3_rrmxmx(uint64_t h, uint64_t len)
{
    h ^= dyn_rotl64(h, 49) ^ dyn_rotl64(h, 24);
    h *= XXH_PRIME_MX2;
    h ^= (h >> 35) + len;
    h *= XXH_PRIME_MX2;
    h ^= h >> 28;
    return h;
}

static uint64_t xxh3_mix16b(const uint8_t* in, const uint8_t* sec, uint64_t seed)
{
    return xxh3_mul128_fold(xxh3_load64(in) ^ (xxh3_load64(sec) + seed),
        xxh3_load64(in + 8) ^ (xxh3_load64(sec + 8) - seed));
}

static void xxh3_accum512(uint64_t acc[8], const uint8_t* in, const uint8_t* sec)
{
    int i;
    for (i = 0; i < 8; i++) {
        uint64_t dv = xxh3_load64(in + 8 * i);
        uint64_t dk = dv ^ xxh3_load64(sec + 8 * i);
        acc[i ^ 1] += dv;
        acc[i] += (uint64_t)(uint32_t)dk * (uint64_t)(uint32_t)(dk >> 32);
    }
}

static void xxh3_scramble(uint64_t acc[8], const uint8_t* sec)
{
    int i;
    for (i = 0; i < 8; i++) {
        uint64_t k = xxh3_load64(sec + 8 * i);
        uint64_t a = acc[i];
        a ^= a >> 47;
        a ^= k;
        a *= XXH_PRIME32_1;
        acc[i] = a;
    }
}

static uint64_t xxh3_merge_accs(const uint64_t acc[8], const uint8_t* sec,
    uint64_t start)
{
    uint64_t r = start;
    int i;
    for (i = 0; i < 4; i++)
        r += xxh3_mix16b((const uint8_t*)(acc + 2 * i), sec + 16 * i, 0);
    return xxh3_avalanche(r);
}

static uint64_t xxh3_hashlong(const uint8_t* in, size_t len, uint64_t seed)
{
    uint64_t acc[8] = { XXH_PRIME32_3, XXH_PRIME64_1, XXH_PRIME64_2,
        XXH_PRIME64_3, XXH_PRIME64_4, XXH_PRIME32_2,
        XXH_PRIME64_5, XXH_PRIME32_1 };
    uint8_t secret[XXH3_SECRET_SIZE];
    const int nb_stripes_per_block = (XXH3_SECRET_SIZE - 64) / 8;
    size_t block_len = 64 * (size_t)nb_stripes_per_block;
    size_t nb_blocks = (len - 1) / block_len, b, n;

    if (seed == 0) {
        memcpy(secret, xxh3_ksecret, sizeof secret);
    } else {
        int i;
        for (i = 0; i < XXH3_SECRET_SIZE; i += 16) {
            xxh3_store64(secret + i, xxh3_load64(xxh3_ksecret + i) + seed);
            xxh3_store64(secret + i + 8, xxh3_load64(xxh3_ksecret + i + 8) - seed);
        }
    }
    for (b = 0; b < nb_blocks; b++) {
        for (n = 0; n < (size_t)nb_stripes_per_block; n++)
            xxh3_accum512(acc, in + n * 64, secret + n * 8);
        xxh3_scramble(acc, secret + XXH3_SECRET_SIZE - 64);
        in += block_len;
    }
    for (n = 0; n < ((len - 1) - b * block_len) / 64; n++)
        xxh3_accum512(acc, in + n * 64, secret + n * 8);
    xxh3_accum512(acc, in + len - (b * block_len) - 64,
        secret + XXH3_SECRET_SIZE - 64 - 7);
    return xxh3_merge_accs(acc, secret + 11, len * XXH_PRIME64_1);
}

static uint64_t dyn_xxh3_64(const uint8_t* data, size_t len, uint64_t seed)
{
    if (len == 0)
        return xxh3_avalanche64(seed ^ (xxh3_load64(xxh3_ksecret + 56) ^ xxh3_load64(xxh3_ksecret + 64)));
    if (len <= 3) {
        uint32_t c1 = data[0], c2 = data[len >> 1], c3 = data[len - 1];
        uint32_t combined = (c1 << 16) | (c2 << 24) | c3 | ((uint32_t)len << 8);
        uint64_t keyed = (((uint64_t)(xxh3_load32(xxh3_ksecret) ^ xxh3_load32(xxh3_ksecret + 4))) + seed) ^ (uint64_t)combined;
        return xxh3_avalanche64(keyed);
    }
    if (len <= 8) {
        uint64_t seedmod = seed ^ ((uint64_t)(uint32_t)__builtin_bswap32((uint32_t)seed) << 32);
        uint64_t in1 = xxh3_load32(data);
        uint64_t in2 = xxh3_load32(data + len - 4);
        uint64_t bitflip = (xxh3_load64(xxh3_ksecret + 8) ^ xxh3_load64(xxh3_ksecret + 16)) - seedmod;
        uint64_t keyed = (in2 + (in1 << 32)) ^ bitflip;
        return xxh3_rrmxmx(keyed, (uint64_t)len);
    }
    if (len <= 16) {
        uint64_t bf1 = (xxh3_load64(xxh3_ksecret + 24) ^ xxh3_load64(xxh3_ksecret + 32)) + seed;
        uint64_t bf2 = (xxh3_load64(xxh3_ksecret + 40) ^ xxh3_load64(xxh3_ksecret + 48)) - seed;
        uint64_t in_lo = xxh3_load64(data) ^ bf1;
        uint64_t in_hi = xxh3_load64(data + len - 8) ^ bf2;
        uint64_t acc = (uint64_t)len + __builtin_bswap64(in_lo) + in_hi + xxh3_mul128_fold(in_lo, in_hi);
        return xxh3_avalanche(acc);
    }
    if (len <= 128) {
        uint64_t acc = (uint64_t)len * XXH_PRIME64_1;
        if (len > 32) {
            if (len > 64) {
                if (len > 96) {
                    acc += xxh3_mix16b(data + 48, xxh3_ksecret + 96, seed);
                    acc += xxh3_mix16b(data + len - 64, xxh3_ksecret + 112, seed);
                }
                acc += xxh3_mix16b(data + 32, xxh3_ksecret + 64, seed);
                acc += xxh3_mix16b(data + len - 48, xxh3_ksecret + 80, seed);
            }
            acc += xxh3_mix16b(data + 16, xxh3_ksecret + 32, seed);
            acc += xxh3_mix16b(data + len - 32, xxh3_ksecret + 48, seed);
        }
        acc += xxh3_mix16b(data, xxh3_ksecret, seed);
        acc += xxh3_mix16b(data + len - 16, xxh3_ksecret + 16, seed);
        return xxh3_avalanche(acc);
    }
    if (len <= 240) {
        uint64_t acc = (uint64_t)len * XXH_PRIME64_1;
        size_t nb_rounds = len / 16, i;
        for (i = 0; i < 8; i++)
            acc += xxh3_mix16b(data + 16 * i, xxh3_ksecret + 16 * i, seed);
        acc = xxh3_avalanche(acc);
        for (i = 8; i < nb_rounds; i++)
            acc += xxh3_mix16b(data + 16 * i, xxh3_ksecret + 16 * (i - 8) + 3, seed);
        acc += xxh3_mix16b(data + len - 16, xxh3_ksecret + 136 - 17, seed);
        return xxh3_avalanche(acc);
    }
    return xxh3_hashlong(data, len, seed);
}

static const char* const hash_xx_keys[] = { "as" };

JSValue dyn_hash_xx(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv, int magic)
{
    const uint8_t* data;
    size_t len;
    const char* owned;
    uint8_t* hold;
    int live;
    uint64_t seed = 0;
    char buf[17];
    static const char hexd[] = "0123456789abcdef";
    int as_hex = 1, as_bigint = 0, as_bytes = 0;
    (void)this_val;

    if (argc > 1 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
        double d;
        if (JS_ToFloat64(ctx, &d, argv[1]))
            return JS_EXCEPTION;
        if (!(d >= -9223372036854775808.0 && d < 9223372036854775808.0))
            return JS_ThrowRangeError(ctx,
                "xxhash seed must be a finite number in the int64 range");
        seed = (uint64_t)(int64_t)d;
    }
    if (magic != 1 && argc > 2 && !JS_IsUndefined(argv[2]) && !JS_IsNull(argv[2])) {
        if (dyn_opts_strict(ctx, argv[2], hash_xx_keys, countof(hash_xx_keys)) < 0)
            return JS_EXCEPTION;
        JSValue av = JS_GetPropertyStr(ctx, argv[2], "as");
        if (JS_IsException(av))
            return JS_EXCEPTION;
        if (!JS_IsUndefined(av) && !JS_IsNull(av)) {
            const char* as = JS_ToCString(ctx, av);
            if (!as) {
                JS_FreeValue(ctx, av);
                return JS_EXCEPTION;
            }
            if (!strcmp(as, "hex"))
                as_hex = 1;
            else if (!strcmp(as, "bigint")) {
                as_hex = 0;
                as_bigint = 1;
            } else if (!strcmp(as, "bytes")) {
                as_hex = 0;
                as_bytes = 1;
            } else {
                JS_ThrowTypeError(ctx,
                    "XXHash64: unknown as value \"%s\" (valid: hex, bigint, bytes)", as);
                JS_FreeCString(ctx, as);
                JS_FreeValue(ctx, av);
                return JS_EXCEPTION;
            }
            JS_FreeCString(ctx, as);
        }
        JS_FreeValue(ctx, av);
    }
    if (dyn_crypto_data(ctx, argv[0], &data, &len, &owned))
        return JS_EXCEPTION;
    if (dyn_crypto_private(&live, &data, len, owned, &hold) < 0)
        return JS_ThrowOutOfMemory(ctx);
    if (magic == 1) {
        uint32_t h = DYN_OFFLOAD_CPU_IF(live, dyn_xxh32(data, len, (uint32_t)seed));
        if (hold)
            free(hold);
        if (owned)
            JS_FreeCString(ctx, owned);
        return JS_NewUint32(ctx, h);
    }
    {
        uint64_t h = DYN_OFFLOAD_CPU_IF(live, magic ? dyn_xxh3_64(data, len, seed)
                                           : dyn_xxh64(data, len, seed));
        if (hold)
            free(hold);
        if (owned)
            JS_FreeCString(ctx, owned);
        if (as_bigint)
            return JS_NewBigUint64(ctx, h);
        if (as_bytes) {
            uint8_t le[8];
            int i;
            for (i = 0; i < 8; i++)
                le[i] = (uint8_t)(h >> (8 * i));
            return dyn_nat_u8_copy(ctx, le, 8);
        }
        for (int i = 15; i >= 0; i--) {
            buf[i] = hexd[h & 0xf];
            h >>= 4;
        }
        buf[16] = 0;
        (void)as_hex;
        return JS_NewStringLen(ctx, buf, 16);
    }
}

static int dyn_hash_init_module(JSContext* ctx, JSModuleDef* m)
{
    if (dyn_register_class(ctx, m, &dyn_hasher_class_id, &dyn_hasher_class,
            dyn_hasher_proto, countof(dyn_hasher_proto),
            dyn_hasher_ctor, "Hasher")
        < 0)
        return -1;
    return JS_SetModuleExportList(ctx, m, dyn_hash_funcs,
        countof(dyn_hash_funcs));
}

int js_nat_init_hash(JSContext* ctx)
{
    JSModuleDef* m = JS_NewCModule(ctx, "dyna:hash", dyn_hash_init_module);
    if (!m)
        return -1;
    JS_AddModuleExport(ctx, m, "Hasher");
    return JS_AddModuleExportList(ctx, m, dyn_hash_funcs,
        countof(dyn_hash_funcs));
}

#endif
