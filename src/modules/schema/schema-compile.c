#include "schema-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_SCHEMA)

#define DYN_SC_MAX_SCHEMA_DEPTH 256

#define SC_VALIDATION_FLAGS (SC_F_TYPE | SC_F_CONST | SC_F_ENUM | SC_F_MIN | SC_F_MAX | SC_F_EMIN | SC_F_EMAX \
    | SC_F_MULT | SC_F_MINLEN | SC_F_MAXLEN | SC_F_PAT | SC_F_MINITEMS | SC_F_MAXITEMS | SC_F_UNIQUE          \
    | SC_F_MINPROPS | SC_F_MAXPROPS | SC_F_REQ | SC_F_DEPREQ | SC_F_MINCONT | SC_F_MAXCONT)

#define DYN_SC_VOCAB "https://json-schema.org/draft/2020-12/vocab/"

static int dyn_sc_index_add(dyn_sccomp_t* c, const char* path, size_t plen,
    dyn_scnode_t* n)
{
    char* p;
    if (c->n_paths >= c->cap_paths) {
        int ncap = c->cap_paths ? c->cap_paths * 2 : 64;
        char** np;
        dyn_scnode_t** nn;
        np = js_realloc(c->ctx, c->paths, (size_t)ncap * sizeof(*np));
        if (!np)
            return -1;
        c->paths = np;
        nn = js_realloc(c->ctx, c->pnode, (size_t)ncap * sizeof(*nn));
        if (!nn)
            return -1;
        c->pnode = nn;
        c->cap_paths = ncap;
    }
    p = js_malloc(c->ctx, plen + 1);
    if (!p)
        return -1;
    memcpy(p, path, plen);
    p[plen] = '\0';
    c->paths[c->n_paths] = p;
    c->pnode[c->n_paths] = n;
    c->n_paths++;
    return 0;
}

static dyn_scnode_t* dyn_sc_index_find(dyn_sccomp_t* c, const char* path,
    size_t plen)
{
    int i;
    for (i = 0; i < c->n_paths; i++)
        if (strlen(c->paths[i]) == plen && memcmp(c->paths[i], path, plen) == 0)
            return c->pnode[i];
    return NULL;
}

static char* dyn_sc_child_path(JSContext* ctx, const char* path, size_t plen,
    const char* tok, size_t tlen, size_t* olen)
{
    size_t cap = plen + 1 + tlen * 2 + 1, i, o;
    char* buf = js_malloc(ctx, cap);
    if (!buf)
        return NULL;
    memcpy(buf, path, plen);
    o = plen;
    buf[o++] = '/';
    for (i = 0; i < tlen; i++) {
        if (tok[i] == '~') {
            buf[o++] = '~';
            buf[o++] = '0';
        } else if (tok[i] == '/') {
            buf[o++] = '~';
            buf[o++] = '1';
        } else
            buf[o++] = tok[i];
    }
    buf[o] = '\0';
    *olen = o;
    return buf;
}

static int dyn_sc_compile_sub(dyn_sccomp_t* c, JSValueConst v,
    const char* path, size_t plen,
    const char* tok, size_t tlen,
    dyn_scnode_t** out)
{
    char* cp;
    size_t cplen;
    int r;
    cp = dyn_sc_child_path(c->ctx, path, plen, tok, tlen, &cplen);
    if (!cp)
        return -1;
    r = dyn_sc_compile(c, v, cp, cplen, out);
    js_free(c->ctx, cp);
    return r;
}

static int dyn_sc_compile_at(dyn_sccomp_t* c, JSValueConst v,
    const char* path, size_t plen,
    const char* kwd, const char* tok, size_t tlen,
    dyn_scnode_t** out)
{
    char* kp;
    size_t kl;
    int r;
    kp = dyn_sc_child_path(c->ctx, path, plen, kwd, strlen(kwd), &kl);
    if (!kp)
        return -1;
    r = dyn_sc_compile_sub(c, v, kp, kl, tok, tlen, out);
    js_free(c->ctx, kp);
    return r;
}

static int dyn_sc_parse_type(JSContext* ctx, JSValueConst v, int* mask)
{
    int m = 0;
    if (JS_IsString(v)) {
        size_t n;
        const char* s = JS_ToCStringLen(ctx, &n, v);
        int b;
        if (!s)
            return -1;
        if (dyn_sc_type_name(s, n, &b)) {
            JS_ThrowTypeError(ctx, "dyna:schema: unknown type \"%.*s\"",
                (int)n, s);
            JS_FreeCString(ctx, s);
            return -1;
        }
        JS_FreeCString(ctx, s);
        *mask = b;
        return 0;
    }
    if (dyn_sc_is_array(ctx, v)) {
        uint64_t len = dyn_sc_arr_len(ctx, v), i;
        if (len == (uint64_t)-1)
            return -1;
        if (len > DYN_SC_MAX_LIST) {
            JS_ThrowTypeError(ctx, "dyna:schema: \"type\" array is too large");
            return -1;
        }
        for (i = 0; i < len; i++) {
            JSValue el = JS_GetPropertyUint32(ctx, v, (uint32_t)i);
            size_t n;
            const char* s;
            int b;
            if (JS_IsException(el))
                return -1;
            if (!JS_IsString(el)) {
                JS_ThrowTypeError(ctx,
                    "dyna:schema: every element of \"type\" must be a string");
                JS_FreeValue(ctx, el);
                return -1;
            }
            s = JS_ToCStringLen(ctx, &n, el);
            if (!s) {
                JS_FreeValue(ctx, el);
                return -1;
            }
            if (dyn_sc_type_name(s, n, &b)) {
                JS_ThrowTypeError(ctx,
                    "dyna:schema: unknown type \"%.*s\"", (int)n, s);
                JS_FreeCString(ctx, s);
                JS_FreeValue(ctx, el);
                return -1;
            }
            JS_FreeCString(ctx, s);
            JS_FreeValue(ctx, el);
            m |= b;
        }
        *mask = m;
        return 0;
    }
    JS_ThrowTypeError(ctx,
        "dyna:schema: \"type\" must be a string or an array of strings");
    return -1;
}

static int dyn_sc_compile_re(JSContext* ctx, const char* s, size_t n,
    uint8_t** bc)
{
    char errbuf[128];
    int rlen;
    if (n > DYN_SC_MAX_PATTERN) {
        JS_ThrowRangeError(ctx,
            "dyna:schema: pattern source exceeds %d bytes", DYN_SC_MAX_PATTERN);
        return -1;
    }
    *bc = lre_compile(&rlen, errbuf, sizeof errbuf, s, n,
        LRE_FLAG_UNICODE, ctx);
    if (!*bc) {
        JS_ThrowTypeError(ctx, "dyna:schema: invalid pattern: %s", errbuf);
        return -1;
    }
    return 0;
}

static int dyn_sc_compile_kw(dyn_sccomp_t* c, JSValueConst sv,
    dyn_scnode_t* n, const char* path, size_t plen);

static int dyn_sc_enter_resource(dyn_sccomp_t* c, JSValueConst sv, dyn_scnode_t* n,
    const char* path, size_t plen)
{
    JSContext* ctx = c->ctx;
    char *id = NULL, *hash, *abs;
    const char* base = c->cur ? c->cur->uri : c->base_uri;
    dyn_scres_t* r;
    int got = JS_IsUndefined(sv) ? 0 : dyn_sc_str_kw(ctx, sv, "$id", &id);
    if (got < 0)
        return -1;
    if (!got) {
        if (!c->cur && !(c->cur = dyn_sc_res_add(c, base, strlen(base), path, plen, n)))
            return -1;
        return 0;
    }
    hash = strchr(id, '#');
    if (hash) {
        if (hash[1]) {
            JS_ThrowTypeError(ctx, "dyna:schema: \"$id\" must not carry a fragment (\"%s\"); use $anchor", id);
            js_free(ctx, id);
            return -1;
        }
        *hash = 0;
    }
    abs = dyn_uri_resolve(ctx, base, id);
    js_free(ctx, id);
    if (!abs)
        return -1;
    r = dyn_sc_res_find(c->sch, abs, strlen(abs));
    if (r && r->root != n) {
        JS_ThrowTypeError(ctx, "dyna:schema: duplicate \"$id\" \"%s\"", abs);
        js_free(ctx, abs);
        return -1;
    }
    if (!r)
        r = dyn_sc_res_add(c, abs, strlen(abs), path, plen, n);
    js_free(ctx, abs);
    if (!r)
        return -1;
    c->cur = r;
    return 0;
}

