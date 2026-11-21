/* tests/net/test_frame_decoder.c -- C4 + C5 (NET-FRAME-DECODERS): src/core/dyn-frame.c, the Netty LengthField / Delimiter /
 * FixedLength stream decoders bound to dyna:net TCPConn.
 * Owner rule (11-26): the DEFAULT is full Netty 4.1 semantics; dyna-specific behaviour is opt-in (stopOnError,
 * rejectUnsafeLength, holdOverlapping, resumeAfterError). So there are two oracles:
 *  1. NETTY: a literal per-read port of Netty 4.1 (ByteToMessageDecoder cumulation + callDecode loop that stops at a thrown
 *     exception + channelInputClosed, LengthFieldBasedFrameDecoder with failFast/discard, DelimiterBasedFrameDecoder with
 *     partialDelimiterLength, LineBasedFrameDecoder for exactly {"\r\n","\n"}), written from the Netty sources as plain
 *     index arithmetic over one contiguous array. The decoder must emit the same events (frames, error codes AND the
 *     lengths Netty puts in its messages, in order) for the SAME read boundaries, on every segmentation class. Covers
 *     failFast true/false, stopOnError (the reference stops at the first error), rejectUnsafeLength, and resumeAfterError
 *     (owner 11-26: the same port with callDecode's stop at a thrown error removed; default false = Netty's stall).
 *  2. INVARIANT: the brute-force whole-stream reference from C4. The opt-in combination holdOverlapping + stopOnError
 *     (delimiter), stopOnError + rejectUnsafeLength (length), and FixedLength must give the same frames and final error
 *     for EVERY segmentation (one feed, 1-byte feeds, random cuts, every 2-way split, sampled 3-way splits).
 * holdOverlapping WITHOUT stopOnError is not invariant and is not claimed to be: it keeps Netty's error handling, where a
 * thrown error ends the decoding of the current read (the rest waits for the next read, and at end of input a second thrown
 * error drops the remainder, as channelInputClosed does), so the events after an error depend on the read count. (Found by
 * a self-consistency oracle that was removed for that reason: one feed {\r\n,\n} max 4 reported 2 errors, 1-byte feeds 3.)
 * Explicit "default vs option" cases pin, with exact feeds (loopback cannot pin read boundaries), the places where Netty
 * depends on segmentation or on Java's long arithmetic: overlapping delimiters across reads, decoding resuming after an
 * error only with the next read (Netty's callDecode stops at a thrown exception), failFast timing, width-8 signed lengths,
 * the partial-delimiter overflow rule, LineBasedFrameDecoder's own rules and messages, stopOnError stopping at detection.
 * Netty's 4.1 DelimiterBasedFrameDecoder already holds a partial delimiter tail out of the maxFrameLength count, which
 * makes its overflow check segmentation-safe: C4's "wait for maxFrameLength + longest delimiter" rule is therefore not an
 * option any more (Netty's rule is the default and is checked by oracle 1). Each received byte is copied at most once
 * (dyn_frame_copied <= bytes fed; == the tail when the stream arrives in one feed and nothing is fatal).
 * Seeds (tests/lib/dyn-fuzzkit.h): the random streams, configs and cuts come from DYN_FUZZ_SEED (printed first; argv[2]
 * still overrides), loop counts scale with DYN_FUZZ_ITERS (50 = deep). src/fuzz/fuzz_net.c includes this file and drives
 * compare_one with libFuzzer-chosen configs, streams and cuts. */
#include "dyn-frame.h"
#include "../lib/dyn-fuzzkit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail, g_checks;

#define CHECK(c, ...)                         \
    do {                                      \
        g_checks++;                           \
        if (!(c)) {                           \
            g_fail++;                         \
            printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__);              \
            printf("\n");                     \
        }                                     \
    } while (0)

static uint64_t g_rng;
static uint64_t rnd(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return g_rng;
}
static uint64_t rnd_below(uint64_t n) { return n ? rnd() % n : 0; }

typedef struct {
    uint8_t* bytes;
    size_t nbytes, capb;
    size_t* lens;
    int* codes;
    uint64_t* vals;
    size_t nev, cape;
} rec_t;

static void rec_reset(rec_t* r) { r->nbytes = 0; r->nev = 0; }
static void rec_free(rec_t* r) { free(r->bytes); free(r->lens); free(r->codes); free(r->vals); memset(r, 0, sizeof(*r)); }
static void rec_push(rec_t* r, int code, const uint8_t* p, uint64_t n)
{
    size_t len = code ? 0 : (size_t)n;
    if (r->nbytes + len > r->capb) {
        r->capb = (r->nbytes + len) * 2 + 64;
        r->bytes = (uint8_t*)realloc(r->bytes, r->capb);
    }
    if (len)
        memcpy(r->bytes + r->nbytes, p, len);
    r->nbytes += len;
    if (r->nev == r->cape) {
        r->cape = r->cape * 2 + 16;
        r->lens = (size_t*)realloc(r->lens, r->cape * sizeof(size_t));
        r->codes = (int*)realloc(r->codes, r->cape * sizeof(int));
        r->vals = (uint64_t*)realloc(r->vals, r->cape * sizeof(uint64_t));
    }
    r->lens[r->nev] = len;
    r->codes[r->nev] = code;
    r->vals[r->nev] = code ? n : 0;
    r->nev++;
}
static void sink_rec(void* ud, int ev, const uint8_t* p, uint64_t n) { rec_push((rec_t*)ud, ev, p, n); }
static size_t rec_frames(const rec_t* r)
{
    size_t i, k = 0;
    for (i = 0; i < r->nev; i++)
        k += r->codes[i] == 0;
    return k;
}
static int rec_first_err(const rec_t* r)
{
    size_t i;
    for (i = 0; i < r->nev; i++)
        if (r->codes[i])
            return r->codes[i];
    return 0;
}
static int rec_eq(const rec_t* a, const rec_t* b, int vals)
{
    return a->nev == b->nev && a->nbytes == b->nbytes
        && (a->nev == 0 || (memcmp(a->lens, b->lens, a->nev * sizeof(size_t)) == 0 && memcmp(a->codes, b->codes, a->nev * sizeof(int)) == 0))
        && (!vals || a->nev == 0 || memcmp(a->vals, b->vals, a->nev * sizeof(uint64_t)) == 0)
        && (a->nbytes == 0 || memcmp(a->bytes, b->bytes, a->nbytes) == 0);
}
static size_t rec_frame_at(const rec_t* r, size_t k, const uint8_t** p)
{
    size_t i, off = 0;
    for (i = 0; i < r->nev; i++) {
        if (r->codes[i])
            continue;
        if (k-- == 0) {
            *p = r->bytes + off;
            return r->lens[i];
        }
        off += r->lens[i];
    }
    *p = NULL;
    return (size_t)-1;
}
static int rec_frames_are(const rec_t* r, const char* const* want, size_t n)
{
    size_t i;
    if (rec_frames(r) != n)
        return 0;
    for (i = 0; i < n; i++) {
        const uint8_t* p;
        size_t L = rec_frame_at(r, i, &p);
        if (L != strlen(want[i]) || (L && memcmp(p, want[i], L) != 0))
            return 0;
    }
    return 1;
}

typedef struct {
    int kind;
    dyn_frame_length_cfg L;
    const uint8_t* dptr[8];
    size_t dlen[8];
    size_t nd;
    int strip_delim;
    uint64_t dmax;
    int dff, dstop, dhold;
    int resume;
    uint64_t fixed;
} spec_t;

typedef struct {
    int err;
    size_t buffered;
    size_t pre;
} outcome_t;

static dyn_frame_t* make_dec(const spec_t* s)
{
    int err = 0;
    dyn_frame_t* d;
    if (s->kind == 1) {
        dyn_frame_length_cfg L = s->L;
        L.resume_after_error = s->resume;
        d = dyn_frame_new_length(&L, &err);
    } else if (s->kind == 2) {
        dyn_frame_delim_cfg c;
        c.delims = s->dptr;
        c.lens = s->dlen;
        c.count = s->nd;
        c.strip_delimiter = s->strip_delim;
        c.max_frame = s->dmax;
        c.fail_fast = s->dff;
        c.stop_on_error = s->dstop;
        c.hold_overlapping = s->dhold;
        c.resume_after_error = s->resume;
        d = dyn_frame_new_delim(&c, &err);
    } else {
        d = dyn_frame_new_fixed(s->fixed, &err);
    }
    if (!d) {
        printf("FATAL: decoder config rejected (%d)\n", err);
        exit(2);
    }
    return d;
}

