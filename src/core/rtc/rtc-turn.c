#include "rtc-turn.h"

static void rtc_turn_cpy(char* dst, const char* src)
{
    size_t n = src ? strlen(src) : 0;
    if (n >= RTC_TURN_CRED_MAX)
        n = RTC_TURN_CRED_MAX - 1;
    if (n)
        memcpy(dst, src, n);
    dst[n] = 0;
}

void rtc_turn_init(rtc_turn* t, rtc_rng* rng, int idx, int kind, int sock, const rtc_addr* server, const char* user,
    const char* pass, rtc_turn_send_fn send, rtc_turn_ev_fn ev, void* ud)
{
    memset(t, 0, sizeof(*t));
    t->rng = rng;
    t->idx = idx;
    t->kind = kind;
    t->sock = sock;
    t->server = *server;
    t->send = send;
    t->ev = ev;
    t->ud = ud;
    rtc_turn_cpy(t->user, user);
    rtc_turn_cpy(t->pass, pass);
}

static void rtc_turn_auth(rtc_turn* t, rtc_stun_wr* w)
{
    if (!t->have_key)
        return;
    rtc_stun_wr_attr(w, RTC_SA_USERNAME, t->user, strlen(t->user));
    rtc_stun_wr_attr(w, RTC_SA_REALM, t->realm, strlen(t->realm));
    rtc_stun_wr_attr(w, RTC_SA_NONCE, t->nonce, t->nonce_len);
    rtc_stun_wr_integrity(w, t->key, 16);
}

static void rtc_turn_emit(rtc_turn* t, rtc_turn_tx* x)
{
    uint8_t buf[700];
    rtc_stun_wr w;
    size_t n;
    rtc_turn_perm* pm = &t->perm[x->idx];
    switch (x->kind) {
    case RTC_TX_BINDING:
        rtc_stun_wr_init(&w, buf, sizeof(buf), rtc_stun_type(RTC_STUN_BINDING, RTC_STUN_REQUEST), x->tid);
        rtc_stun_wr_fingerprint(&w);
        break;
    case RTC_TX_ALLOCATE:
        rtc_stun_wr_init(&w, buf, sizeof(buf), rtc_stun_type(RTC_STUN_ALLOCATE, RTC_STUN_REQUEST), x->tid);
        rtc_stun_wr_u32(&w, RTC_SA_REQUESTED_TRANSPORT, 17u << 24);
        rtc_stun_wr_u32(&w, RTC_SA_LIFETIME, 600);
        rtc_turn_auth(t, &w);
        break;
    case RTC_TX_REFRESH:
        rtc_stun_wr_init(&w, buf, sizeof(buf), rtc_stun_type(RTC_STUN_REFRESH, RTC_STUN_REQUEST), x->tid);
        rtc_stun_wr_u32(&w, RTC_SA_LIFETIME, (x->lifetime0 || t->state == RTC_TURN_CLOSED) ? 0 : 600);
        rtc_turn_auth(t, &w);
        break;
    case RTC_TX_PERM:
        rtc_stun_wr_init(&w, buf, sizeof(buf), rtc_stun_type(RTC_STUN_CREATE_PERMISSION, RTC_STUN_REQUEST), x->tid);
        rtc_stun_wr_addr(&w, RTC_SA_XOR_PEER_ADDRESS, &pm->peer);
        rtc_turn_auth(t, &w);
        break;
    case RTC_TX_CHANNEL:
        rtc_stun_wr_init(&w, buf, sizeof(buf), rtc_stun_type(RTC_STUN_CHANNEL_BIND, RTC_STUN_REQUEST), x->tid);
        rtc_stun_wr_u32(&w, RTC_SA_CHANNEL_NUMBER, (uint32_t)pm->chan << 16);
        rtc_stun_wr_addr(&w, RTC_SA_XOR_PEER_ADDRESS, &pm->peer);
        rtc_turn_auth(t, &w);
        break;
    default:
        return;
    }
    n = rtc_stun_wr_done(&w);
    if (n)
        t->send(t->ud, t->sock, &t->server, buf, n);
}

static const uint32_t rtc_turn_rto0 = 500000u;

