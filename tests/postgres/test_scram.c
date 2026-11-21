/*
 * test_scram.c -- src/core/dyn-scram.c, the dyna:postgres SCRAM-SHA-256 client (RFC 5802, RFC 7677).
 * Fixed checks: the RFC 7677 3 vector, a mock-server exchange, nonce/iteration/salt/e=/m=/NUL refusals,
 * step order, entropy nonce.
 * Seeded property tests (tests/lib/dyn-fuzzkit.h): DYN_FUZZ_SEED replays a run, DYN_FUZZ_ITERS (50 = deep)
 * multiplies every loop, DYN_FUZZ_ONLY=case selects. Each tag is proven by its own mutant:
 *  [scram-exchange] random password/salt/iterations/nonce suffix/padding vs an RFC 5802 model, byte-exact
 *                   client-final, a short buffer is E_SHORT and a retry succeeds without leaking
 *                   (__sanitizer_get_current_allocated_bytes), a flipped ServerSignature bit is refused;
 *                   mutants: AuthMessage length off by one, sfirst not freed on retry (the bug fixed here),
 *                   ServerSignature compared on 31 bytes.
 *  [scram-faults]   grammar-built server-first with one fault each (nonce, iterations, salt, missing
 *                   attribute in any order, e=, m=, NUL, empty) vs an explicit verdict table;
 *                   mutants: no-suffix nonce accepted, 10-digit count accepted, m= ignored.
 *  [scram-final]    1-2 byte mutations or truncations of a genuine v= vs a decode-and-compare model;
 *                   mutant (DYN_FUZZ_ONLY=scram-final): ServerSignature compared on 31 bytes.
 * src/fuzz/fuzz_scram.c covers the same contract with libFuzzer (seeds: tests/ctest/run.sh scram-seeds).
 */
#include "dyn-scram.h"
#include "dyn-codec.h"
#include "dyn-hash.h"
#include "../lib/dyn-fuzzkit.h"

#include <sanitizer/allocator_interface.h>

void simd_init(void);

#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c, ...)            \
    do {                         \
        if (!(c)) {              \
            printf("FAIL: ");    \
            printf(__VA_ARGS__); \
            printf("\n");        \
            fails++;             \
        }                        \
    } while (0)

static const char* SHA = "sha256";

static void rfc7677(void)
{
    const char* pw = "pencil";
    const char* salt_b64 = "W22ZaJ0SNY7soEsUEjb6gQ==";
    const char* cfirst_bare = "n=user,r=rOprNGfwEbeRWgbNEkqO";
    const char* sfirst = "r=rOprNGfwEbeRWgbNEkqO%hvYDpWUa2RaTCAfuxFIlj)hNlF$k0,"
                         "s=W22ZaJ0SNY7soEsUEjb6gQ==,i=4096";
    const char* cfinal_np = "c=biws,r=rOprNGfwEbeRWgbNEkqO%hvYDpWUa2RaTCAfuxFIlj)hNlF$k0";
    const char* want_p = "dHzbZapWIk4jUhN+Ute9ytag9zjfMHgsqmmiz7AndVQ=";
    const char* want_v = "6rriTRBi23WpRR/wtup+mMhUZUn/dB5nLTJRsjl95G4=";

    const dyn_hash_algo_t* a = dyn_hash_algo_by_name(SHA);
    uint8_t salt[64], salted[32], ckey[32], stored[32], csig[32];
    uint8_t skey[32], ssig[32], proof[32];
    char authmsg[512], b64[128];
    size_t saltlen, n;
    int i;

    CHECK(a != NULL, "sha256 must be available");
    if (!a)
        return;

    saltlen = dyn_codec_base64_decode(salt_b64, strlen(salt_b64), salt);
    CHECK(saltlen == 16, "the vector's salt is 16 octets, decoded %zu", saltlen);

    snprintf(authmsg, sizeof(authmsg), "%s,%s,%s", cfirst_bare, sfirst, cfinal_np);

    dyn_pbkdf2(a, (const uint8_t*)pw, strlen(pw), salt, saltlen, 4096,
        salted, sizeof(salted));
    dyn_hmac(a, salted, 32, (const uint8_t*)"Client Key", 10, ckey);
    dyn_sha256(ckey, 32, stored);
    dyn_hmac(a, stored, 32, (const uint8_t*)authmsg, strlen(authmsg), csig);
    for (i = 0; i < 32; i++)
        proof[i] = (uint8_t)(ckey[i] ^ csig[i]);
    dyn_hmac(a, salted, 32, (const uint8_t*)"Server Key", 10, skey);
    dyn_hmac(a, skey, 32, (const uint8_t*)authmsg, strlen(authmsg), ssig);

    n = dyn_codec_base64_encode(proof, 32, b64);
    b64[n] = '\0';
    CHECK(strcmp(b64, want_p) == 0, "RFC 7677 ClientProof:\n    got  %s\n    want %s",
        b64, want_p);
    n = dyn_codec_base64_encode(ssig, 32, b64);
    b64[n] = '\0';
    CHECK(strcmp(b64, want_v) == 0, "RFC 7677 ServerSignature:\n    got  %s\n    want %s",
        b64, want_v);
}

