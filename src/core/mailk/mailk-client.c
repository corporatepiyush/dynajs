#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../dyn-mailk.h"

enum { C_GREET, C_EHLO, C_HELO, C_STLS, C_TLSWAIT, C_AUTH, C_READY, C_TX, C_RSETU, C_QUIT, C_DEAD, C_CLOSED };
enum { K_MAIL = 1, K_RCPT, K_DATA, K_DOT, K_DOTX, K_BDAT, K_RSETX };

static const struct {
    const char* name;
    unsigned bit;
} mechtab[] = {
    { "PLAIN", MAILK_MECH_PLAIN }, { "LOGIN", MAILK_MECH_LOGIN }, { "CRAM-MD5", MAILK_MECH_CRAM_MD5 },
    { "XOAUTH2", MAILK_MECH_XOAUTH2 }, { "OAUTHBEARER", MAILK_MECH_OAUTHBEARER },
    { "SCRAM-SHA-1", MAILK_MECH_SCRAM_SHA1 }, { "SCRAM-SHA-256", MAILK_MECH_SCRAM_SHA256 },
    { "SCRAM-SHA-512", MAILK_MECH_SCRAM_SHA512 },
};

static const struct {
    const char* kw;
    unsigned bit;
} captab[] = {
    { "PIPELINING", MAILK_CAP_PIPELINING }, { "8BITMIME", MAILK_CAP_8BITMIME }, { "SMTPUTF8", MAILK_CAP_SMTPUTF8 },
    { "CHUNKING", MAILK_CAP_CHUNKING }, { "BINARYMIME", MAILK_CAP_BINARYMIME }, { "DSN", MAILK_CAP_DSN },
    { "STARTTLS", MAILK_CAP_STARTTLS }, { "SIZE", MAILK_CAP_SIZE }, { "ENHANCEDSTATUSCODES", MAILK_CAP_ENHANCED },
    { "AUTH", MAILK_CAP_AUTH },
};

static int ieqn(const char* a, size_t n, const char* lit)
{
    size_t i;
    if (strlen(lit) != n)
        return 0;
    for (i = 0; i < n; i++)
        if ((a[i] >= 'a' && a[i] <= 'z' ? a[i] - 32 : a[i]) != lit[i])
            return 0;
    return 1;
}

static char* dupz(const char* s)
{
    char* d;
    size_t n;
    if (!s)
        return NULL;
    n = strlen(s);
    d = malloc(n + 1);
    if (d)
        memcpy(d, s, n + 1);
    return d;
}

static void wipez(char** p)
{
    if (*p) {
        volatile char* v = *p;
        while (*v)
            *v++ = 0;
        free(*p);
        *p = NULL;
    }
}

static int fail(mailk_cli_t* c, const char* msg)
{
    c->err.n = 0;
    dyn_sb_puts(&c->err, msg);
    if (c->last.code) {
        char b[8];
        snprintf(b, sizeof b, " (%d", c->last.code);
        dyn_sb_puts(&c->err, b);
        if (c->last.len) {
            dyn_sb_putc(&c->err, ' ');
            dyn_sb_put(&c->err, c->texts.p + c->last.off, c->last.len);
        }
        dyn_sb_putc(&c->err, ')');
    }
    dyn_sb_putc(&c->err, 0);
    c->err.n--;
    c->st = C_DEAD;
    c->ev |= MAILK_EV_ERROR;
    wipez(&c->pass);
    wipez(&c->token);
    return MAILK_EBAD;
}

static int local_err(mailk_cli_t* c, const char* msg)
{
    c->err.n = 0;
    dyn_sb_puts(&c->err, msg);
    dyn_sb_putc(&c->err, 0);
    c->err.n--;
    return MAILK_EBAD;
}

static void save(mailk_cli_t* c, mailk_res_t* d)
{
    d->code = c->r.code;
    d->cls = c->r.esc_class;
    d->sub = c->r.esc_subject;
    d->det = c->r.esc_detail;
    d->off = c->texts.n;
    d->len = c->r.text.n;
    dyn_sb_put(&c->texts, c->r.text.p, c->r.text.n);
}

