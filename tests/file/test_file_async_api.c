// covers: c:src/modules/file/file-module.c c:src/modules/file/file-fs.c c:src/modules/file/file-filecopy.c c:src/modules/file/file-ahandle.c -- rule 18 embedder rows for
// dyna:file.async (F1 read/write, F2 path calls, F3a FileHandle: per-handle queue, fd handed to the in-flight job at teardown, fd counts): forced allocation failure in every file-module.c/file-fs.c/file-filecopy.c malloc (JS thread and pool worker), GC while requests are in flight, a one-shot interrupt with requests in flight,
// runtime teardown with requests pending (the shutdown sweep rejects and the late pool completion only frees C memory; run it
// under --san), and the DYN_MAX_INPUT edges on sparse files (max+1 rejects with RangeError before reading; FA_BIG=1 also reads
// exactly DYN_MAX_INPUT bytes). probe() fails a run when a dyna:file call returned with an exception pending. Hooks: dyn_aio_offload
// returning -1 must reject (not hang), and no I/O reactor must reject InternalError with no disk I/O on the JS thread.
// Two runtimes on one thread share the thread reactor: each registers its own loop (a second one used to fail "awaited promise can
// never settle"), a freed runtime detaches (no stale runtime pointer); another runtime's queued promise jobs stay queued while one runtime awaits.
// Usage: test_file_async_api [CASE-SUBSTRING]; run from the repo root.
#include "dynajs.h"
#include "dyna-libc.h"
#include "dyna-nat.h"
#include "dyna-io.h"
#include "dyna-aio.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int armed, fired, stale, fails, checks;
static unsigned long fa_fail_at;
static unsigned long fa_alloc_n;
static int fa_failed;
static char tdir[512];

#undef malloc
#undef realloc
void* malloc(size_t n);
void* realloc(void* p, size_t n);

static int fa_should_fail(void)
{
    unsigned long at = __atomic_load_n(&fa_fail_at, __ATOMIC_SEQ_CST);
    if (at && __atomic_add_fetch(&fa_alloc_n, 1, __ATOMIC_SEQ_CST) == at) {
        __atomic_store_n(&fa_failed, 1, __ATOMIC_SEQ_CST);
        return 1;
    }
    return 0;
}

static long fa_live;

#undef free
void free(void* p);

void fa_t_free(void* p)
{
    if (p)
        __atomic_sub_fetch(&fa_live, 1, __ATOMIC_SEQ_CST);
    free(p);
}

void* fa_t_malloc(size_t n)
{
    void* p = fa_should_fail() ? NULL : malloc(n);
    if (p)
        __atomic_add_fetch(&fa_live, 1, __ATOMIC_SEQ_CST);
    return p;
}

void* fa_t_realloc(void* p, size_t n)
{
    void* r = fa_should_fail() ? NULL : realloc(p, n);
    if (r && !p)
        __atomic_add_fetch(&fa_live, 1, __ATOMIC_SEQ_CST);
    return r;
}

static int fa_offload_fail, fa_acquire_null;

static int count_fds(void)
{
    int fd, n = 0;
    for (fd = 0; fd < 4096; fd++)
        if (fcntl(fd, F_GETFD) >= 0)
            n++;
    return n;
}

#undef dyn_aio_offload
#undef dyn_net_reactor_acquire
int dyn_aio_offload(dyn_aio_t* aio, void (*work)(void*), void (*done)(void*), void* arg);
dyn_aio_t* dyn_net_reactor_acquire(JSContext* ctx);

int fa_t_offload(dyn_aio_t* aio, void (*work)(void*), void (*done)(void*), void* arg)
{
    if (fa_offload_fail)
        return -1;
    return dyn_aio_offload(aio, work, done, arg);
}

dyn_aio_t* fa_t_acquire(JSContext* ctx)
{
    if (fa_acquire_null)
        return NULL;
    return dyn_net_reactor_acquire(ctx);
}

typedef struct hop {
    JSContext* ctx;
    JSValue fn[2];
    long left;
    dyn_aio_t* aio;
} hop_t;

static void hop_work(void* a)
{
    (void)a;
}

static void hop_done(void* a)
{
    hop_t* h = (hop_t*)a;
    JSValue r;
    if (--h->left > 0) {
        dyn_aio_offload(h->aio, hop_work, hop_done, h);
        return;
    }
    r = JS_Call(h->ctx, h->fn[0], JS_UNDEFINED, 0, NULL);
    JS_FreeValue(h->ctx, r);
    JS_FreeValue(h->ctx, h->fn[0]);
    JS_FreeValue(h->ctx, h->fn[1]);
    dyn_net_reactor_release(h->ctx);
    free(h);
}

static JSValue h_hop(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    int64_t n = 1;
    hop_t* h;
    JSValue p;
    (void)this_val;
    if (argc > 0 && JS_ToInt64(ctx, &n, argv[0]))
        return JS_EXCEPTION;
    h = (hop_t*)malloc(sizeof *h);
    if (!h)
        return JS_ThrowOutOfMemory(ctx);
    p = JS_NewPromiseCapability(ctx, h->fn);
    if (JS_IsException(p)) {
        free(h);
        return p;
    }
    h->ctx = ctx;
    h->left = n > 0 ? n : 1;
    h->aio = dyn_net_reactor_acquire(ctx);
    if (!h->aio) {
        JS_FreeValue(ctx, h->fn[0]);
        JS_FreeValue(ctx, h->fn[1]);
        JS_FreeValue(ctx, p);
        free(h);
        return JS_ThrowInternalError(ctx, "no reactor");
    }
    dyn_aio_offload(h->aio, hop_work, hop_done, h);
    return p;
}

