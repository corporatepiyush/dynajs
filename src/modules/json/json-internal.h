#ifndef JSON_INTERNAL_H
#define JSON_INTERNAL_H

#include "dyna-nat.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_JSON)

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

#define DYN_JP_MAX_PTR 65536

JSValue dyn_jp_clone(JSContext* ctx, JSValueConst v, int depth);

typedef struct {
    uint64_t idx;
    uint64_t alen;
} dyn_jp_walk_t;

enum {
    JP_OK = 0,
    JP_MISSING,
    JP_SCALAR,
    JP_BADESC,
    JP_LEADZERO,
    JP_TOOBIG,
    JP_NOTNUM,
    JP_RANGE,
    JP_APPEND,
    JP_SYNTAX,
    JP_TOODEEP,
};

JSValue dyn_jp_throw(JSContext* ctx, const char* pfx, int code,
    const dyn_jp_walk_t* w, const char* p, size_t n);

int dyn_jp_walk(JSContext* ctx, JSValueConst root, const char* p,
    size_t n, int keep_last, char* tok, size_t* tlen,
    JSValue* out, dyn_jp_walk_t* w);

int dyn_jp_add_at(JSContext* ctx, JSValue* work, const char* pfx,
    const char* path, size_t plen, JSValue vadd);

int dyn_jp_remove_at(JSContext* ctx, JSValue* work, const char* pfx,
    const char* path, size_t plen);

extern const JSCFunctionListEntry dyn_ndjson_funcs[2];

extern const JSCFunctionListEntry dyn_jp_pointer_funcs[6];

extern const JSCFunctionListEntry dyn_jp_funcs[3];

int dyn_jp_init_module(JSContext* ctx, JSModuleDef* m);

#endif

#endif