static int is_line(const spec_t* s)
{
    if (s->kind != 2 || s->dhold || s->nd != 2)
        return 0;
    return (s->dlen[0] == 2 && memcmp(s->dptr[0], "\r\n", 2) == 0 && s->dlen[1] == 1 && s->dptr[1][0] == '\n')
        || (s->dlen[1] == 2 && memcmp(s->dptr[1], "\r\n", 2) == 0 && s->dlen[0] == 1 && s->dptr[0][0] == '\n');
}

/* ---------- oracle 1: literal per-read port of Netty 4.1 ---------- */

enum { N_NULL = 0, N_FRAME = 1, N_THROW = 2 };

typedef struct {
    const spec_t* s;
    rec_t* out;
    uint8_t* c;
    size_t ri, wi, cap;
    int dead;
    long long fli;
    int discarding;
    long long btd;
    long long tooLong;
    long long discardedBytes;
} netty_t;

static int nt_err(netty_t* n, int code, uint64_t v, int thrown)
{
    rec_push(n->out, code, NULL, v);
    if ((n->s->kind == 1 && n->s->L.stop_on_error) || (n->s->kind == 2 && n->s->dstop))
        n->dead = 1;
    return (thrown && !n->s->resume) || n->dead ? N_THROW : N_NULL;
}

static int nt_fail_if_necessary(netty_t* n, int first)
{
    int ff = n->s->L.fail_fast, stop = n->s->L.stop_on_error;
    if (n->btd == 0) {
        long long t = n->tooLong;
        n->tooLong = 0;
        n->discarding = 0;
        if (!ff || first)
            return nt_err(n, DYN_FRAME_ETOOLONG, (uint64_t)t, 1);
    } else if ((ff || stop) && first) {
        return nt_err(n, DYN_FRAME_ETOOLONG, (uint64_t)n->tooLong, 1);
    }
    return N_NULL;
}

static int nt_length_decode(netty_t* n)
{
    const dyn_frame_length_cfg* L = &n->s->L;
    long long eo = (long long)(L->offset + (uint64_t)L->width);
    if (n->fli == -1) {
        uint64_t raw = 0;
        long long frameLength;
        int i;
        if (n->discarding) {
            long long readable = (long long)(n->wi - n->ri);
            long long k = n->btd < readable ? n->btd : readable;
            n->ri += (size_t)k;
            n->btd -= k;
            if (nt_fail_if_necessary(n, 0) == N_THROW)
                return N_THROW;
        }
        if ((long long)(n->wi - n->ri) < eo)
            return N_NULL;
        for (i = 0; i < L->width; i++) {
            size_t at = n->ri + L->offset + (size_t)(L->little_endian ? L->width - 1 - i : i);
            raw = (raw << 8) | n->c[at];
        }
        if (L->reject_unsafe && raw > (((uint64_t)1) << 53)) {
            n->ri += (size_t)eo;
            return nt_err(n, DYN_FRAME_ERANGE, raw, 1);
        }
        if (L->width == 8 && (raw >> 63)) {
            n->ri += (size_t)eo;
            return nt_err(n, DYN_FRAME_ENEG, raw, 1);
        }
        frameLength = (long long)(raw + (uint64_t)(L->adjustment + eo));
        if (frameLength < eo) {
            n->ri += (size_t)eo;
            return nt_err(n, DYN_FRAME_ESHORT, (uint64_t)frameLength, 1);
        }
        if ((unsigned long long)frameLength > L->max_frame) {
            long long discard = frameLength - (long long)(n->wi - n->ri);
            n->tooLong = frameLength;
            if (discard < 0) {
                n->ri += (size_t)frameLength;
            } else {
                n->discarding = 1;
                n->btd = discard;
                n->ri = n->wi;
            }
            if (nt_fail_if_necessary(n, 1) == N_THROW)
                return N_THROW;
            return N_NULL;
        }
        n->fli = frameLength;
    }
    if ((long long)(n->wi - n->ri) < n->fli)
        return N_NULL;
    if ((long long)L->strip > n->fli) {
        long long t = n->fli;
        n->fli = -1;
        n->ri += (size_t)t;
        return nt_err(n, DYN_FRAME_ESTRIP, (uint64_t)t, 1);
    }
    rec_push(n->out, 0, n->c + n->ri + L->strip, (uint64_t)(n->fli - (long long)L->strip));
    n->ri += (size_t)n->fli;
    n->fli = -1;
    return N_FRAME;
}

static long long nt_index_of(const netty_t* n, const uint8_t* dl, size_t len)
{
    size_t i, j;
    for (i = n->ri; i + len <= n->wi; i++) {
        for (j = 0; j < len && n->c[i + j] == dl[j]; j++) { }
        if (j == len)
            return (long long)(i - n->ri);
    }
    return -1;
}

static int nt_delim_decode(netty_t* n)
{
    const spec_t* s = n->s;
    long long minLen = -1;
    size_t k, minK = 0, P = 0, readable = n->wi - n->ri, fb;
    for (k = 0; k < s->nd; k++) {
        long long idx = nt_index_of(n, s->dptr[k], s->dlen[k]);
        if (idx >= 0 && (minLen < 0 || idx < minLen)) {
            minLen = idx;
            minK = k;
        }
    }
    if (minLen >= 0) {
        size_t dl = s->dlen[minK];
        if (n->discarding) {
            long long t = n->tooLong;
            n->discarding = 0;
            n->ri += (size_t)minLen + dl;
            n->tooLong = 0;
            if (!s->dff)
                return nt_err(n, DYN_FRAME_ETOOLONG, (uint64_t)t, 1);
            return N_NULL;
        }
        if ((unsigned long long)minLen > s->dmax) {
            n->ri += (size_t)minLen + dl;
            return nt_err(n, DYN_FRAME_ETOOLONG, (uint64_t)minLen, 1);
        }
        rec_push(n->out, 0, n->c + n->ri, (uint64_t)minLen + (s->strip_delim ? 0 : dl));
        n->ri += (size_t)minLen + dl;
        return N_FRAME;
    }
    for (k = 0; k < s->nd; k++) {
        size_t L = s->dlen[k] - 1 < readable ? s->dlen[k] - 1 : readable;
        for (; L > P; L--)
            if (memcmp(n->c + n->wi - L, s->dptr[k], L) == 0) {
                P = L;
                break;
            }
    }
    fb = readable - P;
    if (!n->discarding) {
        if (fb > s->dmax) {
            n->tooLong = (long long)fb;
            n->ri += fb;
            n->discarding = 1;
            if (s->dff || s->dstop)
                return nt_err(n, DYN_FRAME_ETOOLONG, fb, 1);
        }
    } else {
        n->tooLong += (long long)fb;
        n->ri += fb;
    }
    return N_NULL;
}

static int nt_line_decode(netty_t* n)
{
    const spec_t* s = n->s;
    size_t i;
    long long eol = -1;
    for (i = n->ri; i < n->wi; i++)
        if (n->c[i] == '\n') {
            eol = (long long)i;
            if (i > n->ri && n->c[i - 1] == '\r')
                eol--;
            break;
        }
    if (!n->discarding) {
        if (eol >= 0) {
            long long length = eol - (long long)n->ri;
            int delimLength = n->c[eol] == '\r' ? 2 : 1;
            if ((unsigned long long)length > s->dmax) {
                n->ri = (size_t)eol + (size_t)delimLength;
                return nt_err(n, DYN_FRAME_ETOOLONG, (uint64_t)length, 0);
            }
            rec_push(n->out, 0, n->c + n->ri, (uint64_t)length + (s->strip_delim ? 0 : (uint64_t)delimLength));
            n->ri += (size_t)length + (size_t)delimLength;
            return N_FRAME;
        }
        {
            long long length = (long long)(n->wi - n->ri);
            if ((unsigned long long)length > s->dmax) {
                n->discardedBytes = length;
                n->ri = n->wi;
                n->discarding = 1;
                if (s->dff || s->dstop)
                    return nt_err(n, DYN_FRAME_EOVER, (uint64_t)n->discardedBytes, 0);
            }
            return N_NULL;
        }
    }
    if (eol >= 0) {
        long long length = n->discardedBytes + eol - (long long)n->ri;
        int delimLength = n->c[eol] == '\r' ? 2 : 1;
        n->ri = (size_t)eol + (size_t)delimLength;
        n->discardedBytes = 0;
        n->discarding = 0;
        if (!s->dff)
            return nt_err(n, DYN_FRAME_ETOOLONG, (uint64_t)length, 0);
    } else {
        n->discardedBytes += (long long)(n->wi - n->ri);
        n->ri = n->wi;
    }
    return N_NULL;
}