int dyn_sc_compile(dyn_sccomp_t* c, JSValueConst sv, const char* path,
    size_t plen, dyn_scnode_t** out)
{
    JSContext* ctx = c->ctx;
    dyn_scnode_t* n;

    if (c->depth > DYN_SC_MAX_SCHEMA_DEPTH) {
        JS_ThrowRangeError(ctx,
            "dyna:schema: schema nesting exceeds %d levels at \"%s\"",
            DYN_SC_MAX_SCHEMA_DEPTH, path);
        return -1;
    }

    if (JS_IsBool(sv)) {
        n = dyn_sc_new_node(c);
        if (!n)
            return -1;
        n->kind = SC_BOOL;
        n->boolv = JS_ToBool(ctx, sv) > 0;
        if (!c->cur && dyn_sc_enter_resource(c, JS_UNDEFINED, n, path, plen))
            return -1;
        n->res = c->cur;
        if (dyn_sc_index_add(c, path, plen, n))
            return -1;
        *out = n;
        return 0;
    }
    if (!JS_IsObject(sv) || dyn_sc_is_array(ctx, sv)) {
        JS_ThrowTypeError(ctx,
            "dyna:schema: schema at \"%s\" must be an object or boolean", path);
        return -1;
    }

    n = dyn_sc_new_node(c);
    if (!n)
        return -1;
    n->kind = SC_SCHEMA;
    if (dyn_sc_index_add(c, path, plen, n))
        return -1;
    {
        dyn_scres_t* saved = c->cur;
        int rc;
        if (dyn_sc_enter_resource(c, sv, n, path, plen) || dyn_sc_read_anchors(c, sv, n))
            return -1;
        n->res = c->cur;
        c->depth++;
        rc = dyn_sc_compile_kw(c, sv, n, path, plen);
        c->depth--;
        if (saved)
            c->cur = saved;
        if (rc)
            return -1;
    }
    if (c->no_validation)
        n->f &= ~(uint64_t)SC_VALIDATION_FLAGS;
    *out = n;
    return 0;
}

static int dyn_sc_dep_kw(JSContext* ctx, JSValueConst sv, int schemas, JSValue* out)
{
    JSValue deps, obj;
    JSPropertyEnum* tab = NULL;
    uint32_t n = 0, i;
    int any = 0;
    if (dyn_sc_kw(ctx, sv, schemas ? "dependentSchemas" : "dependentRequired", out))
        return -1;
    if (!JS_IsUndefined(*out))
        return 0;
    if (dyn_sc_kw(ctx, sv, "dependencies", &deps))
        return -1;
    if (!JS_IsObject(deps) || dyn_sc_is_array(ctx, deps)) {
        JS_FreeValue(ctx, deps);
        return 0;
    }
    obj = JS_NewObject(ctx);
    if (JS_IsException(obj) || JS_GetOwnPropertyNames(ctx, &tab, &n, deps, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY)) {
        JS_FreeValue(ctx, obj);
        JS_FreeValue(ctx, deps);
        return -1;
    }
    for (i = 0; i < n; i++) {
        JSValue v = JS_GetProperty(ctx, deps, tab[i].atom);
        if (JS_IsException(v)) {
            JS_FreePropertyEnum(ctx, tab, n);
            JS_FreeValue(ctx, obj);
            JS_FreeValue(ctx, deps);
            return -1;
        }
        if ((dyn_sc_is_array(ctx, v) == 1) != (schemas != 0)) {
            JS_DefinePropertyValue(ctx, obj, tab[i].atom, v, JS_PROP_C_W_E);
            any = 1;
        } else {
            JS_FreeValue(ctx, v);
        }
    }
    JS_FreePropertyEnum(ctx, tab, n);
    JS_FreeValue(ctx, deps);
    if (any)
        *out = obj;
    else
        JS_FreeValue(ctx, obj);
    return 0;
}

