#include "dkim-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_DKIM) && defined(CONFIG_OPENSSL)

#include <stdio.h>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>

static int lc(int c)
{
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

static EVP_PKEY* load_private(dkim_sign_job_t* j)
{
    BIO* b;
    EVP_PKEY* k;
    if (j->has_seed)
        return EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL, j->seed, 32);
    b = BIO_new_mem_buf(j->pem, (int)j->pemn);
    if (!b)
        return NULL;
    k = PEM_read_bio_PrivateKey(b, NULL, NULL, NULL);
    BIO_free(b);
    return k;
}

static int do_sign(EVP_PKEY* k, int algo, const uint8_t hh[32], uint8_t** sig, size_t* sl)
{
    int ok = 0;
    *sig = NULL;
    *sl = 0;
    if (algo == DKIM_RSA) {
        EVP_PKEY_CTX* c = EVP_PKEY_CTX_new(k, NULL);
        if (c && EVP_PKEY_sign_init(c) > 0 && EVP_PKEY_CTX_set_rsa_padding(c, RSA_PKCS1_PADDING) > 0
            && EVP_PKEY_CTX_set_signature_md(c, EVP_sha256()) > 0 && EVP_PKEY_sign(c, NULL, sl, hh, 32) > 0
            && (*sig = (uint8_t*)malloc(*sl)) && EVP_PKEY_sign(c, *sig, sl, hh, 32) > 0)
            ok = 1;
        EVP_PKEY_CTX_free(c);
    } else {
        EVP_MD_CTX* m = EVP_MD_CTX_new();
        *sl = 64;
        if (m && (*sig = (uint8_t*)malloc(64)) && EVP_DigestSignInit(m, NULL, NULL, NULL, k) > 0
            && EVP_DigestSign(m, *sig, sl, hh, 32) > 0)
            ok = 1;
        EVP_MD_CTX_free(m);
    }
    if (!ok) {
        free(*sig);
        *sig = NULL;
    }
    return ok;
}

int dkim_sign_work(dkim_sign_job_t* j)
{
    EVP_PKEY* k = load_private(j);
    dkim_msg_t* m = NULL;
    uint8_t hh[32], *sig = NULL;
    size_t sl = 0;
    int id, rc;
    if (!k) {
        j->err = "privateKey is not a PEM private key";
        return -1;
    }
    id = EVP_PKEY_get_base_id(k);
    if (id == EVP_PKEY_RSA) {
        j->o.algo = DKIM_RSA;
        if (EVP_PKEY_get_bits(k) < 1024) {
            j->err = "RSA key is shorter than 1024 bits (RFC 8301)";
            goto out;
        }
    } else if (id == EVP_PKEY_ED25519)
        j->o.algo = DKIM_ED25519;
    else {
        j->err = "privateKey must be RSA or Ed25519";
        goto out;
    }
    if (j->algo_req && j->algo_req != j->o.algo) {
        j->err = "algorithm does not match the private key";
        goto out;
    }
    m = (dkim_msg_t*)calloc(1, sizeof *m);
    if (!m) {
        j->err = "out of memory";
        goto out;
    }
    rc = dkim_msg_parse(m, j->msg, j->n);
    if (rc != DKIM_OK) {
        j->err = rc == DKIM_ELONG ? "message has more than 1000 header fields" : "message header is malformed";
        goto out;
    }
    if ((j->err = dkim_sign_field(&j->out, m, &j->o, hh)))
        goto out;
    if (!do_sign(k, j->o.algo, hh, &sig, &sl)) {
        j->err = "signing failed";
        goto out;
    }
    if (dkim_fold_b64(&j->out, sig, sl) != DKIM_OK) {
        j->err = "out of memory";
        goto out;
    }
    dyn_sb_put(&j->out, "\r\n", 2);
    if (j->out.oom)
        j->err = "out of memory";
out:
    free(sig);
    if (m)
        dkim_msg_free(m);
    free(m);
    EVP_PKEY_free(k);
    return j->err ? -1 : 0;
}

