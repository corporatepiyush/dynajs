/*
 * test_mailk_client.c -- src/core/mailk/mailk-client.c, the sans-I/O SMTP client session engine
 * behind dyna:smtp (S9 S1a, 2026-11-27). Bytes in, bytes to write plus events out; no sockets and
 * no JS values, so one pool job advances it (owner rule: mail parse/encode/crypto off the main
 * thread). Each case below is a scripted server transcript.
 *
 * External oracles:
 *  - RFC 5321 3.1/4.2/4.3.2: 220 greeting, EHLO then HELO fallback on 5xx (only with no TLS or
 *    AUTH demand), MAIL/RCPT/DATA sequencing, 354 then the dot-stuffed body and "." (4.5.2),
 *    RSET after an aborted transaction, 421 at any point ends the session, QUIT/221.
 *  - RFC 2920 PIPELINING: MAIL, every RCPT and DATA in one write; when every RCPT failed but DATA
 *    still got 354, send the lone "." (3.1); without PIPELINING one command per reply.
 *  - RFC 3207 STARTTLS: re-EHLO after the handshake and forget the pre-TLS capabilities; any byte
 *    after the 220 is refused (the CVE-2011-0411 injection shape); "required" fails when the
 *    server lacks STARTTLS.
 *  - RFC 4954 AUTH: initial response, "334 " challenges, "*" cancel, 235/535; PLAIN (RFC 4616
 *    example), LOGIN, CRAM-MD5 (RFC 2195 example), XOAUTH2 error continuation (empty line),
 *    OAUTHBEARER failure (RFC 7628 3.2.3 "AQ=="), SCRAM-SHA-256 against mailk's own server side
 *    with the server signature checked before the empty final response. Cleartext mechanisms are
 *    refused without TLS unless insecure_auth.
 *  - RFC 1870 SIZE (refused locally when over the advertised limit), RFC 6152 8BITMIME, RFC 6531
 *    SMTPUTF8, RFC 3030 CHUNKING/BDAT LAST with a CRLF-canonical body; BINARYMIME octets byte-exact (S1c), RFC 3461
 *    DSN RET/ENVID (xtext)/NOTIFY/ORCPT, only when advertised.
 * Fuzz: 20000 random server transcripts (replies, garbage, split at random points) through every
 * state under ASan/UBSan.
 * Mutants (each red here): STARTTLS trailing-bytes check dropped, lone-dot on 354-after-all-RCPT-
 * failed dropped, cleartext-AUTH gate dropped, SCRAM server-signature requirement dropped, SIZE
 * limit check dropped, RSET after an aborted transaction dropped.
 * Seeded property tests (tests/lib/dyn-fuzzkit.h, 2026-11-27): DYN_FUZZ_SEED replays a run (the seed is
 * printed first), DYN_FUZZ_ITERS (integer, 50 = deep run) multiplies every loop. Each tagged check is proven by its own mutant:
 *  [cli-model]    2500 random sessions (PIPELINING, CHUNKING, AUTH, random accept/reject per command)
 *                 against a model server; per-message and per-recipient results must match the model.
 *  [cli-body]     delivered bodies equal the CRLF-normalised input (DATA unstuffed, BDAT joined).
 *  [no-pipe]      without PIPELINING no command is sent before the previous reply.
 *  [split-inv]    the same session fed whole vs at random byte splits gives identical output.
 *  [drop] [cli-term] a 421 at a random point ends the session with an error, all pending messages
 *                 failed, nothing written after; QUIT may be dropped.
 *  [fuzz-cli]     src/fuzz/fuzz_mailk.c over mutated server transcripts (aborts on stuff/unstuff
 *                 model mismatch and split divergence). Mutants: k-split, k-pipe, k-rcpt-ok, k-421, k-body, k-fz-dot (dot
 *                 stuffing off). The pure engine has no timer or EOF entry: timeouts are server-side.
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

static int feed(mailk_cli_t* c, const char* s)
{
    return mailk_cli_feed(c, (const uint8_t*)s, strlen(s));
}

static int out_is(mailk_cli_t* c, const char* want)
{
    size_t n = strlen(want);
    int ok = c->out.n == n && (!n || !memcmp(c->out.p, want, n));
    if (!ok)
        printf("  out was [%.*s] want [%s]\n", (int)c->out.n, c->out.p ? c->out.p : "", want);
    c->out.n = 0;
    return ok;
}

static const char* rtext(mailk_cli_t* c, const mailk_res_t* r)
{
    static char b[256];
    snprintf(b, sizeof b, "%.*s", (int)r->len, c->texts.p ? c->texts.p + r->off : "");
    return b;
}

static void open_cli(mailk_cli_t* c, int tls, const char* caps, const char* user, const char* pass, int insecure)
{
    mailk_cli_opts_t o;
    memset(&o, 0, sizeof o);
    o.name = "client.example";
    o.tls = tls;
    o.user = user;
    o.pass = pass;
    o.insecure_auth = insecure;
    CHECK(mailk_cli_init(c, &o) == MAILK_OK, "init");
    if (caps) {
        CHECK(feed(c, "220 mx.example ESMTP\r\n") == MAILK_OK && out_is(c, "EHLO client.example\r\n"), "greet->EHLO");
        feed(c, caps);
    }
}

#define CAPS_FULL "250-mx.example hello\r\n250-PIPELINING\r\n250-8BITMIME\r\n250-SIZE 1000\r\n250-DSN\r\n250-SMTPUTF8\r\n250-CHUNKING\r\n250 ENHANCEDSTATUSCODES\r\n"
#define CAPS_MIN "250-mx.example\r\n250 SIZE\r\n"
#define CAPS_FULL_BIN "250-mx.example hello\r\n250-PIPELINING\r\n250-8BITMIME\r\n250-SIZE 100000\r\n250-DSN\r\n250-SMTPUTF8\r\n250-CHUNKING\r\n250-BINARYMIME\r\n250 ENHANCEDSTATUSCODES\r\n"

static void test_pipelined(void)
{
    mailk_cli_t c;
    const char* to[] = { "x@y.example", "z@y.example" };
    mailk_msg_t m;
    open_cli(&c, MAILK_TLS_NONE, CAPS_FULL, NULL, NULL, 0);
    CHECK((c.ev & MAILK_EV_READY) && out_is(&c, ""), "ready without STARTTLS in NONE mode");
    CHECK(c.caps & MAILK_CAP_PIPELINING && c.size_max == 1000 && c.caps & MAILK_CAP_DSN, "caps parsed");
    memset(&m, 0, sizeof m);
    m.from = "a@b.example";
    m.to = to;
    m.nto = 2;
    m.body = (const uint8_t*)"Hi\r\n.dot\r\nend";
    m.bodyn = 13;
    CHECK(mailk_cli_send(&c, &m) == MAILK_OK, "send");
    CHECK(out_is(&c, "MAIL FROM:<a@b.example> SIZE=19\r\nRCPT TO:<x@y.example>\r\nRCPT TO:<z@y.example>\r\nDATA\r\n"), "one pipelined write");
    CHECK(feed(&c, "250 2.1.0 ok\r\n250 2.1.5 ok\r\n550 5.1.1 no such user\r\n354 go\r\n") == MAILK_OK, "feed");
    CHECK(out_is(&c, "Hi\r\n..dot\r\nend\r\n.\r\n"), "stuffed body after 354");
    CHECK(!(c.ev & MAILK_EV_DONE), "not done before final");
    CHECK(feed(&c, "250 2.0.0 queued as ABC\r\n") == MAILK_OK && (c.ev & MAILK_EV_DONE) && !c.failed && c.ok == 1, "done");
    CHECK(c.rcpt[1].code == 550 && c.rcpt[1].cls == 5 && c.rcpt[1].det == 1 && !strcmp(rtext(&c, &c.rcpt[1]), "5.1.1 no such user"), "per-rcpt result");
    CHECK(c.final.code == 250 && !strcmp(rtext(&c, &c.final), "2.0.0 queued as ABC"), "final reply");
    CHECK(mailk_cli_quit(&c) == MAILK_OK && out_is(&c, "QUIT\r\n") && feed(&c, "221 bye\r\n") == MAILK_OK && (c.ev & MAILK_EV_CLOSED), "quit");
    mailk_cli_free(&c);

    open_cli(&c, MAILK_TLS_NONE, CAPS_FULL, NULL, NULL, 0);
    c.out.n = 0;
    mailk_cli_send(&c, &m);
    c.out.n = 0;
    feed(&c, "250 ok\r\n550 no\r\n550 no\r\n354 go\r\n");
    CHECK(out_is(&c, ".\r\n"), "lone dot when every RCPT failed but DATA got 354");
    feed(&c, "554 no valid recipients\r\n");
    CHECK((c.ev & MAILK_EV_DONE) && c.failed && out_is(&c, ""), "failed, no RSET after the dot");
    mailk_cli_free(&c);

    open_cli(&c, MAILK_TLS_NONE, CAPS_FULL, NULL, NULL, 0);
    c.out.n = 0;
    mailk_cli_send(&c, &m);
    c.out.n = 0;
    feed(&c, "550 sender refused\r\n503 no\r\n503 no\r\n503 no MAIL\r\n");
    CHECK(out_is(&c, "RSET\r\n") && !(c.ev & MAILK_EV_DONE), "RSET after an aborted transaction");
    feed(&c, "250 reset\r\n");
    CHECK((c.ev & MAILK_EV_DONE) && c.failed && c.mail.code == 550, "done after RSET");
    c.ev = 0;
    CHECK(mailk_cli_send(&c, &m) == MAILK_OK, "session reusable after failure");
    mailk_cli_free(&c);
}

static void test_serial(void)
{
    mailk_cli_t c;
    const char* to[] = { "x@y.example", "z@y.example" };
    mailk_msg_t m;
    open_cli(&c, MAILK_TLS_NONE, CAPS_MIN, NULL, NULL, 0);
    memset(&m, 0, sizeof m);
    m.from = "";
    m.to = to;
    m.nto = 2;
    m.body = (const uint8_t*)"x";
    m.bodyn = 1;
    CHECK(mailk_cli_send(&c, &m) == MAILK_OK && out_is(&c, "MAIL FROM:<> SIZE=6\r\n"), "null reverse-path, one command");
    feed(&c, "250 ok\r\n");
    CHECK(out_is(&c, "RCPT TO:<x@y.example>\r\n"), "rcpt 1");
    feed(&c, "451 later\r\n");
    CHECK(out_is(&c, "RCPT TO:<z@y.example>\r\n"), "rcpt 2");
    feed(&c, "250 ok\r\n");
    CHECK(out_is(&c, "DATA\r\n"), "data");
    feed(&c, "354 go\r\n");
    CHECK(out_is(&c, "x\r\n.\r\n"), "body");
    feed(&c, "250 ok\r\n");
    CHECK((c.ev & MAILK_EV_DONE) && !c.failed && c.ok == 1 && c.rcpt[0].code == 451, "serial done");
    mailk_cli_free(&c);

    open_cli(&c, MAILK_TLS_NONE, CAPS_MIN, NULL, NULL, 0);
    c.out.n = 0;
    mailk_cli_send(&c, &m);
    c.out.n = 0;
    feed(&c, "250 ok\r\n");
    c.out.n = 0;
    feed(&c, "550 no\r\n");
    c.out.n = 0;
    feed(&c, "550 no\r\n");
    CHECK(out_is(&c, "RSET\r\n"), "no DATA when every RCPT failed; RSET instead");
    feed(&c, "250 ok\r\n");
    CHECK((c.ev & MAILK_EV_DONE) && c.failed, "failed");
    mailk_cli_free(&c);

    open_cli(&c, MAILK_TLS_NONE, NULL, NULL, NULL, 0);
    feed(&c, "220 old\r\n");
    c.out.n = 0;
    feed(&c, "502 unknown command\r\n");
    CHECK(out_is(&c, "HELO client.example\r\n"), "HELO fallback");
    feed(&c, "250 hi\r\n");
    CHECK((c.ev & MAILK_EV_READY) && c.caps == 0, "ready after HELO");
    mailk_cli_free(&c);

    open_cli(&c, MAILK_TLS_STARTTLS, NULL, NULL, NULL, 0);
    feed(&c, "220 old\r\n");
    feed(&c, "502 unknown command\r\n");
    CHECK(c.ev & MAILK_EV_ERROR, "no HELO fallback when STARTTLS is required");
    mailk_cli_free(&c);

    open_cli(&c, MAILK_TLS_NONE, NULL, NULL, NULL, 0);
    feed(&c, "554 go away\r\n");
    CHECK((c.ev & MAILK_EV_ERROR) && strstr(c.err.p, "554"), "greeting 554: %s", c.err.p ? c.err.p : "");
    mailk_cli_free(&c);

    open_cli(&c, MAILK_TLS_NONE, CAPS_MIN, NULL, NULL, 0);
    c.out.n = 0;
    mailk_cli_send(&c, &m);
    feed(&c, "421 4.3.2 shutting down\r\n");
    CHECK((c.ev & MAILK_EV_ERROR) && strstr(c.err.p, "421"), "421 ends the session");
    CHECK(feed(&c, "250 ok\r\n") == MAILK_EBAD, "dead session refuses input");
    mailk_cli_free(&c);
}

static void test_starttls(void)
{
    mailk_cli_t c;
    open_cli(&c, MAILK_TLS_STARTTLS, "250-mx\r\n250-STARTTLS\r\n250 AUTH PLAIN\r\n", NULL, NULL, 0);
    CHECK(out_is(&c, "STARTTLS\r\n"), "STARTTLS sent");
    CHECK(feed(&c, "220 2.0.0 go ahead\r\n") == MAILK_OK && (c.ev & MAILK_EV_TLS), "TLS event");
    CHECK(mailk_cli_tls_done(&c) == MAILK_OK && out_is(&c, "EHLO client.example\r\n") && c.caps == 0, "re-EHLO, caps forgotten");
    feed(&c, "250-mx\r\n250 PIPELINING\r\n");
    CHECK((c.ev & MAILK_EV_READY) && c.tls_on, "ready over TLS");
    mailk_cli_free(&c);

    open_cli(&c, MAILK_TLS_STARTTLS, "250-mx\r\n250 STARTTLS\r\n", NULL, NULL, 0);
    c.out.n = 0;
    CHECK(feed(&c, "220 go ahead\r\n250-injected\r\n") == MAILK_EBAD && (c.ev & MAILK_EV_ERROR), "bytes after 220 in the same read refused");
    mailk_cli_free(&c);

    open_cli(&c, MAILK_TLS_STARTTLS, "250-mx\r\n250 STARTTLS\r\n", NULL, NULL, 0);
    c.out.n = 0;
    feed(&c, "220 go ahead\r\n");
    CHECK(feed(&c, "2") == MAILK_EBAD, "bytes after 220 in a later read refused");
    mailk_cli_free(&c);

    open_cli(&c, MAILK_TLS_STARTTLS, "250-mx\r\n250 PIPELINING\r\n", NULL, NULL, 0);
    CHECK((c.ev & MAILK_EV_ERROR) && strstr(c.err.p, "STARTTLS"), "required STARTTLS missing");
    mailk_cli_free(&c);

    open_cli(&c, MAILK_TLS_OPPORTUNISTIC, "250-mx\r\n250 PIPELINING\r\n", NULL, NULL, 0);
    CHECK(c.ev & MAILK_EV_READY, "opportunistic continues in plaintext");
    mailk_cli_free(&c);

    open_cli(&c, MAILK_TLS_OPPORTUNISTIC, "250-mx\r\n250 STARTTLS\r\n", NULL, NULL, 0);
    c.out.n = 0;
    feed(&c, "454 TLS not available\r\n");
    CHECK(c.ev & MAILK_EV_READY, "opportunistic 454 continues");
    mailk_cli_free(&c);

    open_cli(&c, MAILK_TLS_STARTTLS, "250-mx\r\n250 STARTTLS\r\n", NULL, NULL, 0);
    c.out.n = 0;
    feed(&c, "454 TLS not available\r\n");
    CHECK(c.ev & MAILK_EV_ERROR, "required 454 fails");
    mailk_cli_free(&c);
}

static void test_auth(void)
{
    mailk_cli_t c;
    mailk_cli_opts_t o;
    open_cli(&c, MAILK_TLS_IMPLICIT, "250-mx\r\n250 AUTH LOGIN PLAIN\r\n", "tim", "tanstaaftanstaaf", 0);
    CHECK(out_is(&c, "AUTH PLAIN AHRpbQB0YW5zdGFhZnRhbnN0YWFm\r\n"), "PLAIN initial response (RFC 4616 example)");
    feed(&c, "235 2.7.0 ok\r\n");
    CHECK((c.ev & MAILK_EV_READY) && !c.pass, "ready, password wiped");
    mailk_cli_free(&c);

    open_cli(&c, MAILK_TLS_NONE, "250-mx\r\n250 AUTH PLAIN\r\n", "tim", "pw", 0);
    CHECK((c.ev & MAILK_EV_ERROR) && strstr(c.err.p, "cleartext"), "cleartext PLAIN refused");
    mailk_cli_free(&c);
    open_cli(&c, MAILK_TLS_NONE, "250-mx\r\n250 AUTH PLAIN\r\n", "tim", "pw", 1);
    CHECK(out_is(&c, "AUTH PLAIN AHRpbQBwdw==\r\n"), "insecure_auth allows it");
    mailk_cli_free(&c);

    open_cli(&c, MAILK_TLS_IMPLICIT, "250-mx\r\n250 AUTH LOGIN\r\n", "tim", "pw", 0);
    CHECK(out_is(&c, "AUTH LOGIN\r\n"), "LOGIN");
    feed(&c, "334 VXNlcm5hbWU6\r\n");
    CHECK(out_is(&c, "dGlt\r\n"), "LOGIN user");
    feed(&c, "334 UGFzc3dvcmQ6\r\n");
    CHECK(out_is(&c, "cHc=\r\n"), "LOGIN pass");
    feed(&c, "535 5.7.8 bad credentials\r\n");
    CHECK((c.ev & MAILK_EV_ERROR) && strstr(c.err.p, "535"), "535 fails: %s", c.err.p);
    mailk_cli_free(&c);

    memset(&o, 0, sizeof o);
    o.tls = MAILK_TLS_NONE;
    o.user = "tim";
    o.pass = "tanstaaftanstaaf";
    CHECK(mailk_cli_init(&c, &o) == MAILK_OK, "init");
    feed(&c, "220 mx\r\n");
    c.out.n = 0;
    feed(&c, "250-mx\r\n250 AUTH CRAM-MD5\r\n");
    CHECK(c.ev & MAILK_EV_ERROR, "CRAM-MD5 off by default");
    mailk_cli_free(&c);
    o.mechs = MAILK_MECH_CRAM_MD5;
    mailk_cli_init(&c, &o);
    feed(&c, "220 mx\r\n");
    c.out.n = 0;
    feed(&c, "250-mx\r\n250 AUTH CRAM-MD5\r\n");
    CHECK(out_is(&c, "AUTH CRAM-MD5\r\n"), "CRAM-MD5 when enabled (no cleartext secret)");
    feed(&c, "334 PDE4OTYuNjk3MTcwOTUyQHBvc3RvZmZpY2UucmVzdG9uLm1jaS5uZXQ+\r\n");
    CHECK(out_is(&c, "dGltIGI5MTNhNjAyYzdlZGE3YTQ5NWI0ZTZlNzMzNGQzODkw\r\n"), "RFC 2195 response");
    mailk_cli_free(&c);

    memset(&o, 0, sizeof o);
    o.tls = MAILK_TLS_IMPLICIT;
    o.user = "someuser@example.com";
    o.token = "ya29.vF9dft4qmTc2Nvb3RlckBhdHRhdmlzdGEuY29tCg";
    mailk_cli_init(&c, &o);
    feed(&c, "220 mx\r\n");
    c.out.n = 0;
    feed(&c, "250-mx\r\n250 AUTH PLAIN XOAUTH2\r\n");
    CHECK(out_is(&c, "AUTH XOAUTH2 dXNlcj1zb21ldXNlckBleGFtcGxlLmNvbQFhdXRoPUJlYXJlciB5YTI5LnZGOWRmdDRxbVRjMk52YjNSbGNrQmhkSFJoZG1semRHRXVZMjl0Q2cBAQ==\r\n"), "XOAUTH2");
    feed(&c, "334 eyJzdGF0dXMiOiI0MDEifQ==\r\n");
    CHECK(out_is(&c, "\r\n"), "XOAUTH2 error continuation is an empty line");
    feed(&c, "535 5.7.8 nope\r\n");
    CHECK(c.ev & MAILK_EV_ERROR, "then fails");
    mailk_cli_free(&c);
    mailk_cli_init(&c, &o);
    feed(&c, "220 mx\r\n");
    c.out.n = 0;
    feed(&c, "250-mx\r\n250 AUTH OAUTHBEARER XOAUTH2\r\n");
    CHECK(c.out.n > 22 && !memcmp(c.out.p, "AUTH OAUTHBEARER ", 17), "OAUTHBEARER preferred");
    c.out.n = 0;
    feed(&c, "334 eyJzdGF0dXMiOiI0MDEifQ==\r\n");
    CHECK(out_is(&c, "AQ==\r\n"), "OAUTHBEARER failure ack");
    mailk_cli_free(&c);

    open_cli(&c, MAILK_TLS_IMPLICIT, "250-mx\r\n250 AUTH GSSAPI\r\n", "u", "p", 0);
    CHECK((c.ev & MAILK_EV_ERROR) && strstr(c.err.p, "mechanism"), "no common mechanism");
    mailk_cli_free(&c);
    open_cli(&c, MAILK_TLS_IMPLICIT, "250-mx\r\n250 PIPELINING\r\n", "u", "p", 0);
    CHECK((c.ev & MAILK_EV_ERROR) && strstr(c.err.p, "AUTH"), "no AUTH offered");
    mailk_cli_free(&c);
    memset(&o, 0, sizeof o);
    o.user = "u\r\nMAIL FROM:<x>";
    o.pass = "p";
    CHECK(mailk_cli_init(&c, &o) == MAILK_EBAD, "CRLF in user refused");
    mailk_cli_free(&c);
    o.user = NULL;
    o.name = "bad name\r\nRSET";
    CHECK(mailk_cli_init(&c, &o) == MAILK_EBAD, "CRLF in EHLO name refused");
    mailk_cli_free(&c);
}

static int b64_line(const char* line, size_t n, dyn_sb_t* out)
{
    out->n = 0;
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
        n--;
    return mailk_sasl_resp_decode(line, n, out);
}

static void scram_case(int tamper, int skip_proof)
{
    mailk_cli_t c;
    mailk_scram_t s;
    dyn_sb_t d, t;
    uint8_t st[32], sk[32], salt[8] = { 9, 8, 7, 6, 5, 4, 3, 2 };
    char line[512];
    mailk_scram_init(&s, MAILK_SCRAM_SHA256);
    dyn_sb_init(&d);
    dyn_sb_init(&t);
    mailk_scram_derive(MAILK_SCRAM_SHA256, "pencil", salt, sizeof salt, 4096, st, sk);
    open_cli(&c, MAILK_TLS_NONE, "250-mx\r\n250 AUTH PLAIN SCRAM-SHA-256\r\n", "user", "pencil", 0);
    CHECK(c.out.n > 20 && !memcmp(c.out.p, "AUTH SCRAM-SHA-256 ", 19), "SCRAM chosen over PLAIN, allowed without TLS");
    CHECK(b64_line(c.out.p + 19, c.out.n - 19, &d) == MAILK_OK && mailk_scram_server_first(&s, (const uint8_t*)d.p, d.n) == MAILK_OK, "client-first valid");
    c.out.n = 0;
    mailk_scram_server_challenge(&s, salt, sizeof salt, 4096, st, sk, NULL, &t);
    snprintf(line, sizeof line, "334 ");
    d.n = 0;
    mailk_b64_encode(&d, (const uint8_t*)t.p, t.n);
    snprintf(line, sizeof line, "334 %.*s\r\n", (int)d.n, d.p);
    feed(&c, line);
    CHECK(b64_line(c.out.p, c.out.n, &d) == MAILK_OK, "client-final b64");
    c.out.n = 0;
    t.n = 0;
    CHECK(mailk_scram_server_final(&s, (const uint8_t*)d.p, d.n, &t) == MAILK_OK, "server accepts the proof");
    if (tamper)
        t.p[3] ^= 1;
    if (skip_proof) {
        feed(&c, "235 2.7.0 ok\r\n");
        CHECK(c.ev & MAILK_EV_ERROR, "235 before the server signature refused");
    } else {
        d.n = 0;
        mailk_b64_encode(&d, (const uint8_t*)t.p, t.n);
        snprintf(line, sizeof line, "334 %.*s\r\n", (int)d.n, d.p);
        feed(&c, line);
        if (tamper) {
            CHECK((c.ev & MAILK_EV_ERROR) && out_is(&c, "*\r\n"), "bad server signature cancels and fails");
        } else {
            CHECK(out_is(&c, "\r\n"), "empty response after a good server signature");
            feed(&c, "235 2.7.0 ok\r\n");
            CHECK(c.ev & MAILK_EV_READY, "SCRAM ready");
        }
    }
    mailk_cli_free(&c);
    mailk_scram_free(&s);
    dyn_sb_free(&d);
    dyn_sb_free(&t);
}

static void test_msg_checks(void)
{
    mailk_cli_t c;
    mailk_msg_t m;
    const char* to[] = { "x@y.example" };
    const char* uto[] = { "\xE7\x94\xA8\xE6\x88\xB7@\xE4\xBE\x8B\xE5\xAD\x90.\xE5\xB9\xBF\xE5\x91\x8A" };
    char big[1200];
    open_cli(&c, MAILK_TLS_NONE, CAPS_MIN, NULL, NULL, 0);
    memset(&m, 0, sizeof m);
    m.from = "a@b.example";
    m.to = to;
    m.nto = 1;
    m.body = (const uint8_t*)"caf\xC3\xA9";
    m.bodyn = 5;
    CHECK(mailk_cli_send(&c, &m) == MAILK_EBAD && strstr(c.err.p, "8BITMIME") && !(c.ev & MAILK_EV_ERROR), "8-bit needs 8BITMIME, session survives");
    m.body = (const uint8_t*)"x";
    m.bodyn = 1;
    m.to = uto;
    CHECK(mailk_cli_send(&c, &m) == MAILK_EBAD && strstr(c.err.p, "SMTPUTF8"), "UTF-8 address needs SMTPUTF8");
    m.to = to;
    m.ret = "FULL";
    CHECK(mailk_cli_send(&c, &m) == MAILK_EBAD && strstr(c.err.p, "DSN"), "DSN needs DSN");
    m.ret = NULL;
    memset(big, 'a', sizeof big);
    m.body = (const uint8_t*)big;
    m.bodyn = 999;
    CHECK(mailk_cli_send(&c, &m) == MAILK_EBAD && strstr(c.err.p, "998"), "line over 998");
    m.from = "a@b.example> SIZE=1\r\nRCPT TO:<evil@x";
    m.bodyn = 1;
    CHECK(mailk_cli_send(&c, &m) == MAILK_EBAD && strstr(c.err.p, "sender"), "CRLF injection in from");
    m.from = "a@b.example";
    to[0] = "x@y.example>\r\nDATA";
    CHECK(mailk_cli_send(&c, &m) == MAILK_EBAD && strstr(c.err.p, "recipient"), "CRLF injection in to");
    to[0] = "x@y.example";
    m.body = (const uint8_t*)"a\0b";
    m.bodyn = 3;
    CHECK(mailk_cli_send(&c, &m) == MAILK_EBAD && strstr(c.err.p, "NUL"), "NUL needs BINARYMIME");
    mailk_cli_free(&c);

    open_cli(&c, MAILK_TLS_NONE, CAPS_FULL, NULL, NULL, 0);
    m.body = (const uint8_t*)big;
    m.bodyn = 997;
    CHECK(mailk_cli_send(&c, &m) == MAILK_EBAD && strstr(c.err.p, "SIZE"), "SIZE 1000 exceeded");
    m.body = (const uint8_t*)"caf\xC3\xA9\nline2\n";
    m.bodyn = 12;
    m.ret = "HDRS";
    m.envid = "id 1+2=3";
    m.notify = "SUCCESS,FAILURE";
    m.orcpt = 1;
    CHECK(mailk_cli_send(&c, &m) == MAILK_OK, "DSN send");
    CHECK(out_is(&c, "MAIL FROM:<a@b.example> SIZE=17 BODY=8BITMIME RET=HDRS ENVID=id+201+2B2+3D3\r\nRCPT TO:<x@y.example> NOTIFY=SUCCESS,FAILURE ORCPT=rfc822;x@y.example\r\nDATA\r\n"), "DSN params");
    mailk_cli_free(&c);
    open_cli(&c, MAILK_TLS_NONE, CAPS_FULL, NULL, NULL, 0);
    m.notify = "SUCCESS\r\nDATA";
    CHECK(mailk_cli_send(&c, &m) == MAILK_EBAD && strstr(c.err.p, "notify"), "NOTIFY injection");
    m.notify = NULL;
    m.ret = NULL;
    m.envid = NULL;
    m.orcpt = 0;
    m.to = uto;
    m.body = (const uint8_t*)"x";
    m.bodyn = 1;
    CHECK(mailk_cli_send(&c, &m) == MAILK_OK && strstr(c.out.p, "SMTPUTF8") && strstr(c.out.p, "RCPT TO:<\xE7\x94\xA8"), "SMTPUTF8 send");
    mailk_cli_free(&c);

    open_cli(&c, MAILK_TLS_NONE, CAPS_FULL, NULL, NULL, 0);
    m.to = to;
    m.bdat = 1;
    m.body = (const uint8_t*)"a\n.b\nc";
    m.bodyn = 6;
    CHECK(mailk_cli_send(&c, &m) == MAILK_OK, "BDAT send");
    CHECK(out_is(&c, "MAIL FROM:<a@b.example> SIZE=10\r\nRCPT TO:<x@y.example>\r\nBDAT 10 LAST\r\na\r\n.b\r\nc\r\n"), "BDAT pipelined, CRLF canonical, no dot-stuffing");
    feed(&c, "250 ok\r\n250 ok\r\n250 2.0.0 queued\r\n");
    CHECK((c.ev & MAILK_EV_DONE) && !c.failed, "BDAT done");
    mailk_cli_free(&c);

    open_cli(&c, MAILK_TLS_NONE, CAPS_FULL_BIN, NULL, NULL, 0);
    {
        static const char bin[] = "a\n.b\0\rc";
        static const char want[] = "MAIL FROM:<a@b.example> SIZE=7 BODY=BINARYMIME\r\nRCPT TO:<x@y.example>\r\nBDAT 7 LAST\r\na\n.b\0\rc";
        m.body = (const uint8_t*)bin;
        m.bodyn = 7;
        CHECK(mailk_cli_send(&c, &m) == MAILK_OK, "BINARYMIME send");
        CHECK(c.out.n == sizeof want - 1 && !memcmp(c.out.p, want, sizeof want - 1), "BINARYMIME: octets exact, no CRLF fix-up, no final CRLF (RFC 3030 3)");
        c.out.n = 0;
        feed(&c, "250 ok\r\n250 ok\r\n250 2.0.0 queued\r\n");
        m.body = (const uint8_t*)big;
        m.bodyn = 999;
        CHECK(mailk_cli_send(&c, &m) == MAILK_OK && strstr(c.out.p, "BODY=BINARYMIME") && strstr(c.out.p, "BDAT 999 LAST"), "999-octet line goes as BINARYMIME");
        c.out.n = 0;
        feed(&c, "250 ok\r\n250 ok\r\n250 2.0.0 queued\r\n");
        m.body = (const uint8_t*)"a\r\nb\r\n";
        m.bodyn = 6;
        CHECK(mailk_cli_send(&c, &m) == MAILK_OK && out_is(&c, "MAIL FROM:<a@b.example> SIZE=6\r\nRCPT TO:<x@y.example>\r\nBDAT 6 LAST\r\na\r\nb\r\n"), "clean text stays 7BIT under BDAT");
    }
    mailk_cli_free(&c);
}

static void test_fuzz(void)
{
    static const char* rep[] = { "220 x\r\n", "250 ok\r\n", "250-a\r\n250 PIPELINING\r\n", "250-a\r\n250-STARTTLS\r\n250 AUTH PLAIN LOGIN SCRAM-SHA-256 CRAM-MD5\r\n",
        "354 go\r\n", "334 \r\n", "334 VXNlcm5hbWU6\r\n", "334 !!\r\n", "235 ok\r\n", "535 no\r\n", "421 bye\r\n", "550 no\r\n", "221 bye\r\n",
        "250-SIZE 10\r\n250 CHUNKING\r\n", "\r\n", "999 x\r\n", "25", "0 ok\r\n", "220 go\r\n250 x\r\n" };
    const char* to[] = { "x@y.example", "q@y.example" };
    int it;
    for (it = 0; it < 20000; it++) {
        mailk_cli_t c;
        mailk_cli_opts_t o;
        int k, steps = 1 + (int)(rnd() % 12);
        memset(&o, 0, sizeof o);
        o.tls = (int)(rnd() % 4);
        if (rnd() & 1) {
            o.user = "u";
            o.pass = "p";
            o.insecure_auth = (int)(rnd() & 1);
        }
        o.mechs = (unsigned)rnd() & 0xFF;
        o.scram_max_iters = 64;
        mailk_cli_init(&c, &o);
        for (k = 0; k < steps; k++) {
            char buf[128];
            size_t n;
            if (rnd() % 5 == 0) {
                n = rnd() % sizeof buf;
                for (size_t i = 0; i < n; i++)
                    buf[i] = (char)rnd();
            } else {
                const char* r = rep[rnd() % (sizeof rep / sizeof rep[0])];
                n = strlen(r);
                memcpy(buf, r, n);
            }
            if (rnd() % 3 == 0 && n > 1) {
                size_t cut = rnd() % n;
                mailk_cli_feed(&c, (const uint8_t*)buf, cut);
                mailk_cli_feed(&c, (const uint8_t*)buf + cut, n - cut);
            } else {
                mailk_cli_feed(&c, (const uint8_t*)buf, n);
            }
            if (c.ev & MAILK_EV_TLS && rnd() & 1)
                mailk_cli_tls_done(&c);
            if (c.ev & MAILK_EV_READY && rnd() % 3 == 0) {
                mailk_msg_t m;
                memset(&m, 0, sizeof m);
                m.from = "a@b.example";
                m.to = to;
                m.nto = 1 + rnd() % 2;
                m.body = (const uint8_t*)"x\n.\ny";
                m.bodyn = 5;
                m.bdat = (int)(rnd() & 1);
                mailk_cli_send(&c, &m);
            }
            c.out.n = 0;
        }
        mailk_cli_free(&c);
    }
    CHECK(1, "fuzz survived");
}


int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

typedef struct {
    int pipe, chunk, st, mail, nok, dead, drop_at, ncmd, viol, quit, drop_quit;
    uint64_t bdat_left;
    int bdat_last;
    dyn_sb_t in, out, body, got, codes;
} ms_t;

static void ms_init(ms_t* m, int pipe, int chunk, int drop_at)
{
    memset(m, 0, sizeof *m);
    m->pipe = pipe;
    m->chunk = chunk;
    m->drop_at = drop_at;
    dyn_sb_init(&m->in);
    dyn_sb_init(&m->out);
    dyn_sb_init(&m->body);
    dyn_sb_init(&m->got);
    dyn_sb_init(&m->codes);
    dyn_sb_puts(&m->out, "220 mx.model ESMTP\r\n");
}

static void ms_free(ms_t* m)
{
    dyn_sb_free(&m->in);
    dyn_sb_free(&m->out);
    dyn_sb_free(&m->body);
    dyn_sb_free(&m->got);
    dyn_sb_free(&m->codes);
}

static void ms_reply(ms_t* m, const char* r)
{
    dyn_sb_puts(&m->out, r);
}

static void ms_final(ms_t* m)
{
    int x = m->body.n && memchr(m->body.p, 'X', m->body.n) != NULL;
    dyn_sb_put(&m->got, m->body.p, m->body.n);
    dyn_sb_putc(&m->got, '|');
    ms_reply(m, x ? "550 5.7.1 rejected\r\n" : "250 2.0.0 queued\r\n");
    dyn_sb_putc(&m->codes, x ? 'R' : 'Q');
    m->body.n = 0;
    m->mail = 0;
    m->nok = 0;
}

static int ms_cmd(ms_t* m, const char* l, size_t n)
{
    m->ncmd++;
    if (m->drop_at >= 0 && m->ncmd > m->drop_at) {
        ms_reply(m, "421 4.4.2 closing\r\n");
        m->dead = 1;
        m->drop_quit = n == 4 && !memcmp(l, "QUIT", 4);
        return 0;
    }
    if (n >= 4 && !memcmp(l, "EHLO", 4)) {
        ms_reply(m, "250-mx.model\r\n250-8BITMIME\r\n250-SIZE 100000\r\n");
        if (m->pipe)
            ms_reply(m, "250-PIPELINING\r\n");
        if (m->chunk)
            ms_reply(m, "250-CHUNKING\r\n");
        ms_reply(m, "250 ENHANCEDSTATUSCODES\r\n");
    } else if (n >= 10 && !memcmp(l, "MAIL FROM:", 10)) {
        m->mail = 1;
        m->nok = 0;
        ms_reply(m, "250 2.1.0 ok\r\n");
    } else if (n >= 8 && !memcmp(l, "RCPT TO:", 8)) {
        int no = n >= 11 && !memcmp(l + 9, "no", 2);
        if (!m->mail)
            ms_reply(m, "503 5.5.1 no MAIL\r\n");
        else if (no)
            ms_reply(m, "550 5.1.1 no such user\r\n");
        else {
            m->nok++;
            ms_reply(m, "250 2.1.5 ok\r\n");
        }
    } else if (n == 4 && !memcmp(l, "DATA", 4)) {
        if (!m->mail)
            ms_reply(m, "503 5.5.1 no MAIL\r\n");
        else if (!m->nok)
            ms_reply(m, "554 5.5.1 no valid recipients\r\n");
        else {
            ms_reply(m, "354 go\r\n");
            m->st = 1;
        }
    } else if (n >= 5 && !memcmp(l, "BDAT ", 5)) {
        char* e;
        m->bdat_left = strtoull(l + 5, &e, 10);
        m->bdat_last = n >= 9 && !memcmp(l + n - 4, "LAST", 4);
        (void)e;
        m->st = 2;
    } else if (n == 4 && !memcmp(l, "RSET", 4)) {
        m->mail = 0;
        m->nok = 0;
        m->body.n = 0;
        ms_reply(m, "250 2.0.0 ok\r\n");
    } else if (n == 4 && !memcmp(l, "QUIT", 4)) {
        ms_reply(m, "221 2.0.0 bye\r\n");
        m->quit = 1;
    } else
        ms_reply(m, "500 5.5.2 what\r\n");
    return 1;
}

static void ms_take(ms_t* m, const uint8_t* p, size_t n)
{
    size_t at = 0;
    dyn_sb_put(&m->in, (const char*)p, n);
    while (!m->dead && (at < m->in.n || (m->st == 2 && !m->bdat_left))) {
        const char* s = m->in.p + at;
        size_t left = m->in.n - at;
        if (m->st == 0) {
            const char* e = NULL;
            size_t k;
            for (k = 0; k + 1 < left; k++)
                if (s[k] == '\r' && s[k + 1] == '\n') {
                    e = s + k;
                    break;
                }
            if (!e)
                break;
            ms_cmd(m, s, (size_t)(e - s));
            at += (size_t)(e - s) + 2;
        } else if (m->st == 1) {
            size_t k = 0, ls;
            int done = 0;
            while (k < left) {
                const char* e = NULL;
                size_t q;
                for (q = k; q + 1 < left; q++)
                    if (s[q] == '\r' && s[q + 1] == '\n') {
                        e = s + q;
                        break;
                    }
                if (!e)
                    break;
                ls = (size_t)(e - (s + k));
                if (ls == 1 && s[k] == '.') {
                    k += 3;
                    done = 1;
                    break;
                }
                dyn_sb_put(&m->body, s + k + (s[k] == '.'), ls - (s[k] == '.'));
                dyn_sb_put(&m->body, "\r\n", 2);
                k += ls + 2;
            }
            at += k;
            if (done) {
                m->st = 0;
                if (m->nok)
                    ms_final(m);
                else
                    ms_reply(m, "554 5.5.1 no valid recipients\r\n");
            } else
                break;
        } else {
            size_t take = left < m->bdat_left ? left : (size_t)m->bdat_left;
            dyn_sb_put(&m->body, s, take);
            at += take;
            m->bdat_left -= take;
            if (m->bdat_left)
                break;
            m->st = 0;
            if (!m->mail)
                ms_reply(m, "503 5.5.1 no MAIL\r\n");
            else if (!m->nok)
                ms_reply(m, "554 5.5.1 no valid recipients\r\n");
            else if (m->bdat_last)
                ms_final(m);
            else
                ms_reply(m, "250 2.0.0 chunk ok\r\n");
        }
    }
    memmove(m->in.p, m->in.p + at, m->in.n - at);
    m->in.n -= at;
}

static void cli_normalise(const uint8_t* p, size_t n, dyn_sb_t* o)
{
    size_t i, o0 = o->n;
    for (i = 0; i < n; i++) {
        if (p[i] == '\r' || p[i] == '\n') {
            dyn_sb_put(o, "\r\n", 2);
            if (p[i] == '\r' && i + 1 < n && p[i + 1] == '\n')
                i++;
        } else
            dyn_sb_putc(o, (char)p[i]);
    }
    if (o->n > o0 && !(o->n - o0 >= 2 && o->p[o->n - 2] == '\r' && o->p[o->n - 1] == '\n'))
        dyn_sb_put(o, "\r\n", 2);
}

static size_t count_cmds(const char* p, size_t n)
{
    size_t k, c = 0;
    for (k = 0; k + 1 < n; k++)
        if (p[k] == '\r' && p[k + 1] == '\n')
            c++;
    return c;
}

typedef struct {
    dyn_sb_t wire, res;
    int viol, hang;
} trace_t;

static void cli_to_ms(mailk_cli_t* c, ms_t* m, trace_t* t, int split, int cmd_mode)
{
    size_t at = 0;
    if (!c->out.n)
        return;
    if (cmd_mode && !m->pipe && m->st == 0 && count_cmds(c->out.p, c->out.n) > 1 && !(c->out.n > 5 && !memcmp(c->out.p, "BDAT ", 5)))
        t->viol++;
    dyn_sb_put(&t->wire, c->out.p, c->out.n);
    while (at < c->out.n) {
        size_t ch = split ? 1 + rnd() % 11 : c->out.n - at;
        if (ch > c->out.n - at)
            ch = c->out.n - at;
        ms_take(m, (const uint8_t*)c->out.p + at, ch);
        at += ch;
    }
    c->out.n = 0;
}

static void ms_to_cli(ms_t* m, mailk_cli_t* c, int split)
{
    size_t at = 0;
    while (at < m->out.n && !(c->ev & (MAILK_EV_ERROR | MAILK_EV_CLOSED))) {
        size_t ch = split ? 1 + rnd() % 7 : m->out.n - at;
        if (ch > m->out.n - at)
            ch = m->out.n - at;
        mailk_cli_feed(c, (const uint8_t*)m->out.p + at, ch);
        at += ch;
    }
    m->out.n = 0;
}

typedef struct {
    char tob[3][24];
    const char* to[3];
    size_t nto;
    int accepted, x, bdat;
    char body[400];
    size_t bodyn;
} cm_t;

static const mailk_msg_t zero_msg;

static void run_session(const cm_t* msgs, int nm, int pipe, int chunk, int drop_at, int split, trace_t* t, ms_t* m)
{
    mailk_cli_opts_t o;
    mailk_cli_t c;
    int k, g;
    char b[64];
    memset(&o, 0, sizeof o);
    o.name = "client.example";
    dyn_sb_init(&t->wire);
    dyn_sb_init(&t->res);
    t->viol = t->hang = 0;
    ms_init(m, pipe, chunk, drop_at);
    mailk_cli_init(&c, &o);
    for (g = 0; g < 20 && !(c.ev & (MAILK_EV_READY | MAILK_EV_ERROR)); g++) {
        ms_to_cli(m, &c, split);
        cli_to_ms(&c, m, t, split, 1);
    }
    for (k = 0; k < nm && !(c.ev & MAILK_EV_ERROR); k++) {
        mailk_msg_t mm;
        size_t j;
        memset(&mm, 0, sizeof mm);
        mm.from = "a@b.example";
        mm.to = msgs[k].to;
        mm.nto = msgs[k].nto;
        mm.body = (const uint8_t*)msgs[k].body;
        mm.bodyn = msgs[k].bodyn;
        mm.bdat = msgs[k].bdat;
        c.ev &= ~(MAILK_EV_READY | MAILK_EV_DONE);
        if (mailk_cli_send(&c, &mm) != MAILK_OK) {
            dyn_sb_puts(&t->res, "send-refused;");
            break;
        }
        for (g = 0; g < 60 && !(c.ev & (MAILK_EV_DONE | MAILK_EV_ERROR)); g++) {
            cli_to_ms(&c, m, t, split, m->st == 0);
            ms_to_cli(m, &c, split);
        }
        if (!(c.ev & (MAILK_EV_DONE | MAILK_EV_ERROR)))
            t->hang++;
        snprintf(b, sizeof b, "m%d:%s ok=%d failed=%d final=%d rc=", k, c.ev & MAILK_EV_ERROR ? "ERR" : "DONE", c.ok, c.failed, c.final.code);
        dyn_sb_puts(&t->res, b);
        for (j = 0; j < msgs[k].nto && !(c.ev & MAILK_EV_ERROR); j++) {
            snprintf(b, sizeof b, "%d,", c.rcpt[j].code);
            dyn_sb_puts(&t->res, b);
        }
        dyn_sb_putc(&t->res, ';');
    }
    if (!(c.ev & MAILK_EV_ERROR)) {
        mailk_cli_quit(&c);
        for (g = 0; g < 10 && !(c.ev & (MAILK_EV_CLOSED | MAILK_EV_ERROR)); g++) {
            cli_to_ms(&c, m, t, split, 1);
            ms_to_cli(m, &c, split);
        }
        dyn_sb_puts(&t->res, c.ev & MAILK_EV_CLOSED ? "closed" : "not-closed");
    } else
        dyn_sb_puts(&t->res, mailk_cli_send(&c, &zero_msg) == MAILK_OK ? "send-after-error-ok" : "dead");
    mailk_cli_free(&c);
}

static void test_model(void)
{
    long it, N = (long)fk_iters(2500);
    int bad_model = 0, bad_body = 0, bad_pipe = 0, bad_split = 0, bad_drop = 0, bad_hang = 0;
    dyn_sb_t want, res;
    fk_g = fk_stream("cli-model");
    dyn_sb_init(&want);
    dyn_sb_init(&res);
    for (it = 0; it < N; it++) {
        static const char al[] = "ab .\r\nzz..\r\n\r\n";
        cm_t msgs[4];
        int nm = 1 + (int)(rnd() % 4), k, j, pipe = (int)(rnd() & 1), chunk = (int)(rnd() & 1), drop_at = rnd() % 5 ? -1 : (int)(rnd() % 14);
        trace_t a, b;
        ms_t ma, mb;
        char e[64];
        for (k = 0; k < nm; k++) {
            size_t q;
            msgs[k].nto = 1 + rnd() % 3;
            msgs[k].accepted = 0;
            for (j = 0; j < (int)msgs[k].nto; j++) {
                int no = rnd() % 4 == 0;
                snprintf(msgs[k].tob[j], sizeof msgs[k].tob[j], "%s%d@y.example", no ? "no" : "u", j);
                msgs[k].to[j] = msgs[k].tob[j];
                msgs[k].accepted += !no;
            }
            msgs[k].bodyn = rnd() % 300;
            for (q = 0; q < msgs[k].bodyn; q++)
                msgs[k].body[q] = al[rnd() % (sizeof al - 1)];
            msgs[k].x = rnd() % 5 == 0;
            if (msgs[k].x)
                msgs[k].body[msgs[k].bodyn++] = 'X';
            msgs[k].bdat = (int)(rnd() & 1);
        }
        run_session(msgs, nm, pipe, chunk, drop_at, 0, &a, &ma);
        run_session(msgs, nm, pipe, chunk, drop_at, 1, &b, &mb);
        if (a.viol + b.viol && bad_pipe++ < 3)
            CHECK(0, "[no-pipe] client sent several commands in one write without PIPELINING it=%ld", it);
        if (a.hang + b.hang && bad_hang++ < 3)
            CHECK(0, "[cli-term] a message never reached DONE or ERROR it=%ld", it);
        if ((a.wire.n != b.wire.n || memcmp(a.wire.p, b.wire.p, a.wire.n) || a.res.n != b.res.n || memcmp(a.res.p, b.res.p, a.res.n)) && bad_split++ < 3)
            CHECK(0, "[split-inv] whole and split replies give different client behaviour it=%ld: [%.*s] vs [%.*s]", it, (int)a.res.n, a.res.p, (int)b.res.n, b.res.p);
        if (drop_at < 0) {
            res.n = 0;
            want.n = 0;
            for (k = 0; k < nm; k++) {
                int ok = msgs[k].accepted > 0;
                snprintf(e, sizeof e, "m%d:DONE ok=%d failed=%d final=%d rc=", k, msgs[k].accepted, !ok || msgs[k].x, !ok ? (pipe ? 554 : 0) : msgs[k].x ? 550 : 250);
                dyn_sb_puts(&res, e);
                for (j = 0; j < (int)msgs[k].nto; j++) {
                    snprintf(e, sizeof e, "%d,", msgs[k].to[j][0] == 'n' ? 550 : 250);
                    dyn_sb_puts(&res, e);
                }
                dyn_sb_putc(&res, ';');
                if (ok) {
                    cli_normalise((const uint8_t*)msgs[k].body, msgs[k].bodyn, &want);
                    dyn_sb_putc(&want, '|');
                }
            }
            dyn_sb_puts(&res, "closed");
            if ((a.res.n != res.n || memcmp(a.res.p, res.p, res.n)) && bad_model++ < 3)
                CHECK(0, "[cli-model] results differ from the model it=%ld pipe %d chunk %d:\n   got  [%.*s]\n   want [%.*s]", it, pipe, chunk, (int)a.res.n, a.res.p, (int)res.n, res.p);
            if ((ma.got.n != want.n || (want.n && memcmp(ma.got.p, want.p, want.n))) && bad_body++ < 3)
                CHECK(0, "[cli-body] delivered bodies differ from the CRLF-normalised input it=%ld (%zu vs %zu bytes)", it, ma.got.n, want.n);
        } else {
            const char* tail = a.res.n >= 4 ? a.res.p + a.res.n - 4 : "";
            if ((a.res.n < 4 || memcmp(tail, "dead", 4)) && ma.dead && !ma.drop_quit && bad_drop++ < 3)
                CHECK(0, "[drop] a 421 mid-session did not leave the client dead it=%ld drop_at %d: [%.*s]", it, drop_at, (int)a.res.n, a.res.p);
        }
        dyn_sb_free(&a.wire);
        dyn_sb_free(&a.res);
        dyn_sb_free(&b.wire);
        dyn_sb_free(&b.res);
        ms_free(&ma);
        ms_free(&mb);
    }
    CHECK(!bad_model && !bad_body && !bad_pipe && !bad_split && !bad_drop && !bad_hang, "[cli-model] %ld sessions against the model server: model %d body %d pipe %d split %d drop %d term %d bad", N,
        bad_model, bad_body, bad_pipe, bad_split, bad_drop, bad_hang);
    dyn_sb_free(&want);
    dyn_sb_free(&res);
}

static void test_fuzz_entry(void)
{
    static const char* const seeds[] = {
        "\x00" "220 mx\r\n250-mx\r\n250 PIPELINING\r\n250 ok\r\n250 ok\r\n354 go\r\n250 queued\r\n",
        "\x05" "220 mx\r\n250-mx\r\n250-AUTH PLAIN LOGIN SCRAM-SHA-256\r\n250 CHUNKING\r\n334 \r\n235 ok\r\n250 ok\r\n250 ok\r\n250 done\r\n",
        "\x01" "220 mx\r\n250-mx\r\n250 STARTTLS\r\n220 go\r\n",
        "\x04" "220 mx\r\n250 mx\r\n250 ok\r\n550 no\r\n503 no\r\n250 reset\r\n",
        "\x00" "250 ok\r\n.\r\n354 go\r\n..x\r\n",
    };
    uint8_t buf[1024];
    long it, N = (long)fk_iters(10000);
    fk_g = fk_stream("cli-fuzz-entry");
    printf("[fuzz-cli] start\n");
    fflush(stdout);
    for (it = 0; it < N; it++) {
        const char* sd = seeds[rnd() % 5];
        size_t n = (sd[0] ? strlen(sd) : 1 + strlen(sd + 1)), k, m = 1 + rnd() % 4;
        memcpy(buf, sd, n);
        buf[0] = (uint8_t)rnd();
        for (k = 0; k < m && n > 2; k++) {
            size_t at = 1 + rnd() % (n - 1);
            switch (rnd() % 3) {
            case 0:
                buf[at] = (uint8_t)"0123459 -\r\n"[rnd() % 11];
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
    printf("[fuzz-cli] done\n");
    CHECK(1, "[fuzz-cli] %ld mutated server transcripts through src/fuzz/fuzz_mailk.c", N);
}

int main(void)
{
    fk_init("test_mailk_client");
    fflush(stdout);
    fk_g = fk_stream("fixed");
    test_pipelined();
    test_serial();
    test_starttls();
    test_auth();
    scram_case(0, 0);
    scram_case(1, 0);
    scram_case(0, 1);
    test_msg_checks();
    fk_g = fk_stream("fuzz");
    test_fuzz();
    test_model();
    test_fuzz_entry();    printf("%d checks\n", checks);
    fk_fails += fails;
    return fk_done(__FILE__);
}
