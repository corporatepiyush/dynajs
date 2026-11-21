#include "rtc-rtpstat.h"

static void rtp_rx_reset(rtc_rtp_rx* s, uint16_t seq)
{
    s->base_seq = seq;
    s->max_seq = seq;
    s->bad_seq = 65536u + 1;
    s->cycles = 0;
    s->received = 0;
    s->received_prior = 0;
    s->expected_prior = 0;
}

void rtc_rtp_rx_init(rtc_rtp_rx* s, uint32_t ssrc, uint16_t seq)
{
    memset(s, 0, sizeof(*s));
    s->ssrc = ssrc;
    rtp_rx_reset(s, seq);
    s->max_seq = (uint16_t)(seq - 1);
}

int rtc_rtp_rx_update(rtc_rtp_rx* s, uint16_t seq)
{
    uint16_t udelta = (uint16_t)(seq - s->max_seq);
    if (udelta < RTC_RTP_MAX_DROPOUT) {
        if (udelta == 0)
            s->dups++;
        if (seq < s->max_seq)
            s->cycles += 65536u;
        s->max_seq = seq;
    } else if (udelta <= 65536u - RTC_RTP_MAX_MISORDER) {
        if (seq == s->bad_seq) {
            rtp_rx_reset(s, seq);
        } else {
            s->bad_seq = (uint32_t)(seq + 1) & 65535u;
            return 0;
        }
    }
    s->received++;
    return 1;
}

void rtc_rtp_rx_arrival(rtc_rtp_rx* s, uint32_t rtp_ts, uint32_t arrival)
{
    uint32_t transit = arrival - rtp_ts;
    int32_t d;
    if (!s->has_transit) {
        s->transit = transit;
        s->has_transit = 1;
        return;
    }
    d = (int32_t)(transit - s->transit);
    s->transit = transit;
    if (d < 0)
        d = d == INT32_MIN ? INT32_MAX : -d;
    s->jitter += (uint32_t)d - ((s->jitter + 8) >> 4);
}

void rtc_rtp_rx_on_sr(rtc_rtp_rx* s, uint64_t ntp, uint64_t now_us)
{
    s->lsr = (uint32_t)(ntp >> 16);
    s->lsr_at_us = now_us;
}

uint32_t rtc_rtp_rx_ext_max(const rtc_rtp_rx* s)
{
    return s->cycles + s->max_seq;
}

int32_t rtc_rtp_rx_lost(const rtc_rtp_rx* s)
{
    int64_t expected = (int64_t)rtc_rtp_rx_ext_max(s) - s->base_seq + 1;
    int64_t lost = expected - s->received;
    if (lost > 0x7FFFFF)
        lost = 0x7FFFFF;
    if (lost < -0x800000)
        lost = -0x800000;
    return (int32_t)lost;
}

void rtc_rtp_rx_report(rtc_rtp_rx* s, uint64_t now_us, rtc_rtcp_rb* rb)
{
    uint32_t expected = rtc_rtp_rx_ext_max(s) - s->base_seq + 1;
    uint32_t ei = expected - s->expected_prior, ri = s->received - s->received_prior;
    int64_t li = (int64_t)ei - (int64_t)ri;
    s->expected_prior = expected;
    s->received_prior = s->received;
    rb->ssrc = s->ssrc;
    rb->fraction = (ei == 0 || li <= 0) ? 0 : (uint8_t)(((uint64_t)li << 8) / ei > 255 ? 255 : ((uint64_t)li << 8) / ei);
    rb->lost = rtc_rtp_rx_lost(s);
    rb->ext_seq = rtc_rtp_rx_ext_max(s);
    rb->jitter = s->jitter >> 4;
    rb->lsr = s->lsr;
    rb->dlsr = s->lsr && now_us > s->lsr_at_us ? (uint32_t)(((now_us - s->lsr_at_us) << 16) / 1000000u) : 0;
}

uint64_t rtc_ntp_from_us(uint64_t unix_us)
{
    uint64_t sec = unix_us / 1000000u + RTC_NTP_UNIX_OFFSET, us = unix_us % 1000000u;
    return (sec << 32) | ((us << 32) / 1000000u);
}

uint64_t rtc_ntp_to_us(uint64_t ntp)
{
    uint64_t sec = ntp >> 32, frac = ntp & 0xFFFFFFFFu;
    if (sec < RTC_NTP_UNIX_OFFSET)
        return 0;
    return (sec - RTC_NTP_UNIX_OFFSET) * 1000000u + ((frac * 1000000u + 0x80000000u) >> 32);
}

int64_t rtc_rtcp_rtt_us(uint64_t now_unix_us, uint32_t lsr, uint32_t dlsr)
{
    uint32_t now = (uint32_t)(rtc_ntp_from_us(now_unix_us) >> 16);
    int32_t r;
    if (!lsr)
        return -1;
    r = (int32_t)(now - lsr - dlsr);
    if (r < 0)
        return 0;
    return (int64_t)(((uint64_t)(uint32_t)r * 1000000u) >> 16);
}
