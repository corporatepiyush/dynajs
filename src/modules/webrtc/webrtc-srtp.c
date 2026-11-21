#include "webrtc-srtp.h"

#include <stdlib.h>
#include <string.h>

#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>

static uint32_t srtp_rd32(const uint8_t* p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static void srtp_wr32(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

int wrt_srtp_profile_lengths(int profile, size_t* key, size_t* salt)
{
    switch (profile) {
    case WRT_SRTP_CM80:
    case WRT_SRTP_CM32:
        *key = 16;
        *salt = 14;
        return 0;
    case WRT_SRTP_GCM128:
        *key = 16;
        *salt = 12;
        return 0;
    case WRT_SRTP_GCM256:
        *key = 32;
        *salt = 12;
        return 0;
    default:
        return -1;
    }
}

int wrt_srtp_kdf(int profile, const uint8_t* master_key, const uint8_t* master_salt, uint8_t label, uint8_t* out, size_t n)
{
    size_t klen, slen;
    uint8_t iv[16];
    EVP_CIPHER_CTX* c;
    int l = 0, ok;
    if (wrt_srtp_profile_lengths(profile, &klen, &slen) < 0 || n > 4096)
        return -1;
    memset(iv, 0, sizeof(iv));
    memcpy(iv, master_salt, slen);
    iv[7] ^= label;
    c = EVP_CIPHER_CTX_new();
    if (!c)
        return -1;
    memset(out, 0, n);
    ok = EVP_EncryptInit_ex(c, klen == 32 ? EVP_aes_256_ctr() : EVP_aes_128_ctr(), NULL, master_key, iv) == 1
        && EVP_EncryptUpdate(c, out, &l, out, (int)n) == 1 && (size_t)l == n;
    EVP_CIPHER_CTX_free(c);
    OPENSSL_cleanse(iv, sizeof(iv));
    return ok ? 0 : -1;
}

static void* srtp_cipher_new(int gcm, size_t klen, const uint8_t* key)
{
    EVP_CIPHER_CTX* c = EVP_CIPHER_CTX_new();
    const EVP_CIPHER* ci;
    if (!c)
        return NULL;
    if (gcm)
        ci = klen == 32 ? EVP_aes_256_gcm() : EVP_aes_128_gcm();
    else
        ci = EVP_aes_128_ctr();
    if (EVP_CipherInit_ex(c, ci, NULL, NULL, NULL, 1) != 1
        || (gcm && EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_SET_IVLEN, 12, NULL) != 1)
        || EVP_CipherInit_ex(c, NULL, NULL, key, NULL, 1) != 1) {
        EVP_CIPHER_CTX_free(c);
        return NULL;
    }
    return c;
}

static void* srtp_hmac_new(const uint8_t* key)
{
    EVP_MAC* mac = EVP_MAC_fetch(NULL, "HMAC", NULL);
    EVP_MAC_CTX* h;
    OSSL_PARAM params[2];
    char digest[] = "SHA1";
    if (!mac)
        return NULL;
    h = EVP_MAC_CTX_new(mac);
    EVP_MAC_free(mac);
    if (!h)
        return NULL;
    params[0] = OSSL_PARAM_construct_utf8_string(OSSL_MAC_PARAM_DIGEST, digest, 0);
    params[1] = OSSL_PARAM_construct_end();
    if (EVP_MAC_init(h, key, 20, params) != 1) {
        EVP_MAC_CTX_free(h);
        return NULL;
    }
    return h;
}

int wrt_srtp_init_session(wrt_srtp* s, int profile, const uint8_t* rtp_key, const uint8_t* rtp_salt, const uint8_t* rtp_auth,
    const uint8_t* rtcp_key, const uint8_t* rtcp_salt, const uint8_t* rtcp_auth)
{
    size_t klen, slen;
    memset(s, 0, sizeof(*s));
    if (wrt_srtp_profile_lengths(profile, &klen, &slen) < 0)
        return WRT_SRTP_ERR_STATE;
    s->profile = (uint8_t)profile;
    s->gcm = profile == WRT_SRTP_GCM128 || profile == WRT_SRTP_GCM256;
    s->tag = s->gcm ? 16 : profile == WRT_SRTP_CM32 ? 4 : 10;
    s->rtcp_tag = s->gcm ? 16 : 10;
    memcpy(s->rtp_salt, rtp_salt, slen);
    memcpy(s->rtcp_salt, rtcp_salt, slen);
    s->rtp_c = srtp_cipher_new(s->gcm, klen, rtp_key);
    s->rtcp_c = srtp_cipher_new(s->gcm, klen, rtcp_key);
    if (!s->gcm) {
        s->rtp_h = rtp_auth ? srtp_hmac_new(rtp_auth) : NULL;
        s->rtcp_h = rtcp_auth ? srtp_hmac_new(rtcp_auth) : NULL;
    }
    if (!s->rtp_c || !s->rtcp_c || (!s->gcm && (!s->rtp_h || !s->rtcp_h))) {
        wrt_srtp_free(s);
        return WRT_SRTP_ERR_STATE;
    }
    return WRT_SRTP_OK;
}

int wrt_srtp_init(wrt_srtp* s, int profile, const uint8_t* master_key, const uint8_t* master_salt)
{
    size_t klen, slen;
    uint8_t k[2][32], a[2][20], sa[2][14];
    int i, r = WRT_SRTP_ERR_STATE;
    memset(s, 0, sizeof(*s));
    if (wrt_srtp_profile_lengths(profile, &klen, &slen) < 0)
        return r;
    for (i = 0; i < 2; i++) {
        uint8_t base = (uint8_t)(i * 3);
        if (wrt_srtp_kdf(profile, master_key, master_salt, base, k[i], klen) < 0
            || wrt_srtp_kdf(profile, master_key, master_salt, (uint8_t)(base + 1), a[i], 20) < 0
            || wrt_srtp_kdf(profile, master_key, master_salt, (uint8_t)(base + 2), sa[i], slen) < 0)
            goto done;
    }
    r = wrt_srtp_init_session(s, profile, k[0], sa[0], a[0], k[1], sa[1], a[1]);
done:
    OPENSSL_cleanse(k, sizeof(k));
    OPENSSL_cleanse(a, sizeof(a));
    OPENSSL_cleanse(sa, sizeof(sa));
    return r;
}

int wrt_srtp_init_dtls(wrt_srtp* tx, wrt_srtp* rx, int profile, const uint8_t* block, size_t len, int is_client)
{
    size_t kl, sl;
    const uint8_t *lk, *ls, *rk, *rs;
    memset(tx, 0, sizeof(*tx));
    memset(rx, 0, sizeof(*rx));
    if (wrt_srtp_profile_lengths(profile, &kl, &sl) < 0 || len != 2 * (kl + sl))
        return WRT_SRTP_ERR_STATE;
    lk = block + (is_client ? 0 : kl);
    rk = block + (is_client ? kl : 0);
    ls = block + 2 * kl + (is_client ? 0 : sl);
    rs = block + 2 * kl + (is_client ? sl : 0);
    if (wrt_srtp_init(tx, profile, lk, ls) < 0)
        return WRT_SRTP_ERR_STATE;
    if (wrt_srtp_init(rx, profile, rk, rs) < 0) {
        wrt_srtp_free(tx);
        return WRT_SRTP_ERR_STATE;
    }
    return WRT_SRTP_OK;
}

void wrt_srtp_free(wrt_srtp* s)
{
    EVP_CIPHER_CTX_free((EVP_CIPHER_CTX*)s->rtp_c);
    EVP_CIPHER_CTX_free((EVP_CIPHER_CTX*)s->rtcp_c);
    EVP_MAC_CTX_free((EVP_MAC_CTX*)s->rtp_h);
    EVP_MAC_CTX_free((EVP_MAC_CTX*)s->rtcp_h);
    free(s->st);
    OPENSSL_cleanse(s, sizeof(*s));
}

static wrt_srtp_stream* srtp_find(wrt_srtp* s, uint32_t ssrc)
{
    uint32_t i;
    for (i = 0; i < s->nst; i++)
        if (s->st[i].ssrc == ssrc)
            return &s->st[i];
    return NULL;
}

static wrt_srtp_stream* srtp_add(wrt_srtp* s, uint32_t ssrc)
{
    wrt_srtp_stream* t;
    if (s->nst == s->cap) {
        uint32_t nc = s->cap ? s->cap * 2 : 2;
        wrt_srtp_stream* n;
        if (s->cap >= WRT_SRTP_MAX_STREAMS)
            return NULL;
        if (nc > WRT_SRTP_MAX_STREAMS)
            nc = WRT_SRTP_MAX_STREAMS;
        n = (wrt_srtp_stream*)realloc(s->st, (size_t)nc * sizeof(*n));
        if (!n)
            return NULL;
        s->st = n;
        s->cap = nc;
    }
    t = &s->st[s->nst++];
    memset(t, 0, sizeof(*t));
    t->ssrc = ssrc;
    return t;
}

static int replay_check(const wrt_srtp_replay* r, uint64_t idx)
{
    uint64_t b;
    if (!r->init || idx > r->top)
        return WRT_SRTP_OK;
    if (r->top - idx >= WRT_SRTP_WINDOW)
        return WRT_SRTP_ERR_REPLAY;
    b = idx % WRT_SRTP_WINDOW;
    return (r->win[b >> 6] >> (b & 63)) & 1 ? WRT_SRTP_ERR_REPLAY : WRT_SRTP_OK;
}

static void replay_add(wrt_srtp_replay* r, uint64_t idx)
{
    uint64_t b;
    if (!r->init) {
        memset(r->win, 0, sizeof(r->win));
        r->init = 1;
        r->top = idx;
    } else if (idx > r->top) {
        uint64_t d = idx - r->top, k;
        if (d >= WRT_SRTP_WINDOW)
            memset(r->win, 0, sizeof(r->win));
        else
            for (k = 1; k <= d; k++) {
                b = (r->top + k) % WRT_SRTP_WINDOW;
                r->win[b >> 6] &= ~((uint64_t)1 << (b & 63));
            }
        r->top = idx;
    }
    b = idx % WRT_SRTP_WINDOW;
    r->win[b >> 6] |= (uint64_t)1 << (b & 63);
}

static int srtp_guess(const wrt_srtp_stream* t, uint16_t seq, uint32_t* v)
{
    if (!t || !t->init) {
        *v = t ? t->roc : 0;
        return 0;
    }
    if (t->s_l < 32768) {
        if ((int32_t)seq - (int32_t)t->s_l > 32768) {
            if (t->roc == 0)
                return -1;
            *v = t->roc - 1;
        } else
            *v = t->roc;
    } else {
        if ((int32_t)t->s_l - 32768 > (int32_t)seq) {
            if (t->roc == UINT32_MAX)
                return -1;
            *v = t->roc + 1;
        } else
            *v = t->roc;
    }
    return 0;
}

static void srtp_advance(wrt_srtp_stream* t, uint32_t v, uint16_t seq)
{
    uint64_t n = (uint64_t)v << 16 | seq, c = (uint64_t)t->roc << 16 | t->s_l;
    if (!t->init || n > c) {
        t->roc = v;
        t->s_l = seq;
        t->init = 1;
    }
}

static int rtp_hlen(const uint8_t* p, size_t n, size_t* hl)
{
    size_t h;
    if (n < 12 || (p[0] >> 6) != 2)
        return -1;
    h = 12 + 4u * (p[0] & 15u);
    if (p[0] & 0x10) {
        if (h + 4 > n)
            return -1;
        h += 4 + 4u * (((size_t)p[h + 2] << 8) | p[h + 3]);
    }
    if (h > n)
        return -1;
    *hl = h;
    return 0;
}

static void cm_iv(uint8_t* iv, const uint8_t* salt, uint32_t ssrc, uint64_t idx)
{
    int i;
    memcpy(iv, salt, 14);
    iv[14] = iv[15] = 0;
    iv[4] ^= (uint8_t)(ssrc >> 24);
    iv[5] ^= (uint8_t)(ssrc >> 16);
    iv[6] ^= (uint8_t)(ssrc >> 8);
    iv[7] ^= (uint8_t)ssrc;
    for (i = 0; i < 6; i++)
        iv[8 + i] ^= (uint8_t)(idx >> (40 - 8 * i));
}

static void gcm_iv(uint8_t* iv, const uint8_t* salt, uint32_t ssrc, uint32_t hi, uint32_t lo, int rtcp)
{
    int i;
    iv[0] = iv[1] = 0;
    srtp_wr32(iv + 2, ssrc);
    if (rtcp) {
        iv[6] = iv[7] = 0;
        srtp_wr32(iv + 8, lo & 0x7FFFFFFFu);
    } else {
        srtp_wr32(iv + 6, hi);
        iv[10] = (uint8_t)(lo >> 8);
        iv[11] = (uint8_t)lo;
    }
    for (i = 0; i < 12; i++)
        iv[i] ^= salt[i];
}

static int ctr_xor(void* cv, const uint8_t* iv, uint8_t* p, size_t n)
{
    EVP_CIPHER_CTX* c = (EVP_CIPHER_CTX*)cv;
    int l = 0;
    if (EVP_CipherInit_ex(c, NULL, NULL, NULL, iv, 1) != 1)
        return -1;
    if (n && EVP_CipherUpdate(c, p, &l, p, (int)n) != 1)
        return -1;
    return 0;
}

static int hmac_tag(void* hv, const uint8_t* p, size_t n, const uint8_t* tail, size_t tn, uint8_t* out)
{
    EVP_MAC_CTX* h = (EVP_MAC_CTX*)hv;
    size_t ol = 0;
    if (EVP_MAC_init(h, NULL, 0, NULL) != 1 || EVP_MAC_update(h, p, n) != 1 || (tn && EVP_MAC_update(h, tail, tn) != 1)
        || EVP_MAC_final(h, out, &ol, 20) != 1 || ol != 20)
        return -1;
    return 0;
}

static int gcm_seal(void* cv, const uint8_t* iv, const uint8_t* aad, size_t an, const uint8_t* aad2, size_t an2, uint8_t* p,
    size_t n, uint8_t* tag)
{
    EVP_CIPHER_CTX* c = (EVP_CIPHER_CTX*)cv;
    int l = 0;
    if (EVP_CipherInit_ex(c, NULL, NULL, NULL, iv, 1) != 1 || (an && EVP_CipherUpdate(c, NULL, &l, aad, (int)an) != 1)
        || (an2 && EVP_CipherUpdate(c, NULL, &l, aad2, (int)an2) != 1) || (n && EVP_CipherUpdate(c, p, &l, p, (int)n) != 1)
        || EVP_CipherFinal_ex(c, p + n, &l) != 1 || EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_GET_TAG, 16, tag) != 1)
        return -1;
    return 0;
}

