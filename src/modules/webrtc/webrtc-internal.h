#ifndef DYNA_WEBRTC_INTERNAL_H
#define DYNA_WEBRTC_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include "../../core/dyn-rtc.h"

#define WRT_MAX_SOCKS 8
#define WRT_MAX_SERVERS 8
#define WRT_DTLS_MTU 1200
#define WRT_SCTP_MTU 1152
#define WRT_REMOTE_DC_BUDGET (16u << 20)
#define WRT_REMOTE_DC_COST 1024u
#define WRT_REFUSE_MAX 4096u

enum {
    WRT_DTLS_NEW,
    WRT_DTLS_CONNECTING,
    WRT_DTLS_CONNECTED,
    WRT_DTLS_CLOSED,
    WRT_DTLS_FAILED
};

enum {
    WRT_DC_CONNECTING,
    WRT_DC_OPEN,
    WRT_DC_CLOSING,
    WRT_DC_CLOSED
};

enum {
    WRT_EV_CANDIDATE,
    WRT_EV_GATHER_DONE,
    WRT_EV_ICE_STATE,
    WRT_EV_DTLS_STATE,
    WRT_EV_SCTP_UP,
    WRT_EV_SCTP_DOWN,
    WRT_EV_DC_OPEN,
    WRT_EV_DC_NEW,
    WRT_EV_DC_MSG,
    WRT_EV_DC_CLOSING,
    WRT_EV_DC_CLOSED,
    WRT_EV_DC_BUFLOW,
    WRT_EV_DC_ERROR,
    WRT_EV_CAND_ERROR,
    WRT_EV_PAIR,
    WRT_EV_RTP_UNMUTE,
    WRT_EV_RTP_MUTE
};

typedef struct {
    uint8_t type;
    uint8_t flag;
    uint32_t id;
    uint32_t a;
    uint32_t off;
    uint32_t len;
} wrt_ev;

typedef struct wrt_cert {
    int refs;
    void* pkey;
    void* x509;
    uint8_t fp[32];
    double expires_ms;
    int alg_rsa;
} wrt_cert;

typedef struct {
    void* ssl;
    void* bio;
    const uint8_t* in_p;
    size_t in_n;
    int state;
    int is_server;
    uint8_t remote_fp[64];
    uint8_t remote_fp_len;
    uint8_t remote_fp_alg;
    uint8_t srtp_profile;
    uint8_t fp_failed;
    uint8_t keys[120];
    uint32_t keys_len;
    struct wrt_pc* pc;
} wrt_dtls;

typedef struct {
    uint32_t slot;
    uint16_t id;
    uint8_t state;
    uint8_t negotiated;
    uint8_t ordered;
    uint8_t acked;
    uint8_t reset_in;
    uint8_t reset_out;
    uint8_t open_pending;
    uint8_t has_id;
    uint8_t open_sent;
    uint8_t remote;
    int32_t max_rtx;
    int64_t max_life;
    uint64_t buffered;
    uint64_t low_threshold;
    uint32_t dcep_pending;
    uint32_t zero_pending;
    uint64_t msgs_tx, msgs_rx, bytes_tx, bytes_rx;
    char* label;
    char* protocol;
    void* js;
} wrt_dc;

struct dyn_aio;

typedef void (*wrt_udp_rx_fn)(void* ud, const uint8_t* p, size_t n, const rtc_addr* from);
typedef void (*wrt_udp_end_fn)(void* ud);

struct wrt_udpx;

typedef struct {
    struct wrt_udpx* x;
    rtc_addr local;
} wrt_udp;

typedef struct {
    wrt_udp u;
    struct wrt_pc* pc;
    uint8_t idx;
    uint8_t loopback;
} wrt_sock;

typedef struct {
    rtc_addr addr;
    char user[RTC_TURN_CRED_MAX];
    char pass[RTC_TURN_CRED_MAX];
    char url[256];
    int turn;
    int tcp;
    int unresolved;
} wrt_server;