static int handler(JSRuntime* rt, void* opaque)
{
    (void)rt;
    (void)opaque;
    if (armed) {
        armed = 0;
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

static size_t fa_js_big;
static int fa_big_rt;

static void* fa_js_malloc(JSMallocState* s, size_t n)
{
    (void)s;
    return fa_js_big && n >= fa_js_big ? NULL : malloc(n);
}

static void fa_js_free(JSMallocState* s, void* p)
{
    (void)s;
    free(p);
}

static void* fa_js_realloc(JSMallocState* s, void* p, size_t n)
{
    (void)s;
    return fa_js_big && n >= fa_js_big ? NULL : realloc(p, n);
}

static const JSMallocFunctions fa_js_mf = { fa_js_malloc, fa_js_free, fa_js_realloc, NULL };

static JSValue h_big_fail(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    int64_t n = 0;
    (void)this_val;
    if (argc > 0 && JS_ToInt64(ctx, &n, argv[0]))
        return JS_EXCEPTION;
    fa_js_big = n > 0 ? (size_t)n : 0;
    return JS_UNDEFINED;
}

static JSValue h_gc(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)this_val;
    (void)argc;
    (void)argv;
    JS_RunGC(JS_GetRuntime(ctx));
    return JS_UNDEFINED;
}

static JSValue h_fail_alloc(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    int64_t at = 0;
    (void)this_val;
    if (argc < 1 || JS_ToInt64(ctx, &at, argv[0]))
        return JS_EXCEPTION;
    __atomic_store_n(&fa_fail_at, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&fa_alloc_n, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&fa_failed, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&fa_fail_at, at > 0 ? (unsigned long)at : 0, __ATOMIC_SEQ_CST);
    return JS_UNDEFINED;
}

static JSValue h_alloc_failed(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)this_val;
    (void)argc;
    (void)argv;
    return JS_NewBool(ctx, __atomic_load_n(&fa_failed, __ATOMIC_SEQ_CST));
}

static JSValue h_offload_fail(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)this_val;
    fa_offload_fail = argc > 0 && JS_ToBool(ctx, argv[0]) > 0;
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

static JSRuntime* new_rt(JSContext** pctx)
{
    JSRuntime* rt = fa_big_rt ? JS_NewRuntime2(&fa_js_mf, NULL) : JS_NewRuntime();
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
    JS_SetPropertyStr(ctx, g, "gc", JS_NewCFunction(ctx, h_gc, "gc", 0));
    JS_SetPropertyStr(ctx, g, "bigFail", JS_NewCFunction(ctx, h_big_fail, "bigFail", 1));
    JS_SetPropertyStr(ctx, g, "failAlloc", JS_NewCFunction(ctx, h_fail_alloc, "failAlloc", 1));
    JS_SetPropertyStr(ctx, g, "allocFailed", JS_NewCFunction(ctx, h_alloc_failed, "allocFailed", 0));
    JS_SetPropertyStr(ctx, g, "TDIR", JS_NewString(ctx, tdir));
    JS_SetPropertyStr(ctx, g, "offloadFail", JS_NewCFunction(ctx, h_offload_fail, "offloadFail", 1));
    JS_SetPropertyStr(ctx, g, "BIG", JS_NewBool(ctx, getenv("FA_BIG") != NULL));
    JS_SetPropertyStr(ctx, g, "hop", JS_NewCFunction(ctx, h_hop, "hop", 1));
    JS_SetPropertyStr(ctx, g, "FA_N", JS_NewInt32(ctx, getenv("FA_N") ? atoi(getenv("FA_N")) : 2000));
    JS_FreeValue(ctx, g);
    *pctx = ctx;
    return rt;
}

static void free_rt(JSRuntime* rt, JSContext* ctx)
{
    js_std_free_handlers(rt);
    JS_FreeContext(ctx);
    JS_SetExitTeardown(rt, 1);
    JS_FreeRuntime(rt);
}

static void check(int ok, const char* name, const char* what)
{
    checks++;
    if (!ok) {
        fails++;
        printf("  FAIL: [%s] %s\n", name, what);
    }
}

static char* result_of(JSContext* ctx)
{
    JSValue g = JS_GetGlobalObject(ctx), r = JS_GetPropertyStr(ctx, g, "res");
    const char* s = JS_ToCString(ctx, r);
    const char* src = s ? s : "(no result)";
    size_t sl = strlen(src);
    char* out = (char*)malloc(sl + 1);
    if (out)
        memcpy(out, src, sl + 1);
    if (s)
        JS_FreeCString(ctx, s);
    else
        JS_FreeValue(ctx, JS_GetException(ctx));
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, g);
    if (!out)
        abort();
    return out;
}

static int run_module(JSContext* ctx, const char* src, int loop)
{
    JSValue v = js_std_await(ctx, JS_Eval(ctx, src, strlen(src), "<case>", JS_EVAL_TYPE_MODULE));
    int rc = JS_IsException(v) ? -1 : 0;
    if (rc) {
        JSValue e = JS_GetException(ctx);
        const char* m = JS_ToCString(ctx, e);
        printf("  module threw: %s\n", m ? m : "(unprintable)");
        if (m)
            JS_FreeCString(ctx, m);
        else
            JS_FreeValue(ctx, JS_GetException(ctx));
        JS_FreeValue(ctx, e);
    }
    JS_FreeValue(ctx, v);
    if (!rc && loop)
        rc = js_std_loop(ctx);
    return rc;
}

#define PRE                                                       \
    "import * as file from 'dyna:file';\n"                        \
    "const A = file.async, P = (n) => TDIR + '/' + n;\n"          \
    "file.writeFile(P('s.txt'), 'small \\xff text');\n"

