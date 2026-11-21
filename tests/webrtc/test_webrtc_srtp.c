/*
 * test_webrtc_srtp.c -- SRTP/SRTCP for dyna:webrtc media (src/modules/webrtc/webrtc-srtp.c): AES_CM_128_HMAC_SHA1_80/32
 * (RFC 3711), AEAD_AES_128_GCM and AEAD_AES_256_GCM (RFC 7714), keys from DTLS-SRTP (RFC 5764).
 *
 * Oracles, none derived from the code under test:
 * - RFC 3711 B.3: the AES-CM PRF key derivation (cipher key, cipher salt, the 94-octet auth key) from the published
 *   master key and salt;
 * - RFC 3711 B.2: the AES-CM keystream for session key 2B7E1516..., salt F0F1...FD, SSRC 0, index 0 (the first three
 *   blocks), read back as the ciphertext of an all-zero payload;
 * - RFC 7714 16.1.1/16.1.2 and 16.2.1/16.2.2: the "Gallia est omnis divisa" RTP packet encrypted and tagged with
 *   AEAD_AES_128_GCM and AEAD_AES_256_GCM (session key 00..0f / 00..1f, salt "Quid pro quo"), and decrypted back;
 * - RFC 7714 17.1-17.4: the SRTCP sender report with index 0x5d4 encrypted (128), decrypted (256), tagged only with the
 *   E flag clear (128) and tag-verified (256);
 * - AES-CM + HMAC-SHA1 packets, where no RFC publishes a full packet: an independent oracle in this file computes
 *   E(payload) with a one-shot EVP AES-128-CTR on the RFC 3711 4.1.1 IV and the tag with a one-shot EVP_Q_mac over
 *   header || ciphertext || ROC (RFC 3711 4.2), for ROC 0 and after a sequence wrap (ROC 1), and for SRTCP over
 *   header || ciphertext || E|index (RFC 3711 3.4); keys come from the B.3-checked KDF;
 * - behaviour from RFC 3711 3.3: a replayed or too-old (outside the 1024-packet window) packet is rejected; reordering
 *   inside the window is accepted; the ROC is guessed across a wrap (Appendix A); a packet that fails authentication
 *   leaves no state (it must not create a stream nor move the window), so forged SSRCs cannot exhaust the table;
 * - malformed input: short, wrong version, CSRC count or extension length past the end, missing tag.
 * - RFC 5764 4.2: the exported key block is client_write_SRTP_master_key, server_write_SRTP_master_key, client salt, server
 *   salt; a fixed block checked against contexts built from the slices by hand catches a symmetric role swap that a
 *   client-to-server round trip cannot see;
 * - RFC 3711 9.1 and RFC 7714 8.1: a key and index pair must never protect two packets, so protect refuses an index at or below the
 *   last one it protected for that SSRC; a NACKed packet is resent byte for byte from the protected history, never re-protected.
 * Why a 1024-packet window: RTP retransmissions after NACK arrive several RTTs late at video rates; RFC 3711 3.3.2
 * requires at least 64; browsers accept 1024.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <openssl/evp.h>

#include "modules/webrtc/webrtc-srtp.h"

static int checks, fails;
#define CHECK(c, ...)                                                                                                      \
    do {                                                                                                                   \
        checks++;                                                                                                          \
        if (!(c)) {                                                                                                        \
            fails++;                                                                                                       \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                    \
            printf(__VA_ARGS__);                                                                                           \
            printf("\n");                                                                                                  \
        }                                                                                                                  \
    } while (0)

static size_t hex(const char* s, uint8_t* out)
{
    size_t n = 0;
    int hi = -1;
    for (; *s; s++) {
        int v;
        if (*s >= '0' && *s <= '9')
            v = *s - '0';
        else if (*s >= 'a' && *s <= 'f')
            v = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'F')
            v = *s - 'A' + 10;
        else
            continue;
        if (hi < 0)
            hi = v;
        else {
            out[n++] = (uint8_t)(hi << 4 | v);
            hi = -1;
        }
    }
    return n;
}

static int eqhex(const uint8_t* p, size_t n, const char* h)
{
    uint8_t b[2048];
    size_t m = hex(h, b);
    return m == n && !memcmp(p, b, n);
}

static void test_kdf(void)
{
    uint8_t mk[16], ms[14], out[94];
    hex("E1F97A0D3E018BE0D64FA32C06DE4139", mk);
    hex("0EC675AD498AFEEBB6960B3AABE6", ms);
    CHECK(wrt_srtp_kdf(WRT_SRTP_CM80, mk, ms, 0, out, 16) == 0 && eqhex(out, 16, "C61E7A93744F39EE10734AFE3FF7A087"),
        "B.3: cipher key");
    CHECK(wrt_srtp_kdf(WRT_SRTP_CM80, mk, ms, 2, out, 14) == 0 && eqhex(out, 14, "30CBBC08863D8C85D49DB34A9AE1"), "B.3: cipher salt");
    CHECK(wrt_srtp_kdf(WRT_SRTP_CM80, mk, ms, 1, out, 94) == 0
            && eqhex(out, 94,
                "CEBE321F6FF7716B6FD4AB49AF256A156D38BAA48F0A0ACF3C34E2359E6CDBCEE049646C43D9327AD175578EF7227098"
                "6371C10C9A369AC2F94A8C5FBCDDDC256D6E919A48B610EF17C2041E474035766B68642C59BBFC2F34DB60DBDFB2"),
        "B.3: the 94-octet auth key");
    CHECK(wrt_srtp_kdf(99, mk, ms, 0, out, 16) < 0, "kdf: unknown profile");
}

static void test_keystream(void)
{
    uint8_t k[16], salt[14], auth[20] = { 1 }, pkt[128];
    size_t n = 0;
    wrt_srtp s;
    hex("2B7E151628AED2A6ABF7158809CF4F3C", k);
    hex("F0F1F2F3F4F5F6F7F8F9FAFBFCFD", salt);
    CHECK(wrt_srtp_init_session(&s, WRT_SRTP_CM80, k, salt, auth, k, salt, auth) == 0, "B.2: session");
    memset(pkt, 0, sizeof(pkt));
    pkt[0] = 0x80;
    CHECK(wrt_srtp_protect(&s, pkt, 12 + 48, sizeof(pkt), &n) == 0 && n == 12 + 48 + 10, "B.2: protect (%zu)", n);
    CHECK(eqhex(pkt + 12, 48, "E03EAD0935C95E80E166B16DD92B4EB4D23513162B02D0F72A43A2FE4A5F97AB41E95B3BB0A2E8DD477901E4FCA894C0"),
        "B.2: keystream blocks 0..2 = ciphertext of zeros");
    wrt_srtp_free(&s);
}

static const char* GALLIA = "8040f17b8041f8d35501a0b247616c6c696120657374206f6d6e697320646976697361"
                            "20696e207061727465732074726573";

static void test_gcm_rtp(int prof, const char* key, const char* want)
{
    uint8_t k[32], salt[12], pkt[256];
    size_t kl = hex(key, k), n = hex(GALLIA, pkt), out = 0;
    wrt_srtp tx, rx;
    (void)kl;
    hex("517569642070726f2071756f", salt);
    CHECK(wrt_srtp_init_session(&tx, prof, k, salt, NULL, k, salt, NULL) == 0, "7714 %d: tx session", prof);
    CHECK(wrt_srtp_init_session(&rx, prof, k, salt, NULL, k, salt, NULL) == 0, "7714 %d: rx session", prof);
    CHECK(wrt_srtp_protect(&tx, pkt, n, sizeof(pkt), &out) == 0 && eqhex(pkt, out, want), "7714 16.%d.1: encrypted and tagged packet",
        prof == WRT_SRTP_GCM128 ? 1 : 2);
    n = out;
    CHECK(wrt_srtp_unprotect(&rx, pkt, n, &out) == 0 && eqhex(pkt, out, GALLIA), "7714 16.%d.2: decrypted packet",
        prof == WRT_SRTP_GCM128 ? 1 : 2);
    hex(want, pkt);
    CHECK(wrt_srtp_unprotect(&rx, pkt, n, &out) == WRT_SRTP_ERR_REPLAY, "7714 %d: the same packet again is a replay", prof);
    hex(want, pkt);
    pkt[n - 1] ^= 1;
    pkt[3] ^= 1;
    {
        wrt_srtp rx2;
        wrt_srtp_init_session(&rx2, prof, k, salt, NULL, k, salt, NULL);
        CHECK(wrt_srtp_unprotect(&rx2, pkt, n, &out) == WRT_SRTP_ERR_AUTH, "7714 %d: a flipped tag bit fails", prof);
        CHECK(rx2.nst == 0, "7714 %d: a failed packet creates no stream", prof);
        wrt_srtp_free(&rx2);
    }
    wrt_srtp_free(&tx);
    wrt_srtp_free(&rx);
}

static void test_gcm_rtcp(void)
{
    uint8_t k[32], salt[12], pkt[256];
    const char* rtcp = "81c8000d4d6172734e5450314e54503252545020 0000042a0000e9304c756e61deadbeefdeadbeefdeadbeefdeadbeefdeadbeef";
    size_t n, out = 0;
    wrt_srtp s;
    hex("517569642070726f2071756f", salt);
    hex("000102030405060708090a0b0c0d0e0f", k);
    wrt_srtp_init_session(&s, WRT_SRTP_GCM128, k, salt, NULL, k, salt, NULL);
    wrt_srtp_set_rtcp_index(&s, 0x4d617273, 0x5d4);
    n = hex(rtcp, pkt);
    CHECK(wrt_srtcp_protect(&s, pkt, n, sizeof(pkt), &out) == 0
            && eqhex(pkt, out,
                "81c8000d4d61727363e94885dcdab67ca727d7662f6b7e997ff5c0f76c06f32dc676a5f1730d6fda4ce09b4686303ded0bb9275b"
                "c84aa45896cf4d2fc5abf87245d9eade800005d4"),
        "7714 17.1: SRTCP encrypted and tagged");
    wrt_srtp_free(&s);
    wrt_srtp_init_session(&s, WRT_SRTP_GCM128, k, salt, NULL, k, salt, NULL);
    wrt_srtp_set_rtcp_index(&s, 0x4d617273, 0x5d4);
    n = hex(rtcp, pkt);
    CHECK(wrt_srtcp_protect_tag_only(&s, pkt, n, sizeof(pkt), &out) == 0
            && eqhex(pkt, out,
                "81c8000d4d6172734e5450314e54503252545020 0000042a0000e9304c756e61deadbeefdeadbeefdeadbeefdeadbeefdeadbeef"
                "841dd9683dd78ec92ae58790125f62b3000005d4"),
        "7714 17.3: SRTCP tagged only, E flag clear");
    wrt_srtp_free(&s);
    hex("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f", k);
    wrt_srtp_init_session(&s, WRT_SRTP_GCM256, k, salt, NULL, k, salt, NULL);
    n = hex("81c8000d4d617273d50ae4d1f5ce5d304ba297e47d470c282c3ece5dbffe0a50a2eaa5c1110555be8415f658c61de047"
            "6f1b6fad1d1eb30c4446839f57ff6f6cb26ac3be800005d4",
        pkt);
    CHECK(wrt_srtcp_unprotect(&s, pkt, n, &out) == 0 && eqhex(pkt, out, rtcp), "7714 17.2: SRTCP 256 verified and decrypted");
    n = hex("81c8000d4d6172734e5450314e54503252545020 0000042a0000e9304c756e61deadbeefdeadbeefdeadbeefdeadbeefdeadbeef"
            "91db4afbfeee5a978fab4393ed2615fe000005d4",
        pkt);
    CHECK(wrt_srtcp_unprotect(&s, pkt, n, &out) == WRT_SRTP_ERR_REPLAY, "7714 17.4 after 17.2: index 0x5d4 again is a replay");
    wrt_srtp_free(&s);
    wrt_srtp_init_session(&s, WRT_SRTP_GCM256, k, salt, NULL, k, salt, NULL);
    CHECK(wrt_srtcp_unprotect(&s, pkt, n, &out) == 0 && eqhex(pkt, out, rtcp), "7714 17.4: SRTCP 256 tag verified (E clear)");
    n = hex("81c8000d4d6172734e5450314e54503252545020 0000042a0000e9304c756e61deadbeefdeadbeefdeadbeefdeadbeefdeadbeef"
            "91db4afbfeee5a978fab4393ed2615ff000005d5",
        pkt);
    CHECK(wrt_srtcp_unprotect(&s, pkt, n, &out) == WRT_SRTP_ERR_AUTH, "7714 17.4: a changed tag and index fail");
    wrt_srtp_free(&s);
}

static void oracle_rtp(const uint8_t* key, const uint8_t* salt, const uint8_t* auth, size_t tag, uint32_t roc, uint8_t* pkt,
    size_t hl, size_t n, size_t* out)
{
    uint8_t iv[16], mac[20], *buf;
    uint32_t ssrc = (uint32_t)pkt[8] << 24 | (uint32_t)pkt[9] << 16 | (uint32_t)pkt[10] << 8 | pkt[11];
    uint64_t idx = (uint64_t)roc << 16 | (uint32_t)(pkt[2] << 8 | pkt[3]);
    int i, l = 0;
    size_t ml = 0;
    EVP_CIPHER_CTX* c = EVP_CIPHER_CTX_new();
    memcpy(iv, salt, 14);
    iv[14] = iv[15] = 0;
    for (i = 0; i < 4; i++)
        iv[4 + i] ^= (uint8_t)(ssrc >> (24 - 8 * i));
    for (i = 0; i < 6; i++)
        iv[8 + i] ^= (uint8_t)(idx >> (40 - 8 * i));
    EVP_EncryptInit_ex(c, EVP_aes_128_ctr(), NULL, key, iv);
    EVP_EncryptUpdate(c, pkt + hl, &l, pkt + hl, (int)(n - hl));
    EVP_CIPHER_CTX_free(c);
    buf = (uint8_t*)malloc(n + 4);
    memcpy(buf, pkt, n);
    buf[n] = (uint8_t)(roc >> 24);
    buf[n + 1] = (uint8_t)(roc >> 16);
    buf[n + 2] = (uint8_t)(roc >> 8);
    buf[n + 3] = (uint8_t)roc;
    EVP_Q_mac(NULL, "HMAC", NULL, "SHA1", NULL, auth, 20, buf, n + 4, mac, sizeof(mac), &ml);
    free(buf);
    memcpy(pkt + n, mac, tag);
    *out = n + tag;
}

static void derive(const uint8_t* mk, const uint8_t* ms, int base, uint8_t* k, uint8_t* a, uint8_t* s)
{
    wrt_srtp_kdf(WRT_SRTP_CM80, mk, ms, (uint8_t)base, k, 16);
    wrt_srtp_kdf(WRT_SRTP_CM80, mk, ms, (uint8_t)(base + 1), a, 20);
    wrt_srtp_kdf(WRT_SRTP_CM80, mk, ms, (uint8_t)(base + 2), s, 14);
}

static size_t mkpkt(uint8_t* p, uint16_t seq, uint32_t ssrc, size_t plen, uint8_t fill)
{
    size_t i;
    p[0] = 0x80;
    p[1] = 0x60;
    p[2] = (uint8_t)(seq >> 8);
    p[3] = (uint8_t)seq;
    p[4] = 0;
    p[5] = 0;
    p[6] = (uint8_t)(seq >> 4);
    p[7] = (uint8_t)seq;
    p[8] = (uint8_t)(ssrc >> 24);
    p[9] = (uint8_t)(ssrc >> 16);
    p[10] = (uint8_t)(ssrc >> 8);
    p[11] = (uint8_t)ssrc;
    for (i = 0; i < plen; i++)
        p[12 + i] = (uint8_t)(fill + i * 7 + (i >> 3));
    return 12 + plen;
}

static void test_cm_oracle(int prof)
{
    uint8_t mk[16], ms[14], k[16], a[20], s[14], p1[256], p2[256], plain[256];
    size_t tag = prof == WRT_SRTP_CM32 ? 4 : 10, n, o1 = 0, o2 = 0, o3 = 0;
    wrt_srtp tx, rx;
    int seqs[] = { 65534, 65535, 0, 1 }, i;
    hex("E1F97A0D3E018BE0D64FA32C06DE4139", mk);
    hex("0EC675AD498AFEEBB6960B3AABE6", ms);
    derive(mk, ms, 0, k, a, s);
    CHECK(wrt_srtp_init(&tx, prof, mk, ms) == 0 && wrt_srtp_init(&rx, prof, mk, ms) == 0, "cm %d: sessions from the master key", prof);
    for (i = 0; i < 4; i++) {
        uint32_t roc = seqs[i] < 2 ? 1u : 0u;
        n = mkpkt(p1, (uint16_t)seqs[i], 0xdecafbad, 37 + (size_t)i * 13, (uint8_t)(i * 31));
        memcpy(p2, p1, n);
        memcpy(plain, p1, n);
        CHECK(wrt_srtp_protect(&tx, p1, n, sizeof(p1), &o1) == 0 && o1 == n + tag, "cm %d: protect seq %d", prof, seqs[i]);
        oracle_rtp(k, s, a, tag, roc, p2, 12, n, &o2);
        CHECK(o1 == o2 && !memcmp(p1, p2, o1), "cm %d: seq %d (ROC %u) matches the independent RFC 3711 oracle", prof, seqs[i], roc);
        CHECK(wrt_srtp_unprotect(&rx, p1, o1, &o3) == 0 && o3 == n && !memcmp(p1, plain, n), "cm %d: unprotect seq %d", prof, seqs[i]);
    }
    CHECK(rx.nst == 1 && rx.st[0].roc == 1 && rx.st[0].s_l == 1, "cm %d: the receiver followed the wrap (ROC %u, s_l %u)", prof,
        rx.nst ? rx.st[0].roc : 99, rx.nst ? rx.st[0].s_l : 99);
    wrt_srtp_free(&tx);
    wrt_srtp_free(&rx);
}

static void test_cm_rtcp_oracle(void)
{
    uint8_t mk[16], ms[14], k[16], a[20], s[14], p1[256], p2[256], plain[256], iv[16], mac[20];
    size_t n = 48, o1 = 0, ml = 0, i;
    wrt_srtp tx, rx;
    int l = 0;
    EVP_CIPHER_CTX* c;
    hex("E1F97A0D3E018BE0D64FA32C06DE4139", mk);
    hex("0EC675AD498AFEEBB6960B3AABE6", ms);
    derive(mk, ms, 3, k, a, s);
    wrt_srtp_init(&tx, WRT_SRTP_CM32, mk, ms);
    wrt_srtp_init(&rx, WRT_SRTP_CM32, mk, ms);
    p1[0] = 0x80;
    p1[1] = 200;
    p1[2] = 0;
    p1[3] = 11;
    p1[4] = 0xca;
    p1[5] = 0xfe;
    p1[6] = 0xba;
    p1[7] = 0xbe;
    for (i = 8; i < n; i++)
        p1[i] = (uint8_t)(i * 13 + 5);
    memcpy(p2, p1, n);
    memcpy(plain, p1, n);
    wrt_srtp_set_rtcp_index(&tx, 0xcafebabe, 7);
    CHECK(wrt_srtcp_protect(&tx, p1, n, sizeof(p1), &o1) == 0 && o1 == n + 4 + 10, "srtcp cm: protect (%zu; SHA1_32 keeps an 80-bit SRTCP tag)",
        o1);
    memcpy(iv, s, 14);
    iv[14] = iv[15] = 0;
    iv[4] ^= 0xca;
    iv[5] ^= 0xfe;
    iv[6] ^= 0xba;
    iv[7] ^= 0xbe;
    iv[13] ^= 7;
    c = EVP_CIPHER_CTX_new();
    EVP_EncryptInit_ex(c, EVP_aes_128_ctr(), NULL, k, iv);
    EVP_EncryptUpdate(c, p2 + 8, &l, p2 + 8, (int)(n - 8));
    EVP_CIPHER_CTX_free(c);
    p2[n] = 0x80;
    p2[n + 1] = 0;
    p2[n + 2] = 0;
    p2[n + 3] = 7;
    EVP_Q_mac(NULL, "HMAC", NULL, "SHA1", NULL, a, 20, p2, n + 4, mac, sizeof(mac), &ml);
    memcpy(p2 + n + 4, mac, 10);
    CHECK(!memcmp(p1, p2, o1), "srtcp cm: matches the independent oracle (RFC 3711 3.4: E|index then tag)");
    CHECK(wrt_srtcp_unprotect(&rx, p1, o1, &o1) == 0 && o1 == n && !memcmp(p1, plain, n), "srtcp cm: unprotect");
    wrt_srtp_free(&tx);
    wrt_srtp_free(&rx);
}

static void test_replay_window(void)
{
    uint8_t mk[16] = { 3 }, ms[14] = { 4 }, store[1300][64], p[64];
    size_t len[1300], o = 0;
    wrt_srtp tx, rx;
    int i, ok = 1;
    wrt_srtp_init(&tx, WRT_SRTP_GCM128, mk, ms);
    wrt_srtp_init(&rx, WRT_SRTP_GCM128, mk, ms);
    for (i = 0; i < 1300; i++) {
        size_t n = mkpkt(store[i], (uint16_t)(1000 + i), 42, 20, (uint8_t)i);
        wrt_srtp_protect(&tx, store[i], n, 64, &len[i]);
    }
    memcpy(p, store[5], len[5]);
    CHECK(wrt_srtp_unprotect(&rx, p, len[5], &o) == 0, "replay: first packet");
    memcpy(p, store[4], len[4]);
    CHECK(wrt_srtp_unprotect(&rx, p, len[4], &o) == 0, "replay: an older packet inside the window is accepted (reorder)");
    memcpy(p, store[4], len[4]);
    CHECK(wrt_srtp_unprotect(&rx, p, len[4], &o) == WRT_SRTP_ERR_REPLAY, "replay: its duplicate is rejected");
    for (i = 6; i < 1300; i++) {
        memcpy(p, store[i], len[i]);
        ok &= wrt_srtp_unprotect(&rx, p, len[i], &o) == 0;
    }
    CHECK(ok, "replay: 1294 in-order packets");
    memcpy(p, store[1299 - 1023], len[1299 - 1023]);
    CHECK(wrt_srtp_unprotect(&rx, p, len[1299 - 1023], &o) == WRT_SRTP_ERR_REPLAY, "replay: 1023 behind the top, already seen");
    memcpy(p, store[1299 - 1024], len[1299 - 1024]);
    CHECK(wrt_srtp_unprotect(&rx, p, len[1299 - 1024], &o) == WRT_SRTP_ERR_REPLAY, "replay: 1024 behind the top is outside the window");
    wrt_srtp_free(&rx);
    wrt_srtp_init(&rx, WRT_SRTP_GCM128, mk, ms);
    memcpy(p, store[1200], len[1200]);
    wrt_srtp_unprotect(&rx, p, len[1200], &o);
    memcpy(p, store[1200 - 1023], len[1200 - 1023]);
    CHECK(wrt_srtp_unprotect(&rx, p, len[1200 - 1023], &o) == 0, "replay: a never-seen packet 1023 behind is still accepted (late NACK retransmission)");
    memcpy(p, store[300], len[300]);
    CHECK(wrt_srtp_unprotect(&rx, p, len[300], &o) == 0, "replay: never seen, 900 behind after a jump: inside the window, accepted");
    memcpy(p, store[1200 - 1024], len[1200 - 1024]);
    CHECK(wrt_srtp_unprotect(&rx, p, len[1200 - 1024], &o) == WRT_SRTP_ERR_REPLAY, "replay: never seen but 1024 behind: rejected");
    wrt_srtp_free(&rx);
    wrt_srtp_init(&rx, WRT_SRTP_GCM128, mk, ms);
    ok = 1;
    for (i = 0; i < 1300; i++) {
        if (i == 1250)
            continue;
        memcpy(p, store[i], len[i]);
        ok &= wrt_srtp_unprotect(&rx, p, len[i], &o) == 0;
    }
    memcpy(p, store[1250], len[1250]);
    CHECK(ok && wrt_srtp_unprotect(&rx, p, len[1250], &o) == 0,
        "replay: a packet skipped after more than 1024 in-order packets arrives late: its ring slot (last used by 226) was cleared, so it is accepted");
    wrt_srtp_free(&tx);
    wrt_srtp_free(&rx);
}

static void test_limits_and_malformed(void)
{
    uint8_t mk[16] = { 5 }, ms[14] = { 6 }, p[128], q[128];
    size_t n, o = 0, before;
    wrt_srtp tx, rx;
    uint32_t i;
    int ok = 1;
    wrt_srtp_init(&tx, WRT_SRTP_CM80, mk, ms);
    wrt_srtp_init(&rx, WRT_SRTP_CM80, mk, ms);
    for (i = 0; i < WRT_SRTP_MAX_STREAMS; i++) {
        n = mkpkt(p, 1, 1000 + i, 10, 1);
        ok &= wrt_srtp_protect(&tx, p, n, sizeof(p), &o) == 0;
        ok &= wrt_srtp_unprotect(&rx, p, o, &o) == 0;
    }
    CHECK(ok && rx.nst == WRT_SRTP_MAX_STREAMS, "limits: %u SSRCs", WRT_SRTP_MAX_STREAMS);
    n = mkpkt(p, 1, 5000, 10, 1);
    CHECK(wrt_srtp_protect(&tx, p, n, sizeof(p), &o) == WRT_SRTP_ERR_LIMIT, "limits: one more SSRC to send is refused");
    n = mkpkt(p, 1, 1000, 10, 1);
    CHECK(wrt_srtp_protect(&tx, p, n, n + 9, &o) == WRT_SRTP_ERR_SPACE, "malformed: no room for the tag");
    before = rx.nst;
    n = mkpkt(p, 9, 777777, 10, 1);
    memset(p + n, 0xAB, 10);
    CHECK(wrt_srtp_unprotect(&rx, p, n + 10, &o) == WRT_SRTP_ERR_AUTH && rx.nst == before, "malformed: a forged SSRC fails auth and adds no stream");
    CHECK(wrt_srtp_unprotect(&rx, p, 11, &o) == WRT_SRTP_ERR_PARSE, "malformed: shorter than a header");
    n = mkpkt(p, 2, 1000, 10, 1);
    memcpy(q, p, n);
    q[0] = 0x40;
    CHECK(wrt_srtp_unprotect(&rx, q, n + 10, &o) == WRT_SRTP_ERR_PARSE, "malformed: version 1");
    memcpy(q, p, n);
    q[0] = 0x8F;
    CHECK(wrt_srtp_unprotect(&rx, q, n + 10, &o) == WRT_SRTP_ERR_PARSE, "malformed: 15 CSRCs past the end");
    memcpy(q, p, n);
    q[0] = 0x90;
    q[12] = 0xBE;
    q[13] = 0xDE;
    q[14] = 0xFF;
    q[15] = 0xFF;
    CHECK(wrt_srtp_unprotect(&rx, q, n + 10, &o) == WRT_SRTP_ERR_PARSE, "malformed: extension length past the end");
    memcpy(q, p, n);
    q[0] = 0x90;
    CHECK(wrt_srtp_unprotect(&rx, q, 15, &o) == WRT_SRTP_ERR_PARSE, "malformed: X bit with no extension header");
    CHECK(wrt_srtp_unprotect(&rx, p, 12 + 9, &o) == WRT_SRTP_ERR_PARSE, "malformed: payload shorter than the tag");
    CHECK(wrt_srtcp_unprotect(&rx, p, 8 + 4 + 9, &o) == WRT_SRTP_ERR_PARSE, "malformed: SRTCP shorter than header + index + tag");
    wrt_srtp_free(&tx);
    wrt_srtp_free(&rx);
}

static void test_profiles_roundtrip(void)
{
    int profs[] = { WRT_SRTP_CM80, WRT_SRTP_CM32, WRT_SRTP_GCM128, WRT_SRTP_GCM256 }, i;
    uint8_t mk[32], ms[14], p[256], plain[256];
    size_t n, o = 0, kl, sl;
    for (i = 0; i < 32; i++)
        mk[i] = (uint8_t)(i * 9 + 1);
    for (i = 0; i < 14; i++)
        ms[i] = (uint8_t)(i * 5 + 3);
    for (i = 0; i < 4; i++) {
        wrt_srtp tx, rx;
        CHECK(wrt_srtp_profile_lengths(profs[i], &kl, &sl) == 0 && kl == (profs[i] == WRT_SRTP_GCM256 ? 32u : 16u)
                && sl == (profs[i] >= WRT_SRTP_GCM128 ? 12u : 14u),
            "profile %d: key and salt lengths (RFC 5764 4.1.2, RFC 7714 14.2)", profs[i]);
        wrt_srtp_init(&tx, profs[i], mk, ms);
        wrt_srtp_init(&rx, profs[i], mk, ms);
        n = mkpkt(p, 7, 99, 100, 3);
        memcpy(plain, p, n);
        CHECK(wrt_srtp_protect(&tx, p, n, sizeof(p), &o) == 0 && memcmp(p + 12, plain + 12, n - 12) != 0, "profile %d: protect encrypts",
            profs[i]);
        CHECK(wrt_srtp_unprotect(&rx, p, o, &o) == 0 && o == n && !memcmp(p, plain, n), "profile %d: round trip", profs[i]);
        p[0] = 0x80;
        p[1] = 201;
        p[2] = 0;
        p[3] = 1;
        p[4] = 0;
        p[5] = 0;
        p[6] = 0;
        p[7] = 99;
        memcpy(plain, p, 8);
        CHECK(wrt_srtcp_protect(&tx, p, 8, sizeof(p), &o) == 0 && wrt_srtcp_unprotect(&rx, p, o, &o) == 0 && o == 8 && !memcmp(p, plain, 8),
            "profile %d: SRTCP round trip of a bare RR", profs[i]);
        wrt_srtp_free(&tx);
        wrt_srtp_free(&rx);
    }
}

static void test_send_index_monotonic(void)
{
    int profs[] = { WRT_SRTP_CM80, WRT_SRTP_GCM128 }, i;
    uint8_t mk[16], ms[14], p[256], q[256];
    size_t n, o = 0;
    for (i = 0; i < 16; i++)
        mk[i] = (uint8_t)(i + 40);
    for (i = 0; i < 14; i++)
        ms[i] = (uint8_t)(i + 90);
    for (i = 0; i < 2; i++) {
        wrt_srtp tx;
        wrt_srtp_init(&tx, profs[i], mk, ms);
        n = mkpkt(p, 5, 7, 100, 0x11);
        CHECK(wrt_srtp_protect(&tx, p, n, sizeof(p), &o) == 0, "send index %d: first packet", profs[i]);
        n = mkpkt(p, 5, 7, 100, 0x11);
        memcpy(q, p, n);
        CHECK(wrt_srtp_protect(&tx, p, n, sizeof(p), &o) == WRT_SRTP_ERR_REPLAY && !memcmp(p, q, n),
            "send index %d: protecting the same index again is refused and leaves the buffer (keystream / GCM nonce reuse)", profs[i]);
        n = mkpkt(p, 4, 7, 100, 0x11);
        CHECK(wrt_srtp_protect(&tx, p, n, sizeof(p), &o) == WRT_SRTP_ERR_REPLAY, "send index %d: an older index is refused", profs[i]);
        n = mkpkt(p, 6, 7, 100, 0x11);
        CHECK(wrt_srtp_protect(&tx, p, n, sizeof(p), &o) == 0, "send index %d: the next index is accepted", profs[i]);
        n = mkpkt(p, 6, 8, 100, 0x11);
        CHECK(wrt_srtp_protect(&tx, p, n, sizeof(p), &o) == 0, "send index %d: the index is per SSRC", profs[i]);
        wrt_srtp_free(&tx);
    }
}

static void test_dtls_key_block(void)
{
    int profs[] = { WRT_SRTP_CM80, WRT_SRTP_CM32, WRT_SRTP_GCM128, WRT_SRTP_GCM256 }, i, j;
    uint8_t blk[88], p[256], plain[256];
    size_t kl, sl, n, o = 0;
    for (i = 0; i < 88; i++)
        blk[i] = (uint8_t)(i * 7 + 13);
    for (i = 0; i < 4; i++) {
        wrt_srtp ctx, crx, stx, srx;
        wrt_srtp_profile_lengths(profs[i], &kl, &sl);
        CHECK(wrt_srtp_init_dtls(&ctx, &crx, profs[i], blk, 2 * (kl + sl) - 1, 1) < 0, "key block %d: wrong length refused", profs[i]);
        CHECK(wrt_srtp_init_dtls(&ctx, &crx, profs[i], blk, 2 * (kl + sl), 1) == 0 && wrt_srtp_init_dtls(&stx, &srx, profs[i], blk, 2 * (kl + sl), 0) == 0,
            "key block %d: both roles", profs[i]);
        for (j = 0; j < 4; j++) {
            wrt_srtp* snd = (j & 1) ? &stx : &ctx;
            wrt_srtp* rcv = (j & 1) ? &crx : &srx;
            n = mkpkt(p, (uint16_t)(10 + j), 42, 60, (uint8_t)j);
            memcpy(plain, p, n);
            CHECK(wrt_srtp_protect(snd, p, n, sizeof(p), &o) == 0, "key block %d.%d: protect", profs[i], j);
            if (j == 0) {
                wrt_srtp chk;
                wrt_srtp_init(&chk, profs[i], blk, blk + 2 * kl);
                CHECK(wrt_srtp_unprotect(&chk, p, o, &o) == 0 && !memcmp(p, plain, n),
                    "key block %d: the client sends with client_write_key + client_write_salt (RFC 5764 4.2)", profs[i]);
                wrt_srtp_free(&chk);
                continue;
            }
            if (j == 1) {
                wrt_srtp chk;
                wrt_srtp_init(&chk, profs[i], blk + kl, blk + 2 * kl + sl);
                CHECK(wrt_srtp_unprotect(&chk, p, o, &o) == 0 && !memcmp(p, plain, n),
                    "key block %d: the server sends with server_write_key + server_write_salt", profs[i]);
                wrt_srtp_free(&chk);
                continue;
            }
            CHECK(wrt_srtp_unprotect(rcv, p, o, &o) == 0 && !memcmp(p, plain, n), "key block %d.%d: the peer role receives", profs[i], j);
        }
        wrt_srtp_free(&ctx);
        wrt_srtp_free(&crx);
        wrt_srtp_free(&stx);
        wrt_srtp_free(&srx);
    }
}

int main(void)
{
    test_kdf();
    test_keystream();
    test_gcm_rtp(WRT_SRTP_GCM128, "000102030405060708090a0b0c0d0e0f",
        "8040f17b8041f8d35501a0b2f24de3a3fb34de6cacba861c9d7e4bcabe633bd50d294e6f42a5f47a51c7d19b36de3adf8833899d7f27beb1"
        "6a9152cf765ee4390cce");
    test_gcm_rtp(WRT_SRTP_GCM256, "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f",
        "8040f17b8041f8d35501a0b232b1de78a822fe12ef9f78fa332e33aab18012389a58e2f3b50b2a0276ffae0f1ba63799b87b7aa3db36dfff"
        "d6b0f9bb7878d7a76c13");
    test_gcm_rtcp();
    test_cm_oracle(WRT_SRTP_CM80);
    test_cm_oracle(WRT_SRTP_CM32);
    test_cm_rtcp_oracle();
    test_replay_window();
    test_limits_and_malformed();
    test_profiles_roundtrip();
    test_send_index_monotonic();
    test_dtls_key_block();
    printf("test_webrtc_srtp: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
