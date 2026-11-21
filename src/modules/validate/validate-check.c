#include "validate-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_VALIDATE)

#define DYN_V_MAX 4096

static int dyn_v_arg(JSContext* ctx, int argc, JSValueConst* argv,
    const char* fn, const char** s, size_t* n)
{
    if (argc < 1 || !JS_IsString(argv[0])) {
        JS_ThrowTypeError(ctx, "%s(text): argument must be a string", fn);
        return -1;
    }
    *s = JS_ToCStringLen(ctx, n, argv[0]);
    if (!*s)
        return -1;
    if (*n > DYN_V_MAX) {
        JS_FreeCString(ctx, *s);
        JS_ThrowRangeError(ctx, "%s(text): input exceeds %d bytes", fn, DYN_V_MAX);
        return -1;
    }
    return 0;
}

JSValue dyn_v_check(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    static const char* const NAMES[] = {
        "IsAlpha", "IsAlphanumeric", "IsAscii", "IsEmail", "IsCreditCard", "IsIBAN",
        "IsIPv4", "IsIPv6", "IsIP", "IsPort", "IsBase64", "IsBase64Url",
        "IsHex", "IsHexColor", "IsNumeric", "IsDateString", "IsRFC3339", "IsMimeType"
    };
    const char* s;
    size_t n, i;
    int ok = 1;

    if (dyn_v_arg(ctx, argc, argv, NAMES[magic], &s, &n) < 0)
        return JS_EXCEPTION;
    switch (magic) {
    case V_EMAIL:
        ok = dyn_v_email(s, n);
        break;
    case V_LUHN:
        ok = dyn_v_luhn(s, n);
        break;
    case V_IBAN:
        ok = dyn_v_iban(s, n);
        break;
    case V_IPV4:
        ok = dyn_v_ipv4(s, n);
        break;
    case V_IPV6:
        ok = dyn_v_ipv6(s, n);
        break;
    case V_IP:
        ok = dyn_v_ipv4(s, n) || dyn_v_ipv6(s, n);
        break;
    case V_PORT:
        ok = dyn_v_port(s, n);
        break;
    case V_BASE64:
        ok = dyn_v_base64(s, n);
        break;
    case V_BASE64URL:
        ok = dyn_v_base64url(s, n);
        break;
    case V_HEX:
        ok = dyn_v_hex(s, n);
        break;
    case V_HEXCOLOR:
        ok = dyn_v_hexcolor(s, n);
        break;
    case V_NUMERIC:
        ok = dyn_v_numeric(s, n);
        break;
    case V_DATESTRING:
        ok = dyn_v_datestring(s, n);
        break;
    case V_RFC3339:
        ok = dyn_v_rfc3339(s, n);
        break;
    case V_MIME:
        ok = dyn_v_mime(s, n);
        break;
    default:
        if (n == 0) {
            ok = 0;
            break;
        }
        for (i = 0; i < n && ok; i++) {
            unsigned char c = (unsigned char)s[i];
            if (magic == V_ALPHA)
                ok = dyn_v_alpha(c);
            else if (magic == V_ALNUM)
                ok = dyn_v_alpha(c) || dyn_v_digit(c);
            else
                ok = c < 0x80;
        }
    }
    JS_FreeCString(ctx, s);
    return JS_NewBool(ctx, ok);
}

