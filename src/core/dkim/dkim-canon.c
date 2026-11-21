#include <stdlib.h>
#include <string.h>
#include "../dyn-dkim.h"

static int lc(int c)
{
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

static int wsp(int c)
{
    return c == ' ' || c == '\t';
}

static size_t eol_at(const uint8_t* p, size_t n, size_t i)
{
    if (p[i] == '\r')
        return i + 1 < n && p[i + 1] == '\n' ? 2 : 1;
    return p[i] == '\n' ? 1 : 0;
}

int dkim_msg_parse(dkim_msg_t* m, const uint8_t* p, size_t n)
{
    size_t i = 0;
    m->p = p;
    m->n = n;
    m->nf = 0;
    m->body = n;
    while (i < n) {
        size_t s = i, e, k;
        while (i < n && !eol_at(p, n, i))
            i++;
        e = i;
        if (i < n)
            i += eol_at(p, n, i);
        if (e == s) {
            m->body = i;
            return DKIM_OK;
        }
        if (wsp(p[s])) {
            if (!m->nf)
                return DKIM_EBAD;
            m->f[m->nf - 1].len = e - m->f[m->nf - 1].off;
            continue;
        }
        for (k = s; k < e && p[k] != ':'; k++)
            ;
        if (k == e || k == s)
            return DKIM_EBAD;
        if (m->nf == DKIM_HDR_MAX)
            return DKIM_ELONG;
        if (m->nf == m->cap) {
            size_t nc = m->cap ? m->cap * 2 : 16;
            dkim_field_t* nf;
            if (nc > DKIM_HDR_MAX)
                nc = DKIM_HDR_MAX;
            nf = (dkim_field_t*)realloc(m->f, nc * sizeof *nf);
            if (!nf)
                return DKIM_ENOMEM;
            m->f = nf;
            m->cap = nc;
        }
        while (k > s && wsp(p[k - 1]))
            k--;
        m->f[m->nf].off = s;
        m->f[m->nf].len = e - s;
        m->f[m->nf].nlen = k - s;
        m->nf++;
    }
    return DKIM_OK;
}

void dkim_msg_free(dkim_msg_t* m)
{
    free(m->f);
    m->f = NULL;
    m->cap = m->nf = 0;
}

int dkim_name_is(const dkim_msg_t* m, size_t fi, const char* name, size_t nn)
{
    const dkim_field_t* f = &m->f[fi];
    size_t i;
    if (f->nlen != nn)
        return 0;
    for (i = 0; i < nn; i++)
        if (lc(m->p[f->off + i]) != lc((uint8_t)name[i]))
            return 0;
    return 1;
}

static void body_flush(dkim_body_t* b)
{
    if (b->on)
        dyn_hash_update(&b->h, b->ob, b->on);
    b->on = 0;
}

static void body_emit(dkim_body_t* b, const uint8_t* p, size_t n)
{
    size_t k;
    if (b->n < b->limit) {
        k = b->limit - b->n < n ? (size_t)(b->limit - b->n) : n;
        if (b->on + k > sizeof b->ob)
            body_flush(b);
        if (k > sizeof b->ob)
            dyn_hash_update(&b->h, p, k);
        else {
            memcpy(b->ob + b->on, p, k);
            b->on += k;
        }
    }
    b->n += n;
}

void dkim_body_init(dkim_body_t* b, int relaxed, uint64_t limit)
{
    memset(b, 0, sizeof *b);
    b->relaxed = relaxed;
    b->limit = limit;
    dyn_hash_init(&b->h, dyn_hash_algo_by_id(DYN_HASH_SHA256));
}

static void body_eol(dkim_body_t* b)
{
    if (b->text) {
        body_emit(b, (const uint8_t*)"\r\n", 2);
        b->text = 0;
    } else
        b->blank++;
    b->wsp = 0;
}

void dkim_body_feed(dkim_body_t* b, const uint8_t* p, size_t n)
{
    size_t i = 0;
    while (i < n) {
        uint8_t c = p[i];
        size_t j;
        if (b->cr) {
            b->cr = 0;
            if (c == '\n') {
                i++;
                continue;
            }
        }
        if (c == '\r' || c == '\n') {
            body_eol(b);
            b->cr = c == '\r';
            i++;
            continue;
        }
        if (b->relaxed && wsp(c)) {
            b->wsp = 1;
            i++;
            continue;
        }
        if (!b->text)
            for (; b->blank; b->blank--)
                body_emit(b, (const uint8_t*)"\r\n", 2);
        if (b->wsp) {
            body_emit(b, (const uint8_t*)" ", 1);
            b->wsp = 0;
        }
        for (j = i; j < n && p[j] != '\r' && p[j] != '\n' && !(b->relaxed && wsp(p[j])); j++)
            ;
        body_emit(b, p + i, j - i);
        b->text = 1;
        i = j;
    }
}

void dkim_body_final(dkim_body_t* b, uint8_t out[32])
{
    if (b->text)
        body_eol(b);
    if (!b->relaxed && b->n == 0)
        body_emit(b, (const uint8_t*)"\r\n", 2);
    body_flush(b);
    dyn_hash_final(&b->h, out);
}

int dkim_hdr_canon(dyn_sb_t* out, const uint8_t* p, size_t n, size_t nlen, int relaxed)
{
    size_t i;
    int pend = 0, started = 0;
    if (!relaxed) {
        for (i = 0; i < n; i++) {
            size_t e = eol_at(p, n, i);
            if (e) {
                dyn_sb_put(out, "\r\n", 2);
                i += e - 1;
            } else
                dyn_sb_putc(out, (char)p[i]);
        }
        return out->oom ? DKIM_ENOMEM : DKIM_OK;
    }
    for (i = 0; i < nlen; i++)
        dyn_sb_putc(out, (char)lc(p[i]));
    dyn_sb_putc(out, ':');
    for (i = nlen; i < n && p[i] != ':'; i++)
        ;
    for (i++; i < n; i++) {
        uint8_t c = p[i];
        if (c == '\r' || c == '\n')
            continue;
        if (wsp(c)) {
            pend = 1;
            continue;
        }
        if (pend && started)
            dyn_sb_putc(out, ' ');
        pend = 0;
        started = 1;
        dyn_sb_putc(out, (char)c);
    }
    return out->oom ? DKIM_ENOMEM : DKIM_OK;
}

int dkim_hdr_hash_raw(const dkim_msg_t* m, const dkim_sig_t* s, const uint8_t* f, size_t fn, size_t nlen, uint8_t out[32])
{
    dyn_sb_t c;
    dyn_hash_ctx_t h;
    unsigned char* used;
    const char* name = s->h.p;
    size_t k;
    int rc;
    used = (unsigned char*)calloc(m->nf ? m->nf : 1, 1);
    if (!used)
        return DKIM_ENOMEM;
    dyn_sb_init(&c);
    dyn_hash_init(&h, dyn_hash_algo_by_id(DYN_HASH_SHA256));
    for (k = 0; k < s->nh; k++) {
        size_t nn = strlen(name), j = m->nf;
        while (j-- > 0)
            if (!used[j] && dkim_name_is(m, j, name, nn)) {
                used[j] = 1;
                c.n = 0;
                dkim_hdr_canon(&c, m->p + m->f[j].off, m->f[j].len, m->f[j].nlen, s->hrel);
                dyn_sb_put(&c, "\r\n", 2);
                if (!c.oom)
                    dyn_hash_update(&h, (const uint8_t*)c.p, c.n);
                break;
            }
        name += nn + 1;
    }
    c.n = 0;
    dkim_hdr_canon(&c, f, fn, nlen, s->hrel);
    if (!c.oom)
        dyn_hash_update(&h, (const uint8_t*)c.p, c.n);
    dyn_hash_final(&h, out);
    rc = c.oom ? DKIM_ENOMEM : DKIM_OK;
    dyn_sb_free(&c);
    free(used);
    return rc;
}

int dkim_hdr_hash(const dkim_msg_t* m, const dkim_sig_t* s, size_t sigfield, uint8_t out[32])
{
    const dkim_field_t* sf = &m->f[sigfield];
    const uint8_t* fp = m->p + sf->off;
    dyn_sb_t raw;
    size_t a, z, vo;
    int rc;
    for (vo = sf->nlen; vo < sf->len && fp[vo] != ':'; vo++)
        ;
    vo++;
    if (vo > sf->len || dkim_tag_span(fp + vo, sf->len - vo, 'b', &a, &z) != DKIM_OK)
        return DKIM_EBAD;
    dyn_sb_init(&raw);
    dyn_sb_put(&raw, (const char*)fp, vo + a);
    dyn_sb_put(&raw, (const char*)fp + vo + z, sf->len - vo - z);
    rc = raw.oom ? DKIM_ENOMEM : dkim_hdr_hash_raw(m, s, (const uint8_t*)raw.p, raw.n, sf->nlen, out);
    dyn_sb_free(&raw);
    return rc;
}

static int b64v(uint8_t c)
{
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    return c == '+' ? 62 : c == '/' ? 63 : -1;
}

int dkim_b64_decode(const uint8_t* s, size_t n, dyn_sb_t* out)
{
    uint32_t w = 0;
    int bits = 0, pad = 0;
    size_t i, cnt = 0;
    for (i = 0; i < n; i++) {
        int v;
        if (wsp(s[i]) || s[i] == '\r' || s[i] == '\n')
            continue;
        if (s[i] == '=') {
            pad++;
            cnt++;
            continue;
        }
        if (pad || (v = b64v(s[i])) < 0)
            return DKIM_EBAD;
        cnt++;
        w = (w << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            dyn_sb_putc(out, (char)(w >> bits));
        }
    }
    if (cnt % 4 || pad > 2 || (w & ((1u << bits) - 1)))
        return DKIM_EBAD;
    return out->oom ? DKIM_ENOMEM : DKIM_OK;
}

int dkim_b64_encode(dyn_sb_t* out, const uint8_t* p, size_t n)
{
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i;
    for (i = 0; i + 2 < n; i += 3) {
        uint32_t w = (uint32_t)p[i] << 16 | (uint32_t)p[i + 1] << 8 | p[i + 2];
        char q[4] = { t[w >> 18], t[(w >> 12) & 63], t[(w >> 6) & 63], t[w & 63] };
        dyn_sb_put(out, q, 4);
    }
    if (i < n) {
        uint32_t w = (uint32_t)p[i] << 16 | (i + 1 < n ? (uint32_t)p[i + 1] << 8 : 0);
        char q[4] = { t[w >> 18], t[(w >> 12) & 63], i + 1 < n ? t[(w >> 6) & 63] : '=', '=' };
        dyn_sb_put(out, q, 4);
    }
    return out->oom ? DKIM_ENOMEM : DKIM_OK;
}
