/* BigInt */

static JSValue JS_ToBigIntCtorFree(JSContext *ctx, JSValue val)
{
    uint32_t tag;

 redo:
    tag = JS_VALUE_GET_NORM_TAG(val);
    switch(tag) {
    case JS_TAG_INT:
    case JS_TAG_BOOL:
        val = JS_NewBigInt64(ctx, JS_VALUE_GET_INT(val));
        break;
    case JS_TAG_SHORT_BIG_INT:
    case JS_TAG_BIG_INT:
        break;
    case JS_TAG_FLOAT64:
        {
            double d = JS_VALUE_GET_FLOAT64(val);
            JSBigInt *r;
            int res;
            r = js_bigint_from_float64(ctx, &res, d);
            if (!r) {
                if (res == 0) {
                    val = JS_EXCEPTION;
                } else if (res == 1) {
                    val = JS_ThrowRangeError(ctx, "cannot convert to BigInt: not an integer");
                } else {
                    val = JS_ThrowRangeError(ctx, "cannot convert NaN or Infinity to BigInt");                }
            } else {
                val = JS_CompactBigInt(ctx, r);
            }
        }
        break;
    case JS_TAG_STRING:
    case JS_TAG_STRING_ROPE:
        val = JS_StringToBigIntErr(ctx, val);
        break;
    case JS_TAG_OBJECT:
        val = JS_ToPrimitiveFree(ctx, val, HINT_NUMBER);
        if (JS_IsException(val))
            break;
        goto redo;
    case JS_TAG_NULL:
    case JS_TAG_UNDEFINED:
    default:
        JS_FreeValue(ctx, val);
        return JS_ThrowTypeError(ctx, "cannot convert to BigInt");
    }
    return val;
}

static JSValue js_bigint_constructor(JSContext *ctx,
                                     JSValueConst new_target,
                                     int argc, JSValueConst *argv)
{
    if (!JS_IsUndefined(new_target))
        return JS_ThrowTypeErrorNotAConstructor(ctx, new_target);
    return JS_ToBigIntCtorFree(ctx, JS_DupValue(ctx, argv[0]));
}

static JSValue js_thisBigIntValue(JSContext *ctx, JSValueConst this_val)
{
    if (JS_IsBigInt(ctx, this_val))
        return JS_DupValue(ctx, this_val);

    if (JS_VALUE_GET_TAG(this_val) == JS_TAG_OBJECT) {
        JSObject *p = JS_VALUE_GET_OBJ(this_val);
        if (p->class_id == JS_CLASS_BIG_INT) {
            if (JS_IsBigInt(ctx, p->u.object_data))
                return JS_DupValue(ctx, p->u.object_data);
        }
    }
    return JS_ThrowTypeError(ctx, "not a BigInt");
}

static JSValue js_bigint_toString(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    JSValue val;
    int base;
    JSValue ret;

    val = js_thisBigIntValue(ctx, this_val);
    if (JS_IsException(val))
        return val;
    if (argc == 0 || JS_IsUndefined(argv[0])) {
        base = 10;
    } else {
        base = js_get_radix(ctx, argv[0]);
        if (base < 0)
            goto fail;
    }
    ret = js_bigint_to_string1(ctx, val, base);
    JS_FreeValue(ctx, val);
    return ret;
 fail:
    JS_FreeValue(ctx, val);
    return JS_EXCEPTION;
}

static JSValue js_bigint_valueOf(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    return js_thisBigIntValue(ctx, this_val);
}

/* Limb i of `a`, sign-extended past its length. `a` is a short or a heap
   BigInt; both are two's complement, so a negative value's limbs above its
   length are all ones. */
static js_limb_t js_bigint_limb_ext(JSValueConst a, uint64_t i)
{
    if (JS_VALUE_GET_TAG(a) == JS_TAG_SHORT_BIG_INT) {
        js_slimb_t v = (js_slimb_t)JS_VALUE_GET_SHORT_BIG_INT(a);
        return i == 0 ? (js_limb_t)v : (js_limb_t)(v >> (JS_LIMB_BITS - 1));
    } else {
        const JSBigInt *p = JS_VALUE_GET_PTR(a);
        if (i < (uint64_t)p->len)
            return p->tab[i];
        return js_bigint_sign(p) ? (js_limb_t)-1 : 0;
    }
}

/* BigInt.asUintN(bits, a) = a mod 2^bits, for ANY sign of `a`. The result is
   non-negative by definition, so it needs one limb beyond `bits` to keep its
   sign bit clear -- and that is the whole bug: both old paths could produce a
   NEGATIVE BigInt, which asUintN can never yield. Returning the operand
   unchanged made asUintN(64,-1n) be -1n, and the truncation path made
   asUintN(64, 2n**63n) be -2^63 because the top bit of a one-limb result IS
   the sign. Computing it here for every input is correct by construction
   rather than by case analysis. */
static JSValue js_bigint_as_uintn(JSContext *ctx, JSValueConst a,
                                  uint64_t bits)
{
    uint64_t full = bits / JS_LIMB_BITS;
    int rem = (int)(bits % JS_LIMB_BITS);
    uint64_t len = full + 1;            /* the extra limb carries the 0 sign */
    JSBigInt *r;
    uint64_t i;

    if (len > INT32_MAX)
        return JS_ThrowRangeError(ctx, "BigInt.asUintN: bits too large");
    r = js_bigint_new(ctx, (int)len);
    if (!r)
        return JS_EXCEPTION;
    r->len = (int)len;
    for (i = 0; i < full; i++)
        r->tab[i] = js_bigint_limb_ext(a, i);
    r->tab[full] = rem ? (js_bigint_limb_ext(a, full) &
                          (((js_limb_t)1 << rem) - 1))
                       : 0;
    r = js_bigint_normalize(ctx, r);
    return JS_CompactBigInt(ctx, r);
}

static JSValue js_bigint_asUintN(JSContext *ctx,
                                  JSValueConst this_val,
                                  int argc, JSValueConst *argv, int asIntN)
{
    uint64_t bits;
    JSValue res, a;
    
    if (JS_ToIndex(ctx, &bits, argv[0]))
        return JS_EXCEPTION;
    a = JS_ToBigInt(ctx, argv[1]);
    if (JS_IsException(a))
        return JS_EXCEPTION;
    if (bits == 0) {
        JS_FreeValue(ctx, a);
        res = __JS_NewShortBigInt(ctx, 0);
    } else if (!asIntN) {
        res = js_bigint_as_uintn(ctx, a, bits);
        JS_FreeValue(ctx, a);
    } else if (JS_VALUE_GET_TAG(a) == JS_TAG_SHORT_BIG_INT) {
        /* fast case */
        if (bits >= JS_SHORT_BIG_INT_BITS) {
            res = a;
        } else {
            uint64_t v;
            int shift;
            shift = 64 - bits;
            v = JS_VALUE_GET_SHORT_BIG_INT(a);
            v = v << shift;
            if (asIntN)
                v = (int64_t)v >> shift;
            else
                v = v >> shift;
            res = __JS_NewShortBigInt(ctx, v);
        }
    } else {
        JSBigInt *r, *p = JS_VALUE_GET_PTR(a);
        if (bits >= (uint64_t)p->len * JS_LIMB_BITS) {
            res = a;
        } else {
            int len, shift, i;
            js_limb_t v;
            len = (bits + JS_LIMB_BITS - 1) / JS_LIMB_BITS;
            r = js_bigint_new(ctx, len);
            if (!r) {
                JS_FreeValue(ctx, a);
                return JS_EXCEPTION;
            }
            r->len = len;
            for(i = 0; i < len - 1; i++)
                r->tab[i] = p->tab[i];
            shift = (-bits) & (JS_LIMB_BITS - 1);
            /* 0 <= shift <= JS_LIMB_BITS - 1 */
            v = p->tab[len - 1] << shift;
            if (asIntN)
                v = (js_slimb_t)v >> shift;
            else
                v = v >> shift;
            r->tab[len - 1] = v;
            r = js_bigint_normalize(ctx, r);
            JS_FreeValue(ctx, a);
            res = JS_CompactBigInt(ctx, r);
        }
    }
    return res;
}

static const JSCFunctionListEntry js_bigint_funcs[] = {
    JS_CFUNC_MAGIC_DEF("asUintN", 2, js_bigint_asUintN, 0 ),
    JS_CFUNC_MAGIC_DEF("asIntN", 2, js_bigint_asUintN, 1 ),
};

static const JSCFunctionListEntry js_bigint_proto_funcs[] = {
    JS_CFUNC_DEF("toString", 0, js_bigint_toString ),
    JS_CFUNC_DEF("valueOf", 0, js_bigint_valueOf ),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "BigInt", JS_PROP_CONFIGURABLE ),
};

static int JS_AddIntrinsicBigInt(JSContext *ctx)
{
    JSValue obj1;

    obj1 = JS_NewCConstructor(ctx, JS_CLASS_BIG_INT, "BigInt",
                                     js_bigint_constructor, 1, JS_CFUNC_constructor_or_func, 0,
                                     JS_UNDEFINED,
                                     js_bigint_funcs, countof(js_bigint_funcs),
                                     js_bigint_proto_funcs, countof(js_bigint_proto_funcs),
                                     0);
    if (JS_IsException(obj1))
        return -1;
    JS_FreeValue(ctx, obj1);
    return 0;
}

/* Minimum amount of objects to be able to compile code and display
   error messages. */
static int JS_AddIntrinsicBasicObjects(JSContext *ctx)
{
    JSValue obj;
    JSCFunctionType ft;
    int i;

    /* warning: ordering is tricky */
    ctx->class_proto[JS_CLASS_OBJECT] =
        JS_NewObjectProtoClassAlloc(ctx, JS_NULL, JS_CLASS_OBJECT,
                                    countof(js_object_proto_funcs) + 1);
    if (JS_IsException(ctx->class_proto[JS_CLASS_OBJECT]))
        return -1;
    JS_SetImmutablePrototype(ctx, ctx->class_proto[JS_CLASS_OBJECT]);

    /* 2 more properties: caller and arguments */
    ctx->function_proto = JS_NewCFunction3(ctx, js_function_proto, "", 0,
                                           JS_CFUNC_generic, 0,
                                           ctx->class_proto[JS_CLASS_OBJECT],
                                           countof(js_function_proto_funcs) + 3 + 2);
    if (JS_IsException(ctx->function_proto))
        return -1;
    ctx->class_proto[JS_CLASS_BYTECODE_FUNCTION] = JS_DupValue(ctx, ctx->function_proto);

    ctx->global_obj = JS_NewObjectProtoClassAlloc(ctx, ctx->class_proto[JS_CLASS_OBJECT],
                                                  JS_CLASS_GLOBAL_OBJECT, 64);
    if (JS_IsException(ctx->global_obj))
        return -1;
    {
        JSObject *p;
        obj = JS_NewObjectProtoClassAlloc(ctx, JS_NULL, JS_CLASS_OBJECT, 4);
        p = JS_VALUE_GET_OBJ(ctx->global_obj);
        p->u.global_object.uninitialized_vars = obj;
    }
    ctx->global_var_obj = JS_NewObjectProtoClassAlloc(ctx, JS_NULL,
                                                      JS_CLASS_OBJECT, 16);
    if (JS_IsException(ctx->global_var_obj))
        return -1;

    /* Error */
    ft.generic_magic = js_error_constructor;
    obj = JS_NewCConstructor(ctx, JS_CLASS_ERROR, "Error",
                                    ft.generic, 1, JS_CFUNC_constructor_or_func_magic, -1,
                                    JS_UNDEFINED,
                                    js_error_funcs, countof(js_error_funcs),
                                    js_error_proto_funcs, countof(js_error_proto_funcs),
                                    0);
    if (JS_IsException(obj))
        return -1;

    for(i = 0; i < JS_NATIVE_ERROR_COUNT; i++) {
        JSValue func_obj;
        const JSCFunctionListEntry *funcs;
        int n_args;
        char buf[ATOM_GET_STR_BUF_SIZE];
        const char *name = JS_AtomGetStr(ctx, buf, sizeof(buf),
                                         JS_ATOM_EvalError + i);
        if (i == JS_SUPPRESSED_ERROR)
            n_args = 3;
        else
            n_args = 1 + (i == JS_AGGREGATE_ERROR);
        funcs = js_native_error_proto_funcs + 2 * i;
        func_obj = JS_NewCConstructor(ctx, -1, name,
                                      ft.generic, n_args, JS_CFUNC_constructor_or_func_magic, i,
                                      obj,
                                      NULL, 0,
                                      funcs, 2,
                                      0);
        if (JS_IsException(func_obj)) {
            JS_FreeValue(ctx, obj);
            return -1;
        }
        ctx->native_error_proto[i] = JS_GetProperty(ctx, func_obj, JS_ATOM_prototype);
        JS_FreeValue(ctx, func_obj);
        if (JS_IsException(ctx->native_error_proto[i])) {
            JS_FreeValue(ctx, obj);
            return -1;
        }
    }
    JS_FreeValue(ctx, obj);

    /* Array */
    obj = JS_NewCConstructor(ctx, JS_CLASS_ARRAY, "Array",
                                    js_array_constructor, 1, JS_CFUNC_constructor_or_func, 0,
                                    JS_UNDEFINED,
                                    js_array_funcs, countof(js_array_funcs),
                                    js_array_proto_funcs, countof(js_array_proto_funcs),
                                    JS_NEW_CTOR_PROTO_CLASS);
    if (JS_IsException(obj))
        return -1;
    ctx->array_ctor = obj;

    /* non-ECMAScript `_`-prefixed methods, installed non-enumerable on
     * Array.prototype. */
    JS_SetPropertyFunctionList(ctx, ctx->class_proto[JS_CLASS_ARRAY],
                               js_array_ext_funcs, countof(js_array_ext_funcs));
    /* static Array.* helpers (Array.repeat) on the Array constructor */
    JS_SetPropertyFunctionList(ctx, ctx->array_ctor,
                               js_array_static_ext_funcs, countof(js_array_static_ext_funcs));

    {
        JSObject *p = JS_VALUE_GET_OBJ(ctx->class_proto[JS_CLASS_ARRAY]);
        p->is_std_array_prototype = TRUE;
    }
    
    ctx->array_shape = js_new_shape2(ctx, get_proto_obj(ctx->class_proto[JS_CLASS_ARRAY]),
                                     JS_PROP_INITIAL_HASH_SIZE, 1);
    if (!ctx->array_shape)
        return -1;
    if (add_shape_property(ctx, &ctx->array_shape, NULL,
                           JS_ATOM_length, JS_PROP_WRITABLE | JS_PROP_LENGTH))
        return -1;

    ctx->arguments_shape = js_new_shape2(ctx, get_proto_obj(ctx->class_proto[JS_CLASS_OBJECT]),
                                         JS_PROP_INITIAL_HASH_SIZE, 3);
    if (!ctx->arguments_shape)
        return -1;
    if (add_shape_property(ctx, &ctx->arguments_shape, NULL,
                           JS_ATOM_length, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE))
        return -1;
    if (add_shape_property(ctx, &ctx->arguments_shape, NULL,
                           JS_ATOM_Symbol_iterator, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE))
        return -1;
    if (add_shape_property(ctx, &ctx->arguments_shape, NULL,
                           JS_ATOM_callee, JS_PROP_GETSET))
        return -1;

    ctx->mapped_arguments_shape = js_new_shape2(ctx, get_proto_obj(ctx->class_proto[JS_CLASS_OBJECT]),
                                         JS_PROP_INITIAL_HASH_SIZE, 3);
    if (!ctx->mapped_arguments_shape)
        return -1;
    if (add_shape_property(ctx, &ctx->mapped_arguments_shape, NULL,
                           JS_ATOM_length, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE))
        return -1;
    if (add_shape_property(ctx, &ctx->mapped_arguments_shape, NULL,
                           JS_ATOM_Symbol_iterator, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE))
        return -1;
    if (add_shape_property(ctx, &ctx->mapped_arguments_shape, NULL,
                           JS_ATOM_callee, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE))
        return -1;
    
    return 0;
}

int JS_AddIntrinsicBaseObjects(JSContext *ctx)
{
    JSValue obj1, obj2;
    JSCFunctionType ft;

    ctx->throw_type_error = JS_NewCFunction(ctx, js_throw_type_error, NULL, 0);
    if (JS_IsException(ctx->throw_type_error))
        return -1;
    /* add caller and arguments properties to throw a TypeError */
    if (JS_DefineProperty(ctx, ctx->function_proto, JS_ATOM_caller, JS_UNDEFINED,
                          ctx->throw_type_error, ctx->throw_type_error,
                          JS_PROP_HAS_GET | JS_PROP_HAS_SET |
                          JS_PROP_HAS_CONFIGURABLE | JS_PROP_CONFIGURABLE) < 0)
        return -1;
    if (JS_DefineProperty(ctx, ctx->function_proto, JS_ATOM_arguments, JS_UNDEFINED,
                          ctx->throw_type_error, ctx->throw_type_error,
                          JS_PROP_HAS_GET | JS_PROP_HAS_SET |
                          JS_PROP_HAS_CONFIGURABLE | JS_PROP_CONFIGURABLE) < 0)
        return -1;
    JS_FreeValue(ctx, js_object_seal(ctx, JS_UNDEFINED, 1, (JSValueConst *)&ctx->throw_type_error, 1));

    /* Object */
    obj1 = JS_NewCConstructor(ctx, JS_CLASS_OBJECT, "Object",
                              js_object_constructor, 1, JS_CFUNC_constructor_or_func, 0,
                              JS_UNDEFINED,
                              js_object_funcs, countof(js_object_funcs),
                              js_object_proto_funcs, countof(js_object_proto_funcs),
                              JS_NEW_CTOR_PROTO_EXIST);
    if (JS_IsException(obj1))
        return -1;
    /* non-ECMAScript static Object utilities,
     * installed non-enumerable on the Object constructor (never on the proto). */
    JS_SetPropertyFunctionList(ctx, obj1, js_object_ext_funcs,
                               countof(js_object_ext_funcs));
    JS_FreeValue(ctx, obj1);

    /* Function */
    ft.generic_magic = js_function_constructor;
    obj1 = JS_NewCConstructor(ctx, JS_CLASS_BYTECODE_FUNCTION, "Function",
                              ft.generic, 1, JS_CFUNC_constructor_or_func_magic, JS_FUNC_NORMAL,
                              JS_UNDEFINED,
                              NULL, 0,
                              js_function_proto_funcs, countof(js_function_proto_funcs),
                              JS_NEW_CTOR_PROTO_EXIST);
    if (JS_IsException(obj1))
        return -1;
    ctx->function_ctor = obj1;
    /* non-ECMAScript Function.prototype combinators,
     * installed non-enumerable on Function.prototype. */
    JS_SetPropertyFunctionList(ctx, ctx->function_proto, js_function_ext_funcs,
                               countof(js_function_ext_funcs));
    /* Function STATICS (identity/always/cond/of/not/negate/applyTo/
     * uncurryN/lift/liftN/ap), installed non-enumerable on the Function ctor. */
    JS_SetPropertyFunctionList(ctx, ctx->function_ctor, js_function_static_ext_funcs,
                               countof(js_function_static_ext_funcs));

    /* Iterator */
    obj2 = JS_NewCConstructor(ctx, JS_CLASS_ITERATOR, "Iterator",
                                     js_iterator_constructor, 0, JS_CFUNC_constructor_or_func, 0,
                                     JS_UNDEFINED,
                                     js_iterator_funcs, countof(js_iterator_funcs),
                                     js_iterator_proto_funcs, countof(js_iterator_proto_funcs),
                                     0);
    if (JS_IsException(obj2))
        return -1;
    /* the lazy ext tier (array.inc.c, W6.2), non-enumerable on
     * Iterator.prototype — the same install as Function.prototype's above. */
    JS_SetPropertyFunctionList(ctx, ctx->class_proto[JS_CLASS_ITERATOR],
                               js_iterator_ext_funcs, countof(js_iterator_ext_funcs));
    // quirk: Iterator.prototype.constructor is an accessor property
    // TODO(bnoordhuis) mildly inefficient because JS_NewGlobalCConstructor
    // first creates a .constructor value property that we then replace with
    // an accessor
    obj1 = JS_NewCFunctionData(ctx, js_iterator_constructor_getset,
                               0, 0, 1, (JSValueConst *)&obj2);
    if (JS_IsException(obj1)) {
        JS_FreeValue(ctx, obj2);
        return -1;
    }
    if (JS_DefineProperty(ctx, ctx->class_proto[JS_CLASS_ITERATOR],
                          JS_ATOM_constructor, JS_UNDEFINED,
                          obj1, obj1,
                          JS_PROP_HAS_GET | JS_PROP_HAS_SET | JS_PROP_CONFIGURABLE) < 0) {
        JS_FreeValue(ctx, obj2);
        JS_FreeValue(ctx, obj1);
        return -1;
    }
    JS_FreeValue(ctx, obj1);
    ctx->iterator_ctor = obj2;
    
    ctx->class_proto[JS_CLASS_ITERATOR_CONCAT] =
        JS_NewObjectProtoList(ctx, ctx->class_proto[JS_CLASS_ITERATOR], 
                              js_iterator_concat_proto_funcs,
                              countof(js_iterator_concat_proto_funcs));
    if (JS_IsException(ctx->class_proto[JS_CLASS_ITERATOR_CONCAT]))
        return -1;

    ctx->class_proto[JS_CLASS_ITERATOR_ZIP] =
        JS_NewObjectProtoList(ctx, ctx->class_proto[JS_CLASS_ITERATOR],
                              js_iterator_zip_proto_funcs,
                              countof(js_iterator_zip_proto_funcs));
    if (JS_IsException(ctx->class_proto[JS_CLASS_ITERATOR_ZIP]))
        return -1;
    ctx->class_proto[JS_CLASS_ITERATOR_HELPER] =
        JS_NewObjectProtoList(ctx, ctx->class_proto[JS_CLASS_ITERATOR], 
                              js_iterator_helper_proto_funcs,
                              countof(js_iterator_helper_proto_funcs));
    if (JS_IsException(ctx->class_proto[JS_CLASS_ITERATOR_HELPER]))
        return -1;
                       
    ctx->class_proto[JS_CLASS_ITERATOR_WRAP] =
        JS_NewObjectProtoList(ctx, ctx->class_proto[JS_CLASS_ITERATOR], 
                              js_iterator_wrap_proto_funcs,
                              countof(js_iterator_wrap_proto_funcs));
    if (JS_IsException(ctx->class_proto[JS_CLASS_ITERATOR_WRAP]))
        return -1;

    /* needed to initialize arguments[Symbol.iterator] */
    ctx->array_proto_values =
        JS_GetProperty(ctx, ctx->class_proto[JS_CLASS_ARRAY], JS_ATOM_values);
    if (JS_IsException(ctx->array_proto_values))
        return -1;

    ctx->class_proto[JS_CLASS_ARRAY_ITERATOR] =
        JS_NewObjectProtoList(ctx, ctx->class_proto[JS_CLASS_ITERATOR], 
                              js_array_iterator_proto_funcs,
                              countof(js_array_iterator_proto_funcs));
    if (JS_IsException(ctx->class_proto[JS_CLASS_ARRAY_ITERATOR]))
        return -1;

    /* parseFloat and parseInteger must be defined before Number
       because of the Number.parseFloat and Number.parseInteger
       aliases */
    if (JS_SetPropertyFunctionList(ctx, ctx->global_obj, js_global_funcs,
                                   countof(js_global_funcs)))
        return -1;

    /* Number */
    obj1 = JS_NewCConstructor(ctx, JS_CLASS_NUMBER, "Number",
                                     js_number_constructor, 1, JS_CFUNC_constructor_or_func, 0,
                                     JS_UNDEFINED,
                                     js_number_funcs, countof(js_number_funcs),
                                     js_number_proto_funcs, countof(js_number_proto_funcs),
                                     JS_NEW_CTOR_PROTO_CLASS);
    if (JS_IsException(obj1))
        return -1;
    JS_FreeValue(ctx, obj1);
    if (JS_SetObjectData(ctx, ctx->class_proto[JS_CLASS_NUMBER], JS_NewInt32(ctx, 0)))
        return -1;

    /* non-ECMAScript methods, installed non-enumerable on Number.prototype. */
    JS_SetPropertyFunctionList(ctx, ctx->class_proto[JS_CLASS_NUMBER],
                               js_number_ext_funcs, countof(js_number_ext_funcs));

    /* Boolean */
    obj1 = JS_NewCConstructor(ctx, JS_CLASS_BOOLEAN, "Boolean",
                                     js_boolean_constructor, 1, JS_CFUNC_constructor_or_func, 0,
                                     JS_UNDEFINED,
                                     NULL, 0,
                                     js_boolean_proto_funcs, countof(js_boolean_proto_funcs),
                                     JS_NEW_CTOR_PROTO_CLASS);
    if (JS_IsException(obj1))
        return -1;
    JS_FreeValue(ctx, obj1);
    if (JS_SetObjectData(ctx, ctx->class_proto[JS_CLASS_BOOLEAN], JS_NewBool(ctx, FALSE)))
        return -1;

    /* String */
    obj1 = JS_NewCConstructor(ctx, JS_CLASS_STRING, "String",
                                     js_string_constructor, 1, JS_CFUNC_constructor_or_func, 0,
                                     JS_UNDEFINED,
                                     js_string_funcs, countof(js_string_funcs),
                                     js_string_proto_funcs, countof(js_string_proto_funcs),
                                     JS_NEW_CTOR_PROTO_CLASS);
    if (JS_IsException(obj1))
        return -1;
    JS_FreeValue(ctx, obj1);
    if (JS_SetObjectData(ctx, ctx->class_proto[JS_CLASS_STRING], JS_AtomToString(ctx, JS_ATOM_empty_string)))
        return -1;

    /* non-ECMAScript methods, installed non-enumerable on String.prototype. */
    JS_SetPropertyFunctionList(ctx, ctx->class_proto[JS_CLASS_STRING],
                               js_string_ext_funcs, countof(js_string_ext_funcs));

    ctx->class_proto[JS_CLASS_STRING_ITERATOR] =
        JS_NewObjectProtoList(ctx, ctx->class_proto[JS_CLASS_ITERATOR], 
                              js_string_iterator_proto_funcs,
                              countof(js_string_iterator_proto_funcs));
    if (JS_IsException(ctx->class_proto[JS_CLASS_STRING_ITERATOR]))
        return -1;

    /* Math: create as autoinit object */
    js_random_init(ctx);
    if (JS_SetPropertyFunctionList(ctx, ctx->global_obj, js_math_obj, countof(js_math_obj)))
        return -1;

    /* ES6 Reflect: create as autoinit object */
    if (JS_SetPropertyFunctionList(ctx, ctx->global_obj, js_reflect_obj, countof(js_reflect_obj)))
        return -1;

    /* ES6 Symbol */
    obj1 = JS_NewCConstructor(ctx, JS_CLASS_SYMBOL, "Symbol",
                                     js_symbol_constructor, 0, JS_CFUNC_constructor_or_func, 0,
                                     JS_UNDEFINED,
                                     js_symbol_funcs, countof(js_symbol_funcs),
                                     js_symbol_proto_funcs, countof(js_symbol_proto_funcs),
                                     0);
    if (JS_IsException(obj1))
        return -1;
    JS_FreeValue(ctx, obj1);
    
    /* ES6 Generator */
    ctx->class_proto[JS_CLASS_GENERATOR] =
        JS_NewObjectProtoList(ctx, ctx->class_proto[JS_CLASS_ITERATOR],
                              js_generator_proto_funcs,
                              countof(js_generator_proto_funcs));
    if (JS_IsException(ctx->class_proto[JS_CLASS_GENERATOR]))
        return -1;

    ft.generic_magic = js_function_constructor;
    obj1 = JS_NewCConstructor(ctx, JS_CLASS_GENERATOR_FUNCTION, "GeneratorFunction",
                                     ft.generic, 1, JS_CFUNC_constructor_or_func_magic, JS_FUNC_GENERATOR,
                                     ctx->function_ctor,
                                     NULL, 0,
                                     js_generator_function_proto_funcs,
                                     countof(js_generator_function_proto_funcs),
                                     JS_NEW_CTOR_NO_GLOBAL | JS_NEW_CTOR_READONLY);
    if (JS_IsException(obj1))
        return -1;
    JS_FreeValue(ctx, obj1);
    if (JS_SetConstructor2(ctx, ctx->class_proto[JS_CLASS_GENERATOR_FUNCTION],
                           ctx->class_proto[JS_CLASS_GENERATOR],
                           JS_PROP_CONFIGURABLE, JS_PROP_CONFIGURABLE))
        return -1;
    
    /* global properties */
    ctx->eval_obj = JS_GetProperty(ctx, ctx->global_obj, JS_ATOM_eval);
    if (JS_IsException(ctx->eval_obj))
        return -1;
    
    if (JS_DefinePropertyValue(ctx, ctx->global_obj, JS_ATOM_globalThis,
                               JS_DupValue(ctx, ctx->global_obj),
                               JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE) < 0)
        return -1;

    /* BigInt */
    if (JS_AddIntrinsicBigInt(ctx))
        return -1;
    return 0;
}