static int qpush(mailk_cli_t* c, uint8_t k)
{
    if (c->qn == c->qcap) {
        size_t nc = c->qcap ? c->qcap * 2 : 16, i;
        uint8_t* nq = malloc(nc);
        if (!nq)
            return MAILK_ENOMEM;
        for (i = 0; i < c->qn; i++)
            nq[i] = c->q[(c->qh + i) % c->qcap];
        free(c->q);
        c->q = nq;
        c->qcap = nc;
        c->qh = 0;
    }
    c->q[(c->qh + c->qn++) % c->qcap] = k;
    return MAILK_OK;
}

static int qpop(mailk_cli_t* c)
{
    int k;
    if (!c->qn)
        return 0;
    k = c->q[c->qh];
    c->qh = (c->qh + 1) % c->qcap;
    c->qn--;
    return k;
}

int mailk_cli_init(mailk_cli_t* c, const mailk_cli_opts_t* o)
{
    const char* nm = o->name && *o->name ? o->name : "localhost";
    size_t nl = strlen(nm);
    memset(c, 0, sizeof *c);
    mailk_reply_init(&c->r);
    mailk_scram_init(&c->scram, MAILK_SCRAM_SHA256);
    dyn_sb_init(&c->out);
    dyn_sb_init(&c->texts);
    dyn_sb_init(&c->err);
    dyn_sb_init(&c->body);
    dyn_sb_init(&c->cmd);
    if (o->tls < MAILK_TLS_NONE || o->tls > MAILK_TLS_IMPLICIT)
        return local_err(c, "SMTP: invalid tls mode");
    if (!(mailk_domain_ok(nm, nl, 0) || (nl > 2 && nm[0] == '[' && nm[nl - 1] == ']' && !strpbrk(nm, " \r\n"))))
        return local_err(c, "SMTP: invalid EHLO name");
    if ((o->user || o->token) && !(o->user && *o->user && (o->pass || o->token)))
        return local_err(c, "SMTP: AUTH needs user and pass (or token)");
    if ((o->user && strpbrk(o->user, "\r\n")) || (o->pass && strpbrk(o->pass, "\r\n")))
        return local_err(c, "SMTP: CR or LF in credentials");
    c->tls_mode = o->tls;
    c->tls_on = o->tls == MAILK_TLS_IMPLICIT;
    c->mechs = o->mechs ? o->mechs : MAILK_MECH_DEFAULT;
    c->insecure_auth = o->insecure_auth;
    c->scram_max_iters = o->scram_max_iters;
    c->name = dupz(nm);
    c->user = dupz(o->user);
    c->pass = dupz(o->pass);
    c->token = dupz(o->token);
    c->authzid = dupz(o->authzid);
    if (!c->name || (o->user && !c->user) || (o->pass && !c->pass) || (o->token && !c->token) || (o->authzid && !c->authzid))
        return local_err(c, "SMTP: out of memory");
    c->st = C_GREET;
    return MAILK_OK;
}

void mailk_cli_free(mailk_cli_t* c)
{
    mailk_reply_free(&c->r);
    mailk_scram_free(&c->scram);
    dyn_sb_free(&c->out);
    dyn_sb_free(&c->texts);
    dyn_sb_free(&c->err);
    dyn_sb_free(&c->body);
    dyn_sb_free(&c->cmd);
    free(c->q);
    free(c->rcpt);
    free(c->name);
    free(c->user);
    free(c->authzid);
    wipez(&c->pass);
    wipez(&c->token);
    c->q = NULL;
    c->rcpt = NULL;
}

static void put_line(mailk_cli_t* c, const char* a, const char* b)
{
    dyn_sb_puts(&c->out, a);
    if (b)
        dyn_sb_puts(&c->out, b);
    dyn_sb_put(&c->out, "\r\n", 2);
}

static int put_b64(mailk_cli_t* c, const char* prefix, const uint8_t* p, size_t n)
{
    int rc;
    dyn_sb_puts(&c->out, prefix);
    rc = mailk_sasl_resp_encode(&c->out, p, n);
    dyn_sb_put(&c->out, "\r\n", 2);
    return rc;
}

static void send_ehlo(mailk_cli_t* c)
{
    c->caps = 0;
    c->auth_mechs = 0;
    c->size_max = 0;
    put_line(c, "EHLO ", c->name);
    c->st = C_EHLO;
}

