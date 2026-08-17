/* Symbol */

static JSValue js_symbol_constructor(JSContext *ctx, JSValueConst new_target,
                                     int argc, JSValueConst *argv)
{
    JSValue str;
    JSString *p;

    if (!JS_IsUndefined(new_target))
        return JS_ThrowTypeErrorNotAConstructor(ctx, new_target);
    if (argc == 0 || JS_IsUndefined(argv[0])) {
        p = NULL;
    } else {
        str = JS_ToString(ctx, argv[0]);
        if (JS_IsException(str))
            return JS_EXCEPTION;
        p = JS_VALUE_GET_STRING(str);
    }
    return JS_NewSymbol(ctx, p, JS_ATOM_TYPE_SYMBOL);
}

static JSValue js_thisSymbolValue(JSContext *ctx, JSValueConst this_val)
{
    if (JS_VALUE_GET_TAG(this_val) == JS_TAG_SYMBOL)
        return JS_DupValue(ctx, this_val);

    if (JS_VALUE_GET_TAG(this_val) == JS_TAG_OBJECT) {
        JSObject *p = JS_VALUE_GET_OBJ(this_val);
        if (p->class_id == JS_CLASS_SYMBOL) {
            if (JS_VALUE_GET_TAG(p->u.object_data) == JS_TAG_SYMBOL)
                return JS_DupValue(ctx, p->u.object_data);
        }
    }
    return JS_ThrowTypeError(ctx, "not a symbol");
}

static JSValue js_symbol_toString(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    JSValue val, ret;
    val = js_thisSymbolValue(ctx, this_val);
    if (JS_IsException(val))
        return val;
    /* XXX: use JS_ToStringInternal() with a flags */
    ret = js_string_constructor(ctx, JS_UNDEFINED, 1, (JSValueConst *)&val);
    JS_FreeValue(ctx, val);
    return ret;
}

static JSValue js_symbol_valueOf(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    return js_thisSymbolValue(ctx, this_val);
}

static JSValue js_symbol_get_description(JSContext *ctx, JSValueConst this_val)
{
    JSValue val, ret;
    JSAtomStruct *p;

    val = js_thisSymbolValue(ctx, this_val);
    if (JS_IsException(val))
        return val;
    p = JS_VALUE_GET_PTR(val);
    if (p->len == 0 && p->is_wide_char != 0) {
        ret = JS_UNDEFINED;
    } else {
        ret = JS_AtomToString(ctx, js_get_atom_index(ctx->rt, p));
    }
    JS_FreeValue(ctx, val);
    return ret;
}

static const JSCFunctionListEntry js_symbol_proto_funcs[] = {
    JS_CFUNC_DEF("toString", 0, js_symbol_toString ),
    JS_CFUNC_DEF("valueOf", 0, js_symbol_valueOf ),
    // XXX: should have writable: false
    JS_CFUNC_DEF("[Symbol.toPrimitive]", 1, js_symbol_valueOf ),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "Symbol", JS_PROP_CONFIGURABLE ),
    JS_CGETSET_DEF("description", js_symbol_get_description, NULL ),
};

static JSValue js_symbol_for(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    JSValue str;

    str = JS_ToString(ctx, argv[0]);
    if (JS_IsException(str))
        return JS_EXCEPTION;
    return JS_NewSymbol(ctx, JS_VALUE_GET_STRING(str), JS_ATOM_TYPE_GLOBAL_SYMBOL);
}

static JSValue js_symbol_keyFor(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    JSAtomStruct *p;

    if (!JS_IsSymbol(argv[0]))
        return JS_ThrowTypeError(ctx, "not a symbol");
    p = JS_VALUE_GET_PTR(argv[0]);
    if (p->atom_type != JS_ATOM_TYPE_GLOBAL_SYMBOL)
        return JS_UNDEFINED;
    return JS_DupValue(ctx, JS_MKPTR(JS_TAG_STRING, p));
}

static const JSCFunctionListEntry js_symbol_funcs[] = {
    JS_CFUNC_DEF("for", 1, js_symbol_for ),
    JS_CFUNC_DEF("keyFor", 1, js_symbol_keyFor ),
    JS_PROP_ATOM_DEF("toPrimitive", JS_ATOM_Symbol_toPrimitive, 0),
    JS_PROP_ATOM_DEF("iterator", JS_ATOM_Symbol_iterator, 0),
    JS_PROP_ATOM_DEF("match", JS_ATOM_Symbol_match, 0),
    JS_PROP_ATOM_DEF("matchAll", JS_ATOM_Symbol_matchAll, 0),
    JS_PROP_ATOM_DEF("replace", JS_ATOM_Symbol_replace, 0),
    JS_PROP_ATOM_DEF("search", JS_ATOM_Symbol_search, 0),
    JS_PROP_ATOM_DEF("split", JS_ATOM_Symbol_split, 0),
    JS_PROP_ATOM_DEF("toStringTag", JS_ATOM_Symbol_toStringTag, 0),
    JS_PROP_ATOM_DEF("isConcatSpreadable", JS_ATOM_Symbol_isConcatSpreadable, 0),
    JS_PROP_ATOM_DEF("hasInstance", JS_ATOM_Symbol_hasInstance, 0),
    JS_PROP_ATOM_DEF("species", JS_ATOM_Symbol_species, 0),
    JS_PROP_ATOM_DEF("unscopables", JS_ATOM_Symbol_unscopables, 0),
    JS_PROP_ATOM_DEF("asyncIterator", JS_ATOM_Symbol_asyncIterator, 0),
    JS_PROP_ATOM_DEF("dispose", JS_ATOM_Symbol_dispose, 0),
    JS_PROP_ATOM_DEF("asyncDispose", JS_ATOM_Symbol_asyncDispose, 0),
};

/* Set/Map/WeakSet/WeakMap */

static BOOL js_weakref_is_target(JSValueConst val)
{
    switch (JS_VALUE_GET_TAG(val)) {
    case JS_TAG_OBJECT:
        return TRUE;
    case JS_TAG_SYMBOL:
        {
            JSAtomStruct *p = JS_VALUE_GET_PTR(val);
            if (p->atom_type == JS_ATOM_TYPE_SYMBOL &&
                p->hash != JS_ATOM_HASH_PRIVATE)
                return TRUE;
        }
        break;
    default:
        break;
    }
    return FALSE;
}

/* JS_UNDEFINED is considered as a live weakref */
/* XXX: add a specific JSWeakRef value type ? */
static BOOL js_weakref_is_live(JSValueConst val)
{
    void *p;
    if (JS_IsUndefined(val))
        return TRUE;
    p = JS_VALUE_GET_PTR(val);
    return (js_rc(p)->ref_count != 0);
}

/* 'val' can be JS_UNDEFINED */
static void js_weakref_free(JSRuntime *rt, JSValue val)
{
    if (JS_VALUE_GET_TAG(val) == JS_TAG_OBJECT) {
        JSObject *p = JS_VALUE_GET_OBJ(val);
        assert(p->weakref_count >= 1);
        p->weakref_count--;
        /* 'mark' is tested to avoid freeing the object structure when
           it is about to be freed in a cycle or in
           free_zero_refcount() */
        if (p->weakref_count == 0 && js_rc(p)->ref_count == 0 &&
            js_rc(p)->mark == 0) {
            js_free_rt(rt, p);
        }
    } else if (JS_VALUE_GET_TAG(val) == JS_TAG_SYMBOL) {
        JSString *p = JS_VALUE_GET_STRING(val);
        assert(p->hash >= 1);
        p->hash--;
        if (p->hash == 0 && js_rc(p)->ref_count == 0) {
            /* can remove the dummy structure */
            js_free_rt(rt, p);
        }
    }
}

/* val must be an object, a symbol or undefined (see
   js_weakref_is_target). */
