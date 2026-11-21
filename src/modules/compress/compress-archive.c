#include "compress-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_COMPRESS)

typedef struct {
    JSAtom name, data, method, mtime, mode, comment;
} zip_key_atoms_t;

static int ar_opt_unsafe(JSContext* ctx, JSValueConst o, int* out)
{
    static const char* const ar_unsafe_keys[] = { "allowUnsafeNames" };
    JSValue v;

    *out = 0;
    if (!JS_IsObject(o))
        return 0;
    if (dyn_opts_strict(ctx, o, ar_unsafe_keys, 1) < 0)
        return -1;
    v = JS_GetPropertyStr(ctx, o, "allowUnsafeNames");
    if (JS_IsException(v))
        return -1;
    if (!JS_IsUndefined(v)) {
        *out = JS_ToBool(ctx, v);
        if (*out < 0) {
            JS_FreeValue(ctx, v);
            return -1;
        }
    }
    JS_FreeValue(ctx, v);
    return 0;
}

static JSValue ar_entry_obj(JSContext* ctx, const char* name, size_t nlen,
    uint64_t size, uint64_t mtime, uint64_t mode,
    const char* type, const char* link)
{
    JSValue e = JS_NewObject(ctx);

    if (JS_IsException(e))
        return e;
    JS_DefinePropertyValueStr(ctx, e, "name", JS_NewStringLen(ctx, name, nlen), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, e, "size", JS_NewInt64(ctx, (int64_t)size), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, e, "mtime", JS_NewInt64(ctx, (int64_t)mtime), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, e, "mode", JS_NewInt64(ctx, (int64_t)mode), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, e, "type", JS_NewString(ctx, type), JS_PROP_C_W_E);
    if (link && link[0])
        JS_DefinePropertyValueStr(ctx, e, "linkname", JS_NewString(ctx, link), JS_PROP_C_W_E);
    return e;
}

static const char* tar_type_name(char t)
{
    switch (t) {
    case '5':
        return "directory";
    case '2':
        return "symlink";
    case '1':
        return "link";
    case '3':
    case '4':
        return "device";
    case '6':
        return "fifo";
    default:
        return "file";
    }
}

JSValue dyn_tar_read(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    uint8_t* src = NULL;
    size_t src_len = 0;
    tar_rd_t r;
    tar_ent_t e;
    JSValue out;
    uint32_t k = 0;
    int rc;

    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "%s(bytes): an archive is required",
            magic ? "TarExtract" : "TarList");
    if (dyn_read_input(ctx, argv[0], &src, &src_len) < 0)
        return JS_EXCEPTION;
    memset(&r, 0, sizeof r);
    r.ud = ctx;
    r.p = src;
    r.n = src_len;
    if (ar_opt_unsafe(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, &r.allow_unsafe) < 0) {
        free(src);
        return JS_EXCEPTION;
    }
    out = JS_NewArray(ctx);
    if (JS_IsException(out)) {
        free(src);
        return out;
    }
    for (;;) {
        JSValue eo;
        memset(&e, 0, sizeof e);
        rc = arck_tar_next(&r, &e);
        if (rc <= 0)
            break;
        if (k >= AR_MAX_ENTRIES) {
            snprintf(r.err, sizeof r.err, "archive holds more than %u entries",
                AR_MAX_ENTRIES);
            rc = -1;
            break;
        }
        eo = ar_entry_obj(ctx, e.name, e.nlen, e.size, e.mtime, e.mode,
            tar_type_name(e.type), e.link);
        if (JS_IsException(eo)) {
            rc = -1;
            break;
        }
        if (magic == 1 && e.type != '5')
            JS_DefinePropertyValueStr(ctx, eo, "data",
                dyn_bytes_to_uint8(ctx, src + e.body, (size_t)e.size), JS_PROP_C_W_E);
        if (JS_DefinePropertyValueUint32(ctx, out, k++, eo, JS_PROP_C_W_E) < 0) {
            rc = -1;
            break;
        }
    }
    free(src);
    if (rc < 0) {
        JS_FreeValue(ctx, out);
        return JS_ThrowSyntaxError(ctx, "%s: %s", magic ? "TarExtract" : "TarList",
            r.err[0] ? r.err : "malformed archive");
    }
    return out;
}

static int tar_emit_header(JSContext* ctx, ab_t* b, const char* name,
    size_t nlen, uint64_t size, uint64_t mtime,
    uint64_t mode, char type)
{
    uint8_t h[TAR_BLOCK];
    size_t split = 0;

    if (size > 077777777777ULL) {
        JS_ThrowRangeError(ctx, "TarPack: entry is larger than ustar's "
                                "11-octal-digit size field (%llu bytes); this "
                                "writer emits no PAX records",
            (unsigned long long)size);
        return -1;
    }
    if (mtime > 077777777777ULL) {
        JS_ThrowRangeError(ctx, "TarPack: mtime %llu does not fit ustar's "
                                "11-octal-digit field (max 8589934591); this "
                                "writer emits no PAX records",
            (unsigned long long)mtime);
        return -1;
    }

    memset(h, 0, sizeof h);
    if (nlen <= 100) {
        memcpy(h, name, nlen);
    } else {
        size_t i;
        for (i = nlen > 101 ? nlen - 101 : 1; i < nlen && i <= 155; i++)
            if (name[i] == '/') {
                split = i;
                break;
            }
        if (!split || nlen - split - 1 > 100 || split > 155) {
            JS_ThrowRangeError(ctx, "TarPack: the name is too long for ustar "
                                    "and this writer emits no PAX records: %.60s",
                name);
            return -1;
        }
        memcpy(h + 345, name, split);
        memcpy(h, name + split + 1, nlen - split - 1);
    }
    arck_tar_put_octal(h + 100, 8, mode & 07777);
    arck_tar_put_octal(h + 108, 8, 0);
    arck_tar_put_octal(h + 116, 8, 0);
    arck_tar_put_octal(h + 124, 12, size);
    arck_tar_put_octal(h + 136, 12, mtime);
    h[156] = (uint8_t)type;
    memcpy(h + 257, "ustar", 5);
    h[263] = '0';
    h[264] = '0';
    {
        uint32_t sum = arck_tar_checksum(h);
        int k;
        for (k = 5; k >= 0; k--) {
            h[148 + k] = (uint8_t)('0' + (sum & 7));
            sum >>= 3;
        }
        h[154] = 0;
        h[155] = ' ';
    }
    arck_ab_write(b, h, TAR_BLOCK);
    return 0;
}

