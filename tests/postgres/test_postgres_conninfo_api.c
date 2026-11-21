// covers: c:src/modules/postgres/postgres-conninfo.c -- rule 18 forced allocation failure for the url, env and .pgpass paths of new PostgreSQL(), then pg_pgpass_read edges and cost
// The hook fails the Nth allocation that any postgres-*.c makes (all are built with the hooks, so a block freed in another file is untracked): in the constructor it must throw out of memory, on the I/O pool (.pgpass read)
// the first query must reject with out of memory, and everything is freed (a live-pointer table on the hooks; macOS has no LSan); the read must allocate off the JS thread and hold the reactor (1000 constructor+close free it 0 times); a refused connect with a queued query leaks nothing.
// Then .pgpass reads: a match, group/other bits, a directory, a FIFO (must not block), a NUL byte, exactly 1 MiB and 1 MiB + 1; PGPASS-COST prints ns per lookup for
// a missing, a 5-line and a 10000-line file. Rule 18 one-shot interrupt, each in its own runtime: a timer spins while attempt 2 of 3 hangs, and while
// attempt 2's .pgpass read is stalled on the I/O pool; the run must end uncatchably, close must start no attempt 3, and nothing may stay allocated.
// Combined mutants: case 2 = orphaning off + the done-callback guard off (ASan UAF in pg_pp_done, run alone first). Case 1: every closed-client guard on the host loop off stays green, since close frees
// the client, unhooks the tick and closes the fd; those guards off + teardown leaving the tick hooked and the fd open goes red (ASan UAF in pg_on_recv). argv[1] runs only the matching interrupt cases.
// Teardown: a second runtime freed without close() while a postgres client (or a redis client) is connecting with a query queued must not abort and must free every block;
// the shutdown sweep releases the queued queries' promise state (mutant: no drop in the sweep). The interrupt teardown case queues a query too.
// HOME points at a private temp dir. Run from the repo root.
#include "dynajs.h"
#include "dyna-libc.h"
#include "dyna-nat.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#undef malloc
#undef calloc
#undef realloc
#undef strdup
#undef free
void* malloc(size_t n);
void* calloc(size_t k, size_t n);
void* realloc(void* p, size_t n);
char* strdup(const char* s);
void free(void* p);
int pg_pgpass_read(const char* file, const char* host, const char* port, const char* db, const char* user, char** out);

static int stale, fails, checks, reactor_frees;
static unsigned long ci_alloc_n, ci_fail_at;
static int ci_failed, ci_nlive, ci_failed_off;
static unsigned long ci_off_n, ci_off_last, ci_stall_ms, ci_stalling, ci_stall_armed;
static uint64_t ci_stall_from;
static int ci_armed, ci_fired, ci_hits, ci_nlater;
static JSValue ci_later[8];
static const char* ci_filter;
static int ci_cnts = -1, ci_cntp;
static void* ci_live[256];
static char dir[512];
static pthread_mutex_t ci_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_t ci_main;

static int ci_fail_now(void)
{
    int off = !pthread_equal(pthread_self(), ci_main);
    ++ci_alloc_n;
    if (off) {
        ci_off_n++;
        ci_off_last = ci_alloc_n;
    }
    if (ci_fail_at && ci_alloc_n == ci_fail_at) {
        ci_failed = 1;
        ci_failed_off = off;
        return 1;
    }
    return 0;
}

static uint64_t ci_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void* ci_track(void* p)
{
    if (p && ci_nlive < 256)
        ci_live[ci_nlive++] = p;
    return p;
}

static void ci_untrack(void* p)
{
    int i;
    for (i = 0; i < ci_nlive; i++)
        if (ci_live[i] == p) {
            ci_live[i] = ci_live[--ci_nlive];
            return;
        }
}

void* ci_t_malloc(size_t n)
{
    void* p;
    if (ci_stall_ms && !pthread_equal(pthread_self(), ci_main) && ci_now_ms() >= ci_stall_from && __atomic_exchange_n(&ci_stall_armed, 0, __ATOMIC_SEQ_CST)) {
        __atomic_store_n(&ci_stalling, 1, __ATOMIC_SEQ_CST);
        usleep((useconds_t)(ci_stall_ms * 1000));
        __atomic_store_n(&ci_stalling, 0, __ATOMIC_SEQ_CST);
    }
    pthread_mutex_lock(&ci_mu);
    p = ci_fail_now() ? NULL : ci_track(malloc(n));
    pthread_mutex_unlock(&ci_mu);
    return p;
}

