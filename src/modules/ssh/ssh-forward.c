#include "ssh-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_SSH)

#include <errno.h>
#include <stdio.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#define RELAY_HIGH (256u << 10)
#define RELAY_LOW (64u << 10)

typedef struct ssh_relay {
    ssh_chan_t* ch;
    dyn_aio_t* aio;
    int fd;
    int refs;
    int reading;
    int sock_eof;
    int chan_eof;
    int shut;
    int linger;
    JSRuntime* rt;
    ssh_conn_t* lc;
    struct ssh_relay* lprev;
    struct ssh_relay* lnext;
    uint64_t to_sock;
} ssh_relay_t;

typedef struct ssh_lis {
    ssh_conn_t* c;
    int fd;
    char* addr;
    uint16_t port;
    struct ssh_lis* next;
} ssh_lis_t;

typedef struct ssh_greq {
    uint64_t token;
    int want;
    int done;
    int ok;
    int with_port;
    uint32_t port;
    char* addr;
} ssh_greq_t;

static uint64_t fwd_next_token;

static void relay_on_recv(dyn_aio_t* aio, int res, const uint8_t* buf, unsigned len, void* ud);

static void relay_unref(ssh_relay_t* r)
{
    if (--r->refs == 0)
        free(r);
}

static void relay_close_fd(ssh_relay_t* r)
{
    int fd = r->fd;
    if (fd < 0)
        return;
    r->fd = -1;
    r->reading = 0;
    r->refs++;
    dyn_aio_close(r->aio, fd);
    if (r->linger) {
        r->linger = 0;
        if (r->lc) {
            if (r->lprev)
                r->lprev->lnext = r->lnext;
            else
                r->lc->lingers = r->lnext;
            if (r->lnext)
                r->lnext->lprev = r->lprev;
            r->lc->nlinger--;
            r->lc = NULL;
            r->lprev = r->lnext = NULL;
        }
        dyn_net_reactor_release_rt(r->rt);
    }
    relay_unref(r);
}

static int relay_drain_then_close(ssh_relay_t* r)
{
    ssh_conn_t* c = r->ch ? r->ch->conn : NULL;
    if (r->fd < 0 || r->to_sock == 0)
        return 0;
    if (!r->linger) {
        if (!c || c->state == SSH_ST_DEAD || c->nlinger >= c->cfg.max_channels)
            return 0;
        r->lc = c;
        r->lprev = NULL;
        r->lnext = c->lingers;
        if (c->lingers)
            c->lingers->lprev = r;
        c->lingers = r;
        c->nlinger++;
        if (r->reading) {
            dyn_aio_cancel(r->aio, relay_on_recv, r);
            r->reading = 0;
        }
        r->linger = 1;
        dyn_net_reactor_hold();
    }
    return 1;
}

static void relay_maybe_close(ssh_relay_t* r)
{
    ssh_chan_t* ch = r->ch;
    if (!ch || !ch->conn)
        return;
    if (r->chan_eof && r->to_sock == 0 && !r->shut && r->fd >= 0) {
        r->shut = 1;
        shutdown(r->fd, SHUT_WR);
    }
    if (r->sock_eof && ch->sent_eof && r->chan_eof && r->to_sock == 0)
        ssh_chan_close(ch);
}

static int relay_read(ssh_relay_t* r)
{
    if (r->reading || r->fd < 0 || r->sock_eof)
        return 0;
    r->reading = 1;
    if (dyn_aio_recv(r->aio, r->fd, 0, 1, relay_on_recv, r) < 0) {
        r->reading = 0;
        return -1;
    }
    return 0;
}

static void relay_fail(ssh_relay_t* r)
{
    ssh_chan_t* ch = r->ch;
    relay_close_fd(r);
    if (ch && ch->conn)
        ssh_chan_close(ch);
}

static void relay_on_recv(dyn_aio_t* aio, int res, const uint8_t* buf, unsigned len, void* ud)
{
    ssh_relay_t* r = (ssh_relay_t*)ud;
    ssh_chan_t* ch = r->ch;
    ssh_conn_t* c;
    if (r->fd < 0 || !ch || !(c = ch->conn))
        return;
    ssh_conn_enter(c);
    if (res < 0)
        relay_fail(r);
    else if (len == 0) {
        r->sock_eof = 1;
        r->reading = 0;
        dyn_aio_cancel(aio, relay_on_recv, r);
        ch->want_eof = 1;
    } else if (sshb_put(&ch->inq, buf, len) < 0)
        relay_fail(r);
    else {
        ch->in_total += len;
        if (ch->inq.len - ch->inq_off >= RELAY_HIGH && r->reading) {
            dyn_aio_cancel(aio, relay_on_recv, r);
            r->reading = 0;
        }
    }
    if (ch->conn) {
        ssh_chan_pump(ch);
        relay_maybe_close(r);
        if (ch->conn)
            ssh_flush(ch->conn);
    }
    ssh_conn_leave(c);
}

