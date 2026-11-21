#include "ai-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_AI) && defined(CONFIG_NATIVE_MODULE_HTTP)

enum {
    AI_CT_CONTENT = 0,
    AI_CT_REFUSAL,
    AI_CT_REASONING,
    AI_CT_REASONING_CONTENT,
    AI_CT_N
};

static const char* const ai_ct_keys[AI_CT_N] = { "content", "refusal", "reasoning", "reasoning_content" };

typedef struct {
    dyn_sb_t args;
} ai_chat_tool_t;

typedef struct {
    dyn_sb_t txt[AI_CT_N];
    uint8_t has[AI_CT_N];
    ai_chat_tool_t* tools;
    size_t ntools;
} ai_chat_choice_t;

typedef struct ai_chat {
    ai_chat_choice_t* ch;
    size_t nch;
} ai_chat_t;

void ai_chat_free(ai_accum_t* a)
{
    ai_chat_t* c = a->chat;
    size_t i, j;
    if (!c)
        return;
    for (i = 0; i < c->nch; i++) {
        for (j = 0; j < AI_CT_N; j++)
            dyn_sb_free(&c->ch[i].txt[j]);
        for (j = 0; j < c->ch[i].ntools; j++)
            dyn_sb_free(&c->ch[i].tools[j].args);
        free(c->ch[i].tools);
    }
    free(c->ch);
    free(c);
    a->chat = NULL;
}

static ai_chat_choice_t* ai_chat_choice(ai_accum_t* a, uint32_t idx)
{
    ai_chat_t* c = a->chat;
    if (!c) {
        c = (ai_chat_t*)calloc(1, sizeof *c);
        if (!c)
            return NULL;
        a->chat = c;
    }
    if (idx >= c->nch) {
        size_t nn = (size_t)idx + 1;
        ai_chat_choice_t* nc = (ai_chat_choice_t*)realloc(c->ch, nn * sizeof *nc);
        if (!nc)
            return NULL;
        memset(nc + c->nch, 0, (nn - c->nch) * sizeof *nc);
        c->ch = nc;
        c->nch = nn;
    }
    return &c->ch[idx];
}

static ai_chat_tool_t* ai_chat_tool(ai_chat_choice_t* ch, uint32_t idx)
{
    if (idx >= ch->ntools) {
        size_t nn = (size_t)idx + 1;
        ai_chat_tool_t* nt = (ai_chat_tool_t*)realloc(ch->tools, nn * sizeof *nt);
        if (!nt)
            return NULL;
        memset(nt + ch->ntools, 0, (nn - ch->ntools) * sizeof *nt);
        ch->tools = nt;
        ch->ntools = nn;
    }
    return &ch->tools[idx];
}

static JSValue ai_chat_error(JSContext* ctx, const char* what)
{
    char msg[256];
    snprintf(msg, sizeof msg, "malformed stream: %s", what);
    return ai_error_throw(ctx, msg, 0, "stream_error");
}

static int ai_chat_index(JSContext* ctx, JSValueConst o, uint32_t* idx)
{
    JSValue v = JS_GetPropertyStr(ctx, o, "index");
    double d;
    int rc = 1;
    if (JS_IsException(v))
        return -1;
    if (JS_IsNumber(v) && JS_ToFloat64(ctx, &d, v) == 0 && d >= 0
        && d < (double)AI_CHAT_MAX_INDEX && d == (double)(uint32_t)d) {
        *idx = (uint32_t)d;
        rc = 0;
    }
    JS_FreeValue(ctx, v);
    return rc;
}

