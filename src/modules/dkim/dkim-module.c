#include "dkim-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_DKIM) && defined(CONFIG_OPENSSL)

#include <stdio.h>
#include <time.h>

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

static const char* const sign_keys[] = {
    "domain", "selector", "privateKey", "algorithm", "canonicalization", "headers", "identity",
    "bodyLength", "timestamp", "expiresIn", "oversign",
};
static const char* const verify_keys[] = { "lookup", "now", "maxSignatures", "minRsaBits" };

static uint8_t* bytes_copy(JSContext* ctx, JSValueConst v, size_t* n, const char* what)
{
    uint8_t* out;
    if (JS_IsString(v)) {
        const char* s = JS_ToCStringLen(ctx, n, v);
        if (!s)
            return NULL;
        out = (uint8_t*)malloc(*n ? *n : 1);
        if (out)
            memcpy(out, s, *n);
        JS_FreeCString(ctx, s);
    } else {
        int k = JS_GetBufferKind(v);
        const uint8_t* p = NULL;
        size_t off = 0, len = 0, bpe = 0, bl = 0;
        if (k == JS_BUFFER_KIND_BUFFER) {
            p = JS_GetArrayBuffer(ctx, &len, v);
        } else if (k == JS_BUFFER_KIND_VIEW) {
            JSValue b = JS_GetTypedArrayBuffer(ctx, v, &off, &len, &bpe);
            if (JS_IsException(b))
                return NULL;
            p = JS_GetArrayBuffer(ctx, &bl, b);
            JS_FreeValue(ctx, b);
            if (p && off + len <= bl)
                p += off;
            else if (len)
                p = NULL;
        } else {
            JS_ThrowTypeError(ctx, "dyna:dkim: %s must be a string, ArrayBuffer or ArrayBufferView", what);
            return NULL;
        }
        if (!p && len) {
            JS_ThrowTypeError(ctx, "dyna:dkim: %s is detached", what);
            return NULL;
        }
        *n = len;
        out = (uint8_t*)malloc(len ? len : 1);
        if (out && len)
            memcpy(out, p, len);
    }
    if (!out)
        JS_ThrowOutOfMemory(ctx);
    return out;
}