static JSValue dyn_v_export(JSContext* ctx, const char* module,
    const char* fnname)
{
    JSValue cached = dyn_v_cache_find(ctx, module, fnname);

    if (!JS_IsUndefined(cached))
        return cached;
    char src[256], gname[80];
    JSValue r, v, g, fn;
    JSPropertyDescriptor desc;
    JSAtom atom;
    uint64_t rnd = 0;
    static uint64_t seq;

    if (dyn_os_entropy(&rnd, sizeof rnd) < 0)
        rnd = (uint64_t)(uintptr_t)ctx;
    snprintf(gname, sizeof gname, "__dyna_vx_%llx_%llx",
        (unsigned long long)rnd, (unsigned long long)++seq);
    snprintf(src, sizeof src,
        "import * as m from \"%s\";\n"
        "globalThis[\"%s\"] = m.%s;\n",
        module, gname, fnname);
    g = JS_GetGlobalObject(ctx);
    atom = JS_NewAtom(ctx, gname);
    if (atom == JS_ATOM_NULL) {
        JS_FreeValue(ctx, g);
        return JS_EXCEPTION;
    }
    {
        int gr = JS_GetOwnProperty(ctx, &desc, g, atom);
        if (gr != 0) {
            if (gr > 0) {
                JS_FreeValue(ctx, desc.value);
                JS_FreeValue(ctx, desc.getter);
                JS_FreeValue(ctx, desc.setter);
            }
            JS_FreeAtom(ctx, atom);
            JS_FreeValue(ctx, g);
            return gr > 0
                ? JS_ThrowInternalError(ctx, "dyna:validate: bridge slot occupied")
                : JS_EXCEPTION;
        }
    }
    r = JS_Eval(ctx, src, strlen(src), "<dyna:validate>",
        JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(r)) {
        JS_FreeAtom(ctx, atom);
        JS_FreeValue(ctx, g);
        return r;
    }
    v = JS_EvalFunction(ctx, r);
    if (JS_IsException(v)) {
        JS_FreeAtom(ctx, atom);
        JS_FreeValue(ctx, g);
        return v;
    }
    if (JS_PromiseState(ctx, v) == JS_PROMISE_REJECTED) {
        JSValue err = JS_DupValue(ctx, JS_PromiseResult(ctx, v));
        JS_FreeValue(ctx, v);
        JS_FreeAtom(ctx, atom);
        JS_FreeValue(ctx, g);
        return JS_Throw(ctx, err);
    }
    JS_FreeValue(ctx, v);
    {
        int gr = JS_GetOwnProperty(ctx, &desc, g, atom);
        if (gr != 1) {
            if (gr > 0) {
                JS_FreeValue(ctx, desc.value);
                JS_FreeValue(ctx, desc.getter);
                JS_FreeValue(ctx, desc.setter);
            }
            JS_DeleteProperty(ctx, g, atom, 0);
            JS_FreeAtom(ctx, atom);
            JS_FreeValue(ctx, g);
            return gr < 0
                ? JS_EXCEPTION
                : JS_ThrowInternalError(ctx, "dyna:validate: bridge export failed");
        }
        if (desc.flags & JS_PROP_GETSET) {
            JS_FreeValue(ctx, desc.getter);
            JS_FreeValue(ctx, desc.setter);
            JS_DeleteProperty(ctx, g, atom, 0);
            JS_FreeAtom(ctx, atom);
            JS_FreeValue(ctx, g);
            return JS_ThrowInternalError(ctx,
                "dyna:validate: bridge slot is an accessor");
        }
        fn = desc.value;
        JS_DeleteProperty(ctx, g, atom, 0);
        JS_FreeAtom(ctx, atom);
        JS_FreeValue(ctx, g);
    }
    if (!JS_IsFunction(ctx, fn)) {
        JSValue e = fn;
        fn = JS_ThrowTypeError(ctx, "dyna:validate: %s has no export %s",
            module, fnname);
        JS_FreeValue(ctx, e);
    } else {
        dyn_v_cache_store(ctx, module, fnname, fn);
    }
    return fn;
}

static int dyn_v_js_bool(JSContext* ctx, const char* module,
    const char* fnname, const char* s, size_t n)
{
    JSValue fn, arg, v;
    JSValueConst a1[1];
    int ok;

    fn = dyn_v_export(ctx, module, fnname);
    if (JS_IsException(fn))
        return -1;
    arg = JS_NewStringLen(ctx, s, n);
    if (JS_IsException(arg)) {
        JS_FreeValue(ctx, fn);
        return -1;
    }
    a1[0] = arg;
    v = JS_Call(ctx, fn, JS_UNDEFINED, 1, a1);
    JS_FreeValue(ctx, fn);
    JS_FreeValue(ctx, arg);
    if (JS_IsException(v))
        return -1;
    ok = JS_ToBool(ctx, v);
    JS_FreeValue(ctx, v);
    return ok;
}