static void parse_caps(mailk_cli_t* c)
{
    size_t pos = 0, i;
    mailk_ehlo_ext_t e;
    const char* t = c->r.text.p ? c->r.text.p : "";
    while (mailk_ehlo_next(t, c->r.text.n, &pos, &e) == 1) {
        for (i = 0; i < sizeof captab / sizeof captab[0]; i++)
            if (ieqn(e.kw, e.kwn, captab[i].kw))
                c->caps |= captab[i].bit;
        if (ieqn(e.kw, e.kwn, "SIZE")) {
            uint64_t v = 0;
            size_t k;
            for (k = 0; k < e.paramsn && e.params[k] >= '0' && e.params[k] <= '9' && v < (1ull << 50); k++)
                v = v * 10 + (uint64_t)(e.params[k] - '0');
            c->size_max = v;
        }
        if (ieqn(e.kw, e.kwn, "AUTH")) {
            size_t a = 0;
            while (a < e.paramsn) {
                size_t b = a;
                while (b < e.paramsn && e.params[b] != ' ')
                    b++;
                for (i = 0; i < sizeof mechtab / sizeof mechtab[0]; i++)
                    if (ieqn(e.params + a, b - a, mechtab[i].name))
                        c->auth_mechs |= mechtab[i].bit;
                a = b + 1;
            }
        }
    }
}

static int ready(mailk_cli_t* c)
{
    c->st = C_READY;
    c->ev |= MAILK_EV_READY;
    wipez(&c->pass);
    wipez(&c->token);
    return MAILK_OK;
}

static int auth_start(mailk_cli_t* c)
{
    static const unsigned order_pw[] = { MAILK_MECH_SCRAM_SHA256, MAILK_MECH_SCRAM_SHA512, MAILK_MECH_SCRAM_SHA1,
        MAILK_MECH_PLAIN, MAILK_MECH_LOGIN, MAILK_MECH_CRAM_MD5 };
    static const unsigned order_tok[] = { MAILK_MECH_OAUTHBEARER, MAILK_MECH_XOAUTH2 };
    const unsigned* ord = c->token ? order_tok : order_pw;
    size_t n = c->token ? 2 : 6, i;
    unsigned avail = c->auth_mechs & c->mechs, m = 0;
    dyn_sb_t t;
    int rc = MAILK_OK;
    if (!(c->caps & MAILK_CAP_AUTH))
        return fail(c, "SMTP: the server does not offer AUTH");
    for (i = 0; i < n && !m; i++)
        if (avail & ord[i])
            m = ord[i];
    if (!m)
        return fail(c, "SMTP: no common AUTH mechanism");
    if (!c->tls_on && !c->insecure_auth && (m & (MAILK_MECH_PLAIN | MAILK_MECH_LOGIN | MAILK_MECH_XOAUTH2 | MAILK_MECH_OAUTHBEARER)))
        return fail(c, "SMTP: refusing cleartext AUTH without TLS (set insecureAuth to allow)");
    c->mech = (int)m;
    c->sub = 0;
    c->st = C_AUTH;
    dyn_sb_init(&t);
    switch (m) {
    case MAILK_MECH_PLAIN:
        rc = mailk_plain_build(&t, c->authzid, c->user, c->pass);
        if (rc == MAILK_OK)
            rc = put_b64(c, "AUTH PLAIN ", (const uint8_t*)t.p, t.n);
        break;
    case MAILK_MECH_LOGIN:
        put_line(c, "AUTH LOGIN", NULL);
        break;
    case MAILK_MECH_CRAM_MD5:
        put_line(c, "AUTH CRAM-MD5", NULL);
        break;
    case MAILK_MECH_XOAUTH2:
        rc = mailk_xoauth2_build(&t, c->user, c->token);
        if (rc == MAILK_OK)
            rc = put_b64(c, "AUTH XOAUTH2 ", (const uint8_t*)t.p, t.n);
        break;
    case MAILK_MECH_OAUTHBEARER:
        rc = mailk_oauthbearer_build(&t, c->user, NULL, 0, c->token);
        if (rc == MAILK_OK)
            rc = put_b64(c, "AUTH OAUTHBEARER ", (const uint8_t*)t.p, t.n);
        break;
    default:
        mailk_scram_free(&c->scram);
        mailk_scram_init(&c->scram, m == MAILK_MECH_SCRAM_SHA1 ? MAILK_SCRAM_SHA1 : m == MAILK_MECH_SCRAM_SHA512 ? MAILK_SCRAM_SHA512 : MAILK_SCRAM_SHA256);
        if (c->scram_max_iters)
            c->scram.max_iters = c->scram_max_iters;
        rc = mailk_scram_client_first(&c->scram, c->user, c->authzid, c->pass, NULL, &t);
        if (rc == MAILK_OK)
            rc = put_b64(c, m == MAILK_MECH_SCRAM_SHA1 ? "AUTH SCRAM-SHA-1 " : m == MAILK_MECH_SCRAM_SHA512 ? "AUTH SCRAM-SHA-512 " : "AUTH SCRAM-SHA-256 ",
                (const uint8_t*)t.p, t.n);
        break;
    }
    dyn_sb_free(&t);
    if (rc != MAILK_OK)
        return fail(c, "SMTP: cannot build the AUTH request");
    return MAILK_OK;
}

