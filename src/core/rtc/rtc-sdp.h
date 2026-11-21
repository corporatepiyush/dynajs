#ifndef DYN_RTC_SDP_H
#define DYN_RTC_SDP_H

#include "rtc.h"

#define RTC_SDP_MAX_INPUT (1u << 20)
#define RTC_SDP_LINE_MAX 4096
#define RTC_SDP_MAX_MEDIA 64
#define RTC_SDP_MAX_CODECS 128
#define RTC_SDP_MAX_EXT 64
#define RTC_SDP_MAX_CANDS 64
#define RTC_SDP_MAX_SSRC 64
#define RTC_SDP_MAX_MSID 16
#define RTC_SDP_MAX_GROUPS 16
#define RTC_SDP_GROUP_SSRCS 16
#define RTC_SDP_MAX_EXTRA 1024
#define RTC_SDP_MID_MAX 33
#define RTC_SDP_TOKEN_MAX 65
#define RTC_SDP_CRED_MAX 257

enum {
    RTC_SETUP_NONE,
    RTC_SETUP_ACTPASS,
    RTC_SETUP_ACTIVE,
    RTC_SETUP_PASSIVE
};

enum {
    RTC_DIR_SENDRECV,
    RTC_DIR_SENDONLY,
    RTC_DIR_RECVONLY,
    RTC_DIR_INACTIVE
};

enum {
    RTC_FP_NONE,
    RTC_FP_SHA1,
    RTC_FP_SHA224,
    RTC_FP_SHA256,
    RTC_FP_SHA384,
    RTC_FP_SHA512
};

enum {
    RTC_FB_NACK = 1,
    RTC_FB_PLI = 2,
    RTC_FB_FIR = 4,
    RTC_FB_TWCC = 8,
    RTC_FB_REMB = 16
};

typedef struct {
    char foundation[33];
    char typ[8];
    char tcptype[8];
    char host[64];
    rtc_addr addr;
    rtc_addr rel;
    uint32_t comp;
    uint32_t prio;
    uint8_t udp;
    uint8_t has_rel;
    uint8_t type;
} rtc_sdp_cand;

typedef struct {
    size_t max_input;
    size_t line_max;
    int media;
    int codecs;
    int ext;
    int cands;
    int ssrc;
    int msid;
    int groups;
    int group_ssrcs;
    int extra;
} rtc_sdp_limits;

typedef struct {
    unsigned char** chunk;
    int n;
    int cap;
    size_t used;
    size_t last;
    size_t bytes;
    size_t hint;
} rtc_arena;

typedef struct {
    const char* name;
    const char* fmtp;
    uint32_t clock;
    uint8_t pt;
    uint8_t channels;
    uint8_t fb;
    uint8_t rtx_apt_set;
    uint8_t rtx_apt;
    uint8_t has_rtpmap;
    uint8_t has_fmtp;
} rtc_sdp_codec;

typedef struct {
    const char* uri;
    const char* attrs;
    uint16_t id;
    uint8_t has_dir;
    uint8_t dir;
} rtc_sdp_ext;

typedef struct {
    const char* stream;
    const char* track;
} rtc_sdp_msid;

typedef struct {
    const char* cname;
    rtc_sdp_msid msid;
    uint32_t id;
} rtc_sdp_ssrc;

typedef struct {
    const char* semantics;
    uint32_t* ssrc;
    int n;
} rtc_sdp_ssrc_group;

typedef struct {
    const char* kind;
    const char* proto;
    const char* fmt_text;
    const char* conn;
    const char* mid;
    const char* ufrag;
    const char* pwd;
    const char* ice_opts;
    uint8_t fp[64];
    uint8_t fp_len;
    uint8_t fp_alg;
    uint8_t setup;
    uint8_t holdconn;
    uint8_t dir;
    uint8_t has_dir;
    uint8_t has_c;
    uint8_t rtcp_mux;
    uint8_t rtcp_mux_only;
    uint8_t rtcp_rsize;
    uint8_t eoc;
    uint8_t trickle;
    uint8_t bundle_only;
    uint8_t allow_mixed;
    uint8_t has_max_msg;
    uint8_t has_sctp_port;
    uint8_t has_port_count;
    uint16_t port;
    uint16_t port_count;
    uint16_t sctp_port;
    uint32_t max_msg;
    int ncands, cap_cands;
    int ncodecs, cap_codecs;
    int next, cap_ext;
    int nssrc, cap_ssrc;
    int nmsid, cap_msid;
    int ngroups, cap_groups;
    int nextra, cap_extra;
    rtc_sdp_cand* cands;
    rtc_sdp_codec* codecs;
    rtc_sdp_ext* ext;
    rtc_sdp_ssrc* ssrcs;
    rtc_sdp_msid* msid;
    rtc_sdp_ssrc_group* groups;
    const char** extra;
} rtc_sdp_media;

