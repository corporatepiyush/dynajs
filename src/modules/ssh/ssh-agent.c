#include "ssh-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_SSH)

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>

#define AG_FAILURE 5
#define AG_SUCCESS 6
#define AG_REQ_IDS 11
#define AG_IDS 12
#define AG_SIGN 13
#define AG_SIGN_RESP 14
#define AG_ADD 17
#define AG_REMOVE 18
#define AG_REMOVE_ALL 19
#define AG_LOCK 22
#define AG_UNLOCK 23
#define AG_ADD_C 25
#define AG_EXT 27
#define AG_C_LIFETIME 1
#define AG_C_CONFIRM 2
#define AG_C_EXT 255
#define AG_LOCK_ITER 50000

static const char* const kind_type[3] = { "ssh-ed25519", "sk-ssh-ed25519@openssh.com", "ssh-ed25519-cert-v01@openssh.com" };

static int is_n(const uint8_t* p, size_t n, const char* s)
{
    return p && n == strlen(s) && memcmp(p, s, n) == 0;
}

int ssh_agent_blob_kind(const uint8_t* blob, size_t n)
{
    sshr_t r;
    size_t tn;
    const uint8_t* t;
    int k;
    sshr_init(&r, blob, n);
    t = sshr_str(&r, &tn);
    if (r.err)
        return -1;
    for (k = 0; k < 3; k++)
        if (is_n(t, tn, kind_type[k]))
            return k;
    return -1;
}

static void akey_free(ssh_akey_t* k)
{
    if (!k)
        return;
    EVP_PKEY_free(k->k);
    sshb_free(&k->blob);
    free(k->comment);
    free(k);
}

static void store_unlink(ssh_agent_store_t* st, ssh_akey_t* k)
{
    ssh_akey_t** pp;
    ssh_akey_t* prev = NULL;
    for (pp = &st->keys; *pp; prev = *pp, pp = &(*pp)->next)
        if (*pp == k) {
            *pp = k->next;
            if (st->tail == k)
                st->tail = prev;
            st->nkeys--;
            akey_free(k);
            return;
        }
}

void ssh_agent_store_clear(ssh_agent_store_t* st)
{
    while (st->keys)
        store_unlink(st, st->keys);
}

void ssh_agent_store_expire(ssh_agent_store_t* st, uint64_t now)
{
    ssh_akey_t *k = st->keys, *n;
    while (k) {
        n = k->next;
        if (k->expires_ms && k->expires_ms <= now)
            store_unlink(st, k);
        k = n;
    }
}

static ssh_akey_t* store_find(ssh_agent_store_t* st, const uint8_t* blob, size_t n)
{
    ssh_akey_t* k;
    for (k = st->keys; k; k = k->next)
        if (k->blob.len == n && memcmp(k->blob.p, blob, n) == 0)
            return k;
    return NULL;
}

int ssh_agent_store_put(ssh_agent_store_t* st, ssh_akey_t* nk)
{
    ssh_akey_t* old = store_find(st, nk->blob.p, nk->blob.len);
    if (old) {
        EVP_PKEY_free(old->k);
        old->k = nk->k;
        nk->k = NULL;
        free(old->comment);
        old->comment = nk->comment;
        nk->comment = NULL;
        old->expires_ms = nk->expires_ms;
        akey_free(nk);
        return 0;
    }
    if (st->nkeys >= st->max_keys)
        return -1;
    nk->next = NULL;
    if (st->tail)
        st->tail->next = nk;
    else
        st->keys = nk;
    st->tail = nk;
    st->nkeys++;
    return 0;
}

int ssh_agent_store_remove(ssh_agent_store_t* st, const uint8_t* blob, size_t n)
{
    ssh_akey_t* k = store_find(st, blob, n);
    if (!k)
        return 0;
    store_unlink(st, k);
    return 1;
}

ssh_akey_t* ssh_agent_key_from(int kind, EVP_PKEY* pk, const uint8_t* blob, size_t bn, const char* comment, size_t cn)
{
    ssh_akey_t* k = (ssh_akey_t*)calloc(1, sizeof *k);
    if (!k)
        return NULL;
    k->kind = kind;
    k->k = pk;
    k->comment = (char*)malloc(cn + 1);
    if (!k->comment || sshb_put(&k->blob, blob, bn) < 0) {
        k->k = NULL;
        akey_free(k);
        return NULL;
    }
    memcpy(k->comment, comment ? comment : "", cn);
    k->comment[cn] = 0;
    return k;
}

static EVP_PKEY* ed_from_wire(const uint8_t* pub, size_t pn, const uint8_t* priv, size_t vn)
{
    EVP_PKEY* k;
    uint8_t chk[32];
    size_t cl = sizeof chk;
    if (pn != 32 || vn != 64 || memcmp(priv + 32, pub, 32) != 0)
        return NULL;
    k = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL, priv, 32);
    if (!k)
        return NULL;
    if (EVP_PKEY_get_raw_public_key(k, chk, &cl) != 1 || cl != 32 || memcmp(chk, pub, 32) != 0) {
        EVP_PKEY_free(k);
        return NULL;
    }
    return k;
}

