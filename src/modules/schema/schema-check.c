#include "schema-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_SCHEMA)

#define DYN_SC_MAX_ARRAY (1 << 22)
#define DYN_SC_MAX_UNIQUE 4096
#define DYN_SC_MULT_EPS 1e-9

static inline int dyn_sc_utf8_cplen(const char* s, size_t n)
{
    size_t i, count = 0;
    for (i = 0; i < n; i++)
        if (((unsigned char)s[i] & 0xC0) != 0x80)
            count++;
    return (int)count;
}

int dyn_sc_check_list(JSContext* ctx, JSValueConst v, const char* kw,
    uint64_t* len)
{
    if (dyn_sc_is_array(ctx, v) != 1) {
        JS_ThrowTypeError(ctx, "dyna:schema: \"%s\" must be an array", kw);
        return -1;
    }
    *len = dyn_sc_arr_len(ctx, v);
    if (*len == (uint64_t)-1)
        return -1;
    if (*len > DYN_SC_MAX_LIST) {
        JS_ThrowTypeError(ctx,
            "dyna:schema: \"%s\" has too many elements", kw);
        return -1;
    }
    return 0;
}

int dyn_sc_check_pure_ref_cycles(dyn_sccomp_t* c)
{
    int i;
    for (i = 0; i < c->sch->n_nodes; i++) {
        dyn_scnode_t* n = c->sch->nodes[i];
        dyn_scnode_t* chain[DYN_SC_MAX_REFCHAIN];
        dyn_scnode_t* cur;
        int nchain = 0;
        if ((n->f & SC_F_REF) == 0)
            continue;
        cur = n;
        while (cur && (cur->f & SC_F_REF) && cur->ref_tgt && (cur->ref_tgt->f & SC_F_REF)) {
            int k;
            if (nchain >= DYN_SC_MAX_REFCHAIN)
                break;
            for (k = 0; k < nchain; k++)
                if (chain[k] == cur) {
                    JS_ThrowTypeError(c->ctx,
                        "dyna:schema: circular $ref chain through \"%s\"",
                        n->ref ? n->ref : "");
                    return -1;
                }
            chain[nchain++] = cur;
            cur = cur->ref_tgt;
        }
    }
    return 0;
}

static int dyn_sc_has_own(JSContext* ctx, JSValueConst obj, const char* s,
    size_t n)
{
    JSAtom a = JS_NewAtomLen(ctx, s, n);
    JSValue v;
    int r;
    if (a == JS_ATOM_NULL)
        return -1;
    r = dyn_sc_get_own(ctx, obj, a, &v);
    JS_FreeAtom(ctx, a);
    if (r == 1)
        JS_FreeValue(ctx, v);
    return r;
}

#define DYN_SC_RE_STEPS 100000000ull

static int dyn_sc_re_match(JSContext* ctx, const uint8_t* bc, const char* s,
    size_t n)
{
    int cc;
    uint8_t** cap = NULL;
    uint16_t* u = NULL;
    int ulen, rc;
    if (n > 2147483647u)
        return -1;
    ulen = dyn_sc_utf8_to_utf16(ctx, s, n, &u);
    if (ulen < 0)
        return -1;
    cc = lre_get_alloc_count(bc);
    if (cc > 0) {
        cap = js_malloc(ctx, (size_t)cc * sizeof(uint8_t*));
        if (!cap) {
            js_free(ctx, u);
            return -1;
        }
    }
    rc = lre_exec_budget(cap, bc, (const uint8_t*)u, 0, ulen, 1, ctx,
        DYN_SC_RE_STEPS);
    js_free(ctx, cap);
    js_free(ctx, u);
    if (rc < 0) {
        if (rc == LRE_RET_TIMEOUT)
            JS_ThrowRangeError(ctx, "dyna:schema: pattern execution timed out");
        else
            JS_ThrowInternalError(ctx,
                "dyna:schema: pattern execution failed");
        return -1;
    }
    return rc == 1;
}

