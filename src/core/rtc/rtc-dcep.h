#ifndef DYN_RTC_DCEP_H
#define DYN_RTC_DCEP_H

#include "rtc.h"

#define RTC_DCEP_PPID 50u
#define RTC_DCEP_OPEN 0x03u
#define RTC_DCEP_ACK 0x02u
#define RTC_DCEP_HDR 12u

typedef struct {
    uint8_t type;
    uint8_t ordered;
    uint16_t priority;
    int32_t max_rtx;
    int64_t max_life;
    const uint8_t* label;
    const uint8_t* protocol;
    uint16_t label_len;
    uint16_t protocol_len;
} rtc_dcep_msg;

int rtc_dcep_parse(const uint8_t* p, size_t n, rtc_dcep_msg* m);
size_t rtc_dcep_open_size(size_t label_len, size_t protocol_len);
size_t rtc_dcep_write_open(uint8_t* out, size_t cap, int ordered, int32_t max_rtx, int64_t max_life, uint16_t priority,
    const char* label, size_t label_len, const char* protocol, size_t protocol_len);

#endif
