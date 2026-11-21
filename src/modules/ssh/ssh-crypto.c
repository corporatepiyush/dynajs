#include "ssh-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_SSH)

#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/param_build.h>
#include <openssl/rand.h>
#include <openssl/bn.h>

static EVP_MAC* ssh_poly_mac;

void ssh_random(void* p, size_t n)
{
    if (RAND_bytes((unsigned char*)p, (int)n) != 1)
        abort();
}

unsigned ssh_cipher_keylen(int kind)
{
    return kind == SSH_CIPHER_CHACHAPOLY ? 64 : kind == SSH_CIPHER_AES256GCM ? 32 : 0;
}

unsigned ssh_cipher_ivlen(int kind)
{
    return kind == SSH_CIPHER_AES256GCM ? 12 : 0;
}

void ssh_aead_free(ssh_aead_t* a)
{
    EVP_CIPHER_CTX_free(a->main);
    EVP_CIPHER_CTX_free(a->hdr);
    EVP_MAC_CTX_free(a->mac);
    OPENSSL_cleanse(a, sizeof *a);
}

int ssh_aead_init(ssh_aead_t* a, int kind, int enc, const uint8_t* key,
    const uint8_t* iv)
{
    memset(a, 0, sizeof *a);
    a->kind = kind;
    a->enc = enc;
    if (kind == SSH_CIPHER_CHACHAPOLY) {
        uint8_t z[16] = { 0 };
        if (!ssh_poly_mac)
            ssh_poly_mac = EVP_MAC_fetch(NULL, "POLY1305", NULL);
        a->main = EVP_CIPHER_CTX_new();
        a->hdr = EVP_CIPHER_CTX_new();
        a->mac = ssh_poly_mac ? EVP_MAC_CTX_new(ssh_poly_mac) : NULL;
        a->bs = 8;
        a->tag = 16;
        if (!a->main || !a->hdr || !a->mac
            || EVP_EncryptInit_ex(a->main, EVP_chacha20(), NULL, key, z) != 1
            || EVP_EncryptInit_ex(a->hdr, EVP_chacha20(), NULL, key + 32, z) != 1)
            goto fail;
        return 0;
    }
    if (kind == SSH_CIPHER_AES256GCM) {
        a->main = EVP_CIPHER_CTX_new();
        a->bs = 16;
        a->tag = 16;
        memcpy(a->iv, iv, 12);
        if (!a->main || EVP_CipherInit_ex(a->main, EVP_aes_256_gcm(), NULL, NULL, NULL, enc) != 1
            || EVP_CIPHER_CTX_ctrl(a->main, EVP_CTRL_GCM_SET_IVLEN, 12, NULL) != 1
            || EVP_CipherInit_ex(a->main, NULL, NULL, key, NULL, enc) != 1)
            goto fail;
        return 0;
    }
    a->bs = 8;
    return 0;
fail:
    ssh_aead_free(a);
    return -1;
}

static void chacha_iv(uint8_t iv[16], uint32_t seq, uint8_t ctr)
{
    memset(iv, 0, 16);
    iv[0] = ctr;
    ssh_put_be32(iv + 12, seq);
}

static int chacha_xor(EVP_CIPHER_CTX* c, uint32_t seq, uint8_t ctr, uint8_t* p, size_t n)
{
    uint8_t iv[16];
    int ol;
    chacha_iv(iv, seq, ctr);
    if (EVP_EncryptInit_ex(c, NULL, NULL, NULL, iv) != 1)
        return -1;
    while (n) {
        int step = n > (1u << 30) ? (1 << 30) : (int)n;
        if (EVP_EncryptUpdate(c, p, &ol, p, step) != 1)
            return -1;
        p += step;
        n -= (size_t)step;
    }
    return 0;
}

