#include "http-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_HTTP)
static size_t dyn_rpc_err_compose_n(char* buf, size_t cap, int code,
    const char* msg, size_t msg_len,
    const char* id_json)
{
    const char* idsafe = id_json && *id_json ? id_json : "null";
    size_t idlen = strlen(idsafe);
    size_t tail_room, pos = 0;
    int n;

    if (idlen > 100) {
        idsafe = "null";
        idlen = 4;
    }
    tail_room = idlen + 16;
    if (tail_room >= cap)
        return 0;
    n = snprintf(buf, cap,
        "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":%d,\"message\":\"",
        code);
    if (n < 0 || (size_t)n >= cap - tail_room)
        return 0;
    pos = (size_t)n;
    dyn_json_escape_n_into(buf, cap - tail_room, &pos, msg ? msg : "", msg_len);
    n = snprintf(buf + pos, cap - pos, "\"},\"id\":%s}", idsafe);
    return n < 0 ? 0 : pos + (size_t)n;
}

size_t dyn_rpc_err_compose(char* buf, size_t cap, int code,
    const char* msg, const char* id_json)
{
    return dyn_rpc_err_compose_n(buf, cap, code, msg,
        msg ? strlen(msg) : 0, id_json);
}

void dyn_app_rpc_error(dyn_app_conn_t* c, int http_status, int code,
    const char* msg, const char* id_json)
{
    char buf[256];
    size_t n = dyn_rpc_err_compose(buf, sizeof(buf), code, msg, id_json);
    if (n > 0)
        dyn_app_send_json(c, http_status, buf, n);
}

void dyn_app_rpc_error_n(dyn_app_conn_t* c, int http_status, int code,
    const char* msg, size_t msg_len,
    const char* id_json)
{
    char buf[256];
    size_t n = dyn_rpc_err_compose_n(buf, sizeof(buf), code, msg, msg_len,
        id_json);
    if (n > 0)
        dyn_app_send_json(c, http_status, buf, n);
}

JSValue dyn_app_rpc_settle(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic,
    JSValue* data)
{
    dyn_app_pend_t* pd;
    dyn_app_conn_t* c;
    JSValueConst v = argc > 0 ? argv[0] : JS_UNDEFINED;
    (void)this_val;
    pd = dyn_app_pend_claim(data[0]);
    if (!pd)
        return JS_UNDEFINED;
    c = pd->conn;
    if (!c->closed) {
        int saved_g = c->accept_gzip, saved_i = c->identity_refused;
        c->accept_gzip = pd->accept_gzip;
        c->identity_refused = pd->identity_refused;
        if (magic)
            dyn_app_send_rpc_throw(c, v, pd->id);
        else
            dyn_app_send_rpc_result(c, v, pd->id);
        c->accept_gzip = saved_g;
        c->identity_refused = saved_i;
    }
    dyn_app_pend_release(pd, 0);
    return JS_UNDEFINED;
}

