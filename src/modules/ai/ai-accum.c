#include "ai-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_AI) && defined(CONFIG_NATIVE_MODULE_HTTP)

static int ai_accum_on_event(void* ud, const httpk_sse_event_t* ev)
{
    ai_accum_t* a = (ai_accum_t*)ud;
    ai_rawev_t* e;
    if (a->qn == a->qcap) {
        size_t nc = a->qcap ? a->qcap * 2 : 16;
        ai_rawev_t* nq;
        if (a->qhead) {
            memmove(a->q, a->q + a->qhead, (a->qn - a->qhead) * sizeof *a->q);
            a->qn -= a->qhead;
            a->qhead = 0;
        }
        if (a->qn == a->qcap) {
            nq = (ai_rawev_t*)realloc(a->q, nc * sizeof *nq);
            if (!nq) {
                a->oom = 1;
                return 1;
            }
            a->q = nq;
            a->qcap = nc;
        }
    }
    e = &a->q[a->qn];
    e->type = (char*)malloc(ev->type_len + 1);
    e->data = (char*)malloc(ev->data_len + 1);
    if (!e->type || !e->data) {
        free(e->type);
        free(e->data);
        a->oom = 1;
        return 1;
    }
    memcpy(e->type, ev->type, ev->type_len);
    e->type[ev->type_len] = 0;
    e->type_len = ev->type_len;
    memcpy(e->data, ev->data, ev->data_len);
    e->data[ev->data_len] = 0;
    e->data_len = ev->data_len;
    a->qn++;
    return 0;
}

void ai_accum_init(ai_accum_t* a)
{
    memset(a, 0, sizeof *a);
    a->msg = JS_UNDEFINED;
    httpk_sse_init(&a->sse, ai_accum_on_event, a, AI_MAX_EVENT);
}

void ai_accum_init_kind(ai_accum_t* a, int kind)
{
    ai_accum_init(a);
    a->kind = kind;
}

void ai_accum_free(JSRuntime* rt, ai_accum_t* a)
{
    size_t i;
    for (i = a->qhead; i < a->qn; i++) {
        free(a->q[i].type);
        free(a->q[i].data);
    }
    free(a->q);
    for (i = 0; i < a->nbb; i++)
        dyn_sb_free(&a->bb[i].buf);
    free(a->bb);
    httpk_sse_free(&a->sse);
    ai_chat_free(a);
    JS_FreeValueRT(rt, a->msg);
    a->msg = JS_UNDEFINED;
    a->q = NULL;
    a->bb = NULL;
    a->qn = a->qcap = a->qhead = a->nbb = 0;
}

int ai_accum_feed(ai_accum_t* a, const char* p, size_t n)
{
    int rc = httpk_sse_feed(&a->sse, p, n);
    if (rc == HTTPK_SSE_E_TOO_BIG)
        a->too_big = 1;
    if (rc == HTTPK_SSE_E_OOM)
        a->oom = 1;
    return rc;
}

int ai_accum_pending(const ai_accum_t* a)
{
    return a->qn > a->qhead;
}

static ai_blockbuf_t* ai_accum_bb(ai_accum_t* a, uint32_t idx)
{
    if (idx >= a->nbb) {
        size_t nn = (size_t)idx + 1, i;
        ai_blockbuf_t* nb;
        if (nn > 1u << 20)
            return NULL;
        nb = (ai_blockbuf_t*)realloc(a->bb, nn * sizeof *nb);
        if (!nb)
            return NULL;
        for (i = a->nbb; i < nn; i++) {
            dyn_sb_init(&nb[i].buf);
            nb[i].field = AI_FIELD_NONE;
            nb[i].live = 0;
        }
        a->bb = nb;
        a->nbb = nn;
    }
    return &a->bb[idx];
}

static const char* ai_field_name(int f)
{
    return f == AI_FIELD_TEXT ? "text" : f == AI_FIELD_THINKING ? "thinking"
                                                                : NULL;
}

static JSValue ai_content(JSContext* ctx, ai_accum_t* a)
{
    JSValue c = JS_GetPropertyStr(ctx, a->msg, "content");
    if (JS_IsException(c))
        return c;
    if (!JS_IsArray(ctx, c)) {
        JS_FreeValue(ctx, c);
        c = JS_NewArray(ctx);
        if (JS_IsException(c))
            return c;
        JS_SetPropertyStr(ctx, a->msg, "content", JS_DupValue(ctx, c));
    }
    return c;
}

