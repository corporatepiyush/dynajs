/* JSON */

static int json_parse_expect(JSParseState *s, int tok)
{
    if (s->token.val != tok) {
        /* XXX: dump token correctly in all cases */
        return js_parse_error(s, "expecting '%c'", tok);
    }
    return json_next_token(s);
}


typedef struct {
    int count;
    uint32_t hash_size;
    struct JSONParseRecordEntry *entries;
    uint32_t *hash_table;
} JSONParseRecordObject;
    
typedef struct JSONParseRecord {
    JSValue value;
    union {
        JSONParseRecordObject obj;
        struct {
            int count;
            struct JSONParseRecord *elements;
        } array;
        struct {
            uint32_t source_pos;
            uint32_t source_len;
        } primitive;
    } u;
} JSONParseRecord;

typedef struct JSONParseRecordEntry {
    JSAtom atom;
    uint32_t hash_next;
    JSONParseRecord parse_record;
} JSONParseRecordEntry;

static void json_parse_record_init_obj(JSContext *ctx, JSONParseRecord *pr, JSValueConst val)
{
    pr->value = JS_DupValue(ctx, val);
    pr->u.obj.count = 0;
    pr->u.obj.entries = NULL;
    pr->u.obj.hash_table = NULL;
    pr->u.obj.hash_size = 0;
}

static void json_parse_record_init_array(JSContext *ctx, JSONParseRecord *pr, JSValueConst val)
{
    pr->value = JS_DupValue(ctx, val);
    pr->u.array.count = 0;
    pr->u.array.elements = NULL;
}

static void json_parse_record_init_primitive(JSContext *ctx, JSONParseRecord *pr, JSValueConst val,
                                             uint32_t source_pos, uint32_t source_len)
{
    pr->value = JS_DupValue(ctx, val);
    pr->u.primitive.source_pos = source_pos;
    pr->u.primitive.source_len = source_len;
}

static int json_parse_record_resize_hash(JSContext *ctx, JSONParseRecordObject *po, uint32_t new_hash_size)
{
    uint32_t i, h, *new_hash_table;
    JSONParseRecordEntry *e;

    new_hash_table = js_malloc(ctx, sizeof(new_hash_table[0]) * new_hash_size);
    if (!new_hash_table)
        return -1;
    js_free(ctx, po->hash_table);
    po->hash_table = new_hash_table;
    po->hash_size = new_hash_size;

    for(i = 0; i < po->hash_size; i++) {
        po->hash_table[i] = -1;
    }
    for(i = 0; i < po->count; i++) {
        e = &po->entries[i];
        h = e->atom & (po->hash_size - 1);
        e->hash_next = po->hash_table[h];
        po->hash_table[h] = i;
    }
    return 0;
}

static JSONParseRecord *json_parse_record_add(JSContext *ctx, JSONParseRecord *pr, JSAtom key, int *psize)
{
    JSONParseRecordObject *po = &pr->u.obj;
    JSONParseRecordEntry *e;
    JSONParseRecord *pr1;
    uint32_t h;
    
    if (js_resize_array(ctx, (void **)&po->entries, sizeof(po->entries[0]),
                        psize, po->count + 1)) {
        return NULL;
    }
    /* don't use a hash table when the number of entries is small */
    if (po->count >= 8 && (po->count + 1) > po->hash_size) {
        int hash_bits = 32 - clz32(po->count);
        if (json_parse_record_resize_hash(ctx, po, 1 << hash_bits))
            return NULL;
    }

    e = &po->entries[po->count++];
    e->atom = JS_DupAtom(ctx, key);
    pr1 = &e->parse_record;
    pr1->value = JS_UNDEFINED;
    if (po->hash_size != 0) {
        h = key & (po->hash_size - 1);
        e->hash_next = po->hash_table[h];
        po->hash_table[h] = po->count - 1;
    }
    return pr1;
}

static JSONParseRecord *json_parse_record_find(JSONParseRecord *pr, JSAtom key)
{
    JSONParseRecordObject *po = &pr->u.obj;
    JSONParseRecordEntry *e;
    uint32_t h, i;
    
    if (po->hash_size == 0) {
        for(i = 0; i < po->count; i++) {
            if (po->entries[i].atom == key)
                return &po->entries[i].parse_record;
        }
    } else {
        h = key & (po->hash_size - 1);
        i = po->hash_table[h];
        while (i != -1) {
            e = &po->entries[i];
            if (e->atom == key)
                return &e->parse_record;
            i = e->hash_next;
        }
    }
    return NULL;
}

static void json_free_parse_record(JSContext *ctx, JSONParseRecord *pr)
{
    int i;
    if (!pr)
        return;
    if (JS_IsObject(pr->value)) {
        if (JS_IsArray(ctx, pr->value)) {
            for(i = 0; i < pr->u.array.count; i++) {
                json_free_parse_record(ctx, &pr->u.array.elements[i]);
            }
            js_free(ctx, pr->u.array.elements);
        } else {
            for(i = 0; i < pr->u.obj.count; i++) {
                JS_FreeAtom(ctx, pr->u.obj.entries[i].atom);
                json_free_parse_record(ctx, &pr->u.obj.entries[i].parse_record);
            }
            js_free(ctx, pr->u.obj.entries);
            js_free(ctx, pr->u.obj.hash_table);
        }
    }
    JS_FreeValue(ctx, pr->value);
    pr->value = JS_UNDEFINED; /* fail safe */
}

