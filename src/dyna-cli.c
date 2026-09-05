/*
 * DynaJS stand alone interpreter
 *
 * Copyright (c) 2017-2021 Fabrice Bellard
 * Copyright (c) 2017-2021 Charlie Gordon
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <inttypes.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#if defined(__APPLE__)
#include <malloc/malloc.h>
#elif defined(__linux__) || defined(__GLIBC__)
#include <malloc.h>
#elif defined(__FreeBSD__)
#include <malloc_np.h>
#endif

#include "cutils.h"
#include "dyna-libc.h"
#include "dyna-io.h"
#ifdef CONFIG_NATIVE_MODULES
#include "dyna-nat.h"
#include "core/dyn-pool.h"   /* --io-threads sets the pool's default */
#endif

extern const uint8_t dynajsc_repl[];
extern const uint32_t dynajsc_repl_size;

static int eval_buf(JSContext *ctx, const void *buf, int buf_len,
                    const char *filename, int eval_flags)
{
    JSValue val;
    int ret;

    if ((eval_flags & JS_EVAL_TYPE_MASK) == JS_EVAL_TYPE_MODULE) {
        /* for the modules, we compile then run to be able to set
           import.meta */
        val = JS_Eval(ctx, buf, buf_len, filename,
                      eval_flags | JS_EVAL_FLAG_COMPILE_ONLY);
        if (!JS_IsException(val)) {
            js_module_set_import_meta(ctx, val, TRUE, TRUE);
            val = JS_EvalFunction(ctx, val);
        }
        val = js_std_await(ctx, val);
    } else {
        val = JS_Eval(ctx, buf, buf_len, filename, eval_flags);
    }
    if (JS_IsException(val)) {
        js_std_dump_error(ctx);
        ret = -1;
    } else {
        ret = 0;
    }
    JS_FreeValue(ctx, val);
    return ret;
}

/* ===================== content-addressed bytecode cache =====================
 * Startup on a large program is dominated by parsing (measured ~185 ms to parse
 * 100k lines vs ~20 ms to load the equivalent bytecode). This caches the
 * compiled bytecode next to the source as "<file>.qbc" so a warm start skips
 * the parser entirely. The cache key is the exact source bytes, the engine
 * build version and the eval flags; JS_ReadObject additionally validates the
 * internal bytecode version, so a blob from an incompatible engine degrades to
 * a recompile rather than misbehaving. Opt in with DYNAJS_BYTECODE_CACHE=1.
 */
#define QBC_MAGIC 0x31434251u  /* 'Q','B','C','1' */

typedef struct QbcHeader {
    uint32_t magic;
    uint32_t eval_flags;   /* compile-time eval flags (module/strict/…) */
    uint32_t strip_flags;  /* JS_SetStripInfo() setting: changes emitted bytecode */
    uint32_t reserved;
    uint64_t cfg_hash;     /* fnv1a64(CONFIG_VERSION): invalidate across builds */
    uint64_t source_len;
    uint64_t source_hash;  /* fnv1a64(source): content addressing */
    uint64_t blob_len;
} QbcHeader;

/* mirrors the runtime JS_SetStripInfo() setting so the cache key tracks it */
static int cache_strip_flags = 0;

static uint64_t fnv1a64(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint64_t h = 0xcbf29ce484222325ULL;
    size_t i;
    for (i = 0; i < len; i++)
        h = (h ^ p[i]) * 0x100000001b3ULL;
    return h;
}

static BOOL bytecode_cache_enabled(void)
{
    const char *e = getenv("DYNAJS_BYTECODE_CACHE");
    return e && e[0] && e[0] != '0';
}

static char *bytecode_cache_path(const char *filename)
{
    size_t n = strlen(filename);
    char *p = malloc(n + 5);
    if (p) {
        memcpy(p, filename, n);
        memcpy(p + n, ".qbc", 5);
    }
    return p;
}

