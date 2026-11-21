#include "ssh-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_SSH)

#include <stdio.h>
#include <openssl/crypto.h>

#define SSH_ENV_MAX 64

static int is_str(const uint8_t* p, size_t n, const char* s)
{
    return p && n == strlen(s) && memcmp(p, s, n) == 0;
}

int ssh_srv_kex_init(ssh_conn_t* c, sshr_t* r)
{
    ssh_srv_t* s = c->srv;
    const uint8_t* qc;
    size_t qn, qsn, want = c->kex_alg == SSH_KEX_MLKEM768X25519 ? 1184 + 32 : c->kex_alg == SSH_KEX_SNTRUP761X25519 ? SSH_SNTRUP_PK + 32 : 32;
    uint8_t qs[1088 + 32], xp[32], xs[32], ms[32], hh[64], h[64], sig[64];
    sshb_t kenc = { 0 }, sb = { 0 };
    const sshb_t* ks = c->hk_alg == SSH_HK_ED25519_CERT ? &s->hcert : &s->hblob;
    EVP_PKEY* xk;
    int rc = -1, bad;
    if (!c->kexinit_recv || c->reply_done) {
        ssh_fail(c, SSH_DISC_PROTOCOL_ERROR, "unexpected key exchange init");
        return -1;
    }
    qc = sshr_str(r, &qn);
    if (r->err || r->off != r->len || qn != want) {
        ssh_fail(c, SSH_DISC_KEY_EXCHANGE_FAILED, "malformed key exchange init (%zu bytes, %s expects %zu)", qn, ssh_kex_name(c->kex_alg), want);
        return -1;
    }
    xk = ssh_x25519_new(xp);
    if (!xk || ssh_x25519_derive(xk, qc + qn - 32, xs) < 0) {
        EVP_PKEY_free(xk);
        ssh_fail(c, SSH_DISC_KEY_EXCHANGE_FAILED, "invalid X25519 public value from the client");
        return -1;
    }
    EVP_PKEY_free(xk);
    if (c->kex_alg == SSH_KEX_MLKEM768X25519) {
        bad = ssh_mlkem_encap(qc, qs, ms) < 0;
        memcpy(qs + 1088, xp, 32);
        qsn = 1088 + 32;
    } else if (c->kex_alg == SSH_KEX_SNTRUP761X25519) {
        bad = ssh_sntrup_enc(qs, ms, qc) < 0;
        memcpy(qs + SSH_SNTRUP_CT, xp, 32);
        qsn = SSH_SNTRUP_CT + 32;
    } else {
        bad = 0;
        memcpy(qs, xp, 32);
        qsn = 32;
    }
    if (bad) {
        ssh_fail(c, SSH_DISC_KEY_EXCHANGE_FAILED, "%s encapsulation failed", ssh_kex_name(c->kex_alg));
        goto out;
    }
    if (c->kex_alg != SSH_KEX_CURVE25519) {
        if (ssh_hash2(c->hlen == 64, ms, 32, xs, 32, hh) < 0 || sshb_str(&kenc, hh, c->hlen) < 0)
            goto oom;
    } else if (sshb_mpint(&kenc, xs, 32) < 0)
        goto oom;
    if (ssh_kex_hash(c, ks->p, ks->len, qc, qn, qs, qsn, &kenc, h) < 0
        || ssh_ed25519_sign(s->hkey, h, c->hlen, sig) < 0
        || sshb_cstr(&sb, "ssh-ed25519") < 0 || sshb_str(&sb, sig, 64) < 0)
        goto oom;
    ssh_msg_begin(c, SSH_MSG_KEX_REPLY);
    ssh_m_str(c, ks->p, ks->len);
    ssh_m_str(c, qs, qsn);
    ssh_m_str(c, sb.p, sb.len);
    if (ssh_send_msg(c) < 0)
        goto out;
    if (ssh_kex_keys(c, &kenc, h) < 0) {
        ssh_fail(c, SSH_DISC_KEY_EXCHANGE_FAILED, "key derivation failed");
        goto out;
    }
    if (!c->kex_done_once) {
        c->hk0.len = 0;
        snprintf(c->hk_type, sizeof c->hk_type, "%s", ssh_hk_name(c->hk_alg));
    }
    c->reply_done = 1;
    rc = ssh_newkeys_out(c);
    goto out;
oom:
    ssh_fail(c, SSH_DISC_BY_APPLICATION, "out of memory during key exchange");
out:
    OPENSSL_cleanse(xs, sizeof xs);
    OPENSSL_cleanse(ms, sizeof ms);
    OPENSSL_cleanse(hh, sizeof hh);
    OPENSSL_cleanse(h, sizeof h);
    if (kenc.p)
        OPENSSL_cleanse(kenc.p, kenc.len);
    sshb_free(&kenc);
    sshb_free(&sb);
    return rc;
}

