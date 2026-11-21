#include "ssh-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_SSH)

#include <stdio.h>
#include <time.h>
#include <arpa/inet.h>

#define SSH_PUBLINE_MAX 16384

static int str_is(const uint8_t* p, size_t n, const char* s)
{
    return p && n == strlen(s) && memcmp(p, s, n) == 0;
}

static uint64_t rd_u64(sshr_t* r)
{
    uint64_t hi = sshr_u32(r);
    return hi << 32 | sshr_u32(r);
}

void ssh_ed25519_blob(sshb_t* b, const uint8_t pk[32])
{
    b->len = 0;
    if (sshb_cstr(b, "ssh-ed25519") < 0 || sshb_str(b, pk, 32) < 0)
        b->len = 0;
}

int ssh_cert_parse(const uint8_t* blob, size_t n, ssh_cert_t* c, char* why, size_t whylen)
{
    sshr_t r, k, s;
    const uint8_t *t, *pk, *sig, *kt, *kp, *st, *sv;
    size_t tn, pkn, sn, ktn, kpn, stn, svn, nn, rn, signed_len;
    memset(c, 0, sizeof *c);
    sshr_init(&r, blob, n);
    t = sshr_str(&r, &tn);
    (void)sshr_str(&r, &nn);
    pk = sshr_str(&r, &pkn);
    c->serial = rd_u64(&r);
    c->type = sshr_u32(&r);
    c->key_id = sshr_str(&r, &c->key_id_len);
    c->principals = sshr_str(&r, &c->principals_len);
    c->valid_after = rd_u64(&r);
    c->valid_before = rd_u64(&r);
    c->crit = sshr_str(&r, &c->crit_len);
    c->ext = sshr_str(&r, &c->ext_len);
    (void)sshr_str(&r, &rn);
    c->ca = sshr_str(&r, &c->ca_len);
    signed_len = r.off;
    sig = sshr_str(&r, &sn);
    if (r.err || r.off != r.len || !str_is(t, tn, "ssh-ed25519-cert-v01@openssh.com") || pkn != 32) {
        snprintf(why, whylen, "malformed ssh-ed25519-cert-v01@openssh.com certificate");
        return -1;
    }
    memcpy(c->pk, pk, 32);
    sshr_init(&k, c->ca, c->ca_len);
    kt = sshr_str(&k, &ktn);
    kp = sshr_str(&k, &kpn);
    if (k.err || k.off != k.len || !str_is(kt, ktn, "ssh-ed25519") || kpn != 32) {
        snprintf(why, whylen, "the certificate is signed by an unsupported CA key type (only ssh-ed25519 CAs)");
        return -1;
    }
    sshr_init(&s, sig, sn);
    st = sshr_str(&s, &stn);
    sv = sshr_str(&s, &svn);
    if (s.err || s.off != s.len || !str_is(st, stn, "ssh-ed25519")
        || !ssh_ed25519_verify(kp, sv, svn, blob, signed_len)) {
        snprintf(why, whylen, "the certificate's CA signature does not verify");
        return -1;
    }
    return 0;
}

static int principal_has(const ssh_cert_t* c, const char* name)
{
    sshr_t r;
    sshr_init(&r, c->principals, c->principals_len);
    while (r.off < r.len) {
        size_t pn;
        const uint8_t* p = sshr_str(&r, &pn);
        if (r.err)
            return 0;
        if (str_is(p, pn, name))
            return 1;
    }
    return 0;
}

int ssh_cert_ext_has(const ssh_cert_t* c, const char* name)
{
    sshr_t r;
    sshr_init(&r, c->ext, c->ext_len);
    while (r.off < r.len) {
        size_t nn, dn;
        const uint8_t* nm = sshr_str(&r, &nn);
        (void)sshr_str(&r, &dn);
        if (r.err)
            return 0;
        if (str_is(nm, nn, name))
            return 1;
    }
    return 0;
}

