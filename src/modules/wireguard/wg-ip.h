#ifndef WG_IP_H
#define WG_IP_H

#include <stddef.h>
#include <stdint.h>

enum {
    WG_IP_MTU = 1420,
    WG_IP_MAX_ADDRS = 8,
    WG_IP_MAX_HOOKS = 4,
    WG_IP_EXT_MAX = 8,
    WG_IP_HDR4 = 20,
    WG_IP_HDR6 = 40,
    WG_UDP_HDR = 8,
    WG_IP_TTL = 64,
    WG_IP_EPH_LO = 49152,
    WG_IP_ERR_BURST = 10,
    WG_IP_ERR_EVERY_MS = 100,
    WG_IP_ERR4_MAX = 576,
    WG_IP_ERR6_MAX = 1280
};

enum { WG_PROTO_ICMP = 1, WG_PROTO_TCP = 6, WG_PROTO_UDP = 17, WG_PROTO_ICMP6 = 58 };

typedef struct wg_ip wg_ip_t;
typedef struct wg_udp wg_udp_t;

typedef struct {
    int v6;
    uint8_t a[16];
} wg_ipaddr_t;

typedef int (*wg_ip_out_fn)(void* ud, const uint8_t* pkt, size_t n);
typedef uint64_t (*wg_ip_clock_fn)(void* ud);
typedef void (*wg_ip_proto_fn)(void* ud, const wg_ipaddr_t* src, const wg_ipaddr_t* dst, const uint8_t* l4, size_t n);
typedef void (*wg_udp_rx_fn)(void* ud, wg_udp_t* s, const wg_ipaddr_t* src, uint16_t sport, const uint8_t* data, size_t n);

typedef struct {
    uint64_t rx, rx_bad, rx_csum, rx_frag, rx_not_local, rx_proto, udp_rx, udp_no_port, echo, unreach_sent, unreach_limited, tx, tx_fail, csum_skipped;
} wg_ip_stats_t;

uint64_t wg_csum_acc(uint64_t acc, const uint8_t* p, size_t n);
uint16_t wg_csum_fin(uint64_t acc);
uint64_t wg_csum_pseudo(const wg_ipaddr_t* src, const wg_ipaddr_t* dst, uint8_t proto, size_t len);

wg_ip_t* wg_ip_new(wg_ip_out_fn out, void* ud);
void wg_ip_free(wg_ip_t* ip);
void wg_ip_set_clock(wg_ip_t* ip, wg_ip_clock_fn fn, void* ud);
int wg_ip_add_addr(wg_ip_t* ip, const wg_ipaddr_t* a);
const wg_ipaddr_t* wg_ip_src_for(const wg_ip_t* ip, int v6);
int wg_ip_set_proto(wg_ip_t* ip, uint8_t proto, wg_ip_proto_fn fn, void* ud);
void wg_ip_input(wg_ip_t* ip, const uint8_t* pkt, size_t n);
void wg_ip_input_trusted(wg_ip_t* ip, const uint8_t* pkt, size_t n);
int wg_ip_verify(const uint8_t* pkt, size_t n);
int wg_ip_take_trust(wg_ip_t* ip);
int wg_ip_output(wg_ip_t* ip, const wg_ipaddr_t* src, const wg_ipaddr_t* dst, uint8_t proto, const uint8_t* l4, size_t n);
void wg_ip_stats(const wg_ip_t* ip, wg_ip_stats_t* st);

wg_udp_t* wg_udp_bind(wg_ip_t* ip, int v6, uint16_t port, wg_udp_rx_fn rx, void* ud);
uint16_t wg_udp_port(const wg_udp_t* s);
int wg_udp_sendto(wg_udp_t* s, const wg_ipaddr_t* dst, uint16_t dport, const uint8_t* data, size_t n);
void wg_udp_close(wg_udp_t* s);

#endif