void ssh_srv_send_ext_info(ssh_conn_t* c)
{
    ssh_msg_begin(c, SSH_MSG_EXT_INFO);
    ssh_m_u32(c, 1);
    ssh_m_cstr(c, "server-sig-algs");
    ssh_m_cstr(c, "ssh-ed25519,sk-ssh-ed25519@openssh.com");
    (void)ssh_send_msg(c);
}

int ssh_srv_service(ssh_conn_t* c, sshr_t* r)
{
    size_t n;
    const uint8_t* name = sshr_str(r, &n);
    if (r->err || c->state != SSH_ST_SERVICE || !is_str(name, n, "ssh-userauth")) {
        ssh_fail(c, SSH_DISC_SERVICE_NOT_AVAILABLE, "service %.*s is not available", (int)(n > 60 ? 60 : n), name ? (const char*)name : "");
        return -1;
    }
    ssh_msg_begin(c, SSH_MSG_SERVICE_ACCEPT);
    ssh_m_cstr(c, "ssh-userauth");
    c->state = SSH_ST_AUTH;
    return ssh_send_msg(c);
}

static void auth_clear(ssh_conn_t* c)
{
    free(c->force_cmd);
    c->force_cmd = NULL;
    c->ucert.len = 0;
    c->cert_pty = 1;
    c->cert_fwd = 1;
    c->ufp[0] = c->ukt[0] = 0;
}

static int auth_fail(ssh_conn_t* c, int count)
{
    auth_clear(c);
    if (count && ++c->auth_tries >= c->cfg.max_auth_tries) {
        ssh_fail(c, SSH_DISC_NO_MORE_AUTH_METHODS, "too many authentication failures for %s (dyna.maxAuthTries is %d)", c->user ? c->user : "", c->cfg.max_auth_tries);
        return -1;
    }
    ssh_msg_begin(c, SSH_MSG_USERAUTH_FAILURE);
    ssh_m_cstr(c, "publickey");
    ssh_m_u8(c, 0);
    return ssh_send_msg(c);
}

static int auth_success(ssh_conn_t* c)
{
    ssh_msg_begin(c, SSH_MSG_USERAUTH_SUCCESS);
    c->state = SSH_ST_READY;
    c->deadline_ms = 0;
    return ssh_send_msg(c);
}

void ssh_srv_auth_done(ssh_conn_t* c, uint32_t seq, int accept)
{
    if (c->state != SSH_ST_AUTH || !c->auth_pending || seq != c->auth_seq)
        return;
    c->auth_pending = 0;
    if (accept)
        (void)auth_success(c);
    else
        (void)auth_fail(c, 1);
    ssh_flush(c);
}

