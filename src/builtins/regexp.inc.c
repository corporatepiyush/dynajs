/* RegExp */

static void js_regexp_finalizer(JSRuntime *rt, JSValue val)
{
    JSObject *p = JS_VALUE_GET_OBJ(val);
    JSRegExp *re = &p->u.regexp;
    if (re->bytecode != NULL)
        JS_FreeValueRT(rt, JS_MKPTR(JS_TAG_STRING, re->bytecode));
    if (re->pattern != NULL)
        JS_FreeValueRT(rt, JS_MKPTR(JS_TAG_STRING, re->pattern));
}

/* create a string containing the RegExp bytecode */
static JSValue js_compile_regexp(JSContext *ctx, JSValueConst pattern,
                                 JSValueConst flags)
{
    const char *str;
    int re_flags, mask;
    uint8_t *re_bytecode_buf;
    size_t i, len;
    int re_bytecode_len;
    JSValue ret;
    char error_msg[64];

    re_flags = 0;
    if (!JS_IsUndefined(flags)) {
        str = JS_ToCStringLen(ctx, &len, flags);
        if (!str)
            return JS_EXCEPTION;
        /* XXX: re_flags = LRE_FLAG_OCTAL unless strict mode? */
        for (i = 0; i < len; i++) {
            switch(str[i]) {
            case 'd':
                mask = LRE_FLAG_INDICES;
                break;
            case 'g':
                mask = LRE_FLAG_GLOBAL;
                break;
            case 'i':
                mask = LRE_FLAG_IGNORECASE;
                break;
            case 'm':
                mask = LRE_FLAG_MULTILINE;
                break;
            case 's':
                mask = LRE_FLAG_DOTALL;
                break;
            case 'u':
                mask = LRE_FLAG_UNICODE;
                break;
            case 'v':
                mask = LRE_FLAG_UNICODE_SETS;
                break;
            case 'y':
                mask = LRE_FLAG_STICKY;
                break;
            default:
                goto bad_flags;
            }
            if ((re_flags & mask) != 0) {
            bad_flags:
                JS_FreeCString(ctx, str);
                goto bad_flags1;
            }
            re_flags |= mask;
        }
        JS_FreeCString(ctx, str);
    }

    /* 'u' and 'v' cannot be both set */
    if ((re_flags & LRE_FLAG_UNICODE_SETS) && (re_flags & LRE_FLAG_UNICODE)) {
    bad_flags1:
        return JS_ThrowSyntaxError(ctx, "invalid regular expression flags");
    }
    
    str = JS_ToCStringLen2(ctx, &len, pattern, !(re_flags & (LRE_FLAG_UNICODE | LRE_FLAG_UNICODE_SETS)));
    if (!str)
        return JS_EXCEPTION;
    re_bytecode_buf = lre_compile(&re_bytecode_len, error_msg,
                                  sizeof(error_msg), str, len, re_flags, ctx);
    JS_FreeCString(ctx, str);
    if (!re_bytecode_buf) {
        JS_ThrowSyntaxError(ctx, "%s", error_msg);
        return JS_EXCEPTION;
    }

    ret = js_new_string8_len(ctx, (const char *)re_bytecode_buf, re_bytecode_len);
    js_free(ctx, re_bytecode_buf);
    return ret;
}

/* fast regexp creation */
static JSValue JS_NewRegexp(JSContext *ctx, JSValue pattern, JSValue bc)
{
    JSValue obj;
    JSProperty props[1];
    JSObject *p;
    JSRegExp *re;

    /* sanity check */
    if (unlikely(JS_VALUE_GET_TAG(bc) != JS_TAG_STRING ||
                 JS_VALUE_GET_TAG(pattern) != JS_TAG_STRING)) {
        JS_ThrowTypeError(ctx, "string expected");
        goto fail;
    }
    props[0].u.value = JS_NewInt32(ctx, 0); /* lastIndex */
    obj = JS_NewObjectFromShape(ctx, js_dup_shape(ctx->regexp_shape), JS_CLASS_REGEXP, props);
    if (JS_IsException(obj))
        goto fail;
    p = JS_VALUE_GET_OBJ(obj);
    re = &p->u.regexp;
    re->pattern = JS_VALUE_GET_STRING(pattern);
    re->bytecode = JS_VALUE_GET_STRING(bc);
    return obj;
 fail:
    JS_FreeValue(ctx, bc);
    JS_FreeValue(ctx, pattern);
    return JS_EXCEPTION;
}

/* set the RegExp fields */
static JSValue js_regexp_set_internal(JSContext *ctx,
                                      JSValue obj,
                                      JSValue pattern, JSValue bc)
{
    JSObject *p;
    JSRegExp *re;

    /* sanity check */
    if (unlikely(JS_VALUE_GET_TAG(bc) != JS_TAG_STRING ||
                 JS_VALUE_GET_TAG(pattern) != JS_TAG_STRING)) {
        JS_ThrowTypeError(ctx, "string expected");
        JS_FreeValue(ctx, obj);
        JS_FreeValue(ctx, bc);
        JS_FreeValue(ctx, pattern);
        return JS_EXCEPTION;
    }

    p = JS_VALUE_GET_OBJ(obj);
    re = &p->u.regexp;
    re->pattern = JS_VALUE_GET_STRING(pattern);
    re->bytecode = JS_VALUE_GET_STRING(bc);
    /* Note: cannot fail because the field is preallocated */
    JS_DefinePropertyValue(ctx, obj, JS_ATOM_lastIndex, JS_NewInt32(ctx, 0),
                           JS_PROP_WRITABLE);
    return obj;
}

static JSRegExp *js_get_regexp(JSContext *ctx, JSValueConst obj, BOOL throw_error)
{
    if (JS_VALUE_GET_TAG(obj) == JS_TAG_OBJECT) {
        JSObject *p = JS_VALUE_GET_OBJ(obj);
        if (p->class_id == JS_CLASS_REGEXP)
            return &p->u.regexp;
    }
    if (throw_error) {
        JS_ThrowTypeErrorInvalidClass(ctx, JS_CLASS_REGEXP);
    }
    return NULL;
}

/* return < 0 if exception or TRUE/FALSE */
static int js_is_regexp(JSContext *ctx, JSValueConst obj)
{
    JSValue m;

    if (!JS_IsObject(obj))
        return FALSE;
    m = JS_GetProperty(ctx, obj, JS_ATOM_Symbol_match);
    if (JS_IsException(m))
        return -1;
    if (!JS_IsUndefined(m))
        return JS_ToBoolFree(ctx, m);
    return js_get_regexp(ctx, obj, FALSE) != NULL;
}

static JSValue js_regexp_constructor(JSContext *ctx, JSValueConst new_target,
                                     int argc, JSValueConst *argv)
{
    JSValue pattern, flags, bc, val, obj = JS_UNDEFINED;
    JSValueConst pat, flags1;
    JSRegExp *re;
    int pat_is_regexp;

    pat = argv[0];
    flags1 = argv[1];
    pat_is_regexp = js_is_regexp(ctx, pat);
    if (pat_is_regexp < 0)
        return JS_EXCEPTION;
    if (JS_IsUndefined(new_target)) {
        /* called as a function */
        new_target = JS_GetActiveFunction(ctx);
        if (pat_is_regexp && JS_IsUndefined(flags1)) {
            JSValue ctor;
            BOOL res;
            ctor = JS_GetProperty(ctx, pat, JS_ATOM_constructor);
            if (JS_IsException(ctor))
                return ctor;
            res = js_same_value(ctx, ctor, new_target);
            JS_FreeValue(ctx, ctor);
            if (res)
                return JS_DupValue(ctx, pat);
        }
    }
    re = js_get_regexp(ctx, pat, FALSE);
    flags = JS_UNDEFINED;
    if (re) {
        pattern = JS_DupValue(ctx, JS_MKPTR(JS_TAG_STRING, re->pattern));
        if (JS_IsUndefined(flags1)) {
            bc = JS_DupValue(ctx, JS_MKPTR(JS_TAG_STRING, re->bytecode));
            obj = js_create_from_ctor(ctx, new_target, JS_CLASS_REGEXP);
            if (JS_IsException(obj))
                goto fail;
            goto no_compilation;
        } else {
            flags = JS_DupValue(ctx, flags1);
        }
    } else {
        if (pat_is_regexp) {
            pattern = JS_GetProperty(ctx, pat, JS_ATOM_source);
            if (JS_IsException(pattern))
                goto fail;
            if (JS_IsUndefined(flags1)) {
                flags = JS_GetProperty(ctx, pat, JS_ATOM_flags);
                if (JS_IsException(flags))
                    goto fail;
            } else {
                flags = JS_DupValue(ctx, flags1);
            }
        } else {
            pattern = JS_DupValue(ctx, pat);
            flags = JS_DupValue(ctx, flags1);
        }
        if (JS_IsUndefined(pattern)) {
            pattern = JS_AtomToString(ctx, JS_ATOM_empty_string);
        } else {
            val = pattern;
            pattern = JS_ToString(ctx, val);
            JS_FreeValue(ctx, val);
            if (JS_IsException(pattern))
                goto fail;
        }
    }
    obj = js_create_from_ctor(ctx, new_target, JS_CLASS_REGEXP);
    if (JS_IsException(obj))
        goto fail;
    bc = js_compile_regexp(ctx, pattern, flags);
    if (JS_IsException(bc))
        goto fail;
    JS_FreeValue(ctx, flags);
 no_compilation:
    return js_regexp_set_internal(ctx, obj, pattern, bc);
 fail:
    JS_FreeValue(ctx, pattern);
    JS_FreeValue(ctx, flags);
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
}

static JSValue js_regexp_compile(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    JSRegExp *re1, *re;
    JSValueConst pattern1, flags1;
    JSValue bc, pattern;

    re = js_get_regexp(ctx, this_val, TRUE);
    if (!re)
        return JS_EXCEPTION;
    pattern1 = argv[0];
    flags1 = argv[1];
    re1 = js_get_regexp(ctx, pattern1, FALSE);
    if (re1) {
        if (!JS_IsUndefined(flags1))
            return JS_ThrowTypeError(ctx, "flags must be undefined");
        pattern = JS_DupValue(ctx, JS_MKPTR(JS_TAG_STRING, re1->pattern));
        bc = JS_DupValue(ctx, JS_MKPTR(JS_TAG_STRING, re1->bytecode));
    } else {
        bc = JS_UNDEFINED;
        if (JS_IsUndefined(pattern1))
            pattern = JS_AtomToString(ctx, JS_ATOM_empty_string);
        else
            pattern = JS_ToString(ctx, pattern1);
        if (JS_IsException(pattern))
            goto fail;
        bc = js_compile_regexp(ctx, pattern, flags1);
        if (JS_IsException(bc))
            goto fail;
    }
    JS_FreeValue(ctx, JS_MKPTR(JS_TAG_STRING, re->pattern));
    JS_FreeValue(ctx, JS_MKPTR(JS_TAG_STRING, re->bytecode));
    re->pattern = JS_VALUE_GET_STRING(pattern);
    re->bytecode = JS_VALUE_GET_STRING(bc);
    if (JS_SetProperty(ctx, this_val, JS_ATOM_lastIndex,
                       JS_NewInt32(ctx, 0)) < 0)
        return JS_EXCEPTION;
    return JS_DupValue(ctx, this_val);
 fail:
    JS_FreeValue(ctx, pattern);
    JS_FreeValue(ctx, bc);
    return JS_EXCEPTION;
}