static int dyn_sc_compile_kw(dyn_sccomp_t* c, JSValueConst sv,
    dyn_scnode_t* n, const char* path, size_t plen)
{
    JSContext* ctx = c->ctx;
    JSValue kw;
    int r;

    if (dyn_sc_kw(ctx, sv, "type", &kw))
        return -1;
    if (!JS_IsUndefined(kw)) {
        if (dyn_sc_parse_type(ctx, kw, &n->type_mask)) {
            JS_FreeValue(ctx, kw);
            return -1;
        }
        n->f |= SC_F_TYPE;
    }
    JS_FreeValue(ctx, kw);

    if (dyn_sc_kw(ctx, sv, "const", &kw))
        return -1;
    if (!JS_IsUndefined(kw)) {
        n->val = JS_DupValue(ctx, kw);
        n->f |= SC_F_CONST;
    }
    JS_FreeValue(ctx, kw);

    if (dyn_sc_kw(ctx, sv, "enum", &kw))
        return -1;
    if (!JS_IsUndefined(kw)) {
        uint64_t len, i;
        if (dyn_sc_check_list(ctx, kw, "enum", &len)) {
            JS_FreeValue(ctx, kw);
            return -1;
        }
        n->enumv = js_malloc(ctx, (len ? len : 1) * sizeof(JSValue));
        if (!n->enumv) {
            JS_FreeValue(ctx, kw);
            return -1;
        }
        for (i = 0; i < len; i++) {
            n->enumv[i] = JS_GetPropertyUint32(ctx, kw, (uint32_t)i);
            if (JS_IsException(n->enumv[i])) {
                int j;
                for (j = 0; j < (int)i; j++)
                    JS_FreeValue(ctx, n->enumv[j]);
                js_free(ctx, n->enumv);
                n->enumv = NULL;
                JS_FreeValue(ctx, kw);
                return -1;
            }
        }
        n->n_enum = (int)len;
        n->f |= SC_F_ENUM;
    }
    JS_FreeValue(ctx, kw);

    if (dyn_sc_kw(ctx, sv, "minimum", &kw))
        return -1;
    if (!JS_IsUndefined(kw)) {
        if (dyn_sc_read_num(ctx, kw, "minimum", &n->dmin)) {
            JS_FreeValue(ctx, kw);
            return -1;
        }
        n->f |= SC_F_MIN;
    }
    JS_FreeValue(ctx, kw);

    if (dyn_sc_kw(ctx, sv, "maximum", &kw))
        return -1;
    if (!JS_IsUndefined(kw)) {
        if (dyn_sc_read_num(ctx, kw, "maximum", &n->dmax)) {
            JS_FreeValue(ctx, kw);
            return -1;
        }
        n->f |= SC_F_MAX;
    }
    JS_FreeValue(ctx, kw);

    if (dyn_sc_kw(ctx, sv, "exclusiveMinimum", &kw))
        return -1;
    if (!JS_IsUndefined(kw)) {
        if (dyn_sc_read_num(ctx, kw, "exclusiveMinimum", &n->demin)) {
            JS_FreeValue(ctx, kw);
            return -1;
        }
        n->f |= SC_F_EMIN;
    }
    JS_FreeValue(ctx, kw);

    if (dyn_sc_kw(ctx, sv, "exclusiveMaximum", &kw))
        return -1;
    if (!JS_IsUndefined(kw)) {
        if (dyn_sc_read_num(ctx, kw, "exclusiveMaximum", &n->demax)) {
            JS_FreeValue(ctx, kw);
            return -1;
        }
        n->f |= SC_F_EMAX;
    }
    JS_FreeValue(ctx, kw);

    if (dyn_sc_kw(ctx, sv, "multipleOf", &kw))
        return -1;
    if (!JS_IsUndefined(kw)) {
        if (dyn_sc_read_num(ctx, kw, "multipleOf", &n->dmult)) {
            JS_FreeValue(ctx, kw);
            return -1;
        }
        if (!(n->dmult > 0)) {
            JS_ThrowTypeError(ctx,
                "dyna:schema: \"multipleOf\" must be greater than zero");
            JS_FreeValue(ctx, kw);
            return -1;
        }
        n->f |= SC_F_MULT;
    }
    JS_FreeValue(ctx, kw);

    if (dyn_sc_kw(ctx, sv, "minLength", &kw))
        return -1;
    if (!JS_IsUndefined(kw)) {
        if (dyn_sc_read_int(ctx, kw, "minLength", &n->minlen)) {
            JS_FreeValue(ctx, kw);
            return -1;
        }
        n->f |= SC_F_MINLEN;
    }
    JS_FreeValue(ctx, kw);

    if (dyn_sc_kw(ctx, sv, "maxLength", &kw))
        return -1;
    if (!JS_IsUndefined(kw)) {
        if (dyn_sc_read_int(ctx, kw, "maxLength", &n->maxlen)) {
            JS_FreeValue(ctx, kw);
            return -1;
        }
        n->f |= SC_F_MAXLEN;
    }
    JS_FreeValue(ctx, kw);

    if (dyn_sc_kw(ctx, sv, "pattern", &kw))
        return -1;
    if (!JS_IsUndefined(kw)) {
        size_t nl;
        const char* ps;
        if (!JS_IsString(kw)) {
            JS_ThrowTypeError(ctx, "dyna:schema: \"pattern\" must be a string");
            JS_FreeValue(ctx, kw);
            return -1;
        }
        ps = JS_ToCStringLen(ctx, &nl, kw);
        if (!ps) {
            JS_FreeValue(ctx, kw);
            return -1;
        }
        r = dyn_sc_compile_re(ctx, ps, nl, &n->re);
        JS_FreeCString(ctx, ps);
        JS_FreeValue(ctx, kw);
        if (r)
            return -1;
        n->f |= SC_F_PAT;
    }

    if (dyn_sc_kw(ctx, sv, "format", &kw))
        return -1;
    if (!JS_IsUndefined(kw) && JS_IsString(kw) && !(n->f & SC_F_FMT)) {
        const char* fs;
        size_t fl;
        fs = JS_ToCStringLen(ctx, &fl, kw);
        if (!fs) {
            JS_FreeValue(ctx, kw);
            return -1;
        }
        if (fl > 0 && strlen(fs) == fl) {
            n->fmt = (char*)js_malloc(ctx, fl + 1);
            if (!n->fmt) {
                JS_FreeCString(ctx, fs);
                JS_FreeValue(ctx, kw);
                return -1;
            }
            memcpy(n->fmt, fs, fl + 1);
            n->f |= SC_F_FMT;
        }
        JS_FreeCString(ctx, fs);
    }
    JS_FreeValue(ctx, kw);

    if (dyn_sc_kw(ctx, sv, "minItems", &kw))
        return -1;
    if (!JS_IsUndefined(kw)) {
        if (dyn_sc_read_int(ctx, kw, "minItems", &n->minitems)) {
            JS_FreeValue(ctx, kw);
            return -1;
        }
        n->f |= SC_F_MINITEMS;
    }
    JS_FreeValue(ctx, kw);

    if (dyn_sc_kw(ctx, sv, "maxItems", &kw))
        return -1;
    if (!JS_IsUndefined(kw)) {
        if (dyn_sc_read_int(ctx, kw, "maxItems", &n->maxitems)) {
            JS_FreeValue(ctx, kw);
            return -1;
        }
        n->f |= SC_F_MAXITEMS;
    }
    JS_FreeValue(ctx, kw);

    if (dyn_sc_kw(ctx, sv, "uniqueItems", &kw))
        return -1;
    if (!JS_IsUndefined(kw)) {
        if (!JS_IsBool(kw)) {
            JS_ThrowTypeError(ctx, "dyna:schema: \"uniqueItems\" must be a boolean");
            JS_FreeValue(ctx, kw);
            return -1;
        }
        n->unique = JS_ToBool(ctx, kw) > 0;
        n->f |= SC_F_UNIQUE;
    }
    JS_FreeValue(ctx, kw);

    if (dyn_sc_kw(ctx, sv, "minProperties", &kw))
        return -1;
    if (!JS_IsUndefined(kw)) {
        if (dyn_sc_read_int(ctx, kw, "minProperties", &n->minprops)) {
            JS_FreeValue(ctx, kw);
            return -1;
        }
        n->f |= SC_F_MINPROPS;
    }
    JS_FreeValue(ctx, kw);

    if (dyn_sc_kw(ctx, sv, "maxProperties", &kw))
        return -1;
    if (!JS_IsUndefined(kw)) {
        if (dyn_sc_read_int(ctx, kw, "maxProperties", &n->maxprops)) {
            JS_FreeValue(ctx, kw);
            return -1;
        }
        n->f |= SC_F_MAXPROPS;
    }
    JS_FreeValue(ctx, kw);

    if (dyn_sc_kw(ctx, sv, "required", &kw))
        return -1;
    if (!JS_IsUndefined(kw)) {
        uint64_t len, i;
        if (dyn_sc_check_list(ctx, kw, "required", &len)) {
            JS_FreeValue(ctx, kw);
            return -1;
        }
        n->required.names = js_mallocz(ctx, (len ? len : 1) * sizeof(char*));
        n->required.v = js_mallocz(ctx, (len ? len : 1) * sizeof(dyn_scnode_t*));
        if (!n->required.names || !n->required.v) {
            js_free(ctx, n->required.names);
            js_free(ctx, n->required.v);
            n->required.names = NULL;
            n->required.v = NULL;
            JS_FreeValue(ctx, kw);
            return -1;
        }
        n->required.n = (int)len;
        for (i = 0; i < len; i++) {
            JSValue el = JS_GetPropertyUint32(ctx, kw, (uint32_t)i);
            size_t nl;
            const char* nb;
            if (JS_IsException(el)) {
                JS_FreeValue(ctx, kw);
                return -1;
            }
            if (!JS_IsString(el)) {
                JS_ThrowTypeError(ctx,
                    "dyna:schema: every element of \"required\" must be a string");
                JS_FreeValue(ctx, el);
                JS_FreeValue(ctx, kw);
                return -1;
            }
            nb = JS_ToCStringLen(ctx, &nl, el);
            if (!nb) {
                JS_FreeValue(ctx, el);
                JS_FreeValue(ctx, kw);
                return -1;
            }
            n->required.names[i] = js_malloc(ctx, nl + 1);
            if (!n->required.names[i]) {
                JS_FreeCString(ctx, nb);
                JS_FreeValue(ctx, el);
                JS_FreeValue(ctx, kw);
                return -1;
            }
            memcpy(n->required.names[i], nb, nl);
            n->required.names[i][nl] = '\0';
            JS_FreeCString(ctx, nb);
            JS_FreeValue(ctx, el);
        }
        JS_FreeValue(ctx, kw);
        n->f |= SC_F_REQ;
    }

    if (dyn_sc_kw(ctx, sv, "properties", &kw))
        return -1;
    if (!JS_IsUndefined(kw)) {
        char** names;
        JSValue* vals;
        int i, nk;
        if (dyn_sc_read_map(ctx, kw, "properties", &names, &vals, &nk)) {
            JS_FreeValue(ctx, kw);
            return -1;
        }
        n->props.names = js_mallocz(ctx, (nk ? nk : 1) * sizeof(char*));
        n->props.v = js_mallocz(ctx, (nk ? nk : 1) * sizeof(dyn_scnode_t*));
        if (!n->props.names || !n->props.v) {
            int j;
            for (j = 0; j < nk; j++) {
                js_free(ctx, names[j]);
                JS_FreeValue(ctx, vals[j]);
            }
            js_free(ctx, names);
            js_free(ctx, vals);
            js_free(ctx, n->props.names);
            js_free(ctx, n->props.v);
            n->props.names = NULL;
            n->props.v = NULL;
            JS_FreeValue(ctx, kw);
            return -1;
        }
        n->props.n = nk;
        for (i = 0; i < nk; i++) {
            dyn_scnode_t* sub;
            n->props.names[i] = names[i];
            if (dyn_sc_compile_at(c, vals[i], path, plen, "properties",
                    names[i], strlen(names[i]), &sub)) {
                int j;
                for (j = i; j < nk; j++)
                    JS_FreeValue(ctx, vals[j]);
                for (j = i + 1; j < nk; j++)
                    js_free(ctx, names[j]);
                js_free(ctx, names);
                js_free(ctx, vals);
                JS_FreeValue(ctx, kw);
                return -1;
            }
            n->props.v[i] = sub;
            JS_FreeValue(ctx, vals[i]);
        }
        js_free(ctx, names);
        js_free(ctx, vals);
        n->f |= SC_F_PROPS;
    }
    JS_FreeValue(ctx, kw);

    if (dyn_sc_kw(ctx, sv, "patternProperties", &kw))
        return -1;
    if (!JS_IsUndefined(kw)) {
        char** names;
        JSValue* vals;
        int i, nk;
        if (dyn_sc_read_map(ctx, kw, "patternProperties", &names, &vals,
                &nk)) {
            JS_FreeValue(ctx, kw);
            return -1;
        }
        n->patts = js_mallocz(ctx, (nk ? nk : 1) * sizeof(dyn_scpat_t));
        if (!n->patts) {
            int j;
            for (j = 0; j < nk; j++) {
                js_free(ctx, names[j]);
                JS_FreeValue(ctx, vals[j]);
            }
            js_free(ctx, names);
            js_free(ctx, vals);
            JS_FreeValue(ctx, kw);
            return -1;
        }
        n->n_patts = nk;
        for (i = 0; i < nk; i++) {
            size_t nl = strlen(names[i]);
            dyn_scnode_t* sub;
            if (dyn_sc_compile_re(ctx, names[i], nl, &n->patts[i].re)) {
                int j;
                for (j = i; j < nk; j++) {
                    js_free(ctx, names[j]);
                    JS_FreeValue(ctx, vals[j]);
                }
                js_free(ctx, names);
                js_free(ctx, vals);
                JS_FreeValue(ctx, kw);
                return -1;
            }
            n->patts[i].pat = names[i];
            if (dyn_sc_compile_at(c, vals[i], path, plen,
                    "patternProperties", names[i], nl,
                    &sub)) {
                int j;
                for (j = i + 1; j < nk; j++)
                    js_free(ctx, names[j]);
                for (j = i; j < nk; j++)
                    JS_FreeValue(ctx, vals[j]);
                js_free(ctx, names);
                js_free(ctx, vals);
                JS_FreeValue(ctx, kw);
                return -1;
            }
            n->patts[i].node = sub;
            JS_FreeValue(ctx, vals[i]);
        }
        js_free(ctx, names);
        js_free(ctx, vals);
        n->f |= SC_F_PATTS;
    }
    JS_FreeValue(ctx, kw);

    if (dyn_sc_kw(ctx, sv, "additionalProperties", &kw))
        return -1;
    if (!JS_IsUndefined(kw)) {
        n->f |= SC_F_ADDL;
        if (JS_IsBool(kw)) {
            n->addl_allow = JS_ToBool(ctx, kw) > 0;
        } else if (JS_IsObject(kw) && !dyn_sc_is_array(ctx, kw)) {
            if (dyn_sc_compile_sub(c, kw, path, plen, "additionalProperties",
                    strlen("additionalProperties"), &n->addl)) {
                JS_FreeValue(ctx, kw);
                return -1;
            }
        } else {
            JS_ThrowTypeError(ctx,
                "dyna:schema: \"additionalProperties\" must be a boolean or a schema");
            JS_FreeValue(ctx, kw);
            return -1;
        }
    }
    JS_FreeValue(ctx, kw);

    if (dyn_sc_kw(ctx, sv, "unevaluatedProperties", &kw))
        return -1;
    if (!JS_IsUndefined(kw)) {
        n->f |= SC_F_UNEVALP;
        if (JS_IsBool(kw)) {
            n->unevalp_allow = JS_ToBool(ctx, kw) > 0;
        } else if (JS_IsObject(kw) && !dyn_sc_is_array(ctx, kw)) {
            if (dyn_sc_compile_sub(c, kw, path, plen, "unevaluatedProperties",
                    strlen("unevaluatedProperties"),
                    &n->unevalp)) {
                JS_FreeValue(ctx, kw);
                return -1;
            }
        } else {
            JS_ThrowTypeError(ctx,
                "dyna:schema: \"unevaluatedProperties\" must be a boolean or "
                "a schema");
            JS_FreeValue(ctx, kw);
            return -1;
        }
    }
    JS_FreeValue(ctx, kw);

    if (dyn_sc_kw(ctx, sv, "unevaluatedItems", &kw))
        return -1;
    if (!JS_IsUndefined(kw)) {
        n->f |= SC_F_UNEVALI;
        if (JS_IsBool(kw)) {
            n->unevali_allow = JS_ToBool(ctx, kw) > 0;
        } else if (JS_IsObject(kw) && !dyn_sc_is_array(ctx, kw)) {
            if (dyn_sc_compile_sub(c, kw, path, plen, "unevaluatedItems",
                    strlen("unevaluatedItems"),
                    &n->unevali)) {
                JS_FreeValue(ctx, kw);
                return -1;
            }
        } else {
            JS_ThrowTypeError(ctx,
                "dyna:schema: \"unevaluatedItems\" must be a boolean or a "
                "schema");
            JS_FreeValue(ctx, kw);
            return -1;
        }
    }
    JS_FreeValue(ctx, kw);

    if (dyn_sc_kw(ctx, sv, "prefixItems", &kw))
        return -1;
    if (!JS_IsUndefined(kw)) {
        uint64_t len, i;
        if (dyn_sc_check_list(ctx, kw, "prefixItems", &len)) {
            JS_FreeValue(ctx, kw);
            return -1;
        }
        n->prefix.v = js_mallocz(ctx, (len ? len : 1) * sizeof(dyn_scnode_t*));
        if (!n->prefix.v) {
            JS_FreeValue(ctx, kw);
            return -1;
        }
        n->prefix.n = (int)len;
        for (i = 0; i < len; i++) {
            char idx[24];
            int il = snprintf(idx, sizeof idx, "%llu",
                (unsigned long long)i);
            JSValue el = JS_GetPropertyUint32(ctx, kw, (uint32_t)i);
            dyn_scnode_t* sub;
            if (JS_IsException(el)) {
                JS_FreeValue(ctx, kw);
                return -1;
            }
            if (dyn_sc_compile_at(c, el, path, plen, "prefixItems", idx,
                    (size_t)il, &sub)) {
                JS_FreeValue(ctx, el);
                JS_FreeValue(ctx, kw);
                return -1;
            }
            JS_FreeValue(ctx, el);
            n->prefix.v[i] = sub;
        }
        n->f |= SC_F_PREFIX;
    }
    JS_FreeValue(ctx, kw);

    if (dyn_sc_kw(ctx, sv, "items", &kw))
        return -1;
    if (!JS_IsUndefined(kw)) {
        if (dyn_sc_is_array(ctx, kw)) {
            JS_ThrowTypeError(ctx,
                "dyna:schema: \"items\" must be a schema (use prefixItems for "
                "a tuple)");
            JS_FreeValue(ctx, kw);
            return -1;
        }
        if (!JS_IsBool(kw) && !JS_IsObject(kw)) {
            JS_ThrowTypeError(ctx,
                "dyna:schema: \"items\" must be a boolean or a schema");
            JS_FreeValue(ctx, kw);
            return -1;
        }
        if (dyn_sc_compile_sub(c, kw, path, plen, "items", strlen("items"),
                &n->items)) {
            JS_FreeValue(ctx, kw);
            return -1;
        }
        n->f |= SC_F_ITEMS;
    }
    JS_FreeValue(ctx, kw);

    if (dyn_sc_kw(ctx, sv, "contains", &kw))
        return -1;
    if (!JS_IsUndefined(kw)) {
        if (!JS_IsBool(kw) && !JS_IsObject(kw)) {
            JS_ThrowTypeError(ctx,
                "dyna:schema: \"contains\" must be a boolean or a schema");
            JS_FreeValue(ctx, kw);
            return -1;
        }
        if (dyn_sc_compile_sub(c, kw, path, plen, "contains",
                strlen("contains"), &n->cont)) {
            JS_FreeValue(ctx, kw);
            return -1;
        }
        n->f |= SC_F_CONT;
    }
    JS_FreeValue(ctx, kw);

    if (dyn_sc_kw(ctx, sv, "minContains", &kw))
        return -1;
    if (!JS_IsUndefined(kw)) {
        if (dyn_sc_read_int(ctx, kw, "minContains", &n->mincont)) {
            JS_FreeValue(ctx, kw);
            return -1;
        }
        n->f |= SC_F_MINCONT;
    }
    JS_FreeValue(ctx, kw);

    if (dyn_sc_kw(ctx, sv, "maxContains", &kw))
        return -1;
    if (!JS_IsUndefined(kw)) {
        if (dyn_sc_read_int(ctx, kw, "maxContains", &n->maxcont)) {
            JS_FreeValue(ctx, kw);
            return -1;
        }
        n->f |= SC_F_MAXCONT;
    }
    JS_FreeValue(ctx, kw);

    if (dyn_sc_dep_kw(ctx, sv, 0, &kw))
        return -1;
    if (!JS_IsUndefined(kw)) {
        char** names;
        JSValue* vals;
        int i, k, nk;
        if (dyn_sc_read_map(ctx, kw, "dependentRequired", &names, &vals,
                &nk)) {
            JS_FreeValue(ctx, kw);
            return -1;
        }
        n->depreq.props = names;
        n->depreq.need = js_mallocz(ctx, (nk ? nk : 1) * sizeof(char**));
        n->depreq.nneed = js_mallocz(ctx, (nk ? nk : 1) * sizeof(int));
        if (!n->depreq.need || !n->depreq.nneed) {
            js_free(ctx, n->depreq.need);
            js_free(ctx, n->depreq.nneed);
            n->depreq.need = NULL;
            n->depreq.nneed = NULL;
            for (k = 0; k < nk; k++)
                JS_FreeValue(ctx, vals[k]);
            js_free(ctx, vals);
            JS_FreeValue(ctx, kw);
            return -1;
        }
        n->depreq.n = nk;
        for (i = 0; i < nk; i++) {
            uint64_t m, j;
            char** need;
            uint64_t t;
            if (dyn_sc_is_array(ctx, vals[i]) != 1) {
                JS_ThrowTypeError(ctx,
                    "dyna:schema: each \"dependentRequired\" value must be an "
                    "array of strings");
                goto dep_fail;
            }
            m = dyn_sc_arr_len(ctx, vals[i]);
            if (m == (uint64_t)-1)
                goto dep_fail;
            if (m > DYN_SC_MAX_LIST) {
                JS_ThrowTypeError(ctx,
                    "dyna:schema: \"dependentRequired\" list is too large");
                goto dep_fail;
            }
            need = js_mallocz(ctx, (m ? m : 1) * sizeof(char*));
            if (!need)
                goto dep_fail;
            for (j = 0; j < m; j++) {
                JSValue el = JS_GetPropertyUint32(ctx, vals[i], (uint32_t)j);
                size_t eln;
                const char* eb;
                if (JS_IsException(el)) {
                    for (t = 0; t < j; t++)
                        js_free(ctx, need[t]);
                    js_free(ctx, need);
                    goto dep_fail;
                }
                if (!JS_IsString(el)) {
                    JS_ThrowTypeError(ctx,
                        "dyna:schema: each \"dependentRequired\" value must be "
                        "an array of strings");
                    JS_FreeValue(ctx, el);
                    for (t = 0; t < j; t++)
                        js_free(ctx, need[t]);
                    js_free(ctx, need);
                    goto dep_fail;
                }
                eb = JS_ToCStringLen(ctx, &eln, el);
                if (!eb) {
                    JS_FreeValue(ctx, el);
                    for (t = 0; t < j; t++)
                        js_free(ctx, need[t]);
                    js_free(ctx, need);
                    goto dep_fail;
                }
                need[j] = js_malloc(ctx, eln + 1);
                if (!need[j]) {
                    JS_FreeCString(ctx, eb);
                    JS_FreeValue(ctx, el);
                    for (t = 0; t < j; t++)
                        js_free(ctx, need[t]);
                    js_free(ctx, need);
                    goto dep_fail;
                }
                memcpy(need[j], eb, eln);
                need[j][eln] = '\0';
                JS_FreeCString(ctx, eb);
                JS_FreeValue(ctx, el);
            }
            n->depreq.need[i] = need;
            n->depreq.nneed[i] = (int)m;
            JS_FreeValue(ctx, vals[i]);
            continue;
        dep_fail:
            JS_FreeValue(ctx, vals[i]);
            for (k = i + 1; k < nk; k++)
                JS_FreeValue(ctx, vals[k]);
            js_free(ctx, vals);
            JS_FreeValue(ctx, kw);
            return -1;
        }
        js_free(ctx, vals);
        n->f |= SC_F_DEPREQ;
    }
    JS_FreeValue(ctx, kw);

    if (dyn_sc_kw(ctx, sv, "propertyNames", &kw))
        return -1;
    if (!JS_IsUndefined(kw)) {
        if (!JS_IsBool(kw) && !JS_IsObject(kw)) {
            JS_ThrowTypeError(ctx,
                "dyna:schema: \"propertyNames\" must be a boolean or a schema");
            JS_FreeValue(ctx, kw);
            return -1;
        }
        if (dyn_sc_compile_sub(c, kw, path, plen, "propertyNames",
                strlen("propertyNames"), &n->pn)) {
            JS_FreeValue(ctx, kw);
            return -1;
        }
        n->f |= SC_F_PNAMES;
    }
    JS_FreeValue(ctx, kw);

    if (dyn_sc_dep_kw(ctx, sv, 1, &kw))
        return -1;
    if (!JS_IsUndefined(kw)) {
        char** names;
        JSValue* vals;
        int i, nk;
        if (dyn_sc_read_map(ctx, kw, "dependentSchemas", &names, &vals,
                &nk)) {
            JS_FreeValue(ctx, kw);
            return -1;
        }
        n->depsch.names = js_mallocz(ctx, (nk ? nk : 1) * sizeof(char*));
        n->depsch.v = js_mallocz(ctx, (nk ? nk : 1) * sizeof(dyn_scnode_t*));
        if (!n->depsch.names || !n->depsch.v) {
            int j;
            for (j = 0; j < nk; j++) {
                js_free(ctx, names[j]);
                JS_FreeValue(ctx, vals[j]);
            }
            js_free(ctx, names);
            js_free(ctx, vals);
            js_free(ctx, n->depsch.names);
            js_free(ctx, n->depsch.v);
            n->depsch.names = NULL;
            n->depsch.v = NULL;
            JS_FreeValue(ctx, kw);
            return -1;
        }
        n->depsch.n = nk;
        for (i = 0; i < nk; i++) {
            dyn_scnode_t* sub;
            n->depsch.names[i] = names[i];
            if (dyn_sc_compile_at(c, vals[i], path, plen, "dependentSchemas",
                    names[i], strlen(names[i]), &sub)) {
                int j;
                for (j = i; j < nk; j++)
                    JS_FreeValue(ctx, vals[j]);
                for (j = i + 1; j < nk; j++)
                    js_free(ctx, names[j]);
                js_free(ctx, names);
                js_free(ctx, vals);
                JS_FreeValue(ctx, kw);
                return -1;
            }
            n->depsch.v[i] = sub;
            JS_FreeValue(ctx, vals[i]);
        }
        js_free(ctx, names);
        js_free(ctx, vals);
        n->f |= SC_F_DEPSCH;
    }
    JS_FreeValue(ctx, kw);

    if (dyn_sc_kw(ctx, sv, "allOf", &kw))
        return -1;
    if (!JS_IsUndefined(kw)) {
        uint64_t len, i;
        if (dyn_sc_check_list(ctx, kw, "allOf", &len)) {
            JS_FreeValue(ctx, kw);
            return -1;
        }
        n->all.v = js_mallocz(ctx, (len ? len : 1) * sizeof(dyn_scnode_t*));
        if (!n->all.v) {
            JS_FreeValue(ctx, kw);
            return -1;
        }
        n->all.n = (int)len;
        for (i = 0; i < len; i++) {
            char idx[24];
            int il = snprintf(idx, sizeof idx, "%llu",
                (unsigned long long)i);
            JSValue el = JS_GetPropertyUint32(ctx, kw, (uint32_t)i);
            dyn_scnode_t* sub;
            if (JS_IsException(el)) {
                JS_FreeValue(ctx, kw);
                return -1;
            }
            if (dyn_sc_compile_at(c, el, path, plen, "allOf", idx,
                    (size_t)il, &sub)) {
                JS_FreeValue(ctx, el);
                JS_FreeValue(ctx, kw);
                return -1;
            }
            JS_FreeValue(ctx, el);
            n->all.v[i] = sub;
        }
        n->f |= SC_F_ALL;
    }
    JS_FreeValue(ctx, kw);

    if (dyn_sc_kw(ctx, sv, "anyOf", &kw))
        return -1;
    if (!JS_IsUndefined(kw)) {
        uint64_t len, i;
        if (dyn_sc_check_list(ctx, kw, "anyOf", &len)) {
            JS_FreeValue(ctx, kw);
            return -1;
        }
        n->any.v = js_mallocz(ctx, (len ? len : 1) * sizeof(dyn_scnode_t*));
        if (!n->any.v) {
            JS_FreeValue(ctx, kw);
            return -1;
        }
        n->any.n = (int)len;
        for (i = 0; i < len; i++) {
            char idx[24];
            int il = snprintf(idx, sizeof idx, "%llu",
                (unsigned long long)i);
            JSValue el = JS_GetPropertyUint32(ctx, kw, (uint32_t)i);
            dyn_scnode_t* sub;
            if (JS_IsException(el)) {
                JS_FreeValue(ctx, kw);
                return -1;
            }
            if (dyn_sc_compile_at(c, el, path, plen, "anyOf", idx,
                    (size_t)il, &sub)) {
                JS_FreeValue(ctx, el);
                JS_FreeValue(ctx, kw);
                return -1;
            }
            JS_FreeValue(ctx, el);
            n->any.v[i] = sub;
        }
        n->f |= SC_F_ANY;
    }
    JS_FreeValue(ctx, kw);

    if (dyn_sc_kw(ctx, sv, "oneOf", &kw))
        return -1;
    if (!JS_IsUndefined(kw)) {
        uint64_t len, i;
        if (dyn_sc_check_list(ctx, kw, "oneOf", &len)) {
            JS_FreeValue(ctx, kw);
            return -1;
        }
        n->one.v = js_mallocz(ctx, (len ? len : 1) * sizeof(dyn_scnode_t*));
        if (!n->one.v) {
            JS_FreeValue(ctx, kw);
            return -1;
        }
        n->one.n = (int)len;
        for (i = 0; i < len; i++) {
            char idx[24];
            int il = snprintf(idx, sizeof idx, "%llu",
                (unsigned long long)i);
            JSValue el = JS_GetPropertyUint32(ctx, kw, (uint32_t)i);
            dyn_scnode_t* sub;
            if (JS_IsException(el)) {
                JS_FreeValue(ctx, kw);
                return -1;
            }
            if (dyn_sc_compile_at(c, el, path, plen, "oneOf", idx,
                    (size_t)il, &sub)) {
                JS_FreeValue(ctx, el);
                JS_FreeValue(ctx, kw);
                return -1;
            }
            JS_FreeValue(ctx, el);
            n->one.v[i] = sub;
        }
        n->f |= SC_F_ONE;
    }
    JS_FreeValue(ctx, kw);

    if (dyn_sc_kw(ctx, sv, "not", &kw))
        return -1;
    if (!JS_IsUndefined(kw)) {
        if (dyn_sc_compile_sub(c, kw, path, plen, "not", strlen("not"),
                &n->notn)) {
            JS_FreeValue(ctx, kw);
            return -1;
        }
        n->f |= SC_F_NOT;
    }
    JS_FreeValue(ctx, kw);

    if (dyn_sc_kw(ctx, sv, "if", &kw))
        return -1;
    if (!JS_IsUndefined(kw)) {
        if (dyn_sc_compile_sub(c, kw, path, plen, "if", strlen("if"),
                &n->ifn)) {
            JS_FreeValue(ctx, kw);
            return -1;
        }
        n->f |= SC_F_IF;
    }
    JS_FreeValue(ctx, kw);

    {
        if (dyn_sc_kw(ctx, sv, "then", &kw))
            return -1;
        if (!JS_IsUndefined(kw)) {
            if (dyn_sc_compile_sub(c, kw, path, plen, "then", strlen("then"),
                    &n->then)) {
                JS_FreeValue(ctx, kw);
                return -1;
            }
        }
        JS_FreeValue(ctx, kw);

        if (dyn_sc_kw(ctx, sv, "else", &kw))
            return -1;
        if (!JS_IsUndefined(kw)) {
            if (dyn_sc_compile_sub(c, kw, path, plen, "else", strlen("else"),
                    &n->els)) {
                JS_FreeValue(ctx, kw);
                return -1;
            }
        }
        JS_FreeValue(ctx, kw);
    }

    {
        int got = dyn_sc_str_kw(ctx, sv, "$dynamicRef", &n->dref);
        if (got < 0)
            return -1;
        if (got)
            n->f |= SC_F_DREF;
    }

    if (dyn_sc_kw(ctx, sv, "$ref", &kw))
        return -1;
    if (!JS_IsUndefined(kw)) {
        size_t nl;
        const char* rs;
        if (!JS_IsString(kw)) {
            JS_ThrowTypeError(ctx, "dyna:schema: \"$ref\" must be a string");
            JS_FreeValue(ctx, kw);
            return -1;
        }
        rs = JS_ToCStringLen(ctx, &nl, kw);
        if (!rs) {
            JS_FreeValue(ctx, kw);
            return -1;
        }
        n->ref = js_malloc(ctx, nl + 1);
        if (!n->ref) {
            JS_FreeCString(ctx, rs);
            JS_FreeValue(ctx, kw);
            return -1;
        }
        memcpy(n->ref, rs, nl);
        n->ref[nl] = '\0';
        JS_FreeCString(ctx, rs);
        JS_FreeValue(ctx, kw);
        n->f |= SC_F_REF;
    }

    if (dyn_sc_kw(ctx, sv, "$defs", &kw))
        return -1;
    if (!JS_IsUndefined(kw)) {
        char** names;
        JSValue* vals;
        int i, nk;
        char* defpath;
        size_t deflen;
        if (dyn_sc_read_map(ctx, kw, "$defs", &names, &vals, &nk)) {
            JS_FreeValue(ctx, kw);
            return -1;
        }
        defpath = dyn_sc_child_path(c->ctx, path, plen, "$defs", 5, &deflen);
        if (!defpath) {
            for (i = 0; i < nk; i++) {
                js_free(ctx, names[i]);
                JS_FreeValue(ctx, vals[i]);
            }
            js_free(ctx, names);
            js_free(ctx, vals);
            JS_FreeValue(ctx, kw);
            return -1;
        }
        for (i = 0; i < nk; i++) {
            dyn_scnode_t* sub;
            if (dyn_sc_compile_sub(c, vals[i], defpath, deflen, names[i],
                    strlen(names[i]), &sub)) {
                int j;
                for (j = i; j < nk; j++)
                    JS_FreeValue(ctx, vals[j]);
                for (j = i; j < nk; j++)
                    js_free(ctx, names[j]);
                js_free(ctx, names);
                js_free(ctx, vals);
                js_free(c->ctx, defpath);
                JS_FreeValue(ctx, kw);
                return -1;
            }
            JS_FreeValue(ctx, vals[i]);
            js_free(ctx, names[i]);
        }
        js_free(c->ctx, defpath);
        js_free(ctx, names);
        js_free(ctx, vals);
    }
    JS_FreeValue(ctx, kw);

    if (dyn_sc_kw(ctx, sv, "definitions", &kw))
        return -1;
    if (!JS_IsUndefined(kw)) {
        char** names;
        JSValue* vals;
        int i, nk;
        char* defpath;
        size_t deflen;
        if (dyn_sc_read_map(ctx, kw, "definitions", &names, &vals, &nk)) {
            JS_FreeValue(ctx, kw);
            return -1;
        }
        defpath = dyn_sc_child_path(c->ctx, path, plen, "definitions", 11, &deflen);
        if (!defpath) {
            for (i = 0; i < nk; i++) {
                js_free(ctx, names[i]);
                JS_FreeValue(ctx, vals[i]);
            }
            js_free(ctx, names);
            js_free(ctx, vals);
            JS_FreeValue(ctx, kw);
            return -1;
        }
        for (i = 0; i < nk; i++) {
            dyn_scnode_t* sub;
            if (dyn_sc_compile_sub(c, vals[i], defpath, deflen, names[i],
                    strlen(names[i]), &sub)) {
                int j;
                for (j = i; j < nk; j++)
                    JS_FreeValue(ctx, vals[j]);
                for (j = i; j < nk; j++)
                    js_free(ctx, names[j]);
                js_free(ctx, names);
                js_free(ctx, vals);
                js_free(c->ctx, defpath);
                JS_FreeValue(ctx, kw);
                return -1;
            }
            JS_FreeValue(ctx, vals[i]);
            js_free(ctx, names[i]);
        }
        js_free(c->ctx, defpath);
        js_free(ctx, names);
        js_free(ctx, vals);
    }
    JS_FreeValue(ctx, kw);

    return 0;
}

