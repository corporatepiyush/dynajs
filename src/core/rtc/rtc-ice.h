#ifndef DYN_RTC_ICE_H
#define DYN_RTC_ICE_H

#include "rtc.h"
#include "rtc-stun.h"

#define RTC_ICE_MAX_LOCAL 16
#define RTC_ICE_MAX_REMOTE 32
#define RTC_ICE_MAX_PAIRS 128
#define RTC_ICE_CRED_MAX 257
#define RTC_ICE_TA_US 50000u
#define RTC_ICE_RC 7
#define RTC_ICE_CONSENT_EXPIRE_US 30000000u
#define RTC_ICE_DISCONNECT_US 7500000u
#define RTC_ICE_ERR_BURST 20u

enum {
    RTC_CAND_HOST,
    RTC_CAND_SRFLX,
    RTC_CAND_PRFLX,
    RTC_CAND_RELAY
};

enum {
    RTC_PAIR_FROZEN,
    RTC_PAIR_WAITING,
    RTC_PAIR_INPROGRESS,
    RTC_PAIR_SUCCEEDED,
    RTC_PAIR_FAILED
};

enum {
    RTC_PF_VALID = 1,
    RTC_PF_NOMINATED = 2,
    RTC_PF_USE_CAND = 4,
    RTC_PF_NOM_ON_OK = 8,
    RTC_PF_TRIGGERED = 16,
    RTC_PF_CONSENT = 32,
    RTC_PF_SENT_CTL = 64,
    RTC_PF_AUTH_RX = 128
};

enum {
    RTC_ICE_NEW,
    RTC_ICE_CHECKING,
    RTC_ICE_CONNECTED,
    RTC_ICE_COMPLETED,
    RTC_ICE_DISCONNECTED,
    RTC_ICE_FAILED,
    RTC_ICE_CLOSED
};

enum {
    RTC_ICE_EV_STATE,
    RTC_ICE_EV_SELECTED,
    RTC_ICE_EV_ROLE
};

typedef struct {
    rtc_addr addr;
    rtc_addr base;
    rtc_addr rel;
    uint32_t prio;
    uint32_t found;
    uint8_t type;
    uint8_t comp;
    uint8_t sock;
    uint8_t relay;
    uint8_t off;
} rtc_cand;

typedef struct {
    uint64_t prio;
    uint64_t next_us;
    uint64_t sent_us;
    uint64_t last_rx_us;
    uint64_t req_tx, req_rx, rsp_tx, rsp_rx;
    uint32_t rto_us;
    uint32_t rtt_us;
    uint32_t rtt_total_us;
    uint8_t tid[12];
    uint8_t tid2[12];
    uint8_t l, r, state, flags, tries;
} rtc_pair;

typedef struct {
    rtc_cand l, r;
    uint64_t last_consent_us;
    uint64_t consent_next_us;
    uint8_t tid[12];
    uint8_t tid2[12];
    uint8_t controlling;
    char lufrag[RTC_ICE_CRED_MAX], lpwd[RTC_ICE_CRED_MAX];
    char rufrag[RTC_ICE_CRED_MAX], rpwd[RTC_ICE_CRED_MAX];
} rtc_ice_prev;

typedef void (*rtc_ice_send_fn)(void* ud, const rtc_cand* local, const rtc_addr* to, const uint8_t* p, size_t n);
typedef void (*rtc_ice_ev_fn)(void* ud, int ev);

typedef struct {
    rtc_rng* rng;
    rtc_ice_send_fn send;
    rtc_ice_ev_fn ev;
    void* ud;
    uint64_t tiebreaker;
    uint64_t next_ta_us;
    uint64_t first_valid_us;
    uint64_t consent_next_us;
    uint64_t last_consent_us;
    uint64_t started_us;
    int controlling;
    int state;
    int selected;
    int nominating;
    int started;
    int remote_done;
    int local_done;
    int relay_only;
    int nlocal, nremote, npairs;
    int ntrig;
    uint8_t trig[RTC_ICE_MAX_PAIRS];
    char lufrag[RTC_ICE_CRED_MAX], lpwd[RTC_ICE_CRED_MAX];
    char rufrag[RTC_ICE_CRED_MAX], rpwd[RTC_ICE_CRED_MAX];
    char stage_ufrag[RTC_ICE_CRED_MAX], stage_pwd[RTC_ICE_CRED_MAX];
    rtc_cand local[RTC_ICE_MAX_LOCAL];
    rtc_cand remote[RTC_ICE_MAX_REMOTE];
    rtc_pair pairs[RTC_ICE_MAX_PAIRS];
    uint64_t err_refill_us;
    uint32_t err_tokens;
    int have_prev;
    int prev_rx;
    rtc_ice_prev prev;
} rtc_ice;

uint32_t rtc_ice_cand_prio(int type, uint32_t local_pref, int comp);
uint32_t rtc_ice_foundation(int type, const rtc_addr* base, const char* server);

void rtc_ice_init(rtc_ice* ice, rtc_rng* rng, int controlling, rtc_ice_send_fn send, rtc_ice_ev_fn ev, void* ud);
void rtc_ice_set_local_creds(rtc_ice* ice, const char* ufrag, const char* pwd);
void rtc_ice_stage_local(rtc_ice* ice, const char* ufrag, const char* pwd);
int rtc_ice_set_remote_creds(rtc_ice* ice, const char* ufrag, const char* pwd);
void rtc_ice_set_role(rtc_ice* ice, int controlling);
int rtc_ice_add_local(rtc_ice* ice, const rtc_cand* c);
int rtc_ice_add_remote(rtc_ice* ice, const rtc_cand* c);
void rtc_ice_start(rtc_ice* ice, uint64_t now);
int rtc_ice_input(rtc_ice* ice, int local, const rtc_addr* from, const rtc_stun_msg* m, uint64_t now);
uint64_t rtc_ice_tick(rtc_ice* ice, uint64_t now);
int rtc_ice_get_selected(const rtc_ice* ice, const rtc_cand** l, const rtc_cand** r);
void rtc_ice_restart(rtc_ice* ice, const char* ufrag, const char* pwd);
void rtc_ice_close(rtc_ice* ice);
int rtc_ice_find_local(const rtc_ice* ice, int sock, int relay);
int rtc_ice_peer_ok(const rtc_ice* ice, int local, const rtc_addr* from);
void rtc_ice_note_rx(rtc_ice* ice, const rtc_addr* from);

#endif