static JSValue js_weakref_new(JSContext *ctx, JSValueConst val)
{
    if (JS_VALUE_GET_TAG(val) == JS_TAG_OBJECT) {
        JSObject *p = JS_VALUE_GET_OBJ(val);
        p->weakref_count++;
    } else if (JS_VALUE_GET_TAG(val) == JS_TAG_SYMBOL) {
        JSString *p = JS_VALUE_GET_STRING(val);
        /* XXX: could return an exception if too many references */
        assert(p->hash < JS_ATOM_HASH_MASK - 2);
        p->hash++;
    } else {
        assert(JS_IsUndefined(val));
    }
    return (JSValue)val;
}

#define MAGIC_SET (1 << 0)
#define MAGIC_WEAK (1 << 1)

static JSValue js_map_constructor(JSContext *ctx, JSValueConst new_target,
                                  int argc, JSValueConst *argv, int magic)
{
    JSMapState *s;
    JSValue obj, adder = JS_UNDEFINED, iter = JS_UNDEFINED, next_method = JS_UNDEFINED;
    JSValueConst arr;
    BOOL is_set, is_weak;

    is_set = magic & MAGIC_SET;
    is_weak = ((magic & MAGIC_WEAK) != 0);
    obj = js_create_from_ctor(ctx, new_target, JS_CLASS_MAP + magic);
    if (JS_IsException(obj))
        return JS_EXCEPTION;
    s = js_mallocz(ctx, sizeof(*s));
    if (!s)
        goto fail;
    init_list_head(&s->records);
    s->is_weak = is_weak;
    if (is_weak) {
        s->weakref_header.weakref_type = JS_WEAKREF_TYPE_MAP;
        list_add_tail(&s->weakref_header.link, &ctx->rt->weakref_list);
    }
    JS_SetOpaque(obj, s);
    s->hash_bits = 1;
    s->hash_size = 1U << s->hash_bits;
    s->hash_table = js_mallocz(ctx, sizeof(s->hash_table[0]) * s->hash_size);
    if (!s->hash_table)
        goto fail;
    s->record_count_threshold = 4;

    arr = JS_UNDEFINED;
    if (argc > 0)
        arr = argv[0];
    if (!JS_IsUndefined(arr) && !JS_IsNull(arr)) {
        JSValue item, ret;
        BOOL done;

        adder = JS_GetProperty(ctx, obj, is_set ? JS_ATOM_add : JS_ATOM_set);
        if (JS_IsException(adder))
            goto fail;
        if (!JS_IsFunction(ctx, adder)) {
            JS_ThrowTypeError(ctx, "set/add is not a function");
            goto fail;
        }

        iter = JS_GetIterator(ctx, arr, FALSE);
        if (JS_IsException(iter))
            goto fail;
        next_method = JS_GetProperty(ctx, iter, JS_ATOM_next);
        if (JS_IsException(next_method))
            goto fail;

        for(;;) {
            item = JS_IteratorNext(ctx, iter, next_method, 0, NULL, &done);
            if (JS_IsException(item))
                goto fail;
            if (done)
                break;
            if (is_set) {
                ret = JS_Call(ctx, adder, obj, 1, (JSValueConst *)&item);
                if (JS_IsException(ret)) {
                    JS_FreeValue(ctx, item);
                    goto fail_close;
                }
            } else {
                JSValue key, value;
                JSValueConst args[2];
                key = JS_UNDEFINED;
                value = JS_UNDEFINED;
                if (!JS_IsObject(item)) {
                    JS_ThrowTypeErrorNotAnObject(ctx);
                    goto fail1;
                }
                key = JS_GetPropertyUint32(ctx, item, 0);
                if (JS_IsException(key))
                    goto fail1;
                value = JS_GetPropertyUint32(ctx, item, 1);
                if (JS_IsException(value))
                    goto fail1;
                args[0] = key;
                args[1] = value;
                ret = JS_Call(ctx, adder, obj, 2, args);
                if (JS_IsException(ret)) {
                fail1:
                    JS_FreeValue(ctx, item);
                    JS_FreeValue(ctx, key);
                    JS_FreeValue(ctx, value);
                    goto fail_close;
                }
                JS_FreeValue(ctx, key);
                JS_FreeValue(ctx, value);
            }
            JS_FreeValue(ctx, ret);
            JS_FreeValue(ctx, item);
        }
        JS_FreeValue(ctx, next_method);
        JS_FreeValue(ctx, iter);
        JS_FreeValue(ctx, adder);
    }
    return obj;
 fail_close:
    /* close the iterator object, preserving pending exception */
    JS_IteratorClose(ctx, iter, TRUE);
 fail:
    JS_FreeValue(ctx, next_method);
    JS_FreeValue(ctx, iter);
    JS_FreeValue(ctx, adder);
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
}

/* XXX: could normalize strings to speed up comparison */
static JSValue map_normalize_key(JSContext *ctx, JSValue key)
{
    uint32_t tag = JS_VALUE_GET_TAG(key);
    /* convert -0.0 to +0.0 */
    if (JS_TAG_IS_FLOAT64(tag) && JS_VALUE_GET_FLOAT64(key) == 0.0) {
        key = JS_NewInt32(ctx, 0);
    }
    return key;
}

static JSValueConst map_normalize_key_const(JSContext *ctx, JSValueConst key)
{
    return (JSValueConst)map_normalize_key(ctx, (JSValue)key);
}

/* hash multipliers, same as the Linux kernel (see Knuth vol 3,
   section 6.4, exercise 9) */
#define HASH_MUL32 0x61C88647
#define HASH_MUL64 UINT64_C(0x61C8864680B583EB)

static uint32_t map_hash32(uint32_t a, int hash_bits)
{
    return (a * HASH_MUL32) >> (32 - hash_bits);
}

static uint32_t map_hash64(uint64_t a, int hash_bits)
{
    return (a * HASH_MUL64) >> (64 - hash_bits);
}

static uint32_t map_hash_pointer(uintptr_t a, int hash_bits)
{
#ifdef JS_PTR64
    return map_hash64(a, hash_bits);
#else
    return map_hash32(a, hash_bits);
#endif
}

/* XXX: better hash ? */
/* precondition: 1 <= hash_bits <= 32 */
static uint32_t map_hash_key(JSValueConst key, int hash_bits)
{
    uint32_t tag = JS_VALUE_GET_NORM_TAG(key);
    uint32_t h;
    double d;
    JSBigInt *p;
    JSBigIntBuf buf;
    
    switch(tag) {
    case JS_TAG_BOOL:
        h = map_hash32(JS_VALUE_GET_INT(key) ^ JS_TAG_BOOL, hash_bits);
        break;
    case JS_TAG_STRING:
        h = map_hash32(hash_string(JS_VALUE_GET_STRING(key), 0) ^ JS_TAG_STRING, hash_bits);
        break;
    case JS_TAG_STRING_ROPE:
        h = map_hash32(hash_string_rope(key, 0) ^ JS_TAG_STRING, hash_bits);
        break;
    case JS_TAG_OBJECT:
    case JS_TAG_SYMBOL:
        h = map_hash_pointer((uintptr_t)JS_VALUE_GET_PTR(key) ^ tag, hash_bits);
        break;
    case JS_TAG_INT:
        d = JS_VALUE_GET_INT(key);
        goto hash_float64;
    case JS_TAG_FLOAT64:
        d = JS_VALUE_GET_FLOAT64(key);
        /* normalize the NaN */
        if (isnan(d))
            d = JS_FLOAT64_NAN;
    hash_float64:
        h = map_hash64(float64_as_uint64(d) ^ JS_TAG_FLOAT64, hash_bits);
        break;
    case JS_TAG_SHORT_BIG_INT:
        p = js_bigint_set_short(&buf, key);
        goto hash_bigint;
    case JS_TAG_BIG_INT:
        p = JS_VALUE_GET_PTR(key);
    hash_bigint:
        {
            int i;
            h = 1;
            for(i = p->len - 1; i >= 0; i--) {
                h = h * 263 + p->tab[i];
            }
            /* the final step is necessary otherwise h mod n only
               depends of p->tab[i] mod n */
            h = map_hash32(h ^ JS_TAG_BIG_INT, hash_bits);
        }
        break;
    default:
        h = 0;
        break;
    }
    return h;
}