static int chacha_tag(ssh_aead_t* a, uint32_t seq, const uint8_t* p, size_t n, uint8_t tag[16])
{
    uint8_t pk[32];
    size_t ol = 0;
    int rc = -1;
    memset(pk, 0, sizeof pk);
    if (chacha_xor(a->main, seq, 0, pk, sizeof pk) < 0)
        goto out;
    if (EVP_MAC_init(a->mac, pk, sizeof pk, NULL) != 1 || EVP_MAC_update(a->mac, p, n) != 1
        || EVP_MAC_final(a->mac, tag, &ol, 16) != 1 || ol != 16)
        goto out;
    rc = 0;
out:
    OPENSSL_cleanse(pk, sizeof pk);
    return rc;
}

int ssh_aead_len(ssh_aead_t* a, uint32_t seq, const uint8_t* p, uint32_t* len)
{
    uint8_t t[4];
    if (a->kind != SSH_CIPHER_CHACHAPOLY) {
        *len = ssh_be32(p);
        return 0;
    }
    memcpy(t, p, 4);
    if (chacha_xor(a->hdr, seq, 0, t, 4) < 0)
        return -1;
    *len = ssh_be32(t);
    return 0;
}

static void gcm_next(ssh_aead_t* a)
{
    int i;
    for (i = 11; i >= 4; i--)
        if (++a->iv[i])
            break;
}

int ssh_aead_seal(ssh_aead_t* a, uint32_t seq, uint8_t* p, size_t plen)
{
    int ol;
    if (a->kind == SSH_CIPHER_CHACHAPOLY) {
        if (chacha_xor(a->hdr, seq, 0, p, 4) < 0 || chacha_xor(a->main, seq, 1, p + 4, plen) < 0)
            return -1;
        return chacha_tag(a, seq, p, 4 + plen, p + 4 + plen);
    }
    if (a->kind == SSH_CIPHER_AES256GCM) {
        if (EVP_CipherInit_ex(a->main, NULL, NULL, NULL, a->iv, 1) != 1
            || EVP_EncryptUpdate(a->main, NULL, &ol, p, 4) != 1
            || EVP_EncryptUpdate(a->main, p + 4, &ol, p + 4, (int)plen) != 1
            || EVP_EncryptFinal_ex(a->main, p + 4 + plen, &ol) != 1
            || EVP_CIPHER_CTX_ctrl(a->main, EVP_CTRL_GCM_GET_TAG, 16, p + 4 + plen) != 1)
            return -1;
        gcm_next(a);
        return 0;
    }
    return 0;
}

int ssh_aead_open(ssh_aead_t* a, uint32_t seq, uint8_t* p, size_t plen)
{
    int ol;
    if (a->kind == SSH_CIPHER_CHACHAPOLY) {
        uint8_t tag[16];
        if (chacha_tag(a, seq, p, 4 + plen, tag) < 0)
            return -1;
        if (CRYPTO_memcmp(tag, p + 4 + plen, 16) != 0)
            return -1;
        if (chacha_xor(a->hdr, seq, 0, p, 4) < 0)
            return -1;
        return chacha_xor(a->main, seq, 1, p + 4, plen);
    }
    if (a->kind == SSH_CIPHER_AES256GCM) {
        uint8_t tag[16];
        int ok;
        memcpy(tag, p + 4 + plen, 16);
        ok = EVP_CipherInit_ex(a->main, NULL, NULL, NULL, a->iv, 0) == 1
            && EVP_DecryptUpdate(a->main, NULL, &ol, p, 4) == 1
            && EVP_DecryptUpdate(a->main, p + 4, &ol, p + 4, (int)plen) == 1
            && EVP_CIPHER_CTX_ctrl(a->main, EVP_CTRL_GCM_SET_TAG, 16, tag) == 1
            && EVP_DecryptFinal_ex(a->main, tag, &ol) == 1;
        gcm_next(a);
        return ok ? 0 : -1;
    }
    return 0;
}

int ssh_hash(int sha512, const void* p, size_t n, uint8_t* out)
{
    return EVP_Digest(p, n, out, NULL, sha512 ? EVP_sha512() : EVP_sha256(), NULL) == 1 ? 0 : -1;
}

