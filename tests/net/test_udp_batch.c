/* dyna-udp.h: the C-level batched UDP path for native modules (first customer: WebRTC).
   Pinned: every datagram is delivered exactly once with exact bytes and sender address; it is received
   in place into a buffer the owner's take() handed out (pointer identity: zero copy); one batch() call
   carries at most DYN_UDPB_BATCH datagrams and one wakeup at most DYN_UDPB_ROUNDS rounds; a datagram larger
   than its buffer is flagged truncated; an empty pool parks the socket without losing datagrams (they
   arrive after dyn_udpb_refill(); a parked socket does not spin on the level-triggered watch); pause/resume; close from
   inside batch() is safe and every taken buffer comes back exactly once (batched or released); the
   batched send delivers all datagrams, splits a gso-sized payload into segments (kernel UDP_SEGMENT on
   Linux, emulated elsewhere), and reports EMSGSIZE for an oversized datagram without wedging the socket.
   The burst is measured deterministically: the socket is paused while a window is sent, and the window counts
   as queued once a probe datagram sent after it reaches another socket (loopback input is FIFO; on macOS it
   runs on a separate input thread, so under load the datagrams could otherwise trickle in and spread over
   many wakeups); then one wakeup must take the whole window.
   Buffer reuse is LIFO (net-udp-2): fill() puts the buffers take() just handed out first and the kept spares after them,
   top of a pointer stack first, so with one datagram per wakeup the buffer batch() just returned is the next one
   filled (a working set of one buffer instead of all 64, and 16-byte pointer pairs copied instead of 152-byte msgs).
   ops.batch_end (optional) runs once per wakeup that delivered anything, after its last batch(), never after close.
   Mutants (red): FIFO rotation (spares bottom-up before the taken buffers, kept in index order: 64 buffers cycle; either
   half alone is equivalent under this LIFO owner); batch_end once per batch (multi-round
   wakeup); batch_end after close inside batch(); ROUNDS bound removed (wakeup round check); one datagram per round (burst window split); spare slots dropped instead of kept (buffer
   accounting); the starved socket left watched (spin check); truncated flag never set; GSO emulation sending the whole payload once. */
#include "dyna-udp.h"

#if defined(CONFIG_NATIVE_MODULES)

#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int fails, checks;
#define CHECK(c, ...)                \
    do {                             \
        checks++;                    \
        if (!(c)) {                  \
            printf("FAIL: ");        \
            printf(__VA_ARGS__);     \
            printf("\n");            \
            fails++;                 \
        }                            \
    } while (0)

#define POOL 256
#define BUFCAP 2048

typedef struct {
    uint8_t* bufs[POOL];
    int owned[POOL];
    unsigned taken, released, batched, batches, max_batch;
    unsigned limit;
    int close_after;
    dyn_udpb_batch_t* u;
    uint8_t* seen;
    unsigned nseen, dup, bad, truncated;
    unsigned from_port;
    int foreign;
} owner_t;

static int buf_index(owner_t* o, const uint8_t* p)
{
    int i;
    for (i = 0; i < POOL; i++)
        if (o->bufs[i] == p)
            return i;
    return -1;
}

static unsigned o_take(void* ud, dyn_udpb_msg_t* m, unsigned n)
{
    owner_t* o = (owner_t*)ud;
    unsigned got = 0;
    int i;
    for (i = 0; i < POOL && got < n; i++) {
        if (o->owned[i] || o->taken >= o->limit)
            continue;
        o->owned[i] = 1;
        o->taken++;
        m[got].buf = o->bufs[i];
        m[got].cap = BUFCAP;
        got++;
    }
    return got;
}

static uint8_t pat(unsigned seq, unsigned j)
{
    uint32_t x = seq * 2654435761u + j * 40503u + 7;
    x ^= x >> 13;
    return (uint8_t)(x * 0x9E3779B1u >> 24);
}

static unsigned dg_len(unsigned seq)
{
    return 4 + (seq * 37u) % 1400u;
}

