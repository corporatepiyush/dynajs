#include "http-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_HTTP)
#define DYN_APP_MAX_HEADER (64 * 1024)
#define DYN_APP_MAX_BATCH 256

#define DYN_APP_CHAINED(fn)                                          \
    static JSValue fn##_chain(JSContext* ctx, JSValueConst this_val, \
        int argc, JSValueConst* argv)                                \
    {                                                                \
        JSValue r = fn(ctx, this_val, argc, argv);                   \
        if (JS_IsUndefined(r))                                       \
            return JS_DupValue(ctx, this_val);                       \
        return r;                                                    \
    }

static const char* const http_app_keys[] = { "port", "idleTimeoutMs", "maxConns", "host", "compress", "metrics",
    "workers", "backlog" };

static const char* const http_env_stream_keys[] = { "status", "contentType", "stream", "headers" };

static const JSClassDef dyn_http_client_class = {
    "HTTPClient",
    .finalizer = dyn_res_finalizer,
};

static const JSClassDef dyn_hc_stream_class = {
    "HTTPBodyStream",
    .finalizer = dyn_res_finalizer,
};

static const JSCFunctionListEntry dyn_hc_stream_proto[] = {
    JS_CFUNC_DEF("read", 1, dyn_hc_stream_read),
};

static const JSCFunctionListEntry dyn_http_client_proto[] = {
    JS_CFUNC_DEF("get", 1, dyn_http_client_get),
    JS_CFUNC_DEF("post", 1, dyn_http_client_post),
    JS_CFUNC_DEF("request", 2, dyn_http_client_request),
    JS_CFUNC_DEF("setTimeout", 1, dyn_http_client_set_timeout),
    JS_CFUNC_DEF("disconnect", 0, dyn_http_client_disconnect),
    JS_CFUNC_DEF("getStream", 1, dyn_http_client_get_stream),
    JS_CFUNC_DEF("useCookies", 1, dyn_http_client_use_cookies),
};

static const JSCFunctionListEntry dyn_http_server_proto[] = {
    JS_CFUNC_DEF("start", 0, dyn_http_server_start),
    JS_CFUNC_DEF("stop", 0, dyn_http_server_stop),
    JS_CGETSET_DEF("port", dyn_http_server_get_port, NULL),
};

static const JSClassDef dyn_http_server_class = {
    "HTTPServer",
    .finalizer = dyn_res_finalizer,
};

static const JSCFunctionListEntry dyn_http_async_proto[] = {
    JS_CFUNC_DEF("start", 0, dyn_http_async_start),
    JS_CFUNC_DEF("stop", 0, dyn_http_async_stop),
    JS_CGETSET_DEF("port", dyn_http_async_get_port, NULL),
};

static const JSClassDef dyn_http_async_class = {
    "HTTPServerAsync",
    .finalizer = dyn_res_finalizer,
};

static const JSClassDef dyn_ws_class = { "WsConn", .finalizer = dyn_ws_finalizer };

static const JSCFunctionListEntry dyn_ws_proto[] = {
    JS_CFUNC_DEF("send", 1, dyn_ws_send),
    JS_CFUNC_DEF("close", 0, dyn_ws_close_method),
    JS_CGETSET_DEF("bufferedAmount", dyn_ws_get_buffered, NULL),
};

static const JSClassDef dyn_sse_class = { "SseConn", .finalizer = dyn_sse_finalizer };

static const JSCFunctionListEntry dyn_sse_proto[] = {
    JS_CFUNC_DEF("send", 1, dyn_sse_send),
    JS_CFUNC_DEF("close", 0, dyn_sse_close_method),
    JS_CGETSET_DEF("bufferedAmount", dyn_sse_get_buffered, NULL),
};

static JSValue dyn_value_stream_member(JSContext* ctx, JSValueConst v)
{
    JSValue sv;
    if (!JS_IsObject(v) || dyn_value_is_byte_source(ctx, v))
        return JS_UNDEFINED;
    sv = dyn_app_own_get(ctx, v, "stream");
    if (!dyn_value_is_byte_source(ctx, sv)) {
        JS_FreeValue(ctx, sv);
        return JS_UNDEFINED;
    }
    return sv;
}

int dyn_app_try_stream_result(dyn_app_conn_t* c, JSValueConst result)
{
    JSContext* ctx = c->app->ctx;
    JSValue src = JS_UNDEFINED;
    JSValue sv, ctv;
    const char* cts = NULL;
    size_t cts_len = 0;
    int status = 200;
    dyn_app_stream_t* st;
    dyn_iobuf_t xh;
    char* head;
    size_t head_cap;
    int hn;

    if (c->st) {
        dyn_app_conn_close(c);
        return 1;
    }
    dyn_iobuf_init(&xh);
    if (dyn_value_is_byte_source(ctx, result)) {
        src = JS_DupValue(ctx, result);
    } else {
        const char* xerr;
        src = dyn_value_stream_member(ctx, result);
        if (JS_IsUndefined(src))
            return 0;
        xerr = dyn_app_env_headers(ctx, result, http_env_stream_keys,
            countof(http_env_stream_keys), &xh);
        if (xerr) {
            dyn_iobuf_free(&xh);
            JS_FreeValue(ctx, src);
            dyn_app_send_err(c, 500, xerr);
            return 1;
        }
        sv = dyn_app_own_get(ctx, result, "status");
        if (JS_IsException(sv)) {
            JS_FreeValue(ctx, sv);
            JS_FreeValue(ctx, JS_GetException(ctx));
        } else if (JS_IsNumber(sv)) {
            int32_t s32 = 200;
            if (JS_ToInt32(ctx, &s32, sv) == 0 && s32 >= 100 && s32 < 600)
                status = s32;
        }
        JS_FreeValue(ctx, sv);
        ctv = dyn_app_own_get(ctx, result, "contentType");
        if (JS_IsException(ctv)) {
            JS_FreeValue(ctx, ctv);
            JS_FreeValue(ctx, JS_GetException(ctx));
        } else if (JS_IsString(ctv)) {
            cts = JS_ToCStringLen(ctx, &cts_len, ctv);
        }
        JS_FreeValue(ctx, ctv);
    }
    if (cts) {
        int ctl = !httpk_value_gen_ok(cts, cts_len);
        if (ctl || cts_len > DYN_HTTP_CT_MAX) {
            JS_FreeCString(ctx, cts);
            JS_FreeValue(ctx, src);
            dyn_app_send_err(c, 500, ctl ? "{\"error\":\"stream contentType must not contain control "
                                           "characters\"}"
                                         : "{\"error\":\"stream contentType too long\"}");
            dyn_iobuf_free(&xh);
            return 1;
        }
    }
    if (js_streams_kind(src) == 1) {
        JSValue bs = js_streams_call(ctx, "fromReadableStream", 1, (JSValueConst*)&src);
        JS_FreeValue(ctx, src);
        src = bs;
        if (JS_IsException(src)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            if (cts)
                JS_FreeCString(ctx, cts);
            dyn_app_send_err(c, 500, "{\"error\":\"stream is locked\"}");
            dyn_iobuf_free(&xh);
            return 1;
        }
    }
    if (c->closed) {
        JS_FreeValue(ctx, src);
        if (cts)
            JS_FreeCString(ctx, cts);
        dyn_iobuf_free(&xh);
        return 1;
    }
    if (!c->http11) {
        JS_FreeValue(ctx, src);
        if (cts)
            JS_FreeCString(ctx, cts);
        dyn_app_send_err(c, 500,
            "{\"error\":\"streaming bodies require an HTTP/1.1 peer\"}");
        dyn_iobuf_free(&xh);
        return 1;
    }
    if (c->identity_refused) {
        JS_FreeValue(ctx, src);
        if (cts)
            JS_FreeCString(ctx, cts);
        c->identity_refused = 0;
        dyn_app_send_err(c, 406, "{\"error\":\"not acceptable\"}");
        dyn_iobuf_free(&xh);
        return 1;
    }
    st = (dyn_app_stream_t*)calloc(1, sizeof *st);
    if (!st) {
        JS_FreeValue(ctx, src);
        if (cts)
            JS_FreeCString(ctx, cts);
        dyn_app_send_err(c, 500, "{\"error\":\"oom\"}");
        dyn_iobuf_free(&xh);
        return 1;
    }
    st->status = status;
    st->ct = strdup(cts && *cts ? cts : "application/octet-stream");
    if (cts)
        JS_FreeCString(ctx, cts);
    st->frame_cap = DYN_APP_STREAM_CHUNK + 32;
    st->frame = (uint8_t*)malloc(st->frame_cap);
    st->pending = JS_UNDEFINED;
    if (st->frame) {
        JSValue dim = JS_NewInt64(ctx, DYN_APP_STREAM_CHUNK);
        st->chunk = JS_NewTypedArray(ctx, 1, &dim, JS_TYPED_ARRAY_UINT8);
        JS_FreeValue(ctx, dim);
        st->readfn = JS_GetPropertyStr(ctx, src, "read");
    }
    if (!st->frame || !st->ct || JS_IsException(st->chunk) || !JS_IsFunction(ctx, st->readfn)) {
        if (JS_IsException(st->chunk))
            JS_FreeValue(ctx, JS_GetException(ctx));
        JS_FreeValue(ctx, src);
        JS_FreeValue(ctx, st->readfn);
        JS_FreeValue(ctx, st->chunk);
        free(st->frame);
        free(st->ct);
        free(st);
        dyn_app_send_err(c, 500, "{\"error\":\"oom\"}");
        dyn_iobuf_free(&xh);
        return 1;
    }
    st->src = src;
    c->st = st;
    c->refs++;
    dyn_app_stream_enter(st);
    head_cap = 512 + xh.len;
    head = (char*)malloc(head_cap);
    hn = head ? snprintf(head, head_cap,
                    "HTTP/1.1 %d %s\r\n%s%sContent-Type: %s\r\n"
                    "Transfer-Encoding: chunked\r\nConnection: %s\r\n\r\n",
                    st->status, dyn_reason_phrase(st->status),
                    st->status >= 200 ? dyn_http_date((const char*)xh.data, xh.len) : "",
                    xh.len ? (const char*)xh.data : "", st->ct,
                    c->close_after ? "close" : "keep-alive")
              : 0;
    dyn_iobuf_free(&xh);
    if (hn <= 0 || (size_t)hn >= head_cap) {
        free(head);
        dyn_app_conn_close(c);
        dyn_app_send_err(c, 500, "{\"error\":\"stream start failed\"}");
        dyn_app_stream_retire(c);
        dyn_app_stream_leave(c);
        return 1;
    }
    st->in_send = 1;
    if (dyn_aio_send(c->app->aio, c->fd, (const uint8_t*)head,
            (unsigned)hn, DYN_AIO_HOLD, dyn_app_stream_sent, c)
        < 0) {
        free(head);
        st->in_send = 0;
        dyn_app_conn_close(c);
        dyn_app_stream_retire(c);
        dyn_app_stream_leave(c);
        return 1;
    }
    free(head);
    if (!st->in_send && !st->retiring)
        dyn_app_stream_resume(c);
    dyn_app_stream_leave(c);
    return 1;
}

