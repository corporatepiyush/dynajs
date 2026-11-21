#include "smtp-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_SMTP)
#include <time.h>

enum { OP_FEED = 1, OP_SEND, OP_TLS, OP_QUIT };

typedef struct {
    smtp_conn_t* c;
    int op, rc;
    uint8_t* data;
    size_t n;
    mailk_msg_t m;
    uint64_t t_sub, t_start, t_end;
} smtp_job_t;

uint64_t smtp_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void sample(uint32_t* ring, uint64_t* n, uint64_t ns)
{
    ring[*n % SMTP_SAMPLES] = ns / 1000 > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)(ns / 1000);
    (*n)++;
}

static void conn_free(smtp_conn_t* c)
{
    smtp_t* s = c->s;
    smtp_conn_t** pp;
    for (pp = &s->conns; *pp; pp = &(*pp)->next)
        if (*pp == c) {
            *pp = c->next;
            break;
        }
    mailk_cli_free(&c->eng);
    dyn_sb_free(&c->inq);
    free(c->parked);
    free(c);
    smtp_unref(s);
}

static void conn_unref(smtp_conn_t* c)
{
    if (--c->refs == 0)
        conn_free(c);
}

static void conn_close_fd(smtp_conn_t* c)
{
    smtp_t* s = c->s;
    if (c->dead)
        return;
    c->dead = 1;
    s->nconns--;
    if (c->fd >= 0) {
        dyn_aio_close(s->aio, c->fd);
        c->fd = -1;
    }
    conn_unref(c);
}

static void job_work(void* arg)
{
    smtp_job_t* j = (smtp_job_t*)arg;
    mailk_cli_t* e = &j->c->eng;
    j->t_start = smtp_now_ns();
    switch (j->op) {
    case OP_FEED:
        j->rc = mailk_cli_feed(e, j->data, j->n);
        break;
    case OP_SEND:
        j->rc = mailk_cli_send(e, &j->m);
        break;
    case OP_TLS:
        j->rc = mailk_cli_tls_done(e);
        break;
    default:
        j->rc = mailk_cli_quit(e);
        break;
    }
    j->t_end = smtp_now_ns();
}

static void after_job(smtp_conn_t* c, smtp_job_t* j);

static void job_done(void* arg)
{
    smtp_job_t* j = (smtp_job_t*)arg;
    smtp_conn_t* c = j->c;
    smtp_t* s = c->s;
    uint64_t t0 = smtp_now_ns();
    c->busy = 0;
    s->jobs++;
    sample(s->hs, &s->hsn, j->t_start - j->t_sub);
    sample(s->hs, &s->hsn, t0 - j->t_end);
    smtp_enter(s);
    if (!c->dead)
        after_job(c, j);
    if (!c->dead && c->eof && !c->busy && !c->inq.n)
        smtp_conn_kill(c, "SMTP_CONNECTION", c->quitting ? "SMTP: closed" : "SMTP: the server closed the connection");
    if (c->cur)
        c->cur->main_ns += smtp_now_ns() - t0;
    free(j->data);
    free(j);
    conn_unref(c);
    smtp_leave(s);
}

static int submit(smtp_conn_t* c, int op, uint8_t* data, size_t n, const smtp_msg_t* m)
{
    smtp_job_t* j = (smtp_job_t*)calloc(1, sizeof *j);
    if (!j) {
        free(data);
        return -1;
    }
    j->c = c;
    j->op = op;
    j->data = data;
    j->n = n;
    if (m) {
        j->m.from = m->from;
        j->m.to = (const char* const*)m->to;
        j->m.nto = m->nto;
        j->m.body = m->body;
        j->m.bodyn = m->bodyn;
        j->m.bdat = m->bdat;
        j->m.ret = m->ret;
        j->m.envid = m->envid;
        j->m.notify = m->notify;
        j->m.orcpt = m->orcpt;
    }
    c->busy = 1;
    c->refs++;
    j->t_sub = smtp_now_ns();
    if (dyn_aio_offload_cpu(c->s->aio, job_work, job_done, j) != 0)
        c->parked = j;
    return 0;
}

void smtp_run_parked(smtp_t* s)
{
    smtp_conn_t *c, *n;
    int again = 1;
    while (again) {
        again = 0;
        for (c = s->conns; c; c = n) {
            smtp_job_t* j = (smtp_job_t*)c->parked;
            c->refs++;
            if (j && dyn_aio_pool_warm(s->aio) != 0) {
                c->parked = NULL;
                job_work(j);
                job_done(j);
                again = 1;
            }
            n = c->next;
            conn_unref(c);
        }
    }
}

