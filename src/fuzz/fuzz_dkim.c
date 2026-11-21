#include "core/dyn-dkim.h"
#include "core/dyn-hash.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

static const char* const heads[] = { "from", "to", "subject", "x-a" };

static void die(const char* why)
{
    fprintf(stderr, "fuzz_dkim oracle: %s\n", why);
    abort();
}

static int wsp(uint8_t c)
{
    return c == ' ' || c == '\t';
}

static int sb_eq(const dyn_sb_t* a, const dyn_sb_t* b)
{
    return a->n == b->n && (!a->n || !memcmp(a->p, b->p, a->n));
}

static void model_body(const uint8_t* p, size_t n, int relaxed, dyn_sb_t* o)
{
    dyn_sb_t t;
    size_t i = 0, keep = 0;
    dyn_sb_init(&t);
    while (i < n) {
        size_t s = i, j, b = t.n;
        while (i < n && p[i] != '\r' && p[i] != '\n')
            i++;
        if (relaxed) {
            for (j = s; j < i; j++) {
                if (!wsp(p[j]))
                    dyn_sb_putc(&t, (char)p[j]);
                else if (j == s || !wsp(p[j - 1]))
                    dyn_sb_putc(&t, ' ');
            }
            while (t.n > b && t.p[t.n - 1] == ' ')
                t.n--;
        } else
            dyn_sb_put(&t, (const char*)p + s, i - s);
        dyn_sb_put(&t, "\r\n", 2);
        if (t.n - b > 2)
            keep = t.n;
        if (i < n)
            i += p[i] == '\r' && i + 1 < n && p[i + 1] == '\n' ? 2 : 1;
    }
    dyn_sb_put(o, t.p ? t.p : "", keep);
    if (!relaxed && !o->n)
        dyn_sb_put(o, "\r\n", 2);
    dyn_sb_free(&t);
}

static void model_hdr(const uint8_t* f, size_t n, size_t nlen, int relaxed, dyn_sb_t* o)
{
    size_t i, st;
    int pend = 0;
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
    for (i = nlen; i < n && f[i] != ':'; i++)
        ;
    st = o->n;
    for (i++; i < n; i++) {
        if (f[i] == '\r' || f[i] == '\n')
            continue;
        if (wsp(f[i])) {
            pend = 1;
            continue;
        }
        if (pend && o->n > st)
            dyn_sb_putc(o, ' ');
        pend = 0;
        dyn_sb_putc(o, (char)f[i]);
    }
}

static void check_body(const uint8_t* p, size_t n, unsigned fl, unsigned split)
{
    dkim_body_t b1, b2;
    dyn_sb_t m;
    uint8_t h1[32], h2[32], hm[32];
    uint64_t lim = (fl & 2) ? DKIM_NO_LIMIT : (uint64_t)(fl >> 2) * 5u;
    size_t at, take;
    int rel = (int)(fl & 1);
    dkim_body_init(&b1, rel, lim);
    dkim_body_feed(&b1, p, n);
    dkim_body_final(&b1, h1);
    dkim_body_init(&b2, rel, lim);
    for (at = 0; at < n;) {
        size_t k = 1 + (split * 17u + at * 3u) % (split & 63u ? split & 63u : 5u);
        if (k > n - at)
            k = n - at;
        dkim_body_feed(&b2, p + at, k);
        at += k;
    }
    dkim_body_final(&b2, h2);
    if (memcmp(h1, h2, 32) || b1.n != b2.n)
        die("body hash differs between whole and split feeds");
    dyn_sb_init(&m);
    model_body(p, n, rel, &m);
    take = lim < m.n ? (size_t)lim : m.n;
    dyn_sha256((const uint8_t*)(m.p ? m.p : ""), take, hm);
    if (b1.n != m.n || memcmp(h1, hm, 32))
        die("body canonicalisation differs from the RFC 6376 3.4 model");
    dyn_sb_free(&m);
}