static const char ok_src[] = PRE
    "const want = file.readFile(P('s.txt')), got = [];\n"
    "for (let i = 0; i < 32; i++) got.push(i % 2 ? A.readFile(P('s.txt')) : A.readBytes(P('s.txt')));\n"
    "for (let i = 0; i < 32; i++) got.push(A.writeFile(P('g' + i), 'v' + i, { atomic: i % 3 === 0 }));\n"
    "gc(); gc();\n"
    "const all = await Promise.all(got);\n"
    "gc();\n"
    "let bad = 0;\n"
    "for (let i = 0; i < 32; i++) if (i % 2 ? all[i] !== want : all[i].length !== 13) bad++;\n"
    "for (let i = 0; i < 32; i++) if (file.readFile(P('g' + i)) !== 'v' + i) bad++;\n"
    "globalThis.res = bad ? 'bad ' + bad : 'ok';\n";

static const char gc_drop_src[] = PRE
    "for (let i = 0; i < 64; i++) A.readFile(P('s.txt'));\n"
    "for (let i = 0; i < 16; i++) A.writeFile(P('d' + i), new Uint8Array(1 << 16));\n"
    "for (let i = 0; i < 32; i++) { A.readDir(P('.')); A.realPath(P('s.txt')); A.stat(P('s.txt')); A.readLink(P('s.txt')).catch(() => {}); }\n"
    "gc();\n"
    "const last = await A.readFile(P('s.txt'));\n"
    "gc();\n"
    "globalThis.res = last === file.readFile(P('s.txt')) ? 'ok' : 'bad';\n";

static const char interrupt_src[] = PRE
    "file.writeFile(P('big.bin'), new Uint8Array(16 << 20));\n"
    "const ps = [];\n"
    "for (let i = 0; i < 8; i++) ps.push(A.readBytes(P('big.bin')), A.writeFile(P('ib' + i), new Uint8Array(1 << 20)), A.readDir(P('.')), A.stat(P('big.bin')), A.copyFile(P('big.bin'), P('ic' + i), { overwrite: true }));\n"
    "const ih = await A.open(P('big.bin'));\n"
    "for (let i = 0; i < 8; i++) ps.push(ih.read(1 << 20, i << 20), ih.stat());\n"
    "const ir = await A.reader(P('big.bin')), iw = await A.writer(P('iw.bin'));\n"
    "for (let i = 0; i < 8; i++) ps.push(ir.readBytes(1 << 20), ir.readLine(), iw.write(new Uint8Array(1 << 20)), iw.flush());\n"
    "try { arm(); for (;;) {} } catch (e) { globalThis.leak = String(e); }\n"
    "globalThis.leak = 'after';\n";

static const char teardown_src[] = PRE
    "file.writeFile(P('big.bin'), new Uint8Array(16 << 20));\n"
    "file.makeDir(P('td'), { recursive: true }); for (let i = 0; i < 80; i++) file.writeFile(P('td/' + 'x'.repeat(60) + i), '');\n"
    "for (let i = 0; i < 8; i++) { A.readBytes(P('big.bin')); A.readFile(P('big.bin')).then(() => {}, () => {}); A.writeFile(P('t' + i), new Uint8Array(1 << 20), { atomic: true }); A.readDir(P('td')); A.realPath(P('big.bin')); A.readLink(P('big.bin')).catch(() => {}); A.stat(P('big.bin')); A.copyFile(P('big.bin'), P('tc' + i), { overwrite: true }); }\n"
    "globalThis.res = 'started';\n";

static const char handle_gc_src[] = PRE
    "let p;\n"
    "{ const h = await A.open(P('gh.bin'), { flags: 'w+' }); for (let i = 0; i < 32; i++) h.write('x'.repeat(1000)); p = h.read(32000, 0); }\n"
    "for (let i = 0; i < 16; i++) A.open(P('s.txt')).then((h) => h.read(4));\n"
    "gc(); gc();\n"
    "const r = await p;\n"
    "for (let i = 0; i < 4; i++) { await A.readFile(P('s.txt')); gc(); }\n"
    "globalThis.res = r.length === 32000 ? 'ok' : 'bad ' + r.length;\n";

static const char handle_teardown_src[] = PRE
    "file.writeFile(P('hbig.bin'), new Uint8Array(16 << 20));\n"
    "globalThis.hs = [];\n"
    "for (let i = 0; i < 6; i++) { const h = await A.open(P('hbig.bin'), { flags: 'r+' }); hs.push(h); for (let k = 0; k < 4; k++) h.read(4 << 20, k << 20).catch(() => {}); h.write('x', 0).catch(() => {}); h.close().catch(() => {}); }\n"
    "for (let i = 0; i < 4; i++) { const h = await A.open(P('hbig.bin')); h.read(16 << 20).catch(() => {}); h.stat().catch(() => {}); }\n"
    "for (let i = 0; i < 4; i++) A.open(P('hbig.bin')).catch(() => {});\n"
    "globalThis.res = 'started';\n";

static const char handle_offload_src[] = PRE
    "const out = [];\n"
    "const h = await A.open(P('s.txt'));\n"
    "const a = h.read(4);\n"
    "offloadFail(1);\n"
    "const b = h.read(4), c = h.stat(), d = h.close();\n"
    "const nh = A.open(P('s.txt'));\n"
    "out.push((await a).length === 4 ? 'ok' : 'bad a');\n"
    "offloadFail(0);\n"
    "for (const p of [b, c, d, nh]) { try { await p; out.push('resolved'); } catch (e) { out.push(e instanceof InternalError && /I\\/O pool refused/.test(e.message) ? 'ok' : 'bad ' + e); } }\n"
    "out.push(h.closed ? 'ok' : 'bad closed');\n"
    "globalThis.res = out.every((s) => s === 'ok') ? 'ok' : 'bad ' + out.join();\n";

