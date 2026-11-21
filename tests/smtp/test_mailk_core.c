/*
 * test_mailk_core.c -- src/core/mailk, the mail line kit shared by dyna:smtp (client and server),
 * dyna:imap and dyna:pop3, as a standalone C17 library (no engine, no I/O). S9 M1, 2026-11-26.
 * It is pure C with no JS values so every call can run on a pool thread (owner rule: mail
 * parse/encode/crypto always runs off the main JS thread).
 *
 * External oracles:
 *  - RFC 5321 2.3.8 / 4.1.1.4: only CRLF ends a line; a bare CR or LF is never a line end, and
 *    only <CRLF>.<CRLF> ends DATA ("\n.\n", "\r.\r", "\r\n.\n" do not: the SMTP smuggling
 *    shapes of CVE-2023-51766). Strict mode refuses bare CR/LF outright (MAILK_EBARE).
 *  - RFC 5321 4.2: Reply-code = %x32-35 %x30-35 %x30-39; "ddd-" continues, "ddd SP" or a bare
 *    "ddd" ends; every line of one reply carries the same code. RFC 1869 / 5321 D.1 EHLO examples.
 *  - RFC 3463 / 2034: enhanced status class.subject.detail, class 2/4/5 equal to the reply's first
 *    digit, subject and detail 1*3 digits.
 *  - RFC 5321 4.1.2 grammar: Reverse-path "<>", A-d-l source routes accepted and ignored,
 *    Dot-string / Quoted-string local parts, sub-domain = Let-dig [Ldh-str], address literals
 *    (IPv4 Snum, "IPv6:", General tag), RCPT "<Postmaster>"; 4.5.3.1 limits: local-part 64,
 *    domain 255, path 256 octets; RFC 1035 label 63.
 *  - RFC 6531 (SMTPUTF8): UTF-8 in local parts and U-labels only with MAILK_F_UTF8; overlong and
 *    surrogate sequences are refused even then. The 用户@例子.广告 example from RFC 6530.
 *  - RFC 1869 / 5321 4.1.2 esmtp-param: keyword (ALPHA/DIGIT) *(ALPHA/DIGIT/"-"), value
 *    1*(%d33-60 / %d62-126), one SP between params; MAIL FROM: with no space before the path
 *    (MAILK_F_LENIENT accepts one).
 *  - RFC 3461 4 xtext: "+" 2 UPPERCASE hex for bytes outside 33..126 and for "+" and "=".
 *  - RFC 5321 4.5.2 transparency: a line starting with "." gets one more "." on send and loses one
 *    on receive; stuff then unstuff is the identity on CRLF-normalised input, split at every byte.
 * Fuzz: 200000 random and mutated inputs through every parser under ASan/UBSan, random chunking.
 * Mutants (each red here): reply code-equality check dropped, bare LF accepted as a line end in
 * the unstuffer, the dot-stuffing on send dropped, the local-part 64 limit dropped, xtext "+"
 * left unencoded.
 * Seeded property tests (tests/lib/dyn-fuzzkit.h, 2026-11-27): DYN_FUZZ_SEED replays a run (the seed is
 * printed first), DYN_FUZZ_ITERS (integer, 50 = deep run) multiplies every loop. Each tagged check is proven by its own mutant:
 *  [rt-reply]     generated multi-line, pipelined replies (codes, enhanced codes, text) fed whole, per
 *                 byte and at random splits parse back to the model; mutant: continuation join char.
 *  [rt-cmd]       every verb in random case with args round-trips through mailk_cmd_parse; mutant:
 *                 case-sensitive verb match.
 *  [rt-path]      random local parts (dot-atom, quoted), domains, literals and ESMTP params through
 *                 cmd_path -> cmd_parse -> mail_parse -> param_next; mutant: param value truncated.
 *  [diff-stuff] [diff-unstuff] stuff/unstuff vs a naive line model at random chunk splits, bare CR/LF,
 *                 strict mode; mutants: bare-LF fix dropped, leading dot of a line kept.
 *  [rt-xtext]     xtext encode/decode identity on random bytes; mutant: '=' left unescaped.
 *  [bound]        domain 255/256, label 63/64, path 256 ok / 257 ELONG, 512 reply lines, reply line
 *                 4096/4097; mutant: path cap off by one.
 *  [fuzz-core]    src/fuzz/fuzz_mailk_reply.c (libFuzzer target; aborts on split-feed divergence and reply,
 *                 MAIL/RCPT, esc and xtext round-trip loss) over mutated seeds; mutant: line state reset per feed.
 */
#include "dyn-mailk.h"
#include "../lib/dyn-fuzzkit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static fk_rng_t fk_g;

static uint64_t rnd(void)
{
    return fk_next(&fk_g);
}

static int reply_all(const char* s, size_t n, mailk_reply_t* r, size_t split)
{
    size_t off = 0, u;
    int rc = MAILK_NEED;
    mailk_reply_reset(r);
    while (off < n) {
        size_t m = split && n - off > split ? split : n - off;
        rc = mailk_reply_feed(r, (const uint8_t*)s + off, m, &u);
        off += u;
        if (rc != MAILK_NEED)
            return rc;
    }
    return rc;
}

static void test_line(void)
{
    mailk_line_t l;
    size_t u;
    mailk_line_init(&l, 8);
    CHECK(mailk_line_feed(&l, (const uint8_t*)"abc\r", 4, &u) == MAILK_NEED && u == 4, "partial");
    CHECK(mailk_line_feed(&l, (const uint8_t*)"\nX", 2, &u) == MAILK_OK && u == 1 && l.buf.n == 3, "crlf across feeds");
    mailk_line_reset(&l);
    CHECK(mailk_line_feed(&l, (const uint8_t*)"a\nb", 3, &u) == MAILK_EBARE, "bare LF");
    mailk_line_reset(&l);
    CHECK(mailk_line_feed(&l, (const uint8_t*)"a\rb", 3, &u) == MAILK_EBARE, "bare CR");
    mailk_line_reset(&l);
    CHECK(mailk_line_feed(&l, (const uint8_t*)"123456789\r\n", 11, &u) == MAILK_ELONG, "over max");
    mailk_line_free(&l);
}