static int nt_decode(netty_t* n)
{
    if (n->s->kind == 1)
        return nt_length_decode(n);
    if (is_line(n->s))
        return nt_line_decode(n);
    if (n->s->kind == 2)
        return nt_delim_decode(n);
    if (n->wi - n->ri < n->s->fixed)
        return N_NULL;
    rec_push(n->out, 0, n->c + n->ri, n->s->fixed);
    n->ri += (size_t)n->s->fixed;
    return N_FRAME;
}

static int nt_call_decode(netty_t* n)
{
    while (!n->dead && n->wi > n->ri) {
        size_t old = n->wi - n->ri;
        int r = nt_decode(n);
        if (r == N_THROW)
            return 1;
        if (r == N_NULL && old == n->wi - n->ri)
            break;
    }
    return 0;
}

static void nt_read(netty_t* n, const uint8_t* p, size_t len)
{
    if (n->dead || len == 0)
        return;
    if (n->ri == n->wi)
        n->ri = n->wi = 0;
    if (n->wi + len > n->cap) {
        n->cap = (n->wi + len) * 2 + 64;
        n->c = (uint8_t*)realloc(n->c, n->cap);
    }
    memcpy(n->c + n->wi, p, len);
    n->wi += len;
    nt_call_decode(n);
}

static void nt_input_closed(netty_t* n)
{
    if (n->dead)
        return;
    if (nt_call_decode(n))
        return;
    if (!n->dead && n->wi > n->ri)
        nt_decode(n);
}

static void netty_run(const spec_t* s, const uint8_t* S, size_t n, const size_t* cuts, size_t ncuts, rec_t* out)
{
    netty_t nt;
    size_t prev = 0, i;
    memset(&nt, 0, sizeof(nt));
    nt.s = s;
    nt.out = out;
    nt.fli = -1;
    for (i = 0; i <= ncuts; i++) {
        size_t at = i < ncuts ? cuts[i] : n;
        nt_read(&nt, S + prev, at - prev);
        prev = at;
    }
    nt_input_closed(&nt);
    free(nt.c);
}

/* ---------- oracle 2: C4 whole-stream reference (invariant option mode) ---------- */

static uint64_t ref_raw(const spec_t* s, const uint8_t* p)
{
    uint64_t v = 0, mul = 1;
    int i;
    for (i = 0; i < s->L.width; i++) {
        int idx = s->L.little_endian ? i : s->L.width - 1 - i;
        v += (uint64_t)p[idx] * mul;
        mul = i == 7 ? 0 : mul * 256;
    }
    return v;
}

static outcome_t ref_length(const spec_t* s, const uint8_t* S, size_t n, rec_t* out)
{
    outcome_t o = { 0, 0, 0 };
    size_t pos = 0;
    long long eo = (long long)(s->L.offset + (uint64_t)s->L.width);
    for (;;) {
        uint64_t raw;
        long long fl;
        if ((long long)(n - pos) < eo)
            break;
        raw = ref_raw(s, S + pos + s->L.offset);
        if (raw > (((uint64_t)1) << 53)) { o.err = DYN_FRAME_ERANGE; break; }
        fl = (long long)raw + (long long)s->L.adjustment + eo;
        if (fl < eo) { o.err = DYN_FRAME_ESHORT; break; }
        if ((unsigned long long)fl > s->L.max_frame) { o.err = DYN_FRAME_ETOOLONG; break; }
        if ((long long)(n - pos) < fl)
            break;
        if ((long long)s->L.strip > fl) { o.err = DYN_FRAME_ESTRIP; break; }
        rec_push(out, 0, S + pos + s->L.strip, (uint64_t)(fl - (long long)s->L.strip));
        pos += (size_t)fl;
    }
    if (o.err)
        rec_push(out, o.err, NULL, 0);
    o.buffered = o.err ? 0 : n - pos;
    return o;
}

static outcome_t ref_delim(const spec_t* s, const uint8_t* S, size_t n, rec_t* out)
{
    outcome_t o = { 0, 0, 0 };
    size_t pos = 0, k, P = 0;
    for (;;) {
        size_t best = (size_t)-1, bk = 0, i, j;
        for (k = 0; k < s->nd; k++) {
            for (i = pos; i + s->dlen[k] <= n; i++) {
                for (j = 0; j < s->dlen[k] && S[i + j] == s->dptr[k][j]; j++) { }
                if (j == s->dlen[k])
                    break;
            }
            if (i + s->dlen[k] <= n && i < best) {
                best = i;
                bk = k;
            }
        }
        if (best == (size_t)-1)
            break;
        if (best - pos > s->dmax) { o.err = DYN_FRAME_ETOOLONG; break; }
        rec_push(out, 0, S + pos, best - pos + (s->strip_delim ? 0 : s->dlen[bk]));
        pos = best + s->dlen[bk];
    }
    if (!o.err) {
        for (k = 0; k < s->nd; k++) {
            size_t L;
            for (L = s->dlen[k] - 1 < n - pos ? s->dlen[k] - 1 : n - pos; L > P; L--)
                if (memcmp(S + n - L, s->dptr[k], L) == 0) {
                    P = L;
                    break;
                }
        }
        if (n - pos - P > s->dmax)
            o.err = DYN_FRAME_ETOOLONG;
    }
    if (o.err)
        rec_push(out, o.err, NULL, 0);
    o.buffered = o.err ? 0 : n - pos;
    return o;
}

static outcome_t ref_fixed(const spec_t* s, const uint8_t* S, size_t n, rec_t* out)
{
    outcome_t o = { 0, 0, 0 };
    size_t pos = 0;
    while (n - pos >= s->fixed) {
        rec_push(out, 0, S + pos, s->fixed);
        pos += (size_t)s->fixed;
    }
    o.buffered = n - pos;
    return o;
}

static int invariant_mode(const spec_t* s)
{
    if (s->kind == 1)
        return s->L.stop_on_error && s->L.reject_unsafe;
    if (s->kind == 2)
        return s->dhold && s->dstop;
    return 1;
}

static outcome_t ref_run(const spec_t* s, const uint8_t* S, size_t n, rec_t* out)
{
    if (s->kind == 1)
        return ref_length(s, S, n, out);
    if (s->kind == 2)
        return ref_delim(s, S, n, out);
    return ref_fixed(s, S, n, out);
}

static outcome_t run_cuts(const spec_t* s, const uint8_t* S, size_t n, const size_t* cuts, size_t ncuts, rec_t* out, uint64_t* copied)
{
    dyn_frame_t* d = make_dec(s);
    outcome_t o = { 0, 0, 0 };
    size_t prev = 0, i;
    int rc = 0;
    for (i = 0; i <= ncuts; i++) {
        size_t at = i < ncuts ? cuts[i] : n;
        int r = dyn_frame_feed(d, S + prev, at - prev, sink_rec, out);
        if (rc && r != rc)
            printf("FAIL: fatal error changed %d -> %d\n", rc, r), g_fail++;
        if (r)
            rc = r;
        prev = at;
    }
    *copied = dyn_frame_copied(d);
    o.pre = dyn_frame_buffered(d);
    {
        int fr = dyn_frame_finish(d, sink_rec, out);
        if (rc && fr != rc)
            printf("FAIL: finish changed the fatal error %d -> %d\n", rc, fr), g_fail++;
        rc = fr;
    }
    o.err = dyn_frame_error(d);
    if (o.err != rc)
        printf("FAIL: dyn_frame_error %d != feed %d\n", o.err, rc), g_fail++;
    o.buffered = dyn_frame_buffered(d);
    dyn_frame_free(d);
    return o;
}

static void dump_fail(const spec_t* s, const uint8_t* S, size_t n, const size_t* cuts, size_t ncuts, const rec_t* want, const rec_t* got)
{
    size_t q;
    if (!getenv("FRAME_DUMP") || g_fail > 2)
        return;
    if (s->kind == 2) {
        printf("  delims:");
        for (q = 0; q < s->nd; q++) printf(" [%.*s]", (int)s->dlen[q], (const char*)s->dptr[q]);
        printf(" strip=%d max=%llu ff=%d stop=%d hold=%d resume=%d\n", s->strip_delim, (unsigned long long)s->dmax, s->dff, s->dstop, s->dhold, s->resume);
    } else if (s->kind == 1) {
        printf("  length off=%llu w=%d le=%d adj=%lld strip=%llu max=%llu ff=%d stop=%d rej=%d\n", (unsigned long long)s->L.offset, s->L.width,
            s->L.little_endian, (long long)s->L.adjustment, (unsigned long long)s->L.strip, (unsigned long long)s->L.max_frame,
            s->L.fail_fast, s->L.stop_on_error, s->L.reject_unsafe);
    }
    printf("  stream(%zu):", n);
    for (q = 0; q < n && q < 200; q++) printf(s->kind == 2 ? "%c" : " %02x", s->kind == 2 ? (S[q] == 13 ? 126 : S[q] == 10 ? 124 : S[q]) : S[q]);
    printf("\n  cuts:");
    for (q = 0; q < ncuts && q < 40; q++) printf(" %zu", cuts[q]);
    printf("\n  want:");
    for (q = 0; q < want->nev; q++) printf(want->codes[q] ? " E%d(%llu)" : " %d", want->codes[q] ? want->codes[q] : (int)want->lens[q], (unsigned long long)want->vals[q]);
    printf("\n  got: ");
    for (q = 0; q < got->nev; q++) printf(got->codes[q] ? " E%d(%llu)" : " %d", got->codes[q] ? got->codes[q] : (int)got->lens[q], (unsigned long long)got->vals[q]);
    printf("\n");
}

