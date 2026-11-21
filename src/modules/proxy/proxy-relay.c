#include "proxy-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_PROXY)

#define PXY_HIGH_WATER (256 * 1024)
#define PXY_LOW_WATER (64 * 1024)

static const char* const pxy_upstream_keys[] = { "host", "port" };

static void pxy_sent_client(dyn_aio_t* aio, int res, const uint8_t* buf,
    unsigned len, void* udata);

static void pxy_sent_upstream(dyn_aio_t* aio, int res, const uint8_t* buf,
    unsigned len, void* udata);

static void pxy_half_close(dyn_pxy_pair_t* pr, int from_client)
{
    dyn_pxy_t* p = pr->owner;

    if (from_client) {
        pr->c_eof = 1;
        if (dyn_aio_cancel(p->aio, pxy_recv_client, pr) > 0)
            pr->refs--;
        if (pr->ufd >= 0)
            shutdown(pr->ufd, SHUT_WR);
    } else {
        pr->u_eof = 1;
        if (dyn_aio_cancel(p->aio, pxy_recv_upstream, pr) > 0)
            pr->refs--;
        if (pr->cfd >= 0)
            shutdown(pr->cfd, SHUT_WR);
    }
    if (pr->c_eof && pr->u_eof)
        pxy_pair_close(pr);
}

static void pxy_forward(dyn_pxy_pair_t* pr, const uint8_t* buf, size_t len,
    int to_upstream)
{
    dyn_pxy_t* p = pr->owner;
    int dst = to_upstream ? pr->ufd : pr->cfd;

    if (dst < 0 || len == 0)
        return;
    if (to_upstream) {
        pr->to_u += len;
        p->bytes_up += len;
    } else {
        pr->to_c += len;
        p->bytes_down += len;
    }

    pr->refs++;
    if (dyn_aio_send(p->aio, dst, buf, len, 0,
            to_upstream ? pxy_sent_upstream : pxy_sent_client, pr)
        < 0) {
        pxy_pair_unref(pr);
        pxy_pair_close(pr);
        return;
    }
    if (to_upstream) {
        if (pr->to_u >= PXY_HIGH_WATER && !pr->c_paused && pr->cfd >= 0) {
            if (dyn_aio_cancel(p->aio, pxy_recv_client, pr) > 0) {
                pr->c_paused = 1;
                pr->refs--;
            }
        }
    } else {
        if (pr->to_c >= PXY_HIGH_WATER && !pr->u_paused && pr->ufd >= 0) {
            if (dyn_aio_cancel(p->aio, pxy_recv_upstream, pr) > 0) {
                pr->u_paused = 1;
                pr->refs--;
            }
        }
    }
    pr->last_ms = dyn_timer_now_ms();
}

static void pxy_drained(dyn_pxy_pair_t* pr, int res, int to_upstream)
{
    dyn_pxy_t* p = pr->owner;
    size_t done = res > 0 ? (size_t)res : 0;

    if (to_upstream) {
        pr->to_u = pr->to_u > done ? pr->to_u - done : 0;
        if (pr->c_paused && pr->to_u <= PXY_LOW_WATER && pr->cfd >= 0) {
            pr->c_paused = 0;
            pr->refs++;
            if (dyn_aio_recv(p->aio, pr->cfd, 0, 1, pxy_recv_client, pr) < 0) {
                pr->refs--;
                pxy_pair_close(pr);
            }
        }
    } else {
        pr->to_c = pr->to_c > done ? pr->to_c - done : 0;
        if (pr->u_paused && pr->to_c <= PXY_LOW_WATER && pr->ufd >= 0) {
            pr->u_paused = 0;
            pr->refs++;
            if (dyn_aio_recv(p->aio, pr->ufd, 0, 1, pxy_recv_upstream, pr) < 0) {
                pr->refs--;
                pxy_pair_close(pr);
            }
        }
    }
    if (res < 0)
        pxy_pair_close(pr);
    else
        pr->last_ms = dyn_timer_now_ms();
}

static void pxy_sent_upstream(dyn_aio_t* aio, int res, const uint8_t* buf,
    unsigned len, void* udata)
{
    dyn_pxy_pair_t* pr = (dyn_pxy_pair_t*)udata;
    (void)aio;
    (void)buf;
    (void)len;
    pxy_drained(pr, res, 1);
    pxy_pair_unref(pr);
}

static void pxy_sent_client(dyn_aio_t* aio, int res, const uint8_t* buf,
    unsigned len, void* udata)
{
    dyn_pxy_pair_t* pr = (dyn_pxy_pair_t*)udata;
    (void)aio;
    (void)buf;
    (void)len;
    pxy_drained(pr, res, 0);
    pxy_pair_unref(pr);
}

