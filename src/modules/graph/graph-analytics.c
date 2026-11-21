#include "graph-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_GRAPH)

#include "core/dyn-prng.h"

#define GX_OFFLOAD_MIN ((uint64_t)1 << 16)
#define GX_MAX_ITER 100000000u
#define GX_MAX_TOPICS 65536u
#define GX_MAX_SEEDS (1u << 28)
#define GX_MAX_TOPIC_CELLS ((uint64_t)1 << 27)

static const char* const gx_names[GX_COUNT] = {
    "pageRank", "labelPropagation", "louvain", "modularity", "coreNumbers",
    "triangles", "clustering", "betweenness", "closeness",
    "stronglyConnectedComponents", "weaklyConnectedComponents",
    "topologicalLevels", "maxFlow", "bipartiteMatching", "linkPrediction", "topicPageRank"
};

static const char* const gx_pr_keys[] = { "damping", "tolerance", "maxIterations", "personalization" };
static const char* const gx_tpr_keys[] = { "damping", "tolerance", "maxIterations", "types", "typeWeights" };
static const char* const gx_lpa_keys[] = { "seed", "maxRounds" };
static const char* const gx_lv_keys[] = { "resolution", "threshold", "seed" };
static const char* const gx_mod_keys[] = { "resolution" };
static const char* const gx_bc_keys[] = { "normalized", "endpoints", "samples", "seed", "sources", "weighted" };
static const char* const gx_cl_keys[] = { "wfImproved", "weighted" };
static const char* const gx_match_keys[] = { "left" };
static const char* const gx_link_keys[] = { "method" };
static const char* const gx_csr_keys[] = { "offsets", "edges", "weights", "directed" };
static const char* const gx_link_methods[] = { "jaccard", "adamicAdar", "resourceAllocation", "commonNeighbors", "preferentialAttachment" };

static JSClassID gx_u32_cid, gx_f64_cid, gx_u8_cid;

typedef struct {
    int op;
    int method;
    JSValue this_val;
    int argc;
    JSValue argv[4];
    dyn_gk_csr_t g;
    int directed;
    int use_w;
    double num[3];
    uint32_t cnt[2];
    uint64_t seed;
    int flag[2];
    double* vec;
    int32_t* ivec;
    uint32_t* idx;
    uint8_t* bytes;
    uint32_t nidx;
    double* outf;
    int32_t* outi;
    uint8_t* outb;
    double res[3];
    uint32_t rcount;
    int rc;
    uint32_t ntopic;
    uint32_t mask;
    uint32_t bad_topic;
    uint32_t* toff;
    uint32_t* titers;
    double* tyw;
    double* pmat;
} gx_job_t;

static int gx_stop(void* ud)
{
    (void)ud;
    return dyn_fiber_cancel_state() > 0;
}

static const dyn_gk_ctl_t gx_ctl = { gx_stop, NULL };

static void gx_job_free(JSContext* ctx, gx_job_t* j)
{
    int i;
    JS_FreeValue(ctx, j->this_val);
    for (i = 0; i < j->argc; i++)
        JS_FreeValue(ctx, j->argv[i]);
    dyn_gk_csr_free(&j->g);
    free(j->vec);
    free(j->ivec);
    free(j->idx);
    free(j->bytes);
    free(j->outf);
    free(j->outi);
    free(j->outb);
    free(j->toff);
    free(j->titers);
    free(j->tyw);
    free(j->pmat);
    free(j);
}

static JSValue gx_arg(gx_job_t* j, int i)
{
    i += j->method ? 0 : 1;
    return i < j->argc ? j->argv[i] : JS_UNDEFINED;
}

static int gx_view(JSContext* ctx, JSValueConst v, JSClassID* cid, uint8_t** p, size_t* nelem)
{
    JSValue buf;
    uint8_t* base;
    size_t off, len, bpe, ab;
    JSClassID c = JS_GetClassID(v);
    if (c != dyn_graph_i32_class_id && c != gx_u32_cid && c != gx_f64_cid && c != gx_u8_cid)
        return 0;
    buf = JS_GetTypedArrayBuffer(ctx, v, &off, &len, &bpe);
    if (JS_IsException(buf))
        return -1;
    base = JS_GetArrayBuffer(ctx, &ab, buf);
    JS_FreeValue(ctx, buf);
    if (!base || off > ab || len > ab - off) {
        JS_ThrowRangeError(ctx, "dyna:graph typed array is detached or out of bounds");
        return -1;
    }
    *cid = c;
    *p = base + off;
    *nelem = len / bpe;
    return 1;
}

static int gx_len(JSContext* ctx, JSValueConst v, uint64_t* n)
{
    JSValue lv;
    int64_t l;
    lv = JS_GetPropertyStr(ctx, v, "length");
    if (JS_IsException(lv))
        return -1;
    if (JS_ToInt64(ctx, &l, lv)) {
        JS_FreeValue(ctx, lv);
        return -1;
    }
    JS_FreeValue(ctx, lv);
    *n = l < 0 ? 0 : (uint64_t)l;
    return 0;
}

static int gx_read_f64(JSContext* ctx, JSValueConst v, const char* what, uint64_t want, double** out)
{
    JSClassID cid;
    uint8_t* p;
    size_t n;
    uint64_t i, len;
    int r = gx_view(ctx, v, &cid, &p, &n);
    if (r < 0)
        return -1;
    if (r > 0) {
        if (cid != gx_f64_cid) {
            JS_ThrowTypeError(ctx, "%s must be a Float64Array or an array of numbers", what);
            return -1;
        }
        if (n != want) {
            JS_ThrowRangeError(ctx, "%s has %llu entries, expected %llu", what, (unsigned long long)n, (unsigned long long)want);
            return -1;
        }
        *out = (double*)malloc((want ? (size_t)want : 1) * sizeof(double));
        if (!*out) {
            JS_ThrowOutOfMemory(ctx);
            return -1;
        }
        memcpy(*out, p, (size_t)want * sizeof(double));
        return 0;
    }
    if (!JS_IsArray(ctx, v)) {
        JS_ThrowTypeError(ctx, "%s must be a Float64Array or an array of numbers", what);
        return -1;
    }
    if (gx_len(ctx, v, &len) < 0)
        return -1;
    if (len != want) {
        JS_ThrowRangeError(ctx, "%s has %llu entries, expected %llu", what, (unsigned long long)len, (unsigned long long)want);
        return -1;
    }
    *out = (double*)malloc((want ? (size_t)want : 1) * sizeof(double));
    if (!*out) {
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    for (i = 0; i < len; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, v, (uint32_t)i);
        int bad = JS_IsException(e) || JS_ToFloat64(ctx, &(*out)[i], e);
        JS_FreeValue(ctx, e);
        if (bad) {
            free(*out);
            *out = NULL;
            return -1;
        }
    }
    return 0;
}