static char* dyn_sc_pct_decode(JSContext* ctx, const char* s)
{
    size_t n = strlen(s);
    char* out = js_malloc(ctx, n + 1);
    dyn_pct_buf_t b;
    if (!out)
        return NULL;
    b.p = out;
    b.n = 0;
    dyn_pct_decode_core(&b, dyn_pct_buf_sink, s, n, 0);
    out[b.n] = '\0';
    return out;
}

static int dyn_sc_compile_pointer(dyn_sccomp_t* c, dyn_scres_t* r, const char* gp,
    size_t gl, dyn_scnode_t** out)
{
    JSContext* ctx = c->ctx;
    int d, best = -1;
    size_t bl = 0, i;
    JSValue cur;
    dyn_scres_t* saved = c->cur;
    int rc;
    *out = NULL;
    for (d = 0; d < c->ndocv; d++) {
        size_t l = strlen(c->docp[d]);
        if (l <= gl && !memcmp(gp, c->docp[d], l) && (gp[l] == '/' || gp[l] == 0) && (best < 0 || l >= bl)) {
            best = d;
            bl = l;
        }
    }
    if (best < 0)
        return 0;
    cur = JS_DupValue(ctx, c->docv[best]);
    for (i = bl; i < gl;) {
        size_t j = i + 1, k;
        char* tok;
        JSValue nx;
        while (j < gl && gp[j] != '/')
            j++;
        tok = js_malloc(ctx, j - i);
        if (!tok) {
            JS_FreeValue(ctx, cur);
            return -1;
        }
        for (k = 0, i++; i < j; i++) {
            if (gp[i] == '~' && i + 1 < j && (gp[i + 1] == '0' || gp[i + 1] == '1')) {
                tok[k++] = gp[i + 1] == '0' ? '~' : '/';
                i++;
            } else {
                tok[k++] = gp[i];
            }
        }
        tok[k] = 0;
        if (!JS_IsObject(cur)) {
            js_free(ctx, tok);
            JS_FreeValue(ctx, cur);
            return 0;
        }
        nx = JS_GetPropertyStr(ctx, cur, tok);
        js_free(ctx, tok);
        JS_FreeValue(ctx, cur);
        if (JS_IsException(nx))
            return -1;
        cur = nx;
        i = j;
    }
    if (!JS_IsBool(cur) && (!JS_IsObject(cur) || dyn_sc_is_array(ctx, cur))) {
        JS_FreeValue(ctx, cur);
        return 0;
    }
    c->cur = r;
    rc = dyn_sc_compile(c, cur, gp, gl, out);
    c->cur = saved;
    JS_FreeValue(ctx, cur);
    return rc;
}

