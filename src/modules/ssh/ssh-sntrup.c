#include "ssh-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_SSH)

#include <openssl/crypto.h>

#define P 761
#define Q 4591
#define W 286
#define Q12 ((Q - 1) / 2)
#define SMALL_BYTES ((P + 3) / 4)
#define RQ_BYTES 1158
#define ROUNDED_BYTES 1007
#define HASH_BYTES 32
#define SK_FIXED (2 * SMALL_BYTES)

typedef int8_t small;
typedef int16_t Fq;

static void u32_divmod_u14(uint32_t* qo, uint16_t* r, uint32_t x, uint16_t m)
{
    uint32_t v = 0x80000000u, qpart, mask, qq = 0;
    v /= m;
    qpart = (uint32_t)(((uint64_t)x * v) >> 31);
    x -= qpart * m;
    qq += qpart;
    qpart = (uint32_t)(((uint64_t)x * v) >> 31);
    x -= qpart * m;
    qq += qpart;
    x -= m;
    qq += 1;
    mask = (uint32_t)0 - (x >> 31);
    x += mask & (uint32_t)m;
    qq += mask;
    *qo = qq;
    *r = (uint16_t)x;
}

static uint16_t u32_mod_u14(uint32_t x, uint16_t m)
{
    uint32_t qo;
    uint16_t r;
    u32_divmod_u14(&qo, &r, x, m);
    return r;
}

static uint16_t i32_mod_u14(int32_t x, uint16_t m)
{
    uint32_t uq, uq2;
    uint16_t ur, ur2;
    uint32_t mask;
    u32_divmod_u14(&uq, &ur, 0x80000000u + (uint32_t)x, m);
    u32_divmod_u14(&uq2, &ur2, 0x80000000u, m);
    ur = (uint16_t)(ur - ur2);
    mask = (uint32_t)0 - (uint32_t)(ur >> 15);
    ur = (uint16_t)(ur + (mask & m));
    return ur;
}

static int nonzero_mask(int16_t x)
{
    uint16_t u = (uint16_t)x;
    uint32_t v = u;
    v = (uint32_t)0 - v;
    v >>= 31;
    return -(int)v;
}

static int negative_mask(int16_t x)
{
    uint16_t u = (uint16_t)x;
    u >>= 15;
    return -(int)u;
}

static small f3_freeze(int16_t x)
{
    return (small)((int)i32_mod_u14((int32_t)x + 1, 3) - 1);
}

static Fq fq_freeze(int32_t x)
{
    return (Fq)((int)i32_mod_u14(x + Q12, Q) - Q12);
}

static Fq fq_recip(Fq a1)
{
    int i = 1;
    Fq ai = a1;
    while (i < Q - 2) {
        ai = fq_freeze(a1 * (int32_t)ai);
        i += 1;
    }
    return ai;
}

static void minmax32(uint32_t* a, uint32_t* b)
{
    uint32_t x = *a, y = *b;
    uint32_t mask = (uint32_t)0 - (uint32_t)(((uint64_t)y - (uint64_t)x) >> 63);
    uint32_t t = (x ^ y) & mask;
    *a = x ^ t;
    *b = y ^ t;
}

static void sort_u32(uint32_t* x)
{
    uint32_t v[1024];
    size_t k, j, i;
    memcpy(v, x, P * sizeof *x);
    for (i = P; i < 1024; i++)
        v[i] = 0xffffffffu;
    for (k = 2; k <= 1024; k <<= 1)
        for (j = k >> 1; j > 0; j >>= 1)
            for (i = 0; i < 1024; i++) {
                size_t l = i ^ j;
                if (l > i) {
                    if ((i & k) == 0)
                        minmax32(&v[i], &v[l]);
                    else
                        minmax32(&v[l], &v[i]);
                }
            }
    memcpy(x, v, P * sizeof *x);
    OPENSSL_cleanse(v, sizeof v);
}

static uint32_t urandom32(void)
{
    uint8_t c[4];
    ssh_random(c, 4);
    return (uint32_t)c[0] | (uint32_t)c[1] << 8 | (uint32_t)c[2] << 16 | (uint32_t)c[3] << 24;
}