static int after_ehlo(mailk_cli_t* c)
{
    if (!c->tls_on && (c->tls_mode == MAILK_TLS_STARTTLS || c->tls_mode == MAILK_TLS_OPPORTUNISTIC)) {
        if (c->caps & MAILK_CAP_STARTTLS) {
            put_line(c, "STARTTLS", NULL);
            c->st = C_STLS;
            return MAILK_OK;
        }
        if (c->tls_mode == MAILK_TLS_STARTTLS)
            return fail(c, "SMTP: the server does not offer STARTTLS");
    }
    if (c->user)
        return auth_start(c);
    return ready(c);
}

static int on_auth(mailk_cli_t* c)
{
    dyn_sb_t ch, t;
    int rc = MAILK_OK;
    if (c->r.code == 235) {
        if ((c->mech & (MAILK_MECH_SCRAM_SHA1 | MAILK_MECH_SCRAM_SHA256 | MAILK_MECH_SCRAM_SHA512)) && c->sub != 2)
            return fail(c, "SMTP: the server accepted SCRAM without proving its signature");
        return ready(c);
    }
    if (c->r.code != 334)
        return fail(c, "SMTP: authentication failed");
    dyn_sb_init(&ch);
    dyn_sb_init(&t);
    if (c->r.text.n && mailk_b64_decode(c->r.text.p, c->r.text.n, &ch) != MAILK_OK) {
        dyn_sb_free(&ch);
        put_line(c, "*", NULL);
        return fail(c, "SMTP: undecodable AUTH challenge");
    }
    switch (c->mech) {
    case MAILK_MECH_LOGIN: {
        int which = mailk_login_prompt((const uint8_t*)ch.p, ch.n);
        if (!which)
            which = c->sub + 1;
        if (c->sub++ >= 2)
            rc = MAILK_EBAD;
        else if (which == 1)
            rc = put_b64(c, "", (const uint8_t*)c->user, strlen(c->user));
        else
            rc = put_b64(c, "", (const uint8_t*)c->pass, strlen(c->pass));
        break;
    }
    case MAILK_MECH_CRAM_MD5:
        if (c->sub++ || !ch.n)
            rc = MAILK_EBAD;
        else if ((rc = mailk_cram_md5(&t, c->user, (const uint8_t*)c->pass, strlen(c->pass), (const uint8_t*)ch.p, ch.n)) == MAILK_OK)
            rc = put_b64(c, "", (const uint8_t*)t.p, t.n);
        break;
    case MAILK_MECH_XOAUTH2:
    case MAILK_MECH_OAUTHBEARER:
        if (c->sub++)
            rc = MAILK_EBAD;
        else if (c->mech == MAILK_MECH_XOAUTH2)
            put_line(c, "", NULL);
        else
            put_line(c, "AQ==", NULL);
        break;
    case MAILK_MECH_PLAIN:
        rc = MAILK_EBAD;
        break;
    default:
        if (c->sub == 0) {
            rc = mailk_scram_client_final(&c->scram, (const uint8_t*)ch.p, ch.n, &t);
            if (rc == MAILK_OK)
                rc = put_b64(c, "", (const uint8_t*)t.p, t.n);
            c->sub = 1;
        } else if (c->sub == 1) {
            rc = mailk_scram_client_verify(&c->scram, (const uint8_t*)ch.p, ch.n);
            if (rc == MAILK_OK)
                put_line(c, "", NULL);
            c->sub = 2;
        } else {
            rc = MAILK_EBAD;
        }
        break;
    }
    dyn_sb_free(&ch);
    dyn_sb_free(&t);
    if (rc != MAILK_OK) {
        put_line(c, "*", NULL);
        return fail(c, rc == MAILK_EAUTH ? "SMTP: the server failed SCRAM mutual authentication" : "SMTP: AUTH exchange failed");
    }
    return c->out.oom ? fail(c, "SMTP: out of memory") : MAILK_OK;
}