static int addr_in(const char* peer, const char* ent, size_t en)
{
    char buf[80];
    const char* slash;
    unsigned char a[16], b[16];
    int fam, bits, i, maxb;
    if (en == 0 || en >= sizeof buf)
        return 0;
    memcpy(buf, ent, en);
    buf[en] = 0;
    slash = strchr(buf, '/');
    fam = strchr(buf, ':') ? AF_INET6 : AF_INET;
    maxb = fam == AF_INET6 ? 128 : 32;
    bits = maxb;
    if (slash) {
        char* end;
        long v = strtol(slash + 1, &end, 10);
        if (*end || v < 0 || v > maxb)
            return 0;
        bits = (int)v;
        buf[slash - buf] = 0;
    }
    if (inet_pton(fam, buf, a) != 1 || inet_pton(fam, peer, b) != 1)
        return 0;
    for (i = 0; i < bits / 8; i++)
        if (a[i] != b[i])
            return 0;
    if (bits % 8) {
        unsigned char m = (unsigned char)(0xff << (8 - bits % 8));
        if ((a[i] & m) != (b[i] & m))
            return 0;
    }
    return 1;
}

static int source_ok(const char* peer, const uint8_t* list, size_t n)
{
    size_t i = 0;
    while (i < n) {
        size_t j = i;
        while (j < n && list[j] != ',')
            j++;
        if (addr_in(peer, (const char*)list + i, j - i))
            return 1;
        i = j + 1;
    }
    return 0;
}

int ssh_cert_check(const ssh_cert_t* c, uint32_t type, const char* principal,
    const char* peer, char** force_cmd, int* permit_pty, char* why, size_t whylen)
{
    uint64_t now = (uint64_t)time(NULL);
    sshr_t r;
    if (c->type != type) {
        snprintf(why, whylen, "the certificate is a %s certificate, not a %s certificate",
            c->type == SSH_CERT_USER ? "user" : c->type == SSH_CERT_HOST ? "host" : "unknown",
            type == SSH_CERT_USER ? "user" : "host");
        return -1;
    }
    if (now < c->valid_after || now >= c->valid_before) {
        snprintf(why, whylen, "the certificate is not valid now (valid %llu..%llu, now %llu)",
            (unsigned long long)c->valid_after, (unsigned long long)c->valid_before, (unsigned long long)now);
        return -1;
    }
    if (c->principals_len == 0 || !principal_has(c, principal)) {
        snprintf(why, whylen, "the certificate does not list the principal \"%s\"", principal);
        return -1;
    }
    if (force_cmd)
        *force_cmd = NULL;
    sshr_init(&r, c->crit, c->crit_len);
    while (r.off < r.len) {
        size_t nn, dn, vn;
        const uint8_t* nm = sshr_str(&r, &nn);
        const uint8_t* d = sshr_str(&r, &dn);
        sshr_t dr;
        const uint8_t* v;
        if (r.err) {
            snprintf(why, whylen, "malformed certificate critical options");
            goto fail;
        }
        sshr_init(&dr, d, dn);
        v = sshr_str(&dr, &vn);
        if (type == SSH_CERT_USER && str_is(nm, nn, "force-command") && !dr.err) {
            if (force_cmd) {
                free(*force_cmd);
                *force_cmd = (char*)malloc(vn + 1);
                if (!*force_cmd) {
                    snprintf(why, whylen, "out of memory");
                    return -1;
                }
                memcpy(*force_cmd, v, vn);
                (*force_cmd)[vn] = 0;
            }
            continue;
        }
        if (type == SSH_CERT_USER && str_is(nm, nn, "source-address") && !dr.err) {
            if (!peer || !source_ok(peer, v, vn)) {
                snprintf(why, whylen, "the certificate's source-address does not allow %s", peer ? peer : "this peer");
                goto fail;
            }
            continue;
        }
        snprintf(why, whylen, "the certificate carries the unsupported critical option \"%.*s\"", (int)(nn > 60 ? 60 : nn), nm ? (const char*)nm : "");
        goto fail;
    }
    if (permit_pty)
        *permit_pty = ssh_cert_ext_has(c, "permit-pty");
    return 0;
fail:
    if (force_cmd) {
        free(*force_cmd);
        *force_cmd = NULL;
    }
    return -1;
}

