#include "yaml-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_YAML)

static JSValue yml_flow_scalar(yml_t* p, size_t* pi, size_t end)
{
    size_t i = *pi, st;
    const char* bad;

    while (i < end && (p->s[i] == ' ' || p->s[i] == '\t'))
        i++;
    if (i < end && (p->s[i] == '"' || p->s[i] == '\'')) {
        *pi = i;
        return yml_quoted(p, pi, end, 0);
    }
    st = i;
    while (i < end && p->s[i] != ',' && p->s[i] != ']' && p->s[i] != '}') {
        if (p->s[i] == '#' && (i == st || p->s[i - 1] == ' ' || p->s[i - 1] == '\t'))
            break;
        i++;
    }
    while (i > st && (p->s[i - 1] == ' ' || p->s[i - 1] == '\t'))
        i--;
    *pi = i;
    bad = yml_unsupported(p->s + st, i - st);
    if (bad) {
        snprintf(p->err, sizeof p->err, "%s at line %u", bad,
            (unsigned)(p->li + 1));
        return JS_EXCEPTION;
    }
    return yml_scalar(p, p->s + st, i - st);
}

static int yml_flow_pair(yml_t* p, size_t* pi, size_t end, JSAtom* pkey, JSValue* pv)
{
    size_t i = *pi, ks, ke;
    JSValue kv, v;
    JSAtom key;
    int null_val = 0;

    ks = i;
    {
        size_t f = ks;
        while (f < end && (p->s[f] == ' ' || p->s[f] == '\t'))
            f++;
        if (f < end && p->s[f] == ':') {
            yml_fail(p, "a flow mapping entry needs a key");
            return -1;
        }
    }
    i = yml_flow_colon(p->s, ks, end);
    if (i == (size_t)-1) {
        int has_colon = 0;
        size_t j = ks;
        while (j < end) {
            char c = p->s[j];
            if (c == '"' || c == '\'') {
                char q = c;
                j++;
                while (j < end) {
                    if (q == '"' && p->s[j] == '\\' && j + 1 < end) {
                        j += 2;
                        continue;
                    }
                    if (p->s[j] == q) {
                        j++;
                        break;
                    }
                    j++;
                }
                continue;
            }
            if (c == ':')
                has_colon = 1;
            else if (c == ',' || c == ']' || c == '}')
                break;
            j++;
        }
        if (!has_colon) {
            yml_fail(p, "a flow mapping entry needs a colon");
            return -1;
        }
        i = j;
        null_val = 1;
    }
    ke = i;
    while (ke > ks && (p->s[ke - 1] == ' ' || p->s[ke - 1] == '\t'))
        ke--;
    if (ke > ks && (p->s[ks] == '"' || p->s[ks] == '\'')) {
        size_t qp = ks;
        kv = yml_quoted(p, &qp, ke, 0);
        if (!JS_IsException(kv) && qp != ke) {
            JS_FreeValue(p->ctx, kv);
            yml_fail(p, "unexpected content after the quoted key");
            return -1;
        }
    } else {
        const char* bad = yml_unsupported(p->s + ks, ke - ks);
        if (bad) {
            snprintf(p->err, sizeof p->err, "%s at line %u", bad,
                (unsigned)(p->li + 1));
            return -1;
        }
        kv = JS_NewStringLen(p->ctx, p->s + ks, ke - ks);
    }
    if (JS_IsException(kv))
        return -1;
    key = JS_ValueToAtom(p->ctx, kv);
    JS_FreeValue(p->ctx, kv);
    if (key == JS_ATOM_NULL)
        return -1;
    if (null_val) {
        v = JS_NULL;
    } else {
        i++;
        v = yml_flow(p, &i, end);
    }
    if (JS_IsException(v)) {
        JS_FreeAtom(p->ctx, key);
        return -1;
    }
    *pkey = key;
    *pv = v;
    *pi = i;
    return 0;
}

JSValue yml_flow(yml_t* p, size_t* pi, size_t end)
{
    size_t i = *pi;
    JSValue out;
    yml_kv_t kv;
    uint32_t k = 0;
    int seq;

    while (i < end && (p->s[i] == ' ' || p->s[i] == '\t'))
        i++;
    if (i >= end)
        return yml_fail(p, "an empty flow collection needs its brackets");
    if (p->s[i] != '[' && p->s[i] != '{') {
        *pi = i;
        return yml_flow_scalar(p, pi, end);
    }
    seq = p->s[i] == '[';
    i++;
    if (p->depth >= p->max_depth)
        return yml_fail(p, "nesting exceeds the depth limit");
    out = seq ? JS_NewArray(p->ctx) : JS_UNDEFINED;
    if (JS_IsException(out))
        return out;
    kv.n = 0;
    kv.out = JS_UNDEFINED;
    p->depth++;
    for (;;) {
        JSValue v;
        while (i < end && (p->s[i] == ' ' || p->s[i] == '\t' || p->s[i] == ','))
            i++;
        if (i < end && p->s[i] == '#') {
            yml_fail(p, "a flow collection cannot contain a comment");
            goto fail;
        }
        if (i < end && p->s[i] == (seq ? ']' : '}')) {
            i++;
            break;
        }
        if (i >= end) {
            yml_fail(p, "unterminated flow collection");
            goto fail;
        }
        if (seq) {
            if (p->s[i] == '[' || p->s[i] == '{') {
                v = yml_flow(p, &i, end);
            } else if (yml_flow_is_pair(p->s, i, end)) {
                JSAtom key;
                JSValue pv;
                if (yml_flow_pair(p, &i, end, &key, &pv) < 0)
                    goto fail;
                v = JS_NewObjectFromProps(p->ctx, NULL, 1, &key, &pv);
            } else {
                v = yml_flow_scalar(p, &i, end);
            }
            if (JS_IsException(v)
                || JS_DefinePropertyValueUint32(p->ctx, out, k++, v,
                       JS_PROP_C_W_E)
                    < 0)
                goto fail;
            continue;
        }
        {
            JSAtom key;
            JSValue pv;
            if (yml_flow_pair(p, &i, end, &key, &pv) < 0 || yml_kv_add(p, &kv, key, pv) < 0)
                goto fail;
        }
    }
    if (!seq)
        out = yml_kv_finish(p, &kv);
    p->depth--;
    *pi = i;
    return out;
fail:
    yml_kv_abort(p, &kv);
    JS_FreeValue(p->ctx, out);
    p->depth--;
    return JS_EXCEPTION;
}

#endif
