#include "wg-tcp-int.h"

static void hswap(wg_tcp_t* t, unsigned a, unsigned b)
{
    wg_tconn_t* x = t->heap[a];
    t->heap[a] = t->heap[b];
    t->heap[b] = x;
    t->heap[a]->hpos = a;
    t->heap[b]->hpos = b;
}

static void hfix(wg_tcp_t* t, unsigned i)
{
    while (i > 1 && t->heap[i]->due < t->heap[i / 2]->due) {
        hswap(t, i, i / 2);
        i /= 2;
    }
    for (;;) {
        unsigned l = 2 * i, m = i;
        if (l <= t->nheap && t->heap[l]->due < t->heap[m]->due)
            m = l;
        if (l + 1 <= t->nheap && t->heap[l + 1]->due < t->heap[m]->due)
            m = l + 1;
        if (m == i)
            break;
        hswap(t, i, m);
        i = m;
    }
}

void wgt_hdel(wg_tcp_t* t, wg_tconn_t* c)
{
    unsigned i = c->hpos;
    if (!i)
        return;
    hswap(t, i, t->nheap);
    t->nheap--;
    c->hpos = 0;
    if (i <= t->nheap)
        hfix(t, i);
}

void wgt_sched(wg_tconn_t* c)
{
    wg_tcp_t* t = c->t;
    uint64_t d = 0, v[4];
    unsigned i;
    v[0] = c->rto_at;
    v[1] = c->dack_at;
    v[2] = c->tw_at;
    v[3] = c->ka_at;
    for (i = 0; i < 4; i++)
        if (v[i] && (!d || v[i] < d))
            d = v[i];
    c->due = d;
    if (!d || c->dead) {
        wgt_hdel(t, c);
        return;
    }
    if (!c->hpos) {
        if (t->nheap + 1 >= t->cheap) {
            unsigned n = t->cheap ? t->cheap * 2 : 64;
            wg_tconn_t** h = (wg_tconn_t**)realloc(t->heap, n * sizeof *h);
            if (!h)
                return;
            t->heap = h;
            t->cheap = n;
        }
        t->heap[++t->nheap] = c;
        c->hpos = t->nheap;
    }
    hfix(t, c->hpos);
}

void wgt_arm_rto(wg_tconn_t* c, int restart)
{
    if (restart || !c->rto_at)
        c->rto_at = now_ms(c->t) + c->rto;
}

void wgt_rtt_sample(wg_tconn_t* c, uint32_t r)
{
    if (!c->sampled) {
        c->srtt = r << 3;
        c->rttvar = r << 1;
        c->sampled = 1;
    } else {
        uint32_t s = c->srtt >> 3, d = s > r ? s - r : r - s;
        c->rttvar = c->rttvar - (c->rttvar >> 2) + d;
        c->srtt = c->srtt - (c->srtt >> 3) + r;
    }
    c->rto = (c->srtt >> 3) + (c->rttvar > 1 ? c->rttvar : 1);
    if (c->rto < WG_TCP_RTO_MIN_MS)
        c->rto = WG_TCP_RTO_MIN_MS;
    if (c->rto > WG_TCP_RTO_MAX_MS)
        c->rto = WG_TCP_RTO_MAX_MS;
}