/* Typed Arrays */

static uint8_t const typed_array_size_log2[JS_TYPED_ARRAY_COUNT] = {
    0, 0, 0, 1, 1, 2, 2,
    3, 3,                   // BigInt64Array, BigUint64Array
    1, 2, 3                 // Float16Array, Float32Array, Float64Array
};

static JSValue js_array_buffer_constructor3(JSContext *ctx,
                                            JSValueConst new_target,
                                            uint64_t len, uint64_t *max_len,
                                            JSClassID class_id,
                                            uint8_t *buf,
                                            JSFreeArrayBufferDataFunc *free_func,
                                            void *opaque, BOOL alloc_flag)
{
    JSRuntime *rt = ctx->rt;
    JSValue obj;
    JSArrayBuffer *abuf = NULL;
    uint64_t sab_alloc_len;

    if (!alloc_flag && buf && max_len && free_func != js_array_buffer_free) {
        // not observable from JS land, only through C API misuse;
        // JS code cannot create externally managed buffers directly
        return JS_ThrowInternalError(ctx,
                                     "resizable ArrayBuffers not supported "
                                     "for externally managed buffers");
    }
    obj = js_create_from_ctor(ctx, new_target, class_id);
    if (JS_IsException(obj))
        return obj;
    /* XXX: we are currently limited to 2 GB */
    if (len > INT32_MAX) {
        JS_ThrowRangeError(ctx, "invalid array buffer length");
        goto fail;
    }
    if (max_len && *max_len > INT32_MAX) {
        JS_ThrowRangeError(ctx, "invalid max array buffer length");
        goto fail;
    }
    abuf = js_malloc(ctx, sizeof(*abuf));
    if (!abuf)
        goto fail;
    abuf->byte_length = len;
    abuf->max_byte_length = max_len ? *max_len : -1;
    if (alloc_flag) {
        if (class_id == JS_CLASS_SHARED_ARRAY_BUFFER &&
            rt->sab_funcs.sab_alloc) {
            // TOOD(bnoordhuis) resizing backing memory for SABs atomically
            // is hard so we cheat and allocate |maxByteLength| bytes upfront
            sab_alloc_len = max_len ? *max_len : len;
            abuf->data = rt->sab_funcs.sab_alloc(rt->sab_funcs.sab_opaque,
                                                 max_int(sab_alloc_len, 1));
            if (!abuf->data)
                goto fail;
            memset(abuf->data, 0, sab_alloc_len);
        } else {
            /* the allocation must be done after the object creation */
            abuf->data = js_mallocz(ctx, max_int(len, 1));
            if (!abuf->data)
                goto fail;
        }
    } else {
        if (class_id == JS_CLASS_SHARED_ARRAY_BUFFER &&
            rt->sab_funcs.sab_dup) {
            rt->sab_funcs.sab_dup(rt->sab_funcs.sab_opaque, buf);
        }
        abuf->data = buf;
    }
    init_list_head(&abuf->array_list);
    abuf->detached = FALSE;
    abuf->shared = (class_id == JS_CLASS_SHARED_ARRAY_BUFFER);
    abuf->opaque = opaque;
    abuf->free_func = free_func;
    if (alloc_flag && buf)
        memcpy(abuf->data, buf, len);
    JS_SetOpaque(obj, abuf);
    return obj;
 fail:
    JS_FreeValue(ctx, obj);
    js_free(ctx, abuf);
    return JS_EXCEPTION;
}

static void js_array_buffer_free(JSRuntime *rt, void *opaque, void *ptr)
{
    js_free_rt(rt, ptr);
}

static JSValue js_array_buffer_constructor2(JSContext *ctx,
                                            JSValueConst new_target,
                                            uint64_t len, uint64_t *max_len,
                                            JSClassID class_id)
{
    return js_array_buffer_constructor3(ctx, new_target, len, max_len, class_id,
                                        NULL, js_array_buffer_free, NULL,
                                        TRUE);
}

static JSValue js_array_buffer_constructor1(JSContext *ctx,
                                            JSValueConst new_target,
                                            uint64_t len, uint64_t *max_len)
{
    return js_array_buffer_constructor2(ctx, new_target, len, max_len,
                                        JS_CLASS_ARRAY_BUFFER);
}

JSValue JS_NewArrayBuffer(JSContext *ctx, uint8_t *buf, size_t len,
                          JSFreeArrayBufferDataFunc *free_func, void *opaque,
                          BOOL is_shared)
{
    JSClassID class_id =
        is_shared ? JS_CLASS_SHARED_ARRAY_BUFFER : JS_CLASS_ARRAY_BUFFER;
    return js_array_buffer_constructor3(ctx, JS_UNDEFINED, len, NULL, class_id,
                                        buf, free_func, opaque, FALSE);
}

/* create a new ArrayBuffer of length 'len' and copy 'buf' to it */
JSValue JS_NewArrayBufferCopy(JSContext *ctx, const uint8_t *buf, size_t len)
{
    return js_array_buffer_constructor3(ctx, JS_UNDEFINED, len, NULL,
                                        JS_CLASS_ARRAY_BUFFER,
                                        (uint8_t *)buf,
                                        js_array_buffer_free, NULL,
                                        TRUE);
}

static JSValue js_array_buffer_constructor0(JSContext *ctx, JSValueConst new_target,
                                            int argc, JSValueConst *argv,
                                            JSClassID class_id)
 {
    uint64_t len, max_len, *pmax_len = NULL;
    JSValue obj, val;
    int64_t i;

     if (JS_ToIndex(ctx, &len, argv[0]))
         return JS_EXCEPTION;
    if (argc < 2)
        goto next;
    if (!JS_IsObject(argv[1]))
        goto next;
    obj = JS_ToObject(ctx, argv[1]);
    if (JS_IsException(obj))
        return JS_EXCEPTION;
    val = JS_GetProperty(ctx, obj, JS_ATOM_maxByteLength);
    JS_FreeValue(ctx, obj);
    if (JS_IsException(val))
        return JS_EXCEPTION;
    if (JS_IsUndefined(val))
        goto next;
    if (JS_ToInt64Free(ctx, &i, val))
        return JS_EXCEPTION;
    // don't have to check i < 0 because len >= 0
    if (len > i || i > MAX_SAFE_INTEGER)
        return JS_ThrowRangeError(ctx, "invalid array buffer max length");
    max_len = i;
    pmax_len = &max_len;
next:
    return js_array_buffer_constructor2(ctx, new_target, len, pmax_len,
                                        class_id);
}

static JSValue js_array_buffer_constructor(JSContext *ctx,
                                           JSValueConst new_target,
                                           int argc, JSValueConst *argv)
{
    return js_array_buffer_constructor0(ctx, new_target, argc, argv,
                                        JS_CLASS_ARRAY_BUFFER);
}

static JSValue js_shared_array_buffer_constructor(JSContext *ctx,
                                                  JSValueConst new_target,
                                                  int argc, JSValueConst *argv)
{
    return js_array_buffer_constructor0(ctx, new_target, argc, argv,
                                        JS_CLASS_SHARED_ARRAY_BUFFER);
}

/* also used for SharedArrayBuffer */
static void js_array_buffer_finalizer(JSRuntime *rt, JSValue val)
{
    JSObject *p = JS_VALUE_GET_OBJ(val);
    JSArrayBuffer *abuf = p->u.array_buffer;
    struct list_head *el, *el1;

    if (abuf) {
        /* The ArrayBuffer finalizer may be called before the typed
           array finalizers using it, so abuf->array_list is not
           necessarily empty. */
        list_for_each_safe(el, el1, &abuf->array_list) {
            JSTypedArray *ta;
            JSObject *p1;

            ta = list_entry(el, JSTypedArray, link);
            ta->link.prev = NULL;
            ta->link.next = NULL;
            p1 = ta->obj;
            /* Note: the typed array length and offset fields are not modified */
            if (p1->class_id != JS_CLASS_DATAVIEW) {
                p1->u.array.count = 0;
                p1->u.array.u.ptr = NULL;
            }
        }
        if (abuf->shared && rt->sab_funcs.sab_free) {
            rt->sab_funcs.sab_free(rt->sab_funcs.sab_opaque, abuf->data);
        } else {
            if (abuf->free_func)
                abuf->free_func(rt, abuf->opaque, abuf->data);
        }
        js_free_rt(rt, abuf);
    }
}

static JSValue js_array_buffer_isView(JSContext *ctx,
                                      JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    JSObject *p;
    BOOL res;
    res = FALSE;
    if (JS_VALUE_GET_TAG(argv[0]) == JS_TAG_OBJECT) {
        p = JS_VALUE_GET_OBJ(argv[0]);
        if (p->class_id >= JS_CLASS_UINT8C_ARRAY &&
            p->class_id <= JS_CLASS_DATAVIEW) {
            res = TRUE;
        }
    }
    return JS_NewBool(ctx, res);
}

static const JSCFunctionListEntry js_array_buffer_funcs[] = {
    JS_CFUNC_DEF("isView", 1, js_array_buffer_isView ),
    JS_CGETSET_DEF("[Symbol.species]", js_get_this, NULL ),
};

static JSValue JS_ThrowTypeErrorDetachedArrayBuffer(JSContext *ctx)
{
    return JS_ThrowTypeError(ctx, "ArrayBuffer is detached");
}

static JSValue JS_ThrowTypeErrorArrayBufferOOB(JSContext *ctx)
{
    return JS_ThrowTypeError(ctx, "ArrayBuffer is detached or resized");
}

// #sec-get-arraybuffer.prototype.detached
static JSValue js_array_buffer_get_detached(JSContext *ctx,
                                                 JSValueConst this_val)
{
    JSArrayBuffer *abuf = JS_GetOpaque2(ctx, this_val, JS_CLASS_ARRAY_BUFFER);
    if (!abuf)
        return JS_EXCEPTION;
    if (abuf->shared)
        return JS_ThrowTypeError(ctx, "detached called on SharedArrayBuffer");
    return JS_NewBool(ctx, abuf->detached);
}

static JSValue js_array_buffer_get_byteLength(JSContext *ctx,
                                              JSValueConst this_val,
                                              int class_id)
{
    JSArrayBuffer *abuf = JS_GetOpaque2(ctx, this_val, class_id);
    if (!abuf)
        return JS_EXCEPTION;
    /* return 0 if detached */
    return JS_NewUint32(ctx, abuf->byte_length);
}

static JSValue js_array_buffer_get_maxByteLength(JSContext *ctx,
                                                 JSValueConst this_val,
                                                 int class_id)
{
    JSArrayBuffer *abuf = JS_GetOpaque2(ctx, this_val, class_id);
    if (!abuf)
        return JS_EXCEPTION;
    if (array_buffer_is_resizable(abuf))
        return JS_NewUint32(ctx, abuf->max_byte_length);
    return JS_NewUint32(ctx, abuf->byte_length);
}

static JSValue js_array_buffer_get_resizable(JSContext *ctx,
                                             JSValueConst this_val,
                                             int class_id)
{
    JSArrayBuffer *abuf = JS_GetOpaque2(ctx, this_val, class_id);
    if (!abuf)
        return JS_EXCEPTION;
    return JS_NewBool(ctx, array_buffer_is_resizable(abuf));
}

static void js_array_buffer_update_typed_arrays(JSArrayBuffer *abuf)
{
    uint32_t size_log2, size_elem;
    struct list_head *el;
    JSTypedArray *ta;
    JSObject *p;
    uint8_t *data;
    int64_t len;

    len = abuf->byte_length;
    data = abuf->data;
    // update lengths of all typed arrays backed by this array buffer
    list_for_each(el, &abuf->array_list) {
        ta = list_entry(el, JSTypedArray, link);
        p = ta->obj;
        if (p->class_id == JS_CLASS_DATAVIEW) {
            if (ta->track_rab) {
                if (ta->offset < len)
                    ta->length = len - ta->offset;
                else
                    ta->length = 0;
            }
        } else {
            p->u.array.count = 0;
            p->u.array.u.ptr = NULL;
            size_log2 = typed_array_size_log2(p->class_id);
            size_elem = 1 << size_log2;
            if (ta->track_rab) {
                if (len >= (int64_t)ta->offset + size_elem) {
                    p->u.array.count = (len - ta->offset) >> size_log2;
                    p->u.array.u.ptr = &data[ta->offset];
                }
            } else {
                if (len >= (int64_t)ta->offset + ta->length) {
                    p->u.array.count = ta->length >> size_log2;
                    p->u.array.u.ptr = &data[ta->offset];
                }
            }
        }
    }
    
}

void JS_DetachArrayBuffer(JSContext *ctx, JSValueConst obj)
{
    JSArrayBuffer *abuf = JS_GetOpaque(obj, JS_CLASS_ARRAY_BUFFER);

    if (!abuf || abuf->detached)
        return;
    if (abuf->free_func)
        abuf->free_func(ctx->rt, abuf->opaque, abuf->data);
    abuf->data = NULL;
    abuf->byte_length = 0;
    abuf->detached = TRUE;
    js_array_buffer_update_typed_arrays(abuf);
}

/* get an ArrayBuffer or SharedArrayBuffer */
static JSArrayBuffer *js_get_array_buffer(JSContext *ctx, JSValueConst obj)
{
    JSObject *p;
    if (JS_VALUE_GET_TAG(obj) != JS_TAG_OBJECT)
        goto fail;
    p = JS_VALUE_GET_OBJ(obj);
    if (p->class_id != JS_CLASS_ARRAY_BUFFER &&
        p->class_id != JS_CLASS_SHARED_ARRAY_BUFFER) {
    fail:
        JS_ThrowTypeErrorInvalidClass(ctx, JS_CLASS_ARRAY_BUFFER);
        return NULL;
    }
    return p->u.array_buffer;
}

/* return NULL if exception. WARNING: any JS call can detach the
   buffer and render the returned pointer invalid */
uint8_t *JS_GetArrayBuffer(JSContext *ctx, size_t *psize, JSValueConst obj)
{
    JSArrayBuffer *abuf = js_get_array_buffer(ctx, obj);
    if (!abuf)
        goto fail;
    if (abuf->detached) {
        JS_ThrowTypeErrorDetachedArrayBuffer(ctx);
        goto fail;
    }
    *psize = abuf->byte_length;
    return abuf->data;
 fail:
    *psize = 0;
    return NULL;
}

static BOOL array_buffer_is_resizable(const JSArrayBuffer *abuf)
{
    return abuf->max_byte_length >= 0;
}

// ES #sec-arraybuffer.prototype.transfer
static JSValue js_array_buffer_transfer(JSContext *ctx,
                                        JSValueConst this_val,
                                        int argc, JSValueConst *argv,
                                        int transfer_to_fixed_length)
{
    JSArrayBuffer *abuf;
    uint64_t new_len, *pmax_len, max_len;
    JSValue res;

    abuf = JS_GetOpaque2(ctx, this_val, JS_CLASS_ARRAY_BUFFER);
    if (!abuf)
        return JS_EXCEPTION;
    if (abuf->shared)
        return JS_ThrowTypeError(ctx, "cannot transfer a SharedArrayBuffer");
    if (argc < 1 || JS_IsUndefined(argv[0]))
        new_len = abuf->byte_length;
    else if (JS_ToIndex(ctx, &new_len, argv[0]))
        return JS_EXCEPTION;
    if (abuf->detached)
        return JS_ThrowTypeErrorDetachedArrayBuffer(ctx);
    pmax_len = NULL;
    if (!transfer_to_fixed_length) {
        if (array_buffer_is_resizable(abuf)) { // carry over maxByteLength
            max_len = abuf->max_byte_length;
            if (new_len > max_len)
                return JS_ThrowTypeError(ctx, "invalid array buffer length");
            // TODO(bnoordhuis) support externally managed RABs
            if (abuf->free_func == js_array_buffer_free)
                pmax_len = &max_len;
        }
    }

    /* create an empty AB */
    if (new_len == 0) {
        res = js_array_buffer_constructor2(ctx, JS_UNDEFINED, 0, pmax_len, JS_CLASS_ARRAY_BUFFER);
        if (JS_IsException(res))
            return res;
        JS_DetachArrayBuffer(ctx, this_val);
    } else {
        uint64_t old_len;
        
        old_len = abuf->byte_length;

        /* if length mismatch, realloc. Otherwise, use the same backing buffer. */
        if (new_len != old_len) {
            /* XXX: we are currently limited to 2 GB */
            if (new_len > INT32_MAX)
                return JS_ThrowRangeError(ctx, "invalid array buffer length");

            if (abuf->free_func != js_array_buffer_free) {
                JSArrayBuffer *new_abuf;
                /* cannot use js_realloc() because the buffer was
                   allocated with a custom allocator */
                res = js_array_buffer_constructor2(ctx, JS_UNDEFINED, new_len, pmax_len, JS_CLASS_ARRAY_BUFFER);
                if (JS_IsException(res))
                    return res;
                new_abuf = JS_GetOpaque2(ctx, res, JS_CLASS_ARRAY_BUFFER);
                memcpy(new_abuf->data, abuf->data, min_int(old_len, new_len));
                abuf->free_func(ctx->rt, abuf->opaque, abuf->data);
            } else {
                JSArrayBuffer *new_abuf;
                uint8_t *new_bs;
                /* reallocate the buffer after the new array buffer is
                   created in case the new array buffer creation
                   fails. */
                res = js_array_buffer_constructor2(ctx, JS_UNDEFINED, 0, pmax_len, JS_CLASS_ARRAY_BUFFER);
                if (JS_IsException(res))
                    return res;
                new_bs = js_realloc(ctx, abuf->data, new_len);
                if (!new_bs) {
                    JS_FreeValue(ctx, res);
                    return JS_EXCEPTION;
                }
                if (new_len > old_len)
                    memset(new_bs + old_len, 0, new_len - old_len);
                new_abuf = JS_GetOpaque2(ctx, res, JS_CLASS_ARRAY_BUFFER);
                js_free(ctx, new_abuf->data);
                new_abuf->data = new_bs;
                new_abuf->byte_length = new_len;
            }
        } else {
            /* can keep the custom free function */
            res = js_array_buffer_constructor3(ctx, JS_UNDEFINED, new_len, pmax_len,
                                               JS_CLASS_ARRAY_BUFFER,
                                               abuf->data, abuf->free_func,
                                               abuf->opaque, FALSE);
            if (JS_IsException(res))
                return res;
        }
        /* neuter the backing buffer */
        abuf->data = NULL;
        abuf->byte_length = 0;
        abuf->detached = TRUE;
        js_array_buffer_update_typed_arrays(abuf);
    }
    return res;
}