static rtc_turn_tx* rtc_turn_tx_new(rtc_turn* t, int kind, int idx, uint64_t now)
{
    int i;
    for (i = 0; i < RTC_TURN_MAX_TX; i++) {
        rtc_turn_tx* x = &t->tx[i];
        if (x->kind != RTC_TX_NONE)
            continue;
        memset(x, 0, sizeof(*x));
        x->kind = (uint8_t)kind;
        x->idx = (uint8_t)idx;
        x->tries = 1;
        x->rto_us = rtc_turn_rto0;
        x->next_us = now + x->rto_us;
        rtc_rng_bytes(t->rng, x->tid, 12);
        rtc_turn_emit(t, x);
        if (kind == RTC_TX_CHANNEL)
            t->perm[idx].chan_hold_us = now + 900000000u;
        return x;
    }
    return NULL;
}

static void rtc_turn_fail(rtc_turn* t, int code)
{
    if (t->state == RTC_TURN_FAILED || t->state == RTC_TURN_CLOSED)
        return;
    t->state = RTC_TURN_FAILED;
    t->err_code = code;
    t->ev(t->ud, t->idx, RTC_TURN_EV_FAILED, code);
}

void rtc_turn_start(rtc_turn* t, uint64_t now)
{
    if (t->state != RTC_TURN_IDLE)
        return;
    t->state = RTC_TURN_ALLOCATING;
    rtc_turn_tx_new(t, t->kind == RTC_TURN_KIND_STUN ? RTC_TX_BINDING : RTC_TX_ALLOCATE, 0, now);
}

static int rtc_turn_copy_attr(const rtc_stun_msg* m, uint16_t type, uint8_t* dst, size_t cap, uint16_t* out_len)
{
    const rtc_stun_attr* a = rtc_stun_find(m, type);
    if (!a || a->len >= cap)
        return -1;
    memcpy(dst, m->buf + a->off, a->len);
    dst[a->len] = 0;
    if (out_len)
        *out_len = a->len;
    return 0;
}

static void rtc_turn_on_response(rtc_turn* t, rtc_turn_tx* x, const rtc_stun_msg* m, uint64_t now)
{
    int cls = rtc_stun_class(m->type);
    int kind = x->kind, idx = x->idx, lifetime0 = x->lifetime0;
    rtc_turn_perm* pm = &t->perm[idx];
    x->kind = RTC_TX_NONE;
    if (cls == RTC_STUN_ERROR) {
        int code = rtc_stun_error_code(m);
        if ((code == 401 || code == 438) && kind != RTC_TX_BINDING && t->auth_tries < 3) {
            uint16_t nl = 0;
            if (rtc_turn_copy_attr(m, RTC_SA_NONCE, t->nonce, sizeof(t->nonce), &nl) == 0)
                t->nonce_len = nl;
            if (code == 401) {
                if (rtc_turn_copy_attr(m, RTC_SA_REALM, (uint8_t*)t->realm, sizeof(t->realm), NULL) < 0) {
                    rtc_turn_fail(t, code);
                    return;
                }
                rtc_stun_long_term_key(t->user, t->realm, t->pass, t->key);
                t->have_key = 1;
            }
            t->auth_tries++;
            {
                rtc_turn_tx* nx = rtc_turn_tx_new(t, kind, idx, now);
                if (nx)
                    nx->lifetime0 = (uint8_t)lifetime0;
            }
            return;
        }
        if (kind != RTC_TX_BINDING && code != 401 && code != 438 && t->have_key && !rtc_stun_check_integrity(m, t->key, 16)) {
            x->kind = (uint8_t)kind;
            return;
        }
        if (kind == RTC_TX_ALLOCATE || kind == RTC_TX_BINDING)
            rtc_turn_fail(t, code < 0 ? 0 : code);
        else if (kind == RTC_TX_PERM)
            pm->perm_pending = 0;
        else if (kind == RTC_TX_CHANNEL)
            pm->chan_pending = 0;
        return;
    }
    if (kind != RTC_TX_BINDING && t->have_key && !rtc_stun_check_integrity(m, t->key, 16))
        return;
    t->auth_tries = 0;
    switch (kind) {
    case RTC_TX_BINDING: {
        const rtc_stun_attr* a = rtc_stun_find(m, RTC_SA_XOR_MAPPED_ADDRESS);
        if (!a)
            a = rtc_stun_find(m, RTC_SA_MAPPED_ADDRESS);
        if (rtc_stun_get_addr(m, a, &t->mapped) < 0) {
            rtc_turn_fail(t, 0);
            return;
        }
        t->state = RTC_TURN_READY;
        t->ev(t->ud, t->idx, RTC_TURN_EV_MAPPED, 0);
        break;
    }
    case RTC_TX_ALLOCATE: {
        uint32_t lt = 600;
        if (rtc_stun_get_addr(m, rtc_stun_find(m, RTC_SA_XOR_RELAYED_ADDRESS), &t->relayed) < 0) {
            rtc_turn_fail(t, 0);
            return;
        }
        if (rtc_stun_get_addr(m, rtc_stun_find(m, RTC_SA_XOR_MAPPED_ADDRESS), &t->mapped) == 0)
            t->ev(t->ud, t->idx, RTC_TURN_EV_MAPPED, 0);
        rtc_stun_get_u32(m, RTC_SA_LIFETIME, &lt);
        t->lifetime = lt;
        t->refresh_us = now + (uint64_t)(lt > 120 ? lt - 60 : lt / 2) * 1000000u;
        t->state = RTC_TURN_READY;
        t->ev(t->ud, t->idx, RTC_TURN_EV_RELAYED, 0);
        break;
    }
    case RTC_TX_REFRESH: {
        uint32_t lt = 600;
        rtc_stun_get_u32(m, RTC_SA_LIFETIME, &lt);
        t->lifetime = lt;
        t->refresh_us = now + (uint64_t)(lt > 120 ? lt - 60 : lt / 2 + 1) * 1000000u;
        break;
    }
    case RTC_TX_PERM:
        pm->perm_pending = 0;
        pm->perm_ok = 1;
        pm->perm_refresh_us = now + 240000000u;
        if (!pm->chan_ok && !pm->chan_pending) {
            pm->chan_pending = 1;
            if (!rtc_turn_tx_new(t, RTC_TX_CHANNEL, idx, now))
                pm->chan_pending = 0;
        }
        break;
    case RTC_TX_CHANNEL:
        pm->chan_pending = 0;
        pm->chan_ok = 1;
        pm->chan_refresh_us = now + 540000000u;
        break;
    default:
        break;
    }
}

