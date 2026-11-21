/*
 * test_dkim_core.c -- src/core/dkim (dyn-dkim.h), the pure-C DKIM engine behind dyna:dkim (S9 M3,
 * 2026-11-27): message field split, body/header canonicalization, header hash with b= blanked,
 * DKIM-Signature tag-list and key-record parsing, signature field construction. No crypto here:
 * the module signs/verifies the header hash with OpenSSL on a pool thread.
 *
 * External oracles:
 *  - RFC 6376 3.4.3/3.4.4: empty body is CRLF under simple ("frcCV1k9...") and "" under relaxed
 *    ("47DEQpj8..."); 3.4.5 example (a:X / b:Y Z, " C\r\nD E\r\n", " C \r\nD \t E\r\n").
 *  - RFC 8463 Appendix A: relaxed bh=2jUSOH9NhtVGCQWNr9BrIAPreKQjO6Sn7XIkfJVOzv8= of the
 *    "Hi. / We lost the game..." body.
 *  - RFC 6376 3.2 tag-list (duplicate tag = invalid), 3.5 required tags v a b bh d h s, i= within
 *    d=, h= must contain From (5.4), x= not before t=; RFC 8301 refuses rsa-sha1; 3.6.1 key record
 *    (v=DKIM1 first, k=, h=, s=, t=y:s, empty p= = revoked); 3.7 b= value blanked, field last,
 *    no trailing CRLF; 5.4.2 multiple instances signed bottom-up, absent instances hash nothing.
 * Self-consistency: dkim_sign_field's header hash equals dkim_hdr_hash over the signed message
 * (every canonicalization pair, l=, oversigning, folding), for random messages.
 * Fuzz: 30000 random/mutated inputs through msg_parse, sig_parse, key_parse, b64, body feed split at
 * random points under ASan/UBSan; streaming body hash equals the one-shot hash.
 * Mutants (each red here): relaxed trailing-WSP drop removed, trailing-empty-line deferral removed,
 * bare CR line end ignored, b= blanking removed, bottom-up instance order reversed, duplicate-tag
 * check removed, i=/d= subdomain check removed, rsa-sha1 accepted, key v= position check removed.
 * Seeded property tests (tests/lib/dyn-fuzzkit.h, 2026-11-27): DYN_FUZZ_SEED replays a run (the seed is
 * printed first), DYN_FUZZ_ITERS (integer, 50 = deep run) multiplies every loop. Each tagged check has its own mutant:
 *  [diff-body]   streaming body canon (random chunks, l=) vs a whole-buffer multi-pass model; mutant:
 *                CR state dropped at each feed (CRLF split across chunks).
 *  [diff-hdr]    dkim_hdr_canon vs a naive unfold/collapse/trim model; mutant: leading WSP kept.
 *  [diff-select] dkim_hdr_hash vs a bottom-up h= selection model over generated messages with folds,
 *                random case, oversigned and absent names, b= anywhere; mutant: used-instance mark dropped.
 *  [tag-model]   generated tag-lists (random FWS, order, case) with one injected fault (missing,
 *                duplicate, bad l=, rsa-sha1, x<t, i= outside d=, t= overflow) vs predicted outcome;
 *                mutant: trailing FWS of a value kept.
 *  [bound]       1000/1001 fields, h= 128/129, 64/65 tags, l= UINT64_MAX-1/UINT64_MAX/21 digits, t=/x=
 *                INT64_MAX/+1, x=t/t-1, l=0/len/len-1; mutants: tag cap and h= cap off by one.
 *  [fuzz-entry]  src/fuzz/fuzz_dkim.c (libFuzzer target; aborts on body/header canon vs model, split body,
 *                sig/key parse-serialise and FWS invariance, b64, sign/verify loss); mutant: b64 1-byte pad.
 * d= is now checked as a domain (fuzz found d=@ accepted while its default i=@@ is refused).
 * The tag cap was off by one (64 tags refused); now 64 are accepted and the 65th refused.
 */
#include "dyn-dkim.h"
#include "../lib/dyn-fuzzkit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../lib/dyn-xalloc.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static fk_rng_t fk_g;

static uint64_t rnd(void)
{
    return fk_next(&fk_g);
}

static void b64(const uint8_t* p, size_t n, char* out)
{
    dyn_sb_t s;
    dyn_sb_init(&s);
    dkim_b64_encode(&s, p, n);
    memcpy(out, s.p, s.n);
    out[s.n] = 0;
    dyn_sb_free(&s);
}

static void body_b64(const char* body, size_t n, int relaxed, uint64_t limit, char* out)
{
    dkim_body_t b;
    uint8_t h[32];
    dkim_body_init(&b, relaxed, limit);
    dkim_body_feed(&b, (const uint8_t*)body, n);
    dkim_body_final(&b, h);
    b64(h, 32, out);
}

static void sha_b64(const char* s, size_t n, char* out)
{
    uint8_t h[32];
    dyn_sha256((const uint8_t*)s, n, h);
    b64(h, 32, out);
}

static void t_empty_body(void)
{
    char g[64];
    body_b64("", 0, 0, DKIM_NO_LIMIT, g);
    CHECK(!strcmp(g, "frcCV1k9oG9oKj3dpUqdJg1PxRT2RSN/XKdLCPjaYaY="), "simple empty %s", g);
    body_b64("", 0, 1, DKIM_NO_LIMIT, g);
    CHECK(!strcmp(g, "47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFU="), "relaxed empty %s", g);
    body_b64("\r\n\r\n\r\n", 6, 0, DKIM_NO_LIMIT, g);
    CHECK(!strcmp(g, "frcCV1k9oG9oKj3dpUqdJg1PxRT2RSN/XKdLCPjaYaY="), "simple blank lines %s", g);
    body_b64(" \t\r\n \r\n", 7, 1, DKIM_NO_LIMIT, g);
    CHECK(!strcmp(g, "47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFU="), "relaxed wsp lines %s", g);
}

static const char ex345[] = "A: X\r\nB : Y\t\r\n\tZ  \r\n\r\n C \r\nD \t E\r\n\r\n\r\n";

static void t_rfc6376_345(void)
{
    dkim_msg_t* m = xcalloc(1, sizeof *m);
    dyn_sb_t c;
    char g[64], w[64];
    CHECK(dkim_msg_parse(m, (const uint8_t*)ex345, sizeof ex345 - 1) == DKIM_OK, "parse");
    CHECK(m->nf == 2, "nf %zu", m->nf);
    dyn_sb_init(&c);
    dkim_hdr_canon(&c, m->p + m->f[0].off, m->f[0].len, m->f[0].nlen, 1);
    dyn_sb_put(&c, "\r\n", 2);
    dkim_hdr_canon(&c, m->p + m->f[1].off, m->f[1].len, m->f[1].nlen, 1);
    dyn_sb_put(&c, "\r\n", 2);
    CHECK(c.n == 12 && !memcmp(c.p, "a:X\r\nb:Y Z\r\n", 12), "relaxed headers [%.*s]", (int)c.n, c.p);
    c.n = 0;
    dkim_hdr_canon(&c, m->p + m->f[1].off, m->f[1].len, m->f[1].nlen, 0);
    CHECK(c.n == 12 && !memcmp(c.p, "B : Y\t\r\n\tZ  ", 12), "simple header [%.*s]", (int)c.n, c.p);
    dyn_sb_free(&c);
    body_b64(ex345 + m->body, sizeof ex345 - 1 - m->body, 1, DKIM_NO_LIMIT, g);
    sha_b64(" C\r\nD E\r\n", 9, w);
    CHECK(!strcmp(g, w), "relaxed body %s want %s", g, w);
    body_b64(ex345 + m->body, sizeof ex345 - 1 - m->body, 0, DKIM_NO_LIMIT, g);
    sha_b64(" C \r\nD \t E\r\n", 12, w);
    CHECK(!strcmp(g, w), "simple body %s want %s", g, w);
    dkim_msg_free(m);
    free(m);
}

static const char rfc8463_body[] = "Hi.\r\n\r\nWe lost the game.  Are you hungry yet?\r\n\r\nJoe.\r\n";

static void t_rfc8463_bh(void)
{
    char g[64];
    body_b64(rfc8463_body, sizeof rfc8463_body - 1, 1, DKIM_NO_LIMIT, g);
    CHECK(!strcmp(g, "2jUSOH9NhtVGCQWNr9BrIAPreKQjO6Sn7XIkfJVOzv8="), "rfc8463 bh %s", g);
    body_b64("Hi.\n\nWe lost the game.  Are you hungry yet?\n\nJoe.\n\n\n", 52, 1, DKIM_NO_LIMIT, g);
    CHECK(!strcmp(g, "2jUSOH9NhtVGCQWNr9BrIAPreKQjO6Sn7XIkfJVOzv8="), "rfc8463 bh LF %s", g);
    body_b64("Hi.\r\rWe lost the game.  Are you hungry yet?\r\rJoe.", 49, 1, DKIM_NO_LIMIT, g);
    CHECK(!strcmp(g, "2jUSOH9NhtVGCQWNr9BrIAPreKQjO6Sn7XIkfJVOzv8="), "rfc8463 bh bare CR %s", g);
}

