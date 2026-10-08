#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <inttypes.h>
#include <string.h>
#include <assert.h>
#include <math.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/time.h>
#include <time.h>
#include <signal.h>
#include <limits.h>
#include <sys/stat.h>
#include <dirent.h>
#if defined(_WIN32)
#include <windows.h>
#include <conio.h>
#include <utime.h>
#else
#include <dlfcn.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <poll.h>

#if defined(__FreeBSD__)
extern char** environ;
#endif

#if defined(__APPLE__) || defined(__FreeBSD__)
typedef sig_t sighandler_t;
#endif

#if defined(__APPLE__)
#if !defined(environ)
#include <crt_externs.h>
#define environ (*_NSGetEnviron())
#endif
#endif

#endif

#define USE_WORKER

#ifdef USE_WORKER
#include <pthread.h>
#include <stdatomic.h>
#endif

#include "cutils.h"
#include "list.h"
#include "dyna-libc.h"
#include "dyna-simd-kernels.h"
#include "dyna-io.h"

#if !defined(PATH_MAX)
#define PATH_MAX 4096
#endif

#define DYN_EXEC_TIMEOUT_GRACE_MS 200
#define DYN_PRINTF_FIELD_MAX 1000000

typedef struct {
    struct list_head link;
    int fd;
    int poll_fd_index;
    JSValue rw_func[2];
} JSOSRWHandler;

typedef struct {
    struct list_head link;
    int sig_num;
    JSValue func;
} JSOSSignalHandler;

typedef struct JSOSTimer {
    struct JSOSTimer* id_next;
    uint64_t seq;
    uint32_t heap_idx;
    int timer_id;
    int repeat;
    int64_t delay;
    int64_t timeout;
    JSValue func;
    int argc;
    JSValue* args;
} JSOSTimer;

typedef struct {
    struct list_head link;
    uint8_t* data;
    size_t data_len;
    uint8_t** sab_tab;
    size_t sab_tab_len;
} JSWorkerMessage;

typedef struct JSWaker {
#ifdef _WIN32
    HANDLE handle;
#else
    int read_fd;
    int write_fd;
#endif
} JSWaker;

typedef struct {
    int ref_count;
#ifdef USE_WORKER
    pthread_mutex_t mutex;
#endif
    struct list_head msg_queue;
    size_t queued_bytes;
    JSWaker waker;
} JSWorkerMessagePipe;

typedef struct {
    struct list_head link;
    JSWorkerMessagePipe* recv_pipe;
    JSValue on_message_func;
    int poll_fd_index;
} JSWorkerMessageHandler;

typedef struct {
    struct list_head link;
    JSValue obj;
} JSWorkerObjRef;

typedef struct {
    struct list_head link;
    JSValue promise;
    JSValue reason;
} JSRejectedPromiseEntry;

extern uint64_t js_std_exec_deadline_ms;
extern uint64_t js_std_js_memory_limit;
void js_std_apply_budgets(JSRuntime* rt);
void js_std_arm_exec_deadline(uint64_t timeout_ms);
int js_std_interrupt_handler(JSRuntime* rt, void* opaque);
static int js_std_deadline_remaining_ms(void);
static int js_std_deadline_expired(void);

typedef struct JSThreadState {
    struct list_head os_rw_handlers;
    struct list_head os_signal_handlers;
    struct JSOSTimer** timer_heap;
    struct JSOSTimer** timer_ids;
    uint64_t timer_seq;
    uint32_t timer_heap_cap;
    uint32_t timer_ids_cap;
    int n_timers;
    struct list_head port_list;
    struct list_head worker_obj_list;
    struct list_head rejected_promise_list;
    int eval_script_recurse;
    int next_timer_id;
    JSWorkerMessagePipe *recv_pipe, *send_pipe;
#if !defined(_WIN32)
    struct pollfd* poll_fds;
    int poll_fds_size;
#endif
    int io_reactor_fd;
    void (*io_reactor_drain)(void* udata);
    int (*io_reactor_wait)(void* udata, int timeout_ms);
    int64_t last_io_poll_ms;
    void* io_reactor_udata;
} JSThreadState;

static JSOSRWHandler* find_rh(JSThreadState* ts, int fd);
static void free_rw_handler(JSRuntime* rt, JSOSRWHandler* rh);
static void js_os_clear_handler(JSContext* ctx, int fd);

static void js_release_worker_refs(JSRuntime* rt, JSThreadState* ts);
static BOOL js_ports_have_pending(JSThreadState* ts);

static _Atomic(int) js_live_worker_threads;

#define WORKER_EXIT_POLL_MS 20

static _Atomic uint64_t os_pending_signals;
typedef int (*JSOSPollFunc)(JSContext* ctx);
static _Atomic(JSOSPollFunc) os_poll_func;

static void* js_std_dbuf_realloc(void* opaque, void* ptr, size_t size)
{
    return js_realloc_rt((JSRuntime*)opaque, ptr, size);
}

static void js_std_dbuf_init(JSContext* ctx, DynBuf* s)
{
    dbuf_init2(s, JS_GetRuntime(ctx), js_std_dbuf_realloc);
}

static BOOL my_isdigit(int c)
{
    return (c >= '0' && c <= '9');
}

static JSValue js_printf_internal(JSContext* ctx,
    int argc, JSValueConst* argv, FILE* fp)
{
    char fmtbuf[32];
    uint8_t cbuf[UTF8_CHAR_LEN_MAX + 1];
    JSValue res;
    DynBuf dbuf;
    const char* fmt_str = NULL;
    const uint8_t *fmt, *fmt_end;
    const uint8_t* p;
    char* q;
    int i, c, len, mod;
    size_t fmt_len;
    int32_t int32_arg;
    int64_t int64_arg;
    double double_arg;
    const char* string_arg;
    int (*dbuf_printf_fun)(DynBuf* s, const char* fmt, ...) = (void*)dbuf_printf;

    js_std_dbuf_init(ctx, &dbuf);

    if (argc > 0) {
        fmt_str = JS_ToCStringLen(ctx, &fmt_len, argv[0]);
        if (!fmt_str)
            goto fail;

        i = 1;
        fmt = (const uint8_t*)fmt_str;
        fmt_end = fmt + fmt_len;
        while (fmt < fmt_end) {
            for (p = fmt; fmt < fmt_end && *fmt != '%'; fmt++)
                continue;
            dbuf_put(&dbuf, p, fmt - p);
            if (fmt >= fmt_end)
                break;
            q = fmtbuf;
            *q++ = *fmt++;

            for (;;) {
                c = *fmt;
                if (c == '0' || c == '#' || c == '+' || c == '-' || c == ' ' || c == '\'') {
                    if (q >= fmtbuf + sizeof(fmtbuf) - 1)
                        goto invalid;
                    *q++ = c;
                    fmt++;
                } else {
                    break;
                }
            }
            if (*fmt == '*') {
                if (i >= argc)
                    goto missing;
                if (JS_ToInt32(ctx, &int32_arg, argv[i++]))
                    goto fail;
                if (int32_arg > DYN_PRINTF_FIELD_MAX || int32_arg < -DYN_PRINTF_FIELD_MAX)
                    goto field;
                q += snprintf(q, fmtbuf + sizeof(fmtbuf) - q, "%d", int32_arg);
                if (unlikely(q >= fmtbuf + sizeof(fmtbuf) - 1))
                    goto invalid;
                fmt++;
            } else {
                unsigned long long fw = 0;
                while (my_isdigit(*fmt)) {
                    if (q >= fmtbuf + sizeof(fmtbuf) - 1)
                        goto invalid;
                    fw = fw * 10 + (unsigned)(*fmt - '0');
                    if (fw > DYN_PRINTF_FIELD_MAX)
                        goto field;
                    *q++ = *fmt++;
                }
            }
            if (*fmt == '.') {
                if (q >= fmtbuf + sizeof(fmtbuf) - 1)
                    goto invalid;
                *q++ = *fmt++;
                if (*fmt == '*') {
                    if (i >= argc)
                        goto missing;
                    if (JS_ToInt32(ctx, &int32_arg, argv[i++]))
                        goto fail;
                    if (int32_arg > DYN_PRINTF_FIELD_MAX || int32_arg < -DYN_PRINTF_FIELD_MAX)
                        goto field;
                    q += snprintf(q, fmtbuf + sizeof(fmtbuf) - q, "%d", int32_arg);
                    if (unlikely(q >= fmtbuf + sizeof(fmtbuf) - 1))
                        goto invalid;
                    fmt++;
                } else {
                    unsigned long long prec = 0;
                    while (my_isdigit(*fmt)) {
                        if (q >= fmtbuf + sizeof(fmtbuf) - 1)
                            goto invalid;
                        prec = prec * 10 + (unsigned)(*fmt - '0');
                        if (prec > DYN_PRINTF_FIELD_MAX)
                            goto field;
                        *q++ = *fmt++;
                    }
                }
            }

            mod = ' ';
            if (*fmt == 'l') {
                mod = *fmt++;
            }

            c = *fmt++;
            if (q >= fmtbuf + sizeof(fmtbuf) - 1)
                goto invalid;
            *q++ = c;
            *q = '\0';

            switch (c) {
            case 'c':
                if (mod == 'l')
                    goto invalid;
                if (i >= argc)
                    goto missing;
                if (JS_IsString(argv[i])) {
                    string_arg = JS_ToCString(ctx, argv[i++]);
                    if (!string_arg)
                        goto fail;
                    int32_arg = unicode_from_utf8((const uint8_t*)string_arg, UTF8_CHAR_LEN_MAX, &p);
                    JS_FreeCString(ctx, string_arg);
                } else {
                    if (JS_ToInt32(ctx, &int32_arg, argv[i++]))
                        goto fail;
                }
                if ((unsigned)int32_arg > 0x10FFFF)
                    int32_arg = 0xFFFD;
                len = unicode_to_utf8(cbuf, int32_arg);
                if (q - fmtbuf > 2) {
                    cbuf[len] = '\0';
                    q[-1] = 's';
                    if (dbuf_printf_fun(&dbuf, fmtbuf, (char*)cbuf) < 0)
                        goto fail;
                } else {
                    dbuf_put(&dbuf, cbuf, len);
                }
                break;

            case 'd':
            case 'i':
            case 'o':
            case 'u':
            case 'x':
            case 'X':
                if (i >= argc)
                    goto missing;
                if (JS_ToInt64Ext(ctx, &int64_arg, argv[i++]))
                    goto fail;
                if (mod == 'l') {
#if defined(_WIN32)
                    if (q >= fmtbuf + sizeof(fmtbuf) - 3)
                        goto invalid;
                    q[2] = q[-1];
                    q[-1] = 'I';
                    q[0] = '6';
                    q[1] = '4';
                    q[3] = '\0';
                    dbuf_printf_fun(&dbuf, fmtbuf, (int64_t)int64_arg);
#else
                    if (q >= fmtbuf + sizeof(fmtbuf) - 2)
                        goto invalid;
                    q[1] = q[-1];
                    q[-1] = q[0] = 'l';
                    q[2] = '\0';
                    dbuf_printf_fun(&dbuf, fmtbuf, (long long)int64_arg);
#endif
                } else {
                    dbuf_printf_fun(&dbuf, fmtbuf, (int)int64_arg);
                }
                break;

            case 's':
                if (mod == 'l')
                    goto invalid;
                if (i >= argc)
                    goto missing;
                string_arg = JS_ToCString(ctx, argv[i++]);
                if (!string_arg)
                    goto fail;
                dbuf_printf_fun(&dbuf, fmtbuf, string_arg);
                JS_FreeCString(ctx, string_arg);
                break;

            case 'e':
            case 'f':
            case 'g':
            case 'a':
            case 'E':
            case 'F':
            case 'G':
            case 'A':
                if (i >= argc)
                    goto missing;
                if (JS_ToFloat64(ctx, &double_arg, argv[i++]))
                    goto fail;
                dbuf_printf_fun(&dbuf, fmtbuf, double_arg);
                break;

            case '%':
                dbuf_putc(&dbuf, '%');
                break;

            default:
            invalid:
                JS_ThrowTypeError(ctx, "std: invalid conversion specifier in format string");
                goto fail;
            missing:
                JS_ThrowReferenceError(ctx, "std: missing argument for conversion specifier");
                goto fail;
            field:
                JS_ThrowRangeError(ctx, "std: width/precision exceeds 1000000");
                goto fail;
            }
        }
        JS_FreeCString(ctx, fmt_str);
    }
    if (dbuf.error) {
        res = JS_ThrowOutOfMemory(ctx);
    } else {
        if (fp) {
            len = fwrite(dbuf.buf, 1, dbuf.size, fp);
            res = JS_NewInt32(ctx, len);
        } else {
            res = JS_NewStringLen(ctx, (char*)dbuf.buf, dbuf.size);
        }
    }
    dbuf_free(&dbuf);
    return res;

fail:
    JS_FreeCString(ctx, fmt_str);
    dbuf_free(&dbuf);
    return JS_EXCEPTION;
}

uint8_t* js_load_file(JSContext* ctx, size_t* pbuf_len, const char* filename)
{
    FILE* f;
    uint8_t* buf;
    size_t buf_len;
    long lret;

    f = fopen(filename, "rb");
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) < 0)
        goto fail;
    lret = ftell(f);
    if (lret < 0)
        goto fail;
    if (lret == LONG_MAX) {
        errno = EISDIR;
        goto fail;
    }
    buf_len = lret;
    if (fseek(f, 0, SEEK_SET) < 0)
        goto fail;
    if (ctx)
        buf = js_malloc(ctx, buf_len + 1);
    else
        buf = malloc(buf_len + 1);
    if (!buf)
        goto fail;
    if (fread(buf, 1, buf_len, f) != buf_len) {
        errno = EIO;
        if (ctx)
            js_free(ctx, buf);
        else
            free(buf);
    fail:
        fclose(f);
        return NULL;
    }
    buf[buf_len] = '\0';
    fclose(f);
    *pbuf_len = buf_len;
    return buf;
}

static JSValue js_loadScript(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_iobuf_t src;
    const char* filename;
    JSValue ret;

    filename = JS_ToCString(ctx, argv[0]);
    if (!filename)
        return JS_EXCEPTION;
    if (dyn_io_read_buf(filename, &src, DYN_SLURP_NUL, DYN_MAX_INPUT) < 0) {
        JS_ThrowReferenceError(ctx, "std.loadScript: could not load '%s'", filename);
        JS_FreeCString(ctx, filename);
        return JS_EXCEPTION;
    }
    ret = JS_Eval(ctx, (char*)dyn_iobuf_rdata(&src), dyn_iobuf_rlen(&src),
        filename, JS_EVAL_TYPE_GLOBAL);
    dyn_iobuf_free(&src);
    JS_FreeCString(ctx, filename);
    return ret;
}

static JSValue js_std_loadFile(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_iobuf_t src;
    const char* filename;
    JSValue ret;

    filename = JS_ToCString(ctx, argv[0]);
    if (!filename)
        return JS_EXCEPTION;
    if (dyn_io_read_buf(filename, &src, 0, DYN_MAX_INPUT) < 0) {
        JS_FreeCString(ctx, filename);
        return JS_NULL;
    }
    JS_FreeCString(ctx, filename);
    ret = JS_NewStringLen(ctx, (char*)dyn_iobuf_rdata(&src),
        dyn_iobuf_rlen(&src));
    dyn_iobuf_free(&src);
    return ret;
}

typedef JSModuleDef*(JSInitModuleFunc)(JSContext * ctx,
    const char* module_name);

#if defined(_WIN32)
static JSModuleDef* js_module_loader_so(JSContext* ctx,
    const char* module_name)
{
    JS_ThrowReferenceError(ctx, "shared library modules are not supported yet");
    return NULL;
}
#else
static JSModuleDef* js_module_loader_so(JSContext* ctx,
    const char* module_name)
{
    JSModuleDef* m;
    void* hd;
    JSInitModuleFunc* init;
    char* filename;

    if (module_name[0] == '.' && module_name[1] == '/')
        module_name += 2;
    if (module_name[0] == '\0' || module_name[0] == '/' || !strncmp(module_name, "..", 2) || strstr(module_name, "/..") || strchr(module_name, '\\') || strchr(module_name, ':')) {
        JS_ThrowReferenceError(ctx, "invalid shared library module name '%s'",
            module_name);
        return NULL;
    }

    if (!strchr(module_name, '/')) {
        size_t mn_len = strlen(module_name);
        filename = js_malloc(ctx, mn_len + 2 + 1);
        if (!filename)
            return NULL;
        memcpy(filename, "./", 2);
        memcpy(filename + 2, module_name, mn_len + 1);
    } else {
        filename = DYN_UNCONST(module_name);
    }

    hd = dlopen(filename, RTLD_NOW | RTLD_LOCAL);
    if (filename != module_name)
        js_free(ctx, filename);
    if (!hd) {
        JS_ThrowReferenceError(ctx, "could not load module filename '%s' as shared library",
            module_name);
        goto fail;
    }

    init = dlsym(hd, "js_init_module");
    if (!init) {
        JS_ThrowReferenceError(ctx, "could not load module filename '%s': js_init_module not found",
            module_name);
        goto fail;
    }

    m = init(ctx, module_name);
    if (!m) {
        JS_ThrowReferenceError(ctx, "could not load module filename '%s': initialization error",
            module_name);
    fail:
        if (hd)
            dlclose(hd);
        return NULL;
    }
    return m;
}
#endif

int js_module_set_import_meta(JSContext* ctx, JSValueConst func_val,
    JS_BOOL use_realpath, JS_BOOL is_main)
{
    JSModuleDef* m;
    char buf[PATH_MAX + 16];
    JSValue meta_obj;
    JSAtom module_name_atom;
    const char* module_name;

    assert(JS_VALUE_GET_TAG(func_val) == JS_TAG_MODULE);
    m = JS_VALUE_GET_PTR(func_val);

    module_name_atom = JS_GetModuleName(ctx, m);
    module_name = JS_AtomToCString(ctx, module_name_atom);
    JS_FreeAtom(ctx, module_name_atom);
    if (!module_name)
        return -1;
    if (!strchr(module_name, ':')) {
        memcpy(buf, "file://", 8);
#if !defined(_WIN32)
        if (use_realpath) {
            char* res = realpath(module_name, buf + strlen(buf));
            if (!res) {
                JS_ThrowTypeError(ctx, "realpath failure");
                JS_FreeCString(ctx, module_name);
                return -1;
            }
        } else
#endif
        {
            pstrcat(buf, sizeof(buf), module_name);
        }
    } else {
        pstrcpy(buf, sizeof(buf), module_name);
    }
    JS_FreeCString(ctx, module_name);

    meta_obj = JS_GetImportMeta(ctx, m);
    if (JS_IsException(meta_obj))
        return -1;
    JS_DefinePropertyValueStr(ctx, meta_obj, "url",
        JS_NewString(ctx, buf),
        JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, meta_obj, "main",
        JS_NewBool(ctx, is_main),
        JS_PROP_C_W_E);
    JS_FreeValue(ctx, meta_obj);
    return 0;
}

static int json_module_init(JSContext* ctx, JSModuleDef* m)
{
    JSValue val;
    val = JS_GetModulePrivateValue(ctx, m);
    JS_SetModuleExport(ctx, m, "default", val);
    return 0;
}

static JSModuleDef* create_json_module(JSContext* ctx, const char* module_name, JSValue val)
{
    JSModuleDef* m;
    m = JS_NewCModule(ctx, module_name, json_module_init);
    if (!m) {
        JS_FreeValue(ctx, val);
        return NULL;
    }
    JS_AddModuleExport(ctx, m, "default");
    JS_SetModulePrivateValue(ctx, m, val);
    return m;
}

int js_module_check_attributes(JSContext* ctx, void* opaque,
    JSValueConst attributes)
{
    JSPropertyEnum* tab;
    uint32_t i, len;
    int ret;
    const char* cstr;
    size_t cstr_len;

    if (JS_GetOwnPropertyNames(ctx, &tab, &len, attributes, JS_GPN_ENUM_ONLY | JS_GPN_STRING_MASK))
        return -1;
    ret = 0;
    for (i = 0; i < len; i++) {
        cstr = JS_AtomToCStringLen(ctx, &cstr_len, tab[i].atom);
        if (!cstr) {
            ret = -1;
            break;
        }
        if (!(cstr_len == 4 && !memcmp(cstr, "type", cstr_len))) {
            JS_ThrowTypeError(ctx, "import attribute '%s' is not supported", cstr);
            ret = -1;
        }
        JS_FreeCString(ctx, cstr);
        if (ret)
            break;
    }
    JS_FreePropertyEnum(ctx, tab, len);
    return ret;
}

int js_module_test_json(JSContext* ctx, JSValueConst attributes)
{
    JSValue str;
    const char* cstr;
    size_t len;
    BOOL res;

    if (JS_IsUndefined(attributes))
        return FALSE;
    str = JS_GetPropertyStr(ctx, attributes, "type");
    if (!JS_IsString(str))
        return FALSE;
    cstr = JS_ToCStringLen(ctx, &len, str);
    JS_FreeValue(ctx, str);
    if (!cstr)
        return FALSE;
    if (len == 4 && !memcmp(cstr, "json", len)) {
        res = 1;
    } else if (len == 5 && !memcmp(cstr, "json5", len)) {
        res = 2;
    } else {
        res = 0;
    }
    JS_FreeCString(ctx, cstr);
    return res;
}

JSModuleDef* js_module_loader(JSContext* ctx,
    const char* module_name, void* opaque,
    JSValueConst attributes)
{
    JSModuleDef* m;
    int res;

    if (has_suffix(module_name, ".so")) {
        m = js_module_loader_so(ctx, module_name);
    } else {
        dyn_iobuf_t src;

        if (dyn_io_read_buf(module_name, &src, DYN_SLURP_NUL, DYN_MAX_INPUT) < 0) {
            JS_ThrowReferenceError(ctx, "could not load module filename '%s'",
                module_name);
            return NULL;
        }
        res = js_module_test_json(ctx, attributes);
        if (has_suffix(module_name, ".json") || res > 0) {
            JSValue val;
            int flags;
            if (res == 2)
                flags = JS_PARSE_JSON_EXT;
            else
                flags = 0;
            val = JS_ParseJSON2(ctx, (char*)dyn_iobuf_rdata(&src),
                dyn_iobuf_rlen(&src), module_name, flags);
            dyn_iobuf_free(&src);
            if (JS_IsException(val))
                return NULL;
            m = create_json_module(ctx, module_name, val);
            if (!m)
                return NULL;
        } else {
            JSValue func_val;
            func_val = JS_Eval(ctx, (char*)dyn_iobuf_rdata(&src),
                dyn_iobuf_rlen(&src), module_name,
                JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
            dyn_iobuf_free(&src);
            if (JS_IsException(func_val))
                return NULL;
            if (js_module_set_import_meta(ctx, func_val, TRUE, FALSE) < 0) {
                JS_FreeValue(ctx, JS_GetException(ctx));
            }
            m = JS_VALUE_GET_PTR(func_val);
            JS_FreeValue(ctx, func_val);
        }
    }
    return m;
}

static JSValue js_std_exit(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    int status;
    if (JS_ToInt32(ctx, &status, argv[0]))
        status = -1;
    exit(status);
    return JS_UNDEFINED;
}

static JSValue js_std_getenv(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char *name, *str;
    name = JS_ToCString(ctx, argv[0]);
    if (!name)
        return JS_EXCEPTION;
    str = getenv(name);
    JS_FreeCString(ctx, name);
    if (!str)
        return JS_UNDEFINED;
    else
        return JS_NewString(ctx, str);
}

#if defined(_WIN32)
static void setenv(const char* name, const char* value, int overwrite)
{
    char* str;
    size_t name_len, value_len;
    name_len = strlen(name);
    value_len = strlen(value);
    str = malloc(name_len + 1 + value_len + 1);
    if (!str)
        return;
    memcpy(str, name, name_len);
    str[name_len] = '=';
    memcpy(str + name_len + 1, value, value_len);
    str[name_len + 1 + value_len] = '\0';
    _putenv(str);
    free(str);
}

static void unsetenv(const char* name)
{
    setenv(name, "", TRUE);
}
#endif

static JSValue js_std_setenv(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char *name, *value;
    name = JS_ToCString(ctx, argv[0]);
    if (!name)
        return JS_EXCEPTION;
    value = JS_ToCString(ctx, argv[1]);
    if (!value) {
        JS_FreeCString(ctx, name);
        return JS_EXCEPTION;
    }
    setenv(name, value, TRUE);
    JS_FreeCString(ctx, name);
    JS_FreeCString(ctx, value);
    return JS_UNDEFINED;
}

static JSValue js_std_unsetenv(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char* name;
    name = JS_ToCString(ctx, argv[0]);
    if (!name)
        return JS_EXCEPTION;
    unsetenv(name);
    JS_FreeCString(ctx, name);
    return JS_UNDEFINED;
}

static JSValue js_std_getenviron(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    char** envp;
    const char *name, *p, *value;
    JSValue obj;
    uint32_t idx;
    size_t name_len;
    JSAtom atom;
    int ret;

    obj = JS_NewObject(ctx);
    if (JS_IsException(obj))
        return JS_EXCEPTION;
    envp = environ;
    for (idx = 0; envp[idx] != NULL; idx++) {
        name = envp[idx];
        p = strchr(name, '=');
        if (!p)
            continue;
        name_len = p - name;
        value = p + 1;
        atom = JS_NewAtomLen(ctx, name, name_len);
        if (atom == JS_ATOM_NULL)
            goto fail;
        ret = JS_DefinePropertyValue(ctx, obj, atom, JS_NewString(ctx, value),
            JS_PROP_C_W_E);
        JS_FreeAtom(ctx, atom);
        if (ret < 0)
            goto fail;
    }
    return obj;
fail:
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
}

static JSValue js_std_gc(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JS_RunGC(JS_GetRuntime(ctx));
    return JS_UNDEFINED;
}

static int interrupt_handler(JSRuntime* rt, void* opaque)
{
    return (atomic_load_explicit(&os_pending_signals, memory_order_relaxed) >> SIGINT) & 1;
}

static int get_bool_option(JSContext* ctx, BOOL* pbool,
    JSValueConst obj,
    const char* option)
{
    JSValue val;
    val = JS_GetPropertyStr(ctx, obj, option);
    if (JS_IsException(val))
        return -1;
    if (!JS_IsUndefined(val)) {
        *pbool = JS_ToBool(ctx, val);
    }
    JS_FreeValue(ctx, val);
    return 0;
}

static JSValue js_evalScript(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSRuntime* rt = JS_GetRuntime(ctx);
    JSThreadState* ts = JS_GetRuntimeOpaque(rt);
    const char* str;
    size_t len;
    JSValue ret;
    JSValueConst options_obj;
    BOOL backtrace_barrier = FALSE;
    BOOL is_async = FALSE;
    int flags;

    if (argc >= 2) {
        options_obj = argv[1];
        if (get_bool_option(ctx, &backtrace_barrier, options_obj,
                "backtrace_barrier"))
            return JS_EXCEPTION;
        if (get_bool_option(ctx, &is_async, options_obj,
                "async"))
            return JS_EXCEPTION;
    }

    str = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!str)
        return JS_EXCEPTION;
    if (!ts->recv_pipe && ++ts->eval_script_recurse == 1) {
        JS_SetInterruptHandler(JS_GetRuntime(ctx), interrupt_handler, NULL);
    }
    flags = JS_EVAL_TYPE_GLOBAL;
    if (backtrace_barrier)
        flags |= JS_EVAL_FLAG_BACKTRACE_BARRIER;
    if (is_async)
        flags |= JS_EVAL_FLAG_ASYNC;
    ret = JS_Eval(ctx, str, len, "<evalScript>", flags);
    JS_FreeCString(ctx, str);
    if (!ts->recv_pipe && --ts->eval_script_recurse == 0) {
        JS_SetInterruptHandler(JS_GetRuntime(ctx), NULL, NULL);
        atomic_fetch_and_explicit(&os_pending_signals,
            ~((uint64_t)1 << SIGINT), memory_order_relaxed);
        if (JS_IsException(ret))
            JS_SetUncatchableException(ctx, FALSE);
    }
    return ret;
}

