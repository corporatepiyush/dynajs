/*
 * webrtc_media_harness.c -- the W6 media origin. The product has no JS way to put RTP on the wire before encoders (P3):
 * Encoded Transform is strict (no frame constructor) and the owner rule is "media origin for tests = C harness, no
 * hidden JS hooks". This program is a dynajs runner (same runtime set-up as dyna-cli.c: std/os, native modules,
 * module loader, scriptArgs) that adds one global, rtpHarness, defined here and only here. Its functions call the C
 * media kernel (src/modules/webrtc/webrtc-rtp.c) the way an encoder will:
 *   send(sender, {count, pt, ts, tsStep, marker, csrc, level, clevels, bytes}) -> packets sent (RTP + SRTP + UDP), 0 when
 *     sender.track has ended (an encoder gets no frames from an ended track);
 *     level is the RFC 6464 byte (bit 7 = V flag, bits 0-6 = -dBov), omitted when absent
 *   bye(sender)                       RTCP BYE for the sender's SSRC (SRTCP)
 *   sr(sender)                        RTCP sender report (RFC 3550 6.4.1, reduced size per a=rtcp-rsize) for the
 *                                     sender's SSRC (SRTCP)
 *   impair(sender, {loss, dup, reorder, seed})   wire impairment on that pc's media output, in parts per million
 *   age(receiver, ms)                 move the receiver's last-activity time ms into the past, then run the pc's
 *                                     timer now (the RFC 3550 6.3.5 inactivity timeout without a 25 s wait)
 *   raw(sender, Uint8Array)           unprotected bytes to the selected pair (forged / unauthenticated packets)
 *   stats(receiver)                   RFC 3550 receiver state {pkts, bytes, extMax, jitter, lost, ssrc,
 *                                     tePkts, teEnds, teMarkers, teSegs, teDur, teFirstDur, teEvent, teVol} (RFC 4733 telephone-event)
 *   counts(senderOrReceiver)          the pc's media counters (drop reasons, impairment actions)
 *   bytes(senderOrReceiver)           bytes held by the pc's media state (rule 11)
 *   relayBytes(track)                 bytes of the track's relay hub (its sink list), 0 for a local track
 *   rss()                             peak RSS in bytes (getrusage)
 *   cpu()                             process user+system CPU in microseconds (getrusage)
 *   oracle(sender, pt, ssrc)          one RTP packet protected with an SRTP context keyed here, independently of the
 *                                     product, from the RFC 5764 4.2 split of the exported block for this side's DTLS
 *                                     role (client: client_write key/salt); the peer must accept it (keying oracle)
 * Usage: webrtc_media_harness FILE.js [--case=PAT ...]; exit status 1 when the script throws or the loop fails.
 */
#include "dynajs.h"
#include "dyna-libc.h"
#include "dyna-nat.h"
#include "modules/webrtc/webrtc-js.h"
#include "modules/webrtc/webrtc-srtp.h"
#include "core/rtc/rtc-rtp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>

static wrt_pc* side(JSContext* ctx, JSValueConst v, uint32_t* id)
{
    wrt_pc* pc = wjs_side_route(v, id);
    if (!pc)
        JS_ThrowTypeError(ctx, "rtpHarness: expected an associated RTCRtpSender or RTCRtpReceiver");
    return pc;
}

static int opt_num(JSContext* ctx, JSValueConst o, const char* k, double dflt, double* out)
{
    JSValue v = JS_GetPropertyStr(ctx, o, k);
    int r = 0;
    *out = dflt;
    if (!JS_IsUndefined(v))
        r = JS_ToFloat64(ctx, out, v);
    JS_FreeValue(ctx, v);
    return r;
}

