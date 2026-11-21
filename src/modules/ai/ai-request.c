#include "ai-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_AI) && defined(CONFIG_NATIVE_MODULE_HTTP)

static void ai_pct(dyn_sb_t* out, const char* s, size_t n)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t i;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
            || c == '-' || c == '_' || c == '.' || c == '~') {
            dyn_sb_putc(out, (char)c);
        } else {
            char e[3] = { '%', hex[c >> 4], hex[c & 15] };
            dyn_sb_put(out, e, 3);
        }
    }
}

void ai_path_seg(dyn_sb_t* out, const char* s, size_t n)
{
    ai_pct(out, s, n);
}

static int ai_query_put(JSContext* ctx, dyn_sb_t* out, int* first,
    const char* k, size_t kl, JSValueConst v)
{
    size_t vl = 0;
    const char* s;
    if (!JS_IsString(v) && !JS_IsNumber(v) && !JS_IsBool(v)) {
        JS_ThrowTypeError(ctx, "dyna:ai: a query parameter must be a string, number, boolean or an array of those");
        return -1;
    }
    s = JS_ToCStringLen(ctx, &vl, v);
    if (!s)
        return -1;
    dyn_sb_putc(out, *first ? '?' : '&');
    *first = 0;
    ai_pct(out, k, kl);
    dyn_sb_putc(out, '=');
    ai_pct(out, s, vl);
    JS_FreeCString(ctx, s);
    return 0;
}

static int ai_query(JSContext* ctx, JSValueConst q, dyn_sb_t* out)
{
    JSPropertyEnum* tab = NULL;
    uint32_t len = 0, i;
    int rc = 0, first = 1;
    if (JS_IsUndefined(q) || JS_IsNull(q))
        return 0;
    if (!JS_IsObject(q) || JS_IsArray(ctx, q)) {
        JS_ThrowTypeError(ctx, "dyna:ai: query parameters must be an object");
        return -1;
    }
    if (JS_GetOwnPropertyNames(ctx, &tab, &len, q,
            JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY)
        < 0)
        return -1;
    for (i = 0; i < len && !rc; i++) {
        JSValue v = JS_GetProperty(ctx, q, tab[i].atom);
        size_t kl = 0;
        const char* k;
        if (JS_IsException(v)) {
            rc = -1;
            break;
        }
        if (JS_IsUndefined(v) || JS_IsNull(v)) {
            JS_FreeValue(ctx, v);
            continue;
        }
        k = JS_AtomToCStringLen(ctx, &kl, tab[i].atom);
        if (!k) {
            rc = -1;
        } else if (JS_IsArray(ctx, v)) {
            int64_t n = 0, j;
            if (ai_arr_len(ctx, v, &n) < 0)
                rc = -1;
            for (j = 0; j < n && !rc; j++) {
                JSValue e = JS_GetPropertyUint32(ctx, v, (uint32_t)j);
                rc = JS_IsException(e) ? -1 : ai_query_put(ctx, out, &first, k, kl, e);
                JS_FreeValue(ctx, e);
            }
        } else {
            rc = ai_query_put(ctx, out, &first, k, kl, v);
        }
        JS_FreeCString(ctx, k);
        JS_FreeValue(ctx, v);
    }
    JS_FreePropertyEnum(ctx, tab, len);
    return rc;
}

