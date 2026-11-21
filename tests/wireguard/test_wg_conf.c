/*
 * test_wg_conf.c -- dyna:wireguard S13e wg-quick reader and value parsers (src/modules/wireguard/wg-conf.c), pure C,
 * no sockets. Backs WireGuard.parseConfig and every key / CIDR / endpoint option of WireGuard.create.
 *  1. Keys: base64 of exactly 32 bytes, 44 chars ending '=', canonical (the 2 spare bits zero, as wg(8) requires);
 *     encode(decode(k)) == k over 2000 random keys; bad length, alphabet, padding rejected and the output zeroed.
 *  2. CIDR and IP: v4/v6 literals, prefix 0..32 / 0..128, default full length, no spaces, no empty prefix.
 *  3. Endpoints: a.b.c.d:port and [v6]:port, port 1..65535; bare v6, missing port, port 0 rejected.
 *  4. Unsigned parse (digits only, <= max) and the comma-list iterator (trims, skips empties).
 *  5. Scanner: sections case-insensitive, comments anywhere, CRLF, duplicate scalar keys, repeated list keys, ignored
 *     wg-quick keys (may be empty), every error message with its line number, a callback veto.
 *  6. The fuzz entry (src/fuzz/fuzz_wg_conf.c) on 20000 random inputs and 20000 mutated configs.
 *  7. Seeded properties (tests/lib/dyn-fuzzkit.h; DYN_FUZZ_SEED replays, DYN_FUZZ_ITERS scales, DYN_FUZZ_ONLY picks):
 *     conf-roundtrip: random model -> text (case, separators, comments, CRLF, split lists) -> scan == model;
 *     conf-faults: one injected fault (unknown key, no =, duplicate scalar, rejected value, bad section) at its line.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/modules/wireguard/wg-conf.h"
#include "../lib/dyn-fuzzkit.h"

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

static int fails, checks;
#define CHECK(c, msg)                                          \
    do {                                                       \
        checks++;                                              \
        if (!(c)) {                                            \
            fails++;                                           \
            printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); \
        }                                                      \
    } while (0)

static uint64_t rs = 0x9e3779b97f4a7c15ULL;
static uint32_t rnd(void)
{
    rs ^= rs << 13;
    rs ^= rs >> 7;
    rs ^= rs << 17;
    return (uint32_t)(rs >> 11);
}

static int key_ok(const char* s)
{
    uint8_t k[32];
    return wg_conf_key(s, strlen(s), k) == 0;
}

static void t_keys(void)
{
    uint8_t k[32], k2[32];
    char b[WG_CONF_KEY_B64 + 1], m[WG_CONF_KEY_B64 + 1];
    int i, j, bad = 0;
    for (i = 0; i < 2000; i++) {
        for (j = 0; j < 32; j++)
            k[j] = (uint8_t)rnd();
        wg_conf_key_b64(k, b);
        if (strlen(b) != 44 || b[43] != '=' || wg_conf_key(b, 44, k2) != 0 || memcmp(k, k2, 32))
            bad++;
    }
    CHECK(!bad, "decode(encode(k)) == k for 2000 random keys");
    CHECK(key_ok("yAnz5TF+lXXJte14tji3zlMNq+hd2rYUIgJBgB3fBmk="), "a wg(8) example key");
    memset(k, 0, 32);
    wg_conf_key_b64(k, b);
    CHECK(!strcmp(b, "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA="), "zero key encodes as A...A=");
    memset(k, 0xff, 32);
    wg_conf_key_b64(k, b);
    CHECK(!strcmp(b, "//////////////////////////////////////////8="), "all-ones key (last char carries 4 bits)");
    CHECK(!key_ok("//////////////////////////////////////////9=") && !key_ok("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAB="), "non-canonical spare bits rejected");
    CHECK(!key_ok("yAnz5TF+lXXJte14tji3zlMNq+hd2rYUIgJBgB3fBmk") && !key_ok("yAnz5TF+lXXJte14tji3zlMNq+hd2rYUIgJBgB3fBmk=="), "length must be 44");
    CHECK(!key_ok("yAnz5TF+lXXJte14tji3zlMNq+hd2rYUIgJBgB3fBm=="), "padding only at the end");
    CHECK(!key_ok("yAnz5TF-lXXJte14tji3zlMNq+hd2rYUIgJBgB3fBmk=") && !key_ok("yAnz5TF lXXJte14tji3zlMNq+hd2rYUIgJBgB3fBmk="), "base64url and spaces rejected");
    memcpy(m, "yAnz5TF+lXXJte14tji3zlMNq+hd2rYUIgJBgB3fBmk=", 45);
    m[10] = '*';
    memset(k, 0x55, 32);
    CHECK(wg_conf_key(m, 44, k) < 0 && !k[0] && !k[31], "output zeroed on failure");
}

static int cidr(const char* s, int* v6, unsigned* c)
{
    uint8_t a[16];
    return wg_conf_cidr(s, strlen(s), v6, a, c);
}

static void t_cidr(void)
{
    int v6 = -1;
    unsigned c = 99;
    uint8_t a[16];
    CHECK(cidr("10.0.0.0/8", &v6, &c) == 0 && !v6 && c == 8, "v4/8");
    CHECK(cidr("10.0.0.1", &v6, &c) == 0 && c == 32, "v4 default /32");
    CHECK(cidr("0.0.0.0/0", &v6, &c) == 0 && c == 0, "v4 /0");
    CHECK(cidr("fd00::/8", &v6, &c) == 0 && v6 && c == 8, "v6/8");
    CHECK(cidr("::", &v6, &c) == 0 && v6 && c == 128, "v6 default /128");
    CHECK(cidr("::/0", &v6, &c) == 0 && c == 0, "v6 /0");
    CHECK(cidr("10.0.0.0/33", &v6, &c) < 0 && cidr("fd00::/129", &v6, &c) < 0, "prefix over the family length");
    CHECK(cidr("10.0.0.0/", &v6, &c) < 0 && cidr("10.0.0.0/ 8", &v6, &c) < 0 && cidr("10.0.0.0/-1", &v6, &c) < 0 && cidr("10.0.0.0/8x", &v6, &c) < 0, "bad prefix text");
    CHECK(cidr("10.0.0.256/8", &v6, &c) < 0 && cidr("10.0.0/8", &v6, &c) < 0 && cidr("", &v6, &c) < 0 && cidr(" 10.0.0.1", &v6, &c) < 0, "bad address");
    CHECK(wg_conf_ip("10.1.2.3", 8, &v6, a) == 0 && a[0] == 10 && a[3] == 3 && !a[4], "ip bytes, the tail zeroed");
    CHECK(wg_conf_ip("10.1.2.3/8", 10, &v6, a) < 0, "ip refuses a prefix");
    CHECK(wg_conf_ip("1234567890123456789012345678901234567890123456789", 49, &v6, a) < 0, "over-long literal");
}

static int ep(const char* s, int* v6, uint16_t* port)
{
    uint8_t a[16];
    return wg_conf_endpoint(s, strlen(s), v6, a, port);
}

static void t_endpoint(void)
{
    int v6 = -1;
    uint16_t p = 0;
    CHECK(ep("192.0.2.1:51820", &v6, &p) == 0 && !v6 && p == 51820, "v4 endpoint");
    CHECK(ep("[2001:db8::1]:443", &v6, &p) == 0 && v6 && p == 443, "v6 endpoint");
    CHECK(ep("192.0.2.1:65535", &v6, &p) == 0 && p == 65535, "port 65535");
    CHECK(ep("192.0.2.1:0", &v6, &p) < 0 && ep("192.0.2.1:65536", &v6, &p) < 0, "port 0 and 65536");
    CHECK(ep("192.0.2.1", &v6, &p) < 0 && ep("192.0.2.1:", &v6, &p) < 0, "missing port");
    CHECK(ep("2001:db8::1:443", &v6, &p) < 0 && ep("[192.0.2.1]:443", &v6, &p) < 0 && ep("[::1]", &v6, &p) < 0 && ep("[::1]443", &v6, &p) < 0, "v6 needs brackets, brackets need v6");
    CHECK(ep("host.test:443", &v6, &p) < 0, "names are not resolved here");
}

static void t_uint_list(void)
{
    unsigned u = 0;
    size_t off = 0, in;
    const char *it, *s = " a , ,b,, c d ,";
    char got[64] = "";
    CHECK(wg_conf_uint("25", 2, 65535, &u) == 0 && u == 25, "25");
    CHECK(wg_conf_uint("007", 3, 65535, &u) == 0 && u == 7, "leading zeros");
    CHECK(wg_conf_uint("65536", 5, 65535, &u) < 0 && wg_conf_uint("99999999999", 11, 0xffffffffu, &u) < 0, "over max, over 10 digits");
    CHECK(wg_conf_uint("", 0, 9, &u) < 0 && wg_conf_uint("+1", 2, 9, &u) < 0 && wg_conf_uint("1 ", 2, 9, &u) < 0, "digits only");
    while (wg_conf_next(s, strlen(s), &off, &it, &in)) {
        strncat(got, it, in);
        strcat(got, "|");
    }
    CHECK(!strcmp(got, "a|b|c d|"), "list trims and skips empty items");
}

typedef struct {
    char log[512];
    int veto_line;
} cap_t;

static int cap_cb(void* ud, int sec, int key, const char* v, size_t vn, unsigned line)
{
    cap_t* c = (cap_t*)ud;
    char b[96];
    if ((int)line == c->veto_line)
        return -1;
    snprintf(b, sizeof b, "%u:%c%d=%.*s;", line, sec == WG_CONF_PEER ? 'P' : 'I', key, (int)vn, v ? v : "");
    strncat(c->log, b, sizeof c->log - strlen(c->log) - 1);
    return 0;
}

static const char* scan_err(const char* s, unsigned* line)
{
    wg_conf_err_t e;
    memset(&e, 0, sizeof e);
    if (wg_conf_scan(s, strlen(s), NULL, NULL, &e) == 0)
        return NULL;
    *line = e.line;
    return e.msg;
}

static void t_scan(void)
{
    static const char good[] = "# c\r\n[ interface ]\r\nPrivateKey = abc # tail\r\nADDRESS=1.2.3.4/24\naddress = ::1\nDNS =\n\n[Peer]\nPublicKey=x\nAllowedIPs = 0.0.0.0/0\n[PEER]\npublickey = y\n";
    static const struct {
        const char* txt;
        unsigned line;
        const char* msg;
    } bad[] = {
        { "[Interface]\nFoo = 1", 2, "unknown key" },
        { "PrivateKey = x", 1, "key outside a section" },
        { "[Interface]\nListenPort = 1\nlistenport = 2", 3, "duplicate key" },
        { "[Interface]\n[Interface]", 2, "duplicate [Interface]" },
        { "[Wat]", 1, "unknown section" },
        { "[Peer", 1, "unterminated section header" },
        { "[Peer]\nPublicKey", 2, "expected key = value" },
        { "[Peer]\nPublicKey =   # nothing", 2, "empty value" },
        { "[Peer]\nPrivateKey = x", 2, "unknown key" },
        { "[Interface]\nPublicKey = x", 2, "unknown key" },
        { "[Peer]\nEndpoint = a\n[Peer]\nEndpoint = b\nendpoint = c", 5, "duplicate key" },
    };
    cap_t c;
    wg_conf_err_t e;
    char big[5000];
    unsigned i, line = 0;
    int bad_n = 0;
    memset(&c, 0, sizeof c);
    c.veto_line = -1;
    CHECK(wg_conf_scan(good, strlen(good), cap_cb, &c, &e) == 0, "good config scans");
    CHECK(!strcmp(c.log, "2:I10=;3:I0=abc;4:I2=1.2.3.4/24;5:I2=::1;6:I9=;8:P10=;9:P4=x;10:P7=0.0.0.0/0;11:P10=;12:P4=y;"), c.log);
    for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        const char* m = scan_err(bad[i].txt, &line);
        if (!m || strcmp(m, bad[i].msg) || line != bad[i].line) {
            printf("  case %u: %s line %u\n", i, m ? m : "(ok)", line);
            bad_n++;
        }
    }
    CHECK(!bad_n, "every error message with its line");
    memset(big, '#', sizeof big);
    memcpy(big, "[Interface]\n", 12);
    big[sizeof big - 1] = 0;
    CHECK(scan_err(big, &line) && line == 2, "line longer than 4096 bytes");
    CHECK(wg_conf_scan("[Peer]\0x", 8, NULL, NULL, &e) < 0 && e.line == 0 && !strcmp(e.msg, "NUL byte in the text"), "NUL byte");
    memset(&c, 0, sizeof c);
    c.veto_line = 3;
    memset(&e, 0, sizeof e);
    CHECK(wg_conf_scan(good, strlen(good), cap_cb, &c, &e) < 0 && e.line == 3 && !strcmp(e.msg, "rejected value"), "callback veto stops the scan at its line");
    CHECK(wg_conf_scan("", 0, NULL, NULL, &e) == 0 && wg_conf_scan("\n\n# x\n", 6, NULL, NULL, &e) == 0, "empty and comment-only text");
}

static void t_fuzz(void)
{
    static const char seed[] = "[Interface]\nPrivateKey = yAnz5TF+lXXJte14tji3zlMNq+hd2rYUIgJBgB3fBmk=\nListenPort = 51820\nAddress = 10.0.0.1/24, fd00::1\n[Peer]\nPublicKey = xTIBA5rboUvnH4htodjb6e697QjLERt1NAB4mZqp8Dg=\nEndpoint = [2001:db8::1]:51820\nAllowedIPs = 0.0.0.0/0, ::/0\nPersistentKeepalive = 25\n";
    uint8_t b[600];
    int i, j;
    for (i = 0; i < 20000; i++) {
        size_t n = rnd() % sizeof b;
        for (j = 0; j < (int)n; j++)
            b[j] = (uint8_t)(rnd() % 4 ? " =[],#\n:/.0123456789abcdefABCDEF+="[rnd() % 33] : rnd());
        LLVMFuzzerTestOneInput(b, n);
    }
    for (i = 0; i < 20000; i++) {
        size_t n = sizeof seed - 1;
        memcpy(b, seed, n);
        for (j = 0; j < 1 + (int)(rnd() % 4); j++)
            b[rnd() % n] = (uint8_t)rnd();
        LLVMFuzzerTestOneInput(b, n);
    }
    CHECK(1, "fuzz entry: 40000 inputs without a sanitizer report or invariant abort");
}

typedef struct {
    uint8_t a[16];
    int v6;
    unsigned c;
} cc_t;

typedef struct {
    uint8_t pub[32], psk[32], ep[16];
    int has_psk, has_ep, ep6, has_ka;
    uint16_t port;
    unsigned ka, na;
    cc_t al[6];
} cpeer_t;

typedef struct {
    uint8_t priv[32];
    int has_port, has_mtu;
    unsigned port, mtu, na, np;
    cc_t ad[6];
    cpeer_t p[4];
} cmodel_t;

static int cm_list(const char* v, size_t vn, cc_t* out, unsigned* n)
{
    size_t off = 0, in;
    const char* it;
    while (wg_conf_next(v, vn, &off, &it, &in)) {
        if (*n >= 6 || wg_conf_cidr(it, in, &out[*n].v6, out[*n].a, &out[*n].c) < 0)
            return -1;
        (*n)++;
    }
    return 0;
}

static int cm_cb(void* ud, int sec, int key, const char* v, size_t vn, unsigned line)
{
    cmodel_t* m = (cmodel_t*)ud;
    cpeer_t* p = m->np ? &m->p[m->np - 1] : NULL;
    (void)line;
    if (key == WG_CK_SECTION) {
        if (sec == WG_CONF_PEER) {
            if (m->np >= 4)
                return -1;
            memset(&m->p[m->np++], 0, sizeof(cpeer_t));
        }
        return 0;
    }
    if (key == WG_CK_IGNORED)
        return 0;
    if (sec == WG_CONF_IFACE) {
        if (key == WG_CK_PRIVATEKEY)
            return wg_conf_key(v, vn, m->priv);
        if (key == WG_CK_LISTENPORT)
            return (m->has_port = 1, wg_conf_uint(v, vn, 65535, &m->port));
        if (key == WG_CK_MTU)
            return (m->has_mtu = 1, wg_conf_uint(v, vn, 65535, &m->mtu));
        if (key == WG_CK_ADDRESS)
            return cm_list(v, vn, m->ad, &m->na);
        return -1;
    }
    if (!p)
        return -1;
    if (key == WG_CK_PUBLICKEY)
        return wg_conf_key(v, vn, p->pub);
    if (key == WG_CK_PRESHAREDKEY)
        return (p->has_psk = 1, wg_conf_key(v, vn, p->psk));
    if (key == WG_CK_ENDPOINT)
        return (p->has_ep = 1, wg_conf_endpoint(v, vn, &p->ep6, p->ep, &p->port));
    if (key == WG_CK_ALLOWEDIPS)
        return cm_list(v, vn, p->al, &p->na);
    if (key == WG_CK_KEEPALIVE)
        return (p->has_ka = 1, wg_conf_uint(v, vn, 65535, &p->ka));
    return -1;
}

static void cm_rand_cc(fk_rng_t* r, cc_t* c)
{
    memset(c, 0, sizeof *c);
    c->v6 = fk_chance(r, 40);
    fk_bytes(r, c->a, c->v6 ? 16 : 4);
    if (c->v6 && fk_chance(r, 60))
        memset(c->a + 2 + fk_below(r, 6), 0, 4 + fk_below(r, 6));
    c->c = fk_chance(r, 30) ? (c->v6 ? 128 : 32) : fk_below(r, c->v6 ? 129 : 33);
}

static void cm_rand(fk_rng_t* r, cmodel_t* m)
{
    unsigned i, k;
    memset(m, 0, sizeof *m);
    fk_bytes(r, m->priv, 32);
    if ((m->has_port = fk_chance(r, 70)))
        m->port = fk_below(r, 65536);
    if ((m->has_mtu = fk_chance(r, 30)))
        m->mtu = 576 + fk_below(r, 9000);
    m->na = fk_below(r, 4);
    for (k = 0; k < m->na; k++)
        cm_rand_cc(r, &m->ad[k]);
    m->np = fk_below(r, 5);
    for (i = 0; i < m->np; i++) {
        cpeer_t* p = &m->p[i];
        fk_bytes(r, p->pub, 32);
        if ((p->has_psk = fk_chance(r, 40)))
            fk_bytes(r, p->psk, 32);
        if ((p->has_ep = fk_chance(r, 60))) {
            p->ep6 = fk_chance(r, 40);
            fk_bytes(r, p->ep, p->ep6 ? 16 : 4);
            p->port = (uint16_t)(1 + fk_below(r, 65535));
        }
        if ((p->has_ka = fk_chance(r, 40)))
            p->ka = fk_below(r, 65536);
        p->na = fk_below(r, 6);
        for (k = 0; k < p->na; k++)
            cm_rand_cc(r, &p->al[k]);
    }
}

typedef struct {
    char t[16384];
    size_t n;
    unsigned line, kline[64], nk, kv[64];
} cbuf_t;

static void cb_line(fk_rng_t* r, cbuf_t* b, const char* s, int kv)
{
    if (r && fk_chance(r, 8))
        cb_line(NULL, b, fk_chance(r, 50) ? "" : "# note = [Peer]", -1);
    b->line++;
    if (kv >= 0 && b->nk < 64) {
        b->kline[b->nk] = b->line;
        b->kv[b->nk++] = (unsigned)kv;
    }
    b->n += (size_t)snprintf(b->t + b->n, sizeof b->t - b->n, "%s%s", s, r && fk_chance(r, 20) ? "\r\n" : "\n");
}

static const char* cm_case(fk_rng_t* r, const char* k, char* out)
{
    size_t i;
    unsigned mode = r ? fk_below(r, 3) : 0;
    for (i = 0; k[i]; i++)
        out[i] = mode == 1 ? (char)tolower((unsigned char)k[i]) : mode == 2 ? (char)toupper((unsigned char)k[i]) : k[i];
    out[i] = 0;
    return out;
}

static void cb_kv(fk_rng_t* r, cbuf_t* b, const char* key, const char* val, int scalar)
{
    static const char* const sep[] = { " = ", "=", "\t=\t", "  =" };
    char l[600], kc[32];
    snprintf(l, sizeof l, "%s%s%s%s%s", r && fk_chance(r, 15) ? " \t" : "", cm_case(r, key, kc), sep[r ? fk_below(r, 4) : 0], val,
        r && fk_chance(r, 15) ? " # tail" : "");
    cb_line(r, b, l, scalar);
}

static void cm_fmt_cc(const cc_t* c, int omit, char* out)
{
    char ip[48];
    wg_conf_fmt_ip(c->v6, c->a, ip);
    if (omit && c->c == (c->v6 ? 128u : 32u))
        snprintf(out, 64, "%s", ip);
    else
        snprintf(out, 64, "%s/%u", ip, c->c);
}

static void cb_list(fk_rng_t* r, cbuf_t* b, const char* key, const cc_t* c, unsigned n)
{
    char v[512], one[64];
    unsigned k = 0;
    while (k < n) {
        unsigned take = r ? 1 + fk_below(r, n - k) : n - k, j;
        v[0] = 0;
        for (j = 0; j < take; j++, k++) {
            cm_fmt_cc(&c[k], r && fk_chance(r, 50), one);
            if (j)
                strcat(v, r && fk_chance(r, 30) ? " ,  " : ", ");
            strcat(v, one);
        }
        cb_kv(r, b, key, v, 9);
    }
}

static void cm_render(fk_rng_t* r, const cmodel_t* m, cbuf_t* b)
{
    static const char* const ih[] = { "[Interface]", "[ interface ]", "[INTERFACE]" }, *const ph[] = { "[Peer]", "[ peer ]", "[PEER]" };
    char v[96], ip[48];
    unsigned i;
    b->n = 0;
    b->line = 0;
    b->nk = 0;
    if (r && fk_chance(r, 30))
        cb_line(NULL, b, "# generated", -1);
    cb_line(r, b, ih[r ? fk_below(r, 3) : 0], 8);
    wg_conf_key_b64(m->priv, v);
    cb_kv(r, b, "PrivateKey", v, 0);
    if (m->has_port) {
        snprintf(v, sizeof v, "%u", m->port);
        cb_kv(r, b, "ListenPort", v, 1);
    }
    if (m->has_mtu) {
        snprintf(v, sizeof v, "%u", m->mtu);
        cb_kv(r, b, "MTU", v, 1);
    }
    cb_list(r, b, "Address", m->ad, m->na);
    if (r && fk_chance(r, 20))
        cb_kv(r, b, "DNS", fk_chance(r, 50) ? "" : "1.1.1.1", -1);
    for (i = 0; i < m->np; i++) {
        const cpeer_t* p = &m->p[i];
        cb_line(r, b, ph[r ? fk_below(r, 3) : 0], 8);
        wg_conf_key_b64(p->pub, v);
        cb_kv(r, b, "PublicKey", v, 0);
        if (p->has_psk) {
            wg_conf_key_b64(p->psk, v);
            cb_kv(r, b, "PresharedKey", v, 0);
        }
        if (p->has_ep) {
            wg_conf_fmt_ip(p->ep6, p->ep, ip);
            snprintf(v, sizeof v, p->ep6 ? "[%s]:%u" : "%s:%u", ip, p->port);
            cb_kv(r, b, "Endpoint", v, 1);
        }
        if (p->has_ka) {
            snprintf(v, sizeof v, "%u", p->ka);
            cb_kv(r, b, "PersistentKeepalive", v, 1);
        }
        cb_list(r, b, "AllowedIPs", p->al, p->na);
    }
}

static int cc_eq(const cc_t* a, const cc_t* b, unsigned n)
{
    unsigned k;
    for (k = 0; k < n; k++)
        if (a[k].v6 != b[k].v6 || a[k].c != b[k].c || memcmp(a[k].a, b[k].a, a[k].v6 ? 16 : 4))
            return 0;
    return 1;
}

static int cm_eq(const cmodel_t* a, const cmodel_t* b)
{
    unsigned i;
    if (memcmp(a->priv, b->priv, 32) || a->has_port != b->has_port || (a->has_port && a->port != b->port) || a->has_mtu != b->has_mtu
        || (a->has_mtu && a->mtu != b->mtu) || a->na != b->na || !cc_eq(a->ad, b->ad, a->na) || a->np != b->np)
        return 0;
    for (i = 0; i < a->np; i++) {
        const cpeer_t *p = &a->p[i], *q = &b->p[i];
        if (memcmp(p->pub, q->pub, 32) || p->has_psk != q->has_psk || (p->has_psk && memcmp(p->psk, q->psk, 32)) || p->has_ep != q->has_ep
            || (p->has_ep && (p->ep6 != q->ep6 || p->port != q->port || memcmp(p->ep, q->ep, p->ep6 ? 16 : 4))) || p->has_ka != q->has_ka
            || (p->has_ka && p->ka != q->ka) || p->na != q->na || !cc_eq(p->al, q->al, p->na))
            return 0;
    }
    return 1;
}

static void p_conf_roundtrip(void)
{
    static cbuf_t b, c;
    fk_rng_t r;
    cmodel_t m, g;
    wg_conf_err_t e;
    unsigned i, n, bad = 0;
    if (!fk_only("conf-roundtrip"))
        return;
    r = fk_stream("conf-roundtrip");
    n = (unsigned)fk_iters(6000);
    for (i = 0; i < n; i++) {
        int rc;
        cm_rand(&r, &m);
        cm_render(&r, &m, &b);
        memset(&g, 0, sizeof g);
        memset(&e, 0, sizeof e);
        rc = wg_conf_scan(b.t, b.n, cm_cb, &g, &e);
        if (rc != 0 || !cm_eq(&m, &g)) {
            if (bad++ < 3)
                printf("  conf-roundtrip: rc %d line %u %s\n%s", rc, e.line, e.msg ? e.msg : "", b.t);
            continue;
        }
        cm_render(NULL, &g, &c);
        cm_render(NULL, &m, &b);
        bad += c.n != b.n || memcmp(c.t, b.t, b.n);
    }
    CHECK(bad == 0, "conf-roundtrip: random configs rendered with random case, separators, comments, blank lines, CRLF and split lists scan back to the same model; canonical render is idempotent");
}

static void p_conf_faults(void)
{
    static cbuf_t b;
    static char t[20000];
    fk_rng_t r;
    cmodel_t m, g;
    wg_conf_err_t e;
    unsigned i, n, bad = 0, tried = 0;
    if (!fk_only("conf-faults"))
        return;
    r = fk_stream("conf-faults");
    n = (unsigned)fk_iters(6000);
    for (i = 0; i < n; i++) {
        unsigned k, ln, kind = fk_below(&r, 5), cur = 0, wl;
        size_t o = 0, tn = 0;
        const char* want;
        cm_rand(&r, &m);
        cm_render(fk_chance(&r, 50) ? &r : NULL, &m, &b);
        k = fk_below(&r, b.nk);
        if (kind == 4 && b.kv[k] != 8)
            kind = 0;
        if (kind != 4 && b.kv[k] == 8)
            continue;
        if (kind == 2 && b.kv[k] != 0 && b.kv[k] != 1)
            kind = 3;
        if (kind == 1 && b.kv[k] == 0)
            kind = 0;
        ln = b.kline[k];
        wl = kind == 2 ? ln + 1 : ln;
        want = kind == 0 ? "unknown key" : kind == 1 ? "expected key = value" : kind == 2 ? "duplicate key" : kind == 3 ? "rejected value" : "unknown section";
        while (o < b.n) {
            const char* nl = memchr(b.t + o, '\n', b.n - o);
            size_t len = (size_t)(nl - (b.t + o)) + 1, a = 0;
            char l[700];
            memcpy(l, b.t + o, len);
            l[len] = 0;
            o += len;
            cur++;
            if (cur == ln) {
                char* eq = strchr(l, '=');
                while (l[a] == ' ' || l[a] == '\t')
                    a++;
                if (kind == 0)
                    l[a] = 'Z';
                else if (kind == 1 && eq)
                    *eq = ' ';
                else if (kind == 3) {
                    char* h = strchr(l, '#');
                    size_t vend = h ? (size_t)(h - l) : strcspn(l, "\r\n");
                    while (vend > 0 && (l[vend - 1] == ' ' || l[vend - 1] == '\t'))
                        vend--;
                    l[vend - 1] = '!';
                } else if (kind == 4)
                    memcpy(l + strcspn(l, "IiPp"), "Wat]\n", 6);
            }
            memcpy(t + tn, l, strlen(l));
            tn += strlen(l);
            if (cur == ln && kind == 2) {
                memcpy(t + tn, l, strlen(l));
                tn += strlen(l);
            }
        }
        tried++;
        memset(&g, 0, sizeof g);
        memset(&e, 0, sizeof e);
        if (wg_conf_scan(t, tn, cm_cb, &g, &e) == 0 || e.line != wl || !e.msg || strcmp(e.msg, want)) {
            if (bad++ < 3)
                printf("  conf-faults: kind %u line %u: got line %u \"%s\"\n%.*s", kind, wl, e.line, e.msg ? e.msg : "(ok)", (int)tn, t);
        }
    }
    CHECK(bad == 0 && tried > n / 2, "conf-faults: one injected fault (unknown key, missing '=', duplicated scalar line, value the callback rejects, unknown section) fails with that message at that line");
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    fk_init("test_wg_conf");
    if (fk_legacy()) {
        t_keys();
        t_cidr();
        t_endpoint();
        t_uint_list();
        t_scan();
        t_fuzz();
    }
    p_conf_roundtrip();
    p_conf_faults();
    printf("test_wg_conf: %d checks, %d failed\n", checks, fails);
    if (fails)
        printf("replay: DYN_FUZZ_SEED=%llu\n", (unsigned long long)fk_seed);
    return fails ? 1 : 0;
}
