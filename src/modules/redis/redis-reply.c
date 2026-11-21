#include "redis-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_REDIS)
#define RD_INT_NONE 0
#define RD_INT_AUTH 2
#define RD_INT_SELECT 3

static JSValue redis_error_value(JSContext* ctx, const char* text, size_t len)
{
    JSValue e = JS_NewError(ctx);
    size_t i = 0;
    while (i < len && text[i] != ' ')
        i++;
    JS_DefinePropertyValueStr(ctx, e, "message", JS_NewStringLen(ctx, text, len),
        JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, e, "code", JS_NewStringLen(ctx, text, i),
        JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, e, "redis", JS_TRUE, JS_PROP_C_W_E);
    return e;
}

static JSValue redis_bytes(JSContext* ctx, const uint8_t* p, size_t n)
{
    JSValue ab = JS_NewArrayBufferCopy(ctx, p, n), ta;
    JSValueConst a3[3];
    if (JS_IsException(ab))
        return ab;
    a3[0] = ab;
    a3[1] = JS_NewInt32(ctx, 0);
    a3[2] = JS_NewInt32(ctx, (int)n);
    ta = JS_NewTypedArray(ctx, 3, a3, JS_TYPED_ARRAY_UINT8);
    JS_FreeValue(ctx, ab);
    return ta;
}

static JSValue redis_bignum(JSContext* ctx, const char* p, size_t n)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue ctor = JS_GetPropertyStr(ctx, global, "BigInt");
    JSValue arg = JS_NewStringLen(ctx, p, n), out;
    JSValueConst a1[1];

    JS_FreeValue(ctx, global);
    if (JS_IsException(ctor)) {
        if (JS_HasUncatchableException(ctx)) {
            JS_FreeValue(ctx, arg);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, JS_GetException(ctx));
        ctor = JS_UNDEFINED;
    }
    if (JS_IsException(arg)) {
        JS_FreeValue(ctx, ctor);
        return arg;
    }
    if (!JS_IsFunction(ctx, ctor)) {
        JS_FreeValue(ctx, ctor);
        return arg;
    }
    a1[0] = arg;
    out = JS_Call(ctx, ctor, JS_UNDEFINED, 1, a1);
    JS_FreeValue(ctx, ctor);
    if (JS_IsException(out)) {
        if (JS_HasUncatchableException(ctx)) {
            JS_FreeValue(ctx, arg);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, JS_GetException(ctx));
        return arg;
    }
    JS_FreeValue(ctx, arg);
    return out;
}

