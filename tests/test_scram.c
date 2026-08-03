/* test_scram.c -- SCRAM-SHA-256, client side.
 *
 * Two oracles, because one is not enough here:
 *
 *  1. THE RFC 7677 VECTOR, computed from the definition. This pins the key
 *     schedule against a published answer, so a plausible-but-wrong derivation
 *     (a swapped HMAC key/message, "ClientKey" instead of "Client Key") cannot
 *     pass. It does not go through dyn_scram at all -- it is the reference.
 *  2. A SERVER, implemented here, that VERIFIES the client's proof the way a
 *     real server does: recover ClientKey from the proof and check that its
 *     hash is StoredKey. That is a different computation from the client's, so
 *     it is a real differential and not the same code twice.
 *
 * Then the adversarial half: every value the server chooses is attacker-
 * controlled, so each one gets a case -- a nonce that does not extend ours, an
 * iteration count that is a denial of service, a forged server signature.
 */
#include "dyn-scram.h"
#include "dyn-codec.h"
#include "dyn-hash.h"

/* dyn-codec dispatches base64 through the shared kernel table, which the
 * ENGINE initialises at startup. A standalone test has no engine, and an
 * indirect call through the uninitialised table SPINS on this target rather
 * than faulting -- 100% CPU, flat RSS, no signal (CLAUDE.md 9). */
void simd_init(void);

#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { \
    printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static const char *SHA = "sha256";

/* ---- 1. the RFC 7677 vector, straight from the definition ---- */
static void rfc7677(void)
{
    const char *pw = "pencil";
    const char *salt_b64 = "W22ZaJ0SNY7soEsUEjb6gQ==";
    const char *cfirst_bare = "n=user,r=rOprNGfwEbeRWgbNEkqO";
    const char *sfirst =
        "r=rOprNGfwEbeRWgbNEkqO%hvYDpWUa2RaTCAfuxFIlj)hNlF$k0,"
        "s=W22ZaJ0SNY7soEsUEjb6gQ==,i=4096";
    const char *cfinal_np =
        "c=biws,r=rOprNGfwEbeRWgbNEkqO%hvYDpWUa2RaTCAfuxFIlj)hNlF$k0";
    const char *want_p = "dHzbZapWIk4jUhN+Ute9ytag9zjfMHgsqmmiz7AndVQ=";
    const char *want_v = "6rriTRBi23WpRR/wtup+mMhUZUn/dB5nLTJRsjl95G4=";

    const dyn_hash_algo_t *a = dyn_hash_algo_by_name(SHA);
    uint8_t salt[64], salted[32], ckey[32], stored[32], csig[32];
    uint8_t skey[32], ssig[32], proof[32];
    char authmsg[512], b64[128];
    size_t saltlen, n;
    int i;

    CHECK(a != NULL, "sha256 must be available");
    if (!a) return;

    saltlen = dyn_codec_base64_decode(salt_b64, strlen(salt_b64), salt);
    CHECK(saltlen == 16, "the vector's salt is 16 octets, decoded %zu", saltlen);

    snprintf(authmsg, sizeof(authmsg), "%s,%s,%s", cfirst_bare, sfirst, cfinal_np);

    dyn_pbkdf2(a, (const uint8_t *)pw, strlen(pw), salt, saltlen, 4096,
               salted, sizeof(salted));
    dyn_hmac(a, salted, 32, (const uint8_t *)"Client Key", 10, ckey);
    dyn_sha256(ckey, 32, stored);
    dyn_hmac(a, stored, 32, (const uint8_t *)authmsg, strlen(authmsg), csig);
    for (i = 0; i < 32; i++)
        proof[i] = (uint8_t)(ckey[i] ^ csig[i]);
    dyn_hmac(a, salted, 32, (const uint8_t *)"Server Key", 10, skey);
    dyn_hmac(a, skey, 32, (const uint8_t *)authmsg, strlen(authmsg), ssig);

    n = dyn_codec_base64_encode(proof, 32, b64); b64[n] = '\0';
    CHECK(strcmp(b64, want_p) == 0, "RFC 7677 ClientProof:\n    got  %s\n    want %s",
          b64, want_p);
    n = dyn_codec_base64_encode(ssig, 32, b64); b64[n] = '\0';
    CHECK(strcmp(b64, want_v) == 0, "RFC 7677 ServerSignature:\n    got  %s\n    want %s",
          b64, want_v);
}

/* ---- 2. a server that actually verifies the proof ---- */

typedef struct {
    const char *password;
    char snonce[64];        /* client nonce + our suffix */
    char sfirst[256];
    uint8_t stored[32], skey[32];
} mock_srv_t;

static const char *field(const char *msg, char key, char *out, size_t cap)
{
    const char *p = msg;
    while (p && *p) {
        const char *e = strchr(p, ',');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (n >= 2 && p[0] == key && p[1] == '=') {
            size_t v = n - 2;
            if (v >= cap) v = cap - 1;
            memcpy(out, p + 2, v);
            out[v] = '\0';
            return out;
        }
        p = e ? e + 1 : NULL;
    }
    return NULL;
}

