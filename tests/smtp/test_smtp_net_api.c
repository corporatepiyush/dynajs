// covers: c:src/modules/smtp/smtp-module.c c:src/modules/smtp/smtp-sopts.c c:src/modules/smtp/smtp-server.c c:src/modules/net-tcp/net-tcp-module.c c:src/modules/net-tcp/net-tcp-connect.c -- rule 18 embedder rows for dyna:smtp SmtpClient/SmtpServer and dyna:net TCPServer/connect/connectHappy: a one-shot interrupt in an option getter, valueOf, Proxy trap, server hook or verdict getter, and forced allocation failure (malloc hook)
// D49/D50 (corenet b). SMTP option reads used the value of a throwing getter (a TypeError replaced the error) and went on
// reading later options with it pending; SmtpServer verdict and SCRAM credential getters dropped the pending exception and a
// hook throw became a 451, so a one-shot interrupt raised there let the session and the script run on. TCPServer/connect read
// path and tls without an exception check. Interrupt cases pass when the run ends with the error and the script never set
// globalThis.leak. The malloc hook fails the Nth libc allocation in the SMTP option/message parsers and TCPServer: every
// failure must throw "out of memory" with nothing left pending (send({to:'x'}) used to return an exception with none set),
// and the module must work afterwards.
// Usage: test_smtp_net_api [CASE-SUBSTRING]; run from the repo root.
#include "dynajs.h"
#include "dyna-libc.h"
#include "dyna-nat.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int armed, fired, stale;
static int fails, checks;

static int handler(JSRuntime* rt, void* opaque)
{
    (void)rt;
    (void)opaque;
    if (armed && !fired) {
        fired = 1;
        return 1;
    }
    return 0;
}

static JSValue h_arm(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)ctx;
    (void)this_val;
    (void)argc;
    (void)argv;
    armed = 1;
    return JS_UNDEFINED;
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

static unsigned long sn_alloc_n, sn_fail_at;
static int sn_failed;

#undef malloc
#undef calloc
#undef realloc
#undef strdup
void* malloc(size_t n);
void* calloc(size_t k, size_t n);
void* realloc(void* p, size_t n);

static int sn_fail_now(void)
{
    if (sn_fail_at && ++sn_alloc_n == sn_fail_at) {
        sn_failed = 1;
        return 1;
    }
    return 0;
}

void* sn_t_malloc(size_t n)
{
    return sn_fail_now() ? NULL : malloc(n);
}

void* sn_t_calloc(size_t k, size_t n)
{
    return sn_fail_now() ? NULL : calloc(k, n);
}

void* sn_t_realloc(void* p, size_t n)
{
    return sn_fail_now() ? NULL : realloc(p, n);
}

char* sn_t_strdup(const char* s)
{
    size_t n = strlen(s) + 1;
    char* d;
    if (sn_fail_now())
        return NULL;
    d = (char*)malloc(n);
    if (d)
        memcpy(d, s, n);
    return d;
}

static JSValue h_fail_alloc(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    int64_t at = 0;
    (void)this_val;
    if (argc < 1 || JS_ToInt64(ctx, &at, argv[0]))
        return JS_EXCEPTION;
    sn_alloc_n = 0;
    sn_failed = 0;
    sn_fail_at = at > 0 ? (unsigned long)at : 0;
    return JS_UNDEFINED;
}

static JSValue h_alloc_failed(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)this_val;
    (void)argc;
    (void)argv;
    return JS_NewBool(ctx, sn_failed);
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

static void check(int ok, const char* name, const char* what)
{
    checks++;
    if (!ok) {
        fails++;
        printf("  FAIL: [%s] %s\n", name, what);
    }
}

#define PRE                                                                                          \
    "import * as os from 'os';\n"                                                                    \
    "import { SmtpClient, SmtpServer } from 'dyna:smtp';\n"                                          \
    "import { TCPServer, connectHappy } from 'dyna:net';\n"                                          \
    "const spin = () => { arm(); for (;;) {} };\n"                                                   \
    "const leak = (e) => { globalThis.leak = String(e); };\n"                                        \
    "const keep = (...cl) => { globalThis.__cl = (globalThis.__cl || []).concat(cl); };\n"           \
    "const later = (ms, ...cl) => { const t = os.setTimeout(() => { if (globalThis.leak === undefined) globalThis.leak = 'timer'; }, ms);\n" \
    "  keep({ close: () => os.clearTimeout(t) }, ...cl); };\n"                                       \
    "const H = '127.0.0.1';\n"                                                                       \
    "const at0 = (a) => new Proxy(a, { get(t, k) { if (k === '0') spin(); return Reflect.get(t, k); } });\n" \
    "const srv = (o) => { const s = new SmtpServer({ port: 0, tls: 'none', name: 'mx.test', host: H, ...o }); s.listen(); return s; };\n" \
    "const cli = (s, o) => new SmtpClient({ host: H, port: s.port, tls: 'none', name: 'c.test', ...o });\n" \
    "const M = { from: 'a@b.test', to: 'x@y.test', data: 'Subject: t\\r\\n\\r\\nhi\\r\\n' };\n"     \
    "const sync = (f) => { try { const r = f(); if (r && r.close) keep(r); if (r && r.catch) r.catch(() => {}); } catch (e) { leak(e); } leak('after'); };\n" \
    "const C = () => { const c = new SmtpClient({ host: H, port: 1, tls: 'none', connectTimeoutMs: 200 }); keep(c); return c; };\n"