static ssh_akey_t* parse_add(sshr_t* r)
{
    size_t tn, pn, vn, cn, an, hn, rn, bn;
    const uint8_t *t = sshr_str(r, &tn), *pub, *priv, *cm, *blobp = NULL;
    sshb_t blob = { 0 };
    EVP_PKEY* pk = NULL;
    ssh_akey_t* k = NULL;
    int kind = -1;
    if (is_n(t, tn, kind_type[0])) {
        kind = 0;
        pub = sshr_str(r, &pn);
        priv = sshr_str(r, &vn);
        cm = sshr_str(r, &cn);
        if (r->err || pn != 32)
            return NULL;
        pk = ed_from_wire(pub, pn, priv, vn);
        ssh_ed25519_blob(&blob, pub);
    } else if (is_n(t, tn, kind_type[1])) {
        const uint8_t* app;
        kind = 1;
        pub = sshr_str(r, &pn);
        app = sshr_str(r, &an);
        (void)sshr_u8(r);
        (void)sshr_str(r, &hn);
        (void)sshr_str(r, &rn);
        cm = sshr_str(r, &cn);
        if (r->err || pn != 32 || an < 4 || memcmp(app, "ssh:", 4) != 0 || hn == 0)
            return NULL;
        if (sshb_cstr(&blob, kind_type[1]) < 0 || sshb_str(&blob, pub, 32) < 0 || sshb_str(&blob, app, an) < 0) {
            sshb_free(&blob);
            return NULL;
        }
    } else if (is_n(t, tn, kind_type[2])) {
        ssh_cert_t ct;
        char why[120];
        kind = 2;
        blobp = sshr_str(r, &bn);
        pub = sshr_str(r, &pn);
        priv = sshr_str(r, &vn);
        cm = sshr_str(r, &cn);
        if (r->err || pn != 32 || ssh_cert_parse(blobp, bn, &ct, why, sizeof why) < 0 || ct.type != SSH_CERT_USER || memcmp(ct.pk, pub, 32) != 0)
            return NULL;
        pk = ed_from_wire(pub, pn, priv, vn);
        if (sshb_put(&blob, blobp, bn) < 0) {
            EVP_PKEY_free(pk);
            return NULL;
        }
    } else
        return NULL;
    if ((kind != 1 && !pk) || !blob.len || cn > 4096) {
        EVP_PKEY_free(pk);
        sshb_free(&blob);
        return NULL;
    }
    k = ssh_agent_key_from(kind, pk, blob.p, blob.len, (const char*)cm, cn);
    if (!k)
        EVP_PKEY_free(pk);
    sshb_free(&blob);
    return k;
}

static int parse_constraints(sshr_t* r, int kind, uint64_t now, uint64_t* expires)
{
    while (r->off < r->len) {
        uint8_t c = sshr_u8(r);
        if (r->err)
            return -1;
        if (c == AG_C_LIFETIME) {
            uint32_t s = sshr_u32(r);
            if (r->err || s == 0)
                return -1;
            *expires = now + (uint64_t)s * 1000;
        } else if (c == AG_C_EXT) {
            size_t nn, dn;
            const uint8_t* nm = sshr_str(r, &nn);
            if (r->err || kind != 1 || !is_n(nm, nn, "sk-provider@openssh.com"))
                return -1;
            (void)sshr_str(r, &dn);
            if (r->err)
                return -1;
        } else
            return -1;
    }
    return 0;
}

static void lock_hash(const uint8_t* pass, size_t n, const uint8_t salt[16], uint8_t out[64])
{
    if (PKCS5_PBKDF2_HMAC((const char*)pass, (int)n, salt, 16, AG_LOCK_ITER, EVP_sha512(), 64, out) != 1)
        ssh_random(out, 64);
}

static int put_ids(ssh_agent_store_t* st, sshb_t* out)
{
    ssh_akey_t* k;
    uint32_t n = 0;
    if (!st->locked)
        for (k = st->keys; k; k = k->next)
            n++;
    if (sshb_u8(out, AG_IDS) < 0 || sshb_u32(out, n) < 0)
        return -1;
    if (!st->locked)
        for (k = st->keys; k; k = k->next)
            if (sshb_str(out, k->blob.p, k->blob.len) < 0 || sshb_cstr(out, k->comment) < 0)
                return -1;
    return 0;
}

static int do_sign(ssh_agent_store_t* st, sshr_t* r, sshb_t* out)
{
    size_t bn, dn;
    const uint8_t* blob = sshr_str(r, &bn);
    const uint8_t* data = sshr_str(r, &dn);
    ssh_akey_t* k;
    uint8_t sig[64];
    sshb_t sb = { 0 };
    int rc;
    (void)sshr_u32(r);
    if (r->err || st->locked || !(k = store_find(st, blob, bn)) || !k->k) {
        st->refused++;
        return 1;
    }
    if (ssh_ed25519_sign(k->k, data, dn, sig) < 0)
        return 1;
    rc = sshb_cstr(&sb, "ssh-ed25519") < 0 || sshb_str(&sb, sig, 64) < 0 || sshb_u8(out, AG_SIGN_RESP) < 0
        || sshb_str(out, sb.p, sb.len) < 0;
    sshb_free(&sb);
    OPENSSL_cleanse(sig, sizeof sig);
    if (rc)
        return -1;
    st->signs++;
    return 0;
}