int ssh_hash2(int sha512, const void* a, size_t an, const void* b, size_t bn,
    uint8_t* out)
{
    EVP_MD_CTX* m = EVP_MD_CTX_new();
    int ok = m && EVP_DigestInit_ex(m, sha512 ? EVP_sha512() : EVP_sha256(), NULL) == 1
        && EVP_DigestUpdate(m, a, an) == 1 && EVP_DigestUpdate(m, b, bn) == 1
        && EVP_DigestFinal_ex(m, out, NULL) == 1;
    EVP_MD_CTX_free(m);
    return ok ? 0 : -1;
}

EVP_PKEY* ssh_x25519_new(uint8_t pub[32])
{
    EVP_PKEY* k = EVP_PKEY_Q_keygen(NULL, NULL, "X25519");
    size_t n = 32;
    if (!k)
        return NULL;
    if (EVP_PKEY_get_raw_public_key(k, pub, &n) != 1 || n != 32) {
        EVP_PKEY_free(k);
        return NULL;
    }
    return k;
}

int ssh_x25519_derive(EVP_PKEY* k, const uint8_t peer[32], uint8_t out[32])
{
    EVP_PKEY* pk = EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, NULL, peer, 32);
    EVP_PKEY_CTX* d = pk ? EVP_PKEY_CTX_new(k, NULL) : NULL;
    size_t n = 32;
    uint8_t acc = 0;
    int i, ok;
    ok = d && EVP_PKEY_derive_init(d) == 1 && EVP_PKEY_derive_set_peer(d, pk) == 1
        && EVP_PKEY_derive(d, out, &n) == 1 && n == 32;
    EVP_PKEY_CTX_free(d);
    EVP_PKEY_free(pk);
    if (!ok)
        return -1;
    for (i = 0; i < 32; i++)
        acc |= out[i];
    return acc ? 0 : -1;
}

EVP_PKEY* ssh_mlkem_new(uint8_t pub[1184])
{
    EVP_PKEY_CTX* g = EVP_PKEY_CTX_new_from_name(NULL, "ML-KEM-768", NULL);
    EVP_PKEY* k = NULL;
    size_t n = 1184;
    if (!g || EVP_PKEY_keygen_init(g) != 1 || EVP_PKEY_generate(g, &k) != 1)
        k = NULL;
    EVP_PKEY_CTX_free(g);
    if (k && (EVP_PKEY_get_raw_public_key(k, pub, &n) != 1 || n != 1184)) {
        EVP_PKEY_free(k);
        k = NULL;
    }
    return k;
}

int ssh_mlkem_decap(EVP_PKEY* k, const uint8_t ct[1088], uint8_t out[32])
{
    EVP_PKEY_CTX* d = EVP_PKEY_CTX_new_from_pkey(NULL, k, NULL);
    size_t n = 32;
    int ok = d && EVP_PKEY_decapsulate_init(d, NULL) == 1
        && EVP_PKEY_decapsulate(d, out, &n, ct, 1088) == 1 && n == 32;
    EVP_PKEY_CTX_free(d);
    return ok ? 0 : -1;
}

static int ed25519_verify(const uint8_t pk[32], const uint8_t* sig, size_t siglen,
    const uint8_t* data, size_t n)
{
    EVP_PKEY* k;
    EVP_MD_CTX* m;
    int ok;
    if (siglen != 64)
        return 0;
    k = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, NULL, pk, 32);
    m = EVP_MD_CTX_new();
    ok = k && m && EVP_DigestVerifyInit(m, NULL, NULL, NULL, k) == 1
        && EVP_DigestVerify(m, sig, 64, data, n) == 1;
    EVP_MD_CTX_free(m);
    EVP_PKEY_free(k);
    return ok;
}

int ssh_ed25519_verify(const uint8_t pk[32], const uint8_t* sig, size_t siglen,
    const uint8_t* data, size_t n)
{
    return ed25519_verify(pk, sig, siglen, data, n);
}

