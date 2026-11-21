#include "wg-tcp-int.h"

size_t wgt_sack_opts(wg_tconn_t* c, uint8_t* o)
{
    uint32_t r[WG_TCP_OOO_MAX][2];
    unsigned n = 0, i, first = 0, k = 0;
    wg_tseg_t* s;
    for (s = c->ooo; s; s = s->next) {
        if (n && SLE(s->seq, r[n - 1][1])) {
            r[n - 1][1] = SMAX(r[n - 1][1], s->seq + s->len);
            continue;
        }
        r[n][0] = s->seq;
        r[n][1] = s->seq + s->len;
        n++;
    }
    if (!n)
        return 0;
    for (i = 0; i < n; i++)
        if (SLE(r[i][0], c->last_ooo) && SLT(c->last_ooo, r[i][1]))
            first = i;
    o[0] = o[1] = 1;
    o[2] = 5;
    put32(o + 4, r[first][0]);
    put32(o + 8, r[first][1]);
    k = 1;
    for (i = n; i-- > 0 && k < 4;)
        if (i != first) {
            put32(o + 4 + 8 * k, r[i][0]);
            put32(o + 8 + 8 * k, r[i][1]);
            k++;
        }
    o[3] = (uint8_t)(2 + 8 * k);
    return 4 + 8 * k;
}

static void sacked_above(const wg_tconn_t* c, uint32_t s, uint32_t* bytes, unsigned* cnt)
{
    unsigned i;
    *bytes = 0;
    *cnt = 0;
    for (i = 0; i < c->nsk; i++)
        if (SLE(s, c->sk[i][0])) {
            *bytes += c->sk[i][1] - c->sk[i][0];
            (*cnt)++;
        }
}

static int is_lost(const wg_tconn_t* c, uint32_t s)
{
    uint32_t b;
    unsigned n;
    if (c->rto_rec)
        return 1;
    sacked_above(c, s, &b, &n);
    return n >= WG_TCP_DUPTHRESH || b > (WG_TCP_DUPTHRESH - 1) * c->mss;
}

static uint32_t set_pipe(const wg_tconn_t* c)
{
    uint32_t pipe = 0, pos = c->snd_una, a, b;
    unsigned i;
    for (i = 0; i <= c->nsk; i++) {
        a = pos;
        b = i < c->nsk ? c->sk[i][0] : c->snd_nxt;
        if (SLT(a, b)) {
            if (!is_lost(c, a))
                pipe += b - a;
            if (SLT(a, c->high_rxt))
                pipe += umin(b, c->high_rxt) - a;
        }
        if (i < c->nsk)
            pos = SMAX(pos, c->sk[i][1]);
    }
    return pipe;
}

static int next_hole(const wg_tconn_t* c, int rule, uint32_t* s, uint32_t* len)
{
    uint32_t pos = c->snd_una, a, b, sb;
    unsigned i, n;
    for (i = 0; i <= c->nsk; i++) {
        a = SMAX(pos, c->high_rxt);
        b = i < c->nsk ? c->sk[i][0] : c->snd_nxt;
        if (SLT(a, b)) {
            sacked_above(c, a, &sb, &n);
            if (rule == 1 ? is_lost(c, a) : n > 0) {
                *s = a;
                *len = umin(b - a, c->mss);
                return 1;
            }
        }
        if (i < c->nsk)
            pos = SMAX(pos, c->sk[i][1]);
    }
    return 0;
}

void wgt_rexmit(wg_tconn_t* c, uint32_t s, uint32_t len)
{
    wg_tcp_t* t = c->t;
    int n = wgt_xmit(c, s, len, F_ACK);
    t->st.rexmit_segs++;
    t->st.rexmit_bytes += (uint64_t)n;
    c->high_rxt = SMAX(c->high_rxt, s + (uint32_t)n);
    if (c->rtt_ts && SLE(s, c->rtt_seq) && SLT(c->rtt_seq, s + (uint32_t)n))
        c->rtt_ts = 0;
    wgt_arm_rto(c, 0);
}

uint32_t wgt_init_win(const wg_tconn_t* c)
{
    return umin(10 * c->mss, SMAX(2 * c->mss, 14600u));
}