int rtc_turn_input(rtc_turn* t, const uint8_t* p, size_t n, uint64_t now, rtc_addr* peer, const uint8_t** data,
    size_t* dlen)
{
    rtc_stun_msg m;
    int i;
    if (t->state == RTC_TURN_CLOSED)
        return 0;
    if (n >= 4 && p[0] >= 0x40 && p[0] <= 0x4F) {
        uint16_t ch = rtc_rd16(p), len = rtc_rd16(p + 2);
        if ((size_t)len + 4 > n)
            return 1;
        for (i = 0; i < RTC_TURN_MAX_PERM; i++) {
            if (t->perm[i].used && (t->perm[i].chan_ok || t->perm[i].chan_pending) && t->perm[i].chan == ch) {
                *peer = t->perm[i].peer;
                *data = p + 4;
                *dlen = len;
                return 2;
            }
        }
        return 0;
    }
    if (rtc_stun_parse(&m, p, n) < 0)
        return 0;
    if (m.fp >= 0 && !rtc_stun_check_fingerprint(&m))
        return 1;
    if (rtc_stun_class(m.type) == RTC_STUN_INDICATION && rtc_stun_method(m.type) == RTC_STUN_DATA) {
        const rtc_stun_attr* d = rtc_stun_find(&m, RTC_SA_DATA);
        if (!d || rtc_stun_get_addr(&m, rtc_stun_find(&m, RTC_SA_XOR_PEER_ADDRESS), peer) < 0)
            return 1;
        for (i = 0; i < RTC_TURN_MAX_PERM; i++)
            if (t->perm[i].used && t->perm[i].perm_ok && rtc_addr_eq(&t->perm[i].peer, peer))
                break;
        if (i == RTC_TURN_MAX_PERM)
            return 0;
        *data = p + d->off;
        *dlen = d->len;
        return 2;
    }
    for (i = 0; i < RTC_TURN_MAX_TX; i++) {
        rtc_turn_tx* x = &t->tx[i];
        if (x->kind != RTC_TX_NONE && !memcmp(x->tid, m.tid, 12)) {
            uint16_t c = rtc_stun_class(m.type);
            if (c == RTC_STUN_SUCCESS || c == RTC_STUN_ERROR)
                rtc_turn_on_response(t, x, &m, now);
            return 1;
        }
    }
    return 0;
}

