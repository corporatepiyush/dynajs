#include "ssh-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_SSH)

#include <stdarg.h>
#include <stdio.h>
#include <time.h>
#include <openssl/crypto.h>
#include "dyna-nat.h"

#define SSH_PEND_MAX (64u << 20)
#define SSH_PRE_MAX (64u << 10)
#define SSH_BANNER_MAX (64u << 10)

typedef struct {
    const char* name;
    int id;
} ssh_alg_t;

static const ssh_alg_t kex_tab[] = {
    { "mlkem768x25519-sha256", SSH_KEX_MLKEM768X25519 },
    { "sntrup761x25519-sha512", SSH_KEX_SNTRUP761X25519 },
    { "sntrup761x25519-sha512@openssh.com", SSH_KEX_SNTRUP761X25519 },
    { "curve25519-sha256", SSH_KEX_CURVE25519 },
    { "curve25519-sha256@libssh.org", SSH_KEX_CURVE25519 },
};

static const ssh_alg_t hk_tab[] = {
    { "ssh-ed25519-cert-v01@openssh.com", SSH_HK_ED25519_CERT },
    { "ssh-ed25519", SSH_HK_ED25519 },
    { "sk-ssh-ed25519@openssh.com", SSH_HK_SK_ED25519 },
    { "rsa-sha2-512", SSH_HK_RSA_SHA2_512 },
};

static const ssh_alg_t cipher_tab[] = {
    { "chacha20-poly1305@openssh.com", SSH_CIPHER_CHACHAPOLY },
    { "aes256-gcm@openssh.com", SSH_CIPHER_AES256GCM },
};

#define m_u8 ssh_m_u8
#define m_u32 ssh_m_u32
#define m_str ssh_m_str
#define m_cstr ssh_m_cstr
#define frame ssh_frame
#define derive ssh_derive
#define chan_finish ssh_chan_finish
#define chan_send_close ssh_chan_send_close

static const char* const hk_blob_type[SSH_HK_N] = { "ssh-ed25519", "sk-ssh-ed25519@openssh.com", "ssh-rsa", "ssh-ed25519-cert-v01@openssh.com" };
static const char* const kex_names[SSH_KEX_N] = { "mlkem768x25519-sha256", "sntrup761x25519-sha512", "curve25519-sha256" };
static const char* const hk_names[SSH_HK_N] = { "ssh-ed25519", "sk-ssh-ed25519@openssh.com", "rsa-sha2-512", "ssh-ed25519-cert-v01@openssh.com" };

const char* ssh_kex_name(int id)
{
    return id >= 0 && id < SSH_KEX_N ? kex_names[id] : "";
}

const char* ssh_hk_name(int id)
{
    return id >= 0 && id < SSH_HK_N ? hk_names[id] : "";
}

uint64_t ssh_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

void ssh_m_u8(ssh_conn_t* c, uint8_t v)
{
    if (sshb_u8(&c->msg, v) < 0)
        c->merr = 1;
}

void ssh_m_u32(ssh_conn_t* c, uint32_t v)
{
    if (sshb_u32(&c->msg, v) < 0)
        c->merr = 1;
}

void ssh_m_str(ssh_conn_t* c, const void* p, size_t n)
{
    if (sshb_str(&c->msg, p, n) < 0)
        c->merr = 1;
}

void ssh_m_cstr(ssh_conn_t* c, const char* s)
{
    m_str(c, s, strlen(s));
}

void ssh_msg_begin(ssh_conn_t* c, uint8_t type)
{
    c->msg.len = 0;
    c->merr = 0;
    m_u8(c, type);
}

int ssh_frame(ssh_conn_t* c, const uint8_t* p, size_t n)
{
    ssh_aead_t* a = &c->aout;
    size_t bs = a->bs ? a->bs : 8, aad = a->kind ? 4 : 0, len = 5 + n, pad, pl;
    uint8_t* q;
    pad = bs - ((len - aad) % bs);
    if (pad < 4)
        pad += bs;
    pl = 1 + n + pad;
    if (pl > UINT32_MAX - 64 || sshb_reserve(&c->obuf, 4 + pl + a->tag) < 0)
        return -1;
    q = c->obuf.p + c->obuf.len;
    ssh_put_be32(q, (uint32_t)pl);
    q[4] = (uint8_t)pad;
    memcpy(q + 5, p, n);
    ssh_random(q + 5 + n, pad);
    if (a->kind && ssh_aead_seal(a, c->seq_out, q, pl) < 0)
        return -1;
    c->obuf.len += 4 + pl + a->tag;
    c->seq_out++;
    c->bytes_out += 4 + pl + a->tag;
    c->kex_bytes += 4 + pl + a->tag;
    c->pk_out++;
    if (c->seq_out == 0 && !c->kex_done_once)
        return -1;
    return 0;
}

static int send_payload(ssh_conn_t* c, const uint8_t* p, size_t n)
{
    uint8_t t = p[0];
    if (c->state == SSH_ST_DEAD)
        return -1;
    if (c->kexinit_sent && !c->newkeys_sent && !(t < 50 && t != SSH_MSG_SERVICE_REQUEST && t != SSH_MSG_SERVICE_ACCEPT && t != SSH_MSG_EXT_INFO)) {
        if (c->pend.len + n + 4 > SSH_PEND_MAX || sshb_u32(&c->pend, (uint32_t)n) < 0 || sshb_put(&c->pend, p, n) < 0)
            return -1;
        return 0;
    }
    if (frame(c, p, n) < 0)
        return -1;
    if (c->state == SSH_ST_READY && !c->kex_active && c->kex_bytes >= c->cfg.rekey_bytes)
        return ssh_start_rekey(c) < 0 ? -1 : 0;
    return 0;
}

int ssh_send_msg(ssh_conn_t* c)
{
    if (c->merr) {
        ssh_fail(c, SSH_DISC_BY_APPLICATION, "out of memory building a message");
        return -1;
    }
    if (send_payload(c, c->msg.p, c->msg.len) < 0) {
        if (c->state != SSH_ST_DEAD)
            ssh_fail(c, SSH_DISC_PROTOCOL_ERROR, "cannot frame an outgoing packet");
        return -1;
    }
    return 0;
}

static void q_trim(const ssh_conn_t* c, sshb_t* b)
{
    if (c->cfg.shrink && b->len == 0 && b->cap > SSH_TRIM_CAP)
        sshb_free(b);
}

void ssh_flush(ssh_conn_t* c)
{
    if (c->via) {
        ssh_via_flush(c);
        q_trim(c, &c->obuf);
        return;
    }
    if (!c->tcp_up || c->fd < 0 || c->obuf.len == 0)
        return;
    if (dyn_aio_send(c->aio, c->fd, c->obuf.p, c->obuf.len, 0, NULL, NULL) < 0) {
        c->obuf.len = 0;
        ssh_fail(c, 0, "write to the socket failed");
        return;
    }
    c->obuf.len = 0;
    q_trim(c, &c->obuf);
}

void ssh_conn_free_kex(ssh_conn_t* c)
{
    EVP_PKEY_free(c->xkey);
    EVP_PKEY_free(c->mkey);
    c->xkey = c->mkey = NULL;
    if (c->ssk) {
        OPENSSL_cleanse(c->ssk, SSH_SNTRUP_SK);
        free(c->ssk);
        c->ssk = NULL;
    }
    ssh_aead_free(&c->nin);
    ssh_aead_free(&c->nout);
}

