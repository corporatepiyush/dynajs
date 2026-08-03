/* dyn-scram -- see dyn-scram.h. */
#include "dyn-scram.h"

#include "dyn-codec.h"
#include "dyn-hash.h"
#include "dyn-prng.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* base64("n,,") -- the gs2 header for "this client does not do channel
 * binding". Constant because without TLS there is nothing to bind to. */
#define SCRAM_CBIND_B64 "biws"

const char *dyn_scram_strerror(int code)
{
    switch (code) {
    case DYN_SCRAM_OK:       return "ok";
    case DYN_SCRAM_E_SYNTAX: return "malformed SCRAM message";
    case DYN_SCRAM_E_NONCE:  return "server nonce does not extend the client nonce";
    case DYN_SCRAM_E_ITERS:  return "SCRAM iteration count missing or out of range";
    case DYN_SCRAM_E_SALT:   return "SCRAM salt missing or out of range";
    case DYN_SCRAM_E_VERIFY: return "server signature mismatch";
    case DYN_SCRAM_E_SERVER: return "server rejected the SCRAM exchange";
    case DYN_SCRAM_E_SHORT:  return "SCRAM output buffer too small";
    case DYN_SCRAM_E_STATE:  return "SCRAM steps called out of order";
    case DYN_SCRAM_E_EXT:    return "SCRAM mandatory extension not supported";
    default:                 return "unknown error";
    }
}

/* Compare without an early exit. A byte-at-a-time memcmp on a MAC leaks how
 * many bytes matched, which is enough to forge one byte at a time. */
static int ct_equal(const uint8_t *a, const uint8_t *b, size_t n)
{
    uint8_t d = 0;
    size_t i;
    for (i = 0; i < n; i++)
        d |= (uint8_t)(a[i] ^ b[i]);
    return d == 0;
}

/* Find attribute `key` in a comma-separated attr-val list. Returns its value
 * (borrowed) and length, or NULL. */
static const char *attr(const char *msg, size_t len, char key, size_t *vlen)
{
    size_t i = 0;
    while (i < len) {
        size_t start = i;
        while (i < len && msg[i] != ',')
            i++;
        if (i - start >= 2 && msg[start] == key && msg[start + 1] == '=') {
            *vlen = i - start - 2;
            return msg + start + 2;
        }
        if (i < len)
            i++;                    /* step over the comma */
    }
    return NULL;
}

static void scram_free_parts(dyn_scram_t *s)
{
    free(s->cfirst_bare);
    free(s->sfirst);
    s->cfirst_bare = s->sfirst = NULL;
}

void dyn_scram_free(dyn_scram_t *s)
{
    if (!s)
        return;
    scram_free_parts(s);
    /* The signature is not secret, but the struct may sit in a long-lived
     * connection object; leaving key material behind costs nothing to avoid. */
    memset(s->server_sig, 0, sizeof(s->server_sig));
    s->step = 0;
}

int dyn_scram_client_first(dyn_scram_t *s, char *out, size_t outcap)
{
    uint8_t raw[DYN_SCRAM_RAW_NONCE];
    size_t nb, need;

    if (!s || !out)
        return DYN_SCRAM_E_SYNTAX;
    memset(s, 0, sizeof(*s));

    dyn_os_entropy(raw, sizeof(raw));
    nb = dyn_codec_base64_encode(raw, sizeof(raw), s->nonce);
    memset(raw, 0, sizeof(raw));
    if (nb >= sizeof(s->nonce))
        return DYN_SCRAM_E_SHORT;
    s->nonce[nb] = '\0';

    /* "n=,": PostgreSQL ignores the SCRAM username, so an empty one is both
     * correct and one less thing that can disagree with the startup packet. */
    need = strlen("n,,n=,r=") + nb;
    if (need + 1 > outcap)
        return DYN_SCRAM_E_SHORT;
    snprintf(out, outcap, "n,,n=,r=%s", s->nonce);

    {
        size_t cap = strlen("n=,r=") + nb + 1;
        s->cfirst_bare = (char *)malloc(cap);
        if (!s->cfirst_bare)
            return DYN_SCRAM_E_SHORT;
        snprintf(s->cfirst_bare, cap, "n=,r=%s", s->nonce);
    }
    s->step = 1;
    return (int)need;
}

