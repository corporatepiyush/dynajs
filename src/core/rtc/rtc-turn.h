#ifndef DYN_RTC_TURN_H
#define DYN_RTC_TURN_H

#include "rtc.h"
#include "rtc-stun.h"

#define RTC_TURN_MAX_PERM 16
#define RTC_TURN_MAX_TX 8
#define RTC_TURN_CRED_MAX 129
#define RTC_TURN_CHQ 48

enum {
    RTC_TURN_KIND_STUN,
    RTC_TURN_KIND_TURN
};

enum {
    RTC_TURN_IDLE,
    RTC_TURN_ALLOCATING,
    RTC_TURN_READY,
    RTC_TURN_FAILED,
    RTC_TURN_CLOSED
};

enum {
    RTC_TURN_EV_MAPPED,
    RTC_TURN_EV_RELAYED,
    RTC_TURN_EV_FAILED
};

enum {
    RTC_TX_NONE,
    RTC_TX_BINDING,
    RTC_TX_ALLOCATE,
    RTC_TX_REFRESH,
    RTC_TX_PERM,
    RTC_TX_CHANNEL
};

typedef struct {
    rtc_addr peer;
    uint64_t perm_refresh_us;
    uint64_t chan_refresh_us;
    uint64_t last_use_us;
    uint64_t chan_hold_us;
    uint16_t chan;
    uint8_t perm_ok;
    uint8_t chan_ok;
    uint8_t perm_pending;
    uint8_t chan_pending;
    uint8_t used;
} rtc_turn_perm;

typedef struct {
    rtc_addr peer;
    uint16_t chan;
    uint64_t until_us;
} rtc_turn_chq;

typedef struct {
    uint8_t tid[12];
    uint8_t kind;
    uint8_t tries;
    uint8_t idx;
    uint8_t lifetime0;
    uint32_t rto_us;
    uint64_t next_us;
} rtc_turn_tx;

typedef void (*rtc_turn_send_fn)(void* ud, int sock, const rtc_addr* to, const uint8_t* p, size_t n);
typedef void (*rtc_turn_ev_fn)(void* ud, int idx, int ev, int code);

typedef struct {
    rtc_rng* rng;
    rtc_turn_send_fn send;
    rtc_turn_ev_fn ev;
    void* ud;
    int idx;
    int sock;
    int kind;
    int state;
    int err_code;
    rtc_addr server;
    rtc_addr mapped;
    rtc_addr relayed;
    uint32_t lifetime;
    uint64_t refresh_us;
    uint16_t nonce_len;
    uint8_t have_key;
    uint8_t auth_tries;
    uint8_t key[16];
    char user[RTC_TURN_CRED_MAX];
    char pass[RTC_TURN_CRED_MAX];
    char realm[RTC_TURN_CRED_MAX];
    uint8_t nonce[RTC_TURN_CRED_MAX];
    rtc_turn_tx tx[RTC_TURN_MAX_TX];
    rtc_turn_perm perm[RTC_TURN_MAX_PERM];
    rtc_turn_chq chq[RTC_TURN_CHQ];
    uint16_t chan_next;
} rtc_turn;

void rtc_turn_init(rtc_turn* t, rtc_rng* rng, int idx, int kind, int sock, const rtc_addr* server, const char* user,
    const char* pass, rtc_turn_send_fn send, rtc_turn_ev_fn ev, void* ud);
void rtc_turn_start(rtc_turn* t, uint64_t now);
int rtc_turn_input(rtc_turn* t, const uint8_t* p, size_t n, uint64_t now, rtc_addr* peer, const uint8_t** data,
    size_t* dlen);
int rtc_turn_send(rtc_turn* t, const rtc_addr* peer, const uint8_t* p, size_t n, uint64_t now);
uint64_t rtc_turn_tick(rtc_turn* t, uint64_t now);
void rtc_turn_reset_perms(rtc_turn* t);
void rtc_turn_close(rtc_turn* t, uint64_t now);

#endif