typedef struct wrt_pc {
    struct dyn_aio* aio;
    rtc_rng rng;
    rtc_ice ice;
    wrt_dtls dtls;
    rtc_sctp* sctp;
    wrt_cert* cert;
    wrt_sock socks[WRT_MAX_SOCKS];
    int nsocks;
    wrt_server servers[WRT_MAX_SERVERS];
    int nservers;
    rtc_turn* turns;
    uint8_t* turn_server;
    uint64_t turn_done;
    int nturns;
    int turns_cap;
    int relay_only;
    int relay_next;
    int bind_n;
    rtc_addr bind_addrs[WRT_MAX_SOCKS];
    int role_known;
    int has_sctp;
    int have_dtls_peer;
    int dtls_local;
    rtc_addr dtls_peer;
    int ever_selected;
    int release_due;
    int nearly;
    struct wrt_early* early;
    uint64_t dtls_dropped;
    int gathering;
    int gather_pending;
    int gather_done;
    int closed;
    int in_input;
    int rx_dirty;
    int free_pending;
    int controlling;
    int sctp_client;
    int sctp_started;
    uint16_t sctp_port;
    uint16_t remote_sctp_port;
    uint32_t remote_max_msg;
    uint32_t local_max_msg;
    int ice_lite_remote;
    char mid[RTC_SDP_MID_MAX];
    int mline;
    wrt_dc** dcs;
    uint32_t ndcs, cap_dcs;
    uint32_t* id2slot;
    uint32_t id_cap;
    wrt_ev* evs;
    uint32_t nev, cap_ev;
    uint8_t* arena;
    uint32_t arena_len, arena_cap;
    uint64_t bytes_tx, bytes_rx, pkts_tx, pkts_rx, rx_drains;
    uint64_t data_tx, data_rx;
    uint64_t dc_opened, dc_closed;
    uint64_t remote_dc_bytes;
    uint32_t dc_refused;
    uint16_t sctp_streams;
    uint8_t sctp_zcsum_off;
    uint64_t ev_lost, send_errors, rtp_dropped;
    struct wrt_media* media;
    void (*notify)(struct wrt_pc* pc);
    void* js;
} wrt_pc;

uint64_t wrt_now_us(void);

int wrt_udp_open(wrt_udp* u, struct dyn_aio* aio, const rtc_addr* bind_addr, wrt_udp_rx_fn rx, wrt_udp_end_fn end, void* ud);
int wrt_udp_send(wrt_udp* u, const rtc_addr* to, const uint8_t* p, size_t n);
void wrt_udp_close(wrt_udp* u);
int wrt_collect_addrs(rtc_addr* out, int cap);
int wrt_parse_server(const char* url, wrt_server* sv, const char** err);
void wrt_ev_push(wrt_pc* pc, int type, int flag, uint32_t id, uint32_t a, const void* p, size_t n);
void wrt_sctp_on_state(void* ud, int state);
extern const rtc_sctp_cb wrt_sctp_cbs;

wrt_cert* wrt_cert_generate(int rsa, uint32_t modulus, double expires_ms, const char** err);
void wrt_cert_ref(wrt_cert* c);
void wrt_cert_unref(wrt_cert* c);
size_t wrt_cert_fp_text(const wrt_cert* c, char* out, size_t cap);
int wrt_cert_pem(const wrt_cert* c, char** cert_pem, char** key_pem);
wrt_cert* wrt_cert_from_pem(const char* cert_pem, const char* key_pem, const char** err);

