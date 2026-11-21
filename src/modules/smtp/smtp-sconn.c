#include "smtp-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_SMTP)

enum { SOP_FEED = 1, SOP_TLS, SOP_TIMEOUT };

#define SSRV_INQ_MAX (1u << 20)

typedef struct {
    ssrv_conn_t* c;
    int op, has_dec, msg_end;
    ssrv_dec_t dec;
    uint8_t* data;
    size_t n, used;
    uint32_t tmo;
    uint64_t t_sub, t_start, t_end;
} ssrv_job_t;

static void on_recv(dyn_aio_t* aio, int res, const uint8_t* buf, unsigned len, void* ud);
static void job_done_cb(void* arg);

static void ring(uint32_t* r, uint64_t* n, uint64_t ns)
{
    r[*n % SMTP_SAMPLES] = ns / 1000 > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)(ns / 1000);
    (*n)++;
}

static void conn_free(ssrv_conn_t* c)
{
    ssrv_t* sv = c->sv;
    ssrv_conn_t** pp;
    for (pp = &sv->conns; *pp; pp = &(*pp)->next)
        if (*pp == c) {
            *pp = c->next;
            break;
        }
    mailk_srv_free(&c->eng);
    dyn_sb_free(&c->inq);
    JS_FreeValueRT(sv->rt, c->session);
    free(c->dec.text);
    free(c->parked);
    free(c);
    ssrv_unref(sv);
}

static void conn_unref(ssrv_conn_t* c)
{
    if (--c->refs == 0)
        conn_free(c);
}

void ssrv_conn_unref(ssrv_conn_t* c)
{
    conn_unref(c);
}

static void close_fd(ssrv_conn_t* c)
{
    if (c->dead)
        return;
    c->dead = 1;
    c->sv->nconns--;
    if (c->fd >= 0) {
        dyn_aio_close(c->sv->aio, c->fd);
        c->fd = -1;
    }
    conn_unref(c);
}

static void on_sent_close(dyn_aio_t* aio, int res, const uint8_t* buf, unsigned len, void* ud)
{
    ssrv_conn_t* c = (ssrv_conn_t*)ud;
    (void)aio;
    (void)res;
    (void)buf;
    (void)len;
    close_fd(c);
    conn_unref(c);
}

static void finish(ssrv_conn_t* c)
{
    c->closing = 1;
    if (c->dead)
        return;
    if (c->bye) {
        dyn_sb_puts(&c->eng.out, c->bye);
        c->bye = NULL;
    }
    if (!c->eng.out.n || c->eng.out.oom) {
        close_fd(c);
        return;
    }
    c->refs += 2;
    if (dyn_aio_send(c->sv->aio, c->fd, c->eng.out.p, c->eng.out.n, 0, on_sent_close, c) != 0) {
        close_fd(c);
        conn_unref(c);
    }
    c->eng.out.n = 0;
    conn_unref(c);
}

void ssrv_kill(ssrv_conn_t* c, int bye)
{
    if (c->dead)
        return;
    c->bye = bye ? "421 4.3.2 Service shutting down\r\n" : NULL;
    c->closing = 1;
    if (!bye)
        close_fd(c);
    else if (!c->busy)
        finish(c);
}

static void flush(ssrv_conn_t* c)
{
    if (c->dead || !c->eng.out.n)
        return;
    dyn_aio_send(c->sv->aio, c->fd, c->eng.out.p, c->eng.out.n, 0, NULL, NULL);
    c->eng.out.n = 0;
}

static void job_work(void* arg)
{
    ssrv_job_t* j = (ssrv_job_t*)arg;
    mailk_srv_t* e = &j->c->eng;
    j->t_start = smtp_now_ns();
    if (j->has_dec) {
        int p = e->pend, rc;
        rc = j->dec.scram ? mailk_srv_scram_creds(e, j->dec.salt, j->dec.saltn, j->dec.iters, j->dec.stored, j->dec.server)
                          : mailk_srv_decide(e, j->dec.code, j->dec.esc[0] ? j->dec.esc : NULL, j->dec.text);
        if (rc == MAILK_EBAD && e->pend)
            mailk_srv_decide(e, p == MAILK_PEND_AUTH ? 454 : 451, p == MAILK_PEND_AUTH ? "4.7.0" : "4.3.0", p == MAILK_PEND_AUTH ? "Temporary authentication failure" : "Local error in processing");
    }
    if (j->op == SOP_FEED && j->n)
        mailk_srv_feed(e, j->data, j->n, &j->used);
    else if (j->op == SOP_TLS)
        mailk_srv_tls_done(e);
    else if (j->op == SOP_TIMEOUT)
        mailk_srv_timeout(e);
    j->tmo = mailk_srv_timeout_ms(e);
    j->t_end = smtp_now_ns();
}