static JSValue js_regexp_get_source(JSContext *ctx, JSValueConst this_val)
{
    JSRegExp *re;
    JSString *p;
    StringBuffer b_s, *b = &b_s;
    int i, n, c, c2, bra;

    if (JS_VALUE_GET_TAG(this_val) != JS_TAG_OBJECT)
        return JS_ThrowTypeErrorNotAnObject(ctx);

    if (js_same_value(ctx, this_val, ctx->class_proto[JS_CLASS_REGEXP]))
        goto empty_regex;

    re = js_get_regexp(ctx, this_val, TRUE);
    if (!re)
        return JS_EXCEPTION;

    p = re->pattern;

    if (p->len == 0) {
    empty_regex:
        return js_new_string8(ctx, "(?:)");
    }
    string_buffer_init2(ctx, b, p->len, p->is_wide_char);

    /* Escape '/' and newline sequences as needed */
    bra = 0;
    for (i = 0, n = p->len; i < n;) {
        c2 = -1;
        switch (c = string_get(p, i++)) {
        case '\\':
            if (i < n)
                c2 = string_get(p, i++);
            break;
        case ']':
            bra = 0;
            break;
        case '[':
            if (!bra) {
                if (i < n && string_get(p, i) == ']')
                    c2 = string_get(p, i++);
                bra = 1;
            }
            break;
        case '\n':
            c = '\\';
            c2 = 'n';
            break;
        case '\r':
            c = '\\';
            c2 = 'r';
            break;
        case '/':
            if (!bra) {
                c = '\\';
                c2 = '/';
            }
            break;
        }
        string_buffer_putc16(b, c);
        if (c2 >= 0)
            string_buffer_putc16(b, c2);
    }
    return string_buffer_end(b);
}

static JSValue js_regexp_get_flag(JSContext *ctx, JSValueConst this_val, int mask)
{
    JSRegExp *re;
    int flags;

    if (JS_VALUE_GET_TAG(this_val) != JS_TAG_OBJECT)
        return JS_ThrowTypeErrorNotAnObject(ctx);

    re = js_get_regexp(ctx, this_val, FALSE);
    if (!re) {
        if (js_same_value(ctx, this_val, ctx->class_proto[JS_CLASS_REGEXP]))
            return JS_UNDEFINED;
        else
            return JS_ThrowTypeErrorInvalidClass(ctx, JS_CLASS_REGEXP);
    }

    flags = lre_get_flags(re->bytecode->u.str8);
    return JS_NewBool(ctx, flags & mask);
}

#define RE_FLAG_COUNT 8

static JSValue js_regexp_get_flags(JSContext *ctx, JSValueConst this_val)
{
    char str[RE_FLAG_COUNT], *p = str;
    int res, i;
    static const int flag_atom[RE_FLAG_COUNT] = {
        JS_ATOM_hasIndices,
        JS_ATOM_global,
        JS_ATOM_ignoreCase,
        JS_ATOM_multiline,
        JS_ATOM_dotAll,
        JS_ATOM_unicode,
        JS_ATOM_unicodeSets,
        JS_ATOM_sticky,
    };
    static const char flag_char[RE_FLAG_COUNT] = { 'd', 'g', 'i', 'm', 's', 'u', 'v', 'y' };
    
    if (JS_VALUE_GET_TAG(this_val) != JS_TAG_OBJECT)
        return JS_ThrowTypeErrorNotAnObject(ctx);

    for(i = 0; i < RE_FLAG_COUNT; i++) {
        res = JS_ToBoolFree(ctx, JS_GetProperty(ctx, this_val, flag_atom[i]));
        if (res < 0)
            goto exception;
        if (res)
            *p++ = flag_char[i];
    }
    return JS_NewStringLen(ctx, str, p - str);

exception:
    return JS_EXCEPTION;
}

static JSValue js_regexp_toString(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    JSValue pattern, flags;
    StringBuffer b_s, *b = &b_s;

    if (!JS_IsObject(this_val))
        return JS_ThrowTypeErrorNotAnObject(ctx);

    string_buffer_init(ctx, b, 0);
    string_buffer_putc8(b, '/');
    pattern = JS_GetProperty(ctx, this_val, JS_ATOM_source);
    if (string_buffer_concat_value_free(b, pattern))
        goto fail;
    string_buffer_putc8(b, '/');
    flags = JS_GetProperty(ctx, this_val, JS_ATOM_flags);
    if (string_buffer_concat_value_free(b, flags))
        goto fail;
    return string_buffer_end(b);

fail:
    string_buffer_free(b);
    return JS_EXCEPTION;
}

int lre_check_stack_overflow(void *opaque, size_t alloca_size)
{
    JSContext *ctx = opaque;
    return js_check_stack_overflow(ctx->rt, alloca_size);
}

int lre_check_timeout(void *opaque)
{
    JSContext *ctx = opaque;
    JSRuntime *rt = ctx->rt;
    return (rt->interrupt_handler && 
            rt->interrupt_handler(rt, rt->interrupt_opaque));
}

void *lre_realloc(void *opaque, void *ptr, size_t size)
{
    JSContext *ctx = opaque;
    /* No JS exception is raised here */
    return js_realloc_rt(ctx->rt, ptr, size);
}

static JSValue js_regexp_escape(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    JSValue str;
    StringBuffer b_s, *b = &b_s;
    JSString *p;
    uint32_t c;
    char s[16];
    int i, i0;
    
    if (!JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "not a string");
    str = JS_ToString(ctx, argv[0]); /* must call it to linearlize ropes */
    if (JS_IsException(str))
        return JS_EXCEPTION;
    p = JS_VALUE_GET_STRING(str);
    string_buffer_init2(ctx, b, 0, p->is_wide_char);
    for (i = 0; i < p->len; ) {
        i0 = i;
        c = string_getc(p, &i);
        if (c < 33) {
            if (c >= 9 && c <= 13) {
                string_buffer_putc8(b, '\\');
                string_buffer_putc8(b, "tnvfr"[c - 9]);
            } else {
                goto hex2;
            }
        } else if (c < 128) {
            if ((c >= '0' && c <= '9')
             || (c >= 'A' && c <= 'Z')
             || (c >= 'a' && c <= 'z')) {
                if (i0 == 0)
                    goto hex2;
            } else if (strchr(",-=<>#&!%:;@~'`\"", c)) {
                goto hex2;
            } else if (c != '_') {
                string_buffer_putc8(b, '\\');
            }
            string_buffer_putc8(b, c);
        } else if (c < 256) {
        hex2:
            snprintf(s, sizeof(s), "\\x%02x", c);
            string_buffer_puts8(b, s);
        } else if (is_surrogate(c) || lre_is_space(c)) {
            snprintf(s, sizeof(s), "\\u%04x", c);
            string_buffer_puts8(b, s);
        } else {
            string_buffer_putc(b, c);
        }
    }
    JS_FreeValue(ctx, str);
    return string_buffer_end(b);
}

/* this_val must be of JS_CLASS_REGEXP */
static force_inline int js_regexp_get_lastIndex(JSContext *ctx, int64_t *plast_index,
                                                JSValueConst this_val)
{
    JSObject *p = JS_VALUE_GET_OBJ(this_val);
    
    /* lastIndex is always the first property (it is not configurable) */
    if (likely(JS_VALUE_GET_TAG(p->prop[0].u.value) == JS_TAG_INT)) {
        *plast_index = max_int(JS_VALUE_GET_INT(p->prop[0].u.value), 0);
        return 0;
    } else {
        return JS_ToLengthFree(ctx, plast_index, JS_DupValue(ctx, p->prop[0].u.value));
    }
}

/* this_val must be of JS_CLASS_REGEXP */
static force_inline int js_regexp_set_lastIndex(JSContext *ctx, JSValueConst this_val,
                                                int last_index)
{
    JSObject *p = JS_VALUE_GET_OBJ(this_val);
    
    /* lastIndex is always the first property (it is not configurable) */
    if (likely(JS_VALUE_GET_TAG(p->prop[0].u.value) == JS_TAG_INT &&
               (get_shape_prop(p->shape)->flags & JS_PROP_WRITABLE))) {
        set_value(ctx, &p->prop[0].u.value, JS_NewInt32(ctx, last_index));
    } else {
        if (JS_SetProperty(ctx, this_val, JS_ATOM_lastIndex,
                           JS_NewInt32(ctx, last_index)) < 0)
            return -1;
    }
    return 0;
}

/* ---------------------------------------------------------------------------
   Standard-regexp fast paths.

   js_is_standard_regexp() proves that `exec` is the built-in C function, the
   flag getters are the built-in getters and lastIndex is a plain number data
   property. Under that proof, the only OBSERVABLE effects of the spec
   algorithms for test/match/search/split are: ToString of the arguments, the
   final value of lastIndex, and the returned values. Everything else --
   the per-match exec RESULT OBJECTS with their index/input/groups properties,
   the property reads of `flags`/`global`/`unicode`, the lastIndex property
   churn between iterations -- is internal scaffolding the caller immediately
   discards. These helpers run the same lre_exec calls in the same order with
   the same arguments and materialise only what the caller keeps.

   RE_CAPTURE_STACK: lre_get_alloc_count is capture_count*2 + register_count.
   Nearly every real pattern fits far below 64, so the per-call js_malloc in
   the old exec path was pure overhead -- measured, it is the difference
   between 156 and 500+ ops/s on a tight test() loop. */
#define RE_CAPTURE_STACK 64

typedef struct {
    uint8_t **capture;                 /* points at stack_cap or a js_malloc */
    uint8_t *stack_cap[RE_CAPTURE_STACK];
    uint8_t *heap_mark;                /* non-NULL: capture is heap */
} RECaptureBuf;

static int re_capture_init(JSContext *ctx, RECaptureBuf *cb, int alloc_count)
{
    cb->heap_mark = NULL;
    if (alloc_count <= RE_CAPTURE_STACK) {
        cb->capture = cb->stack_cap;
    } else {
        cb->capture = js_malloc(ctx, sizeof(cb->capture[0]) * alloc_count);
        if (!cb->capture)
            return -1;
        cb->heap_mark = (uint8_t *)cb->capture;
    }
    return 0;
}

static void re_capture_free(JSContext *ctx, RECaptureBuf *cb)
{
    if (cb->heap_mark)
        js_free(ctx, cb->heap_mark);
}