static JSClassID js_std_file_class_id;

typedef struct {
    FILE* f;
    BOOL close_in_finalizer;
} JSSTDFile;

static void js_std_file_finalizer(JSRuntime* rt, JSValue val)
{
    JSSTDFile* s = JS_GetOpaque(val, js_std_file_class_id);
    if (s) {
        if (s->f && s->close_in_finalizer)
            fclose(s->f);
        js_free_rt(rt, s);
    }
}

static ssize_t js_get_errno(ssize_t ret)
{
    if (ret == -1)
        ret = -errno;
    return ret;
}

static JSValue js_std_strerror(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    int err;
    if (JS_ToInt32(ctx, &err, argv[0]))
        return JS_EXCEPTION;
    return JS_NewString(ctx, strerror(err));
}

static JSValue js_std_parseExtJSON(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSValue obj;
    const char* str;
    size_t len;

    str = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!str)
        return JS_EXCEPTION;
    obj = JS_ParseJSON2(ctx, str, len, "<input>", JS_PARSE_JSON_EXT);
    JS_FreeCString(ctx, str);
    return obj;
}

static JSValue js_new_std_file(JSContext* ctx, FILE* f,
    BOOL close_in_finalizer)
{
    JSSTDFile* s;
    JSValue obj;
    obj = JS_NewObjectClass(ctx, js_std_file_class_id);
    if (JS_IsException(obj))
        return obj;
    s = js_mallocz(ctx, sizeof(*s));
    if (!s) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    s->close_in_finalizer = close_in_finalizer;
    s->f = f;
    JS_SetOpaque(obj, s);
    return obj;
}

static void js_set_error_object(JSContext* ctx, JSValue obj, int err)
{
    if (!JS_IsUndefined(obj)) {
        JS_SetPropertyStr(ctx, obj, "errno", JS_NewInt32(ctx, err));
    }
}

static JSValue js_std_open(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char *filename, *mode = NULL;
    FILE* f;
    int err;

    filename = JS_ToCString(ctx, argv[0]);
    if (!filename)
        goto fail;
    mode = JS_ToCString(ctx, argv[1]);
    if (!mode)
        goto fail;
    if (mode[strspn(mode, "rwa+b")] != '\0') {
        JS_ThrowTypeError(ctx, "std.open: invalid file mode");
        goto fail;
    }

    f = fopen(filename, mode);
    if (!f)
        err = errno;
    else
        err = 0;
    if (argc >= 3)
        js_set_error_object(ctx, argv[2], err);
    JS_FreeCString(ctx, filename);
    JS_FreeCString(ctx, mode);
    if (!f)
        return JS_NULL;
    return js_new_std_file(ctx, f, TRUE);
fail:
    JS_FreeCString(ctx, filename);
    JS_FreeCString(ctx, mode);
    return JS_EXCEPTION;
}

static JSValue js_std_fdopen(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char* mode;
    FILE* f;
    int fd, err;

    if (JS_ToInt32(ctx, &fd, argv[0]))
        return JS_EXCEPTION;
    mode = JS_ToCString(ctx, argv[1]);
    if (!mode)
        goto fail;
    if (mode[strspn(mode, "rwa+")] != '\0') {
        JS_ThrowTypeError(ctx, "std.fdopen: invalid file mode");
        goto fail;
    }

    f = fdopen(fd, mode);
    if (!f)
        err = errno;
    else
        err = 0;
    if (argc >= 3)
        js_set_error_object(ctx, argv[2], err);
    JS_FreeCString(ctx, mode);
    if (!f)
        return JS_NULL;
    return js_new_std_file(ctx, f, TRUE);
fail:
    JS_FreeCString(ctx, mode);
    return JS_EXCEPTION;
}

static JSValue js_std_tmpfile(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    FILE* f;
    f = tmpfile();
    if (argc >= 1)
        js_set_error_object(ctx, argv[0], f ? 0 : errno);
    if (!f)
        return JS_NULL;
    return js_new_std_file(ctx, f, TRUE);
}

static JSValue js_std_sprintf(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    return js_printf_internal(ctx, argc, argv, NULL);
}

static JSValue js_std_printf(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    return js_printf_internal(ctx, argc, argv, stdout);
}

static FILE* js_std_file_get(JSContext* ctx, JSValueConst obj)
{
    JSSTDFile* s = JS_GetOpaque2(ctx, obj, js_std_file_class_id);
    if (!s)
        return NULL;
    if (!s->f) {
        JS_ThrowTypeError(ctx, "std.file: invalid file handle");
        return NULL;
    }
    return s->f;
}

static JSValue js_std_file_puts(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    FILE* f;
    int i;
    const char* str;
    size_t len;

    if (magic == 0) {
        f = stdout;
    } else {
        f = js_std_file_get(ctx, this_val);
        if (!f)
            return JS_EXCEPTION;
    }

    for (i = 0; i < argc; i++) {
        str = JS_ToCStringLen(ctx, &len, argv[i]);
        if (!str)
            return JS_EXCEPTION;
        fwrite(str, 1, len, f);
        JS_FreeCString(ctx, str);
    }
    return JS_UNDEFINED;
}

static JSValue js_std_file_close(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSSTDFile* s = JS_GetOpaque2(ctx, this_val, js_std_file_class_id);
    int err;
    if (!s)
        return JS_EXCEPTION;
    if (!s->f)
        return JS_ThrowTypeError(ctx, "std.file: invalid file handle");
    err = js_get_errno(fclose(s->f));
    s->f = NULL;
    return JS_NewInt32(ctx, err);
}

static JSValue js_std_file_printf(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    FILE* f = js_std_file_get(ctx, this_val);
    if (!f)
        return JS_EXCEPTION;
    return js_printf_internal(ctx, argc, argv, f);
}

static void js_print_value_write(void* opaque, const char* buf, size_t len)
{
    FILE* fo = opaque;
    fwrite(buf, 1, len, fo);
}

static JSValue js_std_file_printObject(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JS_PrintValue(ctx, js_print_value_write, stdout, argv[0], NULL);
    return JS_UNDEFINED;
}

static JSValue js_std_file_flush(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    FILE* f = js_std_file_get(ctx, this_val);
    if (!f)
        return JS_EXCEPTION;
    fflush(f);
    return JS_UNDEFINED;
}

static JSValue js_std_file_tell(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int is_bigint)
{
    FILE* f = js_std_file_get(ctx, this_val);
    int64_t pos;
    if (!f)
        return JS_EXCEPTION;
#if defined(__linux__) || defined(__GLIBC__)
    pos = ftello(f);
#else
    pos = ftell(f);
#endif
    if (is_bigint)
        return JS_NewBigInt64(ctx, pos);
    else
        return JS_NewInt64(ctx, pos);
}

static JSValue js_std_file_seek(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    FILE* f = js_std_file_get(ctx, this_val);
    int64_t pos;
    int whence, ret;
    if (!f)
        return JS_EXCEPTION;
    if (JS_ToInt64Ext(ctx, &pos, argv[0]))
        return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &whence, argv[1]))
        return JS_EXCEPTION;
#if defined(__linux__) || defined(__GLIBC__)
    ret = fseeko(f, pos, whence);
#else
    ret = fseek(f, pos, whence);
#endif
    if (ret < 0)
        ret = -errno;
    return JS_NewInt32(ctx, ret);
}

static JSValue js_std_file_eof(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    FILE* f = js_std_file_get(ctx, this_val);
    if (!f)
        return JS_EXCEPTION;
    return JS_NewBool(ctx, feof(f));
}

static JSValue js_std_file_error(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    FILE* f = js_std_file_get(ctx, this_val);
    if (!f)
        return JS_EXCEPTION;
    return JS_NewBool(ctx, ferror(f));
}

static JSValue js_std_file_clearerr(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    FILE* f = js_std_file_get(ctx, this_val);
    if (!f)
        return JS_EXCEPTION;
    clearerr(f);
    return JS_UNDEFINED;
}

static JSValue js_std_file_fileno(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    FILE* f = js_std_file_get(ctx, this_val);
    if (!f)
        return JS_EXCEPTION;
    return JS_NewInt32(ctx, fileno(f));
}

static JSValue js_std_file_read_write(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    FILE* f = js_std_file_get(ctx, this_val);
    uint64_t pos, len;
    size_t size, ret;
    uint8_t* buf;

    if (!f)
        return JS_EXCEPTION;
    if (JS_ToIndex(ctx, &pos, argv[1]))
        return JS_EXCEPTION;
    if (JS_ToIndex(ctx, &len, argv[2]))
        return JS_EXCEPTION;
    buf = JS_GetArrayBuffer(ctx, &size, argv[0]);
    if (!buf)
        return JS_EXCEPTION;
    if (pos + len > size)
        return JS_ThrowRangeError(ctx, "std.file: read/write array buffer overflow");
    if (magic)
        ret = fwrite(buf + pos, 1, len, f);
    else
        ret = fread(buf + pos, 1, len, f);
    return JS_NewInt64(ctx, ret);
}

static JSValue js_std_file_write_bytes(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    FILE* f = js_std_file_get(ctx, this_val);
    JSValue abuf;
    uint8_t* base;
    size_t size, bpe, off, len;

    (void)argc;
    if (!f)
        return JS_EXCEPTION;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "std.file: writeBytes requires a byte view");
    abuf = JS_GetArrayBufferView(ctx, argv[0], &off, &len, &bpe);
    if (JS_IsException(abuf) || bpe != 1) {
        if (!JS_IsException(abuf))
            JS_FreeValue(ctx, abuf);
        return JS_ThrowTypeError(ctx, "std.file: writeBytes requires a Uint8Array");
    }
    base = JS_GetArrayBuffer(ctx, &size, abuf);
    JS_FreeValue(ctx, abuf);
    if (!base)
        return JS_EXCEPTION;
    if (off > size || len > size - off)
        return JS_ThrowRangeError(ctx, "std.file: writeBytes array buffer overflow");
    return JS_NewInt64(ctx, fwrite(base + off, 1, len, f));
}

static JSValue js_std_file_read_bytes(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    FILE* f = js_std_file_get(ctx, this_val);
    uint64_t max64 = 0;
    uint8_t* buf;
    size_t got;
    JSValue ab, args[3], out;

    if (!f)
        return JS_EXCEPTION;
    if (argc < 1 || JS_ToIndex(ctx, &max64, argv[0]))
        return JS_ThrowTypeError(ctx, "std.file: readBytes requires a length");
    if (max64 > (uint64_t)1 << 31)
        return JS_ThrowRangeError(ctx, "std.file: readBytes max is out of range");
    buf = (uint8_t*)js_malloc(ctx, max64 ? (size_t)max64 : 1);
    if (!buf)
        return JS_ThrowOutOfMemory(ctx);
    got = fread(buf, 1, (size_t)max64, f);
    ab = JS_NewArrayBufferCopy(ctx, buf, got);
    js_free(ctx, buf);
    if (JS_IsException(ab))
        return ab;
    args[0] = ab;
    args[1] = JS_UNDEFINED;
    args[2] = JS_UNDEFINED;
    out = JS_NewTypedArray(ctx, 3, args, JS_TYPED_ARRAY_UINT8);
    JS_FreeValue(ctx, ab);
    return out;
}

static JSValue js_std_file_getline(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    FILE* f = js_std_file_get(ctx, this_val);
    JSValue obj;

    if (!f)
        return JS_EXCEPTION;

#if !defined(_WIN32)
    {
        char* line = NULL;
        size_t cap = 0;
        ssize_t n = getdelim(&line, &cap, '\n', f);
        if (n < 0) {
            free(line);
            return JS_NULL;
        }
        if (n > 0 && line[n - 1] == '\n')
            n--;
        obj = JS_NewStringLen(ctx, line, n);
        free(line);
        return obj;
    }
#else
    {
        int c;
        DynBuf dbuf;
        js_std_dbuf_init(ctx, &dbuf);
        for (;;) {
            c = fgetc(f);
            if (c == EOF) {
                if (dbuf.size == 0) {
                    dbuf_free(&dbuf);
                    return JS_NULL;
                } else {
                    break;
                }
            }
            if (c == '\n')
                break;
            if (dbuf_putc(&dbuf, c)) {
                dbuf_free(&dbuf);
                return JS_ThrowOutOfMemory(ctx);
            }
        }
        obj = JS_NewStringLen(ctx, (const char*)dbuf.buf, dbuf.size);
        dbuf_free(&dbuf);
        return obj;
    }
#endif
}

static JSValue js_std_file_readAsString(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    FILE* f = js_std_file_get(ctx, this_val);
    DynBuf dbuf;
    JSValue obj;
    uint64_t max_size64;
    size_t max_size;
    JSValueConst max_size_val;

    if (!f)
        return JS_EXCEPTION;

    if (argc >= 1)
        max_size_val = argv[0];
    else
        max_size_val = JS_UNDEFINED;
    max_size = (size_t)-1;
    if (!JS_IsUndefined(max_size_val)) {
        if (JS_ToIndex(ctx, &max_size64, max_size_val))
            return JS_EXCEPTION;
        if (max_size64 < max_size)
            max_size = max_size64;
    }

    js_std_dbuf_init(ctx, &dbuf);

    {
        struct stat st;
        int fd = fileno(f);
        if (fd >= 0 && fstat(fd, &st) == 0 && S_ISREG(st.st_mode)) {
            int64_t pos = ftello(f);
            if (pos >= 0 && st.st_size > pos) {
                uint64_t want = (uint64_t)(st.st_size - pos);
                if (want > max_size)
                    want = max_size;
                if (want <= (UINT64_C(1) << 31))
                    dbuf_claim(&dbuf, (size_t)want);
            }
        }
    }

    while (max_size != 0) {
        char buf[65536];
        size_t want = max_size < sizeof(buf) ? max_size : sizeof(buf);
        size_t n = fread(buf, 1, want, f);
        if (n == 0)
            break;
        if (dbuf_put(&dbuf, (const uint8_t*)buf, n)) {
            dbuf_free(&dbuf);
            return JS_EXCEPTION;
        }
        max_size -= n;
    }
    obj = JS_NewStringLen(ctx, (const char*)dbuf.buf, dbuf.size);
    dbuf_free(&dbuf);
    return obj;
}

static JSValue js_std_file_getByte(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    FILE* f = js_std_file_get(ctx, this_val);
    if (!f)
        return JS_EXCEPTION;
    return JS_NewInt32(ctx, fgetc(f));
}

static JSValue js_std_file_putByte(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    FILE* f = js_std_file_get(ctx, this_val);
    int c;
    if (!f)
        return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &c, argv[0]))
        return JS_EXCEPTION;
    c = fputc(c, f);
    return JS_NewInt32(ctx, c);
}

static JSClassDef js_std_file_class = {
    "FILE",
    .finalizer = js_std_file_finalizer,
};

static const JSCFunctionListEntry js_std_error_props[] = {
#define DEF(x) JS_PROP_INT32_DEF(#x, x, JS_PROP_CONFIGURABLE)
    DEF(EINVAL),
    DEF(EIO),
    DEF(EACCES),
    DEF(EEXIST),
    DEF(ENOSPC),
    DEF(ENOSYS),
    DEF(EBUSY),
    DEF(ENOENT),
    DEF(EPERM),
    DEF(EPIPE),
    DEF(EBADF),
#undef DEF
};

static const JSCFunctionListEntry js_std_funcs[] = {
    JS_CFUNC_DEF("exit", 1, js_std_exit),
    JS_CFUNC_DEF("gc", 0, js_std_gc),
    JS_CFUNC_DEF("evalScript", 1, js_evalScript),
    JS_CFUNC_DEF("loadScript", 1, js_loadScript),
    JS_CFUNC_DEF("getenv", 1, js_std_getenv),
    JS_CFUNC_DEF("setenv", 1, js_std_setenv),
    JS_CFUNC_DEF("unsetenv", 1, js_std_unsetenv),
    JS_CFUNC_DEF("getenviron", 1, js_std_getenviron),
    JS_CFUNC_DEF("loadFile", 1, js_std_loadFile),
    JS_CFUNC_DEF("strerror", 1, js_std_strerror),
    JS_CFUNC_DEF("parseExtJSON", 1, js_std_parseExtJSON),

    JS_CFUNC_DEF("open", 2, js_std_open),
    JS_CFUNC_DEF("fdopen", 2, js_std_fdopen),
    JS_CFUNC_DEF("tmpfile", 0, js_std_tmpfile),
    JS_CFUNC_MAGIC_DEF("puts", 1, js_std_file_puts, 0),
    JS_CFUNC_DEF("printf", 1, js_std_printf),
    JS_CFUNC_DEF("sprintf", 1, js_std_sprintf),
    JS_PROP_INT32_DEF("SEEK_SET", SEEK_SET, JS_PROP_CONFIGURABLE),
    JS_PROP_INT32_DEF("SEEK_CUR", SEEK_CUR, JS_PROP_CONFIGURABLE),
    JS_PROP_INT32_DEF("SEEK_END", SEEK_END, JS_PROP_CONFIGURABLE),
    JS_OBJECT_DEF("Error", js_std_error_props, countof(js_std_error_props), JS_PROP_CONFIGURABLE),
    JS_CFUNC_DEF("__printObject", 1, js_std_file_printObject),
};

static const JSCFunctionListEntry js_std_file_proto_funcs[] = {
    JS_CFUNC_DEF("close", 0, js_std_file_close),
    JS_CFUNC_MAGIC_DEF("puts", 1, js_std_file_puts, 1),
    JS_CFUNC_MAGIC_DEF("writeStr", 1, js_std_file_puts, 1),
    JS_CFUNC_DEF("writeBytes", 1, js_std_file_write_bytes),
    JS_CFUNC_DEF("readBytes", 1, js_std_file_read_bytes),
    JS_CFUNC_DEF("printf", 1, js_std_file_printf),
    JS_CFUNC_DEF("flush", 0, js_std_file_flush),
    JS_CFUNC_MAGIC_DEF("tell", 0, js_std_file_tell, 0),
    JS_CFUNC_MAGIC_DEF("tello", 0, js_std_file_tell, 1),
    JS_CFUNC_DEF("seek", 2, js_std_file_seek),
    JS_CFUNC_DEF("eof", 0, js_std_file_eof),
    JS_CFUNC_DEF("fileno", 0, js_std_file_fileno),
    JS_CFUNC_DEF("error", 0, js_std_file_error),
    JS_CFUNC_DEF("clearerr", 0, js_std_file_clearerr),
    JS_CFUNC_MAGIC_DEF("read", 3, js_std_file_read_write, 0),
    JS_CFUNC_MAGIC_DEF("write", 3, js_std_file_read_write, 1),
    JS_CFUNC_DEF("getline", 0, js_std_file_getline),
    JS_CFUNC_DEF("readAsString", 0, js_std_file_readAsString),
    JS_CFUNC_DEF("getByte", 0, js_std_file_getByte),
    JS_CFUNC_DEF("putByte", 1, js_std_file_putByte),
};

static int js_std_init(JSContext* ctx, JSModuleDef* m)
{
    JSValue proto;

    JS_NewClassID(&js_std_file_class_id);
    JS_NewClass(JS_GetRuntime(ctx), js_std_file_class_id, &js_std_file_class);
    proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, proto, js_std_file_proto_funcs,
        countof(js_std_file_proto_funcs));
    JS_SetClassProto(ctx, js_std_file_class_id, proto);

    JS_SetModuleExportList(ctx, m, js_std_funcs,
        countof(js_std_funcs));
    JS_SetModuleExport(ctx, m, "in", js_new_std_file(ctx, stdin, FALSE));
    JS_SetModuleExport(ctx, m, "out", js_new_std_file(ctx, stdout, FALSE));
    JS_SetModuleExport(ctx, m, "err", js_new_std_file(ctx, stderr, FALSE));
    return 0;
}

JSModuleDef* js_init_module_std(JSContext* ctx, const char* module_name)
{
    JSModuleDef* m;
    m = JS_NewCModule(ctx, module_name, js_std_init);
    if (!m)
        return NULL;
    JS_AddModuleExportList(ctx, m, js_std_funcs, countof(js_std_funcs));
    JS_AddModuleExport(ctx, m, "in");
    JS_AddModuleExport(ctx, m, "out");
    JS_AddModuleExport(ctx, m, "err");
    return m;
}

static JSValue js_os_open(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char* filename;
    int flags, mode, ret;

    filename = JS_ToCString(ctx, argv[0]);
    if (!filename)
        return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &flags, argv[1]))
        goto fail;
    if (argc >= 3 && !JS_IsUndefined(argv[2])) {
        if (JS_ToInt32(ctx, &mode, argv[2])) {
        fail:
            JS_FreeCString(ctx, filename);
            return JS_EXCEPTION;
        }
    } else {
        mode = 0666;
    }
#if defined(_WIN32)
    if (!(flags & O_TEXT))
        flags |= O_BINARY;
#endif
    if (!(flags & O_CLOEXEC))
        flags |= O_CLOEXEC;
    ret = js_get_errno(open(filename, flags, mode));
    JS_FreeCString(ctx, filename);
    return JS_NewInt32(ctx, ret);
}

static JSValue js_os_close(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    int fd, ret;
    if (JS_ToInt32(ctx, &fd, argv[0]))
        return JS_EXCEPTION;
    js_os_clear_handler(ctx, fd);
    ret = js_get_errno(close(fd));
    return JS_NewInt32(ctx, ret);
}

static JSValue js_os_seek(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    int fd, whence;
    int64_t pos, ret;
    BOOL is_bigint;

    if (JS_ToInt32(ctx, &fd, argv[0]))
        return JS_EXCEPTION;
    is_bigint = JS_IsBigInt(ctx, argv[1]);
    if (JS_ToInt64Ext(ctx, &pos, argv[1]))
        return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &whence, argv[2]))
        return JS_EXCEPTION;
    ret = lseek(fd, pos, whence);
    if (ret == -1)
        ret = -errno;
    if (is_bigint)
        return JS_NewBigInt64(ctx, ret);
    else
        return JS_NewInt64(ctx, ret);
}