int ssh_mlkem_encap(const uint8_t pk[1184], uint8_t ct[1088], uint8_t ss[32])
{
    EVP_PKEY* k = EVP_PKEY_new_raw_public_key_ex(NULL, "ML-KEM-768", NULL, pk, 1184);
    EVP_PKEY_CTX* e = k ? EVP_PKEY_CTX_new_from_pkey(NULL, k, NULL) : NULL;
    size_t cl = 1088, sl = 32;
    int ok = e && EVP_PKEY_encapsulate_init(e, NULL) == 1
        && EVP_PKEY_encapsulate(e, ct, &cl, ss, &sl) == 1 && cl == 1088 && sl == 32;
    EVP_PKEY_CTX_free(e);
    EVP_PKEY_free(k);
    return ok ? 0 : -1;
}

static int rsa_verify(const uint8_t* e, size_t elen, const uint8_t* nn, size_t nlen,
    const uint8_t* sig, size_t siglen, const uint8_t* data, size_t n, char* why, size_t whylen)
{
    BIGNUM* be = BN_bin2bn(e, (int)elen, NULL);
    BIGNUM* bn = BN_bin2bn(nn, (int)nlen, NULL);
    OSSL_PARAM_BLD* b = OSSL_PARAM_BLD_new();
    OSSL_PARAM* prm = NULL;
    EVP_PKEY_CTX* fc = NULL;
    EVP_PKEY* k = NULL;
    EVP_MD_CTX* m = NULL;
    int ok = 0, bits;
    if (!be || !bn || !b)
        goto out;
    bits = BN_num_bits(bn);
    if (bits < 2048 || bits > 16384) {
        snprintf(why, whylen, "RSA host key of %d bits is outside 2048..16384", bits);
        goto out;
    }
    if (OSSL_PARAM_BLD_push_BN(b, OSSL_PKEY_PARAM_RSA_N, bn) != 1
        || OSSL_PARAM_BLD_push_BN(b, OSSL_PKEY_PARAM_RSA_E, be) != 1
        || !(prm = OSSL_PARAM_BLD_to_param(b)))
        goto out;
    fc = EVP_PKEY_CTX_new_from_name(NULL, "RSA", NULL);
    if (!fc || EVP_PKEY_fromdata_init(fc) != 1
        || EVP_PKEY_fromdata(fc, &k, EVP_PKEY_PUBLIC_KEY, prm) != 1)
        goto out;
    m = EVP_MD_CTX_new();
    ok = m && EVP_DigestVerifyInit(m, NULL, EVP_sha512(), NULL, k) == 1
        && EVP_DigestVerify(m, sig, siglen, data, n) == 1;
out:
    EVP_MD_CTX_free(m);
    EVP_PKEY_free(k);
    EVP_PKEY_CTX_free(fc);
    OSSL_PARAM_free(prm);
    OSSL_PARAM_BLD_free(b);
    BN_free(be);
    BN_free(bn);
    return ok;
}

static int str_is(const uint8_t* p, size_t n, const char* s)
{
    return p && n == strlen(s) && memcmp(p, s, n) == 0;
}

