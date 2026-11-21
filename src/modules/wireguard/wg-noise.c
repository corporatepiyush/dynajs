#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_WIREGUARD)

#include "wg-internal.h"

#include <string.h>
#include <openssl/crypto.h>

static const char construction[] = "Noise_IKpsk2_25519_ChaChaPoly_BLAKE2s";
static const char identifier[] = "WireGuard v1 zx2c4 Jason@zx2c4.com";
static const char label_mac1[] = "mac1----";
static const char label_cookie[] = "cookie--";

static void initial_ck(uint8_t ck[WG_KEY])
{
    wg_blake2s(ck, WG_KEY, NULL, 0, (const uint8_t*)construction, sizeof construction - 1);
}

static void hash0_for(uint8_t out[WG_KEY], const uint8_t pub[WG_KEY])
{
    uint8_t ck[WG_KEY], h[WG_KEY];
    initial_ck(ck);
    wg_hash2(h, ck, WG_KEY, (const uint8_t*)identifier, sizeof identifier - 1);
    wg_hash2(out, h, WG_KEY, pub, WG_KEY);
}

static void label_key(uint8_t out[WG_KEY], const char* label, const uint8_t pub[WG_KEY])
{
    wg_hash2(out, (const uint8_t*)label, 8, pub, WG_KEY);
}

static void mix_hash(uint8_t h[WG_KEY], const uint8_t* p, size_t n)
{
    wg_hash2(h, h, WG_KEY, p, n);
}

int wg_static_init(wg_static_t* s, const uint8_t priv[WG_KEY])
{
    memset(s, 0, sizeof *s);
    memcpy(s->priv, priv, WG_KEY);
    s->pk = wg_pkey_new(priv);
    if (!s->pk || wg_x25519_pub(s->pub, priv) < 0) {
        wg_static_free(s);
        return -1;
    }
    hash0_for(s->hash0, s->pub);
    label_key(s->mac1_key, label_mac1, s->pub);
    label_key(s->cookie_key, label_cookie, s->pub);
    return 0;
}

void wg_static_free(wg_static_t* s)
{
    wg_pkey_free(s->pk);
    wg_wipe(s, sizeof *s);
}

int wg_peer_init(wg_peer_t* p, const wg_static_t* s, const uint8_t pub[WG_KEY], const uint8_t* psk)
{
    memset(p, 0, sizeof *p);
    memcpy(p->pub, pub, WG_KEY);
    if (psk)
        memcpy(p->psk, psk, WG_KEY);
    if (wg_x25519_static(p->ss, s, pub) < 0) {
        wg_wipe(p, sizeof *p);
        return -1;
    }
    hash0_for(p->hash0, pub);
    label_key(p->mac1_key, label_mac1, pub);
    label_key(p->cookie_key, label_cookie, pub);
    return 0;
}

void wg_sym_start(wg_sym_t* st, const uint8_t* prologue, size_t pn, const uint8_t rs[WG_KEY])
{
    initial_ck(st->ck);
    wg_hash2(st->h, st->ck, WG_KEY, prologue, pn);
    mix_hash(st->h, rs, WG_KEY);
}

void wg_sym_start_h0(wg_sym_t* st, const uint8_t hash0[WG_KEY])
{
    initial_ck(st->ck);
    memcpy(st->h, hash0, WG_KEY);
}

static void mix_e(wg_sym_t* st, const uint8_t e[WG_KEY])
{
    wg_kdf(st->ck, NULL, NULL, st->ck, e, WG_KEY);
    mix_hash(st->h, e, WG_KEY);
}

static int enc_hash(wg_sym_t* st, const uint8_t k[WG_KEY], const uint8_t* pt, size_t n, uint8_t* out)
{
    if (wg_aead_seal(k, 0, pt, n, st->h, WG_KEY, out) < 0)
        return -1;
    mix_hash(st->h, out, n + WG_TAG);
    return 0;
}

static int dec_hash(wg_sym_t* st, const uint8_t k[WG_KEY], const uint8_t* ct, size_t n, uint8_t* out)
{
    uint8_t none[1];
    if (n < WG_TAG || wg_aead_open(k, 0, ct, n, st->h, WG_KEY, n > WG_TAG ? out : none) < 0)
        return -1;
    mix_hash(st->h, ct, n);
    return 0;
}