/* 'pr' can be NULL */
static JSValue json_parse_value(JSParseState *s, JSONParseRecord *pr)
{
    JSContext *ctx = s->ctx;
    JSValue val = JS_NULL;
    int ret;

    if (js_check_stack_overflow(ctx->rt, 0)) {
        JS_ThrowStackOverflow(ctx);
        return JS_EXCEPTION;
    }

    if (pr) {
        pr->value = JS_UNDEFINED;
    }
    
    switch(s->token.val) {
    case '{':
        {
            JSValue prop_val;
            JSAtom prop_name;
            JSONParseRecord *pr1;
            int pr_size;
            
            if (json_next_token(s))
                goto fail;
            val = JS_NewObject(ctx);
            if (JS_IsException(val))
                goto fail;
            if (pr) {
                json_parse_record_init_obj(ctx, pr, val);
                pr_size = 0;
            }
            if (s->token.val != '}') {
                for(;;) {
                    if (s->token.val == TOK_STRING) {
                        prop_name = JS_ValueToAtom(ctx, s->token.u.str.str);
                        if (prop_name == JS_ATOM_NULL)
                            goto fail;
                    } else if (s->ext_json && s->token.val == TOK_IDENT) {
                        prop_name = JS_DupAtom(ctx, s->token.u.ident.atom);
                    } else {
                        js_parse_error(s, "expecting property name");
                        goto fail;
                    }
                    if (json_next_token(s))
                        goto fail1;
                    if (json_parse_expect(s, ':'))
                        goto fail1;
                    if (pr) {
                        pr1 = json_parse_record_add(ctx, pr, prop_name, &pr_size);
                        if (!pr1)
                            goto fail1;
                    } else {
                        pr1 = NULL;
                    }
                    prop_val = json_parse_value(s, pr1);
                    if (JS_IsException(prop_val)) {
                    fail1:
                        JS_FreeAtom(ctx, prop_name);
                        goto fail;
                    }
                    ret = JS_DefinePropertyValue(ctx, val, prop_name,
                                                 prop_val, JS_PROP_C_W_E);
                    JS_FreeAtom(ctx, prop_name);
                    if (ret < 0)
                        goto fail;

                    if (s->token.val != ',')
                        break;
                    if (json_next_token(s))
                        goto fail;
                    if (s->ext_json && s->token.val == '}')
                        break;
                }
            }
            if (json_parse_expect(s, '}'))
                goto fail;
        }
        break;
    case '[':
        {
            JSValue el;
            JSONParseRecord *pr1;
            int pr_size;
            
            if (json_next_token(s))
                goto fail;
            val = JS_NewArray(ctx);
            if (JS_IsException(val))
                goto fail;
            if (pr) {
                json_parse_record_init_array(ctx, pr, val);
                pr_size = 0;
            }
            if (s->token.val != ']') {
                for(;;) {
                    if (pr) {
                        if (js_resize_array(ctx, (void **)&pr->u.array.elements, sizeof(pr->u.array.elements[0]),
                                            &pr_size, pr->u.array.count + 1))
                            goto fail;
                        pr1 = &pr->u.array.elements[pr->u.array.count++];
                        pr1->value = JS_UNDEFINED;
                    } else {
                        pr1 = NULL;
                    }
                    el = json_parse_value(s, pr1);
                    if (JS_IsException(el))
                        goto fail;
                    /* `val` is the fresh fast array created above and JSON
                       parsing runs no user code (the reviver runs after the
                       whole tree exists), so elements append straight into
                       the fast-array slots; indices are sequential from 0 by
                       construction. */
                    ret = add_fast_array_element(ctx, JS_VALUE_GET_OBJ(val),
                                                 el, JS_PROP_THROW);
                    if (ret < 0)
                        goto fail;
                    if (s->token.val != ',')
                        break;
                    if (json_next_token(s))
                        goto fail;
                    if (s->ext_json && s->token.val == ']')
                        break;
                }
            }
            if (json_parse_expect(s, ']'))
                goto fail;
        }
        break;
    case TOK_STRING:
        val = JS_DupValue(ctx, s->token.u.str.str);
        if (pr) {
            json_parse_record_init_primitive(ctx, pr, val, 
                                             s->token.ptr - s->buf_start,
                                             s->buf_ptr - s->token.ptr);
        }
        if (json_next_token(s))
            goto fail;
        break;
    case TOK_NUMBER:
        val = s->token.u.num.val;
        if (pr) {
            json_parse_record_init_primitive(ctx, pr, val, 
                                             s->token.ptr - s->buf_start,
                                             s->buf_ptr - s->token.ptr);
        }
        if (json_next_token(s))
            goto fail;
        break;
    case TOK_IDENT:
        if (s->token.u.ident.atom == JS_ATOM_false ||
            s->token.u.ident.atom == JS_ATOM_true) {
            val = JS_NewBool(ctx, s->token.u.ident.atom == JS_ATOM_true);
            if (pr) {
                json_parse_record_init_primitive(ctx, pr, val, 
                                                 s->token.ptr - s->buf_start,
                                                 s->buf_ptr - s->token.ptr);
            }
        } else if (s->token.u.ident.atom == JS_ATOM_null) {
            val = JS_NULL;
            if (pr) {
                json_parse_record_init_primitive(ctx, pr, val, 
                                                 s->token.ptr - s->buf_start,
                                                 s->buf_ptr - s->token.ptr);
            }
        } else if (s->token.u.ident.atom == JS_ATOM_NaN && s->ext_json) {
            /* Note: json5 identifier handling is ambiguous e.g. is 
               '{ NaN: 1 }' a valid JSON5 production ? */ 
            val = JS_NewFloat64(s->ctx, NAN);
        } else if (s->token.u.ident.atom == JS_ATOM_Infinity && s->ext_json) {
            val = JS_NewFloat64(s->ctx, INFINITY);
        } else {
            goto def_token;
        }
        if (json_next_token(s))
            goto fail;
        break;
    default:
    def_token:
        if (s->token.val == TOK_EOF) {
            js_parse_error(s, "Unexpected end of JSON input");
        } else {
            js_parse_error(s, "unexpected token: '%.*s'",
                           (int)(s->buf_ptr - s->token.ptr), s->token.ptr);
        }
        goto fail;
    }
    return val;
 fail:
    json_free_parse_record(ctx, pr);
    JS_FreeValue(ctx, val);
    return JS_EXCEPTION;
}

JSValue JS_ParseJSON3(JSContext *ctx, const char *buf, size_t buf_len,
                      const char *filename, int flags, JSONParseRecord *pr)
{
    JSParseState s1, *s = &s1;
    JSValue val = JS_UNDEFINED;

    js_parse_init(ctx, s, buf, buf_len, filename);
    s->ext_json = ((flags & JS_PARSE_JSON_EXT) != 0);
    if (json_next_token(s))
        goto fail;
    val = json_parse_value(s, pr);
    if (JS_IsException(val))
        goto fail;
    if (s->token.val != TOK_EOF) {
        if (js_parse_error(s, "unexpected data at the end")) {
            json_free_parse_record(ctx, pr);
            goto fail;
        }
    }
    return val;
 fail:
    JS_FreeValue(ctx, val);
    free_token(s, &s->token);
    return JS_EXCEPTION;
}

JSValue JS_ParseJSON2(JSContext *ctx, const char *buf, size_t buf_len,
                      const char *filename, int flags)
{
    return JS_ParseJSON3(ctx, buf, buf_len, filename, flags, NULL);
}

JSValue JS_ParseJSON(JSContext *ctx, const char *buf, size_t buf_len,
                     const char *filename)
{
    return JS_ParseJSON3(ctx, buf, buf_len, filename, 0, NULL);
}