int ssh_srv_userauth(ssh_conn_t* c, sshr_t* r)
{
    ssh_srv_t* s = c->srv;
    const uint8_t *user, *svc, *meth, *alg, *blob, *sig = NULL, *kt;
    size_t un, sn, mn, an, bn, sgn = 0, ktn;
    int has_sig, kind, listed = 0, pty = 1, rc;
    sshr_t kr;
    sshb_t plain = { 0 }, sd = { 0 };
    const uint8_t* vblob;
    size_t vn;
    char* force = NULL;
    char why[200];
    ssh_cert_t cert;
    if (c->state != SSH_ST_AUTH) {
        ssh_fail(c, SSH_DISC_PROTOCOL_ERROR, "USERAUTH_REQUEST outside the ssh-userauth service");
        return -1;
    }
    if (c->auth_pending) {
        ssh_fail(c, SSH_DISC_PROTOCOL_ERROR, "USERAUTH_REQUEST while the previous request is pending");
        return -1;
    }
    user = sshr_str(r, &un);
    svc = sshr_str(r, &sn);
    meth = sshr_str(r, &mn);
    if (r->err) {
        ssh_fail(c, SSH_DISC_PROTOCOL_ERROR, "malformed USERAUTH_REQUEST");
        return -1;
    }
    if (un == 0 || un > 255 || memchr(user, 0, un)) {
        ssh_fail(c, SSH_DISC_ILLEGAL_USER_NAME, "illegal user name");
        return -1;
    }
    if (!is_str(svc, sn, "ssh-connection")) {
        ssh_fail(c, SSH_DISC_SERVICE_NOT_AVAILABLE, "service %.*s is not available", (int)(sn > 60 ? 60 : sn), (const char*)svc);
        return -1;
    }
    if (c->user && (strlen(c->user) != un || memcmp(c->user, user, un) != 0)) {
        ssh_fail(c, SSH_DISC_PROTOCOL_ERROR, "the user name changed during authentication");
        return -1;
    }
    if (!c->user) {
        c->user = (char*)malloc(un + 1);
        if (!c->user) {
            ssh_fail(c, SSH_DISC_BY_APPLICATION, "out of memory");
            return -1;
        }
        memcpy(c->user, user, un);
        c->user[un] = 0;
    }
    if (s->banner && !c->banner_sent) {
        c->banner_sent = 1;
        ssh_msg_begin(c, SSH_MSG_USERAUTH_BANNER);
        ssh_m_cstr(c, s->banner);
        ssh_m_cstr(c, "");
        if (ssh_send_msg(c) < 0)
            return -1;
    }
    if (is_str(meth, mn, "none"))
        return auth_fail(c, 0);
    if (!is_str(meth, mn, "publickey"))
        return auth_fail(c, 1);
    has_sig = sshr_bool(r);
    alg = sshr_str(r, &an);
    blob = sshr_str(r, &bn);
    if (has_sig)
        sig = sshr_str(r, &sgn);
    if (r->err || r->off != r->len) {
        ssh_fail(c, SSH_DISC_PROTOCOL_ERROR, "malformed publickey USERAUTH_REQUEST");
        return -1;
    }
    kind = is_str(alg, an, "ssh-ed25519") ? 0 : is_str(alg, an, "sk-ssh-ed25519@openssh.com") ? 1 : is_str(alg, an, "ssh-ed25519-cert-v01@openssh.com") ? 2 : -1;
    sshr_init(&kr, blob, bn);
    kt = sshr_str(&kr, &ktn);
    if (kind < 0 || kr.err || ktn != an || memcmp(kt, alg, an) != 0)
        return auth_fail(c, 1);
    vblob = blob;
    vn = bn;
    if (kind == 2) {
        if (ssh_cert_parse(blob, bn, &cert, why, sizeof why) < 0)
            return auth_fail(c, 1);
        listed = ssh_blob_list_has(&s->cakeys, cert.ca, cert.ca_len)
            && ssh_cert_check(&cert, SSH_CERT_USER, c->user, c->peer, &force, &pty, why, sizeof why) == 0;
        ssh_ed25519_blob(&plain, cert.pk);
        if (!plain.len) {
            free(force);
            ssh_fail(c, SSH_DISC_BY_APPLICATION, "out of memory");
            return -1;
        }
        vblob = plain.p;
        vn = plain.len;
    } else
        listed = ssh_blob_list_has(&s->akeys, blob, bn);
    if (kind == 2 && !listed) {
        free(force);
        sshb_free(&plain);
        return auth_fail(c, 1);
    }
    if (!has_sig) {
        free(force);
        sshb_free(&plain);
        if (!listed && !c->srv_hook)
            return auth_fail(c, 1);
        ssh_msg_begin(c, SSH_MSG_USERAUTH_PK_OK);
        ssh_m_str(c, alg, an);
        ssh_m_str(c, blob, bn);
        return ssh_send_msg(c);
    }
    if (sshb_str(&sd, c->sid, c->sidlen) < 0 || sshb_u8(&sd, SSH_MSG_USERAUTH_REQUEST) < 0
        || sshb_str(&sd, user, un) < 0 || sshb_cstr(&sd, "ssh-connection") < 0
        || sshb_cstr(&sd, "publickey") < 0 || sshb_u8(&sd, 1) < 0
        || sshb_str(&sd, alg, an) < 0 || sshb_str(&sd, blob, bn) < 0) {
        free(force);
        sshb_free(&plain);
        sshb_free(&sd);
        ssh_fail(c, SSH_DISC_BY_APPLICATION, "out of memory");
        return -1;
    }
    rc = ssh_verify_usersig(vblob, vn, sig, sgn, sd.p, sd.len, why, sizeof why);
    sshb_free(&sd);
    if (rc < 0) {
        free(force);
        sshb_free(&plain);
        return auth_fail(c, 1);
    }
    auth_clear(c);
    c->force_cmd = force;
    c->cert_pty = kind == 2 ? pty : 1;
    c->cert_fwd = kind == 2 ? ssh_cert_ext_has(&cert, "permit-port-forwarding") : 1;
    if (kind == 2 && sshb_put(&c->ucert, blob, bn) < 0) {
        sshb_free(&plain);
        ssh_fail(c, SSH_DISC_BY_APPLICATION, "out of memory");
        return -1;
    }
    ssh_fingerprint(vblob, vn, c->ufp);
    snprintf(c->ukt, sizeof c->ukt, "%.*s", (int)an, (const char*)alg);
    if (!c->srv_hook) {
        sshb_free(&plain);
        return listed ? auth_success(c) : auth_fail(c, 1);
    }
    rc = ssh_js_authorize(c, c->user, c->ukt, vblob, vn, kind == 2 ? blob : NULL, kind == 2 ? bn : 0, listed);
    sshb_free(&plain);
    if (c->state == SSH_ST_DEAD)
        return -1;
    if (rc == 2)
        return 0;
    return rc == 1 ? auth_success(c) : auth_fail(c, 1);
}