static int gx_read_idx(JSContext* ctx, JSValueConst v, const char* what, uint64_t cap, uint32_t bound, uint32_t** out, uint32_t* pn)
{
    JSClassID cid;
    uint8_t* p;
    size_t n;
    uint64_t i, len;
    int r = gx_view(ctx, v, &cid, &p, &n);
    if (r < 0)
        return -1;
    if (r > 0) {
        if (cid == gx_f64_cid) {
            JS_ThrowTypeError(ctx, "%s must hold integers (an Int32Array, Uint32Array, Uint8Array or an array)", what);
            return -1;
        }
        len = n;
    } else {
        if (!JS_IsArray(ctx, v)) {
            JS_ThrowTypeError(ctx, "%s must be an Int32Array, Uint32Array, Uint8Array or an array of integers", what);
            return -1;
        }
        if (gx_len(ctx, v, &len) < 0)
            return -1;
    }
    if (len > cap) {
        JS_ThrowRangeError(ctx, "%s has %llu entries, at most %llu", what, (unsigned long long)len, (unsigned long long)cap);
        return -1;
    }
    *out = (uint32_t*)malloc((len ? (size_t)len : 1) * sizeof(uint32_t));
    if (!*out) {
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    for (i = 0; i < len; i++) {
        double d;
        if (r > 0) {
            if (cid == dyn_graph_i32_class_id)
                d = (double)((const int32_t*)(const void*)p)[i];
            else if (cid == gx_u32_cid)
                d = (double)((const uint32_t*)(const void*)p)[i];
            else
                d = (double)p[i];
        } else {
            JSValue e = JS_GetPropertyUint32(ctx, v, (uint32_t)i);
            int bad = JS_IsException(e) || JS_ToFloat64(ctx, &d, e);
            JS_FreeValue(ctx, e);
            if (bad) {
                free(*out);
                *out = NULL;
                return -1;
            }
        }
        if (!(d >= 0) || d >= (double)bound || d != (double)(uint32_t)d) {
            JS_ThrowRangeError(ctx, "%s[%llu] = %g is not an integer in 0..%u", what, (unsigned long long)i, d, bound - 1);
            free(*out);
            *out = NULL;
            return -1;
        }
        (*out)[i] = (uint32_t)d;
    }
    *pn = (uint32_t)len;
    return 0;
}

static int gx_csr_arg(JSContext* ctx, gx_job_t* j, JSValueConst o)
{
    JSValue off = JS_UNDEFINED, ed = JS_UNDEFINED, wv = JS_UNDEFINED, dv = JS_UNDEFINED;
    uint32_t *po = NULL, *pe = NULL, no = 0, ne = 0;
    int rc = -1, d;
    if (!JS_IsObject(o)) {
        JS_ThrowTypeError(ctx, "%s: expected a CSR object { offsets, edges, weights?, directed? }", gx_names[j->op]);
        return -1;
    }
    if (dyn_opts_strict(ctx, o, gx_csr_keys, countof(gx_csr_keys)))
        return -1;
    off = JS_GetPropertyStr(ctx, o, "offsets");
    ed = JS_GetPropertyStr(ctx, o, "edges");
    wv = JS_GetPropertyStr(ctx, o, "weights");
    dv = JS_GetPropertyStr(ctx, o, "directed");
    if (JS_IsException(off) || JS_IsException(ed) || JS_IsException(wv) || JS_IsException(dv))
        goto done;
    d = JS_IsUndefined(dv) ? 1 : JS_ToBool(ctx, dv);
    if (d < 0)
        goto done;
    j->directed = d;
    if (JS_GetClassID(off) != dyn_graph_i32_class_id && JS_GetClassID(off) != gx_u32_cid) {
        JS_ThrowTypeError(ctx, "%s: csr.offsets must be an Int32Array or Uint32Array", gx_names[j->op]);
        goto done;
    }
    if (JS_GetClassID(ed) != dyn_graph_i32_class_id && JS_GetClassID(ed) != gx_u32_cid) {
        JS_ThrowTypeError(ctx, "%s: csr.edges must be an Int32Array or Uint32Array", gx_names[j->op]);
        goto done;
    }
    if (gx_read_idx(ctx, off, "csr.offsets", (uint64_t)DYN_GRAPH_MAX_NODES + 1, UINT32_MAX, &po, &no) < 0)
        goto done;
    if (no == 0) {
        JS_ThrowRangeError(ctx, "%s: csr.offsets needs n + 1 entries (at least one)", gx_names[j->op]);
        goto done;
    }
    if (gx_read_idx(ctx, ed, "csr.edges", UINT32_MAX, no - 1 ? no - 1 : 1, &pe, &ne) < 0)
        goto done;
    j->g.n = no - 1;
    j->g.m = ne;
    j->g.off = po;
    j->g.tgt = pe;
    po = pe = NULL;
    if (!JS_IsUndefined(wv) && !JS_IsNull(wv)) {
        if (gx_read_f64(ctx, wv, "csr.weights", ne, &j->g.w) < 0)
            goto done;
        j->use_w = 1;
    }
    if (dyn_gk_csr_validate(&j->g) < 0) {
        JS_ThrowRangeError(ctx, "%s: malformed CSR (offsets must start at 0, never decrease and end at edges.length; targets < n; weights finite)", gx_names[j->op]);
        goto done;
    }
    rc = 0;
done:
    free(po);
    free(pe);
    JS_FreeValue(ctx, off);
    JS_FreeValue(ctx, ed);
    JS_FreeValue(ctx, wv);
    JS_FreeValue(ctx, dv);
    return rc;
}

static int gx_opt_num(JSContext* ctx, JSValueConst opts, const char* key, double lo, double hi, int integer, double* out)
{
    JSValue v;
    double d;
    if (!JS_IsObject(opts))
        return 0;
    v = JS_GetPropertyStr(ctx, opts, key);
    if (JS_IsException(v))
        return -1;
    if (JS_IsUndefined(v))
        return 0;
    if (JS_ToFloat64(ctx, &d, v)) {
        JS_FreeValue(ctx, v);
        return -1;
    }
    JS_FreeValue(ctx, v);
    if (!(d >= lo && d <= hi) || (integer && d != floor(d))) {
        JS_ThrowRangeError(ctx, "options.%s must be %s in [%g, %g]", key, integer ? "an integer" : "a number", lo, hi);
        return -1;
    }
    *out = d;
    return 1;
}

static int gx_opt_bool(JSContext* ctx, JSValueConst opts, const char* key, int* out)
{
    JSValue v;
    int b;
    if (!JS_IsObject(opts))
        return 0;
    v = JS_GetPropertyStr(ctx, opts, key);
    if (JS_IsException(v))
        return -1;
    if (JS_IsUndefined(v))
        return 0;
    b = JS_ToBool(ctx, v);
    JS_FreeValue(ctx, v);
    if (b < 0)
        return -1;
    *out = b;
    return 1;
}

static int gx_opts(JSContext* ctx, JSValueConst opts, const char* const* keys, int nkeys, const char* op)
{
    if (JS_IsUndefined(opts) || JS_IsNull(opts))
        return 0;
    if (!JS_IsObject(opts)) {
        JS_ThrowTypeError(ctx, "%s: options must be an object", op);
        return -1;
    }
    return dyn_opts_strict(ctx, opts, keys, nkeys);
}

static int gx_seed(JSContext* ctx, JSValueConst opts, uint64_t* seed)
{
    double d = 0;
    int r = gx_opt_num(ctx, opts, "seed", 0, 9007199254740991.0, 1, &d);
    if (r < 0)
        return -1;
    *seed = (uint64_t)d;
    return 0;
}

static int gx_parse_opts(JSContext* ctx, gx_job_t* j)
{
    JSValueConst opts;
    const char* op = gx_names[j->op];
    uint32_t n = j->g.n;
    double d;
    switch (j->op) {
    case GX_PAGERANK:
        opts = gx_arg(j, 0);
        if (gx_opts(ctx, opts, gx_pr_keys, countof(gx_pr_keys), op))
            return -1;
        j->num[0] = 0.85;
        j->num[1] = 1e-6;
        j->num[2] = 100;
        if (gx_opt_num(ctx, opts, "damping", 0, 1, 0, &j->num[0]) < 0
            || gx_opt_num(ctx, opts, "tolerance", 0, 1, 0, &j->num[1]) < 0
            || gx_opt_num(ctx, opts, "maxIterations", 1, GX_MAX_ITER, 1, &j->num[2]) < 0)
            return -1;
        if (JS_IsObject(opts)) {
            JSValue p = JS_GetPropertyStr(ctx, opts, "personalization");
            int bad;
            if (JS_IsException(p))
                return -1;
            bad = !JS_IsUndefined(p) && gx_read_f64(ctx, p, "options.personalization", n, &j->vec) < 0;
            JS_FreeValue(ctx, p);
            if (bad)
                return -1;
        }
        return 0;
    case GX_LPA:
        opts = gx_arg(j, 0);
        if (gx_opts(ctx, opts, gx_lpa_keys, countof(gx_lpa_keys), op) || gx_seed(ctx, opts, &j->seed))
            return -1;
        j->num[0] = 100;
        return gx_opt_num(ctx, opts, "maxRounds", 1, GX_MAX_ITER, 1, &j->num[0]) < 0 ? -1 : 0;
    case GX_LOUVAIN:
        opts = gx_arg(j, 0);
        if (gx_opts(ctx, opts, gx_lv_keys, countof(gx_lv_keys), op) || gx_seed(ctx, opts, &j->seed))
            return -1;
        j->num[0] = 1.0;
        j->num[1] = 1e-7;
        if (gx_opt_num(ctx, opts, "resolution", 0, 1e300, 0, &j->num[0]) < 0
            || gx_opt_num(ctx, opts, "threshold", -1e300, 1e300, 0, &j->num[1]) < 0)
            return -1;
        return 0;
    case GX_MODULARITY: {
        uint32_t nc = 0;
        opts = gx_arg(j, 1);
        if (gx_opts(ctx, opts, gx_mod_keys, countof(gx_mod_keys), op))
            return -1;
        j->num[0] = 1.0;
        if (gx_opt_num(ctx, opts, "resolution", 0, 1e300, 0, &j->num[0]) < 0)
            return -1;
        if (gx_read_idx(ctx, gx_arg(j, 0), "communities", n, n ? n : 1, &j->idx, &nc) < 0)
            return -1;
        if (nc != n) {
            JS_ThrowRangeError(ctx, "modularity: communities has %u entries, the graph %u nodes", nc, n);
            return -1;
        }
        return 0;
    }
    case GX_BETWEENNESS: {
        JSValue sv;
        int w = j->use_w, r;
        opts = gx_arg(j, 0);
        if (gx_opts(ctx, opts, gx_bc_keys, countof(gx_bc_keys), op) || gx_seed(ctx, opts, &j->seed))
            return -1;
        j->flag[0] = 1;
        j->flag[1] = 0;
        if (gx_opt_bool(ctx, opts, "normalized", &j->flag[0]) < 0 || gx_opt_bool(ctx, opts, "endpoints", &j->flag[1]) < 0
            || gx_opt_bool(ctx, opts, "weighted", &w) < 0)
            return -1;
        if (w && !j->g.w) {
            JS_ThrowTypeError(ctx, "betweenness: weighted: true needs edge weights");
            return -1;
        }
        j->use_w = w;
        d = n;
        r = gx_opt_num(ctx, opts, "samples", 1, n ? n : 1, 1, &d);
        if (r < 0)
            return -1;
        j->cnt[0] = (uint32_t)d;
        j->cnt[1] = r > 0;
        sv = JS_IsObject(opts) ? JS_GetPropertyStr(ctx, opts, "sources") : JS_UNDEFINED;
        if (JS_IsException(sv))
            return -1;
        if (!JS_IsUndefined(sv)) {
            uint8_t* seen;
            uint32_t i;
            if (r > 0) {
                JS_FreeValue(ctx, sv);
                JS_ThrowTypeError(ctx, "betweenness: give samples or sources, not both");
                return -1;
            }
            r = gx_read_idx(ctx, sv, "options.sources", n, n ? n : 1, &j->idx, &j->nidx);
            JS_FreeValue(ctx, sv);
            if (r < 0)
                return -1;
            seen = (uint8_t*)calloc(n ? n : 1, 1);
            if (!seen) {
                JS_ThrowOutOfMemory(ctx);
                return -1;
            }
            for (i = 0; i < j->nidx; i++) {
                if (seen[j->idx[i]]) {
                    free(seen);
                    JS_ThrowRangeError(ctx, "betweenness: options.sources repeats node %u", j->idx[i]);
                    return -1;
                }
                seen[j->idx[i]] = 1;
            }
            free(seen);
            j->cnt[0] = j->nidx;
            j->cnt[1] = 2;
        }
        return 0;
    }
    case GX_CLOSENESS: {
        int w = j->use_w;
        opts = gx_arg(j, 0);
        if (gx_opts(ctx, opts, gx_cl_keys, countof(gx_cl_keys), op))
            return -1;
        j->flag[0] = 1;
        if (gx_opt_bool(ctx, opts, "wfImproved", &j->flag[0]) < 0 || gx_opt_bool(ctx, opts, "weighted", &w) < 0)
            return -1;
        if (w && !j->g.w) {
            JS_ThrowTypeError(ctx, "closeness: weighted: true needs edge weights");
            return -1;
        }
        j->use_w = w;
        return 0;
    }
    case GX_MAXFLOW: {
        uint32_t s, t;
        if (dyn_idx_arg(ctx, gx_arg(j, 0), &s) || dyn_idx_arg(ctx, gx_arg(j, 1), &t))
            return -1;
        if (s >= n || t >= n) {
            JS_ThrowRangeError(ctx, "maxFlow: source %u / sink %u out of range for %u nodes", s, t, n);
            return -1;
        }
        if (s == t) {
            JS_ThrowRangeError(ctx, "maxFlow: source and sink are the same node (%u)", s);
            return -1;
        }
        j->cnt[0] = s;
        j->cnt[1] = t;
        return 0;
    }
    case GX_MATCHING: {
        JSValue lv;
        uint32_t nl = 0, *l = NULL, i;
        opts = gx_arg(j, 0);
        if (gx_opts(ctx, opts, gx_match_keys, countof(gx_match_keys), op))
            return -1;
        lv = JS_IsObject(opts) ? JS_GetPropertyStr(ctx, opts, "left") : JS_UNDEFINED;
        if (JS_IsException(lv))
            return -1;
        if (JS_IsUndefined(lv))
            return 0;
        if (JS_IsArray(ctx, lv)) {
            uint64_t len;
            if (gx_len(ctx, lv, &len) < 0) {
                JS_FreeValue(ctx, lv);
                return -1;
            }
            if (len != n) {
                JS_FreeValue(ctx, lv);
                JS_ThrowRangeError(ctx, "bipartiteMatching: options.left has %llu entries, the graph %u nodes", (unsigned long long)len, n);
                return -1;
            }
            j->bytes = (uint8_t*)malloc(n ? n : 1);
            if (!j->bytes) {
                JS_FreeValue(ctx, lv);
                JS_ThrowOutOfMemory(ctx);
                return -1;
            }
            for (i = 0; i < n; i++) {
                JSValue e = JS_GetPropertyUint32(ctx, lv, i);
                int b = JS_IsException(e) ? -1 : JS_ToBool(ctx, e);
                JS_FreeValue(ctx, e);
                if (b < 0) {
                    JS_FreeValue(ctx, lv);
                    return -1;
                }
                j->bytes[i] = (uint8_t)b;
            }
            JS_FreeValue(ctx, lv);
            return 0;
        }
        if (gx_read_idx(ctx, lv, "options.left", n, 2, &l, &nl) < 0) {
            JS_FreeValue(ctx, lv);
            return -1;
        }
        JS_FreeValue(ctx, lv);
        if (nl != n) {
            free(l);
            JS_ThrowRangeError(ctx, "bipartiteMatching: options.left has %u entries, the graph %u nodes", nl, n);
            return -1;
        }
        j->bytes = (uint8_t*)malloc(n ? n : 1);
        if (!j->bytes) {
            free(l);
            JS_ThrowOutOfMemory(ctx);
            return -1;
        }
        for (i = 0; i < n; i++)
            j->bytes[i] = (uint8_t)l[i];
        free(l);
        return 0;
    }
    case GX_LINK: {
        JSValue pv = gx_arg(j, 0), mv;
        uint32_t np = 0, k;
        opts = gx_arg(j, 1);
        if (gx_opts(ctx, opts, gx_link_keys, countof(gx_link_keys), op))
            return -1;
        j->cnt[0] = DYN_GK_LINK_JACCARD;
        mv = JS_IsObject(opts) ? JS_GetPropertyStr(ctx, opts, "method") : JS_UNDEFINED;
        if (JS_IsException(mv))
            return -1;
        if (!JS_IsUndefined(mv)) {
            const char* s = JS_ToCString(ctx, mv);
            JS_FreeValue(ctx, mv);
            if (!s)
                return -1;
            for (k = 0; k < countof(gx_link_methods); k++)
                if (strcmp(s, gx_link_methods[k]) == 0)
                    break;
            JS_FreeCString(ctx, s);
            if (k == countof(gx_link_methods)) {
                JS_ThrowTypeError(ctx, "linkPrediction: options.method must be one of jaccard, adamicAdar, resourceAllocation, commonNeighbors, preferentialAttachment");
                return -1;
            }
            j->cnt[0] = k;
        }
        if (JS_IsArray(ctx, pv)) {
            JSValue first = JS_GetPropertyUint32(ctx, pv, 0);
            int nested;
            if (JS_IsException(first))
                return -1;
            nested = JS_IsArray(ctx, first);
            JS_FreeValue(ctx, first);
            if (nested) {
                uint64_t len, i;
                if (gx_len(ctx, pv, &len) < 0)
                    return -1;
                if (len > (uint64_t)UINT32_MAX / 2) {
                    JS_ThrowRangeError(ctx, "linkPrediction: too many pairs");
                    return -1;
                }
                j->idx = (uint32_t*)malloc((size_t)(len ? 2 * len : 2) * sizeof(uint32_t));
                if (!j->idx) {
                    JS_ThrowOutOfMemory(ctx);
                    return -1;
                }
                for (i = 0; i < len; i++) {
                    JSValue pr = JS_GetPropertyUint32(ctx, pv, (uint32_t)i);
                    uint32_t *two = NULL, nt = 0;
                    int bad;
                    if (JS_IsException(pr))
                        return -1;
                    bad = gx_read_idx(ctx, pr, "linkPrediction pair", 2, n ? n : 1, &two, &nt) < 0;
                    JS_FreeValue(ctx, pr);
                    if (bad)
                        return -1;
                    if (nt != 2) {
                        free(two);
                        JS_ThrowRangeError(ctx, "linkPrediction: pair %llu is not [u, v]", (unsigned long long)i);
                        return -1;
                    }
                    j->idx[2 * i] = two[0];
                    j->idx[2 * i + 1] = two[1];
                    free(two);
                }
                j->nidx = (uint32_t)len;
                return 0;
            }
        }
        if (gx_read_idx(ctx, pv, "pairs", (uint64_t)UINT32_MAX - 1, n ? n : 1, &j->idx, &np) < 0)
            return -1;
        if (np & 1) {
            JS_ThrowRangeError(ctx, "linkPrediction: a flat pairs array needs an even length, got %u", np);
            return -1;
        }
        j->nidx = np / 2;
        return 0;
    }
    default:
        return 0;
    }
}

static int gx_topic_list(JSContext* ctx, JSValueConst topics, uint32_t* pnt)
{
    uint64_t len;
    if (!JS_IsArray(ctx, topics)) {
        JS_ThrowTypeError(ctx, "topicPageRank: topics must be an array");
        return -1;
    }
    if (gx_len(ctx, topics, &len) < 0)
        return -1;
    if (len > GX_MAX_TOPICS) {
        JS_ThrowRangeError(ctx, "topicPageRank: %llu topics, at most %u", (unsigned long long)len, GX_MAX_TOPICS);
        return -1;
    }
    *pnt = (uint32_t)len;
    return 0;
}

static int gx_topic_read(JSContext* ctx, gx_job_t* j, JSValueConst topics)
{
    static const char* const tkeys[] = { "seeds", "weights" };
    uint32_t t, nt = j->ntopic, total = 0;
    char what[64];
    j->toff = (uint32_t*)calloc((size_t)nt + 1, sizeof(uint32_t));
    if (!j->toff) {
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    for (t = 0; t < nt; t++) {
        JSValue el = JS_GetPropertyUint32(ctx, topics, t), sv = JS_UNDEFINED, wv = JS_UNDEFINED;
        uint32_t *seeds = NULL, ns = 0, k;
        double* w = NULL;
        uint8_t* vp;
        size_t vn;
        JSClassID cid;
        int r, bad = 0;
        if (JS_IsException(el))
            return -1;
        r = gx_view(ctx, el, &cid, &vp, &vn);
        if (r < 0) {
            JS_FreeValue(ctx, el);
            return -1;
        }
        snprintf(what, sizeof what, "topics[%u].seeds", t);
        if (r > 0 || JS_IsArray(ctx, el)) {
            sv = JS_DupValue(ctx, el);
        } else if (JS_IsObject(el) && !JS_IsFunction(ctx, el)) {
            if (dyn_opts_strict(ctx, el, tkeys, 2))
                bad = 1;
            else {
                sv = JS_GetPropertyStr(ctx, el, "seeds");
                wv = JS_IsException(sv) ? JS_UNDEFINED : JS_GetPropertyStr(ctx, el, "weights");
                bad = JS_IsException(sv) || JS_IsException(wv);
                if (!bad && JS_IsUndefined(sv)) {
                    JS_ThrowTypeError(ctx, "topicPageRank: topics[%u] needs seeds", t);
                    bad = 1;
                }
            }
        } else {
            JS_ThrowTypeError(ctx, "topicPageRank: topics[%u] must be an array of seed nodes or { seeds, weights? }", t);
            bad = 1;
        }
        JS_FreeValue(ctx, el);
        if (!bad && gx_read_idx(ctx, sv, what, GX_MAX_SEEDS, DYN_GRAPH_MAX_NODES, &seeds, &ns) < 0)
            bad = 1;
        if (!bad && ns == 0) {
            JS_ThrowRangeError(ctx, "topicPageRank: topics[%u] has no seeds", t);
            bad = 1;
        }
        if (!bad && !JS_IsUndefined(wv)) {
            snprintf(what, sizeof what, "topics[%u].weights", t);
            if (gx_read_f64(ctx, wv, what, ns, &w) < 0)
                bad = 1;
            else {
                double s = 0.0;
                for (k = 0; k < ns; k++) {
                    if (!(w[k] >= 0.0) || !isfinite(w[k])) {
                        JS_ThrowRangeError(ctx, "topicPageRank: topics[%u].weights[%u] must be finite and >= 0", t, k);
                        bad = 1;
                        break;
                    }
                    s += w[k];
                }
                if (!bad && !(s > 0.0 && isfinite(s))) {
                    JS_ThrowRangeError(ctx, "topicPageRank: topics[%u].weights must have a positive finite sum", t);
                    bad = 1;
                }
            }
        }
        JS_FreeValue(ctx, sv);
        JS_FreeValue(ctx, wv);
        if (!bad && (uint64_t)total + ns > GX_MAX_SEEDS) {
            JS_ThrowRangeError(ctx, "topicPageRank: more than %u seeds in all", GX_MAX_SEEDS);
            bad = 1;
        }
        if (!bad) {
            uint32_t* ns_seed = (uint32_t*)realloc(j->idx, ((size_t)total + ns) * sizeof(uint32_t));
            double* ns_w = ns_seed ? (double*)realloc(j->vec, ((size_t)total + ns) * sizeof(double)) : NULL;
            if (ns_seed)
                j->idx = ns_seed;
            if (ns_w)
                j->vec = ns_w;
            if (!ns_seed || !ns_w) {
                JS_ThrowOutOfMemory(ctx);
                bad = 1;
            } else {
                for (k = 0; k < ns; k++) {
                    j->idx[total + k] = seeds[k];
                    j->vec[total + k] = w ? w[k] : 1.0;
                }
                total += ns;
                j->toff[t + 1] = total;
            }
        }
        free(seeds);
        free(w);
        if (bad)
            return -1;
    }
    return 0;
}

static int gx_topic_prepare(JSContext* ctx, gx_job_t* j)
{
    JSValueConst topics = gx_arg(j, 0), opts = gx_arg(j, 1);
    const char* op = gx_names[GX_TOPIC];
    dyn_graph_t* g;
    uint32_t n, t, k;
    size_t cells;
    int rc;
    if (!graph_of(ctx, j->this_val))
        return -1;
    if (gx_topic_list(ctx, topics, &j->ntopic) || gx_opts(ctx, opts, gx_tpr_keys, countof(gx_tpr_keys), op))
        return -1;
    j->num[0] = 0.85;
    j->num[1] = 1e-6;
    j->num[2] = 100;
    j->mask = DYN_GRAPH_ALL_TYPES;
    if (gx_opt_num(ctx, opts, "damping", 0, 1, 0, &j->num[0]) < 0
        || gx_opt_num(ctx, opts, "tolerance", 0, 1, 0, &j->num[1]) < 0
        || gx_opt_num(ctx, opts, "maxIterations", 1, GX_MAX_ITER, 1, &j->num[2]) < 0)
        return -1;
    if (JS_IsObject(opts)) {
        JSValue tv = JS_GetPropertyStr(ctx, opts, "types"), wv;
        int bad = JS_IsException(tv) || (!JS_IsUndefined(tv) && dyn_graph_mask_arg(ctx, tv, op, &j->mask));
        JS_FreeValue(ctx, tv);
        if (bad)
            return -1;
        wv = JS_GetPropertyStr(ctx, opts, "typeWeights");
        bad = JS_IsException(wv) || (!JS_IsUndefined(wv) && gx_read_f64(ctx, wv, "options.typeWeights", DYN_GRAPH_MAX_TYPES, &j->tyw) < 0);
        JS_FreeValue(ctx, wv);
        if (bad)
            return -1;
        for (k = 0; j->tyw && k < DYN_GRAPH_MAX_TYPES; k++)
            if (!(j->tyw[k] >= 0.0) || !isfinite(j->tyw[k])) {
                JS_ThrowRangeError(ctx, "topicPageRank: options.typeWeights[%u] must be finite and >= 0", k);
                return -1;
            }
    }
    if (gx_topic_read(ctx, j, topics))
        return -1;
    g = graph_of(ctx, j->this_val);
    if (!g)
        return -1;
    n = g->n;
    for (k = 0; k < j->toff[j->ntopic]; k++)
        if (j->idx[k] >= n) {
            JS_ThrowRangeError(ctx, "topicPageRank: seed node %u out of range (%u nodes)", j->idx[k], n);
            return -1;
        }
    if ((uint64_t)n * j->ntopic > GX_MAX_TOPIC_CELLS) {
        JS_ThrowRangeError(ctx, "topicPageRank: topics x nodes = %llu exceeds %llu", (unsigned long long)n * j->ntopic, (unsigned long long)GX_MAX_TOPIC_CELLS);
        return -1;
    }
    if (dyn_gk_has_negative(g)) {
        JS_ThrowRangeError(ctx, "topicPageRank: negative edge weight");
        return -1;
    }
    j->directed = g->directed;
    j->use_w = 1;
    rc = dyn_gk_csr_of_graph_tw(g, j->mask, j->tyw, &j->g);
    if (rc == DYN_GK_E_TOO_LARGE) {
        JS_ThrowRangeError(ctx, "topicPageRank: graph too large");
        return -1;
    }
    if (rc < 0) {
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    cells = (size_t)n * j->ntopic;
    j->pmat = (double*)calloc(cells ? cells : 1, sizeof(double));
    if (!j->pmat) {
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    for (t = 0; t < j->ntopic; t++) {
        double s = 0.0;
        for (k = j->toff[t]; k < j->toff[t + 1]; k++)
            s += j->vec[k];
        for (k = j->toff[t]; k < j->toff[t + 1]; k++)
            j->pmat[(size_t)t * n + j->idx[k]] += j->vec[k] / s;
    }
    return 0;
}

static uint32_t* gx_sample(uint32_t n, uint32_t k, uint64_t seed)
{
    uint32_t *perm = (uint32_t*)malloc((size_t)(n ? n : 1) * sizeof(uint32_t)), i;
    dyn_prng_t r;
    if (!perm)
        return NULL;
    dyn_prng_seed(&r, seed);
    for (i = 0; i < n; i++)
        perm[i] = i;
    for (i = 0; i < k && i + 1 < n; i++) {
        uint32_t x = i + (uint32_t)dyn_prng_next_bounded(&r, n - i), t = perm[i];
        perm[i] = perm[x];
        perm[x] = t;
    }
    return perm;
}

static int gx_alloc_out(gx_job_t* j, int f, int i, int b)
{
    size_t n = j->g.n ? j->g.n : 1;
    if (f && !(j->outf = (double*)malloc(n * sizeof(double))))
        return DYN_GK_E_OOM;
    if (i && !(j->outi = (int32_t*)malloc(n * sizeof(int32_t))))
        return DYN_GK_E_OOM;
    if (b && !(j->outb = (uint8_t*)malloc(n)))
        return DYN_GK_E_OOM;
    return DYN_GK_OK;
}

static int gx_run(gx_job_t* j)
{
    dyn_gk_csr_t a = { 0, 0, NULL, NULL, NULL }, b = { 0, 0, NULL, NULL, NULL };
    uint32_t n = j->g.n;
    int rc;
    if (!j->use_w && j->g.w) {
        free(j->g.w);
        j->g.w = NULL;
    }
    switch (j->op) {
    case GX_PAGERANK: {
        dyn_gk_pr_opts_t o;
        if (dyn_gk_has_negative_w(&j->g, 1))
            return DYN_GK_E_NEG_WEIGHT;
        o.alpha = j->num[0];
        o.tol = j->num[1];
        o.max_iter = (uint32_t)j->num[2];
        o.pers = j->vec;
        if ((rc = gx_alloc_out(j, 1, 0, 0)) < 0)
            return rc;
        rc = dyn_gk_pagerank(&j->g, &o, j->outf, &gx_ctl);
        j->rcount = o.iters;
        return rc;
    }
    case GX_TOPIC: {
        size_t cells = (size_t)n * j->ntopic;
        j->outf = (double*)malloc((cells ? cells : 1) * sizeof(double));
        j->titers = (uint32_t*)calloc(j->ntopic ? j->ntopic : 1, sizeof(uint32_t));
        if (!j->outf || !j->titers)
            return DYN_GK_E_OOM;
        rc = dyn_gk_topic_pr(&j->g, j->ntopic, j->pmat, j->num[0], j->num[1], (uint32_t)j->num[2], j->outf, j->titers, &j->bad_topic, &gx_ctl);
        j->rcount = j->ntopic ? j->titers[j->bad_topic] : 0;
        return rc;
    }
    case GX_LPA:
        if (dyn_gk_has_negative_w(&j->g, 1))
            return DYN_GK_E_NEG_WEIGHT;
        if ((rc = gx_alloc_out(j, 0, 1, 0)) < 0 || (rc = dyn_gk_csr_sym(&j->g, 0, 1, &a)) < 0)
            return rc;
        rc = dyn_gk_lpa(&a, j->seed, (uint32_t)j->num[0], j->outi, &j->rcount);
        break;
    case GX_LOUVAIN: {
        dyn_gk_louvain_opts_t o;
        uint32_t e;
        if (dyn_gk_has_negative_w(&j->g, 1))
            return DYN_GK_E_NEG_WEIGHT;
        if (!j->g.w) {
            j->g.w = (double*)malloc((j->g.m ? (size_t)j->g.m : 1) * sizeof(double));
            if (!j->g.w)
                return DYN_GK_E_OOM;
            for (e = 0; e < j->g.m; e++)
                j->g.w[e] = 1.0;
        }
        if ((rc = gx_alloc_out(j, 0, 1, 0)) < 0 || (rc = dyn_gk_csr_sym(&j->g, j->directed ? 1 : 2, 1, &a)) < 0)
            return rc;
        o.resolution = j->num[0];
        o.threshold = j->num[1];
        o.seed = j->seed;
        rc = dyn_gk_louvain(&a, &o, j->outi, &j->rcount, &gx_ctl);
        j->res[0] = o.q;
        j->res[1] = o.levels;
        break;
    }
    case GX_MODULARITY:
        if (dyn_gk_has_negative_w(&j->g, 1))
            return DYN_GK_E_NEG_WEIGHT;
        return dyn_gk_modularity(&j->g, j->directed, (const int32_t*)j->idx, j->num[0], &j->res[0]);
    case GX_CORES:
        if ((rc = gx_alloc_out(j, 0, 1, 0)) < 0 || (rc = dyn_gk_csr_sym(&j->g, 0, 0, &a)) < 0)
            return rc;
        rc = dyn_gk_core_numbers(&a, j->outi);
        break;
    case GX_TRIANGLES:
    case GX_CLUSTERING:
        if ((rc = gx_alloc_out(j, 1, 0, 0)) < 0 || (rc = dyn_gk_csr_sym(&j->g, 0, 0, &a)) < 0)
            return rc;
        rc = dyn_gk_triangles(&a, j->outf, &j->res[0], &gx_ctl);
        if (rc == DYN_GK_OK && j->op == GX_CLUSTERING) {
            double* local = (double*)malloc((size_t)(n ? n : 1) * sizeof(double));
            if (!local) {
                rc = DYN_GK_E_OOM;
                break;
            }
            dyn_gk_clustering(&a, j->outf, local, &j->res[0], &j->res[1]);
            free(j->outf);
            j->outf = local;
        }
        break;
    case GX_BETWEENNESS: {
        uint32_t k = j->cnt[0], *src = j->idx;
        int sampled = j->cnt[1] == 2 ? k < n : (j->cnt[1] == 1 && k < n);
        if (j->use_w && dyn_gk_has_negative_w(&j->g, 0))
            return DYN_GK_E_NEG_WEIGHT;
        if (!src) {
            src = j->cnt[1] == 1 && k < n ? gx_sample(n, k, j->seed) : gx_sample(n, 0, 0);
            if (!src)
                return DYN_GK_E_OOM;
            free(j->idx);
            j->idx = src;
            j->nidx = k;
        }
        if ((rc = gx_alloc_out(j, 1, 0, 0)) < 0 || (rc = dyn_gk_csr_dedup(&j->g, j->use_w, &a)) < 0)
            return rc;
        rc = dyn_gk_betweenness(&a, src, k, j->flag[1], j->outf, &gx_ctl);
        if (rc == DYN_GK_OK)
            dyn_gk_betweenness_rescale(j->outf, n, j->flag[0], j->directed, j->flag[1], src, k, sampled);
        break;
    }
    case GX_CLOSENESS:
        if (j->use_w && dyn_gk_has_negative_w(&j->g, 1))
            return DYN_GK_E_NEG_WEIGHT;
        if ((rc = gx_alloc_out(j, 1, 0, 0)) < 0 || (rc = dyn_gk_csr_dedup(&j->g, j->use_w, &a)) < 0)
            return rc;
        if (j->directed) {
            if ((rc = dyn_gk_csr_transpose(&a, 1, &b)) < 0)
                break;
            rc = dyn_gk_closeness(&b, j->flag[0], j->outf, &gx_ctl);
        } else {
            rc = dyn_gk_closeness(&a, j->flag[0], j->outf, &gx_ctl);
        }
        break;
    case GX_SCC:
        if ((rc = gx_alloc_out(j, 0, 1, 0)) < 0)
            return rc;
        return dyn_gk_scc(&j->g, j->outi, &j->rcount);
    case GX_WCC:
        if ((rc = gx_alloc_out(j, 0, 1, 0)) < 0)
            return rc;
        return dyn_gk_wcc(&j->g, j->outi, &j->rcount);
    case GX_LEVELS:
        if ((rc = gx_alloc_out(j, 0, 1, 0)) < 0)
            return rc;
        return dyn_gk_topo_levels(&j->g, j->outi, &j->rcount);
    case GX_MAXFLOW:
        if (dyn_gk_has_negative_w(&j->g, 1))
            return DYN_GK_E_NEG_WEIGHT;
        if ((rc = gx_alloc_out(j, 0, 0, 1)) < 0)
            return rc;
        j->outf = (double*)malloc((j->g.m ? (size_t)j->g.m : 1) * sizeof(double));
        if (!j->outf)
            return DYN_GK_E_OOM;
        return dyn_gk_maxflow(&j->g, j->cnt[0], j->cnt[1], &j->res[0], j->outf, j->outb, &gx_ctl);
    case GX_MATCHING:
        if ((rc = gx_alloc_out(j, 0, 1, 0)) < 0 || (rc = dyn_gk_csr_sym(&j->g, 0, 0, &a)) < 0)
            return rc;
        if (!j->bytes) {
            j->bytes = (uint8_t*)malloc(n ? n : 1);
            if (!j->bytes) {
                rc = DYN_GK_E_OOM;
                break;
            }
            if ((rc = dyn_gk_bipartition(&a, j->bytes)) < 0)
                break;
        }
        rc = dyn_gk_matching(&a, j->bytes, j->outi, &j->rcount);
        break;
    case GX_LINK:
        if ((rc = dyn_gk_csr_sym(&j->g, 0, 0, &a)) < 0)
            return rc;
        j->outf = (double*)malloc((j->nidx ? (size_t)j->nidx : 1) * sizeof(double));
        if (!j->outf) {
            rc = DYN_GK_E_OOM;
            break;
        }
        rc = dyn_gk_link_scores(&a, j->idx, j->nidx, (int)j->cnt[0], j->outf);
        break;
    default:
        rc = DYN_GK_E_BAD_INPUT;
        break;
    }
    dyn_gk_csr_free(&a);
    dyn_gk_csr_free(&b);
    return rc;
}

static uint64_t gx_work(const gx_job_t* j)
{
    uint64_t nm = (uint64_t)j->g.n + j->g.m;
    switch (j->op) {
    case GX_PAGERANK:
        return nm * 16;
    case GX_TOPIC:
        return nm * 16 * (j->ntopic ? j->ntopic : 1);
    case GX_BETWEENNESS:
        return nm * (j->cnt[0] ? j->cnt[0] : 1);
    case GX_CLOSENESS:
        return nm * (j->g.n ? j->g.n : 1);
    case GX_LOUVAIN:
    case GX_LPA:
    case GX_TRIANGLES:
    case GX_CLUSTERING:
    case GX_MAXFLOW:
    case GX_MATCHING:
        return nm * 4;
    default:
        return nm;
    }
}

static JSValue gx_take(JSContext* ctx, void** pp, size_t nbytes, JSTypedArrayEnum type)
{
    JSValueConst args[3];
    JSValue ab, out;
    void* p = *pp;
    if (!p) {
        p = malloc(1);
        if (!p)
            return JS_ThrowOutOfMemory(ctx);
    }
    *pp = NULL;
    ab = dyn_nat_ab_take(ctx, p, nbytes);
    if (JS_IsException(ab))
        return ab;
    args[0] = ab;
    args[1] = JS_UNDEFINED;
    args[2] = JS_UNDEFINED;
    out = JS_NewTypedArray(ctx, 3, args, type);
    JS_FreeValue(ctx, ab);
    return out;
}

static JSValue gx_obj(JSContext* ctx, const char* const* keys, JSValue* vals, int nv)
{
    JSValue o;
    int i;
    for (i = 0; i < nv; i++)
        if (JS_IsException(vals[i]))
            goto fail;
    o = JS_NewObject(ctx);
    if (JS_IsException(o))
        goto fail;
    for (i = 0; i < nv; i++) {
        if (JS_DefinePropertyValueStr(ctx, o, keys[i], vals[i], JS_PROP_C_W_E) < 0) {
            int k;
            for (k = i + 1; k < nv; k++)
                JS_FreeValue(ctx, vals[k]);
            JS_FreeValue(ctx, o);
            return JS_EXCEPTION;
        }
    }
    return o;
fail:
    for (i = 0; i < nv; i++)
        JS_FreeValue(ctx, vals[i]);
    return JS_EXCEPTION;
}

static JSValue gx_throw(JSContext* ctx, const gx_job_t* j)
{
    const char* op = gx_names[j->op];
    switch (j->rc) {
    case DYN_GK_E_OOM:
        return JS_ThrowOutOfMemory(ctx);
    case DYN_GK_E_NEG_WEIGHT:
        return JS_ThrowRangeError(ctx, "%s: %s edge weight", op, j->op == GX_BETWEENNESS ? "non-positive" : "negative");
    case DYN_GK_E_NO_CONVERGE:
        if (j->op == GX_TOPIC)
            return JS_ThrowRangeError(ctx, "%s: topic %u: no convergence within %u iterations", op, j->bad_topic, j->rcount);
        return JS_ThrowRangeError(ctx, "%s: no convergence within %u iterations", op, j->rcount);
    case DYN_GK_E_CYCLE:
        return JS_ThrowRangeError(ctx, "%s: graph has a cycle", op);
    case DYN_GK_E_NOT_BIPARTITE:
        return JS_ThrowRangeError(ctx, "%s: the graph is not bipartite%s", op, j->bytes ? " under options.left" : "");
    case DYN_GK_E_TOO_LARGE:
        return JS_ThrowRangeError(ctx, "%s: graph too large", op);
    case DYN_GK_E_CANCELLED:
        return JS_ThrowInternalError(ctx, "interrupted");
    default:
        return JS_ThrowRangeError(ctx, "%s: invalid input", op);
    }
}

static JSValue gx_build(JSContext* ctx, gx_job_t* j)
{
    size_t n = j->g.n;
    JSValue v[4];
    switch (j->op) {
    case GX_PAGERANK:
    case GX_BETWEENNESS:
    case GX_CLOSENESS:
        return gx_take(ctx, (void**)&j->outf, n * sizeof(double), JS_TYPED_ARRAY_FLOAT64);
    case GX_LINK:
        return gx_take(ctx, (void**)&j->outf, (size_t)j->nidx * sizeof(double), JS_TYPED_ARRAY_FLOAT64);
    case GX_TOPIC:
        return gx_take(ctx, (void**)&j->outf, n * j->ntopic * sizeof(double), JS_TYPED_ARRAY_FLOAT64);
    case GX_LPA:
    case GX_CORES:
        return gx_take(ctx, (void**)&j->outi, n * sizeof(int32_t), JS_TYPED_ARRAY_INT32);
    case GX_MODULARITY:
        return JS_NewFloat64(ctx, j->res[0]);
    case GX_LOUVAIN: {
        static const char* const k[] = { "communities", "count", "modularity", "levels" };
        v[0] = gx_take(ctx, (void**)&j->outi, n * sizeof(int32_t), JS_TYPED_ARRAY_INT32);
        v[1] = JS_NewInt64(ctx, j->rcount);
        v[2] = JS_NewFloat64(ctx, j->res[0]);
        v[3] = JS_NewInt64(ctx, (int64_t)j->res[1]);
        return gx_obj(ctx, k, v, 4);
    }
    case GX_TRIANGLES: {
        static const char* const k[] = { "total", "perVertex" };
        v[0] = JS_NewFloat64(ctx, j->res[0]);
        v[1] = gx_take(ctx, (void**)&j->outf, n * sizeof(double), JS_TYPED_ARRAY_FLOAT64);
        return gx_obj(ctx, k, v, 2);
    }
    case GX_CLUSTERING: {
        static const char* const k[] = { "local", "average", "transitivity" };
        v[0] = gx_take(ctx, (void**)&j->outf, n * sizeof(double), JS_TYPED_ARRAY_FLOAT64);
        v[1] = JS_NewFloat64(ctx, j->res[0]);
        v[2] = JS_NewFloat64(ctx, j->res[1]);
        return gx_obj(ctx, k, v, 3);
    }
    case GX_SCC:
    case GX_WCC:
    case GX_LEVELS: {
        static const char* const k[] = { "count", "labels" };
        v[0] = JS_NewInt64(ctx, j->rcount);
        v[1] = gx_take(ctx, (void**)&j->outi, n * sizeof(int32_t), JS_TYPED_ARRAY_INT32);
        return gx_obj(ctx, k, v, 2);
    }
    case GX_MAXFLOW: {
        static const char* const k[] = { "value", "flows", "sourceSide" };
        v[0] = JS_NewFloat64(ctx, j->res[0]);
        v[1] = gx_take(ctx, (void**)&j->outf, (size_t)j->g.m * sizeof(double), JS_TYPED_ARRAY_FLOAT64);
        v[2] = gx_take(ctx, (void**)&j->outb, n, JS_TYPED_ARRAY_UINT8);
        return gx_obj(ctx, k, v, 3);
    }
    case GX_MATCHING: {
        static const char* const k[] = { "size", "mate" };
        v[0] = JS_NewInt64(ctx, j->rcount);
        v[1] = gx_take(ctx, (void**)&j->outi, n * sizeof(int32_t), JS_TYPED_ARRAY_INT32);
        return gx_obj(ctx, k, v, 2);
    }
    default:
        return JS_UNDEFINED;
    }
}

static int gx_snapshot(JSContext* ctx, gx_job_t* j)
{
    if (j->method) {
        dyn_graph_t* g = graph_of(ctx, j->this_val);
        int rc;
        if (!g)
            return -1;
        if ((j->op == GX_LEVELS) && !g->directed) {
            JS_ThrowTypeError(ctx, "%s requires a directed graph", gx_names[j->op]);
            return -1;
        }
        j->directed = g->directed;
        j->use_w = g->weighted;
        rc = dyn_gk_csr_of_graph(g, g->weighted, &j->g);
        if (rc == DYN_GK_E_TOO_LARGE) {
            JS_ThrowRangeError(ctx, "%s: graph too large", gx_names[j->op]);
            return -1;
        }
        if (rc < 0) {
            JS_ThrowOutOfMemory(ctx);
            return -1;
        }
        return 0;
    }
    if (gx_csr_arg(ctx, j, j->argc > 0 ? j->argv[0] : JS_UNDEFINED) < 0)
        return -1;
    if (j->op == GX_LEVELS && !j->directed) {
        JS_ThrowTypeError(ctx, "%s requires a directed CSR", gx_names[j->op]);
        return -1;
    }
    return 0;
}

static JSValue gx_entry(JSContext* ctx, void* arg)
{
    gx_job_t* j = (gx_job_t*)arg;
    JSValue res = JS_EXCEPTION;
    if (j->op == GX_TOPIC ? gx_topic_prepare(ctx, j) == 0 : (gx_snapshot(ctx, j) == 0 && gx_parse_opts(ctx, j) == 0)) {
        if (gx_work(j) >= GX_OFFLOAD_MIN)
            dyn_fiber_offload();
        j->rc = gx_run(j);
        dyn_fiber_onjs();
        res = j->rc < 0 ? gx_throw(ctx, j) : gx_build(ctx, j);
    }
    gx_job_free(ctx, j);
    return res;
}

static JSValue gx_start(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int op, int method)
{
    gx_job_t* j = (gx_job_t*)calloc(1, sizeof(*j));
    JSValue p;
    int i;
    if (!j)
        return JS_ThrowOutOfMemory(ctx);
    j->op = op;
    j->method = method;
    j->this_val = JS_DupValue(ctx, this_val);
    j->argc = argc < (int)countof(j->argv) ? argc : (int)countof(j->argv);
    for (i = 0; i < j->argc; i++)
        j->argv[i] = JS_DupValue(ctx, argv[i]);
    p = dyn_fiber_start(ctx, gx_entry, j);
    if (JS_IsException(p))
        gx_job_free(ctx, j);
    return p;
}

JSValue dyn_graph_algo(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    return gx_start(ctx, this_val, argc, argv, magic, 1);
}

static JSValue gx_free_fn(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    (void)this_val;
    return gx_start(ctx, JS_UNDEFINED, argc, argv, magic, 0);
}

static const JSCFunctionListEntry gx_funcs[] = {
    JS_CFUNC_MAGIC_DEF("pageRank", 2, gx_free_fn, GX_PAGERANK),
    JS_CFUNC_MAGIC_DEF("labelPropagation", 2, gx_free_fn, GX_LPA),
    JS_CFUNC_MAGIC_DEF("louvain", 2, gx_free_fn, GX_LOUVAIN),
    JS_CFUNC_MAGIC_DEF("modularity", 3, gx_free_fn, GX_MODULARITY),
    JS_CFUNC_MAGIC_DEF("coreNumbers", 1, gx_free_fn, GX_CORES),
    JS_CFUNC_MAGIC_DEF("triangles", 1, gx_free_fn, GX_TRIANGLES),
    JS_CFUNC_MAGIC_DEF("clustering", 1, gx_free_fn, GX_CLUSTERING),
    JS_CFUNC_MAGIC_DEF("betweenness", 2, gx_free_fn, GX_BETWEENNESS),
    JS_CFUNC_MAGIC_DEF("closeness", 2, gx_free_fn, GX_CLOSENESS),
    JS_CFUNC_MAGIC_DEF("stronglyConnectedComponents", 1, gx_free_fn, GX_SCC),
    JS_CFUNC_MAGIC_DEF("weaklyConnectedComponents", 1, gx_free_fn, GX_WCC),
    JS_CFUNC_MAGIC_DEF("topologicalLevels", 1, gx_free_fn, GX_LEVELS),
    JS_CFUNC_MAGIC_DEF("maxFlow", 3, gx_free_fn, GX_MAXFLOW),
    JS_CFUNC_MAGIC_DEF("bipartiteMatching", 2, gx_free_fn, GX_MATCHING),
    JS_CFUNC_MAGIC_DEF("linkPrediction", 3, gx_free_fn, GX_LINK),
};

static JSClassID gx_capture_cid(JSContext* ctx, JSTypedArrayEnum t)
{
    JSValueConst a[1];
    JSValue zero = JS_NewInt32(ctx, 0), ta;
    JSClassID id;
    a[0] = zero;
    ta = JS_NewTypedArray(ctx, 1, a, t);
    if (JS_IsException(ta))
        return 0;
    id = JS_GetClassID(ta);
    JS_FreeValue(ctx, ta);
    return id;
}

int dyn_graph_algo_register(JSContext* ctx, JSModuleDef* m)
{
    JSValue ns;
    gx_u32_cid = gx_capture_cid(ctx, JS_TYPED_ARRAY_UINT32);
    gx_f64_cid = gx_capture_cid(ctx, JS_TYPED_ARRAY_FLOAT64);
    gx_u8_cid = gx_capture_cid(ctx, JS_TYPED_ARRAY_UINT8);
    if (!gx_u32_cid || !gx_f64_cid || !gx_u8_cid)
        return -1;
    ns = JS_NewObject(ctx);
    if (JS_IsException(ns))
        return -1;
    if (JS_SetPropertyFunctionList(ctx, ns, gx_funcs, countof(gx_funcs)) < 0) {
        JS_FreeValue(ctx, ns);
        return -1;
    }
    return JS_SetModuleExport(ctx, m, "graph", ns);
}

#endif