static void feed_inq(smtp_conn_t* c)
{
    uint8_t* d;
    size_t n;
    if (c->busy || c->dead || !c->inq.n)
        return;
    d = (uint8_t*)c->inq.p;
    n = c->inq.n;
    dyn_sb_init(&c->inq);
    if (submit(c, OP_FEED, d, n, NULL) < 0)
        smtp_conn_kill(c, "SMTP_CONNECTION", "SMTP: out of memory");
}

static void flush_out(smtp_conn_t* c)
{
    smtp_t* s = c->s;
    if (!c->eng.out.n || c->fd < 0)
        return;
    if (dyn_aio_send(s->aio, c->fd, c->eng.out.p, c->eng.out.n, 0, NULL, NULL) < 0) {
        smtp_conn_kill(c, "SMTP_CONNECTION", "SMTP: cannot write to the socket");
        return;
    }
    c->eng.out.n = 0;
    c->deadline_ms = dyn_timer_now_ms() + s->command_ms;
}

void smtp_conn_kill(smtp_conn_t* c, const char* code, const char* msg)
{
    smtp_t* s = c->s;
    int was_ready = c->ready;
    smtp_msg_t* m;
    if (c->dead)
        return;
    c->refs++;
    conn_close_fd(c);
    if (c->cur) {
        m = c->cur;
        c->cur = NULL;
        smtp_settle_msg(s, m, 1, smtp_error(s->ctx, code, msg, NULL, NULL));
    }
    if (!was_ready && !c->quitting && !c->keepq) {
        while ((m = s->qh) != NULL) {
            s->qh = m->next;
            if (!s->qh)
                s->qt = NULL;
            s->nq--;
            smtp_settle_msg(s, m, 1, smtp_error(s->ctx, code, msg, NULL, NULL));
        }
    }
    conn_unref(c);
    smtp_pump(s);
}

static void conn_kill_engine(smtp_conn_t* c)
{
    const char* code = "SMTP_CONNECTION";
    const char* em = c->eng.err.p ? c->eng.err.p : "SMTP: protocol error";
    if (strstr(em, "AUTH") || strstr(em, "SCRAM") || strstr(em, "authentication"))
        code = "SMTP_AUTH";
    else if (strstr(em, "STARTTLS"))
        code = "SMTP_TLS";
    if (c->cur) {
        smtp_msg_t* m = c->cur;
        c->cur = NULL;
        if (c->eng.last.code == 421 && c->sent) {
            m->next = c->s->qh;
            c->s->qh = m;
            if (!c->s->qt)
                c->s->qt = m;
            c->s->nq++;
            c->keepq = 1;
        } else
            smtp_settle_msg(c->s, m, 1, smtp_error(c->s->ctx, code, em, &c->eng, c->eng.last.code ? &c->eng.last : NULL));
    }
    smtp_conn_kill(c, code, em);
}

#ifdef CONFIG_TLS
static void on_tls_done(dyn_aio_t* aio, int res, const uint8_t* buf, unsigned len, void* ud)
{
    smtp_conn_t* c = (smtp_conn_t*)ud;
    (void)aio;
    (void)buf;
    (void)len;
    smtp_t* s = c->s;
    if (c->dead)
        return;
    smtp_enter(s);
    c->refs++;
    if (res < 0) {
        smtp_conn_kill(c, "SMTP_TLS", "SMTP: TLS handshake failed");
    } else if (c->tls_wait) {
        c->tls_wait = 0;
        if (submit(c, OP_TLS, NULL, 0, NULL) < 0)
            smtp_conn_kill(c, "SMTP_CONNECTION", "SMTP: out of memory");
    }
    conn_unref(c);
    smtp_leave(s);
}

static int start_tls(smtp_conn_t* c)
{
    smtp_t* s = c->s;
    dyn_tls_opts_t to;
    char terr[192];
    dyn_tls_conn_t* t;
    memset(&to, 0, sizeof to);
    to.ca_file = s->ca;
    if (!s->tls_ctx)
        s->tls_ctx = dyn_tls_ctx_client(&to, terr, sizeof terr);
    if (!s->tls_ctx) {
        smtp_conn_kill(c, "SMTP_TLS", terr);
        return -1;
    }
    t = dyn_tls_conn_new(s->tls_ctx, s->servername ? s->servername : s->host, terr, sizeof terr);
    if (!t) {
        smtp_conn_kill(c, "SMTP_TLS", terr);
        return -1;
    }
    if (dyn_aio_tls_attach(s->aio, c->fd, t, on_tls_done, c) < 0) {
        dyn_tls_conn_free(t);
        smtp_conn_kill(c, "SMTP_TLS", "SMTP: cannot attach the TLS engine");
        return -1;
    }
    if (dyn_aio_tls_start(s->aio, c->fd) < 0) {
        smtp_conn_kill(c, "SMTP_TLS", "SMTP: TLS handshake failed to start");
        return -1;
    }
    c->deadline_ms = dyn_timer_now_ms() + s->command_ms;
    return 0;
}
#endif

