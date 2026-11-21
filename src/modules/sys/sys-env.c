#include "sys-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_SYS)

JSValue dyn_sys_env(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    char** envp = dyn_environ;
    JSValue obj;
    uint32_t idx;
    (void)this_val;
    (void)argc;
    (void)argv;

    obj = JS_NewObject(ctx);
    if (JS_IsException(obj))
        return JS_EXCEPTION;
    for (idx = 0; envp[idx] != NULL; idx++) {
        const char* entry = envp[idx];
        const char* eq = strchr(entry, '=');
        JSAtom atom;
        if (!eq)
            continue;
        atom = JS_NewAtomLen(ctx, entry, (size_t)(eq - entry));
        if (atom == JS_ATOM_NULL) {
            JS_FreeValue(ctx, obj);
            return JS_EXCEPTION;
        }
        JS_DefinePropertyValue(ctx, obj, atom,
            dyn_utf8_lossy_string(ctx, (const uint8_t*)(eq + 1),
                strlen(eq + 1)),
            JS_PROP_C_W_E);
        JS_FreeAtom(ctx, atom);
    }
    return obj;
}

JSValue dyn_sys_get_env(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    const char *name, *val;
    JSValue out;
    (void)this_val;
    (void)argc;

    name = dyn_sys_cstr(ctx, argv[0], "getEnv: name");
    if (!name)
        return JS_EXCEPTION;
    val = getenv(name);
    out = val ? dyn_utf8_lossy_string(ctx, (const uint8_t*)val, strlen(val))
              : JS_UNDEFINED;
    JS_FreeCString(ctx, name);
    return out;
}

JSValue dyn_sys_set_env(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    const char *name, *val;
    size_t name_len, val_len;
    int r;
    (void)this_val;
    (void)argc;

    name = JS_ToCStringLen(ctx, &name_len, argv[0]);
    if (!name)
        return JS_EXCEPTION;
    val = JS_ToCStringLen(ctx, &val_len, argv[1]);
    if (!val) {
        JS_FreeCString(ctx, name);
        return JS_EXCEPTION;
    }
    if (strlen(name) != name_len || strlen(val) != val_len || strchr(name, '=') != NULL || name_len == 0) {
        JSValue ex = JS_ThrowTypeError(ctx,
            "setEnv: name must be non-empty and free of '=' and NUL, "
            "and the value free of NUL");
        JS_FreeCString(ctx, name);
        JS_FreeCString(ctx, val);
        return ex;
    }
    r = setenv(name, val, 1);
    if (r != 0) {
        int e = errno;
        JSValue ex = dyn_sys_throw(ctx, e, "setEnv", name);
        JS_FreeCString(ctx, name);
        JS_FreeCString(ctx, val);
        return ex;
    }
    JS_FreeCString(ctx, name);
    JS_FreeCString(ctx, val);
    return JS_UNDEFINED;
}

int spawn_env_at(JSContext* ctx, JSValueConst obj, JSAtom atom,
    char** slot)
{
    JSValue val = JS_GetProperty(ctx, obj, atom);
    const char *nm, *vs;
    size_t nm_len, sz;

    nm = JS_AtomToCStringLen(ctx, &nm_len, atom);
    if (!nm) {
        JS_FreeValue(ctx, val);
        return -1;
    }
    if (strlen(nm) != nm_len) {
        JS_ThrowTypeError(ctx, "Spawn: env name contains a NUL byte");
        JS_FreeCString(ctx, nm);
        JS_FreeValue(ctx, val);
        return -1;
    }
    vs = JS_IsException(val) ? NULL : dyn_sys_cstr(ctx, val, "Spawn: env value");
    JS_FreeValue(ctx, val);
    if (!vs) {
        JS_FreeCString(ctx, nm);
        return -1;
    }
    sz = nm_len + strlen(vs) + 2;
    *slot = (char*)malloc(sz);
    if (*slot)
        snprintf(*slot, sz, "%s=%s", nm, vs);
    JS_FreeCString(ctx, nm);
    JS_FreeCString(ctx, vs);
    if (!*slot) {
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    return 0;
}

#endif