static void next_line(mailk_cli_t* c)
{
    const char* s = c->cmd.p + c->cpos;
    const char* e = memchr(s, '\n', c->cmd.n - c->cpos);
    size_t l = e ? (size_t)(e - s) + 1 : c->cmd.n - c->cpos;
    dyn_sb_put(&c->out, s, l);
    c->cpos += l;
}

static int tx_end(mailk_cli_t* c)
{
    if (!c->sub) {
        c->failed = 1;
        put_line(c, "RSET", NULL);
        return qpush(c, K_RSETX);
    }
    c->st = C_READY;
    c->ev |= MAILK_EV_DONE;
    return MAILK_OK;
}

static int on_tx(mailk_cli_t* c)
{
    int k = qpop(c), pipe = (c->caps & MAILK_CAP_PIPELINING) != 0, two = c->r.code / 100 == 2;
    if (!k)
        return fail(c, "SMTP: unsolicited reply");
    switch (k) {
    case K_MAIL:
        save(c, &c->mail);
        if (!two)
            c->failed = 1;
        else if (!pipe) {
            next_line(c);
            if (qpush(c, K_RCPT))
                return fail(c, "SMTP: out of memory");
        }
        break;
    case K_RCPT:
        if (c->ri < c->nrcpt)
            save(c, &c->rcpt[c->ri++]);
        if (two)
            c->ok++;
        if (!pipe) {
            if (c->ri < c->nrcpt) {
                next_line(c);
                if (qpush(c, K_RCPT))
                    return fail(c, "SMTP: out of memory");
            } else if (c->ok) {
                next_line(c);
                if (c->bdat)
                    dyn_sb_put(&c->out, c->body.p, c->body.n);
                if (qpush(c, c->bdat ? K_BDAT : K_DATA))
                    return fail(c, "SMTP: out of memory");
            } else {
                c->failed = 1;
            }
        }
        break;
    case K_DATA:
        if (c->r.code == 354) {
            if (c->failed || !c->ok) {
                c->failed = 1;
                dyn_sb_put(&c->out, ".\r\n", 3);
                if (qpush(c, K_DOTX))
                    return fail(c, "SMTP: out of memory");
            } else {
                dyn_sb_put(&c->out, c->body.p, c->body.n);
                if (qpush(c, K_DOT))
                    return fail(c, "SMTP: out of memory");
            }
        } else {
            save(c, &c->final);
            c->failed = 1;
        }
        break;
    case K_DOT:
        save(c, &c->final);
        c->sub = 1;
        if (!two)
            c->failed = 1;
        break;
    case K_BDAT:
        save(c, &c->final);
        c->sub = two;
        if (!two)
            c->failed = 1;
        break;
    case K_DOTX:
        c->sub = 1;
        break;
    default:
        break;
    }
    if (c->out.oom || c->texts.oom)
        return fail(c, "SMTP: out of memory");
    if (!c->qn)
        return k == K_RSETX ? (c->st = C_READY, c->ev |= MAILK_EV_DONE, MAILK_OK) : tx_end(c);
    return MAILK_OK;
}

