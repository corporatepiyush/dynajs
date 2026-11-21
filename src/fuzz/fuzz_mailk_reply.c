#include "core/dyn-mailk.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

static int no_rt;

static void die(const char* why)
{
    fprintf(stderr, "fuzz_mailk_reply oracle: %s\n", why);
    abort();
}

static size_t chunk_of(unsigned split, size_t at, size_t left)
{
    size_t c;
    if (!split)
        return left;
    c = 1 + (split * 31u + at * 7u) % (split & 15u ? split & 15u : 3u);
    return c < left ? c : left;
}

static void rec(dyn_sb_t* o, const mailk_reply_t* r)
{
    char h[64];
    snprintf(h, sizeof h, "%d|%u|%d.%d.%d|", r->code, r->nlines, r->esc_class, r->esc_subject, r->esc_detail);
    dyn_sb_puts(o, h);
    dyn_sb_put(o, r->text.p ? r->text.p : "", r->text.n);
    dyn_sb_putc(o, 0);
}

static void replies(const uint8_t* p, size_t n, unsigned split, dyn_sb_t* o, dyn_sb_t* wire)
{
    mailk_reply_t r;
    size_t at = 0, guard = 0;
    mailk_reply_init(&r);
    while (at < n && guard++ < 2 * n + 8) {
        size_t c = chunk_of(split, at, n - at), u = 0;
        int rc = mailk_reply_feed(&r, p + at, c, &u);
        if (u > c)
            die("reply used past chunk");
        at += u;
        if (rc == MAILK_OK) {
            rec(o, &r);
            if (wire) {
                size_t i, ls = 0, nl = 1;
                char cd[8];
                const char* t = r.text.p ? r.text.p : "";
                for (i = 0; i < r.text.n; i++)
                    nl += t[i] == '\n';
                if (nl != r.nlines && r.text.n)
                    no_rt = 1;
                else {
                    snprintf(cd, sizeof cd, "%03d", r.code);
                    if (!r.text.n)
                        for (i = 0; i < r.nlines; i++) {
                            dyn_sb_puts(wire, cd);
                            dyn_sb_puts(wire, i + 1 < r.nlines ? "-\r\n" : " \r\n");
                        }
                    else
                        for (i = 0; i <= r.text.n; i++)
                            if (i == r.text.n || t[i] == '\n') {
                                dyn_sb_puts(wire, cd);
                                dyn_sb_putc(wire, i == r.text.n ? ' ' : '-');
                                dyn_sb_put(wire, t + ls, i - ls);
                                dyn_sb_put(wire, "\r\n", 2);
                                ls = i + 1;
                            }
                }
            }
            mailk_reply_reset(&r);
        } else if (rc != MAILK_NEED) {
            char e[16];
            snprintf(e, sizeof e, "E%d", rc);
            dyn_sb_puts(o, e);
            break;
        } else if (u < c)
            die("reply NEED without consuming the chunk");
    }
    mailk_reply_free(&r);
}

static void check_esc(const char* s, size_t n)
{
    int c, sj, d, c2, sj2, d2;
    size_t u, u2;
    char b[48];
    if (mailk_esc_parse(s, n, &c, &sj, &d, &u) != MAILK_OK)
        return;
    if (u > n || (c != 2 && c != 4 && c != 5) || sj < 0 || sj > 999 || d < 0 || d > 999)
        die("esc out of range");
    snprintf(b, sizeof b, "%d.%d.%d", c, sj, d);
    if (mailk_esc_parse(b, strlen(b), &c2, &sj2, &d2, &u2) != MAILK_OK || c2 != c || sj2 != sj || d2 != d || u2 != strlen(b))
        die("esc reformat round trip");
}

static int params_of(const char* a, size_t n, size_t pos, unsigned fl, dyn_sb_t* rec_out, char** list, size_t* np)
{
    mailk_param_t pr;
    int rc;
    *np = 0;
    while ((rc = mailk_param_next(a, n, &pos, fl, &pr)) == 1) {
        if (pr.kw < a || pr.kw + pr.kwn > a + n || (pr.val && (pr.val < a || pr.val + pr.valn > a + n)))
            die("param outside the argument");
        dyn_sb_put(rec_out, pr.kw, pr.kwn);
        dyn_sb_putc(rec_out, '=');
        if (pr.val)
            dyn_sb_put(rec_out, pr.val, pr.valn);
        dyn_sb_putc(rec_out, 0);
        if (list && *np < 16) {
            size_t l = pr.kwn + (pr.val ? pr.valn + 1 : 0);
            char* q = malloc(l + 1);
            if (!q)
                return -1;
            memcpy(q, pr.kw, pr.kwn);
            if (pr.val) {
                q[pr.kwn] = '=';
                memcpy(q + pr.kwn + 1, pr.val, pr.valn);
            }
            q[l] = 0;
            list[(*np)++] = q;
        }
    }
    return rc;
}

