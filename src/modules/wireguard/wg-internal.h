#ifndef WG_INTERNAL_H
#define WG_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#define WG_KEY 32
#define WG_TAG 16
#define WG_MACLEN 16
#define WG_INIT_LEN 148
#define WG_RESP_LEN 92
#define WG_COOKIE_LEN 64
#define WG_DATA_HDR 16
#define WG_MSG_INIT 1
#define WG_MSG_RESP 2
#define WG_MSG_COOKIE 3
#define WG_MSG_DATA 4
#define WG_COOKIE_SECRET_MS 120000u
#define WG_COOKIE_VALID_MS 115000u
#define WG_INIT_MIN_GAP_MS 20u
#define WG_REJECT_AFTER_MESSAGES (UINT64_MAX - 8192u)
#define WG_RW_WORDS 32
#define WG_RW_WINDOW (WG_RW_WORDS * 64 - 64)
#define WG_REKEY_AFTER_MESSAGES (1ULL << 60)
#define WG_REKEY_AFTER_MS 120000u
#define WG_REJECT_AFTER_MS 180000u
#define WG_REKEY_ATTEMPT_MS 90000u
#define WG_REKEY_TIMEOUT_MS 5000u
#define WG_REKEY_JITTER_MS 334u
#define WG_MAX_TIMER_HANDSHAKES (WG_REKEY_ATTEMPT_MS / WG_REKEY_TIMEOUT_MS)
#define WG_KEEPALIVE_MS 10000u
#define WG_ZERO_KEYS_MS (WG_REJECT_AFTER_MS * 3u)
#define WG_PAD 16u
#define WG_DATA_MIN (WG_DATA_HDR + WG_TAG)
#define WG_BATCH_MAX 64
#define WG_HS_QUEUE 1024u
#define WG_UNDER_LOAD_QUEUE (WG_HS_QUEUE / 8u)
#define WG_UNDER_LOAD_MS 1000u

enum {
    WG_HS_NONE = 0,
    WG_HS_CREATED_INIT,
    WG_HS_CONSUMED_INIT,
    WG_HS_CREATED_RESP,
    WG_HS_CONSUMED_RESP
};

enum {
    WG_MAC_OK = 0,
    WG_MAC_BAD = -1,
    WG_MAC_NEED_COOKIE = 1
};

typedef struct {
    uint32_t h[8];
    uint32_t t[2];
    uint8_t buf[64];
    size_t buflen;
    size_t outlen;
} wg_b2s_t;

typedef struct {
    uint8_t priv[WG_KEY];
    uint8_t pub[WG_KEY];
    uint8_t hash0[WG_KEY];
    uint8_t mac1_key[WG_KEY];
    uint8_t cookie_key[WG_KEY];
    void* pk;
} wg_static_t;

typedef struct {
    int state;
    uint8_t ck[WG_KEY];
    uint8_t h[WG_KEY];
    uint8_t e_priv[WG_KEY];
    uint8_t e_pub[WG_KEY];
    uint8_t remote_e[WG_KEY];
    uint32_t local_index;
    uint32_t remote_index;
} wg_handshake_t;

typedef struct {
    uint8_t pub[WG_KEY];
    uint8_t psk[WG_KEY];
    uint8_t ss[WG_KEY];
    uint8_t hash0[WG_KEY];
    uint8_t mac1_key[WG_KEY];
    uint8_t cookie_key[WG_KEY];
    uint8_t cookie[WG_MACLEN];
    uint64_t cookie_ms;
    int has_cookie;
    uint8_t last_mac1[WG_MACLEN];
    int has_last_mac1;
    uint8_t greatest_ts[12];
    uint64_t last_init_ms;
    int has_last_init;
    wg_handshake_t hs;
} wg_peer_t;

typedef struct {
    uint8_t secret[WG_KEY];
    uint64_t secret_ms;
    int has_secret;
} wg_cookie_gen_t;

typedef struct {
    uint64_t next;
    uint64_t bits[WG_RW_WORDS];
} wg_replay_t;

typedef struct {
    uint8_t ck[WG_KEY];
    uint8_t h[WG_KEY];
} wg_sym_t;

