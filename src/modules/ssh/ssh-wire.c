#include "ssh-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_SSH)

uint32_t ssh_be32(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

void ssh_put_be32(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

void sshb_free(sshb_t* b)
{
    if (b->p) {
        OPENSSL_cleanse(b->p, b->cap);
        free(b->p);
    }
    b->p = NULL;
    b->len = b->cap = 0;
}

int sshb_reserve(sshb_t* b, size_t extra)
{
    size_t need, c;
    uint8_t* n;
    if (extra > SIZE_MAX / 2 - b->len)
        return -1;
    need = b->len + extra;
    if (need <= b->cap)
        return 0;
    c = b->cap ? b->cap : 256;
    while (c < need)
        c = c < ((size_t)1 << 20) ? c * 2 : c + c / 2;
    n = (uint8_t*)realloc(b->p, c);
    if (!n)
        return -1;
    b->p = n;
    b->cap = c;
    return 0;
}

int sshb_put(sshb_t* b, const void* p, size_t n)
{
    if (sshb_reserve(b, n) < 0)
        return -1;
    if (n)
        memcpy(b->p + b->len, p, n);
    b->len += n;
    return 0;
}

int sshb_u8(sshb_t* b, uint8_t v)
{
    return sshb_put(b, &v, 1);
}

int sshb_u32(sshb_t* b, uint32_t v)
{
    uint8_t t[4];
    ssh_put_be32(t, v);
    return sshb_put(b, t, 4);
}

int sshb_str(sshb_t* b, const void* p, size_t n)
{
    if (n > UINT32_MAX || sshb_u32(b, (uint32_t)n) < 0)
        return -1;
    return sshb_put(b, p, n);
}

int sshb_cstr(sshb_t* b, const char* s)
{
    return sshb_str(b, s, strlen(s));
}

int sshb_mpint(sshb_t* b, const uint8_t* p, size_t n)
{
    while (n && *p == 0) {
        p++;
        n--;
    }
    if (n && (p[0] & 0x80)) {
        if (sshb_u32(b, (uint32_t)n + 1) < 0 || sshb_u8(b, 0) < 0)
            return -1;
        return sshb_put(b, p, n);
    }
    return sshb_str(b, p, n);
}

void sshr_init(sshr_t* r, const uint8_t* p, size_t n)
{
    r->p = p;
    r->len = n;
    r->off = 0;
    r->err = 0;
}

uint8_t sshr_u8(sshr_t* r)
{
    if (r->err || r->len - r->off < 1) {
        r->err = 1;
        return 0;
    }
    return r->p[r->off++];
}

uint32_t sshr_u32(sshr_t* r)
{
    uint32_t v;
    if (r->err || r->len - r->off < 4) {
        r->err = 1;
        return 0;
    }
    v = ssh_be32(r->p + r->off);
    r->off += 4;
    return v;
}

const uint8_t* sshr_str(sshr_t* r, size_t* n)
{
    uint32_t l = sshr_u32(r);
    const uint8_t* p;
    if (r->err || r->len - r->off < l) {
        r->err = 1;
        *n = 0;
        return NULL;
    }
    p = r->p + r->off;
    r->off += l;
    *n = l;
    return p;
}

int sshr_bool(sshr_t* r)
{
    return sshr_u8(r) != 0;
}

int ssh_namelist_has(const uint8_t* list, size_t n, const char* name)
{
    size_t nl = strlen(name), i = 0;
    while (i <= n) {
        size_t j = i;
        while (j < n && list[j] != ',')
            j++;
        if (j - i == nl && memcmp(list + i, name, nl) == 0)
            return 1;
        i = j + 1;
    }
    return 0;
}

static int b64v(unsigned char ch)
{
    if (ch >= 'A' && ch <= 'Z')
        return ch - 'A';
    if (ch >= 'a' && ch <= 'z')
        return ch - 'a' + 26;
    if (ch >= '0' && ch <= '9')
        return ch - '0' + 52;
    if (ch == '+')
        return 62;
    if (ch == '/')
        return 63;
    return -1;
}

size_t ssh_b64_decode(const char* s, size_t n, uint8_t* out, size_t cap)
{
    uint32_t acc = 0;
    int bits = 0;
    size_t o = 0, i;
    int pad = 0;
    for (i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)s[i];
        int v;
        if (ch == '\n' || ch == '\r' || ch == ' ' || ch == '\t')
            continue;
        if (ch == '=') {
            pad++;
            continue;
        }
        if (pad)
            return (size_t)-1;
        v = b64v(ch);
        if (v < 0)
            return (size_t)-1;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o >= cap)
                return (size_t)-1;
            out[o++] = (uint8_t)(acc >> bits);
        }
    }
    if (pad > 2 || bits >= 6)
        return (size_t)-1;
    return o;
}

size_t ssh_b64_encode(const uint8_t* p, size_t n, char* out, int pad)
{
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i, o = 0;
    for (i = 0; i + 2 < n; i += 3) {
        uint32_t v = ((uint32_t)p[i] << 16) | ((uint32_t)p[i + 1] << 8) | p[i + 2];
        out[o++] = t[v >> 18];
        out[o++] = t[(v >> 12) & 63];
        out[o++] = t[(v >> 6) & 63];
        out[o++] = t[v & 63];
    }
    if (i < n) {
        uint32_t v = (uint32_t)p[i] << 16;
        if (i + 1 < n)
            v |= (uint32_t)p[i + 1] << 8;
        out[o++] = t[v >> 18];
        out[o++] = t[(v >> 12) & 63];
        if (i + 1 < n)
            out[o++] = t[(v >> 6) & 63];
        else if (pad)
            out[o++] = '=';
        if (pad)
            out[o++] = '=';
    }
    out[o] = 0;
    return o;
}

#endif
