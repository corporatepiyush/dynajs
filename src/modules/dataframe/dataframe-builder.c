#include "dataframe-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_DATAFRAME)

static inline uint32_t df_str_hash_basis(void)
{
    uint64_t seed = atomic_load_explicit(&dfc_hash_seed, memory_order_relaxed);
    return 2166136261u ^ (uint32_t)(seed ^ (seed >> 32));
}

#define DFB_GATHER_ONE(cty)     \
    do {                        \
        const cty* x = b.p;     \
        cty* d = out;           \
        for (i = 0; i < n; i++) \
            d[i] = x[rows[i]];  \
    } while (0)

#define DFB_GATHER_F64(cty)        \
    do {                           \
        const cty* x = b.p;        \
        double* d = out;           \
        for (i = 0; i < n; i++) {  \
            uint32_t r = rows[i];  \
            if (r == DFB_NO_ROW) { \
                d[i] = DYN_NAN;    \
                continue;          \
            }                      \
            if (r >= b.n)          \
                goto bad_row;      \
            d[i] = (double)x[r];   \
        }                          \
    } while (0)

int df_add_strings(JSContext* ctx, DataFrame* df, const char* name,
    JSValueConst arr, uint32_t n)
{
    DFColumn* c = &df->cols[df->ncols];
    uint32_t i;
    uint32_t *dfh = NULL, dfh_mask = 0;
    size_t dfh_cap = 16;

    memset(c, 0, sizeof(*c));
    c->name = strdup(name);
    c->type = DF_STR;
    c->buffer = JS_UNDEFINED;
    c->length = n;
    c->codes = malloc((size_t)(n ? n : 1) * sizeof(int32_t));
    if (!c->name || !c->codes)
        goto oom;
    while (dfh_cap < (size_t)n * 2 + 8)
        dfh_cap <<= 1;
    dfh = (uint32_t*)malloc(dfh_cap * sizeof(uint32_t));
    if (!dfh)
        goto oom;
    memset(dfh, 0xFF, dfh_cap * sizeof(uint32_t));
    dfh_mask = (uint32_t)(dfh_cap - 1);

    for (i = 0; i < n; i++) {
        JSValue v = JS_GetPropertyUint32(ctx, arr, i);
        const char* s;
        uint32_t k;
        if (JS_IsException(v))
            goto fail;
        s = JS_ToCString(ctx, v);
        JS_FreeValue(ctx, v);
        if (!s)
            goto fail;
        {
            uint32_t h = df_str_hash_basis(), m, probe;
            const char* cp;
            for (cp = s; *cp; cp++) {
                h ^= (unsigned char)*cp;
                h *= 16777619u;
            }
            m = dfh_mask;
            probe = h & m;
            k = c->dict_len;
            while (dfh[probe] != 0xFFFFFFFFu) {
                if (strcmp(c->dict[dfh[probe]], s) == 0) {
                    k = dfh[probe];
                    break;
                }
                probe = (probe + 1) & m;
            }
            if (k == c->dict_len)
                dfh[probe] = c->dict_len;
        }
        if (k == c->dict_len) {
            if (c->dict_len == c->dict_cap) {
                uint32_t nc = c->dict_cap ? c->dict_cap << 1 : 16;
                char** nd = realloc(c->dict, nc * sizeof(char*));
                if (!nd) {
                    JS_FreeCString(ctx, s);
                    goto oom;
                }
                c->dict = nd;
                c->dict_cap = nc;
            }
            c->dict[c->dict_len] = strdup(s);
            if (!c->dict[c->dict_len]) {
                JS_FreeCString(ctx, s);
                goto oom;
            }
            c->dict_len++;
        }
        JS_FreeCString(ctx, s);
        c->codes[i] = (int32_t)k;
    }
    df->ncols++;
    free(dfh);
    return 0;
oom:
    JS_ThrowOutOfMemory(ctx);
fail:
    free(dfh);
    df_col_free(c);
    memset(c, 0, sizeof(*c));
    return -1;
}

