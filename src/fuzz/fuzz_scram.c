#include "dyn-scram.h"
#include "dyn-codec.h"
#include "dyn-hash.h"
#include "core/dyn-simdk.h"

#include <sanitizer/allocator_interface.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXIT 64
#define BIG 16384

typedef struct {
    const uint8_t* p;
    size_t n;
} in_t;

typedef struct {
    char* p;
    size_t n;
} buf_t;

static int inited;
static const dyn_hash_algo_t* sha;

static void die(const char* m)
{
    fprintf(stderr, "oracle: %s\n", m);
    abort();
}

static uint8_t take(in_t* in)
{
    if (!in->n)
        return 0;
    in->n--;
    return *in->p++;
}

static void put(buf_t* b, const void* p, size_t n)
{
    if (b->n + n > BIG)
        n = BIG - b->n;
    memcpy(b->p + b->n, p, n);
    b->n += n;
}

static void puts_(buf_t* b, const char* s)
{
    put(b, s, strlen(s));
}

static const char* fld(const char* m, size_t n, char k, size_t* vl)
{
    size_t i = 0, st;
    while (i < n) {
        st = i;
        while (i < n && m[i] != ',')
            i++;
        if (i - st >= 2 && m[st] == k && m[st + 1] == '=') {
            *vl = i - st - 2;
            return m + st + 2;
        }
        i++;
    }
    return NULL;
}

static long mdec(const char* s, size_t n, uint8_t* out)
{
    static char pad[DYN_SCRAM_MAX_MSG + 4];
    size_t k = n, got;
    if (n == 0 || n > DYN_SCRAM_MAX_MSG || n % 4 == 1)
        return -1;
    memcpy(pad, s, n);
    while (k % 4)
        pad[k++] = '=';
    got = dyn_codec_base64_decode(pad, k, out);
    return got == SIZE_MAX ? -1 : (long)got;
}

typedef struct {
    const char* rv;
    size_t rl;
    uint8_t salt[DYN_SCRAM_MAX_MSG];
    size_t sl;
    uint32_t it;
} sf_t;

static int pred_first(const char* nonce, const char* m, size_t n, sf_t* o)
{
    const char *sv, *iv;
    size_t sl, il, i, nl = strlen(nonce);
    long d;
    if (n == 0 || n > DYN_SCRAM_MAX_MSG || memchr(m, 0, n))
        return DYN_SCRAM_E_SYNTAX;
    if (fld(m, n, 'e', &sl))
        return DYN_SCRAM_E_SERVER;
    if (fld(m, n, 'm', &sl) && sl)
        return DYN_SCRAM_E_EXT;
    o->rv = fld(m, n, 'r', &o->rl);
    sv = fld(m, n, 's', &sl);
    iv = fld(m, n, 'i', &il);
    if (!o->rv || !sv || !iv)
        return DYN_SCRAM_E_SYNTAX;
    if (o->rl <= nl || memcmp(o->rv, nonce, nl))
        return DYN_SCRAM_E_NONCE;
    if (sl > DYN_SCRAM_MAX_SALT || (d = mdec(sv, sl, o->salt)) <= 0)
        return DYN_SCRAM_E_SALT;
    o->sl = (size_t)d;
    if (il == 0 || il > 9)
        return DYN_SCRAM_E_ITERS;
    o->it = 0;
    for (i = 0; i < il; i++) {
        if (iv[i] < '0' || iv[i] > '9')
            return DYN_SCRAM_E_ITERS;
        o->it = o->it * 10 + (uint32_t)(iv[i] - '0');
    }
    if (o->it == 0 || o->it > DYN_SCRAM_MAX_ITERS)
        return DYN_SCRAM_E_ITERS;
    return DYN_SCRAM_OK;
}

static int pred_final(const char* m, size_t n, const uint8_t* sig)
{
    const char* v;
    size_t vl;
    uint8_t got[64];
    if (n == 0 || n > DYN_SCRAM_MAX_MSG)
        return DYN_SCRAM_E_SYNTAX;
    if (fld(m, n, 'e', &vl))
        return DYN_SCRAM_E_SERVER;
    if (!(v = fld(m, n, 'v', &vl)) || vl > 64)
        return DYN_SCRAM_E_SYNTAX;
    if (mdec(v, vl, got) != DYN_SCRAM_KEY_LEN || memcmp(got, sig, DYN_SCRAM_KEY_LEN))
        return DYN_SCRAM_E_VERIFY;
    return DYN_SCRAM_OK;
}