static int ar_entry_field(JSContext* ctx, JSValueConst e, const char* key,
    uint64_t* out, uint64_t dflt)
{
    JSValue v = JS_GetPropertyStr(ctx, e, key);
    int64_t t;

    *out = dflt;
    if (JS_IsException(v))
        return -1;
    if (JS_IsUndefined(v) || JS_IsNull(v)) {
        JS_FreeValue(ctx, v);
        return 0;
    }
    if (JS_ToInt64(ctx, &t, v) < 0) {
        JS_FreeValue(ctx, v);
        return -1;
    }
    JS_FreeValue(ctx, v);
    if (t < 0) {
        JS_ThrowRangeError(ctx, "TarPack: %s must not be negative", key);
        return -1;
    }
    *out = (uint64_t)t;
    return 0;
}

static int tar_take_entry(JSContext* ctx, JSValueConst arr, uint32_t i,
    char** name, size_t* nlen, uint8_t** data,
    size_t* dlen, uint64_t* mode, uint64_t* mtime,
    int* isdir, char* ptypeflag)
{
    JSValue e = JS_GetPropertyUint32(ctx, arr, i), nv, dv, tv;
    const char* s;

    *name = NULL;
    *data = NULL;
    *ptypeflag = 0;
    if (JS_IsException(e))
        return -1;
    if (!JS_IsObject(e)) {
        JS_FreeValue(ctx, e);
        JS_ThrowTypeError(ctx, "TarPack: every entry is an object");
        return -1;
    }
    tv = JS_GetPropertyStr(ctx, e, "type");
    if (JS_IsException(tv)) {
        JS_FreeValue(ctx, e);
        return -1;
    }
    if (!JS_IsUndefined(tv) && !JS_IsNull(tv)) {
        const char* t = JS_ToCString(ctx, tv);
        JS_FreeValue(ctx, tv);
        if (!t) {
            JS_FreeValue(ctx, e);
            return -1;
        }
        if (!strcmp(t, "file"))
            *ptypeflag = '0';
        else if (!strcmp(t, "directory"))
            *ptypeflag = '5';
        else {
            JS_ThrowRangeError(ctx, "TarPack: entry type is \"file\" or "
                                    "\"directory\" (this writer carries no "
                                    "link metadata), not \"%.40s\"",
                t);
            JS_FreeCString(ctx, t);
            JS_FreeValue(ctx, e);
            return -1;
        }
        JS_FreeCString(ctx, t);
    } else {
        JS_FreeValue(ctx, tv);
    }
    nv = JS_GetPropertyStr(ctx, e, "name");
    s = JS_IsException(nv) ? NULL : JS_ToCStringLen(ctx, nlen, nv);
    JS_FreeValue(ctx, nv);
    if (!s) {
        JS_FreeValue(ctx, e);
        return -1;
    }
    if (!arck_ar_name_safe(s, *nlen)) {
        JS_ThrowRangeError(ctx, "TarPack: %.60s is not a safe archive name", s);
        JS_FreeCString(ctx, s);
        JS_FreeValue(ctx, e);
        return -1;
    }
    *name = (char*)malloc(*nlen + 1);
    if (*name) {
        memcpy(*name, s, *nlen);
        (*name)[*nlen] = 0;
    }
    JS_FreeCString(ctx, s);
    if (!*name) {
        JS_FreeValue(ctx, e);
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    dv = JS_GetPropertyStr(ctx, e, "data");
    *isdir = JS_IsUndefined(dv) || JS_IsNull(dv);
    if ((!*isdir && dyn_read_input(ctx, dv, data, dlen) < 0)
        || ar_entry_field(ctx, e, "mode", mode, *isdir ? 0755 : 0644) < 0
        || ar_entry_field(ctx, e, "mtime", mtime, 0) < 0) {
        JS_FreeValue(ctx, dv);
        JS_FreeValue(ctx, e);
        free(*name);
        free(*data);
        *name = NULL;
        *data = NULL;
        return -1;
    }
    JS_FreeValue(ctx, dv);
    JS_FreeValue(ctx, e);
    return 0;
}

JSValue dyn_tar_pack(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    ab_t b;
    JSValue lv, out;
    int64_t n = 0, i;

    (void)this_val;
    if (argc < 1 || JS_IsArray(ctx, argv[0]) != 1)
        return JS_ThrowTypeError(ctx, "TarPack(entries): entries is an array");
    lv = JS_GetPropertyStr(ctx, argv[0], "length");
    if (JS_IsException(lv) || JS_ToInt64(ctx, &n, lv) < 0) {
        JS_FreeValue(ctx, lv);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, lv);
    if (n < 0 || n > AR_MAX_ENTRIES)
        return JS_ThrowRangeError(ctx, "TarPack: at most %u entries", AR_MAX_ENTRIES);
    arck_ab_init(&b);
    for (i = 0; i < n; i++) {
        char* name;
        uint8_t* data;
        size_t nlen = 0, dlen = 0;
        uint64_t mode, mtime;
        int isdir, rc;
        char typeflag;

        if (tar_take_entry(ctx, argv[0], (uint32_t)i, &name, &nlen, &data,
                &dlen, &mode, &mtime, &isdir, &typeflag)
            < 0)
            goto fail;
        isdir = typeflag == '5' || (typeflag == 0 && isdir);
        if (isdir && data) {
            free(data);
            data = NULL;
            dlen = 0;
        }
        rc = tar_emit_header(ctx, &b, name, nlen, isdir ? 0 : dlen, mtime, mode,
            isdir ? '5' : '0');
        free(name);
        if (rc < 0) {
            free(data);
            goto fail;
        }
        if (dlen) {
            arck_ab_write(&b, data, dlen);
            arck_ab_zero(&b, (TAR_BLOCK - dlen % TAR_BLOCK) % TAR_BLOCK);
        }
        free(data);
        if (b.oom) {
            JS_ThrowOutOfMemory(ctx);
            goto fail;
        }
    }
    arck_ab_zero(&b, TAR_BLOCK * 2);
    if (b.oom) {
        JS_ThrowOutOfMemory(ctx);
        goto fail;
    }
    out = dyn_bytes_to_uint8(ctx, b.p, b.n);
    arck_ab_free(&b);
    return out;
fail:
    arck_ab_free(&b);
    return JS_EXCEPTION;
}

static JSValue zip_entry_obj(JSContext* ctx, const zip_ent_t* e)
{
    JSValue eo = ar_entry_obj(ctx, e->name, e->nlen, e->usize,
        (uint64_t)arck_zip_unix_from_dos(e->mtime), e->mode,
        e->nlen && e->name[e->nlen - 1] == '/'
            ? "directory"
            : "file",
        NULL);
    if (JS_IsException(eo))
        return eo;
    JS_DefinePropertyValueStr(ctx, eo, "compressedSize", JS_NewInt64(ctx, (int64_t)e->csize), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, eo, "crc32", JS_NewUint32(ctx, e->crc), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, eo, "method",
        JS_NewString(ctx, e->method == 0 ? "store" : e->method == 8 ? "deflate"
                                                                    : "other"),
        JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, eo, "comment",
        JS_NewStringLen(ctx, e->comment, e->clen), JS_PROP_C_W_E);
    return eo;
}

static int zip_walk_list(zip_rd_t* z, JSValue out)
{
    size_t off = z->cd_off;
    zip_ent_t e;
    uint32_t k = 0;

    for (;;) {
        JSValue eo;
        int r;
        if (k >= z->count) {
            if (off + 46 <= z->cd_off + z->cd_size
                && dyn_ld32le(z->p + off) == 0x02014b50) {
                snprintf(z->err, sizeof z->err,
                    "the central directory holds more than the %u entries "
                    "the archive declares",
                    z->count);
                return -1;
            }
            return 0;
        }
        r = arck_zip_entry_at(z, &off, &e);
        if (r <= 0)
            return r;
        eo = zip_entry_obj(z->ud, &e);
        if (JS_IsException(eo)
            || JS_DefinePropertyValueUint32(z->ud, out, k++, eo, JS_PROP_C_W_E) < 0)
            return -2;
    }
}

static int zip_walk_find(zip_rd_t* z, const char* want, size_t wlen, JSValue* out)
{
    size_t off = z->cd_off;
    zip_ent_t e;

    for (;;) {
        int r = arck_zip_entry_at(z, &off, &e);
        if (r < 0)
            return -1;
        if (r == 0) {
            snprintf(z->err, sizeof z->err, "no member named %.60s", want);
            return -1;
        }
        if (e.nlen == wlen && memcmp(e.name, want, wlen) == 0) {
            dyn_outbuf_t o = { NULL, 0, 0 };
            if (arck_zip_member(z, &e, &o) < 0) {
                free(o.buf);
                return -1;
            }
            *out = dyn_bytes_to_uint8(z->ud, o.buf, o.len);
            free(o.buf);
            return JS_IsException(*out) ? -2 : 0;
        }
    }
}

JSValue dyn_zip_read(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    uint8_t* src = NULL;
    size_t src_len = 0, wlen = 0;
    zip_rd_t z;
    JSValue out = JS_UNDEFINED;
    const char* want = NULL;
    int rc;

    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "%s(bytes): an archive is required",
            magic ? "ZipRead" : "ZipList");
    if (magic == 1 && (argc < 2 || !JS_IsString(argv[1])))
        return JS_ThrowTypeError(ctx, "ZipRead(bytes, name): a name is required");
    if (dyn_read_input(ctx, argv[0], &src, &src_len) < 0)
        return JS_EXCEPTION;
    memset(&z, 0, sizeof z);
    z.ud = ctx;
    z.p = src;
    z.n = src_len;
    if (ar_opt_unsafe(ctx, argc > (magic ? 2 : 1) ? argv[magic ? 2 : 1] : JS_UNDEFINED,
            &z.allow_unsafe)
        < 0) {
        free(src);
        return JS_EXCEPTION;
    }
    if (magic == 1) {
        want = JS_ToCStringLen(ctx, &wlen, argv[1]);
        if (!want) {
            free(src);
            return JS_EXCEPTION;
        }
    }
    rc = arck_zip_open(&z);
    if (rc == 0 && magic == 0) {
        out = JS_NewArray(ctx);
        rc = JS_IsException(out) ? -2 : zip_walk_list(&z, out);
    } else if (rc == 0) {
        rc = zip_walk_find(&z, want, wlen, &out);
    }
    if (want)
        JS_FreeCString(ctx, want);
    free(src);
    if (rc != 0) {
        JS_FreeValue(ctx, out);
        if (rc == -2)
            return JS_EXCEPTION;
        return JS_ThrowSyntaxError(ctx, "%s: %s", magic ? "ZipRead" : "ZipList",
            z.err[0] ? z.err : "malformed archive");
    }
    return out;
}