static void check_hdrs(const dkim_msg_t* m, const uint8_t* p, size_t n)
{
    size_t i;
    dyn_sb_t a, b, c;
    dyn_sb_init(&a);
    dyn_sb_init(&b);
    dyn_sb_init(&c);
    for (i = 0; i < m->nf && i < 64; i++) {
        const dkim_field_t* f = &m->f[i];
        int rel;
        if (f->off + f->len > n || f->nlen > f->len || m->body > n)
            die("message field outside the input");
        for (rel = 0; rel < 2; rel++) {
            a.n = b.n = c.n = 0;
            dkim_hdr_canon(&a, p + f->off, f->len, f->nlen, rel);
            model_hdr(p + f->off, f->len, f->nlen, rel, &b);
            if (!sb_eq(&a, &b))
                die("header canonicalisation differs from the RFC 6376 3.4 model");
            if (rel) {
                size_t nl = 0;
                while (nl < a.n && a.p[nl] != ':')
                    nl++;
                dkim_hdr_canon(&c, (const uint8_t*)a.p, a.n, nl, 1);
                if (!sb_eq(&a, &c))
                    die("relaxed header canonicalisation is not idempotent");
            }
        }
    }
    dyn_sb_free(&a);
    dyn_sb_free(&b);
    dyn_sb_free(&c);
}

static void sig_ser(const dkim_sig_t* s, dyn_sb_t* o, int fws)
{
    const char* sep = fws ? " ;\r\n\t" : "; ";
    const char* eq = fws ? " = " : "=";
    char nb[32];
    size_t i;
    dyn_sb_t e;
    dyn_sb_init(&e);
    dyn_sb_puts(o, "v");
    dyn_sb_puts(o, eq);
    dyn_sb_puts(o, "1");
    dyn_sb_puts(o, sep);
    dyn_sb_puts(o, "a");
    dyn_sb_puts(o, eq);
    dyn_sb_puts(o, s->algo == DKIM_ED25519 ? "ed25519-sha256" : "rsa-sha256");
    dyn_sb_puts(o, sep);
    dyn_sb_puts(o, "c");
    dyn_sb_puts(o, eq);
    dyn_sb_puts(o, s->hrel ? "relaxed/" : "simple/");
    dyn_sb_puts(o, s->brel ? "relaxed" : "simple");
#define TAGV(nm, sb)                                \
    do {                                            \
        dyn_sb_puts(o, sep);                        \
        dyn_sb_puts(o, nm);                         \
        dyn_sb_puts(o, eq);                         \
        dyn_sb_put(o, (sb)->p ? (sb)->p : "", (sb)->n); \
    } while (0)
    TAGV("d", &s->d);
    TAGV("s", &s->s);
    TAGV("i", &s->i);
    dyn_sb_puts(o, sep);
    dyn_sb_puts(o, "h");
    dyn_sb_puts(o, eq);
    for (i = 0; i < s->h.n; i++)
        dyn_sb_putc(o, s->h.p[i] ? s->h.p[i] : (i + 1 < s->h.n ? ':' : 0));
    if (o->n && !o->p[o->n - 1])
        o->n--;
    dkim_b64_encode(&e, s->bh, 32);
    TAGV("bh", &e);
    e.n = 0;
    dkim_b64_encode(&e, (const uint8_t*)s->b.p, s->b.n);
    TAGV("b", &e);
    if (s->l != DKIM_NO_LIMIT) {
        snprintf(nb, sizeof nb, "%" PRIu64, s->l);
        dyn_sb_puts(o, sep);
        dyn_sb_puts(o, "l");
        dyn_sb_puts(o, eq);
        dyn_sb_puts(o, nb);
    }
    if (s->t >= 0) {
        snprintf(nb, sizeof nb, "%" PRId64, s->t);
        dyn_sb_puts(o, sep);
        dyn_sb_puts(o, "t");
        dyn_sb_puts(o, eq);
        dyn_sb_puts(o, nb);
    }
    if (s->x >= 0) {
        snprintf(nb, sizeof nb, "%" PRId64, s->x);
        dyn_sb_puts(o, sep);
        dyn_sb_puts(o, "x");
        dyn_sb_puts(o, eq);
        dyn_sb_puts(o, nb);
    }
#undef TAGV
    dyn_sb_free(&e);
}