void ssh_fail(ssh_conn_t* c, int reason, const char* fmt, ...)
{
    char buf[512];
    va_list ap;
    if (c->state == SSH_ST_DEAD)
        return;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    free(c->err);
    c->err = strdup(buf);
    if (reason && c->tcp_up && c->fd >= 0) {
        int saved = c->kexinit_sent;
        c->kexinit_sent = 0;
        ssh_msg_begin(c, SSH_MSG_DISCONNECT);
        m_u32(c, (uint32_t)reason);
        m_cstr(c, buf);
        m_cstr(c, "");
        if (!c->merr)
            (void)frame(c, c->msg.p, c->msg.len);
        c->kexinit_sent = saved;
        ssh_flush(c);
    }
    c->state = SSH_ST_DEAD;
    ssh_conn_free_kex(c);
    sshb_free(&c->pend);
    {
        int i;
        for (i = 0; i < c->nch; i++)
            if (c->ch[i])
                chan_finish(c, c->ch[i], c->err);
    }
    ssh_js_died(c);
}

static int hk_ok(const ssh_conn_t* c, int id)
{
    if (c->server)
        return id == SSH_HK_ED25519 || (id == SSH_HK_ED25519_CERT && c->srv && c->srv->hcert.len);
    if (id == SSH_HK_RSA_SHA2_512)
        return c->cfg.allow_rsa;
    if (id == SSH_HK_ED25519_CERT)
        return c->offer_hk_cert;
    return 1;
}

static void kex_drop(ssh_conn_t* c)
{
    EVP_PKEY_free(c->xkey);
    EVP_PKEY_free(c->mkey);
    c->xkey = c->mkey = NULL;
    if (c->ssk) {
        OPENSSL_cleanse(c->ssk, SSH_SNTRUP_SK);
        free(c->ssk);
        c->ssk = NULL;
    }
}

static void name_list(ssh_conn_t* c, sshb_t* b, const ssh_alg_t* t, size_t n, const char* extra)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (t == hk_tab && !hk_ok(c, t[i].id))
            continue;
        if (b->len)
            sshb_u8(b, ',');
        sshb_put(b, t[i].name, strlen(t[i].name));
    }
    if (extra) {
        if (b->len)
            sshb_u8(b, ',');
        sshb_put(b, extra, strlen(extra));
    }
}

static int send_kexinit(ssh_conn_t* c)
{
    sshb_t l = { 0 };
    sshb_t* mine = c->server ? &c->is : &c->ic;
    uint8_t cookie[16];
    int i;
    ssh_msg_begin(c, SSH_MSG_KEXINIT);
    ssh_random(cookie, sizeof cookie);
    if (sshb_put(&c->msg, cookie, 16) < 0)
        c->merr = 1;
    name_list(c, &l, kex_tab, countof(kex_tab),
        c->kex_done_once ? NULL : c->server ? "kex-strict-s-v00@openssh.com" : "ext-info-c,kex-strict-c-v00@openssh.com");
    m_str(c, l.p, l.len);
    l.len = 0;
    name_list(c, &l, hk_tab, countof(hk_tab), NULL);
    m_str(c, l.p, l.len);
    l.len = 0;
    name_list(c, &l, cipher_tab, countof(cipher_tab), NULL);
    m_str(c, l.p, l.len);
    m_str(c, l.p, l.len);
    sshb_free(&l);
    m_cstr(c, "");
    m_cstr(c, "");
    m_cstr(c, "none");
    m_cstr(c, "none");
    m_cstr(c, "");
    m_cstr(c, "");
    m_u8(c, 0);
    m_u32(c, 0);
    mine->len = 0;
    if (c->merr || sshb_put(mine, c->msg.p, c->msg.len) < 0) {
        ssh_fail(c, SSH_DISC_BY_APPLICATION, "out of memory building KEXINIT");
        return -1;
    }
    i = ssh_send_msg(c);
    c->kexinit_sent = 1;
    c->newkeys_sent = 0;
    return i;
}

int ssh_start(ssh_conn_t* c)
{
    char* own = c->server ? c->vs : c->vc;
    snprintf(own, sizeof c->vc, "SSH-2.0-dynajs_%s", CONFIG_VERSION);
    if (sshb_put(&c->obuf, own, strlen(own)) < 0 || sshb_put(&c->obuf, "\r\n", 2) < 0)
        return -1;
    c->state = SSH_ST_VERSION;
    c->kex_active = 1;
    c->kex_at_ms = c->last_io_ms = ssh_now_ms();
    if (c->cfg.handshake_ms)
        c->deadline_ms = c->kex_at_ms + c->cfg.handshake_ms;
    return send_kexinit(c);
}

int ssh_start_rekey(ssh_conn_t* c)
{
    if (c->state != SSH_ST_READY || c->kex_active)
        return 0;
    c->kex_active = 1;
    return send_kexinit(c);
}

static int choose(ssh_conn_t* c, const ssh_alg_t* t, size_t n, const uint8_t* peer, size_t pn, const char** chosen)
{
    size_t i;
    if (c->server) {
        size_t a = 0;
        while (a < pn) {
            size_t e = a;
            while (e < pn && peer[e] != ',')
                e++;
            for (i = 0; i < n; i++) {
                if (t == hk_tab && !hk_ok(c, t[i].id))
                    continue;
                if (strlen(t[i].name) == e - a && memcmp(t[i].name, peer + a, e - a) == 0) {
                    *chosen = t[i].name;
                    return t[i].id;
                }
            }
            a = e + 1;
        }
        return -1;
    }
    for (i = 0; i < n; i++) {
        if (t == hk_tab && !hk_ok(c, t[i].id))
            continue;
        if (ssh_namelist_has(peer, pn, t[i].name)) {
            *chosen = t[i].name;
            return t[i].id;
        }
    }
    return -1;
}

static int first_is(const uint8_t* l, size_t n, const char* name)
{
    size_t k = strlen(name);
    return n >= k && memcmp(l, name, k) == 0 && (n == k || l[k] == ',');
}

