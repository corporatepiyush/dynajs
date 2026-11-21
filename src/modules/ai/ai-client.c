#include "ai-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_AI) && defined(CONFIG_NATIVE_MODULE_HTTP)

JSClassID ai_client_class_id;
JSClassID ai_ns_class_id;

ai_client_t* ai_client_ref(ai_client_t* cl)
{
    if (cl)
        cl->refs++;
    return cl;
}

void ai_client_unref(ai_client_t* cl)
{
    if (!cl || --cl->refs > 0)
        return;
    free(cl->base_url);
    free(cl->api_key);
    free(cl->auth_token);
    free(cl->version);
    free(cl->betas);
    free(cl->extra_headers);
    free(cl->ca);
    free(cl->org);
    free(cl->project);
#ifdef CONFIG_TLS
    if (cl->tls)
        dyn_tls_ctx_free(cl->tls);
#endif
    free(cl);
}

int ai_client_tls(JSContext* ctx, ai_client_t* cl)
{
#ifdef CONFIG_TLS
    char why[192];
    if (cl->tls)
        return 0;
    why[0] = 0;
    cl->tls = hc_tls_client_ctx(cl->ca, why, sizeof why);
    if (!cl->tls) {
        JS_ThrowTypeError(ctx, "dyna:ai: TLS setup failed: %s", why[0] ? why : "unknown error");
        return -1;
    }
    return 0;
#else
    (void)cl;
    dyn_tls_missing(ctx, "dyna:ai", "https");
    return -1;
#endif
}

int ai_opts_check(JSContext* ctx, JSValueConst opts, const char* who,
    const char* const* keys, int n)
{
    JSPropertyEnum* tab = NULL;
    uint32_t len = 0, i;
    int k, rc = 0;
    if (JS_IsUndefined(opts) || JS_IsNull(opts))
        return 0;
    if (!JS_IsObject(opts) || JS_IsArray(ctx, opts)) {
        JS_ThrowTypeError(ctx, "%s: options must be an object", who);
        return -1;
    }
    if (JS_GetOwnPropertyNames(ctx, &tab, &len, opts,
            JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY)
        < 0)
        return -1;
    for (i = 0; i < len && !rc; i++) {
        const char* name = JS_AtomToCString(ctx, tab[i].atom);
        int found = 0;
        if (!name) {
            rc = -1;
            break;
        }
        for (k = 0; k < n; k++)
            if (strcmp(name, keys[k]) == 0)
                found = 1;
        if (!found) {
            dyn_sb_t sb;
            dyn_sb_init(&sb);
            for (k = 0; k < n; k++) {
                if (k)
                    dyn_sb_put(&sb, ", ", 2);
                dyn_sb_puts(&sb, keys[k]);
            }
            dyn_sb_putc(&sb, 0);
            JS_ThrowTypeError(ctx, "%s: unknown option \"%s\" (valid: %s)", who,
                name, sb.p ? sb.p : "");
            dyn_sb_free(&sb);
            rc = -1;
        }
        JS_FreeCString(ctx, name);
    }
    JS_FreePropertyEnum(ctx, tab, len);
    return rc;
}

char* ai_opt_str(JSContext* ctx, JSValueConst opts, const char* key,
    const char* who, int* err)
{
    JSValue v;
    char* out = NULL;
    *err = 0;
    if (!JS_IsObject(opts))
        return NULL;
    v = JS_GetPropertyStr(ctx, opts, key);
    if (JS_IsException(v)) {
        *err = 1;
        return NULL;
    }
    if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
        if (!JS_IsString(v)) {
            JS_ThrowTypeError(ctx, "%s: %s must be a string", who, key);
            *err = 1;
        } else {
            const char* s = JS_ToCString(ctx, v);
            if (!s || !(out = strdup(s))) {
                if (s)
                    JS_ThrowOutOfMemory(ctx);
                *err = 1;
            }
            JS_FreeCString(ctx, s);
        }
    }
    JS_FreeValue(ctx, v);
    return out;
}

static char* ai_env_dup(const char* name)
{
    const char* v = getenv(name);
    return v && *v ? strdup(v) : NULL;
}

