#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_WIREGUARD)

#include "wg-internal.h"

#include <string.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <pthread.h>

static const uint32_t b2s_iv[8] = { 0x6A09E667u, 0xBB67AE85u, 0x3C6EF372u, 0xA54FF53Au, 0x510E527Fu, 0x9B05688Cu, 0x1F83D9ABu, 0x5BE0CD19u };

static const uint8_t b2s_sigma[10][16] = {
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

uint32_t wg_le32(const uint8_t* p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

void wg_put_le32(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

uint64_t wg_le64(const uint8_t* p)
{
    return (uint64_t)wg_le32(p) | (uint64_t)wg_le32(p + 4) << 32;
}

void wg_put_le64(uint8_t* p, uint64_t v)
{
    wg_put_le32(p, (uint32_t)v);
    wg_put_le32(p + 4, (uint32_t)(v >> 32));
}

void wg_wipe(void* p, size_t n)
{
    OPENSSL_cleanse(p, n);
}

static uint32_t rotr32(uint32_t x, unsigned n)
{
    return x >> n | x << (32 - n);
}

static void b2s_compress(wg_b2s_t* s, const uint8_t* block, int last)
{
    uint32_t m[16], v[16];
    int i, r;
    for (i = 0; i < 16; i++)
        m[i] = wg_le32(block + 4 * i);
    for (i = 0; i < 8; i++) {
        v[i] = s->h[i];
        v[i + 8] = b2s_iv[i];
    }
    v[12] ^= s->t[0];
    v[13] ^= s->t[1];
    if (last)
        v[14] = ~v[14];
    for (r = 0; r < 10; r++) {
        const uint8_t* g = b2s_sigma[r];
        static const uint8_t ix[8][4] = { { 0, 4, 8, 12 }, { 1, 5, 9, 13 }, { 2, 6, 10, 14 }, { 3, 7, 11, 15 }, { 0, 5, 10, 15 }, { 1, 6, 11, 12 }, { 2, 7, 8, 13 }, { 3, 4, 9, 14 } };
        for (i = 0; i < 8; i++) {
            uint32_t *a = &v[ix[i][0]], *b = &v[ix[i][1]], *c = &v[ix[i][2]], *d = &v[ix[i][3]];
            *a = *a + *b + m[g[2 * i]];
            *d = rotr32(*d ^ *a, 16);
            *c = *c + *d;
            *b = rotr32(*b ^ *c, 12);
            *a = *a + *b + m[g[2 * i + 1]];
            *d = rotr32(*d ^ *a, 8);
            *c = *c + *d;
            *b = rotr32(*b ^ *c, 7);
        }
    }
    for (i = 0; i < 8; i++)
        s->h[i] ^= v[i] ^ v[i + 8];
    wg_wipe(m, sizeof m);
    wg_wipe(v, sizeof v);
}

static void b2s_count(wg_b2s_t* s, uint32_t n)
{
    s->t[0] += n;
    if (s->t[0] < n)
        s->t[1]++;
}

void wg_b2s_init(wg_b2s_t* s, size_t outlen, const uint8_t* key, size_t keylen)
{
    int i;
    memset(s, 0, sizeof *s);
    for (i = 0; i < 8; i++)
        s->h[i] = b2s_iv[i];
    s->h[0] ^= 0x01010000u ^ (uint32_t)(keylen << 8) ^ (uint32_t)outlen;
    s->outlen = outlen;
    if (keylen) {
        memcpy(s->buf, key, keylen);
        s->buflen = 64;
    }
}

void wg_b2s_update(wg_b2s_t* s, const uint8_t* p, size_t n)
{
    while (n) {
        size_t take;
        if (s->buflen == 64) {
            b2s_count(s, 64);
            b2s_compress(s, s->buf, 0);
            s->buflen = 0;
        }
        take = 64 - s->buflen;
        if (take > n)
            take = n;
        memcpy(s->buf + s->buflen, p, take);
        s->buflen += take;
        p += take;
        n -= take;
    }
}

void wg_b2s_final(wg_b2s_t* s, uint8_t* out)
{
    uint8_t full[32];
    int i;
    b2s_count(s, (uint32_t)s->buflen);
    memset(s->buf + s->buflen, 0, 64 - s->buflen);
    b2s_compress(s, s->buf, 1);
    for (i = 0; i < 8; i++)
        wg_put_le32(full + 4 * i, s->h[i]);
    memcpy(out, full, s->outlen);
    wg_wipe(full, sizeof full);
    wg_wipe(s, sizeof *s);
}

void wg_blake2s(uint8_t* out, size_t outlen, const uint8_t* key, size_t keylen, const uint8_t* in, size_t n)
{
    wg_b2s_t s;
    wg_b2s_init(&s, outlen, key, keylen);
    wg_b2s_update(&s, in, n);
    wg_b2s_final(&s, out);
}

void wg_hash2(uint8_t out[WG_KEY], const uint8_t* a, size_t an, const uint8_t* b, size_t bn)
{
    wg_b2s_t s;
    wg_b2s_init(&s, WG_KEY, NULL, 0);
    wg_b2s_update(&s, a, an);
    wg_b2s_update(&s, b, bn);
    wg_b2s_final(&s, out);
}

void wg_hmac(uint8_t out[WG_KEY], const uint8_t* key, size_t kn, const uint8_t* in, size_t n)
{
    uint8_t k[64], pad[64], inner[WG_KEY];
    wg_b2s_t s;
    int i;
    memset(k, 0, sizeof k);
    if (kn > 64)
        wg_blake2s(k, WG_KEY, NULL, 0, key, kn);
    else
        memcpy(k, key, kn);
    for (i = 0; i < 64; i++)
        pad[i] = k[i] ^ 0x36;
    wg_b2s_init(&s, WG_KEY, NULL, 0);
    wg_b2s_update(&s, pad, 64);
    wg_b2s_update(&s, in, n);
    wg_b2s_final(&s, inner);
    for (i = 0; i < 64; i++)
        pad[i] = k[i] ^ 0x5c;
    wg_b2s_init(&s, WG_KEY, NULL, 0);
    wg_b2s_update(&s, pad, 64);
    wg_b2s_update(&s, inner, WG_KEY);
    wg_b2s_final(&s, out);
    wg_wipe(k, sizeof k);
    wg_wipe(pad, sizeof pad);
    wg_wipe(inner, sizeof inner);
}

void wg_kdf(uint8_t* t1, uint8_t* t2, uint8_t* t3, const uint8_t ck[WG_KEY], const uint8_t* in, size_t n)
{
    uint8_t t0[WG_KEY], buf[WG_KEY + 1], prev[WG_KEY];
    wg_hmac(t0, ck, WG_KEY, in, n);
    buf[0] = 1;
    wg_hmac(prev, t0, WG_KEY, buf, 1);
    if (t1)
        memcpy(t1, prev, WG_KEY);
    if (t2 || t3) {
        memcpy(buf, prev, WG_KEY);
        buf[WG_KEY] = 2;
        wg_hmac(prev, t0, WG_KEY, buf, WG_KEY + 1);
        if (t2)
            memcpy(t2, prev, WG_KEY);
    }
    if (t3) {
        memcpy(buf, prev, WG_KEY);
        buf[WG_KEY] = 3;
        wg_hmac(t3, t0, WG_KEY, buf, WG_KEY + 1);
    }
    wg_wipe(t0, sizeof t0);
    wg_wipe(buf, sizeof buf);
    wg_wipe(prev, sizeof prev);
}

void wg_mac(uint8_t out[WG_MACLEN], const uint8_t* key, size_t kn, const uint8_t* in, size_t n)
{
    wg_blake2s(out, WG_MACLEN, key, kn, in, n);
}

#define QR(a, b, c, d)                 \
    a += b, d = rotr32(d ^ a, 16);     \
    c += d, b = rotr32(b ^ c, 20);     \
    a += b, d = rotr32(d ^ a, 24);     \
    c += d, b = rotr32(b ^ c, 25)

void wg_hchacha20(uint8_t out[WG_KEY], const uint8_t key[WG_KEY], const uint8_t nonce[16])
{
    uint32_t x[16];
    int i;
    x[0] = 0x61707865u;
    x[1] = 0x3320646eu;
    x[2] = 0x79622d32u;
    x[3] = 0x6b206574u;
    for (i = 0; i < 8; i++)
        x[4 + i] = wg_le32(key + 4 * i);
    for (i = 0; i < 4; i++)
        x[12 + i] = wg_le32(nonce + 4 * i);
    for (i = 0; i < 10; i++) {
        QR(x[0], x[4], x[8], x[12]);
        QR(x[1], x[5], x[9], x[13]);
        QR(x[2], x[6], x[10], x[14]);
        QR(x[3], x[7], x[11], x[15]);
        QR(x[0], x[5], x[10], x[15]);
        QR(x[1], x[6], x[11], x[12]);
        QR(x[2], x[7], x[8], x[13]);
        QR(x[3], x[4], x[9], x[14]);
    }
    for (i = 0; i < 4; i++) {
        wg_put_le32(out + 4 * i, x[i]);
        wg_put_le32(out + 16 + 4 * i, x[12 + i]);
    }
    wg_wipe(x, sizeof x);
}

static pthread_once_t aead_once = PTHREAD_ONCE_INIT;
static pthread_key_t aead_key;
static EVP_CIPHER* aead_cipher;

static void aead_ctx_free(void* p)
{
    EVP_CIPHER_CTX_free((EVP_CIPHER_CTX*)p);
}

static void aead_setup(void)
{
    aead_cipher = EVP_CIPHER_fetch(NULL, "ChaCha20-Poly1305", NULL);
    if (pthread_key_create(&aead_key, aead_ctx_free) != 0)
        aead_cipher = NULL;
}

static EVP_CIPHER_CTX* aead_ctx(int* fresh)
{
    EVP_CIPHER_CTX* c;
    pthread_once(&aead_once, aead_setup);
    if (!aead_cipher)
        return NULL;
    c = (EVP_CIPHER_CTX*)pthread_getspecific(aead_key);
    *fresh = 0;
    if (!c) {
        c = EVP_CIPHER_CTX_new();
        if (!c || pthread_setspecific(aead_key, c) != 0) {
            EVP_CIPHER_CTX_free(c);
            return NULL;
        }
        *fresh = 1;
    }
    return c;
}

static int aead(int enc, const uint8_t key[WG_KEY], const uint8_t iv[12], const uint8_t* in, size_t n, const uint8_t* ad, size_t adn, uint8_t* out)
{
    EVP_CIPHER_CTX* c;
    int ol, ok, fresh;
    size_t body = enc ? n : n - WG_TAG;
    if ((!enc && n < WG_TAG) || body > 0x7fffffffu || adn > 0x7fffffffu)
        return -1;
    c = aead_ctx(&fresh);
    if (!c)
        return -1;
    if (EVP_CipherInit_ex2(c, fresh ? aead_cipher : NULL, key, iv, enc, NULL) != 1) {
        pthread_setspecific(aead_key, NULL);
        EVP_CIPHER_CTX_free(c);
        return -1;
    }
    ok = 1;
    if (ok && !enc)
        ok = EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_AEAD_SET_TAG, WG_TAG, (void*)(uintptr_t)(in + body)) == 1;
    if (ok && adn)
        ok = EVP_CipherUpdate(c, NULL, &ol, ad, (int)adn) == 1;
    if (ok && body)
        ok = EVP_CipherUpdate(c, out, &ol, in, (int)body) == 1;
    if (ok)
        ok = EVP_CipherFinal_ex(c, out + body, &ol) == 1;
    if (ok && enc)
        ok = EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_AEAD_GET_TAG, WG_TAG, out + body) == 1;
    if (!ok && !enc && body)
        wg_wipe(out, body);
    return ok ? 0 : -1;
}

static void ctr_iv(uint8_t iv[12], uint64_t ctr)
{
    memset(iv, 0, 4);
    wg_put_le64(iv + 4, ctr);
}

int wg_aead_seal(const uint8_t key[WG_KEY], uint64_t ctr, const uint8_t* pt, size_t n, const uint8_t* ad, size_t adn, uint8_t* out)
{
    uint8_t iv[12];
    ctr_iv(iv, ctr);
    return aead(1, key, iv, pt, n, ad, adn, out);
}

int wg_aead_open(const uint8_t key[WG_KEY], uint64_t ctr, const uint8_t* ct, size_t n, const uint8_t* ad, size_t adn, uint8_t* out)
{
    uint8_t iv[12];
    ctr_iv(iv, ctr);
    return aead(0, key, iv, ct, n, ad, adn, out);
}

static int xaead(int enc, const uint8_t key[WG_KEY], const uint8_t nonce[24], const uint8_t* in, size_t n, const uint8_t* ad, size_t adn, uint8_t* out)
{
    uint8_t sub[WG_KEY], iv[12];
    int rc;
    wg_hchacha20(sub, key, nonce);
    memset(iv, 0, 4);
    memcpy(iv + 4, nonce + 16, 8);
    rc = aead(enc, sub, iv, in, n, ad, adn, out);
    wg_wipe(sub, sizeof sub);
    return rc;
}

int wg_xaead_seal(const uint8_t key[WG_KEY], const uint8_t nonce[24], const uint8_t* pt, size_t n, const uint8_t* ad, size_t adn, uint8_t* out)
{
    return xaead(1, key, nonce, pt, n, ad, adn, out);
}

int wg_xaead_open(const uint8_t key[WG_KEY], const uint8_t nonce[24], const uint8_t* ct, size_t n, const uint8_t* ad, size_t adn, uint8_t* out)
{
    return xaead(0, key, nonce, ct, n, ad, adn, out);
}

static int x25519_with(uint8_t out[WG_KEY], EVP_PKEY* a, const uint8_t pub[WG_KEY])
{
    static const uint8_t zero[WG_KEY];
    EVP_PKEY* b = EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, NULL, pub, WG_KEY);
    EVP_PKEY_CTX* c = a && b ? EVP_PKEY_CTX_new(a, NULL) : NULL;
    size_t ol = WG_KEY;
    int ok = c && EVP_PKEY_derive_init(c) == 1 && EVP_PKEY_derive_set_peer(c, b) == 1 && EVP_PKEY_derive(c, out, &ol) == 1 && ol == WG_KEY;
    EVP_PKEY_CTX_free(c);
    EVP_PKEY_free(b);
    if (ok && CRYPTO_memcmp(out, zero, WG_KEY) == 0)
        ok = 0;
    if (!ok)
        wg_wipe(out, WG_KEY);
    return ok ? 0 : -1;
}