static int on_kexinit(ssh_conn_t* c, const uint8_t* p, size_t n, sshr_t* r)
{
    const uint8_t* l[10];
    size_t ln[10];
    const char *kn = NULL, *hn = NULL, *cn = NULL;
    const char* peer = c->server ? "client" : "server";
    sshb_t* theirs = c->server ? &c->ic : &c->is;
    int i, follows;
    if (c->kexinit_recv) {
        ssh_fail(c, SSH_DISC_PROTOCOL_ERROR, "duplicate KEXINIT");
        return -1;
    }
    if (!c->kex_active) {
        if (c->state != SSH_ST_READY) {
            ssh_fail(c, SSH_DISC_PROTOCOL_ERROR, "unexpected KEXINIT");
            return -1;
        }
        c->kex_active = 1;
        if (send_kexinit(c) < 0)
            return -1;
    }
    theirs->len = 0;
    if (sshb_put(theirs, p, n) < 0) {
        ssh_fail(c, SSH_DISC_BY_APPLICATION, "out of memory");
        return -1;
    }
    c->kexinit_recv = 1;
    if (r->len - r->off < 16) {
        ssh_fail(c, SSH_DISC_PROTOCOL_ERROR, "truncated KEXINIT");
        return -1;
    }
    r->off += 16;
    for (i = 0; i < 10; i++)
        l[i] = sshr_str(r, &ln[i]);
    follows = sshr_bool(r);
    (void)sshr_u32(r);
    if (r->err) {
        ssh_fail(c, SSH_DISC_PROTOCOL_ERROR, "malformed KEXINIT");
        return -1;
    }
    if (!c->kex_done_once) {
        c->strict = ssh_namelist_has(l[0], ln[0], c->server ? "kex-strict-c-v00@openssh.com" : "kex-strict-s-v00@openssh.com");
        if (!c->strict) {
            ssh_fail(c, SSH_DISC_KEY_EXCHANGE_FAILED, "the %s does not offer strict key exchange (%s); refused as the Terrapin countermeasure",
                peer, c->server ? "kex-strict-c-v00@openssh.com" : "kex-strict-s-v00@openssh.com");
            return -1;
        }
        if (c->server)
            c->ext_info_c = ssh_namelist_has(l[0], ln[0], "ext-info-c");
    }
    c->kex_alg = choose(c, kex_tab, countof(kex_tab), l[0], ln[0], &kn);
    if (c->kex_alg < 0) {
        ssh_fail(c, SSH_DISC_KEY_EXCHANGE_FAILED, "no common key exchange algorithm (%s offers %.*s)", peer, (int)(ln[0] > 300 ? 300 : ln[0]), l[0]);
        return -1;
    }
    c->hk_alg = choose(c, hk_tab, countof(hk_tab), l[1], ln[1], &hn);
    if (c->hk_alg < 0) {
        ssh_fail(c, SSH_DISC_KEY_EXCHANGE_FAILED, "no common host key algorithm (%s offers %.*s)", peer, (int)(ln[1] > 300 ? 300 : ln[1]), l[1]);
        return -1;
    }
    c->cip_cs = choose(c, cipher_tab, countof(cipher_tab), l[2], ln[2], &cn);
    c->cip_sc = choose(c, cipher_tab, countof(cipher_tab), l[3], ln[3], &cn);
    if (c->cip_cs < 0 || c->cip_sc < 0) {
        ssh_fail(c, SSH_DISC_KEY_EXCHANGE_FAILED, "no common cipher (%s offers %.*s)", peer, (int)(ln[2] > 300 ? 300 : ln[2]), l[2]);
        return -1;
    }
    if (!ssh_namelist_has(l[6], ln[6], "none") || !ssh_namelist_has(l[7], ln[7], "none")) {
        ssh_fail(c, SSH_DISC_KEY_EXCHANGE_FAILED, "the %s requires compression, which is not supported", peer);
        return -1;
    }
    if (follows && !(first_is(l[0], ln[0], kn) && first_is(l[1], ln[1], hn)))
        c->skip_next = 1;
    ssh_conn_free_kex(c);
    c->hlen = c->kex_alg == SSH_KEX_SNTRUP761X25519 ? 64 : 32;
    if (c->server)
        return 0;
    {
        uint8_t xp[32];
        c->xkey = ssh_x25519_new(xp);
        if (!c->xkey) {
            ssh_fail(c, SSH_DISC_KEY_EXCHANGE_FAILED, "X25519 key generation failed");
            return -1;
        }
        if (c->kex_alg == SSH_KEX_MLKEM768X25519) {
            c->mkey = ssh_mlkem_new(c->qc);
            if (!c->mkey) {
                ssh_fail(c, SSH_DISC_KEY_EXCHANGE_FAILED, "ML-KEM-768 key generation failed");
                return -1;
            }
            memcpy(c->qc + 1184, xp, 32);
            c->qclen = 1216;
        } else if (c->kex_alg == SSH_KEX_SNTRUP761X25519) {
            c->ssk = (uint8_t*)malloc(SSH_SNTRUP_SK);
            if (!c->ssk || ssh_sntrup_keypair(c->qc, c->ssk) < 0) {
                ssh_fail(c, SSH_DISC_KEY_EXCHANGE_FAILED, "sntrup761 key generation failed");
                return -1;
            }
            memcpy(c->qc + SSH_SNTRUP_PK, xp, 32);
            c->qclen = SSH_SNTRUP_PK + 32;
        } else {
            memcpy(c->qc, xp, 32);
            c->qclen = 32;
        }
    }
    ssh_msg_begin(c, SSH_MSG_KEX_INIT);
    m_str(c, c->qc, c->qclen);
    return ssh_send_msg(c);
}

int ssh_derive(const sshb_t* k, const uint8_t* h, size_t hl, const uint8_t* sid, size_t sidl,
    char letter, uint8_t* out, size_t need)
{
    uint8_t blk[64];
    size_t have = 0;
    EVP_MD_CTX* m = EVP_MD_CTX_new();
    const EVP_MD* md = hl == 64 ? EVP_sha512() : EVP_sha256();
    int ok = m != NULL;
    uint8_t ch = (uint8_t)letter;
    while (ok && have < need) {
        size_t take;
        ok = EVP_DigestInit_ex(m, md, NULL) == 1 && EVP_DigestUpdate(m, k->p, k->len) == 1
            && EVP_DigestUpdate(m, h, hl) == 1;
        if (ok && have == 0)
            ok = EVP_DigestUpdate(m, &ch, 1) == 1 && EVP_DigestUpdate(m, sid, sidl) == 1;
        else if (ok)
            ok = EVP_DigestUpdate(m, out, have) == 1;
        ok = ok && EVP_DigestFinal_ex(m, blk, NULL) == 1;
        take = need - have < hl ? need - have : hl;
        if (ok)
            memcpy(out + have, blk, take);
        have += take;
    }
    EVP_MD_CTX_free(m);
    OPENSSL_cleanse(blk, sizeof blk);
    return ok ? 0 : -1;
}

int ssh_kex_hash(ssh_conn_t* c, const uint8_t* ks, size_t kn, const uint8_t* qc, size_t qcn,
    const uint8_t* qs, size_t qsn, const sshb_t* k, uint8_t* h)
{
    sshb_t hb = { 0 };
    int rc = -1;
    if (sshb_str(&hb, c->vc, strlen(c->vc)) < 0 || sshb_str(&hb, c->vs, strlen(c->vs)) < 0
        || sshb_str(&hb, c->ic.p, c->ic.len) < 0 || sshb_str(&hb, c->is.p, c->is.len) < 0
        || sshb_str(&hb, ks, kn) < 0 || sshb_str(&hb, qc, qcn) < 0
        || sshb_str(&hb, qs, qsn) < 0 || sshb_put(&hb, k->p, k->len) < 0)
        goto out;
    if (ssh_hash(c->hlen == 64, hb.p, hb.len, h) < 0)
        goto out;
    if (!c->kex_done_once) {
        memcpy(c->sid, h, c->hlen);
        c->sidlen = c->hlen;
    }
    rc = 0;
out:
    sshb_free(&hb);
    return rc;
}

int ssh_kex_keys(ssh_conn_t* c, const sshb_t* k, const uint8_t* h)
{
    uint8_t ko[64 + 12], ki[64 + 12];
    int out_alg = c->server ? c->cip_sc : c->cip_cs, in_alg = c->server ? c->cip_cs : c->cip_sc;
    char oiv = c->server ? 'B' : 'A', okey = c->server ? 'D' : 'C';
    char iiv = c->server ? 'A' : 'B', ikey = c->server ? 'C' : 'D';
    int rc = derive(k, h, c->hlen, c->sid, c->sidlen, oiv, ko + 64, ssh_cipher_ivlen(out_alg)) < 0
            || derive(k, h, c->hlen, c->sid, c->sidlen, okey, ko, ssh_cipher_keylen(out_alg)) < 0
            || ssh_aead_init(&c->nout, out_alg, 1, ko, ko + 64) < 0
            || derive(k, h, c->hlen, c->sid, c->sidlen, iiv, ki + 64, ssh_cipher_ivlen(in_alg)) < 0
            || derive(k, h, c->hlen, c->sid, c->sidlen, ikey, ki, ssh_cipher_keylen(in_alg)) < 0
            || ssh_aead_init(&c->nin, in_alg, 0, ki, ki + 64) < 0
        ? -1
        : 0;
    OPENSSL_cleanse(ko, sizeof ko);
    OPENSSL_cleanse(ki, sizeof ki);
    return rc;
}