static void relay_on_sent(dyn_aio_t* aio, int res, const uint8_t* buf, unsigned len, void* ud)
{
    ssh_relay_t* r = (ssh_relay_t*)ud;
    ssh_chan_t* ch = r->ch;
    (void)aio;
    (void)buf;
    (void)len;
    if (res > 0)
        r->to_sock = r->to_sock > (uint64_t)res ? r->to_sock - (uint64_t)res : 0;
    if (r->linger && (res < 0 || r->to_sock == 0))
        relay_close_fd(r);
    else if (ch && ch->conn) {
        ssh_conn_t* c = ch->conn;
        ssh_conn_enter(c);
        ch->queued[0] = r->to_sock;
        if (res < 0)
            relay_fail(r);
        else {
            relay_maybe_close(r);
            if (ch->conn)
                ssh_chan_kick(ch);
        }
        ssh_conn_leave(c);
    }
    relay_unref(r);
}

static void relay_opened(ssh_chan_t* ch, const char* err)
{
    ssh_relay_t* r = (ssh_relay_t*)ch->opq;
    if (!r)
        return;
    if (err)
        relay_close_fd(r);
}

static void relay_data(ssh_chan_t* ch, int which, const uint8_t* p, size_t n)
{
    ssh_relay_t* r = (ssh_relay_t*)ch->opq;
    if (!r || which || n == 0 || r->fd < 0)
        return;
    r->to_sock += n;
    ch->queued[0] = r->to_sock;
    r->refs++;
    if (dyn_aio_send(r->aio, r->fd, p, n, 0, relay_on_sent, r) < 0) {
        relay_unref(r);
        relay_fail(r);
    }
}

static void relay_eof(ssh_chan_t* ch)
{
    ssh_relay_t* r = (ssh_relay_t*)ch->opq;
    if (!r)
        return;
    r->chan_eof = 1;
    relay_maybe_close(r);
}

static void relay_written(ssh_chan_t* ch)
{
    ssh_relay_t* r = (ssh_relay_t*)ch->opq;
    if (!r)
        return;
    if (!r->reading && !r->sock_eof && r->fd >= 0 && ch->state == SSH_CH_OPEN && ch->inq.len - ch->inq_off <= RELAY_LOW) {
        if (relay_read(r) < 0) {
            relay_fail(r);
            return;
        }
    }
    relay_maybe_close(r);
}

static void relay_closed(ssh_chan_t* ch, const char* err)
{
    ssh_relay_t* r = (ssh_relay_t*)ch->opq;
    (void)err;
    if (r && !relay_drain_then_close(r))
        relay_close_fd(r);
}

static void relay_release(JSRuntime* rt, ssh_chan_t* ch)
{
    ssh_relay_t* r = (ssh_relay_t*)ch->opq;
    (void)rt;
    ch->opq = NULL;
    ch->ops = NULL;
    if (!r)
        return;
    r->ch = NULL;
    if (!r->linger)
        relay_close_fd(r);
    relay_unref(r);
}

static const ssh_chan_ops_t relay_ops = { relay_opened, relay_data, relay_eof, relay_written, relay_closed, relay_release };

static ssh_relay_t* relay_attach(ssh_conn_t* c, ssh_chan_t* ch, int fd)
{
    ssh_relay_t* r = (ssh_relay_t*)calloc(1, sizeof *r);
    if (!r)
        return NULL;
    r->ch = ch;
    r->aio = c->aio;
    r->rt = c->rt;
    r->fd = fd;
    r->refs = 1;
    ch->ops = &relay_ops;
    ch->opq = r;
    ch->fwd = 1;
    return r;
}

static ssh_chan_t* chan_new(ssh_conn_t* c)
{
    ssh_chan_t* ch = (ssh_chan_t*)calloc(1, sizeof *ch);
    if (!ch)
        return NULL;
    ch->ctl[0] = ch->ctl[1] = ch->open_res = ch->open_rej = ch->exit_res = ch->jsobj = JS_UNDEFINED;
    ch->refs = 1;
    ch->win_size = ch->lwin = c->cfg.window;
    ch->lmax = c->cfg.chan_packet;
    ch->fwd = 1;
    return ch;
}

