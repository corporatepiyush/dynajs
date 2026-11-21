#ifndef DYN_RTC_RTP_H
#define DYN_RTC_RTP_H

#include "rtc.h"

#define RTC_RTP_MIN 12u
#define RTC_RTP_EXT_ONE 0xBEDEu
#define RTC_RTP_EXT_TWO 0x1000u
#define RTC_RTP_EXT_MAX 16u

typedef struct {
    uint32_t ts;
    uint32_t ssrc;
    uint16_t seq;
    uint8_t pt;
    uint8_t marker;
    uint8_t cc;
    uint8_t pad;
    uint16_t hlen;
    uint16_t ext_profile;
    uint16_t ext_off;
    uint16_t ext_len;
    uint32_t plen;
} rtc_rtp_hdr;

typedef struct {
    const uint8_t* val;
    uint8_t id;
    uint8_t len;
} rtc_rtp_ext;

static inline int rtc_rtp_is_rtcp(const uint8_t* p, size_t n)
{
    return n >= 8 && (p[0] >> 6) == 2 && p[1] >= 192 && p[1] <= 223;
}

static inline int rtc_rtp_is_media(const uint8_t* p, size_t n)
{
    return n >= RTC_RTP_MIN && p[0] >= 128 && p[0] <= 191;
}

int rtc_rtp_parse(const uint8_t* p, size_t n, rtc_rtp_hdr* h);
unsigned rtc_rtp_ext_list(const uint8_t* p, const rtc_rtp_hdr* h, rtc_rtp_ext* out, unsigned max);
int rtc_rtp_ext_get(const uint8_t* p, const rtc_rtp_hdr* h, uint8_t id, const uint8_t** val, size_t* len);
size_t rtc_rtp_hdr_size(unsigned cc, const rtc_rtp_ext* ext, unsigned next);
size_t rtc_rtp_write_hdr(uint8_t* out, size_t cap, uint8_t pt, int marker, uint16_t seq, uint32_t ts, uint32_t ssrc,
    const uint32_t* csrc, unsigned cc, const rtc_rtp_ext* ext, unsigned next);
size_t rtc_rtp_write(uint8_t* out, size_t cap, uint8_t pt, int marker, uint16_t seq, uint32_t ts, uint32_t ssrc,
    const uint32_t* csrc, unsigned cc, const rtc_rtp_ext* ext, unsigned next, const uint8_t* payload, size_t plen);
uint8_t* rtc_rtp_ext_slot(uint8_t* p, const rtc_rtp_hdr* h, uint8_t id, size_t len);

#endif