static const char stream_gc_src[] = PRE
    "file.writeFile(P('sg.txt'), 'line\\n'.repeat(20000));\n"
    "let p, q;\n"
    "{ const r = await A.reader(P('sg.txt'), { bufferSize: 4096 }); p = r.readAll(); }\n"
    "{ const w = await A.writer(P('sgw.txt')); for (let i = 0; i < 50; i++) w.write('y'.repeat(100)); q = w.close(); }\n"
    "for (let i = 0; i < 16; i++) A.reader(P('s.txt')).then((r) => r.readLine());\n"
    "for (let i = 0; i < 16; i++) A.writer(P('sgd' + i)).then((w) => w.write('dropped'));\n"
    "gc(); gc();\n"
    "const s = await p; await q;\n"
    "for (let i = 0; i < 4; i++) { await A.readFile(P('s.txt')); gc(); }\n"
    "let d = 0; for (let i = 0; i < 16; i++) d += file.readFile(P('sgd' + i)) === 'dropped' ? 1 : 0;\n"
    "globalThis.res = s.length === 100000 && file.readFile(P('sgw.txt')).length === 5000 && d === 16 ? 'ok' : 'bad ' + s.length + ' ' + d;\n";

static const char writer_teardown_src[] = PRE
    "globalThis.ws = [];\n"
    "{ const w = await A.writer(P('wt_idle.txt')); await w.write('idle-data'); ws.push(w); }\n"
    "{ const w = await A.writer(P('wt_fly.txt'), { bufferSize: 64 << 20 }); const mb = new Uint8Array(1 << 20).fill(97); for (let i = 0; i < 60; i++) await w.write(mb); w.write(new Uint8Array(5 << 20).fill(98)).catch(() => {}); ws.push(w); }\n"
    "{ const r = await A.reader(P('s.txt')); r.readAll().catch(() => {}); r.readLine().catch(() => {}); ws.push(r); }\n"
    "globalThis.res = 'started';\n";

static const char fast_oom_src[] = PRE
    "file.writeFile(P('fo.txt'), 'short\\n' + 'L'.repeat(500000) + '\\nnext\\n');\n"
    "const r = await A.reader(P('fo.txt'), { bufferSize: 1 << 20 });\n"
    "const a = await r.readLine();\n"
    "let e = null; bigFail(400000); const p = r.readLine(); bigFail(0);\n"
    "try { await p; } catch (x) { e = x; }\n"
    "const b = await r.readLine(), c = await r.readLine(), d = await r.readLine(); await r.close();\n"
    "globalThis.res = a === 'short' && e instanceof Error && /memory/i.test(e.message) && b.length === 500000 && c === 'next' && d === null ? 'ok' : 'bad ' + [a, e, b && b.length, c, d];\n";

static const char hook_src[] = PRE
    "const bin = new Uint8Array([0x61, 0xff, 0x62, 0xc3, 0xa9]);\n"
    "file.writeFile(P('h.bin'), bin);\n"
    "const want = file.readFile(P('h.bin')), bad = [], seen = {};\n"
    "file.makeDir(P('hd'), { recursive: true }); for (let i = 0; i < 40; i++) file.writeFile(P('hd/' + 'n'.repeat(60) + i), '');\n"
    "try { file.symlink('h.bin', P('hl')); } catch {}\n"
    "const ops = {\n"
    "  readFile: [() => A.readFile(P('h.bin')), (r) => r === want],\n"
    "  readFileStr: [() => A.readFile(P('s.txt')), (r) => r === file.readFile(P('s.txt'))],\n"
    "  readFileBytes: [() => A.readFile(P('h.bin'), { bytes: true }), (r) => r.length === 5 && r[1] === 0xff],\n"
    "  readBytes: [() => A.readBytes(P('h.bin')), (r) => r.length === 5 && r[4] === 0xa9],\n"
    "  writeFile: [() => A.writeFile(P('hw.bin'), bin), (r) => r === 5 && file.readBytes(P('hw.bin')).length === 5],\n"
    "  writeAtomic: [() => A.writeFile(P('hw.bin'), 'xyz', { atomic: true }), (r) => r === 3 && file.readFile(P('hw.bin')) === 'xyz'],\n"
    "  stat: [() => A.stat(P('h.bin')), (r) => r.size === 5 && r.isFile],\n"
    "  exists: [() => A.exists(P('h.bin')), (r) => r === true],\n"
    "  readDirLong: [() => A.readDir(P('hd')), (r) => r.length === 40 && r[39].name.length > 60],\n"
    "  readLink: [() => A.readLink(P('hl')), (r) => r === 'h.bin'],\n"
    "  realPath: [() => A.realPath(P('h.bin')), (r) => String(r).endsWith('/h.bin')],\n"
    "  copyFile: [() => A.copyFile(P('h.bin'), P('hc.bin'), { overwrite: true }), (r) => r === 5],\n"
    "  makeDir: [() => A.makeDir(P('hm/a/b'), { recursive: true }), (r) => r === undefined],\n"
    "  symlink: [() => (file.removeAll(P('hs')), A.symlink('t', P('hs'))), (r) => r === undefined],\n"
    "  chmod: [() => A.chmod(P('h.bin'), 0o644), (r) => r === undefined],\n"
    "  reader: [async () => { const r = await A.reader(P('h.bin')); try { return [await r.readLine(), await r.read(), await r.readLine()]; } finally { await r.close(); } }, (r) => r[0] === want && r[1] === '' && r[2] === null],\n"
    "  lines: [async () => { file.writeFile(P('hl.txt'), 'l1\\nl2\\r\\nl3'); const r = await A.reader(P('hl.txt')); try { return [await r.readLine(), await r.readLine(), await r.read(1), await r.readLine(), await r.readLine()].join(); } finally { await r.close(); } }, (r) => r === 'l1,l2,l,3,'],\n"
    "  writer: [async () => { const w = await A.writer(P('hw2.bin')); try { await w.write('abc'); await w.write(bin); await w.flush(); await w.write('z'); } finally { await w.close(); } return file.readBytes(P('hw2.bin')).length; }, (r) => r === 9],\n"
    "  handle: [async () => { const h = await A.open(P('hh.bin'), { flags: 'w+' }); try { await h.write('abc'); const r = await h.read(3, 0); const v = new Uint8Array(2); const n = await h.read(v, 1); await h.sync(); const s = await h.stat(); return [r.length, n, v[1], s.size]; } finally { await h.close(); } }, (r) => r[0] === 3 && r[1] === 2 && r[2] === 99 && r[3] === 3],\n"
    "};\n"
    "for (const [name, [op, good]] of Object.entries(ops)) {\n"
    "  seen[name] = 0;\n"
    "  for (let at = 1; at < 64; at++) {\n"
    "    let r, threw = false; failAlloc(at);\n"
    "    try { r = await op(); } catch (e) { r = e; threw = true; }\n"
    "    const failed = allocFailed(); failAlloc(0); probe();\n"
    "    if (!failed) { if (threw || !good(r)) bad.push(name + ' wrong result ' + r + ' with no failed allocation'); break; }\n"
    "    seen[name]++;\n"
    "    if (threw && !(r instanceof Error)) bad.push(name + '@' + at + ' threw a non-Error ' + r);\n"
    "    else if (threw && !/out of memory|ENOMEM|Cannot allocate/i.test(String(r.message))) bad.push(name + '@' + at + ' unexpected ' + r);\n"
    "    else if (!threw && !good(r)) bad.push(name + '@' + at + ' resolved a wrong result');\n"
    "  }\n"
    "}\n"
    "const counts = Object.entries(seen).map(([k, v]) => k + '=' + v).join(' ');\n"
    "if (Object.values(seen).some((v) => v === 0)) bad.push('an op never hit a failing allocation: ' + counts);\n"
    "globalThis.res = bad.length ? 'bad ' + bad.slice(0, 4).join('; ') : 'ok ' + counts;\n";