static void peer_of(int fd, char* host, size_t cap, uint32_t* port)
{
    struct sockaddr_storage sa;
    socklen_t sl = sizeof sa;
    host[0] = 0;
    *port = 0;
    if (getpeername(fd, (struct sockaddr*)&sa, &sl) != 0)
        return;
    if (sa.ss_family == AF_INET) {
        inet_ntop(AF_INET, &((struct sockaddr_in*)&sa)->sin_addr, host, (socklen_t)cap);
        *port = ntohs(((struct sockaddr_in*)&sa)->sin_port);
    } else if (sa.ss_family == AF_INET6) {
        inet_ntop(AF_INET6, &((struct sockaddr_in6*)&sa)->sin6_addr, host, (socklen_t)cap);
        *port = ntohs(((struct sockaddr_in6*)&sa)->sin6_port);
    }
}

static uint16_t bound_port(int fd)
{
    struct sockaddr_storage sa;
    socklen_t sl = sizeof sa;
    if (getsockname(fd, (struct sockaddr*)&sa, &sl) != 0)
        return 0;
    if (sa.ss_family == AF_INET)
        return ntohs(((struct sockaddr_in*)&sa)->sin_port);
    if (sa.ss_family == AF_INET6)
        return ntohs(((struct sockaddr_in6*)&sa)->sin6_port);
    return 0;
}

static int open_out(ssh_conn_t* c, int fd, const char* type, const char* a1, uint32_t p1)
{
    ssh_chan_t* ch;
    char oh[64];
    uint32_t op;
    if (c->state != SSH_ST_READY || c->nopen >= c->cfg.max_channels)
        return -1;
    ch = chan_new(c);
    if (!ch)
        return -1;
    ch->ctype = type;
    peer_of(fd, oh, sizeof oh, &op);
    if (sshb_cstr(&ch->oextra, a1) < 0 || sshb_u32(&ch->oextra, p1) < 0 || sshb_cstr(&ch->oextra, oh[0] ? oh : "127.0.0.1") < 0
        || sshb_u32(&ch->oextra, op) < 0 || !relay_attach(c, ch, fd)) {
        ssh_chan_unref(c->rt, ch);
        return -1;
    }
    if (ssh_chan_open(c, ch) < 0) {
        ssh_relay_t* r = (ssh_relay_t*)ch->opq;
        r->fd = -1;
        if (!ch->conn)
            ssh_chan_unref(c->rt, ch);
        return -1;
    }
    if (relay_read((ssh_relay_t*)ch->opq) < 0)
        ssh_chan_close(ch);
    return 0;
}

int ssh_chan_ready_in(ssh_chan_t* ch)
{
    ssh_conn_t* c = ch->conn;
    if (!c || ch->state != SSH_CH_PENDING)
        return -1;
    ch->state = SSH_CH_OPEN;
    ssh_msg_begin(c, SSH_MSG_CHANNEL_OPEN_CONFIRMATION);
    ssh_m_u32(c, ch->rid);
    ssh_m_u32(c, ch->lid);
    ssh_m_u32(c, (uint32_t)ch->lwin);
    ssh_m_u32(c, ch->lmax);
    if (ssh_send_msg(c) < 0)
        return -1;
    ssh_chan_window(ch);
    ssh_chan_pump(ch);
    return 0;
}

static void chan_refuse(ssh_chan_t* ch, uint32_t code, const char* why)
{
    ssh_conn_t* c = ch->conn;
    if (!c || ch->state != SSH_CH_PENDING)
        return;
    ssh_msg_begin(c, SSH_MSG_CHANNEL_OPEN_FAILURE);
    ssh_m_u32(c, ch->rid);
    ssh_m_u32(c, code);
    ssh_m_cstr(c, why);
    ssh_m_cstr(c, "");
    (void)ssh_send_msg(c);
    ch->state = SSH_CH_CLOSED;
    ssh_chan_finish(c, ch, why);
}