static void cbind_modes(void)
{
    static const struct { int mode; const char* cv; const char* p; const char* v; } V[3] = {
        { DYN_SCRAM_CB_NONE, "biws", "dHzbZapWIk4jUhN+Ute9ytag9zjfMHgsqmmiz7AndVQ=", "6rriTRBi23WpRR/wtup+mMhUZUn/dB5nLTJRsjl95G4=" },
        { DYN_SCRAM_CB_CLIENT, "eSws", "FoqiHTtQEDE8lz1CdaEe3tK4mS+iMDTl77SPyDS53DY=", "dI4KpiQJwBr1+V+K6U1dA6l6I4I9DUNXWND4pcpRU3U=" },
        { DYN_SCRAM_CB_TLS_EP, "cD10bHMtc2VydmVyLWVuZC1wb2ludCwsAAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8=", "nY1Wus9a+gM2DrbQ1msXFgyhW6KM5ktOxWiU+/P/EGY=", "RwppMGddhz/J0lFYaRReBjXcQeNUFP5Qc76Lo5Exrig=" },
    };
    static const char* const hdr[3] = { "n,,n=,r=", "y,,n=,r=", "p=tls-server-end-point,,n=,r=" };
    const char* sfirst = "r=rOprNGfwEbeRWgbNEkqO%hvYDpWUa2RaTCAfuxFIlj)hNlF$k0,s=W22ZaJ0SNY7soEsUEjb6gQ==,i=4096";
    uint8_t cb[65];
    char out[512], want[512], fin[128];
    dyn_scram_t s;
    int i, rc;
    for (i = 0; i < 65; i++)
        cb[i] = (uint8_t)i;
    for (i = 0; i < 3; i++) {
        memset(&s, 0, sizeof s);
        rc = dyn_scram_client_first_cb(&s, V[i].mode, V[i].mode == DYN_SCRAM_CB_TLS_EP ? cb : NULL, V[i].mode == DYN_SCRAM_CB_TLS_EP ? 32 : 0, out, sizeof out);
        CHECK(rc > 0 && strncmp(out, hdr[i], strlen(hdr[i])) == 0, "mode %d client-first header: %s", V[i].mode, out);
        free(s.cfirst_bare);
        s.cfirst_bare = strdup("n=user,r=rOprNGfwEbeRWgbNEkqO");
        snprintf(s.nonce, sizeof s.nonce, "rOprNGfwEbeRWgbNEkqO");
        rc = dyn_scram_server_first(&s, sfirst, strlen(sfirst), "pencil", out, sizeof out);
        snprintf(want, sizeof want, "c=%s,r=rOprNGfwEbeRWgbNEkqO%%hvYDpWUa2RaTCAfuxFIlj)hNlF$k0,p=%s", V[i].cv, V[i].p);
        CHECK(rc == (int)strlen(want) && strcmp(out, want) == 0, "mode %d client-final (openssl CLI reference):\n    got  %s\n    want %s", V[i].mode, rc > 0 ? out : "(error)", want);
        snprintf(fin, sizeof fin, "v=%s", V[i].v);
        CHECK(dyn_scram_server_final(&s, fin, strlen(fin)) == DYN_SCRAM_OK, "mode %d server signature verifies", V[i].mode);
        dyn_scram_free(&s);
    }
    memset(&s, 0, sizeof s);
    CHECK(dyn_scram_client_first_cb(&s, DYN_SCRAM_CB_TLS_EP, cb, 0, out, sizeof out) == DYN_SCRAM_E_SYNTAX, "tls-server-end-point with 0 bytes is refused");
    CHECK(dyn_scram_client_first_cb(&s, DYN_SCRAM_CB_TLS_EP, NULL, 32, out, sizeof out) == DYN_SCRAM_E_SYNTAX, "tls-server-end-point with NULL data is refused");
    CHECK(dyn_scram_client_first_cb(&s, DYN_SCRAM_CB_TLS_EP, cb, 65, out, sizeof out) == DYN_SCRAM_E_SYNTAX, "65 bytes of binding data (max+1) is refused");
    rc = dyn_scram_client_first_cb(&s, DYN_SCRAM_CB_TLS_EP, cb, 64, out, sizeof out);
    CHECK(rc > 0, "64 bytes of binding data (SHA-512 max) is accepted");
    dyn_scram_free(&s);
    CHECK(dyn_scram_client_first_cb(&s, DYN_SCRAM_CB_NONE, cb, 1, out, sizeof out) == DYN_SCRAM_E_SYNTAX, "binding data with mode n is refused");
    CHECK(dyn_scram_client_first_cb(&s, 3, NULL, 0, out, sizeof out) == DYN_SCRAM_E_SYNTAX, "unknown mode is refused");
    CHECK(dyn_scram_client_first_cb(&s, -1, NULL, 0, out, sizeof out) == DYN_SCRAM_E_SYNTAX, "negative mode is refused");
    CHECK(dyn_scram_client_first_cb(&s, DYN_SCRAM_CB_TLS_EP, cb, 32, out, 40) == DYN_SCRAM_E_SHORT, "a short output buffer is refused");
    dyn_scram_free(&s);
}