static void o_batch(void* ud, dyn_udpb_msg_t* m, unsigned n)
{
    owner_t* o = (owner_t*)ud;
    unsigned i;
    o->batches++;
    o->batched += n;
    if (n > o->max_batch)
        o->max_batch = n;
    for (i = 0; i < n; i++) {
        int bi = buf_index(o, m[i].buf);
        if (bi < 0 || !o->owned[bi]) {
            o->foreign++;
            continue;
        }
        o->owned[bi] = 0;
        if (m[i].truncated) {
            o->truncated++;
            continue;
        }
        if (m[i].len >= 4) {
            unsigned seq, j, ok = 1;
            memcpy(&seq, m[i].buf, 4);
            if (m[i].len != dg_len(seq))
                ok = 0;
            for (j = 4; ok && j < m[i].len; j++)
                if (m[i].buf[j] != pat(seq, j))
                    ok = 0;
            if (m[i].addr.ss_family != AF_INET || ntohs(((struct sockaddr_in*)&m[i].addr)->sin_port) != o->from_port)
                ok = 0;
            if (!ok)
                o->bad++;
            else if (seq < o->nseen) {
                if (o->seen[seq])
                    o->dup++;
                o->seen[seq] = 1;
            }
        }
    }
    if (o->close_after && o->batches >= (unsigned)o->close_after && o->u) {
        dyn_udpb_close(o->u);
        o->u = NULL;
    }
}

static void o_release(void* ud, dyn_udpb_msg_t* m, unsigned n)
{
    owner_t* o = (owner_t*)ud;
    unsigned i;
    for (i = 0; i < n; i++) {
        int bi = buf_index(o, m[i].buf);
        if (bi >= 0 && o->owned[bi]) {
            o->owned[bi] = 0;
            o->released++;
        } else
            o->foreign++;
    }
}

static const dyn_udpb_ops_t OPS = { o_take, o_batch, o_release, NULL };

static void owner_init(owner_t* o, unsigned nseen)
{
    int i;
    memset(o, 0, sizeof(*o));
    for (i = 0; i < POOL; i++)
        o->bufs[i] = (uint8_t*)malloc(BUFCAP);
    o->limit = ~0u;
    o->nseen = nseen;
    o->seen = (uint8_t*)calloc(nseen ? nseen : 1, 1);
}

static void owner_free(owner_t* o)
{
    int i;
    for (i = 0; i < POOL; i++)
        free(o->bufs[i]);
    free(o->seen);
}

static unsigned owned_now(owner_t* o)
{
    unsigned i, n = 0;
    for (i = 0; i < POOL; i++)
        n += (unsigned)o->owned[i];
    return n;
}

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static void loopback(struct sockaddr_in* sa, unsigned port)
{
    memset(sa, 0, sizeof(*sa));
    sa->sin_family = AF_INET;
    sa->sin_port = htons((uint16_t)port);
    sa->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
}

static int plain_socket(unsigned* port)
{
    struct sockaddr_in sa;
    socklen_t sl = sizeof(sa);
    int fd = socket(AF_INET, SOCK_DGRAM, 0), big = 4 << 20;
    loopback(&sa, 0);
    bind(fd, (struct sockaddr*)&sa, sizeof(sa));
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &big, sizeof(big));
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &big, sizeof(big));
    getsockname(fd, (struct sockaddr*)&sa, &sl);
    *port = ntohs(sa.sin_port);
    return fd;
}

static void send_seq(int fd, unsigned port, unsigned seq)
{
    uint8_t b[1500];
    unsigned j, len = dg_len(seq);
    struct sockaddr_in to;
    loopback(&to, port);
    memcpy(b, &seq, 4);
    for (j = 4; j < len; j++)
        b[j] = pat(seq, j);
    sendto(fd, b, len, 0, (struct sockaddr*)&to, sizeof(to));
}

static void queued_behind(int tx, int probe, unsigned pport)
{
    struct pollfd p;
    uint8_t b[8];
    struct sockaddr_in to;
    loopback(&to, pport);
    sendto(tx, "probe", 5, 0, (struct sockaddr*)&to, sizeof(to));
    p.fd = probe;
    p.events = POLLIN;
    if (poll(&p, 1, 5000) == 1)
        (void)recv(probe, b, sizeof(b), 0);
}

static void run_until(dyn_aio_t* a, int (*done)(owner_t*), owner_t* o, double ms)
{
    double end = now_ms() + ms;
    while (!done(o) && now_ms() < end)
        dyn_aio_run(a, 5);
}

static unsigned want_n;
static int got_all(owner_t* o)
{
    unsigned i, n = 0;
    for (i = 0; i < o->nseen; i++)
        n += o->seen[i];
    return n >= want_n;
}