void* ci_t_calloc(size_t k, size_t n)
{
    void* p;
    pthread_mutex_lock(&ci_mu);
    p = ci_fail_now() ? NULL : ci_track(calloc(k, n));
    pthread_mutex_unlock(&ci_mu);
    return p;
}

void* ci_t_realloc(void* p, size_t n)
{
    void* q = NULL;
    pthread_mutex_lock(&ci_mu);
    if (!ci_fail_now()) {
        q = realloc(p, n);
        if (q) {
            ci_untrack(p);
            ci_track(q);
        }
    }
    pthread_mutex_unlock(&ci_mu);
    return q;
}

char* ci_t_strdup(const char* s)
{
    char* p;
    pthread_mutex_lock(&ci_mu);
    p = ci_fail_now() ? NULL : (char*)ci_track(strdup(s));
    pthread_mutex_unlock(&ci_mu);
    return p;
}

void ci_t_free(void* p)
{
    pthread_mutex_lock(&ci_mu);
    ci_untrack(p);
    pthread_mutex_unlock(&ci_mu);
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
    pthread_mutex_lock(&ci_mu);
    ci_alloc_n = 0;
    ci_failed = 0;
    ci_failed_off = 0;
    ci_off_n = 0;
    ci_off_last = 0;
    ci_nlive = 0;
    ci_fail_at = at > 0 ? (unsigned long)at : 0;
    pthread_mutex_unlock(&ci_mu);
    return JS_UNDEFINED;
}

static JSValue h_off(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    JSValue a = JS_NewArray(ctx);
    (void)this_val;
    (void)argc;
    (void)argv;
    if (JS_IsException(a))
        return a;
    pthread_mutex_lock(&ci_mu);
    JS_SetPropertyUint32(ctx, a, 0, JS_NewBool(ctx, ci_failed_off));
    JS_SetPropertyUint32(ctx, a, 1, JS_NewInt64(ctx, (int64_t)ci_off_n));
    JS_SetPropertyUint32(ctx, a, 2, JS_NewInt64(ctx, (int64_t)ci_off_last));
    pthread_mutex_unlock(&ci_mu);
    return a;
}

static void h_reactor_freed(void)
{
    reactor_frees++;
}

static JSValue h_rfrees(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)this_val;
    (void)argc;
    (void)argv;
    return JS_NewInt32(ctx, reactor_frees);
}

static int ci_irq(JSRuntime* rt, void* opaque)
{
    (void)rt;
    (void)opaque;
    if (!ci_armed)
        return 0;
    ci_armed = 0;
    ci_fired = 1;
    return 1;
}

static JSValue h_arm(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)ctx;
    (void)this_val;
    (void)argc;
    (void)argv;
    ci_armed = 1;
    return JS_UNDEFINED;
}

static JSValue h_hit(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)ctx;
    (void)this_val;
    (void)argc;
    (void)argv;
    ci_hits++;
    return JS_UNDEFINED;
}

static JSValue h_stall(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    int64_t a = 0, b = 0;
    (void)this_val;
    if (argc < 2 || JS_ToInt64(ctx, &a, argv[0]) || JS_ToInt64(ctx, &b, argv[1]))
        return JS_EXCEPTION;
    ci_stall_from = ci_now_ms() + (uint64_t)a;
    ci_stall_armed = 1;
    ci_stall_ms = (unsigned long)b;
    return JS_UNDEFINED;
}

static JSValue h_stalling(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)this_val;
    (void)argc;
    (void)argv;
    return JS_NewInt32(ctx, (int)__atomic_load_n(&ci_stalling, __ATOMIC_SEQ_CST));
}

static JSValue h_later(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)this_val;
    if (argc > 0 && ci_nlater < 8)
        ci_later[ci_nlater++] = JS_DupValue(ctx, argv[0]);
    return JS_UNDEFINED;
}

static JSValue h_alloc_failed(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)this_val;
    (void)argc;
    (void)argv;
    return JS_NewBool(ctx, ci_failed);
}

static JSValue h_live(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)this_val;
    (void)argc;
    (void)argv;
    int n;
    pthread_mutex_lock(&ci_mu);
    n = ci_nlive;
    pthread_mutex_unlock(&ci_mu);
    return JS_NewInt32(ctx, n);
}