int ssh_agent_handle(ssh_agent_store_t* st, const uint8_t* req, size_t n, uint64_t now, sshb_t* out)
{
    sshr_t r;
    uint8_t t;
    int fail = 1;
    out->len = 0;
    st->requests++;
    ssh_agent_store_expire(st, now);
    sshr_init(&r, req, n);
    t = sshr_u8(&r);
    if (r.err)
        t = 0;
    switch (t) {
    case AG_REQ_IDS:
        return put_ids(st, out);
    case AG_SIGN: {
        int rc = do_sign(st, &r, out);
        if (rc <= 0)
            return rc;
        break;
    }
    case AG_ADD:
    case AG_ADD_C: {
        ssh_akey_t* k;
        uint64_t exp = 0;
        if (st->locked)
            break;
        k = parse_add(&r);
        if (!k)
            break;
        if ((t == AG_ADD && r.off != r.len) || (t == AG_ADD_C && parse_constraints(&r, k->kind, now, &exp) < 0)) {
            akey_free(k);
            break;
        }
        k->expires_ms = exp;
        fail = ssh_agent_store_put(st, k) < 0;
        if (fail)
            akey_free(k);
        break;
    }
    case AG_REMOVE: {
        size_t bn;
        const uint8_t* b = sshr_str(&r, &bn);
        fail = r.err || st->locked || !ssh_agent_store_remove(st, b, bn);
        break;
    }
    case AG_REMOVE_ALL:
        if (!st->locked) {
            ssh_agent_store_clear(st);
            fail = 0;
        }
        break;
    case AG_LOCK:
    case AG_UNLOCK: {
        size_t pn;
        const uint8_t* p = sshr_str(&r, &pn);
        uint8_t h[64];
        if (r.err)
            break;
        if (t == AG_LOCK && !st->locked) {
            ssh_random(st->lsalt, sizeof st->lsalt);
            lock_hash(p, pn, st->lsalt, st->lhash);
            st->locked = 1;
            fail = 0;
        } else if (t == AG_UNLOCK && st->locked) {
            lock_hash(p, pn, st->lsalt, h);
            if (CRYPTO_memcmp(h, st->lhash, 64) == 0) {
                st->locked = 0;
                OPENSSL_cleanse(st->lhash, sizeof st->lhash);
                fail = 0;
            } else
                st->refused++;
            OPENSSL_cleanse(h, sizeof h);
        }
        break;
    }
    default:
        break;
    }
    return sshb_u8(out, fail ? AG_FAILURE : AG_SUCCESS);
}

static pthread_mutex_t live_mu = PTHREAD_MUTEX_INITIALIZER;
static ssh_agent_srv_t* live_head;
static int live_hooked;

static void live_unlink_all(void)
{
    ssh_agent_srv_t* a;
    pthread_mutex_lock(&live_mu);
    for (a = live_head; a; a = a->live_next)
        if (a->path)
            unlink(a->path);
    pthread_mutex_unlock(&live_mu);
}

static void live_add(ssh_agent_srv_t* a)
{
    pthread_mutex_lock(&live_mu);
    if (!live_hooked)
        live_hooked = atexit(live_unlink_all) == 0;
    a->live_prev = NULL;
    a->live_next = live_head;
    if (live_head)
        live_head->live_prev = a;
    live_head = a;
    a->live = 1;
    pthread_mutex_unlock(&live_mu);
}

static void live_del(ssh_agent_srv_t* a)
{
    pthread_mutex_lock(&live_mu);
    if (a->live) {
        if (a->live_prev)
            a->live_prev->live_next = a->live_next;
        else
            live_head = a->live_next;
        if (a->live_next)
            a->live_next->live_prev = a->live_prev;
        a->live = 0;
    }
    pthread_mutex_unlock(&live_mu);
}

typedef struct ssh_agconn {
    struct ssh_agent_srv* a;
    int fd;
    int refs;
    int busy;
    sshb_t in;
    struct ssh_agconn* next;
    struct ssh_agconn* prev;
} ssh_agconn_t;

static void agconn_unref(ssh_agconn_t* x)
{
    if (--x->refs == 0) {
        sshb_free(&x->in);
        free(x);
    }
}

static void agconn_close(ssh_agconn_t* x)
{
    ssh_agent_srv_t* a = x->a;
    int fd = x->fd;
    if (fd < 0)
        return;
    x->fd = -1;
    if (x->prev)
        x->prev->next = x->next;
    else if (a)
        a->conns = x->next;
    if (x->next)
        x->next->prev = x->prev;
    if (a)
        a->nconns--;
    x->a = NULL;
    dyn_aio_close(a ? a->aio : NULL, fd);
    if (x->in.p)
        OPENSSL_cleanse(x->in.p, x->in.cap);
    agconn_unref(x);
}

