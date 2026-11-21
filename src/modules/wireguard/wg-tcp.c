#include "wg-tcp-int.h"

static uint32_t def_isn(void* ud, const wg_ipaddr_t* la, uint16_t lp, const wg_ipaddr_t* ra, uint16_t rp)
{
    wg_tcp_t* t = (wg_tcp_t*)ud;
    (void)la;
    (void)ra;
    t->rs ^= t->rs << 13;
    t->rs ^= t->rs >> 7;
    t->rs ^= t->rs << 17;
    return (uint32_t)(t->rs >> 16) ^ ((uint32_t)lp << 16 | rp) ^ (uint32_t)(now_ms(t) * 250);
}

static unsigned hkey(const wg_tcp_t* t, const wg_ipaddr_t* ra, uint16_t lp, uint16_t rp)
{
    uint32_t h = 2166136261u ^ ((uint32_t)lp << 16 | rp);
    unsigned i;
    for (i = 0; i < 16; i++)
        h = (h ^ ra->a[i]) * 16777619u;
    return (h ^ h >> 15) & t->hmask;
}

static int same(const wg_ipaddr_t* a, const wg_ipaddr_t* b)
{
    return a->v6 == b->v6 && memcmp(a->a, b->a, a->v6 ? 16 : 4) == 0;
}

static wg_tconn_t* lookup(const wg_tcp_t* t, const wg_ipaddr_t* la, uint16_t lp, const wg_ipaddr_t* ra, uint16_t rp)
{
    wg_tconn_t* c;
    if (!t->ht)
        return NULL;
    for (c = t->ht[hkey(t, ra, lp, rp)]; c; c = c->hnext)
        if (c->lp == lp && c->rp == rp && same(&c->ra, ra) && same(&c->la, la))
            return c;
    return NULL;
}

static int hash_add(wg_tcp_t* t, wg_tconn_t* c)
{
    unsigned i, k;
    if (t->hcount + 1 > (t->hmask + 1) / 2 || !t->ht) {
        unsigned n = t->ht ? (t->hmask + 1) * 2 : 64;
        wg_tconn_t** nh = (wg_tconn_t**)calloc(n, sizeof *nh);
        wg_tconn_t** old = t->ht;
        unsigned on = t->ht ? t->hmask + 1 : 0;
        if (!nh)
            return -1;
        t->ht = nh;
        t->hmask = n - 1;
        for (i = 0; i < on; i++)
            while (old[i]) {
                wg_tconn_t* x = old[i];
                old[i] = x->hnext;
                k = hkey(t, &x->ra, x->lp, x->rp);
                x->hnext = nh[k];
                nh[k] = x;
            }
        free(old);
    }
    k = hkey(t, &c->ra, c->lp, c->rp);
    c->hnext = t->ht[k];
    t->ht[k] = c;
    t->hcount++;
    return 0;
}

static void hash_del(wg_tcp_t* t, wg_tconn_t* c)
{
    wg_tconn_t** p = &t->ht[hkey(t, &c->ra, c->lp, c->rp)];
    while (*p && *p != c)
        p = &(*p)->hnext;
    if (*p) {
        *p = c->hnext;
        t->hcount--;
    }
}

static void free_segs(wg_tseg_t* s)
{
    while (s) {
        wg_tseg_t* n = s->next;
        free(s);
        s = n;
    }
}

static void conn_free(wg_tconn_t* c)
{
    free(c->sb);
    free_segs(c->ooo);
    free_segs(c->pq);
    free(c);
}

void wgt_kill(wg_tconn_t* c)
{
    wg_tcp_t* t = c->t;
    if (c->dead)
        return;
    c->dead = 1;
    hash_del(t, c);
    wgt_hdel(t, c);
    if (c->ls && c->state == WG_TCP_SYN_RCVD)
        c->ls->pending--;
    c->state = WG_TCP_CLOSED;
    t->st.live--;
    c->znext = t->zombies;
    t->zombies = c;
}

void wgt_reap(wg_tcp_t* t)
{
    while (!t->depth && t->zombies) {
        wg_tconn_t* c = t->zombies;
        t->zombies = c->znext;
        conn_free(c);
    }
}

void wgt_close_err(wg_tconn_t* c, int err)
{
    const wg_tcp_cb_t* cb = c->cb;
    c->cb = NULL;
    if (cb && cb->on_close && !c->dead)
        cb->on_close(c->ud, c, err);
    wgt_kill(c);
}