static void ccheck(int ok, const char* what)
{
    checks++;
    if (!ok) {
        fails++;
        printf("  FAIL: %s\n", what);
    } else {
        printf("  ok %s\n", what);
    }
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
    ccheck(!strncmp(s, "ok ", 3), s);
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
    "import { PostgreSQL } from 'dyna:postgres';\n"
    "import * as std from 'std';\n"
    "import * as os from 'os';\n"
    "const tick = () => new Promise((r) => os.setTimeout(r, 5));\n"
    "for (const k of ['PGHOST', 'PGPORT', 'PGUSER', 'PGPASSWORD', 'PGDATABASE', 'PGPASSFILE', 'PGTARGETSESSIONATTRS', 'PGLOADBALANCEHOSTS']) std.unsetenv(k);\n"
    "function sweep(name, mk, want) {\n"
    "  let seen = 0, bad = '';\n"
    "  for (let at = 1; at < 300; at++) {\n"
    "    let r = null; failAlloc(at);\n"
    "    try { const c = mk(); c.close(); } catch (e) { r = e; }\n"
    "    const f = allocFailed(), lv = liveCount(); failAlloc(0); probe();\n"
    "    if (!f) { if (r) bad = 'threw ' + r + ' with no failed allocation'; break; }\n"
    "    seen++;\n"
    "    if (!(r instanceof Error) || !/out of memory/i.test(String(r.message))) { bad = 'allocation ' + at + ' failed but the constructor ended with ' + String(r); break; }\n"
    "    if (lv) { bad = 'allocation ' + at + ' failed and ' + lv + ' block(s) stayed allocated'; break; }\n"
    "  }\n"
    "  report(bad ? name + ': ' + bad : seen < want ? name + ': only ' + seen + ' allocations failed, want ' + want : 'ok ' + name + ': ' + seen + ' failed allocations each threw out of memory and freed everything');\n"
    "}\n"
    "std.setenv('PGPASSFILE', PF);\n"
    "{ const f0 = reactorFrees();\n"
    "  for (let i = 0; i < 1000; i++) new PostgreSQL('postgresql://alice@127.0.0.1:1/shop', { connectTimeoutMs: 200 }).close();\n"
    "  const d = reactorFrees() - f0;\n"
    "  for (let i = 0; i < 400 && reactorFrees() - f0 === d; i++) await tick();\n"
    "  const d2 = reactorFrees() - f0 - d;\n"
    "  report(d === 0 && d2 === 1 ? 'ok reactor: 1000 constructor+close with the .pgpass read in flight freed the reactor 0 times, then once when the reads drained' : 'reactor: 1000 constructor+close freed the reactor ' + d + ' times (want 0: the in-flight read holds it), then ' + d2 + ' (want 1)'); }\n"
    "std.unsetenv('PGPASSFILE');\n"
    "sweep('url', () => new PostgreSQL('postgresql://us%65r:p%40ss@127.0.0.1:1/d%62?application_name=a%70p&sslmode=disable&connect_timeout=3&target_session_attrs=prefer-standby&load_balance_hosts=random', { connectTimeoutMs: 200 }), 10);\n"
    "std.setenv('PGPASSFILE', PF);\n"
    "async function sweepPool(name, mk, want, wantOff) {\n"
    "  let seen = 0, off = 0, offN = 0, later = 0, bad = '';\n"
    "  for (let at = 1; at < 300 && !bad; at++) {\n"
    "    let r = null, c = null; failAlloc(at);\n"
    "    try { c = mk(); } catch (e) { r = e; }\n"
    "    if (c) { try { await c.query('select 1'); } catch (e) { r = e; } c.close(); }\n"
    "    let lv = liveCount();\n"
    "    for (let i = 0; i < 200 && lv; i++) { await tick(); lv = liveCount(); }\n"
    "    const f = allocFailed(), o = offStat(); failAlloc(0); probe();\n"
    "    if (!f) { offN = o[1]; if (!r || !/connect/.test(String(r.message))) bad = 'with no failed allocation the client ended with ' + String(r); else if (lv) bad = 'with no failed allocation ' + lv + ' block(s) stayed allocated'; break; }\n"
    "    seen++; if (o[0]) off++;\n"
    "    if (lv) bad = 'allocation ' + at + (o[0] ? ' (I/O pool)' : '') + ' failed and ' + lv + ' block(s) stayed allocated';\n"
    "    else if (c && !o[0]) { later++; if (!(r instanceof Error)) bad = 'allocation ' + at + ' failed after the constructor and the client ended with ' + String(r); }\n"
    "    else if (!(r instanceof Error) || !/out of memory/i.test(String(r.message))) bad = 'allocation ' + at + (o[0] ? ' (I/O pool)' : '') + ' failed but the client ended with ' + String(r);\n"
    "    else if (o[0] && !c) bad = 'allocation ' + at + ' failed on the I/O pool yet the constructor threw';\n"
    "  }\n"
    "  report(bad ? name + ': ' + bad : seen < want || off < wantOff ? name + ': ' + seen + ' failed allocations (' + off + ' on the I/O pool), want ' + want + ' (' + wantOff + ')' : 'ok ' + name + ': ' + seen + ' failed allocations (' + off + ' on the I/O pool, ' + later + ' after the constructor) freed everything; constructor and pool failures ended in out of memory');\n"
    "  if (wantOff) report(offN >= 2 ? 'ok ' + name + ': the read allocated ' + offN + ' times off the JS thread' : name + ': the .pgpass read allocated ' + offN + ' times off the JS thread, want >= 2 (it ran on the JS thread)');\n"
    "}\n"
    "await sweepPool('refused', () => new PostgreSQL('postgresql://alice:pw@127.0.0.1:1/shop', { connectTimeoutMs: 200, env: false }), 3, 0);\n"
    "await sweepPool('pgpass', () => new PostgreSQL('postgresql://alice@127.0.0.1:1/shop', { connectTimeoutMs: 200 }), 3, 2);\n"
    "await sweepPool('multi', () => new PostgreSQL('postgresql://alice@127.0.0.1:1,127.0.0.1:2/shop', { connectTimeoutMs: 200 }), 3, 2);\n"
    "std.unsetenv('PGPASSFILE'); std.setenv('PGUSER', 'envuser'); std.setenv('PGDATABASE', 'shop'); std.setenv('PGTARGETSESSIONATTRS', 'read-write'); std.setenv('PGLOADBALANCEHOSTS', 'random');\n"
    "sweep('env', () => new PostgreSQL({ host: '127.0.0.1', port: 1, connectTimeoutMs: 200 }), 4);\n"
    "std.unsetenv('PGUSER'); std.unsetenv('PGDATABASE'); std.unsetenv('PGTARGETSESSIONATTRS'); std.unsetenv('PGLOADBALANCEHOSTS');\n";

static JSContext* new_ctx(JSRuntime* rt);

static int ci_listen(int* port)
{
    struct sockaddr_in a;
    socklen_t l = sizeof a;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (s < 0 || bind(s, (struct sockaddr*)&a, sizeof a) || listen(s, 8) || getsockname(s, (struct sockaddr*)&a, &l) || fcntl(s, F_SETFL, O_NONBLOCK)) {
        if (s >= 0)
            close(s);
        return -1;
    }
    *port = ntohs(a.sin_port);
    return s;
}

static int ci_accepts(int s)
{
    int n = 0, c;
    while ((c = accept(s, NULL, NULL)) >= 0) {
        n++;
        close(c);
    }
    return n;
}

static void irq_case(const char* name, const char* body, const char* pf, const char* want_at)
{
    if (ci_filter && !strstr(name, ci_filter))
        return;
    JSRuntime* rt = JS_NewRuntime();
    JSContext* ctx;
    JSValue g, v, lk;
    int rc, i, live, leaked, atok, nodrain;
    JSValue at;
    const char* ats;
    char what[320], atbuf[64];
    ci_armed = ci_fired = ci_hits = 0;
    ci_nlater = 0;
    pthread_mutex_lock(&ci_mu);
    ci_nlive = 0;
    ci_fail_at = 0;
    pthread_mutex_unlock(&ci_mu);
    JS_SetInterruptHandler(rt, ci_irq, NULL);
    js_std_init_handlers(rt);
    ctx = new_ctx(rt);
    JS_SetModuleLoaderFunc2(rt, NULL, js_module_loader, js_module_check_attributes, NULL);
    js_std_add_helpers(ctx, 0, NULL);
    g = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, g, "arm", JS_NewCFunction(ctx, h_arm, "arm", 0));
    JS_SetPropertyStr(ctx, g, "hit", JS_NewCFunction(ctx, h_hit, "hit", 0));
    JS_SetPropertyStr(ctx, g, "stallOff", JS_NewCFunction(ctx, h_stall, "stallOff", 2));
    JS_SetPropertyStr(ctx, g, "later", JS_NewCFunction(ctx, h_later, "later", 1));
    JS_SetPropertyStr(ctx, g, "stalling", JS_NewCFunction(ctx, h_stalling, "stalling", 0));
    JS_SetPropertyStr(ctx, g, "PF", JS_NewString(ctx, pf));
    JS_SetPropertyStr(ctx, g, "CNTP", JS_NewInt32(ctx, ci_cntp));
    v = js_std_await(ctx, JS_Eval(ctx, body, strlen(body), "tests/postgres/<conninfo-irq>.js", JS_EVAL_TYPE_MODULE));
    rc = JS_IsException(v);
    if (rc)
        JS_FreeValue(ctx, JS_GetException(ctx));
    JS_FreeValue(ctx, v);
    if (!rc && js_std_loop(ctx)) {
        rc = 1;
        JS_FreeValue(ctx, JS_GetException(ctx));
    }
    lk = JS_GetPropertyStr(ctx, g, "leak");
    leaked = !JS_IsUndefined(lk);
    JS_FreeValue(ctx, lk);
    at = JS_GetPropertyStr(ctx, g, "at");
    ats = JS_IsUndefined(at) ? NULL : JS_ToCString(ctx, at);
    atok = ats && !strcmp(ats, want_at);
    snprintf(atbuf, sizeof atbuf, "%s", ats ? ats : "?");
    if (ats)
        JS_FreeCString(ctx, ats);
    else if (JS_HasException(ctx))
        JS_FreeValue(ctx, JS_GetException(ctx));
    JS_FreeValue(ctx, at);
    at = JS_GetPropertyStr(ctx, g, "__nodrain");
    nodrain = JS_ToBool(ctx, at) > 0;
    JS_FreeValue(ctx, at);
    for (i = 0; i < ci_nlater; i++) {
        JSValue cl = JS_GetPropertyStr(ctx, ci_later[i], "close");
        JSValue r = JS_Call(ctx, cl, ci_later[i], 0, NULL);
        if (JS_IsException(r))
            JS_FreeValue(ctx, JS_GetException(ctx));
        JS_FreeValue(ctx, r);
        JS_FreeValue(ctx, cl);
        JS_FreeValue(ctx, ci_later[i]);
    }
    if (!nodrain) {
        {
            static const char dr[] = "import * as os from 'os';\nos.setTimeout(() => { for (const k of globalThis.__keep || []) try { k.close(); } catch (e) {} }, 2600);\n";
            JSValue d = js_std_await(ctx, JS_Eval(ctx, dr, strlen(dr), "tests/postgres/<conninfo-irq-drain>.js", JS_EVAL_TYPE_MODULE));
            if (JS_IsException(d))
                JS_FreeValue(ctx, JS_GetException(ctx));
            JS_FreeValue(ctx, d);
        }
        if (js_std_loop(ctx))
            JS_FreeValue(ctx, JS_GetException(ctx));
    }
    ci_stall_ms = 0;
    JS_FreeValue(ctx, g);
    js_std_free_handlers(rt);
    JS_FreeContext(ctx);
    if (nodrain)
        JS_SetExitTeardown(rt, 1);
    JS_FreeRuntime(rt);
    if (ci_cnts >= 0)
        ci_hits += ci_accepts(ci_cnts);
    pthread_mutex_lock(&ci_mu);
    live = ci_nlive;
    pthread_mutex_unlock(&ci_mu);
    snprintf(what, sizeof what, "%s: interrupt fired=%d, run ended uncatchably=%d, leak flag=%d, attempt-3 hits=%d, blocks left=%d, at the interrupt %s (want %s)", name, ci_fired, rc, leaked, ci_hits, live, atbuf, want_at);
    ccheck(rc && ci_fired && !leaked && !ci_hits && !live && atok, what);
}