int ssh_newkeys_out(ssh_conn_t* c)
{
    ssh_msg_begin(c, SSH_MSG_NEWKEYS);
    if (c->merr || frame(c, c->msg.p, c->msg.len) < 0) {
        ssh_fail(c, SSH_DISC_PROTOCOL_ERROR, "cannot send NEWKEYS");
        return -1;
    }
    ssh_aead_free(&c->aout);
    c->aout = c->nout;
    memset(&c->nout, 0, sizeof c->nout);
    if (c->strict)
        c->seq_out = 0;
    c->newkeys_sent = 1;
    if (c->server && !c->kex_done_once && c->ext_info_c)
        ssh_srv_send_ext_info(c);
    if (c->pend.len) {
        size_t off = 0;
        sshb_t q = c->pend;
        memset(&c->pend, 0, sizeof c->pend);
        while (off + 4 <= q.len && c->state != SSH_ST_DEAD) {
            uint32_t l = ssh_be32(q.p + off);
            if (frame(c, q.p + off + 4, l) < 0) {
                ssh_fail(c, SSH_DISC_PROTOCOL_ERROR, "cannot frame a queued packet");
                break;
            }
            off += 4 + l;
        }
        sshb_free(&q);
    }
    return c->state == SSH_ST_DEAD ? -1 : 0;
}

static int check_host(ssh_conn_t* c, const uint8_t* ks, size_t kn)
{
    const char* type = hk_blob_type[c->hk_alg];
    int rc;
    ssh_fingerprint(ks, kn, c->hk_fp);
    snprintf(c->hk_type, sizeof c->hk_type, "%s", type);
    if (!c->known_hosts) {
        ssh_fail(c, SSH_DISC_HOST_KEY_NOT_VERIFIABLE, "no known_hosts file to verify the host key %s", c->hk_fp);
        return -1;
    }
    rc = ssh_known_hosts_check(c->known_hosts, c->host, c->port, type, ks, kn);
    if (rc == SSH_KH_OK)
        return 0;
    if (rc == SSH_KH_UNKNOWN && c->cfg.accept_new) {
        if (ssh_known_hosts_add(c->known_hosts, c->host, c->port, type, ks, kn) < 0) {
            ssh_fail(c, SSH_DISC_HOST_KEY_NOT_VERIFIABLE, "cannot append the new host key to %s", c->known_hosts);
            return -1;
        }
        return 0;
    }
    if (rc == SSH_KH_UNKNOWN)
        ssh_fail(c, SSH_DISC_HOST_KEY_NOT_VERIFIABLE, "host key for %s is not in %s (%s %s); refused (dyna.hostKeyPolicy \"accept-new\" trusts a new host on first use)", c->host, c->known_hosts, type, c->hk_fp);
    else if (rc == SSH_KH_CHANGED)
        ssh_fail(c, SSH_DISC_HOST_KEY_NOT_VERIFIABLE, "HOST KEY MISMATCH for %s: %s in %s differs from the server's %s; refused", c->host, type, c->known_hosts, c->hk_fp);
    else if (rc == SSH_KH_REVOKED)
        ssh_fail(c, SSH_DISC_HOST_KEY_NOT_VERIFIABLE, "the host key %s of %s is marked @revoked in %s", c->hk_fp, c->host, c->known_hosts);
    else
        ssh_fail(c, SSH_DISC_HOST_KEY_NOT_VERIFIABLE, "cannot read known_hosts file %s", c->known_hosts);
    return -1;
}

static int check_host_cert(ssh_conn_t* c, const ssh_cert_t* cert, const sshb_t* kb)
{
    char why[200], cafp[64];
    int rc;
    ssh_fingerprint(kb->p, kb->len, c->hk_fp);
    snprintf(c->hk_type, sizeof c->hk_type, "%s", hk_blob_type[SSH_HK_ED25519_CERT]);
    if (ssh_cert_check(cert, SSH_CERT_HOST, c->host, NULL, NULL, NULL, why, sizeof why) < 0) {
        ssh_fail(c, SSH_DISC_HOST_KEY_NOT_VERIFIABLE, "host certificate refused: %s", why);
        return -1;
    }
    ssh_fingerprint(cert->ca, cert->ca_len, cafp);
    rc = c->known_hosts ? ssh_known_hosts_check_ca(c->known_hosts, c->host, c->port, cert->ca, cert->ca_len, kb->p, kb->len) : SSH_KH_UNKNOWN;
    if (rc == SSH_KH_OK)
        return 0;
    if (rc == SSH_KH_REVOKED)
        ssh_fail(c, SSH_DISC_HOST_KEY_NOT_VERIFIABLE, "the host certificate's key or CA %s is marked @revoked in %s", cafp, c->known_hosts);
    else
        ssh_fail(c, SSH_DISC_HOST_KEY_NOT_VERIFIABLE, "host certificate signed by CA %s, which no @cert-authority line in %s trusts for %s", cafp, c->known_hosts ? c->known_hosts : "known_hosts", c->host);
    return -1;
}

static int on_kex_reply(ssh_conn_t* c, sshr_t* r)
{
    const uint8_t *ks, *qs, *sig;
    size_t kn, qn, sn;
    size_t want = c->kex_alg == SSH_KEX_MLKEM768X25519 ? 1088 + 32 : c->kex_alg == SSH_KEX_SNTRUP761X25519 ? SSH_SNTRUP_CT + 32 : 32;
    uint8_t xs[32], h[64];
    sshb_t kenc = { 0 }, plain = { 0 };
    char why[200];
    int rc = -1;
    if (!c->kexinit_recv || c->reply_done || !c->xkey) {
        ssh_fail(c, SSH_DISC_PROTOCOL_ERROR, "unexpected key exchange reply");
        return -1;
    }
    ks = sshr_str(r, &kn);
    qs = sshr_str(r, &qn);
    sig = sshr_str(r, &sn);
    if (r->err || r->off != r->len || qn != want || kn == 0) {
        ssh_fail(c, SSH_DISC_KEY_EXCHANGE_FAILED, "malformed key exchange reply");
        return -1;
    }
    if (ssh_x25519_derive(c->xkey, qs + qn - 32, xs) < 0) {
        ssh_fail(c, SSH_DISC_KEY_EXCHANGE_FAILED, "invalid X25519 public value from the server");
        return -1;
    }
    if (c->kex_alg != SSH_KEX_CURVE25519) {
        uint8_t ms[32], hh[64];
        int bad = c->kex_alg == SSH_KEX_MLKEM768X25519 ? ssh_mlkem_decap(c->mkey, qs, ms) < 0 : ssh_sntrup_dec(ms, qs, c->ssk) < 0;
        if (bad || ssh_hash2(c->hlen == 64, ms, 32, xs, 32, hh) < 0) {
            OPENSSL_cleanse(ms, sizeof ms);
            ssh_fail(c, SSH_DISC_KEY_EXCHANGE_FAILED, "%s decapsulation failed", c->kex_alg == SSH_KEX_MLKEM768X25519 ? "ML-KEM-768" : "sntrup761");
            return -1;
        }
        OPENSSL_cleanse(ms, sizeof ms);
        if (sshb_str(&kenc, hh, c->hlen) < 0)
            goto oom;
        OPENSSL_cleanse(hh, sizeof hh);
    } else if (sshb_mpint(&kenc, xs, 32) < 0)
        goto oom;
    OPENSSL_cleanse(xs, sizeof xs);
    if (ssh_kex_hash(c, ks, kn, c->qc, c->qclen, qs, qn, &kenc, h) < 0)
        goto oom;
    if (c->hk_alg == SSH_HK_ED25519_CERT) {
        ssh_cert_t cert;
        if (ssh_cert_parse(ks, kn, &cert, why, sizeof why) < 0) {
            ssh_fail(c, SSH_DISC_HOST_KEY_NOT_VERIFIABLE, "host certificate refused: %s", why);
            goto out;
        }
        ssh_ed25519_blob(&plain, cert.pk);
        if (!plain.len)
            goto oom;
        if (ssh_verify_hostsig(SSH_HK_ED25519, plain.p, plain.len, sig, sn, h, c->hlen, why, sizeof why) < 0) {
            ssh_fail(c, SSH_DISC_HOST_KEY_NOT_VERIFIABLE, "%s", why);
            goto out;
        }
        if (!c->kex_done_once && check_host_cert(c, &cert, &plain) < 0)
            goto out;
    } else {
        if (ssh_verify_hostsig(c->hk_alg, ks, kn, sig, sn, h, c->hlen, why, sizeof why) < 0) {
            ssh_fail(c, SSH_DISC_HOST_KEY_NOT_VERIFIABLE, "%s", why);
            goto out;
        }
        if (!c->kex_done_once && check_host(c, ks, kn) < 0)
            goto out;
    }
    if (!c->kex_done_once) {
        c->hk0.len = 0;
        if (sshb_put(&c->hk0, ks, kn) < 0)
            goto oom;
    } else if (kn != c->hk0.len || memcmp(ks, c->hk0.p, kn) != 0) {
        ssh_fail(c, SSH_DISC_HOST_KEY_NOT_VERIFIABLE, "the host key changed during re-key");
        goto out;
    }
    if (ssh_kex_keys(c, &kenc, h) < 0) {
        ssh_fail(c, SSH_DISC_KEY_EXCHANGE_FAILED, "key derivation failed");
        goto out;
    }
    kex_drop(c);
    c->reply_done = 1;
    rc = ssh_newkeys_out(c);
    goto out;
oom:
    ssh_fail(c, SSH_DISC_BY_APPLICATION, "out of memory during key exchange");
out:
    OPENSSL_cleanse(h, sizeof h);
    sshb_free(&kenc);
    sshb_free(&plain);
    return rc;
}

