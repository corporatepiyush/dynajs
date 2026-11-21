#include "dns-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_DNS)

JSValue dns_shape_records(JSContext* ctx, JSValueConst records,
    int shape)
{
    JSValue arr;
    uint32_t n = 0, i, out = 0;
    JSValue lv;

    if (shape == DNS_SHAPE_RECORDS)
        return JS_DupValue(ctx, records);
    lv = JS_GetPropertyStr(ctx, records, "length");
    if (JS_ToUint32(ctx, &n, lv)) {
        JS_FreeValue(ctx, lv);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, lv);
    arr = JS_NewArray(ctx);
    if (JS_IsException(arr))
        return arr;
    for (i = 0; i < n; i++) {
        JSValue rec = JS_GetPropertyUint32(ctx, records, i), v;
        if (JS_IsException(rec))
            goto fail;
        switch (shape) {
        case DNS_SHAPE_STRINGS:
        case DNS_SHAPE_TXT: {
            v = JS_GetPropertyStr(ctx, rec, shape == DNS_SHAPE_TXT ? "chunks" : "target");
            if (JS_IsException(v)) {
                JS_FreeValue(ctx, rec);
                goto fail;
            }
            break;
        }
        case DNS_SHAPE_MX: {
            JSValue prio = JS_GetPropertyStr(ctx, rec, "priority");
            JSValue exch;
            if (JS_IsException(prio)) {
                JS_FreeValue(ctx, rec);
                goto fail;
            }
            exch = JS_GetPropertyStr(ctx, rec, "exchange");
            if (JS_IsException(exch)) {
                JS_FreeValue(ctx, prio);
                JS_FreeValue(ctx, rec);
                goto fail;
            }
            v = JS_NewObject(ctx);
            if (JS_IsException(v)) {
                JS_FreeValue(ctx, prio);
                JS_FreeValue(ctx, exch);
                JS_FreeValue(ctx, rec);
                goto fail;
            }
            JS_DefinePropertyValueStr(ctx, v, "priority", prio, JS_PROP_C_W_E);
            JS_DefinePropertyValueStr(ctx, v, "exchange", exch, JS_PROP_C_W_E);
            break;
        }
        default:
            v = JS_DupValue(ctx, rec);
            break;
        }
        JS_FreeValue(ctx, rec);
        JS_DefinePropertyValueUint32(ctx, arr, out++, v, JS_PROP_C_W_E);
    }
    return arr;

fail:
    JS_FreeValue(ctx, arr);
    return JS_EXCEPTION;
}

int dns_rr_owner_is(const dyn_dns_rr_t* rr, const char* name)
{
    return strcasecmp(rr->name, name) == 0;
}

JSValue dns_rr_to_js(JSContext* ctx, const dyn_dns_rr_t* rr,
    const uint8_t* msg, size_t len)
{
    char ip[64], name[DYN_DNS_MAX_NAME + 1];
    JSValue o;
    size_t rdoff;
    int t;

    rdoff = (size_t)(rr->rdata - msg);
    switch (rr->type) {
    case DYN_DNS_T_A:
        if (rr->rdlen != 4 || !inet_ntop(AF_INET, rr->rdata, ip, sizeof(ip)))
            return JS_UNDEFINED;
        break;
    case DYN_DNS_T_AAAA:
        if (rr->rdlen != 16 || !inet_ntop(AF_INET6, rr->rdata, ip, sizeof(ip)))
            return JS_UNDEFINED;
        break;
    case DYN_DNS_T_CNAME:
    case DYN_DNS_T_NS:
    case DYN_DNS_T_PTR:
        if (rdoff >= len || (size_t)rr->rdlen > len - rdoff)
            return JS_UNDEFINED;
        t = dyn_dns_name_decode(msg, len, rdoff, name, sizeof(name));
        if (t < 0 || (size_t)t > rdoff + rr->rdlen)
            return JS_UNDEFINED;
        break;
    case DYN_DNS_T_MX:
        if (rr->rdlen < 3 || rdoff >= len || (size_t)rr->rdlen > len - rdoff)
            return JS_UNDEFINED;
        t = dyn_dns_name_decode(msg, len, rdoff + 2, name, sizeof(name));
        if (t < 0 || (size_t)t > rdoff + rr->rdlen)
            return JS_UNDEFINED;
        break;
    case DYN_DNS_T_TXT:
        if (rr->rdlen < 1 || rdoff >= len || (size_t)rr->rdlen > len - rdoff)
            return JS_UNDEFINED;
        break;
    default:
        return JS_UNDEFINED;
    }
    o = JS_NewObject(ctx);
    if (JS_IsException(o))
        return o;
    JS_DefinePropertyValueStr(ctx, o, "name", JS_NewString(ctx, rr->name),
        JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, o, "type", JS_NewInt32(ctx, rr->type),
        JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, o, "ttl", JS_NewInt64(ctx, rr->ttl),
        JS_PROP_C_W_E);
    switch (rr->type) {
    case DYN_DNS_T_A:
    case DYN_DNS_T_AAAA:
        JS_DefinePropertyValueStr(ctx, o, "address", JS_NewString(ctx, ip),
            JS_PROP_C_W_E);
        break;
    case DYN_DNS_T_CNAME:
    case DYN_DNS_T_NS:
    case DYN_DNS_T_PTR:
        JS_DefinePropertyValueStr(ctx, o, "target", JS_NewString(ctx, name),
            JS_PROP_C_W_E);
        break;
    case DYN_DNS_T_MX:
        JS_DefinePropertyValueStr(ctx, o, "priority",
            JS_NewInt32(ctx, (rr->rdata[0] << 8) | rr->rdata[1]),
            JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, o, "exchange", JS_NewString(ctx, name),
            JS_PROP_C_W_E);
        break;
    case DYN_DNS_T_TXT: {
        JSValue chunks = JS_NewArray(ctx);
        size_t at = 0;
        uint32_t ci = 0;
        if (JS_IsException(chunks))
            return chunks;
        while (at < rr->rdlen) {
            size_t cl = rr->rdata[at];
            if (cl > rr->rdlen - at - 1)
                break;
            JS_DefinePropertyValueUint32(ctx, chunks, ci++,
                JS_NewStringLen(ctx, (const char*)rr->rdata + at + 1, cl),
                JS_PROP_C_W_E);
            at += 1 + cl;
        }
        JS_DefinePropertyValueStr(ctx, o, "chunks", chunks, JS_PROP_C_W_E);
        break;
    }
    }
    return o;
}