static void after_job(smtp_conn_t* c, smtp_job_t* j)
{
    smtp_t* s = c->s;
    unsigned ev = c->eng.ev;
    c->eng.ev = 0;
    flush_out(c);
    if (c->dead)
        return;
    if (j->op == OP_SEND && j->rc != MAILK_OK && !(ev & MAILK_EV_ERROR)) {
        smtp_msg_t* m = c->cur;
        c->cur = NULL;
        if (m)
            smtp_settle_msg(s, m, 1, smtp_error(s->ctx, "SMTP_LOCAL", c->eng.err.p ? c->eng.err.p : "SMTP: message refused locally", NULL, NULL));
        c->ready = 1;
        c->idle_ms = dyn_timer_now_ms();
        c->deadline_ms = 0;
        smtp_pump(s);
        return;
    }
    if (ev & MAILK_EV_ERROR) {
        conn_kill_engine(c);
        return;
    }
    if (ev & MAILK_EV_TLS) {
        if (c->inq.n) {
            smtp_conn_kill(c, "SMTP_TLS", "SMTP: data after the STARTTLS 220 reply (possible command injection)");
            return;
        }
#ifdef CONFIG_TLS
        c->tls_wait = 1;
        if (start_tls(c) < 0)
            return;
#else
        smtp_conn_kill(c, "SMTP_TLS", "SMTP: built without TLS");
        return;
#endif
    }
    if (ev & MAILK_EV_DONE && c->cur) {
        smtp_msg_t* m = c->cur;
        int ok = !c->eng.failed && c->eng.final.code / 100 == 2;
        c->cur = NULL;
        c->sent++;
        if (ok)
            smtp_settle_msg(s, m, 0, smtp_result(s->ctx, m, &c->eng));
        else {
            const mailk_res_t* r = c->eng.mail.code / 100 != 2 ? &c->eng.mail : c->eng.final.code ? &c->eng.final : NULL;
            JSValue err = smtp_error(s->ctx, "SMTP_REJECTED",
                                         c->eng.mail.code / 100 != 2 ? "SMTP: sender rejected" : !c->eng.ok ? "SMTP: every recipient was rejected" : "SMTP: message rejected",
                &c->eng, r);
            smtp_add_rejected(s->ctx, err, m, &c->eng);
            smtp_settle_msg(s, m, 1, err);
        }
        if (c->dead)
            return;
    }
    if (ev & MAILK_EV_CLOSED) {
        conn_close_fd(c);
        smtp_pump(s);
        return;
    }
    if (ev & (MAILK_EV_READY | MAILK_EV_DONE)) {
        c->ready = 1;
        c->idle_ms = dyn_timer_now_ms();
        c->deadline_ms = 0;
    }
    feed_inq(c);
    smtp_pump(s);
}

static void on_recv(dyn_aio_t* aio, int res, const uint8_t* buf, unsigned len, void* ud)
{
    smtp_conn_t* c = (smtp_conn_t*)ud;
    uint64_t t0 = smtp_now_ns();
    (void)aio;
    smtp_t* s = c->s;
    if (c->dead)
        return;
    smtp_enter(s);
    c->refs++;
    if (res == 0 && len == 0 && (c->busy || c->inq.n)) {
        c->eof = 1;
    } else if (res < 0 || (res == 0 && len == 0)) {
        smtp_conn_kill(c, "SMTP_CONNECTION", c->quitting ? "SMTP: closed" : "SMTP: the server closed the connection");
    } else if (c->inq.n + len > MAILK_REPLY_LINE_MAX * 64u) {
        smtp_conn_kill(c, "SMTP_CONNECTION", "SMTP: the server sent too much data");
    } else {
        dyn_sb_put(&c->inq, (const char*)buf, len);
        if (c->inq.oom)
            smtp_conn_kill(c, "SMTP_CONNECTION", "SMTP: out of memory");
        else
            feed_inq(c);
    }
    if (!c->dead && c->cur)
        c->cur->main_ns += smtp_now_ns() - t0;
    conn_unref(c);
    smtp_leave(s);
}