static int on_newkeys(ssh_conn_t* c)
{
    if (!c->reply_done) {
        ssh_fail(c, SSH_DISC_PROTOCOL_ERROR, "NEWKEYS before the key exchange reply");
        return -1;
    }
    ssh_aead_free(&c->ain);
    c->ain = c->nin;
    memset(&c->nin, 0, sizeof c->nin);
    c->have_len = 0;
    if (c->strict)
        c->seq_in = 0;
    c->kex_active = c->kexinit_sent = c->kexinit_recv = c->reply_done = c->newkeys_sent = 0;
    c->ic.len = c->is.len = 0;
    c->kex_bytes = 0;
    c->kex_at_ms = ssh_now_ms();
    if (!c->kex_done_once) {
        c->kex_done_once = 1;
        c->state = SSH_ST_SERVICE;
        if (c->server)
            return 0;
        ssh_msg_begin(c, SSH_MSG_SERVICE_REQUEST);
        m_cstr(c, "ssh-userauth");
        return ssh_send_msg(c);
    }
    c->rekeys++;
    for (int i = 0; i < c->nch && c->state == SSH_ST_READY; i++)
        if (c->ch[i])
            ssh_chan_pump(c->ch[i]);
    return c->state == SSH_ST_DEAD ? -1 : 0;
}

static char* dup_n(const uint8_t* p, size_t n, size_t cap)
{
    char* s;
    if (n > cap)
        n = cap;
    s = (char*)malloc(n + 1);
    if (!s)
        return NULL;
    memcpy(s, p, n);
    s[n] = 0;
    return s;
}

static ssh_chan_t* chan_get(ssh_conn_t* c, sshr_t* r)
{
    uint32_t id = sshr_u32(r);
    if (r->err || id >= (uint32_t)c->nch || !c->ch[id]) {
        ssh_fail(c, SSH_DISC_PROTOCOL_ERROR, "message for unknown channel %u", (unsigned)id);
        return NULL;
    }
    return c->ch[id];
}

static void chan_unlink(ssh_conn_t* c, ssh_chan_t* ch)
{
    if (ch->conn != c || ch->lid >= (uint32_t)c->nch || c->ch[ch->lid] != ch)
        return;
    c->ch[ch->lid] = NULL;
    c->nopen--;
    ch->conn = NULL;
    ssh_chan_unref(c->rt, ch);
}

void ssh_chan_send_close(ssh_chan_t* ch)
{
    ssh_conn_t* c = ch->conn;
    if (!c || ch->sent_close || ch->state == SSH_CH_OPENING || ch->state == SSH_CH_PENDING || c->state == SSH_ST_DEAD)
        return;
    ch->sent_close = 1;
    ssh_msg_begin(c, SSH_MSG_CHANNEL_CLOSE);
    m_u32(c, ch->rid);
    (void)ssh_send_msg(c);
}

void ssh_chan_close(ssh_chan_t* ch)
{
    if (!ch->conn)
        return;
    if (ch->state == SSH_CH_OPENING) {
        ch->want_close = 1;
        return;
    }
    chan_send_close(ch);
}

static int pump_q(ssh_conn_t* c, ssh_chan_t* ch, sshb_t* q, size_t* off, uint64_t* sent, int ext)
{
    while (q->len > *off && ch->rwin > 0 && !c->kex_active) {
        size_t n = q->len - *off;
        uint32_t lim = ch->rmax > 4096 ? ch->rmax - 64 : ch->rmax;
        if (n > ch->rwin)
            n = (size_t)ch->rwin;
        if (n > lim)
            n = lim;
        if (n == 0)
            break;
        ssh_msg_begin(c, ext ? SSH_MSG_CHANNEL_EXTENDED_DATA : SSH_MSG_CHANNEL_DATA);
        m_u32(c, ch->rid);
        if (ext)
            m_u32(c, 1);
        m_str(c, q->p + *off, n);
        if (ssh_send_msg(c) < 0)
            return -1;
        *off += n;
        ch->rwin -= n;
        *sent += n;
    }
    if (*off == q->len) {
        q->len = *off = 0;
        q_trim(c, q);
    } else if (*off > (1u << 20)) {
        memmove(q->p, q->p + *off, q->len - *off);
        q->len -= *off;
        *off = 0;
    }
    return 0;
}

void ssh_chan_pump(ssh_chan_t* ch)
{
    ssh_conn_t* c = ch->conn;
    if (!c || c->state != SSH_ST_READY || ch->state != SSH_CH_OPEN || ch->sent_close)
        return;
    if (!ch->sent_eof) {
        if (pump_q(c, ch, &ch->inq, &ch->inq_off, &ch->in_sent, 0) < 0)
            return;
        if (ch->srv && pump_q(c, ch, &ch->errq, &ch->errq_off, &ch->err_sent, 1) < 0)
            return;
    }
    if (ch->nww || (ch->ops && ch->ops->written))
        ssh_js_chan_written(ch);
    if (!ch->conn)
        return;
    if (ch->want_eof && ch->inq.len == 0 && ch->errq.len == 0 && !ch->sent_eof) {
        ch->sent_eof = 1;
        ssh_msg_begin(c, SSH_MSG_CHANNEL_EOF);
        m_u32(c, ch->rid);
        if (ssh_send_msg(c) < 0)
            return;
    }
    if (ch->srv && ch->want_exit && ch->sent_eof && !ch->sent_close) {
        if (ch->exit_kind == 1) {
            ssh_msg_begin(c, SSH_MSG_CHANNEL_REQUEST);
            m_u32(c, ch->rid);
            m_cstr(c, "exit-status");
            m_u8(c, 0);
            m_u32(c, (uint32_t)ch->exit_code);
            if (ssh_send_msg(c) < 0)
                return;
        } else if (ch->exit_kind == 2) {
            ssh_msg_begin(c, SSH_MSG_CHANNEL_REQUEST);
            m_u32(c, ch->rid);
            m_cstr(c, "exit-signal");
            m_u8(c, 0);
            m_cstr(c, ch->exit_signal ? ch->exit_signal : "TERM");
            m_u8(c, 0);
            m_cstr(c, ch->exit_msg ? ch->exit_msg : "");
            m_cstr(c, "");
            if (ssh_send_msg(c) < 0)
                return;
        }
        chan_send_close(ch);
    }
}