static int gcm_open(void* cv, const uint8_t* iv, const uint8_t* aad, size_t an, const uint8_t* aad2, size_t an2, uint8_t* p,
    size_t n, const uint8_t* tag)
{
    EVP_CIPHER_CTX* c = (EVP_CIPHER_CTX*)cv;
    uint8_t t[16];
    int l = 0;
    memcpy(t, tag, 16);
    if (EVP_CipherInit_ex(c, NULL, NULL, NULL, iv, 0) != 1 || (an && EVP_CipherUpdate(c, NULL, &l, aad, (int)an) != 1)
        || (an2 && EVP_CipherUpdate(c, NULL, &l, aad2, (int)an2) != 1) || (n && EVP_CipherUpdate(c, p, &l, p, (int)n) != 1)
        || EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_SET_TAG, 16, t) != 1 || EVP_CipherFinal_ex(c, p + n, &l) != 1)
        return -1;
    return 0;
}

int wrt_srtp_protect(wrt_srtp* s, uint8_t* pkt, size_t len, size_t cap, size_t* out_len)
{
    size_t hl;
    uint32_t ssrc, v;
    uint16_t seq;
    uint64_t idx;
    wrt_srtp_stream* t;
    if (!s->rtp_c)
        return WRT_SRTP_ERR_STATE;
    if (rtp_hlen(pkt, len, &hl) < 0)
        return WRT_SRTP_ERR_PARSE;
    if (cap < len + s->tag)
        return WRT_SRTP_ERR_SPACE;
    ssrc = srtp_rd32(pkt + 8);
    seq = (uint16_t)(pkt[2] << 8 | pkt[3]);
    t = srtp_find(s, ssrc);
    if (!t && !(t = srtp_add(s, ssrc)))
        return WRT_SRTP_ERR_LIMIT;
    if (srtp_guess(t, seq, &v) < 0)
        return WRT_SRTP_ERR_LIMIT;
    idx = (uint64_t)v << 16 | seq;
    if (t->init && idx <= ((uint64_t)t->roc << 16 | t->s_l))
        return WRT_SRTP_ERR_REPLAY;
    if (s->gcm) {
        uint8_t iv[12];
        gcm_iv(iv, s->rtp_salt, ssrc, v, seq, 0);
        if (gcm_seal(s->rtp_c, iv, pkt, hl, NULL, 0, pkt + hl, len - hl, pkt + len) < 0)
            return WRT_SRTP_ERR_STATE;
    } else {
        uint8_t iv[16], roc[4], mac[20];
        cm_iv(iv, s->rtp_salt, ssrc, idx);
        if (ctr_xor(s->rtp_c, iv, pkt + hl, len - hl) < 0)
            return WRT_SRTP_ERR_STATE;
        srtp_wr32(roc, v);
        if (hmac_tag(s->rtp_h, pkt, len, roc, 4, mac) < 0)
            return WRT_SRTP_ERR_STATE;
        memcpy(pkt + len, mac, s->tag);
    }
    srtp_advance(t, v, seq);
    *out_len = len + s->tag;
    return WRT_SRTP_OK;
}

