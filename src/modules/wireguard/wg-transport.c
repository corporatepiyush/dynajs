#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_WIREGUARD)

#include "wg-internal.h"

#include <string.h>

size_t wg_pad_len(size_t n, unsigned mtu)
{
    size_t last = n, padded;
    if (mtu == 0)
        return (n + WG_PAD - 1) & ~(size_t)(WG_PAD - 1);
    if (last > mtu)
        last %= mtu;
    padded = (last + WG_PAD - 1) & ~(size_t)(WG_PAD - 1);
    if (padded > mtu)
        padded = mtu;
    return n - last + padded;
}

int wg_data_parse(const uint8_t* msg, size_t n, uint32_t* receiver, uint64_t* ctr)
{
    if (n < WG_DATA_MIN || wg_le32(msg) != WG_MSG_DATA)
        return -1;
    *receiver = wg_le32(msg + 4);
    *ctr = wg_le64(msg + 8);
    return 0;
}

int wg_data_seal(const uint8_t key[WG_KEY], uint32_t receiver, uint64_t ctr, uint8_t* buf, size_t n, size_t cap, unsigned mtu)
{
    size_t padded = wg_pad_len(n, mtu);
    if (padded > cap || cap - padded < WG_DATA_MIN || ctr >= WG_REJECT_AFTER_MESSAGES)
        return -1;
    memset(buf + WG_DATA_HDR + n, 0, padded - n);
    wg_put_le32(buf, WG_MSG_DATA);
    wg_put_le32(buf + 4, receiver);
    wg_put_le64(buf + 8, ctr);
    if (wg_aead_seal(key, ctr, buf + WG_DATA_HDR, padded, NULL, 0, buf + WG_DATA_HDR) < 0)
        return -1;
    return (int)(padded + WG_DATA_MIN);
}

int wg_data_open(const uint8_t key[WG_KEY], uint8_t* buf, size_t n)
{
    uint32_t rx;
    uint64_t ctr;
    if (wg_data_parse(buf, n, &rx, &ctr) < 0 || ctr >= WG_REJECT_AFTER_MESSAGES || n - WG_DATA_HDR > 0x7fffffffu)
        return -1;
    if (wg_aead_open(key, ctr, buf + WG_DATA_HDR, n - WG_DATA_HDR, NULL, 0, buf + WG_DATA_HDR) < 0)
        return -1;
    return (int)(n - WG_DATA_MIN);
}

int wg_kp_can_send(const wg_keypair_t* kp, uint64_t now_ms)
{
    return kp && kp->send_ctr < WG_REJECT_AFTER_MESSAGES && now_ms - kp->created_ms < WG_REJECT_AFTER_MS;
}

int wg_kp_can_recv(const wg_keypair_t* kp, uint64_t now_ms)
{
    return kp && now_ms - kp->created_ms <= WG_REJECT_AFTER_MS;
}

int wg_kp_fresh_send(const wg_keypair_t* kp, uint64_t now_ms)
{
    return kp->send_ctr > WG_REKEY_AFTER_MESSAGES || (kp->initiator && now_ms - kp->created_ms > WG_REKEY_AFTER_MS);
}

int wg_kp_fresh_recv(const wg_keypair_t* kp, uint64_t now_ms)
{
    return kp->initiator && now_ms - kp->created_ms > WG_REJECT_AFTER_MS - WG_KEEPALIVE_MS - WG_REKEY_TIMEOUT_MS;
}

static void drop(wg_kp_drop_fn fn, void* ud, wg_keypair_t* kp)
{
    if (kp)
        fn(ud, kp);
}

void wg_slots_begin(wg_slots_t* s, wg_keypair_t* kp, wg_kp_drop_fn fn, void* ud)
{
    wg_keypair_t *prev = s->prev, *cur = s->cur, *next = s->next;
    if (kp->initiator) {
        if (next) {
            s->prev = next;
            s->next = NULL;
            drop(fn, ud, cur);
        } else
            s->prev = cur;
        drop(fn, ud, prev);
        s->cur = kp;
    } else {
        s->next = kp;
        drop(fn, ud, next);
        s->prev = NULL;
        drop(fn, ud, prev);
    }
}

int wg_slots_received(wg_slots_t* s, wg_keypair_t* kp, wg_kp_drop_fn fn, void* ud)
{
    if (!kp || kp != s->next)
        return 0;
    drop(fn, ud, s->prev);
    s->prev = s->cur;
    s->cur = kp;
    s->next = NULL;
    return 1;
}

void wg_slots_clear(wg_slots_t* s, wg_kp_drop_fn fn, void* ud)
{
    wg_keypair_t *prev = s->prev, *cur = s->cur, *next = s->next;
    s->prev = s->cur = s->next = NULL;
    drop(fn, ud, prev);
    drop(fn, ud, cur);
    drop(fn, ud, next);
}

void wg_batch_work(void* arg)
{
    wg_batch_t* b = (wg_batch_t*)arg;
    unsigned i;
    for (i = 0; i < b->n; i++) {
        wg_pkt_t* p = &b->pkt[i];
        p->res = b->enc ? wg_data_seal(b->key, b->remote_index, p->ctr, p->buf, p->len, p->cap, b->mtu) : wg_data_open(b->key, p->buf, p->len);
    }
}

void wg_fifo_push(wg_fifo_t* q, wg_batch_t* b)
{
    b->next = NULL;
    b->done = 0;
    if (q->tail)
        q->tail->next = b;
    else
        q->head = b;
    q->tail = b;
}

wg_batch_t* wg_fifo_pop_ready(wg_fifo_t* q)
{
    wg_batch_t* b = q->head;
    if (!b || !b->done)
        return NULL;
    q->head = b->next;
    if (!q->head)
        q->tail = NULL;
    b->next = NULL;
    return b;
}

#endif