static const char* const cases[][2] = {
    { "SmtpClient: host getter spins", "sync(() => new SmtpClient({ get host() { spin(); }, tls: 'none' }));\n" },
    { "SmtpClient: port valueOf spins", "sync(() => new SmtpClient({ host: H, tls: 'none', port: { valueOf() { spin(); } } }));\n" },
    { "SmtpClient: mechanisms element spins", "sync(() => new SmtpClient({ host: H, tls: 'none', user: 'u', pass: 'p', mechanisms: at0(['PLAIN']) }));\n" },
    { "SmtpClient: insecureAuth getter spins", "sync(() => new SmtpClient({ host: H, tls: 'none', get insecureAuth() { spin(); } }));\n" },
    { "SmtpClient.send: to[] element spins", "const c = C(); sync(() => c.send({ ...M, to: at0(['x@y.test']) }));\n" },
    { "SmtpClient.send: data getter spins", "const c = C(); sync(() => c.send({ from: M.from, to: M.to, get data() { spin(); } }));\n" },
    { "SmtpClient.send: bdat getter spins", "const c = C(); sync(() => c.send({ ...M, get bdat() { spin(); } }));\n" },
    { "SmtpClient.send: dsn.notify getter spins", "const c = C(); sync(() => c.send({ ...M, dsn: { get notify() { spin(); } } }));\n" },
    { "SmtpClient.send: dsn.notify element spins", "const c = C(); sync(() => c.send({ ...M, dsn: { notify: at0(['SUCCESS']) } }));\n" },
    { "SmtpClient.send: dsn.orcpt getter spins", "const c = C(); sync(() => c.send({ ...M, dsn: { get orcpt() { spin(); } } }));\n" },
    { "SmtpServer: name getter spins", "sync(() => new SmtpServer({ port: 0, tls: 'none', get name() { spin(); } }));\n" },
    { "SmtpServer: disable element spins", "sync(() => new SmtpServer({ port: 0, tls: 'none', disable: at0(['DSN']) }));\n" },
    { "SmtpServer: requireAuth getter spins", "sync(() => new SmtpServer({ port: 0, tls: 'none', get requireAuth() { spin(); } }));\n" },
    { "SmtpServer: chunkSize valueOf spins", "sync(() => new SmtpServer({ port: 0, tls: 'none', chunkSize: { valueOf() { spin(); } } }));\n" },
    { "SmtpServer: onData getter spins", "sync(() => new SmtpServer({ port: 0, tls: 'none', get onData() { spin(); } }));\n" },
    { "TCPServer: path getter spins", "sync(() => new TCPServer({ get path() { spin(); } }));\n" },
    { "TCPServer: tls getter spins", "sync(() => new TCPServer({ port: 0, get tls() { spin(); } }));\n" },
    { "connect: path getter spins", "sync(() => TCPServer.connect({ get path() { spin(); } }, { error() {} }));\n" },
    { "connect: tls getter spins", "sync(() => TCPServer.connect({ host: H, port: 1, get tls() { spin(); } }, { error() {} }));\n" },
    { "connectHappy: tls getter spins", "sync(() => connectHappy(H, 1, { get tls() { spin(); } }, { connect() {} }));\n" },
    { "SmtpServer: onMail hook spins", "const s = srv({ onMail: () => spin() }); const c = cli(s); c.send(M).then(leak, leak); later(2000, c, s);\n" },
    { "SmtpServer: verdict code getter spins", "const s = srv({ onMail: () => ({ get code() { spin(); }, message: 'x' }) }); const c = cli(s); c.send(M).then(leak, leak); later(2000, c, s);\n" },
    { "SmtpServer: verdict enhanced getter spins", "const s = srv({ onRcpt: () => ({ code: 250, message: 'ok', get enhanced() { spin(); } }) }); const c = cli(s); c.send(M).then(leak, leak); later(2000, c, s);\n" },
    { "SmtpServer: async verdict getter spins", "const s = srv({ onData: async () => ({ code: 250, get message() { spin(); } }) }); const c = cli(s); c.send(M).then(leak, leak); later(2000, c, s);\n" },
    { "SmtpServer: SCRAM salt getter spins", "const s = srv({ insecureAuth: true, onAuth: () => ({ iterations: 4096, get salt() { spin(); }, storedKey: new Uint8Array(32), serverKey: new Uint8Array(32) }) });\n"
                                             "const c = cli(s, { user: 'u', pass: 'p', mechanisms: ['SCRAM-SHA-256'], insecureAuth: true }); c.send(M).then(leak, leak); later(2000, c, s);\n" },
    { "SmtpServer: SCRAM iterations getter spins", "const s = srv({ insecureAuth: true, onAuth: () => ({ get iterations() { spin(); }, salt: 's', storedKey: new Uint8Array(32), serverKey: new Uint8Array(32) }) });\n"
                                                   "const c = cli(s, { user: 'u', pass: 'p', mechanisms: ['SCRAM-SHA-256'], insecureAuth: true }); c.send(M).then(leak, leak); later(2000, c, s);\n" },
};