int wrt_srtp_unprotect(wrt_srtp* s, uint8_t* pkt, size_t len, size_t* out_len)
{
    size_t hl, body;
    uint32_t ssrc, v;
    uint16_t seq;
    uint64_t idx;
    wrt_srtp_stream* t;
    int r;
    if (!s->rtp_c)
        return WRT_SRTP_ERR_STATE;
    if (rtp_hlen(pkt, len, &hl) < 0 || len < hl + s->tag)
        return WRT_SRTP_ERR_PARSE;
    body = len - s->tag;
    ssrc = srtp_rd32(pkt + 8);
    seq = (uint16_t)(pkt[2] << 8 | pkt[3]);
    t = srtp_find(s, ssrc);
    if (srtp_guess(t, seq, &v) < 0)
        return WRT_SRTP_ERR_REPLAY;
    idx = (uint64_t)v << 16 | seq;
    if (t && (r = replay_check(&t->rtp, idx)) != WRT_SRTP_OK)
        return r;
    if (s->gcm) {
        uint8_t iv[12];
        gcm_iv(iv, s->rtp_salt, ssrc, v, seq, 0);
        if (gcm_open(s->rtp_c, iv, pkt, hl, NULL, 0, pkt + hl, body - hl, pkt + body) < 0)
            return WRT_SRTP_ERR_AUTH;
    } else {
        uint8_t iv[16], roc[4], mac[20];
        srtp_wr32(roc, v);
        if (hmac_tag(s->rtp_h, pkt, body, roc, 4, mac) < 0)
            return WRT_SRTP_ERR_STATE;
        if (CRYPTO_memcmp(mac, pkt + body, s->tag) != 0)
            return WRT_SRTP_ERR_AUTH;
        cm_iv(iv, s->rtp_salt, ssrc, idx);
        if (ctr_xor(s->rtp_c, iv, pkt + hl, body - hl) < 0)
            return WRT_SRTP_ERR_STATE;
    }
    if (!t && !(t = srtp_add(s, ssrc)))
        return WRT_SRTP_ERR_LIMIT;
    replay_add(&t->rtp, idx);
    srtp_advance(t, v, seq);
    *out_len = body;
    return WRT_SRTP_OK;
}