typedef struct wg_keypair {
    uint8_t send[WG_KEY];
    uint8_t recv[WG_KEY];
    uint64_t send_ctr;
    uint64_t created_ms;
    wg_replay_t replay;
    uint32_t local_index;
    uint32_t remote_index;
    uint32_t refs;
    uint8_t initiator;
} wg_keypair_t;

typedef struct {
    wg_keypair_t* prev;
    wg_keypair_t* cur;
    wg_keypair_t* next;
} wg_slots_t;

typedef void (*wg_kp_drop_fn)(void* ud, wg_keypair_t* kp);

typedef struct {
    uint8_t* buf;
    uint32_t len;
    uint32_t cap;
    uint64_t ctr;
    int32_t res;
    uint8_t verified;
} wg_pkt_t;

typedef struct wg_batch {
    struct wg_batch* next;
    wg_keypair_t* kp;
    void* owner;
    uint8_t key[WG_KEY];
    uint32_t remote_index;
    uint16_t n;
    uint16_t mtu;
    uint8_t enc;
    uint8_t done;
    wg_pkt_t pkt[WG_BATCH_MAX];
} wg_batch_t;

typedef struct {
    wg_batch_t* head;
    wg_batch_t* tail;
} wg_fifo_t;

enum {
    WG_ACT_SEND_INIT = 1,
    WG_ACT_RETRY = 2,
    WG_ACT_SEND_KEEPALIVE = 4,
    WG_ACT_ZERO_KEYS = 8,
    WG_ACT_GIVE_UP = 16
};

typedef struct {
    uint64_t retransmit_at;
    uint64_t keepalive_at;
    uint64_t new_hs_at;
    uint64_t zero_at;
    uint64_t persist_at;
    uint64_t last_hs_sent_ms;
    uint64_t last_hs_done_ms;
    uint32_t attempts;
    uint16_t persist_s;
    uint8_t has_hs_sent;
    uint8_t need_keepalive;
    uint8_t last_minute;
} wg_timers_t;

typedef struct {
    uint32_t key;
    uint32_t used;
    void* val;
} wg_idx_slot_t;

typedef struct {
    wg_idx_slot_t* s;
    uint32_t mask;
    uint32_t n;
} wg_idx_t;

typedef struct {
    void** s;
    const uint8_t* (*pub_of)(const void* v);
    uint64_t seed;
    uint32_t mask;
    uint32_t n;
} wg_pubtab_t;

typedef struct wg_aip_node {
    struct wg_aip_node* c[2];
    void* peer;
    uint8_t key[16];
    uint8_t cidr;
} wg_aip_node_t;

typedef struct {
    wg_aip_node_t* root4;
    wg_aip_node_t* root6;
    size_t nodes;
} wg_aip_t;

typedef wg_peer_t* (*wg_by_pub_fn)(void* ud, const uint8_t pub[WG_KEY]);
typedef wg_peer_t* (*wg_by_index_fn)(void* ud, uint32_t index);