static JSValue redis_value(JSContext* ctx, dyn_redis_t* r,
    dyn_resp_reader_t* rd, int depth, int* is_err)
{
    dyn_resp_item_t it;
    int rc;

    if (depth >= DYN_RESP_MAX_DEPTH)
        return JS_ThrowInternalError(ctx, "Redis: %s",
            dyn_resp_strerror(DYN_RESP_E_DEPTH));
    rc = dyn_resp_next(rd, &it);

    if (rc != DYN_RESP_OK)
        return JS_ThrowInternalError(ctx, "Redis: %s", dyn_resp_strerror(rc));

    switch (it.type) {
    case DYN_RESP_SIMPLE:
        return JS_NewStringLen(ctx, (const char*)it.str, it.slen);
    case DYN_RESP_ERROR:
    case DYN_RESP_BLOBERR:
        if (is_err)
            *is_err = 1;
        return redis_error_value(ctx, (const char*)it.str, it.slen);
    case DYN_RESP_INT:
        if (r->bigint)
            return JS_NewBigInt64(ctx, it.ival);
        if (it.ival > 9007199254740992LL || it.ival < -9007199254740992LL)
            return JS_NewStringLen(ctx, (const char*)it.str, it.slen);
        return JS_NewInt64(ctx, it.ival);
    case DYN_RESP_NULL:
        return JS_NULL;
    case DYN_RESP_BOOL:
        return JS_NewBool(ctx, (int)it.ival);
    case DYN_RESP_DOUBLE:
        return JS_NewFloat64(ctx, it.dval);
    case DYN_RESP_BIGNUM:
        if (r->bigint)
            return redis_bignum(ctx, (const char*)it.str, it.slen);
        return JS_NewStringLen(ctx, (const char*)it.str, it.slen);
    case DYN_RESP_BULK:
    case DYN_RESP_VERB: {
        const uint8_t* p = it.str;
        size_t n = it.slen;
        if (it.isnull)
            return JS_NULL;
        if (it.type == DYN_RESP_VERB && n >= 4 && p[3] == ':') {
            p += 4;
            n -= 4;
        }
        if (r->binary)
            return redis_bytes(ctx, p, n);
        return JS_NewStringLen(ctx, (const char*)p, n);
    }
    case DYN_RESP_ARRAY:
    case DYN_RESP_SET:
    case DYN_RESP_PUSH: {
        JSValue arr;
        int64_t i;
        if (it.isnull)
            return JS_NULL;
        arr = JS_NewArray(ctx);
        if (JS_IsException(arr))
            return arr;
        for (i = 0; i < it.count; i++) {
            JSValue v = redis_value(ctx, r, rd, depth + 1, is_err);
            if (JS_IsException(v)) {
                JS_FreeValue(ctx, arr);
                return v;
            }
            JS_DefinePropertyValueUint32(ctx, arr, (uint32_t)i, v,
                JS_PROP_C_W_E);
        }
        return arr;
    }
    case DYN_RESP_ATTR: {
        int64_t i;
        for (i = 0; i < it.count * 2; i++) {
            JSValue skip = redis_value(ctx, r, rd, depth + 1, is_err);
            if (JS_IsException(skip))
                return skip;
            JS_FreeValue(ctx, skip);
        }
        return redis_value(ctx, r, rd, depth + 1, is_err);
    }
    case DYN_RESP_MAP: {
        JSValue obj;
        int64_t i;
        obj = JS_NewObject(ctx);
        if (JS_IsException(obj))
            return obj;
        for (i = 0; i < it.count; i++) {
            JSValue k, v, kstr;
            const char* ks;
            int saved = r->binary;
            r->binary = 0;
            k = redis_value(ctx, r, rd, depth + 1, is_err);
            r->binary = saved;
            if (JS_IsException(k)) {
                JS_FreeValue(ctx, obj);
                return k;
            }
            v = redis_value(ctx, r, rd, depth + 1, is_err);
            if (JS_IsException(v)) {
                JS_FreeValue(ctx, k);
                JS_FreeValue(ctx, obj);
                return v;
            }
            kstr = JS_ToString(ctx, k);
            JS_FreeValue(ctx, k);
            ks = JS_ToCString(ctx, kstr);
            JS_FreeValue(ctx, kstr);
            if (!ks) {
                JS_FreeValue(ctx, v);
                JS_FreeValue(ctx, obj);
                return JS_EXCEPTION;
            }
            JS_DefinePropertyValueStr(ctx, obj, ks, v, JS_PROP_C_W_E);
            JS_FreeCString(ctx, ks);
        }
        return obj;
    }
    default:
        return JS_ThrowInternalError(ctx, "Redis: unexpected type '%c'",
            it.type);
    }
}

static void redis_release_waitq(dyn_redis_t* r)
{
    dyn_redis_pending_t* p;
    while ((p = pend_pop(&r->wq_head, &r->wq_tail)) != NULL) {
        r->nwait--;
        if (redis_arm(r, p) < 0) {
            pend_push(&r->wq_head, &r->wq_tail, p);
            r->nwait++;
            redis_fail_all(r, "Redis: out of memory");
            return;
        }
    }
    if (redis_flush(r) < 0)
        redis_fail_all(r, "Redis: cannot write to the socket");
}

static void redis_after_handshake(dyn_redis_t* r)
{
    r->state = RD_ST_READY;
    r->connect_deadline_ms = 0;
    if (r->pass) {
        redis_secure_zero(r->pass, strlen(r->pass));
        free(r->pass);
        r->pass = NULL;
    }
    redis_release_waitq(r);
}

