#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <inttypes.h>
#include <string.h>
#include <limits.h>
#include <strings.h>
#include <assert.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
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
extern void js_std_apply_budgets(JSRuntime* rt);
extern void js_std_arm_exec_deadline(uint64_t timeout_ms);
#ifdef CONFIG_NATIVE_MODULES
#include "dyna-nat.h"
#include "core/dyn-pool.h"
#endif

static void eval_set_import_meta(JSContext* ctx, JSValueConst obj, BOOL is_main)
{
    if (js_module_set_import_meta(ctx, obj, TRUE, is_main) < 0) {
        JSValue exc = JS_GetException(ctx);
        JS_FreeValue(ctx, exc);
    }
}

static int script_has_await(const char* buf, size_t len)
{
    size_t i;
    for (i = 0; i + 5 <= len; i++) {
        if (buf[i] == 'a' && memcmp(buf + i, "await", 5) == 0)
            return 1;
    }
    return 0;
}

static int script_eval_flags(JSContext* ctx, const char* buf, size_t len,
    const char* filename, int eval_flags)
{
    JSValue v;

    if ((eval_flags & JS_EVAL_TYPE_MASK) != JS_EVAL_TYPE_GLOBAL || !script_has_await(buf, len))
        return eval_flags;
    v = JS_Eval(ctx, buf, len, filename, eval_flags | JS_EVAL_FLAG_COMPILE_ONLY);
    if (!JS_IsException(v)) {
        JS_FreeValue(ctx, v);
        return eval_flags;
    }
    JS_FreeValue(ctx, JS_GetException(ctx));
    v = JS_Eval(ctx, buf, len, filename,
        eval_flags | JS_EVAL_FLAG_ASYNC | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(v)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return eval_flags;
    }
    JS_FreeValue(ctx, v);
    return eval_flags | JS_EVAL_FLAG_ASYNC;
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
        if (eval_flags & JS_EVAL_FLAG_ASYNC)
            val = js_std_await(ctx, val);
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

#define QBC_MAGIC 0x32434251u

typedef struct QbcHeader {
    uint32_t magic;
    uint32_t eval_flags;
    uint32_t reserved;
    uint64_t cfg_hash;
    uint64_t source_len;
    uint64_t source_hash;
    uint64_t blob_len;
} QbcHeader;


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
    struct stat st;
    QbcHeader h;
    uint8_t* blob;
    JSValue obj;
    int ret = 0;

    f = fopen(path, "rb");
    if (!f)
        return 0;
    if (fstat(fileno(f), &st) < 0 || !S_ISREG(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & (S_IWGRP | S_IWOTH))) {
        fclose(f);
        return 0;
    }
    if (fread(&h, 1, sizeof(h), f) == sizeof(h) && h.magic == QBC_MAGIC && h.eval_flags == (uint32_t)eval_flags && h.cfg_hash == bytecode_cache_cfg_hash() && h.source_len == src_len && h.source_hash == src_hash && h.blob_len > 0 && h.blob_len <= (uint64_t)INT32_MAX) {
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
    int fd;
    FILE* f;
    QbcHeader h;

    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0)
        return;
    f = fdopen(fd, "wb");
    if (!f) {
        close(fd);
        return;
    }
    h.magic = QBC_MAGIC;
    h.eval_flags = (uint32_t)eval_flags;
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
    if (is_module || (eval_flags & JS_EVAL_FLAG_ASYNC))
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

static int eval_file(JSContext* ctx, const char* filename, int module)
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
    }
    eval_flags = script_eval_flags(ctx, (const char*)buf, buf_len, filename, eval_flags);
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


#define PROG_NAME "dynajs"

static void help(int code)
{
    printf("DynaJS version " CONFIG_VERSION "\n"
           "usage: " PROG_NAME " [options] [file [args]]\n"
           "-h  --help         list options\n"
           "-v  --version      print the version and exit\n"
           "-e  --eval EXPR    evaluate EXPR\n"
           "-i  --interactive  go to interactive mode\n"
           "-m  --module       load as ES6 module (default=autodetect)\n"
           "    --std          make 'std' and 'os' available to the loaded script\n"
           "    --no-unhandled-rejection  ignore unhandled promise rejections\n"
           "    --no-prototypes  do not install the project-added Array/String/\n"
           "                     Number/Date/Function/Iterator prototype extensions\n"
           "                     (env: DYNAJS_NO_PROTOTYPES=1)\n"
           "    --timeout-ms N    stop the script after N ms (a native call still\n"
           "                      running after a grace period exits with 113)\n"
#ifdef CONFIG_NATIVE_MODULES
           "    --native-memory-limit N  cap native module memory at N bytes,\n"
           "                      process-wide; a script may lower it, never raise it\n"
           "    --io-threads N    IO worker threads, 0..256 (default and ceiling\n"
           "                      the usable cpus; 0 runs every offload inline)\n"
           "                      also caps the fork-join threads a native\n"
           "                      starts inside one call; 0 = serial\n"
#endif
        );
    exit(code);
}