static int wfile(const char* name, const char* data, size_t n, mode_t mode, char* out, size_t cap)
{
    int fd;
    snprintf(out, cap, "%s/%s", dir, name);
    fd = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        return -1;
    if (n && write(fd, data, n) != (ssize_t)n) {
        close(fd);
        return -1;
    }
    fchmod(fd, mode);
    close(fd);
    return 0;
}

static int lookup(const char* f, char* got, size_t cap)
{
    char* o = NULL;
    int r = pg_pgpass_read(f, "127.0.0.1", "5432", "shop", "alice", &o);
    got[0] = 0;
    if (r > 0 && o)
        snprintf(got, cap, "%s", o);
    free(o);
    return r;
}

static double now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e9 + (double)t.tv_nsec;
}

static double cost(const char* f, int iters)
{
    double best = 1e18;
    int rep, i;
    char got[64];
    for (rep = 0; rep < 5; rep++) {
        double t0 = now_ns(), dt;
        for (i = 0; i < iters; i++)
            lookup(f, got, sizeof got);
        dt = (now_ns() - t0) / iters;
        if (dt < best)
            best = dt;
    }
    return best;
}

static void file_checks(void)
{
    static const char small[] = "# c\nother:*:*:*:no\n*:*:other:*:no\n127.0.0.1:5432:other:alice:no\n127.0.0.1:5432:shop:alice:s\\:e\\\\c\n";
    static const char match[] = "127.0.0.1:5432:shop:alice:edge\n";
    char p1[640], p2[640], p3[640], p4[640], p5[640], p6[640], p7[640], got[256];
    size_t big = 1u << 20, i, ml = sizeof match - 1, o;
    char* buf = (char*)malloc(big + 2);
    int r;
    if (!buf) {
        ccheck(0, "test buffer");
        return;
    }
    wfile("small", small, sizeof small - 1, 0600, p1, sizeof p1);
    r = lookup(p1, got, sizeof got);
    ccheck(r == 1 && !strcmp(got, "s:e\\c"), "a 0600 file answers the first matching line, unescaped");
    chmod(p1, 0644);
    ccheck(lookup(p1, got, sizeof got) == 0, "group/other read bits: the file is ignored");
    chmod(p1, 0600);
    ccheck(lookup(dir, got, sizeof got) == 0, "a directory is ignored");
    snprintf(p2, sizeof p2, "%s/fifo", dir);
    ccheck(mkfifo(p2, 0600) == 0 && lookup(p2, got, sizeof got) == 0, "a FIFO is ignored without blocking");
    wfile("nul", "127.0.0.1:5432:shop:alice:x\0\n", 29, 0600, p3, sizeof p3);
    ccheck(lookup(p3, got, sizeof got) == 0, "a NUL byte: the file is ignored");
    buf[0] = '#';
    memset(buf + 1, 'x', big - ml - 2);
    buf[big - ml - 1] = '\n';
    memcpy(buf + big - ml, match, ml);
    wfile("mib", buf, big, 0600, p4, sizeof p4);
    r = lookup(p4, got, sizeof got);
    ccheck(r == 1 && !strcmp(got, "edge"), "exactly 1 MiB is read");
    memset(buf + 1, 'x', big - ml - 1);
    buf[big - ml] = '\n';
    memcpy(buf + big - ml + 1, match, ml);
    wfile("mib1", buf, big + 1, 0600, p5, sizeof p5);
    ccheck(lookup(p5, got, sizeof got) == 0, "1 MiB + 1 is ignored");
    for (i = 0, o = 0; i < 10000; i++) {
        memcpy(buf + o, "127.0.0.1:5432:other:alice:no\n", 30);
        o += 30;
    }
    memcpy(buf + o, match, ml);
    o += ml;
    wfile("lines10k", buf, o, 0600, p6, sizeof p6);
    r = lookup(p6, got, sizeof got);
    ccheck(r == 1 && !strcmp(got, "edge"), "the match after 10000 lines");
    snprintf(p7, sizeof p7, "%s/missing", dir);
    printf("PGPASS-COST missing=%.0f small=%.0f lines10k=%.0f ns\n", cost(p7, 20000), cost(p1, 20000), cost(p6, 200));
    free(buf);
    unlink(p1);
    unlink(p2);
    unlink(p3);
    unlink(p4);
    unlink(p5);
    unlink(p6);
}