static void agconn_sent(dyn_aio_t* aio, int res, const uint8_t* buf, unsigned len, void* ud)
{
    ssh_agconn_t* x = (ssh_agconn_t*)ud;
    (void)aio;
    (void)buf;
    (void)len;
    if (res < 0 && x->fd >= 0)
        agconn_close(x);
    agconn_unref(x);
}

typedef struct {
    ssh_agconn_t* x;
    uint8_t t;
    int rc;
    EVP_PKEY* k;
    uint8_t* d;
    size_t dn;
    uint8_t salt[16];
    uint8_t h[64];
    uint8_t sig[64];
} ssh_agjob_t;

static void agconn_pump(ssh_agconn_t* x);

static int agconn_reply(ssh_agconn_t* x, const sshb_t* out)
{
    uint8_t hdr[4];
    ssh_put_be32(hdr, (uint32_t)out->len);
    x->refs++;
    if (dyn_aio_send2(x->a->aio, x->fd, hdr, 4, out->p, out->len, 0, agconn_sent, x) < 0) {
        agconn_unref(x);
        agconn_close(x);
        return -1;
    }
    return 0;
}

static void agjob_work(void* p)
{
    ssh_agjob_t* j = (ssh_agjob_t*)p;
    if (j->t == AG_SIGN)
        j->rc = ssh_ed25519_sign(j->k, j->d, j->dn, j->sig);
    else
        lock_hash(j->d, j->dn, j->salt, j->h);
}

static void agjob_free(ssh_agjob_t* j)
{
    EVP_PKEY_free(j->k);
    if (j->d) {
        OPENSSL_cleanse(j->d, j->dn);
        free(j->d);
    }
    OPENSSL_cleanse(j, sizeof *j);
    free(j);
}

static void agjob_done(void* p)
{
    ssh_agjob_t* j = (ssh_agjob_t*)p;
    ssh_agconn_t* x = j->x;
    ssh_agent_srv_t* a = x->a;
    sshb_t out = { 0 }, sb = { 0 };
    int fail = 1;
    x->busy = 0;
    if (x->fd >= 0 && a) {
        ssh_agent_store_t* st = &a->st;
        if (j->t == AG_SIGN) {
            if (j->rc == 0 && !st->locked && sshb_cstr(&sb, "ssh-ed25519") == 0 && sshb_str(&sb, j->sig, 64) == 0 && sshb_u8(&out, AG_SIGN_RESP) == 0
                && sshb_str(&out, sb.p, sb.len) == 0) {
                st->signs++;
                fail = 0;
            } else
                out.len = 0;
        } else if (j->t == AG_LOCK) {
            if (!st->locked) {
                memcpy(st->lsalt, j->salt, 16);
                memcpy(st->lhash, j->h, 64);
                st->locked = 1;
                fail = 0;
            }
        } else if (st->locked && memcmp(st->lsalt, j->salt, 16) == 0 && CRYPTO_memcmp(st->lhash, j->h, 64) == 0) {
            st->locked = 0;
            OPENSSL_cleanse(st->lhash, sizeof st->lhash);
            fail = 0;
        } else
            st->refused++;
        if (fail) {
            out.len = 0;
            if (j->t == AG_SIGN)
                st->refused++;
        }
        if ((fail || j->t != AG_SIGN) && sshb_u8(&out, fail ? AG_FAILURE : AG_SUCCESS) < 0)
            agconn_close(x);
        else if (agconn_reply(x, &out) == 0)
            agconn_pump(x);
    }
    if (sb.p)
        OPENSSL_cleanse(sb.p, sb.cap);
    sshb_free(&sb);
    if (out.p)
        OPENSSL_cleanse(out.p, out.cap);
    sshb_free(&out);
    agjob_free(j);
    agconn_unref(x);
}

static int agconn_offload(ssh_agconn_t* x, const uint8_t* req, size_t n)
{
    ssh_agent_srv_t* a = x->a;
    ssh_agent_store_t* st = &a->st;
    ssh_agjob_t* j;
    sshr_t r;
    uint8_t t;
    const uint8_t *p, *d = NULL;
    size_t pn, dn = 0;
    ssh_akey_t* k = NULL;
    sshr_init(&r, req, n);
    t = sshr_u8(&r);
    if (r.err || (t != AG_SIGN && t != AG_LOCK && t != AG_UNLOCK))
        return 0;
    p = sshr_str(&r, &pn);
    if (t == AG_SIGN) {
        d = sshr_str(&r, &dn);
        (void)sshr_u32(&r);
        if (r.err || st->locked)
            return 0;
        ssh_agent_store_expire(st, ssh_now_ms());
        if (!(k = store_find(st, p, pn)) || !k->k)
            return 0;
    } else {
        if (r.err || (t == AG_LOCK) == (st->locked != 0))
            return 0;
        d = p;
        dn = pn;
    }
    j = (ssh_agjob_t*)calloc(1, sizeof *j);
    if (!j)
        return 0;
    j->x = x;
    j->t = t;
    j->d = (uint8_t*)malloc(dn ? dn : 1);
    if (!j->d) {
        free(j);
        return 0;
    }
    if (dn)
        memcpy(j->d, d, dn);
    j->dn = dn;
    if (t == AG_SIGN) {
        j->k = k->k;
        EVP_PKEY_up_ref(j->k);
    } else if (t == AG_LOCK)
        ssh_random(j->salt, sizeof j->salt);
    else
        memcpy(j->salt, st->lsalt, 16);
    x->refs++;
    x->busy = 1;
    if (dyn_aio_offload_cpu(a->aio, agjob_work, agjob_done, j) < 0) {
        x->busy = 0;
        x->refs--;
        agjob_free(j);
        return 0;
    }
    st->requests++;
    a->offloaded++;
    return 1;
}