static int opt_str(JSContext* ctx, JSValueConst o, const char* k, char** out, int required)
{
    JSValue v = JS_GetPropertyStr(ctx, o, k);
    const char* s;
    size_t n;
    *out = NULL;
    if (JS_IsException(v))
        return -1;
    if (JS_IsUndefined(v)) {
        if (required) {
            JS_ThrowTypeError(ctx, "dyna:dkim: %s is required", k);
            return -1;
        }
        return 0;
    }
    if (!JS_IsString(v)) {
        JS_FreeValue(ctx, v);
        JS_ThrowTypeError(ctx, "dyna:dkim: %s must be a string", k);
        return -1;
    }
    s = JS_ToCStringLen(ctx, &n, v);
    JS_FreeValue(ctx, v);
    if (!s)
        return -1;
    *out = (char*)malloc(n + 1);
    if (*out) {
        memcpy(*out, s, n);
        (*out)[n] = 0;
    }
    JS_FreeCString(ctx, s);
    if (!*out) {
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    if (strlen(*out) != n) {
        free(*out);
        *out = NULL;
        JS_ThrowTypeError(ctx, "dyna:dkim: %s contains a NUL", k);
        return -1;
    }
    return 1;
}

static int opt_num(JSContext* ctx, JSValueConst o, const char* k, double lo, double hi, double* out)
{
    JSValue v = JS_GetPropertyStr(ctx, o, k);
    double d;
    if (JS_IsException(v))
        return -1;
    if (JS_IsUndefined(v))
        return 0;
    if (JS_IsNull(v))
        return 2;
    if (!JS_IsNumber(v)) {
        JS_FreeValue(ctx, v);
        JS_ThrowTypeError(ctx, "dyna:dkim: %s must be a number", k);
        return -1;
    }
    JS_ToFloat64(ctx, &d, v);
    if (!(d >= lo && d <= hi) || d != (double)(int64_t)d) {
        JS_ThrowRangeError(ctx, "dyna:dkim: %s must be an integer in [%.0f, %.0f]", k, lo, hi);
        return -1;
    }
    *out = d;
    return 1;
}

static int opt_bool(JSContext* ctx, JSValueConst o, const char* k, int* out)
{
    JSValue v = JS_GetPropertyStr(ctx, o, k);
    if (JS_IsException(v))
        return -1;
    if (JS_IsUndefined(v))
        return 0;
    if (!JS_IsBool(v)) {
        JS_FreeValue(ctx, v);
        JS_ThrowTypeError(ctx, "dyna:dkim: %s must be a boolean", k);
        return -1;
    }
    *out = JS_ToBool(ctx, v);
    return 1;
}

static int js_len(JSContext* ctx, JSValueConst v, int64_t* n)
{
    JSValue l = JS_GetPropertyStr(ctx, v, "length");
    int r = JS_ToInt64(ctx, n, l);
    JS_FreeValue(ctx, l);
    return r;
}

static void sign_job_free(dkim_sign_job_t* j)
{
    size_t i;
    if (!j)
        return;
    free(j->msg);
    if (j->pem) {
        volatile char* v = j->pem;
        size_t k;
        for (k = 0; k < j->pemn; k++)
            v[k] = 0;
        free(j->pem);
    }
    {
        volatile uint8_t* v = j->seed;
        for (i = 0; i < 32; i++)
            v[i] = 0;
    }
    free(j->d);
    free(j->s);
    free(j->i);
    for (i = 0; i < j->nheads; i++)
        free(j->heads[i]);
    free(j->heads);
    dyn_sb_free(&j->out);
    free(j);
}

static JSValue dkim_error(JSContext* ctx, const char* msg)
{
    JSValue e = JS_NewError(ctx);
    char buf[200];
    snprintf(buf, sizeof buf, "dyna:dkim: %s", msg);
    JS_DefinePropertyValueStr(ctx, e, "message", JS_NewString(ctx, buf), JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    return JS_Throw(ctx, e);
}

static JSValue sign_entry(JSContext* ctx, void* arg)
{
    dkim_sign_job_t* j = (dkim_sign_job_t*)arg;
    JSValue r;
    int rc = DYN_OFFLOAD_CPU(dkim_sign_work(j));
    r = rc ? dkim_error(ctx, j->err ? j->err : "signing failed") : JS_NewStringLen(ctx, j->out.p, j->out.n);
    sign_job_free(j);
    return r;
}

static int parse_canon(JSContext* ctx, const char* c, dkim_sign_opts_t* o)
{
    const char* sl = strchr(c, '/');
    size_t hn = sl ? (size_t)(sl - c) : strlen(c);
    const char* b = sl ? sl + 1 : "simple";
    if (hn == 7 && !strncmp(c, "relaxed", 7))
        o->hrel = 1;
    else if (!(hn == 6 && !strncmp(c, "simple", 6)))
        goto bad;
    if (!strcmp(b, "relaxed"))
        o->brel = 1;
    else if (strcmp(b, "simple"))
        goto bad;
    return 0;
bad:
    JS_ThrowTypeError(ctx, "dyna:dkim: canonicalization must be simple|relaxed[/simple|relaxed]");
    return -1;
}

static int sign_heads(JSContext* ctx, JSValueConst opts, dkim_sign_job_t* j)
{
    JSValue v = JS_GetPropertyStr(ctx, opts, "headers"), e;
    int64_t len = 0, i;
    if (JS_IsException(v))
        return -1;
    if (JS_IsUndefined(v))
        return 0;
    if (!JS_IsArray(ctx, v) || js_len(ctx, v, &len) < 0 || len < 1 || len > DKIM_H_MAX) {
        JS_FreeValue(ctx, v);
        JS_ThrowTypeError(ctx, "dyna:dkim: headers must be an array of 1..%u header names", DKIM_H_MAX);
        return -1;
    }
    j->heads = (char**)calloc((size_t)len, sizeof(char*));
    if (!j->heads) {
        JS_FreeValue(ctx, v);
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    for (i = 0; i < len; i++) {
        const char* s;
        size_t n;
        e = JS_GetPropertyUint32(ctx, v, (uint32_t)i);
        if (!JS_IsString(e)) {
            JS_FreeValue(ctx, e);
            JS_FreeValue(ctx, v);
            JS_ThrowTypeError(ctx, "dyna:dkim: headers[%lld] must be a string", (long long)i);
            return -1;
        }
        s = JS_ToCStringLen(ctx, &n, e);
        JS_FreeValue(ctx, e);
        if (!s || !(j->heads[i] = (char*)malloc(n + 1))) {
            if (s)
                JS_FreeCString(ctx, s);
            JS_FreeValue(ctx, v);
            if (s)
                JS_ThrowOutOfMemory(ctx);
            return -1;
        }
        memcpy(j->heads[i], s, n + 1);
        JS_FreeCString(ctx, s);
        j->nheads = (size_t)i + 1;
    }
    JS_FreeValue(ctx, v);
    return 0;
}

static int sign_opts(JSContext* ctx, JSValueConst opts, dkim_sign_job_t* j)
{
    JSValue v;
    char* tmp = NULL;
    double d = 0;
    int r;
    if (!JS_IsObject(opts)) {
        JS_ThrowTypeError(ctx, "dyna:dkim: sign options must be an object");
        return -1;
    }
    if (dyn_opts_strict(ctx, opts, sign_keys, countof(sign_keys)))
        return -1;
    if (opt_str(ctx, opts, "domain", &j->d, 1) < 0 || opt_str(ctx, opts, "selector", &j->s, 1) < 0
        || opt_str(ctx, opts, "identity", &j->i, 0) < 0)
        return -1;
    v = JS_GetPropertyStr(ctx, opts, "privateKey");
    if (JS_IsException(v))
        return -1;
    if (JS_IsString(v)) {
        j->pem = (char*)bytes_copy(ctx, v, &j->pemn, "privateKey");
    } else if (JS_GetBufferKind(v) != JS_BUFFER_KIND_NONE) {
        size_t n = 0;
        uint8_t* p = bytes_copy(ctx, v, &n, "privateKey");
        if (p && n == 32) {
            memcpy(j->seed, p, 32);
            j->has_seed = 1;
        }
        if (p) {
            memset(p, 0, n);
            free(p);
        }
        if (p && n != 32) {
            JS_FreeValue(ctx, v);
            JS_ThrowTypeError(ctx, "dyna:dkim: a byte privateKey must be a 32-byte Ed25519 seed");
            return -1;
        }
    } else {
        JS_FreeValue(ctx, v);
        JS_ThrowTypeError(ctx, "dyna:dkim: privateKey is required (PEM string or 32-byte Ed25519 seed)");
        return -1;
    }
    JS_FreeValue(ctx, v);
    if (!j->pem && !j->has_seed)
        return -1;
    if ((r = opt_str(ctx, opts, "algorithm", &tmp, 0)) < 0)
        return -1;
    if (r) {
        j->algo_req = !strcmp(tmp, "rsa-sha256") ? DKIM_RSA : !strcmp(tmp, "ed25519-sha256") ? DKIM_ED25519 : 0;
        free(tmp);
        if (!j->algo_req) {
            JS_ThrowTypeError(ctx, "dyna:dkim: algorithm must be \"rsa-sha256\" or \"ed25519-sha256\"");
            return -1;
        }
    }
    j->o.hrel = j->o.brel = 1;
    if ((r = opt_str(ctx, opts, "canonicalization", &tmp, 0)) < 0)
        return -1;
    if (r) {
        j->o.hrel = j->o.brel = 0;
        r = parse_canon(ctx, tmp, &j->o);
        free(tmp);
        if (r < 0)
            return -1;
    }
    if (sign_heads(ctx, opts, j) < 0)
        return -1;
    if (opt_bool(ctx, opts, "bodyLength", &j->o.use_l) < 0 || opt_bool(ctx, opts, "oversign", &j->o.oversign) < 0)
        return -1;
    j->o.t = (int64_t)time(NULL);
    j->o.x = -1;
    if ((r = opt_num(ctx, opts, "timestamp", 0, 9007199254740991.0, &d)) < 0)
        return -1;
    if (r == 1)
        j->o.t = (int64_t)d;
    else if (r == 2)
        j->o.t = -1;
    if ((r = opt_num(ctx, opts, "expiresIn", 1, 9007199254740991.0, &d)) < 0)
        return -1;
    if (r == 1) {
        int64_t base = j->o.t >= 0 ? j->o.t : (int64_t)time(NULL);
        j->o.x = base + (int64_t)d;
    }
    j->o.d = j->d;
    j->o.s = j->s;
    j->o.i = j->i;
    j->o.heads = (const char* const*)j->heads;
    j->o.nheads = j->nheads;
    return 0;
}

static JSValue js_dkim_sign(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    dkim_sign_job_t* j;
    JSValue p;
    (void)this_val;
    j = (dkim_sign_job_t*)calloc(1, sizeof *j);
    if (!j)
        return JS_ThrowOutOfMemory(ctx);
    dyn_sb_init(&j->out);
    if (!(j->msg = bytes_copy(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, &j->n, "message"))
        || sign_opts(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, j) < 0) {
        sign_job_free(j);
        return JS_EXCEPTION;
    }
    p = dyn_fiber_start(ctx, sign_entry, j);
    if (JS_IsException(p))
        sign_job_free(j);
    return p;
}

static void verify_job_free(JSRuntime* rt, dkim_verify_job_t* j)
{
    size_t i;
    if (!j)
        return;
    for (i = 0; i < j->ns; i++) {
        dkim_sig_free(&j->sig[i].s);
        dyn_sb_free(&j->sig[i].txt);
    }
    JS_FreeValueRT(rt, j->lookup);
    JS_FreeValueRT(rt, j->lthis);
    free(j->sig);
    if (j->m)
        dkim_msg_free(j->m);
    free(j->m);
    free(j->msg);
    free(j);
}

static int txt_take(JSContext* ctx, JSValueConst v, dyn_sb_t* out)
{
    int64_t n = 0, i;
    if (JS_IsString(v)) {
        size_t k;
        const char* s = JS_ToCStringLen(ctx, &k, v);
        if (!s)
            return -1;
        dyn_sb_put(out, s, k);
        JS_FreeCString(ctx, s);
        return out->n > DKIM_TXT_MAX ? -2 : 0;
    }
    if (!JS_IsArray(ctx, v) || js_len(ctx, v, &n) < 0)
        return -3;
    if (n == 0)
        return 1;
    for (i = 0; i < n; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, v, (uint32_t)i);
        int r;
        if (JS_IsArray(ctx, e)) {
            r = i == 0 ? txt_take(ctx, e, out) : 0;
            JS_FreeValue(ctx, e);
            if (r)
                return r;
            if (i == 0)
                return 0;
            continue;
        }
        if (!JS_IsString(e)) {
            JS_FreeValue(ctx, e);
            return -3;
        }
        r = txt_take(ctx, e, out);
        JS_FreeValue(ctx, e);
        if (r)
            return r;
    }
    return 0;
}

static void lookup_failed(JSContext* ctx, dkim_vsig_t* v)
{
    JSValue e = JS_GetException(ctx);
    const char* s = JS_ToCString(ctx, e);
    if (!s)
        JS_FreeValue(ctx, JS_GetException(ctx));
    if (s && (strstr(s, "rcode 3") || strstr(s, "NXDOMAIN") || strstr(s, "ENOTFOUND"))) {
        v->keyst = 2;
    } else {
        v->keyst = 3;
        snprintf(v->rbuf, sizeof v->rbuf, "key lookup failed: %s", s ? s : "error");
        v->status = VS_TEMPERROR;
        v->reason = v->rbuf;
    }
    if (s)
        JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, e);
}

static const char* const st_names[] = { "permerror", "pass", "fail", "neutral", "temperror", "permerror" };

static void put_sb(JSContext* ctx, JSValue o, const char* k, const dyn_sb_t* b)
{
    JS_SetPropertyStr(ctx, o, k, JS_NewStringLen(ctx, b->p ? b->p : "", b->n));
}

static JSValue result_of(JSContext* ctx, const dkim_vsig_t* v)
{
    JSValue o = JS_NewObject(ctx), h;
    const char* n = v->s.h.p;
    size_t i;
    if (JS_IsException(o))
        return o;
    JS_SetPropertyStr(ctx, o, "status", JS_NewString(ctx, st_names[v->status]));
    put_sb(ctx, o, "domain", &v->s.d);
    put_sb(ctx, o, "selector", &v->s.s);
    JS_SetPropertyStr(ctx, o, "algorithm", v->s.algo == DKIM_RSA ? JS_NewString(ctx, "rsa-sha256") : v->s.algo == DKIM_ED25519 ? JS_NewString(ctx, "ed25519-sha256") : JS_UNDEFINED);
    put_sb(ctx, o, "identity", &v->s.i);
    h = JS_NewArray(ctx);
    for (i = 0; i < v->s.nh && n; i++) {
        size_t k = strlen(n);
        JS_SetPropertyInt64(ctx, h, (int64_t)i, JS_NewStringLen(ctx, n, k));
        n += k + 1;
    }
    JS_SetPropertyStr(ctx, o, "signedHeaders", h);
    if (v->s.l != DKIM_NO_LIMIT)
        JS_SetPropertyStr(ctx, o, "bodyLength", JS_NewFloat64(ctx, (double)v->s.l));
    if (v->s.t >= 0)
        JS_SetPropertyStr(ctx, o, "timestamp", JS_NewFloat64(ctx, (double)v->s.t));
    if (v->s.x >= 0)
        JS_SetPropertyStr(ctx, o, "expires", JS_NewFloat64(ctx, (double)v->s.x));
    JS_SetPropertyStr(ctx, o, "testing", JS_NewBool(ctx, v->testing));
    if (v->reason)
        JS_SetPropertyStr(ctx, o, "reason", JS_NewString(ctx, v->reason));
    return o;
}

static JSValue verify_entry(JSContext* ctx, void* arg)
{
    dkim_verify_job_t* j = (dkim_verify_job_t*)arg;
    JSValue pend[DKIM_SIGS_MAX], arr, r = JS_UNDEFINED;
    size_t i;
    int rc = DYN_OFFLOAD_CPU(dkim_verify_stage1(j));
    if (rc < 0) {
        verify_job_free(JS_GetRuntime(ctx), j);
        return JS_ThrowOutOfMemory(ctx);
    }
    arr = JS_NewArray(ctx);
    if (j->bad_msg) {
        JSValue o = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, o, "status", JS_NewString(ctx, "permerror"));
        JS_SetPropertyStr(ctx, o, "reason", JS_NewString(ctx, "message header is malformed"));
        JS_SetPropertyInt64(ctx, arr, 0, o);
        verify_job_free(JS_GetRuntime(ctx), j);
        return arr;
    }
    for (i = 0; i < j->ns; i++) {
        dkim_vsig_t* v = &j->sig[i];
        char name[300];
        JSValue a;
        pend[i] = JS_UNDEFINED;
        if (v->status != VS_PENDING)
            continue;
        snprintf(name, sizeof name, "%.*s._domainkey.%.*s", (int)v->s.s.n, v->s.s.p, (int)v->s.d.n, v->s.d.p);
        a = JS_NewString(ctx, name);
        pend[i] = JS_Call(ctx, j->lookup, j->lthis, 1, (JSValueConst*)&a);
        JS_FreeValue(ctx, a);
        if (!JS_IsException(pend[i]) && (int)JS_PromiseState(ctx, pend[i]) >= 0)
            JS_PromiseMarkHandled(ctx, pend[i]);
        if (JS_IsException(pend[i])) {
            lookup_failed(ctx, v);
            pend[i] = JS_UNDEFINED;
            if (v->keyst == 3)
                continue;
        }
        v->keyst = v->keyst ? v->keyst : 4;
    }
    for (i = 0; i < j->ns; i++) {
        dkim_vsig_t* v = &j->sig[i];
        JSValue res;
        int t;
        if (v->keyst != 4)
            continue;
        res = dyn_fiber_await(ctx, pend[i]);
        pend[i] = JS_UNDEFINED;
        if (JS_IsException(res)) {
            v->keyst = 0;
            lookup_failed(ctx, v);
            continue;
        }
        if (JS_IsNull(res) || JS_IsUndefined(res))
            t = 1;
        else
            t = txt_take(ctx, res, &v->txt);
        JS_FreeValue(ctx, res);
        if (t < 0 && JS_HasException(ctx)) {
            JSValue e = JS_GetException(ctx);
            const char* es = JS_ToCString(ctx, e);
            snprintf(v->rbuf, sizeof v->rbuf, "lookup result could not be read: %s", es ? es : "error");
            if (es)
                JS_FreeCString(ctx, es);
            JS_FreeValue(ctx, e);
            if (JS_HasException(ctx))
                JS_FreeValue(ctx, JS_GetException(ctx));
            t = -4;
        }
        if (t == 0)
            v->keyst = 1;
        else if (t == 1)
            v->keyst = 2;
        else {
            v->keyst = 3;
            v->status = VS_PERMERROR;
            v->reason = t == -4 ? v->rbuf : t == -2 ? "key record is longer than 64 KiB" : "lookup must resolve to a string, string[], string[][] or null";
        }
    }
    for (i = 0; i < j->ns; i++)
        if (j->sig[i].status == VS_PENDING && j->sig[i].keyst == 2) {
            j->sig[i].status = VS_PERMERROR;
            j->sig[i].reason = "no key record for selector";
        }
    rc = DYN_OFFLOAD_CPU(dkim_verify_stage2(j));
    for (i = 0; i < j->ns && !JS_IsException(arr); i++) {
        if (j->sig[i].status == VS_PENDING) {
            j->sig[i].status = VS_TEMPERROR;
            j->sig[i].reason = "key lookup did not complete";
        }
        r = result_of(ctx, &j->sig[i]);
        if (JS_IsException(r)) {
            JS_FreeValue(ctx, arr);
            arr = r;
            break;
        }
        JS_SetPropertyInt64(ctx, arr, (int64_t)i, r);
    }
    (void)rc;
    verify_job_free(JS_GetRuntime(ctx), j);
    return arr;
}