static const char limits_src[] = PRE
    "const out = [];\n"
    "const syncMsg = (f) => { try { f(); } catch (e) { return e.constructor.name + ':' + e.message; } return 'none'; };\n"
    "for (const [f, s] of [[() => A.readFile(P('over.bin')), () => file.readFile(P('over.bin'))], [() => A.readBytes(P('over.bin')), () => file.readBytes(P('over.bin'))], [() => A.readFile(P('over.bin'), { bytes: true }), () => file.readFile(P('over.bin'), { bytes: true })]]) {\n"
    "  try { await f(); out.push('resolved'); } catch (e) { out.push(e instanceof RangeError && /DYN_MAX_INPUT/.test(e.message) && syncMsg(s) === 'RangeError:' + e.message ? 'ok' : 'bad ' + e); }\n"
    "}\n"
    "const z = await A.readBytes(P('zero.bin'));\n"
    "out.push(z.length === 0 && (await A.readFile(P('zero.bin'))) === '' ? 'ok' : 'bad zero');\n"
    "if (BIG) { const m = await A.readBytes(P('max.bin')); out.push(m.length === 1073741824 && m[m.length - 1] === 0 ? 'ok' : 'bad max ' + m.length); }\n"
    "globalThis.res = out.every((s) => s === 'ok') ? 'ok ' + out.length : 'bad ' + out.join(',');\n";

static const char floor_src[] = PRE
    "const now = () => performance.now(), N = FA_N, r = {};\n"
    "file.writeFile(P('b4k'), new Uint8Array(4096).fill(97));\n"
    "await A.readFile(P('b4k'));\n"
    "let t = now(); await hop(N); r.hopChained = (now() - t) * 1000 / N;\n"
    "t = now(); for (let i = 0; i < N; i++) await hop(1); r.hopAwaited = (now() - t) * 1000 / N;\n"
    "t = now(); for (let i = 0; i < N; i++) await A.readFile(P('b4k')); r.asyncRead = (now() - t) * 1000 / N;\n"
    "t = now(); for (let i = 0; i < N; i++) await A.readBytes(P('b4k')); r.asyncBytes = (now() - t) * 1000 / N;\n"
    "t = now(); for (let i = 0; i < N; i++) file.readFile(P('b4k')); r.syncRead = (now() - t) * 1000 / N;\n"
    "t = now(); for (let i = 0; i < N; i += 64) { const b = []; for (let k = 0; k < 64; k++) b.push(A.readFile(P('b4k'))); await Promise.all(b); } r.asyncRead64 = (now() - t) * 1000 / N;\n"
    "for (const k in r) r[k] = +r[k].toFixed(2);\n"
    "print('floor us/op ' + JSON.stringify(r));\n"
    "globalThis.res = 'ok ' + JSON.stringify(r);\n";