static JSMapRecord *map_find_record(JSContext *ctx, JSMapState *s,
                                    JSValueConst key)
{
    JSMapRecord *mr;
    uint32_t h;
    h = map_hash_key(key, s->hash_bits);
    for(mr = s->hash_table[h]; mr != NULL; mr = mr->hash_next) {
        if (mr->empty || (s->is_weak && !js_weakref_is_live(mr->key))) {
            /* cannot match */
        } else {
            if (js_same_value_zero(ctx, mr->key, key))
                return mr;
        }
    }
    return NULL;
}

static void map_hash_resize(JSContext *ctx, JSMapState *s)
{
    uint32_t new_hash_size, h;
    int new_hash_bits;
    struct list_head *el;
    JSMapRecord *mr, **new_hash_table;

    /* XXX: no reporting of memory allocation failure */
    new_hash_bits = min_int(s->hash_bits + 1, 31);
    new_hash_size = 1U << new_hash_bits;
    new_hash_table = js_realloc(ctx, s->hash_table,
                                sizeof(new_hash_table[0]) * new_hash_size);
    if (!new_hash_table)
        return;

    memset(new_hash_table, 0, sizeof(new_hash_table[0]) * new_hash_size);

    list_for_each(el, &s->records) {
        mr = list_entry(el, JSMapRecord, link);
        if (mr->empty || (s->is_weak && !js_weakref_is_live(mr->key))) {
        } else {
            h = map_hash_key(mr->key, new_hash_bits);
            mr->hash_next = new_hash_table[h];
            new_hash_table[h] = mr;
        }
    }
    s->hash_table = new_hash_table;
    s->hash_bits = new_hash_bits;
    s->hash_size = new_hash_size;
    s->record_count_threshold = new_hash_size * 2;
}

static JSMapRecord *map_add_record(JSContext *ctx, JSMapState *s,
                                   JSValueConst key)
{
    uint32_t h;
    JSMapRecord *mr;

    mr = js_malloc(ctx, sizeof(*mr));
    if (!mr)
        return NULL;
    mr->ref_count = 1;
    mr->empty = FALSE;
    if (s->is_weak) {
        mr->key = js_weakref_new(ctx, key);
    } else {
        mr->key = JS_DupValue(ctx, key);
    }
    h = map_hash_key(key, s->hash_bits);
    mr->hash_next = s->hash_table[h];
    s->hash_table[h] = mr;
    list_add_tail(&mr->link, &s->records);
    s->record_count++;
    if (s->record_count >= s->record_count_threshold) {
        map_hash_resize(ctx, s);
    }
    return mr;
}

static JSMapRecord *set_add_record(JSContext *ctx, JSMapState *s,
                                   JSValueConst key)
{
    JSMapRecord *mr;
    mr = map_add_record(ctx, s, key);
    if (!mr)
        return NULL;
    mr->value = JS_UNDEFINED;
    return mr;
}

/* warning: the record must be removed from the hash table before */
static void map_delete_record_internal(JSRuntime *rt, JSMapState *s, JSMapRecord *mr)
{
    if (mr->empty)
        return;
    
    if (s->is_weak) {
        js_weakref_free(rt, mr->key);
    } else {
        JS_FreeValueRT(rt, mr->key);
    }
    JS_FreeValueRT(rt, mr->value);
    if (--mr->ref_count == 0) {
        list_del(&mr->link);
        js_free_rt(rt, mr);
    } else {
        /* keep a zombie record for iterators */
        mr->empty = TRUE;
        mr->key = JS_UNDEFINED;
        mr->value = JS_UNDEFINED;
    }
    s->record_count--;
}

static void map_decref_record(JSRuntime *rt, JSMapRecord *mr)
{
    if (--mr->ref_count == 0) {
        /* the record can be safely removed */
        assert(mr->empty);
        list_del(&mr->link);
        js_free_rt(rt, mr);
    }
}

static void map_delete_weakrefs(JSRuntime *rt, JSWeakRefHeader *wh)
{
    JSMapState *s = container_of(wh, JSMapState, weakref_header);
    struct list_head *el, *el1;
    JSMapRecord *mr1, **pmr;
    uint32_t h;

    list_for_each_safe(el, el1, &s->records) {
        JSMapRecord *mr = list_entry(el, JSMapRecord, link);
        if (!js_weakref_is_live(mr->key)) {

            /* even if key is not live it can be hashed as a pointer */
            h = map_hash_key(mr->key, s->hash_bits);
            pmr = &s->hash_table[h];
            for(;;) {
                mr1 = *pmr;
                /* the entry may already be removed from the hash
                   table if the map was resized */
                if (mr1 == NULL)
                    goto done; 
                if (mr1 == mr)
                    break;
                pmr = &mr1->hash_next;
            }
            /* remove from the hash table */
            *pmr = mr1->hash_next;
        done:
            map_delete_record_internal(rt, s, mr);
        }
    }
}

static JSValue js_map_set(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv, int magic)
{
    JSMapState *s = JS_GetOpaque2(ctx, this_val, JS_CLASS_MAP + magic);
    JSMapRecord *mr;
    JSValueConst key, value;

    if (!s)
        return JS_EXCEPTION;
    key = map_normalize_key_const(ctx, argv[0]);
    if (s->is_weak && !js_weakref_is_target(key))
        return JS_ThrowTypeError(ctx, "invalid value used as %s key", (magic & MAGIC_SET) ? "WeakSet" : "WeakMap");
    if (magic & MAGIC_SET)
        value = JS_UNDEFINED;
    else
        value = argv[1];
    mr = map_find_record(ctx, s, key);
    if (mr) {
        JS_FreeValue(ctx, mr->value);
    } else {
        mr = map_add_record(ctx, s, key);
        if (!mr)
            return JS_EXCEPTION;
    }
    mr->value = JS_DupValue(ctx, value);
    return JS_DupValue(ctx, this_val);
}

static JSValue js_map_get(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv, int magic)
{
    JSMapState *s = JS_GetOpaque2(ctx, this_val, JS_CLASS_MAP + magic);
    JSMapRecord *mr;
    JSValueConst key;

    if (!s)
        return JS_EXCEPTION;
    key = map_normalize_key_const(ctx, argv[0]);
    mr = map_find_record(ctx, s, key);
    if (!mr)
        return JS_UNDEFINED;
    else
        return JS_DupValue(ctx, mr->value);
}

/* return JS_TRUE or JS_FALSE */
static JSValue map_delete_record(JSContext *ctx, JSMapState *s, JSValueConst key)
{
    JSMapRecord *mr, **pmr;
    uint32_t h;

    key = map_normalize_key_const(ctx, key);
    
    h = map_hash_key(key, s->hash_bits);
    pmr = &s->hash_table[h];
    for(;;) {
        mr = *pmr;
        if (mr == NULL)
            return JS_FALSE;
        if (mr->empty || (s->is_weak && !js_weakref_is_live(mr->key))) {
            /* not valid */
        } else {
            if (js_same_value_zero(ctx, mr->key, key))
                break;
        }
        pmr = &mr->hash_next;
    }

    /* remove from the hash table */
    *pmr = mr->hash_next;
    
    map_delete_record_internal(ctx->rt, s, mr);
    return JS_TRUE;
}

static JSValue js_map_getOrInsert(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv, int magic)
{
    BOOL computed = magic & 1;
    JSClassID class_id = magic >> 1;
    JSMapState *s = JS_GetOpaque2(ctx, this_val, class_id);
    JSMapRecord *mr;
    JSValueConst key;
    JSValue value;

    if (!s)
        return JS_EXCEPTION;
    if (computed && !JS_IsFunction(ctx, argv[1]))
        return JS_ThrowTypeError(ctx, "not a function");
    key = map_normalize_key_const(ctx, argv[0]);
    if (s->is_weak && !js_weakref_is_target(key))
        return JS_ThrowTypeError(ctx, "invalid value used as WeakMap key");
    mr = map_find_record(ctx, s, key);
    if (!mr) {
        if (computed) {
            value = JS_Call(ctx, argv[1], JS_UNDEFINED, 1, &key);
            if (JS_IsException(value))
                return JS_EXCEPTION;
            map_delete_record(ctx, s, key);
        } else {
            value = JS_DupValue(ctx, argv[1]);
        }
        mr = map_add_record(ctx, s, key);
        if (!mr) {
            JS_FreeValue(ctx, value);
            return JS_EXCEPTION;
        }
        mr->value = value;
    }
    return JS_DupValue(ctx, mr->value);
}

