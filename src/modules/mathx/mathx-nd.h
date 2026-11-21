#ifndef MATHX_ND_H
#define MATHX_ND_H

#include "dyna-nat.h"
#include "core/dyn-lak.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_MATHX)

extern JSClassID dyna_nd_class_id;

int dyna_nd_ensure_class(JSContext* ctx);
int dyna_nd_view(JSContext* ctx, JSValueConst v, dyn_nd_t* out);
int dyna_nd_writable(JSContext* ctx, JSValueConst v);
JSValue dyna_nd_new(JSContext* ctx, int dtype, int ndim, const int64_t* shape, void** data);
JSValue dyna_nd_take(JSContext* ctx, int dtype, int ndim, const int64_t* shape, void* data);
JSValue dyna_nd_wrap(JSContext* ctx, JSValueConst buffer, const dyn_nd_t* view);
JSValue dyna_nd_from_value(JSContext* ctx, JSValueConst v, int dtype, dyn_nd_t* out);
int dyna_nd_parse_dtype(JSContext* ctx, JSValueConst v, int dflt);
const char* dyna_nd_dtype_name(int dtype);
JSValue dyna_nd_ctor_value(JSContext* ctx);

#endif
#endif