static void timer(wg_tconn_t* c, uint64_t now)
{
    wg_tcp_t* t = c->t;
    if (c->tw_at && c->tw_at <= now) {
        wgt_kill(c);
        return;
    }
    if (c->dack_at && c->dack_at <= now) {
        t->st.dack_sent++;
        wgt_xmit(c, c->snd_nxt, 0, F_ACK);
    }
    if (c->ka_at && c->ka_at <= now) {
        if (c->state != WG_TCP_ESTABLISHED || flight(c) || unsent(c))
            c->ka_at = now + c->ka_idle;
        else if (++c->ka_probes > WG_TCP_KA_PROBES) {
            wgt_out_seg(t, &c->la, &c->ra, c->lp, c->rp, c->snd_nxt, 0, F_RST, 0, NULL, 0, NULL, 0, NULL, 0);
            wgt_close_err(c, -ETIMEDOUT);
            return;
        } else {
            t->st.ka_probes++;
            wgt_out_seg(t, &c->la, &c->ra, c->lp, c->rp, c->snd_una - 1, c->rcv_nxt, F_ACK, wgt_win_field(c, 0), NULL, 0, NULL, 0, NULL, 0);
            c->ka_at = now + WG_TCP_KA_INTVL_MS;
        }
    }
    if (!c->rto_at || c->rto_at > now)
        return;
    c->rto_at = 0;
    if (!flight(c) && c->state >= WG_TCP_ESTABLISHED) {
        if (unsent(c)) {
            uint32_t s = c->snd_nxt;
            int k;
            if (!c->snd_wnd)
                t->st.persist_probes++;
            k = wgt_xmit(c, s, c->snd_wnd ? umin(c->snd_wnd, c->mss) : 1, F_ACK);
            c->snd_nxt += (uint32_t)k;
            c->rto = umin(c->rto * 2, WG_TCP_RTO_MAX_MS);
            wgt_arm_rto(c, 1);
        }
        return;
    }
    if (!c->rexmit_start)
        c->rexmit_start = now;
    if (now - c->rexmit_start >= (c->state < WG_TCP_ESTABLISHED ? WG_TCP_R2_SYN_MS : WG_TCP_R2_MS)) {
        if (c->state >= WG_TCP_SYN_RCVD)
            wgt_out_seg(t, &c->la, &c->ra, c->lp, c->rp, c->snd_nxt, 0, F_RST, 0, NULL, 0, NULL, 0, NULL, 0);
        if (c->ls)
            wgt_kill(c);
        else
            wgt_close_err(c, -ETIMEDOUT);
        return;
    }
    t->st.rto_fired++;
    c->rtt_ts = 0;
    c->rto = umin(c->rto * 2, WG_TCP_RTO_MAX_MS);
    if (c->state < WG_TCP_ESTABLISHED) {
        c->syn_rexmit = 1;
        t->st.rexmit_segs++;
        wgt_xmit(c, c->iss, 0, c->state == WG_TCP_SYN_SENT ? F_SYN : F_SYN | F_ACK);
        wgt_arm_rto(c, 1);
        return;
    }
    if (c->snd_wnd)
        c->ssthresh = c->rto_rec ? c->ssthresh : SMAX(flight(c) / 2, 2 * c->mss);
    else
        t->st.persist_probes++;
    c->cwnd = c->mss;
    c->in_rec = c->rto_rec = 1;
    c->recover = c->snd_nxt;
    c->high_rxt = c->snd_una;
    c->nsk = 0;
    c->dupacks = 0;
    wgt_rexmit(c, c->snd_una, umin(c->mss, flight(c)));
    wgt_arm_rto(c, 1);
}

void wg_tcp_set_keepalive(wg_tconn_t* c, uint64_t idle_ms)
{
    c->ka_idle = idle_ms;
    c->ka_probes = 0;
    c->ka_at = idle_ms && c->state == WG_TCP_ESTABLISHED ? now_ms(c->t) + idle_ms : 0;
    wgt_sched(c);
}

void wg_tcp_tick(wg_tcp_t* t)
{
    uint64_t now = now_ms(t);
    t->depth++;
    while (t->nheap && t->heap[1]->due <= now) {
        wg_tconn_t* c = t->heap[1];
        timer(c, now);
        if (!c->dead) {
            wgt_output(c);
            wgt_sched(c);
        }
    }
    t->depth--;
    wgt_reap(t);
}

uint64_t wg_tcp_next_deadline(const wg_tcp_t* t)
{
    return t->nheap ? t->heap[1]->due : 0;
}
