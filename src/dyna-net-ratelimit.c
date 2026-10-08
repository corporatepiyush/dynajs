#include "dyna-nat.h"

#include <stdlib.h>
#include <string.h>

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_NET)

#include "core/dyn-timer.h"

#define RL_MIN_SLOTS 8u
#define RL_MAX_SLOTS (1u << 20)
#define RL_DEFAULT_SLOTS 1024u
#define RL_SCALE 1000

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

static int dyn_rl_opts_check(JSContext* ctx, JSValueConst o,
    const char* const* keys, int nkeys)
{
    JSPropertyEnum* props = NULL;
    uint32_t nprops = 0, i;
    int j, k, bad = 0;

    if (!JS_IsObject(o))
        return 0;
    if (JS_GetOwnPropertyNames(ctx, &props, &nprops, o,
            JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY))
        return -1;
    for (i = 0; i < nprops && !bad; i++) {
        const char* name = JS_AtomToCString(ctx, props[i].atom);
        if (!name) {
            bad = 1;
            break;
        }
        for (k = 0; k < nkeys; k++)
            if (strcmp(name, keys[k]) == 0)
                break;
        if (k == nkeys) {
            size_t need = 1, l;
            char *valid, *w;
            for (k = 0; k < nkeys; k++)
                need += strlen(keys[k]) + 2;
            valid = (char*)js_malloc(ctx, need);
            if (!valid) {
                JS_FreeCString(ctx, name);
                bad = 1;
                break;
            }
            w = valid;
            for (k = 0; k < nkeys; k++) {
                l = strlen(keys[k]);
                if (k) {
                    *w++ = ',';
                    *w++ = ' ';
                }
                memcpy(w, keys[k], l);
                w += l;
            }
            *w = '\0';
            JS_ThrowTypeError(ctx, "unknown option \"%s\" (valid: %s)",
                name, valid);
            js_free(ctx, valid);
            bad = 1;
        }
        JS_FreeCString(ctx, name);
    }
    for (j = 0; j < (int)nprops; j++)
        JS_FreeAtom(ctx, props[j].atom);
    js_free(ctx, props);
    return bad ? -1 : 0;
}

static const char* const dyn_rl_keys[] = {
    "tokensPerSec",
    "refill",
    "burst",
    "capacity",
    "grow",
    "slots",
};

typedef struct {
    uint64_t key;
    int64_t tokens;
    uint64_t last_ms;
    int used;
} rl_slot_t;

typedef struct {
    rl_slot_t* slots;
    uint32_t n_slots;
    uint32_t n_used;
    uint32_t rate;
    int64_t burst;
    uint64_t allowed, denied;
    uint32_t grew;
    int grow;
} dyn_rl_t;

static JSClassID dyn_rl_class_id;

static void dyn_rl_free(void* native)
{
    dyn_rl_t* R = (dyn_rl_t*)native;
    if (!R)
        return;
    free(R->slots);
    free(R);
}

static void dyn_rl_finalizer(JSRuntime* rt, JSValue val)
{
    (void)rt;
    dyn_rl_free(JS_GetOpaque(val, dyn_rl_class_id));
}

static const JSClassDef dyn_rl_class = {
    "RateLimiter",
    .finalizer = dyn_rl_finalizer,
};

static uint64_t rl_hash(const char* s, size_t n)
{
    uint64_t h = 1469598103934665603ULL;
    size_t i;
    for (i = 0; i < n; i++) {
        h ^= (uint64_t)(unsigned char)s[i];
        h *= 1099511628211ULL;
    }
    return h ? h : 1;
}

static int rl_grow(dyn_rl_t* R);


static rl_slot_t* rl_find_live(dyn_rl_t* R, uint64_t key)
{
    uint32_t mask = R->n_slots - 1;
    uint32_t i = (uint32_t)key & mask;
    uint32_t start = i;

    do {
        rl_slot_t* s = &R->slots[i];
        if (!s->used)
            return NULL;
        if (s->key == key)
            return s;
        i = (i + 1) & mask;
    } while (i != start);
    return NULL;
}


static void rl_remove(dyn_rl_t* R, rl_slot_t* s)
{
    uint32_t mask = R->n_slots - 1;
    uint32_t j = (uint32_t)(s - R->slots);

    j = (j + 1) & mask;
    memset(s, 0, sizeof(*s));
    R->n_used--;
    while (R->slots[j].used) {
        rl_slot_t t = R->slots[j];
        uint32_t h = (uint32_t)t.key & mask;
        uint32_t k = h;

        memset(&R->slots[j], 0, sizeof(t));
        while (R->slots[k].used)
            k = (k + 1) & mask;
        R->slots[k] = t;
        j = (j + 1) & mask;
    }
}