static int zip_entry_opt_method(JSContext* ctx, JSValueConst v, int pack_store,
    int* is_store)
{
    const char* s;

    if (JS_IsUndefined(v) || JS_IsNull(v)) {
        *is_store = pack_store;
        return 0;
    }
    if (!JS_IsString(v)) {
        JS_ThrowTypeError(ctx, "ZipPack: entry method must be a string");
        return -1;
    }
    s = JS_ToCString(ctx, v);
    if (!s)
        return -1;
    if (!strcmp(s, "store"))
        *is_store = 1;
    else if (!strcmp(s, "deflate"))
        *is_store = 0;
    else {
        JS_ThrowRangeError(ctx, "ZipPack: entry method is \"deflate\" or \"store\"");
        JS_FreeCString(ctx, s);
        return -1;
    }
    JS_FreeCString(ctx, s);
    return 0;
}

static int zip_entry_opt_mtime(JSContext* ctx, JSValueConst v, uint32_t* dos)
{
    int64_t t;

    *dos = 0;
    if (JS_IsUndefined(v) || JS_IsNull(v))
        return 0;
    if (JS_ToInt64(ctx, &t, v) < 0)
        return -1;
    if (t < 0) {
        JS_ThrowRangeError(ctx, "ZipPack: entry mtime must not be negative");
        return -1;
    }
    *dos = arck_zip_dos_from_unix(t);
    return 0;
}

static int zip_entry_opt_mode(JSContext* ctx, JSValueConst v, uint32_t* mode,
    int isdir)
{
    int64_t t;

    *mode = 0;
    if (JS_IsUndefined(v) || JS_IsNull(v)) {
        *mode = (uint32_t)(isdir ? 0755 : 0644);
        return 0;
    }
    if (JS_ToInt64(ctx, &t, v) < 0)
        return -1;
    if (t < 0 || t > 0xFFFF) {
        JS_ThrowRangeError(ctx, "ZipPack: entry mode must be 0..0xFFFF");
        return -1;
    }
    *mode = (uint32_t)t;
    return 0;
}

