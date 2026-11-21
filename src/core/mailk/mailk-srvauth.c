#include <stdlib.h>
#include <string.h>
#include "mailk-srv.h"

static const struct {
    const char* name;
    unsigned bit;
    int algo;
} smechs[] = {
    { "PLAIN", MAILK_MECH_PLAIN, 0 }, { "LOGIN", MAILK_MECH_LOGIN, 0 }, { "XOAUTH2", MAILK_MECH_XOAUTH2, 0 },
    { "OAUTHBEARER", MAILK_MECH_OAUTHBEARER, 0 }, { "SCRAM-SHA-1", MAILK_MECH_SCRAM_SHA1, MAILK_SCRAM_SHA1 },
    { "SCRAM-SHA-256", MAILK_MECH_SCRAM_SHA256, MAILK_SCRAM_SHA256 }, { "SCRAM-SHA-512", MAILK_MECH_SCRAM_SHA512, MAILK_SCRAM_SHA512 },
};

int mailk_srv_mechs_line(mailk_srv_t* s)
{
    size_t i;
    dyn_sb_puts(&s->out, "250-AUTH");
    for (i = 0; i < sizeof smechs / sizeof smechs[0]; i++)
        if (s->mechs & smechs[i].bit) {
            dyn_sb_putc(&s->out, ' ');
            dyn_sb_puts(&s->out, smechs[i].name);
        }
    dyn_sb_put(&s->out, "\r\n", 2);
    return 0;
}

int mailk_srv_auth_failed(mailk_srv_t* s, int code, const char* esc, const char* text)
{
    mailk_srv_reply(s, code, esc, text);
    s->st = V_CMD;
    mailk_srv_wipe(&s->secret);
    if (s->auth_max && ++s->auth_fails >= s->auth_max) {
        mailk_srv_reply(s, 421, "4.7.0", "Too many failed authentication attempts");
        mailk_srv_close(s);
    }
    return 0;
}

int mailk_srv_auth_reject(mailk_srv_t* s, int code, const char* esc, const char* text)
{
    char b[16];
    if (s->mech != MAILK_MECH_XOAUTH2 && s->mech != MAILK_MECH_OAUTHBEARER)
        return mailk_srv_auth_failed(s, code, esc, text);
    s->rej.n = 0;
    b[0] = (char)('0' + code / 100);
    b[1] = (char)('0' + code / 10 % 10);
    b[2] = (char)('0' + code % 10);
    dyn_sb_put(&s->rej, b, 3);
    dyn_sb_puts(&s->rej, esc ? esc : "");
    dyn_sb_putc(&s->rej, 0);
    dyn_sb_puts(&s->rej, text);
    dyn_sb_putc(&s->rej, 0);
    dyn_sb_puts(&s->out, "334 eyJzdGF0dXMiOiJpbnZhbGlkX3Rva2VuIn0=\r\n");
    s->st = V_OERR;
    return 0;
}

static int challenge(mailk_srv_t* s, const dyn_sb_t* msg)
{
    dyn_sb_puts(&s->out, "334 ");
    mailk_b64_encode(&s->out, (const uint8_t*)msg->p, msg->n);
    dyn_sb_put(&s->out, "\r\n", 2);
    s->st = V_AUTH;
    return 0;
}

int mailk_srv_scram_creds(mailk_srv_t* s, const uint8_t* salt, size_t saltn, uint32_t iters,
    const uint8_t* stored, const uint8_t* server)
{
    dyn_sb_t m;
    int rc;
    if (s->pend != MAILK_PEND_AUTH || (unsigned)s->mech < MAILK_MECH_SCRAM_SHA1 || s->st == V_CLOSED)
        return MAILK_EBAD;
    s->pend = MAILK_PEND_NONE;
    s->ev &= ~MAILK_SEV_PENDING;
    dyn_sb_init(&m);
    rc = mailk_scram_server_challenge(&s->scram, salt, saltn, iters, stored, server, NULL, &m);
    if (rc == MAILK_OK) {
        s->step = 1;
        challenge(s, &m);
    } else
        mailk_srv_auth_failed(s, 454, "4.7.0", "Temporary authentication failure");
    dyn_sb_free(&m);
    return s->out.oom ? MAILK_ENOMEM : MAILK_OK;
}

static int scram_step(mailk_srv_t* s, const dyn_sb_t* d)
{
    dyn_sb_t m;
    int rc;
    if (s->step == 0) {
        rc = mailk_scram_server_first(&s->scram, (const uint8_t*)d->p, d->n);
        if (rc != MAILK_OK)
            return mailk_srv_auth_failed(s, rc == MAILK_EAUTH ? 535 : 501, rc == MAILK_EAUTH ? "5.7.8" : "5.5.2", rc == MAILK_EAUTH ? "Channel binding not supported" : "Malformed authentication response");
        dyn_sb_put(&s->user, s->scram.user.p, s->scram.user.n);
        dyn_sb_put(&s->authz, s->scram.authzid.p, s->scram.authzid.n);
        if (!mailk_srv_want(s, MAILK_PEND_AUTH))
            return mailk_srv_auth_failed(s, 535, "5.7.8", "Authentication credentials invalid");
        return 0;
    }
    dyn_sb_init(&m);
    rc = mailk_scram_server_final(&s->scram, (const uint8_t*)d->p, d->n, &m);
    if (rc == MAILK_OK) {
        s->step = 2;
        challenge(s, &m);
    } else
        mailk_srv_auth_failed(s, 535, "5.7.8", "Authentication credentials invalid");
    dyn_sb_free(&m);
    return 0;
}