int wgt_out_seg(wg_tcp_t* t, const wg_ipaddr_t* la, const wg_ipaddr_t* ra, uint16_t lp, uint16_t rp, uint32_t seq, uint32_t ack, uint8_t fl, unsigned win, const uint8_t* opt,
                   size_t ol, const uint8_t* d1, size_t n1, const uint8_t* d2, size_t n2)
{
    uint8_t* s = t->seg;
    size_t hl = 20 + ol, n = hl + n1 + n2;
    uint16_t c;
    put16(s, lp);
    put16(s + 2, rp);
    put32(s + 4, seq);
    put32(s + 8, ack);
    s[12] = (uint8_t)(hl / 4 << 4);
    s[13] = fl;
    put16(s + 14, win);
    s[16] = s[17] = s[18] = s[19] = 0;
    if (ol)
        memcpy(s + 20, opt, ol);
    if (n1)
        memcpy(s + hl, d1, n1);
    if (n2)
        memcpy(s + hl + n1, d2, n2);
    c = wg_csum_fin(wg_csum_acc(wg_csum_pseudo(la, ra, WG_PROTO_TCP, n), s, n));
    memcpy(s + 16, &c, 2);
    t->st.segs_out++;
    if (fl & F_RST)
        t->st.rst_sent++;
    if (wg_ip_output(t->ip, la, ra, WG_PROTO_TCP, s, n) < 0) {
        t->st.tx_fail++;
        return -1;
    }
    return 0;
}

static void ack_sent(wg_tconn_t* c)
{
    c->dack_at = 0;
    c->unacked = 0;
}

int wgt_xmit(wg_tconn_t* c, uint32_t seq, uint32_t len, uint8_t extra)
{
    wg_tcp_t* t = c->t;
    uint8_t o[40];
    size_t ol = 0;
    uint32_t dl = 0, off, a = 0, b = 0;
    uint8_t fl = extra;
    int syn = (fl & F_SYN) != 0, r;
    if (syn) {
        o[0] = 2;
        o[1] = 4;
        put16(o + 2, WG_IP_MTU - (c->ra.v6 ? WG_IP_HDR6 : WG_IP_HDR4) - 20);
        ol = 4;
        if (c->sack_ok) {
            o[4] = o[5] = 1;
            o[6] = 4;
            o[7] = 2;
            ol = 8;
        }
        if (c->ws_ok) {
            o[ol] = 1;
            o[ol + 1] = 3;
            o[ol + 2] = 3;
            o[ol + 3] = c->rcv_scale;
            ol += 4;
        }
    } else if (c->sack_ok && c->ooo && (fl & F_ACK))
        ol = wgt_sack_opts(c, o);
    if (!syn && SLT(seq, c->snd_end)) {
        dl = umin(umin(len, c->snd_end - seq), c->mss - (uint32_t)ol);
        off = (c->sb_head + (seq - c->sbase)) & (c->sb_cap - 1);
        a = umin(dl, c->sb_cap - off);
        b = dl - a;
        if (c->fin_req && seq + dl == c->snd_end && len > dl)
            fl |= F_FIN;
        if (dl)
            fl |= F_PSH;
    } else if (!syn && len && c->fin_req && seq == c->snd_end)
        fl |= F_FIN;
    r = wgt_out_seg(t, &c->la, &c->ra, c->lp, c->rp, seq, (fl & F_ACK) ? c->rcv_nxt : 0, fl, (fl & F_RST) ? 0 : wgt_win_field(c, syn), o, ol, dl ? c->sb + ((c->sb_head + (seq - c->sbase)) & (c->sb_cap - 1)) : NULL, a,
                c->sb, b);
    if (fl & F_ACK)
        ack_sent(c);
    c->last_send = now_ms(t);
    (void)r;
    return (int)(dl + ((fl & (F_SYN | F_FIN)) ? 1 : 0));
}

void wgt_send_ack(wg_tconn_t* c)
{
    c->t->st.ack_now++;
    wgt_xmit(c, c->snd_nxt, 0, F_ACK);
}