static const char offload_fail_src[] = PRE
    "const out = [];\n"
    "for (const f of [() => A.readFile(P(\'s.txt\')), () => A.readBytes(P(\'s.txt\')), () => A.writeFile(P(\'of.txt\'), \'x\', { atomic: true }), () => A.open(P(\'s.txt\')), () => A.reader(P(\'s.txt\')), () => A.writer(P(\'ofw.txt\'))]) {\n"
    "  try { await f(); out.push(\'resolved\'); } catch (e) { out.push(e instanceof InternalError && /I\\/O pool refused/.test(e.message) ? \'ok\' : \'bad \' + e); }\n"
    "}\n"
    "globalThis.res = out.every((s) => s === \'ok\') && !file.exists(P(\'of.txt\')) ? \'ok\' : \'bad \' + out.join();\n";

static const char no_reactor_src[] = PRE
    "const out = [];\n"
    "for (const f of [() => A.readFile(P(\'s.txt\')), () => A.readBytes(P(\'s.txt\')), () => A.writeFile(P(\'nr.txt\'), \'x\'), () => A.open(P(\'s.txt\')), () => A.reader(P(\'s.txt\')), () => A.writer(P(\'ofw.txt\'))]) {\n"
    "  const r = f();\n"
    "  if (!(r instanceof Promise)) { out.push(\'not a promise\'); continue; }\n"
    "  try { await r; out.push(\'resolved\'); } catch (e) { out.push(e instanceof InternalError && /io reactor could not be created/.test(e.message) ? \'ok\' : \'bad \' + e); }\n"
    "}\n"
    "globalThis.res = out.every((s) => s === \'ok\') && !file.exists(P(\'nr.txt\')) ? \'ok\' : \'bad \' + out.join() + (file.exists(P(\'nr.txt\')) ? \' wrote-inline\' : \'\');\n";

static void run_cross_teardown(void)
{
    JSContext *cb, *ca;
    JSRuntime *rb, *ra;
    long live0 = __atomic_load_n(&fa_live, __ATOMIC_SEQ_CST);
    char* s;
    int rc;
    rb = new_rt(&cb);
    rc = run_module(cb, PRE "globalThis.res = (await A.readFile(P('s.txt'))).length > 0 ? 'armed' : 'bad';\n", 1);
    ra = new_rt(&ca);
    rc |= run_module(ca, teardown_src, 0);
    free_rt(ra, ca);
    rc |= run_module(cb, PRE "for (let i = 0; i < 40; i++) { await A.readBytes(P('big.bin')); }\nglobalThis.res = 'ok';\n", 1);
    s = result_of(cb);
    printf("cross-runtime teardown: rc=%d res=%s\n", rc, s);
    check(rc == 0 && !strcmp(s, "ok"), "cross-runtime teardown", "the surviving runtime keeps working while the dead runtime's requests complete");
    free(s);
    free_rt(rb, cb);
    printf("cross-runtime teardown: live file-module allocations %ld -> %ld\n", live0, __atomic_load_n(&fa_live, __ATOMIC_SEQ_CST));
    check(__atomic_load_n(&fa_live, __ATOMIC_SEQ_CST) == live0, "cross-runtime teardown", "every request of the torn-down runtime is freed");
}

static void run_two_runtimes(int free_first_owner)
{
    static const char aw[] = PRE "let n = 0; for (let i = 0; i < 20; i++) n += (await A.readFile(P('s.txt'))).length + (await A.stat(P('s.txt'))).size;\nglobalThis.res = n === 20 * 25 ? 'ok' : 'bad ' + n;\n";
    const char* name = free_first_owner ? "second runtime awaits after the first is freed" : "second runtime awaits while the first lives";
    JSContext *c1, *c2;
    JSRuntime *r1, *r2;
    long live0 = __atomic_load_n(&fa_live, __ATOMIC_SEQ_CST);
    char *s1, *s2;
    int rc1, rc2, rc3 = 0;
    r1 = new_rt(&c1);
    rc1 = run_module(c1, aw, 1);
    r2 = new_rt(&c2);
    if (free_first_owner) {
        s1 = result_of(c1);
        free_rt(r1, c1);
        r1 = NULL;
    }
    rc2 = run_module(c2, aw, 1);
    s2 = result_of(c2);
    if (!free_first_owner) {
        rc3 = run_module(c1, aw, 1);
        s1 = result_of(c1);
        free_rt(r1, c1);
    }
    printf("%s: rc=%d/%d/%d res=%s/%s\n", name, rc1, rc2, rc3, s1, s2);
    check(rc1 == 0 && rc2 == 0 && rc3 == 0 && !strcmp(s1, "ok") && !strcmp(s2, "ok"), name, "both runtimes on one thread can await dyna:file.async");
    free(s1);
    free(s2);
    free_rt(r2, c2);
    check(__atomic_load_n(&fa_live, __ATOMIC_SEQ_CST) == live0, name, "every request is freed");
}

static void run_job_isolation(void)
{
    static const char start1[] = PRE "globalThis.p = A.readFile(P('s.txt'));\n";
    static const char start2[] = PRE "A.readFile(P('s.txt')).then(() => { globalThis.done2 = 1; });\nPromise.resolve().then(() => { globalThis.ran2 = 1; });\n";
    static const char await1[] = "globalThis.res = (await globalThis.p).length === 12 ? 'ok' : 'bad';\n";
    const char* name = "reactor runs only the polling runtime's jobs";
    JSContext *c1, *c2;
    JSRuntime *r1, *r2;
    JSValue v, g;
    char *s1, *early, *late;
    int rc1, rc2;
    r1 = new_rt(&c1);
    r2 = new_rt(&c2);
    v = JS_Eval(c1, start1, strlen(start1), "<start1>", JS_EVAL_TYPE_MODULE);
    JS_FreeValue(c1, v);
    v = JS_Eval(c2, start2, strlen(start2), "<start2>", JS_EVAL_TYPE_MODULE);
    JS_FreeValue(c2, v);
    rc1 = run_module(c1, await1, 0);
    s1 = result_of(c1);
    g = JS_GetGlobalObject(c2);
    JS_SetPropertyStr(c2, g, "res", JS_GetPropertyStr(c2, g, "ran2"));
    early = result_of(c2);
    rc2 = js_std_loop(c2);
    JS_SetPropertyStr(c2, g, "res", JS_GetPropertyStr(c2, g, "done2"));
    JS_FreeValue(c2, g);
    late = result_of(c2);
    printf("%s: rc=%d/%d res=%s ran2-before=%s done2-after=%s\n", name, rc1, rc2, s1, early, late);
    check(rc1 == 0 && rc2 == 0 && !strcmp(s1, "ok") && !strcmp(early, "undefined") && !strcmp(late, "1"), name, "the other runtime's promise jobs stay queued until its own loop runs");
    free(s1);
    free(early);
    free(late);
    free_rt(r1, c1);
    free_rt(r2, c2);
}