int ssh_blob_list_has(const sshb_t* list, const uint8_t* blob, size_t n)
{
    size_t off = 0;
    while (off + 4 <= list->len) {
        uint32_t l = ssh_be32(list->p + off);
        if (l == n && memcmp(list->p + off + 4, blob, n) == 0)
            return 1;
        off += 4 + l;
    }
    return 0;
}

static int known_type(const char* t, size_t n)
{
    return (n == 11 && memcmp(t, "ssh-ed25519", 11) == 0)
        || (n == 26 && memcmp(t, "sk-ssh-ed25519@openssh.com", 26) == 0)
        || (n == 32 && memcmp(t, "ssh-ed25519-cert-v01@openssh.com", 32) == 0);
}

int ssh_parse_pubkey(const char* s, size_t n, sshb_t* out, char* type, size_t typecap)
{
    size_t i = 0, a, tl, bl;
    const char *t, *b;
    uint8_t* raw;
    sshr_t r;
    const uint8_t* inner;
    size_t in, in2;
    while (i < n && (s[i] == ' ' || s[i] == '\t'))
        i++;
    a = i;
    while (i < n && s[i] != ' ' && s[i] != '\t')
        i++;
    t = s + a;
    tl = i - a;
    while (i < n && (s[i] == ' ' || s[i] == '\t'))
        i++;
    a = i;
    while (i < n && s[i] != ' ' && s[i] != '\t' && s[i] != '\r' && s[i] != '\n')
        i++;
    b = s + a;
    bl = i - a;
    if (!known_type(t, tl) || bl == 0 || bl > SSH_PUBLINE_MAX)
        return -1;
    raw = (uint8_t*)malloc(bl);
    if (!raw)
        return -1;
    in = ssh_b64_decode(b, bl, raw, bl);
    if (in == (size_t)-1) {
        free(raw);
        return -1;
    }
    sshr_init(&r, raw, in);
    inner = sshr_str(&r, &in2);
    if (r.err || in2 != tl || !inner || memcmp(inner, t, tl) != 0) {
        free(raw);
        return -1;
    }
    out->len = 0;
    if (sshb_put(out, raw, in) < 0) {
        free(raw);
        return -1;
    }
    if (type && typecap) {
        snprintf(type, typecap, "%.*s", (int)tl, (const char*)inner);
    }
    free(raw);
    return 0;
}

int ssh_parse_pubkey_lines(const char* text, size_t n, sshb_t* list, char* why, size_t whylen)
{
    size_t off = 0, line_no = 0;
    sshb_t one = { 0 };
    while (off < n) {
        size_t e = off, s;
        line_no++;
        while (e < n && text[e] != '\n')
            e++;
        s = off;
        off = e + 1;
        while (s < e && (text[s] == ' ' || text[s] == '\t' || text[s] == '\r'))
            s++;
        if (s == e || text[s] == '#')
            continue;
        if (ssh_parse_pubkey(text + s, e - s, &one, NULL, 0) < 0) {
            snprintf(why, whylen, "line %zu is not \"ssh-ed25519|sk-ssh-ed25519@openssh.com <base64> [comment]\" (key options are not supported)", line_no);
            sshb_free(&one);
            return -1;
        }
        if (sshb_u32(list, (uint32_t)one.len) < 0 || sshb_put(list, one.p, one.len) < 0) {
            snprintf(why, whylen, "out of memory");
            sshb_free(&one);
            return -1;
        }
    }
    sshb_free(&one);
    return 0;
}

#endif