static int dyn_sc_lookup(dyn_sccomp_t* c, dyn_scnode_t* n, const char* ref,
    const char* kw, dyn_scnode_t** tgt, char** dyn_name)
{
    JSContext* ctx = c->ctx;
    const char* base = n->res ? n->res->uri : c->base_uri;
    char *full, *hash, *frag;
    size_t ulen;
    dyn_scres_t* r;
    dyn_scnode_t* t = NULL;

    *tgt = NULL;
    full = dyn_uri_resolve(ctx, base, ref);
    if (!full)
        return -1;
    hash = strchr(full, '#');
    ulen = hash ? (size_t)(hash - full) : strlen(full);
    r = dyn_sc_res_find(c->sch, full, ulen);
    if (!r) {
        if (dyn_sc_load_doc(c, full, ulen) < 0) {
            js_free(ctx, full);
            return -1;
        }
        r = dyn_sc_res_find(c->sch, full, ulen);
    }
    if (!r) {
        JS_ThrowTypeError(ctx, "dyna:schema: unresolved %s \"%s\": no schema resource \"%.*s\" (pass it in { resources })",
            kw, ref, (int)ulen, full);
        js_free(ctx, full);
        return -1;
    }
    frag = dyn_sc_pct_decode(ctx, hash ? hash + 1 : "");
    js_free(ctx, full);
    if (!frag)
        return -1;
    if (!frag[0]) {
        t = r->root;
    } else if (frag[0] == '/') {
        size_t pl = strlen(r->prefix), fl = strlen(frag);
        char* gp = js_malloc(ctx, pl + fl + 1);
        if (!gp) {
            js_free(ctx, frag);
            return -1;
        }
        memcpy(gp, r->prefix, pl);
        memcpy(gp + pl, frag, fl + 1);
        t = dyn_sc_index_find(c, gp, pl + fl);
        if (!t && dyn_sc_compile_pointer(c, r, gp, pl + fl, &t) < 0) {
            js_free(ctx, gp);
            js_free(ctx, frag);
            return -1;
        }
        js_free(ctx, gp);
    } else {
        t = dyn_sc_res_anchor(r, frag, 0);
        if (t && dyn_name && dyn_sc_res_anchor(r, frag, 1) == t) {
            *dyn_name = frag;
            frag = NULL;
        }
    }
    if (!t) {
        JS_ThrowTypeError(ctx, "dyna:schema: unresolved %s \"%s\"", kw, ref);
        js_free(ctx, frag);
        return -1;
    }
    js_free(ctx, frag);
    *tgt = t;
    return 0;
}