void wg_b2s_init(wg_b2s_t* s, size_t outlen, const uint8_t* key, size_t keylen);
void wg_b2s_update(wg_b2s_t* s, const uint8_t* p, size_t n);
void wg_b2s_final(wg_b2s_t* s, uint8_t* out);
void wg_blake2s(uint8_t* out, size_t outlen, const uint8_t* key, size_t keylen, const uint8_t* in, size_t n);
void wg_hash2(uint8_t out[WG_KEY], const uint8_t* a, size_t an, const uint8_t* b, size_t bn);
void wg_hmac(uint8_t out[WG_KEY], const uint8_t* key, size_t kn, const uint8_t* in, size_t n);
void wg_kdf(uint8_t* t1, uint8_t* t2, uint8_t* t3, const uint8_t ck[WG_KEY], const uint8_t* in, size_t n);
void wg_mac(uint8_t out[WG_MACLEN], const uint8_t* key, size_t kn, const uint8_t* in, size_t n);
void wg_hchacha20(uint8_t out[WG_KEY], const uint8_t key[WG_KEY], const uint8_t nonce[16]);
int wg_aead_seal(const uint8_t key[WG_KEY], uint64_t ctr, const uint8_t* pt, size_t n, const uint8_t* ad, size_t adn, uint8_t* out);
int wg_aead_open(const uint8_t key[WG_KEY], uint64_t ctr, const uint8_t* ct, size_t n, const uint8_t* ad, size_t adn, uint8_t* out);
int wg_xaead_seal(const uint8_t key[WG_KEY], const uint8_t nonce[24], const uint8_t* pt, size_t n, const uint8_t* ad, size_t adn, uint8_t* out);
int wg_xaead_open(const uint8_t key[WG_KEY], const uint8_t nonce[24], const uint8_t* ct, size_t n, const uint8_t* ad, size_t adn, uint8_t* out);
int wg_x25519(uint8_t out[WG_KEY], const uint8_t priv[WG_KEY], const uint8_t pub[WG_KEY]);
int wg_x25519_pub(uint8_t pub[WG_KEY], const uint8_t priv[WG_KEY]);
int wg_keygen(uint8_t priv[WG_KEY], uint8_t pub[WG_KEY]);
int wg_random(uint8_t* p, size_t n);
void wg_tai64n(uint8_t out[12], uint64_t unix_ns);
void wg_wipe(void* p, size_t n);
uint32_t wg_le32(const uint8_t* p);
void wg_put_le32(uint8_t* p, uint32_t v);
uint64_t wg_le64(const uint8_t* p);
void wg_put_le64(uint8_t* p, uint64_t v);

int wg_static_init(wg_static_t* s, const uint8_t priv[WG_KEY]);
void wg_static_free(wg_static_t* s);
void* wg_pkey_new(const uint8_t priv[WG_KEY]);
void wg_pkey_free(void* pk);
void* wg_pkey_up(void* pk);
void* wg_keygen_pk(uint8_t priv[WG_KEY], uint8_t pub[WG_KEY]);
int wg_x25519_pk(uint8_t out[WG_KEY], const void* pk, const uint8_t pub[WG_KEY]);
int wg_x25519_static(uint8_t out[WG_KEY], const wg_static_t* s, const uint8_t pub[WG_KEY]);
void wg_sym_start(wg_sym_t* st, const uint8_t* prologue, size_t pn, const uint8_t rs[WG_KEY]);
void wg_sym_start_h0(wg_sym_t* st, const uint8_t hash0[WG_KEY]);
int wg_noise_init_write(wg_sym_t* st, const void* e_pk, const uint8_t e_pub[WG_KEY], const uint8_t rs[WG_KEY], const uint8_t s_pub[WG_KEY], const uint8_t ss[WG_KEY], const uint8_t* pl, size_t pn, uint8_t* out);
int wg_noise_init_read_s(wg_sym_t* st, const wg_static_t* s, const uint8_t* in, uint8_t s_pub[WG_KEY]);
int wg_noise_init_read_pl(wg_sym_t* st, const uint8_t ss[WG_KEY], const uint8_t* ct, size_t ctn, uint8_t* pl);
int wg_noise_resp_write(wg_sym_t* st, const void* e_pk, const uint8_t e_pub[WG_KEY], const uint8_t re[WG_KEY], const uint8_t rs[WG_KEY], const uint8_t psk[WG_KEY], const uint8_t* pl, size_t pn, uint8_t* out);
int wg_noise_resp_read(wg_sym_t* st, const void* e_pk, const wg_static_t* s, const uint8_t psk[WG_KEY], const uint8_t* in, size_t n, uint8_t* pl);
void wg_noise_split(const wg_sym_t* st, uint8_t k1[WG_KEY], uint8_t k2[WG_KEY]);
int wg_peer_init(wg_peer_t* p, const wg_static_t* s, const uint8_t pub[WG_KEY], const uint8_t* psk);
int wg_create_initiation(const wg_static_t* s, wg_peer_t* p, uint32_t local_index, uint64_t unix_ns, uint64_t now_ms, uint8_t out[WG_INIT_LEN]);
wg_peer_t* wg_consume_initiation(const wg_static_t* s, const uint8_t* msg, size_t n, uint64_t now_ms, wg_by_pub_fn lookup, void* ud);
int wg_create_response(const wg_static_t* s, wg_peer_t* p, uint32_t local_index, uint64_t now_ms, uint8_t out[WG_RESP_LEN]);
wg_peer_t* wg_consume_response(const wg_static_t* s, const uint8_t* msg, size_t n, wg_by_index_fn lookup, void* ud);
int wg_derive_keys(wg_peer_t* p, uint8_t send[WG_KEY], uint8_t recv[WG_KEY]);
int wg_check_macs(const wg_static_t* s, wg_cookie_gen_t* g, uint64_t now_ms, const uint8_t* msg, size_t n, const uint8_t* src, size_t srcn, int under_load);
int wg_create_cookie_reply(const wg_static_t* s, wg_cookie_gen_t* g, uint64_t now_ms, const uint8_t* msg, size_t n, const uint8_t* src, size_t srcn, uint8_t out[WG_COOKIE_LEN]);
wg_peer_t* wg_consume_cookie_reply(const uint8_t* msg, size_t n, uint64_t now_ms, wg_by_index_fn lookup, void* ud);
int wg_replay_check(wg_replay_t* r, uint64_t counter);
int wg_replay_peek(const wg_replay_t* r, uint64_t counter);

