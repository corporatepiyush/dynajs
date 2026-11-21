#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mailk-srv.h"

#define SRV_CAPS (MAILK_CAP_PIPELINING | MAILK_CAP_8BITMIME | MAILK_CAP_SMTPUTF8 | MAILK_CAP_DSN | MAILK_CAP_STARTTLS | MAILK_CAP_SIZE | MAILK_CAP_ENHANCED | MAILK_CAP_AUTH | MAILK_CAP_CHUNKING | MAILK_CAP_BINARYMIME)

static const struct {
    const char* kw;
    int rcpt;
    unsigned cap;
} ptab[] = {
    { "SIZE", 0, MAILK_CAP_SIZE }, { "BODY", 0, MAILK_CAP_8BITMIME }, { "SMTPUTF8", 0, MAILK_CAP_SMTPUTF8 },
    { "RET", 0, MAILK_CAP_DSN }, { "ENVID", 0, MAILK_CAP_DSN }, { "AUTH", 0, MAILK_CAP_AUTH },
    { "NOTIFY", 1, MAILK_CAP_DSN }, { "ORCPT", 1, MAILK_CAP_DSN },
};

int mailk_srv_ieqn(const char* a, size_t n, const char* lit)
{
    size_t i;
    if (strlen(lit) != n)
        return 0;
    for (i = 0; i < n; i++)
        if ((a[i] >= 'a' && a[i] <= 'z' ? a[i] - 32 : a[i]) != lit[i])
            return 0;
    return 1;
}

void mailk_srv_wipe(dyn_sb_t* b)
{
    volatile char* v = b->p;
    size_t i;
    for (i = 0; v && i < b->cap; i++)
        v[i] = 0;
    b->n = 0;
}

int mailk_srv_reply(mailk_srv_t* s, int code, const char* esc, const char* text)
{
    char b[8];
    snprintf(b, sizeof b, "%d ", code);
    dyn_sb_puts(&s->out, b);
    if (esc && s->ehlo && (s->caps & MAILK_CAP_ENHANCED)) {
        dyn_sb_puts(&s->out, esc);
        dyn_sb_putc(&s->out, ' ');
    }
    dyn_sb_puts(&s->out, text);
    dyn_sb_put(&s->out, "\r\n", 2);
    return 0;
}

int mailk_srv_close(mailk_srv_t* s)
{
    s->st = V_CLOSED;
    s->ev |= MAILK_SEV_CLOSE;
    mailk_srv_wipe(&s->secret);
    return 0;
}

int mailk_srv_bad(mailk_srv_t* s, int code, const char* esc, const char* text)
{
    mailk_srv_reply(s, code, esc, text);
    if (s->errors_max && ++s->errors >= s->errors_max) {
        mailk_srv_reply(s, 421, "4.7.0", "Too many errors, closing connection");
        mailk_srv_close(s);
    }
    return 0;
}

int mailk_srv_reset_tx(mailk_srv_t* s)
{
    s->tx = s->chunking = s->over = s->bdat_err = 0;
    s->nrcpt = 0;
    s->msg_bytes = 0;
    s->body8 = s->utf8 = s->ret = 0;
    s->from.n = s->envid.n = s->rcpt.n = s->rcpts.n = s->body.n = 0;
    return 0;
}

int mailk_srv_want(mailk_srv_t* s, int p)
{
    if (!(s->hooks & MAILK_HOOK(p)))
        return 0;
    s->pend = p;
    s->ev |= MAILK_SEV_PENDING;
    return 1;
}

static void ehlo_reply(mailk_srv_t* s, const char* first)
{
    static const struct {
        unsigned cap;
        const char* line;
    } ext[] = {
        { MAILK_CAP_PIPELINING, "250-PIPELINING\r\n" }, { MAILK_CAP_8BITMIME, "250-8BITMIME\r\n" },
        { MAILK_CAP_SMTPUTF8, "250-SMTPUTF8\r\n" }, { MAILK_CAP_DSN, "250-DSN\r\n" },
        { MAILK_CAP_CHUNKING, "250-CHUNKING\r\n" }, { MAILK_CAP_BINARYMIME, "250-BINARYMIME\r\n" },
    };
    char b[48];
    size_t i;
    dyn_sb_puts(&s->out, "250-");
    dyn_sb_puts(&s->out, first ? first : s->name);
    dyn_sb_put(&s->out, "\r\n", 2);
    for (i = 0; i < sizeof ext / sizeof ext[0]; i++)
        if (s->caps & ext[i].cap)
            dyn_sb_puts(&s->out, ext[i].line);
    if (s->caps & MAILK_CAP_SIZE) {
        snprintf(b, sizeof b, "250-SIZE %llu\r\n", (unsigned long long)s->size_max);
        dyn_sb_puts(&s->out, b);
    }
    if ((s->caps & MAILK_CAP_STARTTLS) && !s->tls_on)
        dyn_sb_puts(&s->out, "250-STARTTLS\r\n");
    if ((s->caps & MAILK_CAP_AUTH) && (s->tls_on || s->insecure_auth))
        mailk_srv_mechs_line(s);
    dyn_sb_puts(&s->out, (s->caps & MAILK_CAP_ENHANCED) ? "250 ENHANCEDSTATUSCODES\r\n" : "250 HELP\r\n");
}