static void agconn_pump(ssh_agconn_t* x)
{
    ssh_agent_srv_t* a = x->a;
    sshb_t out = { 0 };
    size_t off = 0;
    if (x->fd < 0 || !a)
        return;
    x->refs++;
    while (x->fd >= 0 && !x->busy && x->in.len - off >= 4) {
        uint32_t L = ssh_be32(x->in.p + off);
        if (L == 0 || L > SSH_AGENT_MAX_MSG) {
            agconn_close(x);
            break;
        }
        if (x->in.len - off < 4 + (size_t)L)
            break;
        if (agconn_offload(x, x->in.p + off + 4, L)) {
            off += 4 + (size_t)L;
            break;
        }
        if (ssh_agent_handle(&a->st, x->in.p + off + 4, L, ssh_now_ms(), &out) < 0 || out.len == 0) {
            agconn_close(x);
            break;
        }
        off += 4 + (size_t)L;
        if (agconn_reply(x, &out) < 0)
            break;
    }
    if (x->fd >= 0) {
        if (off) {
            OPENSSL_cleanse(x->in.p, off);
            memmove(x->in.p, x->in.p + off, x->in.len - off);
            x->in.len -= off;
        }
        if (x->in.len > SSH_AGENT_MAX_MSG + 4)
            agconn_close(x);
    }
    if (out.p)
        OPENSSL_cleanse(out.p, out.cap);
    sshb_free(&out);
    agconn_unref(x);
}

static void agconn_recv(dyn_aio_t* aio, int res, const uint8_t* buf, unsigned len, void* ud)
{
    ssh_agconn_t* x = (ssh_agconn_t*)ud;
    (void)aio;
    if (x->fd < 0 || !x->a)
        return;
    if (res < 0 || len == 0 || sshb_put(&x->in, buf, len) < 0) {
        agconn_close(x);
        return;
    }
    agconn_pump(x);
}

static int peer_ok(int fd)
{
    uid_t uid;
#if defined(__linux__)
    struct ucred cr;
    socklen_t cl = sizeof cr;
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cr, &cl) != 0)
        return 0;
    uid = cr.uid;
#else
    gid_t gid;
    if (getpeereid(fd, &uid, &gid) != 0)
        return 0;
#endif
    return uid == geteuid() || uid == 0;
}

static void agent_on_accept(dyn_aio_t* aio, int res, const uint8_t* buf, unsigned len, void* ud)
{
    ssh_agent_srv_t* a = (ssh_agent_srv_t*)ud;
    ssh_agconn_t* x;
    (void)buf;
    (void)len;
    if (res < 0)
        return;
    if (a->closed || a->nconns >= a->max_conns || !peer_ok(res)) {
        a->refused_conns++;
        dyn_aio_close(aio, res);
        return;
    }
    x = (ssh_agconn_t*)calloc(1, sizeof *x);
    if (!x) {
        dyn_aio_close(aio, res);
        return;
    }
    x->a = a;
    x->fd = res;
    x->refs = 1;
    x->next = a->conns;
    if (a->conns)
        a->conns->prev = x;
    a->conns = x;
    a->nconns++;
    a->accepted++;
    if (dyn_aio_recv(aio, res, 0, 1, agconn_recv, x) < 0)
        agconn_close(x);
}

int ssh_agent_srv_listen(ssh_agent_srv_t* a, const char* path, char* why, size_t whylen)
{
    struct stat st;
    struct sockaddr_un probe;
    mode_t old;
    if (strlen(path) >= sizeof probe.sun_path) {
        snprintf(why, whylen, "the socket path is %zu bytes; at most %zu fit in a unix socket address", strlen(path), sizeof probe.sun_path - 1);
        return -1;
    }
    if (lstat(path, &st) == 0) {
        snprintf(why, whylen, "%s already exists (remove a stale socket first)", path);
        return -1;
    }
    old = umask(0177);
    a->lfd = dyn_aio_unix_listen(a->aio, path, 64);
    umask(old);
    if (a->lfd < 0) {
        snprintf(why, whylen, "cannot listen on %s: %s", path, strerror(errno));
        return -1;
    }
    chmod(path, 0600);
    a->path = strdup(path);
    if (a->path)
        live_add(a);
    if (!a->path || dyn_aio_accept(a->aio, a->lfd, agent_on_accept, a) < 0) {
        snprintf(why, whylen, "cannot accept on %s", path);
        ssh_agent_srv_close(a);
        return -1;
    }
    return 0;
}