static int dyn_sc_resolve_ref(dyn_sccomp_t* c, dyn_scnode_t* n)
{
    if ((n->f & SC_F_REF) && dyn_sc_lookup(c, n, n->ref, "$ref", &n->ref_tgt, NULL))
        return -1;
    if ((n->f & SC_F_DREF) && dyn_sc_lookup(c, n, n->dref, "$dynamicRef", &n->dref_tgt, &n->dref_name))
        return -1;
    return 0;
}

static int dyn_sc_resolve_refs(dyn_sccomp_t* c)
{
    int i;
    for (i = 0; i < c->sch->n_nodes; i++) {
        dyn_scnode_t* n = c->sch->nodes[i];
        if (n->f & (SC_F_REF | SC_F_DREF))
            if (dyn_sc_resolve_ref(c, n))
                return -1;
    }
    return dyn_sc_check_pure_ref_cycles(c);
}

static int dyn_sc_dyn_targets(dyn_schema_t* sch, dyn_scnode_t* m, int* rcount)
{
    int r, a, cnt = 0;
    for (r = 0; m->dref_name && r < sch->n_res; r++)
        for (a = 0; a < sch->res[r]->na; a++)
            if (sch->res[r]->adyn[a] && !strcmp(sch->res[r]->anames[a], m->dref_name)) {
                cnt++;
                if (rcount)
                    rcount[sch->res[r]->anodes[a]->idx]++;
            }
    return cnt;
}