static int redis_send_select(dyn_redis_t* r)
{
    char dbs[16];
    const char* argv[2];
    int n = snprintf(dbs, sizeof(dbs), "%d", r->db);
    if (n < 0 || (size_t)n >= sizeof(dbs))
        return -1;
    argv[0] = "SELECT";
    argv[1] = dbs;
    return redis_send_internal(r, RD_INT_SELECT, 2, argv, NULL);
}

static void redis_handle_hello(dyn_redis_t* r, int is_err)
{
    if (is_err) {
        r->proto = 2;
        if (r->pass) {
            const char* argv[3];
            int argc = 0;
            argv[argc++] = "AUTH";
            if (r->user)
                argv[argc++] = r->user;
            argv[argc++] = r->pass;
            if (redis_send_internal(r, RD_INT_AUTH, argc, argv, NULL) < 0) {
                redis_fail_all(r, "Redis: out of memory during handshake");
                return;
            }
            if (r->db > 0 && redis_send_select(r) < 0) {
                redis_fail_all(r, "Redis: out of memory during handshake");
                return;
            }
            if (redis_flush(r) < 0)
                redis_fail_all(r, "Redis: cannot write to the socket");
            return;
        }
    } else {
        r->proto = 3;
    }
    if (r->db > 0) {
        if (redis_send_select(r) < 0) {
            redis_fail_all(r, "Redis: out of memory during handshake");
            return;
        }
        if (redis_flush(r) < 0)
            redis_fail_all(r, "Redis: cannot write to the socket");
        return;
    }
    redis_after_handshake(r);
}

static void redis_note_confirm(dyn_redis_t* r, dyn_redis_pending_t* p,
    const uint8_t* msg, size_t len)
{
    dyn_resp_reader_t rd;
    dyn_resp_item_t it;
    static const char* const kinds[6] = { "subscribe", "psubscribe",
        "ssubscribe", "unsubscribe", "punsubscribe", "sunsubscribe" };
    int shard, unsub, kind;

    dyn_resp_reader_init(&rd, msg, len);
    if (dyn_resp_next(&rd, &it) != DYN_RESP_OK
        || (it.type != DYN_RESP_ARRAY && it.type != DYN_RESP_PUSH)
        || it.count != 3)
        return;
    if (dyn_resp_next(&rd, &it) != DYN_RESP_OK || it.type != DYN_RESP_BULK)
        return;
    for (kind = 0; kind < 6; kind++)
        if (it.slen == strlen(kinds[kind])
            && memcmp(it.str, kinds[kind], it.slen) == 0)
            break;
    if (kind == 6)
        return;
    shard = kind == 2 || kind == 5;
    unsub = kind >= 3;
    if (dyn_resp_next(&rd, &it) != DYN_RESP_OK)
        return;
    if (dyn_resp_next(&rd, &it) != DYN_RESP_OK || it.type != DYN_RESP_INT
        || it.ival < 0 || it.ival > INT32_MAX)
        return;
    if (shard)
        r->sub_shard = (int)it.ival;
    else
        r->sub_cp = (int)it.ival;
    if (!unsub && p->sub_left > 0) {
        p->sub_left--;
        r->sub_guess--;
    }
    redis_sub_recount(r);
}

static int redis_push_is_delivery(const uint8_t* msg, size_t len)
{
    dyn_resp_reader_t rd;
    dyn_resp_item_t it;
    static const char* deliv[] = { "message", "pmessage", "smessage" };
    size_t i;

    dyn_resp_reader_init(&rd, msg, len);
    if (dyn_resp_next(&rd, &it) != DYN_RESP_OK || it.type != DYN_RESP_ARRAY)
        return 1;
    if (it.count < 3)
        return 0;
    if (dyn_resp_next(&rd, &it) != DYN_RESP_OK || it.type != DYN_RESP_BULK)
        return 1;
    for (i = 0; i < countof(deliv); i++)
        if (it.slen == strlen(deliv[i]) && memcmp(it.str, deliv[i], it.slen) == 0)
            return 1;
    return 0;
}