void ssh_agent_srv_close(ssh_agent_srv_t* a)
{
    a->closed = 1;
    if (a->lfd >= 0) {
        dyn_aio_close(a->aio, a->lfd);
        a->lfd = -1;
    }
    live_del(a);
    if (a->path) {
        unlink(a->path);
        free(a->path);
        a->path = NULL;
    }
    while (a->conns)
        agconn_close(a->conns);
    ssh_agent_store_clear(&a->st);
    OPENSSL_cleanse(a->st.lhash, sizeof a->st.lhash);
}

typedef struct ssh_agc {
    ssh_conn_t* c;
    dyn_aio_t* aio;
    int fd;
    int refs;
    int up;
    int waiting;
    sshb_t in;
    sshb_t out;
} ssh_agc_t;

static void agc_unref(ssh_agc_t* g)
{
    if (--g->refs == 0) {
        sshb_free(&g->in);
        if (g->out.p)
            OPENSSL_cleanse(g->out.p, g->out.cap);
        sshb_free(&g->out);
        free(g);
    }
}

void ssh_auth_drop(ssh_conn_t* c)
{
    ssh_agc_t* g = c->agc;
    int i;
    c->agc = NULL;
    if (g) {
        int fd = g->fd;
        g->c = NULL;
        g->fd = -1;
        if (fd >= 0)
            dyn_aio_close(g->aio, fd);
        agc_unref(g);
    }
    for (i = 0; i < c->nids; i++)
        sshb_free(&c->ids[i].blob);
    free(c->ids);
    c->ids = NULL;
    c->nids = c->idi = 0;
}

static void auth_fail_all(ssh_conn_t* c, const char* extra)
{
    ssh_fail(c, SSH_DISC_NO_MORE_AUTH_METHODS, "publickey authentication of %s was refused for every key tried (%d tried: %s%d agent identities)%s%s",
        c->user, c->tried, c->ukey ? "the privateKey and " : "", c->tried - (c->ukey ? 1 : 0), extra ? "; " : "", extra ? extra : "");
}

static int send_query(ssh_conn_t* c, int i)
{
    ssh_ident_t* id = &c->ids[i];
    ssh_msg_begin(c, SSH_MSG_USERAUTH_REQUEST);
    ssh_m_cstr(c, c->user);
    ssh_m_cstr(c, "ssh-connection");
    ssh_m_cstr(c, "publickey");
    ssh_m_u8(c, 0);
    ssh_m_cstr(c, kind_type[id->kind]);
    ssh_m_str(c, id->blob.p, id->blob.len);
    c->tried++;
    return ssh_send_msg(c);
}

static int signed_data(ssh_conn_t* c, sshb_t* sd, const char* alg, const uint8_t* blob, size_t bn)
{
    return sshb_str(sd, c->sid, c->sidlen) < 0 || sshb_u8(sd, SSH_MSG_USERAUTH_REQUEST) < 0 || sshb_cstr(sd, c->user) < 0
            || sshb_cstr(sd, "ssh-connection") < 0 || sshb_cstr(sd, "publickey") < 0 || sshb_u8(sd, 1) < 0 || sshb_cstr(sd, alg) < 0
            || sshb_str(sd, blob, bn) < 0
        ? -1
        : 0;
}

static int send_signed(ssh_conn_t* c, const char* alg, const uint8_t* blob, size_t bn, const uint8_t* sig, size_t sn)
{
    ssh_msg_begin(c, SSH_MSG_USERAUTH_REQUEST);
    ssh_m_cstr(c, c->user);
    ssh_m_cstr(c, "ssh-connection");
    ssh_m_cstr(c, "publickey");
    ssh_m_u8(c, 1);
    ssh_m_cstr(c, alg);
    ssh_m_str(c, blob, bn);
    ssh_m_str(c, sig, sn);
    return ssh_send_msg(c);
}

static int send_direct(ssh_conn_t* c)
{
    sshb_t pub = { 0 }, sd = { 0 }, sb = { 0 };
    const char* alg = c->ucert.len ? "ssh-ed25519-cert-v01@openssh.com" : "ssh-ed25519";
    uint8_t sig[64];
    int bad, rc;
    if (c->ucert.len)
        bad = sshb_put(&pub, c->ucert.p, c->ucert.len) < 0;
    else
        bad = sshb_cstr(&pub, "ssh-ed25519") < 0 || sshb_str(&pub, c->upub, 32) < 0;
    bad = bad || signed_data(c, &sd, alg, pub.p, pub.len) < 0 || ssh_ed25519_sign(c->ukey, sd.p, sd.len, sig) < 0
        || sshb_cstr(&sb, "ssh-ed25519") < 0 || sshb_str(&sb, sig, 64) < 0;
    sshb_free(&sd);
    if (bad) {
        sshb_free(&pub);
        sshb_free(&sb);
        ssh_fail(c, SSH_DISC_BY_APPLICATION, "cannot sign the publickey authentication request");
        return -1;
    }
    c->tried++;
    rc = send_signed(c, alg, pub.p, pub.len, sb.p, sb.len);
    sshb_free(&pub);
    sshb_free(&sb);
    return rc;
}