int wg_x25519(uint8_t out[WG_KEY], const uint8_t priv[WG_KEY], const uint8_t pub[WG_KEY])
{
    EVP_PKEY* a = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, NULL, priv, WG_KEY);
    int rc = x25519_with(out, a, pub);
    EVP_PKEY_free(a);
    return rc;
}

int wg_x25519_pk(uint8_t out[WG_KEY], const void* pk, const uint8_t pub[WG_KEY])
{
    return x25519_with(out, (EVP_PKEY*)(uintptr_t)pk, pub);
}

int wg_x25519_static(uint8_t out[WG_KEY], const wg_static_t* s, const uint8_t pub[WG_KEY])
{
    return wg_x25519_pk(out, s->pk, pub);
}

void* wg_pkey_new(const uint8_t priv[WG_KEY])
{
    return EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, NULL, priv, WG_KEY);
}

void wg_pkey_free(void* pk)
{
    EVP_PKEY_free((EVP_PKEY*)pk);
}

void* wg_pkey_up(void* pk)
{
    return pk && EVP_PKEY_up_ref((EVP_PKEY*)pk) == 1 ? pk : NULL;
}

int wg_x25519_pub(uint8_t pub[WG_KEY], const uint8_t priv[WG_KEY])
{
    EVP_PKEY* a = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, NULL, priv, WG_KEY);
    size_t ol = WG_KEY;
    int ok = a && EVP_PKEY_get_raw_public_key(a, pub, &ol) == 1 && ol == WG_KEY;
    EVP_PKEY_free(a);
    return ok ? 0 : -1;
}