/* One lre_exec with the exact rc conventions of js_regexp_exec:
   1 match, 0 no-match, 2 lastIndex-beyond-length, <0 engine error. */
static int js_regexp_raw_exec(JSContext *ctx, uint8_t **capture,
                              uint8_t *re_bytecode, JSString *str,
                              int64_t last_index)
{
    if (last_index > str->len)
        return 2;
    return lre_exec(capture, re_bytecode, str->u.str8, last_index, str->len,
                    str->is_wide_char, ctx);
}

/* Throw the same errors js_regexp_exec throws for rc < 0. */
static void js_regexp_throw_exec_error(JSContext *ctx, int rc)
{
    if (rc == LRE_RET_TIMEOUT)
        JS_ThrowInterrupted(ctx);
    else
        JS_ThrowInternalError(ctx, "out of memory in regexp execution");
}

static JSValue js_regexp_exec(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    JSRegExp *re = js_get_regexp(ctx, this_val, TRUE);
    JSString *str;
    JSValue t, ret, str_val, obj, groups;
    JSValue indices, indices_groups;
    uint8_t *re_bytecode;
    uint8_t **capture, *str_buf;
    int rc, capture_count, shift, i, re_flags, alloc_count;
    int64_t last_index;
    const char *group_name_ptr;
    JSObject *p_obj;
    JSAtom group_name;
    RECaptureBuf cb;

    if (!re)
        return JS_EXCEPTION;

    str_val = JS_ToString(ctx, argv[0]);
    if (JS_IsException(str_val))
        return JS_EXCEPTION;

    ret = JS_EXCEPTION;
    obj = JS_NULL;
    groups = JS_UNDEFINED;
    indices = JS_UNDEFINED;
    indices_groups = JS_UNDEFINED;
    cb.heap_mark = NULL;
    capture = NULL;
    group_name = JS_ATOM_NULL;
    
    if (js_regexp_get_lastIndex(ctx, &last_index, this_val))
        goto fail;

    re_bytecode = re->bytecode->u.str8;
    re_flags = lre_get_flags(re_bytecode);
    if ((re_flags & (LRE_FLAG_GLOBAL | LRE_FLAG_STICKY)) == 0) {
        last_index = 0;
    }
    str = JS_VALUE_GET_STRING(str_val);
    alloc_count = lre_get_alloc_count(re_bytecode);
    if (alloc_count > 0) {
        if (re_capture_init(ctx, &cb, alloc_count))
            goto fail;
        capture = cb.capture;
    }
    capture_count = lre_get_capture_count(re_bytecode);
    shift = str->is_wide_char;
    str_buf = str->u.str8;
    if (last_index > str->len) {
        rc = 2;
    } else {
        rc = lre_exec(capture, re_bytecode,
                      str_buf, last_index, str->len,
                      shift, ctx);
    }
    if (rc != 1) {
        if (rc >= 0) {
            if (rc == 2 || (re_flags & (LRE_FLAG_GLOBAL | LRE_FLAG_STICKY))) {
                if (js_regexp_set_lastIndex(ctx, this_val, 0) < 0)
                    goto fail;
            }
        } else {
            if (rc == LRE_RET_TIMEOUT) {
                JS_ThrowInterrupted(ctx);
            } else {
                JS_ThrowInternalError(ctx, "out of memory in regexp execution");
            }
            goto fail;
        }
    } else {
        int prop_flags;
        JSProperty props[4];
        
        if (re_flags & (LRE_FLAG_GLOBAL | LRE_FLAG_STICKY)) {
            if (js_regexp_set_lastIndex(ctx, this_val,
                                        (capture[1] - str_buf) >> shift) < 0)
                goto fail;
        }
        prop_flags = JS_PROP_C_W_E | JS_PROP_THROW;
        group_name_ptr = lre_get_groupnames(re_bytecode);
        if (group_name_ptr) {
            groups = JS_NewObjectProto(ctx, JS_NULL);
            if (JS_IsException(groups))
                goto fail;
        }
        if (re_flags & LRE_FLAG_INDICES) {
            indices = JS_NewArray(ctx);
            if (JS_IsException(indices))
                goto fail;
            if (group_name_ptr) {
                indices_groups = JS_NewObjectProto(ctx, JS_NULL);
                if (JS_IsException(indices_groups))
                    goto fail;
            }
        }

        props[0].u.value = JS_NewInt32(ctx, capture_count); /* length */
        props[1].u.value = JS_NewInt32(ctx, (capture[0] - str_buf) >> shift); /* index */
        props[2].u.value = str_val; /* input */
        props[3].u.value = JS_DupValue(ctx, groups); /* groups */

        str_val = JS_UNDEFINED;
        obj = JS_NewObjectFromShape(ctx, js_dup_shape(ctx->regexp_result_shape),
                                    JS_CLASS_ARRAY, props);
        if (JS_IsException(obj))
            goto fail;

        p_obj = JS_VALUE_GET_OBJ(obj);
        if (expand_fast_array(ctx, p_obj, capture_count))
            goto fail;
        
        for(i = 0; i < capture_count; i++) {
            uint8_t **match = &capture[2 * i];
            int start = -1;
            int end = -1;
            JSValue val;

            if (group_name_ptr && i > 0) {
                if (*group_name_ptr) {
                    /* XXX: slow, should create a shape when the regexp is
                       compiled */
                    group_name = JS_NewAtom(ctx, group_name_ptr);
                    if (group_name == JS_ATOM_NULL)
                        goto fail;
                }
                group_name_ptr += strlen(group_name_ptr) + LRE_GROUP_NAME_TRAILER_LEN;
            }

            if (match[0] && match[1]) {
                start = (match[0] - str_buf) >> shift;
                end = (match[1] - str_buf) >> shift;
            }

            if (!JS_IsUndefined(indices)) {
                val = JS_UNDEFINED;
                if (start != -1) {
                    val = JS_NewArray(ctx);
                    if (JS_IsException(val))
                        goto fail;
                    if (JS_DefinePropertyValueUint32(ctx, val, 0,
                                                     JS_NewInt32(ctx, start),
                                                     prop_flags) < 0) {
                        JS_FreeValue(ctx, val);
                        goto fail;
                    }
                    if (JS_DefinePropertyValueUint32(ctx, val, 1,
                                                     JS_NewInt32(ctx, end),
                                                     prop_flags) < 0) {
                        JS_FreeValue(ctx, val);
                        goto fail;
                    }
                }
                if (group_name != JS_ATOM_NULL) {
                    /* JS_HasProperty() cannot fail here */
                    if (!JS_IsUndefined(val) ||
                        !JS_HasProperty(ctx, indices_groups, group_name)) {
                        if (JS_DefinePropertyValue(ctx, indices_groups,
                                                   group_name, JS_DupValue(ctx, val), prop_flags) < 0) {
                            JS_FreeValue(ctx, val);
                            goto fail;
                        }
                    }
                }
                if (JS_DefinePropertyValueUint32(ctx, indices, i, val,
                                                 prop_flags) < 0) {
                    goto fail;
                }
            }

            val = JS_UNDEFINED;
            if (start != -1) {
                val = js_sub_string(ctx, str, start, end);
                if (JS_IsException(val))
                    goto fail;
            }

            if (group_name != JS_ATOM_NULL) {
                /* JS_HasProperty() cannot fail here */
                if (!JS_IsUndefined(val) ||
                    !JS_HasProperty(ctx, groups, group_name)) {
                    if (JS_DefinePropertyValue(ctx, groups, group_name,
                                               JS_DupValue(ctx, val),
                                               prop_flags) < 0) {
                        JS_FreeValue(ctx, val);
                        goto fail;
                    }
                }
                JS_FreeAtom(ctx, group_name);
                group_name = JS_ATOM_NULL;
            }
            p_obj->u.array.u.values[p_obj->u.array.count++] = val;
        }

        if (!JS_IsUndefined(indices)) {
            t = indices_groups, indices_groups = JS_UNDEFINED;
            if (JS_DefinePropertyValue(ctx, indices, JS_ATOM_groups,
                                       t, prop_flags) < 0) {
                goto fail;
            }
            t = indices, indices = JS_UNDEFINED;
            if (JS_DefinePropertyValue(ctx, obj, JS_ATOM_indices,
                                       t, prop_flags) < 0) {
                goto fail;
            }
        }
    }
    ret = obj;
    obj = JS_UNDEFINED;
fail:
    JS_FreeAtom(ctx, group_name);
    JS_FreeValue(ctx, indices_groups);
    JS_FreeValue(ctx, indices);
    JS_FreeValue(ctx, str_val);
    JS_FreeValue(ctx, groups);
    JS_FreeValue(ctx, obj);
    re_capture_free(ctx, &cb);
    return ret;
}

/* XXX: add group names support */
static JSValue js_regexp_replace(JSContext *ctx, JSValueConst this_val, JSValueConst arg,
                                 JSValueConst rep_val)
{
    JSRegExp *re = js_get_regexp(ctx, this_val, TRUE);
    JSString *str;
    JSValue str_val;
    uint8_t *re_bytecode;
    int ret;
    uint8_t **capture, *str_buf;
    int capture_count, alloc_count, shift, re_flags;
    int next_src_pos, start, end;
    int64_t last_index;
    StringBuffer b_s, *b = &b_s;
    JSString *rp = JS_VALUE_GET_STRING(rep_val);
    const char *group_name_ptr;
    BOOL fullUnicode;
    RECaptureBuf cb;

    cb.heap_mark = NULL;
    if (!re)
        return JS_EXCEPTION;
    re_bytecode = re->bytecode->u.str8;
    group_name_ptr = lre_get_groupnames(re_bytecode);
    if (group_name_ptr)
        return JS_UNDEFINED; /* group names are not supported yet */
    
    string_buffer_init(ctx, b, 0);

    capture = NULL;
    str_val = JS_ToString(ctx, arg);
    if (JS_IsException(str_val))
        goto fail;
    str = JS_VALUE_GET_STRING(str_val);
    re_flags = lre_get_flags(re_bytecode);

    if (re_flags & LRE_FLAG_GLOBAL) {
        if (js_regexp_set_lastIndex(ctx, this_val, 0))
            goto fail;
    }
    if ((re_flags & (LRE_FLAG_GLOBAL | LRE_FLAG_STICKY)) == 0) {
        last_index = 0;
    } else {
        if (js_regexp_get_lastIndex(ctx, &last_index, this_val))
            goto fail;
    }
    alloc_count = lre_get_alloc_count(re_bytecode);
    if (alloc_count > 0) {
        if (re_capture_init(ctx, &cb, alloc_count))
            goto fail;
        capture = cb.capture;
    }
    capture_count = lre_get_capture_count(re_bytecode);
    fullUnicode = ((re_flags & (LRE_FLAG_UNICODE | LRE_FLAG_UNICODE_SETS)) != 0);
    shift = str->is_wide_char;
    str_buf = str->u.str8;
    next_src_pos = 0;
    for (;;) {
        if (last_index > str->len) {
            ret = 0;
        } else {
            ret = lre_exec(capture, re_bytecode,
                           str_buf, last_index, str->len, shift, ctx);
        }
        if (ret != 1) {
            if (ret >= 0) {
                if (ret == 2 || (re_flags & (LRE_FLAG_GLOBAL | LRE_FLAG_STICKY))) {
                    if (js_regexp_set_lastIndex(ctx, this_val, 0) < 0)
                        goto fail;
                }
            } else {
                if (ret == LRE_RET_TIMEOUT) {
                    JS_ThrowInterrupted(ctx);
                } else {
                    JS_ThrowInternalError(ctx, "out of memory in regexp execution");
                }
                goto fail;
            }
            break;
        }
        start = (capture[0] - str_buf) >> shift;
        end = (capture[1] - str_buf) >> shift;
        last_index = end;
        if (next_src_pos < start) {
            if (string_buffer_concat(b, str, next_src_pos, start))
                goto fail;
        }
        if (rp->len != 0) {
            if (js_string_GetSubstitution(ctx, b, JS_UNDEFINED, str, start,
                                          JS_UNDEFINED, JS_UNDEFINED, rep_val,
                                          capture, capture_count)) {
                goto fail;
            }
        }
        next_src_pos = end;
        if (!(re_flags & LRE_FLAG_GLOBAL)) {
            if (re_flags & LRE_FLAG_STICKY) {
                if (js_regexp_set_lastIndex(ctx, this_val, end) < 0)
                    goto fail;
            }
            break;
        }
        if (end == start) {
            end = string_advance_index(str, end, fullUnicode);
        }
        last_index = end;
    }
    if (string_buffer_concat(b, str, next_src_pos, str->len))
        goto fail;
    JS_FreeValue(ctx, str_val);
    re_capture_free(ctx, &cb);
    return string_buffer_end(b);
fail:
    JS_FreeValue(ctx, str_val);
    re_capture_free(ctx, &cb);
    string_buffer_free(b);
    return JS_EXCEPTION;
}