static void relay_on_connect(dyn_aio_t* aio, int res, const uint8_t* buf, unsigned len, void* ud)
{
    ssh_relay_t* r = (ssh_relay_t*)ud;
    ssh_chan_t* ch = r->ch;
    ssh_conn_t* c;
    (void)aio;
    (void)buf;
    (void)len;
    if (!ch || !(c = ch->conn) || r->fd < 0)
        return;
    ssh_conn_enter(c);
    if (res < 0) {
        relay_close_fd(r);
        chan_refuse(ch, 2, "connect failed");
    } else if (relay_read(r) < 0 || ssh_chan_ready_in(ch) < 0)
        relay_fail(r);
    if (c->state != SSH_ST_DEAD)
        ssh_flush(c);
    ssh_conn_leave(c);
}

static void connect_target(ssh_chan_t* ch, const char* host, uint32_t port)
{
    ssh_conn_t* c = ch->conn;
    ssh_relay_t* r;
    if (!c)
        return;
    r = relay_attach(c, ch, -1);
    if (!r) {
        chan_refuse(ch, 4, "out of memory");
        return;
    }
    r->fd = dyn_aio_connect(c->aio, host, (uint16_t)port, relay_on_connect, r);
    if (r->fd < 0)
        chan_refuse(ch, 2, "connect failed");
}

static char* str_dup(const uint8_t* p, size_t n)
{
    char* s;
    if (n > 255 || (n && memchr(p, 0, n)))
        return NULL;
    s = (char*)malloc(n + 1);
    if (!s)
        return NULL;
    memcpy(s, p, n);
    s[n] = 0;
    return s;
}

int ssh_fwd_open_in(ssh_conn_t* c, uint32_t sender, uint32_t win, uint32_t maxp, sshr_t* r)
{
    size_t hn, on;
    const uint8_t* hp = sshr_str(r, &hn);
    uint32_t port = sshr_u32(r);
    const uint8_t* op = sshr_str(r, &on);
    uint32_t oport = sshr_u32(r);
    char *host = NULL, *ohost = NULL;
    const char* why = NULL;
    uint32_t code = 1;
    ssh_chan_t* ch = NULL;
    ssh_fwd_t* f = NULL;
    if (r->err || port > 65535 || oport > 65535) {
        ssh_fail(c, SSH_DISC_PROTOCOL_ERROR, "malformed %s channel open", c->server ? "direct-tcpip" : "forwarded-tcpip");
        return -1;
    }
    host = str_dup(hp, hn);
    ohost = str_dup(op, on);
    if (!host || !ohost)
        why = "bad address";
    else if (c->server && (!c->fwd_hook || !c->cert_fwd))
        why = "port forwarding is disabled";
    else if (!c->server) {
        for (f = c->fwds; f; f = f->next)
            if (f->kind == 1 && f->state == 1 && f->rport == port)
                break;
        if (!f)
            why = "no remote forward is open on that port";
    }
    if (!why && c->nopen >= c->cfg.max_channels) {
        code = 4;
        why = "too many channels";
    }
    if (!why) {
        ch = chan_new(c);
        if (!ch || ssh_chan_slot(c, ch) < 0) {
            if (ch)
                ssh_chan_unref(c->rt, ch);
            ch = NULL;
            code = 4;
            why = "out of channel resources";
        }
    }
    if (!ch) {
        free(host);
        free(ohost);
        ssh_msg_begin(c, SSH_MSG_CHANNEL_OPEN_FAILURE);
        ssh_m_u32(c, sender);
        ssh_m_u32(c, code);
        ssh_m_cstr(c, why);
        ssh_m_cstr(c, "");
        return ssh_send_msg(c);
    }
    ch->rid = sender;
    ch->rwin = win;
    ch->rmax = maxp > (1u << 20) ? (1u << 20) : maxp;
    ch->state = SSH_CH_PENDING;
    if (!c->server) {
        f->conns++;
        connect_target(ch, f->lhost, f->lport);
    } else {
        int rc;
        ch->ftoken = ++fwd_next_token;
        if (sshb_cstr(&ch->oextra, host) < 0 || sshb_u32(&ch->oextra, port) < 0) {
            chan_refuse(ch, 4, "out of memory");
            free(host);
            free(ohost);
            return c->state == SSH_ST_DEAD ? -1 : 0;
        }
        rc = ssh_js_forward(c, "local", host, port, ohost, oport, ch->ftoken);
        if (ch->conn && ch->state == SSH_CH_PENDING) {
            if (rc == 1)
                connect_target(ch, host, port);
            else if (rc == 0)
                chan_refuse(ch, 1, "the forward hook refused the connection");
        }
    }
    free(host);
    free(ohost);
    return c->state == SSH_ST_DEAD ? -1 : 0;
}