static int g_netty_cmp, g_inv_cmp, g_hold_only;

static int compare_one(const spec_t* s, const uint8_t* S, size_t n, const size_t* cuts, size_t ncuts, const char* what)
{
    rec_t got = { 0 };
    uint64_t copied = 0;
    outcome_t b = run_cuts(s, S, n, cuts, ncuts, &got, &copied);
    int ok = 1;
    if (s->kind == 2 && s->dhold && !s->dstop)
        g_hold_only++;
    if (!(s->kind == 2 && s->dhold)) {
        rec_t want = { 0 };
        netty_run(s, S, n, cuts, ncuts, &want);
        g_checks++;
        g_netty_cmp++;
        if (!rec_eq(&want, &got, 1)) {
            ok = 0;
            g_fail++;
            printf("FAIL [%s/netty kind=%d n=%zu cuts=%zu]: events %zu/%zu\n", what, s->kind, n, ncuts, want.nev, got.nev);
            dump_fail(s, S, n, cuts, ncuts, &want, &got);
        }
        rec_free(&want);
    }
    if (invariant_mode(s)) {
        rec_t want = { 0 };
        outcome_t a = ref_run(s, S, n, &want);
        g_checks++;
        g_inv_cmp++;
        if (!rec_eq(&want, &got, 0) || a.err != b.err || (!a.err && a.buffered != b.buffered && !(s->kind == 2 && s->dhold))) {
            ok = 0;
            g_fail++;
            printf("FAIL [%s/invariant kind=%d n=%zu cuts=%zu]: events %zu/%zu err %d/%d buffered %zu/%zu\n", what, s->kind, n, ncuts,
                want.nev, got.nev, a.err, b.err, a.buffered, b.buffered);
            dump_fail(s, S, n, cuts, ncuts, &want, &got);
        }
        rec_free(&want);
    }
    g_checks++;
    if (copied > n) {
        g_fail++;
        printf("FAIL [%s]: copied %llu bytes of %zu fed (a byte copied twice)\n", what, (unsigned long long)copied, n);
    }
    if (ncuts == 0 && !b.err) {
        g_checks++;
        if (copied != b.pre) {
            g_fail++;
            printf("FAIL [%s]: one feed copied %llu, tail is %zu\n", what, (unsigned long long)copied, b.pre);
        }
    }
    rec_free(&got);
    return ok;
}

static size_t g_cuts[1 << 16];

static int compare_all(const spec_t* s, const uint8_t* S, size_t n, const char* what, int splits2)
{
    size_t i, k, nc;
    int ok = 1;
    ok &= compare_one(s, S, n, NULL, 0, what);
    for (i = 1; i < n && i <= (sizeof(g_cuts) / sizeof(g_cuts[0])); i++)
        g_cuts[i - 1] = i;
    ok &= compare_one(s, S, n, g_cuts, n ? n - 1 : 0, what);
    for (k = 0; k < 6; k++) {
        size_t at = 0;
        nc = 0;
        for (;;) {
            at += 1 + rnd_below(k < 3 ? 7 : 64);
            if (at >= n)
                break;
            g_cuts[nc++] = at;
        }
        ok &= compare_one(s, S, n, g_cuts, nc, what);
    }
    if (splits2)
        for (i = 1; i < n; i++) {
            g_cuts[0] = i;
            ok &= compare_one(s, S, n, g_cuts, 1, what);
        }
    for (k = 0; k < 8 && n > 2; k++) {
        size_t a = 1 + rnd_below(n - 1), b = 1 + rnd_below(n - 1);
        if (a == b)
            continue;
        g_cuts[0] = a < b ? a : b;
        g_cuts[1] = a < b ? b : a;
        ok &= compare_one(s, S, n, g_cuts, 2, what);
    }
    return ok;
}

static size_t put_raw(uint8_t* p, int width, int le, uint64_t raw)
{
    int i;
    for (i = 0; i < width; i++) {
        uint8_t b = (uint8_t)(raw >> (8 * i));
        p[le ? i : width - 1 - i] = b;
    }
    return (size_t)width;
}

static size_t gen_length_stream(spec_t* s, uint8_t* S, size_t cap, int corrupt)
{
    size_t n = 0;
    uint64_t eo = s->L.offset + (uint64_t)s->L.width;
    uint64_t wmax = s->L.width == 8 ? ~(uint64_t)0 : ((((uint64_t)1) << (8 * s->L.width)) - 1);
    int frames = 1 + (int)rnd_below(12), f;
    for (f = 0; f < frames; f++) {
        uint64_t fl, raw, j;
        int64_t r;
        if (corrupt && rnd_below(5) == 0) {
            int mode = (int)rnd_below(5);
            if (mode == 0)
                fl = s->L.max_frame + 1 + rnd_below(rnd_below(2) ? 3 : 90);
            else if (mode == 1)
                fl = (uint64_t)rnd_below(eo + 1);
            else
                fl = eo + rnd_below(s->L.max_frame - eo + 1);
            r = (int64_t)fl - (int64_t)eo - s->L.adjustment;
            if (mode == 3 && s->L.width == 8)
                raw = (((uint64_t)1) << 53) + 1 + rnd_below(1000);
            else if (mode == 4 && s->L.width == 8)
                raw = (((uint64_t)1) << 63) + rnd_below(1000);
            else if (r < 0)
                raw = wmax - rnd_below(3);
            else
                raw = (uint64_t)r;
        } else {
            uint64_t lo = eo;
            if (s->L.adjustment < 0 && lo < eo + (uint64_t)(-s->L.adjustment))
                lo = eo + (uint64_t)(-s->L.adjustment);
            if (lo < s->L.strip)
                lo = s->L.strip;
            if (lo > s->L.max_frame)
                return n;
            fl = lo + rnd_below((s->L.max_frame - lo + 1) < 40 ? (s->L.max_frame - lo + 1) : 40);
            raw = (uint64_t)((int64_t)fl - (int64_t)eo - s->L.adjustment);
        }
        if (raw > wmax)
            raw &= wmax;
        if (n + eo > cap)
            return n;
        for (j = 0; j < s->L.offset; j++)
            S[n + j] = (uint8_t)rnd();
        put_raw(S + n + s->L.offset, s->L.width, s->L.little_endian, raw);
        n += (size_t)eo;
        {
            uint64_t real = raw + (uint64_t)((int64_t)eo + s->L.adjustment);
            uint64_t body = real > eo && real < 4096 ? real - eo : rnd_below(8);
            for (j = 0; j < body && n < cap; j++)
                S[n++] = (uint8_t)rnd();
        }
    }
    if (rnd_below(3) == 0)
        n -= rnd_below(n < 5 ? n : 5);
    return n;
}

static void fuzz_length(int iters)
{
    static uint8_t S[1 << 14];
    int it;
    const int widths[5] = { 1, 2, 3, 4, 8 };
    for (it = 0; it < iters; it++) {
        spec_t s;
        size_t n;
        memset(&s, 0, sizeof(s));
        s.kind = 1;
        s.L.width = widths[rnd_below(5)];
        s.L.offset = rnd_below(5);
        s.L.little_endian = (int)rnd_below(2);
        s.L.max_frame = s.L.offset + (uint64_t)s.L.width + rnd_below(s.L.width == 1 ? 200 : 300);
        s.L.adjustment = (int64_t)rnd_below(12) - 6;
        s.L.strip = rnd_below(s.L.offset + (uint64_t)s.L.width + 3);
        s.L.fail_fast = (int)rnd_below(2);
        s.L.stop_on_error = (int)rnd_below(3) == 0;
        s.L.reject_unsafe = s.L.stop_on_error ? (int)rnd_below(2) : (int)(rnd_below(4) == 0);
        s.resume = (int)rnd_below(3) == 0;
        n = gen_length_stream(&s, S, sizeof(S), (int)rnd_below(2));
        compare_all(&s, S, n, "fuzz-length", n < 200);
    }
}