static int opt_u32s(JSContext* ctx, JSValueConst o, const char* k, uint32_t* out, unsigned max, unsigned* n)
{
    JSValue v = JS_GetPropertyStr(ctx, o, k), lv;
    int64_t len = 0, i;
    *n = 0;
    if (JS_IsUndefined(v))
        return 0;
    lv = JS_GetPropertyStr(ctx, v, "length");
    if (JS_ToInt64(ctx, &len, lv)) {
        JS_FreeValue(ctx, lv);
        JS_FreeValue(ctx, v);
        return -1;
    }
    JS_FreeValue(ctx, lv);
    for (i = 0; i < len && *n < max; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, v, (uint32_t)i);
        uint32_t x = 0;
        int bad = JS_ToUint32(ctx, &x, e);
        JS_FreeValue(ctx, e);
        if (bad) {
            JS_FreeValue(ctx, v);
            return -1;
        }
        out[(*n)++] = x;
    }
    JS_FreeValue(ctx, v);
    return 0;
}

static int track_ended(JSContext* ctx, JSValueConst sender)
{
    JSValue tr = JS_GetPropertyStr(ctx, sender, "track");
    JSValue st;
    const char* s;
    int r = 0;
    if (!JS_IsObject(tr)) {
        JS_FreeValue(ctx, tr);
        return 0;
    }
    st = JS_GetPropertyStr(ctx, tr, "readyState");
    s = JS_ToCString(ctx, st);
    if (!s)
        JS_FreeValue(ctx, JS_GetException(ctx));
    r = s && !strcmp(s, "ended");
    JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, st);
    JS_FreeValue(ctx, tr);
    return r;
}

static JSValue h_send(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    uint32_t id, csrc[16], cl[16];
    uint8_t clevels[16], payload[1200];
    unsigned cc = 0, ncl = 0, i;
    double count, pt, ts, step, marker, level, bytes;
    wrt_pc* pc = side(ctx, argv[0], &id);
    JSValueConst o = argc > 1 ? argv[1] : JS_UNDEFINED;
    int sent = 0, k;
    (void)this_val;
    if (!pc)
        return JS_EXCEPTION;
    if (track_ended(ctx, argv[0]))
        return JS_NewInt32(ctx, 0);
    if (opt_num(ctx, o, "count", 1, &count) || opt_num(ctx, o, "pt", -1, &pt) || opt_num(ctx, o, "ts", 0, &ts)
        || opt_num(ctx, o, "tsStep", 960, &step) || opt_num(ctx, o, "marker", 0, &marker) || opt_num(ctx, o, "level", -1, &level)
        || opt_num(ctx, o, "bytes", 100, &bytes))
        return JS_EXCEPTION;
    opt_u32s(ctx, o, "csrc", csrc, 15, &cc);
    opt_u32s(ctx, o, "clevels", cl, 15, &ncl);
    for (i = 0; i < ncl; i++)
        clevels[i] = (uint8_t)cl[i];
    if (bytes > sizeof(payload))
        bytes = sizeof(payload);
    for (i = 0; i < (unsigned)bytes; i++)
        payload[i] = (uint8_t)(i * 7 + 1);
    for (k = 0; k < (int)count; k++) {
        wrt_rtp_out out;
        int r;
        memset(&out, 0, sizeof(out));
        out.pt = pt < 0 ? WRT_PT_DEFAULT : (uint32_t)pt;
        out.ts = (uint32_t)((uint64_t)ts + (uint64_t)step * (uint64_t)k);
        out.marker = marker != 0;
        out.csrc = cc ? csrc : NULL;
        out.cc = cc;
        out.level = (int)level;
        out.clevels = ncl >= cc && cc ? clevels : NULL;
        out.payload = payload;
        out.len = (size_t)bytes;
        r = wrt_media_send(pc, id, &out);
        if (r < 0)
            break;
        sent++;
    }
    return JS_NewInt32(ctx, sent);
}

static JSValue h_bye(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    uint32_t id;
    wrt_pc* pc = side(ctx, argv[0], &id);
    (void)this_val;
    (void)argc;
    if (!pc)
        return JS_EXCEPTION;
    return JS_NewBool(ctx, wrt_media_send_bye(pc, id) == 0);
}

static JSValue h_sr(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    uint32_t id;
    wrt_pc* pc = side(ctx, argv[0], &id);
    (void)this_val;
    (void)argc;
    if (!pc)
        return JS_EXCEPTION;
    return JS_NewBool(ctx, wrt_media_send_sr(pc, id, wrt_now_us()) == 0);
}

