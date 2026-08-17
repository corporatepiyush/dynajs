/*
 * dyna:matcher -- substring and multi-pattern search.
 *
 * `Matcher` borrows the subject's narrow bytes when the PATTERN is pure ASCII,
 * decided once at construction: no Latin-1/UTF-8 mismatch is possible, and no
 * per-search scan or copy. Searching routes through simd.strfind.
 * `MultiMatcher` (Aho-Corasick) wins above ~36 patterns; below that, N calls
 * to String.prototype.indexOf are faster.
 * Full API: see the dyna:* module in dyna-libc.h.
 */
#include "dyna-nat.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_MATCHER)

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "dyna-simd-kernels.h"
#include "core/dyn-ac.h"
#include "core/dyn-hash.h"

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

/* ---------- UTF-8 byte offset -> UTF-16 code-unit offset ------------------- */

/* Number of code units in the UTF-8 prefix `p[0..nbytes)`. Only called when the
 * text is known to contain a byte >= 0x80; ASCII skips it entirely. */
static size_t dyn_m_units(const uint8_t *p, size_t nbytes)
{
    size_t i = 0, units = 0;
    while (i < nbytes) {
        uint8_t c = p[i];
        if (c < 0x80)       { i += 1; units += 1; }
        else if (c < 0xE0)  { i += 2; units += 1; }
        else if (c < 0xF0)  { i += 3; units += 1; }
        else                { i += 4; units += 2; }  /* a surrogate pair */
    }
    return units;
}

/* One text argument, ready to scan, and the decision of whether offsets need
 * translating at all.
 *
 * THE FAST PATH IS THE ONLY ONE THAT MATTERS HERE, and it is not an
 * optimisation -- it is the difference between this API costing nothing and
 * costing more than the function it replaces. JS_ToCStringLen SCANS the whole
 * string to count non-ASCII bytes, then ALLOCATES and COPIES it to produce
 * UTF-8. On a 40 KB haystack that is 40 KB of scan, malloc and copy on every
 * single call -- while String.prototype.indexOf searches the engine's own
 * bytes in place. Measured: it made Matcher 1.62x SLOWER than the indexOf it
 * exists to improve on.
 *
 * The bypass has to be narrower than "the string is narrow", and the test
 * suite said so: a narrow string is LATIN-1, which is not UTF-8 above U+007F.
 * "eee" with acutes is three 0xE9 bytes in the engine and six bytes as UTF-8,
 * so searching one representation for the other finds nothing -- silently,
 * because a miss is a legitimate answer.
 *
 * The condition is on the PATTERN, not the text, and it costs nothing to
 * check because it is checked ONCE at construction -- which is the one thing a
 * compiled capability is unambiguously for.
 *
 *   A PURE-ASCII PATTERN CAN BE SEARCHED DIRECTLY IN LATIN-1 BYTES.
 *
 * Every pattern byte is below 0x80 and every non-ASCII Latin-1 byte is at or
 * above it, so no false match is possible; and the pattern's characters occupy
 * exactly the code-unit positions its bytes do, so no match is missed. Byte
 * offset is code-unit offset. No text scan, no malloc, no copy, no
 * translation.
 *
 * Scanning the TEXT for ASCII-ness was the obvious version of this and it was
 * measured at 2.70x -- the scan costs as much as the search it guards. The
 * cheapest test is the one that was already free. */
typedef struct {
    const uint8_t *data;
    size_t len;
    const char *owned;  /* non-NULL when the UTF-8 copy must be released */
    JSValue str;        /* holds the string alive while `data` is borrowed */
    int direct;         /* 1 => byte offset == code-unit offset, no translation */
} dyn_m_text_t;

