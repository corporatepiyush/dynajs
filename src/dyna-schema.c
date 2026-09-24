/* dyna:schema -- JSON Schema validator, Draft 2020-12 core keywords.
   Schema.compile(schema) builds a native node tree (patterns pre-compiled
   with unicode semantics, $ref resolved) so validate() is pure dispatch.
   Recursion is bounded at compile (schema depth, per-array list caps) and
   at run time (instance depth + a $ref indirection stack). Cuts: remote or
   anchored $ref, $dynamicRef (REFUSED at compile: silently ignoring an
   applicator would drop a constraint), format (annotation only per
   2020-12) and content* (annotation only). Implemented: type, enum/const,
   numeric/string/array/object bounds, uniqueItems, properties,
   patternProperties, additionalProperties, required, propertyNames,
   dependentRequired, dependentSchemas, contains/minContains/maxContains,
   allOf/anyOf/oneOf/not, if/then/else, local $ref/$defs, and
 unevaluatedProperties/unevaluatedItems (2020-12 ) via per-frame
   evaluation records: every check frame that can evaluate or consume
   annotations gets a fresh record; it merges into the CALLER's record only
   on success (a failed subschema's annotations vanish with it; child
   instance descents never merge), which yields the corpus-pinned
   propagation rules -- allOf/$ref/if/then/else/dependentSchemas and
   PASSING anyOf/oneOf branches feed the holder's unevaluated*, sibling
   branches ("cousins") never see each other, failed evaluations leave no
   trace, and contains/prefixItems/items successes annotate evaluated
   indices. Records live on the C stack, own at most
   DYN_SC_MAX_EVAL names/indices, and are gated by a compile-time
   SC_F_UREACH bit so schemas without unevaluated* pay nothing. */
#include "dyna-nat.h"
#include "core/dyn-pct.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_SCHEMA)

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "libregexp.h"

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

/* ---------------------------------------------------------------- limits */

#define DYN_SC_MAX_SCHEMA_DEPTH  256      /* compile: schema nesting cap */
#define DYN_SC_MAX_DEPTH         512      /* validate: instance recursion cap */
#define DYN_SC_MAX_REFCHAIN      64       /* validate: ref-chain guard cap */
#define DYN_SC_MAX_ERRORS        256      /* collected errors cap (bounded mem) */
#define DYN_SC_MAX_EQUAL_DEPTH   128      /* deep-equality recursion cap */
#define DYN_SC_MAX_LIST          (1<<20)  /* compile: elements per array keyword */
#define DYN_SC_MAX_PATTERN       (64*1024) /* compile: pattern source byte cap --
                                             lre_compile cost is superlinear-ish
                                             in the source, so a megabyte-scale
                                             "pattern" is a memory/CPU bomb */
#define DYN_SC_MAX_ARRAY         (1<<22)  /* validate: items loop element cap */
#define DYN_SC_MAX_UNIQUE        4096     /* validate: uniqueItems pairwise cap */
#define DYN_SC_MAX_EVAL          4096     /* validate: evaluated names/indices
                                             tracked per instance position --
                                             over that the validation FAILS
                                             CLOSED (RangeError), never with a
                                             guessed outcome */
#define DYN_SC_MULT_EPS          1e-9     /* multipleOf tolerance (ajv-style) */

/* -------------------------------------------------------------- type bits */

#define DYN_SC_T_NULL    1
#define DYN_SC_T_BOOL    2
#define DYN_SC_T_OBJECT  4
#define DYN_SC_T_ARRAY   8
#define DYN_SC_T_NUMBER  16
#define DYN_SC_T_STRING  32
#define DYN_SC_T_INTEGER 64

/* ------------------------------------------------------------- node tree */

/* Schema positions reachable by structural keywords are compiled once into a
   native tree. A $ref resolves to a SHARED node (never a copy), so recursive
   schemas compile without copying; the tree is a DAG. Every node is registered
   in a flat list owned by the schema -- free (iterate once; shared nodes are
   never freed twice) and gc_mark (mark the const/enum JSValues) walk it.
   Ownership rule for every array inside a node: js_mallocz the array and set
   its count IMMEDIATELY, so a mid-compile failure leaves the node in a state
   the flat-list teardown frees exactly once. */

typedef struct dyn_scnode dyn_scnode_t;

typedef struct { dyn_scnode_t **v; int n; } dyn_scsubs_t;

/* An ordered name -> schema map (properties, required). names are owned. */
typedef struct { char **names; dyn_scnode_t **v; int n; } dyn_scnamed_t;

/* One patternProperties entry: the pattern source, its compiled bytecode. */
typedef struct { char *pat; uint8_t *re; dyn_scnode_t *node; } dyn_scpat_t;

/* dependentRequired: trigger prop -> array of required prop names. */
typedef struct { char **props; char ***need; int *nneed; int n; } dyn_screq_t;

enum {
    SC_F_TYPE     = 1u << 0,  SC_F_CONST  = 1u << 1,  SC_F_ENUM   = 1u << 2,
    SC_F_MIN      = 1u << 3,  SC_F_MAX    = 1u << 4,  SC_F_EMIN   = 1u << 5,
    SC_F_EMAX     = 1u << 6,  SC_F_MULT   = 1u << 7,  SC_F_MINLEN = 1u << 8,
    SC_F_MAXLEN   = 1u << 9,  SC_F_PAT    = 1u << 10, SC_F_MINITEMS=1u << 11,
    SC_F_MAXITEMS = 1u << 12, SC_F_UNIQUE = 1u << 13, SC_F_MINPROPS=1u << 14,
    SC_F_MAXPROPS = 1u << 15, SC_F_REQ    = 1u << 16, SC_F_PROPS  = 1u << 17,
    SC_F_PATTS    = 1u << 18, SC_F_ADDL   = 1u << 19, SC_F_ITEMS  = 1u << 20,
    SC_F_PREFIX   = 1u << 21, SC_F_ALL    = 1u << 22, SC_F_ANY    = 1u << 23,
    SC_F_ONE      = 1u << 24, SC_F_NOT    = 1u << 25, SC_F_IF     = 1u << 26,
    SC_F_REF      = 1u << 27, SC_F_DEPREQ = 1u << 28, SC_F_CONT   = 1u << 29,
    SC_F_MINCONT  = 1u << 30, SC_F_MAXCONT= 1u << 31, SC_F_PNAMES = 1ull << 32,
    SC_F_DEPSCH   = 1ull << 33,
    SC_F_UNEVALP  = 1ull << 34, SC_F_UNEVALI = 1ull << 35,
    /* format: the name is stored when the schema carries a string
       "format"; the assertion itself runs only against the process-wide
       registry (registerFormat) and only for string instances. */
    SC_F_FMT      = 1ull << 37,
    /* Compile-time fixpoint bit: this node's in-place closure (self +
       allOf/dependentSchemas/then/else/$ref-target, transitively) contains
       an unevaluated* keyword, i.e. a check frame HERE may CONSUME an
       evaluation record. Frames reached from such a consumer also open
       records (to feed it), which the runtime gate `saved || UREACH` covers
       without a second bit. anyOf/oneOf/not/if branches are excluded from
       the closure: they open their own records and merge only on success. */
    SC_F_UREACH   = 1ull << 36,
};

enum { SC_SCHEMA = 0, SC_BOOL = 1 };

struct dyn_scnode {
    uint64_t f;                 /* SC_F_* presence flags */
    int kind;                   /* SC_SCHEMA / SC_BOOL */
    int idx;                    /* position in the owning schema's flat node
                                   list (stable node identity; used by the
                                   unevaluated* reach fixpoint) */
    int boolv;                  /* SC_BOOL: 1 = true schema, 0 = false */
    int type_mask;              /* DYN_SC_T_* or 0 */
    double dmin, dmax, dmult;   /* inclusive min/max, multipleOf */
    double demin, demax;        /* exclusive min/max (own fields: a schema
                                   may carry both a bound and its exclusive
                                   form, e.g. minimum:5 exclusiveMinimum:3) */
    int minlen, maxlen;         /* minLength/maxLength, -1 absent */
    int minitems, maxitems;     /* minItems/maxItems, -1 absent */
    int minprops, maxprops;     /* minProperties/maxProperties, -1 absent */
    int mincont, maxcont;       /* minContains/maxContains, -1 absent */
    int unique;                 /* uniqueItems value */
    JSValue val;                /* const value (owned, JS_UNDEFINED absent) */
    JSValue *enumv;             /* enum items (owned array) */
    int n_enum;
    uint8_t *re;                /* compiled pattern bytecode (owned) */
    char *ref;                  /* $ref source string (owned, for messages) */
    char *fmt;                  /* "format" name (owned); assertion runs only
 when the name is registered */
    dyn_scnode_t *ref_tgt;      /* resolved $ref target (shared) */
    dyn_scnamed_t props;        /* properties */
    dyn_scnamed_t depsch;       /* dependentSchemas: trigger name -> schema */
    dyn_scnode_t *pn;           /* propertyNames subschema */
    dyn_scnode_t *cont;         /* contains subschema */
    dyn_scpat_t *patts;         /* patternProperties */
    int n_patts;
    int addl_allow;             /* additionalProperties true (or absent) */
    dyn_scnode_t *addl;         /* additionalProperties subschema, or NULL */
    int unevalp_allow;          /* unevaluatedProperties true */
    dyn_scnode_t *unevalp;      /* unevaluatedProperties subschema, or NULL */
    int unevali_allow;          /* unevaluatedItems true */
    dyn_scnode_t *unevali;      /* unevaluatedItems subschema, or NULL */
    dyn_scnode_t *items;        /* items subschema */
    dyn_scsubs_t prefix;        /* prefixItems tuple */
    dyn_scnamed_t required;     /* required property names (v unused) */
    dyn_screq_t depreq;         /* dependentRequired */
    dyn_scsubs_t all, any, one; /* allOf / anyOf / oneOf */
    dyn_scnode_t *notn, *ifn, *then, *els;
};

/* The CompiledSchema native: the root node plus every node ever created (the
   DAG is freed/marked by iterating this flat list exactly once). */
typedef struct {
    dyn_scnode_t *root;
    dyn_scnode_t **nodes;
    int n_nodes, cap_nodes;
} dyn_schema_t;

static JSClassID dyn_schema_class_id;

/* ---------------------------------------------------------- compile state */

/* Phase 1 walks every schema position and records (JSON pointer -> node) in
   this index; phase 2 resolves every $ref by lookup. Recursion is bounded by
   c->depth. The index dies with the compile; the nodes live in the schema. */
typedef struct {
    JSContext *ctx;
    dyn_schema_t *sch;
    char **paths;
    dyn_scnode_t **pnode;
    int n_paths, cap_paths;
    int depth;
} dyn_sccomp_t;

