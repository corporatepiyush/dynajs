// covers: c:src/modules/postgres/postgres-cancel.c -- rule 18 forced allocation failure for PostgreSQL.cancel() over TLS (calloc of the cancel state, strdup of host and CA) and a reactor that cannot be acquired
// The hook fails the Nth allocation postgres-cancel.c makes inside cancel(); each must throw out of memory synchronously, leave no exception pending and free
// everything it allocated (a live-pointer table on the hooks; macOS has no LSan). With no failure the CancelRequest reaches the TLS fake server. Run from the repo root.
#include "dynajs.h"
#include "dyna-libc.h"
#include "dyna-nat.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#undef malloc
#undef calloc
#undef realloc
#undef strdup
#undef free
#undef dyn_net_reactor_acquire
struct dyn_aio* dyn_net_reactor_acquire(JSContext* ctx);
void* malloc(size_t n);
void* calloc(size_t k, size_t n);
void* realloc(void* p, size_t n);
char* strdup(const char* s);
void free(void* p);

static int stale, fails, checks;
static unsigned long pk_alloc_n, pk_fail_at;
static int pk_failed, pk_nlive, pk_no_reactor;
static void* pk_live[64];

static int pk_fail_now(void)
{
    if (pk_fail_at && ++pk_alloc_n == pk_fail_at) {
        pk_failed = 1;
        return 1;
    }
    return 0;
}

static void* pk_track(void* p)
{
    if (p && pk_nlive < 64)
        pk_live[pk_nlive++] = p;
    return p;
}

static void pk_untrack(void* p)
{
    int i;
    for (i = 0; i < pk_nlive; i++)
        if (pk_live[i] == p) {
            pk_live[i] = pk_live[--pk_nlive];
            return;
        }
}

void* pk_t_malloc(size_t n)
{
    return pk_fail_now() ? NULL : pk_track(malloc(n));
}

void* pk_t_calloc(size_t k, size_t n)
{
    return pk_fail_now() ? NULL : pk_track(calloc(k, n));
}

void* pk_t_realloc(void* p, size_t n)
{
    void* q;
    if (pk_fail_now())
        return NULL;
    q = realloc(p, n);
    if (q) {
        pk_untrack(p);
        pk_track(q);
    }
    return q;
}

char* pk_t_strdup(const char* s)
{
    return pk_fail_now() ? NULL : (char*)pk_track(strdup(s));
}

struct dyn_aio* pk_t_reactor(JSContext* ctx)
{
    return pk_no_reactor ? NULL : dyn_net_reactor_acquire(ctx);
}

static JSValue h_fail_reactor(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)this_val;
    pk_no_reactor = argc > 0 && JS_ToBool(ctx, argv[0]) > 0;
    pk_nlive = 0;
    return JS_UNDEFINED;
}

void pk_t_free(void* p)
{
    pk_untrack(p);
    free(p);
}

static JSValue h_probe(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)this_val;
    (void)argc;
    (void)argv;
    if (JS_HasException(ctx)) {
        stale++;
        JS_FreeValue(ctx, JS_GetException(ctx));
    }
    return JS_UNDEFINED;
}

static JSValue h_fail_alloc(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    int64_t at = 0;
    (void)this_val;
    if (argc < 1 || JS_ToInt64(ctx, &at, argv[0]))
        return JS_EXCEPTION;
    pk_alloc_n = 0;
    pk_failed = 0;
    pk_nlive = 0;
    pk_fail_at = at > 0 ? (unsigned long)at : 0;
    return JS_UNDEFINED;
}

static JSValue h_alloc_failed(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)this_val;
    (void)argc;
    (void)argv;
    return JS_NewBool(ctx, pk_failed);
}

static JSValue h_live(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)this_val;
    (void)argc;
    (void)argv;
    return JS_NewInt32(ctx, pk_nlive);
}

static JSValue h_report(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    const char* s;
    (void)this_val;
    if (argc < 1)
        return JS_UNDEFINED;
    s = JS_ToCString(ctx, argv[0]);
    if (!s)
        return JS_EXCEPTION;
    checks++;
    if (strncmp(s, "ok ", 3)) {
        fails++;
        printf("  FAIL: %s\n", s);
    } else {
        printf("  %s\n", s);
    }
    JS_FreeCString(ctx, s);
    return JS_UNDEFINED;
}

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

