// covers: c:src/modules/postgres/postgres-protocol.c -- rule 18 for the SCRAM PBKDF2 job: offload, inline fallback, close mid-job, runtime exit mid-job, forced allocation failure; forced allocation failure in array spans, the array unescape buffer and the json copy (pg_array_text, pg_json_value), range and multirange unescape buffers (pg_range_any)
// Hooks malloc/calloc/realloc/strdup/free and dyn_aio_offload_cpu inside postgres-protocol.c against an in-process fake server: every job is freed on every
// path (late done, reap, reactor free at runtime exit) and its keys and password copy are zero when freed. Usage: test_postgres_scram_api; run from the repo root.
#include "dynajs.h"
#include "dyna-aio.h"
#include "dyna-libc.h"
#include "dyna-nat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#undef malloc
#undef calloc
#undef realloc
#undef strdup
#undef free
#undef dyn_aio_offload_cpu
void* malloc(size_t n);
void* calloc(size_t k, size_t n);
void* realloc(void* p, size_t n);
char* strdup(const char* s);
void free(void* p);
int dyn_aio_offload_cpu(dyn_aio_t* aio, void (*work)(void*), void (*done)(void*), void* arg);
void pg_scram_jobs_reap(void);

#define PW "pencil-scram-api-0123456789"
#define NTAB 8192

typedef struct {
    void* p;
    size_t n;
    int kind;
} ent_t;

static ent_t tab[NTAB];
static int ntab, stale, fails, checks;
static unsigned long alloc_n, fail_at;
static int failed, mode, held, inline_hits, wipe_bad, job_freed;
static void (*h_work)(void*);
static void (*h_done)(void*);
static void* h_arg;
static void* job_p;
static size_t job_n;

static int find(const void* p)
{
    int i;
    for (i = 0; p && i < ntab; i++)
        if (tab[i].p == p)
            return i;
    return -1;
}

static void track(void* p, size_t n, int kind)
{
    int i = find(p);
    if (!p)
        return;
    if (i < 0 && ntab < NTAB)
        i = ntab++;
    if (i >= 0) {
        tab[i].p = p;
        tab[i].n = n;
        tab[i].kind = kind;
    }
}

static int zero(const void* p, size_t n)
{
    const unsigned char* c = (const unsigned char*)p;
    while (n--)
        if (*c++)
            return 0;
    return 1;
}

static int fail_now(void)
{
    if (fail_at && ++alloc_n == fail_at) {
        failed = 1;
        return 1;
    }
    return 0;
}

void* ps_t_malloc(size_t n)
{
    void* p;
    if (fail_now())
        return NULL;
    p = malloc(n);
    track(p, n, 1);
    return p;
}

void* ps_t_calloc(size_t k, size_t n)
{
    void* p;
    if (fail_now())
        return NULL;
    p = calloc(k, n);
    track(p, k * n, 2);
    return p;
}

void* ps_t_realloc(void* p, size_t n)
{
    void* q;
    int i;
    if (fail_now())
        return NULL;
    i = find(p);
    q = realloc(p, n);
    if (q && i >= 0)
        tab[i] = tab[--ntab];
    if (q)
        track(q, n, 1);
    return q;
}

char* ps_t_strdup(const char* s)
{
    char* p;
    if (fail_now())
        return NULL;
    p = strdup(s);
    if (p)
        track(p, strlen(p) + 1, 3);
    return p;
}

void ps_t_free(void* p)
{
    int i = find(p);
    if (p && p == job_p) {
        job_freed = 1;
        if (!zero(p, job_n))
            wipe_bad++;
        job_p = NULL;
    }
    if (i >= 0) {
        if (tab[i].kind == 3 && tab[i].n == sizeof PW && !zero(p, sizeof PW))
            wipe_bad++;
        tab[i] = tab[--ntab];
    }
    free(p);
}