int mailk_srv_accept(mailk_srv_t* s, int p, int code, const char* esc, const char* text)
{
    switch (p) {
    case MAILK_PEND_HELO:
        s->helo = 1;
        s->ehlo = s->pend_ehlo;
        if (s->ehlo)
            ehlo_reply(s, text);
        else
            mailk_srv_reply(s, code ? code : 250, NULL, text ? text : s->name);
        break;
    case MAILK_PEND_MAIL:
        s->tx = 1;
        mailk_srv_reply(s, code ? code : 250, esc ? esc : "2.1.0", text ? text : "Ok");
        break;
    case MAILK_PEND_RCPT:
        dyn_sb_put(&s->rcpts, s->rcpt.p, s->rcpt.n);
        s->nrcpt++;
        mailk_srv_reply(s, code ? code : 250, esc ? esc : "2.1.5", text ? text : "Ok");
        break;
    case MAILK_PEND_AUTH:
        s->authed = 1;
        s->auth_fails = 0;
        mailk_srv_reply(s, code ? code : 235, esc ? esc : "2.7.0", text ? text : "Authentication successful");
        break;
    case MAILK_PEND_DATA:
        s->msgs++;
        mailk_srv_reply(s, code ? code : 250, esc ? esc : "2.0.0", text ? text : "Ok: queued");
        mailk_srv_reset_tx(s);
        break;
    }
    return 0;
}

static int text_ok(const char* t)
{
    size_t i;
    for (i = 0; t[i]; i++)
        if ((uint8_t)t[i] < 32 || t[i] == 127 || i >= 400)
            return 0;
    return 1;
}

int mailk_srv_decide(mailk_srv_t* s, int code, const char* esc, const char* text)
{
    int p = s->pend, cls, sub, det;
    size_t u;
    if (!p || s->st == V_CLOSED || (code && (code < 200 || code > 599)) || (text && !text_ok(text)))
        return MAILK_EBAD;
    if (esc && (!code || mailk_esc_parse(esc, strlen(esc), &cls, &sub, &det, &u) != MAILK_OK || u != strlen(esc) || cls != code / 100))
        return MAILK_EBAD;
    if ((code >= 400 && !text) || (p == MAILK_PEND_AUTH && code < 400 && (unsigned)s->mech >= MAILK_MECH_SCRAM_SHA1))
        return MAILK_EBAD;
    s->pend = MAILK_PEND_NONE;
    s->ev &= ~MAILK_SEV_PENDING;
    if (code < 400)
        mailk_srv_accept(s, p, code, esc, text);
    else if (p == MAILK_PEND_AUTH)
        mailk_srv_auth_reject(s, code, esc, text);
    else {
        mailk_srv_reply(s, code, esc, text);
        if (p == MAILK_PEND_MAIL || p == MAILK_PEND_DATA)
            mailk_srv_reset_tx(s);
    }
    mailk_srv_wipe(&s->secret);
    return s->out.oom ? MAILK_ENOMEM : MAILK_OK;
}

static int helo_ok(mailk_srv_t* s, const char* a, size_t n)
{
    dyn_sb_t t;
    mailk_path_t p;
    int ok;
    dyn_sb_init(&t);
    dyn_sb_put(&t, "x@", 2);
    dyn_sb_put(&t, a, n);
    ok = !t.oom && mailk_mailbox_parse(t.p, t.n, (s->caps & MAILK_CAP_SMTPUTF8) ? MAILK_F_UTF8 : 0, &p) == MAILK_OK;
    dyn_sb_free(&t);
    return ok;
}