static void dyn_pend_finalizer(JSRuntime* rt, JSValue val)
{
    dyn_app_pend_t* pd = (dyn_app_pend_t*)JS_GetOpaque(val, dyn_pend_class_id);
    (void)rt;
    if (pd) {
        JS_SetOpaque(val, NULL);
        dyn_app_pend_release(pd, 1);
    }
}

static const JSClassDef dyn_pend_class = {
    "PendingResponse",
    .finalizer = dyn_pend_finalizer,
};

static void dyn_app_dispatch_batch(dyn_app_conn_t* c, JSValueConst methods,
    JSValueConst arr)
{
    JSContext* ctx = c->app->ctx;
    uint32_t len = 0, i;
    int nresp = 0;
    dyn_iobuf_t out;
    JSValue lv = JS_GetPropertyStr(ctx, arr, "length");
    if (JS_ToUint32(ctx, &len, lv)) {
        JS_FreeValue(ctx, lv);
        dyn_app_rpc_error(c, 400, -32600, "Invalid Request", "null");
        return;
    }
    JS_FreeValue(ctx, lv);
    if (len == 0) {
        dyn_app_rpc_error(c, 400, -32600, "Invalid Request", "null");
        return;
    }
    if (len > DYN_APP_MAX_BATCH) {
        dyn_app_rpc_error(c, 400, -32600, "Invalid Request", "null");
        return;
    }
    dyn_iobuf_init(&out);
    dyn_iobuf_append(&out, "[", 1);
    for (i = 0; i < len; i++) {
        JSValue elem = JS_GetPropertyUint32(ctx, arr, i);
        if (dyn_app_rpc_build_one(c, methods, elem, &out, nresp > 0))
            nresp++;
        JS_FreeValue(ctx, elem);
    }
    dyn_iobuf_append(&out, "]", 1);
    if (nresp > 0)
        dyn_app_send_json(c, 200, (const char*)out.data, out.len);
    else
        dyn_app_send_json(c, 200, "", 0);
    dyn_iobuf_free(&out);
}

