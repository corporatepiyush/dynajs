#include "wg-conf.h"

#include <arpa/inet.h>
#include <string.h>

static const struct {
    const char* name;
    int sec, key, scalar;
} keys[] = {
    { "privatekey", WG_CONF_IFACE, WG_CK_PRIVATEKEY, 1 },
    { "listenport", WG_CONF_IFACE, WG_CK_LISTENPORT, 1 },
    { "address", WG_CONF_IFACE, WG_CK_ADDRESS, 0 },
    { "mtu", WG_CONF_IFACE, WG_CK_MTU, 1 },
    { "dns", WG_CONF_IFACE, WG_CK_IGNORED, 0 },
    { "table", WG_CONF_IFACE, WG_CK_IGNORED, 0 },
    { "fwmark", WG_CONF_IFACE, WG_CK_IGNORED, 0 },
    { "preup", WG_CONF_IFACE, WG_CK_IGNORED, 0 },
    { "postup", WG_CONF_IFACE, WG_CK_IGNORED, 0 },
    { "predown", WG_CONF_IFACE, WG_CK_IGNORED, 0 },
    { "postdown", WG_CONF_IFACE, WG_CK_IGNORED, 0 },
    { "saveconfig", WG_CONF_IFACE, WG_CK_IGNORED, 0 },
    { "publickey", WG_CONF_PEER, WG_CK_PUBLICKEY, 1 },
    { "presharedkey", WG_CONF_PEER, WG_CK_PRESHAREDKEY, 1 },
    { "endpoint", WG_CONF_PEER, WG_CK_ENDPOINT, 1 },
    { "allowedips", WG_CONF_PEER, WG_CK_ALLOWEDIPS, 0 },
    { "persistentkeepalive", WG_CONF_PEER, WG_CK_KEEPALIVE, 1 },
};

static int is_ws(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\v' || c == '\f';
}

static void trim(const char** p, size_t* n)
{
    while (*n && is_ws(**p)) {
        (*p)++;
        (*n)--;
    }
    while (*n && is_ws((*p)[*n - 1]))
        (*n)--;
}

static int ieq(const char* a, size_t an, const char* b)
{
    size_t i;
    for (i = 0; i < an; i++) {
        char c = a[i];
        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
        if (!b[i] || c != b[i])
            return 0;
    }
    return b[an] == 0;
}

static int fail(wg_conf_err_t* e, unsigned line, const char* msg)
{
    if (e) {
        e->line = line;
        e->msg = msg;
    }
    return -1;
}

int wg_conf_scan(const char* s, size_t n, wg_conf_fn fn, void* ud, wg_conf_err_t* e)
{
    size_t at = 0;
    unsigned line = 0, seen = 0;
    int sec = 0, ifaces = 0;
    if (memchr(s, 0, n))
        return fail(e, 0, "NUL byte in the text");
    while (at < n) {
        const char *p = s + at, *nl = (const char*)memchr(p, '\n', n - at), *hash, *eq, *k, *v;
        size_t ln = nl ? (size_t)(nl - p) : n - at, kn, vn;
        unsigned i;
        at += ln + (nl ? 1 : 0);
        line++;
        if (ln > WG_CONF_LINE_MAX)
            return fail(e, line, "line longer than 4096 bytes");
        hash = (const char*)memchr(p, '#', ln);
        if (hash)
            ln = (size_t)(hash - p);
        trim(&p, &ln);
        if (!ln)
            continue;
        if (p[0] == '[') {
            if (p[ln - 1] != ']')
                return fail(e, line, "unterminated section header");
            k = p + 1;
            kn = ln - 2;
            trim(&k, &kn);
            if (ieq(k, kn, "interface")) {
                if (ifaces++)
                    return fail(e, line, "duplicate [Interface]");
                sec = WG_CONF_IFACE;
            } else if (ieq(k, kn, "peer"))
                sec = WG_CONF_PEER;
            else
                return fail(e, line, "unknown section");
            seen = 0;
            if (fn && fn(ud, sec, WG_CK_SECTION, NULL, 0, line) != 0)
                return fail(e, line, e && e->msg ? e->msg : "rejected section");
            continue;
        }
        eq = (const char*)memchr(p, '=', ln);
        if (!eq)
            return fail(e, line, "expected key = value");
        if (!sec)
            return fail(e, line, "key outside a section");
        k = p;
        kn = (size_t)(eq - p);
        v = eq + 1;
        vn = ln - kn - 1;
        trim(&k, &kn);
        trim(&v, &vn);
        for (i = 0; i < sizeof keys / sizeof keys[0]; i++)
            if (keys[i].sec == sec && ieq(k, kn, keys[i].name))
                break;
        if (i == sizeof keys / sizeof keys[0])
            return fail(e, line, "unknown key");
        if (keys[i].scalar) {
            if (seen & (1u << i))
                return fail(e, line, "duplicate key");
            seen |= 1u << i;
        }
        if (!vn && keys[i].key != WG_CK_IGNORED)
            return fail(e, line, "empty value");
        if (fn && fn(ud, sec, keys[i].key, v, vn, line) != 0)
            return fail(e, line, e && e->msg ? e->msg : "rejected value");
    }
    return 0;
}