static JSValue js_array_buffer_resize(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv, int class_id)
{
    JSArrayBuffer *abuf;
    uint8_t *data;
    int64_t len;

    abuf = JS_GetOpaque2(ctx, this_val, class_id);
    if (!abuf)
        return JS_EXCEPTION;
    if (JS_ToInt64(ctx, &len, argv[0]))
        return JS_EXCEPTION;
    if (abuf->detached)
        return JS_ThrowTypeErrorDetachedArrayBuffer(ctx);
    if (!array_buffer_is_resizable(abuf))
        return JS_ThrowTypeError(ctx, "array buffer is not resizable");
    // TODO(bnoordhuis) support externally managed RABs
    if (abuf->free_func != js_array_buffer_free)
        return JS_ThrowTypeError(ctx, "external array buffer is not resizable");
    if (len < 0 || len > abuf->max_byte_length) {
    bad_length:
        return JS_ThrowRangeError(ctx, "invalid array buffer length");
    }
    // SABs can only grow and we don't need to realloc because
    // js_array_buffer_constructor3 commits all memory upfront;
    // regular RABs are resizable both ways and realloc
    if (abuf->shared) {
        if (len < abuf->byte_length)
            goto bad_length;
        // Note this is off-spec; there's supposed to be a single atomic
        // |byteLength| property that's shared across SABs but we store
        // it per SAB instead. That means when thread A calls sab.grow(2)
        // at time t0, and thread B calls sab.grow(1) at time t1, we don't
        // throw a TypeError in thread B as the spec says we should,
        // instead both threads get their own view of the backing memory,
        // 2 bytes big in A, and 1 byte big in B
        abuf->byte_length = len;
    } else {
        data = js_realloc(ctx, abuf->data, max_int(len, 1));
        if (!data)
            return JS_EXCEPTION;
        if (len > abuf->byte_length)
            memset(&data[abuf->byte_length], 0, len - abuf->byte_length);
        abuf->byte_length = len;
        abuf->data = data;
    }
    js_array_buffer_update_typed_arrays(abuf);
    return JS_UNDEFINED;
}

static JSValue js_array_buffer_slice(JSContext *ctx,
                                     JSValueConst this_val,
                                     int argc, JSValueConst *argv, int class_id)
{
    JSArrayBuffer *abuf, *new_abuf;
    int64_t len, start, end, new_len;
    JSValue ctor, new_obj;

    abuf = JS_GetOpaque2(ctx, this_val, class_id);
    if (!abuf)
        return JS_EXCEPTION;
    if (abuf->detached)
        return JS_ThrowTypeErrorDetachedArrayBuffer(ctx);
    len = abuf->byte_length;

    if (JS_ToInt64Clamp(ctx, &start, argv[0], 0, len, len))
        return JS_EXCEPTION;

    end = len;
    if (!JS_IsUndefined(argv[1])) {
        if (JS_ToInt64Clamp(ctx, &end, argv[1], 0, len, len))
            return JS_EXCEPTION;
    }
    new_len = max_int64(end - start, 0);
    ctor = JS_SpeciesConstructor(ctx, this_val, JS_UNDEFINED);
    if (JS_IsException(ctor))
        return ctor;
    if (JS_IsUndefined(ctor)) {
        new_obj = js_array_buffer_constructor2(ctx, JS_UNDEFINED, new_len,
                                               NULL, class_id);
    } else {
        JSValue args[1];
        args[0] = JS_NewInt64(ctx, new_len);
        new_obj = JS_CallConstructor(ctx, ctor, 1, (JSValueConst *)args);
        JS_FreeValue(ctx, ctor);
        JS_FreeValue(ctx, args[0]);
    }
    if (JS_IsException(new_obj))
        return new_obj;
    new_abuf = JS_GetOpaque2(ctx, new_obj, class_id);
    if (!new_abuf)
        goto fail;
    if (js_same_value(ctx, new_obj, this_val)) {
        JS_ThrowTypeError(ctx, "cannot use identical ArrayBuffer");
        goto fail;
    }
    if (new_abuf->detached) {
        JS_ThrowTypeErrorDetachedArrayBuffer(ctx);
        goto fail;
    }
    if (new_abuf->byte_length < new_len) {
        JS_ThrowTypeError(ctx, "new ArrayBuffer is too small");
        goto fail;
    }
    /* must test again because of side effects */
    if (abuf->detached || abuf->byte_length < start + new_len) {
        JS_ThrowTypeErrorDetachedArrayBuffer(ctx);
        goto fail;
    }
    memcpy(new_abuf->data, abuf->data + start, new_len);
    return new_obj;
 fail:
    JS_FreeValue(ctx, new_obj);
    return JS_EXCEPTION;
}

static const JSCFunctionListEntry js_array_buffer_proto_funcs[] = {
    JS_CGETSET_MAGIC_DEF("byteLength", js_array_buffer_get_byteLength, NULL, JS_CLASS_ARRAY_BUFFER ),
    JS_CGETSET_MAGIC_DEF("maxByteLength", js_array_buffer_get_maxByteLength, NULL, JS_CLASS_ARRAY_BUFFER ),
    JS_CGETSET_MAGIC_DEF("resizable", js_array_buffer_get_resizable, NULL, JS_CLASS_ARRAY_BUFFER ),
    JS_CGETSET_DEF("detached", js_array_buffer_get_detached, NULL ),
    JS_CFUNC_MAGIC_DEF("resize", 1, js_array_buffer_resize, JS_CLASS_ARRAY_BUFFER ),
    JS_CFUNC_MAGIC_DEF("slice", 2, js_array_buffer_slice, JS_CLASS_ARRAY_BUFFER ),
    JS_CFUNC_MAGIC_DEF("transfer", 0, js_array_buffer_transfer, 0 ),
    JS_CFUNC_MAGIC_DEF("transferToFixedLength", 0, js_array_buffer_transfer, 1 ),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "ArrayBuffer", JS_PROP_CONFIGURABLE ),
};

/* SharedArrayBuffer */

static const JSCFunctionListEntry js_shared_array_buffer_funcs[] = {
    JS_CGETSET_DEF("[Symbol.species]", js_get_this, NULL ),
};

static const JSCFunctionListEntry js_shared_array_buffer_proto_funcs[] = {
    JS_CGETSET_MAGIC_DEF("byteLength", js_array_buffer_get_byteLength, NULL, JS_CLASS_SHARED_ARRAY_BUFFER ),
    JS_CGETSET_MAGIC_DEF("maxByteLength", js_array_buffer_get_maxByteLength, NULL, JS_CLASS_SHARED_ARRAY_BUFFER ),
    JS_CGETSET_MAGIC_DEF("growable", js_array_buffer_get_resizable, NULL, JS_CLASS_SHARED_ARRAY_BUFFER ),
    JS_CFUNC_MAGIC_DEF("grow", 1, js_array_buffer_resize, JS_CLASS_SHARED_ARRAY_BUFFER ),
    JS_CFUNC_MAGIC_DEF("slice", 2, js_array_buffer_slice, JS_CLASS_SHARED_ARRAY_BUFFER ),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "SharedArrayBuffer", JS_PROP_CONFIGURABLE ),
};

static JSObject *get_typed_array(JSContext *ctx, JSValueConst this_val)
{
    JSObject *p;
    if (JS_VALUE_GET_TAG(this_val) != JS_TAG_OBJECT)
        goto fail;
    p = JS_VALUE_GET_OBJ(this_val);
    if (!(p->class_id >= JS_CLASS_UINT8C_ARRAY &&
          p->class_id <= JS_CLASS_FLOAT64_ARRAY)) {
    fail:
        JS_ThrowTypeError(ctx, "not a TypedArray");
        return NULL;
    }
    return p;
}

// is the typed array detached or out of bounds relative to its RAB?
// |p| must be a typed array, *not* a DataView
static BOOL typed_array_is_oob(JSObject *p)
{
    JSArrayBuffer *abuf;
    JSTypedArray *ta;
    int len, size_elem;
    int64_t end;

    assert(p->class_id >= JS_CLASS_UINT8C_ARRAY);
    assert(p->class_id <= JS_CLASS_FLOAT64_ARRAY);

    ta = p->u.typed_array;
    abuf = ta->buffer->u.array_buffer;
    if (abuf->detached)
        return TRUE;
    len = abuf->byte_length;
    if (ta->offset > len)
        return TRUE;
    if (ta->track_rab)
        return FALSE;
    if (len < (int64_t)ta->offset + ta->length)
        return TRUE;
    size_elem = 1 << typed_array_size_log2(p->class_id);
    end = (int64_t)ta->offset + (int64_t)p->u.array.count * size_elem;
    return end > len;
}

// Be *very* careful if you touch the typed array's memory directly:
// the length is only valid until the next call into JS land because
// JS code can detach or resize the backing array buffer. Functions
// like JS_GetProperty and JS_ToIndex call JS code.
//
// Exclusively reading or writing elements with JS_GetProperty,
// JS_GetPropertyInt64, JS_SetProperty, etc. is safe because they
// perform bounds checks, as does js_get_fast_array_element.
static int js_typed_array_get_length_unsafe(JSContext *ctx, JSValueConst obj)
{
    JSObject *p;
    p = get_typed_array(ctx, obj);
    if (!p)
        return -1;
    if (typed_array_is_oob(p)) {
        JS_ThrowTypeErrorArrayBufferOOB(ctx);
        return -1;
    }
    return p->u.array.count;
}

static int validate_typed_array(JSContext *ctx, JSValueConst this_val)
{
    JSObject *p;
    p = get_typed_array(ctx, this_val);
    if (!p)
        return -1;
    if (typed_array_is_oob(p)) {
        JS_ThrowTypeErrorArrayBufferOOB(ctx);
        return -1;
    }
    return 0;
}

static JSValue js_typed_array_get_length(JSContext *ctx,
                                         JSValueConst this_val)
{
    JSObject *p;
    p = get_typed_array(ctx, this_val);
    if (!p)
        return JS_EXCEPTION;
    return JS_NewInt32(ctx, p->u.array.count);
}

static JSValue js_typed_array_get_buffer(JSContext *ctx,
                                         JSValueConst this_val)
{
    JSObject *p;
    JSTypedArray *ta;
    p = get_typed_array(ctx, this_val);
    if (!p)
        return JS_EXCEPTION;
    ta = p->u.typed_array;
    return JS_DupValue(ctx, JS_MKPTR(JS_TAG_OBJECT, ta->buffer));
}

static JSValue js_typed_array_get_byteLength(JSContext *ctx,
                                             JSValueConst this_val)
{
    JSObject *p;
    JSTypedArray *ta;
    int size_log2;

    p = get_typed_array(ctx, this_val);
    if (!p)
        return JS_EXCEPTION;
    if (typed_array_is_oob(p))
        return JS_NewInt32(ctx, 0);
    ta = p->u.typed_array;
    if (!ta->track_rab)
        return JS_NewUint32(ctx, ta->length);
    size_log2 = typed_array_size_log2(p->class_id);
    return JS_NewInt64(ctx, (int64_t)p->u.array.count << size_log2);
}

static JSValue js_typed_array_get_byteOffset(JSContext *ctx,
                                             JSValueConst this_val)
{
    JSObject *p;
    JSTypedArray *ta;
    p = get_typed_array(ctx, this_val);
    if (!p)
        return JS_EXCEPTION;
    if (typed_array_is_oob(p))
        return JS_NewInt32(ctx, 0);
    ta = p->u.typed_array;
    return JS_NewUint32(ctx, ta->offset);
}

JSValue JS_NewTypedArray(JSContext *ctx, int argc, JSValueConst *argv,
                         JSTypedArrayEnum type)
{
    if (type < JS_TYPED_ARRAY_UINT8C || type > JS_TYPED_ARRAY_FLOAT64)
        return JS_ThrowRangeError(ctx, "invalid typed array type");

    return js_typed_array_constructor(ctx, JS_UNDEFINED, argc, argv,
                                      JS_CLASS_UINT8C_ARRAY + type);
}

/* Return the buffer associated to the typed array or an exception if
   it is not a typed array or if the buffer is detached. pbyte_offset,
   pbyte_length or pbytes_per_element can be NULL. */
JSValue JS_GetTypedArrayBuffer(JSContext *ctx, JSValueConst obj,
                               size_t *pbyte_offset,
                               size_t *pbyte_length,
                               size_t *pbytes_per_element)
{
    JSObject *p;
    JSTypedArray *ta;
    p = get_typed_array(ctx, obj);
    if (!p)
        return JS_EXCEPTION;
    if (typed_array_is_oob(p))
        return JS_ThrowTypeErrorArrayBufferOOB(ctx);
    ta = p->u.typed_array;
    if (pbyte_offset)
        *pbyte_offset = ta->offset;
    if (pbyte_length)
        *pbyte_length = ta->length;
    if (pbytes_per_element) {
        *pbytes_per_element = 1 << typed_array_size_log2(p->class_id);
    }
    return JS_DupValue(ctx, JS_MKPTR(JS_TAG_OBJECT, ta->buffer));
}

static JSValue js_typed_array_get_toStringTag(JSContext *ctx,
                                              JSValueConst this_val)
{
    JSObject *p;
    if (JS_VALUE_GET_TAG(this_val) != JS_TAG_OBJECT)
        return JS_UNDEFINED;
    p = JS_VALUE_GET_OBJ(this_val);
    if (!(p->class_id >= JS_CLASS_UINT8C_ARRAY &&
          p->class_id <= JS_CLASS_FLOAT64_ARRAY))
        return JS_UNDEFINED;
    return JS_AtomToString(ctx, ctx->rt->class_array[p->class_id].class_name);
}

static JSValue js_typed_array_set_internal(JSContext *ctx,
                                           JSValueConst dst,
                                           JSValueConst src,
                                           JSValueConst off)
{
    JSObject *p;
    JSObject *src_p;
    uint32_t i;
    int64_t dst_len, src_len, offset;
    JSValue val, src_obj = JS_UNDEFINED;

    p = get_typed_array(ctx, dst);
    if (!p)
        goto fail;
    if (JS_ToInt64Sat(ctx, &offset, off))
        goto fail;
    if (offset < 0)
        goto range_error;
    if (typed_array_is_oob(p)) {
    detached:
        JS_ThrowTypeErrorArrayBufferOOB(ctx);
        goto fail;
    }
    dst_len = p->u.array.count;
    src_obj = JS_ToObject(ctx, src);
    if (JS_IsException(src_obj))
        goto fail;
    src_p = JS_VALUE_GET_OBJ(src_obj);
    if (src_p->class_id >= JS_CLASS_UINT8C_ARRAY &&
        src_p->class_id <= JS_CLASS_FLOAT64_ARRAY) {
        JSTypedArray *dest_ta = p->u.typed_array;
        JSArrayBuffer *dest_abuf = dest_ta->buffer->u.array_buffer;
        JSTypedArray *src_ta = src_p->u.typed_array;
        JSArrayBuffer *src_abuf = src_ta->buffer->u.array_buffer;
        int shift = typed_array_size_log2(p->class_id);

        if (typed_array_is_oob(src_p))
            goto detached;

        src_len = src_p->u.array.count;
        if (offset > dst_len - src_len)
            goto range_error;

        /* copying between typed objects */
        if (src_p->class_id == p->class_id) {
            /* same type, use memmove */
            memmove(dest_abuf->data + dest_ta->offset + (offset << shift),
                    src_abuf->data + src_ta->offset, src_len << shift);
            goto done;
        }
        if (dest_abuf->data == src_abuf->data) {
            /* copying between the same buffer using different types of mappings
               would require a temporary buffer */
        }
        /* otherwise, default behavior is slow but correct */
    } else {
        // can change |dst| as a side effect; per spec,
        // perform the range check against its old length
        if (js_get_length64(ctx, &src_len, src_obj))
            goto fail;
        if (offset > dst_len - src_len) {
        range_error:
            JS_ThrowRangeError(ctx, "invalid array length");
            goto fail;
        }
    }
    for(i = 0; i < src_len; i++) {
        val = JS_GetPropertyUint32(ctx, src_obj, i);
        if (JS_IsException(val))
            goto fail;
        if (JS_SetPropertyUint32(ctx, dst, offset + i, val) < 0)
            goto fail;
    }
done:
    JS_FreeValue(ctx, src_obj);
    return JS_UNDEFINED;
fail:
    JS_FreeValue(ctx, src_obj);
    return JS_EXCEPTION;
}

static JSValue js_typed_array_at(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    JSObject *p;
    int64_t idx, len;

    p = get_typed_array(ctx, this_val);
    if (!p)
        return JS_EXCEPTION;

    if (typed_array_is_oob(p))
        return JS_ThrowTypeErrorDetachedArrayBuffer(ctx);
    len = p->u.array.count;

    // note: can change p->u.array.count
    if (JS_ToInt64Sat(ctx, &idx, argv[0]))
        return JS_EXCEPTION;

    if (idx < 0)
        idx = len + idx;

    len = p->u.array.count;
    if (idx < 0 || idx >= len)
        return JS_UNDEFINED;
    return JS_GetPropertyInt64(ctx, this_val, idx);
}

static JSValue js_typed_array_with(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    JSValue arr, val;
    JSObject *p;
    int64_t idx, len;

    p = get_typed_array(ctx, this_val);
    if (!p)
        return JS_EXCEPTION;
    if (typed_array_is_oob(p))
        return JS_ThrowTypeErrorDetachedArrayBuffer(ctx);

    len = p->u.array.count;
    if (JS_ToInt64Sat(ctx, &idx, argv[0]))
        return JS_EXCEPTION;

    if (idx < 0)
        idx = len + idx;

    val = JS_ToPrimitive(ctx, argv[1], HINT_NUMBER);
    if (JS_IsException(val))
        return JS_EXCEPTION;

    if (typed_array_is_oob(p) || idx < 0 || idx >= p->u.array.count)
        return JS_ThrowRangeError(ctx, "invalid array index");

    /* warning: 'this_val' may have been resized, so 'len' may be
       larger than its length */
    arr = js_typed_array_constructor_ta(ctx, JS_UNDEFINED, this_val,
                                        p->class_id, len);
    if (JS_IsException(arr)) {
        JS_FreeValue(ctx, val);
        return JS_EXCEPTION;
    }
    if (JS_SetPropertyInt64(ctx, arr, idx, val) < 0) {
        JS_FreeValue(ctx, arr);
        return JS_EXCEPTION;
    }
    return arr;
}

static JSValue js_typed_array_set(JSContext *ctx,
                                  JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    JSValueConst offset = JS_UNDEFINED;
    if (argc > 1) {
        offset = argv[1];
    }
    return js_typed_array_set_internal(ctx, this_val, argv[0], offset);
}

static JSValue js_create_typed_array_iterator(JSContext *ctx, JSValueConst this_val,
                                              int argc, JSValueConst *argv, int magic)
{
    if (validate_typed_array(ctx, this_val))
        return JS_EXCEPTION;
    return js_create_array_iterator(ctx, this_val, argc, argv, magic);
}

static JSValue js_typed_array_create(JSContext *ctx, JSValueConst ctor,
                                     int argc, JSValueConst *argv)
{
    JSValue ret;
    int new_len;
    int64_t len;

    ret = JS_CallConstructor(ctx, ctor, argc, argv);
    if (JS_IsException(ret))
        return ret;
    /* validate the typed array */
    new_len = js_typed_array_get_length_unsafe(ctx, ret);
    if (new_len < 0)
        goto fail;
    if (argc == 1) {
        /* ensure that it is large enough */
        if (JS_ToLengthFree(ctx, &len, JS_DupValue(ctx, argv[0])))
            goto fail;
        if (new_len < len) {
            JS_ThrowTypeError(ctx, "TypedArray length is too small");
        fail:
            JS_FreeValue(ctx, ret);
            return JS_EXCEPTION;
        }
    }
    return ret;
}

#if 0
static JSValue js_typed_array___create(JSContext *ctx,
                                       JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    return js_typed_array_create(ctx, argv[0], max_int(argc - 1, 0), argv + 1);
}
#endif

static JSValue js_typed_array___speciesCreate(JSContext *ctx,
                                              JSValueConst this_val,
                                              int argc, JSValueConst *argv)
{
    JSValueConst obj;
    JSObject *p;
    JSValue ctor, ret;
    int argc1;

    obj = argv[0];
    p = get_typed_array(ctx, obj);
    if (!p)
        return JS_EXCEPTION;
    ctor = JS_SpeciesConstructor(ctx, obj, JS_UNDEFINED);
    if (JS_IsException(ctor))
        return ctor;
    argc1 = max_int(argc - 1, 0);
    if (JS_IsUndefined(ctor)) {
        ret = js_typed_array_constructor(ctx, JS_UNDEFINED, argc1, argv + 1,
                                         p->class_id);
    } else {
        ret = js_typed_array_create(ctx, ctor, argc1, argv + 1);
        JS_FreeValue(ctx, ctor);
    }
    return ret;
}

