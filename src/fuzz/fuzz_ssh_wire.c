#include "modules/ssh/ssh-internal.h"

#include <arpa/inet.h>
#include <openssl/evp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

static void fz_die(int line)
{
    fprintf(stderr, "fuzz_ssh_wire: oracle failed at line %d\n", line);
    abort();
}

#define FZ(c) \
    do { \
        if (!(c)) \
            fz_die(__LINE__); \
    } while (0)

typedef struct {
    const uint8_t* p;
    size_t n, i;
} in_t;

static unsigned in_u8(in_t* s)
{
    return s->i < s->n ? s->p[s->i++] : 0;
}

static size_t in_take(in_t* s, size_t want, const uint8_t** out)
{
    size_t k = s->n - s->i < want ? s->n - s->i : want;
    *out = s->p + s->i;
    s->i += k;
    return k;
}

static uint32_t nbe32(const uint8_t* p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static void m_reader(const uint8_t* d, size_t n, const uint8_t* ops, size_t on)
{
    sshr_t r;
    size_t off = 0, k;
    int err = 0;
    sshr_init(&r, d, n);
    for (k = 0; k < on; k++) {
        unsigned op = ops[k] % 4;
        if (op == 0) {
            uint8_t v = sshr_u8(&r);
            uint8_t ev = 0;
            if (!err && n - off >= 1)
                ev = d[off++];
            else
                err = 1;
            FZ(v == ev);
        } else if (op == 1) {
            uint32_t v = sshr_u32(&r), ev = 0;
            if (!err && n - off >= 4) {
                ev = nbe32(d + off);
                off += 4;
            } else
                err = 1;
            FZ(v == ev);
        } else if (op == 2) {
            size_t ln = 99;
            const uint8_t* p = sshr_str(&r, &ln);
            if (!err && n - off >= 4 && n - off - 4 >= nbe32(d + off)) {
                uint32_t l = nbe32(d + off);
                FZ(p == d + off + 4 && ln == l);
                off += 4 + l;
            } else {
                if (!err && n - off >= 4)
                    off += 4;
                err = 1;
                FZ(p == NULL && ln == 0);
            }
        } else {
            int v = sshr_bool(&r), ev = 0;
            if (!err && n - off >= 1)
                ev = d[off++] != 0;
            else
                err = 1;
            FZ(v == ev);
        }
        FZ(!!r.err == err);
        if (!err)
            FZ(r.off == off);
        FZ(r.off <= r.len);
    }
}

static int nb64v(int c)
{
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const char* q = c ? strchr(t, c) : NULL;
    return q ? (int)(q - t) : -1;
}

static size_t n_b64(const char* s, size_t n, uint8_t* out, size_t cap)
{
    char core[4096];
    size_t cn = 0, pads = 0, i, o = 0, b;
    for (i = 0; i < n; i++) {
        char c = s[i];
        if (c == '\n' || c == '\r' || c == ' ' || c == '\t')
            continue;
        if (c == '=') {
            pads++;
            continue;
        }
        if (pads || nb64v(c) < 0)
            return (size_t)-1;
        core[cn++] = c;
    }
    if (pads > 2 || cn % 4 == 1)
        return (size_t)-1;
    for (b = 0; b + 8 <= cn * 6; b += 8) {
        unsigned v = 0, j;
        for (j = 0; j < 8; j++) {
            size_t bit = b + j;
            v = v << 1 | (unsigned)((nb64v(core[bit / 6]) >> (5 - bit % 6)) & 1);
        }
        if (o >= cap)
            return (size_t)-1;
        out[o++] = (uint8_t)v;
    }
    return o;
}

static void m_b64(const uint8_t* d, size_t n, unsigned sel)
{
    uint8_t a[4096], b[4096];
    char e[6000];
    size_t cap = sel & 1 ? (n * 3 / 4) / 2 : sizeof a, x, y, en;
    if (n > 4000)
        n = 4000;
    x = ssh_b64_decode((const char*)d, n, a, cap);
    y = n_b64((const char*)d, n, b, cap);
    FZ(x == y);
    if (x != (size_t)-1)
        FZ(memcmp(a, b, x) == 0);
    en = ssh_b64_encode(d, n, e, (int)(sel >> 1 & 1));
    FZ(en == ((sel >> 1 & 1) ? (n + 2) / 3 * 4 : (n * 8 + 5) / 6));
    x = ssh_b64_decode(e, en, a, sizeof a);
    FZ(x == n && memcmp(a, d, n) == 0);
}

static int n_nl(const uint8_t* l, size_t n, const char* name)
{
    size_t nl = strlen(name), s = 0, i;
    for (i = 0; i <= n; i++)
        if (i == n || l[i] == ',') {
            if (i - s == nl && memcmp(l + s, name, nl) == 0)
                return 1;
            s = i + 1;
        }
    return 0;
}

static void m_namelist(in_t* s)
{
    const uint8_t* name;
    size_t nn = in_take(s, in_u8(s) % 24, &name), k;
    char nm[32];
    uint8_t list[512];
    size_t ln = 0;
    memcpy(nm, name, nn);
    nm[nn] = 0;
    for (k = 0; k < nn; k++)
        if (!nm[k])
            nm[k] = 'x';
    while (s->i < s->n && ln + 40 < sizeof list) {
        unsigned op = in_u8(s) % 5;
        if (op == 0 && ln + nn < sizeof list) {
            memcpy(list + ln, nm, nn);
            ln += nn;
        } else if (op == 1)
            list[ln++] = ',';
        else if (op == 2 && nn) {
            size_t c = in_u8(s) % nn;
            memcpy(list + ln, nm, c);
            ln += c;
        } else
            list[ln++] = (uint8_t)in_u8(s);
    }
    FZ(ssh_namelist_has(list, ln, nm) == n_nl(list, ln, nm));
}

static void m_writer(in_t* s)
{
    sshb_t b = { 0 };
    uint8_t kind[64];
    const uint8_t* ptr[64];
    size_t len[64];
    uint32_t val[64];
    size_t k = 0, i;
    sshr_t r;
    while (s->i < s->n && k < 64) {
        unsigned op = in_u8(s) % 4;
        kind[k] = (uint8_t)op;
        if (op == 0) {
            val[k] = in_u8(s);
            FZ(sshb_u8(&b, (uint8_t)val[k]) == 0);
        } else if (op == 1) {
            val[k] = in_u8(s) << 24 | in_u8(s) << 16 | in_u8(s) << 8 | in_u8(s);
            FZ(sshb_u32(&b, val[k]) == 0);
        } else {
            len[k] = in_take(s, in_u8(s) % 64, &ptr[k]);
            FZ((op == 2 ? sshb_str(&b, ptr[k], len[k]) : sshb_mpint(&b, ptr[k], len[k])) == 0);
        }
        k++;
    }
    sshr_init(&r, b.p, b.len);
    for (i = 0; i < k; i++) {
        if (kind[i] == 0)
            FZ(sshr_u8(&r) == val[i]);
        else if (kind[i] == 1)
            FZ(sshr_u32(&r) == val[i]);
        else {
            size_t gn;
            const uint8_t* g = sshr_str(&r, &gn);
            FZ(!r.err);
            if (kind[i] == 2)
                FZ(gn == len[i] && (gn == 0 || memcmp(g, ptr[i], gn) == 0));
            else {
                const uint8_t* p = ptr[i];
                size_t pn = len[i];
                while (pn && !*p) {
                    p++;
                    pn--;
                }
                FZ(gn == pn + (pn && (p[0] & 0x80)));
                if (gn > pn)
                    FZ(g[0] == 0 && memcmp(g + 1, p, pn) == 0);
                else if (gn)
                    FZ(g[0] && !(g[0] & 0x80) && memcmp(g, p, pn) == 0);
            }
        }
    }
    FZ(!r.err && r.off == r.len);
    sshb_free(&b);
}

static EVP_PKEY* ca_key;
static uint8_t ca_pub[32];

static int n_addr(const char* peer, const uint8_t* e, size_t en)
{
    char buf[80];
    unsigned char a[16], b[16];
    int six, maxb, bits, i;
    const char* sl;
    if (en == 0 || en >= sizeof buf)
        return 0;
    memcpy(buf, e, en);
    buf[en] = 0;
    six = strchr(buf, ':') != NULL;
    maxb = six ? 128 : 32;
    bits = maxb;
    sl = strchr(buf, '/');
    if (sl) {
        const char* q = sl + 1;
        long v = 0;
        int neg = 0;
        while (*q == ' ' || (*q >= '\t' && *q <= '\r'))
            q++;
        if (*q == '+' || *q == '-')
            neg = *q++ == '-';
        if (!(*q >= '0' && *q <= '9') && sl[1])
            return 0;
        while (*q >= '0' && *q <= '9' && v <= 1000)
            v = v * 10 + (*q++ - '0');
        if (*q || (neg && v) || v > maxb)
            return 0;
        bits = (int)v;
        buf[sl - buf] = 0;
    }
    if (inet_pton(six ? AF_INET6 : AF_INET, buf, a) != 1 || inet_pton(six ? AF_INET6 : AF_INET, peer, b) != 1)
        return 0;
    for (i = 0; i < bits; i++)
        if ((a[i / 8] >> (7 - i % 8) & 1) != (b[i / 8] >> (7 - i % 8) & 1))
            return 0;
    return 1;
}

static int n_src(const char* peer, const uint8_t* l, size_t n)
{
    size_t s = 0, i;
    for (i = 0; i <= n; i++)
        if (i == n || l[i] == ',') {
            if (n_addr(peer, l + s, i - s))
                    return 1;
            s = i + 1;
        }
    return 0;
}

static const char* const OPTN[] = { "force-command", "source-address", "permit-pty", "no-touch-required", "x" };
static const char* const PEERS[] = { "10.0.0.1", "10.0.0.200", "192.168.1.7", "fd00::1", "fd00::ff:1", "::1", "bogus" };

static void m_cert(in_t* s)
{
    sshb_t b = { 0 }, crit = { 0 }, ext = { 0 }, pr = { 0 }, ca = { 0 }, sig = { 0 };
    uint8_t pk[32] = { 0 }, sv[64];
    char prin[24], why[256], *fc = NULL;
    const uint8_t* q;
    uint64_t now = (uint64_t)time(NULL), va, vb;
    uint32_t ctype = in_u8(s) % 3 + 1 == 3 ? 7 : in_u8(s) % 2 + 1, wtype = in_u8(s) % 2 + 1;
    unsigned np = in_u8(s) % 4, nc = in_u8(s) % 4, i, sel = in_u8(s);
    int pty = -1, rc, exp_ok = 1, fc_set = 0, has_pty = 0, flip = 0;
    unsigned pv = in_u8(s);
    const char* peer = pv % 8 == 7 ? NULL : PEERS[pv % 7];
    char efc[80];
    ssh_cert_t c;
    size_t pn = in_take(s, in_u8(s) % 12, &q), k;
    memcpy(prin, q, pn);
    prin[pn] = 0;
    for (k = 0; k < pn; k++)
        if (!prin[k])
            prin[k] = 'p';
    va = sel & 1 ? now + 1000 : now - 1000;
    vb = sel & 2 ? now - 1 : (sel & 4 ? now + 100 : UINT64_MAX);
    if (sel & 8)
        va = 0;
    for (i = 0; i < np; i++) {
        size_t ln = in_take(s, in_u8(s) % 12, &q);
        if (in_u8(s) & 1)
            sshb_str(&pr, prin, pn);
        else
            sshb_str(&pr, q, ln);
    }
    if (in_u8(s) % 5 == 0)
        sshb_u8(&pr, 0);
    efc[0] = 0;
    for (i = 0; i < nc; i++) {
        unsigned o = in_u8(s) % 5;
        sshb_t dv = { 0 };
        char gb[64];
        size_t ln = in_take(s, in_u8(s) % 40, &q);
        if (o == 1 && in_u8(s) % 2) {
            unsigned g = in_u8(s);
            ln = (size_t)snprintf(gb, sizeof gb, "%s%s/%u", g & 1 ? "" : "1.2.3.4,", PEERS[g % 6], in_u8(s) % 136);
            q = (const uint8_t*)gb;
        }
        int raw = in_u8(s) % 6 == 0;
        sshb_cstr(&crit, OPTN[o]);
        if (raw)
            sshb_str(&crit, q, ln);
        else {
            sshb_str(&dv, q, ln);
            sshb_str(&crit, dv.p, dv.len);
        }
        sshb_free(&dv);
        if (!exp_ok)
            continue;
        if (wtype == SSH_CERT_USER && o == 0 && !(raw && (ln < 4 || nbe32(q) > ln - 4))) {
            sshr_t rr;
            size_t vn;
            const uint8_t* v;
            sshr_init(&rr, q, ln);
            v = raw ? sshr_str(&rr, &vn) : q;
            if (!raw)
                vn = ln;
            snprintf(efc, sizeof efc, "%.*s", (int)vn, v ? (const char*)v : "");
            fc_set = memchr(v ? v : (const uint8_t*)"", 0, vn) ? 2 : 1;
            continue;
        }
        if (wtype == SSH_CERT_USER && o == 1 && !(raw && (ln < 4 || nbe32(q) > ln - 4))) {
            const uint8_t* v = raw ? q + 4 : q;
            size_t vn = raw ? nbe32(q) : ln;
            if (!peer || !n_src(peer, v, vn))
                exp_ok = 0;
            continue;
        }
        exp_ok = 0;
    }
    if (in_u8(s) % 7 == 0) {
        sshb_u8(&crit, 1);
        exp_ok = 0;
    }
    for (i = in_u8(s) % 3; i; i--) {
        unsigned o = in_u8(s) % 5;
        sshb_cstr(&ext, OPTN[o]);
        sshb_str(&ext, "", 0);
        if (o == 2)
            has_pty = 1;
    }
    if (in_u8(s) % 9 == 0)
        sshb_u8(&ext, 2);
    for (k = 0; k < 32; k++)
        pk[k] = (uint8_t)in_u8(s);
    ssh_ed25519_blob(&ca, ca_pub);
    sshb_cstr(&b, "ssh-ed25519-cert-v01@openssh.com");
    sshb_str(&b, "nonce", 5);
    sshb_str(&b, pk, 32);
    sshb_u32(&b, 0);
    sshb_u32(&b, 7);
    sshb_u32(&b, ctype);
    sshb_cstr(&b, "id");
    sshb_str(&b, pr.p, pr.len);
    sshb_u32(&b, (uint32_t)(va >> 32));
    sshb_u32(&b, (uint32_t)va);
    sshb_u32(&b, (uint32_t)(vb >> 32));
    sshb_u32(&b, (uint32_t)vb);
    sshb_str(&b, crit.p, crit.len);
    sshb_str(&b, ext.p, ext.len);
    sshb_str(&b, "", 0);
    sshb_str(&b, ca.p, ca.len);
    FZ(ssh_ed25519_sign(ca_key, b.p, b.len, sv) == 0);
    if (in_u8(s) % 11 == 0 && (flip = 1))
        sv[in_u8(s) % 64] ^= (uint8_t)(1u << (in_u8(s) % 8));
    sshb_cstr(&sig, "ssh-ed25519");
    sshb_str(&sig, sv, 64);
    sshb_str(&b, sig.p, sig.len);
    rc = ssh_cert_parse(b.p, b.len, &c, why, sizeof why);
    FZ((rc == 0) == !flip);
    if (rc == 0) {
        sshr_t pp;
        int ph = 0;
        FZ(memcmp(c.pk, pk, 32) == 0 && c.type == ctype && c.valid_after == va && c.valid_before == vb && c.serial == 7);
        FZ(c.crit_len == crit.len && c.ext_len == ext.len && c.principals_len == pr.len);
        sshr_init(&pp, pr.p, pr.len);
        while (pp.off < pp.len) {
            size_t ln;
            const uint8_t* p = sshr_str(&pp, &ln);
            if (pp.err)
                break;
            if (ln == pn && memcmp(p, prin, pn) == 0)
                ph = 1;
        }
        if (ctype != wtype || now < va || now >= vb || !ph)
            exp_ok = 0;
        rc = ssh_cert_check(&c, wtype, prin, peer, &fc, &pty, why, sizeof why);
        FZ((rc == 0) == exp_ok);
        if (rc == 0) {
            FZ(pty == has_pty);
            FZ(fc_set ? fc != NULL : fc == NULL);
            if (fc_set == 1)
                FZ(strcmp(fc, efc) == 0);
        } else
            FZ(fc == NULL);
        free(fc);
    } else
        FZ(memcmp(why, "the certificate's CA", 20) == 0 || memcmp(why, "malformed", 9) == 0);
    sshb_free(&b);
    sshb_free(&crit);
    sshb_free(&ext);
    sshb_free(&pr);
    sshb_free(&ca);
    sshb_free(&sig);
}

static void m_pubkey(const uint8_t* d, size_t n)
{
    sshb_t out = { 0 }, lst = { 0 };
    char ty[64], why[128];
    int rc = ssh_parse_pubkey((const char*)d, n, &out, ty, sizeof ty);
    if (rc == 0) {
        sshr_t r;
        size_t tn;
        const uint8_t* t;
        sshr_init(&r, out.p, out.len);
        t = sshr_str(&r, &tn);
        FZ(!r.err && tn == strlen(ty) && memcmp(t, ty, tn) == 0);
        FZ(strcmp(ty, "ssh-ed25519") == 0 || strcmp(ty, "sk-ssh-ed25519@openssh.com") == 0 || strcmp(ty, "ssh-ed25519-cert-v01@openssh.com") == 0);
        {
            size_t a = 0;
            while (a < n && (d[a] == ' ' || d[a] == '\t'))
                a++;
            FZ(n - a >= tn && memcmp(d + a, ty, tn) == 0 && (n - a == tn || d[a + tn] == ' ' || d[a + tn] == '\t'));
        }
    }
    (void)ssh_parse_pubkey_lines((const char*)d, n, &lst, why, sizeof why);
    sshb_free(&out);
    sshb_free(&lst);
}

static void m_pubgen(in_t* s)
{
    static const char* const TY[] = { "ssh-ed25519", "sk-ssh-ed25519@openssh.com", "ssh-ed25519-cert-v01@openssh.com" };
    sshb_t blob = { 0 }, out = { 0 };
    char line[512], ty[64];
    const uint8_t* q;
    unsigned t = in_u8(s) % 3, sp = in_u8(s);
    size_t ln = in_take(s, in_u8(s) % 96, &q), o = 0, en;
    if (sp & 32) {
        char ib[48];
        sshb_str(&blob, ib, (size_t)snprintf(ib, sizeof ib, "%s%c", TY[t], 'a' + sp % 26));
    } else
        sshb_cstr(&blob, TY[t]);
    sshb_put(&blob, q, ln);
    o += (size_t)snprintf(line, sizeof line, "%s%s%s", sp & 1 ? " \t" : "", TY[t], sp & 2 ? "\t " : " ");
    en = ssh_b64_encode(blob.p, blob.len, line + o, (int)(sp >> 2 & 1));
    o += en;
    o += (size_t)snprintf(line + o, sizeof line - o, "%s", sp & 8 ? " comment here" : (sp & 16 ? "\r\n" : ""));
    if (sp & 32)
        FZ(ssh_parse_pubkey(line, o, &out, ty, sizeof ty) == -1);
    else {
        FZ(ssh_parse_pubkey(line, o, &out, ty, sizeof ty) == 0);
        FZ(strcmp(ty, TY[t]) == 0 && out.len == blob.len && memcmp(out.p, blob.p, blob.len) == 0);
    }
    sshb_free(&blob);
    sshb_free(&out);
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    in_t s;
    if (!ca_key) {
        uint8_t seed[32];
        size_t pl = 32;
        memset(seed, 0x5a, sizeof seed);
        ca_key = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL, seed, sizeof seed);
        if (!ca_key || EVP_PKEY_get_raw_public_key(ca_key, ca_pub, &pl) != 1)
            abort();
    }
    if (size < 1)
        return 0;
    s.p = data + 1;
    s.n = size - 1;
    s.i = 0;
    switch (data[0] % 7) {
    case 0: {
        size_t on = s.n ? s.p[0] % 48 : 0;
        if (on + 1 > s.n)
            on = s.n ? s.n - 1 : 0;
        if (s.n)
            m_reader(s.p + 1 + on, s.n - 1 - on, s.p + 1, on);
        break;
    }
    case 1:
        m_b64(s.p, s.n, data[0] / 7);
        break;
    case 2:
        m_namelist(&s);
        break;
    case 3:
        m_writer(&s);
        break;
    case 4:
        m_cert(&s);
        break;
    case 5:
        m_pubkey(s.p, s.n);
        break;
    default:
        m_pubgen(&s);
        break;
    }
    return 0;
}
