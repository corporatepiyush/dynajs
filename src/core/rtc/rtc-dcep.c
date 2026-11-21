#include "rtc-dcep.h"

#include <string.h>

int rtc_dcep_parse(const uint8_t* p, size_t n, rtc_dcep_msg* m)
{
    uint8_t ct;
    uint32_t rel;
    memset(m, 0, sizeof(*m));
    m->max_rtx = -1;
    m->max_life = -1;
    if (!n)
        return -1;
    m->type = p[0];
    if (p[0] == RTC_DCEP_ACK)
        return 0;
    if (p[0] != RTC_DCEP_OPEN || n < RTC_DCEP_HDR)
        return -1;
    ct = p[1];
    m->priority = rtc_rd16(p + 2);
    rel = rtc_rd32(p + 4);
    m->label_len = rtc_rd16(p + 8);
    m->protocol_len = rtc_rd16(p + 10);
    if ((size_t)RTC_DCEP_HDR + m->label_len + m->protocol_len > n)
        return -1;
    switch (ct & 0x7f) {
    case 0x00:
        break;
    case 0x01:
        m->max_rtx = (int32_t)(rel > 65535 ? 65535 : rel);
        break;
    case 0x02:
        m->max_life = rel;
        break;
    default:
        return -1;
    }
    m->ordered = (ct & 0x80) ? 0 : 1;
    m->label = p + RTC_DCEP_HDR;
    m->protocol = p + RTC_DCEP_HDR + m->label_len;
    return 0;
}

size_t rtc_dcep_open_size(size_t label_len, size_t protocol_len)
{
    return RTC_DCEP_HDR + label_len + protocol_len;
}

size_t rtc_dcep_write_open(uint8_t* out, size_t cap, int ordered, int32_t max_rtx, int64_t max_life, uint16_t priority,
    const char* label, size_t label_len, const char* protocol, size_t protocol_len)
{
    uint8_t ct = 0;
    uint32_t rel = 0;
    size_t n = rtc_dcep_open_size(label_len, protocol_len);
    if (label_len > 65535 || protocol_len > 65535 || n > cap)
        return 0;
    if (max_rtx >= 0) {
        ct = 0x01;
        rel = (uint32_t)max_rtx;
    } else if (max_life >= 0) {
        ct = 0x02;
        rel = (uint32_t)(max_life > (int64_t)UINT32_MAX ? UINT32_MAX : (uint64_t)max_life);
    }
    if (!ordered)
        ct |= 0x80;
    out[0] = RTC_DCEP_OPEN;
    out[1] = ct;
    rtc_wr16(out + 2, priority);
    rtc_wr32(out + 4, rel);
    rtc_wr16(out + 8, (uint32_t)label_len);
    rtc_wr16(out + 10, (uint32_t)protocol_len);
    if (label_len)
        memcpy(out + RTC_DCEP_HDR, label, label_len);
    if (protocol_len)
        memcpy(out + RTC_DCEP_HDR + label_len, protocol, protocol_len);
    return n;
}