static JSValue js_typed_array_from(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    // from(items, mapfn = void 0, this_arg = void 0)
    JSValueConst items = argv[0], mapfn, this_arg;
    JSValueConst args[2];
    JSValue iter, arr, r, v, v2;
    int64_t k, len;
    int mapping;

    mapping = FALSE;
    mapfn = JS_UNDEFINED;
    this_arg = JS_UNDEFINED;
    r = JS_UNDEFINED;
    arr = JS_UNDEFINED;
    iter = JS_UNDEFINED;

    if (argc > 1) {
        mapfn = argv[1];
        if (!JS_IsUndefined(mapfn)) {
            if (check_function(ctx, mapfn))
                goto exception;
            mapping = 1;
            if (argc > 2)
                this_arg = argv[2];
        }
    }
    iter = JS_GetProperty(ctx, items, JS_ATOM_Symbol_iterator);
    if (JS_IsException(iter))
        goto exception;
    if (!JS_IsUndefined(iter) && !JS_IsNull(iter)) {
        uint32_t len1;
        if (!JS_IsFunction(ctx, iter)) {
            JS_ThrowTypeError(ctx, "value is not iterable");
            goto exception;
        }
        arr = js_array_from_iterator(ctx, &len1, items, iter);
        if (JS_IsException(arr))
            goto exception;
        len = len1;
    } else {
        arr = JS_ToObject(ctx, items);
        if (JS_IsException(arr))
            goto exception;
        if (js_get_length64(ctx, &len, arr) < 0)
            goto exception;
    }
    v = JS_NewInt64(ctx, len);
    args[0] = v;
    r = js_typed_array_create(ctx, this_val, 1, args);
    JS_FreeValue(ctx, v);
    if (JS_IsException(r))
        goto exception;
    for(k = 0; k < len; k++) {
        v = JS_GetPropertyInt64(ctx, arr, k);
        if (JS_IsException(v))
            goto exception;
        if (mapping) {
            args[0] = v;
            args[1] = JS_NewInt32(ctx, k);
            v2 = JS_Call(ctx, mapfn, this_arg, 2, args);
            JS_FreeValue(ctx, v);
            v = v2;
            if (JS_IsException(v))
                goto exception;
        }
        if (JS_SetPropertyInt64(ctx, r, k, v) < 0)
            goto exception;
    }
    goto done;
 exception:
    JS_FreeValue(ctx, r);
    r = JS_EXCEPTION;
 done:
    JS_FreeValue(ctx, arr);
    JS_FreeValue(ctx, iter);
    return r;
}

static JSValue js_typed_array_of(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    JSValue obj;
    JSValueConst args[1];
    int i;

    args[0] = JS_NewInt32(ctx, argc);
    obj = js_typed_array_create(ctx, this_val, 1, args);
    if (JS_IsException(obj))
        return obj;

    for(i = 0; i < argc; i++) {
        if (JS_SetPropertyUint32(ctx, obj, i, JS_DupValue(ctx, argv[i])) < 0) {
            JS_FreeValue(ctx, obj);
            return JS_EXCEPTION;
        }
    }
    return obj;
}

static JSValue js_typed_array_copyWithin(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv)
{
    JSObject *p;
    int len, to, from, final, count, shift, space;

    p = get_typed_array(ctx, this_val);
    if (!p)
        return JS_EXCEPTION;
    if (typed_array_is_oob(p))
        return JS_ThrowTypeErrorArrayBufferOOB(ctx);
    len = p->u.array.count;

    if (JS_ToInt32Clamp(ctx, &to, argv[0], 0, len, len))
        return JS_EXCEPTION;

    if (JS_ToInt32Clamp(ctx, &from, argv[1], 0, len, len))
        return JS_EXCEPTION;

    final = len;
    if (argc > 2 && !JS_IsUndefined(argv[2])) {
        if (JS_ToInt32Clamp(ctx, &final, argv[2], 0, len, len))
            return JS_EXCEPTION;
    }

    if (typed_array_is_oob(p))
        return JS_ThrowTypeErrorArrayBufferOOB(ctx);

    // RAB may have been resized by evil .valueOf method
    space = p->u.array.count - max_int(to, from);
    count = min_int(final - from, len - to);
    count = min_int(count, space);
    if (count > 0) {
        shift = typed_array_size_log2(p->class_id);
        memmove(p->u.array.u.uint8_ptr + (to << shift),
                p->u.array.u.uint8_ptr + (from << shift),
                count << shift);
    }
    return JS_DupValue(ctx, this_val);
}

/* P1.22/P1.23: block-accelerated TypedArray fill / indexOf / lastIndexOf /
   includes for 2/4/8-byte elements. arm64 NEON when available; a hoisted-pointer
   scalar loop otherwise (the naive `p->u.array.u.T_ptr[k]=v` form defeats
   vectorization because the store may alias the reloaded base pointer). Semantics
   are identical to the scalar path: the NEON compare is only a coarse block filter;
   a scalar re-scan of the flagged block selects the exact first/last index, so the
   authoritative predicate is always the C `==` (correct NaN / -0 / +0 behavior). */
#if defined(__aarch64__) || defined(__ARM_NEON)
#include <arm_neon.h>
#define TA_NEON 1
#endif

/* Scan pv[k], pv[k+inc], ... up to (exclusive) stop for the first == v.
   inc is +1 (indexOf/includes) or -1 (lastIndexOf). Returns index or -1. */
static int ta_scan_u16(const uint16_t *pv, int k, int stop, int inc, uint16_t v)
{
    if (inc > 0) { /* forward: shared SIMD dispatch table */
        size_t r = simd.find_u16(pv + k, v, (size_t)(stop - k));
        return r == SIZE_MAX ? -1 : k + (int)r;
    }
    for (; k != stop; k += inc) if (pv[k] == v) return k; /* backward */
    return -1;
}

static int ta_scan_u32(const uint32_t *pv, int k, int stop, int inc, uint32_t v)
{
    if (inc > 0) { /* forward: shared SIMD dispatch table */
        size_t r = simd.find_u32(pv + k, v, (size_t)(stop - k));
        return r == SIZE_MAX ? -1 : k + (int)r;
    }
    for (; k != stop; k += inc) if (pv[k] == v) return k; /* backward */
    return -1;
}

static int ta_scan_u64(const uint64_t *pv, int k, int stop, int inc, uint64_t v)
{
#ifdef TA_NEON
    uint64x2_t vv = vdupq_n_u64(v);
    if (inc > 0) {
        for (; k + 2 <= stop; k += 2)
            if (vmaxvq_u32(vreinterpretq_u32_u64(vceqq_u64(vld1q_u64(pv + k), vv)))) {
                if (pv[k] == v) return k;
                if (pv[k + 1] == v) return k + 1;
            }
    } else {
        for (; k - 2 >= stop; k -= 2)
            if (vmaxvq_u32(vreinterpretq_u32_u64(vceqq_u64(vld1q_u64(pv + k - 1), vv)))) {
                if (pv[k] == v) return k;
                if (pv[k - 1] == v) return k - 1;
            }
    }
#endif
    for (; k != stop; k += inc) if (pv[k] == v) return k;
    return -1;
}

/* Float value scan (v is finite / non-NaN; FP compare gives IEEE -0 == +0). */
static int ta_scan_f32(const float *pv, int k, int stop, int inc, float v)
{
    if (inc > 0) { /* forward: shared SIMD dispatch table */
        size_t r = simd.find_f32(pv + k, v, (size_t)(stop - k));
        return r == SIZE_MAX ? -1 : k + (int)r;
    }
    for (; k != stop; k += inc) if (pv[k] == v) return k; /* backward */
    return -1;
}

static int ta_scan_f64(const double *pv, int k, int stop, int inc, double v)
{
    if (inc > 0) { /* forward: shared SIMD dispatch table */
        size_t r = simd.find_f64(pv + k, v, (size_t)(stop - k));
        return r == SIZE_MAX ? -1 : k + (int)r;
    }
    for (; k != stop; k += inc) if (pv[k] == v) return k; /* backward */
    return -1;
}

/* NaN scan for includes (SameValueZero: NaN matches NaN). Forward only:
   includes always scans forward. vceqq(x,x) is 0 exactly on NaN lanes. */
static int ta_scan_f32_nan(const float *pv, int k, int stop)
{
#ifdef TA_NEON
    for (; k + 4 <= stop; k += 4) {
        float32x4_t x = vld1q_f32(pv + k);
        if (vminvq_u32(vceqq_f32(x, x)) == 0)
            for (int j = k; j < k + 4; j++) if (isnan(pv[j])) return j;
    }
#endif
    for (; k < stop; k++) if (isnan(pv[k])) return k;
    return -1;
}

static int ta_scan_f64_nan(const double *pv, int k, int stop)
{
#ifdef TA_NEON
    for (; k + 2 <= stop; k += 2) {
        float64x2_t x = vld1q_f64(pv + k);
        if (vminvq_u32(vreinterpretq_u32_u64(vceqq_f64(x, x))) == 0) {
            if (isnan(pv[k])) return k;
            if (isnan(pv[k + 1])) return k + 1;
        }
    }
#endif
    for (; k < stop; k++) if (isnan(pv[k])) return k;
    return -1;
}

/* Block fill of [k, final) elements of 2^shift bytes, value in the low bytes of
   v64. Pointer is a local (not a reloaded struct field) so the fallback loop
   vectorizes; darwin uses libSystem's tuned memset_pattern. */
static void ta_block_fill(void *base, int k, int final, int shift, uint64_t v64)
{
    int n = final - k;
    if (n <= 0)
        return;
    switch (shift) {
    case 0:
        memset((uint8_t *)base + k, (uint8_t)v64, (size_t)n);
        break;
    case 1: {
        uint16_t *p = (uint16_t *)base + k, v = (uint16_t)v64;
#if defined(__APPLE__)
        uint32_t pat = (uint32_t)v | ((uint32_t)v << 16);
        memset_pattern4(p, &pat, (size_t)n * 2);
#else
        for (int i = 0; i < n; i++) p[i] = v;
#endif
        break;
    }
    case 2: {
        uint32_t *p = (uint32_t *)base + k, v = (uint32_t)v64;
#if defined(__APPLE__)
        memset_pattern4(p, &v, (size_t)n * 4);
#else
        for (int i = 0; i < n; i++) p[i] = v;
#endif
        break;
    }
    case 3: {
        uint64_t *p = (uint64_t *)base + k;
#if defined(__APPLE__)
        memset_pattern8(p, &v64, (size_t)n * 8);
#else
        for (int i = 0; i < n; i++) p[i] = v64;
#endif
        break;
    }
    default:
        abort();
    }
}

static JSValue js_typed_array_fill(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    JSObject *p;
    int len, k, final, shift;
    uint64_t v64;

    p = get_typed_array(ctx, this_val);
    if (!p)
        return JS_EXCEPTION;
    if (typed_array_is_oob(p))
        return JS_ThrowTypeErrorArrayBufferOOB(ctx);
    len = p->u.array.count;

    if (p->class_id == JS_CLASS_UINT8C_ARRAY) {
        int32_t v;
        if (JS_ToUint8ClampFree(ctx, &v, JS_DupValue(ctx, argv[0])))
            return JS_EXCEPTION;
        v64 = v;
    } else if (p->class_id <= JS_CLASS_UINT32_ARRAY) {
        uint32_t v;
        if (JS_ToUint32(ctx, &v, argv[0]))
            return JS_EXCEPTION;
        v64 = v;
    } else if (p->class_id <= JS_CLASS_BIG_UINT64_ARRAY) {
        if (JS_ToBigInt64(ctx, (int64_t *)&v64, argv[0]))
            return JS_EXCEPTION;
    } else {
        double d;
        if (JS_ToFloat64(ctx, &d, argv[0]))
            return JS_EXCEPTION;
        if (p->class_id == JS_CLASS_FLOAT16_ARRAY) {
            v64 = tofp16(d);
        } else if (p->class_id == JS_CLASS_FLOAT32_ARRAY) {
            union {
                float f;
                uint32_t u32;
            } u;
            u.f = d;
            v64 = u.u32;
        } else {
            JSFloat64Union u;
            u.d = d;
            v64 = u.u64;
        }
    }

    k = 0;
    if (argc > 1) {
        if (JS_ToInt32Clamp(ctx, &k, argv[1], 0, len, len))
            return JS_EXCEPTION;
    }

    final = len;
    if (argc > 2 && !JS_IsUndefined(argv[2])) {
        if (JS_ToInt32Clamp(ctx, &final, argv[2], 0, len, len))
            return JS_EXCEPTION;
    }

    if (typed_array_is_oob(p))
        return JS_ThrowTypeErrorArrayBufferOOB(ctx);

    // RAB may have been resized by evil .valueOf method
    final = min_int(final, p->u.array.count);
    shift = typed_array_size_log2(p->class_id);
    ta_block_fill(p->u.array.u.uint8_ptr, k, final, shift, v64);
    return JS_DupValue(ctx, this_val);
}

static JSValue js_typed_array_find(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv, int mode)
{
    JSValueConst func, this_arg;
    JSValueConst args[3];
    JSValue val, index_val, res;
    int len, k, end;
    int dir;

    val = JS_UNDEFINED;
    len = js_typed_array_get_length_unsafe(ctx, this_val);
    if (len < 0)
        goto exception;

    func = argv[0];
    if (check_function(ctx, func))
        goto exception;

    this_arg = JS_UNDEFINED;
    if (argc > 1)
        this_arg = argv[1];

    k = 0;
    dir = 1;
    end = len;
    if (mode == ArrayFindLast || mode == ArrayFindLastIndex) {
        k = len - 1;
        dir = -1;
        end = -1;
    }

    for(; k != end; k += dir) {
        index_val = JS_NewInt32(ctx, k);
        val = JS_GetPropertyValue(ctx, this_val, index_val);
        if (JS_IsException(val))
            goto exception;
        args[0] = val;
        args[1] = index_val;
        args[2] = this_val;
        res = JS_Call(ctx, func, this_arg, 3, args);
        if (JS_IsException(res))
            goto exception;
        if (JS_ToBoolFree(ctx, res)) {
            if (mode == ArrayFindIndex || mode == ArrayFindLastIndex) {
                JS_FreeValue(ctx, val);
                return index_val;
            } else {
                return val;
            }
        }
        JS_FreeValue(ctx, val);
    }
    if (mode == ArrayFindIndex || mode == ArrayFindLastIndex)
        return JS_NewInt32(ctx, -1);
    else
        return JS_UNDEFINED;

exception:
    JS_FreeValue(ctx, val);
    return JS_EXCEPTION;
}

#define special_indexOf 0
#define special_lastIndexOf 1
#define special_includes -1

static JSValue js_typed_array_indexOf(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv, int special)
{
    JSObject *p;
    int len, tag, is_int, is_bigint, k, stop, inc, res = -1;
    int64_t v64;
    double d;
    float f;
    uint16_t hf;

    p = get_typed_array(ctx, this_val);
    if (!p)
        return JS_EXCEPTION;
    if (typed_array_is_oob(p))
        return JS_ThrowTypeErrorArrayBufferOOB(ctx);
    len = p->u.array.count;

    if (len == 0)
        goto done;

    if (special == special_lastIndexOf) {
        k = len - 1;
        if (argc > 1) {
            int64_t k1;
            if (JS_ToInt64Clamp(ctx, &k1, argv[1], -1, len - 1, len))
                goto exception;
            k = k1;
            if (k < 0)
                goto done;
        }
        stop = -1;
        inc = -1;
    } else {
        k = 0;
        if (argc > 1) {
            if (JS_ToInt32Clamp(ctx, &k, argv[1], 0, len, len))
                goto exception;
        }
        stop = len;
        inc = 1;
    }

    /* includes function: 'undefined' can be found if searching out of bounds */
    if (len > p->u.array.count && special == special_includes &&
        JS_IsUndefined(argv[0]) && k < len) {
        res = 0;
        goto done;
    }

    // RAB may have been resized by evil .valueOf method
    len = min_int(len, p->u.array.count);
    if (len == 0)
        goto done;
    if (special == special_lastIndexOf)
        k = min_int(k, len - 1);
    else
        k = min_int(k, len);
    stop = min_int(stop, len);

    is_bigint = 0;
    is_int = 0; /* avoid warning */
    v64 = 0; /* avoid warning */
    tag = JS_VALUE_GET_NORM_TAG(argv[0]);
    if (tag == JS_TAG_INT) {
        is_int = 1;
        v64 = JS_VALUE_GET_INT(argv[0]);
        d = v64;
    } else
    if (tag == JS_TAG_FLOAT64) {
        d = JS_VALUE_GET_FLOAT64(argv[0]);
        if (d >= INT64_MIN && d < 0x1p63) {
            v64 = d;
            is_int = (v64 == d);
        }
    } else if (tag == JS_TAG_BIG_INT || tag == JS_TAG_SHORT_BIG_INT) {
        JSBigIntBuf buf1;
        JSBigInt *p1;
        int sz = (64 / JS_LIMB_BITS);
        if (tag == JS_TAG_SHORT_BIG_INT)
            p1 = js_bigint_set_short(&buf1, argv[0]);
        else
            p1 = JS_VALUE_GET_PTR(argv[0]);
        
        if (p->class_id == JS_CLASS_BIG_INT64_ARRAY) {
            if (p1->len > sz)
                goto done; /* does not fit an int64 : cannot be found */
        } else if (p->class_id == JS_CLASS_BIG_UINT64_ARRAY) {
            if (js_bigint_sign(p1))
                goto done; /* v < 0 */
            if (p1->len <= sz) {
                /* OK */
            } else if (p1->len == sz + 1 && p1->tab[sz] == 0) {
                /* 2^63 <= v <= 2^64-1 */
            } else {
                goto done;
            }
        } else {
            goto done;
        }
        if (JS_ToBigInt64(ctx, &v64, argv[0]))
            goto exception;
        d = 0;
        is_bigint = 1;
    } else {
        goto done;
    }

    switch (p->class_id) {
    case JS_CLASS_INT8_ARRAY:
        if (is_int && (int8_t)v64 == v64)
            goto scan8;
        break;
    case JS_CLASS_UINT8C_ARRAY:
    case JS_CLASS_UINT8_ARRAY:
        if (is_int && (uint8_t)v64 == v64) {
            const uint8_t *pv, *pp;
            uint16_t v;
        scan8:
            pv = p->u.array.u.uint8_ptr;
            v = v64;
            if (inc > 0) {
                pp = NULL;
                if (pv)
                    pp = memchr(pv + k, v, len - k);
                if (pp)
                    res = pp - pv;
            } else {
                for (; k != stop; k += inc) {
                    if (pv[k] == v) {
                        res = k;
                        break;
                    }
                }
            }
        }
        break;
    case JS_CLASS_INT16_ARRAY:
        if (is_int && (int16_t)v64 == v64)
            goto scan16;
        break;
    case JS_CLASS_UINT16_ARRAY:
        if (is_int && (uint16_t)v64 == v64) {
        scan16:
            res = ta_scan_u16(p->u.array.u.uint16_ptr, k, stop, inc, (uint16_t)v64);
        }
        break;
    case JS_CLASS_INT32_ARRAY:
        if (is_int && (int32_t)v64 == v64)
            goto scan32;
        break;
    case JS_CLASS_UINT32_ARRAY:
        if (is_int && (uint32_t)v64 == v64) {
        scan32:
            res = ta_scan_u32(p->u.array.u.uint32_ptr, k, stop, inc, (uint32_t)v64);
        }
        break;
    case JS_CLASS_FLOAT16_ARRAY:
        if (is_bigint)
            break;
        if (isnan(d)) {
            const uint16_t *pv = p->u.array.u.fp16_ptr;
            /* special case: indexOf returns -1, includes finds NaN */
            if (special != special_includes)
                goto done;
            for (; k != stop; k += inc) {
                if (isfp16nan(pv[k])) {
                    res = k;
                    break;
                }
            }
        } else if (d == 0) {
            // special case: includes also finds negative zero
            const uint16_t *pv = p->u.array.u.fp16_ptr;
            for (; k != stop; k += inc) {
                if (isfp16zero(pv[k])) {
                    res = k;
                    break;
                }
            }
        } else if (hf = tofp16(d), d == fromfp16(hf)) {
            /* exact nonzero non-NaN fp16 bit pattern: plain uint16 compare */
            res = ta_scan_u16(p->u.array.u.fp16_ptr, k, stop, inc, hf);
        }
        break;
    case JS_CLASS_FLOAT32_ARRAY:
        if (is_bigint)
            break;
        if (isnan(d)) {
            /* special case: indexOf returns -1, includes finds NaN (forward) */
            if (special != special_includes)
                goto done;
            res = ta_scan_f32_nan(p->u.array.u.float_ptr, k, stop);
        } else if ((f = (float)d) == d) {
            res = ta_scan_f32(p->u.array.u.float_ptr, k, stop, inc, f);
        }
        break;
    case JS_CLASS_FLOAT64_ARRAY:
        if (is_bigint)
            break;
        if (isnan(d)) {
            /* special case: indexOf returns -1, includes finds NaN (forward) */
            if (special != special_includes)
                goto done;
            res = ta_scan_f64_nan(p->u.array.u.double_ptr, k, stop);
        } else {
            res = ta_scan_f64(p->u.array.u.double_ptr, k, stop, inc, d);
        }
        break;
    case JS_CLASS_BIG_INT64_ARRAY:
        if (is_bigint) {
            goto scan64;
        }
        break;
    case JS_CLASS_BIG_UINT64_ARRAY:
        if (is_bigint) {
        scan64:
            res = ta_scan_u64(p->u.array.u.uint64_ptr, k, stop, inc, (uint64_t)v64);
        }
        break;
    }

done:
    if (special == special_includes)
        return JS_NewBool(ctx, res >= 0);
    else
        return JS_NewInt32(ctx, res);

exception:
    return JS_EXCEPTION;
}

static JSValue js_typed_array_join(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv, int toLocaleString)
{
    JSValue sep = JS_UNDEFINED, el;
    StringBuffer b_s, *b = &b_s;
    JSString *s = NULL;
    JSObject *p;
    int i, len, oldlen, newlen;
    int c;

    p = get_typed_array(ctx, this_val);
    if (!p)
        return JS_EXCEPTION;
    if (typed_array_is_oob(p))
        return JS_ThrowTypeErrorArrayBufferOOB(ctx);
    len = oldlen = newlen = p->u.array.count;

    c = ',';    /* default separator */
    if (!toLocaleString && argc > 0 && !JS_IsUndefined(argv[0])) {
        sep = JS_ToString(ctx, argv[0]);
        if (JS_IsException(sep))
            goto exception;
        s = JS_VALUE_GET_STRING(sep);
        if (s->len == 1 && !s->is_wide_char)
            c = s->u.str8[0];
        else
            c = -1;
        // ToString(sep) can detach or resize the arraybuffer as a side effect
        newlen = p->u.array.count;
        len = min_int(len, newlen);
    }
    string_buffer_init(ctx, b, 0);

    /* XXX: optimize with direct access */
    for(i = 0; i < len; i++) {
        if (i > 0) {
            if (c >= 0) {
                if (string_buffer_putc8(b, c))
                    goto fail;
            } else {
                if (string_buffer_concat(b, s, 0, s->len))
                    goto fail;
            }
        }
        el = JS_GetPropertyUint32(ctx, this_val, i);
        /* Can return undefined for example if the typed array is detached */
        if (!JS_IsNull(el) && !JS_IsUndefined(el)) {
            if (JS_IsException(el))
                goto fail;
            if (toLocaleString) {
                el = JS_ToLocaleStringFree(ctx, el);
            }
            if (string_buffer_concat_value_free(b, el))
                goto fail;
        }
    }

    // add extra separators in case RAB was resized by evil .valueOf method
    i = max_int(1, newlen);
    for(/*empty*/; i < oldlen; i++) {
        if (c >= 0) {
            if (string_buffer_putc8(b, c))
                goto fail;
        } else {
            if (string_buffer_concat(b, s, 0, s->len))
                goto fail;
        }
    }

    JS_FreeValue(ctx, sep);
    return string_buffer_end(b);

fail:
    string_buffer_free(b);
    JS_FreeValue(ctx, sep);
exception:
    return JS_EXCEPTION;
}