static int ai_chat_append(JSContext* ctx, dyn_sb_t* sb, JSValueConst v)
{
    size_t l;
    const char* s = JS_ToCStringLen(ctx, &l, v);
    if (!s)
        return -1;
    dyn_sb_put(sb, s, l);
    JS_FreeCString(ctx, s);
    if (sb->oom) {
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    return 0;
}

static JSValue ai_chat_get_obj(JSContext* ctx, JSValueConst parent,
    const char* key)
{
    JSValue v = JS_GetPropertyStr(ctx, parent, key);
    if (JS_IsException(v) || JS_IsObject(v))
        return v;
    JS_FreeValue(ctx, v);
    v = JS_NewObject(ctx);
    if (JS_IsException(v))
        return v;
    if (JS_SetPropertyStr(ctx, parent, key, JS_DupValue(ctx, v)) < 0) {
        JS_FreeValue(ctx, v);
        return JS_EXCEPTION;
    }
    return v;
}

static JSValue ai_chat_get_arr(JSContext* ctx, JSValueConst parent,
    const char* key)
{
    JSValue v = JS_GetPropertyStr(ctx, parent, key);
    if (JS_IsException(v) || JS_IsArray(ctx, v))
        return v;
    JS_FreeValue(ctx, v);
    v = JS_NewArray(ctx);
    if (JS_IsException(v))
        return v;
    if (JS_SetPropertyStr(ctx, parent, key, JS_DupValue(ctx, v)) < 0) {
        JS_FreeValue(ctx, v);
        return JS_EXCEPTION;
    }
    return v;
}

static JSValue ai_chat_slot(JSContext* ctx, JSValueConst arr, uint32_t idx,
    int choice)
{
    JSValue v = JS_GetPropertyUint32(ctx, arr, idx);
    if (JS_IsException(v) || JS_IsObject(v))
        return v;
    JS_FreeValue(ctx, v);
    v = JS_NewObject(ctx);
    if (JS_IsException(v))
        return v;
    if (choice) {
        JSValue m = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, v, "index", JS_NewInt32(ctx, (int32_t)idx));
        JS_SetPropertyStr(ctx, m, "role", JS_NewString(ctx, "assistant"));
        JS_SetPropertyStr(ctx, m, "content", JS_NULL);
        JS_SetPropertyStr(ctx, m, "refusal", JS_NULL);
        JS_SetPropertyStr(ctx, v, "message", m);
        JS_SetPropertyStr(ctx, v, "finish_reason", JS_NULL);
        JS_SetPropertyStr(ctx, v, "logprobs", JS_NULL);
    } else {
        JSValue f = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, v, "id", JS_NewString(ctx, ""));
        JS_SetPropertyStr(ctx, v, "type", JS_NewString(ctx, "function"));
        JS_SetPropertyStr(ctx, f, "name", JS_NewString(ctx, ""));
        JS_SetPropertyStr(ctx, f, "arguments", JS_NewString(ctx, ""));
        JS_SetPropertyStr(ctx, v, "function", f);
    }
    if (JS_SetPropertyUint32(ctx, arr, idx, JS_DupValue(ctx, v)) < 0) {
        JS_FreeValue(ctx, v);
        return JS_EXCEPTION;
    }
    return v;
}

static int ai_chat_is_empty_str(JSContext* ctx, JSValueConst v)
{
    size_t l = 1;
    const char* s;
    if (!JS_IsString(v))
        return 1;
    s = JS_ToCStringLen(ctx, &l, v);
    JS_FreeCString(ctx, s);
    return l == 0;
}