static const char src[] =
    "import { TCPServer } from 'dyna:net';\n"
    "import { PostgreSQL } from 'dyna:postgres';\n"
    "import { testCert } from '../lib/services.js';\n"
    "const str = (u) => { let s = ''; for (let i = 0; i < u.length; i++) s += String.fromCharCode(u[i]); return s; };\n"
    "const bin = (s) => { const u = new Uint8Array(s.length); for (let i = 0; i < s.length; i++) u[i] = s.charCodeAt(i) & 255; return u; };\n"
    "const i32 = (v) => String.fromCharCode((v >>> 24) & 255, (v >>> 16) & 255, (v >>> 8) & 255, v & 255);\n"
    "const be = (s, o) => (s.charCodeAt(o) << 24 | s.charCodeAt(o + 1) << 16 | s.charCodeAt(o + 2) << 8 | s.charCodeAt(o + 3)) >>> 0;\n"
    "const msg = (t, b) => t + i32(b.length + 4) + b;\n"
    "const sleep = (ms) => new Promise((r) => setTimeout(r, ms));\n"
    "const rsa = testCert();\n"
    "if (!rsa) { report('openssl made no test certificate'); } else {\n"
    "let cancels = 0;\n"
    "const bufs = new Map(), live = new Map();\n"
    "const srv = new TCPServer({ port: 0, tls: { cert: rsa.cert, key: rsa.key, alpn: 'postgresql' } });\n"
    "srv.start({ data: (c, b) => {\n"
    "  bufs.set(c, (bufs.get(c) || '') + str(b));\n"
    "  for (;;) { const s = bufs.get(c);\n"
    "    if (!live.get(c)) { if (s.length < 8) return; const n = be(s, 0); if (s.length < n) return; bufs.set(c, s.slice(n));\n"
    "      if (be(s, 4) === 80877102) { cancels++; try { c.close(); } catch (e) {} return; }\n"
    "      live.set(c, 1); c.write(bin(msg('R', i32(0)) + msg('K', i32(4242) + 'abcd') + msg('Z', 'I'))); continue; }\n"
    "    if (s.length < 5) return; const n = be(s, 1); if (s.length < n + 1) return; const t = s[0]; bufs.set(c, s.slice(n + 1));\n"
    "    if (t === 'Q') try { c.write(bin(msg('C', 'SELECT 0\\0') + msg('Z', 'I'))); } catch (e) {} } },\n"
    "  close: (c) => { bufs.delete(c); live.delete(c); } });\n"
    "const c = new PostgreSQL({ host: '127.0.0.1', port: srv.port, user: 'u', tls: true, ca: rsa.ca, sslNegotiation: 'direct', connectTimeoutMs: 3000 });\n"
    "await c.query('SELECT 1');\n"
    "let seen = 0, bad = '', at;\n"
    "for (at = 1; at < 40; at++) {\n"
    "  let r = null; failAlloc(at);\n"
    "  try { c.cancel(); } catch (e) { r = e; }\n"
    "  const failed = allocFailed(), lv = liveCount(); failAlloc(0); probe();\n"
    "  if (!failed) { if (r) bad = 'threw ' + r + ' with no failed allocation'; break; }\n"
    "  seen++;\n"
    "  if (!(r instanceof Error) || !/out of memory/i.test(String(r.message))) { bad = 'allocation ' + at + ' failed but cancel() ended with ' + String(r); break; }\n"
    "  if (lv) { bad = 'allocation ' + at + ' failed and ' + lv + ' block(s) stayed allocated'; break; }\n"
    "}\n"
    "report(bad ? 'sweep: ' + bad : seen < 3 ? 'sweep: only ' + seen + ' allocations failed; want calloc + 2 strdup' : 'ok sweep: ' + seen + ' failed allocations each threw out of memory and freed everything');\n"
    "{ let r = null; failReactor(1); try { c.cancel(); } catch (e) { r = e; } const lv = liveCount(); failReactor(0); probe();\n"
    "  report(r instanceof Error && /reactor/.test(String(r.message)) && !lv ? 'ok no reactor: cancel() throws and frees host and CA' : 'no reactor: ' + String(r) + ', ' + lv + ' block(s) stayed allocated'); }\n"
    "for (let k = 0; k < 300 && !cancels; k++) await sleep(10);\n"
    "report(cancels ? 'ok the CancelRequest reaches the TLS server after the sweep' : 'no CancelRequest arrived after the sweep');\n"
    "c.close(); srv.close(); }\n";

int main(void)
{
    JSRuntime* rt = JS_NewRuntime();
    JSContext* ctx;
    JSValue g, v;
    alarm(120);
    js_std_set_worker_new_context_func(new_ctx);
    js_std_init_handlers(rt);
    ctx = new_ctx(rt);
    JS_SetModuleLoaderFunc2(rt, NULL, js_module_loader, js_module_check_attributes, NULL);
    js_std_add_helpers(ctx, 0, NULL);
    g = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, g, "probe", JS_NewCFunction(ctx, h_probe, "probe", 0));
    JS_SetPropertyStr(ctx, g, "failAlloc", JS_NewCFunction(ctx, h_fail_alloc, "failAlloc", 1));
    JS_SetPropertyStr(ctx, g, "allocFailed", JS_NewCFunction(ctx, h_alloc_failed, "allocFailed", 0));
    JS_SetPropertyStr(ctx, g, "liveCount", JS_NewCFunction(ctx, h_live, "liveCount", 0));
    JS_SetPropertyStr(ctx, g, "failReactor", JS_NewCFunction(ctx, h_fail_reactor, "failReactor", 1));
    JS_SetPropertyStr(ctx, g, "report", JS_NewCFunction(ctx, h_report, "report", 1));
    JS_FreeValue(ctx, g);
    v = js_std_await(ctx, JS_Eval(ctx, src, sizeof src - 1, "tests/postgres/<cancel-api>.js", JS_EVAL_TYPE_MODULE));
    if (JS_IsException(v)) {
        js_std_dump_error(ctx);
        fails++;
    }
    JS_FreeValue(ctx, v);
    if (js_std_loop(ctx)) {
        js_std_dump_error(ctx);
        fails++;
    }
    if (checks < 3) {
        printf("  FAIL: only %d of 3 checks reported\n", checks);
        fails++;
    }
    if (stale) {
        printf("  FAIL: %d runs left an exception pending\n", stale);
        fails++;
    }
    js_std_free_handlers(rt);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    printf("postgres_cancel_api: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