static void short_random(small* out)
{
    uint32_t L[P];
    int i;
    for (i = 0; i < P; i++)
        L[i] = urandom32();
    for (i = 0; i < W; i++)
        L[i] = L[i] & (uint32_t)-2;
    for (i = W; i < P; i++)
        L[i] = (L[i] & (uint32_t)-3) | 1;
    sort_u32(L);
    for (i = 0; i < P; i++)
        out[i] = (small)((L[i] & 3) - 1);
    OPENSSL_cleanse(L, sizeof L);
}

static void small_random(small* out)
{
    int i;
    for (i = 0; i < P; i++)
        out[i] = (small)((int)(((urandom32() & 0x3fffffffu) * 3) >> 30) - 1);
}

static void r3_mult(small* h, const small* f, const small* g)
{
    int16_t fg[P + P - 1];
    int i, j;
    for (i = 0; i < P + P - 1; i++) {
        int32_t acc = 0;
        int lo = i < P ? 0 : i - P + 1, hi = i < P ? i : P - 1;
        for (j = lo; j <= hi; j++)
            acc += f[j] * (int32_t)g[i - j];
        fg[i] = f3_freeze((int16_t)acc);
    }
    for (i = P + P - 2; i >= P; i--) {
        fg[i - P] = f3_freeze((int16_t)(fg[i - P] + fg[i]));
        fg[i - P + 1] = f3_freeze((int16_t)(fg[i - P + 1] + fg[i]));
    }
    for (i = 0; i < P; i++)
        h[i] = (small)fg[i];
}

static int r3_recip(small* out, const small* in)
{
    small f[P + 1], g[P + 1], v[P + 1], r[P + 1];
    int i, loop, delta, sign, swap, t;
    memset(v, 0, sizeof v);
    memset(r, 0, sizeof r);
    r[0] = 1;
    memset(f, 0, sizeof f);
    f[0] = 1;
    f[P - 1] = f[P] = -1;
    for (i = 0; i < P; i++)
        g[P - 1 - i] = in[i];
    g[P] = 0;
    delta = 1;
    for (loop = 0; loop < 2 * P - 1; loop++) {
        for (i = P; i > 0; i--)
            v[i] = v[i - 1];
        v[0] = 0;
        sign = -g[0] * f[0];
        swap = negative_mask((int16_t)-delta) & nonzero_mask(g[0]);
        delta ^= swap & (delta ^ -delta);
        delta += 1;
        for (i = 0; i < P + 1; i++) {
            t = swap & (f[i] ^ g[i]);
            f[i] = (small)(f[i] ^ t);
            g[i] = (small)(g[i] ^ t);
            t = swap & (v[i] ^ r[i]);
            v[i] = (small)(v[i] ^ t);
            r[i] = (small)(r[i] ^ t);
        }
        for (i = 0; i < P + 1; i++)
            g[i] = f3_freeze((int16_t)(g[i] + sign * f[i]));
        for (i = 0; i < P + 1; i++)
            r[i] = f3_freeze((int16_t)(r[i] + sign * v[i]));
        for (i = 0; i < P; i++)
            g[i] = g[i + 1];
        g[P] = 0;
    }
    sign = f[0];
    for (i = 0; i < P; i++)
        out[i] = (small)(sign * v[P - 1 - i]);
    return nonzero_mask((int16_t)delta);
}

static void rq_mult_small(Fq* h, const Fq* f, const small* g)
{
    Fq fg[P + P - 1];
    int i, j;
    for (i = 0; i < P + P - 1; i++) {
        int32_t acc = 0;
        int lo = i < P ? 0 : i - P + 1, hi = i < P ? i : P - 1;
        for (j = lo; j <= hi; j++)
            acc += f[j] * (int32_t)g[i - j];
        fg[i] = fq_freeze(acc);
    }
    for (i = P + P - 2; i >= P; i--) {
        fg[i - P] = fq_freeze(fg[i - P] + fg[i]);
        fg[i - P + 1] = fq_freeze(fg[i - P + 1] + fg[i]);
    }
    for (i = 0; i < P; i++)
        h[i] = fg[i];
}