static void test_reply(void)
{
    static const char ehlo[] = "250-foo.com greets bar.com\r\n250-8BITMIME\r\n250-SIZE 10240000\r\n250-DSN\r\n250-AUTH PLAIN LOGIN\r\n250 HELP\r\n";
    mailk_reply_t r;
    size_t split, pos;
    mailk_ehlo_ext_t e;
    int n;
    mailk_reply_init(&r);
    for (split = 0; split < sizeof ehlo; split++) {
        CHECK(reply_all(ehlo, sizeof ehlo - 1, &r, split) == MAILK_OK && r.code == 250 && r.nlines == 6, "ehlo split %zu", split);
    }
    pos = 0;
    n = 0;
    while (mailk_ehlo_next(r.text.p, r.text.n, &pos, &e) == 1) {
        if (n == 1)
            CHECK(e.kwn == 4 && !memcmp(e.kw, "SIZE", 4) && e.paramsn == 8 && !memcmp(e.params, "10240000", 8), "SIZE param");
        if (n == 3)
            CHECK(e.paramsn == 11 && !memcmp(e.params, "PLAIN LOGIN", 11), "AUTH params");
        n++;
    }
    CHECK(n == 5, "five extensions, got %d", n);
    CHECK(reply_all("250-a\r\n251 b\r\n", 14, &r, 0) == MAILK_EBAD, "code mismatch");
    CHECK(reply_all("2501\r\n", 6, &r, 0) == MAILK_EBAD, "4 digits");
    CHECK(reply_all("199 x\r\n", 7, &r, 0) == MAILK_EBAD, "1xx");
    CHECK(reply_all("260 x\r\n", 7, &r, 0) == MAILK_EBAD, "second digit 6");
    CHECK(reply_all("25 x\r\n", 6, &r, 0) == MAILK_EBAD, "2 digits");
    CHECK(reply_all("250\r\n", 5, &r, 0) == MAILK_OK && r.code == 250 && r.text.n == 0, "code alone");
    CHECK(reply_all("550 5.1.1 User unknown\r\n", 24, &r, 0) == MAILK_OK && r.esc_class == 5 && r.esc_subject == 1 && r.esc_detail == 1, "esc");
    CHECK(reply_all("451 4.3.0\r\n", 11, &r, 0) == MAILK_OK && r.esc_class == 4 && r.esc_detail == 0, "esc alone");
    CHECK(reply_all("550 2.1.1 x\r\n", 13, &r, 0) == MAILK_OK && r.esc_class == -1, "esc class mismatch ignored");
    CHECK(reply_all("550 5.1234.1 x\r\n", 16, &r, 0) == MAILK_OK && r.esc_class == -1, "esc subject 4 digits");
    CHECK(reply_all("250 a\nb\r\n", 9, &r, 0) == MAILK_EBARE, "bare LF in reply");
    {
        size_t u;
        mailk_reply_reset(&r);
        CHECK(mailk_reply_feed(&r, (const uint8_t*)"220 hi\r\n250 next", 16, &u) == MAILK_OK && u == 8, "used stops at reply end");
    }
    {
        dyn_sb_t b;
        int k;
        dyn_sb_init(&b);
        for (k = 0; k < (int)MAILK_REPLY_LINES_MAX + 1; k++)
            dyn_sb_puts(&b, "250-x\r\n");
        dyn_sb_puts(&b, "250 y\r\n");
        CHECK(reply_all(b.p, b.n, &r, 0) == MAILK_ELONG, "too many lines");
        dyn_sb_free(&b);
    }
    mailk_reply_free(&r);
}

static void test_cmd(void)
{
    mailk_cmd_t c;
    CHECK(mailk_cmd_parse("MAIL FROM:<a@b>", 15, &c) == MAILK_OK && c.verb == MAILK_VERB_MAIL && c.argn == 10, "MAIL");
    CHECK(mailk_cmd_parse("starttls", 8, &c) == MAILK_OK && c.verb == MAILK_VERB_STARTTLS && c.argn == 0, "STARTTLS lc");
    CHECK(mailk_cmd_parse("XFOO bar", 8, &c) == MAILK_OK && c.verb == MAILK_VERB_UNKNOWN, "unknown");
    CHECK(mailk_cmd_parse("", 0, &c) == MAILK_EBAD, "empty");
    CHECK(mailk_cmd_parse("MA!L x", 6, &c) == MAILK_EBAD, "punct verb");
}

static int path(const char* s, unsigned f, int rcpt, mailk_path_t* p)
{
    size_t u;
    int rc = mailk_path_parse(s, strlen(s), f, rcpt, p, &u);
    return rc == MAILK_OK && u != strlen(s) ? MAILK_EBAD : rc;
}

static void test_path(void)
{
    static const char* good[] = {
        "<user@example.com>", "<@a.com,@b.com:user@c.com>", "<\"john doe\"@example.com>",
        "<\"a\\\"b\"@x.org>", "<u@[192.0.2.1]>", "<u@[IPv6:2001:db8::1]>", "<u@[IPv6:::ffff:192.0.2.1]>",
        "<u@[x-tag:abc]>", "<a.b+tag@sub-1.example.co>", "<!#$%&'*+-/=?^_`{|}~@x.y>", "<a@x>",
    };
    static const char* bad[] = {
        "<a..b@x>", "<.a@x>", "<a.@x>", "<a@-x.com>", "<a@x-.com>", "<a@x..com>", "<a@[256.0.0.1]>",
        "<a@[1.2.3]>", "<a b@x>", "<a@x", "a@x", "<a@>", "<@x>", "<a@[IPv6:zz::1]>", "<\"a\rb\"@x>",
        "<\"a\nb\"@x>", "<a@x.com.>", "<a@x_y.com>", "<a@[1.2.3.4]x>", "<Postmaster>", "<>x",
    };
    mailk_path_t p;
    size_t i;
    char buf[400];
    for (i = 0; i < sizeof good / sizeof good[0]; i++)
        CHECK(path(good[i], 0, 0, &p) == MAILK_OK, "good %s", good[i]);
    for (i = 0; i < sizeof bad / sizeof bad[0]; i++)
        CHECK(path(bad[i], 0, 0, &p) != MAILK_OK, "bad %s", bad[i]);
    CHECK(path("<>", 0, 0, &p) == MAILK_OK && p.null_path, "null reverse-path");
    CHECK(path("<>", 0, 1, &p) != MAILK_OK, "null forward-path refused");
    CHECK(path("<Postmaster>", 0, 1, &p) == MAILK_OK && p.postmaster, "RCPT <Postmaster>");
    CHECK(path("<user@example.com>", 0, 0, &p) == MAILK_OK && p.lo == 1 && p.ln == 4 && p.dn_off == 6 && p.dn == 11, "offsets");
    CHECK(path("<@a.com:u@c.com>", 0, 0, &p) == MAILK_OK && p.lo == 8 && p.ln == 1, "offsets after a-d-l");
    CHECK(path("<用户@例子.广告>", 0, 0, &p) != MAILK_OK, "utf8 without SMTPUTF8");
    CHECK(path("<用户@例子.广告>", MAILK_F_UTF8, 0, &p) == MAILK_OK && p.utf8, "RFC 6530 example");
    CHECK(path("<\xC0\x80@x>", MAILK_F_UTF8, 0, &p) != MAILK_OK, "overlong");
    CHECK(path("<\xED\xA0\x80@x>", MAILK_F_UTF8, 0, &p) != MAILK_OK, "surrogate");
    memset(buf, 'a', sizeof buf);
    buf[0] = '<';
    memcpy(buf + 66, "@x>", 4);
    CHECK(path(buf, 0, 0, &p) == MAILK_ELONG, "local 65");
    memcpy(buf + 65, "@x>", 4);
    CHECK(path(buf, 0, 0, &p) == MAILK_OK, "local exactly 64");
    memset(buf, 'b', sizeof buf);
    memcpy(buf, "<a@", 3);
    memcpy(buf + 3 + 64, ".c>", 4);
    CHECK(path(buf, 0, 0, &p) != MAILK_OK, "label 64");
    {
        size_t k = 3;
        memcpy(buf, "<a@", 3);
        while (k < 3 + 260) {
            memcpy(buf + k, "abcdefghi.", 10);
            k += 10;
        }
        memcpy(buf + k, "zz>", 4);
        CHECK(path(buf, 0, 0, &p) != MAILK_OK, "domain over 255");
    }
}