static int has8(const char* p, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++)
        if ((uint8_t)p[i] >= 0x80)
            return 1;
    return 0;
}

static int xtext_ok(const char* v, size_t n, size_t max)
{
    dyn_sb_t t;
    int ok;
    dyn_sb_init(&t);
    ok = mailk_xtext_decode(&t, v, n) == MAILK_OK && t.n <= max;
    dyn_sb_free(&t);
    return ok;
}

static int notify_ok(const char* v, size_t n)
{
    size_t i = 0, e;
    unsigned seen = 0;
    if (mailk_srv_ieqn(v, n, "NEVER"))
        return 1;
    while (i < n) {
        unsigned bit;
        for (e = i; e < n && v[e] != ','; e++)
            ;
        bit = mailk_srv_ieqn(v + i, e - i, "SUCCESS") ? 1u : mailk_srv_ieqn(v + i, e - i, "FAILURE") ? 2u : mailk_srv_ieqn(v + i, e - i, "DELAY") ? 4u : 0u;
        if (!bit || (seen & bit) || (e < n && e + 1 == n))
            return 0;
        seen |= bit;
        i = e + 1;
    }
    return seen != 0;
}

static int orcpt_ok(const char* v, size_t n)
{
    const char* sc = memchr(v, ';', n);
    return sc && sc > v && mailk_keyword_ok(v, (size_t)(sc - v)) && sc + 1 < v + n && xtext_ok(sc + 1, n - (size_t)(sc + 1 - v), 500);
}

static int param_val(mailk_srv_t* s, int k, const char* v, size_t vn, int* utf8)
{
    switch (k) {
    case 0: {
        unsigned long long z = 0;
        size_t j;
        if (!v || !vn || vn > 20)
            return 0;
        for (j = 0; j < vn; j++) {
            if (v[j] < '0' || v[j] > '9')
                return 0;
            z = z > (ULLONG_MAX - 9) / 10 ? ULLONG_MAX : z * 10 + (unsigned)(v[j] - '0');
        }
        return s->size_max && z > s->size_max ? -1 : 1;
    }
    case 1:
        s->body8 = !v ? -1 : mailk_srv_ieqn(v, vn, "7BIT") ? 0 : mailk_srv_ieqn(v, vn, "8BITMIME") ? 1 : mailk_srv_ieqn(v, vn, "BINARYMIME") && (s->caps & MAILK_CAP_BINARYMIME) ? 2 : -1;
        return s->body8 >= 0;
    case 2:
        *utf8 = 1;
        return !v;
    case 3:
        s->ret = !v ? 0 : mailk_srv_ieqn(v, vn, "FULL") ? 1 : mailk_srv_ieqn(v, vn, "HDRS") ? 2 : 0;
        return s->ret != 0;
    case 4:
        s->envid.n = 0;
        dyn_sb_put(&s->envid, v, vn);
        return v && xtext_ok(v, vn, 100);
    case 5:
        return v && (mailk_srv_ieqn(v, vn, "<>") || xtext_ok(v, vn, 500));
    case 6:
        return v && notify_ok(v, vn);
    default:
        return v && orcpt_ok(v, vn);
    }
}

static int params(mailk_srv_t* s, const char* a, size_t n, size_t pos, int rcpt, unsigned flags, int* utf8)
{
    mailk_param_t pr;
    const char* val[8] = { 0 };
    size_t valn[8] = { 0 };
    unsigned seen = 0;
    int rc, k, ok;
    if (pos < n && !s->ehlo)
        return mailk_srv_bad(s, 555, "5.5.4", "Parameters require EHLO"), 0;
    while ((rc = mailk_param_next(a, n, &pos, flags, &pr)) == 1) {
        for (k = 0; k < 8 && !(ptab[k].rcpt == rcpt && (s->caps & ptab[k].cap) && mailk_srv_ieqn(pr.kw, pr.kwn, ptab[k].kw)); k++)
            ;
        if (k == 8)
            return mailk_srv_bad(s, 555, "5.5.4", "Unsupported parameter"), 0;
        if (seen & (1u << k))
            return mailk_srv_bad(s, 501, "5.5.4", "Duplicate parameter"), 0;
        seen |= 1u << k;
        val[k] = pr.val;
        valn[k] = pr.valn;
        ok = param_val(s, k, pr.val, pr.valn, utf8);
        if (ok < 0)
            return mailk_srv_reply(s, 552, "5.3.4", "Message size exceeds fixed maximum message size"), 0;
        if (!ok)
            return mailk_srv_bad(s, 501, "5.5.4", "Invalid parameter value"), 0;
    }
    if (rc < 0)
        return mailk_srv_bad(s, 501, "5.5.4", "Syntax error in parameters"), 0;
    if (rcpt) {
        dyn_sb_put(&s->rcpt, val[6], valn[6]);
        dyn_sb_putc(&s->rcpt, 0);
        dyn_sb_put(&s->rcpt, val[7], valn[7]);
        dyn_sb_putc(&s->rcpt, 0);
    }
    return 1;
}