static BOOL js_is_standard_regexp(JSContext *ctx, JSValueConst obj);

static JSValue JS_RegExpExec(JSContext *ctx, JSValueConst r, JSValueConst s)
{
    JSValue method, ret;

    method = JS_GetProperty(ctx, r, JS_ATOM_exec);
    if (JS_IsException(method))
        return method;
    if (JS_IsFunction(ctx, method)) {
        ret = JS_CallFree(ctx, method, r, 1, &s);
        if (JS_IsException(ret))
            return ret;
        if (!JS_IsObject(ret) && !JS_IsNull(ret)) {
            JS_FreeValue(ctx, ret);
            return JS_ThrowTypeError(ctx, "RegExp exec method must return an object or null");
        }
        return ret;
    }
    JS_FreeValue(ctx, method);
    return js_regexp_exec(ctx, r, 1, &s);
}

static JSValue js_regexp_test(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    JSValue val;
    BOOL ret;

    /* Standard regexp: run the same match with the same lastIndex protocol as
       js_regexp_exec, but skip building the result object (array, index,
       input, groups, capture substrings) that this function would only
       null-test and free. The observable effects -- ToString of the argument,
       the thrown errors, and the final lastIndex value -- are reproduced
       exactly; js_is_standard_regexp has proven `exec` is the built-in, so no
       user code can tell the object was never made. */
    if (js_is_standard_regexp(ctx, this_val)) {
        JSRegExp *re = js_get_regexp(ctx, this_val, TRUE);
        JSValue str_val;
        JSString *str;
        uint8_t *re_bytecode;
        int rc, re_flags, alloc_count, shift;
        int64_t last_index;
        RECaptureBuf cb;

        if (!re)
            return JS_EXCEPTION;
        str_val = JS_ToString(ctx, argv[0]);
        if (JS_IsException(str_val))
            return JS_EXCEPTION;
        cb.heap_mark = NULL;
        if (js_regexp_get_lastIndex(ctx, &last_index, this_val))
            goto fast_fail;
        re_bytecode = re->bytecode->u.str8;
        re_flags = lre_get_flags(re_bytecode);
        if ((re_flags & (LRE_FLAG_GLOBAL | LRE_FLAG_STICKY)) == 0)
            last_index = 0;
        str = JS_VALUE_GET_STRING(str_val);
        alloc_count = lre_get_alloc_count(re_bytecode);
        if (alloc_count > 0 && re_capture_init(ctx, &cb, alloc_count))
            goto fast_fail;
        rc = js_regexp_raw_exec(ctx, cb.capture, re_bytecode, str, last_index);
        if (rc == 1) {
            if (re_flags & (LRE_FLAG_GLOBAL | LRE_FLAG_STICKY)) {
                shift = str->is_wide_char;
                if (js_regexp_set_lastIndex(ctx, this_val,
                        (cb.capture[1] - str->u.str8) >> shift) < 0)
                    goto fast_fail;
            }
        } else if (rc >= 0) {
            if (rc == 2 || (re_flags & (LRE_FLAG_GLOBAL | LRE_FLAG_STICKY))) {
                if (js_regexp_set_lastIndex(ctx, this_val, 0) < 0)
                    goto fast_fail;
            }
        } else {
            js_regexp_throw_exec_error(ctx, rc);
            goto fast_fail;
        }
        JS_FreeValue(ctx, str_val);
        re_capture_free(ctx, &cb);
        return JS_NewBool(ctx, rc == 1);
    fast_fail:
        JS_FreeValue(ctx, str_val);
        re_capture_free(ctx, &cb);
        return JS_EXCEPTION;
    }

    val = JS_RegExpExec(ctx, this_val, argv[0]);
    if (JS_IsException(val))
        return JS_EXCEPTION;
    ret = !JS_IsNull(val);
    JS_FreeValue(ctx, val);
    return JS_NewBool(ctx, ret);
}

static JSValue js_regexp_Symbol_match(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    // [Symbol.match](str)
    JSValueConst rx = this_val;
    JSValue A, S, flags, result, matchStr;
    int global, n, fullUnicode, isEmpty;
    JSString *p;

    if (!JS_IsObject(rx))
        return JS_ThrowTypeErrorNotAnObject(ctx);

    A = JS_UNDEFINED;
    flags = JS_UNDEFINED;
    result = JS_UNDEFINED;
    matchStr = JS_UNDEFINED;
    S = JS_ToString(ctx, argv[0]);
    if (JS_IsException(S))
        goto exception;

    flags = JS_GetProperty(ctx, rx, JS_ATOM_flags);
    if (JS_IsException(flags))
        goto exception;
    flags = JS_ToStringFree(ctx, flags);
    if (JS_IsException(flags))
        goto exception;
    p = JS_VALUE_GET_STRING(flags);

    global = (-1 != string_indexof_char(p, 'g', 0));
    if (!global) {
        A = JS_RegExpExec(ctx, rx, S);
    } else if (js_is_standard_regexp(ctx, rx)) {
        /* Global match on a standard regexp: the spec loop builds a full exec
           result object per match only to read element 0 out of it. Run the
           same lre_exec sequence directly and materialise only the matched
           substrings. lastIndex is a plain data property (proven by the
           standard check), so its intermediate values are unobservable while
           this function runs; the FINAL value the spec leaves is always 0
           (the loop only exits through a null exec, whose global path resets
           lastIndex), which is also what the initial SetProperty wrote -- on
           the error paths the property is set to the value the spec sequence
           would have left. */
        JSRegExp *re = js_get_regexp(ctx, rx, TRUE);
        uint8_t *re_bytecode;
        JSString *sp2;
        int rc, re_flags, alloc_count, shift, fu;
        int64_t last_index, prop_last;
        RECaptureBuf cb;

        if (!re)
            goto exception;
        cb.heap_mark = NULL;
        if (JS_SetProperty(ctx, rx, JS_ATOM_lastIndex, JS_NewInt32(ctx, 0)) < 0)
            goto exception;
        A = JS_NewArray(ctx);
        if (JS_IsException(A))
            goto exception;
        re_bytecode = re->bytecode->u.str8;
        re_flags = lre_get_flags(re_bytecode);
        fu = (re_flags & (LRE_FLAG_UNICODE | LRE_FLAG_UNICODE_SETS)) != 0;
        sp2 = JS_VALUE_GET_STRING(S);
        shift = sp2->is_wide_char;
        alloc_count = lre_get_alloc_count(re_bytecode);
        if (alloc_count > 0 && re_capture_init(ctx, &cb, alloc_count))
            goto match_fast_fail0;
        n = 0;
        last_index = 0;
        prop_last = 0;
        for (;;) {
            int start, end;
            rc = js_regexp_raw_exec(ctx, cb.capture, re_bytecode, sp2,
                                    last_index);
            if (rc != 1) {
                if (rc < 0) {
                    js_regexp_throw_exec_error(ctx, rc);
                    goto match_fast_fail;
                }
                break;              /* spec: null exec resets lastIndex to 0,
                                       which is the value already stored */
            }
            start = (cb.capture[0] - sp2->u.str8) >> shift;
            end = (cb.capture[1] - sp2->u.str8) >> shift;
            matchStr = js_sub_string(ctx, sp2, start, end);
            if (JS_IsException(matchStr))
                goto match_fast_fail;
            if (JS_DefinePropertyValueInt64(ctx, A, n++, matchStr,
                                            JS_PROP_C_W_E | JS_PROP_THROW) < 0) {
                matchStr = JS_UNDEFINED;
                goto match_fast_fail;
            }
            matchStr = JS_UNDEFINED;
            prop_last = end;        /* what exec's global path stored */
            if (start == end) {
                last_index = string_advance_index(sp2, end, fu);
                prop_last = last_index;
            } else {
                last_index = end;
            }
        }
        re_capture_free(ctx, &cb);
        if (n == 0) {
            JS_FreeValue(ctx, A);
            A = JS_NULL;
        }
        JS_FreeValue(ctx, result);
        JS_FreeValue(ctx, flags);
        JS_FreeValue(ctx, S);
        return A;
    match_fast_fail:
        /* leave lastIndex where the spec sequence would have: after the last
           successful iteration */
        JS_SetProperty(ctx, rx, JS_ATOM_lastIndex, JS_NewInt64(ctx, prop_last));
    match_fast_fail0:
        re_capture_free(ctx, &cb);
        goto exception;
    } else {
        fullUnicode = (string_indexof_char(p, 'u', 0) >= 0 ||
                       string_indexof_char(p, 'v', 0) >= 0);

        if (JS_SetProperty(ctx, rx, JS_ATOM_lastIndex, JS_NewInt32(ctx, 0)) < 0)
            goto exception;
        A = JS_NewArray(ctx);
        if (JS_IsException(A))
            goto exception;
        n = 0;
        for(;;) {
            JS_FreeValue(ctx, result);
            result = JS_RegExpExec(ctx, rx, S);
            if (JS_IsException(result))
                goto exception;
            if (JS_IsNull(result))
                break;
            matchStr = JS_ToStringFree(ctx, JS_GetPropertyInt64(ctx, result, 0));
            if (JS_IsException(matchStr))
                goto exception;
            isEmpty = JS_IsEmptyString(matchStr);
            if (JS_DefinePropertyValueInt64(ctx, A, n++, matchStr, JS_PROP_C_W_E | JS_PROP_THROW) < 0)
                goto exception;
            if (isEmpty) {
                int64_t thisIndex, nextIndex;
                if (JS_ToLengthFree(ctx, &thisIndex,
                                    JS_GetProperty(ctx, rx, JS_ATOM_lastIndex)) < 0)
                    goto exception;
                p = JS_VALUE_GET_STRING(S);
                nextIndex = string_advance_index(p, thisIndex, fullUnicode);
                if (JS_SetProperty(ctx, rx, JS_ATOM_lastIndex, JS_NewInt64(ctx, nextIndex)) < 0)
                    goto exception;
            }
        }
        if (n == 0) {
            JS_FreeValue(ctx, A);
            A = JS_NULL;
        }
    }
    JS_FreeValue(ctx, result);
    JS_FreeValue(ctx, flags);
    JS_FreeValue(ctx, S);
    return A;

exception:
    JS_FreeValue(ctx, A);
    JS_FreeValue(ctx, result);
    JS_FreeValue(ctx, flags);
    JS_FreeValue(ctx, S);
    return JS_EXCEPTION;
}