void ssh_chan_window(ssh_chan_t* ch)
{
    ssh_conn_t* c = ch->conn;
    uint64_t used, grant;
    if (!c || c->state != SSH_ST_READY || ch->state == SSH_CH_OPENING || ch->sent_close || ch->got_close)
        return;
    used = ch->lwin + ch->queued[0] + ch->queued[1];
    if (used >= ch->win_size)
        return;
    grant = ch->win_size - used;
    if (grant < ch->win_size / 2)
        return;
    ssh_msg_begin(c, SSH_MSG_CHANNEL_WINDOW_ADJUST);
    m_u32(c, ch->rid);
    m_u32(c, (uint32_t)grant);
    if (ssh_send_msg(c) == 0)
        ch->lwin += grant;
}

int ssh_chan_slot(ssh_conn_t* c, ssh_chan_t* ch)
{
    int i, slot = -1;
    for (i = 0; i < c->nch; i++)
        if (!c->ch[i]) {
            slot = i;
            break;
        }
    if (slot < 0) {
        ssh_chan_t** n;
        int cap = c->nch ? c->nch * 2 : 8;
        if (cap > c->cfg.max_channels)
            cap = c->cfg.max_channels;
        if (cap <= c->nch)
            return -1;
        n = (ssh_chan_t**)realloc(c->ch, (size_t)cap * sizeof *n);
        if (!n)
            return -1;
        memset(n + c->nch, 0, (size_t)(cap - c->nch) * sizeof *n);
        c->ch = n;
        slot = c->nch;
        c->nch = cap;
    }
    c->ch[slot] = ch;
    c->nopen++;
    ch->conn = c;
    ch->lid = (uint32_t)slot;
    return 0;
}

int ssh_chan_open(ssh_conn_t* c, ssh_chan_t* ch)
{
    if (ssh_chan_slot(c, ch) < 0)
        return -1;
    ch->state = SSH_CH_OPENING;
    ssh_msg_begin(c, SSH_MSG_CHANNEL_OPEN);
    m_cstr(c, ch->ctype ? ch->ctype : "session");
    m_u32(c, ch->lid);
    m_u32(c, (uint32_t)ch->lwin);
    m_u32(c, ch->lmax);
    if (ch->oextra.len && sshb_put(&c->msg, ch->oextra.p, ch->oextra.len) < 0)
        return -1;
    return ssh_send_msg(c);
}

void ssh_chan_finish(ssh_conn_t* c, ssh_chan_t* ch, const char* err)
{
    ch->refs++;
    ch->state = SSH_CH_CLOSED;
    ssh_js_chan_closed(ch, err);
    chan_unlink(c, ch);
    ssh_chan_unref(c->rt, ch);
}

static int on_channel(ssh_conn_t* c, uint8_t t, sshr_t* r)
{
    ssh_chan_t* ch = chan_get(c, r);
    const uint8_t* p;
    size_t n;
    if (!ch)
        return -1;
    switch (t) {
    case SSH_MSG_CHANNEL_OPEN_CONFIRMATION: {
        uint32_t rid = sshr_u32(r), rw = sshr_u32(r), rm = sshr_u32(r);
        if (r->err || ch->state != SSH_CH_OPENING || rm == 0) {
            ssh_fail(c, SSH_DISC_PROTOCOL_ERROR, "bad channel open confirmation");
            return -1;
        }
        ch->rid = rid;
        ch->rwin = rw;
        ch->rmax = rm > (1u << 20) ? (1u << 20) : rm;
        ch->state = ch->ctype ? SSH_CH_OPEN : SSH_CH_REQUESTING;
        if (ch->want_close) {
            chan_send_close(ch);
            return 0;
        }
        if (ch->ctype) {
            ssh_js_chan_opened(ch, NULL);
            if (ch->conn) {
                ssh_chan_window(ch);
                ssh_chan_pump(ch);
            }
            return 0;
        }
        ssh_msg_begin(c, SSH_MSG_CHANNEL_REQUEST);
        m_u32(c, ch->rid);
        m_cstr(c, ch->subsys ? "subsystem" : "exec");
        m_u8(c, 1);
        m_str(c, ch->cmd, ch->cmdlen);
        return ssh_send_msg(c);
    }
    case SSH_MSG_CHANNEL_OPEN_FAILURE: {
        uint32_t code = sshr_u32(r);
        char msg[200];
        p = sshr_str(r, &n);
        if (r->err || ch->state != SSH_CH_OPENING) {
            ssh_fail(c, SSH_DISC_PROTOCOL_ERROR, "bad channel open failure");
            return -1;
        }
        snprintf(msg, sizeof msg, "the %s refused the %s channel (code %u: %.*s)", c->server ? "client" : "server", ch->ctype ? ch->ctype : "session", (unsigned)code, (int)(n > 120 ? 120 : n), p);
        ssh_js_chan_opened(ch, msg);
        chan_finish(c, ch, msg);
        return 0;
    }
    case SSH_MSG_CHANNEL_WINDOW_ADJUST: {
        uint32_t add = sshr_u32(r);
        if (r->err) {
            ssh_fail(c, SSH_DISC_PROTOCOL_ERROR, "malformed window adjust");
            return -1;
        }
        ch->rwin += add;
        if (ch->rwin > UINT32_MAX)
            ch->rwin = UINT32_MAX;
        ssh_chan_pump(ch);
        return 0;
    }
    case SSH_MSG_CHANNEL_DATA:
    case SSH_MSG_CHANNEL_EXTENDED_DATA: {
        uint32_t code = t == SSH_MSG_CHANNEL_EXTENDED_DATA ? sshr_u32(r) : 0;
        p = sshr_str(r, &n);
        if (r->err || r->off != r->len || ch->state == SSH_CH_OPENING || ch->state == SSH_CH_PENDING || ch->got_eof) {
            ssh_fail(c, SSH_DISC_PROTOCOL_ERROR, "malformed or unexpected channel data");
            return -1;
        }
        if (n > ch->lwin || n > ch->lmax) {
            ssh_fail(c, SSH_DISC_PROTOCOL_ERROR, "channel data of %zu bytes exceeds the window (%llu) or packet size (%u)", n, (unsigned long long)ch->lwin, (unsigned)ch->lmax);
            return -1;
        }
        ch->lwin -= n;
        ch->got_data = 1;
        if (t == SSH_MSG_CHANNEL_DATA || code == 1)
            ssh_js_chan_data(ch, t == SSH_MSG_CHANNEL_DATA ? 0 : 1, p, n);
        ssh_chan_window(ch);
        return 0;
    }
    case SSH_MSG_CHANNEL_EOF:
        ch->got_eof = 1;
        ssh_js_chan_eof(ch);
        return 0;
    case SSH_MSG_CHANNEL_CLOSE:
        if (ch->state == SSH_CH_PENDING) {
            ssh_fail(c, SSH_DISC_PROTOCOL_ERROR, "CHANNEL_CLOSE for a channel that was never confirmed");
            return -1;
        }
        ch->got_close = 1;
        if (ch->state == SSH_CH_REQUESTING)
            ssh_js_chan_opened(ch, ch->subsys ? "the channel closed before the subsystem request was answered" : "the channel closed before the exec request was answered");
        chan_send_close(ch);
        chan_finish(c, ch, NULL);
        return 0;
    case SSH_MSG_CHANNEL_REQUEST: {
        const uint8_t* ty;
        int want;
        if (c->server)
            return ssh_srv_chan_request(c, ch, r);
        ty = sshr_str(r, &n);
        want = sshr_bool(r);
        if (r->err) {
            ssh_fail(c, SSH_DISC_PROTOCOL_ERROR, "malformed channel request");
            return -1;
        }
        if (n == 11 && memcmp(ty, "exit-status", 11) == 0) {
            uint32_t code = sshr_u32(r);
            if (!r->err) {
                ch->has_status = 1;
                ch->exit_code = (int)code;
            }
        } else if (n == 11 && memcmp(ty, "exit-signal", 11) == 0) {
            size_t sn, mn;
            const uint8_t* s = sshr_str(r, &sn);
            int core = sshr_bool(r);
            const uint8_t* m = sshr_str(r, &mn);
            if (!r->err) {
                free(ch->exit_signal);
                free(ch->exit_msg);
                ch->exit_signal = dup_n(s, sn, 64);
                ch->exit_msg = dup_n(m, mn, 1024);
                ch->exit_core = core;
                ch->has_status = 2;
            }
        } else if (want) {
            ssh_msg_begin(c, SSH_MSG_CHANNEL_FAILURE);
            m_u32(c, ch->rid);
            return ssh_send_msg(c);
        }
        return 0;
    }
    case SSH_MSG_CHANNEL_SUCCESS:
        if (!c->server && ch->state == SSH_CH_REQUESTING) {
            ch->state = SSH_CH_OPEN;
            ssh_js_chan_opened(ch, NULL);
            ssh_chan_pump(ch);
            if (ch->want_close)
                chan_send_close(ch);
        }
        return 0;
    case SSH_MSG_CHANNEL_FAILURE:
        if (!c->server && ch->state == SSH_CH_REQUESTING) {
            ssh_js_chan_opened(ch, "the server refused the exec request");
            ch->state = SSH_CH_OPEN;
            chan_send_close(ch);
        }
        return 0;
    default:
        return 0;
    }
}

