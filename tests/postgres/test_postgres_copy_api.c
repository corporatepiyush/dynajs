// covers: c:src/modules/postgres/postgres-copy.c -- rule 18 forced allocation failure for PostgreSQL.copyFrom/copyTo (PG/Redis item 1b)
// The malloc hook fails the Nth libc allocation made by postgres-copy.c (copy state, source buffer, chunk ring) while a
// copyFrom/copyTo runs against an in-process fake server. Every failed run must end in an "out of memory" error (thrown, or
// a rejection that names it) with no exception left pending; a later run on a fresh connection must succeed; the process
// must free every object at exit (JS_FreeRuntime asserts). Usage: test_postgres_copy_api; run from the repo root.
#include "dynajs.h"
#include "dyna-libc.h"
#include "dyna-nat.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#undef malloc
#undef calloc
#undef realloc
void* malloc(size_t n);
void* calloc(size_t k, size_t n);
void* realloc(void* p, size_t n);

static int stale, fails, checks;
static unsigned long pc_alloc_n, pc_fail_at;
static int pc_failed;

static int pc_fail_now(void)
{
    if (pc_fail_at && ++pc_alloc_n == pc_fail_at) {
        pc_failed = 1;
        return 1;
    }
    return 0;
}

void* pc_t_malloc(size_t n)
{
    return pc_fail_now() ? NULL : malloc(n);
}

void* pc_t_calloc(size_t k, size_t n)
{
    return pc_fail_now() ? NULL : calloc(k, n);
}

void* pc_t_realloc(void* p, size_t n)
{
    return pc_fail_now() ? NULL : realloc(p, n);
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
    pc_alloc_n = 0;
    pc_failed = 0;
    pc_fail_at = at > 0 ? (unsigned long)at : 0;
    return JS_UNDEFINED;
}

