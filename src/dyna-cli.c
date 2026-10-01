#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <inttypes.h>
#include <string.h>
#include <strings.h>
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
extern int js_std_modules_enabled;
extern uint64_t js_std_exec_deadline_ms;
extern uint64_t js_std_js_memory_limit;
extern void js_std_apply_budgets(JSRuntime* rt);
extern void js_std_arm_exec_deadline(uint64_t timeout_ms);
#ifdef CONFIG_NATIVE_MODULES
#include "dyna-nat.h"
#include "core/dyn-pool.h"
#endif

extern const uint8_t dynajsc_repl[];
extern const uint32_t dynajsc_repl_size;

static void eval_set_import_meta(JSContext* ctx, JSValueConst obj, BOOL is_main)
{
    if (js_module_set_import_meta(ctx, obj, TRUE, is_main) < 0) {
        JSValue exc = JS_GetException(ctx);
        JS_FreeValue(ctx, exc);
    }
}

static int eval_buf(JSContext* ctx, const void* buf, int buf_len,
    const char* filename, int eval_flags)
{
    JSValue val;
    int ret;

    if ((eval_flags & JS_EVAL_TYPE_MASK) == JS_EVAL_TYPE_MODULE) {
        val = JS_Eval(ctx, buf, buf_len, filename,
            eval_flags | JS_EVAL_FLAG_COMPILE_ONLY);
        if (!JS_IsException(val)) {
            eval_set_import_meta(ctx, val, TRUE);
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

#define QBC_MAGIC 0x31434251u

typedef struct QbcHeader {
    uint32_t magic;
    uint32_t eval_flags;
    uint32_t strip_flags;
    uint32_t reserved;
    uint64_t cfg_hash;
    uint64_t source_len;
    uint64_t source_hash;
    uint64_t blob_len;
} QbcHeader;

static int cache_strip_flags = 0;

static uint64_t fnv1a64(const void* data, size_t len)
{
    const uint8_t* p = (const uint8_t*)data;
    uint64_t h = 0xcbf29ce484222325ULL;
    size_t i;
    for (i = 0; i < len; i++)
        h = (h ^ p[i]) * 0x100000001b3ULL;
    return h;
}

static uint64_t fnv1a64_update(uint64_t h, const void* data, size_t len)
{
    const uint8_t* p = (const uint8_t*)data;
    size_t i;
    for (i = 0; i < len; i++)
        h = (h ^ p[i]) * 0x100000001b3ULL;
    return h;
}

static uint64_t dyna_opcode_table_sig(void)
{
    static const struct {
        const char *name, *fmt;
        int size, pop, push;
    } tab[] = {
#define DEF(id, size, n_pop, n_push, f) { #id, #f, size, n_pop, n_push },
#include "dyna-opcode.h"
#undef DEF
#define DEF2(id, size, n_pop, n_push, f) { "ext:" #id, #f, size, n_pop, n_push },
#include "dyna-opcode2.h"
#undef DEF2
    };
    uint64_t h = fnv1a64_update(0xcbf29ce484222325ULL, "optab1", 6);
    char row[160];
    size_t i, n;
    for (i = 0; i < sizeof(tab) / sizeof(tab[0]); i++) {
        n = (size_t)snprintf(row, sizeof(row), "%s|%s|%d|%d|%d",
            tab[i].name, tab[i].fmt, tab[i].size,
            tab[i].pop, tab[i].push);
        h = fnv1a64_update(h, row, n);
    }
    return fnv1a64_update(h, &i, sizeof(i));
}

static uint64_t bytecode_cache_cfg_hash(void)
{
    uint64_t h = fnv1a64(CONFIG_VERSION, strlen(CONFIG_VERSION));
    uint64_t sig = dyna_opcode_table_sig();
    return fnv1a64_update(h, &sig, sizeof(sig));
}

static BOOL bytecode_cache_enabled(void)
{
    const char* e = getenv("DYNAJS_BYTECODE_CACHE");
    return e && e[0] && e[0] != '0';
}

static char* bytecode_cache_path(const char* filename)
{
    size_t n = strlen(filename);
    char* p = malloc(n + 5);
    if (p) {
        memcpy(p, filename, n);
        memcpy(p + n, ".qbc", 5);
    }
    return p;
}

static int bytecode_cache_load(JSContext* ctx, const char* path, int eval_flags,
    uint64_t src_len, uint64_t src_hash, JSValue* pobj)
{
    FILE* f;
    QbcHeader h;
    uint8_t* blob;
    JSValue obj;
    int ret = 0;

    f = fopen(path, "rb");
    if (!f)
        return 0;
    if (fread(&h, 1, sizeof(h), f) == sizeof(h) && h.magic == QBC_MAGIC && h.eval_flags == (uint32_t)eval_flags && h.strip_flags == (uint32_t)cache_strip_flags && h.cfg_hash == bytecode_cache_cfg_hash() && h.source_len == src_len && h.source_hash == src_hash && h.blob_len > 0 && h.blob_len <= (uint64_t)INT32_MAX) {
        blob = malloc(h.blob_len);
        if (blob) {
            if (fread(blob, 1, h.blob_len, f) == h.blob_len) {
                obj = JS_ReadObject(ctx, blob, h.blob_len, JS_READ_OBJ_BYTECODE);
                if (JS_IsException(obj)) {
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

static void bytecode_cache_store(const char* path, int eval_flags,
    uint64_t src_len, uint64_t src_hash,
    const uint8_t* blob, size_t blob_len)
{
    FILE* f;
    QbcHeader h;

    f = fopen(path, "wb");
    if (!f)
        return;
    h.magic = QBC_MAGIC;
    h.eval_flags = (uint32_t)eval_flags;
    h.strip_flags = (uint32_t)cache_strip_flags;
    h.reserved = 0;
    h.cfg_hash = bytecode_cache_cfg_hash();
    h.source_len = src_len;
    h.source_hash = src_hash;
    h.blob_len = blob_len;
    if (fwrite(&h, 1, sizeof(h), f) != sizeof(h) || fwrite(blob, 1, blob_len, f) != blob_len) {
        fclose(f);
        remove(path);
        return;
    }
    fclose(f);
}

static int eval_buf_cached(JSContext* ctx, const void* buf, size_t buf_len,
    const char* filename, int eval_flags)
{
    int is_module = (eval_flags & JS_EVAL_TYPE_MASK) == JS_EVAL_TYPE_MODULE;
    char* path;
    uint64_t src_hash;
    JSValue obj = JS_UNDEFINED, val;
    int ret;

    if (!bytecode_cache_enabled())
        return eval_buf(ctx, buf, buf_len, filename, eval_flags);

    path = bytecode_cache_path(filename);
    src_hash = fnv1a64(buf, buf_len);

    if (path && bytecode_cache_load(ctx, path, eval_flags, buf_len, src_hash, &obj)) {
        if (is_module) {
            if (JS_ResolveModule(ctx, obj) < 0) {
                JS_FreeValue(ctx, obj);
                free(path);
                js_std_dump_error(ctx);
                return -1;
            }
            eval_set_import_meta(ctx, obj, TRUE);
        }
    } else {
        obj = JS_Eval(ctx, buf, buf_len, filename,
            eval_flags | JS_EVAL_FLAG_COMPILE_ONLY);
        if (JS_IsException(obj)) {
            free(path);
            js_std_dump_error(ctx);
            return -1;
        }
        if (path) {
            size_t blob_len;
            uint8_t* blob = JS_WriteObject(ctx, &blob_len, obj,
                JS_WRITE_OBJ_BYTECODE);
            if (blob) {
                bytecode_cache_store(path, eval_flags, buf_len, src_hash,
                    blob, blob_len);
                js_free(ctx, blob);
            }
        }
        if (is_module)
            eval_set_import_meta(ctx, obj, TRUE);
    }
    free(path);

    val = JS_EvalFunction(ctx, obj);
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

static int eval_file(JSContext* ctx, const char* filename, int module, int strict)
{
    dyn_iobuf_t src;
    int ret, eval_flags;
    const uint8_t* buf;
    size_t buf_len;

    if (dyn_io_read_buf(filename, &src, DYN_SLURP_NUL, DYN_MAX_INPUT) < 0) {
        perror(filename);
        exit(1);
    }
    buf = dyn_iobuf_rdata(&src);
    buf_len = dyn_iobuf_rlen(&src);

    if (module < 0) {
        module = (has_suffix(filename, ".mjs") || JS_DetectModule((const char*)buf, buf_len));
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

#ifdef CONFIG_NATIVE_MODULES
static uint64_t cli_native_limit = 0;
#endif

static JSContext* JS_NewCustomContext(JSRuntime* rt)
{
    JSContext* ctx;
    ctx = JS_NewContext(rt);
    if (!ctx)
        return NULL;
    if (js_std_modules_enabled) {
        js_init_module_std(ctx, "std");
        js_init_module_os(ctx, "os");
    }
#ifdef CONFIG_NATIVE_MODULES
    js_nat_init_all(ctx);
#endif
    return ctx;
}

static int cli_no_prototypes = 0;

static int no_prototypes_env(void)
{
    const char* v = getenv("DYNAJS_NO_PROTOTYPES");
    if (!v || !*v)
        return 0;
    return !(strcmp(v, "0") == 0 || strcasecmp(v, "false") == 0 || strcasecmp(v, "no") == 0);
}

#if defined(__APPLE__)
#define MALLOC_OVERHEAD 0
#else
#define MALLOC_OVERHEAD 8
#endif

struct trace_malloc_data {
    uint8_t* base;
};

static inline unsigned long long js_trace_malloc_ptr_offset(uint8_t* ptr,
    struct trace_malloc_data* dp)
{
    return ptr - dp->base;
}

static size_t js_trace_malloc_usable_size(const void* ptr)
{
#if defined(__APPLE__)
    return malloc_size(ptr);
#elif defined(_WIN32)
    return _msize((void*)ptr);
#elif defined(__EMSCRIPTEN__)
    return 0;
#elif defined(__linux__) || defined(__GLIBC__)
    return malloc_usable_size((void*)(uintptr_t)ptr);
#else
    return malloc_usable_size((void*)(uintptr_t)ptr);
#endif
}

static void
#ifdef _WIN32
    __attribute__((format(gnu_printf, 2, 3)))
#else
    __attribute__((format(printf, 2, 3)))
#endif
    js_trace_malloc_printf(JSMallocState* s, const char* fmt, ...)
{
    va_list ap;
    int c;

    va_start(ap, fmt);
    while ((c = *fmt++) != '\0') {
        if (c == '%') {
            if (*fmt == 'p') {
                uint8_t* ptr = va_arg(ap, void*);
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

static void js_trace_malloc_init(struct trace_malloc_data* s)
{
    free(s->base = malloc(8));
}

static void* js_trace_malloc(JSMallocState* s, size_t size)
{
    void* ptr;

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

static void js_trace_free(JSMallocState* s, void* ptr)
{
    if (!ptr)
        return;

    js_trace_malloc_printf(s, "F %p\n", ptr);
    s->malloc_count--;
    s->malloc_size -= js_trace_malloc_usable_size(ptr) + MALLOC_OVERHEAD;
    free(ptr);
}

static void* js_trace_realloc(JSMallocState* s, void* ptr, size_t size)
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

static void help(void)
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
           "    --no-prototypes  do not install the project-added Array/String/\n"
           "                     Number/Date/Function/Iterator prototype extensions\n"
           "                     (env: DYNAJS_NO_PROTOTYPES=1)\n"
           "-s                    strip all the debug info\n"
           "    --strip-source    strip the source code\n"
#ifdef CONFIG_NATIVE_MODULES
           "    --io-threads N    IO worker threads (default max(ncpu,4); 0 runs\n"
           "                      every offload inline, the no-pool control)\n"
#endif
           "-q  --quit         just instantiate the interpreter and quit\n");
    exit(1);
}

int main(int argc, char** argv)
{
    JSRuntime* rt;
    JSContext* ctx;
    struct trace_malloc_data trace_data = { NULL };
    int optind_local;
    char* expr = NULL;
    int interactive = 0;
    int dump_memory = 0;
    int trace_memory = 0;
    int empty_run = 0;
    int module = -1;
    int strict = 0;
    int load_std = 0;
    int dump_unhandled_promise_rejection = 1;
    char* include_list[32];
    int i, include_count = 0;
    int strip_flags = 0;

    optind_local = 1;
    while (optind_local < argc && *argv[optind_local] == '-') {
        char* arg = argv[optind_local] + 1;
        const char* longopt = "";
        if (!*arg)
            break;
        optind_local++;
        if (*arg == '-') {
            longopt = arg + 1;
            arg += strlen(arg);
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
                if (optind_local < argc) {
                    expr = argv[optind_local++];
                    break;
                }
                fprintf(stderr, "dyna: missing expression for -e\n");
                exit(2);
            }
            if (opt == 'I' || !strcmp(longopt, "include")) {
                if (optind_local >= argc) {
                    fprintf(stderr, "expecting filename");
                    exit(1);
                }
                if (include_count >= countof(include_list)) {
                    fprintf(stderr, "too many included files");
                    exit(1);
                }
                include_list[include_count++] = argv[optind_local++];
                continue;
            }
            if (opt == 'i' || !strcmp(longopt, "interactive")) {
                interactive++;
                js_std_modules_enabled = 1;
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
#ifdef CONFIG_NATIVE_MODULES
                dyn_net_set_debug(1);
#endif
                continue;
            }
            if (opt == 'T' || !strcmp(longopt, "trace")) {
                trace_memory++;
                continue;
            }
            if (!strcmp(longopt, "std")) {
                load_std = 1;
                js_std_modules_enabled = 1;
                continue;
            }
            if (!strcmp(longopt, "no-unhandled-rejection")) {
                dump_unhandled_promise_rejection = 0;
                continue;
            }
            if (!strcmp(longopt, "no-prototypes")) {
                cli_no_prototypes = 1;
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
            if (!strcmp(longopt, "timeout-ms")) {
                char* end;
                long v;
                if (optind_local >= argc) {
                    fprintf(stderr, "dyna: --timeout-ms expects milliseconds\n");
                    exit(1);
                }
                errno = 0;
                v = strtol(argv[optind_local], &end, 10);
                if (errno || *end || end == argv[optind_local] || v < 1) {
                    fprintf(stderr,
                        "dyna: --timeout-ms must be a positive integer\n");
                    exit(1);
                }
                optind_local++;
                js_std_arm_exec_deadline((uint64_t)v);
                continue;
            }
            if (!strcmp(longopt, "memory-limit")) {
                char* end;
                long long v;
                if (optind_local >= argc) {
                    fprintf(stderr, "dyna: --memory-limit expects bytes\n");
                    exit(1);
                }
                errno = 0;
                v = strtoll(argv[optind_local], &end, 10);
                if (errno || *end || end == argv[optind_local] || v < 0) {
                    fprintf(stderr,
                        "dyna: --memory-limit must be >= 0 bytes\n");
                    exit(1);
                }
                optind_local++;
                js_std_js_memory_limit = (uint64_t)v;
                continue;
            }
#ifdef CONFIG_NATIVE_MODULES
            if (!strcmp(longopt, "native-memory-limit")) {
                char* end;
                long long v;
                if (optind_local >= argc) {
                    fprintf(stderr,
                        "dyna: --native-memory-limit expects bytes\n");
                    exit(1);
                }
                errno = 0;
                v = strtoll(argv[optind_local], &end, 10);
                if (errno || *end || end == argv[optind_local] || v < 0) {
                    fprintf(stderr,
                        "dyna: --native-memory-limit must be >= 0 bytes\n");
                    exit(1);
                }
                optind_local++;
                cli_native_limit = (uint64_t)v;
                continue;
            }
#endif
#ifdef CONFIG_NATIVE_MODULES
            if (!strcmp(longopt, "io-threads")) {
                char* end;
                long v;
                if (optind_local >= argc) {
                    fprintf(stderr, "dyna: --io-threads expects a count\n");
                    exit(1);
                }
                errno = 0;
                v = strtol(argv[optind_local], &end, 10);
                if (errno || *end || end == argv[optind_local] || v < 0 || v > 1024) {
                    fprintf(stderr,
                        "dyna: --io-threads must be 0..1024 (0 = inline, no pool)\n");
                    exit(1);
                }
                optind_local++;
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
    if (cli_no_prototypes || no_prototypes_env())
        JS_SetNoPrototypeExtensions(1);
    js_std_apply_budgets(rt);
#ifdef CONFIG_NATIVE_MODULES
    if (cli_native_limit)
        dyn_nat_set_limit(cli_native_limit);
#endif
    if (!empty_run && optind_local >= argc && !expr)
        js_std_modules_enabled = 1;
    js_std_set_worker_new_context_func(JS_NewCustomContext);
    js_std_init_handlers(rt);
    ctx = JS_NewCustomContext(rt);
    if (!ctx) {
        fprintf(stderr, "dyna: cannot allocate JS context\n");
        exit(2);
    }

    JS_SetModuleLoaderFunc2(rt, NULL, js_module_loader, js_module_check_attributes, NULL);

    if (dump_unhandled_promise_rejection) {
        JS_SetHostPromiseRejectionTracker(rt, js_std_promise_rejection_tracker,
            NULL);
    }

    if (!empty_run) {
        js_std_add_helpers(ctx, argc - optind_local, argv + optind_local);

        if (load_std) {
            const char* str = "import * as std from 'std';\n"
                              "import * as os from 'os';\n"
                              "globalThis.std = std;\n"
                              "globalThis.os = os;\n";
            eval_buf(ctx, str, strlen(str), "<input>", JS_EVAL_TYPE_MODULE);
        }

        for (i = 0; i < include_count; i++) {
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
        } else if (optind_local >= argc) {
            interactive = 1;
            js_std_modules_enabled = 1;
        } else {
            const char* filename;
            filename = argv[optind_local];
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
#ifdef CONFIG_NATIVE_MODULES
        fprintf(stdout, "net swallowed handler throws: %" PRIu64 "\n",
            dyn_net_handler_throw_count());
#endif
    }
    js_std_free_handlers(rt);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);

    if (empty_run && dump_memory) {
        clock_t t[5];
        double best[5];
        int ri, j;
        for (ri = 0; ri < 100; ri++) {
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
                if (ri == 0 || best[j] > ms)
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