static dyn_udpb_batch_t* open_rx(dyn_aio_t* a, owner_t* o, unsigned* port)
{
    struct sockaddr_in sa;
    struct sockaddr_storage ss;
    socklen_t sl;
    dyn_udpb_opts_t opts = { 0, 4 << 20, 4 << 20 };
    dyn_udpb_batch_t* u;
    loopback(&sa, 0);
    u = dyn_udpb_open(a, (struct sockaddr*)&sa, sizeof(sa), &opts, &OPS, o);
    if (!u)
        return NULL;
    dyn_udpb_local(u, &ss, &sl);
    *port = ntohs(((struct sockaddr_in*)&ss)->sin_port);
    o->u = u;
    return u;
}

static void case_burst(dyn_aio_t* a)
{
    owner_t o;
    unsigned port, sport, pport, s, i, missing = 0, split = 0;
    int tx, probe;
    dyn_udpb_batch_t* u;
    dyn_udpb_stats_t st;
    const unsigned N = 3000, W = 200;
    owner_init(&o, N);
    u = open_rx(a, &o, &port);
    CHECK(u != NULL, "burst: open");
    if (!u) {
        owner_free(&o);
        return;
    }
    tx = plain_socket(&sport);
    o.from_port = sport;
    probe = plain_socket(&pport);
    for (s = 0; s < N; s += W) {
        uint64_t w0;
        dyn_udpb_pause(u, 1);
        for (i = s; i < s + W && i < N; i++)
            send_seq(tx, port, i);
        queued_behind(tx, probe, pport);
        dyn_udpb_stats(u, &st);
        w0 = st.wakeups;
        dyn_udpb_pause(u, 0);
        want_n = s + W < N ? s + W : N;
        dyn_aio_run(a, 1000);
        dyn_udpb_stats(u, &st);
        if (!got_all(&o) || st.wakeups - w0 != 1)
            split++;
        run_until(a, got_all, &o, 3000);
    }
    close(probe);
    for (i = 0; i < N; i++)
        missing += !o.seen[i];
    dyn_udpb_stats(u, &st);
    CHECK(missing == 0, "burst: %u of %u datagrams missing", missing, N);
    CHECK(o.dup == 0 && o.bad == 0 && o.foreign == 0, "burst: dup=%u bad=%u foreign=%u", o.dup, o.bad, o.foreign);
    CHECK(o.max_batch <= DYN_UDPB_BATCH, "burst: max batch %u > %u", o.max_batch, DYN_UDPB_BATCH);
    CHECK(st.rx_datagrams == N && st.rx_bytes > 0, "burst: stats rx %llu", (unsigned long long)st.rx_datagrams);
    CHECK(st.rounds <= st.wakeups * DYN_UDPB_ROUNDS, "burst: %llu rounds over %llu wakeups exceeds the %u-round bound",
        (unsigned long long)st.rounds, (unsigned long long)st.wakeups, DYN_UDPB_ROUNDS);
    CHECK(split == 0, "burst: %u of %u windows of %u queued datagrams took more than one wakeup: not batched", split, (N + W - 1) / W, W);
    printf("burst: %u datagrams, %llu wakeups, %llu rounds, %u batches, max batch %u\n", N,
        (unsigned long long)st.wakeups, (unsigned long long)st.rounds, o.batches, o.max_batch);
    dyn_udpb_close(u);
    CHECK(owned_now(&o) == 0, "burst: %u buffers still owned by the socket after close", owned_now(&o));
    CHECK(o.taken == o.batched + o.released, "burst: taken %u != batched %u + released %u", o.taken, o.batched, o.released);
    close(tx);
    owner_free(&o);
}

static void case_rounds_bound(dyn_aio_t* a)
{
    owner_t o;
    unsigned port, sport, pport, i;
    int tx, probe;
    dyn_udpb_batch_t* u;
    dyn_udpb_stats_t st;
    const unsigned N = 600;
    owner_init(&o, N);
    u = open_rx(a, &o, &port);
    tx = plain_socket(&sport);
    o.from_port = sport;
    for (i = 0; i < N; i++)
        send_seq(tx, port, i);
    probe = plain_socket(&pport);
    queued_behind(tx, probe, pport);
    close(probe);
    dyn_aio_run(a, 50);
    dyn_udpb_stats(u, &st);
    CHECK(st.wakeups >= 1 && st.rx_datagrams <= (uint64_t)st.wakeups * DYN_UDPB_BATCH * DYN_UDPB_ROUNDS,
        "rounds: one run took %llu datagrams in %llu wakeups (bound %u per wakeup)",
        (unsigned long long)st.rx_datagrams, (unsigned long long)st.wakeups, DYN_UDPB_BATCH * DYN_UDPB_ROUNDS);
    CHECK(st.rx_datagrams < N, "rounds: a single wakeup drained all %u queued datagrams (%llu): the round bound is gone", N,
        (unsigned long long)st.rx_datagrams);
    want_n = N;
    run_until(a, got_all, &o, 3000);
    CHECK(got_all(&o), "rounds: the rest arrived on later wakeups");
    dyn_udpb_close(u);
    CHECK(o.taken == o.batched + o.released, "rounds: taken %u != batched %u + released %u", o.taken, o.batched, o.released);
    close(tx);
    owner_free(&o);
}