/* On a validated hit, return 1 and set *pobj (owned by the caller); else 0. */
static int bytecode_cache_load(JSContext *ctx, const char *path, int eval_flags,
                               uint64_t src_len, uint64_t src_hash, JSValue *pobj)
{
    FILE *f;
    QbcHeader h;
    uint8_t *blob;
    JSValue obj;
    int ret = 0;

    f = fopen(path, "rb");
    if (!f)
        return 0;
    if (fread(&h, 1, sizeof(h), f) == sizeof(h) &&
        h.magic == QBC_MAGIC &&
        h.eval_flags == (uint32_t)eval_flags &&
        h.strip_flags == (uint32_t)cache_strip_flags &&
        h.cfg_hash == fnv1a64(CONFIG_VERSION, strlen(CONFIG_VERSION)) &&
        h.source_len == src_len &&
        h.source_hash == src_hash &&
        h.blob_len > 0 && h.blob_len <= (uint64_t)INT32_MAX) {
        blob = malloc(h.blob_len);
        if (blob) {
            if (fread(blob, 1, h.blob_len, f) == h.blob_len) {
                obj = JS_ReadObject(ctx, blob, h.blob_len, JS_READ_OBJ_BYTECODE);
                if (JS_IsException(obj)) {
                    /* incompatible/corrupt blob: drop it and recompile */
                    JS_FreeValue(ctx, JS_GetException(ctx));
                } else {
                    *pobj = obj;
                    ret = 1;
                }
            }
            free(blob);
        }
    }
    fclose(f);
    return ret;
}

static void bytecode_cache_store(const char *path, int eval_flags,
                                 uint64_t src_len, uint64_t src_hash,
                                 const uint8_t *blob, size_t blob_len)
{
    FILE *f;
    QbcHeader h;

    f = fopen(path, "wb");
    if (!f)
        return;  /* best-effort: a read-only source dir just means no caching */
    h.magic = QBC_MAGIC;
    h.eval_flags = (uint32_t)eval_flags;
    h.strip_flags = (uint32_t)cache_strip_flags;
    h.reserved = 0;
    h.cfg_hash = fnv1a64(CONFIG_VERSION, strlen(CONFIG_VERSION));
    h.source_len = src_len;
    h.source_hash = src_hash;
    h.blob_len = blob_len;
    if (fwrite(&h, 1, sizeof(h), f) != sizeof(h) ||
        fwrite(blob, 1, blob_len, f) != blob_len) {
        fclose(f);
        remove(path);  /* never leave a truncated cache behind */
        return;
    }
    fclose(f);
}

/* eval_buf plus the bytecode cache: compile-once, load-fast. Falls back to the
   plain eval_buf path when caching is disabled. */
static int eval_buf_cached(JSContext *ctx, const void *buf, size_t buf_len,
                           const char *filename, int eval_flags)
{
    int is_module = (eval_flags & JS_EVAL_TYPE_MASK) == JS_EVAL_TYPE_MODULE;
    char *path;
    uint64_t src_hash;
    JSValue obj = JS_UNDEFINED, val;
    int ret;

    if (!bytecode_cache_enabled())
        return eval_buf(ctx, buf, buf_len, filename, eval_flags);

    path = bytecode_cache_path(filename);
    src_hash = fnv1a64(buf, buf_len);

    if (path &&
        bytecode_cache_load(ctx, path, eval_flags, buf_len, src_hash, &obj)) {
        /* hit: JS_ReadObject gives an uninstantiated program; a module still
           needs its dependencies linked and import.meta set before running. */
        if (is_module) {
            if (JS_ResolveModule(ctx, obj) < 0) {
                JS_FreeValue(ctx, obj);
                free(path);
                js_std_dump_error(ctx);
                return -1;
            }
            js_module_set_import_meta(ctx, obj, TRUE, TRUE);
        }
    } else {
        /* miss: compile to bytecode, persist it, then run. */
        obj = JS_Eval(ctx, buf, buf_len, filename,
                      eval_flags | JS_EVAL_FLAG_COMPILE_ONLY);
        if (JS_IsException(obj)) {
            free(path);
            js_std_dump_error(ctx);
            return -1;
        }
        if (path) {
            size_t blob_len;
            uint8_t *blob = JS_WriteObject(ctx, &blob_len, obj,
                                           JS_WRITE_OBJ_BYTECODE);
            if (blob) {
                bytecode_cache_store(path, eval_flags, buf_len, src_hash,
                                     blob, blob_len);
                js_free(ctx, blob);
            }
        }
        if (is_module)
            js_module_set_import_meta(ctx, obj, TRUE, TRUE);
    }
    free(path);

    val = JS_EvalFunction(ctx, obj);  /* consumes obj */
    if (is_module)
        val = js_std_await(ctx, val);
    if (JS_IsException(val)) {
        js_std_dump_error(ctx);
        ret = -1;
    } else {
        ret = 0;
    }
    JS_FreeValue(ctx, val);
    return ret;
}

