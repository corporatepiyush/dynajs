#ifndef DYNAJS_UDP_H
#define DYNAJS_UDP_H

#ifdef CONFIG_NATIVE_MODULES

#include <stdint.h>
#include <sys/socket.h>
#include "dyna-aio.h"

#define DYN_UDPB_BATCH 64u
#define DYN_UDPB_ROUNDS 4u

#define DYN_UDPB_GRO 1u
#define DYN_UDPB_V6ONLY 2u
#define DYN_UDPB_REUSEPORT 4u

typedef struct dyn_udpb_batch dyn_udpb_batch_t;

typedef struct dyn_udpb_msg {
    uint8_t* buf;
    uint32_t cap;
    uint32_t len;
    uint16_t gso;
    uint8_t truncated;
    uint8_t reserved;
    socklen_t addr_len;
    struct sockaddr_storage addr;
} dyn_udpb_msg_t;

typedef struct dyn_udpb_ops {
    unsigned (*take)(void* ud, dyn_udpb_msg_t* m, unsigned n);
    void (*batch)(void* ud, dyn_udpb_msg_t* m, unsigned n);
    void (*release)(void* ud, dyn_udpb_msg_t* m, unsigned n);
    void (*error)(void* ud, int err);
    void (*batch_end)(void* ud);
} dyn_udpb_ops_t;

typedef struct dyn_udpb_opts {
    unsigned flags;
    int rcvbuf;
    int sndbuf;
} dyn_udpb_opts_t;

typedef struct dyn_udpb_stats {
    uint64_t wakeups;
    uint64_t rounds;
    uint64_t rx_datagrams;
    uint64_t rx_bytes;
    uint64_t rx_truncated;
    uint64_t rx_errors;
    uint64_t rx_starved;
    uint64_t tx_datagrams;
    uint64_t tx_bytes;
    uint64_t tx_syscalls;
    uint64_t tx_again;
    uint64_t tx_errors;
} dyn_udpb_stats_t;

dyn_udpb_batch_t* dyn_udpb_open(dyn_aio_t* aio, const struct sockaddr* bind_addr,
    socklen_t bind_len, const dyn_udpb_opts_t* opts, const dyn_udpb_ops_t* ops, void* ud);

int dyn_udpb_fd(const dyn_udpb_batch_t* u);

int dyn_udpb_local(const dyn_udpb_batch_t* u, struct sockaddr_storage* ss, socklen_t* len);

int dyn_udpb_send(dyn_udpb_batch_t* u, const dyn_udpb_msg_t* m, unsigned n, int* err);

int dyn_udpb_pause(dyn_udpb_batch_t* u, int paused);

int dyn_udpb_refill(dyn_udpb_batch_t* u);

void dyn_udpb_stats(const dyn_udpb_batch_t* u, dyn_udpb_stats_t* out);

int dyn_udpb_gso_supported(void);

void dyn_udpb_close(dyn_udpb_batch_t* u);

#endif
#endif