static int auth_data(mailk_srv_t* s, const char* a, size_t n, int ir)
{
    dyn_sb_t d;
    int rc;
    s->st = V_CMD;
    if (s->step == 2 && (unsigned)(unsigned)s->mech >= MAILK_MECH_SCRAM_SHA1)
        return n ? mailk_srv_auth_failed(s, 535, "5.7.8", "Authentication credentials invalid") : mailk_srv_accept(s, MAILK_PEND_AUTH, 0, NULL, NULL);
    dyn_sb_init(&d);
    rc = mailk_sasl_resp_decode(a, n, &d);
    if (rc != MAILK_OK || (ir && !d.n && s->mech == MAILK_MECH_LOGIN)) {
        mailk_srv_wipe(&d);
        dyn_sb_free(&d);
        return mailk_srv_bad(s, 501, rc == MAILK_CANCEL ? "5.0.0" : "5.5.2", rc == MAILK_CANCEL ? "Authentication cancelled" : "Cannot decode response");
    }
    if ((unsigned)(unsigned)s->mech >= MAILK_MECH_SCRAM_SHA1)
        rc = scram_step(s, &d);
    else if (s->mech == MAILK_MECH_LOGIN && s->step == 0) {
        dyn_sb_put(&s->user, d.p, d.n);
        s->step = 1;
        s->st = V_AUTH;
        dyn_sb_puts(&s->out, "334 UGFzc3dvcmQ6\r\n");
    } else {
        if (s->mech == MAILK_MECH_LOGIN)
            dyn_sb_put(&s->secret, d.p, d.n);
        else if (s->mech == MAILK_MECH_PLAIN) {
            mailk_plain_t pl;
            if ((rc = mailk_plain_parse((const uint8_t*)d.p, d.n, &pl)) == MAILK_OK) {
                dyn_sb_put(&s->authz, d.p + pl.az, pl.azn);
                dyn_sb_put(&s->user, d.p + pl.ac, pl.acn);
                dyn_sb_put(&s->secret, d.p + pl.pw, pl.pwn);
            }
        } else {
            mailk_oauth_t o;
            rc = s->mech == MAILK_MECH_XOAUTH2 ? mailk_xoauth2_parse((const uint8_t*)d.p, d.n, &o) : mailk_oauthbearer_parse((const uint8_t*)d.p, d.n, &o);
            if (rc == MAILK_OK) {
                dyn_sb_put(&s->user, d.p + o.az, o.azn);
                dyn_sb_put(&s->secret, d.p + o.tok, o.tokn);
            }
        }
        if (rc != MAILK_OK)
            mailk_srv_auth_failed(s, 501, "5.5.2", "Malformed authentication response");
        else if (!mailk_srv_want(s, MAILK_PEND_AUTH))
            mailk_srv_auth_failed(s, 535, "5.7.8", "Authentication credentials invalid");
    }
    mailk_srv_wipe(&d);
    dyn_sb_free(&d);
    return 0;
}

int mailk_srv_auth_line(mailk_srv_t* s, const char* l, size_t n)
{
    const char* r = s->rej.p;
    size_t a;
    if (s->st != V_OERR)
        return auth_data(s, l, n, 0);
    a = strlen(r) + 1;
    return mailk_srv_auth_failed(s, (r[0] - '0') * 100 + (r[1] - '0') * 10 + (r[2] - '0'), r[3] ? r + 3 : NULL, r + a);
}

int mailk_srv_cmd_auth(mailk_srv_t* s, const char* a, size_t n)
{
    size_t k, i;
    int algo = 0;
    if (!(s->caps & MAILK_CAP_AUTH))
        return mailk_srv_bad(s, 502, "5.5.1", "Command not implemented");
    if (!s->ehlo)
        return mailk_srv_bad(s, 503, "5.5.1", "Send EHLO first");
    if (s->authed)
        return mailk_srv_bad(s, 503, "5.5.1", "Already authenticated");
    if (s->tx)
        return mailk_srv_bad(s, 503, "5.5.1", "AUTH not permitted during a mail transaction");
    if (!s->tls_on && !s->insecure_auth)
        return mailk_srv_bad(s, 538, "5.7.11", "Encryption required for requested authentication mechanism");
    for (k = 0; k < n && a[k] != ' '; k++)
        ;
    s->mech = 0;
    for (i = 0; i < sizeof smechs / sizeof smechs[0]; i++)
        if ((s->mechs & smechs[i].bit) && mailk_srv_ieqn(a, k, smechs[i].name)) {
            s->mech = (int)smechs[i].bit;
            algo = smechs[i].algo;
        }
    if (!s->mech)
        return mailk_srv_bad(s, 504, "5.5.4", "Unrecognized authentication type");
    s->step = 0;
    s->user.n = s->authz.n = 0;
    mailk_srv_wipe(&s->secret);
    if (algo) {
        mailk_scram_free(&s->scram);
        mailk_scram_init(&s->scram, algo);
    }
    if (k < n)
        return auth_data(s, a + k + 1, n - k - 1, 1);
    s->st = V_AUTH;
    dyn_sb_puts(&s->out, s->mech == MAILK_MECH_LOGIN ? "334 VXNlcm5hbWU6\r\n" : "334 \r\n");
    return 0;
}