static int srtcp_protect(wrt_srtp* s, uint8_t* pkt, size_t len, size_t cap, size_t* out_len, int encrypt)
{
    uint32_t ssrc, idx, word;
    wrt_srtp_stream* t;
    if (!s->rtcp_c)
        return WRT_SRTP_ERR_STATE;
    if (len < 8 || (pkt[0] >> 6) != 2)
        return WRT_SRTP_ERR_PARSE;
    if (cap < len + 4 + s->rtcp_tag)
        return WRT_SRTP_ERR_SPACE;
    ssrc = srtp_rd32(pkt + 4);
    t = srtp_find(s, ssrc);
    if (!t && !(t = srtp_add(s, ssrc)))
        return WRT_SRTP_ERR_LIMIT;
    idx = t->rtcp_next;
    if (idx > 0x7FFFFFFFu)
        return WRT_SRTP_ERR_LIMIT;
    word = idx | (encrypt ? 0x80000000u : 0);
    if (s->gcm) {
        uint8_t iv[12], w[4];
        srtp_wr32(w, word);
        gcm_iv(iv, s->rtcp_salt, ssrc, 0, idx, 1);
        if (encrypt) {
            if (gcm_seal(s->rtcp_c, iv, pkt, 8, w, 4, pkt + 8, len - 8, pkt + len) < 0)
                return WRT_SRTP_ERR_STATE;
        } else if (gcm_seal(s->rtcp_c, iv, pkt, len, w, 4, NULL, 0, pkt + len) < 0)
            return WRT_SRTP_ERR_STATE;
        memcpy(pkt + len + 16, w, 4);
        *out_len = len + 20;
    } else {
        uint8_t iv[16], mac[20];
        if (encrypt) {
            cm_iv(iv, s->rtcp_salt, ssrc, idx);
            if (ctr_xor(s->rtcp_c, iv, pkt + 8, len - 8) < 0)
                return WRT_SRTP_ERR_STATE;
        }
        srtp_wr32(pkt + len, word);
        if (hmac_tag(s->rtcp_h, pkt, len + 4, NULL, 0, mac) < 0)
            return WRT_SRTP_ERR_STATE;
        memcpy(pkt + len + 4, mac, s->rtcp_tag);
        *out_len = len + 4 + s->rtcp_tag;
    }
    t->rtcp_next = idx + 1;
    return WRT_SRTP_OK;
}