static int eval_file(JSContext *ctx, const char *filename, int module, int strict)
{
    dyn_iobuf_t src;
    int ret, eval_flags;
    const uint8_t *buf;
    size_t buf_len;

    /* read source through the shared io core (heap read: SIGBUS-safe, never mmap) */
    if (dyn_io_read_buf(filename, &src, DYN_SLURP_NUL, DYN_MAX_INPUT) < 0) {
        perror(filename);
        exit(1);
    }
    buf = dyn_iobuf_rdata(&src);
    buf_len = dyn_iobuf_rlen(&src);

    if (module < 0) {
        module = (has_suffix(filename, ".mjs") ||
                  JS_DetectModule((const char *)buf, buf_len));
    }
    if (module) {
        eval_flags = JS_EVAL_TYPE_MODULE;
    } else {
        eval_flags = JS_EVAL_TYPE_GLOBAL;
        if (strict)
            eval_flags |= JS_EVAL_FLAG_STRICT;
    }
    ret = eval_buf_cached(ctx, buf, buf_len, filename, eval_flags);
    dyn_iobuf_free(&src);
    return ret;
}

/* also used to initialize the worker context */
static JSContext *JS_NewCustomContext(JSRuntime *rt)
{
    JSContext *ctx;
    ctx = JS_NewContext(rt);
    if (!ctx)
        return NULL;
    /* system modules */
    js_init_module_std(ctx, "std");
    js_init_module_os(ctx, "os");
#ifdef CONFIG_NATIVE_MODULES
    js_nat_init_all(ctx);
#endif
    return ctx;
}

#if defined(__APPLE__)
#define MALLOC_OVERHEAD  0
#else
#define MALLOC_OVERHEAD  8
#endif

struct trace_malloc_data {
    uint8_t *base;
};

static inline unsigned long long js_trace_malloc_ptr_offset(uint8_t *ptr,
                                                struct trace_malloc_data *dp)
{
    return ptr - dp->base;
}

/* default memory allocation functions with memory limitation */
static size_t js_trace_malloc_usable_size(const void *ptr)
{
#if defined(__APPLE__)
    return malloc_size(ptr);
#elif defined(_WIN32)
    return _msize((void *)ptr);
#elif defined(__EMSCRIPTEN__)
    return 0;
#elif defined(__linux__) || defined(__GLIBC__)
    return malloc_usable_size((void *)ptr);
#else
    /* change this to `return 0;` if compilation fails */
    return malloc_usable_size((void *)ptr);
#endif
}