static int ai_opt_num(JSContext* ctx, JSValueConst opts, const char* key,
    const char* who, double lo, double hi, double* out)
{
    JSValue v = JS_GetPropertyStr(ctx, opts, key);
    double d;
    if (JS_IsException(v))
        return -1;
    if (JS_IsUndefined(v)) {
        JS_FreeValue(ctx, v);
        return 0;
    }
    if (!JS_IsNumber(v)) {
        JS_FreeValue(ctx, v);
        JS_ThrowTypeError(ctx, "%s: %s must be a number", who, key);
        return -1;
    }
    JS_ToFloat64(ctx, &d, v);
    JS_FreeValue(ctx, v);
    if (!(d >= lo && d <= hi) || d != (double)(int64_t)d) {
        JS_ThrowRangeError(ctx, "%s: %s must be an integer in %.0f..%.0f", who,
            key, lo, hi);
        return -1;
    }
    *out = d;
    return 1;
}

int ai_hdr_block(JSContext* ctx, JSValueConst obj, const char* who,
    dyn_sb_t* out)
{
    JSPropertyEnum* tab = NULL;
    uint32_t len = 0, i;
    int rc = 0;
    if (JS_IsUndefined(obj) || JS_IsNull(obj))
        return 0;
    if (!JS_IsObject(obj) || JS_IsArray(ctx, obj)) {
        JS_ThrowTypeError(ctx, "%s: headers must be an object of strings", who);
        return -1;
    }
    if (JS_GetOwnPropertyNames(ctx, &tab, &len, obj,
            JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY)
        < 0)
        return -1;
    for (i = 0; i < len && !rc; i++) {
        JSValue v = JS_GetProperty(ctx, obj, tab[i].atom);
        const char *name = JS_IsException(v) ? NULL : JS_AtomToCString(ctx, tab[i].atom), *val = NULL;
        if (!name || !JS_IsString(v)) {
            if (name)
                JS_ThrowTypeError(ctx, "%s: header %s must be a string", who, name);
            rc = -1;
        } else {
            size_t vl;
            val = JS_ToCStringLen(ctx, &vl, v);
            if (!val || httpk_req_header(out, name, strlen(name), val, vl) < 0) {
                if (val)
                    JS_ThrowTypeError(ctx, "%s: invalid header %s", who, name);
                rc = -1;
            }
        }
        JS_FreeCString(ctx, name);
        JS_FreeCString(ctx, val);
        JS_FreeValue(ctx, v);
    }
    JS_FreePropertyEnum(ctx, tab, len);
    return rc;
}

int ai_arr_len(JSContext* ctx, JSValueConst arr, int64_t* n)
{
    JSValue lv = JS_GetPropertyStr(ctx, arr, "length");
    double d = 0;
    int rc = JS_ToFloat64(ctx, &d, lv);
    JS_FreeValue(ctx, lv);
    if (rc < 0)
        return -1;
    *n = (int64_t)d;
    return 0;
}

int ai_betas_join(JSContext* ctx, JSValueConst arr, const char* who,
    dyn_sb_t* out)
{
    int64_t n, i;
    if (JS_IsUndefined(arr) || JS_IsNull(arr))
        return 0;
    if (!JS_IsArray(ctx, arr)) {
        JS_ThrowTypeError(ctx, "%s: betas must be an array of strings", who);
        return -1;
    }
    if (ai_arr_len(ctx, arr, &n) < 0)
        return -1;
    for (i = 0; i < n; i++) {
        JSValue v = JS_GetPropertyUint32(ctx, arr, (uint32_t)i);
        const char* s;
        size_t sl;
        if (JS_IsException(v))
            return -1;
        if (!JS_IsString(v)) {
            JS_FreeValue(ctx, v);
            JS_ThrowTypeError(ctx, "%s: betas must be an array of strings", who);
            return -1;
        }
        s = JS_ToCStringLen(ctx, &sl, v);
        JS_FreeValue(ctx, v);
        if (!s)
            return -1;
        if (!sl || !httpk_value_ok(s, sl) || memchr(s, ',', sl)) {
            JS_FreeCString(ctx, s);
            JS_ThrowTypeError(ctx, "%s: invalid beta name", who);
            return -1;
        }
        if (out->n)
            dyn_sb_putc(out, ',');
        dyn_sb_put(out, s, sl);
        JS_FreeCString(ctx, s);
    }
    return 0;
}