int ai_id_path(JSContext* ctx, const char* who, const char* pre, JSValueConst id,
    const char* post, dyn_sb_t* out)
{
    const char* s;
    size_t n;
    if (!JS_IsString(id)) {
        JS_ThrowTypeError(ctx, "%s: the id must be a string", who);
        return -1;
    }
    s = JS_ToCStringLen(ctx, &n, id);
    if (!s)
        return -1;
    if (!n) {
        JS_FreeCString(ctx, s);
        JS_ThrowTypeError(ctx, "%s: the id must not be empty", who);
        return -1;
    }
    dyn_sb_puts(out, pre);
    ai_path_seg(out, s, n);
    if (post)
        dyn_sb_puts(out, post);
    dyn_sb_putc(out, 0);
    JS_FreeCString(ctx, s);
    if (out->oom) {
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    return 0;
}

static const char* const ai_req_keys[] = { "betas", "headers", "timeout", "maxRetries", "signal" };
static const char* const ai_oa_req_keys[] = { "headers", "timeout", "maxRetries", "signal" };

void ai_req_free(ai_req_t* r)
{
    if (!r)
        return;
    ai_req_drop_stream(r);
    ai_cancel_unref(r->cancel);
    if (r->rt) {
        JS_FreeValueRT(r->rt, r->signal);
        JS_FreeValueRT(r->rt, r->listener);
    }
    hc_head_free(&r->hd);
    dyn_sb_free(&r->resp);
    free(r->method);
    free(r->url);
    free(r->headers);
    free(r->body);
    ai_client_unref(r->cl);
    free(r);
}

static int ai_req_opts(JSContext* ctx, ai_req_t* r, JSValueConst opts,
    dyn_sb_t* betas, dyn_sb_t* hdrs)
{
    JSValue v;
    double d;
    int rc;
    if (JS_IsUndefined(opts) || JS_IsNull(opts))
        return 0;
    if (r->cl->kind == AI_KIND_OPENAI
            ? ai_opts_check(ctx, opts, "dyna:ai request", ai_oa_req_keys, (int)countof(ai_oa_req_keys)) < 0
            : ai_opts_check(ctx, opts, "dyna:ai request", ai_req_keys, (int)countof(ai_req_keys)) < 0)
        return -1;
    v = r->cl->kind == AI_KIND_OPENAI ? JS_UNDEFINED : JS_GetPropertyStr(ctx, opts, "betas");
    rc = JS_IsException(v) ? -1 : ai_betas_join(ctx, v, "dyna:ai request", betas);
    JS_FreeValue(ctx, v);
    if (rc < 0)
        return -1;
    v = JS_GetPropertyStr(ctx, opts, "headers");
    rc = JS_IsException(v) ? -1 : ai_hdr_block(ctx, v, "dyna:ai request", hdrs);
    JS_FreeValue(ctx, v);
    if (rc < 0)
        return -1;
    if (dyn_get(ctx, opts, "timeout", &v))
        return -1;
    if (!JS_IsUndefined(v)) {
        if (!JS_IsNumber(v) || JS_ToFloat64(ctx, &d, v) < 0 || !(d >= 1 && d <= 86400000.0)) {
            JS_FreeValue(ctx, v);
            JS_ThrowRangeError(ctx, "dyna:ai request: timeout must be a number of ms in 1..86400000");
            return -1;
        }
        r->timeout_ms = (int64_t)d;
    }
    JS_FreeValue(ctx, v);
    if (dyn_get(ctx, opts, "maxRetries", &v))
        return -1;
    if (!JS_IsUndefined(v)) {
        if (!JS_IsNumber(v) || JS_ToFloat64(ctx, &d, v) < 0 || !(d >= 0 && d <= AI_MAX_RETRIES_CAP)
            || d != (double)(int)d) {
            JS_FreeValue(ctx, v);
            JS_ThrowRangeError(ctx, "dyna:ai request: maxRetries must be an integer in 0..%d", AI_MAX_RETRIES_CAP);
            return -1;
        }
        r->max_retries = (int)d;
    }
    JS_FreeValue(ctx, v);
    v = JS_GetPropertyStr(ctx, opts, "signal");
    rc = JS_IsException(v) ? -1 : ai_req_signal(ctx, r, v);
    JS_FreeValue(ctx, v);
    if (rc < 0)
        return -1;
    return 0;
}

ai_req_t* ai_req_new(JSContext* ctx, ai_client_t* cl, const char* method,
    const char* path, JSValueConst query, JSValueConst body, JSValueConst opts,
    int out)
{
    JSValue js;
    const char* bs;
    size_t bl = 0;
    ai_req_t* r;
    if (JS_IsUndefined(body))
        return ai_req_new_raw(ctx, cl, method, path, query, NULL, 0, NULL, opts, out);
    js = JS_JSONStringify(ctx, body, JS_UNDEFINED, JS_UNDEFINED);
    if (JS_IsException(js))
        return NULL;
    bs = JS_ToCStringLen(ctx, &bl, js);
    JS_FreeValue(ctx, js);
    if (!bs)
        return NULL;
    r = ai_req_new_raw(ctx, cl, method, path, query, (const uint8_t*)bs, bl,
        "application/json", opts, out);
    JS_FreeCString(ctx, bs);
    return r;
}

ai_req_t* ai_req_new_raw(JSContext* ctx, ai_client_t* cl, const char* method,
    const char* path, JSValueConst query, const uint8_t* body, size_t body_len,
    const char* ctype, JSValueConst opts, int out)
{
    ai_req_t* r;
    dyn_sb_t url, hdr, betas, extra;
    int bad = 0, oa = cl->kind == AI_KIND_OPENAI;
    const char* who = ai_client_name(cl);

    if (!cl->api_key && !cl->auth_token) {
        if (oa)
            JS_ThrowTypeError(ctx, "OpenAI: no credentials: pass apiKey or set OPENAI_API_KEY");
        else
            JS_ThrowTypeError(ctx, "Anthropic: no credentials: pass apiKey or authToken, or set ANTHROPIC_API_KEY");
        return NULL;
    }
    r = (ai_req_t*)calloc(1, sizeof *r);
    if (!r) {
        JS_ThrowOutOfMemory(ctx);
        return NULL;
    }
    r->cl = ai_client_ref(cl);
    r->signal = JS_UNDEFINED;
    r->listener = JS_UNDEFINED;
    r->out = out;
    r->timeout_ms = cl->timeout_ms;
    r->max_retries = cl->max_retries;
    dyn_sb_init(&r->resp);
    dyn_sb_init(&url);
    dyn_sb_init(&hdr);
    dyn_sb_init(&betas);
    dyn_sb_init(&extra);
    if (cl->betas)
        dyn_sb_puts(&betas, cl->betas);
    if (ai_req_opts(ctx, r, opts, &betas, &extra) < 0)
        goto fail;
    dyn_sb_puts(&url, cl->base_url);
    dyn_sb_puts(&url, path);
    if (ai_query(ctx, query, &url) < 0)
        goto fail;
    dyn_sb_putc(&url, 0);
    if (cl->api_key && !oa)
        bad |= httpk_req_header(&hdr, "x-api-key", 9, cl->api_key, strlen(cl->api_key));
    else {
        dyn_sb_t b;
        dyn_sb_init(&b);
        dyn_sb_put(&b, "Bearer ", 7);
        dyn_sb_puts(&b, oa ? cl->api_key : cl->auth_token);
        bad |= b.oom || httpk_req_header(&hdr, "Authorization", 13, b.p, b.n);
        dyn_sb_free(&b);
    }
    if (bad) {
        JS_ThrowTypeError(ctx, "%s: the credential holds a character not allowed in a header", who);
        goto fail;
    }
    if (oa) {
        if (cl->org)
            bad |= httpk_req_header(&hdr, "OpenAI-Organization", 19, cl->org, strlen(cl->org));
        if (cl->project)
            bad |= httpk_req_header(&hdr, "OpenAI-Project", 14, cl->project, strlen(cl->project));
    } else {
        bad |= httpk_req_header(&hdr, "anthropic-version", 17, cl->version, strlen(cl->version));
    }
    if (betas.n)
        bad |= httpk_req_header(&hdr, "anthropic-beta", 14, betas.p, betas.n);
    if (out == AI_OUT_STREAM)
        dyn_sb_puts(&hdr, "Accept: text/event-stream\r\n");
    else if (out == AI_OUT_JSON)
        dyn_sb_puts(&hdr, "Accept: application/json\r\n");
    else
        dyn_sb_puts(&hdr, "Accept: */*\r\n");
    dyn_sb_puts(&hdr, "User-Agent: dynajs-ai\r\n");
    if (ctype)
        bad |= httpk_req_header(&hdr, "Content-Type", 12, ctype, strlen(ctype));
    if (cl->extra_headers)
        dyn_sb_puts(&hdr, cl->extra_headers);
    if (extra.n)
        dyn_sb_put(&hdr, extra.p, extra.n);
    dyn_sb_putc(&hdr, 0);
    if (bad || hdr.oom || url.oom) {
        JS_ThrowTypeError(ctx, "%s: invalid header value", who);
        goto fail;
    }
    if (body) {
        r->body = (uint8_t*)malloc(body_len ? body_len : 1);
        if (!r->body) {
            JS_ThrowOutOfMemory(ctx);
            goto fail;
        }
        memcpy(r->body, body, body_len);
        r->body_len = body_len;
    }
    r->method = strdup(method);
    r->url = url.p;
    r->headers = hdr.p;
    url.p = NULL;
    hdr.p = NULL;
    if (!r->method) {
        JS_ThrowOutOfMemory(ctx);
        goto fail;
    }
    if (strncmp(r->url, "https:", 6) == 0 && ai_client_tls(ctx, cl) < 0)
        goto fail;
    dyn_sb_free(&betas);
    dyn_sb_free(&extra);
    return r;
fail:
    dyn_sb_free(&url);
    dyn_sb_free(&hdr);
    dyn_sb_free(&betas);
    dyn_sb_free(&extra);
    ai_req_free(r);
    return NULL;
}

static void ai_sleep_ms(ai_req_t* r, double ms)
{
    while (ms > 20 && !ai_req_cancelled(r)) {
        ai_sleep_ms(NULL, 20);
        ms -= 20;
    }
    if (ai_req_cancelled(r))
        return;
    struct timespec ts;
    if (ms <= 0)
        return;
    ts.tv_sec = (time_t)(ms / 1000.0);
    ts.tv_nsec = (long)((ms - (double)ts.tv_sec * 1000.0) * 1e6);
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
}

static double ai_req_retry_after(const ai_req_t* r)
{
    size_t al = 0, ml = 0;
    const char* a = hc_head_get(&r->hd, "retry-after", &al);
    const char* m = hc_head_get(&r->hd, "retry-after-ms", &ml);
    return httpk_retry_after_ms(a, a ? al : 0, m, m ? ml : 0, (int64_t)time(NULL));
}

void ai_req_run(ai_req_t* r)
{
    uint64_t rng = (uint64_t)(uintptr_t)r ^ (uint64_t)time(NULL);
    for (r->attempts = 0;; r->attempts++) {
        hc_call_req_t rq;
        int rc, retry = 0;
        double delay = -1;
        memset(&rq, 0, sizeof rq);
        rq.method = r->method;
        rq.url = r->url;
        rq.headers = r->headers;
        rq.body = r->body;
        rq.body_len = r->body_len;
        rq.timeout_ms = r->timeout_ms;
        rq.max_body = r->out == AI_OUT_BYTES ? AI_MAX_FILE : AI_MAX_BODY;
#ifdef CONFIG_TLS
        rq.tls = r->cl->tls;
#endif
        ai_req_drop_stream(r);
        hc_head_free(&r->hd);
        r->resp.n = 0;
        r->status = 0;
        r->why[0] = 0;
        if (ai_req_cancelled(r)) {
            r->code = AI_E_ABORTED;
            return;
        }
        rq.on_fd = r->cancel ? ai_cancel_on_fd : NULL;
        rq.fd_ud = r->cancel;
        rc = hc_call_open(&rq, &r->hd, &r->st, r->why, sizeof r->why);
        r->code = rc;
        if (rc < 0) {
            retry = rc == HC_CALL_E_CONNECT || rc == HC_CALL_E_SEND || rc == HC_CALL_E_RECV;
        } else {
            r->status = r->hd.h.status;
            if (r->status >= 200 && r->status < 300) {
                if (r->out == AI_OUT_STREAM || r->out == AI_OUT_JSONL)
                    return;
                if (hc_call_read_all(r->st, &r->resp, r->why, sizeof r->why) < 0) {
                    r->code = HC_CALL_E_RECV;
                    retry = 1;
                } else {
                    return;
                }
            } else {
                if (hc_call_read_all(r->st, &r->resp, r->why, sizeof r->why) < 0)
                    r->resp.n = 0;
                retry = httpk_retry_status(r->status, HTTPK_RETRY_API);
                delay = ai_req_retry_after(r);
            }
        }
        if (r->code < 0 && ai_req_cancelled(r)) {
            r->code = AI_E_ABORTED;
            return;
        }
        if (!retry || r->attempts >= r->max_retries)
            return;
        if (!(delay >= 0 && delay <= AI_RETRY_AFTER_CAP_MS))
            delay = httpk_backoff_ms(AI_BACKOFF_BASE_MS, AI_BACKOFF_CAP_MS, r->attempts, &rng);
        ai_sleep_ms(r, delay);
        if (ai_req_cancelled(r)) {
            r->code = AI_E_ABORTED;
            return;
        }
    }
}

JSValue ai_req_error(JSContext* ctx, ai_req_t* r)
{
    char msg[512];
    if (r->code == AI_E_ABORTED || (r->code < 0 && ai_req_cancelled(r))) {
        ai_abort_throw(ctx);
        return JS_EXCEPTION;
    }
    if (r->code < 0) {
        snprintf(msg, sizeof msg, "%s %s failed: %s%s%s", r->method, r->url,
            hc_call_strerror(r->code), r->why[0] ? ": " : "", r->why);
        return ai_error_new(ctx, msg, 0, "connection_error", NULL, -1, JS_NULL, JS_NULL);
    }
    {
        JSValue headers = hc_head_headers(ctx, (const hc_head_t*)&r->hd), e;
        if (JS_IsException(headers)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            headers = JS_NULL;
        }
        e = ai_error_from_body(ctx, r->status, r->resp.p ? r->resp.p : "",
            r->resp.n, headers, ai_req_retry_after(r));
        JS_FreeValue(ctx, headers);
        return e;
    }
}

JSValue ai_req_result(JSContext* ctx, ai_req_t* r)
{
    JSValue v;
    if (r->code < 0 || r->status < 200 || r->status >= 300) {
        JSValue e = ai_req_error(ctx, r);
        if (JS_IsException(e))
            return e;
        return JS_Throw(ctx, e);
    }
    if (r->out == AI_OUT_BYTES) {
        if (r->resp.oom)
            return JS_ThrowOutOfMemory(ctx);
        return dyn_nat_u8_copy(ctx, (const uint8_t*)r->resp.p, r->resp.n);
    }
    if (!r->resp.n)
        return JS_NULL;
    dyn_sb_putc(&r->resp, 0);
    if (r->resp.oom)
        return JS_ThrowOutOfMemory(ctx);
    v = JS_ParseJSON(ctx, r->resp.p, r->resp.n - 1, "<api response>");
    if (JS_IsException(v)) {
        if (!dyn_take_syntax_error(ctx))
            return JS_EXCEPTION;
        return ai_error_throw(ctx, "malformed JSON in a 2xx response", r->status, "api_error");
    }
    if (r->emb_f32)
        return ai_emb_f32(ctx, v, r->status);
    return v;
}

static JSValue ai_req_entry(JSContext* ctx, void* arg)
{
    ai_req_t* r = (ai_req_t*)arg;
    JSValue v;
    dyn_fiber_offload();
    ai_req_run(r);
    dyn_fiber_onjs();
    ai_req_unlisten(ctx, r);
    v = ai_req_result(ctx, r);
    ai_req_free(r);
    return v;
}

JSValue ai_req_start(JSContext* ctx, ai_req_t* r)
{
    JSValue p = dyn_fiber_start(ctx, ai_req_entry, r);
    if (JS_IsException(p))
        ai_req_free(r);
    return p;
}
#endif