static int dyn_m_text(JSContext *ctx, JSValueConst v, dyn_m_text_t *t,
                      int ascii_pattern)
{
    JSValue s = JS_ToString(ctx, v), lv;
    int64_t units;

    if (JS_IsException(s))
        return -1;
    t->data = ascii_pattern ? JS_GetNarrowStringBytes(ctx, s, &t->len) : NULL;
    if (t->data) {
        /* borrowed: valid until JS runs, and nothing between here and the scan
         * runs any. The JSValue is freed after the scan, not here. */
        t->owned = NULL;
        t->direct = 1;
        t->str = s;
        return 0;
    }
    t->data = NULL;
    /* wide, or a non-ASCII pattern: fall back to UTF-8, and translate offsets unless it is pure ASCII
     * (which a wide string can still be if it was never narrowed) */
    lv = JS_GetPropertyStr(ctx, s, "length");
    if (JS_IsException(lv) || JS_ToInt64(ctx, &units, lv)) {
        JS_FreeValue(ctx, lv);
        JS_FreeValue(ctx, s);
        return -1;
    }
    JS_FreeValue(ctx, lv);
    t->owned = JS_ToCStringLen(ctx, &t->len, s);
    if (!t->owned) {
        JS_FreeValue(ctx, s);
        return -1;
    }
    t->data = (const uint8_t *)t->owned;
    t->direct = (t->len == (size_t)units);
    t->str = s;
    return 0;
}

static void dyn_m_text_free(JSContext *ctx, dyn_m_text_t *t)
{
    if (t->owned)
        JS_FreeCString(ctx, t->owned);
    JS_FreeValue(ctx, t->str);
}

static int64_t dyn_m_off(const dyn_m_text_t *t, size_t byte_off)
{
    if (t->direct)
        return (int64_t)byte_off;
    return (int64_t)dyn_m_units(t->data, byte_off);
}

/* =====================================================================
 * Matcher -- one compiled pattern.
 * ===================================================================== */

enum { MATCH_KMP, MATCH_BMH };

/* A Matcher holds the pattern and NOTHING ELSE.
 *
 * It used to carry a KMP failure function and a 2 KB bad-character table, both
 * built in the constructor and NEITHER EVER READ: the search has gone through
 * simd.strfind since the table was measured at 14.7-30.8x slower than the
 * kernel, and the preprocessing was left behind. Deleting it took construction
 * of a 1024-byte pattern from 2212 ns to 119 ns and moved the crossover from
 * "never" to N=100. `algo` is still parsed and validated -- an unknown value is
 * a caller's mistake worth reporting -- and reported back by the getter. */
typedef struct {
    uint8_t *pat;
    size_t plen;
    int algo;
    int ascii;          /* every pattern byte < 0x80: searchable in Latin-1 */
} dyn_matcher_t;

static void dyn_matcher_free(void *native)
{
    dyn_matcher_t *m = (dyn_matcher_t *)native;
    if (!m)
        return;
    free(m->pat);
    free(m);
}

static JSClassID dyn_matcher_class_id;
static void dyn_matcher_finalizer(JSRuntime *rt, JSValue val)
{
    (void)rt;
    dyn_matcher_free(JS_GetOpaque(val, dyn_matcher_class_id));
}
static const JSClassDef dyn_matcher_class = {
    "Matcher", .finalizer = dyn_matcher_finalizer,
};

static JSValue dyn_matcher_ctor(JSContext *ctx, JSValueConst new_target,
                                int argc, JSValueConst *argv)
{
    dyn_matcher_t *m;
    const char *pat;
    size_t plen;
    int algo = MATCH_KMP;

    (void)new_target;
    if (argc >= 2 && JS_IsObject(argv[1])) {
        JSValue av = JS_GetPropertyStr(ctx, argv[1], "algo");
        if (JS_IsException(av))
            return JS_EXCEPTION;
        if (!JS_IsUndefined(av)) {
            const char *s = JS_ToCString(ctx, av);
            JS_FreeValue(ctx, av);
            if (!s)
                return JS_EXCEPTION;
            if (!strcmp(s, "kmp")) algo = MATCH_KMP;
            else if (!strcmp(s, "bmh") || !strcmp(s, "boyer-moore")) algo = MATCH_BMH;
            else {
                JS_FreeCString(ctx, s);
                return JS_ThrowRangeError(ctx, "algo must be \"kmp\" or \"bmh\"");
            }
            JS_FreeCString(ctx, s);
        } else {
            JS_FreeValue(ctx, av);
        }
    }
    pat = JS_ToCStringLen(ctx, &plen, argv[0]);
    if (!pat)
        return JS_EXCEPTION;
    m = (dyn_matcher_t *)malloc(sizeof(*m));
    if (!m) {
        JS_FreeCString(ctx, pat);
        return JS_ThrowOutOfMemory(ctx);
    }
    m->plen = plen;
    m->algo = algo;
    m->pat = (uint8_t *)malloc(plen ? plen : 1);
    if (!m->pat) {
        free(m);
        JS_FreeCString(ctx, pat);
        return JS_ThrowOutOfMemory(ctx);
    }
    memcpy(m->pat, pat, plen);
    JS_FreeCString(ctx, pat);
    m->ascii = 1;
    for (size_t k = 0; k < plen; k++)
        if (m->pat[k] & 0x80) { m->ascii = 0; break; }
    return dyn_plain_wrap(ctx, dyn_matcher_class_id, m, dyn_matcher_free);
}

