#include "core/dyn-mailk.h"

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

static void die(const char* why)
{
    fprintf(stderr, "fuzz_mailk oracle: %s\n", why);
    abort();
}

static size_t chunk_of(unsigned split, size_t at, size_t left)
{
    size_t c;
    if (!split)
        return left;
    c = 1 + (split * 13u + at * 5u) % (split & 31u ? split & 31u : 7u);
    return c < left ? c : left;
}

static void data_model(dyn_sb_t* o, const uint8_t* p, size_t n)
{
    size_t i;
    int sol = 1;
    for (i = 0; i < n; i++) {
        if (p[i] == '\r' || p[i] == '\n') {
            if (p[i] == '\r' && i + 1 < n && p[i + 1] == '\n')
                i++;
            dyn_sb_put(o, "\r\n", 2);
            sol = 1;
        } else {
            dyn_sb_putc(o, (char)p[i]);
            sol = 0;
        }
    }
    if (!sol)
        dyn_sb_put(o, "\r\n", 2);
}

static void check_data(const uint8_t* p, size_t n, unsigned flags, unsigned split)
{
    mailk_stuff_t s, s2;
    mailk_unstuff_t u, u2;
    dyn_sb_t a, b, m, c;
    size_t at, used, i, bare = 0;
    int rc, rc2;
    dyn_sb_init(&a);
    dyn_sb_init(&b);
    dyn_sb_init(&m);
    dyn_sb_init(&c);
    for (i = 0; i < n; i++)
        bare += (p[i] == '\r' && (i + 1 >= n || p[i + 1] != '\n')) || (p[i] == '\n' && (!i || p[i - 1] != '\r'));
    mailk_stuff_init(&s, flags & MAILK_F_STRICT);
    rc = mailk_stuff_feed(&s, &a, p, n);
    if (rc == MAILK_OK)
        rc = mailk_stuff_end(&s, &a);
    if ((flags & MAILK_F_STRICT) && (rc == MAILK_EBARE) != (bare != 0))
        die("strict stuff EBARE does not match bare CR/LF in the input");
    mailk_stuff_init(&s2, flags & MAILK_F_STRICT);
    for (at = 0, rc2 = MAILK_OK; at < n && rc2 == MAILK_OK;) {
        size_t k = chunk_of(split, at, n - at);
        rc2 = mailk_stuff_feed(&s2, &b, p + at, k);
        at += k;
    }
    if (rc2 == MAILK_OK)
        rc2 = mailk_stuff_end(&s2, &b);
    if (rc != rc2 || (rc == MAILK_OK && (a.n != b.n || memcmp(a.p, b.p, a.n) || s.maxcol != s2.maxcol || s.has8bit != s2.has8bit || s.hasnul != s2.hasnul)))
        die("stuff differs between whole and split feeds");
    if (rc == MAILK_OK) {
        if (s.out != a.n || s.in != n)
            die("stuff byte counters");
        data_model(&m, p, n);
        b.n = 0;
        mailk_unstuff_init(&u, 0, 0);
        if (mailk_unstuff_feed(&u, &b, (const uint8_t*)a.p, a.n, &used) != MAILK_OK || used != a.n)
            die("unstuff did not stop exactly at the stuffed terminator");
        if (b.n != m.n || (m.n && memcmp(b.p, m.p, m.n)))
            die("unstuff(stuff(x)) differs from the CRLF-normalised input");
        mailk_unstuff_init(&u2, 0, 0);
        for (at = 0, rc2 = MAILK_NEED; at < a.n && rc2 == MAILK_NEED;) {
            size_t k = chunk_of(split ^ 0x55u, at, a.n - at), uu = 0;
            rc2 = mailk_unstuff_feed(&u2, &c, (const uint8_t*)a.p + at, k, &uu);
            at += uu;
        }
        if (rc2 != MAILK_OK || at != a.n || c.n != b.n || (b.n && memcmp(c.p, b.p, b.n)))
            die("unstuff differs between whole and split feeds");
    }
    b.n = c.n = 0;
    {
        uint64_t max = (flags & 8) ? 64 : 0;
        mailk_unstuff_init(&u, flags & MAILK_F_STRICT, max);
        rc = mailk_unstuff_feed(&u, &b, p, n, &used);
        if (used > n || (max && b.n > max && !u.over))
            die("unstuff output past its limit without over");
        mailk_unstuff_init(&u2, flags & MAILK_F_STRICT, max);
        for (at = 0, rc2 = MAILK_NEED; at < n && rc2 == MAILK_NEED;) {
            size_t k = chunk_of(split, at, n - at), uu = 0;
            rc2 = mailk_unstuff_feed(&u2, &c, p + at, k, &uu);
            at += uu;
            if (rc2 == MAILK_NEED && uu < k)
                die("unstuff NEED without consuming the chunk");
        }
        if (rc != rc2 || (rc == MAILK_OK && at != used) || (rc == MAILK_OK && (b.n != c.n || (b.n && memcmp(b.p, c.p, b.n)) || u.over != u2.over || u.bytes != u2.bytes)))
            die("raw unstuff differs between whole and split feeds");
    }
    dyn_sb_free(&a);
    dyn_sb_free(&b);
    dyn_sb_free(&m);
    dyn_sb_free(&c);
}