static JSValue js_map_has(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv, int magic)
{
    JSMapState *s = JS_GetOpaque2(ctx, this_val, JS_CLASS_MAP + magic);
    JSMapRecord *mr;
    JSValueConst key;

    if (!s)
        return JS_EXCEPTION;
    key = map_normalize_key_const(ctx, argv[0]);
    mr = map_find_record(ctx, s, key);
    return JS_NewBool(ctx, mr != NULL);
}

static JSValue js_map_delete(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv, int magic)
{
    JSMapState *s = JS_GetOpaque2(ctx, this_val, JS_CLASS_MAP + magic);
    if (!s)
        return JS_EXCEPTION;
    return map_delete_record(ctx, s, argv[0]);
}

static JSValue js_map_clear(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv, int magic)
{
    JSMapState *s = JS_GetOpaque2(ctx, this_val, JS_CLASS_MAP + magic);
    struct list_head *el, *el1;
    JSMapRecord *mr;

    if (!s)
        return JS_EXCEPTION;

    /* remove from the hash table */
    memset(s->hash_table, 0, sizeof(s->hash_table[0]) * s->hash_size);
    
    list_for_each_safe(el, el1, &s->records) {
        mr = list_entry(el, JSMapRecord, link);
        map_delete_record_internal(ctx->rt, s, mr);
    }
    return JS_UNDEFINED;
}

static JSValue js_map_get_size(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSMapState *s = JS_GetOpaque2(ctx, this_val, JS_CLASS_MAP + magic);
    if (!s)
        return JS_EXCEPTION;
    return JS_NewUint32(ctx, s->record_count);
}

static JSValue js_map_forEach(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv, int magic)
{
    JSMapState *s = JS_GetOpaque2(ctx, this_val, JS_CLASS_MAP + magic);
    JSValueConst func, this_arg;
    JSValue ret, args[3];
    struct list_head *el;
    JSMapRecord *mr;

    if (!s)
        return JS_EXCEPTION;
    func = argv[0];
    if (argc > 1)
        this_arg = argv[1];
    else
        this_arg = JS_UNDEFINED;
    if (check_function(ctx, func))
        return JS_EXCEPTION;
    /* Note: the list can be modified while traversing it, but the
       current element is locked */
    el = s->records.next;
    while (el != &s->records) {
        mr = list_entry(el, JSMapRecord, link);
        if (!mr->empty) {
            mr->ref_count++;
            /* must duplicate in case the record is deleted */
            args[1] = JS_DupValue(ctx, mr->key);
            if (magic)
                args[0] = args[1];
            else
                args[0] = JS_DupValue(ctx, mr->value);
            args[2] = (JSValue)this_val;
            ret = JS_Call(ctx, func, this_arg, 3, (JSValueConst *)args);
            JS_FreeValue(ctx, args[0]);
            if (!magic)
                JS_FreeValue(ctx, args[1]);
            el = el->next;
            map_decref_record(ctx->rt, mr);
            if (JS_IsException(ret))
                return ret;
            JS_FreeValue(ctx, ret);
        } else {
            el = el->next;
        }
    }
    return JS_UNDEFINED;
}

static JSValue js_object_groupBy(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv, int is_map)
{
    JSValueConst cb, args[2];
    JSValue res, iter, next, groups, key, v, prop;
    JSAtom key_atom = JS_ATOM_NULL;
    int64_t idx;
    BOOL done;

    // "is function?" check must be observed before argv[0] is accessed
    cb = argv[1];
    if (check_function(ctx, cb))
        return JS_EXCEPTION;

    iter = JS_GetIterator(ctx, argv[0], /*is_async*/FALSE);
    if (JS_IsException(iter))
        return JS_EXCEPTION;

    key = JS_UNDEFINED;
    key_atom = JS_ATOM_NULL;
    v = JS_UNDEFINED;
    prop = JS_UNDEFINED;
    groups = JS_UNDEFINED;

    next = JS_GetProperty(ctx, iter, JS_ATOM_next);
    if (JS_IsException(next))
        goto exception;

    if (is_map) {
        groups = js_map_constructor(ctx, JS_UNDEFINED, 0, NULL, 0);
    } else {
        groups = JS_NewObjectProto(ctx, JS_NULL);
    }
    if (JS_IsException(groups))
        goto exception;

    for (idx = 0; ; idx++) {
        if (idx >= MAX_SAFE_INTEGER) {
            JS_ThrowTypeError(ctx, "too many elements");
            goto iterator_close_exception;
        }
        v = JS_IteratorNext(ctx, iter, next, 0, NULL, &done);
        if (JS_IsException(v))
            goto exception;
        if (done)
            break; // v is JS_UNDEFINED

        args[0] = v;
        args[1] = JS_NewInt64(ctx, idx);
        key = JS_Call(ctx, cb, ctx->global_obj, 2, args);
        if (JS_IsException(key))
            goto iterator_close_exception;

        if (is_map) {
            prop = js_map_get(ctx, groups, 1, (JSValueConst *)&key, 0);
        } else {
            key_atom = JS_ValueToAtom(ctx, key);
            JS_FreeValue(ctx, key);
            key = JS_UNDEFINED;
            if (key_atom == JS_ATOM_NULL)
                goto iterator_close_exception;
            prop = JS_GetProperty(ctx, groups, key_atom);
        }
        if (JS_IsException(prop))
            goto exception;

        if (JS_IsUndefined(prop)) {
            prop = JS_NewArray(ctx);
            if (JS_IsException(prop))
                goto exception;
            if (is_map) {
                args[0] = key;
                args[1] = prop;
                res = js_map_set(ctx, groups, 2, args, 0);
                if (JS_IsException(res))
                    goto exception;
                JS_FreeValue(ctx, res);
            } else {
                prop = JS_DupValue(ctx, prop);
                if (JS_DefinePropertyValue(ctx, groups, key_atom, prop,
                                           JS_PROP_C_W_E) < 0) {
                    goto exception;
                }
            }
        }
        res = js_array_push(ctx, prop, 1, (JSValueConst *)&v, /*unshift*/0);
        if (JS_IsException(res))
            goto exception;
        // res is an int64

        JS_FreeValue(ctx, prop);
        JS_FreeValue(ctx, key);
        JS_FreeAtom(ctx, key_atom);
        JS_FreeValue(ctx, v);
        prop = JS_UNDEFINED;
        key = JS_UNDEFINED;
        key_atom = JS_ATOM_NULL;
        v = JS_UNDEFINED;
    }

    JS_FreeValue(ctx, iter);
    JS_FreeValue(ctx, next);
    return groups;

 iterator_close_exception:
    JS_IteratorClose(ctx, iter, TRUE);
 exception:
    JS_FreeAtom(ctx, key_atom);
    JS_FreeValue(ctx, prop);
    JS_FreeValue(ctx, key);
    JS_FreeValue(ctx, v);
    JS_FreeValue(ctx, groups);
    JS_FreeValue(ctx, iter);
    JS_FreeValue(ctx, next);
    return JS_EXCEPTION;
}

static void js_map_finalizer(JSRuntime *rt, JSValue val)
{
    JSObject *p;
    JSMapState *s;
    struct list_head *el, *el1;
    JSMapRecord *mr;

    p = JS_VALUE_GET_OBJ(val);
    s = p->u.map_state;
    if (s) {
        /* if the object is deleted we are sure that no iterator is
           using it */
        list_for_each_safe(el, el1, &s->records) {
            mr = list_entry(el, JSMapRecord, link);
            if (!mr->empty) {
                if (s->is_weak)
                    js_weakref_free(rt, mr->key);
                else
                    JS_FreeValueRT(rt, mr->key);
                JS_FreeValueRT(rt, mr->value);
            }
            js_free_rt(rt, mr);
        }
        js_free_rt(rt, s->hash_table);
        if (s->is_weak) {
            list_del(&s->weakref_header.link);
        }
        js_free_rt(rt, s);
    }
}