static void test_mail(void)
{
    static const char a[] = "FROM:<a@b.c> SIZE=1000 BODY=8BITMIME";
    mailk_path_t p;
    mailk_param_t pr;
    size_t off, pos;
    int n = 0, rc;
    CHECK(mailk_mail_parse(a, sizeof a - 1, 0, 0, &p, &off) == MAILK_OK && off == 13, "mail parse");
    pos = off;
    while ((rc = mailk_param_next(a, sizeof a - 1, &pos, 0, &pr)) == 1) {
        if (!n)
            CHECK(pr.kwn == 4 && pr.valn == 4 && !memcmp(pr.val, "1000", 4), "SIZE=1000");
        n++;
    }
    CHECK(rc == 0 && n == 2, "two params");
    CHECK(mailk_mail_parse("from:<a@b.c>", 12, 0, 0, &p, &off) == MAILK_OK && off == 12, "lowercase, no params");
    CHECK(mailk_mail_parse("FROM: <a@b.c>", 13, 0, 0, &p, &off) == MAILK_EBAD, "space before path strict");
    CHECK(mailk_mail_parse("FROM: <a@b.c>", 13, 0, MAILK_F_LENIENT, &p, &off) == MAILK_OK && p.lo == 7, "lenient space");
    CHECK(mailk_mail_parse("FROM:<a@b.c> ", 13, 0, 0, &p, &off) == MAILK_EBAD, "trailing space");
    CHECK(mailk_mail_parse("TO:<Postmaster>", 15, 1, 0, &p, &off) == MAILK_OK && p.postmaster, "rcpt postmaster");
    pos = 0;
    CHECK(mailk_param_next("X=a=b", 5, &pos, 0, &pr) == MAILK_EBAD, "'=' in value");
    pos = 0;
    CHECK(mailk_param_next("A=1  B=2", 8, &pos, 0, &pr) == 1 && mailk_param_next("A=1  B=2", 8, &pos, 0, &pr) == MAILK_EBAD, "double space");
    pos = 0;
    CHECK(mailk_param_next("-X=1", 4, &pos, 0, &pr) == MAILK_EBAD, "keyword starts with '-'");
}

static void test_encode(void)
{
    dyn_sb_t b;
    const char* ps[] = { "SIZE=10", "BODY=8BITMIME" };
    const char* inj[] = { "SIZE=1\r\nRSET" };
    const char* sp[] = { "SIZE=1 X" };
    dyn_sb_init(&b);
    CHECK(mailk_cmd_path(&b, 0, "a@b.c", 5, 0, ps, 2) == MAILK_OK && b.n == 41 && !memcmp(b.p, "MAIL FROM:<a@b.c> SIZE=10 BODY=8BITMIME\r\n", 41), "MAIL encode");
    b.n = 0;
    CHECK(mailk_cmd_path(&b, 0, "", 0, 0, NULL, 0) == MAILK_OK && b.n == 14 && !memcmp(b.p, "MAIL FROM:<>\r\n", 14), "null MAIL");
    b.n = 0;
    CHECK(mailk_cmd_path(&b, 1, "", 0, 0, NULL, 0) != MAILK_OK, "empty RCPT refused");
    CHECK(mailk_cmd_path(&b, 1, "a@b>\r\nRCPT TO:<x@y", 18, 0, NULL, 0) != MAILK_OK, "address injection");
    CHECK(mailk_cmd_path(&b, 1, "a@b.c", 5, 0, inj, 1) != MAILK_OK, "param CRLF injection");
    CHECK(mailk_cmd_path(&b, 1, "a@b.c", 5, 0, sp, 1) != MAILK_OK, "param with space");
    CHECK(mailk_cmd_path(&b, 1, "用户@例子.广告", strlen("用户@例子.广告"), 0, NULL, 0) != MAILK_OK, "utf8 needs SMTPUTF8");
    dyn_sb_free(&b);
}

static void test_xtext(void)
{
    dyn_sb_t b, d;
    uint8_t all[256];
    int i;
    dyn_sb_init(&b);
    dyn_sb_init(&d);
    CHECK(mailk_xtext_encode(&b, (const uint8_t*)"a+b=c d", 7) == MAILK_OK && b.n == 13 && !memcmp(b.p, "a+2Bb+3Dc+20d", 13), "encode");
    CHECK(mailk_xtext_decode(&d, "+2b", 3) == MAILK_EBAD, "lowercase hex");
    CHECK(mailk_xtext_decode(&d, "ab+2", 4) == MAILK_EBAD, "truncated");
    CHECK(mailk_xtext_decode(&d, "a=b", 3) == MAILK_EBAD, "raw '='");
    for (i = 0; i < 256; i++)
        all[i] = (uint8_t)i;
    b.n = d.n = 0;
    mailk_xtext_encode(&b, all, 256);
    CHECK(mailk_xtext_decode(&d, b.p, b.n) == MAILK_OK && d.n == 256 && !memcmp(d.p, all, 256), "roundtrip 0..255");
    dyn_sb_free(&b);
    dyn_sb_free(&d);
}

static int stuff_str(const char* in, size_t n, unsigned f, dyn_sb_t* out, size_t split, mailk_stuff_t* st)
{
    size_t off = 0;
    int rc;
    mailk_stuff_init(st, f);
    out->n = 0;
    while (off < n) {
        size_t m = split && n - off > split ? split : n - off;
        if ((rc = mailk_stuff_feed(st, out, (const uint8_t*)in + off, m)) != MAILK_OK)
            return rc;
        off += m;
    }
    return mailk_stuff_end(st, out);
}

