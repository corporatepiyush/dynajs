#include "json-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_JSON)

static JSValue dyn_jp_pointer_get(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    const char* p;
    size_t n, tlen;
    JSValue out;
    dyn_jp_walk_t w;
    int code;

    if (argc < 2 || !JS_IsString(argv[1])) {
        JS_ThrowTypeError(ctx,
            "Pointer.get(doc, pointer): pointer must be a string");
        return JS_EXCEPTION;
    }
    p = JS_ToCStringLen(ctx, &n, argv[1]);
    if (!p)
        return JS_EXCEPTION;
    if (n > DYN_JP_MAX_PTR) {
        JS_FreeCString(ctx, p);
        JS_ThrowRangeError(ctx, "Pointer.get: pointer longer than %d bytes",
            DYN_JP_MAX_PTR);
        return JS_EXCEPTION;
    }
    code = dyn_jp_walk(ctx, argv[0], p, n, 1, NULL, &tlen, &out, &w);
    if (code) {
        JSValue e = dyn_jp_throw(ctx, "Pointer.get", code, &w, p, n);
        JS_FreeCString(ctx, p);
        return e;
    }
    JS_FreeCString(ctx, p);
    return out;
}

static JSValue dyn_jp_pointer_has(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    const char* p;
    size_t n, tlen;
    JSValue out;
    dyn_jp_walk_t w;
    int code;

    if (argc < 2 || !JS_IsString(argv[1])) {
        JS_ThrowTypeError(ctx,
            "Pointer.has(doc, pointer): pointer must be a string");
        return JS_EXCEPTION;
    }
    p = JS_ToCStringLen(ctx, &n, argv[1]);
    if (!p)
        return JS_EXCEPTION;
    if (n > DYN_JP_MAX_PTR) {
        JS_FreeCString(ctx, p);
        JS_ThrowRangeError(ctx, "Pointer.has: pointer longer than %d bytes",
            DYN_JP_MAX_PTR);
        return JS_EXCEPTION;
    }
    code = dyn_jp_walk(ctx, argv[0], p, n, 1, NULL, &tlen, &out, &w);

    if (code == JP_MISSING || code == JP_SCALAR || code == JP_RANGE || code == JP_APPEND || code == JP_TOOBIG) {
        JS_FreeCString(ctx, p);
        return JS_FALSE;
    }
    if (code) {
        JSValue e = dyn_jp_throw(ctx, "Pointer.has", code, &w, p, n);
        JS_FreeCString(ctx, p);
        return e;
    }
    JS_FreeValue(ctx, out);
    JS_FreeCString(ctx, p);
    return JS_TRUE;
}

static JSValue dyn_jp_pointer_set(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    const char* p;
    size_t n;
    JSValue d, vadd;

    if (argc < 3 || !JS_IsString(argv[1])) {
        JS_ThrowTypeError(ctx,
            "Pointer.set(doc, pointer, value): pointer must be a string");
        return JS_EXCEPTION;
    }
    p = JS_ToCStringLen(ctx, &n, argv[1]);
    if (!p)
        return JS_EXCEPTION;
    if (n > DYN_JP_MAX_PTR) {
        JS_FreeCString(ctx, p);
        JS_ThrowRangeError(ctx, "Pointer.set: pointer longer than %d bytes",
            DYN_JP_MAX_PTR);
        return JS_EXCEPTION;
    }
    d = JS_DupValue(ctx, argv[0]);
    vadd = dyn_jp_clone(ctx, argv[2], 0);
    if (JS_IsException(vadd)) {
        JS_FreeCString(ctx, p);
        JS_FreeValue(ctx, d);
        return JS_EXCEPTION;
    }
    if (dyn_jp_add_at(ctx, &d, "Pointer.set", p, n, vadd) < 0) {
        JS_FreeCString(ctx, p);
        JS_FreeValue(ctx, d);
        return JS_EXCEPTION;
    }
    JS_FreeCString(ctx, p);
    return d;
}

