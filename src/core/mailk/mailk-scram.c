#include <string.h>
#include "../dyn-mailk.h"
#include "../dyn-hash.h"
#include "../dyn-prng.h"

enum { SC_INIT, SC_C1, SC_C2, SC_CDONE, SC_S1, SC_S2, SC_SDONE, SC_FAIL };

static const dyn_hash_algo_t* sc_hash(int algo)
{
    return dyn_hash_algo_by_id(algo == MAILK_SCRAM_SHA1 ? DYN_HASH_SHA1
            : algo == MAILK_SCRAM_SHA512                ? DYN_HASH_SHA512
                                                        : DYN_HASH_SHA256);
}

static void wipe(void* p, size_t n)
{
    volatile uint8_t* v = p;
    while (n--)
        *v++ = 0;
}

static void sb_wipe(dyn_sb_t* b)
{
    if (b->p)
        wipe(b->p, b->cap);
    dyn_sb_free(b);
    dyn_sb_init(b);
}

void mailk_scram_init(mailk_scram_t* c, int algo)
{
    memset(c, 0, sizeof(*c));
    c->algo = algo == MAILK_SCRAM_SHA1 || algo == MAILK_SCRAM_SHA512 ? algo : MAILK_SCRAM_SHA256;
    c->dlen = sc_hash(c->algo)->digest_size;
    c->cbflag = 'n';
    c->min_iters = MAILK_SCRAM_ITER_MIN;
    c->max_iters = MAILK_SCRAM_ITER_MAX;
    dyn_sb_init(&c->gs2);
    dyn_sb_init(&c->msg);
    dyn_sb_init(&c->user);
    dyn_sb_init(&c->authzid);
    dyn_sb_init(&c->nonce);
    dyn_sb_init(&c->cbname);
    dyn_sb_init(&c->cbd);
}

void mailk_scram_free(mailk_scram_t* c)
{
    if (c->pw) {
        wipe(c->pw, c->pwn);
        free(c->pw);
    }
    sb_wipe(&c->gs2);
    sb_wipe(&c->msg);
    sb_wipe(&c->user);
    sb_wipe(&c->authzid);
    sb_wipe(&c->nonce);
    sb_wipe(&c->cbname);
    sb_wipe(&c->cbd);
    wipe(c, sizeof(*c));
}