static int zip_entry_opt_string(JSContext* ctx, JSValueConst v,
    char** out, size_t* outlen)
{
    const char* s;
    size_t n = 0;

    *out = NULL;
    if (outlen)
        *outlen = 0;
    if (JS_IsUndefined(v) || JS_IsNull(v))
        return 0;
    if (!JS_IsString(v)) {
        JS_ThrowTypeError(ctx, "ZipPack: entry \"comment\" must be a string");
        return -1;
    }
    s = JS_ToCStringLen(ctx, &n, v);
    if (!s)
        return -1;
    *out = (char*)malloc(n + 1);
    if (!*out) {
        JS_FreeCString(ctx, s);
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    memcpy(*out, s, n);
    (*out)[n] = 0;
    if (outlen)
        *outlen = n;
    JS_FreeCString(ctx, s);
    return 0;
}

static int zip_key_atoms_init(JSContext* ctx, zip_key_atoms_t* A)
{
    A->name = JS_NewAtom(ctx, "name");
    A->data = JS_NewAtom(ctx, "data");
    A->method = JS_NewAtom(ctx, "method");
    A->mtime = JS_NewAtom(ctx, "mtime");
    A->mode = JS_NewAtom(ctx, "mode");
    A->comment = JS_NewAtom(ctx, "comment");
    if (A->name == JS_ATOM_NULL || A->data == JS_ATOM_NULL || A->method == JS_ATOM_NULL || A->mtime == JS_ATOM_NULL || A->mode == JS_ATOM_NULL || A->comment == JS_ATOM_NULL)
        return -1;
    return 0;
}

static void zip_key_atoms_free(JSContext* ctx, zip_key_atoms_t* A)
{
    JS_FreeAtom(ctx, A->name);
    JS_FreeAtom(ctx, A->data);
    JS_FreeAtom(ctx, A->method);
    JS_FreeAtom(ctx, A->mtime);
    JS_FreeAtom(ctx, A->mode);
    JS_FreeAtom(ctx, A->comment);
}

static int zip_take_entry(JSContext* ctx, JSValueConst arr, uint32_t i,
    const zip_key_atoms_t* A, zip_out_t* o,
    uint8_t** data, size_t* dlen,
    int pack_store, int* is_store)
{
    JSValue e, v, m = JS_UNDEFINED, mt = JS_UNDEFINED, md = JS_UNDEFINED,
                  cm = JS_UNDEFINED;
    JSPropertyEnum* props = NULL;
    uint32_t nprops = 0, j;
    const char* s;
    int r = -1;

    *data = NULL;
    *dlen = 0;
    o->name = NULL;
    o->comment = NULL;
    o->clen = 0;
    e = JS_GetPropertyUint32(ctx, arr, i);
    if (JS_IsException(e))
        return -1;
    if (JS_GetOwnPropertyNames(ctx, &props, &nprops, e,
            JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY)
        < 0)
        goto done;
    for (j = 0; j < nprops; j++) {
        JSAtom at = props[j].atom;
        JSValue* slot = NULL;

        if (at == A->method)
            slot = &m;
        else if (at == A->mtime)
            slot = &mt;
        else if (at == A->mode)
            slot = &md;
        else if (at == A->comment)
            slot = &cm;
        else if (at == A->name || at == A->data)
            continue;
        else {
            const char* nm = JS_AtomToCString(ctx, at);
            if (nm) {
                JS_ThrowTypeError(ctx,
                    "unknown option \"%s\" (valid: name, data, method, "
                    "mtime, mode, comment)",
                    nm);
                JS_FreeCString(ctx, nm);
            }
            goto done;
        }
        *slot = JS_GetProperty(ctx, e, at);
        if (JS_IsException(*slot))
            goto done;
    }
    v = JS_GetProperty(ctx, e, A->name);
    s = NULL;
    if (!JS_IsException(v) && JS_IsString(v))
        s = JS_ToCStringLen(ctx, &o->nlen, v);
    JS_FreeValue(ctx, v);
    if (!s) {
        JS_ThrowTypeError(ctx, "ZipPack: every entry needs a string name");
        goto done;
    }
    if (!arck_ar_name_safe(s, o->nlen)) {
        JS_ThrowRangeError(ctx, "ZipPack: %.60s is not a safe archive name", s);
        JS_FreeCString(ctx, s);
        goto done;
    }
    o->name = (char*)malloc(o->nlen + 1);
    if (!o->name) {
        JS_FreeCString(ctx, s);
        JS_ThrowOutOfMemory(ctx);
        goto done;
    }
    memcpy(o->name, s, o->nlen);
    o->name[o->nlen] = 0;
    JS_FreeCString(ctx, s);
    v = JS_GetProperty(ctx, e, A->data);
    if (JS_IsException(v) || dyn_read_input(ctx, v, data, dlen) < 0) {
        JS_FreeValue(ctx, v);
        free(o->name);
        o->name = NULL;
        goto done;
    }
    JS_FreeValue(ctx, v);
    if (zip_entry_opt_method(ctx, m, pack_store, is_store) < 0 || zip_entry_opt_mtime(ctx, mt, &o->dos_mtime) < 0 || zip_entry_opt_mode(ctx, md, &o->mode, o->nlen && o->name[o->nlen - 1] == '/') < 0 || zip_entry_opt_string(ctx, cm, &o->comment, &o->clen) < 0) {
        free(*data);
        *data = NULL;
        *dlen = 0;
        free(o->name);
        o->name = NULL;
        free(o->comment);
        o->comment = NULL;
        o->clen = 0;
        goto done;
    }
    r = 0;
done:
    JS_FreeValue(ctx, m);
    JS_FreeValue(ctx, mt);
    JS_FreeValue(ctx, md);
    JS_FreeValue(ctx, cm);
    if (props) {
        uint32_t q;
        for (q = 0; q < nprops; q++)
            JS_FreeAtom(ctx, props[q].atom);
        js_free(ctx, props);
    }
    JS_FreeValue(ctx, e);
    return r;
}

static void zip_emit_local(ab_t* b, zip_out_t* o, const uint8_t* body)
{
    arck_ab_u32(b, 0x04034b50);
    arck_ab_u16(b, 20);
    arck_ab_u16(b, 0);
    arck_ab_u16(b, (uint32_t)o->method);
    arck_ab_u32(b, o->dos_mtime);
    arck_ab_u32(b, o->crc);
    arck_ab_u32(b, (uint32_t)o->csize);
    arck_ab_u32(b, (uint32_t)o->usize);
    arck_ab_u16(b, (uint32_t)o->nlen);
    arck_ab_u16(b, 0);
    arck_ab_write(b, o->name, o->nlen);
    arck_ab_write(b, body, o->csize);
}

static void zip_emit_central(ab_t* b, const zip_out_t* o)
{
    arck_ab_u32(b, 0x02014b50);
    arck_ab_u16(b, 20 | (3 << 8));
    arck_ab_u16(b, 20);
    arck_ab_u16(b, 0);
    arck_ab_u16(b, (uint32_t)o->method);
    arck_ab_u32(b, o->dos_mtime);
    arck_ab_u32(b, o->crc);
    arck_ab_u32(b, (uint32_t)o->csize);
    arck_ab_u32(b, (uint32_t)o->usize);
    arck_ab_u16(b, (uint32_t)o->nlen);
    arck_ab_u16(b, 0);
    arck_ab_u16(b, (uint32_t)o->clen);
    arck_ab_u16(b, 0);
    arck_ab_u16(b, 0);
    arck_ab_u32(b, o->mode << 16);
    arck_ab_u32(b, (uint32_t)o->local);
    arck_ab_write(b, o->name, o->nlen);
    if (o->clen)
        arck_ab_write(b, o->comment, o->clen);
}

static int zip_pack_method(JSContext* ctx, JSValueConst o, int* store_only)
{
    JSValue v;
    const char* s;

    *store_only = 0;
    if (!JS_IsObject(o))
        return 0;
    v = JS_GetPropertyStr(ctx, o, "method");
    if (JS_IsException(v))
        return -1;
    if (!JS_IsString(v)) {
        JS_FreeValue(ctx, v);
        return 0;
    }
    s = JS_ToCString(ctx, v);
    JS_FreeValue(ctx, v);
    if (!s)
        return -1;
    *store_only = strcmp(s, "store") == 0;
    if (!*store_only && strcmp(s, "deflate") != 0) {
        JS_FreeCString(ctx, s);
        JS_ThrowRangeError(ctx, "ZipPack: method is \"deflate\" or \"store\"");
        return -1;
    }
    JS_FreeCString(ctx, s);
    return 0;
}

JSValue dyn_zip_pack(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    ab_t b;
    JSValue lv, out;
    int64_t n = 0, i;
    zip_out_t* ents = NULL;
    zip_key_atoms_t Z;
    size_t cd_off;
    int store_only = 0;

    (void)this_val;
    if (argc < 1 || JS_IsArray(ctx, argv[0]) != 1)
        return JS_ThrowTypeError(ctx, "ZipPack(entries): entries is an array");
    if (zip_pack_method(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, &store_only) < 0)
        return JS_EXCEPTION;
    if (argc > 1 && JS_IsObject(argv[1]) && dyn_opts_strict(ctx, argv[1], (const char* const[]) { "method" }, 1) < 0)
        return JS_EXCEPTION;
    lv = JS_GetPropertyStr(ctx, argv[0], "length");
    if (JS_IsException(lv) || JS_ToInt64(ctx, &n, lv) < 0) {
        JS_FreeValue(ctx, lv);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, lv);
    if (n < 0 || n > ZIP_MAX_ENTRIES)
        return JS_ThrowRangeError(ctx, "ZipPack: at most %u entries (the zip "
                                       "EOCD count field is 16-bit; zip64 is not "
                                       "written)",
            ZIP_MAX_ENTRIES);
    if (zip_key_atoms_init(ctx, &Z) < 0) {
        zip_key_atoms_free(ctx, &Z);
        return JS_ThrowOutOfMemory(ctx);
    }
    ents = (zip_out_t*)calloc((size_t)(n ? n : 1), sizeof *ents);
    if (!ents) {
        zip_key_atoms_free(ctx, &Z);
        return JS_ThrowOutOfMemory(ctx);
    }
    arck_ab_init(&b);
    for (i = 0; i < n; i++) {
        uint8_t *data = NULL, *comp = NULL;
        size_t dlen = 0, clen = 0;
        int is_store;

        if (zip_take_entry(ctx, argv[0], (uint32_t)i, &Z, &ents[i], &data,
                &dlen, store_only, &is_store)
            < 0)
            goto fail;
        if (ents[i].comment && ents[i].clen > 1024) {
            ents[i].comment[1024] = 0;
            ents[i].clen = 1024;
        }
        if (dlen > AR_MAX_MEMBER) {
            JS_ThrowRangeError(ctx, "ZipPack: entry %u exceeds the size limit",
                (unsigned)i);
            free(data);
            goto fail;
        }
        ents[i].crc = dyn_crc32(data, dlen);
        ents[i].usize = dlen;
        ents[i].local = b.n;
        if (!is_store && dlen && arck_zip_deflate_raw(data, dlen, &comp, &clen) == 0
            && clen < dlen) {
            ents[i].method = 8;
            ents[i].csize = clen;
        } else {
            free(comp);
            comp = NULL;
            ents[i].method = 0;
            ents[i].csize = dlen;
        }
        zip_emit_local(&b, &ents[i], comp ? comp : data);
        free(comp);
        free(data);
        if (b.oom) {
            JS_ThrowOutOfMemory(ctx);
            goto fail;
        }
        if (b.n > (size_t)UINT32_MAX - 65536) {
            JS_ThrowRangeError(ctx,
                "ZipPack: archive exceeds the 4 GiB offset limit");
            goto fail;
        }
    }
    cd_off = b.n;
    for (i = 0; i < n; i++)
        zip_emit_central(&b, &ents[i]);
    {
        size_t cd_size = b.n - cd_off;
        if (cd_off > (size_t)UINT32_MAX || cd_size > (size_t)UINT32_MAX) {
            JS_ThrowRangeError(ctx,
                "ZipPack: central directory exceeds the 4 GiB offset limit");
            goto fail;
        }
        arck_ab_u32(&b, 0x06054b50);
        arck_ab_u16(&b, 0);
        arck_ab_u16(&b, 0);
        arck_ab_u16(&b, (uint32_t)n);
        arck_ab_u16(&b, (uint32_t)n);
        arck_ab_u32(&b, (uint32_t)cd_size);
        arck_ab_u32(&b, (uint32_t)cd_off);
        arck_ab_u16(&b, 0);
    }
    if (b.oom) {
        JS_ThrowOutOfMemory(ctx);
        goto fail;
    }
    zip_key_atoms_free(ctx, &Z);
    for (i = 0; i < n; i++) {
        free(ents[i].name);
        free(ents[i].comment);
    }
    free(ents);
    out = dyn_bytes_to_uint8(ctx, b.p, b.n);
    arck_ab_free(&b);
    return out;
fail:
    zip_key_atoms_free(ctx, &Z);
    for (i = 0; i < n; i++) {
        free(ents[i].name);
        free(ents[i].comment);
    }
    free(ents);
    arck_ab_free(&b);
    return JS_EXCEPTION;
}

static int dyn_format_opt(JSContext* ctx, JSValueConst opts, const char* fn, int* pzlib)
{
    JSValue v = JS_GetPropertyStr(ctx, opts, "format");
    const char* name;
    if (JS_IsException(v))
        return -1;
    if (JS_IsUndefined(v) || JS_IsNull(v))
        return 0;
    if (!JS_IsString(v)) {
        JS_FreeValue(ctx, v);
        JS_ThrowTypeError(ctx, "%s: format must be \"raw\" or \"zlib\"", fn);
        return -1;
    }
    name = JS_ToCString(ctx, v);
    JS_FreeValue(ctx, v);
    if (!name)
        return -1;
    if (!strcmp(name, "zlib"))
        *pzlib = 1;
    else if (strcmp(name, "raw")) {
        JS_FreeCString(ctx, name);
        JS_ThrowTypeError(ctx, "%s: format must be \"raw\" or \"zlib\"", fn);
        return -1;
    }
    JS_FreeCString(ctx, name);
    return 0;
}

JSValue dyn_deflate_js(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    uint8_t *src = NULL, *out = NULL;
    size_t src_len = 0, out_len = 0;
    int level = 6, zlib = 0;
    static const char* const lvl_keys[] = { "level", "format" };

    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "deflate(data): input required");
    if (argc > 1 && JS_IsObject(argv[1])) {
        JSValue lv;
        int64_t t;
        if (dyn_opts_strict(ctx, argv[1], lvl_keys, 2) < 0)
            return JS_EXCEPTION;
        if (dyn_format_opt(ctx, argv[1], "deflate", &zlib) < 0)
            return JS_EXCEPTION;
        lv = JS_GetPropertyStr(ctx, argv[1], "level");
        if (JS_IsException(lv))
            return JS_EXCEPTION;
        if (!JS_IsUndefined(lv) && !JS_IsNull(lv)) {
            if (JS_ToInt64(ctx, &t, lv) < 0) {
                JS_FreeValue(ctx, lv);
                return JS_EXCEPTION;
            }
            if (t < 1 || t > 12) {
                JS_FreeValue(ctx, lv);
                return JS_ThrowRangeError(ctx, "deflate: level must be 1..12");
            }
            level = (int)t;
        }
        JS_FreeValue(ctx, lv);
    } else if (argc > 1 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
        return JS_ThrowTypeError(ctx, "deflate(data, opts): opts must be an object");
    }
    if (dyn_read_input(ctx, argv[0], &src, &src_len) < 0)
        return JS_EXCEPTION;
    if (src_len > AR_MAX_MEMBER) {
        free(src);
        return JS_ThrowRangeError(ctx, "deflate: input exceeds the size limit");
    }
    if (DYN_OFFLOAD_CPU(zlib ? dyn_zlib_deflate(src, src_len, level, NULL, &out, &out_len)
                            : dyn_raw_deflate(src, src_len, level, NULL, &out, &out_len))
        < 0) {
        free(src);
        return JS_ThrowOutOfMemory(ctx);
    }
    free(src);
    {
        JSValue r = dyn_bytes_to_uint8(ctx, out, out_len);
        free(out);
        return r;
    }
}

