#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_WIREGUARD)

#include "wg-internal.h"

#include <string.h>

static uint64_t at(uint64_t now_ms, uint64_t d)
{
    return now_ms + d;
}

void wg_timers_start(wg_timers_t* t, uint64_t now_ms)
{
    uint16_t ps = t->persist_s;
    (void)now_ms;
    memset(t, 0, sizeof *t);
    t->persist_s = ps;
}

int wg_timers_hs_allowed(const wg_timers_t* t, uint64_t now_ms)
{
    return !t->has_hs_sent || now_ms - t->last_hs_sent_ms >= WG_REKEY_TIMEOUT_MS;
}

void wg_timers_hs_initiated(wg_timers_t* t, uint64_t now_ms, unsigned jitter_ms, int retry)
{
    if (!retry)
        t->attempts = 0;
    t->last_hs_sent_ms = now_ms;
    t->has_hs_sent = 1;
    t->retransmit_at = at(now_ms, WG_REKEY_TIMEOUT_MS + jitter_ms % WG_REKEY_JITTER_MS);
}

void wg_timers_hs_complete(wg_timers_t* t, uint64_t now_ms)
{
    t->retransmit_at = 0;
    t->attempts = 0;
    t->last_minute = 0;
    t->last_hs_done_ms = now_ms;
}

void wg_timers_session_derived(wg_timers_t* t, uint64_t now_ms)
{
    t->zero_at = at(now_ms, WG_ZERO_KEYS_MS);
}

void wg_timers_data_sent(wg_timers_t* t, uint64_t now_ms, unsigned jitter_ms)
{
    if (!t->new_hs_at)
        t->new_hs_at = at(now_ms, WG_KEEPALIVE_MS + WG_REKEY_TIMEOUT_MS + jitter_ms % WG_REKEY_JITTER_MS);
}

void wg_timers_data_recv(wg_timers_t* t, uint64_t now_ms)
{
    if (!t->keepalive_at)
        t->keepalive_at = at(now_ms, WG_KEEPALIVE_MS);
    else
        t->need_keepalive = 1;
}

void wg_timers_auth_recv(wg_timers_t* t)
{
    t->new_hs_at = 0;
}

void wg_timers_auth_sent(wg_timers_t* t)
{
    t->keepalive_at = 0;
}

void wg_timers_traversal(wg_timers_t* t, uint64_t now_ms)
{
    if (t->persist_s)
        t->persist_at = at(now_ms, (uint64_t)t->persist_s * 1000u);
}

unsigned wg_timers_due(wg_timers_t* t, uint64_t now_ms)
{
    unsigned act = 0;
    if (t->retransmit_at && t->retransmit_at <= now_ms) {
        t->retransmit_at = 0;
        if (t->attempts > WG_MAX_TIMER_HANDSHAKES) {
            act |= WG_ACT_GIVE_UP;
            t->keepalive_at = 0;
            if (!t->zero_at)
                t->zero_at = at(now_ms, WG_ZERO_KEYS_MS);
        } else {
            t->attempts++;
            act |= WG_ACT_SEND_INIT | WG_ACT_RETRY;
        }
    }
    if (t->keepalive_at && t->keepalive_at <= now_ms) {
        t->keepalive_at = 0;
        act |= WG_ACT_SEND_KEEPALIVE;
        if (t->need_keepalive) {
            t->need_keepalive = 0;
            t->keepalive_at = at(now_ms, WG_KEEPALIVE_MS);
        }
    }
    if (t->new_hs_at && t->new_hs_at <= now_ms) {
        t->new_hs_at = 0;
        act |= WG_ACT_SEND_INIT;
    }
    if (t->zero_at && t->zero_at <= now_ms) {
        t->zero_at = 0;
        act |= WG_ACT_ZERO_KEYS;
    }
    if (t->persist_at && t->persist_at <= now_ms) {
        t->persist_at = 0;
        if (t->persist_s)
            act |= WG_ACT_SEND_KEEPALIVE;
    }
    return act;
}

uint64_t wg_timers_next(const wg_timers_t* t)
{
    uint64_t d[5] = { t->retransmit_at, t->keepalive_at, t->new_hs_at, t->zero_at, t->persist_at }, m = 0;
    unsigned i;
    for (i = 0; i < 5; i++)
        if (d[i] && (!m || d[i] < m))
            m = d[i];
    return m;
}

#endif