int wrt_dtls_init(wrt_dtls* d, wrt_pc* pc, wrt_cert* cert, int is_server);
void wrt_dtls_free(wrt_dtls* d);
void wrt_dtls_start(wrt_dtls* d);
int wrt_dtls_input(wrt_dtls* d, const uint8_t* p, size_t n);
int wrt_dtls_write(wrt_dtls* d, const uint8_t* p, size_t n);
uint64_t wrt_dtls_deadline(wrt_dtls* d, uint64_t now);
void wrt_dtls_timeout(wrt_dtls* d);
void wrt_dtls_close(wrt_dtls* d);
const char* wrt_dtls_cipher(const wrt_dtls* d);
const char* wrt_dtls_srtp_name(const wrt_dtls* d);
int wrt_dtls_peer_fp(const wrt_dtls* d, uint8_t out[32]);
int wrt_dtls_peer_der(const wrt_dtls* d, uint8_t** out);
void wrt_dtls_der_free(uint8_t* p);

void wrt_pc_send_dtls(wrt_pc* pc, const uint8_t* p, size_t n);
void wrt_pc_dtls_data(wrt_pc* pc, const uint8_t* p, size_t n);
void wrt_pc_dtls_state(wrt_pc* pc);
int wrt_pc_set_dtls_role(wrt_pc* pc, int is_server, const uint8_t* fp, size_t fp_len, int fp_alg);
size_t wrt_cand_text(const rtc_cand* c, char* out, size_t cap);

wrt_pc* wrt_pc_new(struct dyn_aio* aio, wrt_cert* cert, const uint8_t seed[32]);
void wrt_pc_free(wrt_pc* pc);
int wrt_pc_add_server(wrt_pc* pc, const char* url, const char* user, const char* pass, const char** err);
int wrt_pc_gather(wrt_pc* pc);
int wrt_pc_add_remote_cand(wrt_pc* pc, const rtc_sdp_cand* c);
void wrt_pc_remote_eoc(wrt_pc* pc);
void wrt_pc_start(wrt_pc* pc);
void wrt_pc_regather(wrt_pc* pc);
int wrt_pc_local_shown(const wrt_pc* pc, int i);
uint64_t wrt_pc_tick(wrt_pc* pc, uint64_t now);
void wrt_pc_close(wrt_pc* pc);
void wrt_pc_flush(wrt_pc* pc);
void wrt_pc_events_done(wrt_pc* pc);
size_t wrt_pc_cand_text(const wrt_pc* pc, int i, char* out, size_t cap);
size_t wrt_pc_local_cands(const wrt_pc* pc);

wrt_dc* wrt_dc_create(wrt_pc* pc, const char* label, size_t ll, const char* proto, size_t pl, int ordered, int32_t max_rtx,
    int64_t max_life, int negotiated, int id);
wrt_dc* wrt_dc_by_id(wrt_pc* pc, uint16_t id);
int wrt_dc_send(wrt_pc* pc, wrt_dc* dc, const uint8_t* p, size_t n, int binary);
void wrt_dc_close(wrt_pc* pc, wrt_dc* dc);
void wrt_dc_release(wrt_pc* pc, wrt_dc* dc);
int wrt_dc_id_taken(const wrt_pc* pc, int id);
int wrt_dc_id_limit(const wrt_pc* pc);
int wrt_dc_free_id(const wrt_pc* pc);
void wrt_pc_assign_ids(wrt_pc* pc);


#define WRT_RX_SSRC_MAX 4
#define WRT_RX_CSRC_MAX 16
#define WRT_SRC_WINDOW_US 10000000ull
#define WRT_LEVEL_NONE 255
#define WRT_PT_DEFAULT 128u
#define WRT_PT_NONE 255u
#define WRT_RX_TIMEOUT_US 25000000ull
#define WRT_DTMF_INTERVAL_US 50000ull
#define WRT_DTMF_VOLUME 10u
#define WRT_DTMF_ENDS 3u
#define WRT_RELAY_CODECS 16

struct wrt_relay;

typedef struct {
    uint32_t clock;
    uint8_t pt;
    uint8_t channels;
    char name[22];
} wrt_codec_desc;

typedef struct {
    uint64_t at_us;
    uint32_t src;
    uint32_t rtp_ts;
    uint8_t level;
    uint8_t used;
} wrt_src;

