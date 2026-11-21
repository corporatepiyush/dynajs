#include "core/dyn-mailk.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

static void die(const char* why)
{
    fprintf(stderr, "fuzz_mailk_sasl oracle: %s\n", why);
    abort();
}

static int same(const dyn_sb_t* a, const void* p, size_t n)
{
    return a->n == n && (!n || !memcmp(a->p, p, n));
}

static char* dupz(const uint8_t* p, size_t n)
{
    char* q = malloc(n + 1);
    if (q) {
        memcpy(q, p, n);
        q[n] = 0;
    }
    return q;
}

static void check_b64(const uint8_t* p, size_t n)
{
    dyn_sb_t a, b;
    dyn_sb_init(&a);
    dyn_sb_init(&b);
    if (n <= MAILK_SASL_MAX) {
        if (mailk_b64_encode(&a, p, n) != MAILK_OK || a.n != 4 * ((n + 2) / 3))
            die("b64 encode length");
        if (mailk_b64_decode(a.p ? a.p : "", a.n, &b) != MAILK_OK || !same(&b, p, n))
            die("b64 encode/decode identity");
    }
    a.n = b.n = 0;
    if (mailk_b64_decode((const char*)p, n, &a) == MAILK_OK) {
        if (n % 4 || a.n > 3 * (n / 4))
            die("b64 decode accepted a bad length");
        if (mailk_b64_encode(&b, (const uint8_t*)a.p, a.n) != MAILK_OK || !same(&b, p, n))
            die("b64 decode is not canonical (re-encode differs)");
    }
    a.n = b.n = 0;
    {
        int rc = mailk_sasl_resp_decode((const char*)p, n, &a);
        if (rc == MAILK_OK && (mailk_sasl_resp_encode(&b, (const uint8_t*)a.p, a.n) != MAILK_OK || !same(&b, p, n)))
            die("sasl response decode/encode round trip");
        if (rc == MAILK_CANCEL && !(n == 1 && p[0] == '*'))
            die("cancel without a lone *");
    }
    dyn_sb_free(&a);
    dyn_sb_free(&b);
}

static void check_names(const uint8_t* p, size_t n)
{
    dyn_sb_t a, b, c;
    dyn_sb_init(&a);
    dyn_sb_init(&b);
    dyn_sb_init(&c);
    if (mailk_saslname_decode(&a, (const char*)p, n) == MAILK_OK) {
        if (mailk_saslname_encode(&b, a.p ? a.p : "", a.n) != MAILK_OK || !same(&b, p, n))
            die("saslname decode/encode round trip");
    }
    a.n = b.n = 0;
    if (mailk_saslname_encode(&a, (const char*)p, n) == MAILK_OK
        && (mailk_saslname_decode(&b, a.p ? a.p : "", a.n) != MAILK_OK || !same(&b, p, n)))
        die("saslname encode/decode identity");
    a.n = b.n = 0;
    if (mailk_saslprep((const char*)p, n, &a) == MAILK_OK) {
        size_t i = 0;
        while (i < a.n) {
            int k = mailk_utf8_len((const uint8_t*)a.p + i, a.n - i);
            if (k <= 0)
                die("saslprep output is not UTF-8");
            if (k == 1 && ((uint8_t)a.p[i] < 0x20 || a.p[i] == 0x7F))
                die("saslprep output has a control character");
            i += (size_t)k;
        }
        if (mailk_saslprep(a.p ? a.p : "", a.n, &c) != MAILK_OK || !same(&c, a.p, a.n))
            die("saslprep is not idempotent");
    }
    dyn_sb_free(&a);
    dyn_sb_free(&b);
    dyn_sb_free(&c);
}

