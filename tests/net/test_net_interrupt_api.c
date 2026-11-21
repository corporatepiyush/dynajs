// covers: c:src/modules/dns/dns-server.c c:src/dyna-net-handler.c c:src/modules/dns/dns-module.c c:src/modules/dns/dns-cache.c c:src/modules/dns/dns-resolver.c c:src/modules/redis/redis-module.c c:src/modules/redis/redis-reply.c c:src/modules/postgres/postgres-module.c c:src/modules/postgres/postgres-query-queue.c c:src/modules/postgres/postgres-cps.c c:src/modules/postgres/postgres-protocol.c -- D50: a one-shot interrupt raised in a user callback a network module calls must end the run, never be dropped or turned into a rejection
// D50 (corenet). Reactor callbacks (a DNSServer handler, a resolver callback, Redis error/push handlers, PostgreSQL
// notice/notification handlers and row iterators) took the pending exception with JS_GetException and dropped it or
// settled a promise with it, so the uncatchable interrupt the embedder's handler raised once became a value and the
// script ran on; only the CLI's repeating --timeout-ms handler hid it. They now hand an uncatchable exception to the
// event loop (dyn_exc_drop / dyn_exc_forward), which ends the run. A case passes when the loop ends with an error and
// the script never set globalThis.leak (a catch or rejection handler, or a timer that only runs if the loop goes on).
// PostgreSQL cases get a live server from tests/lib/services_hold.js (a child over two pipes) and skip only when it
// cannot start; Redis cases talk to an in-script fake RESP3 server (FR), which can send pushes and big numbers.
// After each case the harness closes what the case handed to later() and drains the loop, as an embedder would: a
// client still mid-command when the runtime is freed after a loop error is a separate, still-open teardown leak (the runtime asserts on its gc_obj_list).
// Usage: test_net_interrupt_api [CASE-SUBSTRING]; run from the repo root.
#include "dynajs.h"
#include "dyna-libc.h"
#include "dyna-nat.h"

#include <spawn.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

static int armed, fired;
static int fails, checks, skips;

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

#define PRE                                                                                       \
    "import * as os from 'os';\n"                                                                 \
    "const spin = () => { arm(); for (;;) {} };\n"                                                \
    "const leak = (e) => { globalThis.leak = String(e); };\n"                                     \
    "const later = (ms, ...cl) => { const t = os.setTimeout(() => { if (globalThis.leak === undefined) globalThis.leak = 'timer'; for (const c of cl) try { c.close(); } catch (e) {} }, ms);\n" \
    "  globalThis.__cl = [{ close: () => os.clearTimeout(t) }, ...cl]; };\n" \
    "const H = '127.0.0.1';\n"                                                                    \
    "const FR = async (reply) => { const { TCPServer } = await import('dyna:net'); const s = new TCPServer({ port: 0 }); let buf = '';\n" \
    "  s.start({ data: (c, b) => { for (const x of b) buf += String.fromCharCode(x); let m;\n"   \
    "    while ((m = /^\\*(\\d+)\\r\\n/.exec(buf))) { let p = m[0].length; const a = [];\n"          \
    "      for (let k = 0; k < +m[1]; k++) { const j = buf.indexOf('\\r\\n', p); if (j < 0) return; const len = +buf.slice(p + 1, j); if (buf.length < j + 4 + len) return; a.push(buf.slice(j + 2, j + 2 + len)); p = j + 4 + len; }\n" \
    "      buf = buf.slice(p); const out = a[0].toUpperCase() === 'HELLO' ? '%1\\r\\n$5\\r\\nproto\\r\\n:3\\r\\n' : reply;\n" \
    "      c.write(new Uint8Array([...out].map((ch) => ch.charCodeAt(0)))); } } }); return s; };\n"