static void agc_fail(ssh_agc_t* g, const char* why)
{
    ssh_conn_t* c = g->c;
    if (!c || c->state == SSH_ST_DEAD)
        return;
    ssh_fail(c, SSH_DISC_NO_MORE_AUTH_METHODS, "the agent at %s %s", c->agent_path, why);
}

static int agc_request(ssh_agc_t* g);

static void agc_sent(dyn_aio_t* aio, int res, const uint8_t* buf, unsigned len, void* ud)
{
    ssh_agc_t* g = (ssh_agc_t*)ud;
    ssh_conn_t* c = g->c;
    (void)aio;
    (void)buf;
    (void)len;
    if (res < 0 && c && c->state != SSH_ST_DEAD) {
        ssh_conn_enter(c);
        agc_fail(g, "closed the connection");
        ssh_conn_leave(c);
    }
    agc_unref(g);
}

static int next_identity(ssh_conn_t* c)
{
    if (c->idi >= c->nids) {
        auth_fail_all(c, NULL);
        return -1;
    }
    return send_query(c, c->idi);
}

static int on_ids(ssh_conn_t* c, sshr_t* r)
{
    uint32_t n = sshr_u32(r), i;
    if (r->err || n > 4096)
        return -1;
    c->ids = (ssh_ident_t*)calloc(n ? n : 1, sizeof *c->ids);
    if (!c->ids)
        return -1;
    for (i = 0; i < n; i++) {
        size_t bn, cn;
        const uint8_t* b = sshr_str(r, &bn);
        int k;
        (void)sshr_str(r, &cn);
        if (r->err)
            return -1;
        k = ssh_agent_blob_kind(b, bn);
        if (k < 0)
            continue;
        if (c->ukey && !c->ucert.len && k == 0 && bn == 51 && memcmp(b + 19, c->upub, 32) == 0)
            continue;
        c->ids[c->nids].kind = k;
        if (sshb_put(&c->ids[c->nids].blob, b, bn) < 0)
            return -1;
        c->nids++;
    }
    return r->off == r->len ? 0 : -1;
}

static void agc_reply(ssh_agc_t* g, const uint8_t* p, size_t n)
{
    ssh_conn_t* c = g->c;
    sshr_t r;
    uint8_t t;
    sshr_init(&r, p, n);
    t = sshr_u8(&r);
    if (c->agent_phase == 1) {
        if (r.err || t != AG_IDS || on_ids(c, &r) < 0) {
            agc_fail(g, "sent a malformed identities answer");
            return;
        }
        c->agent_phase = 2;
        c->idi = 0;
        next_identity(c);
        return;
    }
    if (c->agent_phase == 3) {
        ssh_ident_t* id = &c->ids[c->idi];
        size_t sn, an, rn;
        const uint8_t* sig;
        sshr_t sr;
        const uint8_t* alg;
        if (r.err || t != AG_SIGN_RESP) {
            c->idi++;
            c->agent_phase = 2;
            next_identity(c);
            return;
        }
        sig = sshr_str(&r, &sn);
        if (r.err || r.off != r.len) {
            agc_fail(g, "sent a malformed signature");
            return;
        }
        sshr_init(&sr, sig, sn);
        alg = sshr_str(&sr, &an);
        (void)sshr_str(&sr, &rn);
        if (sr.err || !is_n(alg, an, id->kind == 1 ? kind_type[1] : kind_type[0])) {
            agc_fail(g, "signed with an unexpected algorithm");
            return;
        }
        c->agent_phase = 4;
        send_signed(c, kind_type[id->kind], id->blob.p, id->blob.len, sig, sn);
    }
}

static void agc_recv(dyn_aio_t* aio, int res, const uint8_t* buf, unsigned len, void* ud)
{
    ssh_agc_t* g = (ssh_agc_t*)ud;
    ssh_conn_t* c = g->c;
    (void)aio;
    if (!c || g->fd < 0 || c->state == SSH_ST_DEAD)
        return;
    g->refs++;
    ssh_conn_enter(c);
    if (res < 0 || len == 0)
        agc_fail(g, "closed the connection");
    else if (!g->waiting || sshb_put(&g->in, buf, len) < 0)
        agc_fail(g, "sent an unsolicited reply");
    else if (g->in.len >= 4) {
        uint32_t L = ssh_be32(g->in.p);
        if (L == 0 || L > SSH_AGENT_MAX_MSG)
            agc_fail(g, "sent an oversized reply");
        else if (g->in.len > 4 + (size_t)L)
            agc_fail(g, "sent more than one reply");
        else if (g->in.len == 4 + (size_t)L) {
            g->waiting = 0;
            agc_reply(g, g->in.p + 4, L);
            g->in.len = 0;
        }
    }
    if (c->state != SSH_ST_DEAD)
        ssh_flush(c);
    ssh_conn_leave(c);
    agc_unref(g);
}

static int agc_request(ssh_agc_t* g)
{
    uint8_t hdr[4];
    if (!g->up)
        return 0;
    ssh_put_be32(hdr, (uint32_t)g->out.len);
    g->waiting = 1;
    g->in.len = 0;
    g->refs++;
    if (dyn_aio_send2(g->aio, g->fd, hdr, 4, g->out.p, g->out.len, 0, agc_sent, g) < 0) {
        agc_unref(g);
        return -1;
    }
    return 0;
}