void wgt_time_wait(wg_tconn_t* c)
{
    const wg_tcp_cb_t* cb = c->cb;
    c->state = WG_TCP_TIME_WAIT;
    c->rto_at = c->ka_at = 0;
    c->tw_at = now_ms(c->t) + 2 * WG_TCP_MSL_MS;
    c->cb = NULL;
    if (cb && cb->on_close)
        cb->on_close(c->ud, c, 0);
}

void wgt_established(wg_tconn_t* c)
{
    c->state = WG_TCP_ESTABLISHED;
    if (c->syn_rexmit && !c->sampled)
        c->rto = WG_TCP_RTO_SYN_EST_MS;
    c->cwnd = wgt_init_win(c);
    c->rexmit_start = 0;
    c->rto_at = 0;
    if (c->ls) {
        c->ls->pending--;
        if (!c->cb) {
            c->cb = c->ls->cb;
            c->ud = c->ls->ud;
        }
        c->ls = NULL;
    }
    if (c->ka_idle)
        c->ka_at = now_ms(c->t) + c->ka_idle;
    if (c->cb && c->cb->on_open)
        c->cb->on_open(c->ud, c);
    if (!c->dead && c->fin_req)
        c->state = WG_TCP_FIN_WAIT_1;
}

static void rst_reply(wg_tcp_t* t, const wg_ipaddr_t* la, const wg_ipaddr_t* ra, uint16_t lp, uint16_t rp, const seg_t* g)
{
    if (g->fl & F_RST)
        return;
    if (g->fl & F_ACK)
        wgt_out_seg(t, la, ra, lp, rp, g->ack, 0, F_RST, 0, NULL, 0, NULL, 0, NULL, 0);
    else
        wgt_out_seg(t, la, ra, lp, rp, 0, g->seq + g->dlen + ((g->fl & F_SYN) ? 1u : 0u) + ((g->fl & F_FIN) ? 1u : 0u), F_RST | F_ACK, 0, NULL, 0, NULL, 0, NULL, 0);
}

static void peer_opts(wg_tconn_t* c, const seg_t* g, int we_offer_ws, int we_offer_sack)
{
    unsigned def = c->ra.v6 ? WG_TCP_MSS6_DEFAULT : WG_TCP_MSS4_DEFAULT, ours = WG_IP_MTU - (c->ra.v6 ? WG_IP_HDR6 : WG_IP_HDR4) - 20;
    c->mss = g->mss ? g->mss : def;
    if (c->mss > ours)
        c->mss = ours;
    if (c->mss < 64)
        c->mss = 64;
    c->ws_ok = we_offer_ws && g->ws;
    c->snd_scale = c->ws_ok ? (uint8_t)(g->ws - 1) : 0;
    c->rcv_scale = c->ws_ok ? WG_TCP_WSCALE : 0;
    c->sack_ok = we_offer_sack && g->sackp;
}

static wg_tconn_t* conn_new(wg_tcp_t* t, const wg_ipaddr_t* la, uint16_t lp, const wg_ipaddr_t* ra, uint16_t rp)
{
    wg_tconn_t* c = (wg_tconn_t*)calloc(1, sizeof *c);
    if (!c)
        return NULL;
    c->t = t;
    c->la = *la;
    c->ra = *ra;
    c->lp = lp;
    c->rp = rp;
    c->rto = WG_TCP_RTO_INIT_MS;
    c->ssthresh = 0xffffffffu;
    c->mss = ra->v6 ? WG_TCP_MSS6_DEFAULT : WG_TCP_MSS4_DEFAULT;
    c->iss = t->isn(t->isn_ud, la, lp, ra, rp);
    c->snd_una = c->snd_nxt = c->iss;
    c->sbase = c->snd_end = c->iss + 1;
    if (hash_add(t, c) < 0) {
        free(c);
        return NULL;
    }
    t->st.conns++;
    t->st.live++;
    return c;
}

static void syn_out(wg_tconn_t* c, uint8_t fl)
{
    c->rtt_ts = now_ms(c->t);
    c->rtt_seq = c->iss;
    wgt_xmit(c, c->iss, 0, fl);
    c->snd_nxt = c->iss + 1;
    wgt_arm_rto(c, 1);
}

