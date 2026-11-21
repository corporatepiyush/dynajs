#include "dns-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_DNS)

typedef struct {
    JSValue cb, resolve, reject, records;
    int is_promise, shape;
} dns_hit_t;

static void dns_name_fold(char* s)
{
    for (; *s; s++)
        if (*s >= 'A' && *s <= 'Z')
            *s = (char)(*s - 'A' + 'a');
}

static void dns_cache_free_entry(JSContext* ctx, dns_cache_entry_t* e)
{
    if (e->used) {
        JS_FreeValue(ctx, e->records);
        e->records = JS_UNDEFINED;
        e->used = 0;
    }
}

void dns_cache_free_entry_rt(JSRuntime* rt, dns_cache_entry_t* e)
{
    if (e->used) {
        JS_FreeValueRT(rt, e->records);
        e->records = JS_UNDEFINED;
        e->used = 0;
    }
}

void dns_cache_store(dyn_resolver_t* r, const char* name,
    uint16_t qtype, uint32_t min_ttl, JSValue records)
{
    JSContext* ctx = r->ctx;
    dns_cache_entry_t *slot = NULL, *victim = NULL;
    int i;
    uint64_t ttl_ms;

    if (!r->ttl_cap_s || !min_ttl)
        return;
    ttl_ms = 1000ull * (min_ttl < r->ttl_cap_s ? min_ttl : r->ttl_cap_s);
    for (i = 0; i < DNS_CACHE_SLOTS; i++) {
        dns_cache_entry_t* e = &r->cache[i];
        if (!e->used) {
            slot = e;
            break;
        }
        if (e->expire_ms <= dyn_timer_now_ms()) {
            slot = e;
            break;
        }
        if (!victim || e->stamp < victim->stamp)
            victim = e;
    }
    if (!slot)
        slot = victim;
    dns_cache_free_entry(ctx, slot);
    slot->used = 1;
    snprintf(slot->name, sizeof(slot->name), "%s", name);
    dns_name_fold(slot->name);
    slot->qtype = qtype;
    slot->expire_ms = dyn_timer_now_ms() + ttl_ms;
    slot->stamp = ++r->cache_seq;
    slot->records = dns_clone_records(ctx, records);
    if (JS_IsException(slot->records)) {
        dyn_exc_drop(ctx);
        slot->records = JS_UNDEFINED;
        slot->used = 0;
    }
}

JSValue dns_cache_fetch(dyn_resolver_t* r, const char* name,
    uint16_t qtype)
{
    JSContext* ctx = r->ctx;
    char key[DYN_DNS_MAX_NAME + 1];
    int i;

    if (!r->ttl_cap_s)
        return JS_UNDEFINED;
    snprintf(key, sizeof(key), "%s", name);
    dns_name_fold(key);
    for (i = 0; i < DNS_CACHE_SLOTS; i++) {
        dns_cache_entry_t* e = &r->cache[i];
        if (!e->used || e->qtype != qtype || strcmp(e->name, key) != 0)
            continue;
        if (e->expire_ms <= dyn_timer_now_ms()) {
            dns_cache_free_entry(ctx, e);
            return JS_UNDEFINED;
        }
        return dns_clone_records(ctx, e->records);
    }
    return JS_UNDEFINED;
}

static void dns_hit_deliver(JSContext* ctx, dns_hit_t* h)
{
    if (h->is_promise) {
        dns_pending_t tmp;
        memset(&tmp, 0, sizeof(tmp));
        tmp.is_promise = 1;
        tmp.shape = h->shape;
        tmp.resolve = h->resolve;
        tmp.reject = h->reject;
        dns_promise_settle(ctx, &tmp, JS_NULL, h->records);
    } else if (JS_IsFunction(ctx, h->cb)) {
        JSValueConst a[2] = { JS_NULL, h->records };
        JSValue r = JS_Call(ctx, h->cb, JS_UNDEFINED, 2, a);
        if (JS_IsException(r))
            dyn_exc_drop(ctx);
        else
            JS_FreeValue(ctx, r);
    }
}

static JSValue dns_hit_job(JSContext* ctx, int argc, JSValueConst* argv)
{
    dns_hit_t* h;
    int64_t p = 0;

    (void)argc;
    if (JS_ToInt64(ctx, &p, argv[0]))
        return JS_EXCEPTION;
    h = (dns_hit_t*)(uintptr_t)p;
    dns_hit_deliver(ctx, h);
    JS_FreeValue(ctx, h->cb);
    JS_FreeValue(ctx, h->resolve);
    JS_FreeValue(ctx, h->reject);
    JS_FreeValue(ctx, h->records);
    free(h);
    return JS_UNDEFINED;
}

int dns_cache_deliver(JSContext* ctx, JSValue records, int shape,
    JSValueConst cb, JSValue resolve, JSValue reject,
    int is_promise)
{
    dns_hit_t* h = (dns_hit_t*)calloc(1, sizeof(*h));
    JSValue dptr, a[2];
    int rc;

    if (!h)
        return -1;
    h->is_promise = is_promise;
    h->shape = shape;
    h->cb = JS_IsFunction(ctx, cb) ? JS_DupValue(ctx, cb) : JS_UNDEFINED;
    h->resolve = is_promise ? JS_DupValue(ctx, resolve) : JS_UNDEFINED;
    h->reject = is_promise ? JS_DupValue(ctx, reject) : JS_UNDEFINED;
    h->records = JS_DupValue(ctx, records);
    dptr = JS_NewInt64(ctx, (int64_t)(uintptr_t)h);
    a[0] = dptr;
    a[1] = records;
    rc = JS_EnqueueJob(ctx, dns_hit_job, 2, a);
    JS_FreeValue(ctx, dptr);
    if (rc < 0) {
        JS_FreeValue(ctx, h->cb);
        JS_FreeValue(ctx, h->resolve);
        JS_FreeValue(ctx, h->reject);
        JS_FreeValue(ctx, h->records);
        free(h);
        return -1;
    }
    return 0;
}

#endif