/* if pr != NULL, then pr->value = holder by construction */
static JSValue internalize_json_property(JSContext *ctx, JSValueConst holder,
                                         JSAtom name, JSValueConst reviver,
                                         const char *text_str, JSONParseRecord *pr)
{
    JSValue val, new_el, name_val, res, context;
    JSValueConst args[3];
    int ret, is_array;
    uint32_t i, len = 0;
    JSAtom prop;
    JSPropertyEnum *atoms = NULL;

    if (js_check_stack_overflow(ctx->rt, 0)) {
        return JS_ThrowStackOverflow(ctx);
    }

    val = JS_GetProperty(ctx, holder, name);
    if (JS_IsException(val))
        return val;

    if (pr) {
        if (JS_IsArray(ctx, pr->value)) {
            if (__JS_AtomIsTaggedInt(name)) {
                uint32_t idx = __JS_AtomToUInt32(name);
                if (idx < pr->u.array.count) {
                    pr = &pr->u.array.elements[idx];
                } else {
                    pr = NULL;
                }
            }
        } else {
            pr = json_parse_record_find(pr, name);
        }
        if (pr && !js_same_value(ctx, pr->value, val)) {
            pr = NULL;
        }
    }

    context = JS_NewObject(ctx);
    if (JS_IsException(context))
        goto fail;
    
    if (JS_IsObject(val)) {
        is_array = JS_IsArray(ctx, val);
        if (is_array < 0)
            goto fail;
        if (is_array) {
            if (js_get_length32(ctx, &len, val))
                goto fail;
        } else {
            ret = JS_GetOwnPropertyNamesInternal(ctx, &atoms, &len, JS_VALUE_GET_OBJ(val), JS_GPN_ENUM_ONLY | JS_GPN_STRING_MASK);
            if (ret < 0)
                goto fail;
        }
        for(i = 0; i < len; i++) {
            if (is_array) {
                prop = JS_NewAtomUInt32(ctx, i);
                if (prop == JS_ATOM_NULL)
                    goto fail;
            } else {
                prop = JS_DupAtom(ctx, atoms[i].atom);
            }
            new_el = internalize_json_property(ctx, val, prop, reviver, text_str, pr);
            if (JS_IsException(new_el)) {
                JS_FreeAtom(ctx, prop);
                goto fail;
            }
            if (JS_IsUndefined(new_el)) {
                ret = JS_DeleteProperty(ctx, val, prop, 0);
            } else {
                ret = JS_DefinePropertyValue(ctx, val, prop, new_el, JS_PROP_C_W_E);
            }
            JS_FreeAtom(ctx, prop);
            if (ret < 0)
                goto fail;
        }
    } else {
        if (pr) {
            new_el = JS_NewStringLen(ctx, text_str + pr->u.primitive.source_pos,
                                     pr->u.primitive.source_len);
            if (JS_IsException(new_el))
                goto fail;
            if (JS_DefinePropertyValue(ctx, context, JS_ATOM_source, new_el, JS_PROP_C_W_E) < 0)
                goto fail;
        }
    }
    JS_FreePropertyEnum(ctx, atoms, len);
    atoms = NULL;
    name_val = JS_AtomToValue(ctx, name);
    if (JS_IsException(name_val))
        goto fail;
    args[0] = name_val;
    args[1] = val;
    args[2] = context;
    res = JS_Call(ctx, reviver, holder, 3, args);
    JS_FreeValue(ctx, name_val);
    JS_FreeValue(ctx, val);
    JS_FreeValue(ctx, context);
    return res;
 fail:
    JS_FreePropertyEnum(ctx, atoms, len);
    JS_FreeValue(ctx, context);
    JS_FreeValue(ctx, val);
    return JS_EXCEPTION;
}

static JSValue js_json_parse(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    JSValue obj;
    const char *str;
    size_t len;
    
    str = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!str)
        return JS_EXCEPTION;
    if (argc > 1 && JS_IsFunction(ctx, argv[1])) {
        JSONParseRecord pr_s, *pr = &pr_s, *pr1;
        JSValue root;
        JSValueConst reviver;
        int size;
        
        reviver = argv[1];
        root = JS_NewObject(ctx);
        if (JS_IsException(root))
            goto fail;
        json_parse_record_init_obj(ctx, pr, root);
        size = 0;
        pr1 = json_parse_record_add(ctx, pr, JS_ATOM_empty_string, &size);
        if (!pr1)
            goto fail1;

        obj = JS_ParseJSON3(ctx, str, len, "<input>", 0, pr1);
        if (JS_IsException(obj))
            goto fail1;
        
        if (JS_DefinePropertyValue(ctx, root, JS_ATOM_empty_string, obj,
                                   JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, obj);
        fail1:
            json_free_parse_record(ctx, pr);
            JS_FreeValue(ctx, root);
            goto fail;
        }
        
        obj = internalize_json_property(ctx, root, JS_ATOM_empty_string,
                                        reviver, str, pr);
        json_free_parse_record(ctx, pr);
        JS_FreeValue(ctx, root);
    } else {
        obj = JS_ParseJSON3(ctx, str, len, "<input>", 0, NULL);
    }
    JS_FreeCString(ctx, str);
    return obj;
 fail:
    JS_FreeCString(ctx, str);
    return JS_EXCEPTION;
}

static JSValue js_json_isRawJSON(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    JSValueConst obj = argv[0];
    if (JS_VALUE_GET_TAG(obj) == JS_TAG_OBJECT) {
        JSObject *p = JS_VALUE_GET_OBJ(obj);
        return JS_NewBool(ctx, p->class_id == JS_CLASS_RAWJSON);
    } else {
        return JS_FALSE;
    }
}