static JSValue js_os_read_write(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    int fd;
    uint64_t pos = 0, len = 0;
    int64_t fpos = 0;
    BOOL has_len = FALSE, has_fpos = FALSE;
    size_t size;
    ssize_t ret;
    uint8_t* buf;

    if (JS_ToInt32(ctx, &fd, argv[0]))
        return JS_EXCEPTION;
    if (argc > 2 && !JS_IsUndefined(argv[2])) {
        if (JS_ToIndex(ctx, &pos, argv[2]))
            return JS_EXCEPTION;
    }
    if (argc > 3 && !JS_IsUndefined(argv[3])) {
        if (JS_ToIndex(ctx, &len, argv[3]))
            return JS_EXCEPTION;
        has_len = TRUE;
    }
    if (argc > 4 && !JS_IsUndefined(argv[4])) {
        if (JS_ToInt64(ctx, &fpos, argv[4]))
            return JS_EXCEPTION;
        if (fpos < 0)
            return JS_ThrowRangeError(ctx, "os: read/write position must not be negative");
        has_fpos = TRUE;
    }
    buf = JS_GetArrayBuffer(ctx, &size, argv[1]);
    if (!buf)
        return JS_EXCEPTION;
    if (pos > size)
        return JS_ThrowRangeError(ctx, "os: read/write array buffer overflow");
    if (has_len) {
        if (len > size - pos)
            return JS_ThrowRangeError(ctx, "os: read/write array buffer overflow");
    } else {
        len = size - pos;
    }
    if (has_fpos) {
        if (magic)
            ret = js_get_errno(pwrite(fd, buf + pos, len, (off_t)fpos));
        else
            ret = js_get_errno(pread(fd, buf + pos, len, (off_t)fpos));
        if (ret < 0 && errno == ESPIPE) {
            if (magic)
                ret = js_get_errno(write(fd, buf + pos, len));
            else
                ret = js_get_errno(read(fd, buf + pos, len));
        }
    } else {
        if (magic)
            ret = js_get_errno(write(fd, buf + pos, len));
        else
            ret = js_get_errno(read(fd, buf + pos, len));
    }
    return JS_NewInt64(ctx, ret);
}

static JSValue js_os_isatty(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    int fd;
    if (JS_ToInt32(ctx, &fd, argv[0]))
        return JS_EXCEPTION;
    return JS_NewBool(ctx, isatty(fd));
}

#if defined(_WIN32)
static JSValue js_os_ttyGetWinSize(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    int fd;
    HANDLE handle;
    CONSOLE_SCREEN_BUFFER_INFO info;
    JSValue obj;

    if (JS_ToInt32(ctx, &fd, argv[0]))
        return JS_EXCEPTION;
    handle = (HANDLE)_get_osfhandle(fd);

    if (!GetConsoleScreenBufferInfo(handle, &info))
        return JS_NULL;
    obj = JS_NewArray(ctx);
    if (JS_IsException(obj))
        return obj;
    JS_DefinePropertyValueUint32(ctx, obj, 0, JS_NewInt32(ctx, info.dwSize.X), JS_PROP_C_W_E);
    JS_DefinePropertyValueUint32(ctx, obj, 1, JS_NewInt32(ctx, info.dwSize.Y), JS_PROP_C_W_E);
    return obj;
}

#define __ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#define __ENABLE_VIRTUAL_TERMINAL_INPUT 0x0200

static JSValue js_os_ttySetRaw(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    int fd;
    HANDLE handle;

    if (JS_ToInt32(ctx, &fd, argv[0]))
        return JS_EXCEPTION;
    handle = (HANDLE)_get_osfhandle(fd);
    SetConsoleMode(handle, ENABLE_WINDOW_INPUT | __ENABLE_VIRTUAL_TERMINAL_INPUT);
    _setmode(fd, _O_BINARY);
    if (fd == 0) {
        handle = (HANDLE)_get_osfhandle(1);
        SetConsoleMode(handle, ENABLE_PROCESSED_OUTPUT | ENABLE_WRAP_AT_EOL_OUTPUT | __ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
    return JS_UNDEFINED;
}
#else
static JSValue js_os_ttyGetWinSize(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    int fd;
    struct winsize ws;
    JSValue obj;

    if (JS_ToInt32(ctx, &fd, argv[0]))
        return JS_EXCEPTION;
    if (ioctl(fd, TIOCGWINSZ, &ws) == 0 && ws.ws_col >= 4 && ws.ws_row >= 4) {
        obj = JS_NewArray(ctx);
        if (JS_IsException(obj))
            return obj;
        JS_DefinePropertyValueUint32(ctx, obj, 0, JS_NewInt32(ctx, ws.ws_col), JS_PROP_C_W_E);
        JS_DefinePropertyValueUint32(ctx, obj, 1, JS_NewInt32(ctx, ws.ws_row), JS_PROP_C_W_E);
        return obj;
    } else {
        return JS_NULL;
    }
}

static struct termios oldtty;
static int term_exit_armed;

static void term_exit(void)
{
    if (isatty(1)) {
        ssize_t n = write(1, "\x1b[?2004l", 8);
        (void)n;
    }
    tcsetattr(0, TCSANOW, &oldtty);
}

static JSValue js_os_ttySetRaw(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    struct termios tty;
    int fd;

    if (JS_ToInt32(ctx, &fd, argv[0]))
        return JS_EXCEPTION;

    memset(&tty, 0, sizeof(tty));
    tcgetattr(fd, &tty);
    oldtty = tty;

    tty.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP
        | INLCR | IGNCR | ICRNL | IXON);
    tty.c_oflag |= OPOST;
    tty.c_lflag &= ~(ECHO | ECHONL | ICANON | IEXTEN);
    tty.c_cflag &= ~(CSIZE | PARENB);
    tty.c_cflag |= CS8;
    tty.c_cc[VMIN] = 1;
    tty.c_cc[VTIME] = 0;

    tcsetattr(fd, TCSANOW, &tty);

    if (!term_exit_armed) {
        term_exit_armed = 1;
        atexit(term_exit);
    }
    return JS_UNDEFINED;
}

#endif

static JSValue js_os_remove(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char* filename;
    int ret;

    filename = JS_ToCString(ctx, argv[0]);
    if (!filename)
        return JS_EXCEPTION;
#if defined(_WIN32)
    {
        struct stat st;
        if (stat(filename, &st) == 0 && S_ISDIR(st.st_mode)) {
            ret = rmdir(filename);
        } else {
            ret = unlink(filename);
        }
    }
#else
    ret = remove(filename);
#endif
    ret = js_get_errno(ret);
    JS_FreeCString(ctx, filename);
    return JS_NewInt32(ctx, ret);
}

static JSValue js_os_rename(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char *oldpath, *newpath;
    int ret;

    oldpath = JS_ToCString(ctx, argv[0]);
    if (!oldpath)
        return JS_EXCEPTION;
    newpath = JS_ToCString(ctx, argv[1]);
    if (!newpath) {
        JS_FreeCString(ctx, oldpath);
        return JS_EXCEPTION;
    }
    ret = js_get_errno(rename(oldpath, newpath));
    JS_FreeCString(ctx, oldpath);
    JS_FreeCString(ctx, newpath);
    return JS_NewInt32(ctx, ret);
}

static BOOL is_main_thread(JSRuntime* rt)
{
    JSThreadState* ts = JS_GetRuntimeOpaque(rt);
    return !ts->recv_pipe;
}

static JSOSRWHandler* find_rh(JSThreadState* ts, int fd)
{
    JSOSRWHandler* rh;
    struct list_head* el;

    list_for_each(el, &ts->os_rw_handlers)
    {
        rh = list_entry(el, JSOSRWHandler, link);
        if (rh->fd == fd)
            return rh;
    }
    return NULL;
}

static void free_rw_handler(JSRuntime* rt, JSOSRWHandler* rh)
{
    int i;
    list_del(&rh->link);
    for (i = 0; i < 2; i++) {
        JS_FreeValueRT(rt, rh->rw_func[i]);
    }
    js_free_rt(rt, rh);
}

static void js_os_clear_handler(JSContext* ctx, int fd)
{
    JSRuntime* rt = JS_GetRuntime(ctx);
    JSThreadState* ts = JS_GetRuntimeOpaque(rt);
    JSOSRWHandler* rh = find_rh(ts, fd);
    if (rh)
        free_rw_handler(rt, rh);
}

static JSValue js_os_setReadHandler(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    JSRuntime* rt = JS_GetRuntime(ctx);
    JSThreadState* ts = JS_GetRuntimeOpaque(rt);
    JSOSRWHandler* rh;
    int fd;
    JSValueConst func;

    if (JS_ToInt32(ctx, &fd, argv[0]))
        return JS_EXCEPTION;
    func = argv[1];
    if (JS_IsNull(func)) {
        rh = find_rh(ts, fd);
        if (rh) {
            JS_FreeValue(ctx, rh->rw_func[magic]);
            rh->rw_func[magic] = JS_NULL;
            if (JS_IsNull(rh->rw_func[0]) && JS_IsNull(rh->rw_func[1])) {
                free_rw_handler(JS_GetRuntime(ctx), rh);
            }
        }
    } else {
        if (!JS_IsFunction(ctx, func))
            return JS_ThrowTypeError(ctx, magic ? "os.setWriteHandler: not a function" : "os.setReadHandler: not a function");
        rh = find_rh(ts, fd);
        if (!rh) {
            rh = js_mallocz(ctx, sizeof(*rh));
            if (!rh)
                return JS_EXCEPTION;
            rh->fd = fd;
            rh->rw_func[0] = JS_NULL;
            rh->rw_func[1] = JS_NULL;
            list_add_tail(&rh->link, &ts->os_rw_handlers);
        }
        JS_FreeValue(ctx, rh->rw_func[magic]);
        rh->rw_func[magic] = JS_DupValue(ctx, func);
    }
    return JS_UNDEFINED;
}

static JSOSSignalHandler* find_sh(JSThreadState* ts, int sig_num)
{
    JSOSSignalHandler* sh;
    struct list_head* el;
    list_for_each(el, &ts->os_signal_handlers)
    {
        sh = list_entry(el, JSOSSignalHandler, link);
        if (sh->sig_num == sig_num)
            return sh;
    }
    return NULL;
}

static void free_sh(JSRuntime* rt, JSOSSignalHandler* sh)
{
    list_del(&sh->link);
    JS_FreeValueRT(rt, sh->func);
    js_free_rt(rt, sh);
}

static void os_signal_handler(int sig_num)
{
    atomic_fetch_or_explicit(&os_pending_signals,
        (uint64_t)1 << sig_num, memory_order_relaxed);
}

#if defined(_WIN32)
typedef void (*sighandler_t)(int sig_num);
#endif

#if !defined(_WIN32)
static int os_signal_install(uint32_t sig_num, sighandler_t handler)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    return sigaction((int)sig_num, &sa, NULL);
}
#else
#define os_signal_install(sig_num, handler) signal((sig_num), (handler))
#endif

static JSValue js_os_signal(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSRuntime* rt = JS_GetRuntime(ctx);
    JSThreadState* ts = JS_GetRuntimeOpaque(rt);
    JSOSSignalHandler* sh;
    uint32_t sig_num;
    JSValueConst func;
    sighandler_t handler;

    if (!is_main_thread(rt))
        return JS_ThrowTypeError(ctx,
            "os.signal: signal handler can only be set in the main thread");

    if (JS_ToUint32(ctx, &sig_num, argv[0]))
        return JS_EXCEPTION;
    if (sig_num >= 64)
        return JS_ThrowRangeError(ctx, "os.signal: invalid signal number");
    func = argv[1];
    if (JS_IsNull(func) || JS_IsUndefined(func)) {
        sh = find_sh(ts, sig_num);
        if (sh) {
            free_sh(JS_GetRuntime(ctx), sh);
        }
        if (JS_IsNull(func))
            handler = SIG_DFL;
        else
            handler = SIG_IGN;
        os_signal_install(sig_num, handler);
    } else {
        if (!JS_IsFunction(ctx, func))
            return JS_ThrowTypeError(ctx, "os.signal: not a function");
        sh = find_sh(ts, sig_num);
        if (!sh) {
            sh = js_mallocz(ctx, sizeof(*sh));
            if (!sh)
                return JS_EXCEPTION;
            sh->sig_num = sig_num;
            list_add_tail(&sh->link, &ts->os_signal_handlers);
        }
        JS_FreeValue(ctx, sh->func);
        sh->func = JS_DupValue(ctx, func);
        os_signal_install(sig_num, os_signal_handler);
    }
    return JS_UNDEFINED;
}

#if defined(__linux__) || defined(__APPLE__)
static int64_t get_time_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (ts.tv_nsec / 1000000);
}

static int64_t get_time_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}
#else
static int64_t get_time_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + (tv.tv_usec / 1000);
}

static int64_t get_time_ns(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000000000 + (tv.tv_usec * 1000);
}
#endif

static JSValue js_os_now(JSContext* ctx, JSValue this_val,
    int argc, JSValue* argv)
{
    uint64_t ns = (uint64_t)get_time_ns();
    ns = (ns / 1000) * 1000;
    return JS_NewFloat64(ctx, (double)ns / 1e6);
}

static void free_timer_body(JSRuntime* rt, JSOSTimer* th)
{
    int i;
    JS_FreeValueRT(rt, th->func);
    if (th->args) {
        for (i = 0; i < th->argc; i++)
            JS_FreeValueRT(rt, th->args[i]);
        js_free_rt(rt, th->args);
    }
    js_free_rt(rt, th);
}

static int timer_before(const JSOSTimer* a, const JSOSTimer* b)
{
    return a->timeout < b->timeout || (a->timeout == b->timeout && a->seq < b->seq);
}

static void timer_heap_place(JSThreadState* ts, uint32_t i, JSOSTimer* th)
{
    ts->timer_heap[i] = th;
    th->heap_idx = i;
}

static void timer_heap_up(JSThreadState* ts, uint32_t i)
{
    JSOSTimer* th = ts->timer_heap[i];
    while (i > 0) {
        uint32_t parent = (i - 1) / 2;
        if (!timer_before(th, ts->timer_heap[parent]))
            break;
        timer_heap_place(ts, i, ts->timer_heap[parent]);
        i = parent;
    }
    timer_heap_place(ts, i, th);
}

static void timer_heap_down(JSThreadState* ts, uint32_t i)
{
    JSOSTimer* th = ts->timer_heap[i];
    uint32_t n = (uint32_t)ts->n_timers;
    for (;;) {
        uint32_t c = 2 * i + 1;
        if (c >= n)
            break;
        if (c + 1 < n && timer_before(ts->timer_heap[c + 1], ts->timer_heap[c]))
            c++;
        if (!timer_before(ts->timer_heap[c], th))
            break;
        timer_heap_place(ts, i, ts->timer_heap[c]);
        i = c;
    }
    timer_heap_place(ts, i, th);
}

static void timer_ids_unlink(JSThreadState* ts, JSOSTimer* th)
{
    JSOSTimer** pp;
    if (th->timer_id <= 0 || !ts->timer_ids_cap)
        return;
    pp = &ts->timer_ids[(uint32_t)th->timer_id & (ts->timer_ids_cap - 1)];
    while (*pp && *pp != th)
        pp = &(*pp)->id_next;
    if (*pp)
        *pp = th->id_next;
    th->id_next = NULL;
}

static void timer_ids_grow(JSRuntime* rt, JSThreadState* ts)
{
    uint32_t cap = ts->timer_ids_cap ? ts->timer_ids_cap * 2 : 64, i;
    JSOSTimer** tab = (JSOSTimer**)js_mallocz_rt(rt, (size_t)cap * sizeof(*tab));
    if (!tab)
        return;
    for (i = 0; i < ts->timer_ids_cap; i++) {
        JSOSTimer* th = ts->timer_ids[i];
        while (th) {
            JSOSTimer* next = th->id_next;
            uint32_t b = (uint32_t)th->timer_id & (cap - 1);
            th->id_next = tab[b];
            tab[b] = th;
            th = next;
        }
    }
    js_free_rt(rt, ts->timer_ids);
    ts->timer_ids = tab;
    ts->timer_ids_cap = cap;
}

static void free_timer(JSRuntime* rt, JSOSTimer* th)
{
    JSThreadState* ts = JS_GetRuntimeOpaque(rt);
    if (ts) {
        uint32_t i = th->heap_idx;
        timer_ids_unlink(ts, th);
        ts->n_timers--;
        if (i != (uint32_t)ts->n_timers) {
            JSOSTimer* moved = ts->timer_heap[ts->n_timers];
            timer_heap_place(ts, i, moved);
            timer_heap_down(ts, i);
            timer_heap_up(ts, moved->heap_idx);
        }
    }
    free_timer_body(rt, th);
}

static JSOSTimer* find_timer_by_id(JSThreadState* ts, int timer_id);

#define DYN_TIMER_MAX_LIVE 100000
#define JS_WORKER_QUEUE_MAX_BYTES ((size_t)256 << 20)
#define JS_SIGNAL_POLL_MS 50
static int insert_timer_sorted(JSRuntime* rt, JSThreadState* ts, JSOSTimer* th)
{
    if ((uint32_t)ts->n_timers == ts->timer_heap_cap) {
        uint32_t cap = ts->timer_heap_cap ? ts->timer_heap_cap * 2 : 64;
        JSOSTimer** h = (JSOSTimer**)js_realloc_rt(rt, ts->timer_heap, (size_t)cap * sizeof(*h));
        if (!h)
            return -1;
        ts->timer_heap = h;
        ts->timer_heap_cap = cap;
    }
    if (th->timer_id > 0) {
        uint32_t b;
        if ((uint32_t)ts->n_timers >= ts->timer_ids_cap)
            timer_ids_grow(rt, ts);
        if (!ts->timer_ids_cap)
            return -1;
        b = (uint32_t)th->timer_id & (ts->timer_ids_cap - 1);
        th->id_next = ts->timer_ids[b];
        ts->timer_ids[b] = th;
    }
    th->seq = ++ts->timer_seq;
    timer_heap_place(ts, (uint32_t)ts->n_timers, th);
    ts->n_timers++;
    timer_heap_up(ts, th->heap_idx);
    return 0;
}

static void timer_requeue(JSThreadState* ts, JSOSTimer* th)
{
    th->seq = ++ts->timer_seq;
    timer_heap_down(ts, th->heap_idx);
    timer_heap_up(ts, th->heap_idx);
}

static JSValue js_os_setTimeout(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    JSRuntime* rt = JS_GetRuntime(ctx);
    JSThreadState* ts = JS_GetRuntimeOpaque(rt);
    int64_t delay;
    JSValueConst func;
    JSOSTimer* th;

    if (!ts)
        return JS_ThrowTypeError(ctx, "os.setTimeout: timers are not available in this context");
    func = argv[0];
    if (!JS_IsFunction(ctx, func))
        return JS_ThrowTypeError(ctx, "os.setTimeout: not a function");
    {
        double d;
        if (JS_ToFloat64(ctx, &d, argv[1]))
            return JS_EXCEPTION;
        if (isnan(d) || d < 0)
            d = 0;
        if (d > 2147483647.0)
            d = 2147483647.0;
        delay = (int64_t)d;
    }
    if (ts->n_timers >= DYN_TIMER_MAX_LIVE)
        return JS_ThrowRangeError(ctx, "os.setTimeout: too many live timers (cap %d)",
            DYN_TIMER_MAX_LIVE);
    th = js_mallocz(ctx, sizeof(*th));
    if (!th)
        return JS_EXCEPTION;
    do {
        th->timer_id = ts->next_timer_id;
        ts->next_timer_id++;
        if (ts->next_timer_id == INT32_MAX)
            ts->next_timer_id = 1;
    } while (find_timer_by_id(ts, th->timer_id));
    th->repeat = magic;
    if (magic && delay == 0)
        delay = 1;
    th->delay = delay;
    {
        int64_t now = get_time_ms();
        if (delay > INT64_MAX - now)
            th->timeout = INT64_MAX;
        else
            th->timeout = now + delay;
    }
    th->func = JS_DupValue(ctx, func);
    if (argc > 2) {
        int i;
        th->argc = argc - 2;
        th->args = js_mallocz(ctx, sizeof(JSValue) * (size_t)th->argc);
        if (!th->args) {
            free_timer_body(rt, th);
            return JS_EXCEPTION;
        }
        for (i = 0; i < th->argc; i++)
            th->args[i] = JS_DupValue(ctx, argv[i + 2]);
    }
    if (insert_timer_sorted(rt, ts, th) < 0) {
        free_timer_body(rt, th);
        return JS_ThrowOutOfMemory(ctx);
    }
    return JS_NewInt32(ctx, th->timer_id);
}

static JSOSTimer* find_timer_by_id(JSThreadState* ts, int timer_id)
{
    JSOSTimer* th;
    if (timer_id <= 0 || !ts->timer_ids_cap)
        return NULL;
    th = ts->timer_ids[(uint32_t)timer_id & (ts->timer_ids_cap - 1)];
    while (th && th->timer_id != timer_id)
        th = th->id_next;
    return th;
}

static JSValue js_os_clearTimeout(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSRuntime* rt = JS_GetRuntime(ctx);
    JSThreadState* ts = JS_GetRuntimeOpaque(rt);
    JSOSTimer* th;
    int timer_id;

    if (JS_ToInt32(ctx, &timer_id, argv[0]))
        return JS_EXCEPTION;
    th = find_timer_by_id(ts, timer_id);
    if (!th)
        return JS_UNDEFINED;
    free_timer(rt, th);
    return JS_UNDEFINED;
}

static JSValue js_os_sleepAsync(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSRuntime* rt = JS_GetRuntime(ctx);
    JSThreadState* ts = JS_GetRuntimeOpaque(rt);
    int64_t delay;
    JSOSTimer* th;
    JSValue promise, resolving_funcs[2];

    {
        double d;
        if (JS_ToFloat64(ctx, &d, argv[0]))
            return JS_EXCEPTION;
        if (isnan(d) || d < 0)
            d = 0;
        if (d > 2147483647.0)
            d = 2147483647.0;
        delay = (int64_t)d;
    }
    promise = JS_NewPromiseCapability(ctx, resolving_funcs);
    if (JS_IsException(promise))
        return JS_EXCEPTION;

    th = js_mallocz(ctx, sizeof(*th));
    if (!th) {
        JS_FreeValue(ctx, promise);
        JS_FreeValue(ctx, resolving_funcs[0]);
        JS_FreeValue(ctx, resolving_funcs[1]);
        return JS_EXCEPTION;
    }
    th->timer_id = -1;
    {
        int64_t now = get_time_ms();
        if (delay > INT64_MAX - now)
            th->timeout = INT64_MAX;
        else
            th->timeout = now + delay;
    }
    th->func = JS_DupValue(ctx, resolving_funcs[0]);
    JS_FreeValue(ctx, resolving_funcs[0]);
    JS_FreeValue(ctx, resolving_funcs[1]);
    if (insert_timer_sorted(rt, ts, th) < 0) {
        free_timer_body(rt, th);
        JS_FreeValue(ctx, promise);
        return JS_ThrowOutOfMemory(ctx);
    }
    return promise;
}

static void call_handler(JSContext* ctx, JSValueConst func)
{
    JSValue ret, func1;
    func1 = JS_DupValue(ctx, func);
    ret = JS_Call(ctx, func1, JS_UNDEFINED, 0, NULL);
    JS_FreeValue(ctx, func1);
    if (JS_IsException(ret)) {
        js_std_dump_error(ctx);
        exit(1);
    }
    JS_FreeValue(ctx, ret);
}

static void call_timer(JSContext* ctx, JSValueConst func, int argc,
    JSValueConst* args)
{
    JSRuntime* rt = JS_GetRuntime(ctx);
    JSValue ret, func1;
    JSValue* save = NULL;
    int i;

    func1 = JS_DupValue(ctx, func);
    if (argc > 0) {
        save = js_malloc_rt(rt, sizeof(JSValue) * (size_t)argc);
        if (save) {
            for (i = 0; i < argc; i++)
                save[i] = JS_DupValue(ctx, args[i]);
        } else {
            argc = 0;
        }
    }
    ret = JS_Call(ctx, func1, JS_UNDEFINED, argc, (JSValueConst*)save);
    JS_FreeValue(ctx, func1);
    if (save) {
        for (i = 0; i < argc; i++)
            JS_FreeValue(ctx, save[i]);
        js_free_rt(rt, save);
    }
    if (JS_IsException(ret)) {
        js_std_dump_error(ctx);
        exit(1);
    }
    JS_FreeValue(ctx, ret);
}

#ifdef USE_WORKER

#ifdef _WIN32

static int js_waker_init(JSWaker* w)
{
    w->handle = CreateEvent(NULL, TRUE, FALSE, NULL);
    return w->handle ? 0 : -1;
}

static void js_waker_signal(JSWaker* w)
{
    SetEvent(w->handle);
}

static void js_waker_clear(JSWaker* w)
{
    ResetEvent(w->handle);
}

static void js_waker_close(JSWaker* w)
{
    CloseHandle(w->handle);
    w->handle = INVALID_HANDLE_VALUE;
}

#else

static int js_waker_init(JSWaker* w)
{
    int fds[2];

    if (pipe(fds) < 0)
        return -1;
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    w->read_fd = fds[0];
    w->write_fd = fds[1];
    return 0;
}

static void js_waker_signal(JSWaker* w)
{
    int ret;

    for (;;) {
        ret = write(w->write_fd, "", 1);
        if (ret == 1)
            break;
        if (ret < 0 && errno != EAGAIN && errno != EINTR)
            break;
    }
}

static void js_waker_clear(JSWaker* w)
{
    uint8_t buf[16];
    int ret;

    for (;;) {
        ret = read(w->read_fd, buf, sizeof(buf));
        if (ret >= 0)
            break;
        if (errno != EAGAIN && errno != EINTR)
            break;
    }
}

static void js_waker_close(JSWaker* w)
{
    close(w->read_fd);
    close(w->write_fd);
    w->read_fd = -1;
    w->write_fd = -1;
}

#endif

static void js_free_message(JSWorkerMessage* msg);

