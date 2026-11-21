#ifndef HTML_INTERNAL_H
#define HTML_INTERNAL_H

#include "dyna-nat.h"
#include "core/dyn-common.h"
#include "core/dyn-simdk.h"
#include "core/dyn-htmlkit.h"
#include "core/dyn-markdown.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_HTML)

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static inline int ht_len(JSContext* ctx, JSValue lv, int64_t* len)
{
    int rc = 0;
    if (JS_IsException(lv) || JS_ToInt64(ctx, len, lv) < 0)
        rc = -1;
    else if (*len > (int64_t)HT_MAX_INPUT) {
        JS_ThrowRangeError(ctx, "html: a node list of %lld entries is larger than any document", (long long)*len);
        rc = -1;
    }
    JS_FreeValue(ctx, lv);
    return rc;
}

typedef struct {
    JSContext* ctx;
    const char* s;
    size_t n, i;
    hb_t text, abuf;
    ht_attr_t attr[HT_MAX_ATTRS];
    uint32_t nattr;
    const char* tag;
    size_t taglen;
    int selfclose;
    JSValue stack[HT_MAX_DEPTH];
    const char* nptr[HT_MAX_DEPTH];
    size_t nlen[HT_MAX_DEPTH];
    uint32_t counts[HT_MAX_DEPTH];
    int depth;
    char err[160];
    JSAtom el_atoms[3];
    JSShapeCache el_shape, at_shape;
} ht_t;

JSValue ht_new_attrs(ht_t* p);

JSValue ht_parse_into(JSContext* ctx, const char* s, size_t n);

JSValue dyn_html_parse(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

enum { HT_K_NAME,
    HT_K_ATTRS,
    HT_K_CHILDREN,
    HT_K_LENGTH,
    HT_K_COUNT };

typedef struct {
    JSAtom a[HT_K_COUNT];
    JSPropCache pc[HT_K_COUNT];
} ht_keys_t;

static inline int ht_keys_init(JSContext* ctx, ht_keys_t* k)
{
    static const char* const names[HT_K_COUNT] = { "name", "attrs", "children", "length" };
    int i;
    memset(k, 0, sizeof *k);
    for (i = 0; i < HT_K_COUNT; i++) {
        k->a[i] = JS_NewAtom(ctx, names[i]);
        if (k->a[i] == JS_ATOM_NULL)
            return -1;
    }
    return 0;
}

static inline void ht_keys_free(JSContext* ctx, ht_keys_t* k)
{
    int i;
    for (i = 0; i < HT_K_COUNT; i++) {
        if (k->a[i] != JS_ATOM_NULL)
            JS_FreeAtom(ctx, k->a[i]);
        JS_FreePropCache(JS_GetRuntime(ctx), &k->pc[i]);
    }
}

static inline JSValue ht_get(JSContext* ctx, ht_keys_t* k, JSValueConst obj, int key)
{
    return JS_GetPropertyCached(ctx, obj, k->a[key], &k->pc[key]);
}

typedef struct {
    JSContext* ctx;
    hb_t b;
    int depth;
    ht_keys_t k;
} ht_ser_t;

int ht_ser_node(ht_ser_t* w, JSValueConst v);

extern JSClassID dyn_sel_class_id;

void dyn_sel_finalizer(JSRuntime* rt, JSValue val);

JSValue dyn_sel_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv);

JSValue dyn_sel_query(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic);

extern JSClassID dyn_san_class_id;

void dyn_san_finalizer(JSRuntime* rt, JSValue val);

JSValue dyn_san_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv);

JSValue dyn_san_clean(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_md_render(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

extern JSClassID dyn_tpl_class_id;

extern const JSClassDef dyn_tpl_class;

JSValue dyn_tpl_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv);

extern const JSCFunctionListEntry dyn_tpl_proto[1];

JSValue dyn_html_rewrite_links(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

#endif

#endif
