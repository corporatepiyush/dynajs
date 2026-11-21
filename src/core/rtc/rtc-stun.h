#ifndef DYN_RTC_STUN_H
#define DYN_RTC_STUN_H

#include "rtc.h"

#define RTC_STUN_MAGIC 0x2112A442u
#define RTC_STUN_MAX_ATTR 40

enum {
    RTC_STUN_BINDING = 0x0001,
    RTC_STUN_ALLOCATE = 0x0003,
    RTC_STUN_REFRESH = 0x0004,
    RTC_STUN_SEND = 0x0006,
    RTC_STUN_DATA = 0x0007,
    RTC_STUN_CREATE_PERMISSION = 0x0008,
    RTC_STUN_CHANNEL_BIND = 0x0009
};

enum {
    RTC_STUN_REQUEST = 0x0000,
    RTC_STUN_INDICATION = 0x0010,
    RTC_STUN_SUCCESS = 0x0100,
    RTC_STUN_ERROR = 0x0110
};

enum {
    RTC_SA_MAPPED_ADDRESS = 0x0001,
    RTC_SA_USERNAME = 0x0006,
    RTC_SA_MESSAGE_INTEGRITY = 0x0008,
    RTC_SA_ERROR_CODE = 0x0009,
    RTC_SA_UNKNOWN_ATTRIBUTES = 0x000A,
    RTC_SA_CHANNEL_NUMBER = 0x000C,
    RTC_SA_LIFETIME = 0x000D,
    RTC_SA_XOR_PEER_ADDRESS = 0x0012,
    RTC_SA_DATA = 0x0013,
    RTC_SA_REALM = 0x0014,
    RTC_SA_NONCE = 0x0015,
    RTC_SA_XOR_RELAYED_ADDRESS = 0x0016,
    RTC_SA_REQUESTED_TRANSPORT = 0x0019,
    RTC_SA_MESSAGE_INTEGRITY_SHA256 = 0x001C,
    RTC_SA_XOR_MAPPED_ADDRESS = 0x0020,
    RTC_SA_PRIORITY = 0x0024,
    RTC_SA_USE_CANDIDATE = 0x0025,
    RTC_SA_SOFTWARE = 0x8022,
    RTC_SA_FINGERPRINT = 0x8028,
    RTC_SA_ICE_CONTROLLED = 0x8029,
    RTC_SA_ICE_CONTROLLING = 0x802A
};

typedef struct {
    uint16_t type;
    uint16_t len;
    uint16_t off;
} rtc_stun_attr;

typedef struct {
    const uint8_t* buf;
    size_t len;
    uint16_t type;
    uint8_t tid[12];
    uint8_t nattr;
    int8_t mi;
    int8_t fp;
    rtc_stun_attr attr[RTC_STUN_MAX_ATTR];
} rtc_stun_msg;

static inline uint16_t rtc_stun_method(uint16_t type)
{
    return (uint16_t)((type & 0x000F) | ((type & 0x00E0) >> 1) | ((type & 0x3E00) >> 2));
}

static inline uint16_t rtc_stun_class(uint16_t type)
{
    return (uint16_t)(type & 0x0110);
}

static inline uint16_t rtc_stun_type(uint16_t method, uint16_t cls)
{
    return (uint16_t)((method & 0x000F) | ((method & 0x0070) << 1) | ((method & 0x0F80) << 2) | cls);
}

int rtc_stun_is(const uint8_t* p, size_t n);
int rtc_stun_parse(rtc_stun_msg* m, const uint8_t* p, size_t n);
const rtc_stun_attr* rtc_stun_find(const rtc_stun_msg* m, uint16_t type);
int rtc_stun_check_fingerprint(const rtc_stun_msg* m);
int rtc_stun_check_integrity(const rtc_stun_msg* m, const uint8_t* key, size_t keylen);
int rtc_stun_get_addr(const rtc_stun_msg* m, const rtc_stun_attr* a, rtc_addr* out);
int rtc_stun_get_u32(const rtc_stun_msg* m, uint16_t type, uint32_t* out);
int rtc_stun_get_u64(const rtc_stun_msg* m, uint16_t type, uint64_t* out);
int rtc_stun_error_code(const rtc_stun_msg* m);
int rtc_stun_unknown_required(const rtc_stun_msg* m, const uint16_t* known, int nknown, uint16_t* out, int cap);

typedef struct {
    uint8_t* buf;
    size_t cap;
    size_t len;
    int err;
} rtc_stun_wr;

void rtc_stun_wr_init(rtc_stun_wr* w, uint8_t* buf, size_t cap, uint16_t type, const uint8_t tid[12]);
uint8_t* rtc_stun_wr_attr(rtc_stun_wr* w, uint16_t type, const void* data, size_t len);
void rtc_stun_wr_u32(rtc_stun_wr* w, uint16_t type, uint32_t v);
void rtc_stun_wr_u64(rtc_stun_wr* w, uint16_t type, uint64_t v);
void rtc_stun_wr_addr(rtc_stun_wr* w, uint16_t type, const rtc_addr* a);
void rtc_stun_wr_error(rtc_stun_wr* w, int code, const char* reason);
void rtc_stun_wr_integrity(rtc_stun_wr* w, const uint8_t* key, size_t keylen);
void rtc_stun_wr_fingerprint(rtc_stun_wr* w);
size_t rtc_stun_wr_done(rtc_stun_wr* w);

void rtc_stun_long_term_key(const char* user, const char* realm, const char* pass, uint8_t out[16]);

#endif