static JSValue ai_block(JSContext* ctx, ai_accum_t* a, uint32_t idx)
{
    JSValue c = ai_content(ctx, a), b;
    if (JS_IsException(c))
        return c;
    b = JS_GetPropertyUint32(ctx, c, idx);
    JS_FreeValue(ctx, c);
    return b;
}

static int ai_flush_text(JSContext* ctx, ai_accum_t* a, uint32_t idx)
{
    ai_blockbuf_t* bb = idx < a->nbb ? &a->bb[idx] : NULL;
    const char* name;
    JSValue b;
    int rc = 0;
    if (!bb || !bb->live)
        return 0;
    name = ai_field_name(bb->field);
    if (!name)
        return 0;
    b = ai_block(ctx, a, idx);
    if (JS_IsObject(b))
        rc = JS_SetPropertyStr(ctx, b, name,
            JS_NewStringLen(ctx, bb->buf.p ? bb->buf.p : "", bb->buf.n));
    JS_FreeValue(ctx, b);
    return rc < 0 ? -1 : 0;
}

static int ai_set_new(JSContext* ctx, JSValueConst o, const char* k, JSValue v)
{
    if (JS_IsException(v))
        return -1;
    return JS_SetPropertyStr(ctx, o, k, v) < 0 ? -1 : 0;
}