static JSRuntime* new_rt(JSContext** pctx)
{
    JSRuntime* rt = JS_NewRuntime();
    JSContext* ctx;
    JSValue g;
    js_std_set_worker_new_context_func(new_ctx);
    js_std_init_handlers(rt);
    ctx = new_ctx(rt);
    JS_SetModuleLoaderFunc2(rt, NULL, js_module_loader, js_module_check_attributes, NULL);
    JS_SetInterruptHandler(rt, handler, NULL);
    js_std_add_helpers(ctx, 0, NULL);
    g = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, g, "arm", JS_NewCFunction(ctx, h_arm, "arm", 0));
    JS_SetPropertyStr(ctx, g, "probe", JS_NewCFunction(ctx, h_probe, "probe", 0));
    JS_SetPropertyStr(ctx, g, "failAlloc", JS_NewCFunction(ctx, h_fail_alloc, "failAlloc", 1));
    JS_SetPropertyStr(ctx, g, "allocFailed", JS_NewCFunction(ctx, h_alloc_failed, "allocFailed", 0));
    JS_FreeValue(ctx, g);
    *pctx = ctx;
    return rt;
}

static void drain(JSContext* ctx)
{
    static const char src[] = "for (const c of globalThis.__cl || []) try { c.close(); } catch (e) {} globalThis.__cl = [];";
    JSValue d = JS_Eval(ctx, src, strlen(src), "<drain>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(d))
        js_std_dump_error(ctx);
    JS_FreeValue(ctx, d);
    if (js_std_loop(ctx))
        JS_FreeValue(ctx, JS_GetException(ctx));
}

static void free_rt(JSRuntime* rt, JSContext* ctx)
{
    js_std_free_handlers(rt);
    JS_FreeContext(ctx);
    JS_SetExitTeardown(rt, 1);
    JS_FreeRuntime(rt);
}

static void run_case(const char* name, const char* body)
{
    JSContext* ctx;
    JSRuntime* rt;
    JSValue v, g, l;
    char src[8192];
    int rc;
    armed = 0;
    fired = 0;
    rt = new_rt(&ctx);
    snprintf(src, sizeof(src), "%s%s", PRE, body);
    v = js_std_await(ctx, JS_Eval(ctx, src, strlen(src), "tests/smtp/<case>.js", JS_EVAL_TYPE_MODULE));
    rc = JS_IsException(v) ? -1 : 0;
    if (rc && !fired)
        js_std_dump_error(ctx);
    else if (rc)
        JS_FreeValue(ctx, JS_GetException(ctx));
    JS_FreeValue(ctx, v);
    if (!rc)
        rc = js_std_loop(ctx);
    g = JS_GetGlobalObject(ctx);
    l = JS_GetPropertyStr(ctx, g, "leak");
    check(fired, name, "the handler fired");
    check(rc != 0, name, "the run ended with the interrupt");
    if (JS_IsException(l)) {
        check(0, name, "reading leak threw");
        JS_FreeValue(ctx, JS_GetException(ctx));
    } else if (!JS_IsUndefined(l)) {
        const char* s = JS_ToCString(ctx, l);
        char what[256];
        snprintf(what, sizeof(what), "user code observed the interrupt (leak = %s)", s ? s : "?");
        check(0, name, what);
        if (s)
            JS_FreeCString(ctx, s);
        else
            JS_FreeValue(ctx, JS_GetException(ctx));
    }
    drain(ctx);
    JS_FreeValue(ctx, l);
    JS_FreeValue(ctx, g);
    free_rt(rt, ctx);
}

#define OPS                                                                                                   \
    "const { SmtpClient, SmtpServer } = await import('dyna:smtp'); const { TCPServer } = await import('dyna:net');\n" \
    "const H = '127.0.0.1', live = [];\n"                                                                     \
    "const M = { from: 'a@b.test', to: ['x@y.test', 'z@y.test'], data: 'hi', bdat: false, dsn: { ret: 'HDRS', envid: 'e1', notify: ['SUCCESS', 'FAILURE'], orcpt: true } };\n" \
    "const sc = new SmtpClient({ host: H, port: 1, tls: 'none', connectTimeoutMs: 200 }); live.push(sc);\n"   \
    "const ops = {\n"                                                                                         \
    "  SmtpClient: () => new SmtpClient({ host: H, port: 1, tls: 'none', name: 'c.test', user: 'u', pass: 'p', authzid: 'z', mechanisms: ['PLAIN'] }),\n" \
    "  SmtpServer: () => new SmtpServer({ port: 0, tls: 'none', name: 'mx.test', host: H, disable: ['DSN'] }),\n" \
    "  send: () => sc.send(M),\n"                                                                             \
    "  sendOne: () => sc.send({ from: 'a@b.test', to: 'x@y.test', data: new Uint8Array(3) }),\n"             \
    "  TCPServer: () => new TCPServer({ port: 0, path: '.obj/smtp_net_api.sock' }) };\n"                      \
    "const done = (r) => { if (r && typeof r.close === 'function') r.close(); if (r && typeof r.catch === 'function') r.catch(() => {}); };\n"

static const char hook_src[] = OPS
    "const bad = [], seen = {};\n"
    "for (const [name, op] of Object.entries(ops)) {\n"
    "  seen[name] = 0;\n"
    "  for (let at = 1; at < 64; at++) {\n"
    "    let r; failAlloc(at);\n"
    "    try { r = op(); } catch (e) { r = e; }\n"
    "    const failed = allocFailed(); failAlloc(0); probe();\n"
    "    if (!failed) { if (r instanceof Error) bad.push(name + ' threw ' + r + ' with no failed allocation'); done(r); break; }\n"
    "    seen[name]++;\n"
    "    if (!(r instanceof Error) || !/out of memory/.test(String(r.message))) { bad.push(name + ' at ' + at + ' returned ' + String(r)); done(r); }\n"
    "  }\n"
    "  let r; try { r = op(); done(r); } catch (e) { bad.push(name + ' broken after the sweep: ' + e); }\n"
    "}\n"
    "for (const k in seen) if (!seen[k]) bad.push(k + ': no allocation failed');\n"
    "for (const c of live) c.close();\n"
    "globalThis.res = bad.length ? bad.slice(0, 4).join('; ') : 'ok ' + JSON.stringify(seen);\n";


static void run_hook(void)
{
    JSContext* ctx;
    JSRuntime* rt = new_rt(&ctx);
    JSValue v, g, r;
    const char* s;
    stale = 0;
    v = js_std_await(ctx, JS_Eval(ctx, hook_src, sizeof hook_src - 1, "<hook>", JS_EVAL_TYPE_MODULE));
    if (JS_IsException(v))
        js_std_dump_error(ctx);
    JS_FreeValue(ctx, v);
    if (js_std_loop(ctx))
        js_std_dump_error(ctx);
    g = JS_GetGlobalObject(ctx);
    r = JS_GetPropertyStr(ctx, g, "res");
    s = JS_ToCString(ctx, r);
    printf("alloc hook: %s\n", s ? s : "(no result)");
    check(s && !strncmp(s, "ok ", 3), "alloc hook", "every failing allocation throws out of memory and the module works afterwards");
    check(stale == 0, "alloc hook", "no call returned with an exception pending");
    if (s)
        JS_FreeCString(ctx, s);
    else
        JS_FreeValue(ctx, JS_GetException(ctx));
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, g);
    free_rt(rt, ctx);
}

int main(int argc, char** argv)
{
    size_t i;
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        if (argc > 1 && !strstr(cases[i][0], argv[1]))
            continue;
        alarm(40);
        printf("case: %s\n", cases[i][0]);
        fflush(stdout);
        run_case(cases[i][0], cases[i][1]);
    }
    alarm(120);
    if (argc < 2 || strstr("alloc hook", argv[1]))
        run_hook();
    printf("smtp_net_api: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
