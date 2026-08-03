/*
 * dyna:net -- RateLimiter, a token bucket over a FIXED, direct-mapped table.
 *
 * The table cannot grow. That is the security property, not a limitation: a
 * limiter that allocates a slot per key lets an attacker with forged keys turn
 * the defence into the memory exhaustion it was meant to prevent. With 2^k
 * buckets two keys can hash together and share a budget, which is the price
 * paid for a bound an attacker cannot move.
 *
 * DNSServer keeps its own 64-slot IPv4 bucket rather than using this. That is
 * deliberate: its policy is fixed, its key is a uint32 in network order, and it
 * runs inside a UDP receive path where a string key would mean an allocation
 * per packet -- exactly what a flood wants.
 */
#include "dyna-nat.h"

#include <stdlib.h>
#include <string.h>

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_NET)

#include "core/dyn-timer.h"

#define RL_MIN_SLOTS      8u
#define RL_MAX_SLOTS      (1u << 20)
#define RL_DEFAULT_SLOTS  1024u
#define RL_SCALE          1000       /* milli-tokens: exact integer refill */

typedef struct {
    uint64_t key;                    /* hash; a different key takes the slot */
    int64_t  tokens;                 /* milli-tokens */
    uint64_t last_ms;
    int      used;
} rl_slot_t;

typedef struct {
    rl_slot_t *slots;
    uint32_t   n_slots;              /* power of two */
    uint32_t   rate;                 /* tokens per second */
    int64_t    burst;                /* milli-tokens */
    uint64_t   allowed, denied;
} dyn_rl_t;

static JSClassID dyn_rl_class_id;

static void dyn_rl_free(void *native)
{
    dyn_rl_t *R = (dyn_rl_t *)native;
    if (!R)
        return;
    free(R->slots);
    free(R);
}

static void dyn_rl_finalizer(JSRuntime *rt, JSValue val)
{
    (void)rt;
    dyn_rl_free(JS_GetOpaque(val, dyn_rl_class_id));
}

static const JSClassDef dyn_rl_class = {
    "RateLimiter", .finalizer = dyn_rl_finalizer,
};

static uint64_t rl_hash(const char *s, size_t n)
{
    uint64_t h = 1469598103934665603ULL;
    size_t i;
    for (i = 0; i < n; i++) { h ^= (uint64_t)(unsigned char)s[i]; h *= 1099511628211ULL; }
    return h ? h : 1;                /* 0 marks an unused slot */
}

/* Refill is exact: milli-tokens per millisecond equals tokens per second, so
   the bucket needs no floating point and cannot drift. */
static rl_slot_t *rl_slot(dyn_rl_t *R, uint64_t key, uint64_t now)
{
    rl_slot_t *s = &R->slots[key & (R->n_slots - 1)];

    if (!s->used || s->key != key) {  /* a colliding key takes the slot over */
        s->key = key;
        s->tokens = R->burst;
        s->last_ms = now;
        s->used = 1;
        return s;
    }
    if (now > s->last_ms) {
        int64_t add = (int64_t)(now - s->last_ms) * (int64_t)R->rate;
        s->tokens += add;
        if (s->tokens > R->burst)
            s->tokens = R->burst;
    }
    s->last_ms = now;
    return s;
}

static uint32_t rl_round_pow2(uint32_t v)
{
    uint32_t p = RL_MIN_SLOTS;
    while (p < v && p < RL_MAX_SLOTS) p <<= 1;
    return p;
}

static JSValue dyn_rl_ctor(JSContext *ctx, JSValueConst new_target,
                           int argc, JSValueConst *argv)
{
    dyn_rl_t *R;
    int64_t rate = 0, burst = -1, slots = RL_DEFAULT_SLOTS;
    JSValue v;
    (void)new_target;

    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "new RateLimiter({tokensPerSec, burst?, "
                                      "slots?}): an options object is required");
    v = JS_GetPropertyStr(ctx, argv[0], "tokensPerSec");
    if (JS_IsException(v))
        return JS_EXCEPTION;
    if (JS_IsUndefined(v)) {
        JS_FreeValue(ctx, v);
        return JS_ThrowTypeError(ctx, "RateLimiter: tokensPerSec is required");
    }
    if (JS_ToInt64(ctx, &rate, v)) { JS_FreeValue(ctx, v); return JS_EXCEPTION; }
    JS_FreeValue(ctx, v);
    if (rate <= 0 || rate > 1000000000)
        return JS_ThrowRangeError(ctx, "RateLimiter: tokensPerSec is 1 to 1e9");

    v = JS_GetPropertyStr(ctx, argv[0], "burst");
    if (JS_IsException(v))
        return JS_EXCEPTION;
    if (!JS_IsUndefined(v) && JS_ToInt64(ctx, &burst, v)) {
        JS_FreeValue(ctx, v);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, v);
    if (burst < 0)
        burst = rate;                /* one second of traffic */
    if (burst <= 0 || burst > 1000000000)
        return JS_ThrowRangeError(ctx, "RateLimiter: burst is 1 to 1e9");

    v = JS_GetPropertyStr(ctx, argv[0], "slots");
    if (JS_IsException(v))
        return JS_EXCEPTION;
    if (!JS_IsUndefined(v) && JS_ToInt64(ctx, &slots, v)) {
        JS_FreeValue(ctx, v);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, v);
    if (slots < RL_MIN_SLOTS || slots > RL_MAX_SLOTS)
        return JS_ThrowRangeError(ctx, "RateLimiter: slots is %u to %u",
                                  RL_MIN_SLOTS, RL_MAX_SLOTS);

    R = (dyn_rl_t *)malloc(sizeof(*R));
    if (!R)
        return JS_ThrowOutOfMemory(ctx);
    R->n_slots = rl_round_pow2((uint32_t)slots);
    R->rate = (uint32_t)rate;
    R->burst = burst * RL_SCALE;
    R->allowed = R->denied = 0;
    R->slots = (rl_slot_t *)calloc(R->n_slots, sizeof(*R->slots));
    if (!R->slots) { free(R); return JS_ThrowOutOfMemory(ctx); }
    return dyn_plain_wrap(ctx, dyn_rl_class_id, R, dyn_rl_free);
}

