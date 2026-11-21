#include "ai-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_AI) && defined(CONFIG_NATIVE_MODULE_HTTP)

static JSValue ai_reject(JSContext* ctx)
{
    return hc_promise_rejected(ctx, JS_EXCEPTION);
}

static const char* const ai_upload_keys[] = { "data", "filename", "mimeType", "expiresInSeconds" };

static int ai_filename_ok(const char* s, size_t n)
{
    size_t i, chars = 0;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 32 || strchr("<>:\"|?*\\/", (int)c))
            return 0;
        if ((c & 0xC0) != 0x80)
            chars++;
    }
    return chars >= 1 && chars <= AI_FILENAME_MAX;
}

static uint64_t ai_mix(uint64_t* s)
{
    uint64_t z = (*s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

static const void* ai_memmem(const uint8_t* h, size_t hn, const char* n, size_t nn)
{
    const uint8_t* p = h;
    const uint8_t* end = h + hn;
    while ((size_t)(end - p) >= nn) {
        const uint8_t* q = (const uint8_t*)memchr(p, (unsigned char)n[0], (size_t)(end - p) - nn + 1);
        if (!q)
            return NULL;
        if (memcmp(q, n, nn) == 0)
            return q;
        p = q + 1;
    }
    return NULL;
}

static int ai_multipart_boundary(uint64_t* rng, const uint8_t* data, size_t n,
    char out[AI_BOUNDARY_LEN + 1])
{
    static const char hex[] = "0123456789abcdef";
    int tries, i;
    for (tries = 0; tries < 16; tries++) {
        uint64_t a = ai_mix(rng), b = ai_mix(rng);
        memcpy(out, "dynajs-", 7);
        for (i = 0; i < 16; i++)
            out[7 + i] = hex[(a >> (i * 4)) & 15];
        for (i = 0; i < 16; i++)
            out[23 + i] = hex[(b >> (i * 4)) & 15];
        out[AI_BOUNDARY_LEN] = 0;
        if (!ai_memmem(data, n, out, AI_BOUNDARY_LEN))
            return 0;
    }
    return -1;
}

static char* ai_upload_str(JSContext* ctx, JSValueConst v, size_t* plen)
{
    size_t n = 0;
    const char* s = JS_ToCStringLen(ctx, &n, v);
    char* d;
    if (!s)
        return NULL;
    d = (char*)malloc(n + 1);
    if (d) {
        memcpy(d, s, n);
        d[n] = 0;
        *plen = n;
    } else {
        JS_ThrowOutOfMemory(ctx);
    }
    JS_FreeCString(ctx, s);
    return d;
}

static JSValue ai_files_upload(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    ai_client_t* cl = ai_ns_client(ctx, this_val);
    JSValueConst a = argc > 0 ? argv[0] : JS_UNDEFINED;
    JSValue data = JS_UNDEFINED, fn = JS_UNDEFINED, mt = JS_UNDEFINED, ex = JS_UNDEFINED;
    const uint8_t* bytes = NULL;
    const char* sdata = NULL;
    char* name = NULL;
    char* mime = NULL;
    size_t blen = 0, nlen = 0, mlen = 0;
    int64_t expires = 0;
    int has_exp = 0;
    char bnd[AI_BOUNDARY_LEN + 1];
    dyn_sb_t body, ct;
    ai_req_t* r = NULL;
    if (!cl)
        return ai_reject(ctx);
    dyn_sb_init(&body);
    dyn_sb_init(&ct);
    if (!JS_IsObject(a) || JS_IsArray(ctx, a) || JS_IsFunction(ctx, a)) {
        JS_ThrowTypeError(ctx, "files.upload({ data, filename, mimeType?, expiresInSeconds? })");
        goto fail;
    }
    if (ai_opts_check(ctx, a, "files.upload", ai_upload_keys, (int)countof(ai_upload_keys)) < 0)
        goto fail;
    data = JS_GetPropertyStr(ctx, a, "data");
    fn = JS_GetPropertyStr(ctx, a, "filename");
    mt = JS_GetPropertyStr(ctx, a, "mimeType");
    ex = JS_GetPropertyStr(ctx, a, "expiresInSeconds");
    if (JS_IsException(data) || JS_IsException(fn) || JS_IsException(mt) || JS_IsException(ex))
        goto fail;
    if (!JS_IsString(fn)) {
        JS_ThrowTypeError(ctx, "files.upload: filename must be a string");
        goto fail;
    }
    name = ai_upload_str(ctx, fn, &nlen);
    if (!name)
        goto fail;
    if (!ai_filename_ok(name, nlen)) {
        JS_ThrowTypeError(ctx, "files.upload: filename must be 1..255 characters with none of < > : \" | ? * \\ / or control characters 0-31");
        goto fail;
    }
    if (!JS_IsUndefined(mt)) {
        if (!JS_IsString(mt)) {
            JS_ThrowTypeError(ctx, "files.upload: mimeType must be a string");
            goto fail;
        }
        mime = ai_upload_str(ctx, mt, &mlen);
        if (!mime)
            goto fail;
        if (!mlen || !httpk_value_ok(mime, mlen)) {
            JS_ThrowTypeError(ctx, "files.upload: mimeType must be a non-empty header-safe string");
            goto fail;
        }
    }
    if (!JS_IsUndefined(ex)) {
        double d;
        if (!JS_IsNumber(ex) || JS_ToFloat64(ctx, &d, ex) < 0 || d != (double)(int64_t)d) {
            if (!JS_HasException(ctx))
                JS_ThrowTypeError(ctx, "files.upload: expiresInSeconds must be an integer");
            goto fail;
        }
        if (d < AI_FILE_EXPIRES_MIN || d > AI_FILE_EXPIRES_MAX) {
            JS_ThrowRangeError(ctx, "files.upload: expiresInSeconds must be %d..%d", AI_FILE_EXPIRES_MIN, AI_FILE_EXPIRES_MAX);
            goto fail;
        }
        expires = (int64_t)d;
        has_exp = 1;
    }
    if (JS_IsString(data)) {
        sdata = JS_ToCStringLen(ctx, &blen, data);
        if (!sdata)
            goto fail;
        bytes = (const uint8_t*)sdata;
    } else if (JS_GetBufferKind(data) != JS_BUFFER_KIND_NONE) {
        bytes = hc_view_bytes(ctx, data, &blen, "files.upload");
        if (!bytes)
            goto fail;
    } else {
        JS_ThrowTypeError(ctx, "files.upload: data must be a Uint8Array, ArrayBuffer or string");
        goto fail;
    }
    if (ai_multipart_boundary(&cl->rng, bytes, blen, bnd) < 0) {
        JS_ThrowTypeError(ctx, "files.upload: could not pick a multipart boundary absent from the data");
        goto fail;
    }
    dyn_sb_puts(&body, "--");
    dyn_sb_puts(&body, bnd);
    dyn_sb_puts(&body, "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"");
    dyn_sb_put(&body, name, nlen);
    dyn_sb_puts(&body, "\"\r\n");
    if (mime) {
        dyn_sb_puts(&body, "Content-Type: ");
        dyn_sb_put(&body, mime, mlen);
        dyn_sb_puts(&body, "\r\n");
    }
    dyn_sb_puts(&body, "\r\n");
    dyn_sb_put(&body, (const char*)bytes, blen);
    dyn_sb_puts(&body, "\r\n");
    if (has_exp) {
        char num[32];
        snprintf(num, sizeof num, "%lld", (long long)expires);
        dyn_sb_puts(&body, "--");
        dyn_sb_puts(&body, bnd);
        dyn_sb_puts(&body, "\r\nContent-Disposition: form-data; name=\"expires_in_seconds\"\r\n\r\n");
        dyn_sb_puts(&body, num);
        dyn_sb_puts(&body, "\r\n");
    }
    dyn_sb_puts(&body, "--");
    dyn_sb_puts(&body, bnd);
    dyn_sb_puts(&body, "--\r\n");
    dyn_sb_puts(&ct, "multipart/form-data; boundary=");
    dyn_sb_puts(&ct, bnd);
    dyn_sb_putc(&ct, 0);
    if (body.oom || ct.oom) {
        JS_ThrowOutOfMemory(ctx);
        goto fail;
    }
    if (sdata) {
        JS_FreeCString(ctx, sdata);
        sdata = NULL;
    }
    r = ai_req_new_raw(ctx, cl, "POST", "/v1/files", JS_UNDEFINED,
        (const uint8_t*)(body.p ? body.p : ""), body.n, ct.p,
        argc > 1 ? argv[1] : JS_UNDEFINED, AI_OUT_JSON);
fail:
    if (sdata)
        JS_FreeCString(ctx, sdata);
    free(name);
    free(mime);
    dyn_sb_free(&body);
    dyn_sb_free(&ct);
    JS_FreeValue(ctx, data);
    JS_FreeValue(ctx, fn);
    JS_FreeValue(ctx, mt);
    JS_FreeValue(ctx, ex);
    if (!r)
        return ai_reject(ctx);
    return ai_req_start(ctx, r);
}

static JSValue ai_files_query(JSContext* ctx, JSValueConst q)
{
    JSPropertyEnum* tab = NULL;
    uint32_t len = 0, i;
    JSValue o;
    if (JS_IsUndefined(q) || JS_IsNull(q))
        return JS_UNDEFINED;
    if (!JS_IsObject(q) || JS_IsArray(ctx, q))
        return JS_ThrowTypeError(ctx, "files.list: query parameters must be an object");
    o = JS_NewObject(ctx);
    if (JS_IsException(o))
        return o;
    if (JS_GetOwnPropertyNames(ctx, &tab, &len, q, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0) {
        JS_FreeValue(ctx, o);
        return JS_EXCEPTION;
    }
    for (i = 0; i < len; i++) {
        const char* k = JS_AtomToCString(ctx, tab[i].atom);
        JSValue v;
        int rc;
        if (!k)
            goto fail;
        v = JS_GetProperty(ctx, q, tab[i].atom);
        if (JS_IsException(v)) {
            JS_FreeCString(ctx, k);
            goto fail;
        }
        if (strcmp(k, "ids") == 0) {
            int64_t n = 0;
            if (!JS_IsArray(ctx, v) || ai_arr_len(ctx, v, &n) < 0 || n > AI_FILES_IDS_MAX) {
                JS_FreeValue(ctx, v);
                JS_FreeCString(ctx, k);
                if (!JS_HasException(ctx))
                    JS_ThrowRangeError(ctx, "files.list: ids must be an array of at most %d file ids", AI_FILES_IDS_MAX);
                goto fail;
            }
            rc = JS_SetPropertyStr(ctx, o, "ids[]", v);
        } else {
            rc = JS_SetProperty(ctx, o, tab[i].atom, v);
        }
        JS_FreeCString(ctx, k);
        if (rc < 0)
            goto fail;
    }
    JS_FreePropertyEnum(ctx, tab, len);
    return o;
fail:
    JS_FreePropertyEnum(ctx, tab, len);
    JS_FreeValue(ctx, o);
    return JS_EXCEPTION;
}

static JSValue ai_files_list(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    ai_client_t* cl = ai_ns_client(ctx, this_val);
    JSValue q;
    ai_req_t* r;
    if (!cl)
        return ai_reject(ctx);
    q = ai_files_query(ctx, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (JS_IsException(q))
        return ai_reject(ctx);
    r = ai_req_new(ctx, cl, "GET", "/v1/files", q, JS_UNDEFINED,
        argc > 1 ? argv[1] : JS_UNDEFINED, AI_OUT_JSON);
    JS_FreeValue(ctx, q);
    if (!r)
        return ai_reject(ctx);
    return ai_req_start(ctx, r);
}

static JSValue ai_files_by_id(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv, const char* who, const char* method, const char* post,
    int out)
{
    ai_client_t* cl = ai_ns_client(ctx, this_val);
    dyn_sb_t path;
    ai_req_t* r;
    if (!cl)
        return ai_reject(ctx);
    dyn_sb_init(&path);
    if (ai_id_path(ctx, who, "/v1/files/", argc > 0 ? argv[0] : JS_UNDEFINED, post, &path) < 0) {
        dyn_sb_free(&path);
        return ai_reject(ctx);
    }
    r = ai_req_new(ctx, cl, method, path.p, JS_UNDEFINED, JS_UNDEFINED,
        argc > 1 ? argv[1] : JS_UNDEFINED, out);
    dyn_sb_free(&path);
    if (!r)
        return ai_reject(ctx);
    return ai_req_start(ctx, r);
}

static JSValue ai_files_meta(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    return ai_files_by_id(ctx, this_val, argc, argv, "files.retrieveMetadata", "GET", NULL, AI_OUT_JSON);
}

static JSValue ai_files_download(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    return ai_files_by_id(ctx, this_val, argc, argv, "files.download", "GET", "/content", AI_OUT_BYTES);
}

static JSValue ai_files_delete(JSContext* ctx, JSValueConst this_val, int argc,
    JSValueConst* argv)
{
    return ai_files_by_id(ctx, this_val, argc, argv, "files.delete", "DELETE", NULL, AI_OUT_JSON);
}

const JSCFunctionListEntry ai_files_fns[] = {
    JS_CFUNC_DEF("upload", 2, ai_files_upload),
    JS_CFUNC_DEF("list", 2, ai_files_list),
    JS_CFUNC_DEF("retrieveMetadata", 2, ai_files_meta),
    JS_CFUNC_DEF("download", 2, ai_files_download),
    JS_CFUNC_DEF("delete", 2, ai_files_delete),
};
const int ai_files_nfns = (int)countof(ai_files_fns);
#endif