static int dispatch(ssh_conn_t* c, const uint8_t* p, size_t n, uint32_t seq)
{
    uint8_t t = p[0];
    sshr_t r;
    sshr_init(&r, p + 1, n - 1);
    if (c->skip_next) {
        c->skip_next = 0;
        if (t >= 30 && t <= 49)
            return 0;
    }
    if (!c->kex_done_once) {
        if (t != SSH_MSG_KEXINIT && t != (c->server ? SSH_MSG_KEX_INIT : SSH_MSG_KEX_REPLY) && t != SSH_MSG_NEWKEYS && t != SSH_MSG_DISCONNECT) {
            ssh_fail(c, SSH_DISC_PROTOCOL_ERROR, "strict KEX: unexpected message %u during the initial key exchange", (unsigned)t);
            return -1;
        }
        if (!c->kexinit_recv && t != SSH_MSG_KEXINIT && t != SSH_MSG_DISCONNECT) {
            ssh_fail(c, SSH_DISC_PROTOCOL_ERROR, "strict KEX: the first packet is message %u, not KEXINIT", (unsigned)t);
            return -1;
        }
    }
    switch (t) {
    case SSH_MSG_DISCONNECT: {
        uint32_t code = sshr_u32(&r);
        size_t dn;
        const uint8_t* d = sshr_str(&r, &dn);
        ssh_fail(c, 0, "the %s disconnected (reason %u: %.*s)", c->server ? "client" : "server", (unsigned)code, (int)(dn > 300 ? 300 : dn), d ? (const char*)d : "");
        return -1;
    }
    case SSH_MSG_IGNORE:
    case SSH_MSG_DEBUG:
        return 0;
    case SSH_MSG_UNIMPLEMENTED:
        ssh_fail(c, SSH_DISC_PROTOCOL_ERROR, "the %s reported packet %u as unimplemented", c->server ? "client" : "server", (unsigned)sshr_u32(&r));
        return -1;
    case SSH_MSG_EXT_INFO: {
        uint32_t k, cnt = sshr_u32(&r);
        if (c->server)
            return 0;
        if (r.err || cnt > 64) {
            ssh_fail(c, SSH_DISC_PROTOCOL_ERROR, "malformed EXT_INFO");
            return -1;
        }
        for (k = 0; k < cnt; k++) {
            size_t nn, vn;
            const uint8_t* nm = sshr_str(&r, &nn);
            const uint8_t* v = sshr_str(&r, &vn);
            if (r.err) {
                ssh_fail(c, SSH_DISC_PROTOCOL_ERROR, "malformed EXT_INFO");
                return -1;
            }
            if (nn == 15 && memcmp(nm, "server-sig-algs", 15) == 0) {
                free(c->server_sig_algs);
                c->server_sig_algs = dup_n(v, vn, 4096);
            }
        }
        return 0;
    }
    case SSH_MSG_KEXINIT:
        return on_kexinit(c, p, n, &r);
    case SSH_MSG_KEX_REPLY:
        if (c->server)
            break;
        return on_kex_reply(c, &r);
    case SSH_MSG_KEX_INIT:
        if (!c->server)
            break;
        return ssh_srv_kex_init(c, &r);
    case SSH_MSG_SERVICE_REQUEST:
        if (!c->server)
            break;
        return ssh_srv_service(c, &r);
    case SSH_MSG_USERAUTH_REQUEST:
        if (!c->server)
            break;
        return ssh_srv_userauth(c, &r);
    case SSH_MSG_NEWKEYS:
        return on_newkeys(c);
    case SSH_MSG_SERVICE_ACCEPT:
        if (c->server)
            break;
        if (c->state != SSH_ST_SERVICE) {
            ssh_fail(c, SSH_DISC_PROTOCOL_ERROR, "unexpected SERVICE_ACCEPT");
            return -1;
        }
        return ssh_auth_begin(c);
    case SSH_MSG_USERAUTH_BANNER: {
        size_t bn;
        const uint8_t* b = sshr_str(&r, &bn);
        if (r.err || c->server || c->state != SSH_ST_AUTH) {
            ssh_fail(c, SSH_DISC_PROTOCOL_ERROR, "unexpected USERAUTH_BANNER");
            return -1;
        }
        free(c->banner);
        c->banner = dup_n(b, bn, SSH_BANNER_MAX);
        return 0;
    }
    case SSH_MSG_USERAUTH_SUCCESS:
        if (c->server || c->state != SSH_ST_AUTH) {
            ssh_fail(c, SSH_DISC_PROTOCOL_ERROR, "unexpected USERAUTH_SUCCESS");
            return -1;
        }
        ssh_auth_drop(c);
        c->state = SSH_ST_READY;
        c->deadline_ms = 0;
        ssh_js_connected(c);
        return 0;
    case SSH_MSG_USERAUTH_FAILURE: {
        size_t mn;
        const uint8_t* m = sshr_str(&r, &mn);
        if (c->server || c->state != SSH_ST_AUTH) {
            ssh_fail(c, SSH_DISC_PROTOCOL_ERROR, "unexpected USERAUTH_FAILURE");
            return -1;
        }
        return ssh_auth_failure(c, m, mn);
    }
    case SSH_MSG_GLOBAL_REQUEST: {
        size_t gn;
        int want;
        const uint8_t* gname = sshr_str(&r, &gn);
        want = sshr_bool(&r);
        if (r.err || c->state != SSH_ST_READY) {
            ssh_fail(c, SSH_DISC_PROTOCOL_ERROR, "malformed or unexpected global request");
            return -1;
        }
        return ssh_fwd_global(c, gname, gn, want, &r);
    }
    case SSH_MSG_REQUEST_SUCCESS:
    case SSH_MSG_REQUEST_FAILURE:
        if (c->state != SSH_ST_READY) {
            ssh_fail(c, SSH_DISC_PROTOCOL_ERROR, "unexpected global request reply");
            return -1;
        }
        return ssh_fwd_reply(c, t == SSH_MSG_REQUEST_SUCCESS, &r);
    case SSH_MSG_CHANNEL_OPEN: {
        size_t tn;
        uint32_t sender;
        if (c->server) {
            if (c->state != SSH_ST_READY) {
                ssh_fail(c, SSH_DISC_PROTOCOL_ERROR, "CHANNEL_OPEN before authentication");
                return -1;
            }
            return ssh_srv_chan_open(c, &r);
        }
        {
            const uint8_t* ty = sshr_str(&r, &tn);
            uint32_t win, maxp;
            sender = sshr_u32(&r);
            win = sshr_u32(&r);
            maxp = sshr_u32(&r);
            if (r.err || c->state != SSH_ST_READY || maxp == 0) {
                ssh_fail(c, SSH_DISC_PROTOCOL_ERROR, "unexpected or malformed CHANNEL_OPEN");
                return -1;
            }
            if (tn == 15 && memcmp(ty, "forwarded-tcpip", 15) == 0)
                return ssh_fwd_open_in(c, sender, win, maxp, &r);
        }
        ssh_msg_begin(c, SSH_MSG_CHANNEL_OPEN_FAILURE);
        m_u32(c, sender);
        m_u32(c, 1);
        m_cstr(c, "this client accepts no channels");
        m_cstr(c, "");
        return ssh_send_msg(c);
    }
    case SSH_MSG_CHANNEL_OPEN_CONFIRMATION:
    case SSH_MSG_CHANNEL_OPEN_FAILURE:
    case SSH_MSG_CHANNEL_WINDOW_ADJUST:
    case SSH_MSG_CHANNEL_DATA:
    case SSH_MSG_CHANNEL_EXTENDED_DATA:
    case SSH_MSG_CHANNEL_EOF:
    case SSH_MSG_CHANNEL_CLOSE:
    case SSH_MSG_CHANNEL_REQUEST:
    case SSH_MSG_CHANNEL_SUCCESS:
    case SSH_MSG_CHANNEL_FAILURE:
        if (c->state != SSH_ST_READY) {
            ssh_fail(c, SSH_DISC_PROTOCOL_ERROR, "channel message %u before authentication", (unsigned)t);
            return -1;
        }
        return on_channel(c, t, &r);
    case SSH_MSG_USERAUTH_PK_OK:
        if (c->server || c->state != SSH_ST_AUTH)
            break;
        return ssh_auth_pk_ok(c, &r);
    default:
        ssh_msg_begin(c, SSH_MSG_UNIMPLEMENTED);
        m_u32(c, seq);
        return ssh_send_msg(c);
    }
    ssh_fail(c, SSH_DISC_PROTOCOL_ERROR, "unexpected message %u", (unsigned)t);
    return -1;
}