typedef struct {
    uint64_t sess_id;
    uint64_t sess_ver;
    uint8_t ice_lite;
    uint8_t trickle;
    uint8_t fp_alg;
    uint8_t fp_len;
    uint8_t setup;
    uint8_t holdconn;
    uint8_t has_bundle;
    uint8_t nbundle_groups;
    uint8_t has_c;
    uint8_t allow_mixed;
    uint8_t has_o;
    uint8_t has_s;
    uint8_t has_t;
    uint8_t fp[64];
    const char* origin;
    const char* name;
    const char* timing;
    const char* conn;
    const char* ice_opts;
    const char* ufrag;
    const char* pwd;
    const char** bundle;
    const char** extra;
    int nbundle, cap_bundle;
    int nextra, cap_extra;
    int nmedia, cap_media;
    rtc_sdp_media* media;
    rtc_arena arena;
    rtc_sdp_limits lim;
} rtc_sdp;

typedef struct {
    char* buf;
    size_t len;
    size_t cap;
    int err;
} rtc_sb;

void rtc_sdp_limits_default(rtc_sdp_limits* l);
void rtc_sdp_init(rtc_sdp* sdp);
void* rtc_sdp_alloc(rtc_sdp* sdp, size_t n);
void rtc_sdp_free(rtc_sdp* sdp);
void rtc_sdp_reset(rtc_sdp* sdp, size_t hint);
int rtc_sdp_parse(rtc_sdp* sdp, const char* s, size_t n, const char** err);
int rtc_sdp_check(const rtc_sdp* sdp, const char** err);
int rtc_sdp_write(const rtc_sdp* sdp, rtc_sb* b);
int rtc_sdp_media_rejected(const rtc_sdp_media* m);
rtc_sdp_media* rtc_sdp_add_media(rtc_sdp* sdp);
rtc_sdp_codec* rtc_sdp_add_codec(rtc_sdp* sdp, rtc_sdp_media* m);
rtc_sdp_ext* rtc_sdp_add_ext(rtc_sdp* sdp, rtc_sdp_media* m);
rtc_sdp_msid* rtc_sdp_add_msid(rtc_sdp* sdp, rtc_sdp_media* m);
int rtc_sdp_add_bundle(rtc_sdp* sdp, const char* mid);
rtc_sdp_ssrc* rtc_sdp_add_ssrc(rtc_sdp* sdp, rtc_sdp_media* m);
rtc_sdp_ssrc_group* rtc_sdp_add_group(rtc_sdp* sdp, rtc_sdp_media* m);
int rtc_sdp_extra_add(rtc_sdp* sdp, rtc_sdp_media* m, char type, const char* v, size_t n);
const char* rtc_sdp_strdup(rtc_sdp* sdp, const char* s, size_t n);
size_t rtc_sdp_bytes(const rtc_sdp* sdp);
int rtc_sdp_fmtp_get(const char* fmtp, const char* key, const char** v, size_t* n);
int rtc_sdp_parse_candidate(const char* s, size_t n, rtc_sdp_cand* c);
size_t rtc_sdp_write_candidate(const rtc_sdp_cand* c, char* out, size_t cap);
int rtc_sdp_fp_alg(const char* s, size_t n);
const char* rtc_sdp_fp_name(int alg);
size_t rtc_sdp_fp_len(int alg);

void rtc_sb_init(rtc_sb* b);
void rtc_sb_free(rtc_sb* b);
void rtc_sb_put(rtc_sb* b, const char* s, size_t n);
void rtc_sb_str(rtc_sb* b, const char* s);
void rtc_sb_u64(rtc_sb* b, uint64_t v);
void rtc_sb_line(rtc_sb* b, const char* a, const char* c);

#endif