static int unstuff_str(const char* in, size_t n, unsigned f, uint64_t max, dyn_sb_t* out, size_t split, mailk_unstuff_t* u, size_t* end)
{
    size_t off = 0, used;
    int rc = MAILK_NEED;
    mailk_unstuff_init(u, f, max);
    out->n = 0;
    while (off < n) {
        size_t m = split && n - off > split ? split : n - off;
        rc = mailk_unstuff_feed(u, out, (const uint8_t*)in + off, m, &used);
        off += used;
        if (rc != MAILK_NEED)
            break;
    }
    *end = off;
    return rc;
}

#define EQ(b, lit) ((b).n == sizeof(lit) - 1 && !memcmp((b).p, lit, sizeof(lit) - 1))

static void test_data(void)
{
    dyn_sb_t o;
    mailk_stuff_t s;
    mailk_unstuff_t u;
    size_t e, split;
    dyn_sb_init(&o);
    CHECK(stuff_str(".hello\r\n", 8, 0, &o, 0, &s) == MAILK_OK && EQ(o, "..hello\r\n.\r\n"), "leading dot");
    CHECK(stuff_str("a\r\n.\r\nb", 7, 0, &o, 0, &s) == MAILK_OK && EQ(o, "a\r\n..\r\nb\r\n.\r\n"), "lone dot line");
    CHECK(stuff_str("a\nb", 3, 0, &o, 0, &s) == MAILK_OK && EQ(o, "a\r\nb\r\n.\r\n") && s.fixed_bare == 1, "bare LF normalised");
    CHECK(stuff_str("a\rb", 3, 0, &o, 0, &s) == MAILK_OK && EQ(o, "a\r\nb\r\n.\r\n"), "bare CR normalised");
    CHECK(stuff_str("a\nb", 3, MAILK_F_STRICT, &o, 0, &s) == MAILK_EBARE, "strict bare LF");
    CHECK(stuff_str("", 0, 0, &o, 0, &s) == MAILK_OK && EQ(o, ".\r\n"), "empty");
    CHECK(stuff_str("x\r\n", 3, 0, &o, 0, &s) == MAILK_OK && EQ(o, "x\r\n.\r\n"), "no extra CRLF");
    CHECK(stuff_str("\xC3\xA9\r\n", 4, 0, &o, 0, &s) == MAILK_OK && s.has8bit && s.maxcol == 2, "8bit");
    for (split = 1; split < 12; split++)
        CHECK(stuff_str(".a\r\n.\r\n..b", 10, 0, &o, split, &s) == MAILK_OK && EQ(o, "..a\r\n..\r\n...b\r\n.\r\n"), "stuff split %zu", split);
    CHECK(unstuff_str(".\r\n", 3, 0, 0, &o, 0, &u, &e) == MAILK_OK && o.n == 0 && e == 3, "empty data");
    CHECK(unstuff_str("..a\r\n.\r\nNEXT", 12, 0, 0, &o, 0, &u, &e) == MAILK_OK && EQ(o, ".a\r\n") && e == 8, "unstuff + used");
    for (split = 1; split < 20; split++) {
        CHECK(unstuff_str("a\n.\nb\r\n.\r\n", 10, 0, 0, &o, split, &u, &e) == MAILK_OK && e == 10 && u.bare == 2 && EQ(o, "a\n.\nb\r\n"), "smuggle LF split %zu", split);
        CHECK(unstuff_str("a\r\n.\nX\r\n.\r\n", 11, 0, 0, &o, split, &u, &e) == MAILK_OK && e == 11, "smuggle CRLF.LF split %zu", split);
        CHECK(unstuff_str("a\r.\rb\r\n.\r\n", 10, 0, 0, &o, split, &u, &e) == MAILK_OK && e == 10, "smuggle CR.CR split %zu", split);
    }
    CHECK(unstuff_str("a\n.\nb\r\n.\r\n", 10, MAILK_F_STRICT, 0, &o, 0, &u, &e) == MAILK_EBARE, "strict refuses bare LF");
    CHECK(unstuff_str("abcdef\r\n.\r\n", 11, 0, 4, &o, 0, &u, &e) == MAILK_OK && u.over && e == 11, "max: over, still finds end");
    CHECK(unstuff_str("abc\r\n", 5, 0, 0, &o, 0, &u, &e) == MAILK_NEED, "unterminated");
    dyn_sb_free(&o);
}

static void normalise(const uint8_t* p, size_t n, dyn_sb_t* o)
{
    size_t i;
    o->n = 0;
    for (i = 0; i < n; i++) {
        if (p[i] == '\r') {
            dyn_sb_put(o, "\r\n", 2);
            if (i + 1 < n && p[i + 1] == '\n')
                i++;
        } else if (p[i] == '\n')
            dyn_sb_put(o, "\r\n", 2);
        else
            dyn_sb_putc(o, (char)p[i]);
    }
    if (o->n && !(o->n >= 2 && o->p[o->n - 2] == '\r' && o->p[o->n - 1] == '\n'))
        dyn_sb_put(o, "\r\n", 2);
}

static void test_fuzz(void)
{
    static const char alpha[] = "ab.\r\n<>@:\"\\[]=+ 0123456789-,\xC3\xA9\xFF";
    uint8_t buf[256];
    dyn_sb_t a, b, c;
    mailk_reply_t r;
    mailk_path_t p;
    mailk_param_t pr;
    mailk_stuff_t s;
    mailk_unstuff_t u;
    size_t used, pos, e;
    int it, rt_bad = 0;
    dyn_sb_init(&a);
    dyn_sb_init(&b);
    dyn_sb_init(&c);
    mailk_reply_init(&r);
    for (it = 0; it < 200000; it++) {
        size_t n = (size_t)(rnd() % sizeof buf), i, off;
        for (i = 0; i < n; i++)
            buf[i] = (rnd() & 3) ? (uint8_t)alpha[rnd() % (sizeof alpha - 1)] : (uint8_t)rnd();
        mailk_reply_reset(&r);
        for (off = 0; off < n;) {
            size_t m = 1 + (size_t)(rnd() % 16);
            if (m > n - off)
                m = n - off;
            if (mailk_reply_feed(&r, buf + off, m, &used) != MAILK_NEED)
                break;
            off += used;
        }
        mailk_path_parse((const char*)buf, n, (unsigned)(rnd() & 3), (int)(rnd() & 1), &p, &used);
        mailk_mail_parse((const char*)buf, n, (int)(rnd() & 1), (unsigned)(rnd() & 3), &p, &used);
        pos = 0;
        while (mailk_param_next((const char*)buf, n, &pos, MAILK_F_UTF8, &pr) == 1)
            ;
        pos = 0;
        {
            mailk_ehlo_ext_t ex;
            while (mailk_ehlo_next((const char*)buf, n, &pos, &ex) != 0 && pos < n)
                ;
        }
        a.n = 0;
        mailk_xtext_decode(&a, (const char*)buf, n);
        unstuff_str((const char*)buf, n, (unsigned)(rnd() & MAILK_F_STRICT), rnd() % 300, &a, 1 + (size_t)(rnd() % 9), &u, &e);
        stuff_str((const char*)buf, n, 0, &a, 1 + (size_t)(rnd() % 9), &s);
        if (unstuff_str(a.p, a.n, 0, 0, &b, 1 + (size_t)(rnd() % 9), &u, &e) != MAILK_OK || e != a.n)
            rt_bad++;
        else {
            normalise(buf, n, &c);
            if (b.n != c.n || (b.n && memcmp(b.p, c.p, b.n)))
                rt_bad++;
        }
    }
    CHECK(rt_bad == 0, "unstuff(stuff(x)) == normalise(x) failed %d times", rt_bad);
    mailk_reply_free(&r);
    dyn_sb_free(&a);
    dyn_sb_free(&b);
    dyn_sb_free(&c);
}


