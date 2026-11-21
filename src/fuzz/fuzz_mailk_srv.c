#include "core/dyn-mailk.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

static void die(const char* why)
{
    fprintf(stderr, "fuzz_mailk_srv oracle: %s\n", why);
    abort();
}

typedef struct {
    dyn_sb_t out, body, acc;
    uint32_t msgs;
    unsigned ev;
    size_t at;
} run_t;

static void decide(mailk_srv_t* s, int x)
{
    switch (s->pend) {
    case MAILK_PEND_RCPT:
        if (s->rcpt.n && s->rcpt.p[0] == 'n')
            mailk_srv_decide(s, 550, "5.1.1", "no such user");
        else
            mailk_srv_decide(s, 0, NULL, NULL);
        break;
    case MAILK_PEND_DATA:
        if (x)
            mailk_srv_decide(s, 554, "5.7.1", "rejected");
        else
            mailk_srv_decide(s, 0, NULL, NULL);
        break;
    case MAILK_PEND_AUTH:
        if (s->secret.n == 2)
            mailk_srv_decide(s, 0, NULL, NULL);
        else
            mailk_srv_decide(s, 535, "5.7.8", "bad");
        break;
    case MAILK_PEND_MAIL:
        if (s->from.n && s->from.p[0] == 'q')
            mailk_srv_decide(s, 451, "4.3.0", "later");
        else
            mailk_srv_decide(s, 0, NULL, NULL);
        break;
    default:
        mailk_srv_decide(s, 0, NULL, NULL);
        break;
    }
}

static void aborted(mailk_srv_t* s, run_t* r)
{
    size_t i;
    for (i = 0; i + 4 <= s->out.n; i++)
        if ((!i || s->out.p[i - 1] == '\n') && !memcmp(s->out.p + i, "552 ", 4))
            r->acc.n = 0;
}

static void take(mailk_srv_t* s, run_t* r)
{
    dyn_sb_put(&r->out, s->out.p, s->out.n);
    s->out.n = 0;
}

static void srv_run(const uint8_t* p, size_t n, unsigned fl, unsigned split, run_t* r)
{
    mailk_srv_opts_t o;
    mailk_srv_t s;
    size_t guard = 0;
    memset(&o, 0, sizeof o);
    o.name = "mx.fuzz";
    o.caps = MAILK_CAP_PIPELINING | MAILK_CAP_8BITMIME | MAILK_CAP_SMTPUTF8 | MAILK_CAP_DSN | MAILK_CAP_SIZE | MAILK_CAP_ENHANCED | MAILK_CAP_AUTH;
    if (fl & 1)
        o.caps |= MAILK_CAP_CHUNKING | MAILK_CAP_BINARYMIME;
    o.hooks = (fl >> 1 & 31u) << 1;
    o.insecure_auth = 1;
    o.mechs = MAILK_MECH_PLAIN | MAILK_MECH_LOGIN | MAILK_MECH_XOAUTH2 | MAILK_MECH_OAUTHBEARER;
    o.body_hwm = (fl & 64) ? 8 + (fl >> 7) * 9 : 0;
    o.size_max = (fl & 128) ? 40 : 0;
    o.rcpt_max = 3;
    o.errors_max = 6;
    o.line_max = 512;
    memset(r, 0, sizeof *r);
    dyn_sb_init(&r->out);
    dyn_sb_init(&r->body);
    dyn_sb_init(&r->acc);
    if (mailk_srv_init(&s, &o) != MAILK_OK) {
        mailk_srv_free(&s);
        return;
    }
    take(&s, r);
    while (r->at < n && guard++ < 4 * n + 64) {
        size_t chunk = !split ? n - r->at : split & 64 ? 1 : 1 + (split + r->at) % (split & 31 ? split & 31 : 1), u = 0;
        if (chunk > n - r->at)
            chunk = n - r->at;
        mailk_srv_feed(&s, p + r->at, chunk, &u);
        r->at += u;
        aborted(&s, r);
        if (s.ev & MAILK_SEV_BODY) {
            dyn_sb_put(&r->acc, s.body.p, s.body.n);
            s.body.n = 0;
            s.ev &= ~MAILK_SEV_BODY;
        }
        if (s.pend) {
            int x = 0;
            if (s.pend == MAILK_PEND_DATA) {
                dyn_sb_put(&r->acc, s.body.p, s.body.n);
                dyn_sb_put(&r->body, r->acc.p, r->acc.n);
                dyn_sb_putc(&r->body, 0);
                x = r->acc.n && memchr(r->acc.p, 'X', r->acc.n);
                r->acc.n = 0;
            }
            decide(&s, x);
        }
        take(&s, r);
        if (s.ev & MAILK_SEV_CLOSE)
            break;
        if (s.ev & MAILK_SEV_TLS)
            die("TLS event without STARTTLS offered");
    }
    r->msgs = s.msgs;
    r->ev = s.ev & MAILK_SEV_CLOSE;
    mailk_srv_free(&s);
}

static void run_free(run_t* r)
{
    dyn_sb_free(&r->out);
    dyn_sb_free(&r->body);
    dyn_sb_free(&r->acc);
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    run_t a, b;
    size_t i;
    unsigned fl, split;
    if (size < 2 || size > 16384)
        return 0;
    fl = data[0];
    split = data[1] | 1u;
    data += 2;
    size -= 2;
    srv_run(data, size, fl, 0, &a);
    srv_run(data, size, fl, split, &b);
    if (a.out.n != b.out.n || (a.out.n && memcmp(a.out.p, b.out.p, a.out.n)) || a.body.n != b.body.n
        || (a.body.n && memcmp(a.body.p, b.body.p, a.body.n)) || a.msgs != b.msgs || a.ev != b.ev)
        die("session differs between whole and split feeds");
    for (i = 0; i < a.out.n; i++)
        if (a.out.p[i] == '\n' && (!i || a.out.p[i - 1] != '\r'))
            die("bare LF in server output");
    if (a.out.n && (a.out.n < 2 || a.out.p[a.out.n - 1] != '\n'))
        die("server output does not end in a full line");
    run_free(&a);
    run_free(&b);
    return 0;
}
