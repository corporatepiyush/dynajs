#ifndef XML_INTERNAL_H
#define XML_INTERNAL_H

#include "dyna-nat.h"
#include "core/dyn-common.h"
#include "core/dyn-simdk.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_XML)

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "core/dyn-sb.h"
#include "core/dyn-xmlscan.h"

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

JSObjectTemplate* xml_element_template(JSContext* ctx);

JSValue xml_new_element(JSContext* ctx, const xml_ev_t* ev, JSValue* children,
    JSObjectTemplate* tpl);

typedef struct {
    int trim, keep_entities, multiple;
} xml_opts_t;

int xml_read_opts(JSContext* ctx, JSValueConst o, xml_opts_t* out);

JSValue xml_parse(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

typedef struct {
    JSAtom name, attrs, children, length, text;
} xml_atoms_t;

int xml_atoms_init(JSContext* ctx, xml_atoms_t* a);

void xml_atoms_free(JSContext* ctx, xml_atoms_t* a);

typedef struct {
    JSContext* ctx;
    xb_t out;
    int indent, depth, err;
    xml_atoms_t at;
} xml_ser_t;

int xml_ser_node(xml_ser_t* w, JSValueConst node);

extern JSClassID dyn_xw_class_id;

void xml_writer_finalizer(JSRuntime* rt, JSValue val);

JSValue xml_writer_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv);

JSValue xml_writer_open(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue xml_writer_text(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue xml_writer_close(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue xml_writer_to_string(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue xml_writer_finish(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

extern JSClassID dyn_sax_class_id;

void dyn_sax_finalizer(JSRuntime* rt, JSValue val);

void dyn_sax_gc_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark);

JSValue dyn_sax_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv);

JSValue dyn_sax_next(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_sax_feed(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic);

JSValue xml_collapse(JSContext* ctx, const xml_atoms_t* at, xb_t* text, JSValueConst el, int depth);

#define DYN_SAX_GEN_PROP "\x02" \
                         "iterateGen"

JSValue dyn_sax_iterate(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

#endif

#endif