static void on_connect(dyn_aio_t* aio, int res, const uint8_t* buf, unsigned len, void* ud)
{
    smtp_conn_t* c = (smtp_conn_t*)ud;
    smtp_t* s = c->s;
    (void)aio;
    (void)buf;
    (void)len;
    if (c->dead)
        return;
    smtp_enter(s);
    c->refs++;
    if (res < 0) {
        smtp_conn_kill(c, "SMTP_CONNECTION", "SMTP: connect failed");
    } else if (dyn_aio_recv(s->aio, c->fd, 0, 1, on_recv, c) < 0) {
        smtp_conn_kill(c, "SMTP_CONNECTION", "SMTP: cannot read from the socket");
    } else if (s->tls == MAILK_TLS_IMPLICIT) {
#ifdef CONFIG_TLS
        start_tls(c);
#endif
    }
    conn_unref(c);
    smtp_leave(s);
}

static smtp_conn_t* conn_open(smtp_t* s)
{
    smtp_conn_t* c = (smtp_conn_t*)calloc(1, sizeof *c);
    mailk_cli_opts_t o;
    if (!c)
        return NULL;
    memset(&o, 0, sizeof o);
    o.name = s->name;
    o.tls = s->tls;
    o.mechs = s->mechs;
    o.insecure_auth = s->insecure_auth;
    o.user = s->user;
    o.pass = s->pass;
    o.token = s->token;
    o.authzid = s->authzid;
    o.scram_max_iters = s->scram_max_iters;
    c->s = s;
    c->fd = -1;
    c->refs = 1;
    dyn_sb_init(&c->inq);
    if (mailk_cli_init(&c->eng, &o) != MAILK_OK) {
        mailk_cli_free(&c->eng);
        free(c);
        return NULL;
    }
    c->next = s->conns;
    s->conns = c;
    s->nconns++;
    s->refs++;
    c->deadline_ms = dyn_timer_now_ms() + s->connect_ms;
    c->fd = dyn_aio_connect(s->aio, s->host, s->port, on_connect, c);
    if (c->fd < 0) {
        smtp_conn_kill(c, "SMTP_CONNECTION", "SMTP: connect failed");
        return NULL;
    }
    return c;
}

void smtp_pump(smtp_t* s)
{
    smtp_conn_t* c;
    if (s->closing)
        return;
    for (c = s->conns; c && s->qh; c = c->next) {
        smtp_msg_t* m;
        if (c->dead || c->busy || !c->ready || c->cur || c->quitting)
            continue;
        if (s->max_per_conn && c->sent >= s->max_per_conn)
            continue;
        m = s->qh;
        s->qh = m->next;
        if (!s->qh)
            s->qt = NULL;
        s->nq--;
        m->next = NULL;
        c->cur = m;
        c->ready = 0;
        if (submit(c, OP_SEND, NULL, 0, m) < 0) {
            smtp_conn_kill(c, "SMTP_CONNECTION", "SMTP: out of memory");
            return;
        }
    }
    if (s->qh) {
        int pending = 0;
        for (c = s->conns; c; c = c->next)
            if (!c->dead && !c->quitting && !c->ready && !c->cur)
                pending++;
        while (s->nconns < s->max_conns && pending < s->nq) {
            if (!conn_open(s))
                break;
            pending++;
        }
    }
    for (c = s->conns; c; c = c->next) {
        if (c->dead || c->busy || !c->ready || c->cur || c->quitting)
            continue;
        if ((s->max_per_conn && c->sent >= s->max_per_conn) || (s->quitting && !s->qh)) {
            c->quitting = 1;
            c->ready = 0;
            submit(c, OP_QUIT, NULL, 0, NULL);
        }
    }
    smtp_release_if_idle(s);
}

void smtp_tick(void* ud)
{
    smtp_t* s = (smtp_t*)ud;
    smtp_conn_t *c, *n;
    uint64_t now = dyn_timer_now_ms();
    smtp_enter(s);
    for (c = s->conns; c; c = n) {
        n = c->next;
        if (c->dead)
            continue;
        if (c->parked && dyn_aio_cpu_room(s->aio)) {
            void* j = c->parked;
            c->parked = NULL;
            if (dyn_aio_offload_cpu(s->aio, job_work, job_done, j) != 0)
                c->parked = j;
        }
        if (c->deadline_ms && now >= c->deadline_ms) {
            smtp_conn_kill(c, "SMTP_TIMEOUT", c->ready ? "SMTP: command timed out" : "SMTP: timed out waiting for the server");
            continue;
        }
        if (c->ready && !c->busy && !c->cur && !c->quitting && !s->qh && now - c->idle_ms >= s->idle_timeout_ms) {
            c->quitting = 1;
            c->ready = 0;
            submit(c, OP_QUIT, NULL, 0, NULL);
        }
    }
    smtp_leave(s);
}
#endif