JSValue dyn_inflate_js(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    uint8_t* src = NULL;
    size_t src_len = 0;
    dyn_outbuf_t o = { NULL, 0, 0 };
    static const char* const as_keys[] = { "asString", "maxOutputBytes", "format" };
    int zlib = 0, rc;

    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "inflate(data): input required");
    if (argc > 1 && JS_IsObject(argv[1]) && dyn_opts_strict(ctx, argv[1], as_keys, 3) < 0)
        return JS_EXCEPTION;
    if (argc > 1 && JS_IsObject(argv[1]) && dyn_format_opt(ctx, argv[1], "inflate", &zlib) < 0)
        return JS_EXCEPTION;
    if (argc > 1 && JS_IsObject(argv[1])) {
        JSValue mv = JS_GetPropertyStr(ctx, argv[1], "maxOutputBytes");
        if (JS_IsException(mv))
            return JS_EXCEPTION;
        if (!JS_IsUndefined(mv) && !JS_IsNull(mv)) {
            int64_t n;
            if (JS_ToInt64(ctx, &n, mv) < 0) {
                JS_FreeValue(ctx, mv);
                return JS_EXCEPTION;
            }
            if (n < 1 || (uint64_t)n > (uint64_t)DYN_MAX_OUTPUT) {
                JS_FreeValue(ctx, mv);
                return JS_ThrowRangeError(ctx,
                    "inflate: maxOutputBytes must be an integer in [1, %llu]",
                    (unsigned long long)DYN_MAX_OUTPUT);
            }
            o.limit = (size_t)n;
        }
        JS_FreeValue(ctx, mv);
    }
    if (dyn_read_input(ctx, argv[0], &src, &src_len) < 0)
        return JS_EXCEPTION;
    rc = DYN_OFFLOAD_CPU(zlib ? dyn_zlib_inflate(src, src_len, &o) : dyn_raw_inflate(src, src_len, &o));
    if (rc < 0) {
        free(src);
        free(o.buf);
        if (o.exceeded)
            return JS_ThrowRangeError(ctx,
                "inflate: decompressed output exceeds maxOutputBytes (%llu)",
                (unsigned long long)o.limit);
        if (rc == DYN_ZLIB_E_HEADER)
            return JS_ThrowSyntaxError(ctx, "inflate: bad zlib header");
        if (rc == DYN_ZLIB_E_DICT)
            return JS_ThrowSyntaxError(ctx, "inflate: zlib preset dictionaries are not supported");
        if (rc == DYN_ZLIB_E_FRAME)
            return JS_ThrowSyntaxError(ctx, "inflate: zlib stream is truncated or has trailing bytes");
        if (rc == DYN_ZLIB_E_CHECKSUM)
            return JS_ThrowSyntaxError(ctx, "inflate: zlib Adler-32 checksum mismatch");
        return JS_ThrowSyntaxError(ctx, "inflate: malformed DEFLATE stream");
    }
    free(src);
    if (argc > 1 && JS_IsObject(argv[1])) {
        JSValue av = JS_GetPropertyStr(ctx, argv[1], "asString");
        int as = 0;
        if (JS_IsException(av)) {
            free(o.buf);
            return JS_EXCEPTION;
        }
        if (!JS_IsUndefined(av) && !JS_IsNull(av))
            as = JS_ToBool(ctx, av);
        JS_FreeValue(ctx, av);
        if (as < 0) {
            free(o.buf);
            return JS_EXCEPTION;
        }
        if (as) {
            JSValue s = JS_NewStringLen(ctx, (const char*)o.buf, o.len);
            free(o.buf);
            return s;
        }
    }
    {
        JSValue r = dyn_bytes_to_uint8(ctx, o.buf, o.len);
        free(o.buf);
        return r;
    }
}