static void
#ifdef _WIN32
/* mingw printf is used */
__attribute__((format(gnu_printf, 2, 3)))
#else
__attribute__((format(printf, 2, 3)))
#endif
    js_trace_malloc_printf(JSMallocState *s, const char *fmt, ...)
{
    va_list ap;
    int c;

    va_start(ap, fmt);
    while ((c = *fmt++) != '\0') {
        if (c == '%') {
            /* only handle %p and %zd */
            if (*fmt == 'p') {
                uint8_t *ptr = va_arg(ap, void *);
                if (ptr == NULL) {
                    printf("NULL");
                } else {
                    printf("H%+06lld.%zd",
                           js_trace_malloc_ptr_offset(ptr, s->opaque),
                           js_trace_malloc_usable_size(ptr));
                }
                fmt++;
                continue;
            }
            if (fmt[0] == 'z' && fmt[1] == 'd') {
                size_t sz = va_arg(ap, size_t);
                printf("%zd", sz);
                fmt += 2;
                continue;
            }
        }
        putc(c, stdout);
    }
    va_end(ap);
}

static void js_trace_malloc_init(struct trace_malloc_data *s)
{
    free(s->base = malloc(8));
}

static void *js_trace_malloc(JSMallocState *s, size_t size)
{
    void *ptr;

    /* Do not allocate zero bytes: behavior is platform dependent */
    assert(size != 0);

    if (unlikely(s->malloc_size + size > s->malloc_limit))
        return NULL;
    ptr = malloc(size);
    js_trace_malloc_printf(s, "A %zd -> %p\n", size, ptr);
    if (ptr) {
        s->malloc_count++;
        s->malloc_size += js_trace_malloc_usable_size(ptr) + MALLOC_OVERHEAD;
    }
    return ptr;
}

static void js_trace_free(JSMallocState *s, void *ptr)
{
    if (!ptr)
        return;

    js_trace_malloc_printf(s, "F %p\n", ptr);
    s->malloc_count--;
    s->malloc_size -= js_trace_malloc_usable_size(ptr) + MALLOC_OVERHEAD;
    free(ptr);
}

static void *js_trace_realloc(JSMallocState *s, void *ptr, size_t size)
{
    size_t old_size;

    if (!ptr) {
        if (size == 0)
            return NULL;
        return js_trace_malloc(s, size);
    }
    old_size = js_trace_malloc_usable_size(ptr);
    if (size == 0) {
        js_trace_malloc_printf(s, "R %zd %p\n", size, ptr);
        s->malloc_count--;
        s->malloc_size -= old_size + MALLOC_OVERHEAD;
        free(ptr);
        return NULL;
    }
    if (s->malloc_size + size - old_size > s->malloc_limit)
        return NULL;

    js_trace_malloc_printf(s, "R %zd %p", size, ptr);

    ptr = realloc(ptr, size);
    js_trace_malloc_printf(s, " -> %p\n", ptr);
    if (ptr) {
        s->malloc_size += js_trace_malloc_usable_size(ptr) - old_size;
    }
    return ptr;
}

static const JSMallocFunctions trace_mf = {
    js_trace_malloc,
    js_trace_free,
    js_trace_realloc,
    js_trace_malloc_usable_size,
};

#define PROG_NAME "dynajs"

void help(void)
{
    printf("DynaJS version " CONFIG_VERSION "\n"
           "usage: " PROG_NAME " [options] [file [args]]\n"
           "-h  --help         list options\n"
           "-e  --eval EXPR    evaluate EXPR\n"
           "-i  --interactive  go to interactive mode\n"
           "-m  --module       load as ES6 module (default=autodetect)\n"
           "    --script       load as ES6 script (default=autodetect)\n"
           "    --strict       force strict mode\n"
           "-I  --include file include an additional file\n"
           "    --std          make 'std' and 'os' available to the loaded script\n"
           "-T  --trace        trace memory allocation\n"
           "-d  --dump         dump the memory usage stats\n"
           "    --no-unhandled-rejection  ignore unhandled promise rejections\n"
           "-s                    strip all the debug info\n"
           "    --strip-source    strip the source code\n"
#ifdef CONFIG_NATIVE_MODULES
           "    --io-threads N    IO worker threads (default max(ncpu,4); 0 runs\n"
           "                      every offload inline, the no-pool control)\n"
#endif
           "-q  --quit         just instantiate the interpreter and quit\n");
    exit(1);
}