static int verify_opts(JSContext* ctx, JSValueConst opts, dkim_verify_job_t* j)
{
    JSValue lk;
    double d = 0;
    int r;
    if (!JS_IsObject(opts)) {
        JS_ThrowTypeError(ctx, "dyna:dkim: verify options must be an object with lookup");
        return -1;
    }
    if (dyn_opts_strict(ctx, opts, verify_keys, countof(verify_keys)))
        return -1;
    lk = JS_GetPropertyStr(ctx, opts, "lookup");
    if (JS_IsException(lk))
        return -1;
    if (JS_IsFunction(ctx, lk)) {
        j->lookup = lk;
    } else if (JS_IsObject(lk)) {
        JSValue f = JS_GetPropertyStr(ctx, lk, "resolveTxt");
        if (!JS_IsFunction(ctx, f)) {
            JS_FreeValue(ctx, f);
            JS_FreeValue(ctx, lk);
            JS_ThrowTypeError(ctx, "dyna:dkim: lookup must be a function or an object with resolveTxt()");
            return -1;
        }
        j->lookup = f;
        j->lthis = lk;
    } else {
        JS_FreeValue(ctx, lk);
        JS_ThrowTypeError(ctx, "dyna:dkim: lookup must be a function or an object with resolveTxt()");
        return -1;
    }
    j->now = (int64_t)time(NULL);
    j->max = 5;
    j->min_bits = 1024;
    if ((r = opt_num(ctx, opts, "now", 0, 9007199254740991.0, &d)) < 0)
        return -1;
    if (r == 1)
        j->now = (int64_t)d;
    if ((r = opt_num(ctx, opts, "maxSignatures", 1, DKIM_SIGS_MAX, &d)) < 0)
        return -1;
    if (r == 1)
        j->max = (size_t)d;
    if ((r = opt_num(ctx, opts, "minRsaBits", 512, 16384, &d)) < 0)
        return -1;
    if (r == 1)
        j->min_bits = (int)d;
    return 0;
}