static void check_plain(const uint8_t* p, size_t n)
{
    mailk_plain_t pl, p2;
    dyn_sb_t a;
    char *z, *c, *w;
    if (mailk_plain_parse(p, n, &pl) != MAILK_OK)
        return;
    if (pl.azn + pl.acn + pl.pwn + 2 != n || pl.ac != pl.azn + 1 || pl.pw != pl.ac + pl.acn + 1 || !pl.acn || !pl.pwn)
        die("plain offsets");
    z = dupz(p, pl.azn);
    c = dupz(p + pl.ac, pl.acn);
    w = dupz(p + pl.pw, pl.pwn);
    dyn_sb_init(&a);
    if (z && c && w) {
        if (mailk_plain_build(&a, pl.azn ? z : NULL, c, w) != MAILK_OK || !same(&a, p, n))
            die("plain parse/build round trip");
        if (mailk_plain_parse((const uint8_t*)a.p, a.n, &p2) != MAILK_OK || memcmp(&pl, &p2, sizeof pl))
            die("plain build/parse round trip");
    }
    dyn_sb_free(&a);
    free(z);
    free(c);
    free(w);
}

static void check_oauth(const uint8_t* p, size_t n)
{
    mailk_oauth_t o, o2;
    dyn_sb_t a, t;
    dyn_sb_init(&a);
    dyn_sb_init(&t);
    if (mailk_xoauth2_parse(p, n, &o) == MAILK_OK) {
        char* u = dupz(p + o.az, o.azn);
        char* k = dupz(p + o.tok, o.tokn);
        if (o.az + o.azn > n || o.tok + o.tokn > n || !o.azn || !o.tokn)
            die("xoauth2 offsets");
        if (u && k && mailk_xoauth2_build(&a, u, k) == MAILK_OK
            && (mailk_xoauth2_parse((const uint8_t*)a.p, a.n, &o2) != MAILK_OK || o2.azn != o.azn || memcmp(a.p + o2.az, u, o.azn) || o2.tokn != o.tokn
                || memcmp(a.p + o2.tok, k, o.tokn)))
            die("xoauth2 build/parse round trip");
        free(u);
        free(k);
    }
    a.n = 0;
    if (mailk_oauthbearer_parse(p, n, &o) == MAILK_OK) {
        unsigned port = 0;
        size_t i;
        int canon = o.portn > 0 && o.portn <= 5 && p[o.port] != '0';
        if (o.az + o.azn > n || o.host + o.hostn > n || o.port + o.portn > n || o.tok + o.tokn > n || !o.tokn)
            die("oauthbearer offsets");
        for (i = 0; i < o.portn; i++)
            port = port * 10 + (unsigned)(p[o.port + i] - '0');
        if (!o.portn || (canon && port <= 65535)) {
            char* k = dupz(p + o.tok, o.tokn);
            char* h = dupz(p + o.host, o.hostn);
            if (o.azn && mailk_saslname_decode(&t, (const char*)p + o.az, o.azn) != MAILK_OK)
                die("oauthbearer accepted a bad authzid");
            dyn_sb_putc(&t, 0);
            if (k && h && !t.oom && mailk_oauthbearer_build(&a, o.azn ? t.p : NULL, o.hostn ? h : NULL, port, k) == MAILK_OK
                && (mailk_oauthbearer_parse((const uint8_t*)a.p, a.n, &o2) != MAILK_OK || o2.tokn != o.tokn || memcmp(a.p + o2.tok, k, o.tokn) || o2.hostn != o.hostn
                    || (o.hostn && memcmp(a.p + o2.host, h, o.hostn)) || o2.portn != o.portn || (o.portn && memcmp(a.p + o2.port, p + o.port, o.portn))))
                die("oauthbearer build/parse round trip");
            free(k);
            free(h);
        }
    }
    mailk_login_prompt(p, n);
    dyn_sb_free(&a);
    dyn_sb_free(&t);
}

static void scram_peer(const uint8_t* p, size_t n, int algo)
{
    static const uint8_t salt[4] = { 1, 2, 3, 4 }, key[64] = { 0 };
    mailk_scram_t sc;
    dyn_sb_t a;
    dyn_sb_init(&a);
    mailk_scram_init(&sc, algo);
    if (mailk_scram_server_first(&sc, p, n) == MAILK_OK) {
        if (!sc.user.n)
            die("scram server accepted an empty user");
        if (mailk_scram_server_challenge(&sc, salt, sizeof salt, 1, key, key, "srvnonce", &a) == MAILK_OK)
            mailk_scram_server_final(&sc, p, n, &a);
    }
    mailk_scram_free(&sc);
    mailk_scram_init(&sc, algo);
    sc.min_iters = 1;
    sc.max_iters = 8;
    a.n = 0;
    if (mailk_scram_client_first(&sc, "u", NULL, "p", "clinonce", &a) == MAILK_OK) {
        a.n = 0;
        if (mailk_scram_client_final(&sc, p, n, &a) == MAILK_OK)
            mailk_scram_client_verify(&sc, p, n);
    }
    mailk_scram_free(&sc);
    dyn_sb_free(&a);
}

