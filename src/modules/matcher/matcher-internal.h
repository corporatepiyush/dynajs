#ifndef MATCHER_INTERNAL_H
#define MATCHER_INTERNAL_H

#include "dyna-nat.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_MATCHER)

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "core/dyn-simdk.h"
#include "core/dyn-ac.h"
#include "core/dyn-hash.h"
#include "core/dyn-textdiff.h"

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

#define DYN_MATCH_MAX_SCAN_STEPS (1000ULL * 1000ULL * 1000ULL)

size_t dyn_m_units(const uint8_t* p, size_t nbytes);

typedef struct {
    const uint8_t* data;
    size_t len;
    const char* owned;
    JSValue str;
    int direct;
    size_t cur_byte;
    size_t cur_units;
} dyn_m_text_t;

int dyn_m_text(JSContext* ctx, JSValueConst v, dyn_m_text_t* t,
    int ascii_pattern);

static inline void dyn_m_text_free(JSContext* ctx, dyn_m_text_t* t)
{
    if (t->owned)
        JS_FreeCString(ctx, t->owned);
    JS_FreeValue(ctx, t->str);
}

static inline int64_t dyn_m_off(dyn_m_text_t* t, size_t byte_off)
{
    if (t->direct)
        return (int64_t)byte_off;
    if (byte_off >= t->cur_byte)
        t->cur_units += dyn_m_units(t->data + t->cur_byte, byte_off - t->cur_byte);
    else
        t->cur_units -= dyn_m_units(t->data + byte_off, t->cur_byte - byte_off);
    t->cur_byte = byte_off;
    return (int64_t)t->cur_units;
}

static inline JSValue dyn_matcher_budget_error(JSContext* ctx)
{
    return JS_ThrowRangeError(ctx,
        "overlapping scan exceeds its %u byte-step budget; the pattern "
        "matches too densely to count or enumerate",
        (unsigned)DYN_MATCH_MAX_SCAN_STEPS);
}

size_t dyn_porter2(const uint8_t* in, size_t n, uint8_t* out);
JSValue dyn_matcher_stem(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv);

extern JSClassID dyn_ac_class_id;

extern const JSClassDef dyn_ac_class;

JSValue dyn_ac_ctor(JSContext* ctx, JSValueConst new_target, int argc,
    JSValueConst* argv);

typedef struct {
    char* buf;
    size_t len, cap;
} dyn_m_buf_t;

int dyn_m_buf_put(dyn_m_buf_t* b, const char* p, size_t n);

extern const JSCFunctionListEntry dyn_ac_proto[8];

#ifdef DYN_APPROX_REFERENCE
static size_t dyn_lev_reference(const uint32_t* A, size_t n, const uint32_t* B,
    size_t m, size_t* scratch)
{
    size_t i, j;
    for (j = 0; j <= m; j++)
        scratch[j] = j;
    for (i = 1; i <= n; i++) {
        size_t diag = scratch[0];
        scratch[0] = i;
        for (j = 1; j <= m; j++) {
            size_t up = scratch[j], v;
            size_t sub = diag + (A[i - 1] != B[j - 1]);
            v = (scratch[j - 1] + 1 < up + 1) ? scratch[j - 1] + 1 : up + 1;
            scratch[j] = (v < sub) ? v : sub;
            diag = up;
        }
    }
    return scratch[m];
}
#endif

typedef struct {
    const char* cs;
    const uint8_t* u8;
    uint32_t* cp;
    size_t n;
    size_t bytes;
    int ascii;
} dyn_operand_t;

void dyn_operand_free(JSContext* ctx, dyn_operand_t* o);

int dyn_operand_get(JSContext* ctx, JSValueConst v, const char* what,
    dyn_operand_t* o);

int dyn_operand_widen(JSContext* ctx, dyn_operand_t* o);

JSValue dyn_levenshtein(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_smith_waterman(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

#ifdef DYN_APPROX_REFERENCE
static JSValue dyn_levenshtein_ref(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_operand_t A, B;
    size_t* scratch = NULL;
    JSValue ret = JS_EXCEPTION;

    A.cs = NULL;
    A.cp = NULL;
    B.cs = NULL;
    B.cp = NULL;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "LevenshteinReference(a, b): two strings required");
    if (dyn_operand_get(ctx, argv[0], "a", &A) < 0)
        return JS_EXCEPTION;
    if (dyn_operand_get(ctx, argv[1], "b", &B) < 0)
        goto done;
    if (dyn_operand_widen(ctx, &A) < 0 || dyn_operand_widen(ctx, &B) < 0)
        goto done;
    scratch = (size_t*)malloc((B.n + 1) * sizeof(size_t));
    if (!scratch) {
        JS_ThrowOutOfMemory(ctx);
        goto done;
    }
    ret = JS_NewInt64(ctx, (int64_t)dyn_lev_reference(A.cp, A.n, B.cp, B.n, scratch));
done:
    free(scratch);
    dyn_operand_free(ctx, &A);
    dyn_operand_free(ctx, &B);
    return ret;
}
#endif

JSValue dyn_dice(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_jaro_winkler(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_damerau_levenshtein(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

enum { DYN_TOK_LINES = DYN_TD_LINES,
    DYN_TOK_WORDS = DYN_TD_WORDS,
    DYN_TOK_CHARS = DYN_TD_CHARS };

JSValue dyn_unified_diff(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_apply_patch(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_diff(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic);

#endif

#endif