int dyn_scram_server_first(dyn_scram_t *s, const char *msg, size_t len,
                           const char *password, char *out, size_t outcap)
{
    const char *rv, *sv, *iv, *mv;
    size_t rlen, slen, ilen, mlen, nonce_len;
    uint8_t salt[DYN_SCRAM_MAX_SALT];
    size_t saltlen;
    uint32_t iters = 0;
    uint8_t salted[DYN_SCRAM_KEY_LEN], ckey[DYN_SCRAM_KEY_LEN];
    uint8_t stored[DYN_SCRAM_KEY_LEN], csig[DYN_SCRAM_KEY_LEN];
    uint8_t skey[DYN_SCRAM_KEY_LEN], proof[DYN_SCRAM_KEY_LEN];
    char proof_b64[64];
    char *without_proof = NULL, *authmsg = NULL;
    size_t wlen, alen, pb64;
    const dyn_hash_algo_t *sha256;
    int rc = DYN_SCRAM_E_SYNTAX, i;

    if (!s || s->step != 1 || !msg || !password || !out)
        return DYN_SCRAM_E_STATE;
    if (len == 0 || len > DYN_SCRAM_MAX_MSG)
        return DYN_SCRAM_E_SYNTAX;

    /* e= is an error, and it comes before anything else is worth reading. */
    if ((rv = attr(msg, len, 'e', &rlen)) != NULL) {
        size_t n = rlen < sizeof(s->err) - 1 ? rlen : sizeof(s->err) - 1;
        memcpy(s->err, rv, n);
        s->err[n] = '\0';
        return DYN_SCRAM_E_SERVER;
    }
    /* A mandatory extension we do not understand must FAIL the exchange, not
     * be skipped: that is what "mandatory" means (RFC 5802 5.1). */
    if ((mv = attr(msg, len, 'm', &mlen)) != NULL && mlen > 0) {
        (void)mv;
        return DYN_SCRAM_E_EXT;
    }

    rv = attr(msg, len, 'r', &rlen);
    sv = attr(msg, len, 's', &slen);
    iv = attr(msg, len, 'i', &ilen);
    if (!rv || !sv || !iv)
        return DYN_SCRAM_E_SYNTAX;

    /* THE SERVER NONCE MUST EXTEND OURS. Without this the nonce mechanism is
     * inert: a replaying attacker supplies any nonce it likes. */
    nonce_len = strlen(s->nonce);
    if (rlen <= nonce_len || memcmp(rv, s->nonce, nonce_len) != 0)
        return DYN_SCRAM_E_NONCE;

    if (slen == 0 || slen > DYN_SCRAM_MAX_SALT)
        return DYN_SCRAM_E_SALT;
    saltlen = dyn_codec_base64_decode(sv, slen, salt);
    if (saltlen == 0 || saltlen > sizeof(salt))
        return DYN_SCRAM_E_SALT;

    /* The iteration count is work the SERVER chooses for US. Bounded here
     * because RFC 5802 section 9 names an inflated count as a client DoS and
     * libpq does not bound it. */
    if (ilen == 0 || ilen > 9)
        return DYN_SCRAM_E_ITERS;
    for (i = 0; i < (int)ilen; i++) {
        if (iv[i] < '0' || iv[i] > '9')
            return DYN_SCRAM_E_ITERS;
        iters = iters * 10 + (uint32_t)(iv[i] - '0');
    }
    if (iters == 0 || iters > DYN_SCRAM_MAX_ITERS)
        return DYN_SCRAM_E_ITERS;

    sha256 = dyn_hash_algo_by_name("sha256");
    if (!sha256)
        return DYN_SCRAM_E_SYNTAX;

    s->sfirst = (char *)malloc(len + 1);
    if (!s->sfirst)
        return DYN_SCRAM_E_SHORT;
    memcpy(s->sfirst, msg, len);
    s->sfirst[len] = '\0';

    /* client-final-message-without-proof, with the FULL server nonce echoed. */
    wlen = strlen("c=" SCRAM_CBIND_B64 ",r=") + rlen;
    without_proof = (char *)malloc(wlen + 1);
    if (!without_proof) { rc = DYN_SCRAM_E_SHORT; goto done; }
    memcpy(without_proof, "c=" SCRAM_CBIND_B64 ",r=",
           strlen("c=" SCRAM_CBIND_B64 ",r="));
    memcpy(without_proof + strlen("c=" SCRAM_CBIND_B64 ",r="), rv, rlen);
    without_proof[wlen] = '\0';

    /* AuthMessage is built from the bytes as they went over the wire, never
     * from a re-serialisation: one different space and the proof is garbage. */
    alen = strlen(s->cfirst_bare) + 1 + len + 1 + wlen;
    authmsg = (char *)malloc(alen + 1);
    if (!authmsg) { rc = DYN_SCRAM_E_SHORT; goto done; }
    snprintf(authmsg, alen + 1, "%s,%.*s,%s", s->cfirst_bare, (int)len, msg,
             without_proof);

    if (dyn_pbkdf2(sha256, (const uint8_t *)password, strlen(password),
                   salt, saltlen, iters, salted, sizeof(salted)) < 0) {
        rc = DYN_SCRAM_E_ITERS;
        goto done;
    }
    dyn_hmac(sha256, salted, sizeof(salted), (const uint8_t *)"Client Key", 10,
             ckey);
    dyn_sha256(ckey, sizeof(ckey), stored);
    dyn_hmac(sha256, stored, sizeof(stored), (const uint8_t *)authmsg,
             alen, csig);
    for (i = 0; i < DYN_SCRAM_KEY_LEN; i++)
        proof[i] = (uint8_t)(ckey[i] ^ csig[i]);

    dyn_hmac(sha256, salted, sizeof(salted), (const uint8_t *)"Server Key", 10,
             skey);
    dyn_hmac(sha256, skey, sizeof(skey), (const uint8_t *)authmsg,
             alen, s->server_sig);

    pb64 = dyn_codec_base64_encode(proof, sizeof(proof), proof_b64);
    proof_b64[pb64] = '\0';
    if (wlen + strlen(",p=") + pb64 + 1 > outcap) { rc = DYN_SCRAM_E_SHORT; goto done; }
    snprintf(out, outcap, "%s,p=%s", without_proof, proof_b64);
    rc = (int)(wlen + strlen(",p=") + pb64);
    s->step = 2;

done:
    /* Key material out of a struct that outlives this call. */
    memset(salted, 0, sizeof(salted));
    memset(ckey, 0, sizeof(ckey));
    memset(stored, 0, sizeof(stored));
    memset(skey, 0, sizeof(skey));
    memset(proof, 0, sizeof(proof));
    memset(salt, 0, sizeof(salt));
    if (authmsg) { memset(authmsg, 0, alen); free(authmsg); }
    free(without_proof);
    return rc;
}