static int cbname_ok(const char* s, size_t n)
{
    size_t i;
    if (!n || n > 64)
        return 0;
    for (i = 0; i < n; i++)
        if (!((s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9')
                || s[i] == '.' || s[i] == '-'))
            return 0;
    return 1;
}

int mailk_scram_cb(mailk_scram_t* c, int flag, const char* cbname, const uint8_t* cbdata, size_t cbdlen)
{
    if (c->st != SC_INIT || (flag != 'n' && flag != 'y' && flag != 'p'))
        return MAILK_EBAD;
    c->cbflag = flag;
    c->cbd.n = 0;
    c->plus = 0;
    c->cbname.n = 0;
    if (flag == 'p') {
        if (!cbname || !cbname_ok(cbname, strlen(cbname)) || !cbdata || !cbdlen || cbdlen > 1024)
            return MAILK_EBAD;
        dyn_sb_puts(&c->cbname, cbname);
        dyn_sb_put(&c->cbd, (const char*)cbdata, cbdlen);
        c->plus = 1;
    }
    return c->cbname.oom || c->cbd.oom ? MAILK_ENOMEM : MAILK_OK;
}

static int nonce_ok(const char* s, size_t n)
{
    size_t i;
    if (!n || n > 256)
        return 0;
    for (i = 0; i < n; i++)
        if ((uint8_t)s[i] < 0x21 || (uint8_t)s[i] > 0x7E || s[i] == ',')
            return 0;
    return 1;
}

static int gen_nonce(dyn_sb_t* out)
{
    uint8_t r[18];
    if (dyn_os_entropy(r, sizeof(r)) != 0)
        return MAILK_EBAD;
    return mailk_b64_encode(out, r, sizeof(r));
}

int mailk_scram_client_first(mailk_scram_t* c, const char* authcid, const char* authzid,
    const char* passwd, const char* cnonce, dyn_sb_t* out)
{
    dyn_sb_t prep;
    int rc;
    if (c->st != SC_INIT)
        return MAILK_EBAD;
    c->st = SC_FAIL;
    if (c->cbflag == 'p') {
        dyn_sb_puts(&c->gs2, "p=");
        dyn_sb_put(&c->gs2, c->cbname.p, c->cbname.n);
        dyn_sb_putc(&c->gs2, ',');
    } else {
        dyn_sb_putc(&c->gs2, (char)c->cbflag);
        dyn_sb_putc(&c->gs2, ',');
    }
    dyn_sb_init(&prep);
    if (authzid && *authzid) {
        if ((rc = mailk_saslprep(authzid, strlen(authzid), &prep)) != MAILK_OK)
            goto done;
        if (prep.n) {
            dyn_sb_puts(&c->gs2, "a=");
            if ((rc = mailk_saslname_encode(&c->gs2, prep.p, prep.n)) != MAILK_OK)
                goto done;
        }
        prep.n = 0;
    }
    dyn_sb_putc(&c->gs2, ',');
    if ((rc = mailk_saslprep(authcid, strlen(authcid), &prep)) != MAILK_OK)
        goto done;
    rc = MAILK_EBAD;
    if (!prep.n)
        goto done;
    dyn_sb_puts(&c->msg, "n=");
    if ((rc = mailk_saslname_encode(&c->msg, prep.p, prep.n)) != MAILK_OK)
        goto done;
    dyn_sb_puts(&c->msg, ",r=");
    if (cnonce) {
        rc = MAILK_EBAD;
        if (!nonce_ok(cnonce, strlen(cnonce)))
            goto done;
        dyn_sb_puts(&c->nonce, cnonce);
    } else if ((rc = gen_nonce(&c->nonce)) != MAILK_OK) {
        goto done;
    }
    dyn_sb_put(&c->msg, c->nonce.p, c->nonce.n);
    c->cnonce_n = c->nonce.n;
    sb_wipe(&prep);
    if ((rc = mailk_saslprep(passwd, strlen(passwd), &prep)) != MAILK_OK)
        goto done;
    c->pw = malloc(prep.n + 1);
    rc = MAILK_ENOMEM;
    if (!c->pw)
        goto done;
    memcpy(c->pw, prep.p ? prep.p : "", prep.n);
    c->pwn = prep.n;
    c->bare_n = c->msg.n;
    dyn_sb_put(out, c->gs2.p, c->gs2.n);
    dyn_sb_put(out, c->msg.p, c->msg.n);
    rc = c->gs2.oom || c->msg.oom || c->nonce.oom || out->oom ? MAILK_ENOMEM : MAILK_OK;
    if (rc == MAILK_OK)
        c->st = SC_C1;
done:
    sb_wipe(&prep);
    return rc;
}

static int attr(const uint8_t* m, size_t n, size_t* pos, char key, const uint8_t** v, size_t* vn)
{
    const uint8_t* e;
    if (*pos + 2 > n || m[*pos] != (uint8_t)key || m[*pos + 1] != '=')
        return 0;
    *v = m + *pos + 2;
    e = memchr(*v, ',', n - *pos - 2);
    *vn = e ? (size_t)(e - *v) : n - *pos - 2;
    *pos += 2 + *vn + (e ? 1 : 0);
    return 1;
}

static void keys(int algo, const uint8_t* sp, uint8_t* ck, uint8_t* stored, uint8_t* server)
{
    const dyn_hash_algo_t* a = sc_hash(algo);
    dyn_hmac(a, sp, a->digest_size, (const uint8_t*)"Client Key", 10, ck);
    dyn_hash_oneshot(a, ck, a->digest_size, stored);
    dyn_hmac(a, sp, a->digest_size, (const uint8_t*)"Server Key", 10, server);
}

int mailk_scram_derive(int algo, const char* passwd, const uint8_t* salt, size_t saltn, uint32_t iters,
    uint8_t* stored, uint8_t* server)
{
    dyn_sb_t prep;
    uint8_t sp[64], ck[64];
    int rc;
    if (iters < 1 || iters > MAILK_SCRAM_ITER_MAX || !saltn || saltn > 1024)
        return MAILK_EBAD;
    dyn_sb_init(&prep);
    if ((rc = mailk_saslprep(passwd, strlen(passwd), &prep)) == MAILK_OK) {
        dyn_pbkdf2(sc_hash(algo), (const uint8_t*)(prep.p ? prep.p : ""), prep.n, salt, saltn, iters, sp,
            sc_hash(algo)->digest_size);
        keys(algo, sp, ck, stored, server);
    }
    sb_wipe(&prep);
    wipe(sp, sizeof(sp));
    wipe(ck, sizeof(ck));
    return rc;
}

static int cbind_put(mailk_scram_t* c, dyn_sb_t* out)
{
    dyn_sb_t raw;
    int rc;
    dyn_sb_init(&raw);
    dyn_sb_put(&raw, c->gs2.p, c->gs2.n);
    if (c->cbflag == 'p')
        dyn_sb_put(&raw, c->cbd.p, c->cbd.n);
    rc = raw.oom ? MAILK_ENOMEM : mailk_b64_encode(out, (const uint8_t*)raw.p, raw.n);
    dyn_sb_free(&raw);
    return rc;
}

static int u32_parse(const uint8_t* v, size_t n, uint32_t* out)
{
    uint64_t x = 0;
    size_t i;
    if (!n || n > 10 || v[0] == '0')
        return 0;
    for (i = 0; i < n; i++) {
        if (v[i] < '0' || v[i] > '9')
            return 0;
        x = x * 10 + (v[i] - '0');
    }
    if (x > 0xFFFFFFFFu)
        return 0;
    *out = (uint32_t)x;
    return 1;
}

int mailk_scram_client_final(mailk_scram_t* c, const uint8_t* in, size_t n, dyn_sb_t* out)
{
    const uint8_t *r, *s, *iv;
    size_t rn, sn, ivn, pos = 0, i;
    dyn_sb_t salt;
    uint8_t sp[64], ck[64], csig[64];
    const dyn_hash_algo_t* a = sc_hash(c->algo);
    int rc = MAILK_EBAD;
    if (c->st != SC_C1)
        return MAILK_EBAD;
    c->st = SC_FAIL;
    if (n > MAILK_SASL_MAX || !attr(in, n, &pos, 'r', &r, &rn) || !attr(in, n, &pos, 's', &s, &sn)
        || !attr(in, n, &pos, 'i', &iv, &ivn) || rn <= c->cnonce_n || memcmp(r, c->nonce.p, c->cnonce_n)
        || !nonce_ok((const char*)r, rn) || !u32_parse(iv, ivn, &c->iters))
        return MAILK_EBAD;
    if (c->iters < c->min_iters || c->iters > c->max_iters)
        return MAILK_EAUTH;
    dyn_sb_init(&salt);
    if (!sn || mailk_b64_decode((const char*)s, sn, &salt) != MAILK_OK || !salt.n)
        goto done;
    dyn_pbkdf2(a, (const uint8_t*)(c->pw ? c->pw : ""), c->pwn, (const uint8_t*)salt.p, salt.n, c->iters, sp,
        c->dlen);
    keys(c->algo, sp, ck, c->stored, c->server);
    dyn_sb_putc(&c->msg, ',');
    dyn_sb_put(&c->msg, (const char*)in, n);
    dyn_sb_puts(&c->msg, ",c=");
    {
        size_t mark = c->msg.n;
        if ((rc = cbind_put(c, &c->msg)) != MAILK_OK)
            goto done;
        dyn_sb_puts(&c->msg, ",r=");
        dyn_sb_put(&c->msg, (const char*)r, rn);
        if (c->msg.oom) {
            rc = MAILK_ENOMEM;
            goto done;
        }
        dyn_sb_puts(out, "c=");
        dyn_sb_put(out, c->msg.p + mark, c->msg.n - mark);
    }
    dyn_hmac(a, c->stored, c->dlen, (const uint8_t*)c->msg.p, c->msg.n, csig);
    for (i = 0; i < c->dlen; i++)
        csig[i] ^= ck[i];
    dyn_sb_puts(out, ",p=");
    if ((rc = mailk_b64_encode(out, csig, c->dlen)) != MAILK_OK)
        goto done;
    dyn_hmac(a, c->server, c->dlen, (const uint8_t*)c->msg.p, c->msg.n, c->ssig);
    rc = out->oom ? MAILK_ENOMEM : MAILK_OK;
    if (rc == MAILK_OK)
        c->st = SC_C2;
done:
    if (c->pw) {
        wipe(c->pw, c->pwn);
        free(c->pw);
        c->pw = NULL;
    }
    dyn_sb_free(&salt);
    wipe(sp, sizeof(sp));
    wipe(ck, sizeof(ck));
    wipe(csig, sizeof(csig));
    return rc;
}

int mailk_scram_client_verify(mailk_scram_t* c, const uint8_t* in, size_t n)
{
    const uint8_t* v;
    size_t vn, pos = 0;
    dyn_sb_t sig;
    int rc = MAILK_EAUTH;
    if (c->st != SC_C2)
        return MAILK_EBAD;
    c->st = SC_FAIL;
    if (n > MAILK_SASL_MAX)
        return MAILK_EBAD;
    if (n >= 2 && in[0] == 'e' && in[1] == '=')
        return MAILK_EAUTH;
    if (!attr(in, n, &pos, 'v', &v, &vn))
        return MAILK_EBAD;
    dyn_sb_init(&sig);
    if (mailk_b64_decode((const char*)v, vn, &sig) == MAILK_OK && sig.n == c->dlen
        && dyn_ct_equal((const uint8_t*)sig.p, c->ssig, c->dlen))
        rc = MAILK_OK;
    dyn_sb_free(&sig);
    if (rc == MAILK_OK)
        c->st = SC_CDONE;
    return rc;
}

int mailk_scram_server_first(mailk_scram_t* s, const uint8_t* in, size_t n)
{
    const uint8_t *v, *e;
    size_t vn, pos;
    int flag;
    if (s->st != SC_INIT)
        return MAILK_EBAD;
    s->st = SC_FAIL;
    if (n < 3 || n > MAILK_SASL_MAX)
        return MAILK_EBAD;
    flag = in[0];
    if (flag == 'n' || flag == 'y') {
        if (in[1] != ',')
            return MAILK_EBAD;
        pos = 2;
    } else if (flag == 'p') {
        pos = 0;
        if (!attr(in, n, &pos, 'p', &v, &vn) || pos >= n || !cbname_ok((const char*)v, vn))
            return MAILK_EBAD;
        if (!s->plus || vn != s->cbname.n || memcmp(v, s->cbname.p, vn))
            return MAILK_EAUTH;
    } else {
        return MAILK_EBAD;
    }
    if (flag == 'y' && s->plus)
        return MAILK_EAUTH;
    if (pos < n && in[pos] == 'a') {
        if (!attr(in, n, &pos, 'a', &v, &vn) || !vn || mailk_saslname_decode(&s->authzid, (const char*)v, vn) != MAILK_OK)
            return MAILK_EBAD;
    } else if (pos >= n || in[pos] != ',') {
        return MAILK_EBAD;
    } else {
        pos++;
    }
    if (pos > n)
        return MAILK_EBAD;
    dyn_sb_put(&s->gs2, (const char*)in, pos);
    s->cbflag = flag;
    e = in + pos;
    {
        size_t bp = 0, bn = n - pos;
        const uint8_t *u, *r;
        size_t un, rn;
        if (!attr(e, bn, &bp, 'n', &u, &un) || !un || mailk_saslname_decode(&s->user, (const char*)u, un) != MAILK_OK
            || !attr(e, bn, &bp, 'r', &r, &rn) || !nonce_ok((const char*)r, rn))
            return MAILK_EBAD;
        if (e[bn - 1] == ',')
            return MAILK_EBAD;
        if (bp < bn && (e[bp] == 'm' || !(bn - bp >= 2 && e[bp + 1] == '=')))
            return MAILK_EBAD;
        dyn_sb_put(&s->nonce, (const char*)r, rn);
        s->cnonce_n = rn;
        dyn_sb_put(&s->msg, (const char*)e, bn);
    }
    if (s->user.oom || s->msg.oom || s->nonce.oom || s->gs2.oom || s->authzid.oom)
        return MAILK_ENOMEM;
    {
        size_t i;
        for (i = 0; i < s->user.n; i++)
            if (!s->user.p[i])
                return MAILK_EBAD;
    }
    s->st = SC_S1;
    return MAILK_OK;
}

int mailk_scram_server_challenge(mailk_scram_t* s, const uint8_t* salt, size_t saltn, uint32_t iters,
    const uint8_t* stored, const uint8_t* server, const char* snonce, dyn_sb_t* out)
{
    size_t mark;
    int rc;
    if (s->st != SC_S1 || !saltn || saltn > 1024 || iters < 1 || iters > MAILK_SCRAM_ITER_MAX)
        return MAILK_EBAD;
    s->st = SC_FAIL;
    if (snonce) {
        if (!nonce_ok(snonce, strlen(snonce)))
            return MAILK_EBAD;
        dyn_sb_puts(&s->nonce, snonce);
    } else if ((rc = gen_nonce(&s->nonce)) != MAILK_OK) {
        return rc;
    }
    memcpy(s->stored, stored, s->dlen);
    memcpy(s->server, server, s->dlen);
    s->iters = iters;
    dyn_sb_putc(&s->msg, ',');
    mark = s->msg.n;
    dyn_sb_puts(&s->msg, "r=");
    dyn_sb_put(&s->msg, s->nonce.p, s->nonce.n);
    dyn_sb_puts(&s->msg, ",s=");
    if ((rc = mailk_b64_encode(&s->msg, salt, saltn)) != MAILK_OK)
        return rc;
    dyn_sb_puts(&s->msg, ",i=");
    {
        char t[12];
        int l = 0;
        while (iters) {
            t[l++] = (char)('0' + iters % 10);
            iters /= 10;
        }
        while (l)
            dyn_sb_putc(&s->msg, t[--l]);
    }
    if (s->msg.oom || s->nonce.oom)
        return MAILK_ENOMEM;
    dyn_sb_put(out, s->msg.p + mark, s->msg.n - mark);
    if (out->oom)
        return MAILK_ENOMEM;
    s->st = SC_S2;
    return MAILK_OK;
}

static int server_err(mailk_scram_t* s, dyn_sb_t* out, const char* e)
{
    s->st = SC_FAIL;
    dyn_sb_puts(out, "e=");
    dyn_sb_puts(out, e);
    return out->oom ? MAILK_ENOMEM : MAILK_EAUTH;
}

int mailk_scram_server_final(mailk_scram_t* s, const uint8_t* in, size_t n, dyn_sb_t* out)
{
    const uint8_t *cv, *r, *pv;
    size_t cn, rn, pn, pos = 0, last = 0, i;
    const dyn_hash_algo_t* a = sc_hash(s->algo);
    dyn_sb_t cb, proof;
    uint8_t csig[64], ck[64], hk[64];
    int rc;
    if (s->st != SC_S2)
        return MAILK_EBAD;
    s->st = SC_FAIL;
    if (n > MAILK_SASL_MAX || !attr(in, n, &pos, 'c', &cv, &cn) || !attr(in, n, &pos, 'r', &r, &rn))
        return server_err(s, out, "other-error");
    while (pos < n && !(in[pos] == 'p' && pos + 1 < n && in[pos + 1] == '=')) {
        const uint8_t* x;
        size_t xn;
        if (!attr(in, n, &pos, (char)in[pos], &x, &xn) || in[pos - 1] != ',')
            return server_err(s, out, "other-error");
    }
    last = pos;
    if (!attr(in, n, &pos, 'p', &pv, &pn) || pos != n || !last)
        return server_err(s, out, "other-error");
    if (rn != s->nonce.n || memcmp(r, s->nonce.p, rn))
        return server_err(s, out, "other-error");
    dyn_sb_init(&cb);
    dyn_sb_init(&proof);
    rc = mailk_b64_decode((const char*)cv, cn, &cb);
    if (rc != MAILK_OK || cb.n != s->gs2.n + (s->cbflag == 'p' ? s->cbd.n : 0) || memcmp(cb.p, s->gs2.p, s->gs2.n)
        || (s->cbflag == 'p' && memcmp(cb.p + s->gs2.n, s->cbd.p, s->cbd.n))) {
        dyn_sb_free(&cb);
        return server_err(s, out, "channel-bindings-dont-match");
    }
    dyn_sb_free(&cb);
    if (mailk_b64_decode((const char*)pv, pn, &proof) != MAILK_OK || proof.n != s->dlen) {
        dyn_sb_free(&proof);
        return server_err(s, out, "invalid-proof");
    }
    dyn_sb_putc(&s->msg, ',');
    dyn_sb_put(&s->msg, (const char*)in, last - 1);
    if (s->msg.oom) {
        dyn_sb_free(&proof);
        return MAILK_ENOMEM;
    }
    dyn_hmac(a, s->stored, s->dlen, (const uint8_t*)s->msg.p, s->msg.n, csig);
    for (i = 0; i < s->dlen; i++)
        ck[i] = (uint8_t)proof.p[i] ^ csig[i];
    dyn_sb_free(&proof);
    dyn_hash_oneshot(a, ck, s->dlen, hk);
    rc = dyn_ct_equal(hk, s->stored, s->dlen);
    wipe(ck, sizeof(ck));
    if (!rc)
        return server_err(s, out, "invalid-proof");
    dyn_hmac(a, s->server, s->dlen, (const uint8_t*)s->msg.p, s->msg.n, csig);
    dyn_sb_puts(out, "v=");
    if ((rc = mailk_b64_encode(out, csig, s->dlen)) != MAILK_OK)
        return rc;
    s->st = SC_SDONE;
    return MAILK_OK;
}