static void redis_deliver_push(dyn_redis_t* r, const uint8_t* msg, size_t len)
{
    JSContext* ctx = r->ctx;
    dyn_resp_reader_t rd;
    JSValue v;
    int is_err = 0;

    if (!JS_IsFunction(ctx, r->h_push))
        return;
    dyn_resp_reader_init(&rd, msg, len);
    v = redis_value(ctx, r, &rd, 0, &is_err);
    if (JS_IsException(v)) {
        dyn_exc_drop(ctx);
        return;
    }
    {
        JSValueConst a[1] = { v };
        JSValue res = JS_Call(ctx, r->h_push, JS_UNDEFINED, 1, a);
        if (JS_IsException(res))
            dyn_exc_drop(ctx);
        JS_FreeValue(ctx, res);
    }
    JS_FreeValue(ctx, v);
}

void redis_one_reply(dyn_redis_t* r, const uint8_t* msg, size_t len)
{
    JSContext* ctx = r->ctx;
    dyn_redis_pending_t* p;
    dyn_resp_reader_t rd;
    JSValue v;
    int is_err = 0;

    if (msg[0] == DYN_RESP_PUSH) {
        if (!(r->head && !r->head->internal && r->head->is_sub_cmd)) {
            redis_deliver_push(r, msg, len);
            return;
        }
    } else if (r->subscribed > 0 && msg[0] == DYN_RESP_ARRAY
        && redis_push_is_delivery(msg, len)
        && (r->sub_cp + r->sub_shard > 0 || r->head == NULL
            || r->head->internal || r->head->is_sub_cmd)) {
        redis_deliver_push(r, msg, len);
        return;
    }

    p = r->head;
    if (!p) {
        redis_fail_all(r, "Redis: reply with no command outstanding");
        return;
    }

    if (p->internal) {
        int kind = p->internal;
        dyn_resp_reader_init(&rd, msg, len);
        v = redis_value(ctx, r, &rd, 0, &is_err);
        if (JS_IsException(v)) {
            dyn_exc_drop(ctx);
            redis_fail_all(r, "Redis: malformed handshake reply");
            return;
        }
        JS_FreeValue(ctx, v);
        pend_pop(&r->head, &r->tail);
        r->npending--;
        pend_free(ctx, p);
        redis_release_if_idle(r);
        if (kind == RD_INT_HELLO) {
            redis_handle_hello(r, is_err);
        } else if (kind == RD_INT_AUTH && is_err) {
            redis_fail_all(r, "Redis: authentication rejected");
        } else if (kind == RD_INT_SELECT && is_err) {
            redis_fail_all(r, "Redis: SELECT rejected");
        } else if (r->head == NULL || r->head->internal == RD_INT_NONE) {
            if (r->state != RD_ST_READY)
                redis_after_handshake(r);
        }
        return;
    }

    if (len > 8 && msg[0] == DYN_RESP_ERROR && memcmp(msg + 1, "NOAUTH ", 7) == 0) {
        redis_fail_all(r, "Redis: the server requires authentication again");
        return;
    }

    dyn_resp_reader_init(&rd, msg, len);
    v = redis_value(ctx, r, &rd, 0, &is_err);
    if (JS_IsException(v)) {
        dyn_exc_drop(ctx);
        redis_fail_all(r, "Redis: malformed reply");
        return;
    }

    if (p->is_sub_cmd)
        redis_note_confirm(r, p, msg, len);

    if (p->want > 1) {
        JS_DefinePropertyValueUint32(ctx, p->acc, (uint32_t)p->got, v,
            JS_PROP_C_W_E);
        p->got++;
        if (p->got < p->want)
            return;
        pend_pop(&r->head, &r->tail);
        r->npending--;
        v = p->acc;
        p->acc = JS_UNDEFINED;
        redis_settle(r, p, 0, v);
        return;
    }
    pend_pop(&r->head, &r->tail);
    r->npending--;
    redis_settle(r, p, is_err, v);
}

int redis_reply_count(const char* cmd, size_t len, int argc)
{
    int per_channel = redis_verb_is(cmd, len, RD_SUB, countof(RD_SUB)) || redis_verb_is(cmd, len, RD_UNSUB, countof(RD_UNSUB));
    if (!per_channel)
        return 1;
    return argc > 1 ? argc - 1 : -1;
}
#endif
