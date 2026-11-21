/*
 * test_cryptok_core.c -- src/core/cryptok (BLAKE3, BLAKE2b/2s, Murmur3
 * x64_128, bcrypt/Blowfish, Argon2id and its PHC string) as a standalone
 * C library.
 *
 * Oracles, none derived from the code under test:
 * - BLAKE3: the official test_vectors.json (pinned in tests/test-data.tsv),
 *   hash / keyed_hash / derive_key at every listed length with the full
 *   131-byte XOF output; plus one-shot vs byte-by-byte vs ragged-chunk
 *   updates of a 3 MiB input, which crosses the SIMD-wide and parallel paths.
 * - BLAKE2b / BLAKE2s: a straight RFC 7693 reference written here (also
 *   checked against the RFC's "abc" digests), random lengths, keys and
 *   output sizes.
 * - Murmur3 x64_128: the smhasher MurmurHash3_x64_128 reference written
 *   here, random lengths 0..300 and seeds.
 * - bcrypt: the crypt_blowfish (openwall) $2a$ vectors, including the empty
 *   password and the >72-byte truncation case.
 * - Argon2id: PHC strings produced by argon2-cffi (t/m/p 2/64/1, 3/1024/4
 *   with 16-byte salt, 2/64/2 with hashLen 40) and the RFC/phc-winner
 *   reference vector t=2 m=65536 p=1; the PHC salt/tag base64 round-trips
 *   through arg_b64u_decode.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dyn-cryptok.h"
#include "../lib/dyn-xalloc.h"

static int fails, checks;

#define CHECK(c, ...)                                       \
    do {                                                    \
        checks++;                                           \
        if (!(c)) {                                         \
            fails++;                                        \
            if (fails < 30) {                               \
                printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                printf(__VA_ARGS__);                        \
                printf("\n");                               \
            }                                               \
        }                                                   \
    } while (0)

static uint32_t rs = 99;
static uint32_t rnd(uint32_t n)
{
    rs = rs * 1103515245u + 12345u;
    return (rs >> 8) % n;
}

static void hex(const uint8_t* p, size_t n, char* out)
{
    static const char H[] = "0123456789abcdef";
    size_t i;
    for (i = 0; i < n; i++) {
        out[2 * i] = H[p[i] >> 4];
        out[2 * i + 1] = H[p[i] & 15];
    }
    out[2 * n] = 0;
}

static uint64_t r64(uint64_t x, int n) { return (x >> n) | (x << (64 - n)); }
static uint32_t r32(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static const uint8_t SIGMA[10][16] = {
    { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 },
    { 14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3 },
    { 11, 8, 12, 0, 5, 2, 15, 13, 10, 14, 3, 6, 7, 1, 9, 4 },
    { 7, 9, 3, 1, 13, 12, 11, 14, 2, 6, 5, 10, 4, 0, 15, 8 },
    { 9, 0, 5, 7, 2, 4, 10, 15, 14, 1, 11, 12, 6, 8, 3, 13 },
    { 2, 12, 6, 10, 0, 11, 8, 3, 4, 13, 7, 5, 15, 14, 1, 9 },
    { 12, 5, 1, 15, 14, 13, 4, 10, 0, 7, 6, 3, 9, 2, 8, 11 },
    { 13, 11, 7, 14, 12, 1, 3, 9, 5, 0, 15, 4, 8, 6, 2, 10 },
    { 6, 15, 14, 9, 11, 3, 0, 8, 12, 2, 13, 7, 1, 4, 10, 5 },
    { 10, 2, 8, 4, 7, 6, 1, 5, 15, 11, 9, 14, 3, 12, 13, 0 },
};

static const uint64_t B2B_IV[8] = { 0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
    0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL, 0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL };
static const uint32_t B2S_IV[8] = { 0x6A09E667u, 0xBB67AE85u, 0x3C6EF372u, 0xA54FF53Au, 0x510E527Fu, 0x9B05688Cu, 0x1F83D9ABu, 0x5BE0CD19u };

static void ref_b2b_comp(uint64_t h[8], const uint8_t* blk, uint64_t t, int last)
{
    uint64_t v[16], m[16];
    int i, r;
    for (i = 0; i < 16; i++) {
        int k;
        m[i] = 0;
        for (k = 7; k >= 0; k--)
            m[i] = (m[i] << 8) | blk[8 * i + k];
    }
    for (i = 0; i < 8; i++) {
        v[i] = h[i];
        v[i + 8] = B2B_IV[i];
    }
    v[12] ^= t;
    if (last)
        v[14] = ~v[14];
    for (r = 0; r < 12; r++) {
        const uint8_t* s = SIGMA[r % 10];
        static const int G[8][4] = { { 0, 4, 8, 12 }, { 1, 5, 9, 13 }, { 2, 6, 10, 14 }, { 3, 7, 11, 15 }, { 0, 5, 10, 15 }, { 1, 6, 11, 12 }, { 2, 7, 8, 13 }, { 3, 4, 9, 14 } };
        for (i = 0; i < 8; i++) {
            int a = G[i][0], b = G[i][1], c = G[i][2], d = G[i][3];
            v[a] = v[a] + v[b] + m[s[2 * i]];
            v[d] = r64(v[d] ^ v[a], 32);
            v[c] = v[c] + v[d];
            v[b] = r64(v[b] ^ v[c], 24);
            v[a] = v[a] + v[b] + m[s[2 * i + 1]];
            v[d] = r64(v[d] ^ v[a], 16);
            v[c] = v[c] + v[d];
            v[b] = r64(v[b] ^ v[c], 63);
        }
    }
    for (i = 0; i < 8; i++)
        h[i] ^= v[i] ^ v[i + 8];
}

static void ref_b2b(const uint8_t* p, size_t n, const uint8_t* key, size_t kl, uint8_t* out, size_t ol)
{
    uint64_t h[8];
    uint8_t blk[128];
    uint64_t t = 0;
    size_t i;
    memcpy(h, B2B_IV, sizeof h);
    h[0] ^= 0x01010000ULL ^ ((uint64_t)kl << 8) ^ ol;
    if (kl) {
        memset(blk, 0, 128);
        memcpy(blk, key, kl);
        t = 128;
        ref_b2b_comp(h, blk, t, n == 0);
    }
    if (n || !kl) {
        while (n > 128) {
            t += 128;
            ref_b2b_comp(h, p, t, 0);
            p += 128;
            n -= 128;
        }
        memset(blk, 0, 128);
        memcpy(blk, p, n);
        t += n;
        ref_b2b_comp(h, blk, t, 1);
    }
    for (i = 0; i < ol; i++)
        out[i] = (uint8_t)(h[i / 8] >> (8 * (i % 8)));
}

static void ref_b2s_comp(uint32_t h[8], const uint8_t* blk, uint64_t t, int last)
{
    uint32_t v[16], m[16];
    int i, r;
    for (i = 0; i < 16; i++)
        m[i] = (uint32_t)blk[4 * i] | (uint32_t)blk[4 * i + 1] << 8 | (uint32_t)blk[4 * i + 2] << 16 | (uint32_t)blk[4 * i + 3] << 24;
    for (i = 0; i < 8; i++) {
        v[i] = h[i];
        v[i + 8] = B2S_IV[i];
    }
    v[12] ^= (uint32_t)t;
    v[13] ^= (uint32_t)(t >> 32);
    if (last)
        v[14] = ~v[14];
    for (r = 0; r < 10; r++) {
        const uint8_t* s = SIGMA[r];
        static const int G[8][4] = { { 0, 4, 8, 12 }, { 1, 5, 9, 13 }, { 2, 6, 10, 14 }, { 3, 7, 11, 15 }, { 0, 5, 10, 15 }, { 1, 6, 11, 12 }, { 2, 7, 8, 13 }, { 3, 4, 9, 14 } };
        for (i = 0; i < 8; i++) {
            int a = G[i][0], b = G[i][1], c = G[i][2], d = G[i][3];
            v[a] = v[a] + v[b] + m[s[2 * i]];
            v[d] = r32(v[d] ^ v[a], 16);
            v[c] = v[c] + v[d];
            v[b] = r32(v[b] ^ v[c], 12);
            v[a] = v[a] + v[b] + m[s[2 * i + 1]];
            v[d] = r32(v[d] ^ v[a], 8);
            v[c] = v[c] + v[d];
            v[b] = r32(v[b] ^ v[c], 7);
        }
    }
    for (i = 0; i < 8; i++)
        h[i] ^= v[i] ^ v[i + 8];
}

static void ref_b2s(const uint8_t* p, size_t n, const uint8_t* key, size_t kl, uint8_t* out, size_t ol)
{
    uint32_t h[8];
    uint8_t blk[64];
    uint64_t t = 0;
    size_t i;
    memcpy(h, B2S_IV, sizeof h);
    h[0] ^= 0x01010000u ^ ((uint32_t)kl << 8) ^ (uint32_t)ol;
    if (kl) {
        memset(blk, 0, 64);
        memcpy(blk, key, kl);
        t = 64;
        ref_b2s_comp(h, blk, t, n == 0);
    }
    if (n || !kl) {
        while (n > 64) {
            t += 64;
            ref_b2s_comp(h, p, t, 0);
            p += 64;
            n -= 64;
        }
        memset(blk, 0, 64);
        memcpy(blk, p, n);
        t += n;
        ref_b2s_comp(h, blk, t, 1);
    }
    for (i = 0; i < ol; i++)
        out[i] = (uint8_t)(h[i / 4] >> (8 * (i % 4)));
}

static uint64_t rotl(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }
static uint64_t fmix(uint64_t k)
{
    k ^= k >> 33;
    k *= 0xff51afd7ed558ccdULL;
    k ^= k >> 33;
    k *= 0xc4ceb9fe1a85ec53ULL;
    k ^= k >> 33;
    return k;
}

static void ref_mm3(const uint8_t* data, size_t len, uint32_t seed, uint8_t out[16])
{
    const size_t nblocks = len / 16;
    uint64_t h1 = seed, h2 = seed, k1, k2;
    const uint64_t c1 = 0x87c37b91114253d5ULL, c2 = 0x4cf5ad432745937fULL;
    const uint8_t* tail;
    size_t i;
    int b;
    for (i = 0; i < nblocks; i++) {
        k1 = 0;
        k2 = 0;
        for (b = 7; b >= 0; b--) {
            k1 = (k1 << 8) | data[i * 16 + (size_t)b];
            k2 = (k2 << 8) | data[i * 16 + 8 + (size_t)b];
        }
        k1 *= c1;
        k1 = rotl(k1, 31);
        k1 *= c2;
        h1 ^= k1;
        h1 = rotl(h1, 27);
        h1 += h2;
        h1 = h1 * 5 + 0x52dce729;
        k2 *= c2;
        k2 = rotl(k2, 33);
        k2 *= c1;
        h2 ^= k2;
        h2 = rotl(h2, 31);
        h2 += h1;
        h2 = h2 * 5 + 0x38495ab5;
    }
    tail = data + nblocks * 16;
    k1 = 0;
    k2 = 0;
    for (b = (int)(len & 15) - 1; b >= 8; b--)
        k2 = (k2 << 8) | tail[b];
    if ((len & 15) > 8) {
        k2 *= c2;
        k2 = rotl(k2, 33);
        k2 *= c1;
        h2 ^= k2;
    }
    for (b = ((int)(len & 15) > 8 ? 8 : (int)(len & 15)) - 1; b >= 0; b--)
        k1 = (k1 << 8) | tail[b];
    if (len & 15) {
        k1 *= c1;
        k1 = rotl(k1, 31);
        k1 *= c2;
        h1 ^= k1;
    }
    h1 ^= len;
    h2 ^= len;
    h1 += h2;
    h2 += h1;
    h1 = fmix(h1);
    h2 = fmix(h2);
    h1 += h2;
    h2 += h1;
    for (b = 0; b < 8; b++) {
        out[b] = (uint8_t)(h1 >> (8 * b));
        out[8 + b] = (uint8_t)(h2 >> (8 * b));
    }
}

static void test_blake2_murmur(void)
{
    static uint8_t buf[2000], key[64], a[64], b[64];
    char hx[200];
    int it;
    size_t i;
    for (i = 0; i < sizeof buf; i++)
        buf[i] = (uint8_t)rnd(256);
    bl2b_hash_key((const uint8_t*)"abc", 3, a, 64, NULL, 0);
    hex(a, 64, hx);
    CHECK(strcmp(hx, "ba80a53f981c4d0d6a2797b69f12f6e94c212f14685ac4b74b12bb6fdbffa2d17d87c5392aab792dc252d5de4533cc9518d38aa8dbf1925ab92386edd4009923") == 0, "BLAKE2b-512(abc) RFC 7693: %s", hx);
    bl2s_hash_key((const uint8_t*)"abc", 3, a, 32, NULL, 0);
    hex(a, 32, hx);
    CHECK(strcmp(hx, "508c5e8c327c14e2e1a72ba34eeb452f37458b209ed63a294d999b4c86675982") == 0, "BLAKE2s-256(abc) RFC 7693: %s", hx);
    for (it = 0; it < 3000; it++) {
        size_t n = rnd(4) == 0 ? rnd(2000) : rnd(300), kl = rnd(3) == 0 ? 0 : 1 + rnd(64), ol = 1 + rnd(64);
        for (i = 0; i < kl; i++)
            key[i] = (uint8_t)rnd(256);
        bl2b_hash_key(buf, n, a, ol, kl ? key : NULL, kl);
        ref_b2b(buf, n, key, kl, b, ol);
        CHECK(memcmp(a, b, ol) == 0, "blake2b n %zu key %zu out %zu", n, kl, ol);
        kl = kl > 32 ? 32 : kl;
        ol = 1 + rnd(32);
        bl2s_hash_key(buf, n, a, ol, kl ? key : NULL, kl);
        ref_b2s(buf, n, key, kl, b, ol);
        CHECK(memcmp(a, b, ol) == 0, "blake2s n %zu key %zu out %zu", n, kl, ol);
        {
            uint32_t seed = rnd(3) ? rnd(1u << 30) : 0;
            n = rnd(300);
            mm3_128(buf, n, seed, a);
            ref_mm3(buf, n, seed, b);
            CHECK(memcmp(a, b, 16) == 0, "murmur3 n %zu seed %u", n, seed);
        }
    }
}

static int unhex(const char* s, size_t n, uint8_t* out)
{
    size_t i;
    for (i = 0; i < n; i++) {
        unsigned v;
        if (sscanf(s + 2 * i, "%2x", &v) != 1)
            return -1;
        out[i] = (uint8_t)v;
    }
    return 0;
}

static const char* field(const char* p, const char* name, size_t* len)
{
    const char* q = strstr(p, name);
    const char* e;
    if (!q)
        return NULL;
    q = strchr(q + strlen(name), '"');
    if (!q)
        return NULL;
    q++;
    e = strchr(q, '"');
    *len = (size_t)(e - q);
    return q;
}

static void test_blake3(void)
{
    FILE* f = fopen("tests/crypto/blake3_official_vectors.json", "rb");
    char* js;
    long fl;
    const char *p, *k, *ctxs;
    size_t kn = 0, cn = 0, ncase = 0;
    uint8_t *in, out[131], want[131];
    CHECK(f != NULL, "blake3 vectors file present (tests/test-data.tsv)");
    if (!f)
        return;
    fseek(f, 0, SEEK_END);
    fl = ftell(f);
    fseek(f, 0, SEEK_SET);
    js = malloc((size_t)fl + 1);
    if (fread(js, 1, (size_t)fl, f) != (size_t)fl)
        fl = 0;
    js[fl] = 0;
    fclose(f);
    k = field(js, "\"key\"", &kn);
    ctxs = field(js, "\"context_string\"", &cn);
    in = xmalloc(110000);
    {
        size_t i;
        for (i = 0; i < 110000; i++)
            in[i] = (uint8_t)(i % 251);
    }
    for (p = strstr(js, "\"input_len\""); p; p = strstr(p + 1, "\"input_len\"")) {
        size_t n = (size_t)strtoul(strchr(p, ':') + 1, NULL, 10), hl;
        const char* h;
        bl3_hasher_t st;
        uint8_t ck[32];
        ncase++;
        h = field(p, "\"hash\"", &hl);
        unhex(h, 131, want);
        bl3_init(&st);
        bl3_update(&st, in, n);
        bl3_final(&st, out, 131);
        CHECK(memcmp(out, want, 131) == 0, "blake3 hash len %zu", n);
        h = field(p, "\"keyed_hash\"", &hl);
        unhex(h, 131, want);
        bl3_init_keyed(&st, (const uint8_t*)k, BL3_KEYED);
        bl3_update(&st, in, n);
        bl3_final(&st, out, 131);
        CHECK(kn == 32 && memcmp(out, want, 131) == 0, "blake3 keyed len %zu", n);
        h = field(p, "\"derive_key\"", &hl);
        unhex(h, 131, want);
        bl3_init_flags(&st, BL3_DERIVE_CTX);
        bl3_update(&st, (const uint8_t*)ctxs, cn);
        bl3_final(&st, ck, 32);
        bl3_init_keyed(&st, ck, BL3_DERIVE_MAT);
        bl3_update(&st, in, n);
        bl3_final(&st, out, 131);
        CHECK(memcmp(out, want, 131) == 0, "blake3 derive len %zu", n);
    }
    CHECK(ncase >= 30, "blake3 vectors parsed: %zu cases", ncase);
    free(in);
    free(js);
    {
        size_t N = 3u << 20, i, off;
        uint8_t* big = xmalloc(N);
        uint8_t a[64], b[64], c[64];
        bl3_hasher_t st;
        for (i = 0; i < N; i++)
            big[i] = (uint8_t)rnd(256);
        bl3_init(&st);
        bl3_update(&st, big, N);
        bl3_final(&st, a, 64);
        bl3_init(&st);
        for (off = 0; off < N;) {
            size_t step = 1 + rnd(9000);
            if (step > N - off)
                step = N - off;
            bl3_update(&st, big + off, step);
            off += step;
        }
        bl3_final(&st, b, 64);
        bl3_init(&st);
        for (off = 0; off < 300000; off++)
            bl3_update(&st, big + off, 1);
        bl3_update(&st, big + 300000, N - 300000);
        bl3_final(&st, c, 64);
        CHECK(memcmp(a, b, 64) == 0 && memcmp(a, c, 64) == 0, "blake3 3MiB one-shot == ragged == bytewise");
        free(big);
    }
}

static void test_bcrypt(void)
{
    static const char* const V[][2] = {
        { "$2a$05$CCCCCCCCCCCCCCCCCCCCC.E5YPO9kmyuRGyh0XouQYb4YMJKvyOeW", "U*U" },
        { "$2a$05$CCCCCCCCCCCCCCCCCCCCC.VGOzA784oUp/Z0DY336zx7pLYAy0lwK", "U*U*" },
        { "$2a$05$XXXXXXXXXXXXXXXXXXXXXOAcXxm9kjPGEMsLznoKqmqw7tc8WCx4a", "U*U*U" },
        { "$2a$05$abcdefghijklmnopqrstuu5s2v8.iXieOjg/.AySBTTZIIVFJeBui", "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789chars after 72 are ignored" },
        { "$2a$05$CCCCCCCCCCCCCCCCCCCCC.7uG0VCzI2bS7j6ymqJi9CdcdxiRTWNy", "" },
    };
    size_t i;
    for (i = 0; i < sizeof V / sizeof V[0]; i++) {
        uint32_t logr = 0;
        uint8_t salt[16];
        char out[64];
        int rc = dyn_bcrypt_parse(V[i][0], strlen(V[i][0]), &logr, salt);
        CHECK(rc == 0 && logr == 5, "bcrypt parse %zu", i);
        rc = dyn_bcrypt_core((const uint8_t*)V[i][1], strlen(V[i][1]), salt, logr, out);
        CHECK(rc == 0 && strncmp(out, "$2b$05$", 7) == 0 && strcmp(out + 7, V[i][0] + 7) == 0, "bcrypt '%s': %s", V[i][1], out);
    }
    {
        uint8_t salt[16] = { 0 };
        char out[64];
        CHECK(dyn_bcrypt_core((const uint8_t*)"x", 1, salt, 3, out) == -1, "bcrypt cost 3 rejected");
        CHECK(dyn_bcrypt_parse("$2a$21$CCCCCCCCCCCCCCCCCCCCC.E5YPO9kmyuRGyh0XouQYb4YMJKvyOeW", 60, (uint32_t[]) { 0 }, salt) == -1, "bcrypt verify cost cap");
        CHECK(dyn_bcrypt_parse("$2c$05$CCCCCCCCCCCCCCCCCCCCC.E5YPO9kmyuRGyh0XouQYb4YMJKvyOeW", 60, (uint32_t[]) { 0 }, salt) == -1, "bcrypt $2c rejected");
    }
}

static void test_argon2(void)
{
    static const struct {
        const char* pwd;
        const char* phc;
        const uint8_t* salt;
        size_t saltlen, taglen;
        uint32_t t, m, p;
    } V[] = {
        { "password", "$argon2id$v=19$m=64,t=2,p=1$c29tZXNhbHQ$FqGkmHNGCd0BRW2kBt6fPZ2pPmyGwwChL8FGUhTOSSI", (const uint8_t*)"somesalt", 8, 32, 2, 64, 1 },
        { "correct horse battery staple", "$argon2id$v=19$m=1024,t=3,p=4$AAECAwQFBgcICQoLDA0ODw$AKJWSd3f4iLMxeJ0xuAYJDdnVESfcRgtRQhKAg87Xhk", (const uint8_t*)"\x00\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0a\x0b\x0c\x0d\x0e\x0f", 16, 32, 3, 1024, 4 },
        { "password", "$argon2id$v=19$m=64,t=2,p=2$b3RoZXJzYWx0$sOfolfj3a7lpw7yKtizMg+WcWERTwS5MFNAzHiaM/QEaabSz9lU8ew", (const uint8_t*)"othersalt", 9, 40, 2, 64, 2 },
        { "password", "$argon2id$v=19$m=65536,t=2,p=1$c29tZXNhbHQ$CTFhFdXPJO1aFaMaO6Mm5c8y7cJHAph8ArZWb2GRPPc", (const uint8_t*)"somesalt", 8, 32, 2, 65536, 1 },
    };
    size_t i;
    for (i = 0; i < sizeof V / sizeof V[0]; i++) {
        uint8_t tag[64], dec[64];
        char* s;
        const char *ps, *pt;
        size_t psl, ptl, dn = 0;
        uint32_t t, m, p;
        int rc = arg_hash_id((const uint8_t*)V[i].pwd, strlen(V[i].pwd), V[i].salt, V[i].saltlen, V[i].t, V[i].m, V[i].p, tag, V[i].taglen);
        CHECK(rc == 0, "argon2 %zu rc %d", i, rc);
        s = arg_phc_encode(V[i].salt, V[i].saltlen, tag, V[i].taglen, V[i].t, V[i].m, V[i].p);
        CHECK(s && strcmp(s, V[i].phc) == 0, "argon2id PHC %zu: %s", i, s ? s : "(null)");
        free(s);
        rc = arg_phc_parse(V[i].phc, &ps, &psl, &pt, &ptl, &t, &m, &p);
        CHECK(rc == 0 && t == V[i].t && m == V[i].m && p == V[i].p, "argon2 parse %zu", i);
        CHECK(rc == 0 && arg_b64u_decode(pt, ptl, dec, &dn) == 0 && dn == V[i].taglen && memcmp(dec, tag, dn) == 0, "argon2 tag base64 round trip %zu", i);
        CHECK(rc == 0 && arg_b64u_decode(ps, psl, dec, &dn) == 0 && dn == V[i].saltlen && memcmp(dec, V[i].salt, dn) == 0, "argon2 salt base64 round trip %zu", i);
    }
}

int main(void)
{
    test_blake2_murmur();
    test_blake3();
    test_bcrypt();
    test_argon2();
    printf("test_cryptok_core: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