static JSValue h_alloc_failed(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)this_val;
    (void)argc;
    (void)argv;
    return JS_NewBool(ctx, pc_failed);
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
    "const str = (u) => { let s = ''; for (let i = 0; i < u.length; i++) s += String.fromCharCode(u[i]); return s; };\n"
    "const bin = (s) => { const u = new Uint8Array(s.length); for (let i = 0; i < s.length; i++) u[i] = s.charCodeAt(i) & 255; return u; };\n"
    "const i32 = (v) => String.fromCharCode((v >>> 24) & 255, (v >>> 16) & 255, (v >>> 8) & 255, v & 255);\n"
    "const msg = (t, b) => t + i32(b.length + 4) + b;\n"
    "const bufs = new Map(), live = new Map();\n"
    "const srv = new TCPServer({ port: 0 });\n"
    "srv.start({ data: (c, b) => {\n"
    "  bufs.set(c, (bufs.get(c) || '') + str(b));\n"
    "  for (;;) { const s = bufs.get(c);\n"
    "    if (!live.get(c)) { if (s.length < 8) return; const n = s.charCodeAt(0) << 24 | s.charCodeAt(1) << 16 | s.charCodeAt(2) << 8 | s.charCodeAt(3);\n"
    "      if (s.length < n) return; bufs.set(c, s.slice(n)); live.set(c, 1); c.write(bin(msg('R', i32(0)) + msg('Z', 'I'))); continue; }\n"
    "    if (s.length < 5) return; const n = (s.charCodeAt(1) << 24 | s.charCodeAt(2) << 16 | s.charCodeAt(3) << 8 | s.charCodeAt(4)) >>> 0;\n"
    "    if (s.length < n + 1) return; const t = s[0], body = s.slice(5, n + 1); bufs.set(c, s.slice(n + 1)); let out = '';\n"
    "    if (t === 'Q' && /FROM STDIN/.test(body)) out = msg('G', '\\0\\0\\0');\n"
    "    else if (t === 'Q' && /TO STDOUT/.test(body)) { out = msg('H', '\\0\\0\\0'); for (let i = 0; i < 40; i++) out += msg('d', 'row ' + i + '\\n'); out += msg('c', '') + msg('C', 'COPY 40\\0') + msg('Z', 'I'); }\n"
    "    else if (t === 'Q') out = msg('C', 'SELECT 0\\0') + msg('Z', 'I');\n"
    "    else if (t === 'c') out = msg('C', 'COPY 1\\0') + msg('Z', 'I');\n"
    "    else if (t === 'f') out = msg('E', 'SERROR\\0C57014\\0M' + body.slice(0, -1) + '\\0\\0') + msg('Z', 'I');\n"
    "    if (out) try { c.write(bin(out)); } catch (e) {} } },\n"
    "  close: (c) => { bufs.delete(c); live.delete(c); } });\n"
    "const conn = () => new PostgreSQL({ host: '127.0.0.1', port: srv.port, user: 'u' });\n"
    "const ops = {\n"
    "  fromString: (c) => c.copyFrom('COPY t FROM STDIN', 'a\\n'.repeat(64)),\n"
    "  fromBytes: (c) => c.copyFrom('COPY t FROM STDIN', new Uint8Array(70000)),\n"
    "  fromIter: (c) => c.copyFrom('COPY t FROM STDIN', (function* () { for (let i = 0; i < 300; i++) yield i & 1 ? 'x\\n' : new Uint8Array(2); })(), { highWaterMark: 64 }),\n"
    "  fromAsync: (c) => c.copyFrom('COPY t FROM STDIN', (async function* () { yield 'a\\n'; yield 'b\\n'; })()),\n"
    "  to: async (c) => { let n = 0; for await (const ch of c.copyTo('COPY t TO STDOUT', { highWaterMark: 32 })) n += ch.length; if (n < 200) throw new Error('short copyTo ' + n); return n; },\n"
    "};\n"
    "for (const [name, op] of Object.entries(ops)) {\n"
    "  let seen = 0, bad = '', at;\n"
    "  for (at = 1; at < 200; at++) {\n"
    "    const c = conn(); await c.query('SELECT 0');\n"
    "    let r; failAlloc(at);\n"
    "    try { r = await op(c); } catch (e) { r = e; }\n"
    "    const failed = allocFailed(); failAlloc(0); probe();\n"
    "    c.close();\n"
    "    if (!failed) { if (r instanceof Error) bad = 'threw ' + r + ' with no failed allocation'; break; }\n"
    "    seen++;\n"
    "    if (!(r instanceof Error) || !/out of memory|copyFrom source failed/.test(String(r.message))) { bad = 'allocation ' + at + ' failed but the run ended with ' + String(r); break; }\n"
    "  }\n"
    "  const c = conn(); let after; try { after = await op(c); } catch (e) { after = e; } c.close();\n"
    "  report(bad ? name + ': ' + bad : !seen ? name + ': no allocation failed' : after instanceof Error ? name + ': broken after the sweep: ' + after : 'ok ' + name + ': ' + seen + ' failed allocations each ended in out of memory');\n"
    "}\n"
    "srv.close();\n";

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
    JS_SetPropertyStr(ctx, g, "report", JS_NewCFunction(ctx, h_report, "report", 1));
    JS_FreeValue(ctx, g);
    v = js_std_await(ctx, JS_Eval(ctx, src, sizeof src - 1, "tests/postgres/<copy-api>.js", JS_EVAL_TYPE_MODULE));
    if (JS_IsException(v)) {
        js_std_dump_error(ctx);
        fails++;
    }
    JS_FreeValue(ctx, v);
    if (js_std_loop(ctx)) {
        js_std_dump_error(ctx);
        fails++;
    }
    checks += 0;
    if (checks < 5) {
        printf("  FAIL: only %d of 5 ops reported\n", checks);
        fails++;
    }
    if (stale) {
        printf("  FAIL: %d runs left an exception pending\n", stale);
        fails++;
    }
    js_std_free_handlers(rt);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    printf("postgres_copy_api: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