static void js_map_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    JSObject *p = JS_VALUE_GET_OBJ(val);
    JSMapState *s;
    struct list_head *el;
    JSMapRecord *mr;

    s = p->u.map_state;
    if (s) {
        list_for_each(el, &s->records) {
            mr = list_entry(el, JSMapRecord, link);
            if (!s->is_weak)
                JS_MarkValue(rt, mr->key, mark_func);
            JS_MarkValue(rt, mr->value, mark_func);
        }
    }
}

/* Map Iterator */

typedef struct JSMapIteratorData {
    JSValue obj;
    JSIteratorKindEnum kind;
    JSMapRecord *cur_record;
} JSMapIteratorData;

static void js_map_iterator_finalizer(JSRuntime *rt, JSValue val)
{
    JSObject *p;
    JSMapIteratorData *it;

    p = JS_VALUE_GET_OBJ(val);
    it = p->u.map_iterator_data;
    if (it) {
        /* During the GC sweep phase the Map finalizer may be
           called before the Map iterator finalizer */
        if (JS_IsLiveObject(rt, it->obj) && it->cur_record) {
            map_decref_record(rt, it->cur_record);
        }
        JS_FreeValueRT(rt, it->obj);
        js_free_rt(rt, it);
    }
}

static void js_map_iterator_mark(JSRuntime *rt, JSValueConst val,
                                 JS_MarkFunc *mark_func)
{
    JSObject *p = JS_VALUE_GET_OBJ(val);
    JSMapIteratorData *it;
    it = p->u.map_iterator_data;
    if (it) {
        /* the record is already marked by the object */
        JS_MarkValue(rt, it->obj, mark_func);
    }
}

static JSValue js_create_map_iterator(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv, int magic)
{
    JSIteratorKindEnum kind;
    JSMapState *s;
    JSMapIteratorData *it;
    JSValue enum_obj;

    kind = magic >> 2;
    magic &= 3;
    s = JS_GetOpaque2(ctx, this_val, JS_CLASS_MAP + magic);
    if (!s)
        return JS_EXCEPTION;
    enum_obj = JS_NewObjectClass(ctx, JS_CLASS_MAP_ITERATOR + magic);
    if (JS_IsException(enum_obj))
        goto fail;
    it = js_malloc(ctx, sizeof(*it));
    if (!it) {
        JS_FreeValue(ctx, enum_obj);
        goto fail;
    }
    it->obj = JS_DupValue(ctx, this_val);
    it->kind = kind;
    it->cur_record = NULL;
    JS_SetOpaque(enum_obj, it);
    return enum_obj;
 fail:
    return JS_EXCEPTION;
}

static JSValue js_map_iterator_next(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv,
                                    BOOL *pdone, int magic)
{
    JSMapIteratorData *it;
    JSMapState *s;
    JSMapRecord *mr;
    struct list_head *el;

    it = JS_GetOpaque2(ctx, this_val, JS_CLASS_MAP_ITERATOR + magic);
    if (!it) {
        *pdone = FALSE;
        return JS_EXCEPTION;
    }
    if (JS_IsUndefined(it->obj))
        goto done;
    s = JS_GetOpaque(it->obj, JS_CLASS_MAP + magic);
    assert(s != NULL);
    if (!it->cur_record) {
        el = s->records.next;
    } else {
        mr = it->cur_record;
        el = mr->link.next;
        map_decref_record(ctx->rt, mr); /* the record can be freed here */
    }
    for(;;) {
        if (el == &s->records) {
            /* no more record  */
            it->cur_record = NULL;
            JS_FreeValue(ctx, it->obj);
            it->obj = JS_UNDEFINED;
        done:
            /* end of enumeration */
            *pdone = TRUE;
            return JS_UNDEFINED;
        }
        mr = list_entry(el, JSMapRecord, link);
        if (!mr->empty)
            break;
        /* get the next record */
        el = mr->link.next;
    }

    /* lock the record so that it won't be freed */
    mr->ref_count++;
    it->cur_record = mr;
    *pdone = FALSE;

    if (it->kind == JS_ITERATOR_KIND_KEY) {
        return JS_DupValue(ctx, mr->key);
    } else {
        JSValueConst args[2];
        args[0] = mr->key;
        if (magic)
            args[1] = mr->key;
        else
            args[1] = mr->value;
        if (it->kind == JS_ITERATOR_KIND_VALUE) {
            return JS_DupValue(ctx, args[1]);
        } else {
            return js_create_array(ctx, 2, args);
        }
    }
}

static int get_set_record(JSContext *ctx, JSValueConst obj,
                          int64_t *psize, JSValue *phas, JSValue *pkeys)
{
    JSMapState *s;
    int64_t size;
    JSValue has = JS_UNDEFINED, keys = JS_UNDEFINED;
    
    s = JS_GetOpaque(obj, JS_CLASS_SET);
    if (s) {
        size = s->record_count;
    } else {
        JSValue v;
        double d;

        v = JS_GetProperty(ctx, obj, JS_ATOM_size);
        if (JS_IsException(v))
            goto exception;
        if (JS_ToFloat64Free(ctx, &d, v) < 0)
            goto exception;
        if (isnan(d)) {
            JS_ThrowTypeError(ctx, ".size is not a number");
            goto exception;
        }
        if (d < INT64_MIN)
            size = INT64_MIN;
        else if (d >= 0x1p63) /* must use INT64_MAX + 1 because INT64_MAX cannot be exactly represented as a double */
            size = INT64_MAX;
        else
            size = (int64_t)d;
        if (size < 0) {
            JS_ThrowRangeError(ctx, ".size must be positive");
            goto exception;
        }
    }

    has = JS_GetProperty(ctx, obj, JS_ATOM_has);
    if (JS_IsException(has))
        goto exception;
    if (JS_IsUndefined(has)) {
        JS_ThrowTypeError(ctx, ".has is undefined");
        goto exception;
    }
    if (!JS_IsFunction(ctx, has)) {
        JS_ThrowTypeError(ctx, ".has is not a function");
        goto exception;
    }

    keys = JS_GetProperty(ctx, obj, JS_ATOM_keys);
    if (JS_IsException(keys))
        goto exception;
    if (JS_IsUndefined(keys)) {
        JS_ThrowTypeError(ctx, ".keys is undefined");
        goto exception;
    }
    if (!JS_IsFunction(ctx, keys)) {
        JS_ThrowTypeError(ctx, ".keys is not a function");
        goto exception;
    }
    *psize = size;
    *phas = has;
    *pkeys = keys;
    return 0;

 exception:
    JS_FreeValue(ctx, has);
    JS_FreeValue(ctx, keys);
    *psize = 0;
    *phas = JS_UNDEFINED;
    *pkeys = JS_UNDEFINED;
    return -1;
}

/* copy 'this_val' in a new set without side effects */
static JSValue js_copy_set(JSContext *ctx, JSValueConst this_val)
{
    JSValue newset;
    JSMapState *s, *t;
    struct list_head *el;
    JSMapRecord *mr;
   
    s = JS_GetOpaque2(ctx, this_val, JS_CLASS_SET);
    if (!s)
        return JS_EXCEPTION;

    newset = js_map_constructor(ctx, JS_UNDEFINED, 0, NULL, MAGIC_SET);
    if (JS_IsException(newset))
        return JS_EXCEPTION;
    t = JS_GetOpaque(newset, JS_CLASS_SET);

    // can't clone this_val using js_map_constructor(),
    // test262 mandates we don't call the .add method
    list_for_each(el, &s->records) {
        mr = list_entry(el, JSMapRecord, link);
        if (mr->empty)
            continue;
        if (!set_add_record(ctx, t, mr->key))
            goto exception;
    }
    return newset;
 exception:
    JS_FreeValue(ctx, newset);
    return JS_EXCEPTION;
}