static void t_body_misc(void)
{
    char g[64], w[64];
    body_b64("a  b \t\r\n\r\nc", 11, 1, DKIM_NO_LIMIT, g);
    sha_b64("a b\r\n\r\nc\r\n", 10, w);
    CHECK(!strcmp(g, w), "relaxed inner %s", g);
    body_b64("a  b \t\r\n\r\nc", 11, 0, DKIM_NO_LIMIT, g);
    sha_b64("a  b \t\r\n\r\nc\r\n", 13, w);
    CHECK(!strcmp(g, w), "simple unterminated %s", g);
    body_b64("abc\r\ndef\r\n", 10, 0, 4, g);
    sha_b64("abc\r", 4, w);
    CHECK(!strcmp(g, w), "l=4 %s", g);
    body_b64("abc\r\n\r\n", 7, 0, 0, g);
    sha_b64("", 0, w);
    CHECK(!strcmp(g, w), "l=0 %s", g);
    {
        dkim_body_t b;
        uint8_t h[32];
        dkim_body_init(&b, 0, DKIM_NO_LIMIT);
        dkim_body_feed(&b, (const uint8_t*)"x\r\n\r\n", 5);
        dkim_body_final(&b, h);
        CHECK(b.n == 3, "simple length counts deferred blanks out %llu", (unsigned long long)b.n);
    }
}

static void t_stream(void)
{
    int it;
    static const char al[] = "ab \t\r\n";
    for (it = 0; it < 4000; it++) {
        char buf[300];
        size_t n = rnd() % sizeof buf, i, cut;
        int rel = (int)(rnd() & 1);
        dkim_body_t a, b;
        uint8_t ha[32], hb[32];
        for (i = 0; i < n; i++)
            buf[i] = al[rnd() % 6];
        dkim_body_init(&a, rel, DKIM_NO_LIMIT);
        dkim_body_feed(&a, (const uint8_t*)buf, n);
        dkim_body_final(&a, ha);
        dkim_body_init(&b, rel, DKIM_NO_LIMIT);
        for (i = 0; i < n; i = cut) {
            cut = i + 1 + rnd() % 7;
            if (cut > n)
                cut = n;
            dkim_body_feed(&b, (const uint8_t*)buf + i, cut - i);
        }
        dkim_body_final(&b, hb);
        CHECK(!memcmp(ha, hb, 32) && a.n == b.n, "stream split differs it=%d", it);
        if (memcmp(ha, hb, 32))
            break;
    }
}

static void t_msg_parse(void)
{
    dkim_msg_t* m = xcalloc(1, sizeof *m);
    static const char a[] = "From: a\n\tb\r\nTo:c\r\rbody";
    char* big;
    size_t i;
    CHECK(dkim_msg_parse(m, (const uint8_t*)a, sizeof a - 1) == DKIM_OK && m->nf == 2, "mixed eol");
    CHECK(m->f[0].len == 10 && m->f[0].nlen == 4, "fold len %zu", m->f[0].len);
    CHECK(m->body == sizeof a - 5, "body off %zu", m->body);
    CHECK(dkim_msg_parse(m, (const uint8_t*)" x: y\r\n\r\n", 9) == DKIM_EBAD, "leading continuation");
    CHECK(dkim_msg_parse(m, (const uint8_t*)"nocolon\r\n\r\n", 11) == DKIM_EBAD, "no colon");
    CHECK(dkim_msg_parse(m, (const uint8_t*)": x\r\n\r\n", 7) == DKIM_EBAD, "empty name");
    CHECK(dkim_msg_parse(m, (const uint8_t*)"A: x", 4) == DKIM_OK && m->body == 4 && m->nf == 1, "no body");
    big = malloc(1001 * 5 + 2);
    for (i = 0; i < 1001; i++)
        memcpy(big + i * 5, "X:1\r\n", 5);
    memcpy(big + 5005, "\r\n", 2);
    CHECK(dkim_msg_parse(m, (const uint8_t*)big, 5005 + 2) == DKIM_ELONG, "field cap");
    CHECK(dkim_msg_parse(m, (const uint8_t*)big, 5000) == DKIM_OK && m->nf == 1000, "1000 ok");
    free(big);
    dkim_msg_free(m);
    free(m);
}

static const char* sig_err(const char* v)
{
    static char keep[128];
    dkim_sig_t s;
    const char* e;
    dkim_sig_init(&s);
    e = dkim_sig_parse(&s, (const uint8_t*)v, strlen(v));
    snprintf(keep, sizeof keep, "%s", e ? e : "");
    dkim_sig_free(&s);
    return e ? keep : NULL;
}

#define BH "bh=2jUSOH9NhtVGCQWNr9BrIAPreKQjO6Sn7XIkfJVOzv8="
#define OKSIG "v=1; a=rsa-sha256; d=example.com; s=sel; h=from:to; " BH "; b=AAAA"

static void t_sig_parse(void)
{
    dkim_sig_t s;
    const char* e;
    static const char good[] = " v=1; a=ed25519-sha256; c=relaxed/relaxed;\r\n d=Football.Example.com; i=joe@mail.football.example.com;\r\n"
                               " q=dns/txt; s=brisbane; t=1528637909; x=1528637999; l=10; h=from : to :\r\n subject : From;\r\n " BH ";\r\n b=/gCr inpc\r\n QA==";
    dkim_sig_init(&s);
    e = dkim_sig_parse(&s, (const uint8_t*)good, sizeof good - 1);
    CHECK(!e, "good: %s", e);
    CHECK(s.algo == DKIM_ED25519 && s.hrel && s.brel && s.t == 1528637909 && s.x == 1528637999 && s.l == 10, "fields");
    CHECK(s.nh == 4 && !memcmp(s.h.p, "from\0to\0subject\0from\0", 21), "h list");
    CHECK(s.b.n == 7, "b len %zu", s.b.n);
    CHECK(s.d.n == 20 && s.s.n == 8, "d s");
    dkim_sig_free(&s);
    dkim_sig_init(&s);
    CHECK(!dkim_sig_parse(&s, (const uint8_t*)OKSIG, strlen(OKSIG)), "ok sig");
    CHECK(!s.hrel && !s.brel && s.l == DKIM_NO_LIMIT && s.t == -1 && s.i.n == 12 && !memcmp(s.i.p, "@example.com", 12), "defaults");
    dkim_sig_free(&s);
    CHECK(!sig_err(OKSIG "; c=relaxed"), "c relaxed only");
    CHECK(!sig_err(OKSIG "; zz=unknown;"), "unknown tag and trailing ;");
    CHECK(!sig_err(OKSIG "; i=@sub.example.com"), "subdomain i");
    CHECK((e = sig_err(OKSIG "; i=@badexample.com")) && strstr(e, "within"), "i not within d: %s", e);
    CHECK((e = sig_err(OKSIG "; i=x@example.org")) && strstr(e, "within"), "i other");
    CHECK((e = sig_err(OKSIG "; i=noat")) && strstr(e, "within"), "i without @");
    CHECK((e = sig_err(OKSIG "; d=x.com")) && strstr(e, "duplicate"), "dup d");
    CHECK((e = sig_err(OKSIG "; zz=1; zz=2")) && strstr(e, "duplicate"), "dup unknown");
    CHECK((e = sig_err("v=1; a=rsa-sha1; d=example.com; s=sel; h=from; " BH "; b=AAAA")) && strstr(e, "8301"), "sha1");
    CHECK((e = sig_err("v=1; a=rsa-md5; d=example.com; s=sel; h=from; " BH "; b=AAAA")) && strstr(e, "unsupported"), "md5");
    CHECK((e = sig_err("v=2; a=rsa-sha256; d=example.com; s=sel; h=from; " BH "; b=AAAA")) && strstr(e, "v="), "v=2");
    CHECK((e = sig_err("a=rsa-sha256; d=example.com; s=sel; h=from; " BH "; b=AAAA")) && strstr(e, "missing"), "no v");
    CHECK((e = sig_err("v=1; a=rsa-sha256; d=example.com; s=sel; h=from; " BH)) && strstr(e, "missing"), "no b");
    CHECK((e = sig_err("v=1; a=rsa-sha256; d=example.com; s=sel; h=to:subject; " BH "; b=AAAA")) && strstr(e, "from"), "no from");
    CHECK((e = sig_err("v=1; a=rsa-sha256; d=example.com; s=sel; h=from; bh=AAAA; b=AAAA")) && strstr(e, "bh"), "bh len");
    CHECK((e = sig_err(OKSIG "; q=http/foo")) && strstr(e, "q="), "q");
    CHECK(!sig_err(OKSIG "; q=http/foo:DNS/TXT"), "q list");
    CHECK((e = sig_err(OKSIG "; c=fancy/simple")) && strstr(e, "canon"), "c bad");
    CHECK((e = sig_err(OKSIG "; c=simple/")) && strstr(e, "canon"), "c empty body");
    CHECK((e = sig_err(OKSIG "; t=200; x=100")) && strstr(e, "before"), "x<t");
    CHECK(!sig_err(OKSIG "; t=100; x=100"), "x==t");
    CHECK((e = sig_err(OKSIG "; l=99999999999999999999999")) && strstr(e, "l="), "l overflow");
    CHECK((e = sig_err(OKSIG "; l=-1")) && strstr(e, "l="), "l negative");
    CHECK((e = sig_err(OKSIG "; t=9223372036854775808")) && strstr(e, "t="), "t overflow");
    CHECK((e = sig_err("v=1; a=rsa-sha256; d=example.com; s=sel; h=from::to; " BH "; b=AAAA")) && strstr(e, "h="), "h empty entry");
    CHECK((e = sig_err("v=1; a=rsa-sha256; d=example.com; s=sel; h=from; " BH "; b=A*AA")) && strstr(e, "b="), "b bad char");
    CHECK((e = sig_err("v=1; a=rsa-sha256; d=example.com; s=sel; h=from; " BH "; b=AAA")) && strstr(e, "b="), "b bad len");
    CHECK((e = sig_err("v=1; a=rsa-sha256; d=; s=sel; h=from; " BH "; b=AAAA")) && strstr(e, "d="), "d empty");
    CHECK((e = sig_err("v=1;; a=rsa-sha256")) && strstr(e, "syntax"), "empty tag");
    CHECK((e = sig_err("v 1")) && strstr(e, "syntax"), "no =");
    CHECK((e = sig_err("1v=1")) && strstr(e, "syntax"), "digit name");
    CHECK((e = sig_err("v=\x01")) && strstr(e, "syntax"), "ctl value");
    {
        char big[2000];
        size_t o = (size_t)snprintf(big, sizeof big, "v=1; a=rsa-sha256; d=example.com; s=sel; " BH "; b=AAAA; h=from");
        int i;
        for (i = 0; i < 128; i++)
            o += (size_t)snprintf(big + o, sizeof big - o, ":x");
        CHECK((e = sig_err(big)) && strstr(e, "too many"), "h cap %s", e ? e : "-");
        o = 0;
        for (i = 0; i < 70; i++)
            o += (size_t)snprintf(big + o, sizeof big - o, "t%d=1;", i);
        CHECK((e = sig_err(big)) && strstr(e, "too many"), "tag cap");
    }
}