static void teardown_case(const char* name, const char* body)
{
    JSRuntime* rt;
    JSContext* ctx;
    JSValue g, v;
    int port = 0, s, live, bad;
    char what[256];
    if (ci_filter && !strstr(name, ci_filter))
        return;
    s = ci_listen(&port);
    rt = JS_NewRuntime();
    pthread_mutex_lock(&ci_mu);
    ci_nlive = 0;
    ci_fail_at = 0;
    pthread_mutex_unlock(&ci_mu);
    js_std_init_handlers(rt);
    ctx = new_ctx(rt);
    JS_SetModuleLoaderFunc2(rt, NULL, js_module_loader, js_module_check_attributes, NULL);
    js_std_add_helpers(ctx, 0, NULL);
    g = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, g, "CNTP", JS_NewInt32(ctx, port));
    v = js_std_await(ctx, JS_Eval(ctx, body, strlen(body), "tests/postgres/<conninfo-teardown>.js", JS_EVAL_TYPE_MODULE));
    bad = JS_IsException(v);
    if (bad)
        js_std_dump_error(ctx);
    JS_FreeValue(ctx, v);
    JS_FreeValue(ctx, g);
    js_std_free_handlers(rt);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    if (s >= 0)
        close(s);
    pthread_mutex_lock(&ci_mu);
    live = ci_nlive;
    pthread_mutex_unlock(&ci_mu);
    snprintf(what, sizeof what, "%s: module ran=%d, second runtime freed without close() and without aborting, blocks left=%d", name, !bad, live);
    ccheck(s >= 0 && !bad && !live, what);
}

