#ifndef DYN_RTC_SCTP_H
#define DYN_RTC_SCTP_H

#include "rtc.h"

#define RTC_SCTP_RWND (1u << 20)
#define RTC_SCTP_RX_WIN 4096u
#define RTC_SCTP_SLOT 1280u
#define RTC_SCTP_SLOT_CACHE 256u
#define RTC_SCTP_HIX 2048u
#define RTC_SCTP_DEFAULT_MTU 1172u
#define RTC_SCTP_RTO_INITIAL_US 1000000u
#define RTC_SCTP_RTO_MIN_US 1000000u
#define RTC_SCTP_RTO_MAX_US 60000000u
#define RTC_SCTP_SACK_DELAY_US 200000u
#define RTC_SCTP_MAX_INIT_RTX 8
#define RTC_SCTP_ASSOC_MAX_RTX 10
#define RTC_SCTP_HB_INTERVAL_US 30000000u
#define RTC_SCTP_ASM_KEEP 65536u
#define RTC_SCTP_OM_KEEP 1024u
#define RTC_SCTP_STREAMS 65535u
#define RTC_SCTP_SPAGE 32u
#define RTC_SCTP_EDMID_DTLS 1u

enum {
    RTC_SCTP_CLOSED,
    RTC_SCTP_COOKIE_WAIT,
    RTC_SCTP_COOKIE_ECHOED,
    RTC_SCTP_ESTABLISHED,
    RTC_SCTP_SHUTDOWN_SENT,
    RTC_SCTP_ABORTED,
    RTC_SCTP_SHUTDOWN_RECEIVED,
    RTC_SCTP_SHUTDOWN_ACK_SENT
};

enum {
    RTC_SCTP_DATA = 0,
    RTC_SCTP_INIT = 1,
    RTC_SCTP_INIT_ACK = 2,
    RTC_SCTP_SACK = 3,
    RTC_SCTP_HEARTBEAT = 4,
    RTC_SCTP_HEARTBEAT_ACK = 5,
    RTC_SCTP_ABORT = 6,
    RTC_SCTP_SHUTDOWN = 7,
    RTC_SCTP_SHUTDOWN_ACK = 8,
    RTC_SCTP_ERROR = 9,
    RTC_SCTP_COOKIE_ECHO = 10,
    RTC_SCTP_COOKIE_ACK = 11,
    RTC_SCTP_SHUTDOWN_COMPLETE = 14,
    RTC_SCTP_RECONFIG = 130,
    RTC_SCTP_FORWARD_TSN = 192
};

typedef struct {
    void (*packet)(void* ud, const uint8_t* p, size_t n);
    void (*message)(void* ud, uint16_t stream, uint32_t ppid, const uint8_t* p, size_t n);
    void (*state)(void* ud, int state);
    void (*reset_in)(void* ud, uint16_t stream);
    void (*reset_out)(void* ud, uint16_t stream);
    void (*sent)(void* ud, uint16_t stream, size_t n);
} rtc_sctp_cb;

typedef struct {
    uint8_t* data;
    uint32_t len;
    uint32_t off;
    uint32_t ppid;
    uint32_t first_tsn;
    uint32_t unacked;
    uint64_t expire_us;
    int32_t max_rtx;
    uint16_t stream;
    uint16_t ssn;
    uint8_t unordered;
    uint8_t abandoned;
    uint8_t counted;
} rtc_sctp_omsg;

typedef struct {
    uint64_t sent_us;
    uint32_t msg;
    uint32_t off;
    uint16_t len;
    uint8_t flags;
    uint8_t state;
    uint8_t rtx;
    uint8_t miss;
    uint8_t fast;
    uint8_t pad;
} rtc_sctp_tx;

typedef struct rtc_sctp_rxc {
    uint32_t seg;
    uint32_t len;
    uint32_t ppid;
    uint16_t stream;
    uint16_t ssn;
    uint8_t flags;
    uint8_t data[];
} rtc_sctp_rxc;

typedef struct {
    uint16_t out_ssn;
    uint16_t in_ssn;
    uint8_t in_sync;
    uint8_t reset_want;
    uint8_t reset_sent;
    uint32_t queued;
} rtc_sctp_stream;

typedef struct {
    uint64_t bytes_tx, bytes_rx, msgs_tx, msgs_rx, pkts_tx, pkts_rx, rtx_chunks, fwd_tsn_tx, t3_expiries, fast_rtx, zcsum_tx, zcsum_rx, hb_tx, hb_ack_rx, init_tx, early_steps;
} rtc_sctp_stats;