static const char* key_err(const char* v, dkim_key_t* out)
{
    static char keep[128];
    dkim_key_t k;
    const char* e;
    dkim_key_init(&k);
    e = dkim_key_parse(&k, (const uint8_t*)v, strlen(v));
    snprintf(keep, sizeof keep, "%s", e ? e : "");
    if (out)
        *out = k;
    else
        dkim_key_free(&k);
    return e ? keep : NULL;
}

static void t_key_parse(void)
{
    dkim_key_t k;
    const char* e;
    CHECK(!key_err("v=DKIM1; k=ed25519; p=11qYAYKxCrfVS/7TyWQHOg7hcvPapiMlrwIaaPcHURo=", &k), "rfc8463 key");
    CHECK(k.k == DKIM_ED25519 && k.p.n == 32 && k.p.p[0] == (char)0xd7 && k.p.p[31] == 0x1a && !k.revoked, "ed key bytes");
    dkim_key_free(&k);
    CHECK(!key_err("p=AAAA", &k) && k.k == DKIM_RSA && k.p.n == 3, "rsa default");
    dkim_key_free(&k);
    CHECK(!key_err("v=DKIM1; p=", &k) && k.revoked, "revoked");
    dkim_key_free(&k);
    CHECK(!key_err("v=DKIM1; t=y:s; s=email; h=sha1:sha256; n=notes here; p=AAAA", &k) && k.testing && k.strict, "flags");
    dkim_key_free(&k);
    CHECK(!key_err("t=y; s=*; p=AAAA", &k) && k.testing && !k.strict, "testing only");
    dkim_key_free(&k);
    CHECK((e = key_err("p=AAAA; v=DKIM1", NULL)) && strstr(e, "v="), "v not first");
    CHECK((e = key_err("v=DKIM2; p=AAAA", NULL)) && strstr(e, "v="), "v=DKIM2");
    CHECK((e = key_err("k=dsa; p=AAAA", NULL)) && strstr(e, "key type"), "k dsa");
    CHECK((e = key_err("h=sha1; p=AAAA", NULL)) && strstr(e, "sha256"), "h sha1");
    CHECK((e = key_err("s=web; p=AAAA", NULL)) && strstr(e, "service"), "s web");
    CHECK((e = key_err("v=DKIM1; k=rsa", NULL)) && strstr(e, "p="), "no p");
    CHECK((e = key_err("p=AAAA; p=BBBB", NULL)) && strstr(e, "duplicate"), "dup p");
    CHECK((e = key_err("p=AA!A", NULL)) && strstr(e, "p="), "bad p");
}

static void t_b64(void)
{
    dyn_sb_t s;
    int it;
    dyn_sb_init(&s);
    CHECK(dkim_b64_decode((const uint8_t*)"Zm9v YmE=\r\n", 11, &s) == DKIM_OK && s.n == 5 && !memcmp(s.p, "fooba", 5), "fws decode");
    s.n = 0;
    CHECK(dkim_b64_decode((const uint8_t*)"Zm9=v", 5, &s) == DKIM_EBAD, "data after pad");
    s.n = 0;
    CHECK(dkim_b64_decode((const uint8_t*)"Zm9vY", 5, &s) == DKIM_EBAD, "bad length");
    s.n = 0;
    CHECK(dkim_b64_decode((const uint8_t*)"Zh==", 4, &s) == DKIM_EBAD, "nonzero pad bits");
    s.n = 0;
    CHECK(dkim_b64_decode((const uint8_t*)"Z===", 4, &s) == DKIM_EBAD, "three pads");
    for (it = 0; it < 2000; it++) {
        uint8_t raw[64];
        dyn_sb_t e;
        size_t n = rnd() % sizeof raw, i;
        for (i = 0; i < n; i++)
            raw[i] = (uint8_t)rnd();
        dyn_sb_init(&e);
        dkim_b64_encode(&e, raw, n);
        s.n = 0;
        CHECK(dkim_b64_decode((const uint8_t*)e.p, e.n, &s) == DKIM_OK && s.n == n && (!n || !memcmp(s.p, raw, n)), "roundtrip %zu", n);
        dyn_sb_free(&e);
    }
    dyn_sb_free(&s);
}

static void t_tag_span(void)
{
    static const char v[] = " v=1; bh=QQ==; b = AB\r\n CD ; d=x";
    size_t a = 0, z = 0;
    CHECK(dkim_tag_span((const uint8_t*)v, sizeof v - 1, 'b', &a, &z) == DKIM_OK, "span");
    CHECK(a == 18 && z == 27 && v[z] == ';', "span %zu %zu", a, z);
    CHECK(dkim_tag_span((const uint8_t*)" b=", 3, 'b', &a, &z) == DKIM_OK && a == 3 && z == 3, "empty last");
    CHECK(dkim_tag_span((const uint8_t*)"bh=QQ==", 7, 'b', &a, &z) == DKIM_EBAD, "bh is not b");
}

static size_t find_sig(const dkim_msg_t* m)
{
    size_t j;
    for (j = 0; j < m->nf; j++)
        if (dkim_name_is(m, j, "dkim-signature", 14))
            return j;
    return m->nf;
}

static int sign_check(const char* msg, size_t n, const dkim_sign_opts_t* o, uint8_t hh[32], dkim_sig_t* sig, char** signedp)
{
    dkim_msg_t* m = calloc(1, sizeof *m);
    dkim_msg_t* m2 = calloc(1, sizeof *m2);
    dyn_sb_t f, full;
    uint8_t vh[32], fake[3] = { 1, 2, 3 }, bh[32];
    const char* e;
    size_t j, vo;
    int ok = 0;
    dkim_body_t b;
    dyn_sb_init(&f);
    dkim_sig_init(sig);
    dyn_sb_init(&full);
    if (dkim_msg_parse(m, (const uint8_t*)msg, n) != DKIM_OK)
        goto out;
    if ((e = dkim_sign_field(&f, m, o, hh))) {
        printf("  sign: %s\n", e);
        goto out;
    }
    dkim_fold_b64(&f, fake, 3);
    dyn_sb_put(&f, "\r\n", 2);
    dyn_sb_put(&full, f.p, f.n);
    dyn_sb_put(&full, msg, n);
    if (dkim_msg_parse(m2, (const uint8_t*)full.p, full.n) != DKIM_OK || (j = find_sig(m2)) == m2->nf)
        goto out;
    for (vo = 0; full.p[m2->f[j].off + vo] != ':'; vo++)
        ;
    vo++;
    if ((e = dkim_sig_parse(sig, (const uint8_t*)full.p + m2->f[j].off + vo, m2->f[j].len - vo))) {
        printf("  reparse: %s [%.*s]\n", e, (int)f.n, f.p);
        goto out;
    }
    if (dkim_hdr_hash(m2, sig, j, vh) != DKIM_OK)
        goto out;
    dkim_body_init(&b, sig->brel, sig->l);
    dkim_body_feed(&b, (const uint8_t*)full.p + m2->body, full.n - m2->body);
    dkim_body_final(&b, bh);
    ok = !memcmp(vh, hh, 32) && !memcmp(bh, sig->bh, 32) && sig->b.n == 3;
    if (signedp) {
        *signedp = malloc(full.n + 1);
        memcpy(*signedp, full.p, full.n);
        (*signedp)[full.n] = 0;
    }
out:
    dyn_sb_free(&f);
    dyn_sb_free(&full);
    dkim_msg_free(m);
    free(m);
    dkim_msg_free(m2);
    free(m2);
    return ok;
}