static int cli_parse_bytes(const char* opt, const char* s, uint64_t* out)
{
    char* end;
    unsigned long long v;
    if (s[0] == '-') {
        fprintf(stderr, "dyna: --%s must be >= 0 bytes, got '%s'\n", opt, s);
        return -1;
    }
    if (s[0] < '0' || s[0] > '9') {
        fprintf(stderr, "dyna: --%s expects an integer number of bytes (no sign, exponent or suffix), got '%s'\n", opt, s);
        return -1;
    }
    errno = 0;
    v = strtoull(s, &end, 10);
    if (*end) {
        fprintf(stderr, "dyna: --%s expects an integer number of bytes (no sign, exponent or suffix), got '%s'\n", opt, s);
        return -1;
    }
    if (errno == ERANGE) {
        fprintf(stderr, "dyna: --%s is out of range (max %llu), got '%s'\n", opt, ULLONG_MAX, s);
        return -1;
    }
    *out = (uint64_t)v;
    return 0;
}


int main(int argc, char** argv)
{
    JSRuntime* rt;
    JSContext* ctx;
    int optind_local;
    char* expr = NULL;
    int interactive = 0;
    int module = -1;
    int load_std = 0;
    int dump_unhandled_promise_rejection = 1;

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
                help(0);
                continue;
            }
            if (opt == 'v' || !strcmp(longopt, "version")) {
                printf("DynaJS version " CONFIG_VERSION "\n");
                exit(0);
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
            if (opt == 'i' || !strcmp(longopt, "interactive")) {
                interactive++;
                js_std_modules_enabled = 1;
                continue;
            }
            if (opt == 'm' || !strcmp(longopt, "module")) {
                module = 1;
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
#ifdef CONFIG_NATIVE_MODULES
            if (!strcmp(longopt, "native-memory-limit")) {
                uint64_t v;
                if (optind_local >= argc) {
                    fprintf(stderr,
                        "dyna: --native-memory-limit expects bytes\n");
                    exit(1);
                }
                if (cli_parse_bytes("native-memory-limit", argv[optind_local], &v) < 0)
                    exit(1);
                optind_local++;
                cli_native_limit = v;
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
                if (errno || *end || end == argv[optind_local] || v < 0 || v > 256) {
                    fprintf(stderr,
                        "dyna: --io-threads must be 0..256 (0 = inline, no pool)\n");
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
            help(1);
        }
    }

    rt = JS_NewRuntime();
    if (!rt) {
        fprintf(stderr, "dyna: cannot allocate JS runtime\n");
        exit(2);
    }
    if (cli_no_prototypes || no_prototypes_env())
        JS_SetNoPrototypeExtensions(1);
    js_std_apply_budgets(rt);
#ifdef CONFIG_NATIVE_MODULES
    if (cli_native_limit)
        dyn_nat_set_limit(cli_native_limit);
#endif
    if (optind_local >= argc && !expr)
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

    {
        js_std_add_helpers(ctx, argc - optind_local, argv + optind_local);

        if (load_std) {
            const char* str = "import * as std from 'std';\n"
                              "import * as os from 'os';\n"
                              "globalThis.std = std;\n"
                              "globalThis.os = os;\n";
            eval_buf(ctx, str, strlen(str), "<input>", JS_EVAL_TYPE_MODULE);
        }

        if (expr) {
            int eval_flags;
            if (module > 0) {
                eval_flags = JS_EVAL_TYPE_MODULE;
            } else {
                eval_flags = JS_EVAL_TYPE_GLOBAL;
            }
            eval_flags = script_eval_flags(ctx, expr, strlen(expr), "<cmdline>", eval_flags);
            if (eval_buf(ctx, expr, strlen(expr), "<cmdline>", eval_flags))
                goto fail;
        } else if (optind_local >= argc) {
            interactive = 1;
            js_std_modules_enabled = 1;
        } else {
            const char* filename;
            filename = argv[optind_local];
            if (eval_file(ctx, filename, module))
                goto fail;
        }
        if (interactive) {
            JS_SetHostPromiseRejectionTracker(rt, NULL, NULL);
            if (dyna_repl_eval(ctx) < 0) {
                js_std_dump_error(ctx);
                goto fail;
            }
        }
        if (js_std_loop(ctx))
            goto fail;
    }

    js_std_free_handlers(rt);
    JS_FreeContext(ctx);
    JS_SetExitTeardown(rt, 1);
    JS_FreeRuntime(rt);

    return 0;
fail:
    js_std_free_handlers(rt);
    JS_FreeContext(ctx);
    JS_SetExitTeardown(rt, TRUE);
    JS_FreeRuntime(rt);
    return 1;
}