static void lis_on_accept(dyn_aio_t* aio, int res, const uint8_t* buf, unsigned len, void* ud)
{
    ssh_lis_t* l = (ssh_lis_t*)ud;
    ssh_conn_t* c = l->c;
    (void)buf;
    (void)len;
    if (res < 0)
        return;
    if (!c || c->state != SSH_ST_READY) {
        dyn_aio_close(aio, res);
        return;
    }
    ssh_conn_enter(c);
    if (open_out(c, res, "forwarded-tcpip", l->addr, l->port) < 0)
        dyn_aio_close(aio, res);
    if (c->state != SSH_ST_DEAD)
        ssh_flush(c);
    ssh_conn_leave(c);
}

static void lis_free(ssh_lis_t* l)
{
    if (l->fd >= 0 && l->c)
        dyn_aio_close(l->c->aio, l->fd);
    free(l->addr);
    free(l);
}

static int is_any(const char* a)
{
    return !a[0] || strcmp(a, "*") == 0 || strcmp(a, "0.0.0.0") == 0 || strcmp(a, "::") == 0;
}

static int lis_open(ssh_conn_t* c, ssh_greq_t* g)
{
    ssh_lis_t* l;
    const char* host = "127.0.0.1";
    if (c->cfg.public_binds) {
        if (is_any(g->addr))
            host = strcmp(g->addr, "::") == 0 ? "::" : "0.0.0.0";
        else if (strcmp(g->addr, "localhost") != 0)
            host = g->addr;
    }
    l = (ssh_lis_t*)calloc(1, sizeof *l);
    if (!l)
        return -1;
    l->c = c;
    l->addr = strdup(g->addr);
    l->fd = dyn_aio_listen(c->aio, host, (uint16_t)g->port, 128);
    if (!l->addr || l->fd < 0) {
        lis_free(l);
        return -1;
    }
    l->port = bound_port(l->fd);
    if (dyn_aio_accept(c->aio, l->fd, lis_on_accept, l) < 0) {
        lis_free(l);
        return -1;
    }
    if (g->port == 0) {
        g->with_port = 1;
        g->port = l->port;
    }
    l->next = c->lis;
    c->lis = l;
    c->nlis++;
    return 0;
}

static void gq_flush(ssh_conn_t* c)
{
    int k = 0, i;
    while (k < c->ngq && c->gq[k].done) {
        ssh_greq_t* g = &c->gq[k];
        if (g->want) {
            ssh_msg_begin(c, g->ok ? SSH_MSG_REQUEST_SUCCESS : SSH_MSG_REQUEST_FAILURE);
            if (g->ok && g->with_port)
                ssh_m_u32(c, g->port);
            if (ssh_send_msg(c) < 0)
                break;
        }
        free(g->addr);
        k++;
    }
    if (k == 0)
        return;
    for (i = k; i < c->ngq; i++)
        c->gq[i - k] = c->gq[i];
    c->ngq -= k;
}

static ssh_greq_t* gq_push(ssh_conn_t* c, int want)
{
    ssh_greq_t* g;
    if (c->ngq >= 64)
        return NULL;
    if (c->ngq == c->cgq) {
        int n = c->cgq ? c->cgq * 2 : 4;
        ssh_greq_t* q = (ssh_greq_t*)realloc(c->gq, (size_t)n * sizeof *q);
        if (!q)
            return NULL;
        c->gq = q;
        c->cgq = n;
    }
    g = &c->gq[c->ngq++];
    memset(g, 0, sizeof *g);
    g->want = want;
    return g;
}

static void greq_bind(ssh_conn_t* c, ssh_greq_t* g)
{
    g->done = 1;
    g->ok = c->nlis < SSH_MAX_REMOTE_FWD && lis_open(c, g) == 0;
}