size_t wg_pad_len(size_t n, unsigned mtu);
int wg_data_parse(const uint8_t* msg, size_t n, uint32_t* receiver, uint64_t* ctr);
int wg_data_seal(const uint8_t key[WG_KEY], uint32_t receiver, uint64_t ctr, uint8_t* buf, size_t n, size_t cap, unsigned mtu);
int wg_data_open(const uint8_t key[WG_KEY], uint8_t* buf, size_t n);
int wg_kp_can_send(const wg_keypair_t* kp, uint64_t now_ms);
int wg_kp_can_recv(const wg_keypair_t* kp, uint64_t now_ms);
int wg_kp_fresh_send(const wg_keypair_t* kp, uint64_t now_ms);
int wg_kp_fresh_recv(const wg_keypair_t* kp, uint64_t now_ms);
void wg_slots_begin(wg_slots_t* s, wg_keypair_t* kp, wg_kp_drop_fn drop, void* ud);
int wg_slots_received(wg_slots_t* s, wg_keypair_t* kp, wg_kp_drop_fn drop, void* ud);
void wg_slots_clear(wg_slots_t* s, wg_kp_drop_fn drop, void* ud);
void wg_batch_work(void* arg);
void wg_fifo_push(wg_fifo_t* q, wg_batch_t* b);
wg_batch_t* wg_fifo_pop_ready(wg_fifo_t* q);

void wg_timers_start(wg_timers_t* t, uint64_t now_ms);
int wg_timers_hs_allowed(const wg_timers_t* t, uint64_t now_ms);
void wg_timers_hs_initiated(wg_timers_t* t, uint64_t now_ms, unsigned jitter_ms, int retry);
void wg_timers_hs_complete(wg_timers_t* t, uint64_t now_ms);
void wg_timers_session_derived(wg_timers_t* t, uint64_t now_ms);
void wg_timers_data_sent(wg_timers_t* t, uint64_t now_ms, unsigned jitter_ms);
void wg_timers_data_recv(wg_timers_t* t, uint64_t now_ms);
void wg_timers_auth_recv(wg_timers_t* t);
void wg_timers_auth_sent(wg_timers_t* t);
void wg_timers_traversal(wg_timers_t* t, uint64_t now_ms);
unsigned wg_timers_due(wg_timers_t* t, uint64_t now_ms);
uint64_t wg_timers_next(const wg_timers_t* t);