int df_add_dict(JSContext* ctx, DataFrame* df, const char* name, JSValueConst obj)
{
    DFColumn* c = &df->cols[df->ncols];
    JSValue codes = JS_UNDEFINED, values = JS_UNDEFINED, lv;
    uint32_t nv = 0, i, *slot = NULL, mask;
    size_t off = 0, blen = 0, bpe = 0, slots = 16;
    JSValue ab;
    const uint8_t* src;
    DFType ctype = DF_F64;
    int rc = -1;

    memset(c, 0, sizeof(*c));
    c->buffer = JS_UNDEFINED;
    codes = JS_GetPropertyStr(ctx, obj, "codes");
    values = JS_GetPropertyStr(ctx, obj, "values");
    if (JS_IsException(codes) || JS_IsException(values))
        goto done;
    if (JS_GetBufferKind(codes) != JS_BUFFER_KIND_VIEW || !JS_IsArray(ctx, values)) {
        JS_ThrowTypeError(ctx, "column '%s': expected a dictionary column {codes: Int32Array, values: string[]}", name);
        goto done;
    }
    ab = JS_GetTypedArrayBuffer(ctx, codes, &off, &blen, &bpe);
    if (JS_IsException(ab))
        goto done;
    src = JS_GetArrayBuffer(ctx, &slots, ab);
    JS_FreeValue(ctx, ab);
    slots = 16;
    if (!src || bpe != 4 || df_type_of_value(codes, &ctype) < 0 || ctype != DF_I32) {
        if (!JS_HasException(ctx))
            JS_ThrowTypeError(ctx, "column '%s': dictionary codes must be an Int32Array", name);
        goto done;
    }
    lv = JS_GetPropertyStr(ctx, values, "length");
    if (JS_IsException(lv) || JS_ToUint32(ctx, &nv, lv)) {
        JS_FreeValue(ctx, lv);
        goto done;
    }
    JS_FreeValue(ctx, lv);
    c->name = strdup(name);
    c->type = DF_STR;
    c->length = (uint32_t)(blen / 4);
    c->codes = malloc(blen ? blen : 4);
    c->dict = calloc(nv ? nv : 1, sizeof(char*));
    c->dict_cap = nv ? nv : 1;
    while (slots < (size_t)nv * 2 + 8)
        slots <<= 1;
    slot = (uint32_t*)malloc(slots * sizeof(uint32_t));
    if (!c->name || !c->codes || !c->dict || !slot) {
        JS_ThrowOutOfMemory(ctx);
        goto done;
    }
    memset(slot, 0xFF, slots * sizeof(uint32_t));
    mask = (uint32_t)(slots - 1);
    for (i = 0; i < nv; i++) {
        JSValue sv = JS_GetPropertyUint32(ctx, values, i);
        const char* cs;
        uint32_t h = df_str_hash_basis(), probe;
        const char* cp;
        if (JS_IsException(sv))
            goto done;
        if (!JS_IsString(sv)) {
            JS_FreeValue(ctx, sv);
            JS_ThrowTypeError(ctx, "column '%s': dictionary values[%u] is not a string", name, i);
            goto done;
        }
        cs = JS_ToCString(ctx, sv);
        JS_FreeValue(ctx, sv);
        if (!cs)
            goto done;
        for (cp = cs; *cp; cp++) {
            h ^= (unsigned char)*cp;
            h *= 16777619u;
        }
        probe = h & mask;
        while (slot[probe] != 0xFFFFFFFFu) {
            if (strcmp(c->dict[slot[probe]], cs) == 0) {
                JS_ThrowTypeError(ctx, "column '%s': dictionary values[%u] repeats values[%u]", name, i, slot[probe]);
                JS_FreeCString(ctx, cs);
                goto done;
            }
            probe = (probe + 1) & mask;
        }
        c->dict[i] = strdup(cs);
        JS_FreeCString(ctx, cs);
        if (!c->dict[i]) {
            JS_ThrowOutOfMemory(ctx);
            goto done;
        }
        slot[probe] = i;
        c->dict_len = i + 1;
    }
    memcpy(c->codes, src + off, blen);
    for (i = 0; i < c->length; i++)
        if ((uint32_t)c->codes[i] >= nv) {
            JS_ThrowRangeError(ctx, "column '%s': codes[%u] = %d is outside the %u dictionary values", name, i, c->codes[i], nv);
            goto done;
        }
    df->ncols++;
    rc = 0;
done:
    if (rc) {
        df_col_free(c);
        memset(c, 0, sizeof(*c));
    }
    free(slot);
    JS_FreeValue(ctx, codes);
    JS_FreeValue(ctx, values);
    return rc;
}

static void dfb_names_free(DFBuilder* B)
{
    free(B->name_slots);
    B->name_slots = NULL;
    B->name_mask = 0;
}

static uint32_t dfb_name_hash(const char* s)
{
    uint32_t h = 2166136261u;
    while (*s)
        h = (h ^ (uint8_t)*s++) * 16777619u;
    return h;
}