static void set(dkim_vsig_t* v, int st, const char* why)
{
    v->status = st;
    v->reason = why;
}

static int name_chars_ok(const dyn_sb_t* b)
{
    size_t i;
    if (!b->n || b->n > 253)
        return 0;
    for (i = 0; i < b->n; i++) {
        int c = (uint8_t)b->p[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_'))
            return 0;
    }
    return 1;
}

static int body_hash(const dkim_verify_job_t* j, dkim_vsig_t* v)
{
    dkim_body_t* b = (dkim_body_t*)malloc(sizeof *b);
    uint8_t bh[32];
    int r;
    if (!b)
        return -1;
    dkim_body_init(b, v->s.brel, v->s.l);
    dkim_body_feed(b, j->msg + j->m->body, j->n - j->m->body);
    dkim_body_final(b, bh);
    if (v->s.l != DKIM_NO_LIMIT && b->n < v->s.l)
        r = 1;
    else
        r = memcmp(bh, v->s.bh, 32) ? 2 : 0;
    free(b);
    return r;
}

int dkim_verify_stage1(dkim_verify_job_t* j)
{
    size_t f, vo, cnt = 0;
    int rc;
    j->m = (dkim_msg_t*)calloc(1, sizeof *j->m);
    if (!j->m)
        return -1;
    if ((rc = dkim_msg_parse(j->m, j->msg, j->n)) != DKIM_OK) {
        if (rc == DKIM_ENOMEM)
            return -1;
        j->bad_msg = 1;
        return 0;
    }
    for (f = 0; f < j->m->nf; f++)
        cnt += (size_t)dkim_name_is(j->m, f, "dkim-signature", 14);
    if (cnt > DKIM_SIGS_MAX)
        cnt = DKIM_SIGS_MAX;
    if (cnt && !(j->sig = (dkim_vsig_t*)calloc(cnt, sizeof *j->sig)))
        return -1;
    for (f = 0; f < j->m->nf && j->ns < cnt; f++) {
        const dkim_field_t* fd = &j->m->f[f];
        dkim_vsig_t* v;
        const char* e;
        int r;
        if (!dkim_name_is(j->m, f, "dkim-signature", 14))
            continue;
        v = &j->sig[j->ns++];
        dkim_sig_init(&v->s);
        dyn_sb_init(&v->txt);
        v->parsed = 1;
        if (j->ns > j->max) {
            set(v, VS_NEUTRAL, "not evaluated: over maxSignatures");
            continue;
        }
        for (vo = fd->nlen; vo < fd->len && j->msg[fd->off + vo] != ':'; vo++)
            ;
        vo++;
        if ((e = dkim_sig_parse(&v->s, j->msg + fd->off + vo, fd->len - vo))) {
            set(v, VS_PERMERROR, e);
            continue;
        }
        if (!name_chars_ok(&v->s.d) || !name_chars_ok(&v->s.s) || v->s.d.n + v->s.s.n + 12 > 253) {
            set(v, VS_PERMERROR, "d= or s= is not a valid DNS name");
            continue;
        }
        if (v->s.x >= 0 && v->s.x < j->now) {
            set(v, VS_PERMERROR, "signature expired");
            continue;
        }
        r = body_hash(j, v);
        if (r < 0)
            return -1;
        if (r == 1) {
            set(v, VS_FAIL, "l= is longer than the body");
            continue;
        }
        if (r == 2) {
            set(v, VS_FAIL, "body hash did not verify");
            continue;
        }
        if (dkim_hdr_hash(j->m, &v->s, f, v->hh) != DKIM_OK) {
            set(v, VS_PERMERROR, "b= tag not found");
            continue;
        }
        set(v, VS_PENDING, NULL);
    }
    return 0;
}

static EVP_PKEY* load_public(const dkim_key_t* k)
{
    const unsigned char* p = (const unsigned char*)k->p.p;
    EVP_PKEY* pk;
    if (k->k == DKIM_ED25519)
        return k->p.n == 32 ? EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, NULL, p, 32) : NULL;
    pk = d2i_PUBKEY(NULL, &p, (long)k->p.n);
    if (pk && EVP_PKEY_get_base_id(pk) != EVP_PKEY_RSA) {
        EVP_PKEY_free(pk);
        return NULL;
    }
    if (!pk) {
        p = (const unsigned char*)k->p.p;
        pk = d2i_PublicKey(EVP_PKEY_RSA, NULL, &p, (long)k->p.n);
    }
    return pk;
}