typedef struct rtc_sctp {
    rtc_sctp_cb cb;
    void* ud;
    rtc_rng* rng;
    int state;
    int closing_reason;
    uint16_t lport, rport;
    uint32_t mtu;
    uint32_t my_tag, peer_tag;
    uint32_t my_itsn, peer_itsn;
    uint8_t secret[16];
    uint8_t peer_prsctp;
    uint8_t peer_reconfig;
    uint8_t peer_zcsum;
    uint8_t zcsum_local;
    uint8_t need_sack;
    uint8_t sack_now;
    uint8_t pkts_since_sack;
    uint8_t in_recovery;
    uint8_t fwd_pending;
    uint8_t cookie_ack_pending;
    uint8_t init_tries;
    uint8_t assoc_errors;
    uint8_t hb_outstanding;
    uint64_t hb_us;
    uint64_t hb_nonce;
    uint8_t* cookie;
    uint16_t cookie_len;
    uint64_t t1_us;
    uint64_t t2_us;
    uint8_t t2_tries;
    uint32_t t1_rto;
    uint64_t sack_due_us;
    uint64_t t3_us;
    uint32_t rto_us, srtt_us, rttvar_us;
    uint8_t have_rtt;
    uint32_t cwnd, ssthresh, pba, flight, peer_rwnd;
    uint32_t recovery_tsn;
    uint32_t next_tsn;
    uint32_t ack_base;
    uint32_t adv_ack;
    uint32_t tx_cap;
    rtc_sctp_tx* tx;
    uint32_t* rtxq;
    uint32_t rtxq_head, rtxq_len, rtxq_cap;
    rtc_sctp_omsg* om;
    uint32_t om_cap, om_head, om_tail, om_chunk;
    uint32_t rx_cum;
    uint64_t rx_bits[RTC_SCTP_RX_WIN / 64];
    rtc_sctp_rxc** stash;
    uint16_t* hix;
    uint32_t stash_bytes;
    rtc_sctp_rxc** slot_stack;
    uint32_t slot_nfree;
    uint32_t dups[16];
    uint8_t ndups;
    uint8_t asm_active;
    uint8_t asm_flags;
    uint16_t asm_stream;
    uint32_t asm_ppid;
    uint32_t asm_len, asm_cap;
    uint8_t* asm_buf;
    uint32_t max_msg;
    rtc_sctp_stream** spages;
    uint32_t npages, nspages;
    uint8_t pg_rx_seen[65536u / RTC_SCTP_SPAGE / 8u];
    uint16_t local_streams;
    uint16_t n_in, n_out;
    uint32_t rw_n, rw_cap;
    uint16_t* rw_list;
    uint8_t bad_sid_pending;
    uint16_t bad_sid;
    uint32_t reconf_req_seq;
    uint32_t reconf_peer_seq;
    uint8_t reconf_inflight;
    uint8_t reconf_tries;
    uint8_t reconf_resp_due;
    uint32_t reconf_resp_seq;
    uint32_t reconf_resp_result;
    uint32_t reconf_last_result;
    uint8_t in_reset_all;
    uint64_t reconf_us;
    uint32_t reconf_last_tsn;
    uint16_t reconf_n;
    uint16_t reconf_list[256];
    uint8_t in_reset_pending;
    uint32_t in_reset_seq;
    uint32_t in_reset_tsn;
    uint16_t in_reset_n;
    uint16_t in_reset_list[256];
    uint64_t now_us;
    rtc_sctp_stats st;
    uint8_t pkt[1500];
    uint32_t pkt_len;
} rtc_sctp;

rtc_sctp* rtc_sctp_new(const rtc_sctp_cb* cb, void* ud, rtc_rng* rng, uint16_t lport, uint16_t rport, uint32_t mtu);
void rtc_sctp_free(rtc_sctp* s);
void rtc_sctp_connect(rtc_sctp* s, uint64_t now);
void rtc_sctp_input(rtc_sctp* s, const uint8_t* p, size_t n, uint64_t now);
int rtc_sctp_send(rtc_sctp* s, uint16_t stream, uint32_t ppid, const uint8_t* p, size_t n, int unordered, int32_t max_rtx,
    int64_t lifetime_ms, uint64_t now);
int rtc_sctp_reset_stream(rtc_sctp* s, uint16_t stream);
void rtc_sctp_flush(rtc_sctp* s, uint64_t now);
uint64_t rtc_sctp_tick(rtc_sctp* s, uint64_t now);
void rtc_sctp_abort(rtc_sctp* s);
uint32_t rtc_sctp_buffered(const rtc_sctp* s, uint16_t stream);
uint32_t rtc_sctp_max_channels(const rtc_sctp* s);
int rtc_sctp_check_packet(const uint8_t* p, size_t n);

#endif