static void dyn_app_dispatch_rpc(dyn_app_conn_t* c, JSValueConst methods,
    const char* body, size_t body_len)
{
    JSContext* ctx = c->app->ctx;
    JSValue req, method_v, params, id, fn, result;
    const char *method_s, *id_s;

    {
        char* m = DYN_UNCONST(body);
        char save = m[body_len];
        m[body_len] = 0;
        req = JS_ParseJSON(ctx, body, body_len, "<rpc>");
        m[body_len] = save;
    }
    if (JS_IsException(req)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        dyn_app_rpc_error(c, 400, -32700, "Parse error", "null");
        return;
    }
    if (JS_IsArray(ctx, req)) {
        JSValue m = JS_DupValue(ctx, methods);
        dyn_app_dispatch_batch(c, m, req);
        JS_FreeValue(ctx, m);
        JS_FreeValue(ctx, req);
        return;
    }
    methods = JS_DupValue(ctx, methods);
    method_v = JS_GetPropertyStr(ctx, req, "method");
    id = JS_GetPropertyStr(ctx, req, "id");
    params = JS_GetPropertyStr(ctx, req, "params");
    {
        JSValue idj = JS_JSONStringify(ctx, id, JS_UNDEFINED, JS_UNDEFINED);
        id_s = (JS_IsException(idj) || JS_IsUndefined(idj))
            ? NULL
            : JS_ToCString(ctx, idj);
        JS_FreeValue(ctx, idj);
    }

    if (JS_IsUndefined(id)) {
        goto cleanup;
    }

    method_s = JS_IsString(method_v) ? JS_ToCString(ctx, method_v) : NULL;
    if (!method_s) {
        dyn_app_rpc_error(c, 400, -32600, "Invalid Request", id_s);
        goto cleanup;
    }
    fn = dyn_app_own_get(ctx, methods, method_s);
    if (!JS_IsFunction(ctx, fn)) {
        JS_FreeValue(ctx, fn);
        dyn_app_rpc_error(c, 404, -32601, "Method not found", id_s);
        JS_FreeCString(ctx, method_s);
        goto cleanup;
    }
    if (JS_IsUndefined(params)) {
        result = JS_Call(ctx, fn, JS_UNDEFINED, 0, NULL);
    } else if (JS_IsArray(ctx, params)) {
        JSValue lenv = JS_GetPropertyStr(ctx, params, "length");
        uint32_t len = 0, i;
        JSValueConst argv_stack[8];
        JSValueConst* argv = NULL;
        if (JS_ToUint32(ctx, &len, lenv)) {
            JS_FreeValue(ctx, lenv);
            JS_FreeValue(ctx, fn);
            JS_FreeCString(ctx, method_s);
            dyn_app_rpc_error(c, 400, -32600, "Invalid Request", "null");
            goto cleanup;
        }
        JS_FreeValue(ctx, lenv);
        if (len > DYN_APP_MAX_PARAMS) {
            JS_FreeValue(ctx, fn);
            JS_FreeCString(ctx, method_s);
            dyn_app_rpc_error(c, 400, -32602, "Invalid params", id_s);
            goto cleanup;
        }
        if (len > 0) {
            if (len <= countof(argv_stack)) {
                argv = argv_stack;
            } else {
                argv = (JSValueConst*)malloc(len * sizeof(*argv));
                if (!argv) {
                    JS_FreeValue(ctx, fn);
                    JS_FreeCString(ctx, method_s);
                    dyn_app_send_rpc_throw(c, JS_ThrowOutOfMemory(ctx), id_s);
                    goto cleanup;
                }
            }
            for (i = 0; i < len; i++)
                argv[i] = JS_GetPropertyUint32(ctx, params, i);
            result = JS_Call(ctx, fn, JS_UNDEFINED, len, argv);
            for (i = 0; i < len; i++)
                JS_FreeValue(ctx, argv[i]);
            if (argv != argv_stack)
                free(argv);
        } else {
            result = JS_Call(ctx, fn, JS_UNDEFINED, 0, NULL);
        }
    } else {
        JSValueConst argv[1] = { params };
        result = JS_Call(ctx, fn, JS_UNDEFINED, 1, argv);
    }
    JS_FreeValue(ctx, fn);
    JS_FreeCString(ctx, method_s);

    if (JS_IsException(result)) {
        JSValue exc = JS_GetException(ctx);
        dyn_app_send_rpc_throw(c, exc, id_s);
        JS_FreeValue(ctx, exc);
        goto cleanup;
    }
    if (JS_IsObject(result)) {
        JSValue then = JS_GetPropertyStr(ctx, result, "then");
        if (JS_IsFunction(ctx, then)) {
            dyn_app_pend_t* pd = (dyn_app_pend_t*)calloc(1, sizeof(*pd));
            if (pd) {
                JSValue funcs[2] = { JS_UNDEFINED, JS_UNDEFINED };
                JSValue promise, pthen, pobj, onres, onrej, tr;
                JSValueConst thenargs[2], settleargs[2];

                pd->conn = c;
                c->refs++;
                pd->id = strdup(id_s ? id_s : "null");
                pd->accept_gzip = c->accept_gzip;
                pd->identity_refused = c->identity_refused;
                pobj = JS_NewObjectClass(ctx, dyn_pend_class_id);
                if (JS_IsException(pobj)) {
                    JS_FreeValue(ctx, JS_GetException(ctx));
                    dyn_app_pend_release(pd, 0);
                    dyn_app_send_rpc_throw(c, JS_ThrowOutOfMemory(ctx), id_s);
                    JS_FreeValue(ctx, then);
                    JS_FreeValue(ctx, result);
                    goto cleanup;
                }
                JS_SetOpaque(pobj, pd);
                promise = JS_NewPromiseCapability(ctx, funcs);
                if (JS_IsException(promise) || !pd->id) {
                    if (!JS_IsException(promise))
                        JS_FreeValue(ctx, promise);
                    JS_FreeValue(ctx, funcs[0]);
                    JS_FreeValue(ctx, funcs[1]);
                    JS_FreeValue(ctx, pobj);
                    dyn_app_send_rpc_throw(c, JS_ThrowOutOfMemory(ctx), id_s);
                    JS_FreeValue(ctx, then);
                    JS_FreeValue(ctx, result);
                    goto cleanup;
                }
                onres = JS_NewCFunctionData(ctx, dyn_app_rpc_settle, 1, 0, 1, &pobj);
                onrej = JS_NewCFunctionData(ctx, dyn_app_rpc_settle, 1, 1, 1, &pobj);
                pthen = JS_GetPropertyStr(ctx, promise, "then");
                settleargs[0] = onres;
                settleargs[1] = onrej;
                tr = JS_IsException(pthen) ? pthen
                                           : JS_Call(ctx, pthen, promise, 2, settleargs);
                if (JS_IsException(tr)) {
                    JS_FreeValue(ctx, tr);
                    JS_FreeValue(ctx, JS_GetException(ctx));
                    JS_FreeValue(ctx, pthen);
                    JS_FreeValue(ctx, onres);
                    JS_FreeValue(ctx, onrej);
                    JS_FreeValue(ctx, funcs[0]);
                    JS_FreeValue(ctx, funcs[1]);
                    JS_FreeValue(ctx, promise);
                    JS_FreeValue(ctx, pobj);
                    dyn_app_send_rpc_throw(c, JS_ThrowOutOfMemory(ctx), id_s);
                    JS_FreeValue(ctx, then);
                    JS_FreeValue(ctx, result);
                    goto cleanup;
                }
                JS_FreeValue(ctx, tr);
                JS_FreeValue(ctx, pthen);
                JS_FreeValue(ctx, onres);
                JS_FreeValue(ctx, onrej);
                pd->counted = 1;
                c->resp_parked++;
                thenargs[0] = funcs[0];
                thenargs[1] = funcs[1];
                tr = JS_Call(ctx, then, result, 2, thenargs);
                if (JS_IsException(tr)) {
                    JSValue exc = JS_GetException(ctx);
                    JS_FreeValue(ctx, tr);
                    tr = JS_Call(ctx, funcs[1], JS_UNDEFINED, 1, &exc);
                    JS_FreeValue(ctx, tr);
                    JS_FreeValue(ctx, exc);
                } else {
                    JS_FreeValue(ctx, tr);
                }
                JS_FreeValue(ctx, funcs[0]);
                JS_FreeValue(ctx, funcs[1]);
                JS_FreeValue(ctx, promise);
                JS_FreeValue(ctx, pobj);
                JS_FreeValue(ctx, then);
                JS_FreeValue(ctx, result);
                goto cleanup;
            }
        }
        JS_FreeValue(ctx, then);
    }
    dyn_app_send_rpc_result(c, result, id_s);
    JS_FreeValue(ctx, result);
cleanup:
    if (id_s)
        JS_FreeCString(ctx, id_s);
    JS_FreeValue(ctx, method_v);
    JS_FreeValue(ctx, id);
    JS_FreeValue(ctx, params);
    JS_FreeValue(ctx, req);
    JS_FreeValue(ctx, methods);
}

static int dyn_app_try_dyn(dyn_app_conn_t* c, const char* base, size_t head_len,
    const char* path, const char* body, size_t body_len, int mount_fallback);

