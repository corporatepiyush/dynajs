/*
 * bench_rtc_core.c -- hot-path costs of src/core/rtc, comparable with libwr's docs/PERFORMANCE.md rows: STUN parse
 * (+ attribute find, + integrity + fingerprint), SDP parse of a browser offer, candidate parse, DCEP parse, CRC-32c of
 * 1200 B, SCTP input of one 1200-byte DATA packet on an established association (includes CRC-32c and the SACK
 * bookkeeping), and SCTP end to end (send + packetize + input + deliver) per 1200-byte message between two in-memory
 * associations, each with RFC 9653 zero checksum negotiated and with it off. Each row is the best of 5 timed passes, measured with the system allocator. Allocation: the SCTP receive
 * path is allocation-free after warm-up (stash slots are recycled); rtc_sctp_send copies each message into one malloc.
 * Usage: bench_rtc_core [iterations-scale]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "dyn-rtc.h"
#include "dyn-hash.h"

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

static volatile uint64_t sink;

#define BEST(label, iters, bytes, body)                                                              \
    do {                                                                                             \
        double best = 1e30;                                                                          \
        int pass;                                                                                    \
        for (pass = 0; pass < 5; pass++) {                                                           \
            uint64_t t0 = now_ns();                                                                  \
            long it_;                                                                                \
            for (it_ = 0; it_ < (iters); it_++) {                                                    \
                body;                                                                                \
            }                                                                                        \
            {                                                                                        \
                double ns = (double)(now_ns() - t0) / (double)(iters);                               \
                if (ns < best)                                                                       \
                    best = ns;                                                                       \
            }                                                                                        \
        }                                                                                            \
        if (bytes)                                                                                   \
            printf("%-44s %10.1f ns/op %10.0f MB/s\n", label, best, (double)(bytes) * 1000.0 / best); \
        else                                                                                         \
            printf("%-44s %10.1f ns/op\n", label, best);                                             \
    } while (0)

static const char* const offer_sdp = "v=0\r\n"
                                     "o=- 4215775240449105457 2 IN IP4 127.0.0.1\r\n"
                                     "s=-\r\n"
                                     "t=0 0\r\n"
                                     "a=group:BUNDLE 0\r\n"
                                     "a=msid-semantic: WMS\r\n"
                                     "m=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\n"
                                     "c=IN IP4 0.0.0.0\r\n"
                                     "a=candidate:1467250027 1 udp 2122260223 192.168.1.10 56143 typ host generation 0\r\n"
                                     "a=candidate:842163049 1 udp 1686052607 203.0.113.5 46154 typ srflx raddr 192.168.1.10 rport 56143\r\n"
                                     "a=ice-ufrag:EsAw\r\n"
                                     "a=ice-pwd:bP+XJMM09aR8AiX1jdukzR6Y\r\n"
                                     "a=ice-options:trickle\r\n"
                                     "a=fingerprint:sha-256 D2:FA:0E:C3:22:59:5E:14:95:69:92:3D:13:B4:84:24:2C:C2:A2:C0:3E:FD:34:8E:5E:EA:6F:AF:52:CE:E6:0F\r\n"
                                     "a=setup:actpass\r\n"
                                     "a=mid:0\r\n"
                                     "a=sctp-port:5000\r\n"
                                     "a=max-message-size:262144\r\n";

typedef struct {
    rtc_sctp* ep[2];
    uint8_t q[2][64][1500];
    size_t ql[2][64];
    int qn[2];
    uint64_t delivered;
} pair_t;

static pair_t* P;

static void b_packet(void* ud, const uint8_t* p, size_t n)
{
    int i = (int)(intptr_t)ud;
    if (P->qn[i] < 64 && n <= 1500) {
        memcpy(P->q[i][P->qn[i]], p, n);
        P->ql[i][P->qn[i]++] = n;
    }
}

static void b_message(void* ud, uint16_t stream, uint32_t ppid, const uint8_t* p, size_t n)
{
    (void)ud;
    (void)stream;
    (void)ppid;
    (void)p;
    P->delivered += n;
}

static void b_state(void* ud, int st)
{
    (void)ud;
    (void)st;
}

static void b_reset(void* ud, uint16_t s)
{
    (void)ud;
    (void)s;
}

static void b_sent(void* ud, uint16_t s, size_t n)
{
    (void)ud;
    (void)s;
    (void)n;
}

static void pump(uint64_t now)
{
    int moved = 1;
    while (moved) {
        int i;
        moved = 0;
        for (i = 0; i < 2; i++) {
            int k, n = P->qn[i];
            static uint8_t tmp[64][1500];
            static size_t tl[64];
            for (k = 0; k < n; k++) {
                memcpy(tmp[k], P->q[i][k], P->ql[i][k]);
                tl[k] = P->ql[i][k];
            }
            P->qn[i] = 0;
            for (k = 0; k < n; k++)
                rtc_sctp_input(P->ep[!i], tmp[k], tl[k], now);
            if (n)
                moved = 1;
        }
        rtc_sctp_flush(P->ep[0], now);
        rtc_sctp_flush(P->ep[1], now);
    }
}

int main(int argc, char** argv)
{
    long scale = argc > 1 ? atol(argv[1]) : 1;
    int zc;
    static uint8_t stun[512], pkt1200[1500], data_pkt[1500], msg[1200];
    static rtc_sdp sdp;
    rtc_stun_wr w;
    rtc_stun_msg m;
    rtc_sdp_cand c;
    rtc_dcep_msg dm;
    const char* err;
    size_t sn, dn = 0;
    uint8_t tid[12] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 };
    static const rtc_sctp_cb cb = { b_packet, b_message, b_state, b_reset, b_reset, b_sent };
    rtc_rng rng;
    uint8_t seed[32] = { 9 };
    uint64_t now = 1000000;
    const char* cand = "candidate:842163049 1 udp 1686052607 203.0.113.5 46154 typ srflx raddr 192.168.1.10 rport 56143";
    static const uint8_t dcep[] = { 0x03, 0x00, 0x01, 0x00, 0, 0, 0, 0, 0, 4, 0, 0, 'c', 'h', 'a', 't' };
    long i;
    if (scale < 1)
        scale = 1;
    printf("# bench_rtc_core (best of 5), scale %ld\n", scale);
    rtc_stun_wr_init(&w, stun, sizeof(stun), rtc_stun_type(RTC_STUN_BINDING, RTC_STUN_REQUEST), tid);
    rtc_stun_wr_attr(&w, RTC_SA_USERNAME, "EsAw:abcd", 9);
    rtc_stun_wr_u32(&w, RTC_SA_PRIORITY, 1686052607);
    rtc_stun_wr_u64(&w, RTC_SA_ICE_CONTROLLING, 12345);
    rtc_stun_wr_attr(&w, RTC_SA_USE_CANDIDATE, NULL, 0);
    rtc_stun_wr_integrity(&w, (const uint8_t*)"bP+XJMM09aR8AiX1jdukzR6Y", 24);
    rtc_stun_wr_fingerprint(&w);
    sn = rtc_stun_wr_done(&w);
    BEST("STUN parse (ICE check, 6 attrs)", 2000000 * scale, sn, sink += (uint64_t)rtc_stun_parse(&m, stun, sn));
    BEST("STUN parse + attr find", 2000000 * scale, sn, {
        rtc_stun_parse(&m, stun, sn);
        sink += (uintptr_t)rtc_stun_find(&m, RTC_SA_PRIORITY);
    });
    BEST("STUN parse + integrity + fingerprint", 300000 * scale, sn, {
        rtc_stun_parse(&m, stun, sn);
        sink += (uint64_t)rtc_stun_check_integrity(&m, (const uint8_t*)"bP+XJMM09aR8AiX1jdukzR6Y", 24) + (uint64_t)rtc_stun_check_fingerprint(&m);
    });
    BEST("SDP parse (1 m-line, 2 candidates)", 200000 * scale, strlen(offer_sdp),
        sink += (uint64_t)rtc_sdp_parse(&sdp, offer_sdp, strlen(offer_sdp), &err));
    BEST("candidate parse (srflx)", 2000000 * scale, strlen(cand), sink += (uint64_t)rtc_sdp_parse_candidate(cand, strlen(cand), &c));
    BEST("DCEP parse", 10000000 * scale, sizeof(dcep), sink += (uint64_t)rtc_dcep_parse(dcep, sizeof(dcep), &dm));
    for (i = 0; i < 1200; i++)
        pkt1200[i] = (uint8_t)(i * 7);
    BEST("CRC-32c 1200 B", 2000000 * scale, 1200, sink += dyn_crc32c(pkt1200, 1200));

    for (zc = 1; zc >= 0; zc--) {
        printf("-- RFC 9653 zero checksum %s\n", zc ? "negotiated (CRC-32c skipped)" : "off (CRC-32c on every packet)");
        P = (pair_t*)calloc(1, sizeof(*P));
        rtc_rng_seed(&rng, seed);
        P->ep[0] = rtc_sctp_new(&cb, (void*)(intptr_t)0, &rng, 5000, 5000, 0);
        P->ep[1] = rtc_sctp_new(&cb, (void*)(intptr_t)1, &rng, 5000, 5000, 0);
        P->ep[0]->zcsum_local = (uint8_t)zc;
        P->ep[1]->zcsum_local = (uint8_t)zc;
        rtc_sctp_connect(P->ep[0], now);
        pump(now);
        for (i = 0; i < 1200; i++)
            msg[i] = (uint8_t)i;
        BEST("SCTP end to end, 1200 B message", 300000 * scale, 1200, {
            rtc_sctp_send(P->ep[0], 1, 53, msg, sizeof(msg), 0, -1, -1, now);
            rtc_sctp_flush(P->ep[0], now);
            pump(now);
            if ((it_ & 63) == 63) {
                now += 250000;
                rtc_sctp_tick(P->ep[0], now);
                rtc_sctp_tick(P->ep[1], now);
                pump(now);
            }
        });
        rtc_sctp_send(P->ep[0], 1, 53, msg, sizeof(msg), 1, -1, -1, now);
        rtc_sctp_flush(P->ep[0], now);
        if (P->qn[0]) {
            dn = P->ql[0][0];
            memcpy(data_pkt, P->q[0][0], dn);
        }
        P->qn[0] = 0;
        BEST("SCTP input, one 1200 B DATA packet (dup)", 2000000 * scale, dn, {
            rtc_sctp_input(P->ep[1], data_pkt, dn, now);
            P->qn[1] = 0;
        });
        printf("delivered %llu bytes, sink %llu\n", (unsigned long long)P->delivered, (unsigned long long)sink);
        rtc_sctp_free(P->ep[0]);
        rtc_sctp_free(P->ep[1]);
        free(P);
    }
    return 0;
}