static void scram_pair(const uint8_t* p, size_t n, int algo, int wrong)
{
    const uint8_t* z1 = memchr(p, 0, n);
    const uint8_t* z2 = z1 ? memchr(z1 + 1, 0, n - (size_t)(z1 + 1 - p)) : NULL;
    uint8_t st[64], sk[64];
    static const uint8_t salt[8] = { 9, 8, 7, 6, 5, 4, 3, 2 };
    mailk_scram_t c, s;
    dyn_sb_t o, f;
    char *user, *authz, *pw;
    int r;
    if (!z2)
        return;
    user = dupz(p, (size_t)(z1 - p));
    authz = dupz(z1 + 1, (size_t)(z2 - z1 - 1));
    pw = dupz(z2 + 1, n - (size_t)(z2 + 1 - p));
    dyn_sb_init(&o);
    dyn_sb_init(&f);
    mailk_scram_init(&c, algo);
    mailk_scram_init(&s, algo);
    c.min_iters = 1;
    if (user && authz && pw && mailk_scram_client_first(&c, user, authz[0] ? authz : NULL, pw, "c1nonce", &o) == MAILK_OK) {
        if (mailk_scram_server_first(&s, (const uint8_t*)o.p, o.n) != MAILK_OK)
            die("scram server refused the client-first message the client built");
        {
            dyn_sb_t pz, pu;
            dyn_sb_init(&pz);
            dyn_sb_init(&pu);
            if (authz[0] && mailk_saslprep(authz, strlen(authz), &pz) != MAILK_OK)
                die("scram client accepted an authzid SASLprep refuses");
            if (mailk_saslprep(user, strlen(user), &pu) != MAILK_OK)
                die("scram client accepted a username SASLprep refuses");
            if (!same(&s.authzid, pz.p, pz.n) || !same(&s.user, pu.p, pu.n))
                die("scram username or authzid changed in transit");
            dyn_sb_free(&pz);
            dyn_sb_free(&pu);
        }
        if (mailk_scram_derive(algo, wrong ? "wrong-password" : pw, salt, sizeof salt, 2, st, sk) == MAILK_OK) {
            o.n = 0;
            if (mailk_scram_server_challenge(&s, salt, sizeof salt, 2, st, sk, "s1nonce", &o) != MAILK_OK)
                die("scram server challenge failed");
            if (mailk_scram_client_final(&c, (const uint8_t*)o.p, o.n, &f) != MAILK_OK)
                die("scram client refused the server-first message");
            o.n = 0;
            r = mailk_scram_server_final(&s, (const uint8_t*)f.p, f.n, &o);
            if (wrong ? r != MAILK_EAUTH : (r != MAILK_OK || mailk_scram_client_verify(&c, (const uint8_t*)o.p, o.n) != MAILK_OK))
                die("scram exchange outcome differs from the password match");
        }
    }
    mailk_scram_free(&c);
    mailk_scram_free(&s);
    dyn_sb_free(&o);
    dyn_sb_free(&f);
    free(user);
    free(authz);
    free(pw);
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    unsigned mode;
    int algo;
    if (!size || size > 65536)
        return 0;
    mode = data[0];
    data++;
    size--;
    algo = 1 + (int)(mode >> 6 & 1u) + (int)(mode >> 7);
    check_b64(data, size);
    check_names(data, size < 4096 ? size : 4096);
    check_plain(data, size);
    check_oauth(data, size);
    if (mode & 1)
        scram_peer(data, size, algo);
    if ((mode & 6) == 6 && size < 1024)
        scram_pair(data, size, algo, (int)(mode >> 3 & 1));
    return 0;
}