/* Find the pattern at or after `start` (byte offsets), or SIZE_MAX.
 *
 * THE SIMD KERNEL, not a compiled table, because the table loses -- badly.
 * Measured on arm64: simd.strfind runs at 6.8-15.0 GB/s against scalar KMP's
 * 463-493 MB/s, and the gap WIDENS with text size, so the "one pattern, many
 * long texts" case the class was built for is the one it loses hardest. */
static size_t matcher_find(const dyn_matcher_t *m, const uint8_t *t, size_t tlen,
                           size_t start)
{
    size_t plen = m->plen, rel;
    if (start > tlen || plen > tlen - start)
        return SIZE_MAX;
    if (plen == 0)
        return start;
    rel = simd.strfind(t + start, tlen - start, m->pat, plen);
    return rel == SIZE_MAX ? SIZE_MAX : start + rel;
}

static dyn_matcher_t *matcher_of(JSContext *ctx, JSValueConst t)
{
    return (dyn_matcher_t *)dyn_plain_get(ctx, t, dyn_matcher_class_id);
}

/* firstIn / test / countIn (magic 0/1/2): one scan, three answers. */
static JSValue dyn_matcher_scan(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv, int magic)
{
    dyn_matcher_t *m;
    dyn_m_text_t t;
    size_t pos;
    int64_t count = 0, first = -1;
    (void)argc;

    m = matcher_of(ctx, this_val);
    if (!m)
        return JS_EXCEPTION;
    if (dyn_m_text(ctx, argv[0], &t, m->ascii))
        return JS_EXCEPTION;
    if (m->plen == 0) {
        dyn_m_text_free(ctx, &t);
        /* an empty pattern occurs at position 0 and nowhere else worth
         * enumerating -- countIn stays 0 rather than reporting length+1 */
        return magic == 0 ? JS_NewInt32(ctx, 0)
                          : (magic == 1 ? JS_TRUE : JS_NewInt32(ctx, 0));
    }
    pos = matcher_find(m, t.data, t.len, 0);
    if (pos != SIZE_MAX) {
        first = dyn_m_off(&t, pos);
        if (magic == 2) {
            while (pos != SIZE_MAX) {
                count++;
                pos = matcher_find(m, t.data, t.len, pos + 1);
            }
        }
    }
    dyn_m_text_free(ctx, &t);
    if (magic == 0)
        return JS_NewInt64(ctx, first);
    if (magic == 1)
        return JS_NewBool(ctx, first >= 0);
    return JS_NewInt64(ctx, count);
}

static JSValue dyn_matcher_all_in(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    dyn_matcher_t *m;
    dyn_m_text_t t;
    size_t pos;
    JSValue arr;
    uint32_t count = 0;
    (void)argc;

    m = matcher_of(ctx, this_val);
    if (!m)
        return JS_EXCEPTION;
    if (dyn_m_text(ctx, argv[0], &t, m->ascii))
        return JS_EXCEPTION;
    arr = JS_NewArray(ctx);
    if (JS_IsException(arr)) {
        dyn_m_text_free(ctx, &t);
        return arr;
    }
    if (m->plen > 0) {
        pos = matcher_find(m, t.data, t.len, 0);
        while (pos != SIZE_MAX) {
            if (JS_DefinePropertyValueUint32(ctx, arr, count++,
                    JS_NewInt64(ctx, dyn_m_off(&t, pos)), JS_PROP_C_W_E) < 0) {
                dyn_m_text_free(ctx, &t);
                JS_FreeValue(ctx, arr);
                return JS_EXCEPTION;
            }
            pos = matcher_find(m, t.data, t.len, pos + 1);
        }
    }
    dyn_m_text_free(ctx, &t);
    return arr;
}

/* replaceAllIn(text, repl): NON-overlapping, left to right, which is the only
 * sane rule once bytes are being removed -- allIn's overlapping matches cannot
 * all be replaced. */