static JSValue h_relay_bytes(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    wjs_track* t = argc > 0 ? wjs_track_get(argv[0]) : NULL;
    (void)this_val;
    if (!t)
        return JS_ThrowTypeError(ctx, "rtpHarness.relayBytes: expected a MediaStreamTrack");
    return JS_NewFloat64(ctx, (double)wrt_relay_bytes(t->relay));
}

static JSValue h_impair(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    uint32_t id;
    double loss, dup, reo, seed;
    wrt_pc* pc = side(ctx, argv[0], &id);
    JSValueConst o = argc > 1 ? argv[1] : JS_UNDEFINED;
    (void)this_val;
    if (!pc)
        return JS_EXCEPTION;
    if (opt_num(ctx, o, "loss", 0, &loss) || opt_num(ctx, o, "dup", 0, &dup) || opt_num(ctx, o, "reorder", 0, &reo)
        || opt_num(ctx, o, "seed", 1, &seed))
        return JS_EXCEPTION;
    wrt_media_impair(pc, (uint32_t)loss, (uint32_t)dup, (uint32_t)reo, (uint64_t)seed);
    return JS_UNDEFINED;
}

static JSValue h_age(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    uint32_t id;
    double ms = 0;
    wrt_pc* pc = side(ctx, argv[0], &id);
    (void)this_val;
    if (!pc)
        return JS_EXCEPTION;
    if (argc > 1 && JS_ToFloat64(ctx, &ms, argv[1]))
        return JS_EXCEPTION;
    wrt_media_rx_age(pc, id, (uint64_t)(ms * 1000.0));
    wjs_side_rearm(ctx, argv[0]);
    return JS_UNDEFINED;
}

static JSValue h_raw(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    uint32_t id;
    size_t n = 0;
    uint8_t* b;
    wrt_pc* pc = side(ctx, argv[0], &id);
    (void)this_val;
    if (!pc || argc < 2)
        return JS_EXCEPTION;
    {
        size_t off = 0, el = 0, bl = 0;
        JSValue ab = JS_GetTypedArrayBuffer(ctx, argv[1], &off, &n, &el);
        if (JS_IsException(ab))
            return ab;
        b = JS_GetArrayBuffer(ctx, &bl, ab);
        JS_FreeValue(ctx, ab);
        if (!b || off + n > bl)
            return JS_ThrowTypeError(ctx, "rtpHarness.raw: expected a Uint8Array");
        b += off;
    }
    wrt_pc_send_dtls(pc, b, n);
    return JS_UNDEFINED;
}

static JSValue h_stats(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    uint32_t id;
    wrt_rx_stats s;
    JSValue o;
    wrt_pc* pc = side(ctx, argv[0], &id);
    (void)this_val;
    (void)argc;
    if (!pc)
        return JS_EXCEPTION;
    if (wrt_media_rx_stats(pc, id, &s) < 0)
        return JS_NULL;
    o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "pkts", JS_NewFloat64(ctx, (double)s.pkts));
    JS_SetPropertyStr(ctx, o, "bytes", JS_NewFloat64(ctx, (double)s.bytes));
    JS_SetPropertyStr(ctx, o, "extMax", JS_NewUint32(ctx, s.ext_max));
    JS_SetPropertyStr(ctx, o, "jitter", JS_NewUint32(ctx, s.jitter));
    JS_SetPropertyStr(ctx, o, "lost", JS_NewInt32(ctx, s.lost));
    JS_SetPropertyStr(ctx, o, "ssrc", JS_NewUint32(ctx, s.ssrc));
    JS_SetPropertyStr(ctx, o, "tePkts", JS_NewUint32(ctx, s.te_pkts));
    JS_SetPropertyStr(ctx, o, "teEnds", JS_NewUint32(ctx, s.te_ends));
    JS_SetPropertyStr(ctx, o, "teMarkers", JS_NewUint32(ctx, s.te_markers));
    JS_SetPropertyStr(ctx, o, "teSegs", JS_NewUint32(ctx, s.te_segs));
    JS_SetPropertyStr(ctx, o, "teDur", JS_NewUint32(ctx, s.te_last_dur));
    JS_SetPropertyStr(ctx, o, "teFirstDur", JS_NewUint32(ctx, s.te_first_dur));
    JS_SetPropertyStr(ctx, o, "teEvent", JS_NewUint32(ctx, s.te_last_event));
    JS_SetPropertyStr(ctx, o, "teVol", JS_NewUint32(ctx, s.te_last_vol));
    return o;
}