typedef struct {
    uint32_t id;
    const char* mid;
    uint64_t pts[2];
    uint64_t rtx_pts[2];
    const uint32_t* ssrcs;
    int nssrc;
    uint32_t clock;
    uint8_t receptive;
    uint8_t muted;
    uint8_t mid_ext;
    uint8_t te_pt[2];
    struct wrt_relay* relay;
    const wrt_codec_desc* codecs;
    int ncodec;
    uint8_t alevel_ext;
    uint8_t clevel_ext;
} wrt_rx_cfg;

typedef struct {
    uint32_t id;
    uint32_t ssrc;
    const char* mid;
    uint8_t live;
    uint8_t pt;
    uint8_t mid_ext;
    uint32_t clock;
    struct wrt_relay* relay;
    const wrt_codec_desc* codecs;
    int ncodec;
    uint8_t alevel_ext;
    uint8_t clevel_ext;
} wrt_tx_cfg;

typedef struct {
    uint32_t pt;
    uint32_t ts;
    int marker;
    const uint32_t* csrc;
    unsigned cc;
    int level;
    const uint8_t* clevels;
    const uint8_t* payload;
    size_t len;
} wrt_rtp_out;

typedef struct {
    uint64_t rtp_in, rtcp_in, srtp_auth, srtp_replay, srtp_other, unroutable, unkeyed, oversize, rtcp_bad, byes, pt_drop;
    uint64_t rtp_out, rtcp_out, send_fail, imp_drop, imp_dup, imp_reorder;
    uint64_t relay_out, relay_skip, dtmf_out, timeouts;
} wrt_media_counts;

typedef struct {
    uint64_t pkts, bytes;
    uint32_t ext_max, jitter;
    int32_t lost;
    uint32_t te_pkts, te_ends, te_markers, te_segs, te_last_dur, te_first_dur;
    uint8_t te_last_event;
    uint8_t te_last_vol;
    uint32_t ssrc;
    uint8_t has;
} wrt_rx_stats;

int wrt_media_key(wrt_pc* pc);
void wrt_media_free(wrt_pc* pc);
int wrt_media_input(wrt_pc* pc, const uint8_t* p, size_t n, uint64_t now);
int wrt_media_sync_begin(wrt_pc* pc);
int wrt_media_rx_set(wrt_pc* pc, const wrt_rx_cfg* c);
int wrt_media_tx_set(wrt_pc* pc, const wrt_tx_cfg* c);
void wrt_media_sync_end(wrt_pc* pc);
int wrt_media_send(wrt_pc* pc, uint32_t id, const wrt_rtp_out* o);
int wrt_media_send_bye(wrt_pc* pc, uint32_t id);
int wrt_media_send_sr(wrt_pc* pc, uint32_t id, uint64_t now);
int wrt_media_sources(wrt_pc* pc, uint32_t id, int csrc, uint64_t now, wrt_src* out, int max);
int wrt_media_rx_stats(wrt_pc* pc, uint32_t id, wrt_rx_stats* out);
void wrt_media_counts_get(const wrt_pc* pc, wrt_media_counts* out);
void wrt_media_impair(wrt_pc* pc, uint32_t loss_ppm, uint32_t dup_ppm, uint32_t reorder_ppm, uint64_t seed);
void wrt_media_rx_age(wrt_pc* pc, uint32_t id, uint64_t us);
size_t wrt_media_bytes(const wrt_pc* pc);
double wrt_wall_ms(uint64_t mono_us);
uint64_t wrt_media_tick(wrt_pc* pc, uint64_t now);
int wrt_media_dtmf(wrt_pc* pc, uint32_t id, int event, uint32_t dur_ms, uint8_t te_pt, uint32_t te_clock, uint64_t now);
struct wrt_relay* wrt_relay_new(void);
void wrt_relay_ref(struct wrt_relay* h);
void wrt_relay_unref(struct wrt_relay* h);
void wrt_relay_end(struct wrt_relay* h);
size_t wrt_relay_bytes(const struct wrt_relay* h);

#endif