typedef struct JSRegExpStringIteratorData {
    JSValue iterating_regexp;
    JSValue iterated_string;
    BOOL global;
    BOOL unicode;
    BOOL done;
} JSRegExpStringIteratorData;

static void js_regexp_string_iterator_finalizer(JSRuntime *rt, JSValue val)
{
    JSObject *p = JS_VALUE_GET_OBJ(val);
    JSRegExpStringIteratorData *it = p->u.regexp_string_iterator_data;
    if (it) {
        JS_FreeValueRT(rt, it->iterating_regexp);
        JS_FreeValueRT(rt, it->iterated_string);
        js_free_rt(rt, it);
    }
}

static void js_regexp_string_iterator_mark(JSRuntime *rt, JSValueConst val,
                                           JS_MarkFunc *mark_func)
{
    JSObject *p = JS_VALUE_GET_OBJ(val);
    JSRegExpStringIteratorData *it = p->u.regexp_string_iterator_data;
    if (it) {
        JS_MarkValue(rt, it->iterating_regexp, mark_func);
        JS_MarkValue(rt, it->iterated_string, mark_func);
    }
}

static JSValue js_regexp_string_iterator_next(JSContext *ctx,
                                              JSValueConst this_val,
                                              int argc, JSValueConst *argv,
                                              BOOL *pdone, int magic)
{
    JSRegExpStringIteratorData *it;
    JSValueConst R, S;
    JSValue matchStr = JS_UNDEFINED, match = JS_UNDEFINED;
    JSString *sp;

    it = JS_GetOpaque2(ctx, this_val, JS_CLASS_REGEXP_STRING_ITERATOR);
    if (!it)
        goto exception;
    if (it->done) {
        *pdone = TRUE;
        return JS_UNDEFINED;
    }
    R = it->iterating_regexp;
    S = it->iterated_string;
    match = JS_RegExpExec(ctx, R, S);
    if (JS_IsException(match))
        goto exception;
    if (JS_IsNull(match)) {
        it->done = TRUE;
        *pdone = TRUE;
        return JS_UNDEFINED;
    } else if (it->global) {
        matchStr = JS_ToStringFree(ctx, JS_GetPropertyInt64(ctx, match, 0));
        if (JS_IsException(matchStr))
            goto exception;
        if (JS_IsEmptyString(matchStr)) {
            int64_t thisIndex, nextIndex;
            if (JS_ToLengthFree(ctx, &thisIndex,
                                JS_GetProperty(ctx, R, JS_ATOM_lastIndex)) < 0)
                goto exception;
            sp = JS_VALUE_GET_STRING(S);
            nextIndex = string_advance_index(sp, thisIndex, it->unicode);
            if (JS_SetProperty(ctx, R, JS_ATOM_lastIndex,
                               JS_NewInt64(ctx, nextIndex)) < 0)
                goto exception;
        }
        JS_FreeValue(ctx, matchStr);
    } else {
        it->done = TRUE;
    }
    *pdone = FALSE;
    return match;
 exception:
    JS_FreeValue(ctx, match);
    JS_FreeValue(ctx, matchStr);
    *pdone = FALSE;
    return JS_EXCEPTION;
}

static JSValue js_regexp_Symbol_matchAll(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv)
{
    // [Symbol.matchAll](str)
    JSValueConst R = this_val;
    JSValue S, C, flags, matcher, iter;
    JSValueConst args[2];
    JSString *strp;
    int64_t lastIndex;
    JSRegExpStringIteratorData *it;

    if (!JS_IsObject(R))
        return JS_ThrowTypeErrorNotAnObject(ctx);

    C = JS_UNDEFINED;
    flags = JS_UNDEFINED;
    matcher = JS_UNDEFINED;
    iter = JS_UNDEFINED;

    S = JS_ToString(ctx, argv[0]);
    if (JS_IsException(S))
        goto exception;
    C = JS_SpeciesConstructor(ctx, R, ctx->regexp_ctor);
    if (JS_IsException(C))
        goto exception;
    flags = JS_ToStringFree(ctx, JS_GetProperty(ctx, R, JS_ATOM_flags));
    if (JS_IsException(flags))
        goto exception;
    args[0] = R;
    args[1] = flags;
    matcher = JS_CallConstructor(ctx, C, 2, args);
    if (JS_IsException(matcher))
        goto exception;
    if (JS_ToLengthFree(ctx, &lastIndex,
                        JS_GetProperty(ctx, R, JS_ATOM_lastIndex)))
        goto exception;
    if (JS_SetProperty(ctx, matcher, JS_ATOM_lastIndex,
                       JS_NewInt64(ctx, lastIndex)) < 0)
        goto exception;

    iter = JS_NewObjectClass(ctx, JS_CLASS_REGEXP_STRING_ITERATOR);
    if (JS_IsException(iter))
        goto exception;
    it = js_malloc(ctx, sizeof(*it));
    if (!it)
        goto exception;
    it->iterating_regexp = matcher;
    it->iterated_string = S;
    strp = JS_VALUE_GET_STRING(flags);
    it->global = string_indexof_char(strp, 'g', 0) >= 0;
    it->unicode = (string_indexof_char(strp, 'u', 0) >= 0 ||
                   string_indexof_char(strp, 'v', 0) >= 0);
    it->done = FALSE;
    JS_SetOpaque(iter, it);

    JS_FreeValue(ctx, C);
    JS_FreeValue(ctx, flags);
    return iter;
 exception:
    JS_FreeValue(ctx, S);
    JS_FreeValue(ctx, C);
    JS_FreeValue(ctx, flags);
    JS_FreeValue(ctx, matcher);
    JS_FreeValue(ctx, iter);
    return JS_EXCEPTION;
}

typedef struct ValueBuffer {
    JSContext *ctx;
    JSValue *arr;
    JSValue def[4];
    int len;
    int size;
    int error_status;
} ValueBuffer;

static int value_buffer_init(JSContext *ctx, ValueBuffer *b)
{
    b->ctx = ctx;
    b->len = 0;
    b->size = 4;
    b->error_status = 0;
    b->arr = b->def;
    return 0;
}

static void value_buffer_free(ValueBuffer *b)
{
    while (b->len > 0)
        JS_FreeValue(b->ctx, b->arr[--b->len]);
    if (b->arr != b->def)
        js_free(b->ctx, b->arr);
    b->arr = b->def;
    b->size = 4;
}

static int value_buffer_append(ValueBuffer *b, JSValue val)
{
    if (b->error_status)
        return -1;

    if (b->len >= b->size) {
        int new_size = (b->len + (b->len >> 1) + 31) & ~16;
        size_t slack;
        JSValue *new_arr;

        if (b->arr == b->def) {
            new_arr = js_realloc2(b->ctx, NULL, sizeof(*b->arr) * new_size, &slack);
            if (new_arr)
                memcpy(new_arr, b->def, sizeof b->def);
        } else {
            new_arr = js_realloc2(b->ctx, b->arr, sizeof(*b->arr) * new_size, &slack);
        }
        if (!new_arr) {
            value_buffer_free(b);
            JS_FreeValue(b->ctx, val);
            b->error_status = -1;
            return -1;
        }
        new_size += slack / sizeof(*new_arr);
        b->arr = new_arr;
        b->size = new_size;
    }
    b->arr[b->len++] = val;
    return 0;
}

/* find in 'p' or its prototypes */
static JSShapeProperty *find_property_regexp(JSProperty **ppr,
                                             JSObject *p, JSAtom atom)
{
    JSShapeProperty *prs;

    for(;;) {
        prs = find_own_property(ppr, p, atom);
        if (prs)
            return prs;
        p = p->shape->proto;
        if (!p)
            return NULL;
        if (p->is_exotic)
            return NULL;
    }
}

static BOOL check_regexp_getter(JSContext *ctx,
                                JSObject *p, JSAtom atom,
                                JSCFunction *func, int magic)
{
    JSProperty *pr;
    JSShapeProperty *prs;

    prs = find_property_regexp(&pr, p, atom);
    if (!prs)
        return FALSE;
    if ((prs->flags & JS_PROP_TMASK) != JS_PROP_GETSET)
        return FALSE;
    if (!pr->u.getset.getter)
        return FALSE;
    return JS_IsCFunction(ctx, JS_MKPTR(JS_TAG_OBJECT, pr->u.getset.getter),
                          func, magic);
}

static BOOL js_is_standard_regexp(JSContext *ctx, JSValueConst obj)
{
    JSObject *p;
    JSProperty *pr;
    JSShapeProperty *prs;
    JSCFunctionType ft;
    
    if (JS_VALUE_GET_TAG(obj) != JS_TAG_OBJECT)
        return FALSE;
    p = JS_VALUE_GET_OBJ(obj);
    if (p->class_id != JS_CLASS_REGEXP)
        return FALSE;
    /* lastIndex must be a plain writable number: the fast paths below skip the
       Set(lastIndex) the spec performs, and on a NON-WRITABLE property that Set
       is required to throw a TypeError. Refusing here puts the check in the one
       function every fast path routes through. */
    prs = find_own_property(&pr, p, JS_ATOM_lastIndex);
    if (!prs)
        return FALSE;
    if ((prs->flags & (JS_PROP_TMASK | JS_PROP_WRITABLE)) != JS_PROP_WRITABLE)
        return FALSE;
    if (!JS_IsNumber(pr->u.value))
        return FALSE;

    /* check the 'exec' method. */
    prs = find_property_regexp(&pr, p, JS_ATOM_exec);
    if (!prs)
        return FALSE;
    if ((prs->flags & JS_PROP_TMASK) != JS_PROP_NORMAL)
        return FALSE;
    if (!JS_IsCFunction(ctx, pr->u.value, js_regexp_exec, 0))
        return FALSE;
    /* check the flag getters */
    ft.getter = js_regexp_get_flags;
    if (!check_regexp_getter(ctx, p, JS_ATOM_flags, ft.generic, 0))
        return FALSE;
    ft.getter_magic = js_regexp_get_flag;
    if (!check_regexp_getter(ctx, p, JS_ATOM_global, ft.generic, LRE_FLAG_GLOBAL))
        return FALSE;
    if (!check_regexp_getter(ctx, p, JS_ATOM_unicode, ft.generic, LRE_FLAG_UNICODE))
        return FALSE;
    /* XXX: need to check all accessors, need a faster way.  */
    return TRUE;
}