static void listen_in(wg_tcp_t* t, const wg_ipaddr_t* la, const wg_ipaddr_t* ra, uint16_t lp, uint16_t rp, const seg_t* g)
{
    wg_tlisten_t* l;
    wg_tconn_t* c;
    for (l = t->ls; l; l = l->next)
        if (l->port == lp && l->v6 == la->v6)
            break;
    if (!l || (g->fl & (F_RST | F_ACK | F_SYN)) != F_SYN) {
        rst_reply(t, la, ra, lp, rp, g);
        return;
    }
    if (l->pending >= l->backlog || !(c = conn_new(t, la, lp, ra, rp))) {
        t->st.syn_drop++;
        return;
    }
    c->ls = l;
    l->pending++;
    c->state = WG_TCP_SYN_RCVD;
    c->irs = g->seq;
    c->rcv_nxt = c->rcv_user = c->rcv_adv = c->rcv_wedge = g->seq + 1;
    peer_opts(c, g, t->ws, t->sack);
    c->snd_wnd = g->win;
    syn_out(c, F_SYN | F_ACK);
    wgt_sched(c);
}

static void syn_sent_in(wg_tconn_t* c, const seg_t* g)
{
    if ((g->fl & F_ACK) && (SLE(g->ack, c->iss) || SLT(c->snd_nxt, g->ack))) {
        if (!(g->fl & F_RST))
            wgt_out_seg(c->t, &c->la, &c->ra, c->lp, c->rp, g->ack, 0, F_RST, 0, NULL, 0, NULL, 0, NULL, 0);
        return;
    }
    if (g->fl & F_RST) {
        if (g->fl & F_ACK) {
            c->t->st.rst_rcvd++;
            wgt_close_err(c, -ECONNREFUSED);
        }
        return;
    }
    if (!(g->fl & F_SYN))
        return;
    c->irs = g->seq;
    c->rcv_nxt = c->rcv_user = c->rcv_adv = c->rcv_wedge = g->seq + 1;
    peer_opts(c, g, c->t->ws, c->t->sack);
    c->snd_wnd = g->win;
    c->snd_wl1 = g->seq;
    c->snd_wl2 = g->ack;
    if (g->fl & F_ACK) {
        c->snd_una = g->ack;
        c->max_sndwnd = c->snd_wnd;
        if (c->rtt_ts && !c->syn_rexmit)
            wgt_rtt_sample(c, (uint32_t)(now_ms(c->t) - c->rtt_ts));
        c->rtt_ts = 0;
        wgt_send_ack(c);
        wgt_established(c);
        if (!c->dead)
            wgt_output(c);
    } else {
        c->state = WG_TCP_SYN_RCVD;
        c->max_sndwnd = c->snd_wnd;
        wgt_xmit(c, c->iss, 0, F_SYN | F_ACK);
    }
}

static void tcp_in(void* ud, const wg_ipaddr_t* src, const wg_ipaddr_t* dst, const uint8_t* l4, size_t n)
{
    wg_tcp_t* t = (wg_tcp_t*)ud;
    seg_t g;
    wg_tconn_t* c;
    uint16_t sp, dp;
    t->st.segs_in++;
    if (n < 20 || wgt_parse(&g, l4, n) < 0) {
        t->st.bad++;
        return;
    }
    if (!wg_ip_take_trust(t->ip) && wg_csum_fin(wg_csum_acc(wg_csum_pseudo(src, dst, WG_PROTO_TCP, n), l4, n))) {
        t->st.bad_csum++;
        return;
    }
    t->st.sack_blocks_in += g.nsk;
    sp = be16(l4);
    dp = be16(l4 + 2);
    t->depth++;
    c = lookup(t, dst, dp, src, sp);
    if (!c)
        listen_in(t, dst, src, dp, sp, &g);
    else {
        if (c->ka_idle && c->state == WG_TCP_ESTABLISHED) {
            c->ka_at = now_ms(t) + c->ka_idle;
            c->ka_probes = 0;
        }
        if (c->state == WG_TCP_SYN_SENT)
            syn_sent_in(c, &g);
        else
            wgt_synced_in(c, &g);
        if (!c->dead) {
            wgt_output(c);
            wgt_sched(c);
        }
    }
    t->depth--;
    wgt_reap(t);
}