static JSValue js_typed_array_reverse(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    JSObject *p;
    int len;

    len = js_typed_array_get_length_unsafe(ctx, this_val);
    if (len < 0)
        return JS_EXCEPTION;
    if (len > 0) {
        p = JS_VALUE_GET_OBJ(this_val);
        switch (typed_array_size_log2(p->class_id)) {
        case 0:
            {
                uint8_t *p1 = p->u.array.u.uint8_ptr;
                uint8_t *p2 = p1 + len - 1;
                while (p1 < p2) {
                    uint8_t v = *p1;
                    *p1++ = *p2;
                    *p2-- = v;
                }
            }
            break;
        case 1:
            {
                uint16_t *p1 = p->u.array.u.uint16_ptr;
                uint16_t *p2 = p1 + len - 1;
                while (p1 < p2) {
                    uint16_t v = *p1;
                    *p1++ = *p2;
                    *p2-- = v;
                }
            }
            break;
        case 2:
            {
                uint32_t *p1 = p->u.array.u.uint32_ptr;
                uint32_t *p2 = p1 + len - 1;
                while (p1 < p2) {
                    uint32_t v = *p1;
                    *p1++ = *p2;
                    *p2-- = v;
                }
            }
            break;
        case 3:
            {
                uint64_t *p1 = p->u.array.u.uint64_ptr;
                uint64_t *p2 = p1 + len - 1;
                while (p1 < p2) {
                    uint64_t v = *p1;
                    *p1++ = *p2;
                    *p2-- = v;
                }
            }
            break;
        default:
            abort();
        }
    }
    return JS_DupValue(ctx, this_val);
}

static JSValue js_typed_array_toReversed(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv)
{
    JSValue arr, ret;
    JSObject *p;

    p = get_typed_array(ctx, this_val);
    if (!p)
        return JS_EXCEPTION;
    arr = js_typed_array_constructor_ta(ctx, JS_UNDEFINED, this_val,
                                        p->class_id, p->u.array.count);
    if (JS_IsException(arr))
        return JS_EXCEPTION;
    ret = js_typed_array_reverse(ctx, arr, argc, argv);
    JS_FreeValue(ctx, arr);
    return ret;
}

static void slice_memcpy(uint8_t *dst, const uint8_t *src, size_t len)
{
    if (dst + len <= src || dst >= src + len) {
        /* no overlap: can use memcpy */
        memcpy(dst, src, len);
    } else {
        /* otherwise the spec mandates byte copy */
        while (len-- != 0)
            *dst++ = *src++;
    }
}

static JSValue js_typed_array_slice(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    JSValueConst args[2];
    JSValue arr, val;
    JSObject *p, *p1;
    int n, len, start, final, count, shift, space;

    arr = JS_UNDEFINED;
    p = get_typed_array(ctx, this_val);
    if (!p)
        goto exception;
    if (typed_array_is_oob(p))
        return JS_ThrowTypeErrorArrayBufferOOB(ctx);
    len = p->u.array.count;

    if (JS_ToInt32Clamp(ctx, &start, argv[0], 0, len, len))
        goto exception;
    final = len;
    if (!JS_IsUndefined(argv[1])) {
        if (JS_ToInt32Clamp(ctx, &final, argv[1], 0, len, len))
            goto exception;
    }
    count = max_int(final - start, 0);

    shift = typed_array_size_log2(p->class_id);

    args[0] = this_val;
    args[1] = JS_NewInt32(ctx, count);
    arr = js_typed_array___speciesCreate(ctx, JS_UNDEFINED, 2, args);
    if (JS_IsException(arr))
        goto exception;

    if (count > 0) {
        if (validate_typed_array(ctx, this_val)
        ||  validate_typed_array(ctx, arr))
            goto exception;

        p1 = get_typed_array(ctx, arr);
        space = max_int(0, p->u.array.count - start);
        count = min_int(count, space);
        if (p1 != NULL && p->class_id == p1->class_id) {
            slice_memcpy(p1->u.array.u.uint8_ptr,
                         p->u.array.u.uint8_ptr + (start << shift),
                         count << shift);
        } else {
            for (n = 0; n < count; n++) {
                val = JS_GetPropertyValue(ctx, this_val, JS_NewInt32(ctx, start + n));
                if (JS_IsException(val))
                    goto exception;
                if (JS_SetPropertyValue(ctx, arr, JS_NewInt32(ctx, n), val,
                                        JS_PROP_THROW) < 0)
                    goto exception;
            }
        }
    }
    return arr;

 exception:
    JS_FreeValue(ctx, arr);
    return JS_EXCEPTION;
}

static JSValue js_typed_array_subarray(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    JSValueConst args[4];
    JSValue arr, ta_buffer;
    JSTypedArray *ta;
    JSObject *p;
    int len, start, final, count, shift, offset;
    BOOL is_auto;
        
    p = get_typed_array(ctx, this_val);
    if (!p)
        goto exception;
    len = p->u.array.count;
    if (JS_ToInt32Clamp(ctx, &start, argv[0], 0, len, len))
        goto exception;

    shift = typed_array_size_log2(p->class_id);
    ta = p->u.typed_array;
    /* Read byteOffset (ta->offset) even if detached */
    offset = ta->offset + (start << shift);

    final = len;
    if (JS_IsUndefined(argv[1])) {
        is_auto = ta->track_rab;
    } else {
        is_auto = FALSE;
        if (JS_ToInt32Clamp(ctx, &final, argv[1], 0, len, len))
            goto exception;
    } 
    count = max_int(final - start, 0);
    ta_buffer = js_typed_array_get_buffer(ctx, this_val);
    if (JS_IsException(ta_buffer))
        goto exception;
    args[0] = this_val;
    args[1] = ta_buffer;
    args[2] = JS_NewInt32(ctx, offset);
    args[3] = JS_NewInt32(ctx, count);
    arr = js_typed_array___speciesCreate(ctx, JS_UNDEFINED, is_auto ? 3 : 4, args);
    JS_FreeValue(ctx, ta_buffer);
    return arr;

 exception:
    return JS_EXCEPTION;
}

/* TypedArray.prototype.sort */

static int js_cmp_doubles(double x, double y)
{
    if (isnan(x))    return isnan(y) ? 0 : +1;
    if (isnan(y))    return -1;
    if (x < y)       return -1;
    if (x > y)       return 1;
    if (x != 0)      return 0;
    if (signbit(x))  return signbit(y) ? 0 : -1;
    else             return signbit(y) ? 1 : 0;
}

static int js_TA_cmp_int8(const void *a, const void *b, void *opaque) {
    return *(const int8_t *)a - *(const int8_t *)b;
}

static int js_TA_cmp_uint8(const void *a, const void *b, void *opaque) {
    return *(const uint8_t *)a - *(const uint8_t *)b;
}

static int js_TA_cmp_int16(const void *a, const void *b, void *opaque) {
    return *(const int16_t *)a - *(const int16_t *)b;
}

static int js_TA_cmp_uint16(const void *a, const void *b, void *opaque) {
    return *(const uint16_t *)a - *(const uint16_t *)b;
}

static int js_TA_cmp_int32(const void *a, const void *b, void *opaque) {
    int32_t x = *(const int32_t *)a;
    int32_t y = *(const int32_t *)b;
    return (y < x) - (y > x);
}

static int js_TA_cmp_uint32(const void *a, const void *b, void *opaque) {
    uint32_t x = *(const uint32_t *)a;
    uint32_t y = *(const uint32_t *)b;
    return (y < x) - (y > x);
}

static int js_TA_cmp_int64(const void *a, const void *b, void *opaque) {
    int64_t x = *(const int64_t *)a;
    int64_t y = *(const int64_t *)b;
    return (y < x) - (y > x);
}

static int js_TA_cmp_uint64(const void *a, const void *b, void *opaque) {
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;
    return (y < x) - (y > x);
}

static int js_TA_cmp_float16(const void *a, const void *b, void *opaque) {
    return js_cmp_doubles(fromfp16(*(const uint16_t *)a),
                          fromfp16(*(const uint16_t *)b));
}

static int js_TA_cmp_float32(const void *a, const void *b, void *opaque) {
    return js_cmp_doubles(*(const float *)a, *(const float *)b);
}

static int js_TA_cmp_float64(const void *a, const void *b, void *opaque) {
    return js_cmp_doubles(*(const double *)a, *(const double *)b);
}

static JSValue js_TA_get_int8(JSContext *ctx, const void *a) {
    return JS_NewInt32(ctx, *(const int8_t *)a);
}

static JSValue js_TA_get_uint8(JSContext *ctx, const void *a) {
    return JS_NewInt32(ctx, *(const uint8_t *)a);
}

static JSValue js_TA_get_int16(JSContext *ctx, const void *a) {
    return JS_NewInt32(ctx, *(const int16_t *)a);
}

static JSValue js_TA_get_uint16(JSContext *ctx, const void *a) {
    return JS_NewInt32(ctx, *(const uint16_t *)a);
}

static JSValue js_TA_get_int32(JSContext *ctx, const void *a) {
    return JS_NewInt32(ctx, *(const int32_t *)a);
}

static JSValue js_TA_get_uint32(JSContext *ctx, const void *a) {
    return JS_NewUint32(ctx, *(const uint32_t *)a);
}

static JSValue js_TA_get_int64(JSContext *ctx, const void *a) {
    return JS_NewBigInt64(ctx, *(int64_t *)a);
}

static JSValue js_TA_get_uint64(JSContext *ctx, const void *a) {
    return JS_NewBigUint64(ctx, *(uint64_t *)a);
}

static JSValue js_TA_get_float16(JSContext *ctx, const void *a) {
    return __JS_NewFloat64(ctx, fromfp16(*(const uint16_t *)a));
}

static JSValue js_TA_get_float32(JSContext *ctx, const void *a) {
    return __JS_NewFloat64(ctx, *(const float *)a);
}

static JSValue js_TA_get_float64(JSContext *ctx, const void *a) {
    return __JS_NewFloat64(ctx, *(const double *)a);
}

struct TA_sort_context {
    JSContext *ctx;
    int exception; /* 1 = exception, 2 = detached typed array */
    uint8_t *array;
    JSValueConst cmp;
    JSValue (*getfun)(JSContext *ctx, const void *a);
    int elt_size;
};

static int js_TA_cmp_generic(const void *a, const void *b, void *opaque) {
    struct TA_sort_context *psc = opaque;
    JSContext *ctx = psc->ctx;
    uint32_t a_idx, b_idx;
    JSValueConst argv[2];
    JSValue res;
    int cmp;
    
    cmp = 0;
    if (!psc->exception) {
        /* Note: the typed array can be detached without causing an
           error */
        a_idx = *(uint32_t *)a;
        b_idx = *(uint32_t *)b;
        argv[0] = psc->getfun(ctx, psc->array +
                              a_idx * (size_t)psc->elt_size);
        argv[1] = psc->getfun(ctx, psc->array +
                              b_idx * (size_t)(psc->elt_size));
        res = JS_Call(ctx, psc->cmp, JS_UNDEFINED, 2, argv);
        if (JS_IsException(res)) {
            psc->exception = 1;
            goto done;
        }
        if (JS_VALUE_GET_TAG(res) == JS_TAG_INT) {
            int val = JS_VALUE_GET_INT(res);
            cmp = (val > 0) - (val < 0);
        } else {
            double val;
            if (JS_ToFloat64Free(ctx, &val, res) < 0) {
                psc->exception = 1;
                goto done;
            } else {
                cmp = (val > 0) - (val < 0);
            }
        }
        if (cmp == 0) {
            /* make sort stable: compare array offsets */
            cmp = (a_idx > b_idx) - (a_idx < b_idx);
        }
    done:
        JS_FreeValue(ctx, (JSValue)argv[0]);
        JS_FreeValue(ctx, (JSValue)argv[1]);
    }
    return cmp;
}

static JSValue js_typed_array_sort(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    JSObject *p;
    int len;
    size_t elt_size;
    struct TA_sort_context tsc;
    int (*cmpfun)(const void *a, const void *b, void *opaque);

    tsc.ctx = ctx;
    tsc.exception = 0;
    tsc.cmp = argv[0];

    if (!JS_IsUndefined(tsc.cmp) && check_function(ctx, tsc.cmp))
        return JS_EXCEPTION;
    len = js_typed_array_get_length_unsafe(ctx, this_val);
    if (len < 0)
        return JS_EXCEPTION;

    if (len > 1) {
        p = JS_VALUE_GET_OBJ(this_val);
        switch (p->class_id) {
        case JS_CLASS_INT8_ARRAY:
            tsc.getfun = js_TA_get_int8;
            cmpfun = js_TA_cmp_int8;
            break;
        case JS_CLASS_UINT8C_ARRAY:
        case JS_CLASS_UINT8_ARRAY:
            tsc.getfun = js_TA_get_uint8;
            cmpfun = js_TA_cmp_uint8;
            break;
        case JS_CLASS_INT16_ARRAY:
            tsc.getfun = js_TA_get_int16;
            cmpfun = js_TA_cmp_int16;
            break;
        case JS_CLASS_UINT16_ARRAY:
            tsc.getfun = js_TA_get_uint16;
            cmpfun = js_TA_cmp_uint16;
            break;
        case JS_CLASS_INT32_ARRAY:
            tsc.getfun = js_TA_get_int32;
            cmpfun = js_TA_cmp_int32;
            break;
        case JS_CLASS_UINT32_ARRAY:
            tsc.getfun = js_TA_get_uint32;
            cmpfun = js_TA_cmp_uint32;
            break;
        case JS_CLASS_BIG_INT64_ARRAY:
            tsc.getfun = js_TA_get_int64;
            cmpfun = js_TA_cmp_int64;
            break;
        case JS_CLASS_BIG_UINT64_ARRAY:
            tsc.getfun = js_TA_get_uint64;
            cmpfun = js_TA_cmp_uint64;
            break;
        case JS_CLASS_FLOAT16_ARRAY:
            tsc.getfun = js_TA_get_float16;
            cmpfun = js_TA_cmp_float16;
            break;
        case JS_CLASS_FLOAT32_ARRAY:
            tsc.getfun = js_TA_get_float32;
            cmpfun = js_TA_cmp_float32;
            break;
        case JS_CLASS_FLOAT64_ARRAY:
            tsc.getfun = js_TA_get_float64;
            cmpfun = js_TA_cmp_float64;
            break;
        default:
            abort();
        }
        elt_size = 1 << typed_array_size_log2(p->class_id);
        if (!JS_IsUndefined(tsc.cmp)) {
            uint32_t *array_idx;
            void *array;
            size_t i, j;

            /* the array must be copied because the comparison
               function may modify it */
            array = js_malloc(ctx, len * elt_size);
            if (!array)
                return JS_EXCEPTION;
            memcpy(array, p->u.array.u.ptr, len * elt_size);
            
            /* array_idx is needed to have a stable sort */
            array_idx = js_malloc(ctx, len * sizeof(array_idx[0]));
            if (!array_idx) {
                js_free(ctx, array);
                return JS_EXCEPTION;
            }
            for(i = 0; i < len; i++)
                array_idx[i] = i;
            tsc.elt_size = elt_size;
            tsc.array = array;
            rqsort(array_idx, len, sizeof(array_idx[0]),
                   js_TA_cmp_generic, &tsc);
            if (tsc.exception) {
                if (tsc.exception == 1) {
                    js_free(ctx, array_idx);
                    js_free(ctx, array);
                    return JS_EXCEPTION;
                }
                /* detached typed array during the sort: no error */
            } else {
                void *array_ptr = p->u.array.u.ptr;
                len = min_int(len, p->u.array.count);
                switch(elt_size) {
                case 1:
                    for(i = 0; i < len; i++) {
                        j = array_idx[i];
                        ((uint8_t *)array_ptr)[i] = ((uint8_t *)array)[j];
                    }
                    break;
                case 2:
                    for(i = 0; i < len; i++) {
                        j = array_idx[i];
                        ((uint16_t *)array_ptr)[i] = ((uint16_t *)array)[j];
                    }
                    break;
                case 4:
                    for(i = 0; i < len; i++) {
                        j = array_idx[i];
                        ((uint32_t *)array_ptr)[i] = ((uint32_t *)array)[j];
                    }
                    break;
                case 8:
                    for(i = 0; i < len; i++) {
                        j = array_idx[i];
                        ((uint64_t *)array_ptr)[i] = ((uint64_t *)array)[j];
                    }
                    break;
                default:
                    abort();
                }
            }
            js_free(ctx, array_idx);
            js_free(ctx, array);
        } else {
            rqsort(p->u.array.u.ptr, len, elt_size, cmpfun, &tsc);
            if (tsc.exception)
                return JS_EXCEPTION;
        }
    }
    return JS_DupValue(ctx, this_val);
}

static JSValue js_typed_array_toSorted(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    JSValue arr, ret;
    JSObject *p;

    p = get_typed_array(ctx, this_val);
    if (!p)
        return JS_EXCEPTION;
    arr = js_typed_array_constructor_ta(ctx, JS_UNDEFINED, this_val,
                                        p->class_id, p->u.array.count);
    if (JS_IsException(arr))
        return JS_EXCEPTION;
    ret = js_typed_array_sort(ctx, arr, argc, argv);
    JS_FreeValue(ctx, arr);
    return ret;
}

/* Uint8Array base64/hex (tc39 proposal-arraybuffer-base64) */

enum {
    B64_ALPHABET_BASE64 = 0,
    B64_ALPHABET_BASE64URL = 1,
};

enum {
    B64_LAST_LOOSE = 0,
    B64_LAST_STRICT = 1,
    B64_LAST_STOP_BEFORE_PARTIAL = 2,
};

static const unsigned char b64_enc[64] = {
    'A','B','C','D','E','F','G','H','I','J','K','L','M','N','O','P',
    'Q','R','S','T','U','V','W','X','Y','Z',
    'a','b','c','d','e','f','g','h','i','j','k','l','m','n','o','p',
    'q','r','s','t','u','v','w','x','y','z',
    '0','1','2','3','4','5','6','7','8','9',
    '+','/'
};

static const unsigned char b64url_enc[64] = {
    'A','B','C','D','E','F','G','H','I','J','K','L','M','N','O','P',
    'Q','R','S','T','U','V','W','X','Y','Z',
    'a','b','c','d','e','f','g','h','i','j','k','l','m','n','o','p',
    'q','r','s','t','u','v','w','x','y','z',
    '0','1','2','3','4','5','6','7','8','9',
    '-','_'
};

#define K_WS 64
#define K_ER 65

static const uint8_t b64_dec[256] = {
 [  0]=K_ER, [  1]=K_ER, [  2]=K_ER, [  3]=K_ER, [  4]=K_ER, [  5]=K_ER, [  6]=K_ER, [  7]=K_ER,
 [  8]=K_ER, [  9]=K_WS, [ 10]=K_WS, [ 11]=K_ER, [ 12]=K_WS, [ 13]=K_WS, [ 14]=K_ER, [ 15]=K_ER,
 [ 16]=K_ER, [ 17]=K_ER, [ 18]=K_ER, [ 19]=K_ER, [ 20]=K_ER, [ 21]=K_ER, [ 22]=K_ER, [ 23]=K_ER,
 [ 24]=K_ER, [ 25]=K_ER, [ 26]=K_ER, [ 27]=K_ER, [ 28]=K_ER, [ 29]=K_ER, [ 30]=K_ER, [ 31]=K_ER,
 [' ']=K_WS, ['!']=K_ER, ['"']=K_ER, ['#']=K_ER, ['$']=K_ER, ['%']=K_ER, ['&']=K_ER, [ 39]=K_ER,
 ['(']=K_ER, [')']=K_ER, ['*']=K_ER, ['+']=  62, [',']=K_ER, ['-']=K_ER, ['.']=K_ER, ['/']=  63,
 ['0']=  52, ['1']=  53, ['2']=  54, ['3']=  55, ['4']=  56, ['5']=  57, ['6']=  58, ['7']=  59,
 ['8']=  60, ['9']=  61, [':']=K_ER, [';']=K_ER, ['<']=K_ER, ['=']=K_ER, ['>']=K_ER, ['?']=K_ER,
 ['@']=K_ER, ['A']=   0, ['B']=   1, ['C']=   2, ['D']=   3, ['E']=   4, ['F']=   5, ['G']=   6,
 ['H']=   7, ['I']=   8, ['J']=   9, ['K']=  10, ['L']=  11, ['M']=  12, ['N']=  13, ['O']=  14,
 ['P']=  15, ['Q']=  16, ['R']=  17, ['S']=  18, ['T']=  19, ['U']=  20, ['V']=  21, ['W']=  22,
 ['X']=  23, ['Y']=  24, ['Z']=  25, ['[']=K_ER, [ 92]=K_ER, [']']=K_ER, ['^']=K_ER, ['_']=K_ER,
 ['`']=K_ER, ['a']=  26, ['b']=  27, ['c']=  28, ['d']=  29, ['e']=  30, ['f']=  31, ['g']=  32,
 ['h']=  33, ['i']=  34, ['j']=  35, ['k']=  36, ['l']=  37, ['m']=  38, ['n']=  39, ['o']=  40,
 ['p']=  41, ['q']=  42, ['r']=  43, ['s']=  44, ['t']=  45, ['u']=  46, ['v']=  47, ['w']=  48,
 ['x']=  49, ['y']=  50, ['z']=  51, ['{']=K_ER, ['|']=K_ER, ['}']=K_ER, ['~']=K_ER, [127]=K_ER,
 [128]=K_ER, [129]=K_ER, [130]=K_ER, [131]=K_ER, [132]=K_ER, [133]=K_ER, [134]=K_ER, [135]=K_ER,
 [136]=K_ER, [137]=K_ER, [138]=K_ER, [139]=K_ER, [140]=K_ER, [141]=K_ER, [142]=K_ER, [143]=K_ER,
 [144]=K_ER, [145]=K_ER, [146]=K_ER, [147]=K_ER, [148]=K_ER, [149]=K_ER, [150]=K_ER, [151]=K_ER,
 [152]=K_ER, [153]=K_ER, [154]=K_ER, [155]=K_ER, [156]=K_ER, [157]=K_ER, [158]=K_ER, [159]=K_ER,
 [160]=K_ER, [161]=K_ER, [162]=K_ER, [163]=K_ER, [164]=K_ER, [165]=K_ER, [166]=K_ER, [167]=K_ER,
 [168]=K_ER, [169]=K_ER, [170]=K_ER, [171]=K_ER, [172]=K_ER, [173]=K_ER, [174]=K_ER, [175]=K_ER,
 [176]=K_ER, [177]=K_ER, [178]=K_ER, [179]=K_ER, [180]=K_ER, [181]=K_ER, [182]=K_ER, [183]=K_ER,
 [184]=K_ER, [185]=K_ER, [186]=K_ER, [187]=K_ER, [188]=K_ER, [189]=K_ER, [190]=K_ER, [191]=K_ER,
 [192]=K_ER, [193]=K_ER, [194]=K_ER, [195]=K_ER, [196]=K_ER, [197]=K_ER, [198]=K_ER, [199]=K_ER,
 [200]=K_ER, [201]=K_ER, [202]=K_ER, [203]=K_ER, [204]=K_ER, [205]=K_ER, [206]=K_ER, [207]=K_ER,
 [208]=K_ER, [209]=K_ER, [210]=K_ER, [211]=K_ER, [212]=K_ER, [213]=K_ER, [214]=K_ER, [215]=K_ER,
 [216]=K_ER, [217]=K_ER, [218]=K_ER, [219]=K_ER, [220]=K_ER, [221]=K_ER, [222]=K_ER, [223]=K_ER,
 [224]=K_ER, [225]=K_ER, [226]=K_ER, [227]=K_ER, [228]=K_ER, [229]=K_ER, [230]=K_ER, [231]=K_ER,
 [232]=K_ER, [233]=K_ER, [234]=K_ER, [235]=K_ER, [236]=K_ER, [237]=K_ER, [238]=K_ER, [239]=K_ER,
 [240]=K_ER, [241]=K_ER, [242]=K_ER, [243]=K_ER, [244]=K_ER, [245]=K_ER, [246]=K_ER, [247]=K_ER,
 [248]=K_ER, [249]=K_ER, [250]=K_ER, [251]=K_ER, [252]=K_ER, [253]=K_ER, [254]=K_ER, [255]=K_ER,
};