void wgt_output(wg_tconn_t* c)
{
    wg_tcp_t* t = c->t;
    uint64_t now = now_ms(t);
    uint32_t s, len;
    int k;
    if (c->dead || c->state < WG_TCP_ESTABLISHED || c->state == WG_TCP_TIME_WAIT)
        return;
    if (!flight(c) && c->last_send && now - c->last_send > c->rto && c->cwnd > wgt_init_win(c))
        c->cwnd = wgt_init_win(c);
    if (c->in_rec && (c->sack_ok || c->rto_rec)) {
        for (k = 0; k < 4096; k++) {
            uint32_t pipe = set_pipe(c);
            if (pipe >= c->cwnd || c->cwnd - pipe < umin(c->mss, unsent(c) ? c->mss : 1))
                break;
            if (next_hole(c, 1, &s, &len)) {
                t->st.sack_rexmit++;
                wgt_rexmit(c, s, len);
            } else if (!wgt_send_new(c, c->snd_wnd)) {
                if (!c->rto_rec && next_hole(c, 3, &s, &len)) {
                    t->st.sack_rexmit++;
                    wgt_rexmit(c, s, len);
                } else
                    break;
            }
        }
    } else
        for (k = 0; k < 4096 && wgt_send_new(c, umin(c->cwnd, c->snd_wnd)); k++) {
        }
    if (unsent(c) && !flight(c) && !c->rto_at)
        wgt_arm_rto(c, 1);
}

static void enter_recovery(wg_tconn_t* c)
{
    wg_tcp_t* t = c->t;
    uint32_t len = umin(c->mss, flight(c));
    t->st.fast_rexmit++;
    c->in_rec = 1;
    c->recover = c->snd_nxt;
    c->ssthresh = SMAX(flight(c) / 2, 2 * c->mss);
    c->high_rxt = c->snd_una;
    if (c->nsk && SLT(c->sk[0][0], c->snd_una + len))
        len = c->sk[0][0] - c->snd_una;
    wgt_rexmit(c, c->snd_una, len);
    c->cwnd = c->sack_ok ? c->ssthresh : c->ssthresh + WG_TCP_DUPTHRESH * c->mss;
}

static void sb_add(wg_tconn_t* c, uint32_t s, uint32_t e)
{
    unsigned i, j;
    if (!SLT(c->snd_una, s) || !SLT(s, e) || SLT(c->snd_nxt, e))
        return;
    for (i = 0; i < c->nsk && SLT(c->sk[i][1], s); i++) {
    }
    if (i < c->nsk && SLE(c->sk[i][0], e)) {
        c->sk[i][0] = SLT(s, c->sk[i][0]) ? s : c->sk[i][0];
        c->sk[i][1] = SMAX(e, c->sk[i][1]);
        while (i + 1 < c->nsk && SLE(c->sk[i + 1][0], c->sk[i][1])) {
            c->sk[i][1] = SMAX(c->sk[i][1], c->sk[i + 1][1]);
            for (j = i + 1; j + 1 < c->nsk; j++) {
                c->sk[j][0] = c->sk[j + 1][0];
                c->sk[j][1] = c->sk[j + 1][1];
            }
            c->nsk--;
        }
        return;
    }
    if (c->nsk == WG_TCP_SB_MAX)
        return;
    for (j = c->nsk; j > i; j--) {
        c->sk[j][0] = c->sk[j - 1][0];
        c->sk[j][1] = c->sk[j - 1][1];
    }
    c->sk[i][0] = s;
    c->sk[i][1] = e;
    c->nsk++;
}

static void sb_cut(wg_tconn_t* c)
{
    unsigned i = 0, j;
    while (i < c->nsk && SLE(c->sk[i][1], c->snd_una))
        i++;
    for (j = 0; i + j < c->nsk; j++) {
        c->sk[j][0] = c->sk[i + j][0];
        c->sk[j][1] = c->sk[i + j][1];
    }
    c->nsk -= i;
    if (c->nsk && SLT(c->sk[0][0], c->snd_una))
        c->sk[0][0] = c->snd_una;
}

static void snd_drop(wg_tconn_t* c, uint32_t ack)
{
    uint32_t to = SLT(c->snd_end, ack) ? c->snd_end : ack, n;
    if (!SLT(c->sbase, to))
        return;
    n = to - c->sbase;
    c->sbase = to;
    c->sb_head = (c->sb_head + n) & (c->sb_cap ? c->sb_cap - 1 : 0);
    if (c->sbase == c->snd_end && c->sb) {
        free(c->sb);
        c->sb = NULL;
        c->sb_cap = c->sb_head = 0;
    }
}

