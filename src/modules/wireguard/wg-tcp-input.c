#include "wg-tcp-int.h"

unsigned wgt_win_field(wg_tconn_t* c, int syn)
{
    uint32_t edge = c->rcv_user + WG_TCP_RCV_MAX, w, f;
    if (SLT(c->rcv_adv, edge) && edge - c->rcv_adv >= umin(WG_TCP_RCV_MAX / 2, c->mss))
        c->rcv_adv = edge;
    w = SLT(c->rcv_nxt, c->rcv_adv) ? c->rcv_adv - c->rcv_nxt : 0;
    if (syn) {
        f = umin(w, 65535);
        c->rcv_wedge = c->rcv_nxt + f;
        return f;
    }
    f = umin(w >> c->rcv_scale, 65535);
    c->rcv_wedge = SMAX(c->rcv_wedge, c->rcv_nxt + (f << c->rcv_scale));
    return f;
}

int wgt_send_new(wg_tconn_t* c, uint32_t wnd)
{
    wg_tcp_t* t = c->t;
    uint32_t avail = unsent(c), data = SLT(c->snd_nxt, c->snd_end) ? c->snd_end - c->snd_nxt : 0, usable, len, maxd;
    int n;
    if (!avail)
        return 0;
    maxd = c->mss - (c->sack_ok && c->ooo ? 36u : 0u);
    usable = wnd > flight(c) ? wnd - flight(c) : 0;
    if (data) {
        len = umin(umin(data, maxd), usable);
        if (!len)
            return 0;
        if (len < maxd && len < data && len < c->max_sndwnd / 2)
            return 0;
        if (len < maxd && len == data && !c->nodelay && flight(c) && !c->fin_req)
            return 0;
    } else
        len = 1;
    if (!c->rtt_ts) {
        c->rtt_ts = now_ms(t);
        c->rtt_seq = c->snd_nxt;
    }
    n = wgt_xmit(c, c->snd_nxt, len, F_ACK);
    if (n <= 0)
        return 0;
    c->snd_nxt += (uint32_t)n;
    wgt_arm_rto(c, 0);
    return 1;
}

static void deliver(wg_tconn_t* c, const uint8_t* d, uint32_t n)
{
    wg_tseg_t* s;
    if (!n)
        return;
    if (!c->paused && !c->pq && c->cb && c->cb->on_data) {
        c->rcv_user += n;
        c->cb->on_data(c->ud, c, d, n);
        return;
    }
    s = (wg_tseg_t*)malloc(sizeof *s + n);
    if (!s)
        return;
    s->next = NULL;
    s->seq = 0;
    s->len = n;
    memcpy(s->d, d, n);
    if (c->pq_tail)
        c->pq_tail->next = s;
    else
        c->pq = s;
    c->pq_tail = s;
}

static void eof_check(wg_tconn_t* c)
{
    if (c->eof_due && !c->eof_done && !c->pq && !c->paused && !c->dead) {
        c->eof_done = 1;
        if (c->cb && c->cb->on_eof)
            c->cb->on_eof(c->ud, c);
    }
}

static void fin_in(wg_tconn_t* c)
{
    c->rcv_nxt++;
    c->eof_due = 1;
    wgt_send_ack(c);
    if (c->state == WG_TCP_ESTABLISHED || c->state == WG_TCP_SYN_RCVD)
        c->state = WG_TCP_CLOSE_WAIT;
    else if (c->state == WG_TCP_FIN_WAIT_1)
        c->state = WG_TCP_CLOSING;
    eof_check(c);
    if (!c->dead && c->state == WG_TCP_FIN_WAIT_2)
        wgt_time_wait(c);
}