int ssh_srv_chan_open(ssh_conn_t* c, sshr_t* r)
{
    size_t tn;
    const uint8_t* ty = sshr_str(r, &tn);
    uint32_t sender = sshr_u32(r), win = sshr_u32(r), maxp = sshr_u32(r);
    const char* why = NULL;
    uint32_t code = 0;
    ssh_chan_t* ch = NULL;
    if (r->err || maxp == 0) {
        ssh_fail(c, SSH_DISC_PROTOCOL_ERROR, "malformed CHANNEL_OPEN");
        return -1;
    }
    if (is_str(ty, tn, "direct-tcpip"))
        return ssh_fwd_open_in(c, sender, win, maxp, r);
    if (!is_str(ty, tn, "session")) {
        code = 3;
        why = "only session and direct-tcpip channels are supported";
    } else if (c->nopen >= c->cfg.max_channels) {
        code = 4;
        why = "too many channels (dyna.maxChannels)";
    } else {
        ch = (ssh_chan_t*)calloc(1, sizeof *ch);
        if (ch) {
            ch->ctl[0] = ch->ctl[1] = ch->open_res = ch->open_rej = ch->exit_res = ch->jsobj = JS_UNDEFINED;
            ch->refs = 1;
            ch->srv = 1;
            ch->rid = sender;
            ch->rwin = win;
            ch->rmax = maxp > (1u << 20) ? (1u << 20) : maxp;
            ch->win_size = ch->lwin = c->cfg.window;
            ch->lmax = c->cfg.chan_packet;
            ch->state = SSH_CH_REQUESTING;
            if (ssh_chan_slot(c, ch) < 0) {
                ssh_chan_unref(c->rt, ch);
                ch = NULL;
            }
        }
        if (!ch) {
            code = 4;
            why = "out of channel resources";
        }
    }
    if (!ch) {
        ssh_msg_begin(c, SSH_MSG_CHANNEL_OPEN_FAILURE);
        ssh_m_u32(c, sender);
        ssh_m_u32(c, code);
        ssh_m_cstr(c, why);
        ssh_m_cstr(c, "");
        return ssh_send_msg(c);
    }
    ssh_msg_begin(c, SSH_MSG_CHANNEL_OPEN_CONFIRMATION);
    ssh_m_u32(c, ch->rid);
    ssh_m_u32(c, ch->lid);
    ssh_m_u32(c, (uint32_t)ch->lwin);
    ssh_m_u32(c, ch->lmax);
    return ssh_send_msg(c);
}

static int env_ok(const uint8_t* n, size_t nn, const uint8_t* v, size_t vn)
{
    size_t i;
    if (nn == 0 || nn > 64 || vn > 1024 || memchr(v, 0, vn))
        return 0;
    if (!(is_str(n, nn, "LANG") || (nn > 3 && memcmp(n, "LC_", 3) == 0)))
        return 0;
    for (i = 0; i < nn; i++)
        if (!((n[i] >= 'A' && n[i] <= 'Z') || (n[i] >= '0' && n[i] <= '9') || n[i] == '_'))
            return 0;
    return 1;
}