int ps_t_offload(dyn_aio_t* a, void (*w)(void*), void (*d)(void*), void* arg)
{
    int i = find(arg);
    job_p = arg;
    job_n = i >= 0 ? tab[i].n : 0;
    job_freed = 0;
    if (mode == 1) {
        inline_hits++;
        return -1;
    }
    if (mode == 2) {
        h_work = w;
        h_done = d;
        h_arg = arg;
        held = 1;
        return 0;
    }
    return dyn_aio_offload_cpu(a, w, d, arg);
}

static JSValue h_set_mode(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    int32_t m = 0;
    (void)this_val;
    if (argc < 1 || JS_ToInt32(ctx, &m, argv[0]))
        return JS_EXCEPTION;
    mode = m;
    held = 0;
    return JS_UNDEFINED;
}

static JSValue h_int(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    (void)this_val;
    (void)argc;
    (void)argv;
    switch (magic) {
    case 0:
        return JS_NewBool(ctx, held);
    case 1:
        if (held && h_work && h_done) {
            held = 0;
            h_work(h_arg);
            h_done(h_arg);
        }
        return JS_UNDEFINED;
    case 2:
        held = 0;
        pg_scram_jobs_reap();
        return JS_UNDEFINED;
    case 3:
        return JS_NewBool(ctx, job_freed);
    case 4:
        return JS_NewInt32(ctx, wipe_bad);
    case 5:
        return JS_NewInt32(ctx, inline_hits);
    case 6:
        return JS_NewBool(ctx, failed);
    case 7: {
        int i, n = 0;
        for (i = 0; i < ntab; i++)
            if ((tab[i].kind == 3 && tab[i].n == sizeof PW) || (job_n && tab[i].kind == 2 && tab[i].n == job_n))
                n++;
        return JS_NewInt32(ctx, n);
    }
    default:
        if (JS_HasException(ctx)) {
            stale++;
            JS_FreeValue(ctx, JS_GetException(ctx));
        }
        return JS_UNDEFINED;
    }
}