static size_t b64(const uint8_t* p, size_t n, char* out, int strip)
{
    size_t k = dyn_codec_base64_encode(p, n, out);
    while (strip && k && out[k - 1] == '=')
        k--;
    out[k] = 0;
    return k;
}

static void model(const char* nonce, const char* m, size_t n, const sf_t* f,
    const char* pw, const char* cbc, buf_t* want, uint8_t* sig)
{
    uint8_t salted[32], ckey[32], stored[32], csig[32], skey[32], proof[32], chk[32];
    char pb[64];
    buf_t am;
    size_t wl;
    int i;
    am.p = (char*)malloc(BIG);
    am.n = 0;
    puts_(&am, "n=,r=");
    puts_(&am, nonce);
    puts_(&am, ",");
    put(&am, m, n);
    puts_(&am, ",");
    wl = am.n;
    puts_(&am, "c=");
    puts_(&am, cbc);
    puts_(&am, ",r=");
    put(&am, f->rv, f->rl);
    want->n = 0;
    put(want, am.p + wl, am.n - wl);
    dyn_pbkdf2(sha, (const uint8_t*)pw, strlen(pw), f->salt, f->sl, f->it, salted, 32);
    dyn_hmac(sha, salted, 32, (const uint8_t*)"Client Key", 10, ckey);
    dyn_sha256(ckey, 32, stored);
    dyn_hmac(sha, stored, 32, (const uint8_t*)am.p, am.n, csig);
    for (i = 0; i < 32; i++)
        proof[i] = (uint8_t)(ckey[i] ^ csig[i]);
    for (i = 0; i < 32; i++)
        chk[i] = (uint8_t)(proof[i] ^ csig[i]);
    dyn_sha256(chk, 32, chk);
    if (memcmp(chk, stored, 32))
        die("model proof does not verify");
    dyn_hmac(sha, salted, 32, (const uint8_t*)"Server Key", 10, skey);
    dyn_hmac(sha, skey, 32, (const uint8_t*)am.p, am.n, sig);
    puts_(want, ",p=");
    put(want, pb, b64(proof, 32, pb, 0));
    free(am.p);
}

static void subst(buf_t* o, const uint8_t* p, size_t n, uint8_t ph, const char* with)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (p[i] == ph)
            puts_(o, with);
        else
            put(o, p + i, 1);
    }
}

static void gen_text(in_t* in, buf_t* o, size_t len, int nocomma)
{
    size_t i;
    for (i = 0; i < len; i++) {
        uint8_t c = take(in);
        c = (uint8_t)(0x21 + c % 94);
        if (nocomma && c == ',')
            c = '~';
        put(o, &c, 1);
    }
}

static const char* const ITERS[] = { "0", "1000001", "999999999", "1234567890", "12a", "",
    "+5", " 5", "0000000016", "000000016", "1000000", "-1" };
static const char* const SALTS[] = { "", "!!!!", "QQ=", "Q", "QUJD===", "QU=J", "Q Q=", "////" };

static void gen_first(in_t* in, const char* nonce, buf_t* m, char* pw)
{
    uint8_t b1 = take(in), b2 = take(in), b6 = take(in), raw[64];
    size_t pl = take(in) % 32, sl = 1 + take(in) % 64, xl = 1 + take(in) % 32, i;
    unsigned fault = b1 >> 4, ord = (b6 >> 2) & 3;
    char sb[128], ib[16], rb[160], tmp[64];
    buf_t r = { rb, 0 };
    for (i = 0; i < pl; i++) {
        uint8_t c = take(in);
        pw[i] = (char)(c ? c : 'a');
    }
    pw[pl] = 0;
    for (i = 0; i < sl; i++)
        raw[i] = take(in);
    b64(raw, sl, sb, b6 & 1);
    snprintf(ib, sizeof ib, "%u", 1u + (b1 & 15));
    puts_(&r, nonce);
    if (fault == 8)
        rb[b2 % DYN_SCRAM_NONCE_B64] ^= 1;
    if (fault != 9) {
        buf_t t = { tmp, 0 };
        gen_text(in, &t, xl, 1);
        put(&r, tmp, t.n);
    }
    rb[r.n] = 0;
    if (fault == 10)
        snprintf(ib, sizeof ib, "%s", ITERS[b2 % (sizeof ITERS / sizeof *ITERS)]);
    if (fault == 11)
        snprintf(sb, sizeof sb, "%s", SALTS[b2 % (sizeof SALTS / sizeof *SALTS)]);
    if (fault == 13)
        puts_(m, "e=other-error,");
    if (fault == 14)
        puts_(m, b2 & 1 ? "m=ext," : "m=,");
    for (i = 0; i < 3; i++) {
        unsigned k = (i + ord) % 3;
        if (fault == 12 && k == b2 % 3)
            continue;
        if (m->n && m->p[m->n - 1] != ',')
            puts_(m, ",");
        puts_(m, k == 0 ? "r=" : k == 1 ? "s=" : "i=");
        puts_(m, k == 0 ? rb : k == 1 ? sb : ib);
    }
    if (b6 & 16) {
        puts_(m, ",x=");
        gen_text(in, m, take(in) % 16, 1);
    }
    if (fault == 15 && m->n)
        m->p[take(in) % m->n] = 0;
}

