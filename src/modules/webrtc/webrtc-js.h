#ifndef DYNA_WEBRTC_JS_H
#define DYNA_WEBRTC_JS_H

#include "dyna-nat.h"
#include "dyna-libc.h"
#include "webrtc-internal.h"
#include "../../core/rtc/rtc-jsep.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_WEBRTC) && defined(CONFIG_TLS)

enum {
    WJS_PE_ICECANDIDATE,
    WJS_PE_ICECANDIDATEERROR,
    WJS_PE_SIGNALING,
    WJS_PE_ICECONN,
    WJS_PE_GATHERING,
    WJS_PE_CONN,
    WJS_PE_DATACHANNEL,
    WJS_PE_NEGOTIATION,
    WJS_PE_TRACK,
    WJS_PE_N
};

enum {
    WJS_DE_OPEN,
    WJS_DE_MESSAGE,
    WJS_DE_BUFLOW,
    WJS_DE_ERROR,
    WJS_DE_CLOSING,
    WJS_DE_CLOSE,
    WJS_DE_N
};

enum {
    WJS_SIG_STABLE,
    WJS_SIG_HAVE_LOCAL_OFFER,
    WJS_SIG_HAVE_REMOTE_OFFER,
    WJS_SIG_HAVE_LOCAL_PRANSWER,
    WJS_SIG_HAVE_REMOTE_PRANSWER,
    WJS_SIG_CLOSED
};

enum {
    WJS_SDP_OFFER,
    WJS_SDP_PRANSWER,
    WJS_SDP_ANSWER,
    WJS_SDP_ROLLBACK
};

typedef struct {
    JSValue fn;
    uint32_t id;
    uint8_t type;
    uint8_t once;
    uint8_t handler;
    uint8_t capture;
} wjs_l;

typedef struct {
    wjs_l* ls;
    uint32_t n, cap, next_id;
    uint8_t ntypes;
    JSValue on[WJS_PE_N];
} wjs_et;

typedef struct {
    char kind[16];
    char mid[RTC_SDP_MID_MAX];
    char proto[40];
    char fmt[64];
    uint8_t app;
    uint8_t rejected;
    uint8_t media;
    uint8_t bundle_only;
    uint8_t ldir;
    uint8_t rdir;
    uint8_t has_rdir;
    uint32_t trid;
    uint32_t msid_hash;
} wjs_m;

typedef struct {
    wjs_m* v;
    int n;
    int cap;
} wjs_ml;

typedef struct {
    char* sdp;
    int type;
} wjs_desc;

struct wjs_op;

typedef struct wjs_pc {
    JSContext* ctx;
    JSValue self;
    JSValue pin;
    JSValue timer;
    JSValue tick_fn;
    JSValue config;
    JSValue sctp_obj;
    JSValue dtls_obj;
    JSValue ice_obj;
    uint8_t dtls_st;
    JSValue certs;
    wrt_pc* t;
    wrt_cert* cert;
    wjs_et et;
    struct wjs_op *op_head, *op_tail;
    int op_running;
    int op_job;
    int cert_wait;
    const char* cert_err;
    int signaling;
    int ice_state;
    int conn_state;
    int gather_state;
    int closed;
    int draining;
    int checkpoint;
    int flush_queued;
    int reactor;
    int swept;
    int nneeded;
    int nneeded_queued;
    int is_offerer;
    int local_setup;
    int restart_wanted;
    int offer_restart;
    int lrestart;
    int rrestart;
    int snap_setup;
    int snap_lite;
    int snap_trickle;
    int nscands;
    int seoc;
    rtc_sdp_cand* scands;
    int relay_only;
    int bundle_policy;
    int remote_lite;
    int started;
    int sctp_state;
    uint64_t timer_at;
    uint64_t sess_id, sess_ver;
    char ufrag[8], pwd[32];
    char pend_ufrag[8], pend_pwd[32];
    char rufrag[RTC_SDP_CRED_MAX], rpwd[RTC_SDP_CRED_MAX];
    char srufrag[RTC_SDP_CRED_MAX], srpwd[RTC_SDP_CRED_MAX];
    char fp_text[128];
    wjs_ml m;
    wjs_ml pend;
    wjs_ml offer;
    int app_index;
    int tag_index;
    int local_offer;
    rtc_sdp* roffer;
    rtc_sdp* lanswer;
    rtc_sdp_limits lim;
    JSValue* trs;
    int ntr;
    int cap_tr;
    uint32_t next_trid;
    int mid_floor;
    JSValue rstreams;
    JSValue track_evs;
    char cname[24];
    wjs_desc cur_local, pend_local, cur_remote, pend_remote;
    char* last_offer;
    char* last_answer;
    int can_trickle;
} wjs_pc;