static int sparse(const char* name, off_t size)
{
    char p[600];
    int fd;
    snprintf(p, sizeof p, "%s/%s", tdir, name);
    fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return -1;
    if (ftruncate(fd, size) < 0) {
        close(fd);
        return -1;
    }
    return close(fd);
}

static void run_ok_case(const char* name, const char* src, const char* want_prefix)
{
    JSContext* ctx;
    JSRuntime* rt = new_rt(&ctx);
    int rc;
    char* s;
    stale = 0;
    rc = run_module(ctx, src, 1);
    s = result_of(ctx);
    printf("%s: rc=%d res=%s\n", name, rc, s);
    check(rc == 0, name, "the run ended normally");
    check(!strncmp(s, want_prefix, strlen(want_prefix)), name, "expected result");
    check(stale == 0, name, "no call returned with an exception pending");
    free(s);
    free_rt(rt, ctx);
}

static void run_interrupt(void)
{
    JSContext* ctx;
    JSRuntime* rt = new_rt(&ctx);
    JSValue g, l;
    int rc;
    armed = fired = 0;
    rc = run_module(ctx, interrupt_src, 1);
    g = JS_GetGlobalObject(ctx);
    l = JS_GetPropertyStr(ctx, g, "leak");
    printf("interrupt in flight: rc=%d fired=%d\n", rc, fired);
    check(fired, "interrupt in flight", "the handler fired");
    check(rc != 0, "interrupt in flight", "the run ended with the interrupt");
    check(JS_IsUndefined(l), "interrupt in flight", "user code never observed the interrupt");
    JS_FreeValue(ctx, l);
    JS_FreeValue(ctx, g);
    free_rt(rt, ctx);
}

static long tfile(const char* name, char* first, size_t fn)
{
    char p[1024];
    struct stat st;
    FILE* f;
    snprintf(p, sizeof p, "%s/%s", tdir, name);
    if (first && fn)
        memset(first, 0, fn);
    if (stat(p, &st) < 0)
        return -1;
    if (first && fn && (f = fopen(p, "rb")) != NULL) {
        if (fread(first, 1, fn - 1, f) == 0)
            first[0] = 0;
        fclose(f);
    }
    return (long)st.st_size;
}

static void run_writer_teardown(void)
{
    int f0 = count_fds(), rc;
    long live0 = __atomic_load_n(&fa_live, __ATOMIC_SEQ_CST), fly;
    char idle[32];
    char* s;
    JSContext* ctx;
    JSRuntime* rt = new_rt(&ctx);
    rc = run_module(ctx, writer_teardown_src, 0);
    s = result_of(ctx);
    printf("writer teardown flush: rc=%d res=%s\n", rc, s);
    check(rc == 0 && !strcmp(s, "started"), "writer teardown flush", "writers started");
    free(s);
    free_rt(rt, ctx);
    usleep(300000);
    rt = new_rt(&ctx);
    rc = run_module(ctx, PRE "globalThis.res = (await A.readFile(P('s.txt'))) === file.readFile(P('s.txt')) ? 'ok' : 'bad';\n", 1);
    free_rt(rt, ctx);
    usleep(300000);
    tfile("wt_idle.txt", idle, sizeof idle);
    fly = tfile("wt_fly.txt", NULL, 0);
    printf("writer teardown flush: idle=%s fly=%ld fds %d -> %d live %ld -> %ld\n", idle, fly, f0, count_fds(), live0, __atomic_load_n(&fa_live, __ATOMIC_SEQ_CST));
    check(rc == 0, "writer teardown flush", "a fresh runtime after teardown works");
    check(!strcmp(idle, "idle-data"), "writer teardown flush", "an idle writer's buffered bytes reach the file at teardown");
    check(fly == 65L << 20, "writer teardown flush", "the in-flight write's buffer passes to the job and is flushed whole");
    check(count_fds() == f0, "writer teardown flush", "every reader and writer fd is closed");
    check(__atomic_load_n(&fa_live, __ATOMIC_SEQ_CST) == live0, "writer teardown flush", "every buffer and job is freed");
}