static const uint8_t b64url_dec[256] = {
 [  0]=K_ER, [  1]=K_ER, [  2]=K_ER, [  3]=K_ER, [  4]=K_ER, [  5]=K_ER, [  6]=K_ER, [  7]=K_ER,
 [  8]=K_ER, [  9]=K_WS, [ 10]=K_WS, [ 11]=K_ER, [ 12]=K_WS, [ 13]=K_WS, [ 14]=K_ER, [ 15]=K_ER,
 [ 16]=K_ER, [ 17]=K_ER, [ 18]=K_ER, [ 19]=K_ER, [ 20]=K_ER, [ 21]=K_ER, [ 22]=K_ER, [ 23]=K_ER,
 [ 24]=K_ER, [ 25]=K_ER, [ 26]=K_ER, [ 27]=K_ER, [ 28]=K_ER, [ 29]=K_ER, [ 30]=K_ER, [ 31]=K_ER,
 [' ']=K_WS, ['!']=K_ER, ['"']=K_ER, ['#']=K_ER, ['$']=K_ER, ['%']=K_ER, ['&']=K_ER, [ 39]=K_ER,
 ['(']=K_ER, [')']=K_ER, ['*']=K_ER, ['+']=K_ER, [',']=K_ER, ['-']=  62, ['.']=K_ER, ['/']=K_ER,
 ['0']=  52, ['1']=  53, ['2']=  54, ['3']=  55, ['4']=  56, ['5']=  57, ['6']=  58, ['7']=  59,
 ['8']=  60, ['9']=  61, [':']=K_ER, [';']=K_ER, ['<']=K_ER, ['=']=K_ER, ['>']=K_ER, ['?']=K_ER,
 ['@']=K_ER, ['A']=   0, ['B']=   1, ['C']=   2, ['D']=   3, ['E']=   4, ['F']=   5, ['G']=   6,
 ['H']=   7, ['I']=   8, ['J']=   9, ['K']=  10, ['L']=  11, ['M']=  12, ['N']=  13, ['O']=  14,
 ['P']=  15, ['Q']=  16, ['R']=  17, ['S']=  18, ['T']=  19, ['U']=  20, ['V']=  21, ['W']=  22,
 ['X']=  23, ['Y']=  24, ['Z']=  25, ['[']=K_ER, [ 92]=K_ER, [']']=K_ER, ['^']=K_ER, ['_']=  63,
 ['`']=K_ER, ['a']=  26, ['b']=  27, ['c']=  28, ['d']=  29, ['e']=  30, ['f']=  31, ['g']=  32,
 ['h']=  33, ['i']=  34, ['j']=  35, ['k']=  36, ['l']=  37, ['m']=  38, ['n']=  39, ['o']=  40,
 ['p']=  41, ['q']=  42, ['r']=  43, ['s']=  44, ['t']=  45, ['u']=  46, ['v']=  47, ['w']=  48,
 ['x']=  49, ['y']=  50, ['z']=  51, ['{']=K_ER, ['|']=K_ER, ['}']=K_ER, ['~']=K_ER, [127]=K_ER,
 [128]=K_ER, [129]=K_ER, [130]=K_ER, [131]=K_ER, [132]=K_ER, [133]=K_ER, [134]=K_ER, [135]=K_ER,
 [136]=K_ER, [137]=K_ER, [138]=K_ER, [139]=K_ER, [140]=K_ER, [141]=K_ER, [142]=K_ER, [143]=K_ER,
 [144]=K_ER, [145]=K_ER, [146]=K_ER, [147]=K_ER, [148]=K_ER, [149]=K_ER, [150]=K_ER, [151]=K_ER,
 [152]=K_ER, [153]=K_ER, [154]=K_ER, [155]=K_ER, [156]=K_ER, [157]=K_ER, [158]=K_ER, [159]=K_ER,
 [160]=K_ER, [161]=K_ER, [162]=K_ER, [163]=K_ER, [164]=K_ER, [165]=K_ER, [166]=K_ER, [167]=K_ER,
 [168]=K_ER, [169]=K_ER, [170]=K_ER, [171]=K_ER, [172]=K_ER, [173]=K_ER, [174]=K_ER, [175]=K_ER,
 [176]=K_ER, [177]=K_ER, [178]=K_ER, [179]=K_ER, [180]=K_ER, [181]=K_ER, [182]=K_ER, [183]=K_ER,
 [184]=K_ER, [185]=K_ER, [186]=K_ER, [187]=K_ER, [188]=K_ER, [189]=K_ER, [190]=K_ER, [191]=K_ER,
 [192]=K_ER, [193]=K_ER, [194]=K_ER, [195]=K_ER, [196]=K_ER, [197]=K_ER, [198]=K_ER, [199]=K_ER,
 [200]=K_ER, [201]=K_ER, [202]=K_ER, [203]=K_ER, [204]=K_ER, [205]=K_ER, [206]=K_ER, [207]=K_ER,
 [208]=K_ER, [209]=K_ER, [210]=K_ER, [211]=K_ER, [212]=K_ER, [213]=K_ER, [214]=K_ER, [215]=K_ER,
 [216]=K_ER, [217]=K_ER, [218]=K_ER, [219]=K_ER, [220]=K_ER, [221]=K_ER, [222]=K_ER, [223]=K_ER,
 [224]=K_ER, [225]=K_ER, [226]=K_ER, [227]=K_ER, [228]=K_ER, [229]=K_ER, [230]=K_ER, [231]=K_ER,
 [232]=K_ER, [233]=K_ER, [234]=K_ER, [235]=K_ER, [236]=K_ER, [237]=K_ER, [238]=K_ER, [239]=K_ER,
 [240]=K_ER, [241]=K_ER, [242]=K_ER, [243]=K_ER, [244]=K_ER, [245]=K_ER, [246]=K_ER, [247]=K_ER,
 [248]=K_ER, [249]=K_ER, [250]=K_ER, [251]=K_ER, [252]=K_ER, [253]=K_ER, [254]=K_ER, [255]=K_ER,
};
 
static size_t b64_encode(const uint8_t *src, size_t len, char *dst,
                         const unsigned char *alpha)
{
    size_t i, j;

    for (i = 0, j = 0; i + 3 <= len; i += 3, j += 4) {
        uint32_t v = 65536*src[i] + 256*src[i + 1] + src[i + 2];
        dst[j + 0] = alpha[(v >> 18) & 63];
        dst[j + 1] = alpha[(v >> 12) & 63];
        dst[j + 2] = alpha[(v >> 6) & 63];
        dst[j + 3] = alpha[v & 63];
    }

    size_t rem = len - i;
    if (rem == 1) {
        uint32_t v = 65536*src[i];
        dst[j++] = alpha[(v >> 18) & 63];
        dst[j++] = alpha[(v >> 12) & 63];
        dst[j++] = '=';
        dst[j++] = '=';
    } else if (rem == 2) {
        uint32_t v = 65536*src[i] + 256*src[i + 1];
        dst[j++] = alpha[(v >> 18) & 63];
        dst[j++] = alpha[(v >> 12) & 63];
        dst[j++] = alpha[(v >> 6) & 63];
        dst[j++] = '=';
    }
    return j;
}

static size_t b64_skip_ws(const char *src, size_t len, size_t index,
                          const uint8_t *dec_table)
{
    while (index < len && dec_table[(unsigned char)src[index]] == K_WS)
        index++;
    return index;
}

/* Implements the FromBase64 abstract operation.
   src/src_len: the input string (must be ASCII/latin1)
   dst/max_len: output buffer
   flags: b64_flags or b64_flags_url (selects valid characters)
   last_chunk: B64_LAST_LOOSE, B64_LAST_STRICT, or B64_LAST_STOP_BEFORE_PARTIAL
   *p_read: set to number of input characters consumed
   *p_err: set to 1 on error, 0 on success
   Returns: number of bytes written to dst */
static size_t from_base64(const char *src, size_t src_len,
                          uint8_t *dst, size_t max_len,
                          const uint8_t *dec_table, int last_chunk,
                          size_t *p_read, int *p_err)
{
    size_t read = 0, written = 0;
    uint32_t v, acc = 0;
    int seen = 0;
    size_t index = 0;
    uint8_t ch;
    
    *p_err = 0;

    if (max_len == 0) {
        *p_read = 0;
        return 0;
    }

    for (;;) {
        if (seen == 0) {
            /* Fast path: decode complete groups of 4 valid characters.
               Breaks out on whitespace, padding, invalid chars, or capacity. */
            while (index + 4 <= src_len && written + 3 <= max_len) {
                uint32_t v0, v1, v2, v3;
                v0 = dec_table[(unsigned char)src[index]];
                v1 = dec_table[(unsigned char)src[index + 1]];
                v2 = dec_table[(unsigned char)src[index + 2]];
                v3 = dec_table[(unsigned char)src[index + 3]];
                if ((v0 | v1 | v2 | v3) >= 64)
                    break;
                v = (v0 << 18) | (v1 << 12) | (v2 << 6) | v3;
                dst[written]     = (uint8_t)(v >> 16);
                dst[written + 1] = (uint8_t)(v >> 8);
                dst[written + 2] = (uint8_t)(v);
                written += 3;
                index += 4;
            }
            read = index;
            
            if (written >= max_len) {
                *p_read = read;
                return written;
            }
        }
        
        /* Slow path: handle whitespace, padding, partial groups, capacity. */
        index = b64_skip_ws(src, src_len, index, dec_table);

        if (index == src_len) {
            if (seen > 0) {
                if (last_chunk == B64_LAST_STOP_BEFORE_PARTIAL) {
                    *p_read = read;
                    return written;
                }
                if (last_chunk == B64_LAST_STRICT) {
                    *p_err = 1;
                    return 0;
                }
                /* loose */
                if (seen == 1) {
                    *p_err = 1;
                    return 0;
                }
                break;
            }
            *p_read = src_len;
            return written;
        }

        ch = src[index++];

        if (ch == '=') {
            if (seen < 2) {
                *p_err = 1;
                return 0;
            }
            index = b64_skip_ws(src, src_len, index, dec_table);
            if (seen == 2) {
                if (index == src_len) {
                    if (last_chunk == B64_LAST_STOP_BEFORE_PARTIAL) {
                        *p_read = read;
                        return written;
                    }
                    *p_err = 1;
                    return 0;
                }
                if (src[index] == '=') {
                    index++;
                    index = b64_skip_ws(src, src_len, index, dec_table);
                } else {
                    *p_err = 1;
                    return 0;
                }
            }
            /* After padding, only whitespace is allowed */
            if (index != src_len) {
                *p_err = 1;
                return 0;
            }
            if (last_chunk == B64_LAST_STRICT) {
                uint32_t mask = (seen == 2) ? 0xF : 0x3;
                if (acc & mask) {
                    *p_err = 1;
                    return 0;
                }
            }
            break;
        }

        v = dec_table[ch];
        if (v >= 64) {
            *p_err = 1;
            return 0;
        }

        /* Check remaining capacity before committing to this group */
        {
            size_t remaining = max_len - written;
            if ((remaining == 1 && seen == 2) ||
                    (remaining == 2 && seen == 3)) {
                *p_read = read;
                return written;
            }
        }

        acc = (acc << 6) | v;
        seen++;

        if (seen == 4) {
            dst[written]     = (uint8_t)(acc >> 16);
            dst[written + 1] = (uint8_t)(acc >> 8);
            dst[written + 2] = (uint8_t)(acc);
            written += 3;
            acc = 0;
            seen = 0;
            read = index;
            if (written >= max_len) {
                *p_read = read;
                return written;
            }
        }
    }

    if (seen == 2) {
        dst[written++] = (uint8_t)(acc >> 4);
    } else if (seen == 3) {
        dst[written]     = (uint8_t)(acc >> 10);
        dst[written + 1] = (uint8_t)(acc >> 2);
        written += 2;
    }
    *p_read = src_len;
    return written;
}

/* Hex helpers */
static const char u8a_hex_digits[] = "0123456789abcdef";

static size_t u8a_hex_encode(const uint8_t *src, size_t len, char *dst)
{
    for (size_t i = 0; i < len; i++) {
        dst[i * 2]     = u8a_hex_digits[src[i] >> 4];
        dst[i * 2 + 1] = u8a_hex_digits[src[i] & 0xF];
    }
    return len * 2;
}

/* Decode hex string to bytes.
   Returns bytes written. Sets *p_read to chars consumed, *p_err on error. */
static size_t u8a_hex_decode(const char *src, size_t src_len,
                             uint8_t *dst, size_t max_len,
                             size_t *p_read, int *p_err)
{
    size_t written = 0, i = 0;
    *p_err = 0;

    if (src_len & 1) {
        *p_err = 1;
        return 0;
    }

    while (i < src_len && written < max_len) {
        int hi = from_hex(src[i]);
        int lo = from_hex(src[i + 1]);
        if (hi < 0 || lo < 0) {
            *p_err = 1;
            return 0;
        }
        dst[written++] = (uint8_t)((hi << 4) | lo);
        i += 2;
    }

    *p_read = i;
    return written;
}

