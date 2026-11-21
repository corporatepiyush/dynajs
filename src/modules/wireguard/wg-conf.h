#ifndef WG_CONF_H
#define WG_CONF_H

#include <stddef.h>
#include <stdint.h>

enum { WG_CONF_IFACE = 1, WG_CONF_PEER = 2 };

enum {
    WG_CK_PRIVATEKEY,
    WG_CK_LISTENPORT,
    WG_CK_ADDRESS,
    WG_CK_MTU,
    WG_CK_PUBLICKEY,
    WG_CK_PRESHAREDKEY,
    WG_CK_ENDPOINT,
    WG_CK_ALLOWEDIPS,
    WG_CK_KEEPALIVE,
    WG_CK_IGNORED,
    WG_CK_SECTION,
    WG_CK_N
};

enum { WG_CONF_LINE_MAX = 4096, WG_CONF_KEY_B64 = 44 };

typedef struct {
    unsigned line;
    const char* msg;
} wg_conf_err_t;

typedef int (*wg_conf_fn)(void* ud, int section, int key, const char* v, size_t vn, unsigned line);

int wg_conf_scan(const char* s, size_t n, wg_conf_fn fn, void* ud, wg_conf_err_t* e);
int wg_conf_key(const char* s, size_t n, uint8_t out[32]);
void wg_conf_key_b64(const uint8_t k[32], char out[WG_CONF_KEY_B64 + 1]);
int wg_conf_ip(const char* s, size_t n, int* v6, uint8_t addr[16]);
int wg_conf_cidr(const char* s, size_t n, int* v6, uint8_t addr[16], unsigned* cidr);
int wg_conf_endpoint(const char* s, size_t n, int* v6, uint8_t addr[16], uint16_t* port);
int wg_conf_uint(const char* s, size_t n, unsigned max, unsigned* out);
int wg_conf_next(const char* s, size_t n, size_t* off, const char** item, size_t* in);
void wg_conf_fmt_ip(int v6, const uint8_t addr[16], char out[48]);

#endif
