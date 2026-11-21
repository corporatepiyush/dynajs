#ifndef WG_TCP_H
#define WG_TCP_H

#include "wg-ip.h"

enum {
    WG_TCP_RTO_INIT_MS = 1000,
    WG_TCP_RTO_MIN_MS = 1000,
    WG_TCP_RTO_MAX_MS = 60000,
    WG_TCP_RTO_SYN_EST_MS = 3000,
    WG_TCP_DACK_MS = 200,
    WG_TCP_MSL_MS = 30000,
    WG_TCP_R2_MS = 120000,
    WG_TCP_R2_SYN_MS = 180000,
    WG_TCP_KA_INTVL_MS = 75000,
    WG_TCP_KA_PROBES = 9,
    WG_TCP_SND_MAX = 1 << 20,
    WG_TCP_RCV_MAX = 1 << 20,
    WG_TCP_WSCALE = 5,
    WG_TCP_OOO_MAX = 256,
    WG_TCP_SB_MAX = 32,
    WG_TCP_DUPTHRESH = 3,
    WG_TCP_MSS4_DEFAULT = 536,
    WG_TCP_MSS6_DEFAULT = 1220
};

enum { WG_TCP_CLOSED, WG_TCP_SYN_SENT, WG_TCP_SYN_RCVD, WG_TCP_ESTABLISHED, WG_TCP_FIN_WAIT_1, WG_TCP_FIN_WAIT_2, WG_TCP_CLOSE_WAIT, WG_TCP_CLOSING, WG_TCP_LAST_ACK, WG_TCP_TIME_WAIT };

typedef struct wg_tcp wg_tcp_t;
typedef struct wg_tconn wg_tconn_t;
typedef struct wg_tlisten wg_tlisten_t;

typedef struct {
    void (*on_open)(void* ud, wg_tconn_t* c);
    void (*on_data)(void* ud, wg_tconn_t* c, const uint8_t* d, size_t n);
    void (*on_eof)(void* ud, wg_tconn_t* c);
    void (*on_writable)(void* ud, wg_tconn_t* c);
    void (*on_close)(void* ud, wg_tconn_t* c, int err);
} wg_tcp_cb_t;

typedef uint32_t (*wg_tcp_isn_fn)(void* ud, const wg_ipaddr_t* la, uint16_t lp, const wg_ipaddr_t* ra, uint16_t rp);

typedef struct {
    uint64_t segs_in, segs_out, bad, bad_csum, rst_sent, rst_rcvd, challenge_acks, syn_drop, conns, live, fast_rexmit, rto_fired, rexmit_segs, rexmit_bytes, sack_rexmit,
        persist_probes, ka_probes, dack_sent, ack_now, ooo_segs, ooo_drop, sack_blocks_in, tx_fail;
} wg_tcp_stats_t;

typedef struct {
    int state, sack_ok, ws_ok, in_recovery, rto_recovery;
    unsigned snd_scale, rcv_scale, mss, nsb, nooo;
    uint32_t iss, irs, snd_una, snd_nxt, rcv_nxt, snd_wnd, rcv_wnd, cwnd, ssthresh, queued, srtt_ms, rttvar_ms, rto_ms;
} wg_tcp_info_t;

wg_tcp_t* wg_tcp_new(wg_ip_t* ip, wg_ip_clock_fn clock, void* ud);
void wg_tcp_free(wg_tcp_t* t);
void wg_tcp_set_isn(wg_tcp_t* t, wg_tcp_isn_fn fn, void* ud);
void wg_tcp_set_options(wg_tcp_t* t, int sack, int wscale);
wg_tlisten_t* wg_tcp_listen(wg_tcp_t* t, int v6, uint16_t port, unsigned backlog, const wg_tcp_cb_t* cb, void* ud);
void wg_tcp_unlisten(wg_tlisten_t* l);
wg_tconn_t* wg_tcp_connect(wg_tcp_t* t, const wg_ipaddr_t* dst, uint16_t port, const wg_tcp_cb_t* cb, void* ud);
void wg_tcp_set_cb(wg_tconn_t* c, const wg_tcp_cb_t* cb, void* ud);
size_t wg_tcp_write(wg_tconn_t* c, const uint8_t* d, size_t n);
void wg_tcp_shutdown(wg_tconn_t* c);
void wg_tcp_abort(wg_tconn_t* c);
void wg_tcp_pause(wg_tconn_t* c, int on);
void wg_tcp_set_nodelay(wg_tconn_t* c, int on);
void wg_tcp_set_keepalive(wg_tconn_t* c, uint64_t idle_ms);
void wg_tcp_endpoints(const wg_tconn_t* c, wg_ipaddr_t* la, uint16_t* lp, wg_ipaddr_t* ra, uint16_t* rp);
void wg_tcp_tick(wg_tcp_t* t);
uint64_t wg_tcp_next_deadline(const wg_tcp_t* t);
void wg_tcp_info(const wg_tconn_t* c, wg_tcp_info_t* i);
void wg_tcp_stats(const wg_tcp_t* t, wg_tcp_stats_t* st);

#endif