static JSValue dyn_matcher_replace_all_in(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv)
{
    dyn_matcher_t *m;
    const char *text, *repl;
    size_t tlen, rlen, pos, last = 0, cap, used = 0;
    char *buf;
    JSValue out;
    (void)argc;

    /* both arguments coerced before the handle is resolved */
    text = JS_ToCStringLen(ctx, &tlen, argv[0]);
    if (!text)
        return JS_EXCEPTION;
    repl = JS_ToCStringLen(ctx, &rlen, argv[1]);
    if (!repl) {
        JS_FreeCString(ctx, text);
        return JS_EXCEPTION;
    }
    m = matcher_of(ctx, this_val);
    if (!m) {
        JS_FreeCString(ctx, text);
        JS_FreeCString(ctx, repl);
        return JS_EXCEPTION;
    }
    if (m->plen == 0) {
        out = JS_NewStringLen(ctx, text, tlen);
        JS_FreeCString(ctx, text);
        JS_FreeCString(ctx, repl);
        return out;
    }
    /* Count the matches first, so the output is allocated ONCE at exactly the
     * right size. A growable buffer would reallocate through the whole string
     * for a replacement that is longer than the pattern, and the count is one
     * extra pass over a kernel that runs at 15 GB/s. */
    {
        size_t nmatch = 0;
        pos = matcher_find(m, (const uint8_t *)text, tlen, 0);
        while (pos != SIZE_MAX) {
            nmatch++;
            pos = matcher_find(m, (const uint8_t *)text, tlen, pos + m->plen);
        }
        if (nmatch && rlen > m->plen &&
            nmatch > (SIZE_MAX - tlen) / (rlen - m->plen)) {
            JS_FreeCString(ctx, text);
            JS_FreeCString(ctx, repl);
            return JS_ThrowRangeError(ctx, "replacement result is too large");
        }
        cap = tlen + (rlen > m->plen ? nmatch * (rlen - m->plen) : 0);
    }
    buf = (char *)malloc(cap ? cap : 1);
    if (!buf) {
        JS_FreeCString(ctx, text);
        JS_FreeCString(ctx, repl);
        return JS_ThrowOutOfMemory(ctx);
    }
    pos = matcher_find(m, (const uint8_t *)text, tlen, 0);
    while (pos != SIZE_MAX) {
        memcpy(buf + used, text + last, pos - last);
        used += pos - last;
        memcpy(buf + used, repl, rlen);
        used += rlen;
        last = pos + m->plen;
        pos = matcher_find(m, (const uint8_t *)text, tlen, last);
    }
    memcpy(buf + used, text + last, tlen - last);
    used += tlen - last;
    out = JS_NewStringLen(ctx, buf, used);
    free(buf);
    JS_FreeCString(ctx, text);
    JS_FreeCString(ctx, repl);
    return out;
}

static JSValue dyn_matcher_length(JSContext *ctx, JSValueConst this_val)
{
    dyn_matcher_t *m = matcher_of(ctx, this_val);
    if (!m)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)m->plen);
}

static JSValue dyn_matcher_algo(JSContext *ctx, JSValueConst this_val)
{
    dyn_matcher_t *m = matcher_of(ctx, this_val);
    if (!m)
        return JS_EXCEPTION;
    return JS_NewString(ctx, m->algo == MATCH_BMH ? "bmh" : "kmp");
}

static const JSCFunctionListEntry dyn_matcher_proto[] = {
    JS_CFUNC_MAGIC_DEF("firstIn", 1, dyn_matcher_scan, 0),
    JS_CFUNC_MAGIC_DEF("test", 1, dyn_matcher_scan, 1),
    JS_CFUNC_MAGIC_DEF("countIn", 1, dyn_matcher_scan, 2),
    JS_CFUNC_DEF("allIn", 1, dyn_matcher_all_in),
    JS_CFUNC_DEF("replaceAllIn", 2, dyn_matcher_replace_all_in),
    JS_CGETSET_DEF("length", dyn_matcher_length, NULL),
    JS_CGETSET_DEF("algo", dyn_matcher_algo, NULL),
};