static void data_in(wg_tconn_t* c, const seg_t* g, int fin)
{
    wg_tcp_t* t = c->t;
    uint64_t now = now_ms(t);
    int had = c->ooo != NULL;
    if (g->dlen && g->seq != c->rcv_nxt) {
        wgt_ooo_add(c, g->seq, g->d, g->dlen);
        if (fin) {
            c->has_ooo_fin = 1;
            c->ooo_fin = g->seq + g->dlen;
        }
        wgt_send_ack(c);
        return;
    }
    if (!g->dlen && fin && g->seq != c->rcv_nxt) {
        c->has_ooo_fin = 1;
        c->ooo_fin = g->seq;
        wgt_send_ack(c);
        return;
    }
    if (g->dlen) {
        c->rcv_nxt += g->dlen;
        deliver(c, g->d, g->dlen);
        while (!c->dead && c->ooo && SLE(c->ooo->seq, c->rcv_nxt)) {
            wg_tseg_t* s = c->ooo;
            c->ooo = s->next;
            c->nooo--;
            if (SLT(c->rcv_nxt, s->seq + s->len)) {
                uint32_t skip = c->rcv_nxt - s->seq;
                c->rcv_nxt = s->seq + s->len;
                deliver(c, s->d + skip, s->len - skip);
            }
            free(s);
        }
        if (c->dead)
            return;
        if (c->has_ooo_fin && c->ooo_fin == c->rcv_nxt) {
            c->has_ooo_fin = 0;
            fin = 1;
        }
        c->unacked += g->dlen;
        if (had || c->unacked >= 2 * c->mss)
            wgt_send_ack(c);
        else if (!c->dack_at)
            c->dack_at = now + WG_TCP_DACK_MS;
    }
    if (fin)
        fin_in(c);
}

static void challenge(wg_tconn_t* c)
{
    c->t->st.challenge_acks++;
    wgt_send_ack(c);
}

void wgt_synced_in(wg_tconn_t* c, seg_t* g)
{
    uint32_t rwnd = c->rcv_wedge - c->rcv_nxt, slen = g->dlen + ((g->fl & F_SYN) ? 1u : 0u) + ((g->fl & F_FIN) ? 1u : 0u), seq0 = g->seq;
    int ok, zw = 0, fin = (g->fl & F_FIN) != 0;
    if (c->state == WG_TCP_SYN_RCVD && (g->fl & F_SYN) && !(g->fl & F_ACK) && g->seq == c->irs) {
        wgt_xmit(c, c->iss, 0, F_SYN | F_ACK);
        return;
    }
    if (!slen)
        ok = rwnd ? SLE(c->rcv_nxt, g->seq) && SLT(g->seq, c->rcv_nxt + rwnd) : g->seq == c->rcv_nxt;
    else
        ok = rwnd && ((SLE(c->rcv_nxt, g->seq) && SLT(g->seq, c->rcv_nxt + rwnd)) || (SLE(c->rcv_nxt, g->seq + slen - 1) && SLT(g->seq + slen - 1, c->rcv_nxt + rwnd)));
    if (!ok && !rwnd && g->seq == c->rcv_nxt) {
        ok = 1;
        zw = slen > 0;
        g->dlen = 0;
        fin = 0;
    }
    if (!ok) {
        if (!(g->fl & F_RST)) {
            if (c->state == WG_TCP_TIME_WAIT && fin)
                c->tw_at = now_ms(c->t) + 2 * WG_TCP_MSL_MS;
            wgt_send_ack(c);
        }
        return;
    }
    if (g->fl & F_RST) {
        c->t->st.rst_rcvd++;
        if (seq0 != c->rcv_nxt)
            challenge(c);
        else if (c->ls)
            wgt_kill(c);
        else
            wgt_close_err(c, c->state >= WG_TCP_CLOSING ? 0 : -ECONNRESET);
        return;
    }
    if (g->fl & F_SYN) {
        challenge(c);
        return;
    }
    if (SLT(g->seq, c->rcv_nxt)) {
        uint32_t cut = c->rcv_nxt - g->seq;
        if (cut >= g->dlen) {
            if (cut > g->dlen)
                fin = 0;
            g->dlen = 0;
        } else {
            g->d += cut;
            g->dlen -= cut;
        }
        g->seq = c->rcv_nxt;
    }
    if (SLT(c->rcv_wedge, g->seq + g->dlen)) {
        g->dlen = SLT(g->seq, c->rcv_wedge) ? c->rcv_wedge - g->seq : 0;
        fin = 0;
    }
    if (!(g->fl & F_ACK))
        return;
    if (c->state == WG_TCP_SYN_RCVD) {
        if (!(SLT(c->snd_una, g->ack) && SLE(g->ack, c->snd_nxt))) {
            wgt_out_seg(c->t, &c->la, &c->ra, c->lp, c->rp, g->ack, 0, F_RST, 0, NULL, 0, NULL, 0, NULL, 0);
            return;
        }
        c->snd_una = g->ack;
        c->snd_wnd = g->win << c->snd_scale;
        c->max_sndwnd = c->snd_wnd;
        c->snd_wl1 = g->seq;
        c->snd_wl2 = g->ack;
        if (c->rtt_ts && !c->syn_rexmit)
            wgt_rtt_sample(c, (uint32_t)(now_ms(c->t) - c->rtt_ts));
        c->rtt_ts = 0;
        wgt_established(c);
        if (c->dead)
            return;
    } else {
        if (SLT(c->snd_nxt, g->ack)) {
            wgt_send_ack(c);
            return;
        }
        if (SLT(g->ack, c->snd_una - c->max_sndwnd)) {
            challenge(c);
            return;
        }
        wgt_ack_in(c, g);
        if (c->dead)
            return;
    }
    if (c->state == WG_TCP_TIME_WAIT) {
        if (fin) {
            c->tw_at = now_ms(c->t) + 2 * WG_TCP_MSL_MS;
            wgt_send_ack(c);
        }
        return;
    }
    if (c->state == WG_TCP_ESTABLISHED || c->state == WG_TCP_FIN_WAIT_1 || c->state == WG_TCP_FIN_WAIT_2)
        data_in(c, g, fin);
    else if (fin && g->seq == c->rcv_nxt - 1)
        wgt_send_ack(c);
    if (zw && !c->dead)
        wgt_send_ack(c);
}