static const char* const ai_client_keys[] = {
    "apiKey", "authToken", "baseURL", "apiVersion", "maxRetries", "timeout",
    "betas", "headers", "ca"
};

static const char* const ai_openai_keys[] = {
    "apiKey", "baseURL", "organization", "project", "maxRetries", "timeout",
    "headers", "ca"
};

const char* ai_client_name(const ai_client_t* cl)
{
    return cl->kind == AI_KIND_OPENAI ? "OpenAI" : "Anthropic";
}

static void ai_client_dispose(void* native)
{
    ai_client_t* cl = (ai_client_t*)native;
    if (!cl)
        return;
    cl->closed = 1;
    ai_client_unref(cl);
}

static int ai_client_define_ns(JSContext* ctx, JSValue obj, ai_client_t* cl);

static JSValue ai_client_new(JSContext* ctx, JSValueConst new_target, int argc,
    JSValueConst* argv, int kind)
{
    JSValueConst opts = argc > 0 ? argv[0] : JS_UNDEFINED;
    int oa = kind == AI_KIND_OPENAI;
    const char* who = oa ? "OpenAI" : "Anthropic";
    ai_client_t* cl;
    JSValue obj, v;
    double d;
    int err = 0, rc;
    dyn_sb_t sb;

    if (oa ? ai_opts_check(ctx, opts, who, ai_openai_keys, (int)countof(ai_openai_keys)) < 0
           : ai_opts_check(ctx, opts, who, ai_client_keys, (int)countof(ai_client_keys)) < 0)
        return JS_EXCEPTION;
    cl = (ai_client_t*)calloc(1, sizeof *cl);
    if (!cl)
        return JS_ThrowOutOfMemory(ctx);
    cl->refs = 1;
    cl->kind = kind;
    cl->timeout_ms = AI_TIMEOUT_MS;
    cl->max_retries = AI_MAX_RETRIES;
    cl->rng = (uint64_t)time(NULL) ^ (uint64_t)(uintptr_t)cl;
    cl->api_key = ai_opt_str(ctx, opts, "apiKey", who, &err);
    if (!err && !oa)
        cl->auth_token = ai_opt_str(ctx, opts, "authToken", who, &err);
    if (!err)
        cl->base_url = ai_opt_str(ctx, opts, "baseURL", who, &err);
    if (!err && !oa)
        cl->version = ai_opt_str(ctx, opts, "apiVersion", who, &err);
    if (!err && oa)
        cl->org = ai_opt_str(ctx, opts, "organization", who, &err);
    if (!err && oa)
        cl->project = ai_opt_str(ctx, opts, "project", who, &err);
    if (!err)
        cl->ca = ai_opt_str(ctx, opts, "ca", who, &err);
    if (err)
        goto fail;
    if (oa) {
        if (!cl->api_key)
            cl->api_key = ai_env_dup("OPENAI_API_KEY");
        if (!cl->org)
            cl->org = ai_env_dup("OPENAI_ORG_ID");
        if (!cl->project)
            cl->project = ai_env_dup("OPENAI_PROJECT_ID");
        if (!cl->base_url)
            cl->base_url = ai_env_dup("OPENAI_BASE_URL");
        if (!cl->base_url)
            cl->base_url = strdup(AI_OPENAI_BASE_URL);
        if (!cl->version)
            cl->version = strdup("");
    } else {
        if (!cl->api_key && !cl->auth_token) {
            cl->api_key = ai_env_dup("ANTHROPIC_API_KEY");
            if (!cl->api_key)
                cl->auth_token = ai_env_dup("ANTHROPIC_AUTH_TOKEN");
        }
        if (!cl->base_url)
            cl->base_url = ai_env_dup("ANTHROPIC_BASE_URL");
        if (!cl->base_url)
            cl->base_url = strdup(AI_ANTHROPIC_BASE_URL);
        if (!cl->version)
            cl->version = strdup(AI_ANTHROPIC_VERSION);
    }
    if (!cl->base_url || !cl->version) {
        JS_ThrowOutOfMemory(ctx);
        goto fail;
    }
    {
        httpk_url_t u;
        size_t bl = strlen(cl->base_url);
        while (bl && cl->base_url[bl - 1] == '/')
            cl->base_url[--bl] = 0;
        if (httpk_url_split(cl->base_url, bl, &u) < 0 || u.query_len) {
            JS_ThrowTypeError(ctx, "%s: baseURL must be an absolute http(s) URL without a query", who);
            goto fail;
        }
    }
    if (!oa && (!httpk_value_ok(cl->version, strlen(cl->version)) || !*cl->version)) {
        JS_ThrowTypeError(ctx, "%s: invalid apiVersion", who);
        goto fail;
    }
    if (JS_IsObject(opts)) {
        rc = ai_opt_num(ctx, opts, "maxRetries", who, 0, AI_MAX_RETRIES_CAP, &d);
        if (rc < 0)
            goto fail;
        if (rc)
            cl->max_retries = (int)d;
        rc = ai_opt_num(ctx, opts, "timeout", who, 1, 86400000.0, &d);
        if (rc < 0)
            goto fail;
        if (rc)
            cl->timeout_ms = (int64_t)d;
        dyn_sb_init(&sb);
        v = oa ? JS_UNDEFINED : JS_GetPropertyStr(ctx, opts, "betas");
        rc = JS_IsException(v) ? -1 : ai_betas_join(ctx, v, who, &sb);
        JS_FreeValue(ctx, v);
        if (rc < 0 || sb.oom) {
            dyn_sb_free(&sb);
            goto fail;
        }
        if (sb.n) {
            dyn_sb_putc(&sb, 0);
            cl->betas = sb.p;
        } else {
            dyn_sb_free(&sb);
        }
        dyn_sb_init(&sb);
        v = JS_GetPropertyStr(ctx, opts, "headers");
        rc = JS_IsException(v) ? -1 : ai_hdr_block(ctx, v, who, &sb);
        JS_FreeValue(ctx, v);
        if (rc < 0 || sb.oom) {
            dyn_sb_free(&sb);
            goto fail;
        }
        if (sb.n) {
            dyn_sb_putc(&sb, 0);
            cl->extra_headers = sb.p;
        } else {
            dyn_sb_free(&sb);
        }
    }
    obj = dyn_res_wrap(ctx, new_target, oa ? ai_openai_class_id : ai_client_class_id, cl,
        ai_client_dispose);
    if (JS_IsException(obj))
        return obj;
    if (ai_client_define_ns(ctx, obj, cl) < 0) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    return obj;
fail:
    ai_client_unref(cl);
    return JS_EXCEPTION;
}