int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

typedef struct {
    int code, ncls, sj, dt, esc;
    int nl;
    dyn_sb_t wire, text;
} rmodel_t;

static void gen_reply(rmodel_t* m)
{
    static const char tch[] = "abc XYZ-09.=:;()<>@\t";
    int l, k;
    m->code = (2 + (int)(rnd() % 4)) * 100 + (int)(rnd() % 6) * 10 + (int)(rnd() % 10);
    m->nl = 1 + (int)(rnd() % (rnd() % 8 == 0 ? 40 : 4));
    m->esc = 0;
    m->wire.n = m->text.n = 0;
    for (l = 0; l < m->nl; l++) {
        char hd[8];
        int last = l == m->nl - 1, nt = (int)(rnd() % 30);
        snprintf(hd, sizeof hd, "%d", m->code);
        dyn_sb_puts(&m->wire, hd);
        if (last && rnd() % 6 == 0 && l == 0) {
            dyn_sb_puts(&m->wire, "\r\n");
            break;
        }
        dyn_sb_putc(&m->wire, last ? ' ' : '-');
        if (l)
            dyn_sb_putc(&m->text, '\n');
        if (l == 0 && rnd() % 2) {
            int cls = rnd() % 5 == 0 ? (int)(2 + rnd() % 4) : m->code / 100, sd = (int)(1 + rnd() % (rnd() % 6 ? 3 : 4)), dd = (int)(1 + rnd() % 3);
            char e[24];
            int sv = (int)(rnd() % 1000), dv = (int)(rnd() % 1000);
            for (k = sd; k < 3; k++)
                sv %= k == 1 ? 10 : 100;
            if (sd == 1)
                sv %= 10;
            else if (sd == 2)
                sv %= 100;
            if (dd == 1)
                dv %= 10;
            else if (dd == 2)
                dv %= 100;
            snprintf(e, sizeof e, "%d.%0*d.%0*d", cls, sd, sv, dd, dv);
            dyn_sb_puts(&m->wire, e);
            dyn_sb_puts(&m->text, e);
            if (sd <= 3 && cls == m->code / 100 && (cls == 2 || cls == 4 || cls == 5)) {
                m->esc = 1;
                m->ncls = cls;
                m->sj = sv;
                m->dt = dv;
            }
            if (nt) {
                dyn_sb_putc(&m->wire, ' ');
                dyn_sb_putc(&m->text, ' ');
            }
        }
        for (k = 0; k < nt; k++) {
            char ch = tch[rnd() % (sizeof tch - 1)];
            dyn_sb_putc(&m->wire, ch);
            dyn_sb_putc(&m->text, ch);
        }
        dyn_sb_puts(&m->wire, "\r\n");
    }
}

static void test_rt_reply(void)
{
    rmodel_t ms[6];
    dyn_sb_t stream;
    mailk_reply_t r;
    long it, N = (long)fk_iters(20000);
    int bad = 0, k;
    fk_g = fk_stream("rt-reply");
    dyn_sb_init(&stream);
    for (k = 0; k < 6; k++) {
        dyn_sb_init(&ms[k].wire);
        dyn_sb_init(&ms[k].text);
    }
    mailk_reply_init(&r);
    for (it = 0; it < N; it++) {
        int nm = 1 + (int)(rnd() % 6), cur = 0, mode = (int)(rnd() % 3);
        size_t at = 0;
        stream.n = 0;
        for (k = 0; k < nm; k++) {
            gen_reply(&ms[k]);
            dyn_sb_put(&stream, ms[k].wire.p, ms[k].wire.n);
        }
        mailk_reply_reset(&r);
        while (at < stream.n && cur < nm) {
            size_t ch = mode == 0 ? stream.n - at : mode == 1 ? 1 : 1 + rnd() % 13, u = 0;
            int rc;
            if (ch > stream.n - at)
                ch = stream.n - at;
            rc = mailk_reply_feed(&r, (const uint8_t*)stream.p + at, ch, &u);
            at += u;
            if (rc == MAILK_OK) {
                const rmodel_t* m = &ms[cur];
                if (r.code != m->code || (int)r.nlines != m->nl || r.text.n != m->text.n || (m->text.n && memcmp(r.text.p, m->text.p, m->text.n))
                    || (m->esc ? (r.esc_class != m->ncls || r.esc_subject != m->sj || r.esc_detail != m->dt) : r.esc_class != -1)) {
                    if (bad++ < 3)
                        CHECK(0, "[rt-reply] reply %d of %d misread it=%ld mode %d: code %d/%d lines %u/%d esc %d.%d.%d/%s%d.%d.%d text [%.*s] want [%.*s]", cur, nm, it, mode, r.code, m->code,
                            r.nlines, m->nl, r.esc_class, r.esc_subject, r.esc_detail, m->esc ? "" : "none ", m->ncls, m->sj, m->dt, (int)r.text.n, r.text.p ? r.text.p : "", (int)m->text.n,
                            m->text.p ? m->text.p : "");
                }
                cur++;
                mailk_reply_reset(&r);
            } else if (rc != MAILK_NEED) {
                if (bad++ < 3)
                    CHECK(0, "[rt-reply] generated reply %d refused rc=%d it=%ld [%.*s]", cur, rc, it, (int)ms[cur].wire.n, ms[cur].wire.p);
                break;
            } else if (u < ch) {
                if (bad++ < 3)
                    CHECK(0, "[rt-reply] NEED with %zu of %zu bytes used it=%ld", u, ch, it);
                break;
            }
        }
        if (cur != nm && bad++ < 3)
            CHECK(0, "[rt-reply] %d of %d pipelined replies parsed it=%ld mode %d", cur, nm, it, mode);
    }
    CHECK(bad == 0, "[rt-reply] %d of %ld generated reply streams misread", bad, N);
    for (k = 0; k < 6; k++) {
        dyn_sb_free(&ms[k].wire);
        dyn_sb_free(&ms[k].text);
    }
    dyn_sb_free(&stream);
    mailk_reply_free(&r);
}