static int submit(ssrv_conn_t* c, int op)
{
    ssrv_job_t* j = (ssrv_job_t*)calloc(1, sizeof *j);
    if (!j)
        return -1;
    j->c = c;
    j->op = op;
    if (op == SOP_FEED) {
        j->data = (uint8_t*)c->inq.p;
        j->n = c->inq.n;
        dyn_sb_init(&c->inq);
    }
    if (c->has_dec) {
        j->has_dec = 1;
        j->msg_end = c->eng.pend == MAILK_PEND_DATA;
        j->dec = c->dec;
        c->dec.text = NULL;
        c->has_dec = 0;
    }
    c->busy = 1;
    c->refs++;
    j->t_sub = smtp_now_ns();
    if (dyn_aio_offload_cpu(c->sv->aio, job_work, job_done_cb, j) != 0)
        c->parked = j;
    return 0;
}

static void start_tls(ssrv_conn_t* c);

static void after_job(ssrv_conn_t* c)
{
    unsigned ev = c->eng.ev;
    if ((ev & MAILK_SEV_CLOSE) || c->closing) {
        finish(c);
    } else if (ev & MAILK_SEV_TLS) {
        flush(c);
        c->inq.n = 0;
        start_tls(c);
    } else {
        flush(c);
        if (ev & (MAILK_SEV_PENDING | MAILK_SEV_BODY))
            ssrv_dispatch(c);
        else
            ssrv_kick(c);
    }
}

static void job_done(void* arg)
{
    ssrv_job_t* j = (ssrv_job_t*)arg;
    ssrv_conn_t* c = j->c;
    ssrv_t* sv = c->sv;
    uint64_t t0 = smtp_now_ns();
    c->busy = 0;
    sv->jobs++;
    ring(sv->hs, &sv->hsn, j->t_start - j->t_sub);
    ring(sv->hs, &sv->hsn, t0 - j->t_end);
    sv->cb_depth++;
    if (!c->dead) {
        if (j->used < j->n) {
            dyn_sb_t t;
            dyn_sb_init(&t);
            dyn_sb_put(&t, (const char*)j->data + j->used, j->n - j->used);
            dyn_sb_put(&t, c->inq.p, c->inq.n);
            dyn_sb_free(&c->inq);
            c->inq = t;
        }
        c->tmo_ms = j->tmo;
        c->deadline_ms = dyn_timer_now_ms() + j->tmo;
        after_job(c);
    }
    c->main_ns += smtp_now_ns() - t0 - c->hook_ns;
    c->hook_ns = 0;
    if (j->msg_end) {
        sv->msgs++;
        ring(sv->ms, &sv->msn, c->main_ns);
        c->main_ns = 0;
    }
    free(j->data);
    free(j->dec.text);
    free(j);
    conn_unref(c);
    sv->cb_depth--;
    ssrv_unref(sv);
}

static void job_done_cb(void* arg)
{
    ssrv_job_t* j = (ssrv_job_t*)arg;
    j->c->sv->refs++;
    job_done(arg);
}

void ssrv_kick(ssrv_conn_t* c)
{
    ssrv_t* sv = c->sv;
    if (c->dead || c->busy || c->waiting || c->tls_wait || c->closing)
        return;
    if (c->paused && c->inq.n < SSRV_INQ_MAX / 2) {
        c->paused = 0;
        dyn_aio_recv(sv->aio, c->fd, 0, 1, on_recv, c);
    }
    if (!c->has_dec && (!c->inq.n || (c->eng.ev & (MAILK_SEV_PENDING | MAILK_SEV_BODY))))
        return;
    submit(c, SOP_FEED);
}

static void on_recv(dyn_aio_t* aio, int res, const uint8_t* buf, unsigned len, void* ud)
{
    ssrv_conn_t* c = (ssrv_conn_t*)ud;
    uint64_t t0 = smtp_now_ns();
    if (c->dead)
        return;
    c->refs++;
    if (res <= 0) {
        ssrv_kill(c, 0);
    } else {
        dyn_sb_put(&c->inq, (const char*)buf, len);
        if (c->inq.oom)
            ssrv_kill(c, 0);
        else {
            if (c->inq.n > SSRV_INQ_MAX && !c->paused) {
                c->paused = 1;
                dyn_aio_cancel(aio, on_recv, c);
            }
            c->deadline_ms = dyn_timer_now_ms() + (c->tmo_ms ? c->tmo_ms : c->sv->eo.cmd_ms);
            ssrv_kick(c);
        }
    }
    c->main_ns += smtp_now_ns() - t0;
    conn_unref(c);
}

