#include <string.h>
#include "../dyn-mailk.h"
#include "../dyn-hash.h"
#include "../idna/idna-tables.h"
#include "libunicode.h"

static int b64v(uint8_t c)
{
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    if (c == '+')
        return 62;
    if (c == '/')
        return 63;
    return -1;
}

int mailk_b64_decode(const char* s, size_t n, dyn_sb_t* out)
{
    size_t i, pad = 0;
    const uint8_t* u = (const uint8_t*)s;
    if (n % 4)
        return MAILK_EBAD;
    if (n > 4 * ((MAILK_SASL_MAX + 2) / 3))
        return MAILK_ELONG;
    if (n && u[n - 1] == '=')
        pad = u[n - 2] == '=' ? 2 : 1;
    for (i = 0; i < n; i += 4) {
        int v[4], k, m = i + 4 == n ? 4 - (int)pad : 4;
        uint32_t w = 0;
        for (k = 0; k < 4; k++) {
            v[k] = k < m ? b64v(u[i + k]) : 0;
            if (v[k] < 0)
                return MAILK_EBAD;
            w = (w << 6) | (uint32_t)v[k];
        }
        if ((m == 3 && (w & 0xFF)) || (m == 2 && (w & 0xFFFF)))
            return MAILK_EBAD;
        dyn_sb_putc(out, (char)(w >> 16));
        if (m > 2)
            dyn_sb_putc(out, (char)(w >> 8));
        if (m > 3)
            dyn_sb_putc(out, (char)w);
    }
    return out->oom ? MAILK_ENOMEM : MAILK_OK;
}