static int do_verify(EVP_PKEY* pk, int algo, const uint8_t hh[32], const dyn_sb_t* sig)
{
    int ok = 0;
    if (algo == DKIM_RSA) {
        EVP_PKEY_CTX* c = EVP_PKEY_CTX_new(pk, NULL);
        if (c && EVP_PKEY_verify_init(c) > 0 && EVP_PKEY_CTX_set_rsa_padding(c, RSA_PKCS1_PADDING) > 0
            && EVP_PKEY_CTX_set_signature_md(c, EVP_sha256()) > 0)
            ok = EVP_PKEY_verify(c, (const unsigned char*)sig->p, sig->n, hh, 32) == 1;
        EVP_PKEY_CTX_free(c);
    } else {
        EVP_MD_CTX* m = EVP_MD_CTX_new();
        if (m && EVP_DigestVerifyInit(m, NULL, NULL, NULL, pk) > 0)
            ok = EVP_DigestVerify(m, (const unsigned char*)sig->p, sig->n, hh, 32) == 1;
        EVP_MD_CTX_free(m);
    }
    return ok;
}

static int strict_ok(const dkim_sig_t* s)
{
    const char* at = NULL;
    size_t dn, i, k = s->i.n;
    while (k-- > 0)
        if (s->i.p[k] == '@') {
            at = s->i.p + k;
            break;
        }
    if (!at)
        return 0;
    at++;
    dn = s->i.n - (size_t)(at - s->i.p);
    if (dn != s->d.n)
        return 0;
    for (i = 0; i < dn; i++)
        if (lc((uint8_t)at[i]) != lc((uint8_t)s->d.p[i]))
            return 0;
    return 1;
}

int dkim_verify_stage2(dkim_verify_job_t* j)
{
    size_t i;
    for (i = 0; i < j->ns; i++) {
        dkim_vsig_t* v = &j->sig[i];
        dkim_key_t k;
        EVP_PKEY* pk;
        const char* e;
        if (v->status != VS_PENDING || v->keyst != 1)
            continue;
        dkim_key_init(&k);
        e = dkim_key_parse(&k, (const uint8_t*)v->txt.p, v->txt.n);
        v->testing = k.testing;
        if (e)
            set(v, VS_PERMERROR, e);
        else if (k.revoked)
            set(v, VS_PERMERROR, "key revoked");
        else if (k.k != v->s.algo)
            set(v, VS_PERMERROR, "key type does not match a=");
        else if (k.strict && !strict_ok(&v->s))
            set(v, VS_PERMERROR, "key t=s requires i= domain to equal d=");
        else if (!(pk = load_public(&k)))
            set(v, VS_PERMERROR, "key p= is not a valid public key");
        else {
            if (k.k == DKIM_RSA && EVP_PKEY_get_bits(pk) < j->min_bits) {
                snprintf(v->rbuf, sizeof v->rbuf, "RSA key has %d bits, under minRsaBits %d", EVP_PKEY_get_bits(pk), j->min_bits);
                set(v, VS_PERMERROR, v->rbuf);
            } else if (do_verify(pk, v->s.algo, v->hh, &v->s.b))
                set(v, VS_PASS, NULL);
            else
                set(v, VS_FAIL, "signature did not verify");
            EVP_PKEY_free(pk);
        }
        dkim_key_free(&k);
    }
    return 0;
}

#endif
