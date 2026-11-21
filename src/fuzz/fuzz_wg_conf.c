#include "modules/wireguard/wg-conf.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

static void fz_die(int line)
{
    fprintf(stderr, "fuzz_wg_conf: oracle failed at line %d\n", line);
    abort();
}

enum { EV_MAX = 512, TXT_MAX = 16384 };

typedef struct {
    int sec, key;
    unsigned line;
    size_t off, n;
} ev_t;

typedef struct {
    const char* s;
    size_t n;
    unsigned line, nev;
    int sec;
    ev_t ev[EV_MAX];
} fz_t;

typedef struct {
    const uint8_t* p;
    size_t n;
} in_t;

static const struct {
    const char* name;
    int sec, key, scalar;
} K[] = {
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
enum { NK = sizeof K / sizeof K[0] };

static volatile size_t fz_sink;

static unsigned take(in_t* in)
{
    unsigned v;
    if (!in->n)
        return 0;
    v = in->p[0];
    in->p++;
    in->n--;
    return v;
}

static int naive_b64(const char* s, size_t n, uint8_t out[32])
{
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    uint8_t six[43];
    size_t i;
    if (n != 44 || s[43] != '=')
        return -1;
    for (i = 0; i < 43; i++) {
        const char* q = s[i] ? strchr(t, s[i]) : NULL;
        if (!q)
            return -1;
        six[i] = (uint8_t)(q - t);
    }
    if (six[42] & 3)
        return -1;
    for (i = 0; i < 32; i++) {
        size_t bit = i * 8, w = bit / 6, sh = bit % 6;
        unsigned v = (unsigned)six[w] << 6 | (w + 1 < 43 ? six[w + 1] : 0);
        out[i] = (uint8_t)(v >> (4 - sh));
    }
    return 0;
}

static int naive_uint(const char* s, size_t n, unsigned max, unsigned* out)
{
    unsigned long long v = 0;
    size_t i;
    if (!n || n > 10)
        return -1;
    for (i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9')
            return -1;
        v = v * 10 + (unsigned)(s[i] - '0');
    }
    if (v > max)
        return -1;
    *out = (unsigned)v;
    return 0;
}

static void fz_item(const char* v, size_t vn)
{
    uint8_t k[32], k2[32], a[16], a2[16];
    char b[WG_CONF_KEY_B64 + 1], ip[48], ep[64];
    int v6, v62, rk, rn;
    unsigned c, u, u2;
    uint16_t port, port2;
    rk = wg_conf_key(v, vn, k);
    rn = naive_b64(v, vn, k2);
    if (rk != rn || (rk == 0 && memcmp(k, k2, 32)))
        fz_die(__LINE__);
    if (rk == 0) {
        wg_conf_key_b64(k, b);
        if (vn != WG_CONF_KEY_B64 || memcmp(b, v, vn))
            fz_die(__LINE__);
    }
    rk = wg_conf_uint(v, vn, 65535, &u);
    rn = naive_uint(v, vn, 65535, &u2);
    if (rk != rn || (rk == 0 && u != u2))
        fz_die(__LINE__);
    if (wg_conf_cidr(v, vn, &v6, a, &c) == 0) {
        const char* sl = (const char*)memchr(v, '/', vn);
        if (c > (v6 ? 128u : 32u) || wg_conf_ip(v, sl ? (size_t)(sl - v) : vn, &v62, a2) || v62 != v6 || memcmp(a, a2, 16))
            fz_die(__LINE__);
        if (sl && (naive_uint(sl + 1, vn - (size_t)(sl + 1 - v), v6 ? 128 : 32, &u2) || u2 != c))
            fz_die(__LINE__);
    }
    if (wg_conf_ip(v, vn, &v6, a) == 0) {
        wg_conf_fmt_ip(v6, a, ip);
        if (wg_conf_ip(ip, strlen(ip), &v62, a2) != 0 || v62 != v6 || memcmp(a, a2, 16))
            fz_die(__LINE__);
    }
    if (wg_conf_endpoint(v, vn, &v6, a, &port) == 0) {
        size_t n;
        wg_conf_fmt_ip(v6, a, ip);
        n = (size_t)snprintf(ep, sizeof ep, v6 ? "[%s]:%u" : "%s:%u", ip, (unsigned)port);
        if (!port || wg_conf_endpoint(ep, n, &v62, a2, &port2) || v62 != v6 || port2 != port || memcmp(a, a2, 16))
            fz_die(__LINE__);
        if ((v[0] == '[') != v6)
            fz_die(__LINE__);
    }
}

static void fz_values(const char* v, size_t vn)
{
    size_t off = 0, in, total = 0;
    const char* it;
    fz_item(v, vn);
    while (wg_conf_next(v, vn, &off, &it, &in)) {
        if (it < v || it + in > v + vn || !in || memchr(it, ',', in) || it[0] == ' ' || it[in - 1] == ' ' || it[0] == '\t' || it[in - 1] == '\t')
            fz_die(__LINE__);
        fz_item(it, in);
        total += in;
    }
    if (total > vn)
        fz_die(__LINE__);
    fz_sink += total;
}

static int fz_cb(void* ud, int sec, int key, const char* v, size_t vn, unsigned line)
{
    fz_t* f = (fz_t*)ud;
    if (line < f->line || (sec != WG_CONF_IFACE && sec != WG_CONF_PEER) || key < 0 || key >= WG_CK_N)
        fz_die(__LINE__);
    f->line = line;
    if (f->nev < EV_MAX) {
        ev_t* e = &f->ev[f->nev++];
        e->sec = sec;
        e->key = key;
        e->line = line;
        e->off = v ? (size_t)(v - f->s) : 0;
        e->n = vn;
    }
    if (key == WG_CK_SECTION) {
        if (v || vn)
            fz_die(__LINE__);
        f->sec = sec;
        return 0;
    }
    if (sec != f->sec || v < f->s || v + vn > f->s + f->n || (!vn && key != WG_CK_IGNORED) || memchr(v, '\n', vn) || memchr(v, '#', vn))
        fz_die(__LINE__);
    fz_values(v, vn);
    return 0;
}

static int scan(fz_t* f, const char* s, size_t n, wg_conf_err_t* e)
{
    int rc;
    memset(f, 0, sizeof *f);
    memset(e, 0, sizeof *e);
    f->s = s;
    f->n = n;
    rc = wg_conf_scan(s, n, fz_cb, f, e);
    if ((rc < 0) != (e->msg != NULL))
        fz_die(__LINE__);
    return rc;
}

typedef struct {
    char t[TXT_MAX];
    size_t n;
    int fail;
    unsigned line, nev, fail_line;
    ev_t ev[EV_MAX];
} gen_t;

static int put(gen_t* g, const char* s, size_t n)
{
    if (g->n + n >= TXT_MAX)
        return -1;
    memcpy(g->t + g->n, s, n);
    g->n += n;
    return 0;
}

static void ws(gen_t* g, in_t* in)
{
    static const char w[] = " \t\v\f\r";
    unsigned k = take(in) % 4, i;
    for (i = 0; i < k; i++)
        put(g, w + take(in) % 5, 1);
}

static void ccase(gen_t* g, in_t* in, const char* s)
{
    unsigned m = take(in);
    size_t i;
    for (i = 0; s[i]; i++) {
        char c = s[i];
        if (((m >> (i % 8)) & 1) && c >= 'a' && c <= 'z')
            c = (char)(c - 'a' + 'A');
        put(g, &c, 1);
    }
}

static void value(gen_t* g, in_t* in, int type)
{
    char b[96], ip[48];
    uint8_t a[16];
    unsigned i, k;
    switch (type) {
    case 0: {
        uint8_t key[32];
        char out[WG_CONF_KEY_B64 + 1];
        for (i = 0; i < 32; i++)
            key[i] = (uint8_t)take(in);
        wg_conf_key_b64(key, out);
        if ((take(in) & 7) == 0)
            out[take(in) % 44] = "A/+=_-Z9"[take(in) % 8];
        put(g, out, 44);
        break;
    }
    case 1:
    case 2: {
        unsigned items = 1 + take(in) % 3;
        for (k = 0; k < items; k++) {
            int v6 = take(in) & 1;
            for (i = 0; i < 16; i++)
                a[i] = (uint8_t)take(in);
            wg_conf_fmt_ip(v6, a, ip);
            if (k)
                put(g, ", ", 2);
            put(g, ip, strlen(ip));
            if (type == 2) {
                int n = snprintf(b, sizeof b, "/%u", take(in) % (v6 ? 140 : 40));
                put(g, b, (size_t)n);
            }
        }
        break;
    }
    case 3: {
        int v6 = take(in) & 1, n;
        unsigned port = take(in) << 8 | take(in);
        for (i = 0; i < 16; i++)
            a[i] = (uint8_t)take(in);
        wg_conf_fmt_ip(v6, a, ip);
        if (take(in) & 1)
            port += 65536 * (take(in) & 1);
        n = snprintf(b, sizeof b, v6 ? "[%s]:%u" : "%s:%u", ip, port);
        put(g, b, (size_t)n);
        break;
    }
    case 4: {
        int n = snprintf(b, sizeof b, "%u", (take(in) << 16 | take(in) << 8 | take(in)) % (take(in) & 1 ? 70000 : 300));
        put(g, b, (size_t)n);
        break;
    }
    default: {
        unsigned n = 1 + take(in) % 24;
        char c;
        for (i = 0; i < n; i++) {
            c = (char)(33 + take(in) % 94);
            if (c == '#')
                c = '=';
            put(g, &c, 1);
        }
    }
    }
}

static void expect(gen_t* g, int sec, int key, size_t off, size_t n)
{
    if (g->nev < EV_MAX && !g->fail) {
        ev_t* e = &g->ev[g->nev++];
        e->sec = sec;
        e->key = key;
        e->line = g->line;
        e->off = off;
        e->n = n;
    }
}

static void fail_at(gen_t* g)
{
    if (!g->fail) {
        g->fail = 1;
        g->fail_line = g->line;
    }
}

static void generate(gen_t* g, in_t* in)
{
    int sec = 0, ifaces = 0;
    unsigned seen = 0, lines = 0;
    memset(g, 0, sizeof *g);
    while (in->n && lines++ < 200 && g->n < TXT_MAX - 512) {
        unsigned op = take(in) % 10;
        g->line++;
        if (op == 0) {
            ws(g, in);
            if (take(in) & 1)
                put(g, "# note = [peer]", 15);
        } else if (op == 1) {
            static const char* const names[] = { "interface", "peer", "peers", "inter face", "", "peer]", "inter", "pee" };
            unsigned which = take(in) % 8, unterminated = (take(in) & 15) == 0 && which != 5;
            ws(g, in);
            put(g, "[", 1);
            ws(g, in);
            ccase(g, in, names[which]);
            ws(g, in);
            if (!unterminated)
                put(g, "]", 1);
            ws(g, in);
            if (take(in) & 1)
                put(g, "#x", 2);
            if (unterminated || which > 1 || (which == 0 && ifaces))
                fail_at(g);
            else {
                if (which == 0)
                    ifaces++;
                sec = which == 0 ? WG_CONF_IFACE : WG_CONF_PEER;
                seen = 0;
                expect(g, sec, WG_CK_SECTION, 0, 0);
            }
        } else if (op <= 7) {
            unsigned ki = take(in) % (NK + 4), type;
            size_t voff, vend;
            int unknown = ki >= NK;
            ws(g, in);
            ccase(g, in, unknown ? (ki == NK ? "privatekeys" : ki == NK + 1 ? "allowed ips" : ki == NK + 2 ? "private" : "endpoin") : K[ki].name);
            ws(g, in);
            put(g, "=", 1);
            ws(g, in);
            voff = g->n;
            type = take(in) % 8;
            if (!unknown)
                type = (take(in) & 3) ? (K[ki].key == WG_CK_PRIVATEKEY || K[ki].key == WG_CK_PUBLICKEY || K[ki].key == WG_CK_PRESHAREDKEY ? 0u
                                         : K[ki].key == WG_CK_ADDRESS || K[ki].key == WG_CK_ALLOWEDIPS                                ? 2u
                                         : K[ki].key == WG_CK_ENDPOINT                                                                 ? 3u
                                         : K[ki].key == WG_CK_IGNORED                                                                  ? 5u
                                                                                                                                       : 4u)
                                      : type;
            if ((take(in) & 15) != 0)
                value(g, in, (int)type);
            vend = g->n;
            ws(g, in);
            if (take(in) & 1)
                put(g, "# = c", 5);
            if (!sec || unknown || K[ki].sec != sec || (K[ki].scalar && (seen >> ki & 1)) || (vend == voff && K[ki].key != WG_CK_IGNORED))
                fail_at(g);
            else {
                if (K[ki].scalar)
                    seen |= 1u << ki;
                expect(g, sec, K[ki].key, voff, vend - voff);
            }
        } else if (op == 8) {
            ws(g, in);
            put(g, "garbage line", 12);
            fail_at(g);
        } else {
            ws(g, in);
            put(g, "= x", 3);
            fail_at(g);
        }
        if (in->n || (take(in) & 1)) {
            int crlf = take(in) & 1;
            put(g, crlf ? "\r\n" : "\n", crlf ? 2u : 1u);
        }
    }
}

static void check_gen(in_t* in)
{
    static gen_t g;
    static fz_t f;
    wg_conf_err_t e;
    unsigned i;
    int rc;
    generate(&g, in);
    rc = scan(&f, g.t, g.n, &e);
    if ((rc < 0) != g.fail || (g.fail && e.line != g.fail_line))
        fz_die(__LINE__);
    if (f.nev != g.nev)
        fz_die(__LINE__);
    for (i = 0; i < f.nev && i < g.nev; i++)
        if (f.ev[i].sec != g.ev[i].sec || f.ev[i].key != g.ev[i].key || f.ev[i].line != g.ev[i].line || f.ev[i].n != g.ev[i].n ||
            (f.ev[i].n && memcmp(g.t + f.ev[i].off, g.t + g.ev[i].off, g.ev[i].n)))
            fz_die(__LINE__);
}

static void check_crlf(const char* s, size_t n)
{
    static char t[TXT_MAX * 2];
    static fz_t a, b;
    wg_conf_err_t ea, eb;
    size_t i, k = 0, run = 0;
    int ra, rb;
    unsigned j;
    if (n > TXT_MAX)
        return;
    for (i = 0; i < n; i++) {
        run = s[i] == '\n' ? 0 : run + 1;
        if (run >= WG_CONF_LINE_MAX - 1)
            return;
        if (s[i] == '\n')
            t[k++] = '\r';
        t[k++] = s[i];
    }
    ra = scan(&a, s, n, &ea);
    rb = scan(&b, t, k, &eb);
    if (ra != rb || ea.line != eb.line || a.nev != b.nev)
        fz_die(__LINE__);
    for (j = 0; j < a.nev; j++)
        if (a.ev[j].key != b.ev[j].key || a.ev[j].sec != b.ev[j].sec || a.ev[j].line != b.ev[j].line || a.ev[j].n != b.ev[j].n || memcmp(s + a.ev[j].off, t + b.ev[j].off, a.ev[j].n))
            fz_die(__LINE__);
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    static fz_t f;
    wg_conf_err_t e;
    in_t in;
    unsigned mode;
    if (!size)
        return 0;
    mode = data[0] % 4;
    in.p = data + 1;
    in.n = size - 1;
    if (mode == 0) {
        int rc = scan(&f, (const char*)in.p, in.n, &e);
        if (rc == 0 && e.msg)
            fz_die(__LINE__);
        if (in.n <= 200)
            fz_values((const char*)in.p, in.n);
    } else if (mode == 1 || mode == 2)
        check_gen(&in);
    else {
        check_crlf((const char*)in.p, in.n);
        if (in.n <= 200)
            fz_values((const char*)in.p, in.n);
    }
    return 0;
}