static int handle_posted_message(JSRuntime* rt, JSContext* ctx,
    JSWorkerMessageHandler* port)
{
    JSWorkerMessagePipe* ps = port->recv_pipe;
    int ret;
    struct list_head* el;
    JSWorkerMessage* msg;
    JSValue obj, data_obj, func, retval;

    pthread_mutex_lock(&ps->mutex);
    if (!list_empty(&ps->msg_queue)) {
        el = ps->msg_queue.next;
        msg = list_entry(el, JSWorkerMessage, link);

        list_del(&msg->link);
        ps->queued_bytes = ps->queued_bytes > msg->data_len
            ? ps->queued_bytes - msg->data_len
            : 0;

        if (list_empty(&ps->msg_queue))
            js_waker_clear(&ps->waker);

        pthread_mutex_unlock(&ps->mutex);

        data_obj = JS_ReadObject(ctx, msg->data, msg->data_len,
            JS_READ_OBJ_SAB | JS_READ_OBJ_REFERENCE);

        js_free_message(msg);

        if (JS_IsException(data_obj))
            goto fail;
        obj = JS_NewObject(ctx);
        if (JS_IsException(obj)) {
            JS_FreeValue(ctx, data_obj);
            goto fail;
        }
        JS_DefinePropertyValueStr(ctx, obj, "data", data_obj, JS_PROP_C_W_E);

        func = JS_DupValue(ctx, port->on_message_func);
        retval = JS_Call(ctx, func, JS_UNDEFINED, 1, (JSValueConst*)&obj);
        JS_FreeValue(ctx, obj);
        JS_FreeValue(ctx, func);
        if (JS_IsException(retval)) {
        fail:
            js_std_dump_error(ctx);
        } else {
            JS_FreeValue(ctx, retval);
        }
        ret = 1;
    } else {
        pthread_mutex_unlock(&ps->mutex);
        ret = 0;
    }
    return ret;
}
#else
static int handle_posted_message(JSRuntime* rt, JSContext* ctx,
    JSWorkerMessageHandler* port)
{
    return 0;
}
#endif

#if defined(_WIN32)

static int js_os_poll(JSContext* ctx)
{
    JSRuntime* rt = JS_GetRuntime(ctx);
    JSThreadState* ts = JS_GetRuntimeOpaque(rt);
    int min_delay, count;
    int64_t cur_time, delay;
    JSOSRWHandler* rh;
    struct list_head* el;
    HANDLE handles[MAXIMUM_WAIT_OBJECTS];

    if (list_empty(&ts->os_rw_handlers) && (ts->n_timers == 0) && list_empty(&ts->port_list)) {
        return -1;
    }

    if ((ts->n_timers > 0)) {
        JSOSTimer* th = ts->timer_heap[0];
        cur_time = get_time_ms();
        delay = th->timeout - cur_time;
        if (delay <= 0) {
            JSValue func;
            if (th->repeat) {
                th->timeout = cur_time + th->delay;
                timer_requeue(ts, th);
                call_timer(ctx, th->func, th->argc, (JSValueConst*)th->args);
                return 0;
            }
            func = th->func;
            th->func = JS_UNDEFINED;
            {
                int nargs = th->argc;
                JSValue* args = th->args;
                th->argc = 0;
                th->args = NULL;
                free_timer(rt, th);
                call_timer(ctx, func, nargs, (JSValueConst*)args);
                if (args) {
                    for (int i = 0; i < nargs; i++)
                        JS_FreeValueRT(rt, args[i]);
                    js_free_rt(rt, args);
                }
            }
            JS_FreeValue(ctx, func);
            return 0;
        }
        min_delay = delay < 10000 ? delay : 10000;
    } else {
        min_delay = -1;
    }

    count = 0;
    list_for_each(el, &ts->os_rw_handlers)
    {
        rh = list_entry(el, JSOSRWHandler, link);
        if (rh->fd == 0 && !JS_IsNull(rh->rw_func[0])) {
            handles[count++] = (HANDLE)_get_osfhandle(rh->fd);
            if (count == (int)countof(handles))
                break;
        }
    }

    list_for_each(el, &ts->port_list)
    {
        JSWorkerMessageHandler* port = list_entry(el, JSWorkerMessageHandler, link);
        if (JS_IsNull(port->on_message_func))
            continue;
        handles[count++] = port->recv_pipe->waker.handle;
        if (count == (int)countof(handles))
            break;
    }

    if (count > 0) {
        DWORD ret, timeout = INFINITE;
        if (min_delay != -1)
            timeout = min_delay;
        ret = WaitForMultipleObjects(count, handles, FALSE, timeout);

        if (ret < count) {
            list_for_each(el, &ts->os_rw_handlers)
            {
                rh = list_entry(el, JSOSRWHandler, link);
                if (rh->fd == 0 && !JS_IsNull(rh->rw_func[0])) {
                    call_handler(ctx, rh->rw_func[0]);
                    goto done;
                }
            }

            list_for_each(el, &ts->port_list)
            {
                JSWorkerMessageHandler* port = list_entry(el, JSWorkerMessageHandler, link);
                if (!JS_IsNull(port->on_message_func)) {
                    JSWorkerMessagePipe* ps = port->recv_pipe;
                    if (ps->waker.handle == handles[ret]) {
                        if (handle_posted_message(rt, ctx, port))
                            goto done;
                    }
                }
            }
        }
    } else {
        Sleep(min_delay);
    }
done:
    return 0;
}

#else

static no_inline int js_poll_expand(JSThreadState* ts)
{
    struct pollfd* new_fds;
    int new_size = max_int(ts->poll_fds_size + ts->poll_fds_size / 2, 16);
    new_fds = realloc(ts->poll_fds, new_size * sizeof(struct pollfd));
    if (!new_fds)
        return -1;
    ts->poll_fds = new_fds;
    ts->poll_fds_size = new_size;
    return 0;
}

static int js_poll_add_poll_fd(JSThreadState* ts, int* pnfds, int fd, int events)
{
    struct pollfd* fds;
    int nfds;
    nfds = *pnfds;
    if (unlikely(nfds >= ts->poll_fds_size)) {
        if (js_poll_expand(ts))
            return -1;
    }
    fds = &ts->poll_fds[nfds++];
    fds->fd = fd;
    fds->events = events;
    fds->revents = 0;
    *pnfds = nfds;
    return 0;
}

void js_std_set_io_reactor_rt(JSRuntime* rt, int fd,
    void (*drain)(void* udata), void* udata)
{
    JSThreadState* ts = JS_GetRuntimeOpaque(rt);
    if (!ts)
        return;
    ts->io_reactor_fd = fd;
    ts->io_reactor_drain = drain;
    ts->io_reactor_udata = udata;
    if (fd < 0)
        ts->io_reactor_wait = NULL;
}

void js_std_set_io_reactor_wait_rt(JSRuntime* rt,
    int (*wait)(void* udata, int timeout_ms))
{
    JSThreadState* ts = JS_GetRuntimeOpaque(rt);
    if (!ts)
        return;
    ts->io_reactor_wait = wait;
}

void js_std_set_io_reactor_wait(JSContext* ctx,
    int (*wait)(void* udata, int timeout_ms))
{
    js_std_set_io_reactor_wait_rt(JS_GetRuntime(ctx), wait);
}

void js_std_set_io_reactor(JSContext* ctx, int fd,
    void (*drain)(void* udata), void* udata)
{
    js_std_set_io_reactor_rt(JS_GetRuntime(ctx), fd, drain, udata);
}

static BOOL js_ports_have_pending(JSThreadState* ts)
{
    struct list_head* el;
    BOOL pending = FALSE;

    list_for_each(el, &ts->port_list)
    {
        JSWorkerMessageHandler* port = list_entry(el, JSWorkerMessageHandler, link);
        if (!JS_IsNull(port->on_message_func)) {
            JSWorkerMessagePipe* ps = port->recv_pipe;
            BOOL empty;
            pthread_mutex_lock(&ps->mutex);
            empty = list_empty(&ps->msg_queue);
            pthread_mutex_unlock(&ps->mutex);
            if (!empty) {
                pending = TRUE;
                break;
            }
        }
    }
    return pending;
}

static void js_release_worker_refs(JSRuntime* rt, JSThreadState* ts)
{
    struct list_head *el, *el1;

    list_for_each_safe(el, el1, &ts->worker_obj_list)
    {
        JSWorkerObjRef* wref = list_entry(el, JSWorkerObjRef, link);
        list_del(&wref->link);
        JS_FreeValueRT(rt, wref->obj);
        js_free_rt(rt, wref);
    }
}

#define JS_TIMER_IO_SLICE_MS 2

static int js_os_poll(JSContext* ctx)
{
    JSRuntime* rt = JS_GetRuntime(ctx);
    JSThreadState* ts = JS_GetRuntimeOpaque(rt);
    int min_delay, nfds;
    int64_t cur_time, delay;
    JSOSRWHandler* rh;
    struct list_head *el, *el1;

    if (!ts->recv_pipe && unlikely(atomic_load_explicit(&os_pending_signals, memory_order_relaxed) != 0)) {
        JSOSSignalHandler* sh;
        uint64_t mask;

        list_for_each(el, &ts->os_signal_handlers)
        {
            sh = list_entry(el, JSOSSignalHandler, link);
            mask = (uint64_t)1 << sh->sig_num;
            if (atomic_load_explicit(&os_pending_signals, memory_order_relaxed) & mask) {
                atomic_fetch_and_explicit(&os_pending_signals, ~mask,
                    memory_order_relaxed);
                call_handler(ctx, sh->func);
                return 0;
            }
        }
    }

    if (list_empty(&ts->os_rw_handlers) && (ts->n_timers == 0) && list_empty(&ts->port_list) && ts->io_reactor_fd < 0)
        return -1;

    if (!ts->recv_pipe && atomic_load_explicit(&js_live_worker_threads, memory_order_acquire) == 0 && !js_ports_have_pending(ts)) {
        js_release_worker_refs(rt, ts);
        if (ts->io_reactor_fd < 0 && list_empty(&ts->os_rw_handlers) && (ts->n_timers == 0))
            return -1;
    }

    if ((ts->n_timers > 0)) {
        JSOSTimer* th = ts->timer_heap[0];
        cur_time = get_time_ms();
        delay = th->timeout - cur_time;
        if (delay <= 0
            && (ts->last_io_poll_ms == 0
                || cur_time - ts->last_io_poll_ms < JS_TIMER_IO_SLICE_MS
                || (ts->io_reactor_fd < 0 && !ts->recv_pipe
                    && list_empty(&ts->os_rw_handlers)
                    && list_empty(&ts->port_list)))) {
            JSValue func;
            if (ts->last_io_poll_ms == 0)
                ts->last_io_poll_ms = cur_time;
            if (th->repeat) {
                th->timeout = cur_time + th->delay;
                timer_requeue(ts, th);
                call_timer(ctx, th->func, th->argc, (JSValueConst*)th->args);
                return 0;
            }
            func = th->func;
            th->func = JS_UNDEFINED;
            {
                int nargs = th->argc;
                JSValue* args = th->args;
                th->argc = 0;
                th->args = NULL;
                free_timer(rt, th);
                call_timer(ctx, func, nargs, (JSValueConst*)args);
                if (args) {
                    for (int i = 0; i < nargs; i++)
                        JS_FreeValueRT(rt, args[i]);
                    js_free_rt(rt, args);
                }
            }
            JS_FreeValue(ctx, func);
            return 0;
        }
        ts->last_io_poll_ms = 0;
        min_delay = delay <= 0 ? 0 : delay < 10000 ? delay : 10000;
    } else {
        min_delay = -1;
    }

    nfds = 0;
    list_for_each(el, &ts->os_rw_handlers)
    {
        int events;

        rh = list_entry(el, JSOSRWHandler, link);
        events = 0;
        if (!JS_IsNull(rh->rw_func[0]))
            events |= POLLIN;
        if (!JS_IsNull(rh->rw_func[1]))
            events |= POLLOUT;
        if (events) {
            rh->poll_fd_index = nfds;
            if (js_poll_add_poll_fd(ts, &nfds, rh->fd, events))
                return -1;
        }
    }

    list_for_each(el, &ts->port_list)
    {
        JSWorkerMessageHandler* port = list_entry(el, JSWorkerMessageHandler, link);
        if (!JS_IsNull(port->on_message_func)) {
            JSWorkerMessagePipe* ps = port->recv_pipe;
            port->poll_fd_index = nfds;
            if (js_poll_add_poll_fd(ts, &nfds, ps->waker.read_fd, POLLIN))
                return -1;
        }
    }

    int reactor_pfd_index = -1;
    if (ts->io_reactor_fd >= 0 && ts->io_reactor_drain) {
        reactor_pfd_index = nfds;
        if (js_poll_add_poll_fd(ts, &nfds, ts->io_reactor_fd, POLLIN))
            return -1;
    }

    if (!ts->recv_pipe && atomic_load_explicit(&js_live_worker_threads, memory_order_acquire) != 0 && (min_delay < 0 || min_delay > WORKER_EXIT_POLL_MS))
        min_delay = WORKER_EXIT_POLL_MS;

    {
        int deadline_left = js_std_deadline_remaining_ms();
        if (deadline_left >= 0 && (min_delay < 0 || min_delay > deadline_left))
            min_delay = deadline_left;
    }
    if (!list_empty(&ts->os_signal_handlers) && (min_delay < 0 || min_delay > JS_SIGNAL_POLL_MS))
        min_delay = JS_SIGNAL_POLL_MS;

    if (ts->io_reactor_fd >= 0 && ts->io_reactor_wait && ts->io_reactor_drain && list_empty(&ts->os_rw_handlers) && list_empty(&ts->port_list) && !ts->recv_pipe) {
        ts->io_reactor_wait(ts->io_reactor_udata, min_delay);
        goto done;
    }

    nfds = poll(ts->poll_fds, nfds, min_delay);
    if (nfds > 0) {
        if (reactor_pfd_index >= 0 && (ts->poll_fds[reactor_pfd_index].revents & (POLLIN | POLLERR | POLLHUP | POLLNVAL))) {
            ts->io_reactor_drain(ts->io_reactor_udata);
            goto done;
        }
        list_for_each_safe(el, el1, &ts->os_rw_handlers)
        {
            rh = list_entry(el, JSOSRWHandler, link);

            if (ts->poll_fds[rh->poll_fd_index].revents & POLLNVAL) {
                free_rw_handler(rt, rh);
                continue;
            }

            if (!JS_IsNull(rh->rw_func[0]) && (ts->poll_fds[rh->poll_fd_index].revents & (POLLERR | POLLHUP | POLLIN))) {
                call_handler(ctx, rh->rw_func[0]);
                goto done;
            }
            if (!JS_IsNull(rh->rw_func[1]) && (ts->poll_fds[rh->poll_fd_index].revents & (POLLERR | POLLHUP | POLLOUT))) {
                call_handler(ctx, rh->rw_func[1]);
                goto done;
            }
        }

        list_for_each(el, &ts->port_list)
        {
            JSWorkerMessageHandler* port = list_entry(el, JSWorkerMessageHandler, link);
            if (!JS_IsNull(port->on_message_func)) {
                if (ts->poll_fds[port->poll_fd_index].revents != 0) {
                    if (handle_posted_message(rt, ctx, port))
                        goto done;
                }
            }
        }
    }
done:
    return 0;
}
#endif

static JSValue make_obj_error(JSContext* ctx,
    JSValue obj,
    int err)
{
    JSValue arr;
    if (JS_IsException(obj))
        return obj;
    arr = JS_NewArray(ctx);
    if (JS_IsException(arr))
        return JS_EXCEPTION;
    JS_DefinePropertyValueUint32(ctx, arr, 0, obj,
        JS_PROP_C_W_E);
    JS_DefinePropertyValueUint32(ctx, arr, 1, JS_NewInt32(ctx, err),
        JS_PROP_C_W_E);
    return arr;
}

static JSValue make_string_error(JSContext* ctx,
    const char* buf,
    int err)
{
    return make_obj_error(ctx, JS_NewString(ctx, buf), err);
}

static JSValue js_os_getcwd(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    char buf[PATH_MAX];
    int err;

    if (!getcwd(buf, sizeof(buf))) {
        buf[0] = '\0';
        err = errno;
    } else {
        err = 0;
    }
    return make_string_error(ctx, buf, err);
}

static JSValue js_os_chdir(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char* target;
    int err;

    target = JS_ToCString(ctx, argv[0]);
    if (!target)
        return JS_EXCEPTION;
    err = js_get_errno(chdir(target));
    JS_FreeCString(ctx, target);
    return JS_NewInt32(ctx, err);
}

static JSValue js_os_mkdir(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    int mode, ret;
    const char* path;

    if (argc >= 2) {
        if (JS_ToInt32(ctx, &mode, argv[1]))
            return JS_EXCEPTION;
    } else {
        mode = 0777;
    }
    path = JS_ToCString(ctx, argv[0]);
    if (!path)
        return JS_EXCEPTION;
#if defined(_WIN32)
    (void)mode;
    ret = js_get_errno(mkdir(path));
#else
    ret = js_get_errno(mkdir(path, mode));
#endif
    JS_FreeCString(ctx, path);
    return JS_NewInt32(ctx, ret);
}

static JSValue js_os_readdir(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char* path;
    DIR* f;
    struct dirent* d;
    JSValue obj;
    int err;
    uint32_t len;

    path = JS_ToCString(ctx, argv[0]);
    if (!path)
        return JS_EXCEPTION;
    obj = JS_NewArray(ctx);
    if (JS_IsException(obj)) {
        JS_FreeCString(ctx, path);
        return JS_EXCEPTION;
    }
    f = opendir(path);
    if (!f)
        err = errno;
    else
        err = 0;
    JS_FreeCString(ctx, path);
    if (!f)
        goto done;
    len = 0;
    for (;;) {
        errno = 0;
        d = readdir(f);
        if (!d) {
            err = errno;
            break;
        }
        JS_DefinePropertyValueUint32(ctx, obj, len++,
            JS_NewString(ctx, d->d_name),
            JS_PROP_C_W_E);
    }
    closedir(f);
done:
    return make_obj_error(ctx, obj, err);
}

#if !defined(_WIN32)
static int64_t timespec_to_ms(const struct timespec* tv)
{
    return (int64_t)tv->tv_sec * 1000 + (tv->tv_nsec / 1000000);
}
#endif

static JSValue js_os_stat(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int is_lstat)
{
    const char* path;
    int err, res;
    struct stat st;
    JSValue obj;

    path = JS_ToCString(ctx, argv[0]);
    if (!path)
        return JS_EXCEPTION;
#if defined(_WIN32)
    res = stat(path, &st);
#else
    if (is_lstat)
        res = lstat(path, &st);
    else
        res = stat(path, &st);
#endif
    if (res < 0)
        err = errno;
    else
        err = 0;
    JS_FreeCString(ctx, path);
    if (res < 0) {
        obj = JS_NULL;
    } else {
        obj = JS_NewObject(ctx);
        if (JS_IsException(obj))
            return JS_EXCEPTION;
        JS_DefinePropertyValueStr(ctx, obj, "dev",
            JS_NewInt64(ctx, st.st_dev),
            JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, obj, "ino",
            JS_NewInt64(ctx, st.st_ino),
            JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, obj, "mode",
            JS_NewInt32(ctx, st.st_mode),
            JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, obj, "nlink",
            JS_NewInt64(ctx, st.st_nlink),
            JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, obj, "uid",
            JS_NewInt64(ctx, st.st_uid),
            JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, obj, "gid",
            JS_NewInt64(ctx, st.st_gid),
            JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, obj, "rdev",
            JS_NewInt64(ctx, st.st_rdev),
            JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, obj, "size",
            JS_NewInt64(ctx, st.st_size),
            JS_PROP_C_W_E);
#if !defined(_WIN32)
        JS_DefinePropertyValueStr(ctx, obj, "blocks",
            JS_NewInt64(ctx, st.st_blocks),
            JS_PROP_C_W_E);
#endif
#if defined(_WIN32)
        JS_DefinePropertyValueStr(ctx, obj, "atime",
            JS_NewInt64(ctx, (int64_t)st.st_atime * 1000),
            JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, obj, "mtime",
            JS_NewInt64(ctx, (int64_t)st.st_mtime * 1000),
            JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, obj, "ctime",
            JS_NewInt64(ctx, (int64_t)st.st_ctime * 1000),
            JS_PROP_C_W_E);
#elif defined(__APPLE__)
        JS_DefinePropertyValueStr(ctx, obj, "atime",
            JS_NewInt64(ctx, timespec_to_ms(&st.st_atimespec)),
            JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, obj, "mtime",
            JS_NewInt64(ctx, timespec_to_ms(&st.st_mtimespec)),
            JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, obj, "ctime",
            JS_NewInt64(ctx, timespec_to_ms(&st.st_ctimespec)),
            JS_PROP_C_W_E);
#else
        JS_DefinePropertyValueStr(ctx, obj, "atime",
            JS_NewInt64(ctx, timespec_to_ms(&st.st_atim)),
            JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, obj, "mtime",
            JS_NewInt64(ctx, timespec_to_ms(&st.st_mtim)),
            JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, obj, "ctime",
            JS_NewInt64(ctx, timespec_to_ms(&st.st_ctim)),
            JS_PROP_C_W_E);
#endif
    }
    return make_obj_error(ctx, obj, err);
}

#if !defined(_WIN32)
static void ms_to_timeval(struct timeval* tv, uint64_t v)
{
    tv->tv_sec = v / 1000;
    tv->tv_usec = (v % 1000) * 1000;
}
#endif

static JSValue js_os_utimes(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char* path;
    int64_t atime, mtime;
    int ret;

    if (JS_ToInt64(ctx, &atime, argv[1]))
        return JS_EXCEPTION;
    if (JS_ToInt64(ctx, &mtime, argv[2]))
        return JS_EXCEPTION;
    path = JS_ToCString(ctx, argv[0]);
    if (!path)
        return JS_EXCEPTION;
#if defined(_WIN32)
    {
        struct _utimbuf times;
        times.actime = atime / 1000;
        times.modtime = mtime / 1000;
        ret = js_get_errno(_utime(path, &times));
    }
#else
    {
        struct timeval times[2];
        ms_to_timeval(&times[0], atime);
        ms_to_timeval(&times[1], mtime);
        ret = js_get_errno(utimes(path, times));
    }
#endif
    JS_FreeCString(ctx, path);
    return JS_NewInt32(ctx, ret);
}

static JSValue js_os_sleep(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    int64_t delay;
    int ret;

    {
        double d;
        if (JS_ToFloat64(ctx, &d, argv[0]))
            return JS_EXCEPTION;
        if (isnan(d) || d < 0)
            d = 0;
        if (d > 2147483647.0)
            d = 2147483647.0;
        delay = (int64_t)d;
    }
#if defined(_WIN32)
    {
        if (delay > INT32_MAX)
            delay = INT32_MAX;
        Sleep(delay);
        ret = 0;
    }
#else
    {
        struct timespec ts, rem;
        ts.tv_sec = delay / 1000;
        ts.tv_nsec = (delay % 1000) * 1000000;
        while ((ret = nanosleep(&ts, &rem)) < 0 && errno == EINTR)
            ts = rem;
        ret = js_get_errno(ret);
    }
#endif
    return JS_NewInt32(ctx, ret);
}

#if defined(_WIN32)
static char* realpath(const char* path, char* buf)
{
    if (!_fullpath(buf, path, PATH_MAX)) {
        errno = ENOENT;
        return NULL;
    } else {
        return buf;
    }
}
#endif

static JSValue js_os_realpath(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char* path;
    char buf[PATH_MAX], *res;
    int err;

    path = JS_ToCString(ctx, argv[0]);
    if (!path)
        return JS_EXCEPTION;
    res = realpath(path, buf);
    JS_FreeCString(ctx, path);
    if (!res) {
        buf[0] = '\0';
        err = errno;
    } else {
        err = 0;
    }
    return make_string_error(ctx, buf, err);
}

#if !defined(_WIN32)
static JSValue js_os_symlink(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char *target, *linkpath;
    int err;

    target = JS_ToCString(ctx, argv[0]);
    if (!target)
        return JS_EXCEPTION;
    linkpath = JS_ToCString(ctx, argv[1]);
    if (!linkpath) {
        JS_FreeCString(ctx, target);
        return JS_EXCEPTION;
    }
    err = js_get_errno(symlink(target, linkpath));
    JS_FreeCString(ctx, target);
    JS_FreeCString(ctx, linkpath);
    return JS_NewInt32(ctx, err);
}

static JSValue js_os_readlink(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char* path;
    char buf[PATH_MAX];
    int err;
    ssize_t res;

    path = JS_ToCString(ctx, argv[0]);
    if (!path)
        return JS_EXCEPTION;
    res = readlink(path, buf, sizeof(buf) - 1);
    if (res < 0) {
        buf[0] = '\0';
        err = errno;
    } else {
        buf[res] = '\0';
        err = 0;
    }
    JS_FreeCString(ctx, path);
    return make_string_error(ctx, buf, err);
}