int ssh_srv_chan_request(ssh_conn_t* c, ssh_chan_t* ch, sshr_t* r)
{
    size_t tn;
    const uint8_t* ty = sshr_str(r, &tn);
    int want = sshr_bool(r), ok = 0, start = 0;
    const uint8_t* cmd = NULL;
    size_t cmdlen = 0;
    if (r->err) {
        ssh_fail(c, SSH_DISC_PROTOCOL_ERROR, "malformed channel request");
        return -1;
    }
    if (ch->fwd) {
        if (!want)
            return 0;
        ssh_msg_begin(c, SSH_MSG_CHANNEL_FAILURE);
        ssh_m_u32(c, ch->rid);
        return ssh_send_msg(c);
    }
    if (is_str(ty, tn, "pty-req")) {
        size_t termn, mdn;
        const uint8_t* term = sshr_str(r, &termn);
        uint32_t cols = sshr_u32(r), rows = sshr_u32(r), w = sshr_u32(r), hgt = sshr_u32(r);
        const uint8_t* modes = sshr_str(r, &mdn);
        if (!r->err && !ch->started && !ch->has_pty && c->cert_pty && termn <= 64 && !memchr(term, 0, termn) && mdn <= 4096) {
            free(ch->term);
            ch->term = (char*)malloc(termn + 1);
            if (ch->term) {
                memcpy(ch->term, term, termn);
                ch->term[termn] = 0;
                ch->cols = cols;
                ch->rows = rows;
                ch->width = w;
                ch->height = hgt;
                ch->modes.len = 0;
                ok = sshb_put(&ch->modes, modes, mdn) == 0;
                ch->has_pty = ok;
            }
        }
    } else if (is_str(ty, tn, "env")) {
        size_t nn, vn;
        const uint8_t* n = sshr_str(r, &nn);
        const uint8_t* v = sshr_str(r, &vn);
        if (!r->err && !ch->started && ch->nenv < SSH_ENV_MAX && env_ok(n, nn, v, vn))
            ok = sshb_str(&ch->env, n, nn) == 0 && sshb_str(&ch->env, v, vn) == 0 && ++ch->nenv;
    } else if (is_str(ty, tn, "exec") || is_str(ty, tn, "shell")) {
        if (tn == 4) {
            cmd = sshr_str(r, &cmdlen);
            if (cmd && memchr(cmd, 0, cmdlen))
                cmd = NULL;
        }
        if (!r->err && !ch->started && (tn == 5 || cmd) && c->srv && c->srv_session) {
            ok = 1;
            start = 1;
        }
    } else if (is_str(ty, tn, "subsystem")) {
        cmd = sshr_str(r, &cmdlen);
        if (cmd && (cmdlen == 0 || cmdlen > 64 || memchr(cmd, 0, cmdlen)))
            cmd = NULL;
        if (!r->err && !ch->started && cmd && c->srv && c->srv_session) {
            ok = 1;
            start = 1;
        }
    } else if (is_str(ty, tn, "signal")) {
        size_t nn;
        const uint8_t* n = sshr_str(r, &nn);
        if (!r->err && ch->started && nn > 0 && nn <= 32) {
            char name[40];
            memcpy(name, n, nn);
            name[nn] = 0;
            ok = 1;
            ssh_js_signal(ch, name);
        }
    } else if (is_str(ty, tn, "window-change")) {
        uint32_t cols = sshr_u32(r), rows = sshr_u32(r), w = sshr_u32(r), hgt = sshr_u32(r);
        if (!r->err && ch->has_pty) {
            ch->cols = cols;
            ch->rows = rows;
            ch->width = w;
            ch->height = hgt;
            ok = 1;
            if (ch->started)
                ssh_js_resize(ch);
        }
    }
    if (!ch->conn || c->state == SSH_ST_DEAD)
        return c->state == SSH_ST_DEAD ? -1 : 0;
    if (want) {
        ssh_msg_begin(c, ok ? SSH_MSG_CHANNEL_SUCCESS : SSH_MSG_CHANNEL_FAILURE);
        ssh_m_u32(c, ch->rid);
        if (ssh_send_msg(c) < 0)
            return -1;
    }
    if (start) {
        ch->started = 1;
        ch->state = SSH_CH_OPEN;
        if (ssh_js_session(c, ch, tn == 4 ? "exec" : tn == 5 ? "shell" : "subsystem", cmd, cmdlen) < 0 && ch->conn && !ch->want_exit) {
            ch->want_eof = ch->want_exit = 1;
            ch->exit_kind = 1;
            ch->exit_code = 1;
            ch->has_status = 1;
        }
        if (ch->conn) {
            ssh_chan_window(ch);
            ssh_chan_pump(ch);
        }
    }
    return c->state == SSH_ST_DEAD ? -1 : 0;
}

#endif