static void dyn_app_process_loop(dyn_app_conn_t* c)
{
    dyn_app_t* app = c->app;
    for (;;) {
        const char* base = (const char*)dyn_iobuf_rdata(&c->in);
        size_t avail = dyn_iobuf_rlen(&c->in);
        size_t from = c->hdr_scan_from < avail ? c->hdr_scan_from : avail;
        const char* hdr_end = dyn_memfind(base + from, avail - from, "\r\n\r\n", 4);
        size_t head_len, body_len, req_total, clv_len = 0, i;
        const char* cl;
        char path[1024];
        int64_t clen = 0;
        const dyn_app_route_t* route = NULL;
        const dyn_app_route_t* mount = NULL;
        dyn_reqinfo_t ri;

        if (dyn_app_out_blocked(c))
            return;

        if (!hdr_end) {
            size_t resume = c->hdr_counted >= 1 ? c->hdr_counted - 1 : 0;
            c->hdr_scan_from = avail >= 3 ? avail - 3 : 0;
            if (resume <= avail) {
                c->hdr_lines += (uint32_t)dyn_req_header_count(
                    base + resume, avail - resume);
                c->hdr_counted = avail;
            }
            if (avail > DYN_APP_MAX_HEADER || c->hdr_lines > DYN_APP_MAX_HEADER_COUNT) {
                dyn_app_send_err(c, 431, "{\"error\":\"header too large\"}");
                dyn_app_conn_close(c);
            }
            return;
        }

        {
            int src = dyn_scan_head(base, avail, &ri);
            if (src == DYN_HEAD_ERR_LINE) {
                dyn_app_send_err(c, 400, "{\"error\":\"malformed header line\"}");
                dyn_app_conn_close(c);
                return;
            }
            if (src != 0 || ri.head_len == 0 || ri.head_len > (size_t)(hdr_end - base) + 4) {
                dyn_app_send_err(c, 400, "{\"error\":\"malformed header line\"}");
                dyn_app_conn_close(c);
                return;
            }
        }
        head_len = ri.head_len;

        if (head_len > DYN_APP_MAX_HEADER || ri.n_lines > DYN_APP_MAX_HEADER_COUNT) {
            dyn_app_send_err(c, 431, "{\"error\":\"header too large\"}");
            dyn_app_conn_close(c);
            return;
        }

        if (ri.has_te) {
            dyn_app_send_err(c, 501, "{\"error\":\"transfer-encoding unsupported\"}");
            dyn_app_conn_close(c);
            return;
        }
        if (ri.cl_count > 1) {
            dyn_app_send_err(c, 400, "{\"error\":\"duplicate content-length\"}");
            dyn_app_conn_close(c);
            return;
        }
        if (ri.http11 && ri.host_count != 1) {
            dyn_app_send_err(c, 400, "{\"error\":\"bad host\"}");
            dyn_app_conn_close(c);
            return;
        }
        if (ri.dup) {
            dyn_app_send_err(c, 400, "{\"error\":\"duplicate header\"}");
            dyn_app_conn_close(c);
            return;
        }
        cl = ri.cl;
        clv_len = ri.cl_len;
        if (cl) {
            if (clv_len == 0) {
                dyn_app_send_err(c, 400, "{\"error\":\"bad content-length\"}");
                dyn_app_conn_close(c);
                return;
            }
            for (i = 0; i < clv_len; i++) {
                if (cl[i] < '0' || cl[i] > '9') {
                    dyn_app_send_err(c, 400, "{\"error\":\"bad content-length\"}");
                    dyn_app_conn_close(c);
                    return;
                }
                clen = clen * 10 + (cl[i] - '0');
                if (clen > (int64_t)(1LL << 40)) {
                    clen = (int64_t)(1LL << 40);
                    break;
                }
            }
        }

        if (!c->req_start_ms)
            c->req_start_ms = dyn_timer_now_ms();
        {
            c->accept_gzip = 0;
            c->identity_refused = 0;
            if (ri.ae) {
                int qg = httpk_qvalue(ri.ae, ri.ae_len, "gzip");
                if (qg < 0)
                    qg = httpk_qvalue(ri.ae, ri.ae_len, "*");
                c->accept_gzip = qg > 0;
                {
                    int qi = httpk_qvalue(ri.ae, ri.ae_len, "identity");
                    if (qi < 0 && httpk_qvalue(ri.ae, ri.ae_len, "*") == 0)
                        qi = 0;
                    c->identity_refused = (qi == 0);
                }
            }
        }

        {
            int keep = ri.http11;
            if (httpk_list_has(ri.conn, ri.conn_len, "close"))
                keep = 0;
            else if (httpk_list_has(ri.conn, ri.conn_len, "keep-alive"))
                keep = 1;
            c->close_after = !keep;
            c->http11 = ri.http11;
            c->head_only = ri.method_len == 4 && memcmp(ri.method, "HEAD", 4) == 0;
        }

        if (ri.bad_target) {
            dyn_app_send_err(c, 400, "{\"error\":\"bad request\"}");
            dyn_app_conn_close(c);
            return;
        }
        {
            size_t plen = ri.target_len < sizeof(path) - 1
                ? ri.target_len
                : sizeof(path) - 1;
            size_t r2, w2;
            memcpy(path, ri.target, plen);
            path[plen] = '\0';
            for (r2 = 0, w2 = 0; r2 < plen; r2++) {
                path[w2++] = path[r2];
                if (path[r2] == '/')
                    while (r2 + 1 < plen && path[r2 + 1] == '/')
                        r2++;
            }
            path[w2] = '\0';
        }
        route = dyn_app_route_find(app, path, &mount);
        if (!route)
            route = mount;
        else
            mount = NULL;

        if (clen > (int64_t)DYN_ACONN_MAX_REQ && !(route && route->type == APP_UPLOAD)) {
            dyn_app_reject_linger(c, 413, "{\"error\":\"payload too large\"}", clen);
            return;
        }

        if (route && route->type == APP_UPLOAD) {
            dyn_app_upload_start(c, route, base, head_len, clen);
            if (c->up || c->closed || c->linger)
                return;
            continue;
        }

        body_len = clen > DYN_ACONN_MAX_REQ ? (size_t)DYN_ACONN_MAX_REQ + 1 : (size_t)clen;
        req_total = head_len + body_len;
        if (avail < req_total)
            return;

        if (mount && dyn_app_try_dyn(c, base, head_len, path, base + head_len, body_len, 1) != 0) {
            route = NULL;
        } else if (!route) {
            int dyn_rc = dyn_app_try_dyn(c, base, head_len, path,
                base + head_len, body_len, 0);
            if (dyn_rc == 0) {
                if (app->metrics_http && strcmp(path, "/metrics") == 0) {
                    size_t ml = 0;
                    char* m = dyn_metrics_c_scrape(&ml);
                    if (m) {
                        dyn_app_send_body(c, 200, "text/plain; version=0.0.4",
                            m, ml);
                        free(m);
                    } else {
                        dyn_app_send_err(c, 500, "{\"error\":\"oom\"}");
                    }
                } else if (app->metrics_http && strcmp(path, "/healthz") == 0) {
                    dyn_app_send_json(c, 200, "{\"ok\":true}", 11);
                } else {
                    dyn_app_send_err(c, 404, "{\"error\":\"not found\"}");
                }
            }
        } else if (route->type == APP_RPC) {
            {
                const char* rct = ri.ct;
                size_t rctlen = ri.ct_len;
                if (rct) {
                    char typebuf[64];
                    size_t n = rctlen < sizeof(typebuf) - 1 ? rctlen : sizeof(typebuf) - 1;
                    char* sc;
                    memcpy(typebuf, rct, n);
                    typebuf[n] = 0;
                    if ((sc = strchr(typebuf, ';')) != NULL)
                        *sc = 0;
                    {
                        size_t tlen = strlen(typebuf);
                        while (tlen && (typebuf[tlen - 1] == ' ' || typebuf[tlen - 1] == '\t'))
                            typebuf[--tlen] = 0;
                    }
                    if (strcasecmp(typebuf, "application/json") != 0) {
                        dyn_app_send_err(c, 415, "{\"error\":\"unsupported media type\"}");
                        dyn_app_conn_close(c);
                        return;
                    }
                }
            }
            dyn_app_dispatch_rpc(c, route->handler, base + head_len, body_len);
        } else if (route->type == APP_STATIC) {
            dyn_app_serve_static(c, route, path, base, head_len);
        } else if (route->type == APP_PROXY) {
            dyn_app_proxy_start(c, route, base, head_len, path, (int64_t)body_len);
        } else if (route->type == APP_WS) {
            int upgraded = dyn_app_ws_handshake(c, route, base, head_len);
            if (upgraded > 0) {
                dyn_iobuf_consume(&c->in, req_total);
                c->hdr_scan_from = 0;
                c->hdr_counted = 0;
                c->hdr_lines = 0;
                return;
            }
            if (upgraded < 0)
                dyn_app_send_err(c, 403, "{\"error\":\"websocket upgrade refused\"}");
            else
                dyn_app_send_err(c, 400, "{\"error\":\"expected websocket upgrade\"}");
        } else if (route->type == APP_SSE) {
            dyn_app_sse_handshake(c, route);
            dyn_iobuf_consume(&c->in, req_total);
            c->hdr_scan_from = 0;
            c->hdr_counted = 0;
            c->hdr_lines = 0;
            return;
        }
        dyn_iobuf_consume(&c->in, req_total);
        c->hdr_scan_from = 0;
        c->hdr_counted = 0;
        c->hdr_lines = 0;
        if (c->closed)
            return;
        if (c->resp_parked || c->st)
            return;
        if (c->close_after)
            return;
    }
}

void dyn_app_process(dyn_app_conn_t* c)
{
    if (c->in_process || c->resp_parked || c->st || c->linger)
        return;
    c->in_process = 1;
    c->refs++;
    dyn_app_process_loop(c);
    c->in_process = 0;
    dyn_app_conn_unref(c);
}

static void dyn_app_dispose(void* native)
{
    dyn_app_t* app = (dyn_app_t*)native;
    size_t i;
    JS_RemoveShutdownSweep(JS_GetRuntime(app->ctx), dyn_app_shutdown_sweep, app);
    while (app->conns) {
        dyn_app_conn_t* c = (dyn_app_conn_t*)app->conns;
        dyn_app_conn_unlink(c);
        dyn_app_conn_close(c);
    }
    if (app->timers) {
        if (app->in_sweep)
            app->timers_orphan = app->timers;
        else
            dyn_timers_free(app->timers);
        app->timers = NULL;
        app->idle_ms = 0;
    }
    if (app->aio) {
        if (!app->in_sweep)
            dyn_net_off_drain(app);
        if (app->listen_fd >= 0)
            dyn_aio_close(app->aio, app->listen_fd);
        dyn_net_reactor_release(app->ctx);
    }
    for (i = 0; i < app->n_routes; i++) {
        size_t k;
        free(app->routes[i].path);
        free(app->routes[i].dir);
        if (app->routes[i].type == APP_STATIC)
            dyn_static_root_free(app->routes[i].sroot);
        else
            free(app->routes[i].up_host);
        free(app->routes[i].dyn_method);
        free(app->routes[i].dyn_pattern);
        for (k = 0; k < app->routes[i].n_allow; k++)
            free(app->routes[i].allow[k]);
        free(app->routes[i].allow);
        JS_FreeValue(app->ctx, app->routes[i].handler);
    }
    free(app->routes);
    app->routes = NULL;
    app->n_routes = 0;
    dyn_app_ridx_free(app);
    for (i = 0; i < app->n_mw; i++)
        JS_FreeValue(app->ctx, app->mw[i]);
    JS_FreeObjectTemplate(app->ctx, app->req_tpl);
    app->req_tpl = NULL;
    dyn_app_hdr_templates_free(app->ctx, app);
    free(app->mw);
    app->mw = NULL;
    app->n_mw = 0;
    app->cap_mw = 0;
    free(app->listen_host);
    app->dispose_called = 1;
    if (app->n_conns == 0 && !app->in_sweep)
        free(app);
}

