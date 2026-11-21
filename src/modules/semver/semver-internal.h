#ifndef SEMVER_INTERNAL_H
#define SEMVER_INTERNAL_H

#include "dyna-nat.h"
#include "dtoa.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_SEMVER)

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "core/dyn-semverk.h"

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

static inline int sv_u64toa(uint64_t v, char* buf)
{
    return (int)u64toa(buf, v);
}

JSValue sv_build_version(JSContext* ctx, const SemVer* v);

JSValue sv_pre_array(JSContext* ctx, const SemVer* v);

static inline const char* sv_arg_str(JSContext* ctx, int argc, JSValueConst* argv,
    size_t* plen)
{
    if (argc < 1 || !JS_IsString(argv[0])) {
        JS_ThrowTypeError(ctx, "dyna:semver: argument must be a string");
        return NULL;
    }
    return JS_ToCStringLen(ctx, plen, argv[0]);
}

JSValue dyn_semver_parse(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

int sv_two(JSContext* ctx, int argc, JSValueConst* argv, int* cmp);

JSValue dyn_semver_compare(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

int sv_two_versions(JSContext* ctx, int argc, JSValueConst* argv,
    SemVer* a, SemVer* b,
    const char** pas, const char** pbs);

JSValue dyn_semver_compare_build(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

int sv_range_arg(JSContext* ctx, JSValueConst v, const Range** out,
    const char** powned, int* powned_flag, Range** pheap);

int sv_read_versions(JSContext* ctx, JSValueConst arr, JSValue** pelems,
    const char*** pcstrs, SemVer** psvs, uint32_t* pcount,
    int allow_invalid);

void sv_free_versions(JSContext* ctx, JSValue* elems,
    const char** cstrs, SemVer* svs, uint32_t count);

JSValue dyn_semver_sort(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_semver_bestsat(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int want_max);

JSValue dyn_semver_inc(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_semver_coerce(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

extern JSClassID dyn_range_class_id;

void dyn_range_finalizer(JSRuntime* rt, JSValue val);

JSValue dyn_range_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv);

JSValue dyn_range_test(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_range_bestsat(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int want_max);

JSValue dyn_range_filter(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_range_get_source(JSContext* ctx, JSValueConst this_val);

JSValue dyn_range_intersects(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);

JSValue dyn_range_get_set_count(JSContext* ctx, JSValueConst this_val);

#endif

#endif
