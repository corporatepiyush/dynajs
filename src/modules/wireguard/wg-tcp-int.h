#ifndef WG_TCP_INT_H
#define WG_TCP_INT_H

#include "wg-tcp.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

enum { F_FIN = 1, F_SYN = 2, F_RST = 4, F_PSH = 8, F_ACK = 16 };

#define SLT(a, b) ((int32_t)((uint32_t)(a) - (uint32_t)(b)) < 0)
#define SLE(a, b) ((int32_t)((uint32_t)(a) - (uint32_t)(b)) <= 0)
#define SMAX(a, b) (SLT(a, b) ? (b) : (a))

typedef struct wg_tseg {
    struct wg_tseg* next;
    uint32_t seq, len;
    uint8_t d[];
} wg_tseg_t;

typedef struct {
    uint32_t seq, ack, dlen, win;
    uint8_t fl;
    const uint8_t* d;
    unsigned mss, ws, sackp, nsk;
    uint32_t sk[4][2];
} seg_t;

struct wg_tlisten {
    wg_tcp_t* t;
    wg_tlisten_t* next;
    int v6;
    uint16_t port;
    unsigned backlog, pending;
    const wg_tcp_cb_t* cb;
    void* ud;
};

struct wg_tconn {
    wg_tcp_t* t;
    wg_tconn_t *hnext, *znext;
    unsigned hpos;
    wg_ipaddr_t la, ra;
    uint16_t lp, rp;
    uint8_t state, dead, sack_ok, ws_ok, snd_scale, rcv_scale, nodelay, paused, fin_req, eof_due, eof_done, blocked, in_rec, rto_rec, syn_rexmit, sampled, ka_probes;
    wg_tlisten_t* ls;
    const wg_tcp_cb_t* cb;
    void* ud;
    uint32_t iss, irs, snd_una, snd_nxt, snd_end, sbase, snd_wl1, snd_wl2, rcv_nxt, rcv_adv, rcv_wedge, rcv_user, recover, high_rxt, rtt_seq, ooo_fin, last_ooo;
    uint32_t snd_wnd, max_sndwnd, cwnd, ssthresh, mss, cacc, dupacks, unacked, srtt, rttvar, rto;
    uint64_t rtt_ts, rto_at, dack_at, tw_at, ka_at, ka_idle, rexmit_start, last_send, due;
    uint8_t* sb;
    uint32_t sb_cap, sb_head;
    uint32_t sk[WG_TCP_SB_MAX][2];
    unsigned nsk, nooo, has_ooo_fin;
    wg_tseg_t *ooo, *pq, *pq_tail;
};

struct wg_tcp {
    wg_ip_t* ip;
    wg_ip_clock_fn clock;
    void* cud;
    wg_tcp_isn_fn isn;
    void* isn_ud;
    wg_tconn_t** ht;
    unsigned hmask, hcount;
    wg_tconn_t** heap;
    unsigned nheap, cheap;
    wg_tlisten_t* ls;
    wg_tconn_t* zombies;
    unsigned depth;
    uint16_t eph;
    int sack, ws;
    uint64_t rs;
    wg_tcp_stats_t st;
    uint8_t seg[WG_IP_MTU];
};

static inline uint32_t be32(const uint8_t* p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static inline uint16_t be16(const uint8_t* p)
{
    return (uint16_t)(p[0] << 8 | p[1]);
}

static inline void put32(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static inline void put16(uint8_t* p, unsigned v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static inline uint32_t umin(uint32_t a, uint32_t b)
{
    return a < b ? a : b;
}

static inline uint64_t now_ms(const wg_tcp_t* t)
{
    return t->clock(t->cud);
}

static inline uint32_t flight(const wg_tconn_t* c)
{
    return c->snd_nxt - c->snd_una;
}

static inline uint32_t unsent(const wg_tconn_t* c)
{
    if (SLT(c->snd_end, c->snd_nxt))
        return 0;
    return c->snd_end - c->snd_nxt + (c->fin_req ? 1u : 0u);
}

void wgt_hdel(wg_tcp_t* t, wg_tconn_t* c);
void wgt_sched(wg_tconn_t* c);
void wgt_kill(wg_tconn_t* c);
void wgt_reap(wg_tcp_t* t);
void wgt_close_err(wg_tconn_t* c, int err);
int wgt_out_seg(wg_tcp_t* t, const wg_ipaddr_t* la, const wg_ipaddr_t* ra, uint16_t lp, uint16_t rp, uint32_t seq, uint32_t ack, uint8_t fl, unsigned win, const uint8_t* opt, size_t ol, const uint8_t* d1, size_t n1, const uint8_t* d2, size_t n2);
unsigned wgt_win_field(wg_tconn_t* c, int syn);
size_t wgt_sack_opts(wg_tconn_t* c, uint8_t* o);
int wgt_xmit(wg_tconn_t* c, uint32_t seq, uint32_t len, uint8_t extra);
void wgt_send_ack(wg_tconn_t* c);
void wgt_arm_rto(wg_tconn_t* c, int restart);
void wgt_rexmit(wg_tconn_t* c, uint32_t s, uint32_t len);
int wgt_send_new(wg_tconn_t* c, uint32_t wnd);
uint32_t wgt_init_win(const wg_tconn_t* c);
void wgt_output(wg_tconn_t* c);
void wgt_rtt_sample(wg_tconn_t* c, uint32_t r);
void wgt_ooo_add(wg_tconn_t* c, uint32_t seq, const uint8_t* d, uint32_t n);
void wgt_time_wait(wg_tconn_t* c);
void wgt_established(wg_tconn_t* c);
void wgt_ack_in(wg_tconn_t* c, const seg_t* g);
void wgt_synced_in(wg_tconn_t* c, seg_t* g);
int wgt_parse(seg_t* g, const uint8_t* l4, size_t n);

#endif