JSValue ai_client_ctor(JSContext* ctx, JSValueConst new_target, int argc,
    JSValueConst* argv)
{
    return ai_client_new(ctx, new_target, argc, argv, AI_KIND_ANTHROPIC);
}

JSValue ai_openai_ctor(JSContext* ctx, JSValueConst new_target, int argc,
    JSValueConst* argv)
{
    return ai_client_new(ctx, new_target, argc, argv, AI_KIND_OPENAI);
}

typedef struct {
    ai_client_t* cl;
    JSValue owner;
} ai_ns_t;

static void ai_ns_finalizer(JSRuntime* rt, JSValue val)
{
    ai_ns_t* n = (ai_ns_t*)JS_GetOpaque(val, ai_ns_class_id);
    if (!n)
        return;
    JS_FreeValueRT(rt, n->owner);
    ai_client_unref(n->cl);
    free(n);
}

static void ai_ns_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark)
{
    ai_ns_t* n = (ai_ns_t*)JS_GetOpaque(val, ai_ns_class_id);
    if (n)
        JS_MarkValue(rt, n->owner, mark);
}

static JSClassDef ai_ns_class = { "AnthropicNamespace", ai_ns_finalizer, ai_ns_mark, NULL, NULL };

ai_client_t* ai_ns_client(JSContext* ctx, JSValueConst this_val)
{
    ai_ns_t* n = (ai_ns_t*)JS_GetOpaque2(ctx, this_val, ai_ns_class_id);
    if (!n)
        return NULL;
    if (n->cl->closed) {
        JS_ThrowTypeError(ctx, "%s: the client is closed", ai_client_name(n->cl));
        return NULL;
    }
    return n->cl;
}