static int dyn_sc_compute_ureach(JSContext* ctx, dyn_schema_t* sch)
{
    int i, n = sch->n_nodes, nedges = 0;
    int *rcount = NULL, *rstart = NULL, *redge = NULL, *fill = NULL;
    int* queue = NULL;
    int head = 0, tail = 0;
    int ret = -1;

    if (n <= 0)
        return 0;
    for (i = 0; i < n; i++) {
        dyn_scnode_t* m = sch->nodes[i];
        nedges += m->all.n + m->depsch.n + (m->then ? 1 : 0)
            + (m->els ? 1 : 0) + ((m->f & SC_F_REF) ? 1 : 0);
        if (m->f & SC_F_DREF)
            nedges += 1 + dyn_sc_dyn_targets(sch, m, NULL);
    }
    rcount = js_mallocz(ctx, ((size_t)n + 1) * sizeof(int));
    rstart = js_mallocz(ctx, ((size_t)n + 2) * sizeof(int));
    redge = js_mallocz(ctx, (size_t)(nedges ? nedges : 1) * sizeof(int));
    fill = js_mallocz(ctx, (size_t)n * sizeof(int));
    queue = js_mallocz(ctx, (size_t)n * sizeof(int));
    if (!rcount || !rstart || !redge || !fill || !queue)
        goto out;

    for (i = 0; i < n; i++) {
        dyn_scnode_t* m = sch->nodes[i];
        int j;
        for (j = 0; j < m->all.n; j++)
            rcount[m->all.v[j]->idx]++;
        for (j = 0; j < m->depsch.n; j++)
            rcount[m->depsch.v[j]->idx]++;
        if (m->then)
            rcount[m->then->idx]++;
        if (m->els)
            rcount[m->els->idx]++;
        if (m->f & SC_F_REF)
            rcount[m->ref_tgt->idx]++;
        if (m->f & SC_F_DREF) {
            rcount[m->dref_tgt->idx]++;
            dyn_sc_dyn_targets(sch, m, rcount);
        }
    }
    {
        int acc = 0;
        for (i = 0; i < n; i++) {
            rstart[i] = acc;
            acc += rcount[i];
        }
        rstart[n] = acc;
    }
    for (i = 0; i < n; i++) {
        dyn_scnode_t* m = sch->nodes[i];
        int j;
#define UREACH_EDGE(child) redge[rstart[(child)->idx] + fill[(child)->idx]++] = i
        for (j = 0; j < m->all.n; j++)
            UREACH_EDGE(m->all.v[j]);
        for (j = 0; j < m->depsch.n; j++)
            UREACH_EDGE(m->depsch.v[j]);
        if (m->then)
            UREACH_EDGE(m->then);
        if (m->els)
            UREACH_EDGE(m->els);
        if (m->f & SC_F_REF)
            UREACH_EDGE(m->ref_tgt);
        if (m->f & SC_F_DREF) {
            int r2, a2;
            UREACH_EDGE(m->dref_tgt);
            for (r2 = 0; m->dref_name && r2 < sch->n_res; r2++)
                for (a2 = 0; a2 < sch->res[r2]->na; a2++)
                    if (sch->res[r2]->adyn[a2] && !strcmp(sch->res[r2]->anames[a2], m->dref_name))
                        UREACH_EDGE(sch->res[r2]->anodes[a2]);
        }
#undef UREACH_EDGE
    }

    for (i = 0; i < n; i++) {
        dyn_scnode_t* m = sch->nodes[i];
        if (m->f & (SC_F_UNEVALP | SC_F_UNEVALI)) {
            m->f |= SC_F_UREACH;
            queue[tail++] = i;
        }
    }
    while (head < tail) {
        int p = queue[head++];
        for (i = rstart[p]; i < rstart[p + 1]; i++) {
            dyn_scnode_t* a = sch->nodes[redge[i]];
            if (!(a->f & SC_F_UREACH)) {
                a->f |= SC_F_UREACH;
                queue[tail++] = redge[i];
            }
        }
    }
    ret = 0;
out:
    js_free(ctx, rcount);
    js_free(ctx, rstart);
    js_free(ctx, redge);
    js_free(ctx, fill);
    js_free(ctx, queue);
    return ret;
}