typedef struct {
    const char* password;
    char snonce[64];
    char sfirst[256];
    uint8_t stored[32], skey[32];
} mock_srv_t;

static const char* field(const char* msg, char key, char* out, size_t cap)
{
    const char* p = msg;
    while (p && *p) {
        const char* e = strchr(p, ',');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (n >= 2 && p[0] == key && p[1] == '=') {
            size_t v = n - 2;
            if (v >= cap)
                v = cap - 1;
            memcpy(out, p + 2, v);
            out[v] = '\0';
            return out;
        }
        p = e ? e + 1 : NULL;
    }
    return NULL;
}

static void srv_first(mock_srv_t* s, const char* cfirst, const char* salt_b64,
    unsigned iters)
{
    const dyn_hash_algo_t* a = dyn_hash_algo_by_name(SHA);
    uint8_t salt[64], salted[32], ckey[32];
    size_t saltlen;
    char cnonce[64];

    field(cfirst, 'r', cnonce, sizeof(cnonce));
    snprintf(s->snonce, sizeof(s->snonce), "%s3rfcNHYJY1ZVvWVs7j", cnonce);
    snprintf(s->sfirst, sizeof(s->sfirst), "r=%s,s=%s,i=%u",
        s->snonce, salt_b64, iters);

    saltlen = dyn_codec_base64_decode(salt_b64, strlen(salt_b64), salt);
    dyn_pbkdf2(a, (const uint8_t*)s->password, strlen(s->password),
        salt, saltlen, iters, salted, sizeof(salted));
    dyn_hmac(a, salted, 32, (const uint8_t*)"Client Key", 10, ckey);
    dyn_sha256(ckey, 32, s->stored);
    dyn_hmac(a, salted, 32, (const uint8_t*)"Server Key", 10, s->skey);
}

static int srv_final(mock_srv_t* s, const char* cfirst, const char* cfinal,
    char* out, size_t outcap)
{
    const dyn_hash_algo_t* a = dyn_hash_algo_by_name(SHA);
    char authmsg[1024], pb64[128], b64[128];
    char bare[256], np[256];
    const char* comma;
    uint8_t proof[64], csig[32], ckey[32], stored[32], ssig[32];
    size_t n, plen;
    int i;

    comma = strchr(cfirst, ',');
    comma = comma ? strchr(comma + 1, ',') : NULL;
    if (!comma)
        return -1;
    snprintf(bare, sizeof(bare), "%s", comma + 1);

    {
        const char* pp = strstr(cfinal, ",p=");
        if (!pp)
            return -1;
        snprintf(np, sizeof(np), "%.*s", (int)(pp - cfinal), cfinal);
    }
    if (!field(cfinal, 'p', pb64, sizeof(pb64)))
        return -1;

    snprintf(authmsg, sizeof(authmsg), "%s,%s,%s", bare, s->sfirst, np);
    plen = dyn_codec_base64_decode(pb64, strlen(pb64), proof);
    if (plen != 32)
        return -1;

    dyn_hmac(a, s->stored, 32, (const uint8_t*)authmsg, strlen(authmsg), csig);
    for (i = 0; i < 32; i++)
        ckey[i] = (uint8_t)(proof[i] ^ csig[i]);
    dyn_sha256(ckey, 32, stored);
    if (memcmp(stored, s->stored, 32) != 0)
        return -2;

    dyn_hmac(a, s->skey, 32, (const uint8_t*)authmsg, strlen(authmsg), ssig);
    n = dyn_codec_base64_encode(ssig, 32, b64);
    b64[n] = '\0';
    snprintf(out, outcap, "v=%s", b64);
    return 0;
}

static fk_rng_t fk_g;

static size_t p_b64(const uint8_t* p, size_t n, char* out, int strip)
{
    size_t k = dyn_codec_base64_encode(p, n, out);
    while (strip && k && out[k - 1] == '=')
        k--;
    out[k] = '\0';
    return k;
}