typedef struct {
    char* rid;
    double max_bitrate;
    double max_framerate;
    double scale;
    uint8_t active;
    uint8_t has_bitrate;
    uint8_t has_framerate;
    uint8_t has_scale;
    uint8_t priority;
    uint8_t net_priority;
    struct wjs_cpref* codec;
} wjs_enc;

typedef struct wjs_cpref {
    char mime[96];
    char* fmtp;
    double clock;
    int channels;
    uint8_t has_ch;
    uint8_t has_fmtp;
} wjs_cpref;

typedef struct {
    rtc_sdp_codec* v;
    int n;
    rtc_sdp_ext* x;
    int nx;
    char* blob;
} wjs_codecs;

typedef struct wjs_tr {
    JSContext* ctx;
    wjs_pc* pc;
    JSValue self;
    JSValue pc_obj;
    JSValue sender;
    JSValue receiver;
    JSValue send_track;
    JSValue recv_track;
    JSValue rstreams;
    JSValue clear_fn;
    JSValue dtmf;
    JSValue snap_streams;
    char** sids;
    int nsids;
    wjs_enc* enc;
    int nenc;
    wjs_codecs prefs;
    wjs_codecs neg;
    uint32_t id;
    uint32_t ssrc;
    uint32_t rtx_ssrc;
    uint32_t tid_seq;
    double jbt;
    uint8_t has_jbt;
    uint8_t kind;
    uint8_t dir;
    uint8_t cur_dir;
    uint8_t has_cur;
    uint8_t fired_dir;
    uint8_t stopping;
    uint8_t stopped;
    uint8_t removed;
    uint8_t ever_sent;
    uint8_t by_addtrack;
    uint8_t pend_created;
    uint8_t pend_assoc;
    uint8_t has_mid;
    uint8_t has_last;
    uint8_t rsize;
    uint8_t snap_valid;
    uint8_t snap_dir;
    uint8_t snap_cur;
    uint8_t snap_has_cur;
    uint8_t snap_stopping;
    uint8_t snap_stopped;
    uint8_t snap_fired;
    uint8_t receptive;
    uint8_t nrssrc;
    uint32_t rssrc[4];
    char mid[RTC_SDP_MID_MAX];
} wjs_tr;

typedef struct {
    wjs_et et;
    char id[40];
    char* label;
    uint8_t kind;
    uint8_t enabled;
    uint8_t muted;
    uint8_t ended;
    uint8_t remote;
    uint8_t hint;
    struct wrt_relay* relay;
} wjs_track;

typedef struct {
    wjs_et et;
    char* id;
    JSValue* tracks;
    int n;
    int cap;
} wjs_stream;

typedef struct wjs_dc {
    JSContext* ctx;
    JSValue self;
    JSValue pc_obj;
    wjs_pc* pc;
    wrt_dc* dc;
    wjs_et et;
    char* label;
    char* protocol;
    int state;
    int binary_blob;
    int id;
    int ordered;
    int negotiated;
    int32_t max_rtx;
    int64_t max_life;
    uint64_t low_threshold;
    uint64_t buffered_shadow;
} wjs_dc;

extern JSClassID wjs_pc_cid, wjs_dc_cid, wjs_cand_cid, wjs_desc_cid, wjs_cert_cid, wjs_track_cid, wjs_stream_cid, wjs_tr_cid,
    wjs_sender_cid, wjs_receiver_cid, wjs_dtls_cid, wjs_ice_cid, wjs_dtmf_cid;

int wjs_et_add(JSContext* ctx, wjs_et* et, int type, JSValueConst fn, int capture, int once, int handler);
JSValue wjs_et_listen(JSContext* ctx, wjs_et* et, const char* const* names, int nn, int argc, JSValueConst* argv, int remove);
JSValue wjs_et_on_get(JSContext* ctx, wjs_et* et, int type);
JSValue wjs_et_on_set(JSContext* ctx, wjs_et* et, int type, JSValueConst v);
void wjs_et_free(JSRuntime* rt, wjs_et* et);
void wjs_et_mark(JSRuntime* rt, wjs_et* et, JS_MarkFunc* mark);
void wjs_et_fire(JSContext* ctx, JSValueConst self, wjs_et* et, int type, JSValueConst ev);
JSValue wjs_event(JSContext* ctx, const char* type, JSValueConst target);
int wjs_dispatch(JSContext* ctx, JSValueConst self, wjs_et* et, const char* const* names, int nn, JSValueConst ev);