static void dyn_app_gc_mark(JSRuntime* rt, JSValueConst val,
    JS_MarkFunc* mark_func)
{
    DynResource* r = (DynResource*)JS_GetOpaque(val, dyn_app_class_id);
    dyn_app_t* app = (r && !r->closed) ? (dyn_app_t*)r->native : NULL;
    size_t i;

    if (!app)
        return;
    for (i = 0; i < app->n_routes; i++)
        JS_MarkValue(rt, app->routes[i].handler, mark_func);
    for (i = 0; i < app->n_mw; i++)
        JS_MarkValue(rt, app->mw[i], mark_func);
    JS_MarkObjectTemplate(rt, app->req_tpl, mark_func);
    dyn_app_hdr_templates_mark(rt, app, mark_func);
}

static const JSClassDef dyn_app_class = {
    "App",
    .finalizer = dyn_res_finalizer,
    .gc_mark = dyn_app_gc_mark,
};

static JSValue dyn_app_ctor(JSContext* ctx, JSValueConst new_target, int argc,
    JSValueConst* argv)
{
    dyn_app_t* app;
    int64_t port = 0;
    int64_t workers = 0, backlog = 0;
    char* host_dup = NULL;
    int64_t idle_ms = 30000;
    int32_t max_conns = DYN_ACONN_MAX_CONNS_DEFAULT;
    int compress_opt = 1;
    int metrics_opt = 0;
    if (argc > 0 && JS_IsObject(argv[0])) {
        JSValue v;
        if (dyn_opts_strict(ctx, argv[0], http_app_keys, 8))
            return JS_EXCEPTION;
        v = JS_GetPropertyStr(ctx, argv[0], "port");
        if (!JS_IsUndefined(v)) {
            if (JS_ToInt64(ctx, &port, v))
                return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[0], "idleTimeoutMs");
        if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
            if (JS_ToInt64(ctx, &idle_ms, v)) {
                JS_FreeValue(ctx, v);
                return JS_EXCEPTION;
            }
            if (idle_ms < 0)
                idle_ms = 0;
        }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[0], "maxConns");
        if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
            if (JS_ToInt32(ctx, &max_conns, v)) {
                JS_FreeValue(ctx, v);
                return JS_EXCEPTION;
            }
            if (max_conns < 0)
                max_conns = 0;
        }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[0], "host");
        if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
            const char* h = JS_ToCString(ctx, v);
            if (!h) {
                JS_FreeValue(ctx, v);
                return JS_EXCEPTION;
            }
            host_dup = strdup(h);
            JS_FreeCString(ctx, h);
            if (!host_dup) {
                JS_FreeValue(ctx, v);
                return JS_ThrowOutOfMemory(ctx);
            }
        }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[0], "compress");
        if (!JS_IsUndefined(v)) {
            compress_opt = JS_ToBool(ctx, v);
            if (compress_opt < 0) {
                JS_FreeValue(ctx, v);
                return JS_EXCEPTION;
            }
        }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[0], "metrics");
        if (!JS_IsUndefined(v)) {
            metrics_opt = JS_ToBool(ctx, v);
            if (metrics_opt < 0) {
                JS_FreeValue(ctx, v);
                return JS_EXCEPTION;
            }
        }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[0], "workers");
        if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
            double d;
            if (JS_ToFloat64(ctx, &d, v)) {
                JS_FreeValue(ctx, v);
                return JS_EXCEPTION;
            }
            if (!(d >= 1 && d <= 64) || (int64_t)d != d) {
                JS_FreeValue(ctx, v);
                return JS_ThrowRangeError(ctx,
                    "App: workers must be an integer from 1 to 64 (the same "
                    "range the thread-pool HTTPServer accepts)");
            }
            workers = (int64_t)d;
        }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[0], "backlog");
        if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
            double d;
            if (JS_ToFloat64(ctx, &d, v)) {
                JS_FreeValue(ctx, v);
                return JS_EXCEPTION;
            }
            if (!(d >= 1 && d <= 65535) || (int64_t)d != d) {
                JS_FreeValue(ctx, v);
                return JS_ThrowRangeError(ctx,
                    "App: backlog must be an integer from 1 to 65535");
            }
            backlog = (int64_t)d;
        }
        JS_FreeValue(ctx, v);
    }
    app = (dyn_app_t*)calloc(1, sizeof(*app));
    if (!app)
        return JS_ThrowOutOfMemory(ctx);
    app->ctx = ctx;
    app->listen_fd = -1;
    app->port = (uint16_t)port;
    app->idle_ms = (uint64_t)idle_ms;
    app->max_conns = max_conns;
    app->compress = compress_opt;
    app->metrics_http = metrics_opt;
    app->listen_host = host_dup;
    app->workers = (int)workers;
    app->backlog = (int)backlog;
    return dyn_res_wrap(ctx, new_target, dyn_app_class_id, app, dyn_app_dispose);
}

static int dyn_is_pname(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}

static int dyn_app_dyn_validate(JSContext* ctx, const char* pat)
{
    size_t n, seg_start;
    int wild = 0;
    char names[16][64];
    int nnames = 0;
    if (!pat || !*pat)
        return JS_ThrowTypeError(ctx, "dynamic route pattern must be a non-empty string"), -1;
    n = strlen(pat);
    if (n > 1024)
        return JS_ThrowTypeError(ctx, "dynamic route pattern exceeds 1024 bytes"), -1;
    if (pat[0] != '/')
        return JS_ThrowTypeError(ctx, "dynamic route pattern must start with '/'"), -1;
    if (strchr(pat, '?') || strchr(pat, '#'))
        return JS_ThrowTypeError(ctx, "dynamic route pattern is a path only (no query or fragment)"), -1;
    seg_start = 1;
    while (1) {
        size_t seg_end = seg_start;
        while (seg_end < n && pat[seg_end] != '/')
            seg_end++;
        {
            size_t slen = seg_end - seg_start;
            if (slen == 0 && seg_end < n) {
                JS_ThrowTypeError(ctx, "dynamic route pattern has an empty segment");
                return -1;
            }
            if (slen > 0) {
                const char* seg = pat + seg_start;
                if (seg[0] == ':') {
                    size_t k;
                    if (slen < 2) {
                        JS_ThrowTypeError(ctx, "dynamic route ':param' needs a name");
                        return -1;
                    }
                    for (k = 1; k < slen; k++)
                        if (!dyn_is_pname(seg[k])) {
                            JS_ThrowTypeError(ctx, "dynamic route ':param' names match [A-Za-z0-9_]+");
                            return -1;
                        }
                    if (slen - 1 >= sizeof names[0]) {
                        JS_ThrowTypeError(ctx, "dynamic route param name too long");
                        return -1;
                    }
                    for (k = 0; k < (size_t)nnames; k++)
                        if (strlen(names[k]) == slen - 1 && !memcmp(names[k], seg + 1, slen - 1)) {
                            JS_ThrowTypeError(ctx, "dynamic route has a duplicate param name");
                            return -1;
                        }
                    if (nnames >= 16) {
                        JS_ThrowTypeError(ctx,
                            "dynamic route has more than 16 named captures");
                        return -1;
                    }
                    memcpy(names[nnames], seg + 1, slen - 1);
                    names[nnames][slen - 1] = '\0';
                    nnames++;
                    if (wild) {
                        JS_ThrowTypeError(ctx, "dynamic route wildcard must be the last segment");
                        return -1;
                    }
                } else if (seg[0] == '*') {
                    size_t k;
                    if (wild) {
                        JS_ThrowTypeError(ctx, "dynamic route has more than one wildcard");
                        return -1;
                    }
                    if (slen < 2) {
                        JS_ThrowTypeError(ctx, "dynamic route '*rest' needs a name");
                        return -1;
                    }
                    for (k = 1; k < slen; k++)
                        if (!dyn_is_pname(seg[k])) {
                            JS_ThrowTypeError(ctx, "dynamic route '*rest' names match [A-Za-z0-9_]+");
                            return -1;
                        }
                    for (k = 0; k < (size_t)nnames; k++)
                        if (strlen(names[k]) == slen - 1 && !memcmp(names[k], seg + 1, slen - 1)) {
                            JS_ThrowTypeError(ctx, "dynamic route has a duplicate param name");
                            return -1;
                        }
                    if (seg_end < n) {
                        JS_ThrowTypeError(ctx, "dynamic route wildcard must be the last segment");
                        return -1;
                    }
                    wild = 1;
                } else {
                    size_t k;
                    for (k = 0; k < slen; k++)
                        if (seg[k] == ':' || seg[k] == '*') {
                            JS_ThrowTypeError(ctx, "dynamic route ':' and '*' start a segment");
                            return -1;
                        }
                }
            }
        }
        if (seg_end >= n)
            break;
        seg_start = seg_end + 1;
    }
    return 0;
}