int wg_noise_init_write(wg_sym_t* st, const void* e_pk, const uint8_t e_pub[WG_KEY], const uint8_t rs[WG_KEY], const uint8_t s_pub[WG_KEY], const uint8_t ss[WG_KEY], const uint8_t* pl, size_t pn, uint8_t* out)
{
    uint8_t k[WG_KEY], dh[WG_KEY];
    int rc = -1;
    memcpy(out, e_pub, WG_KEY);
    mix_e(st, e_pub);
    if (wg_x25519_pk(dh, e_pk, rs) < 0)
        goto done;
    wg_kdf(st->ck, k, NULL, st->ck, dh, WG_KEY);
    if (enc_hash(st, k, s_pub, WG_KEY, out + WG_KEY) < 0)
        goto done;
    wg_kdf(st->ck, k, NULL, st->ck, ss, WG_KEY);
    rc = enc_hash(st, k, pl, pn, out + 2 * WG_KEY + WG_TAG);
done:
    wg_wipe(k, sizeof k);
    wg_wipe(dh, sizeof dh);
    return rc;
}

int wg_noise_init_read_s(wg_sym_t* st, const wg_static_t* s, const uint8_t* in, uint8_t s_pub[WG_KEY])
{
    uint8_t k[WG_KEY], dh[WG_KEY];
    int rc = -1;
    mix_e(st, in);
    if (wg_x25519_static(dh, s, in) < 0)
        goto done;
    wg_kdf(st->ck, k, NULL, st->ck, dh, WG_KEY);
    rc = dec_hash(st, k, in + WG_KEY, WG_KEY + WG_TAG, s_pub);
done:
    wg_wipe(k, sizeof k);
    wg_wipe(dh, sizeof dh);
    return rc;
}

int wg_noise_init_read_pl(wg_sym_t* st, const uint8_t ss[WG_KEY], const uint8_t* ct, size_t ctn, uint8_t* pl)
{
    uint8_t k[WG_KEY];
    int rc;
    wg_kdf(st->ck, k, NULL, st->ck, ss, WG_KEY);
    rc = dec_hash(st, k, ct, ctn, pl);
    wg_wipe(k, sizeof k);
    return rc;
}

static void mix_psk(wg_sym_t* st, const uint8_t psk[WG_KEY], uint8_t k[WG_KEY])
{
    uint8_t t[WG_KEY];
    wg_kdf(st->ck, t, k, st->ck, psk, WG_KEY);
    mix_hash(st->h, t, WG_KEY);
    wg_wipe(t, sizeof t);
}

int wg_noise_resp_write(wg_sym_t* st, const void* e_pk, const uint8_t e_pub[WG_KEY], const uint8_t re[WG_KEY], const uint8_t rs[WG_KEY], const uint8_t psk[WG_KEY], const uint8_t* pl, size_t pn, uint8_t* out)
{
    uint8_t k[WG_KEY], dh[WG_KEY];
    int rc = -1;
    memcpy(out, e_pub, WG_KEY);
    mix_e(st, e_pub);
    if (wg_x25519_pk(dh, e_pk, re) < 0)
        goto done;
    wg_kdf(st->ck, NULL, NULL, st->ck, dh, WG_KEY);
    if (wg_x25519_pk(dh, e_pk, rs) < 0)
        goto done;
    wg_kdf(st->ck, NULL, NULL, st->ck, dh, WG_KEY);
    mix_psk(st, psk, k);
    rc = enc_hash(st, k, pl, pn, out + WG_KEY);
done:
    wg_wipe(k, sizeof k);
    wg_wipe(dh, sizeof dh);
    return rc;
}

int wg_noise_resp_read(wg_sym_t* st, const void* e_pk, const wg_static_t* s, const uint8_t psk[WG_KEY], const uint8_t* in, size_t n, uint8_t* pl)
{
    uint8_t k[WG_KEY], dh[WG_KEY];
    int rc = -1;
    if (n < WG_KEY + WG_TAG)
        return -1;
    mix_e(st, in);
    if (wg_x25519_pk(dh, e_pk, in) < 0)
        goto done;
    wg_kdf(st->ck, NULL, NULL, st->ck, dh, WG_KEY);
    if (wg_x25519_static(dh, s, in) < 0)
        goto done;
    wg_kdf(st->ck, NULL, NULL, st->ck, dh, WG_KEY);
    mix_psk(st, psk, k);
    rc = dec_hash(st, k, in + WG_KEY, n - WG_KEY, pl);
done:
    wg_wipe(k, sizeof k);
    wg_wipe(dh, sizeof dh);
    return rc;
}

