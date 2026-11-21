#ifndef DYN_WEBRTC_SRTP_H
#define DYN_WEBRTC_SRTP_H

#include <stddef.h>
#include <stdint.h>

enum {
    WRT_SRTP_CM80 = 1,
    WRT_SRTP_CM32 = 2,
    WRT_SRTP_GCM128 = 7,
    WRT_SRTP_GCM256 = 8
};

enum {
    WRT_SRTP_OK = 0,
    WRT_SRTP_ERR_PARSE = -1,
    WRT_SRTP_ERR_AUTH = -2,
    WRT_SRTP_ERR_REPLAY = -3,
    WRT_SRTP_ERR_SPACE = -4,
    WRT_SRTP_ERR_LIMIT = -5,
    WRT_SRTP_ERR_STATE = -6
};

#define WRT_SRTP_WINDOW 1024u
#define WRT_SRTP_MAX_STREAMS 64u
#define WRT_SRTP_MAX_TRAILER 20u

typedef struct {
    uint64_t win[WRT_SRTP_WINDOW / 64u];
    uint64_t top;
    uint8_t init;
} wrt_srtp_replay;

typedef struct {
    uint32_t ssrc;
    uint32_t roc;
    uint32_t rtcp_next;
    uint16_t s_l;
    uint8_t init;
    wrt_srtp_replay rtp;
    wrt_srtp_replay rtcp;
} wrt_srtp_stream;

typedef struct {
    void* rtp_c;
    void* rtcp_c;
    void* rtp_h;
    void* rtcp_h;
    wrt_srtp_stream* st;
    uint32_t nst;
    uint32_t cap;
    uint8_t rtp_salt[14];
    uint8_t rtcp_salt[14];
    uint8_t profile;
    uint8_t gcm;
    uint8_t tag;
    uint8_t rtcp_tag;
} wrt_srtp;

int wrt_srtp_profile_lengths(int profile, size_t* key, size_t* salt);
int wrt_srtp_init(wrt_srtp* s, int profile, const uint8_t* master_key, const uint8_t* master_salt);
int wrt_srtp_init_session(wrt_srtp* s, int profile, const uint8_t* rtp_key, const uint8_t* rtp_salt, const uint8_t* rtp_auth,
    const uint8_t* rtcp_key, const uint8_t* rtcp_salt, const uint8_t* rtcp_auth);
int wrt_srtp_init_dtls(wrt_srtp* tx, wrt_srtp* rx, int profile, const uint8_t* block, size_t len, int is_client);
void wrt_srtp_free(wrt_srtp* s);
int wrt_srtp_kdf(int profile, const uint8_t* master_key, const uint8_t* master_salt, uint8_t label, uint8_t* out, size_t n);
int wrt_srtp_protect(wrt_srtp* s, uint8_t* pkt, size_t len, size_t cap, size_t* out_len);
int wrt_srtp_unprotect(wrt_srtp* s, uint8_t* pkt, size_t len, size_t* out_len);
int wrt_srtcp_protect(wrt_srtp* s, uint8_t* pkt, size_t len, size_t cap, size_t* out_len);
int wrt_srtcp_protect_tag_only(wrt_srtp* s, uint8_t* pkt, size_t len, size_t cap, size_t* out_len);
int wrt_srtcp_unprotect(wrt_srtp* s, uint8_t* pkt, size_t len, size_t* out_len);
int wrt_srtp_set_roc(wrt_srtp* s, uint32_t ssrc, uint32_t roc, uint16_t seq);
int wrt_srtp_set_rtcp_index(wrt_srtp* s, uint32_t ssrc, uint32_t next);

#endif