static const char* const cases[][2] = {
    { "dns: DNSServer handler spins", "const { DNSServer, DNSResolver } = await import('dyna:dns');\n"
                                      "const s = new DNSServer({ port: 0, host: H }); s.start(() => spin());\n"
                                      "new DNSResolver({ server: H, port: s.port, timeoutMs: 400 }).lookup('x.test').then(leak, leak); later(1500, s);\n" },
    { "dns: lookup callback spins", "const { DNSServer, DNSResolver } = await import('dyna:dns');\n"
                                    "const s = new DNSServer({ port: 0, host: H }); s.start(() => '192.0.2.1');\n"
                                    "const r = new DNSResolver({ server: H, port: s.port, timeoutMs: 400 }); r.lookup('x.test', () => spin()); later(1500, s, r);\n" },
    { "dns: cache-hit callback spins", "const { DNSServer, DNSResolver } = await import('dyna:dns');\n"
                                       "const s = new DNSServer({ port: 0, host: H }); s.start(() => '192.0.2.1');\n"
                                       "const r = new DNSResolver({ server: H, port: s.port, timeoutMs: 400, ttl: 60 });\n"
                                       "r.lookup('x.test').then(() => r.lookup('x.test', () => spin()), leak); later(1500, s, r);\n" },

    { "dns: inherited record getter spins in the cache clone", "const { DNSServer, DNSResolver } = await import('dyna:dns');\n"
                                                               "const s = new DNSServer({ port: 0, host: H }); s.start(() => '192.0.2.1');\n"
                                                               "const r = new DNSResolver({ server: H, port: s.port, timeoutMs: 400, ttl: 60 });\n"
                                                               "Object.defineProperty(Object.prototype, 'target', { configurable: true, get() { spin(); } });\n"
                                                               "r.lookup('x.test').then(leak, leak); later(1500, s, r);\n" },
    { "redis: command pending when a promise job spins", "const { Redis } = await import('dyna:redis');\n"
                                   "const s = await FR('>3\\r\\n$7\\r\\nmessage\\r\\n$2\\r\\nch\\r\\n$1\\r\\nx\\r\\n');\n"
                                   "const c = new Redis({ host: H, port: s.port }); c.command('PING'); globalThis.__nodrain = 1; os.setTimeout(() => Promise.resolve().then(() => spin()), 200);\n" },
    { "pg: query pending when a promise job spins", "if (!PGC) globalThis.skip = 'postgres unavailable: ' + PGWHY; else {\n"
                                                   "const { PostgreSQL } = await import('dyna:postgres'); const c = new PostgreSQL(PGC);\n"
                                                   "c.query('SELECT pg_sleep(2)'); globalThis.__nodrain = 1; os.setTimeout(() => Promise.resolve().then(() => spin()), 300); }\n" },
    { "redis: error handler spins", "const { Redis } = await import('dyna:redis');\n"
                                    "const c = new Redis({ host: H, port: 1, connectTimeoutMs: 300 }); c.on('error', () => spin()); later(1500, c);\n" },
    { "redis: push handler spins", "const { Redis } = await import('dyna:redis');\n"
                                   "const s = await FR('>3\\r\\n$7\\r\\nmessage\\r\\n$2\\r\\nch\\r\\n$1\\r\\nx\\r\\n+PONG\\r\\n');\n"
                                   "const c = new Redis({ host: H, port: s.port }); c.on('push', () => spin()); c.command('PING').then(leak, leak); later(1500, c, s);\n" },
    { "redis: BigInt for a RESP3 big number spins", "const { Redis } = await import('dyna:redis');\n"
                                                    "const s = await FR('(123456789012345678901234567890\\r\\n');\n"
                                                    "const c = new Redis({ host: H, port: s.port, bigint: true }); c.on('error', () => {});\n"
                                                    "c.command('PING').then(() => { globalThis.BigInt = function () { spin(); }; return c.command('PING'); }).then(leak, leak); later(1500, c, s);\n" },
    { "redis: BigInt getter spins for a RESP3 big number", "const { Redis } = await import('dyna:redis');\n"
                                                           "const s = await FR('(123456789012345678901234567890\\r\\n');\n"
                                                           "const c = new Redis({ host: H, port: s.port, bigint: true }); c.on('error', () => {});\n"
                                                           "c.command('PING').then(() => { Object.defineProperty(globalThis, 'BigInt', { configurable: true, get() { spin(); } }); return c.command('PING'); }).then(leak, leak); later(1500, c, s);\n" },
    { "pg: numeric constructor spins while a DataRow decodes", "if (!PGC) globalThis.skip = 'postgres unavailable: ' + PGWHY; else {\n"
                                                              "const { PostgreSQL } = await import('dyna:postgres'); const c = new PostgreSQL({ ...PGC, numeric: class { constructor() { spin(); } } });\n"
                                                              "c.query('SELECT (i * 1.5)::numeric AS n FROM generate_series(1, 3) i').then(leak, leak); later(3000, c); }\n" },
    { "pg: numeric constructor spins inside an array element", "if (!PGC) globalThis.skip = 'postgres unavailable: ' + PGWHY; else {\n"
                                                               "const { PostgreSQL } = await import('dyna:postgres'); const c = new PostgreSQL({ ...PGC, arrays: 'array', numeric: class { constructor() { spin(); } } });\n"
                                                               "c.query('SELECT ARRAY[1.5, 2.5]::numeric[] AS a FROM generate_series(1, 3) i').then(leak, leak); later(3000, c); }\n" },
    { "pg: numeric constructor spins inside a numrange bound", "if (!PGC) globalThis.skip = 'postgres unavailable: ' + PGWHY; else {\n"
                                                              "const { PostgreSQL } = await import('dyna:postgres'); const c = new PostgreSQL({ ...PGC, ranges: 'object', numeric: class { constructor() { spin(); } } });\n"
                                                              "c.query('SELECT numrange(1.5, 2.5) AS r FROM generate_series(1, 3) i').then(leak, leak); later(3000, c); }\n" },
    { "pg: url toString spins in the constructor", "const { PostgreSQL } = await import('dyna:postgres');\n"
                                                   "try { new PostgreSQL({ url: { toString() { spin(); } } }); } catch (e) { leak(e); }\n" },
    { "pg: notice handler spins", "if (!PGC) globalThis.skip = 'postgres unavailable: ' + PGWHY; else {\n"
                                  "const { PostgreSQL } = await import('dyna:postgres'); const c = new PostgreSQL(PGC);\n"
                                  "c.on('notice', () => spin()); c.query(\"DO $$BEGIN RAISE NOTICE 'n'; END$$\").then(leak, leak); later(3000, c); }\n" },
    { "pg: notification handler spins", "if (!PGC) globalThis.skip = 'postgres unavailable: ' + PGWHY; else {\n"
                                        "const { PostgreSQL } = await import('dyna:postgres'); const c = new PostgreSQL(PGC);\n"
                                        "c.on('notification', () => spin()); c.query('LISTEN ch').then(() => c.query(\"NOTIFY ch, 'x'\")).then(() => c.query('select 1')).then(leak, leak); later(3000, c); }\n" },
    { "pg: Promise.prototype.then getter spins under queryIter", "if (!PGC) globalThis.skip = 'postgres unavailable: ' + PGWHY; else {\n"
                                                                "const { PostgreSQL } = await import('dyna:postgres'); const c = new PostgreSQL(PGC);\n"
                                                                "c.query('select 1').then(() => { const d = Object.getOwnPropertyDescriptor(Promise.prototype, 'then'); let once = 0;\n"
                                                                "  Object.defineProperty(Promise.prototype, 'then', { configurable: true, get() { if (!once++) spin(); return d.value; } });\n"
                                                                "  const n = c.queryIter('select 1').next(); globalThis.leak = 'ran on after next()'; return n; }).catch(leak); later(3000, c); }\n" },
    { "pg: queryIter's query() spins", "if (!PGC) globalThis.skip = 'postgres unavailable: ' + PGWHY; else {\n"
                                       "const { PostgreSQL } = await import('dyna:postgres'); const c = new PostgreSQL(PGC);\n"
                                       "c.query('select 1').then(() => { const q = c.query; let once = 0; c.query = function (...a) { if (once++) return q.apply(c, a); spin(); }; const n = c.queryIter('select 1').next(); globalThis.leak = 'ran on after next()'; return n; }).then(leak, leak); later(3000, c); }\n" },
    { "pg: channelBinding getter spins", "if (!PGC) globalThis.skip = 'postgres unavailable: ' + PGWHY; else {\n"
                                         "const { PostgreSQL } = await import('dyna:postgres');\n"
                                         "try { const c = new PostgreSQL({ ...PGC, get channelBinding() { spin(); } }); c.close(); } catch (e) { leak(e); } leak('after'); }\n" },
    { "pg: sslNegotiation getter spins", "if (!PGC) globalThis.skip = 'postgres unavailable: ' + PGWHY; else {\n"
                                         "const { PostgreSQL } = await import('dyna:postgres');\n"
                                         "try { const c = new PostgreSQL({ ...PGC, get sslNegotiation() { spin(); } }); c.close(); } catch (e) { leak(e); } leak('after'); }\n" },
    { "pg: copyFrom source iterator spins", "if (!PGC) globalThis.skip = 'postgres unavailable: ' + PGWHY; else {\n"
                                           "const { PostgreSQL } = await import('dyna:postgres'); const c = new PostgreSQL(PGC);\n"
                                           "c.query('CREATE TEMP TABLE ci(a int)').then(() => c.copyFrom('COPY ci FROM STDIN', { [Symbol.iterator]() { return { next() { spin(); return { done: true }; } }; } })).then(leak, leak); later(3000, c); }\n" },
};