void wg_noise_split(const wg_sym_t* st, uint8_t k1[WG_KEY], uint8_t k2[WG_KEY])
{
    wg_kdf(k1, k2, NULL, st->ck, NULL, 0);
}

static void hs_clear(wg_handshake_t* hs)
{
    wg_wipe(hs, sizeof *hs);
}

static void add_macs(wg_peer_t* p, uint8_t* msg, size_t n, uint64_t now_ms)
{
    uint8_t* mac1 = msg + n - 2 * WG_MACLEN;
    uint8_t* mac2 = msg + n - WG_MACLEN;
    wg_mac(mac1, p->mac1_key, WG_KEY, msg, n - 2 * WG_MACLEN);
    memcpy(p->last_mac1, mac1, WG_MACLEN);
    p->has_last_mac1 = 1;
    if (p->has_cookie && now_ms - p->cookie_ms < WG_COOKIE_VALID_MS)
        wg_mac(mac2, p->cookie, WG_MACLEN, msg, n - WG_MACLEN);
    else
        memset(mac2, 0, WG_MACLEN);
}

int wg_create_initiation(const wg_static_t* s, wg_peer_t* p, uint32_t local_index, uint64_t unix_ns, uint64_t now_ms, uint8_t out[WG_INIT_LEN])
{
    wg_handshake_t* hs = &p->hs;
    wg_sym_t st;
    uint8_t ts[12];
    void* pk;
    int rc;
    hs_clear(hs);
    memset(out, 0, WG_INIT_LEN);
    wg_sym_start_h0(&st, p->hash0);
    wg_tai64n(ts, unix_ns);
    pk = wg_keygen_pk(hs->e_priv, hs->e_pub);
    rc = pk ? wg_noise_init_write(&st, pk, hs->e_pub, p->pub, s->pub, p->ss, ts, sizeof ts, out + 8) : -1;
    wg_pkey_free(pk);
    if (rc < 0) {
        wg_wipe(&st, sizeof st);
        hs_clear(hs);
        return -1;
    }
    out[0] = WG_MSG_INIT;
    wg_put_le32(out + 4, local_index);
    memcpy(hs->ck, st.ck, WG_KEY);
    memcpy(hs->h, st.h, WG_KEY);
    wg_wipe(&st, sizeof st);
    hs->local_index = local_index;
    hs->state = WG_HS_CREATED_INIT;
    add_macs(p, out, WG_INIT_LEN, now_ms);
    return 0;
}

wg_peer_t* wg_consume_initiation(const wg_static_t* s, const uint8_t* msg, size_t n, uint64_t now_ms, wg_by_pub_fn lookup, void* ud)
{
    wg_sym_t st;
    uint8_t spub[WG_KEY], ts[12];
    wg_peer_t* p = NULL;
    if (n != WG_INIT_LEN || wg_le32(msg) != WG_MSG_INIT)
        return NULL;
    wg_sym_start_h0(&st, s->hash0);
    if (wg_noise_init_read_s(&st, s, msg + 8, spub) < 0)
        goto fail;
    p = lookup(ud, spub);
    if (!p || wg_noise_init_read_pl(&st, p->ss, msg + 88, sizeof ts + WG_TAG, ts) < 0)
        goto fail;
    if (memcmp(ts, p->greatest_ts, sizeof ts) <= 0 || (p->has_last_init && now_ms - p->last_init_ms < WG_INIT_MIN_GAP_MS))
        goto fail;
    memcpy(p->greatest_ts, ts, sizeof ts);
    p->last_init_ms = now_ms;
    p->has_last_init = 1;
    hs_clear(&p->hs);
    memcpy(p->hs.ck, st.ck, WG_KEY);
    memcpy(p->hs.h, st.h, WG_KEY);
    memcpy(p->hs.remote_e, msg + 8, WG_KEY);
    p->hs.remote_index = wg_le32(msg + 4);
    p->hs.state = WG_HS_CONSUMED_INIT;
    goto done;
fail:
    p = NULL;
done:
    wg_wipe(&st, sizeof st);
    return p;
}