static void case_trunc_starve_pause(dyn_aio_t* a)
{
    owner_t o;
    unsigned port, sport, i;
    int tx;
    struct sockaddr_in to;
    uint8_t big[4000];
    dyn_udpb_batch_t* u;
    dyn_udpb_stats_t st;
    owner_init(&o, 256);
    u = open_rx(a, &o, &port);
    tx = plain_socket(&sport);
    o.from_port = sport;
    loopback(&to, port);
    memset(big, 0x5a, sizeof(big));
    sendto(tx, big, sizeof(big), 0, (struct sockaddr*)&to, sizeof(to));
    for (i = 0; i < 40 && !o.truncated; i++)
        dyn_aio_run(a, 5);
    CHECK(o.truncated == 1, "trunc: a 4000-byte datagram in a %u-byte buffer is flagged truncated (%u)", BUFCAP, o.truncated);

    o.limit = o.taken;
    {
        unsigned spares = owned_now(&o), before = o.batched, S = owned_now(&o) + 40;
        for (i = 0; i < S; i++)
            send_seq(tx, port, i);
        for (i = 0; i < 10; i++)
            dyn_aio_run(a, 5);
        dyn_udpb_stats(u, &st);
        CHECK(st.rx_starved > 0, "starve: an empty pool is counted");
        CHECK(o.batched - before == spares, "starve: only the %u buffers already held are filled while take() gives none (%u)",
            spares, o.batched - before);
        {
            uint64_t w0 = st.wakeups;
            for (i = 0; i < 10; i++)
                dyn_aio_run(a, 2);
            dyn_udpb_stats(u, &st);
            CHECK(st.wakeups == w0, "starve: a parked socket does not spin (%llu wakeups while starved)",
                (unsigned long long)(st.wakeups - w0));
        }
        o.limit = ~0u;
        CHECK(dyn_udpb_refill(u) == 0, "refill");
        want_n = S;
        run_until(a, got_all, &o, 2000);
        CHECK(got_all(&o), "starve: the parked datagrams arrive once buffers come back");

        CHECK(dyn_udpb_pause(u, 1) == 0, "pause");
        for (i = S; i < S + 10; i++)
            send_seq(tx, port, i);
        before = o.batched;
        for (i = 0; i < 10; i++)
            dyn_aio_run(a, 5);
        CHECK(o.batched == before, "pause: nothing delivered while paused (%u -> %u)", before, o.batched);
        CHECK(dyn_udpb_pause(u, 0) == 0, "resume");
        want_n = S + 10;
        run_until(a, got_all, &o, 2000);
        CHECK(got_all(&o), "resume: the datagrams sent while paused arrive");
    }
    dyn_udpb_close(u);
    CHECK(owned_now(&o) == 0 && o.taken == o.batched + o.released, "trunc/starve/pause: buffer accounting taken=%u batched=%u released=%u",
        o.taken, o.batched, o.released);
    close(tx);
    owner_free(&o);
}

static void case_close_in_batch(dyn_aio_t* a)
{
    owner_t o;
    unsigned port, sport, i;
    int tx;
    owner_init(&o, 400);
    open_rx(a, &o, &port);
    tx = plain_socket(&sport);
    o.from_port = sport;
    o.close_after = 1;
    for (i = 0; i < 300; i++)
        send_seq(tx, port, i);
    usleep(10000);
    for (i = 0; i < 20 && o.u; i++)
        dyn_aio_run(a, 5);
    CHECK(o.u == NULL, "close-in-batch: the callback closed the socket");
    for (i = 0; i < 10; i++)
        dyn_aio_run(a, 5);
    CHECK(o.batches == 1, "close-in-batch: no batch after close (%u)", o.batches);
    CHECK(owned_now(&o) == 0 && o.taken == o.batched + o.released && o.foreign == 0,
        "close-in-batch: every taken buffer came back once: taken=%u batched=%u released=%u foreign=%u",
        o.taken, o.batched, o.released, o.foreign);
    close(tx);
    owner_free(&o);
}