static const char* mail_gate(mailk_srv_t* s, int rcpt, int* code, const char** esc)
{
    *code = 503;
    *esc = "5.5.1";
    if (rcpt)
        return s->tx ? NULL : "Need MAIL command";
    if (!s->helo)
        return "Send EHLO first";
    if (s->tx)
        return "Nested MAIL command";
    *code = 530;
    *esc = "5.7.0";
    if (s->require_tls && !s->tls_on)
        return "Must issue a STARTTLS command first";
    if (s->require_auth && !s->authed)
        return "Authentication required";
    return NULL;
}

static int cmd_mail(mailk_srv_t* s, const char* a, size_t n, int rcpt)
{
    mailk_path_t p;
    size_t po;
    int utf8 = 0, code;
    const char *esc, *why = mail_gate(s, rcpt, &code, &esc);
    unsigned fl = (s->caps & MAILK_CAP_SMTPUTF8) && s->ehlo ? MAILK_F_UTF8 : 0;
    if (why)
        return mailk_srv_bad(s, code, esc, why);
    if (!rcpt && s->msgs_max && s->msgs >= s->msgs_max) {
        mailk_srv_reply(s, 421, "4.7.0", "Too many messages in this session");
        return mailk_srv_close(s);
    }
    if (rcpt && s->rcpt_max && s->nrcpt >= s->rcpt_max)
        return mailk_srv_reply(s, 452, "4.5.3", "Too many recipients");
    if (rcpt && !s->utf8)
        fl = 0;
    if (mailk_mail_parse(a, n, rcpt, fl, &p, &po) != MAILK_OK)
        return mailk_srv_bad(s, 501, rcpt ? "5.1.3" : "5.1.7", rcpt ? "Bad recipient address syntax" : "Bad sender address syntax");
    if (rcpt) {
        s->rcpt.n = 0;
        dyn_sb_put(&s->rcpt, a + p.lo, p.postmaster ? p.ln : p.dn_off + p.dn - p.lo);
        dyn_sb_putc(&s->rcpt, 0);
    } else {
        s->from.n = s->envid.n = 0;
        s->body8 = s->ret = 0;
        if (!p.null_path)
            dyn_sb_put(&s->from, a + p.lo, p.dn_off + p.dn - p.lo);
    }
    if (!params(s, a, n, po, rcpt, fl, &utf8))
        return 0;
    if (!rcpt && !utf8 && has8(s->from.p, s->from.n))
        return mailk_srv_bad(s, 553, "5.6.7", "SMTPUTF8 required for this address");
    if (!rcpt)
        s->utf8 = utf8;
    if (!mailk_srv_want(s, rcpt ? MAILK_PEND_RCPT : MAILK_PEND_MAIL))
        mailk_srv_accept(s, rcpt ? MAILK_PEND_RCPT : MAILK_PEND_MAIL, 0, NULL, NULL);
    return 0;
}

static int cmd_starttls(mailk_srv_t* s, size_t argn)
{
    if (!(s->caps & MAILK_CAP_STARTTLS))
        return mailk_srv_bad(s, 502, "5.5.1", "Command not implemented");
    if (s->tls_on)
        return mailk_srv_bad(s, 503, "5.5.1", "TLS already active");
    if (argn)
        return mailk_srv_bad(s, 501, "5.5.4", "Syntax: STARTTLS");
    mailk_srv_reply(s, 220, "2.0.0", "Ready to start TLS");
    mailk_srv_reset_tx(s);
    s->st = V_TLS;
    s->ev |= MAILK_SEV_TLS;
    return 0;
}

