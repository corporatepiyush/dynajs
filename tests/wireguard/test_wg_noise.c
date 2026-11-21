/*
 * test_wg_noise.c -- dyna:wireguard S13a (src/modules/wireguard/wg-crypto.c, wg-noise.c): the primitives and the
 * Noise_IKpsk2_25519_ChaChaPoly_BLAKE2s handshake of the WireGuard whitepaper (section 5.4), linked alone with OpenSSL.
 *  1. BLAKE2s: RFC 7693 known answers, and differential against OpenSSL's BLAKE2s-256 (lengths 0..300) and BLAKE2SMAC
 *     (keyed, every key length 1..32, 16- and 32-byte output). HMAC-BLAKE2s differential against OpenSSL HMAC(BLAKE2S-256)
 *     with keys shorter and longer than the 64-byte block. KDF1..3 rebuilt from HMAC by the whitepaper's definition.
 *  2. HChaCha20 (draft-irtf-cfrg-xchacha section 2.2.1); XChaCha20-Poly1305 round trip and tamper of tag, data, AD, nonce.
 *  3. X25519 RFC 7748 section 6.1; all-zero results (u = 0 and u = 1) refused.
 *  4. WireGuard's initial chaining key and hash (the constants other implementations ship).
 *  5. Full handshakes: both sides derive mirrored transport keys; transport AEAD with the counter nonce; a wrong
 *     preshared key, an unknown initiator and a wrong responder key all fail; state misuse fails.
 *  6. Replay: the same initiation twice, an older TAI64N, a second initiation within 20 ms; TAI64N rounds ns to 2^24.
 *  7. mac1/mac2 and cookies: bad mac1 dropped, under load NEED_COOKIE, cookie reply consumed once, mac2 then accepted,
 *     another source address refused, the cookie stops being used after 115 s, the secret rotates after 120 s.
 *  8. Every single-bit flip of an initiation, a response and a cookie reply is refused (sender/receiver index bytes are
 *    outside the Noise transcript, so mac1 refuses those; the AEADs refuse the rest).
 *  9. Replay window: 300000 random counters against a set model (window 1984).
 * 10. Fuzz: random lengths and bytes into every consumer (under ASan/UBSan).
 * 11. Noise test vectors: tests/corpus/noise/cacophony.txt (sha256 pinned below, listed in tests/test-data.tsv) holds one
 *     Noise_IKpsk2_25519_ChaChaPoly_BLAKE2s vector (prologue, fixed ephemerals, payloads). It drives the Noise steps
 *     wg-noise.c itself uses (wg_noise_init_write/read, wg_noise_resp_write/read, wg_noise_split): both handshake
 *     ciphertexts, the decrypted payloads, the handshake hash on both sides and the four transport messages must match.
 *     snow.txt is sha256-pinned too but has no IKpsk2 BLAKE2s vector (plain IK mixes keys differently), so only its
 *     pin is checked. A missing file prints a note; a wrong sha256 fails.
 * 12. Seeded properties (tests/lib/dyn-fuzzkit.h: DYN_FUZZ_SEED replays, DYN_FUZZ_ITERS scales, DYN_FUZZ_ONLY selects):
 *     replay-bitset: check/peek against a hash-set model over the full 64-bit space (big jumps, the exact window edge,
 *     64-bit word edges, REJECT_AFTER_MESSAGES, fresh windows); b2s-stream: random chunked BLAKE2s and keyed BLAKE2s with
 *     random key/output lengths against OpenSSL; aead-diff: ChaCha20-Poly1305 (counter nonce, full 64-bit counters) and
 *     XChaCha20-Poly1305 against OpenSSL, tampering of every part; hs-garble: multi-byte corruption, truncation,
 *     extension and type rewrites of all three handshake messages, with and without the mac1 gate (mac2 is unchecked
 *     when not under load, so mac2-only changes count as unchanged); hs-order: random interleaving of initiation
 *     creation, delivery, replay and responses against a model (TAI64N strictly greater, 20 ms gap, only the latest
 *     pending index completes, one use) with mirrored keys on every completion. Equivalent mutant: dropping the
 *     local-index compare in wg_consume_response (every lookup already selects by that index).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/params.h>

#include "../../src/modules/wireguard/wg-internal.h"
#include "../lib/dyn-fuzzkit.h"

static int fails, checks;
#define CHECK(c, msg)                                          \
    do {                                                       \
        checks++;                                              \
        if (!(c)) {                                            \
            fails++;                                           \
            printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); \
        }                                                      \
    } while (0)

static uint64_t rs = 0x9e3779b97f4a7c15ULL;
static uint64_t rnd(void)
{
    rs ^= rs << 13;
    rs ^= rs >> 7;
    rs ^= rs << 17;
    return rs;
}
static void fill(uint8_t* p, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++)
        p[i] = (uint8_t)rnd();
}
static void unhex(uint8_t* out, const char* h)
{
    size_t i;
    for (i = 0; h[2 * i]; i++) {
        unsigned v;
        sscanf(h + 2 * i, "%2x", &v);
        out[i] = (uint8_t)v;
    }
}
static int eqhex(const uint8_t* p, const char* h)
{
    uint8_t b[64];
    unhex(b, h);
    return memcmp(p, b, strlen(h) / 2) == 0;
}

static void ossl_b2s(uint8_t out[32], const uint8_t* in, size_t n)
{
    unsigned ol = 32;
    EVP_Digest(in, n, out, &ol, EVP_blake2s256(), NULL);
}
static void ossl_mac(const char* alg, const char* digest, size_t outlen, const uint8_t* key, size_t kn, const uint8_t* in, size_t n, uint8_t* out)
{
    EVP_MAC* m = EVP_MAC_fetch(NULL, alg, NULL);
    EVP_MAC_CTX* c = EVP_MAC_CTX_new(m);
    OSSL_PARAM ps[3], *p = ps;
    size_t ol = 0;
    if (digest)
        *p++ = OSSL_PARAM_construct_utf8_string(OSSL_MAC_PARAM_DIGEST, (char*)(uintptr_t)digest, 0);
    else
        *p++ = OSSL_PARAM_construct_size_t(OSSL_MAC_PARAM_SIZE, &outlen);
    *p = OSSL_PARAM_construct_end();
    EVP_MAC_init(c, key, kn, ps);
    EVP_MAC_update(c, in, n);
    EVP_MAC_final(c, out, &ol, 64);
    EVP_MAC_CTX_free(c);
    EVP_MAC_free(m);
}

static void t_blake(void)
{
    uint8_t a[32], b[32], buf[300], key[64];
    size_t n, k;
    wg_blake2s(a, 32, NULL, 0, (const uint8_t*)"abc", 3);
    CHECK(eqhex(a, "508c5e8c327c14e2e1a72ba34eeb452f37458b209ed63a294d999b4c86675982"), "BLAKE2s-256(abc) RFC 7693");
    wg_blake2s(a, 32, NULL, 0, NULL, 0);
    CHECK(eqhex(a, "69217a3079908094e11121d042354a7c1f55b6482ca1a51e1b250dfd1ed0eef9"), "BLAKE2s-256(empty)");
    fill(buf, sizeof buf);
    for (n = 0; n <= sizeof buf; n++) {
        wg_blake2s(a, 32, NULL, 0, buf, n);
        ossl_b2s(b, buf, n);
        if (memcmp(a, b, 32) != 0) {
            CHECK(0, "BLAKE2s differs from OpenSSL");
            break;
        }
    }
    checks++;
    fill(key, sizeof key);
    for (k = 1; k <= 32; k++)
        for (n = 0; n <= 200; n += 13) {
            wg_blake2s(a, 16, key, k, buf, n);
            ossl_mac("BLAKE2SMAC", NULL, 16, key, k, buf, n, b);
            CHECK(memcmp(a, b, 16) == 0, "keyed BLAKE2s-128 differs from OpenSSL BLAKE2SMAC");
            wg_blake2s(a, 32, key, k, buf, n);
            ossl_mac("BLAKE2SMAC", NULL, 32, key, k, buf, n, b);
            CHECK(memcmp(a, b, 32) == 0, "keyed BLAKE2s-256 differs from OpenSSL BLAKE2SMAC");
        }
    {
        wg_b2s_t s;
        wg_b2s_init(&s, 32, NULL, 0);
        for (n = 0; n < 300; n += 7)
            wg_b2s_update(&s, buf + n, n + 7 <= 300 ? 7 : 300 - n);
        wg_b2s_final(&s, a);
        ossl_b2s(b, buf, 300);
        CHECK(memcmp(a, b, 32) == 0, "incremental BLAKE2s");
    }
}

static void t_hmac_kdf(void)
{
    uint8_t key[100], in[150], a[32], b[32], ck[32], t1[32], t2[32], t3[32], t0[32], x[33];
    size_t kn;
    fill(key, sizeof key);
    fill(in, sizeof in);
    for (kn = 0; kn <= 100; kn += 9) {
        wg_hmac(a, key, kn, in, kn + 20);
        ossl_mac("HMAC", "BLAKE2S-256", 0, kn ? key : (const uint8_t*)"", kn, in, kn + 20, b);
        CHECK(memcmp(a, b, 32) == 0, "HMAC-BLAKE2s differs from OpenSSL");
    }
    fill(ck, 32);
    wg_kdf(t1, t2, t3, ck, in, 77);
    wg_hmac(t0, ck, 32, in, 77);
    x[0] = 1;
    wg_hmac(a, t0, 32, x, 1);
    CHECK(memcmp(a, t1, 32) == 0, "KDF tau1");
    memcpy(x, a, 32);
    x[32] = 2;
    wg_hmac(b, t0, 32, x, 33);
    CHECK(memcmp(b, t2, 32) == 0, "KDF tau2");
    memcpy(x, b, 32);
    x[32] = 3;
    wg_hmac(a, t0, 32, x, 33);
    CHECK(memcmp(a, t3, 32) == 0, "KDF tau3");
    wg_kdf(a, NULL, NULL, ck, in, 77);
    CHECK(memcmp(a, t1, 32) == 0, "KDF1 equals tau1 of KDF3");
}

static void t_chacha(void)
{
    uint8_t key[32], nonce[24], out[32], pt[100], ct[116], back[100], ad[20];
    int i;
    for (i = 0; i < 32; i++)
        key[i] = (uint8_t)i;
    unhex(nonce, "000000090000004a0000000031415927");
    wg_hchacha20(out, key, nonce);
    CHECK(eqhex(out, "82413b4227b27bfed30e42508a877d73a0f9e4d58a74a853c12ec41326d3ecdc"), "HChaCha20 draft-irtf-cfrg-xchacha 2.2.1");
    fill(nonce, 24);
    fill(pt, sizeof pt);
    fill(ad, sizeof ad);
    CHECK(wg_xaead_seal(key, nonce, pt, sizeof pt, ad, sizeof ad, ct) == 0, "xaead seal");
    CHECK(wg_xaead_open(key, nonce, ct, sizeof ct, ad, sizeof ad, back) == 0 && memcmp(back, pt, sizeof pt) == 0, "xaead round trip");
    ct[115] ^= 1;
    CHECK(wg_xaead_open(key, nonce, ct, sizeof ct, ad, sizeof ad, back) < 0, "xaead tag tamper");
    ct[115] ^= 1;
    ct[3] ^= 1;
    CHECK(wg_xaead_open(key, nonce, ct, sizeof ct, ad, sizeof ad, back) < 0, "xaead data tamper");
    ct[3] ^= 1;
    ad[0] ^= 1;
    CHECK(wg_xaead_open(key, nonce, ct, sizeof ct, ad, sizeof ad, back) < 0, "xaead AD tamper");
    ad[0] ^= 1;
    nonce[20] ^= 1;
    CHECK(wg_xaead_open(key, nonce, ct, sizeof ct, ad, sizeof ad, back) < 0, "xaead nonce tamper (second half)");
    nonce[20] ^= 1;
    nonce[2] ^= 1;
    CHECK(wg_xaead_open(key, nonce, ct, sizeof ct, ad, sizeof ad, back) < 0, "xaead nonce tamper (HChaCha half)");
    nonce[2] ^= 1;
    CHECK(wg_aead_seal(key, 7, pt, sizeof pt, NULL, 0, ct) == 0, "aead seal");
    CHECK(wg_aead_open(key, 8, ct, sizeof ct, NULL, 0, back) < 0, "aead wrong counter");
    CHECK(wg_aead_open(key, 7, ct, sizeof ct, NULL, 0, back) == 0 && memcmp(back, pt, sizeof pt) == 0, "aead round trip");
    CHECK(wg_aead_open(key, 7, ct, 15, NULL, 0, back) < 0, "aead shorter than a tag");
}

static void t_x25519(void)
{
    uint8_t a[32], b[32], ap[32], bp[32], s1[32], s2[32], z[32] = { 0 }, one[32] = { 1 };
    unhex(a, "77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a");
    unhex(b, "5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb");
    CHECK(wg_x25519_pub(ap, a) == 0 && eqhex(ap, "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a"), "X25519 alice pub");
    CHECK(wg_x25519_pub(bp, b) == 0 && eqhex(bp, "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f"), "X25519 bob pub");
    CHECK(wg_x25519(s1, a, bp) == 0 && wg_x25519(s2, b, ap) == 0 && memcmp(s1, s2, 32) == 0
            && eqhex(s1, "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742"),
        "X25519 RFC 7748 shared secret");
    CHECK(wg_x25519(s1, a, z) < 0, "X25519 u=0 refused");
    CHECK(wg_x25519(s1, a, one) < 0, "X25519 u=1 refused");
}

static void t_constants(void)
{
    uint8_t ck[32], h[32];
    static const char c[] = "Noise_IKpsk2_25519_ChaChaPoly_BLAKE2s", id[] = "WireGuard v1 zx2c4 Jason@zx2c4.com";
    wg_blake2s(ck, 32, NULL, 0, (const uint8_t*)c, sizeof c - 1);
    wg_hash2(h, ck, 32, (const uint8_t*)id, sizeof id - 1);
    CHECK(eqhex(ck, "60e26daef327efc02ec335e2a025d2d016eb4206f87277f52d38d1988b78cd36"), "initial chaining key");
    CHECK(eqhex(h, "2211b361081ac566691243db458ad5322d9c6c662293e8b70ee19c65ba079ef3"), "initial hash");
}

typedef struct {
    wg_static_t s;
    wg_peer_t peers[4];
    int npeers;
} side_t;

static wg_peer_t* by_pub(void* ud, const uint8_t pub[32])
{
    side_t* x = (side_t*)ud;
    int i;
    for (i = 0; i < x->npeers; i++)
        if (memcmp(x->peers[i].pub, pub, 32) == 0)
            return &x->peers[i];
    return NULL;
}
static wg_peer_t* by_index(void* ud, uint32_t idx)
{
    side_t* x = (side_t*)ud;
    int i;
    for (i = 0; i < x->npeers; i++)
        if (x->peers[i].hs.state != WG_HS_NONE && x->peers[i].hs.local_index == idx)
            return &x->peers[i];
    return NULL;
}

static side_t I, R;
static uint8_t psk[32];

static void setup(int with_psk)
{
    uint8_t pi[32], pr[32], pub[32];
    wg_keygen(pi, pub);
    wg_keygen(pr, pub);
    wg_static_free(&I.s);
    wg_static_free(&R.s);
    wg_static_init(&I.s, pi);
    wg_static_init(&R.s, pr);
    fill(psk, 32);
    wg_peer_init(&I.peers[0], &I.s, R.s.pub, with_psk ? psk : NULL);
    wg_peer_init(&R.peers[0], &R.s, I.s.pub, with_psk ? psk : NULL);
    I.npeers = R.npeers = 1;
}

static uint64_t ns = 1700000000000000000ULL, ms = 1000000;

static int handshake(uint8_t is[32], uint8_t ir[32], uint8_t rs_[32], uint8_t rr[32])
{
    uint8_t init[WG_INIT_LEN], resp[WG_RESP_LEN];
    wg_peer_t *p, *q;
    ns += 100000000;
    ms += 100;
    if (wg_create_initiation(&I.s, &I.peers[0], 0x11111111, ns, ms, init) < 0)
        return -1;
    if (wg_check_macs(&R.s, NULL, ms, init, sizeof init, NULL, 0, 0) != WG_MAC_OK)
        return -2;
    p = wg_consume_initiation(&R.s, init, sizeof init, ms, by_pub, &R);
    if (!p)
        return -3;
    if (wg_create_response(&R.s, p, 0x22222222, ms, resp) < 0)
        return -4;
    if (wg_check_macs(&I.s, NULL, ms, resp, sizeof resp, NULL, 0, 0) != WG_MAC_OK)
        return -5;
    q = wg_consume_response(&I.s, resp, sizeof resp, by_index, &I);
    if (!q)
        return -6;
    if (wg_derive_keys(q, is, ir) < 0 || wg_derive_keys(p, rs_, rr) < 0)
        return -7;
    return 0;
}

static void t_handshake(void)
{
    uint8_t is[32], ir[32], rs_[32], rr[32], pt[64], ct[80], back[64], init[WG_INIT_LEN], resp[WG_RESP_LEN];
    int rc;
    setup(1);
    rc = handshake(is, ir, rs_, rr);
    CHECK(rc == 0, "handshake with psk");
    CHECK(memcmp(is, rr, 32) == 0 && memcmp(ir, rs_, 32) == 0 && memcmp(is, ir, 32) != 0, "mirrored transport keys");
    fill(pt, sizeof pt);
    CHECK(wg_aead_seal(is, 5, pt, sizeof pt, NULL, 0, ct) == 0 && wg_aead_open(rr, 5, ct, sizeof ct, NULL, 0, back) == 0 && memcmp(pt, back, 64) == 0, "transport data");
    CHECK(wg_derive_keys(&I.peers[0], is, ir) < 0, "derive twice refused");
    setup(0);
    CHECK(handshake(is, ir, rs_, rr) == 0, "handshake without psk");
    setup(1);
    R.peers[0].psk[0] ^= 1;
    CHECK(handshake(is, ir, rs_, rr) == -6, "psk mismatch fails at the initiator");
    setup(1);
    R.npeers = 0;
    CHECK(handshake(is, ir, rs_, rr) == -3, "unknown initiator");
    setup(1);
    {
        uint8_t other[32], op[32];
        wg_keygen(other, op);
        wg_peer_init(&I.peers[0], &I.s, op, psk);
    }
    CHECK(handshake(is, ir, rs_, rr) == -2, "initiation for another responder fails mac1");
    setup(1);
    CHECK(wg_create_response(&R.s, &R.peers[0], 1, ms, resp) < 0, "response without an initiation");
    wg_create_initiation(&I.s, &I.peers[0], 9, ns += 100000000, ms += 100, init);
    CHECK(wg_consume_response(&I.s, init, sizeof init, by_index, &I) == NULL, "an initiation is not a response");
}

static void t_replay(void)
{
    uint8_t init[WG_INIT_LEN], init2[WG_INIT_LEN], ts[12];
    setup(1);
    wg_create_initiation(&I.s, &I.peers[0], 1, ns, ms, init);
    CHECK(wg_consume_initiation(&R.s, init, sizeof init, ms, by_pub, &R) != NULL, "first initiation");
    CHECK(wg_consume_initiation(&R.s, init, sizeof init, ms + 1000, by_pub, &R) == NULL, "replayed initiation");
    wg_create_initiation(&I.s, &I.peers[0], 2, ns - 1000000000, ms + 2000, init2);
    CHECK(wg_consume_initiation(&R.s, init2, sizeof init2, ms + 2000, by_pub, &R) == NULL, "older timestamp");
    wg_create_initiation(&I.s, &I.peers[0], 3, ns + 1000000000, ms + 3000, init2);
    CHECK(wg_consume_initiation(&R.s, init2, sizeof init2, ms + 3010, by_pub, &R) != NULL, "newer timestamp accepted");
    wg_create_initiation(&I.s, &I.peers[0], 4, ns + 2000000000, ms + 3020, init2);
    CHECK(wg_consume_initiation(&R.s, init2, sizeof init2, ms + 3020, by_pub, &R) == NULL, "second initiation within 20 ms");
    CHECK(wg_consume_initiation(&R.s, init2, sizeof init2, ms + 3031, by_pub, &R) != NULL, "after 20 ms it is accepted");
    wg_tai64n(ts, 5ULL * 1000000000u + 0x00ffffffu);
    CHECK(eqhex(ts, "400000000000000f00000000"), "TAI64N rounds nanoseconds down to 2^24");
    wg_tai64n(ts, 5ULL * 1000000000u + 0x01000001u);
    CHECK(eqhex(ts, "400000000000000f01000000"), "TAI64N keeps 2^24 multiples");
}

static void t_cookie(void)
{
    uint8_t init[WG_INIT_LEN], cr[WG_COOKIE_LEN], src[18], src2[18];
    wg_cookie_gen_t g;
    memset(&g, 0, sizeof g);
    fill(src, sizeof src);
    memcpy(src2, src, sizeof src);
    src2[17] ^= 1;
    setup(1);
    wg_create_initiation(&I.s, &I.peers[0], 0x77, ns, ms, init);
    CHECK(wg_check_macs(&R.s, &g, ms, init, sizeof init, src, sizeof src, 0) == WG_MAC_OK, "mac1 ok");
    CHECK(wg_check_macs(&R.s, &g, ms, init, sizeof init, src, sizeof src, 1) == WG_MAC_NEED_COOKIE, "under load without mac2");
    init[100] ^= 1;
    CHECK(wg_check_macs(&R.s, &g, ms, init, sizeof init, src, sizeof src, 0) == WG_MAC_BAD, "bad mac1 dropped");
    init[100] ^= 1;
    CHECK(wg_create_cookie_reply(&R.s, &g, ms, init, sizeof init, src, sizeof src, cr) == 0, "cookie reply");
    CHECK(wg_le32(cr + 4) == 0x77, "cookie reply names the initiator's index");
    CHECK(wg_consume_cookie_reply(cr, sizeof cr, ms, by_index, &I) == &I.peers[0], "cookie consumed");
    CHECK(wg_consume_cookie_reply(cr, sizeof cr, ms, by_index, &I) == NULL, "cookie reply replay refused");
    wg_create_initiation(&I.s, &I.peers[0], 0x78, ns + 1000000000, ms + 1000, init);
    CHECK(wg_check_macs(&R.s, &g, ms + 1000, init, sizeof init, src, sizeof src, 1) == WG_MAC_OK, "mac2 accepted under load");
    CHECK(wg_check_macs(&R.s, &g, ms + 1000, init, sizeof init, src2, sizeof src2, 1) == WG_MAC_NEED_COOKIE, "mac2 bound to the source address");
    wg_create_initiation(&I.s, &I.peers[0], 0x79, ns + 2000000000, ms + WG_COOKIE_VALID_MS + 1, init);
    CHECK(wg_check_macs(&R.s, &g, ms + 1001, init, sizeof init, src, sizeof src, 1) == WG_MAC_NEED_COOKIE, "a cookie older than 115 s is not used");
    wg_create_initiation(&I.s, &I.peers[0], 0x7a, ns + 3000000000, ms, init);
    I.peers[0].has_cookie = 1;
    I.peers[0].cookie_ms = ms;
    CHECK(wg_check_macs(&R.s, &g, ms + WG_COOKIE_SECRET_MS + 1, init, sizeof init, src, sizeof src, 1) == WG_MAC_NEED_COOKIE, "the secret rotates after 120 s");
    wg_create_initiation(&I.s, &I.peers[0], 0x7b, ns + 4000000000, ms, init);
    CHECK(wg_create_cookie_reply(&R.s, &g, ms, init, sizeof init, src, sizeof src, cr) == 0, "cookie reply 2");
    cr[40] ^= 1;
    CHECK(wg_consume_cookie_reply(cr, sizeof cr, ms, by_index, &I) == NULL, "tampered cookie reply");
}

static void t_flips(void)
{
    uint8_t init[WG_INIT_LEN], m[WG_INIT_LEN], resp[WG_RESP_LEN], r2[WG_RESP_LEN], cr[WG_COOKIE_LEN], c2[WG_COOKIE_LEN];
    wg_cookie_gen_t g;
    wg_peer_t* p;
    wg_handshake_t saved;
    int i, b, bad = 0;
    memset(&g, 0, sizeof g);
    setup(1);
    wg_create_initiation(&I.s, &I.peers[0], 5, ns += 100000000, ms += 100, init);
    for (i = 0; i < WG_INIT_LEN - 2 * WG_MACLEN; i++)
        for (b = 0; b < 8; b++) {
            memcpy(m, init, sizeof m);
            m[i] ^= (uint8_t)(1 << b);
            if (wg_check_macs(&R.s, &g, ms, m, sizeof m, NULL, 0, 0) == WG_MAC_OK && wg_consume_initiation(&R.s, m, sizeof m, ms, by_pub, &R) != NULL)
                bad++;
        }
    CHECK(bad == 0, "a bit flip in an initiation's body is refused (index bytes by mac1, the rest by the AEADs)");
    for (i = WG_INIT_LEN - 2 * WG_MACLEN; i < WG_INIT_LEN - WG_MACLEN; i++) {
        memcpy(m, init, sizeof m);
        m[i] ^= 0x10;
        if (wg_check_macs(&R.s, &g, ms, m, sizeof m, NULL, 0, 0) != WG_MAC_BAD)
            bad++;
    }
    CHECK(bad == 0, "a bit flip in mac1 is refused");
    p = wg_consume_initiation(&R.s, init, sizeof init, ms, by_pub, &R);
    CHECK(p != NULL, "unflipped initiation");
    wg_create_response(&R.s, p, 6, ms, resp);
    saved = I.peers[0].hs;
    for (i = 0; i < WG_RESP_LEN - 2 * WG_MACLEN; i++)
        for (b = 0; b < 8; b++) {
            memcpy(r2, resp, sizeof r2);
            r2[i] ^= (uint8_t)(1 << b);
            I.peers[0].hs = saved;
            if (wg_check_macs(&I.s, &g, ms, r2, sizeof r2, NULL, 0, 0) == WG_MAC_OK && wg_consume_response(&I.s, r2, sizeof r2, by_index, &I) != NULL)
                bad++;
        }
    CHECK(bad == 0, "a bit flip in a response is refused");
    I.peers[0].hs = saved;
    CHECK(wg_consume_response(&I.s, resp, sizeof resp, by_index, &I) != NULL, "unflipped response");
    wg_create_initiation(&I.s, &I.peers[0], 7, ns += 100000000, ms += 100, init);
    wg_create_cookie_reply(&R.s, &g, ms, init, sizeof init, (const uint8_t*)"a", 1, cr);
    for (i = 0; i < WG_COOKIE_LEN; i++)
        for (b = 0; b < 8; b++) {
            memcpy(c2, cr, sizeof c2);
            c2[i] ^= (uint8_t)(1 << b);
            if (wg_consume_cookie_reply(c2, sizeof c2, ms, by_index, &I) != NULL)
                bad++;
        }
    CHECK(bad == 0, "a bit flip in a cookie reply is refused");
}

static void t_window(void)
{
    static uint8_t seen[1 << 18];
    wg_replay_t r;
    uint64_t greatest = 0;
    int have = 0, i, mism = 0;
    memset(&r, 0, sizeof r);
    for (i = 0; i < 300000; i++) {
        uint64_t c;
        int want, got;
        if (have && (rnd() & 3))
            c = greatest > 2100 ? greatest - (rnd() % 2100) : rnd() % (greatest + 1);
        else
            c = (have ? greatest : 0) + rnd() % (i % 1000 == 0 ? 5000 : 40);
        if (c >= sizeof seen * 8)
            break;
        want = !(seen[c >> 3] & (1 << (c & 7))) && (!have || c > greatest || greatest - c <= WG_RW_WINDOW);
        got = wg_replay_check(&r, c) == 0;
        if (want != got)
            mism++;
        if (got) {
            seen[c >> 3] |= (uint8_t)(1 << (c & 7));
            if (!have || c > greatest)
                greatest = c;
            have = 1;
        }
    }
    CHECK(mism == 0, "replay window matches the set model");
    memset(&r, 0, sizeof r);
    CHECK(wg_replay_check(&r, 0) == 0 && wg_replay_check(&r, 0) < 0, "counter 0 once");
    CHECK(wg_replay_check(&r, WG_REJECT_AFTER_MESSAGES) < 0, "REJECT_AFTER_MESSAGES");
    CHECK(wg_replay_check(&r, WG_REJECT_AFTER_MESSAGES - 1) == 0, "last usable counter");
}

static void t_fuzz(void)
{
    static uint8_t buf[256];
    wg_cookie_gen_t g;
    int i, hits = 0;
    memset(&g, 0, sizeof g);
    setup(1);
    for (i = 0; i < 20000; i++) {
        size_t n = (size_t)(rnd() % 3 == 0 ? (rnd() % 3 == 0 ? WG_INIT_LEN : rnd() % 3 ? WG_RESP_LEN : WG_COOKIE_LEN) : rnd() % sizeof buf);
        fill(buf, n);
        if (rnd() & 1)
            buf[0] = (uint8_t)(1 + rnd() % 4), buf[1] = buf[2] = buf[3] = 0;
        if (n >= 12 && (rnd() & 1))
            wg_put_le32(buf + 8, I.peers[0].hs.local_index);
        hits += wg_consume_initiation(&R.s, buf, n, ms, by_pub, &R) != NULL;
        hits += wg_consume_response(&I.s, buf, n, by_index, &I) != NULL;
        hits += wg_consume_cookie_reply(buf, n, ms, by_index, &I) != NULL;
        hits += wg_check_macs(&R.s, &g, ms, buf, n, buf, n < 18 ? n : 18, (int)(rnd() & 1)) == WG_MAC_OK;
    }
    CHECK(hits == 0, "random messages are refused");
}

static uint8_t* slurp(const char* path, size_t* n)
{
    FILE* f = fopen(path, "rb");
    uint8_t* b = NULL;
    long len;
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) == 0 && (len = ftell(f)) > 0 && fseek(f, 0, SEEK_SET) == 0 && (b = malloc((size_t)len + 1)) != NULL) {
        *n = fread(b, 1, (size_t)len, f);
        b[*n] = 0;
    }
    fclose(f);
    return b;
}

static int pinned(const uint8_t* b, size_t n, const char* want)
{
    uint8_t d[32];
    unsigned dl = 32;
    EVP_Digest(b, n, d, &dl, EVP_sha256(), NULL);
    return eqhex(d, want);
}

static size_t field(const char* blk, const char* end, const char* key, const char** from, uint8_t* out, size_t max)
{
    char pat[64];
    const char *p, *q;
    size_t i = 0;
    snprintf(pat, sizeof pat, "\"%s\": ", key);
    p = strstr(*from, pat);
    if (!p || p >= end)
        return (size_t)-1;
    p += strlen(pat);
    if (*p == '[')
        p++;
    if (*p != '"')
        return (size_t)-1;
    p++;
    for (q = p; q[0] != '"' && q[0] && q[1] && i < max; q += 2) {
        unsigned v;
        if (sscanf(q, "%2x", &v) != 1)
            return (size_t)-1;
        out[i++] = (uint8_t)v;
    }
    *from = q;
    (void)blk;
    return *q == '"' ? i : (size_t)-1;
}

static void t_noise_vectors(void)
{
    static const char cac_sha[] = "3bde7c09a6f349ee11c825c50fcc02649f8f02a47c857a459206b357f9386cae";
    static const char snow_sha[] = "69da433305fd045f6c9f01b656662a389d022688986fd39fbe7af009cd402fd3";
    static const char name[] = "\"protocol_name\": \"Noise_IKpsk2_25519_ChaChaPoly_BLAKE2s\"";
    size_t n = 0, sn = 0, pro_n, len[8], cl[8], k;
    uint8_t* b = slurp("tests/corpus/noise/cacophony.txt", &n);
    uint8_t* sb = slurp("tests/corpus/noise/snow.txt", &sn);
    uint8_t pro[64], ist[32], ie[32], irs[32], rst[32], re[32], psk[32], ipsk[32], hh[32], pl[8][128], ct[8][256], tmp[256], out[256];
    void *iek, *rek;
    uint8_t ie_pub[32], re_pub[32], ss_i[32], ss_r[32], spub[32], k1[32], k2[32], r1[32], r2[32];
    const char *blk, *end, *cur;
    wg_static_t si, sr;
    wg_sym_t a, z;
    int nm = 0, ok;
    uint64_t nonce[2] = { 0, 0 };
    if (sb) {
        CHECK(pinned(sb, sn, snow_sha), "snow.txt sha256 pin");
        CHECK(!strstr((const char*)sb, "Noise_IKpsk2_25519_ChaChaPoly_BLAKE2s"), "snow.txt has no IKpsk2 BLAKE2s vector (pin covers this)");
        free(sb);
    } else
        printf("note: tests/corpus/noise/snow.txt not present (tools/test-data.js fetch)\n");
    if (!b) {
        printf("note: Noise test vectors not run: tests/corpus/noise/cacophony.txt not present (tools/test-data.js fetch)\n");
        return;
    }
    CHECK(pinned(b, n, cac_sha), "cacophony.txt sha256 pin");
    blk = strstr((const char*)b, name);
    CHECK(blk != NULL, "cacophony has the IKpsk2 BLAKE2s vector");
    if (!blk) {
        free(b);
        return;
    }
    CHECK(!strstr(blk + 1, name), "exactly one IKpsk2 BLAKE2s vector");
    end = strstr(blk + 1, "\"protocol_name\"");
    if (!end)
        end = (const char*)b + n;
    cur = blk;
    pro_n = field(blk, end, "init_prologue", &cur, pro, sizeof pro);
    ok = pro_n != (size_t)-1;
    ok &= field(blk, end, "init_psks", &cur, ipsk, 32) == 32;
    ok &= field(blk, end, "init_static", &cur, ist, 32) == 32;
    ok &= field(blk, end, "init_ephemeral", &cur, ie, 32) == 32;
    ok &= field(blk, end, "init_remote_static", &cur, irs, 32) == 32;
    ok &= field(blk, end, "resp_psks", &cur, psk, 32) == 32;
    ok &= field(blk, end, "resp_static", &cur, rst, 32) == 32;
    ok &= field(blk, end, "resp_ephemeral", &cur, re, 32) == 32;
    ok &= field(blk, end, "handshake_hash", &cur, hh, 32) == 32;
    while (ok && nm < 8 && (len[nm] = field(blk, end, "payload", &cur, pl[nm], sizeof pl[nm])) != (size_t)-1) {
        cl[nm] = field(blk, end, "ciphertext", &cur, ct[nm], sizeof ct[nm]);
        ok &= cl[nm] != (size_t)-1;
        nm++;
    }
    CHECK(ok && nm == 6, "vector fields parsed (2 handshake + 4 transport messages)");
    if (!ok || nm != 6) {
        free(b);
        return;
    }
    memset(&si, 0, sizeof si);
    memset(&sr, 0, sizeof sr);
    wg_static_init(&si, ist);
    wg_static_init(&sr, rst);
    CHECK(memcmp(sr.pub, irs, 32) == 0 && memcmp(ipsk, psk, 32) == 0, "vector keys consistent");
    wg_x25519_pub(ie_pub, ie);
    wg_x25519_pub(re_pub, re);
    iek = wg_pkey_new(ie);
    rek = wg_pkey_new(re);
    wg_x25519(ss_i, si.priv, sr.pub);
    wg_sym_start(&a, pro, pro_n, sr.pub);
    CHECK(cl[0] == 32 + 48 + len[0] + 16 && wg_noise_init_write(&a, iek, ie_pub, sr.pub, si.pub, ss_i, pl[0], len[0], out) == 0 && memcmp(out, ct[0], cl[0]) == 0, "message 1 (-> e, es, s, ss) equals the vector");
    wg_sym_start(&z, pro, pro_n, sr.pub);
    CHECK(wg_noise_init_read_s(&z, &sr, ct[0], spub) == 0 && memcmp(spub, si.pub, 32) == 0, "responder decrypts the initiator static key");
    wg_x25519_static(ss_r, &sr, spub);
    CHECK(wg_noise_init_read_pl(&z, ss_r, ct[0] + 80, cl[0] - 80, tmp) == 0 && memcmp(tmp, pl[0], len[0]) == 0, "responder decrypts payload 1");
    CHECK(cl[1] == 32 + len[1] + 16 && wg_noise_resp_write(&z, rek, re_pub, ie_pub, si.pub, psk, pl[1], len[1], out) == 0 && memcmp(out, ct[1], cl[1]) == 0, "message 2 (<- e, ee, se, psk) equals the vector");
    CHECK(wg_noise_resp_read(&a, iek, &si, ipsk, ct[1], cl[1], tmp) == 0 && memcmp(tmp, pl[1], len[1]) == 0, "initiator decrypts payload 2");
    CHECK(memcmp(a.h, hh, 32) == 0 && memcmp(z.h, hh, 32) == 0, "handshake hash on both sides equals the vector");
    wg_noise_split(&a, k1, k2);
    wg_noise_split(&z, r1, r2);
    CHECK(memcmp(k1, r1, 32) == 0 && memcmp(k2, r2, 32) == 0, "split agrees");
    for (k = 2; k < 6; k++) {
        int dir = (int)(k & 1);
        size_t m = len[k];
        CHECK(cl[k] == m + 16 && wg_aead_seal(dir ? k2 : k1, nonce[dir], pl[k], m, NULL, 0, out) == 0 && memcmp(out, ct[k], cl[k]) == 0, "transport message equals the vector (initiator sends with the first split key)");
        CHECK(wg_aead_open(dir ? r2 : r1, nonce[dir], ct[k], cl[k], NULL, 0, tmp) == 0 && memcmp(tmp, pl[k], m) == 0, "transport message opens");
        nonce[dir]++;
    }
    wg_pkey_free(iek);
    wg_pkey_free(rek);
    wg_static_free(&si);
    wg_static_free(&sr);
    free(b);
}

typedef struct {
    uint64_t* k;
    size_t cap, n;
} u64set_t;

static int u64set_has(const u64set_t* s, uint64_t v)
{
    size_t i = (size_t)((v * 0x9e3779b97f4a7c15ULL) >> 20) & (s->cap - 1);
    while (s->k[i] != UINT64_MAX) {
        if (s->k[i] == v)
            return 1;
        i = (i + 1) & (s->cap - 1);
    }
    return 0;
}

static void u64set_add(u64set_t* s, uint64_t v)
{
    size_t i = (size_t)((v * 0x9e3779b97f4a7c15ULL) >> 20) & (s->cap - 1);
    while (s->k[i] != UINT64_MAX)
        i = (i + 1) & (s->cap - 1);
    s->k[i] = v;
    s->n++;
}

static void p_replay_bitset(void)
{
    fk_rng_t r;
    wg_replay_t w;
    u64set_t set;
    uint64_t greatest = 0;
    unsigned i, n, mism = 0, pmism = 0, marked = 0, accepted = 0, far = 0, edge = 0, resets = 0;
    int have = 0;
    if (!fk_only("replay-bitset"))
        return;
    r = fk_stream("replay-bitset");
    n = (unsigned)fk_iters(200000);
    set.cap = 1;
    while (set.cap < (size_t)n * 2 + 16)
        set.cap <<= 1;
    set.k = (uint64_t*)malloc(set.cap * sizeof *set.k);
    memset(set.k, 0xff, set.cap * sizeof *set.k);
    set.n = 0;
    memset(&w, 0, sizeof w);
    for (i = 0; i < n; i++) {
        uint64_t c, k = fk_below(&r, 16);
        int want, got, peek;
        if (!have || k == 0)
            c = fk_below(&r, 40) == 0 ? fk_next(&r) : (have ? greatest : 0) + fk_below(&r, 70);
        else if (k == 1)
            c = greatest + 1 + fk_below(&r, (uint64_t)1 << (1 + fk_below(&r, 40)));
        else if (k == 2)
            c = greatest >= WG_RW_WINDOW ? greatest - WG_RW_WINDOW + fk_below(&r, 3) - 1 : fk_below(&r, greatest + 1);
        else if (k == 3)
            c = WG_REJECT_AFTER_MESSAGES - 2 + fk_below(&r, 4);
        else if (k == 4)
            c = (greatest & ~(uint64_t)63) + fk_below(&r, 3) * 64 - 64 + fk_below(&r, 2) * 63;
        else
            c = greatest > 2200 ? greatest - fk_below(&r, 2200) : fk_below(&r, greatest + 1);
        if (c == UINT64_MAX)
            c--;
        if (i % 25000 == 0 || (greatest > ((uint64_t)1 << 62) && fk_chance(&r, 3))) {
            memset(&w, 0, sizeof w);
            memset(set.k, 0xff, set.cap * sizeof *set.k);
            set.n = 0;
            have = 0;
            greatest = 0;
            resets++;
        }
        want = c < WG_REJECT_AFTER_MESSAGES && !u64set_has(&set, c) && (!have || c > greatest || greatest - c <= WG_RW_WINDOW);
        far += have && c < greatest && greatest - c > WG_RW_WINDOW;
        edge += have && c < greatest && greatest - c == WG_RW_WINDOW;
        peek = wg_replay_peek(&w, c) == 0;
        if (peek != want)
            pmism++;
        if (fk_chance(&r, 10) && wg_replay_peek(&w, c) != wg_replay_peek(&w, c))
            pmism++;
        got = wg_replay_check(&w, c) == 0;
        if (want != got)
            mism++;
        if (got) {
            accepted++;
            if (set.n * 2 >= set.cap) {
                free(set.k);
                set.cap *= 2;
                set.k = (uint64_t*)malloc(set.cap * sizeof *set.k);
                memset(set.k, 0xff, set.cap * sizeof *set.k);
                set.n = 0;
                u64set_add(&set, c);
            } else
                u64set_add(&set, c);
            if (!have || c > greatest)
                greatest = c;
            have = 1;
            if (wg_replay_check(&w, c) == 0)
                marked++;
        }
    }
    free(set.k);
    CHECK(mism == 0, "replay-bitset: check() matches the hash-set model over the full 64-bit space (jumps, window edge, word edges, REJECT_AFTER_MESSAGES)");
    CHECK(pmism == 0, "replay-bitset: peek() predicts check() and marks nothing");
    CHECK(marked == 0, "replay-bitset: an accepted counter is refused the second time");
    CHECK(accepted > n / 8 && far > n / 50 && edge > 10 && resets > 1, "replay-bitset: the generator reached acceptances, beyond-window and exact-edge counters and fresh windows");
}

static void p_b2s_stream(void)
{
    fk_rng_t r;
    static uint8_t in[4096];
    uint8_t a[32], b[32], key[32];
    unsigned i, n, bad = 0, kbad = 0;
    if (!fk_only("b2s-stream"))
        return;
    r = fk_stream("b2s-stream");
    n = (unsigned)fk_iters(1500);
    for (i = 0; i < n; i++) {
        size_t len = (size_t)fk_below(&r, sizeof in + 1), off = 0, kl = 1 + (size_t)fk_below(&r, 32), ol = 1 + (size_t)fk_below(&r, 32);
        wg_b2s_t s;
        if (fk_chance(&r, 30))
            len = 64 * (size_t)fk_below(&r, 6) + fk_below(&r, 3) - 1;
        if (len > sizeof in)
            len = 0;
        fk_bytes(&r, in, len);
        fk_bytes(&r, key, kl);
        wg_b2s_init(&s, 32, NULL, 0);
        while (off < len) {
            size_t c = fk_chance(&r, 20) ? 64 - (off & 63) : (size_t)fk_below(&r, 130);
            if (c > len - off)
                c = len - off;
            wg_b2s_update(&s, in + off, c);
            off += c;
        }
        wg_b2s_final(&s, a);
        ossl_b2s(b, in, len);
        bad += memcmp(a, b, 32) != 0;
        wg_blake2s(a, ol, key, kl, in, len);
        ossl_mac("BLAKE2SMAC", NULL, ol, key, kl, in, len, b);
        kbad += memcmp(a, b, ol) != 0;
    }
    CHECK(bad == 0, "b2s-stream: BLAKE2s over random chunkings (block edges, empty updates) equals OpenSSL one-shot");
    CHECK(kbad == 0, "b2s-stream: keyed BLAKE2s, random key and output lengths, equals OpenSSL BLAKE2SMAC");
}

static int ossl_aead(int enc, const uint8_t key[32], const uint8_t iv[12], const uint8_t* in, size_t n, const uint8_t* ad, size_t adn, uint8_t* out)
{
    EVP_CIPHER_CTX* c = EVP_CIPHER_CTX_new();
    int l = 0, ok;
    size_t body = enc ? n : n - 16;
    ok = EVP_CipherInit_ex(c, EVP_chacha20_poly1305(), NULL, key, iv, enc) == 1;
    if (!enc && ok)
        ok = EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_AEAD_SET_TAG, 16, (void*)(uintptr_t)(in + body)) == 1;
    ok = ok && (adn == 0 || EVP_CipherUpdate(c, NULL, &l, ad, (int)adn) == 1);
    ok = ok && (body == 0 || EVP_CipherUpdate(c, out, &l, in, (int)body) == 1);
    ok = ok && EVP_CipherFinal_ex(c, out + body, &l) == 1;
    if (enc && ok)
        ok = EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_AEAD_GET_TAG, 16, out + n) == 1;
    EVP_CIPHER_CTX_free(c);
    return ok ? 0 : -1;
}

static void p_aead_diff(void)
{
    fk_rng_t r;
    static uint8_t pt[2100], ct[2140], ref[2140], back[2140];
    uint8_t key[32], ad[80], nonce[24], iv[12], sub[32];
    unsigned i, n, bad = 0, tam = 0, xbad = 0;
    if (!fk_only("aead-diff"))
        return;
    r = fk_stream("aead-diff");
    n = (unsigned)fk_iters(3000);
    for (i = 0; i < n; i++) {
        size_t len = (size_t)fk_below(&r, 2049), adn = (size_t)fk_below(&r, 81);
        uint64_t ctr = fk_chance(&r, 10) ? UINT64_MAX - fk_below(&r, 3) : fk_next(&r) >> fk_below(&r, 64);
        fk_bytes(&r, key, 32);
        fk_bytes(&r, pt, len);
        fk_bytes(&r, ad, adn);
        memset(iv, 0, 4);
        wg_put_le64(iv + 4, ctr);
        if (wg_aead_seal(key, ctr, pt, len, ad, adn, ct) != 0 || ossl_aead(1, key, iv, pt, len, ad, adn, ref) != 0 || memcmp(ct, ref, len + 16))
            bad++;
        if (wg_aead_open(key, ctr, ref, len + 16, ad, adn, back) != 0 || memcmp(back, pt, len))
            bad++;
        if (len + adn) {
            size_t at = (size_t)fk_below(&r, len + 16 + adn);
            if (at < len + 16)
                ct[at] ^= (uint8_t)(1u << fk_below(&r, 8));
            else
                ad[at - len - 16] ^= (uint8_t)(1u << fk_below(&r, 8));
            tam += wg_aead_open(key, ctr, ct, len + 16, ad, adn, back) == 0;
        }
        tam += wg_aead_open(key, ctr ^ 1, ref, len + 16, ad, adn, back) == 0;
        tam += wg_aead_open(key, ctr, ref, fk_below(&r, 16), ad, adn, back) == 0;
        fk_bytes(&r, nonce, 24);
        wg_hchacha20(sub, key, nonce);
        memset(iv, 0, 4);
        memcpy(iv + 4, nonce + 16, 8);
        if (wg_xaead_seal(key, nonce, pt, len, ad, adn, ct) != 0 || ossl_aead(1, sub, iv, pt, len, ad, adn, ref) != 0 || memcmp(ct, ref, len + 16))
            xbad++;
        nonce[fk_below(&r, 24)] ^= 1;
        tam += wg_xaead_open(key, nonce, ct, len + 16, ad, adn, back) == 0;
    }
    CHECK(bad == 0, "aead-diff: ChaCha20-Poly1305 with the counter nonce equals OpenSSL both ways (random key, counter, lengths, AD)");
    CHECK(xbad == 0, "aead-diff: XChaCha20-Poly1305 equals HChaCha20 subkey + OpenSSL ChaCha20-Poly1305");
    CHECK(tam == 0, "aead-diff: any tampered byte, counter, nonce or truncation below the tag fails to open");
}

static void garble(fk_rng_t* r, uint8_t* m, size_t* n, size_t cap)
{
    unsigned k = 1 + (unsigned)fk_below(r, 4), j;
    for (j = 0; j < k; j++) {
        unsigned op = (unsigned)fk_below(r, 5);
        if (op == 0 && *n)
            m[fk_below(r, *n)] ^= (uint8_t)(1 + fk_below(r, 255));
        else if (op == 1)
            *n = (size_t)fk_below(r, *n + 1);
        else if (op == 2 && *n < cap)
            m[(*n)++] = (uint8_t)fk_next(r);
        else if (op == 3 && *n >= 4)
            wg_put_le32(m, (uint32_t)(fk_chance(r, 40) ? 1 + fk_below(r, 4) : fk_chance(r, 50) ? (uint32_t)m[0] | (uint32_t)(1 + fk_below(r, 0xffffff)) << 8 : fk_next(r)));
        else if (*n)
            memset(m + fk_below(r, *n), 0, 1 + (size_t)fk_below(r, 8) > *n ? 1 : 1 + (size_t)fk_below(r, 8));
    }
}

static void p_hs_garble(void)
{
    fk_rng_t r;
    uint8_t init[WG_INIT_LEN], resp[WG_RESP_LEN], cr[WG_COOKIE_LEN], m[WG_INIT_LEN + 64];
    wg_cookie_gen_t g;
    wg_handshake_t saved;
    wg_peer_t* p;
    unsigned i, n, bad = 0, same = 0;
    int gate;
    if (!fk_only("hs-garble"))
        return;
    r = fk_stream("hs-garble");
    memset(&g, 0, sizeof g);
    setup(fk_chance(&r, 50));
    wg_create_initiation(&I.s, &I.peers[0], 0x51, ns += 100000000, ms += 100, init);
    p = wg_consume_initiation(&R.s, init, sizeof init, ms, by_pub, &R);
    wg_create_response(&R.s, p, 0x52, ms, resp);
    saved = I.peers[0].hs;
    wg_create_cookie_reply(&R.s, &g, ms, init, sizeof init, (const uint8_t*)"src", 3, cr);
    n = (unsigned)fk_iters(4000);
    for (i = 0; i < n; i++) {
        unsigned kind = (unsigned)fk_below(&r, 3);
        size_t len = kind == 0 ? WG_INIT_LEN : kind == 1 ? WG_RESP_LEN : WG_COOKIE_LEN;
        memcpy(m, kind == 0 ? init : kind == 1 ? resp : cr, len);
        garble(&r, m, &len, sizeof m);
        if (len == (kind == 0 ? WG_INIT_LEN : kind == 1 ? WG_RESP_LEN : WG_COOKIE_LEN) && !memcmp(m, kind == 0 ? init : kind == 1 ? resp : cr, kind == 2 ? len : len - WG_MACLEN)) {
            same++;
            continue;
        }
        I.peers[0].hs = saved;
        gate = fk_chance(&r, 50);
        if (!gate && kind < 2 && len == (kind == 0 ? WG_INIT_LEN : WG_RESP_LEN) && !memcmp(m, kind == 0 ? init : resp, 4) && !memcmp(m + 8, (kind == 0 ? init : resp) + 8, len - 8 - 2 * WG_MACLEN)) {
            same++;
            continue;
        }
        if (kind == 0)
            bad += (!gate || wg_check_macs(&R.s, &g, ms, m, len, NULL, 0, 0) == WG_MAC_OK) && wg_consume_initiation(&R.s, m, len, ms + 1000, by_pub, &R) != NULL;
        else if (kind == 1)
            bad += (!gate || wg_check_macs(&I.s, &g, ms, m, len, NULL, 0, 0) == WG_MAC_OK) && wg_consume_response(&I.s, m, len, by_index, &I) != NULL;
        else
            bad += wg_consume_cookie_reply(m, len, ms, by_index, &I) != NULL;
    }
    I.peers[0].hs = saved;
    CHECK(bad == 0, "hs-garble: (mac2 aside, unchecked when not under load) with or without the mac1 gate (ungated: sender index and macs are not the consumer's to check), multi-byte corruption, truncation, extension and type rewrites of initiation/response/cookie are refused");
    CHECK(wg_consume_response(&I.s, resp, sizeof resp, by_index, &I) != NULL && same < n / 2, "hs-garble: the untouched response still works after the storm");
}

typedef struct {
    uint8_t m[WG_RESP_LEN], ks[32], kr[32];
    uint32_t for_init;
    int used;
} hresp_t;

static void p_hs_order(void)
{
    fk_rng_t r;
    static uint8_t inits[64][WG_INIT_LEN];
    static uint8_t init_tai[64][12];
    static hresp_t resps[64];
    uint8_t gts[12], ts[12], is[32], ir[32];
    unsigned i, n, ninit = 0, nresp = 0, mism = 0, keybad = 0, done = 0, racc = 0, gapref = 0, tsref = 0;
    uint64_t now, last = 0, unix_ns;
    int have_last = 0, pending = -1;
    uint32_t next_idx = 1;
    if (!fk_only("hs-order"))
        return;
    r = fk_stream("hs-order");
    setup(fk_chance(&r, 70));
    memset(gts, 0, sizeof gts);
    now = ms + 1000000;
    unix_ns = ns + 1000000000000ULL;
    n = (unsigned)fk_iters(1500);
    for (i = 0; i < n; i++) {
        unsigned op = (unsigned)fk_below(&r, 10);
        op = op < 2 ? 0 : op < 6 ? 1 : 2;
        now += fk_chance(&r, 50) ? fk_below(&r, 6) : fk_below(&r, 61);
        unix_ns += fk_below(&r, 60) * 1000000ULL;
        if (op == 0 || ninit == 0) {
            unsigned s = ninit % 64;
            uint64_t t = unix_ns - (fk_chance(&r, 10) ? 400000000ULL : 0);
            wg_create_initiation(&I.s, &I.peers[0], next_idx, t, now, inits[s]);
            wg_tai64n(init_tai[s], t);
            pending = (int)next_idx;
            next_idx++;
            ninit++;
        } else if (op == 1) {
            unsigned s = fk_chance(&r, 60) ? (ninit - 1) % 64 : (unsigned)fk_below(&r, ninit < 64 ? ninit : 64);
            int want;
            wg_peer_t* p;
            memcpy(ts, init_tai[s], 12);
            want = 1;
            if (memcmp(ts, gts, 12) <= 0)
                want = 0, tsref++;
            else if (have_last && now - last < WG_INIT_MIN_GAP_MS)
                want = 0, gapref++;
            p = wg_check_macs(&R.s, NULL, now, inits[s], WG_INIT_LEN, NULL, 0, 0) == WG_MAC_OK ? wg_consume_initiation(&R.s, inits[s], WG_INIT_LEN, now, by_pub, &R) : NULL;
            if ((p != NULL) != want)
                mism++;
            if (p) {
                hresp_t* h = &resps[nresp % 64];
                racc++;
                memcpy(gts, ts, 12);
                last = now;
                have_last = 1;
                h->for_init = wg_le32(inits[s] + 4);
                h->used = 0;
                if (wg_create_response(&R.s, p, 0x40000000 + nresp, now, h->m) != 0 || wg_derive_keys(p, h->ks, h->kr) != 0)
                    mism++;
                nresp++;
            }
        } else if (nresp) {
            hresp_t* h = &resps[fk_chance(&r, 60) ? (nresp - 1) % 64 : fk_below(&r, nresp < 64 ? nresp : 64)];
            int want = !h->used && pending >= 0 && h->for_init == (uint32_t)pending;
            wg_peer_t* q = wg_check_macs(&I.s, NULL, now, h->m, WG_RESP_LEN, NULL, 0, 0) == WG_MAC_OK ? wg_consume_response(&I.s, h->m, WG_RESP_LEN, by_index, &I) : NULL;
            if ((q != NULL) != want)
                mism++;
            if (q) {
                h->used = 1;
                done++;
                if (wg_derive_keys(q, is, ir) != 0 || memcmp(is, h->kr, 32) || memcmp(ir, h->ks, 32))
                    keybad++;
                pending = -1;
            }
        }
    }
    CHECK(mism == 0, "hs-order: random create/deliver/replay order of initiations and responses matches the model (TAI64N greater, 20 ms gap, latest pending index, one use)");
    CHECK(keybad == 0, "hs-order: every completed handshake derives mirrored transport keys");
    CHECK(done > n / 40 && racc > n / 20 && gapref > 0 && tsref > 0, "hs-order: completions, acceptances, gap and timestamp refusals were all reached");
}

int main(void)
{
    fk_init("test_wg_noise");
    if (fk_legacy()) {
        t_blake();
        t_hmac_kdf();
        t_chacha();
        t_x25519();
        t_constants();
        t_handshake();
        t_replay();
        t_cookie();
        t_flips();
        t_window();
        t_fuzz();
        t_noise_vectors();
    }
    p_replay_bitset();
    p_b2s_stream();
    p_aead_diff();
    p_hs_garble();
    p_hs_order();
    wg_static_free(&I.s);
    wg_static_free(&R.s);
    printf("test_wg_noise: %d checks, %d failed\n", checks, fails);
    if (fails)
        printf("replay: DYN_FUZZ_SEED=%llu\n", (unsigned long long)fk_seed);
    return fails ? 1 : 0;
}
