#ifndef DYN_DNS_H
#define DYN_DNS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DYN_DNS_MAX_NAME 255
#define DYN_DNS_MAX_LABEL 63
#define DYN_DNS_HDR_LEN 12

#define DYN_DNS_T_A 1
#define DYN_DNS_T_NS 2
#define DYN_DNS_T_CNAME 5
#define DYN_DNS_T_SOA 6
#define DYN_DNS_T_PTR 12
#define DYN_DNS_T_MX 15
#define DYN_DNS_T_TXT 16
#define DYN_DNS_T_AAAA 28
#define DYN_DNS_C_IN 1

typedef struct {
    uint16_t id, flags;
    uint16_t qdcount, ancount, nscount, arcount;
} dyn_dns_hdr_t;

typedef struct {
    char name[DYN_DNS_MAX_NAME + 1];
    uint16_t type, cls;
    uint32_t ttl;
    const uint8_t* rdata;
    uint16_t rdlen;
} dyn_dns_rr_t;

#define DYN_DNS_OK 0
#define DYN_DNS_E_SHORT -1
#define DYN_DNS_E_LOOP -2
#define DYN_DNS_E_NAME -3
#define DYN_DNS_E_FORMAT -4

int dyn_dns_name_decode(const uint8_t* msg, size_t len, size_t off,
    char* out, size_t outcap);

int dyn_dns_name_encode(const char* name, uint8_t* out, size_t outcap);

int dyn_dns_hdr_decode(const uint8_t* msg, size_t len, dyn_dns_hdr_t* h);
int dyn_dns_hdr_encode(const dyn_dns_hdr_t* h, uint8_t* out, size_t outcap);

int dyn_dns_build_query(uint16_t id, const char* name, uint16_t type,
    uint8_t* out, size_t outcap);

int dyn_dns_skip_questions(const uint8_t* msg, size_t len, size_t* off,
    uint16_t qdcount);

int dyn_dns_rr_decode(const uint8_t* msg, size_t len, size_t* off,
    dyn_dns_rr_t* rr);

int dyn_dns_begin_response(const uint8_t* query, size_t qlen, int rcode,
    uint8_t* out, size_t outcap);

int dyn_dns_add_answer(uint8_t* out, size_t outcap, size_t off, uint16_t type,
    uint32_t ttl, const uint8_t* addr, uint16_t addrlen);

void dyn_dns_set_ancount(uint8_t* out, uint16_t n);

const char* dyn_dns_strerror(int code);

#ifdef __cplusplus
}
#endif

#endif
