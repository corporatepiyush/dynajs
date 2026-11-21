#ifndef WG_MOD_H
#define WG_MOD_H

#include "../../cutils.h"
#include "../../dyna-nat.h"
#include "../../dyna-aio.h"
#include "wg-conf.h"
#include "wg-glue.h"

enum { WGM_CONN = 1, WGM_LIS, WGM_UDP };
enum { WGM_RS_HWM = 262144, WGM_UDP_MAX = 65507 };

typedef struct wgm wgm_t;

typedef struct wgm_peer {
    struct wgm_peer *next, *prev;
    wgm_t* w;
    wg_dpeer_t* p;
    uint8_t pub[32];
    JSValue res, rej;
    int pending, initial, linked;
} wgm_peer_t;

typedef struct {
    void* p;
    uint32_t gen;
    int kind;
} wgm_slot_t;

typedef struct wgm_conn {
    wgm_t* w;
    wg_tconn_t* tc;
    uint32_t id;
    JSValue ctl, wctl, dres, drej, wres, wrej, cres, crej;
    uint8_t* pend;
    size_t pn, poff;
    int rdone, paused, fin_wanted, dialing;
} wgm_conn_t;

typedef struct wgm_lis {
    wgm_t* w;
    wg_tlisten_t *l4, *l6;
    uint32_t id;
    uint16_t port;
    JSValue handler;
} wgm_lis_t;

typedef struct wgm_udp {
    wgm_t* w;
    wg_udp_t *u4, *u6;
    uint32_t id;
    uint16_t port;
    JSValue handler;
} wgm_udp_t;

struct wgm {
    JSContext* ctx;
    JSRuntime* rt;
    dyn_aio_t* aio;
    wg_glue_t* g;
    JSValue obj, cres, crej;
    wgm_peer_t* peers;
    unsigned npeers, create_left;
    wgm_slot_t* slots;
    uint32_t nslots, cap;
    uint16_t port;
    int has4, has6, closing, in_create, holds_reactor;
    char tun[16];
    uint8_t pub[32];
};

extern JSClassID wgm_class_id;
extern _Thread_local int wgm_in_final;

wgm_t* wgm_of(JSValueConst obj);
JSValue wgm_err(JSContext* ctx, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void wgm_settle(wgm_t* w, JSValue* res, JSValue* rej, int reject, JSValue v);
void wgm_call_error(wgm_t* w);
int wgm_slot_add(wgm_t* w, int kind, void* p, uint32_t* id);
void* wgm_slot_get(wgm_t* w, int kind, uint32_t id);
void wgm_slot_del(wgm_t* w, uint32_t id);
int wgm_bytes(JSContext* ctx, JSValueConst v, const uint8_t** p, size_t* n, const char** cstr, const char* who);
int wgm_ip_arg(JSContext* ctx, JSValueConst v, wg_ipaddr_t* a, const char* who);
int wgm_port_arg(JSContext* ctx, JSValueConst v, int lo, uint16_t* port, const char* who);
JSValue wgm_ip_str(JSContext* ctx, const wg_ipaddr_t* a);
void wgm_kick(wgm_t* w);

JSValue wgm_dial(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue wgm_listen(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue wgm_udp(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
void wgm_conns_shutdown(wgm_t* w);
void wgm_conns_mark(JSRuntime* rt, wgm_t* w, JS_MarkFunc* mf);

#endif