static void __attribute__((format(printf, 3, 4))) dyn_sc_add_err(dyn_vctx_t* v, const char* keyword,
    const char* fmt, ...)
{
    dyn_scerr_t* e;
    char msg[192];
    va_list ap;

    if (v->errs->done)
        return;
    if (v->errs->n >= DYN_SC_MAX_ERRORS) {
        v->errs->done = 1;
        return;
    }
    if (v->errs->n >= v->errs->cap) {
        int ncap = v->errs->cap ? v->errs->cap * 2 : 16;
        dyn_scerr_t* ne = js_realloc(v->ctx, v->errs->e,
            (size_t)ncap * sizeof(*ne));
        if (!ne) {
            v->errs->drops++;
            return;
        }
        v->errs->e = ne;
        v->errs->cap = ncap;
    }
    e = &v->errs->e[v->errs->n];
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    size_t mlen = strlen(msg), klen = strlen(keyword), plen = 0, si, ci, o;
    for (si = 0; si < v->path.len; si++) {
        plen += 1 + v->path.seg[si].n;
        for (ci = 0; ci < v->path.seg[si].n; ci++)
            plen += v->path.seg[si].p[ci] == '~' || v->path.seg[si].p[ci] == '/';
    }
    e->pathlen = (int)plen;
    e->path = js_malloc(v->ctx, plen + 1);
    if (!e->path) {
        v->errs->drops++;
        return;
    }
    for (si = 0, o = 0; si < v->path.len; si++) {
        const char* tok = v->path.seg[si].p;
        e->path[o++] = '/';
        for (ci = 0; ci < v->path.seg[si].n; ci++) {
            if (tok[ci] == '~') {
                e->path[o++] = '~';
                e->path[o++] = '0';
            } else if (tok[ci] == '/') {
                e->path[o++] = '~';
                e->path[o++] = '1';
            } else
                e->path[o++] = tok[ci];
        }
    }
    e->path[plen] = '\0';
    e->message = js_malloc(v->ctx, mlen + 1);
    e->keyword = js_malloc(v->ctx, klen + 1);
    if (!e->message || !e->keyword) {
        js_free(v->ctx, e->path);
        js_free(v->ctx, e->message);
        js_free(v->ctx, e->keyword);
        memset(e, 0, sizeof(*e));
        v->errs->drops++;
        return;
    }
    memcpy(e->message, msg, mlen + 1);
    memcpy(e->keyword, keyword, klen + 1);
    v->errs->n++;
}

static int dyn_sc_path_push(dyn_vctx_t* v, const char* tok, size_t n)
{
    if (v->path.len == v->path.cap) {
        size_t ncap = v->path.cap ? v->path.cap * 2 : 16;
        dyn_scseg_t* ns = js_realloc(v->ctx, v->path.seg, ncap * sizeof(*ns));
        if (!ns)
            return -1;
        v->path.seg = ns;
        v->path.cap = ncap;
    }
    v->path.seg[v->path.len].p = tok;
    v->path.seg[v->path.len].n = n;
    v->path.len++;
    return 0;
}

static void dyn_sc_path_restore(dyn_vctx_t* v, size_t mark)
{
    v->path.len = mark;
}

static int dyn_sc_check(dyn_vctx_t* v, dyn_scnode_t* n, JSValueConst inst);

int dyn_sc_check_top(dyn_vctx_t* v, dyn_scnode_t* n, JSValueConst inst)
{
    int r;
    if (++v->depth > DYN_SC_MAX_DEPTH) {
        v->depth--;
        JS_ThrowRangeError(v->ctx,
            "dyna:schema: instance nesting exceeds %d levels",
            DYN_SC_MAX_DEPTH);
        return -1;
    }
    if (n->res && v->dstack && (v->dn == 0 || v->dstack[v->dn - 1] != n->res)) {
        v->dstack[v->dn++] = n->res;
        r = dyn_sc_check(v, n, inst);
        v->dn--;
    } else {
        r = dyn_sc_check(v, n, inst);
    }
    v->depth--;
    return r;
}

static dyn_scnode_t* dyn_sc_dyn_target(dyn_vctx_t* v, dyn_scnode_t* n)
{
    int i;
    if (!n->dref_name)
        return n->dref_tgt;
    for (i = 0; i < v->dn; i++) {
        dyn_scnode_t* t = dyn_sc_res_anchor(v->dstack[i], n->dref_name, 1);
        if (t)
            return t;
    }
    return n->dref_tgt;
}

static int dyn_sc_ref_enter(dyn_vctx_t* v, dyn_scnode_t* tgt)
{
    int i;
    for (i = 0; i < v->refn; i++)
        if (v->refstk[i] == tgt)
            return 1;
    if (v->refn >= DYN_SC_MAX_REFCHAIN)
        return 2;
    v->refstk[v->refn++] = tgt;
    return 0;
}

static int dyn_sc_check_child(dyn_vctx_t* v, dyn_scnode_t* sub,
    JSValueConst inst, const char* tok, size_t n)
{
    size_t mark = v->path.len;
    int saved_refn = v->refn;
    dyn_sceval_t* saved_eval = v->cur_eval;
    int r;
    if (dyn_sc_path_push(v, tok, n) < 0)
        return -1;
    v->refn = 0;
    v->cur_eval = NULL;
    r = dyn_sc_check_top(v, sub, inst);
    dyn_sc_path_restore(v, mark);
    v->refn = saved_refn;
    v->cur_eval = saved_eval;
    return r;
}

static int dyn_sc_check_impl(dyn_vctx_t* v, dyn_scnode_t* n,
    JSValueConst inst);