JSValue dyn_zip_read_at(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    uint8_t* src = NULL;
    size_t src_len = 0;
    zip_rd_t z;
    int64_t idx = 0;
    size_t off;
    zip_ent_t e;
    int rc, k = 0;

    (void)this_val;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "ZipReadAt(bytes, index): both required");
    if (JS_ToInt64(ctx, &idx, argv[1]) < 0)
        return JS_EXCEPTION;
    if (idx < 0)
        return JS_ThrowRangeError(ctx, "ZipReadAt: index must be >= 0");
    if (dyn_read_input(ctx, argv[0], &src, &src_len) < 0)
        return JS_EXCEPTION;
    memset(&z, 0, sizeof z);
    z.ud = ctx;
    z.p = src;
    z.n = src_len;
    if (ar_opt_unsafe(ctx, argc > 2 ? argv[2] : JS_UNDEFINED, &z.allow_unsafe) < 0) {
        free(src);
        return JS_EXCEPTION;
    }
    if (arck_zip_open(&z) < 0) {
        free(src);
        return JS_ThrowSyntaxError(ctx, "ZipReadAt: %s",
            z.err[0] ? z.err : "malformed archive");
    }
    off = z.cd_off;
    memset(&e, 0, sizeof e);
    for (;;) {
        rc = arck_zip_entry_at(&z, &off, &e);
        if (rc < 0) {
            free(src);
            return JS_ThrowSyntaxError(ctx, "ZipReadAt: %s",
                z.err[0] ? z.err : "malformed archive");
        }
        if (rc == 0) {
            free(src);
            return JS_ThrowRangeError(ctx, "ZipReadAt: index %lld out of range",
                (long long)idx);
        }
        if (k == idx)
            break;
        k++;
        memset(&e, 0, sizeof e);
    }
    {
        dyn_outbuf_t o = { NULL, 0, 0 };
        JSValue eo;
        if (arck_zip_member(&z, &e, &o) < 0) {
            free(o.buf);
            free(src);
            return JS_ThrowSyntaxError(ctx, "ZipReadAt: %s",
                z.err[0] ? z.err : "malformed archive");
        }
        eo = zip_entry_obj(ctx, &e);
        free(src);
        if (JS_IsException(eo)) {
            free(o.buf);
            return JS_EXCEPTION;
        }
        {
            JSValue dv = dyn_bytes_to_uint8(ctx, o.buf, o.len);
            if (JS_IsException(dv)) {
                JS_FreeValue(ctx, eo);
                free(o.buf);
                return JS_EXCEPTION;
            }
            JS_DefinePropertyValueStr(ctx, eo, "data", dv, JS_PROP_C_W_E);
        }
        free(o.buf);
        return eo;
    }
}