static char** build_envp(JSContext* ctx, JSValueConst obj)
{
    uint32_t len, i;
    JSPropertyEnum* tab;
    char **envp, *pair;
    const char *key, *str;
    JSValue val;
    size_t key_len, str_len;

    if (JS_GetOwnPropertyNames(ctx, &tab, &len, obj,
            JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY)
        < 0)
        return NULL;
    envp = js_mallocz(ctx, sizeof(envp[0]) * ((size_t)len + 1));
    if (!envp)
        goto fail;
    for (i = 0; i < len; i++) {
        val = JS_GetProperty(ctx, obj, tab[i].atom);
        if (JS_IsException(val))
            goto fail;
        str = JS_ToCString(ctx, val);
        JS_FreeValue(ctx, val);
        if (!str)
            goto fail;
        key = JS_AtomToCString(ctx, tab[i].atom);
        if (!key) {
            JS_FreeCString(ctx, str);
            goto fail;
        }
        key_len = strlen(key);
        str_len = strlen(str);
        pair = js_malloc(ctx, key_len + str_len + 2);
        if (!pair) {
            JS_FreeCString(ctx, key);
            JS_FreeCString(ctx, str);
            goto fail;
        }
        memcpy(pair, key, key_len);
        pair[key_len] = '=';
        memcpy(pair + key_len + 1, str, str_len);
        pair[key_len + 1 + str_len] = '\0';
        envp[i] = pair;
        JS_FreeCString(ctx, key);
        JS_FreeCString(ctx, str);
    }
done:
    JS_FreePropertyEnum(ctx, tab, len);
    return envp;
fail:
    if (envp) {
        for (i = 0; i < len; i++)
            js_free(ctx, envp[i]);
        js_free(ctx, envp);
        envp = NULL;
    }
    goto done;
}

#include <sys/types.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>
#include <spawn.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif

#if defined(__APPLE__)
#define DYN_SPAWN_HAVE_ADDCHDIR 1
#elif defined(__GLIBC__) && defined(__GLIBC_PREREQ)
#if __GLIBC_PREREQ(2, 29)
#define DYN_SPAWN_HAVE_ADDCHDIR 1
#endif
#elif defined(__linux__) && !defined(__GLIBC__)
#define DYN_SPAWN_HAVE_ADDCHDIR 1
#endif

#if defined(__GLIBC__) && defined(__GLIBC_PREREQ)
#if __GLIBC_PREREQ(2, 34)
#define DYN_SPAWN_HAVE_ADDCLOSEFROM 1
#endif
#endif

#define DYN_SPAWN_EXTRACT_BEGIN
static int my_execvpe(const char* filename, char** argv, char** envp)
{
    const char *path, *p, *p_next, *p1;
    char buf[PATH_MAX];
    size_t filename_len, path_len;
    int eacces_error;

    filename_len = strlen(filename);
    if (filename_len == 0) {
        errno = ENOENT;
        return -1;
    }
    if (strchr(filename, '/'))
        return execve(filename, argv, envp);

    path = getenv("PATH");
    if (!path)
        path = "/bin:/usr/bin";
    eacces_error = 0;
    p = path;
    for (p = path; p != NULL; p = p_next) {
        p1 = p;
        while (*p1 && *p1 != ':')
            p1++;
        path_len = p1 - p;
        if (*p1) {
            p_next = p1 + 1;
        } else {
            p_next = NULL;
            p1 = NULL;
        }
        if ((path_len + 1 + filename_len + 1) > PATH_MAX)
            continue;
        memcpy(buf, p, path_len);
        buf[path_len] = '/';
        memcpy(buf + path_len + 1, filename, filename_len);
        buf[path_len + 1 + filename_len] = '\0';

        execve(buf, argv, envp);

        switch (errno) {
        case EACCES:
            eacces_error = 1;
            break;
        case ENOENT:
        case ENOTDIR:
            break;
        default:
            return -1;
        }
    }
    if (eacces_error)
        errno = EACCES;
    return -1;
}

static _Thread_local int os_exec_own_group;

static int os_exec_fork_child(const char* file, char** argv, char** envp,
    int std_fds[3], const char* cwd,
    uid_t uid, gid_t gid, int use_path, pid_t* out)
{
    pid_t pid;
    int i;

    pid = fork();
    if (pid < 0)
        return errno;
    if (pid == 0) {
        if (os_exec_own_group)
            setpgid(0, 0);
        signal(SIGPIPE, SIG_DFL);
        for (i = 0; i < 3; i++) {
            if (std_fds[i] != i) {
                if (dup2(std_fds[i], i) < 0)
                    _exit(127);
            }
        }
#if defined(HAVE_CLOSEFROM)
        closefrom(3);
#elif defined(__linux__) && defined(SYS_close_range)
        syscall(SYS_close_range, 3, ~0U, 0);
#else
        {
            int fd_max = sysconf(_SC_OPEN_MAX);
            if (fd_max > 1024)
                fd_max = 1024;
            for (i = 3; i < fd_max; i++)
                close(i);
        }
#endif
        if (cwd) {
            if (chdir(cwd) < 0)
                _exit(127);
        }
        if (gid != (gid_t)-1) {
            if (setgid(gid) < 0)
                _exit(127);
        }
        if (uid != (uid_t)-1) {
            if (setuid(uid) < 0)
                _exit(127);
        }

        if (use_path)
            my_execvpe(file, argv, envp);
        else
            execve(file, argv, envp);
        _exit(127);
    }
    *out = pid;
    return 0;
}

static int os_exec_spawn_child(const char* file, char** argv, char** envp,
    int std_fds[3], const char* cwd,
    uid_t uid, gid_t gid, int use_path, pid_t* out)
{
    posix_spawn_file_actions_t fa;
    posix_spawnattr_t sa;
    short flags = 0;
    int i, rc;
    pid_t pid = 0;

#if defined(POSIX_SPAWN_SETUID) && defined(POSIX_SPAWN_SETGID)
#else
    if (uid != (uid_t)-1 || gid != (gid_t)-1)
        return os_exec_fork_child(file, argv, envp, std_fds, cwd,
            uid, gid, use_path, out);
#endif
#ifndef DYN_SPAWN_HAVE_ADDCHDIR
    if (cwd)
        return os_exec_fork_child(file, argv, envp, std_fds, cwd,
            uid, gid, use_path, out);
#endif

    for (i = 0; i < 3; i++) {
        if (std_fds[i] != i && fcntl(std_fds[i], F_GETFD) < 0)
            return errno ? errno : EBADF;
    }

    rc = posix_spawn_file_actions_init(&fa);
    if (rc == 0)
        rc = posix_spawnattr_init(&sa);
    if (rc == 0) {
        sigset_t dfl;
        sigemptyset(&dfl);
        sigaddset(&dfl, SIGPIPE);
        rc = posix_spawnattr_setsigdefault(&sa, &dfl);
        flags |= POSIX_SPAWN_SETSIGDEF;
    }
#if defined(__APPLE__) && defined(POSIX_SPAWN_CLOEXEC_DEFAULT)
    for (i = 0; rc == 0 && i < 3; i++) {
        if (std_fds[i] != i)
            rc = posix_spawn_file_actions_adddup2(&fa, std_fds[i], i);
        else
            rc = posix_spawn_file_actions_adddup2(&fa, i, i);
    }
    flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
#else
    for (i = 0; rc == 0 && i < 3; i++) {
        if (std_fds[i] != i)
            rc = posix_spawn_file_actions_adddup2(&fa, std_fds[i], i);
    }
#ifdef DYN_SPAWN_HAVE_ADDCLOSEFROM
    if (rc == 0)
        rc = posix_spawn_file_actions_addclosefrom_np(&fa, 3);
#else
    {
        int fd_max = sysconf(_SC_OPEN_MAX);
        if (fd_max > 1024)
            fd_max = 1024;
        for (i = 3; rc == 0 && i < fd_max; i++) {
#if defined(__linux__)
            rc = posix_spawn_file_actions_addclose(&fa, i);
#else
            if (fcntl(i, F_GETFD) >= 0)
                rc = posix_spawn_file_actions_addclose(&fa, i);
#endif
        }
    }
#endif
#endif
#ifdef DYN_SPAWN_HAVE_ADDCHDIR
    if (rc == 0 && cwd) {
#ifdef __APPLE__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
        rc = posix_spawn_file_actions_addchdir_np(&fa, cwd);
#ifdef __APPLE__
#pragma clang diagnostic pop
#endif
    }
#endif
#ifdef POSIX_SPAWN_USEVFORK
    flags |= POSIX_SPAWN_USEVFORK;
#endif
#if defined(POSIX_SPAWN_SETUID) && defined(POSIX_SPAWN_SETGID)
    if (rc == 0 && gid != (gid_t)-1) {
        flags |= POSIX_SPAWN_SETGID;
        rc = posix_spawnattr_setgid(&sa, gid);
    }
    if (rc == 0 && uid != (uid_t)-1) {
        flags |= POSIX_SPAWN_SETUID;
        rc = posix_spawnattr_setuid(&sa, uid);
    }
#endif
    if (rc == 0 && os_exec_own_group) {
        flags |= POSIX_SPAWN_SETPGROUP;
        rc = posix_spawnattr_setpgroup(&sa, 0);
    }
    if (rc == 0)
        rc = posix_spawnattr_setflags(&sa, flags);
    if (rc == 0) {
        if (use_path)
            rc = posix_spawnp(&pid, file, &fa, &sa, argv, envp);
        else
            rc = posix_spawn(&pid, file, &fa, &sa, argv, envp);
    }
    posix_spawnattr_destroy(&sa);
    posix_spawn_file_actions_destroy(&fa);
    if (rc)
        return rc;
    *out = pid;
    return 0;
}
#define DYN_SPAWN_EXTRACT_END
static const char* os_exec_cstr(JSContext* ctx, JSValueConst val)
{
    size_t n = 0;
    const char* str = JS_ToCStringLen(ctx, &n, val);
    if (str && memchr(str, 0, n)) {
        JS_FreeCString(ctx, str);
        JS_ThrowTypeError(ctx, "os.exec: string contains a NUL character");
        return NULL;
    }
    return str;
}

static JSValue js_os_exec(JSContext* ctx, JSValueConst this_val,
#define DYN_SPAWN_EXTRACT_END
    int argc, JSValueConst* argv)
{
    JSValueConst options, args = argv[0];
    JSValue val, ret_val;
    const char **exec_argv, *file = NULL, *str, *cwd = NULL;
    char** envp = environ;
    uint32_t exec_argc, i;
    int ret, status;
    pid_t pid;
    BOOL block_flag = TRUE, use_path = TRUE;
    int64_t timeout_ms = 0;
    static const char* std_name[3] = { "stdin", "stdout", "stderr" };
    int std_fds[3];
    uint32_t uid = -1, gid = -1;

    val = JS_GetPropertyStr(ctx, args, "length");
    if (JS_IsException(val))
        return JS_EXCEPTION;
    ret = JS_ToUint32(ctx, &exec_argc, val);
    JS_FreeValue(ctx, val);
    if (ret)
        return JS_EXCEPTION;
    if (exec_argc < 1 || exec_argc > 65535) {
        return JS_ThrowTypeError(ctx, "os.exec: invalid number of arguments");
    }
    exec_argv = js_mallocz(ctx, sizeof(exec_argv[0]) * (exec_argc + 1));
    if (!exec_argv)
        return JS_EXCEPTION;
    for (i = 0; i < exec_argc; i++) {
        val = JS_GetPropertyUint32(ctx, args, i);
        if (JS_IsException(val))
            goto exception;
        str = os_exec_cstr(ctx, val);
        JS_FreeValue(ctx, val);
        if (!str)
            goto exception;
        exec_argv[i] = str;
    }
    exec_argv[exec_argc] = NULL;

    for (i = 0; i < 3; i++)
        std_fds[i] = i;

    if (argc >= 2) {
        options = argv[1];

        if (get_bool_option(ctx, &block_flag, options, "blocking"))
            goto exception;
        if (get_bool_option(ctx, &block_flag, options, "block"))
            goto exception;
        if (get_bool_option(ctx, &use_path, options, "usePath"))
            goto exception;

        val = JS_GetPropertyStr(ctx, options, "timeout");
        if (JS_IsException(val))
            goto exception;
        if (JS_IsUndefined(val)) {
            JS_FreeValue(ctx, val);
            val = JS_GetPropertyStr(ctx, options, "timeoutMs");
            if (JS_IsException(val))
                goto exception;
        }
        if (!JS_IsUndefined(val)) {
            if (JS_ToInt64(ctx, &timeout_ms, val)) {
                JS_FreeValue(ctx, val);
                goto exception;
            }
            if (timeout_ms < 0) {
                JS_FreeValue(ctx, val);
                JS_ThrowRangeError(ctx, "os.exec: timeout must not be negative");
                goto exception;
            }
        }
        JS_FreeValue(ctx, val);

        val = JS_GetPropertyStr(ctx, options, "file");
        if (JS_IsException(val))
            goto exception;
        if (!JS_IsUndefined(val)) {
            file = os_exec_cstr(ctx, val);
            JS_FreeValue(ctx, val);
            if (!file)
                goto exception;
        }

        val = JS_GetPropertyStr(ctx, options, "cwd");
        if (JS_IsException(val))
            goto exception;
        if (!JS_IsUndefined(val)) {
            cwd = os_exec_cstr(ctx, val);
            JS_FreeValue(ctx, val);
            if (!cwd)
                goto exception;
        }

        for (i = 0; i < 3; i++) {
            val = JS_GetPropertyStr(ctx, options, std_name[i]);
            if (JS_IsException(val))
                goto exception;
            if (!JS_IsUndefined(val)) {
                int fd;
                ret = JS_ToInt32(ctx, &fd, val);
                JS_FreeValue(ctx, val);
                if (ret)
                    goto exception;
                std_fds[i] = fd;
            }
        }

        val = JS_GetPropertyStr(ctx, options, "env");
        if (JS_IsException(val))
            goto exception;
        if (!JS_IsUndefined(val)) {
            envp = build_envp(ctx, val);
            JS_FreeValue(ctx, val);
            if (!envp)
                goto exception;
        }

        val = JS_GetPropertyStr(ctx, options, "uid");
        if (JS_IsException(val))
            goto exception;
        if (!JS_IsUndefined(val)) {
            ret = JS_ToUint32(ctx, &uid, val);
            JS_FreeValue(ctx, val);
            if (ret)
                goto exception;
        }

        val = JS_GetPropertyStr(ctx, options, "gid");
        if (JS_IsException(val))
            goto exception;
        if (!JS_IsUndefined(val)) {
            ret = JS_ToUint32(ctx, &gid, val);
            JS_FreeValue(ctx, val);
            if (ret)
                goto exception;
        }
    }

    {
        const char* spawn_file = file ? file : exec_argv[0];
        os_exec_own_group = block_flag && timeout_ms > 0;
        ret = os_exec_spawn_child(spawn_file, DYN_UNCONST(exec_argv), envp,
            std_fds, cwd, (uid_t)uid, (gid_t)gid,
            use_path, &pid);
    }
    if (ret != 0) {
        if (ret == EAGAIN || ret == ENOMEM || ret == EPERM || ret == EINVAL) {
            JS_ThrowTypeError(ctx, "os.exec: fork error");
            goto exception;
        }
        if (!block_flag) {
            JS_ThrowTypeError(ctx, "os.exec: fork error");
            goto exception;
        }
        ret_val = JS_NewInt32(ctx, 127);
        goto done;
    }
    if (block_flag) {
        int64_t start = get_time_ms();
        int64_t grace = timeout_ms > 0 ? DYN_EXEC_TIMEOUT_GRACE_MS : 0;
        pid_t wret;

        if (timeout_ms <= 0) {
            for (;;) {
                wret = waitpid(pid, &status, 0);
                if (wret == pid) {
                    if (WIFEXITED(status)) {
                        ret = WEXITSTATUS(status);
                        break;
                    } else if (WIFSIGNALED(status)) {
                        ret = -WTERMSIG(status);
                        break;
                    }
                    continue;
                }
                if (wret < 0) {
                    if (errno == EINTR)
                        continue;
                    ret = -errno;
                }
                break;
            }
        } else {
            for (;;) {
                wret = waitpid(pid, &status, WNOHANG);
                if (wret == pid) {
                    if (WIFEXITED(status)) {
                        ret = WEXITSTATUS(status);
                        break;
                    } else if (WIFSIGNALED(status)) {
                        ret = -WTERMSIG(status);
                        break;
                    }
                    continue;
                }
                if (wret < 0) {
                    if (errno == EINTR)
                        continue;
                    ret = -errno;
                    break;
                }
                if (get_time_ms() >= start + timeout_ms) {
                    if (kill(-pid, SIGTERM) == 0 || kill(pid, SIGTERM) == 0) {
                        struct timespec ts = { grace / 1000, (grace % 1000) * 1000000L };
                        nanosleep(&ts, NULL);
                        kill(-pid, SIGKILL);
                        if (waitpid(pid, &status, WNOHANG) != pid) {
                            kill(pid, SIGKILL);
                            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
                            }
                        }
                    }
                    ret = -SIGTERM;
                    break;
                }
                {
                    struct timespec ts = { 0, 1000000L };
                    nanosleep(&ts, NULL);
                }
            }
        }
    } else {
        ret = pid;
    }
    ret_val = JS_NewInt32(ctx, ret);
done:
    JS_FreeCString(ctx, file);
    JS_FreeCString(ctx, cwd);
    for (i = 0; i < exec_argc; i++)
        JS_FreeCString(ctx, exec_argv[i]);
    js_free(ctx, exec_argv);
    if (envp && envp != environ) {
        char** p;
        p = envp;
        while (*p != NULL) {
            js_free(ctx, *p);
            p++;
        }
        js_free(ctx, envp);
    }
    return ret_val;
exception:
    ret_val = JS_EXCEPTION;
    goto done;
}

static JSValue js_os_getpid(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    return JS_NewInt32(ctx, getpid());
}

static JSValue js_os_waitpid(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    int pid, status, options, ret;
    JSValue obj;

    if (JS_ToInt32(ctx, &pid, argv[0]))
        return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &options, argv[1]))
        return JS_EXCEPTION;

    ret = waitpid(pid, &status, options);
    if (ret < 0) {
        ret = -errno;
        status = 0;
    }

    obj = JS_NewArray(ctx);
    if (JS_IsException(obj))
        return obj;
    JS_DefinePropertyValueUint32(ctx, obj, 0, JS_NewInt32(ctx, ret),
        JS_PROP_C_W_E);
    JS_DefinePropertyValueUint32(ctx, obj, 1, JS_NewInt32(ctx, status),
        JS_PROP_C_W_E);
    return obj;
}

static JSValue js_os_pipe(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    int pipe_fds[2], ret;
    JSValue obj;

    ret = pipe(pipe_fds);
    if (ret < 0)
        return JS_NULL;
    fcntl(pipe_fds[0], F_SETFD, FD_CLOEXEC);
    fcntl(pipe_fds[1], F_SETFD, FD_CLOEXEC);
    obj = JS_NewArray(ctx);
    if (JS_IsException(obj))
        return obj;
    JS_DefinePropertyValueUint32(ctx, obj, 0, JS_NewInt32(ctx, pipe_fds[0]),
        JS_PROP_C_W_E);
    JS_DefinePropertyValueUint32(ctx, obj, 1, JS_NewInt32(ctx, pipe_fds[1]),
        JS_PROP_C_W_E);
    return obj;
}

static JSValue js_os_kill(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    int pid, sig, ret;

    if (JS_ToInt32(ctx, &pid, argv[0]))
        return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &sig, argv[1]))
        return JS_EXCEPTION;
    if (pid == 0 || pid == -1)
        return JS_ThrowRangeError(ctx,
            "os.kill: pid %d addresses every process; name a pid or -pgid", pid);
    if (sig < 0 || sig >= NSIG)
        return JS_ThrowRangeError(ctx, "os.kill: signal %d out of range", sig);
    ret = js_get_errno(kill(pid, sig));
    return JS_NewInt32(ctx, ret);
}

static JSValue js_os_dup(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    int fd, ret;

    if (JS_ToInt32(ctx, &fd, argv[0]))
        return JS_EXCEPTION;
    ret = js_get_errno(fcntl(fd, F_DUPFD_CLOEXEC, 3));
    return JS_NewInt32(ctx, ret);
}

static JSValue js_os_dup2(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    int fd, fd2, ret;

    if (JS_ToInt32(ctx, &fd, argv[0]))
        return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &fd2, argv[1]))
        return JS_EXCEPTION;
    if (fd != fd2)
        js_os_clear_handler(ctx, fd2);
    ret = js_get_errno(dup2(fd, fd2));
    return JS_NewInt32(ctx, ret);
}

#endif

#ifdef USE_WORKER

typedef struct {
    JSWorkerMessagePipe* recv_pipe;
    JSWorkerMessagePipe* send_pipe;
    JSWorkerMessageHandler* msg_handler;
} JSWorkerData;

typedef struct {
    char* filename;
    char* basename;
    JSWorkerMessagePipe *recv_pipe, *send_pipe;
    int strip_flags;
} WorkerFuncArgs;

typedef struct {
    int ref_count;
    struct list_head link;
    size_t size;
    uint64_t buf[0];
} JSSABHeader;

static struct list_head sab_registry = LIST_HEAD_INIT(sab_registry);
static pthread_mutex_t sab_registry_mutex = PTHREAD_MUTEX_INITIALIZER;

static JSClassID js_worker_class_id;
static JSContext* (*js_worker_new_context_func)(JSRuntime* rt);

static int atomic_add_int(int* ptr, int v)
{
    return atomic_fetch_add((_Atomic(uint32_t)*)ptr, v) + v;
}

static void* js_sab_alloc(void* opaque, size_t size)
{
    JSSABHeader* sab;
    sab = malloc(sizeof(JSSABHeader) + size);
    if (!sab)
        return NULL;
    sab->ref_count = 1;
    sab->size = size;
    pthread_mutex_lock(&sab_registry_mutex);
    list_add_tail(&sab->link, &sab_registry);
    pthread_mutex_unlock(&sab_registry_mutex);
    return sab->buf;
}

static void js_sab_free(void* opaque, void* ptr)
{
    JSSABHeader* sab;
    int ref_count;
    sab = (JSSABHeader*)((uint8_t*)ptr - sizeof(JSSABHeader));
    ref_count = atomic_add_int(&sab->ref_count, -1);
    assert(ref_count >= 0);
    if (ref_count == 0) {
        pthread_mutex_lock(&sab_registry_mutex);
        list_del(&sab->link);
        pthread_mutex_unlock(&sab_registry_mutex);
        free(sab);
    }
}

static void js_sab_dup(void* opaque, void* ptr)
{
    JSSABHeader* sab;
    sab = (JSSABHeader*)((uint8_t*)ptr - sizeof(JSSABHeader));
    atomic_add_int(&sab->ref_count, 1);
}

static int js_sab_valid_ptr(void* opaque, void* ptr, size_t byte_length,
    size_t max_byte_length)
{
    struct list_head* el;
    int valid = 0;

    pthread_mutex_lock(&sab_registry_mutex);
    list_for_each(el, &sab_registry)
    {
        JSSABHeader* sab = list_entry(el, JSSABHeader, link);
        if (sab->buf == ptr && byte_length <= sab->size
            && (max_byte_length == (size_t)UINT32_MAX
                || max_byte_length <= sab->size)) {
            valid = 1;
            break;
        }
    }
    pthread_mutex_unlock(&sab_registry_mutex);
    return valid;
}

static JSWorkerMessagePipe* js_new_message_pipe(void)
{
    JSWorkerMessagePipe* ps;

    ps = malloc(sizeof(*ps));
    if (!ps)
        return NULL;
    if (js_waker_init(&ps->waker)) {
        free(ps);
        return NULL;
    }
    ps->ref_count = 1;
    init_list_head(&ps->msg_queue);
    pthread_mutex_init(&ps->mutex, NULL);
    return ps;
}

static JSWorkerMessagePipe* js_dup_message_pipe(JSWorkerMessagePipe* ps)
{
    atomic_add_int(&ps->ref_count, 1);
    return ps;
}

static void* (*dyn_wmsg_alloc_fn)(size_t);
static void (*dyn_wmsg_free_fn)(void*);

void dyn_wmsg_set_allocator(void* (*alloc_fn)(size_t), void (*free_fn)(void*))
{
    dyn_wmsg_alloc_fn = alloc_fn;
    dyn_wmsg_free_fn = free_fn;
}

static void* dyn_wmsg_malloc(size_t n)
{
    if (dyn_wmsg_alloc_fn)
        return dyn_wmsg_alloc_fn(n);
    return malloc(n);
}

static void dyn_wmsg_free(void* p)
{
    if (dyn_wmsg_free_fn) {
        dyn_wmsg_free_fn(p);
        return;
    }
    free(p);
}

static void js_free_message(JSWorkerMessage* msg)
{
    size_t i;
    for (i = 0; i < msg->sab_tab_len; i++) {
        js_sab_free(NULL, msg->sab_tab[i]);
    }
    dyn_wmsg_free(msg->sab_tab);
    dyn_wmsg_free(msg->data);
    dyn_wmsg_free(msg);
}

static void js_free_message_pipe(JSWorkerMessagePipe* ps)
{
    struct list_head *el, *el1;
    JSWorkerMessage* msg;
    int ref_count;

    if (!ps)
        return;

    ref_count = atomic_add_int(&ps->ref_count, -1);
    assert(ref_count >= 0);
    if (ref_count == 0) {
        list_for_each_safe(el, el1, &ps->msg_queue)
        {
            msg = list_entry(el, JSWorkerMessage, link);
            js_free_message(msg);
        }
        pthread_mutex_destroy(&ps->mutex);
        js_waker_close(&ps->waker);
        free(ps);
    }
}

static void js_free_port(JSRuntime* rt, JSWorkerMessageHandler* port)
{
    if (port) {
        js_free_message_pipe(port->recv_pipe);
        JS_FreeValueRT(rt, port->on_message_func);
        if (port->link.prev)
            list_del(&port->link);
        js_free_rt(rt, port);
    }
}

static void js_worker_finalizer(JSRuntime* rt, JSValue val)
{
    JSWorkerData* worker = JS_GetOpaque(val, js_worker_class_id);
    if (worker) {
        js_free_message_pipe(worker->recv_pipe);
        js_free_message_pipe(worker->send_pipe);
        js_free_port(rt, worker->msg_handler);
        js_free_rt(rt, worker);
    }
}