JSValue dyn_v_check_ext(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    static const char* const NAMES[] = {
        "IsURL", "IsDomain", "IsSlug", "IsUUID", "IsJWT", "IsSemver", "IsE164",
        "IsJSON", "IsStrongPassword"
    };
    const char* s;
    size_t n;
    int ok = 0;

    (void)this_val;
    if (dyn_v_arg(ctx, argc, argv, NAMES[magic], &s, &n) < 0)
        return JS_EXCEPTION;
    switch (magic) {
    case VX_URL: {
        JSValue fn, arg, u;
        JSValueConst a1[1];
        fn = dyn_v_export(ctx, "dyna:url", "URL");
        if (JS_IsException(fn)) {
            JS_FreeCString(ctx, s);
            return fn;
        }
        arg = JS_NewStringLen(ctx, s, n);
        if (JS_IsException(arg)) {
            JS_FreeValue(ctx, fn);
            JS_FreeCString(ctx, s);
            return arg;
        }
        a1[0] = arg;
        u = JS_CallConstructor(ctx, fn, 1, a1);
        JS_FreeValue(ctx, fn);
        JS_FreeValue(ctx, arg);
        if (JS_IsException(u)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            break;
        }
        ok = 1;
        {
            JSValue proto = JS_GetPropertyStr(ctx, u, "protocol");
            JSValue host = JS_GetPropertyStr(ctx, u, "hostname");
            const char* p = JS_IsString(proto) ? JS_ToCString(ctx, proto) : NULL;
            const char* h = JS_IsString(host) ? JS_ToCString(ctx, host) : NULL;
            if (!p || !h) {
                JS_FreeValue(ctx, JS_GetException(ctx));
                ok = 0;
            } else if (h[0] == '\0' && (!strcmp(p, "http:") || !strcmp(p, "https:") || !strcmp(p, "ws:") || !strcmp(p, "wss:") || !strcmp(p, "ftp:"))) {
                ok = 0;
            }
            if (p)
                JS_FreeCString(ctx, p);
            if (h)
                JS_FreeCString(ctx, h);
            JS_FreeValue(ctx, proto);
            JS_FreeValue(ctx, host);
        }
        JS_FreeValue(ctx, u);
        break;
    }
    case VX_DOMAIN:
        if (dyn_v_domain_grammar(s, n)) {
            int r = dyn_v_js_bool(ctx, "dyna:netip", "isValid",
                s, n);
            if (r < 0) {
                JS_FreeCString(ctx, s);
                return JS_EXCEPTION;
            }
            ok = !r;
        }
        break;
    case VX_SLUG:
        ok = dyn_v_slug(s, n);
        break;
    case VX_UUID:
        if (n == 36) {
            int r = dyn_v_js_bool(ctx, "dyna:uuid", "validate",
                s, n);
            if (r < 0) {
                JS_FreeCString(ctx, s);
                return JS_EXCEPTION;
            }
            ok = r;
        }
        break;
    case VX_JWT: {
        static const char* const jwt_keys[] = { "algs" };
        JSValue algs = JS_UNDEFINED;
        if (dyn_opts_bag(ctx, argc, argv, 1, jwt_keys, 1, "IsJWT") < 0) {
            JS_FreeCString(ctx, s);
            return JS_EXCEPTION;
        }
        if (argc > 1 && JS_IsObject(argv[1])) {
            algs = JS_GetPropertyStr(ctx, argv[1], "algs");
            if (JS_IsException(algs)) {
                JS_FreeCString(ctx, s);
                return JS_EXCEPTION;
            }
        }
        ok = dyn_v_jwt(ctx, s, n, algs);
        JS_FreeValue(ctx, algs);
        if (ok < 0) {
            JS_FreeCString(ctx, s);
            return JS_EXCEPTION;
        }
        break;
    }
    case VX_SEMVER: {
        int r = dyn_v_js_bool(ctx, "dyna:semver", "isValid",
            s, n);
        if (r < 0) {
            JS_FreeCString(ctx, s);
            return JS_EXCEPTION;
        }
        ok = r;
        break;
    }
    case VX_E164:
        ok = dyn_v_e164(s, n);
        break;
    case VX_JSON: {
        JSValue v = JS_ParseJSON(ctx, s, n, "<IsJSON>");
        if (JS_IsException(v)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            ok = 0;
        } else {
            JS_FreeValue(ctx, v);
            ok = 1;
        }
        break;
    }
    case VX_STRONGPW: {
        static const char* const OPTS[] = { "minLength", "minLower", "minUpper",
            "minDigits", "minSymbols" };
        uint32_t mins[5] = { 8, 1, 1, 1, 1 };
        uint32_t counts[5] = { 0, 0, 0, 0, 0 };
        uint32_t oi;
        size_t si;

        ok = 0;
        if (argc > 1 && JS_IsObject(argv[1])) {
            if (dyn_opts_strict(ctx, argv[1], OPTS, countof(OPTS)) < 0) {
                JS_FreeCString(ctx, s);
                return JS_EXCEPTION;
            }
            for (oi = 0; oi < 5; oi++) {
                JSValue v = JS_GetPropertyStr(ctx, argv[1], OPTS[oi]);
                double dv;
                if (JS_IsException(v)) {
                    JS_FreeCString(ctx, s);
                    return JS_EXCEPTION;
                }
                if (JS_IsUndefined(v)) {
                    JS_FreeValue(ctx, v);
                    continue;
                }
                if (!JS_IsNumber(v) || JS_ToFloat64(ctx, &dv, v) < 0) {
                    JS_FreeValue(ctx, v);
                    JS_FreeCString(ctx, s);
                    return JS_ThrowTypeError(ctx,
                        "%s(opts): %s must be a number", NAMES[magic], OPTS[oi]);
                }
                if (!(dv >= 0 && dv <= 4096 && dv == (uint32_t)dv)) {
                    JS_FreeValue(ctx, v);
                    JS_FreeCString(ctx, s);
                    return JS_ThrowRangeError(ctx,
                        "%s(opts): %s must be a non-negative integer <= 4096",
                        NAMES[magic], OPTS[oi]);
                }
                mins[oi] = (uint32_t)dv;
                JS_FreeValue(ctx, v);
            }
        }
        if (n == 0)
            break;
        ok = 1;
        for (si = 0; si < n; si++) {
            unsigned char c = (unsigned char)s[si];
            if (c < 0x20 || c > 0x7e) {
                ok = 0;
                break;
            }
            if (c >= 'a' && c <= 'z')
                counts[1]++;
            else if (c >= 'A' && c <= 'Z')
                counts[2]++;
            else if (dyn_v_digit(c))
                counts[3]++;
            else if (c != ' ')
                counts[4]++;
        }
        if (ok)
            ok = (uint32_t)n >= mins[0] && counts[1] >= mins[1] && counts[2] >= mins[2] && counts[3] >= mins[3] && counts[4] >= mins[4];
        break;
    }
    }
    JS_FreeCString(ctx, s);
    return JS_NewBool(ctx, ok);
}

#endif