int ssh_fwd_global(ssh_conn_t* c, const uint8_t* name, size_t nn, int want, sshr_t* r)
{
    ssh_greq_t* g;
    int fwd = c->server && nn == 13 && memcmp(name, "tcpip-forward", 13) == 0;
    int cancel = c->server && nn == 20 && memcmp(name, "cancel-tcpip-forward", 20) == 0;
    g = gq_push(c, want);
    if (!g) {
        ssh_fail(c, SSH_DISC_PROTOCOL_ERROR, "too many unanswered global requests");
        return -1;
    }
    if (fwd || cancel) {
        size_t an;
        const uint8_t* ap = sshr_str(r, &an);
        uint32_t port = sshr_u32(r);
        if (r->err || port > 65535 || !(g->addr = str_dup(ap, an))) {
            ssh_fail(c, SSH_DISC_PROTOCOL_ERROR, "malformed %s request", fwd ? "tcpip-forward" : "cancel-tcpip-forward");
            return -1;
        }
        g->port = port;
    }
    if (cancel) {
        ssh_lis_t **pp = &c->lis, *l;
        g->done = 1;
        for (; (l = *pp); pp = &l->next)
            if (l->port == g->port && strcmp(l->addr, g->addr) == 0) {
                *pp = l->next;
                c->nlis--;
                lis_free(l);
                g->ok = 1;
                break;
            }
    } else if (fwd && c->fwd_hook && c->cert_fwd && c->nlis < SSH_MAX_REMOTE_FWD) {
        uint64_t tok = ++fwd_next_token;
        int rc, i;
        char* addr = strdup(g->addr);
        uint32_t port = g->port;
        g->token = tok;
        if (!addr) {
            g->done = 1;
        } else {
            rc = ssh_js_forward(c, "remote", addr, port, c->peer, 0, tok);
            free(addr);
            for (i = 0; i < c->ngq; i++)
                if (c->gq[i].token == tok && !c->gq[i].done) {
                    if (rc == 1)
                        greq_bind(c, &c->gq[i]);
                    else if (rc == 0)
                        c->gq[i].done = 1;
                    break;
                }
        }
    } else
        g->done = 1;
    if (c->state != SSH_ST_DEAD)
        gq_flush(c);
    return c->state == SSH_ST_DEAD ? -1 : 0;
}

void ssh_fwd_decided(ssh_conn_t* c, uint64_t token, int ok)
{
    int i;
    ssh_chan_t* ch = NULL;
    if (c->state != SSH_ST_READY)
        return;
    ssh_conn_enter(c);
    for (i = 0; i < c->nch && !ch; i++)
        if (c->ch[i] && c->ch[i]->ftoken == token && c->ch[i]->state == SSH_CH_PENDING && !c->ch[i]->ops)
            ch = c->ch[i];
    if (ch) {
        sshr_t r;
        const uint8_t* hp;
        size_t hn;
        uint32_t port;
        char host[256];
        sshr_init(&r, ch->oextra.p, ch->oextra.len);
        hp = sshr_str(&r, &hn);
        port = sshr_u32(&r);
        if (!ok)
            chan_refuse(ch, 1, "the forward hook refused the connection");
        else if (r.err || hn >= sizeof host)
            chan_refuse(ch, 1, "bad address");
        else {
            memcpy(host, hp, hn);
            host[hn] = 0;
            connect_target(ch, host, port);
        }
    } else {
        for (i = 0; i < c->ngq; i++)
            if (c->gq[i].token == token && !c->gq[i].done) {
                if (ok)
                    greq_bind(c, &c->gq[i]);
                else
                    c->gq[i].done = 1;
                break;
            }
        if (c->state != SSH_ST_DEAD)
            gq_flush(c);
    }
    if (c->state != SSH_ST_DEAD)
        ssh_flush(c);
    ssh_conn_leave(c);
}

static void fwd_unlink(ssh_fwd_t* f);

int ssh_fwd_reply(ssh_conn_t* c, int ok, sshr_t* r)
{
    ssh_fwd_t* f;
    int i;
    if (c->server || c->ngw == 0)
        return 0;
    f = c->gw[0];
    for (i = 1; i < c->ngw; i++)
        c->gw[i - 1] = c->gw[i];
    c->ngw--;
    if (!f || f->c != c || f->state != 0)
        return 0;
    if (ok && f->rport == 0) {
        uint32_t p = sshr_u32(r);
        if (r->err || p == 0 || p > 65535)
            ok = 0;
        else
            f->rport = (uint16_t)p;
    }
    ssh_fwd_settle(f, ok ? NULL : "the server refused the remote forward");
    if (!ok) {
        fwd_unlink(f);
        f->state = 2;
        ssh_fwd_drop(f);
    }
    return 0;
}

static void lfwd_on_accept(dyn_aio_t* aio, int res, const uint8_t* buf, unsigned len, void* ud)
{
    ssh_fwd_t* f = (ssh_fwd_t*)ud;
    ssh_conn_t* c = f->c;
    (void)buf;
    (void)len;
    if (res < 0)
        return;
    if (!c || c->state != SSH_ST_READY) {
        dyn_aio_close(aio, res);
        return;
    }
    ssh_conn_enter(c);
    f->conns++;
    if (open_out(c, res, "direct-tcpip", f->rhost, f->rport) < 0)
        dyn_aio_close(aio, res);
    if (c->state != SSH_ST_DEAD)
        ssh_flush(c);
    ssh_conn_leave(c);
}