static JSValue js_set_isDisjointFrom(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    JSValue item, iter, keys, has, next, rv, rval;
    int done;
    BOOL found;
    JSMapState *s;
    int64_t size;
    int ok;

    iter = JS_UNDEFINED;
    next = JS_UNDEFINED;
    rval = JS_EXCEPTION;
    s = JS_GetOpaque2(ctx, this_val, JS_CLASS_SET);
    if (!s)
        return JS_EXCEPTION;
    if (get_set_record(ctx, argv[0], &size, &has, &keys) < 0)
        goto exception;
    if (s->record_count <= size) {
        iter = js_create_map_iterator(ctx, this_val, 0, NULL, MAGIC_SET);
        if (JS_IsException(iter))
            goto exception;
        found = FALSE;
        do {
            item = js_map_iterator_next(ctx, iter, 0, NULL, &done, MAGIC_SET);
            if (JS_IsException(item))
                goto exception;
            if (done) // item is JS_UNDEFINED
                break;
            rv = JS_Call(ctx, has, argv[0], 1, (JSValueConst *)&item);
            JS_FreeValue(ctx, item);
            ok = JS_ToBoolFree(ctx, rv); // returns -1 if rv is JS_EXCEPTION
            if (ok < 0)
                goto exception;
            found = (ok > 0);
        } while (!found);
    } else {
        iter = JS_Call(ctx, keys, argv[0], 0, NULL);
        if (JS_IsException(iter))
            goto exception;
        next = JS_GetProperty(ctx, iter, JS_ATOM_next);
        if (JS_IsException(next))
            goto exception;
        found = FALSE;
        for(;;) {
            item = JS_IteratorNext(ctx, iter, next, 0, NULL, &done);
            if (JS_IsException(item))
                goto exception;
            if (done) // item is JS_UNDEFINED
                break;
            item = map_normalize_key(ctx, item);
            found = (NULL != map_find_record(ctx, s, item));
            JS_FreeValue(ctx, item);
            if (found) {
                JS_IteratorClose(ctx, iter, FALSE);
                break;
            }
        }
    }
    rval = !found ? JS_TRUE : JS_FALSE;
exception:
    JS_FreeValue(ctx, has);
    JS_FreeValue(ctx, keys);
    JS_FreeValue(ctx, iter);
    JS_FreeValue(ctx, next);
    return rval;
}

static JSValue js_set_isSubsetOf(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    JSValue item, iter, keys, has, next, rv, rval;
    BOOL found;
    JSMapState *s;
    int64_t size;
    int done, ok;

    iter = JS_UNDEFINED;
    next = JS_UNDEFINED;
    rval = JS_EXCEPTION;
    s = JS_GetOpaque2(ctx, this_val, JS_CLASS_SET);
    if (!s)
        return JS_EXCEPTION;
    if (get_set_record(ctx, argv[0], &size, &has, &keys) < 0)
        goto exception;
    found = FALSE;
    if (s->record_count > size)
        goto fini;
    iter = js_create_map_iterator(ctx, this_val, 0, NULL, MAGIC_SET);
    if (JS_IsException(iter))
        goto exception;
    found = TRUE;
    do {
        item = js_map_iterator_next(ctx, iter, 0, NULL, &done, MAGIC_SET);
        if (JS_IsException(item))
            goto exception;
        if (done) // item is JS_UNDEFINED
            break;
        rv = JS_Call(ctx, has, argv[0], 1, (JSValueConst *)&item);
        JS_FreeValue(ctx, item);
        ok = JS_ToBoolFree(ctx, rv); // returns -1 if rv is JS_EXCEPTION
        if (ok < 0)
            goto exception;
        found = (ok > 0);
    } while (found);
fini:
    rval = found ? JS_TRUE : JS_FALSE;
exception:
    JS_FreeValue(ctx, has);
    JS_FreeValue(ctx, keys);
    JS_FreeValue(ctx, iter);
    JS_FreeValue(ctx, next);
    return rval;
}

static JSValue js_set_isSupersetOf(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    JSValue item, iter, keys, has, next, rval;
    int done;
    BOOL found;
    JSMapState *s;
    int64_t size;

    iter = JS_UNDEFINED;
    next = JS_UNDEFINED;
    rval = JS_EXCEPTION;
    s = JS_GetOpaque2(ctx, this_val, JS_CLASS_SET);
    if (!s)
        return JS_EXCEPTION;
    if (get_set_record(ctx, argv[0], &size, &has, &keys) < 0)
        goto exception;
    found = FALSE;
    if (s->record_count < size)
        goto fini;
    iter = JS_Call(ctx, keys, argv[0], 0, NULL);
    if (JS_IsException(iter))
        goto exception;
    next = JS_GetProperty(ctx, iter, JS_ATOM_next);
    if (JS_IsException(next))
        goto exception;
    found = TRUE;
    for(;;) {
        item = JS_IteratorNext(ctx, iter, next, 0, NULL, &done);
        if (JS_IsException(item))
            goto exception;
        if (done) // item is JS_UNDEFINED
            break;
        item = map_normalize_key(ctx, item);
        found = (NULL != map_find_record(ctx, s, item));
        JS_FreeValue(ctx, item);
        if (!found) {
            JS_IteratorClose(ctx, iter, FALSE);
            break;
        }
    }
fini:
    rval = found ? JS_TRUE : JS_FALSE;
exception:
    JS_FreeValue(ctx, has);
    JS_FreeValue(ctx, keys);
    JS_FreeValue(ctx, iter);
    JS_FreeValue(ctx, next);
    return rval;
}

static JSValue js_set_intersection(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    JSValue newset, item, iter, keys, has, next, rv;
    JSMapState *s, *t;
    JSMapRecord *mr;
    int64_t size;
    int done, ok;

    iter = JS_UNDEFINED;
    next = JS_UNDEFINED;
    newset = JS_UNDEFINED;
    s = JS_GetOpaque2(ctx, this_val, JS_CLASS_SET);
    if (!s)
        return JS_EXCEPTION;
    if (get_set_record(ctx, argv[0], &size, &has, &keys) < 0)
        goto exception;
    if (s->record_count > size) {
        iter = JS_Call(ctx, keys, argv[0], 0, NULL);
        if (JS_IsException(iter))
            goto exception;
        next = JS_GetProperty(ctx, iter, JS_ATOM_next);
        if (JS_IsException(next))
            goto exception;
        newset = js_map_constructor(ctx, JS_UNDEFINED, 0, NULL, MAGIC_SET);
        if (JS_IsException(newset))
            goto exception;
        t = JS_GetOpaque(newset, JS_CLASS_SET);
        for (;;) {
            item = JS_IteratorNext(ctx, iter, next, 0, NULL, &done);
            if (JS_IsException(item))
                goto exception;
            if (done) // item is JS_UNDEFINED
                break;
            item = map_normalize_key(ctx, item);
            if (!map_find_record(ctx, s, item)) {
                JS_FreeValue(ctx, item);
            } else if (map_find_record(ctx, t, item)) {
                JS_FreeValue(ctx, item); // no duplicates
            } else {
                mr = set_add_record(ctx, t, item);
                JS_FreeValue(ctx, item);
                if (!mr)
                    goto exception;
            }
        }
    } else {
        iter = js_create_map_iterator(ctx, this_val, 0, NULL, MAGIC_SET);
        if (JS_IsException(iter))
            goto exception;
        newset = js_map_constructor(ctx, JS_UNDEFINED, 0, NULL, MAGIC_SET);
        if (JS_IsException(newset))
            goto exception;
        t = JS_GetOpaque(newset, JS_CLASS_SET);
        for (;;) {
            item = js_map_iterator_next(ctx, iter, 0, NULL, &done, MAGIC_SET);
            if (JS_IsException(item))
                goto exception;
            if (done) // item is JS_UNDEFINED
                break;
            rv = JS_Call(ctx, has, argv[0], 1, (JSValueConst *)&item);
            ok = JS_ToBoolFree(ctx, rv); // returns -1 if rv is JS_EXCEPTION
            if (ok > 0) {
                item = map_normalize_key(ctx, item);
                if (map_find_record(ctx, t, item)) {
                    JS_FreeValue(ctx, item); // no duplicates
                } else {
                    mr = set_add_record(ctx, t, item);
                    JS_FreeValue(ctx, item);
                    if (!mr)
                        goto exception;
                }
            } else {
                JS_FreeValue(ctx, item);
                if (ok < 0)
                    goto exception;
            }
        }
    }
    goto fini;
exception:
    JS_FreeValue(ctx, newset);
    newset = JS_EXCEPTION;
fini:
    JS_FreeValue(ctx, has);
    JS_FreeValue(ctx, keys);
    JS_FreeValue(ctx, iter);
    JS_FreeValue(ctx, next);
    return newset;
}