static JSValue js_regexp_Symbol_replace(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv)
{
    // [Symbol.replace](str, rep)
    JSValueConst rx = this_val, rep = argv[1];
    JSValueConst args[6];
    JSValue flags, str, rep_val, matched, tab, rep_str, namedCaptures, res;
    JSString *p, *sp;
    StringBuffer b_s, *b = &b_s;
    ValueBuffer v_b, *results = &v_b;
    int nextSourcePosition, n, j, functionalReplace, is_global, fullUnicode;
    uint32_t nCaptures;
    int64_t position;

    if (!JS_IsObject(rx))
        return JS_ThrowTypeErrorNotAnObject(ctx);

    string_buffer_init(ctx, b, 0);
    value_buffer_init(ctx, results);

    rep_val = JS_UNDEFINED;
    matched = JS_UNDEFINED;
    tab = JS_UNDEFINED;
    flags = JS_UNDEFINED;
    rep_str = JS_UNDEFINED;
    namedCaptures = JS_UNDEFINED;

    str = JS_ToString(ctx, argv[0]);
    if (JS_IsException(str))
        goto exception;

    sp = JS_VALUE_GET_STRING(str);
    functionalReplace = JS_IsFunction(ctx, rep);
    if (!functionalReplace) {
        rep_val = JS_ToString(ctx, rep);
        if (JS_IsException(rep_val))
            goto exception;
    }

    if (!functionalReplace && js_is_standard_regexp(ctx, rx)) {
        /* use faster version for simple cases */
        res = js_regexp_replace(ctx, rx, str, rep_val);
        if (!JS_IsUndefined(res))
            goto done;
    }

    /* Functional replace on a standard GLOBAL regexp without named groups:
       the spec is two-phase -- collect every match first, then call the
       replacer once per match -- and the only values phase 2 needs from
       phase 1 are the capture spans. The generic loop materialises a full
       exec result object per match and then re-reads it property by property;
       this path stores the raw span integers and builds exactly the values
       the replacer OBSERVES: its arguments (matched, p1..pn, position,
       string), in spec order. Named groups decline (the replacer would
       receive a groups argument this path does not build); non-global
       declines (single match, the generic cost is negligible).
       lastIndex is a plain data property (proven): the spec sequence leaves
       it at 0 after phase 1's terminating null-exec, which is what the
       initial write below stores, so phase 2's replacer reads the correct
       value; error paths restore the value the sequence would have left. */
    if (functionalReplace && js_is_standard_regexp(ctx, rx)) {
        JSRegExp *re = js_get_regexp(ctx, rx, FALSE);
        uint8_t *re_bytecode = re ? re->bytecode->u.str8 : NULL;
        if (re && !lre_get_groupnames(re_bytecode) &&
            (lre_get_flags(re_bytecode) & LRE_FLAG_GLOBAL)) {
            int re_flags = lre_get_flags(re_bytecode);
            int fu = (re_flags & (LRE_FLAG_UNICODE | LRE_FLAG_UNICODE_SETS)) != 0;
            int capture_count = lre_get_capture_count(re_bytecode);
            int alloc_count = lre_get_alloc_count(re_bytecode);
            int shift, rc, k, nm = 0, spans_cap = 0;
            int64_t last_index = 0, prop_last = 0;
            uint32_t *spans = NULL;    /* capture_count*2 span ints per match */
            RECaptureBuf cb;

            cb.heap_mark = NULL;
            sp = JS_VALUE_GET_STRING(str);
            shift = sp->is_wide_char;
            if (JS_SetProperty(ctx, rx, JS_ATOM_lastIndex,
                               JS_NewInt32(ctx, 0)) < 0)
                goto exception;
            if (alloc_count > 0 && re_capture_init(ctx, &cb, alloc_count))
                goto exception;
            /* phase 1: collect spans */
            for (;;) {
                rc = js_regexp_raw_exec(ctx, cb.capture, re_bytecode, sp,
                                        last_index);
                if (rc != 1) {
                    if (rc < 0) {
                        js_regexp_throw_exec_error(ctx, rc);
                        JS_SetProperty(ctx, rx, JS_ATOM_lastIndex,
                                       JS_NewInt64(ctx, prop_last));
                        goto frep_fail;
                    }
                    break;          /* lastIndex already holds the 0 the
                                       null-exec would store */
                }
                if (nm == spans_cap) {
                    int nc = spans_cap ? spans_cap * 2 : 16;
                    uint32_t *ns;
                    /* nm can approach 2^30 on a multi-GB subject with dense
                       matches: guard the doubling and the byte-size product */
                    if (nc <= spans_cap ||
                        (size_t)nc > SIZE_MAX / (sizeof(*spans) *
                                                 (size_t)capture_count * 2)) {
                        JS_ThrowOutOfMemory(ctx);
                        JS_SetProperty(ctx, rx, JS_ATOM_lastIndex,
                                       JS_NewInt64(ctx, prop_last));
                        goto frep_fail;
                    }
                    ns = js_realloc(ctx, spans,
                        sizeof(*spans) * (size_t)nc * capture_count * 2);
                    if (!ns) {
                        JS_SetProperty(ctx, rx, JS_ATOM_lastIndex,
                                       JS_NewInt64(ctx, prop_last));
                        goto frep_fail;
                    }
                    spans = ns;
                    spans_cap = nc;
                }
                {
                    uint32_t *row = spans + (size_t)nm * capture_count * 2;
                    for (k = 0; k < capture_count; k++) {
                        uint8_t **m = &cb.capture[2 * k];
                        if (m[0] && m[1]) {
                            row[2*k] = (uint32_t)((m[0] - sp->u.str8) >> shift);
                            row[2*k+1] = (uint32_t)((m[1] - sp->u.str8) >> shift);
                        } else {
                            row[2*k] = row[2*k+1] = UINT32_MAX;
                        }
                    }
                    nm++;
                    prop_last = row[1];
                    if (row[0] == row[1]) {
                        last_index = string_advance_index(sp, row[1], fu);
                        prop_last = last_index;
                    } else {
                        last_index = row[1];
                    }
                }
            }
            re_capture_free(ctx, &cb);
            cb.heap_mark = NULL;
            /* phase 2: gaps + replacer calls, exactly the observable order */
            nextSourcePosition = 0;
            for (j = 0; j < nm; j++) {
                uint32_t *row = spans + (size_t)j * capture_count * 2;
                int64_t pos = row[0];
                JSValueConst *cargs;
                JSValue cargv[16], *cargh = NULL, rv;
                int nargs = capture_count + 2;

                if (nargs <= (int)countof(cargv)) {
                    cargs = (JSValueConst *)cargv;
                } else {
                    cargh = js_malloc(ctx, sizeof(*cargh) * nargs);
                    if (!cargh)
                        goto frep_fail;
                    cargs = (JSValueConst *)cargh;
                }
                for (k = 0; k < capture_count; k++) {
                    if (row[2*k] != UINT32_MAX) {
                        ((JSValue *)cargs)[k] =
                            js_sub_string(ctx, sp, row[2*k], row[2*k+1]);
                        if (JS_IsException(((JSValue *)cargs)[k])) {
                            while (--k >= 0)
                                JS_FreeValue(ctx, ((JSValue *)cargs)[k]);
                            js_free(ctx, cargh);
                            goto frep_fail;
                        }
                    } else {
                        ((JSValue *)cargs)[k] = JS_UNDEFINED;
                    }
                }
                ((JSValue *)cargs)[capture_count] = JS_NewInt64(ctx, pos);
                ((JSValue *)cargs)[capture_count + 1] = JS_DupValue(ctx, str);
                rv = JS_Call(ctx, rep, JS_UNDEFINED, nargs, cargs);
                for (k = 0; k < nargs; k++)
                    JS_FreeValue(ctx, ((JSValue *)cargs)[k]);
                js_free(ctx, cargh);
                rep_str = JS_ToStringFree(ctx, rv);
                if (JS_IsException(rep_str))
                    goto frep_fail;
                if (pos > nextSourcePosition) {
                    if (string_buffer_concat(b, sp, nextSourcePosition, pos))
                        goto frep_fail;
                }
                if (string_buffer_concat_value(b, rep_str))
                    goto frep_fail;
                JS_FreeValue(ctx, rep_str);
                rep_str = JS_UNDEFINED;
                if ((int64_t)row[1] > nextSourcePosition)
                    nextSourcePosition = row[1];
            }
            js_free(ctx, spans);
            if (nextSourcePosition < sp->len) {
                if (string_buffer_concat(b, sp, nextSourcePosition, sp->len))
                    goto exception;
            }
            res = string_buffer_end(b);
            goto done1;   /* frees everything except the consumed buffer,
                             exactly like the generic success path */
        frep_fail:
            re_capture_free(ctx, &cb);
            js_free(ctx, spans);
            goto exception;
        }
    }
    
    flags = JS_GetProperty(ctx, rx, JS_ATOM_flags);
    if (JS_IsException(flags))
        goto exception;
    flags = JS_ToStringFree(ctx, flags);
    if (JS_IsException(flags))
        goto exception;
    p = JS_VALUE_GET_STRING(flags);

    fullUnicode = 0;
    is_global = (-1 != string_indexof_char(p, 'g', 0));
    if (is_global) {
        fullUnicode = (string_indexof_char(p, 'u', 0) >= 0 ||
                       string_indexof_char(p, 'v', 0) >= 0);
        if (JS_SetProperty(ctx, rx, JS_ATOM_lastIndex, JS_NewInt32(ctx, 0)) < 0)
            goto exception;
    }

    for(;;) {
        JSValue result;
        result = JS_RegExpExec(ctx, rx, str);
        if (JS_IsException(result))
            goto exception;
        if (JS_IsNull(result))
            break;
        if (value_buffer_append(results, result) < 0)
            goto exception;
        if (!is_global)
            break;
        JS_FreeValue(ctx, matched);
        matched = JS_ToStringFree(ctx, JS_GetPropertyInt64(ctx, result, 0));
        if (JS_IsException(matched))
            goto exception;
        if (JS_IsEmptyString(matched)) {
            /* always advance of at least one char */
            int64_t thisIndex, nextIndex;
            if (JS_ToLengthFree(ctx, &thisIndex, JS_GetProperty(ctx, rx, JS_ATOM_lastIndex)) < 0)
                goto exception;
            nextIndex = string_advance_index(sp, thisIndex, fullUnicode);
            if (JS_SetProperty(ctx, rx, JS_ATOM_lastIndex, JS_NewInt64(ctx, nextIndex)) < 0)
                goto exception;
        }
    }
    nextSourcePosition = 0;
    for(j = 0; j < results->len; j++) {
        JSValueConst result;
        result = results->arr[j];
        if (js_get_length32(ctx, &nCaptures, result) < 0)
            goto exception;
        JS_FreeValue(ctx, matched);
        matched = JS_ToStringFree(ctx, JS_GetPropertyInt64(ctx, result, 0));
        if (JS_IsException(matched))
            goto exception;
        if (JS_ToLengthFree(ctx, &position, JS_GetProperty(ctx, result, JS_ATOM_index)))
            goto exception;
        if (position > sp->len)
            position = sp->len;
        else if (position < 0)
            position = 0;
        /* ignore substition if going backward (can happen
           with custom regexp object) */
        JS_FreeValue(ctx, tab);
        tab = JS_NewArray(ctx);
        if (JS_IsException(tab))
            goto exception;
        if (JS_DefinePropertyValueInt64(ctx, tab, 0, JS_DupValue(ctx, matched),
                                        JS_PROP_C_W_E | JS_PROP_THROW) < 0)
            goto exception;
        for(n = 1; n < nCaptures; n++) {
            JSValue capN;
            capN = JS_GetPropertyInt64(ctx, result, n);
            if (JS_IsException(capN))
                goto exception;
            if (!JS_IsUndefined(capN)) {
                capN = JS_ToStringFree(ctx, capN);
                if (JS_IsException(capN))
                    goto exception;
            }
            if (JS_DefinePropertyValueInt64(ctx, tab, n, capN,
                                            JS_PROP_C_W_E | JS_PROP_THROW) < 0)
                goto exception;
        }
        JS_FreeValue(ctx, namedCaptures);
        namedCaptures = JS_GetProperty(ctx, result, JS_ATOM_groups);
        if (JS_IsException(namedCaptures))
            goto exception;
        if (functionalReplace) {
            if (JS_DefinePropertyValueInt64(ctx, tab, n++, JS_NewInt32(ctx, position), JS_PROP_C_W_E | JS_PROP_THROW) < 0)
                goto exception;
            if (JS_DefinePropertyValueInt64(ctx, tab, n++, JS_DupValue(ctx, str), JS_PROP_C_W_E | JS_PROP_THROW) < 0)
                goto exception;
            if (!JS_IsUndefined(namedCaptures)) {
                if (JS_DefinePropertyValueInt64(ctx, tab, n++, JS_DupValue(ctx, namedCaptures), JS_PROP_C_W_E | JS_PROP_THROW) < 0)
                    goto exception;
            }
            args[0] = JS_UNDEFINED;
            args[1] = tab;
            JS_FreeValue(ctx, rep_str);
            rep_str = JS_ToStringFree(ctx, js_function_apply(ctx, rep, 2, args, 0));
        } else {
            JSValue namedCaptures1;
            StringBuffer b1_s, *b1 = &b1_s;
            int ret;
            
            if (!JS_IsUndefined(namedCaptures)) {
                namedCaptures1 = JS_ToObject(ctx, namedCaptures);
                if (JS_IsException(namedCaptures1))
                    goto exception;
            } else {
                namedCaptures1 = JS_UNDEFINED;
            }
            JS_FreeValue(ctx, rep_str);
            
            string_buffer_init(ctx, b1, 0);
            ret = js_string_GetSubstitution(ctx, b1, matched, sp, position,
                                            tab, namedCaptures1, rep_val,
                                            NULL, 0);
            rep_str = string_buffer_end(b1);
            JS_FreeValue(ctx, namedCaptures1);
            if (ret)
                goto exception;
        }
        if (JS_IsException(rep_str))
            goto exception;
        if (position >= nextSourcePosition) {
            string_buffer_concat(b, sp, nextSourcePosition, position);
            string_buffer_concat_value(b, rep_str);
            nextSourcePosition = position + JS_VALUE_GET_STRING(matched)->len;
        }
    }
    string_buffer_concat(b, sp, nextSourcePosition, sp->len);
    res = string_buffer_end(b);
    goto done1;

exception:
    res = JS_EXCEPTION;
done:
    string_buffer_free(b);
done1:
    value_buffer_free(results);
    JS_FreeValue(ctx, rep_val);
    JS_FreeValue(ctx, matched);
    JS_FreeValue(ctx, flags);
    JS_FreeValue(ctx, tab);
    JS_FreeValue(ctx, rep_str);
    JS_FreeValue(ctx, namedCaptures);
    JS_FreeValue(ctx, str);
    return res;
}