int wg_create_response(const wg_static_t* s, wg_peer_t* p, uint32_t local_index, uint64_t now_ms, uint8_t out[WG_RESP_LEN])
{
    wg_handshake_t* hs = &p->hs;
    wg_sym_t st;
    void* pk;
    int rc;
    (void)s;
    memset(out, 0, WG_RESP_LEN);
    if (hs->state != WG_HS_CONSUMED_INIT)
        return -1;
    memcpy(st.ck, hs->ck, WG_KEY);
    memcpy(st.h, hs->h, WG_KEY);
    pk = wg_keygen_pk(hs->e_priv, hs->e_pub);
    rc = pk ? wg_noise_resp_write(&st, pk, hs->e_pub, hs->remote_e, p->pub, p->psk, NULL, 0, out + 12) : -1;
    wg_pkey_free(pk);
    if (rc < 0) {
        wg_wipe(&st, sizeof st);
        hs_clear(hs);
        return -1;
    }
    out[0] = WG_MSG_RESP;
    wg_put_le32(out + 4, local_index);
    wg_put_le32(out + 8, hs->remote_index);
    memcpy(hs->ck, st.ck, WG_KEY);
    memcpy(hs->h, st.h, WG_KEY);
    wg_wipe(&st, sizeof st);
    hs->local_index = local_index;
    hs->state = WG_HS_CREATED_RESP;
    add_macs(p, out, WG_RESP_LEN, now_ms);
    return 0;
}

wg_peer_t* wg_consume_response(const wg_static_t* s, const uint8_t* msg, size_t n, wg_by_index_fn lookup, void* ud)
{
    wg_sym_t st;
    wg_peer_t* p;
    void* pk;
    int ok;
    if (n != WG_RESP_LEN || wg_le32(msg) != WG_MSG_RESP)
        return NULL;
    p = lookup(ud, wg_le32(msg + 8));
    if (!p || p->hs.state != WG_HS_CREATED_INIT || p->hs.local_index != wg_le32(msg + 8))
        return NULL;
    memcpy(st.ck, p->hs.ck, WG_KEY);
    memcpy(st.h, p->hs.h, WG_KEY);
    pk = wg_pkey_new(p->hs.e_priv);
    ok = pk && wg_noise_resp_read(&st, pk, s, p->psk, msg + 12, WG_KEY + WG_TAG, NULL) == 0;
    wg_pkey_free(pk);
    if (ok) {
        memcpy(p->hs.ck, st.ck, WG_KEY);
        memcpy(p->hs.h, st.h, WG_KEY);
        p->hs.remote_index = wg_le32(msg + 4);
        p->hs.state = WG_HS_CONSUMED_RESP;
    }
    wg_wipe(&st, sizeof st);
    return ok ? p : NULL;
}

int wg_derive_keys(wg_peer_t* p, uint8_t send[WG_KEY], uint8_t recv[WG_KEY])
{
    wg_handshake_t* hs = &p->hs;
    wg_sym_t st;
    memcpy(st.ck, hs->ck, WG_KEY);
    if (hs->state == WG_HS_CONSUMED_RESP)
        wg_noise_split(&st, send, recv);
    else if (hs->state == WG_HS_CREATED_RESP)
        wg_noise_split(&st, recv, send);
    else {
        wg_wipe(&st, sizeof st);
        return -1;
    }
    wg_wipe(&st, sizeof st);
    wg_wipe(hs->ck, WG_KEY);
    wg_wipe(hs->h, WG_KEY);
    wg_wipe(hs->e_priv, WG_KEY);
    wg_wipe(hs->e_pub, WG_KEY);
    wg_wipe(hs->remote_e, WG_KEY);
    hs->state = WG_HS_NONE;
    return 0;
}

static void cookie_value(wg_cookie_gen_t* g, uint64_t now_ms, const uint8_t* src, size_t srcn, uint8_t out[WG_MACLEN])
{
    if (!g->has_secret || now_ms - g->secret_ms >= WG_COOKIE_SECRET_MS) {
        if (wg_random(g->secret, WG_KEY) < 0)
            memset(g->secret, 0xa5, WG_KEY);
        g->secret_ms = now_ms;
        g->has_secret = 1;
    }
    wg_mac(out, g->secret, WG_KEY, src, srcn);
}

static int msg_len_ok(const uint8_t* msg, size_t n)
{
    return (n == WG_INIT_LEN && wg_le32(msg) == WG_MSG_INIT) || (n == WG_RESP_LEN && wg_le32(msg) == WG_MSG_RESP);
}