static JSValue js_set_difference(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    JSValue newset, item, iter, keys, has, next, rv;
    JSMapState *s, *t;
    int64_t size;
    int done;
    int ok;

    iter = JS_UNDEFINED;
    next = JS_UNDEFINED;
    newset = JS_UNDEFINED;
    s = JS_GetOpaque2(ctx, this_val, JS_CLASS_SET);
    if (!s)
        return JS_EXCEPTION;
    if (get_set_record(ctx, argv[0], &size, &has, &keys) < 0)
        goto exception;

    newset = js_copy_set(ctx, this_val);
    if (JS_IsException(newset))
        goto exception;
    t = JS_GetOpaque(newset, JS_CLASS_SET);
    
    if (s->record_count <= size) {
        iter = js_create_map_iterator(ctx, newset, 0, NULL, MAGIC_SET);
        if (JS_IsException(iter))
            goto exception;
        for (;;) {
            item = js_map_iterator_next(ctx, iter, 0, NULL, &done, MAGIC_SET);
            if (JS_IsException(item))
                goto exception;
            if (done) // item is JS_UNDEFINED
                break;
            rv = JS_Call(ctx, has, argv[0], 1, (JSValueConst *)&item);
            ok = JS_ToBoolFree(ctx, rv); // returns -1 if rv is JS_EXCEPTION
            if (ok < 0) {
                JS_FreeValue(ctx, item);
                goto exception;
            }
            if (ok) {
                map_delete_record(ctx, t, item);
            }
            JS_FreeValue(ctx, item);
        }
    } else {
        iter = JS_Call(ctx, keys, argv[0], 0, NULL);
        if (JS_IsException(iter))
            goto exception;
        next = JS_GetProperty(ctx, iter, JS_ATOM_next);
        if (JS_IsException(next))
            goto exception;
        for (;;) {
            item = JS_IteratorNext(ctx, iter, next, 0, NULL, &done);
            if (JS_IsException(item))
                goto exception;
            if (done) // item is JS_UNDEFINED
                break;
            map_delete_record(ctx, t, item);
            JS_FreeValue(ctx, item);
        }
    }
    goto fini;
exception:
    JS_FreeValue(ctx, newset);
    newset = JS_EXCEPTION;
fini:
    JS_FreeValue(ctx, has);
    JS_FreeValue(ctx, keys);
    JS_FreeValue(ctx, iter);
    JS_FreeValue(ctx, next);
    return newset;
}

static JSValue js_set_symmetricDifference(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv)
{
    JSValue newset, item, iter, next, has, keys;
    JSMapState *s, *t;
    JSMapRecord *mr;
    int64_t size;
    int done;
    BOOL present;

    s = JS_GetOpaque2(ctx, this_val, JS_CLASS_SET);
    if (!s)
        return JS_EXCEPTION;
    if (get_set_record(ctx, argv[0], &size, &has, &keys) < 0)
        return JS_EXCEPTION;
    JS_FreeValue(ctx, has);

    next = JS_UNDEFINED;
    newset = JS_UNDEFINED;
    iter = JS_Call(ctx, keys, argv[0], 0, NULL);
    if (JS_IsException(iter))
        goto exception;
    next = JS_GetProperty(ctx, iter, JS_ATOM_next);
    if (JS_IsException(next))
        goto exception;
    newset = js_copy_set(ctx, this_val);
    if (JS_IsException(newset))
        goto exception;
    t = JS_GetOpaque(newset, JS_CLASS_SET);
    for (;;) {
        item = JS_IteratorNext(ctx, iter, next, 0, NULL, &done);
        if (JS_IsException(item))
            goto exception;
        if (done) // item is JS_UNDEFINED
            break;
        // note the subtlety here: due to mutating iterators, it's
        // possible for keys to disappear during iteration; test262
        // still expects us to maintain insertion order though, so
        // we first check |this|, then |new|; |new| is a copy of |this|
        // - if item exists in |this|, delete (if it exists) from |new|
        // - if item misses in |this| and |new|, add to |new|
        // - if item exists in |new| but misses in |this|, *don't* add it,
        //   mutating iterator erased it
        item = map_normalize_key(ctx, item);
        present = (NULL != map_find_record(ctx, s, item));
        mr = map_find_record(ctx, t, item);
        if (present) {
            map_delete_record(ctx, t, item);
            JS_FreeValue(ctx, item);
        } else if (mr) {
            JS_FreeValue(ctx, item);
        } else {
            mr = set_add_record(ctx, t, item);
            JS_FreeValue(ctx, item);
            if (!mr)
                goto exception;
        }
    }
    goto fini;
exception:
    JS_FreeValue(ctx, newset);
    newset = JS_EXCEPTION;
fini:
    JS_FreeValue(ctx, next);
    JS_FreeValue(ctx, iter);
    JS_FreeValue(ctx, keys);
    return newset;
}

static JSValue js_set_union(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    JSValue newset, item, iter, next, has, keys, rv;
    JSMapState *s;
    int64_t size;
    int done;

    s = JS_GetOpaque2(ctx, this_val, JS_CLASS_SET);
    if (!s)
        return JS_EXCEPTION;
    if (get_set_record(ctx, argv[0], &size, &has, &keys) < 0)
        return JS_EXCEPTION;
    JS_FreeValue(ctx, has);

    next = JS_UNDEFINED;
    newset = JS_UNDEFINED;
    iter = JS_Call(ctx, keys, argv[0], 0, NULL);
    if (JS_IsException(iter))
        goto exception;
    next = JS_GetProperty(ctx, iter, JS_ATOM_next);
    if (JS_IsException(next))
        goto exception;

    newset = js_copy_set(ctx, this_val);
    if (JS_IsException(newset))
        goto exception;

    for (;;) {
        item = JS_IteratorNext(ctx, iter, next, 0, NULL, &done);
        if (JS_IsException(item))
            goto exception;
        if (done) // item is JS_UNDEFINED
            break;
        rv = js_map_set(ctx, newset, 1, (JSValueConst *)&item, MAGIC_SET);
        JS_FreeValue(ctx, item);
        if (JS_IsException(rv))
            goto exception;
        JS_FreeValue(ctx, rv);
    }
    goto fini;
exception:
    JS_FreeValue(ctx, newset);
    newset = JS_EXCEPTION;
fini:
    JS_FreeValue(ctx, next);
    JS_FreeValue(ctx, iter);
    JS_FreeValue(ctx, keys);
    return newset;
}

static const JSCFunctionListEntry js_map_funcs[] = {
    JS_CFUNC_MAGIC_DEF("groupBy", 2, js_object_groupBy, 1 ),
    JS_CGETSET_DEF("[Symbol.species]", js_get_this, NULL ),
};

