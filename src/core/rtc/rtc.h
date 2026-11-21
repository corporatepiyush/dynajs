#ifndef DYN_RTC_RTC_H
#define DYN_RTC_RTC_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static inline uint16_t rtc_rd16(const uint8_t* p)
{
    return (uint16_t)(((unsigned)p[0] << 8) | p[1]);
}

static inline uint32_t rtc_rd24(const uint8_t* p)
{
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
}

static inline uint32_t rtc_rd32(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static inline uint64_t rtc_rd64(const uint8_t* p)
{
    return ((uint64_t)rtc_rd32(p) << 32) | rtc_rd32(p + 4);
}

static inline void rtc_wr16(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static inline void rtc_wr24(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 16);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)v;
}

static inline void rtc_wr32(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static inline void rtc_wr64(uint8_t* p, uint64_t v)
{
    rtc_wr32(p, (uint32_t)(v >> 32));
    rtc_wr32(p + 4, (uint32_t)v);
}

static inline int rtc_seq_lt32(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) < 0;
}

static inline int rtc_seq_le32(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) <= 0;
}

static inline int rtc_seq_lt16(uint16_t a, uint16_t b)
{
    return (int16_t)(uint16_t)(a - b) < 0;
}

typedef struct {
    uint8_t fam;
    uint8_t pad;
    uint16_t port;
    uint8_t ip[16];
} rtc_addr;

static inline int rtc_addr_eq(const rtc_addr* a, const rtc_addr* b)
{
    if (a->fam != b->fam || a->port != b->port)
        return 0;
    return memcmp(a->ip, b->ip, a->fam == 4 ? 4 : 16) == 0;
}

static inline int rtc_addr_ip_eq(const rtc_addr* a, const rtc_addr* b)
{
    return a->fam == b->fam && memcmp(a->ip, b->ip, a->fam == 4 ? 4 : 16) == 0;
}

int rtc_addr_parse(const char* s, size_t n, uint16_t port, rtc_addr* out);
size_t rtc_addr_fmt(const rtc_addr* a, char* buf, size_t cap);
int rtc_addr_is_loopback(const rtc_addr* a);
int rtc_addr_is_link_local(const rtc_addr* a);
int rtc_addr_host_ok(const rtc_addr* a);

typedef struct {
    uint32_t key[8];
    uint64_t ctr;
    uint8_t buf[64];
    uint32_t pos;
} rtc_rng;

void rtc_chacha20_block(const uint32_t key[8], uint32_t counter, const uint32_t nonce[3], uint8_t out[64]);

void rtc_rng_seed(rtc_rng* r, const uint8_t seed[32]);
uint64_t rtc_rng_next(rtc_rng* r);
uint32_t rtc_rng_u32(rtc_rng* r);
void rtc_rng_bytes(rtc_rng* r, uint8_t* out, size_t n);
void rtc_rng_alnum(rtc_rng* r, char* out, size_t n);

size_t rtc_hex_upper_colon(const uint8_t* p, size_t n, char* out, size_t cap);
int rtc_hex_colon_parse(const char* s, size_t n, uint8_t* out, size_t cap);

#endif