int wrt_srtcp_protect(wrt_srtp* s, uint8_t* pkt, size_t len, size_t cap, size_t* out_len)
{
    return srtcp_protect(s, pkt, len, cap, out_len, 1);
}

int wrt_srtcp_protect_tag_only(wrt_srtp* s, uint8_t* pkt, size_t len, size_t cap, size_t* out_len)
{
    return srtcp_protect(s, pkt, len, cap, out_len, 0);
}

int wrt_srtcp_unprotect(wrt_srtp* s, uint8_t* pkt, size_t len, size_t* out_len)
{
    uint32_t ssrc, word, idx;
    size_t body;
    int enc, r;
    wrt_srtp_stream* t;
    if (!s->rtcp_c)
        return WRT_SRTP_ERR_STATE;
    if (len < 8 + 4 + (size_t)s->rtcp_tag || (pkt[0] >> 6) != 2)
        return WRT_SRTP_ERR_PARSE;
    ssrc = srtp_rd32(pkt + 4);
    if (s->gcm) {
        word = srtp_rd32(pkt + len - 4);
        body = len - 4 - 16;
    } else {
        word = srtp_rd32(pkt + len - s->rtcp_tag - 4);
        body = len - s->rtcp_tag - 4;
    }
    enc = (word >> 31) & 1;
    idx = word & 0x7FFFFFFFu;
    t = srtp_find(s, ssrc);
    if (t && (r = replay_check(&t->rtcp, idx)) != WRT_SRTP_OK)
        return r;
    if (s->gcm) {
        uint8_t iv[12], w[4];
        memcpy(w, pkt + len - 4, 4);
        gcm_iv(iv, s->rtcp_salt, ssrc, 0, idx, 1);
        if (enc) {
            if (gcm_open(s->rtcp_c, iv, pkt, 8, w, 4, pkt + 8, body - 8, pkt + body) < 0)
                return WRT_SRTP_ERR_AUTH;
        } else if (gcm_open(s->rtcp_c, iv, pkt, body, w, 4, NULL, 0, pkt + body) < 0)
            return WRT_SRTP_ERR_AUTH;
    } else {
        uint8_t iv[16], mac[20];
        if (hmac_tag(s->rtcp_h, pkt, body + 4, NULL, 0, mac) < 0)
            return WRT_SRTP_ERR_STATE;
        if (CRYPTO_memcmp(mac, pkt + body + 4, s->rtcp_tag) != 0)
            return WRT_SRTP_ERR_AUTH;
        if (enc) {
            cm_iv(iv, s->rtcp_salt, ssrc, idx);
            if (ctr_xor(s->rtcp_c, iv, pkt + 8, body - 8) < 0)
                return WRT_SRTP_ERR_STATE;
        }
    }
    if (!t && !(t = srtp_add(s, ssrc)))
        return WRT_SRTP_ERR_LIMIT;
    replay_add(&t->rtcp, idx);
    *out_len = body;
    return WRT_SRTP_OK;
}

int wrt_srtp_set_roc(wrt_srtp* s, uint32_t ssrc, uint32_t roc, uint16_t seq)
{
    wrt_srtp_stream* t = srtp_find(s, ssrc);
    if (!t && !(t = srtp_add(s, ssrc)))
        return WRT_SRTP_ERR_LIMIT;
    t->roc = roc;
    t->s_l = seq;
    t->init = 1;
    return WRT_SRTP_OK;
}

int wrt_srtp_set_rtcp_index(wrt_srtp* s, uint32_t ssrc, uint32_t next)
{
    wrt_srtp_stream* t = srtp_find(s, ssrc);
    if (!t && !(t = srtp_add(s, ssrc)))
        return WRT_SRTP_ERR_LIMIT;
    t->rtcp_next = next;
    return WRT_SRTP_OK;
}