static char svc_json[2048] = "{\"postgres\":null,\"why\":{\"postgres\":\"not started\"}}";
static pid_t svc_pid = -1;
static int svc_in = -1;

static void svc_start(void)
{
    int to[2], from[2];
    static char a0[] = "./dynajs", a1[] = "--std", a2[] = "tests/lib/services_hold.js", a3[] = "postgres";
    char* av[] = { a0, a1, a2, a3, NULL };
    posix_spawn_file_actions_t fa;
    FILE* f;
    if (pipe(to) || pipe(from))
        return;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, to[0], 0);
    posix_spawn_file_actions_adddup2(&fa, from[1], 1);
    posix_spawn_file_actions_addclose(&fa, to[1]);
    posix_spawn_file_actions_addclose(&fa, from[0]);
    if (posix_spawn(&svc_pid, av[0], &fa, NULL, av, environ))
        svc_pid = -1;
    posix_spawn_file_actions_destroy(&fa);
    close(to[0]);
    close(from[1]);
    svc_in = to[1];
    f = fdopen(from[0], "r");
    if (f) {
        if (!fgets(svc_json, sizeof(svc_json), f))
            snprintf(svc_json, sizeof(svc_json), "{\"postgres\":null,\"why\":{\"postgres\":\"services_hold.js printed nothing\"}}");
        fclose(f);
    }
}