static void rtc_turn_quarantine(rtc_turn* t, const rtc_turn_perm* pm)
{
    int i, k = 0;
    if (!pm->chan_hold_us)
        return;
    for (i = 1; i < RTC_TURN_CHQ; i++)
        if (t->chq[i].until_us < t->chq[k].until_us)
            k = i;
    t->chq[k].peer = pm->peer;
    t->chq[k].chan = pm->chan;
    t->chq[k].until_us = pm->chan_hold_us;
}

static int rtc_turn_chan_busy(const rtc_turn* t, uint16_t c, uint64_t now)
{
    int i;
    for (i = 0; i < RTC_TURN_MAX_PERM; i++)
        if (t->perm[i].used && t->perm[i].chan == c)
            return 1;
    for (i = 0; i < RTC_TURN_CHQ; i++)
        if (t->chq[i].until_us > now && t->chq[i].chan == c)
            return 1;
    return 0;
}

static int rtc_turn_chan_pick(rtc_turn* t, rtc_turn_perm* pm, uint64_t now)
{
    int i;
    for (i = 0; i < RTC_TURN_CHQ; i++) {
        rtc_turn_chq* q = &t->chq[i];
        if (q->until_us > now && rtc_addr_eq(&q->peer, &pm->peer)) {
            pm->chan = q->chan;
            pm->chan_hold_us = q->until_us;
            q->until_us = 0;
            return 0;
        }
    }
    for (i = 0; i < 0x1000; i++) {
        uint16_t c = (uint16_t)(0x4000u + (t->chan_next++ & 0xFFFu));
        if (!rtc_turn_chan_busy(t, c, now)) {
            pm->chan = c;
            return 0;
        }
    }
    return -1;
}

static int rtc_turn_perm_for(rtc_turn* t, const rtc_addr* peer, uint64_t now)
{
    int i, free_i = -1;
    for (i = 0; i < RTC_TURN_MAX_PERM; i++) {
        rtc_turn_perm* pm = &t->perm[i];
        if (pm->used && rtc_addr_eq(&pm->peer, peer)) {
            pm->last_use_us = now;
            if (!pm->perm_ok && !pm->perm_pending) {
                pm->perm_pending = 1;
                if (!rtc_turn_tx_new(t, RTC_TX_PERM, i, now))
                    pm->perm_pending = 0;
            }
            return i;
        }
        if (!t->perm[i].used && free_i < 0)
            free_i = i;
    }
    if (free_i < 0)
        return -1;
    memset(&t->perm[free_i], 0, sizeof(t->perm[free_i]));
    t->perm[free_i].peer = *peer;
    if (rtc_turn_chan_pick(t, &t->perm[free_i], now) < 0)
        return -1;
    t->perm[free_i].used = 1;
    t->perm[free_i].perm_pending = 1;
    t->perm[free_i].last_use_us = now;
    if (!rtc_turn_tx_new(t, RTC_TX_PERM, free_i, now)) {
        rtc_turn_quarantine(t, &t->perm[free_i]);
        t->perm[free_i].used = 0;
        return -1;
    }
    return free_i;
}

int rtc_turn_send(rtc_turn* t, const rtc_addr* peer, const uint8_t* p, size_t n, uint64_t now)
{
    uint8_t buf[1600];
    int i;
    if (t->state != RTC_TURN_READY || t->kind != RTC_TURN_KIND_TURN || n > 1500)
        return -1;
    i = rtc_turn_perm_for(t, peer, now);
    if (i < 0)
        return -1;
    if (t->perm[i].chan_ok) {
        rtc_wr16(buf, t->perm[i].chan);
        rtc_wr16(buf + 2, (uint32_t)n);
        memcpy(buf + 4, p, n);
        t->send(t->ud, t->sock, &t->server, buf, 4 + n);
        return 0;
    }
    {
        rtc_stun_wr w;
        uint8_t tid[12];
        size_t len;
        rtc_rng_bytes(t->rng, tid, 12);
        rtc_stun_wr_init(&w, buf, sizeof(buf), rtc_stun_type(RTC_STUN_SEND, RTC_STUN_INDICATION), tid);
        rtc_stun_wr_addr(&w, RTC_SA_XOR_PEER_ADDRESS, peer);
        rtc_stun_wr_attr(&w, RTC_SA_DATA, p, n);
        len = rtc_stun_wr_done(&w);
        if (len)
            t->send(t->ud, t->sock, &t->server, buf, len);
    }
    return 0;
}

