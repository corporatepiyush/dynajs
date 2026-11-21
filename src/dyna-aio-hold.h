#ifndef DYNAJS_AIO_HOLD_H
#define DYNAJS_AIO_HOLD_H

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include "dyna-aio.h"

#define DYN_AIO_HOLD_MAX 65536u

typedef struct dyn_aio_hold_cb {
    dyn_aio_cb cb;
    void* udata;
    size_t len;
} dyn_aio_hold_cb_t;

typedef struct dyn_aio_hold {
    uint8_t* buf;
    dyn_aio_hold_cb_t* cbs;
    size_t len, cap;
    uint32_t ncb, cbcap;
    int next;
    uint8_t listed;
} dyn_aio_hold_t;

typedef dyn_aio_hold_t* (*dyn_aio_hold_get_fn)(dyn_aio_t* a, int fd, uint32_t life);
typedef int (*dyn_aio_hold_queue_fn)(dyn_aio_t* a, int fd, const void* buf, size_t len,
    int flags, dyn_aio_cb cb, void* udata);

#define DYN_AIO_HOLD_POOL_N 32
#define DYN_AIO_HOLD_POOL_CAP (256u * 1024u)

typedef struct dyn_aio_hold_spare {
    uint8_t* buf;
    dyn_aio_hold_cb_t* cbs;
    size_t cap;
    uint32_t cbcap;
} dyn_aio_hold_spare_t;

static _Thread_local dyn_aio_hold_spare_t dyn_aio_hold_pool[DYN_AIO_HOLD_POOL_N];
static _Thread_local unsigned dyn_aio_hold_pool_n;

static inline void dyn_aio_hold_recycle(uint8_t* buf, size_t cap, dyn_aio_hold_cb_t* cbs, uint32_t cbcap)
{
    if (buf && cbs && cap <= DYN_AIO_HOLD_POOL_CAP && dyn_aio_hold_pool_n < DYN_AIO_HOLD_POOL_N) {
        dyn_aio_hold_spare_t* s = &dyn_aio_hold_pool[dyn_aio_hold_pool_n++];
        s->buf = buf;
        s->cap = cap;
        s->cbs = cbs;
        s->cbcap = cbcap;
        return;
    }
    free(buf);
    free(cbs);
}

static inline void dyn_aio_hold_pool_drain(void)
{
    while (dyn_aio_hold_pool_n) {
        dyn_aio_hold_spare_t* s = &dyn_aio_hold_pool[--dyn_aio_hold_pool_n];
        free(s->buf);
        free(s->cbs);
    }
}

static inline int dyn_aio_hold_room(dyn_aio_hold_t* h, size_t len)
{
    if (!h->buf && !h->cbs && dyn_aio_hold_pool_n) {
        dyn_aio_hold_spare_t* s = &dyn_aio_hold_pool[--dyn_aio_hold_pool_n];
        h->buf = s->buf;
        h->cap = s->cap;
        h->cbs = s->cbs;
        h->cbcap = s->cbcap;
    }
    if (h->len + len > h->cap) {
        size_t nc = h->cap ? h->cap : 4096;
        uint8_t* nb;
        while (nc < h->len + len)
            nc *= 2;
        nb = (uint8_t*)realloc(h->buf, nc);
        if (!nb)
            return -1;
        h->buf = nb;
        h->cap = nc;
    }
    if (h->ncb == h->cbcap) {
        uint32_t nc = h->cbcap ? h->cbcap * 2 : 8;
        dyn_aio_hold_cb_t* ncb = (dyn_aio_hold_cb_t*)realloc(h->cbs, (size_t)nc * sizeof(*ncb));
        if (!ncb)
            return -1;
        h->cbs = ncb;
        h->cbcap = nc;
    }
    return 0;
}

static inline int dyn_aio_hold_append(dyn_aio_hold_t* h, const void* buf, size_t len,
    dyn_aio_cb cb, void* udata)
{
    if (dyn_aio_hold_room(h, len) < 0)
        return -1;
    memcpy(h->buf + h->len, buf, len);
    h->len += len;
    h->cbs[h->ncb].cb = cb;
    h->cbs[h->ncb].udata = udata;
    h->cbs[h->ncb].len = len;
    h->ncb++;
    return 0;
}

static inline int dyn_aio_hold_append2(dyn_aio_hold_t* h, const void* b1, size_t n1,
    const void* b2, size_t n2, dyn_aio_cb cb, void* udata)
{
    size_t len = n1 + n2;
    if (dyn_aio_hold_room(h, len) < 0)
        return -1;
    memcpy(h->buf + h->len, b1, n1);
    memcpy(h->buf + h->len + n1, b2, n2);
    h->len += len;
    h->cbs[h->ncb].cb = cb;
    h->cbs[h->ncb].udata = udata;
    h->cbs[h->ncb].len = len;
    h->ncb++;
    return 0;
}

static inline int dyn_aio_send2_concat(dyn_aio_t* a, int fd, const void* head, size_t head_len,
    const void* body, size_t body_len, int flags, dyn_aio_cb cb, void* udata)
{
    uint8_t* tmp = (uint8_t*)malloc(head_len + body_len ? head_len + body_len : 1);
    int rc;
    if (!tmp)
        return -1;
    memcpy(tmp, head, head_len);
    memcpy(tmp + head_len, body, body_len);
    rc = dyn_aio_send(a, fd, tmp, head_len + body_len, flags, cb, udata);
    free(tmp);
    return rc;
}

static inline void dyn_aio_hold_release(dyn_aio_hold_t* h)
{
    dyn_aio_hold_recycle(h->buf, h->cap, h->cbs, h->cbcap);
    h->buf = NULL;
    h->cbs = NULL;
    h->len = h->cap = 0;
    h->ncb = h->cbcap = 0;
}

static inline void dyn_aio_hold_flush(dyn_aio_t* a, int fd, uint32_t life,
    int send_flags, dyn_aio_hold_get_fn get, dyn_aio_hold_queue_fn queue)
{
    dyn_aio_hold_t* h = get(a, fd, life);
    dyn_aio_hold_cb_t* cbs;
    uint8_t* buf;
    size_t len, cap, off = 0, pos = 0;
    uint32_t ncb, cbcap, i;
    int err = 0;

    if (!h || !h->ncb)
        return;
    buf = h->buf;
    len = h->len;
    cap = h->cap;
    cbs = h->cbs;
    ncb = h->ncb;
    cbcap = h->cbcap;
    h->buf = NULL;
    h->cbs = NULL;
    h->len = h->cap = 0;
    h->ncb = h->cbcap = 0;
    while (off < len) {
        ssize_t n = send(fd, buf + off, len - off, send_flags);
        if (n > 0) {
            off += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            break;
        err = n < 0 ? -errno : -EIO;
        break;
    }
    if (!err && off < len) {
        for (i = 0; i < ncb; i++) {
            size_t st = pos, en = pos + cbs[i].len;
            pos = en;
            if (en <= off)
                continue;
            if (st < off)
                st = off;
            if (queue(a, fd, buf + st, en - st, DYN_AIO_NOSEND, cbs[i].cb, cbs[i].udata) < 0) {
                err = errno ? -errno : -ENOMEM;
                break;
            }
            cbs[i].cb = NULL;
        }
    }
    for (i = 0; i < ncb; i++)
        if (cbs[i].cb)
            cbs[i].cb(a, err ? err : (int)cbs[i].len, NULL, 0, cbs[i].udata);
    dyn_aio_hold_recycle(buf, cap, cbs, cbcap);
}

#endif