static void js_worker_mark(JSRuntime* rt, JSValueConst val,
    JS_MarkFunc* mark_func)
{
    JSWorkerData* worker = JS_GetOpaque(val, js_worker_class_id);
    if (worker) {
        JSWorkerMessageHandler* port = worker->msg_handler;
        if (port) {
            JS_MarkValue(rt, port->on_message_func, mark_func);
        }
    }
}

static JSClassDef js_worker_class = {
    "Worker",
    .finalizer = js_worker_finalizer,
    .gc_mark = js_worker_mark,
};

#define JS_WORKER_LIVE_MAX 256
#define JS_WORKER_THREAD_STACK_BYTES ((size_t)JS_DEFAULT_STACK_SIZE * 8)

static void* worker_func(void* opaque)
{
    WorkerFuncArgs* args = opaque;
    JSRuntime* rt;
    JSThreadState* ts;
    JSContext* ctx;
    JSValue val;

    rt = JS_NewRuntime();
    if (rt == NULL) {
        fprintf(stderr, "JS_NewRuntime failure");
        exit(1);
    }
    JS_SetStripInfo(rt, args->strip_flags);
    js_std_apply_budgets(rt);
    js_std_init_handlers(rt);

    JS_SetModuleLoaderFunc2(rt, NULL, js_module_loader, js_module_check_attributes, NULL);

    ts = JS_GetRuntimeOpaque(rt);
    ts->recv_pipe = args->recv_pipe;
    ts->send_pipe = args->send_pipe;

    ctx = js_worker_new_context_func(rt);
    if (ctx == NULL) {
        fprintf(stderr, "JS_NewContext failure");
        free(args->filename);
        free(args->basename);
        free(args);
        atomic_fetch_sub_explicit(&js_live_worker_threads, 1, memory_order_release);
        js_std_free_handlers(rt);
        JS_FreeRuntime(rt);
        return NULL;
    }

    JS_SetCanBlock(rt, TRUE);

    js_std_add_helpers(ctx, -1, NULL);

    val = JS_LoadModule(ctx, args->basename, args->filename);
    free(args->filename);
    free(args->basename);
    free(args);
    val = js_std_await(ctx, val);
    if (JS_IsException(val))
        js_std_dump_error(ctx);
    JS_FreeValue(ctx, val);

    js_std_loop(ctx);

    atomic_fetch_sub_explicit(&js_live_worker_threads, 1, memory_order_release);

    JS_FreeContext(ctx);
    js_std_free_handlers(rt);
    JS_FreeRuntime(rt);
    return NULL;
}

static JSValue js_worker_ctor_internal(JSContext* ctx, JSValueConst new_target,
    JSWorkerMessagePipe* recv_pipe,
    JSWorkerMessagePipe* send_pipe)
{
    JSValue obj = JS_UNDEFINED, proto;
    JSWorkerData* s;

    if (JS_IsUndefined(new_target)) {
        proto = JS_GetClassProto(ctx, js_worker_class_id);
    } else {
        proto = JS_GetPropertyStr(ctx, new_target, "prototype");
        if (JS_IsException(proto))
            goto fail;
    }
    obj = JS_NewObjectProtoClass(ctx, proto, js_worker_class_id);
    JS_FreeValue(ctx, proto);
    if (JS_IsException(obj))
        goto fail;
    s = js_mallocz(ctx, sizeof(*s));
    if (!s)
        goto fail;
    s->recv_pipe = js_dup_message_pipe(recv_pipe);
    s->send_pipe = js_dup_message_pipe(send_pipe);

    JS_SetOpaque(obj, s);
    return obj;
fail:
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
}

static JSValue js_worker_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    JSRuntime* rt = JS_GetRuntime(ctx);
    JSThreadState* ts = JS_GetRuntimeOpaque(rt);
    WorkerFuncArgs* args = NULL;
    JSWorkerObjRef* wref;
    pthread_t tid;
    pthread_attr_t attr;
    JSValue obj = JS_UNDEFINED;
    int ret;
    const char *filename = NULL, *basename;
    JSAtom basename_atom;

    if (!is_main_thread(rt))
        return JS_ThrowTypeError(ctx, "Worker: cannot create a worker inside a worker");

    basename_atom = JS_GetScriptOrModuleName(ctx, 1);
    if (basename_atom == JS_ATOM_NULL) {
        return JS_ThrowTypeError(ctx, "Worker: could not determine calling script or module name");
    }
    basename = JS_AtomToCString(ctx, basename_atom);
    JS_FreeAtom(ctx, basename_atom);
    if (!basename)
        goto fail;

    filename = JS_ToCString(ctx, argv[0]);
    if (!filename)
        goto fail;

    args = malloc(sizeof(*args));
    if (!args)
        goto oom_fail;
    memset(args, 0, sizeof(*args));
    args->filename = strdup(filename);
    args->basename = strdup(basename);

    if (atomic_load_explicit(&js_live_worker_threads, memory_order_relaxed) >= JS_WORKER_LIVE_MAX) {
        JS_ThrowRangeError(ctx, "Worker: %d workers are already running", JS_WORKER_LIVE_MAX);
        goto fail;
    }
    args->recv_pipe = js_new_message_pipe();
    if (!args->recv_pipe)
        goto oom_fail;
    args->send_pipe = js_new_message_pipe();
    if (!args->send_pipe)
        goto oom_fail;

    args->strip_flags = JS_GetStripInfo(rt);

    obj = js_worker_ctor_internal(ctx, new_target,
        args->send_pipe, args->recv_pipe);
    if (JS_IsException(obj))
        goto fail;

    wref = js_mallocz(ctx, sizeof(*wref));
    if (!wref) {
        JS_FreeValue(ctx, obj);
        obj = JS_EXCEPTION;
        goto fail;
    }
    wref->obj = JS_DupValue(ctx, obj);
    list_add_tail(&wref->link, &ts->worker_obj_list);

    atomic_fetch_add_explicit(&js_live_worker_threads, 1, memory_order_relaxed);
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, JS_WORKER_THREAD_STACK_BYTES);
    ret = pthread_create(&tid, &attr, worker_func, args);
    pthread_attr_destroy(&attr);
    if (ret != 0) {
        JS_ThrowTypeError(ctx, "Worker: could not create worker");
        atomic_fetch_sub_explicit(&js_live_worker_threads, 1, memory_order_relaxed);
        list_del(&wref->link);
        JS_FreeValue(ctx, wref->obj);
        js_free(ctx, wref);
        goto fail;
    }
    JS_FreeCString(ctx, basename);
    JS_FreeCString(ctx, filename);
    return obj;
oom_fail:
    JS_ThrowOutOfMemory(ctx);
fail:
    JS_FreeCString(ctx, basename);
    JS_FreeCString(ctx, filename);
    if (args) {
        free(args->filename);
        free(args->basename);
        js_free_message_pipe(args->recv_pipe);
        js_free_message_pipe(args->send_pipe);
        free(args);
    }
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
}

static JSValue js_worker_postMessage(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSWorkerData* worker = JS_GetOpaque2(ctx, this_val, js_worker_class_id);
    JSWorkerMessagePipe* ps;
    size_t data_len, sab_tab_len, i;
    int queue_full;
    uint8_t* data;
    JSWorkerMessage* msg;
    uint8_t** sab_tab;
    JSValue* xfer = NULL;
    uint32_t nxfer = 0;

    if (!worker)
        return JS_EXCEPTION;
    if (argc >= 2 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
        JSValue lv;
        if (!JS_IsObject(argv[1]) || JS_IsArray(ctx, argv[1]) != 1) {
            JS_ThrowTypeError(ctx, "Worker.postMessage: transfer must be an array");
            return JS_EXCEPTION;
        }
        lv = JS_GetPropertyStr(ctx, argv[1], "length");
        if (JS_IsException(lv) || JS_ToUint32(ctx, &nxfer, lv)) {
            JS_FreeValue(ctx, lv);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, lv);
        if (nxfer > 1024) {
            JS_ThrowRangeError(ctx, "Worker.postMessage: transfer too large");
            return JS_EXCEPTION;
        }
        if (nxfer) {
            xfer = (JSValue*)malloc(sizeof(JSValue) * nxfer);
            if (!xfer)
                return JS_ThrowOutOfMemory(ctx);
            for (uint32_t ti = 0; ti < nxfer; ti++) {
                JSValue e = JS_GetPropertyUint32(ctx, argv[1], ti);
                JSValue buf = JS_UNDEFINED;
                int is_buf = 0;
                if (JS_IsException(e)) {
                    for (uint32_t k = 0; k < ti; k++)
                        JS_FreeValue(ctx, xfer[k]);
                    free(xfer);
                    return JS_EXCEPTION;
                }
                {
                    JSValue vb = JS_GetArrayBufferView(ctx, e, NULL, NULL, NULL);
                    if (!JS_IsException(vb)) {
                        buf = vb;
                        is_buf = 1;
                    } else {
                        JS_FreeValue(ctx, JS_GetException(ctx));
                        {
                            size_t ablen = 0;
                            uint8_t* ab = JS_GetArrayBuffer(ctx, &ablen, e);
                            if (ab || !JS_HasException(ctx)) {
                                if (JS_HasException(ctx)) {
                                    JS_FreeValue(ctx, JS_GetException(ctx));
                                }
                                buf = JS_DupValue(ctx, e);
                                is_buf = 1;
                            }
                        }
                    }
                }
                JS_FreeValue(ctx, e);
                if (is_buf) {
                    JSValue glob = JS_GetGlobalObject(ctx);
                    JSValue abctor = JS_GetPropertyStr(ctx, glob, "ArrayBuffer");
                    int is_ab = JS_IsInstanceOf(ctx, buf, abctor);
                    int is_sab = 0;
                    if (is_ab == 0) {
                        JSValue sabctor = JS_GetPropertyStr(ctx, glob, "SharedArrayBuffer");
                        if (!JS_IsException(sabctor) && !JS_IsUndefined(sabctor) && !JS_IsNull(sabctor)) {
                            is_sab = JS_IsInstanceOf(ctx, buf, sabctor);
                            if (is_sab < 0)
                                is_sab = 0;
                        }
                        JS_FreeValue(ctx, sabctor);
                    }
                    JS_FreeValue(ctx, abctor);
                    JS_FreeValue(ctx, glob);
                    if (is_ab <= 0) {
                        JS_FreeValue(ctx, buf);
                        for (uint32_t k = 0; k < ti; k++)
                            JS_FreeValue(ctx, xfer[k]);
                        free(xfer);
                        if (is_ab < 0)
                            return JS_EXCEPTION;
                        if (is_sab)
                            return JS_ThrowTypeError(ctx,
                                "Worker.postMessage: transfer[%u]: cannot transfer a SharedArrayBuffer", ti);
                        return JS_ThrowTypeError(ctx,
                            "Worker.postMessage: transfer[%u]: cannot transfer an ArrayBuffer or ArrayBufferView from another realm", ti);
                    }
                }
                if (!is_buf) {
                    JS_FreeValue(ctx, buf);
                    for (uint32_t k = 0; k < ti; k++)
                        JS_FreeValue(ctx, xfer[k]);
                    free(xfer);
                    JS_ThrowTypeError(ctx, "Worker.postMessage: transfer[%u] is not an ArrayBuffer", ti);
                    return JS_EXCEPTION;
                }
                if (JS_IsArrayBufferBorrowed(ctx, buf)) {
                    JS_FreeValue(ctx, buf);
                    for (uint32_t k = 0; k < ti; k++)
                        JS_FreeValue(ctx, xfer[k]);
                    free(xfer);
                    return JS_ThrowTypeError(ctx,
                        "Worker.postMessage: transfer[%u] is in use by a running native call", ti);
                }
                xfer[ti] = buf;
            }
        }
    }

    data = JS_WriteObject2(ctx, &data_len, argv[0],
        JS_WRITE_OBJ_SAB | JS_WRITE_OBJ_REFERENCE,
        &sab_tab, &sab_tab_len);
    if (!data) {
        if (xfer) {
            for (uint32_t k = 0; k < nxfer; k++)
                JS_FreeValue(ctx, xfer[k]);
            free(xfer);
        }
        return JS_EXCEPTION;
    }

    ps = worker->send_pipe;
    pthread_mutex_lock(&ps->mutex);
    queue_full = data_len > JS_WORKER_QUEUE_MAX_BYTES
        || ps->queued_bytes > JS_WORKER_QUEUE_MAX_BYTES - data_len;
    pthread_mutex_unlock(&ps->mutex);
    if (queue_full) {
        js_free(ctx, data);
        js_free(ctx, sab_tab);
        if (xfer) {
            for (uint32_t k = 0; k < nxfer; k++)
                JS_FreeValue(ctx, xfer[k]);
            free(xfer);
        }
        return JS_ThrowRangeError(ctx,
            "Worker.postMessage: the receiver has %u MiB undelivered; "
            "wait for it to drain",
            (unsigned)(JS_WORKER_QUEUE_MAX_BYTES >> 20));
    }

    msg = dyn_wmsg_malloc(sizeof(*msg));
    if (!msg)
        goto fail;
    msg->data = NULL;
    msg->sab_tab = NULL;

    msg->data = dyn_wmsg_malloc(data_len);
    if (!msg->data)
        goto fail;
    memcpy(msg->data, data, data_len);
    msg->data_len = data_len;

    if (sab_tab_len > 0) {
        msg->sab_tab = dyn_wmsg_malloc(sizeof(msg->sab_tab[0]) * sab_tab_len);
        if (!msg->sab_tab)
            goto fail;
        memcpy(msg->sab_tab, sab_tab, sizeof(msg->sab_tab[0]) * sab_tab_len);
    }
    msg->sab_tab_len = sab_tab_len;

    js_free(ctx, data);
    js_free(ctx, sab_tab);

    for (i = 0; i < msg->sab_tab_len; i++) {
        js_sab_dup(NULL, msg->sab_tab[i]);
    }

    pthread_mutex_lock(&ps->mutex);
    if (list_empty(&ps->msg_queue))
        js_waker_signal(&ps->waker);
    list_add_tail(&msg->link, &ps->msg_queue);
    ps->queued_bytes += msg->data_len;
    pthread_mutex_unlock(&ps->mutex);
    if (xfer) {
        for (uint32_t k = 0; k < nxfer; k++) {
            JS_DetachArrayBuffer(ctx, xfer[k]);
            JS_FreeValue(ctx, xfer[k]);
        }
        free(xfer);
    }
    return JS_UNDEFINED;
fail:
    if (msg) {
        dyn_wmsg_free(msg->data);
        dyn_wmsg_free(msg->sab_tab);
        dyn_wmsg_free(msg);
    }
    js_free(ctx, data);
    js_free(ctx, sab_tab);
    if (xfer) {
        for (uint32_t k = 0; k < nxfer; k++)
            JS_FreeValue(ctx, xfer[k]);
        free(xfer);
    }
    return JS_EXCEPTION;
}

static JSValue js_worker_set_onmessage(JSContext* ctx, JSValueConst this_val,
    JSValueConst func)
{
    JSRuntime* rt = JS_GetRuntime(ctx);
    JSThreadState* ts = JS_GetRuntimeOpaque(rt);
    JSWorkerData* worker = JS_GetOpaque2(ctx, this_val, js_worker_class_id);
    JSWorkerMessageHandler* port;

    if (!worker)
        return JS_EXCEPTION;

    port = worker->msg_handler;
    if (JS_IsNull(func)) {
        if (port) {
            js_free_port(rt, port);
            worker->msg_handler = NULL;
        }
    } else {
        if (!JS_IsFunction(ctx, func))
            return JS_ThrowTypeError(ctx, "Worker.onmessage: not a function");
        if (!port) {
            port = js_mallocz(ctx, sizeof(*port));
            if (!port)
                return JS_EXCEPTION;
            port->recv_pipe = js_dup_message_pipe(worker->recv_pipe);
            port->on_message_func = JS_NULL;
            list_add_tail(&port->link, &ts->port_list);
            worker->msg_handler = port;
        }
        JS_FreeValue(ctx, port->on_message_func);
        port->on_message_func = JS_DupValue(ctx, func);
    }
    return JS_UNDEFINED;
}

static JSValue js_worker_get_onmessage(JSContext* ctx, JSValueConst this_val)
{
    JSWorkerData* worker = JS_GetOpaque2(ctx, this_val, js_worker_class_id);
    JSWorkerMessageHandler* port;
    if (!worker)
        return JS_EXCEPTION;
    port = worker->msg_handler;
    if (port) {
        return JS_DupValue(ctx, port->on_message_func);
    } else {
        return JS_NULL;
    }
}

static const JSCFunctionListEntry js_worker_proto_funcs[] = {
    JS_CFUNC_DEF("postMessage", 2, js_worker_postMessage),
    JS_CGETSET_DEF("onmessage", js_worker_get_onmessage, js_worker_set_onmessage),
};

#endif

void js_std_set_worker_new_context_func(JSContext* (*func)(JSRuntime* rt))
{
#ifdef USE_WORKER
    js_worker_new_context_func = func;
#endif
}

#if defined(_WIN32)
#define OS_PLATFORM "win32"
#elif defined(__APPLE__)
#define OS_PLATFORM "darwin"
#elif defined(__EMSCRIPTEN__)
#define OS_PLATFORM "js"
#else
#define OS_PLATFORM "linux"
#endif

#define OS_FLAG(x) JS_PROP_INT32_DEF(#x, x, JS_PROP_CONFIGURABLE)

static const JSCFunctionListEntry js_os_funcs[] = {
    JS_CFUNC_DEF("open", 2, js_os_open),
    OS_FLAG(O_RDONLY),
    OS_FLAG(O_WRONLY),
    OS_FLAG(O_RDWR),
    OS_FLAG(O_APPEND),
    OS_FLAG(O_CREAT),
    OS_FLAG(O_EXCL),
    OS_FLAG(O_TRUNC),
#if defined(_WIN32)
    OS_FLAG(O_BINARY),
    OS_FLAG(O_TEXT),
#endif
    JS_CFUNC_DEF("close", 1, js_os_close),
    JS_CFUNC_DEF("seek", 3, js_os_seek),
    JS_CFUNC_MAGIC_DEF("read", 4, js_os_read_write, 0),
    JS_CFUNC_MAGIC_DEF("write", 4, js_os_read_write, 1),
    JS_CFUNC_DEF("isatty", 1, js_os_isatty),
    JS_CFUNC_DEF("ttyGetWinSize", 1, js_os_ttyGetWinSize),
    JS_CFUNC_DEF("ttySetRaw", 1, js_os_ttySetRaw),
    JS_CFUNC_DEF("remove", 1, js_os_remove),
    JS_CFUNC_DEF("rename", 2, js_os_rename),
    JS_CFUNC_MAGIC_DEF("setReadHandler", 2, js_os_setReadHandler, 0),
    JS_CFUNC_MAGIC_DEF("setWriteHandler", 2, js_os_setReadHandler, 1),
    JS_CFUNC_DEF("signal", 2, js_os_signal),
    OS_FLAG(SIGINT),
    OS_FLAG(SIGABRT),
    OS_FLAG(SIGFPE),
    OS_FLAG(SIGILL),
    OS_FLAG(SIGSEGV),
    OS_FLAG(SIGTERM),
#if !defined(_WIN32)
    OS_FLAG(SIGQUIT),
    OS_FLAG(SIGPIPE),
    OS_FLAG(SIGALRM),
    OS_FLAG(SIGUSR1),
    OS_FLAG(SIGUSR2),
    OS_FLAG(SIGCHLD),
    OS_FLAG(SIGCONT),
    OS_FLAG(SIGSTOP),
    OS_FLAG(SIGTSTP),
    OS_FLAG(SIGTTIN),
    OS_FLAG(SIGTTOU),
#endif
    JS_CFUNC_DEF("now", 0, js_os_now),
    JS_CFUNC_MAGIC_DEF("setTimeout", 2, js_os_setTimeout, 0),
    JS_CFUNC_MAGIC_DEF("setInterval", 2, js_os_setTimeout, 1),
    JS_CFUNC_DEF("clearTimeout", 1, js_os_clearTimeout),
    JS_CFUNC_DEF("clearInterval", 1, js_os_clearTimeout),
    JS_PROP_STRING_DEF("platform", OS_PLATFORM, 0),
    JS_CFUNC_DEF("getcwd", 0, js_os_getcwd),
    JS_CFUNC_DEF("chdir", 0, js_os_chdir),
    JS_CFUNC_DEF("mkdir", 1, js_os_mkdir),
    JS_CFUNC_DEF("readdir", 1, js_os_readdir),
    OS_FLAG(S_IFMT),
    OS_FLAG(S_IFIFO),
    OS_FLAG(S_IFCHR),
    OS_FLAG(S_IFDIR),
    OS_FLAG(S_IFBLK),
    OS_FLAG(S_IFREG),
#if !defined(_WIN32)
    OS_FLAG(S_IFSOCK),
    OS_FLAG(S_IFLNK),
    OS_FLAG(S_ISGID),
    OS_FLAG(S_ISUID),
#endif
    JS_CFUNC_MAGIC_DEF("stat", 1, js_os_stat, 0),
    JS_CFUNC_DEF("utimes", 3, js_os_utimes),
    JS_CFUNC_DEF("sleep", 1, js_os_sleep),
    JS_CFUNC_DEF("realpath", 1, js_os_realpath),
#if !defined(_WIN32)
    JS_CFUNC_MAGIC_DEF("lstat", 1, js_os_stat, 1),
    JS_CFUNC_DEF("symlink", 2, js_os_symlink),
    JS_CFUNC_DEF("readlink", 1, js_os_readlink),
    JS_CFUNC_DEF("exec", 1, js_os_exec),
    JS_CFUNC_DEF("getpid", 0, js_os_getpid),
    JS_CFUNC_DEF("waitpid", 2, js_os_waitpid),
    OS_FLAG(WNOHANG),
    JS_CFUNC_DEF("pipe", 0, js_os_pipe),
    JS_CFUNC_DEF("kill", 2, js_os_kill),
    JS_CFUNC_DEF("dup", 1, js_os_dup),
    JS_CFUNC_DEF("dup2", 2, js_os_dup2),
#endif
};

static int js_os_init(JSContext* ctx, JSModuleDef* m)
{
    atomic_store_explicit(&os_poll_func, js_os_poll, memory_order_relaxed);

#ifdef USE_WORKER
    {
        JSRuntime* rt = JS_GetRuntime(ctx);
        JSThreadState* ts = JS_GetRuntimeOpaque(rt);
        JSValue proto, obj;
        JS_NewClassID(&js_worker_class_id);
        JS_NewClass(JS_GetRuntime(ctx), js_worker_class_id, &js_worker_class);
        proto = JS_NewObject(ctx);
        JS_SetPropertyFunctionList(ctx, proto, js_worker_proto_funcs, countof(js_worker_proto_funcs));

        obj = JS_NewCFunction2(ctx, js_worker_ctor, "Worker", 1,
            JS_CFUNC_constructor, 0);
        JS_SetConstructor(ctx, obj, proto);

        JS_SetClassProto(ctx, js_worker_class_id, proto);

        if (ts->recv_pipe && ts->send_pipe) {
            JS_DefinePropertyValueStr(ctx, obj, "parent",
                js_worker_ctor_internal(ctx, JS_UNDEFINED, ts->recv_pipe, ts->send_pipe),
                JS_PROP_C_W_E);
        }

        JS_SetModuleExport(ctx, m, "Worker", obj);
    }
#endif

    return JS_SetModuleExportList(ctx, m, js_os_funcs,
        countof(js_os_funcs));
}

JSModuleDef* js_init_module_os(JSContext* ctx, const char* module_name)
{
    JSModuleDef* m;
    m = JS_NewCModule(ctx, module_name, js_os_init);
    if (!m)
        return NULL;
    JS_AddModuleExportList(ctx, m, js_os_funcs, countof(js_os_funcs));
#ifdef USE_WORKER
    JS_AddModuleExport(ctx, m, "Worker");
#endif
    return m;
}

static JSValue js_print_internal(JSContext* ctx, int argc, JSValueConst* argv,
    FILE* fp)
{
    int i;
    JSValueConst v;

    for (i = 0; i < argc; i++) {
        if (i != 0)
            fputc(' ', fp);
        v = argv[i];
        if (JS_IsString(v)) {
            const char* str;
            size_t len;
            str = JS_ToCStringLen(ctx, &len, v);
            if (!str)
                return JS_EXCEPTION;
            fwrite(str, 1, len, fp);
            JS_FreeCString(ctx, str);
        } else {
            JS_PrintValue(ctx, js_print_value_write, fp, v, NULL);
        }
    }
    fputc('\n', fp);
    return JS_UNDEFINED;
}

static JSValue js_print(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    return js_print_internal(ctx, argc, argv, stdout);
}

static JSValue js_console_write(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    FILE* fp = (magic & 1) ? stderr : stdout;
    JSValue ret;
    ret = js_print_internal(ctx, argc, argv, fp);
    fflush(fp);
    return ret;
}

static JSValue js_console_assert(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    if (argc > 0 && JS_ToBool(ctx, argv[0]))
        return JS_UNDEFINED;
    fputs("Assertion failed:", stderr);
    if (argc > 1)
        fputc(' ', stderr);
    return js_console_write(ctx, this_val, argc > 1 ? argc - 1 : 0,
        argv + 1, 1);
}