static void run_teardown(void)
{
    int round, fds0;
    long live0 = __atomic_load_n(&fa_live, __ATOMIC_SEQ_CST);
    {
        JSContext* ctx;
        JSRuntime* rt = new_rt(&ctx);
        run_module(ctx, PRE "globalThis.res = (await A.readFile(P('s.txt'))).length;\n", 1);
        free_rt(rt, ctx);
    }
    usleep(100000);
    fds0 = count_fds();
    for (round = 0; round < 8; round++) {
        JSContext* ctx;
        JSRuntime* rt = new_rt(&ctx);
        long before = __atomic_load_n(&fa_live, __ATOMIC_SEQ_CST);
        int rc = run_module(ctx, round & 1 ? handle_teardown_src : teardown_src, 0);
        char* s = result_of(ctx);
        check(rc == 0 && !strcmp(s, "started"), "teardown in flight", "requests started");
        free(s);
        free_rt(rt, ctx);
        if (round & 1) {
            long left = __atomic_load_n(&fa_live, __ATOMIC_SEQ_CST) - before;
            printf("teardown in flight: round %d leaves %ld allocations to in-flight jobs\n", round, left);
            check(left <= 24, "teardown in flight", "a runtime torn down with queued FileHandle calls leaves at most its in-flight jobs behind");
        }
    }
    usleep(300000);
    {
        JSContext* ctx;
        JSRuntime* rt = new_rt(&ctx);
        int rc = run_module(ctx, PRE "globalThis.res = (await A.readFile(P('s.txt'))) === file.readFile(P('s.txt')) ? 'ok' : 'bad';\n", 1);
        char* s = result_of(ctx);
        printf("teardown in flight: next runtime rc=%d res=%s\n", rc, s);
        check(rc == 0 && !strcmp(s, "ok"), "teardown in flight", "a fresh runtime after torn-down requests works");
        free(s);
        free_rt(rt, ctx);
    }
    usleep(300000);
    printf("teardown in flight: fds %d -> %d\n", fds0, count_fds());
    check(count_fds() == fds0, "teardown in flight", "every FileHandle fd (idle, queued and in flight) is closed");
    printf("teardown in flight: live file-module allocations %ld -> %ld\n", live0, __atomic_load_n(&fa_live, __ATOMIC_SEQ_CST));
    check(__atomic_load_n(&fa_live, __ATOMIC_SEQ_CST) == live0, "teardown in flight", "every torn-down request is freed (reaped after the pool drains)");
}

int main(int argc, char** argv)
{
    const char* only = argc > 1 ? argv[1] : NULL;
    const char* base = getenv("TMPDIR");
    alarm(300);
    snprintf(tdir, sizeof tdir, "%s/dj_fa_api_%d", base && *base ? base : ".", (int)getpid());
    if (mkdir(tdir, 0755) < 0) {
        printf("FAIL: mkdir %s\n", tdir);
        return 1;
    }
    if (sparse("over.bin", (off_t)DYN_MAX_INPUT + 1) < 0 || sparse("zero.bin", 0) < 0 || (getenv("FA_BIG") && sparse("max.bin", (off_t)DYN_MAX_INPUT) < 0)) {
        printf("FAIL: sparse files\n");
        return 1;
    }
    if (!only || strstr("gc in flight", only))
        run_ok_case("gc in flight", ok_src, "ok");
    if (!only || strstr("gc drop in flight", only))
        run_ok_case("gc drop in flight", gc_drop_src, "ok");
    if (!only || strstr("handle gc in flight", only)) {
        int f0 = count_fds();
        run_ok_case("handle gc in flight", handle_gc_src, "ok");
        usleep(100000);
        printf("handle gc in flight: fds %d -> %d\n", f0, count_fds());
        check(count_fds() <= f0, "handle gc in flight", "handles dropped unclosed are closed by GC");
    }
    if (!only || strstr("handle offload fails", only)) {
        alarm(30);
        run_ok_case("handle offload fails", handle_offload_src, "ok");
        fa_offload_fail = 0;
        alarm(300);
    }
    if (!only || strstr("stream gc in flight", only)) {
        int f0 = count_fds();
        run_ok_case("stream gc in flight", stream_gc_src, "ok");
        usleep(100000);
        printf("stream gc in flight: fds %d -> %d\n", f0, count_fds());
        check(count_fds() <= f0, "stream gc in flight", "readers and writers dropped unclosed are closed by GC");
    }
    if (!only || strstr("fast path oom", only)) {
        fa_big_rt = 1;
        run_ok_case("fast path oom", fast_oom_src, "ok");
        fa_big_rt = 0;
        fa_js_big = 0;
    }
    if (!only || strstr("writer teardown flush", only))
        run_writer_teardown();
    if (!only || strstr("limits", only))
        run_ok_case("limits", limits_src, "ok");
    if (!only || strstr("alloc hook", only))
        run_ok_case("alloc hook", hook_src, "ok ");
    if (!only || strstr("interrupt in flight", only))
        run_interrupt();
    if (!only || strstr("teardown in flight", only))
        run_teardown();
    if (!only || strstr("cross-runtime teardown", only))
        run_cross_teardown();
    if (!only || strstr("second runtime awaits while the first lives", only))
        run_two_runtimes(0);
    if (!only || strstr("second runtime awaits after the first is freed", only))
        run_two_runtimes(1);
    if (!only || strstr("reactor runs only the polling runtime's jobs", only))
        run_job_isolation();
    if (!only || strstr("offload fails", only)) {
        alarm(30);
        fa_offload_fail = 1;
        run_ok_case("offload fails", offload_fail_src, "ok");
        fa_offload_fail = 0;
        alarm(300);
    }
    if (!only || strstr("no reactor", only)) {
        alarm(30);
        fa_acquire_null = 1;
        run_ok_case("no reactor", no_reactor_src, "ok");
        fa_acquire_null = 0;
        alarm(300);
    }
    if (only && strstr("floor", only))
        run_ok_case("floor", floor_src, "ok");
    {
        char cmd[700];
        snprintf(cmd, sizeof cmd, "rm -rf '%s'", tdir);
        if (system(cmd) != 0)
            printf("note: could not remove %s\n", tdir);
    }
    printf("file_async_api: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