JSValue ai_ns_new(JSContext* ctx, JSValueConst owner, ai_client_t* cl,
    const JSCFunctionListEntry* fns, int n)
{
    JSValue o = JS_NewObjectClass(ctx, (int)ai_ns_class_id);
    ai_ns_t* ns;
    if (JS_IsException(o))
        return o;
    ns = (ai_ns_t*)malloc(sizeof *ns);
    if (!ns) {
        JS_FreeValue(ctx, o);
        return JS_ThrowOutOfMemory(ctx);
    }
    ns->cl = ai_client_ref(cl);
    ns->owner = JS_DupValue(ctx, owner);
    JS_SetOpaque(o, ns);
    JS_SetPropertyFunctionList(ctx, o, fns, n);
    return o;
}

extern const JSCFunctionListEntry ai_messages_fns[];
extern const int ai_messages_nfns;
extern const JSCFunctionListEntry ai_models_fns[];
extern const int ai_models_nfns;
extern const JSCFunctionListEntry ai_batches_fns[];
extern const int ai_batches_nfns;
extern const JSCFunctionListEntry ai_files_fns[];
extern const int ai_files_nfns;

extern const JSCFunctionListEntry ai_completions_fns[];
extern const int ai_completions_nfns;
extern const JSCFunctionListEntry ai_oa_models_fns[];
extern const int ai_oa_models_nfns;
extern const JSCFunctionListEntry ai_embeddings_fns[];
extern const int ai_embeddings_nfns;

static int ai_openai_define_ns(JSContext* ctx, JSValue obj, ai_client_t* cl)
{
    JSValue ch = ai_ns_new(ctx, obj, cl, NULL, 0), cc, md;
    if (JS_IsException(ch))
        return -1;
    cc = ai_ns_new(ctx, obj, cl, ai_completions_fns, ai_completions_nfns);
    if (JS_IsException(cc) || JS_DefinePropertyValueStr(ctx, ch, "completions", cc, JS_PROP_ENUMERABLE) < 0) {
        JS_FreeValue(ctx, ch);
        return -1;
    }
    if (JS_DefinePropertyValueStr(ctx, obj, "chat", ch, JS_PROP_ENUMERABLE) < 0)
        return -1;
    md = ai_ns_new(ctx, obj, cl, ai_oa_models_fns, ai_oa_models_nfns);
    if (JS_IsException(md))
        return -1;
    if (JS_DefinePropertyValueStr(ctx, obj, "models", md, JS_PROP_ENUMERABLE) < 0)
        return -1;
    md = ai_ns_new(ctx, obj, cl, ai_embeddings_fns, ai_embeddings_nfns);
    if (JS_IsException(md))
        return -1;
    return JS_DefinePropertyValueStr(ctx, obj, "embeddings", md, JS_PROP_ENUMERABLE);
}

static int ai_anthropic_define_ns(JSContext* ctx, JSValue obj, ai_client_t* cl)
{
    JSValue ms = ai_ns_new(ctx, obj, cl, ai_messages_fns, ai_messages_nfns), md, bt, fl;
    if (JS_IsException(ms))
        return -1;
    bt = ai_ns_new(ctx, obj, cl, ai_batches_fns, ai_batches_nfns);
    if (JS_IsException(bt) || JS_DefinePropertyValueStr(ctx, ms, "batches", bt, JS_PROP_ENUMERABLE) < 0) {
        JS_FreeValue(ctx, ms);
        return -1;
    }
    if (JS_DefinePropertyValueStr(ctx, obj, "messages", ms, JS_PROP_ENUMERABLE) < 0)
        return -1;
    md = ai_ns_new(ctx, obj, cl, ai_models_fns, ai_models_nfns);
    if (JS_IsException(md))
        return -1;
    if (JS_DefinePropertyValueStr(ctx, obj, "models", md, JS_PROP_ENUMERABLE) < 0)
        return -1;
    fl = ai_ns_new(ctx, obj, cl, ai_files_fns, ai_files_nfns);
    if (JS_IsException(fl))
        return -1;
    return JS_DefinePropertyValueStr(ctx, obj, "files", fl, JS_PROP_ENUMERABLE);
}

