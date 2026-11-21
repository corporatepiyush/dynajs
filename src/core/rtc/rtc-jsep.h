#ifndef DYN_RTC_JSEP_H
#define DYN_RTC_JSEP_H

#include "rtc-sdp.h"

typedef struct {
    const rtc_sdp_codec* audio;
    int naudio;
    const rtc_sdp_codec* video;
    int nvideo;
    const char* const* ext;
    const uint8_t* ext_kinds;
    int next;
    uint8_t data;
    uint8_t mixed;
} rtc_jsep_caps;

typedef struct {
    uint8_t dir;
    uint8_t stopped;
    rtc_sdp_msid msid;
    const rtc_sdp_msid* msids;
    int nmsid;
    const rtc_sdp_codec* codecs;
    int ncodecs;
    uint32_t ssrc;
    uint32_t rtx_ssrc;
    const char* cname;
} rtc_jsep_local;

typedef struct {
    const char* kind;
    const char* mid;
    uint8_t dir;
    uint8_t rejected;
    uint8_t bundle_only;
    const rtc_sdp_codec* codecs;
    int ncodecs;
    const rtc_sdp_msid* msid;
    int nmsid;
    uint32_t ssrc;
    uint32_t rtx_ssrc;
    const char* cname;
    const char* const* rids;
    int nrids;
    const uint16_t* ext_ids;
} rtc_jsep_offer_m;

typedef struct {
    uint64_t sess_id;
    uint64_t sess_ver;
    const char* ufrag;
    const char* pwd;
    const uint8_t* fp;
    uint8_t fp_alg;
    uint8_t fp_len;
    uint8_t setup;
    uint8_t trickle;
    uint16_t sctp_port;
    uint32_t max_msg;
} rtc_jsep_params;

enum {
    RTC_H264_UNKNOWN,
    RTC_H264_CONSTRAINED_BASELINE,
    RTC_H264_BASELINE,
    RTC_H264_MAIN,
    RTC_H264_EXTENDED,
    RTC_H264_HIGH,
    RTC_H264_HIGH10,
    RTC_H264_HIGH422,
    RTC_H264_HIGH444,
    RTC_H264_HIGH10_INTRA,
    RTC_H264_HIGH422_INTRA,
    RTC_H264_HIGH444_INTRA,
    RTC_H264_CAVLC444_INTRA
};

int rtc_jsep_dir_answer(int offered, int local);
int rtc_jsep_h264_profile(const char* fmtp);
int rtc_jsep_codec_match(const rtc_sdp_codec* offered, const rtc_sdp_codec* local);
int rtc_jsep_h264_answer_fmtp(const char* offered, const char* local, char* out, size_t cap);
const rtc_jsep_caps* rtc_jsep_default_caps(void);
int rtc_jsep_offer(const rtc_jsep_offer_m* ms, int n, const rtc_jsep_caps* caps, const rtc_jsep_params* prm, rtc_sdp* out,
    const char** err);
int rtc_jsep_answer(const rtc_sdp* offer, const rtc_jsep_caps* caps, const rtc_jsep_local* local, const rtc_jsep_params* prm,
    rtc_sdp* ans, const char** err);

#endif