static int on_reply(mailk_cli_t* c)
{
    int code = c->r.code;
    switch (c->st) {
    case C_GREET:
        if (code != 220)
            return fail(c, "SMTP: greeting refused");
        send_ehlo(c);
        return MAILK_OK;
    case C_EHLO:
        if (code == 250) {
            parse_caps(c);
            return after_ehlo(c);
        }
        if (code >= 500 && !c->tls_on && c->tls_mode == MAILK_TLS_NONE && !c->user && !c->helo) {
            c->helo = 1;
            put_line(c, "HELO ", c->name);
            c->st = C_HELO;
            return MAILK_OK;
        }
        return fail(c, "SMTP: EHLO refused");
    case C_HELO:
        if (code != 250)
            return fail(c, "SMTP: HELO refused");
        return ready(c);
    case C_STLS:
        if (code == 220) {
            c->st = C_TLSWAIT;
            c->ev |= MAILK_EV_TLS;
            return MAILK_OK;
        }
        if (c->tls_mode == MAILK_TLS_STARTTLS)
            return fail(c, "SMTP: STARTTLS refused");
        return c->user ? auth_start(c) : ready(c);
    case C_AUTH:
        return on_auth(c);
    case C_TX:
        return on_tx(c);
    case C_RSETU:
        if (code != 250)
            return fail(c, "SMTP: RSET refused");
        return ready(c);
    case C_QUIT:
        c->st = C_CLOSED;
        c->ev |= MAILK_EV_CLOSED;
        return MAILK_OK;
    default:
        return fail(c, "SMTP: unsolicited reply");
    }
}

int mailk_cli_feed(mailk_cli_t* c, const uint8_t* p, size_t n)
{
    size_t off = 0;
    if (c->st == C_DEAD || c->st == C_CLOSED)
        return MAILK_EBAD;
    if (c->st == C_TLSWAIT && n)
        return fail(c, "SMTP: data after the STARTTLS 220 reply (possible command injection)");
    while (off < n) {
        size_t u;
        int rc = mailk_reply_feed(&c->r, p + off, n - off, &u);
        off += u;
        if (rc == MAILK_NEED)
            break;
        if (rc != MAILK_OK)
            return fail(c, rc == MAILK_EBARE ? "SMTP: bare CR or LF in a reply" : rc == MAILK_ELONG ? "SMTP: reply too long" : "SMTP: malformed reply");
        save(c, &c->last);
        if (c->r.code == 421 && c->st != C_QUIT)
            rc = fail(c, "SMTP: the server is closing the connection");
        else
            rc = on_reply(c);
        mailk_reply_reset(&c->r);
        if (rc != MAILK_OK)
            return rc;
        if (c->st == C_TLSWAIT && off < n)
            return fail(c, "SMTP: data after the STARTTLS 220 reply (possible command injection)");
        if (c->st == C_DEAD || c->st == C_CLOSED)
            return MAILK_OK;
    }
    if (c->texts.oom || c->out.oom)
        return fail(c, "SMTP: out of memory");
    return MAILK_OK;
}

int mailk_cli_tls_done(mailk_cli_t* c)
{
    if (c->st != C_TLSWAIT)
        return MAILK_EBAD;
    c->tls_on = 1;
    send_ehlo(c);
    return MAILK_OK;
}

int mailk_cli_quit(mailk_cli_t* c)
{
    if (c->st != C_READY)
        return MAILK_EBAD;
    put_line(c, "QUIT", NULL);
    c->st = C_QUIT;
    return c->out.oom ? MAILK_ENOMEM : MAILK_OK;
}

int mailk_cli_rset(mailk_cli_t* c)
{
    if (c->st != C_READY)
        return MAILK_EBAD;
    put_line(c, "RSET", NULL);
    c->st = C_RSETU;
    return c->out.oom ? MAILK_ENOMEM : MAILK_OK;
}

static int non_ascii(const char* s)
{
    for (; *s; s++)
        if ((uint8_t)*s >= 0x80)
            return 1;
    return 0;
}

static int notify_ok(const char* s)
{
    const char* w[] = { "SUCCESS", "FAILURE", "DELAY" };
    if (!strcmp(s, "NEVER"))
        return 1;
    while (*s) {
        size_t l = strcspn(s, ","), i, hit = 0;
        for (i = 0; i < 3; i++)
            hit |= ieqn(s, l, w[i]);
        if (!hit)
            return 0;
        s += l;
        if (*s == ',' && !*++s)
            return 0;
    }
    return 1;
}