static JSValue h_counts(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    uint32_t id;
    wrt_media_counts c;
    JSValue o;
    wrt_pc* pc = side(ctx, argv[0], &id);
    (void)this_val;
    (void)argc;
    if (!pc)
        return JS_EXCEPTION;
    wrt_media_counts_get(pc, &c);
    o = JS_NewObject(ctx);
#define C(k, f) JS_SetPropertyStr(ctx, o, k, JS_NewFloat64(ctx, (double)c.f))
    C("rtpIn", rtp_in);
    C("rtcpIn", rtcp_in);
    C("srtpAuth", srtp_auth);
    C("srtpReplay", srtp_replay);
    C("srtpOther", srtp_other);
    C("unroutable", unroutable);
    C("unkeyed", unkeyed);
    C("oversize", oversize);
    C("rtcpBad", rtcp_bad);
    C("byes", byes);
    C("ptDrop", pt_drop);
    C("rtpOut", rtp_out);
    C("rtcpOut", rtcp_out);
    C("sendFail", send_fail);
    C("drop", imp_drop);
    C("dup", imp_dup);
    C("reorder", imp_reorder);
    C("relayOut", relay_out);
    C("relaySkip", relay_skip);
    C("dtmfOut", dtmf_out);
    C("timeouts", timeouts);
#undef C
    JS_SetPropertyStr(ctx, o, "rtpDropped", JS_NewFloat64(ctx, (double)pc->rtp_dropped));
    return o;
}

static JSValue h_bytes(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    uint32_t id;
    wrt_pc* pc = side(ctx, argv[0], &id);
    (void)this_val;
    (void)argc;
    if (!pc)
        return JS_EXCEPTION;
    return JS_NewFloat64(ctx, (double)wrt_media_bytes(pc));
}

static JSValue h_rss(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    struct rusage ru;
    (void)this_val;
    (void)argc;
    (void)argv;
    getrusage(RUSAGE_SELF, &ru);
#ifdef __APPLE__
    return JS_NewFloat64(ctx, (double)ru.ru_maxrss);
#else
    return JS_NewFloat64(ctx, (double)ru.ru_maxrss * 1024.0);
#endif
}

static JSValue h_cpu(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    struct rusage ru;
    (void)this_val;
    (void)argc;
    (void)argv;
    getrusage(RUSAGE_SELF, &ru);
    return JS_NewFloat64(ctx, (double)(ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) * 1e6 + (double)(ru.ru_utime.tv_usec + ru.ru_stime.tv_usec));
}