JSValue wjs_dc_new(JSContext* ctx, wjs_pc* p, wrt_dc* dc);
void wjs_dc_on_event(JSContext* ctx, wjs_pc* p, const wrt_ev* e);
void wjs_dc_detach_all(JSRuntime* rt, wjs_pc* p);
JSValue wjs_cand_new(JSContext* ctx, const char* cand, size_t n, const char* mid, int mline, const char* ufrag);
JSValue wjs_desc_new(JSContext* ctx, int type, const char* sdp, size_t n);
const char* wjs_sdp_type_name(int t);
int wjs_sdp_type_parse(const char* s);
JSValue wjs_cert_new(JSContext* ctx, wrt_cert* c);
wrt_cert* wjs_cert_get(JSContext* ctx, JSValueConst v);
JSValue wjs_rtc_error(JSContext* ctx, const char* detail, const char* msg);

int wjs_pc_register(JSContext* ctx, JSModuleDef* m);
int wjs_media_register(JSContext* ctx, JSModuleDef* m);
JSValue wjs_pc_op_push(JSContext* ctx, wjs_pc* p, int kind, JSValueConst arg);
JSValue wjs_reject_pending(JSContext* ctx);
void wjs_pc_nneeded(JSContext* ctx, wjs_pc* p);
wjs_pc* wjs_pc_get_opaque(JSContext* ctx, JSValueConst v);
wjs_tr* wjs_tr_get(JSValueConst v);
wjs_tr* wjs_tr_by_mid(wjs_pc* p, const char* mid);
wjs_tr* wjs_tr_by_id(wjs_pc* p, uint32_t id);
wjs_track* wjs_track_get(JSValueConst v);
JSValue wjs_tr_create(JSContext* ctx, wjs_pc* p, int kind, int dir, JSValueConst track);
int wjs_media_offer(JSContext* ctx, wjs_pc* p, const wjs_ml* ms, rtc_jsep_offer_m* out);
void wjs_media_offer_free(JSContext* ctx, rtc_jsep_offer_m* out, int n);
int wjs_media_answer_local(JSContext* ctx, wjs_pc* p, const rtc_sdp* offer, rtc_jsep_local* out);
void wjs_media_answer_local_free(JSContext* ctx, rtc_jsep_local* out, int n);
int wjs_media_srd(JSContext* ctx, wjs_pc* p, const rtc_sdp* s, int offer, wjs_ml* ms);
void wjs_media_sld_offer(JSContext* ctx, wjs_pc* p, wjs_ml* ms);
void wjs_media_apply_answer(JSContext* ctx, wjs_pc* p, const wjs_ml* ms, const rtc_sdp* ans, int remote);
void wjs_media_stable(JSContext* ctx, wjs_pc* p);
void wjs_media_rollback(JSContext* ctx, wjs_pc* p);
void wjs_media_commit_pending(wjs_pc* p);
int wjs_media_nneeded(wjs_pc* p);
void wjs_media_fire_tracks(JSContext* ctx, wjs_pc* p);
void wjs_media_drop_events(JSContext* ctx, wjs_pc* p);
void wjs_media_close(JSContext* ctx, wjs_pc* p);
void wjs_media_mark(JSRuntime* rt, wjs_pc* p, JS_MarkFunc* mark);
void wjs_media_free(JSRuntime* rt, wjs_pc* p);
int wjs_parse_meta(JSContext* ctx, wjs_pc* p, JSValueConst meta);
JSValue wjs_pc_media_list(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic);
JSValue wjs_pc_add_track(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue wjs_pc_remove_track(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue wjs_pc_add_transceiver(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue wjs_tr_replace_op(JSContext* ctx, wjs_pc* p, JSValueConst arg);
int wjs_misc_register(JSContext* ctx, JSModuleDef* m);
int wjs_xport_register(JSContext* ctx, JSModuleDef* m);
JSValue wjs_pc_dtls(JSContext* ctx, wjs_pc* p);
void wjs_xport_event(JSContext* ctx, wjs_pc* p, int what);
JSValue wjs_dtmf_new(JSContext* ctx, JSValueConst tr_obj);
void wjs_media_snapshot(JSContext* ctx, wjs_pc* p);
void wjs_media_snap_clear(JSContext* ctx, wjs_pc* p);
int wjs_tr_has_codec(const wjs_tr* t, const char* name);
int wjs_pc_is_connected(const wjs_pc* p);
void wjs_pc_schedule_flush(JSContext* ctx, wjs_pc* p);
void wjs_pc_rearm(JSContext* ctx, wjs_pc* p);
void wjs_pc_rearm_later(JSContext* ctx, wjs_pc* p);
void wjs_pc_drain(JSContext* ctx, wjs_pc* p);
void wjs_media_sync(JSContext* ctx, wjs_pc* p);
void wjs_media_rtp_event(JSContext* ctx, wjs_pc* p, int unmute, uint32_t id);
wrt_pc* wjs_side_route(JSValueConst v, uint32_t* id);
void wjs_side_rearm(JSContext* ctx, JSValueConst v);
int wjs_tr_te(const wjs_tr* t, uint8_t* pt, uint32_t* clock);

#endif

#endif