static void test_rt_cmd(void)
{
    static const char* const vn[] = { "HELO", "EHLO", "MAIL", "RCPT", "DATA", "BDAT", "RSET", "NOOP", "QUIT", "VRFY", "EXPN", "HELP", "STARTTLS", "AUTH" };
    static const char al[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    long it, N = (long)fk_iters(20000);
    int bad = 0;
    char b[96], arg[48];
    fk_g = fk_stream("rt-cmd");
    for (it = 0; it < N; it++) {
        mailk_cmd_t c;
        int want, v = (int)(rnd() % 16), k;
        size_t n = 0, an = rnd() % 40, vl;
        if (v < 14) {
            vl = strlen(vn[v]);
            for (k = 0; k < (int)vl; k++)
                b[n++] = rnd() & 1 ? (char)(vn[v][k] | 32) : vn[v][k];
            want = v + 1;
        } else {
            vl = 1 + rnd() % 9;
            for (k = 0; k < (int)vl; k++)
                b[n++] = al[rnd() % (sizeof al - 1)];
            want = 0;
            for (k = 0; k < 14; k++)
                if (strlen(vn[k]) == vl) {
                    size_t j = 0;
                    while (j < vl && (b[j] & ~32) == vn[k][j])
                        j++;
                    if (j == vl)
                        want = k + 1;
                }
        }
        for (k = 0; k < (int)an; k++)
            arg[k] = (char)(32 + rnd() % 95);
        if (an || rnd() & 1) {
            b[n++] = ' ';
            memcpy(b + n, arg, an);
            n += an;
        } else
            an = (size_t)-1;
        if (mailk_cmd_parse(b, n, &c) != MAILK_OK || c.verb != want || (an == (size_t)-1 ? c.argn != 0 : (c.argn != an || memcmp(c.arg, arg, an)))) {
            if (bad++ < 3)
                CHECK(0, "[rt-cmd] command misread it=%ld [%.*s] verb %d want %d", it, (int)n, b, c.verb, want);
        }
    }
    CHECK(bad == 0, "[rt-cmd] %d of %ld generated commands misread", bad, N);
}

static const char atext_ch[] = "abcXYZ019!#$%&'*+-/=?^_`{|}~";

static void gen_local(dyn_sb_t* o, size_t want)
{
    size_t k;
    if (rnd() % 5 == 0 && want >= 2) {
        dyn_sb_putc(o, '"');
        for (k = 0; k + 2 < want; k++) {
            if (rnd() % 9 == 0 && k + 3 < want) {
                dyn_sb_putc(o, '\\');
                dyn_sb_putc(o, "\"\\ a"[rnd() % 4]);
                k++;
            } else
                dyn_sb_putc(o, "ab @,:<> .x"[rnd() % 11]);
        }
        dyn_sb_putc(o, '"');
        return;
    }
    for (k = 0; k < want; k++) {
        if (k && k + 1 < want && o->p[o->n - 1] != '.' && rnd() % 6 == 0)
            dyn_sb_putc(o, '.');
        else
            dyn_sb_putc(o, atext_ch[rnd() % (sizeof atext_ch - 1)]);
    }
}

static void gen_domain(dyn_sb_t* o, size_t labels, size_t maxlab)
{
    static const char ld[] = "abcxyz0189ABZ";
    size_t l, k;
    for (l = 0; l < labels; l++) {
        size_t ln = 1 + rnd() % maxlab;
        if (l)
            dyn_sb_putc(o, '.');
        for (k = 0; k < ln; k++)
            dyn_sb_putc(o, k && k + 1 < ln && rnd() % 5 == 0 ? '-' : ld[rnd() % (sizeof ld - 1)]);
    }
}

static void test_rt_path(void)
{
    long it, N = (long)fk_iters(10000);
    int bad = 0;
    dyn_sb_t loc, dom, mbox, cmd;
    char pb[8][40];
    const char* pv[8];
    fk_g = fk_stream("rt-path");
    dyn_sb_init(&loc);
    dyn_sb_init(&dom);
    dyn_sb_init(&mbox);
    dyn_sb_init(&cmd);
    for (it = 0; it < N; it++) {
        int rcpt = (int)(rnd() & 1), lit = rnd() % 8 == 0;
        size_t np = rnd() % 5, k, off, pos;
        mailk_cmd_t c;
        mailk_path_t p;
        mailk_param_t pr;
        int rc;
        loc.n = dom.n = mbox.n = cmd.n = 0;
        gen_local(&loc, 1 + rnd() % 20);
        if (lit) {
            char ip[40];
            snprintf(ip, sizeof ip, "[%u.%u.%u.%u]", (unsigned)(rnd() % 256), (unsigned)(rnd() % 256), (unsigned)(rnd() % 256), (unsigned)(rnd() % 256));
            dyn_sb_puts(&dom, ip);
        } else
            gen_domain(&dom, 1 + rnd() % 4, 12);
        dyn_sb_put(&mbox, loc.p, loc.n);
        dyn_sb_putc(&mbox, '@');
        dyn_sb_put(&mbox, dom.p, dom.n);
        for (k = 0; k < np; k++) {
            size_t kl = 1 + rnd() % 8, j, vl = rnd() % 10;
            char* q = pb[k];
            for (j = 0; j < kl; j++)
                *q++ = j && rnd() % 5 == 0 ? '-' : "ABZ09xy"[rnd() % 7];
            if (vl) {
                *q++ = '=';
                for (j = 0; j < vl; j++) {
                    char ch = (char)(33 + rnd() % 94);
                    *q++ = ch == '=' ? '+' : ch;
                }
            }
            *q = 0;
            pv[k] = pb[k];
        }
        if ((rc = mailk_cmd_path(&cmd, rcpt, mbox.p, mbox.n, 0, pv, np)) != MAILK_OK) {
            if (bad++ < 3)
                CHECK(0, "[rt-path] cmd_path refused a grammar-valid path rc=%d it=%ld [%.*s]", rc, it, (int)mbox.n, mbox.p);
            continue;
        }
        if (mailk_cmd_parse(cmd.p, cmd.n - 2, &c) != MAILK_OK || c.verb != (rcpt ? MAILK_VERB_RCPT : MAILK_VERB_MAIL)
            || mailk_mail_parse(c.arg, c.argn, rcpt, 0, &p, &off) != MAILK_OK || p.ln != loc.n || memcmp(c.arg + p.lo, loc.p, loc.n) || p.dn != dom.n
            || memcmp(c.arg + p.dn_off, dom.p, dom.n) || p.literal != lit || p.quoted != (loc.p[0] == '"')) {
            if (bad++ < 3)
                CHECK(0, "[rt-path] round trip lost the address it=%ld [%.*s]", it, (int)cmd.n, cmd.p);
            continue;
        }
        pos = off;
        for (k = 0; k < np; k++) {
            const char* eq = strchr(pv[k], '=');
            size_t kl = eq ? (size_t)(eq - pv[k]) : strlen(pv[k]);
            if (mailk_param_next(c.arg, c.argn, &pos, 0, &pr) != 1 || pr.kwn != kl || memcmp(pr.kw, pv[k], kl) || (eq ? pr.valn != strlen(eq + 1) || memcmp(pr.val, eq + 1, pr.valn) : pr.val != NULL))
                break;
        }
        if (k != np || mailk_param_next(c.arg, c.argn, &pos, 0, &pr) != 0) {
            if (bad++ < 3)
                CHECK(0, "[rt-path] parameters misread at %zu of %zu it=%ld [%.*s]", k, np, it, (int)cmd.n, cmd.p);
        }
    }
    CHECK(bad == 0, "[rt-path] %d of %ld generated MAIL/RCPT commands misread", bad, N);
    dyn_sb_free(&loc);
    dyn_sb_free(&dom);
    dyn_sb_free(&mbox);
    dyn_sb_free(&cmd);
}

static void model_stuff(const uint8_t* p, size_t n, dyn_sb_t* o, uint32_t* bare)
{
    dyn_sb_t t;
    size_t i, ls = 0;
    dyn_sb_init(&t);
    *bare = 0;
    for (i = 0; i < n; i++) {
        if (p[i] == '\r' && i + 1 < n && p[i + 1] == '\n') {
            dyn_sb_put(&t, "\r\n", 2);
            i++;
        } else if (p[i] == '\r' || p[i] == '\n') {
            dyn_sb_put(&t, "\r\n", 2);
            ++*bare;
        } else
            dyn_sb_putc(&t, (char)p[i]);
    }
    if (t.n && !(t.n >= 2 && t.p[t.n - 2] == '\r' && t.p[t.n - 1] == '\n'))
        dyn_sb_put(&t, "\r\n", 2);
    for (i = 0; i < t.n; i++) {
        if (i == ls && t.p[i] == '.')
            dyn_sb_putc(o, '.');
        dyn_sb_putc(o, t.p[i]);
        if (t.p[i] == '\n')
            ls = i + 1;
    }
    dyn_sb_puts(o, ".\r\n");
    dyn_sb_free(&t);
}

static int model_unstuff(const uint8_t* p, size_t n, dyn_sb_t* o, size_t* end, uint32_t* bare)
{
    size_t ls = 0, i;
    *bare = 0;
    for (;;) {
        size_t e = ls;
        while (e + 1 < n && !(p[e] == '\r' && p[e + 1] == '\n'))
            e++;
        if (e + 1 >= n)
            break;
        if (e - ls == 1 && p[ls] == '.') {
            *end = e + 2;
            return MAILK_OK;
        }
        for (i = ls; i < e; i++)
            if ((p[i] == '\r' && !(i + 1 < n && p[i + 1] == '\n')) || (p[i] == '\n' && !(i && p[i - 1] == '\r')))
                ++*bare;
        dyn_sb_put(o, (const char*)p + ls + (p[ls] == '.' && e > ls), e - ls - (p[ls] == '.' && e > ls));
        dyn_sb_put(o, "\r\n", 2);
        ls = e + 2;
    }
    return MAILK_NEED;
}

static void test_diff_stuff(void)
{
    static const char al[] = "ab.\r\n..\r\n\r\n. x";
    uint8_t buf[600];
    dyn_sb_t got, want;
    long it, N = (long)fk_iters(20000);
    int bad_s = 0, bad_u = 0;
    fk_g = fk_stream("diff-stuff");
    dyn_sb_init(&got);
    dyn_sb_init(&want);
    for (it = 0; it < N; it++) {
        size_t n = rnd() % (rnd() % 10 ? 80 : 600), i, e = 0, we = 0;
        mailk_stuff_t s;
        mailk_unstuff_t u;
        uint32_t wb;
        int rc, wrc;
        for (i = 0; i < n; i++)
            buf[i] = rnd() % 13 ? (uint8_t)al[rnd() % (sizeof al - 1)] : (uint8_t)rnd();
        want.n = 0;
        model_stuff(buf, n, &want, &wb);
        rc = stuff_str((const char*)buf, n, 0, &got, 1 + (size_t)(rnd() % 17), &s);
        if ((rc != MAILK_OK || got.n != want.n || memcmp(got.p, want.p, want.n) || s.fixed_bare != wb) && bad_s++ < 3)
            CHECK(0, "[diff-stuff] dot-stuffing differs from the naive model it=%ld n=%zu bare %u/%u", it, n, s.fixed_bare, wb);
        want.n = 0;
        wrc = model_unstuff(buf, n, &want, &we, &wb);
        rc = unstuff_str((const char*)buf, n, 0, 0, &got, 1 + (size_t)(rnd() % 17), &u, &e);
        if (wrc == MAILK_OK ? (rc != MAILK_OK || e != we || got.n != want.n || memcmp(got.p, want.p, want.n) || u.bare != wb) : rc != MAILK_NEED) {
            if (bad_u++ < 3)
                CHECK(0, "[diff-unstuff] unstuffing differs from the naive model it=%ld n=%zu rc %d/%d end %zu/%zu bare %u/%u", it, n, rc, wrc, e, we, u.bare, wb);
        }
        rc = unstuff_str((const char*)buf, n, MAILK_F_STRICT, 0, &got, 1 + (size_t)(rnd() % 17), &u, &e);
        if (wrc == MAILK_OK && (wb ? rc != MAILK_EBARE : rc != MAILK_OK) && bad_u++ < 3)
            CHECK(0, "[diff-unstuff] strict mode rc %d with %u bare line ends it=%ld", rc, wb, it);
    }
    CHECK(bad_s == 0 && bad_u == 0, "[diff-stuff] %d stuff and %d unstuff disagreements with the naive model in %ld inputs", bad_s, bad_u, N);
    dyn_sb_free(&got);
    dyn_sb_free(&want);
}

static void test_rt_xtext(void)
{
    uint8_t buf[200];
    dyn_sb_t e, d, w;
    long it, N = (long)fk_iters(20000);
    int bad = 0;
    fk_g = fk_stream("rt-xtext");
    dyn_sb_init(&e);
    dyn_sb_init(&d);
    dyn_sb_init(&w);
    for (it = 0; it < N; it++) {
        size_t n = rnd() % sizeof buf, i;
        for (i = 0; i < n; i++)
            buf[i] = rnd() & 1 ? (uint8_t)"+= a~!\x7f"[rnd() % 7] : (uint8_t)rnd();
        e.n = d.n = w.n = 0;
        for (i = 0; i < n; i++) {
            if (buf[i] >= 33 && buf[i] <= 126 && buf[i] != '+' && buf[i] != '=')
                dyn_sb_putc(&w, (char)buf[i]);
            else {
                char h[4];
                snprintf(h, sizeof h, "+%02X", buf[i]);
                dyn_sb_puts(&w, h);
            }
        }
        if (mailk_xtext_encode(&e, buf, n) != MAILK_OK || e.n != w.n || (w.n && memcmp(e.p, w.p, w.n)) || mailk_xtext_decode(&d, e.p ? e.p : "", e.n) != MAILK_OK || d.n != n
            || (n && memcmp(d.p, buf, n))) {
            if (bad++ < 3)
                CHECK(0, "[rt-xtext] xtext differs from the RFC 3461 model it=%ld", it);
        }
    }
    CHECK(bad == 0, "[rt-xtext] %d of %ld random strings differ", bad, N);
    dyn_sb_free(&e);
    dyn_sb_free(&d);
    dyn_sb_free(&w);
}

static void path_of(dyn_sb_t* s, size_t l1, size_t l2, size_t l3)
{
    size_t k;
    s->n = 0;
    dyn_sb_putc(s, '<');
    for (k = 0; k < 64; k++)
        dyn_sb_putc(s, 'l');
    dyn_sb_putc(s, '@');
    for (k = 0; k < l1; k++)
        dyn_sb_putc(s, 'a');
    dyn_sb_putc(s, '.');
    for (k = 0; k < l2; k++)
        dyn_sb_putc(s, 'b');
    dyn_sb_putc(s, '.');
    for (k = 0; k < l3; k++)
        dyn_sb_putc(s, 'c');
    dyn_sb_puts(s, ">");
    dyn_sb_putc(s, 0);
}

static void test_bound(void)
{
    char b[600];
    mailk_path_t p;
    dyn_sb_t s;
    mailk_reply_t r;
    size_t k;
    dyn_sb_init(&s);
    memset(b, 0, sizeof b);
    for (k = 0; k < 255; k++)
        b[k] = (k % 64 == 63) ? '.' : 'd';
    CHECK(mailk_domain_ok(b, 255, 0), "[bound] a 255-octet domain is valid");
    b[255] = 'd';
    CHECK(!mailk_domain_ok(b, 256, 0), "[bound] a 256-octet domain is refused");
    memset(b, 'e', 63);
    memcpy(b + 63, ".x", 2);
    CHECK(mailk_domain_ok(b, 65, 0), "[bound] a 63-octet label is valid");
    memset(b, 'e', 64);
    memcpy(b + 64, ".x", 2);
    CHECK(!mailk_domain_ok(b, 66, 0), "[bound] a 64-octet label is refused");
    path_of(&s, 62, 62, 63);
    CHECK(s.n == 257 && path(s.p, 0, 0, &p) == MAILK_OK, "[bound] a 256-octet path (brackets included) is valid");
    path_of(&s, 62, 63, 63);
    CHECK(path(s.p, 0, 0, &p) == MAILK_ELONG, "[bound] a 257-octet path is ELONG");
    s.n = 0;
    for (k = 0; k < MAILK_REPLY_LINES_MAX - 1; k++)
        dyn_sb_puts(&s, "250-x\r\n");
    dyn_sb_puts(&s, "250 y\r\n");
    mailk_reply_init(&r);
    CHECK(reply_all(s.p, s.n, &r, 0) == MAILK_OK && r.nlines == MAILK_REPLY_LINES_MAX, "[bound] a reply of exactly 512 lines parses");
    s.n = 0;
    dyn_sb_puts(&s, "250 ");
    while (s.n < MAILK_REPLY_LINE_MAX)
        dyn_sb_putc(&s, 'z');
    dyn_sb_puts(&s, "\r\n");
    CHECK(reply_all(s.p, s.n, &r, 7) == MAILK_OK && r.text.n == MAILK_REPLY_LINE_MAX - 4, "[bound] a 4096-octet reply line (CRLF excluded) parses");
    s.n = 0;
    dyn_sb_puts(&s, "250 ");
    while (s.n < MAILK_REPLY_LINE_MAX + 1)
        dyn_sb_putc(&s, 'z');
    dyn_sb_puts(&s, "\r\n");
    CHECK(reply_all(s.p, s.n, &r, 7) == MAILK_ELONG, "[bound] a 4097-octet reply line is ELONG");
    mailk_reply_free(&r);
    dyn_sb_free(&s);
}

static void test_fuzz_entry(void)
{
    static const char* const seeds[] = { "\x01" "250-mx greets\r\n250-PIPELINING\r\n250 SIZE 100\r\n", "\x02" "FROM:<a@b.example> SIZE=10 BODY=8BITMIME",
        "\x02" "<\"q x\"@[IPv6:2001:db8::1]>", "\x05" "n,,n=user,r=fyko+d2lbbFgONRv9qkxdawL", "\x03" ".a\r\n..\r\nb\r\n.\r\n", "\x04" "a+2Bb+3D" };
    uint8_t buf[1024];
    long it, N = (long)fk_iters(20000);
    fk_g = fk_stream("core-fuzz-entry");
    printf("[fuzz-core] start\n");
    fflush(stdout);
    for (it = 0; it < N; it++) {
        const char* sd = seeds[rnd() % 6];
        size_t n = sd[0] ? strlen(sd) : 1 + strlen(sd + 1), k, m = 1 + rnd() % 4;
        memcpy(buf, sd, n);
        buf[0] = (uint8_t)rnd();
        for (k = 0; k < m && n > 2; k++) {
            size_t at = 1 + rnd() % (n - 1);
            switch (rnd() % 3) {
            case 0:
                buf[at] = rnd() & 1 ? (uint8_t)rnd() : (uint8_t)"\r\n.<>@:\"\\[]=+ 0"[rnd() % 16];
                break;
            case 1:
                memmove(buf + at, buf + at + 1, n - at - 1);
                n--;
                break;
            default:
                if (n * 2 < sizeof buf) {
                    size_t l = 1 + rnd() % (n - at);
                    memmove(buf + at + l, buf + at, n - at);
                    n += l;
                }
                break;
            }
        }
        LLVMFuzzerTestOneInput(buf, n);
    }
    printf("[fuzz-core] done\n");
    CHECK(1, "[fuzz-core] %ld mutated inputs through src/fuzz/fuzz_mailk_reply.c", N);
}

int main(void)
{
    fk_init("test_mailk_core");
    fflush(stdout);
    fk_g = fk_stream("fixed");
    test_line();
    test_reply();
    test_cmd();
    test_path();
    test_mail();
    test_encode();
    test_xtext();
    test_data();
    fk_g = fk_stream("fuzz");
    test_fuzz();
    test_rt_reply();
    test_rt_cmd();
    test_rt_path();
    test_diff_stuff();
    test_rt_xtext();
    test_bound();
    test_fuzz_entry();    printf("%d checks\n", checks);
    fk_fails += fails;
    return fk_done(__FILE__);
}