static const JSCFunctionListEntry js_map_proto_funcs[] = {
    JS_CFUNC_MAGIC_DEF("set", 2, js_map_set, 0 ),
    JS_CFUNC_MAGIC_DEF("get", 1, js_map_get, 0 ),
    JS_CFUNC_MAGIC_DEF("getOrInsert", 2, js_map_getOrInsert,
                       (JS_CLASS_MAP << 1) | /*computed*/FALSE ),
    JS_CFUNC_MAGIC_DEF("getOrInsertComputed", 2, js_map_getOrInsert,
                       (JS_CLASS_MAP << 1) | /*computed*/TRUE ),
    JS_CFUNC_MAGIC_DEF("has", 1, js_map_has, 0 ),
    JS_CFUNC_MAGIC_DEF("delete", 1, js_map_delete, 0 ),
    JS_CFUNC_MAGIC_DEF("clear", 0, js_map_clear, 0 ),
    JS_CGETSET_MAGIC_DEF("size", js_map_get_size, NULL, 0),
    JS_CFUNC_MAGIC_DEF("forEach", 1, js_map_forEach, 0 ),
    JS_CFUNC_MAGIC_DEF("values", 0, js_create_map_iterator, (JS_ITERATOR_KIND_VALUE << 2) | 0 ),
    JS_CFUNC_MAGIC_DEF("keys", 0, js_create_map_iterator, (JS_ITERATOR_KIND_KEY << 2) | 0 ),
    JS_CFUNC_MAGIC_DEF("entries", 0, js_create_map_iterator, (JS_ITERATOR_KIND_KEY_AND_VALUE << 2) | 0 ),
    JS_ALIAS_DEF("[Symbol.iterator]", "entries" ),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "Map", JS_PROP_CONFIGURABLE ),
};

static const JSCFunctionListEntry js_map_iterator_proto_funcs[] = {
    JS_ITERATOR_NEXT_DEF("next", 0, js_map_iterator_next, 0 ),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "Map Iterator", JS_PROP_CONFIGURABLE ),
};

static const JSCFunctionListEntry js_set_proto_funcs[] = {
    JS_CFUNC_MAGIC_DEF("add", 1, js_map_set, MAGIC_SET ),
    JS_CFUNC_MAGIC_DEF("has", 1, js_map_has, MAGIC_SET ),
    JS_CFUNC_MAGIC_DEF("delete", 1, js_map_delete, MAGIC_SET ),
    JS_CFUNC_MAGIC_DEF("clear", 0, js_map_clear, MAGIC_SET ),
    JS_CGETSET_MAGIC_DEF("size", js_map_get_size, NULL, MAGIC_SET ),
    JS_CFUNC_MAGIC_DEF("forEach", 1, js_map_forEach, MAGIC_SET ),
    JS_CFUNC_DEF("isDisjointFrom", 1, js_set_isDisjointFrom ),
    JS_CFUNC_DEF("isSubsetOf", 1, js_set_isSubsetOf ),
    JS_CFUNC_DEF("isSupersetOf", 1, js_set_isSupersetOf ),
    JS_CFUNC_DEF("intersection", 1, js_set_intersection ),
    JS_CFUNC_DEF("difference", 1, js_set_difference ),
    JS_CFUNC_DEF("symmetricDifference", 1, js_set_symmetricDifference ),
    JS_CFUNC_DEF("union", 1, js_set_union ),
    JS_CFUNC_MAGIC_DEF("values", 0, js_create_map_iterator, (JS_ITERATOR_KIND_KEY << 2) | MAGIC_SET ),
    JS_ALIAS_DEF("keys", "values" ),
    JS_ALIAS_DEF("[Symbol.iterator]", "values" ),
    JS_CFUNC_MAGIC_DEF("entries", 0, js_create_map_iterator, (JS_ITERATOR_KIND_KEY_AND_VALUE << 2) | MAGIC_SET ),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "Set", JS_PROP_CONFIGURABLE ),
};

static const JSCFunctionListEntry js_set_iterator_proto_funcs[] = {
    JS_ITERATOR_NEXT_DEF("next", 0, js_map_iterator_next, MAGIC_SET ),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "Set Iterator", JS_PROP_CONFIGURABLE ),
};

static const JSCFunctionListEntry js_weak_map_proto_funcs[] = {
    JS_CFUNC_MAGIC_DEF("set", 2, js_map_set, MAGIC_WEAK ),
    JS_CFUNC_MAGIC_DEF("get", 1, js_map_get, MAGIC_WEAK ),
    JS_CFUNC_MAGIC_DEF("getOrInsert", 2, js_map_getOrInsert,
                       (JS_CLASS_WEAKMAP << 1) | /*computed*/FALSE ),
    JS_CFUNC_MAGIC_DEF("getOrInsertComputed", 2, js_map_getOrInsert,
                       (JS_CLASS_WEAKMAP << 1) | /*computed*/TRUE ),
    JS_CFUNC_MAGIC_DEF("has", 1, js_map_has, MAGIC_WEAK ),
    JS_CFUNC_MAGIC_DEF("delete", 1, js_map_delete, MAGIC_WEAK ),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "WeakMap", JS_PROP_CONFIGURABLE ),
};

static const JSCFunctionListEntry js_weak_set_proto_funcs[] = {
    JS_CFUNC_MAGIC_DEF("add", 1, js_map_set, MAGIC_SET | MAGIC_WEAK ),
    JS_CFUNC_MAGIC_DEF("has", 1, js_map_has, MAGIC_SET | MAGIC_WEAK ),
    JS_CFUNC_MAGIC_DEF("delete", 1, js_map_delete, MAGIC_SET | MAGIC_WEAK ),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "WeakSet", JS_PROP_CONFIGURABLE ),
};

static const JSCFunctionListEntry * const js_map_proto_funcs_ptr[6] = {
    js_map_proto_funcs,
    js_set_proto_funcs,
    js_weak_map_proto_funcs,
    js_weak_set_proto_funcs,
    js_map_iterator_proto_funcs,
    js_set_iterator_proto_funcs,
};

static const uint8_t js_map_proto_funcs_count[6] = {
    countof(js_map_proto_funcs),
    countof(js_set_proto_funcs),
    countof(js_weak_map_proto_funcs),
    countof(js_weak_set_proto_funcs),
    countof(js_map_iterator_proto_funcs),
    countof(js_set_iterator_proto_funcs),
};

int JS_AddIntrinsicMapSet(JSContext *ctx)
{
    int i;
    JSValue obj1;
    char buf[ATOM_GET_STR_BUF_SIZE];

    for(i = 0; i < 4; i++) {
        JSCFunctionType ft;
        const char *name = JS_AtomGetStr(ctx, buf, sizeof(buf),
                                         JS_ATOM_Map + i);
        ft.constructor_magic = js_map_constructor;
        obj1 = JS_NewCConstructor(ctx, JS_CLASS_MAP + i, name,
                                  ft.generic, 0, JS_CFUNC_constructor_magic, i,
                                  JS_UNDEFINED,
                                  js_map_funcs, i < 2 ? countof(js_map_funcs) : 0,
                                  js_map_proto_funcs_ptr[i], js_map_proto_funcs_count[i],
                                  0);
        if (JS_IsException(obj1))
            return -1;
        JS_FreeValue(ctx, obj1);
    }

    for(i = 0; i < 2; i++) {
        ctx->class_proto[JS_CLASS_MAP_ITERATOR + i] =
            JS_NewObjectProtoList(ctx, ctx->class_proto[JS_CLASS_ITERATOR], 
                                  js_map_proto_funcs_ptr[i + 4],
                                  js_map_proto_funcs_count[i + 4]);
        if (JS_IsException(ctx->class_proto[JS_CLASS_MAP_ITERATOR + i]))
            return -1;
    }
    return 0;
}

/* Generator */
static const JSCFunctionListEntry js_generator_function_proto_funcs[] = {
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "GeneratorFunction", JS_PROP_CONFIGURABLE),
};

static const JSCFunctionListEntry js_generator_proto_funcs[] = {
    JS_ITERATOR_NEXT_DEF("next", 1, js_generator_next, GEN_MAGIC_NEXT ),
    JS_ITERATOR_NEXT_DEF("return", 1, js_generator_next, GEN_MAGIC_RETURN ),
    JS_ITERATOR_NEXT_DEF("throw", 1, js_generator_next, GEN_MAGIC_THROW ),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "Generator", JS_PROP_CONFIGURABLE),
};