static void fuzz_delim(int iters)
{
    static const char* const pool[][4] = {
        { "\n", NULL, NULL, NULL },
        { "\r\n", "\n", NULL, NULL },
        { "\n", "\r\n", NULL, NULL },
        { "ab", "abc", "b", NULL },
        { "aa", NULL, NULL, NULL },
        { "abcab", "ca", NULL, NULL },
        { "<END>", "\r\n", "\n", NULL },
        { "bb", "b", NULL, NULL },
    };
    static const char alpha[] = "abc\r\n<END>";
    static uint8_t S[4096];
    int it;
    for (it = 0; it < iters; it++) {
        spec_t s;
        size_t n, i, p = rnd_below(sizeof(pool) / sizeof(pool[0]));
        memset(&s, 0, sizeof(s));
        s.kind = 2;
        for (i = 0; i < 4 && pool[p][i]; i++) {
            s.dptr[i] = (const uint8_t*)pool[p][i];
            s.dlen[i] = strlen(pool[p][i]);
        }
        s.nd = i;
        s.strip_delim = (int)rnd_below(2);
        s.dmax = 1 + rnd_below(rnd_below(2) ? 8 : 60);
        s.dff = (int)rnd_below(2);
        s.dhold = (int)rnd_below(3) == 0;
        s.dstop = s.dhold ? (int)rnd_below(2) : (int)(rnd_below(4) == 0);
        s.resume = (int)rnd_below(3) == 0;
        n = rnd_below(rnd_below(4) ? 120 : sizeof(S));
        for (i = 0; i < n; i++)
            S[i] = (uint8_t)alpha[rnd_below(rnd_below(3) ? 3 : sizeof(alpha) - 1)];
        compare_all(&s, S, n, "fuzz-delim", n < 150);
    }
}

static void fuzz_fixed(int iters)
{
    static uint8_t S[2048];
    int it;
    for (it = 0; it < iters; it++) {
        spec_t s;
        size_t n, i;
        memset(&s, 0, sizeof(s));
        s.kind = 3;
        s.fixed = 1 + rnd_below(rnd_below(2) ? 4 : 50);
        n = rnd_below(sizeof(S));
        for (i = 0; i < n; i++)
            S[i] = (uint8_t)rnd();
        compare_all(&s, S, n, "fuzz-fixed", n < 120);
    }
}

typedef struct {
    rec_t r;
    int err;
    int fatal;
} one_t;

static one_t decode_once(const spec_t* s, const uint8_t* S, size_t n)
{
    one_t o;
    uint64_t copied;
    outcome_t out;
    memset(&o, 0, sizeof(o));
    out = run_cuts(s, S, n, NULL, 0, &o.r, &copied);
    o.err = rec_first_err(&o.r);
    o.fatal = out.err;
    return o;
}

static spec_t lspec(uint64_t off, int width, int le, int64_t adj, uint64_t strip, uint64_t max)
{
    spec_t s;
    memset(&s, 0, sizeof(s));
    s.kind = 1;
    s.L.offset = off;
    s.L.width = width;
    s.L.little_endian = le;
    s.L.adjustment = adj;
    s.L.strip = strip;
    s.L.max_frame = max;
    s.L.fail_fast = 1;
    return s;
}

static spec_t dspec(const char* a, const char* b, int strip, uint64_t max)
{
    spec_t s;
    memset(&s, 0, sizeof(s));
    s.kind = 2;
    s.dptr[0] = (const uint8_t*)a;
    s.dlen[0] = strlen(a);
    s.nd = 1;
    if (b) {
        s.dptr[1] = (const uint8_t*)b;
        s.dlen[1] = strlen(b);
        s.nd = 2;
    }
    s.strip_delim = strip;
    s.dmax = max;
    s.dff = 1;
    return s;
}

static void netty_javadoc_examples(void)
{
    static const uint8_t body[] = "HELLO, WORLD";
    uint8_t S[64];
    spec_t s;
    one_t o;

    S[0] = 0x00, S[1] = 0x0C;
    memcpy(S + 2, body, 12);
    s = lspec(0, 2, 0, 0, 0, 1024);
    o = decode_once(&s, S, 14);
    CHECK(o.r.nev == 1 && o.r.lens[0] == 14 && memcmp(o.r.bytes, S, 14) == 0, "javadoc 1: 2 bytes length field at offset 0, do not strip header");
    rec_free(&o.r);
    s = lspec(0, 2, 0, 0, 2, 1024);
    o = decode_once(&s, S, 14);
    CHECK(o.r.nev == 1 && o.r.lens[0] == 12 && memcmp(o.r.bytes, body, 12) == 0, "javadoc 2: strip header");
    rec_free(&o.r);

    S[1] = 0x0E;
    s = lspec(0, 2, 0, -2, 0, 1024);
    o = decode_once(&s, S, 14);
    CHECK(o.r.nev == 1 && o.r.lens[0] == 14, "javadoc 3: length field represents the whole message");
    rec_free(&o.r);

    S[0] = 0xCA, S[1] = 0xFE, S[2] = 0x00, S[3] = 0x00, S[4] = 0x0C;
    memcpy(S + 5, body, 12);
    s = lspec(2, 3, 0, 0, 0, 1024);
    o = decode_once(&s, S, 17);
    CHECK(o.r.nev == 1 && o.r.lens[0] == 17, "javadoc 4: 3 bytes length field at the end of a 5 bytes header (width 3)");
    rec_free(&o.r);

    S[0] = 0x00, S[1] = 0x00, S[2] = 0x0C, S[3] = 0xCA, S[4] = 0xFE;
    s = lspec(0, 3, 0, 2, 0, 1024);
    o = decode_once(&s, S, 17);
    CHECK(o.r.nev == 1 && o.r.lens[0] == 17, "javadoc 5: 3 bytes length field at the beginning of a 5 bytes header");
    rec_free(&o.r);

    S[0] = 0xCA, S[1] = 0x00, S[2] = 0x0C, S[3] = 0xFE;
    memcpy(S + 4, body, 12);
    s = lspec(1, 2, 0, 1, 3, 1024);
    o = decode_once(&s, S, 16);
    CHECK(o.r.nev == 1 && o.r.lens[0] == 13 && o.r.bytes[0] == 0xFE && memcmp(o.r.bytes + 1, body, 12) == 0, "javadoc 6: 2 bytes length at offset 1 in a 4 byte header, strip first header field and length field");
    rec_free(&o.r);

    S[2] = 0x10;
    s = lspec(1, 2, 0, -3, 3, 1024);
    o = decode_once(&s, S, 16);
    CHECK(o.r.nev == 1 && o.r.lens[0] == 13 && o.r.bytes[0] == 0xFE, "javadoc 7: length represents the whole message, adjustment -3");
    rec_free(&o.r);
}

static void length_edges(void)
{
    uint8_t S[64];
    spec_t s;
    one_t o;
    int le;

    for (le = 0; le < 2; le++) {
        s = lspec(0, 4, le, 0, 4, 16);
        memset(S, 0, sizeof(S));
        put_raw(S, 4, le, 12);
        o = decode_once(&s, S, 16);
        CHECK(o.err == 0 && rec_frames(&o.r) == 1 && o.r.lens[0] == 12, "le=%d: frame of exactly maxFrameLength (16) is delivered", le);
        rec_free(&o.r);
        put_raw(S, 4, le, 13);
        o = decode_once(&s, S, 4);
        CHECK(o.err == DYN_FRAME_ETOOLONG && o.r.vals[0] == 17 && rec_frames(&o.r) == 0 && o.fatal == 0, "le=%d: maxFrameLength + 1 is reported from the header alone (failFast), not fatal", le);
        rec_free(&o.r);
    }

    s = lspec(0, 2, 1, 0, 0, 4096);
    S[0] = 0x05, S[1] = 0x00;
    memcpy(S + 2, "abcde", 5);
    o = decode_once(&s, S, 7);
    CHECK(rec_frames(&o.r) == 1 && o.r.lens[0] == 7, "little endian 0x0005");
    rec_free(&o.r);
    S[0] = 0x00, S[1] = 0x05;
    o = decode_once(&s, S, 7);
    CHECK(o.r.nev == 0, "little endian reads 0x0500: incomplete, no error");
    rec_free(&o.r);

    s = lspec(0, 1, 0, -2, 0, 64);
    S[0] = 0;
    o = decode_once(&s, S, 1);
    CHECK(o.err == DYN_FRAME_ESHORT && (int64_t)o.r.vals[0] == -1, "adjusted length below the end offset is ESHORT (value -1)");
    rec_free(&o.r);

    s = lspec(0, 1, 0, 0, 5, 64);
    S[0] = 2, S[1] = 'x', S[2] = 'y';
    o = decode_once(&s, S, 2);
    CHECK(o.r.nev == 0, "strip > frame length: Netty reports it only once the frame is complete");
    rec_free(&o.r);
    o = decode_once(&s, S, 3);
    CHECK(o.err == DYN_FRAME_ESTRIP && o.r.vals[0] == 3 && rec_frames(&o.r) == 0, "strip > frame length is ESTRIP(3)");
    rec_free(&o.r);
    s = lspec(0, 1, 0, 0, 3, 64);
    o = decode_once(&s, S, 3);
    CHECK(o.err == 0 && rec_frames(&o.r) == 1 && o.r.lens[0] == 0, "strip == frame length gives an empty frame");
    rec_free(&o.r);

    s = lspec(3, 1, 0, 0, 0, 64);
    memcpy(S, "\x01\x02\x03\x01Z", 5);
    o = decode_once(&s, S, 5);
    CHECK(rec_frames(&o.r) == 1 && o.r.lens[0] == 5, "offset 3: header bytes before the length field count in the frame");
    rec_free(&o.r);
}