static JSValue dns_clone_record(JSContext* ctx, JSValueConst rec)
{
    static const char* const keys[] = { "name", "type", "ttl", "address",
        "target", "priority", "exchange",
        "chunks" };
    JSValue o = JS_NewObject(ctx);
    int k;

    if (JS_IsException(o))
        return o;
    for (k = 0; k < (int)countof(keys); k++) {
        JSValue v = JS_GetPropertyStr(ctx, rec, keys[k]);
        if (JS_IsException(v)) {
            if (JS_HasUncatchableException(ctx)) {
                JS_FreeValue(ctx, o);
                return JS_EXCEPTION;
            }
            JS_FreeValue(ctx, JS_GetException(ctx));
            continue;
        }
        if (JS_IsUndefined(v)) {
            JS_FreeValue(ctx, v);
            continue;
        }
        if (k == (int)countof(keys) - 1 && JS_IsArray(ctx, v)) {
            JSValue lv = JS_GetPropertyStr(ctx, v, "length");
            uint32_t n = 0, i;
            JSValue copy = JS_NewArray(ctx);
            if (!JS_IsException(lv) && !JS_IsException(copy) && !JS_ToUint32(ctx, &n, lv)) {
                for (i = 0; i < n; i++) {
                    JSValue el = JS_GetPropertyUint32(ctx, v, i);
                    if (JS_IsException(el)) {
                        if (JS_HasUncatchableException(ctx)) {
                            JS_FreeValue(ctx, copy);
                            JS_FreeValue(ctx, lv);
                            JS_FreeValue(ctx, v);
                            JS_FreeValue(ctx, o);
                            return JS_EXCEPTION;
                        }
                        JS_FreeValue(ctx, JS_GetException(ctx));
                        el = JS_UNDEFINED;
                    }
                    JS_DefinePropertyValueUint32(ctx, copy, i, el,
                        JS_PROP_C_W_E);
                }
            }
            JS_FreeValue(ctx, lv);
            JS_FreeValue(ctx, v);
            v = copy;
        }
        JS_DefinePropertyValueStr(ctx, o, keys[k], v, JS_PROP_C_W_E);
    }
    return o;
}

JSValue dns_clone_records(JSContext* ctx, JSValueConst records)
{
    JSValue lv = JS_GetPropertyStr(ctx, records, "length");
    uint32_t n = 0, i;
    JSValue arr = JS_NewArray(ctx);

    if (JS_IsException(lv))
        return lv;
    if (JS_ToUint32(ctx, &n, lv)) {
        JS_FreeValue(ctx, lv);
        JS_FreeValue(ctx, arr);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, lv);
    if (JS_IsException(arr))
        return arr;
    for (i = 0; i < n; i++) {
        JSValue rec = JS_GetPropertyUint32(ctx, records, i), c;
        if (JS_IsException(rec)) {
            if (JS_HasUncatchableException(ctx)) {
                JS_FreeValue(ctx, arr);
                return JS_EXCEPTION;
            }
            JS_FreeValue(ctx, JS_GetException(ctx));
            continue;
        }
        c = dns_clone_record(ctx, rec);
        JS_FreeValue(ctx, rec);
        if (JS_IsException(c)) {
            JS_FreeValue(ctx, arr);
            return c;
        }
        JS_DefinePropertyValueUint32(ctx, arr, i, c, JS_PROP_C_W_E);
    }
    return arr;
}

#endif
