#ifndef DYN_RTC_PKTRING_H
#define DYN_RTC_PKTRING_H

#include "rtc.h"

typedef struct {
    uint64_t off;
    uint32_t len;
    uint32_t ts;
    uint32_t at_ms;
    uint16_t seq;
    uint8_t used;
    uint8_t marker;
} rtc_pkt_slot;

typedef struct {
    uint8_t* buf;
    rtc_pkt_slot* slot;
    uint64_t head;
    size_t cap;
    uint32_t mask;
    uint32_t count;
} rtc_pktring;

int rtc_pktring_init(rtc_pktring* r, size_t bytes, uint32_t nslot);
void rtc_pktring_free(rtc_pktring* r);
void rtc_pktring_clear(rtc_pktring* r);
int rtc_pktring_put(rtc_pktring* r, uint16_t seq, uint32_t ts, int marker, uint32_t at_ms, const uint8_t* p, size_t n);
const uint8_t* rtc_pktring_get(const rtc_pktring* r, uint16_t seq, const rtc_pkt_slot** s);
void rtc_pktring_drop(rtc_pktring* r, uint16_t seq);
size_t rtc_pktring_bytes(const rtc_pktring* r);

#endif