int dyn_app_rpc_build_one(dyn_app_conn_t* c, JSValueConst methods,
    JSValueConst elem, dyn_iobuf_t* out, int comma)
{
    JSContext* ctx = c->app->ctx;
    JSValue method_v = JS_GetPropertyStr(ctx, elem, "method");
    JSValue id = JS_GetPropertyStr(ctx, elem, "id");
    JSValue params = JS_GetPropertyStr(ctx, elem, "params");
    int has_id = !JS_IsUndefined(id);
    const char* method_s = JS_IsString(method_v) ? JS_ToCString(ctx, method_v) : NULL;
    const char* id_s;
    int wrote = 0;
    dyn_iobuf_t tmp;
    dyn_iobuf_init(&tmp);
    {
        JSValue idj = JS_JSONStringify(ctx, id, JS_UNDEFINED, JS_UNDEFINED);
        id_s = (JS_IsException(idj) || JS_IsUndefined(idj))
            ? NULL
            : JS_ToCString(ctx, idj);
        JS_FreeValue(ctx, idj);
    }

    if (!method_s) {
        if (has_id) {
            dyn_app_build_error(-32600, "Invalid Request", id_s, &tmp);
            wrote = 1;
        }
    } else {
        JSValue fn = dyn_app_own_get(ctx, methods, method_s);
        if (!JS_IsFunction(ctx, fn)) {
            if (has_id) {
                dyn_app_build_error(-32601, "Method not found", id_s, &tmp);
                wrote = 1;
            }
        } else {
            JSValue res = JS_UNDEFINED;
            int arg_err = 0;
            if (JS_IsUndefined(params)) {
                res = JS_Call(ctx, fn, JS_UNDEFINED, 0, NULL);
            } else if (JS_IsArray(ctx, params)) {
                JSValue lenv = JS_GetPropertyStr(ctx, params, "length");
                uint32_t len = 0, i;
                JSValueConst* argv = NULL;
                int badlen = 0;
                if (JS_ToUint32(ctx, &len, lenv)) {
                    dyn_app_rpc_error(c, 400, -32600, "Invalid Request", "null");
                    badlen = 1;
                }
                if (badlen) {
                    arg_err = 1;
                    len = 0;
                }
                JS_FreeValue(ctx, lenv);
                if (!badlen) {
                    if (len > DYN_APP_MAX_PARAMS) {
                        if (has_id) {
                            dyn_app_build_error(-32602, "Invalid params", id_s, &tmp);
                            wrote = 1;
                        }
                        arg_err = 1;
                    } else if (len > 0) {
                        argv = (JSValueConst*)malloc(len * sizeof(*argv));
                        if (!argv) {
                            if (has_id) {
                                dyn_app_build_error(-32603, "Internal error", id_s, &tmp);
                                wrote = 1;
                            }
                            arg_err = 1;
                        } else {
                            for (i = 0; i < len; i++)
                                argv[i] = JS_GetPropertyUint32(ctx, params, i);
                            res = JS_Call(ctx, fn, JS_UNDEFINED, len, argv);
                            for (i = 0; i < len; i++)
                                JS_FreeValue(ctx, argv[i]);
                            free(argv);
                        }
                    } else {
                        res = JS_Call(ctx, fn, JS_UNDEFINED, 0, NULL);
                    }
                }
            } else {
                JSValueConst a[1] = { params };
                res = JS_Call(ctx, fn, JS_UNDEFINED, 1, a);
            }
            if (!arg_err && JS_IsException(res)) {
                JSValue exc = JS_GetException(ctx);
                if (has_id) {
                    const char* em = JS_ToCString(ctx, exc);
                    if (!em)
                        JS_FreeValue(ctx, JS_GetException(ctx));
                    dyn_app_build_error(-32000, em ? em : "Server error", id_s, &tmp);
                    if (em)
                        JS_FreeCString(ctx, em);
                    wrote = 1;
                }
                JS_FreeValue(ctx, exc);
            } else if (!arg_err) {
                int thenable = 0;
                if (JS_IsObject(res)) {
                    JSValue th = JS_GetPropertyStr(ctx, res, "then");
                    thenable = JS_IsFunction(ctx, th);
                    JS_FreeValue(ctx, th);
                }
                if (thenable) {
                    if (has_id) {
                        dyn_app_build_error(-32000, "async handler not allowed in batch", id_s, &tmp);
                        wrote = 1;
                    }
                } else if (has_id && dyn_value_is_byte_source(ctx, res)) {
                    dyn_app_build_error(-32603,
                        "streaming body not allowed in a batch", id_s, &tmp);
                    wrote = 1;
                } else if (has_id) {
                    dyn_app_build_result(ctx, res, id_s, &tmp);
                    wrote = 1;
                }
                JS_FreeValue(ctx, res);
            }
        }
        JS_FreeValue(ctx, fn);
    }
    if (wrote) {
        if (comma)
            dyn_iobuf_append(out, ",", 1);
        dyn_iobuf_append(out, tmp.data, tmp.len);
    }
    dyn_iobuf_free(&tmp);
    if (method_s)
        JS_FreeCString(ctx, method_s);
    if (id_s)
        JS_FreeCString(ctx, id_s);
    JS_FreeValue(ctx, method_v);
    JS_FreeValue(ctx, id);
    JS_FreeValue(ctx, params);
    return wrote;
}

JSValue dyn_app_rpc(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    dyn_app_t* app;
    const char* path;
    dyn_app_route_t* nr;
    if (argc < 2 || !JS_IsObject(argv[1]))
        return JS_ThrowTypeError(ctx, "rpc(path, methods) requires a methods object");
    path = JS_ToCString(ctx, argv[0]);
    if (!path)
        return JS_EXCEPTION;
    app = (dyn_app_t*)dyn_res_native(ctx, this_val, dyn_app_class_id);
    if (!app) {
        JS_FreeCString(ctx, path);
        return JS_EXCEPTION;
    }
    nr = (dyn_app_route_t*)realloc(app->routes,
        (app->n_routes + 1) * sizeof(*nr));
    if (!nr) {
        JS_FreeCString(ctx, path);
        return JS_ThrowOutOfMemory(ctx);
    }
    app->routes = nr;
    nr = &app->routes[app->n_routes];
    memset(nr, 0, sizeof(*nr));
    nr->path = strdup(path);
    nr->type = APP_RPC;
    nr->handler = JS_DupValue(ctx, argv[1]);
    app->n_routes++;
    JS_FreeCString(ctx, path);
    return JS_UNDEFINED;
}
#endif
