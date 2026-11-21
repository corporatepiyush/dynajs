/*
 * test_mailk_sasl.c -- src/core/mailk SASL kit (mailk-sasl.c, mailk-scram.c) shared by dyna:smtp,
 * dyna:imap and dyna:pop3 AUTH. S9 M2, 2026-11-26. Pure C, no JS values, so every call runs on a
 * pool thread (owner rule: mail parse/encode/crypto always off the main JS thread).
 *
 * External oracles:
 *  - RFC 4648 4 base64 vectors; strict decode (RFC 4954 4 / RFC 9051 6.2.2): length a multiple of
 *    4, padding only at the end, zero pad bits, no whitespace. "=" is the empty response, "*"
 *    cancels (RFC 4954 4).
 *  - RFC 4013 3 SASLprep examples (soft hyphen, ordinal, Roman numeral, BEL, bidi) with the RFC
 *    3454 C.1.2 / B.1 maps, NFKC, the C.2-C.9 prohibitions and the 6 bidi rule; query profile
 *    (unassigned code points allowed, RFC 5802 2.2). Bidi classes and NFKC are current Unicode,
 *    not 3.2.
 *  - RFC 4616 4 PLAIN examples; authcid and passwd 1..255 octets, authzid 0..255, exactly two NULs.
 *  - Google XOAUTH2 protocol page example; RFC 7628 4.1 OAUTHBEARER example (gs2 header,
 *    kvsep 0x01, host/port/auth keys, Bearer token68).
 *  - RFC 5802 5 SCRAM-SHA-1 and RFC 7677 3 SCRAM-SHA-256 exchanges, byte-exact client and server
 *    sides; RFC 5802 5.1 saslname "=2C"/"=3D", gs2 "n"/"y"/"p=" channel binding (RFC 5056,
 *    RFC 9266 tls-exporter), "y" against a PLUS-capable server is a downgrade and fails, "m=" is
 *    refused; RFC 7677 4 iteration floor 4096 (client refuses fewer).
 * Fuzz: 100000 random and mutated inputs through every parser and both SCRAM sides under
 * ASan/UBSan.
 * Also: RFC 2195 CRAM-MD5 example (kept for legacy peers; modules leave it off by default).
 * Mutants (each red here): base64 pad-bit check dropped, SASLprep bidi rule dropped, server proof
 * comparison dropped, client nonce-prefix check dropped, server "y" downgrade check dropped,
 * PLAIN empty-authcid floor dropped, client max_iters cap dropped, server cb-data compare dropped.
 * The PLAIN third-NUL check is an equivalent mutant: the passwd NUL scan refuses the same input.
 * Seeded property tests (tests/lib/dyn-fuzzkit.h, 2026-11-27): DYN_FUZZ_SEED replays a run (the seed is
 * printed first), DYN_FUZZ_ITERS (integer, 50 = deep run) multiplies every loop. Each tagged check is proven by its own mutant:
 *  [diff-b64]     mailk base64 encode/decode/validate vs a naive RFC 4648 model (padding, junk, length
 *                 classes); mutant: non-zero bits before "==" accepted.
 *  [rt-saslname]  saslname encode/decode identity incl. ',' and '='; mutant: "=" not encoded as =3D.
 *  [rt-plain]     random authzid/authcid/passwd PLAIN build -> parse; [diff-prep] SASLprep of random
 *                 ASCII with RFC 4013 mapped code points (B.1 nothing, C.1.2 space, NFKC); mutant:
 *                 U+1680 dropped from the non-ASCII space table.
 *  [rt-scram]     30 random-credential client/server exchanges per SHA-1/256/512, 1 in 4 wrong
 *                 password -> EAUTH; mutant: username not compared.
 *  [bound]        PLAIN passwd MAILK_SASL_MAX ok / +1 EBAD, saslname 255/256, SASLprep 1024 / 1025
 *                 ELONG, b64 max / +1 quantum ELONG; mutant: PLAIN cap off by one.
 *  [fuzz-sasl]    src/fuzz/fuzz_mailk_sasl.c (libFuzzer target; b64, saslname, SASLprep, PLAIN, OAuth and
 *                 SCRAM round trips) over mutated seeds; mutant: PLAIN parse NUL scan overrun.
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

static int sbeq(const dyn_sb_t* b, const char* s, size_t n)
{
    return b->n == n && (!n || !memcmp(b->p, s, n));
}
#define SBEQ(b, lit) sbeq(b, lit, sizeof(lit) - 1)
#define U(s) (const uint8_t*)(s), strlen(s)
#define UL(s) (const uint8_t*)(s), sizeof(s) - 1

static int dec(const char* s, dyn_sb_t* o)
{
    o->n = 0;
    return mailk_b64_decode(s, strlen(s), o);
}

static void test_b64(void)
{
    static const char* raw[] = { "", "f", "fo", "foo", "foob", "fooba", "foobar" };
    static const char* enc[] = { "", "Zg==", "Zm8=", "Zm9v", "Zm9vYg==", "Zm9vYmE=", "Zm9vYmFy" };
    static const char* bad[] = { "Zg=", "Zh==", "Zm9=", "Z===", "Zm 9v", "=Zm9", "Zm9v\r\n", "Zg==Zg==", "Zm9v====", "Z@==", "Zg=a" };
    dyn_sb_t o;
    size_t i;
    dyn_sb_init(&o);
    for (i = 0; i < 7; i++) {
        CHECK(dec(enc[i], &o) == MAILK_OK && sbeq(&o, raw[i], strlen(raw[i])), "b64 dec %s", enc[i]);
        o.n = 0;
        CHECK(mailk_b64_encode(&o, (const uint8_t*)raw[i], strlen(raw[i])) == MAILK_OK && sbeq(&o, enc[i], strlen(enc[i])), "b64 enc %s", raw[i]);
    }
    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        CHECK(dec(bad[i], &o) == MAILK_EBAD, "b64 strict refuses %s", bad[i]);
    o.n = 0;
    CHECK(mailk_sasl_resp_decode("*", 1, &o) == MAILK_CANCEL, "* cancels");
    CHECK(mailk_sasl_resp_decode("=", 1, &o) == MAILK_OK && o.n == 0, "= is empty");
    CHECK(mailk_sasl_resp_decode("", 0, &o) == MAILK_EBAD, "empty line refused");
    CHECK(mailk_sasl_resp_decode("dGVzdA==", 8, &o) == MAILK_OK && SBEQ(&o, "test"), "resp decode");
    o.n = 0;
    CHECK(mailk_sasl_resp_encode(&o, NULL, 0) == MAILK_OK && SBEQ(&o, "="), "empty resp encodes =");
    {
        size_t big = 4 * ((MAILK_SASL_MAX + 2) / 3) + 4;
        char* s = malloc(big);
        memset(s, 'A', big);
        o.n = 0;
        CHECK(mailk_b64_decode(s, big, &o) == MAILK_ELONG, "b64 over MAILK_SASL_MAX refused");
        free(s);
    }
    dyn_sb_free(&o);
}

static int prep(const char* in, const char* want)
{
    dyn_sb_t o;
    int rc;
    dyn_sb_init(&o);
    rc = mailk_saslprep(in, strlen(in), &o);
    if (want)
        rc = rc == MAILK_OK && sbeq(&o, want, strlen(want));
    else
        rc = rc == MAILK_EBAD;
    dyn_sb_free(&o);
    return rc;
}

static void test_saslprep(void)
{
    CHECK(prep("I\xC2\xADX", "IX"), "4013 soft hyphen");
    CHECK(prep("user", "user"), "4013 user");
    CHECK(prep("USER", "USER"), "4013 case preserved");
    CHECK(prep("\xC2\xAA", "a"), "4013 ordinal");
    CHECK(prep("\xE2\x85\xA8", "IX"), "4013 roman nine");
    CHECK(prep("\x07", NULL), "4013 BEL prohibited");
    CHECK(prep("\xD8\xA7" "1", NULL), "4013 bidi RandAL then EN");
    CHECK(prep("\xD8\xA7" "1" "\xD8\xA8", "\xD8\xA7" "1" "\xD8\xA8"), "bidi RandAL both ends ok");
    CHECK(prep("\xD8\xA7" "a" "\xD8\xA8", NULL), "bidi RandAL with LCat");
    CHECK(prep("a\xC2\xA0" "b", "a b"), "C.1.2 nbsp to space");
    CHECK(prep("a\xE3\x80\x80" "b", "a b"), "C.1.2 ideographic space");
    CHECK(prep("a\xE2\x80\x8D" "b", "ab"), "B.1 ZWJ to nothing");
    CHECK(prep("a\xEF\xBB\xBF" "b", "ab"), "B.1 BOM to nothing");
    CHECK(prep("\xEF\xAC\x81", "fi"), "NFKC ligature");
    CHECK(prep("\xEE\x80\x80", NULL), "C.3 private use");
    CHECK(prep("\xEF\xBF\xBE", NULL), "C.4 non-character");
    CHECK(prep("\xF3\xA0\x80\x81", NULL), "C.9 tag");
    CHECK(prep("\xE2\x80\x8E", NULL), "C.8 LRM");
    CHECK(prep("a\x7F", NULL), "C.2.1 DEL");
    CHECK(prep("\xC2\x85", NULL), "C.2.2 NEL");
    CHECK(prep("\xC0\xAF", NULL), "overlong refused");
    CHECK(prep("\xED\xA0\x80", NULL), "surrogate refused");
    CHECK(prep("\xF0\x9F\x98\x80", "\xF0\x9F\x98\x80"), "emoji passes (query profile)");
    CHECK(prep("", ""), "empty passes");
    {
        dyn_sb_t o;
        char* s = malloc(MAILK_PREP_MAX + 1);
        memset(s, 'a', MAILK_PREP_MAX + 1);
        dyn_sb_init(&o);
        CHECK(mailk_saslprep(s, MAILK_PREP_MAX + 1, &o) == MAILK_ELONG, "prep over max");
        CHECK(mailk_saslprep(s, MAILK_PREP_MAX, &o) == MAILK_OK && o.n == MAILK_PREP_MAX, "prep at max");
        dyn_sb_free(&o);
        free(s);
    }
}

static void test_plain_login(void)
{
    dyn_sb_t o;
    mailk_plain_t p;
    char big[300];
    dyn_sb_init(&o);
    CHECK(mailk_plain_build(&o, NULL, "tim", "tanstaaftanstaaf") == MAILK_OK && SBEQ(&o, "\0tim\0tanstaaftanstaaf"), "4616 ex1");
    o.n = 0;
    CHECK(mailk_plain_build(&o, "Ursel", "Kurt", "xipj3plmq") == MAILK_OK && SBEQ(&o, "Ursel\0Kurt\0xipj3plmq"), "4616 ex2");
    CHECK(mailk_plain_parse((const uint8_t*)o.p, o.n, &p) == MAILK_OK && p.azn == 5 && p.acn == 4 && !memcmp(o.p + p.ac, "Kurt", 4)
            && p.pwn == 9 && !memcmp(o.p + p.pw, "xipj3plmq", 9), "4616 parse");
    CHECK(mailk_plain_parse((const uint8_t*)"\0tim\0pw\0x", 9, &p) == MAILK_EBAD, "third NUL refused");
    CHECK(mailk_plain_parse((const uint8_t*)"\0tim", 4, &p) == MAILK_EBAD, "one NUL refused");
    CHECK(mailk_plain_parse((const uint8_t*)"\0\0pw", 4, &p) == MAILK_EBAD, "empty authcid refused");
    CHECK(mailk_plain_parse((const uint8_t*)"\0u\0", 3, &p) == MAILK_EBAD, "empty passwd refused");
    CHECK(mailk_plain_parse((const uint8_t*)"\0u\0\xFF", 4, &p) == MAILK_EBAD, "bad utf8 refused");
    memset(big, 'a', sizeof(big));
    big[0] = 0;
    big[2] = 0;
    CHECK(mailk_plain_parse((const uint8_t*)big, 3 + 255, &p) == MAILK_OK, "255-octet passwd ok");
    CHECK(mailk_plain_parse((const uint8_t*)big, 3 + 256, &p) == MAILK_OK, "256-octet passwd accepted (255 is a floor, RFC 4616 2)");
    o.n = 0;
    CHECK(mailk_plain_build(&o, NULL, "", "x") == MAILK_EBAD, "build empty authcid refused");
    o.n = 0;
    CHECK(mailk_cram_md5(&o, "tim", UL("tanstaaftanstaaf"), UL("<1896.697170952@postoffice.reston.mci.net>")) == MAILK_OK
            && SBEQ(&o, "tim b913a602c7eda7a495b4e6e7334d3890"), "RFC 2195 CRAM-MD5 example");
    CHECK(mailk_cram_md5(&o, "a b", UL("x"), UL("<c>")) == MAILK_EBAD, "cram user with space");
    CHECK(mailk_login_prompt((const uint8_t*)"Username:", 9) == 1, "login user prompt");
    CHECK(mailk_login_prompt((const uint8_t*)"Password:", 9) == 2, "login pass prompt");
    CHECK(mailk_login_prompt((const uint8_t*)"User Name", 9) == 1, "login User Name");
    CHECK(mailk_login_prompt((const uint8_t*)"Token:", 6) == 0, "login unknown prompt");
    o.n = 0;
    CHECK(dec("VXNlcm5hbWU6", &o) == MAILK_OK && mailk_login_prompt((const uint8_t*)o.p, o.n) == 1, "login b64 prompt");
    dyn_sb_free(&o);
}

static void test_oauth(void)
{
    static const char ob[] = "n,a=user@example.com,\001host=server.example.com\001port=143\001auth=Bearer vF9dft4qmTc2Nvb3RlckBhbHRhdmlzdGEuY29tCg==\001\001";
    dyn_sb_t o, e;
    mailk_oauth_t t;
    dyn_sb_init(&o);
    dyn_sb_init(&e);
    CHECK(mailk_xoauth2_build(&o, "someuser@example.com", "ya29.vF9dft4qmTc2Nvb3RlckBhdHRhdmlzdGEuY29tCg") == MAILK_OK, "xoauth2 build");
    mailk_b64_encode(&e, (const uint8_t*)o.p, o.n);
    CHECK(SBEQ(&e, "dXNlcj1zb21ldXNlckBleGFtcGxlLmNvbQFhdXRoPUJlYXJlciB5YTI5LnZGOWRmdDRxbVRjMk52YjNSbGNrQmhkSFJoZG1semRHRXVZMjl0Q2cBAQ=="), "xoauth2 google example");
    CHECK(mailk_xoauth2_parse((const uint8_t*)o.p, o.n, &t) == MAILK_OK && t.azn == 20 && t.tokn == 45, "xoauth2 parse");
    CHECK(mailk_xoauth2_parse((const uint8_t*)o.p, o.n - 1, &t) == MAILK_EBAD, "xoauth2 missing final kvsep");
    CHECK(mailk_xoauth2_parse(UL("auth=Bearer x\001\001"), &t) == MAILK_EBAD, "xoauth2 no user");
    o.n = 0;
    CHECK(mailk_xoauth2_build(&o, "u", "bad token") == MAILK_EBAD, "xoauth2 token68 enforced");
    o.n = 0;
    CHECK(mailk_oauthbearer_build(&o, "user@example.com", "server.example.com", 143, "vF9dft4qmTc2Nvb3RlckBhbHRhdmlzdGEuY29tCg==") == MAILK_OK
            && sbeq(&o, ob, sizeof(ob) - 1), "7628 4.1 example");
    CHECK(mailk_oauthbearer_parse((const uint8_t*)ob, sizeof(ob) - 1, &t) == MAILK_OK && t.azn == 16 && t.hostn == 18
            && t.portn == 3 && !memcmp(ob + t.port, "143", 3) && t.tokn == 42, "7628 parse");
    CHECK(mailk_oauthbearer_parse(UL("n,,\001auth=Bearer abc\001\001"), &t) == MAILK_OK && t.azn == 0, "no authzid");
    CHECK(mailk_oauthbearer_parse(UL("y,,\001auth=Bearer abc\001\001"), &t) == MAILK_EBAD, "cb flag y refused");
    CHECK(mailk_oauthbearer_parse(UL("n,a=x=2Bc,\001auth=Bearer abc\001\001"), &t) == MAILK_EBAD, "bad saslname escape");
    CHECK(mailk_oauthbearer_parse(UL("n,,\001auth=Bearer abc\001auth=Bearer d\001\001"), &t) == MAILK_EBAD, "two auth keys");
    CHECK(mailk_oauthbearer_parse(UL("n,,\001port=99999x\001auth=Bearer a\001\001"), &t) == MAILK_EBAD, "bad port");
    CHECK(mailk_oauthbearer_parse(UL("n,,\001\001"), &t) == MAILK_EBAD, "no auth key");
    o.n = 0;
    CHECK(mailk_oauthbearer_build(&o, "a,b=c", NULL, 0, "t") == MAILK_OK && SBEQ(&o, "n,a=a=2Cb=3Dc,\001auth=Bearer t\001\001"), "authzid escaped");
    o.n = 0;
    CHECK(mailk_saslname_decode(&o, "a=2Cb=3Dc", 9) == MAILK_OK && SBEQ(&o, "a,b=c"), "saslname decode");
    CHECK(mailk_saslname_decode(&o, "a=2c", 4) == MAILK_EBAD, "lowercase escape refused");
    CHECK(mailk_saslname_decode(&o, "a,b", 3) == MAILK_EBAD, "raw comma refused");
    dyn_sb_free(&o);
    dyn_sb_free(&e);
}

typedef struct {
    int algo;
    const char *cnonce, *cfirst, *sfirst, *snonce, *cfinal, *sfinal, *salt;
} vec_t;

static const vec_t vecs[] = {
    { MAILK_SCRAM_SHA1, "fyko+d2lbbFgONRv9qkxdawL", "n,,n=user,r=fyko+d2lbbFgONRv9qkxdawL",
        "r=fyko+d2lbbFgONRv9qkxdawL3rfcNHYJY1ZVvWVs7j,s=QSXCR+Q6sek8bf92,i=4096", "3rfcNHYJY1ZVvWVs7j",
        "c=biws,r=fyko+d2lbbFgONRv9qkxdawL3rfcNHYJY1ZVvWVs7j,p=v0X8v3Bz2T0CJGbJQyF0X+HI4Ts=",
        "v=rmF9pqV8S7suAoZWja4dJRkFsKQ=", "QSXCR+Q6sek8bf92" },
    { MAILK_SCRAM_SHA256, "rOprNGfwEbeRWgbNEkqO", "n,,n=user,r=rOprNGfwEbeRWgbNEkqO",
        "r=rOprNGfwEbeRWgbNEkqO%hvYDpWUa2RaTCAfuxFIlj)hNlF$k0,s=W22ZaJ0SNY7soEsUEjb6gQ==,i=4096",
        "%hvYDpWUa2RaTCAfuxFIlj)hNlF$k0",
        "c=biws,r=rOprNGfwEbeRWgbNEkqO%hvYDpWUa2RaTCAfuxFIlj)hNlF$k0,p=dHzbZapWIk4jUhN+Ute9ytag9zjfMHgsqmmiz7AndVQ=",
        "v=6rriTRBi23WpRR/wtup+mMhUZUn/dB5nLTJRsjl95G4=", "W22ZaJ0SNY7soEsUEjb6gQ==" },
};

static int L(const char* s) { return (int)strlen(s); }

static void test_scram_vectors(void)
{
    size_t i;
    for (i = 0; i < 2; i++) {
        const vec_t* v = &vecs[i];
        mailk_scram_t c, s;
        dyn_sb_t o, salt;
        uint8_t st[64], sk[64];
        dyn_sb_init(&o);
        dyn_sb_init(&salt);
        mailk_scram_init(&c, v->algo);
        CHECK(mailk_scram_client_first(&c, "user", NULL, "pencil", v->cnonce, &o) == MAILK_OK && sbeq(&o, v->cfirst, (size_t)L(v->cfirst)), "client-first %zu", i);
        o.n = 0;
        CHECK(mailk_scram_client_final(&c, U(v->sfirst), &o) == MAILK_OK && sbeq(&o, v->cfinal, (size_t)L(v->cfinal)), "client-final %zu: %.*s", i, (int)o.n, o.p);
        CHECK(mailk_scram_client_verify(&c, U(v->sfinal)) == MAILK_OK, "client verify %zu", i);
        CHECK(mailk_scram_client_verify(&c, U(v->sfinal)) == MAILK_EBAD, "verify twice is a state error %zu", i);
        mailk_scram_free(&c);
        dec(v->salt, &salt);
        CHECK(mailk_scram_derive(v->algo, "pencil", (const uint8_t*)salt.p, salt.n, 4096, st, sk) == MAILK_OK, "derive %zu", i);
        mailk_scram_init(&s, v->algo);
        CHECK(mailk_scram_server_first(&s, U(v->cfirst)) == MAILK_OK && SBEQ(&s.user, "user") && s.authzid.n == 0, "server-first parse %zu", i);
        o.n = 0;
        CHECK(mailk_scram_server_challenge(&s, (const uint8_t*)salt.p, salt.n, 4096, st, sk, v->snonce, &o) == MAILK_OK
                && sbeq(&o, v->sfirst, (size_t)L(v->sfirst)), "server challenge %zu: %.*s", i, (int)o.n, o.p);
        o.n = 0;
        CHECK(mailk_scram_server_final(&s, U(v->cfinal), &o) == MAILK_OK && sbeq(&o, v->sfinal, (size_t)L(v->sfinal)), "server final %zu: %.*s", i, (int)o.n, o.p);
        mailk_scram_free(&s);
        dyn_sb_free(&o);
        dyn_sb_free(&salt);
    }
}

typedef struct {
    int c_flag, s_plus;
    const char *c_cbname, *s_cbname, *c_data, *s_data;
    const char *user, *authz, *pw, *spw;
    uint32_t iters, min_iters;
    int want_c2, want_s3, want_v;
    int algo;
} rt_t;

static void roundtrip(const rt_t* t, const char* tag)
{
    mailk_scram_t c, s;
    dyn_sb_t o, prep;
    uint8_t st[64], sk[64], salt[16] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
    int rc;
    dyn_sb_init(&o);
    dyn_sb_init(&prep);
    mailk_scram_init(&c, t->algo);
    mailk_scram_init(&s, t->algo);
    c.min_iters = t->min_iters;
    if (t->c_flag)
        mailk_scram_cb(&c, t->c_flag, t->c_cbname, (const uint8_t*)t->c_data, t->c_data ? strlen(t->c_data) : 0);
    if (t->s_plus)
        mailk_scram_cb(&s, 'p', t->s_cbname, (const uint8_t*)t->s_data, strlen(t->s_data));
    mailk_scram_derive(t->algo, t->spw, salt, sizeof(salt), t->iters, st, sk);
    rc = mailk_scram_client_first(&c, t->user, t->authz, t->pw, NULL, &o);
    CHECK(rc == MAILK_OK, "%s client-first %d", tag, rc);
    rc = mailk_scram_server_first(&s, (const uint8_t*)o.p, o.n);
    if (rc != MAILK_OK) {
        CHECK(t->want_s3 == rc, "%s server-first rc %d", tag, rc);
        goto out;
    }
    mailk_saslprep(t->user, strlen(t->user), &prep);
    CHECK(sbeq(&s.user, prep.p, prep.n), "%s user round-trips", tag);
    o.n = 0;
    mailk_scram_server_challenge(&s, salt, sizeof(salt), t->iters, st, sk, NULL, &o);
    {
        dyn_sb_t cf;
        dyn_sb_init(&cf);
        rc = mailk_scram_client_final(&c, (const uint8_t*)o.p, o.n, &cf);
        CHECK(rc == t->want_c2, "%s client-final rc %d", tag, rc);
        if (rc == MAILK_OK) {
            o.n = 0;
            rc = mailk_scram_server_final(&s, (const uint8_t*)cf.p, cf.n, &o);
            CHECK(rc == t->want_s3, "%s server-final rc %d %.*s", tag, rc, (int)o.n, o.p);
            CHECK(mailk_scram_client_verify(&c, (const uint8_t*)o.p, o.n) == t->want_v, "%s client verify", tag);
        }
        dyn_sb_free(&cf);
    }
out:
    mailk_scram_free(&c);
    mailk_scram_free(&s);
    dyn_sb_free(&o);
    dyn_sb_free(&prep);
}

static void test_scram_roundtrip(void)
{
    static const rt_t rts[] = {
        { 0, 0, NULL, NULL, NULL, NULL, "user", NULL, "pencil", "pencil", 4096, 4096, MAILK_OK, MAILK_OK, MAILK_OK, MAILK_SCRAM_SHA256 },
        { 0, 0, NULL, NULL, NULL, NULL, "a,b=c", "z=,", "p\xC2\xA0w", "p w", 4096, 4096, MAILK_OK, MAILK_OK, MAILK_OK, MAILK_SCRAM_SHA256 },
        { 0, 0, NULL, NULL, NULL, NULL, "\xC3\xBC" "ser", NULL, "\xE2\x85\xA8", "IX", 4096, 4096, MAILK_OK, MAILK_OK, MAILK_OK, MAILK_SCRAM_SHA512 },
        { 0, 0, NULL, NULL, NULL, NULL, "user", NULL, "wrong", "pencil", 4096, 4096, MAILK_OK, MAILK_EAUTH, MAILK_EAUTH, MAILK_SCRAM_SHA256 },
        { 0, 0, NULL, NULL, NULL, NULL, "user", NULL, "pencil", "pencil", 100, 4096, MAILK_EAUTH, 0, 0, MAILK_SCRAM_SHA256 },
        { 0, 0, NULL, NULL, NULL, NULL, "user", NULL, "pencil", "pencil", 2, 1, MAILK_OK, MAILK_OK, MAILK_OK, MAILK_SCRAM_SHA1 },
        { 'p', 1, "tls-exporter", "tls-exporter", "EKM-32-bytes-of-keying-material!", "EKM-32-bytes-of-keying-material!", "u", NULL, "pw", "pw", 4096, 4096, MAILK_OK, MAILK_OK, MAILK_OK, MAILK_SCRAM_SHA256 },
        { 'p', 1, "tls-exporter", "tls-exporter", "EKM-32-bytes-of-keying-material!", "EKM-from-a-different-tls-session", "u", NULL, "pw", "pw", 4096, 4096, MAILK_OK, MAILK_EAUTH, MAILK_EAUTH, MAILK_SCRAM_SHA256 },
        { 'p', 1, "tls-server-end-point", "tls-exporter", "x", "y", "u", NULL, "pw", "pw", 4096, 4096, 0, MAILK_EAUTH, 0, MAILK_SCRAM_SHA256 },
        { 'p', 0, "tls-exporter", NULL, "x", NULL, "u", NULL, "pw", "pw", 4096, 4096, 0, MAILK_EAUTH, 0, MAILK_SCRAM_SHA256 },
        { 'y', 1, NULL, "tls-exporter", NULL, "x", "u", NULL, "pw", "pw", 4096, 4096, 0, MAILK_EAUTH, 0, MAILK_SCRAM_SHA256 },
        { 'y', 0, NULL, NULL, NULL, NULL, "u", NULL, "pw", "pw", 4096, 4096, MAILK_OK, MAILK_OK, MAILK_OK, MAILK_SCRAM_SHA256 },
    };
    size_t i;
    char tag[32];
    for (i = 0; i < sizeof(rts) / sizeof(rts[0]); i++) {
        snprintf(tag, sizeof(tag), "rt%zu", i);
        roundtrip(&rts[i], tag);
    }
}

static void test_scram_refusals(void)
{
    mailk_scram_t c, s;
    dyn_sb_t o;
    static const char* bad_first[] = {
        "n,,m=ext,n=user,r=abc", "x,,n=user,r=abc", "n,,n=u=2x,r=abc", "n,,n=,r=abc", "n,,n=user,r=", "n,,n=user",
        "n,,n=user,r=a\177b", "p=,,n=u,r=abc", "n,a=,n=u,r=abc", "n,,n=user,r=abc,", "n,n=user,r=abc", "n,,n=user,r=ab,c",
    };
    size_t i;
    dyn_sb_init(&o);
    for (i = 0; i < sizeof(bad_first) / sizeof(bad_first[0]); i++) {
        mailk_scram_init(&s, MAILK_SCRAM_SHA256);
        CHECK(mailk_scram_server_first(&s, U(bad_first[i])) != MAILK_OK, "server-first refuses %s", bad_first[i]);
        mailk_scram_free(&s);
    }
    mailk_scram_init(&s, MAILK_SCRAM_SHA256);
    CHECK(mailk_scram_server_first(&s, U("n,a=adm=2Cin,n=us=3Der,r=abc,x=ext")) == MAILK_OK && SBEQ(&s.authzid, "adm,in") && SBEQ(&s.user, "us=er"), "authzid + extension accepted");
    mailk_scram_free(&s);
    mailk_scram_init(&c, MAILK_SCRAM_SHA256);
    o.n = 0;
    CHECK(mailk_scram_client_first(&c, "u", "\xC2\xAD", "pw", "abc", &o) == MAILK_OK && SBEQ(&o, "n,,n=u,r=abc"), "authzid prepping to empty is omitted");
    mailk_scram_free(&c);
    {
        static const char* bad_sfirst[] = {
            "r=OTHERNONCEabc,s=QSXCR+Q6sek8bf92,i=4096", "r=fyko,s=QSXCR+Q6sek8bf92,i=4096",
            "m=x,r=fyko+d2lbbFgONRv9qkxdawLz,s=QSXCR+Q6sek8bf92,i=4096", "r=fyko+d2lbbFgONRv9qkxdawLz,s=,i=4096",
            "r=fyko+d2lbbFgONRv9qkxdawLz,s=QSX,i=4096", "r=fyko+d2lbbFgONRv9qkxdawLz,s=QSXCR+Q6sek8bf92,i=04096",
            "r=fyko+d2lbbFgONRv9qkxdawLz,s=QSXCR+Q6sek8bf92,i=99999999999", "r=fyko+d2lbbFgONRv9qkxdawLz,s=QSXCR+Q6sek8bf92",
            "r=fyko+d2lbbFgONRv9qkxdawLz,i=4096,s=QSXCR+Q6sek8bf92",
            "r=fyko+d2lbbFgONRv9qkxdawXLz,s=QSXCR+Q6sek8bf92,i=4096",
        };
        for (i = 0; i < sizeof(bad_sfirst) / sizeof(bad_sfirst[0]); i++) {
            mailk_scram_init(&c, MAILK_SCRAM_SHA1);
            o.n = 0;
            mailk_scram_client_first(&c, "user", NULL, "pencil", "fyko+d2lbbFgONRv9qkxdawL", &o);
            o.n = 0;
            CHECK(mailk_scram_client_final(&c, U(bad_sfirst[i]), &o) == MAILK_EBAD, "client refuses %s", bad_sfirst[i]);
            mailk_scram_free(&c);
        }
    }
    mailk_scram_init(&c, MAILK_SCRAM_SHA1);
    o.n = 0;
    mailk_scram_client_first(&c, "user", NULL, "pencil", "fyko+d2lbbFgONRv9qkxdawL", &o);
    o.n = 0;
    mailk_scram_client_final(&c, U(vecs[0].sfirst), &o);
    CHECK(mailk_scram_client_verify(&c, U("v=rmF9pqV8S7suAoZWja4dJRkFsKR=")) == MAILK_EAUTH, "tampered server signature");
    mailk_scram_free(&c);
    mailk_scram_init(&c, MAILK_SCRAM_SHA1);
    o.n = 0;
    mailk_scram_client_first(&c, "user", NULL, "pencil", "fyko+d2lbbFgONRv9qkxdawL", &o);
    o.n = 0;
    CHECK(mailk_scram_client_final(&c, U("r=fyko+d2lbbFgONRv9qkxdawLz,s=QSXCR+Q6sek8bf92,i=1000001"), &o) == MAILK_EAUTH, "iterations over max_iters");
    mailk_scram_free(&c);
    mailk_scram_init(&c, MAILK_SCRAM_SHA1);
    o.n = 0;
    mailk_scram_client_first(&c, "user", NULL, "pencil", "fyko+d2lbbFgONRv9qkxdawL", &o);
    o.n = 0;
    mailk_scram_client_final(&c, U(vecs[0].sfirst), &o);
    CHECK(mailk_scram_client_verify(&c, U("e=invalid-proof")) == MAILK_EAUTH, "server error");
    mailk_scram_free(&c);
    mailk_scram_init(&c, MAILK_SCRAM_SHA256);
    o.n = 0;
    CHECK(mailk_scram_client_final(&c, U(vecs[1].sfirst), &o) == MAILK_EBAD, "final before first");
    CHECK(mailk_scram_client_first(&c, "", NULL, "pw", NULL, &o) == MAILK_EBAD, "empty user");
    mailk_scram_free(&c);
    mailk_scram_init(&c, MAILK_SCRAM_SHA256);
    CHECK(mailk_scram_client_first(&c, "u", NULL, "a\x07", NULL, &o) == MAILK_EBAD, "prohibited password");
    mailk_scram_free(&c);
    mailk_scram_init(&c, MAILK_SCRAM_SHA256);
    CHECK(mailk_scram_client_first(&c, "u", NULL, "pw", "a,b", &o) == MAILK_EBAD, "nonce with comma");
    mailk_scram_free(&c);
    mailk_scram_init(&c, MAILK_SCRAM_SHA256);
    CHECK(mailk_scram_cb(&c, 'p', "bad name!", (const uint8_t*)"x", 1) == MAILK_EBAD, "cb name grammar");
    CHECK(mailk_scram_cb(&c, 'q', NULL, NULL, 0) == MAILK_EBAD, "cb flag");
    mailk_scram_free(&c);
    {
        static const char* bad_final[] = {
            "c=biws,r=rOprNGfwEbeRWgbNEkqO%hvYDpWUa2RaTCAfuxFIlj)hNlF$k0",
            "c=biws,r=rOprNGfwEbeRWgbNEkqOXX,p=dHzbZapWIk4jUhN+Ute9ytag9zjfMHgsqmmiz7AndVQ=",
            "c=eSws,r=rOprNGfwEbeRWgbNEkqO%hvYDpWUa2RaTCAfuxFIlj)hNlF$k0,p=dHzbZapWIk4jUhN+Ute9ytag9zjfMHgsqmmiz7AndVQ=",
            "c=biws,r=rOprNGfwEbeRWgbNEkqO%hvYDpWUa2RaTCAfuxFIlj)hNlF$k0,p=dHzbZapWIk4jUhN+Ute9ytag9zjfMHgsqmmiz7AndVR=",
            "c=biws,r=rOprNGfwEbeRWgbNEkqO%hvYDpWUa2RaTCAfuxFIlj)hNlF$k0,p=dHzbZapWIk4jUhN+Ute9ytag9zjfMHgsqmmiz7AndVQ=,x=1",
            "r=rOprNGfwEbeRWgbNEkqO%hvYDpWUa2RaTCAfuxFIlj)hNlF$k0,c=biws,p=dHzbZapWIk4jUhN+Ute9ytag9zjfMHgsqmmiz7AndVQ=",
            "c=biws,r=rOprNGfwEbeRWgbNEkqO%hvYDpWUa2RaTCAfuxFIlj)hNlF$k0,p=AAAA",
        };
        dyn_sb_t salt;
        uint8_t st[64], sk[64];
        dyn_sb_init(&salt);
        dec(vecs[1].salt, &salt);
        mailk_scram_derive(MAILK_SCRAM_SHA256, "pencil", (const uint8_t*)salt.p, salt.n, 4096, st, sk);
        for (i = 0; i < sizeof(bad_final) / sizeof(bad_final[0]); i++) {
            mailk_scram_init(&s, MAILK_SCRAM_SHA256);
            mailk_scram_server_first(&s, U(vecs[1].cfirst));
            o.n = 0;
            mailk_scram_server_challenge(&s, (const uint8_t*)salt.p, salt.n, 4096, st, sk, vecs[1].snonce, &o);
            o.n = 0;
            CHECK(mailk_scram_server_final(&s, U(bad_final[i]), &o) == MAILK_EAUTH && o.n > 2 && o.p[0] == 'e' && o.p[1] == '=', "server refuses final %zu", i);
            mailk_scram_free(&s);
        }
        dyn_sb_free(&salt);
    }
    dyn_sb_free(&o);
}

static void mutate(uint8_t* b, size_t* n, size_t cap)
{
    int k, m = 1 + (int)(rnd() % 4);
    for (k = 0; k < m; k++) {
        size_t at = *n ? rnd() % *n : 0;
        switch (rnd() % 4) {
        case 0:
            if (*n)
                b[at] = (uint8_t)rnd();
            break;
        case 1:
            if (*n)
                memmove(b + at, b + at + 1, --*n - at);
            break;
        case 2:
            if (*n < cap) {
                memmove(b + at + 1, b + at, *n - at);
                b[at] = "=,\001\000a-+/"[rnd() % 8];
                ++*n;
            }
            break;
        default:
            if (*n)
                *n = rnd() % *n;
            break;
        }
    }
}

static void test_fuzz(void)
{
    static const char* seeds[] = {
        "n,,n=user,r=rOprNGfwEbeRWgbNEkqO", "r=rOprNGfwEbeRWgbNEkqO%hvYDpWUa2RaTCAfuxFIlj)hNlF$k0,s=W22ZaJ0SNY7soEsUEjb6gQ==,i=4096",
        "c=biws,r=rOprNGfwEbeRWgbNEkqO%hvYDpWUa2RaTCAfuxFIlj)hNlF$k0,p=dHzbZapWIk4jUhN+Ute9ytag9zjfMHgsqmmiz7AndVQ=",
        "n,a=user@example.com,\001host=server.example.com\001port=143\001auth=Bearer vF9dft4qmTc2Nvb3RlckBhbHRhdmlzdGEuY29tCg==\001\001",
        "user=someuser@example.com\001auth=Bearer ya29.vF9dft4qmTc2N\001\001", "Ursel\0Kurt\0xipj3plmq", "Zm9vYmE=",
        "I\xC2\xADX\xE2\x85\xA8\xD8\xA7",
    };
    static const size_t seedn[] = { 0, 0, 0, 0, 0, 20, 0, 0 };
    uint8_t buf[512];
    dyn_sb_t o, salt;
    uint8_t st[32], sk[32];
    int it;
    dyn_sb_init(&o);
    dyn_sb_init(&salt);
    dec(vecs[1].salt, &salt);
    mailk_scram_derive(MAILK_SCRAM_SHA256, "pencil", (const uint8_t*)salt.p, salt.n, 2, st, sk);
    for (it = 0; it < 100000; it++) {
        size_t n, si = rnd() % 8;
        mailk_plain_t p;
        mailk_oauth_t t;
        mailk_scram_t c, s;
        if (rnd() % 4 == 0) {
            n = rnd() % 200;
            for (size_t i = 0; i < n; i++)
                buf[i] = (uint8_t)rnd();
        } else {
            n = seedn[si] ? seedn[si] : strlen(seeds[si]);
            memcpy(buf, seeds[si], n);
            mutate(buf, &n, sizeof(buf));
        }
        o.n = 0;
        mailk_b64_decode((const char*)buf, n, &o);
        o.n = 0;
        mailk_sasl_resp_decode((const char*)buf, n, &o);
        o.n = 0;
        mailk_saslprep((const char*)buf, n, &o);
        o.n = 0;
        mailk_saslname_decode(&o, (const char*)buf, n);
        mailk_plain_parse(buf, n, &p);
        mailk_login_prompt(buf, n);
        mailk_xoauth2_parse(buf, n, &t);
        mailk_oauthbearer_parse(buf, n, &t);
        mailk_scram_init(&s, MAILK_SCRAM_SHA256);
        if (mailk_scram_server_first(&s, buf, n) == MAILK_OK) {
            o.n = 0;
            if (mailk_scram_server_challenge(&s, (const uint8_t*)salt.p, salt.n, 2, st, sk, NULL, &o) == MAILK_OK) {
                o.n = 0;
                mailk_scram_server_final(&s, buf, n, &o);
            }
        }
        mailk_scram_free(&s);
        mailk_scram_init(&s, MAILK_SCRAM_SHA256);
        mailk_scram_server_first(&s, U(vecs[1].cfirst));
        o.n = 0;
        mailk_scram_server_challenge(&s, (const uint8_t*)salt.p, salt.n, 2, st, sk, vecs[1].snonce, &o);
        o.n = 0;
        mailk_scram_server_final(&s, buf, n, &o);
        mailk_scram_free(&s);
        mailk_scram_init(&c, MAILK_SCRAM_SHA256);
        c.min_iters = 1;
        c.max_iters = 64;
        o.n = 0;
        mailk_scram_client_first(&c, "user", NULL, "pencil", "rOprNGfwEbeRWgbNEkqO", &o);
        o.n = 0;
        if (mailk_scram_client_final(&c, buf, n, &o) == MAILK_OK)
            mailk_scram_client_verify(&c, buf, n);
        mailk_scram_free(&c);
    }
    CHECK(1, "fuzz loop survived");
    dyn_sb_free(&o);
    dyn_sb_free(&salt);
}


int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

static const char b64t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void model_b64(const uint8_t* p, size_t n, dyn_sb_t* o)
{
    size_t i, bits = 0;
    uint32_t acc = 0;
    for (i = 0; i < n; i++) {
        acc = (acc << 8) | p[i];
        bits += 8;
        while (bits >= 6) {
            bits -= 6;
            dyn_sb_putc(o, b64t[(acc >> bits) & 63]);
        }
    }
    if (bits)
        dyn_sb_putc(o, b64t[(acc << (6 - bits)) & 63]);
    while (o->n % 4)
        dyn_sb_putc(o, '=');
}

static int model_b64_valid(const char* s, size_t n)
{
    size_t i, pad = 0, data;
    uint32_t last = 0;
    if (n % 4)
        return 0;
    while (pad < 2 && pad < n && s[n - 1 - pad] == '=')
        pad++;
    data = n - pad;
    for (i = 0; i < data; i++) {
        const char* q = s[i] ? strchr(b64t, s[i]) : NULL;
        if (!q)
            return 0;
        last = (uint32_t)(q - b64t);
    }
    if (pad && data % 4 != 4 - pad)
        return 0;
    if (pad == 1 && (last & 3))
        return 0;
    if (pad == 2 && (last & 15))
        return 0;
    return 1;
}

static void test_diff_b64(void)
{
    uint8_t buf[300];
    dyn_sb_t e, w, d;
    long it, N = (long)fk_iters(30000);
    int bad = 0;
    fk_g = fk_stream("diff-b64");
    dyn_sb_init(&e);
    dyn_sb_init(&w);
    dyn_sb_init(&d);
    for (it = 0; it < N; it++) {
        size_t n = rnd() % 64, i;
        int v, got;
        for (i = 0; i < n; i++)
            buf[i] = (uint8_t)rnd();
        e.n = w.n = d.n = 0;
        model_b64(buf, n, &w);
        if (mailk_b64_encode(&e, buf, n) != MAILK_OK || e.n != w.n || (w.n && memcmp(e.p, w.p, w.n)) || mailk_b64_decode(w.p ? w.p : "", w.n, &d) != MAILK_OK || d.n != n
            || (n && memcmp(d.p, buf, n))) {
            if (bad++ < 3)
                CHECK(0, "[diff-b64] encode/decode differs from the RFC 4648 model it=%ld n=%zu", it, n);
        }
        if (w.n) {
            int k, m = 1 + (int)(rnd() % 3);
            for (k = 0; k < m; k++) {
                size_t at = rnd() % w.n;
                switch (rnd() % 4) {
                case 0:
                    w.p[at] = "A=+/ \r\x00z"[rnd() % 8];
                    break;
                case 1:
                    w.p[at] = b64t[rnd() % 64];
                    break;
                case 2:
                    if (w.n > 1) {
                        memmove(w.p + at, w.p + at + 1, w.n - at - 1);
                        w.n--;
                    }
                    break;
                default:
                    dyn_sb_putc(&w, '=');
                    break;
                }
            }
        }
        v = model_b64_valid(w.p ? w.p : "", w.n);
        d.n = 0;
        got = mailk_b64_decode(w.p ? w.p : "", w.n, &d) == MAILK_OK;
        if (got != v && bad++ < 3)
            CHECK(0, "[diff-b64] strict decode %s [%.*s], model says %s it=%ld", got ? "accepted" : "refused", (int)w.n, w.p ? w.p : "", v ? "valid" : "invalid", it);
    }
    CHECK(bad == 0, "[diff-b64] %d disagreements with the RFC 4648 model in %ld inputs", bad, N);
    dyn_sb_free(&e);
    dyn_sb_free(&w);
    dyn_sb_free(&d);
}

static void test_rt_saslname(void)
{
    char buf[256];
    dyn_sb_t e, w, d;
    long it, N = (long)fk_iters(20000);
    int bad = 0;
    fk_g = fk_stream("rt-saslname");
    dyn_sb_init(&e);
    dyn_sb_init(&w);
    dyn_sb_init(&d);
    for (it = 0; it < N; it++) {
        size_t n = 1 + rnd() % 60, i;
        for (i = 0; i < n; i++)
            buf[i] = ",=a=3Dz,2C@ "[rnd() % 12];
        e.n = w.n = d.n = 0;
        for (i = 0; i < n; i++)
            dyn_sb_puts(&w, buf[i] == ',' ? "=2C" : buf[i] == '=' ? "=3D" : (char[2]){ buf[i], 0 });
        if (mailk_saslname_encode(&e, buf, n) != MAILK_OK || e.n != w.n || memcmp(e.p, w.p, w.n) || mailk_saslname_decode(&d, e.p, e.n) != MAILK_OK || d.n != n || memcmp(d.p, buf, n)) {
            if (bad++ < 3)
                CHECK(0, "[rt-saslname] RFC 5802 saslname escape differs it=%ld [%.*s]", it, (int)n, buf);
        }
    }
    CHECK(bad == 0, "[rt-saslname] %d of %ld names differ", bad, N);
    dyn_sb_free(&e);
    dyn_sb_free(&w);
    dyn_sb_free(&d);
}

static void rand_ascii(char* b, size_t n, const char* extra)
{
    size_t i;
    for (i = 0; i < n; i++)
        b[i] = extra && rnd() % 4 == 0 ? extra[rnd() % strlen(extra)] : (char)(33 + rnd() % 94);
    b[n] = 0;
}

static void test_rt_plain(void)
{
    char az[300], ac[300], pw[300];
    dyn_sb_t o;
    long it, N = (long)fk_iters(10000);
    int bad = 0;
    fk_g = fk_stream("rt-plain");
    dyn_sb_init(&o);
    for (it = 0; it < N; it++) {
        mailk_plain_t p;
        size_t zn = rnd() % 4 == 0 ? 0 : 1 + rnd() % 255, cn = 1 + rnd() % 255, pn = 1 + rnd() % 255;
        rand_ascii(az, zn, " \t\x7f");
        rand_ascii(ac, cn, " ");
        rand_ascii(pw, pn, "\xc3\xa9");
        if (pn > 1 && (uint8_t)pw[pn - 1] == 0xc3)
            pw[pn - 1] = 'x';
        {
            size_t i;
            for (i = 0; i + 1 < pn; i++)
                if ((uint8_t)pw[i] == 0xc3)
                    pw[i + 1] = (char)0xa9, i++;
                else if ((uint8_t)pw[i] == 0xa9)
                    pw[i] = 'y';
            if ((uint8_t)pw[pn - 1] == 0xa9 && (pn < 2 || (uint8_t)pw[pn - 2] != 0xc3))
                pw[pn - 1] = 'y';
            if ((uint8_t)pw[pn - 1] == 0xc3)
                pw[pn - 1] = 'x';
        }
        o.n = 0;
        if (mailk_plain_build(&o, zn ? az : NULL, ac, pw) != MAILK_OK || mailk_plain_parse((const uint8_t*)o.p, o.n, &p) != MAILK_OK || p.azn != zn || memcmp(o.p + p.az, az, zn) || p.acn != cn
            || memcmp(o.p + p.ac, ac, cn) || p.pwn != pn || memcmp(o.p + p.pw, pw, pn)) {
            if (bad++ < 3)
                CHECK(0, "[rt-plain] RFC 4616 build/parse round trip lost a field it=%ld (%zu/%zu/%zu)", it, zn, cn, pn);
        }
    }
    CHECK(bad == 0, "[rt-plain] %d of %ld messages differ", bad, N);
    dyn_sb_free(&o);
}

static void test_bound(void)
{
    dyn_sb_t b, o;
    mailk_plain_t p;
    dyn_sb_init(&b);
    dyn_sb_init(&o);
    dyn_sb_putc(&b, 0);
    dyn_sb_putc(&b, 'u');
    dyn_sb_putc(&b, 0);
    while (b.n < 3 + MAILK_SASL_MAX)
        dyn_sb_putc(&b, 'p');
    CHECK(mailk_plain_parse((const uint8_t*)b.p, b.n, &p) == MAILK_OK && p.pwn == MAILK_SASL_MAX, "[bound] PLAIN passwd of MAILK_SASL_MAX octets accepted");
    dyn_sb_putc(&b, 'p');
    CHECK(mailk_plain_parse((const uint8_t*)b.p, b.n, &p) == MAILK_EBAD, "[bound] PLAIN passwd of MAILK_SASL_MAX+1 octets refused");
    b.n = 0;
    while (b.n < MAILK_SASL_FIELD_MAX)
        dyn_sb_putc(&b, 'n');
    CHECK(mailk_saslname_encode(&o, b.p, b.n) == MAILK_OK && o.n == MAILK_SASL_FIELD_MAX, "[bound] saslname of 255 octets encodes");
    dyn_sb_putc(&b, 'n');
    o.n = 0;
    CHECK(mailk_saslname_encode(&o, b.p, b.n) == MAILK_EBAD, "[bound] saslname of 256 octets refused");
    b.n = 0;
    while (b.n < MAILK_PREP_MAX)
        dyn_sb_putc(&b, 'q');
    o.n = 0;
    CHECK(mailk_saslprep(b.p, b.n, &o) == MAILK_OK && o.n == MAILK_PREP_MAX, "[bound] SASLprep of 1024 octets");
    dyn_sb_putc(&b, 'q');
    o.n = 0;
    CHECK(mailk_saslprep(b.p, b.n, &o) == MAILK_ELONG, "[bound] SASLprep of 1025 octets is ELONG");
    b.n = 0;
    while (b.n < 4 * ((MAILK_SASL_MAX + 2) / 3))
        dyn_sb_puts(&b, "AAAA");
    o.n = 0;
    CHECK(mailk_b64_decode(b.p, b.n, &o) == MAILK_OK, "[bound] base64 of the largest SASL message decodes");
    dyn_sb_puts(&b, "AAAA");
    o.n = 0;
    CHECK(mailk_b64_decode(b.p, b.n, &o) == MAILK_ELONG, "[bound] base64 one quantum past MAILK_SASL_MAX is ELONG");
    dyn_sb_free(&b);
    dyn_sb_free(&o);
}

typedef struct {
    uint32_t cp;
    const char* utf8;
    const char* mapped;
} prep_cp_t;

static void test_diff_prep(void)
{
    static const prep_cp_t cps[] = { { 0xAD, "\xC2\xAD", "" }, { 0xA0, "\xC2\xA0", " " }, { 0x2003, "\xE2\x80\x83", " " }, { 0x3000, "\xE3\x80\x80", " " },
        { 0xFB01, "\xEF\xAC\x81", "fi" }, { 0x2168, "\xE2\x85\xA8", "IX" }, { 0xFF41, "\xEF\xBD\x81", "a" }, { 0x2060, "\xE2\x81\xA0", "" }, { 0xFE0F, "\xEF\xB8\x8F", "" }, { 0x1680, "\xE1\x9A\x80", " " } };
    dyn_sb_t in, want, got;
    long it, N = (long)fk_iters(20000);
    int bad = 0;
    fk_g = fk_stream("diff-prep");
    dyn_sb_init(&in);
    dyn_sb_init(&want);
    dyn_sb_init(&got);
    for (it = 0; it < N; it++) {
        int k, nk = 1 + (int)(rnd() % 20), any = 0;
        in.n = want.n = got.n = 0;
        for (k = 0; k < nk; k++) {
            if (rnd() % 3 == 0) {
                const prep_cp_t* c = &cps[rnd() % (sizeof cps / sizeof cps[0])];
                dyn_sb_puts(&in, c->utf8);
                dyn_sb_puts(&want, c->mapped);
                any = 1;
            } else {
                char ch = (char)(32 + rnd() % 95);
                dyn_sb_putc(&in, ch);
                dyn_sb_putc(&want, ch);
            }
        }
        (void)any;
        if (mailk_saslprep(in.p, in.n, &got) != MAILK_OK || got.n != want.n || (want.n && memcmp(got.p, want.p, want.n))) {
            if (bad++ < 3)
                CHECK(0, "[diff-prep] SASLprep differs from the RFC 4013 map/NFKC model it=%ld [%.*s] got [%.*s] want [%.*s]", it, (int)in.n, in.p, (int)got.n, got.p ? got.p : "", (int)want.n,
                    want.p ? want.p : "");
        }
    }
    CHECK(bad == 0, "[diff-prep] %d of %ld strings differ", bad, N);
    dyn_sb_free(&in);
    dyn_sb_free(&want);
    dyn_sb_free(&got);
}

static void test_rt_scram(void)
{
    static const int algos[] = { MAILK_SCRAM_SHA1, MAILK_SCRAM_SHA256, MAILK_SCRAM_SHA512 };
    long it, N = (long)fk_iters(30);
    int bad = 0;
    fk_g = fk_stream("rt-scram");
    for (it = 0; it < N; it++) {
        int algo = algos[rnd() % 3], wrong = rnd() % 4 == 0;
        char user[40], authz[40], pw[40], cn[30], sn[30];
        uint8_t salt[16], st[64], sk[64];
        size_t saltn = 4 + rnd() % 12, i;
        mailk_scram_t c, s;
        dyn_sb_t o;
        int r1, r2, r3, r4, r5;
        rand_ascii(user, 1 + rnd() % 30, ",=");
        rand_ascii(authz, rnd() % 3 ? 0 : 1 + rnd() % 30, ",=");
        rand_ascii(pw, 1 + rnd() % 30, NULL);
        rand_ascii(cn, 8 + rnd() % 20, NULL);
        rand_ascii(sn, 8 + rnd() % 20, NULL);
        for (i = 0; cn[i]; i++)
            if (cn[i] == ',')
                cn[i] = '.';
        for (i = 0; sn[i]; i++)
            if (sn[i] == ',')
                sn[i] = '.';
        for (i = 0; i < saltn; i++)
            salt[i] = (uint8_t)rnd();
        dyn_sb_init(&o);
        mailk_scram_init(&c, algo);
        mailk_scram_init(&s, algo);
        r1 = mailk_scram_client_first(&c, user, authz[0] ? authz : NULL, pw, cn, &o);
        r2 = r1 ? r1 : mailk_scram_server_first(&s, (const uint8_t*)o.p, o.n);
        if (!r2 && (s.user.n != strlen(user) || memcmp(s.user.p, user, s.user.n) || s.authzid.n != strlen(authz) || (s.authzid.n && memcmp(s.authzid.p, authz, s.authzid.n))))
            r2 = 99;
        mailk_scram_derive(algo, wrong ? "not-the-password" : pw, salt, saltn, 4096, st, sk);
        o.n = 0;
        r3 = r2 ? r2 : mailk_scram_server_challenge(&s, salt, saltn, 4096, st, sk, sn, &o);
        {
            dyn_sb_t f;
            dyn_sb_init(&f);
            r4 = r3 ? r3 : mailk_scram_client_final(&c, (const uint8_t*)o.p, o.n, &f);
            o.n = 0;
            r5 = r4 ? r4 : mailk_scram_server_final(&s, (const uint8_t*)f.p, f.n, &o);
            dyn_sb_free(&f);
        }
        if (wrong ? (r4 != MAILK_OK || r5 != MAILK_EAUTH) : (r5 != MAILK_OK || mailk_scram_client_verify(&c, (const uint8_t*)o.p, o.n) != MAILK_OK)) {
            if (bad++ < 3)
                CHECK(0, "[rt-scram] SCRAM exchange it=%ld algo %d wrong %d: rc %d %d %d %d %d user [%s] authz [%s]", it, algo, wrong, r1, r2, r3, r4, r5, user, authz);
        }
        mailk_scram_free(&c);
        mailk_scram_free(&s);
        dyn_sb_free(&o);
    }
    CHECK(bad == 0, "[rt-scram] %d of %ld random-credential SCRAM exchanges failed", bad, N);
}

static void test_fuzz_entry(void)
{
    static const char* const seeds[] = { "\x05n,,n=user,r=fyko+d2lbbFgONRv9qkxdawL", "\x00" "AHRpbQB0YW5zdGFhZ3RoZQ==", "\x06Ursel\0Kurt\0xipj3plmq",
        "\x01n,a=user@example.com,\001host=server.example.com\001port=143\001auth=Bearer vF9dft4qmTc2Nvb3RlckBhbHRhdmlzdGEuY29tCg==\001\001",
        "\x02r=fyko+d2lbbFgONRv9qkxdawL3rfcNHYJY1ZVvWVs7j,s=QSXCR+Q6sek8bf92,i=4096" };
    uint8_t buf[1024];
    long it, N = (long)fk_iters(8000);
    fk_g = fk_stream("sasl-fuzz-entry");
    printf("[fuzz-sasl] start\n");
    fflush(stdout);
    for (it = 0; it < N; it++) {
        const char* sd = seeds[rnd() % 5];
        size_t n = (sd[0] ? strlen(sd) : 1 + strlen(sd + 1));
        memcpy(buf, sd, n);
        buf[0] = (uint8_t)rnd();
        mutate(buf + 1, &n, sizeof buf - 1);
        LLVMFuzzerTestOneInput(buf, n + 1);
    }
    printf("[fuzz-sasl] done\n");
    CHECK(1, "[fuzz-sasl] %ld mutated SASL inputs through src/fuzz/fuzz_mailk_sasl.c", N);
}

int main(void)
{
    fk_init("test_mailk_sasl");
    fflush(stdout);
    fk_g = fk_stream("fixed");
    test_b64();
    test_saslprep();
    test_plain_login();
    test_oauth();
    test_scram_vectors();
    test_scram_roundtrip();
    test_scram_refusals();
    fk_g = fk_stream("fuzz");
    test_fuzz();
    test_diff_b64();
    test_rt_saslname();
    test_rt_plain();
    test_bound();
    test_diff_prep();
    test_rt_scram();
    test_fuzz_entry();    printf("%d checks\n", checks);
    fk_fails += fails;
    return fk_done(__FILE__);
}