static int build_cmds(mailk_cli_t* c, const mailk_msg_t* m, unsigned flags, const char* body_kw)
{
    char sz[40];
    const char* params[6];
    size_t np = 0, i;
    dyn_sb_t x;
    int rc;
    dyn_sb_init(&x);
    if (c->caps & MAILK_CAP_SIZE) {
        snprintf(sz, sizeof sz, "SIZE=%zu", c->body.n);
        params[np++] = sz;
    }
    if (body_kw)
        params[np++] = body_kw;
    if (flags & MAILK_F_UTF8)
        params[np++] = "SMTPUTF8";
    if (m->ret)
        params[np++] = !strcmp(m->ret, "HDRS") ? "RET=HDRS" : "RET=FULL";
    if (m->envid) {
        dyn_sb_puts(&x, "ENVID=");
        mailk_xtext_encode(&x, (const uint8_t*)m->envid, strlen(m->envid));
        dyn_sb_putc(&x, 0);
        if (x.oom)
            return MAILK_ENOMEM;
        params[np++] = x.p;
    }
    rc = mailk_cmd_path(&c->cmd, 0, m->from, strlen(m->from), flags, params, np);
    dyn_sb_free(&x);
    if (rc != MAILK_OK)
        return local_err(c, "SMTP: invalid sender address");
    for (i = 0; i < m->nto; i++) {
        char nb[64];
        const char* rp[2];
        size_t rn = 0;
        dyn_sb_init(&x);
        if (m->notify) {
            snprintf(nb, sizeof nb, "NOTIFY=%s", m->notify);
            rp[rn++] = nb;
        }
        if (m->orcpt && !non_ascii(m->to[i])) {
            dyn_sb_puts(&x, "ORCPT=rfc822;");
            mailk_xtext_encode(&x, (const uint8_t*)m->to[i], strlen(m->to[i]));
            dyn_sb_putc(&x, 0);
            rp[rn++] = x.p;
        }
        rc = x.oom ? MAILK_ENOMEM : mailk_cmd_path(&c->cmd, 1, m->to[i], strlen(m->to[i]), flags, rp, rn);
        dyn_sb_free(&x);
        if (rc != MAILK_OK)
            return local_err(c, "SMTP: invalid recipient address");
    }
    if (c->bdat) {
        snprintf(sz, sizeof sz, "BDAT %zu LAST\r\n", c->body.n);
        dyn_sb_puts(&c->cmd, sz);
    } else {
        dyn_sb_puts(&c->cmd, "DATA\r\n");
    }
    return c->cmd.oom ? MAILK_ENOMEM : MAILK_OK;
}

