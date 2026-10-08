
#ifndef DYNAJS_UNITY_DECLS_H
#define DYNAJS_UNITY_DECLS_H

static int JS_InitAtoms(JSRuntime* rt);
static JSAtom __JS_NewAtomInit(JSRuntime* rt, const char* str, int len,
    int atom_type);
static void JS_FreeAtomStruct(JSRuntime* rt, JSAtomStruct* p);
static JSValue js_call_c_function(JSContext* ctx, JSValueConst func_obj,
    JSValueConst this_obj,
    int argc, JSValueConst* argv, int flags);
static JSValue js_call_bound_function(JSContext* ctx, JSValueConst func_obj,
    JSValueConst this_obj,
    int argc, JSValueConst* argv, int flags);
static JSValue JS_CallInternal(JSContext* ctx, JSValueConst func_obj,
    JSValueConst this_obj, JSValueConst new_target,
    int argc, JSValue* argv, int flags);
static JSValue JS_CallConstructorInternal(JSContext* ctx,
    JSValueConst func_obj,
    JSValueConst new_target,
    int argc, JSValue* argv, int flags);
static JSValue JS_InvokeFree(JSContext* ctx, JSValue this_val, JSAtom atom,
    int argc, JSValueConst* argv);
static __maybe_unused void JS_DumpAtoms(JSRuntime* rt);
static __maybe_unused void JS_DumpString(JSRuntime* rt, const JSString* p);
static __maybe_unused void JS_DumpObjectHeader(JSRuntime* rt);
static __maybe_unused void JS_DumpObject(JSRuntime* rt, JSObject* p);
static __maybe_unused void JS_DumpGCObject(JSRuntime* rt, JSGCObjectHeader* p);
static __maybe_unused void JS_DumpAtom(JSContext* ctx, const char* str, JSAtom atom);
static __maybe_unused void JS_DumpValueRT(JSRuntime* rt, const char* str, JSValueConst val);
static __maybe_unused void JS_DumpValue(JSContext* ctx, const char* str, JSValueConst val);
static __maybe_unused void JS_DumpShapes(JSRuntime* rt);
static void js_dump_value_write(void* opaque, const char* buf, size_t len);
static void js_array_finalizer(JSRuntime* rt, JSValue val);
static void js_array_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark_func);
static void js_mapped_arguments_finalizer(JSRuntime* rt, JSValue val);
static void js_mapped_arguments_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark_func);
static void js_object_data_finalizer(JSRuntime* rt, JSValue val);
static void js_object_data_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark_func);
static void js_c_function_finalizer(JSRuntime* rt, JSValue val);
static void js_c_function_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark_func);
static void js_bytecode_function_finalizer(JSRuntime* rt, JSValue val);
static void js_bytecode_function_mark(JSRuntime* rt, JSValueConst val,
    JS_MarkFunc* mark_func);
static void js_bound_function_finalizer(JSRuntime* rt, JSValue val);
static void js_bound_function_mark(JSRuntime* rt, JSValueConst val,
    JS_MarkFunc* mark_func);
static void js_for_in_iterator_finalizer(JSRuntime* rt, JSValue val);
static void js_for_in_iterator_mark(JSRuntime* rt, JSValueConst val,
    JS_MarkFunc* mark_func);
static void js_regexp_finalizer(JSRuntime* rt, JSValue val);
static void js_array_buffer_finalizer(JSRuntime* rt, JSValue val);
static void js_typed_array_finalizer(JSRuntime* rt, JSValue val);
static void js_typed_array_mark(JSRuntime* rt, JSValueConst val,
    JS_MarkFunc* mark_func);
static void js_proxy_finalizer(JSRuntime* rt, JSValue val);
static void js_proxy_mark(JSRuntime* rt, JSValueConst val,
    JS_MarkFunc* mark_func);
static void js_map_finalizer(JSRuntime* rt, JSValue val);
static void js_map_mark(JSRuntime* rt, JSValueConst val,
    JS_MarkFunc* mark_func);
static void js_map_iterator_finalizer(JSRuntime* rt, JSValue val);
static void js_map_iterator_mark(JSRuntime* rt, JSValueConst val,
    JS_MarkFunc* mark_func);
static void js_iterator_concat_finalizer(JSRuntime* rt, JSValue val);
static void js_iterator_zip_finalizer(JSRuntime* rt, JSValue val);
static void js_iterator_zip_mark(JSRuntime* rt, JSValueConst val,
    JS_MarkFunc* mark_func);
static void js_iterator_concat_mark(JSRuntime* rt, JSValueConst val,
    JS_MarkFunc* mark_func);
static void js_iterator_helper_finalizer(JSRuntime* rt, JSValue val);
static void js_iterator_helper_mark(JSRuntime* rt, JSValueConst val,
    JS_MarkFunc* mark_func);
static void js_regexp_string_iterator_finalizer(JSRuntime* rt, JSValue val);
static void js_regexp_string_iterator_mark(JSRuntime* rt, JSValueConst val,
    JS_MarkFunc* mark_func);
static void js_generator_finalizer(JSRuntime* rt, JSValue obj);
static void js_generator_mark(JSRuntime* rt, JSValueConst val,
    JS_MarkFunc* mark_func);