int ssh_fwd_listen_local(ssh_conn_t* c, ssh_fwd_t* f, char* why, size_t whylen)
{
    f->lfd = dyn_aio_listen(c->aio, f->lhost, f->lport, 128);
    if (f->lfd < 0) {
        snprintf(why, whylen, "cannot listen on %s:%u: %s", f->lhost, (unsigned)f->lport, strerror(errno));
        return -1;
    }
    f->lport = bound_port(f->lfd);
    if (dyn_aio_accept(c->aio, f->lfd, lfwd_on_accept, f) < 0) {
        dyn_aio_close(c->aio, f->lfd);
        f->lfd = -1;
        snprintf(why, whylen, "cannot accept on %s:%u", f->lhost, (unsigned)f->lport);
        return -1;
    }
    f->c = c;
    f->state = 1;
    f->next = c->fwds;
    c->fwds = f;
    return 0;
}

int ssh_fwd_request_remote(ssh_conn_t* c, ssh_fwd_t* f)
{
    if (c->ngw == c->cgw) {
        int n = c->cgw ? c->cgw * 2 : 4;
        ssh_fwd_t** q = (ssh_fwd_t**)realloc(c->gw, (size_t)n * sizeof *q);
        if (!q)
            return -1;
        c->gw = q;
        c->cgw = n;
    }
    ssh_msg_begin(c, SSH_MSG_GLOBAL_REQUEST);
    ssh_m_cstr(c, "tcpip-forward");
    ssh_m_u8(c, 1);
    ssh_m_cstr(c, f->rhost);
    ssh_m_u32(c, f->rport);
    if (ssh_send_msg(c) < 0)
        return -1;
    c->gw[c->ngw++] = f;
    f->c = c;
    f->state = 0;
    f->next = c->fwds;
    c->fwds = f;
    ssh_flush(c);
    return 0;
}

static void fwd_unlink(ssh_fwd_t* f)
{
    ssh_conn_t* c = f->c;
    ssh_fwd_t** pp;
    int i;
    if (!c)
        return;
    for (pp = &c->fwds; *pp; pp = &(*pp)->next)
        if (*pp == f) {
            *pp = f->next;
            break;
        }
    for (i = 0; i < c->ngw; i++)
        if (c->gw[i] == f)
            c->gw[i] = NULL;
    if (f->lfd >= 0) {
        dyn_aio_close(c->aio, f->lfd);
        f->lfd = -1;
    }
    f->next = NULL;
    f->c = NULL;
}

void ssh_fwd_close(ssh_fwd_t* f)
{
    ssh_conn_t* c = f->c;
    if (!c)
        return;
    if (f->kind == 1 && f->state == 1 && c->state == SSH_ST_READY) {
        ssh_msg_begin(c, SSH_MSG_GLOBAL_REQUEST);
        ssh_m_cstr(c, "cancel-tcpip-forward");
        ssh_m_u8(c, 0);
        ssh_m_cstr(c, f->rhost);
        ssh_m_u32(c, f->rport);
        if (ssh_send_msg(c) == 0)
            ssh_flush(c);
    }
    if (f->state == 0)
        ssh_fwd_settle(f, "the forward was closed before the server answered");
    fwd_unlink(f);
    f->state = 2;
    ssh_fwd_drop(f);
}

void ssh_fwd_conn_dead(ssh_conn_t* c)
{
    while (c->lingers)
        relay_close_fd(c->lingers);
    while (c->lis) {
        ssh_lis_t* l = c->lis;
        c->lis = l->next;
        lis_free(l);
    }
    c->nlis = 0;
    while (c->fwds) {
        ssh_fwd_t* f = c->fwds;
        if (f->state == 0)
            ssh_fwd_settle(f, c->err ? c->err : "the connection closed");
        fwd_unlink(f);
        f->state = 2;
        ssh_fwd_drop(f);
    }
    ssh_via_detach(c);
}

void ssh_fwd_conn_free(ssh_conn_t* c)
{
    int i;
    ssh_fwd_conn_dead(c);
    for (i = 0; i < c->ngq; i++)
        free(c->gq[i].addr);
    free(c->gq);
    c->gq = NULL;
    c->ngq = c->cgq = 0;
    free(c->gw);
    c->gw = NULL;
    c->ngw = c->cgw = 0;
}