int wg_random(uint8_t* p, size_t n)
{
    return n > 0x7fffffffu || RAND_bytes(p, (int)n) != 1 ? -1 : 0;
}

void* wg_keygen_pk(uint8_t priv[WG_KEY], uint8_t pub[WG_KEY])
{
    EVP_PKEY* a;
    size_t ol = WG_KEY;
    if (wg_random(priv, WG_KEY) < 0)
        return NULL;
    priv[0] &= 248;
    priv[31] = (uint8_t)((priv[31] & 127) | 64);
    a = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, NULL, priv, WG_KEY);
    if (a && (EVP_PKEY_get_raw_public_key(a, pub, &ol) != 1 || ol != WG_KEY)) {
        EVP_PKEY_free(a);
        a = NULL;
    }
    if (!a)
        wg_wipe(priv, WG_KEY);
    return a;
}

int wg_keygen(uint8_t priv[WG_KEY], uint8_t pub[WG_KEY])
{
    EVP_PKEY* a = (EVP_PKEY*)wg_keygen_pk(priv, pub);
    EVP_PKEY_free(a);
    return a ? 0 : -1;
}

void wg_tai64n(uint8_t out[12], uint64_t unix_ns)
{
    uint64_t sec = unix_ns / 1000000000u + 0x400000000000000aULL;
    uint32_t ns = (uint32_t)(unix_ns % 1000000000u) & ~((1u << 24) - 1u);
    int i;
    for (i = 0; i < 8; i++)
        out[i] = (uint8_t)(sec >> (56 - 8 * i));
    out[8] = (uint8_t)(ns >> 24);
    out[9] = (uint8_t)(ns >> 16);
    out[10] = (uint8_t)(ns >> 8);
    out[11] = (uint8_t)ns;
}

#endif