static JSValue h_oracle(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    uint32_t id, pt = 0, ssrc = 0;
    size_t kl = 0, sl = 0, n, ol = 0;
    const uint8_t *key, *salt;
    uint8_t pkt[256], pay[20];
    wrt_srtp s;
    int client, r;
    wrt_pc* pc = side(ctx, argv[0], &id);
    (void)this_val;
    if (!pc || argc < 3 || JS_ToUint32(ctx, &pt, argv[1]) || JS_ToUint32(ctx, &ssrc, argv[2]))
        return JS_EXCEPTION;
    if (!pc->dtls.keys_len || wrt_srtp_profile_lengths(pc->dtls.srtp_profile, &kl, &sl) < 0)
        return JS_ThrowTypeError(ctx, "rtpHarness.oracle: no DTLS-SRTP keys yet");
    client = !pc->dtls.is_server;
    key = pc->dtls.keys + (client ? 0 : kl);
    salt = pc->dtls.keys + 2 * kl + (client ? 0 : sl);
    memset(&s, 0, sizeof(s));
    if (wrt_srtp_init(&s, pc->dtls.srtp_profile, key, salt) < 0)
        return JS_ThrowTypeError(ctx, "rtpHarness.oracle: wrt_srtp_init failed");
    memset(pay, 0x5a, sizeof(pay));
    n = rtc_rtp_write(pkt, sizeof(pkt), (uint8_t)pt, 0, 1000, 5, ssrc, NULL, 0, NULL, 0, pay, sizeof(pay));
    r = n ? wrt_srtp_protect(&s, pkt, n, sizeof(pkt), &ol) : -1;
    wrt_srtp_free(&s);
    if (r != WRT_SRTP_OK)
        return JS_ThrowTypeError(ctx, "rtpHarness.oracle: protect failed");
    wrt_pc_send_dtls(pc, pkt, ol);
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry h_funcs[] = {
    JS_CFUNC_DEF("send", 2, h_send),
    JS_CFUNC_DEF("bye", 1, h_bye),
    JS_CFUNC_DEF("impair", 2, h_impair),
    JS_CFUNC_DEF("relayBytes", 1, h_relay_bytes),
    JS_CFUNC_DEF("sr", 1, h_sr),
    JS_CFUNC_DEF("age", 2, h_age),
    JS_CFUNC_DEF("raw", 2, h_raw),
    JS_CFUNC_DEF("stats", 1, h_stats),
    JS_CFUNC_DEF("counts", 1, h_counts),
    JS_CFUNC_DEF("bytes", 1, h_bytes),
    JS_CFUNC_DEF("rss", 0, h_rss),
    JS_CFUNC_DEF("cpu", 0, h_cpu),
    JS_CFUNC_DEF("oracle", 3, h_oracle),
};

static JSContext* new_ctx(JSRuntime* rt)
{
    JSContext* ctx = JS_NewContext(rt);
    if (!ctx)
        return NULL;
    js_init_module_std(ctx, "std");
    js_init_module_os(ctx, "os");
    js_nat_init_all(ctx);
    return ctx;
}

static int eval_module(JSContext* ctx, const char* src, size_t n, const char* name)
{
    JSValue v = JS_Eval(ctx, src, n, name, JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    if (!JS_IsException(v)) {
        js_module_set_import_meta(ctx, v, 1, 1);
        v = JS_EvalFunction(ctx, v);
    }
    v = js_std_await(ctx, v);
    if (JS_IsException(v)) {
        js_std_dump_error(ctx);
        return -1;
    }
    JS_FreeValue(ctx, v);
    return 0;
}

int main(int argc, char** argv)
{
    JSRuntime* rt;
    JSContext* ctx;
    JSValue g, h;
    uint8_t* buf;
    size_t len = 0;
    int rc = 0;
    const char* pre = "import * as std from 'std';\nimport * as os from 'os';\nglobalThis.std = std;\nglobalThis.os = os;\n";
    if (argc < 2) {
        fprintf(stderr, "usage: %s FILE.js [args]\n", argv[0]);
        return 2;
    }
    rt = JS_NewRuntime();
    js_std_set_worker_new_context_func(new_ctx);
    js_std_init_handlers(rt);
    ctx = new_ctx(rt);
    if (!ctx)
        return 2;
    JS_SetModuleLoaderFunc2(rt, NULL, js_module_loader, js_module_check_attributes, NULL);
    JS_SetHostPromiseRejectionTracker(rt, js_std_promise_rejection_tracker, NULL);
    js_std_add_helpers(ctx, argc - 1, argv + 1);
    g = JS_GetGlobalObject(ctx);
    h = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, h, h_funcs, (int)(sizeof(h_funcs) / sizeof(h_funcs[0])));
    JS_SetPropertyStr(ctx, g, "rtpHarness", h);
    JS_FreeValue(ctx, g);
    if (eval_module(ctx, pre, strlen(pre), "<harness>") < 0)
        rc = 1;
    buf = rc ? NULL : js_load_file(ctx, &len, argv[1]);
    if (!rc && !buf) {
        fprintf(stderr, "cannot read %s\n", argv[1]);
        rc = 1;
    }
    if (!rc && eval_module(ctx, (const char*)buf, len, argv[1]) < 0)
        rc = 1;
    if (buf)
        js_free(ctx, buf);
    if (!rc && js_std_loop(ctx))
        rc = 1;
    js_std_free_handlers(rt);
    JS_FreeContext(ctx);
    JS_SetExitTeardown(rt, 1);
    JS_FreeRuntime(rt);
    return rc;
}