/* Build server-first from the client-first, with a chosen salt and count. */
static void srv_first(mock_srv_t *s, const char *cfirst, const char *salt_b64,
                      unsigned iters)
{
    const dyn_hash_algo_t *a = dyn_hash_algo_by_name(SHA);
    uint8_t salt[64], salted[32], ckey[32];
    size_t saltlen;
    char cnonce[64];

    field(cfirst, 'r', cnonce, sizeof(cnonce));
    snprintf(s->snonce, sizeof(s->snonce), "%s3rfcNHYJY1ZVvWVs7j", cnonce);
    snprintf(s->sfirst, sizeof(s->sfirst), "r=%s,s=%s,i=%u",
             s->snonce, salt_b64, iters);

    saltlen = dyn_codec_base64_decode(salt_b64, strlen(salt_b64), salt);
    dyn_pbkdf2(a, (const uint8_t *)s->password, strlen(s->password),
               salt, saltlen, iters, salted, sizeof(salted));
    dyn_hmac(a, salted, 32, (const uint8_t *)"Client Key", 10, ckey);
    dyn_sha256(ckey, 32, s->stored);
    dyn_hmac(a, salted, 32, (const uint8_t *)"Server Key", 10, s->skey);
}

/* Verify the client's proof exactly as a server does: recover ClientKey from
 * ClientProof and check H(ClientKey) == StoredKey. Writes server-final. */
static int srv_final(mock_srv_t *s, const char *cfirst, const char *cfinal,
                     char *out, size_t outcap)
{
    const dyn_hash_algo_t *a = dyn_hash_algo_by_name(SHA);
    char authmsg[1024], pb64[128], b64[128];
    char bare[256], np[256];
    const char *comma;
    uint8_t proof[64], csig[32], ckey[32], stored[32], ssig[32];
    size_t n, plen;
    int i;

    /* client-first-bare is everything after the gs2 header "n,," */
    comma = strchr(cfirst, ',');
    comma = comma ? strchr(comma + 1, ',') : NULL;
    if (!comma) return -1;
    snprintf(bare, sizeof(bare), "%s", comma + 1);

    /* client-final-without-proof is everything before ",p=" */
    { const char *pp = strstr(cfinal, ",p=");
      if (!pp) return -1;
      snprintf(np, sizeof(np), "%.*s", (int)(pp - cfinal), cfinal); }
    if (!field(cfinal, 'p', pb64, sizeof(pb64))) return -1;

    snprintf(authmsg, sizeof(authmsg), "%s,%s,%s", bare, s->sfirst, np);
    plen = dyn_codec_base64_decode(pb64, strlen(pb64), proof);
    if (plen != 32) return -1;

    dyn_hmac(a, s->stored, 32, (const uint8_t *)authmsg, strlen(authmsg), csig);
    for (i = 0; i < 32; i++)
        ckey[i] = (uint8_t)(proof[i] ^ csig[i]);
    dyn_sha256(ckey, 32, stored);
    if (memcmp(stored, s->stored, 32) != 0)
        return -2;                     /* the proof is wrong: wrong password */

    dyn_hmac(a, s->skey, 32, (const uint8_t *)authmsg, strlen(authmsg), ssig);
    n = dyn_codec_base64_encode(ssig, 32, b64); b64[n] = '\0';
    snprintf(out, outcap, "v=%s", b64);
    return 0;
}