static int dfb_name_probe(JSContext* ctx, DFBuilder* B, const char* name, uint32_t* at)
{
    uint32_t p;
    if (!B->name_slots) {
        uint32_t sz = 64;
        while (sz < B->cap * 2)
            sz <<= 1;
        B->name_slots = calloc(sz, sizeof(uint32_t));
        if (!B->name_slots) {
            JS_ThrowOutOfMemory(ctx);
            return -1;
        }
        B->name_mask = sz - 1;
    }
    for (p = dfb_name_hash(name) & B->name_mask; B->name_slots[p]; p = (p + 1) & B->name_mask)
        if (strcmp(B->df->cols[B->name_slots[p] - 1].name, name) == 0)
            return 1;
    *at = p;
    return 0;
}

void dfb_free(JSContext* ctx, DFBuilder* B)
{
    uint32_t i;
    dfb_names_free(B);
    if (!B->df)
        return;
    for (i = 0; i < B->df->ncols; i++)
        JS_FreeValue(ctx, B->df->cols[i].buffer);
    dyn_df_dispose(B->df);
    B->df = NULL;
}

int dfb_init(JSContext* ctx, DFBuilder* B, const char* op,
    uint32_t ncols, uint64_t nrows)
{
    B->df = NULL;
    B->cap = ncols;
    B->nrows = (uint32_t)nrows;
    B->op = op;
    B->name_slots = NULL;
    B->name_mask = 0;
    if (ncols > DF_MAX_COLS)
        return JS_ThrowRangeError(ctx, "%s: too many columns (%u, max %d)",
                   op, ncols, DF_MAX_COLS),
               -1;
    if (nrows > (uint64_t)DFB_MAX_ROWS)
        return JS_ThrowRangeError(ctx, "%s: result would have %llu rows, the "
                                       "limit is %u",
                   op,
                   (unsigned long long)nrows, DFB_MAX_ROWS),
               -1;
    B->df = calloc(1, sizeof(DataFrame));
    if (B->df)
        B->df->cols = calloc(ncols ? ncols : 1, sizeof(DFColumn));
    if (!B->df || !B->df->cols) {
        dfb_free(ctx, B);
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    B->df->nrows = B->nrows;
    return 0;
}

static DFColumn* dfb_slot(JSContext* ctx, DFBuilder* B, const char* name,
    uint32_t n)
{
    DFColumn* c;
    uint32_t at = 0;
    int dup;

    if (!B->df) {
        JS_ThrowInternalError(ctx, "%s: builder used after seal", B->op);
        return NULL;
    }
    if (B->df->ncols >= B->cap) {
        JS_ThrowInternalError(ctx, "%s: more columns than the %u reserved",
            B->op, B->cap);
        return NULL;
    }
    if (n != B->nrows) {
        JS_ThrowRangeError(ctx, "%s: column '%s' has %u rows, the frame has %u",
            B->op, name, n, B->nrows);
        return NULL;
    }
    dup = B->cap > 32 ? dfb_name_probe(ctx, B, name, &at) : df_find_col(B->df, name) >= 0;
    if (dup < 0)
        return NULL;
    if (dup) {
        JS_ThrowRangeError(ctx, "%s: duplicate output column '%s'", B->op, name);
        return NULL;
    }
    c = &B->df->cols[B->df->ncols];
    memset(c, 0, sizeof(*c));
    c->buffer = JS_UNDEFINED;
    c->length = n;
    c->name = strdup(name);
    if (!c->name) {
        JS_ThrowOutOfMemory(ctx);
        return NULL;
    }
    B->df->ncols++;
    if (B->name_slots)
        B->name_slots[at] = B->df->ncols;
    return c;
}

int dfb_add(JSContext* ctx, DFBuilder* B, const char* name,
    DFType t, void* p, uint32_t n)
{
    DFColumn* c;
    JSValue ab;

    if (t == DF_STR || !p) {
        free(p);
        JS_ThrowInternalError(ctx, "%s: dfb_add('%s') needs a numeric type and "
                                   "a non-NULL buffer",
            B->op, name);
        return -1;
    }
    c = dfb_slot(ctx, B, name, n);
    if (!c) {
        free(p);
        return -1;
    }
    ab = JS_NewArrayBufferCopy(ctx, (const uint8_t*)p,
        (size_t)n * df_elt_size(t));
    free(p);
    if (JS_IsException(ab))
        return -1;
    c->type = t;
    c->buffer = ab;
    c->byte_offset = 0;
    return 0;
}

int dfb_add_str(JSContext* ctx, DFBuilder* B, const char* name,
    int32_t* codes, char** dict, uint32_t dict_len,
    uint32_t n)
{
    DFColumn* c;
    uint32_t i;

    if (!codes || (dict_len && !dict)) {
        JS_ThrowInternalError(ctx, "%s: dfb_add_str('%s') needs codes and a "
                                   "dictionary",
            B->op, name);
        goto drop;
    }
    c = dfb_slot(ctx, B, name, n);
    if (!c)
        goto drop;
    for (i = 0; i < n; i++)
        if (codes[i] < 0 || (uint32_t)codes[i] >= dict_len) {
            JS_ThrowRangeError(ctx, "%s: column '%s' code %d is outside the "
                                    "dictionary (%u entries)",
                B->op, name,
                codes[i], dict_len);
            goto drop;
        }
    c->type = DF_STR;
    c->codes = codes;
    c->dict = dict;
    c->dict_len = dict_len;
    c->dict_cap = dict_len;
    return 0;
drop:
    free(codes);
    if (dict) {
        for (i = 0; i < dict_len; i++)
            free(dict[i]);
        free(dict);
    }
    return -1;
}

int dfb_add_gather(JSContext* ctx, DFBuilder* B, const char* name,
    const DataFrame* sdf, int scol, DFType out_type,
    const uint32_t* rows, uint32_t n)
{
    const DFColumn* sc = &sdf->cols[scol];
    DFBound b;
    void* out;
    uint32_t i;

    if (dyn_df_bind(ctx, sdf, scol, &b))
        return -1;
    if (n && !rows) {
        JS_ThrowInternalError(ctx, "%s: gather of '%s' needs a row index array",
            B->op, sc->name);
        return -1;
    }
    if (out_type != b.type && out_type != DF_F64) {
        JS_ThrowTypeError(ctx, "%s: column '%s' is %s; gather it as that type "
                               "or as Float64Array",
            B->op, sc->name,
            df_type_name(b.type));
        return -1;
    }
    if (b.type == DF_STR) {
        int32_t* codes = df_out_alloc(ctx, n, sizeof(int32_t));
        char** dict;
        uint32_t d;
        if (!codes)
            return -1;
        for (i = 0; i < n; i++) {
            if (rows[i] >= b.n) {
                free(codes);
                JS_ThrowRangeError(ctx, "%s: column '%s' row %u is outside the "
                                        "source (%u rows); a string column has no "
                                        "missing value",
                    B->op, sc->name, rows[i],
                    b.n);
                return -1;
            }
            codes[i] = ((const int32_t*)b.p)[rows[i]];
        }
        dict = calloc(sc->dict_len ? sc->dict_len : 1, sizeof(char*));
        if (!dict) {
            free(codes);
            JS_ThrowOutOfMemory(ctx);
            return -1;
        }
        for (d = 0; d < sc->dict_len; d++) {
            dict[d] = strdup(sc->dict[d]);
            if (!dict[d]) {
                while (d--)
                    free(dict[d]);
                free(dict);
                free(codes);
                JS_ThrowOutOfMemory(ctx);
                return -1;
            }
        }
        return dfb_add_str(ctx, B, name, codes, dict, sc->dict_len, n);
    }

    out = df_out_alloc(ctx, n, df_elt_size(out_type));
    if (!out)
        return -1;
    if (out_type == DF_F64) {
        switch ((int)(b.type)) {
        case DF_F64:
            DFB_GATHER_F64(double);
            break;
        case DF_F32:
            DFB_GATHER_F64(float);
            break;
        case DF_I32:
            DFB_GATHER_F64(int32_t);
            break;
        case DF_U32:
            DFB_GATHER_F64(uint32_t);
            break;
        case DF_I16:
            DFB_GATHER_F64(int16_t);
            break;
        case DF_U16:
            DFB_GATHER_F64(uint16_t);
            break;
        case DF_I8:
            DFB_GATHER_F64(int8_t);
            break;
        default:
            DFB_GATHER_F64(uint8_t);
            break;
        }
    } else {
        for (i = 0; i < n; i++)
            if (rows[i] >= b.n)
                goto bad_row;
        switch ((int)(out_type)) {
        case DF_F32:
            DFB_GATHER_ONE(float);
            break;
        case DF_I32:
            DFB_GATHER_ONE(int32_t);
            break;
        case DF_U32:
            DFB_GATHER_ONE(uint32_t);
            break;
        case DF_I16:
            DFB_GATHER_ONE(int16_t);
            break;
        case DF_U16:
            DFB_GATHER_ONE(uint16_t);
            break;
        case DF_I8:
            DFB_GATHER_ONE(int8_t);
            break;
        default:
            DFB_GATHER_ONE(uint8_t);
            break;
        }
    }
    return dfb_add(ctx, B, name, out_type, out, n);
bad_row:
    free(out);
    JS_ThrowRangeError(ctx, "%s: column '%s' row %u is outside the source "
                            "(%u rows)",
        B->op, sc->name, rows[i], b.n);
    return -1;
}

JSValue dfb_seal(JSContext* ctx, DFBuilder* B)
{
    DataFrame* df = B->df;
    JSValue obj;
    uint32_t i;

    dfb_names_free(B);
    if (!df)
        return JS_ThrowInternalError(ctx, "%s: nothing to seal", B->op);
    if (df->ncols != B->cap) {
        JS_ThrowInternalError(ctx, "%s: reserved %u columns, filled %u",
            B->op, B->cap, df->ncols);
        dfb_free(ctx, B);
        return JS_EXCEPTION;
    }
    B->df = NULL;
    obj = dyn_plain_wrap(ctx, JS_UNDEFINED, dyn_df_class_id, df, NULL);
    if (JS_IsException(obj)) {
        for (i = 0; i < df->ncols; i++)
            JS_FreeValue(ctx, df->cols[i].buffer);
        dyn_df_dispose(df);
    }
    return obj;
}

void df_strtab_free(DfStrTab* t)
{
    uint32_t i;
    for (i = 0; i < t->dict_len; i++)
        free(t->dict[i]);
    free(t->dict);
    free(t->slots);
    memset(t, 0, sizeof(*t));
}

int df_strtab_put(JSContext* ctx, DfStrTab* t, const char* s,
    uint32_t* code)
{
    uint32_t h, p, k;

    if (!t->slots) {
        uint32_t ncap = 64;
        t->slots = malloc(ncap * sizeof(*t->slots));
        if (!t->slots) {
            JS_ThrowOutOfMemory(ctx);
            return -1;
        }
        memset(t->slots, 0xFF, ncap * sizeof(*t->slots));
        t->slots_mask = ncap - 1;
        t->dict_cap = 16;
        t->dict = malloc(t->dict_cap * sizeof(*t->dict));
        if (!t->dict) {
            JS_ThrowOutOfMemory(ctx);
            return -1;
        }
    }
    h = df_str_hash_basis();
    for (p = 0; s[p]; p++) {
        h ^= (unsigned char)s[p];
        h *= 16777619u;
    }
    p = h & t->slots_mask;
    while (t->slots[p] != 0xFFFFFFFFu) {
        if (strcmp(t->dict[t->slots[p]], s) == 0) {
            *code = t->slots[p];
            return 0;
        }
        p = (p + 1) & t->slots_mask;
    }
    if ((t->dict_len + 1) * 2 >= t->slots_mask + 1) {
        uint32_t ncap = (t->slots_mask + 1) << 1, i;
        uint32_t* ns = malloc(ncap * sizeof(*ns));
        if (!ns) {
            JS_ThrowOutOfMemory(ctx);
            return -1;
        }
        memset(ns, 0xFF, ncap * sizeof(*ns));
        for (i = 0; i < t->dict_len; i++) {
            uint32_t q = df_str_hash_basis(), c;
            for (c = 0; t->dict[i][c]; c++) {
                q ^= (unsigned char)t->dict[i][c];
                q *= 16777619u;
            }
            q &= ncap - 1;
            while (ns[q] != 0xFFFFFFFFu)
                q = (q + 1) & (ncap - 1);
            ns[q] = i;
        }
        free(t->slots);
        t->slots = ns;
        t->slots_mask = ncap - 1;
        p = h & t->slots_mask;
        while (t->slots[p] != 0xFFFFFFFFu)
            p = (p + 1) & t->slots_mask;
    }
    if (t->dict_len == t->dict_cap) {
        uint32_t nc = t->dict_cap << 1;
        char** nd = realloc(t->dict, nc * sizeof(*nd));
        if (!nd) {
            JS_ThrowOutOfMemory(ctx);
            return -1;
        }
        t->dict = nd;
        t->dict_cap = nc;
    }
    k = t->dict_len;
    t->dict[k] = strdup(s);
    if (!t->dict[k]) {
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    t->dict_len++;
    t->slots[p] = k;
    *code = k;
    return 0;
}

int df_strtab_find(const DfStrTab* t, const char* s)
{
    uint32_t h, p;

    if (!t->slots)
        return -1;
    h = df_str_hash_basis();
    for (p = 0; s[p]; p++) {
        h ^= (unsigned char)s[p];
        h *= 16777619u;
    }
    p = h & t->slots_mask;
    while (t->slots[p] != 0xFFFFFFFFu) {
        if (strcmp(t->dict[t->slots[p]], s) == 0)
            return (int)t->slots[p];
        p = (p + 1) & t->slots_mask;
    }
    return -1;
}

#endif