static void t_sign_roundtrip(void)
{
    static const char msg[] = "From: Joe <joe@football.example.com>\r\nTo: s@x\r\nSubject:  hi \r\n there\r\nFrom: second@x\r\n\r\nbody  \r\n\r\n";
    static const char* const heads[] = { "From", "subject", "to", "date" };
    dkim_sign_opts_t o;
    dkim_sig_t sig;
    uint8_t hh[32];
    int hr, br;
    char* full = NULL;
    memset(&o, 0, sizeof o);
    o.d = "football.example.com";
    o.s = "brisbane";
    o.t = 1528637909;
    o.x = -1;
    for (hr = 0; hr < 2; hr++)
        for (br = 0; br < 2; br++) {
            o.hrel = hr;
            o.brel = br;
            o.algo = hr ? DKIM_ED25519 : DKIM_RSA;
            CHECK(sign_check(msg, sizeof msg - 1, &o, hh, &sig, NULL), "roundtrip hr=%d br=%d", hr, br);
            CHECK(sig.hrel == hr && sig.brel == br && sig.t == 1528637909, "tags hr=%d", hr);
            dkim_sig_free(&sig);
        }
    o.heads = heads;
    o.nheads = 4;
    o.oversign = 1;
    o.use_l = 1;
    o.i = "joe@football.example.com";
    o.x = 1528637999;
    CHECK(sign_check(msg, sizeof msg - 1, &o, hh, &sig, &full), "oversign + l");
    CHECK(sig.nh == 3 + 2 + 2 + 1 && !memcmp(sig.h.p, "from\0from\0from\0subject\0subject\0to\0to\0date\0", 42), "h=%zu", sig.nh);
    CHECK(sig.l == 6, "l=%llu", (unsigned long long)sig.l);
    {
        char* p = full;
        size_t col = 0;
        int longline = 0;
        for (; *p && !(p[0] == '\r' && p[1] == '\n' && p[2] != ' '); p++) {
            col = *p == '\n' ? 0 : col + 1;
            longline |= col > 78;
        }
        CHECK(!longline, "folded under 78 cols");
    }
    free(full);
    dkim_sig_free(&sig);
    o.oversign = 0;
    {
        dkim_msg_t* m = calloc(1, sizeof *m);
        dyn_sb_t f;
        static const char* const nofrom[] = { "to" };
        static const char* const badname[] = { "from", "x y" };
        dyn_sb_init(&f);
        dkim_msg_parse(m, (const uint8_t*)"To: x\r\n\r\n", 9);
        CHECK(dkim_sign_field(&f, m, &o, hh) != NULL, "no From in message");
        dkim_msg_parse(m, (const uint8_t*)msg, sizeof msg - 1);
        o.heads = nofrom;
        o.nheads = 1;
        CHECK(dkim_sign_field(&f, m, &o, hh) != NULL, "heads without from");
        o.heads = badname;
        o.nheads = 2;
        CHECK(dkim_sign_field(&f, m, &o, hh) != NULL, "bad head name");
        o.heads = NULL;
        o.d = "a;b";
        CHECK(dkim_sign_field(&f, m, &o, hh) != NULL, "bad d");
        o.d = "ex@mple.com";
        CHECK(dkim_sign_field(&f, m, &o, hh) != NULL, "[bound] d= with @ is refused when signing (domain labels only)");
        o.d = "a..com";
        CHECK(dkim_sign_field(&f, m, &o, hh) != NULL, "[bound] d= with an empty label is refused when signing");
        o.d = "x.com";
        o.s = "";
        CHECK(dkim_sign_field(&f, m, &o, hh) != NULL, "empty s");
        dyn_sb_free(&f);
        dkim_msg_free(m);
        free(m);
    }
}

static void t_bottom_up(void)
{
    static const char a[] = "From: one\r\nFrom: two\r\n\r\n";
    static const char b[] = "From: two\r\nFrom: one\r\n\r\n";
    dkim_msg_t* m = calloc(1, sizeof *m);
    dkim_sig_t s;
    uint8_t h1[32], h2[32], h3[32];
    static const char sig[] = "v=1; a=rsa-sha256; d=x.com; s=s; h=from; " BH "; b=AAAA";
    dkim_sig_init(&s);
    dkim_sig_parse(&s, (const uint8_t*)sig, sizeof sig - 1);
    dkim_msg_parse(m, (const uint8_t*)a, sizeof a - 1);
    dkim_hdr_hash_raw(m, &s, (const uint8_t*)"DKIM-Signature: b=", 18, 14, h1);
    dkim_msg_parse(m, (const uint8_t*)b, sizeof b - 1);
    dkim_hdr_hash_raw(m, &s, (const uint8_t*)"DKIM-Signature: b=", 18, 14, h2);
    dkim_msg_parse(m, (const uint8_t*)"From: two\r\n\r\n", 13);
    dkim_hdr_hash_raw(m, &s, (const uint8_t*)"DKIM-Signature: b=", 18, 14, h3);
    CHECK(memcmp(h1, h2, 32) != 0, "order matters");
    CHECK(!memcmp(h1, h3, 32), "bottom instance 'two' is the signed one");
    dkim_sig_free(&s);
    dkim_msg_free(m);
    free(m);
}

static void t_blanking(void)
{
    static const char m1[] = "DKIM-Signature: v=1; a=rsa-sha256; d=x.com; s=s; h=from; " BH "; b=AAAA\r\nFrom: a\r\n\r\n";
    static const char m2[] = "DKIM-Signature: v=1; a=rsa-sha256; d=x.com; s=s; h=from; " BH "; b=QUJD\r\n RA==\r\nFrom: a\r\n\r\n";
    static const char m3[] = "DKIM-Signature: v=1; a=rsa-sha256; d=y.com; s=s; h=from; " BH "; b=AAAA\r\nFrom: a\r\n\r\n";
    dkim_msg_t* m = calloc(1, sizeof *m);
    dkim_sig_t s;
    uint8_t h1[32], h2[32], h3[32];
    dkim_sig_init(&s);
    dkim_sig_parse(&s, (const uint8_t*)" v=1; a=rsa-sha256; d=x.com; s=s; h=from; " BH "; b=AAAA", strlen(" v=1; a=rsa-sha256; d=x.com; s=s; h=from; " BH "; b=AAAA"));
    s.hrel = 1;
    dkim_msg_parse(m, (const uint8_t*)m1, sizeof m1 - 1);
    CHECK(dkim_hdr_hash(m, &s, 0, h1) == DKIM_OK, "hash1");
    dkim_msg_parse(m, (const uint8_t*)m2, sizeof m2 - 1);
    CHECK(dkim_hdr_hash(m, &s, 0, h2) == DKIM_OK, "hash2");
    dkim_msg_parse(m, (const uint8_t*)m3, sizeof m3 - 1);
    CHECK(dkim_hdr_hash(m, &s, 0, h3) == DKIM_OK, "hash3");
    CHECK(!memcmp(h1, h2, 32), "b= value is blanked");
    CHECK(memcmp(h1, h3, 32) != 0, "other tags are signed");
    dkim_sig_free(&s);
    dkim_msg_free(m);
    free(m);
}