static const struct {
    const char* name;
    int magic;
} js_console_methods[] = {
    { "log", 0 },
    { "info", 0 },
    { "debug", 0 },
    { "trace", 1 },
    { "warn", 1 },
    { "error", 1 },
};

static JSValue js_std_queueMicrotask_job(JSContext* ctx,
    int argc, JSValueConst* argv)
{
    return JS_Call(ctx, argv[0], JS_UNDEFINED, 0, NULL);
}

static JSValue js_std_queueMicrotask(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    if (!JS_IsFunction(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "not a function");
    if (JS_EnqueueJob(ctx, js_std_queueMicrotask_job, 1, &argv[0]))
        return JS_EXCEPTION;
    return JS_UNDEFINED;
}

static JSClassID js_textencoder_class_id;
static JSClassID js_textdecoder_class_id;

typedef struct {
    uint8_t brand;
} JSTextEncoder;

#define JS_TEXTDECODER_WHOLE_MIN 32

typedef struct {
    int bytes_needed;
    int bytes_seen;
    uint32_t codepoint;
    uint8_t lower, upper;
} JSUtf8DecodeState;

static void js_utf8_decode_state_init(JSUtf8DecodeState* st)
{
    memset(st, 0, sizeof(*st));
    st->lower = 0x80;
    st->upper = 0xBF;
}

typedef struct {
    BOOL fatal;
    BOOL ignore_bom;
    JSUtf8DecodeState st;
    BOOL stream_active;
    BOOL bom_handled;
} JSTextDecoder;

static const uint8_t utf8_replacement[3] = { 0xEF, 0xBF, 0xBD };
static const uint8_t utf8_bom[3] = { 0xEF, 0xBB, 0xBF };

static void js_textencoder_finalizer(JSRuntime* rt, JSValue val)
{
    JSTextEncoder* te = JS_GetOpaque(val, js_textencoder_class_id);
    if (te)
        js_free_rt(rt, te);
}

static void js_textdecoder_finalizer(JSRuntime* rt, JSValue val)
{
    JSTextDecoder* td = JS_GetOpaque(val, js_textdecoder_class_id);
    if (td)
        js_free_rt(rt, td);
}

static JSClassDef js_textencoder_class = {
    "TextEncoder",
    .finalizer = js_textencoder_finalizer,
};

static JSClassDef js_textdecoder_class = {
    "TextDecoder",
    .finalizer = js_textdecoder_finalizer,
};

static BOOL js_textdecoder_label_is_utf8(const char* s)
{
    char buf[32];
    size_t start = 0, len, i;

    while (s[start] == 0x09 || s[start] == 0x0A || s[start] == 0x0C || s[start] == 0x0D || s[start] == 0x20)
        start++;
    len = strlen(s + start);
    while (len > 0) {
        char c = s[start + len - 1];
        if (c == 0x09 || c == 0x0A || c == 0x0C || c == 0x0D || c == 0x20)
            len--;
        else
            break;
    }
    if (len >= sizeof(buf))
        return FALSE;
    for (i = 0; i < len; i++) {
        char c = s[start + i];
        if (c >= 'A' && c <= 'Z')
            c += 'a' - 'A';
        buf[i] = c;
    }
    buf[len] = '\0';
    return !strcmp(buf, "utf-8") || !strcmp(buf, "utf8") || !strcmp(buf, "unicode-1-1-utf-8");
}

static int js_textcodec_get_bytes(JSContext* ctx, JSValueConst obj,
    uint8_t** pbuf, size_t* plen)
{
    JSValue buffer;
    uint8_t *ptr, *out;
    size_t offset, length, abuf_size;

    *pbuf = NULL;
    *plen = 0;

    if ((JS_GetBufferKind(obj) == JS_BUFFER_KIND_BUFFER ? JS_GetArrayBuffer(ctx, &abuf_size, obj) : NULL)) {
        buffer = JS_DupValue(ctx, obj);
        offset = 0;
        length = abuf_size;
    } else {
        JS_FreeValue(ctx, JS_GetException(ctx));
        buffer = JS_GetBufferKind(obj) == JS_BUFFER_KIND_VIEW ? JS_GetArrayBufferView(ctx, obj, &offset, &length, NULL) : JS_EXCEPTION;
        if (JS_IsException(buffer)) {
            JSValue v;
            int64_t v_off, v_len;
            JS_FreeValue(ctx, JS_GetException(ctx));
            buffer = JS_GetPropertyStr(ctx, obj, "buffer");
            if (JS_IsException(buffer))
                return -1;
            if (!JS_GetArrayBuffer(ctx, &abuf_size, buffer)) {
                JS_FreeValue(ctx, JS_GetException(ctx));
                JS_FreeValue(ctx, buffer);
                JS_ThrowTypeError(ctx, "decode() argument is not a BufferSource");
                return -1;
            }
            v = JS_GetPropertyStr(ctx, obj, "byteOffset");
            if (JS_ToInt64(ctx, &v_off, v)) {
                JS_FreeValue(ctx, v);
                JS_FreeValue(ctx, buffer);
                return -1;
            }
            JS_FreeValue(ctx, v);
            v = JS_GetPropertyStr(ctx, obj, "byteLength");
            if (JS_ToInt64(ctx, &v_len, v)) {
                JS_FreeValue(ctx, v);
                JS_FreeValue(ctx, buffer);
                return -1;
            }
            JS_FreeValue(ctx, v);
            offset = (v_off > 0) ? (size_t)v_off : 0;
            length = (v_len > 0) ? (size_t)v_len : 0;
        }
    }

    ptr = JS_GetArrayBuffer(ctx, &abuf_size, buffer);
    if (!ptr) {
        JS_FreeValue(ctx, buffer);
        return -1;
    }
    if (offset > abuf_size || length > abuf_size - offset) {
        JS_FreeValue(ctx, buffer);
        JS_ThrowRangeError(ctx, "buffer source out of bounds");
        return -1;
    }
    out = js_malloc(ctx, length + 1);
    if (!out) {
        JS_FreeValue(ctx, buffer);
        return -1;
    }
    memcpy(out, ptr + offset, length);
    JS_FreeValue(ctx, buffer);
    *pbuf = out;
    *plen = length;
    return 0;
}

static int js_textcodec_check_options(JSContext* ctx, JSValueConst opts,
    const char* const* valid,
    const char* valid_desc)
{
    JSPropertyEnum* props;
    uint32_t nprops, i;
    int j, ret = 0;

    if (JS_GetOwnPropertyNames(ctx, &props, &nprops, opts,
            JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY)
        < 0)
        return -1;
    for (i = 0; i < nprops; i++) {
        const char* key = JS_AtomToCString(ctx, props[i].atom);
        if (!key) {
            ret = -1;
            break;
        }
        for (j = 0; valid[j]; j++) {
            if (!strcmp(key, valid[j]))
                break;
        }
        if (!valid[j]) {
            JS_ThrowTypeError(ctx, "unknown option \"%s\" (valid: %s)",
                key, valid_desc);
            ret = -1;
        }
        JS_FreeCString(ctx, key);
        if (ret)
            break;
    }
    for (i = 0; i < nprops; i++)
        JS_FreeAtom(ctx, props[i].atom);
    js_free(ctx, props);
    return ret;
}

static int js_utf8_decode_whatwg(JSContext* ctx, DynBuf* dbuf,
    const uint8_t* buf, size_t len, BOOL fatal,
    JSUtf8DecodeState* st, BOOL final)
{
    size_t i = 0;
    uint32_t codepoint = st->codepoint;
    int bytes_needed = st->bytes_needed, bytes_seen = st->bytes_seen;
    uint8_t lower = st->lower, upper = st->upper;
    uint8_t cbuf[UTF8_CHAR_LEN_MAX];

    while (i < len) {
        uint8_t b = buf[i];
        if (bytes_needed == 0) {
            if (b <= 0x7F) {
                dbuf_putc(dbuf, b);
                i++;
            } else if (b >= 0xC2 && b <= 0xDF) {
                bytes_needed = 1;
                codepoint = b & 0x1F;
                i++;
            } else if (b >= 0xE0 && b <= 0xEF) {
                if (b == 0xE0)
                    lower = 0xA0;
                if (b == 0xED)
                    upper = 0x9F;
                bytes_needed = 2;
                codepoint = b & 0x0F;
                i++;
            } else if (b >= 0xF0 && b <= 0xF4) {
                if (b == 0xF0)
                    lower = 0x90;
                if (b == 0xF4)
                    upper = 0x8F;
                bytes_needed = 3;
                codepoint = b & 0x07;
                i++;
            } else {
                if (fatal)
                    goto fatal_error;
                dbuf_put(dbuf, utf8_replacement, 3);
                i++;
            }
        } else if (b < lower || b > upper) {
            codepoint = 0;
            bytes_needed = 0;
            bytes_seen = 0;
            lower = 0x80;
            upper = 0xBF;
            if (fatal)
                goto fatal_error;
            dbuf_put(dbuf, utf8_replacement, 3);
        } else {
            lower = 0x80;
            upper = 0xBF;
            codepoint = (codepoint << 6) | (b & 0x3F);
            bytes_seen++;
            i++;
            if (bytes_seen == bytes_needed) {
                dbuf_put(dbuf, cbuf, unicode_to_utf8(cbuf, codepoint));
                codepoint = 0;
                bytes_needed = 0;
                bytes_seen = 0;
            }
        }
    }
    if (bytes_needed != 0) {
        if (final) {
            if (fatal)
                goto fatal_error;
            dbuf_put(dbuf, utf8_replacement, 3);
            bytes_needed = 0;
            bytes_seen = 0;
            codepoint = 0;
            lower = 0x80;
            upper = 0xBF;
        }
    }
    st->bytes_needed = bytes_needed;
    st->bytes_seen = bytes_seen;
    st->codepoint = codepoint;
    st->lower = lower;
    st->upper = upper;
    if (dbuf->error) {
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    return 0;
fatal_error:
    JS_ThrowTypeError(ctx, "invalid UTF-8 sequence");
    return -1;
}

static JSValue js_textencoder_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    JSValue obj = JS_UNDEFINED, proto;
    JSTextEncoder* te;

    if (JS_IsUndefined(new_target)) {
        proto = JS_GetClassProto(ctx, js_textencoder_class_id);
    } else {
        proto = JS_GetPropertyStr(ctx, new_target, "prototype");
        if (JS_IsException(proto))
            goto fail;
    }
    obj = JS_NewObjectProtoClass(ctx, proto, js_textencoder_class_id);
    JS_FreeValue(ctx, proto);
    if (JS_IsException(obj))
        goto fail;
    te = js_mallocz(ctx, sizeof(*te));
    if (!te)
        goto fail;
    JS_SetOpaque(obj, te);
    return obj;
fail:
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
}

static JSValue js_textencoder_get_encoding(JSContext* ctx, JSValueConst this_val)
{
    if (!JS_GetOpaque2(ctx, this_val, js_textencoder_class_id))
        return JS_EXCEPTION;
    return JS_NewString(ctx, "utf-8");
}

static char* dyn_wtf8_lone_surrogates_to_fffd(const char* s, size_t n, size_t* pn)
{
    size_t i = 0;
    int found = 0;
    char* out;

    while (i < n) {
        uint8_t c = (uint8_t)s[i];
        if (c == 0xED && i + 2 < n && (uint8_t)s[i + 1] >= 0xA0 && (uint8_t)s[i + 1] <= 0xBF
            && ((uint8_t)s[i + 2] & 0xC0) == 0x80) {
            found = 1;
            break;
        }
        i += (c < 0x80) ? 1 : (c < 0xE0) ? 2
            : (c < 0xF0)                 ? 3
                                         : 4;
    }
    if (!found)
        return NULL;
    out = (char*)malloc(n ? n : 1);
    if (!out)
        return NULL;
    memcpy(out, s, n);
    i = 0;
    while (i < n) {
        uint8_t c = (uint8_t)out[i];
        if (c == 0xED && i + 2 < n && (uint8_t)out[i + 1] >= 0xA0 && (uint8_t)out[i + 1] <= 0xBF
            && ((uint8_t)out[i + 2] & 0xC0) == 0x80) {
            out[i] = (char)0xEF;
            out[i + 1] = (char)0xBF;
            out[i + 2] = (char)0xBD;
            i += 3;
            continue;
        }
        i += (c < 0x80) ? 1 : (c < 0xE0) ? 2
            : (c < 0xF0)                 ? 3
                                         : 4;
    }
    *pn = n;
    return out;
}

static JSValue js_textencoder_encode(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char* str = NULL;
    size_t len = 0;
    JSValue ab, u8;
    JSValueConst args[3];

    if (!JS_GetOpaque2(ctx, this_val, js_textencoder_class_id))
        return JS_EXCEPTION;
    if (argc >= 1 && !JS_IsUndefined(argv[0])) {
        str = JS_ToCStringLen(ctx, &len, argv[0]);
        if (!str)
            return JS_EXCEPTION;
    }
    {
        size_t flen = len;
        char* fixed = str ? dyn_wtf8_lone_surrogates_to_fffd(str, len, &flen) : NULL;
        ab = JS_NewArrayBufferCopy(ctx,
            (const uint8_t*)(fixed ? fixed : (str ? str : "")),
            fixed ? flen : len);
        if (fixed)
            free(fixed);
        if (str)
            JS_FreeCString(ctx, str);
    }
    if (JS_IsException(ab))
        return JS_EXCEPTION;
    args[0] = ab;
    args[1] = JS_UNDEFINED;
    args[2] = JS_UNDEFINED;
    u8 = JS_NewTypedArray(ctx, 3, args, JS_TYPED_ARRAY_UINT8);
    JS_FreeValue(ctx, ab);
    return u8;
}

static JSValue js_textencoder_encodeInto(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char* src;
    size_t src_len, offset, dst_len, abuf_size, bpe, si = 0, di = 0;
    int64_t read = 0, written = 0;
    uint8_t *abuf, *dst;
    JSValue buffer, res;

    if (!JS_GetOpaque2(ctx, this_val, js_textencoder_class_id))
        return JS_EXCEPTION;
    src = JS_ToCStringLen(ctx, &src_len, argc >= 1 ? argv[0] : JS_UNDEFINED);
    if (!src)
        return JS_EXCEPTION;
    buffer = JS_GetTypedArrayBuffer(ctx, argc >= 2 ? argv[1] : JS_UNDEFINED,
        &offset, &dst_len, &bpe);
    if (JS_IsException(buffer)) {
        JS_FreeCString(ctx, src);
        return JS_EXCEPTION;
    }
    if (bpe != 1) {
        JS_FreeValue(ctx, buffer);
        JS_FreeCString(ctx, src);
        return JS_ThrowTypeError(ctx, "encodeInto() destination must be a Uint8Array");
    }
    abuf = JS_GetArrayBuffer(ctx, &abuf_size, buffer);
    if (!abuf) {
        JS_FreeValue(ctx, buffer);
        JS_FreeCString(ctx, src);
        return JS_EXCEPTION;
    }
    if (offset > abuf_size || dst_len > abuf_size - offset) {
        JS_FreeValue(ctx, buffer);
        JS_FreeCString(ctx, src);
        return JS_ThrowRangeError(ctx, "destination out of bounds");
    }
    dst = abuf + offset;
    while (si < src_len) {
        const uint8_t* p = (const uint8_t*)src + si;
        const uint8_t* pnext;
        int max_len = (src_len - si) > UTF8_CHAR_LEN_MAX ? UTF8_CHAR_LEN_MAX : (int)(src_len - si);
        int c = unicode_from_utf8(p, max_len, &pnext);
        int clen;
        int lone_surrogate = (c >= 0xD800 && c <= 0xDFFF);
        if (c < 0) {
            c = 0xFFFD;
            clen = 1;
        } else if (lone_surrogate) {
            clen = 3;
        } else {
            clen = (int)(pnext - p);
        }
        if (di + (size_t)clen > dst_len)
            break;
        if (lone_surrogate) {
            dst[di] = (uint8_t)0xEF;
            dst[di + 1] = (uint8_t)0xBF;
            dst[di + 2] = (uint8_t)0xBD;
        } else {
            memcpy(dst + di, p, clen);
        }
        di += clen;
        si += clen;
        written += clen;
        read += (c >= 0x10000) ? 2 : 1;
    }
    JS_FreeValue(ctx, buffer);
    JS_FreeCString(ctx, src);

    res = JS_NewObject(ctx);
    if (JS_IsException(res))
        return JS_EXCEPTION;
    JS_DefinePropertyValueStr(ctx, res, "read", JS_NewInt64(ctx, read), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, res, "written", JS_NewInt64(ctx, written), JS_PROP_C_W_E);
    return res;
}

static JSValue js_textdecoder_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    JSValue obj = JS_UNDEFINED, proto;
    JSTextDecoder* td;
    BOOL fatal = FALSE, ignore_bom = FALSE;

    if (argc >= 1 && !JS_IsUndefined(argv[0])) {
        const char* label = JS_ToCString(ctx, argv[0]);
        BOOL ok;
        if (!label)
            return JS_EXCEPTION;
        ok = js_textdecoder_label_is_utf8(label);
        JS_FreeCString(ctx, label);
        if (!ok)
            return JS_ThrowRangeError(ctx, "unsupported encoding label");
    }
    if (argc >= 2 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
        static const char* const valid[] = { "fatal", "ignoreBOM", NULL };
        JSValue v;
        int b;
        if (!JS_IsObject(argv[1]))
            return JS_ThrowTypeError(ctx, "TextDecoder options must be an object");
        if (js_textcodec_check_options(ctx, argv[1], valid,
                "fatal, ignoreBOM"))
            return JS_EXCEPTION;
        v = JS_GetPropertyStr(ctx, argv[1], "fatal");
        if (JS_IsException(v))
            return JS_EXCEPTION;
        b = JS_ToBool(ctx, v);
        JS_FreeValue(ctx, v);
        if (b < 0)
            return JS_EXCEPTION;
        fatal = b;
        v = JS_GetPropertyStr(ctx, argv[1], "ignoreBOM");
        if (JS_IsException(v))
            return JS_EXCEPTION;
        b = JS_ToBool(ctx, v);
        JS_FreeValue(ctx, v);
        if (b < 0)
            return JS_EXCEPTION;
        ignore_bom = b;
    }

    if (JS_IsUndefined(new_target)) {
        proto = JS_GetClassProto(ctx, js_textdecoder_class_id);
    } else {
        proto = JS_GetPropertyStr(ctx, new_target, "prototype");
        if (JS_IsException(proto))
            goto fail;
    }
    obj = JS_NewObjectProtoClass(ctx, proto, js_textdecoder_class_id);
    JS_FreeValue(ctx, proto);
    if (JS_IsException(obj))
        goto fail;
    td = js_mallocz(ctx, sizeof(*td));
    if (!td)
        goto fail;
    td->fatal = fatal;
    td->ignore_bom = ignore_bom;
    js_utf8_decode_state_init(&td->st);
    JS_SetOpaque(obj, td);
    return obj;
fail:
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
}

static JSValue js_textdecoder_get_encoding(JSContext* ctx, JSValueConst this_val)
{
    if (!JS_GetOpaque2(ctx, this_val, js_textdecoder_class_id))
        return JS_EXCEPTION;
    return JS_NewString(ctx, "utf-8");
}

static JSValue js_textdecoder_get_fatal(JSContext* ctx, JSValueConst this_val)
{
    JSTextDecoder* td = JS_GetOpaque2(ctx, this_val, js_textdecoder_class_id);
    if (!td)
        return JS_EXCEPTION;
    return JS_NewBool(ctx, td->fatal);
}

static JSValue js_textdecoder_get_ignoreBOM(JSContext* ctx, JSValueConst this_val)
{
    JSTextDecoder* td = JS_GetOpaque2(ctx, this_val, js_textdecoder_class_id);
    if (!td)
        return JS_EXCEPTION;
    return JS_NewBool(ctx, td->ignore_bom);
}

static JSValue js_textdecoder_decode(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSTextDecoder* td = JS_GetOpaque2(ctx, this_val, js_textdecoder_class_id);
    BOOL fatal, ignore_bom, stream = FALSE, was_active;
    const uint8_t* p;
    uint8_t* buf;
    size_t len, n;
    DynBuf dbuf;
    JSValue ret;

    if (!td)
        return JS_EXCEPTION;
    fatal = td->fatal;
    ignore_bom = td->ignore_bom;

    if (argc >= 2 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
        static const char* const valid[] = { "stream", NULL };
        JSValue v;
        int b;
        if (!JS_IsObject(argv[1]))
            return JS_ThrowTypeError(ctx, "decode() options must be an object");
        if (js_textcodec_check_options(ctx, argv[1], valid, "stream"))
            return JS_EXCEPTION;
        v = JS_GetPropertyStr(ctx, argv[1], "stream");
        if (JS_IsException(v))
            return JS_EXCEPTION;
        if (!JS_IsUndefined(v) && !JS_IsNull(v) && !JS_IsBool(v)) {
            JS_FreeValue(ctx, v);
            return JS_ThrowTypeError(ctx,
                "option \"stream\" must be a boolean");
        }
        b = JS_ToBool(ctx, v);
        JS_FreeValue(ctx, v);
        if (b < 0)
            return JS_EXCEPTION;
        stream = b;
    }

    if (argc < 1 || JS_IsUndefined(argv[0])) {
        if (!stream && !td->stream_active)
            return JS_NewStringLen(ctx, "", 0);
        buf = NULL;
        len = 0;
    } else {
        if (js_textcodec_get_bytes(ctx, argv[0], &buf, &len))
            return JS_EXCEPTION;
    }

    was_active = td->stream_active;
    p = buf;
    n = len;
    if (!was_active) {
        if (!ignore_bom && n >= 3 && !memcmp(p, utf8_bom, 3)) {
            p += 3;
            n -= 3;
            td->bom_handled = TRUE;
        }
    }
    if (n >= JS_TEXTDECODER_WHOLE_MIN && td->st.bytes_needed == 0 && (!was_active || td->bom_handled)
        && simd.validate_utf8(p, n) == n) {
        td->bom_handled = TRUE;
        ret = JS_NewStringLen(ctx, (const char*)p, n);
        js_free(ctx, buf);
    } else {
        js_std_dbuf_init(ctx, &dbuf);
        if (js_utf8_decode_whatwg(ctx, &dbuf, p, n, fatal, &td->st, !stream)) {
            dbuf_free(&dbuf);
            js_free(ctx, buf);
            js_utf8_decode_state_init(&td->st);
            td->stream_active = FALSE;
            td->bom_handled = FALSE;
            return JS_EXCEPTION;
        }
        if (!ignore_bom && was_active && !td->bom_handled && dbuf.size >= 3 && !memcmp(dbuf.buf, utf8_bom, 3)) {
            memmove(dbuf.buf, dbuf.buf + 3, dbuf.size - 3);
            dbuf.size -= 3;
        }
        if (dbuf.size > 0)
            td->bom_handled = TRUE;
        ret = JS_NewStringLen(ctx, (const char*)dbuf.buf, dbuf.size);
        dbuf_free(&dbuf);
        js_free(ctx, buf);
    }
    if (stream) {
        td->stream_active = TRUE;
    } else {
        js_utf8_decode_state_init(&td->st);
        td->stream_active = FALSE;
        td->bom_handled = FALSE;
    }
    return ret;
}

static const JSCFunctionListEntry js_textencoder_proto_funcs[] = {
    JS_CGETSET_DEF("encoding", js_textencoder_get_encoding, NULL),
    JS_CFUNC_DEF("encode", 1, js_textencoder_encode),
    JS_CFUNC_DEF("encodeInto", 2, js_textencoder_encodeInto),
};

static const JSCFunctionListEntry js_textdecoder_proto_funcs[] = {
    JS_CGETSET_DEF("encoding", js_textdecoder_get_encoding, NULL),
    JS_CGETSET_DEF("fatal", js_textdecoder_get_fatal, NULL),
    JS_CGETSET_DEF("ignoreBOM", js_textdecoder_get_ignoreBOM, NULL),
    JS_CFUNC_DEF("decode", 1, js_textdecoder_decode),
};

static void js_textcodec_install(JSContext* ctx, JSValueConst global_obj)
{
    JSRuntime* rt = JS_GetRuntime(ctx);
    JSValue proto, ctor;

    JS_NewClassID(&js_textencoder_class_id);
    JS_NewClass(rt, js_textencoder_class_id, &js_textencoder_class);
    proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, proto, js_textencoder_proto_funcs,
        countof(js_textencoder_proto_funcs));
    ctor = JS_NewCFunction2(ctx, js_textencoder_ctor, "TextEncoder", 0,
        JS_CFUNC_constructor, 0);
    JS_SetConstructor(ctx, ctor, proto);
    JS_SetClassProto(ctx, js_textencoder_class_id, proto);
    JS_DefinePropertyValueStr(ctx, global_obj, "TextEncoder", ctor, JS_PROP_C_W_E);

    JS_NewClassID(&js_textdecoder_class_id);
    JS_NewClass(rt, js_textdecoder_class_id, &js_textdecoder_class);
    proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, proto, js_textdecoder_proto_funcs,
        countof(js_textdecoder_proto_funcs));
    ctor = JS_NewCFunction2(ctx, js_textdecoder_ctor, "TextDecoder", 0,
        JS_CFUNC_constructor, 0);
    JS_SetConstructor(ctx, ctor, proto);
    JS_SetClassProto(ctx, js_textdecoder_class_id, proto);
    JS_DefinePropertyValueStr(ctx, global_obj, "TextDecoder", ctor, JS_PROP_C_W_E);
}

#include "dyna-fetch.inc.c"

