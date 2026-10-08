#include "dyn-scram.h"

#include "dyn-codec.h"
#include "dyn-hash.h"
#include "dyn-prng.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCRAM_CBIND_B64 "biws"

const char* dyn_scram_strerror(int code)
{
    switch (code) {
    case DYN_SCRAM_OK:
        return "ok";
    case DYN_SCRAM_E_SYNTAX:
        return "malformed SCRAM message";
    case DYN_SCRAM_E_NONCE:
        return "server nonce does not extend the client nonce";
    case DYN_SCRAM_E_ITERS:
        return "SCRAM iteration count missing or out of range";
    case DYN_SCRAM_E_SALT:
        return "SCRAM salt missing or out of range";
    case DYN_SCRAM_E_VERIFY:
        return "server signature mismatch";
    case DYN_SCRAM_E_SERVER:
        return "server rejected the SCRAM exchange";
    case DYN_SCRAM_E_SHORT:
        return "SCRAM output buffer too small";
    case DYN_SCRAM_E_STATE:
        return "SCRAM steps called out of order";
    case DYN_SCRAM_E_EXT:
        return "SCRAM mandatory extension not supported";
    case DYN_SCRAM_E_ENTROPY:
        return "OS entropy unavailable";
    default:
        return "unknown error";
    }
}

static const char* attr(const char* msg, size_t len, char key, size_t* vlen)
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
            i++;
    }
    return NULL;
}

static void scram_wipe(void* p, size_t n)
{
    volatile unsigned char* v = (volatile unsigned char*)p;
    while (n--)
        *v++ = 0;
}

static void scram_free_parts(dyn_scram_t* s)
{
    free(s->cfirst_bare);
    free(s->sfirst);
    s->cfirst_bare = s->sfirst = NULL;
}

static size_t scram_b64_decode(const char* s, size_t n, uint8_t* out)
{
    char pad[DYN_SCRAM_MAX_MSG + 4];
    size_t rem, padded;

    if (n == 0 || n > DYN_SCRAM_MAX_MSG)
        return 0;
    rem = n % 4;
    if (rem == 1)
        return 0;
    padded = n + (rem ? 4 - rem : 0);
    memcpy(pad, s, n);
    while (n < padded)
        pad[n++] = '=';
    {
        size_t got = dyn_codec_base64_decode(pad, padded, out);
        return got == SIZE_MAX ? 0 : got;
    }
}

void dyn_scram_free(dyn_scram_t* s)
{
    if (!s)
        return;
    scram_free_parts(s);
    scram_wipe(s->server_sig, sizeof(s->server_sig));
    s->step = 0;
}

int dyn_scram_client_first(dyn_scram_t* s, char* out, size_t outcap)
{
    uint8_t raw[DYN_SCRAM_RAW_NONCE];
    size_t nb, need;

    if (!s || !out)
        return DYN_SCRAM_E_SYNTAX;
    scram_free_parts(s);
    scram_wipe(s, sizeof(*s));

    if (dyn_os_entropy(raw, sizeof(raw)) < 0)
        return DYN_SCRAM_E_ENTROPY;
    nb = dyn_codec_base64_encode(raw, sizeof(raw), s->nonce);
    scram_wipe(raw, sizeof(raw));
    if (nb >= sizeof(s->nonce))
        return DYN_SCRAM_E_SHORT;
    s->nonce[nb] = '\0';

    need = strlen("n,,n=,r=") + nb;
    if (need + 1 > outcap)
        return DYN_SCRAM_E_SHORT;
    snprintf(out, outcap, "n,,n=,r=%s", s->nonce);

    {
        size_t cap = strlen("n=,r=") + nb + 1;
        s->cfirst_bare = (char*)malloc(cap);
        if (!s->cfirst_bare)
            return DYN_SCRAM_E_SHORT;
        snprintf(s->cfirst_bare, cap, "n=,r=%s", s->nonce);
    }
    s->step = 1;
    return (int)need;
}