static BOOL is_valid_raw_json_char(int c)
{
    return ((c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') ||
            c == '-' || 
            c == '"');
}

static JSValue js_json_rawJSON(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    JSValue str, res, obj;
    JSString *p;
    str = JS_ToString(ctx, argv[0]);
    if (JS_IsException(str))
        return str;
    p = JS_VALUE_GET_STRING(str);
    if (p->len == 0 ||
        !is_valid_raw_json_char(string_get(p, 0)) ||
        !is_valid_raw_json_char(string_get(p, p->len - 1))) {
        goto syntax_error;
    }
    res = js_json_parse(ctx, JS_UNDEFINED, 1, (JSValueConst *)&str);
    if (JS_IsException(res)) {
    syntax_error:
        JS_ThrowSyntaxError(ctx, "invalid rawJSON string");
        goto fail;
    }
    JS_FreeValue(ctx, res);
    
    obj = JS_NewObjectProtoClass(ctx, JS_NULL, JS_CLASS_RAWJSON);
    if (JS_IsException(obj))
        goto fail;
    if (JS_DefinePropertyValue(ctx, obj, JS_ATOM_rawJSON, str, JS_PROP_ENUMERABLE) < 0) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    JS_PreventExtensions(ctx, obj);
    return obj;
 fail:
    JS_FreeValue(ctx, str);
    return JS_EXCEPTION;
}


/* JSON.stringify cycle-detection set.
   The serializer must reject a value that is an ancestor of the node being
   emitted (a circular reference). The traversal is a strict DFS, so this is a
   stack: an object is pushed on entry and popped on exit, and "is it already
   on the stack" is the cycle test. For objects SameValueZero is pointer
   identity, so this is an identity set of JSObject*.

   entries[] holds the live ancestors in push order (top == entries[count-1]),
   accelerated by a chained hash index once nesting passes a small threshold
   (mirrors json_parse_record_*). A push prepends to its bucket chain and the
   DFS always pops the most-recently-pushed object, so the object being popped
   is always at the head of its bucket -> pop is O(1) with no scan; a rehash on
   grow re-links in push order to preserve that invariant. All ops are O(1)
   amortized, replacing the old O(n*depth) js_array_includes linear scan. */
typedef struct JSONStackEntry {
    JSObject *obj;
    uint32_t hash_next; /* next entries[] index in the same bucket, or -1 */
} JSONStackEntry;

typedef struct JSONCycleStack {
    JSONStackEntry *entries; /* live ancestors, push order; top == [count-1] */
    uint32_t *hash_table;    /* hash_size buckets, each an entries[] idx or -1 */
    int count;
    int size;                /* allocated capacity of entries */
    uint32_t hash_size;      /* bucket count (power of two), 0 = no hash yet */
} JSONCycleStack;

static void json_cycle_stack_init(JSONCycleStack *cs)
{
    cs->entries = NULL;
    cs->hash_table = NULL;
    cs->count = 0;
    cs->size = 0;
    cs->hash_size = 0;
}

static void json_cycle_stack_free(JSContext *ctx, JSONCycleStack *cs)
{
    js_free(ctx, cs->entries);
    js_free(ctx, cs->hash_table);
    cs->entries = NULL;
    cs->hash_table = NULL;
}

/* splitmix64 finalizer: full avalanche independent of pointer alignment, so
   masking the low bits for a power-of-two table is sound. */
static uint32_t json_ptr_hash(JSObject *p)
{
    uint64_t x = (uint64_t)(uintptr_t)p;
    x = (x ^ (x >> 30)) * (uint64_t)0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * (uint64_t)0x94d049bb133111ebULL;
    x = x ^ (x >> 31);
    return (uint32_t)x;
}

static int json_cycle_stack_rehash(JSContext *ctx, JSONCycleStack *cs,
                                   uint32_t new_hash_size)
{
    uint32_t *new_table, h, mask;
    int i;

    new_table = js_malloc(ctx, sizeof(new_table[0]) * new_hash_size);
    if (!new_table)
        return -1;
    js_free(ctx, cs->hash_table);
    cs->hash_table = new_table;
    cs->hash_size = new_hash_size;
    mask = new_hash_size - 1;
    for (i = 0; i < (int)new_hash_size; i++)
        cs->hash_table[i] = -1;
    /* re-link in push order so the newest entry of each bucket lands at the
       chain head (keeps pop() O(1)) */
    for (i = 0; i < cs->count; i++) {
        h = json_ptr_hash(cs->entries[i].obj) & mask;
        cs->entries[i].hash_next = cs->hash_table[h];
        cs->hash_table[h] = i;
    }
    return 0;
}

static int json_cycle_stack_push(JSContext *ctx, JSONCycleStack *cs, JSObject *p)
{
    int idx;
    uint32_t h;

    if (js_resize_array(ctx, (void **)&cs->entries, sizeof(cs->entries[0]),
                        &cs->size, cs->count + 1))
        return -1;
    /* below the threshold a linear scan is cheaper than a hash */
    if (cs->count >= 8 && (uint32_t)(cs->count + 1) > cs->hash_size) {
        int hash_bits = 32 - clz32(cs->count);
        if (json_cycle_stack_rehash(ctx, cs, 1u << hash_bits))
            return -1;
    }
    idx = cs->count;
    cs->entries[idx].obj = p;
    if (cs->hash_size != 0) {
        h = json_ptr_hash(p) & (cs->hash_size - 1);
        cs->entries[idx].hash_next = cs->hash_table[h];
        cs->hash_table[h] = idx;
    }
    cs->count++;
    return 0;
}

static void json_cycle_stack_pop(JSONCycleStack *cs)
{
    uint32_t h;

    cs->count--;
    /* the popped entry (DFS top) is the newest in its bucket, hence the chain
       head, so removal needs no scan */
    if (cs->hash_size != 0) {
        h = json_ptr_hash(cs->entries[cs->count].obj) & (cs->hash_size - 1);
        cs->hash_table[h] = cs->entries[cs->count].hash_next;
    }
}

static BOOL json_cycle_stack_has(const JSONCycleStack *cs, JSObject *p)
{
    uint32_t i;
    int k;

    if (cs->hash_size == 0) {
        for (k = 0; k < cs->count; k++) {
            if (cs->entries[k].obj == p)
                return TRUE;
        }
    } else {
        i = cs->hash_table[json_ptr_hash(p) & (cs->hash_size - 1)];
        while (i != -1) {
            if (cs->entries[i].obj == p)
                return TRUE;
            i = cs->entries[i].hash_next;
        }
    }
    return FALSE;
}

/* JSON.stringify replacer-array de-duplication set.
   An array replacer yields a first-occurrence-ordered list of unique property
   names. Names are interned to atoms; interning is injective on string
   content, so atom identity equals SameValueZero on the resulting strings.
   An open-addressed atom set replaces the old O(n^2) js_array_includes scan.
   The set owns one reference per stored atom. */
typedef struct JSONAtomSet {
    JSAtom *atoms;  /* open-addressed; JS_ATOM_NULL (0) marks an empty slot */
    uint32_t size;  /* slot count (power of two), 0 = unallocated */
    uint32_t count; /* number of stored atoms */
} JSONAtomSet;

static void json_atom_set_init(JSONAtomSet *s)
{
    s->atoms = NULL;
    s->size = 0;
    s->count = 0;
}

static void json_atom_set_free(JSContext *ctx, JSONAtomSet *s)
{
    uint32_t i;
    if (s->atoms) {
        for (i = 0; i < s->size; i++) {
            if (s->atoms[i] != JS_ATOM_NULL)
                JS_FreeAtom(ctx, s->atoms[i]);
        }
        js_free(ctx, s->atoms);
        s->atoms = NULL;
    }
    s->size = 0;
    s->count = 0;
}

static uint32_t json_atom_hash(JSAtom atom)
{
    uint32_t h = atom;
    h = (h ^ (h >> 16)) * 0x45d9f3bu;
    h = (h ^ (h >> 16)) * 0x45d9f3bu;
    return h ^ (h >> 16);
}

static int json_atom_set_grow(JSContext *ctx, JSONAtomSet *s, uint32_t new_size)
{
    JSAtom *new_atoms;
    uint32_t i, j, mask = new_size - 1;

    new_atoms = js_malloc(ctx, sizeof(new_atoms[0]) * new_size);
    if (!new_atoms)
        return -1;
    for (i = 0; i < new_size; i++)
        new_atoms[i] = JS_ATOM_NULL;
    for (i = 0; i < s->size; i++) {
        JSAtom a = s->atoms[i];
        if (a != JS_ATOM_NULL) {
            j = json_atom_hash(a) & mask;
            while (new_atoms[j] != JS_ATOM_NULL)
                j = (j + 1) & mask;
            new_atoms[j] = a;
        }
    }
    js_free(ctx, s->atoms);
    s->atoms = new_atoms;
    s->size = new_size;
    return 0;
}

/* returns 1 if newly added (set takes ownership of the atom reference), 0 if
   already present (caller keeps ownership), -1 on error */
static int json_atom_set_add(JSContext *ctx, JSONAtomSet *s, JSAtom atom)
{
    uint32_t j, mask;

    /* keep load factor <= 1/2 for short probe chains */
    if ((s->count + 1) * 2 > s->size) {
        if (json_atom_set_grow(ctx, s, s->size ? s->size * 2 : 8))
            return -1;
    }
    mask = s->size - 1;
    j = json_atom_hash(atom) & mask;
    while (s->atoms[j] != JS_ATOM_NULL) {
        if (s->atoms[j] == atom)
            return 0; /* duplicate */
        j = (j + 1) & mask;
    }
    s->atoms[j] = atom;
    s->count++;
    return 1;
}

typedef struct JSONStringifyContext {
    JSValueConst replacer_func;
    JSONCycleStack stack;
    JSValue property_list;
    JSValue gap;
    JSValue empty;
    StringBuffer *b;
} JSONStringifyContext;

static const char json_hex_digits[17] = "0123456789abcdef";

/* Length of the run at p that needs no escaping, i.e. up to the first byte with
   c < 0x20 || c == '"' || c == '\\'. Bytes >= 0x80 in a narrow (Latin-1) JSString
   are neither control chars nor surrogates, so they belong to the clean run. */
static force_inline int json_clean_run8(const uint8_t *p, int len)
{
    int i = 0;

    while (i + 8 <= len) {
        uint64_t v;
        memcpy(&v, p + i, 8);
        if (swar_has_less(v, 0x20) | swar_has_byte(v, '"') | swar_has_byte(v, '\\'))
            break;      /* a hit somewhere in these 8; let the tail loop place it */
        i += 8;
    }
    while (i < len) {
        uint8_t c = p[i];
        if (c < 0x20 || c == '"' || c == '\\')
            break;
        i++;
    }
    return i;
}

/* Emit the escape for a byte that json_clean_run8() stopped on.
   force_inline and two putc8 calls for the 2-byte form on purpose: a string
   escaping every other character (JSON-in-JSON, quoted paths) spends all its
   time here, and routing that through an out-of-line helper plus a 2-byte
   string_buffer_write8 measured 10% SLOWER than the per-character loop this
   replaced. Only the 6-byte \\u form is worth a write8. */
static force_inline int json_put_escape8(StringBuffer *b, uint32_t c)
{
    char buf[6];

    switch(c) {
    case '\t': c = 't'; goto quote;
    case '\r': c = 'r'; goto quote;
    case '\n': c = 'n'; goto quote;
    case '\b': c = 'b'; goto quote;
    case '\f': c = 'f'; goto quote;
    case '\"':
    case '\\':
    quote:
        if (string_buffer_putc8(b, '\\'))
            return -1;
        return string_buffer_putc8(b, c);
    default:
        /* c < 0x20 */
        buf[0] = '\\';
        buf[1] = 'u';
        buf[2] = '0';
        buf[3] = '0';
        buf[4] = json_hex_digits[(c >> 4) & 0xf];
        buf[5] = json_hex_digits[c & 0xf];
        return string_buffer_write8(b, (const uint8_t *)buf, 6);
    }
}

static int JS_ToQuotedString(JSContext *ctx, StringBuffer *b, JSValueConst val1)
{
    JSValue val;
    JSString *p;
    int i;
    uint32_t c;
    char buf[16];

    val = JS_ToStringCheckObject(ctx, val1);
    if (JS_IsException(val))
        return -1;
    p = JS_VALUE_GET_STRING(val);

    if (string_buffer_putc8(b, '\"'))
        goto fail;

    if (!p->is_wide_char) {
        /* Narrow strings are the overwhelmingly common case and usually contain
           no escapes at all; copy each clean run in bulk instead of walking it
           one character at a time through string_getc()/string_buffer_putc(). */
        const uint8_t *s8 = p->u.str8;
        int len = p->len, j;
        i = 0;
        while (i < len) {
            j = i + json_clean_run8(s8 + i, len - i);
            if (j > i) {
                if (string_buffer_write8(b, s8 + i, j - i))
                    goto fail;
                i = j;
            }
            if (i >= len)
                break;
            if (json_put_escape8(b, s8[i]))
                goto fail;
            i++;
        }
        if (string_buffer_putc8(b, '\"'))
            goto fail;
        JS_FreeValue(ctx, val);
        return 0;
    }

    for(i = 0; i < p->len; ) {
        /* Bulk-copy the clean run first, mirroring what the narrow path above
         * already does. string_getc is an out-of-line call, so without this
         * every code point of every non-Latin1 string -- all CJK, most emoji,
         * any accented text that pushed the string wide -- paid a call where
         * the narrow path pays one memcpy per run.
         *
         * The run stops at ANY unit in the surrogate range, not just a lone
         * one. That is conservative in the safe direction: string_getc folds a
         * valid pair into c > 0xFFFF and only escapes LONE surrogates, so
         * leaving pairs on the per-character path keeps the output
         * byte-identical while the scan handles everything else. */
        {
            uint32_t j = i;
            while (j < p->len) {
                uint32_t u = p->u.str16[j];
                if (u < 32 || u == '"' || u == '\\' ||
                    (u >= 0xD800 && u <= 0xDFFF))
                    break;
                j++;
            }
            if (j > i) {
                if (string_buffer_write16(b, p->u.str16 + i, j - i))
                    goto fail;
                i = j;
                if (i >= p->len)
                    break;
            }
        }
        c = string_getc(p, &i);
        switch(c) {
        case '\t':
            c = 't';
            goto quote;
        case '\r':
            c = 'r';
            goto quote;
        case '\n':
            c = 'n';
            goto quote;
        case '\b':
            c = 'b';
            goto quote;
        case '\f':
            c = 'f';
            goto quote;
        case '\"':
        case '\\':
        quote:
            if (string_buffer_putc8(b, '\\'))
                goto fail;
            if (string_buffer_putc8(b, c))
                goto fail;
            break;
        default:
            if (c < 32 || is_surrogate(c)) {
                /* "\\u%04x" without the snprintf: c <= 0xffff here */
                buf[0] = '\\';
                buf[1] = 'u';
                buf[2] = json_hex_digits[(c >> 12) & 0xf];
                buf[3] = json_hex_digits[(c >> 8) & 0xf];
                buf[4] = json_hex_digits[(c >> 4) & 0xf];
                buf[5] = json_hex_digits[c & 0xf];
                if (string_buffer_write8(b, (const uint8_t *)buf, 6))
                    goto fail;
            } else {
                if (string_buffer_putc(b, c))
                    goto fail;
            }
            break;
        }
    }
    if (string_buffer_putc8(b, '\"'))
        goto fail;
    JS_FreeValue(ctx, val);
    return 0;
 fail:
    JS_FreeValue(ctx, val);
    return -1;
}

static int JS_ToQuotedStringFree(JSContext *ctx, StringBuffer *b, JSValue val) {
    int ret = JS_ToQuotedString(ctx, b, val);
    JS_FreeValue(ctx, val);
    return ret;
}

static JSValue js_json_check(JSContext *ctx, JSONStringifyContext *jsc,
                             JSValueConst holder, JSValue val, JSValueConst key)
{
    JSValue v;
    JSValueConst args[2];

    /* check for object.toJSON method */
    /* ECMA specifies this is done only for Object and BigInt */
    if (JS_IsObject(val) || JS_IsBigInt(ctx, val)) {
        JSValue f = JS_GetProperty(ctx, val, JS_ATOM_toJSON);
        if (JS_IsException(f))
            goto exception;
        if (JS_IsFunction(ctx, f)) {
            v = JS_CallFree(ctx, f, val, 1, &key);
            JS_FreeValue(ctx, val);
            val = v;
            if (JS_IsException(val))
                goto exception;
        } else {
            JS_FreeValue(ctx, f);
        }
    }

    if (!JS_IsUndefined(jsc->replacer_func)) {
        args[0] = key;
        args[1] = val;
        v = JS_Call(ctx, jsc->replacer_func, holder, 2, args);
        JS_FreeValue(ctx, val);
        val = v;
        if (JS_IsException(val))
            goto exception;
    }

    switch (JS_VALUE_GET_NORM_TAG(val)) {
    case JS_TAG_OBJECT:
        if (JS_IsFunction(ctx, val))
            break;
    case JS_TAG_STRING:
    case JS_TAG_STRING_ROPE:
    case JS_TAG_INT:
    case JS_TAG_FLOAT64:
    case JS_TAG_BOOL:
    case JS_TAG_NULL:
    case JS_TAG_SHORT_BIG_INT:
    case JS_TAG_BIG_INT:
    case JS_TAG_EXCEPTION:
        return val;
    default:
        break;
    }
    JS_FreeValue(ctx, val);
    return JS_UNDEFINED;

exception:
    JS_FreeValue(ctx, val);
    return JS_EXCEPTION;
}

static int js_json_to_str(JSContext *ctx, JSONStringifyContext *jsc,
                          JSValueConst holder, JSValue val,
                          JSValueConst indent)
{
    JSValue indent1, sep, sep1, tab, v, prop;
    JSObject *p;
    int64_t i, len;
    int cl, ret;
    BOOL has_content, has_gap;

    indent1 = JS_UNDEFINED;
    sep = JS_UNDEFINED;
    sep1 = JS_UNDEFINED;
    tab = JS_UNDEFINED;
    prop = JS_UNDEFINED;

    if (js_check_stack_overflow(ctx->rt, 0)) {
        JS_ThrowStackOverflow(ctx);
        goto exception;
    }

    if (JS_IsObject(val)) {
        p = JS_VALUE_GET_OBJ(val);
        cl = p->class_id;
        if (cl == JS_CLASS_STRING) {
            val = JS_ToStringFree(ctx, val);
            if (JS_IsException(val))
                goto exception;
            goto concat_primitive;
        } else if (cl == JS_CLASS_NUMBER) {
            val = JS_ToNumberFree(ctx, val);
            if (JS_IsException(val))
                goto exception;
            goto concat_primitive;
        } else if (cl == JS_CLASS_BOOLEAN || cl == JS_CLASS_BIG_INT) {
            /* This will thow the same error as for the primitive object */
            set_value(ctx, &val, JS_DupValue(ctx, p->u.object_data));
            goto concat_primitive;
        } else if (cl == JS_CLASS_RAWJSON) {
            JSValue val1;
            val1 = JS_GetProperty(ctx, val, JS_ATOM_rawJSON);
            if (JS_IsException(val1))
                goto exception;
            JS_FreeValue(ctx, val);
            val = val1;
            goto concat_value;
        }
        if (json_cycle_stack_has(&jsc->stack, p)) {
            JS_ThrowTypeError(ctx, "circular reference");
            goto exception;
        }
        /* has_gap decides everything separator-related. In the compact case
           (the overwhelming default) the old code still concatenated two empty
           strings into indent1 per OBJECT NODE and pushed an empty `sep`
           through string_buffer_concat_value per ELEMENT -- a call per element
           that appends zero bytes. */
        has_gap = !JS_IsEmptyString(jsc->gap);
        if (has_gap) {
            indent1 = JS_ConcatString(ctx, JS_DupValue(ctx, indent), JS_DupValue(ctx, jsc->gap));
            if (JS_IsException(indent1))
                goto exception;
            sep = JS_ConcatString3(ctx, "\n", JS_DupValue(ctx, indent1), "");
            if (JS_IsException(sep))
                goto exception;
            sep1 = js_new_string8(ctx, " ");
            if (JS_IsException(sep1))
                goto exception;
        } else {
            indent1 = JS_DupValue(ctx, jsc->empty);
            sep = JS_DupValue(ctx, jsc->empty);
            sep1 = JS_DupValue(ctx, jsc->empty);
        }
        if (json_cycle_stack_push(ctx, &jsc->stack, p))
            goto exception;
        ret = JS_IsArray(ctx, val);
        if (ret < 0)
            goto exception;
        if (ret) {
            if (js_get_length64(ctx, &len, val))
                goto exception;
            string_buffer_putc8(jsc->b, '[');
            for(i = 0; i < len; i++) {
                JSValue *arrp;
                uint32_t acount;
                if (i > 0)
                    string_buffer_putc8(jsc->b, ',');
                if (has_gap)
                    string_buffer_concat_value(jsc->b, sep);
                /* Fast-array element read: a dup of the slot instead of the
                   generic indexed-property machinery. Re-validated on EVERY
                   iteration because the recursion below runs user code
                   (toJSON, replacer, getters on nested values) that can
                   shrink or de-fast the array and realloc its slots -- a
                   cached pointer would dangle. On any mismatch this falls
                   back to the generic get, which also covers holes
                   (fast arrays have none by definition). */
                if (js_get_fast_array(ctx, val, &arrp, &acount) &&
                    (uint64_t)i < acount) {
                    v = JS_DupValue(ctx, arrp[i]);
                } else {
                    v = JS_GetPropertyInt64(ctx, val, i);
                    if (JS_IsException(v))
                        goto exception;
                }
                /* Built ONLY if something will read it. js_json_check touches
                 * `key` on exactly two paths: an object or BigInt carrying a
                 * callable toJSON, and a replacer function. For a plain array
                 * neither fires, and this was one malloc plus one free per
                 * element -- comparable to the cost of emitting the element.
                 * Creating a string has no observable side effect, so
                 * deferring it past the property get (which already ran)
                 * changes no ordering test262 can see. */
                if (JS_IsObject(v) || JS_IsBigInt(ctx, v) ||
                    !JS_IsUndefined(jsc->replacer_func)) {
                    prop = JS_ToStringFree(ctx, JS_NewInt64(ctx, i));
                    if (JS_IsException(prop))
                        goto exception;
                }
                v = js_json_check(ctx, jsc, val, v, prop);
                JS_FreeValue(ctx, prop);
                prop = JS_UNDEFINED;
                if (JS_IsException(v))
                    goto exception;
                if (JS_IsUndefined(v))
                    v = JS_NULL;
                if (js_json_to_str(ctx, jsc, val, v, indent1))
                    goto exception;
            }
            if (len > 0 && !JS_IsEmptyString(jsc->gap)) {
                string_buffer_putc8(jsc->b, '\n');
                string_buffer_concat_value(jsc->b, indent);
            }
            string_buffer_putc8(jsc->b, ']');
        } else {
            /* Plain-object fast path: enumerate own enumerable STRING keys as
               ATOMS. The generic path materialises a JS array of key STRINGS
               (js_object_keys) and then, per property, reads the string back
               out of that array and converts it to an atom inside
               JS_GetPropertyValue -- an intern-table round trip per property.
               For an ordinary object the atom list IS the source of truth;
               JS_GetProperty(atom) skips the conversion entirely and
               JS_AtomToValue is a refcount bump on the interned string.
               Key SNAPSHOT semantics are identical (both paths list keys
               eagerly, then Get each -- a key deleted mid-serialisation reads
               undefined and is skipped either way). Gated to non-exotic plain
               objects with no replacer array; everything else keeps the
               generic path. */
            if (JS_IsUndefined(jsc->property_list) &&
                p->class_id == JS_CLASS_OBJECT && !p->is_exotic) {
                JSPropertyEnum *ptab;
                uint32_t plen, pi;

                if (JS_GetOwnPropertyNames(ctx, &ptab, &plen, val,
                                           JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY))
                    goto exception;
                string_buffer_putc8(jsc->b, '{');
                has_content = FALSE;
                for (pi = 0; pi < plen; pi++) {
                    JS_FreeValue(ctx, prop);
                    prop = JS_UNDEFINED;
                    v = JS_GetProperty(ctx, val, ptab[pi].atom);
                    if (JS_IsException(v)) {
                        JS_FreePropertyEnum(ctx, ptab, plen);
                        goto exception;
                    }
                    /* the key STRING is needed by js_json_check only when a
                       toJSON/replacer can observe it, and by the quoting step
                       only when the value is actually emitted */
                    if (JS_IsObject(v) || JS_IsBigInt(ctx, v) ||
                        !JS_IsUndefined(jsc->replacer_func)) {
                        prop = JS_AtomToValue(ctx, ptab[pi].atom);
                        if (JS_IsException(prop)) {
                            JS_FreeValue(ctx, v);
                            JS_FreePropertyEnum(ctx, ptab, plen);
                            goto exception;
                        }
                    }
                    v = js_json_check(ctx, jsc, val, v, prop);
                    if (JS_IsException(v)) {
                        JS_FreePropertyEnum(ctx, ptab, plen);
                        goto exception;
                    }
                    if (!JS_IsUndefined(v)) {
                        if (has_content)
                            string_buffer_putc8(jsc->b, ',');
                        if (has_gap)
                            string_buffer_concat_value(jsc->b, sep);
                        if (JS_IsUndefined(prop)) {
                            prop = JS_AtomToValue(ctx, ptab[pi].atom);
                            if (JS_IsException(prop)) {
                                JS_FreeValue(ctx, v);
                                JS_FreePropertyEnum(ctx, ptab, plen);
                                goto exception;
                            }
                        }
                        if (JS_ToQuotedString(ctx, jsc->b, prop)) {
                            JS_FreeValue(ctx, v);
                            JS_FreePropertyEnum(ctx, ptab, plen);
                            goto exception;
                        }
                        string_buffer_putc8(jsc->b, ':');
                        if (has_gap)
                            string_buffer_concat_value(jsc->b, sep1);
                        if (js_json_to_str(ctx, jsc, val, v, indent1)) {
                            JS_FreePropertyEnum(ctx, ptab, plen);
                            goto exception;
                        }
                        has_content = TRUE;
                    }
                }
                JS_FreePropertyEnum(ctx, ptab, plen);
                goto object_body_done;
            }
            if (!JS_IsUndefined(jsc->property_list))
                tab = JS_DupValue(ctx, jsc->property_list);
            else
                tab = js_object_keys(ctx, JS_UNDEFINED, 1, (JSValueConst *)&val, JS_ITERATOR_KIND_KEY);
            if (JS_IsException(tab))
                goto exception;
            if (js_get_length64(ctx, &len, tab))
                goto exception;
            string_buffer_putc8(jsc->b, '{');
            has_content = FALSE;
            for(i = 0; i < len; i++) {
                JS_FreeValue(ctx, prop);
                prop = JS_GetPropertyInt64(ctx, tab, i);
                if (JS_IsException(prop))
                    goto exception;
                v = JS_GetPropertyValue(ctx, val, JS_DupValue(ctx, prop));
                if (JS_IsException(v))
                    goto exception;
                v = js_json_check(ctx, jsc, val, v, prop);
                if (JS_IsException(v))
                    goto exception;
                if (!JS_IsUndefined(v)) {
                    if (has_content)
                        string_buffer_putc8(jsc->b, ',');
                    if (has_gap)
                        string_buffer_concat_value(jsc->b, sep);
                    if (JS_ToQuotedString(ctx, jsc->b, prop)) {
                        JS_FreeValue(ctx, v);
                        goto exception;
                    }
                    string_buffer_putc8(jsc->b, ':');
                    if (has_gap)
                        string_buffer_concat_value(jsc->b, sep1);
                    if (js_json_to_str(ctx, jsc, val, v, indent1))
                        goto exception;
                    has_content = TRUE;
                }
            }
        object_body_done:
            if (has_content && !JS_IsEmptyString(jsc->gap)) {
                string_buffer_putc8(jsc->b, '\n');
                string_buffer_concat_value(jsc->b, indent);
            }
            string_buffer_putc8(jsc->b, '}');
        }
        json_cycle_stack_pop(&jsc->stack);
        JS_FreeValue(ctx, val);
        JS_FreeValue(ctx, tab);
        JS_FreeValue(ctx, sep);
        JS_FreeValue(ctx, sep1);
        JS_FreeValue(ctx, indent1);
        JS_FreeValue(ctx, prop);
        return 0;
    }
 concat_primitive:
    switch (JS_VALUE_GET_NORM_TAG(val)) {
    case JS_TAG_STRING:
    case JS_TAG_STRING_ROPE:
        return JS_ToQuotedStringFree(ctx, jsc->b, val);
    case JS_TAG_FLOAT64:
        {
            /* same as the INT case: js_dtoa with the same flags ToString uses
               (radix 10, FORMAT_FREE), into a stack buffer, no JSString. */
            double dnum = JS_VALUE_GET_FLOAT64(val);
            char dbuf[64];
            int dlen;
            JSDTOATempMem dtoa_mem;
            if (!isfinite(dnum)) {
                val = JS_NULL;
                goto concat_value;
            }
            dlen = js_dtoa(dbuf, dnum, 10, 0, JS_DTOA_FORMAT_FREE, &dtoa_mem);
            return string_buffer_write8(jsc->b, (const uint8_t *)dbuf, dlen);
        }
    case JS_TAG_INT:
        {
            /* string_buffer_concat_value_free on a number tag detours through
               JS_ToStringFree: an intermediate JSString is allocated, copied
               into the buffer and freed -- per number. For numeric payloads
               that allocation IS the serializer profile. i32toa into a stack
               buffer is exactly what JS_ToStringInternal itself uses for
               JS_TAG_INT, so the bytes are identical by construction. */
            char nbuf[16];
            size_t nlen = i32toa(nbuf, JS_VALUE_GET_INT(val));
            return string_buffer_write8(jsc->b, (const uint8_t *)nbuf, nlen);
        }
    case JS_TAG_BOOL:
    case JS_TAG_NULL:
    concat_value:
        return string_buffer_concat_value_free(jsc->b, val);
    case JS_TAG_SHORT_BIG_INT:
    case JS_TAG_BIG_INT:
        /* reject big numbers: use toJSON method to override */
        JS_ThrowTypeError(ctx, "Do not know how to serialize a BigInt");
        goto exception;
    default:
        JS_FreeValue(ctx, val);
        return 0;
    }

exception:
    JS_FreeValue(ctx, val);
    JS_FreeValue(ctx, tab);
    JS_FreeValue(ctx, sep);
    JS_FreeValue(ctx, sep1);
    JS_FreeValue(ctx, indent1);
    JS_FreeValue(ctx, prop);
    return -1;
}

JSValue JS_JSONStringify(JSContext *ctx, JSValueConst obj,
                         JSValueConst replacer, JSValueConst space0)
{
    StringBuffer b_s;
    JSONStringifyContext jsc_s, *jsc = &jsc_s;
    JSONAtomSet key_set;
    JSValue val, v, space, ret, wrapper;
    int res;
    int64_t i, j, n;

    jsc->replacer_func = JS_UNDEFINED;
    json_cycle_stack_init(&jsc->stack);
    jsc->property_list = JS_UNDEFINED;
    jsc->gap = JS_UNDEFINED;
    jsc->b = &b_s;
    jsc->empty = JS_AtomToString(ctx, JS_ATOM_empty_string);
    json_atom_set_init(&key_set);
    ret = JS_UNDEFINED;
    wrapper = JS_UNDEFINED;

    string_buffer_init(ctx, jsc->b, 0);
    if (JS_IsFunction(ctx, replacer)) {
        jsc->replacer_func = replacer;
    } else {
        res = JS_IsArray(ctx, replacer);
        if (res < 0)
            goto exception;
        if (res) {
            /* XXX: enumeration is not fully correct */
            jsc->property_list = JS_NewArray(ctx);
            if (JS_IsException(jsc->property_list))
                goto exception;
            if (js_get_length64(ctx, &n, replacer))
                goto exception;
            for (i = j = 0; i < n; i++) {
                JSAtom key_atom;
                int added;
                v = JS_GetPropertyInt64(ctx, replacer, i);
                if (JS_IsException(v))
                    goto exception;
                if (JS_IsObject(v)) {
                    JSObject *p = JS_VALUE_GET_OBJ(v);
                    if (p->class_id == JS_CLASS_STRING ||
                        p->class_id == JS_CLASS_NUMBER) {
                        v = JS_ToStringFree(ctx, v);
                        if (JS_IsException(v))
                            goto exception;
                    } else {
                        JS_FreeValue(ctx, v);
                        continue;
                    }
                } else if (JS_IsNumber(v)) {
                    v = JS_ToStringFree(ctx, v);
                    if (JS_IsException(v))
                        goto exception;
                } else if (!JS_IsString(v)) {
                    JS_FreeValue(ctx, v);
                    continue;
                }
                /* dedup by interned name: atom identity == SameValueZero on
                   the string values, in O(1) amortized */
                key_atom = JS_ValueToAtom(ctx, v);
                if (key_atom == JS_ATOM_NULL) {
                    JS_FreeValue(ctx, v);
                    goto exception;
                }
                added = json_atom_set_add(ctx, &key_set, key_atom);
                if (added < 0) {
                    JS_FreeAtom(ctx, key_atom);
                    JS_FreeValue(ctx, v);
                    goto exception;
                }
                if (added) {
                    JS_SetPropertyInt64(ctx, jsc->property_list, j++, v);
                } else {
                    JS_FreeAtom(ctx, key_atom);
                    JS_FreeValue(ctx, v);
                }
            }
        }
    }
    space = JS_DupValue(ctx, space0);
    if (JS_IsObject(space)) {
        JSObject *p = JS_VALUE_GET_OBJ(space);
        if (p->class_id == JS_CLASS_NUMBER) {
            space = JS_ToNumberFree(ctx, space);
        } else if (p->class_id == JS_CLASS_STRING) {
            space = JS_ToStringFree(ctx, space);
        }
        if (JS_IsException(space)) {
            JS_FreeValue(ctx, space);
            goto exception;
        }
    }
    if (JS_IsNumber(space)) {
        int n;
        if (JS_ToInt32Clamp(ctx, &n, space, 0, 10, 0))
            goto exception;
        jsc->gap = js_new_string8_len(ctx, "          ", n);
    } else if (JS_IsString(space)) {
        JSString *p = JS_VALUE_GET_STRING(space);
        jsc->gap = js_sub_string(ctx, p, 0, min_int(p->len, 10));
    } else {
        jsc->gap = JS_DupValue(ctx, jsc->empty);
    }
    JS_FreeValue(ctx, space);
    if (JS_IsException(jsc->gap))
        goto exception;
    wrapper = JS_NewObject(ctx);
    if (JS_IsException(wrapper))
        goto exception;
    if (JS_DefinePropertyValue(ctx, wrapper, JS_ATOM_empty_string,
                               JS_DupValue(ctx, obj), JS_PROP_C_W_E) < 0)
        goto exception;
    val = JS_DupValue(ctx, obj);

    val = js_json_check(ctx, jsc, wrapper, val, jsc->empty);
    if (JS_IsException(val))
        goto exception;
    if (JS_IsUndefined(val)) {
        ret = JS_UNDEFINED;
        goto done1;
    }
    if (js_json_to_str(ctx, jsc, wrapper, val, jsc->empty))
        goto exception;

    ret = string_buffer_end(jsc->b);
    goto done;

exception:
    ret = JS_EXCEPTION;
done1:
    string_buffer_free(jsc->b);
done:
    JS_FreeValue(ctx, wrapper);
    JS_FreeValue(ctx, jsc->empty);
    JS_FreeValue(ctx, jsc->gap);
    JS_FreeValue(ctx, jsc->property_list);
    json_cycle_stack_free(ctx, &jsc->stack);
    json_atom_set_free(ctx, &key_set);
    return ret;
}

static JSValue js_json_stringify(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    // stringify(val, replacer, space)
    return JS_JSONStringify(ctx, argv[0], argv[1], argv[2]);
}

static const JSCFunctionListEntry js_json_funcs[] = {
    JS_CFUNC_DEF("isRawJSON", 1, js_json_isRawJSON ),
    JS_CFUNC_DEF("parse", 2, js_json_parse ),
    JS_CFUNC_DEF("rawJSON", 1, js_json_rawJSON ),
    JS_CFUNC_DEF("stringify", 3, js_json_stringify ),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "JSON", JS_PROP_CONFIGURABLE ),
};

static const JSCFunctionListEntry js_json_obj[] = {
    JS_OBJECT_DEF("JSON", js_json_funcs, countof(js_json_funcs), JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE ),
};

int JS_AddIntrinsicJSON(JSContext *ctx)
{
    /* add JSON as autoinit object */
    return JS_SetPropertyFunctionList(ctx, ctx->global_obj, js_json_obj, countof(js_json_obj));
}