static int sig_eq(const dkim_sig_t* a, const dkim_sig_t* b)
{
    return a->algo == b->algo && a->hrel == b->hrel && a->brel == b->brel && a->t == b->t && a->x == b->x && a->l == b->l && a->nh == b->nh && sb_eq(&a->d, &b->d)
        && sb_eq(&a->s, &b->s) && sb_eq(&a->i, &b->i) && sb_eq(&a->b, &b->b) && sb_eq(&a->h, &b->h) && !memcmp(a->bh, b->bh, 32);
}

static void check_sig(const uint8_t* p, size_t n)
{
    dkim_sig_t s, s2;
    dyn_sb_t o;
    int fws;
    size_t x, y;
    if (dkim_tag_span(p, n, 'b', &x, &y) == DKIM_OK && (x > y || y > n))
        die("tag span outside the input");
    dkim_sig_init(&s);
    if (dkim_sig_parse(&s, p, n)) {
        dkim_sig_free(&s);
        return;
    }
    if (!s.nh || !s.algo || !s.d.n || !s.s.n || !s.i.n || !s.b.n || (s.t >= 0 && s.x >= 0 && s.x < s.t))
        die("accepted signature misses a required field");
    dyn_sb_init(&o);
    for (fws = 0; fws < 2; fws++) {
        o.n = 0;
        sig_ser(&s, &o, fws);
        dkim_sig_init(&s2);
        if (dkim_sig_parse(&s2, (const uint8_t*)o.p, o.n) || !sig_eq(&s, &s2))
            die(fws ? "signature parse is not FWS-invariant" : "signature parse/serialise round trip");
        dkim_sig_free(&s2);
    }
    dyn_sb_free(&o);
    dkim_sig_free(&s);
}

static void check_key(const uint8_t* p, size_t n)
{
    dkim_key_t k, k2;
    dyn_sb_t o, e;
    int fws;
    dkim_key_init(&k);
    if (dkim_key_parse(&k, p, n)) {
        dkim_key_free(&k);
        return;
    }
    if (k.revoked && k.p.n)
        die("revoked key carries key bytes");
    dyn_sb_init(&o);
    dyn_sb_init(&e);
    dkim_b64_encode(&e, (const uint8_t*)k.p.p, k.p.n);
    for (fws = 0; fws < 2; fws++) {
        const char* sep = fws ? " ;\r\n " : "; ";
        o.n = 0;
        dyn_sb_puts(&o, fws ? "v = DKIM1" : "v=DKIM1");
        dyn_sb_puts(&o, sep);
        dyn_sb_puts(&o, k.k == DKIM_ED25519 ? "k=ed25519" : "k=rsa");
        if (k.testing || k.strict) {
            dyn_sb_puts(&o, sep);
            dyn_sb_puts(&o, k.testing && k.strict ? "t=y:s" : k.testing ? "t=y" : "t=s");
        }
        dyn_sb_puts(&o, sep);
        dyn_sb_puts(&o, fws ? "p =" : "p=");
        dyn_sb_put(&o, e.p ? e.p : "", e.n);
        dkim_key_init(&k2);
        if (dkim_key_parse(&k2, (const uint8_t*)o.p, o.n) || k2.k != k.k || k2.testing != k.testing || k2.strict != k.strict || k2.revoked != k.revoked || !sb_eq(&k.p, &k2.p))
            die(fws ? "key record parse is not FWS-invariant" : "key record parse/serialise round trip");
        dkim_key_free(&k2);
    }
    dyn_sb_free(&o);
    dyn_sb_free(&e);
    dkim_key_free(&k);
}