static int ar_write_member(int rootfd, const char* name, size_t nlen,
    const uint8_t* buf, size_t len)
{
    char *path, *comp, *sep;
    int cur = rootfd, cur_owned = 0, next, fd = -1, rc = -1;
    struct stat st;

    path = (char*)malloc(nlen + 1);
    if (!path) {
        errno = ENOMEM;
        return -1;
    }
    memcpy(path, name, nlen);
    path[nlen] = '\0';
    comp = path;
    for (;;) {
        sep = strchr(comp, '/');
        if (!sep)
            break;
        *sep = '\0';
        if (comp[0] != '\0' && !(comp[0] == '.' && comp[1] == '\0')) {
            if (mkdirat(cur, comp, 0755) < 0 && errno != EEXIST)
                goto out;
            next = openat(cur, comp,
                O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            if (next < 0)
                goto out;
            if (cur_owned)
                close(cur);
            cur = next;
            cur_owned = 1;
        }
        comp = sep + 1;
    }
    if (comp[0] == '\0' || (comp[0] == '.' && comp[1] == '\0')) {
        errno = EISDIR;
        goto out;
    }
    if (fstatat(cur, comp, &st, AT_SYMLINK_NOFOLLOW) == 0) {
        if (!S_ISREG(st.st_mode)) {
            errno = ELOOP;
            goto out;
        }
        if (unlinkat(cur, comp, 0) < 0)
            goto out;
    } else if (errno != ENOENT) {
        goto out;
    }
    fd = openat(cur, comp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
        0666);
    if (fd < 0)
        goto out;
    while (len) {
        ssize_t w = write(fd, buf, len);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            goto out;
        }
        buf += (size_t)w;
        len -= (size_t)w;
    }
    rc = 0;
out:
    if (fd >= 0)
        close(fd);
    if (cur_owned)
        close(cur);
    free(path);
    return rc;
}