/* magic 0 = allow(key, cost = 1), 1 = tokens(key) */
static JSValue dyn_rl_allow(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv, int magic)
{
    dyn_rl_t *R;
    const char *key;
    size_t klen;
    rl_slot_t *s;
    int64_t cost = 1;

    R = (dyn_rl_t *)dyn_plain_get(ctx, this_val, dyn_rl_class_id);
    if (!R)
        return JS_EXCEPTION;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "%s(key): key is required",
                                 magic ? "tokens" : "allow");
    if (magic == 0 && argc > 1 && !JS_IsUndefined(argv[1])) {
        if (JS_ToInt64(ctx, &cost, argv[1]))
            return JS_EXCEPTION;
        if (cost <= 0)
            return JS_ThrowRangeError(ctx, "allow(key, cost): cost must be > 0");
    }
    key = JS_ToCStringLen(ctx, &klen, argv[0]);
    if (!key)
        return JS_EXCEPTION;
    s = rl_slot(R, rl_hash(key, klen), dyn_timer_now_ms());
    JS_FreeCString(ctx, key);
    if (magic == 1)
        return JS_NewFloat64(ctx, (double)s->tokens / RL_SCALE);
    if (s->tokens < cost * RL_SCALE) {
        R->denied++;
        return JS_NewBool(ctx, 0);
    }
    s->tokens -= cost * RL_SCALE;
    R->allowed++;
    return JS_NewBool(ctx, 1);
}

static JSValue dyn_rl_reset(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    dyn_rl_t *R = (dyn_rl_t *)dyn_plain_get(ctx, this_val, dyn_rl_class_id);

    if (!R)
        return JS_EXCEPTION;
    if (argc < 1 || JS_IsUndefined(argv[0])) {
        memset(R->slots, 0, (size_t)R->n_slots * sizeof(*R->slots));
        return JS_UNDEFINED;
    }
    {
        size_t klen;
        const char *key = JS_ToCStringLen(ctx, &klen, argv[0]);
        rl_slot_t *s;
        if (!key)
            return JS_EXCEPTION;
        s = &R->slots[rl_hash(key, klen) & (R->n_slots - 1)];
        JS_FreeCString(ctx, key);
        memset(s, 0, sizeof(*s));
    }
    return JS_UNDEFINED;
}

static JSValue dyn_rl_get_stats(JSContext *ctx, JSValueConst this_val)
{
    dyn_rl_t *R = (dyn_rl_t *)dyn_plain_get(ctx, this_val, dyn_rl_class_id);
    JSValue o;
    uint32_t i, live = 0;

    if (!R)
        return JS_EXCEPTION;
    for (i = 0; i < R->n_slots; i++)
        live += (uint32_t)(R->slots[i].used != 0);
    o = JS_NewObject(ctx);
    if (JS_IsException(o))
        return o;
#define RSTAT(name, v) \
    if (JS_DefinePropertyValueStr(ctx, o, name, JS_NewInt64(ctx, (int64_t)(v)), \
                                  JS_PROP_C_W_E) < 0) { \
        JS_FreeValue(ctx, o); return JS_EXCEPTION; }
    RSTAT("allowed", R->allowed)
    RSTAT("denied", R->denied)
    RSTAT("slots", R->n_slots)
    RSTAT("live", live)
    RSTAT("tokensPerSec", R->rate)
    RSTAT("burst", R->burst / RL_SCALE)
#undef RSTAT
    return o;
}

static const JSCFunctionListEntry dyn_rl_proto[] = {
    JS_CFUNC_MAGIC_DEF("allow", 2, dyn_rl_allow, 0),
    JS_CFUNC_MAGIC_DEF("tokens", 1, dyn_rl_allow, 1),
    JS_CFUNC_DEF("reset", 1, dyn_rl_reset),
    JS_CGETSET_DEF("stats", dyn_rl_get_stats, NULL),
};

int dyn_ratelimit_register(JSContext *ctx, JSModuleDef *m)
{
    return dyn_register_plain_class(ctx, m, &dyn_rl_class_id, &dyn_rl_class,
                                    dyn_rl_proto,
                                    (int)(sizeof(dyn_rl_proto) / sizeof(dyn_rl_proto[0])),
                                    dyn_rl_ctor, "RateLimiter");
}

void dyn_ratelimit_add_exports(JSContext *ctx, JSModuleDef *m)
{
    JS_AddModuleExport(ctx, m, "RateLimiter");
}

#endif /* CONFIG_NATIVE_MODULES && CONFIG_NATIVE_MODULE_NET */