#ifdef CONFIG_TLS
static void on_tls_hs(dyn_aio_t* aio, int res, const uint8_t* buf, unsigned len, void* ud)
{
    ssrv_conn_t* c = (ssrv_conn_t*)ud;
    (void)aio;
    (void)buf;
    (void)len;
    if (c->dead)
        return;
    c->refs++;
    c->tls_wait = 0;
    if (res < 0)
        ssrv_kill(c, 0);
    else if (c->implicit_hs) {
        c->implicit_hs = 0;
        flush(c);
        ssrv_kick(c);
    } else if (submit(c, SOP_TLS) < 0)
        ssrv_kill(c, 0);
    conn_unref(c);
}
#endif

static void start_tls(ssrv_conn_t* c)
{
#ifdef CONFIG_TLS
    char terr[192];
    dyn_tls_conn_t* t = dyn_tls_conn_accept(c->sv->tls_ctx, terr, sizeof terr);
    c->tls_wait = 1;
    if (!t || dyn_aio_tls_attach(c->sv->aio, c->fd, t, on_tls_hs, c) < 0) {
        if (t)
            dyn_tls_conn_free(t);
        ssrv_kill(c, 0);
        return;
    }
    if (dyn_aio_tls_start(c->sv->aio, c->fd) < 0)
        ssrv_kill(c, 0);
#else
    ssrv_kill(c, 0);
#endif
}

static void refuse_sent(dyn_aio_t* aio, int res, const uint8_t* buf, unsigned len, void* ud)
{
    (void)res;
    (void)buf;
    (void)len;
    dyn_aio_close(aio, (int)(intptr_t)ud);
}

void ssrv_on_accept(dyn_aio_t* aio, int res, const uint8_t* buf, unsigned len, void* ud)
{
    static const char busy[] = "421 4.7.0 Too many connections, try again later\r\n";
    ssrv_t* sv = (ssrv_t*)ud;
    ssrv_conn_t* c;
    (void)buf;
    (void)len;
    if (res < 0)
        return;
    if (sv->closing || sv->nconns >= sv->max_conns) {
        sv->refused++;
        if (sv->closing || dyn_aio_send(aio, res, busy, sizeof busy - 1, 0, refuse_sent, (void*)(intptr_t)res) != 0)
            dyn_aio_close(aio, res);
        return;
    }
    c = (ssrv_conn_t*)calloc(1, sizeof *c);
    if (!c || mailk_srv_init(&c->eng, &sv->eo) != MAILK_OK) {
        if (c) {
            mailk_srv_free(&c->eng);
            free(c);
        }
        dyn_aio_close(aio, res);
        return;
    }
    c->sv = sv;
    c->fd = res;
    c->refs = 1;
    c->session = JS_UNDEFINED;
    sv->refs++;
    c->next = sv->conns;
    sv->conns = c;
    sv->nconns++;
    sv->sessions++;
    if (ssrv_session_new(c) < 0) {
        close_fd(c);
        return;
    }
    c->tmo_ms = sv->eo.cmd_ms;
    c->deadline_ms = dyn_timer_now_ms() + c->tmo_ms;
    if (sv->tls == MAILK_TLS_IMPLICIT) {
        c->implicit_hs = 1;
        start_tls(c);
    } else
        flush(c);
    if (!c->dead && dyn_aio_recv(aio, c->fd, 0, 1, on_recv, c) < 0)
        ssrv_kill(c, 0);
}

void ssrv_run_parked(ssrv_t* sv)
{
    ssrv_conn_t *c, *next;
    int again = 1;
    while (again) {
        again = 0;
        for (c = sv->conns; c; c = next) {
            void* j = c->parked;
            c->refs++;
            if (j && dyn_aio_pool_warm(sv->aio) != 0) {
                c->parked = NULL;
                job_work(j);
                job_done_cb(j);
                again = 1;
            }
            next = c->next;
            conn_unref(c);
        }
    }
}

void ssrv_tick(void* ud)
{
    ssrv_t* sv = (ssrv_t*)ud;
    ssrv_conn_t *c, *next;
    uint64_t now = dyn_timer_now_ms();
    sv->cb_depth++;
    ssrv_run_parked(sv);
    for (c = sv->conns; c; c = next) {
        c->refs++;
        if (c->parked) {
            void* j = c->parked;
            c->parked = NULL;
            if (dyn_aio_offload_cpu(sv->aio, job_work, job_done_cb, j) != 0)
                c->parked = j;
        } else if (!c->dead && !c->busy && !c->waiting && !c->closing && c->deadline_ms && now >= c->deadline_ms) {
            c->deadline_ms = 0;
            if (c->tls_wait)
                ssrv_kill(c, 0);
            else
                submit(c, SOP_TIMEOUT);
        }
        next = c->next;
        conn_unref(c);
    }
    sv->cb_depth--;
    sv->refs++;
    ssrv_unref(sv);
}
#endif
