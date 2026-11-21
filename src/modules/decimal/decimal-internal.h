#ifndef DECIMAL_INTERNAL_H
#define DECIMAL_INTERNAL_H

#include "dyna-nat.h"
#include "cutils.h"
#include "core/dyn-decnum.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_DECIMAL)

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static inline JSValue dyn_dec_too_long(JSContext* ctx)
{
    return JS_ThrowRangeError(ctx,
        "decimal: positional text would exceed %d characters; the exponent is "
        "too large to render",
        DEC_MAX_TEXT);
}

extern JSClassID dyn_dec_class_id;

void dyn_dec_dispose(void* p);

int dyn_dec_opts_strict(JSContext* ctx, JSValueConst o);

int dyn_rnd_mode(JSContext* ctx, JSValueConst o, int* mode);

int dyn_prec(JSContext* ctx, JSValueConst o, uint32_t* prec);

static inline dec_t* dyn_dec_alloc(JSContext* ctx)
{
    dec_t* x = (dec_t*)dyn_nat_malloc(sizeof *x);
    if (!x) {
        JS_ThrowOutOfMemory(ctx);
        return NULL;
    }
    dec_init(x);
    return x;
}

static inline JSValue dyn_dec_wrap(JSContext* ctx, JSValueConst new_target,
    dec_t* x)
{
    return dyn_plain_wrap(ctx, new_target, dyn_dec_class_id, x, dyn_dec_dispose);
}

int dyn_dec_from(JSContext* ctx, JSValueConst v, dec_t* out);

static inline dec_t* dyn_dec_this(JSContext* ctx, JSValueConst t)
{
    dec_t* x = (dec_t*)JS_GetOpaque2(ctx, t, dyn_dec_class_id);
    return x;
}

enum { OP_ADD,
    OP_SUB,
    OP_MUL,
    OP_DIV,
    OP_MOD };

JSValue dyn_dec_arith(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic);

JSValue dyn_dec_round(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic);

JSValue dyn_dec_pow(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_dec_math(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic);

JSValue dyn_dec_divmod(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

extern JSClassID dyn_money_class_id;

void dyn_money_finalizer(JSRuntime* rt, JSValue val);

JSValue dyn_money_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv);

JSValue dyn_money_op(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic);

JSValue dyn_money_mul(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_money_allocate(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_money_query(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic);

JSValue dyn_money_from_string(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_money_from_decimal(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_money_from_minor(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

#endif

#endif