/* =====================================================================
 * MultiMatcher -- Aho-Corasick over N patterns.
 *
 * THIS is the compiled capability `Matcher` only looks like. Finding N patterns
 * with N separate searches costs N passes over the text; the automaton finds
 * all of them in ONE, and the cost of a pass does not grow with N. The
 * preprocessing it amortises is real (a goto/fail/output automaton over the
 * pattern set), unlike the table Matcher was carrying.
 *
 * The automaton is a byte trie in a flat array. Each state holds 256 goto
 * slots, which is 1 KiB per state and the reason this is built once and reused
 * rather than per call. `fail` is the standard suffix link; `out` is the index
 * of a pattern ending at this state, or -1, and `out_link` chains to the next
 * shorter pattern that also ends here -- so "he"/"she"/"hers" all report at the
 * positions they end, without a per-state list allocation.
 * ===================================================================== */

/* The automaton itself now lives in src/core/dyn-ac.{c,h}: it was already
 * pure C here -- only the constructor and the scan wrappers ever touched a
 * JSValue -- so this is a lift, not a rewrite. The dictionary codec in
 * dyn-compress is the second consumer, and it needs the automaton from C. */

static JSClassID dyn_ac_class_id;

/* dyn_ac_free takes a dyn_ac_t*; the resource helpers want a void(*)(void*).
 * An adapter, not a cast: calling a function pointer through a different type
 * is UB and traps under -fsanitize=function (CLAUDE.md sec.1). */
static void dyn_ac_free_v(void *native)
{
    dyn_ac_free((dyn_ac_t *)native);
}

static void dyn_ac_finalizer(JSRuntime *rt, JSValue val)
{
    (void)rt;
    dyn_ac_free_v(JS_GetOpaque(val, dyn_ac_class_id));
}
static const JSClassDef dyn_ac_class = {
    "MultiMatcher", .finalizer = dyn_ac_finalizer,
};

static JSValue dyn_ac_ctor(JSContext *ctx, JSValueConst new_target, int argc,
                           JSValueConst *argv)
{
    dyn_ac_t *a;
    int64_t n, i;
    uint8_t **pats = NULL;
    size_t *lens = NULL;

    (void)new_target;
    if (argc < 1 || !JS_IsArray(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "new MultiMatcher(patterns[])");
    {
        JSValue lv = JS_GetPropertyStr(ctx, argv[0], "length");
        if (JS_IsException(lv))
            return JS_EXCEPTION;
        if (JS_ToInt64(ctx, &n, lv)) {
            JS_FreeValue(ctx, lv);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, lv);
    }
    if (n < 1)
        return JS_ThrowRangeError(ctx, "at least one pattern is required");
    if (n > 65536)
        return JS_ThrowRangeError(ctx, "at most 65536 patterns");

    /* Every pattern is materialised to libc memory FIRST -- reading element i
     * can run a getter, which must not be able to observe a half-built
     * automaton or free one out from under the build. */
    pats = (uint8_t **)calloc((size_t)n, sizeof(*pats));
    lens = (size_t *)calloc((size_t)n, sizeof(*lens));
    if (!pats || !lens) {
        free(pats);
        free(lens);
        return JS_ThrowOutOfMemory(ctx);
    }
    for (i = 0; i < n; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, argv[0], (uint32_t)i);
        const char *s;
        size_t sl;
        if (JS_IsException(e))
            goto fail_pats;
        s = JS_ToCStringLen(ctx, &sl, e);
        JS_FreeValue(ctx, e);
        if (!s)
            goto fail_pats;
        if (sl == 0) {
            JS_FreeCString(ctx, s);
            JS_ThrowRangeError(ctx, "an empty pattern matches everywhere and "
                                    "nowhere; it is not a pattern");
            goto fail_pats;
        }
        pats[i] = (uint8_t *)malloc(sl);
        if (!pats[i]) {
            JS_FreeCString(ctx, s);
            JS_ThrowOutOfMemory(ctx);
            goto fail_pats;
        }
        memcpy(pats[i], s, sl);
        lens[i] = sl;
        JS_FreeCString(ctx, s);
    }

    a = dyn_ac_new((size_t)n);
    if (!a) {
        JS_ThrowOutOfMemory(ctx);
        goto fail_pats;
    }
    /* dyn_ac_insert records each pattern's length and clears `ascii` on any
     * byte >= 0x80, so the separate length table and the separate ASCII sweep
     * this constructor used to run are both gone -- one pass, one owner. */
    for (i = 0; i < n; i++) {
        if (dyn_ac_insert(a, pats[i], lens[i], (int)i) < 0) {
            dyn_ac_free(a);
            JS_ThrowOutOfMemory(ctx);
            goto fail_pats;
        }
    }
    if (dyn_ac_build(a) < 0) {
        dyn_ac_free(a);
        JS_ThrowOutOfMemory(ctx);
        goto fail_pats;
    }
    for (i = 0; i < n; i++)
        free(pats[i]);
    free(pats);
    /* `lens` is no longer donated to the automaton -- dyn_ac_new owns its own
     * length table and dyn_ac_insert fills it -- so this scratch array is the
     * constructor's to free on the success path as well as the failure one. */
    free(lens);
    return dyn_plain_wrap(ctx, dyn_ac_class_id, a, dyn_ac_free_v);

fail_pats:
    if (pats) {
        for (i = 0; i < n; i++)
            free(pats[i]);
        free(pats);
    }
    free(lens);
    return JS_EXCEPTION;
}