int wg_check_macs(const wg_static_t* s, wg_cookie_gen_t* g, uint64_t now_ms, const uint8_t* msg, size_t n, const uint8_t* src, size_t srcn, int under_load)
{
    uint8_t m[WG_MACLEN], c[WG_MACLEN];
    int ok;
    if (!msg_len_ok(msg, n))
        return WG_MAC_BAD;
    wg_mac(m, s->mac1_key, WG_KEY, msg, n - 2 * WG_MACLEN);
    if (CRYPTO_memcmp(m, msg + n - 2 * WG_MACLEN, WG_MACLEN) != 0)
        return WG_MAC_BAD;
    if (!under_load)
        return WG_MAC_OK;
    cookie_value(g, now_ms, src, srcn, c);
    wg_mac(m, c, WG_MACLEN, msg, n - WG_MACLEN);
    ok = CRYPTO_memcmp(m, msg + n - WG_MACLEN, WG_MACLEN) == 0;
    wg_wipe(c, sizeof c);
    return ok ? WG_MAC_OK : WG_MAC_NEED_COOKIE;
}

int wg_create_cookie_reply(const wg_static_t* s, wg_cookie_gen_t* g, uint64_t now_ms, const uint8_t* msg, size_t n, const uint8_t* src, size_t srcn, uint8_t out[WG_COOKIE_LEN])
{
    uint8_t c[WG_MACLEN];
    int rc;
    if (!msg_len_ok(msg, n))
        return -1;
    memset(out, 0, WG_COOKIE_LEN);
    out[0] = WG_MSG_COOKIE;
    memcpy(out + 4, msg + 4, 4);
    if (wg_random(out + 8, 24) < 0)
        return -1;
    cookie_value(g, now_ms, src, srcn, c);
    rc = wg_xaead_seal(s->cookie_key, out + 8, c, WG_MACLEN, msg + n - 2 * WG_MACLEN, WG_MACLEN, out + 32);
    wg_wipe(c, sizeof c);
    return rc;
}

wg_peer_t* wg_consume_cookie_reply(const uint8_t* msg, size_t n, uint64_t now_ms, wg_by_index_fn lookup, void* ud)
{
    wg_peer_t* p;
    uint8_t c[WG_MACLEN];
    if (n != WG_COOKIE_LEN || wg_le32(msg) != WG_MSG_COOKIE)
        return NULL;
    p = lookup(ud, wg_le32(msg + 4));
    if (!p || !p->has_last_mac1)
        return NULL;
    if (wg_xaead_open(p->cookie_key, msg + 8, msg + 32, WG_MACLEN + WG_TAG, p->last_mac1, WG_MACLEN, c) < 0)
        return NULL;
    memcpy(p->cookie, c, WG_MACLEN);
    p->cookie_ms = now_ms;
    p->has_cookie = 1;
    p->has_last_mac1 = 0;
    wg_wipe(c, sizeof c);
    return p;
}

int wg_replay_check(wg_replay_t* r, uint64_t counter)
{
    uint64_t idx, cur, bit;
    if (counter >= WG_REJECT_AFTER_MESSAGES)
        return -1;
    idx = counter >> 6;
    if (counter >= r->next) {
        uint64_t diff, i;
        cur = r->next ? (r->next - 1) >> 6 : 0;
        diff = r->next ? idx - cur : WG_RW_WORDS;
        if (diff > WG_RW_WORDS)
            diff = WG_RW_WORDS;
        for (i = 1; i <= diff; i++)
            r->bits[(cur + i) % WG_RW_WORDS] = 0;
        r->next = counter + 1;
    } else if (r->next - 1 - counter > WG_RW_WINDOW)
        return -1;
    bit = (uint64_t)1 << (counter & 63);
    if (r->bits[idx % WG_RW_WORDS] & bit)
        return -1;
    r->bits[idx % WG_RW_WORDS] |= bit;
    return 0;
}

int wg_replay_peek(const wg_replay_t* r, uint64_t counter)
{
    if (counter >= WG_REJECT_AFTER_MESSAGES)
        return -1;
    if (counter >= r->next)
        return 0;
    if (r->next - 1 - counter > WG_RW_WINDOW)
        return -1;
    return (r->bits[(counter >> 6) % WG_RW_WORDS] >> (counter & 63)) & 1 ? -1 : 0;
}

#endif