static void check_mail(const char* a, size_t n, int rcpt, unsigned fl)
{
    mailk_path_t p, p2;
    mailk_cmd_t c;
    size_t po, po2, np = 0, np2, i, mb, mn;
    char* list[16];
    dyn_sb_t r1, r2, line;
    int rc;
    if (mailk_mail_parse(a, n, rcpt, fl, &p, &po) != MAILK_OK)
        return;
    if (po > n || p.lo + p.ln > n || (p.dn && p.dn_off + p.dn > n))
        die("path offsets outside the argument");
    if (p.null_path || p.postmaster || !p.dn || p.dn_off < p.lo)
        return;
    mb = p.lo;
    mn = p.dn_off + p.dn - p.lo;
    dyn_sb_init(&r1);
    dyn_sb_init(&r2);
    dyn_sb_init(&line);
    rc = params_of(a, n, po, fl, &r1, list, &np);
    if (rc == 0 && np < 16 && mailk_cmd_path(&line, rcpt, a + mb, mn, fl, (const char* const*)list, np) == MAILK_OK) {
        if (line.n < 2 || mailk_cmd_parse(line.p, line.n - 2, &c) != MAILK_OK || c.verb != (rcpt ? MAILK_VERB_RCPT : MAILK_VERB_MAIL))
            die("cmd_path output does not parse as the command");
        if (mailk_mail_parse(c.arg, c.argn, rcpt, fl, &p2, &po2) != MAILK_OK)
            die("cmd_path output path refused");
        if (p2.dn_off + p2.dn - p2.lo != mn || memcmp(c.arg + p2.lo, a + mb, mn))
            die("mailbox changed in the round trip");
        params_of(c.arg, c.argn, po2, fl, &r2, NULL, &np2);
        if (r1.n != r2.n || (r1.n && memcmp(r1.p, r2.p, r1.n)))
            die("params changed in the round trip");
    }
    for (i = 0; i < np; i++)
        free(list[i]);
    dyn_sb_free(&r1);
    dyn_sb_free(&r2);
    dyn_sb_free(&line);
}

static void check_line(const char* s, size_t n, unsigned fl)
{
    mailk_cmd_t c;
    if (mailk_cmd_parse(s, n, &c) == MAILK_OK) {
        if (c.verb < MAILK_VERB_UNKNOWN || c.verb > MAILK_VERB_AUTH || (c.argn && (c.arg < s || c.arg + c.argn > s + n)))
            die("cmd parse out of range");
        if (c.verb == MAILK_VERB_MAIL || c.verb == MAILK_VERB_RCPT)
            check_mail(c.arg, c.argn, c.verb == MAILK_VERB_RCPT, fl);
    }
    if (n > 4)
        check_esc(s + 4, n - 4);
}

static void check_xtext(const uint8_t* p, size_t n)
{
    dyn_sb_t a, b, c;
    dyn_sb_init(&a);
    dyn_sb_init(&b);
    dyn_sb_init(&c);
    if (mailk_xtext_encode(&a, p, n) != MAILK_OK || mailk_xtext_decode(&b, a.p ? a.p : "", a.n) != MAILK_OK || b.n != n || (n && memcmp(b.p, p, n)))
        die("xtext encode/decode identity");
    b.n = a.n = 0;
    if (mailk_xtext_decode(&a, (const char*)p, n) == MAILK_OK) {
        if (mailk_xtext_encode(&b, (const uint8_t*)a.p, a.n) != MAILK_OK || mailk_xtext_decode(&c, b.p ? b.p : "", b.n) != MAILK_OK || c.n != a.n
            || (a.n && memcmp(c.p, a.p, a.n)))
            die("xtext decode/encode/decode identity");
    }
    dyn_sb_free(&a);
    dyn_sb_free(&b);
    dyn_sb_free(&c);
}

static void check_ehlo(const char* t, size_t n)
{
    mailk_ehlo_ext_t e;
    size_t pos = 0, last = 0, k = 0;
    int rc;
    while ((rc = mailk_ehlo_next(t, n, &pos, &e)) != 0 && k++ < n + 2) {
        if (pos > n || (k > 1 && pos <= last))
            die("ehlo position did not advance");
        if (rc == 1 && (e.kw < t || e.kw + e.kwn > t + n || !mailk_keyword_ok(e.kw, e.kwn) || (e.paramsn && (e.params < t || e.params + e.paramsn > t + n))))
            die("ehlo extension out of range or bad keyword");
        last = pos;
    }
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    dyn_sb_t a, b, w, w2;
    unsigned fl, split;
    size_t i, ls;
    if (size < 2 || size > 65536)
        return 0;
    fl = data[0] & 7u;
    split = data[1];
    data += 2;
    size -= 2;
    dyn_sb_init(&a);
    dyn_sb_init(&b);
    dyn_sb_init(&w);
    dyn_sb_init(&w2);
    no_rt = 0;
    replies(data, size, 0, &a, &w);
    replies(data, size, split | 1u, &b, NULL);
    if (a.n != b.n || (a.n && memcmp(a.p, b.p, a.n)))
        die("reply stream differs between whole and split feeds");
    b.n = 0;
    replies((const uint8_t*)(w.p ? w.p : ""), w.n, split, &b, &w2);
    for (i = 0, ls = 0; i < a.n; i++)
        if (!a.p[i])
            ls = i + 1;
    if (!no_rt && (b.n != ls || (ls && memcmp(a.p, b.p, ls))))
        die("reply serialise/parse round trip");
    for (i = 0, ls = 0; i <= size; i++)
        if (i == size || (data[i] == '\n' && i && data[i - 1] == '\r')) {
            size_t e = i == size ? i : i - 1;
            check_line((const char*)data + ls, e - ls, fl);
            ls = i + 1;
        }
    for (i = 0, ls = 0; i < a.n && ls < 3; i++)
        ls += a.p[i] == '|';
    if (ls == 3) {
        const char* z = memchr(a.p + i, 0, a.n - i);
        check_ehlo(a.p + i, z ? (size_t)(z - (a.p + i)) : a.n - i);
    }
    check_ehlo((const char*)data, size);
    check_xtext(data, size < 2048 ? size : 2048);
    dyn_sb_free(&a);
    dyn_sb_free(&b);
    dyn_sb_free(&w);
    dyn_sb_free(&w2);
    return 0;
}