static rl_slot_t* rl_slot(dyn_rl_t* R, uint64_t key, uint64_t now)
{
    rl_slot_t* s = NULL;
    int found = 0;

    if (R->grow && R->n_used * 4 >= R->n_slots * 3)
        rl_grow(R);

    {
        uint32_t mask = R->n_slots - 1;
        uint32_t i = (uint32_t)key & mask;
        uint32_t start = i;
        rl_slot_t* lru = &R->slots[i];

        do {
            rl_slot_t* t = &R->slots[i];
            if (!t->used || t->key == key) {
                s = t;
                found = t->used;
                break;
            }
            if (t->last_ms < lru->last_ms)
                lru = t;
            i = (i + 1) & mask;
        } while (i != start);
        if (!s)
            s = lru;
    }

    if (found) {
        if (now > s->last_ms) {
            uint64_t cap_ms = (uint64_t)(R->burst / R->rate) + 2;
            uint64_t dt = now - s->last_ms;
            int64_t add = dt >= cap_ms
                ? R->burst
                : (int64_t)(dt * (uint64_t)R->rate);
            s->tokens += add;
            if (s->tokens > R->burst)
                s->tokens = R->burst;
        }
    } else {
        if (!s->used)
            R->n_used++;
        s->tokens = R->burst;
    }
    s->key = key;
    s->last_ms = now;
    s->used = 1;
    return s;
}

static int rl_grow(dyn_rl_t* R)
{
    uint32_t newn = R->n_slots << 1;
    rl_slot_t* ns;
    uint32_t i;

    if (newn > RL_MAX_SLOTS || newn <= R->n_slots)
        return 0;
    ns = (rl_slot_t*)calloc(newn, sizeof(*ns));
    if (!ns)
        return 0;
    for (i = 0; i < R->n_slots; i++) {
        rl_slot_t* s = &R->slots[i];
        uint32_t j;
        if (!s->used)
            continue;
        j = (uint32_t)s->key & (newn - 1);
        while (ns[j].used)
            j = (j + 1) & (newn - 1);
        ns[j] = *s;
    }
    free(R->slots);
    R->slots = ns;
    R->n_slots = newn;
    R->grew++;
    return 1;
}

static uint32_t rl_round_pow2(uint32_t v)
{
    uint32_t p = RL_MIN_SLOTS;
    while (p < v && p < RL_MAX_SLOTS)
        p <<= 1;
    return p;
}

static JSValue dyn_rl_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    dyn_rl_t* R;
    int64_t rate = 0, burst = -1, slots = RL_DEFAULT_SLOTS;
    int have_rate = 0, have_burst = 0;
    int grow = 0;
    JSValue v;

    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "new RateLimiter({tokensPerSec, burst?, "
                                      "slots?}): an options object is required");
    if (dyn_rl_opts_check(ctx, argv[0], dyn_rl_keys, countof(dyn_rl_keys)))
        return JS_EXCEPTION;

    v = JS_GetPropertyStr(ctx, argv[0], "tokensPerSec");
    if (JS_IsException(v))
        return JS_EXCEPTION;
    if (!JS_IsUndefined(v)) {
        if (JS_ToInt64(ctx, &rate, v)) {
            JS_FreeValue(ctx, v);
            return JS_EXCEPTION;
        }
        have_rate = 1;
    }
    JS_FreeValue(ctx, v);
    v = JS_GetPropertyStr(ctx, argv[0], "refill");
    if (JS_IsException(v))
        return JS_EXCEPTION;
    if (!JS_IsUndefined(v)) {
        int64_t refill = 0;
        if (JS_ToInt64(ctx, &refill, v)) {
            JS_FreeValue(ctx, v);
            return JS_EXCEPTION;
        }
        if (have_rate && refill != rate) {
            JS_FreeValue(ctx, v);
            return JS_ThrowTypeError(ctx,
                "RateLimiter: tokensPerSec and refill disagree");
        }
        rate = refill;
        have_rate = 1;
    }
    JS_FreeValue(ctx, v);
    if (!have_rate)
        return JS_ThrowTypeError(ctx, "RateLimiter: tokensPerSec (or refill) is required");
    if (rate <= 0 || rate > 1000000000)
        return JS_ThrowRangeError(ctx, "RateLimiter: tokensPerSec is 1 to 1e9");

    v = JS_GetPropertyStr(ctx, argv[0], "burst");
    if (JS_IsException(v))
        return JS_EXCEPTION;
    if (!JS_IsUndefined(v)) {
        if (JS_ToInt64(ctx, &burst, v)) {
            JS_FreeValue(ctx, v);
            return JS_EXCEPTION;
        }
        have_burst = 1;
    }
    JS_FreeValue(ctx, v);
    v = JS_GetPropertyStr(ctx, argv[0], "capacity");
    if (JS_IsException(v))
        return JS_EXCEPTION;
    if (!JS_IsUndefined(v)) {
        int64_t cap = 0;
        if (JS_ToInt64(ctx, &cap, v)) {
            JS_FreeValue(ctx, v);
            return JS_EXCEPTION;
        }
        if (have_burst && cap != burst) {
            JS_FreeValue(ctx, v);
            return JS_ThrowTypeError(ctx,
                "RateLimiter: burst and capacity disagree");
        }
        burst = cap;
        have_burst = 1;
    }
    JS_FreeValue(ctx, v);
    if (burst < 0)
        burst = rate;
    if (burst <= 0 || burst > 1000000000)
        return JS_ThrowRangeError(ctx, "RateLimiter: burst is 1 to 1e9");

    v = JS_GetPropertyStr(ctx, argv[0], "grow");
    if (JS_IsException(v))
        return JS_EXCEPTION;
    if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
        if (!JS_IsBool(v)) {
            JS_FreeValue(ctx, v);
            return JS_ThrowTypeError(ctx, "RateLimiter: grow must be a boolean");
        }
        grow = JS_ToBool(ctx, v);
    }
    JS_FreeValue(ctx, v);

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

    R = (dyn_rl_t*)malloc(sizeof(*R));
    if (!R)
        return JS_ThrowOutOfMemory(ctx);
    memset(R, 0, sizeof(*R));
    R->n_slots = rl_round_pow2((uint32_t)slots);
    R->rate = (uint32_t)rate;
    R->burst = burst * RL_SCALE;
    R->grow = grow;
    R->slots = (rl_slot_t*)calloc(R->n_slots, sizeof(*R->slots));
    if (!R->slots) {
        free(R);
        return JS_ThrowOutOfMemory(ctx);
    }
    return dyn_plain_wrap(ctx, new_target, dyn_rl_class_id, R, dyn_rl_free);
}