int wg_idx_init(wg_idx_t* t, uint32_t cap);
void wg_idx_free(wg_idx_t* t);
int wg_idx_put(wg_idx_t* t, uint32_t key, void* val);
int wg_idx_alloc(wg_idx_t* t, void* val, uint32_t* key);
void* wg_idx_get(const wg_idx_t* t, uint32_t key);
int wg_idx_del(wg_idx_t* t, uint32_t key);
int wg_pubtab_init(wg_pubtab_t* t, const uint8_t* (*pub_of)(const void* v));
void wg_pubtab_free(wg_pubtab_t* t);
int wg_pubtab_put(wg_pubtab_t* t, void* v);
void* wg_pubtab_get(const wg_pubtab_t* t, const uint8_t pub[WG_KEY]);
int wg_pubtab_del(wg_pubtab_t* t, const uint8_t pub[WG_KEY]);
int wg_aip_insert(wg_aip_t* t, int v6, const uint8_t* addr, unsigned cidr, void* peer);
int wg_aip_remove(wg_aip_t* t, int v6, const uint8_t* addr, unsigned cidr);
void wg_aip_remove_peer(wg_aip_t* t, void* peer);
void* wg_aip_lookup(const wg_aip_t* t, int v6, const uint8_t* addr);
void wg_aip_free(wg_aip_t* t);

struct dyn_aio;
struct sockaddr;
typedef struct wg_dev wg_dev_t;
typedef struct wg_dpeer wg_dpeer_t;
typedef void (*wg_dev_rx_fn)(void* ud, wg_dpeer_t* p, const uint8_t* ip, size_t n, int verified);
typedef void (*wg_dev_peer_fn)(void* ud, wg_dpeer_t* p, int rc);
typedef uint64_t (*wg_dev_clock_fn)(void* ud, int realtime_ns);

typedef struct {
    uint64_t rx_data, rx_keepalive, rx_drop, tx_data, tx_drop, hs_init_sent, hs_resp_sent, hs_done, cookie_sent, cookie_recv, keepalive_sent, roams, jobs_offloaded, inline_ops, csum_verified, peers_pending, hs_drop_pending;
} wg_dev_stats_t;

wg_dev_t* wg_dev_new(struct dyn_aio* aio, const uint8_t priv[WG_KEY], const char* host, uint16_t port, wg_dev_rx_fn rx, void* ud);
wg_dev_t* wg_dev_new_static(struct dyn_aio* aio, wg_static_t* s, const char* host, uint16_t port, wg_dev_rx_fn rx, void* ud);
void wg_dev_free(wg_dev_t* d);
void wg_dev_close(wg_dev_t* d);
int wg_dev_routable(const wg_dev_t* d, int v6, const uint8_t* addr);
wg_dpeer_t* wg_dev_peer(const wg_dev_t* d, const uint8_t pub[WG_KEY]);
void* wg_dev_peer_ud(const wg_dpeer_t* p);
int wg_dev_port(const wg_dev_t* d);
const uint8_t* wg_dev_pub(const wg_dev_t* d);
void wg_dev_set_clock(wg_dev_t* d, wg_dev_clock_fn fn, void* ud);
void wg_dev_set_load_threshold(wg_dev_t* d, unsigned pending);
wg_dpeer_t* wg_dev_add_peer(wg_dev_t* d, const uint8_t pub[WG_KEY], const uint8_t* psk, unsigned persist_s, wg_dev_peer_fn ready, void* ud);
int wg_dev_peer_ready(const wg_dpeer_t* p);
void wg_dev_set_verify(wg_dev_t* d, int on);
void wg_dev_remove_peer(wg_dev_t* d, wg_dpeer_t* p);
int wg_dev_set_endpoint(wg_dpeer_t* p, const struct sockaddr* sa, unsigned len);
int wg_dev_get_endpoint(const wg_dpeer_t* p, struct sockaddr* sa, unsigned* len);
int wg_dev_allow(wg_dev_t* d, wg_dpeer_t* p, int v6, const uint8_t* addr, unsigned cidr);
int wg_dev_send(wg_dev_t* d, const uint8_t* ip, size_t n);
void wg_dev_flush(wg_dev_t* d);
void wg_dev_tick(wg_dev_t* d);
uint64_t wg_dev_next_deadline(const wg_dev_t* d);
size_t wg_dev_pending(const wg_dev_t* d);
void wg_dev_stats(const wg_dev_t* d, wg_dev_stats_t* st);
const wg_keypair_t* wg_dev_current(const wg_dpeer_t* p);

#endif
