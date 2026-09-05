/*
 * DynaJS C library
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
extern char **environ;
#endif

#if defined(__APPLE__) || defined(__FreeBSD__)
typedef sig_t sighandler_t;
#endif

#if defined(__APPLE__)
#if !defined(environ)
#include <crt_externs.h>
#define environ (*_NSGetEnviron())
#endif
#endif /* __APPLE__ */

#endif

/* enable the os.Worker API. It relies on POSIX threads */
#define USE_WORKER

#ifdef USE_WORKER
#include <pthread.h>
#include <stdatomic.h>
#endif

#include "cutils.h"
#include "list.h"
#include "dyna-libc.h"
#include "dyna-io.h"

#if !defined(PATH_MAX)
#define PATH_MAX 4096
#endif

/* After the exec timeout, give a killed child this long to die and be reaped
   before os.exec gives up. Kept small so a timed-out exec returns promptly
   (the deadline is the real bound); a child that exits normally is reaped
   immediately and never reaches here. */
#define DYN_EXEC_TIMEOUT_GRACE_MS 200

/* TODO:
   - add socket calls
*/

typedef struct {
    struct list_head link;
    int fd;
    int poll_fd_index; /* temporary use in js_os_poll() */
    JSValue rw_func[2];
} JSOSRWHandler;

typedef struct {
    struct list_head link;
    int sig_num;
    JSValue func;
} JSOSSignalHandler;

typedef struct {
    struct list_head link;
    int timer_id;
    int repeat; /* TRUE for setInterval timers */
    int64_t delay;
    int64_t timeout;
    JSValue func;
    int argc;       /* number of trailing args passed to setTimeout/setInterval */
    JSValue *args;  /* owned copies of those args (NULL when argc == 0) */
} JSOSTimer;

typedef struct {
    struct list_head link;
    uint8_t *data;
    size_t data_len;
    /* list of SharedArrayBuffers, necessary to free the message */
    uint8_t **sab_tab;
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
    struct list_head msg_queue; /* list of JSWorkerMessage.link */
    JSWaker waker;
} JSWorkerMessagePipe;

typedef struct {
    struct list_head link;
    JSWorkerMessagePipe *recv_pipe;
    JSValue on_message_func;
    int poll_fd_index; /* temporary use in js_os_poll() */
} JSWorkerMessageHandler;

typedef struct {
    struct list_head link;
    JSValue promise;
    JSValue reason;
} JSRejectedPromiseEntry;

typedef struct JSThreadState {
    struct list_head os_rw_handlers; /* list of JSOSRWHandler.link */
    struct list_head os_signal_handlers; /* list JSOSSignalHandler.link */
    struct list_head os_timers; /* list of JSOSTimer.link */
    int n_timers;               /* live timers: O(1) flood guard on setTimeout */
    struct list_head port_list; /* list of JSWorkerMessageHandler.link */
    struct list_head rejected_promise_list; /* list of JSRejectedPromiseEntry.link */
    int eval_script_recurse; /* only used in the main thread */
    int next_timer_id; /* for setTimeout() */
    /* not used in the main thread */
    JSWorkerMessagePipe *recv_pipe, *send_pipe;
#if !defined(_WIN32)
    struct pollfd *poll_fds;
    int poll_fds_size;
#endif
    /* An optional native IO reactor (dyna-io / dyna:http) folded into this
     * thread's event loop: its backend fd is polled alongside the rw-handlers,
     * and `io_reactor_drain(udata)` runs on THIS (the JS) thread when it is
     * readable -- so async IO completions dispatch JS handlers with no
     * cross-thread hop. fd == -1 means no reactor is registered. */
    int io_reactor_fd;
    void (*io_reactor_drain)(void *udata);
    void *io_reactor_udata;
} JSThreadState;

/* os read/write handlers (js_os_setReadHandler) are keyed by raw fd in
   ts->os_rw_handlers. Forward declarations so js_os_close/dup2 (defined
   earlier in the file) can clear a slot before the descriptor is released. */
static JSOSRWHandler *find_rh(JSThreadState *ts, int fd);
static void free_rw_handler(JSRuntime *rt, JSOSRWHandler *rh);
static void js_os_clear_handler(JSContext *ctx, int fd);

/* set from an async signal handler, read/cleared on the main thread: must be
   atomic to avoid a torn access / data race. It only gates a flag check (no
   data is published through it), so relaxed ordering is sufficient. */
static _Atomic uint64_t os_pending_signals;
/* process-global poll hook: written by std/os init on every thread that
   creates a runtime (main + workers) and read by each thread's event loop.
   All writes store the same value (js_os_poll) and readers only need to see
   NULL or js_os_poll, so relaxed atomics are sufficient and race-free. */
typedef int (*JSOSPollFunc)(JSContext *ctx);
static _Atomic(JSOSPollFunc) os_poll_func;

/* Correctly-typed DynBufReallocFunc adapter: calling js_realloc_rt (first arg
   JSRuntime*) through the DynBufReallocFunc type (first arg void*) is C UB and
   traps under -fsanitize=function / CFI / LTO. */
static void *js_std_dbuf_realloc(void *opaque, void *ptr, size_t size)
{
    return js_realloc_rt((JSRuntime *)opaque, ptr, size);
}

static void js_std_dbuf_init(JSContext *ctx, DynBuf *s)
{
    dbuf_init2(s, JS_GetRuntime(ctx), js_std_dbuf_realloc);
}

static BOOL my_isdigit(int c)
{
    return (c >= '0' && c <= '9');
}

/* XXX: use 'o' and 'O' for object using JS_PrintValue() ? */
static JSValue js_printf_internal(JSContext *ctx,
                                  int argc, JSValueConst *argv, FILE *fp)
{
    char fmtbuf[32];
    uint8_t cbuf[UTF8_CHAR_LEN_MAX+1];
    JSValue res;
    DynBuf dbuf;
    const char *fmt_str = NULL;
    const uint8_t *fmt, *fmt_end;
    const uint8_t *p;
    char *q;
    int i, c, len, mod;
    size_t fmt_len;
    int32_t int32_arg;
    int64_t int64_arg;
    double double_arg;
    const char *string_arg;
    /* Use indirect call to dbuf_printf to prevent gcc warning */
    int (*dbuf_printf_fun)(DynBuf *s, const char *fmt, ...) = (void*)dbuf_printf;

    js_std_dbuf_init(ctx, &dbuf);

    if (argc > 0) {
        fmt_str = JS_ToCStringLen(ctx, &fmt_len, argv[0]);
        if (!fmt_str)
            goto fail;

        i = 1;
        fmt = (const uint8_t *)fmt_str;
        fmt_end = fmt + fmt_len;
        while (fmt < fmt_end) {
            for (p = fmt; fmt < fmt_end && *fmt != '%'; fmt++)
                continue;
            dbuf_put(&dbuf, p, fmt - p);
            if (fmt >= fmt_end)
                break;
            q = fmtbuf;
            *q++ = *fmt++;  /* copy '%' */

            /* flags */
            for(;;) {
                c = *fmt;
                if (c == '0' || c == '#' || c == '+' || c == '-' || c == ' ' ||
                    c == '\'') {
                    if (q >= fmtbuf + sizeof(fmtbuf) - 1)
                        goto invalid;
                    *q++ = c;
                    fmt++;
                } else {
                    break;
                }
            }
            /* width */
            if (*fmt == '*') {
                if (i >= argc)
                    goto missing;
                if (JS_ToInt32(ctx, &int32_arg, argv[i++]))
                    goto fail;
                q += snprintf(q, fmtbuf + sizeof(fmtbuf) - q, "%d", int32_arg);
                if (unlikely(q >= fmtbuf + sizeof(fmtbuf) - 1))
                    goto invalid;
                fmt++;
            } else {
                while (my_isdigit(*fmt)) {
                    if (q >= fmtbuf + sizeof(fmtbuf) - 1)
                        goto invalid;
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
                    q += snprintf(q, fmtbuf + sizeof(fmtbuf) - q, "%d", int32_arg);
                    if (unlikely(q >= fmtbuf + sizeof(fmtbuf) - 1))
                        goto invalid;
                    fmt++;
                } else {
                    while (my_isdigit(*fmt)) {
                        if (q >= fmtbuf + sizeof(fmtbuf) - 1)
                            goto invalid;
                        *q++ = *fmt++;
                    }
                }
            }

            /* we only support the "l" modifier for 64 bit numbers */
            mod = ' ';
            if (*fmt == 'l') {
                mod = *fmt++;
            }

            /* type */
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
                    int32_arg = unicode_from_utf8((const uint8_t *)string_arg, UTF8_CHAR_LEN_MAX, &p);
                    JS_FreeCString(ctx, string_arg);
                } else {
                    if (JS_ToInt32(ctx, &int32_arg, argv[i++]))
                        goto fail;
                }
                /* handle utf-8 encoding explicitly */
                if ((unsigned)int32_arg > 0x10FFFF)
                    int32_arg = 0xFFFD;
                /* ignore conversion flags, width and precision */
                len = unicode_to_utf8(cbuf, int32_arg);
                dbuf_put(&dbuf, cbuf, len);
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
                    /* 64 bit number */
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
                /* XXX: handle strings containing null characters */
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
                /* XXX: should support an extension mechanism */
            invalid:
                JS_ThrowTypeError(ctx, "invalid conversion specifier in format string");
                goto fail;
            missing:
                JS_ThrowReferenceError(ctx, "missing argument for conversion specifier");
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
            res = JS_NewStringLen(ctx, (char *)dbuf.buf, dbuf.size);
        }
    }
    dbuf_free(&dbuf);
    return res;

fail:
    JS_FreeCString(ctx, fmt_str);
    dbuf_free(&dbuf);
    return JS_EXCEPTION;
}