static void js_global_object_finalizer(JSRuntime* rt, JSValue obj);
static void js_global_object_mark(JSRuntime* rt, JSValueConst val,
    JS_MarkFunc* mark_func);
static void js_promise_finalizer(JSRuntime* rt, JSValue val);
static void js_promise_mark(JSRuntime* rt, JSValueConst val,
    JS_MarkFunc* mark_func);
static void js_promise_resolve_function_finalizer(JSRuntime* rt, JSValue val);
static void js_promise_resolve_function_mark(JSRuntime* rt, JSValueConst val,
    JS_MarkFunc* mark_func);
static JSValue JS_ToPrimitiveFree(JSContext* ctx, JSValue val, int hint);
static int JS_ToInt32Free(JSContext* ctx, int32_t* pres, JSValue val);
static int JS_ToUint8ClampFree(JSContext* ctx, int32_t* pres, JSValue val);
static JSValue js_compile_regexp(JSContext* ctx, JSValueConst pattern,
    JSValueConst flags);
static JSValue JS_NewRegexp(JSContext* ctx, JSValue pattern, JSValue bc);
static void gc_decref(JSRuntime* rt);
static int JS_NewClass1(JSRuntime* rt, JSClassID class_id,
    const JSClassDef* class_def, JSAtom name);
static BOOL js_strict_eq(JSContext* ctx, JSValueConst op1, JSValueConst op2);
static BOOL js_same_value_zero(JSContext* ctx, JSValueConst op1, JSValueConst op2);
static int JS_ToBigInt64Free(JSContext* ctx, int64_t* pres, JSValue val);
static JSValue JS_ThrowTypeErrorRevokedProxy(JSContext* ctx);
static int js_resolve_proxy(JSContext* ctx, JSValueConst* pval, int throw_exception);
static int JS_CreateProperty(JSContext* ctx, JSObject* p,
    JSAtom prop, JSValueConst val,
    JSValueConst getter, JSValueConst setter,
    int flags);
static int js_string_memcmp(const JSString* p1, int pos1, const JSString* p2,
    int pos2, int len);
static BOOL array_buffer_is_resizable(const JSArrayBuffer* abuf);
static JSValue js_typed_array_constructor_ta(JSContext* ctx,
    JSValueConst new_target,
    JSValueConst src_obj,
    int classid, uint32_t len);
static JSVarRef* get_var_ref(JSContext* ctx, JSStackFrame* sf, int var_idx,
    BOOL is_arg);
static void __async_func_free(JSRuntime* rt, JSAsyncFunctionState* s);
static void async_func_free(JSRuntime* rt, JSAsyncFunctionState* s);
static JSValue js_generator_function_call(JSContext* ctx, JSValueConst func_obj,
    JSValueConst this_obj,
    int argc, JSValueConst* argv,
    int flags);
static void js_async_function_resolve_finalizer(JSRuntime* rt, JSValue val);
static void js_async_function_resolve_mark(JSRuntime* rt, JSValueConst val,
    JS_MarkFunc* mark_func);
static JSValue js_new_promise_capability(JSContext* ctx,
    JSValue* resolving_funcs,
    JSValueConst ctor);
static BOOL js_string_eq(JSContext* ctx,
    const JSString* p1, const JSString* p2);
static JSValue JS_ToNumber(JSContext* ctx, JSValueConst val);
static int JS_NumberIsInteger(JSContext* ctx, JSValueConst val);
static BOOL JS_NumberIsNegativeOrMinusZero(JSContext* ctx, JSValueConst val);
static JSValue JS_ToNumberFree(JSContext* ctx, JSValue val);
static int JS_AddIntrinsicBasicObjects(JSContext* ctx);
static void js_free_shape_null(JSRuntime* rt, JSShape* sh);
static int init_shape_hash(JSRuntime* rt);
static void js_c_function_data_finalizer(JSRuntime* rt, JSValue val);
static void js_c_function_data_mark(JSRuntime* rt, JSValueConst val,
    JS_MarkFunc* mark_func);
static JSValue js_c_function_data_call(JSContext* ctx, JSValueConst func_obj,
    JSValueConst this_val,
    int argc, JSValueConst* argv, int flags);
static JSAtom js_symbol_to_atom(JSContext* ctx, JSValue val);
static JSValue js_instantiate_prototype(JSContext* ctx, JSObject* p, JSAtom atom, void* opaque);
static void map_delete_weakrefs(JSRuntime* rt, JSWeakRefHeader* wh);
static void weakref_delete_weakref(JSRuntime* rt, JSWeakRefHeader* wh);
static void finrec_delete_weakref(JSRuntime* rt, JSWeakRefHeader* wh);
static void JS_RunGCInternal(JSRuntime* rt, BOOL remove_weak_objects);
static JSValue js_array_from_iterator(JSContext* ctx, uint32_t* plen,
    JSValueConst obj, JSValueConst method);
static JSValue js_regexp_toString(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv);
static JSValue get_date_string(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic);
static JSVarRef* js_global_object_find_uninitialized_var(JSContext* ctx, JSObject* p,
    JSAtom atom, BOOL is_lexical);
static int typed_array_init(JSContext* ctx, JSValueConst obj,
    JSValue buffer, uint64_t offset, uint64_t len,
    BOOL track_rab);
static BOOL js_ctor_presize_proto_safe(JSObject* proto, JSCtorPresize* cp);

#endif