static int dyn_app_dyn_match(JSContext* ctx, const char* pat, const char* path,
    JSValue params)
{
    const char *pp = pat, *qp = path;
    int probe = JS_IsUndefined(params);
    while (1) {
        const char *pe, *qe;
        size_t plen, qlen, qrem;
        while (*pp == '/')
            pp++;
        while (*qp == '/')
            qp++;
        if (!*pp && !*qp)
            return 1;
        if (!*pp || !*qp) {
            if (*pp == '*' || (pp[0] == '*')) {
                const char* nm = pp + 1;
                size_t nl = strlen(nm);
                char* dec;
                size_t dn;
                if (nl == 0)
                    return 0;
                if (probe)
                    return 1;
                dec = dyn_pct_decode("", 0, 0, &dn);
                if (!dec) {
                    JS_ThrowOutOfMemory(ctx);
                    return -1;
                }
                {
                    JSValue v = JS_NewStringLen(ctx, dec, dn);
                    free(dec);
                    if (JS_IsException(v))
                        return -1;
                    if (JS_DefinePropertyValueStr(ctx, params, nm, v,
                            JS_PROP_C_W_E)
                        < 0)
                        return -1;
                }
                return 1;
            }
            return 0;
        }
        pe = strchr(pp, '/');
        qe = strchr(qp, '/');
        plen = pe ? (size_t)(pe - pp) : strlen(pp);
        qrem = strlen(qp);
        qlen = qe ? (size_t)(qe - qp) : qrem;
        if (pp[0] == ':') {
            char nm[64];
            char* dec;
            size_t dn;
            if (qlen == 0)
                return 0;
            if (plen - 1 >= sizeof nm)
                return 0;
            memcpy(nm, pp + 1, plen - 1);
            nm[plen - 1] = '\0';
            if (probe)
                goto next_segment;
            if (dyn_pct_plain(qp, qlen, 0)) {
                JSValue v = JS_NewStringLen(ctx, qp, qlen);
                if (JS_IsException(v))
                    return -1;
                if (JS_DefinePropertyValueStr(ctx, params, nm, v, JS_PROP_C_W_E) < 0)
                    return -1;
                goto next_segment;
            }
            dec = dyn_pct_decode(qp, qlen, 0, &dn);
            if (!dec) {
                JS_ThrowOutOfMemory(ctx);
                return -1;
            }
            {
                JSValue v = JS_NewStringLen(ctx, dec, dn);
                free(dec);
                if (JS_IsException(v))
                    return -1;
                if (JS_DefinePropertyValueStr(ctx, params, nm, v,
                        JS_PROP_C_W_E)
                    < 0)
                    return -1;
            }
        } else if (pp[0] == '*') {
            char nm[64];
            size_t rlen = qrem;
            char* dec;
            size_t dn;
            if (plen - 1 >= sizeof nm)
                return 0;
            memcpy(nm, pp + 1, plen - 1);
            nm[plen - 1] = '\0';
            if (probe)
                return 1;
            dec = dyn_pct_decode(qp, rlen, 0, &dn);
            if (!dec) {
                JS_ThrowOutOfMemory(ctx);
                return -1;
            }
            {
                JSValue v = JS_NewStringLen(ctx, dec, dn);
                free(dec);
                if (JS_IsException(v))
                    return -1;
                if (JS_DefinePropertyValueStr(ctx, params, nm, v,
                        JS_PROP_C_W_E)
                    < 0)
                    return -1;
            }
            return 1;
        } else {
            if (plen != qlen || memcmp(pp, qp, plen) != 0)
                return 0;
        }
    next_segment:
        pp += plen;
        qp += qlen;
    }
}

static void dyn_app_dispatch_dyn(dyn_app_conn_t* c, const dyn_app_route_t* rt,
    JSValue ctx_obj)
{
    JSContext* ctx = c->app->ctx;
    dyn_app_t* app = c->app;
    size_t i;
    JSValue handler = JS_DupValue(ctx, rt->handler);
    for (i = 0; i < app->n_mw; i++) {
        JSValueConst a[1] = { ctx_obj };
        JSValue r = JS_Call(ctx, app->mw[i], JS_UNDEFINED, 1, a);
        if (JS_IsException(r)) {
            JSValue exc = JS_GetException(ctx);
            size_t em_len = 0;
            const char* em = JS_ToCStringLen(ctx, &em_len, exc);
            JS_FreeValue(ctx, exc);
            if (!em)
                JS_FreeValue(ctx, JS_GetException(ctx));
            JS_FreeValue(ctx, ctx_obj);
            JS_FreeValue(ctx, handler);
            dyn_app_dyn_err_json_n(c, 500, em ? em : "Server error",
                em ? em_len : strlen("Server error"));
            if (em)
                JS_FreeCString(ctx, em);
            return;
        }
        if (JS_IsObject(r)) {
            JSValue th = JS_GetPropertyStr(ctx, r, "then");
            int is_then = JS_IsFunction(ctx, th);
            JS_FreeValue(ctx, th);
            if (is_then) {
                JS_FreeValue(ctx, r);
                JS_FreeValue(ctx, ctx_obj);
                JS_FreeValue(ctx, handler);
                dyn_app_send_err(c, 500, "{\"error\":\"async middleware not supported\"}");
                return;
            }
        }
        if (JS_IsObject(r)) {
            JSValue rv = dyn_app_own_get(ctx, r, "response");
            int has = !JS_IsException(rv) && !JS_IsUndefined(rv);
            JS_FreeValue(ctx, r);
            if (JS_IsException(rv)) {
                JS_FreeValue(ctx, JS_GetException(ctx));
                JS_FreeValue(ctx, ctx_obj);
                JS_FreeValue(ctx, handler);
                dyn_app_send_err(c, 500, "{\"error\":\"middleware response read failed\"}");
                return;
            }
            if (has) {
                JS_FreeValue(ctx, ctx_obj);
                JS_FreeValue(ctx, handler);
                dyn_app_dyn_send_or_await(c, rv);
                return;
            }
            JS_FreeValue(ctx, rv);
            if (app->dispose_called) {
                JS_FreeValue(ctx, ctx_obj);
                JS_FreeValue(ctx, handler);
                dyn_app_send_err(c, 503, "{\"error\":\"server stopping\"}");
                return;
            }
            continue;
        }
        JS_FreeValue(ctx, r);
        if (app->dispose_called) {
            JS_FreeValue(ctx, ctx_obj);
            JS_FreeValue(ctx, handler);
            dyn_app_send_err(c, 503, "{\"error\":\"server stopping\"}");
            return;
        }
    }
    if (app->dispose_called) {
        JS_FreeValue(ctx, ctx_obj);
        JS_FreeValue(ctx, handler);
        dyn_app_send_err(c, 503, "{\"error\":\"server stopping\"}");
        return;
    }
    {
        JSValueConst a[1] = { ctx_obj };
        JSValue res = JS_Call(ctx, handler, JS_UNDEFINED, 1, a);
        JS_FreeValue(ctx, ctx_obj);
        JS_FreeValue(ctx, handler);
        if (JS_IsException(res)) {
            JSValue exc = JS_GetException(ctx);
            size_t em_len = 0;
            const char* em = JS_ToCStringLen(ctx, &em_len, exc);
            JS_FreeValue(ctx, exc);
            if (!em)
                JS_FreeValue(ctx, JS_GetException(ctx));
            dyn_app_dyn_err_json_n(c, 500, em ? em : "Server error",
                em ? em_len : strlen("Server error"));
            if (em)
                JS_FreeCString(ctx, em);
            return;
        }
        dyn_app_dyn_send_or_await(c, res);
    }
}

static JSValue dyn_app_dyn_register(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv,
    const char* method)
{
    dyn_app_t* app;
    const char* pat = NULL;
    dyn_app_route_t* nr;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "%s(pattern, handler): pattern and handler are required",
            method);
    if (!JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "%s(pattern, handler): pattern must be a string", method);
    if (!JS_IsFunction(ctx, argv[1]))
        return JS_ThrowTypeError(ctx, "%s(pattern, handler): handler must be a function", method);
    pat = JS_ToCString(ctx, argv[0]);
    if (!pat)
        return JS_EXCEPTION;
    if (dyn_app_dyn_validate(ctx, pat) < 0) {
        JS_FreeCString(ctx, pat);
        return JS_EXCEPTION;
    }
    app = (dyn_app_t*)dyn_res_native(ctx, this_val, dyn_app_class_id);
    if (!app) {
        JS_FreeCString(ctx, pat);
        return JS_EXCEPTION;
    }
    nr = (dyn_app_route_t*)realloc(app->routes,
        (app->n_routes + 1) * sizeof(*nr));
    if (!nr) {
        JS_FreeCString(ctx, pat);
        return JS_ThrowOutOfMemory(ctx);
    }
    app->routes = nr;
    nr = &app->routes[app->n_routes];
    memset(nr, 0, sizeof(*nr));
    nr->type = APP_DYN;
    nr->handler = JS_DupValue(ctx, argv[1]);
    nr->dyn_method = strdup(method);
    nr->dyn_pattern = strdup(pat);
    nr->path = strdup(pat);
    JS_FreeCString(ctx, pat);
    if (!nr->dyn_method || !nr->dyn_pattern || !nr->path) {
        free(nr->dyn_method);
        free(nr->dyn_pattern);
        free(nr->path);
        JS_FreeValue(ctx, nr->handler);
        return JS_ThrowOutOfMemory(ctx);
    }
    app->n_routes++;
    return JS_DupValue(ctx, this_val);
}