static JSValue dyn_jp_pointer_remove(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    const char* p;
    size_t n;
    JSValue d;

    if (argc < 2 || !JS_IsString(argv[1])) {
        JS_ThrowTypeError(ctx,
            "Pointer.remove(doc, pointer): pointer must be a string");
        return JS_EXCEPTION;
    }
    p = JS_ToCStringLen(ctx, &n, argv[1]);
    if (!p)
        return JS_EXCEPTION;
    if (n > DYN_JP_MAX_PTR) {
        JS_FreeCString(ctx, p);
        JS_ThrowRangeError(ctx, "Pointer.remove: pointer longer than %d bytes",
            DYN_JP_MAX_PTR);
        return JS_EXCEPTION;
    }
    d = JS_DupValue(ctx, argv[0]);
    if (dyn_jp_remove_at(ctx, &d, "Pointer.remove", p, n) < 0) {
        JS_FreeCString(ctx, p);
        JS_FreeValue(ctx, d);
        return JS_EXCEPTION;
    }
    JS_FreeCString(ctx, p);
    return d;
}

static JSValue dyn_jp_pointer_escape(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    const char* s;
    size_t n, i, o = 0;
    char* buf;
    JSValue r;

    if (argc < 1 || !JS_IsString(argv[0])) {
        JS_ThrowTypeError(ctx,
            "Pointer.escape(token): token must be a string");
        return JS_EXCEPTION;
    }
    s = JS_ToCStringLen(ctx, &n, argv[0]);
    if (!s)
        return JS_EXCEPTION;
    buf = js_malloc(ctx, n * 2 + 1);
    if (!buf) {
        JS_FreeCString(ctx, s);
        return JS_EXCEPTION;
    }
    for (i = 0; i < n; i++) {
        char c = s[i];
        if (c == '~') {
            buf[o++] = '~';
            buf[o++] = '0';
        } else if (c == '/') {
            buf[o++] = '~';
            buf[o++] = '1';
        } else {
            buf[o++] = c;
        }
    }
    r = JS_NewStringLen(ctx, buf, o);
    js_free(ctx, buf);
    JS_FreeCString(ctx, s);
    return r;
}

static JSValue dyn_jp_pointer_unescape(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    const char* s;
    size_t n, i, o = 0;
    char* buf;
    JSValue r;

    if (argc < 1 || !JS_IsString(argv[0])) {
        JS_ThrowTypeError(ctx,
            "Pointer.unescape(token): token must be a string");
        return JS_EXCEPTION;
    }
    s = JS_ToCStringLen(ctx, &n, argv[0]);
    if (!s)
        return JS_EXCEPTION;
    buf = js_malloc(ctx, n + 1);
    if (!buf) {
        JS_FreeCString(ctx, s);
        return JS_EXCEPTION;
    }
    for (i = 0; i < n; i++) {
        if (s[i] == '~') {
            if (i + 1 >= n || (s[i + 1] != '0' && s[i + 1] != '1')) {
                js_free(ctx, buf);
                JS_FreeCString(ctx, s);
                JS_ThrowTypeError(ctx,
                    "Pointer.unescape: invalid '~' escape at offset %d",
                    (int)i);
                return JS_EXCEPTION;
            }
            buf[o++] = s[i + 1] == '0' ? '~' : '/';
            i++;
        } else {
            buf[o++] = s[i];
        }
    }
    r = JS_NewStringLen(ctx, buf, o);
    js_free(ctx, buf);
    JS_FreeCString(ctx, s);
    return r;
}

const JSCFunctionListEntry dyn_jp_pointer_funcs[] = {
    JS_CFUNC_MAGIC_DEF("get", 2, dyn_jp_pointer_get, 0),
    JS_CFUNC_MAGIC_DEF("has", 2, dyn_jp_pointer_has, 0),
    JS_CFUNC_MAGIC_DEF("set", 3, dyn_jp_pointer_set, 0),
    JS_CFUNC_MAGIC_DEF("remove", 2, dyn_jp_pointer_remove, 0),
    JS_CFUNC_MAGIC_DEF("escape", 1, dyn_jp_pointer_escape, 0),
    JS_CFUNC_MAGIC_DEF("unescape", 1, dyn_jp_pointer_unescape, 0),
};

#endif