static int ai_chat_tool_delta(JSContext* ctx, ai_chat_choice_t* ch,
    JSValueConst msg, JSValueConst tc, uint32_t pos)
{
    uint32_t ti;
    ai_chat_tool_t* t;
    JSValue arr, slot, v, f = JS_UNDEFINED, cur;
    int rc = 0;
    if (!JS_IsObject(tc))
        return 1;
    if (dyn_get(ctx, tc, "index", &v))
        return -1;
    if (JS_IsUndefined(v))
        ti = pos;
    else
        rc = ai_chat_index(ctx, tc, &ti);
    JS_FreeValue(ctx, v);
    if (rc)
        return rc;
    if (ti >= AI_CHAT_MAX_INDEX)
        return 1;
    t = ai_chat_tool(ch, ti);
    if (!t) {
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    arr = ai_chat_get_arr(ctx, msg, "tool_calls");
    if (JS_IsException(arr))
        return -1;
    slot = ai_chat_slot(ctx, arr, ti, 0);
    JS_FreeValue(ctx, arr);
    if (JS_IsException(slot))
        return -1;
    if (dyn_get(ctx, tc, "id", &v))
        goto fail;
    if (JS_IsString(v) && !ai_chat_is_empty_str(ctx, v))
        rc = JS_SetPropertyStr(ctx, slot, "id", JS_DupValue(ctx, v));
    JS_FreeValue(ctx, v);
    if (rc < 0 || dyn_get(ctx, tc, "type", &v))
        goto fail;
    if (JS_IsString(v))
        rc = JS_SetPropertyStr(ctx, slot, "type", JS_DupValue(ctx, v));
    JS_FreeValue(ctx, v);
    if (rc < 0 || dyn_get(ctx, tc, "function", &f))
        goto fail;
    if (JS_IsObject(f)) {
        JSValue sf = ai_chat_get_obj(ctx, slot, "function");
        if (JS_IsException(sf)) {
            rc = -1;
        } else {
            v = JS_GetPropertyStr(ctx, f, "name");
            cur = JS_IsException(v) ? JS_EXCEPTION : JS_GetPropertyStr(ctx, sf, "name");
            if (JS_IsException(cur))
                rc = -1;
            else if (JS_IsString(v) && ai_chat_is_empty_str(ctx, cur))
                rc = JS_SetPropertyStr(ctx, sf, "name", JS_DupValue(ctx, v));
            JS_FreeValue(ctx, cur);
            JS_FreeValue(ctx, v);
            if (rc >= 0 && dyn_get(ctx, f, "arguments", &v))
                rc = -1;
            else if (rc >= 0) {
                if (JS_IsString(v))
                    rc = ai_chat_append(ctx, &t->args, v);
                JS_FreeValue(ctx, v);
            }
            JS_FreeValue(ctx, sf);
        }
    }
    JS_FreeValue(ctx, f);
    JS_FreeValue(ctx, slot);
    return rc < 0 ? -1 : 0;
fail:
    JS_FreeValue(ctx, f);
    JS_FreeValue(ctx, slot);
    return -1;
}

static int ai_chat_logprobs(JSContext* ctx, JSValueConst choice, JSValueConst lp)
{
    static const char* const keys[] = { "content", "refusal" };
    JSValue dst;
    size_t k;
    int rc = 0;
    if (!JS_IsObject(lp))
        return 0;
    dst = ai_chat_get_obj(ctx, choice, "logprobs");
    if (JS_IsException(dst))
        return -1;
    for (k = 0; k < countof(keys) && rc >= 0; k++) {
        JSValue src = JS_GetPropertyStr(ctx, lp, keys[k]), out;
        int64_t n = 0, m = 0, i;
        if (JS_IsException(src)) {
            rc = -1;
            break;
        }
        if (!JS_IsArray(ctx, src)) {
            JS_FreeValue(ctx, src);
            continue;
        }
        out = ai_chat_get_arr(ctx, dst, keys[k]);
        if (JS_IsException(out) || ai_arr_len(ctx, src, &n) < 0 || ai_arr_len(ctx, out, &m) < 0)
            rc = -1;
        for (i = 0; i < n && rc >= 0; i++) {
            JSValue e = JS_GetPropertyUint32(ctx, src, (uint32_t)i);
            rc = JS_IsException(e) ? -1 : JS_SetPropertyInt64(ctx, out, m + i, e);
        }
        JS_FreeValue(ctx, out);
        JS_FreeValue(ctx, src);
    }
    JS_FreeValue(ctx, dst);
    return rc < 0 ? -1 : 0;
}

static int ai_chat_choice_delta(JSContext* ctx, ai_accum_t* a, JSValueConst choices,
    JSValueConst c)
{
    uint32_t idx;
    ai_chat_choice_t* ch;
    JSValue slot, msg, d, v;
    JSPropertyEnum* tab = NULL;
    uint32_t len = 0, i;
    int rc = 0;
    if (!JS_IsObject(c))
        return 1;
    rc = ai_chat_index(ctx, c, &idx);
    if (rc)
        return rc;
    ch = ai_chat_choice(a, idx);
    if (!ch) {
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    slot = ai_chat_slot(ctx, choices, idx, 1);
    if (JS_IsException(slot))
        return -1;
    msg = ai_chat_get_obj(ctx, slot, "message");
    if (JS_IsException(msg)) {
        JS_FreeValue(ctx, slot);
        return -1;
    }
    d = JS_GetPropertyStr(ctx, c, "delta");
    if (JS_IsException(d))
        rc = -1;
    else if (JS_IsObject(d)
        && JS_GetOwnPropertyNames(ctx, &tab, &len, d, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0)
        rc = -1;
    for (i = 0; i < len && rc == 0; i++) {
        const char* k = JS_AtomToCString(ctx, tab[i].atom);
        int slotk = -1, j;
        if (!k) {
            rc = -1;
            break;
        }
        v = JS_GetProperty(ctx, d, tab[i].atom);
        if (JS_IsException(v)) {
            JS_FreeCString(ctx, k);
            rc = -1;
            break;
        }
        for (j = 0; j < AI_CT_N; j++)
            if (strcmp(k, ai_ct_keys[j]) == 0)
                slotk = j;
        if (slotk >= 0) {
            if (JS_IsString(v)) {
                ch->has[slotk] = 1;
                rc = ai_chat_append(ctx, &ch->txt[slotk], v);
            }
        } else if (strcmp(k, "tool_calls") == 0) {
            if (JS_IsArray(ctx, v)) {
                int64_t n = 0, q;
                if (ai_arr_len(ctx, v, &n) < 0)
                    rc = -1;
                for (q = 0; q < n && rc == 0; q++) {
                    JSValue tc = JS_GetPropertyUint32(ctx, v, (uint32_t)q);
                    rc = JS_IsException(tc) ? -1 : ai_chat_tool_delta(ctx, ch, msg, tc, (uint32_t)q);
                    JS_FreeValue(ctx, tc);
                }
            }
        } else if (!JS_IsNull(v) && !JS_IsUndefined(v)) {
            rc = JS_SetProperty(ctx, msg, tab[i].atom, JS_DupValue(ctx, v)) < 0 ? -1 : 0;
        }
        JS_FreeValue(ctx, v);
        JS_FreeCString(ctx, k);
    }
    if (tab)
        JS_FreePropertyEnum(ctx, tab, len);
    JS_FreeValue(ctx, d);
    if (rc == 0) {
        v = JS_GetPropertyStr(ctx, c, "finish_reason");
        if (JS_IsException(v))
            rc = -1;
        else if (!JS_IsNull(v) && !JS_IsUndefined(v))
            rc = JS_SetPropertyStr(ctx, slot, "finish_reason", JS_DupValue(ctx, v)) < 0 ? -1 : 0;
        JS_FreeValue(ctx, v);
    }
    if (rc == 0) {
        v = JS_GetPropertyStr(ctx, c, "logprobs");
        rc = JS_IsException(v) ? -1 : ai_chat_logprobs(ctx, slot, v);
        JS_FreeValue(ctx, v);
    }
    JS_FreeValue(ctx, msg);
    JS_FreeValue(ctx, slot);
    return rc;
}

static int ai_chat_apply(JSContext* ctx, ai_accum_t* a, JSValueConst chunk)
{
    JSPropertyEnum* tab = NULL;
    uint32_t len = 0, i;
    JSValue choices, u;
    int64_t n = 0, q;
    int rc = 0;
    if (!a->started) {
        JSValue m = JS_NewObject(ctx);
        if (JS_IsException(m))
            return -1;
        JS_SetPropertyStr(ctx, m, "id", JS_NewString(ctx, ""));
        JS_SetPropertyStr(ctx, m, "object", JS_NewString(ctx, "chat.completion"));
        JS_SetPropertyStr(ctx, m, "created", JS_NewInt32(ctx, 0));
        JS_SetPropertyStr(ctx, m, "model", JS_NewString(ctx, ""));
        JS_SetPropertyStr(ctx, m, "choices", JS_NewArray(ctx));
        JS_SetPropertyStr(ctx, m, "usage", JS_NULL);
        JS_FreeValue(ctx, a->msg);
        a->msg = m;
        a->started = 1;
    }
    if (JS_GetOwnPropertyNames(ctx, &tab, &len, chunk, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0)
        return -1;
    for (i = 0; i < len && rc == 0; i++) {
        const char* k = JS_AtomToCString(ctx, tab[i].atom);
        if (!k) {
            rc = -1;
            break;
        }
        if (strcmp(k, "choices") && strcmp(k, "usage") && strcmp(k, "object")) {
            JSValue v = JS_GetProperty(ctx, chunk, tab[i].atom);
            rc = JS_IsException(v) || JS_SetProperty(ctx, a->msg, tab[i].atom, v) < 0 ? -1 : 0;
        }
        JS_FreeCString(ctx, k);
    }
    JS_FreePropertyEnum(ctx, tab, len);
    if (rc)
        return rc;
    if (dyn_get(ctx, chunk, "usage", &u))
        return -1;
    if (JS_IsObject(u)) {
        JSValue copy = JS_NewObject(ctx);
        JSPropertyEnum* ut = NULL;
        uint32_t ul = 0;
        if (JS_IsException(copy)
            || JS_GetOwnPropertyNames(ctx, &ut, &ul, u, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0)
            rc = -1;
        for (i = 0; i < ul && rc == 0; i++) {
            JSValue e = JS_GetProperty(ctx, u, ut[i].atom);
            rc = JS_IsException(e) || JS_SetProperty(ctx, copy, ut[i].atom, e) < 0 ? -1 : 0;
        }
        if (ut)
            JS_FreePropertyEnum(ctx, ut, ul);
        if (rc == 0)
            rc = JS_SetPropertyStr(ctx, a->msg, "usage", copy) < 0 ? -1 : 0;
        else
            JS_FreeValue(ctx, copy);
    }
    JS_FreeValue(ctx, u);
    if (rc)
        return rc;
    if (dyn_get(ctx, chunk, "choices", &choices))
        return -1;
    if (JS_IsUndefined(choices) || JS_IsNull(choices)) {
        JS_FreeValue(ctx, choices);
        return 0;
    }
    if (!JS_IsArray(ctx, choices)) {
        JS_FreeValue(ctx, choices);
        return 1;
    }
    {
        JSValue dst = ai_chat_get_arr(ctx, a->msg, "choices");
        if (JS_IsException(dst) || ai_arr_len(ctx, choices, &n) < 0)
            rc = -1;
        for (q = 0; q < n && rc == 0; q++) {
            JSValue c = JS_GetPropertyUint32(ctx, choices, (uint32_t)q);
            rc = JS_IsException(c) ? -1 : ai_chat_choice_delta(ctx, a, dst, c);
            JS_FreeValue(ctx, c);
        }
        JS_FreeValue(ctx, dst);
    }
    JS_FreeValue(ctx, choices);
    return rc;
}

static void ai_chat_drop_queue(ai_accum_t* a)
{
    size_t i;
    for (i = a->qhead; i < a->qn; i++) {
        free(a->q[i].type);
        free(a->q[i].data);
    }
    a->qhead = a->qn = 0;
}

JSValue ai_chat_next(JSContext* ctx, ai_accum_t* a)
{
    ai_rawev_t e;
    JSValue ev, er;
    int rc;
    if (a->stopped) {
        ai_chat_drop_queue(a);
        return JS_UNDEFINED;
    }
    if (a->qhead >= a->qn)
        return JS_UNDEFINED;
    e = a->q[a->qhead++];
    if (a->qhead == a->qn)
        a->qhead = a->qn = 0;
    if (e.data_len == 6 && memcmp(e.data, "[DONE]", 6) == 0) {
        free(e.type);
        free(e.data);
        a->stopped = 1;
        ai_chat_drop_queue(a);
        return JS_UNDEFINED;
    }
    ev = JS_ParseJSON(ctx, e.data, e.data_len, "<sse chunk>");
    free(e.type);
    free(e.data);
    if (JS_IsException(ev) && !dyn_take_syntax_error(ctx))
        return JS_EXCEPTION;
    if (JS_IsException(ev) || !JS_IsObject(ev) || JS_IsArray(ctx, ev)) {
        JS_FreeValue(ctx, ev);
        return ai_chat_error(ctx, "a data line is not a JSON object");
    }
    if (dyn_get(ctx, ev, "error", &er)) {
        JS_FreeValue(ctx, ev);
        return JS_EXCEPTION;
    }
    if (JS_IsObject(er)) {
        JSValue err = ai_error_from_event(ctx, ev);
        JS_FreeValue(ctx, er);
        JS_FreeValue(ctx, ev);
        if (JS_IsException(err))
            return err;
        return JS_Throw(ctx, err);
    }
    JS_FreeValue(ctx, er);
    rc = ai_chat_apply(ctx, a, ev);
    if (rc) {
        JS_FreeValue(ctx, ev);
        if (rc < 0)
            return JS_EXCEPTION;
        return ai_chat_error(ctx, "a chunk has a choice or tool call without a valid index");
    }
    return ev;
}

JSValue ai_chat_snapshot(JSContext* ctx, ai_accum_t* a)
{
    ai_chat_t* c = a->chat;
    JSValue choices;
    size_t i, j;
    int rc = 0;
    if (!JS_IsObject(a->msg))
        return JS_NULL;
    if (!c)
        return JS_DupValue(ctx, a->msg);
    if (dyn_get(ctx, a->msg, "choices", &choices))
        return JS_EXCEPTION;
    for (i = 0; i < c->nch && rc >= 0; i++) {
        ai_chat_choice_t* ch = &c->ch[i];
        JSValue slot = JS_GetPropertyUint32(ctx, choices, (uint32_t)i), msg, tcs;
        if (JS_IsException(slot)) {
            rc = -1;
            break;
        }
        if (!JS_IsObject(slot)) {
            JS_FreeValue(ctx, slot);
            continue;
        }
        if (dyn_get(ctx, slot, "message", &msg)) {
            JS_FreeValue(ctx, slot);
            rc = -1;
            break;
        }
        for (j = 0; j < AI_CT_N && rc >= 0; j++)
            if (ch->has[j])
                rc = JS_SetPropertyStr(ctx, msg, ai_ct_keys[j],
                    JS_NewStringLen(ctx, ch->txt[j].p ? ch->txt[j].p : "", ch->txt[j].n));
        tcs = rc < 0 ? JS_UNDEFINED : JS_GetPropertyStr(ctx, msg, "tool_calls");
        if (JS_IsException(tcs))
            rc = -1;
        for (j = 0; j < ch->ntools && rc >= 0 && JS_IsArray(ctx, tcs); j++) {
            JSValue t = JS_GetPropertyUint32(ctx, tcs, (uint32_t)j), f;
            if (JS_IsException(t)) {
                rc = -1;
                break;
            }
            if (JS_IsObject(t)) {
                f = JS_GetPropertyStr(ctx, t, "function");
                if (JS_IsException(f))
                    rc = -1;
                else if (JS_IsObject(f))
                    rc = JS_SetPropertyStr(ctx, f, "arguments",
                        JS_NewStringLen(ctx, ch->tools[j].args.p ? ch->tools[j].args.p : "",
                            ch->tools[j].args.n));
                JS_FreeValue(ctx, f);
            }
            JS_FreeValue(ctx, t);
        }
        JS_FreeValue(ctx, tcs);
        JS_FreeValue(ctx, msg);
        JS_FreeValue(ctx, slot);
    }
    JS_FreeValue(ctx, choices);
    if (rc < 0)
        return JS_EXCEPTION;
    return JS_DupValue(ctx, a->msg);
}

static JSValue ai_chat_reject(JSContext* ctx)
{
    return hc_promise_rejected(ctx, JS_EXCEPTION);
}

static int ai_chat_body_check(JSContext* ctx, JSValueConst body, const char* who)
{
    JSValue v;
    int ok;
    if (!JS_IsObject(body) || JS_IsArray(ctx, body) || JS_IsFunction(ctx, body)) {
        JS_ThrowTypeError(ctx, "%s: the request body must be an object", who);
        return -1;
    }
    if (dyn_get(ctx, body, "model", &v))
        return -1;
    ok = JS_IsString(v) && !ai_chat_is_empty_str(ctx, v);
    JS_FreeValue(ctx, v);
    if (!ok) {
        JS_ThrowTypeError(ctx, "%s: model is required (there is no default model)", who);
        return -1;
    }
    if (dyn_get(ctx, body, "messages", &v))
        return -1;
    ok = JS_IsArray(ctx, v);
    JS_FreeValue(ctx, v);
    if (!ok) {
        JS_ThrowTypeError(ctx, "%s: messages must be an array", who);
        return -1;
    }
    return 0;
}

static JSValue ai_chat_create(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    ai_client_t* cl = ai_ns_client(ctx, this_val);
    JSValueConst body = argc > 0 ? argv[0] : JS_UNDEFINED;
    ai_req_t* r;
    int streaming;
    if (!cl || ai_chat_body_check(ctx, body, "chat.completions.create") < 0)
        return ai_chat_reject(ctx);
    streaming = dyn_get_truthy(ctx, body, "stream");
    if (streaming < 0)
        return ai_chat_reject(ctx);
    if (streaming) {
        JS_ThrowTypeError(ctx, "chat.completions.create: stream: true is not accepted here; use chat.completions.stream(body)");
        return ai_chat_reject(ctx);
    }
    r = ai_req_new(ctx, cl, "POST", "/chat/completions", JS_UNDEFINED, body,
        argc > 1 ? argv[1] : JS_UNDEFINED, AI_OUT_JSON);
    if (!r)
        return ai_chat_reject(ctx);
    return ai_req_start(ctx, r);
}

static JSValue ai_chat_stream(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    ai_client_t* cl = ai_ns_client(ctx, this_val);
    JSValueConst body = argc > 0 ? argv[0] : JS_UNDEFINED;
    JSValue copy;
    ai_req_t* r;
    if (!cl || ai_chat_body_check(ctx, body, "chat.completions.stream") < 0)
        return JS_EXCEPTION;
    copy = ai_body_with_stream(ctx, body, 1);
    if (JS_IsException(copy))
        return copy;
    r = ai_req_new(ctx, cl, "POST", "/chat/completions", JS_UNDEFINED, copy,
        argc > 1 ? argv[1] : JS_UNDEFINED, AI_OUT_STREAM);
    JS_FreeValue(ctx, copy);
    if (!r)
        return JS_EXCEPTION;
    return ai_stream_new(ctx, r);
}

const JSCFunctionListEntry ai_completions_fns[] = {
    JS_CFUNC_DEF("create", 2, ai_chat_create),
    JS_CFUNC_DEF("stream", 2, ai_chat_stream),
};
const int ai_completions_nfns = (int)countof(ai_completions_fns);

static JSValue ai_oa_models_list(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    ai_client_t* cl = ai_ns_client(ctx, this_val);
    ai_req_t* r;
    if (!cl)
        return ai_chat_reject(ctx);
    r = ai_req_new(ctx, cl, "GET", "/models", argc > 0 ? argv[0] : JS_UNDEFINED,
        JS_UNDEFINED, argc > 1 ? argv[1] : JS_UNDEFINED, AI_OUT_JSON);
    if (!r)
        return ai_chat_reject(ctx);
    return ai_req_start(ctx, r);
}

static JSValue ai_oa_models_retrieve(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    ai_client_t* cl = ai_ns_client(ctx, this_val);
    ai_req_t* r;
    dyn_sb_t path;
    if (!cl)
        return ai_chat_reject(ctx);
    dyn_sb_init(&path);
    if (ai_id_path(ctx, "models.retrieve", "/models/", argc > 0 ? argv[0] : JS_UNDEFINED,
            NULL, &path)
        < 0) {
        dyn_sb_free(&path);
        return ai_chat_reject(ctx);
    }
    r = ai_req_new(ctx, cl, "GET", path.p, JS_UNDEFINED, JS_UNDEFINED,
        argc > 1 ? argv[1] : JS_UNDEFINED, AI_OUT_JSON);
    dyn_sb_free(&path);
    if (!r)
        return ai_chat_reject(ctx);
    return ai_req_start(ctx, r);
}

const JSCFunctionListEntry ai_oa_models_fns[] = {
    JS_CFUNC_DEF("list", 2, ai_oa_models_list),
    JS_CFUNC_DEF("retrieve", 2, ai_oa_models_retrieve),
};
const int ai_oa_models_nfns = (int)countof(ai_oa_models_fns);
#endif