static void width8_edges(void)
{
    uint8_t S[32];
    spec_t s;
    one_t o;
    int le;
    const uint64_t P53 = ((uint64_t)1) << 53;
    for (le = 0; le < 2; le++) {
        s = lspec(0, 8, le, 0, 0, P53);
        put_raw(S, 8, le, P53 - 8);
        o = decode_once(&s, S, 8);
        CHECK(o.r.nev == 0, "le=%d: raw 2^53-8 (frame == max == 2^53) waits for the body", le);
        rec_free(&o.r);
        put_raw(S, 8, le, P53 - 7);
        o = decode_once(&s, S, 8);
        CHECK(o.err == DYN_FRAME_ETOOLONG, "le=%d: frame 2^53+1 > max is too long", le);
        rec_free(&o.r);
        put_raw(S, 8, le, P53 + 1);
        o = decode_once(&s, S, 8);
        CHECK(o.err == DYN_FRAME_ETOOLONG && o.r.vals[0] == P53 + 9, "le=%d: Netty: raw 2^53+1 is a positive long, too long (adjusted 2^53+9)", le);
        rec_free(&o.r);
        put_raw(S, 8, le, ~(uint64_t)0);
        o = decode_once(&s, S, 8);
        CHECK(o.err == DYN_FRAME_ENEG && (int64_t)o.r.vals[0] == -1, "le=%d: Netty: raw 2^64-1 is the long -1: negative pre-adjustment length field", le);
        rec_free(&o.r);
        put_raw(S, 8, le, ((uint64_t)1) << 63);
        o = decode_once(&s, S, 8);
        CHECK(o.err == DYN_FRAME_ENEG, "le=%d: raw 2^63 is negative (Long.MIN_VALUE)", le);
        rec_free(&o.r);
        s = lspec(0, 8, le, 5, 0, P53);
        put_raw(S, 8, le, (((uint64_t)1) << 63) - 1);
        o = decode_once(&s, S, 8);
        CHECK(o.err == DYN_FRAME_ESHORT && (int64_t)o.r.vals[0] < 0, "le=%d: raw 2^63-1 + adjustment 5 + 8 wraps negative as a Java long: ESHORT", le);
        rec_free(&o.r);
        s = lspec(0, 8, le, -(int64_t)P53, 0, P53);
        put_raw(S, 8, le, P53 + 1);
        memset(S + 8, 'q', 9);
        o = decode_once(&s, S, 9);
        CHECK(o.err == 0 && rec_frames(&o.r) == 1 && o.r.lens[0] == 9, "le=%d: raw 2^53+1 with adjustment -2^53 is a 9-byte frame (Netty: adjusted length decides)", le);
        rec_free(&o.r);
        s.L.reject_unsafe = 1;
        o = decode_once(&s, S, 9);
        CHECK(o.err == DYN_FRAME_ERANGE && rec_frames(&o.r) == 0, "le=%d: rejectUnsafeLength: the same raw 2^53+1 is ERANGE", le);
        rec_free(&o.r);
        put_raw(S, 8, le, ~(uint64_t)0);
        o = decode_once(&s, S, 8);
        CHECK(o.err == DYN_FRAME_ERANGE, "le=%d: rejectUnsafeLength: a negative long is ERANGE too", le);
        rec_free(&o.r);
        s = lspec(0, 8, le, (int64_t)P53, 0, P53);
        put_raw(S, 8, le, P53);
        o = decode_once(&s, S, 8);
        CHECK(o.err == DYN_FRAME_ETOOLONG, "le=%d: raw 2^53 + adjustment 2^53 does not overflow, is too long", le);
        rec_free(&o.r);
    }
}

static void delim_every_split(void)
{
    static const char* const streams[] = { "hello\r\nworld\r\n\r\nx", "a<END>bb<END><END>ccc<EN", "<END>", "abc\r\ndef\nx\r" };
    static const char* const ds[][2] = { { "\r\n", NULL }, { "<END>", "\n" }, { "<END>", NULL }, { "\r\n", "\n" } };
    size_t t;
    for (t = 0; t < 4; t++) {
        int mode;
        for (mode = 0; mode < 8; mode++) {
            spec_t s = dspec(ds[t][0], ds[t][1], mode & 1, (mode & 4) ? 2 : 64);
            s.dhold = (mode & 2) != 0;
            s.dstop = s.dhold;
            compare_all(&s, (const uint8_t*)streams[t], strlen(streams[t]), "delim-split", 1);
        }
    }
}

static void delim_semantics(void)
{
    spec_t s;
    one_t o;
    const char* S;
    static const char* const ab_cd[] = { "ab", "cd" };
    s = dspec("\n", "\r\n", 1, 64);
    S = "ab\r\ncd\n";
    o = decode_once(&s, (const uint8_t*)S, strlen(S));
    CHECK(rec_frames_are(&o.r, ab_cd, 2), "shortest frame wins: \\r\\n beats \\n: frames ab, cd");
    rec_free(&o.r);
    s = dspec("ab", "abc", 0, 64);
    S = "xabcy";
    o = decode_once(&s, (const uint8_t*)S, strlen(S));
    CHECK(rec_frames(&o.r) == 1 && o.r.lens[0] == 3 && memcmp(o.r.bytes, "xab", 3) == 0, "tie at the same index: the earlier delimiter in the list wins ('xab')");
    rec_free(&o.r);
    s = dspec("abc", "ab", 0, 64);
    o = decode_once(&s, (const uint8_t*)S, strlen(S));
    CHECK(rec_frames(&o.r) == 1 && o.r.lens[0] == 4, "tie, list reversed: 'xabc'");
    rec_free(&o.r);
}

static void feed_str(dyn_frame_t* d, const char* p, rec_t* r) { dyn_frame_feed(d, (const uint8_t*)p, strlen(p), sink_rec, r); }