static int dyn_sc_index_add(dyn_sccomp_t *c, const char *path, size_t plen,
                            dyn_scnode_t *n)
{
    char *p;
    if (c->n_paths >= c->cap_paths) {
        int ncap = c->cap_paths ? c->cap_paths * 2 : 64;
        char **np;
        dyn_scnode_t **nn;
        /* Commit the first realloc BEFORE the second: if the second fails we
           must not be left pointing at a block js_realloc already freed. */
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

static dyn_scnode_t *dyn_sc_index_find(dyn_sccomp_t *c, const char *path,
                                       size_t plen)
{
    int i;
    for (i = 0; i < c->n_paths; i++)
        if (strlen(c->paths[i]) == plen && memcmp(c->paths[i], path, plen) == 0)
            return c->pnode[i];
    return NULL;
}

static dyn_scnode_t *dyn_sc_new_node(dyn_sccomp_t *c)
{
    dyn_scnode_t *n = js_mallocz(c->ctx, sizeof(*n));
    if (!n)
        return NULL;
    n->val = JS_UNDEFINED;
    n->minlen = n->maxlen = -1;
    n->minitems = n->maxitems = -1;
    n->minprops = n->maxprops = -1;
    n->addl_allow = 1;
    if (c->sch->n_nodes >= c->sch->cap_nodes) {
        int ncap = c->sch->cap_nodes ? c->sch->cap_nodes * 2 : 32;
        dyn_scnode_t **nn = js_realloc(c->ctx, c->sch->nodes,
                                       (size_t)ncap * sizeof(*nn));
        if (!nn) {
            js_free(c->ctx, n);
            return NULL;
        }
        c->sch->nodes = nn;
        c->sch->cap_nodes = ncap;
    }
    c->sch->nodes[c->sch->n_nodes] = n;
    n->idx = c->sch->n_nodes;
    c->sch->n_nodes++;
    return n;
}

/* Read a keyword; *out owns the value or is JS_UNDEFINED when absent. */
static int dyn_sc_kw(JSContext *ctx, JSValueConst obj, const char *name,
                     JSValue *out)
{
    *out = JS_GetPropertyStr(ctx, obj, name);
    return JS_IsException(*out) ? -1 : 0;
}

static uint64_t dyn_sc_arr_len(JSContext *ctx, JSValue arr)
{
    JSValue lv = JS_GetPropertyStr(ctx, arr, "length");
    uint64_t len;
    if (JS_ToIndex(ctx, &len, lv)) {
        JS_FreeValue(ctx, lv);
        return (uint64_t)-1;
    }
    JS_FreeValue(ctx, lv);
    return len;
}

/* JS_IsArray resolves proxies and THROWS for a revoked one (and for a >1000
   proxy chain). A proxy is not JSON data: clear the exception and report
   "not an array", so a pending exception never poisons the next engine call. */
static int dyn_sc_is_array(JSContext *ctx, JSValueConst v)
{
    int r = JS_IsArray(ctx, v);
    if (r < 0) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return 0;
    }
    return r;
}

/* RFC 6901 pointer of a child position: parent + "/" + escaped(token). */
static char *dyn_sc_child_path(JSContext *ctx, const char *path, size_t plen,
                               const char *tok, size_t tlen, size_t *olen)
{
    size_t cap = plen + 1 + tlen * 2 + 1, i, o;
    char *buf = js_malloc(ctx, cap);
    if (!buf)
        return NULL;
    memcpy(buf, path, plen);
    o = plen;
    buf[o++] = '/';
    for (i = 0; i < tlen; i++) {
        if (tok[i] == '~') { buf[o++] = '~'; buf[o++] = '0'; }
        else if (tok[i] == '/') { buf[o++] = '~'; buf[o++] = '1'; }
        else buf[o++] = tok[i];
    }
    buf[o] = '\0';
    *olen = o;
    return buf;
}

/* Forward declaration for recursive child compilation */
static int dyn_sc_compile(dyn_sccomp_t *c, JSValueConst sv, const char *path,
                          size_t plen, dyn_scnode_t **out);

/* Compile a subschema at a child position (allocates the child path). */
static int dyn_sc_compile_sub(dyn_sccomp_t *c, JSValueConst v,
                              const char *path, size_t plen,
                              const char *tok, size_t tlen,
                              dyn_scnode_t **out)
{
    char *cp;
    size_t cplen;
    int r;
    cp = dyn_sc_child_path(c->ctx, path, plen, tok, tlen, &cplen);
    if (!cp)
        return -1;
    r = dyn_sc_compile(c, v, cp, cplen, out);
    js_free(c->ctx, cp);
    return r;
}

/* Compile a subschema at <path>/<keyword>/<token>. Applicator members are
   indexed under their keyword segment -- /properties/foo, /prefixItems/0,
   /allOf/1 -- because a same-document $ref is an RFC 6901 pointer into the
 schema DOCUMENT (2020-12 evaluation paths), so "#/properties/foo"
   must land on the properties.foo subschema. Registering them at the bare
   /<name> or /<index> made exactly those refs unresolvable (audit M24-02).
   The token is ~0/~1-escaped by dyn_sc_child_path as everywhere else. */
static int dyn_sc_compile_at(dyn_sccomp_t *c, JSValueConst v,
                             const char *path, size_t plen,
                             const char *kwd, const char *tok, size_t tlen,
                             dyn_scnode_t **out)
{
    char *kp;
    size_t kl;
    int r;
    kp = dyn_sc_child_path(c->ctx, path, plen, kwd, strlen(kwd), &kl);
    if (!kp)
        return -1;
    r = dyn_sc_compile_sub(c, v, kp, kl, tok, tlen, out);
    js_free(c->ctx, kp);
    return r;
}

/* ------------------------------------------------------ keyword readers */

static int dyn_sc_type_name(const char *s, size_t n, int *mask)
{
    static const struct { const char *name; int bit; } T[] = {
        { "null", DYN_SC_T_NULL }, { "boolean", DYN_SC_T_BOOL },
        { "object", DYN_SC_T_OBJECT }, { "array", DYN_SC_T_ARRAY },
        { "number", DYN_SC_T_NUMBER }, { "string", DYN_SC_T_STRING },
        { "integer", DYN_SC_T_INTEGER },
    };
    int i;
    for (i = 0; i < (int)countof(T); i++)
        if (strlen(T[i].name) == n && memcmp(T[i].name, s, n) == 0) {
            *mask = T[i].bit;
            return 0;
        }
    return -1;
}

/* "type": a type name or an array of type names. */
static int dyn_sc_parse_type(JSContext *ctx, JSValueConst v, int *mask)
{
    int m = 0;
    if (JS_IsString(v)) {
        size_t n;
        const char *s = JS_ToCStringLen(ctx, &n, v);
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
            const char *s;
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
            if (!s) { JS_FreeValue(ctx, el); return -1; }
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

/* Numeric keywords accept real numbers only: JSON Schema requires a NUMBER,
 * and silently coercing "5" (or true -> 1) would validate a mistyped schema
 * document instead of refusing it. */
static int dyn_sc_read_num(JSContext *ctx, JSValueConst v, const char *kw,
                           double *out)
{
    if (!JS_IsNumber(v) || JS_ToFloat64(ctx, out, v)) {
        JS_ThrowTypeError(ctx, "dyna:schema: \"%s\" must be a number", kw);
        return -1;
    }
    return 0;
}

static int dyn_sc_read_int(JSContext *ctx, JSValueConst v, const char *kw,
                           int *out)
{
    double d;
    if (!JS_IsNumber(v) || JS_ToFloat64(ctx, &d, v) || d < 0 || d != trunc(d) || d > 2147483647.0 || isnan(d) || isinf(d)) {
        JS_ThrowTypeError(ctx,
            "dyna:schema: \"%s\" must be a non-negative integer", kw);
        return -1;
    }
    *out = (int)d;
    return 0;
}

/* Patterns are compiled with ECMA-262 unicode semantics (the dialect the
   2020-12 spec references; ajv does the same). lre_compile NEEDS a non-NULL
   plen: its error path writes *plen = 0. */
static int dyn_sc_compile_re(JSContext *ctx, const char *s, size_t n,
                             uint8_t **bc)
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

/* Read an object-valued keyword's own enumerable string keys into
   *names (owned strings) / *vals (owned), count n. */
static int dyn_sc_read_map(JSContext *ctx, JSValueConst v, const char *kw,
                           char ***pnames, JSValue **pvals, int *pn)
{
    JSPropertyEnum *tab = NULL;
    uint32_t n, i;
    char **names;
    JSValue *vals;

    if (!JS_IsObject(v) || dyn_sc_is_array(ctx, v)) {
        JS_ThrowTypeError(ctx, "dyna:schema: \"%s\" must be an object", kw);
        return -1;
    }
    if (JS_GetOwnPropertyNames(ctx, &tab, &n, v,
                               JS_GPN_ENUM_ONLY | JS_GPN_STRING_MASK) < 0)
        return -1;
    names = js_mallocz(ctx, (n ? n : 1) * sizeof(*names));
    vals = js_mallocz(ctx, (n ? n : 1) * sizeof(*vals));
    if (!names || !vals) {
        JS_FreePropertyEnum(ctx, tab, n);
        js_free(ctx, names);
        js_free(ctx, vals);
        return -1;
    }
    for (i = 0; i < n; i++) {
        JSValue name = JS_AtomToString(ctx, tab[i].atom);
        const char *nb;
        size_t nl;
        if (JS_IsException(name)) goto fail;
        nb = JS_ToCStringLen(ctx, &nl, name);
        if (!nb) { JS_FreeValue(ctx, name); goto fail; }
        names[i] = js_malloc(ctx, nl + 1);
        if (!names[i]) { JS_FreeCString(ctx, nb); JS_FreeValue(ctx, name);
                         goto fail; }
        memcpy(names[i], nb, nl);
        names[i][nl] = '\0';
        JS_FreeCString(ctx, nb);
        JS_FreeValue(ctx, name);
        /* JS_GetProperty returns an OWNED value: never dup it. */
        vals[i] = JS_GetProperty(ctx, v, tab[i].atom);
        if (JS_IsException(vals[i]))
            goto fail;
    }
    JS_FreePropertyEnum(ctx, tab, n);
    *pnames = names; *pvals = vals; *pn = (int)n;
    return 0;
 fail:
    JS_FreePropertyEnum(ctx, tab, n);
    for (i = 0; i < n; i++) {
        js_free(ctx, names[i]);       /* NULL-safe */
        JS_FreeValue(ctx, vals[i]);   /* JS_UNDEFINED free is a no-op */
    }
    js_free(ctx, names);
    js_free(ctx, vals);
    return -1;
}

/* --------------------------------------------------- phase 1: the walk */

static int dyn_sc_compile(dyn_sccomp_t *c, JSValueConst sv, const char *path,
                          size_t plen, dyn_scnode_t **out);
static int dyn_sc_compile_kw(dyn_sccomp_t *c, JSValueConst sv,
                             dyn_scnode_t *n, const char *path, size_t plen);

/* registry lookup (defined near the module glue) */
static JSValue dyn_sc_fmt_lookup(JSContext *ctx, const char *name);

static int dyn_sc_compile(dyn_sccomp_t *c, JSValueConst sv, const char *path,
                          size_t plen, dyn_scnode_t **out)
{
    JSContext *ctx = c->ctx;
    dyn_scnode_t *n;

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

    c->depth++;
    if (dyn_sc_compile_kw(c, sv, n, path, plen)) {
        c->depth--;
        return -1;
    }
    c->depth--;
    *out = n;
    return 0;
}

/* Bounds check shared by every compile-time array keyword. */
static int dyn_sc_check_list(JSContext *ctx, JSValueConst v, const char *kw,
                             uint64_t *len)
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

static int dyn_sc_compile_kw(dyn_sccomp_t *c, JSValueConst sv,
                             dyn_scnode_t *n, const char *path, size_t plen)
{
    JSContext *ctx = c->ctx;
    JSValue kw;
    int r;

    /* ---- type --------------------------------------------------------- */
    if (dyn_sc_kw(ctx, sv, "type", &kw)) return -1;
    if (!JS_IsUndefined(kw)) {
        if (dyn_sc_parse_type(ctx, kw, &n->type_mask)) {
            JS_FreeValue(ctx, kw);
            return -1;
        }
        n->f |= SC_F_TYPE;
    }
    JS_FreeValue(ctx, kw);

    /* ---- const / enum (deep equality against JSValues, owned) --------- */
    if (dyn_sc_kw(ctx, sv, "const", &kw)) return -1;
    if (!JS_IsUndefined(kw)) {
        n->val = JS_DupValue(ctx, kw);
        n->f |= SC_F_CONST;
    }
    JS_FreeValue(ctx, kw);

    if (dyn_sc_kw(ctx, sv, "enum", &kw)) return -1;
    if (!JS_IsUndefined(kw)) {
        uint64_t len, i;
        if (dyn_sc_check_list(ctx, kw, "enum", &len)) {
            JS_FreeValue(ctx, kw);
            return -1;
        }
        n->enumv = js_malloc(ctx, (len ? len : 1) * sizeof(JSValue));
        if (!n->enumv) { JS_FreeValue(ctx, kw); return -1; }
        for (i = 0; i < len; i++) {
            /* owned value, no dup */
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

    /* ---- numeric bounds (each bound gets its OWN field) ---------------- */
    if (dyn_sc_kw(ctx, sv, "minimum", &kw)) return -1;
    if (!JS_IsUndefined(kw)) {
        if (dyn_sc_read_num(ctx, kw, "minimum", &n->dmin)) {
            JS_FreeValue(ctx, kw);
            return -1;
        }
        n->f |= SC_F_MIN;
    }
    JS_FreeValue(ctx, kw);

    if (dyn_sc_kw(ctx, sv, "maximum", &kw)) return -1;
    if (!JS_IsUndefined(kw)) {
        if (dyn_sc_read_num(ctx, kw, "maximum", &n->dmax)) {
            JS_FreeValue(ctx, kw);
            return -1;
        }
        n->f |= SC_F_MAX;
    }
    JS_FreeValue(ctx, kw);

    if (dyn_sc_kw(ctx, sv, "exclusiveMinimum", &kw)) return -1;
    if (!JS_IsUndefined(kw)) {
        if (dyn_sc_read_num(ctx, kw, "exclusiveMinimum", &n->demin)) {
            JS_FreeValue(ctx, kw);
            return -1;
        }
        n->f |= SC_F_EMIN;
    }
    JS_FreeValue(ctx, kw);

    if (dyn_sc_kw(ctx, sv, "exclusiveMaximum", &kw)) return -1;
    if (!JS_IsUndefined(kw)) {
        if (dyn_sc_read_num(ctx, kw, "exclusiveMaximum", &n->demax)) {
            JS_FreeValue(ctx, kw);
            return -1;
        }
        n->f |= SC_F_EMAX;
    }
    JS_FreeValue(ctx, kw);

    if (dyn_sc_kw(ctx, sv, "multipleOf", &kw)) return -1;
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

    /* ---- string bounds ------------------------------------------------ */
    if (dyn_sc_kw(ctx, sv, "minLength", &kw)) return -1;
    if (!JS_IsUndefined(kw)) {
        if (dyn_sc_read_int(ctx, kw, "minLength", &n->minlen)) {
            JS_FreeValue(ctx, kw);
            return -1;
        }
        n->f |= SC_F_MINLEN;
    }
    JS_FreeValue(ctx, kw);

    if (dyn_sc_kw(ctx, sv, "maxLength", &kw)) return -1;
    if (!JS_IsUndefined(kw)) {
        if (dyn_sc_read_int(ctx, kw, "maxLength", &n->maxlen)) {
            JS_FreeValue(ctx, kw);
            return -1;
        }
        n->f |= SC_F_MAXLEN;
    }
    JS_FreeValue(ctx, kw);

    if (dyn_sc_kw(ctx, sv, "pattern", &kw)) return -1;
    if (!JS_IsUndefined(kw)) {
        size_t nl;
        const char *ps;
        if (!JS_IsString(kw)) {
            JS_ThrowTypeError(ctx, "dyna:schema: \"pattern\" must be a string");
            JS_FreeValue(ctx, kw);
            return -1;
        }
        ps = JS_ToCStringLen(ctx, &nl, kw);
        if (!ps) { JS_FreeValue(ctx, kw); return -1; }
        r = dyn_sc_compile_re(ctx, ps, nl, &n->re);
        JS_FreeCString(ctx, ps);
        JS_FreeValue(ctx, kw);
        if (r)
            return -1;
        n->f |= SC_F_PAT;
    }

    /* ---- format --------------------------------------------------
     * Stored as a name. Per Draft 2020-12 a "format" is annotation-only
     * unless a vocabulary makes it assertive: here it asserts ONLY against
     * the process-wide registerFormat() registry (and only for string
     * instances), so unregistered names keep their annotation-only behavior
     * and non-string annotation values stay legal and are ignored. */
    if (dyn_sc_kw(ctx, sv, "format", &kw)) return -1;
    if (!JS_IsUndefined(kw) && JS_IsString(kw) && !(n->f & SC_F_FMT)) {
        const char *fs;
        size_t fl;
        fs = JS_ToCStringLen(ctx, &fl, kw);
        if (!fs) { JS_FreeValue(ctx, kw); return -1; }
        if (fl > 0 && strlen(fs) == fl) {         /* ignore junk annotations */
            n->fmt = (char *)js_malloc(ctx, fl + 1);
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

    /* ---- array bounds -------------------------------------------------- */
    if (dyn_sc_kw(ctx, sv, "minItems", &kw)) return -1;
    if (!JS_IsUndefined(kw)) {
        if (dyn_sc_read_int(ctx, kw, "minItems", &n->minitems)) {
            JS_FreeValue(ctx, kw);
            return -1;
        }
        n->f |= SC_F_MINITEMS;
    }
    JS_FreeValue(ctx, kw);

    if (dyn_sc_kw(ctx, sv, "maxItems", &kw)) return -1;
    if (!JS_IsUndefined(kw)) {
        if (dyn_sc_read_int(ctx, kw, "maxItems", &n->maxitems)) {
            JS_FreeValue(ctx, kw);
            return -1;
        }
        n->f |= SC_F_MAXITEMS;
    }
    JS_FreeValue(ctx, kw);

    if (dyn_sc_kw(ctx, sv, "uniqueItems", &kw)) return -1;
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

    /* ---- object bounds ------------------------------------------------ */
    if (dyn_sc_kw(ctx, sv, "minProperties", &kw)) return -1;
    if (!JS_IsUndefined(kw)) {
        if (dyn_sc_read_int(ctx, kw, "minProperties", &n->minprops)) {
            JS_FreeValue(ctx, kw);
            return -1;
        }
        n->f |= SC_F_MINPROPS;
    }
    JS_FreeValue(ctx, kw);

    if (dyn_sc_kw(ctx, sv, "maxProperties", &kw)) return -1;
    if (!JS_IsUndefined(kw)) {
        if (dyn_sc_read_int(ctx, kw, "maxProperties", &n->maxprops)) {
            JS_FreeValue(ctx, kw);
            return -1;
        }
        n->f |= SC_F_MAXPROPS;
    }
    JS_FreeValue(ctx, kw);

    /* ---- required ------------------------------------------------------ */
    if (dyn_sc_kw(ctx, sv, "required", &kw)) return -1;
    if (!JS_IsUndefined(kw)) {
        uint64_t len, i;
        if (dyn_sc_check_list(ctx, kw, "required", &len)) {
            JS_FreeValue(ctx, kw);
            return -1;
        }
        n->required.names = js_mallocz(ctx, (len ? len : 1) * sizeof(char *));
        n->required.v = js_mallocz(ctx, (len ? len : 1) * sizeof(dyn_scnode_t *));
        if (!n->required.names || !n->required.v) {
            js_free(ctx, n->required.names);
            js_free(ctx, n->required.v);
            n->required.names = NULL;
            n->required.v = NULL;
            JS_FreeValue(ctx, kw);
            return -1;
        }
        n->required.n = (int)len;   /* early: teardown owns names[0..n) */
        for (i = 0; i < len; i++) {
            JSValue el = JS_GetPropertyUint32(ctx, kw, (uint32_t)i);
            size_t nl;
            const char *nb;
            if (JS_IsException(el)) { JS_FreeValue(ctx, kw); return -1; }
            if (!JS_IsString(el)) {
                JS_ThrowTypeError(ctx,
                    "dyna:schema: every element of \"required\" must be a string");
                JS_FreeValue(ctx, el);
                JS_FreeValue(ctx, kw);
                return -1;
            }
            nb = JS_ToCStringLen(ctx, &nl, el);
            if (!nb) { JS_FreeValue(ctx, el); JS_FreeValue(ctx, kw);
                       return -1; }
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

    /* ---- properties ---------------------------------------------------- */
    if (dyn_sc_kw(ctx, sv, "properties", &kw)) return -1;
    if (!JS_IsUndefined(kw)) {
        char **names;
        JSValue *vals;
        int i, nk;
        if (dyn_sc_read_map(ctx, kw, "properties", &names, &vals, &nk)) {
            JS_FreeValue(ctx, kw);
            return -1;
        }
        n->props.names = js_mallocz(ctx, (nk ? nk : 1) * sizeof(char *));
        n->props.v = js_mallocz(ctx, (nk ? nk : 1) * sizeof(dyn_scnode_t *));
        if (!n->props.names || !n->props.v) {
            int j;
            for (j = 0; j < nk; j++) {
                js_free(ctx, names[j]);
                JS_FreeValue(ctx, vals[j]);
            }
            js_free(ctx, names); js_free(ctx, vals);
            js_free(ctx, n->props.names); js_free(ctx, n->props.v);
            n->props.names = NULL; n->props.v = NULL;
            JS_FreeValue(ctx, kw);
            return -1;
        }
        n->props.n = nk;            /* early: teardown owns names[0..nk) */
        for (i = 0; i < nk; i++) {
            dyn_scnode_t *sub;
            n->props.names[i] = names[i];   /* transfer the string */
            if (dyn_sc_compile_at(c, vals[i], path, plen, "properties",
                                  names[i], strlen(names[i]), &sub)) {
                int j;
                for (j = i; j < nk; j++)
                    JS_FreeValue(ctx, vals[j]);
                for (j = i + 1; j < nk; j++)    /* names[0..i] node-owned */
                    js_free(ctx, names[j]);
                js_free(ctx, names); js_free(ctx, vals);
                JS_FreeValue(ctx, kw);
                return -1;
            }
            n->props.v[i] = sub;
            JS_FreeValue(ctx, vals[i]);
        }
        js_free(ctx, names); js_free(ctx, vals);
        n->f |= SC_F_PROPS;
    }
    JS_FreeValue(ctx, kw);

    /* ---- patternProperties -------------------------------------------- */
    if (dyn_sc_kw(ctx, sv, "patternProperties", &kw)) return -1;
    if (!JS_IsUndefined(kw)) {
        char **names;
        JSValue *vals;
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
            js_free(ctx, names); js_free(ctx, vals);
            JS_FreeValue(ctx, kw);
            return -1;
        }
        n->n_patts = nk;            /* early: teardown owns patts[0..nk) */
        for (i = 0; i < nk; i++) {
            size_t nl = strlen(names[i]);
            dyn_scnode_t *sub;
            if (dyn_sc_compile_re(ctx, names[i], nl, &n->patts[i].re)) {
                int j;
                for (j = i; j < nk; j++) {
                    js_free(ctx, names[j]);
                    JS_FreeValue(ctx, vals[j]);
                }
                js_free(ctx, names); js_free(ctx, vals);
                JS_FreeValue(ctx, kw);
                return -1;
            }
            n->patts[i].pat = names[i];     /* transfer */
            if (dyn_sc_compile_at(c, vals[i], path, plen,
                                  "patternProperties", names[i], nl,
                                  &sub)) {
                int j;
                for (j = i + 1; j < nk; j++)    /* names[i] now node-owned */
                    js_free(ctx, names[j]);
                for (j = i; j < nk; j++)
                    JS_FreeValue(ctx, vals[j]);
                js_free(ctx, names); js_free(ctx, vals);
                JS_FreeValue(ctx, kw);
                return -1;
            }
            n->patts[i].node = sub;
            JS_FreeValue(ctx, vals[i]);
        }
        js_free(ctx, names); js_free(ctx, vals);
        n->f |= SC_F_PATTS;
    }
    JS_FreeValue(ctx, kw);

    /* ---- additionalProperties ----------------------------------------- */
    if (dyn_sc_kw(ctx, sv, "additionalProperties", &kw)) return -1;
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

    /* ---- unevaluatedProperties / unevaluatedItems (2020-12 ) ----- */
    if (dyn_sc_kw(ctx, sv, "unevaluatedProperties", &kw)) return -1;
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

    if (dyn_sc_kw(ctx, sv, "unevaluatedItems", &kw)) return -1;
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

    /* ---- prefixItems / items ------------------------------------------ */
    if (dyn_sc_kw(ctx, sv, "prefixItems", &kw)) return -1;
    if (!JS_IsUndefined(kw)) {
        uint64_t len, i;
        if (dyn_sc_check_list(ctx, kw, "prefixItems", &len)) {
            JS_FreeValue(ctx, kw);
            return -1;
        }
        n->prefix.v = js_mallocz(ctx, (len ? len : 1) * sizeof(dyn_scnode_t *));
        if (!n->prefix.v) { JS_FreeValue(ctx, kw); return -1; }
        n->prefix.n = (int)len;     /* early */
        for (i = 0; i < len; i++) {
            char idx[24];
            int il = snprintf(idx, sizeof idx, "%llu",
                              (unsigned long long)i);
            JSValue el = JS_GetPropertyUint32(ctx, kw, (uint32_t)i);
            dyn_scnode_t *sub;
            if (JS_IsException(el)) { JS_FreeValue(ctx, kw); return -1; }
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

    if (dyn_sc_kw(ctx, sv, "items", &kw)) return -1;
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

    /* ---- contains / minContains / maxContains -------------------------- */
    if (dyn_sc_kw(ctx, sv, "contains", &kw)) return -1;
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

    /* min/maxContains apply only alongside contains; without it they are
       recorded (so a lone minContains still round-trips its presence) but
 validation ignores them, per 2020-12 ("only applies in the
       presence of contains" is the observable behavior the suite pins). */
    if (dyn_sc_kw(ctx, sv, "minContains", &kw)) return -1;
    if (!JS_IsUndefined(kw)) {
        if (dyn_sc_read_int(ctx, kw, "minContains", &n->mincont)) {
            JS_FreeValue(ctx, kw);
            return -1;
        }
        n->f |= SC_F_MINCONT;
    }
    JS_FreeValue(ctx, kw);

    if (dyn_sc_kw(ctx, sv, "maxContains", &kw)) return -1;
    if (!JS_IsUndefined(kw)) {
        if (dyn_sc_read_int(ctx, kw, "maxContains", &n->maxcont)) {
            JS_FreeValue(ctx, kw);
            return -1;
        }
        n->f |= SC_F_MAXCONT;
    }
    JS_FreeValue(ctx, kw);

    /* ---- dependentRequired -------------------------------------------- */
    if (dyn_sc_kw(ctx, sv, "dependentRequired", &kw)) return -1;
    if (!JS_IsUndefined(kw)) {
        char **names;
        JSValue *vals;
        int i, k, nk;
        if (dyn_sc_read_map(ctx, kw, "dependentRequired", &names, &vals,
                            &nk)) {
            JS_FreeValue(ctx, kw);
            return -1;
        }
        n->depreq.props = names;    /* whole array owned by the node */
        n->depreq.need = js_mallocz(ctx, (nk ? nk : 1) * sizeof(char **));
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
        n->depreq.n = nk;           /* early: teardown owns need[0..nk) */
        for (i = 0; i < nk; i++) {
            uint64_t m, j;
            char **need;
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
            need = js_mallocz(ctx, (m ? m : 1) * sizeof(char *));
            if (!need)
                goto dep_fail;
            for (j = 0; j < m; j++) {
                JSValue el = JS_GetPropertyUint32(ctx, vals[i], (uint32_t)j);
                size_t eln;
                const char *eb;
                if (JS_IsException(el)) {
                    for (t = 0; t < j; t++) js_free(ctx, need[t]);
                    js_free(ctx, need);
                    goto dep_fail;
                }
                if (!JS_IsString(el)) {
                    JS_ThrowTypeError(ctx,
                        "dyna:schema: each \"dependentRequired\" value must be "
                        "an array of strings");
                    JS_FreeValue(ctx, el);
                    for (t = 0; t < j; t++) js_free(ctx, need[t]);
                    js_free(ctx, need);
                    goto dep_fail;
                }
                eb = JS_ToCStringLen(ctx, &eln, el);
                if (!eb) {
                    JS_FreeValue(ctx, el);
                    for (t = 0; t < j; t++) js_free(ctx, need[t]);
                    js_free(ctx, need);
                    goto dep_fail;
                }
                need[j] = js_malloc(ctx, eln + 1);
                if (!need[j]) {
                    JS_FreeCString(ctx, eb);
                    JS_FreeValue(ctx, el);
                    for (t = 0; t < j; t++) js_free(ctx, need[t]);
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

    /* ---- propertyNames: validate each own property NAME ---------------- */
    if (dyn_sc_kw(ctx, sv, "propertyNames", &kw)) return -1;
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

    /* ---- dependentSchemas: if the trigger prop is present, apply the ---- */
    if (dyn_sc_kw(ctx, sv, "dependentSchemas", &kw)) return -1;
    if (!JS_IsUndefined(kw)) {
        char **names;
        JSValue *vals;
        int i, nk;
        if (dyn_sc_read_map(ctx, kw, "dependentSchemas", &names, &vals,
                            &nk)) {
            JS_FreeValue(ctx, kw);
            return -1;
        }
        n->depsch.names = js_mallocz(ctx, (nk ? nk : 1) * sizeof(char *));
        n->depsch.v = js_mallocz(ctx, (nk ? nk : 1) * sizeof(dyn_scnode_t *));
        if (!n->depsch.names || !n->depsch.v) {
            int j;
            for (j = 0; j < nk; j++) {
                js_free(ctx, names[j]);
                JS_FreeValue(ctx, vals[j]);
            }
            js_free(ctx, names); js_free(ctx, vals);
            js_free(ctx, n->depsch.names); js_free(ctx, n->depsch.v);
            n->depsch.names = NULL; n->depsch.v = NULL;
            JS_FreeValue(ctx, kw);
            return -1;
        }
        n->depsch.n = nk;           /* early: teardown owns names[0..nk) */
        for (i = 0; i < nk; i++) {
            dyn_scnode_t *sub;
            n->depsch.names[i] = names[i];  /* transfer the string */
            if (dyn_sc_compile_at(c, vals[i], path, plen, "dependentSchemas",
                                  names[i], strlen(names[i]), &sub)) {
                int j;
                for (j = i; j < nk; j++)
                    JS_FreeValue(ctx, vals[j]);
                for (j = i + 1; j < nk; j++)    /* names[0..i] node-owned */
                    js_free(ctx, names[j]);
                js_free(ctx, names); js_free(ctx, vals);
                JS_FreeValue(ctx, kw);
                return -1;
            }
            n->depsch.v[i] = sub;
            JS_FreeValue(ctx, vals[i]);
        }
        js_free(ctx, names); js_free(ctx, vals);
        n->f |= SC_F_DEPSCH;
    }
    JS_FreeValue(ctx, kw);

    /* ---- composition --------------------------------------------------- */
    if (dyn_sc_kw(ctx, sv, "allOf", &kw)) return -1;
    if (!JS_IsUndefined(kw)) {
        uint64_t len, i;
        if (dyn_sc_check_list(ctx, kw, "allOf", &len)) {
            JS_FreeValue(ctx, kw);
            return -1;
        }
        n->all.v = js_mallocz(ctx, (len ? len : 1) * sizeof(dyn_scnode_t *));
        if (!n->all.v) { JS_FreeValue(ctx, kw); return -1; }
        n->all.n = (int)len;        /* early */
        for (i = 0; i < len; i++) {
            char idx[24];
            int il = snprintf(idx, sizeof idx, "%llu",
                              (unsigned long long)i);
            JSValue el = JS_GetPropertyUint32(ctx, kw, (uint32_t)i);
            dyn_scnode_t *sub;
            if (JS_IsException(el)) { JS_FreeValue(ctx, kw); return -1; }
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

    if (dyn_sc_kw(ctx, sv, "anyOf", &kw)) return -1;
    if (!JS_IsUndefined(kw)) {
        uint64_t len, i;
        if (dyn_sc_check_list(ctx, kw, "anyOf", &len)) {
            JS_FreeValue(ctx, kw);
            return -1;
        }
        n->any.v = js_mallocz(ctx, (len ? len : 1) * sizeof(dyn_scnode_t *));
        if (!n->any.v) { JS_FreeValue(ctx, kw); return -1; }
        n->any.n = (int)len;        /* early */
        for (i = 0; i < len; i++) {
            char idx[24];
            int il = snprintf(idx, sizeof idx, "%llu",
                              (unsigned long long)i);
            JSValue el = JS_GetPropertyUint32(ctx, kw, (uint32_t)i);
            dyn_scnode_t *sub;
            if (JS_IsException(el)) { JS_FreeValue(ctx, kw); return -1; }
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

    if (dyn_sc_kw(ctx, sv, "oneOf", &kw)) return -1;
    if (!JS_IsUndefined(kw)) {
        uint64_t len, i;
        if (dyn_sc_check_list(ctx, kw, "oneOf", &len)) {
            JS_FreeValue(ctx, kw);
            return -1;
        }
        n->one.v = js_mallocz(ctx, (len ? len : 1) * sizeof(dyn_scnode_t *));
        if (!n->one.v) { JS_FreeValue(ctx, kw); return -1; }
        n->one.n = (int)len;        /* early */
        for (i = 0; i < len; i++) {
            char idx[24];
            int il = snprintf(idx, sizeof idx, "%llu",
                              (unsigned long long)i);
            JSValue el = JS_GetPropertyUint32(ctx, kw, (uint32_t)i);
            dyn_scnode_t *sub;
            if (JS_IsException(el)) { JS_FreeValue(ctx, kw); return -1; }
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

    if (dyn_sc_kw(ctx, sv, "not", &kw)) return -1;
    if (!JS_IsUndefined(kw)) {
        if (dyn_sc_compile_sub(c, kw, path, plen, "not", strlen("not"),
                               &n->notn)) {
            JS_FreeValue(ctx, kw);
            return -1;
        }
        n->f |= SC_F_NOT;
    }
    JS_FreeValue(ctx, kw);

    /* then/else are only compiled when if is present: without if they are
       ignored by the spec, and a broken $ref inside must not reject the
       schema. */
    if (dyn_sc_kw(ctx, sv, "if", &kw)) return -1;
    if (!JS_IsUndefined(kw)) {
        if (dyn_sc_compile_sub(c, kw, path, plen, "if", strlen("if"),
                               &n->ifn)) {
            JS_FreeValue(ctx, kw);
            return -1;
        }
        n->f |= SC_F_IF;
    }
    JS_FreeValue(ctx, kw);

    if (n->f & SC_F_IF) {
        if (dyn_sc_kw(ctx, sv, "then", &kw)) return -1;
        if (!JS_IsUndefined(kw)) {
            if (dyn_sc_compile_sub(c, kw, path, plen, "then", strlen("then"),
                                   &n->then)) {
                JS_FreeValue(ctx, kw);
                return -1;
            }
        }
        JS_FreeValue(ctx, kw);

        if (dyn_sc_kw(ctx, sv, "else", &kw)) return -1;
        if (!JS_IsUndefined(kw)) {
            if (dyn_sc_compile_sub(c, kw, path, plen, "else", strlen("else"),
                                   &n->els)) {
                JS_FreeValue(ctx, kw);
                return -1;
            }
        }
        JS_FreeValue(ctx, kw);
    }

    /* ---- $dynamicRef: an applicator this validator does not implement. --
       Silently ignoring it would drop a constraint (false accept), so the
       schema is REFUSED at compile time instead. ($dynamicAnchor alone is a
       declaration, like $anchor: ignoring it changes nothing by itself.) */
    if (dyn_sc_kw(ctx, sv, "$dynamicRef", &kw)) return -1;
    if (!JS_IsUndefined(kw)) {
        JS_ThrowTypeError(ctx,
            "dyna:schema: $dynamicRef is not supported (use plain $ref)");
        JS_FreeValue(ctx, kw);
        return -1;
    }
    JS_FreeValue(ctx, kw);

    /* ---- $ref ---------------------------------------------------------- */
    if (dyn_sc_kw(ctx, sv, "$ref", &kw)) return -1;
    if (!JS_IsUndefined(kw)) {
        size_t nl;
        const char *rs;
        if (!JS_IsString(kw)) {
            JS_ThrowTypeError(ctx, "dyna:schema: \"$ref\" must be a string");
            JS_FreeValue(ctx, kw);
            return -1;
        }
        rs = JS_ToCStringLen(ctx, &nl, kw);
        if (!rs) { JS_FreeValue(ctx, kw); return -1; }
        n->ref = js_malloc(ctx, nl + 1);
        if (!n->ref) { JS_FreeCString(ctx, rs); JS_FreeValue(ctx, kw);
                       return -1; }
        memcpy(n->ref, rs, nl);
        n->ref[nl] = '\0';
        JS_FreeCString(ctx, rs);
        JS_FreeValue(ctx, kw);
        n->f |= SC_F_REF;
    }

    /* ---- $defs / definitions: register members, validate nothing ------- */
    if (dyn_sc_kw(ctx, sv, "$defs", &kw)) return -1;
    if (!JS_IsUndefined(kw)) {
        char **names;
        JSValue *vals;
        int i, nk;
        char *defpath;
        size_t deflen;
        if (dyn_sc_read_map(ctx, kw, "$defs", &names, &vals, &nk)) {
            JS_FreeValue(ctx, kw);
            return -1;
        }
        defpath = dyn_sc_child_path(c->ctx, path, plen, "$defs", 5, &deflen);
        if (!defpath) {
            for (i = 0; i < nk; i++) { js_free(ctx, names[i]); JS_FreeValue(ctx, vals[i]); }
            js_free(ctx, names); js_free(ctx, vals);
            JS_FreeValue(ctx, kw);
            return -1;
        }
        for (i = 0; i < nk; i++) {
            dyn_scnode_t *sub;
            if (dyn_sc_compile_sub(c, vals[i], defpath, deflen, names[i],
                                   strlen(names[i]), &sub)) {
                int j;
                for (j = i; j < nk; j++)
                    JS_FreeValue(ctx, vals[j]);
                for (j = i; j < nk; j++)
                    js_free(ctx, names[j]);
                js_free(ctx, names); js_free(ctx, vals);
                js_free(c->ctx, defpath);
                JS_FreeValue(ctx, kw);
                return -1;
            }
            JS_FreeValue(ctx, vals[i]);
            js_free(ctx, names[i]);
        }
        js_free(c->ctx, defpath);
        js_free(ctx, names); js_free(ctx, vals);
    }
    JS_FreeValue(ctx, kw);

    if (dyn_sc_kw(ctx, sv, "definitions", &kw)) return -1;
    if (!JS_IsUndefined(kw)) {
        char **names;
        JSValue *vals;
        int i, nk;
        char *defpath;
        size_t deflen;
        if (dyn_sc_read_map(ctx, kw, "definitions", &names, &vals, &nk)) {
            JS_FreeValue(ctx, kw);
            return -1;
        }
        defpath = dyn_sc_child_path(c->ctx, path, plen, "definitions", 11, &deflen);
        if (!defpath) {
            for (i = 0; i < nk; i++) { js_free(ctx, names[i]); JS_FreeValue(ctx, vals[i]); }
            js_free(ctx, names); js_free(ctx, vals);
            JS_FreeValue(ctx, kw);
            return -1;
        }
        for (i = 0; i < nk; i++) {
            dyn_scnode_t *sub;
            if (dyn_sc_compile_sub(c, vals[i], defpath, deflen, names[i],
                                   strlen(names[i]), &sub)) {
                int j;
                for (j = i; j < nk; j++)
                    JS_FreeValue(ctx, vals[j]);
                for (j = i; j < nk; j++)
                    js_free(ctx, names[j]);
                js_free(ctx, names); js_free(ctx, vals);
                js_free(c->ctx, defpath);
                JS_FreeValue(ctx, kw);
                return -1;
            }
            JS_FreeValue(ctx, vals[i]);
            js_free(ctx, names[i]);
        }
        js_free(c->ctx, defpath);
        js_free(ctx, names); js_free(ctx, vals);
    }
    JS_FreeValue(ctx, kw);

    return 0;
}

/* ----------------------------------------------------- phase 2: resolve */

/* RFC 3986 percent-decoding of a $ref fragment. 2020-12: the
   fragment is decoded BEFORE it is read as an RFC 6901 pointer, so a
   $defs key containing '%' or '"' is spelled %25 / %22 in the $ref
   ("#/definitions/~1weird%20name" must reach the key "/weird name" after
   the ~1 step). Decoding is octet-wise: the index paths are UTF-8 byte
   strings, so reassembling %XX octets reproduces them exactly. A malformed
   or partial escape passes through unchanged (best-effort, like common
   validators) rather than failing the whole schema. */
/* Octet-wise decode is the SHARED core's (src/core/dyn-pct.h,
 * plus_space=0): this was a byte-identical copy of dyna-url.c's decoder
 * (round-8 census) and copies drift. */
static char *dyn_sc_pct_decode(JSContext *ctx, const char *s)
{
    size_t n = strlen(s);
    char *out = js_malloc(ctx, n + 1);
    dyn_pct_buf_t b;
    if (!out)
        return NULL;
    b.p = out; b.n = 0;
    dyn_pct_decode_core(&b, dyn_pct_buf_sink, s, n, 0);
    out[b.n] = '\0';
    return out;
}

static int dyn_sc_resolve_ref(dyn_sccomp_t *c, dyn_scnode_t *n)
{
    const char *r = n->ref;
    JSContext *ctx = c->ctx;
    char *frag;
    if (r[0] != '#') {
        JS_ThrowTypeError(ctx, "dyna:schema: remote $ref not supported: \"%s\"",
                          r);
        return -1;
    }
    /* The decoded fragment is the pointer: "" for the root, "/..." otherwise.
       Shape is checked POST-decode so "%2Fproperties%2Ffoo" is accepted as
       the pointer "/properties/foo", per the decode-then-parse order above. */
    frag = dyn_sc_pct_decode(ctx, r + 1);
    if (!frag)
        return -1;
    if (frag[0] != '\0' && frag[0] != '/') {
        JS_ThrowTypeError(ctx,
            "dyna:schema: unsupported $ref fragment \"%s\" (only plain JSON "
            "pointers are supported)", r);
        js_free(ctx, frag);
        return -1;
    }
    n->ref_tgt = dyn_sc_index_find(c, frag, strlen(frag));
    js_free(ctx, frag);
    if (!n->ref_tgt) {
        JS_ThrowTypeError(ctx, "dyna:schema: unresolved $ref \"%s\"", r);
        return -1;
    }
    return 0;
}

/* A pure-ref cycle (a $ref chain that never descends into an instance) can
   never validate; report it at compile time. The walk tracks every node seen
   on the chain, so a cycle that returns to an EARLIER node (not just the
   start) is caught. Recursion THROUGH instance descent is legal and handled
   by the runtime ref stack, not here. */
static int dyn_sc_check_pure_ref_cycles(dyn_sccomp_t *c)
{
    int i;
    for (i = 0; i < c->sch->n_nodes; i++) {
        dyn_scnode_t *n = c->sch->nodes[i];
        dyn_scnode_t *chain[DYN_SC_MAX_REFCHAIN];
        dyn_scnode_t *cur;
        int nchain = 0;
        if ((n->f & SC_F_REF) == 0)
            continue;
        cur = n;
        while (cur && (cur->f & SC_F_REF) && cur->ref_tgt &&
               (cur->ref_tgt->f & SC_F_REF)) {
            int k;
            if (nchain >= DYN_SC_MAX_REFCHAIN)
                break;               /* the runtime ref stack bounds the rest */
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

static int dyn_sc_resolve_refs(dyn_sccomp_t *c)
{
    int i;
    for (i = 0; i < c->sch->n_nodes; i++) {
        dyn_scnode_t *n = c->sch->nodes[i];
        if (n->f & SC_F_REF)
            if (dyn_sc_resolve_ref(c, n))
                return -1;
    }
    return dyn_sc_check_pure_ref_cycles(c);
}

/* Compile-time fixpoint for SC_F_UREACH: the bit is set on every node whose
   in-place closure (self + allOf + dependentSchemas + then + else +
   $ref-target, transitively) contains an unevaluated* keyword. These are
   exactly the EDGES ALONG WHICH evaluation annotations flow at run time:
   each such check frame opens a record and merges it into its caller's on
   success; anyOf/oneOf/not/if are deliberately NOT edges (they run in their
   own evaluation traversal). Implemented as a BFS over reverse in-place
   edges seeded at the unevaluated* nodes -- O(nodes + edges), monotone, so
   recursive $ref graphs converge without iteration caps. */
static int dyn_sc_compute_ureach(JSContext *ctx, dyn_schema_t *sch)
{
    int i, n = sch->n_nodes, nedges = 0;
    int *rcount = NULL, *rstart = NULL, *redge = NULL, *fill = NULL;
    int *queue = NULL;
    int head = 0, tail = 0;
    int ret = -1;

    if (n <= 0)
        return 0;
    for (i = 0; i < n; i++) {
        dyn_scnode_t *m = sch->nodes[i];
        nedges += m->all.n + m->depsch.n + (m->then ? 1 : 0)
                  + (m->els ? 1 : 0) + ((m->f & SC_F_REF) ? 1 : 0);
    }
    rcount = js_mallocz(ctx, ((size_t)n + 1) * sizeof(int));
    rstart = js_mallocz(ctx, ((size_t)n + 2) * sizeof(int));
    redge = js_mallocz(ctx, (size_t)(nedges ? nedges : 1) * sizeof(int));
    fill = js_mallocz(ctx, (size_t)n * sizeof(int));
    queue = js_mallocz(ctx, (size_t)n * sizeof(int));
    if (!rcount || !rstart || !redge || !fill || !queue)
        goto out;

    /* child -> parent adjacency: redge[rstart[c] .. rstart[c+1]) lists the
       nodes that apply child c IN PLACE (n->idx = flat-list position) */
    for (i = 0; i < n; i++) {
        dyn_scnode_t *m = sch->nodes[i];
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
        dyn_scnode_t *m = sch->nodes[i];
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
#undef UREACH_EDGE
    }

    /* seed at the consumers, BFS to the in-place users; each node is
       enqueued at most once (the bit gates), so this terminates */
    for (i = 0; i < n; i++) {
        dyn_scnode_t *m = sch->nodes[i];
        if (m->f & (SC_F_UNEVALP | SC_F_UNEVALI)) {
            m->f |= SC_F_UREACH;
            queue[tail++] = i;
        }
    }
    while (head < tail) {
        int p = queue[head++];
        for (i = rstart[p]; i < rstart[p + 1]; i++) {
            dyn_scnode_t *a = sch->nodes[redge[i]];
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

/* ------------------------------------------------------- schema lifetime */

static void dyn_sc_node_free_rt(JSRuntime *rt, dyn_scnode_t *n)
{
    int i;
    if (!n)
        return;
    if (n->f & SC_F_CONST)
        JS_FreeValueRT(rt, n->val);
    if (n->f & SC_F_ENUM) {
        for (i = 0; i < n->n_enum; i++)
            JS_FreeValueRT(rt, n->enumv[i]);
        js_free_rt(rt, n->enumv);
    }
    js_free_rt(rt, n->re);
    js_free_rt(rt, n->ref);
    js_free_rt(rt, n->fmt);
    for (i = 0; i < n->props.n; i++)
        js_free_rt(rt, n->props.names[i]);
    js_free_rt(rt, n->props.names);
    js_free_rt(rt, n->props.v);
    for (i = 0; i < n->depsch.n; i++)
        js_free_rt(rt, n->depsch.names[i]);
    js_free_rt(rt, n->depsch.names);
    js_free_rt(rt, n->depsch.v);
    for (i = 0; i < n->n_patts; i++) {
        js_free_rt(rt, n->patts[i].pat);
        js_free_rt(rt, n->patts[i].re);
    }
    js_free_rt(rt, n->patts);
    for (i = 0; i < n->required.n; i++)
        js_free_rt(rt, n->required.names[i]);
    js_free_rt(rt, n->required.names);
    js_free_rt(rt, n->required.v);
    for (i = 0; i < n->depreq.n; i++) {
        int j;
        js_free_rt(rt, n->depreq.props[i]);
        for (j = 0; j < n->depreq.nneed[i]; j++)
            js_free_rt(rt, n->depreq.need[i][j]);
        js_free_rt(rt, n->depreq.need[i]);
    }
    js_free_rt(rt, n->depreq.props);
    js_free_rt(rt, n->depreq.need);
    js_free_rt(rt, n->depreq.nneed);
    js_free_rt(rt, n->prefix.v);
    js_free_rt(rt, n->all.v);
    js_free_rt(rt, n->any.v);
    js_free_rt(rt, n->one.v);
    js_free_rt(rt, n);
}

static void dyn_sc_schema_free_rt(JSRuntime *rt, dyn_schema_t *sch)
{
    int i;
    if (!sch)
        return;
    for (i = 0; i < sch->n_nodes; i++)
        dyn_sc_node_free_rt(rt, sch->nodes[i]);
    js_free_rt(rt, sch->nodes);
    js_free_rt(rt, sch);
}

static void dyn_sc_schema_finalizer(JSRuntime *rt, JSValue val)
{
    dyn_sc_schema_free_rt(rt, JS_GetOpaque(val, dyn_schema_class_id));
}

/* The compiled tree holds JSValues (const / enum items), so the cycle
   collector must be able to trace them -- the flat node list is the trace. */
static void dyn_sc_schema_mark(JSRuntime *rt, JSValueConst val,
                               JS_MarkFunc *mark_func)
{
    dyn_schema_t *sch = JS_GetOpaque(val, dyn_schema_class_id);
    int i, j;
    if (!sch)
        return;
    for (i = 0; i < sch->n_nodes; i++) {
        dyn_scnode_t *n = sch->nodes[i];
        if (n->f & SC_F_CONST)
            JS_MarkValue(rt, n->val, mark_func);
        if (n->f & SC_F_ENUM)
            for (j = 0; j < n->n_enum; j++)
                JS_MarkValue(rt, n->enumv[j], mark_func);
    }
}

/* --------------------------------------------------------------- instance */

static int dyn_sc_inst_type(JSContext *ctx, JSValueConst v)
{
    if (JS_IsNull(v))
        return DYN_SC_T_NULL;
    if (JS_IsBool(v))
        return DYN_SC_T_BOOL;
    if (JS_IsString(v))
        return DYN_SC_T_STRING;
    if (JS_IsNumber(v))
        return DYN_SC_T_NUMBER;
    if (JS_IsObject(v)) {
        if (dyn_sc_is_array(ctx, v))
            return DYN_SC_T_ARRAY;
        if (!JS_IsFunction(ctx, v))
            return DYN_SC_T_OBJECT;
    }
    return 0;                              /* undefined / bigint / function */
}

/* Own-property read (prototype chain must never answer a JSON key, or a
   "__proto__" key would resolve Object.prototype). 1 found (*out owns the
   value), 0 missing, -1 exception. */
static int dyn_sc_get_own(JSContext *ctx, JSValueConst obj, JSAtom a,
                          JSValue *out)
{
    JSPropertyDescriptor desc;
    int r = JS_GetOwnProperty(ctx, &desc, obj, a);
    if (r < 0)
        return -1;
    if (r == 0)
        return 0;
    JS_FreeValue(ctx, desc.getter);
    JS_FreeValue(ctx, desc.setter);
    *out = desc.value;
    return 1;
}

static int dyn_sc_has_own(JSContext *ctx, JSValueConst obj, const char *s,
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

/* Code points, from a UTF-8 span (any byte that is not a continuation byte
   starts a code point). Matches the JSON Schema minLength/maxLength unit. */
static int dyn_sc_utf8_cplen(const char *s, size_t n)
{
    size_t i, count = 0;
    for (i = 0; i < n; i++)
        if (((unsigned char)s[i] & 0xC0) != 0x80)
            count++;
    return (int)count;
}

/* ---- deep equality (for const / enum / uniqueItems) -------------------- */

/* Decode a UTF-8 span back into UTF-16 code units. JS_ToCStringLen output is
   well-formed UTF-8 with lone surrogates kept as 3-byte CESU-8; this restores
   exactly the string's code units so a unicode-flag pattern sees the same
   characters a JS RegExp would. Returns unit count, or -1 on alloc failure. */
static int dyn_sc_utf8_to_utf16(JSContext *ctx, const char *s, size_t n,
                                uint16_t **out)
{
    uint16_t *u = js_malloc(ctx, (n + 1) * sizeof(uint16_t));
    size_t i = 0, o = 0;
    uint32_t c;
    if (!u)
        return -1;
    while (i < n) {
        uint8_t b = (uint8_t)s[i];
        if (b < 0x80) {
            u[o++] = b; i++;
        } else if ((b & 0xE0) == 0xC0 && i + 1 < n) {
            u[o++] = (uint16_t)(((uint16_t)(b & 0x1F) << 6) |
                                ((uint8_t)s[i + 1] & 0x3F));
            i += 2;
        } else if ((b & 0xF0) == 0xE0 && i + 2 < n) {
            c = ((uint32_t)(b & 0x0F) << 12) |
                (((uint32_t)(uint8_t)s[i + 1] & 0x3F) << 6) |
                ((uint32_t)(uint8_t)s[i + 2] & 0x3F);
            i += 3;
            if (c >= 0x10000 && c <= 0x10FFFF) {
                c -= 0x10000;
                u[o++] = (uint16_t)(0xD800 + (c >> 10));
                u[o++] = (uint16_t)(0xDC00 + (c & 0x3FF));
            } else {
                u[o++] = (uint16_t)c;       /* incl. lone surrogates */
            }
        } else if ((b & 0xF8) == 0xF0 && i + 3 < n) {
            c = ((uint32_t)(b & 0x07) << 18) |
                (((uint32_t)(uint8_t)s[i + 1] & 0x3F) << 12) |
                (((uint32_t)(uint8_t)s[i + 2] & 0x3F) << 6) |
                ((uint32_t)(uint8_t)s[i + 3] & 0x3F);
            i += 4;
            if (c <= 0x10FFFF) {
                c -= 0x10000;
                u[o++] = (uint16_t)(0xD800 + (c >> 10));
                u[o++] = (uint16_t)(0xDC00 + (c & 0x3FF));
            } else {
                u[o++] = 0xFFFD;
            }
        } else {
            u[o++] = b; i++;                /* invalid byte: pass through */
        }
    }
    *out = u;
    return (int)o;
}

/* Match UTF-8 span s[0..n) against compiled bytecode bc. 1 match, 0 no match,
   -1 exception. The capture array and the UTF-16 conversion are allocated per
   call (never cached in the node -- a property getter can re-enter validate
   and clobber cached state). Patterns run with unicode semantics over the
   instance's code units. */
static int dyn_sc_re_match(JSContext *ctx, const uint8_t *bc, const char *s,
                           size_t n)
{
    int cc;
    uint8_t **cap = NULL;
    uint16_t *u = NULL;
    int ulen, rc;
    if (n > 2147483647u)
        return -1;
    ulen = dyn_sc_utf8_to_utf16(ctx, s, n, &u);
    if (ulen < 0)
        return -1;
    cc = lre_get_alloc_count(bc);
    if (cc > 0) {
        cap = js_malloc(ctx, (size_t)cc * sizeof(uint8_t *));
        if (!cap) {
            js_free(ctx, u);
            return -1;
        }
    }
    rc = lre_exec(cap, bc, (const uint8_t *)u, 0, ulen, 1, ctx);
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

/* A pair on the deep-equality comparison stack. */
typedef struct { JSValueConst a, b; } dyn_sc_pair_t;

static int dyn_sc_equal(JSContext *ctx, JSValueConst a, JSValueConst b,
                        int depth, dyn_sc_pair_t *seen, int nseen)
{
    int i;

    if (depth > DYN_SC_MAX_EQUAL_DEPTH)
        return JS_StrictEq(ctx, a, b);  /* deep acyclic DAG: backstop */

    if (JS_IsObject(a) && JS_IsObject(b) && !JS_IsFunction(ctx, a) && !JS_IsFunction(ctx, b)) {
        for (i = 0; i < nseen; i++) {
            if (JS_VALUE_GET_PTR(seen[i].a) == JS_VALUE_GET_PTR(a) &&
                JS_VALUE_GET_PTR(seen[i].b) == JS_VALUE_GET_PTR(b))
                return 1;
        }
        if (nseen < DYN_SC_MAX_EQUAL_DEPTH) {
            seen[nseen].a = a;
            seen[nseen].b = b;
            nseen++;
        }
    }

    if (dyn_sc_is_array(ctx, a) || dyn_sc_is_array(ctx, b)) {
        uint64_t la, lb, i2;
        if (dyn_sc_is_array(ctx, a) != 1 || dyn_sc_is_array(ctx, b) != 1)
            return 0;
        la = dyn_sc_arr_len(ctx, a);
        lb = dyn_sc_arr_len(ctx, b);
        if (la == (uint64_t)-1 || lb == (uint64_t)-1)
            return -1;
        if (la != lb)
            return 0;
        for (i2 = 0; i2 < la; i2++) {
            JSValue ea = JS_GetPropertyUint32(ctx, a, (uint32_t)i2);
            JSValue eb = JS_GetPropertyUint32(ctx, b, (uint32_t)i2);
            int r;
            if (JS_IsException(ea) || JS_IsException(eb)) {
                JS_FreeValue(ctx, ea);
                JS_FreeValue(ctx, eb);
                return -1;
            }
            r = dyn_sc_equal(ctx, ea, eb, depth + 1, seen, nseen);
            JS_FreeValue(ctx, ea);
            JS_FreeValue(ctx, eb);
            if (r < 0)
                return -1;
            if (r == 0)
                return 0;
        }
        return 1;
    }
    if (JS_IsObject(a) || JS_IsObject(b)) {
        JSPropertyEnum *pa = NULL, *pb = NULL;
        uint32_t na, nb, k;
        if (!JS_IsObject(a) || !JS_IsObject(b))
            return 0;
        if (JS_IsFunction(ctx, a) || JS_IsFunction(ctx, b))
            return JS_StrictEq(ctx, a, b);
        if (JS_GetOwnPropertyNames(ctx, &pa, &na, a,
                                   JS_GPN_ENUM_ONLY | JS_GPN_STRING_MASK) < 0)
            return -1;
        if (JS_GetOwnPropertyNames(ctx, &pb, &nb, b,
                                   JS_GPN_ENUM_ONLY | JS_GPN_STRING_MASK) < 0) {
            JS_FreePropertyEnum(ctx, pa, na);
            return -1;
        }
        if (na != nb) {
            JS_FreePropertyEnum(ctx, pa, na);
            JS_FreePropertyEnum(ctx, pb, nb);
            return 0;
        }
        for (k = 0; k < na; k++) {
            JSValue va, vb;
            int found, r;
            /* OWN reads on BOTH operands (the b side already was one): a
               getter must never run inside a validator, where it could
               re-enter and mutate the value mid-comparison. An accessor
               therefore compares as its undefined value, symmetrically. */
            found = dyn_sc_get_own(ctx, a, pa[k].atom, &va);
            if (found < 0) {
                JS_FreePropertyEnum(ctx, pa, na);
                JS_FreePropertyEnum(ctx, pb, nb);
                return -1;
            }
            if (found == 0) {
                JS_FreePropertyEnum(ctx, pa, na);
                JS_FreePropertyEnum(ctx, pb, nb);
                return 0;
            }
            found = dyn_sc_get_own(ctx, b, pa[k].atom, &vb);
            if (found < 0) {
                JS_FreeValue(ctx, va);
                JS_FreePropertyEnum(ctx, pa, na);
                JS_FreePropertyEnum(ctx, pb, nb);
                return -1;
            }
            if (found == 0) {
                JS_FreeValue(ctx, va);
                JS_FreePropertyEnum(ctx, pa, na);
                JS_FreePropertyEnum(ctx, pb, nb);
                return 0;
            }
            r = dyn_sc_equal(ctx, va, vb, depth + 1, seen, nseen);
            JS_FreeValue(ctx, va);
            JS_FreeValue(ctx, vb);
            if (r < 0) {
                JS_FreePropertyEnum(ctx, pa, na);
                JS_FreePropertyEnum(ctx, pb, nb);
                return -1;
            }
            if (r == 0) {
                JS_FreePropertyEnum(ctx, pa, na);
                JS_FreePropertyEnum(ctx, pb, nb);
                return 0;
            }
        }
        JS_FreePropertyEnum(ctx, pa, na);
        JS_FreePropertyEnum(ctx, pb, nb);
        return 1;
    }
    return JS_StrictEq(ctx, a, b);
}

/* ------------------------------------------------------------- validating */

typedef struct { char *buf; size_t len, cap; } dyn_scpath_t;

/* An EVALUATION RECORD: which properties / item indices of ONE instance
   position the current evaluation traversal has successfully evaluated.
 2020-12: unevaluated* consumes the union of these annotations.
   Lifetime is strictly one check frame (the struct lives on the C stack of
   dyn_sc_check; only the name/index arrays are heap): on SUCCESS the frame
   moves its entries into the CALLER's record; on failure they die with the
   frame -- a failed subschema leaves no annotations (corpus-pinned: failed
   anyOf branches, failed if, not). Child-instance descents never merge, so
   annotations cannot cross instance positions. */
typedef struct {
    char **props;               /* evaluated property names (owned) */
    int *plens;                 /* their byte lengths (names may hold NUL) */
    int nprops, cprops;
    uint64_t *items;            /* evaluated item indices (owned) */
    int nitems, citems;
} dyn_sceval_t;

static void dyn_sc_eval_free(JSContext *ctx, dyn_sceval_t *e)
{
    int i;
    for (i = 0; i < e->nprops; i++)
        js_free(ctx, e->props[i]);
    js_free(ctx, e->props);
    js_free(ctx, e->plens);
    js_free(ctx, e->items);
}

static int dyn_sc_eval_add_prop(JSContext *ctx, dyn_sceval_t *e,
                                const char *name, size_t len)
{
    char *p;
    if (!e)
        return 0;               /* nobody consumes annotations here */
    if (e->nprops >= DYN_SC_MAX_EVAL) {
        JS_ThrowRangeError(ctx,
            "dyna:schema: over %d evaluated properties tracked by "
            "unevaluated* validation", DYN_SC_MAX_EVAL);
        return -1;
    }
    if (e->nprops >= e->cprops) {
        int ncap = e->cprops ? e->cprops * 2 : 8;
        char **np;
        int *nl;
        np = js_realloc(ctx, e->props, (size_t)ncap * sizeof(*np));
        if (!np)
            return -1;
        e->props = np;
        nl = js_realloc(ctx, e->plens, (size_t)ncap * sizeof(*nl));
        if (!nl)
            return -1;
        e->plens = nl;
        e->cprops = ncap;
    }
    p = js_malloc(ctx, len + 1);
    if (!p)
        return -1;
    memcpy(p, name, len);
    p[len] = '\0';
    e->props[e->nprops] = p;
    e->plens[e->nprops] = (int)len;
    e->nprops++;
    return 0;
}

static int dyn_sc_eval_has_prop(const dyn_sceval_t *e, const char *name,
                                size_t len)
{
    int i;
    if (!e)
        return 0;
    for (i = 0; i < e->nprops; i++)
        if ((size_t)e->plens[i] == len &&
            memcmp(e->props[i], name, len) == 0)
            return 1;
    return 0;
}

static int dyn_sc_eval_add_item(JSContext *ctx, dyn_sceval_t *e, uint64_t idx)
{
    int i;
    if (!e)
        return 0;               /* nobody consumes annotations here */
    for (i = 0; i < e->nitems; i++)
        if (e->items[i] == idx)
            return 0;           /* already recorded (items + contains) */
    if (e->nitems >= DYN_SC_MAX_EVAL) {
        JS_ThrowRangeError(ctx,
            "dyna:schema: over %d evaluated items tracked by unevaluated* "
            "validation", DYN_SC_MAX_EVAL);
        return -1;
    }
    if (e->nitems >= e->citems) {
        int ncap = e->citems ? e->citems * 2 : 8;
        uint64_t *ni = js_realloc(ctx, e->items, (size_t)ncap * sizeof(*ni));
        if (!ni)
            return -1;
        e->items = ni;
        e->citems = ncap;
    }
    e->items[e->nitems++] = idx;
    return 0;
}

static int dyn_sc_eval_has_item(const dyn_sceval_t *e, uint64_t idx)
{
    int i;
    if (!e)
        return 0;
    for (i = 0; i < e->nitems; i++)
        if (e->items[i] == idx)
            return 1;
    return 0;
}

/* Move src's annotations onto dst (strings/indices are stolen, not copied).
   Over either cap the validation FAILS CLOSED: an annotation that would be
   dropped could otherwise flip an unevaluated* verdict. */
static int dyn_sc_eval_merge(JSContext *ctx, dyn_sceval_t *dst,
                             dyn_sceval_t *src)
{
    if (src->nprops) {
        if (dst->nprops + src->nprops > DYN_SC_MAX_EVAL ||
            src->nprops > DYN_SC_MAX_EVAL - dst->nprops) {
            JS_ThrowRangeError(ctx,
                "dyna:schema: over %d evaluated properties tracked by "
                "unevaluated* validation", DYN_SC_MAX_EVAL);
            return -1;
        }
        if (dst->nprops + src->nprops > dst->cprops) {
            int ncap = dst->cprops ? dst->cprops : 8;
            char **np;
            int *nl;
            while (ncap < dst->nprops + src->nprops)
                ncap *= 2;
            np = js_realloc(ctx, dst->props, (size_t)ncap * sizeof(*np));
            if (!np)
                return -1;
            dst->props = np;
            nl = js_realloc(ctx, dst->plens, (size_t)ncap * sizeof(*nl));
            if (!nl)
                return -1;
            dst->plens = nl;
            dst->cprops = ncap;
        }
        memcpy(dst->props + dst->nprops, src->props,
               (size_t)src->nprops * sizeof(char *));
        memcpy(dst->plens + dst->nprops, src->plens,
               (size_t)src->nprops * sizeof(int));
        dst->nprops += src->nprops;
        src->nprops = 0;        /* ownership moved */
    }
    if (src->nitems) {
        if (src->nitems > DYN_SC_MAX_EVAL - dst->nitems) {
            JS_ThrowRangeError(ctx,
                "dyna:schema: over %d evaluated items tracked by "
                "unevaluated* validation", DYN_SC_MAX_EVAL);
            return -1;
        }
        if (dst->nitems + src->nitems > dst->citems) {
            int ncap = dst->citems ? dst->citems : 8;
            uint64_t *ni;
            while (ncap < dst->nitems + src->nitems)
                ncap *= 2;
            ni = js_realloc(ctx, dst->items, (size_t)ncap * sizeof(*ni));
            if (!ni)
                return -1;
            dst->items = ni;
            dst->citems = ncap;
        }
        memcpy(dst->items + dst->nitems, src->items,
               (size_t)src->nitems * sizeof(uint64_t));
        dst->nitems += src->nitems;
        src->nitems = 0;
    }
    return 0;
}

typedef struct { char *path; int pathlen; char *message, *keyword; } dyn_scerr_t;

/* A per-validate error collector. Scratch collectors (for anyOf/oneOf/not/if
   branches) are swapped in and out of the context; on a failing branch the
   best branch's collected errors are MOVED into the real sink, so no branch
   is ever validated twice. */
typedef struct {
    dyn_scerr_t *e;
    int n, cap;
    int done;                               /* error cap reached */
    int drops;          /* errors lost to allocation failure: an error that
                         * vanishes would flip the outcome to valid, so any
                         * drop forces invalid (see dyn_sc_result) */
} dyn_scerrs_t;

typedef struct {
    JSContext *ctx;
    dyn_schema_t *sch;
    dyn_scerrs_t *errs;                     /* active collector */
    dyn_scpath_t path;                      /* instance JSON pointer buffer */
    int depth;                              /* recursion guard */
    dyn_scnode_t *refstk[DYN_SC_MAX_REFCHAIN];
    int refn;                               /* $ref indirection stack */
    dyn_sc_pair_t *eqseen;                  /* deep-equality cycle stack */
    dyn_sceval_t *cur_eval;   /* evaluation record of the CURRENT instance
                                 position, or NULL when nothing in this
                                 subtree tracks annotations (owned by the
                                 frame whose C stack holds the struct) */
} dyn_vctx_t;

static void dyn_sc_errs_reset(JSContext *ctx, dyn_scerrs_t *s)
{
    int i;
    for (i = 0; i < s->n; i++) {
        js_free(ctx, s->e[i].path);
        js_free(ctx, s->e[i].message);
        js_free(ctx, s->e[i].keyword);
    }
    s->n = 0;
    s->done = 0;
}

static void dyn_sc_errs_free(JSContext *ctx, dyn_scerrs_t *s)
{
    dyn_sc_errs_reset(ctx, s);
    js_free(ctx, s->e);
    s->e = NULL;
    s->cap = 0;
}

/* Move src's collected errors onto the end of dst, stealing the strings.
   Entries beyond dst's cap stay in src (freed by the caller's reset); a drop
   can only happen when dst already holds the maximum, so it cannot flip the
   outcome, but it is counted the same as any other lost error. Returns 0,
   or -1 on allocation failure (src is untouched and still owned by caller). */
static int dyn_sc_errs_move(JSContext *ctx, dyn_scerrs_t *dst,
                            dyn_scerrs_t *src)
{
    int i, room = DYN_SC_MAX_ERRORS - dst->n;
    if (src->n == 0)
        return 0;
    if (room <= 0) {
        dst->done = 1;
        dst->drops += src->drops;
        src->drops = 0;
        return 0;
    }
    if (src->n < room)
        room = src->n;
    if (dst->n + room > dst->cap) {
        int ncap = dst->cap ? dst->cap : 16;
        dyn_scerr_t *ne;
        while (ncap < dst->n + room)
            ncap *= 2;
        ne = js_realloc(ctx, dst->e, (size_t)ncap * sizeof(*ne));
        if (!ne)
            return -1;
        dst->e = ne;
        dst->cap = ncap;
    }
    for (i = 0; i < room; i++) {
        dst->e[dst->n++] = src->e[i];   /* ownership moves with the slot */
        src->e[i].path = NULL;
        src->e[i].message = NULL;
        src->e[i].keyword = NULL;
    }
    dst->drops += src->drops;
    src->drops = 0;
    return 0;
}

static void dyn_sc_add_err(dyn_vctx_t *v, const char *keyword,
                           const char *fmt, ...)
{
    dyn_scerr_t *e;
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
        dyn_scerr_t *ne = js_realloc(v->ctx, v->errs->e,
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
    size_t mlen = strlen(msg), klen = strlen(keyword);
    /* pathlen is the REAL byte length: a property name may contain NUL, and
       the path must survive it (JS_NewStringLen, not JS_NewString) */
    e->pathlen = (int)v->path.len;
    e->path = js_malloc(v->ctx, v->path.len + 1);
    if (!e->path) {
        v->errs->drops++;
        return;
    }
    memcpy(e->path, v->path.buf, v->path.len);
    e->path[v->path.len] = '\0';
    e->message = js_malloc(v->ctx, mlen + 1);
    e->keyword = js_malloc(v->ctx, klen + 1);
    if (!e->message || !e->keyword) {
        /* zero the slot: its pointers must never be observed twice */
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

/* RFC 6901 token escape into the instance path buffer. */
static int dyn_sc_path_push(dyn_vctx_t *v, const char *tok, size_t n)
{
    size_t need = v->path.len + 1 + n * 2 + 1, i, o;
    if (need > v->path.cap) {
        size_t ncap = v->path.cap ? v->path.cap : 16;
        char *nb;
        while (ncap < need)
            ncap *= 2;
        nb = js_realloc(v->ctx, v->path.buf, ncap);
        if (!nb)
            return -1;
        v->path.buf = nb;
        v->path.cap = ncap;
    }
    o = v->path.len;
    v->path.buf[o++] = '/';
    for (i = 0; i < n; i++) {
        if (tok[i] == '~') { v->path.buf[o++] = '~'; v->path.buf[o++] = '0'; }
        else if (tok[i] == '/') { v->path.buf[o++] = '~'; v->path.buf[o++] = '1'; }
        else v->path.buf[o++] = tok[i];
    }
    v->path.len = o;
    v->path.buf[o] = '\0';
    return 0;
}

static void dyn_sc_path_restore(dyn_vctx_t *v, size_t mark)
{
    v->path.len = mark;
    v->path.buf[mark] = '\0';
}

/* type keyword: mask vs instance. "integer" is a refinement of "number";
   NaN/Infinity are not JSON numbers and satisfy neither. */
static int dyn_sc_type_ok(JSContext *ctx, int mask, JSValueConst inst)
{
    int it = dyn_sc_inst_type(ctx, inst);
    int ok = 0;
    if (it == 0)
        return 0;
    if (mask & it)
        ok = 1;
    if (it == DYN_SC_T_NUMBER) {
        double d = 0;
        JS_ToFloat64(ctx, &d, inst);
        if (!isfinite(d))
            ok = 0;
        if ((mask & DYN_SC_T_INTEGER) && isfinite(d) && d == trunc(d))
            ok = 1;
    }
    return ok;
}

static void dyn_sc_type_str(int mask, char *buf, size_t cap)
{
    static const struct { int bit; const char *name; } T[] = {
        { DYN_SC_T_NULL, "null" }, { DYN_SC_T_BOOL, "boolean" },
        { DYN_SC_T_OBJECT, "object" }, { DYN_SC_T_ARRAY, "array" },
        { DYN_SC_T_NUMBER, "number" }, { DYN_SC_T_STRING, "string" },
        { DYN_SC_T_INTEGER, "integer" },
    };
    size_t o = 0;
    int i, n = 0;
    for (i = 0; i < (int)countof(T); i++)
        if (mask & T[i].bit)
            n++;
    if (n == 1) {
        for (i = 0; i < (int)countof(T); i++)
            if (mask & T[i].bit) {
                snprintf(buf, cap, "a %s", T[i].name);
                return;
            }
    }
    o += (size_t)snprintf(buf + o, cap - o, "one of: ");
    for (i = 0; i < (int)countof(T); i++) {
        if (mask & T[i].bit) {
            o += (size_t)snprintf(buf + o, cap - o, "%s%s", T[i].name,
                                  --n ? ", " : "");
        }
    }
}

static int dyn_sc_check(dyn_vctx_t *v, dyn_scnode_t *n, JSValueConst inst);

/* Depth guard lives in ONE place: every recursive call goes through here. */
static int dyn_sc_check_top(dyn_vctx_t *v, dyn_scnode_t *n, JSValueConst inst)
{
    int r;
    if (++v->depth > DYN_SC_MAX_DEPTH) {
        v->depth--;
        JS_ThrowRangeError(v->ctx,
            "dyna:schema: instance nesting exceeds %d levels",
            DYN_SC_MAX_DEPTH);
        return -1;
    }
    r = dyn_sc_check(v, n, inst);
    v->depth--;
    return r;
}

/* $ref indirection: 1 = cycle (error added, skip the deref), 2 = the
   indirection CAP was reached (not a cycle: a long-but-finite pure ref chain
   is legal and must not be called circular), 0 = ok. */
static int dyn_sc_ref_enter(dyn_vctx_t *v, dyn_scnode_t *tgt)
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

/* Validate a property/array element at an instance-descent boundary. The ref
   stack is SAVED here and RESTORED after: a ref that was in progress at the
   PARENT position is not in progress at a DIFFERENT instance position (which
   is what lets recursive schemas, e.g. $ref:"#" through properties,
   terminate), but the bookkeeping must stay balanced so the pop at a $ref
   site can never underflow the stack. The evaluation record is dropped for
   the descent for the same reason: annotations made at the CHILD position
   belong to the child, never to the parent (a child subschema evaluating a
   property must not mark it evaluated at the parent). */
static int dyn_sc_check_child(dyn_vctx_t *v, dyn_scnode_t *sub,
                              JSValueConst inst, const char *tok, size_t n)
{
    size_t mark = v->path.len;
    int saved_refn = v->refn;
    dyn_sceval_t *saved_eval = v->cur_eval;
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

/* The check body (keyword dispatch) -- dyn_sc_check below owns the
   evaluation-record bookkeeping that wraps it. */
static int dyn_sc_check_impl(dyn_vctx_t *v, dyn_scnode_t *n,
                             JSValueConst inst);

static int dyn_sc_check_impl(dyn_vctx_t *v, dyn_scnode_t *n, JSValueConst inst)
{
    JSContext *ctx = v->ctx;
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

    /* ---- type ---------------------------------------------------------- */
    if ((n->f & SC_F_TYPE) && !dyn_sc_type_ok(ctx, n->type_mask, inst)) {
        char buf[80];
        dyn_sc_type_str(n->type_mask, buf, sizeof buf);
        dyn_sc_add_err(v, "type", "must be %s", buf);
        r = 0;
    }

    /* ---- const ---------------------------------------------------------- */
    if (n->f & SC_F_CONST) {
        int eq = dyn_sc_equal(ctx, inst, n->val, 0, v->eqseen, 0);
        if (eq < 0)
            return -1;
        if (!eq) {
            dyn_sc_add_err(v, "const", "must be the constant value");
            r = 0;
        }
    }

    /* ---- enum ----------------------------------------------------------- */
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

    /* ---- numeric bounds (number instances only) ------------------------- */
    if (it == DYN_SC_T_NUMBER) {
        double d = 0;
        JS_ToFloat64(ctx, &d, inst);
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
                /* d/dmult overflows to +inf for huge d against a fractional
                 * multiple: floor(inf) is inf, so frac is NaN and the old
                 * `NaN > eps` test silently ACCEPTED (JSON-Schema-Test-Suite
                 * draft2020-12 multipleOf 3.0: 1e308 vs 0.123456789 must
                 * fail). A non-finite quotient means d is astronomically
                 * larger than the multiple; it is a multiple only if dmult
                 * is integral and the remainder is exactly zero. */
                if (q != q || q - q != 0) {     /* NaN or +-inf */
                    double im = floor(n->dmult);
                    if (im != n->dmult || im <= 0 || fmod(d, im) != 0) {
                        dyn_sc_add_err(v, "multipleOf",
                                       "must be a multiple of %g", n->dmult);
                        r = 0;
                    }
                } else if (frac > DYN_SC_MULT_EPS &&
                           frac < 1 - DYN_SC_MULT_EPS) {
                    dyn_sc_add_err(v, "multipleOf", "must be a multiple of %g",
                                   n->dmult);
                    r = 0;
                }
            }
        } else if (n->f & (SC_F_MIN | SC_F_MAX | SC_F_EMIN | SC_F_EMAX |
                           SC_F_MULT)) {
            dyn_sc_add_err(v, "number", "must be a finite number");
            r = 0;
        }
    }

    /* ---- string bounds -------------------------------------------------- */
    if (it == DYN_SC_T_STRING &&
        (n->f & (SC_F_MINLEN | SC_F_MAXLEN | SC_F_PAT | SC_F_FMT))) {
        const char *s;
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
        if (n->f & SC_F_FMT) {
            /*assertive format against the process-wide registry.
             * The validator fn is called with the instance string; its
             * return value is coerced to boolean; a throw aborts validate
             * with that exception. An UNREGISTERED name stays
             * annotation-only (no entry -> no assertion). */
            JSValue fn = dyn_sc_fmt_lookup(ctx, n->fmt);
            if (JS_IsException(fn)) {
                JS_FreeCString(ctx, s);
                return -1;
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

    /* ---- array bounds --------------------------------------------------- */
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
        /* prefixItems then items over the tail */
        if (len > DYN_SC_MAX_ARRAY) {
            JS_ThrowRangeError(ctx,
                "dyna:schema: arrays longer than %d elements are not validated",
                DYN_SC_MAX_ARRAY);
            return -1;
        }
        for (i2 = 0; i2 < len; i2++) {
            JSValue el;
            int rr;
            dyn_scnode_t *sub = NULL;
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
            else if (rr == 1 &&
                     dyn_sc_eval_add_item(ctx, v->cur_eval, i2))
                return -1;
        }
        /* contains: count the elements that match the subschema. Per-element
           failures are probed on a scratch collector -- only the min/max
           outcome is reportable, an element error list would be noise. */
        if (n->f & SC_F_CONT) {
            dyn_scerrs_t *saved = v->errs;
            dyn_scerrs_t scratch;
            dyn_sceval_t *saved_eval = v->cur_eval;
            uint64_t matches = 0;
            int eff_min = (n->f & SC_F_MINCONT) ? n->mincont : 1;
            /* with unevaluatedItems in play every matching index must be
               recorded, so the early exit (safe when only the min/max
               outcome matters) must not fire; maxContains needs all
               matches counted anyway */
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
                /* the element is a CHILD instance position: its own
                   annotations die with the probe, only the INDEX is
                   annotated (into the array's record) on a match */
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
                    /* early exit is safe only when the max is not enforced
                       AND no annotations are being collected */
                    if (!probe_all &&
                        matches >= (uint64_t)(eff_min > 0 ? eff_min : 0))
                        break;
                }
            }
            dyn_sc_errs_free(ctx, &scratch);
            if (matches < (uint64_t)eff_min) {
                if (n->f & SC_F_MINCONT) {
                    dyn_sc_add_err(v, "minContains",
                                   "must contain at least %d item matching "
                                   "the \"contains\" schema", n->mincont);
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
                               "\"contains\" schema", n->maxcont);
                r = 0;
            }
        }
    }

    /* ---- object keywords ------------------------------------------------ */
    if (it == DYN_SC_T_OBJECT &&
        (n->f & (SC_F_MINPROPS | SC_F_MAXPROPS | SC_F_REQ | SC_F_DEPREQ |
                 SC_F_DEPSCH | SC_F_PNAMES |
                 SC_F_PROPS | SC_F_PATTS | SC_F_ADDL))) {
        JSPropertyEnum *tab = NULL;
        uint32_t nprops, k;

        if (JS_GetOwnPropertyNames(ctx, &tab, &nprops, inst,
                                   JS_GPN_ENUM_ONLY | JS_GPN_STRING_MASK) < 0)
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
                /* allOf-style: the dependent schema's own errors surface */
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
                const char *nb;
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
                if (get == 0) {         /* deleted by a getter mid-loop */
                    JS_FreeCString(ctx, nb);
                    JS_FreeValue(ctx, name);
                    continue;
                }
                if (n->f & SC_F_PNAMES) {
                    /* the NAME is the instance here; it validates at the
                       child path /<name>, and its own failure is reported
                       under the propertyNames keyword as well */
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
                                       "the schema", nb);
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
                            /* a SUCCESSFUL sub-evaluation annotates the
                               property as evaluated (v->cur_eval is NULL
                               unless an unevaluated* consumer is in play) */
                            if (rr == 1 &&
                                dyn_sc_eval_add_prop(ctx, v->cur_eval, nb,
                                                     nl)) {
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
                            if (rr == 1 &&
                                dyn_sc_eval_add_prop(ctx, v->cur_eval, nb,
                                                     nl)) {
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
                        if (rr == 1 &&
                            dyn_sc_eval_add_prop(ctx, v->cur_eval, nb, nl)) {
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
                        /* additionalProperties: true still EVALUATES every
                           not-covered property (trivially successfully) */
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

    /* ---- composition ---------------------------------------------------- */
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
        dyn_scerrs_t *saved = v->errs;
        dyn_scerrs_t scratch, bests;
        int passed = 0, best = -1;
        memset(&scratch, 0, sizeof scratch);
        memset(&bests, 0, sizeof bests);
        /* With annotations being collected (unevaluated* in play) EVERY
           branch is evaluated: the suite pins that a passing branch's
           annotations reach the holder's unevaluated* even when an earlier
           branch already passed ("collect annotations inside a 'not'" --
           the inner anyOf's later branch evaluates the property the inner
           unevaluatedProperties then asserts on). Without a consumer the
           assertion short-circuits as before. */
        for (i = 0; i < n->any.n && (v->cur_eval != NULL || !passed); i++) {
            int rr;
            dyn_sc_errs_reset(ctx, &scratch);   /* frees the last branch's */
            v->errs = &scratch;                 /* errors, no leak per branch */
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
                /* new best: swap the two collectors, so the old best's
                   errors land in scratch where the next iteration's reset
                   frees them */
                dyn_scerrs_t t = scratch;
                scratch = bests;
                bests = t;
                best = i;
            }
        }
        dyn_sc_errs_free(ctx, &scratch);
        if (!passed) {
            /* the best branch's collected errors surface directly -- the
               re-run this replaces validated the instance twice over */
            if (dyn_sc_errs_move(ctx, saved, &bests) < 0) {
                dyn_sc_errs_free(ctx, &bests);
                return -1;
            }
            r = 0;
        }
        dyn_sc_errs_free(ctx, &bests);
    }

    if (n->f & SC_F_ONE) {
        dyn_scerrs_t *saved = v->errs;
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
        dyn_scerrs_t *saved = v->errs;
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
        dyn_scerrs_t *saved = v->errs;
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

    /* ---- unevaluatedProperties / unevaluatedItems (2020-12 ) ------
       Runs LAST so the record already holds this schema's own evaluations
       plus everything merged back by in-place applicators (allOf,
       dependentSchemas, then/else, $ref targets) and PASSING anyOf/oneOf
       branches. Non-object / non-array instances pass trivially. */
    if (it == DYN_SC_T_OBJECT && (n->f & SC_F_UNEVALP)) {
        dyn_sceval_t *rec = v->cur_eval;
        JSPropertyEnum *tab2 = NULL;
        uint32_t nprops2, k2;
        int failed = 0;

        if (JS_GetOwnPropertyNames(ctx, &tab2, &nprops2, inst,
                                   JS_GPN_ENUM_ONLY | JS_GPN_STRING_MASK) < 0)
            return -1;
        for (k2 = 0; k2 < nprops2; k2++) {
            JSValue name = JS_AtomToString(ctx, tab2[k2].atom);
            const char *nb;
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
                /* like additionalProperties: the SUBSCHEMA case validates the
                   value at the child path; a passing check also annotates */
                JSValue val;
                int get = dyn_sc_get_own(ctx, inst, tab2[k2].atom, &val);
                int rr;
                if (get < 0) {
                    JS_FreeCString(ctx, nb);
                    JS_FreeValue(ctx, name);
                    JS_FreePropertyEnum(ctx, tab2, nprops2);
                    return -1;
                }
                if (get == 0)           /* deleted mid-loop */
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
                /* unevaluatedProperties: true evaluates every remaining
                   property (its annotation feeds outer positions) */
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
        dyn_sceval_t *rec = v->cur_eval;
        uint64_t len = dyn_sc_arr_len(ctx, inst), i2;
        int failed = 0;
        if (len == (uint64_t)-1)
            return -1;
        if (len > DYN_SC_MAX_ARRAY) {
            /* the array section capped the length as IT observed it; a
               getter may have grown the array since -- re-refuse here so
               this loop can never run away */
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

/* Evaluation-record wrapper: a frame whose closure can evaluate or consume
   annotations opens a FRESH record for this instance position; on success
   its entries move into the CALLER's record (in-place applicators and
   passing composition branches feed the holder's unevaluated*); on failure
   the record dies (failed branches annotate nothing); child-instance
   descents (dyn_sc_check_child) enter with cur_eval == NULL, so child
   positions never leak annotations upward. Every early return of the impl
   funnels through the single cleanup here. */
static int dyn_sc_check(dyn_vctx_t *v, dyn_scnode_t *n, JSValueConst inst)
{
    dyn_sceval_t rec;
    dyn_sceval_t *saved = v->cur_eval;
    int r;

    if (!saved && !(n->f & SC_F_UREACH))
        return dyn_sc_check_impl(v, n, inst);   /* nothing to track */
    memset(&rec, 0, sizeof rec);
    v->cur_eval = &rec;
    r = dyn_sc_check_impl(v, n, inst);
    v->cur_eval = saved;
    if (r > 0 && saved && dyn_sc_eval_merge(v->ctx, saved, &rec))
        r = -1;                 /* cap/OOM: fail closed with the exception */
    dyn_sc_eval_free(v->ctx, &rec);
    return r;
}

/* ------------------------------------------------------------ result JS */

static JSValue dyn_sc_result(JSContext *ctx, dyn_scerrs_t *errs, int r)
{
    JSValue obj, arr;
    int i;
    obj = JS_NewObject(ctx);
    if (JS_IsException(obj))
        return obj;
    arr = JS_NewArray(ctx);
    if (JS_IsException(arr)) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    for (i = 0; i < errs->n; i++) {
        JSValue eo, ep, em, ek;
        eo = JS_NewObject(ctx);
        /* JS_NewStringLen: error paths carry raw bytes (a property name can
           contain NUL); a NUL-terminated JS_NewString would truncate */
        ep = JS_NewStringLen(ctx, errs->e[i].path,
                             (size_t)errs->e[i].pathlen);
        em = JS_NewString(ctx, errs->e[i].message);
        ek = JS_NewString(ctx, errs->e[i].keyword);
        if (JS_IsException(eo)) {
            JS_FreeValue(ctx, ep); JS_FreeValue(ctx, em); JS_FreeValue(ctx, ek);
            JS_FreeValue(ctx, arr); JS_FreeValue(ctx, obj);
            return JS_EXCEPTION;
        }
        if (JS_IsException(ep) || JS_IsException(em) || JS_IsException(ek)) {
            JS_FreeValue(ctx, eo);
            JS_FreeValue(ctx, ep); JS_FreeValue(ctx, em); JS_FreeValue(ctx, ek);
            JS_FreeValue(ctx, arr); JS_FreeValue(ctx, obj);
            return JS_EXCEPTION;
        }
        /* Each define CONSUMES its value in all paths (engine convention). */
        if (JS_DefinePropertyValueStr(ctx, eo, "path", ep, JS_PROP_C_W_E) < 0 ||
            JS_DefinePropertyValueStr(ctx, eo, "message", em, JS_PROP_C_W_E) < 0 ||
            JS_DefinePropertyValueStr(ctx, eo, "keyword", ek, JS_PROP_C_W_E) < 0 ||
            JS_DefinePropertyValueUint32(ctx, arr, (uint32_t)i, eo,
                                         JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, arr);
            JS_FreeValue(ctx, obj);
            return JS_EXCEPTION;
        }
    }
    /* The check's own outcome is the truth, not the surviving error count:
       a dropped error slot (allocation failure) or a mismatch the collector
       could not record must never read as valid. */
    JS_DefinePropertyValueStr(ctx, obj, "valid",
                              JS_NewBool(ctx, r == 1 && errs->n == 0 &&
                                              errs->drops == 0),
                              JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, obj, "errors", arr, JS_PROP_C_W_E);
    return obj;
}

/* --------------------------------------------------------- JS boundary */

/* The JS entry point -- the phase-1 walk above owns the name dyn_sc_compile. */
static JSValue dyn_sc_compile_entry(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    JSValue schema;
    dyn_sccomp_t c;
    dyn_schema_t *sch;
    dyn_scnode_t *root = NULL;
    JSValue obj;
    JSRuntime *rt;
    int i;

    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx,
            "Schema.compile(schema) requires a schema");
    if (JS_IsString(argv[0])) {
        const char *s;
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
    if (!JS_IsBool(schema) &&
        (!JS_IsObject(schema) || dyn_sc_is_array(ctx, schema))) {
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

    if (dyn_sc_compile(&c, schema, "", 0, &root) ||
        dyn_sc_resolve_refs(&c) ||
        dyn_sc_compute_ureach(ctx, sch)) {
        for (i = 0; i < c.n_paths; i++)
            js_free(ctx, c.paths[i]);
        js_free(ctx, c.paths);
        js_free(ctx, c.pnode);
        JS_FreeValue(ctx, schema);
        dyn_sc_schema_free_rt(JS_GetRuntime(ctx), sch);
        return JS_EXCEPTION;
    }
    for (i = 0; i < c.n_paths; i++)
        js_free(ctx, c.paths[i]);
    js_free(ctx, c.paths);
    js_free(ctx, c.pnode);
    JS_FreeValue(ctx, schema);

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


static JSValue dyn_sc_validate_impl(JSContext *ctx, dyn_schema_t *sch,
                                    JSValueConst inst)
{
    dyn_vctx_t v;
    dyn_scerrs_t errs;
    JSValue res;
    int r;

    memset(&v, 0, sizeof v);
    v.ctx = ctx;
    v.sch = sch;
    memset(&errs, 0, sizeof errs);
    v.errs = &errs;
    v.eqseen = js_malloc(ctx,
        (DYN_SC_MAX_EQUAL_DEPTH + 1) * sizeof(dyn_sc_pair_t));
    if (!v.eqseen)
        return JS_EXCEPTION;

    r = dyn_sc_check_top(&v, sch->root, inst);
    js_free(ctx, v.eqseen);
    if (r < 0) {
        dyn_sc_errs_free(ctx, &errs);
        js_free(ctx, v.path.buf);
        return JS_EXCEPTION;
    }
    res = dyn_sc_result(ctx, &errs, r);
    dyn_sc_errs_free(ctx, &errs);
    js_free(ctx, v.path.buf);
    return res;
}

static JSValue dyn_sc_proto_validate(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    dyn_schema_t *sch = (dyn_schema_t *)dyn_plain_get(ctx, this_val,
                                                      dyn_schema_class_id);
    if (!sch)
        return JS_EXCEPTION;
    return dyn_sc_validate_impl(ctx, sch,
                                argc > 0 ? argv[0] : JS_UNDEFINED);
}

/* The cache keys are created PER CALL (JS_NewAtom/JS_FreeAtom), never cached
 * in a static: atoms are runtime-scoped, and a static atom minted in the
 * parent runtime is use-after-free garbage inside a Worker's runtime (the
 * review's sc_p5 probe crashes on base because of exactly this). Interning
 * two short strings per Schema.validate call is nothing next to hashing the
 * schema. */
#define DYN_SC_CACHE_ATOM_NAME  "__dyna_compiled_schema"
#define DYN_SC_CACHE_HASH_NAME  "__dyna_compiled_hash"

static JSValue dyn_sc_schema_validate(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    JSValue compiled, res;
    dyn_schema_t *sch;
    JSValueConst a1[1];
    (void)this_val;
    if (argc < 2) {
        return JS_ThrowTypeError(ctx,
            "Schema.validate(schema, instance) requires two arguments");
    }

    /* Fast path 1: argv[0] is already a CompiledSchema instance */
    sch = (dyn_schema_t *)JS_GetOpaque(argv[0], dyn_schema_class_id);
    if (sch)
        return dyn_sc_validate_impl(ctx, sch, argv[1]);

    /* Fast path 2: cached CompiledSchema on the schema object, keyed by a
     * canonical-CBOR fingerprint of the schema itself. An in-place mutation
     * changes the hash and forces a recompile — the old cache validated
     * against stale rules forever. */
    if (JS_IsObject(argv[0])) {
        uint64_t now_hash = 0;
        int hres = dyn_value_hash64(ctx, argv[0], &now_hash);
        int have_hash = (hres == 0);
        /* Hashing refuses cyclic schemas, functions and OOM -- all with an
         * exception PENDING. The failure is not fatal: validation degrades
         * to the uncached path, so the exception must be cleared here or it
         * fires later, inside the compile or the result construction. */
        if (hres < 0)
            JS_FreeValue(ctx, JS_GetException(ctx));
        JSValue cached, hv;
        uint64_t old_hash = 0; int have_old = 0;
        JSAtom cache_atom = JS_NewAtom(ctx, DYN_SC_CACHE_ATOM_NAME);
        JSAtom hash_atom = JS_NewAtom(ctx, DYN_SC_CACHE_HASH_NAME);
        if (cache_atom == JS_ATOM_NULL || hash_atom == JS_ATOM_NULL) {
            if (cache_atom != JS_ATOM_NULL) JS_FreeAtom(ctx, cache_atom);
            if (hash_atom != JS_ATOM_NULL) JS_FreeAtom(ctx, hash_atom);
            return JS_EXCEPTION;
        }
        cached = JS_GetProperty(ctx, argv[0], cache_atom);
        hv = have_hash ? JS_GetProperty(ctx, argv[0], hash_atom)
                       : JS_UNDEFINED;
        if (!JS_IsException(hv) && JS_IsString(hv)) {
            const char *hs = JS_ToCString(ctx, hv);
            if (hs) { old_hash = (uint64_t)strtoull(hs, NULL, 16); have_old = 1; JS_FreeCString(ctx, hs); }
        }
        if (!JS_IsException(hv)) JS_FreeValue(ctx, hv);
        if (!JS_IsException(cached) && !JS_IsUndefined(cached) &&
            have_hash && have_old && old_hash == now_hash) {
            sch = (dyn_schema_t *)JS_GetOpaque(cached, dyn_schema_class_id);
            if (sch) {
                res = dyn_sc_validate_impl(ctx, sch, argv[1]);
                JS_FreeValue(ctx, cached);
                return res;
            }
        }
        JS_FreeValue(ctx, cached);
        /* stale or absent: drop the old cache entry so the recompile below
           replaces it */
        if (!JS_IsException(cached))
            JS_DeleteProperty(ctx, argv[0], cache_atom, 0);
        JS_FreeAtom(ctx, hash_atom);
        JS_FreeAtom(ctx, cache_atom);
    }

    a1[0] = argv[0];
    compiled = dyn_sc_compile_entry(ctx, JS_UNDEFINED, 1, a1);
    if (JS_IsException(compiled))
        return compiled;
    sch = (dyn_schema_t *)JS_GetOpaque(compiled, dyn_schema_class_id);
    res = dyn_sc_validate_impl(ctx, sch, argv[1]);
    if (JS_IsException(res)) {
        /* Never run cache bookkeeping with a pending exception: the hash /
         * define path clobbers rt->current_exception and the caller observed
         * a raw internal tag instead of the thrown error (audit S1). Skip
         * the cache write; the next call recompiles. */
        JS_FreeValue(ctx, compiled);
        return res;
    }

    if (JS_IsObject(argv[0])) {
        uint64_t h2 = 0;
        int hres = dyn_value_hash64(ctx, argv[0], &h2);
        if (hres < 0)
            JS_FreeValue(ctx, JS_GetException(ctx));    /* same contract as above */
        if (hres == 0) {
            JSAtom cache_atom = JS_NewAtom(ctx, DYN_SC_CACHE_ATOM_NAME);
            JSAtom hash_atom = cache_atom != JS_ATOM_NULL
                ? JS_NewAtom(ctx, DYN_SC_CACHE_HASH_NAME) : JS_ATOM_NULL;
            if (cache_atom != JS_ATOM_NULL && hash_atom != JS_ATOM_NULL) {
                char hex[17];
                for (int k = 15; k >= 0; k--) { hex[k] = "0123456789abcdef"[h2 & 15]; h2 >>= 4; }
                hex[16] = 0;
                JS_DefinePropertyValue(ctx, argv[0], hash_atom,
                                       JS_NewString(ctx, hex),
                                       JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE);
                JS_DefinePropertyValue(ctx, argv[0], cache_atom,
                                       JS_DupValue(ctx, compiled),
                                       JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE);
            }
            if (hash_atom != JS_ATOM_NULL) JS_FreeAtom(ctx, hash_atom);
            if (cache_atom != JS_ATOM_NULL) JS_FreeAtom(ctx, cache_atom);
        }
    }

    JS_FreeValue(ctx, compiled);
    return res;
}

/* ------------------------------------------------------------ registry
 * The custom-format registry. Held as a property on the global object (a GC
 * root, per-runtime), created lazily on first registerFormat() -- the same
 * lazy-static-atom idiom the compile cache above uses. Process-wide scope:
 * every compiled schema in the runtime asserts against the SAME registry,
 * live at validate() time. There is deliberately no unregister: formats are
 * few, registered once at startup. */

#define DYN_SC_FMT_NAME_MAX 256
#define DYN_SC_FMTREG_NAME  "__dyna_schema_formats"

/* Fetch the registry object; create+install it when `create` and absent.
 * Returns the (caller-freed) JSValue, JS_UNDEFINED when absent and !create,
 * or JS_EXCEPTION.
 *
 * The key atom is created PER CALL (JS_NewAtom/JS_FreeAtom), never cached in
 * a static: atoms are runtime-scoped, and a static atom minted in the parent
 * runtime is use-after-free garbage inside a Worker's runtime (found by the
 * review's parent+Worker probe; the compile cache below had the same class
 * and was converted too). Interning one short string per registry access is
 * nothing next to validate()'s cost, and the registry object itself lives on
 * the global object, so it is a GC root in exactly the runtime that created
 * it -- parent and Worker each own a private registry by construction. */
static JSValue dyn_sc_fmt_registry(JSContext *ctx, int create)
{
    JSValue g, reg;
    JSAtom a = JS_NewAtom(ctx, DYN_SC_FMTREG_NAME);

    if (a == JS_ATOM_NULL)
        return JS_EXCEPTION;
    g = JS_GetGlobalObject(ctx);
    reg = JS_GetProperty(ctx, g, a);
    if (JS_IsException(reg) || !JS_IsObject(reg)) {
        JS_FreeValue(ctx, reg);
        reg = JS_UNDEFINED;
        if (create) {
            reg = JS_NewObject(ctx);
            if (JS_IsException(reg))
                goto out;
            /* non-enumerable on globalThis; held alive by the global */
            if (JS_DefinePropertyValue(ctx, g, a,
                                       JS_DupValue(ctx, reg),
                                       JS_PROP_CONFIGURABLE) < 0) {
                JS_FreeValue(ctx, reg);
                reg = JS_EXCEPTION;
            }
        }
    }
 out:
    JS_FreeAtom(ctx, a);
    JS_FreeValue(ctx, g);
    return reg;
}

/* Look up `name` in the registry. Returns the (caller-freed) JSValue:
 * the function, JS_UNDEFINED when the name is unregistered, or
 * JS_EXCEPTION. */
static JSValue dyn_sc_fmt_lookup(JSContext *ctx, const char *name)
{
    JSValue reg, fn;
    JSAtom a;

    reg = dyn_sc_fmt_registry(ctx, 0);
    if (JS_IsException(reg) || JS_IsUndefined(reg))
        return reg;
    a = JS_NewAtom(ctx, name);
    if (a == JS_ATOM_NULL) {
        JS_FreeValue(ctx, reg);
        return JS_EXCEPTION;
    }
    fn = JS_GetProperty(ctx, reg, a);
    JS_FreeAtom(ctx, a);
    JS_FreeValue(ctx, reg);
    return fn;
}

/* Own-property existence (INFO fix): JS_HasProperty walks the prototype
 * chain, so the name "toString" answered "already registered" from
 * Object.prototype -- misleading, and it also blocks a legal name. */
static int dyn_sc_fmt_has_own(JSContext *ctx, JSValueConst obj, JSAtom a)
{
    JSPropertyDescriptor d;
    int has = JS_GetOwnProperty(ctx, &d, obj, a);
    if (has == 1 && !(d.flags & JS_PROP_GETSET))
        JS_FreeValue(ctx, d.value);
    return has;
}

/* Schema.registerFormat(name, fn): wire a custom "format" validator into
 * every subsequently-validated schema whose "format" keyword names it.
 * Strict: `name` must be a non-empty, NUL-free string (bounded at 256
 * bytes); `fn` must be a function; a duplicate name throws (the registry is
 * immutable once set -- assert an intentional replacement explicitly via a
 * delete if ever needed). */
static JSValue dyn_sc_register_format(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    const char *name;
    size_t nl;
    JSValue reg;
    JSAtom a = JS_ATOM_NULL;
    JSValue ret = JS_UNDEFINED;

    (void)this_val;
    if (argc < 2 || !JS_IsString(argv[0]) || !JS_IsFunction(ctx, argv[1]))
        return JS_ThrowTypeError(ctx,
            "Schema.registerFormat: expected (name: string, fn: function)");
    name = JS_ToCStringLen(ctx, &nl, argv[0]);
    if (!name)
        return JS_EXCEPTION;
    if (nl == 0 || nl > DYN_SC_FMT_NAME_MAX || strlen(name) != nl) {
        JS_ThrowTypeError(ctx,
            "Schema.registerFormat: name must be non-empty, NUL-free, "
            "and at most %d bytes", DYN_SC_FMT_NAME_MAX);
        JS_FreeCString(ctx, name);
        return JS_EXCEPTION;
    }
    reg = dyn_sc_fmt_registry(ctx, 1);
    if (JS_IsException(reg)) {
        JS_FreeCString(ctx, name);
        return reg;
    }
    a = JS_NewAtomLen(ctx, name, nl);
    if (a == JS_ATOM_NULL) {
        JS_FreeCString(ctx, name);
        JS_FreeValue(ctx, reg);
        return JS_EXCEPTION;
    }
    {
        int has = dyn_sc_fmt_has_own(ctx, reg, a);
        if (has < 0) {
            ret = JS_EXCEPTION;
        } else if (has) {
            JS_ThrowTypeError(ctx,
                "Schema.registerFormat: format \"%s\" is already registered",
                name);
            ret = JS_EXCEPTION;
        } else if (JS_SetProperty(ctx, reg, a, JS_DupValue(ctx, argv[1])) < 0) {
            ret = JS_EXCEPTION;
        }
    }
    JS_FreeAtom(ctx, a);
    JS_FreeCString(ctx, name);
    JS_FreeValue(ctx, reg);
    return ret;
}

/* ------------------------------------------------------------ module glue */

static const JSCFunctionListEntry dyn_sc_proto_funcs[] = {
    JS_CFUNC_DEF("validate", 1, dyn_sc_proto_validate),
};

static const JSCFunctionListEntry dyn_sc_schema_funcs[] = {
    JS_CFUNC_DEF("compile", 1, dyn_sc_compile_entry),
    JS_CFUNC_DEF("validate", 2, dyn_sc_schema_validate),
    JS_CFUNC_DEF("registerFormat", 2, dyn_sc_register_format),
};

static const JSCFunctionListEntry dyn_sc_module_funcs[] = {
    JS_OBJECT_DEF("Schema", dyn_sc_schema_funcs, countof(dyn_sc_schema_funcs),
                  JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE),
};

static const JSClassDef dyn_sc_class = {
    "CompiledSchema",
    .finalizer = dyn_sc_schema_finalizer,
    .gc_mark = dyn_sc_schema_mark,
};

static int dyn_sc_init_module(JSContext *ctx, JSModuleDef *m)
{
    return JS_SetModuleExportList(ctx, m, dyn_sc_module_funcs,
                                  countof(dyn_sc_module_funcs));
}

int js_nat_init_schema(JSContext *ctx)
{
    JSModuleDef *m;
    JSValue proto;

    m = JS_NewCModule(ctx, "dyna:schema", dyn_sc_init_module);
    if (!m)
        return -1;
    if (JS_AddModuleExportList(ctx, m, dyn_sc_module_funcs,
                               countof(dyn_sc_module_funcs)) < 0)
        return -1;

    if (!JS_IsRegisteredClass(JS_GetRuntime(ctx), dyn_schema_class_id)) {
        JS_NewClassID(&dyn_schema_class_id);
        if (JS_NewClass(JS_GetRuntime(ctx), dyn_schema_class_id, &dyn_sc_class) < 0)
            return -1;
    }
    proto = JS_NewObject(ctx);
    if (JS_IsException(proto))
        return -1;
    JS_SetPropertyFunctionList(ctx, proto, dyn_sc_proto_funcs,
                               countof(dyn_sc_proto_funcs));
    JS_SetClassProto(ctx, dyn_schema_class_id, proto);
    return 0;
}

#endif /* CONFIG_NATIVE_MODULES && CONFIG_NATIVE_MODULE_SCHEMA */
