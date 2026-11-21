/*
 * test_rtc_media.c -- the media kernel in src/core/rtc: RTP header and extensions (rtc-rtp), RTCP compound packets and
 * feedback (rtc-rtcp), receiver statistics (rtc-rtpstat) and the send history used to answer NACKs (rtc-pktring).
 * The kernel takes time as arguments and does no I/O, so every case runs on virtual time.
 *
 * Oracles, none derived from the code under test:
 * - byte layouts hand-encoded from the RFC figures: RTP fixed header and CSRC list (RFC 3550 5.1), header extensions in
 *   the one-byte form 0xBEDE and the two-byte form 0x100X (RFC 8285 4.2/4.3, including the padding byte 0 and the ID 15
 *   stop), SR/RR report blocks (RFC 3550 6.4.1/6.4.2), SDES CNAME chunks (6.5), BYE with a reason (6.6), the compound
 *   rule that only the last packet may be padded (6.4.1), Generic NACK PID+BLP (RFC 4585 6.2.1), PLI (6.3.1), FIR
 *   (RFC 5104 4.3.1.1) and REMB (draft-alvestrand-rmcat-remb);
 * - the RTT example of RFC 3550 6.4.1: A = 0xb710:8000, LSR = 0xb705:2000, DLSR = 0x0005:4000 give 6.125 s;
 * - RFC 3550 A.1 sequence rules (cycles on wrap, duplicates, a jump past MAX_DROPOUT accepted only when the next packet
 *   follows it) and A.3 loss (expected - received, fraction = lost_interval * 256 / expected_interval);
 * - RFC 3550 A.8 interarrival jitter computed in double precision by the formula J += (|D| - J) / 16;
 * - malformed input: every length, count and padding field pointed past the end must be rejected, never read.
 * Payload formats (W2), oracles hand-built from the RFC figures and rules:
 * - Opus framing (RFC 6716 3.2, Figures 4-7, requirements R1-R7 of 3.4) for codes 0-3 with one- and two-byte lengths,
 *   CBR/VBR, padding chains (255 = 254 + another length byte) and the 120 ms cap for every one of the 32 configs;
 *   RFC 7587 puts exactly one Opus packet in an RTP payload, so the parser is the depacketizer;
 * - G.711 (RFC 3551 4.5.14): one octet per sample at 8000 Hz, the timestamp advances by the sample count;
 * - VP8 descriptor (RFC 7741 4.2) with I/L/T/K in every combination, 7- and 15-bit PictureID, ignored R bits; the
 *   frame tag and key frame header (RFC 6386 9.1: start code 9d 01 2a, 14-bit width/height with 2-bit scale);
 * - H.264 (RFC 6184 5.6-5.8): single NAL, STAP-A, FU-A; round trips are checked against an access unit built
 *   independently from its NAL list (zero-free NAL bodies, mixed 3- and 4-byte start codes), at every MTU from 16 to
 *   120 (VP8 16 to 80) with sizes on each side of every packet boundary, so the expected packet counts are arithmetic.
 * A depacketizer that returns -2 (no room) has written nothing, so the caller can grow the buffer and retry.
 * Loss recovery (W3):
 * - feedback FCI shapes: NACK needs PID+BLP entries (RFC 4585 6.2.1), PLI has no FCI (6.3.1), FIR entries are 8 bytes
 *   with the target in the FCI and media SSRC 0 in the header (RFC 5104 4.3.1.1); a FIR repetition keeps its seq, a new
 *   command adds 1 mod 256 from an arbitrary initial value, and only one is outstanding (RFC 5104 3.5.1);
 * - TWCC (draft-holmer-rmcat-transport-wide-cc-extensions-01): the section 2 extension layout and the examples of 3.1.3 and
 *   3.1.4; example 2 of each uses symbol 11, which 3.1.1 reserves, so those two are expected to be rejected;
 * - differential end to end: frames are packetized, sent through a seeded channel (random and burst loss, duplication,
 *   reordering, sequence and timestamp wrap), NACKed from the receiver and answered from the sender history; every
 *   delivered frame must equal the original byte for byte, in order, and with NACK every frame is delivered. Four tail
 *   frames follow the counted ones because a loss in the very last packet cannot be detected (nothing follows it), and
 *   the first frame is never dropped (only reordered) because no receiver can NACK what precedes the first packet it sees.
 * Why the jitter buffer needs an explicit start for its first packet: a key-frame packet can be the middle of an access
 * unit (an IDR FU-A after SPS and PPS), so a key flag alone would release a frame without its first NAL units.
 * Why u32 slots in the NACK ring and no per-packet malloc: the history is one byte arena plus a power-of-two slot
 * array indexed by sequence number, so a NACK lookup is O(1) and the ring's RSS is fixed per sender.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rtc/rtc-jbuf.h"
#include "rtc/rtc-nack.h"
#include "rtc/rtc-payload.h"
#include "rtc/rtc-pktring.h"
#include "rtc/rtc-rtcp.h"
#include "rtc/rtc-rtp.h"
#include "rtc/rtc-rtpstat.h"
#include "rtc/rtc-twcc.h"

static int checks, fails;
#define CHECK(c, ...)                                                                                                      \
    do {                                                                                                                   \
        checks++;                                                                                                          \
        if (!(c)) {                                                                                                        \
            fails++;                                                                                                       \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                    \
            printf(__VA_ARGS__);                                                                                           \
            printf("\n");                                                                                                  \
        }                                                                                                                  \
    } while (0)

static size_t hex(const char* s, uint8_t* out)
{
    size_t n = 0;
    int hi = -1;
    for (; *s; s++) {
        int v;
        if (*s >= '0' && *s <= '9')
            v = *s - '0';
        else if (*s >= 'a' && *s <= 'f')
            v = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'F')
            v = *s - 'A' + 10;
        else
            continue;
        if (hi < 0)
            hi = v;
        else {
            out[n++] = (uint8_t)(hi << 4 | v);
            hi = -1;
        }
    }
    return n;
}

static void test_rtp_parse(void)
{
    uint8_t p[256];
    rtc_rtp_hdr h;
    rtc_rtp_ext e[8];
    const uint8_t* v = NULL;
    size_t n, vl = 0;
    unsigned k;
    n = hex("92e0 1234 deadbeef 11223344 aaaaaaaa bbbbbbbb"
            "bede 0002 10aa 00 22 b1b2b3 00"
            "0102030405",
        p);
    CHECK(rtc_rtp_parse(p, n, &h) == 0, "rtp: parse the RFC 3550 5.1 + RFC 8285 4.2 fixture");
    CHECK(h.cc == 2 && h.marker == 1 && h.pt == 96 && h.seq == 0x1234 && h.ts == 0xdeadbeef && h.ssrc == 0x11223344,
        "rtp: fixed header fields (cc %u m %u pt %u)", h.cc, h.marker, h.pt);
    CHECK(h.hlen == 12 + 8 + 4 + 8 && h.plen == 5 && h.ext_profile == 0xBEDE && h.ext_len == 8, "rtp: header %u payload %u", h.hlen, h.plen);
    k = rtc_rtp_ext_list(p, &h, e, 8);
    CHECK(k == 2 && e[0].id == 1 && e[0].len == 1 && e[0].val[0] == 0xaa && e[1].id == 2 && e[1].len == 3 && e[1].val[2] == 0xb3,
        "rtp: one-byte elements, the padding byte 0 between them skipped (%u)", k);
    CHECK(rtc_rtp_ext_get(p, &h, 2, &v, &vl) && vl == 3 && v[0] == 0xb1, "rtp: ext_get id 2");
    CHECK(!rtc_rtp_ext_get(p, &h, 3, &v, &vl), "rtp: absent id");
    n = hex("9060 0001 00000001 00000002 bede 0002 10aa f0 22 b1b2b3 00 c0", p);
    CHECK(rtc_rtp_parse(p, n, &h) == 0 && rtc_rtp_ext_list(p, &h, e, 8) == 1, "rtp: ID 15 stops one-byte processing (RFC 8285 4.2)");
    n = hex("9060 0001 00000001 00000002 1003 0002 0100 0702 c1c2 0000 9999", p);
    CHECK(rtc_rtp_parse(p, n, &h) == 0 && rtc_rtp_ext_list(p, &h, e, 8) == 2 && e[0].id == 1 && e[0].len == 0 && e[1].id == 7
            && e[1].len == 2 && e[1].val[1] == 0xc2 && h.plen == 2,
        "rtp: two-byte form 0x1003 with a zero-length element (RFC 8285 4.3)");
    n = hex("a060 0001 00000001 00000002 aabbccdd 0000 03", p);
    CHECK(rtc_rtp_parse(p, n, &h) == 0 && h.pad == 3 && h.plen == 4, "rtp: padding count from the last octet");
    n = hex("a060 0001 00000001 00000002 aabbccdd 0000 00", p);
    CHECK(rtc_rtp_parse(p, n, &h) < 0, "rtp malformed: padding count 0");
    n = hex("a060 0001 00000001 00000002 aa 09", p);
    CHECK(rtc_rtp_parse(p, n, &h) < 0, "rtp malformed: padding past the header");
    n = hex("8f60 0001 00000001 00000002 aabbccdd", p);
    CHECK(rtc_rtp_parse(p, n, &h) < 0, "rtp malformed: 15 CSRCs past the end");
    n = hex("9060 0001 00000001 00000002 bede 0009 10aa", p);
    CHECK(rtc_rtp_parse(p, n, &h) < 0, "rtp malformed: extension length past the end");
    n = hex("9060 0001 00000001 00000002 bede", p);
    CHECK(rtc_rtp_parse(p, n, &h) < 0, "rtp malformed: truncated extension header");
    n = hex("4060 0001 00000001 00000002", p);
    CHECK(rtc_rtp_parse(p, n, &h) < 0, "rtp malformed: version 1");
    CHECK(rtc_rtp_parse(p, 11, &h) < 0, "rtp malformed: 11 bytes");
    n = hex("9060 0001 00000001 00000002 bede 0001 3fff ffff", p);
    CHECK(rtc_rtp_parse(p, n, &h) == 0 && rtc_rtp_ext_list(p, &h, e, 8) == 0, "rtp malformed: element length past the block yields nothing");
    CHECK(rtc_rtp_is_rtcp((const uint8_t*)"\x80\xc8\x00\x06", 8) && !rtc_rtp_is_rtcp((const uint8_t*)"\x80\x60\x00\x06", 8),
        "demux: RTCP is PT 192..223 in the second octet (RFC 5761 4)");
}

static void test_rtp_write(void)
{
    uint8_t out[256], want[256];
    uint32_t csrc[2] = { 0xaaaaaaaa, 0xbbbbbbbb };
    uint8_t v1 = 0xaa, v2[3] = { 0xb1, 0xb2, 0xb3 }, big[17];
    rtc_rtp_ext e[2];
    size_t n, w;
    e[0].id = 1;
    e[0].len = 1;
    e[0].val = &v1;
    e[1].id = 2;
    e[1].len = 3;
    e[1].val = v2;
    n = rtc_rtp_write(out, sizeof(out), 96, 1, 0x1234, 0xdeadbeef, 0x11223344, csrc, 2, e, 2, (const uint8_t*)"\x01\x02", 2);
    w = hex("92e0 1234 deadbeef 11223344 aaaaaaaa bbbbbbbb bede 0002 10aa 22b1b2b3 0000 0102", want);
    CHECK(n == w && !memcmp(out, want, n), "rtp write: one-byte form, padded to a word (%zu vs %zu)", n, w);
    memset(big, 0x5a, sizeof(big));
    e[0].len = 17;
    e[0].val = big;
    n = rtc_rtp_write(out, sizeof(out), 0, 0, 1, 2, 3, NULL, 0, e, 1, NULL, 0);
    CHECK(n == 12 + 4 + 20 && out[12] == 0x10 && out[13] == 0x00 && out[16] == 1 && out[17] == 17, "rtp write: a 17-byte element switches to the two-byte form");
    CHECK(rtc_rtp_write(out, 12, 0, 0, 1, 2, 3, NULL, 0, NULL, 0, (const uint8_t*)"x", 1) == 0, "rtp write: no room");
    CHECK(rtc_rtp_write(out, sizeof(out), 128, 0, 1, 2, 3, NULL, 0, NULL, 0, NULL, 0) == 0, "rtp write: payload type 128");
}

static void test_rtcp(void)
{
    uint8_t p[512];
    size_t n;
    rtc_rtcp_iter it;
    rtc_rtcp_pkt k;
    rtc_rtcp_sr sr;
    rtc_rtcp_rb rb;
    rtc_rtcp_fb fb;
    uint16_t seqs[64];
    uint32_t ss[4], s = 0, fs = 0;
    const uint8_t* name = NULL;
    size_t nl = 0;
    uint8_t fseq = 0;
    uint64_t bps = 0;
    unsigned cnt = 0;
    n = hex("81c8000c 11111111 e0000000 80000000 00001000 00000064 00002710"
            "22222222 0a fffffe 00010203 00000040 b7052000 00054000"
            "81ca0003 11111111 0105 6e616d6573 00"
            "81cb0003 11111111 04 62796521 000000",
        p);
    CHECK(rtc_rtcp_check(p, n) == 0, "rtcp: SR + SDES + BYE compound is valid");
    rtc_rtcp_iter_init(&it, p, n);
    CHECK(rtc_rtcp_next(&it, &k) == 1 && k.pt == RTC_RTCP_SR && k.count == 1 && rtc_rtcp_sr_parse(&k, &sr) == 0 && sr.ssrc == 0x11111111
            && sr.ntp == 0xe000000080000000ull && sr.rtp_ts == 0x1000 && sr.pkts == 100 && sr.octets == 10000,
        "rtcp: SR sender info (RFC 3550 6.4.1)");
    CHECK(rtc_rtcp_rb_count(&k) == 1 && rtc_rtcp_rb_at(&k, 0, &rb) == 0 && rb.ssrc == 0x22222222 && rb.fraction == 10 && rb.lost == -2
            && rb.ext_seq == 0x00010203 && rb.jitter == 0x40 && rb.lsr == 0xb7052000 && rb.dlsr == 0x00054000,
        "rtcp: report block, 24-bit cumulative lost is signed (%d)", rb.lost);
    CHECK(rtc_rtcp_next(&it, &k) == 1 && rtc_rtcp_sdes_cname(&k, &s, &name, &nl) == 0 && s == 0x11111111 && nl == 5 && !memcmp(name, "names", 5),
        "rtcp: SDES CNAME (6.5.1)");
    CHECK(rtc_rtcp_next(&it, &k) == 1 && rtc_rtcp_bye_ssrcs(&k, ss, 4) == 1 && ss[0] == 0x11111111, "rtcp: BYE (6.6)");
    CHECK(rtc_rtcp_next(&it, &k) == 0, "rtcp: end of compound");
    n = hex("a1c90002 33333333 00000004", p);
    CHECK(rtc_rtcp_check(p, n) < 0, "rtcp malformed: count 1 RR without room for its block");
    rtc_rtcp_iter_init(&it, p, n);
    CHECK(rtc_rtcp_next(&it, &k) == 1 && rtc_rtcp_rb_count(&k) == 0 && rtc_rtcp_rb_at(&k, 0, &rb) < 0, "rtcp malformed: RR block count past the payload reads nothing");
    n = hex("a0c90002 33333333 00000004 80c90001 44444444", p);
    CHECK(rtc_rtcp_check(p, n) < 0, "rtcp malformed: padding on a packet that is not the last (6.4.1)");
    n = hex("80c90001 33333333 80c90009 44444444", p);
    CHECK(rtc_rtcp_check(p, n) < 0, "rtcp malformed: length field past the end");
    n = hex("80c90001 33333333 40c90001 44444444", p);
    CHECK(rtc_rtcp_check(p, n) < 0, "rtcp malformed: version 1 in a compound");
    n = hex("80450001 33333333", p);
    CHECK(rtc_rtcp_check(p, n) < 0, "rtcp malformed: payload type 69");
    n = hex("81ca0002 11111111 01ff 0000", p);
    rtc_rtcp_iter_init(&it, p, n);
    CHECK(rtc_rtcp_next(&it, &k) == 1 && rtc_rtcp_sdes_cname(&k, &s, &name, &nl) < 0, "rtcp malformed: CNAME length past the end");
    n = hex("81cd0004 aaaaaaaa bbbbbbbb 0064 8005 0200 0000", p);
    rtc_rtcp_iter_init(&it, p, n);
    CHECK(rtc_rtcp_next(&it, &k) == 1 && rtc_rtcp_fb_parse(&k, &fb) == 0 && fb.sender == 0xaaaaaaaa && fb.media == 0xbbbbbbbb
            && rtc_rtcp_nack_seqs(&fb, seqs, 64) == 5 && seqs[0] == 100 && seqs[1] == 101 && seqs[2] == 103 && seqs[3] == 116 && seqs[4] == 512,
        "rtcp: NACK PID 100 BLP 0x8005 -> 100,101,103,116 then PID 512 (RFC 4585 6.2.1)");
    {
        uint8_t want[64];
        size_t w = hex("81cd0004 aaaaaaaa bbbbbbbb 0064 8005 0200 0000", want);
        uint16_t in[5] = { 100, 101, 103, 116, 512 };
        n = rtc_rtcp_write_nack(p, sizeof(p), 0xaaaaaaaa, 0xbbbbbbbb, in, 5);
        CHECK(n == w && !memcmp(p, want, n), "rtcp write: NACK pairs from sorted sequence numbers (%zu)", n);
    }
    n = hex("81ce0002 aaaaaaaa bbbbbbbb", p);
    rtc_rtcp_iter_init(&it, p, n);
    CHECK(rtc_rtcp_next(&it, &k) == 1 && k.pt == RTC_RTCP_PSFB && k.count == RTC_RTCP_FB_PLI, "rtcp: PLI is PSFB FMT 1 (RFC 4585 6.3.1)");
    CHECK(rtc_rtcp_write_pli(p + 100, 12, 0xaaaaaaaa, 0xbbbbbbbb) == 12 && !memcmp(p, p + 100, 12), "rtcp write: PLI");
    n = hex("84ce0004 aaaaaaaa 00000000 cccccccc 07000000", p);
    rtc_rtcp_iter_init(&it, p, n);
    CHECK(rtc_rtcp_next(&it, &k) == 1 && rtc_rtcp_fb_parse(&k, &fb) == 0 && rtc_rtcp_fir_count(&fb) == 1 && rtc_rtcp_fir_at(&fb, 0, &fs, &fseq) == 0
            && fs == 0xcccccccc && fseq == 7,
        "rtcp: FIR entry SSRC + seq nr (RFC 5104 4.3.1.1)");
    CHECK(rtc_rtcp_write_fir(p + 100, 20, 0xaaaaaaaa, 0xcccccccc, 7) == 20 && !memcmp(p, p + 100, 20), "rtcp write: FIR");
    n = hex("8fce0005 aaaaaaaa 00000000 52454d42 01 0b d6d8 bbbbbbbb", p);
    rtc_rtcp_iter_init(&it, p, n);
    CHECK(rtc_rtcp_next(&it, &k) == 1 && rtc_rtcp_fb_parse(&k, &fb) == 0 && rtc_rtcp_remb(&fb, &bps, ss, 4, &cnt) == 0 && cnt == 1
            && ss[0] == 0xbbbbbbbb && bps == (uint64_t)0x3d6d8 << 2,
        "rtcp: REMB exponent 2 mantissa 0x3d6d8 (%llu)", (unsigned long long)bps);
}

static void test_rtt(void)
{
    uint64_t sec = 0xC000B710ull, unix_us = (sec - RTC_NTP_UNIX_OFFSET) * 1000000u + 500000u;
    int64_t rtt = rtc_rtcp_rtt_us(unix_us, 0xb7052000u, 0x00054000u);
    CHECK(rtt == 6125000, "rtt: RFC 3550 6.4.1 example A=0xb710:8000 gives 6.125 s (%lld us)", (long long)rtt);
    CHECK(rtc_rtcp_rtt_us(unix_us, 0, 0x00054000u) == -1, "rtt: LSR 0 means no SR received yet");
    CHECK(rtc_ntp_to_us(rtc_ntp_from_us(1700000000123456ull)) == 1700000000123456ull, "ntp: round trip to the microsecond");
}

static void test_rtpstat(void)
{
    rtc_rtp_rx s;
    rtc_rtcp_rb rb;
    uint32_t i;
    double J = 0;
    uint32_t prev_transit = 0;
    rtc_rtp_rx_init(&s, 7, 65530);
    for (i = 0; i < 12; i++)
        rtc_rtp_rx_update(&s, (uint16_t)(65530 + i));
    CHECK(s.cycles == 65536 && rtc_rtp_rx_ext_max(&s) == 65536 + 5 && rtc_rtp_rx_lost(&s) == 0, "A.1: wrap adds a cycle (ext max %u)",
        rtc_rtp_rx_ext_max(&s));
    rtc_rtp_rx_update(&s, 5);
    CHECK(s.dups == 1 && rtc_rtp_rx_lost(&s) == -1, "A.1: a duplicate counts as received, so lost goes negative (A.3)");
    CHECK(rtc_rtp_rx_update(&s, 20000) == 0, "A.1: a jump past MAX_DROPOUT is not accepted alone");
    CHECK(rtc_rtp_rx_update(&s, 20001) == 1 && s.base_seq == 20001 && s.received == 1, "A.1: the next sequential one resets the source");
    rtc_rtp_rx_init(&s, 7, 1000);
    for (i = 0; i < 100; i++)
        if (i % 10 != 3)
            rtc_rtp_rx_update(&s, (uint16_t)(1000 + i));
    rtc_rtp_rx_report(&s, 1000000, &rb);
    CHECK(rb.lost == 10 && rb.fraction == 25 && rb.ext_seq == 1099 && rb.ssrc == 7, "A.3: 10 of 100 lost -> cumulative 10, fraction 25/256 (%d, %u)",
        rb.lost, rb.fraction);
    for (i = 100; i < 200; i++)
        rtc_rtp_rx_update(&s, (uint16_t)(1000 + i));
    rtc_rtp_rx_report(&s, 2000000, &rb);
    CHECK(rb.lost == 10 && rb.fraction == 0, "A.3: the next interval lost nothing -> fraction 0, cumulative stays 10");
    rtc_rtp_rx_init(&s, 9, 1);
    for (i = 0; i < 200; i++) {
        uint32_t rtp_ts = i * 960, arrival = i * 960 + (uint32_t)((i * 37) % 101);
        rtc_rtp_rx_arrival(&s, rtp_ts, arrival);
        if (i) {
            double d = fabs((double)(int32_t)((arrival - rtp_ts) - prev_transit));
            J += (d - J) / 16.0;
        }
        prev_transit = arrival - rtp_ts;
    }
    rtc_rtp_rx_report(&s, 0, &rb);
    CHECK(fabs((double)rb.jitter - J) <= 1.0, "A.8: interarrival jitter %u vs double-precision %.2f", rb.jitter, J);
    rtc_rtp_rx_on_sr(&s, 0xe0000000b7052000ull << 0, 5000000);
    rtc_rtp_rx_report(&s, 6000000, &rb);
    CHECK(rb.lsr == (uint32_t)(0xe0000000b7052000ull >> 16) && rb.dlsr == 65536, "report: LSR middle 32 bits, DLSR 1 s = 65536/65536 s");
    rtc_rtp_rx_report(&s, 4000000, &rb);
    CHECK(rb.dlsr == 0, "report: a now earlier than the SR arrival (mixed clocks) gives DLSR 0, never a wrapped value");
}

static void test_pktring(void)
{
    rtc_pktring r;
    const rtc_pkt_slot* sl = NULL;
    const uint8_t* g;
    uint8_t pk[300];
    int i, ok = 1;
    CHECK(rtc_pktring_init(&r, 1000, 6) < 0, "ring: slot count must be a power of two");
    CHECK(rtc_pktring_init(&r, 1000, 8) == 0, "ring: init");
    for (i = 0; i < 300; i++)
        pk[i] = (uint8_t)(i * 3 + 1);
    rtc_pktring_put(&r, 10, 100, 1, 5, pk, 300);
    g = rtc_pktring_get(&r, 10, &sl);
    CHECK(g && sl->len == 300 && sl->ts == 100 && sl->marker == 1 && !memcmp(g, pk, 300), "ring: get what was put");
    CHECK(!rtc_pktring_get(&r, 18, NULL), "ring: same slot, other sequence number");
    rtc_pktring_put(&r, 11, 101, 0, 6, pk + 1, 299);
    rtc_pktring_put(&r, 12, 102, 0, 7, pk + 2, 298);
    rtc_pktring_put(&r, 13, 103, 0, 8, pk + 3, 297);
    CHECK(!rtc_pktring_get(&r, 10, NULL), "ring: bytes overwritten once the arena wrapped past it");
    g = rtc_pktring_get(&r, 13, &sl);
    CHECK(g && !memcmp(g, pk + 3, 297), "ring: the newest packet");
    g = rtc_pktring_get(&r, 12, &sl);
    CHECK(g && !memcmp(g, pk + 2, 298), "ring: the one before it");
    rtc_pktring_drop(&r, 12);
    CHECK(!rtc_pktring_get(&r, 12, NULL), "ring: dropped");
    for (i = 0; i < 1000; i++) {
        uint16_t sq = (uint16_t)(65000 + i);
        rtc_pktring_put(&r, sq, (uint32_t)i, 0, 0, pk, (size_t)(1 + i % 100));
        g = rtc_pktring_get(&r, sq, &sl);
        ok &= g != NULL && sl->len == (uint32_t)(1 + i % 100) && sl->seq == sq;
    }
    CHECK(ok, "ring: 1000 puts across the sequence wrap");
    CHECK(rtc_pktring_put(&r, 1, 0, 0, 0, pk, 1001) < 0, "ring: a packet larger than the arena");
    rtc_pktring_free(&r);
}

static void test_opus(void)
{
    CHECK(rtc_opus_samples((const uint8_t*)"\x08", 1) == 960, "opus: config 1 (SILK NB 20 ms) code 0 is 960 samples at 48 kHz (RFC 6716 3.1)");
    CHECK(rtc_opus_samples((const uint8_t*)"\xf9\x00\x00", 3) == 1920, "opus: config 31 (CELT FB 20 ms) code 1 is two frames");
    CHECK(rtc_opus_samples((const uint8_t*)"\x83\x30", 2) == 5760, "opus: config 16 (2.5 ms) code 3 with 48 frames is 120 ms");
    CHECK(rtc_opus_samples((const uint8_t*)"\x83\x31", 2) < 0, "opus: 49 frames of 2.5 ms exceed 120 ms (RFC 6716 3.2.5)");
    CHECK(rtc_opus_samples((const uint8_t*)"\x1b\x02", 2) == 5760 && rtc_opus_samples((const uint8_t*)"\x1b\x03", 2) < 0,
        "opus: config 3 (60 ms) code 3: two frames fit, three do not");
    CHECK(rtc_opus_samples((const uint8_t*)"\x6a\x00", 2) == 1920, "opus: config 13 (hybrid FB 20 ms) code 2 is two frames");
    CHECK(rtc_opus_samples((const uint8_t*)"\x03\x00", 2) < 0 && rtc_opus_samples((const uint8_t*)"\x03", 1) < 0 && rtc_opus_samples(NULL, 0) < 0,
        "opus malformed: code 3 with zero frames, without a count byte, empty packet");
}

static void test_vp8(void)
{
    uint8_t p[64], frame[3000], pk[1300], back[3000];
    rtc_vp8_desc d;
    rtc_vp8_pay s;
    size_t n, k, got = 0, npk = 0;
    int last = 0, i, ok = 1;
    n = hex("90 80 81 23 10 02 9d", p);
    CHECK(rtc_vp8_desc_parse(p, n, &d) == 4 && d.start && d.partition == 0 && d.picture_id == 0x123 && d.tl0picidx == -1 && d.tid == -1,
        "vp8: X I M with a 15-bit PictureID (RFC 7741 4.2)");
    CHECK(rtc_vp8_is_keyframe(p, n), "vp8: S=1 PID=0 and P=0 in the payload header is a key frame (RFC 7741 4.3)");
    n = hex("b0 f0 81 23 05 b2 01", p);
    CHECK(rtc_vp8_desc_parse(p, n, &d) == 6 && d.non_ref && d.picture_id == 0x123 && d.tl0picidx == 5 && d.tid == 2 && d.layer_sync == 1 && d.keyidx == 0x12,
        "vp8: every optional field: N, PictureID, TL0PICIDX, TID/Y/KEYIDX");
    CHECK(!rtc_vp8_is_keyframe(p, n), "vp8: P=1 is an inter frame");
    n = hex("90 80 05 aa", p);
    CHECK(rtc_vp8_desc_parse(p, n, &d) == 3 && d.picture_id == 5, "vp8: 7-bit PictureID");
    n = hex("00 10", p);
    CHECK(rtc_vp8_desc_parse(p, n, &d) == 1 && !rtc_vp8_is_keyframe(p, n), "vp8: S=0 continuation is never a key frame");
    CHECK(rtc_vp8_desc_parse(p, hex("80", p), &d) < 0, "vp8 malformed: X without the extension octet");
    CHECK(rtc_vp8_desc_parse(p, hex("90 80 81", p), &d) < 0, "vp8 malformed: M=1 with one PictureID octet");
    CHECK(rtc_vp8_desc_parse(p, hex("90 40", p), &d) < 0, "vp8 malformed: L without TL0PICIDX");
    CHECK(rtc_vp8_desc_parse(p, hex("90 80 81 23", p), &d) < 0, "vp8 malformed: descriptor without payload");
    for (i = 0; i < 3000; i++)
        frame[i] = (uint8_t)(i * 13 + 7);
    rtc_vp8_pay_init(&s, frame, sizeof(frame), 1200, 0x1234);
    while ((k = rtc_vp8_pay_next(&s, pk, sizeof(pk), &last)) > 0) {
        int h = rtc_vp8_desc_parse(pk, k, &d);
        ok &= k <= 1200 && h == 4 && d.picture_id == 0x1234 && d.start == (npk == 0) && last == (got + k - 4 == 3000);
        memcpy(back + got, pk + h, k - (size_t)h);
        got += k - (size_t)h;
        npk++;
    }
    CHECK(ok && npk == 3 && got == 3000 && !memcmp(back, frame, 3000), "vp8 pay: 3000 bytes at MTU 1200 in 3 packets, S only on the first, last on the last");
    rtc_vp8_pay_init(&s, frame, 10, 1200, -1);
    k = rtc_vp8_pay_next(&s, pk, sizeof(pk), &last);
    CHECK(k == 11 && pk[0] == 0x10 && last && rtc_vp8_pay_next(&s, pk, sizeof(pk), &last) == 0, "vp8 pay: no PictureID gives the 1-octet descriptor");
}

static size_t h264_depay_all(const uint8_t* pk, const size_t* len, size_t n, uint8_t* out, size_t cap, int* key)
{
    rtc_h264_depay s;
    size_t i, o = 0;
    const uint8_t* q = pk;
    rtc_h264_depay_reset(&s);
    for (i = 0; i < n; q += len[i], i++)
        if (rtc_h264_depay_push(&s, q, len[i], out, cap, &o) < 0)
            return 0;
    *key = s.keyframe;
    return s.in_fu ? 0 : o;
}

static void test_h264(void)
{
    static uint8_t f[4000], want[4000], pk[8][1300], flat[12000], out[12000];
    size_t len[8], n, w, o, k, fn;
    rtc_h264_depay s;
    rtc_h264_pay ps;
    uint32_t pos = 0, st, ln;
    int key = 0, last = 0, i, np, ok;
    n = hex("65 88 84 21", f);
    o = 0;
    rtc_h264_depay_reset(&s);
    CHECK(rtc_h264_depay_push(&s, f, n, out, sizeof(out), &o) == 0 && o == 8 && !memcmp(out, "\0\0\0\1\x65\x88\x84\x21", 8) && s.keyframe,
        "h264: single NAL unit packet, IDR (RFC 6184 5.6)");
    n = hex("78 0002 6742 0002 68ce 0003 658881", f);
    o = 0;
    rtc_h264_depay_reset(&s);
    w = hex("00000001 6742 00000001 68ce 00000001 658881", want);
    CHECK(rtc_h264_depay_push(&s, f, n, out, sizeof(out), &o) == 0 && o == w && !memcmp(out, want, w) && s.keyframe && rtc_h264_is_keyframe(f, n),
        "h264: STAP-A with SPS, PPS, IDR (RFC 6184 5.7.1)");
    {
        uint8_t a[4], b[3], c[3];
        size_t la = hex("7c 85 aa bb", a), lb = hex("7c 05 cc", b), lc = hex("7c 45 dd", c);
        o = 0;
        rtc_h264_depay_reset(&s);
        w = hex("00000001 65 aabb cc dd", want);
        CHECK(rtc_h264_depay_push(&s, a, la, out, sizeof(out), &o) == 0 && s.in_fu && rtc_h264_depay_push(&s, b, lb, out, sizeof(out), &o) == 0
                && rtc_h264_depay_push(&s, c, lc, out, sizeof(out), &o) == 0 && !s.in_fu && o == w && !memcmp(out, want, w) && s.keyframe
                && rtc_h264_is_keyframe(a, la) && !rtc_h264_is_keyframe(b, lb),
            "h264: FU-A start, middle, end rebuild the NAL header from indicator NRI and header type (RFC 6184 5.8)");
        o = 0;
        rtc_h264_depay_reset(&s);
        CHECK(rtc_h264_depay_push(&s, b, lb, out, sizeof(out), &o) < 0, "h264 malformed: FU-A continuation without a start (lost first fragment)");
        CHECK(rtc_h264_depay_push(&s, a, la, out, sizeof(out), &o) == 0 && rtc_h264_depay_push(&s, c, lc, out, sizeof(out), &o) == 0
                && rtc_h264_depay_push(&s, b, lb, out, sizeof(out), &o) < 0,
            "h264 malformed: a same-type FU-A continuation after the unit ended");
        rtc_h264_depay_reset(&s);
        CHECK(rtc_h264_depay_push(&s, a, la, out, sizeof(out), &o) == 0 && rtc_h264_depay_push(&s, (const uint8_t*)"\x41\x9a", 2, out, sizeof(out), &o) < 0,
            "h264 malformed: a new NAL while a FU-A is open (lost last fragment)");
        rtc_h264_depay_reset(&s);
        CHECK(rtc_h264_depay_push(&s, a, la, out, sizeof(out), &o) == 0 && rtc_h264_depay_push(&s, (const uint8_t*)"\x7c\x41\xee", 3, out, sizeof(out), &o) < 0,
            "h264 malformed: FU-A type changes mid-unit");
    }
    rtc_h264_depay_reset(&s);
    o = 0;
    CHECK(rtc_h264_depay_push(&s, (const uint8_t*)"\x7c\xc5\xaa", 3, out, sizeof(out), &o) < 0, "h264 malformed: FU-A with S and E both set");
    n = hex("78 0002 6742 0009 68ce", f);
    CHECK(rtc_h264_depay_push(&s, f, n, out, sizeof(out), &o) < 0 && o == 0, "h264 malformed: STAP-A size past the end writes nothing");
    n = hex("78 0002 6742 0003 780001", f);
    CHECK(rtc_h264_depay_push(&s, f, n, out, sizeof(out), &o) < 0, "h264 malformed: aggregation inside STAP-A");
    n = hex("78 0000 6742", f);
    CHECK(rtc_h264_depay_push(&s, f, n, out, sizeof(out), &o) < 0, "h264 malformed: zero-size STAP-A unit");
    for (i = 25; i <= 31; i++) {
        uint8_t b2[4] = { (uint8_t)(0x60 | i), 0x05, 1, 2 };
        if (i == 28)
            continue;
        rtc_h264_depay_reset(&s);
        CHECK(rtc_h264_depay_push(&s, b2, 4, out, sizeof(out), &o) < 0, "h264: type %d is outside packetization-mode 1 and rejected", i);
    }
    CHECK(rtc_h264_depay_push(&s, (const uint8_t*)"\xe5\x01", 2, out, sizeof(out), &o) < 0, "h264: F=1 (forbidden_zero_bit) dropped");
    o = 0;
    CHECK(rtc_h264_depay_push(&s, (const uint8_t*)"\x65\x01\x02", 3, out, 6, &o) == -2 && o == 0, "h264: no room for start code + NAL");
    n = hex("00000001 6742001f 000001 68ce3c80 0000 00000001 65", f);
    for (i = 0; i < 2999; i++)
        f[n + (size_t)i] = (uint8_t)(1 + i % 250);
    fn = n + 2999;
    w = hex("00000001 6742001f 00000001 68ce3c80 00000001 65", want);
    memcpy(want + w, f + n, 2999);
    w += 2999;
    CHECK(rtc_h264_next_nal(f, fn, &pos, &st, &ln) && ln == 4 && f[st] == 0x67 && rtc_h264_next_nal(f, fn, &pos, &st, &ln) && ln == 4 && f[st] == 0x68,
        "h264 annex B: 4- and 3-byte start codes, trailing zero octets trimmed (H.264 B.1)");
    rtc_h264_pay_init(&ps, f, fn, 1200);
    np = 0;
    ok = 1;
    while (np < 8 && (k = rtc_h264_pay_next(&ps, pk[np], sizeof(pk[0]), &last)) > 0) {
        len[np] = k;
        ok &= k <= 1200 && last == (np == 3);
        np++;
    }
    CHECK(ok && np == 4 && (pk[0][0] & 0x1F) == 24 && (pk[1][0] & 0x1F) == 28 && pk[1][1] == 0x85 && pk[2][1] == 0x05 && pk[3][1] == 0x45
            && len[1] == 1200 && len[3] == 2 + 2999 - 2 * 1198,
        "h264 pay: SPS+PPS aggregated in STAP-A, the 3000-byte IDR in three FU-A at MTU 1200, last only on the final packet");
    CHECK(pk[0][0] == 0x78, "h264 pay: STAP-A F is the OR and NRI the maximum of its units (RFC 6184 5.7.1)");
    for (i = 0, o = 0; i < np; i++) {
        memcpy(flat + o, pk[i], len[i]);
        o += len[i];
    }
    k = h264_depay_all(flat, len, (size_t)np, out, sizeof(out), &key);
    CHECK(k == w && !memcmp(out, want, w) && key, "h264: depacketized packets are the access unit with 4-byte start codes");
    pos = 0;
    n = hex("00000001 41 7f 00 01 05", f);
    CHECK(rtc_h264_next_nal(f, n, &pos, &st, &ln) && ln == 5 && !rtc_h264_next_nal(f, n, &pos, &st, &ln), "h264 annex B: a single zero before 01 is NAL data, not a start code");
    n = hex("00000001 06 05 01 00000001 67 42", f);
    rtc_h264_pay_init(&ps, f, n, 1200);
    CHECK(rtc_h264_pay_next(&ps, pk[0], sizeof(pk[0]), &last) == 1 + 2 + 3 + 2 + 2 && pk[0][0] == 0x78 && last,
        "h264 pay: STAP-A NRI is the highest of its units even when the first is lower");
    n = hex("00000001 41 9a", f);
    rtc_h264_pay_init(&ps, f, n, 1200);
    CHECK(rtc_h264_pay_next(&ps, pk[0], sizeof(pk[0]), &last) == 2 && pk[0][0] == 0x41 && last && rtc_h264_pay_next(&ps, pk[0], sizeof(pk[0]), &last) == 0,
        "h264 pay: a lone small NAL goes as a single NAL unit packet, not a one-unit STAP-A");
    for (i = 0; i < 1200; i++)
        f[4 + i] = (uint8_t)(1 + i % 200);
    f[4] = 0x41;
    rtc_h264_pay_init(&ps, f, 1204, 1200);
    CHECK(rtc_h264_pay_next(&ps, pk[0], sizeof(pk[0]), &last) == 1200 && pk[0][0] == 0x41 && last, "h264 pay: a NAL of exactly the MTU is not fragmented");
    rtc_h264_pay_init(&ps, f, 1205, 1200);
    f[1204] = 7;
    CHECK(rtc_h264_pay_next(&ps, pk[0], sizeof(pk[0]), &last) == 1200 && (pk[0][0] & 0x1F) == 28 && !last, "h264 pay: one octet over the MTU becomes FU-A");
}

static int exact(int which, const char* hx)
{
    uint8_t tmp[64], *b;
    size_t n = hex(hx, tmp);
    int r;
    rtc_opus_info o;
    rtc_vp8_hdr h;
    b = malloc(n);
    if (!b)
        return 0;
    memcpy(b, tmp, n);
    r = which ? rtc_vp8_hdr_parse(b, n, &h) : rtc_opus_parse(b, n, &o);
    free(b);
    return r;
}

static void test_opus_parse(void)
{
    static uint8_t p[1400];
    rtc_opus_info o;
    size_t n;
    int i;
    CHECK(rtc_opus_parse((const uint8_t*)"\x08", 1, &o) == 960 && o.nframes == 1 && o.frame_len[0] == 0 && o.code == 0 && o.config == 1,
        "opus: a TOC-only code 0 packet is one zero-length frame (DTX, RFC 6716 3.2.1 and RFC 7587 4.2)");
    memset(p, 0xaa, sizeof(p));
    p[0] = 0x08;
    CHECK(rtc_opus_parse(p, 1276, &o) == 960 && o.frame_len[0] == 1275 && rtc_opus_parse(p, 1277, &o) < 0,
        "opus [R2]: a code 0 frame of 1275 bytes is valid, 1276 is not (RFC 6716 3.4)");
    p[0] = 0x0d;
    CHECK(rtc_opus_parse(p, 1, &o) == 1920 && o.nframes == 2 && o.frame_len[0] == 0 && o.stereo, "opus code 1: N=1 gives two empty frames; s bit is stereo");
    CHECK(rtc_opus_parse(p, 9, &o) == 1920 && o.frame_len[0] == 4 && o.frame_off[1] == 5, "opus code 1: two 4-byte frames (RFC 6716 Figure 4)");
    CHECK(rtc_opus_parse(p, 10, &o) < 0, "opus [R3]: code 1 with an odd N-1 is rejected");
    CHECK(rtc_opus_parse(p, 2552, &o) < 0 && rtc_opus_parse(p, 2551, &o) == 1920, "opus [R2]: code 1 frames of 1275 fit, 1275.5 do not");
    n = hex("0a 03 a1a2a3 b1b2", p);
    CHECK(rtc_opus_parse(p, n, &o) == 1920 && o.frame_len[0] == 3 && o.frame_off[0] == 2 && o.frame_len[1] == 2 && o.frame_off[1] == 5,
        "opus code 2: one-byte length N1=3, the second frame is the rest (RFC 6716 Figure 5)");
    p[0] = 0x0a;
    p[1] = 252;
    p[2] = 1;
    memset(p + 3, 0x55, 260);
    CHECK(rtc_opus_parse(p, 3 + 256 + 4, &o) == 1920 && o.frame_len[0] == 256 && o.frame_off[0] == 3 && o.frame_len[1] == 4,
        "opus code 2: two-byte length 252 + 4*1 = 256 (RFC 6716 3.1)");
    CHECK(rtc_opus_parse(p, 2 , &o) < 0, "opus [R4]: code 2 truncated inside a two-byte length");
    CHECK(rtc_opus_parse(p, 3 + 255, &o) < 0, "opus [R4]: code 2 with N1 past the end");
    p[1] = 0;
    CHECK(rtc_opus_parse(p, 2, &o) == 1920 && o.frame_len[0] == 0 && o.frame_len[1] == 0, "opus code 2: N1=0 and an empty second frame");
    CHECK(rtc_opus_parse(p, 2 + 1276, &o) < 0, "opus [R2]: code 2 second frame over 1275");
    n = hex("0b 43 02 a1a2a3a4 b1b2b3b4 c1c2c3c4 0000", p);
    CHECK(rtc_opus_parse(p, n, &o) == 2880 && o.nframes == 3 && o.padding == 2 && o.frame_len[2] == 4 && o.frame_off[0] == 3 && o.frame_off[2] == 11 && !o.vbr,
        "opus code 3 CBR with 2 padding bytes (RFC 6716 Figure 6)");
    CHECK(rtc_opus_parse(p, n - 1, &o) < 0, "opus [R6]: CBR payload not a multiple of M");
    n = hex("0b 83 02 05 a1a2 b1b2b3b4b5 c1c2c3", p);
    CHECK(rtc_opus_parse(p, n, &o) == 2880 && o.vbr && o.frame_len[0] == 2 && o.frame_len[1] == 5 && o.frame_len[2] == 3 && o.frame_off[1] == 6 && o.frame_off[2] == 11,
        "opus code 3 VBR: M-1 length fields, the last frame is the rest (RFC 6716 Figure 7)");
    n = hex("0b 83 06 05 a1a2 b1b2b3b4b5 c1c2c3", p);
    CHECK(rtc_opus_parse(p, n, &o) < 0, "opus [R7]: VBR lengths past the end");
    n = hex("0b c3 01 02 05 a1a2 b1b2b3b4b5 c1c2c3 00", p);
    CHECK(rtc_opus_parse(p, n, &o) == 2880 && o.padding == 1 && o.frame_len[2] == 3, "opus code 3 VBR with padding: the last frame excludes the padding");
    n = hex("0b c3 01 02 05 a1a2 b1b2b3b4b5", p);
    CHECK(rtc_opus_parse(p, n, &o) < 0, "opus [R7]: VBR lengths reach into the padding");
    p[0] = 0x0b;
    p[1] = 0x41;
    p[2] = 0xff;
    p[3] = 0x01;
    memset(p + 4, 0, 300);
    CHECK(rtc_opus_parse(p, 4 + 10 + 255, &o) == 960 && o.padding == 255 && o.frame_len[0] == 10 && o.frame_off[0] == 4,
        "opus code 3 padding chain: 255 means 254 more plus another length byte (RFC 6716 3.2.5)");
    CHECK(rtc_opus_parse(p, 4 + 254, &o) < 0 && rtc_opus_parse(p, 3, &o) < 0, "opus [R6]: padding longer than the packet; padding length byte missing");
    p[1] = 0x01;
    CHECK(rtc_opus_parse(p, 2 + 1276, &o) < 0 && rtc_opus_parse(p, 2 + 1275, &o) == 960, "opus [R2]: code 3 CBR frame over 1275");
    p[1] = 0x00;
    CHECK(rtc_opus_parse(p, 5, &o) < 0, "opus [R5]: code 3 with M=0");
    for (i = 0; i < 32; i++) {
        static const uint16_t ms10[32] = { 100, 200, 400, 600, 100, 200, 400, 600, 100, 200, 400, 600, 100, 200, 100, 200, 25, 50, 100, 200, 25, 50, 100, 200, 25, 50, 100, 200, 25, 50, 100, 200 };
        uint32_t mx = 1200u / ms10[i];
        p[0] = (uint8_t)(i << 3 | 3);
        p[1] = (uint8_t)mx;
        p[2] = (uint8_t)(mx + 1);
        CHECK(rtc_opus_parse(p, 2, &o) == (int)(mx * ms10[i] * 48 / 10) && (p[1] = (uint8_t)(mx + 1), rtc_opus_parse(p, 2, &o) < 0),
            "opus [R5] config %d (%u.%u ms frames): %u frames reach 120 ms, one more does not", i, ms10[i] / 10, ms10[i] % 10, mx);
    }
    CHECK(exact(0, "0a fc") < 0, "opus [R4]: a two-byte length cut after its first byte is rejected without reading past the packet (exact heap buffer)");
    CHECK(exact(0, "0b c2 ff 00") < 0, "opus [R6]: padding longer than the packet is rejected without reading past it (exact heap buffer)");
    CHECK(exact(0, "0b c2 01 01 aa 00") == 1920, "opus code 3 VBR padded: exact buffer, frames 1 and 0");
    CHECK(rtc_opus_parse(NULL, 0, &o) < 0 && rtc_opus_parse(p, 0, &o) < 0, "opus [R1]: empty packet");
}

static void test_g711(void)
{
    uint8_t s[2000], out[2000];
    rtc_g711_pay g;
    uint32_t d = 0, tot = 0;
    size_t k, sz[8];
    int i, np = 0, ok = 1;
    for (i = 0; i < 2000; i++)
        s[i] = (uint8_t)(i * 7);
    rtc_g711_pay_init(&g, s, 400, 20);
    while (np < 8 && (k = rtc_g711_pay_next(&g, out, sizeof(out), &d)) > 0) {
        ok &= d == k && !memcmp(out, s + tot, k);
        sz[np++] = k;
        tot += d;
    }
    CHECK(ok && np == 3 && sz[0] == 160 && sz[1] == 160 && sz[2] == 80 && tot == 400,
        "g711: 20 ms at 8000 Hz is 160 one-octet samples per packet and the timestamp advances by the sample count (RFC 3551 4.5.14)");
    rtc_g711_pay_init(&g, s, 400, 0);
    CHECK(rtc_g711_pay_next(&g, out, sizeof(out), &d) == 160, "g711: ptime 0 falls back to 20 ms");
    rtc_g711_pay_init(&g, s, 400, 30);
    CHECK(rtc_g711_pay_next(&g, out, sizeof(out), &d) == 240 && d == 240, "g711: ptime 30 gives 240 samples");
    rtc_g711_pay_init(&g, s, 2000, 200);
    CHECK(rtc_g711_pay_next(&g, out, sizeof(out), &d) == 1200, "g711: ptime is capped at 150 ms (1200 samples)");
    rtc_g711_pay_init(&g, s, 400, 20);
    CHECK(rtc_g711_pay_next(&g, out, 100, &d) == 100 && d == 100 && rtc_g711_pay_next(&g, out, 0, &d) == 0, "g711: a packet never exceeds the buffer");
}

static void test_vp8_ext(void)
{
    uint8_t p[64];
    rtc_vp8_desc d;
    rtc_vp8_hdr h;
    size_t n;
    n = hex("90 10 1f 01", p);
    CHECK(rtc_vp8_desc_parse(p, n, &d) == 3 && d.keyidx == 31 && d.tid == -1 && d.picture_id == -1, "vp8 K without T: KEYIDX only (RFC 7741 4.2)");
    n = hex("90 20 e0 01", p);
    CHECK(rtc_vp8_desc_parse(p, n, &d) == 3 && d.tid == 3 && d.layer_sync == 1 && d.keyidx == -1, "vp8 T without K: TID=3 Y=1");
    CHECK(rtc_vp8_desc_parse(p, hex("90 30", p), &d) < 0 && rtc_vp8_desc_parse(p, hex("90 20", p), &d) < 0,
        "vp8 malformed: T or K set without the TID/Y/KEYIDX octet");
    n = hex("90 80 ff ff 01", p);
    CHECK(rtc_vp8_desc_parse(p, n, &d) == 4 && d.picture_id == 0x7fff, "vp8: 15-bit PictureID 0x7fff");
    n = hex("90 80 7f 01", p);
    CHECK(rtc_vp8_desc_parse(p, n, &d) == 3 && d.picture_id == 0x7f, "vp8: 7-bit PictureID 0x7f");
    n = hex("90 40 ab 01", p);
    CHECK(rtc_vp8_desc_parse(p, n, &d) == 3 && d.tl0picidx == 0xab && d.picture_id == -1, "vp8: TL0PICIDX alone");
    n = hex("90 c0 81 02 ab 01", p);
    CHECK(rtc_vp8_desc_parse(p, n, &d) == 5 && d.picture_id == 0x102 && d.tl0picidx == 0xab, "vp8: PictureID then TL0PICIDX order");
    n = hex("90 f0 05 ab 40 01", p);
    CHECK(rtc_vp8_desc_parse(p, n, &d) == 5 && d.picture_id == 5 && d.tl0picidx == 0xab && d.tid == 1 && d.keyidx == 0, "vp8: I L T K with a 7-bit PictureID");
    CHECK(rtc_vp8_desc_parse(p, hex("90 f0 05 ab", p), &d) < 0, "vp8 malformed: TID/KEYIDX octet missing after TL0PICIDX");
    n = hex("58 01", p);
    CHECK(rtc_vp8_desc_parse(p, n, &d) == 1 && d.start && d.partition == 0 && rtc_vp8_is_keyframe(p, n) == 0,
        "vp8: both R bits are ignored by the receiver; P=1 is not a key frame");
    n = hex("17 01", p);
    CHECK(rtc_vp8_desc_parse(p, n, &d) == 1 && d.start && d.partition == 7, "vp8: partition index 7");
    n = hex("f0 00 00 9d 01 2a 80 82 e0 41 01 02 03 04 05 06 07", p);
    CHECK(rtc_vp8_hdr_parse(p, n, &h) == 10 && h.key && h.show && h.version == 0 && h.first_part == 7 && h.width == 640 && h.height == 480 && h.vscale == 1 && h.hscale == 2,
        "vp8 frame tag + key frame header: 640x480, horizontal scale 2, vertical scale 1, first partition 7 bytes (RFC 6386 9.1, RFC 7741 4.3)");
    CHECK(rtc_vp8_hdr_parse(p, n - 1, &h) < 0, "vp8 malformed: first partition size past the end");
    CHECK(exact(1, "f0 00 00 9d 01 2a 80 02 e0") < 0 && exact(1, "f0 00") < 0, "vp8 malformed: a key frame header cut at 9 bytes and a 2-byte tag, exact heap buffers");
    p[3] = 0x9c;
    CHECK(rtc_vp8_hdr_parse(p, n, &h) < 0, "vp8 malformed: key frame without the 9d 01 2a start code");
    CHECK(rtc_vp8_hdr_parse(p, 9, &h) < 0 && rtc_vp8_hdr_parse(p, 2, &h) < 0, "vp8 malformed: truncated key frame header, truncated tag");
    n = hex("65 00 00 01 02 03", p);
    CHECK(rtc_vp8_hdr_parse(p, n, &h) == 3 && !h.key && h.version == 2 && !h.show && h.first_part == 3, "vp8 inter frame tag: version 2, hidden, 3-byte first partition");
    n = hex("69 00 00 01 02 03", p);
    CHECK(rtc_vp8_hdr_parse(p, n, &h) < 0, "vp8: version 4 is reserved (RFC 6386 9.1)");
}

static uint32_t w2_rng = 12345;
static uint8_t w2_byte(void)
{
    w2_rng = w2_rng * 1103515245u + 12345u;
    return (uint8_t)(1 + (w2_rng >> 16) % 255);
}

static int vp8_round(const uint8_t* f, size_t n, size_t mtu, int32_t pid, size_t* npk)
{
    static uint8_t pk[2000], out[70000];
    rtc_vp8_pay s;
    rtc_vp8_depay dp;
    size_t k, len = 0, dl = pid < 0 ? 1 : 4, want;
    int last = 0, ok = 1;
    *npk = 0;
    rtc_vp8_pay_init(&s, f, n, mtu, pid);
    rtc_vp8_depay_reset(&dp);
    while ((k = rtc_vp8_pay_next(&s, pk, sizeof(pk), &last)) > 0) {
        ok &= k <= mtu && rtc_vp8_depay_push(&dp, pk, k, out, sizeof(out), &len) == 0;
        (*npk)++;
        ok &= last == (len == n);
    }
    want = (n + (mtu - dl) - 1) / (mtu - dl);
    return ok && len == n && !memcmp(out, f, n) && *npk == want && dp.keyframe == !(f[0] & 1u);
}

static void test_vp8_mtu(void)
{
    static uint8_t f[70000], pk[64], out[256];
    rtc_vp8_depay dp;
    size_t mtu, n, np, len;
    int ok = 1, j;
    for (n = 0; n < sizeof(f); n++)
        f[n] = w2_byte();
    for (mtu = RTC_PAY_MIN_MTU; mtu <= 80; mtu++) {
        size_t sizes[] = { 1, mtu - 5, mtu - 4, mtu - 3, 2 * (mtu - 4) - 1, 2 * (mtu - 4), 2 * (mtu - 4) + 1, 7 * (mtu - 4), mtu - 2, mtu - 1, mtu, mtu + 1 };
        for (j = 0; j < (int)(sizeof(sizes) / sizeof(sizes[0])); j++) {
            ok &= vp8_round(f, sizes[j], mtu, 0x1234, &np);
            ok &= vp8_round(f, sizes[j], mtu, -1, &np);
            ok &= vp8_round(f, sizes[j], mtu, 0x55, &np);
        }
    }
    CHECK(ok, "vp8 pay/depay round trip at every MTU 16..80 and payload sizes at each packet boundary (+-1), with and without PictureID");
    CHECK(vp8_round(f, 1196, 1200, 7, &np) && np == 1 && vp8_round(f, 1197, 1200, 7, &np) && np == 2 && vp8_round(f, 65536, 1200, 7, &np) && np == 55,
        "vp8 at MTU 1200: 1196 bytes fit one packet, 1197 need two, 64 KiB needs 55");
    f[0] &= 0xFE;
    CHECK(vp8_round(f, 3000, 1200, 9, &np), "vp8 depay: P=0 in the first payload octet marks the frame as a key frame");
    rtc_vp8_depay_reset(&dp);
    len = 0;
    CHECK(rtc_vp8_depay_push(&dp, (const uint8_t*)"\x00\x01", 2, out, sizeof(out), &len) < 0 && len == 0, "vp8 depay malformed: continuation before the first packet (S=0)");
    CHECK(rtc_vp8_depay_push(&dp, (const uint8_t*)"\x11\x01", 2, out, sizeof(out), &len) < 0, "vp8 depay malformed: S=1 on partition 1 cannot start a frame");
    CHECK(rtc_vp8_depay_push(&dp, (const uint8_t*)"\x90\x80\x81\x23\x00", 5, out, sizeof(out), &len) == 0 && len == 1 && dp.keyframe,
        "vp8 depay: the first packet of a frame");
    CHECK(rtc_vp8_depay_push(&dp, (const uint8_t*)"\x91\x80\x81\x23\x02", 5, out, sizeof(out), &len) == 0 && len == 2, "vp8 depay: S=1 on partition 1 continues the frame");
    CHECK(rtc_vp8_depay_push(&dp, (const uint8_t*)"\x80\x80\x81\x24\x03", 5, out, sizeof(out), &len) < 0 && len == 2, "vp8 depay malformed: PictureID changes mid-frame (lost marker)");
    CHECK(rtc_vp8_depay_push(&dp, (const uint8_t*)"\x00\x03", 2, out, sizeof(out), &len) < 0, "vp8 depay malformed: PictureID disappears mid-frame");
    CHECK(rtc_vp8_depay_push(&dp, (const uint8_t*)"\x90\x80\x81\x23\x03", 5, out, sizeof(out), &len) < 0, "vp8 depay malformed: a second S=1 PID=0 packet in one frame");
    CHECK(rtc_vp8_depay_push(&dp, (const uint8_t*)"\x80\x80\x81", 3, out, sizeof(out), &len) < 0, "vp8 depay malformed: truncated descriptor");
    CHECK(rtc_vp8_depay_push(&dp, (const uint8_t*)"\x80\x80\x81\x23\x04\x05", 6, out, 3, &len) == -2 && len == 2, "vp8 depay: -2 and nothing written when out of room");
    (void)pk;
}

static size_t h264_build(const size_t* nl, int cnt, uint8_t* f, uint8_t* want, size_t* wlen)
{
    size_t n = 0, w = 0;
    int i;
    for (i = 0; i < cnt; i++) {
        size_t k;
        int sc4 = i & 1;
        memcpy(f + n, sc4 ? "\0\0\0\1" : "\0\0\1", sc4 ? 4 : 3);
        n += sc4 ? 4 : 3;
        memcpy(want + w, "\0\0\0\1", 4);
        w += 4;
        f[n] = want[w] = (uint8_t)(0x60 | (i == 0 ? 5 : 1));
        for (k = 1; k < nl[i]; k++)
            f[n + k] = want[w + k] = w2_byte();
        n += nl[i];
        w += nl[i];
    }
    *wlen = w;
    return n;
}

static int h264_round(const size_t* nl, int cnt, size_t mtu, int* types, int* ntypes)
{
    static uint8_t f[20000], want[20000], pk[2000], out[30000];
    rtc_h264_pay ps;
    rtc_h264_depay ds;
    size_t n, w, k, len = 0;
    int last = 0, ok = 1, np = 0;
    n = h264_build(nl, cnt, f, want, &w);
    rtc_h264_pay_init(&ps, f, n, mtu);
    rtc_h264_depay_reset(&ds);
    while ((k = rtc_h264_pay_next(&ps, pk, sizeof(pk), &last)) > 0) {
        ok &= k <= mtu && rtc_h264_depay_push(&ds, pk, k, out, sizeof(out), &len) == 0;
        if (types && np < 16)
            types[np] = pk[0] & 0x1F;
        np++;
        if (last)
            break;
    }
    ok &= last && rtc_h264_pay_next(&ps, pk, sizeof(pk), &last) == 0;
    if (ntypes)
        *ntypes = np;
    return ok && !ds.in_fu && ds.keyframe && len == w && !memcmp(out, want, w);
}

static void test_h264_mtu(void)
{
    CHECK(rtc_h264_start_hint((const uint8_t*)"\x67\x42", 2) == 1 && rtc_h264_start_hint((const uint8_t*)"\x09\xf0", 2) == 1
            && rtc_h264_start_hint((const uint8_t*)"\x78\x00\x02\x67\x42", 5) == 1 && rtc_h264_start_hint((const uint8_t*)"\x7c\x87\x42", 3) == 1
            && rtc_h264_start_hint((const uint8_t*)"\x7c\x07\x42", 3) < 0 && rtc_h264_start_hint((const uint8_t*)"\x65\x88", 2) < 0
            && rtc_h264_start_hint((const uint8_t*)"\x7c\x85\x88", 3) < 0 && rtc_h264_start_hint((const uint8_t*)"\x68\xce", 2) < 0 && rtc_h264_start_hint((const uint8_t*)"\x67", 1) < 0,
        "h264 start hint: an AUD or SPS (alone, first in a STAP-A, or a FU-A start) begins an access unit (H.264 7.4.1.2.3); an IDR slice, PPS or FU-A continuation is unknown");
    static uint8_t out[64];
    size_t mtu, nl[4];
    rtc_h264_depay s;
    size_t o;
    int ok = 1, t[16], nt = 0, okfit = 1, okover = 1, okfu = 1;
    for (mtu = RTC_PAY_MIN_MTU; mtu <= 120; mtu++) {
        size_t a, sizes[] = { 1, 2, mtu - 1, mtu, mtu + 1, mtu - 2 + 1, 2 * (mtu - 2) + 1, 2 * (mtu - 2) + 2, 3 * (mtu - 2) + 1, 5 * mtu };
        int j;
        for (j = 0; j < (int)(sizeof(sizes) / sizeof(sizes[0])); j++) {
            nl[0] = sizes[j];
            ok &= h264_round(nl, 1, mtu, NULL, NULL);
            nl[1] = 3;
            nl[2] = sizes[(j + 3) % 10];
            ok &= h264_round(nl, 3, mtu, NULL, NULL);
        }
        nl[0] = 1 + (mtu - 5) / 2;
        nl[1] = mtu - 5 - nl[0];
        if (nl[1] >= 1) {
            okfit &= h264_round(nl, 2, mtu, t, &nt) && nt == 1 && t[0] == 24;
            nl[1]++;
            okover &= h264_round(nl, 2, mtu, t, &nt) && nt == 2 && t[0] != 24 && t[1] != 24;
        }
        nl[0] = mtu + 1;
        okfu &= h264_round(nl, 1, mtu, t, &nt) && nt == 2 && t[0] == 28 && t[1] == 28;
        nl[0] = 2 * (mtu - 2) + 2;
        okfu &= h264_round(nl, 1, mtu, t, &nt) && nt == 3;
        for (a = 1; a <= 3; a++) {
            nl[0] = nl[1] = nl[2] = a;
            ok &= h264_round(nl, 3, mtu, t, &nt) && nt == 1 && t[0] == 24;
        }
    }
    CHECK(ok, "h264 pay/depay round trip at every MTU 16..120 with NAL sizes at each single/FU-A boundary (+-1), 3- and 4-byte start codes");
    CHECK(okfit, "h264 pay: two NALs whose STAP-A is exactly the MTU (1 + 2+a + 2+b) share one packet");
    CHECK(okover, "h264 pay: one octet more and they go as two single NAL unit packets");
    CHECK(okfu, "h264 pay: MTU+1 needs two FU-A, 2*(MTU-2)+2 needs three (FU payload is MTU-2)");
    rtc_h264_depay_reset(&s);
    o = 0;
    CHECK(rtc_h264_depay_push(&s, (const uint8_t*)"\x78\x00\x02\x67\x42\x00", 6, out, sizeof(out), &o) < 0 && o == 0, "h264 malformed: STAP-A with a 1-byte length field left over");
    CHECK(rtc_h264_depay_push(&s, (const uint8_t*)"\x78", 1, out, sizeof(out), &o) < 0, "h264 malformed: STAP-A header only");
    CHECK(rtc_h264_depay_push(&s, (const uint8_t*)"\x78\x00\x02\xe7\x42", 5, out, sizeof(out), &o) < 0 && o == 0, "h264 malformed: F=1 inside a STAP-A unit");
    CHECK(rtc_h264_depay_push(&s, (const uint8_t*)"\x78\x00\x01\x67\x00\x01\x68", 7, out, 8, &o) == -2 && o == 0, "h264: STAP-A that does not fit returns -2 with nothing written");
    CHECK(rtc_h264_depay_push(&s, (const uint8_t*)"\x7c\x85", 2, out, sizeof(out), &o) < 0, "h264 malformed: FU-A without payload");
    CHECK(rtc_h264_depay_push(&s, (const uint8_t*)"\x7c\x45\x01", 3, out, sizeof(out), &o) < 0, "h264 malformed: FU-A end without a start");
    CHECK(rtc_h264_depay_push(&s, (const uint8_t*)"\x7c\x80\x01", 3, out, sizeof(out), &o) < 0 && rtc_h264_depay_push(&s, (const uint8_t*)"\x7c\x98\x01", 3, out, sizeof(out), &o) < 0,
        "h264 malformed: FU-A of NAL type 0 or 24 (aggregation cannot be fragmented)");
    CHECK(rtc_h264_depay_push(&s, (const uint8_t*)"\x60\x01", 2, out, sizeof(out), &o) < 0, "h264 malformed: NAL type 0 is unspecified");
}

static void test_nack(void)
{
    static rtc_nack k;
    uint16_t out[64];
    unsigned n, i;
    int ok = 1;
    rtc_nack_init(&k);
    rtc_nack_on_packet(&k, 65533);
    rtc_nack_on_packet(&k, 65534);
    rtc_nack_on_packet(&k, 2);
    n = rtc_nack_poll(&k, 1000, 50, out, 64);
    CHECK(n == 3 && out[0] == 65535 && out[1] == 0 && out[2] == 1, "nack: the gap 65535, 0, 1 across the wrap is requested at once (%u)", n);
    CHECK(rtc_nack_poll(&k, 1049, 50, out, 64) == 0, "nack: no retry before one RTT (RFC 4585 3.5.3 spacing)");
    CHECK(rtc_nack_poll(&k, 1050, 50, out, 64) == 3, "nack: retried after one RTT");
    CHECK(rtc_nack_on_packet(&k, 0) == 1 && k.recovered == 1, "nack: a retransmission is recognised and removed");
    n = rtc_nack_poll(&k, 1100, 50, out, 64);
    CHECK(n == 2 && out[0] == 65535 && out[1] == 1, "nack: the recovered one is no longer requested");
    CHECK(rtc_nack_poll(&k, 1105, 5, out, 64) == 0 && rtc_nack_poll(&k, 1110, 5, out, 64) == 2, "nack: the retry interval has a 10 ms floor");
    for (i = 0; i < 20; i++)
        rtc_nack_poll(&k, 2000 + i * 100, 50, out, 64);
    CHECK(k.n == 0, "nack: an entry is dropped after %u tries", RTC_NACK_MAX_TRIES);
    CHECK(rtc_nack_poll(&k, 0x7FFFFFF0u, 50, out, 64) == 0, "nack: empty list");
    rtc_nack_init(&k);
    rtc_nack_on_packet(&k, 10);
    rtc_nack_on_packet(&k, 8);
    CHECK(k.n == 0 && rtc_nack_poll(&k, 5000, 50, out, 64) == 0, "nack: an old packet behind newest opens no request");
    rtc_nack_on_packet(&k, 10 + 1 + RTC_NACK_MAX + 1);
    CHECK(k.n == 0 && rtc_nack_take_keyreq(&k) == 1 && rtc_nack_take_keyreq(&k) == 0, "nack: a gap over the list size asks for a key frame instead");
    rtc_nack_init(&k);
    for (i = 1; i <= 12000; i++)
        if (i % 1000 != 500)
            rtc_nack_on_packet(&k, (uint16_t)i);
    CHECK(k.n == 10, "nack: 12 single losses, the two more than 10000 behind dropped (%u)", k.n);
    ok = k.n > 0;
    for (i = 0; i < k.n; i++)
        ok &= k.newest - k.e[i].ext <= RTC_NACK_MAX_AGE;
    CHECK(ok && k.n < 12, "nack: entries more than %u behind the newest are dropped", RTC_NACK_MAX_AGE);
    rtc_nack_init(&k);
    rtc_nack_on_packet(&k, 0);
    rtc_nack_on_packet(&k, 5);
    CHECK(rtc_nack_poll(&k, 0xFFFFFFF0u, 20, out, 2) == 2 && rtc_nack_poll(&k, 0xFFFFFFF0u, 20, out, 64) == 2
            && rtc_nack_poll(&k, 0x00000004u, 20, out, 64) == 4,
        "nack: max honoured; retry interval measured across the 32-bit ms wrap");
}

static uint8_t jb_payload[1500];

static int jb_put(rtc_jb* j, uint16_t seq, uint32_t ts, int marker, int start, int key, uint32_t now)
{
    jb_payload[0] = (uint8_t)seq;
    return rtc_jb_insert(j, seq, ts, marker, start, key, now, jb_payload, 100);
}

static void test_jbuf(void)
{
    rtc_jb j;
    rtc_jb_frame f;
    size_t l = 0;
    const uint8_t* p;
    uint32_t i;
    CHECK(rtc_jb_init(&j, 100, 1 << 16, 0) < 0, "jb: slot count must be a power of two");
    rtc_jb_init(&j, 64, 1 << 18, 0);
    jb_put(&j, 65534, 9000, 0, 1, 1, 100);
    jb_put(&j, 0, 9000, 1, 0, 0, 104);
    CHECK(!rtc_jb_pop(&j, 200, &f), "jb: the middle packet 65535 is missing, no frame");
    jb_put(&j, 65535, 9000, 0, 0, 0, 102);
    CHECK(rtc_jb_pop(&j, 200, &f) && f.npkts == 3 && f.key && f.ts == 9000 && f.first_ms == 100 && f.first_ext + 2 == f.last_ext,
        "jb: three packets across the wrap reorder into one key frame");
    p = rtc_jb_pkt(&j, f.first_ext + 1, &l);
    CHECK(p && l == 100 && p[0] == 0xff, "jb: the frame's packets stay readable after the pop");
    CHECK(j.st.emitted == 1 && j.st.delay_ms == 100 && j.st.multi == 1 && j.st.keyframes == 1, "jb: jitterBufferDelay counts from the first packet");
    CHECK(jb_put(&j, 65535, 9000, 0, 0, 0, 210) < 0 && j.st.late == 1, "jb: a packet of a released frame is late");
    rtc_jb_set_target(&j, 40, 500);
    jb_put(&j, 1, 12000, 1, 1, 0, 300);
    CHECK(!rtc_jb_pop(&j, 339, &f) && rtc_jb_pop(&j, 340, &f) && f.ts == 12000 && j.st.target_ms == 40,
        "jb: jitterBufferTarget 40 ms holds a complete frame until 40 ms after its first packet");
    CHECK(jb_put(&j, 3, 18000, 1, 1, 0, 350) == 0 && jb_put(&j, 3, 18000, 1, 1, 0, 351) == 1 && j.st.dups == 1, "jb: duplicate");
    CHECK(!rtc_jb_pop(&j, 700, &f), "jb: seq 2 is missing and frame 3 is a delta frame: wait for it or a key frame");
    jb_put(&j, 4, 21000, 0, 1, 1, 700);
    jb_put(&j, 5, 21000, 1, 0, 0, 700);
    CHECK(!rtc_jb_pop(&j, 799, &f), "jb: a later key frame is not jumped to before the head has waited max_wait");
    CHECK(rtc_jb_pop(&j, 850, &f) && f.ts == 21000 && f.key && j.st.frames_dropped == 1, "jb: after max_wait the head skips to the key frame, frame 3 dropped");
    rtc_jb_free(&j);
    rtc_jb_init(&j, 64, 1 << 18, 0);
    jb_put(&j, 100, 3000, 0, 1, 1, 0);
    jb_put(&j, 101, 3000, 0, -1, 0, 0);
    jb_put(&j, 102, 6000, 0, -1, 0, 0);
    CHECK(rtc_jb_pop(&j, 0, &f) && f.ts == 3000 && f.npkts == 2 && f.last_ext == f.first_ext + 1,
        "jb: without a marker a timestamp change ends the frame (the first packet has an explicit start)");
    jb_put(&j, 104, 9000, 1, -1, 0, 0);
    CHECK(!rtc_jb_pop(&j, 0, &f), "jb: 103 missing: frame 6000 is open");
    jb_put(&j, 103, 6000, 1, -1, 0, 0);
    CHECK(rtc_jb_pop(&j, 0, &f) && f.ts == 6000 && rtc_jb_pop(&j, 0, &f) && f.ts == 9000, "jb: then both frames in order");
    jb_put(&j, 106, 15000, 1, -1, 0, 0);
    CHECK(!rtc_jb_pop(&j, 0, &f), "jb: derived start needs the previous packet");
    rtc_jb_free(&j);
    rtc_jb_init(&j, 64, 1 << 18, 0);
    rtc_jb_set_target(&j, 0, 100);
    jb_put(&j, 200, 1000, 0, 1, 1, 0);
    jb_put(&j, 202, 2000, 0, -1, 1, 0);
    jb_put(&j, 203, 2000, 1, -1, 1, 0);
    CHECK(!rtc_jb_pop(&j, 500, &f), "jb: a packet whose predecessor has the same timestamp and no marker is never a frame start");
    jb_put(&j, 205, 3000, 1, 1, 0, 600);
    jb_put(&j, 204, 3000, 0, 1, 0, 610);
    jb_put(&j, 201, 1000, 1, -1, 0, 620);
    CHECK(rtc_jb_pop(&j, 620, &f) && f.ts == 1000 && rtc_jb_pop(&j, 620, &f) && f.ts == 2000 && rtc_jb_pop(&j, 620, &f) && f.first_ms == 600,
        "jb: a frame's first packet time is the earliest arrival of any of its packets");
    rtc_jb_free(&j);
    rtc_jb_init(&j, 16, 1 << 16, 1);
    for (i = 0; i < 5; i++)
        if (i != 2)
            jb_put(&j, (uint16_t)(500 + i), i * 960, 0, 0, 0, i * 20);
    CHECK(rtc_jb_pop(&j, 80, &f) && rtc_jb_pop(&j, 80, &f) && !rtc_jb_pop(&j, 80, &f), "jb audio: every packet is a frame; stop at the hole");
    rtc_jb_set_target(&j, 0, 60);
    CHECK(rtc_jb_pop(&j, 120, &f) && f.ts == 3 * 960 && j.st.frames_dropped == 0, "jb audio: after max_wait the hole is skipped");
    for (i = 0; i < 40; i++)
        jb_put(&j, (uint16_t)(600 + i), i * 960, 0, 0, 0, 200);
    CHECK(j.next_ext >= 0x10000u + 600 + 40 - 16 && j.st.frames_dropped > 0, "jb: a packet more than the slot count ahead advances the head");
    CHECK(rtc_jb_bytes(&j) == 16 * sizeof(rtc_jb_slot) + (1 << 16) + 16 * sizeof(rtc_pkt_slot), "jb: fixed memory per receiver");
    rtc_jb_free(&j);
    rtc_jb_init(&j, 16, 1 << 16, 1);
    jb_put(&j, 1, 0, 0, 0, 0, 0);
    jb_put(&j, 30001, 0, 0, 0, 0, 0);
    CHECK(j.next_ext == 0x10000u + 30001 - 15 && j.slot[0x10001u & j.mask].ext != 0x10001u && j.st.frames_dropped == 1, "jb: a jump of 30000 clears the slots without walking them");
    rtc_jb_free(&j);
}

static void test_twcc(void)
{
    static rtc_twcc_rx rx;
    static rtc_twcc_pkt pk[4096];
    uint8_t p[1500], want[64];
    size_t n, w;
    rtc_rtcp_iter it;
    rtc_rtcp_pkt k;
    rtc_rtcp_fb fb;
    rtc_twcc_fb h;
    uint32_t i, seen = 0, rng = 7, lost = 0;
    int r, ok = 1;
    int64_t at[1200];
    n = hex("afcd0006 11111111 22222222 0064 0006 000001 07 d490 04 08 fffe 01 01", p);
    rtc_rtcp_iter_init(&it, p, n);
    CHECK(rtc_rtcp_next(&it, &k) == 1 && rtc_rtcp_fb_parse(&k, &fb) == 0 && (r = rtc_twcc_parse(&fb, &h, pk, 4096)) == 6 && h.base == 100
            && h.fb_count == 7 && h.ref_us == 64000,
        "twcc: header base 100, count 6, reference 1 x 64 ms, feedback count 7 (draft-holmer-rmcat-transport-wide-cc-extensions-01 3.1)");
    CHECK(pk[0].received && pk[0].at_us == 65000 && pk[1].at_us == 67000 && !pk[2].received && pk[3].at_us == 66500 && pk[4].at_us == 66750
            && !pk[5].received && pk[5].seq == 105,
        "twcc: two-bit status vector 0xd490 with small, large (-500 us) deltas and losses");
    n = hex("8fcd0005 11111111 22222222 0000 012c 000000 00 212c 0000", p);
    rtc_rtcp_iter_init(&it, p, n);
    CHECK(rtc_rtcp_next(&it, &k) == 1 && rtc_rtcp_fb_parse(&k, &fb) == 0 && rtc_twcc_parse(&fb, &h, pk, 4096) < 0, "twcc malformed: run of 300 received with no deltas");
    n = hex("8fcd0006 11111111 22222222 0000 000e 000000 00 ac01 01020304 0000", p);
    rtc_rtcp_iter_init(&it, p, n);
    CHECK(rtc_rtcp_next(&it, &k) == 1 && rtc_rtcp_fb_parse(&k, &fb) == 0 && rtc_twcc_parse(&fb, &h, pk, 4096) == 14 && pk[0].received
            && !pk[1].received && pk[2].received && pk[3].received && !pk[4].received && pk[13].received,
        "twcc: one-bit status vector 0xac01");
    n = hex("8fcd0005 11111111 22222222 0000 0002 000000 00 0000 0002", p);
    rtc_rtcp_iter_init(&it, p, n);
    CHECK(rtc_rtcp_next(&it, &k) == 1 && rtc_rtcp_fb_parse(&k, &fb) == 0 && rtc_twcc_parse(&fb, &h, pk, 4096) < 0, "twcc malformed: a run-length chunk of length 0 (even when a valid chunk follows)");
    n = hex("8fcd0004 11111111 22222222 0000 0002 000000 00 f000 0000", p);
    rtc_rtcp_iter_init(&it, p, n);
    CHECK(rtc_rtcp_next(&it, &k) == 1 && rtc_rtcp_fb_parse(&k, &fb) == 0 && rtc_twcc_parse(&fb, &h, pk, 4096) < 0, "twcc malformed: reserved symbol 3");
    n = hex("8fcd0003 11111111 22222222 0000 0020 000000 00", p);
    rtc_rtcp_iter_init(&it, p, n);
    CHECK(rtc_rtcp_next(&it, &k) == 1 && rtc_rtcp_fb_parse(&k, &fb) == 0 && rtc_twcc_parse(&fb, &h, pk, 4096) < 0, "twcc malformed: status count without chunks");
    n = hex("8fcd0005 11111111 22222222 0000 0001 ffffff 00 2001 05 000000", p);
    rtc_rtcp_iter_init(&it, p, n);
    CHECK(rtc_rtcp_next(&it, &k) == 1 && rtc_rtcp_fb_parse(&k, &fb) == 0 && rtc_twcc_parse(&fb, &h, pk, 4096) == 1 && h.ref_us == -64000
            && pk[0].at_us == -62750,
        "twcc: the 24-bit reference time is signed");
    rtc_twcc_rx_init(&rx);
    rtc_twcc_rx_on_packet(&rx, 10, 64000);
    rtc_twcc_rx_on_packet(&rx, 11, 114000);
    n = rtc_twcc_rx_build(&rx, p, sizeof(p), 1, 2);
    w = hex("8fcd0005 00000001 00000002 000a 0002 000001 00 2002 00c8", want);
    CHECK(n == w && !memcmp(p, want, n), "twcc build: a 50 ms delta (200 x 250 us) is still a one-octet small delta");
    rtc_twcc_rx_init(&rx);
    rtc_twcc_rx_on_packet(&rx, 100, 65000);
    rtc_twcc_rx_on_packet(&rx, 101, 67000);
    rtc_twcc_rx_on_packet(&rx, 103, 66500);
    rtc_twcc_rx_on_packet(&rx, 104, 66750);
    n = rtc_twcc_rx_build(&rx, p, sizeof(p), 0x11111111, 0x22222222);
    w = hex("8fcd0006 11111111 22222222 0064 0005 000001 00 d490 04 08 fffe 01 00", want);
    CHECK(n == w && !memcmp(p, want, n), "twcc build: the hand-encoded packet byte for byte, zero padding without the P bit (draft 3.1) (%zu)", n);
    CHECK(rtc_twcc_rx_build(&rx, p, sizeof(p), 1, 2) == 0, "twcc build: nothing new to report");
    rtc_twcc_rx_on_packet(&rx, 105, 70000);
    n = rtc_twcc_rx_build(&rx, p, sizeof(p), 1, 2);
    CHECK(n == 24 && rtc_rd16(p + 12) == 105 && p[19] == 1, "twcc build: next feedback starts after the last reported, count increments");
    rtc_twcc_rx_init(&rx);
    for (i = 0; i < 1200; i++) {
        rng = rng * 1103515245u + 12345u;
        at[i] = 1000000 + (int64_t)i * 3000 + (int64_t)((rng >> 16) % 4000) - (i == 600 ? 0 : 0);
        if ((rng >> 8) % 10 == 0) {
            at[i] = -1;
            lost++;
            continue;
        }
        if (i == 700)
            at[i] += 20000000;
        rtc_twcc_rx_on_packet(&rx, (uint16_t)(65000 + i), at[i]);
    }
    while ((n = rtc_twcc_rx_build(&rx, p, 400, 1, 2)) > 0) {
        rtc_rtcp_iter_init(&it, p, n);
        ok &= n <= 400 && rtc_rtcp_check(p, n) == 0 && rtc_rtcp_next(&it, &k) == 1 && rtc_rtcp_fb_parse(&k, &fb) == 0;
        r = rtc_twcc_parse(&fb, &h, pk, 4096);
        ok &= r > 0 && h.base == (uint16_t)(65000 + seen);
        for (i = 0; r > 0 && i < (uint32_t)r; i++) {
            uint32_t idx = seen + i;
            int64_t ex = at[idx];
            ok &= pk[i].seq == (uint16_t)(65000 + idx);
            ok &= ex < 0 ? !pk[i].received : pk[i].received && pk[i].at_us == ex - ex % 250;
        }
        seen += r > 0 ? (uint32_t)r : 1200;
        if (seen >= 1200)
            break;
    }
    CHECK(ok && seen == 1200 - (at[1199] < 0), "twcc: 1200 packets with %u lost and a 20 s jump, split at 400 bytes, decode to the arrival times (floor 250 us)", lost);
    rtc_twcc_rx_init(&rx);
    rtc_twcc_rx_on_packet(&rx, 1, 0);
    rtc_twcc_rx_on_packet(&rx, 1 + RTC_TWCC_WIN + 5, 1000);
    CHECK(rx.base_ext == 0x10000u + 1 + 6 && rtc_twcc_rx_build(&rx, p, sizeof(p), 1, 2) > 0, "twcc: the window slides past %u", RTC_TWCC_WIN);
}

static void test_fb_check(void)
{
    uint8_t p[64];
    size_t n;
    rtc_rtcp_iter it;
    rtc_rtcp_pkt k;
    rtc_rtcp_fb fb;
    int i;
    static const struct {
        const char* hx;
        int ok;
        const char* what;
    } v[] = {
        { "81cd0002 11111111 22222222", -1, "NACK without an FCI entry (RFC 4585 6.2.1)" },
        { "81cd0003 11111111 22222222 00640003", 0, "NACK with one PID+BLP" },
        { "81c90002 11111111 22222222", 0, "RR is not feedback and passes" },
        { "81ce0002 11111111 00000000", 0, "PLI has no FCI (RFC 4585 6.3.1)" },
        { "81ce0003 11111111 00000000 00000000", -1, "PLI with an FCI" },
        { "84ce0002 11111111 00000000", -1, "FIR without an FCI entry (RFC 5104 4.3.1.1)" },
        { "84ce0003 11111111 00000000 22222222", -1, "FIR with half an entry" },
        { "84ce0004 11111111 00000000 22222222 05000000", 0, "FIR with one entry" },
        { "8fcd0003 11111111 22222222 00000001", -1, "TWCC shorter than its 8-byte fixed part" },
    };
    for (i = 0; i < (int)(sizeof(v) / sizeof(v[0])); i++) {
        n = hex(v[i].hx, p);
        rtc_rtcp_iter_init(&it, p, n);
        CHECK(rtc_rtcp_next(&it, &k) == 1 && (k.pt == RTC_RTCP_RR || rtc_rtcp_fb_parse(&k, &fb) == 0)
                && (k.pt == RTC_RTCP_RR ? 0 : rtc_rtcp_fb_check(&k, &fb)) == v[i].ok,
            "rtcp fb check: %s", v[i].what);
    }
    n = hex("81cd0003 11111111 22222222 00640003", p);
    k.p = p;
    k.n = 18;
    k.pt = RTC_RTCP_RTPFB;
    k.count = RTC_RTCP_FB_NACK;
    CHECK(rtc_rtcp_fb_parse(&k, &fb) == 0 && fb.fci_len == 6 && rtc_rtcp_fb_check(&k, &fb) < 0,
        "rtcp fb check: a caller-built packet whose NACK FCI is not whole PID+BLP entries");
    n = hex("81ce0001 11111111", p);
    rtc_rtcp_iter_init(&it, p, n);
    CHECK(rtc_rtcp_next(&it, &k) == 1 && rtc_rtcp_fb_parse(&k, &fb) < 0, "rtcp fb malformed: 8 bytes, no media SSRC");
    n = hex("81cd0003 11111111 22222222 0064", p);
    CHECK(rtc_rtcp_check(p, n) < 0, "rtcp fb malformed: length field says 16 bytes, 14 present");
}

static void test_keyreq(void)
{
    rtc_keyreq kq;
    rtc_fir_rx fr;
    uint8_t p[64];
    size_t n;
    rtc_rtcp_iter it;
    rtc_rtcp_pkt k;
    rtc_rtcp_fb fb;
    uint32_t ss = 0;
    uint8_t sq = 0;
    int i, ok = 1;
    rtc_keyreq_init(&kq, 300, 254);
    CHECK(rtc_keyreq_poll(&kq, 0, 0, 1, 2, p, sizeof(p)) == 0, "keyreq: nothing pending, nothing sent");
    rtc_keyreq_want(&kq);
    n = rtc_keyreq_poll(&kq, 1000, 0, 0x11111111, 0x22222222, p, sizeof(p));
    rtc_rtcp_iter_init(&it, p, n);
    CHECK(n == 12 && rtc_rtcp_next(&it, &k) == 1 && k.pt == RTC_RTCP_PSFB && k.count == RTC_RTCP_FB_PLI && rtc_rtcp_fb_parse(&k, &fb) == 0
            && fb.media == 0x22222222 && rtc_rtcp_fb_check(&k, &fb) == 0,
        "keyreq: PLI to the media SSRC (RFC 4585 6.3.1)");
    CHECK(rtc_keyreq_poll(&kq, 1299, 0, 1, 2, p, sizeof(p)) == 0 && rtc_keyreq_poll(&kq, 1300, 0, 1, 2, p, sizeof(p)) == 12, "keyreq: repeated only after the gap");
    rtc_keyreq_want(&kq);
    CHECK(rtc_keyreq_poll(&kq, 1301, 0, 1, 2, p, sizeof(p)) == 0, "keyreq: wanting again while one is outstanding does not reset the gap");
    rtc_keyreq_done(&kq);
    CHECK(rtc_keyreq_poll(&kq, 5000, 0, 1, 2, p, sizeof(p)) == 0, "keyreq: a key frame ends the request");
    rtc_keyreq_init(&kq, 300, 254);
    for (i = 0; i < 4; i++) {
        rtc_keyreq_want(&kq);
        n = rtc_keyreq_poll(&kq, (uint32_t)(i * 1000), 1, 0x11111111, 0x33333333, p, sizeof(p));
        rtc_rtcp_iter_init(&it, p, n);
        ok &= n == 20 && rtc_rtcp_next(&it, &k) == 1 && k.count == RTC_RTCP_FB_FIR && rtc_rtcp_fb_parse(&k, &fb) == 0 && fb.media == 0
            && rtc_rtcp_fir_at(&fb, 0, &ss, &sq) == 0 && ss == 0x33333333 && sq == (uint8_t)(254 + i);
        n = rtc_keyreq_poll(&kq, (uint32_t)(i * 1000 + 300), 1, 0x11111111, 0x33333333, p, sizeof(p));
        rtc_rtcp_iter_init(&it, p, n);
        ok &= n == 20 && rtc_rtcp_next(&it, &k) == 1 && rtc_rtcp_fb_parse(&k, &fb) == 0 && rtc_rtcp_fir_at(&fb, 0, &ss, &sq) == 0 && sq == (uint8_t)(254 + i);
        rtc_keyreq_done(&kq);
    }
    CHECK(ok, "keyreq FIR: media SSRC 0 in the header, target in the FCI; a repetition keeps the seq, each new command adds 1 mod 256 (RFC 5104 4.3.1.1)");
    rtc_keyreq_init(&kq, 300, 7);
    rtc_keyreq_want(&kq);
    CHECK(rtc_keyreq_poll(&kq, 0, 1, 1, 2, p, 10) == 0 && rtc_keyreq_poll(&kq, 1, 1, 1, 2, p, sizeof(p)) == 20 && p[16] == 7 && kq.sent == 1,
        "keyreq: no room sends nothing and keeps the seq; the initial seq is the caller's (arbitrary per RFC 5104)");
    rtc_keyreq_init(&kq, 300, 0);
    rtc_keyreq_want(&kq);
    CHECK(rtc_keyreq_poll(&kq, 0xFFFFFF00u, 0, 1, 2, p, sizeof(p)) == 12 && rtc_keyreq_poll(&kq, 0x00000010u, 0, 1, 2, p, sizeof(p)) == 0
            && rtc_keyreq_poll(&kq, 0x0000002Cu, 0, 1, 2, p, sizeof(p)) == 12,
        "keyreq: the gap is measured across the 32-bit ms wrap");
    memset(&fr, 0, sizeof(fr));
    CHECK(rtc_fir_rx_new(&fr, 9) == 1 && rtc_fir_rx_new(&fr, 9) == 0 && rtc_fir_rx_new(&fr, 10) == 1 && rtc_fir_rx_new(&fr, 9) == 1,
        "fir rx: a repetition (same seq) is not a new command; any other seq is");
    memset(&fr, 0, sizeof(fr));
    CHECK(rtc_fir_rx_new(&fr, 0) == 1, "fir rx: the first command is new even with seq 0");
}

static void test_twcc_ext(void)
{
    uint8_t p[64], v[2];
    size_t n;
    rtc_rtp_hdr h;
    rtc_rtp_ext e;
    uint16_t s = 0;
    rtc_rtcp_iter it;
    rtc_rtcp_pkt k;
    rtc_rtcp_fb fb;
    rtc_twcc_fb th;
    static rtc_twcc_pkt pk[300];
    int r, i, ok;
    n = hex("90 60 0001 00000002 00000003 bede 0001 31 abcd 00 ff", p);
    CHECK(rtc_rtp_parse(p, n, &h) == 0 && rtc_twcc_ext_get(p, &h, 3, &s) == 0 && s == 0xabcd,
        "twcc ext: one-byte form, ID 3, L=1, 16-bit transport-wide seq (draft-holmer-01 2)");
    CHECK(rtc_twcc_ext_get(p, &h, 4, &s) < 0, "twcc ext: absent ID");
    n = hex("90 60 0001 00000002 00000003 bede 0001 30 ab 0000 ff", p);
    CHECK(rtc_rtp_parse(p, n, &h) == 0 && rtc_twcc_ext_get(p, &h, 3, &s) < 0, "twcc ext malformed: one-byte value");
    rtc_twcc_ext_put(v, 0xfffe);
    e.id = 5;
    e.len = 2;
    e.val = v;
    n = rtc_rtp_write_hdr(p, sizeof(p), 96, 1, 7, 9, 11, NULL, 0, &e, 1);
    CHECK(n > 12 && rtc_rtp_parse(p, n, &h) == 0 && rtc_twcc_ext_get(p, &h, 5, &s) == 0 && s == 0xfffe, "twcc ext: written with rtc_rtp_write_hdr, read back");
    n = hex("8fcd0005 11111111 22222222 0000 00dd 000000 00 00dd 0000", p);
    rtc_rtcp_iter_init(&it, p, n);
    r = rtc_rtcp_next(&it, &k) == 1 && rtc_rtcp_fb_parse(&k, &fb) == 0 ? rtc_twcc_parse(&fb, &th, pk, 300) : -9;
    ok = r == 221;
    for (i = 0; ok && i < 221; i++)
        ok &= !pk[i].received && pk[i].seq == i;
    CHECK(ok, "twcc draft-holmer-01 3.1.3 example 1: run-length chunk 0|00|0000011011101 is 221 x not received");
    n = hex("8fcd0007 11111111 22222222 0010 000e 000002 05 9f1c 01 02 03 04 05 06 07 08 0000", p);
    rtc_rtcp_iter_init(&it, p, n);
    r = rtc_rtcp_next(&it, &k) == 1 && rtc_rtcp_fb_parse(&k, &fb) == 0 ? rtc_twcc_parse(&fb, &th, pk, 300) : -9;
    {
        static const char want[] = "01111100011100";
        int64_t t = 128000;
        int d = 0;
        ok = r == 14 && th.base == 16 && th.fb_count == 5;
        for (i = 0; ok && i < 14; i++) {
            ok &= pk[i].received == (want[i] == '1');
            if (pk[i].received) {
                t += 250 * ++d;
                ok &= pk[i].at_us == t;
            }
        }
    }
    CHECK(ok, "twcc draft-holmer-01 3.1.4 example 1: 1-bit vector 1|0|01111100011100: 1 lost, 5 received, 3 lost, 3 received, 2 lost");
    n = hex("8fcd0005 11111111 22222222 0000 0007 000000 00 cd50 0000", p);
    rtc_rtcp_iter_init(&it, p, n);
    CHECK(rtc_rtcp_next(&it, &k) == 1 && rtc_rtcp_fb_parse(&k, &fb) == 0 && rtc_twcc_parse(&fb, &th, pk, 300) < 0,
        "twcc draft 3.1.4 example 2 uses symbol 11, which 3.1.1 reserves: rejected (the example predates the reservation)");
    n = hex("8fcd0005 11111111 22222222 0000 0018 000000 00 6018 0000", p);
    rtc_rtcp_iter_init(&it, p, n);
    CHECK(rtc_rtcp_next(&it, &k) == 1 && rtc_rtcp_fb_parse(&k, &fb) == 0 && rtc_twcc_parse(&fb, &th, pk, 300) < 0,
        "twcc draft 3.1.3 example 2: a run of reserved symbol 11 is rejected");
}

static void test_nack_answer(void)
{
    rtc_pktring h;
    uint8_t b[100];
    const uint8_t* pk[4];
    size_t len[4];
    uint16_t want[4] = { 65531, 2, 100, 65530 };
    uint32_t i;
    rtc_pktring_init(&h, 1 << 16, 64);
    for (i = 0; i < 12; i++) {
        memset(b, (int)i, sizeof(b));
        rtc_pktring_put(&h, (uint16_t)(65530 + i), i, 0, i, b, 50 + i);
    }
    CHECK(rtc_nack_answer(&h, want, 4, pk, len) == 3 && pk[0] && len[0] == 51 && pk[0][0] == 1 && pk[1] && len[1] == 58 && pk[1][0] == 8 && !pk[2]
            && len[2] == 0 && pk[3] && pk[3][0] == 0,
        "nack answer: requested seqs across the wrap come from the send history; one never sent is reported missing");
    for (i = 12; i < 12 + 64; i++)
        rtc_pktring_put(&h, (uint16_t)(65530 + i), i, 0, i, b, 50);
    CHECK(rtc_nack_answer(&h, want, 1, pk, len) == 0 && !pk[0], "nack answer: a packet overwritten in the history cannot be resent");
    rtc_pktring_free(&h);
}

static void test_jb_keyreq(void)
{
    rtc_jb j;
    rtc_jb_frame f;
    uint8_t out[4096];
    size_t len;
    uint8_t pl[64];
    CHECK(rtc_jb_init(&j, 64, 1 << 16, 0) == 0 && !rtc_jb_take_keyreq(&j), "jb keyreq: none at start");
    rtc_jb_set_target(&j, 0, 100);
    memcpy(pl, "\x90\x80\x81\x00\x00\xaa", 6);
    rtc_jb_insert(&j, 10, 1000, 1, 1, 1, 0, pl, 6);
    CHECK(rtc_jb_pop(&j, 0, &f) && !rtc_jb_take_keyreq(&j), "jb keyreq: a complete frame asks for nothing");
    len = 0;
    CHECK(rtc_jb_frame_copy(&j, &f, RTC_JB_FMT_VP8, out, sizeof(out), &len) == 0 && len == 2 && out[0] == 0 && out[1] == 0xaa,
        "jb copy: VP8 descriptor stripped");
    memcpy(pl, "\x90\x80\x81\x01\x01\xbb", 6);
    rtc_jb_insert(&j, 12, 2000, 1, 1, 0, 10, pl, 6);
    CHECK(!rtc_jb_pop(&j, 50, &f) && !rtc_jb_take_keyreq(&j), "jb keyreq: waiting for seq 11 is not giving up");
    CHECK(!rtc_jb_pop(&j, 110, &f) && rtc_jb_take_keyreq(&j) && !rtc_jb_take_keyreq(&j),
        "jb keyreq: the head is stale and there is no key frame to skip to: ask once for one (RFC 4585 PLI)");
    memcpy(pl, "\x90\x80\x81\x02\x00\xcc", 6);
    rtc_jb_insert(&j, 14, 3000, 1, 1, 1, 120, pl, 6);
    CHECK(rtc_jb_pop(&j, 120, &f) && f.ts == 3000 && rtc_jb_take_keyreq(&j), "jb keyreq: skipping to a key frame drops frames, which also asks");
    rtc_jb_free(&j);
    rtc_jb_init(&j, 64, 1 << 16, 0);
    rtc_jb_set_target(&j, 0, 1000);
    rtc_jb_insert(&j, 1, 1000, 0, 1, 1, 0, (const uint8_t*)"\x7c\x85\x01\x02", 4);
    rtc_jb_insert(&j, 2, 1000, 1, 0, 0, 0, (const uint8_t*)"\x7c\x05\x03", 3);
    len = 0;
    CHECK(rtc_jb_pop(&j, 0, &f) && rtc_jb_frame_copy(&j, &f, RTC_JB_FMT_H264, out, sizeof(out), &len) < 0 && len == 0 && rtc_jb_take_keyreq(&j),
        "jb copy: an H.264 frame whose FU-A never ends is rejected and asks for a key frame");
    rtc_jb_insert(&j, 3, 2000, 1, 1, 0, 0, (const uint8_t*)"\x41\x9a\x01", 3);
    CHECK(rtc_jb_pop(&j, 0, &f) && rtc_jb_frame_copy(&j, &f, RTC_JB_FMT_H264, out, 6, &len) == -2 && len == 0 && !rtc_jb_take_keyreq(&j)
            && rtc_jb_frame_copy(&j, &f, RTC_JB_FMT_H264, out, sizeof(out), &len) == 0 && len == 7,
        "jb copy: -2 when out of room writes nothing and is not a loss");
    rtc_jb_free(&j);
    rtc_jb_init(&j, 16, 4096, 1);
    rtc_jb_insert(&j, 5, 960, 0, 0, 0, 0, (const uint8_t*)"\x08\x01\x02", 3);
    len = 0;
    CHECK(rtc_jb_pop(&j, 0, &f) && rtc_jb_frame_copy(&j, &f, RTC_JB_FMT_RAW, out, sizeof(out), &len) == 0 && len == 3 && out[0] == 8, "jb copy: raw (Opus/G.711) frame is the payload");
    CHECK(rtc_jb_frame_copy(&j, &f, RTC_JB_FMT_RAW, out, 4, &len) == -2 && len == 3, "jb copy raw: no room");
    rtc_jb_free(&j);
    rtc_jb_init(&j, 16, 512, 1);
    rtc_jb_insert(&j, 1, 960, 0, 0, 0, 0, pl, 60);
    rtc_jb_pop(&j, 0, &f);
    for (len = 2; len < 20; len++)
        rtc_jb_insert(&j, (uint16_t)len, (uint32_t)len * 960, 0, 0, 0, 0, pl, 60);
    len = 0;
    CHECK(rtc_jb_frame_copy(&j, &f, RTC_JB_FMT_RAW, out, sizeof(out), &len) == -1 && len == 0 && rtc_jb_take_keyreq(&j),
        "jb copy: a popped frame whose bytes were overwritten is a loss");
    rtc_jb_free(&j);
    rtc_jb_init(&j, 64, 1 << 16, 0);
    rtc_jb_insert(&j, 11, 500, 1, 0, 0, 0, pl, 6);
    rtc_jb_insert(&j, 9, 400, 1, 1, 1, 0, pl, 6);
    CHECK(j.st.late == 0 && j.next_ext == 0x10000u + 9 && rtc_jb_pop(&j, 0, &f) && f.ts == 400 && !rtc_jb_pop(&j, 0, &f),
        "jb start: a packet older than the first one received moves the head back while nothing has been released");
    rtc_jb_insert(&j, 10, 500, 0, 1, 0, 0, pl, 6);
    CHECK(rtc_jb_pop(&j, 0, &f) && f.ts == 500 && f.npkts == 2, "jb start: then the stream plays from the earliest frame");
    CHECK(rtc_jb_insert(&j, 8, 300, 1, 1, 1, 0, pl, 6) < 0 && j.st.late == 1, "jb start: once a frame is released an older packet is late");
    rtc_jb_free(&j);
    rtc_jb_init(&j, 16, 1 << 16, 0);
    rtc_jb_insert(&j, 100, 1, 1, 1, 1, 0, pl, 6);
    rtc_jb_insert(&j, 110, 2, 1, 1, 1, 0, pl, 6);
    CHECK(rtc_jb_insert(&j, 95, 0, 1, 1, 1, 0, pl, 6) == 0 && rtc_jb_insert(&j, 94, 0, 1, 1, 1, 0, pl, 6) < 0 && j.st.late == 1,
        "jb start: the head moves back only while the span stays within the slot count");
    rtc_jb_free(&j);
    rtc_jb_init(&j, 64, 1 << 16, 0);
    rtc_jb_insert(&j, 11, 200, 1, 1, 0, 0, pl, 6);
    rtc_jb_insert(&j, 10, 100, 0, -1, 0, 0, pl, 6);
    CHECK(!rtc_jb_pop(&j, 0, &f), "jb start: moving the head back to a packet whose start is unknown makes the head unknown again");
    rtc_jb_insert(&j, 9, 100, 0, 1, 0, 0, pl, 6);
    CHECK(rtc_jb_pop(&j, 0, &f) && f.ts == 100 && f.npkts == 2 && rtc_jb_pop(&j, 0, &f) && f.ts == 200,
        "jb start: the explicit start arrives and both frames play in order");
    rtc_jb_free(&j);
    rtc_jb_init(&j, 64, 1 << 16, 0);
    rtc_jb_insert(&j, 21, 100, 1, -1, 1, 0, pl, 6);
    CHECK(!rtc_jb_pop(&j, 0, &f), "jb start: a key packet of unknown start (an IDR FU-A after SPS and PPS) is not a frame start");
    rtc_jb_insert(&j, 20, 100, 0, 1, 1, 0, pl, 6);
    CHECK(rtc_jb_pop(&j, 0, &f) && f.npkts == 2 && f.first_ext + 1 == f.last_ext, "jb start: the access unit plays from its real first packet");
    rtc_jb_free(&j);
    rtc_jb_init(&j, 64, 1 << 16, 0);
    rtc_jb_insert(&j, 1, 960, 0, 1, 0, 0, (const uint8_t*)"abc", 3);
    rtc_jb_insert(&j, 2, 960, 1, 0, 0, 0, (const uint8_t*)"def", 3);
    len = 5;
    CHECK(rtc_jb_pop(&j, 0, &f) && f.npkts == 2 && rtc_jb_frame_copy(&j, &f, RTC_JB_FMT_RAW, out, 9, &len) == -2 && len == 5,
        "jb copy: running out of room after the first packet rewinds to the caller's length");
    CHECK(rtc_jb_frame_copy(&j, &f, RTC_JB_FMT_RAW, out, 11, &len) == 0 && len == 11 && !memcmp(out + 5, "abcdef", 6),
        "jb copy: appends after the caller's length");
    rtc_jb_free(&j);
}

enum { E2E_MAXF = 410, E2E_CH = 4096, E2E_TAIL = 4 };

typedef struct {
    uint32_t at;
    uint16_t seq;
    uint32_t ts;
    uint8_t marker;
    uint16_t len;
    uint8_t d[1300];
} e2e_pkt;

typedef struct {
    int fmt;
    int frames;
    unsigned loss_pct;
    unsigned burst_pct;
    unsigned dup_pct;
    unsigned reorder_ms;
    int nack;
    uint32_t seed;
} e2e_cfg;

typedef struct {
    int delivered, corrupt, out_of_order, keyreqs, nacks_sent, resent, dropped;
} e2e_res;

static uint8_t e2e_frame[E2E_MAXF][4200];
static size_t e2e_flen[E2E_MAXF];
static e2e_pkt e2e_ch[E2E_CH];
static uint32_t e2e_rng;

static uint32_t e2e_rand(void)
{
    e2e_rng = e2e_rng * 1664525u + 1013904223u;
    return e2e_rng >> 8;
}

static void e2e_send(int* nch, const e2e_cfg* c, uint32_t now, uint16_t seq, uint32_t ts, int marker, const uint8_t* p, size_t n, int* bad)
{
    int copies = 1, k;
    if (ts != 0xFFFFFFFFu - 50 * 3000u && (*bad ? e2e_rand() % 100 < 60 : e2e_rand() % 100 < c->loss_pct)) {
        if (e2e_rand() % 100 < c->burst_pct)
            *bad = 1;
        return;
    }
    if (*bad && e2e_rand() % 100 < 40)
        *bad = 0;
    if (e2e_rand() % 100 < c->dup_pct)
        copies = 2;
    for (k = 0; k < copies && *nch < E2E_CH; k++) {
        e2e_pkt* q = &e2e_ch[(*nch)++];
        q->at = now + 20 + (c->reorder_ms ? e2e_rand() % c->reorder_ms : 0);
        q->seq = seq;
        q->ts = ts;
        q->marker = (uint8_t)marker;
        q->len = (uint16_t)n;
        memcpy(q->d, p, n);
    }
}

static e2e_res e2e_run(const e2e_cfg* c)
{
    static rtc_pktring hist;
    static rtc_jb j;
    static rtc_nack nk;
    static uint8_t pk[1300], out[8192];
    e2e_res res;
    int nch = 0, i, sent_frames = 0, bad = 0, last_idx = -1;
    uint16_t seq = 65000;
    uint32_t ts0 = 0xFFFFFFFFu - 50 * 3000u, now;
    memset(&res, 0, sizeof(res));
    e2e_rng = c->seed;
    rtc_pktring_init(&hist, 1u << 21, 2048);
    rtc_jb_init(&j, c->fmt == RTC_JB_FMT_RAW ? 256 : 1024, 1u << 21, c->fmt == RTC_JB_FMT_RAW);
    rtc_jb_set_target(&j, 0, c->nack ? 2000 : 150);
    rtc_nack_init(&nk);
    for (i = 0; i < c->frames + E2E_TAIL; i++) {
        size_t n = c->fmt == RTC_JB_FMT_RAW ? 20 + e2e_rand() % 200 : 30 + e2e_rand() % 4000, k;
        int key = i % 40 == 0;
        uint8_t* f = e2e_frame[i];
        for (k = 0; k < n; k++)
            f[k] = (uint8_t)(1 + e2e_rand() % 255);
        if (c->fmt == RTC_JB_FMT_VP8)
            f[0] = (uint8_t)(key ? 0x10 : 0x11);
        if (c->fmt == RTC_JB_FMT_H264) {
            memcpy(f, "\0\0\0\1", 4);
            f[4] = (uint8_t)(key ? 0x67 : 0x41);
            if (n > 400) {
                memcpy(f + (key ? 16 : 200), "\0\0\0\1", 4);
                f[key ? 20 : 204] = (uint8_t)(key ? 0x65 : 0x41);
            }
        }
        e2e_flen[i] = n;
    }
    for (now = 0; now < (uint32_t)(c->frames + E2E_TAIL) * 33 + 6000; now++) {
        int k;
        if (sent_frames < c->frames + E2E_TAIL && now == (uint32_t)sent_frames * 33) {
            uint32_t ts = ts0 + (uint32_t)sent_frames * 3000u;
            const uint8_t* f = e2e_frame[sent_frames];
            size_t n = e2e_flen[sent_frames], m;
            int last = 0;
            rtc_vp8_pay vp;
            rtc_h264_pay hp;
            if (c->fmt == RTC_JB_FMT_VP8)
                rtc_vp8_pay_init(&vp, f, n, 1200, sent_frames & 0x7FFF);
            if (c->fmt == RTC_JB_FMT_H264)
                rtc_h264_pay_init(&hp, f, n, 1200);
            for (;;) {
                if (c->fmt == RTC_JB_FMT_VP8)
                    m = rtc_vp8_pay_next(&vp, pk, sizeof(pk), &last);
                else if (c->fmt == RTC_JB_FMT_H264)
                    m = rtc_h264_pay_next(&hp, pk, sizeof(pk), &last);
                else {
                    memcpy(pk, f, n);
                    m = n;
                    last = 1;
                }
                if (!m)
                    break;
                rtc_pktring_put(&hist, seq, ts, last, now, pk, m);
                e2e_send(&nch, c, now, seq, ts, last, pk, m, &bad);
                seq++;
                if (last)
                    break;
            }
            sent_frames++;
        }
        for (k = 0; k < nch;) {
            e2e_pkt* q = &e2e_ch[k];
            if (q->at > now) {
                k++;
                continue;
            }
            {
                int start = -1, key = 0;
                rtc_vp8_desc d;
                if (c->fmt == RTC_JB_FMT_VP8 && rtc_vp8_desc_parse(q->d, q->len, &d) >= 0) {
                    start = d.start && d.partition == 0;
                    key = rtc_vp8_is_keyframe(q->d, q->len);
                } else if (c->fmt == RTC_JB_FMT_H264) {
                    key = rtc_h264_is_keyframe(q->d, q->len);
                    start = rtc_h264_start_hint(q->d, q->len);
                }
                rtc_jb_insert(&j, q->seq, q->ts, q->marker, start, key, now, q->d, q->len);
                rtc_nack_on_packet(&nk, q->seq);
            }
            e2e_ch[k] = e2e_ch[--nch];
        }
        if (c->nack && now % 10 == 0) {
            uint16_t ns[64];
            const uint8_t* rp[64];
            size_t rl[64];
            unsigned nn = rtc_nack_poll(&nk, now, 60, ns, 64), q;
            res.nacks_sent += (int)nn;
            rtc_nack_answer(&hist, ns, nn, rp, rl);
            for (q = 0; q < nn; q++) {
                const rtc_pkt_slot* s = NULL;
                if (rp[q] && rtc_pktring_get(&hist, ns[q], &s)) {
                    res.resent++;
                    e2e_send(&nch, c, now + 10, ns[q], s->ts, s->marker, rp[q], rl[q], &bad);
                }
            }
        }
        {
            rtc_jb_frame f;
            while (rtc_jb_pop(&j, now, &f)) {
                size_t len = 0;
                int idx = (int)((f.ts - ts0) / 3000u);
                if (rtc_jb_frame_copy(&j, &f, c->fmt, out, sizeof(out), &len) < 0)
                    continue;
                if (idx >= c->frames)
                    continue;
                if (idx < 0 || len != e2e_flen[idx] || memcmp(out, e2e_frame[idx], len))
                    res.corrupt++;
                if (idx <= last_idx)
                    res.out_of_order++;
                last_idx = idx;
                res.delivered++;
            }
            res.keyreqs += rtc_jb_take_keyreq(&j) + rtc_nack_take_keyreq(&nk);
        }
    }
    res.dropped = (int)j.st.frames_dropped;
    rtc_jb_free(&j);
    rtc_pktring_free(&hist);
    return res;
}

static void test_e2e(void)
{
    static const char* const fmtn[] = { "raw", "vp8", "h264" };
    int fmt;
    uint32_t seed;
    for (fmt = RTC_JB_FMT_RAW; fmt <= RTC_JB_FMT_H264; fmt++) {
        int ok = 1, total_resent = 0;
        for (seed = 1; seed <= 4; seed++) {
            e2e_cfg c = { fmt, 300, 8, 20, 5, 30, 1, seed * 7919u };
            e2e_res r = e2e_run(&c);
            ok &= r.delivered == 300 && r.corrupt == 0 && r.out_of_order == 0 && r.dropped == 0;
            total_resent += r.resent;
            if (!(r.delivered == 300 && r.corrupt == 0 && r.out_of_order == 0))
                printf("  %s seed %u: delivered %d corrupt %d ooo %d dropped %d resent %d keyreq %d\n", fmtn[fmt], seed, r.delivered, r.corrupt, r.out_of_order, r.dropped, r.resent, r.keyreqs);
        }
        CHECK(ok && total_resent > 0, "e2e %s: 4 seeded schedules (8%% loss, bursts, 5%% dup, 30 ms reorder, seq and ts wrap): NACK from the history recovers every frame, byte-identical and in order (%d resent)",
            fmtn[fmt], total_resent);
    }
    {
        e2e_cfg c = { RTC_JB_FMT_VP8, 300, 10, 30, 5, 30, 0, 4242u };
        e2e_res r = e2e_run(&c);
        CHECK(r.corrupt == 0 && r.out_of_order == 0 && r.delivered < 300 && r.delivered > 0 && r.dropped > 0 && r.keyreqs > 0,
            "e2e vp8 without NACK at 10%% loss: frames are lost, never corrupted or reordered; the buffer skips to key frames and asks for them (%d delivered, %d dropped, %d keyreq)",
            r.delivered, r.dropped, r.keyreqs);
    }
    {
        e2e_cfg c = { RTC_JB_FMT_VP8, 120, 0, 0, 0, 0, 0, 1u };
        e2e_res r = e2e_run(&c);
        CHECK(r.delivered == 120 && r.corrupt == 0 && r.keyreqs == 0 && r.nacks_sent == 0, "e2e vp8 clean channel: everything delivered, no feedback");
    }
}

int main(void)
{
    test_rtp_parse();
    test_rtp_write();
    test_rtcp();
    test_rtt();
    test_rtpstat();
    test_pktring();
    test_opus();
    test_vp8();
    test_h264();
    test_opus_parse();
    test_g711();
    test_vp8_ext();
    test_vp8_mtu();
    test_h264_mtu();
    test_nack();
    test_jbuf();
    test_twcc();
    test_fb_check();
    test_keyreq();
    test_twcc_ext();
    test_nack_answer();
    test_jb_keyreq();
    test_e2e();
    printf("test_rtc_media: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