int mailk_cli_send(mailk_cli_t* c, const mailk_msg_t* m)
{
    mailk_stuff_t s;
    unsigned flags = 0;
    size_t i;
    int utf8 = 0, rc, binary;
    const char* body_kw = NULL;
    if (c->st != C_READY)
        return local_err(c, "SMTP: the session is not ready");
    if (!m->from || !m->nto || m->nto > MAILK_RCPT_MAX)
        return local_err(c, "SMTP: a message needs a sender and 1..1000 recipients");
    if (m->ret && strcmp(m->ret, "FULL") && strcmp(m->ret, "HDRS"))
        return local_err(c, "SMTP: dsn.ret must be FULL or HDRS");
    if (m->notify && (strlen(m->notify) > 40 || !notify_ok(m->notify)))
        return local_err(c, "SMTP: dsn.notify must be NEVER or a list of SUCCESS, FAILURE, DELAY");
    if ((m->ret || m->envid || m->notify || m->orcpt) && !(c->caps & MAILK_CAP_DSN))
        return local_err(c, "SMTP: DSN options need a server that offers DSN");
    if (m->envid && (strlen(m->envid) > 100 || non_ascii(m->envid)))
        return local_err(c, "SMTP: dsn.envid must be ASCII, at most 100 octets");
    utf8 = non_ascii(m->from);
    for (i = 0; i < m->nto; i++)
        utf8 |= m->to[i] ? non_ascii(m->to[i]) : 0;
    if (utf8 && !(c->caps & MAILK_CAP_SMTPUTF8))
        return local_err(c, "SMTP: a non-ASCII address needs a server that offers SMTPUTF8");
    if (utf8)
        flags |= MAILK_F_UTF8;
    c->texts.n = 0;
    c->body.n = 0;
    c->cmd.n = 0;
    c->cpos = 0;
    memset(&c->mail, 0, sizeof c->mail);
    memset(&c->final, 0, sizeof c->final);
    c->failed = c->ok = c->sub = 0;
    c->ri = 0;
    c->bdat = m->bdat && (c->caps & MAILK_CAP_CHUNKING);
    mailk_stuff_init(&s, 0);
    rc = mailk_stuff_feed(&s, &c->body, m->body, m->bodyn);
    if (rc == MAILK_OK)
        rc = mailk_stuff_end(&s, &c->body);
    if (rc != MAILK_OK || c->body.oom)
        return local_err(c, "SMTP: out of memory");
    binary = c->bdat && (c->caps & MAILK_CAP_BINARYMIME)
        && (s.hasnul || s.fixed_bare || s.maxcol > MAILK_TEXT_LINE_MAX || (s.has8bit && !(c->caps & MAILK_CAP_8BITMIME)));
    if (binary) {
        c->body.n = 0;
        dyn_sb_put(&c->body, (const char*)m->body, m->bodyn);
        if (c->body.oom)
            return local_err(c, "SMTP: out of memory");
    } else if (c->bdat) {
        mailk_unstuff_t u;
        dyn_sb_t raw;
        size_t used;
        dyn_sb_init(&raw);
        mailk_unstuff_init(&u, 0, 0);
        rc = mailk_unstuff_feed(&u, &raw, (const uint8_t*)c->body.p, c->body.n, &used);
        dyn_sb_free(&c->body);
        c->body = raw;
        if (rc != MAILK_OK || raw.oom)
            return local_err(c, "SMTP: out of memory");
    }
    if (s.hasnul && !binary)
        return local_err(c, "SMTP: a NUL in the body needs BDAT with BINARYMIME");
    if (s.maxcol > MAILK_TEXT_LINE_MAX && !binary)
        return local_err(c, "SMTP: a body line is longer than 998 octets");
    if (binary)
        body_kw = "BODY=BINARYMIME";
    else if (s.has8bit) {
        if (c->caps & MAILK_CAP_8BITMIME)
            body_kw = "BODY=8BITMIME";
        else
            return local_err(c, "SMTP: an 8-bit body needs a server that offers 8BITMIME");
    }
    if (c->size_max && c->body.n > c->size_max)
        return local_err(c, "SMTP: the message exceeds the server SIZE limit");
    for (i = 0; i < m->nto; i++)
        if (!m->to[i])
            return local_err(c, "SMTP: invalid recipient address");
    if ((rc = build_cmds(c, m, flags, body_kw)) != MAILK_OK)
        return rc == MAILK_ENOMEM ? local_err(c, "SMTP: out of memory") : rc;
    free(c->rcpt);
    c->rcpt = calloc(m->nto, sizeof *c->rcpt);
    if (!c->rcpt)
        return local_err(c, "SMTP: out of memory");
    c->nrcpt = m->nto;
    c->qh = c->qn = 0;
    c->st = C_TX;
    c->ev &= ~MAILK_EV_DONE;
    if (c->caps & MAILK_CAP_PIPELINING) {
        dyn_sb_put(&c->out, c->cmd.p, c->cmd.n);
        if (c->bdat)
            dyn_sb_put(&c->out, c->body.p, c->body.n);
        c->cpos = c->cmd.n;
        rc = qpush(c, K_MAIL);
        for (i = 0; i < m->nto && rc == MAILK_OK; i++)
            rc = qpush(c, K_RCPT);
        if (rc == MAILK_OK)
            rc = qpush(c, c->bdat ? K_BDAT : K_DATA);
    } else {
        next_line(c);
        rc = qpush(c, K_MAIL);
    }
    if (rc != MAILK_OK || c->out.oom)
        return fail(c, "SMTP: out of memory");
    return MAILK_OK;
}