static dyn_ac_t *ac_of(JSContext *ctx, JSValueConst t)
{
    return (dyn_ac_t *)dyn_plain_get(ctx, t, dyn_ac_class_id);
}

typedef struct {
    JSContext *ctx;
    const dyn_ac_t *a;
    const dyn_m_text_t *t;
    JSValue arr;
    int64_t n;
    int64_t first_at;
    uint32_t count;
    int first_pat;
    int failed;
} dyn_ac_sink_t;

static int dyn_ac_emit_first(void *ud, int pat, size_t end_byte)
{
    dyn_ac_sink_t *s = (dyn_ac_sink_t *)ud;
    s->first_pat = pat;
    s->first_at = dyn_m_off(s->t, end_byte - s->a->plen[pat]);
    return 1;                                  /* stop at the first hit */
}

static int dyn_ac_emit_count(void *ud, int pat, size_t end_byte)
{
    dyn_ac_sink_t *s = (dyn_ac_sink_t *)ud;
    (void)pat; (void)end_byte;
    s->n++;
    return 0;
}

static int dyn_ac_emit_all(void *ud, int pat, size_t end_byte)
{
    dyn_ac_sink_t *s = (dyn_ac_sink_t *)ud;
    JSValue hit = JS_NewObject(s->ctx);
    if (JS_IsException(hit)) {
        s->failed = 1;
        return 1;
    }
    if (JS_DefinePropertyValueStr(s->ctx, hit, "index",
            JS_NewInt32(s->ctx, pat), JS_PROP_C_W_E) < 0 ||
        JS_DefinePropertyValueStr(s->ctx, hit, "at",
            JS_NewInt64(s->ctx, dyn_m_off(s->t, end_byte - s->a->plen[pat])),
            JS_PROP_C_W_E) < 0 ||
        JS_DefinePropertyValueUint32(s->ctx, s->arr, s->count++, hit,
                                     JS_PROP_C_W_E) < 0) {
        s->failed = 1;
        return 1;
    }
    return 0;
}

/* firstIn / test / countIn / allIn (magic 0/1/2/3) -- ONE pass each. */
static JSValue dyn_ac_scan(JSContext *ctx, JSValueConst this_val, int argc,
                           JSValueConst *argv, int magic)
{
    dyn_ac_t *a;
    dyn_m_text_t t;
    dyn_ac_sink_t sink;
    JSValue ret;
    (void)argc;

    a = ac_of(ctx, this_val);
    if (!a)
        return JS_EXCEPTION;
    if (dyn_m_text(ctx, argv[0], &t, a->ascii))
        return JS_EXCEPTION;
    memset(&sink, 0, sizeof(sink));
    sink.ctx = ctx;
    sink.a = a;
    sink.t = &t;
    sink.first_pat = -1;
    sink.first_at = -1;
    sink.arr = JS_UNDEFINED;

    if (magic == 3) {
        sink.arr = JS_NewArray(ctx);
        if (JS_IsException(sink.arr)) {
            dyn_m_text_free(ctx, &t);
            return JS_EXCEPTION;
        }
        dyn_ac_run(a, t.data, t.len, dyn_ac_emit_all, &sink);
        dyn_m_text_free(ctx, &t);
        if (sink.failed) {
            JS_FreeValue(ctx, sink.arr);
            return JS_EXCEPTION;
        }
        return sink.arr;
    }
    if (magic == 2) {
        dyn_ac_run(a, t.data, t.len, dyn_ac_emit_count, &sink);
        dyn_m_text_free(ctx, &t);
        return JS_NewInt64(ctx, sink.n);
    }
    dyn_ac_run(a, t.data, t.len, dyn_ac_emit_first, &sink);
    dyn_m_text_free(ctx, &t);
    if (magic == 1)
        return JS_NewBool(ctx, sink.first_pat >= 0);
    if (sink.first_pat < 0)
        return JS_NULL;
    ret = JS_NewObject(ctx);
    if (JS_IsException(ret))
        return ret;
    if (JS_DefinePropertyValueStr(ctx, ret, "index",
            JS_NewInt32(ctx, sink.first_pat), JS_PROP_C_W_E) < 0 ||
        JS_DefinePropertyValueStr(ctx, ret, "at",
            JS_NewInt64(ctx, sink.first_at), JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, ret);
        return JS_EXCEPTION;
    }
    return ret;
}

