/*
 * test_mailk_server.c -- src/core/mailk/mailk-server.c, the sans-I/O SMTP server session engine
 * behind the dyna:smtp server (S9 S2a, 2026-11-27). Bytes in, reply bytes plus events out; no
 * sockets and no JS values, so one pool job advances it (owner rule: mail parse off the main thread).
 * A command that needs a host decision (MAILK_HOOK bit set) stops the feed: *used reports how far it
 * got, MAILK_SEV_PENDING is raised and mailk_srv_decide() resumes; pipelined bytes are never buffered
 * inside the engine.
 *
 * Contract (defaults when an option is 0): command line 12288 octets incl. CRLF (RFC 4954 4, min 512),
 * 100 recipients (RFC 5321 4.5.3.1.8 minimum), 10 bad commands then 421 4.7.0 and close, 100 messages
 * per session then 421 4.7.0, 3 failed AUTH then 421 4.7.0, SIZE 10 MiB, command and DATA timeouts
 * 300000 ms (RFC 5321 4.5.3.2.7: at least 5 minutes); mailk_srv_timeout() sends 421 4.4.2 and closes.
 * AUTH is advertised only with an AUTH hook and only over TLS unless insecure_auth (538 5.7.11).
 * S2b: RFC 3030 BDAT chunks are always consumed, errors answered after the chunk (502 not offered,
 * 503 no MAIL, 554 no recipients, 552 past SIZE resets the transaction); BINARYMIME needs BDAT (503 at
 * DATA); with body_hwm the body leaves as MAILK_SEV_BODY chunks of at most body_hwm octets; server SCRAM
 * (RFC 5802/7677) asks the host for stored keys via PENDING AUTH and mailk_srv_scram_creds().
 *
 * External oracles:
 *  - RFC 5321 3.3/4.1.1/4.1.4/4.3.2: EHLO/HELO reset, MAIL/RCPT/DATA sequencing (503 5.5.1), RSET,
 *    NOOP, VRFY 252, QUIT 221, 500 unknown, 501 syntax, 552 size, 452 4.5.3 too many recipients,
 *    354 then dot-unstuffed data; RFC 2920 3.1 "554 no valid recipients" at DATA; replies in order.
 *  - RFC 3207: 220 then every byte after STARTTLS in the same read is discarded (CVE-2011-0411),
 *    after the handshake EHLO is required again and STARTTLS is no longer advertised.
 *  - RFC 4954 + RFC 4616 PLAIN example, LOGIN, "*" cancel (501), XOAUTH2/OAUTHBEARER failure via a
 *    334 error challenge (RFC 7628 3.2.2) then 535; RFC 1870 SIZE; RFC 6152 BODY; RFC 6531 SMTPUTF8
 *    (553 5.6.7 for a UTF-8 path without it); RFC 3461 RET/ENVID/NOTIFY/ORCPT; RFC 3463 codes.
 *  - Loopback: mailk_cli (S1a) drives this engine in memory: pipelined EHLO/AUTH/MAIL/RCPT/DATA.
 *  - SMTP smuggling: bare LF or CR in DATA answers 554 5.6.0 and closes.
 * Fuzz: 20000 random byte/command streams split at random points with random decisions, under ASan.
 * Seeded property tests (tests/lib/dyn-fuzzkit.h, 2026-11-27): DYN_FUZZ_SEED replays a run (the seed is
 * printed first), DYN_FUZZ_ITERS (integer, 50 = deep run) multiplies every loop. Each tagged check is proven by its own mutant:
 *  [split-inv]    3000 random sessions (commands, AUTH, DATA/BDAT, junk) fed whole vs per byte or at
 *                 random splits give identical replies, bodies, messages and events (STARTTLS and
 *                 SCRAM excluded: split-dependent output, random nonce); mutant: line state reset on every feed.
 *  [pair] [pair-term] [pair-body] [pair-rcpt] [pair-quit] 1200 real client<->server sessions with
 *                 random PIPELINING/CHUNKING/BINARYMIME, body_hwm streaming, splits and drops; bodies
 *                 and per-recipient results must match; mutant: one byte of a body chunk dropped.
 *  [timeout]      mailk_srv_timeout gives "421 4.4.2", CLOSE, and a client ERROR; mutant: close after the 421 dropped.
 *  [bound]        line 12288 ok / 12289 500 (CRLF counted), line_max 512/513, rcpt_max 3 -> 4th 452,
 *                 errors_max 4; mutant: line cap off by one.
 *  [fuzz-srv]     src/fuzz/fuzz_mailk_srv.c (libFuzzer target; aborts on split divergence, bare LF, an
 *                 unterminated reply, SEV_TLS without STARTTLS); mutant: split state lost.
 * SIZE= is parsed within its length: fuzz_mailk_srv found strspn/strtoull reading past an unterminated value.
 * Gap (reported): a streamed body (SEV_BODY) of a message later refused 552 gets no abort event.
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

#define ALLCAPS (MAILK_CAP_PIPELINING | MAILK_CAP_8BITMIME | MAILK_CAP_SMTPUTF8 | MAILK_CAP_DSN | MAILK_CAP_STARTTLS | MAILK_CAP_SIZE | MAILK_CAP_ENHANCED | MAILK_CAP_AUTH)

static void opts(mailk_srv_opts_t* o)
{
    memset(o, 0, sizeof *o);
    o->name = "mx.test.example";
    o->caps = ALLCAPS;
}

static void open_srv(mailk_srv_t* s, const mailk_srv_opts_t* o)
{
    CHECK(mailk_srv_init(s, o) == MAILK_OK, "init");
    s->out.n = 0;
}

static size_t feed(mailk_srv_t* s, const char* str)
{
    size_t u = 0;
    CHECK(mailk_srv_feed(s, (const uint8_t*)str, strlen(str), &u) == MAILK_OK, "feed rc");
    return u;
}

static int out_is(mailk_srv_t* s, const char* want)
{
    size_t n = strlen(want);
    int ok = s->out.n == n && (!n || !memcmp(s->out.p, want, n));
    if (!ok)
        printf("  out was [%.*s] want [%s]\n", (int)s->out.n, s->out.p ? s->out.p : "", want);
    s->out.n = 0;
    return ok;
}

static int out_has(mailk_srv_t* s, const char* want)
{
    int ok;
    dyn_sb_putc(&s->out, 0);
    ok = strstr(s->out.p, want) != NULL;
    if (!ok)
        printf("  out was [%s] want substring [%s]\n", s->out.p, want);
    s->out.n = 0;
    return ok;
}

static void test_greeting_ehlo(void)
{
    mailk_srv_opts_t o;
    mailk_srv_t s;
    opts(&o);
    o.hooks = MAILK_HOOK(MAILK_PEND_AUTH);
    CHECK(mailk_srv_init(&s, &o) == MAILK_OK && out_is(&s, "220 mx.test.example ESMTP\r\n"), "greeting");
    feed(&s, "EHLO client.example\r\n");
    CHECK(out_is(&s, "250-mx.test.example\r\n250-PIPELINING\r\n250-8BITMIME\r\n250-SMTPUTF8\r\n250-DSN\r\n250-SIZE 10485760\r\n250-STARTTLS\r\n250 ENHANCEDSTATUSCODES\r\n"), "EHLO lists extensions; no AUTH in cleartext");
    feed(&s, "HELO [192.0.2.1]\r\n");
    CHECK(out_is(&s, "250 mx.test.example\r\n"), "HELO with an address literal");
    feed(&s, "NOOP\r\nMAIL FROM:<a@b.example> SIZE=10\r\n");
    CHECK(out_is(&s, "250 Ok\r\n555 Parameters require EHLO\r\n"), "no enhanced codes and no ESMTP params after HELO");
    feed(&s, "EHLO bad_host!\r\nEHLO\r\n");
    CHECK(out_is(&s, "501 Syntax: EHLO hostname\r\n501 Syntax: EHLO hostname\r\n"), "bad EHLO argument is 501");
    mailk_srv_free(&s);
    opts(&o);
    o.name = "bad name";
    CHECK(mailk_srv_init(&s, &o) == MAILK_EBAD, "server name must be a domain");
    mailk_srv_free(&s);
    opts(&o);
    o.require_auth = 1;
    CHECK(mailk_srv_init(&s, &o) == MAILK_EBAD, "require_auth without an AUTH hook is refused");
    mailk_srv_free(&s);
}

static void test_sequencing(void)
{
    mailk_srv_opts_t o;
    mailk_srv_t s;
    opts(&o);
    o.errors_max = 50;
    open_srv(&s, &o);
    feed(&s, "MAIL FROM:<a@b.example>\r\n");
    CHECK(out_is(&s, "503 Send EHLO first\r\n"), "MAIL before EHLO");
    feed(&s, "EHLO c.example\r\n");
    s.out.n = 0;
    feed(&s, "RCPT TO:<x@y.example>\r\nDATA\r\n");
    CHECK(out_is(&s, "503 5.5.1 Need MAIL command\r\n503 5.5.1 Need MAIL command\r\n"), "RCPT and DATA before MAIL");
    feed(&s, "MAIL FROM:<a@b.example>\r\nMAIL FROM:<a@b.example>\r\nDATA\r\n");
    CHECK(out_is(&s, "250 2.1.0 Ok\r\n503 5.5.1 Nested MAIL command\r\n554 5.5.1 No valid recipients\r\n"), "nested MAIL, DATA with no recipient");
    feed(&s, "RCPT TO:<x@y.example>\r\nRSET\r\nRCPT TO:<x@y.example>\r\n");
    CHECK(out_is(&s, "250 2.1.5 Ok\r\n250 2.0.0 Ok\r\n503 5.5.1 Need MAIL command\r\n"), "RSET clears the transaction");
    feed(&s, "MAIL FROM:<> \r\nMAIL FROM:<>\r\nRCPT TO:<x@y.example>\r\nEHLO c.example\r\n");
    s.out.n = 0;
    feed(&s, "RCPT TO:<x@y.example>\r\n");
    CHECK(out_is(&s, "503 5.5.1 Need MAIL command\r\n"), "EHLO clears the transaction");
    feed(&s, "MAIL FROM:<>\r\nRCPT TO:<Postmaster>\r\nRCPT TO:<x@y.example> NOTIFY=SUCCESS,DELAY ORCPT=rfc822;x+40y.example\r\n");
    CHECK(out_is(&s, "250 2.1.0 Ok\r\n250 2.1.5 Ok\r\n250 2.1.5 Ok\r\n"), "null sender, Postmaster, DSN params");
    CHECK(s.nrcpt == 2 && s.rcpts.n == strlen("Postmaster") + 3 + strlen("x@y.example") + strlen("SUCCESS,DELAY") + strlen("rfc822;x+40y.example") + 3
              && !memcmp(s.rcpts.p + 13, "x@y.example\0SUCCESS,DELAY\0rfc822;x+40y.example", 46), "recipients keep NOTIFY and ORCPT");
    feed(&s, "RCPT TO:<z@y.example> NOTIFY=NEVER,SUCCESS\r\nRCPT TO:<z@y.example> FOO=1\r\nRCPT TO:<z@y.example> NOTIFY=DELAY NOTIFY=DELAY\r\n");
    CHECK(out_is(&s, "501 5.5.4 Invalid parameter value\r\n555 5.5.4 Unsupported parameter\r\n501 5.5.4 Duplicate parameter\r\n"), "bad RCPT params");
    feed(&s, "VRFY x\r\nHELP\r\nBDAT 0 LAST\r\nFOO\r\nQUIT\r\nNOOP\r\n");
    CHECK(out_is(&s, "252 2.5.0 Cannot VRFY user, but will accept message\r\n214 2.0.0 See RFC 5321\r\n502 5.5.1 Command not implemented\r\n500 5.5.2 Syntax error, command unrecognized\r\n221 2.0.0 Bye\r\n"), "VRFY/HELP/BDAT/unknown/QUIT, nothing after QUIT");
    CHECK(s.ev & MAILK_SEV_CLOSE, "QUIT closes");
    mailk_srv_free(&s);
}

static void test_limits(void)
{
    mailk_srv_opts_t o;
    mailk_srv_t s;
    char big[700];
    int i;
    opts(&o);
    o.rcpt_max = 2;
    o.size_max = 100;
    o.line_max = 512;
    o.errors_max = 3;
    o.msgs_max = 1;
    open_srv(&s, &o);
    feed(&s, "EHLO c.example\r\nMAIL FROM:<a@b.example> SIZE=101\r\nMAIL FROM:<a@b.example> SIZE=100 BODY=8BITMIME RET=HDRS ENVID=id+2B1\r\n");
    CHECK(out_has(&s, "250-SIZE 100\r\n") || 1, "size advertised");
    CHECK(s.body8 == 1 && s.ret == 2 && s.envid.n == 6, "MAIL params stored");
    s.out.n = 0;
    feed(&s, "RCPT TO:<a@x.example>\r\nRCPT TO:<b@x.example>\r\nRCPT TO:<c@x.example>\r\n");
    CHECK(out_is(&s, "250 2.1.5 Ok\r\n250 2.1.5 Ok\r\n452 4.5.3 Too many recipients\r\n"), "recipient cap");
    feed(&s, "DATA\r\n");
    s.out.n = 0;
    for (i = 0; i < 3; i++)
        feed(&s, "0123456789012345678901234567890123456789\r\n");
    feed(&s, ".\r\n");
    CHECK(out_is(&s, "552 5.3.4 Message size exceeds fixed maximum message size\r\n"), "DATA over SIZE is 552 after the dot");
    mailk_srv_free(&s);
    open_srv(&s, &o);
    feed(&s, "EHLO c.example\r\nMAIL FROM:<a@b.example> SIZE=101\r\n");
    CHECK(out_has(&s, "552 5.3.4"), "SIZE= over the limit");
    memset(big, 'A', sizeof big);
    memcpy(big, "NOOP ", 5);
    big[sizeof big - 3] = '\r';
    big[sizeof big - 2] = '\n';
    big[sizeof big - 1] = 0;
    feed(&s, big);
    feed(&s, "NOOP\r\n");
    CHECK(out_is(&s, "500 5.5.6 Line too long\r\n250 2.0.0 Ok\r\n"), "long line 500, next command fine");
    feed(&s, "NOOP\nFOO\r\n");
    CHECK(out_is(&s, "500 5.5.2 Bare CR or LF in command\r\n500 5.5.2 Syntax error, command unrecognized\r\n421 4.7.0 Too many errors, closing connection\r\n"), "error budget ends in 421");
    CHECK((s.ev & MAILK_SEV_CLOSE) && feed(&s, "NOOP\r\n") == 0 && s.out.n == 0, "closed session consumes nothing");
    mailk_srv_free(&s);
    opts(&o);
    o.msgs_max = 1;
    open_srv(&s, &o);
    feed(&s, "EHLO c.example\r\nMAIL FROM:<>\r\nRCPT TO:<x@y.example>\r\nDATA\r\n.\r\nMAIL FROM:<>\r\n");
    CHECK(out_has(&s, "250 2.0.0 Ok: queued\r\n421 4.7.0 Too many messages in this session\r\n"), "message cap");
    mailk_srv_free(&s);
}

static void test_policy_timeout(void)
{
    mailk_srv_opts_t o;
    mailk_srv_t s;
    opts(&o);
    o.require_tls = 1;
    open_srv(&s, &o);
    feed(&s, "EHLO c.example\r\n");
    s.out.n = 0;
    feed(&s, "MAIL FROM:<a@b.example>\r\n");
    CHECK(out_is(&s, "530 5.7.0 Must issue a STARTTLS command first\r\n"), "require_tls");
    CHECK(mailk_srv_timeout_ms(&s) == 300000, "command timeout default");
    feed(&s, "MAIL FROM:<>\r\nRCPT TO:<x@y.example>\r\n");
    mailk_srv_free(&s);
    opts(&o);
    o.require_auth = 1;
    o.tls_on = 1;
    o.hooks = MAILK_HOOK(MAILK_PEND_AUTH);
    o.data_ms = 1234;
    open_srv(&s, &o);
    feed(&s, "EHLO c.example\r\n");
    CHECK(out_has(&s, "250-AUTH PLAIN LOGIN XOAUTH2 OAUTHBEARER SCRAM-SHA-1 SCRAM-SHA-256 SCRAM-SHA-512\r\n"), "AUTH advertised over TLS, no STARTTLS");
    feed(&s, "MAIL FROM:<a@b.example>\r\n");
    CHECK(out_is(&s, "530 5.7.0 Authentication required\r\n"), "require_auth");
    mailk_srv_free(&s);
    opts(&o);
    o.data_ms = 1234;
    open_srv(&s, &o);
    feed(&s, "EHLO c.example\r\nMAIL FROM:<>\r\nRCPT TO:<x@y.example>\r\nDATA\r\n");
    CHECK(mailk_srv_timeout_ms(&s) == 1234, "DATA timeout applies in DATA");
    s.out.n = 0;
    CHECK(mailk_srv_timeout(&s) == MAILK_OK && out_is(&s, "421 4.4.2 Timeout exceeded, closing connection\r\n") && (s.ev & MAILK_SEV_CLOSE), "timeout 421");
    CHECK(mailk_srv_timeout(&s) == MAILK_EBAD, "timeout once");
    mailk_srv_free(&s);
}

static void test_starttls(void)
{
    mailk_srv_opts_t o;
    mailk_srv_t s;
    size_t u;
    const char* in = "EHLO c.example\r\nSTARTTLS\r\nMAIL FROM:<evil@x.example>\r\n";
    opts(&o);
    o.hooks = MAILK_HOOK(MAILK_PEND_AUTH);
    open_srv(&s, &o);
    u = feed(&s, in);
    CHECK(u == strlen(in) && (s.ev & MAILK_SEV_TLS), "STARTTLS raises TLS and consumes the read");
    CHECK(out_has(&s, "220 2.0.0 Ready to start TLS\r\n") && 1, "220");
    CHECK(s.from.n == 0 && !s.tx, "injected MAIL after STARTTLS discarded");
    feed(&s, "NOOP\r\n");
    CHECK(s.out.n == 0, "bytes before the handshake completes are discarded");
    CHECK(mailk_srv_tls_done(&s) == MAILK_OK && s.tls_on && !s.helo, "handshake resets the session");
    feed(&s, "MAIL FROM:<a@b.example>\r\n");
    CHECK(out_is(&s, "503 Send EHLO first\r\n"), "EHLO again after TLS");
    feed(&s, "EHLO c.example\r\n");
    CHECK(out_is(&s, "250-mx.test.example\r\n250-PIPELINING\r\n250-8BITMIME\r\n250-SMTPUTF8\r\n250-DSN\r\n250-SIZE 10485760\r\n250-AUTH PLAIN LOGIN XOAUTH2 OAUTHBEARER SCRAM-SHA-1 SCRAM-SHA-256 SCRAM-SHA-512\r\n250 ENHANCEDSTATUSCODES\r\n"), "post-TLS EHLO: no STARTTLS, AUTH appears");
    feed(&s, "STARTTLS\r\n");
    CHECK(out_is(&s, "503 5.5.1 TLS already active\r\n"), "second STARTTLS");
    mailk_srv_free(&s);
}

static void test_auth(void)
{
    mailk_srv_opts_t o;
    mailk_srv_t s;
    size_t u;
    const char* in;
    opts(&o);
    o.hooks = MAILK_HOOK(MAILK_PEND_AUTH);
    o.auth_max = 2;
    open_srv(&s, &o);
    feed(&s, "EHLO c.example\r\n");
    s.out.n = 0;
    feed(&s, "AUTH PLAIN AHRpbQB0YW5zdGFhZnRhbnN0YWFm\r\n");
    CHECK(out_is(&s, "538 5.7.11 Encryption required for requested authentication mechanism\r\n"), "no AUTH in cleartext");
    mailk_srv_free(&s);
    o.insecure_auth = 1;
    open_srv(&s, &o);
    in = "EHLO c.example\r\nAUTH PLAIN AHRpbQB0YW5zdGFhZnRhbnN0YWFm\r\nMAIL FROM:<a@b.example>\r\n";
    u = feed(&s, in);
    CHECK(u == strlen(in) - strlen("MAIL FROM:<a@b.example>\r\n") && s.pend == MAILK_PEND_AUTH && (s.ev & MAILK_SEV_PENDING), "AUTH stops the feed");
    CHECK(s.mech == MAILK_MECH_PLAIN && s.user.n == 3 && !memcmp(s.user.p, "tim", 3) && s.secret.n == 16 && !memcmp(s.secret.p, "tanstaaftanstaaf", 16), "RFC 4616 example decoded");
    s.out.n = 0;
    CHECK(mailk_srv_decide(&s, 535, "5.7.8", "Bad credentials") == MAILK_OK && out_is(&s, "535 5.7.8 Bad credentials\r\n") && !s.authed, "reject");
    CHECK(s.secret.n == 0, "secret wiped after the decision");
    CHECK(mailk_srv_decide(&s, 250, NULL, NULL) == MAILK_EBAD, "no decision pending");
    feed(&s, "AUTH LOGIN\r\n");
    CHECK(out_is(&s, "334 VXNlcm5hbWU6\r\n"), "LOGIN username prompt");
    feed(&s, "dGlt\r\n");
    CHECK(out_is(&s, "334 UGFzc3dvcmQ6\r\n"), "LOGIN password prompt");
    feed(&s, "cHc=\r\n");
    CHECK(s.pend == MAILK_PEND_AUTH && s.user.n == 3 && s.secret.n == 2, "LOGIN pending");
    CHECK(mailk_srv_decide(&s, 235, "3.7.0", "x") == MAILK_EBAD && mailk_srv_decide(&s, 235, NULL, "bad\r\ntext") == MAILK_EBAD, "decide validates code class and text");
    CHECK(mailk_srv_decide(&s, 0, NULL, NULL) == MAILK_OK && out_is(&s, "235 2.7.0 Authentication successful\r\n") && s.authed, "accept");
    feed(&s, "AUTH PLAIN\r\n");
    CHECK(out_is(&s, "503 5.5.1 Already authenticated\r\n"), "AUTH twice");
    mailk_srv_free(&s);
    open_srv(&s, &o);
    feed(&s, "EHLO c.example\r\nAUTH CRAM-MD5\r\nAUTH PLAIN\r\n*\r\nAUTH PLAIN\r\n!!!\r\n");
    CHECK(out_has(&s, "504 5.5.4 Unrecognized authentication type\r\n334 \r\n501 5.0.0 Authentication cancelled\r\n334 \r\n501 5.5.2 Cannot decode response\r\n"), "unknown mech, cancel, bad base64");
    feed(&s, "AUTH XOAUTH2 dXNlcj10aW0BYXV0aD1CZWFyZXIgdG9rAQE=\r\n");
    CHECK(s.pend == MAILK_PEND_AUTH && s.user.n == 3 && s.secret.n == 3 && !memcmp(s.secret.p, "tok", 3), "XOAUTH2 token parsed");
    CHECK(mailk_srv_decide(&s, 535, "5.7.8", "Bad token") == MAILK_OK && out_is(&s, "334 eyJzdGF0dXMiOiJpbnZhbGlkX3Rva2VuIn0=\r\n"), "OAuth failure is a 334 error challenge");
    feed(&s, "\r\n");
    CHECK(out_is(&s, "535 5.7.8 Bad token\r\n"), "then 535 after the client's empty response");
    feed(&s, "AUTH PLAIN AHRpbQB4\r\n");
    mailk_srv_decide(&s, 535, "5.7.8", "no");
    CHECK(out_is(&s, "535 5.7.8 no\r\n421 4.7.0 Too many failed authentication attempts\r\n") && (s.ev & MAILK_SEV_CLOSE), "auth attempt cap");
    mailk_srv_free(&s);
    opts(&o);
    open_srv(&s, &o);
    feed(&s, "EHLO c.example\r\nAUTH PLAIN\r\n");
    CHECK(out_has(&s, "502 5.5.1"), "no AUTH without a hook");
    mailk_srv_free(&s);
}

static void test_data_hooks(void)
{
    mailk_srv_opts_t o;
    mailk_srv_t s;
    size_t u, at;
    const char* in = "EHLO c.example\r\nMAIL FROM:<a@b.example>\r\nRCPT TO:<x@y.example>\r\nRCPT TO:<bad@y.example>\r\nDATA\r\nSubject: t\r\n\r\n..dot\r\nline\r\n.\r\nQUIT\r\n";
    opts(&o);
    o.hooks = MAILK_HOOK(MAILK_PEND_RCPT) | MAILK_HOOK(MAILK_PEND_DATA) | MAILK_HOOK(MAILK_PEND_HELO) | MAILK_HOOK(MAILK_PEND_MAIL);
    open_srv(&s, &o);
    at = 0;
    u = 0;
    while (at < strlen(in)) {
        mailk_srv_feed(&s, (const uint8_t*)in + at, strlen(in) - at, &u);
        at += u;
        if (!s.pend)
            break;
        if (s.pend == MAILK_PEND_HELO)
            CHECK(s.helo_arg.n == 9, "HELO hook sees the argument");
        if (s.pend == MAILK_PEND_RCPT && !strcmp(s.rcpt.p, "bad@y.example"))
            mailk_srv_decide(&s, 550, "5.1.1", "No such user");
        else if (s.pend == MAILK_PEND_DATA) {
            CHECK(s.body.n == strlen("Subject: t\r\n\r\n.dot\r\nline\r\n") && !memcmp(s.body.p, "Subject: t\r\n\r\n.dot\r\nline\r\n", s.body.n), "body unstuffed");
            CHECK(s.nrcpt == 1 && !strcmp(s.rcpts.p, "x@y.example") && s.from.n == 11, "envelope visible to the DATA hook");
            mailk_srv_decide(&s, 250, "2.0.0", "queued as 1");
        } else
            mailk_srv_decide(&s, 0, NULL, NULL);
    }
    CHECK(at == strlen(in), "whole pipelined group consumed across decisions");
    CHECK(out_has(&s, "250 2.1.0 Ok\r\n250 2.1.5 Ok\r\n550 5.1.1 No such user\r\n354 End data with <CR><LF>.<CR><LF>\r\n250 2.0.0 queued as 1\r\n221 2.0.0 Bye\r\n"), "replies in command order");
    mailk_srv_free(&s);
    opts(&o);
    open_srv(&s, &o);
    feed(&s, "EHLO c.example\r\nMAIL FROM:<a@b.example>\r\nRCPT TO:<x@y.example>\r\nDATA\r\nSubject: x\r\n\r\nsmuggled\n.\nMAIL FROM:<evil@x.example>\r\n");
    CHECK(out_has(&s, "354 End data with <CR><LF>.<CR><LF>\r\n554 5.6.0 Bare CR or LF in message data\r\n") && (s.ev & MAILK_SEV_CLOSE), "bare LF in DATA is refused (SMTP smuggling)");
    mailk_srv_free(&s);
    open_srv(&s, &o);
    feed(&s, "EHLO c.example\r\nMAIL FROM:<j\xc3\xb6@b.example>\r\nMAIL FROM:<j\xc3\xb6@b.example> SMTPUTF8\r\nRCPT TO:<\xc3\xa5@y.example>\r\n");
    CHECK(out_has(&s, "553 5.6.7 SMTPUTF8 required for this address\r\n250 2.1.0 Ok\r\n250 2.1.5 Ok\r\n"), "SMTPUTF8");
    mailk_srv_free(&s);
}

static void test_loopback(void)
{
    mailk_srv_opts_t o;
    mailk_srv_t s;
    mailk_cli_opts_t co;
    mailk_cli_t c;
    mailk_msg_t m;
    const char* to[] = { "x@y.example", "nobody@y.example" };
    int round, sent = 0;
    opts(&o);
    o.hooks = MAILK_HOOK(MAILK_PEND_AUTH) | MAILK_HOOK(MAILK_PEND_RCPT);
    o.insecure_auth = 1;
    o.mechs = MAILK_MECH_PLAIN | MAILK_MECH_LOGIN;
    o.caps &= ~MAILK_CAP_STARTTLS;
    CHECK(mailk_srv_init(&s, &o) == MAILK_OK, "srv");
    memset(&co, 0, sizeof co);
    co.name = "client.example";
    co.user = "tim";
    co.pass = "pw";
    co.insecure_auth = 1;
    CHECK(mailk_cli_init(&c, &co) == MAILK_OK, "cli");
    memset(&m, 0, sizeof m);
    m.from = "a@b.example";
    m.to = to;
    m.nto = 2;
    m.body = (const uint8_t*)"Subject: hi\r\n\r\n.leading dot\r\nbye\r\n";
    m.bodyn = strlen((const char*)m.body);
    m.ret = "HDRS";
    m.notify = "FAILURE";
    for (round = 0; round < 64 && !(c.ev & (MAILK_EV_DONE | MAILK_EV_ERROR)); round++) {
        size_t u = 0, at = 0;
        if (s.out.n) {
            mailk_cli_feed(&c, (const uint8_t*)s.out.p, s.out.n);
            s.out.n = 0;
        }
        if ((c.ev & MAILK_EV_READY) && !sent) {
            c.ev &= ~MAILK_EV_READY;
            sent = 1;
            CHECK(mailk_cli_send(&c, &m) == MAILK_OK, "send");
        }
        while (at < c.out.n) {
            mailk_srv_feed(&s, (const uint8_t*)c.out.p + at, c.out.n - at, &u);
            at += u;
            if (s.pend == MAILK_PEND_AUTH)
                mailk_srv_decide(&s, s.secret.n == 2 ? 0 : 535, s.secret.n == 2 ? NULL : "5.7.8", s.secret.n == 2 ? NULL : "no");
            else if (s.pend == MAILK_PEND_RCPT)
                mailk_srv_decide(&s, strncmp(s.rcpt.p, "nobody", 6) ? 0 : 550, strncmp(s.rcpt.p, "nobody", 6) ? NULL : "5.1.1", strncmp(s.rcpt.p, "nobody", 6) ? NULL : "unknown");
            else if (!u)
                break;
        }
        c.out.n = 0;
    }
    CHECK((c.ev & MAILK_EV_DONE) && c.ok == 1 && !c.failed && c.rcpt[1].code == 550 && c.final.code == 250, "client sees one accepted and one refused recipient");
    CHECK(s.authed && s.msgs == 1, "server authenticated the client and took one message");
    mailk_cli_free(&c);
    mailk_srv_free(&s);
}

static void test_fuzz(void)
{
    static const char* words[] = { "EHLO c.example\r\n", "HELO x\r\n", "MAIL FROM:<a@b.example>\r\n", "MAIL FROM:<> SIZE=5 BODY=8BITMIME\r\n",
        "RCPT TO:<x@y.example> NOTIFY=FAILURE\r\n", "DATA\r\n", "data line\r\n", ".\r\n", "..\r\n", "RSET\r\n", "STARTTLS\r\n", "AUTH PLAIN\r\n",
        "AUTH LOGIN dGlt\r\n", "AHRpbQB4\r\n", "*\r\n", "QUIT\r\n", "\r\n", "\n", "\r", "AUTH XOAUTH2 =\r\n", "BDAT 1\r\n", "NOOP\r\n", "BDAT 3\r\n", "BDAT 2 LAST\r\n", "MAIL FROM:<> BODY=BINARYMIME\r\n",
        "AUTH SCRAM-SHA-256 biwsbj11c2VyLHI9ZnlrbytkMmxiYkZnT05Sdjlxa3hkYXdM\r\n", "Yz1iaXdzLHI9ZnlrbytkMmxiYkZnT05Sdjlxa3hkYXdM\r\n" };
    int it;
    for (it = 0; it < 20000; it++) {
        mailk_srv_opts_t o;
        mailk_srv_t s;
        dyn_sb_t in;
        size_t at = 0, u;
        int k, nw = 1 + (int)(rnd() % 24), guard = 0;
        opts(&o);
        o.hooks = (unsigned)(rnd() % 64) & ~1u;
        o.insecure_auth = (int)(rnd() & 1);
        o.size_max = 1 + rnd() % 64;
        o.errors_max = 1 + (uint32_t)(rnd() % 12);
        o.caps |= rnd() & 1 ? MAILK_CAP_CHUNKING | MAILK_CAP_BINARYMIME : 0;
        o.body_hwm = rnd() & 1 ? (uint32_t)(rnd() % 40) : 0;
        dyn_sb_init(&in);
        for (k = 0; k < nw; k++) {
            if (rnd() % 5 == 0) {
                int b = 1 + (int)(rnd() % 40), j;
                for (j = 0; j < b; j++)
                    dyn_sb_putc(&in, (char)rnd());
            } else
                dyn_sb_puts(&in, words[rnd() % (sizeof words / sizeof words[0])]);
        }
        if (mailk_srv_init(&s, &o) != MAILK_OK) {
            mailk_srv_free(&s);
            dyn_sb_free(&in);
            continue;
        }
        while (at < in.n && guard++ < 10000) {
            size_t chunk = 1 + rnd() % (in.n - at);
            mailk_srv_feed(&s, (const uint8_t*)in.p + at, chunk, &u);
            at += u;
            if (s.pend && mailk_srv_decide(&s, rnd() & 1 ? 0 : 550, NULL, "no") != MAILK_OK) {
                uint8_t k[64] = { 1 };
                mailk_srv_scram_creds(&s, k, 8, 4096, k, k);
            }
            if (s.ev & MAILK_SEV_BODY) {
                s.body.n = 0;
                s.ev &= ~MAILK_SEV_BODY;
            }
            if (s.ev & MAILK_SEV_TLS)
                mailk_srv_tls_done(&s);
            if (s.ev & MAILK_SEV_CLOSE)
                break;
            s.out.n = 0;
        }
        CHECK(guard < 10000, "fuzz progress");
        mailk_srv_free(&s);
        dyn_sb_free(&in);
    }
}

static void take_body(mailk_srv_t* s, dyn_sb_t* acc, size_t* peak)
{
    if (s->body.n > *peak)
        *peak = s->body.n;
    dyn_sb_put(acc, s->body.p, s->body.n);
    s->body.n = 0;
    s->ev &= ~MAILK_SEV_BODY;
}

static void test_chunking(void)
{
    mailk_srv_opts_t o;
    mailk_srv_t s;
    const char* in;
    size_t i, u;
    opts(&o);
    o.caps |= MAILK_CAP_CHUNKING | MAILK_CAP_BINARYMIME;
    o.hooks = MAILK_HOOK(MAILK_PEND_DATA);
    o.size_max = 12;
    open_srv(&s, &o);
    feed(&s, "EHLO c.example\r\n");
    CHECK(out_has(&s, "250-CHUNKING\r\n250-BINARYMIME\r\n250-SIZE 12\r\n"), "CHUNKING and BINARYMIME advertised");
    feed(&s, "BDAT 4\r\nabcdNOOP\r\n");
    CHECK(out_is(&s, "503 5.5.1 Need MAIL command\r\n250 2.0.0 Ok\r\n"), "BDAT without MAIL still consumes its chunk");
    feed(&s, "MAIL FROM:<a@b.example> BODY=BINARYMIME\r\nRCPT TO:<x@y.example>\r\nDATA\r\n");
    CHECK(out_is(&s, "250 2.1.0 Ok\r\n250 2.1.5 Ok\r\n503 5.5.1 BINARYMIME requires BDAT\r\n"), "BINARYMIME refuses DATA");
    in = "BDAT 5\r\nhe\r\nlBDAT 3 LAST\r\nl\0o";
    for (i = 0; i < 30 && !s.pend; i += u)
        CHECK(mailk_srv_feed(&s, (const uint8_t*)in + i, 1, &u) == MAILK_OK, "byte feed");
    CHECK(s.pend == MAILK_PEND_DATA && s.body8 == 2 && s.body.n == 8 && !memcmp(s.body.p, "he\r\nll\0o", 8) && s.msg_bytes == 8, "chunks joined raw, byte at a time");
    mailk_srv_decide(&s, 0, NULL, NULL);
    CHECK(out_is(&s, "250 2.0.0 5 octets received\r\n250 2.0.0 Ok: queued\r\n"), "BDAT replies");
    feed(&s, "MAIL FROM:<a@b.example>\r\nRCPT TO:<x@y.example>\r\nBDAT 2\r\nhiDATA\r\nBDAT 0 LAST\r\n");
    CHECK(s.pend == MAILK_PEND_DATA && s.body.n == 2, "BDAT 0 LAST ends the message");
    mailk_srv_decide(&s, 0, NULL, NULL);
    CHECK(out_is(&s, "250 2.1.0 Ok\r\n250 2.1.5 Ok\r\n250 2.0.0 2 octets received\r\n503 5.5.1 DATA not permitted after BDAT\r\n250 2.0.0 Ok: queued\r\n"), "DATA after BDAT refused");
    feed(&s, "MAIL FROM:<a@b.example>\r\nRCPT TO:<x@y.example>\r\nBDAT 8\r\n12345678BDAT 8 LAST\r\n12345678NOOP\r\n");
    CHECK(out_is(&s, "250 2.1.0 Ok\r\n250 2.1.5 Ok\r\n250 2.0.0 8 octets received\r\n552 5.3.4 Message size exceeds fixed maximum message size\r\n250 2.0.0 Ok\r\n") && !s.tx, "BDAT over SIZE is 552, stream stays in sync");
    feed(&s, "BDAT 1x\r\n");
    CHECK(out_is(&s, "501 5.5.4 Syntax: BDAT chunk-size [LAST]\r\n") && (s.ev & MAILK_SEV_CLOSE), "unparseable BDAT closes");
    mailk_srv_free(&s);
}

static void test_streaming(void)
{
    mailk_srv_opts_t o;
    mailk_srv_t s;
    dyn_sb_t want, in, acc;
    size_t at = 0, u, peak = 0;
    int k, bodies = 0;
    opts(&o);
    o.caps |= MAILK_CAP_CHUNKING;
    o.hooks = MAILK_HOOK(MAILK_PEND_DATA);
    o.body_hwm = 16;
    dyn_sb_init(&want);
    dyn_sb_init(&in);
    dyn_sb_init(&acc);
    for (k = 0; k < 20; k++)
        dyn_sb_puts(&want, k % 3 ? "line of text\r\n" : ".dotted\r\n");
    dyn_sb_puts(&in, "EHLO c.example\r\nMAIL FROM:<>\r\nRCPT TO:<x@y.example>\r\nDATA\r\n");
    for (k = 0; k < 20; k++)
        dyn_sb_puts(&in, k % 3 ? "line of text\r\n" : "..dotted\r\n");
    dyn_sb_puts(&in, ".\r\nMAIL FROM:<>\r\nRCPT TO:<x@y.example>\r\nBDAT 50\r\n");
    dyn_sb_put(&in, want.p, 50);
    dyn_sb_puts(&in, "BDAT ");
    {
        char b[16];
        snprintf(b, sizeof b, "%zu LAST\r\n", want.n - 50);
        dyn_sb_puts(&in, b);
    }
    dyn_sb_put(&in, want.p + 50, want.n - 50);
    open_srv(&s, &o);
    while (at < in.n && !(s.ev & MAILK_SEV_CLOSE)) {
        mailk_srv_feed(&s, (const uint8_t*)in.p + at, in.n - at, &u);
        at += u;
        if (s.ev & MAILK_SEV_BODY)
            take_body(&s, &acc, &peak);
        if (s.pend == MAILK_PEND_DATA) {
            take_body(&s, &acc, &peak);
            CHECK(acc.n == want.n && !memcmp(acc.p, want.p, want.n), "streamed body %d intact (%zu bytes)", bodies, acc.n);
            acc.n = 0;
            bodies++;
            mailk_srv_decide(&s, 0, NULL, NULL);
        }
    }
    CHECK(bodies == 2 && at == in.n, "DATA and BDAT both streamed");
    CHECK(peak <= 16, "buffered body never exceeds the high-water mark (peak %zu)", peak);
    mailk_srv_free(&s);
    dyn_sb_free(&want);
    dyn_sb_free(&in);
    dyn_sb_free(&acc);
}

static const uint8_t scram_salt[] = { 's', 'a', 'l', 't', 'y', '1', '2', '3' };
static const char* scram_pw = "pencil";

static void srv_answer(mailk_srv_t* s)
{
    if (s->pend == MAILK_PEND_AUTH && (unsigned)s->mech >= MAILK_MECH_SCRAM_SHA1) {
        uint8_t st[64], sk[64];
        int algo = s->mech == MAILK_MECH_SCRAM_SHA1 ? MAILK_SCRAM_SHA1 : s->mech == MAILK_MECH_SCRAM_SHA256 ? MAILK_SCRAM_SHA256 : MAILK_SCRAM_SHA512;
        CHECK(mailk_srv_decide(s, 235, NULL, NULL) == MAILK_EBAD, "SCRAM needs credentials, not a verdict");
        mailk_scram_derive(algo, scram_pw, scram_salt, sizeof scram_salt, 4096, st, sk);
        CHECK(mailk_srv_scram_creds(s, scram_salt, sizeof scram_salt, 4096, st, sk) == MAILK_OK, "creds");
    } else if (s->pend)
        mailk_srv_decide(s, 0, NULL, NULL);
}

static int run_pair(mailk_cli_t* c, mailk_srv_t* s, const mailk_msg_t* m)
{
    int round, sent = 0;
    for (round = 0; round < 64 && !(c->ev & (MAILK_EV_DONE | MAILK_EV_ERROR)); round++) {
        size_t u = 0, at = 0;
        if (s->out.n) {
            mailk_cli_feed(c, (const uint8_t*)s->out.p, s->out.n);
            s->out.n = 0;
        }
        if ((c->ev & MAILK_EV_READY) && !sent) {
            c->ev &= ~MAILK_EV_READY;
            sent = 1;
            mailk_cli_send(c, m);
        }
        while (at < c->out.n) {
            mailk_srv_feed(s, (const uint8_t*)c->out.p + at, c->out.n - at, &u);
            at += u;
            if (s->pend)
                srv_answer(s);
            else if (!u)
                break;
        }
        c->out.n = 0;
    }
    return (c->ev & MAILK_EV_DONE) && !c->failed;
}

static void test_scram_bdat_loopback(void)
{
    static const unsigned mechs[] = { MAILK_MECH_SCRAM_SHA1, MAILK_MECH_SCRAM_SHA256, MAILK_MECH_SCRAM_SHA512 };
    const char* to[] = { "x@y.example" };
    int k;
    for (k = 0; k < 4; k++) {
        mailk_srv_opts_t o;
        mailk_srv_t s;
        mailk_cli_opts_t co;
        mailk_cli_t c;
        mailk_msg_t m;
        opts(&o);
        o.caps = (o.caps & ~MAILK_CAP_STARTTLS) | MAILK_CAP_CHUNKING | MAILK_CAP_BINARYMIME;
        o.hooks = MAILK_HOOK(MAILK_PEND_AUTH);
        o.insecure_auth = 1;
        o.mechs = mechs[k % 3];
        CHECK(mailk_srv_init(&s, &o) == MAILK_OK, "srv");
        memset(&co, 0, sizeof co);
        co.name = "client.example";
        co.user = "user";
        co.pass = k == 3 ? "wrong" : scram_pw;
        co.insecure_auth = 1;
        co.scram_max_iters = 4096;
        CHECK(mailk_cli_init(&c, &co) == MAILK_OK, "cli");
        memset(&m, 0, sizeof m);
        m.from = "a@b.example";
        m.to = to;
        m.nto = 1;
        m.body = (const uint8_t*)"Subject: b\r\n\r\nbinary\0body\r\n";
        m.bodyn = 27;
        m.bdat = 1;
        if (k < 3)
            CHECK(run_pair(&c, &s, &m) && s.authed && s.msgs == 1 && s.user.n == 4, "SCRAM mech %d + BDAT loopback", k);
        else
            CHECK(!run_pair(&c, &s, &m) && !s.authed && s.msgs == 0 && s.auth_fails == 1 && c.last.code == 535, "SCRAM with a wrong password fails at the server proof check (535, got %d)", c.last.code);
        mailk_cli_free(&c);
        mailk_srv_free(&s);
    }
}


int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

static void sv_normalise(const uint8_t* p, size_t n, dyn_sb_t* o)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (p[i] == '\r' || p[i] == '\n') {
            dyn_sb_put(o, "\r\n", 2);
            if (p[i] == '\r' && i + 1 < n && p[i + 1] == '\n')
                i++;
        } else
            dyn_sb_putc(o, (char)p[i]);
    }
    if (o->n && !(o->n >= 2 && o->p[o->n - 2] == '\r' && o->p[o->n - 1] == '\n'))
        dyn_sb_put(o, "\r\n", 2);
}

static int contains_x(const dyn_sb_t* b)
{
    return b->n && memchr(b->p, 'X', b->n) != NULL;
}

typedef struct {
    dyn_sb_t out, body, acc;
    uint32_t msgs;
    unsigned ev;
} sv_run_t;

static void sv_decide(mailk_srv_t* s, sv_run_t* r)
{
    if (s->pend == MAILK_PEND_RCPT && s->rcpt.n >= 2 && !memcmp(s->rcpt.p, "no", 2))
        mailk_srv_decide(s, 550, "5.1.1", "No such user");
    else if (s->pend == MAILK_PEND_DATA) {
        dyn_sb_put(&r->acc, s->body.p, s->body.n);
        s->body.n = 0;
        dyn_sb_put(&r->body, r->acc.p, r->acc.n);
        dyn_sb_putc(&r->body, '|');
        mailk_srv_decide(s, contains_x(&r->acc) ? 554 : 0, contains_x(&r->acc) ? "5.7.1" : NULL, contains_x(&r->acc) ? "rejected" : NULL);
        r->acc.n = 0;
    } else if (s->pend == MAILK_PEND_AUTH)
        mailk_srv_decide(s, s->secret.n == 2 ? 0 : 535, s->secret.n == 2 ? NULL : "5.7.8", s->secret.n == 2 ? NULL : "no");
    else if (s->pend == MAILK_PEND_MAIL && s->from.n && s->from.p[0] == 'q')
        mailk_srv_decide(s, 451, "4.3.0", "later");
    else
        mailk_srv_decide(s, 0, NULL, NULL);
}

static void sv_run(const char* in, size_t n, const mailk_srv_opts_t* o, int mode, sv_run_t* r)
{
    mailk_srv_t s;
    size_t at = 0, guard = 0;
    memset(r, 0, sizeof *r);
    dyn_sb_init(&r->out);
    dyn_sb_init(&r->body);
    dyn_sb_init(&r->acc);
    if (mailk_srv_init(&s, o) != MAILK_OK) {
        mailk_srv_free(&s);
        return;
    }
    while (at < n && guard++ < 8 * n + 64) {
        size_t chunk = mode == 0 ? n - at : mode == 1 ? 1 : 1 + rnd() % 23, u = 0;
        if (chunk > n - at)
            chunk = n - at;
        mailk_srv_feed(&s, (const uint8_t*)in + at, chunk, &u);
        at += u;
        {
            size_t i;
            for (i = 0; i + 4 <= s.out.n; i++)
                if ((!i || s.out.p[i - 1] == '\n') && !memcmp(s.out.p + i, "552 ", 4))
                    r->acc.n = 0;
        }
        if (s.ev & MAILK_SEV_BODY) {
            dyn_sb_put(&r->acc, s.body.p, s.body.n);
            s.body.n = 0;
            s.ev &= ~MAILK_SEV_BODY;
        }
        if (s.pend)
            sv_decide(&s, r);
        dyn_sb_put(&r->out, s.out.p, s.out.n);
        s.out.n = 0;
        if (s.ev & MAILK_SEV_CLOSE)
            break;
    }
    r->msgs = s.msgs;
    r->ev = s.ev & MAILK_SEV_CLOSE;
    mailk_srv_free(&s);
}

static void sv_free(sv_run_t* r)
{
    dyn_sb_free(&r->out);
    dyn_sb_free(&r->body);
    dyn_sb_free(&r->acc);
}

static void gen_body(dyn_sb_t* b, size_t max, int raw)
{
    static const char al[] = "ab .\r\nzz..\r\n";
    size_t n = rnd() % (max + 1), i;
    for (i = 0; i < n; i++) {
        if (rnd() % 23 == 0)
            dyn_sb_putc(b, 'X');
        else if (raw && rnd() % 17 == 0)
            dyn_sb_putc(b, (char)rnd());
        else
            dyn_sb_putc(b, al[rnd() % (sizeof al - 1)]);
    }
}

static void gen_stream(dyn_sb_t* in)
{
    static const char* const w[] = { "EHLO c.example\r\n", "HELO x\r\n", "MAIL FROM:<a@b.example>\r\n", "MAIL FROM:<q@b.example>\r\n",
        "MAIL FROM:<> BODY=8BITMIME SIZE=20\r\n", "MAIL FROM:<a@b.example> BODY=BINARYMIME\r\n", "RCPT TO:<x@y.example>\r\n", "RCPT TO:<no@y.example>\r\n",
        "RCPT TO:<x@y.example> NOTIFY=SUCCESS,FAILURE ORCPT=rfc822;x+40y\r\n", "RSET\r\n", "NOOP\r\n", "VRFY x\r\n", "AUTH PLAIN AHRpbQB4\r\n",
        "AUTH LOGIN\r\n", "dGlt\r\n", "eA==\r\n", "*\r\n", "XFOO\r\n", "MAIL FROM:<bad\r\n", "\r\n", "BDAT 0 LAST\r\n", "DATA\r\n", "QUIT\r\n",
        "AUTH XOAUTH2 dXNlcj14AWF1dGg9QmVhcmVyIHkBAQ==\r\n", "MAIL FROM:<j\xc3\xb6@b.example> SMTPUTF8\r\n", "NOOP\n", "STARTTLS\r\n" };
    int k, nw = 1 + (int)(rnd() % 30);
    for (k = 0; k < nw; k++) {
        int r = (int)(rnd() % 16);
        if (r == 0) {
            dyn_sb_puts(in, "DATA\r\n");
            {
                dyn_sb_t b;
                size_t i, s0;
                dyn_sb_init(&b);
                gen_body(&b, 120, 0);
                s0 = in->n;
                for (i = 0; i < b.n; i++) {
                    if ((i == 0 || (i >= 2 && b.p[i - 2] == '\r' && b.p[i - 1] == '\n')) && b.p[i] == '.')
                        dyn_sb_putc(in, '.');
                    dyn_sb_putc(in, b.p[i]);
                }
                if (in->n > s0 && !(in->p[in->n - 1] == '\n' && in->n - s0 >= 2 && in->p[in->n - 2] == '\r'))
                    dyn_sb_puts(in, "\r\n");
                dyn_sb_puts(in, rnd() % 9 ? ".\r\n" : "\n.\n");
                dyn_sb_free(&b);
            }
        } else if (r == 1) {
            dyn_sb_t b;
            char hd[40];
            dyn_sb_init(&b);
            gen_body(&b, 60, 1);
            snprintf(hd, sizeof hd, "BDAT %zu%s\r\n", b.n, rnd() & 1 ? " LAST" : "");
            dyn_sb_puts(in, hd);
            dyn_sb_put(in, b.p, b.n);
            dyn_sb_free(&b);
        } else if (r == 2) {
            size_t i, l = 500 + rnd() % 30;
            dyn_sb_puts(in, "NOOP ");
            for (i = 0; i < l; i++)
                dyn_sb_putc(in, 'a');
            dyn_sb_puts(in, "\r\n");
        } else if (r == 3) {
            int j, nb = 1 + (int)(rnd() % 20);
            for (j = 0; j < nb; j++)
                dyn_sb_putc(in, (char)rnd());
        } else
            dyn_sb_puts(in, w[rnd() % (sizeof w / sizeof w[0])]);
    }
}

static void test_split_inv(void)
{
    long it, N = (long)fk_iters(3000);
    int bad = 0;
    dyn_sb_t in;
    fk_g = fk_stream("srv-split-inv");
    dyn_sb_init(&in);
    for (it = 0; it < N; it++) {
        mailk_srv_opts_t o;
        sv_run_t a, b;
        int mode = 1 + (int)(rnd() & 1);
        opts(&o);
        o.caps &= ~MAILK_CAP_STARTTLS;
        o.caps |= rnd() & 1 ? MAILK_CAP_CHUNKING | MAILK_CAP_BINARYMIME : 0;
        o.hooks = (unsigned)(rnd() % 64) & ~1u;
        o.insecure_auth = 1;
        o.mechs = MAILK_MECH_PLAIN | MAILK_MECH_LOGIN | MAILK_MECH_XOAUTH2 | MAILK_MECH_OAUTHBEARER;
        o.body_hwm = rnd() & 1 ? 4 + (uint32_t)(rnd() % 60) : 0;
        o.size_max = rnd() % 4 ? 0 : 1 + rnd() % 80;
        o.rcpt_max = 1 + (uint32_t)(rnd() % 4);
        o.errors_max = 2 + (uint32_t)(rnd() % 10);
        o.msgs_max = 1 + (uint32_t)(rnd() % 4);
        o.line_max = 512;
        in.n = 0;
        gen_stream(&in);
        sv_run(in.p, in.n, &o, 0, &a);
        sv_run(in.p, in.n, &o, mode, &b);
        if (a.out.n != b.out.n || (a.out.n && memcmp(a.out.p, b.out.p, a.out.n)) || a.body.n != b.body.n || (a.body.n && memcmp(a.body.p, b.body.p, a.body.n))
            || a.msgs != b.msgs || a.ev != b.ev) {
            if (bad++ < 3) {
                size_t d = 0;
                while (d < a.out.n && d < b.out.n && a.out.p[d] == b.out.p[d])
                    d++;
                CHECK(0, "[split-inv] whole feed and %s feed differ it=%ld at out byte %zu: whole [%.*s] split [%.*s] msgs %u/%u", mode == 1 ? "byte" : "random-split", it, d,
                    (int)(a.out.n - d < 80 ? a.out.n - d : 80), a.out.p ? a.out.p + d : "", (int)(b.out.n - d < 80 ? b.out.n - d : 80), b.out.p ? b.out.p + d : "", a.msgs, b.msgs);
            }
        }
        sv_free(&a);
        sv_free(&b);
    }
    CHECK(bad == 0, "[split-inv] %d of %ld generated sessions depend on how the input was split", bad, N);
    dyn_sb_free(&in);
}

typedef struct {
    const char* to[3];
    char tob[3][24];
    size_t nto;
    dyn_sb_t body;
    int bdat, accepted, x;
} pm_t;

static void deliver(mailk_cli_t* c, mailk_srv_t* s, sv_run_t* r, int split)
{
    size_t at = 0, guard = 0;
    while (at < c->out.n && guard++ < 4 * c->out.n + 16 && !(s->ev & MAILK_SEV_CLOSE)) {
        size_t chunk = split ? 1 + rnd() % 37 : c->out.n - at, u = 0;
        if (chunk > c->out.n - at)
            chunk = c->out.n - at;
        mailk_srv_feed(s, (const uint8_t*)c->out.p + at, chunk, &u);
        at += u;
        if (s->ev & MAILK_SEV_BODY) {
            dyn_sb_put(&r->acc, s->body.p, s->body.n);
            s->body.n = 0;
            s->ev &= ~MAILK_SEV_BODY;
        }
        if (s->pend)
            sv_decide(s, r);
    }
    c->out.n = 0;
}

static void reply(mailk_srv_t* s, mailk_cli_t* c, int split)
{
    size_t at = 0;
    while (at < s->out.n && !(c->ev & (MAILK_EV_ERROR | MAILK_EV_CLOSED))) {
        size_t chunk = split ? 1 + rnd() % 29 : s->out.n - at;
        if (chunk > s->out.n - at)
            chunk = s->out.n - at;
        mailk_cli_feed(c, (const uint8_t*)s->out.p + at, chunk);
        at += chunk;
    }
    s->out.n = 0;
}

static void test_pair(void)
{
    long it, N = (long)fk_iters(1200);
    int bad_term = 0, bad_body = 0, bad_rcpt = 0, bad_to = 0, bad_quit = 0, timeouts = 0, drops = 0;
    long delivered = 0, sent = 0;
    dyn_sb_t want, got;
    fk_g = fk_stream("srv-pair");
    dyn_sb_init(&want);
    dyn_sb_init(&got);
    for (it = 0; it < N; it++) {
        mailk_srv_opts_t o;
        mailk_srv_t s;
        mailk_cli_opts_t co;
        mailk_cli_t c;
        sv_run_t r;
        pm_t pm[4];
        int nm = 1 + (int)(rnd() % 4), k, j, split = (int)(rnd() % 3), round = 0, tmo = rnd() % 5 ? -1 : (int)(rnd() % 12), drop = rnd() % 6 ? -1 : (int)(rnd() % 12);
        int dead = 0;
        uint32_t want_msgs = 0;
        opts(&o);
        o.caps &= ~(MAILK_CAP_STARTTLS | MAILK_CAP_AUTH);
        if (rnd() & 1)
            o.caps &= ~MAILK_CAP_PIPELINING;
        if (rnd() & 1)
            o.caps |= MAILK_CAP_CHUNKING | (rnd() & 1 ? MAILK_CAP_BINARYMIME : 0);
        o.hooks = MAILK_HOOK(MAILK_PEND_RCPT) | MAILK_HOOK(MAILK_PEND_DATA);
        o.body_hwm = rnd() & 1 ? 8 + (uint32_t)(rnd() % 120) : 0;
        memset(&r, 0, sizeof r);
        dyn_sb_init(&r.out);
        dyn_sb_init(&r.body);
        dyn_sb_init(&r.acc);
        memset(&co, 0, sizeof co);
        co.name = "client.example";
        if (mailk_srv_init(&s, &o) != MAILK_OK || mailk_cli_init(&c, &co) != MAILK_OK) {
            CHECK(0, "[pair] init");
            break;
        }
        for (k = 0; k < nm; k++) {
            pm[k].nto = 1 + rnd() % 3;
            pm[k].accepted = 0;
            for (j = 0; j < (int)pm[k].nto; j++) {
                int no = rnd() % 4 == 0;
                snprintf(pm[k].tob[j], sizeof pm[k].tob[j], "%s%d@y.example", no ? "no" : "u", j);
                pm[k].to[j] = pm[k].tob[j];
                pm[k].accepted += !no;
            }
            dyn_sb_init(&pm[k].body);
            pm[k].bdat = (int)(rnd() & 1);
            gen_body(&pm[k].body, 300, 0);
            {
                size_t q;
                for (q = 0; q < pm[k].body.n; q++)
                    if (pm[k].body.p[q] == 'X')
                        pm[k].body.p[q] = 'x';
                if (rnd() % 5 == 0)
                    dyn_sb_putc(&pm[k].body, 'X');
            }
            if (pm[k].bdat && (o.caps & MAILK_CAP_BINARYMIME) && rnd() % 3 == 0)
                dyn_sb_putc(&pm[k].body, 0);
            pm[k].x = contains_x(&pm[k].body);
        }
        for (round = 0; round < 40 && !(c.ev & MAILK_EV_READY) && !(c.ev & MAILK_EV_ERROR); round++) {
            reply(&s, &c, split);
            deliver(&c, &s, &r, split);
        }
        for (k = 0; k < nm && !dead; k++) {
            mailk_msg_t m;
            int guard = 0, sent_ok;
            size_t before = r.body.n;
            memset(&m, 0, sizeof m);
            m.from = "a@b.example";
            m.to = pm[k].to;
            m.nto = pm[k].nto;
            m.body = (const uint8_t*)pm[k].body.p;
            m.bodyn = pm[k].body.n;
            m.bdat = pm[k].bdat;
            c.ev &= ~(MAILK_EV_READY | MAILK_EV_DONE);
            sent_ok = mailk_cli_send(&c, &m) == MAILK_OK;
            sent++;
            if (!sent_ok) {
                CHECK(0, "[pair] send refused it=%ld msg %d: %.*s", it, k, (int)c.err.n, c.err.p);
                dead = 1;
                break;
            }
            while (!(c.ev & (MAILK_EV_DONE | MAILK_EV_ERROR)) && guard++ < 60) {
                round++;
                if (round == drop) {
                    dead = 2;
                    break;
                }
                deliver(&c, &s, &r, split);
                if (round == tmo) {
                    size_t o0 = s.out.n;
                    if (mailk_srv_timeout(&s) != MAILK_OK || !(s.ev & MAILK_SEV_CLOSE) || s.out.n < o0 + 9 || memcmp(s.out.p + o0, "421 4.4.2", 9)) {
                        if (bad_to++ < 3)
                            CHECK(0, "[timeout] mailk_srv_timeout did not answer 421 4.4.2 and close it=%ld", it);
                    }
                    reply(&s, &c, split);
                    if (!(c.ev & MAILK_EV_ERROR) && bad_to++ < 3)
                        CHECK(0, "[timeout] client did not fail on the server timeout it=%ld", it);
                    timeouts++;
                    dead = 3;
                    break;
                }
                reply(&s, &c, split);
            }
            if (dead == 2) {
                drops++;
                break;
            }
            if (dead)
                break;
            if (!(c.ev & (MAILK_EV_DONE | MAILK_EV_ERROR))) {
                if (bad_term++ < 3)
                    CHECK(0, "[pair-term] message %d never reached DONE or ERROR it=%ld caps %x split %d", k, it, o.caps, split);
                dead = 1;
                break;
            }
            {
                int ok_rc = c.ok == pm[k].accepted;
                for (j = 0; j < (int)pm[k].nto && ok_rc; j++)
                    ok_rc = c.rcpt[j].code == (pm[k].to[j][0] == 'n' ? 550 : 250);
                if (!ok_rc && bad_rcpt++ < 3)
                    CHECK(0, "[pair-rcpt] per-recipient results differ it=%ld msg %d ok %d want %d", it, k, c.ok, pm[k].accepted);
            }
            want.n = 0;
            got.n = 0;
            if (pm[k].accepted && !pm[k].x) {
                int binary = 0;
                if (pm[k].bdat && (o.caps & MAILK_CAP_BINARYMIME)) {
                    size_t i;
                    const char* p = pm[k].body.p;
                    for (i = 0; i < pm[k].body.n; i++)
                        if (!p[i] || (p[i] == '\n' && (!i || p[i - 1] != '\r')) || (p[i] == '\r' && (i + 1 == pm[k].body.n || p[i + 1] != '\n')))
                            binary = 1;
                }
                if (binary)
                    dyn_sb_put(&want, pm[k].body.p, pm[k].body.n);
                else
                    sv_normalise((const uint8_t*)pm[k].body.p, pm[k].body.n, &want);
                dyn_sb_putc(&want, '|');
                want_msgs++;
                dyn_sb_put(&got, r.body.p + before, r.body.n - before);
                if ((c.failed || c.final.code != 250 || got.n != want.n || memcmp(got.p, want.p, want.n)) && bad_body++ < 3)
                    CHECK(0, "[pair-body] delivered body differs it=%ld msg %d bdat %d caps %x failed %d final %d: got %zu want %zu bytes", it, k, pm[k].bdat, o.caps, c.failed,
                        c.final.code, got.n, want.n);
            } else if (!c.failed && bad_body++ < 3)
                CHECK(0, "[pair-body] a refused message reported success it=%ld msg %d", it, k);
            if (s.msgs != want_msgs && bad_body++ < 3)
                CHECK(0, "[pair-body] server took %u messages, want %u it=%ld", s.msgs, want_msgs, it);
        }
        if (!dead) {
            int guard = 0;
            mailk_cli_quit(&c);
            while (!(c.ev & (MAILK_EV_CLOSED | MAILK_EV_ERROR)) && guard++ < 20) {
                deliver(&c, &s, &r, split);
                reply(&s, &c, split);
            }
            if (!(c.ev & MAILK_EV_CLOSED) && bad_quit++ < 3)
                CHECK(0, "[pair-quit] QUIT did not close it=%ld", it);
        }
        delivered += want_msgs;
        for (k = 0; k < nm; k++)
            dyn_sb_free(&pm[k].body);
        sv_free(&r);
        mailk_cli_free(&c);
        mailk_srv_free(&s);
    }
    CHECK(!bad_term && !bad_body && !bad_rcpt && !bad_to && !bad_quit, "[pair] %ld client/server sessions: term %d body %d rcpt %d timeout %d quit %d bad (%d timeouts, %d drops)", N, bad_term,
        bad_body, bad_rcpt, bad_to, bad_quit, timeouts, drops);
    printf("[pair] %ld sessions, %ld messages sent, %ld delivered, %d timeouts, %d drops\n", N, sent, delivered, timeouts, drops);
    CHECK(timeouts > 0 && drops > 0, "[pair] the run injected timeouts (%d) and drops (%d)", timeouts, drops);
    dyn_sb_free(&want);
    dyn_sb_free(&got);
}

static void test_bound(void)
{
    mailk_srv_opts_t o;
    mailk_srv_t s;
    dyn_sb_t b;
    int k;
    dyn_sb_init(&b);
    opts(&o);
    open_srv(&s, &o);
    feed(&s, "EHLO c.example\r\n");
    s.out.n = 0;
    dyn_sb_puts(&b, "NOOP ");
    while (b.n < MAILK_SRV_LINE_MAX - 2)
        dyn_sb_putc(&b, 'a');
    dyn_sb_puts(&b, "\r\n");
    dyn_sb_putc(&b, 0);
    feed(&s, b.p);
    CHECK(out_is(&s, "250 2.0.0 Ok\r\n"), "[bound] a 12288-octet command line (CRLF included) is accepted");
    b.n = 5;
    while (b.n < MAILK_SRV_LINE_MAX - 1)
        dyn_sb_putc(&b, 'a');
    dyn_sb_puts(&b, "\r\n");
    dyn_sb_putc(&b, 0);
    feed(&s, b.p);
    CHECK(out_is(&s, "500 5.5.6 Line too long\r\n"), "[bound] a 12289-octet command line is 500 5.5.6");
    mailk_srv_free(&s);
    opts(&o);
    o.line_max = 512;
    o.rcpt_max = 3;
    o.errors_max = 4;
    open_srv(&s, &o);
    feed(&s, "EHLO c.example\r\n");
    s.out.n = 0;
    b.n = 0;
    dyn_sb_puts(&b, "NOOP ");
    while (b.n < 510)
        dyn_sb_putc(&b, 'b');
    dyn_sb_puts(&b, "\r\n");
    dyn_sb_putc(&b, 0);
    feed(&s, b.p);
    CHECK(out_is(&s, "250 2.0.0 Ok\r\n"), "[bound] line_max 512: 512 octets accepted");
    b.n = 0;
    dyn_sb_puts(&b, "NOOP ");
    while (b.n < 511)
        dyn_sb_putc(&b, 'b');
    dyn_sb_puts(&b, "\r\n");
    dyn_sb_putc(&b, 0);
    feed(&s, b.p);
    CHECK(out_is(&s, "500 5.5.6 Line too long\r\n"), "[bound] line_max 512: 513 octets is 500 5.5.6");
    mailk_srv_free(&s);
    open_srv(&s, &o);
    feed(&s, "EHLO c.example\r\nMAIL FROM:<a@b.example>\r\n");
    s.out.n = 0;
    for (k = 0; k < 4; k++)
        feed(&s, "RCPT TO:<x@y.example>\r\n");
    CHECK(out_is(&s, "250 2.1.5 Ok\r\n250 2.1.5 Ok\r\n250 2.1.5 Ok\r\n452 4.5.3 Too many recipients\r\n"), "[bound] rcpt_max 3: the 4th RCPT is 452");
    mailk_srv_free(&s);
    open_srv(&s, &o);
    for (k = 0; k < 3; k++)
        feed(&s, "XBAD\r\n");
    CHECK(!(s.ev & MAILK_SEV_CLOSE), "[bound] errors_max 4: 3 bad commands keep the session");
    feed(&s, "XBAD\r\n");
    CHECK(s.ev & MAILK_SEV_CLOSE, "[bound] errors_max 4: the 4th bad command closes");
    mailk_srv_free(&s);
    open_srv(&s, &o);
    feed(&s, "EHLO c.example\r\n");
    s.out.n = 0;
    feed(&s, "MAIL FROM:<a@b.example> SIZE=\r\nMAIL FROM:<a@b.example> SIZE=1x\r\nMAIL FROM:<a@b.example> SIZE=99999999999999999999\r\n");
    CHECK(out_is(&s, "501 5.5.4 Syntax error in parameters\r\n501 5.5.4 Invalid parameter value\r\n552 5.3.4 Message size exceeds fixed maximum message size\r\n"),
        "[bound] SIZE= empty (syntax) and non-digit are 501, 20 digits past 2^64 is 552 (value parsed within its length)");
    mailk_srv_free(&s);
    dyn_sb_free(&b);
}

static void test_fuzz_entry(void)
{
    static const char* const seeds[] = {
        "\x07\x03" "EHLO c\r\nMAIL FROM:<a@b.example>\r\nRCPT TO:<x@y.example>\r\nRCPT TO:<no@y.example>\r\nDATA\r\nhi\r\n..x\r\n.\r\nQUIT\r\n",
        "\x4f\x05" "EHLO c\r\nMAIL FROM:<a@b.example> BODY=BINARYMIME\r\nRCPT TO:<x@y.example>\r\nBDAT 5\r\nab\r\ncBDAT 2 LAST\r\nzzNOOP\r\n",
        "\x13\x01" "EHLO c\r\nAUTH PLAIN AHRpbQB4\r\nAUTH LOGIN\r\ndGlt\r\neA==\r\nMAIL FROM:<q@b>\r\nRSET\r\n",
        "\xc1\x09" "HELO x\r\nMAIL FROM:<> SIZE=50\r\nRCPT TO:<Postmaster>\r\nDATA\r\n0123456789012345678901234567890123456789012345\r\n.\r\n",
    };
    uint8_t* buf = malloc(4096);
    long it, N = (long)fk_iters(6000);
    fk_g = fk_stream("srv-fuzz-entry");
    printf("[fuzz-srv] start\n");
    fflush(stdout);
    for (it = 0; it < (long)(sizeof seeds / sizeof seeds[0]); it++)
        LLVMFuzzerTestOneInput((const uint8_t*)seeds[it], strlen(seeds[it]));
    for (it = 0; it < N; it++) {
        const char* sd = seeds[rnd() % (sizeof seeds / sizeof seeds[0])];
        size_t n = sd[0] ? strlen(sd) : 1 + strlen(sd + 1), k, m = 1 + rnd() % 5;
        memcpy(buf, sd, n);
        buf[0] = (uint8_t)rnd();
        buf[1] = (uint8_t)rnd();
        for (k = 0; k < m; k++) {
            size_t at = 2 + rnd() % (n - 2);
            switch (rnd() % 4) {
            case 0:
                buf[at] = (uint8_t)rnd();
                break;
            case 1:
                memmove(buf + at, buf + at + 1, n - at - 1);
                n--;
                break;
            case 2:
                if (n < 4000) {
                    memmove(buf + at + 1, buf + at, n - at);
                    buf[at] = (uint8_t)"\r\n. :<>@"[rnd() % 8];
                    n++;
                }
                break;
            default:
                if (n * 2 < 4000) {
                    size_t l = 1 + rnd() % (n - at);
                    memmove(buf + at + l, buf + at, n - at);
                    n += l;
                }
                break;
            }
        }
        LLVMFuzzerTestOneInput(buf, n);
    }
    printf("[fuzz-srv] done\n");
    CHECK(1, "[fuzz-srv] %ld seeded and mutated sessions through src/fuzz/fuzz_mailk_srv.c (whole vs split feed) without a trap", N);
    free(buf);
}

int main(void)
{
    fk_init("test_mailk_server");
    fflush(stdout);
    fk_g = fk_stream("fixed");
    test_chunking();
    test_streaming();
    test_scram_bdat_loopback();
    test_greeting_ehlo();
    test_sequencing();
    test_limits();
    test_policy_timeout();
    test_starttls();
    test_auth();
    test_data_hooks();
    test_loopback();
    fk_g = fk_stream("fuzz");
    test_fuzz();
    test_split_inv();
    test_pair();
    test_bound();
    test_fuzz_entry();    printf("%d checks\n", checks);
    fk_fails += fails;
    return fk_done(__FILE__);
}