int mailk_b64_encode(dyn_sb_t* out, const uint8_t* p, size_t n)
{
    static const char al[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i;
    if (n > MAILK_SASL_MAX)
        return MAILK_ELONG;
    for (i = 0; i < n; i += 3) {
        uint32_t w = (uint32_t)p[i] << 16 | (i + 1 < n ? (uint32_t)p[i + 1] << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
        char q[4];
        q[0] = al[w >> 18];
        q[1] = al[(w >> 12) & 63];
        q[2] = i + 1 < n ? al[(w >> 6) & 63] : '=';
        q[3] = i + 2 < n ? al[w & 63] : '=';
        dyn_sb_put(out, q, 4);
    }
    return out->oom ? MAILK_ENOMEM : MAILK_OK;
}

int mailk_sasl_resp_decode(const char* s, size_t n, dyn_sb_t* out)
{
    if (n == 1 && s[0] == '*')
        return MAILK_CANCEL;
    if (n == 1 && s[0] == '=')
        return MAILK_OK;
    if (n == 0)
        return MAILK_EBAD;
    return mailk_b64_decode(s, n, out);
}

int mailk_sasl_resp_encode(dyn_sb_t* out, const uint8_t* p, size_t n)
{
    if (!n) {
        dyn_sb_putc(out, '=');
        return out->oom ? MAILK_ENOMEM : MAILK_OK;
    }
    return mailk_b64_encode(out, p, n);
}

static int utf8_ok(const uint8_t* p, size_t n)
{
    size_t i = 0;
    while (i < n) {
        int k = mailk_utf8_len(p + i, n - i);
        if (k <= 0)
            return 0;
        i += (size_t)k;
    }
    return 1;
}

static uint32_t utf8_cp(const uint8_t* p, int k)
{
    uint32_t cp = k == 1 ? p[0] : k == 2 ? (p[0] & 0x1Fu) : k == 3 ? (p[0] & 0x0Fu) : (p[0] & 0x07u);
    int i;
    for (i = 1; i < k; i++)
        cp = (cp << 6) | (p[i] & 0x3Fu);
    return cp;
}

static void put_cp(dyn_sb_t* o, uint32_t c)
{
    char b[4];
    if (c < 0x80) {
        dyn_sb_putc(o, (char)c);
    } else if (c < 0x800) {
        b[0] = (char)(0xC0 | (c >> 6));
        b[1] = (char)(0x80 | (c & 0x3F));
        dyn_sb_put(o, b, 2);
    } else if (c < 0x10000) {
        b[0] = (char)(0xE0 | (c >> 12));
        b[1] = (char)(0x80 | ((c >> 6) & 0x3F));
        b[2] = (char)(0x80 | (c & 0x3F));
        dyn_sb_put(o, b, 3);
    } else {
        b[0] = (char)(0xF0 | (c >> 18));
        b[1] = (char)(0x80 | ((c >> 12) & 0x3F));
        b[2] = (char)(0x80 | ((c >> 6) & 0x3F));
        b[3] = (char)(0x80 | (c & 0x3F));
        dyn_sb_put(o, b, 4);
    }
}

static const uint32_t prep_nothing[][2] = {
    { 0x00AD, 0x00AD }, { 0x034F, 0x034F }, { 0x1806, 0x1806 }, { 0x180B, 0x180D },
    { 0x200C, 0x200D }, { 0x2060, 0x2060 }, { 0xFE00, 0xFE0F }, { 0xFEFF, 0xFEFF }
};

static const uint32_t prep_space[][2] = {
    { 0x00A0, 0x00A0 }, { 0x1680, 0x1680 }, { 0x2000, 0x200B }, { 0x202F, 0x202F },
    { 0x205F, 0x205F }, { 0x3000, 0x3000 }
};

static const uint32_t prep_banned[][2] = {
    { 0x0000, 0x001F }, { 0x007F, 0x009F }, { 0x00A0, 0x00A0 }, { 0x0340, 0x0341 },
    { 0x06DD, 0x06DD }, { 0x070F, 0x070F }, { 0x1680, 0x1680 }, { 0x180E, 0x180E },
    { 0x2000, 0x200F }, { 0x2028, 0x202F }, { 0x205F, 0x2063 }, { 0x206A, 0x206F },
    { 0x2FF0, 0x2FFB }, { 0x3000, 0x3000 }, { 0xD800, 0xF8FF }, { 0xFDD0, 0xFDEF },
    { 0xFEFF, 0xFEFF }, { 0xFFF9, 0xFFFD }, { 0x1D173, 0x1D17A }, { 0xE0001, 0xE0001 },
    { 0xE0020, 0xE007F }, { 0xF0000, 0x10FFFF }
};

static int in_tab(const uint32_t (*t)[2], int n, uint32_t c)
{
    return dyn_tab2(t, n, c) >= 0;
}

static int prep_prohibited(uint32_t c)
{
    if ((c & 0xFFFE) == 0xFFFE)
        return 1;
    return in_tab(prep_banned, (int)countof(prep_banned), c);
}

static void* prep_realloc(void* opaque, void* ptr, size_t size)
{
    (void)opaque;
    return realloc(ptr, size);
}

int mailk_saslprep(const char* s, size_t n, dyn_sb_t* out)
{
    const uint8_t* u = (const uint8_t*)s;
    uint32_t* cps;
    uint32_t* norm = NULL;
    size_t i, m = 0;
    int nl, ral = 0, lcat = 0, rc = MAILK_OK;
    for (i = 0; i < n && u[i] >= 0x20 && u[i] < 0x7F; i++)
        ;
    if (n > MAILK_PREP_MAX)
        return MAILK_ELONG;
    if (i == n) {
        dyn_sb_put(out, s, n);
        return out->oom ? MAILK_ENOMEM : MAILK_OK;
    }
    if (!utf8_ok(u, n))
        return MAILK_EBAD;
    cps = malloc((n + 1) * sizeof(uint32_t));
    if (!cps)
        return MAILK_ENOMEM;
    for (i = 0; i < n;) {
        int k = mailk_utf8_len(u + i, n - i);
        uint32_t c = utf8_cp(u + i, k);
        i += (size_t)k;
        if (in_tab(prep_space, (int)countof(prep_space), c))
            cps[m++] = 0x20;
        else if (!in_tab(prep_nothing, (int)countof(prep_nothing), c))
            cps[m++] = c;
    }
    nl = unicode_normalize(&norm, cps, (int)m, UNICODE_NFKC, NULL, prep_realloc);
    free(cps);
    if (nl < 0)
        return MAILK_ENOMEM;
    for (i = 0; i < (size_t)nl; i++) {
        int bc = dyn_idna_bidi_class(norm[i]);
        if (prep_prohibited(norm[i]))
            rc = MAILK_EBAD;
        ral |= bc == 1 || bc == 2;
        lcat |= bc == 0;
    }
    if (rc == MAILK_OK && ral) {
        int b0 = dyn_idna_bidi_class(norm[0]), b1 = dyn_idna_bidi_class(norm[nl - 1]);
        if (lcat || !(b0 == 1 || b0 == 2) || !(b1 == 1 || b1 == 2))
            rc = MAILK_EBAD;
    }
    for (i = 0; rc == MAILK_OK && i < (size_t)nl; i++)
        put_cp(out, norm[i]);
    free(norm);
    if (rc == MAILK_OK && out->oom)
        rc = MAILK_ENOMEM;
    return rc;
}

static int field_ok(const char* s, size_t n, size_t min)
{
    return n >= min && n <= MAILK_SASL_MAX && (!n || !memchr(s, 0, n)) && utf8_ok((const uint8_t*)s, n);
}

int mailk_plain_build(dyn_sb_t* out, const char* authzid, const char* authcid, const char* passwd)
{
    size_t zn = authzid ? strlen(authzid) : 0, cn = strlen(authcid), pn = strlen(passwd);
    if (!field_ok(authzid ? authzid : "", zn, 0) || !field_ok(authcid, cn, 1) || !field_ok(passwd, pn, 1))
        return MAILK_EBAD;
    dyn_sb_put(out, authzid ? authzid : "", zn);
    dyn_sb_putc(out, 0);
    dyn_sb_put(out, authcid, cn);
    dyn_sb_putc(out, 0);
    dyn_sb_put(out, passwd, pn);
    return out->oom ? MAILK_ENOMEM : MAILK_OK;
}

int mailk_plain_parse(const uint8_t* m, size_t n, mailk_plain_t* out)
{
    const uint8_t* a = memchr(m, 0, n);
    const uint8_t* b;
    if (!a)
        return MAILK_EBAD;
    b = memchr(a + 1, 0, n - (size_t)(a + 1 - m));
    if (!b || memchr(b + 1, 0, n - (size_t)(b + 1 - m)))
        return MAILK_EBAD;
    out->az = 0;
    out->azn = (size_t)(a - m);
    out->ac = out->azn + 1;
    out->acn = (size_t)(b - a - 1);
    out->pw = out->ac + out->acn + 1;
    out->pwn = n - out->pw;
    if (!field_ok((const char*)m, out->azn, 0) || !field_ok((const char*)m + out->ac, out->acn, 1)
        || !field_ok((const char*)m + out->pw, out->pwn, 1))
        return MAILK_EBAD;
    return MAILK_OK;
}

static int ieq(const uint8_t* m, size_t n, const char* lit)
{
    size_t i, l = strlen(lit);
    if (n != l)
        return 0;
    for (i = 0; i < n; i++) {
        uint8_t c = m[i] >= 'A' && m[i] <= 'Z' ? (uint8_t)(m[i] + 32) : m[i];
        if (c != (uint8_t)lit[i])
            return 0;
    }
    return 1;
}

int mailk_login_prompt(const uint8_t* m, size_t n)
{
    while (n && (m[n - 1] == ' ' || m[n - 1] == ':'))
        n--;
    if (ieq(m, n, "username") || ieq(m, n, "user name") || ieq(m, n, "user"))
        return 1;
    if (ieq(m, n, "password"))
        return 2;
    return 0;
}

static int tok_ok(const char* t, size_t n)
{
    size_t i;
    if (!n)
        return 0;
    for (i = 0; i < n; i++) {
        uint8_t c = (uint8_t)t[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || strchr("-._~+/=", c)))
            return 0;
    }
    for (i = 0; i < n && t[i] != '='; i++)
        ;
    while (i < n && t[i] == '=')
        i++;
    return i == n;
}

static int kv_val_ok(const uint8_t* v, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (!((v[i] >= 0x20 && v[i] <= 0x7E) || v[i] == '\t' || v[i] == '\r' || v[i] == '\n'))
            return 0;
    return 1;
}

int mailk_xoauth2_build(dyn_sb_t* out, const char* user, const char* token)
{
    size_t un = strlen(user), tn = strlen(token);
    if (!un || un > MAILK_SASL_FIELD_MAX || !kv_val_ok((const uint8_t*)user, un) || memchr(user, '\r', un)
        || memchr(user, '\n', un) || !tok_ok(token, tn))
        return MAILK_EBAD;
    dyn_sb_puts(out, "user=");
    dyn_sb_put(out, user, un);
    dyn_sb_puts(out, "\001auth=Bearer ");
    dyn_sb_put(out, token, tn);
    dyn_sb_puts(out, "\001\001");
    return out->oom ? MAILK_ENOMEM : MAILK_OK;
}

static int kv_walk(const uint8_t* m, size_t n, size_t pos, mailk_oauth_t* out, int x2)
{
    for (;;) {
        const uint8_t* e;
        size_t k = pos, kl, v, vl;
        if (pos >= n)
            return MAILK_EBAD;
        if (m[pos] == 1)
            return pos + 1 == n && out->tokn ? MAILK_OK : MAILK_EBAD;
        while (k < n && ((m[k] >= 'A' && m[k] <= 'Z') || (m[k] >= 'a' && m[k] <= 'z')))
            k++;
        kl = k - pos;
        if (!kl || k >= n || m[k] != '=')
            return MAILK_EBAD;
        v = k + 1;
        e = memchr(m + v, 1, n - v);
        if (!e)
            return MAILK_EBAD;
        vl = (size_t)(e - m) - v;
        if (!kv_val_ok(m + v, vl))
            return MAILK_EBAD;
        if (kl == 4 && !memcmp(m + pos, "auth", 4)) {
            if (out->tokn || vl < 8 || !ieq(m + v, 7, "bearer ") || !tok_ok((const char*)m + v + 7, vl - 7))
                return MAILK_EBAD;
            out->tok = v + 7;
            out->tokn = vl - 7;
        } else if (!x2 && kl == 4 && !memcmp(m + pos, "host", 4)) {
            out->host = v;
            out->hostn = vl;
        } else if (!x2 && kl == 4 && !memcmp(m + pos, "port", 4)) {
            size_t i;
            if (!vl || vl > 5)
                return MAILK_EBAD;
            for (i = 0; i < vl; i++)
                if (m[v + i] < '0' || m[v + i] > '9')
                    return MAILK_EBAD;
            out->port = v;
            out->portn = vl;
        } else if (x2 && kl == 4 && !memcmp(m + pos, "user", 4)) {
            if (out->azn || !vl || vl > MAILK_SASL_FIELD_MAX)
                return MAILK_EBAD;
            out->az = v;
            out->azn = vl;
        }
        pos = (size_t)(e - m) + 1;
    }
}

int mailk_xoauth2_parse(const uint8_t* m, size_t n, mailk_oauth_t* out)
{
    int rc;
    memset(out, 0, sizeof(*out));
    rc = kv_walk(m, n, 0, out, 1);
    return rc == MAILK_OK && !out->azn ? MAILK_EBAD : rc;
}

int mailk_oauthbearer_build(dyn_sb_t* out, const char* authzid, const char* host, unsigned port, const char* token)
{
    size_t i, zn = authzid ? strlen(authzid) : 0, hn = host ? strlen(host) : 0;
    char pb[8];
    if (!tok_ok(token, strlen(token)) || port > 65535 || zn > MAILK_SASL_FIELD_MAX || hn > MAILK_DOMAIN_MAX
        || !utf8_ok((const uint8_t*)(authzid ? authzid : ""), zn) || !kv_val_ok((const uint8_t*)(host ? host : ""), hn)
        || (host && strpbrk(host, "\r\n\t ")))
        return MAILK_EBAD;
    for (i = 0; i < zn; i++)
        if ((uint8_t)authzid[i] < 0x20 || authzid[i] == 0x7F)
            return MAILK_EBAD;
    dyn_sb_puts(out, "n,");
    if (zn) {
        dyn_sb_puts(out, "a=");
        if (mailk_saslname_encode(out, authzid, zn) != MAILK_OK)
            return MAILK_EBAD;
    }
    dyn_sb_puts(out, ",\001");
    if (hn) {
        dyn_sb_puts(out, "host=");
        dyn_sb_put(out, host, hn);
        dyn_sb_putc(out, 1);
    }
    if (port) {
        int l = 0;
        unsigned p = port;
        char t[8];
        while (p) {
            t[l++] = (char)('0' + p % 10);
            p /= 10;
        }
        for (i = 0; i < (size_t)l; i++)
            pb[i] = t[l - 1 - (int)i];
        dyn_sb_puts(out, "port=");
        dyn_sb_put(out, pb, (size_t)l);
        dyn_sb_putc(out, 1);
    }
    dyn_sb_puts(out, "auth=Bearer ");
    dyn_sb_puts(out, token);
    dyn_sb_puts(out, "\001\001");
    return out->oom ? MAILK_ENOMEM : MAILK_OK;
}

int mailk_saslname_decode(dyn_sb_t* out, const char* s, size_t n)
{
    size_t i;
    size_t st = out->n;
    if (n > 3 * MAILK_SASL_FIELD_MAX)
        return MAILK_ELONG;
    for (i = 0; i < n; i++) {
        if (s[i] == ',' || s[i] == 0)
            return MAILK_EBAD;
        if (s[i] == '=') {
            if (i + 2 >= n || !((s[i + 1] == '2' && s[i + 2] == 'C') || (s[i + 1] == '3' && s[i + 2] == 'D')))
                return MAILK_EBAD;
            dyn_sb_putc(out, s[i + 1] == '2' ? ',' : '=');
            i += 2;
        } else {
            dyn_sb_putc(out, s[i]);
        }
    }
    if (!out->oom && out->n - st > MAILK_SASL_FIELD_MAX)
        return out->n = st, MAILK_ELONG;
    if (!out->oom && out->n > st && !utf8_ok((const uint8_t*)out->p + st, out->n - st))
        return out->n = st, MAILK_EBAD;
    return out->oom ? MAILK_ENOMEM : MAILK_OK;
}

int mailk_saslname_encode(dyn_sb_t* out, const char* s, size_t n)
{
    size_t i;
    if (!n)
        return MAILK_OK;
    if (n > MAILK_SASL_FIELD_MAX || memchr(s, 0, n) || !utf8_ok((const uint8_t*)s, n))
        return MAILK_EBAD;
    for (i = 0; i < n; i++) {
        if (s[i] == ',')
            dyn_sb_puts(out, "=2C");
        else if (s[i] == '=')
            dyn_sb_puts(out, "=3D");
        else
            dyn_sb_putc(out, s[i]);
    }
    return out->oom ? MAILK_ENOMEM : MAILK_OK;
}

int mailk_oauthbearer_parse(const uint8_t* m, size_t n, mailk_oauth_t* out)
{
    size_t p = 2, az = 0, azn = 0;
    int rc;
    memset(out, 0, sizeof(*out));
    if (n < 4 || m[0] != 'n' || m[1] != ',')
        return MAILK_EBAD;
    if (m[2] == 'a' && m[3] == '=') {
        dyn_sb_t t;
        const uint8_t* e = memchr(m + 4, ',', n - 4);
        if (!e)
            return MAILK_EBAD;
        az = 4;
        azn = (size_t)(e - m) - 4;
        dyn_sb_init(&t);
        rc = azn ? mailk_saslname_decode(&t, (const char*)m + 4, azn) : MAILK_EBAD;
        if (rc == MAILK_OK && !field_ok(t.p, t.n, 1))
            rc = MAILK_EBAD;
        dyn_sb_free(&t);
        if (rc != MAILK_OK)
            return rc;
        p = (size_t)(e - m);
    }
    if (p + 1 >= n || m[p] != ',' || m[p + 1] != 1)
        return MAILK_EBAD;
    rc = kv_walk(m, n, p + 2, out, 0);
    out->az = az;
    out->azn = azn;
    return rc;
}

int mailk_cram_md5(dyn_sb_t* out, const char* user, const uint8_t* secret, size_t secretn,
    const uint8_t* challenge, size_t challengen)
{
    static const char hx[] = "0123456789abcdef";
    uint8_t d[16];
    char h[32];
    size_t i, un = strlen(user);
    if (!un || un > MAILK_SASL_FIELD_MAX || !utf8_ok((const uint8_t*)user, un) || strpbrk(user, " \r\n")
        || challengen > MAILK_SASL_MAX)
        return MAILK_EBAD;
    dyn_hmac(dyn_hash_algo_by_id(DYN_HASH_MD5), secret, secretn, challenge, challengen, d);
    for (i = 0; i < 16; i++) {
        h[2 * i] = hx[d[i] >> 4];
        h[2 * i + 1] = hx[d[i] & 15];
    }
    dyn_sb_put(out, user, un);
    dyn_sb_putc(out, ' ');
    dyn_sb_put(out, h, 32);
    return out->oom ? MAILK_ENOMEM : MAILK_OK;
}