static int command(mailk_srv_t* s, const char* l, size_t n)
{
    mailk_cmd_t c;
    if (mailk_cmd_parse(l, n, &c) != MAILK_OK || c.verb == MAILK_VERB_UNKNOWN)
        return mailk_srv_bad(s, 500, "5.5.2", "Syntax error, command unrecognized");
    switch (c.verb) {
    case MAILK_VERB_HELO:
    case MAILK_VERB_EHLO:
        if (!c.argn || !helo_ok(s, c.arg, c.argn))
            return mailk_srv_bad(s, 501, "5.5.4", "Syntax: EHLO hostname");
        mailk_srv_reset_tx(s);
        s->helo_arg.n = 0;
        dyn_sb_put(&s->helo_arg, c.arg, c.argn);
        s->pend_ehlo = c.verb == MAILK_VERB_EHLO;
        if (!mailk_srv_want(s, MAILK_PEND_HELO))
            mailk_srv_accept(s, MAILK_PEND_HELO, 0, NULL, NULL);
        return 0;
    case MAILK_VERB_MAIL:
    case MAILK_VERB_RCPT:
        return cmd_mail(s, c.arg, c.argn, c.verb == MAILK_VERB_RCPT);
    case MAILK_VERB_DATA:
        return mailk_srv_cmd_data(s, c.argn);
    case MAILK_VERB_BDAT:
        return mailk_srv_cmd_bdat(s, c.arg, c.argn);
    case MAILK_VERB_RSET:
        if (c.argn)
            return mailk_srv_bad(s, 501, "5.5.4", "Syntax: RSET");
        mailk_srv_reset_tx(s);
        return mailk_srv_reply(s, 250, "2.0.0", "Ok");
    case MAILK_VERB_NOOP:
        return mailk_srv_reply(s, 250, "2.0.0", "Ok");
    case MAILK_VERB_QUIT:
        mailk_srv_reply(s, 221, "2.0.0", "Bye");
        return mailk_srv_close(s);
    case MAILK_VERB_VRFY:
        return mailk_srv_reply(s, 252, "2.5.0", "Cannot VRFY user, but will accept message");
    case MAILK_VERB_HELP:
        return mailk_srv_reply(s, 214, "2.0.0", "See RFC 5321");
    case MAILK_VERB_STARTTLS:
        return cmd_starttls(s, c.argn);
    case MAILK_VERB_AUTH:
        return mailk_srv_cmd_auth(s, c.arg, c.argn);
    default:
        return mailk_srv_bad(s, 502, "5.5.1", "Command not implemented");
    }
    return 0;
}

static int line_error(mailk_srv_t* s, int rc, const uint8_t* p, size_t* i)
{
    mailk_line_reset(&s->line);
    if (s->st == V_AUTH || s->st == V_OERR)
        s->st = V_CMD;
    if (p[*i] == '\n')
        ++*i;
    else
        s->skip = 1;
    mailk_srv_bad(s, 500, rc == MAILK_ELONG ? "5.5.6" : "5.5.2", rc == MAILK_ELONG ? "Line too long" : "Bare CR or LF in command");
    return 0;
}

int mailk_srv_feed(mailk_srv_t* s, const uint8_t* p, size_t n, size_t* used)
{
    size_t i = 0, u;
    int rc;
    while (i < n && !s->pend && !(s->ev & MAILK_SEV_BODY) && s->st != V_CLOSED) {
        if (s->st == V_TLS) {
            i = n;
        } else if (s->st == V_DATA || s->st == V_BDAT) {
            i += mailk_srv_body_feed(s, p + i, n - i);
        } else if (s->skip) {
            const uint8_t* e = memchr(p + i, '\n', n - i);
            i = e ? (size_t)(e - p) + 1 : n;
            s->skip = !e;
        } else {
            rc = mailk_line_feed(&s->line, p + i, n - i, &u);
            i += u;
            if (rc == MAILK_OK) {
                const char* l = s->line.buf.p ? s->line.buf.p : "";
                if (s->st == V_AUTH || s->st == V_OERR)
                    mailk_srv_auth_line(s, l, s->line.buf.n);
                else
                    command(s, l, s->line.buf.n);
                mailk_line_reset(&s->line);
            } else if (rc == MAILK_ELONG || rc == MAILK_EBARE) {
                line_error(s, rc, p, &i);
            } else if (rc == MAILK_ENOMEM) {
                mailk_srv_reply(s, 421, "4.3.0", "Local error in processing");
                mailk_srv_close(s);
            }
        }
    }
    *used = i;
    return s->out.oom ? MAILK_ENOMEM : MAILK_OK;
}