static int ai_client_define_ns(JSContext* ctx, JSValue obj, ai_client_t* cl)
{
    if (cl->kind == AI_KIND_OPENAI)
        return ai_openai_define_ns(ctx, obj, cl);
    return ai_anthropic_define_ns(ctx, obj, cl);
}

ai_client_t* ai_client_this(JSContext* ctx, JSValueConst this_val)
{
    JSClassID id = JS_GetClassID(this_val);
    return (ai_client_t*)dyn_res_native(ctx, this_val,
        id == ai_openai_class_id && ai_openai_class_id ? ai_openai_class_id : ai_client_class_id);
}

static JSValue ai_client_get_base(JSContext* ctx, JSValueConst this_val)
{
    ai_client_t* cl = ai_client_this(ctx, this_val);
    return cl ? JS_NewString(ctx, cl->base_url) : JS_EXCEPTION;
}

static JSValue ai_client_get_version(JSContext* ctx, JSValueConst this_val)
{
    ai_client_t* cl = ai_client_this(ctx, this_val);
    return cl ? JS_NewString(ctx, cl->version) : JS_EXCEPTION;
}

static JSValue ai_client_get_retries(JSContext* ctx, JSValueConst this_val)
{
    ai_client_t* cl = ai_client_this(ctx, this_val);
    return cl ? JS_NewInt32(ctx, cl->max_retries) : JS_EXCEPTION;
}

static JSValue ai_client_get_timeout(JSContext* ctx, JSValueConst this_val)
{
    ai_client_t* cl = ai_client_this(ctx, this_val);
    return cl ? JS_NewFloat64(ctx, (double)cl->timeout_ms) : JS_EXCEPTION;
}

static const JSCFunctionListEntry ai_client_proto[] = {
    JS_CGETSET_DEF("baseURL", ai_client_get_base, NULL),
    JS_CGETSET_DEF("apiVersion", ai_client_get_version, NULL),
    JS_CGETSET_DEF("maxRetries", ai_client_get_retries, NULL),
    JS_CGETSET_DEF("timeout", ai_client_get_timeout, NULL),
};

static JSClassDef ai_client_class = { "Anthropic", dyn_res_finalizer, NULL, NULL, NULL };

static const JSCFunctionListEntry ai_openai_proto[] = {
    JS_CGETSET_DEF("baseURL", ai_client_get_base, NULL),
    JS_CGETSET_DEF("maxRetries", ai_client_get_retries, NULL),
    JS_CGETSET_DEF("timeout", ai_client_get_timeout, NULL),
};

static JSClassDef ai_openai_class = { "OpenAI", dyn_res_finalizer, NULL, NULL, NULL };

JSClassID ai_openai_class_id;

int ai_openai_register(JSContext* ctx, JSModuleDef* m)
{
    return dyn_register_class(ctx, m, &ai_openai_class_id, &ai_openai_class,
        ai_openai_proto, (int)countof(ai_openai_proto),
        (JSCFunction*)ai_openai_ctor, "OpenAI");
}

int ai_anthropic_register(JSContext* ctx, JSModuleDef* m)
{
    JSRuntime* rt = JS_GetRuntime(ctx);
    if (!ai_ns_class_id)
        JS_NewClassID(&ai_ns_class_id);
    if (!JS_IsRegisteredClass(rt, ai_ns_class_id)
        && JS_NewClass(rt, ai_ns_class_id, &ai_ns_class) < 0)
        return -1;
    return dyn_register_class(ctx, m, &ai_client_class_id, &ai_client_class,
        ai_client_proto, (int)countof(ai_client_proto),
        (JSCFunction*)ai_client_ctor, "Anthropic");
}
#endif