static JSValue dyn_ac_size(JSContext *ctx, JSValueConst this_val)
{
    dyn_ac_t *a = ac_of(ctx, this_val);
    if (!a)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)a->n_pat);
}

static JSValue dyn_ac_states(JSContext *ctx, JSValueConst this_val)
{
    dyn_ac_t *a = ac_of(ctx, this_val);
    if (!a)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)a->n_states);
}

static const JSCFunctionListEntry dyn_ac_proto[] = {
    JS_CFUNC_MAGIC_DEF("firstIn", 1, dyn_ac_scan, 0),
    JS_CFUNC_MAGIC_DEF("test", 1, dyn_ac_scan, 1),
    JS_CFUNC_MAGIC_DEF("countIn", 1, dyn_ac_scan, 2),
    JS_CFUNC_MAGIC_DEF("allIn", 1, dyn_ac_scan, 3),
    JS_CGETSET_DEF("size", dyn_ac_size, NULL),
    JS_CGETSET_DEF("states", dyn_ac_states, NULL),
};

/* Approximate matching (edit distance, bigram similarity) -- same TU so the
 * kernels inline into the entry points. */
#include "dyna-approx.inc.c"
#include "dyna-diff.inc.c"

/* ---------- module registration -------------------------------------------- */

static const JSCFunctionListEntry dyn_matcher_funcs[] = {
    JS_CFUNC_DEF("Levenshtein", 2, dyn_levenshtein),
    JS_CFUNC_DEF("DiceCoefficient", 2, dyn_dice),
    JS_CFUNC_MAGIC_DEF("DiffLines", 2, dyn_diff, DYN_TOK_LINES),
    JS_CFUNC_MAGIC_DEF("DiffWords", 2, dyn_diff, DYN_TOK_WORDS),
    JS_CFUNC_MAGIC_DEF("DiffChars", 2, dyn_diff, DYN_TOK_CHARS),
#ifdef DYN_APPROX_REFERENCE
    JS_CFUNC_DEF("LevenshteinReference", 2, dyn_levenshtein_ref),
#endif
};

static int dyn_matcher_init_module(JSContext *ctx, JSModuleDef *m)
{
    if (dyn_register_plain_class(ctx, m, &dyn_matcher_class_id,
                                 &dyn_matcher_class, dyn_matcher_proto,
                                 countof(dyn_matcher_proto), dyn_matcher_ctor,
                                 "Matcher") < 0)
        return -1;
    if (dyn_register_plain_class(ctx, m, &dyn_ac_class_id, &dyn_ac_class,
                                 dyn_ac_proto, countof(dyn_ac_proto),
                                 dyn_ac_ctor, "MultiMatcher") < 0)
        return -1;
    return JS_SetModuleExportList(ctx, m, dyn_matcher_funcs,
                                  countof(dyn_matcher_funcs));
}

int js_nat_init_matcher(JSContext *ctx)
{
    JSModuleDef *m = JS_NewCModule(ctx, "dyna:matcher", dyn_matcher_init_module);
    if (!m)
        return -1;
    JS_AddModuleExport(ctx, m, "Matcher");
    JS_AddModuleExport(ctx, m, "MultiMatcher");
    return JS_AddModuleExportList(ctx, m, dyn_matcher_funcs,
                                  countof(dyn_matcher_funcs));
}

#endif /* CONFIG_NATIVE_MODULES && CONFIG_NATIVE_MODULE_MATCHER */