int js_std_modules_enabled = 0;

static int64_t get_time_ms(void);

uint64_t js_std_exec_deadline_ms = 0;
uint64_t js_std_js_memory_limit = 0;

int js_std_interrupt_handler(JSRuntime* rt, void* opaque)
{
    (void)rt;
    (void)opaque;
    return js_std_exec_deadline_ms != 0
        && (uint64_t)get_time_ms() > js_std_exec_deadline_ms;
}

static int js_std_deadline_remaining_ms(void)
{
    int64_t left;
    if (js_std_exec_deadline_ms == 0)
        return -1;
    left = (int64_t)js_std_exec_deadline_ms - get_time_ms();
    if (left < 0)
        return 0;
    return left > INT32_MAX ? INT32_MAX : (int)left;
}

static int js_std_deadline_expired(void)
{
    return js_std_exec_deadline_ms != 0
        && (uint64_t)get_time_ms() > js_std_exec_deadline_ms;
}

#define JS_STD_HARD_STOP_EXIT_CODE 113
#define JS_STD_HARD_STOP_GRACE_MIN_MS 250
#define JS_STD_HARD_STOP_GRACE_MAX_MS 2000

static uint64_t js_std_hard_stop_grace_ms(uint64_t timeout_ms)
{
    uint64_t grace = timeout_ms / 4;
    if (grace < JS_STD_HARD_STOP_GRACE_MIN_MS)
        grace = JS_STD_HARD_STOP_GRACE_MIN_MS;
    if (grace > JS_STD_HARD_STOP_GRACE_MAX_MS)
        grace = JS_STD_HARD_STOP_GRACE_MAX_MS;
    return grace;
}

#if !defined(_WIN32)
static void* js_std_hard_stop_thread(void* opaque)
{
    static const char msg[] = "dyna: execution deadline exceeded and the script did not stop; terminating\n";
    uint64_t stop_at = (uint64_t)(uintptr_t)opaque;
    for (;;) {
        int64_t left = (int64_t)stop_at - get_time_ms();
        if (left <= 0)
            break;
        poll(NULL, 0, left > 1000 ? 1000 : (int)left);
    }
    if (write(2, msg, sizeof(msg) - 1) < 0) {
    }
    _exit(JS_STD_HARD_STOP_EXIT_CODE);
    return NULL;
}
#endif

void js_std_arm_exec_deadline(uint64_t timeout_ms)
{
    js_std_exec_deadline_ms = (uint64_t)get_time_ms() + timeout_ms;
#if !defined(_WIN32)
    {
        pthread_t tid;
        pthread_attr_t attr;
        uint64_t stop_at = js_std_exec_deadline_ms + js_std_hard_stop_grace_ms(timeout_ms);
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&tid, &attr, js_std_hard_stop_thread, (void*)(uintptr_t)stop_at) != 0) {
            fprintf(stderr, "dyna: cannot start the --timeout-ms watchdog thread\n");
            exit(2);
        }
        pthread_attr_destroy(&attr);
    }
#endif
}

void js_std_apply_budgets(JSRuntime* rt)
{
    if (!rt)
        return;
    if (js_std_exec_deadline_ms)
        JS_SetInterruptHandler(rt, js_std_interrupt_handler, NULL);
    if (js_std_js_memory_limit)
        JS_SetMemoryLimit(rt, js_std_js_memory_limit);
}

static JSValue js_global_btoa(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    static const char enc[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t len, i, o;
    char* out;
    JSValue res;

    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "btoa: a string is required");
    const char* s = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!s)
        return JS_EXCEPTION;
    {
        unsigned char* bytes = (unsigned char*)malloc(len + 1);
        size_t bi = 0;
        if (!bytes) {
            JS_FreeCString(ctx, s);
            return JS_ThrowOutOfMemory(ctx);
        }
        for (i = 0; i < len;) {
            unsigned char c = (unsigned char)s[i];
            uint32_t cp;
            size_t adv;
            if (c < 0x80) {
                cp = c;
                adv = 1;
            } else if ((c & 0xe0) == 0xc0 && i + 1 < len
                && ((unsigned char)s[i + 1] & 0xc0) == 0x80) {
                cp = ((uint32_t)(c & 0x1f) << 6)
                    | ((unsigned char)s[i + 1] & 0x3f);
                adv = 2;
            } else if ((c & 0xf0) == 0xe0 && i + 2 < len
                && ((unsigned char)s[i + 1] & 0xc0) == 0x80
                && ((unsigned char)s[i + 2] & 0xc0) == 0x80) {
                cp = ((uint32_t)(c & 0x0f) << 12)
                    | (((unsigned char)s[i + 1] & 0x3f) << 6)
                    | ((unsigned char)s[i + 2] & 0x3f);
                adv = 3;
            } else {
                cp = 0x110000;
                adv = 1;
            }
            if (cp > 0xff) {
                free(bytes);
                JS_FreeCString(ctx, s);
                return JS_ThrowTypeError(ctx,
                    "btoa: string contains characters outside Latin-1");
            }
            bytes[bi++] = (unsigned char)cp;
            i += adv;
        }
        out = (char*)malloc(bi / 3 * 4 + 5);
        if (!out) {
            free(bytes);
            JS_FreeCString(ctx, s);
            return JS_ThrowOutOfMemory(ctx);
        }
        {
            size_t src_len = bi;
            const unsigned char* bs = bytes;
            o = 0;
            for (i = 0; i + 2 < src_len; i += 3) {
                uint32_t v = ((uint32_t)bs[i] << 16)
                    | ((uint32_t)bs[i + 1] << 8)
                    | (uint32_t)bs[i + 2];
                out[o++] = enc[(v >> 18) & 63];
                out[o++] = enc[(v >> 12) & 63];
                out[o++] = enc[(v >> 6) & 63];
                out[o++] = enc[v & 63];
            }
            if (src_len - i == 1) {
                uint32_t v = (uint32_t)bs[i] << 16;
                out[o++] = enc[(v >> 18) & 63];
                out[o++] = enc[(v >> 12) & 63];
                out[o++] = '=';
                out[o++] = '=';
            } else if (src_len - i == 2) {
                uint32_t v = ((uint32_t)bs[i] << 16)
                    | ((uint32_t)bs[i + 1] << 8);
                out[o++] = enc[(v >> 18) & 63];
                out[o++] = enc[(v >> 12) & 63];
                out[o++] = enc[(v >> 6) & 63];
                out[o++] = '=';
            }
            free(bytes);
            JS_FreeCString(ctx, s);
            res = JS_NewStringLen(ctx, out, o);
            free(out);
            return res;
        }
    }
}

static JSValue js_global_atob(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    static int rev_init = 0;
    static int8_t rev[256];
    size_t len, i, o, npad, need;
    uint32_t acc = 0;
    int nbits = 0;
    uint8_t *raw, *out;

    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "atob: a string is required");
    const char* s = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!s)
        return JS_EXCEPTION;
    if (!rev_init) {
        int j;
        memset(rev, -1, sizeof rev);
        for (j = 0; j < 26; j++) {
            rev['A' + j] = (int8_t)j;
            rev['a' + j] = (int8_t)(j + 26);
        }
        for (j = 0; j < 10; j++)
            rev['0' + j] = (int8_t)(j + 52);
        rev['+'] = 62;
        rev['/'] = 63;
        rev_init = 1;
    }
    if (len / 2 > (SIZE_MAX - 2) / 3) {
        JS_FreeCString(ctx, s);
        return JS_ThrowOutOfMemory(ctx);
    }
    raw = (uint8_t*)malloc(len + 1);
    need = len / 2 * 3 + 2;
    out = (uint8_t*)malloc(need);
    if (!raw || !out) {
        free(raw);
        free(out);
        JS_FreeCString(ctx, s);
        return JS_ThrowOutOfMemory(ctx);
    }
    o = 0;
    npad = 0;
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        int8_t d;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == 0x0c)
            continue;
        if (c == '=') {
            npad++;
            continue;
        }
        if (npad) {
        bad:
            free(raw);
            free(out);
            JS_FreeCString(ctx, s);
            return JS_ThrowTypeError(ctx,
                "atob: invalid character in base64 data");
        }
        d = rev[c];
        if (d < 0)
            goto bad;
        acc = (acc << 6) | (uint32_t)d;
        nbits += 6;
        if (nbits >= 8) {
            nbits -= 8;
            raw[o++] = (uint8_t)((acc >> nbits) & 0xff);
        }
    }
    if (nbits >= 6 || npad > 2) {
        free(raw);
        free(out);
        JS_FreeCString(ctx, s);
        return JS_ThrowTypeError(ctx, "atob: base64 length is a non-multiple "
                                      "of 4 characters");
    }
    JS_FreeCString(ctx, s);
    {
        size_t uo = 0;
        for (i = 0; i < o; i++) {
            if (raw[i] < 0x80) {
                out[uo++] = raw[i];
            } else {
                out[uo++] = (uint8_t)(0xc2 | (raw[i] >> 6));
                out[uo++] = (uint8_t)(0x80 | (raw[i] & 0x3f));
            }
        }
        JSValue res = JS_NewStringLen(ctx, (const char*)out, uo);
        free(raw);
        free(out);
        return res;
    }
}

#if defined(CONFIG_NATIVE_MODULE_URL) || defined(CONFIG_NATIVE_MODULE_VSERIALIZE) || defined(CONFIG_NATIVE_MODULE_NET)
static void dyn_global_install_from_module(JSContext* ctx,
    const char* module,
    const char* holder,
    const char* const* pairs,
    int npairs)
{
    char src[192];
    JSValue r, v, g, hobj;
    JSPromiseStateEnum st;
    int i;

    snprintf(src, sizeof src,
        "import * as m from \"%s\";\n"
        "globalThis.%s = m;\n",
        module, holder);
    r = JS_Eval(ctx, src, strlen(src), "<dyna:globals>",
        JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(r))
        goto skip_exc;
    v = JS_EvalFunction(ctx, r);
    if (JS_IsException(v))
        goto skip_exc;
    st = JS_PromiseState(ctx, v);
    if (st == JS_PROMISE_REJECTED) {
        JSValue err = JS_PromiseResult(ctx, v);
        JS_FreeValue(ctx, v);
        JS_FreeValue(ctx, err);
        return;
    }
    g = JS_GetGlobalObject(ctx);
    hobj = JS_GetPropertyStr(ctx, g, holder);
    if (JS_IsObject(hobj)) {
        for (i = 0; i + 1 < 2 * npairs; i += 2) {
            JSValue cv = JS_GetPropertyStr(ctx, hobj, pairs[i]);
            if (!JS_IsUndefined(cv))
                JS_DefinePropertyValueStr(ctx, g, pairs[i + 1], cv, JS_PROP_C_W_E);
            else
                JS_FreeValue(ctx, cv);
        }
        {
            JSAtom hatom = JS_NewAtom(ctx, holder);
            JS_DeleteProperty(ctx, g, hatom, 0);
            JS_FreeAtom(ctx, hatom);
        }
    }
    JS_FreeValue(ctx, hobj);
    JS_FreeValue(ctx, g);
    JS_FreeValue(ctx, v);
    if (JS_HasException(ctx)) {
        JSValue xerr = JS_GetException(ctx);
        JS_FreeValue(ctx, xerr);
    }
    return;
skip_exc:
    {
        JSValue xerr = JS_GetException(ctx);
        JS_FreeValue(ctx, xerr);
    }
}
#endif

void js_std_add_helpers(JSContext* ctx, int argc, char** argv)
{
    JSValue global_obj, console, args, performance;
    int i;

    global_obj = JS_GetGlobalObject(ctx);

    console = JS_NewObject(ctx);
    for (i = 0; i < (int)countof(js_console_methods); i++) {
        JS_DefinePropertyValueStr(ctx, console, js_console_methods[i].name,
            JS_NewCFunctionMagic(ctx, js_console_write,
                js_console_methods[i].name, 1,
                JS_CFUNC_generic_magic,
                js_console_methods[i].magic), JS_PROP_C_W_E);
    }
    JS_DefinePropertyValueStr(ctx, console, "assert",
        JS_NewCFunction(ctx, js_console_assert, "assert", 2), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, global_obj, "console", console, JS_PROP_C_W_E);

    atomic_store_explicit(&os_poll_func, js_os_poll, memory_order_relaxed);
    JS_DefinePropertyValueStr(ctx, global_obj, "setTimeout",
        JS_NewCFunctionMagic(ctx, js_os_setTimeout, "setTimeout",
            2, JS_CFUNC_generic_magic, 0), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, global_obj, "setInterval",
        JS_NewCFunctionMagic(ctx, js_os_setTimeout, "setInterval",
            2, JS_CFUNC_generic_magic, 1), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, global_obj, "clearTimeout",
        JS_NewCFunction(ctx, js_os_clearTimeout, "clearTimeout", 1), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, global_obj, "clearInterval",
        JS_NewCFunction(ctx, js_os_clearTimeout, "clearInterval", 1), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, global_obj, "queueMicrotask",
        JS_NewCFunction(ctx, js_std_queueMicrotask, "queueMicrotask", 1), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, global_obj, "sleep",
        JS_NewCFunction(ctx, js_os_sleepAsync, "sleep", 1), JS_PROP_C_W_E);

    performance = JS_NewObject(ctx);
    JS_DefinePropertyValueStr(ctx, performance, "now",
        JS_NewCFunction(ctx, js_os_now, "now", 0), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, global_obj, "performance", performance, JS_PROP_C_W_E);

    if (argc >= 0) {
        args = JS_NewArray(ctx);
        for (i = 0; i < argc; i++) {
            JS_DefinePropertyValueUint32(ctx, args, i, JS_NewString(ctx, argv[i]), JS_PROP_C_W_E);
        }
        JS_DefinePropertyValueStr(ctx, global_obj, "scriptArgs", args, JS_PROP_C_W_E);
    }

    JS_DefinePropertyValueStr(ctx, global_obj, "print",
        JS_NewCFunction(ctx, js_print, "print", 1), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, global_obj, "__loadScript",
        JS_NewCFunction(ctx, js_loadScript, "__loadScript", 1), JS_PROP_C_W_E);

    js_textcodec_install(ctx, global_obj);
    js_fetch_install(ctx, global_obj);
    JS_DefinePropertyValueStr(ctx, global_obj, "atob",
        JS_NewCFunction(ctx, js_global_atob, "atob", 1), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, global_obj, "btoa",
        JS_NewCFunction(ctx, js_global_btoa, "btoa", 1), JS_PROP_C_W_E);

#if defined(CONFIG_NATIVE_MODULES)
#if defined(CONFIG_NATIVE_MODULE_URL)
    {
        static const char* const url_pairs[] = {
            "URL",
            "URL",
            "URLSearchParams",
            "URLSearchParams",
        };
        dyn_global_install_from_module(ctx, "dyna:url", "__dyna_url_ns",
            url_pairs, 2);
    }
#endif
#if defined(CONFIG_NATIVE_MODULE_VSERIALIZE)
    {
        static const char* const sc_pairs[] = {
            "structuredClone",
            "structuredClone",
        };
        dyn_global_install_from_module(ctx, "dyna:serialize", "__dyna_sc_ns",
            sc_pairs, 1);
    }
#endif
#if defined(CONFIG_NATIVE_MODULE_NET)
    {
        static const char* const ws_pairs[] = {
            "WsClient",
            "WebSocket",
        };
        dyn_global_install_from_module(ctx, "dyna:http", "__dyna_ws_ns",
            ws_pairs, 1);
    }
#endif
#endif

    JS_FreeValue(ctx, global_obj);
}

static _Thread_local int std_handlers_freed;

void js_std_init_handlers(JSRuntime* rt)
{
    JSThreadState* ts;

    std_handlers_freed = 0;

    signal(SIGPIPE, SIG_IGN);

    ts = malloc(sizeof(*ts));
    if (!ts) {
        fprintf(stderr, "Could not allocate memory for the worker");
        exit(1);
    }
    memset(ts, 0, sizeof(*ts));
    init_list_head(&ts->os_rw_handlers);
    init_list_head(&ts->os_signal_handlers);
    init_list_head(&ts->port_list);
    init_list_head(&ts->worker_obj_list);
    init_list_head(&ts->rejected_promise_list);
    ts->next_timer_id = 1;
    ts->io_reactor_fd = -1;

    JS_SetRuntimeOpaque(rt, ts);

#ifdef USE_WORKER
    {
        JSSharedArrayBufferFunctions sf;
        memset(&sf, 0, sizeof(sf));
        sf.sab_alloc = js_sab_alloc;
        sf.sab_free = js_sab_free;
        sf.sab_dup = js_sab_dup;
        sf.sab_valid_ptr = js_sab_valid_ptr;
        JS_SetSharedArrayBufferFunctions(rt, &sf);
    }
#endif
}

void js_std_free_handlers(JSRuntime* rt)
{
    JSThreadState* ts = JS_GetRuntimeOpaque(rt);
    struct list_head *el, *el1;

    std_handlers_freed = 1;

    list_for_each_safe(el, el1, &ts->os_rw_handlers)
    {
        JSOSRWHandler* rh = list_entry(el, JSOSRWHandler, link);
        free_rw_handler(rt, rh);
    }

    list_for_each_safe(el, el1, &ts->os_signal_handlers)
    {
        JSOSSignalHandler* sh = list_entry(el, JSOSSignalHandler, link);
        free_sh(rt, sh);
    }

    while (ts->n_timers > 0)
        free_timer(rt, ts->timer_heap[0]);
    js_free_rt(rt, ts->timer_heap);
    js_free_rt(rt, ts->timer_ids);
    ts->timer_heap = NULL;
    ts->timer_ids = NULL;
    ts->timer_heap_cap = ts->timer_ids_cap = 0;

    list_for_each_safe(el, el1, &ts->rejected_promise_list)
    {
        JSRejectedPromiseEntry* rp = list_entry(el, JSRejectedPromiseEntry, link);
        JS_FreeValueRT(rt, rp->promise);
        JS_FreeValueRT(rt, rp->reason);
        free(rp);
    }

#ifdef USE_WORKER
    js_release_worker_refs(rt, ts);

    js_free_message_pipe(ts->recv_pipe);
    js_free_message_pipe(ts->send_pipe);

    list_for_each_safe(el, el1, &ts->port_list)
    {
        JSWorkerMessageHandler* port = list_entry(el, JSWorkerMessageHandler, link);
        port->link.prev = NULL;
        port->link.next = NULL;
    }
#endif

#if !defined(_WIN32)
    free(ts->poll_fds);
#endif

    free(ts);
    JS_SetRuntimeOpaque(rt, NULL);
}

static void js_std_dump_error1(JSContext* ctx, JSValueConst exception_val)
{
    JS_PrintValue(ctx, js_print_value_write, stderr, exception_val, NULL);
    fputc('\n', stderr);
}

void js_std_dump_error(JSContext* ctx)
{
    JSValue exception_val;

    exception_val = JS_GetException(ctx);
    if (JS_IsNull(exception_val)) {
        fprintf(stderr, "InternalError: out of memory (fatal)\n");
        return;
    }
    js_std_dump_error1(ctx, exception_val);
    JS_FreeValue(ctx, exception_val);
}

static JSRejectedPromiseEntry* find_rejected_promise(JSContext* ctx, JSThreadState* ts,
    JSValueConst promise)
{
    struct list_head* el;

    list_for_each(el, &ts->rejected_promise_list)
    {
        JSRejectedPromiseEntry* rp = list_entry(el, JSRejectedPromiseEntry, link);
        if (JS_SameValue(ctx, rp->promise, promise))
            return rp;
    }
    return NULL;
}

void js_std_promise_rejection_tracker(JSContext* ctx, JSValueConst promise,
    JSValueConst reason,
    BOOL is_handled, void* opaque)
{
    JSRuntime* rt = JS_GetRuntime(ctx);
    JSThreadState* ts = JS_GetRuntimeOpaque(rt);
    JSRejectedPromiseEntry* rp;

    if (!ts || std_handlers_freed)
        return;

    if (!is_handled) {
        rp = find_rejected_promise(ctx, ts, promise);
        if (!rp) {
            rp = malloc(sizeof(*rp));
            if (rp) {
                rp->promise = JS_DupValue(ctx, promise);
                rp->reason = JS_DupValue(ctx, reason);
                list_add_tail(&rp->link, &ts->rejected_promise_list);
            }
        }
    } else {
        rp = find_rejected_promise(ctx, ts, promise);
        if (rp) {
            JS_FreeValue(ctx, rp->promise);
            JS_FreeValue(ctx, rp->reason);
            list_del(&rp->link);
            free(rp);
        }
    }
}

static void js_std_promise_rejection_check(JSContext* ctx)
{
    JSRuntime* rt = JS_GetRuntime(ctx);
    JSThreadState* ts = JS_GetRuntimeOpaque(rt);
    struct list_head* el;

    if (unlikely(!list_empty(&ts->rejected_promise_list))) {
        struct list_head tmp = LIST_HEAD_INIT(tmp);
        tmp.next = ts->rejected_promise_list.next;
        tmp.prev = ts->rejected_promise_list.prev;
        ts->rejected_promise_list.next->prev = &tmp;
        ts->rejected_promise_list.prev->next = &tmp;
        ts->rejected_promise_list.next = &ts->rejected_promise_list;
        ts->rejected_promise_list.prev = &ts->rejected_promise_list;

        list_for_each(el, &tmp)
        {
            JSRejectedPromiseEntry* rp = list_entry(el, JSRejectedPromiseEntry, link);
            fprintf(stderr, "Possibly unhandled promise rejection: ");
            js_std_dump_error1(ctx, rp->reason);
        }
        exit(1);
    }
}

int js_std_loop(JSContext* ctx)
{
    int err;

    for (;;) {
        for (;;) {
            err = JS_ExecutePendingJob(JS_GetRuntime(ctx), NULL);
            if (err <= 0) {
                if (err < 0) {
                    js_std_dump_error(ctx);
                    return -1;
                }
                break;
            }
        }

        if (js_std_deadline_expired())
            goto deadline;

        js_std_promise_rejection_check(ctx);

        JSOSPollFunc poll = atomic_load_explicit(&os_poll_func,
            memory_order_relaxed);
        if (!poll || poll(ctx))
            break;
    }
    return 0;
deadline:
    fprintf(stderr, "InternalError: interrupted\n");
    return -1;
}

JSValue js_std_await(JSContext* ctx, JSValue obj)
{
    JSValue ret;
    int state;

    for (;;) {
        state = JS_PromiseState(ctx, obj);
        if (state == JS_PROMISE_FULFILLED) {
            ret = JS_PromiseResult(ctx, obj);
            JS_FreeValue(ctx, obj);
            break;
        } else if (state == JS_PROMISE_REJECTED) {
            ret = JS_Throw(ctx, JS_PromiseResult(ctx, obj));
            JS_FreeValue(ctx, obj);
            break;
        } else if (state == JS_PROMISE_PENDING) {
            int err;
            err = JS_ExecutePendingJob(JS_GetRuntime(ctx), NULL);
            if (err < 0) {
                JS_FreeValue(ctx, obj);
                return JS_EXCEPTION;
            }
            if (err == 0) {
                JSOSPollFunc poll;
                if (js_std_deadline_expired()) {
                    JS_FreeValue(ctx, obj);
                    JS_ThrowInternalError(ctx, "interrupted");
                    JS_SetUncatchableException(ctx, TRUE);
                    return JS_EXCEPTION;
                }
                js_std_promise_rejection_check(ctx);

                poll = atomic_load_explicit(&os_poll_func,
                    memory_order_relaxed);
                if ((!poll || poll(ctx)) && !JS_IsJobPending(JS_GetRuntime(ctx))
                    && JS_PromiseState(ctx, obj) == JS_PROMISE_PENDING) {
                    JS_FreeValue(ctx, obj);
                    return JS_ThrowInternalError(ctx, "awaited promise can never settle: no pending work is left");
                }
            }
        } else {
            ret = obj;
            break;
        }
    }
    return ret;
}

void js_std_eval_binary(JSContext* ctx, const uint8_t* buf, size_t buf_len,
    int load_only)
{
    JSValue obj, val;
    obj = JS_ReadObject(ctx, buf, buf_len, JS_READ_OBJ_BYTECODE);
    if (JS_IsException(obj))
        goto exception;
    if (load_only) {
        if (JS_VALUE_GET_TAG(obj) == JS_TAG_MODULE) {
            js_module_set_import_meta(ctx, obj, FALSE, FALSE);
        }
        JS_FreeValue(ctx, obj);
    } else {
        if (JS_VALUE_GET_TAG(obj) == JS_TAG_MODULE) {
            if (JS_ResolveModule(ctx, obj) < 0) {
                JS_FreeValue(ctx, obj);
                goto exception;
            }
            js_module_set_import_meta(ctx, obj, FALSE, TRUE);
            val = JS_EvalFunction(ctx, obj);
            val = js_std_await(ctx, val);
        } else {
            val = JS_EvalFunction(ctx, obj);
        }
        if (JS_IsException(val)) {
        exception:
            js_std_dump_error(ctx);
            exit(1);
        }
        JS_FreeValue(ctx, val);
    }
}

void js_std_eval_binary_json_module(JSContext* ctx,
    const uint8_t* buf, size_t buf_len,
    const char* module_name)
{
    JSValue obj;
    JSModuleDef* m;

    obj = JS_ReadObject(ctx, buf, buf_len, 0);
    if (JS_IsException(obj))
        goto exception;
    m = create_json_module(ctx, module_name, obj);
    if (!m) {
    exception:
        js_std_dump_error(ctx);
        exit(1);
    }
}