static int ar_dir_value(JSContext* ctx, JSValueConst v, char** pdir,
    size_t* plen)
{
    const char* s;
    size_t n = 0;
    int is_str = JS_IsString(v);

    if (is_str) {
        s = JS_ToCStringLen(ctx, &n, v);
        if (!s)
            return -1;
    } else if (dyn_value_is_path(v)) {
        s = dyn_path_borrow(ctx, v, "ZipExtractAll: dir", &n);
        if (!s)
            return -1;
    } else {
        JS_ThrowTypeError(ctx, "ZipExtractAll: dir must be a string or Path");
        return -1;
    }
    *pdir = (char*)malloc(n + 1);
    if (!*pdir) {
        if (is_str)
            JS_FreeCString(ctx, s);
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    memcpy(*pdir, s, n);
    (*pdir)[n] = 0;
    *plen = n;
    if (is_str)
        JS_FreeCString(ctx, s);
    return 0;
}

JSValue dyn_zip_extract_all(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    static const char* const ar_extract_keys[] = { "dir", "allowUnsafeNames" };
    uint8_t* src = NULL;
    size_t src_len = 0;
    zip_rd_t z;
    JSValue out = JS_UNDEFINED;
    uint32_t k = 0;
    size_t off, dirlen = 0;
    uint64_t total_out = 0;
    char* dir = NULL;
    int rootfd = -1, allow_unsafe = 0, a;
    JSValueConst bag = JS_UNDEFINED;

    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "ZipExtractAll(bytes[, dir]): archive required");
    for (a = 1; a < argc; a++) {
        JSValueConst v = argv[a];
        if (JS_IsUndefined(v) || JS_IsNull(v))
            continue;
        if (JS_IsString(v) || dyn_value_is_path(v)) {
            if (dir) {
                JS_ThrowTypeError(ctx, "ZipExtractAll: dir given twice");
                goto fail;
            }
            if (ar_dir_value(ctx, v, &dir, &dirlen) < 0)
                goto fail;
        } else if (JS_IsObject(v) && JS_IsArray(ctx, v) != 1) {
            if (!JS_IsUndefined(bag)) {
                JS_ThrowTypeError(ctx, "ZipExtractAll: one options bag");
                goto fail;
            }
            bag = v;
        } else {
            JS_ThrowTypeError(ctx, "ZipExtractAll: dir must be a string or "
                                   "Path, opts an object");
            goto fail;
        }
    }
    if (!JS_IsUndefined(bag)) {
        JSValue v;
        if (dyn_opts_strict(ctx, bag, ar_extract_keys, 2) < 0)
            goto fail;
        v = JS_GetPropertyStr(ctx, bag, "dir");
        if (JS_IsException(v))
            goto fail;
        if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
            if (dir) {
                JS_FreeValue(ctx, v);
                JS_ThrowTypeError(ctx, "ZipExtractAll: dir given twice");
                goto fail;
            }
            if (ar_dir_value(ctx, v, &dir, &dirlen) < 0) {
                JS_FreeValue(ctx, v);
                goto fail;
            }
        }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, bag, "allowUnsafeNames");
        if (JS_IsException(v))
            goto fail;
        if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
            int b = JS_ToBool(ctx, v);
            JS_FreeValue(ctx, v);
            if (b < 0)
                goto fail;
            allow_unsafe = b;
        } else {
            JS_FreeValue(ctx, v);
        }
    }
    if (dyn_read_input(ctx, argv[0], &src, &src_len) < 0)
        goto fail;
    memset(&z, 0, sizeof z);
    z.ud = ctx;
    z.p = src;
    z.n = src_len;
    z.allow_unsafe = allow_unsafe;
    if (arck_zip_open(&z) < 0) {
        JS_ThrowSyntaxError(ctx, "ZipExtractAll: %s",
            z.err[0] ? z.err : "malformed archive");
        goto fail;
    }
    if (arck_zip_preflight(&z) < 0) {
        if (strstr(z.err, "exceeds"))
            JS_ThrowRangeError(ctx, "ZipExtractAll: %s", z.err);
        else
            JS_ThrowSyntaxError(ctx, "ZipExtractAll: %s",
                z.err[0] ? z.err : "malformed archive");
        goto fail;
    }
    if (dir) {
        char* tmp = (char*)malloc(dirlen + 1);
        if (tmp) {
            size_t i;
            memcpy(tmp, dir, dirlen + 1);
            for (i = 1; i < dirlen; i++) {
                if (tmp[i] == '/') {
                    tmp[i] = 0;
                    mkdir(tmp, 0755);
                    tmp[i] = '/';
                }
            }
            mkdir(tmp, 0755);
            free(tmp);
        }
        rootfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (rootfd < 0) {
            JS_ThrowTypeError(ctx, "ZipExtractAll: cannot write '%.80s'", dir);
            goto fail;
        }
    }
    out = JS_NewArray(ctx);
    if (JS_IsException(out))
        goto fail;
    off = z.cd_off;
    for (;;) {
        zip_ent_t e;
        dyn_outbuf_t o = { NULL, 0, 0 };
        JSValue eo, dv;
        int r;

        memset(&e, 0, sizeof e);
        r = arck_zip_entry_at(&z, &off, &e);
        if (r < 0) {
            JS_ThrowSyntaxError(ctx, "ZipExtractAll: %s",
                z.err[0] ? z.err : "malformed archive");
            goto fail;
        }
        if (r == 0)
            break;
        if (k >= AR_MAX_ENTRIES) {
            JS_ThrowRangeError(ctx, "ZipExtractAll: too many entries");
            goto fail;
        }
        if (arck_zip_member(&z, &e, &o) < 0) {
            free(o.buf);
            JS_ThrowSyntaxError(ctx, "ZipExtractAll: %s",
                z.err[0] ? z.err : "malformed archive");
            goto fail;
        }
        total_out += o.len;
        if (total_out > AR_MAX_TOTAL_OUT) {
            free(o.buf);
            JS_ThrowRangeError(ctx,
                "ZipExtractAll: aggregate output exceeds 1 GiB");
            goto fail;
        }
        eo = zip_entry_obj(ctx, &e);
        if (JS_IsException(eo)) {
            free(o.buf);
            goto fail;
        }
        dv = dyn_bytes_to_uint8(ctx, o.buf, o.len);
        if (JS_IsException(dv)) {
            JS_FreeValue(ctx, eo);
            free(o.buf);
            goto fail;
        }
        if (JS_DefinePropertyValueStr(ctx, eo, "data", dv, JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, eo);
            free(o.buf);
            goto fail;
        }
        free(o.buf);
        if (JS_DefinePropertyValueUint32(ctx, out, k++, eo, JS_PROP_C_W_E) < 0)
            goto fail;
    }
    if (rootfd >= 0) {
        uint32_t wi;
        off = z.cd_off;
        for (wi = 0; wi < k; wi++) {
            zip_ent_t e;
            JSValue eo, dv, ab;
            size_t boff = 0, blen = 0, absz = 0;
            uint8_t* bytes;
            int wrc = 0;

            memset(&e, 0, sizeof e);
            if (arck_zip_entry_at(&z, &off, &e) != 1)
                break;
            if (e.nlen && e.name[e.nlen - 1] == '/')
                continue;
            eo = JS_GetPropertyUint32(ctx, out, wi);
            dv = JS_GetPropertyStr(ctx, eo, "data");
            ab = JS_GetTypedArrayBuffer(ctx, dv, &boff, &blen, NULL);
            bytes = JS_IsException(ab) ? NULL : JS_GetArrayBuffer(ctx, &absz, ab);
            if (!bytes && blen)
                wrc = -1;
            else if (ar_write_member(rootfd, e.name, e.nlen,
                         bytes ? bytes + boff : (const uint8_t*)"", blen)
                < 0)
                wrc = -1;
            JS_FreeValue(ctx, ab);
            JS_FreeValue(ctx, dv);
            JS_FreeValue(ctx, eo);
            if (wrc < 0) {
                JS_ThrowTypeError(ctx, "ZipExtractAll: cannot write '%.80s'",
                    e.name);
                goto fail;
            }
        }
    }
    free(src);
    free(dir);
    if (rootfd >= 0)
        close(rootfd);
    return out;
fail:
    JS_FreeValue(ctx, out);
    free(src);
    free(dir);
    if (rootfd >= 0)
        close(rootfd);
    return JS_EXCEPTION;
}

#endif