static void svc_stop(void)
{
    int st;
    if (svc_in >= 0)
        close(svc_in);
    if (svc_pid > 0)
        waitpid(svc_pid, &st, 0);
    svc_in = -1;
    svc_pid = -1;
}

static int no_drain(JSContext* ctx, JSValueConst g)
{
    JSValue v = JS_GetPropertyStr(ctx, g, "__nodrain");
    int r = JS_ToBool(ctx, v) > 0;
    JS_FreeValue(ctx, v);
    return r;
}

static void run_case(const char* name, const char* body)
{
    JSRuntime* rt = JS_NewRuntime();
    JSContext* ctx;
    JSValue v, g, l, sk;
    char src[16384];
    int rc;
    armed = 0;
    fired = 0;
    js_std_set_worker_new_context_func(new_ctx);
    js_std_init_handlers(rt);
    ctx = new_ctx(rt);
    JS_SetModuleLoaderFunc2(rt, NULL, js_module_loader, js_module_check_attributes, NULL);
    JS_SetInterruptHandler(rt, handler, NULL);
    js_std_add_helpers(ctx, 0, NULL);
    g = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, g, "arm", JS_NewCFunction(ctx, h_arm, "arm", 0));
    snprintf(src, sizeof(src), "%sconst SVC = %s; const PGC = SVC.postgres, PGWHY = (SVC.why || {}).postgres;\n%s", PRE, svc_json, body);
    v = JS_Eval(ctx, src, strlen(src), "tests/net/<case>.js", JS_EVAL_TYPE_MODULE);
    v = js_std_await(ctx, v);
    rc = JS_IsException(v) ? -1 : 0;
    if (rc && !fired)
        js_std_dump_error(ctx);
    else if (rc)
        JS_FreeValue(ctx, JS_GetException(ctx));
    JS_FreeValue(ctx, v);
    if (!rc)
        rc = js_std_loop(ctx);
    sk = JS_GetPropertyStr(ctx, g, "skip");
    l = JS_GetPropertyStr(ctx, g, "leak");
    if (JS_IsException(sk) || JS_IsException(l)) {
        check(0, name, "reading skip/leak threw");
        JS_FreeValue(ctx, JS_GetException(ctx));
    } else if (JS_IsString(sk)) {
        const char* s = JS_ToCString(ctx, sk);
        printf("  SKIP: [%s] %s\n", name, s ? s : "?");
        skips++;
        if (s)
            JS_FreeCString(ctx, s);
    } else {
        check(fired, name, "the handler fired");
        check(rc != 0, name, "the run ended with the interrupt");
        if (!JS_IsUndefined(l)) {
            const char* s = JS_ToCString(ctx, l);
            char what[256];
            snprintf(what, sizeof(what), "user code observed the interrupt (leak = %s)", s ? s : "?");
            check(0, name, what);
            if (s)
                JS_FreeCString(ctx, s);
            else
                JS_FreeValue(ctx, JS_GetException(ctx));
        }
    }
    if (!no_drain(ctx, g)) {
        static const char drain[] = "for (const c of globalThis.__cl || []) try { c.close(); } catch (e) {}";
        JSValue d = JS_Eval(ctx, drain, strlen(drain), "<drain>", JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(d))
            js_std_dump_error(ctx);
        JS_FreeValue(ctx, d);
        if (js_std_loop(ctx))
            JS_FreeValue(ctx, JS_GetException(ctx));
    }
    JS_FreeValue(ctx, sk);
    JS_FreeValue(ctx, l);
    JS_FreeValue(ctx, g);
    js_std_free_handlers(rt);
    JS_FreeContext(ctx);
    JS_SetExitTeardown(rt, 1);
    JS_FreeRuntime(rt);
}

int main(int argc, char** argv)
{
    size_t i;
    alarm(170);
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        if (!strncmp(cases[i][0], "pg:", 3) && (argc < 2 || strstr(cases[i][0], argv[1]))) {
            svc_start();
            break;
        }
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        if (argc > 1 && !strstr(cases[i][0], argv[1]))
            continue;
        alarm(40);
        printf("case: %s\n", cases[i][0]);
        fflush(stdout);
        run_case(cases[i][0], cases[i][1]);
    }
    svc_stop();
    printf("net_interrupt_api: %d checks, %d failed, %d skipped\n", checks, fails, skips);
    return fails ? 1 : 0;
}