static int read_version(ssh_conn_t* c)
{
    uint8_t* p = c->rbuf.p + c->rpos;
    size_t av = c->rbuf.len - c->rpos, i, ln;
    for (i = 0; i < av && p[i] != '\n'; i++) {
    }
    if (i == av) {
        if (av > 255 || c->pre_bytes + av > SSH_PRE_MAX) {
            ssh_fail(c, 0, "the %s sent no SSH identification line", c->server ? "client" : "server");
            return -1;
        }
        return 0;
    }
    ln = i;
    if (ln && p[ln - 1] == '\r')
        ln--;
    c->rpos += i + 1;
    c->pre_bytes += (uint32_t)(i + 1);
    if (ln >= 4 && memcmp(p, "SSH-", 4) == 0) {
        if (ln < 8 || memcmp(p, "SSH-2.0-", 8) != 0) {
            ssh_fail(c, SSH_DISC_PROTOCOL_ERROR, "the %s speaks %.*s; only SSH-2.0 is supported", c->server ? "client" : "server", (int)(ln > 40 ? 40 : ln), (const char*)p);
            return -1;
        }
        if (ln > 253 || memchr(p, 0, ln)) {
            ssh_fail(c, 0, "malformed identification line");
            return -1;
        }
        memcpy(c->server ? c->vc : c->vs, p, ln);
        (c->server ? c->vc : c->vs)[ln] = 0;
        c->state = SSH_ST_KEX;
        return 1;
    }
    if (c->server) {
        ssh_fail(c, 0, "the client's first line is not an SSH identification line");
        return -1;
    }
    if (c->pre_bytes > SSH_PRE_MAX) {
        ssh_fail(c, 0, "the server sent no SSH identification line");
        return -1;
    }
    return 1;
}

static int read_packet(ssh_conn_t* c)
{
    ssh_aead_t* a = &c->ain;
    uint8_t* p = c->rbuf.p + c->rpos;
    size_t av = c->rbuf.len - c->rpos, total;
    uint32_t L, seq;
    unsigned bs = a->bs ? a->bs : 8;
    uint8_t pad;
    if (!c->have_len) {
        if (av < 4)
            return 0;
        if (ssh_aead_len(a, c->seq_in, p, &L) < 0) {
            ssh_fail(c, SSH_DISC_PROTOCOL_ERROR, "cannot decrypt the packet length");
            return -1;
        }
        if (L < 8 || L > c->cfg.max_packet || (a->kind ? L % bs : (L + 4) % bs)) {
            ssh_fail(c, SSH_DISC_PROTOCOL_ERROR, "bad packet length %u", (unsigned)L);
            return -1;
        }
        c->cur_len = L;
        c->have_len = 1;
    }
    L = c->cur_len;
    total = 4 + (size_t)L + a->tag;
    if (av < total)
        return 0;
    seq = c->seq_in;
    if (a->kind && ssh_aead_open(a, seq, p, L) < 0) {
        ssh_fail(c, SSH_DISC_MAC_ERROR, "packet authentication failed (corrupted MAC on input)");
        return -1;
    }
    c->have_len = 0;
    pad = p[4];
    if (pad < 4 || (uint32_t)pad + 1 >= L) {
        ssh_fail(c, SSH_DISC_PROTOCOL_ERROR, "bad padding length %u", (unsigned)pad);
        return -1;
    }
    c->rpos += total;
    c->seq_in++;
    c->bytes_in += total;
    c->kex_bytes += total;
    c->pk_in++;
    if (c->seq_in == 0 && !c->kex_done_once) {
        ssh_fail(c, SSH_DISC_PROTOCOL_ERROR, "strict KEX: sequence number wrapped during the initial key exchange");
        return -1;
    }
    if (dispatch(c, p + 5, L - pad - 1, seq) < 0)
        return -1;
    return 1;
}

void ssh_input(ssh_conn_t* c, const uint8_t* buf, size_t len)
{
    if (c->state == SSH_ST_DEAD)
        return;
    c->busy++;
    c->last_io_ms = ssh_now_ms();
    if (sshb_put(&c->rbuf, buf, len) < 0) {
        ssh_fail(c, SSH_DISC_BY_APPLICATION, "out of memory buffering input");
        goto out;
    }
    while (c->state != SSH_ST_DEAD) {
        int rc = c->state == SSH_ST_VERSION ? read_version(c) : read_packet(c);
        if (rc <= 0)
            break;
    }
    if (c->rpos) {
        memmove(c->rbuf.p, c->rbuf.p + c->rpos, c->rbuf.len - c->rpos);
        c->rbuf.len -= c->rpos;
        c->rpos = 0;
    }
    q_trim(c, &c->rbuf);
    if (c->state == SSH_ST_READY && !c->kex_active && c->kex_bytes >= c->cfg.rekey_bytes)
        (void)ssh_start_rekey(c);
out:
    ssh_flush(c);
    c->busy--;
}

void ssh_tick(ssh_conn_t* c, uint64_t now)
{
    if (c->state == SSH_ST_DEAD)
        return;
    if (c->deadline_ms && now >= c->deadline_ms && c->state != SSH_ST_READY) {
        ssh_fail(c, SSH_DISC_BY_APPLICATION, "the handshake timed out after %llu ms", (unsigned long long)c->cfg.handshake_ms);
        return;
    }
    if (c->cfg.idle_ms && c->state == SSH_ST_READY && c->last_io_ms && now - c->last_io_ms >= c->cfg.idle_ms) {
        ssh_fail(c, SSH_DISC_BY_APPLICATION, "idle for %llu ms (dyna.idleTimeoutMs)", (unsigned long long)c->cfg.idle_ms);
        return;
    }
    if (c->state == SSH_ST_READY && !c->kex_active
        && ((c->cfg.rekey_ms && now - c->kex_at_ms >= c->cfg.rekey_ms) || c->kex_bytes >= c->cfg.rekey_bytes)) {
        (void)ssh_start_rekey(c);
        ssh_flush(c);
    }
}

#endif