static void agc_on_connect(dyn_aio_t* aio, int res, const uint8_t* buf, unsigned len, void* ud)
{
    ssh_agc_t* g = (ssh_agc_t*)ud;
    ssh_conn_t* c = g->c;
    (void)buf;
    (void)len;
    if (!c || g->fd < 0 || c->state == SSH_ST_DEAD)
        return;
    g->refs++;
    ssh_conn_enter(c);
    if (res < 0)
        agc_fail(g, "cannot be reached");
    else {
        g->up = 1;
        if (dyn_aio_recv(aio, g->fd, 0, 1, agc_recv, g) < 0 || agc_request(g) < 0)
            agc_fail(g, "cannot be reached");
    }
    if (c->state != SSH_ST_DEAD)
        ssh_flush(c);
    ssh_conn_leave(c);
    agc_unref(g);
}

static int agent_start(ssh_conn_t* c)
{
    ssh_agc_t* g = (ssh_agc_t*)calloc(1, sizeof *g);
    if (!g) {
        ssh_fail(c, SSH_DISC_BY_APPLICATION, "out of memory");
        return -1;
    }
    g->c = c;
    g->aio = c->aio;
    g->refs = 1;
    c->agc = g;
    c->agent_phase = 1;
    if (sshb_u8(&g->out, AG_REQ_IDS) < 0) {
        ssh_fail(c, SSH_DISC_BY_APPLICATION, "out of memory");
        return -1;
    }
    g->fd = dyn_aio_unix_connect(c->aio, c->agent_path, agc_on_connect, g);
    if (g->fd < 0) {
        ssh_fail(c, SSH_DISC_NO_MORE_AUTH_METHODS, "the agent at %s cannot be reached: %s", c->agent_path, strerror(errno));
        return -1;
    }
    return 0;
}

int ssh_auth_begin(ssh_conn_t* c)
{
    c->state = SSH_ST_AUTH;
    c->tried = 0;
    if (c->ukey) {
        c->agent_phase = 0;
        return send_direct(c);
    }
    return agent_start(c);
}

int ssh_auth_failure(ssh_conn_t* c, const uint8_t* m, size_t mn)
{
    char extra[260];
    snprintf(extra, sizeof extra, "the server accepts: %.*s", (int)(mn > 200 ? 200 : mn), m ? (const char*)m : "");
    if (!c->agent_path) {
        ssh_fail(c, SSH_DISC_NO_MORE_AUTH_METHODS, "publickey authentication of %s with the %s key was refused (%s)", c->user, c->ucert.len ? "ssh-ed25519-cert-v01@openssh.com" : "ssh-ed25519", extra);
        return -1;
    }
    if (c->agent_phase == 0)
        return agent_start(c);
    if (c->agent_phase == 2 || c->agent_phase == 4) {
        c->idi++;
        c->agent_phase = 2;
        if (c->idi >= c->nids) {
            auth_fail_all(c, extra);
            return -1;
        }
        return send_query(c, c->idi);
    }
    ssh_fail(c, SSH_DISC_PROTOCOL_ERROR, "unexpected USERAUTH_FAILURE");
    return -1;
}

int ssh_auth_pk_ok(ssh_conn_t* c, sshr_t* r)
{
    size_t an, bn;
    const uint8_t* alg = sshr_str(r, &an);
    const uint8_t* blob = sshr_str(r, &bn);
    ssh_ident_t* id;
    ssh_agc_t* g = c->agc;
    sshb_t sd = { 0 };
    if (r->err || c->agent_phase != 2 || !g || c->idi >= c->nids) {
        ssh_fail(c, SSH_DISC_PROTOCOL_ERROR, "unexpected USERAUTH_PK_OK");
        return -1;
    }
    id = &c->ids[c->idi];
    if (!is_n(alg, an, kind_type[id->kind]) || bn != id->blob.len || memcmp(blob, id->blob.p, bn) != 0) {
        ssh_fail(c, SSH_DISC_PROTOCOL_ERROR, "USERAUTH_PK_OK for a key that was not offered");
        return -1;
    }
    if (signed_data(c, &sd, kind_type[id->kind], id->blob.p, id->blob.len) < 0) {
        sshb_free(&sd);
        ssh_fail(c, SSH_DISC_BY_APPLICATION, "out of memory");
        return -1;
    }
    g->out.len = 0;
    if (sshb_u8(&g->out, AG_SIGN) < 0 || sshb_str(&g->out, id->blob.p, id->blob.len) < 0 || sshb_str(&g->out, sd.p, sd.len) < 0
        || sshb_u32(&g->out, 0) < 0) {
        sshb_free(&sd);
        ssh_fail(c, SSH_DISC_BY_APPLICATION, "out of memory");
        return -1;
    }
    sshb_free(&sd);
    c->agent_phase = 3;
    if (agc_request(g) < 0) {
        agc_fail(g, "closed the connection");
        return -1;
    }
    return 0;
}

#endif