static JSValue dyn_rl_allow(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    dyn_rl_t* R;
    const char* key;
    size_t klen;
    rl_slot_t* s;
    int64_t cost = 1;

    R = (dyn_rl_t*)dyn_plain_get(ctx, this_val, dyn_rl_class_id);
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
    if (cost > R->burst / RL_SCALE) {
        R->denied++;
        return JS_NewBool(ctx, 0);
    }
    {
        int64_t cost_ms = cost * RL_SCALE;
        if (s->tokens < cost_ms) {
            R->denied++;
            return JS_NewBool(ctx, 0);
        }
        s->tokens -= cost_ms;
    }
    R->allowed++;
    return JS_NewBool(ctx, 1);
}

static JSValue dyn_rl_reset(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_rl_t* R = (dyn_rl_t*)dyn_plain_get(ctx, this_val, dyn_rl_class_id);

    if (!R)
        return JS_EXCEPTION;
    if (argc < 1 || JS_IsUndefined(argv[0])) {
        memset(R->slots, 0, (size_t)R->n_slots * sizeof(*R->slots));
        R->n_used = 0;
        return JS_UNDEFINED;
    }
    {
        size_t klen;
        const char* key = JS_ToCStringLen(ctx, &klen, argv[0]);
        rl_slot_t* s;
        if (!key)
            return JS_EXCEPTION;
        s = rl_find_live(R, rl_hash(key, klen));
        JS_FreeCString(ctx, key);
        if (s)
            rl_remove(R, s);
    }
    return JS_UNDEFINED;
}

static JSValue dyn_rl_get_stats(JSContext* ctx, JSValueConst this_val)
{
    dyn_rl_t* R = (dyn_rl_t*)dyn_plain_get(ctx, this_val, dyn_rl_class_id);
    JSValue o;

    if (!R)
        return JS_EXCEPTION;
    o = JS_NewObject(ctx);
    if (JS_IsException(o))
        return o;
#define RSTAT(name, v)                                                          \
    if (JS_DefinePropertyValueStr(ctx, o, name, JS_NewInt64(ctx, (int64_t)(v)), \
            JS_PROP_C_W_E)                                                      \
        < 0) {                                                                  \
        JS_FreeValue(ctx, o);                                                   \
        return JS_EXCEPTION;                                                    \
    }
    RSTAT("allowed", R->allowed)
    RSTAT("denied", R->denied)
    RSTAT("slots", R->n_slots)
    RSTAT("live", R->n_used)
    RSTAT("grew", R->grew)
    RSTAT("tokensPerSec", R->rate)
    RSTAT("burst", R->burst / RL_SCALE)
#undef RSTAT
    return o;
}

static const JSCFunctionListEntry dyn_rl_proto[] = {
    JS_CFUNC_MAGIC_DEF("allow", 1, dyn_rl_allow, 0),
    JS_CFUNC_MAGIC_DEF("tokens", 1, dyn_rl_allow, 1),
    JS_CFUNC_DEF("reset", 0, dyn_rl_reset),
    JS_CGETSET_DEF("stats", dyn_rl_get_stats, NULL),
};

int dyn_ratelimit_register(JSContext* ctx, JSModuleDef* m)
{
    return dyn_register_plain_class(ctx, m, &dyn_rl_class_id, &dyn_rl_class,
        dyn_rl_proto,
        (int)(sizeof(dyn_rl_proto) / sizeof(dyn_rl_proto[0])),
        dyn_rl_ctor, "RateLimiter");
}

void dyn_ratelimit_add_exports(JSContext* ctx, JSModuleDef* m)
{
    JS_AddModuleExport(ctx, m, "RateLimiter");
}

#endif
