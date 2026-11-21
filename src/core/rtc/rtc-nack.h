#ifndef DYN_RTC_NACK_H
#define DYN_RTC_NACK_H

#include "rtc-pktring.h"
#include "rtc-rtcp.h"

#define RTC_NACK_MAX 1000u
#define RTC_NACK_MAX_AGE 10000u
#define RTC_NACK_MAX_TRIES 10u
#define RTC_NACK_MIN_INTERVAL_MS 10u

typedef struct {
    uint32_t ext;
    uint32_t sent_ms;
    uint8_t tries;
} rtc_nack_ent;

typedef struct {
    rtc_nack_ent e[RTC_NACK_MAX];
    uint64_t requested;
    uint64_t unique;
    uint64_t recovered;
    uint32_t n;
    uint32_t newest;
    uint8_t init;
    uint8_t want_key;
} rtc_nack;

void rtc_nack_init(rtc_nack* k);
int rtc_nack_on_packet(rtc_nack* k, uint16_t seq);
unsigned rtc_nack_poll(rtc_nack* k, uint32_t now_ms, uint32_t rtt_ms, uint16_t* out, unsigned max);
int rtc_nack_take_keyreq(rtc_nack* k);
unsigned rtc_nack_answer(const rtc_pktring* h, const uint16_t* seq, unsigned n, const uint8_t** pkt, size_t* len);

typedef struct {
    uint32_t sent_ms;
    uint32_t gap_ms;
    uint64_t sent;
    uint8_t next_seq;
    uint8_t cur_seq;
    uint8_t pending;
    uint8_t active;
} rtc_keyreq;

void rtc_keyreq_init(rtc_keyreq* k, uint32_t gap_ms, uint8_t first_seq);
void rtc_keyreq_want(rtc_keyreq* k);
void rtc_keyreq_done(rtc_keyreq* k);
size_t rtc_keyreq_poll(rtc_keyreq* k, uint32_t now_ms, int fir, uint32_t sender, uint32_t media, uint8_t* out, size_t cap);

typedef struct {
    uint8_t seq;
    uint8_t has;
} rtc_fir_rx;

int rtc_fir_rx_new(rtc_fir_rx* r, uint8_t seq);

#endif
