#ifdef USE_OLD_DNS
#ifndef DNS_OLD_PATH
#define DNS_OLD_PATH "/tmp/dns_old.c"
#endif
#include DNS_OLD_PATH
#else
#include "dyna-net-dns.c"
#endif

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_NET)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { \
    printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

typedef struct {
    char *name, *address;
    int type;
    long long ttl;
    int is_str;
    char *str;
} fake_val_t;

#define FAKE_MAX_BLOCKS 512
#define FAKE_MAX_RECS   64

static void *fake_blocks[FAKE_MAX_BLOCKS];
static int n_fake_blocks;

static fake_val_t *cap_rec[FAKE_MAX_RECS];
static uint32_t cap_idx[FAKE_MAX_RECS];
static int n_cap;

static fake_val_t *fake_val_new(int is_str, const char *s, size_t slen)
{
    uint8_t *mem;
    fake_val_t *v;
    if (n_fake_blocks >= FAKE_MAX_BLOCKS)
        return NULL;
    mem = (uint8_t *)malloc(8 + sizeof(fake_val_t));
    if (!mem)
        return NULL;
    *(int *)(mem + 4) = 0x10000000;
    v = (fake_val_t *)(mem + 8);
    memset(v, 0, sizeof *v);
    v->is_str = is_str;
    if (is_str) {
        v->str = (char *)malloc(slen + 1);
        if (!v->str) { free(mem); return NULL; }
        memcpy(v->str, s, slen);
        v->str[slen] = '\0';
    }
    fake_blocks[n_fake_blocks++] = mem;
    return v;
}

static void fake_js_free_all(void)
{
    int i;
    for (i = 0; i < n_fake_blocks; i++) {
        fake_val_t *v = (fake_val_t *)((uint8_t *)fake_blocks[i] + 8);
        free(v->name);
        free(v->address);
        free(v->str);
        free(fake_blocks[i]);
    }
    n_fake_blocks = 0;
}

void __JS_FreeValue(JSContext *ctx, JSValue v) { (void)ctx; (void)v; }

JSValue JS_NewStringLen(JSContext *ctx, const char *str, size_t len)
{
    fake_val_t *v = fake_val_new(1, str, len);
    (void)ctx;
    return v ? JS_MKPTR(JS_TAG_STRING, v) : JS_UNDEFINED;
}

JSValue JS_NewObject(JSContext *ctx)
{
    fake_val_t *v = fake_val_new(0, NULL, 0);
    (void)ctx;
    return v ? JS_MKPTR(JS_TAG_OBJECT, v) : JS_EXCEPTION;
}

JSValue JS_NewArray(JSContext *ctx) { return JS_NewObject(ctx); }

int JS_DefinePropertyValueStr(JSContext *ctx, JSValueConst this_obj,
                              const char *prop, JSValue val, int flags)
{
    fake_val_t *o;
    (void)ctx; (void)flags;
    if (JS_VALUE_GET_TAG(this_obj) != JS_TAG_OBJECT)
        return -1;
    o = (fake_val_t *)JS_VALUE_GET_PTR(this_obj);
    if (JS_VALUE_GET_TAG(val) == JS_TAG_STRING) {
        fake_val_t *s = (fake_val_t *)JS_VALUE_GET_PTR(val);
        if (!strcmp(prop, "name"))   { free(o->name);    o->name    = strdup(s->str); }
        if (!strcmp(prop, "address")){ free(o->address); o->address = strdup(s->str); }
    } else if (!strcmp(prop, "type")) {
        o->type = JS_VALUE_GET_INT(val);
    } else if (!strcmp(prop, "ttl")) {
        o->ttl = JS_VALUE_GET_TAG(val) == JS_TAG_FLOAT64
            ? (long long)JS_VALUE_GET_FLOAT64(val)
            : (long long)JS_VALUE_GET_INT(val);
    }
    return 1;
}

int JS_DefinePropertyValueUint32(JSContext *ctx, JSValueConst this_obj,
                                 uint32_t idx, JSValue val, int flags)
{
    (void)ctx; (void)this_obj; (void)flags;
    if (JS_VALUE_GET_TAG(val) == JS_TAG_OBJECT &&
        n_cap < FAKE_MAX_RECS) {
        cap_rec[n_cap] = (fake_val_t *)JS_VALUE_GET_PTR(val);
        cap_idx[n_cap] = idx;
        n_cap++;
    }
    return 1;
}

JS_BOOL JS_IsFunction(JSContext *ctx, JSValueConst val)
{ (void)ctx; return JS_VALUE_GET_TAG(val) == JS_TAG_OBJECT ? 1 : 0; }

const char *JS_ToCStringLen2(JSContext *ctx, size_t *plen,
                             JSValueConst val, JS_BOOL cesu8)
{ (void)ctx; (void)val; (void)cesu8; if (plen) *plen = 0; return NULL; }
void JS_FreeCString(JSContext *ctx, const char *ptr) { (void)ctx; (void)ptr; }
int JS_ToInt64(JSContext *ctx, int64_t *pres, JSValueConst val)
{ (void)ctx; (void)val; if (pres) *pres = 0; return -1; }
JSValue JS_GetException(JSContext *ctx)
{ (void)ctx; return JS_NULL; }
JSValue JS_GetPropertyUint32(JSContext *ctx, JSValueConst this_obj,
                             uint32_t idx)
{ (void)ctx; (void)this_obj; (void)idx; return JS_UNDEFINED; }
JSValue JS_NewError(JSContext *ctx)
{ (void)ctx; return JS_EXCEPTION; }
int JS_SetPropertyStr(JSContext *ctx, JSValueConst this_obj,
                      const char *prop, JSValueConst val)
{ (void)ctx; (void)this_obj; (void)prop; (void)val; return -1; }
int JS_ToInt32(JSContext *ctx, int32_t *pres, JSValueConst val)
{ (void)ctx; (void)val; if (pres) *pres = 0; return -1; }
JSValue JS_GetPropertyStr(JSContext *ctx, JSValueConst this_obj,
                          const char *prop)
{ (void)ctx; (void)this_obj; (void)prop; return JS_UNDEFINED; }
JSValue JS_Call(JSContext *ctx, JSValueConst func, JSValueConst this_obj,
                int argc, JSValueConst *argv)
{ (void)ctx; (void)func; (void)this_obj; (void)argc; (void)argv; return JS_UNDEFINED; }
int JS_AddModuleExport(JSContext *ctx, JSModuleDef *m, const char *name)
{ (void)ctx; (void)m; (void)name; return 0; }
JSValue JS_ThrowTypeError(JSContext *ctx, const char *fmt, ...)
{ (void)ctx; (void)fmt; return JS_EXCEPTION; }
JSValue JS_ThrowRangeError(JSContext *ctx, const char *fmt, ...)
{ (void)ctx; (void)fmt; return JS_EXCEPTION; }
JSValue JS_ThrowInternalError(JSContext *ctx, const char *fmt, ...)
{ (void)ctx; (void)fmt; return JS_EXCEPTION; }
JSValue JS_ThrowOutOfMemory(JSContext *ctx)
{ (void)ctx; return JS_EXCEPTION; }
JSValue dyn_res_wrap(JSContext *ctx, JSValueConst new_target,
                     JSClassID id, void *native, DynDisposeFunc dispose)
{ (void)new_target; (void)id; (void)native; (void)dispose; if (ctx) {} return JS_UNDEFINED; }
void dyn_res_finalizer(JSRuntime *rt, JSValue val) { (void)rt; (void)val; }
int dyn_register_class(JSContext *ctx, JSModuleDef *m, JSClassID *pid,
                       const JSClassDef *def,
                       const JSCFunctionListEntry *proto_funcs, int n_funcs,
                       JSCFunction *ctor_fn, const char *name)
{
    (void)ctx; (void)m; (void)pid; (void)def; (void)proto_funcs;
    (void)n_funcs; (void)ctor_fn; (void)name;
    return -1;
}
struct dyn_aio *dyn_net_reactor_acquire(JSContext *ctx) { (void)ctx; return NULL; }
void dyn_net_reactor_release(JSContext *ctx) { (void)ctx; }
int dyn_net_on_drain(void (*fn)(void *), void *udata) { (void)fn; (void)udata; return -1; }
void dyn_net_off_drain(void *udata) { (void)udata; }

static size_t add_rr(uint8_t *m, size_t cap, size_t off, const char *owner,
                     uint16_t type, uint32_t ttl, const void *rdata,
                     uint16_t rdlen)
{
    int n = dyn_dns_name_encode(owner, m + off, cap - off);
    if (n < 0 || off + (size_t)n + 10u + rdlen > cap)
        return (size_t)-1;
    off += (size_t)n;
    m[off++] = (uint8_t)(type >> 8);      m[off++] = (uint8_t)type;
    m[off++] = 0;                         m[off++] = 1;
    m[off++] = (uint8_t)(ttl >> 24);      m[off++] = (uint8_t)(ttl >> 16);
    m[off++] = (uint8_t)(ttl >> 8);       m[off++] = (uint8_t)ttl;
    m[off++] = (uint8_t)(rdlen >> 8);     m[off++] = (uint8_t)rdlen;
    memcpy(m + off, rdata, rdlen);
    return off + rdlen;
}

static size_t build_response(uint8_t *m, size_t cap, const char *qname,
                             uint16_t qtype, int nrr)
{
    int n = dyn_dns_build_query(0x1234, qname, qtype, m, cap);
    if (n < 0)
        return (size_t)-1;
    m[2] |= 0x80;
    m[6] = (uint8_t)(nrr >> 8);
    m[7] = (uint8_t)nrr;
    return (size_t)n;
}

static int drive(dyn_resolver_t *r, const uint8_t *msg, size_t len)
{
    dns_pending_t *p = &r->q[0];

    memset(p, 0, sizeof *p);
    p->used = 1;
    p->id = (uint16_t)((msg[0] << 8) | msg[1]);
    snprintf(p->qname, sizeof p->qname, "%s", "good.example");
    p->qtype = DYN_DNS_T_A;
    p->cb = JS_UNDEFINED;
    p->timer = DYN_TIMER_NONE;
    p->tcp_fd = -1;
    p->owner = r;

    n_cap = 0;
    dns_handle_response(r, msg, (unsigned)len, p);
    return p->used;
}

#ifndef USE_OLD_DNS
static void test_helper(void)
{
    dns_pending_t p;
    dyn_dns_rr_t rr;

    memset(&p, 0, sizeof p);
    snprintf(p.qname, sizeof p.qname, "%s", "good.example");
    memset(&rr, 0, sizeof rr);

    snprintf(rr.name, sizeof rr.name, "%s", "good.example");
    CHECK(dns_rr_matches_query(&rr, &p) == 1, "owner == qname must match");
    snprintf(rr.name, sizeof rr.name, "%s", "GOOD.EXAMPLE");
    CHECK(dns_rr_matches_query(&rr, &p) == 1,
          "owner must match case-insensitively (RFC 1035 2.3.3)");
    snprintf(rr.name, sizeof rr.name, "%s", "Evil.Example");
    CHECK(dns_rr_matches_query(&rr, &p) == 0, "foreign owner must not match");
    snprintf(rr.name, sizeof rr.name, "%s", "good.example.org");
    CHECK(dns_rr_matches_query(&rr, &p) == 0, "a longer name is not a match");
    snprintf(rr.name, sizeof rr.name, "%s", "notgood.example");
    CHECK(dns_rr_matches_query(&rr, &p) == 0,
          "a suffix-sharing name is not a match");
}
#endif

int main(void)
{
    dyn_aio_t *aio;
    dyn_resolver_t r;
    struct sockaddr_in lo;
    socklen_t sl = sizeof(lo);
    int fd, i;

    setvbuf(stdout, NULL, _IOLBF, 0);

    aio = dyn_aio_new(0, 0);
    CHECK(aio != NULL, "dyn_aio_new");
    if (!aio) return 1;
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    CHECK(fd >= 0, "socket");
    memset(&lo, 0, sizeof lo);
    lo.sin_family = AF_INET;
    lo.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    lo.sin_port = 0;
    CHECK(bind(fd, (struct sockaddr *)&lo, sizeof lo) == 0, "bind loopback");
    CHECK(getsockname(fd, (struct sockaddr *)&lo, &sl) == 0, "getsockname");

    memset(&r, 0, sizeof r);
    r.aio = aio;
    r.fd = -1;
    r.server = lo;
    r.timeout_ms = 1000;
#ifndef USE_OLD_DNS
    test_helper();
#endif

    {
        static const uint8_t a1[4]  = { 192, 0, 2, 1 };
        static const uint8_t a2[4]  = { 6, 6, 6, 6 };
        static const uint8_t v6[16] = { 0x20,0x01,0x0d,0xb8, 0,0,0,0, 0,0,0,0, 0,0,0,1 };
        static const uint8_t a3[4]  = { 192, 0, 2, 2 };
        uint8_t m[512];
        size_t off = build_response(m, sizeof m, "good.example", DYN_DNS_T_A, 4);

        CHECK(off != (size_t)-1, "build response header");
        if (off != (size_t)-1) {
            off = add_rr(m, sizeof m, off, "good.example",  DYN_DNS_T_A,    60, a1, 4);
            off = add_rr(m, sizeof m, off, "evil.example",  DYN_DNS_T_A,    60, a2, 4);
            off = add_rr(m, sizeof m, off, "other.example", DYN_DNS_T_AAAA, 60, v6, 16);
            off = add_rr(m, sizeof m, off, "GOOD.EXAMPLE",  DYN_DNS_T_A,    60, a3, 4);
            CHECK(off != (size_t)-1, "append answers");
            CHECK(drive(&r, m, off) == 0, "response must settle the query");
            printf("foreign-among-matches: %d record(s) surfaced\n", n_cap);
            CHECK(n_cap == 2,
                  "a 4-answer response to good.example surfaced %d records, "
                  "want 2 -- foreign-owner answers leaked into the result",
                  n_cap);
            CHECK(n_cap == 2 && cap_idx[0] == 0 && cap_idx[1] == 1,
                  "array indices must stay dense after skipping (0,1)");
            for (i = 0; i < n_cap; i++)
                CHECK(strcasecmp(cap_rec[i]->name, "good.example") == 0,
                      "surfaced record %d owned by '%s', want good.example",
                      i, cap_rec[i]->name);
            CHECK(n_cap == 2 && strcmp(cap_rec[0]->address, "192.0.2.1") == 0,
                  "first surfaced address '%s', want 192.0.2.1",
                  n_cap > 0 ? cap_rec[0]->address : "(none)");
            CHECK(n_cap == 2 && strcmp(cap_rec[1]->address, "192.0.2.2") == 0,
                  "the case-differing owner must still be surfaced, as the "
                  "LAST record (loop skipped, not aborted): got '%s'",
                  n_cap > 1 ? cap_rec[1]->address : "(none)");
            for (i = 0; i < n_cap; i++)
                CHECK(cap_rec[i]->type == DYN_DNS_T_A && cap_rec[i]->ttl == 60,
                      "surfaced record %d has type %d ttl %lld", i,
                      cap_rec[i]->type, cap_rec[i]->ttl);
        }
    }

    {
        static const uint8_t bad[4] = { 6, 6, 6, 6 };
        static const uint8_t ok[4]  = { 203, 0, 113, 9 };
        uint8_t m[512];
        size_t off = build_response(m, sizeof m, "good.example", DYN_DNS_T_A, 2);

        CHECK(off != (size_t)-1, "build response header");
        if (off != (size_t)-1) {
            off = add_rr(m, sizeof m, off, "evil.example", DYN_DNS_T_A, 60, bad, 4);
            off = add_rr(m, sizeof m, off, "good.example", DYN_DNS_T_A, 60, ok, 4);
            CHECK(off != (size_t)-1, "append answers");
            CHECK(drive(&r, m, off) == 0, "response must settle the query");
            printf("foreign-first: %d record(s) surfaced\n", n_cap);
            CHECK(n_cap == 1,
                  "foreign-then-match surfaced %d records, want 1 -- a skip "
                  "that aborted the loop would report 0", n_cap);
            CHECK(n_cap == 1 && cap_idx[0] == 0,
                  "the surviving record must land at array index 0");
            CHECK(n_cap == 1 && strcmp(cap_rec[0]->address, "203.0.113.9") == 0,
                  "surfaced address '%s', want 203.0.113.9",
                  n_cap > 0 ? cap_rec[0]->address : "(none)");
        }
    }

    {
        static const uint8_t a1[4] = { 192, 0, 2, 10 };
        static const uint8_t a2[4] = { 192, 0, 2, 11 };
        uint8_t m[512];
        size_t off = build_response(m, sizeof m, "good.example", DYN_DNS_T_A, 2);

        CHECK(off != (size_t)-1, "build response header");
        if (off != (size_t)-1) {
            off = add_rr(m, sizeof m, off, "good.example", DYN_DNS_T_A, 60, a1, 4);
            off = add_rr(m, sizeof m, off, "good.example", DYN_DNS_T_A, 60, a2, 4);
            CHECK(off != (size_t)-1, "append answers");
            CHECK(drive(&r, m, off) == 0, "response must settle the query");
            printf("all-matching control: %d record(s) surfaced\n", n_cap);
            CHECK(n_cap == 2,
                  "the control must surface both answers (got %d) -- fewer "
                  "means the harness is broken, not the filter", n_cap);
        }
    }

    close(fd);
    dyn_aio_free(aio);
    fake_js_free_all();

    if (fails == 0) printf("test_dns_foreign_answers: all tests passed\n");
    else printf("test_dns_foreign_answers: %d FAILED\n", fails);
    return fails != 0;
}

#else
int main(void)
{
    printf("test_dns_foreign_answers: skipped (net module not built)\n");
    return 0;
}
#endif