int dyn_scram_server_final(dyn_scram_t *s, const char *msg, size_t len)
{
    const char *vv;
    size_t vlen, n;
    uint8_t got[64];               /* 64 base64 chars decode to 48, not 32 */

    if (!s || s->step != 2 || !msg)
        return DYN_SCRAM_E_STATE;
    if (len == 0 || len > DYN_SCRAM_MAX_MSG)
        return DYN_SCRAM_E_SYNTAX;

    if ((vv = attr(msg, len, 'e', &vlen)) != NULL) {
        n = vlen < sizeof(s->err) - 1 ? vlen : sizeof(s->err) - 1;
        memcpy(s->err, vv, n);
        s->err[n] = '\0';
        return DYN_SCRAM_E_SERVER;
    }
    vv = attr(msg, len, 'v', &vlen);
    if (!vv)
        return DYN_SCRAM_E_SYNTAX;
    if (vlen > 64)
        return DYN_SCRAM_E_SYNTAX;
    n = dyn_codec_base64_decode(vv, vlen, got);
    if (n != DYN_SCRAM_KEY_LEN)
        return DYN_SCRAM_E_VERIFY;
    if (!ct_equal(got, s->server_sig, DYN_SCRAM_KEY_LEN))
        return DYN_SCRAM_E_VERIFY;
    s->step = 3;
    return DYN_SCRAM_OK;
}