static JSValue JS_NewUint8ArrayCopy(JSContext *ctx, const uint8_t *buf, size_t len)
{
    JSValue buffer, obj;
    JSArrayBuffer *abuf;

    buffer = js_array_buffer_constructor3(ctx, JS_UNDEFINED, len, NULL,
                                          JS_CLASS_ARRAY_BUFFER,
                                          (uint8_t *)buf,
                                          js_array_buffer_free, NULL,
                                          TRUE);
    if (JS_IsException(buffer))
        return JS_EXCEPTION;
    obj = js_create_from_ctor(ctx, JS_UNDEFINED, JS_CLASS_UINT8_ARRAY);
    if (JS_IsException(obj)) {
        JS_FreeValue(ctx, buffer);
        return JS_EXCEPTION;
    }
    abuf = js_get_array_buffer(ctx, buffer);
    assert(abuf != NULL);
    if (typed_array_init(ctx, obj, buffer, 0, abuf->byte_length, /*track_rab*/FALSE)) {
        // 'buffer' is freed on error above.
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    return obj;
}

/* Validate that this_val is a Uint8Array (type check only, no detach check).
   Returns the JSObject pointer or NULL on error (throws). */
static JSObject *check_uint8array(JSContext *ctx, JSValueConst this_val)
{
    JSObject *p;

    if (JS_VALUE_GET_TAG(this_val) != JS_TAG_OBJECT)
        goto fail;
    p = JS_VALUE_GET_OBJ(this_val);
    if (p->class_id != JS_CLASS_UINT8_ARRAY)
        goto fail;
    return p;
fail:
    JS_ThrowTypeError(ctx, "not a Uint8Array");
    return NULL;
}

/* Get the data pointer and length of a Uint8Array, checking for detached
   buffers. Must be called after options are read (per spec ordering).
   Returns 0 on success, -1 on error (throws). */
static int get_uint8array_bytes(JSContext *ctx, JSObject *p,
                                uint8_t **pdata, size_t *plen)
{
    if (typed_array_is_oob(p)) {
        JS_ThrowTypeErrorArrayBufferOOB(ctx);
        *pdata = NULL; /* fail safe */
        *plen = 0;
        return -1;
    }
    *pdata = p->u.array.u.uint8_ptr;
    *plen = p->u.array.count;
    return 0;
}

/* Validate options is undefined or an object (GetOptionsObject).
   Returns 0 on success, -1 on error (throws). */
static int check_options_object(JSContext *ctx, JSValueConst options)
{
    if (JS_IsUndefined(options))
        return 0;
    if (!JS_IsObject(options)) {
        JS_ThrowTypeError(ctx, "options must be an object");
        return -1;
    }
    return 0;
}

/* Parse the 'alphabet' option from an options object.
   Returns B64_ALPHABET_BASE64 or B64_ALPHABET_BASE64URL, or -1 on error. */
static int parse_alphabet_option(JSContext *ctx, JSValueConst options)
{
    JSValue val;
    const char *str;
    int ret;

    if (JS_IsUndefined(options))
        return B64_ALPHABET_BASE64;

    val = JS_GetProperty(ctx, options, JS_ATOM_alphabet);
    if (JS_IsException(val))
        return -1;
    if (JS_IsUndefined(val))
        return B64_ALPHABET_BASE64;
    if (!JS_IsString(val)) {
        JS_FreeValue(ctx, val);
        JS_ThrowTypeError(ctx, "expected string for alphabet");
        return -1;
    }

    str = JS_ToCString(ctx, val);
    JS_FreeValue(ctx, val);
    if (!str)
        return -1;

    if (!strcmp(str, "base64"))
        ret = B64_ALPHABET_BASE64;
    else if (!strcmp(str, "base64url"))
        ret = B64_ALPHABET_BASE64URL;
    else {
        JS_ThrowTypeError(ctx, "invalid alphabet");
        ret = -1;
    }
    JS_FreeCString(ctx, str);
    return ret;
}

/* Parse the 'lastChunkHandling' option. Returns mode or -1 on error. */
static int parse_last_chunk_option(JSContext *ctx, JSValueConst options)
{
    JSValue val;
    const char *str;
    int ret;

    if (JS_IsUndefined(options))
        return B64_LAST_LOOSE;

    val = JS_GetProperty(ctx, options, JS_ATOM_lastChunkHandling);
    if (JS_IsException(val))
        return -1;
    if (JS_IsUndefined(val))
        return B64_LAST_LOOSE;
    if (!JS_IsString(val)) {
        JS_FreeValue(ctx, val);
        JS_ThrowTypeError(ctx, "expected string for lastChunkHandling");
        return -1;
    }

    str = JS_ToCString(ctx, val);
    JS_FreeValue(ctx, val);
    if (!str)
        return -1;

    if (!strcmp(str, "loose"))
        ret = B64_LAST_LOOSE;
    else if (!strcmp(str, "strict"))
        ret = B64_LAST_STRICT;
    else if (!strcmp(str, "stop-before-partial"))
        ret = B64_LAST_STOP_BEFORE_PARTIAL;
    else {
        JS_ThrowTypeError(ctx, "invalid lastChunkHandling option");
        ret = -1;
    }
    JS_FreeCString(ctx, str);
    return ret;
}

/* Uint8Array.prototype.toBase64([options]) */
static JSValue js_uint8array_to_base64(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    uint8_t *data;
    size_t len;
    JSValueConst options;
    JSObject *p;
    int alphabet, omit_padding;
    size_t out_len, written;
    JSString *ostr;
    char *dst;

    p = check_uint8array(ctx, this_val);
    if (!p)
        return JS_EXCEPTION;

    options = argc > 0 ? argv[0] : JS_UNDEFINED;
    if (check_options_object(ctx, options))
        return JS_EXCEPTION;
    alphabet = parse_alphabet_option(ctx, options);
    if (alphabet < 0)
        return JS_EXCEPTION;

    omit_padding = 0;
    if (!JS_IsUndefined(options)) {
        JSValue op_val = JS_GetProperty(ctx, options, JS_ATOM_omitPadding);
        if (JS_IsException(op_val))
            return JS_EXCEPTION;
        omit_padding = JS_ToBool(ctx, op_val);
        JS_FreeValue(ctx, op_val);
    }

    if (get_uint8array_bytes(ctx, p, &data, &len))
        return JS_EXCEPTION;

    out_len = 4 * ((len + 2) / 3);

    if (unlikely(out_len > JS_STRING_LEN_MAX))
        return JS_ThrowRangeError(ctx, "output too large");

    ostr = js_alloc_string(ctx, out_len, 0);
    if (!ostr)
        return JS_EXCEPTION;

    dst = (char *)ostr->u.str8;
    written = b64_encode(data, len, dst,
                         alphabet == B64_ALPHABET_BASE64URL ? b64url_enc : b64_enc);
    if (omit_padding) {
        while (written > 0 && dst[written - 1] == '=')
            written--;
    }
    dst[written] = '\0';

    ostr->len = written;
    return JS_MKPTR(JS_TAG_STRING, ostr);
}

/* Uint8Array.prototype.toHex() */
static JSValue js_uint8array_to_hex(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    uint8_t *data;
    size_t len, out_len;
    JSObject *p;
    JSString *ostr;

    p = check_uint8array(ctx, this_val);
    if (!p)
        return JS_EXCEPTION;
    if (get_uint8array_bytes(ctx, p, &data, &len))
        return JS_EXCEPTION;

    out_len = len * 2;
    if (unlikely(out_len > JS_STRING_LEN_MAX))
        return JS_ThrowRangeError(ctx, "output too large");

    ostr = js_alloc_string(ctx, out_len, 0);
    if (!ostr)
        return JS_EXCEPTION;

    u8a_hex_encode(data, len, (char *)ostr->u.str8);
    ostr->u.str8[out_len] = '\0';
    return JS_MKPTR(JS_TAG_STRING, ostr);
}

/* Uint8Array.fromBase64(string[, options]) */
static JSValue js_uint8array_from_base64(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv)
{
    const char *str;
    size_t str_len, read_pos, decoded_len, out_cap;
    int alphabet, last_chunk, err;
    uint8_t *buf;
    JSValue result;
    JSValueConst options;
    
    if (!JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "expected string");

    str = JS_ToCStringLen(ctx, &str_len, argv[0]);
    if (!str)
        return JS_EXCEPTION;

    options = argc > 1 ? argv[1] : JS_UNDEFINED;
    if (check_options_object(ctx, options)) {
        JS_FreeCString(ctx, str);
        return JS_EXCEPTION;
    }
    alphabet = parse_alphabet_option(ctx, options);
    if (alphabet < 0) {
        JS_FreeCString(ctx, str);
        return JS_EXCEPTION;
    }
    last_chunk = parse_last_chunk_option(ctx, options);
    if (last_chunk < 0) {
        JS_FreeCString(ctx, str);
        return JS_EXCEPTION;
    }

    out_cap = (str_len / 4) * 3 + 3;
    buf = js_malloc(ctx, out_cap);
    if (!buf) {
        JS_FreeCString(ctx, str);
        return JS_EXCEPTION;
    }

    decoded_len = from_base64(str, str_len, buf, out_cap,
                              alphabet == B64_ALPHABET_BASE64URL
                                  ? b64url_dec : b64_dec,
                              last_chunk, &read_pos, &err);
    JS_FreeCString(ctx, str);

    if (err) {
        js_free(ctx, buf);
        return JS_ThrowSyntaxError(ctx, "invalid base64 string");
    }

    result = JS_NewUint8ArrayCopy(ctx, buf, decoded_len);
    js_free(ctx, buf);
    return result;
}

/* Uint8Array.fromHex(string) */
static JSValue js_uint8array_from_hex(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    const char *str;
    size_t str_len, read_pos, decoded_len, out_cap;
    int err;
    uint8_t *buf;
    JSValue result;

    if (!JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "expected string");

    str = JS_ToCStringLen(ctx, &str_len, argv[0]);
    if (!str)
        return JS_EXCEPTION;

    out_cap = str_len / 2 + 1;
    buf = js_malloc(ctx, out_cap);
    if (!buf) {
        JS_FreeCString(ctx, str);
        return JS_EXCEPTION;
    }

    decoded_len = u8a_hex_decode(str, str_len, buf, out_cap, &read_pos, &err);
    JS_FreeCString(ctx, str);

    if (err) {
        js_free(ctx, buf);
        return JS_ThrowSyntaxError(ctx, "invalid hex string");
    }

    /* XXX: could avoid the copy */
    result = JS_NewUint8ArrayCopy(ctx, buf, decoded_len);
    js_free(ctx, buf);
    return result;
}

/* Return a { read, written } result object */
static JSValue js_make_read_written(JSContext *ctx, size_t read, size_t written)
{
    JSValue obj = JS_NewObject(ctx);
    if (JS_IsException(obj))
        return JS_EXCEPTION;
    if (JS_DefinePropertyValueStr(ctx, obj, "read",
                                  JS_NewUint32(ctx, read), JS_PROP_C_W_E) < 0)
        goto fail;
    if (JS_DefinePropertyValueStr(ctx, obj, "written",
                                  JS_NewUint32(ctx, written), JS_PROP_C_W_E) < 0)
        goto fail;
    return obj;
fail:
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
}

/* Uint8Array.prototype.setFromBase64(string[, options]) */
static JSValue js_uint8array_set_from_base64(JSContext *ctx,
                                             JSValueConst this_val,
                                             int argc, JSValueConst *argv)
{
    uint8_t *data;
    size_t len;
    const char *str;
    size_t str_len, read_pos, decoded_len;
    JSObject *p;
    int alphabet, last_chunk, err;
    JSValueConst options;

    p = check_uint8array(ctx, this_val);
    if (!p)
        return JS_EXCEPTION;

    if (!JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "expected string");

    str = JS_ToCStringLen(ctx, &str_len, argv[0]);
    if (!str)
        return JS_EXCEPTION;

    options = argc > 1 ? argv[1] : JS_UNDEFINED;
    if (check_options_object(ctx, options)) {
        JS_FreeCString(ctx, str);
        return JS_EXCEPTION;
    }
    alphabet = parse_alphabet_option(ctx, options);
    if (alphabet < 0) {
        JS_FreeCString(ctx, str);
        return JS_EXCEPTION;
    }
    last_chunk = parse_last_chunk_option(ctx, options);
    if (last_chunk < 0) {
        JS_FreeCString(ctx, str);
        return JS_EXCEPTION;
    }

    if (get_uint8array_bytes(ctx, p, &data, &len)) {
        JS_FreeCString(ctx, str);
        return JS_EXCEPTION;
    }

    decoded_len = from_base64(str, str_len, data, len,
                              alphabet == B64_ALPHABET_BASE64URL
                                  ? b64url_dec : b64_dec,
                              last_chunk, &read_pos, &err);
    JS_FreeCString(ctx, str);

    if (err)
        return JS_ThrowSyntaxError(ctx, "invalid base64 string");

    return js_make_read_written(ctx, read_pos, decoded_len);
}

/* Uint8Array.prototype.setFromHex(string) */
static JSValue js_uint8array_set_from_hex(JSContext *ctx,
                                          JSValueConst this_val,
                                          int argc, JSValueConst *argv)
{
    uint8_t *data;
    size_t len;
    const char *str;
    size_t str_len, read_pos, decoded_len;
    JSObject *p;
    int err;

    p = check_uint8array(ctx, this_val);
    if (!p)
        return JS_EXCEPTION;

    if (!JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "expected string");

    str = JS_ToCStringLen(ctx, &str_len, argv[0]);
    if (!str)
        return JS_EXCEPTION;

    if (get_uint8array_bytes(ctx, p, &data, &len)) {
        JS_FreeCString(ctx, str);
        return JS_EXCEPTION;
    }

    decoded_len = u8a_hex_decode(str, str_len, data, len, &read_pos, &err);
    JS_FreeCString(ctx, str);

    if (err)
        return JS_ThrowSyntaxError(ctx, "invalid hex string");

    return js_make_read_written(ctx, read_pos, decoded_len);
}

static const JSCFunctionListEntry js_typed_array_base_funcs[] = {
    JS_CFUNC_DEF("from", 1, js_typed_array_from ),
    JS_CFUNC_DEF("of", 0, js_typed_array_of ),
    JS_CGETSET_DEF("[Symbol.species]", js_get_this, NULL ),
};

#include "dyna-simd-kernels.h" /* multi-ISA SIMD reductions (core-linked) */

/* read TypedArray element i as a double (non-SIMD numeric element types) */
static double js_ta_read_double(JSObject *p, size_t i)
{
    switch (p->class_id) {
    case JS_CLASS_INT8_ARRAY:    return p->u.array.u.int8_ptr[i];
    case JS_CLASS_UINT8_ARRAY:
    case JS_CLASS_UINT8C_ARRAY:  return p->u.array.u.uint8_ptr[i];
    case JS_CLASS_INT16_ARRAY:   return p->u.array.u.int16_ptr[i];
    case JS_CLASS_UINT16_ARRAY:  return p->u.array.u.uint16_ptr[i];
    case JS_CLASS_UINT32_ARRAY:  return p->u.array.u.uint32_ptr[i];
    case JS_CLASS_INT32_ARRAY:   return p->u.array.u.int32_ptr[i];
    case JS_CLASS_FLOAT16_ARRAY: return fromfp16(p->u.array.u.fp16_ptr[i]);
    case JS_CLASS_FLOAT32_ARRAY: return p->u.array.u.float_ptr[i];
    case JS_CLASS_FLOAT64_ARRAY: return p->u.array.u.double_ptr[i];
    default:                     return 0;
    }
}

/* SIMD-accelerated numeric reductions on TypedArrays.
 * magic: 0=_sum 1=_min 2=_max 3=_mean. Float64/Float32/Int32 arrays run the
 * multi-ISA SIMD kernels over their contiguous buffers; other numeric element
 * types use a tight scalar loop; BigInt typed arrays throw. */
static JSValue js_typed_array_ext_reduce(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv, int magic)
{
    JSObject *p;
    size_t n, i;
    double d;
    (void)argc; (void)argv;

    p = get_typed_array(ctx, this_val);
    if (!p)
        return JS_EXCEPTION;
    if (typed_array_is_oob(p)) {
        JS_ThrowTypeErrorArrayBufferOOB(ctx);
        return JS_EXCEPTION;
    }
    simd_init(); /* idempotent (pthread_once): select the best kernels */
    n = p->u.array.count;
    if (n == 0) {
        if (magic == 0) return JS_NewInt32(ctx, 0);     /* sum of empty = 0 */
        if (magic == 3) return JS_NewFloat64(ctx, NAN); /* mean of empty = NaN */
        return JS_UNDEFINED;                            /* min/max of empty */
    }
    switch (p->class_id) {
    case JS_CLASS_FLOAT64_ARRAY: {
        const double *x = p->u.array.u.double_ptr;
        if (magic == 1) d = simd.f64_min(x, n);
        else if (magic == 2) d = simd.f64_max(x, n);
        else { d = simd.f64_sum(x, n); if (magic == 3) d /= (double)n; }
        return JS_NewFloat64(ctx, d);
    }
    case JS_CLASS_FLOAT32_ARRAY: {
        const float *x = p->u.array.u.float_ptr;
        if (magic == 1) d = simd.min(x, n);
        else if (magic == 2) d = simd.max(x, n);
        else { d = simd.sum(x, n); if (magic == 3) d /= (double)n; }
        return JS_NewFloat64(ctx, d);
    }
    case JS_CLASS_INT32_ARRAY: {
        const int32_t *x = p->u.array.u.int32_ptr;
        if (magic == 1) return JS_NewInt32(ctx, simd.i32_min(x, n));
        if (magic == 2) return JS_NewInt32(ctx, simd.i32_max(x, n));
        {
            int64_t s = simd.i32_sum(x, n);
            if (magic == 3) return JS_NewFloat64(ctx, (double)s / (double)n);
            return JS_NewInt64(ctx, s);
        }
    }
    case JS_CLASS_BIG_INT64_ARRAY:
    case JS_CLASS_BIG_UINT64_ARRAY:
        return JS_ThrowTypeError(ctx, "sum/min/max/mean: BigInt typed arrays are not supported");
    default: /* int8/uint8/uint8c/int16/uint16/uint32/float16: scalar loop */
        if (magic == 0 || magic == 3) {
            double s = 0;
            for (i = 0; i < n; i++) s += js_ta_read_double(p, i);
            if (magic == 3) s /= (double)n;
            return JS_NewFloat64(ctx, s);
        } else {
            double best = js_ta_read_double(p, 0);
            for (i = 1; i < n; i++) {
                double v = js_ta_read_double(p, i);
                if (magic == 1 ? v < best : v > best) best = v;
            }
            return JS_NewFloat64(ctx, best);
        }
    }
}

/* _dot(other) -> SIMD dot product of two same-type, equal-length TypedArrays
 * (Float64/Float32/Int32 via simd.f64_dot/simd.dot/simd.i32_dot; other numeric
 * types via a scalar loop; BigInt throws). */
static JSValue js_typed_array_ext_dot(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    JSObject *p, *q;
    size_t n, i;
    double acc;

    p = get_typed_array(ctx, this_val);
    if (!p)
        return JS_EXCEPTION;
    q = get_typed_array(ctx, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (!q)
        return JS_EXCEPTION;
    if (typed_array_is_oob(p) || typed_array_is_oob(q)) {
        JS_ThrowTypeErrorArrayBufferOOB(ctx);
        return JS_EXCEPTION;
    }
    if (p->class_id != q->class_id)
        return JS_ThrowTypeError(ctx, "dot: both typed arrays must be the same type");
    if (p->u.array.count != q->u.array.count)
        return JS_ThrowRangeError(ctx, "dot: both typed arrays must have equal length");
    simd_init();
    n = p->u.array.count;
    if (n == 0)
        return JS_NewInt32(ctx, 0);
    switch (p->class_id) {
    case JS_CLASS_FLOAT64_ARRAY:
        return JS_NewFloat64(ctx, simd.f64_dot(p->u.array.u.double_ptr, q->u.array.u.double_ptr, n));
    case JS_CLASS_FLOAT32_ARRAY:
        return JS_NewFloat64(ctx, simd.dot(p->u.array.u.float_ptr, q->u.array.u.float_ptr, n));
    case JS_CLASS_INT32_ARRAY:
        return JS_NewFloat64(ctx, simd.i32_dot(p->u.array.u.int32_ptr, q->u.array.u.int32_ptr, n));
    case JS_CLASS_BIG_INT64_ARRAY:
    case JS_CLASS_BIG_UINT64_ARRAY:
        return JS_ThrowTypeError(ctx, "dot: BigInt typed arrays are not supported");
    default:
        acc = 0;
        for (i = 0; i < n; i++)
            acc += js_ta_read_double(p, i) * js_ta_read_double(q, i);
        return JS_NewFloat64(ctx, acc);
    }
}

static const JSCFunctionListEntry js_typed_array_base_proto_funcs[] = {
    JS_CFUNC_DEF("dot", 1, js_typed_array_ext_dot ),
    JS_CFUNC_MAGIC_DEF("sum", 0, js_typed_array_ext_reduce, 0 ),
    JS_CFUNC_MAGIC_DEF("min", 0, js_typed_array_ext_reduce, 1 ),
    JS_CFUNC_MAGIC_DEF("max", 0, js_typed_array_ext_reduce, 2 ),
    JS_CFUNC_MAGIC_DEF("mean", 0, js_typed_array_ext_reduce, 3 ),
    JS_ALIAS_DEF("average", "mean" ),
    JS_CGETSET_DEF("length", js_typed_array_get_length, NULL ),
    JS_CFUNC_DEF("at", 1, js_typed_array_at ),
    JS_CFUNC_DEF("with", 2, js_typed_array_with ),
    JS_CGETSET_DEF("buffer", js_typed_array_get_buffer, NULL ),
    JS_CGETSET_DEF("byteLength", js_typed_array_get_byteLength, NULL ),
    JS_CGETSET_DEF("byteOffset", js_typed_array_get_byteOffset, NULL ),
    JS_CFUNC_DEF("set", 1, js_typed_array_set ),
    JS_CFUNC_MAGIC_DEF("values", 0, js_create_typed_array_iterator, JS_ITERATOR_KIND_VALUE ),
    JS_ALIAS_DEF("[Symbol.iterator]", "values" ),
    JS_CFUNC_MAGIC_DEF("keys", 0, js_create_typed_array_iterator, JS_ITERATOR_KIND_KEY ),
    JS_CFUNC_MAGIC_DEF("entries", 0, js_create_typed_array_iterator, JS_ITERATOR_KIND_KEY_AND_VALUE ),
    JS_CGETSET_DEF("[Symbol.toStringTag]", js_typed_array_get_toStringTag, NULL ),
    JS_CFUNC_DEF("copyWithin", 2, js_typed_array_copyWithin ),
    JS_CFUNC_MAGIC_DEF("every", 1, js_array_every, special_every | special_TA ),
    JS_CFUNC_MAGIC_DEF("some", 1, js_array_every, special_some | special_TA ),
    JS_CFUNC_MAGIC_DEF("forEach", 1, js_array_every, special_forEach | special_TA ),
    JS_CFUNC_MAGIC_DEF("map", 1, js_array_every, special_map | special_TA ),
    JS_CFUNC_MAGIC_DEF("filter", 1, js_array_every, special_filter | special_TA ),
    JS_CFUNC_MAGIC_DEF("reduce", 1, js_array_reduce, special_reduce | special_TA ),
    JS_CFUNC_MAGIC_DEF("reduceRight", 1, js_array_reduce, special_reduceRight | special_TA ),
    JS_CFUNC_DEF("fill", 1, js_typed_array_fill ),
    JS_CFUNC_MAGIC_DEF("find", 1, js_typed_array_find, ArrayFind ),
    JS_CFUNC_MAGIC_DEF("findIndex", 1, js_typed_array_find, ArrayFindIndex ),
    JS_CFUNC_MAGIC_DEF("findLast", 1, js_typed_array_find, ArrayFindLast ),
    JS_CFUNC_MAGIC_DEF("findLastIndex", 1, js_typed_array_find, ArrayFindLastIndex ),
    JS_CFUNC_DEF("reverse", 0, js_typed_array_reverse ),
    JS_CFUNC_DEF("toReversed", 0, js_typed_array_toReversed ),
    JS_CFUNC_DEF("slice", 2, js_typed_array_slice ),
    JS_CFUNC_DEF("subarray", 2, js_typed_array_subarray ),
    JS_CFUNC_DEF("sort", 1, js_typed_array_sort ),
    JS_CFUNC_DEF("toSorted", 1, js_typed_array_toSorted ),
    JS_CFUNC_MAGIC_DEF("join", 1, js_typed_array_join, 0 ),
    JS_CFUNC_MAGIC_DEF("toLocaleString", 0, js_typed_array_join, 1 ),
    JS_CFUNC_MAGIC_DEF("indexOf", 1, js_typed_array_indexOf, special_indexOf ),
    JS_CFUNC_MAGIC_DEF("lastIndexOf", 1, js_typed_array_indexOf, special_lastIndexOf ),
    JS_CFUNC_MAGIC_DEF("includes", 1, js_typed_array_indexOf, special_includes ),
    //JS_ALIAS_BASE_DEF("toString", "toString", 2 /* Array.prototype. */), @@@
};

static const JSCFunctionListEntry js_typed_array_funcs[] = {
    JS_PROP_INT32_DEF("BYTES_PER_ELEMENT", 1, 0),
    JS_PROP_INT32_DEF("BYTES_PER_ELEMENT", 2, 0),
    JS_PROP_INT32_DEF("BYTES_PER_ELEMENT", 4, 0),
    JS_PROP_INT32_DEF("BYTES_PER_ELEMENT", 8, 0),
};

static const JSCFunctionListEntry js_uint8array_proto_funcs[] = {
    JS_PROP_INT32_DEF("BYTES_PER_ELEMENT", 1, 0),
    JS_CFUNC_DEF("toBase64", 0, js_uint8array_to_base64),
    JS_CFUNC_DEF("toHex", 0, js_uint8array_to_hex),
    JS_CFUNC_DEF("setFromBase64", 1, js_uint8array_set_from_base64),
    JS_CFUNC_DEF("setFromHex", 1, js_uint8array_set_from_hex),
};

static const JSCFunctionListEntry js_uint8array_funcs[] = {
    JS_PROP_INT32_DEF("BYTES_PER_ELEMENT", 1, 0),
    JS_CFUNC_DEF("fromBase64", 1, js_uint8array_from_base64),
    JS_CFUNC_DEF("fromHex", 1, js_uint8array_from_hex),
};

static JSValue js_typed_array_base_constructor(JSContext *ctx,
                                               JSValueConst this_val,
                                               int argc, JSValueConst *argv)
{
    return JS_ThrowTypeError(ctx, "cannot be called");
}

/* 'obj' must be an allocated typed array object */
static int typed_array_init(JSContext *ctx, JSValueConst obj,
                            JSValue buffer, uint64_t offset, uint64_t len,
                            BOOL track_rab)
{
    JSTypedArray *ta;
    JSObject *p, *pbuffer;
    JSArrayBuffer *abuf;
    int size_log2;

    p = JS_VALUE_GET_OBJ(obj);
    size_log2 = typed_array_size_log2(p->class_id);
    ta = js_malloc(ctx, sizeof(*ta));
    if (!ta) {
        JS_FreeValue(ctx, buffer);
        return -1;
    }
    pbuffer = JS_VALUE_GET_OBJ(buffer);
    abuf = pbuffer->u.array_buffer;
    ta->obj = p;
    ta->buffer = pbuffer;
    ta->offset = offset;
    ta->length = len << size_log2;
    ta->track_rab = track_rab;
    list_add_tail(&ta->link, &abuf->array_list);
    p->u.typed_array = ta;
    p->u.array.count = len;
    p->u.array.u.ptr = abuf->data + offset;
    return 0;
}


static JSValue js_array_from_iterator(JSContext *ctx, uint32_t *plen,
                                      JSValueConst obj, JSValueConst method)
{
    JSValue arr, iter, next_method = JS_UNDEFINED, val;
    BOOL done;
    uint32_t k;

    *plen = 0;
    arr = JS_NewArray(ctx);
    if (JS_IsException(arr))
        return arr;
    iter = JS_GetIterator2(ctx, obj, method);
    if (JS_IsException(iter))
        goto fail;
    next_method = JS_GetProperty(ctx, iter, JS_ATOM_next);
    if (JS_IsException(next_method))
        goto fail;
    k = 0;
    for(;;) {
        val = JS_IteratorNext(ctx, iter, next_method, 0, NULL, &done);
        if (JS_IsException(val))
            goto fail;
        if (done)
            break;
        if (JS_CreateDataPropertyUint32(ctx, arr, k, val, JS_PROP_THROW) < 0)
            goto fail;
        k++;
    }
    JS_FreeValue(ctx, next_method);
    JS_FreeValue(ctx, iter);
    *plen = k;
    return arr;
 fail:
    JS_FreeValue(ctx, next_method);
    JS_FreeValue(ctx, iter);
    JS_FreeValue(ctx, arr);
    return JS_EXCEPTION;
}

static JSValue js_typed_array_constructor_obj(JSContext *ctx,
                                              JSValueConst new_target,
                                              JSValueConst obj,
                                              int classid)
{
    JSValue iter, ret, arr = JS_UNDEFINED, val, buffer;
    uint32_t i;
    int size_log2;
    int64_t len;

    size_log2 = typed_array_size_log2(classid);
    ret = js_create_from_ctor(ctx, new_target, classid);
    if (JS_IsException(ret))
        return JS_EXCEPTION;

    iter = JS_GetProperty(ctx, obj, JS_ATOM_Symbol_iterator);
    if (JS_IsException(iter))
        goto fail;
    if (!JS_IsUndefined(iter) && !JS_IsNull(iter)) {
        uint32_t len1;
        arr = js_array_from_iterator(ctx, &len1, obj, iter);
        JS_FreeValue(ctx, iter);
        if (JS_IsException(arr))
            goto fail;
        len = len1;
    } else {
        if (js_get_length64(ctx, &len, obj))
            goto fail;
        arr = JS_DupValue(ctx, obj);
    }

    buffer = js_array_buffer_constructor1(ctx, JS_UNDEFINED,
                                          len << size_log2,
                                          NULL);
    if (JS_IsException(buffer))
        goto fail;
    if (typed_array_init(ctx, ret, buffer, 0, len, /*track_rab*/FALSE))
        goto fail;

    for(i = 0; i < len; i++) {
        val = JS_GetPropertyUint32(ctx, arr, i);
        if (JS_IsException(val))
            goto fail;
        if (JS_SetPropertyUint32(ctx, ret, i, val) < 0)
            goto fail;
    }
    JS_FreeValue(ctx, arr);
    return ret;
 fail:
    JS_FreeValue(ctx, arr);
    JS_FreeValue(ctx, ret);
    return JS_EXCEPTION;
}

static JSValue js_typed_array_constructor_ta(JSContext *ctx,
                                             JSValueConst new_target,
                                             JSValueConst src_obj,
                                             int classid, uint32_t len)
{
    JSObject *p, *src_buffer;
    JSTypedArray *ta;
    JSValue obj, buffer;
    uint32_t i;
    int size_log2;
    JSArrayBuffer *src_abuf, *abuf;

    obj = js_create_from_ctor(ctx, new_target, classid);
    if (JS_IsException(obj))
        return obj;
    p = JS_VALUE_GET_OBJ(src_obj);
    if (typed_array_is_oob(p)) {
        JS_ThrowTypeErrorArrayBufferOOB(ctx);
        goto fail;
    }
    size_log2 = typed_array_size_log2(classid);
    buffer = js_array_buffer_constructor1(ctx, JS_UNDEFINED,
                                          (uint64_t)len << size_log2,
                                          NULL);
    if (JS_IsException(buffer))
        goto fail;
    /* necessary because it could have been detached */
    if (typed_array_is_oob(p)) {
        JS_FreeValue(ctx, buffer);
        JS_ThrowTypeErrorArrayBufferOOB(ctx);
        goto fail;
    }
    abuf = JS_GetOpaque(buffer, JS_CLASS_ARRAY_BUFFER);
    if (typed_array_init(ctx, obj, buffer, 0, len, /*track_rab*/FALSE))
        goto fail;
    ta = p->u.typed_array;
    src_buffer = ta->buffer;
    src_abuf = src_buffer->u.array_buffer;
    if (p->class_id == classid &&
        (int64_t)ta->offset + (int64_t)abuf->byte_length <= src_abuf->byte_length) {
        /* same type and no overflow: copy the content */
        memcpy(abuf->data, src_abuf->data + ta->offset, abuf->byte_length);
    } else {
        for(i = 0; i < len; i++) {
            JSValue val;
            val = JS_GetPropertyUint32(ctx, src_obj, i);
            if (JS_IsException(val))
                goto fail;
            if (JS_SetPropertyUint32(ctx, obj, i, val) < 0)
                goto fail;
        }
    }
    return obj;
 fail:
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
}

static JSValue js_typed_array_constructor(JSContext *ctx,
                                          JSValueConst new_target,
                                          int argc, JSValueConst *argv,
                                          int classid)
{
    BOOL track_rab = FALSE;
    JSValue buffer, obj;
    JSArrayBuffer *abuf;
    int size_log2;
    uint64_t len, offset;

    size_log2 = typed_array_size_log2(classid);
    if (JS_VALUE_GET_TAG(argv[0]) != JS_TAG_OBJECT) {
        if (JS_ToIndex(ctx, &len, argv[0]))
            return JS_EXCEPTION;
        obj = js_create_from_ctor(ctx, new_target, classid);
        if (JS_IsException(obj))
            return JS_EXCEPTION;
        buffer = js_array_buffer_constructor1(ctx, JS_UNDEFINED,
                                              len << size_log2,
                                              NULL);
        if (JS_IsException(buffer))
            goto fail;
        offset = 0;
    } else {
        JSObject *p = JS_VALUE_GET_OBJ(argv[0]);
        if (p->class_id == JS_CLASS_ARRAY_BUFFER ||
            p->class_id == JS_CLASS_SHARED_ARRAY_BUFFER) {
            obj = js_create_from_ctor(ctx, new_target, classid);
            if (JS_IsException(obj))
                return JS_EXCEPTION;
            if (JS_ToIndex(ctx, &offset, argv[1]))
                goto fail;
            if ((offset & ((1 << size_log2) - 1)) != 0)
                goto invalid_offset;
            abuf = p->u.array_buffer;
            if (JS_IsUndefined(argv[2])) {
                if (abuf->detached) {
                    JS_ThrowTypeErrorDetachedArrayBuffer(ctx);
                    goto fail;
                }
                if (offset > abuf->byte_length) {
                invalid_offset:
                    JS_ThrowRangeError(ctx, "invalid offset");
                    goto fail;
                }
                track_rab = array_buffer_is_resizable(abuf);
                if (!track_rab) {
                    if ((abuf->byte_length & ((1 << size_log2) - 1)) != 0)
                        goto invalid_length;
                }
                len = (abuf->byte_length - offset) >> size_log2;
            } else {
                if (JS_ToIndex(ctx, &len, argv[2]))
                    goto fail;
                if (abuf->detached) {
                    JS_ThrowTypeErrorDetachedArrayBuffer(ctx);
                    goto fail;
                }
                if ((offset + (len << size_log2)) > abuf->byte_length) {
                invalid_length:
                    JS_ThrowRangeError(ctx, "invalid length");
                    goto fail;
                }
            }
            buffer = JS_DupValue(ctx, argv[0]);
        } else {
            if (p->class_id >= JS_CLASS_UINT8C_ARRAY &&
                p->class_id <= JS_CLASS_FLOAT64_ARRAY) {
                return js_typed_array_constructor_ta(ctx, new_target, argv[0],
                                                     classid, p->u.array.count);
            } else {
                return js_typed_array_constructor_obj(ctx, new_target, argv[0], classid);
            }
        }
    }
    if (typed_array_init(ctx, obj, buffer, offset, len, track_rab))
        goto fail;
    return obj;
 fail:
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
}

static void js_typed_array_finalizer(JSRuntime *rt, JSValue val)
{
    JSObject *p = JS_VALUE_GET_OBJ(val);
    JSTypedArray *ta = p->u.typed_array;
    if (ta) {
        /* during the GC the finalizers are called in an arbitrary
           order so the ArrayBuffer finalizer may have been called */
        if (ta->link.next) {
            list_del(&ta->link);
        }
        JS_FreeValueRT(rt, JS_MKPTR(JS_TAG_OBJECT, ta->buffer));
        js_free_rt(rt, ta);
    }
}

static void js_typed_array_mark(JSRuntime *rt, JSValueConst val,
                                JS_MarkFunc *mark_func)
{
    JSObject *p = JS_VALUE_GET_OBJ(val);
    JSTypedArray *ta = p->u.typed_array;
    if (ta) {
        JS_MarkValue(rt, JS_MKPTR(JS_TAG_OBJECT, ta->buffer), mark_func);
    }
}

static JSValue js_dataview_constructor(JSContext *ctx,
                                       JSValueConst new_target,
                                       int argc, JSValueConst *argv)
{
    BOOL recompute_len = FALSE;
    BOOL track_rab = FALSE;
    JSArrayBuffer *abuf;
    uint64_t offset;
    uint32_t len;
    JSValueConst buffer;
    JSValue obj;
    JSTypedArray *ta;
    JSObject *p;

    buffer = argv[0];
    abuf = js_get_array_buffer(ctx, buffer);
    if (!abuf)
        return JS_EXCEPTION;
    offset = 0;
    if (argc > 1) {
        if (JS_ToIndex(ctx, &offset, argv[1]))
            return JS_EXCEPTION;
    }
    if (abuf->detached)
        return JS_ThrowTypeErrorDetachedArrayBuffer(ctx);
    if (offset > abuf->byte_length)
        return JS_ThrowRangeError(ctx, "invalid byteOffset");
    len = abuf->byte_length - offset;
    if (argc > 2 && !JS_IsUndefined(argv[2])) {
        uint64_t l;
        if (JS_ToIndex(ctx, &l, argv[2]))
            return JS_EXCEPTION;
        if (l > len)
            return JS_ThrowRangeError(ctx, "invalid byteLength");
        len = l;
    } else {
        recompute_len = TRUE;
        track_rab = array_buffer_is_resizable(abuf);
    }

    obj = js_create_from_ctor(ctx, new_target, JS_CLASS_DATAVIEW);
    if (JS_IsException(obj))
        return JS_EXCEPTION;
    if (abuf->detached) {
        /* could have been detached in js_create_from_ctor() */
        JS_ThrowTypeErrorDetachedArrayBuffer(ctx);
        goto fail;
    }
    // RAB could have been resized in js_create_from_ctor()
    if (offset > abuf->byte_length) {
        goto out_of_bound;
    } else if (recompute_len) {
        len = abuf->byte_length - offset;
    } else if (offset + len > abuf->byte_length) {
    out_of_bound:
        JS_ThrowRangeError(ctx, "invalid byteOffset or byteLength");
        goto fail;
    }
    ta = js_malloc(ctx, sizeof(*ta));
    if (!ta) {
    fail:
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    p = JS_VALUE_GET_OBJ(obj);
    ta->obj = p;
    ta->buffer = JS_VALUE_GET_OBJ(JS_DupValue(ctx, buffer));
    ta->offset = offset;
    ta->length = len;
    ta->track_rab = track_rab;
    list_add_tail(&ta->link, &abuf->array_list);
    p->u.typed_array = ta;
    return obj;
}

// is the DataView out of bounds relative to its parent arraybuffer?
static BOOL dataview_is_oob(JSObject *p)
{
    JSArrayBuffer *abuf;
    JSTypedArray *ta;

    assert(p->class_id == JS_CLASS_DATAVIEW);
    ta = p->u.typed_array;
    abuf = ta->buffer->u.array_buffer;
    if (abuf->detached)
        return TRUE;
    if (ta->offset > abuf->byte_length)
        return TRUE;
    if (ta->track_rab)
        return FALSE;
    return (int64_t)ta->offset + ta->length > abuf->byte_length;
}

static JSObject *get_dataview(JSContext *ctx, JSValueConst this_val)
{
    JSObject *p;
    if (JS_VALUE_GET_TAG(this_val) != JS_TAG_OBJECT)
        goto fail;
    p = JS_VALUE_GET_OBJ(this_val);
    if (p->class_id != JS_CLASS_DATAVIEW) {
    fail:
        JS_ThrowTypeError(ctx, "not a DataView");
        return NULL;
    }
    return p;
}

static JSValue js_dataview_get_buffer(JSContext *ctx, JSValueConst this_val)
{
    JSObject *p;
    JSTypedArray *ta;
    p = get_dataview(ctx, this_val);
    if (!p)
        return JS_EXCEPTION;
    ta = p->u.typed_array;
    return JS_DupValue(ctx, JS_MKPTR(JS_TAG_OBJECT, ta->buffer));
}

static JSValue js_dataview_get_byteLength(JSContext *ctx, JSValueConst this_val)
{
    JSArrayBuffer *abuf;
    JSTypedArray *ta;
    JSObject *p;

    p = get_dataview(ctx, this_val);
    if (!p)
        return JS_EXCEPTION;
    if (dataview_is_oob(p))
        return JS_ThrowTypeErrorArrayBufferOOB(ctx);
    ta = p->u.typed_array;
    if (ta->track_rab) {
        abuf = ta->buffer->u.array_buffer;
        return JS_NewUint32(ctx, abuf->byte_length - ta->offset);
    }
    return JS_NewUint32(ctx, ta->length);
}

static JSValue js_dataview_get_byteOffset(JSContext *ctx, JSValueConst this_val)
{
    JSTypedArray *ta;
    JSObject *p;

    p = get_dataview(ctx, this_val);
    if (!p)
        return JS_EXCEPTION;
    if (dataview_is_oob(p))
        return JS_ThrowTypeErrorArrayBufferOOB(ctx);
    ta = p->u.typed_array;
    return JS_NewUint32(ctx, ta->offset);
}

/* Resolve ANY ArrayBuffer view -- every TypedArray plus DataView -- to its
 * backing buffer, byte offset and byte length. Same contract as
 * JS_GetTypedArrayBuffer (returns an owned reference to the buffer; throws on a
 * detached or out-of-bounds view) but does not reject a DataView, whose
 * bytes_per_element is reported as 1 since it is byte-addressed by definition.
 * Resolution is purely structural -- it never runs JS -- so a caller may use it
 * after coercing its other arguments without opening a reentrancy window. */
JSValue JS_GetArrayBufferView(JSContext *ctx, JSValueConst obj,
                              size_t *pbyte_offset,
                              size_t *pbyte_length,
                              size_t *pbytes_per_element)
{
    JSObject *p;
    JSTypedArray *ta;
    JSArrayBuffer *abuf;

    if (JS_VALUE_GET_TAG(obj) == JS_TAG_OBJECT) {
        p = JS_VALUE_GET_OBJ(obj);
        if (p->class_id == JS_CLASS_DATAVIEW) {
            if (dataview_is_oob(p))
                return JS_ThrowTypeErrorArrayBufferOOB(ctx);
            ta = p->u.typed_array;
            if (pbyte_offset)
                *pbyte_offset = ta->offset;
            if (pbyte_length) {
                if (ta->track_rab) {
                    abuf = ta->buffer->u.array_buffer;
                    *pbyte_length = abuf->byte_length - ta->offset;
                } else {
                    *pbyte_length = ta->length;
                }
            }
            if (pbytes_per_element)
                *pbytes_per_element = 1;
            return JS_DupValue(ctx, JS_MKPTR(JS_TAG_OBJECT, ta->buffer));
        }
    }
    return JS_GetTypedArrayBuffer(ctx, obj, pbyte_offset, pbyte_length,
                                  pbytes_per_element);
}

static JSValue js_dataview_getValue(JSContext *ctx,
                                    JSValueConst this_obj,
                                    int argc, JSValueConst *argv, int class_id)
{
    JSTypedArray *ta;
    JSArrayBuffer *abuf;
    BOOL littleEndian, is_swap;
    int size;
    uint8_t *ptr;
    uint32_t v;
    uint64_t pos;

    ta = JS_GetOpaque2(ctx, this_obj, JS_CLASS_DATAVIEW);
    if (!ta)
        return JS_EXCEPTION;
    size = 1 << typed_array_size_log2(class_id);
    if (JS_ToIndex(ctx, &pos, argv[0]))
        return JS_EXCEPTION;
    littleEndian = argc > 1 && JS_ToBool(ctx, argv[1]);
    is_swap = littleEndian ^ !is_be();
    abuf = ta->buffer->u.array_buffer;
    if (abuf->detached)
        return JS_ThrowTypeErrorDetachedArrayBuffer(ctx);
    // order matters: this check should come before the next one
    if ((pos + size) > ta->length)
        return JS_ThrowRangeError(ctx, "out of bound");
    // test262 expects a TypeError for this and V8, in its infinite wisdom,
    // throws a "detached array buffer" exception, but IMO that doesn't make
    // sense because the buffer is not in fact detached, it's still there
    if ((int64_t)ta->offset + ta->length > abuf->byte_length)
        return JS_ThrowTypeError(ctx, "out of bound");
    ptr = abuf->data + ta->offset + pos;

    switch(class_id) {
    case JS_CLASS_INT8_ARRAY:
        return JS_NewInt32(ctx, *(int8_t *)ptr);
    case JS_CLASS_UINT8_ARRAY:
        return JS_NewInt32(ctx, *(uint8_t *)ptr);
    case JS_CLASS_INT16_ARRAY:
        v = get_u16(ptr);
        if (is_swap)
            v = bswap16(v);
        return JS_NewInt32(ctx, (int16_t)v);
    case JS_CLASS_UINT16_ARRAY:
        v = get_u16(ptr);
        if (is_swap)
            v = bswap16(v);
        return JS_NewInt32(ctx, v);
    case JS_CLASS_INT32_ARRAY:
        v = get_u32(ptr);
        if (is_swap)
            v = bswap32(v);
        return JS_NewInt32(ctx, v);
    case JS_CLASS_UINT32_ARRAY:
        v = get_u32(ptr);
        if (is_swap)
            v = bswap32(v);
        return JS_NewUint32(ctx, v);
    case JS_CLASS_BIG_INT64_ARRAY:
        {
            uint64_t v;
            v = get_u64(ptr);
            if (is_swap)
                v = bswap64(v);
            return JS_NewBigInt64(ctx, v);
        }
        break;
    case JS_CLASS_BIG_UINT64_ARRAY:
        {
            uint64_t v;
            v = get_u64(ptr);
            if (is_swap)
                v = bswap64(v);
            return JS_NewBigUint64(ctx, v);
        }
        break;
    case JS_CLASS_FLOAT16_ARRAY:
        {
            uint16_t v;
            v = get_u16(ptr);
            if (is_swap)
                v = bswap16(v);
            return __JS_NewFloat64(ctx, fromfp16(v));
        }
    case JS_CLASS_FLOAT32_ARRAY:
        {
            union {
                float f;
                uint32_t i;
            } u;
            v = get_u32(ptr);
            if (is_swap)
                v = bswap32(v);
            u.i = v;
            return __JS_NewFloat64(ctx, u.f);
        }
    case JS_CLASS_FLOAT64_ARRAY:
        {
            union {
                double f;
                uint64_t i;
            } u;
            u.i = get_u64(ptr);
            if (is_swap)
                u.i = bswap64(u.i);
            return __JS_NewFloat64(ctx, u.f);
        }
    default:
        abort();
    }
}

static JSValue js_dataview_setValue(JSContext *ctx,
                                    JSValueConst this_obj,
                                    int argc, JSValueConst *argv, int class_id)
{
    JSTypedArray *ta;
    JSArrayBuffer *abuf;
    BOOL littleEndian, is_swap;
    int size;
    uint8_t *ptr;
    uint64_t v64;
    uint32_t v;
    uint64_t pos;
    JSValueConst val;

    ta = JS_GetOpaque2(ctx, this_obj, JS_CLASS_DATAVIEW);
    if (!ta)
        return JS_EXCEPTION;
    size = 1 << typed_array_size_log2(class_id);
    if (JS_ToIndex(ctx, &pos, argv[0]))
        return JS_EXCEPTION;
    val = argv[1];
    v = 0; /* avoid warning */
    v64 = 0; /* avoid warning */
    if (class_id <= JS_CLASS_UINT32_ARRAY) {
        if (JS_ToUint32(ctx, &v, val))
            return JS_EXCEPTION;
    } else if (class_id <= JS_CLASS_BIG_UINT64_ARRAY) {
        if (JS_ToBigInt64(ctx, (int64_t *)&v64, val))
            return JS_EXCEPTION;
    } else {
        double d;
        if (JS_ToFloat64(ctx, &d, val))
            return JS_EXCEPTION;
        if (class_id == JS_CLASS_FLOAT16_ARRAY) {
            v = tofp16(d);
        } else if (class_id == JS_CLASS_FLOAT32_ARRAY) {
            union {
                float f;
                uint32_t i;
            } u;
            u.f = d;
            v = u.i;
        } else {
            JSFloat64Union u;
            u.d = d;
            v64 = u.u64;
        }
    }
    littleEndian = argc > 2 && JS_ToBool(ctx, argv[2]);
    is_swap = littleEndian ^ !is_be();
    abuf = ta->buffer->u.array_buffer;
    if (abuf->detached)
        return JS_ThrowTypeErrorDetachedArrayBuffer(ctx);
    // order matters: this check should come before the next one
    if ((pos + size) > ta->length)
        return JS_ThrowRangeError(ctx, "out of bound");
    // test262 expects a TypeError for this and V8, in its infinite wisdom,
    // throws a "detached array buffer" exception, but IMO that doesn't make
    // sense because the buffer is not in fact detached, it's still there
    if ((int64_t)ta->offset + ta->length > abuf->byte_length)
        return JS_ThrowTypeError(ctx, "out of bound");
    ptr = abuf->data + ta->offset + pos;

    switch(class_id) {
    case JS_CLASS_INT8_ARRAY:
    case JS_CLASS_UINT8_ARRAY:
        *ptr = v;
        break;
    case JS_CLASS_INT16_ARRAY:
    case JS_CLASS_UINT16_ARRAY:
    case JS_CLASS_FLOAT16_ARRAY:
        if (is_swap)
            v = bswap16(v);
        put_u16(ptr, v);
        break;
    case JS_CLASS_INT32_ARRAY:
    case JS_CLASS_UINT32_ARRAY:
    case JS_CLASS_FLOAT32_ARRAY:
        if (is_swap)
            v = bswap32(v);
        put_u32(ptr, v);
        break;
    case JS_CLASS_BIG_INT64_ARRAY:
    case JS_CLASS_BIG_UINT64_ARRAY:
    case JS_CLASS_FLOAT64_ARRAY:
        if (is_swap)
            v64 = bswap64(v64);
        put_u64(ptr, v64);
        break;
    default:
        abort();
    }
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry js_dataview_proto_funcs[] = {
    JS_CGETSET_DEF("buffer", js_dataview_get_buffer, NULL ),
    JS_CGETSET_DEF("byteLength", js_dataview_get_byteLength, NULL ),
    JS_CGETSET_DEF("byteOffset", js_dataview_get_byteOffset, NULL ),
    JS_CFUNC_MAGIC_DEF("getInt8", 1, js_dataview_getValue, JS_CLASS_INT8_ARRAY ),
    JS_CFUNC_MAGIC_DEF("getUint8", 1, js_dataview_getValue, JS_CLASS_UINT8_ARRAY ),
    JS_CFUNC_MAGIC_DEF("getInt16", 1, js_dataview_getValue, JS_CLASS_INT16_ARRAY ),
    JS_CFUNC_MAGIC_DEF("getUint16", 1, js_dataview_getValue, JS_CLASS_UINT16_ARRAY ),
    JS_CFUNC_MAGIC_DEF("getInt32", 1, js_dataview_getValue, JS_CLASS_INT32_ARRAY ),
    JS_CFUNC_MAGIC_DEF("getUint32", 1, js_dataview_getValue, JS_CLASS_UINT32_ARRAY ),
    JS_CFUNC_MAGIC_DEF("getBigInt64", 1, js_dataview_getValue, JS_CLASS_BIG_INT64_ARRAY ),
    JS_CFUNC_MAGIC_DEF("getBigUint64", 1, js_dataview_getValue, JS_CLASS_BIG_UINT64_ARRAY ),
    JS_CFUNC_MAGIC_DEF("getFloat16", 1, js_dataview_getValue, JS_CLASS_FLOAT16_ARRAY ),
    JS_CFUNC_MAGIC_DEF("getFloat32", 1, js_dataview_getValue, JS_CLASS_FLOAT32_ARRAY ),
    JS_CFUNC_MAGIC_DEF("getFloat64", 1, js_dataview_getValue, JS_CLASS_FLOAT64_ARRAY ),
    JS_CFUNC_MAGIC_DEF("setInt8", 2, js_dataview_setValue, JS_CLASS_INT8_ARRAY ),
    JS_CFUNC_MAGIC_DEF("setUint8", 2, js_dataview_setValue, JS_CLASS_UINT8_ARRAY ),
    JS_CFUNC_MAGIC_DEF("setInt16", 2, js_dataview_setValue, JS_CLASS_INT16_ARRAY ),
    JS_CFUNC_MAGIC_DEF("setUint16", 2, js_dataview_setValue, JS_CLASS_UINT16_ARRAY ),
    JS_CFUNC_MAGIC_DEF("setInt32", 2, js_dataview_setValue, JS_CLASS_INT32_ARRAY ),
    JS_CFUNC_MAGIC_DEF("setUint32", 2, js_dataview_setValue, JS_CLASS_UINT32_ARRAY ),
    JS_CFUNC_MAGIC_DEF("setBigInt64", 2, js_dataview_setValue, JS_CLASS_BIG_INT64_ARRAY ),
    JS_CFUNC_MAGIC_DEF("setBigUint64", 2, js_dataview_setValue, JS_CLASS_BIG_UINT64_ARRAY ),
    JS_CFUNC_MAGIC_DEF("setFloat16", 2, js_dataview_setValue, JS_CLASS_FLOAT16_ARRAY ),
    JS_CFUNC_MAGIC_DEF("setFloat32", 2, js_dataview_setValue, JS_CLASS_FLOAT32_ARRAY ),
    JS_CFUNC_MAGIC_DEF("setFloat64", 2, js_dataview_setValue, JS_CLASS_FLOAT64_ARRAY ),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "DataView", JS_PROP_CONFIGURABLE ),
};