static JSValue js_regexp_Symbol_search(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    JSValueConst rx = this_val;
    JSValue str, previousLastIndex, currentLastIndex, result, index;

    if (!JS_IsObject(rx))
        return JS_ThrowTypeErrorNotAnObject(ctx);

    result = JS_UNDEFINED;
    currentLastIndex = JS_UNDEFINED;
    previousLastIndex = JS_UNDEFINED;
    str = JS_ToString(ctx, argv[0]);
    if (JS_IsException(str))
        goto exception;

    /* Standard regexp: the spec saves lastIndex, forces 0, execs, then
       restores -- so the property's FINAL value always equals its initial
       value, and with the built-in exec (proven) nothing between those steps
       can observe the intermediate states. Run the match from 0 directly and
       never touch the property at all: same net effect, none of the
       save/set/exec-object/restore machinery. */
    if (js_is_standard_regexp(ctx, rx)) {
        JSRegExp *re = js_get_regexp(ctx, rx, TRUE);
        JSString *sp;
        uint8_t *re_bytecode;
        int rc, alloc_count, shift;
        RECaptureBuf cb;

        if (!re)
            goto exception;
        cb.heap_mark = NULL;
        re_bytecode = re->bytecode->u.str8;
        sp = JS_VALUE_GET_STRING(str);
        alloc_count = lre_get_alloc_count(re_bytecode);
        if (alloc_count > 0 && re_capture_init(ctx, &cb, alloc_count))
            goto exception;
        rc = js_regexp_raw_exec(ctx, cb.capture, re_bytecode, sp, 0);
        if (rc < 0) {
            js_regexp_throw_exec_error(ctx, rc);
            re_capture_free(ctx, &cb);
            goto exception;
        }
        shift = sp->is_wide_char;
        result = rc == 1 ?
            JS_NewInt32(ctx, (int)((cb.capture[0] - sp->u.str8) >> shift)) :
            JS_NewInt32(ctx, -1);
        re_capture_free(ctx, &cb);
        JS_FreeValue(ctx, str);
        return result;
    }

    previousLastIndex = JS_GetProperty(ctx, rx, JS_ATOM_lastIndex);
    if (JS_IsException(previousLastIndex))
        goto exception;

    if (!js_same_value(ctx, previousLastIndex, JS_NewInt32(ctx, 0))) {
        if (JS_SetProperty(ctx, rx, JS_ATOM_lastIndex, JS_NewInt32(ctx, 0)) < 0) {
            goto exception;
        }
    }
    result = JS_RegExpExec(ctx, rx, str);
    if (JS_IsException(result))
        goto exception;
    currentLastIndex = JS_GetProperty(ctx, rx, JS_ATOM_lastIndex);
    if (JS_IsException(currentLastIndex))
        goto exception;
    if (js_same_value(ctx, currentLastIndex, previousLastIndex)) {
        JS_FreeValue(ctx, previousLastIndex);
    } else {
        if (JS_SetProperty(ctx, rx, JS_ATOM_lastIndex, previousLastIndex) < 0) {
            previousLastIndex = JS_UNDEFINED;
            goto exception;
        }
    }
    JS_FreeValue(ctx, str);
    JS_FreeValue(ctx, currentLastIndex);

    if (JS_IsNull(result)) {
        return JS_NewInt32(ctx, -1);
    } else {
        index = JS_GetProperty(ctx, result, JS_ATOM_index);
        JS_FreeValue(ctx, result);
        return index;
    }

exception:
    JS_FreeValue(ctx, result);
    JS_FreeValue(ctx, str);
    JS_FreeValue(ctx, currentLastIndex);
    JS_FreeValue(ctx, previousLastIndex);
    return JS_EXCEPTION;
}