static void check_b64(const uint8_t* p, size_t n)
{
    dyn_sb_t a, e;
    dyn_sb_init(&a);
    dyn_sb_init(&e);
    dkim_b64_encode(&e, p, n);
    if (dkim_b64_decode((const uint8_t*)e.p, e.n, &a) != DKIM_OK || a.n != n || (n && memcmp(a.p, p, n)))
        die("b64 encode/decode identity");
    dyn_sb_free(&a);
    dyn_sb_free(&e);
}

static void sign_roundtrip(const dkim_msg_t* m, const uint8_t* data, size_t size, unsigned fl)
{
    static const uint8_t fake[3] = { 1, 2, 3 };
    dkim_sign_opts_t o;
    dkim_msg_t m2;
    dkim_sig_t s;
    dyn_sb_t f;
    uint8_t h1[32], h2[32];
    size_t vo;
    memset(&o, 0, sizeof o);
    memset(&m2, 0, sizeof m2);
    o.d = "example.com";
    o.s = "s";
    o.t = (fl & 128) ? 100 : -1;
    o.x = (fl & 128) ? 200 : -1;
    o.hrel = (int)(fl >> 2 & 1);
    o.brel = (int)(fl >> 3 & 1);
    o.use_l = (int)(fl >> 4 & 1);
    o.oversign = (int)(fl >> 5 & 1);
    if (fl & 64) {
        o.heads = heads;
        o.nheads = 4;
    }
    dyn_sb_init(&f);
    if (dkim_sign_field(&f, m, &o, h1)) {
        dyn_sb_free(&f);
        return;
    }
    dkim_fold_b64(&f, fake, 3);
    dyn_sb_put(&f, "\r\n", 2);
    dyn_sb_put(&f, (const char*)data, size);
    if (dkim_msg_parse(&m2, (const uint8_t*)f.p, f.n) == DKIM_OK && m2.nf) {
        for (vo = m2.f[0].nlen; vo < m2.f[0].len && f.p[m2.f[0].off + vo] != ':'; vo++)
            ;
        vo++;
        dkim_sig_init(&s);
        if (vo > m2.f[0].len || dkim_sig_parse(&s, (const uint8_t*)f.p + m2.f[0].off + vo, m2.f[0].len - vo)
            || dkim_hdr_hash(&m2, &s, 0, h2) != DKIM_OK || memcmp(h1, h2, 32) || s.b.n != 3)
            die("signed field does not verify against its own header hash");
        dkim_sig_free(&s);
    }
    dkim_msg_free(&m2);
    dyn_sb_free(&f);
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    dkim_msg_t m;
    dkim_sig_t s;
    uint8_t h1[32];
    size_t i;
    unsigned fl, split;
    if (size < 2 || size > 65536)
        return 0;
    fl = data[0];
    split = data[1];
    data += 2;
    size -= 2;
    check_sig(data, size);
    check_key(data, size);
    check_b64(data, size);
    check_body(data, size, fl, split);
    memset(&m, 0, sizeof m);
    if (dkim_msg_parse(&m, data, size) == DKIM_OK) {
        check_hdrs(&m, data, size);
        for (i = 0; i < m.nf; i++) {
            size_t vo;
            if (!dkim_name_is(&m, i, "dkim-signature", 14))
                continue;
            for (vo = m.f[i].nlen; vo < m.f[i].len && data[m.f[i].off + vo] != ':'; vo++)
                ;
            vo++;
            if (vo > m.f[i].len)
                continue;
            check_sig(data + m.f[i].off + vo, m.f[i].len - vo);
            dkim_sig_init(&s);
            if (!dkim_sig_parse(&s, data + m.f[i].off + vo, m.f[i].len - vo))
                dkim_hdr_hash(&m, &s, i, h1);
            dkim_sig_free(&s);
        }
        if (m.body <= size)
            check_body(data + m.body, size - m.body, fl ^ 1u, split);
        sign_roundtrip(&m, data, size, fl);
    }
    dkim_msg_free(&m);
    return 0;
}