static int b64v(char c)
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

int wg_conf_key(const char* s, size_t n, uint8_t out[32])
{
    unsigned acc = 0, bits = 0;
    size_t i, o = 0;
    int bad = 0;
    if (n != WG_CONF_KEY_B64 || s[43] != '=')
        return -1;
    for (i = 0; i < 43; i++) {
        int v = b64v(s[i]);
        bad |= v < 0;
        acc = acc << 6 | (unsigned)(v & 63);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[o++] = (uint8_t)(acc >> bits);
        }
    }
    bad |= (acc & ((1u << bits) - 1u)) != 0;
    if (bad) {
        memset(out, 0, 32);
        return -1;
    }
    return 0;
}

void wg_conf_key_b64(const uint8_t k[32], char out[WG_CONF_KEY_B64 + 1])
{
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    unsigned acc = 0, bits = 0;
    size_t i, o = 0;
    for (i = 0; i < 32; i++) {
        acc = acc << 8 | k[i];
        bits += 8;
        while (bits >= 6) {
            bits -= 6;
            out[o++] = t[(acc >> bits) & 63];
        }
    }
    out[o++] = t[(acc << (6 - bits)) & 63];
    out[o++] = '=';
    out[o] = 0;
}

int wg_conf_ip(const char* s, size_t n, int* v6, uint8_t addr[16])
{
    char b[48];
    if (!n || n >= sizeof b)
        return -1;
    memcpy(b, s, n);
    b[n] = 0;
    memset(addr, 0, 16);
    if (inet_pton(AF_INET, b, addr) == 1) {
        *v6 = 0;
        return 0;
    }
    if (inet_pton(AF_INET6, b, addr) == 1) {
        *v6 = 1;
        return 0;
    }
    return -1;
}

int wg_conf_uint(const char* s, size_t n, unsigned max, unsigned* out)
{
    unsigned long v = 0;
    size_t i;
    if (!n || n > 10)
        return -1;
    for (i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9')
            return -1;
        v = v * 10 + (unsigned long)(s[i] - '0');
    }
    if (v > max)
        return -1;
    *out = (unsigned)v;
    return 0;
}

int wg_conf_cidr(const char* s, size_t n, int* v6, uint8_t addr[16], unsigned* cidr)
{
    const char* sl = (const char*)memchr(s, '/', n);
    size_t an = sl ? (size_t)(sl - s) : n;
    if (wg_conf_ip(s, an, v6, addr) < 0)
        return -1;
    if (!sl) {
        *cidr = *v6 ? 128 : 32;
        return 0;
    }
    return wg_conf_uint(sl + 1, n - an - 1, *v6 ? 128 : 32, cidr);
}

int wg_conf_endpoint(const char* s, size_t n, int* v6, uint8_t addr[16], uint16_t* port)
{
    const char* c;
    size_t hn;
    unsigned p;
    if (n && s[0] == '[') {
        const char* rb = (const char*)memchr(s, ']', n);
        if (!rb || (size_t)(rb - s) + 1 >= n || rb[1] != ':')
            return -1;
        if (wg_conf_ip(s + 1, (size_t)(rb - s) - 1, v6, addr) < 0 || !*v6)
            return -1;
        c = rb + 1;
    } else {
        c = NULL;
        for (hn = n; hn > 0; hn--)
            if (s[hn - 1] == ':') {
                c = s + hn - 1;
                break;
            }
        if (!c || wg_conf_ip(s, (size_t)(c - s), v6, addr) < 0 || *v6)
            return -1;
    }
    if (wg_conf_uint(c + 1, n - (size_t)(c + 1 - s), 65535, &p) < 0 || !p)
        return -1;
    *port = (uint16_t)p;
    return 0;
}

int wg_conf_next(const char* s, size_t n, size_t* off, const char** item, size_t* in)
{
    while (*off <= n) {
        const char* p = s + *off;
        const char* c = (const char*)memchr(p, ',', n - *off);
        size_t ln = c ? (size_t)(c - p) : n - *off;
        *off += ln + 1;
        trim(&p, &ln);
        if (ln) {
            *item = p;
            *in = ln;
            return 1;
        }
    }
    return 0;
}

void wg_conf_fmt_ip(int v6, const uint8_t addr[16], char out[48])
{
    if (!inet_ntop(v6 ? AF_INET6 : AF_INET, addr, out, 48))
        out[0] = 0;
}