static JSValue dyn_app_get(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    return dyn_app_dyn_register(ctx, this_val, argc, argv, "GET");
}

static JSValue dyn_app_post(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    return dyn_app_dyn_register(ctx, this_val, argc, argv, "POST");
}

static JSValue dyn_app_put(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    return dyn_app_dyn_register(ctx, this_val, argc, argv, "PUT");
}

static JSValue dyn_app_patch(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    return dyn_app_dyn_register(ctx, this_val, argc, argv, "PATCH");
}

static JSValue dyn_app_del(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    return dyn_app_dyn_register(ctx, this_val, argc, argv, "DELETE");
}

static JSValue dyn_app_use(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    dyn_app_t* app;
    JSValue* nm;
    if (argc < 1 || !JS_IsFunction(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "use(fn): fn must be a function");
    app = (dyn_app_t*)dyn_res_native(ctx, this_val, dyn_app_class_id);
    if (!app)
        return JS_EXCEPTION;
    if (app->n_mw == app->cap_mw) {
        size_t nc = app->cap_mw ? app->cap_mw * 2 : 4;
        nm = (JSValue*)realloc(app->mw, nc * sizeof *nm);
        if (!nm)
            return JS_ThrowOutOfMemory(ctx);
        app->mw = nm;
        app->cap_mw = nc;
    }
    app->mw[app->n_mw++] = JS_DupValue(ctx, argv[0]);
    return JS_DupValue(ctx, this_val);
}

static JSObjectTemplate* dyn_app_request_template(JSContext* ctx)
{
    static const char* const NAMES[6] = { "method", "path", "params", "query", "headers", "body" };
    JSAtom atoms[6];
    JSObjectTemplate* tpl = NULL;
    int k, ok = 1;

    for (k = 0; k < 6; k++) {
        atoms[k] = JS_NewAtom(ctx, NAMES[k]);
        if (atoms[k] == JS_ATOM_NULL)
            ok = 0;
    }
    if (ok)
        tpl = JS_NewObjectTemplate(ctx, atoms, 6);
    for (k = 0; k < 6; k++)
        JS_FreeAtom(ctx, atoms[k]);
    return tpl;
}

static int dyn_app_try_dyn(dyn_app_conn_t* c, const char* base, size_t head_len,
    const char* path, const char* body, size_t body_len, int mount_fallback)
{
    dyn_app_t* app = c->app;
    JSContext* ctx = app->ctx;
    char method[16];
    const char* tgt;
    size_t tlen;
    long ri;
    int pat_hit = 0;
    dyn_app_dyn_it_t it;

    dyn_app_req_method(base, method, sizeof method);
    dyn_app_req_target(base, head_len, &tgt, &tlen);
    for (dyn_app_dyn_first(app, path, &it); (ri = dyn_app_dyn_next(app, &it)) >= 0;) {
        const dyn_app_route_t* rt = &app->routes[ri];
        JSValue params, ctx_obj, qv, hv, bv, mv, pv;
        const char* qs;
        size_t qn;
        int m;
        if (dyn_app_dyn_match(ctx, rt->dyn_pattern, path, JS_UNDEFINED) == 0)
            continue;
        pat_hit = 1;
        if (strcmp(rt->dyn_method, method) != 0)
            continue;
        params = JS_NewObject(ctx);
        if (JS_IsException(params))
            goto fail_oom;
        m = dyn_app_dyn_match(ctx, rt->dyn_pattern, path, params);
        if (m <= 0) {
            JS_FreeValue(ctx, params);
            goto fail_oom;
        }
        qs = (const char*)memchr(tgt, '?', tlen);
        qn = qs ? (size_t)(tgt + tlen - (qs + 1)) : 0;
        qv = qs ? dyn_app_build_query(ctx, qs + 1, qn) : JS_NewObject(ctx);
        hv = dyn_app_build_headers(ctx, app, base, head_len);
        bv = JS_NewStringLen(ctx, body, body_len);
        mv = JS_NewString(ctx, method);
        pv = JS_NewString(ctx, path);
        if (!app->req_tpl)
            app->req_tpl = dyn_app_request_template(ctx);
        ctx_obj = app->req_tpl ? JS_NewObjectFromTemplate(ctx, app->req_tpl) : JS_NewObject(ctx);
        if (JS_IsException(qv) || JS_IsException(hv) || JS_IsException(bv) || JS_IsException(mv) || JS_IsException(pv) || JS_IsException(ctx_obj)) {
            JS_FreeValue(ctx, qv);
            JS_FreeValue(ctx, hv);
            JS_FreeValue(ctx, bv);
            JS_FreeValue(ctx, mv);
            JS_FreeValue(ctx, pv);
            JS_FreeValue(ctx, ctx_obj);
            JS_FreeValue(ctx, params);
            goto fail_oom;
        }
        if (app->req_tpl) {
            JS_SetTemplateCell(ctx, ctx_obj, 0, mv);
            JS_SetTemplateCell(ctx, ctx_obj, 1, pv);
            JS_SetTemplateCell(ctx, ctx_obj, 2, params);
            JS_SetTemplateCell(ctx, ctx_obj, 3, qv);
            JS_SetTemplateCell(ctx, ctx_obj, 4, hv);
            JS_SetTemplateCell(ctx, ctx_obj, 5, bv);
        } else {
            JS_DefinePropertyValueStr(ctx, ctx_obj, "method", mv, JS_PROP_C_W_E);
            JS_DefinePropertyValueStr(ctx, ctx_obj, "path", pv, JS_PROP_C_W_E);
            JS_DefinePropertyValueStr(ctx, ctx_obj, "params", params, JS_PROP_C_W_E);
            JS_DefinePropertyValueStr(ctx, ctx_obj, "query", qv, JS_PROP_C_W_E);
            JS_DefinePropertyValueStr(ctx, ctx_obj, "headers", hv, JS_PROP_C_W_E);
            JS_DefinePropertyValueStr(ctx, ctx_obj, "body", bv, JS_PROP_C_W_E);
        }
        dyn_app_dispatch_dyn(c, rt, ctx_obj);
        return 1;
    }
    if (pat_hit && !mount_fallback) {
        char allow[256];
        size_t al = 0, a;
        for (dyn_app_dyn_first(app, path, &it); (ri = dyn_app_dyn_next(app, &it)) >= 0;) {
            const dyn_app_route_t* rt = &app->routes[ri];
            size_t ml;
            int dup = 0;
            if (dyn_app_dyn_match(ctx, rt->dyn_pattern, path, JS_UNDEFINED) == 0)
                continue;
            ml = strlen(rt->dyn_method);
            a = 0;
            while (a + 1 < al) {
                size_t e = a;
                while (e < al && allow[e] != ',')
                    e++;
                if (e - a == ml && memcmp(allow + a, rt->dyn_method, ml) == 0) {
                    dup = 1;
                    break;
                }
                a = e + 2;
            }
            if (dup || al + ml + 3 >= sizeof allow)
                continue;
            if (al) {
                allow[al++] = ',';
                allow[al++] = ' ';
            }
            memcpy(allow + al, rt->dyn_method, ml);
            al += ml;
        }
        allow[al] = '\0';
        dyn_app_send_err_allow(c, 405, "{\"error\":\"method not allowed\"}",
            allow);
        return -1;
    }
    return 0;
fail_oom:
    dyn_app_send_err(c, 500, "{\"error\":\"oom\"}");
    return -1;
}

static JSValue dyn_app_get_port(JSContext* ctx, JSValueConst this_val)
{
    dyn_app_t* app = (dyn_app_t*)dyn_res_native(ctx, this_val, dyn_app_class_id);
    if (!app)
        return JS_EXCEPTION;
    return JS_NewInt32(ctx, app->port);
}

static JSValue dyn_app_get_workers(JSContext* ctx, JSValueConst this_val)
{
    dyn_app_t* app = (dyn_app_t*)dyn_res_native(ctx, this_val, dyn_app_class_id);
    if (!app)
        return JS_EXCEPTION;
    return JS_NewInt32(ctx, app->workers ? app->workers : 1);
}

static JSValue dyn_app_get_backlog(JSContext* ctx, JSValueConst this_val)
{
    dyn_app_t* app = (dyn_app_t*)dyn_res_native(ctx, this_val, dyn_app_class_id);
    if (!app)
        return JS_EXCEPTION;
    return JS_NewInt32(ctx, app->backlog ? app->backlog : 1024);
}

static const JSCFunctionListEntry dyn_wsc_proto[] = {
    JS_CFUNC_DEF("send", 1, dyn_wsc_send),
};

static const JSClassDef dyn_wsc_class = {
    "WsClient",
    .finalizer = dyn_res_finalizer,
};

DYN_APP_CHAINED(dyn_app_rpc)

DYN_APP_CHAINED(dyn_app_static)

DYN_APP_CHAINED(dyn_app_proxy)

DYN_APP_CHAINED(dyn_app_upload)

DYN_APP_CHAINED(dyn_app_ws_register)

DYN_APP_CHAINED(dyn_app_sse_register)

static const JSCFunctionListEntry dyn_app_proto[] = {
    JS_CFUNC_DEF("rpc", 2, dyn_app_rpc_chain),
    JS_CFUNC_DEF("static", 2, dyn_app_static_chain),
    JS_CFUNC_DEF("proxy", 2, dyn_app_proxy_chain),
    JS_CFUNC_DEF("upload", 3, dyn_app_upload_chain),
    JS_CFUNC_DEF("ws", 2, dyn_app_ws_register_chain),
    JS_CFUNC_DEF("sse", 2, dyn_app_sse_register_chain),
    JS_CFUNC_DEF("get", 2, dyn_app_get),
    JS_CFUNC_DEF("post", 2, dyn_app_post),
    JS_CFUNC_DEF("put", 2, dyn_app_put),
    JS_CFUNC_DEF("patch", 2, dyn_app_patch),
    JS_CFUNC_DEF("del", 2, dyn_app_del),
    JS_CFUNC_DEF("use", 1, dyn_app_use),
    JS_CFUNC_DEF("start", 0, dyn_app_start),
    JS_CGETSET_DEF("port", dyn_app_get_port, NULL),
    JS_CGETSET_DEF("workers", dyn_app_get_workers, NULL),
    JS_CGETSET_DEF("backlog", dyn_app_get_backlog, NULL),
};

static int dyn_http_register_one(JSContext* ctx, JSModuleDef* m,
    JSClassID* pid, const JSClassDef* def,
    const JSCFunctionListEntry* proto_funcs,
    int n_funcs, JSCFunction* ctor_fn,
    const char* name)
{
    JSRuntime* rt = JS_GetRuntime(ctx);
    JSValue proto, ctor;

    JS_NewClassID(pid);
    if (JS_NewClass(rt, *pid, def) < 0) {
        proto = JS_GetClassProto(ctx, *pid);
        ctor = JS_GetPropertyStr(ctx, proto, "constructor");
        JS_FreeValue(ctx, proto);
        if (!JS_IsFunction(ctx, ctor)) {
            JS_FreeValue(ctx, ctor);
            return -1;
        }
        if (JS_SetModuleExport(ctx, m, name, ctor) < 0) {
            JS_FreeValue(ctx, ctor);
            return -1;
        }
        return 0;
    }
    proto = JS_NewObject(ctx);
    if (JS_IsException(proto))
        return -1;
    JS_SetPropertyFunctionList(ctx, proto, proto_funcs, n_funcs);
    dyn_res_class_common(ctx, *pid, proto);
    JS_SetClassProto(ctx, *pid, proto);
    ctor = JS_NewCFunction2(ctx, ctor_fn, name, 0, JS_CFUNC_constructor, 0);
    JS_SetConstructor(ctx, ctor, proto);
    return JS_SetModuleExport(ctx, m, name, ctor);
}

static int dyn_http_register_internal(JSContext* ctx, JSClassID* pid,
    const JSClassDef* def,
    const JSCFunctionListEntry* proto_funcs,
    int n_funcs)
{
    JSRuntime* rt = JS_GetRuntime(ctx);

    JS_NewClassID(pid);
    if (JS_NewClass(rt, *pid, def) < 0)
        return 0;
    {
        JSValue wp = JS_NewObject(ctx);
        if (JS_IsException(wp))
            return -1;
        JS_SetPropertyFunctionList(ctx, wp, proto_funcs, n_funcs);
        JS_SetClassProto(ctx, *pid, wp);
    }
    return 0;
}

int dyn_http_register(JSContext* ctx, JSModuleDef* m)
{
    if (dyn_httpmsg_register(ctx, m) < 0)
        return -1;
    simd_init();
    if (dyn_http_register_one(ctx, m, &dyn_http_client_class_id,
            &dyn_http_client_class, dyn_http_client_proto,
            countof(dyn_http_client_proto),
            dyn_http_client_ctor, "HTTPClient")
        < 0)
        return -1;
    {
        JSAtom hook = dyn_registered_symbol(ctx, DYN_HTTP_REQUEST_HOOK);
        JSValue proto = JS_GetClassProto(ctx, dyn_http_client_class_id);
        if (hook != JS_ATOM_NULL && JS_IsObject(proto))
            JS_DefinePropertyValue(ctx, proto, hook,
                JS_NewCFunction(ctx, dyn_http_client_request_pending, "request", 2),
                JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE);
        JS_FreeAtom(ctx, hook);
        hook = dyn_registered_symbol(ctx, DYN_HTTP_REDIRECT_HOOK);
        if (hook != JS_ATOM_NULL && JS_IsObject(proto))
            JS_DefinePropertyValue(ctx, proto, hook,
                JS_NewCFunction(ctx, dyn_http_client_redirect_plan, "redirect", 5),
                JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE);
        JS_FreeAtom(ctx, hook);
        JS_FreeValue(ctx, proto);
    }
    if (dyn_cookiejar_register(ctx, m, dyn_http_register_one) < 0)
        return -1;
    if (dyn_http_register_one(ctx, m, &dyn_http_server_class_id,
            &dyn_http_server_class, dyn_http_server_proto,
            countof(dyn_http_server_proto),
            dyn_http_server_ctor, "HTTPServer")
        < 0)
        return -1;
    if (dyn_http_register_one(ctx, m, &dyn_http_async_class_id,
            &dyn_http_async_class, dyn_http_async_proto,
            countof(dyn_http_async_proto),
            dyn_http_async_ctor, "HTTPServerAsync")
        < 0)
        return -1;
    if (dyn_http_register_one(ctx, m, &dyn_app_class_id,
            &dyn_app_class, dyn_app_proto,
            countof(dyn_app_proto),
            dyn_app_ctor, "App")
        < 0)
        return -1;
    if (dyn_http_register_one(ctx, m, &dyn_wsc_class_id,
            &dyn_wsc_class, dyn_wsc_proto,
            countof(dyn_wsc_proto),
            dyn_wsc_ctor, "WsClient")
        < 0)
        return -1;
    {
        JSRuntime* rt = JS_GetRuntime(ctx);
        JSValue sp = JS_NewObject(ctx);
        if (JS_IsException(sp))
            return -1;
        JS_NewClassID(&dyn_hc_stream_class_id);
        if (JS_NewClass(rt, dyn_hc_stream_class_id, &dyn_hc_stream_class) < 0) {
            JS_FreeValue(ctx, sp);
        } else {
            JS_SetPropertyFunctionList(ctx, sp, dyn_hc_stream_proto,
                (int)countof(dyn_hc_stream_proto));
            dyn_res_class_common(ctx, dyn_hc_stream_class_id, sp);
            JS_SetClassProto(ctx, dyn_hc_stream_class_id, sp);
        }
    }
    if (dyn_http_register_internal(ctx, &dyn_ws_class_id, &dyn_ws_class,
            dyn_ws_proto, countof(dyn_ws_proto))
        < 0)
        return -1;
    if (dyn_http_register_internal(ctx, &dyn_sse_class_id, &dyn_sse_class,
            dyn_sse_proto, countof(dyn_sse_proto))
        < 0)
        return -1;
    if (dyn_http_register_internal(ctx, &dyn_pend_class_id, &dyn_pend_class,
            NULL, 0)
        < 0)
        return -1;
    {
        JSValue g = JS_GetGlobalObject(ctx);
        size_t i;
        for (i = 0; i < DYN_HTTP_FETCH_NAMES_N; i++) {
            JSValue v = JS_GetPropertyStr(ctx, g, dyn_http_fetch_names[i]);
            if (!JS_IsUndefined(v))
                JS_SetModuleExport(ctx, m, dyn_http_fetch_names[i], v);
        }
        JS_FreeValue(ctx, g);
    }
    return 0;
}
#endif