int ssh_verify_hostsig(int hk_alg, const uint8_t* ks, size_t kslen,
    const uint8_t* sig, size_t siglen, const uint8_t* data, size_t dlen,
    char* why, size_t whylen)
{
    sshr_t k, s;
    const uint8_t *kt, *st, *sv;
    size_t ktn, stn, svn;
    sshr_init(&k, ks, kslen);
    sshr_init(&s, sig, siglen);
    kt = sshr_str(&k, &ktn);
    st = sshr_str(&s, &stn);
    sv = sshr_str(&s, &svn);
    snprintf(why, whylen, "the host key signature does not verify");
    if (hk_alg == SSH_HK_ED25519) {
        const uint8_t* pk;
        size_t pkn;
        pk = sshr_str(&k, &pkn);
        if (k.err || s.err || k.off != k.len || s.off != s.len || !str_is(kt, ktn, "ssh-ed25519")
            || !str_is(st, stn, "ssh-ed25519") || pkn != 32) {
            snprintf(why, whylen, "malformed ssh-ed25519 host key or signature");
            return -1;
        }
        return ed25519_verify(pk, sv, svn, data, dlen) ? 0 : -1;
    }
    if (hk_alg == SSH_HK_SK_ED25519) {
        const uint8_t *pk, *app;
        size_t pkn, appn;
        uint8_t msg[32 + 1 + 4 + 32], flags;
        uint32_t ctr;
        pk = sshr_str(&k, &pkn);
        app = sshr_str(&k, &appn);
        flags = sshr_u8(&s);
        ctr = sshr_u32(&s);
        if (k.err || s.err || k.off != k.len || s.off != s.len
            || !str_is(kt, ktn, "sk-ssh-ed25519@openssh.com")
            || !str_is(st, stn, "sk-ssh-ed25519@openssh.com") || pkn != 32) {
            snprintf(why, whylen, "malformed sk-ssh-ed25519 host key or signature");
            return -1;
        }
        if (ssh_hash(0, app, appn, msg) < 0 || ssh_hash(0, data, dlen, msg + 37) < 0)
            return -1;
        msg[32] = flags;
        ssh_put_be32(msg + 33, ctr);
        return ed25519_verify(pk, sv, svn, msg, sizeof msg) ? 0 : -1;
    }
    if (hk_alg == SSH_HK_RSA_SHA2_512) {
        const uint8_t *e, *n;
        size_t en, nn;
        e = sshr_str(&k, &en);
        n = sshr_str(&k, &nn);
        if (k.err || s.err || k.off != k.len || s.off != s.len || !str_is(kt, ktn, "ssh-rsa")
            || !str_is(st, stn, "rsa-sha2-512")) {
            snprintf(why, whylen, "malformed ssh-rsa host key or rsa-sha2-512 signature");
            return -1;
        }
        return rsa_verify(e, en, n, nn, sv, svn, data, dlen, why, whylen) ? 0 : -1;
    }
    return -1;
}

int ssh_verify_usersig(const uint8_t* blob, size_t bloblen, const uint8_t* sig,
    size_t siglen, const uint8_t* data, size_t dlen, char* why, size_t whylen)
{
    sshr_t k, s;
    const uint8_t *kt, *st;
    size_t ktn, stn;
    sshr_init(&k, blob, bloblen);
    sshr_init(&s, sig, siglen);
    kt = sshr_str(&k, &ktn);
    st = sshr_str(&s, &stn);
    if (str_is(kt, ktn, "ssh-ed25519") && str_is(st, stn, "ssh-ed25519"))
        return ssh_verify_hostsig(SSH_HK_ED25519, blob, bloblen, sig, siglen, data, dlen, why, whylen);
    if (str_is(kt, ktn, "sk-ssh-ed25519@openssh.com") && str_is(st, stn, "sk-ssh-ed25519@openssh.com")) {
        size_t svn;
        (void)sshr_str(&s, &svn);
        if (s.err || s.off >= s.len || !(s.p[s.off] & 1)) {
            snprintf(why, whylen, "the sk-ssh-ed25519 signature lacks the user-presence flag");
            return -1;
        }
        return ssh_verify_hostsig(SSH_HK_SK_ED25519, blob, bloblen, sig, siglen, data, dlen, why, whylen);
    }
    snprintf(why, whylen, "unsupported user key or signature type");
    return -1;
}

int ssh_ed25519_sign(EVP_PKEY* k, const uint8_t* data, size_t n, uint8_t sig[64])
{
    EVP_MD_CTX* m = EVP_MD_CTX_new();
    size_t sl = 64;
    int ok = m && EVP_DigestSignInit(m, NULL, NULL, NULL, k) == 1
        && EVP_DigestSign(m, sig, &sl, data, n) == 1 && sl == 64;
    EVP_MD_CTX_free(m);
    return ok ? 0 : -1;
}

void ssh_fingerprint(const uint8_t* blob, size_t n, char out[64])
{
    uint8_t h[32];
    memcpy(out, "SHA256:", 7);
    if (ssh_hash(0, blob, n, h) < 0) {
        out[7] = 0;
        return;
    }
    ssh_b64_encode(h, 32, out + 7, 0);
}

#endif