int main(int argc, char **argv)
{
    JSRuntime *rt;
    JSContext *ctx;
    struct trace_malloc_data trace_data = { NULL };
    int optind;
    char *expr = NULL;
    int interactive = 0;
    int dump_memory = 0;
    int trace_memory = 0;
    int empty_run = 0;
    int module = -1;
    int strict = 0;
    int load_std = 0;
    int dump_unhandled_promise_rejection = 1;
    char *include_list[32];
    int i, include_count = 0;
    int strip_flags = 0;

    /* cannot use getopt because we want to pass the command line to
       the script */
    optind = 1;
    while (optind < argc && *argv[optind] == '-') {
        char *arg = argv[optind] + 1;
        const char *longopt = "";
        /* a single - is not an option, it also stops argument scanning */
        if (!*arg)
            break;
        optind++;
        if (*arg == '-') {
            longopt = arg + 1;
            arg += strlen(arg);
            /* -- stops argument scanning */
            if (!*longopt)
                break;
        }
        for (; *arg || *longopt; longopt = "") {
            char opt = *arg;
            if (opt)
                arg++;
            if (opt == 'h' || opt == '?' || !strcmp(longopt, "help")) {
                help();
                continue;
            }
            if (opt == 'e' || !strcmp(longopt, "eval")) {
                if (*arg) {
                    expr = arg;
                    break;
                }
                if (optind < argc) {
                    expr = argv[optind++];
                    break;
                }
                fprintf(stderr, "dyna: missing expression for -e\n");
                exit(2);
            }
            if (opt == 'I' || !strcmp(longopt, "include")) {
                if (optind >= argc) {
                    fprintf(stderr, "expecting filename");
                    exit(1);
                }
                if (include_count >= countof(include_list)) {
                    fprintf(stderr, "too many included files");
                    exit(1);
                }
                include_list[include_count++] = argv[optind++];
                continue;
            }
            if (opt == 'i' || !strcmp(longopt, "interactive")) {
                interactive++;
                continue;
            }
            if (opt == 'm' || !strcmp(longopt, "module")) {
                module = 1;
                continue;
            }
            if (!strcmp(longopt, "script")) {
                module = 0;
                continue;
            }
            if (!strcmp(longopt, "strict")) {
                strict = 1;
                continue;
            }
            if (opt == 'd' || !strcmp(longopt, "dump")) {
                dump_memory++;
                continue;
            }
            if (opt == 'T' || !strcmp(longopt, "trace")) {
                trace_memory++;
                continue;
            }
            if (!strcmp(longopt, "std")) {
                load_std = 1;
                continue;
            }
            if (!strcmp(longopt, "no-unhandled-rejection")) {
                dump_unhandled_promise_rejection = 0;
                continue;
            }
            if (opt == 'q' || !strcmp(longopt, "quit")) {
                empty_run++;
                continue;
            }
            if (opt == 's') {
                strip_flags = JS_STRIP_DEBUG;
                continue;
            }
            if (!strcmp(longopt, "strip-source")) {
                strip_flags = JS_STRIP_SOURCE;
                continue;
            }
#ifdef CONFIG_NATIVE_MODULES
            if (!strcmp(longopt, "io-threads")) {
                char *end;
                long v;
                if (optind >= argc) {
                    fprintf(stderr, "dyna: --io-threads expects a count\n");
                    exit(1);
                }
                errno = 0;
                v = strtol(argv[optind], &end, 10);
                if (errno || *end || end == argv[optind] || v < 0 || v > 1024) {
                    fprintf(stderr,
                            "dyna: --io-threads must be 0..1024 (0 = inline, no pool)\n");
                    exit(1);
                }
                optind++;
                /* Set before any pool exists -- see dyn_pool_set_default_threads. */
                dyn_pool_set_default_threads((unsigned)v);
                continue;
            }
#endif
            if (opt) {
                fprintf(stderr, "dyna: unknown option '-%c'\n", opt);
            } else {
                fprintf(stderr, "dyna: unknown option '--%s'\n", longopt);
            }
            help();
        }
    }

    if (trace_memory) {
        js_trace_malloc_init(&trace_data);
        rt = JS_NewRuntime2(&trace_mf, &trace_data);
    } else {
        rt = JS_NewRuntime();
    }
    if (!rt) {
        fprintf(stderr, "dyna: cannot allocate JS runtime\n");
        exit(2);
    }
    JS_SetStripInfo(rt, strip_flags);
    cache_strip_flags = strip_flags;
    js_std_set_worker_new_context_func(JS_NewCustomContext);
    js_std_init_handlers(rt);
    ctx = JS_NewCustomContext(rt);
    if (!ctx) {
        fprintf(stderr, "dyna: cannot allocate JS context\n");
        exit(2);
    }

    /* loader for ES6 modules */
    JS_SetModuleLoaderFunc2(rt, NULL, js_module_loader, js_module_check_attributes, NULL);

    if (dump_unhandled_promise_rejection) {
        JS_SetHostPromiseRejectionTracker(rt, js_std_promise_rejection_tracker,
                                          NULL);
    }

    if (!empty_run) {
        js_std_add_helpers(ctx, argc - optind, argv + optind);

        /* make 'std' and 'os' visible to non module code */
        if (load_std) {
            const char *str = "import * as std from 'std';\n"
                "import * as os from 'os';\n"
                "globalThis.std = std;\n"
                "globalThis.os = os;\n";
            eval_buf(ctx, str, strlen(str), "<input>", JS_EVAL_TYPE_MODULE);
        }

        for(i = 0; i < include_count; i++) {
            if (eval_file(ctx, include_list[i], 0, strict))
                goto fail;
        }

        if (expr) {
            int eval_flags;
            if (module > 0) {
                eval_flags = JS_EVAL_TYPE_MODULE;
            } else {
                eval_flags = JS_EVAL_TYPE_GLOBAL;
                if (strict)
                    eval_flags |= JS_EVAL_FLAG_STRICT;
            }
            if (eval_buf(ctx, expr, strlen(expr), "<cmdline>", eval_flags))
                goto fail;
        } else
        if (optind >= argc) {
            /* interactive mode */
            interactive = 1;
        } else {
            const char *filename;
            filename = argv[optind];
            if (eval_file(ctx, filename, module, strict))
                goto fail;
        }
        if (interactive) {
            JS_SetHostPromiseRejectionTracker(rt, NULL, NULL);
            js_std_eval_binary(ctx, dynajsc_repl, dynajsc_repl_size, 0);
        }
        js_std_loop(ctx);
    }

    if (dump_memory) {
        JSMemoryUsage stats;
        JS_ComputeMemoryUsage(rt, &stats);
        JS_DumpMemoryUsage(stdout, &stats, rt);
    }
    js_std_free_handlers(rt);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);

    if (empty_run && dump_memory) {
        clock_t t[5];
        double best[5];
        int i, j;
        for (i = 0; i < 100; i++) {
            t[0] = clock();
            rt = JS_NewRuntime();
            t[1] = clock();
            ctx = JS_NewContext(rt);
            t[2] = clock();
            JS_FreeContext(ctx);
            t[3] = clock();
            JS_FreeRuntime(rt);
            t[4] = clock();
            for (j = 4; j > 0; j--) {
                double ms = 1000.0 * (t[j] - t[j - 1]) / CLOCKS_PER_SEC;
                if (i == 0 || best[j] > ms)
                    best[j] = ms;
            }
        }
        printf("\nInstantiation times (ms): %.3f = %.3f+%.3f+%.3f+%.3f\n",
               best[1] + best[2] + best[3] + best[4],
               best[1], best[2], best[3], best[4]);
    }
    return 0;
 fail:
    js_std_free_handlers(rt);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return 1;
}