static void default_vs_options(void)
{
    rec_t r = { 0 };
    dyn_frame_t* d;
    spec_t s;
    char msg[160];
    static const char* const f_1_e_2[] = { "1", "", "2" };
    static const char* const f_1_2[] = { "1", "2" };
    static const char* const f_ab[] = { "ab" };
    static const char* const f_ab_c[] = { "ab", "c" };

    s = dspec("bb", "b", 1, 64);
    d = make_dec(&s);
    feed_str(d, "1b", &r);
    feed_str(d, "b2b", &r);
    CHECK(rec_frames_are(&r, f_1_e_2, 3), "overlap default (Netty per read): \"1b\" then \"b2b\" gives 1, \"\", 2");
    dyn_frame_free(d);
    rec_reset(&r);
    d = make_dec(&s);
    feed_str(d, "1bb2b", &r);
    CHECK(rec_frames_are(&r, f_1_2, 2), "overlap default, one read \"1bb2b\": bb wins the tie: 1, 2 (segmentation-dependent, as Netty)");
    dyn_frame_free(d);
    rec_reset(&r);
    s.dhold = 1;
    d = make_dec(&s);
    feed_str(d, "1b", &r);
    CHECK(rec_frames(&r) == 0, "holdOverlapping: \"1b\" is held (a following b would make bb win)");
    feed_str(d, "b2b", &r);
    CHECK(rec_frames_are(&r, f_1_2, 1) || (rec_frames(&r) == 1), "holdOverlapping: then 1 (2b still held)");
    dyn_frame_finish(d, sink_rec, &r);
    CHECK(rec_frames_are(&r, f_1_2, 2), "holdOverlapping + finish: 1, 2 for every segmentation");
    dyn_frame_free(d);
    rec_reset(&r);

    s = lspec(0, 1, 0, 0, 1, 8);
    d = make_dec(&s);
    {
        const uint8_t a[] = { 2, 'a', 'b', 20, 1, 2, 3 }, b[] = { 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 2, 'c', 'd' };
        dyn_frame_feed(d, a, sizeof(a), sink_rec, &r);
        CHECK(rec_frames_are(&r, f_ab, 1) && r.nev == 2 && r.codes[1] == DYN_FRAME_ETOOLONG && r.vals[1] == 21, "length failFast: ab, then too long (21) at the header");
        dyn_frame_feed(d, b, sizeof(b), sink_rec, &r);
        CHECK(rec_frames(&r) == 2 && r.nev == 3 && dyn_frame_error(d) == 0, "length default: the 21-byte frame is discarded and decoding continues: cd (not fatal)");
    }
    dyn_frame_free(d);
    rec_reset(&r);
    s.L.fail_fast = 0;
    d = make_dec(&s);
    {
        const uint8_t a[] = { 2, 'a', 'b', 20, 1, 2, 3 }, b[] = { 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 2, 'c', 'd' }, c[] = { 1, 'e' };
        dyn_frame_feed(d, a, sizeof(a), sink_rec, &r);
        CHECK(r.nev == 1, "length failFast false: no error while the too-long frame is still being discarded");
        dyn_frame_feed(d, b, sizeof(b), sink_rec, &r);
        CHECK(r.nev == 2 && r.codes[1] == DYN_FRAME_ETOOLONG && r.vals[1] == 21, "failFast false: the error comes once the discard completes");
        CHECK(dyn_frame_stalled(d) && dyn_frame_buffered(d) == 3, "and, as Netty's callDecode stops at the thrown exception, cd waits in the cumulation");
        dyn_frame_feed(d, c, sizeof(c), sink_rec, &r);
        CHECK(rec_frames(&r) == 3 && r.nev == 4, "the next read resumes: cd, e");
    }
    dyn_frame_free(d);
    rec_reset(&r);
    s.resume = 1;
    d = make_dec(&s);
    {
        const uint8_t a[] = { 2, 'a', 'b', 20, 1, 2, 3 }, b[] = { 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 2, 'c', 'd' };
        dyn_frame_feed(d, a, sizeof(a), sink_rec, &r);
        dyn_frame_feed(d, b, sizeof(b), sink_rec, &r);
        CHECK(rec_frames(&r) == 2 && r.nev == 3 && r.codes[1] == DYN_FRAME_ETOOLONG && !dyn_frame_stalled(d) && dyn_frame_buffered(d) == 0,
            "resumeAfterError: cd is decoded in the same read as the error, nothing waits");
    }
    dyn_frame_free(d);
    rec_reset(&r);
    s.resume = 0;
    {
        spec_t ds = dspec("\n", NULL, 1, 3);
        for (int res = 0; res < 2; res++) {
            ds.resume = res;
            d = make_dec(&ds);
            feed_str(d, "abcdef\nok\n", &r);
            CHECK(r.nev == (res ? 2 : 1) && r.codes[0] == DYN_FRAME_ETOOLONG && dyn_frame_stalled(d) == !res,
                res ? "delimiter resumeAfterError: ok in the same read as the error" : "delimiter default: ok waits after the thrown error (Netty)");
            dyn_frame_free(d);
            rec_reset(&r);
        }
    }
    s.L.fail_fast = 1;
    s.L.stop_on_error = 1;
    d = make_dec(&s);
    {
        const uint8_t a[] = { 2, 'a', 'b', 20, 1, 2, 3 }, b[] = { 2, 'c', 'd' };
        int e1 = dyn_frame_feed(d, a, sizeof(a), sink_rec, &r);
        int e2 = dyn_frame_feed(d, b, sizeof(b), sink_rec, &r);
        CHECK(e1 == DYN_FRAME_ETOOLONG && e2 == DYN_FRAME_ETOOLONG && rec_frames_are(&r, f_ab, 1) && r.nev == 2, "stopOnError: ab, error, nothing more (fatal)");
    }
    dyn_frame_free(d);
    rec_reset(&r);
    s.L.fail_fast = 0;
    d = make_dec(&s);
    {
        const uint8_t a[] = { 2, 'a', 'b', 20, 1, 2, 3 };
        int e1 = dyn_frame_feed(d, a, sizeof(a), sink_rec, &r);
        CHECK(e1 == DYN_FRAME_ETOOLONG && r.nev == 2, "stopOnError + failFast false: stops at detection, does not wait for the discard");
    }
    dyn_frame_free(d);
    rec_reset(&r);

    s = lspec(0, 1, 0, -2, 1, 8);
    d = make_dec(&s);
    {
        const uint8_t a[] = { 0, 4, 'a', 'b' }, b[] = { 3, 'c' };
        dyn_frame_feed(d, a, sizeof(a), sink_rec, &r);
        CHECK(r.nev == 1 && r.codes[0] == DYN_FRAME_ESHORT && dyn_frame_stalled(d), "ESHORT skips the header; the rest of the read waits (Netty threw)");
        dyn_frame_feed(d, b, sizeof(b), sink_rec, &r);
        CHECK(rec_frames_are(&r, f_ab_c, 2), "next read: ab, c");
    }
    dyn_frame_free(d);
    rec_reset(&r);
    d = make_dec(&s);
    {
        const uint8_t a[] = { 0, 4, 'a', 'b' };
        dyn_frame_feed(d, a, sizeof(a), sink_rec, &r);
        dyn_frame_finish(d, sink_rec, &r);
        CHECK(rec_frames_are(&r, f_ab, 1), "end of input decodes what an error left in the cumulation (channelInputClosed)");
    }
    dyn_frame_free(d);
    rec_reset(&r);

    s = lspec(0, 1, 0, 0, 3, 64);
    for (int split = 0; split < 2; split++) {
        static const char* const f_c[] = { "c" };
        const uint8_t a[] = { 1, 'a', 3, 'x', 'y', 'c' };
        size_t cut = split ? 1 : sizeof(a);
        d = make_dec(&s);
        dyn_frame_feed(d, a, cut, sink_rec, &r);
        if (cut < sizeof(a))
            dyn_frame_feed(d, a + cut, sizeof(a) - cut, sink_rec, &r);
        CHECK(r.nev == 1 && r.codes[0] == DYN_FRAME_ESTRIP && r.vals[0] == 2 && dyn_frame_stalled(d), "ESTRIP skips the whole 2-byte frame (Netty 4.1 skipBytes(frameLengthInt)) and the read stalls");
        dyn_frame_finish(d, sink_rec, &r);
        CHECK(rec_frames_are(&r, f_c, 1), "the frame after the ESTRIP frame decodes from its own header");
        dyn_frame_free(d);
        rec_reset(&r);
    }

    s = dspec("<END>", NULL, 1, 10);
    d = make_dec(&s);
    feed_str(d, "xxxxxxxxxx<EN", &r);
    feed_str(d, "D>", &r);
    CHECK(rec_frames(&r) == 1 && r.nev == 1 && r.lens[0] == 10, "partial delimiter tail is not counted (Netty 4.1 partialDelimiterLength): max-length frame survives the split");
    dyn_frame_free(d);
    rec_reset(&r);
    d = make_dec(&s);
    feed_str(d, "xxxxxxxxxxx", &r);
    CHECK(r.nev == 1 && r.codes[0] == DYN_FRAME_ETOOLONG && r.vals[0] == 11, "11 bytes, no delimiter prefix at the tail: too long (11)");
    dyn_frame_describe(d, r.codes[0], r.vals[0], msg, sizeof(msg));
    CHECK(strcmp(msg, "frame length exceeds 10: 11 - discarded") == 0, "Netty message: %s", msg);
    feed_str(d, "yy<END>ok<END>", &r);
    CHECK(r.nev == 2 && r.lens[1] == 2, "failFast: the discard ends at the next delimiter silently, then ok");
    dyn_frame_free(d);
    rec_reset(&r);

    s = dspec("\r\n", "\n", 1, 4);
    d = make_dec(&s);
    CHECK(dyn_frame_line_mode(d), "{\\r\\n, \\n} uses LineBasedFrameDecoder (as Netty's DelimiterBasedFrameDecoder)");
    feed_str(d, "abcde\r", &r);
    CHECK(r.nev == 1 && r.codes[0] == DYN_FRAME_EOVER && r.vals[0] == 6, "line mode counts the trailing \\r: over 6");
    dyn_frame_describe(d, r.codes[0], r.vals[0], msg, sizeof(msg));
    CHECK(strcmp(msg, "frame length (over 6) exceeds the allowed maximum (4)") == 0, "LineBased message: %s", msg);
    feed_str(d, "\nok\n", &r);
    CHECK(r.nev == 2 && r.lens[1] == 2 && !dyn_frame_stalled(d), "line errors are fired, not thrown: ok in the same read");
    dyn_frame_free(d);
    rec_reset(&r);
    s = dspec("\r\n", "\n", 1, 4);
    s.dptr[2] = (const uint8_t*)"\x01";
    s.dlen[2] = 1;
    s.nd = 3;
    d = make_dec(&s);
    CHECK(!dyn_frame_line_mode(d), "a third delimiter: generic DelimiterBasedFrameDecoder");
    feed_str(d, "abcd\r", &r);
    CHECK(r.nev == 0, "generic: the \\r is a partial delimiter, not counted: 4 <= 4");
    dyn_frame_free(d);
    rec_reset(&r);

    s = lspec(0, 2, 0, 0, 0, 100);
    d = make_dec(&s);
    {
        const uint8_t a[] = { 0x7f, 0xff };
        dyn_frame_feed(d, a, 2, sink_rec, &r);
        dyn_frame_describe(d, r.codes[0], r.vals[0], msg, sizeof(msg));
        CHECK(strcmp(msg, "Adjusted frame length exceeds 100: 32769 - discarded") == 0, "LengthField message: %s", msg);
    }
    dyn_frame_free(d);
    rec_free(&r);
}

