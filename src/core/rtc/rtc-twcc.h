#ifndef DYN_RTC_TWCC_H
#define DYN_RTC_TWCC_H

#include "rtc-rtcp.h"
#include "rtc-rtp.h"

#define RTC_TWCC_WIN 4096u
#define RTC_TWCC_MAX_STATUS 2048u
#define RTC_TWCC_NONE INT64_MIN

typedef struct {
    int64_t at_us[RTC_TWCC_WIN];
    uint32_t base_ext;
    uint32_t newest_ext;
    uint8_t init;
    uint8_t fb_count;
} rtc_twcc_rx;

typedef struct {
    int64_t at_us;
    uint16_t seq;
    uint8_t received;
} rtc_twcc_pkt;

typedef struct {
    int64_t ref_us;
    uint16_t base;
    uint16_t count;
    uint8_t fb_count;
} rtc_twcc_fb;

void rtc_twcc_rx_init(rtc_twcc_rx* r);
void rtc_twcc_rx_on_packet(rtc_twcc_rx* r, uint16_t tseq, int64_t now_us);
size_t rtc_twcc_rx_build(rtc_twcc_rx* r, uint8_t* out, size_t cap, uint32_t sender, uint32_t media);
int rtc_twcc_parse(const rtc_rtcp_fb* fb, rtc_twcc_fb* h, rtc_twcc_pkt* out, unsigned max);
void rtc_twcc_ext_put(uint8_t* out, uint16_t tseq);
int rtc_twcc_ext_get(const uint8_t* pkt, const rtc_rtp_hdr* h, uint8_t id, uint16_t* tseq);

#endif
