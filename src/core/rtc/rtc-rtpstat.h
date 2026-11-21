#ifndef DYN_RTC_RTPSTAT_H
#define DYN_RTC_RTPSTAT_H

#include "rtc-rtcp.h"

#define RTC_RTP_MAX_DROPOUT 3000u
#define RTC_RTP_MAX_MISORDER 100u
#define RTC_NTP_UNIX_OFFSET 2208988800ull

typedef struct {
    uint64_t bytes;
    uint64_t lsr_at_us;
    uint32_t ssrc;
    uint32_t cycles;
    uint32_t base_seq;
    uint32_t bad_seq;
    uint32_t received;
    uint32_t expected_prior;
    uint32_t received_prior;
    uint32_t jitter;
    uint32_t transit;
    uint32_t lsr;
    uint32_t dups;
    uint16_t max_seq;
    uint8_t has_transit;
    uint8_t pad;
} rtc_rtp_rx;

void rtc_rtp_rx_init(rtc_rtp_rx* s, uint32_t ssrc, uint16_t seq);
int rtc_rtp_rx_update(rtc_rtp_rx* s, uint16_t seq);
void rtc_rtp_rx_arrival(rtc_rtp_rx* s, uint32_t rtp_ts, uint32_t arrival);
void rtc_rtp_rx_on_sr(rtc_rtp_rx* s, uint64_t ntp, uint64_t now_us);
void rtc_rtp_rx_report(rtc_rtp_rx* s, uint64_t now_us, rtc_rtcp_rb* rb);
uint32_t rtc_rtp_rx_ext_max(const rtc_rtp_rx* s);
int32_t rtc_rtp_rx_lost(const rtc_rtp_rx* s);
uint64_t rtc_ntp_from_us(uint64_t unix_us);
uint64_t rtc_ntp_to_us(uint64_t ntp);
int64_t rtc_rtcp_rtt_us(uint64_t now_unix_us, uint32_t lsr, uint32_t dlsr);

#endif