static void rq_recip3(Fq* out, const small* in)
{
    Fq f[P + 1], g[P + 1], v[P + 1], r[P + 1], scale;
    int i, loop, delta, swap, t;
    int32_t f0, g0;
    memset(v, 0, sizeof v);
    memset(r, 0, sizeof r);
    r[0] = fq_recip(3);
    memset(f, 0, sizeof f);
    f[0] = 1;
    f[P - 1] = f[P] = -1;
    for (i = 0; i < P; i++)
        g[P - 1 - i] = in[i];
    g[P] = 0;
    delta = 1;
    for (loop = 0; loop < 2 * P - 1; loop++) {
        for (i = P; i > 0; i--)
            v[i] = v[i - 1];
        v[0] = 0;
        swap = negative_mask((int16_t)-delta) & nonzero_mask(g[0]);
        delta ^= swap & (delta ^ -delta);
        delta += 1;
        for (i = 0; i < P + 1; i++) {
            t = swap & (f[i] ^ g[i]);
            f[i] = (Fq)(f[i] ^ t);
            g[i] = (Fq)(g[i] ^ t);
            t = swap & (v[i] ^ r[i]);
            v[i] = (Fq)(v[i] ^ t);
            r[i] = (Fq)(r[i] ^ t);
        }
        f0 = f[0];
        g0 = g[0];
        for (i = 0; i < P + 1; i++)
            g[i] = fq_freeze(f0 * g[i] - g0 * f[i]);
        for (i = 0; i < P + 1; i++)
            r[i] = fq_freeze(f0 * r[i] - g0 * v[i]);
        for (i = 0; i < P; i++)
            g[i] = g[i + 1];
        g[P] = 0;
    }
    scale = fq_recip(f[0]);
    for (i = 0; i < P; i++)
        out[i] = fq_freeze(scale * (int32_t)v[P - 1 - i]);
}

static void encode(uint8_t** out, const uint16_t* R, const uint16_t* M, long len)
{
    if (len == 1) {
        uint16_t r = R[0], m = M[0];
        while (m > 1) {
            *(*out)++ = (uint8_t)r;
            r >>= 8;
            m = (uint16_t)((m + 255) >> 8);
        }
    }
    if (len > 1) {
        uint16_t R2[(P + 1) / 2], M2[(P + 1) / 2];
        long i;
        for (i = 0; i < len - 1; i += 2) {
            uint32_t m0 = M[i], r = R[i] + R[i + 1] * m0, m = M[i + 1] * m0;
            while (m >= 16384) {
                *(*out)++ = (uint8_t)r;
                r >>= 8;
                m = (m + 255) >> 8;
            }
            R2[i / 2] = (uint16_t)r;
            M2[i / 2] = (uint16_t)m;
        }
        if (i < len) {
            R2[i / 2] = R[i];
            M2[i / 2] = M[i];
        }
        encode(out, R2, M2, (len + 1) / 2);
    }
}

static void decode(uint16_t* out, const uint8_t* S, const uint16_t* M, long len)
{
    if (len == 1) {
        if (M[0] == 1)
            *out = 0;
        else if (M[0] <= 256)
            *out = u32_mod_u14(S[0], M[0]);
        else
            *out = u32_mod_u14(S[0] + (((uint16_t)S[1]) << 8), M[0]);
    }
    if (len > 1) {
        uint16_t R2[(P + 1) / 2], M2[(P + 1) / 2], bottomr[P / 2];
        uint32_t bottomt[P / 2];
        long i;
        for (i = 0; i < len - 1; i += 2) {
            uint32_t m = M[i] * (uint32_t)M[i + 1];
            if (m > 256 * 16383) {
                bottomt[i / 2] = 256 * 256;
                bottomr[i / 2] = (uint16_t)(S[0] + 256 * S[1]);
                S += 2;
                M2[i / 2] = (uint16_t)((((m + 255) >> 8) + 255) >> 8);
            } else if (m >= 16384) {
                bottomt[i / 2] = 256;
                bottomr[i / 2] = S[0];
                S += 1;
                M2[i / 2] = (uint16_t)((m + 255) >> 8);
            } else {
                bottomt[i / 2] = 1;
                bottomr[i / 2] = 0;
                M2[i / 2] = (uint16_t)m;
            }
        }
        if (i < len)
            M2[i / 2] = M[i];
        decode(R2, S, M2, (len + 1) / 2);
        for (i = 0; i < len - 1; i += 2) {
            uint32_t r = bottomr[i / 2], r1;
            uint16_t r0;
            r += bottomt[i / 2] * R2[i / 2];
            u32_divmod_u14(&r1, &r0, r, M[i]);
            r1 = u32_mod_u14(r1, M[i + 1]);
            *out++ = r0;
            *out++ = (uint16_t)r1;
        }
        if (i < len)
            *out++ = R2[i / 2];
    }
}