int main(void)
{
    dyn_scram_t sc;
    mock_srv_t srv;
    char cfirst[256], cfinal[512], sfinal[256];
    int rc;

    setvbuf(stdout, NULL, _IOLBF, 0);
    simd_init();

    rfc7677();

    /* ---- 3. a full exchange the server accepts ---- */
    {
        srv.password = "correct horse";
        rc = dyn_scram_client_first(&sc, cfirst, sizeof(cfirst));
        CHECK(rc > 0, "client-first: %s", dyn_scram_strerror(rc));
        CHECK(strncmp(cfirst, "n,,n=,r=", 8) == 0,
              "client-first must be 'n,,n=,r=<nonce>' (no channel binding, no "
              "username), got '%s'", cfirst);
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

    /* ---- 4. a WRONG password must be rejected BY THE SERVER ----
     * Without this the success above could be an exchange that agrees with
     * itself regardless of the secret. */
    {
        srv.password = "correct horse";
        dyn_scram_client_first(&sc, cfirst, sizeof(cfirst));
        srv_first(&srv, cfirst, "W22ZaJ0SNY7soEsUEjb6gQ==", 4096);
        rc = dyn_scram_server_first(&sc, srv.sfirst, strlen(srv.sfirst),
                                    "wrong horse", cfinal, sizeof(cfinal));
        CHECK(rc > 0, "the client still produces a message for a bad password");
        rc = srv_final(&srv, cfirst, cfinal, sfinal, sizeof(sfinal));
        CHECK(rc == -2, "the server must REJECT a proof from the wrong "
              "password, got %d", rc);
        dyn_scram_free(&sc);
    }

    /* ---- 5. the server nonce must EXTEND ours ---- */
    {
        char forged[256];
        dyn_scram_client_first(&sc, cfirst, sizeof(cfirst));
        snprintf(forged, sizeof(forged),
                 "r=totallyDifferentNonce,s=W22ZaJ0SNY7soEsUEjb6gQ==,i=4096");
        rc = dyn_scram_server_first(&sc, forged, strlen(forged), "pw",
                                    cfinal, sizeof(cfinal));
        CHECK(rc == DYN_SCRAM_E_NONCE,
              "a server nonce that is not ours + a suffix must be refused, "
              "got %s", dyn_scram_strerror(rc));
        /* and a nonce EQUAL to ours is not an extension either */
        snprintf(forged, sizeof(forged), "r=%s,s=W22ZaJ0SNY7soEsUEjb6gQ==,i=4096",
                 sc.nonce);
        rc = dyn_scram_server_first(&sc, forged, strlen(forged), "pw",
                                    cfinal, sizeof(cfinal));
        CHECK(rc == DYN_SCRAM_E_NONCE,
              "a server nonce with no suffix must be refused, got %s",
              dyn_scram_strerror(rc));
        dyn_scram_free(&sc);
    }

    /* ---- 6. the iteration count is attacker-controlled WORK ---- */
    {
        static const struct { const char *i; int want; const char *why; } cases[] = {
            { "0",         DYN_SCRAM_E_ITERS, "zero iterations" },
            { "999999999", DYN_SCRAM_E_ITERS, "a billion iterations is a DoS" },
            { "1000001",   DYN_SCRAM_E_ITERS, "just past the cap" },
            { "abc",       DYN_SCRAM_E_ITERS, "a non-numeric count" },
            { "",          DYN_SCRAM_E_ITERS, "an empty count" },
            { "4096",      0,                 "the ordinary count must pass" },
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

    /* ---- 7. malformed server-first ---- */
    {
        static const char *bad[] = {
            "s=W22ZaJ0SNY7soEsUEjb6gQ==,i=4096",          /* no nonce */
            "r=%sSUFFIX,i=4096",                          /* no salt */
            "r=%sSUFFIX,s=W22ZaJ0SNY7soEsUEjb6gQ==",      /* no count */
            "r=%sSUFFIX,s=,i=4096",                       /* empty salt */
            "r=%sSUFFIX,s=!!!!,i=4096",                   /* salt not base64 */
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

    /* ---- 8. e= and a mandatory extension ---- */
    {
        const char *e = "e=unknown-user";
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
              "got %s", dyn_scram_strerror(rc));
        dyn_scram_free(&sc);
    }

    /* ---- 9. the server signature is the server's authentication ---- */
    {
        srv.password = "pw";
        dyn_scram_client_first(&sc, cfirst, sizeof(cfirst));
        srv_first(&srv, cfirst, "W22ZaJ0SNY7soEsUEjb6gQ==", 4096);
        dyn_scram_server_first(&sc, srv.sfirst, strlen(srv.sfirst), "pw",
                               cfinal, sizeof(cfinal));
        srv_final(&srv, cfirst, cfinal, sfinal, sizeof(sfinal));

        /* flip one base64 character of a VALID signature */
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
        /* a signature of the wrong LENGTH is not an accidental match either */
        CHECK(dyn_scram_server_final(&sc, "v=QUJD", 6) == DYN_SCRAM_E_VERIFY,
              "a short ServerSignature must be refused");
        CHECK(dyn_scram_server_final(&sc, "x=nope", 6) == DYN_SCRAM_E_SYNTAX,
              "a server-final with no v= must be refused");
        /* the untampered one still verifies, so the checks are not blanket */
        rc = dyn_scram_server_final(&sc, sfinal, strlen(sfinal));
        CHECK(rc == DYN_SCRAM_OK, "the genuine signature must verify, got %s",
              dyn_scram_strerror(rc));
        dyn_scram_free(&sc);
    }

    /* ---- 10. steps out of order ---- */
    {
        memset(&sc, 0, sizeof(sc));
        CHECK(dyn_scram_server_first(&sc, "r=x,s=y,i=1", 11, "pw", cfinal,
                                     sizeof(cfinal)) == DYN_SCRAM_E_STATE,
              "server-first before client-first must be refused");
        CHECK(dyn_scram_server_final(&sc, "v=x", 3) == DYN_SCRAM_E_STATE,
              "server-final before client-final must be refused");
        dyn_scram_client_first(&sc, cfirst, sizeof(cfirst));
        CHECK(dyn_scram_server_final(&sc, "v=x", 3) == DYN_SCRAM_E_STATE,
              "server-final with no client-final must be refused");
        dyn_scram_free(&sc);

        /* a tiny output buffer refuses rather than overruns */
        CHECK(dyn_scram_client_first(&sc, cfirst, 8) == DYN_SCRAM_E_SHORT,
              "a short client-first buffer must be refused");
    }

    /* ---- 11. two exchanges must not produce the same nonce ---- */
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

    if (fails == 0) printf("test_scram: all tests passed\n");
    else printf("test_scram: %d FAILED\n", fails);
    return fails != 0;
}
