#ifndef WG_TUN_H
#define WG_TUN_H

#include <stddef.h>
#include <stdint.h>

enum { WG_TUN_FRAME_NONE, WG_TUN_FRAME_AF };

enum { WG_TUN_AF_INET = 2, WG_TUN_AF_INET6 = 30, WG_TUN_HDR = 4, WG_TUN_MTU_DEFAULT = 1420, WG_TUN_MTU_MIN = 1280, WG_TUN_MTU_MAX = 65535, WG_TUN_NAME_MAX = 15 };

enum {
    WG_TUN_ST_OK,
    WG_TUN_ST_UNSUPPORTED,
    WG_TUN_ST_ARG,
    WG_TUN_ST_SOCKET,
    WG_TUN_ST_IOCTL,
    WG_TUN_ST_CONNECT,
    WG_TUN_ST_OPEN,
    WG_TUN_ST_SETIFF,
    WG_TUN_ST_IFNAME,
    WG_TUN_ST_FCNTL,
    WG_TUN_ST_MTU,
    WG_TUN_ST_NOMEM
};

typedef struct {
    int stage, err;
} wg_tun_err_t;

typedef struct {
    uint64_t rx_pkts, rx_bytes, rx_drop_frame, rx_drop_big, tx_pkts, tx_bytes, tx_drop_bad, tx_again, tx_err;
} wg_tun_stats_t;

typedef struct wg_tun wg_tun_t;
typedef void (*wg_tun_rx_fn)(void* ud, const uint8_t* ip, size_t n);

int wg_tun_name_ok(const char* name, int framing);
size_t wg_tun_frame_hdr(int framing, const uint8_t* ip, size_t n, uint8_t hdr[WG_TUN_HDR]);
int wg_tun_unframe(int framing, const uint8_t* p, size_t n, const uint8_t** ip, size_t* ipn);
const char* wg_tun_reason(const wg_tun_err_t* e, char* buf, size_t cap);

wg_tun_t* wg_tun_open(const char* name, unsigned mtu, wg_tun_err_t* e);
wg_tun_t* wg_tun_from_fd(int fd, int framing, const char* name, unsigned mtu, wg_tun_err_t* e);
int wg_tun_fd(const wg_tun_t* t);
int wg_tun_framing(const wg_tun_t* t);
unsigned wg_tun_mtu(const wg_tun_t* t);
const char* wg_tun_name(const wg_tun_t* t);
int wg_tun_read(wg_tun_t* t, unsigned max, wg_tun_rx_fn fn, void* ud);
int wg_tun_write(wg_tun_t* t, const uint8_t* ip, size_t n);
void wg_tun_stats(const wg_tun_t* t, wg_tun_stats_t* st);
void wg_tun_close(wg_tun_t* t);

#endif