static int dyn_sc_apply_vocabulary(dyn_sccomp_t* c, JSValueConst schema)
{
    static const char* const known[] = { "core", "applicator", "unevaluated", "validation", "meta-data",
        "format-annotation", "format-assertion", "content" };
    JSContext* ctx = c->ctx;
    JSValue meta = JS_UNDEFINED, vocab;
    JSPropertyEnum* props = NULL;
    uint32_t nprops = 0, i;
    char* uri;
    int got, has_validation = 0, has_fmt_assert = 0, rc = -1;

    if (!JS_IsObject(schema) || !JS_IsObject(c->resources))
        return 0;
    got = dyn_sc_str_kw(ctx, schema, "$schema", &uri);
    if (got <= 0)
        return got;
    meta = JS_GetPropertyStr(ctx, c->resources, uri);
    js_free(ctx, uri);
    if (JS_IsException(meta))
        return -1;
    if (!JS_IsObject(meta)) {
        JS_FreeValue(ctx, meta);
        return 0;
    }
    vocab = JS_GetPropertyStr(ctx, meta, "$vocabulary");
    JS_FreeValue(ctx, meta);
    if (JS_IsException(vocab))
        return -1;
    if (!JS_IsObject(vocab)) {
        JS_FreeValue(ctx, vocab);
        return 0;
    }
    if (JS_GetOwnPropertyNames(ctx, &props, &nprops, vocab, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY))
        goto out;
    for (i = 0; i < nprops; i++) {
        const char* name = JS_AtomToCString(ctx, props[i].atom);
        JSValue req;
        size_t k;
        int is_known = 0;
        if (!name)
            goto out;
        for (k = 0; k < countof(known); k++)
            if (!strncmp(name, DYN_SC_VOCAB, strlen(DYN_SC_VOCAB)) && !strcmp(name + strlen(DYN_SC_VOCAB), known[k])) {
                is_known = 1;
                has_validation |= k == 3;
                has_fmt_assert |= k == 6;
            }
        req = JS_GetProperty(ctx, vocab, props[i].atom);
        if (!is_known && JS_ToBool(ctx, req) > 0) {
            JS_ThrowTypeError(ctx, "dyna:schema: the meta-schema requires vocabulary \"%s\", which is not supported", name);
            JS_FreeValue(ctx, req);
            JS_FreeCString(ctx, name);
            goto out;
        }
        JS_FreeValue(ctx, req);
        JS_FreeCString(ctx, name);
    }
    c->no_validation = !has_validation;
    if (has_fmt_assert)
        c->sch->fmt_mode = SC_FMT_ASSERTION;
    rc = 0;
out:
    if (props)
        JS_FreePropertyEnum(ctx, props, nprops);
    JS_FreeValue(ctx, vocab);
    return rc;
}

JSValue dyn_sc_compile_entry(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSValue schema;
    dyn_sccomp_t c;
    dyn_schema_t* sch;
    dyn_scnode_t* root = NULL;
    JSValue obj;
    JSRuntime* rt;
    int i;

    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx,
            "Schema.compile(schema) requires a schema");
    if (JS_IsString(argv[0])) {
        const char* s;
        size_t n;
        s = JS_ToCStringLen(ctx, &n, argv[0]);
        if (!s)
            return JS_EXCEPTION;
        schema = JS_ParseJSON(ctx, s, n, "<dyna:schema>");
        JS_FreeCString(ctx, s);
        if (JS_IsException(schema))
            return schema;
    } else {
        schema = JS_DupValue(ctx, argv[0]);
    }
    if (!JS_IsBool(schema) && (!JS_IsObject(schema) || dyn_sc_is_array(ctx, schema))) {
        JSValue e = JS_ThrowTypeError(ctx,
            "Schema.compile: schema must be an object, boolean or JSON string");
        JS_FreeValue(ctx, schema);
        return e;
    }

    sch = js_mallocz(ctx, sizeof(*sch));
    if (!sch) {
        JS_FreeValue(ctx, schema);
        return JS_ThrowOutOfMemory(ctx);
    }
    memset(&c, 0, sizeof c);
    c.ctx = ctx;
    c.sch = sch;
    c.base_uri = "";
    c.resources = JS_UNDEFINED;
    c.docv[0] = JS_DupValue(ctx, schema);
    c.docp[0][0] = 0;
    c.ndocv = 1;

    if (dyn_sc_read_options(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, &c, sch) || dyn_sc_apply_vocabulary(&c, schema)
        || dyn_sc_compile(&c, schema, "", 0, &root) || dyn_sc_resolve_refs(&c) || dyn_sc_compute_ureach(ctx, sch)) {
        for (i = 0; i < c.n_paths; i++)
            js_free(ctx, c.paths[i]);
        js_free(ctx, c.paths);
        js_free(ctx, c.pnode);
        JS_FreeValue(ctx, schema);
        JS_FreeValue(ctx, c.resources);
        js_free(ctx, c.base_owned);
        for (i = 0; i < c.ndocv; i++)
            JS_FreeValue(ctx, c.docv[i]);
        dyn_sc_schema_free_rt(JS_GetRuntime(ctx), sch);
        return JS_EXCEPTION;
    }
    for (i = 0; i < c.n_paths; i++)
        js_free(ctx, c.paths[i]);
    js_free(ctx, c.paths);
    js_free(ctx, c.pnode);
    JS_FreeValue(ctx, schema);
    JS_FreeValue(ctx, c.resources);
    js_free(ctx, c.base_owned);
    for (i = 0; i < c.ndocv; i++)
        JS_FreeValue(ctx, c.docv[i]);

    sch->root = root;
    rt = JS_GetRuntime(ctx);
    obj = JS_NewObjectClass(ctx, dyn_schema_class_id);
    if (JS_IsException(obj)) {
        dyn_sc_schema_free_rt(rt, sch);
        return obj;
    }
    JS_SetOpaque(obj, sch);
    return obj;
}

#endif