static void p_text(char* o, size_t n)
{
    fk_pick_bytes(&fk_g, (uint8_t*)o, n,
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!#$%&'()*+-./:;<=>?@[]^_`{|}~\"\\");
    o[n] = '\0';
}

static void p_expect(const char* nonce, const char* sfirst, const char* pw,
    const uint8_t* salt, size_t sl, unsigned it, char* want, size_t cap, uint8_t* sig)
{
    const dyn_hash_algo_t* a = dyn_hash_algo_by_name(SHA);
    uint8_t salted[32], ckey[32], stored[32], csig[32], skey[32], proof[32];
    char am[1024], np[256], pb[64];
    const char* r = strstr(sfirst, "r=");
    size_t rl = strcspn(r + 2, ",");
    int i;
    snprintf(np, sizeof np, "c=biws,r=%.*s", (int)rl, r + 2);
    snprintf(am, sizeof am, "n=,r=%s,%s,%s", nonce, sfirst, np);
    dyn_pbkdf2(a, (const uint8_t*)pw, strlen(pw), salt, sl, it, salted, 32);
    dyn_hmac(a, salted, 32, (const uint8_t*)"Client Key", 10, ckey);
    dyn_sha256(ckey, 32, stored);
    dyn_hmac(a, stored, 32, (const uint8_t*)am, strlen(am), csig);
    for (i = 0; i < 32; i++)
        proof[i] = (uint8_t)(ckey[i] ^ csig[i]);
    dyn_hmac(a, salted, 32, (const uint8_t*)"Server Key", 10, skey);
    dyn_hmac(a, skey, 32, (const uint8_t*)am, strlen(am), sig);
    p_b64(proof, 32, pb, 0);
    snprintf(want, cap, "%s,p=%s", np, pb);
}

static void prop_exchange(void)
{
    long it, N = (long)fk_iters(400);
    fk_g = fk_stream("scram-exchange");
    for (it = 0; it < N; it++) {
        dyn_scram_t sc;
        char cf[64], pw[32], sfx[32], sb[128], sf[512], want[512], out[512], fin[128], vb[64];
        uint8_t salt[48], sig[32], bad[32];
        size_t sl = 1 + fk_below(&fk_g, 48), need, cap, before;
        unsigned iters = 1 + fk_below(&fk_g, 4);
        int rc, flip = fk_chance(&fk_g, 40);
        memset(&sc, 0, sizeof sc);
        before = __sanitizer_get_current_allocated_bytes();
        dyn_scram_client_first(&sc, cf, sizeof cf);
        p_text(pw, fk_below(&fk_g, 24));
        p_text(sfx, 1 + fk_below(&fk_g, 20));
        for (rc = 0; sfx[rc]; rc++)
            if (sfx[rc] == ',')
                sfx[rc] = '~';
        fk_bytes(&fk_g, salt, sl);
        p_b64(salt, sl, sb, fk_chance(&fk_g, 50));
        snprintf(sf, sizeof sf, "r=%s%s,s=%s,i=%u%s", sc.nonce, sfx, sb, iters,
            fk_chance(&fk_g, 30) ? ",x=ext-ok" : "");
        p_expect(sc.nonce, sf, pw, salt, sl, iters, want, sizeof want, sig);
        need = strlen(want);
        cap = fk_chance(&fk_g, 50) ? fk_below(&fk_g, (uint32_t)need + 1) : sizeof out;
        rc = dyn_scram_server_first(&sc, sf, strlen(sf), pw, out, cap);
        if (cap <= need) {
            FK_CHECK(rc == DYN_SCRAM_E_SHORT, "cap %zu need %zu must be E_SHORT, got %d", cap, need, rc);
            rc = dyn_scram_server_first(&sc, sf, strlen(sf), pw, out, need + 1);
        }
        FK_CHECK(rc == (int)need && strcmp(out, want) == 0,
            "client-final vs RFC 5802 model for '%s' pw '%s':\n    got  %s\n    want %s", sf, pw, rc > 0 ? out : "(error)", want);
        memcpy(bad, sig, 32);
        if (flip)
            bad[fk_below(&fk_g, 32)] ^= (uint8_t)(1u << fk_below(&fk_g, 8));
        p_b64(bad, 32, vb, fk_chance(&fk_g, 50));
        snprintf(fin, sizeof fin, "%sv=%s", fk_chance(&fk_g, 20) ? "x=1," : "", vb);
        rc = dyn_scram_server_final(&sc, fin, strlen(fin));
        FK_CHECK(rc == (flip ? DYN_SCRAM_E_VERIFY : DYN_SCRAM_OK), "server-final '%s' (flipped %d) got %d", fin, flip, rc);
        dyn_scram_free(&sc);
        FK_CHECK(__sanitizer_get_current_allocated_bytes() == before, "exchange leaked %zu bytes (cap %zu need %zu)",
            __sanitizer_get_current_allocated_bytes() - before, cap, need);
    }
}

static void prop_faults(void)
{
    static const struct {
        const char* v;
        int want;
    } its[] = { { "0", DYN_SCRAM_E_ITERS }, { "1000001", DYN_SCRAM_E_ITERS }, { "999999999", DYN_SCRAM_E_ITERS },
        { "1234567890", DYN_SCRAM_E_ITERS }, { "12a", DYN_SCRAM_E_ITERS }, { "", DYN_SCRAM_E_ITERS },
        { "+5", DYN_SCRAM_E_ITERS }, { " 5", DYN_SCRAM_E_ITERS }, { "0000000003", DYN_SCRAM_E_ITERS },
        { "-1", DYN_SCRAM_E_ITERS }, { "000000003", 1 }, { "2", 1 } },
      salts[] = { { "", DYN_SCRAM_E_SALT }, { "!!!!", DYN_SCRAM_E_SALT }, { "Q", DYN_SCRAM_E_SALT },
          { "QUJDR", DYN_SCRAM_E_SALT }, { "QQ", 1 }, { "QQ=", 1 }, { "QUJD", 1 } };
    long it, N = (long)fk_iters(300);
    fk_g = fk_stream("scram-faults");
    for (it = 0; it < N; it++) {
        dyn_scram_t sc;
        char cf[64], r[64], f[3][64], sf[300], out[512];
        const char* iv = "2";
        const char* sv = "c2FsdA==";
        unsigned fault = fk_below(&fk_g, 10), ord = fk_below(&fk_g, 3), k, drop = fk_below(&fk_g, 3);
        int want = 1, rc;
        size_t n = 0;
        memset(&sc, 0, sizeof sc);
        dyn_scram_client_first(&sc, cf, sizeof cf);
        snprintf(r, sizeof r, "%s%s", sc.nonce, fault == 1 ? "" : "Zq");
        if (fault == 0) {
            r[fk_below(&fk_g, DYN_SCRAM_NONCE_B64)] ^= 1;
            want = DYN_SCRAM_E_NONCE;
        }
        if (fault == 1)
            want = DYN_SCRAM_E_NONCE;
        if (fault == 2) {
            k = fk_below(&fk_g, sizeof its / sizeof *its);
            iv = its[k].v;
            want = its[k].want;
        }
        if (fault == 3) {
            k = fk_below(&fk_g, sizeof salts / sizeof *salts);
            sv = salts[k].v;
            want = salts[k].want;
        }
        snprintf(f[0], sizeof f[0], "r=%s", r);
        snprintf(f[1], sizeof f[1], "s=%s", sv);
        snprintf(f[2], sizeof f[2], "i=%s", iv);
        if (fault == 5)
            n += (size_t)snprintf(sf + n, sizeof sf - n, "e=bad-proof,");
        if (fault == 6)
            n += (size_t)snprintf(sf + n, sizeof sf - n, "m=needed,");
        if (fault == 7)
            n += (size_t)snprintf(sf + n, sizeof sf - n, "m=,");
        for (k = 0; k < 3; k++) {
            if (fault == 4 && (k + ord) % 3 == drop)
                continue;
            n += (size_t)snprintf(sf + n, sizeof sf - n, "%s%s", n && sf[n - 1] != ',' ? "," : "", f[(k + ord) % 3]);
        }
        if (fault == 4)
            want = DYN_SCRAM_E_SYNTAX;
        if (fault == 5)
            want = DYN_SCRAM_E_SERVER;
        if (fault == 6)
            want = DYN_SCRAM_E_EXT;
        if (fault == 8) {
            sf[fk_below(&fk_g, (uint32_t)n)] = '\0';
            want = DYN_SCRAM_E_SYNTAX;
        }
        if (fault == 9) {
            n = 0;
            want = DYN_SCRAM_E_SYNTAX;
        }
        rc = dyn_scram_server_first(&sc, sf, n, "pw", out, sizeof out);
        if (want == 1)
            FK_CHECK(rc > 0, "fault %u '%.*s' must pass, got %s", fault, (int)n, sf, dyn_scram_strerror(rc));
        else
            FK_CHECK(rc == want, "fault %u '%.*s': want %s, got %s", fault, (int)n, sf, dyn_scram_strerror(want),
                dyn_scram_strerror(rc));
        if (want != 1)
            FK_CHECK(dyn_scram_server_final(&sc, "v=AAAA", 6) == DYN_SCRAM_E_STATE,
                "server-final after a refused server-first must be E_STATE");
        dyn_scram_free(&sc);
    }
}

static int p_dec(const char* s, size_t n, uint8_t* out)
{
    char pad[80];
    size_t k = n, got;
    if (n == 0 || n > 64 || n % 4 == 1)
        return -1;
    memcpy(pad, s, n);
    while (k % 4)
        pad[k++] = '=';
    got = dyn_codec_base64_decode(pad, k, out);
    return got == SIZE_MAX ? -1 : (int)got;
}

static void prop_final(void)
{
    long it, N = (long)fk_iters(100);
    fk_g = fk_stream("scram-final");
    for (it = 0; it < N; it++) {
        dyn_scram_t sc;
        char cf[64], sf[128], want[512], out[512], fin[128];
        uint8_t sig[32], got[64];
        int j;
        memset(&sc, 0, sizeof sc);
        dyn_scram_client_first(&sc, cf, sizeof cf);
        snprintf(sf, sizeof sf, "r=%sZq,s=c2FsdA==,i=1", sc.nonce);
        p_expect(sc.nonce, sf, "pw", (const uint8_t*)"salt", 4, 1, want, sizeof want, sig);
        dyn_scram_server_first(&sc, sf, strlen(sf), "pw", out, sizeof out);
        for (j = 0; j < 16; j++) {
            size_t n, vl;
            const char* v;
            int exp, rc;
            n = (size_t)snprintf(fin, sizeof fin, "v=");
            n += p_b64(sig, 32, fin + n, fk_chance(&fk_g, 50));
            fk_mutate(&fk_g, (uint8_t*)fin, n, 1 + fk_below(&fk_g, 2));
            if (fk_chance(&fk_g, 20))
                n -= fk_below(&fk_g, (uint32_t)n);
            v = NULL;
            vl = 0;
            {
                size_t i = 0, st;
                while (i < n && !v) {
                    st = i;
                    while (i < n && fin[i] != ',')
                        i++;
                    if (i - st >= 2 && fin[st] == 'v' && fin[st + 1] == '=') {
                        v = fin + st + 2;
                        vl = i - st - 2;
                    }
                    i++;
                }
            }
            if (n == 0)
                exp = DYN_SCRAM_E_SYNTAX;
            else if (!v)
                exp = DYN_SCRAM_E_SYNTAX;
            else
                exp = p_dec(v, vl, got) == 32 && memcmp(got, sig, 32) == 0 ? DYN_SCRAM_OK : DYN_SCRAM_E_VERIFY;
            if ((n >= 2 && fin[0] == 'e' && fin[1] == '=') || memmem(fin, n, ",e=", 3))
                continue;
            rc = dyn_scram_server_final(&sc, fin, n);
            FK_CHECK(rc == exp, "server-final '%.*s': want %d got %d", (int)n, fin, exp, rc);
            if (rc == DYN_SCRAM_OK)
                break;
        }
        dyn_scram_free(&sc);
    }
}

static void forged_final(void)
{
    static const char v0[] = "v=AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=";
    dyn_scram_t s;
    memset(&s, 0, sizeof s);
    CHECK(dyn_scram_server_final(&s, v0, sizeof v0 - 1) == DYN_SCRAM_E_STATE, "forged final: zeroed state accepted v= of 32 zero bytes");
    s.step = 1;
    CHECK(dyn_scram_server_final(&s, v0, sizeof v0 - 1) == DYN_SCRAM_E_STATE, "forged final: step 1 accepted v= of 32 zero bytes");
}

int main(void)
{
    dyn_scram_t sc = { 0 };
    mock_srv_t srv;
    char cfirst[256], cfinal[512], sfinal[256];
    int rc;

    setvbuf(stdout, NULL, _IOLBF, 0);
    simd_init();
    fk_init("test_scram");

    rfc7677();
    cbind_modes();
    forged_final();

    {
        srv.password = "correct horse";
        rc = dyn_scram_client_first(&sc, cfirst, sizeof(cfirst));
        CHECK(rc > 0, "client-first: %s", dyn_scram_strerror(rc));
        CHECK(strncmp(cfirst, "n,,n=,r=", 8) == 0,
            "client-first must be 'n,,n=,r=<nonce>' (no channel binding, no "
            "username), got '%s'",
            cfirst);
        CHECK(strlen(cfirst) == 8 + DYN_SCRAM_NONCE_B64,
            "nonce must be %d base64 chars, message is '%s'",
            DYN_SCRAM_NONCE_B64, cfirst);

        srv_first(&srv, cfirst, "W22ZaJ0SNY7soEsUEjb6gQ==", 4096);
        rc = dyn_scram_server_first(&sc, srv.sfirst, strlen(srv.sfirst),
            srv.password, cfinal, sizeof(cfinal));
        CHECK(rc > 0, "client-final: %s", dyn_scram_strerror(rc));
        CHECK(strncmp(cfinal, "c=biws,r=", 9) == 0,
            "client-final must carry c=biws, got '%s'", cfinal);
        CHECK(strstr(cfinal, srv.snonce) != NULL,
            "client-final must echo the FULL server nonce");

        rc = srv_final(&srv, cfirst, cfinal, sfinal, sizeof(sfinal));
        CHECK(rc == 0, "THE SERVER MUST ACCEPT OUR PROOF (rc=%d)", rc);
        rc = dyn_scram_server_final(&sc, sfinal, strlen(sfinal));
        CHECK(rc == DYN_SCRAM_OK, "server-final verify: %s",
            dyn_scram_strerror(rc));
        dyn_scram_free(&sc);
    }

    {
        srv.password = "correct horse";
        dyn_scram_client_first(&sc, cfirst, sizeof(cfirst));
        srv_first(&srv, cfirst, "W22ZaJ0SNY7soEsUEjb6gQ==", 4096);
        rc = dyn_scram_server_first(&sc, srv.sfirst, strlen(srv.sfirst),
            "wrong horse", cfinal, sizeof(cfinal));
        CHECK(rc > 0, "the client still produces a message for a bad password");
        rc = srv_final(&srv, cfirst, cfinal, sfinal, sizeof(sfinal));
        CHECK(rc == -2, "the server must REJECT a proof from the wrong "
                        "password, got %d",
            rc);
        dyn_scram_free(&sc);
    }

    {
        char forged[256];
        dyn_scram_client_first(&sc, cfirst, sizeof(cfirst));
        snprintf(forged, sizeof(forged),
            "r=totallyDifferentNonce,s=W22ZaJ0SNY7soEsUEjb6gQ==,i=4096");
        rc = dyn_scram_server_first(&sc, forged, strlen(forged), "pw",
            cfinal, sizeof(cfinal));
        CHECK(rc == DYN_SCRAM_E_NONCE,
            "a server nonce that is not ours + a suffix must be refused, "
            "got %s",
            dyn_scram_strerror(rc));
        snprintf(forged, sizeof(forged), "r=%s,s=W22ZaJ0SNY7soEsUEjb6gQ==,i=4096",
            sc.nonce);
        rc = dyn_scram_server_first(&sc, forged, strlen(forged), "pw",
            cfinal, sizeof(cfinal));
        CHECK(rc == DYN_SCRAM_E_NONCE,
            "a server nonce with no suffix must be refused, got %s",
            dyn_scram_strerror(rc));
        dyn_scram_free(&sc);
    }

    {
        static const struct {
            const char* i;
            int want;
            const char* why;
        } cases[] = {
            { "0", DYN_SCRAM_E_ITERS, "zero iterations" },
            { "999999999", DYN_SCRAM_E_ITERS, "a billion iterations is a DoS" },
            { "1000001", DYN_SCRAM_E_ITERS, "just past the cap" },
            { "abc", DYN_SCRAM_E_ITERS, "a non-numeric count" },
            { "", DYN_SCRAM_E_ITERS, "an empty count" },
            { "4096", 0, "the ordinary count must pass" },
        };
        size_t k;
        for (k = 0; k < sizeof(cases) / sizeof(cases[0]); k++) {
            char m[256];
            dyn_scram_client_first(&sc, cfirst, sizeof(cfirst));
            snprintf(m, sizeof(m), "r=%sSUFFIX,s=W22ZaJ0SNY7soEsUEjb6gQ==,i=%s",
                sc.nonce, cases[k].i);
            rc = dyn_scram_server_first(&sc, m, strlen(m), "pw",
                cfinal, sizeof(cfinal));
            if (cases[k].want == 0)
                CHECK(rc > 0, "%s: got %s", cases[k].why, dyn_scram_strerror(rc));
            else
                CHECK(rc == cases[k].want, "%s: got %s", cases[k].why,
                    dyn_scram_strerror(rc));
            dyn_scram_free(&sc);
        }
    }

    {
        static const char* bad[] = {
            "s=W22ZaJ0SNY7soEsUEjb6gQ==,i=4096",
            "r=%sSUFFIX,i=4096",
            "r=%sSUFFIX,s=W22ZaJ0SNY7soEsUEjb6gQ==",
            "r=%sSUFFIX,s=,i=4096",
            "r=%sSUFFIX,s=!!!!,i=4096",
        };
        size_t k;
        for (k = 0; k < sizeof(bad) / sizeof(bad[0]); k++) {
            char m[256];
            dyn_scram_client_first(&sc, cfirst, sizeof(cfirst));
            snprintf(m, sizeof(m), bad[k], sc.nonce);
            rc = dyn_scram_server_first(&sc, m, strlen(m), "pw",
                cfinal, sizeof(cfinal));
            CHECK(rc < 0, "malformed server-first %zu must be refused, got %d",
                k, rc);
            dyn_scram_free(&sc);
        }
    }

    {
        const char* e = "e=unknown-user";
        char m[256];
        dyn_scram_client_first(&sc, cfirst, sizeof(cfirst));
        rc = dyn_scram_server_first(&sc, e, strlen(e), "pw", cfinal, sizeof(cfinal));
        CHECK(rc == DYN_SCRAM_E_SERVER, "e= must surface as a server error, got %s",
            dyn_scram_strerror(rc));
        CHECK(strcmp(sc.err, "unknown-user") == 0,
            "and must carry the reason, got '%s'", sc.err);
        dyn_scram_free(&sc);

        dyn_scram_client_first(&sc, cfirst, sizeof(cfirst));
        snprintf(m, sizeof(m),
            "m=needthis,r=%sSUFFIX,s=W22ZaJ0SNY7soEsUEjb6gQ==,i=4096",
            sc.nonce);
        rc = dyn_scram_server_first(&sc, m, strlen(m), "pw", cfinal, sizeof(cfinal));
        CHECK(rc == DYN_SCRAM_E_EXT,
            "a MANDATORY extension must fail the exchange, not be ignored, "
            "got %s",
            dyn_scram_strerror(rc));
        dyn_scram_free(&sc);
    }

    {
        srv.password = "pw";
        dyn_scram_client_first(&sc, cfirst, sizeof(cfirst));
        srv_first(&srv, cfirst, "W22ZaJ0SNY7soEsUEjb6gQ==", 4096);
        dyn_scram_server_first(&sc, srv.sfirst, strlen(srv.sfirst), "pw",
            cfinal, sizeof(cfinal));
        srv_final(&srv, cfirst, cfinal, sfinal, sizeof(sfinal));

        {
            char tampered[256];
            size_t at = strlen(sfinal) - 4;
            snprintf(tampered, sizeof(tampered), "%s", sfinal);
            tampered[at] = tampered[at] == 'A' ? 'B' : 'A';
            rc = dyn_scram_server_final(&sc, tampered, strlen(tampered));
            CHECK(rc == DYN_SCRAM_E_VERIFY,
                "a tampered ServerSignature must be refused, got %s",
                dyn_scram_strerror(rc));
        }
        CHECK(dyn_scram_server_final(&sc, "v=QUJD", 6) == DYN_SCRAM_E_VERIFY,
            "a short ServerSignature must be refused");
        CHECK(dyn_scram_server_final(&sc, "x=nope", 6) == DYN_SCRAM_E_SYNTAX,
            "a server-final with no v= must be refused");
        rc = dyn_scram_server_final(&sc, sfinal, strlen(sfinal));
        CHECK(rc == DYN_SCRAM_OK, "the genuine signature must verify, got %s",
            dyn_scram_strerror(rc));
        dyn_scram_free(&sc);
    }

    {
        memset(&sc, 0, sizeof(sc));
        CHECK(dyn_scram_server_first(&sc, "r=x,s=y,i=1", 11, "pw", cfinal,
                  sizeof(cfinal))
                == DYN_SCRAM_E_STATE,
            "server-first before client-first must be refused");
        CHECK(dyn_scram_server_final(&sc, "v=x", 3) == DYN_SCRAM_E_STATE,
            "server-final before client-final must be refused");
        dyn_scram_client_first(&sc, cfirst, sizeof(cfirst));
        CHECK(dyn_scram_server_final(&sc, "v=x", 3) == DYN_SCRAM_E_STATE,
            "server-final with no client-final must be refused");
        dyn_scram_free(&sc);

        CHECK(dyn_scram_client_first(&sc, cfirst, 8) == DYN_SCRAM_E_SHORT,
            "a short client-first buffer must be refused");
    }

    {
        char sf[256];
        size_t pos;
        int n;
        dyn_scram_client_first(&sc, cfirst, sizeof(cfirst));
        n = snprintf(sf, sizeof(sf), "r=%sZ", sc.nonce);
        pos = (size_t)n;
        sf[pos++] = '\0';
        memcpy(sf + pos, ",s=c2FsdA==,i=4096", 19);
        pos += 19;
        CHECK(dyn_scram_server_first(&sc, sf, pos, srv.password, cfinal,
                  sizeof(cfinal))
                == DYN_SCRAM_E_SYNTAX,
            "a server-first with an embedded NUL must be refused, not hashed");
        dyn_scram_free(&sc);
    }

    {
        char n1[64], n2[64];
        dyn_scram_client_first(&sc, cfirst, sizeof(cfirst));
        snprintf(n1, sizeof(n1), "%s", sc.nonce);
        dyn_scram_free(&sc);
        dyn_scram_client_first(&sc, cfirst, sizeof(cfirst));
        snprintf(n2, sizeof(n2), "%s", sc.nonce);
        dyn_scram_free(&sc);
        CHECK(strcmp(n1, n2) != 0,
            "the client nonce must come from entropy, got '%s' twice", n1);
    }

    if (fk_only("scram-exchange"))
        prop_exchange();
    if (fk_only("scram-faults"))
        prop_faults();
    if (fk_only("scram-final"))
        prop_final();
    fk_fails += fails;
    return fk_done("test_scram");
}