wg_tcp_t* wg_tcp_new(wg_ip_t* ip, wg_ip_clock_fn clock, void* ud)
{
    wg_tcp_t* t = (wg_tcp_t*)calloc(1, sizeof *t);
    if (!t)
        return NULL;
    t->ip = ip;
    t->clock = clock;
    t->cud = ud;
    t->isn = def_isn;
    t->isn_ud = t;
    t->rs = 0x9e3779b97f4a7c15ULL ^ clock(ud);
    t->eph = WG_IP_EPH_LO;
    t->sack = t->ws = 1;
    if (wg_ip_set_proto(ip, WG_PROTO_TCP, tcp_in, t) < 0) {
        free(t);
        return NULL;
    }
    return t;
}

void wg_tcp_free(wg_tcp_t* t)
{
    unsigned i;
    if (!t)
        return;
    wg_ip_set_proto(t->ip, WG_PROTO_TCP, NULL, NULL);
    for (i = 0; t->ht && i <= t->hmask; i++)
        while (t->ht[i]) {
            wg_tconn_t* c = t->ht[i];
            t->ht[i] = c->hnext;
            conn_free(c);
        }
    t->depth = 0;
    wgt_reap(t);
    while (t->ls) {
        wg_tlisten_t* l = t->ls;
        t->ls = l->next;
        free(l);
    }
    free(t->ht);
    free(t->heap);
    free(t);
}

void wg_tcp_set_isn(wg_tcp_t* t, wg_tcp_isn_fn fn, void* ud)
{
    t->isn = fn ? fn : def_isn;
    t->isn_ud = fn ? ud : t;
}

void wg_tcp_set_options(wg_tcp_t* t, int sack, int wscale)
{
    t->sack = !!sack;
    t->ws = !!wscale;
}

wg_tlisten_t* wg_tcp_listen(wg_tcp_t* t, int v6, uint16_t port, unsigned backlog, const wg_tcp_cb_t* cb, void* ud)
{
    wg_tlisten_t* l;
    for (l = t->ls; l; l = l->next)
        if (l->port == port && l->v6 == !!v6)
            return NULL;
    if (!port || !(l = (wg_tlisten_t*)calloc(1, sizeof *l)))
        return NULL;
    l->t = t;
    l->v6 = !!v6;
    l->port = port;
    l->backlog = backlog ? backlog : 1;
    l->cb = cb;
    l->ud = ud;
    l->next = t->ls;
    t->ls = l;
    return l;
}

void wg_tcp_unlisten(wg_tlisten_t* l)
{
    wg_tcp_t* t = l->t;
    wg_tlisten_t** p = &t->ls;
    unsigned i;
    wg_tconn_t* c;
    for (i = 0; t->ht && i <= t->hmask; i++)
        for (c = t->ht[i]; c; c = c->hnext)
            if (c->ls == l) {
                c->ls = NULL;
                c->cb = l->cb;
                c->ud = l->ud;
            }
    while (*p != l)
        p = &(*p)->next;
    *p = l->next;
    free(l);
}

wg_tconn_t* wg_tcp_connect(wg_tcp_t* t, const wg_ipaddr_t* dst, uint16_t port, const wg_tcp_cb_t* cb, void* ud)
{
    const wg_ipaddr_t* la = wg_ip_src_for(t->ip, dst->v6);
    wg_tconn_t* c;
    unsigned i;
    uint16_t lp = 0;
    if (!la || !port)
        return NULL;
    for (i = 0; i < 65536u - WG_IP_EPH_LO && !lp; i++) {
        uint16_t p = t->eph;
        t->eph = (uint16_t)(p == 65535 ? WG_IP_EPH_LO : p + 1);
        if (!lookup(t, la, p, dst, port))
            lp = p;
    }
    if (!lp || !(c = conn_new(t, la, lp, dst, port)))
        return NULL;
    c->cb = cb;
    c->ud = ud;
    c->state = WG_TCP_SYN_SENT;
    c->sack_ok = (uint8_t)t->sack;
    c->ws_ok = (uint8_t)t->ws;
    c->rcv_scale = t->ws ? WG_TCP_WSCALE : 0;
    t->depth++;
    syn_out(c, F_SYN);
    wgt_sched(c);
    t->depth--;
    return c;
}

void wg_tcp_set_cb(wg_tconn_t* c, const wg_tcp_cb_t* cb, void* ud)
{
    c->cb = cb;
    c->ud = ud;
}