uint64_t rtc_turn_tick(rtc_turn* t, uint64_t now)
{
    uint64_t next = UINT64_MAX;
    int i;
    if (t->state == RTC_TURN_FAILED || t->state == RTC_TURN_IDLE)
        return next;
    for (i = 0; i < RTC_TURN_MAX_TX; i++) {
        rtc_turn_tx* x = &t->tx[i];
        if (x->kind == RTC_TX_NONE)
            continue;
        if (now >= x->next_us) {
            if (x->tries >= 7) {
                int k = x->kind;
                x->kind = RTC_TX_NONE;
                if (k == RTC_TX_ALLOCATE || k == RTC_TX_BINDING)
                    rtc_turn_fail(t, 701);
                else if (k == RTC_TX_PERM)
                    t->perm[x->idx].perm_pending = 0;
                else if (k == RTC_TX_CHANNEL)
                    t->perm[x->idx].chan_pending = 0;
                continue;
            }
            x->tries++;
            x->rto_us *= 2;
            x->next_us = now + (x->tries >= 7 ? 16u * rtc_turn_rto0 : x->rto_us);
            rtc_turn_emit(t, x);
            if (x->kind == RTC_TX_CHANNEL)
                t->perm[x->idx].chan_hold_us = now + 900000000u;
        }
        if (x->next_us < next)
            next = x->next_us;
    }
    if (t->state != RTC_TURN_READY || t->kind != RTC_TURN_KIND_TURN)
        return next;
    if (now >= t->refresh_us) {
        rtc_turn_tx_new(t, RTC_TX_REFRESH, 0, now);
        t->refresh_us = now + 60000000u;
    }
    if (t->refresh_us < next)
        next = t->refresh_us;
    for (i = 0; i < RTC_TURN_MAX_PERM; i++) {
        rtc_turn_perm* pm = &t->perm[i];
        if (!pm->used)
            continue;
        if (!pm->perm_pending && !pm->chan_pending && now > pm->last_use_us && now - pm->last_use_us > 300000000u) {
            rtc_turn_quarantine(t, pm);
            memset(pm, 0, sizeof(*pm));
            continue;
        }
        if (pm->perm_ok && now >= pm->perm_refresh_us && !pm->perm_pending) {
            pm->perm_pending = 1;
            pm->perm_refresh_us = now + 240000000u;
            if (!rtc_turn_tx_new(t, RTC_TX_PERM, i, now))
                pm->perm_pending = 0;
        }
        if (pm->chan_ok && now >= pm->chan_refresh_us && !pm->chan_pending) {
            pm->chan_pending = 1;
            pm->chan_refresh_us = now + 540000000u;
            if (!rtc_turn_tx_new(t, RTC_TX_CHANNEL, i, now))
                pm->chan_pending = 0;
        }
        if (pm->perm_ok && pm->perm_refresh_us < next)
            next = pm->perm_refresh_us;
        if (pm->chan_ok && pm->chan_refresh_us < next)
            next = pm->chan_refresh_us;
    }
    return next;
}

void rtc_turn_reset_perms(rtc_turn* t)
{
    int i;
    for (i = 0; i < RTC_TURN_MAX_TX; i++)
        if (t->tx[i].kind == RTC_TX_PERM || t->tx[i].kind == RTC_TX_CHANNEL)
            t->tx[i].kind = RTC_TX_NONE;
    for (i = 0; i < RTC_TURN_MAX_PERM; i++)
        if (t->perm[i].used)
            rtc_turn_quarantine(t, &t->perm[i]);
    memset(t->perm, 0, sizeof(t->perm));
}

void rtc_turn_close(rtc_turn* t, uint64_t now)
{
    if (t->state == RTC_TURN_READY && t->kind == RTC_TURN_KIND_TURN) {
        int i;
        for (i = 0; i < RTC_TURN_MAX_TX; i++)
            t->tx[i].kind = RTC_TX_NONE;
        t->state = RTC_TURN_CLOSED;
        rtc_turn_tx_new(t, RTC_TX_REFRESH, 0, now);
        for (i = 0; i < RTC_TURN_MAX_TX; i++)
            t->tx[i].kind = RTC_TX_NONE;
    }
    t->state = RTC_TURN_CLOSED;
}
