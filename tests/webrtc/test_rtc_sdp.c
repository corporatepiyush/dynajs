/*
 * test_rtc_sdp.c -- the SDP layer in src/core/rtc: the session description model (rtc-sdp: parse, the strict check,
 * the writer) and JSEP offer/answer negotiation (rtc-jsep).
 *
 * Oracles, none derived from the code under test:
 * - fixtures written by hand: F_RFC after the session example of RFC 8866 5; F_CHROME and F_FIREFOX in the shape of the
 *   offers the two browser engines send (BUNDLE of audio, video and data, RTX, H.264 with profile-level-id, transport-cc,
 *   msid, ssrc-group FID, a bundle-only m= section with port 0) and in the spirit of the RFC 8829 7 examples; F_LINES
 *   exercises every non-attribute line type (i= b= r= k=, a port count, an unknown x= line). None is a capture;
 * - preservation: the multiset of lines the writer emits equals the multiset of input lines (nothing invented, nothing
 *   lost, unknown attributes and lines included); for F_RFC and F_LINES, whose lines are already in RFC 8866 order,
 *   the output is byte-identical;
 * - stability: parse(write(parse(x))) equals parse(x) field by field and a second write is byte-identical;
 * - typed fields checked against the fixture text (payload types, rtpmap, fmtp, rtcp-fb, extmap id/direction/uri,
 *   msid, ssrc cname/msid, ssrc-group, mid, BUNDLE, rtcp-mux, rtcp-mux-only, bundle-only, direction, setup,
 *   fingerprint, ICE credentials);
 * - malformed input: RFC 8866 5 (o= s= t= required) and 5.7 (c= in the session or every m= section), 6.6 (rtpmap
 *   syntax, a dynamic payload type needs one), RFC 9143 6 and 7.2 (bundle-only needs port 0 and a group; a group mid
 *   must exist; no duplicate mid), RFC 8858 3 (rtcp-mux-only needs rtcp-mux), RFC 8285 5 (unique extmap ids, id 1..255,
 *   direction keyword); the model never truncates: a value too long for it is an error, never a shortened copy;
 * - negotiation tables from the RFC text: direction answers of RFC 3264 6.1 intersected with the local direction
 *   (RFC 8829 5.3.1); payload types, rtx apt values and extmap ids taken from the offer, rtcp-fb and extmap intersected,
 *   codecs kept in offer order (RFC 8829 5.3.1); H.264 (RFC 6184 8.1, 8.2.2): packetization-mode (default 0) and the
 *   profile derived from profile_idc and profile-iop by RFC 6184 Table 5 (default Baseline level 1) must match, so
 *   42e01f, 4de01f and 58c01f are all Constrained Baseline; the table is checked for all 256 profile-iop values of every
 *   listed profile_idc against a matcher that reads the RFC bit patterns as text; combinations outside the table never
 *   match; the answer level is at most the offered one
 *   unless both sides set level-asymmetry-allowed=1, level 1b is between 1 and 1.1 (constraint_set3 with level_idc 11 in
 *   Baseline/Main/Extended, level_idc 9 otherwise); Opus and VP8 fmtp are declarative, so the answer carries the local
 *   values; BUNDLE answers (RFC 9143 7.3): the offerer-tagged mid comes first, rejected m= sections get port 0 and leave
 *   the group, no answer m= section has bundle-only, and when the tagged m= section is rejected there is no group and
 *   bundle-only m= sections are rejected; the data m= section keeps the offered proto and fmt (RFC 8829 5.3.1);
 * - simulcast answer (RFC 8853 5.3, RFC 8851 5): each rid of the offered a=simulcast send list that has an a=rid send
 *   line is answered with a=rid recv and the list is mirrored as a=simulcast:recv (paused ~ kept, unknown rids dropped),
 *   only when the answer receives; per-offer extmap ids (re-offers keep negotiated ids, RFC 8829 5.2.2); the default
 *   audio caps carry RFC 4733 telephone-event at 48000 and 8000 Hz;
 * - memory (CLAUDE.md rule 11): the model has a small fixed part and one arena per description, freed at once; every
 *   list and string is allocated by the input, so a description costs O(input) bytes (asserted: arena within
 *   2 x input + 4 KiB on F_CHROME and a sweep of 1..64 video sections, flat over 1000 re-parses); absurd inputs hit
 *   explicit caps (RTC_SDP_MAX_*, RTC_SDP_LINE_MAX, RTC_SDP_MAX_INPUT), each tested at the cap and one over;
 * - fuzz: seeded mutation of every fixture (byte flips, inserted CR/LF/space/colon/digits, deleted ranges, duplicated,
 *   swapped and cut lines) fed as exact-size heap copies with no terminator; every accepted input must write,
 *   re-parse, re-write byte-identically and negotiate without a crash (ASan+UBSan).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rtc/rtc-ice.h"
#include "rtc/rtc-jsep.h"
#include "rtc/rtc-sdp.h"

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

#define FP "A1:B2:C3:D4:E5:F6:07:18:29:3A:4B:5C:6D:7E:8F:90:A1:B2:C3:D4:E5:F6:07:18:29:3A:4B:5C:6D:7E:8F:90"

static const char F_RFC[] = "v=0\r\n"
                            "o=jdoe 3724394400 3724394405 IN IP4 198.51.100.1\r\n"
                            "s=Call to John Smith\r\n"
                            "i=SDP Offer #1\r\n"
                            "u=http://www.jdoe.example.com/home.html\r\n"
                            "e=Jane Doe <jane@jdoe.example.com>\r\n"
                            "p=+1 617 555-6011\r\n"
                            "c=IN IP4 198.51.100.1\r\n"
                            "t=0 0\r\n"
                            "m=audio 49170 RTP/AVP 0\r\n"
                            "m=audio 49180 RTP/AVP 0\r\n"
                            "m=video 51372 RTP/AVP 99\r\n"
                            "c=IN IP6 2001:db8::2\r\n"
                            "a=rtpmap:99 h263-1998/90000\r\n";

static const char F_CHROME[] = "v=0\r\n"
                               "o=- 4611731400430051336 2 IN IP4 127.0.0.1\r\n"
                               "s=-\r\n"
                               "t=0 0\r\n"
                               "a=group:BUNDLE 0 1 2\r\n"
                               "a=extmap-allow-mixed\r\n"
                               "a=msid-semantic: WMS stream1\r\n"
                               "m=audio 9 UDP/TLS/RTP/SAVPF 111 0 8 110\r\n"
                               "c=IN IP4 0.0.0.0\r\n"
                               "a=rtcp:9 IN IP4 0.0.0.0\r\n"
                               "a=candidate:1467250027 1 udp 2122260223 192.168.1.10 56143 typ host generation 0 network-id 1\r\n"
                               "a=candidate:842163049 1 udp 1686052607 203.0.113.5 46154 typ srflx raddr 192.168.1.10 rport 56143\r\n"
                               "a=ice-ufrag:EsAw\r\n"
                               "a=ice-pwd:bP+XJMM09aR8AiX1jdukzR6Y\r\n"
                               "a=ice-options:trickle\r\n"
                               "a=fingerprint:sha-256 " FP "\r\n"
                               "a=setup:actpass\r\n"
                               "a=mid:0\r\n"
                               "a=extmap:1 urn:ietf:params:rtp-hdrext:ssrc-audio-level\r\n"
                               "a=extmap:3 http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time\r\n"
                               "a=extmap:4 http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01\r\n"
                               "a=extmap:9 urn:ietf:params:rtp-hdrext:sdes:mid\r\n"
                               "a=sendrecv\r\n"
                               "a=msid:stream1 track-a\r\n"
                               "a=rtcp-mux\r\n"
                               "a=rtcp-rsize\r\n"
                               "a=rtpmap:111 opus/48000/2\r\n"
                               "a=rtcp-fb:111 transport-cc\r\n"
                               "a=fmtp:111 minptime=10;useinbandfec=1\r\n"
                               "a=rtpmap:0 PCMU/8000\r\n"
                               "a=rtpmap:8 PCMA/8000\r\n"
                               "a=rtpmap:110 telephone-event/48000\r\n"
                               "a=ssrc:1001 cname:Yk1rPq\r\n"
                               "a=ssrc:1001 msid:stream1 track-a\r\n"
                               "m=video 9 UDP/TLS/RTP/SAVPF 96 97 102 103 127 125 108\r\n"
                               "c=IN IP4 0.0.0.0\r\n"
                               "b=AS:2000\r\n"
                               "a=ice-ufrag:EsAw\r\n"
                               "a=ice-pwd:bP+XJMM09aR8AiX1jdukzR6Y\r\n"
                               "a=ice-options:trickle\r\n"
                               "a=fingerprint:sha-256 " FP "\r\n"
                               "a=setup:actpass\r\n"
                               "a=mid:1\r\n"
                               "a=extmap:14 urn:ietf:params:rtp-hdrext:toffset\r\n"
                               "a=extmap:5/recvonly urn:3gpp:video-orientation\r\n"
                               "a=extmap:4 http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01\r\n"
                               "a=extmap:9 urn:ietf:params:rtp-hdrext:sdes:mid\r\n"
                               "a=sendrecv\r\n"
                               "a=msid:stream1 track-v\r\n"
                               "a=rtcp-mux\r\n"
                               "a=rtcp-rsize\r\n"
                               "a=rtpmap:96 VP8/90000\r\n"
                               "a=rtcp-fb:96 goog-remb\r\n"
                               "a=rtcp-fb:96 transport-cc\r\n"
                               "a=rtcp-fb:96 ccm fir\r\n"
                               "a=rtcp-fb:96 nack\r\n"
                               "a=rtcp-fb:96 nack pli\r\n"
                               "a=rtcp-fb:96 ccm tmmbr\r\n"
                               "a=rtpmap:97 rtx/90000\r\n"
                               "a=fmtp:97 apt=96\r\n"
                               "a=rtpmap:102 H264/90000\r\n"
                               "a=rtcp-fb:102 transport-cc\r\n"
                               "a=rtcp-fb:102 nack\r\n"
                               "a=rtcp-fb:102 nack pli\r\n"
                               "a=fmtp:102 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f\r\n"
                               "a=rtpmap:103 rtx/90000\r\n"
                               "a=fmtp:103 apt=102\r\n"
                               "a=rtpmap:127 H264/90000\r\n"
                               "a=rtcp-fb:127 nack\r\n"
                               "a=fmtp:127 level-asymmetry-allowed=1;packetization-mode=0;profile-level-id=42e01f\r\n"
                               "a=rtpmap:125 rtx/90000\r\n"
                               "a=fmtp:125 apt=127\r\n"
                               "a=rtpmap:108 H264/90000\r\n"
                               "a=fmtp:108 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=4d001f\r\n"
                               "a=ssrc-group:FID 2001 2002\r\n"
                               "a=ssrc:2001 cname:Yk1rPq\r\n"
                               "a=ssrc:2001 msid:stream1 track-v\r\n"
                               "a=ssrc:2002 cname:Yk1rPq\r\n"
                               "a=ssrc:2001 label:legacy-label\r\n"
                               "a=x-google-flag:conference\r\n"
                               "m=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\n"
                               "c=IN IP4 0.0.0.0\r\n"
                               "a=ice-ufrag:EsAw\r\n"
                               "a=ice-pwd:bP+XJMM09aR8AiX1jdukzR6Y\r\n"
                               "a=ice-options:trickle\r\n"
                               "a=fingerprint:sha-256 " FP "\r\n"
                               "a=setup:actpass\r\n"
                               "a=mid:2\r\n"
                               "a=sctp-port:5000\r\n"
                               "a=max-message-size:262144\r\n";

static const char F_FIREFOX[] = "v=0\r\n"
                                "o=mozilla...THIS_IS_SDPARTA-99.0 6113434441329372785 0 IN IP4 0.0.0.0\r\n"
                                "s=-\r\n"
                                "t=0 0\r\n"
                                "a=fingerprint:sha-256 " FP "\r\n"
                                "a=group:BUNDLE 0 1\r\n"
                                "a=ice-options:trickle\r\n"
                                "a=msid-semantic:WMS *\r\n"
                                "m=audio 9 UDP/TLS/RTP/SAVPF 109 9 0 8 101\r\n"
                                "c=IN IP4 0.0.0.0\r\n"
                                "a=sendrecv\r\n"
                                "a=extmap:1 urn:ietf:params:rtp-hdrext:ssrc-audio-level\r\n"
                                "a=extmap:2/recvonly urn:ietf:params:rtp-hdrext:csrc-audio-level\r\n"
                                "a=extmap:3 urn:ietf:params:rtp-hdrext:sdes:mid\r\n"
                                "a=fmtp:109 maxplaybackrate=48000;stereo=1;useinbandfec=1\r\n"
                                "a=fmtp:101 0-15\r\n"
                                "a=ice-pwd:3a2c3e1d9f1b4c6e8a7d5b3c1e9f7a5d\r\n"
                                "a=ice-ufrag:9fb4c2a1\r\n"
                                "a=mid:0\r\n"
                                "a=msid:{7a0ef9c4-6bb4-4b9e-8c9f-1c2c3d4e5f60} {3d8d1f2e-0a4b-4c5d-9e6f-7a8b9c0d1e2f}\r\n"
                                "a=rtcp-mux\r\n"
                                "a=rtpmap:109 opus/48000/2\r\n"
                                "a=rtpmap:9 G722/8000/1\r\n"
                                "a=rtpmap:0 PCMU/8000\r\n"
                                "a=rtpmap:8 PCMA/8000\r\n"
                                "a=rtpmap:101 telephone-event/8000/1\r\n"
                                "a=setup:actpass\r\n"
                                "a=ssrc:1234 cname:{0b2e7f51-3c1d-4a2b-9e8f-1a2b3c4d5e6f}\r\n"
                                "m=video 0 UDP/TLS/RTP/SAVPF 120 124 126\r\n"
                                "c=IN IP4 0.0.0.0\r\n"
                                "a=bundle-only\r\n"
                                "a=recvonly\r\n"
                                "a=extmap:3 urn:ietf:params:rtp-hdrext:sdes:mid\r\n"
                                "a=extmap:7 http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01\r\n"
                                "a=fmtp:126 profile-level-id=42e01f;level-asymmetry-allowed=1;packetization-mode=1\r\n"
                                "a=fmtp:124 apt=120\r\n"
                                "a=ice-pwd:3a2c3e1d9f1b4c6e8a7d5b3c1e9f7a5d\r\n"
                                "a=ice-ufrag:9fb4c2a1\r\n"
                                "a=mid:1\r\n"
                                "a=rtcp-fb:120 nack\r\n"
                                "a=rtcp-fb:120 nack pli\r\n"
                                "a=rtcp-fb:120 ccm fir\r\n"
                                "a=rtcp-fb:120 goog-remb\r\n"
                                "a=rtcp-fb:120 transport-cc\r\n"
                                "a=rtcp-mux\r\n"
                                "a=rtcp-mux-only\r\n"
                                "a=rtpmap:120 VP8/90000\r\n"
                                "a=rtpmap:124 rtx/90000\r\n"
                                "a=rtpmap:126 H264/90000\r\n"
                                "a=setup:actpass\r\n";

static const char F_LINES[] = "v=0\r\n"
                              "o=- 1 1 IN IP6 2001:db8::1\r\n"
                              "s=x\r\n"
                              "c=IN IP6 2001:db8::1\r\n"
                              "b=AS:512\r\n"
                              "t=0 0\r\n"
                              "r=7d 1h 0 25h\r\n"
                              "a=recvonly\r\n"
                              "m=audio 49170/2 RTP/AVP 0 96\r\n"
                              "i=voice\r\n"
                              "b=AS:64\r\n"
                              "a=rtpmap:96 L16/16000/2\r\n"
                              "a=ptime:20\r\n"
                              "x=unknown\r\n";

static rtc_sdp* sdp_new(void)
{
    rtc_sdp* s = (rtc_sdp*)malloc(sizeof(rtc_sdp));
    if (!s)
        abort();
    rtc_sdp_init(s);
    return s;
}

static void sdp_del(rtc_sdp* s)
{
    rtc_sdp_free(s);
    free(s);
}

static int has_extra(const char* const* x, int n, const char* want)
{
    int i;
    for (i = 0; i < n; i++)
        if (strstr(x[i], want))
            return 1;
    return 0;
}

static char* join_lines(const char* const* x, int n)
{
    size_t len = 1, k = 0;
    int i;
    char* d;
    for (i = 0; i < n; i++)
        len += strlen(x[i]) + 1;
    d = (char*)malloc(len);
    if (!d)
        abort();
    for (i = 0; i < n; i++) {
        size_t l = strlen(x[i]);
        memcpy(d + k, x[i], l);
        d[k + l] = '\n';
        k += l + 1;
    }
    d[k] = 0;
    return d;
}

static int same_extra(const char* const* x, int nx, const char* const* y, int ny);

static int parse_exact(rtc_sdp* s, const char* txt, size_t n, const char** err)
{
    char* b = (char*)malloc(n ? n : 1);
    int r;
    if (!b)
        abort();
    memcpy(b, txt, n);
    r = rtc_sdp_parse(s, b, n, err);
    free(b);
    return r;
}

static int parse_str(rtc_sdp* s, const char* txt)
{
    const char* err;
    return parse_exact(s, txt, strlen(txt), &err);
}

static const char* parse_err(const char* txt)
{
    rtc_sdp* s = sdp_new();
    const char* err = NULL;
    int r = parse_exact(s, txt, strlen(txt), &err);
    sdp_del(s);
    return r < 0 ? (err ? err : "?") : NULL;
}

static const char* check_err(const char* txt)
{
    rtc_sdp* s = sdp_new();
    const char* err = NULL;
    int r = parse_exact(s, txt, strlen(txt), &err);
    if (r == 0)
        r = rtc_sdp_check(s, &err);
    else
        err = "parse";
    sdp_del(s);
    return r < 0 ? (err ? err : "?") : NULL;
}

static char* write_sdp(const rtc_sdp* s)
{
    rtc_sb b;
    rtc_sb_init(&b);
    if (rtc_sdp_write(s, &b) < 0) {
        rtc_sb_free(&b);
        return NULL;
    }
    if (!b.buf) {
        b.buf = (char*)malloc(1);
        if (!b.buf)
            abort();
        b.buf[0] = 0;
    }
    return b.buf;
}

static int cmp_line(const void* a, const void* b)
{
    return strcmp(*(char* const*)a, *(char* const*)b);
}

static int split_lines(const char* s, const char* seps, char** buf, char*** out)
{
    size_t n = strlen(s), i, st = 0;
    char* d = (char*)malloc(n + 1);
    char** v = (char**)malloc((n + 1) * sizeof(char*));
    int k = 0;
    if (!d || !v)
        abort();
    memcpy(d, s, n + 1);
    for (i = 0; i <= n; i++)
        if (i == n || strchr(seps, d[i])) {
            d[i] = 0;
            if (i > st)
                v[k++] = d + st;
            st = i + 1;
        }
    qsort(v, (size_t)k, sizeof(char*), cmp_line);
    *buf = d;
    *out = v;
    return k;
}

static int same_lines(const char* a, const char* b, const char* seps)
{
    char **x, **y, *bx, *by;
    int nx = split_lines(a, seps, &bx, &x), ny = split_lines(b, seps, &by, &y), i, eq = nx == ny;
    for (i = 0; eq && i < nx; i++)
        if (strcmp(x[i], y[i])) {
            printf("    line mismatch: '%s' vs '%s'\n", x[i], y[i]);
            eq = 0;
        }
    if (nx != ny)
        printf("    line count %d vs %d\n", nx, ny);
    free(bx);
    free(by);
    free(x);
    free(y);
    return eq;
}

static int same_extra(const char* const* x, int nx, const char* const* y, int ny)
{
    char *a = join_lines(x, nx), *b = join_lines(y, ny);
    int r = nx == ny && same_lines(a, b, "\n");
    free(a);
    free(b);
    return r;
}

static int codec_eq(const rtc_sdp_codec* a, const rtc_sdp_codec* b)
{
    return a->pt == b->pt && a->channels == b->channels && a->fb == b->fb && a->rtx_apt_set == b->rtx_apt_set && a->rtx_apt == b->rtx_apt
        && a->has_rtpmap == b->has_rtpmap && a->has_fmtp == b->has_fmtp && a->clock == b->clock && !strcmp(a->name, b->name)
        && !strcmp(a->fmtp, b->fmtp);
}

static int media_eq(const rtc_sdp_media* a, const rtc_sdp_media* b)
{
    int i;
    if (strcmp(a->kind, b->kind) || strcmp(a->proto, b->proto) || strcmp(a->fmt_text, b->fmt_text) || strcmp(a->conn, b->conn)
        || strcmp(a->mid, b->mid) || strcmp(a->ufrag, b->ufrag) || strcmp(a->pwd, b->pwd) || strcmp(a->ice_opts, b->ice_opts))
        return 0;
    if (a->fp_len != b->fp_len || memcmp(a->fp, b->fp, a->fp_len) || a->fp_alg != b->fp_alg || a->setup != b->setup || a->holdconn != b->holdconn
        || a->dir != b->dir || a->has_dir != b->has_dir || a->has_c != b->has_c || a->rtcp_mux != b->rtcp_mux || a->rtcp_mux_only != b->rtcp_mux_only
        || a->rtcp_rsize != b->rtcp_rsize || a->eoc != b->eoc || a->trickle != b->trickle || a->bundle_only != b->bundle_only
        || a->allow_mixed != b->allow_mixed || a->has_max_msg != b->has_max_msg || a->has_sctp_port != b->has_sctp_port
        || a->has_port_count != b->has_port_count || a->port != b->port || a->port_count != b->port_count || a->sctp_port != b->sctp_port
        || a->max_msg != b->max_msg)
        return 0;
    if (a->ncands != b->ncands || a->ncodecs != b->ncodecs || a->next != b->next || a->nssrc != b->nssrc || a->nmsid != b->nmsid
        || a->ngroups != b->ngroups || !same_extra(a->extra, a->nextra, b->extra, b->nextra))
        return 0;
    for (i = 0; i < a->ncands; i++)
        if (strcmp(a->cands[i].foundation, b->cands[i].foundation) || a->cands[i].prio != b->cands[i].prio
            || a->cands[i].addr.port != b->cands[i].addr.port)
            return 0;
    for (i = 0; i < a->ncodecs; i++)
        if (!codec_eq(&a->codecs[i], &b->codecs[i]))
            return 0;
    for (i = 0; i < a->next; i++)
        if (a->ext[i].id != b->ext[i].id || a->ext[i].has_dir != b->ext[i].has_dir || a->ext[i].dir != b->ext[i].dir
            || strcmp(a->ext[i].uri, b->ext[i].uri) || strcmp(a->ext[i].attrs, b->ext[i].attrs))
            return 0;
    for (i = 0; i < a->nmsid; i++)
        if (strcmp(a->msid[i].stream, b->msid[i].stream) || strcmp(a->msid[i].track, b->msid[i].track))
            return 0;
    for (i = 0; i < a->nssrc; i++)
        if (a->ssrcs[i].id != b->ssrcs[i].id || strcmp(a->ssrcs[i].cname, b->ssrcs[i].cname)
            || strcmp(a->ssrcs[i].msid.stream, b->ssrcs[i].msid.stream) || strcmp(a->ssrcs[i].msid.track, b->ssrcs[i].msid.track))
            return 0;
    for (i = 0; i < a->ngroups; i++)
        if (strcmp(a->groups[i].semantics, b->groups[i].semantics) || a->groups[i].n != b->groups[i].n
            || memcmp(a->groups[i].ssrc, b->groups[i].ssrc, sizeof(uint32_t) * a->groups[i].n))
            return 0;
    return 1;
}

static int sdp_eq(const rtc_sdp* a, const rtc_sdp* b)
{
    int i;
    if (a->sess_id != b->sess_id || a->sess_ver != b->sess_ver || a->ice_lite != b->ice_lite || a->trickle != b->trickle
        || a->fp_alg != b->fp_alg || a->fp_len != b->fp_len || memcmp(a->fp, b->fp, a->fp_len) || a->setup != b->setup
        || a->holdconn != b->holdconn || a->has_bundle != b->has_bundle || a->nbundle_groups != b->nbundle_groups || a->has_c != b->has_c
        || a->allow_mixed != b->allow_mixed || a->has_o != b->has_o || a->has_s != b->has_s || a->has_t != b->has_t)
        return 0;
    if (strcmp(a->origin, b->origin) || strcmp(a->name, b->name) || strcmp(a->timing, b->timing) || strcmp(a->conn, b->conn)
        || strcmp(a->ice_opts, b->ice_opts) || strcmp(a->ufrag, b->ufrag) || strcmp(a->pwd, b->pwd) || a->nbundle != b->nbundle
        || a->nmedia != b->nmedia || !same_extra(a->extra, a->nextra, b->extra, b->nextra))
        return 0;
    for (i = 0; i < a->nbundle; i++)
        if (strcmp(a->bundle[i], b->bundle[i]))
            return 0;
    for (i = 0; i < a->nmedia; i++)
        if (!media_eq(&a->media[i], &b->media[i]))
            return 0;
    return 1;
}

static int roundtrip(const char* txt, int exact, const char* what)
{
    rtc_sdp *a = sdp_new(), *b = sdp_new();
    const char* err = NULL;
    char *w1 = NULL, *w2 = NULL;
    int ok = parse_exact(a, txt, strlen(txt), &err) == 0;
    CHECK(ok, "%s: parses (%s)", what, err ? err : "");
    if (ok) {
        CHECK(rtc_sdp_check(a, &err) == 0, "%s: passes the strict check (%s)", what, err ? err : "");
        w1 = write_sdp(a);
        CHECK(w1 && same_lines(txt, w1, "\r\n"), "%s: the writer emits exactly the input lines, unknown ones included", what);
        if (exact)
            CHECK(w1 && !strcmp(w1, txt), "%s: lines already in RFC 8866 order come back byte-identical", what);
        ok = w1 && parse_exact(b, w1, strlen(w1), &err) == 0;
        CHECK(ok && sdp_eq(a, b), "%s: parse(write(parse(x))) equals parse(x) field by field", what);
        w2 = ok ? write_sdp(b) : NULL;
        CHECK(w2 && !strcmp(w1, w2), "%s: a second write is byte-identical", what);
    }
    free(w1);
    free(w2);
    sdp_del(a);
    sdp_del(b);
    return ok;
}

static void test_fixtures(void)
{
    rtc_sdp* s = sdp_new();
    const rtc_sdp_media* m;
    roundtrip(F_RFC, 1, "F_RFC");
    roundtrip(F_CHROME, 0, "F_CHROME");
    roundtrip(F_FIREFOX, 0, "F_FIREFOX");
    roundtrip(F_LINES, 1, "F_LINES");
    CHECK(parse_str(s, F_RFC) == 0 && s->nmedia == 3 && s->has_c && !s->media[0].has_c && s->media[2].has_c
            && !strcmp(s->media[2].conn, "IN IP6 2001:db8::2") && s->media[2].ncodecs == 1 && s->media[2].codecs[0].pt == 99
            && !strcmp(s->media[2].codecs[0].name, "h263-1998") && s->media[2].codecs[0].clock == 90000 && s->media[1].port == 49180
            && s->sess_id == 3724394400u && s->sess_ver == 3724394405u && !strcmp(s->name, "Call to John Smith"),
        "F_RFC: three m= sections, the session c= serves the audio ones, the video one has its own");
    CHECK(parse_str(s, F_CHROME) == 0, "F_CHROME parses");
    m = &s->media[0];
    CHECK(s->has_bundle && s->nbundle == 3 && !strcmp(s->bundle[2], "2") && s->allow_mixed && s->nmedia == 3, "F_CHROME: BUNDLE 0 1 2 and session extmap-allow-mixed");
    CHECK(m->ncodecs == 4 && m->codecs[0].pt == 111 && !strcmp(m->codecs[0].name, "opus") && m->codecs[0].clock == 48000 && m->codecs[0].channels == 2
            && m->codecs[0].fb == RTC_FB_TWCC && !strcmp(m->codecs[0].fmtp, "minptime=10;useinbandfec=1") && m->codecs[1].pt == 0
            && !strcmp(m->codecs[1].name, "PCMU") && m->codecs[1].channels == 0,
        "F_CHROME audio: payload types in m= order with rtpmap, rtcp-fb and fmtp");
    CHECK(m->next == 4 && m->ext[2].id == 4 && !strcmp(m->ext[3].uri, "urn:ietf:params:rtp-hdrext:sdes:mid") && m->has_dir && m->dir == RTC_DIR_SENDRECV
            && m->nmsid == 1 && !strcmp(m->msid[0].stream, "stream1") && !strcmp(m->msid[0].track, "track-a") && m->rtcp_mux && m->rtcp_rsize
            && m->nssrc == 1 && m->ssrcs[0].id == 1001 && !strcmp(m->ssrcs[0].cname, "Yk1rPq") && !strcmp(m->ssrcs[0].msid.track, "track-a"),
        "F_CHROME audio: extmap, direction, msid, rtcp-mux, ssrc cname and msid");
    CHECK(m->ncands == 2 && m->cands[1].type == RTC_CAND_SRFLX && !strcmp(m->ufrag, "EsAw") && m->fp_alg == RTC_FP_SHA256 && m->fp[0] == 0xA1
            && m->setup == RTC_SETUP_ACTPASS && m->trickle && has_extra(m->extra, m->nextra, "a=rtcp:9 IN IP4 0.0.0.0") && has_extra(m->extra, m->nextra, "network-id 1"),
        "F_CHROME audio: candidates, ICE, DTLS; the a=rtcp line and the candidate text are kept verbatim");
    m = &s->media[1];
    CHECK(m->ncodecs == 7 && m->codecs[0].fb == (RTC_FB_REMB | RTC_FB_TWCC | RTC_FB_FIR | RTC_FB_NACK | RTC_FB_PLI) && m->codecs[1].rtx_apt_set
            && m->codecs[1].rtx_apt == 96 && m->codecs[5].rtx_apt == 127 && has_extra(m->extra, m->nextra, "a=rtcp-fb:96 ccm tmmbr")
            && has_extra(m->extra, m->nextra, "a=ssrc:2001 label:legacy-label") && has_extra(m->extra, m->nextra, "a=x-google-flag:conference")
            && has_extra(m->extra, m->nextra, "b=AS:2000"),
        "F_CHROME video: all five known feedback types, rtx apt; unknown feedback, ssrc and session attributes are preserved");
    CHECK(m->next == 4 && m->ext[1].id == 5 && m->ext[1].has_dir && m->ext[1].dir == RTC_DIR_RECVONLY && !strcmp(m->ext[1].uri, "urn:3gpp:video-orientation")
            && m->ngroups == 1 && !strcmp(m->groups[0].semantics, "FID") && m->groups[0].n == 2 && m->groups[0].ssrc[1] == 2002 && m->nssrc == 2
            && !strcmp(m->ssrcs[0].msid.stream, "stream1"),
        "F_CHROME video: extmap direction, ssrc-group FID");
    m = &s->media[2];
    CHECK(!strcmp(m->kind, "application") && !strcmp(m->proto, "UDP/DTLS/SCTP") && !strcmp(m->fmt_text, "webrtc-datachannel") && m->ncodecs == 0
            && m->sctp_port == 5000 && m->max_msg == 262144,
        "F_CHROME data: the fmt is text, not a payload type");
    CHECK(has_extra(s->extra, s->nextra, "a=msid-semantic: WMS stream1"), "F_CHROME: an unknown session attribute is preserved");
    CHECK(parse_str(s, F_FIREFOX) == 0 && s->fp_len == 32 && !strcmp(s->ice_opts, "trickle") && s->media[1].port == 0 && s->media[1].bundle_only
            && !rtc_sdp_media_rejected(&s->media[1]) && s->media[1].rtcp_mux_only && s->media[1].dir == RTC_DIR_RECVONLY
            && s->media[0].codecs[1].pt == 9 && s->media[0].codecs[1].channels == 1 && s->media[0].ext[1].dir == RTC_DIR_RECVONLY
            && !strcmp(s->media[0].msid[0].stream, "{7a0ef9c4-6bb4-4b9e-8c9f-1c2c3d4e5f60}") && s->media[1].codecs[1].rtx_apt == 120,
        "F_FIREFOX: session fingerprint, bundle-only with port 0 is not rejected, rtcp-mux-only, braced msid");
    CHECK(parse_str(s, F_LINES) == 0 && s->media[0].has_port_count && s->media[0].port_count == 2 && s->media[0].codecs[1].channels == 2
            && has_extra(s->extra, s->nextra, "r=7d 1h 0 25h") && has_extra(s->extra, s->nextra, "a=recvonly") && has_extra(s->media[0].extra, s->media[0].nextra, "x=unknown"),
        "F_LINES: port count, r= and session-level direction kept as lines, an unknown line type is preserved");
    CHECK(parse_str(s, "v=0\no=- 1 1 IN IP4 0.0.0.0\ns=-\nt=0 0\nc=IN IP4 0.0.0.0\nm=audio 9 RTP/AVP 0\n") == 0 && s->nmedia == 1,
        "LF-only line ends are accepted");
    sdp_del(s);
}

#define HDR "v=0\r\no=- 1 1 IN IP4 0.0.0.0\r\ns=-\r\nt=0 0\r\n"
#define CIN "c=IN IP4 0.0.0.0\r\n"

static const char* cap_err(const char* head, const char* rep, int count, const char* tail)
{
    size_t cap = strlen(head) + (size_t)count * (strlen(rep) + 12) + strlen(tail) + 1, n;
    char* b = (char*)malloc(cap);
    const char* r;
    int i;
    if (!b)
        abort();
    n = (size_t)snprintf(b, cap, "%s", head);
    for (i = 0; i < count; i++)
        n += (size_t)snprintf(b + n, cap - n, rep, i + 1);
    snprintf(b + n, cap - n, "%s", tail);
    r = parse_err(b);
    free(b);
    return r;
}

static const char* long_line_err(size_t fmtp_len)
{
    size_t cap = fmtp_len + 256, n;
    char* b = (char*)malloc(cap);
    const char* r;
    if (!b)
        abort();
    n = (size_t)snprintf(b, cap, "%s%sm=audio 9 RTP/AVP 0\r\na=fmtp:0 ", HDR, CIN);
    memset(b + n, 'k', fmtp_len);
    n += fmtp_len;
    snprintf(b + n, cap - n, "\r\n");
    r = parse_err(b);
    free(b);
    return r;
}

static void test_malformed(void)
{
    char big[8192];
    size_t n;
    int i;
    CHECK(check_err(HDR "m=audio 9 RTP/AVP 0\r\n") != NULL, "missing c= in the session and the m= section (RFC 8866 5.7)");
    CHECK(check_err(HDR CIN "m=audio 9 RTP/AVP 0\r\n") == NULL, "a session c= covers the m= sections");
    CHECK(check_err(HDR "m=audio 9 RTP/AVP 0\r\n" CIN "m=video 9 RTP/AVP 96\r\na=rtpmap:96 VP8/90000\r\n") != NULL, "c= missing in the second m= section only");
    CHECK(check_err("v=0\r\ns=-\r\nt=0 0\r\n" CIN) != NULL, "missing o= (RFC 8866 5)");
    CHECK(check_err("v=0\r\no=- 1 1 IN IP4 0.0.0.0\r\ns=-\r\n" CIN) != NULL, "missing t=");
    CHECK(parse_err(HDR CIN "m=video 9 RTP/AVP 96\r\na=rtpmap:96 VP8\r\n") != NULL, "rtpmap without a clock rate");
    CHECK(parse_err(HDR CIN "m=video 9 RTP/AVP 96\r\na=rtpmap:96 VP8/0\r\n") != NULL, "rtpmap with clock rate 0");
    CHECK(parse_err(HDR CIN "m=video 9 RTP/AVP 96\r\na=rtpmap:96 VP8/9x000\r\n") != NULL, "rtpmap with a non-numeric clock rate");
    CHECK(parse_err(HDR CIN "m=video 9 RTP/AVP 96\r\na=rtpmap:96 VP8/90000/1/2\r\n") != NULL, "rtpmap with three slashes");
    CHECK(parse_err(HDR CIN "m=video 9 RTP/AVP 96\r\na=rtpmap:96 VP8/90000 extra\r\n") != NULL, "rtpmap with a trailing token");
    CHECK(parse_err(HDR CIN "m=video 9 RTP/AVP 96\r\na=rtpmap:128 VP8/90000\r\n") != NULL, "rtpmap payload type above 127");
    CHECK(parse_err(HDR CIN "m=video 9 RTP/AVP 96\r\na=rtpmap:96 VP8/90000\r\na=rtpmap:96 VP9/90000\r\n") != NULL, "two rtpmap lines for one payload type");
    CHECK(parse_err(HDR CIN "m=audio 9 RTP/AVP 111\r\na=rtpmap:111 opus/48000/0\r\n") != NULL, "rtpmap with 0 channels");
    CHECK(check_err(HDR CIN "m=video 9 RTP/AVP 96\r\n") != NULL, "a dynamic payload type without rtpmap (RFC 8866 6.6)");
    CHECK(check_err(HDR CIN "m=audio 9 RTP/AVP 0\r\n") == NULL, "a static payload type needs no rtpmap");
    CHECK(parse_err(HDR CIN "m=video 9 RTP/AVP 96 96\r\na=rtpmap:96 VP8/90000\r\n") != NULL, "a payload type listed twice in m=");
    CHECK(parse_err(HDR CIN "m=video 9 RTP/AVP\r\n") != NULL, "m= without a format");
    CHECK(parse_err(HDR CIN "m=video 70000 RTP/AVP 96\r\n") != NULL, "m= port above 65535");
    CHECK(check_err(HDR CIN "m=audio 9 RTP/AVP 0\r\na=mid:a\r\nm=audio 9 RTP/AVP 0\r\na=mid:a\r\n") != NULL, "duplicate mid across m= sections");
    CHECK(check_err(HDR CIN "m=audio 9 RTP/AVP 0\r\na=mid:a\r\nm=audio 9 RTP/AVP 0\r\na=mid:b\r\nm=audio 9 RTP/AVP 0\r\n") == NULL,
        "distinct mids, and an m= section without one, pass");
    CHECK(parse_err(HDR CIN "m=audio 9 RTP/AVP 0\r\na=mid:a\r\na=mid:b\r\n") != NULL, "two a=mid lines in one m= section");
    CHECK(check_err(HDR CIN "a=group:BUNDLE a z\r\nm=audio 9 RTP/AVP 0\r\na=mid:a\r\n") != NULL, "a BUNDLE mid with no m= section");
    CHECK(check_err(HDR CIN "a=group:BUNDLE a a\r\nm=audio 9 RTP/AVP 0\r\na=mid:a\r\n") != NULL, "a mid twice in the BUNDLE group");
    CHECK(check_err(HDR CIN "a=group:BUNDLE a\r\na=group:BUNDLE b\r\nm=audio 9 RTP/AVP 0\r\na=mid:a\r\nm=audio 9 RTP/AVP 0\r\na=mid:b\r\n") != NULL,
        "two BUNDLE groups are refused rather than half-read");
    CHECK(check_err(HDR CIN "a=group:BUNDLE a\r\nm=audio 9 RTP/AVP 0\r\na=mid:a\r\na=bundle-only\r\n") != NULL, "bundle-only with a nonzero port (RFC 9143 6)");
    CHECK(check_err(HDR CIN "m=audio 0 RTP/AVP 0\r\na=mid:a\r\na=bundle-only\r\n") != NULL, "bundle-only outside a BUNDLE group");
    CHECK(check_err(HDR CIN "m=audio 9 RTP/AVP 0\r\na=rtcp-mux-only\r\n") != NULL, "rtcp-mux-only without rtcp-mux (RFC 8858 3)");
    CHECK(check_err(HDR CIN "m=audio 9 RTP/AVP 0\r\na=extmap:1 urn:a\r\na=extmap:1 urn:b\r\n") != NULL, "duplicate extmap id (RFC 8285 5)");
    CHECK(parse_err(HDR CIN "m=audio 9 RTP/AVP 0\r\na=extmap:0 urn:a\r\n") != NULL, "extmap id 0");
    CHECK(parse_err(HDR CIN "m=audio 9 RTP/AVP 0\r\na=extmap:256 urn:a\r\n") != NULL, "extmap id 256");
    CHECK(parse_err(HDR CIN "m=audio 9 RTP/AVP 0\r\na=extmap:2/sideways urn:a\r\n") != NULL, "extmap with an unknown direction");
    CHECK(parse_err(HDR CIN "m=audio 9 RTP/AVP 0\r\na=extmap:2\r\n") != NULL, "extmap without a URI");
    CHECK(parse_err(HDR CIN "m=audio 9 RTP/AVP 0\r\na=mid:0123456789abcdef0123456789abcdefX\r\n") != NULL, "a 33-byte mid is an error, not truncated");
    CHECK(parse_err(HDR CIN "m=audio 9 RTP/AVP 0\r\na=mid:0123456789abcdef0123456789abcdef\r\n") == NULL, "a 32-byte mid fits");
    CHECK(parse_err(HDR CIN "m=audio 9 RTP/AVP 0\r\na=msid:s 0123456789012345678901234567890123456789012345678901234567890123X\r\n") != NULL,
        "an msid appdata over 64 bytes (RFC 8830 2)");
    CHECK(parse_err(HDR CIN "m=audio 9 RTP/AVP 0\r\na=ssrc:5\r\n") != NULL, "ssrc without an attribute (RFC 5576 4.1)");
    CHECK(parse_err(HDR CIN "m=audio 9 RTP/AVP 0\r\na=ssrc-group:FID\r\n") != NULL, "ssrc-group without ssrcs");
    CHECK(parse_err(HDR CIN "m=audio 9 RTP/AVP 0\r\na=ssrc-group:FID 1 x\r\n") != NULL, "ssrc-group with a non-numeric ssrc");
    CHECK(parse_err(HDR CIN "m=audio 9 RTP/AVP 0\r\na=fmtp:0\r\n") != NULL, "fmtp without parameters");
    CHECK(parse_err(HDR CIN "m=audio 9 RTP/AVP 0\r\na=fmtp:0 a=1\r\na=fmtp:0 b=2\r\n") != NULL, "two fmtp lines for one payload type");
    CHECK(parse_err(HDR CIN "m=video 9 RTP/AVP 97\r\na=rtpmap:97 rtx/90000\r\na=fmtp:97 apt=x\r\n") != NULL, "rtx apt not a number");
    CHECK(parse_err(HDR CIN "m=audio 9 RTP/AVP 0\r\na=sendrecv\r\na=recvonly\r\n") != NULL, "two direction attributes");
    CHECK(parse_err(HDR "c=IN IP5 1.2.3.4\r\n") != NULL, "c= with an unknown address type");
    CHECK(parse_err(HDR "c=IN IP4\r\n") != NULL, "c= without an address");
    CHECK(parse_err("v=0\r\no=- 1 IN IP4 0.0.0.0\r\ns=-\r\nt=0 0\r\n") != NULL, "o= with five fields");
    CHECK(parse_err("v=0\r\no=- x 1 IN IP4 0.0.0.0\r\ns=-\r\nt=0 0\r\n") != NULL, "o= with a non-numeric session id");
    CHECK(parse_err("v=0\r\nv=0\r\n") != NULL, "two v= lines");
    CHECK(parse_err(HDR "Z=x\r\n") != NULL, "an upper-case line type");
    CHECK(parse_err(HDR "a=\r\n") != NULL, "an attribute with no name");
    CHECK(parse_err(HDR CIN "m=audio 9 RTP/AVP 0\r\na=ice-ufrag:abcd\r\na=ice-ufrag:efgh\r\n") != NULL, "two ice-ufrag lines in one section");
    CHECK(parse_err(HDR CIN "m=audio 9 RTP/AVP 0\r\na=fingerprint:sha-256 AB:CD\r\n") != NULL, "a fingerprint of the wrong length");
    {
        static const char t[] = HDR "a=x\0y\r\n";
        rtc_sdp* s = sdp_new();
        const char* err;
        CHECK(parse_exact(s, t, sizeof(t) - 1, &err) < 0, "a NUL inside a line");
        sdp_del(s);
    }
    CHECK(parse_err(HDR CIN "m=audio 9 RTP/AVP 0\r\na=x-unknown:1 2 3\r\na=another\r\n") == NULL, "unknown attributes are accepted (RFC 8866 5.13)");
    CHECK(parse_err(HDR CIN "m=audio 9 RTP/AVP 0\r\na=msid:a\r\na=msid:b\r\na=msid:c\r\na=msid:d\r\na=msid:e\r\n") == NULL, "five msid lines fit");
    n = (size_t)snprintf(big, sizeof(big), "%s%sm=video 9 RTP/AVP", HDR, CIN);
    for (i = 0; i < 128; i++)
        n += (size_t)snprintf(big + n, sizeof(big) - n, " %d", i);
    snprintf(big + n, sizeof(big) - n, "\r\n");
    CHECK(parse_err(big) == NULL, "the whole payload type space (0..127) fits in one m= section");
    CHECK(cap_err(HDR CIN, "m=audio 9 RTP/AVP 0\r\n", RTC_SDP_MAX_MEDIA, "") == NULL, "RTC_SDP_MAX_MEDIA m= sections fit");
    CHECK(cap_err(HDR CIN, "m=audio 9 RTP/AVP 0\r\n", RTC_SDP_MAX_MEDIA + 1, "") != NULL, "one m= section over RTC_SDP_MAX_MEDIA is an error");
    CHECK(cap_err(HDR CIN "m=audio 9 RTP/AVP 0\r\n", "a=extmap:%d urn:x\r\n", RTC_SDP_MAX_EXT, "") == NULL, "RTC_SDP_MAX_EXT extmaps fit");
    CHECK(cap_err(HDR CIN "m=audio 9 RTP/AVP 0\r\n", "a=extmap:%d urn:x\r\n", RTC_SDP_MAX_EXT + 1, "") != NULL, "one extmap over RTC_SDP_MAX_EXT is an error");
    CHECK(cap_err(HDR CIN "m=audio 9 RTP/AVP 0\r\n", "a=candidate:%d 1 udp 1 10.0.0.1 9 typ host\r\n", RTC_SDP_MAX_CANDS, "") == NULL,
        "RTC_SDP_MAX_CANDS candidates fit");
    CHECK(cap_err(HDR CIN "m=audio 9 RTP/AVP 0\r\n", "a=candidate:%d 1 udp 1 10.0.0.1 9 typ host\r\n", RTC_SDP_MAX_CANDS + 1, "") != NULL,
        "one candidate over RTC_SDP_MAX_CANDS is an error, never a silent drop");
    CHECK(cap_err(HDR CIN "m=audio 9 RTP/AVP 0\r\n", "a=ssrc:%d cname:c\r\n", RTC_SDP_MAX_SSRC, "") == NULL, "RTC_SDP_MAX_SSRC ssrcs fit");
    CHECK(cap_err(HDR CIN "m=audio 9 RTP/AVP 0\r\n", "a=ssrc:%d cname:c\r\n", RTC_SDP_MAX_SSRC + 1, "") != NULL, "one ssrc over RTC_SDP_MAX_SSRC is an error");
    CHECK(cap_err(HDR CIN "m=audio 9 RTP/AVP 0\r\n", "a=msid:s%d\r\n", RTC_SDP_MAX_MSID + 1, "") != NULL, "one msid over RTC_SDP_MAX_MSID is an error");
    CHECK(cap_err(HDR CIN "m=audio 9 RTP/AVP 0\r\n", "a=ssrc-group:FID %d\r\n", RTC_SDP_MAX_GROUPS + 1, "") != NULL,
        "one ssrc-group over RTC_SDP_MAX_GROUPS is an error");
    CHECK(cap_err(HDR CIN "m=audio 9 RTP/AVP 0\r\na=ssrc-group:FEC-FR", " %d", RTC_SDP_GROUP_SSRCS + 1, "\r\n") != NULL,
        "an ssrc-group with more than RTC_SDP_GROUP_SSRCS ssrcs is an error");
    CHECK(cap_err(HDR CIN "m=audio 9 RTP/AVP 0\r\n", "a=x-filler-%d:abc\r\n", RTC_SDP_MAX_EXTRA, "") == NULL, "RTC_SDP_MAX_EXTRA unknown lines fit");
    CHECK(cap_err(HDR CIN "m=audio 9 RTP/AVP 0\r\n", "a=x-filler-%d:abc\r\n", RTC_SDP_MAX_EXTRA + 1, "") != NULL,
        "one unknown line over RTC_SDP_MAX_EXTRA is an error, never a silent drop");
    CHECK(long_line_err(RTC_SDP_LINE_MAX - 7) == NULL, "an fmtp line of exactly RTC_SDP_LINE_MAX bytes fits");
    CHECK(long_line_err(RTC_SDP_LINE_MAX - 6) != NULL, "a line one byte over RTC_SDP_LINE_MAX is an error, never truncated");
    {
        size_t big_n = RTC_SDP_MAX_INPUT + 1;
        char* huge = (char*)malloc(big_n);
        rtc_sdp* s = sdp_new();
        const char* err = NULL;
        size_t k;
        if (!huge)
            abort();
        memcpy(huge, HDR, sizeof(HDR) - 1);
        for (k = sizeof(HDR) - 1; k < big_n; k++)
            huge[k] = (k % 64 == 63) ? '\n' : 'x';
        CHECK(rtc_sdp_parse(s, huge, big_n, &err) < 0 && err && strstr(err, "input limit"), "input over RTC_SDP_MAX_INPUT is refused up front");
        CHECK(rtc_sdp_bytes(s) < 64 * 1024, "an input refused up front allocates nothing (%u bytes)", (unsigned)rtc_sdp_bytes(s));
        free(huge);
        sdp_del(s);
    }
}

static const rtc_sdp_codec CAP_AUDIO[] = {
    { .name = "opus", .fmtp = "minptime=10;useinbandfec=1;stereo=0", .clock = 48000, .channels = 2, .fb = RTC_FB_TWCC, .has_rtpmap = 1, .has_fmtp = 1 },
    { .name = "PCMU", .fmtp = "", .clock = 8000, .has_rtpmap = 1 },
};

static const rtc_sdp_codec CAP_VIDEO[] = {
    { .name = "VP8", .fmtp = "", .clock = 90000, .fb = RTC_FB_NACK | RTC_FB_PLI | RTC_FB_FIR | RTC_FB_TWCC, .has_rtpmap = 1 },
    { .name = "rtx", .fmtp = "", .clock = 90000, .has_rtpmap = 1 },
    { .name = "H264", .fmtp = "level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e034", .clock = 90000, .fb = RTC_FB_NACK | RTC_FB_PLI, .has_rtpmap = 1, .has_fmtp = 1 },
};

static const char* const CAP_EXT[] = {
    "http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01",
    "urn:ietf:params:rtp-hdrext:sdes:mid",
};

static const uint8_t LOCAL_FP[32] = { 1, 2, 3 };

static void jsep_setup(rtc_jsep_caps* c, rtc_jsep_params* p)
{
    memset(c, 0, sizeof(*c));
    c->audio = CAP_AUDIO;
    c->naudio = 2;
    c->video = CAP_VIDEO;
    c->nvideo = 3;
    c->ext = CAP_EXT;
    c->next = 2;
    c->data = 1;
    c->mixed = 1;
    memset(p, 0, sizeof(*p));
    p->sess_id = 77;
    p->sess_ver = 1;
    p->ufrag = "LoCa";
    p->pwd = "localpasswordlocalpassword";
    p->fp = LOCAL_FP;
    p->fp_alg = RTC_FP_SHA256;
    p->fp_len = 32;
    p->trickle = 1;
    p->sctp_port = 5000;
    p->max_msg = 65536;
}

static int answer_ok(const rtc_sdp* ans, const char* what)
{
    rtc_sdp* b = sdp_new();
    const char* err = NULL;
    char *w = write_sdp(ans), *w2 = NULL;
    int ok = w && parse_exact(b, w, strlen(w), &err) == 0;
    CHECK(ok, "%s: the written answer parses (%s)", what, err ? err : "");
    if (ok) {
        CHECK(rtc_sdp_check(b, &err) == 0, "%s: the written answer passes the strict check (%s)", what, err ? err : "");
        w2 = write_sdp(b);
        CHECK(w2 && !strcmp(w, w2), "%s: the answer goes through the same writer and is stable", what);
        CHECK(!strstr(w, "a=bundle-only"), "%s: no answer m= section has bundle-only (RFC 8829 5.3.1)", what);
    }
    free(w);
    free(w2);
    sdp_del(b);
    return ok;
}

static const rtc_sdp_codec* find_pt(const rtc_sdp_media* m, int pt)
{
    int i;
    for (i = 0; i < m->ncodecs; i++)
        if (m->codecs[i].pt == pt)
            return &m->codecs[i];
    return NULL;
}

static void test_dir_table(void)
{
    static const struct {
        int offered, local, want;
    } t[] = {
        { RTC_DIR_SENDRECV, RTC_DIR_SENDRECV, RTC_DIR_SENDRECV },
        { RTC_DIR_SENDRECV, RTC_DIR_SENDONLY, RTC_DIR_SENDONLY },
        { RTC_DIR_SENDRECV, RTC_DIR_RECVONLY, RTC_DIR_RECVONLY },
        { RTC_DIR_SENDRECV, RTC_DIR_INACTIVE, RTC_DIR_INACTIVE },
        { RTC_DIR_SENDONLY, RTC_DIR_SENDRECV, RTC_DIR_RECVONLY },
        { RTC_DIR_SENDONLY, RTC_DIR_SENDONLY, RTC_DIR_INACTIVE },
        { RTC_DIR_SENDONLY, RTC_DIR_RECVONLY, RTC_DIR_RECVONLY },
        { RTC_DIR_SENDONLY, RTC_DIR_INACTIVE, RTC_DIR_INACTIVE },
        { RTC_DIR_RECVONLY, RTC_DIR_SENDRECV, RTC_DIR_SENDONLY },
        { RTC_DIR_RECVONLY, RTC_DIR_SENDONLY, RTC_DIR_SENDONLY },
        { RTC_DIR_RECVONLY, RTC_DIR_RECVONLY, RTC_DIR_INACTIVE },
        { RTC_DIR_RECVONLY, RTC_DIR_INACTIVE, RTC_DIR_INACTIVE },
        { RTC_DIR_INACTIVE, RTC_DIR_SENDRECV, RTC_DIR_INACTIVE },
        { RTC_DIR_INACTIVE, RTC_DIR_SENDONLY, RTC_DIR_INACTIVE },
        { RTC_DIR_INACTIVE, RTC_DIR_RECVONLY, RTC_DIR_INACTIVE },
        { RTC_DIR_INACTIVE, RTC_DIR_INACTIVE, RTC_DIR_INACTIVE },
    };
    size_t i;
    for (i = 0; i < sizeof(t) / sizeof(t[0]); i++)
        CHECK(rtc_jsep_dir_answer(t[i].offered, t[i].local) == t[i].want, "direction answer row %u (RFC 3264 6.1, RFC 8829 5.3.1)", (unsigned)i);
}

static rtc_sdp_codec mk(int pt, const char* name, uint32_t clock, int ch, const char* fmtp)
{
    rtc_sdp_codec c;
    memset(&c, 0, sizeof(c));
    c.pt = (uint8_t)pt;
    c.clock = clock;
    c.channels = (uint8_t)ch;
    c.has_rtpmap = name[0] != 0;
    c.name = name;
    c.fmtp = fmtp;
    c.has_fmtp = fmtp[0] != 0;
    return c;
}

static void test_codec_table(void)
{
    static const struct {
        const char *on, *of;
        int opt;
        const char *ln, *lf;
        int ch_o, ch_l, want;
        const char* what;
    } t[] = {
        { "H264", "packetization-mode=1;profile-level-id=42e01f", 102, "H264", "packetization-mode=1;profile-level-id=42e01f", 0, 0, 1, "same profile and mode" },
        { "H264", "packetization-mode=1;profile-level-id=42e01f", 102, "H264", "profile-level-id=42e01f", 0, 0, 0, "mode 1 against an absent mode (0)" },
        { "H264", "packetization-mode=0;profile-level-id=42e01f", 102, "H264", "profile-level-id=42e01f", 0, 0, 1, "explicit mode 0 equals the default" },
        { "H264", "packetization-mode=1;profile-level-id=42e00a", 102, "H264", "packetization-mode=1;profile-level-id=42e034", 0, 0, 1, "levels may differ" },
        { "H264", "packetization-mode=1;profile-level-id=42e01f", 102, "H264", "packetization-mode=1;profile-level-id=4d001f", 0, 0, 0, "Baseline against Main" },
        { "H264", "packetization-mode=1;profile-level-id=42001f", 102, "H264", "packetization-mode=1;profile-level-id=42e01f", 0, 0, 0, "different constraint flags" },
        { "H264", "", 102, "H264", "profile-level-id=42000a", 0, 0, 1, "absent profile-level-id is Baseline level 1" },
        { "H264", "packetization-mode=1;profile-level-id=42f00b", 102, "H264", "packetization-mode=1;profile-level-id=42e01f", 0, 0, 1, "level 1b flag is not a profile difference" },
        { "H264", "packetization-mode=1;profile-level-id=42e0zz", 102, "H264", "packetization-mode=1;profile-level-id=42e01f", 0, 0, 0, "bad hex never matches" },
        { "H264", "packetization-mode=1;profile-level-id=42e01", 102, "H264", "packetization-mode=1;profile-level-id=42e01f", 0, 0, 0, "short profile-level-id" },
        { "opus", "minptime=10", 111, "opus", "stereo=1", 2, 2, 1, "Opus fmtp is declarative and never blocks a match" },
        { "opus", "", 111, "opus", "", 0, 2, 0, "opus/48000 (one channel) against opus/48000/2" },
        { "vp8", "", 96, "VP8", "", 0, 0, 1, "encoding names are case-insensitive" },
        { "", "", 0, "PCMU", "", 0, 0, 1, "static payload type 0 without rtpmap is PCMU/8000" },
        { "", "", 8, "PCMU", "", 0, 0, 0, "static payload type 8 is PCMA" },
        { "", "", 35, "PCMU", "", 0, 0, 0, "an unknown payload type without rtpmap never matches" },
        { "H264", "packetization-mode=1;profile-level-id=4de01f", 102, "H264", "packetization-mode=1;profile-level-id=42e01f", 0, 0, 1, "4de01f and 42e01f are both Constrained Baseline (RFC 6184 Table 5)" },
        { "H264", "packetization-mode=1;profile-level-id=58c01f", 102, "H264", "packetization-mode=1;profile-level-id=42e01f", 0, 0, 1, "58c01f is Constrained Baseline too" },
        { "H264", "packetization-mode=1;profile-level-id=58801f", 102, "H264", "packetization-mode=1;profile-level-id=42001f", 0, 0, 1, "58801f and 42001f are both Baseline" },
        { "H264", "packetization-mode=1;profile-level-id=4d401f", 102, "H264", "packetization-mode=1;profile-level-id=4d001f", 0, 0, 1, "constraint_set1 alone keeps Main" },
        { "H264", "packetization-mode=1;profile-level-id=640c1f", 102, "H264", "packetization-mode=1;profile-level-id=640c1f", 0, 0, 0, "a combination outside Table 5 never matches, even itself" },
    };
    size_t i;
    for (i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        int audio = !strcmp(t[i].ln, "opus") || !strcmp(t[i].ln, "PCMU");
        rtc_sdp_codec o = mk(t[i].opt, t[i].on, t[i].on[0] ? (audio ? 48000u : 90000u) : 0, t[i].ch_o, t[i].of);
        rtc_sdp_codec l = mk(0, t[i].ln, !strcmp(t[i].ln, "PCMU") ? 8000u : audio ? 48000u : 90000u, t[i].ch_l, t[i].lf);
        CHECK(rtc_jsep_codec_match(&o, &l) == t[i].want, "codec match: %s", t[i].what);
    }
    {
        rtc_sdp_codec o = mk(96, "VP8", 90000, 0, ""), l = mk(0, "VP8", 48000, 0, "");
        CHECK(!rtc_jsep_codec_match(&o, &l), "codec match: clock rates must agree");
    }
}

static void test_h264_level(void)
{
    static const struct {
        const char *off, *loc, *want, *what;
    } t[] = {
        { "level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f", "level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e034",
            "level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e034", "both allow asymmetry: the answer states the level it can receive" },
        { "packetization-mode=1;profile-level-id=42e01f", "level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e034",
            "level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f", "no asymmetry: never above the offered level" },
        { "packetization-mode=1;profile-level-id=42e034", "packetization-mode=1;profile-level-id=42e01f", "packetization-mode=1;profile-level-id=42e01f",
            "no asymmetry: the lower local level" },
        { "packetization-mode=1;profile-level-id=42f00b", "packetization-mode=1;profile-level-id=42e01f", "packetization-mode=1;profile-level-id=42f00b",
            "Baseline level 1b (constraint_set3, level_idc 11) is below 3.1" },
        { "packetization-mode=1;profile-level-id=42e00c", "packetization-mode=1;profile-level-id=42f00b", "packetization-mode=1;profile-level-id=42f00b",
            "level 1b is below 1.2" },
        { "packetization-mode=1;profile-level-id=42e00a", "packetization-mode=1;profile-level-id=42f00b", "packetization-mode=1;profile-level-id=42e00a",
            "level 1 is below 1b" },
        { "profile-level-id=640009", "profile-level-id=64001f", "profile-level-id=640009", "High level 1b is level_idc 9" },
        { "packetization-mode=1", "packetization-mode=1", "packetization-mode=1", "default profile and level both sides: nothing to state" },
        { "profile-level-id=42e01f", "packetization-mode=0", "packetization-mode=0;profile-level-id=42e00a",
            "the local default level 1 is stated when the offer named a profile" },
    };
    size_t i;
    char out[RTC_SDP_LINE_MAX];
    for (i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        int r = rtc_jsep_h264_answer_fmtp(t[i].off, t[i].loc, out, sizeof(out));
        CHECK(r == 0 && !strcmp(out, t[i].want), "h264 answer fmtp (RFC 6184 8.2.2): %s -> '%s'", t[i].what, r == 0 ? out : "error");
    }
    CHECK(rtc_jsep_h264_answer_fmtp("profile-level-id=42e01f", "packetization-mode=1;profile-level-id=42e01f", out, 20) < 0, "h264 answer fmtp: no room is an error");
}

static void test_answer_browser(void)
{
    rtc_sdp *o = sdp_new(), *a = sdp_new();
    rtc_jsep_caps c;
    rtc_jsep_params p;
    rtc_jsep_local lo[3];
    const char* err;
    const rtc_sdp_media* m;
    const rtc_sdp_codec* k;
    jsep_setup(&c, &p);
    memset(lo, 0, sizeof(lo));
    lo[0].dir = RTC_DIR_RECVONLY;
    lo[1].dir = RTC_DIR_SENDRECV;
    lo[1].msid.stream = "ls";
    lo[1].msid.track = "lt";
    lo[2].dir = RTC_DIR_SENDRECV;
    CHECK(parse_str(o, F_CHROME) == 0 && rtc_jsep_answer(o, &c, lo, &p, a, &err) == 0, "answer to F_CHROME");
    answer_ok(a, "F_CHROME answer");
    CHECK(a->nmedia == 3 && a->has_bundle && a->nbundle == 3 && !strcmp(a->bundle[0], "0") && a->allow_mixed, "BUNDLE: every accepted mid, the offerer-tagged one first");
    m = &a->media[0];
    CHECK(m->port == 9 && m->ncodecs == 2 && m->codecs[0].pt == 111 && m->codecs[1].pt == 0 && !strcmp(m->codecs[0].name, "opus") && m->codecs[0].channels == 2,
        "audio: payload types from the offer in offer order, PCMA and telephone-event dropped");
    CHECK(!strcmp(m->codecs[0].fmtp, "minptime=10;useinbandfec=1;stereo=0") && m->codecs[0].fb == RTC_FB_TWCC && m->codecs[1].fb == 0,
        "audio: Opus fmtp is the local declaration; rtcp-fb is the intersection");
    CHECK(m->next == 2 && m->ext[0].id == 4 && m->ext[1].id == 9 && !strcmp(m->ext[1].uri, "urn:ietf:params:rtp-hdrext:sdes:mid"),
        "audio: header extensions intersected, ids from the offer");
    CHECK(m->dir == RTC_DIR_RECVONLY && m->nmsid == 0 && m->rtcp_mux && m->rtcp_rsize && !strcmp(m->mid, "0") && m->setup == RTC_SETUP_ACTIVE
            && !strcmp(m->ufrag, "LoCa") && m->fp_len == 32 && m->trickle,
        "audio: recvonly answer has no msid; rtcp-mux; setup active for actpass; local ICE and fingerprint");
    m = &a->media[1];
    CHECK(m->ncodecs == 4 && m->codecs[0].pt == 96 && m->codecs[1].pt == 97 && m->codecs[2].pt == 102 && m->codecs[3].pt == 103,
        "video: VP8 and its rtx, H.264 mode 1 Baseline and its rtx; mode-0 H.264, its rtx and Main are dropped");
    k = find_pt(m, 97);
    CHECK(k && k->rtx_apt_set && k->rtx_apt == 96 && !strcmp(k->fmtp, "apt=96") && find_pt(m, 103)->rtx_apt == 102, "video: rtx apt values follow the offer");
    k = find_pt(m, 102);
    CHECK(k && !strcmp(k->fmtp, "level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e034") && k->fb == (RTC_FB_NACK | RTC_FB_PLI),
        "video: H.264 answer fmtp keeps the profile, takes the local level under asymmetry; feedback intersected");
    CHECK(find_pt(m, 96)->fb == (RTC_FB_NACK | RTC_FB_PLI | RTC_FB_FIR | RTC_FB_TWCC), "video: goog-remb not supported locally, so not answered");
    CHECK(m->dir == RTC_DIR_SENDRECV && m->nmsid == 1 && !strcmp(m->msid[0].stream, "ls") && m->next == 2 && m->ext[0].id == 4,
        "video: sendrecv with the local msid");
    m = &a->media[2];
    CHECK(m->port == 9 && !strcmp(m->proto, "UDP/DTLS/SCTP") && !strcmp(m->fmt_text, "webrtc-datachannel") && m->sctp_port == 5000 && m->max_msg == 65536
            && m->ncodecs == 0 && !m->has_dir,
        "data: proto and fmt exactly as offered (RFC 8829 5.3.1), sctp-port and max-message-size");
    lo[1].stopped = 1;
    CHECK(rtc_jsep_answer(o, &c, lo, &p, a, &err) == 0 && a->media[1].port == 0 && a->nbundle == 2 && !strcmp(a->bundle[1], "2")
            && !strcmp(a->media[1].mid, "1") && a->media[1].ncodecs == 7 && !a->media[1].codecs[0].has_rtpmap,
        "a stopped transceiver: port 0, mid kept, formats echoed, out of the group");
    answer_ok(a, "stopped video");
    lo[1].stopped = 0;
    lo[0].stopped = 1;
    CHECK(rtc_jsep_answer(o, &c, lo, &p, a, &err) == 0 && !a->has_bundle && a->media[0].port == 0 && a->media[1].port == 9,
        "the offerer-tagged m= section rejected: no BUNDLE group (RFC 9143 7.3.1)");
    answer_ok(a, "tagged rejected");
    c.mixed = 0;
    c.data = 0;
    CHECK(rtc_jsep_answer(o, &c, NULL, &p, a, &err) == 0 && !a->allow_mixed && a->media[2].port == 0 && a->media[1].dir == RTC_DIR_SENDRECV
            && a->media[1].nmsid == 0,
        "no data channel support rejects the data m= section; extmap-allow-mixed only when supported");
    answer_ok(a, "no data");
    sdp_del(o);
    sdp_del(a);
}

static void test_answer_rules(void)
{
    rtc_sdp *o = sdp_new(), *a = sdp_new();
    rtc_jsep_caps c;
    rtc_jsep_params p;
    const char* err;
    jsep_setup(&c, &p);
    CHECK(parse_str(o, F_FIREFOX) == 0 && rtc_jsep_answer(o, &c, NULL, &p, a, &err) == 0, "answer to F_FIREFOX");
    answer_ok(a, "F_FIREFOX answer");
    CHECK(a->media[1].port == 9 && !a->media[1].bundle_only && a->nbundle == 2 && a->media[1].rtcp_mux && a->media[1].rtcp_mux_only
            && a->media[1].dir == RTC_DIR_SENDONLY && a->media[1].ncodecs == 3 && find_pt(&a->media[1], 124)->rtx_apt == 120
            && a->media[1].ext[0].id == 3 && a->media[1].ext[1].id == 7,
        "bundle-only accepted into the group: port of the bundle, no bundle-only, rtcp-mux-only answered, recvonly offer gives sendonly");
    CHECK(a->media[0].ncodecs == 2 && a->media[0].codecs[0].pt == 109 && a->media[0].codecs[1].pt == 0 && a->media[0].setup == RTC_SETUP_ACTIVE,
        "Firefox audio: Opus 109 and PCMU 0 from the offer; session-level setup actpass answered active");
    CHECK(parse_str(o, HDR CIN "a=group:BUNDLE v d\r\nm=video 9 UDP/TLS/RTP/SAVPF 98\r\na=rtpmap:98 VP9/90000\r\na=mid:v\r\na=rtcp-mux\r\na=setup:active\r\n"
                          "m=application 0 UDP/DTLS/SCTP webrtc-datachannel\r\na=mid:d\r\na=bundle-only\r\n")
                == 0
            && rtc_jsep_answer(o, &c, NULL, &p, a, &err) == 0 && a->media[0].port == 0 && !a->has_bundle && a->media[1].port == 0
            && !a->media[1].bundle_only,
        "no common codec rejects the tagged m= section, so there is no group and the bundle-only data m= section is rejected too");
    answer_ok(a, "no common codec");
    CHECK(parse_str(o, HDR CIN "m=audio 9 RTP/AVP 0\r\na=setup:active\r\nm=audio 0 RTP/AVP 0\r\nm=text 9 RTP/AVP 98\r\na=rtpmap:98 t140/1000\r\n") == 0
            && rtc_jsep_answer(o, &c, NULL, &p, a, &err) == 0 && a->media[0].port == 9 && !a->media[0].rtcp_mux
            && has_extra(a->media[0].extra, a->media[0].nextra, "a=rtcp:9 IN IP4 0.0.0.0") && a->media[0].setup == RTC_SETUP_PASSIVE && a->media[1].port == 0
            && a->media[2].port == 0 && !a->has_bundle,
        "no rtcp-mux offered: a=rtcp in the answer (RFC 8829 5.3.1); setup active answered passive; an offered port 0 stays rejected; an unknown kind is rejected");
    answer_ok(a, "plain RTP/AVP");
    CHECK(parse_str(o, HDR CIN "m=audio 9 RTP/AVP 0\r\na=setup:passive\r\n") == 0 && rtc_jsep_answer(o, &c, NULL, &p, a, &err) == 0
            && a->media[0].setup == RTC_SETUP_ACTIVE,
        "setup passive answered active");
    CHECK(parse_str(o, HDR CIN "m=application 9 UDP/DTLS/SCTP 5000\r\na=sctpmap:5000 webrtc-datachannel 1024\r\nm=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\n") == 0
            && rtc_jsep_answer(o, &c, NULL, &p, a, &err) == 0 && a->media[0].port == 0 && !strcmp(a->media[0].fmt_text, "5000") && a->media[1].port == 9,
        "a data m= section in the pre-RFC 8841 sctpmap form (fmt 5000) is rejected; the webrtc-datachannel one is accepted");
    p.setup = RTC_SETUP_PASSIVE;
    CHECK(parse_str(o, HDR CIN "m=audio 9 RTP/AVP 0\r\na=setup:actpass\r\n") == 0 && rtc_jsep_answer(o, &c, NULL, &p, a, &err) == 0
            && a->media[0].setup == RTC_SETUP_PASSIVE,
        "actpass answered with the caller's preferred role");
    sdp_del(o);
    sdp_del(a);
}

static uint64_t rng = 0x9E3779B97F4A7C15ull;

static unsigned rnd(unsigned n)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return n ? (unsigned)(rng % n) : 0;
}

static size_t mutate(const char* src, size_t n, char* out, size_t cap)
{
    static const char alpha[] = "\r\n :/=0123456789aAzZ-*;";
    size_t len = n < cap ? n : cap, i, k = 1 + rnd(4);
    memcpy(out, src, len);
    for (i = 0; i < k; i++) {
        unsigned op = rnd(6), at = len ? rnd((unsigned)len) : 0;
        if (op == 0 && len)
            out[at] = (char)(out[at] ^ (1 << rnd(8)));
        else if (op == 1 && len + 1 < cap) {
            memmove(out + at + 1, out + at, len - at);
            out[at] = alpha[rnd(sizeof(alpha) - 1)];
            len++;
        } else if (op == 2 && len) {
            size_t d = 1 + rnd(16);
            if (at + d > len)
                d = len - at;
            memmove(out + at, out + at + d, len - at - d);
            len -= d;
        } else if (op == 3 && len) {
            size_t s = at, e;
            while (s > 0 && out[s - 1] != '\n')
                s--;
            e = at;
            while (e < len && out[e] != '\n')
                e++;
            if (e < len)
                e++;
            if (len + (e - s) < cap) {
                memmove(out + e + (e - s), out + e, len - e);
                memcpy(out + e, out + s, e - s);
                len += e - s;
            }
        } else if (op == 4 && len)
            len = at;
        else if (op == 5 && len > 2) {
            unsigned b = rnd((unsigned)len);
            char t = out[at];
            out[at] = out[b];
            out[b] = t;
        }
    }
    return len;
}

static void test_fuzz(void)
{
    static const char* const fx[] = { F_RFC, F_CHROME, F_FIREFOX, F_LINES };
    static char buf[16384];
    rtc_sdp *a = sdp_new(), *b = sdp_new(), *ans = sdp_new();
    rtc_jsep_caps c;
    rtc_jsep_params p;
    int i, accepted = 0, bad = 0, answered = 0, iters = 1500;
    jsep_setup(&c, &p);
    for (i = 0; i < iters * 4; i++) {
        const char* f = fx[i % 4];
        size_t n = mutate(f, strlen(f), buf, sizeof(buf));
        char* x = (char*)malloc(n ? n : 1);
        const char* err;
        if (!x)
            abort();
        memcpy(x, buf, n);
        if (rtc_sdp_parse(a, x, n, &err) == 0) {
            char *w1 = write_sdp(a), *w2 = NULL;
            accepted++;
            (void)rtc_sdp_check(a, &err);
            if (!w1 || rtc_sdp_parse(b, w1, strlen(w1), &err) != 0 || !sdp_eq(a, b) || !(w2 = write_sdp(b)) || strcmp(w1, w2)) {
                if (bad++ < 3)
                    printf("    fuzz case %d unstable (%s)\n", i, err ? err : "differs");
            }
            if (rtc_jsep_answer(a, &c, NULL, &p, ans, &err) == 0) {
                char* aw = write_sdp(ans);
                if (!aw || rtc_sdp_parse(b, aw, strlen(aw), &err) != 0) {
                    if (bad++ < 3)
                        printf("    fuzz case %d: answer does not re-parse (%s)\n", i, err ? err : "");
                }
                answered++;
                free(aw);
            }
            free(w1);
            free(w2);
        }
        free(x);
    }
    CHECK(bad == 0, "fuzz: %d mutated inputs accepted out of %d, %d answered; every accepted one writes, re-parses and re-writes identically (%d not)",
        accepted, iters * 4, answered, bad);
    CHECK(accepted > iters / 2, "fuzz: the mutations leave enough inputs parseable to exercise the writer (%d)", accepted);
    sdp_del(a);
    sdp_del(b);
    sdp_del(ans);
}

static const struct {
    const char* name;
    unsigned idc;
    const char* iop;
    int profile;
} TABLE5[] = {
    { "CB", 0x42, "x1xx0000", RTC_H264_CONSTRAINED_BASELINE },
    { "CB", 0x4D, "1xxx0000", RTC_H264_CONSTRAINED_BASELINE },
    { "CB", 0x58, "11xx0000", RTC_H264_CONSTRAINED_BASELINE },
    { "B", 0x42, "x0xx0000", RTC_H264_BASELINE },
    { "B", 0x58, "10xx0000", RTC_H264_BASELINE },
    { "M", 0x4D, "0x0x0000", RTC_H264_MAIN },
    { "E", 0x58, "00xx0000", RTC_H264_EXTENDED },
    { "H", 0x64, "00000000", RTC_H264_HIGH },
    { "H10", 0x6E, "00000000", RTC_H264_HIGH10 },
    { "H42", 0x7A, "00000000", RTC_H264_HIGH422 },
    { "H44", 0xF4, "00000000", RTC_H264_HIGH444 },
    { "H10I", 0x6E, "00010000", RTC_H264_HIGH10_INTRA },
    { "H42I", 0x7A, "00010000", RTC_H264_HIGH422_INTRA },
    { "H44I", 0xF4, "00010000", RTC_H264_HIGH444_INTRA },
    { "C44I", 0x2C, "00010000", RTC_H264_CAVLC444_INTRA },
};

static int pattern_has(const char* pat, unsigned iop)
{
    int b;
    for (b = 0; b < 8; b++) {
        unsigned bit = iop >> (7 - b) & 1u;
        if (pat[b] != 'x' && (unsigned)(pat[b] - '0') != bit)
            return 0;
    }
    return 1;
}

static void test_h264_profiles(void)
{
    static const unsigned idcs[] = { 0x42, 0x4D, 0x58, 0x64, 0x6E, 0x7A, 0xF4, 0x2C, 0x53, 0x00 };
    static const struct {
        const char* plid;
        int want;
    } ex[] = {
        { "42e01f", RTC_H264_CONSTRAINED_BASELINE },
        { "4de01f", RTC_H264_CONSTRAINED_BASELINE },
        { "58c01f", RTC_H264_CONSTRAINED_BASELINE },
        { "42c01f", RTC_H264_CONSTRAINED_BASELINE },
        { "42001f", RTC_H264_BASELINE },
        { "42a01f", RTC_H264_BASELINE },
        { "58801f", RTC_H264_BASELINE },
        { "4d001f", RTC_H264_MAIN },
        { "4d401f", RTC_H264_MAIN },
        { "58001f", RTC_H264_EXTENDED },
        { "64001f", RTC_H264_HIGH },
        { "6e001f", RTC_H264_HIGH10 },
        { "7a001f", RTC_H264_HIGH422 },
        { "f4001f", RTC_H264_HIGH444 },
        { "6e101f", RTC_H264_HIGH10_INTRA },
        { "7a101f", RTC_H264_HIGH422_INTRA },
        { "f4101f", RTC_H264_HIGH444_INTRA },
        { "2c101f", RTC_H264_CAVLC444_INTRA },
        { "640c1f", RTC_H264_UNKNOWN },
        { "4d201f", RTC_H264_UNKNOWN },
        { "42e11f", RTC_H264_UNKNOWN },
        { "58401f", RTC_H264_UNKNOWN },
        { "2c001f", RTC_H264_UNKNOWN },
    };
    char f[64];
    size_t i, k;
    int bad = 0, total = 0;
    for (i = 0; i < sizeof(idcs) / sizeof(idcs[0]); i++)
        for (k = 0; k < 256; k++) {
            int want = RTC_H264_UNKNOWN, got;
            size_t r;
            for (r = 0; r < sizeof(TABLE5) / sizeof(TABLE5[0]); r++)
                if (TABLE5[r].idc == idcs[i] && pattern_has(TABLE5[r].iop, (unsigned)k)) {
                    want = TABLE5[r].profile;
                    break;
                }
            snprintf(f, sizeof(f), "packetization-mode=1;profile-level-id=%02x%02x1f", idcs[i], (unsigned)k);
            got = rtc_jsep_h264_profile(f);
            total++;
            if (got != want && bad++ < 5)
                printf("    profile of %s: got %d, RFC 6184 Table 5 says %d\n", f, got, want);
        }
    CHECK(bad == 0, "h264 profile: every profile_idc x profile-iop combination (%d) follows RFC 6184 Table 5 (%d differ)", total, bad);
    for (i = 0; i < sizeof(TABLE5) / sizeof(TABLE5[0]); i++) {
        int n = 0;
        for (k = 0; k < 256; k++)
            n += pattern_has(TABLE5[i].iop, (unsigned)k);
        CHECK(n > 0, "Table 5 row %s %02X %s is reachable", TABLE5[i].name, TABLE5[i].idc, TABLE5[i].iop);
    }
    for (i = 0; i < sizeof(ex) / sizeof(ex[0]); i++) {
        snprintf(f, sizeof(f), "profile-level-id=%s", ex[i].plid);
        CHECK(rtc_jsep_h264_profile(f) == ex[i].want, "h264 profile of %s", ex[i].plid);
    }
    CHECK(rtc_jsep_h264_profile("packetization-mode=1") == RTC_H264_BASELINE, "no profile-level-id is Baseline (RFC 6184 8.1)");
    CHECK(rtc_jsep_h264_profile("profile-level-id=42e0zz") < 0, "malformed profile-level-id");
}

static size_t chrome_video_section(char* out, size_t cap, int mid)
{
    const char *v = strstr(F_CHROME, "m=video"), *e = strstr(F_CHROME, "m=application"), *m = strstr(v, "a=mid:1\r\n");
    return (size_t)snprintf(out, cap, "%.*sa=mid:v%d\r\n%.*s", (int)(m - v), v, mid, (int)(e - m - 9), m + 9);
}

static size_t build_n(char* out, size_t cap, int n)
{
    size_t k = (size_t)snprintf(out, cap, "%s", HDR CIN);
    int i;
    for (i = 0; i < n && k < cap; i++)
        k += chrome_video_section(out + k, cap - k, i);
    return k;
}

static void test_memory(void)
{
    rtc_sdp* s = sdp_new();
    static char buf[1 << 18];
    size_t chrome_bytes, b1 = 0, prev_per = 0, n;
    int sizes[] = { 1, 2, 4, 8, 16, 32, 64 }, i, ok = 1;
    printf("    sizeof(rtc_sdp)=%u sizeof(rtc_sdp_media)=%u sizeof(rtc_sdp_codec)=%u (before W4b: 595136 / 36964 / 300)\n", (unsigned)sizeof(rtc_sdp),
        (unsigned)sizeof(rtc_sdp_media), (unsigned)sizeof(rtc_sdp_codec));
    CHECK(sizeof(rtc_sdp) <= 512 && sizeof(rtc_sdp_media) <= 512, "the fixed part of the model is small; lists live in the arena");
    CHECK(parse_str(s, F_CHROME) == 0, "F_CHROME parses for the memory count");
    chrome_bytes = rtc_sdp_bytes(s);
    printf("    F_CHROME: %u input bytes, %u arena bytes, %u per m= section, sizeof+arena %u\n", (unsigned)strlen(F_CHROME), (unsigned)chrome_bytes,
        (unsigned)(chrome_bytes / 3), (unsigned)(chrome_bytes + sizeof(rtc_sdp)));
    CHECK(chrome_bytes <= 2 * strlen(F_CHROME) + 4096, "the arena for F_CHROME is proportional to its input (%u bytes)", (unsigned)chrome_bytes);
    for (i = 0; i < 1000; i++)
        parse_str(s, F_CHROME);
    CHECK(rtc_sdp_bytes(s) == chrome_bytes, "1000 re-parses into one description keep the arena at the same size (no growth)");
    for (i = 0; i < (int)(sizeof(sizes) / sizeof(sizes[0])); i++) {
        size_t bytes, per;
        n = build_n(buf, sizeof(buf), sizes[i]);
        if (parse_exact(s, buf, n, &(const char*) { NULL }) < 0) {
            ok = 0;
            continue;
        }
        bytes = rtc_sdp_bytes(s);
        per = bytes / (size_t)sizes[i];
        if (!b1)
            b1 = bytes;
        printf("    N=%2d video m= sections: input %6u B, arena %7u B, %5u B per section, %.2f arena/input\n", sizes[i], (unsigned)n, (unsigned)bytes,
            (unsigned)per, (double)bytes / (double)n);
        if (bytes > 2 * n + 4096)
            ok = 0;
        prev_per = per;
    }
    CHECK(ok, "size sweep N=1..64: every arena stays within 2 x input + 4 KiB (last %u B per section)", (unsigned)prev_per);
    sdp_del(s);
}

static int count_sub(const char* s, const char* sub)
{
    int n = 0;
    size_t l = strlen(sub);
    while (s && (s = strstr(s, sub))) {
        n++;
        s += l;
    }
    return n;
}

static char* sections_sdp(int n)
{
    size_t cap = 256 + (size_t)n * 80, k;
    char* d = (char*)malloc(cap);
    int i;
    if (!d)
        abort();
    k = (size_t)snprintf(d, cap, "v=0\r\no=- 1 1 IN IP4 0.0.0.0\r\ns=-\r\nt=0 0\r\nc=IN IP4 0.0.0.0\r\n");
    for (i = 0; i < n; i++)
        k += (size_t)snprintf(d + k, cap - k, "m=audio 9 RTP/AVP 0\r\na=mid:m%d\r\n", i);
    return d;
}

static void test_offer_and_limits(void)
{
    const rtc_jsep_caps* dc = rtc_jsep_default_caps();
    rtc_jsep_params p;
    rtc_jsep_caps c;
    rtc_sdp *o = sdp_new(), *a = sdp_new(), *b = sdp_new();
    rtc_jsep_offer_m ms[5];
    rtc_sdp_msid am[2] = { { "s1", "t1" }, { "s2", "t1" } }, vm[1] = { { "s1", "t2" } };
    const char* rids[2] = { "hi", "lo" };
    const char* err = NULL;
    rtc_jsep_local lo[3];
    rtc_sdp_codec pcmu[1];
    char *w, *big;
    int i;
    jsep_setup(&c, &p);
    p.setup = RTC_SETUP_ACTPASS;
    memset(ms, 0, sizeof(ms));
    ms[0].kind = "audio";
    ms[0].mid = "0";
    ms[0].dir = RTC_DIR_SENDRECV;
    ms[0].codecs = dc->audio;
    ms[0].ncodecs = dc->naudio;
    ms[0].msid = am;
    ms[0].nmsid = 2;
    ms[0].ssrc = 1111;
    ms[0].cname = "cn";
    ms[1].kind = "video";
    ms[1].mid = "1";
    ms[1].dir = RTC_DIR_SENDONLY;
    ms[1].codecs = dc->video;
    ms[1].ncodecs = dc->nvideo;
    ms[1].msid = vm;
    ms[1].nmsid = 1;
    ms[1].ssrc = 2222;
    ms[1].rtx_ssrc = 3333;
    ms[1].cname = "cn";
    ms[1].rids = rids;
    ms[1].nrids = 2;
    ms[2].kind = "audio";
    ms[2].mid = "2";
    ms[2].rejected = 1;
    ms[2].codecs = dc->audio;
    ms[2].ncodecs = dc->naudio;
    ms[3].kind = "video";
    ms[3].mid = "3";
    ms[3].dir = RTC_DIR_RECVONLY;
    ms[3].bundle_only = 1;
    ms[3].codecs = dc->video;
    ms[3].ncodecs = dc->nvideo;
    ms[4].kind = "application";
    ms[4].mid = "4";
    CHECK(rtc_jsep_offer(ms, 5, dc, &p, o, &err) == 0, "rtc_jsep_offer builds audio, simulcast video, rejected, bundle-only and data (%s)", err ? err : "");
    w = write_sdp(o);
    CHECK(w && parse_exact(b, w, strlen(w), &err) == 0 && rtc_sdp_check(b, &err) == 0, "the written offer parses and passes the strict check (%s)", err ? err : "");
    CHECK(o->has_bundle && o->nbundle == 4 && !strcmp(o->bundle[0], "0") && !strcmp(o->bundle[2], "3") && !strcmp(o->bundle[3], "4"),
        "BUNDLE lists every live mid, bundle-only included, the rejected one not");
    CHECK(o->media[2].port == 0 && o->media[2].dir == RTC_DIR_INACTIVE && o->media[2].ncodecs == dc->naudio && o->media[2].next == 0,
        "a rejected m= section: port 0, inactive, payload types only");
    CHECK(o->media[3].port == 0 && o->media[3].bundle_only && o->media[3].dir == RTC_DIR_RECVONLY && o->media[3].nssrc == 0,
        "bundle-only: port 0 with a=bundle-only; recvonly has no ssrc");
    CHECK(o->media[0].nmsid == 2 && !strcmp(o->media[0].msid[1].stream, "s2") && o->media[0].nssrc == 1 && o->media[0].ssrcs[0].id == 1111
            && o->media[0].ngroups == 0 && o->media[0].next == 4,
        "audio: every msid, one ssrc, no FID group, the audio header extensions (mid, transport-cc, ssrc- and csrc-audio-level)");
    CHECK(o->media[1].ngroups == 1 && !strcmp(o->media[1].groups[0].semantics, "FID") && o->media[1].groups[0].ssrc[1] == 3333 && o->media[1].nssrc == 2
            && o->media[1].dir == RTC_DIR_SENDONLY && o->media[1].next == 4,
        "video: FID group for rtx, two ssrcs, the video header extensions");
    CHECK(w && strstr(w, "a=rid:hi send\r\n") && strstr(w, "a=rid:lo send\r\n") && strstr(w, "a=simulcast:send hi;lo\r\n"), "video: rid and simulcast lines");
    CHECK(w && strstr(w, "a=setup:actpass\r\n") && count_sub(w, "a=ice-ufrag:LoCa") == 4 && strstr(w, "a=sctp-port:5000"), "transport attributes in every m= section except the rejected one");
    free(w);
    ms[1].nrids = 1;
    CHECK(rtc_jsep_offer(ms, 5, dc, &p, o, &err) == 0 && (w = write_sdp(o)) && !strstr(w, "a=simulcast"), "one rid: no simulcast line");
    free(w);
    o->lim.media = 4;
    CHECK(rtc_jsep_offer(ms, 5, dc, &p, o, &err) < 0, "the offer builder honours lim.media");
    rtc_sdp_limits_default(&o->lim);
    big = sections_sdp(80);
    CHECK(parse_str(a, big) < 0, "80 m= sections exceed the default limit");
    a->lim.media = 100;
    CHECK(parse_str(a, big) == 0 && a->nmedia == 80, "a raised lim.media accepts 80 m= sections");
    CHECK(parse_str(a, big) == 0 && a->lim.media == 100, "the limits survive rtc_sdp_free and a reparse");
    b->lim.media = 3;
    CHECK(rtc_jsep_answer(a, &c, NULL, &p, b, &err) == 0 && b->nmedia == 80 && b->lim.media == 100, "the answer inherits the offer's limits");
    a->lim.media = 79;
    CHECK(parse_str(a, big) < 0, "79 is one under");
    free(big);
    a->lim.codecs = 1;
    CHECK(parse_str(a, HDR CIN "m=audio 9 RTP/AVP 0 8\r\n") < 0 && parse_str(a, HDR CIN "m=audio 9 RTP/AVP 0\r\n") == 0, "lim.codecs at the cap and one over");
    a->lim.group_ssrcs = 2;
    CHECK(parse_str(a, HDR CIN "m=audio 9 RTP/AVP 0\r\na=ssrc-group:FID 1 2\r\n") == 0 && parse_str(a, HDR CIN "m=audio 9 RTP/AVP 0\r\na=ssrc-group:FID 1 2 3\r\n") < 0,
        "lim.group_ssrcs at the cap and one over");
    a->lim.group_ssrcs = 40;
    {
        char g[512];
        size_t k = (size_t)snprintf(g, sizeof(g), HDR CIN "m=audio 9 RTP/AVP 0\r\na=ssrc-group:FEC-FR");
        for (i = 0; i < 40; i++)
            k += (size_t)snprintf(g + k, sizeof(g) - k, " %d", i + 1);
        snprintf(g + k, sizeof(g) - k, "\r\n");
        CHECK(parse_str(a, g) == 0 && a->media[0].groups[0].n == 40 && a->media[0].groups[0].ssrc[39] == 40, "a raised group cap keeps all 40 ssrcs (no fixed array)");
    }
    rtc_sdp_limits_default(&a->lim);
    CHECK(parse_str(a, F_CHROME) == 0, "F_CHROME for the answer preference checks");
    memset(lo, 0, sizeof(lo));
    pcmu[0] = dc->audio[1];
    lo[0].dir = RTC_DIR_SENDRECV;
    lo[0].codecs = pcmu;
    lo[0].ncodecs = 1;
    lo[0].msids = am;
    lo[0].nmsid = 2;
    lo[0].msid = am[0];
    lo[0].ssrc = 4444;
    lo[0].cname = "cn";
    CHECK(rtc_jsep_answer(a, &c, lo, &p, b, &err) == 0 && b->media[0].ncodecs == 1 && b->media[0].codecs[0].pt == 0,
        "answer codec preferences filter the offer's codecs");
    CHECK(b->media[0].nmsid == 2 && !strcmp(b->media[0].msid[1].stream, "s2") && b->media[0].nssrc == 1 && b->media[0].ssrcs[0].id == 4444
            && !strcmp(b->media[0].ssrcs[0].cname, "cn"),
        "the answer carries every local msid and the local ssrc");
    answer_ok(b, "preferences answer");
    sdp_del(o);
    sdp_del(a);
    sdp_del(b);
}

static const char F_SIM[] = "v=0\r\n"
                            "o=- 5 1 IN IP4 0.0.0.0\r\n"
                            "s=-\r\n"
                            "t=0 0\r\n"
                            "a=group:BUNDLE 0\r\n"
                            "m=video 9 UDP/TLS/RTP/SAVPF 96 97\r\n"
                            "c=IN IP4 0.0.0.0\r\n"
                            "a=ice-ufrag:SiMu\r\n"
                            "a=ice-pwd:aaaaaaaaaaaaaaaaaaaaaaaa\r\n"
                            "a=fingerprint:sha-256 " FP "\r\n"
                            "a=setup:actpass\r\n"
                            "a=mid:0\r\n"
                            "a=sendonly\r\n"
                            "a=rtcp-mux\r\n"
                            "a=extmap:3 urn:ietf:params:rtp-hdrext:sdes:rtp-stream-id\r\n"
                            "a=rtpmap:96 VP8/90000\r\n"
                            "a=rtpmap:97 rtx/90000\r\n"
                            "a=fmtp:97 apt=96\r\n"
                            "a=rid:h send\r\n"
                            "a=rid:m send max-width=640\r\n"
                            "a=rid:x recv\r\n"
                            "a=simulcast:send h;~m,zz;x\r\n";

static void test_w5b_jsep(void)
{
    const rtc_jsep_caps* dc = rtc_jsep_default_caps();
    rtc_sdp *o = sdp_new(), *a = sdp_new();
    rtc_jsep_params p;
    rtc_jsep_caps c;
    rtc_jsep_local lo[1];
    rtc_jsep_offer_m ms[1];
    uint16_t ids[8] = { 9, 0, 7, 0, 0, 0, 0, 0 };
    const char* err = NULL;
    char* w;
    int i, te = 0;
    jsep_setup(&c, &p);
    memset(lo, 0, sizeof(lo));
    lo[0].dir = RTC_DIR_RECVONLY;
    CHECK(parse_str(o, F_SIM) == 0 && rtc_jsep_answer(o, dc, lo, &p, a, &err) == 0, "answer to a simulcast offer (%s)", err ? err : "");
    w = write_sdp(a);
    CHECK(w && strstr(w, "a=rid:h recv\r\n") && strstr(w, "a=rid:m recv\r\n") && strstr(w, "a=simulcast:recv h;~m\r\n"),
        "RFC 8853 answer: a=rid recv for each offered send rid kept, the simulcast list mirrored with its paused marker");
    CHECK(w && !strstr(w, "rid:zz") && !strstr(w, "rid:x") && !strstr(w, ";x"), "a rid without an a=rid send line (zz) and a recv rid (x) are dropped");
    CHECK(w && strstr(w, "a=extmap:3 urn:ietf:params:rtp-hdrext:sdes:rtp-stream-id\r\n"), "the rid header extension keeps the offer's id");
    free(w);
    lo[0].dir = RTC_DIR_INACTIVE;
    CHECK(rtc_jsep_answer(o, dc, lo, &p, a, &err) == 0 && (w = write_sdp(a)) && !strstr(w, "a=simulcast") && !strstr(w, "a=rid"),
        "an answer that does not receive carries no simulcast");
    free(w);
    for (i = 0; i < dc->naudio; i++)
        if (!strcmp(dc->audio[i].name, "telephone-event") && !strcmp(dc->audio[i].fmtp, "0-15"))
            te |= dc->audio[i].clock == 48000 ? 1 : dc->audio[i].clock == 8000 ? 2 : 0;
    CHECK(te == 3, "default audio caps carry RFC 4733 telephone-event at 48000 and 8000 Hz");
    memset(ms, 0, sizeof(ms));
    ms[0].kind = "audio";
    ms[0].mid = "0";
    ms[0].dir = RTC_DIR_SENDRECV;
    ms[0].codecs = dc->audio;
    ms[0].ncodecs = dc->naudio;
    ms[0].ext_ids = ids;
    CHECK(rtc_jsep_offer(ms, 1, dc, &p, o, &err) == 0 && (w = write_sdp(o)), "offer with per-offer extmap ids");
    CHECK(w && strstr(w, "a=extmap:9 urn:ietf:params:rtp-hdrext:sdes:mid\r\n") && strstr(w, "a=extmap:2 http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01\r\n")
            && strstr(w, "a=extmap:7 urn:ietf:params:rtp-hdrext:ssrc-audio-level\r\n"),
        "ext_ids override the default index+1 ids (0 keeps the default)");
    CHECK(w && strstr(w, "a=rtpmap:110 telephone-event/48000\r\n") && strstr(w, "a=fmtp:126 0-15\r\n"), "telephone-event is offered with its 0-15 events");
    free(w);
    sdp_del(o);
    sdp_del(a);
}

int main(void)
{
    test_fixtures();
    test_malformed();
    test_dir_table();
    test_codec_table();
    test_h264_profiles();
    test_h264_level();
    test_answer_browser();
    test_answer_rules();
    test_offer_and_limits();
    test_w5b_jsep();
    test_fuzz();
    test_memory();
    printf("test_rtc_sdp: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