size_t wg_tcp_write(wg_tconn_t* c, const uint8_t* d, size_t n)
{
    uint32_t have, room, k, off, a;
    if (c->dead || c->fin_req || c->state == WG_TCP_CLOSED || (c->state > WG_TCP_ESTABLISHED && c->state != WG_TCP_CLOSE_WAIT))
        return 0;
    have = c->snd_end - c->sbase;
    room = WG_TCP_SND_MAX - have;
    k = (uint32_t)(n < room ? n : room);
    if (k < n)
        c->blocked = 1;
    if (!k)
        return 0;
    if (have + k > c->sb_cap) {
        uint32_t cap = c->sb_cap ? c->sb_cap : 4096;
        uint8_t* nb;
        while (cap < have + k)
            cap *= 2;
        if (!(nb = (uint8_t*)malloc(cap)))
            return 0;
        for (off = 0; off < have; off += a) {
            uint32_t at = (c->sb_head + off) & (c->sb_cap - 1);
            a = umin(have - off, c->sb_cap - at);
            memcpy(nb + off, c->sb + at, a);
        }
        free(c->sb);
        c->sb = nb;
        c->sb_cap = cap;
        c->sb_head = 0;
    }
    for (off = 0; off < k; off += a) {
        uint32_t at = (c->sb_head + have + off) & (c->sb_cap - 1);
        a = umin(k - off, c->sb_cap - at);
        memcpy(c->sb + at, d + off, a);
    }
    c->snd_end += k;
    c->t->depth++;
    wgt_output(c);
    wgt_sched(c);
    c->t->depth--;
    wgt_reap(c->t);
    return k;
}

void wg_tcp_shutdown(wg_tconn_t* c)
{
    wg_tcp_t* t = c->t;
    if (c->dead || c->fin_req)
        return;
    t->depth++;
    if (c->state == WG_TCP_SYN_SENT)
        wgt_kill(c);
    else {
        c->fin_req = 1;
        if (c->state == WG_TCP_ESTABLISHED)
            c->state = WG_TCP_FIN_WAIT_1;
        else if (c->state == WG_TCP_CLOSE_WAIT)
            c->state = WG_TCP_LAST_ACK;
        wgt_output(c);
        wgt_sched(c);
    }
    t->depth--;
    wgt_reap(t);
}

void wg_tcp_abort(wg_tconn_t* c)
{
    wg_tcp_t* t = c->t;
    if (c->dead)
        return;
    if (c->state >= WG_TCP_SYN_RCVD && c->state != WG_TCP_TIME_WAIT)
        wgt_out_seg(t, &c->la, &c->ra, c->lp, c->rp, c->snd_nxt, 0, F_RST, 0, NULL, 0, NULL, 0, NULL, 0);
    c->cb = NULL;
    wgt_kill(c);
    wgt_reap(t);
}

void wg_tcp_set_nodelay(wg_tconn_t* c, int on)
{
    c->nodelay = (uint8_t)!!on;
}

void wg_tcp_endpoints(const wg_tconn_t* c, wg_ipaddr_t* la, uint16_t* lp, wg_ipaddr_t* ra, uint16_t* rp)
{
    *la = c->la;
    *lp = c->lp;
    *ra = c->ra;
    *rp = c->rp;
}

void wg_tcp_info(const wg_tconn_t* c, wg_tcp_info_t* i)
{
    memset(i, 0, sizeof *i);
    i->state = c->state;
    i->sack_ok = c->sack_ok;
    i->ws_ok = c->ws_ok;
    i->in_recovery = c->in_rec;
    i->rto_recovery = c->rto_rec;
    i->snd_scale = c->snd_scale;
    i->rcv_scale = c->rcv_scale;
    i->mss = c->mss;
    i->nsb = c->nsk;
    i->nooo = c->nooo;
    i->iss = c->iss;
    i->irs = c->irs;
    i->snd_una = c->snd_una;
    i->snd_nxt = c->snd_nxt;
    i->rcv_nxt = c->rcv_nxt;
    i->snd_wnd = c->snd_wnd;
    i->rcv_wnd = c->rcv_wedge - c->rcv_nxt;
    i->cwnd = c->cwnd;
    i->ssthresh = c->ssthresh;
    i->queued = c->snd_end - c->sbase;
    i->srtt_ms = c->srtt >> 3;
    i->rttvar_ms = c->rttvar >> 2;
    i->rto_ms = c->rto;
}

void wg_tcp_stats(const wg_tcp_t* t, wg_tcp_stats_t* st)
{
    *st = t->st;
}
