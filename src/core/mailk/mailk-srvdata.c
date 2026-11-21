#include <stdio.h>
#include <string.h>
#include "mailk-srv.h"

static const struct {
    int code;
    const char *esc, *text;
} bdat_errs[] = {
    { 0, NULL, NULL }, { 502, "5.5.1", "Command not implemented" }, { 503, "5.5.1", "Need MAIL command" },
    { 554, "5.5.1", "No valid recipients" }
};

static int message_end(mailk_srv_t* s, int over)
{
    s->st = V_CMD;
    if (over) {
        mailk_srv_reply(s, 552, "5.3.4", "Message size exceeds fixed maximum message size");
        mailk_srv_reset_tx(s);
        return 0;
    }
    if (!mailk_srv_want(s, MAILK_PEND_DATA))
        mailk_srv_accept(s, MAILK_PEND_DATA, 0, NULL, NULL);
    return 0;
}

int mailk_srv_cmd_data(mailk_srv_t* s, size_t argn)
{
    if (!s->tx)
        return mailk_srv_bad(s, 503, "5.5.1", "Need MAIL command");
    if (argn)
        return mailk_srv_bad(s, 501, "5.5.4", "Syntax: DATA");
    if (s->body8 == 2 || s->chunking)
        return mailk_srv_bad(s, 503, "5.5.1", s->chunking ? "DATA not permitted after BDAT" : "BINARYMIME requires BDAT");
    if (!s->nrcpt)
        return mailk_srv_reply(s, 554, "5.5.1", "No valid recipients");
    s->body.n = 0;
    s->msg_bytes = 0;
    mailk_unstuff_init(&s->un, MAILK_F_STRICT, s->size_max);
    s->st = V_DATA;
    dyn_sb_puts(&s->out, "354 End data with <CR><LF>.<CR><LF>\r\n");
    return 0;
}

static int bdat_end(mailk_srv_t* s)
{
    char b[64];
    s->st = V_CMD;
    if (s->bdat_err) {
        int e = s->bdat_err;
        s->bdat_err = 0;
        return mailk_srv_bad(s, bdat_errs[e].code, bdat_errs[e].esc, bdat_errs[e].text);
    }
    if (s->bdat_last || s->over)
        return message_end(s, s->over);
    snprintf(b, sizeof b, "%llu octets received", (unsigned long long)s->msg_bytes);
    return mailk_srv_reply(s, 250, "2.0.0", b);
}

int mailk_srv_cmd_bdat(mailk_srv_t* s, const char* a, size_t n)
{
    size_t k = 0;
    unsigned long long z = 0;
    while (k < n && k < 20 && a[k] >= '0' && a[k] <= '9')
        z = z * 10 + (unsigned long long)(a[k++] - '0');
    if (!k || (k < n && !(a[k] == ' ' && mailk_srv_ieqn(a + k + 1, n - k - 1, "LAST")))) {
        mailk_srv_reply(s, 501, "5.5.4", "Syntax: BDAT chunk-size [LAST]");
        return mailk_srv_close(s);
    }
    s->bdat_last = k < n;
    s->bdat_left = z;
    s->bdat_err = !(s->caps & MAILK_CAP_CHUNKING) ? 1 : !s->tx ? 2 : !s->nrcpt ? 3 : 0;
    if (!s->bdat_err && !s->chunking) {
        s->body.n = 0;
        s->msg_bytes = 0;
        s->over = 0;
    }
    if (!s->bdat_err)
        s->chunking = 1;
    s->st = V_BDAT;
    return z ? 0 : bdat_end(s);
}

size_t mailk_srv_body_feed(mailk_srv_t* s, const uint8_t* p, size_t n)
{
    size_t u = 0, lim = n;
    int rc;
    if (s->body_hwm && s->body.n + 2 > s->body_hwm) {
        s->ev |= MAILK_SEV_BODY;
        return 0;
    }
    if (s->body_hwm && lim > s->body_hwm - s->body.n - 1)
        lim = s->body_hwm - s->body.n - 1;
    if (s->st == V_DATA) {
        rc = mailk_unstuff_feed(&s->un, &s->body, p, lim, &u);
        s->msg_bytes = s->un.bytes;
        if (rc == MAILK_OK)
            message_end(s, s->un.over);
        else if (rc == MAILK_EBARE || rc == MAILK_ENOMEM) {
            mailk_srv_reply(s, rc == MAILK_EBARE ? 554 : 451, rc == MAILK_EBARE ? "5.6.0" : "4.3.0", rc == MAILK_EBARE ? "Bare CR or LF in message data" : "Local error in processing");
            mailk_srv_close(s);
        }
    } else {
        u = (size_t)(s->bdat_left < n ? s->bdat_left : n);
        if (!s->bdat_err && !s->over && s->size_max && s->msg_bytes + u > s->size_max)
            s->over = 1;
        if (!s->bdat_err && !s->over) {
            u = u < lim ? u : lim;
            dyn_sb_put(&s->body, (const char*)p, u);
            s->msg_bytes += u;
        }
        s->bdat_left -= u;
        if (!s->bdat_left)
            bdat_end(s);
    }
    if (s->body_hwm && s->body.n + 2 > s->body_hwm && (s->st == V_DATA || s->st == V_BDAT))
        s->ev |= MAILK_SEV_BODY;
    return u;
}