static void gen_final(in_t* in, const uint8_t* sig, buf_t* f)
{
    uint8_t mode = take(in), s2[32];
    char vb[64];
    memcpy(s2, sig, 32);
    switch (mode & 7) {
    case 2:
        s2[take(in) % 32] ^= (uint8_t)(1u << (take(in) & 7));
        break;
    case 3:
        s2[31] ^= 0x80;
        break;
    }
    b64(s2, (mode & 7) == 4 ? 31 : 32, vb, mode & 8);
    if ((mode & 7) == 5) {
        puts_(f, "e=");
        gen_text(in, f, take(in), 0);
        return;
    }
    if (mode & 16) {
        gen_text(in, f, take(in) % 12, 0);
        puts_(f, ",");
    }
    if ((mode & 7) == 6) {
        size_t n = take(in) % 24;
        while (n-- && in->n)
            put(f, in->p, 1), take(in);
        return;
    }
    puts_(f, "v=");
    puts_(f, vb);
    if (mode & 32)
        puts_(f, ",x=1");
}

static void run(const uint8_t* data, size_t size)
{
    in_t in = { data, size };
    dyn_scram_t s;
    char cf[64], pw[40] = "pencil", *out = (char*)malloc(BIG);
    char nonce[DYN_SCRAM_NONCE_B64 + 1];
    uint8_t sel = take(&in), sig[32];
    buf_t m = { (char*)malloc(BIG), 0 }, fin = { (char*)malloc(BIG), 0 }, want = { (char*)malloc(BIG), 0 };
    sf_t* sf = (sf_t*)malloc(sizeof *sf);
    int rc, pr, i, cbm;
    size_t cap, need, cbl, gl;
    uint8_t cb[DYN_SCRAM_MAX_CBIND], cbraw[32 + DYN_SCRAM_MAX_CBIND];
    char cbc[2 * (32 + DYN_SCRAM_MAX_CBIND)];
    const char* gs2;

    memset(&s, 0, sizeof s);
    cbm = (sel >> 2) % 3;
    cbl = cbm == 2 ? 1 + take(&in) % DYN_SCRAM_MAX_CBIND : 0;
    for (i = 0; i < (int)cbl; i++)
        cb[i] = take(&in);
    gs2 = cbm == 0 ? "n,," : cbm == 1 ? "y,," : "p=tls-server-end-point,,";
    gl = strlen(gs2);
    memcpy(cbraw, gs2, gl);
    memcpy(cbraw + gl, cb, cbl);
    b64(cbraw, gl + cbl, cbc, 0);
    rc = dyn_scram_client_first_cb(&s, cbm == 0 ? DYN_SCRAM_CB_NONE : cbm == 1 ? DYN_SCRAM_CB_CLIENT : DYN_SCRAM_CB_TLS_EP, cb, cbl, cf, sizeof cf);
    if (rc != (int)(gl + 5 + DYN_SCRAM_NONCE_B64) || memcmp(cf, gs2, gl) || memcmp(cf + gl, "n=,r=", 5) || (int)strlen(cf) != rc)
        die("client-first is not <gs2>n=,r=<24 b64>");
    for (i = 0; i < DYN_SCRAM_NONCE_B64; i++)
        if (!strchr("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/", s.nonce[i]))
            die("client nonce has a non-base64 char");
    if (strcmp(cf + gl + 5, s.nonce))
        die("client-first nonce differs from state nonce");
    memcpy(nonce, s.nonce, sizeof nonce);

    if ((sel & 3) == 3) {
        const uint8_t* nl = (const uint8_t*)memchr(in.p, '\n', in.n);
        size_t a = nl ? (size_t)(nl - in.p) : in.n;
        subst(&m, in.p, a, 1, nonce);
        in.p += a + (nl != NULL);
        in.n -= a + (nl != NULL);
    } else {
        gen_first(&in, nonce, &m, pw);
    }

    pr = pred_first(nonce, m.p, m.n, sf);
    if (pr == DYN_SCRAM_OK && sf->it > MAXIT)
        goto out;
    if (pr != DYN_SCRAM_OK) {
        rc = dyn_scram_server_first(&s, m.p, m.n, pw, out, BIG);
        if (rc != pr) {
            fprintf(stderr, "server-first rc %d, model %d\n", rc, pr);
            die("server-first error code differs from model");
        }
        if (rc == DYN_SCRAM_E_SERVER) {
            size_t el;
            const char* e = fld(m.p, m.n, 'e', &el);
            if (el > sizeof s.err - 1)
                el = sizeof s.err - 1;
            if (memcmp(s.err, e, el) || s.err[el])
                die("server-first error text not carried");
        }
        if (dyn_scram_server_final(&s, "v=AAAA", 6) != DYN_SCRAM_E_STATE)
            die("server-final accepted after a failed server-first");
        goto out;
    }
    model(nonce, m.p, m.n, sf, pw, cbc, &want, sig);
    need = want.n;
    switch (take(&in) & 3) {
    case 0:
        cap = need;
        break;
    case 1:
        cap = take(&in) % (need + 1);
        break;
    case 2:
        cap = need + 1;
        break;
    default:
        cap = BIG;
    }
    rc = dyn_scram_server_first(&s, m.p, m.n, pw, out, cap);
    if (cap <= need) {
        if (rc != DYN_SCRAM_E_SHORT)
            die("short client-final buffer not refused");
        rc = dyn_scram_server_first(&s, m.p, m.n, pw, out, need + 1);
    }
    if (rc < 0 || (size_t)rc != need || strlen(out) != need || memcmp(out, want.p, need)) {
        fprintf(stderr, "rc %d need %zu\ngot  %.*s\nwant %.*s\n", rc, need, rc > 0 ? rc : 0, out, (int)need, want.p);
        die("client-final differs from RFC 5802 model");
    }
    if (dyn_scram_server_first(&s, m.p, m.n, pw, out, BIG) != DYN_SCRAM_E_STATE)
        die("server-first accepted twice");

    if ((sel & 3) == 3) {
        char vb[64];
        b64(sig, 32, vb, 0);
        subst(&fin, in.p, in.n, 2, vb);
    } else {
        gen_final(&in, sig, &fin);
    }
    pr = pred_final(fin.p, fin.n, sig);
    rc = dyn_scram_server_final(&s, fin.p, fin.n);
    if (rc != pr) {
        fprintf(stderr, "server-final rc %d model %d: %.*s\n", rc, pr, (int)fin.n, fin.p);
        die("server-final verdict differs from model");
    }
    if (rc == DYN_SCRAM_OK && dyn_scram_server_final(&s, fin.p, fin.n) != DYN_SCRAM_E_STATE)
        die("server-final accepted twice");
    if (rc == DYN_SCRAM_E_SERVER) {
        size_t el;
        const char* e = fld(fin.p, fin.n, 'e', &el);
        if (el > sizeof s.err - 1)
            el = sizeof s.err - 1;
        if (memcmp(s.err, e, el) || s.err[el])
            die("server error text not carried");
    }
out:
    dyn_scram_free(&s);
    if (s.cfirst_bare || s.sfirst)
        die("free left parts");
    for (i = 0; i < 32; i++)
        if (s.server_sig[i])
            die("free did not wipe the server signature");
    free(sf);
    free(want.p);
    free(fin.p);
    free(m.p);
    free(out);
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    size_t before;
    if (!inited) {
        simd_init();
        sha = dyn_hash_algo_by_name("sha256");
        inited = 1;
    }
    if (size > 4096)
        return 0;
    before = __sanitizer_get_current_allocated_bytes();
    run(data, size);
    if (__sanitizer_get_current_allocated_bytes() != before)
        die("scram exchange leaked memory");
    return 0;
}