static int dyn_sc_check_impl(dyn_vctx_t* v, dyn_scnode_t* n, JSValueConst inst)
{
    JSContext* ctx = v->ctx;
    int it;
    int r = 1;
    int i;

    if (n->kind == SC_BOOL) {
        if (!n->boolv) {
            dyn_sc_add_err(v, "false", "false schema rejects every value");
            r = 0;
        }
        return r;
    }

    it = dyn_sc_inst_type(ctx, inst);

    if ((n->f & SC_F_TYPE) && !dyn_sc_type_ok(ctx, n->type_mask, inst)) {
        char buf[80];
        dyn_sc_type_str(n->type_mask, buf, sizeof buf);
        dyn_sc_add_err(v, "type", "must be %s", buf);
        r = 0;
    }

    if (n->f & SC_F_CONST) {
        int eq = dyn_sc_equal(ctx, inst, n->val, 0, v->eqseen, 0);
        if (eq < 0)
            return -1;
        if (!eq) {
            dyn_sc_add_err(v, "const", "must be the constant value");
            r = 0;
        }
    }

    if (n->f & SC_F_ENUM) {
        int found = 0;
        for (i = 0; i < n->n_enum && !found; i++) {
            int eq = dyn_sc_equal(ctx, inst, n->enumv[i], 0, v->eqseen, 0);
            if (eq < 0)
                return -1;
            found = eq;
        }
        if (!found) {
            dyn_sc_add_err(v, "enum", "must be one of the allowed values");
            r = 0;
        }
    }

    if (it == DYN_SC_T_NUMBER) {
        double d = dyn_num_value(inst);
        if (isfinite(d)) {
            if ((n->f & SC_F_MIN) && d < n->dmin) {
                dyn_sc_add_err(v, "minimum", "must be >= %g", n->dmin);
                r = 0;
            }
            if ((n->f & SC_F_EMIN) && d <= n->demin) {
                dyn_sc_add_err(v, "exclusiveMinimum", "must be > %g",
                    n->demin);
                r = 0;
            }
            if ((n->f & SC_F_MAX) && d > n->dmax) {
                dyn_sc_add_err(v, "maximum", "must be <= %g", n->dmax);
                r = 0;
            }
            if ((n->f & SC_F_EMAX) && d >= n->demax) {
                dyn_sc_add_err(v, "exclusiveMaximum", "must be < %g",
                    n->demax);
                r = 0;
            }
            if (n->f & SC_F_MULT) {
                double q = d / n->dmult;
                double frac = q - floor(q);
                if (q != q || q - q != 0) {
                    double im = floor(n->dmult), inv = 1.0 / n->dmult;
                    if (d == floor(d) && inv == floor(inv) && inv - inv == 0) {
                    } else if (im != n->dmult || im <= 0 || fmod(d, im) != 0) {
                        dyn_sc_add_err(v, "multipleOf",
                            "must be a multiple of %g", n->dmult);
                        r = 0;
                    }
                } else if (frac > DYN_SC_MULT_EPS && frac < 1 - DYN_SC_MULT_EPS) {
                    dyn_sc_add_err(v, "multipleOf", "must be a multiple of %g",
                        n->dmult);
                    r = 0;
                }
            }
        } else if (n->f & (SC_F_MIN | SC_F_MAX | SC_F_EMIN | SC_F_EMAX | SC_F_MULT)) {
            dyn_sc_add_err(v, "number", "must be a finite number");
            r = 0;
        }
    }

    if (it == DYN_SC_T_STRING && (n->f & (SC_F_MINLEN | SC_F_MAXLEN | SC_F_PAT | SC_F_FMT))) {
        const char* s;
        size_t sn;
        int len = 0;
        s = JS_ToCStringLen(ctx, &sn, inst);
        if (!s)
            return -1;
        if (n->f & (SC_F_MINLEN | SC_F_MAXLEN))
            len = dyn_sc_utf8_cplen(s, sn);
        if ((n->f & SC_F_MINLEN) && len < n->minlen) {
            dyn_sc_add_err(v, "minLength", "must be at least %d characters long",
                n->minlen);
            r = 0;
        }
        if ((n->f & SC_F_MAXLEN) && len > n->maxlen) {
            dyn_sc_add_err(v, "maxLength", "must be at most %d characters long",
                n->maxlen);
            r = 0;
        }
        if (n->f & SC_F_PAT) {
            int m = dyn_sc_re_match(ctx, n->re, s, sn);
            if (m < 0) {
                JS_FreeCString(ctx, s);
                return -1;
            }
            if (!m) {
                dyn_sc_add_err(v, "pattern", "must match the pattern");
                r = 0;
            }
        }
        if ((n->f & SC_F_FMT) && v->sch->fmt_mode != SC_FMT_ANNOTATION) {
            JSValue fn = dyn_sc_fmt_lookup(ctx, n->fmt);
            if (JS_IsException(fn)) {
                JS_FreeCString(ctx, s);
                return -1;
            }
            if (JS_IsUndefined(fn) && v->sch->fmt_mode == SC_FMT_ASSERTION) {
                int ok = sn > DYN_SC_MAX_PATTERN && !strcmp(n->fmt, "regex") ? 0 : dyn_fm_builtin(ctx, n->fmt, s, sn);
                if (ok < 0) {
                    JS_FreeCString(ctx, s);
                    return -1;
                }
                if (!ok) {
                    dyn_sc_add_err(v, "format", "must be a valid \"%s\"", n->fmt);
                    r = 0;
                }
            }
            if (!JS_IsUndefined(fn)) {
                JSValue res;
                res = JS_Call(ctx, fn, JS_UNDEFINED, 1, &inst);
                JS_FreeValue(ctx, fn);
                if (JS_IsException(res)) {
                    JS_FreeCString(ctx, s);
                    return -1;
                }
                if (!JS_ToBool(ctx, res)) {
                    dyn_sc_add_err(v, "format",
                        "must be a valid \"%s\"", n->fmt);
                    r = 0;
                }
                JS_FreeValue(ctx, res);
            }
        }
        JS_FreeCString(ctx, s);
    }

    if (it == DYN_SC_T_ARRAY) {
        uint64_t len = dyn_sc_arr_len(ctx, inst);
        uint64_t i2;
        if (len == (uint64_t)-1)
            return -1;
        if ((n->f & SC_F_MINITEMS) && len < (uint64_t)n->minitems) {
            dyn_sc_add_err(v, "minItems", "must have at least %d items",
                n->minitems);
            r = 0;
        }
        if ((n->f & SC_F_MAXITEMS) && len > (uint64_t)n->maxitems) {
            dyn_sc_add_err(v, "maxItems", "must have at most %d items",
                n->maxitems);
            r = 0;
        }
        if ((n->f & SC_F_UNIQUE) && n->unique) {
            int du = 0;
            uint64_t ii, jj;
            if (len > DYN_SC_MAX_UNIQUE) {
                JS_ThrowRangeError(ctx,
                    "dyna:schema: uniqueItems is bounded to %d elements",
                    DYN_SC_MAX_UNIQUE);
                return -1;
            }
            for (ii = 0; ii < len && !du; ii++)
                for (jj = ii + 1; jj < len && !du; jj++) {
                    JSValue a = JS_GetPropertyUint32(ctx, inst, (uint32_t)ii);
                    JSValue b = JS_GetPropertyUint32(ctx, inst, (uint32_t)jj);
                    int eq;
                    if (JS_IsException(a) || JS_IsException(b)) {
                        JS_FreeValue(ctx, a);
                        JS_FreeValue(ctx, b);
                        return -1;
                    }
                    eq = dyn_sc_equal(ctx, a, b, 0, v->eqseen, 0);
                    JS_FreeValue(ctx, a);
                    JS_FreeValue(ctx, b);
                    if (eq < 0)
                        return -1;
                    du = eq;
                }
            if (du) {
                dyn_sc_add_err(v, "uniqueItems", "must have unique items");
                r = 0;
            }
        }
        if (len > DYN_SC_MAX_ARRAY) {
            JS_ThrowRangeError(ctx,
                "dyna:schema: arrays longer than %d elements are not validated",
                DYN_SC_MAX_ARRAY);
            return -1;
        }
        for (i2 = 0; i2 < len; i2++) {
            JSValue el;
            int rr;
            dyn_scnode_t* sub = NULL;
            char tok[24];
            int tlen;
            if (i2 < (uint64_t)n->prefix.n)
                sub = n->prefix.v[i2];
            else if (n->f & SC_F_ITEMS)
                sub = n->items;
            if (!sub)
                continue;
            el = JS_GetPropertyUint32(ctx, inst, (uint32_t)i2);
            if (JS_IsException(el))
                return -1;
            tlen = snprintf(tok, sizeof tok, "%llu", (unsigned long long)i2);
            rr = dyn_sc_check_child(v, sub, el, tok, (size_t)tlen);
            JS_FreeValue(ctx, el);
            if (rr < 0)
                return -1;
            if (!rr)
                r = 0;
            else if (rr == 1 && dyn_sc_eval_add_item(ctx, v->cur_eval, i2))
                return -1;
        }
        if (n->f & SC_F_CONT) {
            dyn_scerrs_t* saved = v->errs;
            dyn_scerrs_t scratch;
            dyn_sceval_t* saved_eval = v->cur_eval;
            uint64_t matches = 0;
            int eff_min = (n->f & SC_F_MINCONT) ? n->mincont : 1;
            int probe_all = (n->f & SC_F_MAXCONT) || v->cur_eval != NULL;
            memset(&scratch, 0, sizeof scratch);
            for (i2 = 0; i2 < len; i2++) {
                JSValue el;
                int rr;
                el = JS_GetPropertyUint32(ctx, inst, (uint32_t)i2);
                if (JS_IsException(el)) {
                    dyn_sc_errs_free(ctx, &scratch);
                    return -1;
                }
                dyn_sc_errs_reset(ctx, &scratch);
                v->errs = &scratch;
                v->cur_eval = NULL;
                rr = dyn_sc_check_top(v, n->cont, el);
                v->cur_eval = saved_eval;
                v->errs = saved;
                JS_FreeValue(ctx, el);
                if (rr < 0) {
                    dyn_sc_errs_free(ctx, &scratch);
                    return -1;
                }
                if (rr) {
                    matches++;
                    if (dyn_sc_eval_add_item(ctx, v->cur_eval, i2)) {
                        dyn_sc_errs_free(ctx, &scratch);
                        return -1;
                    }
                    if (!probe_all && matches >= (uint64_t)(eff_min > 0 ? eff_min : 0))
                        break;
                }
            }
            dyn_sc_errs_free(ctx, &scratch);
            if (matches < (uint64_t)eff_min) {
                if (n->f & SC_F_MINCONT) {
                    dyn_sc_add_err(v, "minContains",
                        "must contain at least %d item matching "
                        "the \"contains\" schema",
                        n->mincont);
                } else {
                    dyn_sc_add_err(v, "contains",
                        "must contain at least 1 item matching "
                        "the schema");
                }
                r = 0;
            }
            if ((n->f & SC_F_MAXCONT) && matches > (uint64_t)n->maxcont) {
                dyn_sc_add_err(v, "maxContains",
                    "must contain at most %d items matching the "
                    "\"contains\" schema",
                    n->maxcont);
                r = 0;
            }
        }
    }

    if (it == DYN_SC_T_OBJECT && (n->f & (SC_F_MINPROPS | SC_F_MAXPROPS | SC_F_REQ | SC_F_DEPREQ | SC_F_DEPSCH | SC_F_PNAMES | SC_F_PROPS | SC_F_PATTS | SC_F_ADDL))) {
        JSPropertyEnum* tab = NULL;
        uint32_t nprops, k;

        if (JS_GetOwnPropertyNames(ctx, &tab, &nprops, inst,
                JS_GPN_ENUM_ONLY | JS_GPN_STRING_MASK)
            < 0)
            return -1;

        if ((n->f & SC_F_MINPROPS) && nprops < (uint32_t)n->minprops) {
            dyn_sc_add_err(v, "minProperties",
                "must have at least %d properties", n->minprops);
            r = 0;
        }
        if ((n->f & SC_F_MAXPROPS) && nprops > (uint32_t)n->maxprops) {
            dyn_sc_add_err(v, "maxProperties",
                "must have at most %d properties", n->maxprops);
            r = 0;
        }
        if (n->f & SC_F_REQ) {
            for (i = 0; i < n->required.n; i++) {
                int found = dyn_sc_has_own(ctx, inst, n->required.names[i],
                    strlen(n->required.names[i]));
                if (found < 0) {
                    JS_FreePropertyEnum(ctx, tab, nprops);
                    return -1;
                }
                if (!found) {
                    dyn_sc_add_err(v, "required",
                        "must have required property '%s'",
                        n->required.names[i]);
                    r = 0;
                }
            }
        }
        if (n->f & SC_F_DEPREQ) {
            for (i = 0; i < n->depreq.n; i++) {
                int present = dyn_sc_has_own(ctx, inst, n->depreq.props[i],
                    strlen(n->depreq.props[i]));
                int j;
                if (present < 0) {
                    JS_FreePropertyEnum(ctx, tab, nprops);
                    return -1;
                }
                if (!present)
                    continue;
                for (j = 0; j < n->depreq.nneed[i]; j++) {
                    int have = dyn_sc_has_own(ctx, inst,
                        n->depreq.need[i][j],
                        strlen(n->depreq.need[i][j]));
                    if (have < 0) {
                        JS_FreePropertyEnum(ctx, tab, nprops);
                        return -1;
                    }
                    if (!have) {
                        dyn_sc_add_err(v, "dependentRequired",
                            "must have property '%s' when '%s' is "
                            "present",
                            n->depreq.need[i][j], n->depreq.props[i]);
                        r = 0;
                    }
                }
            }
        }
        if (n->f & SC_F_DEPSCH) {
            for (i = 0; i < n->depsch.n; i++) {
                int present = dyn_sc_has_own(ctx, inst, n->depsch.names[i],
                    strlen(n->depsch.names[i]));
                int rr;
                if (present < 0) {
                    JS_FreePropertyEnum(ctx, tab, nprops);
                    return -1;
                }
                if (!present)
                    continue;
                rr = dyn_sc_check_top(v, n->depsch.v[i], inst);
                if (rr < 0) {
                    JS_FreePropertyEnum(ctx, tab, nprops);
                    return -1;
                }
                if (!rr)
                    r = 0;
            }
        }
        if (n->f & (SC_F_PROPS | SC_F_PATTS | SC_F_ADDL | SC_F_PNAMES)) {
            for (k = 0; k < nprops; k++) {
                JSValue name = JS_AtomToString(ctx, tab[k].atom);
                const char* nb;
                size_t nl;
                JSValue val;
                int covered = 0;
                int get;
                if (JS_IsException(name)) {
                    JS_FreePropertyEnum(ctx, tab, nprops);
                    return -1;
                }
                nb = JS_ToCStringLen(ctx, &nl, name);
                if (!nb) {
                    JS_FreeValue(ctx, name);
                    JS_FreePropertyEnum(ctx, tab, nprops);
                    return -1;
                }
                get = dyn_sc_get_own(ctx, inst, tab[k].atom, &val);
                if (get < 0) {
                    JS_FreeCString(ctx, nb);
                    JS_FreeValue(ctx, name);
                    JS_FreePropertyEnum(ctx, tab, nprops);
                    return -1;
                }
                if (get == 0) {
                    JS_FreeCString(ctx, nb);
                    JS_FreeValue(ctx, name);
                    continue;
                }
                if (n->f & SC_F_PNAMES) {
                    int rr = dyn_sc_check_child(v, n->pn, name, nb, nl);
                    if (rr < 0) {
                        JS_FreeValue(ctx, val);
                        JS_FreeCString(ctx, nb);
                        JS_FreeValue(ctx, name);
                        JS_FreePropertyEnum(ctx, tab, nprops);
                        return -1;
                    }
                    if (!rr) {
                        dyn_sc_add_err(v, "propertyNames",
                            "property name '%s' does not match "
                            "the schema",
                            nb);
                        r = 0;
                    }
                }
                if (n->f & SC_F_PROPS) {
                    for (i = 0; i < n->props.n; i++)
                        if (strcmp(n->props.names[i], nb) == 0) {
                            int rr = dyn_sc_check_child(v, n->props.v[i], val,
                                nb, nl);
                            if (rr < 0) {
                                JS_FreeValue(ctx, val);
                                JS_FreeCString(ctx, nb);
                                JS_FreeValue(ctx, name);
                                JS_FreePropertyEnum(ctx, tab, nprops);
                                return -1;
                            }
                            if (!rr)
                                r = 0;
                            if (rr == 1 && dyn_sc_eval_add_prop(ctx, v->cur_eval, nb, nl)) {
                                JS_FreeValue(ctx, val);
                                JS_FreeCString(ctx, nb);
                                JS_FreeValue(ctx, name);
                                JS_FreePropertyEnum(ctx, tab, nprops);
                                return -1;
                            }
                            covered = 1;
                            break;
                        }
                }
                if (n->f & SC_F_PATTS) {
                    for (i = 0; i < n->n_patts; i++) {
                        int m = dyn_sc_re_match(ctx, n->patts[i].re, nb, nl);
                        if (m < 0) {
                            JS_FreeValue(ctx, val);
                            JS_FreeCString(ctx, nb);
                            JS_FreeValue(ctx, name);
                            JS_FreePropertyEnum(ctx, tab, nprops);
                            return -1;
                        }
                        if (m) {
                            int rr = dyn_sc_check_child(v, n->patts[i].node,
                                val, nb, nl);
                            if (rr < 0) {
                                JS_FreeValue(ctx, val);
                                JS_FreeCString(ctx, nb);
                                JS_FreeValue(ctx, name);
                                JS_FreePropertyEnum(ctx, tab, nprops);
                                return -1;
                            }
                            if (!rr)
                                r = 0;
                            if (rr == 1 && dyn_sc_eval_add_prop(ctx, v->cur_eval, nb, nl)) {
                                JS_FreeValue(ctx, val);
                                JS_FreeCString(ctx, nb);
                                JS_FreeValue(ctx, name);
                                JS_FreePropertyEnum(ctx, tab, nprops);
                                return -1;
                            }
                            covered = 1;
                        }
                    }
                }
                if (!covered && (n->f & SC_F_ADDL)) {
                    if (n->addl) {
                        int rr = dyn_sc_check_child(v, n->addl, val, nb, nl);
                        if (rr < 0) {
                            JS_FreeValue(ctx, val);
                            JS_FreeCString(ctx, nb);
                            JS_FreeValue(ctx, name);
                            JS_FreePropertyEnum(ctx, tab, nprops);
                            return -1;
                        }
                        if (!rr)
                            r = 0;
                        if (rr == 1 && dyn_sc_eval_add_prop(ctx, v->cur_eval, nb, nl)) {
                            JS_FreeValue(ctx, val);
                            JS_FreeCString(ctx, nb);
                            JS_FreeValue(ctx, name);
                            JS_FreePropertyEnum(ctx, tab, nprops);
                            return -1;
                        }
                    } else if (!n->addl_allow) {
                        dyn_sc_add_err(v, "additionalProperties",
                            "must have no additional properties");
                        r = 0;
                    } else if (dyn_sc_eval_add_prop(ctx, v->cur_eval, nb,
                                   nl)) {
                        JS_FreeValue(ctx, val);
                        JS_FreeCString(ctx, nb);
                        JS_FreeValue(ctx, name);
                        JS_FreePropertyEnum(ctx, tab, nprops);
                        return -1;
                    }
                }
                JS_FreeValue(ctx, val);
                JS_FreeCString(ctx, nb);
                JS_FreeValue(ctx, name);
            }
        }
        JS_FreePropertyEnum(ctx, tab, nprops);
    }

    if (n->f & SC_F_ALL) {
        for (i = 0; i < n->all.n; i++) {
            int rr = dyn_sc_check_top(v, n->all.v[i], inst);
            if (rr < 0)
                return -1;
            if (!rr)
                r = 0;
        }
    }

    if (n->f & SC_F_ANY) {
        dyn_scerrs_t* saved = v->errs;
        dyn_scerrs_t scratch, bests;
        int passed = 0, best = -1;
        memset(&scratch, 0, sizeof scratch);
        memset(&bests, 0, sizeof bests);
        for (i = 0; i < n->any.n && (v->cur_eval != NULL || !passed); i++) {
            int rr;
            dyn_sc_errs_reset(ctx, &scratch);
            v->errs = &scratch;
            rr = dyn_sc_check_top(v, n->any.v[i], inst);
            v->errs = saved;
            if (rr < 0) {
                dyn_sc_errs_free(ctx, &scratch);
                dyn_sc_errs_free(ctx, &bests);
                return -1;
            }
            if (rr) {
                passed = 1;
            } else if (best < 0 || scratch.n < bests.n) {
                dyn_scerrs_t t = scratch;
                scratch = bests;
                bests = t;
                best = i;
            }
        }
        dyn_sc_errs_free(ctx, &scratch);
        if (!passed) {
            if (dyn_sc_errs_move(ctx, saved, &bests) < 0) {
                dyn_sc_errs_free(ctx, &bests);
                return -1;
            }
            r = 0;
        }
        dyn_sc_errs_free(ctx, &bests);
    }

    if (n->f & SC_F_ONE) {
        dyn_scerrs_t* saved = v->errs;
        dyn_scerrs_t scratch, bests;
        int passed = 0, best = -1;
        memset(&scratch, 0, sizeof scratch);
        memset(&bests, 0, sizeof bests);
        for (i = 0; i < n->one.n; i++) {
            int rr;
            dyn_sc_errs_reset(ctx, &scratch);
            v->errs = &scratch;
            rr = dyn_sc_check_top(v, n->one.v[i], inst);
            v->errs = saved;
            if (rr < 0) {
                dyn_sc_errs_free(ctx, &scratch);
                dyn_sc_errs_free(ctx, &bests);
                return -1;
            }
            if (rr)
                passed++;
            else if (best < 0 || scratch.n < bests.n) {
                dyn_scerrs_t t = scratch;
                scratch = bests;
                bests = t;
                best = i;
            }
        }
        dyn_sc_errs_free(ctx, &scratch);
        if (passed != 1) {
            if (passed == 0) {
                if (best >= 0) {
                    if (dyn_sc_errs_move(ctx, saved, &bests) < 0) {
                        dyn_sc_errs_free(ctx, &bests);
                        return -1;
                    }
                }
            } else {
                dyn_sc_add_err(v, "oneOf",
                    "must match exactly one schema (matched %d)",
                    passed);
            }
            r = 0;
        }
        dyn_sc_errs_free(ctx, &bests);
    }

    if (n->f & SC_F_NOT) {
        dyn_scerrs_t* saved = v->errs;
        dyn_scerrs_t scratch;
        int rr;
        memset(&scratch, 0, sizeof scratch);
        v->errs = &scratch;
        rr = dyn_sc_check_top(v, n->notn, inst);
        v->errs = saved;
        dyn_sc_errs_free(ctx, &scratch);
        if (rr < 0)
            return -1;
        if (rr) {
            dyn_sc_add_err(v, "not", "must not match the schema");
            r = 0;
        }
    }

    if (n->f & SC_F_IF) {
        dyn_scerrs_t* saved = v->errs;
        dyn_scerrs_t scratch;
        int cond;
        memset(&scratch, 0, sizeof scratch);
        v->errs = &scratch;
        cond = dyn_sc_check_top(v, n->ifn, inst);
        v->errs = saved;
        dyn_sc_errs_free(ctx, &scratch);
        if (cond < 0)
            return -1;
        if (cond && n->then) {
            int rr = dyn_sc_check_top(v, n->then, inst);
            if (rr < 0)
                return -1;
            if (!rr)
                r = 0;
        } else if (!cond && n->els) {
            int rr = dyn_sc_check_top(v, n->els, inst);
            if (rr < 0)
                return -1;
            if (!rr)
                r = 0;
        }
    }

    if (n->f & SC_F_REF) {
        int ce = dyn_sc_ref_enter(v, n->ref_tgt);
        if (ce < 0)
            return -1;
        if (ce == 1) {
            dyn_sc_add_err(v, "$ref", "circular $ref chain at \"%s\"", n->ref);
            r = 0;
        } else if (ce == 2) {
            dyn_sc_add_err(v, "$ref",
                "$ref indirection exceeds %d levels at \"%s\"",
                DYN_SC_MAX_REFCHAIN, n->ref);
            r = 0;
        } else {
            int rr = dyn_sc_check_top(v, n->ref_tgt, inst);
            v->refn--;
            if (rr < 0)
                return -1;
            if (!rr)
                r = 0;
        }
    }

    if (n->f & SC_F_DREF) {
        dyn_scnode_t* tgt = dyn_sc_dyn_target(v, n);
        int ce = dyn_sc_ref_enter(v, tgt);
        if (ce == 1) {
            dyn_sc_add_err(v, "$dynamicRef", "circular $dynamicRef chain at \"%s\"", n->dref);
            r = 0;
        } else if (ce == 2) {
            dyn_sc_add_err(v, "$dynamicRef", "$dynamicRef indirection exceeds %d levels at \"%s\"",
                DYN_SC_MAX_REFCHAIN, n->dref);
            r = 0;
        } else {
            int rr = dyn_sc_check_top(v, tgt, inst);
            v->refn--;
            if (rr < 0)
                return -1;
            if (!rr)
                r = 0;
        }
    }

    if (it == DYN_SC_T_OBJECT && (n->f & SC_F_UNEVALP)) {
        dyn_sceval_t* rec = v->cur_eval;
        JSPropertyEnum* tab2 = NULL;
        uint32_t nprops2, k2;
        int failed = 0;

        if (JS_GetOwnPropertyNames(ctx, &tab2, &nprops2, inst,
                JS_GPN_ENUM_ONLY | JS_GPN_STRING_MASK)
            < 0)
            return -1;
        for (k2 = 0; k2 < nprops2; k2++) {
            JSValue name = JS_AtomToString(ctx, tab2[k2].atom);
            const char* nb;
            size_t nl;
            if (JS_IsException(name)) {
                JS_FreePropertyEnum(ctx, tab2, nprops2);
                return -1;
            }
            nb = JS_ToCStringLen(ctx, &nl, name);
            if (!nb) {
                JS_FreeValue(ctx, name);
                JS_FreePropertyEnum(ctx, tab2, nprops2);
                return -1;
            }
            if (dyn_sc_eval_has_prop(rec, nb, nl))
                goto unevalp_next;
            if (n->unevalp) {
                JSValue val;
                int get = dyn_sc_get_own(ctx, inst, tab2[k2].atom, &val);
                int rr;
                if (get < 0) {
                    JS_FreeCString(ctx, nb);
                    JS_FreeValue(ctx, name);
                    JS_FreePropertyEnum(ctx, tab2, nprops2);
                    return -1;
                }
                if (get == 0)
                    goto unevalp_next;
                rr = dyn_sc_check_child(v, n->unevalp, val, nb, nl);
                JS_FreeValue(ctx, val);
                if (rr < 0) {
                    JS_FreeCString(ctx, nb);
                    JS_FreeValue(ctx, name);
                    JS_FreePropertyEnum(ctx, tab2, nprops2);
                    return -1;
                }
                if (!rr) {
                    failed = 1;
                } else if (dyn_sc_eval_add_prop(ctx, rec, nb, nl)) {
                    JS_FreeCString(ctx, nb);
                    JS_FreeValue(ctx, name);
                    JS_FreePropertyEnum(ctx, tab2, nprops2);
                    return -1;
                }
            } else if (!n->unevalp_allow) {
                dyn_sc_add_err(v, "unevaluatedProperties",
                    "property is unevaluated and the schema "
                    "allows none");
                failed = 1;
            } else if (dyn_sc_eval_add_prop(ctx, rec, nb, nl)) {
                JS_FreeCString(ctx, nb);
                JS_FreeValue(ctx, name);
                JS_FreePropertyEnum(ctx, tab2, nprops2);
                return -1;
            }
        unevalp_next:
            JS_FreeCString(ctx, nb);
            JS_FreeValue(ctx, name);
        }
        JS_FreePropertyEnum(ctx, tab2, nprops2);
        if (failed)
            r = 0;
    }

    if (it == DYN_SC_T_ARRAY && (n->f & SC_F_UNEVALI)) {
        dyn_sceval_t* rec = v->cur_eval;
        uint64_t len = dyn_sc_arr_len(ctx, inst), i2;
        int failed = 0;
        if (len == (uint64_t)-1)
            return -1;
        if (len > DYN_SC_MAX_ARRAY) {
            JS_ThrowRangeError(ctx,
                "dyna:schema: arrays longer than %d elements are not validated",
                DYN_SC_MAX_ARRAY);
            return -1;
        }
        for (i2 = 0; i2 < len; i2++) {
            if (dyn_sc_eval_has_item(rec, i2))
                continue;
            if (n->unevali) {
                JSValue el = JS_GetPropertyUint32(ctx, inst, (uint32_t)i2);
                char tok[24];
                int tlen, rr;
                if (JS_IsException(el))
                    return -1;
                tlen = snprintf(tok, sizeof tok, "%llu",
                    (unsigned long long)i2);
                rr = dyn_sc_check_child(v, n->unevali, el, tok,
                    (size_t)tlen);
                JS_FreeValue(ctx, el);
                if (rr < 0)
                    return -1;
                if (!rr)
                    failed = 1;
                else if (dyn_sc_eval_add_item(ctx, rec, i2))
                    return -1;
            } else if (!n->unevali_allow) {
                dyn_sc_add_err(v, "unevaluatedItems",
                    "item is unevaluated and the schema allows "
                    "none");
                failed = 1;
            } else if (dyn_sc_eval_add_item(ctx, rec, i2)) {
                return -1;
            }
        }
        if (failed)
            r = 0;
    }

    return r;
}

static int dyn_sc_check(dyn_vctx_t* v, dyn_scnode_t* n, JSValueConst inst)
{
    dyn_sceval_t rec;
    dyn_sceval_t* saved = v->cur_eval;
    int r;

    if (!saved && !(n->f & SC_F_UREACH))
        return dyn_sc_check_impl(v, n, inst);
    memset(&rec, 0, sizeof rec);
    v->cur_eval = &rec;
    r = dyn_sc_check_impl(v, n, inst);
    v->cur_eval = saved;
    if (r > 0 && saved && dyn_sc_eval_merge(v->ctx, saved, &rec))
        r = -1;
    dyn_sc_eval_free(v->ctx, &rec);
    return r;
}

#endif
