#include "libc-internal.h"

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
    if (JS_IsException(str))
        return -1;
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
        if (res < 0) {
            dyn_iobuf_free(&src);
            return NULL;
        }
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
    dyn_stdio_sync();
    if (dyn_stdio_report_unreported() && status == 0)
        status = 1;
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
void setenv(const char* name, const char* value, int overwrite)
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

JSOSRWHandler* find_rh(JSThreadState* ts, int fd)
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

JSOSSignalHandler* find_sh(JSThreadState* ts, int sig_num)
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

JSOSTimer* find_timer_by_id(JSThreadState* ts, int timer_id)
{
    JSOSTimer* th;
    if (timer_id <= 0 || !ts->timer_ids_cap)
        return NULL;
    th = ts->timer_ids[(uint32_t)timer_id & (ts->timer_ids_cap - 1)];
    while (th && th->timer_id != timer_id)
        th = th->id_next;
    return th;
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

JSRejectedPromiseEntry* find_rejected_promise(JSContext* ctx, JSThreadState* ts,
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