int mailk_srv_tls_done(mailk_srv_t* s)
{
    if (s->st != V_TLS)
        return MAILK_EBAD;
    s->st = V_CMD;
    s->ev &= ~MAILK_SEV_TLS;
    s->tls_on = 1;
    s->helo = s->ehlo = s->authed = 0;
    s->helo_arg.n = 0;
    mailk_srv_reset_tx(s);
    mailk_line_reset(&s->line);
    return MAILK_OK;
}

uint32_t mailk_srv_timeout_ms(const mailk_srv_t* s)
{
    return s->st == V_DATA || s->st == V_BDAT ? s->data_ms : s->cmd_ms;
}

int mailk_srv_timeout(mailk_srv_t* s)
{
    if (s->st == V_CLOSED)
        return MAILK_EBAD;
    s->pend = MAILK_PEND_NONE;
    s->ev &= ~MAILK_SEV_PENDING;
    mailk_srv_reply(s, 421, "4.4.2", "Timeout exceeded, closing connection");
    mailk_srv_close(s);
    return MAILK_OK;
}

#define SRV_SBS(s) { &s->out, &s->helo_arg, &s->from, &s->envid, &s->rcpt, &s->rcpts, &s->body, &s->user, &s->authz, &s->secret, &s->rej }

int mailk_srv_init(mailk_srv_t* s, const mailk_srv_opts_t* o)
{
    size_t n;
    dyn_sb_t* sbs[] = SRV_SBS(s);
    memset(s, 0, sizeof *s);
    for (n = 0; n < sizeof sbs / sizeof sbs[0]; n++)
        dyn_sb_init(sbs[n]);
    mailk_line_init(&s->line, (o->line_max ? o->line_max : MAILK_SRV_LINE_MAX) - 2);
    mailk_scram_init(&s->scram, MAILK_SCRAM_SHA256);
    if (!o->name || !mailk_domain_ok(o->name, strlen(o->name), 0) || (o->line_max && o->line_max < 512))
        return MAILK_EBAD;
    s->mechs = (o->mechs ? o->mechs : MAILK_SRV_MECHS) & MAILK_SRV_MECHS;
    s->caps = o->caps & SRV_CAPS;
    if (!(s->caps & MAILK_CAP_CHUNKING))
        s->caps &= ~MAILK_CAP_BINARYMIME;
    s->hooks = o->hooks;
    if (!(s->hooks & MAILK_HOOK(MAILK_PEND_AUTH)) || !s->mechs)
        s->caps &= ~MAILK_CAP_AUTH;
    if ((o->require_auth && !(s->caps & MAILK_CAP_AUTH)) || (o->require_tls && !o->tls_on && !(s->caps & MAILK_CAP_STARTTLS)))
        return MAILK_EBAD;
    n = strlen(o->name);
    s->name = malloc(n + 1);
    if (!s->name)
        return MAILK_ENOMEM;
    memcpy(s->name, o->name, n + 1);
    s->tls_on = o->tls_on;
    s->require_tls = o->require_tls;
    s->require_auth = o->require_auth;
    s->insecure_auth = o->insecure_auth;
    s->size_max = o->size_max ? o->size_max : 10485760u;
    s->rcpt_max = o->rcpt_max ? o->rcpt_max : 100;
    s->errors_max = o->errors_max ? o->errors_max : 10;
    s->msgs_max = o->msgs_max ? o->msgs_max : 100;
    s->auth_max = o->auth_max ? o->auth_max : 3;
    s->cmd_ms = o->cmd_ms ? o->cmd_ms : 300000;
    s->data_ms = o->data_ms ? o->data_ms : 300000;
    s->body_hwm = o->body_hwm && o->body_hwm < 16 ? 16 : o->body_hwm;
    dyn_sb_puts(&s->out, "220 ");
    dyn_sb_puts(&s->out, s->name);
    dyn_sb_puts(&s->out, " ESMTP\r\n");
    return s->out.oom ? MAILK_ENOMEM : MAILK_OK;
}

void mailk_srv_free(mailk_srv_t* s)
{
    dyn_sb_t* sbs[] = SRV_SBS(s);
    size_t i;
    mailk_srv_wipe(&s->secret);
    for (i = 0; i < sizeof sbs / sizeof sbs[0]; i++)
        dyn_sb_free(sbs[i]);
    mailk_line_free(&s->line);
    mailk_scram_free(&s->scram);
    free(s->name);
    s->name = NULL;
}