static unsigned end_calls, end_pending, end_after_close;
static uint8_t used_buf[POOL];

static void o_batch_mark(void* ud, dyn_udpb_msg_t* m, unsigned n)
{
    owner_t* o = (owner_t*)ud;
    unsigned i;
    for (i = 0; i < n; i++) {
        int bi = buf_index(o, m[i].buf);
        if (bi >= 0)
            used_buf[bi] = 1;
    }
    if (!o->u)
        end_after_close++;
    end_pending = 1;
    o_batch(ud, m, n);
}

static void o_end(void* ud)
{
    owner_t* o = (owner_t*)ud;
    if (!o->u)
        end_after_close++;
    end_calls++;
    end_pending = 0;
}

static const dyn_udpb_ops_t OPS_END = { o_take, o_batch_mark, o_release, NULL, o_end };

static void case_lifo_and_batch_end(dyn_aio_t* a)
{
    owner_t o;
    struct sockaddr_in sa;
    struct sockaddr_storage ss;
    socklen_t sl;
    dyn_udpb_opts_t opts = { 0, 4 << 20, 4 << 20 };
    dyn_udpb_stats_t st;
    unsigned port, sport, pport, i, distinct = 0, missed_end = 0, b0, e0;
    int tx, probe;
    const unsigned PP = 200, N = 600;
    owner_init(&o, N);
    memset(used_buf, 0, sizeof(used_buf));
    end_calls = end_pending = end_after_close = 0;
    loopback(&sa, 0);
    o.u = dyn_udpb_open(a, (struct sockaddr*)&sa, sizeof(sa), &opts, &OPS_END, &o);
    CHECK(o.u != NULL, "lifo: open");
    if (!o.u) {
        owner_free(&o);
        return;
    }
    dyn_udpb_local(o.u, &ss, &sl);
    port = ntohs(((struct sockaddr_in*)&ss)->sin_port);
    tx = plain_socket(&sport);
    o.from_port = sport;
    for (i = 0; i < PP; i++) {
        send_seq(tx, port, i);
        want_n = i + 1;
        run_until(a, got_all, &o, 2000);
        if (end_pending)
            missed_end++;
    }
    CHECK(got_all(&o), "lifo: %u ping-pong datagrams arrived", PP);
    for (i = 0; i < POOL; i++)
        distinct += used_buf[i];
    CHECK(distinct <= 2, "lifo: %u one-datagram wakeups landed in %u distinct buffers; the just-returned buffer must be reused first (want <= 2)",
        PP, distinct);
    CHECK(missed_end == 0, "batch_end: %u wakeups delivered a batch without ending it", missed_end);
    dyn_udpb_stats(o.u, &st);
    CHECK(end_calls <= st.wakeups && end_calls >= PP / 2, "batch_end: %u calls for %llu wakeups (one per delivering wakeup)",
        end_calls, (unsigned long long)st.wakeups);
    for (i = PP; i < N; i++)
        send_seq(tx, port, i);
    probe = plain_socket(&pport);
    queued_behind(tx, probe, pport);
    close(probe);
    b0 = o.batches;
    e0 = end_calls;
    dyn_aio_run(a, 50);
    CHECK(o.batches - b0 >= 2 && end_calls - e0 == 1, "batch_end: a multi-round wakeup made %u batch() calls and %u batch_end() calls (want >= 2 and 1)",
        o.batches - b0, end_calls - e0);
    want_n = N;
    run_until(a, got_all, &o, 3000);
    CHECK(got_all(&o) && !end_pending, "batch_end: the rest arrived, each wakeup ended");
    o.close_after = (int)o.batches + 1;
    for (i = 0; i < 20; i++)
        send_seq(tx, port, i);
    for (i = 0; i < 20 && o.u; i++)
        dyn_aio_run(a, 5);
    CHECK(o.u == NULL && end_after_close == 0, "batch_end: nothing is called after close inside batch() (%u calls)", end_after_close);
    CHECK(owned_now(&o) == 0 && o.taken == o.batched + o.released && o.foreign == 0,
        "lifo: buffer accounting taken=%u batched=%u released=%u foreign=%u", o.taken, o.batched, o.released, o.foreign);
    close(tx);
    owner_free(&o);
}