void pxy_recv_client(dyn_aio_t* aio, int res, const uint8_t* buf,
    unsigned len, void* udata)
{
    dyn_pxy_pair_t* pr = (dyn_pxy_pair_t*)udata;
    dyn_pxy_t* p = pr->owner;
    (void)aio;
    if (pr->cfd < 0)
        return;
    if (res == 0) {
        if (dyn_aio_cancel(p->aio, pxy_recv_client, pr) > 0)
            pr->refs--;
        pxy_half_close(pr, 1);
        return;
    }
    if (res < 0) {
        if (dyn_aio_cancel(p->aio, pxy_recv_client, pr) > 0)
            pr->refs--;
        pxy_pair_close(pr);
        return;
    }
    pxy_forward(pr, buf, len, 1);
}

void pxy_recv_upstream(dyn_aio_t* aio, int res, const uint8_t* buf,
    unsigned len, void* udata)
{
    dyn_pxy_pair_t* pr = (dyn_pxy_pair_t*)udata;
    dyn_pxy_t* p = pr->owner;
    (void)aio;
    if (pr->ufd < 0)
        return;
    if (res == 0) {
        if (dyn_aio_cancel(p->aio, pxy_recv_upstream, pr) > 0)
            pr->refs--;
        pxy_half_close(pr, 0);
        return;
    }
    if (res < 0) {
        if (dyn_aio_cancel(p->aio, pxy_recv_upstream, pr) > 0)
            pr->refs--;
        pxy_pair_close(pr);
        return;
    }
    pxy_forward(pr, buf, len, 0);
}

void pxy_on_accept(dyn_aio_t* aio, int res, const uint8_t* buf,
    unsigned len, void* udata)
{
    dyn_pxy_t* p = (dyn_pxy_t*)udata;
    dyn_pxy_pair_t* pr;
    int idx2;
    (void)buf;
    (void)len;

    if (res < 0)
        return;
    if (p->max_conns && p->npairs >= p->max_conns) {
        p->n_refused++;
        dyn_aio_close(aio, res);
        return;
    }
    pr = (dyn_pxy_pair_t*)calloc(1, sizeof(*pr));
    if (!pr) {
        dyn_aio_close(aio, res);
        return;
    }
    pr->owner = p;
    pr->cfd = res;
    pr->ufd = -1;
    pr->refs = 1;
    pr->last_ms = dyn_timer_now_ms();
    if (p->connect_ms)
        pr->deadline_ms = pr->last_ms + p->connect_ms;

    pr->lnext = p->pairs;
    if (p->pairs)
        p->pairs->lprev = pr;
    p->pairs = pr;
    p->npairs++;
    p->n_accepted++;

    idx2 = p->next_up;
    p->next_up = (p->next_up + 1) % p->n_up;

    pr->refs++;
    pr->ufd = dyn_aio_connect_addr(p->aio, &p->up_sa[idx2], p->up_salen[idx2],
        p->up_fam[idx2], pxy_on_connect, pr);
    if (pr->ufd < 0) {
        p->n_connect_failed++;
        pxy_pair_close(pr);
        pxy_pair_unref(pr);
    }
}

int pxy_read_upstream(JSContext* ctx, dyn_pxy_t* p, JSValueConst o,
    int idx)
{
    JSValue jh, jp;
    const char* h;
    int32_t port = 0;

    if (dyn_opts_strict(ctx, o, pxy_upstream_keys, 2))
        return -1;
    jh = JS_GetPropertyStr(ctx, o, "host");
    jp = JS_GetPropertyStr(ctx, o, "port");

    if (JS_IsException(jh) || JS_IsException(jp)) {
        JS_FreeValue(ctx, jh);
        JS_FreeValue(ctx, jp);
        return -1;
    }
    h = JS_IsUndefined(jh) ? NULL : JS_ToCString(ctx, jh);
    JS_FreeValue(ctx, jh);
    if (JS_ToInt32(ctx, &port, jp) < 0) {
        JS_FreeValue(ctx, jp);
        if (h)
            JS_FreeCString(ctx, h);
        return -1;
    }
    JS_FreeValue(ctx, jp);
    if (port <= 0 || port > 65535) {
        if (h)
            JS_FreeCString(ctx, h);
        JS_ThrowRangeError(ctx, "upstream port out of range");
        return -1;
    }
    p->up_host[idx] = strdup(h ? h : "127.0.0.1");
    if (h)
        JS_FreeCString(ctx, h);
    if (!p->up_host[idx])
        return -1;
    p->up_port[idx] = (uint16_t)port;
    {
        char errbuf[160];
        if (dyn_aio_resolve(p->up_host[idx], p->up_port[idx],
                &p->up_sa[idx], &p->up_salen[idx],
                &p->up_fam[idx])
            != 0) {
            snprintf(errbuf, sizeof errbuf, "TCPProxy: cannot resolve upstream '%s'",
                p->up_host[idx]);
            JS_ThrowTypeError(ctx, "%s", errbuf);
            free(p->up_host[idx]);
            p->up_host[idx] = NULL;
            return -1;
        }
    }
    return 0;
}

#endif