int dyn_scram_server_first(dyn_scram_t* s, const char* msg, size_t len,
    const char* password, char* out, size_t outcap)
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
    const dyn_hash_algo_t* sha256;
    int rc = DYN_SCRAM_E_SYNTAX, i;

    if (!s || s->step != 1 || !msg || !password || !out)
        return DYN_SCRAM_E_STATE;
    if (len == 0 || len > DYN_SCRAM_MAX_MSG)
        return DYN_SCRAM_E_SYNTAX;
    if (memchr(msg, '\0', len) != NULL)
        return DYN_SCRAM_E_SYNTAX;

    if ((rv = attr(msg, len, 'e', &rlen)) != NULL) {
        size_t n = rlen < sizeof(s->err) - 1 ? rlen : sizeof(s->err) - 1;
        memcpy(s->err, rv, n);
        s->err[n] = '\0';
        return DYN_SCRAM_E_SERVER;
    }
    if ((mv = attr(msg, len, 'm', &mlen)) != NULL && mlen > 0) {
        (void)mv;
        return DYN_SCRAM_E_EXT;
    }

    rv = attr(msg, len, 'r', &rlen);
    sv = attr(msg, len, 's', &slen);
    iv = attr(msg, len, 'i', &ilen);
    if (!rv || !sv || !iv)
        return DYN_SCRAM_E_SYNTAX;

    nonce_len = strlen(s->nonce);
    if (rlen <= nonce_len || memcmp(rv, s->nonce, nonce_len) != 0)
        return DYN_SCRAM_E_NONCE;

    if (slen == 0 || slen > DYN_SCRAM_MAX_SALT)
        return DYN_SCRAM_E_SALT;
    saltlen = scram_b64_decode(sv, slen, salt);
    if (saltlen == 0 || saltlen > sizeof(salt))
        return DYN_SCRAM_E_SALT;

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

    s->sfirst = (char*)malloc(len + 1);
    if (!s->sfirst)
        return DYN_SCRAM_E_SHORT;
    memcpy(s->sfirst, msg, len);
    s->sfirst[len] = '\0';

    wlen = strlen("c=" SCRAM_CBIND_B64 ",r=") + rlen;
    without_proof = (char*)malloc(wlen + 1);
    if (!without_proof) {
        rc = DYN_SCRAM_E_SHORT;
        goto done;
    }
    memcpy(without_proof, "c=" SCRAM_CBIND_B64 ",r=",
        strlen("c=" SCRAM_CBIND_B64 ",r="));
    memcpy(without_proof + strlen("c=" SCRAM_CBIND_B64 ",r="), rv, rlen);
    without_proof[wlen] = '\0';

    alen = strlen(s->cfirst_bare) + 1 + len + 1 + wlen;
    authmsg = (char*)malloc(alen + 1);
    if (!authmsg) {
        rc = DYN_SCRAM_E_SHORT;
        goto done;
    }
    {
        size_t cfl = strlen(s->cfirst_bare);
        char* q = authmsg;
        memcpy(q, s->cfirst_bare, cfl);
        q += cfl;
        *q++ = ',';
        memcpy(q, msg, len);
        q += len;
        *q++ = ',';
        memcpy(q, without_proof, wlen);
        q += wlen;
        *q = '\0';
    }

    if (dyn_pbkdf2(sha256, (const uint8_t*)password, strlen(password),
            salt, saltlen, iters, salted, sizeof(salted))
        < 0) {
        rc = DYN_SCRAM_E_ITERS;
        goto done;
    }
    dyn_hmac(sha256, salted, sizeof(salted), (const uint8_t*)"Client Key", 10,
        ckey);
    dyn_sha256(ckey, sizeof(ckey), stored);
    dyn_hmac(sha256, stored, sizeof(stored), (const uint8_t*)authmsg,
        alen, csig);
    for (i = 0; i < DYN_SCRAM_KEY_LEN; i++)
        proof[i] = (uint8_t)(ckey[i] ^ csig[i]);

    dyn_hmac(sha256, salted, sizeof(salted), (const uint8_t*)"Server Key", 10,
        skey);
    dyn_hmac(sha256, skey, sizeof(skey), (const uint8_t*)authmsg,
        alen, s->server_sig);

    pb64 = dyn_codec_base64_encode(proof, sizeof(proof), proof_b64);
    proof_b64[pb64] = '\0';
    if (wlen + strlen(",p=") + pb64 + 1 > outcap) {
        rc = DYN_SCRAM_E_SHORT;
        goto done;
    }
    snprintf(out, outcap, "%s,p=%s", without_proof, proof_b64);
    rc = (int)(wlen + strlen(",p=") + pb64);
    s->step = 2;

done:
    scram_wipe(salted, sizeof(salted));
    scram_wipe(ckey, sizeof(ckey));
    scram_wipe(stored, sizeof(stored));
    scram_wipe(skey, sizeof(skey));
    scram_wipe(proof, sizeof(proof));
    scram_wipe(salt, sizeof(salt));
    if (authmsg) {
        scram_wipe(authmsg, alen);
        free(authmsg);
    }
    free(without_proof);
    return rc;
}

int dyn_scram_server_final(dyn_scram_t* s, const char* msg, size_t len)
{
    const char* vv;
    size_t vlen, n;
    uint8_t got[64];

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
    n = scram_b64_decode(vv, vlen, got);
    if (n != DYN_SCRAM_KEY_LEN)
        return DYN_SCRAM_E_VERIFY;
    if (!dyn_ct_equal(got, s->server_sig, DYN_SCRAM_KEY_LEN))
        return DYN_SCRAM_E_VERIFY;
    s->step = 3;
    return DYN_SCRAM_OK;
}