static void delim_max_boundary(void)
{
    static uint8_t S[64];
    spec_t s = dspec("<END>", NULL, 1, 10);
    size_t i;
    memset(S, 'x', 10);
    memcpy(S + 10, "<END>", 5);
    compare_all(&s, S, 15, "delim max == frame, delimiter split everywhere", 1);
    s.dhold = 1;
    s.dstop = 1;
    compare_all(&s, S, 15, "delim max == frame, option mode", 1);
    s.dhold = 0;
    s.dstop = 0;
    for (i = 1; i < 15; i++) {
        dyn_frame_t* d = make_dec(&s);
        rec_t r = { 0 };
        dyn_frame_feed(d, S, i, sink_rec, &r);
        dyn_frame_feed(d, S + i, 15 - i, sink_rec, &r);
        CHECK(r.nev == 1 && r.lens[0] == 10, "split at %zu: max-length frame survives a delimiter split across reads", i);
        dyn_frame_free(d);
        rec_free(&r);
    }
    memset(S, 'x', 11);
    memcpy(S + 11, "<END>", 5);
    {
        one_t o = decode_once(&s, S, 16);
        CHECK(o.err == DYN_FRAME_ETOOLONG && rec_frames(&o.r) == 0 && o.fatal == 0, "frame of maxFrameLength + 1 is too long (not fatal)");
        rec_free(&o.r);
    }
    compare_all(&s, S, 16, "delim max + 1", 1);
}

static int g_count;
static void sink_count(void* ud, int ev, const uint8_t* p, uint64_t n)
{
    (void)ud;
    (void)p;
    (void)n;
    if (ev == 0)
        g_count++;
}

static void stop_and_config(void)
{
    spec_t s = lspec(0, 1, 0, 0, 0, 8);
    dyn_frame_t* d;
    uint8_t ok[3] = { 2, 'a', 'b' }, bad[1] = { 200 };
    int err;
    dyn_frame_length_cfg c;
    dyn_frame_delim_cfg dc;
    const uint8_t* dp[1] = { (const uint8_t*)"x" };
    size_t dl[1] = { 0 };

    s.L.stop_on_error = 1;
    d = make_dec(&s);
    g_count = 0;
    CHECK(dyn_frame_feed(d, ok, 3, sink_count, NULL) == 0 && g_count == 1, "frame before the error is delivered");
    CHECK(dyn_frame_feed(d, ok, 2, sink_count, NULL) == 0 && dyn_frame_buffered(d) == 2, "partial frame buffered");
    CHECK(dyn_frame_feed(d, bad, 1, sink_count, NULL) == 0 && g_count == 2, "partial completes");
    CHECK(dyn_frame_feed(d, bad, 1, sink_count, NULL) == DYN_FRAME_ETOOLONG, "oversize header fails");
    CHECK(dyn_frame_feed(d, ok, 3, sink_count, NULL) == DYN_FRAME_ETOOLONG && g_count == 2, "stopOnError: no frame after it");
    CHECK(dyn_frame_buffered(d) == 0 && dyn_frame_error(d) == DYN_FRAME_ETOOLONG, "error state holds no bytes");
    dyn_frame_free(d);

    memset(&c, 0, sizeof(c));
    c.width = 3;
    c.max_frame = 10;
    d = dyn_frame_new_length(&c, &err);
    CHECK(d && err == 0, "width 3 is accepted (Netty: 1, 2, 3, 4 or 8)");
    dyn_frame_free(d);
    c.width = 5;
    CHECK(!dyn_frame_new_length(&c, &err) && err == DYN_FRAME_EINVAL, "width 5 is rejected");
    c.width = 4;
    c.offset = 7;
    CHECK(!dyn_frame_new_length(&c, &err) && err == DYN_FRAME_EINVAL, "offset + width > maxFrameLength is rejected (Netty)");
    c.offset = 6;
    d = dyn_frame_new_length(&c, &err);
    CHECK(d && err == 0, "offset + width == maxFrameLength is accepted");
    dyn_frame_free(d);
    c.offset = 0;
    c.max_frame = (((uint64_t)1) << 53) + 1;
    CHECK(!dyn_frame_new_length(&c, &err), "maxFrameLength above 2^53 is rejected");
    c.max_frame = 0;
    CHECK(!dyn_frame_new_length(&c, &err), "maxFrameLength 0 is rejected");
    c.max_frame = 10;
    c.adjustment = (int64_t)(((uint64_t)1) << 53) + 1;
    CHECK(!dyn_frame_new_length(&c, &err), "adjustment above 2^53 is rejected");
    CHECK(!dyn_frame_new_fixed(0, &err) && err == DYN_FRAME_EINVAL, "fixed 0 is rejected");
    memset(&dc, 0, sizeof(dc));
    dc.delims = dp;
    dc.lens = dl;
    dc.count = 1;
    dc.strip_delimiter = 1;
    dc.max_frame = 4;
    CHECK(!dyn_frame_new_delim(&dc, &err) && err == DYN_FRAME_EINVAL, "empty delimiter is rejected (Netty)");
    dc.count = 0;
    CHECK(!dyn_frame_new_delim(&dc, &err), "no delimiters is rejected");
    CHECK(dyn_frame_feed(NULL, ok, 1, sink_count, NULL) == DYN_FRAME_EINVAL, "NULL decoder");
    CHECK(strcmp(dyn_frame_strerror(DYN_FRAME_ENEG), "negative pre-adjustment length field") == 0, "strerror");
}

static void copy_once_big(void)
{
    spec_t s = lspec(0, 4, 0, 0, 4, 1 << 20);
    size_t n = 4 + 300000, i;
    uint8_t* S = (uint8_t*)malloc(n);
    dyn_frame_t* d = make_dec(&s);
    rec_t r = { 0 };
    put_raw(S, 4, 0, 300000);
    for (i = 4; i < n; i++)
        S[i] = (uint8_t)i;
    for (i = 0; i < n; i += 1500)
        dyn_frame_feed(d, S + i, n - i < 1500 ? n - i : 1500, sink_rec, &r);
    CHECK(r.nev == 1 && r.lens[0] == 300000 && memcmp(r.bytes, S + 4, 300000) == 0, "300 KB frame over 1500-byte segments");
    CHECK(dyn_frame_copied(d) == n, "each byte of a segmented frame copied exactly once (%llu of %zu)", (unsigned long long)dyn_frame_copied(d), n);
    dyn_frame_free(d);
    rec_free(&r);
    free(S);
}

int main(int argc, char** argv)
{
    int iters;
    fk_init("test_frame_decoder");
    iters = argc > 1 ? atoi(argv[1]) : (int)fk_iters(400);
    g_rng = (argc > 2 ? strtoull(argv[2], NULL, 10) : fk_seed ^ 0x9E3779B97F4A7C15ull) | 1;
    netty_javadoc_examples();
    length_edges();
    width8_edges();
    delim_every_split();
    delim_semantics();
    default_vs_options();
    delim_max_boundary();
    stop_and_config();
    copy_once_big();
    fuzz_length(iters);
    fuzz_delim(iters);
    fuzz_fixed(iters / 2);
    printf("test_frame_decoder: %d checks, %d failed (netty-oracle %d, invariant-oracle %d comparisons; %d hold-only runs checked for memory safety and copy-once only)\n", g_checks, g_fail,
        g_netty_cmp, g_inv_cmp, g_hold_only);
    return g_fail ? 1 : 0;
}