int wgt_parse(seg_t* g, const uint8_t* l4, size_t n)
{
    size_t hl = (size_t)(l4[12] >> 4) * 4, i = 20;
    memset(g, 0, sizeof *g);
    if (hl < 20 || hl > n)
        return -1;
    g->seq = be32(l4 + 4);
    g->ack = be32(l4 + 8);
    g->fl = l4[13] & 0x3f;
    g->win = be16(l4 + 14);
    g->d = l4 + hl;
    g->dlen = (uint32_t)(n - hl);
    while (i < hl) {
        uint8_t k = l4[i];
        size_t ol;
        if (k == 0)
            break;
        if (k == 1) {
            i++;
            continue;
        }
        if (i + 2 > hl || (ol = l4[i + 1]) < 2 || i + ol > hl)
            break;
        if ((g->fl & F_SYN) && k == 2 && ol == 4)
            g->mss = be16(l4 + i + 2);
        else if ((g->fl & F_SYN) && k == 3 && ol == 3)
            g->ws = 1u + (l4[i + 2] > 14 ? 14u : l4[i + 2]);
        else if ((g->fl & F_SYN) && k == 4 && ol == 2)
            g->sackp = 1;
        else if (!(g->fl & F_SYN) && k == 5 && ol >= 10 && (ol - 2) % 8 == 0) {
            size_t j;
            for (j = 0; j < (ol - 2) / 8 && g->nsk < 4; j++, g->nsk++) {
                g->sk[g->nsk][0] = be32(l4 + i + 2 + 8 * j);
                g->sk[g->nsk][1] = be32(l4 + i + 6 + 8 * j);
            }
        }
        i += ol;
    }
    return 0;
}

void wg_tcp_pause(wg_tconn_t* c, int on)
{
    wg_tcp_t* t = c->t;
    uint32_t before;
    if (c->dead)
        return;
    c->paused = (uint8_t)!!on;
    if (on)
        return;
    t->depth++;
    before = c->rcv_wedge;
    while (!c->dead && !c->paused && c->pq) {
        wg_tseg_t* s = c->pq;
        c->pq = s->next;
        if (!c->pq)
            c->pq_tail = NULL;
        c->rcv_user += s->len;
        if (c->cb && c->cb->on_data)
            c->cb->on_data(c->ud, c, s->d, s->len);
        free(s);
    }
    eof_check(c);
    if (!c->dead && c->state >= WG_TCP_ESTABLISHED && c->state != WG_TCP_TIME_WAIT) {
        uint32_t edge = c->rcv_user + WG_TCP_RCV_MAX;
        if (SLT(c->rcv_adv, edge) && edge - c->rcv_adv >= umin(WG_TCP_RCV_MAX / 2, c->mss) && SLT(before, edge))
            wgt_send_ack(c);
        wgt_sched(c);
    }
    t->depth--;
    wgt_reap(t);
}