static void cli_run(const uint8_t* data, size_t size, unsigned flags, unsigned split, dyn_sb_t* rec)
{
    mailk_cli_t cl;
    mailk_cli_opts_t co;
    static const char* const to[] = { "x@y.example", "z@y.example" };
    mailk_msg_t m;
    size_t cut = size ? data[0] % size : 0, at;
    unsigned ev = 0;
    char h[96];
    memset(&co, 0, sizeof co);
    co.tls = (int)(flags & 3u);
    co.user = flags & 4u ? "u" : NULL;
    co.pass = "p";
    co.insecure_auth = 1;
    co.scram_max_iters = 16;
    co.mechs = flags & 16u ? MAILK_MECH_DEFAULT : MAILK_MECH_PLAIN | MAILK_MECH_LOGIN | MAILK_MECH_CRAM_MD5;
    mailk_cli_init(&cl, &co);
    for (at = 0; at < cut;) {
        size_t k = chunk_of(split, at, cut - at);
        mailk_cli_feed(&cl, data + at, k);
        at += k;
        ev |= cl.ev;
    }
    if (ev & MAILK_EV_TLS) {
        dyn_sb_puts(&cl.out, "<tls>");
        mailk_cli_tls_done(&cl);
        ev |= cl.ev;
    }
    if (ev & MAILK_EV_READY && !(cl.ev & (MAILK_EV_ERROR | MAILK_EV_CLOSED))) {
        memset(&m, 0, sizeof m);
        m.from = "a@b.example";
        m.to = to;
        m.nto = 1 + (cut & 1u);
        m.body = data;
        m.bodyn = size < 256 ? size : 256;
        m.bdat = (int)(flags & 1u);
        mailk_cli_send(&cl, &m);
    }
    for (at = cut; at < size;) {
        size_t k = chunk_of(split, at - cut, size - at);
        mailk_cli_feed(&cl, data + at, k);
        at += k;
    }
    dyn_sb_put(rec, cl.out.p ? cl.out.p : "", cl.out.n);
    snprintf(h, sizeof h, "|%x|%d|%d|%d|%d", cl.ev & (MAILK_EV_DONE | MAILK_EV_ERROR | MAILK_EV_CLOSED), cl.ok, cl.failed, cl.final.code, cl.mail.code);
    dyn_sb_puts(rec, h);
    mailk_cli_free(&cl);
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    mailk_reply_t r;
    mailk_path_t p;
    mailk_param_t pr;
    mailk_ehlo_ext_t ex;
    mailk_cmd_t c;
    dyn_sb_t a, b;
    size_t used, pos, off;
    unsigned flags, scram;
    if (!size)
        return 0;
    flags = data[0] & 7u;
    scram = data[0] & 0x80u ? 16u : 0u;
    data++;
    size--;
    mailk_reply_init(&r);
    for (off = 0; off < size;) {
        if (mailk_reply_feed(&r, data + off, size - off, &used) != MAILK_NEED || !used)
            break;
        off += used;
    }
    mailk_reply_free(&r);
    mailk_cmd_parse((const char*)data, size, &c);
    mailk_path_parse((const char*)data, size, flags, (int)(flags & 1), &p, &used);
    mailk_mail_parse((const char*)data, size, (int)(flags & 1), flags, &p, &used);
    pos = 0;
    while (mailk_param_next((const char*)data, size, &pos, flags, &pr) == 1)
        ;
    pos = 0;
    while (mailk_ehlo_next((const char*)data, size, &pos, &ex) != 0 && pos < size)
        ;
    dyn_sb_init(&a);
    dyn_sb_init(&b);
    mailk_xtext_decode(&a, (const char*)data, size);
    a.n = 0;
    check_data(data, size < 2048 ? size : 2048, flags | (size > 2 ? (unsigned)data[1] & 8u : 0u), size > 3 ? data[2] : 0u);
    a.n = 0;
    mailk_b64_decode((const char*)data, size, &a);
    a.n = 0;
    mailk_sasl_resp_decode((const char*)data, size, &a);
    a.n = 0;
    mailk_saslprep((const char*)data, size, &a);
    a.n = 0;
    mailk_saslname_decode(&a, (const char*)data, size);
    {
        mailk_plain_t pl;
        mailk_oauth_t oa;
        mailk_plain_parse(data, size, &pl);
        mailk_login_prompt(data, size);
        mailk_xoauth2_parse(data, size, &oa);
        mailk_oauthbearer_parse(data, size, &oa);
    }
    {
        unsigned split = size > 1 ? data[size - 1] | 1u : 1u;
        a.n = b.n = 0;
        cli_run(data, size < 2048 ? size : 2048, flags | scram, 0, &a);
        cli_run(data, size < 2048 ? size : 2048, flags | scram, split, &b);
        if (!scram && (a.n != b.n || (a.n && memcmp(a.p, b.p, a.n))))
            die("client output differs between whole and split feeds");
    }
    dyn_sb_free(&a);
    dyn_sb_free(&b);
    return 0;
}
