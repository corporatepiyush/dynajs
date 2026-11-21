#include "log-internal.h"
#include "core/dyn-stdio.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_LOG)

#define DYN_FAST_KEYS 32

pthread_mutex_t dyn_log_mu = PTHREAD_MUTEX_INITIALIZER;

const char* const DYN_LV_NAME[] = {
    "trace", "debug", "info", "warn", "error", "fatal", "silent"
};

JSClassID dyn_logger_class_id;

static JSClassDef dyn_logger_class = {
    "Logger",
    .finalizer = dyn_logger_finalizer,
    .gc_mark = dyn_logger_mark,
};

const char* dyn_iso_sec(dyn_logger_t* L, int64_t ms, int* millis_out)
{
    int64_t sec = ms / 1000;
    if (sec != L->cached_sec) {
        struct tm tmv;
        time_t t = (time_t)sec;
        if (!gmtime_r(&t, &tmv))
            memset(&tmv, 0, sizeof tmv);
        snprintf(L->cached_iso, sizeof L->cached_iso,
            "%04d-%02d-%02dT%02d:%02d:%02d",
            tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
            tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
        L->cached_sec = sec;
    }
    *millis_out = (int)(ms % 1000);
    return L->cached_iso;
}

int dyn_key_reserved(dyn_logger_t* L, const char* k, size_t n,
    int reserve_err)
{
    unsigned char c0 = (unsigned char)k[0];
    if (n == 0)
        return 0;
    if (c0 < 'e' || c0 > 't')
        return 0;
    switch (c0) {
    case 't':
        return L->ts_mode != DYN_TS_NONE && n == 4 && memcmp(k, "time", 4) == 0;
    case 'l':
        return n == 5 && memcmp(k, "level", 5) == 0;
    case 'm':
        return n == 3 && memcmp(k, "msg", 3) == 0;
    case 'n':
        return L->name && n == 4 && memcmp(k, "name", 4) == 0;
    case 'p':
        return L->pid_on && n == 3 && memcmp(k, "pid", 3) == 0;
    case 'h':
        return L->host_on && n == 8 && memcmp(k, "hostname", 8) == 0;
    case 'e':
        return reserve_err && n == 3 && memcmp(k, "err", 3) == 0;
    default:
        return 0;
    }
}

int dyn_props_get(JSContext* ctx, JSValueConst v, dyn_props_t* p)
{
    int rc = JS_GetOwnFastProps(ctx, v, p->stackbuf, DYN_FAST_KEYS, &p->len);
    if (rc == 0) {
        p->tab = p->stackbuf;
        p->heap = 0;
        return 0;
    }
    if (rc == -2) {
        p->tab = (JSPropertyEnum*)malloc(p->len * sizeof *p->tab);
        if (!p->tab) {
            JS_ThrowOutOfMemory(ctx);
            return -1;
        }
        if (JS_GetOwnFastProps(ctx, v, p->tab, p->len, &p->len) == 0) {
            p->heap = 1;
            return 0;
        }
        free(p->tab);
    }
    p->tab = NULL;
    p->heap = 2;
    if (JS_GetOwnPropertyNames(ctx, &p->tab, &p->len, v,
            JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY)
        < 0)
        return -1;
    return 0;
}

int dyn_write_all(int fd, const char* p, size_t n)
{
    size_t off = 0;
    if (fd == 1 || fd == 2)
        dyn_stdio_sync();
    while (off < n) {
        ssize_t wr = write(fd, p + off, n - off);
        if (wr < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        off += (size_t)wr;
    }
    return 0;
}

static const JSCFunctionListEntry dyn_logger_proto[] = {
    JS_CFUNC_MAGIC_DEF("trace", 1, dyn_log_emit, DYN_LV_TRACE),
    JS_CFUNC_MAGIC_DEF("debug", 1, dyn_log_emit, DYN_LV_DEBUG),
    JS_CFUNC_MAGIC_DEF("info", 1, dyn_log_emit, DYN_LV_INFO),
    JS_CFUNC_MAGIC_DEF("warn", 1, dyn_log_emit, DYN_LV_WARN),
    JS_CFUNC_MAGIC_DEF("error", 1, dyn_log_emit, DYN_LV_ERROR),
    JS_CFUNC_MAGIC_DEF("fatal", 1, dyn_log_emit, DYN_LV_FATAL),
    JS_CFUNC_DEF("child", 2, dyn_logger_child),
    JS_CFUNC_DEF("enabled", 1, dyn_logger_enabled),
    JS_CFUNC_DEF("flush", 0, dyn_logger_flush),
    JS_CGETSET_DEF("level", dyn_logger_get_level, dyn_logger_set_level),
};

static const JSCFunctionListEntry dyn_log_funcs[] = {
    JS_CFUNC_DEF("Debug", 1, dyn_debug),
};

static int dyn_log_init_module(JSContext* ctx, JSModuleDef* m)
{
    if (dyn_register_plain_class(ctx, m, &dyn_logger_class_id,
            &dyn_logger_class, dyn_logger_proto,
            countof(dyn_logger_proto), dyn_logger_ctor,
            "Logger")
        < 0)
        return -1;
    return JS_SetModuleExportList(ctx, m, dyn_log_funcs, countof(dyn_log_funcs));
}

int js_nat_init_log(JSContext* ctx)
{
    JSModuleDef* m = JS_NewCModule(ctx, "dyna:log", dyn_log_init_module);
    if (!m)
        return -1;
    JS_AddModuleExport(ctx, m, "Logger");
    return JS_AddModuleExportList(ctx, m, dyn_log_funcs, countof(dyn_log_funcs));
}

#endif