static void t_fuzz(void)
{
    dkim_msg_t* m = calloc(1, sizeof *m);
    static const char al[] = "v=1;abh:d \t\r\n=fromx@.ABZ09+/-_*";
    int it;
    for (it = 0; it < 30000; it++) {
        char buf[512];
        size_t n = rnd() % sizeof buf, i;
        dkim_sig_t s;
        dkim_key_t k;
        dyn_sb_t d;
        int mode = (int)(rnd() % 3);
        for (i = 0; i < n; i++)
            buf[i] = mode == 0 ? (char)rnd() : al[rnd() % (sizeof al - 1)];
        if (mode == 2 && n > 60)
            memcpy(buf, OKSIG, 60 < strlen(OKSIG) ? 60 : strlen(OKSIG));
        dkim_sig_init(&s);
        if (!dkim_sig_parse(&s, (const uint8_t*)buf, n)) {
            CHECK(s.nh >= 1 && s.algo && s.d.n && s.s.n && s.i.n, "accepted sig has required fields");
            if (dkim_msg_parse(m, (const uint8_t*)buf, n) == DKIM_OK && m->nf) {
                uint8_t h[32];
                dkim_hdr_hash(m, &s, 0, h);
            }
        }
        dkim_sig_free(&s);
        dkim_key_init(&k);
        dkim_key_parse(&k, (const uint8_t*)buf, n);
        dkim_key_free(&k);
        dyn_sb_init(&d);
        dkim_b64_decode((const uint8_t*)buf, n, &d);
        dyn_sb_free(&d);
        if (dkim_msg_parse(m, (const uint8_t*)buf, n) == DKIM_OK) {
            size_t j;
            CHECK(m->body <= n, "body in range");
            for (j = 0; j < m->nf; j++)
                CHECK(m->f[j].off + m->f[j].len <= n && m->f[j].nlen <= m->f[j].len, "field in range");
        }
        {
            size_t a, z;
            if (dkim_tag_span((const uint8_t*)buf, n, 'b', &a, &z) == DKIM_OK)
                CHECK(a <= z && z <= n, "span in range");
        }
    }
    for (it = 0; it < 600; it++) {
        char msg[700];
        size_t n = 0, i, nh = 1 + rnd() % 6;
        static const char* const names[] = { "From", "To", "Subject", "Date", "X-A" };
        dkim_sign_opts_t o;
        dkim_sig_t s;
        uint8_t hh[32];
        n += (size_t)snprintf(msg + n, sizeof msg - n, "From: f%d\r\n", it);
        for (i = 0; i < nh; i++) {
            size_t k;
            n += (size_t)snprintf(msg + n, sizeof msg - n, "%s%s:", names[rnd() % 5], rnd() & 1 ? " " : "");
            for (k = rnd() % 30; k > 0; k--)
                msg[n++] = " \tab\r\n"[rnd() % 6] == '\r' ? ' ' : "x \tyz"[rnd() % 5];
            if (rnd() & 1)
                n += (size_t)snprintf(msg + n, sizeof msg - n, "\r\n\tcont  ");
            n += (size_t)snprintf(msg + n, sizeof msg - n, "\r\n");
        }
        n += (size_t)snprintf(msg + n, sizeof msg - n, "\r\n");
        for (i = rnd() % 200; i > 0; i--)
            msg[n++] = "ab \t\r\n"[rnd() % 6];
        memset(&o, 0, sizeof o);
        o.d = "example.com";
        o.s = "s1";
        o.t = -1;
        o.x = -1;
        o.hrel = (int)(rnd() & 1);
        o.brel = (int)(rnd() & 1);
        o.use_l = (int)(rnd() & 1);
        o.oversign = (int)(rnd() & 1);
        CHECK(sign_check(msg, n, &o, hh, &s, NULL), "random roundtrip it=%d", it);
        dkim_sig_free(&s);
    }
    dkim_msg_free(m);
    free(m);
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

static int is_wsp(uint8_t c)
{
    return c == ' ' || c == '\t';
}

static void naive_body(const uint8_t* p, size_t n, int relaxed, dyn_sb_t* o)
{
    dyn_sb_t t, u;
    size_t i = 0, nl = 0, cap = 16, k, last, off;
    size_t* len = malloc(cap * sizeof *len);
    dyn_sb_init(&t);
    dyn_sb_init(&u);
    while (i < n) {
        size_t s = i, before = t.n, j;
        while (i < n && p[i] != '\r' && p[i] != '\n')
            i++;
        if (relaxed) {
            u.n = 0;
            for (j = s; j < i; j++) {
                if (is_wsp(p[j])) {
                    if (!u.n || u.p[u.n - 1] != ' ' || j == s || !is_wsp(p[j - 1]))
                        dyn_sb_putc(&u, ' ');
                } else
                    dyn_sb_putc(&u, (char)p[j]);
            }
            while (u.n && u.p[u.n - 1] == ' ')
                u.n--;
            dyn_sb_put(&t, u.p, u.n);
        } else
            dyn_sb_put(&t, (const char*)p + s, i - s);
        if (nl == cap)
            len = xrealloc(len, (cap *= 2) * sizeof *len);
        len[nl++] = t.n - before;
        if (i < n)
            i += p[i] == '\r' && i + 1 < n && p[i + 1] == '\n' ? 2 : 1;
    }
    last = nl;
    while (last > 0 && len[last - 1] == 0)
        last--;
    for (k = 0, off = 0; k < last; off += len[k], k++) {
        dyn_sb_put(o, t.p + off, len[k]);
        dyn_sb_put(o, "\r\n", 2);
    }
    if (!relaxed && o->n == 0)
        dyn_sb_put(o, "\r\n", 2);
    free(len);
    dyn_sb_free(&t);
    dyn_sb_free(&u);
}

static void naive_hcanon(const uint8_t* f, size_t n, size_t nlen, int relaxed, dyn_sb_t* o)
{
    dyn_sb_t a, b;
    size_t i;
    if (!relaxed) {
        for (i = 0; i < n; i++) {
            if (f[i] == '\r' || f[i] == '\n') {
                dyn_sb_put(o, "\r\n", 2);
                if (f[i] == '\r' && i + 1 < n && f[i + 1] == '\n')
                    i++;
            } else
                dyn_sb_putc(o, (char)f[i]);
        }
        return;
    }
    for (i = 0; i < nlen; i++)
        dyn_sb_putc(o, (char)(f[i] >= 'A' && f[i] <= 'Z' ? f[i] + 32 : f[i]));
    dyn_sb_putc(o, ':');
    i = nlen;
    while (i < n && f[i] != ':')
        i++;
    dyn_sb_init(&a);
    dyn_sb_init(&b);
    for (i++; i < n; i++)
        if (f[i] != '\r' && f[i] != '\n')
            dyn_sb_putc(&a, (char)f[i]);
    for (i = 0; i < a.n; i++) {
        if (is_wsp((uint8_t)a.p[i])) {
            if (!(b.n && b.p[b.n - 1] == ' ' && is_wsp((uint8_t)a.p[i - 1])))
                dyn_sb_putc(&b, ' ');
        } else
            dyn_sb_putc(&b, a.p[i]);
    }
    i = 0;
    while (i < b.n && b.p[i] == ' ')
        i++;
    while (b.n > i && b.p[b.n - 1] == ' ')
        b.n--;
    dyn_sb_put(o, b.p + i, b.n - i);
    dyn_sb_free(&a);
    dyn_sb_free(&b);
}

static void t_diff_body(void)
{
    static const char al[] = "ab \t\r\n.\r\n  x\t";
    dyn_sb_t want;
    uint8_t* buf = malloc(4096);
    long it, N = (long)fk_iters(4000);
    int bad = 0;
    fk_g = fk_stream("diff-body");
    dyn_sb_init(&want);
    for (it = 0; it < N; it++) {
        size_t n = rnd() % 8 == 0 ? rnd() % 4096 : rnd() % 200, i, at, cut;
        int rel = (int)(rnd() & 1);
        uint64_t lim;
        dkim_body_t b;
        uint8_t h1[32], h2[32];
        for (i = 0; i < n; i++)
            buf[i] = rnd() % 16 == 0 ? (uint8_t)rnd() : (uint8_t)al[rnd() % (sizeof al - 1)];
        want.n = 0;
        naive_body(buf, n, rel, &want);
        lim = rnd() % 3 ? DKIM_NO_LIMIT : rnd() % (want.n + 3);
        dyn_sha256(want.n ? (const uint8_t*)want.p : (const uint8_t*)"", lim < want.n ? (size_t)lim : want.n, h1);
        dkim_body_init(&b, rel, lim);
        for (at = 0; at < n; at = cut) {
            cut = at + 1 + (size_t)(rnd() & 1 ? rnd() % 3 : rnd() % 700);
            if (cut > n)
                cut = n;
            dkim_body_feed(&b, buf + at, cut - at);
            if (rnd() % 8 == 0)
                dkim_body_feed(&b, buf, 0);
        }
        dkim_body_final(&b, h2);
        if (memcmp(h1, h2, 32) || b.n != want.n) {
            if (bad++ < 3)
                CHECK(0, "[diff-body] streaming body canon differs from the naive model it=%ld n=%zu rel=%d lim=%llu len %llu vs %zu", it, n, rel,
                    (unsigned long long)lim, (unsigned long long)b.n, want.n);
        }
    }
    CHECK(bad == 0, "[diff-body] %d of %ld random bodies differ from the naive model", bad, N);
    dyn_sb_free(&want);
    free(buf);
}

static const char* const vtok[] = { "a", "B", " ", "\t", "  ", "\r\n ", "\r\n\t", "x:y", ";", "=", "Zz", "\xC3\xA9", "\r\n  \t", "\r", "\n" };

static void gen_value(dyn_sb_t* o, int bare)
{
    int k, nt = (int)(rnd() % 12);
    for (k = 0; k < nt; k++) {
        const char* t = vtok[rnd() % (sizeof vtok / sizeof vtok[0] - (bare ? 0 : 2))];
        dyn_sb_puts(o, t);
    }
}

static void t_diff_hdr(void)
{
    static const char nal[] = "AbcXyz-09_Q";
    dyn_sb_t f, got, want;
    long it, N = (long)fk_iters(6000);
    int bad = 0;
    fk_g = fk_stream("diff-hdr");
    dyn_sb_init(&f);
    dyn_sb_init(&got);
    dyn_sb_init(&want);
    for (it = 0; it < N; it++) {
        size_t nn = 1 + rnd() % 20, i;
        int rel = (int)(rnd() & 1);
        f.n = got.n = want.n = 0;
        for (i = 0; i < nn; i++)
            dyn_sb_putc(&f, rnd() % 40 ? nal[rnd() % (sizeof nal - 1)] : (char)0xC3);
        dyn_sb_puts(&f, rnd() % 3 ? "" : rnd() & 1 ? " " : " \t");
        dyn_sb_putc(&f, ':');
        gen_value(&f, 1);
        dkim_hdr_canon(&got, (const uint8_t*)f.p, f.n, nn, rel);
        naive_hcanon((const uint8_t*)f.p, f.n, nn, rel, &want);
        if (got.n != want.n || (got.n && memcmp(got.p, want.p, got.n))) {
            if (bad++ < 3)
                CHECK(0, "[diff-hdr] header canon differs it=%ld rel=%d in [%.*s] got [%.*s] want [%.*s]", it, rel, (int)f.n, f.p, (int)got.n, got.p,
                    (int)want.n, want.p);
        }
    }
    CHECK(bad == 0, "[diff-hdr] %d of %ld random fields differ from the naive model", bad, N);
    dyn_sb_free(&f);
    dyn_sb_free(&got);
    dyn_sb_free(&want);
}

static const char* const hnames[] = { "From", "To", "Subject", "X-A", "Date", "Reply-To" };

static void rcase(dyn_sb_t* o, const char* s)
{
    for (; *s; s++)
        dyn_sb_putc(o, rnd() & 1 ? (char)(*s >= 'a' && *s <= 'z' ? *s - 32 : *s) : (char)(*s >= 'A' && *s <= 'Z' ? *s + 32 : *s));
}

static const char* fws_pick(void)
{
    static const char* const w[] = { "", "", " ", "\t", "\r\n ", " \r\n\t" };
    return w[rnd() % 6];
}

static int lc_eq(const char* a, size_t an, const char* b, size_t bn)
{
    size_t i;
    if (an != bn)
        return 0;
    for (i = 0; i < an; i++) {
        int x = (uint8_t)a[i], y = (uint8_t)b[i];
        if ((x >= 'A' && x <= 'Z' ? x + 32 : x) != (y >= 'A' && y <= 'Z' ? y + 32 : y))
            return 0;
    }
    return 1;
}

static void t_diff_select(void)
{
    enum { NF = 16 };
    dyn_sb_t raw[NF + 1], sig, blank, msg, hv, model, tmp;
    size_t nlen[NF + 1], nh, k, j;
    char hn[16][16];
    long it, N = (long)fk_iters(1500);
    int bad = 0;
    dkim_msg_t* m = calloc(1, sizeof *m);
    fk_g = fk_stream("diff-select");
    for (k = 0; k <= NF; k++)
        dyn_sb_init(&raw[k]);
    dyn_sb_init(&sig);
    dyn_sb_init(&blank);
    dyn_sb_init(&msg);
    dyn_sb_init(&hv);
    dyn_sb_init(&model);
    dyn_sb_init(&tmp);
    for (it = 0; it < N; it++) {
        size_t nf = 1 + rnd() % (NF - 1), si = rnd() % (nf + 1), tot = nf + 1, order[8], nt = 8;
        int hrel = (int)(rnd() & 1), used[NF + 1];
        unsigned char sigbin[48];
        dkim_sig_t s;
        const char* e;
        uint8_t h1[32], h2[32];
        dyn_sb_t* f;
        for (k = 0, j = 0; k < tot; k++) {
            raw[k].n = 0;
            if (k == si)
                continue;
            rcase(&raw[k], hnames[rnd() % 6]);
            nlen[k] = raw[k].n;
            if (rnd() % 6 == 0)
                dyn_sb_putc(&raw[k], ' ');
            dyn_sb_putc(&raw[k], ':');
            gen_value(&raw[k], 0);
        }
        nh = 1 + rnd() % 10;
        hv.n = 0;
        for (k = 0; k < nh; k++) {
            const char* nm = k == nh / 2 ? "from" : rnd() % 7 == 0 ? "X-None" : hnames[rnd() % 6];
            snprintf(hn[k], sizeof hn[k], "%s", nm);
            if (k)
                dyn_sb_puts(&hv, rnd() & 1 ? ":" : " :\r\n ");
            rcase(&hv, nm);
        }
        for (k = 0; k < nt; k++)
            order[k] = k;
        for (k = nt - 1; k > 0; k--) {
            size_t r = rnd() % (k + 1), x = order[k];
            order[k] = order[r];
            order[r] = x;
        }
        sig.n = blank.n = 0;
        for (k = 0; k < sizeof sigbin; k++)
            sigbin[k] = (unsigned char)rnd();
        for (k = 0; k < nt; k++) {
            const char* sep = k ? (rnd() & 1 ? ";" : "; ") : fws_pick();
            dyn_sb_puts(&sig, sep);
            dyn_sb_puts(&blank, sep);
            switch (order[k]) {
            case 0:
                dyn_sb_puts(&sig, "v=1");
                dyn_sb_puts(&blank, "v=1");
                break;
            case 1:
                dyn_sb_puts(&sig, "a=rsa-sha256");
                dyn_sb_puts(&blank, "a=rsa-sha256");
                break;
            case 2:
                dyn_sb_puts(&sig, hrel ? "c=relaxed/simple" : "c=simple/relaxed");
                dyn_sb_puts(&blank, hrel ? "c=relaxed/simple" : "c=simple/relaxed");
                break;
            case 3:
                dyn_sb_puts(&sig, "d=example.com");
                dyn_sb_puts(&blank, "d=example.com");
                break;
            case 4:
                dyn_sb_puts(&sig, "s=sel");
                dyn_sb_puts(&blank, "s=sel");
                break;
            case 5:
                dyn_sb_puts(&sig, "h=");
                dyn_sb_put(&sig, hv.p, hv.n);
                dyn_sb_puts(&blank, "h=");
                dyn_sb_put(&blank, hv.p, hv.n);
                break;
            case 6:
                dyn_sb_puts(&sig, BH);
                dyn_sb_puts(&blank, BH);
                break;
            default: {
                const char* pre = rnd() & 1 ? " " : "";
                dyn_sb_puts(&sig, "b");
                dyn_sb_puts(&sig, pre);
                dyn_sb_puts(&sig, "=");
                dyn_sb_puts(&blank, "b");
                dyn_sb_puts(&blank, pre);
                dyn_sb_puts(&blank, "=");
                dyn_sb_puts(&sig, fws_pick());
                tmp.n = 0;
                dkim_b64_encode(&tmp, sigbin, 1 + rnd() % sizeof sigbin);
                for (j = 0; j < tmp.n; j++) {
                    if (j && rnd() % 20 == 0)
                        dyn_sb_puts(&sig, "\r\n ");
                    dyn_sb_putc(&sig, tmp.p[j]);
                }
                dyn_sb_puts(&sig, fws_pick());
                break;
            }
            }
        }
        if (rnd() & 1) {
            dyn_sb_putc(&sig, ';');
            dyn_sb_putc(&blank, ';');
        }
        raw[si].n = 0;
        rcase(&raw[si], "DKIM-Signature");
        nlen[si] = raw[si].n;
        dyn_sb_putc(&raw[si], ':');
        dyn_sb_put(&raw[si], sig.p, sig.n);
        msg.n = 0;
        for (k = 0; k < tot; k++) {
            dyn_sb_put(&msg, raw[k].p, raw[k].n);
            dyn_sb_put(&msg, "\r\n", 2);
        }
        dyn_sb_puts(&msg, "\r\nbody\r\n");
        model.n = 0;
        memset(used, 0, sizeof used);
        for (k = 0; k < nh; k++) {
            size_t q = tot;
            while (q-- > 0) {
                if (q == si || used[q] || !lc_eq(raw[q].p, nlen[q], hn[k], strlen(hn[k])))
                    continue;
                used[q] = 1;
                naive_hcanon((const uint8_t*)raw[q].p, raw[q].n, nlen[q], hrel, &model);
                dyn_sb_put(&model, "\r\n", 2);
                break;
            }
        }
        tmp.n = 0;
        dyn_sb_put(&tmp, raw[si].p, nlen[si]);
        dyn_sb_putc(&tmp, ':');
        dyn_sb_put(&tmp, blank.p, blank.n);
        naive_hcanon((const uint8_t*)tmp.p, tmp.n, nlen[si], hrel, &model);
        dyn_sha256((const uint8_t*)model.p, model.n, h1);
        f = &raw[si];
        dkim_sig_init(&s);
        if (dkim_msg_parse(m, (const uint8_t*)msg.p, msg.n) != DKIM_OK || m->nf != tot) {
            if (bad++ < 3)
                CHECK(0, "[diff-select] message did not split into %zu fields it=%ld", tot, it);
        } else if ((e = dkim_sig_parse(&s, (const uint8_t*)f->p + nlen[si] + 1, f->n - nlen[si] - 1))) {
            if (bad++ < 3)
                CHECK(0, "[diff-select] generated signature refused (%s) it=%ld [%.*s]", e, it, (int)sig.n, sig.p);
        } else if (dkim_hdr_hash(m, &s, si, h2) != DKIM_OK || memcmp(h1, h2, 32)) {
            if (bad++ < 3)
                CHECK(0, "[diff-select] header hash differs from the bottom-up model it=%ld nh=%zu nf=%zu", it, nh, tot);
        }
        dkim_sig_free(&s);
    }
    CHECK(bad == 0, "[diff-select] %d of %ld random messages differ from the naive selection model", bad, N);
    for (k = 0; k <= NF; k++)
        dyn_sb_free(&raw[k]);
    dyn_sb_free(&sig);
    dyn_sb_free(&blank);
    dyn_sb_free(&msg);
    dyn_sb_free(&hv);
    dyn_sb_free(&model);
    dyn_sb_free(&tmp);
    dkim_msg_free(m);
    free(m);
}

typedef struct {
    const char* want_err;
    int algo, hrel, brel;
    int64_t t, x;
    uint64_t l;
    size_t nh;
    char d[64], s[32], i[96];
} tag_model_t;

static void put_num_s(dyn_sb_t* o, uint64_t v)
{
    char b[32];
    snprintf(b, sizeof b, "%llu", (unsigned long long)v);
    dyn_sb_puts(o, b);
}

static void t_tag_model(void)
{
    static const char* const algos[] = { "rsa-sha256", "RSA-SHA256", "ed25519-sha256", "Ed25519-Sha256" };
    static const char* const canons[] = { "simple", "relaxed", "simple/relaxed", "relaxed/simple", "relaxed/relaxed", "Simple/Simple" };
    dyn_sb_t tags[16], v;
    long it, N = (long)fk_iters(8000);
    int bad = 0;
    fk_g = fk_stream("tag-model");
    for (it = 0; it < 16; it++)
        dyn_sb_init(&tags[it]);
    dyn_sb_init(&v);
    for (it = 0; it < N; it++) {
        tag_model_t w;
        size_t nt = 0, k, ord[16], drop = 99, dup = 99;
        int em;
        int fault = rnd() % 10 < 6 ? 0 : 1 + (int)(rnd() % 7), ai = (int)(rnd() % 4), ci = (int)(rnd() % 7), labels = 1 + (int)(rnd() % 3);
        dkim_sig_t s;
        const char* e;
        memset(&w, 0, sizeof w);
        w.t = -1;
        w.x = -1;
        w.l = DKIM_NO_LIMIT;
        w.algo = ai < 2 ? DKIM_RSA : DKIM_ED25519;
        for (k = 0; k < 16; k++)
            tags[k].n = 0;
        dyn_sb_puts(&tags[nt++], "v=1");
        dyn_sb_puts(&tags[nt], "a=");
        dyn_sb_puts(&tags[nt++], fault == 4 ? "rsa-sha1" : algos[ai]);
        dyn_sb_puts(&tags[nt], "b=");
        dyn_sb_puts(&tags[nt++], rnd() & 1 ? "AAAA" : "dGVz dA==");
        dyn_sb_puts(&tags[nt++], BH);
        w.d[0] = 0;
        for (k = 0; k < (size_t)labels; k++) {
            char lab[16];
            snprintf(lab, sizeof lab, "%s%c%u", k ? "." : "", "aBcX"[rnd() % 4], (unsigned)(rnd() % 100));
            strcat(w.d, lab);
        }
        strcat(w.d, ".Example");
        dyn_sb_puts(&tags[nt], "d=");
        dyn_sb_puts(&tags[nt++], w.d);
        snprintf(w.s, sizeof w.s, "sel%u", (unsigned)(rnd() % 1000));
        dyn_sb_puts(&tags[nt], "s=");
        dyn_sb_puts(&tags[nt++], w.s);
        w.nh = 1 + rnd() % 6;
        dyn_sb_puts(&tags[nt], "h=");
        for (k = 0; k < w.nh; k++) {
            if (k)
                dyn_sb_puts(&tags[nt], rnd() & 1 ? ":" : " : ");
            dyn_sb_puts(&tags[nt], k == 0 ? (rnd() & 1 ? "From" : "from") : hnames[rnd() % 6]);
        }
        nt++;
        if (ci < 6) {
            dyn_sb_puts(&tags[nt], "c=");
            dyn_sb_puts(&tags[nt++], canons[ci]);
            w.hrel = ci == 1 || ci == 3 || ci == 4;
            w.brel = ci == 2 || ci == 4;
        }
        if (rnd() & 1 || fault == 6) {
            if (fault == 6)
                snprintf(w.i, sizeof w.i, "u@%s", rnd() & 1 ? "other.example" : "xexample");
            else
                snprintf(w.i, sizeof w.i, "%s@%s%s", rnd() & 1 ? "joe" : "", rnd() & 1 ? "sub." : "", w.d);
            dyn_sb_puts(&tags[nt], "i=");
            dyn_sb_puts(&tags[nt++], w.i);
        } else
            snprintf(w.i, sizeof w.i, "@%s", w.d);
        if (rnd() & 1 || fault == 3) {
            dyn_sb_puts(&tags[nt], "l=");
            if (fault == 3) {
                static const char* const badl[] = { "", "12a", "18446744073709551615", "184467440737095516160", "-1", "1 2" };
                dyn_sb_puts(&tags[nt++], badl[rnd() % 6]);
            } else {
                w.l = rnd() & 1 ? rnd() % 100000 : rnd() % DKIM_NO_LIMIT;
                put_num_s(&tags[nt++], w.l);
            }
        }
        if (rnd() & 1 || fault == 5 || fault == 7) {
            w.t = fault == 7 ? INT64_MAX : (int64_t)(rnd() % 4000000000ull);
            dyn_sb_puts(&tags[nt], "t=");
            if (fault == 7)
                dyn_sb_puts(&tags[nt++], "9223372036854775808");
            else
                put_num_s(&tags[nt++], (uint64_t)w.t);
            if ((rnd() & 1 || fault == 5) && fault != 7) {
                w.x = fault == 5 ? w.t - 1 - (int64_t)(rnd() % 10) : w.t + (int64_t)(rnd() % 1000);
                if (w.x < 0)
                    w.x = 0, w.t = 5;
                if (fault == 5) {
                    tags[nt - 1].n = 0;
                    dyn_sb_puts(&tags[nt - 1], "t=");
                    put_num_s(&tags[nt - 1], (uint64_t)w.t);
                }
                dyn_sb_puts(&tags[nt], "x=");
                put_num_s(&tags[nt++], (uint64_t)w.x);
            }
        }
        if (rnd() & 1)
            dyn_sb_puts(&tags[nt++], rnd() & 1 ? "q=dns/txt" : "zz=any thing");
        if (fault == 1)
            drop = rnd() % 7;
        if (fault == 2) {
            dup = rnd() % nt;
            dyn_sb_put(&tags[nt], tags[dup].p, tags[dup].n);
            nt++;
        }
        for (k = 0; k < nt; k++)
            ord[k] = k;
        for (k = nt - 1; k > 0; k--) {
            size_t r = rnd() % (k + 1), x = ord[k];
            ord[k] = ord[r];
            ord[r] = x;
        }
        v.n = 0;
        em = 0;
        dyn_sb_puts(&v, fws_pick());
        for (k = 0; k < nt; k++) {
            const dyn_sb_t* tg = &tags[ord[k]];
            const char* eq = memchr(tg->p, '=', tg->n);
            size_t nl = (size_t)(eq - tg->p);
            if (ord[k] == drop)
                continue;
            if (em++)
                dyn_sb_putc(&v, ';');
            dyn_sb_puts(&v, fws_pick());
            dyn_sb_put(&v, tg->p, nl);
            dyn_sb_puts(&v, rnd() % 4 ? "" : " ");
            dyn_sb_putc(&v, '=');
            dyn_sb_puts(&v, fws_pick());
            dyn_sb_put(&v, eq + 1, tg->n - nl - 1);
            dyn_sb_puts(&v, fws_pick());
        }
        if (rnd() & 1)
            dyn_sb_putc(&v, ';');
        w.want_err = fault == 1 ? "missing required tag" : fault == 2 ? "duplicate tag" : fault == 3 ? "bad l=" : fault == 4 ? "rsa-sha1" : fault == 5 ? "x= is before t=" : fault == 6 ? "within" : fault == 7 ? "bad t=" : NULL;
        dkim_sig_init(&s);
        e = dkim_sig_parse(&s, (const uint8_t*)v.p, v.n);
        if (w.want_err) {
            if (!e || !strstr(e, w.want_err)) {
                if (bad++ < 4)
                    CHECK(0, "[tag-model] fault %d want \"%s\" got \"%s\" it=%ld [%.*s]", fault, w.want_err, e ? e : "(accepted)", it, (int)v.n, v.p);
            }
        } else if (e || s.algo != w.algo || s.hrel != w.hrel || s.brel != w.brel || s.t != w.t || s.x != w.x || s.l != w.l || s.nh != w.nh
            || s.d.n != strlen(w.d) || memcmp(s.d.p, w.d, s.d.n) || s.s.n != strlen(w.s) || memcmp(s.s.p, w.s, s.s.n) || s.i.n != strlen(w.i)
            || memcmp(s.i.p, w.i, s.i.n)) {
            if (bad++ < 4)
                CHECK(0, "[tag-model] clean tag-list misread (%s) it=%ld algo %d/%d c %d%d/%d%d t %lld/%lld x %lld/%lld l %llu/%llu nh %zu/%zu [%.*s]",
                    e ? e : "accepted", it, s.algo, w.algo, s.hrel, s.brel, w.hrel, w.brel, (long long)s.t, (long long)w.t, (long long)s.x,
                    (long long)w.x, (unsigned long long)s.l, (unsigned long long)w.l, s.nh, w.nh, (int)v.n, v.p);
        }
        dkim_sig_free(&s);
    }
    CHECK(bad == 0, "[tag-model] %d of %ld generated tag-lists disagree with the model", bad, N);
    for (it = 0; it < 16; it++)
        dyn_sb_free(&tags[it]);
    dyn_sb_free(&v);
}

static void t_bound(void)
{
    dkim_msg_t* m = xcalloc(1, sizeof *m);
    dyn_sb_t b;
    dkim_sig_t s;
    const char* e;
    int k;
    char h1[64], h2[64];
    dyn_sb_init(&b);
    for (k = 0; k < 1000; k++)
        dyn_sb_puts(&b, "X:a\r\n");
    dyn_sb_puts(&b, "\r\nbody");
    CHECK(dkim_msg_parse(m, (const uint8_t*)b.p, b.n) == DKIM_OK && m->nf == 1000, "[bound] exactly 1000 header fields parse");
    b.n = 0;
    for (k = 0; k < 1001; k++)
        dyn_sb_puts(&b, "X:a\r\n");
    CHECK(dkim_msg_parse(m, (const uint8_t*)b.p, b.n) == DKIM_ELONG, "[bound] 1001 header fields are ELONG");
    b.n = 0;
    dyn_sb_puts(&b, "v=1; a=rsa-sha256; d=example.com; s=sel; " BH "; b=AAAA; h=from");
    for (k = 1; k < 128; k++)
        dyn_sb_puts(&b, ":to");
    dkim_sig_init(&s);
    e = dkim_sig_parse(&s, (const uint8_t*)b.p, b.n);
    CHECK(!e && s.nh == 128, "[bound] h= with exactly 128 entries: %s", e ? e : "ok");
    dkim_sig_free(&s);
    dyn_sb_puts(&b, ":to");
    dyn_sb_putc(&b, 0);
    CHECK((e = sig_err(b.p)) && strstr(e, "too many h="), "[bound] 129 h= entries refused: %s", e ? e : "accepted");
    b.n = 0;
    dyn_sb_puts(&b, OKSIG);
    for (k = 7; k < 64; k++) {
        char t[24];
        snprintf(t, sizeof t, "; z%d=1", k);
        dyn_sb_puts(&b, t);
    }
    dyn_sb_putc(&b, 0);
    CHECK(!(e = sig_err(b.p)), "[bound] exactly 64 tags accepted: %s", e ? e : "ok");
    b.n--;
    dyn_sb_puts(&b, "; z64=1");
    dyn_sb_putc(&b, 0);
    CHECK((e = sig_err(b.p)) && strstr(e, "too many tags"), "[bound] 65 tags refused: %s", e ? e : "accepted");
    CHECK(!sig_err(OKSIG "; l=18446744073709551614"), "[bound] l= UINT64_MAX-1 accepted");
    CHECK((e = sig_err(OKSIG "; l=18446744073709551615")) && strstr(e, "bad l="), "[bound] l= UINT64_MAX refused (it means no limit)");
    CHECK((e = sig_err(OKSIG "; l=018446744073709551614")) && strstr(e, "bad l="), "[bound] l= of 21 digits refused");
    CHECK(!sig_err(OKSIG "; l=0"), "[bound] l=0 accepted");
    CHECK(!sig_err(OKSIG "; t=9223372036854775807; x=9223372036854775807"), "[bound] t=x=INT64_MAX accepted");
    CHECK((e = sig_err(OKSIG "; t=9223372036854775808")) && strstr(e, "bad t="), "[bound] t=INT64_MAX+1 refused");
    CHECK((e = sig_err(OKSIG "; x=9223372036854775808")) && strstr(e, "bad x="), "[bound] x=INT64_MAX+1 refused");
    CHECK((e = sig_err(OKSIG "; t=100; x=99")) && strstr(e, "before"), "[bound] x=t-1 refused");
    CHECK(!sig_err(OKSIG "; t=100; x=100"), "[bound] x=t accepted");
    body_b64("ab\r\n", 4, 0, 0, h1);
    body_b64("zz  \r\n\r\n", 8, 1, 0, h2);
    CHECK(!strcmp(h1, "47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFU=") && !strcmp(h1, h2), "[bound] l=0 hashes the empty string under both canonicalizations");
    body_b64("ab\r\n\r\n", 6, 0, 4, h1);
    body_b64("ab\r\n", 4, 0, DKIM_NO_LIMIT, h2);
    CHECK(!strcmp(h1, h2), "[bound] l= equal to the canonical length is the whole body");
    body_b64("ab\r\n", 4, 0, 3, h1);
    CHECK(strcmp(h1, h2) != 0, "[bound] l= one short of the canonical length differs");
    dyn_sb_free(&b);
    dkim_msg_free(m);
    free(m);
}

static void fz_mutate(uint8_t* b, size_t* n, size_t cap)
{
    int k, m = 1 + (int)(rnd() % 6);
    for (k = 0; k < m; k++) {
        size_t at = *n ? rnd() % *n : 0;
        switch (rnd() % 5) {
        case 0:
            if (*n)
                b[at] = (uint8_t)rnd();
            break;
        case 1:
            if (*n) {
                memmove(b + at, b + at + 1, *n - at - 1);
                --*n;
            }
            break;
        case 2:
            if (*n < cap) {
                memmove(b + at + 1, b + at, *n - at);
                b[at] = (uint8_t)";=:\r\n \t@.aZ09+/"[rnd() % 16];
                ++*n;
            }
            break;
        case 3:
            if (*n && *n * 2 < cap) {
                size_t l = 1 + rnd() % (*n - at);
                memmove(b + at + l, b + at, *n - at);
                *n += l;
            }
            break;
        default:
            if (*n)
                *n = rnd() % *n;
            break;
        }
    }
}

static void t_fuzz_entry(void)
{
    static const char* const seeds[] = {
        "\x01" OKSIG,
        "\x47" "From: a@b\r\nTo: c\r\nSubject: s\r\n\r\nbody  \r\n\r\n",
        "\x00" "From: a\r\nDKIM-Signature: v=1; a=ed25519-sha256; c=relaxed/relaxed; d=example.com; s=s; t=10; x=20; h=from:from; " BH "; b=AAAA\r\n\r\nhi\r\n",
        "\x02" "v=DKIM1; k=ed25519; h=sha256; s=email; t=y:s; p=11qYAYKxCrfVS/7TyWQHOg7hcvPapiMlrwIaaPcHURo=",
        "\x3f" "From:x\r\n\tcont\r\nX: \r\n\r\n\r\n.\r\n",
    };
    uint8_t* buf = malloc(8192);
    long it, N = (long)fk_iters(30000);
    fk_g = fk_stream("fuzz-entry");
    printf("[fuzz-entry] start\n");
    fflush(stdout);
    for (it = 0; it < (long)(sizeof seeds / sizeof seeds[0]); it++)
        LLVMFuzzerTestOneInput((const uint8_t*)seeds[it], strlen(seeds[it]) + (seeds[it][0] == 0));
    for (it = 0; it < N; it++) {
        size_t n, i;
        if (rnd() % 5 == 0) {
            n = rnd() % 600;
            for (i = 0; i < n; i++)
                buf[i] = (uint8_t)rnd();
        } else {
            const char* sd = seeds[rnd() % (sizeof seeds / sizeof seeds[0])];
            n = (sd[0] ? strlen(sd) : 1 + strlen(sd + 1));
            memcpy(buf, sd, n);
            buf[0] = (uint8_t)rnd();
            fz_mutate(buf, &n, 8192);
        }
        LLVMFuzzerTestOneInput(buf, n);
    }
    printf("[fuzz-entry] done\n");
    CHECK(1, "[fuzz-entry] %ld seeded and mutated inputs through src/fuzz/fuzz_dkim.c without a trap", N);
    free(buf);
}

int main(void)
{
    fk_init("test_dkim_core");
    fflush(stdout);
    fk_g = fk_stream("fixed");
    t_empty_body();
    t_rfc6376_345();
    t_rfc8463_bh();
    t_body_misc();
    fk_g = fk_stream("stream");
    t_stream();
    t_msg_parse();
    t_sig_parse();
    t_key_parse();
    t_b64();
    t_tag_span();
    fk_g = fk_stream("sign");
    t_sign_roundtrip();
    t_bottom_up();
    t_blanking();
    fk_g = fk_stream("fuzz");
    t_fuzz();
    t_diff_body();
    t_diff_hdr();
    t_diff_select();
    t_tag_model();
    t_bound();
    t_fuzz_entry();
    printf("%d checks\n", checks);
    fk_fails += fails;
    return fk_done(__FILE__);
}