static JSValue js_dkim_verify(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    dkim_verify_job_t* j;
    JSValue p;
    (void)this_val;
    j = (dkim_verify_job_t*)calloc(1, sizeof *j);
    if (!j)
        return JS_ThrowOutOfMemory(ctx);
    j->lookup = JS_UNDEFINED;
    j->lthis = JS_UNDEFINED;
    if (!(j->msg = bytes_copy(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, &j->n, "message"))
        || verify_opts(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, j) < 0) {
        verify_job_free(JS_GetRuntime(ctx), j);
        return JS_EXCEPTION;
    }
    p = dyn_fiber_start(ctx, verify_entry, j);
    if (JS_IsException(p))
        verify_job_free(JS_GetRuntime(ctx), j);
    return p;
}

static const JSCFunctionListEntry dkim_exports[] = {
    JS_CFUNC_DEF("sign", 2, js_dkim_sign),
    JS_CFUNC_DEF("verify", 2, js_dkim_verify),
};

static int dkim_init_module(JSContext* ctx, JSModuleDef* m)
{
    return JS_SetModuleExportList(ctx, m, dkim_exports, countof(dkim_exports));
}

int js_nat_init_dkim(JSContext* ctx)
{
    JSModuleDef* m = JS_NewCModule(ctx, "dyna:dkim", dkim_init_module);
    if (!m)
        return -1;
    return JS_AddModuleExportList(ctx, m, dkim_exports, countof(dkim_exports));
}

#endif