void wgt_ooo_add(wg_tconn_t* c, uint32_t seq, const uint8_t* d, uint32_t n)
{
    wg_tseg_t **p = &c->ooo, *s;
    c->t->st.ooo_segs++;
    c->last_ooo = seq;
    while (*p && SLT((*p)->seq, seq))
        p = &(*p)->next;
    if (*p && (*p)->seq == seq && (*p)->len >= n)
        return;
    if (c->nooo >= WG_TCP_OOO_MAX || !(s = (wg_tseg_t*)malloc(sizeof *s + n))) {
        c->t->st.ooo_drop++;
        return;
    }
    s->seq = seq;
    s->len = n;
    memcpy(s->d, d, n);
    s->next = *p;
    *p = s;
    c->nooo++;
}

void wgt_ack_in(wg_tconn_t* c, const seg_t* g)
{
    wg_tcp_t* t = c->t;
    uint64_t now = now_ms(t);
    uint32_t ack = g->ack, acked;
    unsigned i;
    int dup = 0;
    if (c->sack_ok)
        for (i = 0; i < g->nsk; i++)
            if (SLE(ack, g->sk[i][0]))
                sb_add(c, g->sk[i][0], g->sk[i][1]);
    if (ack == c->snd_una && flight(c) && !g->dlen && !(g->fl & (F_SYN | F_FIN)) && (g->win << c->snd_scale) == c->snd_wnd)
        dup = 1;
    if (SLT(c->snd_wl1, g->seq) || (c->snd_wl1 == g->seq && SLE(c->snd_wl2, ack))) {
        c->snd_wnd = g->win << c->snd_scale;
        c->snd_wl1 = g->seq;
        c->snd_wl2 = ack;
        if (c->snd_wnd > c->max_sndwnd)
            c->max_sndwnd = c->snd_wnd;
    }
    if (!c->snd_wnd)
        c->rexmit_start = 0;
    if (SLT(c->snd_una, ack)) {
        acked = ack - c->snd_una;
        if (c->rtt_ts && SLT(c->rtt_seq, ack)) {
            wgt_rtt_sample(c, (uint32_t)(now - c->rtt_ts));
            c->rtt_ts = 0;
        }
        c->snd_una = ack;
        if (SLT(c->high_rxt, ack))
            c->high_rxt = ack;
        snd_drop(c, ack);
        sb_cut(c);
        c->rexmit_start = 0;
        c->dupacks = 0;
        if (c->in_rec) {
            if (SLE(c->recover, ack)) {
                c->in_rec = c->rto_rec = 0;
                if (!c->sack_ok)
                    c->cwnd = umin(c->ssthresh, SMAX(flight(c), c->mss) + c->mss);
                else
                    c->cwnd = c->ssthresh;
            } else if (c->rto_rec) {
                c->cwnd += umin(acked, c->mss);
            } else if (!c->sack_ok) {
                c->high_rxt = c->snd_una;
                wgt_rexmit(c, c->snd_una, umin(c->mss, flight(c)));
                c->cwnd = c->cwnd > acked ? c->cwnd - acked : 0;
                if (acked >= c->mss)
                    c->cwnd += c->mss;
            }
        } else if (c->cwnd < c->ssthresh)
            c->cwnd += umin(acked, c->mss);
        else {
            c->cacc += acked;
            if (c->cacc >= c->cwnd) {
                c->cacc -= c->cwnd;
                c->cwnd += c->mss;
            }
        }
        c->rto_at = 0;
        if (flight(c))
            wgt_arm_rto(c, 1);
        if (c->blocked && c->snd_end - c->sbase < WG_TCP_SND_MAX / 2) {
            c->blocked = 0;
            if (c->cb && c->cb->on_writable)
                c->cb->on_writable(c->ud, c);
            if (c->dead)
                return;
        }
    } else if (dup) {
        c->dupacks++;
        if (!c->in_rec && (c->dupacks == WG_TCP_DUPTHRESH || (c->sack_ok && is_lost(c, c->snd_una))))
            enter_recovery(c);
        else if (c->in_rec && !c->sack_ok && !c->rto_rec)
            c->cwnd += c->mss;
    } else if (c->sack_ok && !c->in_rec && flight(c) && ack == c->snd_una && is_lost(c, c->snd_una))
        enter_recovery(c);
    if (c->fin_req && SLT(c->snd_end, ack)) {
        if (c->state == WG_TCP_FIN_WAIT_1)
            c->state = WG_TCP_FIN_WAIT_2;
        else if (c->state == WG_TCP_CLOSING)
            wgt_time_wait(c);
        else if (c->state == WG_TCP_LAST_ACK)
            wgt_close_err(c, 0);
    }
}
