#ifndef DYN_RTC_RTCP_H
#define DYN_RTC_RTCP_H

#include "rtc.h"

enum {
    RTC_RTCP_SR = 200,
    RTC_RTCP_RR = 201,
    RTC_RTCP_SDES = 202,
    RTC_RTCP_BYE = 203,
    RTC_RTCP_APP = 204,
    RTC_RTCP_RTPFB = 205,
    RTC_RTCP_PSFB = 206,
    RTC_RTCP_XR = 207
};

enum {
    RTC_RTCP_FB_NACK = 1,
    RTC_RTCP_FB_TWCC = 15,
    RTC_RTCP_FB_PLI = 1,
    RTC_RTCP_FB_FIR = 4,
    RTC_RTCP_FB_AFB = 15
};

#define RTC_RTCP_MAX_RB 31u

typedef struct {
    const uint8_t* p;
    size_t n;
    uint8_t pt;
    uint8_t count;
} rtc_rtcp_pkt;

typedef struct {
    const uint8_t* p;
    size_t n;
    size_t off;
} rtc_rtcp_iter;

typedef struct {
    uint32_t ssrc;
    uint32_t ext_seq;
    uint32_t jitter;
    uint32_t lsr;
    uint32_t dlsr;
    int32_t lost;
    uint8_t fraction;
} rtc_rtcp_rb;

typedef struct {
    uint64_t ntp;
    uint32_t ssrc;
    uint32_t rtp_ts;
    uint32_t pkts;
    uint32_t octets;
} rtc_rtcp_sr;

typedef struct {
    const uint8_t* fci;
    size_t fci_len;
    uint32_t sender;
    uint32_t media;
} rtc_rtcp_fb;

int rtc_rtcp_check(const uint8_t* p, size_t n);
void rtc_rtcp_iter_init(rtc_rtcp_iter* it, const uint8_t* p, size_t n);
int rtc_rtcp_next(rtc_rtcp_iter* it, rtc_rtcp_pkt* out);
int rtc_rtcp_sr_parse(const rtc_rtcp_pkt* k, rtc_rtcp_sr* sr);
int rtc_rtcp_rr_ssrc(const rtc_rtcp_pkt* k, uint32_t* ssrc);
unsigned rtc_rtcp_rb_count(const rtc_rtcp_pkt* k);
int rtc_rtcp_rb_at(const rtc_rtcp_pkt* k, unsigned i, rtc_rtcp_rb* rb);
int rtc_rtcp_fb_parse(const rtc_rtcp_pkt* k, rtc_rtcp_fb* fb);
int rtc_rtcp_fb_check(const rtc_rtcp_pkt* k, const rtc_rtcp_fb* fb);
unsigned rtc_rtcp_nack_seqs(const rtc_rtcp_fb* fb, uint16_t* out, unsigned max);
unsigned rtc_rtcp_fir_count(const rtc_rtcp_fb* fb);
int rtc_rtcp_fir_at(const rtc_rtcp_fb* fb, unsigned i, uint32_t* ssrc, uint8_t* seq);
int rtc_rtcp_remb(const rtc_rtcp_fb* fb, uint64_t* bps, uint32_t* ssrcs, unsigned max, unsigned* n);
unsigned rtc_rtcp_bye_ssrcs(const rtc_rtcp_pkt* k, uint32_t* out, unsigned max);
int rtc_rtcp_sdes_cname(const rtc_rtcp_pkt* k, uint32_t* ssrc, const uint8_t** name, size_t* len);

size_t rtc_rtcp_write_sr(uint8_t* out, size_t cap, const rtc_rtcp_sr* sr, const rtc_rtcp_rb* rb, unsigned nrb);
size_t rtc_rtcp_write_rr(uint8_t* out, size_t cap, uint32_t ssrc, const rtc_rtcp_rb* rb, unsigned nrb);
size_t rtc_rtcp_write_sdes(uint8_t* out, size_t cap, uint32_t ssrc, const char* cname, size_t len);
size_t rtc_rtcp_write_bye(uint8_t* out, size_t cap, const uint32_t* ssrc, unsigned n, const char* reason, size_t rlen);
size_t rtc_rtcp_write_nack(uint8_t* out, size_t cap, uint32_t sender, uint32_t media, const uint16_t* seq, unsigned n);
size_t rtc_rtcp_write_pli(uint8_t* out, size_t cap, uint32_t sender, uint32_t media);
size_t rtc_rtcp_write_fir(uint8_t* out, size_t cap, uint32_t sender, uint32_t media, uint8_t seq);
size_t rtc_rtcp_write_remb(uint8_t* out, size_t cap, uint32_t sender, uint64_t bps, const uint32_t* ssrc, unsigned n);

#endif