static int ai_finish_json(JSContext* ctx, ai_accum_t* a, uint32_t idx)
{
    ai_blockbuf_t* bb = &a->bb[idx];
    JSValue b, v;
    int rc = 0;
    if (!bb->buf.n)
        return 0;
    b = ai_block(ctx, a, idx);
    if (!JS_IsObject(b)) {
        JS_FreeValue(ctx, b);
        return 0;
    }
    dyn_sb_putc(&bb->buf, 0);
    if (bb->buf.oom) {
        JS_FreeValue(ctx, b);
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    bb->buf.n--;
    v = JS_ParseJSON(ctx, bb->buf.p, bb->buf.n, "<tool input>");
    if (JS_IsException(v) || !JS_IsObject(v) || JS_IsArray(ctx, v)) {
        JSValue info, exc;
        const char* why = NULL;
        if (JS_IsException(v)) {
            if (JS_HasUncatchableException(ctx))
                goto fail;
            exc = JS_GetException(ctx);
            if (!JS_IsSyntaxError(ctx, exc)) {
                JS_Throw(ctx, exc);
                goto fail;
            }
            why = JS_ToCString(ctx, exc);
            JS_FreeValue(ctx, exc);
            if (!why) {
                if (JS_HasUncatchableException(ctx))
                    goto fail;
                JS_FreeValue(ctx, JS_GetException(ctx));
            }
        } else {
            JS_FreeValue(ctx, v);
        }
        info = JS_NewObject(ctx);
        if (JS_IsException(info)
            || ai_set_new(ctx, info, "raw", JS_NewStringLen(ctx, bb->buf.p, bb->buf.n))
            || ai_set_new(ctx, info, "message", JS_NewString(ctx, why ? why : "tool input is not a JSON object"))) {
            JS_FreeCString(ctx, why);
            JS_FreeValue(ctx, info);
            goto fail;
        }
        JS_FreeCString(ctx, why);
        if (JS_DefinePropertyValueStr(ctx, b, "inputError", info, JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE) < 0)
            goto fail;
        v = JS_NewObject(ctx);
        if (JS_IsException(v))
            goto fail;
    }
    rc = JS_SetPropertyStr(ctx, b, "input", v);
    JS_FreeValue(ctx, b);
    return rc < 0 ? -1 : 0;
fail:
    JS_FreeValue(ctx, b);
    return -1;
}

static int ai_get_index(JSContext* ctx, JSValueConst ev, uint32_t* idx)
{
    JSValue v = JS_GetPropertyStr(ctx, ev, "index");
    double d;
    int rc = 1;
    if (JS_IsException(v))
        return -1;
    if (JS_IsNumber(v) && JS_ToFloat64(ctx, &d, v) == 0 && d >= 0 && d < (double)(1u << 20)
        && d == (double)(uint32_t)d) {
        *idx = (uint32_t)d;
        rc = 0;
    }
    JS_FreeValue(ctx, v);
    return rc;
}

static int ai_str_eq(JSContext* ctx, JSValueConst v, const char* lit)
{
    const char* s;
    int eq;
    if (!JS_IsString(v))
        return 0;
    s = JS_ToCString(ctx, v);
    eq = s && strcmp(s, lit) == 0;
    JS_FreeCString(ctx, s);
    return eq;
}

static int ai_merge(JSContext* ctx, JSValueConst dst, JSValueConst src)
{
    JSPropertyEnum* tab = NULL;
    uint32_t len = 0, i;
    if (!JS_IsObject(src) || !JS_IsObject(dst))
        return 0;
    if (JS_GetOwnPropertyNames(ctx, &tab, &len, src,
            JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY)
        < 0)
        return -1;
    for (i = 0; i < len; i++) {
        JSValue v = JS_GetProperty(ctx, src, tab[i].atom);
        if (JS_IsException(v) || JS_SetProperty(ctx, dst, tab[i].atom, v) < 0) {
            JS_FreePropertyEnum(ctx, tab, len);
            return -1;
        }
    }
    JS_FreePropertyEnum(ctx, tab, len);
    return 0;
}

static JSValue ai_stream_error(JSContext* ctx, const char* what, const char* type)
{
    char msg[256];
    snprintf(msg, sizeof msg, "malformed stream: %s", what);
    return ai_error_throw(ctx, msg, 0, type ? type : "stream_error");
}

static int ai_apply_start_block(JSContext* ctx, ai_accum_t* a, JSValueConst ev)
{
    uint32_t idx;
    JSValue blk, c, t;
    ai_blockbuf_t* bb;
    int r = ai_get_index(ctx, ev, &idx);
    if (r)
        return r;
    if (dyn_get(ctx, ev, "content_block", &blk))
        return -1;
    if (!JS_IsObject(blk)) {
        JS_FreeValue(ctx, blk);
        return 1;
    }
    bb = ai_accum_bb(a, idx);
    if (!bb) {
        JS_FreeValue(ctx, blk);
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    bb->buf.n = 0;
    bb->live = 1;
    bb->field = AI_FIELD_NONE;
    t = JS_GetPropertyStr(ctx, blk, "type");
    if (JS_IsException(t)) {
        JS_FreeValue(ctx, blk);
        return -1;
    }
    if (ai_str_eq(ctx, t, "text"))
        bb->field = AI_FIELD_TEXT;
    else if (ai_str_eq(ctx, t, "thinking"))
        bb->field = AI_FIELD_THINKING;
    else if (ai_str_eq(ctx, t, "tool_use") || ai_str_eq(ctx, t, "server_tool_use"))
        bb->field = AI_FIELD_JSON;
    JS_FreeValue(ctx, t);
    {
        const char* name = ai_field_name(bb->field);
        if (name) {
            JSValue init = JS_GetPropertyStr(ctx, blk, name);
            if (JS_IsException(init)) {
                JS_FreeValue(ctx, blk);
                return -1;
            }
            if (JS_IsString(init)) {
                size_t l;
                const char* s = JS_ToCStringLen(ctx, &l, init);
                if (s)
                    dyn_sb_put(&bb->buf, s, l);
                JS_FreeCString(ctx, s);
            }
            JS_FreeValue(ctx, init);
        }
    }
    c = ai_content(ctx, a);
    if (JS_IsException(c)) {
        JS_FreeValue(ctx, blk);
        return -1;
    }
    {
        JSValue copy = JS_NewObject(ctx);
        int rc = ai_merge(ctx, copy, blk);
        JS_FreeValue(ctx, blk);
        if (rc < 0 || JS_SetPropertyUint32(ctx, c, idx, copy) < 0) {
            if (rc < 0)
                JS_FreeValue(ctx, copy);
            JS_FreeValue(ctx, c);
            return -1;
        }
    }
    JS_FreeValue(ctx, c);
    return 0;
}

static int ai_append_str(JSContext* ctx, ai_blockbuf_t* bb, JSValueConst d,
    const char* key)
{
    JSValue v = JS_GetPropertyStr(ctx, d, key);
    size_t l;
    const char* s;
    if (JS_IsException(v))
        return -1;
    if (!JS_IsString(v)) {
        JS_FreeValue(ctx, v);
        return 1;
    }
    s = JS_ToCStringLen(ctx, &l, v);
    JS_FreeValue(ctx, v);
    if (!s)
        return -1;
    dyn_sb_put(&bb->buf, s, l);
    JS_FreeCString(ctx, s);
    if (bb->buf.oom) {
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    return 0;
}

static int ai_apply_delta(JSContext* ctx, ai_accum_t* a, JSValueConst ev)
{
    uint32_t idx;
    JSValue d, t;
    ai_blockbuf_t* bb;
    int rc = ai_get_index(ctx, ev, &idx);
    if (rc)
        return rc;
    if (idx >= a->nbb || !a->bb[idx].live)
        return 0;
    bb = &a->bb[idx];
    if (dyn_get(ctx, ev, "delta", &d))
        return -1;
    if (!JS_IsObject(d)) {
        JS_FreeValue(ctx, d);
        return 1;
    }
    t = JS_GetPropertyStr(ctx, d, "type");
    if (JS_IsException(t)) {
        JS_FreeValue(ctx, d);
        return -1;
    }
    if (ai_str_eq(ctx, t, "text_delta") && bb->field == AI_FIELD_TEXT)
        rc = ai_append_str(ctx, bb, d, "text");
    else if (ai_str_eq(ctx, t, "thinking_delta") && bb->field == AI_FIELD_THINKING)
        rc = ai_append_str(ctx, bb, d, "thinking");
    else if (ai_str_eq(ctx, t, "input_json_delta") && bb->field == AI_FIELD_JSON)
        rc = ai_append_str(ctx, bb, d, "partial_json");
    else if (ai_str_eq(ctx, t, "signature_delta")) {
        JSValue b = ai_block(ctx, a, idx);
        if (JS_IsObject(b)) {
            JSValue sv = JS_GetPropertyStr(ctx, d, "signature");
            rc = JS_IsException(sv) || JS_SetPropertyStr(ctx, b, "signature", sv) < 0 ? -1 : 0;
        }
        JS_FreeValue(ctx, b);
    } else if (ai_str_eq(ctx, t, "citations_delta")) {
        JSValue b = ai_block(ctx, a, idx), arr;
        if (JS_IsObject(b)) {
            arr = JS_GetPropertyStr(ctx, b, "citations");
            if (JS_IsException(arr)) {
                rc = -1;
            } else {
                if (!JS_IsArray(ctx, arr)) {
                    JS_FreeValue(ctx, arr);
                    arr = JS_NewArray(ctx);
                    JS_SetPropertyStr(ctx, b, "citations", JS_DupValue(ctx, arr));
                }
                {
                    int64_t n = 0;
                    if (ai_arr_len(ctx, arr, &n) == 0) {
                        JSValue cv = JS_GetPropertyStr(ctx, d, "citation");
                        rc = JS_IsException(cv) || JS_SetPropertyInt64(ctx, arr, n, cv) < 0 ? -1 : 0;
                    }
                }
                JS_FreeValue(ctx, arr);
            }
        }
        JS_FreeValue(ctx, b);
    }
    JS_FreeValue(ctx, t);
    JS_FreeValue(ctx, d);
    return rc < 0 ? -1 : 0;
}

static int ai_apply_stop_block(JSContext* ctx, ai_accum_t* a, JSValueConst ev)
{
    uint32_t idx;
    int rc = ai_get_index(ctx, ev, &idx);
    if (rc)
        return rc;
    if (idx >= a->nbb || !a->bb[idx].live)
        return 0;
    if (a->bb[idx].field == AI_FIELD_JSON)
        rc = ai_finish_json(ctx, a, idx);
    else
        rc = ai_flush_text(ctx, a, idx);
    a->bb[idx].live = 0;
    dyn_sb_free(&a->bb[idx].buf);
    return rc;
}

static int ai_apply_message_delta(JSContext* ctx, ai_accum_t* a, JSValueConst ev)
{
    JSValue d = JS_GetPropertyStr(ctx, ev, "delta"), u, mu;
    int rc;
    if (JS_IsException(d))
        return -1;
    rc = ai_merge(ctx, a->msg, d);
    JS_FreeValue(ctx, d);
    if (rc < 0 || dyn_get(ctx, ev, "usage", &u))
        return -1;
    if (JS_IsObject(u)) {
        mu = JS_GetPropertyStr(ctx, a->msg, "usage");
        if (JS_IsException(mu)) {
            JS_FreeValue(ctx, u);
            return -1;
        }
        if (!JS_IsObject(mu)) {
            JS_FreeValue(ctx, mu);
            mu = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, a->msg, "usage", JS_DupValue(ctx, mu));
        }
        rc = ai_merge(ctx, mu, u);
        JS_FreeValue(ctx, mu);
    }
    JS_FreeValue(ctx, u);
    return rc;
}

JSValue ai_accum_next(JSContext* ctx, ai_accum_t* a)
{
    ai_rawev_t e;
    JSValue ev, tv;
    const char* type = NULL;
    int rc = 0;

    if (a->oom)
        return JS_ThrowOutOfMemory(ctx);
    if (a->too_big)
        return ai_stream_error(ctx, "an event exceeds 64 MiB", NULL);
    if (a->kind == AI_KIND_OPENAI)
        return ai_chat_next(ctx, a);
    if (a->qhead >= a->qn)
        return JS_UNDEFINED;
    e = a->q[a->qhead++];
    if (a->qhead == a->qn)
        a->qhead = a->qn = 0;
    ev = JS_ParseJSON(ctx, e.data, e.data_len, "<sse event>");
    if (JS_IsException(ev) && !dyn_take_syntax_error(ctx)) {
        free(e.type);
        free(e.data);
        return JS_EXCEPTION;
    }
    if (JS_IsException(ev) || !JS_IsObject(ev)) {
        char what[160];
        JS_FreeValue(ctx, ev);
        snprintf(what, sizeof what, "event \"%.60s\" carries data that is not a JSON object", e.type);
        free(e.type);
        free(e.data);
        return ai_stream_error(ctx, what, NULL);
    }
    tv = JS_GetPropertyStr(ctx, ev, "type");
    if (JS_IsException(tv)) {
        free(e.type);
        free(e.data);
        JS_FreeValue(ctx, ev);
        return JS_EXCEPTION;
    }
    if (JS_IsString(tv))
        type = JS_ToCString(ctx, tv);
    JS_FreeValue(ctx, tv);
    if (!type) {
        tv = JS_NewString(ctx, e.type);
        JS_SetPropertyStr(ctx, ev, "type", tv);
        type = JS_ToCString(ctx, tv);
    }
    free(e.type);
    free(e.data);
    if (!type) {
        JS_FreeValue(ctx, ev);
        return JS_EXCEPTION;
    }
    if (strcmp(type, "error") == 0) {
        JSValue err = ai_error_from_event(ctx, ev);
        JS_FreeCString(ctx, type);
        JS_FreeValue(ctx, ev);
        if (JS_IsException(err))
            return err;
        return JS_Throw(ctx, err);
    }
    if (strcmp(type, "message_start") == 0) {
        JSValue m = JS_GetPropertyStr(ctx, ev, "message");
        if (JS_IsException(m)) {
            rc = -1;
        } else if (!JS_IsObject(m) || a->started) {
            JS_FreeValue(ctx, m);
            rc = 1;
        } else {
            JSValue copy = JS_NewObject(ctx);
            rc = ai_merge(ctx, copy, m);
            JS_FreeValue(ctx, m);
            JS_FreeValue(ctx, a->msg);
            a->msg = copy;
            a->started = 1;
        }
    } else if (!a->started && strcmp(type, "ping") != 0) {
        rc = 1;
    } else if (strcmp(type, "content_block_start") == 0) {
        rc = ai_apply_start_block(ctx, a, ev);
    } else if (strcmp(type, "content_block_delta") == 0) {
        rc = ai_apply_delta(ctx, a, ev);
    } else if (strcmp(type, "content_block_stop") == 0) {
        rc = ai_apply_stop_block(ctx, a, ev);
    } else if (strcmp(type, "message_delta") == 0) {
        rc = ai_apply_message_delta(ctx, a, ev);
    } else if (strcmp(type, "message_stop") == 0) {
        a->stopped = 1;
    }
    if (rc) {
        char what[160];
        snprintf(what, sizeof what, "\"%.60s\" event %s", type,
            a->started ? "is missing a required field" : "before message_start");
        JS_FreeCString(ctx, type);
        JS_FreeValue(ctx, ev);
        if (rc < 0)
            return JS_EXCEPTION;
        return ai_stream_error(ctx, what, NULL);
    }
    JS_FreeCString(ctx, type);
    return ev;
}

JSValue ai_accum_snapshot(JSContext* ctx, ai_accum_t* a)
{
    size_t i;
    if (a->kind == AI_KIND_OPENAI)
        return ai_chat_snapshot(ctx, a);
    if (!JS_IsObject(a->msg))
        return JS_NULL;
    for (i = 0; i < a->nbb; i++)
        if (ai_flush_text(ctx, a, (uint32_t)i) < 0)
            return JS_EXCEPTION;
    return JS_DupValue(ctx, a->msg);
}
#endif
