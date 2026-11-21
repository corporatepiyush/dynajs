#ifndef YAML_INTERNAL_H
#define YAML_INTERNAL_H

#include "dyna-nat.h"
#include "core/dyn-common.h"
#include "cutils.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_YAML)

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "core/dyn-sb.h"
#include "core/dyn-yamlscalar.h"
#include "core/dyn-simdk.h"

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

#define YML_MAX_DEPTH 128
#define YML_SHAPE_DEPTH 8
#define YML_MAP_BATCH 16

typedef struct {
    size_t raw, start, end;
    int indent;
    int blank;
    int tab;
} yml_line_t;

typedef struct {
    JSContext* ctx;
    const char* s;
    size_t n;
    yml_line_t* ln;
    uint32_t nln, li;
    int depth;
    int max_depth;
    int bigint;
    char err[192];
    JSShapeCache shapes[YML_SHAPE_DEPTH];
} yml_t;

static inline void yml_shapes_free(JSRuntime* rt, yml_t* p)
{
    int k;
    for (k = 0; k < YML_SHAPE_DEPTH; k++)
        JS_FreeShapeCache(rt, &p->shapes[k]);
}

static inline JSValue yml_fail(yml_t* p, const char* what)
{
    uint32_t line = p->li < p->nln ? p->li + 1 : p->nln;
    if (!p->err[0])
        snprintf(p->err, sizeof p->err, "%s at line %u", what, (unsigned)line);
    return JS_EXCEPTION;
}

static inline int yml_tab_refuse(yml_t* p, uint32_t li)
{
    if (li < p->nln && p->ln[li].tab) {
        p->li = li;
        yml_fail(p, "a tab may not indent a line");
        return 1;
    }
    return 0;
}

static inline void yml_skip_blank(yml_t* p)
{
    while (p->li < p->nln && p->ln[p->li].blank)
        p->li++;
}

int yml_marker_at(yml_t* p, uint32_t li, const char* m);

JSValue yml_scalar(yml_t* p, const char* s, size_t n);

JSValue yml_quoted(yml_t* p, size_t* pi, size_t end, int multiline);

JSValue yml_flow(yml_t* p, size_t* pi, size_t end);

typedef struct {
    JSAtom atoms[YML_MAP_BATCH];
    JSValue vals[YML_MAP_BATCH];
    int n;
    JSValue out;
} yml_kv_t;

void yml_kv_abort(yml_t* p, yml_kv_t* b);

int yml_kv_add(yml_t* p, yml_kv_t* b, JSAtom key, JSValue v);

JSValue yml_kv_finish(yml_t* p, yml_kv_t* b);

JSValue yml_block_scalar(yml_t* p, int parent_indent, size_t st, size_t end);

JSValue yml_node(yml_t* p, int min_indent);

JSValue yml_inline_value(yml_t* p, int indent, size_t st, size_t end);

JSValue yml_map(yml_t* p, int indent);

static inline int yml_marker_tail_ok(yml_t* p, uint32_t li)
{
    const yml_line_t* L = &p->ln[li];
    size_t vs = L->start + 3;
    while (vs < L->end && (p->s[vs] == ' ' || p->s[vs] == '\t'))
        vs++;
    return vs >= L->end || p->s[vs] == '#';
}

static inline int yml_at_doc_start(yml_t* p)
{
    return yml_marker_at(p, p->li, "---");
}

static inline int yml_at_doc_end(yml_t* p)
{
    return yml_marker_at(p, p->li, "...");
}

JSValue yml_one(yml_t* p);

JSValue yml_parse(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic);

typedef struct {
    JSPropertyEnum e;
    char* name;
} yml_prop_t;

int yml_props(JSContext* ctx, JSValueConst v, int sort,
    yml_prop_t** out, uint32_t* outn);

void yml_props_free(JSContext* ctx, yml_prop_t* t, uint32_t n);

int yml_is_block(JSContext* ctx, JSValueConst v, int64_t* plen);

JSValue yml_stringify(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

typedef struct {
    yml_t p;
    char* buf;
    JSContext* ctx;
    uint32_t ndocs;
    int done, dead;
    char err[192];
    JSValue err_val;
} ystream_t;

extern JSClassID ystream_class_id;

void ystream_free(JSRuntime* rt, ystream_t* st);

void ystream_finalizer(JSRuntime* rt, JSValue val);

JSValue ystream_next(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue ystream_return(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue ystream_iterator(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue yml_parse_stream(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

#endif

#endif
