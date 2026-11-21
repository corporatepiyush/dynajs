#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../dyn-dkim.h"

#define TAGS_MAX 64

typedef struct {
    size_t ns, ne, vs, ve;
} tag_t;

static int fws(int c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static int alpha(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static int lc(int c)
{
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

static int tag_next(const uint8_t* v, size_t n, size_t* pos, tag_t* t)
{
    size_t i = *pos;
    while (i < n) {
        while (i < n && fws(v[i]))
            i++;
        if (i == n)
            break;
        if (v[i] == ';')
            return -1;
        if (!alpha(v[i]))
            return -1;
        t->ns = i;
        while (i < n && (alpha(v[i]) || (v[i] >= '0' && v[i] <= '9') || v[i] == '_'))
            i++;
        t->ne = i;
        while (i < n && fws(v[i]))
            i++;
        if (i == n || v[i] != '=')
            return -1;
        i++;
        while (i < n && fws(v[i]))
            i++;
        t->vs = i;
        while (i < n && v[i] != ';') {
            if (!fws(v[i]) && (v[i] < 0x21 || v[i] > 0x7e))
                return -1;
            i++;
        }
        t->ve = i;
        while (t->ve > t->vs && fws(v[t->ve - 1]))
            t->ve--;
        if (i < n)
            i++;
        *pos = i;
        return 1;
    }
    *pos = n;
    return 0;
}

static int tag_is(const uint8_t* v, const tag_t* t, const char* name)
{
    size_t k = strlen(name);
    return t->ne - t->ns == k && memcmp(v + t->ns, name, k) == 0;
}

static int val_is(const uint8_t* v, const tag_t* t, const char* s)
{
    size_t k = strlen(s), i;
    if (t->ve - t->vs != k)
        return 0;
    for (i = 0; i < k; i++)
        if (lc(v[t->vs + i]) != lc((uint8_t)s[i]))
            return 0;
    return 1;
}

static int tag_list(const uint8_t* v, size_t n, tag_t* tags, size_t* nt)
{
    size_t pos = 0, k = 0, j;
    tag_t cur;
    int r;
    while ((r = tag_next(v, n, &pos, &cur)) == 1) {
        for (j = 0; j < k; j++)
            if (tags[j].ne - tags[j].ns == cur.ne - cur.ns
                && memcmp(v + tags[j].ns, v + cur.ns, cur.ne - cur.ns) == 0)
                return -2;
        if (k == TAGS_MAX)
            return -3;
        tags[k++] = cur;
    }
    *nt = k;
    return r;
}

int dkim_tag_span(const uint8_t* v, size_t n, char tag, size_t* a, size_t* z)
{
    size_t pos = 0, eq;
    tag_t t;
    int r;
    while ((r = tag_next(v, n, &pos, &t)) == 1)
        if (t.ne - t.ns == 1 && v[t.ns] == (uint8_t)tag) {
            for (eq = t.ne; v[eq] != '='; eq++)
                ;
            *a = eq + 1;
            for (*z = t.ve; *z < n && v[*z] != ';'; (*z)++)
                ;
            return DKIM_OK;
        }
    return DKIM_EBAD;
}

static void put_strip(dyn_sb_t* o, const uint8_t* v, const tag_t* t)
{
    size_t i;
    for (i = t->vs; i < t->ve; i++)
        if (!fws(v[i]))
            dyn_sb_putc(o, (char)v[i]);
}

static int put_num(const uint8_t* v, const tag_t* t, uint64_t* out)
{
    uint64_t x = 0;
    size_t i;
    if (t->ve == t->vs || t->ve - t->vs > 20)
        return -1;
    for (i = t->vs; i < t->ve; i++) {
        unsigned d;
        if (v[i] < '0' || v[i] > '9')
            return -1;
        d = (unsigned)(v[i] - '0');
        if (x > (UINT64_MAX - d) / 10)
            return -1;
        x = x * 10 + d;
    }
    *out = x;
    return 0;
}

static int list_has(const uint8_t* v, const tag_t* t, const char* want)
{
    size_t i = t->vs, k = strlen(want);
    while (i <= t->ve) {
        size_t s, e;
        while (i < t->ve && fws(v[i]))
            i++;
        s = i;
        while (i < t->ve && v[i] != ':')
            i++;
        e = i;
        while (e > s && fws(v[e - 1]))
            e--;
        if (e - s == k) {
            size_t j = 0;
            while (j < k && lc(v[s + j]) == lc((uint8_t)want[j]))
                j++;
            if (j == k)
                return 1;
        }
        i++;
    }
    return 0;
}

static int dom_eq(const char* a, size_t an, const char* b, size_t bn)
{
    size_t i;
    if (an != bn)
        return 0;
    for (i = 0; i < an; i++)
        if (lc((uint8_t)a[i]) != lc((uint8_t)b[i]))
            return 0;
    return 1;
}

static int dom_ok(const char* p, size_t n)
{
    size_t i, lab = 0;
    for (i = 0; i < n; i++) {
        uint8_t c = (uint8_t)p[i];
        if (c == '.') {
            if (!lab)
                return 0;
            lab = 0;
        } else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c >= 0x80)
            lab++;
        else
            return 0;
    }
    return lab > 0;
}

static const char* last_at(const dyn_sb_t* b)
{
    size_t k = b->n;
    while (k-- > 0)
        if (b->p[k] == '@')
            return b->p + k;
    return NULL;
}

static int ident_ok(const dkim_sig_t* s)
{
    const char* at = last_at(&s->i);
    size_t dn;
    const char* dp;
    if (!at)
        return 0;
    dp = at + 1;
    dn = s->i.n - (size_t)(dp - s->i.p);
    if (dom_eq(dp, dn, s->d.p, s->d.n))
        return 1;
    return dn > s->d.n && dp[dn - s->d.n - 1] == '.' && dom_eq(dp + dn - s->d.n, s->d.n, s->d.p, s->d.n);
}

void dkim_sig_init(dkim_sig_t* s)
{
    memset(s, 0, sizeof *s);
    s->t = -1;
    s->x = -1;
    s->l = DKIM_NO_LIMIT;
}

void dkim_sig_free(dkim_sig_t* s)
{
    dyn_sb_free(&s->d);
    dyn_sb_free(&s->s);
    dyn_sb_free(&s->i);
    dyn_sb_free(&s->b);
    dyn_sb_free(&s->h);
}

static const char* canon_parse(dkim_sig_t* s, const uint8_t* v, const tag_t* t)
{
    size_t i, sl = t->ve;
    tag_t a = *t, b = *t;
    for (i = t->vs; i < t->ve; i++)
        if (v[i] == '/')
            sl = i;
    a.ve = sl;
    if (val_is(v, &a, "relaxed"))
        s->hrel = 1;
    else if (!val_is(v, &a, "simple"))
        return "unknown header canonicalization";
    if (sl == t->ve)
        return NULL;
    b.vs = sl + 1;
    if (val_is(v, &b, "relaxed"))
        s->brel = 1;
    else if (!val_is(v, &b, "simple"))
        return "unknown body canonicalization";
    return NULL;
}

static const char* h_parse(dkim_sig_t* s, const uint8_t* v, const tag_t* t)
{
    size_t i = t->vs;
    int from = 0;
    while (i <= t->ve) {
        size_t a, e, k;
        while (i < t->ve && fws(v[i]))
            i++;
        a = i;
        while (i < t->ve && v[i] != ':')
            i++;
        e = i;
        while (e > a && fws(v[e - 1]))
            e--;
        if (e == a)
            return "empty h= entry";
        for (k = a; k < e; k++)
            if (v[k] <= 0x20 || v[k] > 0x7e)
                return "bad h= entry";
        if (++s->nh > DKIM_H_MAX)
            return "too many h= entries";
        if (e - a == 4 && lc(v[a]) == 'f' && lc(v[a + 1]) == 'r' && lc(v[a + 2]) == 'o' && lc(v[a + 3]) == 'm')
            from = 1;
        for (k = a; k < e; k++)
            dyn_sb_putc(&s->h, (char)lc(v[k]));
        dyn_sb_putc(&s->h, '\0');
        i++;
    }
    return from ? NULL : "h= does not include from";
}

const char* dkim_sig_parse(dkim_sig_t* s, const uint8_t* v, size_t n)
{
    tag_t tags[TAGS_MAX];
    size_t nt = 0, k;
    unsigned seen = 0;
    const char* err;
    uint64_t num;
    dyn_sb_t tmp;
    int r = tag_list(v, n, tags, &nt);
    if (r == -2)
        return "duplicate tag";
    if (r == -3)
        return "too many tags";
    if (r < 0)
        return "bad tag-list syntax";
    for (k = 0; k < nt; k++) {
        const tag_t* t = &tags[k];
        if (tag_is(v, t, "v")) {
            seen |= 1;
            if (!val_is(v, t, "1"))
                return "unsupported v=";
        } else if (tag_is(v, t, "a")) {
            seen |= 2;
            if (val_is(v, t, "rsa-sha256"))
                s->algo = DKIM_RSA;
            else if (val_is(v, t, "ed25519-sha256"))
                s->algo = DKIM_ED25519;
            else if (val_is(v, t, "rsa-sha1"))
                return "rsa-sha1 is not accepted (RFC 8301)";
            else
                return "unsupported algorithm";
        } else if (tag_is(v, t, "b")) {
            seen |= 4;
            if (dkim_b64_decode(v + t->vs, t->ve - t->vs, &s->b) != DKIM_OK || s->b.n == 0)
                return "bad b=";
        } else if (tag_is(v, t, "bh")) {
            seen |= 8;
            dyn_sb_init(&tmp);
            r = dkim_b64_decode(v + t->vs, t->ve - t->vs, &tmp);
            if (r == DKIM_OK && tmp.n == 32)
                memcpy(s->bh, tmp.p, 32);
            num = tmp.n;
            dyn_sb_free(&tmp);
            if (r != DKIM_OK || num != 32)
                return "bad bh=";
        } else if (tag_is(v, t, "c")) {
            if ((err = canon_parse(s, v, t)))
                return err;
        } else if (tag_is(v, t, "d")) {
            seen |= 16;
            put_strip(&s->d, v, t);
            if (!s->d.n)
                return "empty d=";
            if (!dom_ok(s->d.p, s->d.n))
                return "bad d= domain";
        } else if (tag_is(v, t, "h")) {
            seen |= 32;
            if ((err = h_parse(s, v, t)))
                return err;
        } else if (tag_is(v, t, "i")) {
            put_strip(&s->i, v, t);
        } else if (tag_is(v, t, "l")) {
            if (put_num(v, t, &s->l) < 0 || s->l == DKIM_NO_LIMIT)
                return "bad l=";
        } else if (tag_is(v, t, "q")) {
            if (!list_has(v, t, "dns/txt"))
                return "q= does not include dns/txt";
        } else if (tag_is(v, t, "s")) {
            seen |= 64;
            put_strip(&s->s, v, t);
            if (!s->s.n)
                return "empty s=";
        } else if (tag_is(v, t, "t")) {
            if (put_num(v, t, &num) < 0 || num > INT64_MAX)
                return "bad t=";
            s->t = (int64_t)num;
        } else if (tag_is(v, t, "x")) {
            if (put_num(v, t, &num) < 0 || num > INT64_MAX)
                return "bad x=";
            s->x = (int64_t)num;
        }
    }
    if ((seen & 127) != 127)
        return "missing required tag";
    if (s->t >= 0 && s->x >= 0 && s->x < s->t)
        return "x= is before t=";
    if (!s->i.n) {
        dyn_sb_putc(&s->i, '@');
        dyn_sb_put(&s->i, s->d.p, s->d.n);
    } else if (!ident_ok(s))
        return "i= is not within d=";
    if (s->d.oom || s->s.oom || s->i.oom || s->b.oom || s->h.oom)
        return "out of memory";
    return NULL;
}

void dkim_key_init(dkim_key_t* k)
{
    memset(k, 0, sizeof *k);
    k->k = DKIM_RSA;
}

void dkim_key_free(dkim_key_t* k)
{
    dyn_sb_free(&k->p);
}

const char* dkim_key_parse(dkim_key_t* key, const uint8_t* v, size_t n)
{
    tag_t tags[TAGS_MAX];
    size_t nt = 0, k;
    int r = tag_list(v, n, tags, &nt), havep = 0;
    if (r == -2)
        return "duplicate tag in key record";
    if (r < 0)
        return "bad key record syntax";
    for (k = 0; k < nt; k++) {
        const tag_t* t = &tags[k];
        if (tag_is(v, t, "v")) {
            if (k != 0 || !(t->ve - t->vs == 5 && memcmp(v + t->vs, "DKIM1", 5) == 0))
                return "bad key v=";
        } else if (tag_is(v, t, "h")) {
            if (!list_has(v, t, "sha256"))
                return "key h= does not allow sha256";
        } else if (tag_is(v, t, "k")) {
            if (val_is(v, t, "rsa"))
                key->k = DKIM_RSA;
            else if (val_is(v, t, "ed25519"))
                key->k = DKIM_ED25519;
            else
                return "unsupported key type";
        } else if (tag_is(v, t, "s")) {
            if (!list_has(v, t, "*") && !list_has(v, t, "email"))
                return "key service type excludes email";
        } else if (tag_is(v, t, "t")) {
            key->testing = list_has(v, t, "y");
            key->strict = list_has(v, t, "s");
        } else if (tag_is(v, t, "p")) {
            havep = 1;
            if (t->ve == t->vs)
                key->revoked = 1;
            else if (dkim_b64_decode(v + t->vs, t->ve - t->vs, &key->p) != DKIM_OK)
                return "bad key p=";
        }
    }
    if (!havep)
        return "key record has no p=";
    return key->p.oom ? "out of memory" : NULL;
}

static void fold_put(dyn_sb_t* o, size_t* col, const char* s, size_t n)
{
    if (*col > 1 && *col + n > 76) {
        if (n && s[0] == ' ') {
            dyn_sb_put(o, "\r\n", 2);
            *col = 0;
        } else {
            dyn_sb_put(o, "\r\n ", 3);
            *col = 1;
        }
    }
    dyn_sb_put(o, s, n);
    *col += n;
}

static void fold_tag(dyn_sb_t* o, size_t* col, const char* name, const char* val)
{
    char buf[1024];
    int k = snprintf(buf, sizeof buf, " %s=%s;", name, val);
    if (k > 0)
        fold_put(o, col, buf, (size_t)k < sizeof buf ? (size_t)k : sizeof buf - 1);
}

static const char* const def_heads[] = {
    "from", "to", "cc", "subject", "date", "message-id", "reply-to", "in-reply-to",
    "references", "mime-version", "content-type", "content-transfer-encoding", "sender",
};

static size_t count_name(const dkim_msg_t* m, const char* nm)
{
    size_t c = 0, j, nn = strlen(nm);
    for (j = 0; j < m->nf; j++)
        c += (size_t)dkim_name_is(m, j, nm, nn);
    return c;
}

static int ieq(const char* a, const char* b)
{
    while (*a && lc((uint8_t)*a) == lc((uint8_t)*b))
        a++, b++;
    return !*a && !*b;
}

static int val_ok(const char* s)
{
    size_t n = 0;
    if (!s || !*s)
        return 0;
    for (; *s; s++, n++)
        if ((uint8_t)*s <= 0x20 || (uint8_t)*s > 0x7e || *s == ';' || n > 255)
            return 0;
    return 1;
}

static int name_ok(const char* s)
{
    size_t n = 0;
    if (!*s)
        return 0;
    for (; *s; s++)
        if (++n > 200 || (uint8_t)*s <= 0x20 || (uint8_t)*s > 0x7e || *s == ':' || *s == ';')
            return 0;
    return 1;
}

const char* dkim_sign_field(dyn_sb_t* out, const dkim_msg_t* m, const dkim_sign_opts_t* o, uint8_t hh[32])
{
    dkim_body_t* body;
    dkim_sig_t sig;
    const char* const* hs = o->heads ? o->heads : def_heads;
    size_t nh = o->heads ? o->nheads : sizeof def_heads / sizeof def_heads[0];
    size_t col = 0, k, c, total = 0, start = out->n;
    uint8_t bh[32];
    char num[32];
    dyn_sb_t b64;
    const char* err = NULL;
    int from = 0;
    if (!val_ok(o->d) || !dom_ok(o->d, strlen(o->d)) || !val_ok(o->s) || (o->i && !val_ok(o->i)))
        return "bad domain, selector or identity";
    if (!count_name(m, "from"))
        return "message has no From header";
    for (k = 0; k < nh; k++) {
        if (!name_ok(hs[k]))
            return "bad header name";
        from |= ieq(hs[k], "from");
    }
    if (!from)
        return "headers must include from";
    body = (dkim_body_t*)malloc(sizeof *body);
    if (!body)
        return "out of memory";
    dkim_body_init(body, o->brel, DKIM_NO_LIMIT);
    dkim_body_feed(body, m->p + m->body, m->n - m->body);
    dkim_body_final(body, bh);
    total = (size_t)body->n;
    free(body);
    dkim_sig_init(&sig);
    sig.hrel = o->hrel;
    fold_put(out, &col, "DKIM-Signature: v=1;", 20);
    fold_tag(out, &col, "a", o->algo == DKIM_ED25519 ? "ed25519-sha256" : "rsa-sha256");
    fold_tag(out, &col, "c", o->hrel ? (o->brel ? "relaxed/relaxed" : "relaxed/simple") : (o->brel ? "simple/relaxed" : "simple/simple"));
    fold_tag(out, &col, "d", o->d);
    fold_tag(out, &col, "s", o->s);
    if (o->i)
        fold_tag(out, &col, "i", o->i);
    if (o->t >= 0) {
        snprintf(num, sizeof num, "%lld", (long long)o->t);
        fold_tag(out, &col, "t", num);
    }
    if (o->x >= 0) {
        snprintf(num, sizeof num, "%lld", (long long)o->x);
        fold_tag(out, &col, "x", num);
    }
    if (o->use_l) {
        snprintf(num, sizeof num, "%llu", (unsigned long long)total);
        fold_tag(out, &col, "l", num);
    }
    fold_put(out, &col, " h=", 3);
    for (k = 0; k < nh; k++) {
        size_t nn = strlen(hs[k]);
        c = count_name(m, hs[k]) + (size_t)(o->oversign != 0);
        while (c-- > 0) {
            char nb[256];
            size_t w = nn < sizeof nb - 1 ? nn : sizeof nb - 2;
            if (sig.nh == DKIM_H_MAX) {
                err = "too many signed headers";
                goto done;
            }
            memcpy(nb, hs[k], w);
            nb[w] = ':';
            fold_put(out, &col, nb, w + 1);
            for (size_t j = 0; j < nn; j++)
                dyn_sb_putc(&sig.h, (char)lc((uint8_t)hs[k][j]));
            dyn_sb_putc(&sig.h, '\0');
            sig.nh++;
        }
    }
    if (out->oom) {
        err = "out of memory";
        goto done;
    }
    out->p[out->n - 1] = ';';
    dyn_sb_init(&b64);
    dkim_b64_encode(&b64, bh, 32);
    dyn_sb_putc(&b64, '\0');
    if (!b64.oom)
        fold_tag(out, &col, "bh", b64.p);
    dyn_sb_free(&b64);
    fold_put(out, &col, " b=", 3);
    if (out->oom) {
        err = "out of memory";
        goto done;
    }
    if (dkim_hdr_hash_raw(m, &sig, (const uint8_t*)out->p + start, out->n - start, 14, hh) != DKIM_OK)
        err = "out of memory";
done:
    dkim_sig_free(&sig);
    return err;
}

int dkim_fold_b64(dyn_sb_t* out, const uint8_t* p, size_t n)
{
    dyn_sb_t b;
    size_t col = 0, i, k;
    for (i = out->n; i > 0 && out->p[i - 1] != '\n'; i--)
        col++;
    dyn_sb_init(&b);
    dkim_b64_encode(&b, p, n);
    for (i = 0; i < b.n; i += k) {
        if (col >= 78) {
            dyn_sb_put(out, "\r\n ", 3);
            col = 1;
        }
        k = 78 - col < b.n - i ? 78 - col : b.n - i;
        dyn_sb_put(out, b.p + i, k);
        col += k;
    }
    k = b.oom || out->oom;
    dyn_sb_free(&b);
    return k ? DKIM_ENOMEM : DKIM_OK;
}