static void rq_encode(uint8_t* s, const Fq* r)
{
    uint16_t R[P], M[P];
    int i;
    for (i = 0; i < P; i++) {
        R[i] = (uint16_t)(r[i] + Q12);
        M[i] = Q;
    }
    encode(&s, R, M, P);
}

static void rq_decode(Fq* r, const uint8_t* s)
{
    uint16_t R[P], M[P];
    int i;
    for (i = 0; i < P; i++)
        M[i] = Q;
    decode(R, s, M, P);
    for (i = 0; i < P; i++)
        r[i] = (Fq)(R[i] - Q12);
}

static void rounded_encode(uint8_t* s, const Fq* r)
{
    uint16_t R[P], M[P];
    int i;
    for (i = 0; i < P; i++) {
        R[i] = (uint16_t)(((r[i] + Q12) * 10923) >> 15);
        M[i] = (Q + 2) / 3;
    }
    encode(&s, R, M, P);
}

static void rounded_decode(Fq* r, const uint8_t* s)
{
    uint16_t R[P], M[P];
    int i;
    for (i = 0; i < P; i++)
        M[i] = (Q + 2) / 3;
    decode(R, s, M, P);
    for (i = 0; i < P; i++)
        r[i] = (Fq)(R[i] * 3 - Q12);
}

static void small_encode(uint8_t* s, const small* f)
{
    int i;
    for (i = 0; i < P / 4; i++) {
        small x = (small)(f[0] + 1);
        x = (small)(x + ((f[1] + 1) << 2));
        x = (small)(x + ((f[2] + 1) << 4));
        x = (small)(x + ((f[3] + 1) << 6));
        *s++ = (uint8_t)x;
        f += 4;
    }
    *s = (uint8_t)(f[0] + 1);
}

static void small_decode(small* f, const uint8_t* s)
{
    int i;
    for (i = 0; i < P / 4; i++) {
        uint8_t x = *s++;
        *f++ = (small)((x & 3) - 1);
        x >>= 2;
        *f++ = (small)((x & 3) - 1);
        x >>= 2;
        *f++ = (small)((x & 3) - 1);
        x >>= 2;
        *f++ = (small)((x & 3) - 1);
    }
    *f = (small)((*s & 3) - 1);
}

static void hash_prefix(uint8_t* out, int b, const uint8_t* in, size_t inlen)
{
    uint8_t h[64];
    EVP_MD_CTX* m = EVP_MD_CTX_new();
    uint8_t bb = (uint8_t)b;
    if (!m || EVP_DigestInit_ex(m, EVP_sha512(), NULL) != 1 || EVP_DigestUpdate(m, &bb, 1) != 1
        || EVP_DigestUpdate(m, in, inlen) != 1 || EVP_DigestFinal_ex(m, h, NULL) != 1)
        abort();
    EVP_MD_CTX_free(m);
    memcpy(out, h, 32);
    OPENSSL_cleanse(h, sizeof h);
}

static void hash_confirm(uint8_t* h, const uint8_t* r, const uint8_t* cache)
{
    uint8_t x[HASH_BYTES * 2];
    hash_prefix(x, 3, r, SMALL_BYTES);
    memcpy(x + HASH_BYTES, cache, HASH_BYTES);
    hash_prefix(h, 2, x, sizeof x);
}

static void hash_session(uint8_t* k, int b, const uint8_t* y, const uint8_t* z)
{
    uint8_t x[HASH_BYTES + SSH_SNTRUP_CT];
    hash_prefix(x, 3, y, SMALL_BYTES);
    memcpy(x + HASH_BYTES, z, SSH_SNTRUP_CT);
    hash_prefix(k, b, x, sizeof x);
    OPENSSL_cleanse(x, sizeof x);
}

static void hide(uint8_t* c, uint8_t* r_enc, const small* r, const uint8_t* pk, const uint8_t* cache)
{
    Fq h[P], hr[P], cc[P];
    int i;
    small_encode(r_enc, r);
    rq_decode(h, pk);
    rq_mult_small(hr, h, r);
    for (i = 0; i < P; i++)
        cc[i] = (Fq)(hr[i] - f3_freeze(hr[i]));
    rounded_encode(c, cc);
    hash_confirm(c + ROUNDED_BYTES, r_enc, cache);
}