static void case_send(dyn_aio_t* a)
{
    owner_t o;
    unsigned port, rport, i;
    int rx, err = -1, n;
    dyn_udpb_batch_t* u;
    dyn_udpb_msg_t m[100];
    static uint8_t payload[100][1500];
    struct sockaddr_in to;
    uint8_t rb[4096];
    unsigned got = 0, segs = 0;
    dyn_udpb_stats_t st;
    owner_init(&o, 0);
    u = open_rx(a, &o, &port);
    rx = plain_socket(&rport);
    loopback(&to, rport);
    for (i = 0; i < 100; i++) {
        unsigned j, len = dg_len(i);
        memcpy(payload[i], &i, 4);
        for (j = 4; j < len; j++)
            payload[i][j] = pat(i, j);
        memset(&m[i], 0, sizeof(m[i]));
        m[i].buf = payload[i];
        m[i].len = len;
        memcpy(&m[i].addr, &to, sizeof(to));
        m[i].addr_len = sizeof(to);
    }
    n = dyn_udpb_send(u, m, 100, &err);
    CHECK(n == 100 && err == 0, "send: %d of 100 sent, err %d", n, err);
    {
        struct timeval tv = { 0, 200000 };
        setsockopt(rx, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }
    for (i = 0; i < 100; i++) {
        ssize_t r = recv(rx, rb, sizeof(rb), 0);
        unsigned seq;
        if (r < 4)
            break;
        memcpy(&seq, rb, 4);
        if (seq < 100 && (size_t)r == dg_len(seq) && memcmp(rb, payload[seq], (size_t)r) == 0)
            got++;
    }
    CHECK(got == 100, "send: %u of 100 arrived intact", got);

    memset(&m[0], 0, sizeof(m[0]));
    m[0].buf = payload[0];
    m[0].len = 1300;
    m[0].gso = 500;
    for (i = 0; i < 1300; i++)
        payload[0][i] = (uint8_t)(i * 7 + 1);
    memcpy(&m[0].addr, &to, sizeof(to));
    m[0].addr_len = sizeof(to);
    n = dyn_udpb_send(u, m, 1, &err);
    CHECK(n == 1 && err == 0, "gso: sent %d err %d", n, err);
    for (i = 0; i < 3; i++) {
        ssize_t r = recv(rx, rb, sizeof(rb), 0);
        unsigned want = i < 2 ? 500 : 300;
        if (r == (ssize_t)want && memcmp(rb, payload[0] + i * 500, want) == 0)
            segs++;
    }
    CHECK(segs == 3, "gso: a 1300-byte payload with gso 500 arrives as 500+500+300 (%u segments ok)", segs);

    {
        static uint8_t huge[70000];
        memset(&m[0], 0, sizeof(m[0]));
        m[0].buf = huge;
        m[0].len = sizeof(huge);
        memcpy(&m[0].addr, &to, sizeof(to));
        m[0].addr_len = sizeof(to);
        n = dyn_udpb_send(u, m, 1, &err);
        CHECK(n == 0 && err == EMSGSIZE, "emsgsize: sent %d err %d (want 0, EMSGSIZE %d)", n, err, EMSGSIZE);
        m[0].buf = payload[1];
        m[0].len = 10;
        n = dyn_udpb_send(u, m, 1, &err);
        CHECK(n == 1 && err == 0, "emsgsize: the socket still sends afterwards (%d, %d)", n, err);
    }
    dyn_udpb_stats(u, &st);
    CHECK(st.tx_datagrams >= 104, "send stats: tx_datagrams %llu", (unsigned long long)st.tx_datagrams);
    dyn_udpb_close(u);
    close(rx);
    owner_free(&o);
}

int main(void)
{
    dyn_aio_t* a = dyn_aio_new(256, 0);
    dyn_udpb_opts_t opts = { 0, 0, 0 };
    dyn_udpb_ops_t bad = { NULL, NULL, NULL, NULL };
    struct sockaddr_in sa;
    if (!a) {
        printf("FAIL: dyn_aio_new\n");
        return 1;
    }
    loopback(&sa, 0);
    errno = 0;
    CHECK(dyn_udpb_open(a, (struct sockaddr*)&sa, sizeof(sa), &opts, &bad, NULL) == NULL && errno == EINVAL,
        "open without take/batch is EINVAL");
    case_burst(a);
    case_rounds_bound(a);
    case_trunc_starve_pause(a);
    case_close_in_batch(a);
    case_lifo_and_batch_end(a);
    case_send(a);
    dyn_aio_free(a);
    printf("test_udp_batch: %d of %d checks passed (gso kernel path: %s)\n", checks - fails, checks,
        dyn_udpb_gso_supported() ? "yes" : "emulated");
    return fails ? 1 : 0;
}

#else
int main(void)
{
    return 77;
}
#endif