static JSValue h_fail_alloc(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    int64_t at = 0;
    (void)this_val;
    if (argc < 1 || JS_ToInt64(ctx, &at, argv[0]))
        return JS_EXCEPTION;
    alloc_n = 0;
    failed = 0;
    fail_at = at > 0 ? (unsigned long)at : 0;
    return JS_UNDEFINED;
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
    "import * as std from 'std';\n"
    "import { TCPServer } from 'dyna:net';\n"
    "import { PostgreSQL } from 'dyna:postgres';\n"
    "const str = (u) => { let s = ''; for (let i = 0; i < u.length; i++) s += String.fromCharCode(u[i]); return s; };\n"
    "const bin = (s) => { const u = new Uint8Array(s.length); for (let i = 0; i < s.length; i++) u[i] = s.charCodeAt(i) & 255; return u; };\n"
    "const i32 = (v) => String.fromCharCode((v >>> 24) & 255, (v >>> 16) & 255, (v >>> 8) & 255, v & 255);\n"
    "const msg = (t, b) => t + i32(b.length + 4) + b;\n"
    "const len4 = (s, o) => (s.charCodeAt(o) << 24 | s.charCodeAt(o + 1) << 16 | s.charCodeAt(o + 2) << 8 | s.charCodeAt(o + 3)) >>> 0;\n"
    "const Z = 'A'.repeat(43) + '=', finals = [], bufs = new Map(), st = new Map();\n"
    "const i16 = (v) => String.fromCharCode((v >>> 8) & 255, v & 255);\n"
    "const COLS = [[1007, '{' + Array.from({ length: 100 }, (_, i) => i).join(',') + '}'], [1009, '{' + Array.from({ length: 10 }, () => '\"' + 'x'.repeat(120) + '\"').join(',') + '}'], [114, '{\"k\":\"' + 'y'.repeat(600) + '\"}']];\n"
    "const RCOLS = [[4451, '{' + Array.from({ length: 80 }, (_, i) => '[' + i * 10 + ',' + (i * 10 + 5) + ')').join(',') + '}'], [3906, '[1.' + '0'.repeat(600) + ',2)']];\n"
    "const RROWS = msg('T', i16(2) + RCOLS.map(([o], i) => 'c' + i + '\\0' + i32(0) + i16(0) + i32(o) + i16(-1) + i32(-1) + i16(0)).join('')) + msg('D', i16(2) + RCOLS.map(([, v]) => i32(v.length) + v).join(''));\n"
    "const ROWS = msg('T', i16(3) + COLS.map(([o], i) => 'c' + i + '\\0' + i32(0) + i16(0) + i32(o) + i16(-1) + i32(-1) + i16(0)).join('')) + msg('D', i16(3) + COLS.map(([, v]) => i32(v.length) + v).join(''));\n"
    "const srv = new TCPServer({ port: 0 });\n"
    "srv.start({ data: (c, b) => {\n"
    "  bufs.set(c, (bufs.get(c) || '') + str(b));\n"
    "  for (;;) { const s = bufs.get(c);\n"
    "    if (!st.get(c)) { if (s.length < 8 || s.length < len4(s, 0)) return; bufs.set(c, s.slice(len4(s, 0))); st.set(c, 1); c.write(bin(msg('R', i32(10) + 'SCRAM-SHA-256\\0\\0'))); continue; }\n"
    "    if (s.length < 5 || s.length < len4(s, 1) + 1) return;\n"
    "    const n = len4(s, 1), t = s[0], body = s.slice(5, n + 1); bufs.set(c, s.slice(n + 1)); let out = '';\n"
    "    if (t === 'p' && body.indexOf('SCRAM-SHA-256\\0') === 0) out = msg('R', i32(11) + 'r=' + body.slice(body.indexOf(',r=') + 3) + 'srv,s=QSXCR+Q6sek8bf92,i=4096');\n"
    "    else if (t === 'p') { finals.push(body); out = msg('R', i32(12) + 'v=' + Z); }\n"
    "    else if (t === 'Q') out = msg('C', 'SELECT 0\\0') + msg('Z', 'I');\n"
    "    if (out) try { c.write(bin(out)); } catch (e) {} } },\n"
    "  close: (c) => { bufs.delete(c); st.delete(c); } });\n"
    "const conn = () => new PostgreSQL({ host: '127.0.0.1', port: srv.port, user: 'u', password: PW, connectTimeoutMs: 3000 });\n"
    "const settle = async (p) => { try { return { ok: await p }; } catch (e) { return { err: String(e && e.message || e) }; } };\n"
    "const sleep = (ms) => new Promise((r) => setTimeout(r, ms));\n"
    "const ok = (c, m) => report((c ? 'ok ' : '') + m);\n"
    "const pass = async (label) => { finals.length = 0; const c = conn(); const r = await settle(c.query('SELECT 1')); c.close();\n"
    "  ok(r.err && /failed to prove it knows the password/.test(r.err), label + ': a bad v= ends in the server-proof failure (state restored): ' + r.err);\n"
    "  ok(finals.length === 1 && /^c=biws,r=[^,]+srv,p=/.test(finals[0]), label + ': client-final sent: ' + JSON.stringify(finals[0] || '').slice(0, 30));\n"
    "  ok(jobFreed() && !wipeBad(), label + ': job freed and wiped'); };\n"
    "const hold = async (label) => { setMode(2); const c = conn(); const q = settle(c.query('SELECT 1'));\n"
    "  for (let k = 0; k < 600 && !held(); k++) await sleep(5);\n"
    "  ok(held(), label + ': job captured'); std.gc(); c.close(); std.gc(); const r = await q;\n"
    "  ok(!!r.err && !jobFreed(), label + ': close mid-job rejects and leaves the job to its owner: ' + r.err); };\n"
    "setMode(0); await pass('offload');\n"
    "setMode(1); const ih = inlineHits(); await pass('inline fallback'); ok(inlineHits() > ih, 'inline fallback taken');\n"
    "await hold('late done'); finish(); ok(jobFreed() && !wipeBad(), 'late done on an orphan frees and wipes the job');\n"
    "await hold('reap'); reap(); ok(jobFreed() && !wipeBad(), 'reap frees and wipes an orphan whose done never runs');\n"
    "setMode(0); let seen = 0, oom = 0, bad = '';\n"
    "for (let at = 1; at < 400; at++) { const c = conn(); failAlloc(at); const r = await settle((async () => c.query('SELECT 1'))()); const f = allocFailed(); failAlloc(0); probe(); c.close();\n"
    "  if (!f) break; seen++; if (r.ok !== undefined) { bad = 'allocation ' + at + ' failed but the query resolved'; break; } if (/out of memory during SCRAM/.test(r.err)) oom++; }\n"
    "ok(!bad && seen > 0 && oom > 0, 'alloc sweep: ' + (bad || seen + ' failed allocations, ' + oom + ' ended in out of memory during SCRAM'));\n"
    "ok(liveSecrets() === 0 && !wipeBad(), 'no job or password copy left after the sweep: ' + liveSecrets());\n"
    "const srv2 = new TCPServer({ port: 0 }), b2 = new Map(), s2 = new Map();\n"
    "srv2.start({ data: (c, b) => {\n"
    "  b2.set(c, (b2.get(c) || '') + str(b));\n"
    "  for (;;) { const s = b2.get(c);\n"
    "    if (!s2.get(c)) { if (s.length < 8 || s.length < len4(s, 0)) return; b2.set(c, s.slice(len4(s, 0))); s2.set(c, 1); c.write(bin(msg('R', i32(0)) + msg('Z', 'I'))); continue; }\n"
    "    if (s.length < 5 || s.length < len4(s, 1) + 1) return;\n"
    "    const n = len4(s, 1), t = s[0], body = s.slice(5, n + 1); b2.set(c, s.slice(n + 1)); let out = '';\n"
    "    if (t === 'Q' && /ARR/.test(body)) out = ROWS + msg('C', 'SELECT 1\\0') + msg('Z', 'I');\n"
    "    else if (t === 'Q' && /RNG/.test(body)) out = RROWS + msg('C', 'SELECT 1\\0') + msg('Z', 'I');\n"
    "    else if (t === 'Q') out = msg('C', 'SELECT 0\\0') + msg('Z', 'I');\n"
    "    if (out) try { c.write(bin(out)); } catch (e) {} } },\n"
    "  close: (c) => { b2.delete(c); s2.delete(c); } });\n"
    "{ const dc = () => new PostgreSQL({ host: '127.0.0.1', port: srv2.port, user: 'u', connectTimeoutMs: 3000, arrays: 'array', json: 'parse' });\n"
    "  let aseen = 0, aoom = 0, abad = '';\n"
    "  for (let at = 1; at < 200; at++) { const c = dc(); await settle(c.query('SELECT 1')); failAlloc(at); const r = await settle((async () => c.query('ARR'))()); const f = allocFailed(); failAlloc(0); probe(); c.close();\n"
    "    if (!f) { if (r.err) abad = 'no allocation failed but the query rejected: ' + r.err; break; } aseen++; if (r.ok !== undefined) { abad = 'allocation ' + at + ' failed but the array/json query resolved'; break; } if (/out of memory/i.test(r.err)) aoom++; }\n"
    "  ok(!abad && aseen >= 3 && aoom >= 3, 'array spans, unescape buffer and json copy: ' + (abad || aseen + ' failed allocations, ' + aoom + ' ended in out of memory'));\n"
    "  const c = dc(); await settle(c.query('SELECT 1')); const r = await settle(c.query('ARR')); c.close(); const row = r.ok && r.ok.rows[0];\n"
    "  ok(row && row.c0.length === 100 && row.c0[99] === 99 && row.c1.length === 10 && row.c1[9].length === 120 && row.c2.k.length === 600, 'after the sweep the array/json query decodes: ' + (r.err || '')); }\n"
    "{ const rc = () => new PostgreSQL({ host: '127.0.0.1', port: srv2.port, user: 'u', connectTimeoutMs: 3000, ranges: 'object' });\n"
    "  let rseen = 0, room = 0, rbad = '';\n"
    "  for (let at = 1; at < 200; at++) { const c = rc(); await settle(c.query('SELECT 1')); failAlloc(at); const r = await settle((async () => c.query('RNG'))()); const f = allocFailed(); failAlloc(0); probe(); c.close();\n"
    "    if (!f) { if (r.err) rbad = 'no allocation failed but the query rejected: ' + r.err; break; } rseen++; if (r.ok !== undefined) { rbad = 'allocation ' + at + ' failed but the range query resolved'; break; } if (/out of memory/i.test(r.err)) room++; }\n"
    "  ok(!rbad && rseen >= 2 && room >= 2, 'range and multirange unescape buffers: ' + (rbad || rseen + ' failed allocations, ' + room + ' ended in out of memory'));\n"
    "  const c = rc(); await settle(c.query('SELECT 1')); const r = await settle(c.query('RNG')); c.close(); const row = r.ok && r.ok.rows[0];\n"
    "  ok(row && row.c0.length === 80 && row.c0[79].upper === 795 && row.c1.upper === '2', 'after the sweep the range query decodes: ' + (r.err || '')); srv2.close(); }\n"
    "await hold('runtime exit');\n"
    "srv.close();\n";

static void fn(JSContext* ctx, JSValue g, const char* name, int magic)
{
    JS_SetPropertyStr(ctx, g, name, JS_NewCFunctionMagic(ctx, h_int, name, 0, JS_CFUNC_generic_magic, magic));
}

int main(void)
{
    JSRuntime* rt = JS_NewRuntime();
    JSContext* ctx;
    JSValue g, v;
    alarm(400);
    js_std_set_worker_new_context_func(new_ctx);
    js_std_init_handlers(rt);
    ctx = new_ctx(rt);
    JS_SetModuleLoaderFunc2(rt, NULL, js_module_loader, js_module_check_attributes, NULL);
    js_std_add_helpers(ctx, 0, NULL);
    g = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, g, "PW", JS_NewString(ctx, PW));
    JS_SetPropertyStr(ctx, g, "setMode", JS_NewCFunction(ctx, h_set_mode, "setMode", 1));
    JS_SetPropertyStr(ctx, g, "failAlloc", JS_NewCFunction(ctx, h_fail_alloc, "failAlloc", 1));
    JS_SetPropertyStr(ctx, g, "report", JS_NewCFunction(ctx, h_report, "report", 1));
    fn(ctx, g, "held", 0);
    fn(ctx, g, "finish", 1);
    fn(ctx, g, "reap", 2);
    fn(ctx, g, "jobFreed", 3);
    fn(ctx, g, "wipeBad", 4);
    fn(ctx, g, "inlineHits", 5);
    fn(ctx, g, "allocFailed", 6);
    fn(ctx, g, "liveSecrets", 7);
    fn(ctx, g, "probe", 8);
    JS_FreeValue(ctx, g);
    v = js_std_await(ctx, JS_Eval(ctx, src, sizeof src - 1, "tests/postgres/<scram-api>.js", JS_EVAL_TYPE_MODULE));
    if (JS_IsException(v)) {
        js_std_dump_error(ctx);
        fails++;
    }
    JS_FreeValue(ctx, v);
    if (js_std_loop(ctx)) {
        js_std_dump_error(ctx);
        fails++;
    }
    if (checks < 16) {
        printf("  FAIL: only %d of 16 checks reported\n", checks);
        fails++;
    }
    if (stale) {
        printf("  FAIL: %d runs left an exception pending\n", stale);
        fails++;
    }
    js_std_free_handlers(rt);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    checks++;
    if (!job_freed || wipe_bad) {
        printf("  FAIL: runtime exit left the orphaned job %s\n", job_freed ? "unwiped" : "allocated");
        fails++;
    } else {
        printf("  ok runtime exit: the reactor-free hook freed and wiped the orphaned job\n");
    }
    printf("postgres_scram_api: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