int ssh_sntrup_keypair(uint8_t pk[SSH_SNTRUP_PK], uint8_t sk[SSH_SNTRUP_SK])
{
    small g[P], v[P], f[P];
    Fq finv[P], h[P];
    for (;;) {
        small_random(g);
        if (r3_recip(v, g) == 0)
            break;
    }
    short_random(f);
    rq_recip3(finv, f);
    rq_mult_small(h, finv, g);
    rq_encode(pk, h);
    small_encode(sk, f);
    small_encode(sk + SMALL_BYTES, v);
    memcpy(sk + SK_FIXED, pk, RQ_BYTES);
    ssh_random(sk + SK_FIXED + RQ_BYTES, SMALL_BYTES);
    hash_prefix(sk + SK_FIXED + RQ_BYTES + SMALL_BYTES, 4, pk, RQ_BYTES);
    OPENSSL_cleanse(g, sizeof g);
    OPENSSL_cleanse(v, sizeof v);
    OPENSSL_cleanse(f, sizeof f);
    OPENSSL_cleanse(finv, sizeof finv);
    return 0;
}

int ssh_sntrup_enc(uint8_t ct[SSH_SNTRUP_CT], uint8_t ss[32], const uint8_t pk[SSH_SNTRUP_PK])
{
    small r[P];
    uint8_t r_enc[SMALL_BYTES], cache[HASH_BYTES];
    hash_prefix(cache, 4, pk, RQ_BYTES);
    short_random(r);
    hide(ct, r_enc, r, pk, cache);
    hash_session(ss, 1, r_enc, ct);
    OPENSSL_cleanse(r, sizeof r);
    OPENSSL_cleanse(r_enc, sizeof r_enc);
    return 0;
}

int ssh_sntrup_dec(uint8_t ss[32], const uint8_t ct[SSH_SNTRUP_CT], const uint8_t sk[SSH_SNTRUP_SK])
{
    const uint8_t* pk = sk + SK_FIXED;
    const uint8_t* rho = pk + RQ_BYTES;
    const uint8_t* cache = rho + SMALL_BYTES;
    small f[P], v[P], e[P], ev[P], r[P];
    Fq c[P], cf[P], cf3[P];
    uint8_t r_enc[SMALL_BYTES], cnew[SSH_SNTRUP_CT];
    int i, mask;
    uint16_t diff = 0;
    small_decode(f, sk);
    small_decode(v, sk + SMALL_BYTES);
    rounded_decode(c, ct);
    rq_mult_small(cf, c, f);
    for (i = 0; i < P; i++)
        cf3[i] = fq_freeze(3 * cf[i]);
    for (i = 0; i < P; i++)
        e[i] = f3_freeze(cf3[i]);
    r3_mult(ev, e, v);
    {
        int weight = 0;
        for (i = 0; i < P; i++)
            weight += ev[i] & 1;
        mask = nonzero_mask((int16_t)(weight - W));
    }
    for (i = 0; i < W; i++)
        r[i] = (small)(((ev[i] ^ 1) & ~mask) ^ 1);
    for (i = W; i < P; i++)
        r[i] = (small)(ev[i] & ~mask);
    hide(cnew, r_enc, r, pk, cache);
    for (i = 0; i < SSH_SNTRUP_CT; i++)
        diff = (uint16_t)(diff | (ct[i] ^ cnew[i]));
    mask = (int)(1 & ((diff - 1) >> 8)) - 1;
    for (i = 0; i < SMALL_BYTES; i++)
        r_enc[i] = (uint8_t)(r_enc[i] ^ (mask & (r_enc[i] ^ rho[i])));
    hash_session(ss, 1 + mask, r_enc, ct);
    OPENSSL_cleanse(f, sizeof f);
    OPENSSL_cleanse(v, sizeof v);
    OPENSSL_cleanse(e, sizeof e);
    OPENSSL_cleanse(ev, sizeof ev);
    OPENSSL_cleanse(r, sizeof r);
    OPENSSL_cleanse(r_enc, sizeof r_enc);
    return 0;
}

#endif
