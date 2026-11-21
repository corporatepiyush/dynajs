#ifndef WG_GLUE_H
#define WG_GLUE_H

#include "wg-internal.h"
#include "wg-ip.h"
#include "wg-tcp.h"
#include "wg-tun.h"

struct dyn_aio;
typedef struct wg_glue wg_glue_t;

typedef struct {
    uint64_t tun_rx, tun_tx, tun_drop, timer_fires, isn_calls;
} wg_glue_stats_t;

uint64_t wg_siphash24(const uint8_t key[16], const uint8_t* m, size_t n);
uint32_t wg_isn_rfc6528(const uint8_t key[16], uint64_t now_us, const wg_ipaddr_t* la, uint16_t lp, const wg_ipaddr_t* ra, uint16_t rp);

wg_glue_t* wg_glue_new(struct dyn_aio* aio, const uint8_t priv[WG_KEY], const char* host, uint16_t port, wg_tun_t* tun);
wg_glue_t* wg_glue_new_static(struct dyn_aio* aio, wg_static_t* s, const char* host, uint16_t port, wg_tun_t* tun);
void wg_glue_free(wg_glue_t* g);
void wg_glue_close(wg_glue_t* g);
void wg_glue_release(wg_glue_t* g, void (*freed)(void* ud), void* ud);
int wg_glue_closed(const wg_glue_t* g);
wg_dev_t* wg_glue_dev(const wg_glue_t* g);
wg_ip_t* wg_glue_ip(const wg_glue_t* g);
wg_tcp_t* wg_glue_tcp(const wg_glue_t* g);
wg_tun_t* wg_glue_tun(const wg_glue_t* g);
void wg_glue_kick(wg_glue_t* g);
void wg_glue_set_isn_key(wg_glue_t* g, const uint8_t key[16]);
void wg_glue_stats(const wg_glue_t* g, wg_glue_stats_t* st);

#endif