uint8_t *js_load_file(JSContext *ctx, size_t *pbuf_len, const char *filename)
{
    FILE *f;
    uint8_t *buf;
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
    /* XXX: on Linux, ftell() return LONG_MAX for directories */
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

/* load and evaluate a file */
static JSValue js_loadScript(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    dyn_iobuf_t src;
    const char *filename;
    JSValue ret;

    filename = JS_ToCString(ctx, argv[0]);
    if (!filename)
        return JS_EXCEPTION;
    /* read through the shared io core (heap read: SIGBUS-safe, never mmap) */
    if (dyn_io_read_buf(filename, &src, DYN_SLURP_NUL, DYN_MAX_INPUT) < 0) {
        JS_ThrowReferenceError(ctx, "could not load '%s'", filename);
        JS_FreeCString(ctx, filename);
        return JS_EXCEPTION;
    }
    ret = JS_Eval(ctx, (char *)dyn_iobuf_rdata(&src), dyn_iobuf_rlen(&src),
                  filename, JS_EVAL_TYPE_GLOBAL);
    dyn_iobuf_free(&src);
    JS_FreeCString(ctx, filename);
    return ret;
}

/* load a file as a UTF-8 encoded string */
static JSValue js_std_loadFile(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    dyn_iobuf_t src;
    const char *filename;
    JSValue ret;

    filename = JS_ToCString(ctx, argv[0]);
    if (!filename)
        return JS_EXCEPTION;
    if (dyn_io_read_buf(filename, &src, 0, DYN_MAX_INPUT) < 0) {
        JS_FreeCString(ctx, filename);
        return JS_NULL;
    }
    JS_FreeCString(ctx, filename);
    ret = JS_NewStringLen(ctx, (char *)dyn_iobuf_rdata(&src),
                          dyn_iobuf_rlen(&src));
    dyn_iobuf_free(&src);
    return ret;
}

typedef JSModuleDef *(JSInitModuleFunc)(JSContext *ctx,
                                        const char *module_name);


#if defined(_WIN32)
static JSModuleDef *js_module_loader_so(JSContext *ctx,
                                        const char *module_name)
{
    JS_ThrowReferenceError(ctx, "shared library modules are not supported yet");
    return NULL;
}
#else
static JSModuleDef *js_module_loader_so(JSContext *ctx,
                                        const char *module_name)
{
    JSModuleDef *m;
    void *hd;
    JSInitModuleFunc *init;
    char *filename;

    /* Security (audit 13.7.1): a .so module is arbitrary native code that
       runs at dlopen time, so the name must stay inside the module
       convention: CWD-relative only -- never an absolute path, never a
       traversal, never a drive letter. Both "./name.so" and "sub/name.so"
       (what CONFIG_SHARED_LIBS imports resolve to) are that convention. */
    if (module_name[0] == '.' && module_name[1] == '/')
        module_name += 2;
    if (module_name[0] == '\0' || module_name[0] == '/' ||
        !strncmp(module_name, "..", 2) || strstr(module_name, "/..") ||
        strchr(module_name, '\\') || strchr(module_name, ':')) {
        JS_ThrowReferenceError(ctx, "invalid shared library module name '%s'",
                               module_name);
        return NULL;
    }

    if (!strchr(module_name, '/')) {
        /* must add a '/' so that the DLL is not searched in the
           system library paths */
        size_t mn_len = strlen(module_name);   /* strcpy would scan it again */
        filename = js_malloc(ctx, mn_len + 2 + 1);
        if (!filename)
            return NULL;
        memcpy(filename, "./", 2);
        memcpy(filename + 2, module_name, mn_len + 1);
    } else {
        filename = (char *)module_name;
    }

    /* C module */
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
#endif /* !_WIN32 */

int js_module_set_import_meta(JSContext *ctx, JSValueConst func_val,
                              JS_BOOL use_realpath, JS_BOOL is_main)
{
    JSModuleDef *m;
    char buf[PATH_MAX + 16];
    JSValue meta_obj;
    JSAtom module_name_atom;
    const char *module_name;

    assert(JS_VALUE_GET_TAG(func_val) == JS_TAG_MODULE);
    m = JS_VALUE_GET_PTR(func_val);

    module_name_atom = JS_GetModuleName(ctx, m);
    module_name = JS_AtomToCString(ctx, module_name_atom);
    JS_FreeAtom(ctx, module_name_atom);
    if (!module_name)
        return -1;
    if (!strchr(module_name, ':')) {
        memcpy(buf, "file://", 8);   /* 7 chars + NUL */
#if !defined(_WIN32)
        /* realpath() cannot be used with modules compiled with dynajsc
           because the corresponding module source code is not
           necessarily present */
        if (use_realpath) {
            char *res = realpath(module_name, buf + strlen(buf));
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

static int json_module_init(JSContext *ctx, JSModuleDef *m)
{
    JSValue val;
    val = JS_GetModulePrivateValue(ctx, m);
    JS_SetModuleExport(ctx, m, "default", val);
    return 0;
}

static JSModuleDef *create_json_module(JSContext *ctx, const char *module_name, JSValue val)
{
    JSModuleDef *m;
    m = JS_NewCModule(ctx, module_name, json_module_init);
    if (!m) {
        JS_FreeValue(ctx, val);
        return NULL;
    }
    /* only export the "default" symbol which will contain the JSON object */
    JS_AddModuleExport(ctx, m, "default");
    JS_SetModulePrivateValue(ctx, m, val);
    return m;
}

/* in order to conform with the specification, only the keys should be
   tested and not the associated values. */
int js_module_check_attributes(JSContext *ctx, void *opaque,
                               JSValueConst attributes)
{
    JSPropertyEnum *tab;
    uint32_t i, len;
    int ret;
    const char *cstr;
    size_t cstr_len;
    
    if (JS_GetOwnPropertyNames(ctx, &tab, &len, attributes, JS_GPN_ENUM_ONLY | JS_GPN_STRING_MASK))
        return -1;
    ret = 0;
    for(i = 0; i < len; i++) {
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

/* return > 0 if the attributes indicate a JSON module */
int js_module_test_json(JSContext *ctx, JSValueConst attributes)
{
    JSValue str;
    const char *cstr;
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
    /* XXX: raise an error if unknown type ? */
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

JSModuleDef *js_module_loader(JSContext *ctx,
                              const char *module_name, void *opaque,
                              JSValueConst attributes)
{
    JSModuleDef *m;
    int res;
    
    if (has_suffix(module_name, ".so")) {
        m = js_module_loader_so(ctx, module_name);
    } else {
        dyn_iobuf_t src;

        /* read the module source through the shared io core (heap: SIGBUS-safe) */
        if (dyn_io_read_buf(module_name, &src, DYN_SLURP_NUL, DYN_MAX_INPUT) < 0) {
            JS_ThrowReferenceError(ctx, "could not load module filename '%s'",
                                   module_name);
            return NULL;
        }
        res = js_module_test_json(ctx, attributes);
        if (has_suffix(module_name, ".json") || res > 0) {
            /* compile as JSON or JSON5 depending on "type" */
            JSValue val;
            int flags;
            if (res == 2)
                flags = JS_PARSE_JSON_EXT;
            else
                flags = 0;
            val = JS_ParseJSON2(ctx, (char *)dyn_iobuf_rdata(&src),
                                dyn_iobuf_rlen(&src), module_name, flags);
            dyn_iobuf_free(&src);
            if (JS_IsException(val))
                return NULL;
            m = create_json_module(ctx, module_name, val);
            if (!m)
                return NULL;
        } else {
            JSValue func_val;
            /* compile the module */
            func_val = JS_Eval(ctx, (char *)dyn_iobuf_rdata(&src),
                               dyn_iobuf_rlen(&src), module_name,
                               JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
            dyn_iobuf_free(&src);
            if (JS_IsException(func_val))
                return NULL;
            /* XXX: could propagate the exception */
            js_module_set_import_meta(ctx, func_val, TRUE, FALSE);
            /* the module is already referenced, so we must free it */
            m = JS_VALUE_GET_PTR(func_val);
            JS_FreeValue(ctx, func_val);
        }
    }
    return m;
}

static JSValue js_std_exit(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    int status;
    if (JS_ToInt32(ctx, &status, argv[0]))
        status = -1;
    exit(status);
    return JS_UNDEFINED;
}

static JSValue js_std_getenv(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
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
static void setenv(const char *name, const char *value, int overwrite)
{
    char *str;
    size_t name_len, value_len;
    name_len = strlen(name);
    value_len = strlen(value);
    str = malloc(name_len + 1 + value_len + 1);
    memcpy(str, name, name_len);
    str[name_len] = '=';
    memcpy(str + name_len + 1, value, value_len);
    str[name_len + 1 + value_len] = '\0';
    _putenv(str);
    free(str);
}

static void unsetenv(const char *name)
{
    setenv(name, "", TRUE);
}
#endif /* _WIN32 */

static JSValue js_std_setenv(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
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

static JSValue js_std_unsetenv(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    const char *name;
    name = JS_ToCString(ctx, argv[0]);
    if (!name)
        return JS_EXCEPTION;
    unsetenv(name);
    JS_FreeCString(ctx, name);
    return JS_UNDEFINED;
}

/* return an object containing the list of the available environment
   variables. */
static JSValue js_std_getenviron(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    char **envp;
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
    for(idx = 0; envp[idx] != NULL; idx++) {
        name = envp[idx];
        p = strchr(name, '=');
        if (!p)
            continue;
        /* p - name with p == NULL is UB; the check must come first. */
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

static JSValue js_std_gc(JSContext *ctx, JSValueConst this_val,
                         int argc, JSValueConst *argv)
{
    JS_RunGC(JS_GetRuntime(ctx));
    return JS_UNDEFINED;
}

static int interrupt_handler(JSRuntime *rt, void *opaque)
{
    return (atomic_load_explicit(&os_pending_signals, memory_order_relaxed) >> SIGINT) & 1;
}

static int get_bool_option(JSContext *ctx, BOOL *pbool,
                           JSValueConst obj,
                           const char *option)
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

static JSValue js_evalScript(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    JSRuntime *rt = JS_GetRuntime(ctx);
    JSThreadState *ts = JS_GetRuntimeOpaque(rt);
    const char *str;
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
        /* install the interrupt handler */
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
        /* remove the interrupt handler */
        JS_SetInterruptHandler(JS_GetRuntime(ctx), NULL, NULL);
        atomic_fetch_and_explicit(&os_pending_signals,
                                  ~((uint64_t)1 << SIGINT), memory_order_relaxed);
        /* convert the uncatchable "interrupted" error into a normal error
           so that it can be caught by the REPL */
        if (JS_IsException(ret))
            JS_SetUncatchableException(ctx, FALSE);
    }
    return ret;
}

static JSClassID js_std_file_class_id;

typedef struct {
    FILE *f;
    BOOL close_in_finalizer;
} JSSTDFile;

static void js_std_file_finalizer(JSRuntime *rt, JSValue val)
{
    JSSTDFile *s = JS_GetOpaque(val, js_std_file_class_id);
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

static JSValue js_std_strerror(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    int err;
    if (JS_ToInt32(ctx, &err, argv[0]))
        return JS_EXCEPTION;
    return JS_NewString(ctx, strerror(err));
}

static JSValue js_std_parseExtJSON(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    JSValue obj;
    const char *str;
    size_t len;

    str = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!str)
        return JS_EXCEPTION;
    obj = JS_ParseJSON2(ctx, str, len, "<input>", JS_PARSE_JSON_EXT);
    JS_FreeCString(ctx, str);
    return obj;
}

static JSValue js_new_std_file(JSContext *ctx, FILE *f,
                               BOOL close_in_finalizer)
{
    JSSTDFile *s;
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

static void js_set_error_object(JSContext *ctx, JSValue obj, int err)
{
    if (!JS_IsUndefined(obj)) {
        JS_SetPropertyStr(ctx, obj, "errno", JS_NewInt32(ctx, err));
    }
}

static JSValue js_std_open(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    const char *filename, *mode = NULL;
    FILE *f;
    int err;

    filename = JS_ToCString(ctx, argv[0]);
    if (!filename)
        goto fail;
    mode = JS_ToCString(ctx, argv[1]);
    if (!mode)
        goto fail;
    if (mode[strspn(mode, "rwa+b")] != '\0') {
        JS_ThrowTypeError(ctx, "invalid file mode");
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

static JSValue js_std_fdopen(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    const char *mode;
    FILE *f;
    int fd, err;

    if (JS_ToInt32(ctx, &fd, argv[0]))
        return JS_EXCEPTION;
    mode = JS_ToCString(ctx, argv[1]);
    if (!mode)
        goto fail;
    if (mode[strspn(mode, "rwa+")] != '\0') {
        JS_ThrowTypeError(ctx, "invalid file mode");
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

static JSValue js_std_tmpfile(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    FILE *f;
    f = tmpfile();
    if (argc >= 1)
        js_set_error_object(ctx, argv[0], f ? 0 : errno);
    if (!f)
        return JS_NULL;
    return js_new_std_file(ctx, f, TRUE);
}

static JSValue js_std_sprintf(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv)
{
    return js_printf_internal(ctx, argc, argv, NULL);
}

static JSValue js_std_printf(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    return js_printf_internal(ctx, argc, argv, stdout);
}

static FILE *js_std_file_get(JSContext *ctx, JSValueConst obj)
{
    JSSTDFile *s = JS_GetOpaque2(ctx, obj, js_std_file_class_id);
    if (!s)
        return NULL;
    if (!s->f) {
        JS_ThrowTypeError(ctx, "invalid file handle");
        return NULL;
    }
    return s->f;
}

static JSValue js_std_file_puts(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv, int magic)
{
    FILE *f;
    int i;
    const char *str;
    size_t len;

    if (magic == 0) {
        f = stdout;
    } else {
        f = js_std_file_get(ctx, this_val);
        if (!f)
            return JS_EXCEPTION;
    }

    for(i = 0; i < argc; i++) {
        str = JS_ToCStringLen(ctx, &len, argv[i]);
        if (!str)
            return JS_EXCEPTION;
        fwrite(str, 1, len, f);
        JS_FreeCString(ctx, str);
    }
    return JS_UNDEFINED;
}

static JSValue js_std_file_close(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    JSSTDFile *s = JS_GetOpaque2(ctx, this_val, js_std_file_class_id);
    int err;
    if (!s)
        return JS_EXCEPTION;
    if (!s->f)
        return JS_ThrowTypeError(ctx, "invalid file handle");
    err = js_get_errno(fclose(s->f));
    s->f = NULL;
    return JS_NewInt32(ctx, err);
}

static JSValue js_std_file_printf(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    FILE *f = js_std_file_get(ctx, this_val);
    if (!f)
        return JS_EXCEPTION;
    return js_printf_internal(ctx, argc, argv, f);
}

static void js_print_value_write(void *opaque, const char *buf, size_t len)
{
    FILE *fo = opaque;
    fwrite(buf, 1, len, fo);
}

static JSValue js_std_file_printObject(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    JS_PrintValue(ctx, js_print_value_write, stdout, argv[0], NULL);
    return JS_UNDEFINED;
}

static JSValue js_std_file_flush(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    FILE *f = js_std_file_get(ctx, this_val);
    if (!f)
        return JS_EXCEPTION;
    fflush(f);
    return JS_UNDEFINED;
}

static JSValue js_std_file_tell(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv, int is_bigint)
{
    FILE *f = js_std_file_get(ctx, this_val);
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

static JSValue js_std_file_seek(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    FILE *f = js_std_file_get(ctx, this_val);
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

static JSValue js_std_file_eof(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    FILE *f = js_std_file_get(ctx, this_val);
    if (!f)
        return JS_EXCEPTION;
    return JS_NewBool(ctx, feof(f));
}

static JSValue js_std_file_error(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    FILE *f = js_std_file_get(ctx, this_val);
    if (!f)
        return JS_EXCEPTION;
    return JS_NewBool(ctx, ferror(f));
}

static JSValue js_std_file_clearerr(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    FILE *f = js_std_file_get(ctx, this_val);
    if (!f)
        return JS_EXCEPTION;
    clearerr(f);
    return JS_UNDEFINED;
}

static JSValue js_std_file_fileno(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    FILE *f = js_std_file_get(ctx, this_val);
    if (!f)
        return JS_EXCEPTION;
    return JS_NewInt32(ctx, fileno(f));
}

static JSValue js_std_file_read_write(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv, int magic)
{
    FILE *f = js_std_file_get(ctx, this_val);
    uint64_t pos, len;
    size_t size, ret;
    uint8_t *buf;

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
        return JS_ThrowRangeError(ctx, "read/write array buffer overflow");
    if (magic)
        ret = fwrite(buf + pos, 1, len, f);
    else
        ret = fread(buf + pos, 1, len, f);
    return JS_NewInt64(ctx, ret);
}

/* XXX: could use less memory and go faster */
static JSValue js_std_file_getline(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    FILE *f = js_std_file_get(ctx, this_val);
    JSValue obj;

    if (!f)
        return JS_EXCEPTION;

#if !defined(_WIN32)
    /* getdelim: memchr over the FILE's own buffer, and leaves the stream exactly
       after the newline (a fread loop would need its own pushback).
       Matches the fgetc path below: NULL only at EOF with nothing read, trailing
       '\n' stripped, final unterminated line returned, embedded NULs preserved.
       malloc, not js_malloc: transient C buffer, never a JS object. */
    {
        char *line = NULL;
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
    for(;;) {
        c = fgetc(f);
        if (c == EOF) {
            if (dbuf.size == 0) {
                /* EOF */
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
    obj = JS_NewStringLen(ctx, (const char *)dbuf.buf, dbuf.size);
    dbuf_free(&dbuf);
    return obj;
    }
#endif
}

/* XXX: could use less memory and go faster */
static JSValue js_std_file_readAsString(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv)
{
    FILE *f = js_std_file_get(ctx, this_val);
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

    /* fstat pre-size: one syscall, removes realloc churn on regular files.
       Hint only -- pipes/procfs report no usable size; clamped against a bogus
       st_size. The loop below is correct without it. */
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
                    dbuf_claim(&dbuf, (size_t)want);  /* hint; ignore failure */
            }
        }
    }

    /* Chunked fread: fgetc takes the stdio lock per byte (call-bound, not IO-bound). */
    while (max_size != 0) {
        char buf[65536];
        size_t want = max_size < sizeof(buf) ? max_size : sizeof(buf);
        size_t n = fread(buf, 1, want, f);
        if (n == 0)
            break;
        if (dbuf_put(&dbuf, (const uint8_t *)buf, n)) {
            dbuf_free(&dbuf);
            return JS_EXCEPTION;
        }
        max_size -= n;
    }
    obj = JS_NewStringLen(ctx, (const char *)dbuf.buf, dbuf.size);
    dbuf_free(&dbuf);
    return obj;
}

static JSValue js_std_file_getByte(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    FILE *f = js_std_file_get(ctx, this_val);
    if (!f)
        return JS_EXCEPTION;
    return JS_NewInt32(ctx, fgetc(f));
}

static JSValue js_std_file_putByte(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    FILE *f = js_std_file_get(ctx, this_val);
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
    /* various errno values */
#define DEF(x) JS_PROP_INT32_DEF(#x, x, JS_PROP_CONFIGURABLE )
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
    JS_CFUNC_DEF("exit", 1, js_std_exit ),
    JS_CFUNC_DEF("gc", 0, js_std_gc ),
    JS_CFUNC_DEF("evalScript", 1, js_evalScript ),
    JS_CFUNC_DEF("loadScript", 1, js_loadScript ),
    JS_CFUNC_DEF("getenv", 1, js_std_getenv ),
    JS_CFUNC_DEF("setenv", 1, js_std_setenv ),
    JS_CFUNC_DEF("unsetenv", 1, js_std_unsetenv ),
    JS_CFUNC_DEF("getenviron", 1, js_std_getenviron ),
    JS_CFUNC_DEF("loadFile", 1, js_std_loadFile ),
    JS_CFUNC_DEF("strerror", 1, js_std_strerror ),
    JS_CFUNC_DEF("parseExtJSON", 1, js_std_parseExtJSON ),

    /* FILE I/O */
    JS_CFUNC_DEF("open", 2, js_std_open ),
    JS_CFUNC_DEF("fdopen", 2, js_std_fdopen ),
    JS_CFUNC_DEF("tmpfile", 0, js_std_tmpfile ),
    JS_CFUNC_MAGIC_DEF("puts", 1, js_std_file_puts, 0 ),
    JS_CFUNC_DEF("printf", 1, js_std_printf ),
    JS_CFUNC_DEF("sprintf", 1, js_std_sprintf ),
    JS_PROP_INT32_DEF("SEEK_SET", SEEK_SET, JS_PROP_CONFIGURABLE ),
    JS_PROP_INT32_DEF("SEEK_CUR", SEEK_CUR, JS_PROP_CONFIGURABLE ),
    JS_PROP_INT32_DEF("SEEK_END", SEEK_END, JS_PROP_CONFIGURABLE ),
    JS_OBJECT_DEF("Error", js_std_error_props, countof(js_std_error_props), JS_PROP_CONFIGURABLE),
    JS_CFUNC_DEF("__printObject", 1, js_std_file_printObject ),
};

static const JSCFunctionListEntry js_std_file_proto_funcs[] = {
    JS_CFUNC_DEF("close", 0, js_std_file_close ),
    JS_CFUNC_MAGIC_DEF("puts", 1, js_std_file_puts, 1 ),
    JS_CFUNC_DEF("printf", 1, js_std_file_printf ),
    JS_CFUNC_DEF("flush", 0, js_std_file_flush ),
    JS_CFUNC_MAGIC_DEF("tell", 0, js_std_file_tell, 0 ),
    JS_CFUNC_MAGIC_DEF("tello", 0, js_std_file_tell, 1 ),
    JS_CFUNC_DEF("seek", 2, js_std_file_seek ),
    JS_CFUNC_DEF("eof", 0, js_std_file_eof ),
    JS_CFUNC_DEF("fileno", 0, js_std_file_fileno ),
    JS_CFUNC_DEF("error", 0, js_std_file_error ),
    JS_CFUNC_DEF("clearerr", 0, js_std_file_clearerr ),
    JS_CFUNC_MAGIC_DEF("read", 3, js_std_file_read_write, 0 ),
    JS_CFUNC_MAGIC_DEF("write", 3, js_std_file_read_write, 1 ),
    JS_CFUNC_DEF("getline", 0, js_std_file_getline ),
    JS_CFUNC_DEF("readAsString", 0, js_std_file_readAsString ),
    JS_CFUNC_DEF("getByte", 0, js_std_file_getByte ),
    JS_CFUNC_DEF("putByte", 1, js_std_file_putByte ),
    /* setvbuf, ...  */
};

static int js_std_init(JSContext *ctx, JSModuleDef *m)
{
    JSValue proto;

    /* FILE class */
    /* the class ID is created once */
    JS_NewClassID(&js_std_file_class_id);
    /* the class is created once per runtime */
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

JSModuleDef *js_init_module_std(JSContext *ctx, const char *module_name)
{
    JSModuleDef *m;
    m = JS_NewCModule(ctx, module_name, js_std_init);
    if (!m)
        return NULL;
    JS_AddModuleExportList(ctx, m, js_std_funcs, countof(js_std_funcs));
    JS_AddModuleExport(ctx, m, "in");
    JS_AddModuleExport(ctx, m, "out");
    JS_AddModuleExport(ctx, m, "err");
    return m;
}

/**********************************************************/
/* 'os' object */

static JSValue js_os_open(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv)
{
    const char *filename;
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
    /* force binary mode by default */
    if (!(flags & O_TEXT))
        flags |= O_BINARY;
#endif
    /* CLOEXEC at source: os.exec() forks, and any fd the engine hands out to
       JS that is not close-on-exec leaks into every spawned child. Callers who
       pass the fd as a child's std{in,out,err} still get it through because
       exec() dup2()s it onto 0/1/2, which clears FD_CLOEXEC on the target. */
    if (!(flags & O_CLOEXEC))
        flags |= O_CLOEXEC;
    ret = js_get_errno(open(filename, flags, mode));
    JS_FreeCString(ctx, filename);
    return JS_NewInt32(ctx, ret);
}

static JSValue js_os_close(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    int fd, ret;
    if (JS_ToInt32(ctx, &fd, argv[0]))
        return JS_EXCEPTION;
    /* "del before close": drop any read/write poll slot for this descriptor
       so the next poll() does not see a closed fd (POLLNVAL) and re-dispatch
       a stale callback, and so a later fd-number reuse cannot deliver the old
       callback to a foreign descriptor. */
    js_os_clear_handler(ctx, fd);
    ret = js_get_errno(close(fd));
    return JS_NewInt32(ctx, ret);
}

static JSValue js_os_seek(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv)
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

static JSValue js_os_read_write(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv, int magic)
{
    int fd;
    uint64_t pos = 0, len = 0;
    int64_t fpos = 0;
    BOOL has_len = FALSE, has_fpos = FALSE;
    size_t size;
    ssize_t ret;
    uint8_t *buf;

    if (JS_ToInt32(ctx, &fd, argv[0]))
        return JS_EXCEPTION;
    /* pos, len and the file position are resolved BEFORE the buffer's
       pointer is fetched: the coercion can run a user valueOf, and that JS
       can transfer (detach) or resize the buffer -- a pointer captured
       earlier would be stale freed memory by the time read/write touched it
       (same discipline as js_textcodec_get_bytes: no JS call may sit between
       the pointer fetch and its use). */
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
            return JS_ThrowRangeError(ctx, "read/write position must not be negative");
        has_fpos = TRUE;
    }
    buf = JS_GetArrayBuffer(ctx, &size, argv[1]);
    if (!buf)
        return JS_EXCEPTION;
    if (pos > size)
        return JS_ThrowRangeError(ctx, "read/write array buffer overflow");
    if (has_len) {
        /* overflow-proof form of pos + len > size */
        if (len > size - pos)
            return JS_ThrowRangeError(ctx, "read/write array buffer overflow");
    } else {
        /* length omitted -> rest of buffer from pos */
        len = size - pos;
    }
    if (has_fpos) {
        if (magic)
            ret = js_get_errno(pwrite(fd, buf + pos, len, (off_t)fpos));
        else
            ret = js_get_errno(pread(fd, buf + pos, len, (off_t)fpos));
        if (ret < 0 && errno == ESPIPE) {
            /* non-seekable fd (pipe/socket/tty): position is meaningless,
               fallback to current offset so the call still succeeds when
               the caller passed a position out of habit. */
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

static JSValue js_os_isatty(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    int fd;
    if (JS_ToInt32(ctx, &fd, argv[0]))
        return JS_EXCEPTION;
    return JS_NewBool(ctx, isatty(fd));
}

#if defined(_WIN32)
static JSValue js_os_ttyGetWinSize(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
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

/* Windows 10 built-in VT100 emulation */
#define __ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#define __ENABLE_VIRTUAL_TERMINAL_INPUT 0x0200

static JSValue js_os_ttySetRaw(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    int fd;
    HANDLE handle;

    if (JS_ToInt32(ctx, &fd, argv[0]))
        return JS_EXCEPTION;
    handle = (HANDLE)_get_osfhandle(fd);
    SetConsoleMode(handle, ENABLE_WINDOW_INPUT | __ENABLE_VIRTUAL_TERMINAL_INPUT);
    _setmode(fd, _O_BINARY);
    if (fd == 0) {
        handle = (HANDLE)_get_osfhandle(1); /* corresponding output */
        SetConsoleMode(handle, ENABLE_PROCESSED_OUTPUT | ENABLE_WRAP_AT_EOL_OUTPUT | __ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
    return JS_UNDEFINED;
}
#else
static JSValue js_os_ttyGetWinSize(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    int fd;
    struct winsize ws;
    JSValue obj;

    if (JS_ToInt32(ctx, &fd, argv[0]))
        return JS_EXCEPTION;
    if (ioctl(fd, TIOCGWINSZ, &ws) == 0 &&
        ws.ws_col >= 4 && ws.ws_row >= 4) {
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
static int term_exit_armed;   /* atexit handlers stack: arm once, not per call */

static void term_exit(void)
{
    /* Registered only by ttySetRaw, so this is the REPL. It enables bracketed
       paste; leaving it on outlives the process and breaks pasting in the
       user's shell, including when std.exit() skips the REPL's own cleanup. */
    if (isatty(1)) {
        ssize_t n = write(1, "\x1b[?2004l", 8);
        (void)n; /* an exit handler has no recovery */
    }
    tcsetattr(0, TCSANOW, &oldtty);
}

/* XXX: should add a way to go back to normal mode */
static JSValue js_os_ttySetRaw(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    struct termios tty;
    int fd;

    if (JS_ToInt32(ctx, &fd, argv[0]))
        return JS_EXCEPTION;

    memset(&tty, 0, sizeof(tty));
    tcgetattr(fd, &tty);
    oldtty = tty;

    tty.c_iflag &= ~(IGNBRK|BRKINT|PARMRK|ISTRIP
                          |INLCR|IGNCR|ICRNL|IXON);
    tty.c_oflag |= OPOST;
    tty.c_lflag &= ~(ECHO|ECHONL|ICANON|IEXTEN);
    tty.c_cflag &= ~(CSIZE|PARENB);
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

#endif /* !_WIN32 */

static JSValue js_os_remove(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    const char *filename;
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

static JSValue js_os_rename(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
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

static BOOL is_main_thread(JSRuntime *rt)
{
    JSThreadState *ts = JS_GetRuntimeOpaque(rt);
    return !ts->recv_pipe;
}

static JSOSRWHandler *find_rh(JSThreadState *ts, int fd)
{
    JSOSRWHandler *rh;
    struct list_head *el;

    list_for_each(el, &ts->os_rw_handlers) {
        rh = list_entry(el, JSOSRWHandler, link);
        if (rh->fd == fd)
            return rh;
    }
    return NULL;
}

static void free_rw_handler(JSRuntime *rt, JSOSRWHandler *rh)
{
    int i;
    list_del(&rh->link);
    for(i = 0; i < 2; i++) {
        JS_FreeValueRT(rt, rh->rw_func[i]);
    }
    js_free_rt(rt, rh);
}

/* Release every read/write handler slot registered for 'fd'. Used by
   js_os_close and js_os_dup2 to uphold the reactor invariant "del before
   close" (the same discipline dyna-evloop's dyn_evloop_del enforces): a
   descriptor must not stay in the poll set after it is closed, otherwise
   poll() reports POLLNVAL and the stale callback is re-dispatched forever
   (or fires on a foreign fd when the number is reused). No-op if the fd is
   not registered. */
static void js_os_clear_handler(JSContext *ctx, int fd)
{
    JSRuntime *rt = JS_GetRuntime(ctx);
    JSThreadState *ts = JS_GetRuntimeOpaque(rt);
    JSOSRWHandler *rh = find_rh(ts, fd);
    if (rh)
        free_rw_handler(rt, rh);
}

static JSValue js_os_setReadHandler(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv, int magic)
{
    JSRuntime *rt = JS_GetRuntime(ctx);
    JSThreadState *ts = JS_GetRuntimeOpaque(rt);
    JSOSRWHandler *rh;
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
            if (JS_IsNull(rh->rw_func[0]) &&
                JS_IsNull(rh->rw_func[1])) {
                /* remove the entry */
                free_rw_handler(JS_GetRuntime(ctx), rh);
            }
        }
    } else {
        if (!JS_IsFunction(ctx, func))
            return JS_ThrowTypeError(ctx, "not a function");
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

static JSOSSignalHandler *find_sh(JSThreadState *ts, int sig_num)
{
    JSOSSignalHandler *sh;
    struct list_head *el;
    list_for_each(el, &ts->os_signal_handlers) {
        sh = list_entry(el, JSOSSignalHandler, link);
        if (sh->sig_num == sig_num)
            return sh;
    }
    return NULL;
}

static void free_sh(JSRuntime *rt, JSOSSignalHandler *sh)
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

/* Install with SA_RESTART so a caught signal doesn't surface as EINTR inside
 * engine syscalls that predate the EINTR retry loops; the handler stays the atomic-flag one. */
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

static JSValue js_os_signal(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    JSRuntime *rt = JS_GetRuntime(ctx);
    JSThreadState *ts = JS_GetRuntimeOpaque(rt);
    JSOSSignalHandler *sh;
    uint32_t sig_num;
    JSValueConst func;
    sighandler_t handler;

    if (!is_main_thread(rt))
        return JS_ThrowTypeError(ctx, "signal handler can only be set in the main thread");

    if (JS_ToUint32(ctx, &sig_num, argv[0]))
        return JS_EXCEPTION;
    if (sig_num >= 64)
        return JS_ThrowRangeError(ctx, "invalid signal number");
    func = argv[1];
    /* func = null: SIG_DFL, func = undefined, SIG_IGN */
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
            return JS_ThrowTypeError(ctx, "not a function");
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
/* more portable, but does not work if the date is updated */
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

static JSValue js_os_now(JSContext *ctx, JSValue this_val,
                         int argc, JSValue *argv)
{
    /* Coarsen to ~1us (1000 ns) granularity so the value does not expose a
       full-precision CLOCK_MONOTONIC timing side channel (cache-timing attacks
       can use sub-microsecond deltas to probe cross-thread shared state; the
       audit flags this as a timing side channel). `ns / 1000 * 1000` rounds
       DOWN to the 1000 ns boundary in the integer domain. This is monotonic:
       raw CLOCK_MONOTONIC ns never decreases, and integer division by 1000
       followed by *1000 cannot make an equal-microsecond (or later) call
       return a smaller value -- two calls in the same 1000ns bucket aliases
       to the same value, and a later call in a later bucket is strictly
       greater. (A bitmask like `& ~999` is NOT equivalent: 1000 is not a
       power of two, so it neither lands on 1000ns boundaries nor stays
       non-decreasing for sub-microsecond raw deltas.) */
    uint64_t ns = (uint64_t)get_time_ns();
    ns = (ns / 1000) * 1000;
    return JS_NewFloat64(ctx, (double)ns / 1e6);
}

/* Free a timer's OWNED resources: the handler function, the captured trailing
   args, and the struct. This does NOT unlink from the list -- the caller may
   have already removed it (the fire path unlinks before invoking a one-shot
   timer so clearTimeout() from inside the callback cannot find and re-free
   it), or the timer may never have been inserted (the OOM path). */
static void free_timer_body(JSRuntime *rt, JSOSTimer *th)
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

static void free_timer(JSRuntime *rt, JSOSTimer *th)
{
    JSThreadState *ts = JS_GetRuntimeOpaque(rt);
    if (ts) ts->n_timers--;
    list_del(&th->link);
    free_timer_body(rt, th);
}

/* keep ts->os_timers sorted by ascending 'timeout' so the earliest timer is
   always at the head: firing the next due timer and computing the poll delay
   become O(1) instead of an O(n) rescan (and firing n due timers O(n) instead
   of O(n^2)). Ties keep insertion order (stable) so same-deadline timers fire
   FIFO, matching the HTML timer ordering. */
static void insert_timer_sorted(JSThreadState *ts, JSOSTimer *th);
static JSOSTimer *find_timer_by_id(JSThreadState *ts, int timer_id);

/* A hundred thousand live timers is not a workload, it is a flood: the sorted
   insert would make bulk setTimeout O(n^2) on the loop thread. */
#define DYN_TIMER_MAX_LIVE 100000
static void insert_timer_sorted(JSThreadState *ts, JSOSTimer *th)
{
    struct list_head *el;
    ts->n_timers++;
    list_for_each(el, &ts->os_timers) {
        JSOSTimer *e = list_entry(el, JSOSTimer, link);
        if (e->timeout > th->timeout)
            break;
    }
    /* list_add_tail(x, el) inserts x right before el; if the loop ran to the
       end 'el' is the list head, so this appends at the tail */
    list_add_tail(&th->link, el);
}

static JSValue js_os_setTimeout(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv, int magic)
{
    JSRuntime *rt = JS_GetRuntime(ctx);
    JSThreadState *ts = JS_GetRuntimeOpaque(rt);
    int64_t delay;
    JSValueConst func;
    JSOSTimer *th;

    /* setTimeout/setInterval are also exposed as globals, so guard against
       an embedder that installed helpers without js_std_init_handlers() */
    if (!ts)
        return JS_ThrowTypeError(ctx, "timers are not available in this context");
    func = argv[0];
    if (!JS_IsFunction(ctx, func))
        return JS_ThrowTypeError(ctx, "not a function");
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
        return JS_ThrowRangeError(ctx, "too many live timers (cap %d)",
                                  DYN_TIMER_MAX_LIVE);
    th = js_mallocz(ctx, sizeof(*th));
    if (!th)
        return JS_EXCEPTION;
    /* id uniqueness among LIVE timers: after INT32_MAX the wrap used to hand
       out ids identical to running ones, so clearTimeout hit the wrong one */
    do {
        th->timer_id = ts->next_timer_id;
        ts->next_timer_id++;
        if (ts->next_timer_id == INT32_MAX)
            ts->next_timer_id = 1;
    } while (find_timer_by_id(ts, th->timer_id));
    th->repeat = magic;
    /* HTML: nested timers clamp to 4ms; even non-nested 0 starves I/O so floor
       repeating 0 to 1ms */
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
    /* setTimeout(cb, ms, ...args) / setInterval: forward the trailing args
       to the callback; the d.ts declares ...args and a caller's data would
       otherwise be silently dropped at the fire site. */
    if (argc > 2) {
        int i;
        th->argc = argc - 2;
        th->args = js_mallocz(ctx, sizeof(JSValue) * (size_t)th->argc);
        if (!th->args) {
            free_timer_body(rt, th);   /* never inserted: don't unlink */
            return JS_EXCEPTION;
        }
        for (i = 0; i < th->argc; i++)
            th->args[i] = JS_DupValue(ctx, argv[i + 2]);
    }
    insert_timer_sorted(ts, th);
    return JS_NewInt32(ctx, th->timer_id);
}

static JSOSTimer *find_timer_by_id(JSThreadState *ts, int timer_id)
{
    struct list_head *el;
    if (timer_id <= 0)
        return NULL;
    list_for_each(el, &ts->os_timers) {
        JSOSTimer *th = list_entry(el, JSOSTimer, link);
        if (th->timer_id == timer_id)
            return th;
    }
    return NULL;
}

static JSValue js_os_clearTimeout(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    JSRuntime *rt = JS_GetRuntime(ctx);
    JSThreadState *ts = JS_GetRuntimeOpaque(rt);
    JSOSTimer *th;
    int timer_id;

    if (JS_ToInt32(ctx, &timer_id, argv[0]))
        return JS_EXCEPTION;
    th = find_timer_by_id(ts, timer_id);
    if (!th)
        return JS_UNDEFINED;
    free_timer(rt, th);
    return JS_UNDEFINED;
}

/* return a promise */
static JSValue js_os_sleepAsync(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    JSRuntime *rt = JS_GetRuntime(ctx);
    JSThreadState *ts = JS_GetRuntimeOpaque(rt);
    int64_t delay;
    JSOSTimer *th;
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
    insert_timer_sorted(ts, th);
    JS_FreeValue(ctx, resolving_funcs[0]);
    JS_FreeValue(ctx, resolving_funcs[1]);
    return promise;
}

static void call_handler(JSContext *ctx, JSValueConst func)
{
    JSValue ret, func1;
    /* 'func' might be destroyed when calling itself (if it frees the
       handler), so must take extra care */
    func1 = JS_DupValue(ctx, func);
    ret = JS_Call(ctx, func1, JS_UNDEFINED, 0, NULL);
    JS_FreeValue(ctx, func1);
    if (JS_IsException(ret))
        js_std_dump_error(ctx);
    JS_FreeValue(ctx, ret);
}

/* Call a timer handler with the trailing args captured at registration time.
   func/argc/args are borrowed; both the function and each arg are duplicated
   into refs this function owns, so a handler that clears its OWN timer mid-run
   (the setInterval -> clearInterval stop idiom) cannot free them out from under
   the in-flight JS_Call: a repeat timer stays in the sorted list while it runs,
   so clearTimeout/clearInterval CAN reach and free it during the call. */
static void call_timer(JSContext *ctx, JSValueConst func, int argc,
                       JSValueConst *args)
{
    JSRuntime *rt = JS_GetRuntime(ctx);
    JSValue ret, func1;
    JSValue *save = NULL;
    int i;

    func1 = JS_DupValue(ctx, func);
    if (argc > 0) {
        save = js_malloc_rt(rt, sizeof(JSValue) * (size_t)argc);
        if (save) {
            for (i = 0; i < argc; i++)
                save[i] = JS_DupValue(ctx, args[i]);
        } else {
            argc = 0;   /* OOM: drop the args rather than read freed memory */
        }
    }
    ret = JS_Call(ctx, func1, JS_UNDEFINED, argc, (JSValueConst *)save);
    JS_FreeValue(ctx, func1);
    if (save) {
        for (i = 0; i < argc; i++)
            JS_FreeValue(ctx, save[i]);
        js_free_rt(rt, save);
    }
    if (JS_IsException(ret))
        js_std_dump_error(ctx);
    JS_FreeValue(ctx, ret);
}

#ifdef USE_WORKER

#ifdef _WIN32

static int js_waker_init(JSWaker *w)
{
    w->handle = CreateEvent(NULL, TRUE, FALSE, NULL);
    return w->handle ? 0 : -1;
}

static void js_waker_signal(JSWaker *w)
{
    SetEvent(w->handle);
}

static void js_waker_clear(JSWaker *w)
{
    ResetEvent(w->handle);
}

static void js_waker_close(JSWaker *w)
{
    CloseHandle(w->handle);
    w->handle = INVALID_HANDLE_VALUE;
}

#else // !_WIN32

static int js_waker_init(JSWaker *w)
{
    int fds[2];

    if (pipe(fds) < 0)
        return -1;
    w->read_fd = fds[0];
    w->write_fd = fds[1];
    return 0;
}

static void js_waker_signal(JSWaker *w)
{
    int ret;

    for(;;) {
        ret = write(w->write_fd, "", 1);
        if (ret == 1)
            break;
        if (ret < 0 && errno != EAGAIN && errno != EINTR)
            break;
    }
}

static void js_waker_clear(JSWaker *w)
{
    uint8_t buf[16];
    int ret;

    for(;;) {
        ret = read(w->read_fd, buf, sizeof(buf));
        if (ret >= 0)
            break;
        if (errno != EAGAIN && errno != EINTR)
            break;
    }
}

static void js_waker_close(JSWaker *w)
{
    close(w->read_fd);
    close(w->write_fd);
    w->read_fd = -1;
    w->write_fd = -1;
}

#endif // _WIN32

static void js_free_message(JSWorkerMessage *msg);

/* return 1 if a message was handled, 0 if no message */
static int handle_posted_message(JSRuntime *rt, JSContext *ctx,
                                 JSWorkerMessageHandler *port)
{
    JSWorkerMessagePipe *ps = port->recv_pipe;
    int ret;
    struct list_head *el;
    JSWorkerMessage *msg;
    JSValue obj, data_obj, func, retval;

    pthread_mutex_lock(&ps->mutex);
    if (!list_empty(&ps->msg_queue)) {
        el = ps->msg_queue.next;
        msg = list_entry(el, JSWorkerMessage, link);

        /* remove the message from the queue */
        list_del(&msg->link);

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

        /* 'func' might be destroyed when calling itself (if it frees the
           handler), so must take extra care */
        func = JS_DupValue(ctx, port->on_message_func);
        retval = JS_Call(ctx, func, JS_UNDEFINED, 1, (JSValueConst *)&obj);
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
static int handle_posted_message(JSRuntime *rt, JSContext *ctx,
                                 JSWorkerMessageHandler *port)
{
    return 0;
}
#endif /* !USE_WORKER */

#if defined(_WIN32)

static int js_os_poll(JSContext *ctx)
{
    JSRuntime *rt = JS_GetRuntime(ctx);
    JSThreadState *ts = JS_GetRuntimeOpaque(rt);
    int min_delay, count;
    int64_t cur_time, delay;
    JSOSRWHandler *rh;
    struct list_head *el;
    HANDLE handles[MAXIMUM_WAIT_OBJECTS]; // 64

    /* XXX: handle signals if useful */

    if (list_empty(&ts->os_rw_handlers) && list_empty(&ts->os_timers) &&
        list_empty(&ts->port_list)) {
        return -1; /* no more events */
    }
    
    if (!list_empty(&ts->os_timers)) {
        /* timers are kept sorted by 'timeout', so the head is the earliest:
           check/fire it in O(1). Fire at most one timer per poll so the caller
           runs a microtask checkpoint between timer callbacks (task ordering). */
        JSOSTimer *th = list_entry(ts->os_timers.next, JSOSTimer, link);
        cur_time = get_time_ms();
        delay = th->timeout - cur_time;
        if (delay <= 0) {
            JSValue func;
            /* the timer expired */
            if (th->repeat) {
                /* re-arm and re-sort before the call: the handler may clear
                   this timer, so 'th' cannot be used afterwards. call_timer
                   duplicates func/args, so a self-clearing interval cannot
                   free the refs out from under the in-flight call. */
                th->timeout = cur_time + th->delay;
                list_del(&th->link);
                insert_timer_sorted(ts, th);
                call_timer(ctx, th->func, th->argc, (JSValueConst *)th->args);
                return 0;
            }
            /* One-shot: unlink and free the struct BEFORE running the callback.
               'func' might be destroyed by calling itself, and clearTimeout(id)
               inside the callback must not find this timer to re-free it. The
               func and args are salvaged first (the timer owns them) and freed
               after the call. */
            func = th->func;
            th->func = JS_UNDEFINED;
            {
                int nargs = th->argc;
                JSValue *args = th->args;
                th->argc = 0;
                th->args = NULL;
                free_timer(rt, th);
                call_timer(ctx, func, nargs, (JSValueConst *)args);
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
    list_for_each(el, &ts->os_rw_handlers) {
        rh = list_entry(el, JSOSRWHandler, link);
        if (rh->fd == 0 && !JS_IsNull(rh->rw_func[0])) {
            handles[count++] = (HANDLE)_get_osfhandle(rh->fd); // stdin
            if (count == (int)countof(handles))
                break;
        }
    }

    list_for_each(el, &ts->port_list) {
        JSWorkerMessageHandler *port = list_entry(el, JSWorkerMessageHandler, link);
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
            list_for_each(el, &ts->os_rw_handlers) {
                rh = list_entry(el, JSOSRWHandler, link);
                if (rh->fd == 0 && !JS_IsNull(rh->rw_func[0])) {
                    call_handler(ctx, rh->rw_func[0]);
                    /* must stop because the list may have been modified */
                    goto done;
                }
            }

            list_for_each(el, &ts->port_list) {
                JSWorkerMessageHandler *port = list_entry(el, JSWorkerMessageHandler, link);
                if (!JS_IsNull(port->on_message_func)) {
                    JSWorkerMessagePipe *ps = port->recv_pipe;
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

static no_inline int js_poll_expand(JSThreadState *ts)
{
    struct pollfd *new_fds;
    int new_size = max_int(ts->poll_fds_size +
                           ts->poll_fds_size / 2, 16);
    new_fds = realloc(ts->poll_fds, new_size * sizeof(struct pollfd));
    if (!new_fds)
        return -1;
    ts->poll_fds = new_fds;
    ts->poll_fds_size = new_size;
    return 0;
}

static int js_poll_add_poll_fd(JSThreadState *ts, int *pnfds, int fd, int events)
{
    struct pollfd *fds;
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

void js_std_set_io_reactor(JSContext *ctx, int fd,
                           void (*drain)(void *udata), void *udata)
{
    JSThreadState *ts = JS_GetRuntimeOpaque(JS_GetRuntime(ctx));
    if (!ts)
        return;
    ts->io_reactor_fd = fd;
    ts->io_reactor_drain = drain;
    ts->io_reactor_udata = udata;
}

static int js_os_poll(JSContext *ctx)
{
    JSRuntime *rt = JS_GetRuntime(ctx);
    JSThreadState *ts = JS_GetRuntimeOpaque(rt);
    int min_delay, nfds;
    int64_t cur_time, delay;
    JSOSRWHandler *rh;
    struct list_head *el, *el1;

    /* only check signals in the main thread */
    if (!ts->recv_pipe &&
        unlikely(atomic_load_explicit(&os_pending_signals, memory_order_relaxed) != 0)) {
        JSOSSignalHandler *sh;
        uint64_t mask;

        list_for_each(el, &ts->os_signal_handlers) {
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

    if (list_empty(&ts->os_rw_handlers) && list_empty(&ts->os_timers) &&
        list_empty(&ts->port_list) && ts->io_reactor_fd < 0)
        return -1; /* no more events */

    if (!list_empty(&ts->os_timers)) {
        /* timers are kept sorted by 'timeout', so the head is the earliest:
           check/fire it in O(1). Fire at most one timer per poll so the caller
           runs a microtask checkpoint between timer callbacks (task ordering). */
        JSOSTimer *th = list_entry(ts->os_timers.next, JSOSTimer, link);
        cur_time = get_time_ms();
        delay = th->timeout - cur_time;
        if (delay <= 0) {
            JSValue func;
            /* the timer expired */
            if (th->repeat) {
                /* re-arm and re-sort before the call: the handler may clear
                   this timer, so 'th' cannot be used afterwards. call_timer
                   duplicates func/args, so a self-clearing interval cannot
                   free the refs out from under the in-flight call. */
                th->timeout = cur_time + th->delay;
                list_del(&th->link);
                insert_timer_sorted(ts, th);
                call_timer(ctx, th->func, th->argc, (JSValueConst *)th->args);
                return 0;
            }
            /* One-shot: unlink and free the struct BEFORE running the callback.
               'func' might be destroyed by calling itself, and clearTimeout(id)
               inside the callback must not find this timer to re-free it. The
               func and args are salvaged first (the timer owns them) and freed
               after the call. */
            func = th->func;
            th->func = JS_UNDEFINED;
            {
                int nargs = th->argc;
                JSValue *args = th->args;
                th->argc = 0;
                th->args = NULL;
                free_timer(rt, th);
                call_timer(ctx, func, nargs, (JSValueConst *)args);
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
        min_delay = -1; /* infinite */
    }

    nfds = 0;
    list_for_each(el, &ts->os_rw_handlers) {
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

    list_for_each(el, &ts->port_list) {
        JSWorkerMessageHandler *port = list_entry(el, JSWorkerMessageHandler, link);
        if (!JS_IsNull(port->on_message_func)) {
            JSWorkerMessagePipe *ps = port->recv_pipe;
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

    nfds = poll(ts->poll_fds, nfds, min_delay);
    if (nfds > 0) {
        /* The IO reactor is drained first: one readable signal reaps ALL ready
         * completions (dyn_evloop_poll(0)), each enqueuing JS jobs that run when
         * this returns and the caller pumps the microtask/job queue. */
        if (reactor_pfd_index >= 0 &&
            (ts->poll_fds[reactor_pfd_index].revents &
             (POLLIN | POLLERR | POLLHUP | POLLNVAL))) {
            ts->io_reactor_drain(ts->io_reactor_udata);
            goto done;
        }
        list_for_each_safe(el, el1, &ts->os_rw_handlers) {
            rh = list_entry(el, JSOSRWHandler, link);

            /* A closed (or never-opened) descriptor reports POLLNVAL: the
               slot is dead. Drop it and never dispatch, so the loop cannot
               busy-spin re-dispatching the stale callback and a later fd
               reuse cannot deliver it to a foreign descriptor. Any
               subsequent handler entries keep their own poll_fd_index, so
               removal is safe mid-iteration. */
            if (ts->poll_fds[rh->poll_fd_index].revents & POLLNVAL) {
                free_rw_handler(rt, rh);
                continue;
            }

            if (!JS_IsNull(rh->rw_func[0]) &&
                (ts->poll_fds[rh->poll_fd_index].revents & (POLLERR | POLLHUP | POLLIN))) {
                call_handler(ctx, rh->rw_func[0]);
                /* must stop because the list may have been modified */
                goto done;
            }
            if (!JS_IsNull(rh->rw_func[1]) &&
                (ts->poll_fds[rh->poll_fd_index].revents & (POLLERR | POLLHUP | POLLOUT))) {
                call_handler(ctx, rh->rw_func[1]);
                /* must stop because the list may have been modified */
                goto done;
            }
        }

        list_for_each(el, &ts->port_list) {
            JSWorkerMessageHandler *port = list_entry(el, JSWorkerMessageHandler, link);
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
#endif /* !_WIN32 */

static JSValue make_obj_error(JSContext *ctx,
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

static JSValue make_string_error(JSContext *ctx,
                                 const char *buf,
                                 int err)
{
    return make_obj_error(ctx, JS_NewString(ctx, buf), err);
}

/* return [cwd, errorcode] */
static JSValue js_os_getcwd(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
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

static JSValue js_os_chdir(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    const char *target;
    int err;

    target = JS_ToCString(ctx, argv[0]);
    if (!target)
        return JS_EXCEPTION;
    err = js_get_errno(chdir(target));
    JS_FreeCString(ctx, target);
    return JS_NewInt32(ctx, err);
}

static JSValue js_os_mkdir(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    int mode, ret;
    const char *path;

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

/* return [array, errorcode] */
static JSValue js_os_readdir(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    const char *path;
    DIR *f;
    struct dirent *d;
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
    for(;;) {
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
static int64_t timespec_to_ms(const struct timespec *tv)
{
    return (int64_t)tv->tv_sec * 1000 + (tv->tv_nsec / 1000000);
}
#endif

/* return [obj, errcode] */
static JSValue js_os_stat(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv, int is_lstat)
{
    const char *path;
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
static void ms_to_timeval(struct timeval *tv, uint64_t v)
{
    tv->tv_sec = v / 1000;
    tv->tv_usec = (v % 1000) * 1000;
}
#endif

static JSValue js_os_utimes(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    const char *path;
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

/* sleep(delay_ms) */
static JSValue js_os_sleep(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
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
        while ((ret = nanosleep(&ts, &rem)) < 0 && errno == EINTR) ts = rem;
        ret = js_get_errno(ret);
    }
#endif
    return JS_NewInt32(ctx, ret);
}

#if defined(_WIN32)
static char *realpath(const char *path, char *buf)
{
    if (!_fullpath(buf, path, PATH_MAX)) {
        errno = ENOENT;
        return NULL;
    } else {
        return buf;
    }
}
#endif

/* return [path, errorcode] */
static JSValue js_os_realpath(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    const char *path;
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
static JSValue js_os_symlink(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
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

/* return [path, errorcode] */
static JSValue js_os_readlink(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    const char *path;
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

static char **build_envp(JSContext *ctx, JSValueConst obj)
{
    uint32_t len, i;
    JSPropertyEnum *tab;
    char **envp, *pair;
    const char *key, *str;
    JSValue val;
    size_t key_len, str_len;

    if (JS_GetOwnPropertyNames(ctx, &tab, &len, obj,
                               JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0)
        return NULL;
    envp = js_mallocz(ctx, sizeof(envp[0]) * ((size_t)len + 1));
    if (!envp)
        goto fail;
    for(i = 0; i < len; i++) {
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
        for(i = 0; i < len; i++)
            js_free(ctx, envp[i]);
        js_free(ctx, envp);
        envp = NULL;
    }
    goto done;
}

/* DYN_SPAWN_EXTRACT_BEGIN — os.exec child-spawn implementation.
 *
 * Everything from the BEGIN marker here to the END marker below is plain
 * libc (no JSContext, no quickjs headers): tests/test_exec_spawn.c builds
 * this exact region as a standalone TU (build recipe in that file) so the
 * spawn mapping is pinned without the engine. Keep it libc-only.
 *
 * Audit M35-01: os.exec used to fork() and then run dup2/closefrom/chdir/
 * setgid/setuid in the child. In a process that hosts live threads (http
 * workers, dyn-pool) any libc lock held by another thread at fork time is
 * held forever in the single-threaded child — the classic fork-in-thread
 * deadlock. The spawn path below hands the whole child setup to posix_spawn
 * file actions/attributes, which the libc runs in a vfork-style child that
 * never allocates against the parent's locks. The original fork child
 * survives below, verbatim, only for requests a libc's posix_spawn cannot
 * express (uid/gid without POSIX_SPAWN_SETUID/SETGID, cwd without
 * addchdir_np).
 *
 * Probed behavior this mapping relies on (glibc 2.41, musl 1.2.5, macOS 26):
 *  - posix_spawn/posix_spawnp report exec-class failures (missing binary,
 *    EACCES, unusable cwd) synchronously as the return errno and leave NO
 *    child behind;
 *  - a failing dup2() file action is silently IGNORED by all three libcs
 *    (the forked child died 127 there), so std fds are validated here;
 *  - posix_spawnp searches the CALLER's environ PATH, like the old
 *    my_execvpe() getenv("PATH"), not the passed envp;
 *  - none of the three defines POSIX_SPAWN_SETUID/POSIX_SPAWN_SETGID. */
/* Re-included from the file header, listed again so the extracted region
 * compiles standalone (see tests/test_exec_spawn.c). */
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

/* posix_spawn_file_actions_addchdir_np: glibc >= 2.29, musl >= 1.1.24,
 * macOS >= 10.15. Detected per known libc; anything unrecognized keeps the
 * fork fallback for the cwd case only. */
#if defined(__APPLE__)
#define DYN_SPAWN_HAVE_ADDCHDIR 1
#elif defined(__GLIBC__) && defined(__GLIBC_PREREQ)
#if __GLIBC_PREREQ(2, 29)
#define DYN_SPAWN_HAVE_ADDCHDIR 1
#endif
#elif defined(__linux__) && !defined(__GLIBC__)
#define DYN_SPAWN_HAVE_ADDCHDIR 1 /* musl (>= 1.1.24) */
#endif

/* posix_spawn_file_actions_addclosefrom_np: glibc >= 2.34 only. Where it is
 * missing the spawn path enqueues the same bounded close loop the fork child
 * used (3..min(_SC_OPEN_MAX,1024)); fds >= 1024 still leak into the child
 * exactly as before (audit M35-02's deliberate Alpine tradeoff). */
#if defined(__GLIBC__) && defined(__GLIBC_PREREQ)
#if __GLIBC_PREREQ(2, 34)
#define DYN_SPAWN_HAVE_ADDCLOSEFROM 1
#endif
#endif

/* execvpe is not available on non GNU systems */
static int my_execvpe(const char *filename, char **argv, char **envp)
{
    char *path, *p, *p_next, *p1;
    char buf[PATH_MAX];
    size_t filename_len, path_len;
    int eacces_error; /* plain int (was BOOL): keeps this region
                               compilable without the engine headers */

    filename_len = strlen(filename);
    if (filename_len == 0) {
        errno = ENOENT;
        return -1;
    }
    if (strchr(filename, '/'))
        return execve(filename, argv, envp);

    path = getenv("PATH");
    if (!path)
        path = (char *)"/bin:/usr/bin";
    eacces_error = 0;
    p = path;
    for(p = path; p != NULL; p = p_next) {
        /* One pass: strchr walked to the terminator, strlen walked it again. */
        p1 = p;
        while (*p1 && *p1 != ':') p1++;
        path_len = p1 - p;
        if (*p1) { p_next = p1 + 1; } else { p_next = NULL; p1 = NULL; }
        /* path too long */
        if ((path_len + 1 + filename_len + 1) > PATH_MAX)
            continue;
        memcpy(buf, p, path_len);
        buf[path_len] = '/';
        memcpy(buf + path_len + 1, filename, filename_len);
        buf[path_len + 1 + filename_len] = '\0';

        execve(buf, argv, envp);

        switch(errno) {
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

/* The pre-spawn child, verbatim: only used where the target libc's
 * posix_spawn cannot express the request (uid/gid without attr support,
 * cwd without addchdir_np). Returns 0 with *out set to the child pid, or
 * the errno. Every child failure path is _exit(127), never exit(): the
 * forked child must not touch stdio/atexit. */
static int os_exec_fork_child(const char *file, char **argv, char **envp,
                              int std_fds[3], const char *cwd,
                              uid_t uid, gid_t gid, int use_path, pid_t *out)
{
    pid_t pid;
    int i;

    pid = fork();
    if (pid < 0)
        return errno;
    if (pid == 0) {
        /* child */

        /* remap the stdin/stdout/stderr handles if necessary */
        for (i = 0; i < 3; i++) {
            if (std_fds[i] != i) {
                if (dup2(std_fds[i], i) < 0)
                    _exit(127);
            }
        }
#if defined(HAVE_CLOSEFROM)
        /* closefrom() is available on many recent unix systems:
           Linux with glibc 2.34+, Solaris 9+, FreeBSD 7.3+,
           NetBSD 3.0+, OpenBSD 3.5+.
           Linux with the musl libc and macOS don't have it.
         */

        closefrom(3);
#elif defined(__linux__) && defined(SYS_close_range)
        /* Audit M35-02's residue: the 1024-capped loop below leaks every
           fd >= 1024 into the child. Linux >= 5.9 closes the WHOLE range in
           one kernel call; SYS_close_range is undefined on older headers,
           so the loop stays as the fallback for them (and for non-Linux). */
        syscall(SYS_close_range, 3, ~0U, 0);
#else
        {
            /* Close the file handles manually, limit to 1024 to avoid
               costly loop on linux Alpine where sysconf(_SC_OPEN_MAX)
               returns a huge value 1048576.
               Patch inspired by nicolas-duteil-nova. See also:
               https://stackoverflow.com/questions/73229353/
               https://stackoverflow.com/questions/899038/#918469
             */
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

/* Spawn file with argv/envp, remapping std_fds into 0/1/2, chdir(cwd) and
 * dropping to uid/gid in the new process — the M35-01 posix_spawn mapping of
 * the fork child above. use_path selects posix_spawnp (the execvp analog,
 * PATH taken from the caller's environ like the old my_execvpe) over
 * posix_spawn. Returns 0 with *out set to the child pid, or the errno; the
 * caller decides how the errno maps to os.exec's observable result. */
static int os_exec_spawn_child(const char *file, char **argv, char **envp,
                               int std_fds[3], const char *cwd,
                               uid_t uid, gid_t gid, int use_path, pid_t *out)
{
    posix_spawn_file_actions_t fa;
    posix_spawnattr_t sa;
    short flags = 0;
    int i, rc;
    pid_t pid = 0;

    /* uid/gid: no spawnattr flags on glibc/musl/macOS (probed: none defines
       POSIX_SPAWN_SETUID/SETGID), so privilege requests keep the fork child
       — the one fork left in os.exec, and only when actually asked for. */
#if defined(POSIX_SPAWN_SETUID) && defined(POSIX_SPAWN_SETGID)
    /* Where the flags exist, prefer them: the libc applies the group before
       the user, same privilege-drop order as the fork child. */
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

    /* The probed libcs silently ignore a dup2() file action whose source fd
       is invalid, where the fork child died 127 at the dup2. Validate the
       redirected std fds here so that edge keeps its old observable. */
    for (i = 0; i < 3; i++) {
        if (std_fds[i] != i && fcntl(std_fds[i], F_GETFD) < 0)
            return errno ? errno : EBADF;
    }

    rc = posix_spawn_file_actions_init(&fa);
    if (rc == 0)
        rc = posix_spawnattr_init(&sa);
    /* File actions run in the child in the order added — keep the fork
       child's exact order: dup2, then close, then chdir (dup2 first so the
       close pass cannot kill a source fd the remap still needs). */
#if defined(__APPLE__) && defined(POSIX_SPAWN_CLOEXEC_DEFAULT)
    /* macOS has no addclosefrom_np, and its posix_spawn FAILS the whole
       spawn with EBADF if a close action names a not-open fd, so a plain
       close loop is out. Ask the kernel instead: CLOEXEC_DEFAULT marks
       everything the child inherits close-on-exec; a self-dup2 exempts the
       std slots that are not remapped, and dup2 sources are closed too
       (probed on this SDK). One deliberate delta from the old 3..1024 loop:
       fds >= 1024 no longer leak into the child here — audit M35-02's
       documented tradeoff narrows to the loop platforms. */
    for (i = 0; rc == 0 && i < 3; i++) {
        if (std_fds[i] != i)
            rc = posix_spawn_file_actions_adddup2(&fa, std_fds[i], i);
        else
            rc = posix_spawn_file_actions_adddup2(&fa, i, i); /* keep slot */
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
        /* No addclosefrom_np: enqueue the fork child's bounded close loop.
           glibc < 2.34 and musl tolerate a close action on a not-open fd;
           an unknown libc may reject it the way macOS does (probed), so
           probe fds there first. The actions replay in the child right
           before exec, so the parent only pays for building the list. */
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
#endif /* __APPLE__ && POSIX_SPAWN_CLOEXEC_DEFAULT */
#ifdef DYN_SPAWN_HAVE_ADDCHDIR
    if (rc == 0 && cwd) {
#ifdef __APPLE__
        /* addchdir_np is deprecated by the macOS 26 SDK in favor of the
           POSIX.1-2024 spelling addchdir(); keep _np for older targets
           (>= 10.15) where only it exists. */
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
    /* Vestigial on modern glibc/musl (their spawn already runs the child on
       a CLONE_VM|CLONE_VFORK stack); requested where defined. */
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
/* DYN_SPAWN_EXTRACT_END */

/* exec(args[, options]) -> exitcode */
static JSValue js_os_exec(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv)
{
    JSValueConst options, args = argv[0];
    JSValue val, ret_val;
    const char **exec_argv, *file = NULL, *str, *cwd = NULL;
    char **envp = environ;
    uint32_t exec_argc, i;
    int ret, status;
    pid_t pid;
    BOOL block_flag = TRUE, use_path = TRUE;
    int64_t timeout_ms = 0;
    static const char *std_name[3] = { "stdin", "stdout", "stderr" };
    int std_fds[3];
    uint32_t uid = -1, gid = -1;

    val = JS_GetPropertyStr(ctx, args, "length");
    if (JS_IsException(val))
        return JS_EXCEPTION;
    ret = JS_ToUint32(ctx, &exec_argc, val);
    JS_FreeValue(ctx, val);
    if (ret)
        return JS_EXCEPTION;
    /* arbitrary limit to avoid overflow */
    if (exec_argc < 1 || exec_argc > 65535) {
        return JS_ThrowTypeError(ctx, "invalid number of arguments");
    }
    exec_argv = js_mallocz(ctx, sizeof(exec_argv[0]) * (exec_argc + 1));
    if (!exec_argv)
        return JS_EXCEPTION;
    for(i = 0; i < exec_argc; i++) {
        val = JS_GetPropertyUint32(ctx, args, i);
        if (JS_IsException(val))
            goto exception;
        str = JS_ToCString(ctx, val);
        JS_FreeValue(ctx, val);
        if (!str)
            goto exception;
        exec_argv[i] = str;
    }
    exec_argv[exec_argc] = NULL;

    for(i = 0; i < 3; i++)
        std_fds[i] = i;

    /* get the options, if any */
    if (argc >= 2) {
        options = argv[1];

        /* "blocking" is the documented spelling (types/dynajs.d.ts); "block"
           is the historical one -- accept both, documented spelling primary. */
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
                JS_ThrowRangeError(ctx, "os.exec timeout must not be negative");
                goto exception;
            }
        }
        JS_FreeValue(ctx, val);

        val = JS_GetPropertyStr(ctx, options, "file");
        if (JS_IsException(val))
            goto exception;
        if (!JS_IsUndefined(val)) {
            file = JS_ToCString(ctx, val);
            JS_FreeValue(ctx, val);
            if (!file)
                goto exception;
        }

        val = JS_GetPropertyStr(ctx, options, "cwd");
        if (JS_IsException(val))
            goto exception;
        if (!JS_IsUndefined(val)) {
            cwd = JS_ToCString(ctx, val);
            JS_FreeValue(ctx, val);
            if (!cwd)
                goto exception;
        }

        /* stdin/stdout/stderr handles */
        for(i = 0; i < 3; i++) {
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

    /* Spawn without fork (audit M35-01): the libc performs the child-side
       setup from file actions/attributes in a child that never allocates
       against this process's locks. */
    {
        /* js_os_exec keeps its own CString ownership; resolve argv[0] here
           instead of aliasing `file` (which would double-free it below). */
        const char *spawn_file = file ? file : exec_argv[0];
        ret = os_exec_spawn_child(spawn_file, (char **)exec_argv, envp,
                                  std_fds, cwd, (uid_t)uid, (gid_t)gid,
                                  use_path, &pid);
    }
    if (ret != 0) {
        if (ret == EAGAIN || ret == ENOMEM || ret == EPERM || ret == EINVAL) {
            /* the process could not be created at all — the one failure the
               fork() implementation surfaced as an exception */
            JS_ThrowTypeError(ctx, "fork error");
            goto exception;
        }
        /* Everything else is a failure the forked child answered with
           _exit(127): binary missing or not executable (PATH or direct),
           unusable cwd, invalid std fd, oversized argv/env. posix_spawn
           reports those synchronously as an errno instead of leaving a
           127-exiting child behind (probed on glibc/musl/macOS), so
           blocking mode returns the same 127. Non-blocking mode has no
           child to hand back — the old code returned a pid doomed to exit
           127 — so surface the error rather than invent a pid. */
        if (!block_flag) {
            JS_ThrowTypeError(ctx, "fork error");
            goto exception;
        }
        ret_val = JS_NewInt32(ctx, 127);
        goto done;
    }
    /* parent */
    if (block_flag) {
        /* Bounded, EINTR-aware wait. A blocking waitpid(pid,0) ignores any
           deadline, so poll with WNOHANG and a short sleep; when the timeout is
           reached, escalate SIGTERM -> SIGKILL and stop waiting. This also
           bounds the case where a grandchild holds the output pipe: os.exec does
           not drain pipes, so reaping the direct child is all we can do, and we
           never let the wait stretch past the deadline plus a short grace. */
        int64_t start = get_time_ms();
        int64_t grace = timeout_ms > 0 ? DYN_EXEC_TIMEOUT_GRACE_MS : 0;
        pid_t wret;

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
                continue;               /* stopped/continued: keep polling */
            }
            if (wret < 0) {
                if (errno == EINTR)
                    continue;
                ret = -errno;           /* ECHILD: already reaped */
                break;
            }
            /* wret == 0: still running. If a deadline is set and we are past it,
               escalate and give up -- do not wait forever. */
            if (timeout_ms > 0 && get_time_ms() >= start + timeout_ms) {
                if (kill(pid, SIGTERM) == 0) {
                    /* give the SIGTERM the grace window to land */
                    struct timespec ts = { grace / 1000, (grace % 1000) * 1000000L };
                    nanosleep(&ts, NULL);
                    if (waitpid(pid, &status, WNOHANG) != pid)
                        kill(pid, SIGKILL);
                }
                ret = -SIGTERM;         /* reflect the timeout as a signal */
                break;
            }
            {
                struct timespec ts = { 0, 1000000L };   /* 1 ms */
                nanosleep(&ts, NULL);
            }
        }
    } else {
        ret = pid;
    }
    ret_val = JS_NewInt32(ctx, ret);
 done:
    JS_FreeCString(ctx, file);
    JS_FreeCString(ctx, cwd);
    for(i = 0; i < exec_argc; i++)
        JS_FreeCString(ctx, exec_argv[i]);
    js_free(ctx, exec_argv);
    if (envp && envp != environ) {
        char **p;
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

/* getpid() -> pid */
static JSValue js_os_getpid(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    return JS_NewInt32(ctx, getpid());
}

/* waitpid(pid, block) -> [pid, status] */
static JSValue js_os_waitpid(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
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

/* pipe() -> [read_fd, write_fd] or null if error */
static JSValue js_os_pipe(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv)
{
    int pipe_fds[2], ret;
    JSValue obj;

    ret = pipe(pipe_fds);
    if (ret < 0)
        return JS_NULL;
    /* CLOEXEC at source: the read/write ends are engine-owned and must not
       leak into an os.exec() child. Callers passing an end as a child's
       std{in,out,err} still get it intact -- the child dup2()s it onto 0/1/2,
       which clears FD_CLOEXEC on the target. */
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

/* kill(pid, sig) */
static JSValue js_os_kill(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv)
{
    int pid, sig, ret;

    if (JS_ToInt32(ctx, &pid, argv[0]))
        return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &sig, argv[1]))
        return JS_EXCEPTION;
    ret = js_get_errno(kill(pid, sig));
    return JS_NewInt32(ctx, ret);
}

/* dup(fd) */
static JSValue js_os_dup(JSContext *ctx, JSValueConst this_val,
                         int argc, JSValueConst *argv)
{
    int fd, ret;

    if (JS_ToInt32(ctx, &fd, argv[0]))
        return JS_EXCEPTION;
    /* F_DUPFD_CLOEXEC keeps the duplicate close-on-exec: a dup made by an
       os.exec()-using caller must not leak into the spawned child. */
    ret = js_get_errno(fcntl(fd, F_DUPFD_CLOEXEC, 3));
    return JS_NewInt32(ctx, ret);
}

/* dup2(fd) */
static JSValue js_os_dup2(JSContext *ctx, JSValueConst this_val,
                         int argc, JSValueConst *argv)
{
    int fd, fd2, ret;

    if (JS_ToInt32(ctx, &fd, argv[0]))
        return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &fd2, argv[1]))
        return JS_EXCEPTION;
    /* dup2(fd, fd2) atomically closes fd2 (if it is an open descriptor) and
       points it at fd. If fd2 had a read/write poll slot, that slot is now
       dangling and must be dropped before the descriptor is released, else
       the loop re-dispatches the stale callback (POLLNVAL spin / wrong-cb on
       reuse). The dup'ed fd does NOT inherit the handler slot, so only fd2 is
       cleared. */
    if (fd != fd2)
        js_os_clear_handler(ctx, fd2);
    ret = js_get_errno(dup2(fd, fd2));
    return JS_NewInt32(ctx, ret);
}

#endif /* !_WIN32 */

#ifdef USE_WORKER

/* Worker */

typedef struct {
    JSWorkerMessagePipe *recv_pipe;
    JSWorkerMessagePipe *send_pipe;
    JSWorkerMessageHandler *msg_handler;
} JSWorkerData;

typedef struct {
    char *filename; /* module filename */
    char *basename; /* module base name */
    JSWorkerMessagePipe *recv_pipe, *send_pipe;
    int strip_flags;
} WorkerFuncArgs;

typedef struct {
    int ref_count;
    struct list_head link; /* process-wide live-SAB registry (see below) */
    uint64_t buf[0];
} JSSABHeader;

/* Process-wide registry of live SAB data pointers. The deserializer
   (JS_ReadSharedArrayBuffer) refuses a stream-embedded pointer that is not
   one of these: only this allocator can mint SAB data pointers, so any other
   u64 in a blob is an external/attacker pointer. Mutex-protected because SABs
   are shared across worker threads. */
static struct list_head sab_registry = LIST_HEAD_INIT(sab_registry);
static pthread_mutex_t sab_registry_mutex = PTHREAD_MUTEX_INITIALIZER;

static JSClassID js_worker_class_id;
static JSContext *(*js_worker_new_context_func)(JSRuntime *rt);

static int atomic_add_int(int *ptr, int v)
{
    return atomic_fetch_add((_Atomic(uint32_t) *)ptr, v) + v;
}

/* shared array buffer allocator */
static void *js_sab_alloc(void *opaque, size_t size)
{
    JSSABHeader *sab;
    sab = malloc(sizeof(JSSABHeader) + size);
    if (!sab)
        return NULL;
    sab->ref_count = 1;
    pthread_mutex_lock(&sab_registry_mutex);
    list_add_tail(&sab->link, &sab_registry);
    pthread_mutex_unlock(&sab_registry_mutex);
    return sab->buf;
}

static void js_sab_free(void *opaque, void *ptr)
{
    JSSABHeader *sab;
    int ref_count;
    sab = (JSSABHeader *)((uint8_t *)ptr - sizeof(JSSABHeader));
    ref_count = atomic_add_int(&sab->ref_count, -1);
    assert(ref_count >= 0);
    if (ref_count == 0) {
        pthread_mutex_lock(&sab_registry_mutex);
        list_del(&sab->link);
        pthread_mutex_unlock(&sab_registry_mutex);
        free(sab);
    }
}

static void js_sab_dup(void *opaque, void *ptr)
{
    JSSABHeader *sab;
    sab = (JSSABHeader *)((uint8_t *)ptr - sizeof(JSSABHeader));
    atomic_add_int(&sab->ref_count, 1);
}

/* deserializer probe: is ptr a live SAB data pointer of this process? */
static int js_sab_valid_ptr(void *opaque, void *ptr)
{
    struct list_head *el;
    int valid = 0;

    pthread_mutex_lock(&sab_registry_mutex);
    list_for_each(el, &sab_registry) {
        JSSABHeader *sab = list_entry(el, JSSABHeader, link);
        if (sab->buf == ptr) {
            valid = 1;
            break;
        }
    }
    pthread_mutex_unlock(&sab_registry_mutex);
    return valid;
}

static JSWorkerMessagePipe *js_new_message_pipe(void)
{
    JSWorkerMessagePipe *ps;

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

static JSWorkerMessagePipe *js_dup_message_pipe(JSWorkerMessagePipe *ps)
{
    atomic_add_int(&ps->ref_count, 1);
    return ps;
}

static void js_free_message(JSWorkerMessage *msg)
{
    size_t i;
    /* free the SAB */
    for(i = 0; i < msg->sab_tab_len; i++) {
        js_sab_free(NULL, msg->sab_tab[i]);
    }
    free(msg->sab_tab);
    free(msg->data);
    free(msg);
}

static void js_free_message_pipe(JSWorkerMessagePipe *ps)
{
    struct list_head *el, *el1;
    JSWorkerMessage *msg;
    int ref_count;

    if (!ps)
        return;

    ref_count = atomic_add_int(&ps->ref_count, -1);
    assert(ref_count >= 0);
    if (ref_count == 0) {
        list_for_each_safe(el, el1, &ps->msg_queue) {
            msg = list_entry(el, JSWorkerMessage, link);
            js_free_message(msg);
        }
        pthread_mutex_destroy(&ps->mutex);
        js_waker_close(&ps->waker);
        free(ps);
    }
}

static void js_free_port(JSRuntime *rt, JSWorkerMessageHandler *port)
{
    if (port) {
        js_free_message_pipe(port->recv_pipe);
        JS_FreeValueRT(rt, port->on_message_func);
        if (port->link.prev)
            list_del(&port->link);
        js_free_rt(rt, port);
    }
}

static void js_worker_finalizer(JSRuntime *rt, JSValue val)
{
    JSWorkerData *worker = JS_GetOpaque(val, js_worker_class_id);
    if (worker) {
        js_free_message_pipe(worker->recv_pipe);
        js_free_message_pipe(worker->send_pipe);
        js_free_port(rt, worker->msg_handler);
        js_free_rt(rt, worker);
    }
}

static void js_worker_mark(JSRuntime *rt, JSValueConst val,
                           JS_MarkFunc *mark_func)
{
    JSWorkerData *worker = JS_GetOpaque(val, js_worker_class_id);
    if (worker) {
        JSWorkerMessageHandler *port = worker->msg_handler;
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

static void *worker_func(void *opaque)
{
    WorkerFuncArgs *args = opaque;
    JSRuntime *rt;
    JSThreadState *ts;
    JSContext *ctx;
    JSValue val;

    rt = JS_NewRuntime();
    if (rt == NULL) {
        fprintf(stderr, "JS_NewRuntime failure");
        exit(1);
    }
    JS_SetStripInfo(rt, args->strip_flags);
    js_std_init_handlers(rt);

    JS_SetModuleLoaderFunc2(rt, NULL, js_module_loader, js_module_check_attributes, NULL);

    /* set the pipe to communicate with the parent */
    ts = JS_GetRuntimeOpaque(rt);
    ts->recv_pipe = args->recv_pipe;
    ts->send_pipe = args->send_pipe;

    /* function pointer to avoid linking the whole JS_NewContext() if
       not needed */
    ctx = js_worker_new_context_func(rt);
    if (ctx == NULL) {
        fprintf(stderr, "JS_NewContext failure");
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

    JS_FreeContext(ctx);
    js_std_free_handlers(rt);
    JS_FreeRuntime(rt);
    return NULL;
}

static JSValue js_worker_ctor_internal(JSContext *ctx, JSValueConst new_target,
                                       JSWorkerMessagePipe *recv_pipe,
                                       JSWorkerMessagePipe *send_pipe)
{
    JSValue obj = JS_UNDEFINED, proto;
    JSWorkerData *s;

    /* create the object */
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

static JSValue js_worker_ctor(JSContext *ctx, JSValueConst new_target,
                              int argc, JSValueConst *argv)
{
    JSRuntime *rt = JS_GetRuntime(ctx);
    WorkerFuncArgs *args = NULL;
    pthread_t tid;
    pthread_attr_t attr;
    JSValue obj = JS_UNDEFINED;
    int ret;
    const char *filename = NULL, *basename;
    JSAtom basename_atom;

    /* XXX: in order to avoid problems with resource liberation, we
       don't support creating workers inside workers */
    if (!is_main_thread(rt))
        return JS_ThrowTypeError(ctx, "cannot create a worker inside a worker");

    /* base name, assuming the calling function is a normal JS
       function */
    basename_atom = JS_GetScriptOrModuleName(ctx, 1);
    if (basename_atom == JS_ATOM_NULL) {
        return JS_ThrowTypeError(ctx, "could not determine calling script or module name");
    }
    basename = JS_AtomToCString(ctx, basename_atom);
    JS_FreeAtom(ctx, basename_atom);
    if (!basename)
        goto fail;

    /* module name */
    filename = JS_ToCString(ctx, argv[0]);
    if (!filename)
        goto fail;

    args = malloc(sizeof(*args));
    if (!args)
        goto oom_fail;
    memset(args, 0, sizeof(*args));
    args->filename = strdup(filename);
    args->basename = strdup(basename);

    /* ports */
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

    pthread_attr_init(&attr);
    /* no join at the end */
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    ret = pthread_create(&tid, &attr, worker_func, args);
    pthread_attr_destroy(&attr);
    if (ret != 0) {
        JS_ThrowTypeError(ctx, "could not create worker");
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

static JSValue js_worker_postMessage(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    JSWorkerData *worker = JS_GetOpaque2(ctx, this_val, js_worker_class_id);
    JSWorkerMessagePipe *ps;
    size_t data_len, sab_tab_len, i;
    uint8_t *data;
    JSWorkerMessage *msg;
    uint8_t **sab_tab;

    if (!worker)
        return JS_EXCEPTION;

    data = JS_WriteObject2(ctx, &data_len, argv[0],
                           JS_WRITE_OBJ_SAB | JS_WRITE_OBJ_REFERENCE,
                           &sab_tab, &sab_tab_len);
    if (!data)
        return JS_EXCEPTION;

    msg = malloc(sizeof(*msg));
    if (!msg)
        goto fail;
    msg->data = NULL;
    msg->sab_tab = NULL;

    /* must reallocate because the allocator may be different */
    msg->data = malloc(data_len);
    if (!msg->data)
        goto fail;
    memcpy(msg->data, data, data_len);
    msg->data_len = data_len;

    if (sab_tab_len > 0) {
        msg->sab_tab = malloc(sizeof(msg->sab_tab[0]) * sab_tab_len);
        if (!msg->sab_tab)
            goto fail;
        memcpy(msg->sab_tab, sab_tab, sizeof(msg->sab_tab[0]) * sab_tab_len);
    }
    msg->sab_tab_len = sab_tab_len;

    js_free(ctx, data);
    js_free(ctx, sab_tab);

    /* increment the SAB reference counts */
    for(i = 0; i < msg->sab_tab_len; i++) {
        js_sab_dup(NULL, msg->sab_tab[i]);
    }

    ps = worker->send_pipe;
    pthread_mutex_lock(&ps->mutex);
    /* indicate that data is present */
    if (list_empty(&ps->msg_queue))
        js_waker_signal(&ps->waker);
    list_add_tail(&msg->link, &ps->msg_queue);
    pthread_mutex_unlock(&ps->mutex);
    return JS_UNDEFINED;
 fail:
    if (msg) {
        free(msg->data);
        free(msg->sab_tab);
        free(msg);
    }
    js_free(ctx, data);
    js_free(ctx, sab_tab);
    return JS_EXCEPTION;

}

static JSValue js_worker_set_onmessage(JSContext *ctx, JSValueConst this_val,
                                   JSValueConst func)
{
    JSRuntime *rt = JS_GetRuntime(ctx);
    JSThreadState *ts = JS_GetRuntimeOpaque(rt);
    JSWorkerData *worker = JS_GetOpaque2(ctx, this_val, js_worker_class_id);
    JSWorkerMessageHandler *port;

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
            return JS_ThrowTypeError(ctx, "not a function");
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

static JSValue js_worker_get_onmessage(JSContext *ctx, JSValueConst this_val)
{
    JSWorkerData *worker = JS_GetOpaque2(ctx, this_val, js_worker_class_id);
    JSWorkerMessageHandler *port;
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
    JS_CFUNC_DEF("postMessage", 1, js_worker_postMessage ),
    JS_CGETSET_DEF("onmessage", js_worker_get_onmessage, js_worker_set_onmessage ),
};

#endif /* USE_WORKER */

void js_std_set_worker_new_context_func(JSContext *(*func)(JSRuntime *rt))
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

#define OS_FLAG(x) JS_PROP_INT32_DEF(#x, x, JS_PROP_CONFIGURABLE )

static const JSCFunctionListEntry js_os_funcs[] = {
    JS_CFUNC_DEF("open", 2, js_os_open ),
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
    JS_CFUNC_DEF("close", 1, js_os_close ),
    JS_CFUNC_DEF("seek", 3, js_os_seek ),
    JS_CFUNC_MAGIC_DEF("read", 4, js_os_read_write, 0 ),
    JS_CFUNC_MAGIC_DEF("write", 4, js_os_read_write, 1 ),
    JS_CFUNC_DEF("isatty", 1, js_os_isatty ),
    JS_CFUNC_DEF("ttyGetWinSize", 1, js_os_ttyGetWinSize ),
    JS_CFUNC_DEF("ttySetRaw", 1, js_os_ttySetRaw ),
    JS_CFUNC_DEF("remove", 1, js_os_remove ),
    JS_CFUNC_DEF("rename", 2, js_os_rename ),
    JS_CFUNC_MAGIC_DEF("setReadHandler", 2, js_os_setReadHandler, 0 ),
    JS_CFUNC_MAGIC_DEF("setWriteHandler", 2, js_os_setReadHandler, 1 ),
    JS_CFUNC_DEF("signal", 2, js_os_signal ),
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
    JS_CFUNC_DEF("now", 0, js_os_now ),
    JS_CFUNC_MAGIC_DEF("setTimeout", 2, js_os_setTimeout, 0 ),
    JS_CFUNC_MAGIC_DEF("setInterval", 2, js_os_setTimeout, 1 ),
    JS_CFUNC_DEF("clearTimeout", 1, js_os_clearTimeout ),
    JS_CFUNC_DEF("clearInterval", 1, js_os_clearTimeout ),
    JS_CFUNC_DEF("sleepAsync", 1, js_os_sleepAsync ),
    JS_PROP_STRING_DEF("platform", OS_PLATFORM, 0 ),
    JS_CFUNC_DEF("getcwd", 0, js_os_getcwd ),
    JS_CFUNC_DEF("chdir", 0, js_os_chdir ),
    JS_CFUNC_DEF("mkdir", 1, js_os_mkdir ),
    JS_CFUNC_DEF("readdir", 1, js_os_readdir ),
    /* st_mode constants */
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
    JS_CFUNC_MAGIC_DEF("stat", 1, js_os_stat, 0 ),
    JS_CFUNC_DEF("utimes", 3, js_os_utimes ),
    JS_CFUNC_DEF("sleep", 1, js_os_sleep ),
    JS_CFUNC_DEF("realpath", 1, js_os_realpath ),
#if !defined(_WIN32)
    JS_CFUNC_MAGIC_DEF("lstat", 1, js_os_stat, 1 ),
    JS_CFUNC_DEF("symlink", 2, js_os_symlink ),
    JS_CFUNC_DEF("readlink", 1, js_os_readlink ),
    JS_CFUNC_DEF("exec", 1, js_os_exec ),
    JS_CFUNC_DEF("getpid", 0, js_os_getpid ),
    JS_CFUNC_DEF("waitpid", 2, js_os_waitpid ),
    OS_FLAG(WNOHANG),
    JS_CFUNC_DEF("pipe", 0, js_os_pipe ),
    JS_CFUNC_DEF("kill", 2, js_os_kill ),
    JS_CFUNC_DEF("dup", 1, js_os_dup ),
    JS_CFUNC_DEF("dup2", 2, js_os_dup2 ),
#endif
};

static int js_os_init(JSContext *ctx, JSModuleDef *m)
{
    atomic_store_explicit(&os_poll_func, js_os_poll, memory_order_relaxed);

#ifdef USE_WORKER
    {
        JSRuntime *rt = JS_GetRuntime(ctx);
        JSThreadState *ts = JS_GetRuntimeOpaque(rt);
        JSValue proto, obj;
        /* Worker class */
        JS_NewClassID(&js_worker_class_id);
        JS_NewClass(JS_GetRuntime(ctx), js_worker_class_id, &js_worker_class);
        proto = JS_NewObject(ctx);
        JS_SetPropertyFunctionList(ctx, proto, js_worker_proto_funcs, countof(js_worker_proto_funcs));

        obj = JS_NewCFunction2(ctx, js_worker_ctor, "Worker", 1,
                               JS_CFUNC_constructor, 0);
        JS_SetConstructor(ctx, obj, proto);

        JS_SetClassProto(ctx, js_worker_class_id, proto);

        /* set 'Worker.parent' if necessary */
        if (ts->recv_pipe && ts->send_pipe) {
            JS_DefinePropertyValueStr(ctx, obj, "parent",
                                      js_worker_ctor_internal(ctx, JS_UNDEFINED, ts->recv_pipe, ts->send_pipe),
                                      JS_PROP_C_W_E);
        }

        JS_SetModuleExport(ctx, m, "Worker", obj);
    }
#endif /* USE_WORKER */

    return JS_SetModuleExportList(ctx, m, js_os_funcs,
                                  countof(js_os_funcs));
}

JSModuleDef *js_init_module_os(JSContext *ctx, const char *module_name)
{
    JSModuleDef *m;
    m = JS_NewCModule(ctx, module_name, js_os_init);
    if (!m)
        return NULL;
    JS_AddModuleExportList(ctx, m, js_os_funcs, countof(js_os_funcs));
#ifdef USE_WORKER
    JS_AddModuleExport(ctx, m, "Worker");
#endif
    return m;
}

/**********************************************************/

static JSValue js_print_internal(JSContext *ctx, int argc, JSValueConst *argv,
                                 FILE *fp)
{
    int i;
    JSValueConst v;

    for(i = 0; i < argc; i++) {
        if (i != 0)
            fputc(' ', fp);
        v = argv[i];
        if (JS_IsString(v)) {
            const char *str;
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

static JSValue js_print(JSContext *ctx, JSValueConst this_val,
                        int argc, JSValueConst *argv)
{
    return js_print_internal(ctx, argc, argv, stdout);
}

/* magic bit 0: write to stderr instead of stdout */
static JSValue js_console_write(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv, int magic)
{
    FILE *fp = (magic & 1) ? stderr : stdout;
    JSValue ret;
    ret = js_print_internal(ctx, argc, argv, fp);
    fflush(fp);
    return ret;
}

static JSValue js_console_assert(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    if (argc > 0 && JS_ToBool(ctx, argv[0]))
        return JS_UNDEFINED;
    fputs("Assertion failed:", stderr);
    if (argc > 1)
        fputc(' ', stderr);
    return js_console_write(ctx, this_val, argc > 1 ? argc - 1 : 0,
                            argv + 1, 1);
}

/* console methods writing to stdout (magic 0) or stderr (magic 1);
   installed with enumerable properties as the WHATWG console spec */
static const struct {
    const char *name;
    int magic;
} js_console_methods[] = {
    { "log", 0 }, { "info", 0 }, { "debug", 0 },
    { "trace", 1 }, { "warn", 1 }, { "error", 1 },
};

static JSValue js_std_queueMicrotask_job(JSContext *ctx,
                                         int argc, JSValueConst *argv)
{
    return JS_Call(ctx, argv[0], JS_UNDEFINED, 0, NULL);
}

static JSValue js_std_queueMicrotask(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    if (!JS_IsFunction(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "not a function");
    if (JS_EnqueueJob(ctx, js_std_queueMicrotask_job, 1, &argv[0]))
        return JS_EXCEPTION;
    return JS_UNDEFINED;
}

/**********************************************************/
/* WHATWG TextEncoder / TextDecoder (UTF-8 only) */

static JSClassID js_textencoder_class_id;
static JSClassID js_textdecoder_class_id;

/* TextEncoder is stateless; the heap marker only makes the class opaque
   non-NULL so JS_GetOpaque2() can be used as a brand check. */
typedef struct {
    uint8_t brand;
} JSTextEncoder;

typedef struct {
    BOOL fatal;
    BOOL ignore_bom;
} JSTextDecoder;

static const uint8_t utf8_replacement[3] = { 0xEF, 0xBF, 0xBD };

static void js_textencoder_finalizer(JSRuntime *rt, JSValue val)
{
    JSTextEncoder *te = JS_GetOpaque(val, js_textencoder_class_id);
    if (te)
        js_free_rt(rt, te);
}

static void js_textdecoder_finalizer(JSRuntime *rt, JSValue val)
{
    JSTextDecoder *td = JS_GetOpaque(val, js_textdecoder_class_id);
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

/* the only labels this UTF-8 decoder accepts (case-insensitive, trimmed) */
static BOOL js_textdecoder_label_is_utf8(const char *s)
{
    char buf[32];
    size_t start = 0, len, i;

    while (s[start] == 0x09 || s[start] == 0x0A || s[start] == 0x0C ||
           s[start] == 0x0D || s[start] == 0x20)
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
    return !strcmp(buf, "utf-8") || !strcmp(buf, "utf8") ||
           !strcmp(buf, "unicode-1-1-utf-8");
}

/* Copy the bytes of a BufferSource (ArrayBuffer, TypedArray or DataView)
   into a freshly js_malloc'd buffer (caller frees with js_free). The backing
   pointer is fetched last and copied with no intervening JS call, so a getter
   that detaches the buffer during argument resolution cannot cause a
   use-after-free. Returns 0 on success, -1 with a pending exception. */
static int js_textcodec_get_bytes(JSContext *ctx, JSValueConst obj,
                                  uint8_t **pbuf, size_t *plen)
{
    JSValue buffer;
    uint8_t *ptr, *out;
    size_t offset, length, abuf_size;

    *pbuf = NULL;
    *plen = 0;

    if (JS_GetArrayBuffer(ctx, &abuf_size, obj)) {
        buffer = JS_DupValue(ctx, obj);
        offset = 0;
        length = abuf_size;
    } else {
        JS_FreeValue(ctx, JS_GetException(ctx));
        buffer = JS_GetTypedArrayBuffer(ctx, obj, &offset, &length, NULL);
        if (JS_IsException(buffer)) {
            JSValue v;
            int64_t v_off, v_len;
            JS_FreeValue(ctx, JS_GetException(ctx));
            /* DataView (or generic view) via its public accessors */
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

/* WHATWG UTF-8 decoder: appends the decoded text to dbuf as clean UTF-8.
   Invalid sequences become U+FFFD; if fatal, a TypeError is set instead.
   Returns 0 on success, -1 with a pending exception (fatal error or OOM). */
static int js_utf8_decode_whatwg(JSContext *ctx, DynBuf *dbuf,
                                 const uint8_t *buf, size_t len, BOOL fatal)
{
    size_t i = 0;
    uint32_t codepoint = 0;
    int bytes_needed = 0, bytes_seen = 0;
    uint8_t lower = 0x80, upper = 0xBF;
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
            /* invalid continuation: reset and reprocess b as a lead byte */
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
        /* incomplete trailing sequence */
        if (fatal)
            goto fatal_error;
        dbuf_put(dbuf, utf8_replacement, 3);
    }
    if (dbuf->error) {
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    return 0;
 fatal_error:
    JS_ThrowTypeError(ctx, "invalid UTF-8 sequence");
    return -1;
}

static JSValue js_textencoder_ctor(JSContext *ctx, JSValueConst new_target,
                                   int argc, JSValueConst *argv)
{
    JSValue obj = JS_UNDEFINED, proto;
    JSTextEncoder *te;

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

static JSValue js_textencoder_get_encoding(JSContext *ctx, JSValueConst this_val)
{
    if (!JS_GetOpaque2(ctx, this_val, js_textencoder_class_id))
        return JS_EXCEPTION;
    return JS_NewString(ctx, "utf-8");
}

static JSValue js_textencoder_encode(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    const char *str = NULL;
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
    ab = JS_NewArrayBufferCopy(ctx, (const uint8_t *)(str ? str : ""), len);
    if (str)
        JS_FreeCString(ctx, str);
    if (JS_IsException(ab))
        return JS_EXCEPTION;
    /* the typed-array constructor reads argv[1]/argv[2] (offset/length) */
    args[0] = ab;
    args[1] = JS_UNDEFINED;
    args[2] = JS_UNDEFINED;
    u8 = JS_NewTypedArray(ctx, 3, args, JS_TYPED_ARRAY_UINT8);
    JS_FreeValue(ctx, ab);
    return u8;
}

static JSValue js_textencoder_encodeInto(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv)
{
    const char *src;
    size_t src_len, offset, dst_len, abuf_size, bpe, si = 0, di = 0;
    int64_t read = 0, written = 0;
    uint8_t *abuf, *dst;
    JSValue buffer, res;

    if (!JS_GetOpaque2(ctx, this_val, js_textencoder_class_id))
        return JS_EXCEPTION;
    /* coerce the source (runs JS) before resolving the destination */
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
    /* copy whole code points until the next one would overrun the buffer;
       src is well-formed UTF-8 from JS_ToCStringLen, so no JS runs here */
    while (si < src_len) {
        const uint8_t *p = (const uint8_t *)src + si;
        const uint8_t *pnext;
        int max_len = (src_len - si) > UTF8_CHAR_LEN_MAX ?
                      UTF8_CHAR_LEN_MAX : (int)(src_len - si);
        int c = unicode_from_utf8(p, max_len, &pnext);
        int clen;
        if (c < 0) {
            c = 0xFFFD;
            clen = 1;
        } else {
            clen = (int)(pnext - p);
        }
        if (di + (size_t)clen > dst_len)
            break;
        memcpy(dst + di, p, clen);
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
    JS_SetPropertyStr(ctx, res, "read", JS_NewInt64(ctx, read));
    JS_SetPropertyStr(ctx, res, "written", JS_NewInt64(ctx, written));
    return res;
}

static JSValue js_textdecoder_ctor(JSContext *ctx, JSValueConst new_target,
                                   int argc, JSValueConst *argv)
{
    JSValue obj = JS_UNDEFINED, proto;
    JSTextDecoder *td;
    BOOL fatal = FALSE, ignore_bom = FALSE;

    /* coerce all JS args to C locals first */
    if (argc >= 1 && !JS_IsUndefined(argv[0])) {
        const char *label = JS_ToCString(ctx, argv[0]);
        BOOL ok;
        if (!label)
            return JS_EXCEPTION;
        ok = js_textdecoder_label_is_utf8(label);
        JS_FreeCString(ctx, label);
        if (!ok)
            return JS_ThrowRangeError(ctx, "unsupported encoding label");
    }
    if (argc >= 2 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
        JSValue v;
        int b;
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
    JS_SetOpaque(obj, td);
    return obj;
 fail:
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
}

static JSValue js_textdecoder_get_encoding(JSContext *ctx, JSValueConst this_val)
{
    if (!JS_GetOpaque2(ctx, this_val, js_textdecoder_class_id))
        return JS_EXCEPTION;
    return JS_NewString(ctx, "utf-8");
}

static JSValue js_textdecoder_get_fatal(JSContext *ctx, JSValueConst this_val)
{
    JSTextDecoder *td = JS_GetOpaque2(ctx, this_val, js_textdecoder_class_id);
    if (!td)
        return JS_EXCEPTION;
    return JS_NewBool(ctx, td->fatal);
}

static JSValue js_textdecoder_get_ignoreBOM(JSContext *ctx, JSValueConst this_val)
{
    JSTextDecoder *td = JS_GetOpaque2(ctx, this_val, js_textdecoder_class_id);
    if (!td)
        return JS_EXCEPTION;
    return JS_NewBool(ctx, td->ignore_bom);
}

/* Streaming is not stateful here: each decode() is independent and an
   incomplete trailing sequence becomes U+FFFD (or throws when fatal). */
static JSValue js_textdecoder_decode(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    JSTextDecoder *td = JS_GetOpaque2(ctx, this_val, js_textdecoder_class_id);
    BOOL fatal, ignore_bom;
    const uint8_t *p;
    uint8_t *buf;
    size_t len, n;
    DynBuf dbuf;
    JSValue ret;

    if (!td)
        return JS_EXCEPTION;
    fatal = td->fatal;
    ignore_bom = td->ignore_bom;

    if (argc < 1 || JS_IsUndefined(argv[0]))
        return JS_NewStringLen(ctx, "", 0);
    if (js_textcodec_get_bytes(ctx, argv[0], &buf, &len))
        return JS_EXCEPTION;

    p = buf;
    n = len;
    if (!ignore_bom && n >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) {
        p += 3;
        n -= 3;
    }
    js_std_dbuf_init(ctx, &dbuf);
    if (js_utf8_decode_whatwg(ctx, &dbuf, p, n, fatal)) {
        dbuf_free(&dbuf);
        js_free(ctx, buf);
        return JS_EXCEPTION;
    }
    ret = JS_NewStringLen(ctx, (const char *)dbuf.buf, dbuf.size);
    dbuf_free(&dbuf);
    js_free(ctx, buf);
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

static void js_textcodec_install(JSContext *ctx, JSValueConst global_obj)
{
    JSRuntime *rt = JS_GetRuntime(ctx);
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
    JS_SetPropertyStr(ctx, global_obj, "TextEncoder", ctor);

    JS_NewClassID(&js_textdecoder_class_id);
    JS_NewClass(rt, js_textdecoder_class_id, &js_textdecoder_class);
    proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, proto, js_textdecoder_proto_funcs,
                               countof(js_textdecoder_proto_funcs));
    ctor = JS_NewCFunction2(ctx, js_textdecoder_ctor, "TextDecoder", 0,
                            JS_CFUNC_constructor, 0);
    JS_SetConstructor(ctx, ctor, proto);
    JS_SetClassProto(ctx, js_textdecoder_class_id, proto);
    JS_SetPropertyStr(ctx, global_obj, "TextDecoder", ctor);
}

#include "dyna-fetch.inc.c"

void js_std_add_helpers(JSContext *ctx, int argc, char **argv)
{
    JSValue global_obj, console, args, performance;
    int i;

    /* XXX: should these global definitions be enumerable? */
    global_obj = JS_GetGlobalObject(ctx);

    console = JS_NewObject(ctx);
    for(i = 0; i < (int)countof(js_console_methods); i++) {
        JS_SetPropertyStr(ctx, console, js_console_methods[i].name,
                          JS_NewCFunctionMagic(ctx, js_console_write,
                                               js_console_methods[i].name, 1,
                                               JS_CFUNC_generic_magic,
                                               js_console_methods[i].magic));
    }
    JS_SetPropertyStr(ctx, console, "assert",
                      JS_NewCFunction(ctx, js_console_assert, "assert", 2));
    JS_SetPropertyStr(ctx, global_obj, "console", console);

    /* standard global timer and microtask functions; the timers need
       js_std_init_handlers() to have been called on the runtime. The
       event loop poll hook is normally armed by the 'os' module init;
       arm it here too so global timers fire without importing 'os'. */
    atomic_store_explicit(&os_poll_func, js_os_poll, memory_order_relaxed);
    JS_SetPropertyStr(ctx, global_obj, "setTimeout",
                      JS_NewCFunctionMagic(ctx, js_os_setTimeout, "setTimeout",
                                           2, JS_CFUNC_generic_magic, 0));
    JS_SetPropertyStr(ctx, global_obj, "setInterval",
                      JS_NewCFunctionMagic(ctx, js_os_setTimeout, "setInterval",
                                           2, JS_CFUNC_generic_magic, 1));
    JS_SetPropertyStr(ctx, global_obj, "clearTimeout",
                      JS_NewCFunction(ctx, js_os_clearTimeout, "clearTimeout", 1));
    JS_SetPropertyStr(ctx, global_obj, "clearInterval",
                      JS_NewCFunction(ctx, js_os_clearTimeout, "clearInterval", 1));
    JS_SetPropertyStr(ctx, global_obj, "queueMicrotask",
                      JS_NewCFunction(ctx, js_std_queueMicrotask, "queueMicrotask", 1));

    performance = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, performance, "now",
                      JS_NewCFunction(ctx, js_os_now, "now", 0));
    JS_SetPropertyStr(ctx, global_obj, "performance", performance);

    /* same methods as the mozilla JS shell */
    if (argc >= 0) {
        args = JS_NewArray(ctx);
        for(i = 0; i < argc; i++) {
            JS_SetPropertyUint32(ctx, args, i, JS_NewString(ctx, argv[i]));
        }
        JS_SetPropertyStr(ctx, global_obj, "scriptArgs", args);
    }

    JS_SetPropertyStr(ctx, global_obj, "print",
                      JS_NewCFunction(ctx, js_print, "print", 1));
    JS_SetPropertyStr(ctx, global_obj, "__loadScript",
                      JS_NewCFunction(ctx, js_loadScript, "__loadScript", 1));

    js_textcodec_install(ctx, global_obj);
    js_fetch_install(ctx, global_obj);

    JS_FreeValue(ctx, global_obj);
}

void js_std_init_handlers(JSRuntime *rt)
{
    JSThreadState *ts;

    /* A write to an RST'd peer must be an I/O error, never a process
       killer: SO_NOSIGPIPE guards the aio sockets, but a peer's RST can
       also land against an in-flight write on any other socket path. */
    signal(SIGPIPE, SIG_IGN);

    ts = malloc(sizeof(*ts));
    if (!ts) {
        fprintf(stderr, "Could not allocate memory for the worker");
        exit(1);
    }
    memset(ts, 0, sizeof(*ts));
    init_list_head(&ts->os_rw_handlers);
    init_list_head(&ts->os_signal_handlers);
    init_list_head(&ts->os_timers);
    init_list_head(&ts->port_list);
    init_list_head(&ts->rejected_promise_list);
    ts->next_timer_id = 1;
    ts->io_reactor_fd = -1; /* no IO reactor folded in yet */

    JS_SetRuntimeOpaque(rt, ts);

#ifdef USE_WORKER
    /* set the SharedArrayBuffer memory handlers */
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

void js_std_free_handlers(JSRuntime *rt)
{
    JSThreadState *ts = JS_GetRuntimeOpaque(rt);
    struct list_head *el, *el1;

    list_for_each_safe(el, el1, &ts->os_rw_handlers) {
        JSOSRWHandler *rh = list_entry(el, JSOSRWHandler, link);
        free_rw_handler(rt, rh);
    }

    list_for_each_safe(el, el1, &ts->os_signal_handlers) {
        JSOSSignalHandler *sh = list_entry(el, JSOSSignalHandler, link);
        free_sh(rt, sh);
    }

    list_for_each_safe(el, el1, &ts->os_timers) {
        JSOSTimer *th = list_entry(el, JSOSTimer, link);
        free_timer(rt, th);
    }

    list_for_each_safe(el, el1, &ts->rejected_promise_list) {
        JSRejectedPromiseEntry *rp = list_entry(el, JSRejectedPromiseEntry, link);
        JS_FreeValueRT(rt, rp->promise);
        JS_FreeValueRT(rt, rp->reason);
        free(rp);
    }

#ifdef USE_WORKER
    js_free_message_pipe(ts->recv_pipe);
    js_free_message_pipe(ts->send_pipe);

    list_for_each_safe(el, el1, &ts->port_list) {
        JSWorkerMessageHandler *port = list_entry(el, JSWorkerMessageHandler, link);
        /* unlink the message ports. They are freed by the Worker object */
        port->link.prev = NULL;
        port->link.next = NULL;
    }
#endif

#if !defined(_WIN32)
    free(ts->poll_fds);
#endif

    free(ts);
    JS_SetRuntimeOpaque(rt, NULL); /* fail safe */
}

static void js_std_dump_error1(JSContext *ctx, JSValueConst exception_val)
{
    JS_PrintValue(ctx, js_print_value_write, stderr, exception_val, NULL);
    fputc('\n', stderr);
}

void js_std_dump_error(JSContext *ctx)
{
    JSValue exception_val;

    exception_val = JS_GetException(ctx);
    js_std_dump_error1(ctx, exception_val);
    JS_FreeValue(ctx, exception_val);
}

static JSRejectedPromiseEntry *find_rejected_promise(JSContext *ctx, JSThreadState *ts,
                                                     JSValueConst promise)
{
    struct list_head *el;

    list_for_each(el, &ts->rejected_promise_list) {
        JSRejectedPromiseEntry *rp = list_entry(el, JSRejectedPromiseEntry, link);
        if (JS_SameValue(ctx, rp->promise, promise))
            return rp;
    }
    return NULL;
}

void js_std_promise_rejection_tracker(JSContext *ctx, JSValueConst promise,
                                      JSValueConst reason,
                                      BOOL is_handled, void *opaque)
{
    JSRuntime *rt = JS_GetRuntime(ctx);
    JSThreadState *ts = JS_GetRuntimeOpaque(rt);
    JSRejectedPromiseEntry *rp;

    if (!is_handled) {
        /* add a new entry if needed */
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
        /* the rejection is handled, so the entry can be removed if present */
        rp = find_rejected_promise(ctx, ts, promise);
        if (rp) {
            JS_FreeValue(ctx, rp->promise);
            JS_FreeValue(ctx, rp->reason);
            list_del(&rp->link);
            free(rp);
        }
    }
}

/* check if there are pending promise rejections. It must be done
   asynchrously in case a rejected promise is handled later. Currently
   we do it once the application is about to sleep. It could be done
   more often if needed. */
static void js_std_promise_rejection_check(JSContext *ctx)
{
    JSRuntime *rt = JS_GetRuntime(ctx);
    JSThreadState *ts = JS_GetRuntimeOpaque(rt);
    struct list_head *el;

    if (unlikely(!list_empty(&ts->rejected_promise_list))) {
        /* Detach the whole list BEFORE dumping: js_std_dump_error1() prints
           via JS_PrintValue(), which runs arbitrary user code (toString on
           the reason). That code can attach a handler to one of these
           promises, which makes js_std_promise_rejection_tracker() list_del()
           and free() the very entry this loop is holding -- walking the live
           list is a use-after-free (CVE-2025-62491). */
        struct list_head tmp = LIST_HEAD_INIT(tmp);
        tmp.next = ts->rejected_promise_list.next;
        tmp.prev = ts->rejected_promise_list.prev;
        ts->rejected_promise_list.next->prev = &tmp;
        ts->rejected_promise_list.prev->next = &tmp;
        ts->rejected_promise_list.next = &ts->rejected_promise_list;
        ts->rejected_promise_list.prev = &ts->rejected_promise_list;

        list_for_each(el, &tmp) {
            JSRejectedPromiseEntry *rp = list_entry(el, JSRejectedPromiseEntry, link);
            fprintf(stderr, "Possibly unhandled promise rejection: ");
            js_std_dump_error1(ctx, rp->reason);
        }
        exit(1);
    }
}

/* main loop which calls the user JS callbacks */
void js_std_loop(JSContext *ctx)
{
    int err;

    for(;;) {
        /* execute the pending jobs */
        for(;;) {
            err = JS_ExecutePendingJob(JS_GetRuntime(ctx), NULL);
            if (err <= 0) {
                if (err < 0)
                    js_std_dump_error(ctx);
                break;
            }
        }

        js_std_promise_rejection_check(ctx);

        JSOSPollFunc poll = atomic_load_explicit(&os_poll_func,
                                                 memory_order_relaxed);
        if (!poll || poll(ctx))
            break;
    }
}

/* Wait for a promise and execute pending jobs while waiting for
   it. Return the promise result or JS_EXCEPTION in case of promise
   rejection. */
JSValue js_std_await(JSContext *ctx, JSValue obj)
{
    JSValue ret;
    int state;

    for(;;) {
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
                js_std_dump_error(ctx);
            }
            if (err == 0) {
                JSOSPollFunc poll;
                js_std_promise_rejection_check(ctx);

                poll = atomic_load_explicit(&os_poll_func,
                                            memory_order_relaxed);
                if (poll)
                    poll(ctx);
            }
        } else {
            /* not a promise */
            ret = obj;
            break;
        }
    }
    return ret;
}

void js_std_eval_binary(JSContext *ctx, const uint8_t *buf, size_t buf_len,
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

void js_std_eval_binary_json_module(JSContext *ctx,
                                    const uint8_t *buf, size_t buf_len,
                                    const char *module_name)
{
    JSValue obj;
    JSModuleDef *m;
    
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