static void irq_cases(const char* pf)
{
    ci_cnts = ci_listen(&ci_cntp);
    if (ci_cnts < 0) {
        printf("  FAIL: cannot open the attempt-3 listener\n");
        fails++;
    }
    irq_case("interrupt while attempt 2 of 3 hangs, then teardown without close",
        "import { PostgreSQL } from 'dyna:postgres';\n"
        "import { TCPServer } from 'dyna:net';\n"
        "import * as os from 'os';\n"
        "const hang = new TCPServer({ port: 0 }); hang.start({ connect: () => { globalThis.hc = (globalThis.hc | 0) + 1; }, data: () => {} }); later(hang);\n"
        "globalThis.__nodrain = 1;\n"
        "const c = new PostgreSQL('postgresql://u:pw@127.0.0.1:1,127.0.0.1:' + hang.port + ',127.0.0.1:' + CNTP + '/d?connect_timeout=2', { env: false });\n"
        "c.query('select 1').then(() => { globalThis.leak = 'resolved'; }, () => { globalThis.leak = 'rejected catchably'; });\n"
        "os.setTimeout(() => Promise.resolve().then(() => { globalThis.at = 'hang=' + (globalThis.hc | 0); arm(); for (;;) {} }), 300);\n", pf, "hang=1");
    irq_case("interrupt while attempt 2 of 3 hangs",
        "import { PostgreSQL } from 'dyna:postgres';\n"
        "import { TCPServer } from 'dyna:net';\n"
        "import * as os from 'os';\n"
        "const hang = new TCPServer({ port: 0 }); hang.start({ connect: () => { globalThis.hc = (globalThis.hc | 0) + 1; }, data: () => {} }); later(hang);\n"
        "const cnt = new TCPServer({ port: 0 }); cnt.start({ connect: () => hit(), data: () => hit() }); globalThis.__keep = [cnt];\n"
        "const c = new PostgreSQL('postgresql://u:pw@127.0.0.1:1,127.0.0.1:' + hang.port + ',127.0.0.1:' + cnt.port + '/d?connect_timeout=2', { env: false }); later(c);\n"
        "c.query('select 1').then(() => { globalThis.leak = 'resolved'; }, () => { globalThis.leak = 'rejected catchably'; });\n"
        "os.setTimeout(() => Promise.resolve().then(() => { globalThis.at = 'hang=' + (globalThis.hc | 0); arm(); for (;;) {} }), 300);\n", pf, "hang=1");
    irq_case("interrupt while attempt 2's .pgpass read is on the I/O pool",
        "import { PostgreSQL } from 'dyna:postgres';\n"
        "import { TCPServer } from 'dyna:net';\n"
        "import * as os from 'os';\n"
        "import * as std from 'std';\n"
        "std.setenv('PGPASSFILE', PF);\n"
        "const hang = new TCPServer({ port: 0 }); hang.start({ connect: () => { globalThis.hc = (globalThis.hc | 0) + 1; }, data: () => {} }); later(hang);\n"
        "const cnt = new TCPServer({ port: 0 }); cnt.start({ connect: () => hit(), data: () => hit() }); globalThis.__keep = [cnt];\n"
        "stallOff(1000, 900);\n"
        "const c = new PostgreSQL('postgresql://alice@127.0.0.1:' + hang.port + ',127.0.0.1:1,127.0.0.1:' + cnt.port + '/shop?connect_timeout=2'); later(c);\n"
        "std.unsetenv('PGPASSFILE');\n"
        "c.query('select 1').then(() => { globalThis.leak = 'resolved'; }, () => { globalThis.leak = 'rejected catchably'; });\n"
        "os.setTimeout(() => Promise.resolve().then(() => { globalThis.at = 'hang=' + (globalThis.hc | 0) + ',stalling=' + stalling(); arm(); for (;;) {} }), 2300);\n", pf, "hang=1,stalling=1");
    if (ci_cnts >= 0)
        close(ci_cnts);
    ci_cnts = -1;
    irq_case("interrupt while host 1's target_session_attrs check waits, host 2 untouched",
        "import { PostgreSQL } from 'dyna:postgres';\n"
        "import { TCPServer } from 'dyna:net';\n"
        "import * as os from 'os';\n"
        "const seen = new Set();\n"
        "const auth = new TCPServer({ port: 0 }); auth.start({ data: (c, b) => { if (!seen.has(c)) { seen.add(c); c.write(new Uint8Array([82, 0, 0, 0, 8, 0, 0, 0, 0, 90, 0, 0, 0, 5, 73])); } else if (b[0] === 81) globalThis.hc = (globalThis.hc | 0) + 1; } }); later(auth);\n"
        "const cnt = new TCPServer({ port: 0 }); cnt.start({ connect: () => hit(), data: () => hit() }); globalThis.__keep = [cnt];\n"
        "const c = new PostgreSQL('postgresql://u@127.0.0.1:' + auth.port + ',127.0.0.1:' + cnt.port + '/d?target_session_attrs=read-write&connect_timeout=2', { env: false }); later(c);\n"
        "c.query('select 1').then(() => { globalThis.leak = 'resolved'; }, () => { globalThis.leak = 'rejected catchably'; });\n"
        "os.setTimeout(() => Promise.resolve().then(() => { globalThis.at = 'show=' + (globalThis.hc | 0); arm(); for (;;) {} }), 300);\n", pf, "show=1");
    teardown_case("teardown: postgres connecting with a query queued",
        "import { PostgreSQL } from 'dyna:postgres';\n"
        "import * as os from 'os';\n"
        "const c = new PostgreSQL('postgresql://u:pw@127.0.0.1:' + CNTP + '/d?connect_timeout=2', { env: false });\n"
        "c.query('select 1').then(() => {}, () => {});\n"
        "await new Promise((r) => os.setTimeout(r, 200));\n");
    teardown_case("teardown: redis connecting with a command queued",
        "import { Redis } from 'dyna:redis';\n"
        "import * as os from 'os';\n"
        "const r = new Redis({ host: '127.0.0.1', port: CNTP });\n"
        "r.command('PING').then(() => {}, () => {});\n"
        "await new Promise((r) => os.setTimeout(r, 200));\n");
}