static void via_opened(ssh_chan_t* ch, const char* err)
{
    ssh_conn_t* in = (ssh_conn_t*)ch->opq;
    if (!in)
        return;
    ssh_conn_enter(in);
    if (err)
        ssh_fail(in, 0, "the jump host refused the connection to %s:%u: %s", in->host, (unsigned)in->port, err);
    else {
        in->tcp_up = 1;
        ssh_flush(in);
    }
    ssh_conn_leave(in);
}

static void via_data(ssh_chan_t* ch, int which, const uint8_t* p, size_t n)
{
    ssh_conn_t* in = (ssh_conn_t*)ch->opq;
    if (!in || which || in->state == SSH_ST_DEAD)
        return;
    ssh_conn_enter(in);
    ssh_input(in, p, n);
    ssh_conn_leave(in);
}

static void via_gone(ssh_chan_t* ch, const char* err)
{
    ssh_conn_t* in = (ssh_conn_t*)ch->opq;
    if (!in || in->state == SSH_ST_DEAD)
        return;
    ssh_conn_enter(in);
    ssh_fail(in, 0, "the jump connection closed%s%s", err ? ": " : "", err ? err : "");
    ssh_conn_leave(in);
}

static void via_eof(ssh_chan_t* ch)
{
    via_gone(ch, NULL);
}

static void via_release(JSRuntime* rt, ssh_chan_t* ch)
{
    (void)rt;
    ch->opq = NULL;
    ch->ops = NULL;
}

static const ssh_chan_ops_t via_ops = { via_opened, via_data, via_eof, NULL, via_gone, via_release };

int ssh_via_open(ssh_conn_t* c, ssh_conn_t* outer)
{
    ssh_chan_t* ch;
    if (outer->state != SSH_ST_READY || outer->nopen >= outer->cfg.max_channels)
        return -1;
    ch = chan_new(outer);
    if (!ch)
        return -1;
    ch->ctype = "direct-tcpip";
    if (sshb_cstr(&ch->oextra, c->host) < 0 || sshb_u32(&ch->oextra, c->port) < 0 || sshb_cstr(&ch->oextra, "127.0.0.1") < 0
        || sshb_u32(&ch->oextra, 0) < 0) {
        ssh_chan_unref(outer->rt, ch);
        return -1;
    }
    ch->ops = &via_ops;
    ch->opq = c;
    ch->refs++;
    ssh_conn_enter(outer);
    if (ssh_chan_open(outer, ch) < 0) {
        ch->ops = NULL;
        ch->opq = NULL;
        if (!ch->conn)
            ssh_chan_unref(outer->rt, ch);
        ssh_chan_unref(outer->rt, ch);
        ssh_conn_leave(outer);
        return -1;
    }
    c->via = outer;
    c->via_ch = ch;
    ssh_flush(outer);
    ssh_conn_leave(outer);
    return 0;
}

void ssh_via_flush(ssh_conn_t* c)
{
    ssh_chan_t* ch = c->via_ch;
    if (!c->tcp_up || c->obuf.len == 0)
        return;
    if (!ch || !ch->conn) {
        c->obuf.len = 0;
        return;
    }
    if (sshb_put(&ch->inq, c->obuf.p, c->obuf.len) < 0) {
        c->obuf.len = 0;
        ssh_fail(c, 0, "out of memory queueing for the jump host");
        return;
    }
    ch->in_total += c->obuf.len;
    c->obuf.len = 0;
    {
        ssh_conn_t* outer = ch->conn;
        ssh_conn_enter(outer);
        ssh_chan_pump(ch);
        if (outer->state != SSH_ST_DEAD)
            ssh_flush(outer);
        ssh_conn_leave(outer);
    }
}

void ssh_via_detach(ssh_conn_t* c)
{
    ssh_chan_t* ch = c->via_ch;
    JSRuntime* rt = c->rt;
    if (!ch)
        return;
    c->via_ch = NULL;
    c->via = NULL;
    ch->ops = NULL;
    ch->opq = NULL;
    if (ch->conn) {
        ssh_conn_t* outer = ch->conn;
        ssh_conn_enter(outer);
        ssh_chan_close(ch);
        if (outer->state != SSH_ST_DEAD)
            ssh_flush(outer);
        ssh_conn_leave(outer);
    }
    ssh_chan_unref(rt, ch);
}

#endif