static JSValue js_regexp_Symbol_split(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    // [Symbol.split](str, limit)
    JSValueConst rx = this_val;
    JSValueConst args[2];
    JSValue str, ctor, splitter, A, flags, z, sub;
    JSString *strp;
    uint32_t lim, size, p, q;
    int unicodeMatching;
    int64_t lengthA, e, numberOfCaptures, i;

    if (!JS_IsObject(rx))
        return JS_ThrowTypeErrorNotAnObject(ctx);

    ctor = JS_UNDEFINED;
    splitter = JS_UNDEFINED;
    A = JS_UNDEFINED;
    flags = JS_UNDEFINED;
    z = JS_UNDEFINED;
    str = JS_ToString(ctx, argv[0]);
    if (JS_IsException(str))
        goto exception;
    ctor = JS_SpeciesConstructor(ctx, rx, ctx->regexp_ctor);
    if (JS_IsException(ctor))
        goto exception;
    flags = JS_ToStringFree(ctx, JS_GetProperty(ctx, rx, JS_ATOM_flags));
    if (JS_IsException(flags))
        goto exception;
    strp = JS_VALUE_GET_STRING(flags);
    unicodeMatching = (string_indexof_char(strp, 'u', 0) >= 0 ||
                       string_indexof_char(strp, 'v', 0) >= 0);
    if (string_indexof_char(strp, 'y', 0) < 0) {
        flags = JS_ConcatString3(ctx, "", flags, "y");
        if (JS_IsException(flags))
            goto exception;
    }
    args[0] = rx;
    args[1] = flags;
    splitter = JS_CallConstructor(ctx, ctor, 2, args);
    if (JS_IsException(splitter))
        goto exception;
    A = JS_NewArray(ctx);
    if (JS_IsException(A))
        goto exception;
    lengthA = 0;
    if (JS_IsUndefined(argv[1])) {
        lim = 0xffffffff;
    } else {
        if (JS_ToUint32(ctx, &lim, argv[1]) < 0)
            goto exception;
        if (lim == 0)
            goto done;
    }
    strp = JS_VALUE_GET_STRING(str);
    p = q = 0;
    size = strp->len;
    if (size == 0) {
        z = JS_RegExpExec(ctx, splitter, str);
        if (JS_IsException(z))
            goto exception;
        if (JS_IsNull(z))
            goto add_tail;
        goto done;
    }

    /* Fast loop. Conditions: the species constructor was the built-in RegExp
       (so `splitter` is a fresh internal object no user code holds -- its
       lastIndex churn is unobservable and can be dropped entirely) and the
       fresh splitter is standard (a paranoia check; a patched
       RegExp.prototype.exec would fail it). The spec loop then reduces to:
       sticky-exec at q, and on a match, slice p..q plus one slice per capture.
       The old loop paid, PER POSITION q: a lastIndex property write, an exec
       result object with index/input/groups, a lastIndex property read with
       ToLength, and a length property read -- all to extract two integers and
       the capture substrings it was going to slice anyway. */
    if (JS_VALUE_GET_OBJ(ctor) == JS_VALUE_GET_OBJ(ctx->regexp_ctor) &&
        js_is_standard_regexp(ctx, splitter)) {
        JSRegExp *re = js_get_regexp(ctx, splitter, TRUE);
        uint8_t *re_bytecode;
        int rc, alloc_count, shift, capture_count;
        RECaptureBuf cb;

        if (!re)
            goto exception;
        cb.heap_mark = NULL;
        re_bytecode = re->bytecode->u.str8;
        capture_count = lre_get_capture_count(re_bytecode);
        alloc_count = lre_get_alloc_count(re_bytecode);
        shift = strp->is_wide_char;
        if (alloc_count > 0 && re_capture_init(ctx, &cb, alloc_count))
            goto exception;
        while (q < size) {
            rc = js_regexp_raw_exec(ctx, cb.capture, re_bytecode, strp, q);
            if (rc < 0) {
                js_regexp_throw_exec_error(ctx, rc);
                goto split_fast_fail;
            }
            if (rc != 1) {
                q = string_advance_index(strp, q, unicodeMatching);
            } else {
                e = (cb.capture[1] - strp->u.str8) >> shift;
                if (e > size)
                    e = size;
                if (e == p) {
                    q = string_advance_index(strp, q, unicodeMatching);
                } else {
                    sub = js_sub_string(ctx, strp, p, q);
                    if (JS_IsException(sub))
                        goto split_fast_fail;
                    if (JS_DefinePropertyValueInt64(ctx, A, lengthA++, sub,
                                            JS_PROP_C_W_E | JS_PROP_THROW) < 0)
                        goto split_fast_fail;
                    if (lengthA == lim)
                        goto split_fast_done;
                    p = e;
                    for (i = 1; i < capture_count; i++) {
                        uint8_t **match = &cb.capture[2 * i];
                        if (match[0] && match[1]) {
                            sub = js_sub_string(ctx, strp,
                                    (match[0] - strp->u.str8) >> shift,
                                    (match[1] - strp->u.str8) >> shift);
                            if (JS_IsException(sub))
                                goto split_fast_fail;
                        } else {
                            sub = JS_UNDEFINED;
                        }
                        if (JS_DefinePropertyValueInt64(ctx, A, lengthA++, sub,
                                            JS_PROP_C_W_E | JS_PROP_THROW) < 0)
                            goto split_fast_fail;
                        if (lengthA == lim)
                            goto split_fast_done;
                    }
                    q = p;
                }
            }
        }
        re_capture_free(ctx, &cb);
        goto add_tail;
    split_fast_done:
        re_capture_free(ctx, &cb);
        goto done;
    split_fast_fail:
        re_capture_free(ctx, &cb);
        goto exception;
    }

    while (q < size) {
        if (JS_SetProperty(ctx, splitter, JS_ATOM_lastIndex, JS_NewInt32(ctx, q)) < 0)
            goto exception;
        JS_FreeValue(ctx, z);
        z = JS_RegExpExec(ctx, splitter, str);
        if (JS_IsException(z))
            goto exception;
        if (JS_IsNull(z)) {
            q = string_advance_index(strp, q, unicodeMatching);
        } else {
            if (JS_ToLengthFree(ctx, &e, JS_GetProperty(ctx, splitter, JS_ATOM_lastIndex)))
                goto exception;
            if (e > size)
                e = size;
            if (e == p) {
                q = string_advance_index(strp, q, unicodeMatching);
            } else {
                sub = js_sub_string(ctx, strp, p, q);
                if (JS_IsException(sub))
                    goto exception;
                if (JS_DefinePropertyValueInt64(ctx, A, lengthA++, sub,
                                                JS_PROP_C_W_E | JS_PROP_THROW) < 0)
                    goto exception;
                if (lengthA == lim)
                    goto done;
                p = e;
                if (js_get_length64(ctx, &numberOfCaptures, z))
                    goto exception;
                for(i = 1; i < numberOfCaptures; i++) {
                    sub = JS_GetPropertyInt64(ctx, z, i);
                    if (JS_IsException(sub))
                        goto exception;
                    if (JS_DefinePropertyValueInt64(ctx, A, lengthA++, sub, JS_PROP_C_W_E | JS_PROP_THROW) < 0)
                        goto exception;
                    if (lengthA == lim)
                        goto done;
                }
                q = p;
            }
        }
    }
add_tail:
    if (p > size)
        p = size;
    sub = js_sub_string(ctx, strp, p, size);
    if (JS_IsException(sub))
        goto exception;
    if (JS_DefinePropertyValueInt64(ctx, A, lengthA++, sub, JS_PROP_C_W_E | JS_PROP_THROW) < 0)
        goto exception;
    goto done;
exception:
    JS_FreeValue(ctx, A);
    A = JS_EXCEPTION;
done:
    JS_FreeValue(ctx, str);
    JS_FreeValue(ctx, ctor);
    JS_FreeValue(ctx, splitter);
    JS_FreeValue(ctx, flags);
    JS_FreeValue(ctx, z);
    return A;
}

static const JSCFunctionListEntry js_regexp_funcs[] = {
    JS_CFUNC_DEF("escape", 1, js_regexp_escape ),
    JS_CGETSET_DEF("[Symbol.species]", js_get_this, NULL ),
};

static const JSCFunctionListEntry js_regexp_proto_funcs[] = {
    JS_CGETSET_DEF("flags", js_regexp_get_flags, NULL ),
    JS_CGETSET_DEF("source", js_regexp_get_source, NULL ),
    JS_CGETSET_MAGIC_DEF("global", js_regexp_get_flag, NULL, LRE_FLAG_GLOBAL ),
    JS_CGETSET_MAGIC_DEF("ignoreCase", js_regexp_get_flag, NULL, LRE_FLAG_IGNORECASE ),
    JS_CGETSET_MAGIC_DEF("multiline", js_regexp_get_flag, NULL, LRE_FLAG_MULTILINE ),
    JS_CGETSET_MAGIC_DEF("dotAll", js_regexp_get_flag, NULL, LRE_FLAG_DOTALL ),
    JS_CGETSET_MAGIC_DEF("unicode", js_regexp_get_flag, NULL, LRE_FLAG_UNICODE ),
    JS_CGETSET_MAGIC_DEF("unicodeSets", js_regexp_get_flag, NULL, LRE_FLAG_UNICODE_SETS ),
    JS_CGETSET_MAGIC_DEF("sticky", js_regexp_get_flag, NULL, LRE_FLAG_STICKY ),
    JS_CGETSET_MAGIC_DEF("hasIndices", js_regexp_get_flag, NULL, LRE_FLAG_INDICES ),
    JS_CFUNC_DEF("exec", 1, js_regexp_exec ),
    JS_CFUNC_DEF("compile", 2, js_regexp_compile ),
    JS_CFUNC_DEF("test", 1, js_regexp_test ),
    JS_CFUNC_DEF("toString", 0, js_regexp_toString ),
    JS_CFUNC_DEF("[Symbol.replace]", 2, js_regexp_Symbol_replace ),
    JS_CFUNC_DEF("[Symbol.match]", 1, js_regexp_Symbol_match ),
    JS_CFUNC_DEF("[Symbol.matchAll]", 1, js_regexp_Symbol_matchAll ),
    JS_CFUNC_DEF("[Symbol.search]", 1, js_regexp_Symbol_search ),
    JS_CFUNC_DEF("[Symbol.split]", 2, js_regexp_Symbol_split ),
};

static const JSCFunctionListEntry js_regexp_string_iterator_proto_funcs[] = {
    JS_ITERATOR_NEXT_DEF("next", 0, js_regexp_string_iterator_next, 0 ),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "RegExp String Iterator", JS_PROP_CONFIGURABLE ),
};

void JS_AddIntrinsicRegExpCompiler(JSContext *ctx)
{
    ctx->compile_regexp = js_compile_regexp;
}

int JS_AddIntrinsicRegExp(JSContext *ctx)
{
    JSValue obj;

    JS_AddIntrinsicRegExpCompiler(ctx);

    obj = JS_NewCConstructor(ctx, JS_CLASS_REGEXP, "RegExp",
                                    js_regexp_constructor, 2, JS_CFUNC_constructor_or_func, 0,
                                    JS_UNDEFINED,
                                    js_regexp_funcs, countof(js_regexp_funcs),
                                    js_regexp_proto_funcs, countof(js_regexp_proto_funcs),
                                    0);
    if (JS_IsException(obj))
        return -1;
    ctx->regexp_ctor = obj;
    
    ctx->class_proto[JS_CLASS_REGEXP_STRING_ITERATOR] =
        JS_NewObjectProtoList(ctx, ctx->class_proto[JS_CLASS_ITERATOR],
                              js_regexp_string_iterator_proto_funcs,
                              countof(js_regexp_string_iterator_proto_funcs));
    if (JS_IsException(ctx->class_proto[JS_CLASS_REGEXP_STRING_ITERATOR]))
        return -1;

    ctx->regexp_shape = js_new_shape2(ctx, get_proto_obj(ctx->class_proto[JS_CLASS_REGEXP]),
                                     JS_PROP_INITIAL_HASH_SIZE, 1);
    if (!ctx->regexp_shape)
        return -1;
    if (add_shape_property(ctx, &ctx->regexp_shape, NULL,
                           JS_ATOM_lastIndex, JS_PROP_WRITABLE))
        return -1;

    ctx->regexp_result_shape = js_new_shape2(ctx, get_proto_obj(ctx->class_proto[JS_CLASS_ARRAY]),
                                     JS_PROP_INITIAL_HASH_SIZE, 4);
    if (!ctx->regexp_result_shape)
        return -1;
    if (add_shape_property(ctx, &ctx->regexp_result_shape, NULL,
                           JS_ATOM_length, JS_PROP_WRITABLE | JS_PROP_LENGTH))
        return -1;
    if (add_shape_property(ctx, &ctx->regexp_result_shape, NULL,
                           JS_ATOM_index, JS_PROP_C_W_E))
        return -1;
    if (add_shape_property(ctx, &ctx->regexp_result_shape, NULL,
                           JS_ATOM_input, JS_PROP_C_W_E))
        return -1;
    if (add_shape_property(ctx, &ctx->regexp_result_shape, NULL,
                           JS_ATOM_groups, JS_PROP_C_W_E))
        return -1;

    return 0;
}