int main(int argc, char** argv)
{
    JSRuntime* rt = JS_NewRuntime();
    JSContext* ctx;
    JSValue g, v;
    const char* td = getenv("TMPDIR");
    char pf[640];
    alarm(120);
    ci_main = pthread_self();
    snprintf(dir, sizeof dir, "%s/dj-conninfo-api-%d", td && td[0] ? td : "/tmp", (int)getpid());
    mkdir(dir, 0700);
    setenv("HOME", dir, 1);
    wfile("sweep", "127.0.0.1:1:shop:alice:pw\n", 26, 0600, pf, sizeof pf);
    if (argc > 1) {
        ci_filter = argv[1];
        irq_cases(pf);
        printf("postgres_conninfo_api: %d checks, %d failed (filter %s)\n", checks, fails, ci_filter);
        return fails || !checks ? 1 : 0;
    }
    js_std_set_worker_new_context_func(new_ctx);
    if (dyn_net_on_reactor_free(h_reactor_freed) != 0)
        fails++;
    js_std_init_handlers(rt);
    ctx = new_ctx(rt);
    JS_SetModuleLoaderFunc2(rt, NULL, js_module_loader, js_module_check_attributes, NULL);
    js_std_add_helpers(ctx, 0, NULL);
    g = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, g, "probe", JS_NewCFunction(ctx, h_probe, "probe", 0));
    JS_SetPropertyStr(ctx, g, "failAlloc", JS_NewCFunction(ctx, h_fail_alloc, "failAlloc", 1));
    JS_SetPropertyStr(ctx, g, "allocFailed", JS_NewCFunction(ctx, h_alloc_failed, "allocFailed", 0));
    JS_SetPropertyStr(ctx, g, "reactorFrees", JS_NewCFunction(ctx, h_rfrees, "reactorFrees", 0));
    JS_SetPropertyStr(ctx, g, "offStat", JS_NewCFunction(ctx, h_off, "offStat", 0));
    JS_SetPropertyStr(ctx, g, "liveCount", JS_NewCFunction(ctx, h_live, "liveCount", 0));
    JS_SetPropertyStr(ctx, g, "report", JS_NewCFunction(ctx, h_report, "report", 1));
    JS_SetPropertyStr(ctx, g, "PF", JS_NewString(ctx, pf));
    JS_FreeValue(ctx, g);
    v = js_std_await(ctx, JS_Eval(ctx, src, sizeof src - 1, "tests/postgres/<conninfo-api>.js", JS_EVAL_TYPE_MODULE));
    if (JS_IsException(v)) {
        js_std_dump_error(ctx);
        fails++;
    }
    JS_FreeValue(ctx, v);
    if (js_std_loop(ctx)) {
        js_std_dump_error(ctx);
        fails++;
    }
    file_checks();
    irq_cases(pf);
    if (checks < 22) {
        printf("  FAIL: only %d of 22 checks reported\n", checks);
        fails++;
    }
    if (stale) {
        printf("  FAIL: %d runs left an exception pending\n", stale);
        fails++;
    }
    js_std_free_handlers(rt);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    unlink(pf);
    rmdir(dir);
    printf("postgres_conninfo_api: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
