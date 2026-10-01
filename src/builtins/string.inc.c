static int js_string_get_own_property(JSContext* ctx,
    JSPropertyDescriptor* desc,
    JSValueConst obj, JSAtom prop)
{
    JSObject* p;
    JSString* p1;
    uint32_t idx, ch;

    if (__JS_AtomIsTaggedInt(prop)) {
        p = JS_VALUE_GET_OBJ(obj);
        if (JS_VALUE_GET_TAG(p->u.object_data) == JS_TAG_STRING) {
            p1 = JS_VALUE_GET_STRING(p->u.object_data);
            idx = __JS_AtomToUInt32(prop);
            if (idx < p1->len) {
                if (desc) {
                    ch = string_get(p1, idx);
                    desc->flags = JS_PROP_ENUMERABLE;
                    desc->value = js_new_string_char(ctx, ch);
                    desc->getter = JS_UNDEFINED;
                    desc->setter = JS_UNDEFINED;
                }
                return TRUE;
            }
        }
    }
    return FALSE;
}

static int js_string_define_own_property(JSContext* ctx,
    JSValueConst this_obj,
    JSAtom prop, JSValueConst val,
    JSValueConst getter,
    JSValueConst setter, int flags)
{
    uint32_t idx;
    JSObject* p;
    JSString *p1, *p2;

    if (__JS_AtomIsTaggedInt(prop)) {
        idx = __JS_AtomToUInt32(prop);
        p = JS_VALUE_GET_OBJ(this_obj);
        if (JS_VALUE_GET_TAG(p->u.object_data) != JS_TAG_STRING)
            goto def;
        p1 = JS_VALUE_GET_STRING(p->u.object_data);
        if (idx >= p1->len)
            goto def;
        if (!check_define_prop_flags(JS_PROP_ENUMERABLE, flags))
            goto fail;
        if (flags & JS_PROP_HAS_VALUE) {
            if (JS_VALUE_GET_TAG(val) != JS_TAG_STRING)
                goto fail;
            p2 = JS_VALUE_GET_STRING(val);
            if (p2->len != 1)
                goto fail;
            if (string_get(p1, idx) != string_get(p2, 0)) {
            fail:
                return JS_ThrowTypeErrorOrFalse(ctx, flags, "property is not configurable");
            }
        }
        return TRUE;
    } else {
    def:
        return JS_DefineProperty(ctx, this_obj, prop, val, getter, setter,
            flags | JS_PROP_NO_EXOTIC);
    }
}

static int js_string_delete_property(JSContext* ctx,
    JSValueConst obj, JSAtom prop)
{
    uint32_t idx;

    if (__JS_AtomIsTaggedInt(prop)) {
        idx = __JS_AtomToUInt32(prop);
        if (idx < js_string_obj_get_length(ctx, obj)) {
            return FALSE;
        }
    }
    return TRUE;
}

static const JSClassExoticMethods js_string_exotic_methods = {
    .get_own_property = js_string_get_own_property,
    .define_own_property = js_string_define_own_property,
    .delete_property = js_string_delete_property,
};

static JSValue js_string_constructor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    JSValue val, obj;
    if (argc == 0) {
        val = JS_AtomToString(ctx, JS_ATOM_empty_string);
    } else {
        if (JS_IsUndefined(new_target) && JS_IsSymbol(argv[0])) {
            JSAtomStruct* p = JS_VALUE_GET_PTR(argv[0]);
            val = JS_ConcatString3(ctx, "Symbol(", JS_AtomToString(ctx, js_get_atom_index(ctx->rt, p)), ")");
        } else {
            val = JS_ToString(ctx, argv[0]);
        }
        if (JS_IsException(val))
            return val;
    }
    if (!JS_IsUndefined(new_target)) {
        JSString* p1 = JS_VALUE_GET_STRING(val);

        obj = js_create_from_ctor(ctx, new_target, JS_CLASS_STRING);
        if (JS_IsException(obj)) {
            JS_FreeValue(ctx, val);
        } else {
            JS_SetObjectData(ctx, obj, val);
            JS_DefinePropertyValue(ctx, obj, JS_ATOM_length, JS_NewInt32(ctx, p1->len), 0);
        }
        return obj;
    } else {
        return val;
    }
}

static JSValue js_thisStringValue(JSContext* ctx, JSValueConst this_val)
{
    if (JS_VALUE_GET_TAG(this_val) == JS_TAG_STRING || JS_VALUE_GET_TAG(this_val) == JS_TAG_STRING_ROPE)
        return JS_DupValue(ctx, this_val);

    if (JS_VALUE_GET_TAG(this_val) == JS_TAG_OBJECT) {
        JSObject* p = JS_VALUE_GET_OBJ(this_val);
        if (p->class_id == JS_CLASS_STRING) {
            if (JS_VALUE_GET_TAG(p->u.object_data) == JS_TAG_STRING)
                return JS_DupValue(ctx, p->u.object_data);
        }
    }
    return JS_ThrowTypeError(ctx, "not a string");
}

static JSValue js_string_fromCharCode(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    int i;
    StringBuffer b_s, *b = &b_s;

    string_buffer_init(ctx, b, argc);

    for (i = 0; i < argc; i++) {
        int32_t c;
        if (JS_ToInt32(ctx, &c, argv[i]) || string_buffer_putc16(b, c & 0xffff)) {
            string_buffer_free(b);
            return JS_EXCEPTION;
        }
    }
    return string_buffer_end(b);
}

static JSValue js_string_fromCodePoint(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    double d;
    int i, c;
    StringBuffer b_s, *b = &b_s;

    if (string_buffer_init(ctx, b, argc))
        goto fail;
    for (i = 0; i < argc; i++) {
        if (JS_VALUE_GET_TAG(argv[i]) == JS_TAG_INT) {
            c = JS_VALUE_GET_INT(argv[i]);
            if (c < 0 || c > 0x10ffff)
                goto range_error;
        } else {
            if (JS_ToFloat64(ctx, &d, argv[i]))
                goto fail;
            if (isnan(d) || d < 0 || d > 0x10ffff || (c = (int)d) != d)
                goto range_error;
        }
        if (string_buffer_putc(b, c))
            goto fail;
    }
    return string_buffer_end(b);

range_error:
    JS_ThrowRangeError(ctx, "invalid code point");
fail:
    string_buffer_free(b);
    return JS_EXCEPTION;
}

static JSValue js_string_raw(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSValue cooked, val, raw;
    StringBuffer b_s, *b = &b_s;
    int64_t i, n;

    string_buffer_init(ctx, b, 0);
    raw = JS_UNDEFINED;
    cooked = JS_ToObject(ctx, argv[0]);
    if (JS_IsException(cooked))
        goto exception;
    raw = JS_ToObjectFree(ctx, JS_GetProperty(ctx, cooked, JS_ATOM_raw));
    if (JS_IsException(raw))
        goto exception;
    if (js_get_length64(ctx, &n, raw) < 0)
        goto exception;

    for (i = 0; i < n; i++) {
        val = JS_ToStringFree(ctx, JS_GetPropertyInt64(ctx, raw, i));
        if (JS_IsException(val))
            goto exception;
        string_buffer_concat_value_free(b, val);
        if (i < n - 1 && i + 1 < argc) {
            if (string_buffer_concat_value(b, argv[i + 1]))
                goto exception;
        }
    }
    JS_FreeValue(ctx, cooked);
    JS_FreeValue(ctx, raw);
    return string_buffer_end(b);

exception:
    JS_FreeValue(ctx, cooked);
    JS_FreeValue(ctx, raw);
    string_buffer_free(b);
    return JS_EXCEPTION;
}

JSValue js_string_codePointRange(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    uint32_t start, end, i, n;
    StringBuffer b_s, *b = &b_s;

    if (JS_ToUint32(ctx, &start, argv[0]) || JS_ToUint32(ctx, &end, argv[1]))
        return JS_EXCEPTION;
    end = min_uint32(end, 0x10ffff + 1);

    if (start > end) {
        start = end;
    }
    n = end - start;
    if (end > 0x10000) {
        n += end - max_uint32(start, 0x10000);
    }
    if (string_buffer_init2(ctx, b, n, end >= 0x100))
        return JS_EXCEPTION;
    for (i = start; i < end; i++) {
        string_buffer_putc(b, i);
    }
    return string_buffer_end(b);
}

#if 0
static JSValue js_string___isSpace(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    int c;
    if (JS_ToInt32(ctx, &c, argv[0]))
        return JS_EXCEPTION;
    return JS_NewBool(ctx, lre_is_space(c));
}
#endif

static JSValue js_string_charCodeAt(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSValue val, ret;
    JSString* p;
    int idx, c;

    val = JS_ToStringCheckObject(ctx, this_val);
    if (JS_IsException(val))
        return val;
    p = JS_VALUE_GET_STRING(val);
    if (JS_ToInt32Sat(ctx, &idx, argv[0])) {
        JS_FreeValue(ctx, val);
        return JS_EXCEPTION;
    }
    if (idx < 0 || idx >= p->len) {
        ret = JS_NAN;
    } else {
        c = string_get(p, idx);
        ret = JS_NewInt32(ctx, c);
    }
    JS_FreeValue(ctx, val);
    return ret;
}

static JSValue js_string_charAt(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int is_at)
{
    JSValue val, ret;
    JSString* p;
    int idx, c;

    val = JS_ToStringCheckObject(ctx, this_val);
    if (JS_IsException(val))
        return val;
    p = JS_VALUE_GET_STRING(val);
    if (JS_ToInt32Sat(ctx, &idx, argv[0])) {
        JS_FreeValue(ctx, val);
        return JS_EXCEPTION;
    }
    if (idx < 0 && is_at)
        idx += p->len;
    if (idx < 0 || idx >= p->len) {
        if (is_at)
            ret = JS_UNDEFINED;
        else
            ret = JS_AtomToString(ctx, JS_ATOM_empty_string);
    } else {
        c = string_get(p, idx);
        ret = js_new_string_char(ctx, c);
    }
    JS_FreeValue(ctx, val);
    return ret;
}

static JSValue js_string_codePointAt(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSValue val, ret;
    JSString* p;
    int idx, c;

    val = JS_ToStringCheckObject(ctx, this_val);
    if (JS_IsException(val))
        return val;
    p = JS_VALUE_GET_STRING(val);
    if (JS_ToInt32Sat(ctx, &idx, argv[0])) {
        JS_FreeValue(ctx, val);
        return JS_EXCEPTION;
    }
    if (idx < 0 || idx >= p->len) {
        ret = JS_UNDEFINED;
    } else {
        c = string_getc(p, &idx);
        ret = JS_NewInt32(ctx, c);
    }
    JS_FreeValue(ctx, val);
    return ret;
}

static JSValue js_string_concat(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSValue r;
    int i;

    r = JS_ToStringCheckObject(ctx, this_val);
    for (i = 0; i < argc; i++) {
        if (JS_IsException(r))
            break;
        r = JS_ConcatString(ctx, r, JS_DupValue(ctx, argv[i]));
    }
    return r;
}

static int string_cmp(JSString* p1, JSString* p2, int x1, int x2, int len)
{
    int i, c1, c2;
    for (i = 0; i < len; i++) {
        if ((c1 = string_get(p1, x1 + i)) != (c2 = string_get(p2, x2 + i)))
            return c1 - c2;
    }
    return 0;
}

static int string_indexof_char(JSString* p, int c, int from)
{
    int len = p->len;
    size_t r;
    if (p->is_wide_char) {
        if (c == (uint16_t)c) {
            r = simd.find_u16(js_str_data16(p) + from, (uint16_t)c,
                (size_t)(len - from));
            if (r != SIZE_MAX)
                return from + (int)r;
        }
    } else if ((c & ~0xff) == 0) {
        r = simd.find_u8(js_str_data8(p) + from, (uint8_t)c, (size_t)(len - from));
        if (r != SIZE_MAX)
            return from + (int)r;
    }
    return -1;
}

static int string_indexof(JSString* p1, JSString* p2, int from)
{
    int c, i, j, len1 = p1->len, len2 = p2->len;
    if (len2 == 0)
        return from;
#ifndef DYN_NO_SIMD_STRFIND
    if (!p1->is_wide_char && !p2->is_wide_char) {
        size_t rel;
        if (len2 > len1 - from)
            return -1;
        rel = simd.strfind(js_str_data8(p1) + from, (size_t)(len1 - from),
            js_str_data8(p2), (size_t)len2);
        return rel == SIZE_MAX ? -1 : from + (int)rel;
    }
#endif
    for (i = from, c = string_get(p2, 0); i + len2 <= len1; i = j + 1) {
        j = string_indexof_char(p1, c, i);
        if (j < 0 || j + len2 > len1)
            break;
        if (!string_cmp(p1, p2, j + 1, 1, len2 - 1))
            return j;
    }
    return -1;
}

static int64_t string_advance_index(JSString* p, int64_t index, BOOL unicode)
{
    if (!unicode || index >= p->len || !p->is_wide_char) {
        index++;
    } else {
        int index32 = (int)index;
        string_getc(p, &index32);
        index = index32;
    }
    return index;
}

int js_string_find_invalid_codepoint(JSString* p)
{
    int i;
    if (!p->is_wide_char)
        return -1;
    {
        const uint16_t* d = js_str_data16(p);
        for (i = 0; i < p->len; i++) {
            uint32_t c = d[i];
            if (is_surrogate(c)) {
                if (is_hi_surrogate(c) && (i + 1) < p->len
                    && is_lo_surrogate(d[i + 1])) {
                    i++;
                } else {
                    return i;
                }
            }
        }
    }
    return -1;
}

static JSValue js_string_isWellFormed(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSValue str;
    JSString* p;
    BOOL ret;

    str = JS_ToStringCheckObject(ctx, this_val);
    if (JS_IsException(str))
        return JS_EXCEPTION;
    p = JS_VALUE_GET_STRING(str);
    ret = (js_string_find_invalid_codepoint(p) < 0);
    JS_FreeValue(ctx, str);
    return JS_NewBool(ctx, ret);
}

static JSValue js_string_toWellFormed(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSValue str, ret;
    JSString* p;
    int i;

    str = JS_ToStringCheckObject(ctx, this_val);
    if (JS_IsException(str))
        return JS_EXCEPTION;

    p = JS_VALUE_GET_STRING(str);
    i = js_string_find_invalid_codepoint(p);
    if (i < 0)
        return str;

    ret = js_new_string16_len(ctx, js_str_data16(p), p->len);
    JS_FreeValue(ctx, str);
    if (JS_IsException(ret))
        return JS_EXCEPTION;

    p = JS_VALUE_GET_STRING(ret);
    for (; i < p->len; i++) {
        uint32_t c = p->u.str16[i];
        if (is_surrogate(c)) {
            if (is_hi_surrogate(c) && (i + 1) < p->len
                && is_lo_surrogate(p->u.str16[i + 1])) {
                i++;
            } else {
                p->u.str16[i] = 0xFFFD;
            }
        }
    }
    return ret;
}

static JSValue js_string_indexOf(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int lastIndexOf)
{
    JSValue str, v;
    int i, j, k, len, v_len, pos, start, stop, ret, inc;
    JSString* p;
    JSString* p1;

    str = JS_ToStringCheckObject(ctx, this_val);
    if (JS_IsException(str))
        return str;
    v = JS_ToString(ctx, argv[0]);
    if (JS_IsException(v))
        goto fail;
    p = JS_VALUE_GET_STRING(str);
    p1 = JS_VALUE_GET_STRING(v);
    len = p->len;
    v_len = p1->len;
    if (lastIndexOf) {
        pos = len - v_len;
        if (argc > 1) {
            double d;
            if (JS_ToFloat64(ctx, &d, argv[1]))
                goto fail;
            if (!isnan(d)) {
                if (d <= 0)
                    pos = 0;
                else if (d < pos)
                    pos = d;
            }
        }
        start = pos;
        stop = 0;
        inc = -1;
    } else {
        pos = 0;
        if (argc > 1) {
            if (JS_ToInt32Clamp(ctx, &pos, argv[1], 0, len, 0))
                goto fail;
        }
        start = pos;
        stop = len - v_len;
        inc = 1;
    }
    ret = -1;
    if (len >= v_len && inc * (stop - start) >= 0) {
        if (inc > 0) {
            ret = string_indexof(p, p1, start);
        } else {
            int budget = 1024;
            ret = -1;
            for (i = start;; i += inc) {
                if (!string_cmp(p, p1, i, 0, v_len)) {
                    ret = i;
                    break;
                }
                if (i == stop)
                    break;
                if (--budget == 0) {
                    ret = -1;
                    for (j = 0; j <= start;) {
                        k = string_indexof(p, p1, j);
                        if (k < 0 || k > start)
                            break;
                        ret = k;
                        j = k + 1;
                    }
                    break;
                }
            }
        }
    }
    JS_FreeValue(ctx, str);
    JS_FreeValue(ctx, v);
    return JS_NewInt32(ctx, ret);

fail:
    JS_FreeValue(ctx, str);
    JS_FreeValue(ctx, v);
    return JS_EXCEPTION;
}

static int js_is_regexp(JSContext* ctx, JSValueConst obj);

static JSValue js_string_includes(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    JSValue str, v = JS_UNDEFINED;
    int i, len, v_len, pos, start, stop, ret;
    JSString* p;
    JSString* p1;

    str = JS_ToStringCheckObject(ctx, this_val);
    if (JS_IsException(str))
        return str;
    ret = js_is_regexp(ctx, argv[0]);
    if (ret) {
        if (ret > 0)
            JS_ThrowTypeError(ctx, "regexp not supported");
        goto fail;
    }
    v = JS_ToString(ctx, argv[0]);
    if (JS_IsException(v))
        goto fail;
    p = JS_VALUE_GET_STRING(str);
    p1 = JS_VALUE_GET_STRING(v);
    len = p->len;
    v_len = p1->len;
    pos = (magic == 2) ? len : 0;
    if (argc > 1 && !JS_IsUndefined(argv[1])) {
        if (JS_ToInt32Clamp(ctx, &pos, argv[1], 0, len, 0))
            goto fail;
    }
    len -= v_len;
    ret = 0;
    if (magic == 0) {
        start = pos;
        stop = len;
    } else {
        if (magic == 1) {
            if (pos > len)
                goto done;
        } else {
            pos -= v_len;
        }
        start = stop = pos;
    }
    if (magic == 0 && start >= 0 && start <= stop) {
        ret = string_indexof(p, p1, start) >= 0;
        goto done;
    }
    if (start >= 0 && start <= stop) {
        for (i = start;; i++) {
            if (!string_cmp(p, p1, i, 0, v_len)) {
                ret = 1;
                break;
            }
            if (i == stop)
                break;
        }
    }
done:
    JS_FreeValue(ctx, str);
    JS_FreeValue(ctx, v);
    return JS_NewBool(ctx, ret);

fail:
    JS_FreeValue(ctx, str);
    JS_FreeValue(ctx, v);
    return JS_EXCEPTION;
}

static int check_regexp_g_flag(JSContext* ctx, JSValueConst regexp)
{
    int ret;
    JSValue flags;

    ret = js_is_regexp(ctx, regexp);
    if (ret < 0)
        return -1;
    if (ret) {
        flags = JS_GetProperty(ctx, regexp, JS_ATOM_flags);
        if (JS_IsException(flags))
            return -1;
        if (JS_IsUndefined(flags) || JS_IsNull(flags)) {
            JS_ThrowTypeError(ctx, "cannot convert to object");
            return -1;
        }
        flags = JS_ToStringFree(ctx, flags);
        if (JS_IsException(flags))
            return -1;
        ret = string_indexof_char(JS_VALUE_GET_STRING(flags), 'g', 0);
        JS_FreeValue(ctx, flags);
        if (ret < 0) {
            JS_ThrowTypeError(ctx, "regexp must have the 'g' flag");
            return -1;
        }
    }
    return 0;
}

static JSValue js_string_match(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int atom)
{
    JSValueConst O = this_val, regexp = argv[0], args[2];
    JSValue matcher, S, rx, result, str;
    int args_len;

    if (JS_IsUndefined(O) || JS_IsNull(O))
        return JS_ThrowTypeError(ctx, "cannot convert to object");

    if (JS_IsObject(regexp)) {
        matcher = JS_GetProperty(ctx, regexp, atom);
        if (JS_IsException(matcher))
            return JS_EXCEPTION;
        if (atom == JS_ATOM_Symbol_matchAll) {
            if (check_regexp_g_flag(ctx, regexp) < 0) {
                JS_FreeValue(ctx, matcher);
                return JS_EXCEPTION;
            }
        }
        if (!JS_IsUndefined(matcher) && !JS_IsNull(matcher)) {
            return JS_CallFree(ctx, matcher, regexp, 1, &O);
        }
    }
    S = JS_ToString(ctx, O);
    if (JS_IsException(S))
        return JS_EXCEPTION;
    args_len = 1;
    args[0] = regexp;
    str = JS_UNDEFINED;
    if (atom == JS_ATOM_Symbol_matchAll) {
        str = js_new_string8(ctx, "g");
        if (JS_IsException(str))
            goto fail;
        args[args_len++] = (JSValueConst)str;
    }
    rx = JS_CallConstructor(ctx, ctx->regexp_ctor, args_len, args);
    JS_FreeValue(ctx, str);
    if (JS_IsException(rx)) {
    fail:
        JS_FreeValue(ctx, S);
        return JS_EXCEPTION;
    }
    result = JS_InvokeFree(ctx, rx, atom, 1, (JSValueConst*)&S);
    JS_FreeValue(ctx, S);
    return result;
}

static int js_string_GetSubstitution(JSContext* ctx,
    StringBuffer* b,
    JSValueConst matched,
    JSString* sp,
    uint32_t position,
    JSValueConst captures_val,
    JSValueConst namedCaptures,
    JSValueConst rep,
    uint8_t** captures,
    uint32_t captures_len)
{
    JSValue capture, name, s;
    uint32_t len, matched_len;
    int i, j, j0, k, k1, shift;
    int c, c1;
    JSString* rp;

    if (JS_VALUE_GET_TAG(rep) != JS_TAG_STRING) {
        JS_ThrowTypeError(ctx, "not a string");
        goto exception;
    }
    shift = sp->is_wide_char;
    rp = JS_VALUE_GET_STRING(rep);

    if (captures) {
        matched_len = (captures[1] - captures[0]) >> shift;
    } else {
        captures_len = 0;
        if (!JS_IsUndefined(captures_val)) {
            if (js_get_length32(ctx, &captures_len, captures_val))
                goto exception;
        }
        if (js_get_length32(ctx, &matched_len, matched))
            goto exception;
    }

    len = rp->len;
    i = 0;
    for (;;) {
        j = string_indexof_char(rp, '$', i);
        if (j < 0 || j + 1 >= len)
            break;
        string_buffer_concat(b, rp, i, j);
        j0 = j++;
        c = string_get(rp, j++);
        if (c == '$') {
            string_buffer_putc8(b, '$');
        } else if (c == '&') {
            if (captures) {
                string_buffer_concat(b, sp, position, position + matched_len);
            } else {
                if (string_buffer_concat_value(b, matched))
                    goto exception;
            }
        } else if (c == '`') {
            string_buffer_concat(b, sp, 0, position);
        } else if (c == '\'') {
            string_buffer_concat(b, sp, position + matched_len, sp->len);
        } else if (c >= '0' && c <= '9') {
            k = c - '0';
            if (j < len) {
                c1 = string_get(rp, j);
                if (c1 >= '0' && c1 <= '9') {
                    k1 = k * 10 + c1 - '0';
                    if (k1 >= 1 && k1 < captures_len) {
                        k = k1;
                        j++;
                    }
                }
            }
            if (k >= 1 && k < captures_len) {
                if (captures) {
                    int start, end;
                    if (captures[2 * k] && captures[2 * k + 1]) {
                        start = (captures[2 * k] - js_str_data_units(sp)) >> shift;
                        end = (captures[2 * k + 1] - js_str_data_units(sp)) >> shift;
                        string_buffer_concat(b, sp, start, end);
                    }
                } else {
                    s = JS_GetPropertyInt64(ctx, captures_val, k);
                    if (JS_IsException(s))
                        goto exception;
                    if (!JS_IsUndefined(s)) {
                        if (string_buffer_concat_value_free(b, s))
                            goto exception;
                    }
                }
            } else {
                goto norep;
            }
        } else if (c == '<' && !JS_IsUndefined(namedCaptures)) {
            k = string_indexof_char(rp, '>', j);
            if (k < 0)
                goto norep;
            name = js_sub_string(ctx, rp, j, k);
            if (JS_IsException(name))
                goto exception;
            capture = JS_GetPropertyValue(ctx, namedCaptures, name);
            if (JS_IsException(capture))
                goto exception;
            if (!JS_IsUndefined(capture)) {
                if (string_buffer_concat_value_free(b, capture))
                    goto exception;
            }
            j = k + 1;
        } else {
        norep:
            string_buffer_concat(b, rp, j0, j);
        }
        i = j;
    }
    string_buffer_concat(b, rp, i, rp->len);
    return 0;
exception:
    return -1;
}

static JSValue js_string_replace(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv,
    int is_replaceAll)
{
    JSValueConst O = this_val, searchValue = argv[0], replaceValue = argv[1];
    JSValueConst args[3];
    JSValue str, search_str, replaceValue_str, repl_str;
    JSString *sp, *searchp;
    StringBuffer b_s, *b = &b_s;
    int pos, functionalReplace, endOfLastMatch;
    BOOL is_first;

    if (JS_IsUndefined(O) || JS_IsNull(O))
        return JS_ThrowTypeError(ctx, "cannot convert to object");

    search_str = JS_UNDEFINED;
    replaceValue_str = JS_UNDEFINED;
    repl_str = JS_UNDEFINED;

    if (JS_IsObject(searchValue)) {
        JSValue replacer;
        if (is_replaceAll) {
            if (check_regexp_g_flag(ctx, searchValue) < 0)
                return JS_EXCEPTION;
        }
        replacer = JS_GetProperty(ctx, searchValue, JS_ATOM_Symbol_replace);
        if (JS_IsException(replacer))
            return JS_EXCEPTION;
        if (!JS_IsUndefined(replacer) && !JS_IsNull(replacer)) {
            args[0] = O;
            args[1] = replaceValue;
            return JS_CallFree(ctx, replacer, searchValue, 2, args);
        }
    }
    string_buffer_init(ctx, b, 0);

    str = JS_ToString(ctx, O);
    if (JS_IsException(str))
        goto exception;
    search_str = JS_ToString(ctx, searchValue);
    if (JS_IsException(search_str))
        goto exception;
    functionalReplace = JS_IsFunction(ctx, replaceValue);
    if (!functionalReplace) {
        replaceValue_str = JS_ToString(ctx, replaceValue);
        if (JS_IsException(replaceValue_str))
            goto exception;
    }

    sp = JS_VALUE_GET_STRING(str);
    searchp = JS_VALUE_GET_STRING(search_str);
    endOfLastMatch = 0;
    is_first = TRUE;
    for (;;) {
        if (unlikely(searchp->len == 0)) {
            if (is_first)
                pos = 0;
            else if (endOfLastMatch >= sp->len)
                pos = -1;
            else
                pos = endOfLastMatch + 1;
        } else {
            pos = string_indexof(sp, searchp, endOfLastMatch);
        }
        if (pos < 0) {
            if (is_first) {
                string_buffer_free(b);
                JS_FreeValue(ctx, search_str);
                JS_FreeValue(ctx, replaceValue_str);
                return str;
            } else {
                break;
            }
        }

        string_buffer_concat(b, sp, endOfLastMatch, pos);

        if (functionalReplace) {
            args[0] = search_str;
            args[1] = JS_NewInt32(ctx, pos);
            args[2] = str;
            repl_str = JS_ToStringFree(ctx, JS_Call(ctx, replaceValue, JS_UNDEFINED, 3, args));
            if (JS_IsException(repl_str))
                goto exception;
            string_buffer_concat_value_free(b, repl_str);
        } else {
            if (js_string_GetSubstitution(ctx, b, search_str, sp, pos,
                    JS_UNDEFINED, JS_UNDEFINED, replaceValue_str,
                    NULL, 0)) {
                goto exception;
            }
        }

        endOfLastMatch = pos + searchp->len;
        is_first = FALSE;
        if (!is_replaceAll)
            break;
    }
    string_buffer_concat(b, sp, endOfLastMatch, sp->len);
    JS_FreeValue(ctx, search_str);
    JS_FreeValue(ctx, replaceValue_str);
    JS_FreeValue(ctx, str);
    return string_buffer_end(b);

exception:
    string_buffer_free(b);
    JS_FreeValue(ctx, search_str);
    JS_FreeValue(ctx, replaceValue_str);
    JS_FreeValue(ctx, str);
    return JS_EXCEPTION;
}

static JSValue js_string_split(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSValueConst O = this_val, separator = argv[0], limit = argv[1];
    JSValueConst args[2];
    JSValue S, A, R, T;
    uint32_t lim, lengthA;
    int64_t p, q, s, r, e;
    JSString *sp, *rp;

    if (JS_IsUndefined(O) || JS_IsNull(O))
        return JS_ThrowTypeError(ctx, "cannot convert to object");

    S = JS_UNDEFINED;
    A = JS_UNDEFINED;
    R = JS_UNDEFINED;

    if (JS_IsObject(separator)) {
        JSValue splitter;
        splitter = JS_GetProperty(ctx, separator, JS_ATOM_Symbol_split);
        if (JS_IsException(splitter))
            return JS_EXCEPTION;
        if (!JS_IsUndefined(splitter) && !JS_IsNull(splitter)) {
            args[0] = O;
            args[1] = limit;
            return JS_CallFree(ctx, splitter, separator, 2, args);
        }
    }
    S = JS_ToString(ctx, O);
    if (JS_IsException(S))
        goto exception;
    A = JS_NewArray(ctx);
    if (JS_IsException(A))
        goto exception;
    lengthA = 0;
    if (JS_IsUndefined(limit)) {
        lim = 0xffffffff;
    } else {
        if (JS_ToUint32(ctx, &lim, limit) < 0)
            goto exception;
    }
    sp = JS_VALUE_GET_STRING(S);
    s = sp->len;
    R = JS_ToString(ctx, separator);
    if (JS_IsException(R))
        goto exception;
    rp = JS_VALUE_GET_STRING(R);
    r = rp->len;
    p = 0;
    if (lim == 0)
        goto done;
    if (JS_IsUndefined(separator))
        goto add_tail;
    if (s == 0) {
        if (r != 0)
            goto add_tail;
        goto done;
    }
    if (r == 0) {
        JSObject* ap = JS_VALUE_GET_OBJ(A);
        int64_t n = s;
        if (n > (int64_t)lim)
            n = lim;
        for (q = 0; q < n; q++) {
            T = js_new_string_char(ctx, string_get(sp, q));
            if (JS_IsException(T))
                goto exception;
            if (add_fast_array_element(ctx, ap, T, JS_PROP_THROW) < 0)
                goto exception;
            lengthA++;
        }
        goto done;
    }
    {
        JSObject* ap = JS_VALUE_GET_OBJ(A);
        for (q = p; (q += !r) <= s - r - !r; q = p = e + r) {
            e = string_indexof(sp, rp, q);
            if (e < 0)
                break;
            T = js_sub_string(ctx, sp, p, e);
            if (JS_IsException(T))
                goto exception;
            if (add_fast_array_element(ctx, ap, T, JS_PROP_THROW) < 0)
                goto exception;
            lengthA++;
            if (lengthA == lim)
                goto done;
        }
    }
add_tail:
    T = js_sub_string(ctx, sp, p, s);
    if (JS_IsException(T))
        goto exception;
    if (add_fast_array_element(ctx, JS_VALUE_GET_OBJ(A), T, JS_PROP_THROW) < 0)
        goto exception;
    lengthA++;
done:
    JS_FreeValue(ctx, S);
    JS_FreeValue(ctx, R);
    return A;

exception:
    JS_FreeValue(ctx, A);
    JS_FreeValue(ctx, S);
    JS_FreeValue(ctx, R);
    return JS_EXCEPTION;
}

static JSValue js_string_substring(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSValue str, ret;
    int a, b, start, end;
    JSString* p;

    str = JS_ToStringCheckObject(ctx, this_val);
    if (JS_IsException(str))
        return str;
    p = JS_VALUE_GET_STRING(str);
    if (JS_ToInt32Clamp(ctx, &a, argv[0], 0, p->len, 0)) {
        JS_FreeValue(ctx, str);
        return JS_EXCEPTION;
    }
    b = p->len;
    if (!JS_IsUndefined(argv[1])) {
        if (JS_ToInt32Clamp(ctx, &b, argv[1], 0, p->len, 0)) {
            JS_FreeValue(ctx, str);
            return JS_EXCEPTION;
        }
    }
    if (a < b) {
        start = a;
        end = b;
    } else {
        start = b;
        end = a;
    }
    ret = js_sub_string(ctx, p, start, end);
    JS_FreeValue(ctx, str);
    return ret;
}

static JSValue js_string_substr(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSValue str, ret;
    int a, len, n;
    JSString* p;

    str = JS_ToStringCheckObject(ctx, this_val);
    if (JS_IsException(str))
        return str;
    p = JS_VALUE_GET_STRING(str);
    len = p->len;
    if (JS_ToInt32Clamp(ctx, &a, argv[0], 0, len, len)) {
        JS_FreeValue(ctx, str);
        return JS_EXCEPTION;
    }
    n = len - a;
    if (!JS_IsUndefined(argv[1])) {
        if (JS_ToInt32Clamp(ctx, &n, argv[1], 0, len - a, 0)) {
            JS_FreeValue(ctx, str);
            return JS_EXCEPTION;
        }
    }
    ret = js_sub_string(ctx, p, a, a + n);
    JS_FreeValue(ctx, str);
    return ret;
}

static JSValue js_string_slice(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSValue str, ret;
    int len, start, end;
    JSString* p;

    str = JS_ToStringCheckObject(ctx, this_val);
    if (JS_IsException(str))
        return str;
    p = JS_VALUE_GET_STRING(str);
    len = p->len;
    if (JS_ToInt32Clamp(ctx, &start, argv[0], 0, len, len)) {
        JS_FreeValue(ctx, str);
        return JS_EXCEPTION;
    }
    end = len;
    if (!JS_IsUndefined(argv[1])) {
        if (JS_ToInt32Clamp(ctx, &end, argv[1], 0, len, len)) {
            JS_FreeValue(ctx, str);
            return JS_EXCEPTION;
        }
    }
    ret = js_sub_string(ctx, p, start, max_int(end, start));
    JS_FreeValue(ctx, str);
    return ret;
}

static JSValue js_string_pad(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int padEnd)
{
    JSValue str, v = JS_UNDEFINED;
    StringBuffer b_s, *b = &b_s;
    JSString *p, *p1 = NULL;
    int n, len, c = ' ';

    str = JS_ToStringCheckObject(ctx, this_val);
    if (JS_IsException(str))
        goto fail1;
    if (JS_ToInt32Sat(ctx, &n, argv[0]))
        goto fail2;
    p = JS_VALUE_GET_STRING(str);
    len = p->len;
    if (len >= n)
        return str;
    if (argc > 1 && !JS_IsUndefined(argv[1])) {
        v = JS_ToString(ctx, argv[1]);
        if (JS_IsException(v))
            goto fail2;
        p1 = JS_VALUE_GET_STRING(v);
        if (p1->len == 0) {
            JS_FreeValue(ctx, v);
            return str;
        }
        if (p1->len == 1) {
            c = string_get(p1, 0);
            p1 = NULL;
        }
    }
    if (n > JS_STRING_LEN_MAX) {
        JS_ThrowRangeError(ctx, "invalid string length");
        goto fail3;
    }
    if (string_buffer_init(ctx, b, n))
        goto fail3;
    n -= len;
    if (padEnd) {
        if (string_buffer_concat(b, p, 0, len))
            goto fail;
    }
    if (p1) {
        while (n > 0) {
            int chunk = min_int(n, p1->len);
            if (string_buffer_concat(b, p1, 0, chunk))
                goto fail;
            n -= chunk;
        }
    } else {
        if (string_buffer_fill(b, c, n))
            goto fail;
    }
    if (!padEnd) {
        if (string_buffer_concat(b, p, 0, len))
            goto fail;
    }
    JS_FreeValue(ctx, v);
    JS_FreeValue(ctx, str);
    return string_buffer_end(b);

fail:
    string_buffer_free(b);
fail3:
    JS_FreeValue(ctx, v);
fail2:
    JS_FreeValue(ctx, str);
fail1:
    return JS_EXCEPTION;
}

static JSValue js_string_repeat(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSValue str;
    StringBuffer b_s, *b = &b_s;
    JSString* p;
    int64_t val;
    int n, len;

    str = JS_ToStringCheckObject(ctx, this_val);
    if (JS_IsException(str))
        goto fail;
    if (JS_ToInt64Sat(ctx, &val, argv[0]))
        goto fail;
    if (val < 0 || val > 2147483647) {
        JS_ThrowRangeError(ctx, "invalid repeat count");
        goto fail;
    }
    n = val;
    p = JS_VALUE_GET_STRING(str);
    len = p->len;
    if (len == 0 || n == 1)
        return str;
    if (val * len > JS_STRING_LEN_MAX) {
        JS_ThrowRangeError(ctx, "invalid string length");
        goto fail;
    }
    if (string_buffer_init2(ctx, b, n * len, p->is_wide_char))
        goto fail;
    if (len == 1) {
        string_buffer_fill(b, string_get(p, 0), n);
    } else if (n > 0) {
        int total = n * len;
        if (!string_buffer_concat(b, p, 0, len)) {
            while (b->len < total) {
                int chunk = min_int(b->len, total - b->len);
                if (b->is_wide_char)
                    memcpy(b->str->u.str16 + b->len, b->str->u.str16, chunk << 1);
                else
                    memcpy(b->str->u.str8 + b->len, b->str->u.str8, chunk);
                b->len += chunk;
            }
        }
    }
    JS_FreeValue(ctx, str);
    return string_buffer_end(b);

fail:
    JS_FreeValue(ctx, str);
    return JS_EXCEPTION;
}

static JSValue js_string_trim(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    JSValue str, ret;
    int a, b, len;
    JSString* p;

    str = JS_ToStringCheckObject(ctx, this_val);
    if (JS_IsException(str))
        return str;
    p = JS_VALUE_GET_STRING(str);
    a = 0;
    b = len = p->len;
    if (magic & 1) {
        while (a < len && lre_is_space(string_get(p, a)))
            a++;
    }
    if (magic & 2) {
        while (b > a && lre_is_space(string_get(p, b - 1)))
            b--;
    }
    ret = js_sub_string(ctx, p, a, b);
    JS_FreeValue(ctx, str);
    return ret;
}

static int string_prevc(JSString* p, int* pidx)
{
    int idx, c, c1;

    idx = *pidx;
    if (idx <= 0)
        return 0;
    idx--;
    if (p->is_wide_char) {
        const uint16_t* d = js_str_data16(p);
        c = d[idx];
        if (is_lo_surrogate(c) && idx > 0) {
            c1 = d[idx - 1];
            if (is_hi_surrogate(c1)) {
                c = from_surrogate(c1, c);
                idx--;
            }
        }
    } else {
        c = js_str_data8(p)[idx];
    }
    *pidx = idx;
    return c;
}

static BOOL test_final_sigma(JSString* p, int sigma_pos)
{
    int k, c1;

    k = sigma_pos;
    for (;;) {
        c1 = string_prevc(p, &k);
        if (!lre_is_case_ignorable(c1))
            break;
    }
    if (!lre_is_cased(c1))
        return FALSE;

    k = sigma_pos + 1;
    for (;;) {
        if (k >= p->len)
            return TRUE;
        c1 = string_getc(p, &k);
        if (!lre_is_case_ignorable(c1))
            break;
    }
    return !lre_is_cased(c1);
}

static BOOL str8_is_ascii(const uint8_t* s, int n)
{
    int k = 0;
    for (; k + 8 <= n; k += 8) {
        uint64_t w;
        memcpy(&w, s + k, 8);
        if (w & UINT64_C(0x8080808080808080))
            return FALSE;
    }
    for (; k < n; k++)
        if (s[k] >= 0x80)
            return FALSE;
    return TRUE;
}

static JSValue js_string_toLowerCase(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int to_lower)
{
    JSValue val;
    StringBuffer b_s, *b = &b_s;
    JSString* p;
    int i, c, j, l;
    uint32_t res[LRE_CC_RES_LEN_MAX];

    val = JS_ToStringCheckObject(ctx, this_val);
    if (JS_IsException(val))
        return val;
    p = JS_VALUE_GET_STRING(val);
    if (p->len == 0)
        return val;
    if (!p->is_wide_char && str8_is_ascii(js_str_data8(p), p->len)) {
        JSString* r = js_alloc_string(ctx, p->len, 0);
        if (r) {
            const uint8_t* s8 = js_str_data8(p);
            uint8_t* d8 = r->u.str8;
            int n = p->len, k;
            if (to_lower) {
                for (k = 0; k < n; k++) {
                    uint8_t ch = s8[k];
                    d8[k] = ch + ((uint8_t)(ch - 'A') < 26 ? 0x20 : 0);
                }
            } else {
                for (k = 0; k < n; k++) {
                    uint8_t ch = s8[k];
                    d8[k] = ch - ((uint8_t)(ch - 'a') < 26 ? 0x20 : 0);
                }
            }
            d8[n] = '\0';
            JS_FreeValue(ctx, val);
            return JS_MKPTR(JS_TAG_STRING, r);
        }
    }
    if (string_buffer_init(ctx, b, p->len))
        goto fail;
    for (i = 0; i < p->len;) {
        if (p->is_wide_char) {
            uint32_t u = js_str_data16(p)[i];
            if (u < 0x80) {
                if (to_lower)
                    u += ((uint32_t)(u - 'A') < 26) ? 0x20 : 0;
                else
                    u -= ((uint32_t)(u - 'a') < 26) ? 0x20 : 0;
                if (string_buffer_putc16(b, u))
                    goto fail;
                i++;
                continue;
            }
        }
        c = string_getc(p, &i);
        if (c == 0x3a3 && to_lower && test_final_sigma(p, i - 1)) {
            res[0] = 0x3c2;
            l = 1;
        } else {
            l = lre_case_conv(res, c, to_lower);
        }
        for (j = 0; j < l; j++) {
            if (string_buffer_putc(b, res[j]))
                goto fail;
        }
    }
    JS_FreeValue(ctx, val);
    return string_buffer_end(b);
fail:
    JS_FreeValue(ctx, val);
    string_buffer_free(b);
    return JS_EXCEPTION;
}

#ifdef CONFIG_ALL_UNICODE

static int JS_ToUTF32String(JSContext* ctx, uint32_t** pbuf, JSValueConst val1)
{
    JSValue val;
    JSString* p;
    uint32_t* buf;
    int i, j, len;

    val = JS_ToString(ctx, val1);
    if (JS_IsException(val))
        return -1;
    p = JS_VALUE_GET_STRING(val);
    len = p->len;
    buf = js_malloc(ctx, sizeof(buf[0]) * max_int(len, 1));
    if (!buf) {
        JS_FreeValue(ctx, val);
        goto fail;
    }
    for (i = j = 0; i < len;)
        buf[j++] = string_getc(p, &i);
    JS_FreeValue(ctx, val);
    *pbuf = buf;
    return j;
fail:
    *pbuf = NULL;
    return -1;
}

static JSValue JS_NewUTF32String(JSContext* ctx, const uint32_t* buf, int len)
{
    int i;
    StringBuffer b_s, *b = &b_s;
    if (string_buffer_init(ctx, b, len))
        return JS_EXCEPTION;
    for (i = 0; i < len; i++) {
        if (string_buffer_putc(b, buf[i]))
            goto fail;
    }
    return string_buffer_end(b);
fail:
    string_buffer_free(b);
    return JS_EXCEPTION;
}

static int js_string_normalize1(JSContext* ctx, uint32_t** pout_buf,
    JSValueConst val,
    UnicodeNormalizationEnum n_type)
{
    int buf_len, out_len;
    uint32_t *buf, *out_buf;

    buf_len = JS_ToUTF32String(ctx, &buf, val);
    if (buf_len < 0)
        return -1;
    out_len = unicode_normalize(&out_buf, buf, buf_len, n_type,
        ctx->rt, js_realloc_dbuf_rt);
    js_free(ctx, buf);
    if (out_len < 0)
        return -1;
    *pout_buf = out_buf;
    return out_len;
}

static BOOL js_string_is_ascii(const JSString* p)
{
    int i, n = p->len;
    if (!p->is_wide_char) {
        const uint8_t* s8 = js_str_data8(p);
        for (i = 0; i < n; i++) {
            if (s8[i] >= 0x80)
                return FALSE;
        }
    } else {
        const uint16_t* s16 = js_str_data16(p);
        for (i = 0; i < n; i++) {
            if (s16[i] >= 0x80)
                return FALSE;
        }
    }
    return TRUE;
}

static JSValue js_string_normalize(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    const char *form, *p;
    size_t form_len;
    int is_compat, out_len;
    UnicodeNormalizationEnum n_type;
    JSValue val;
    uint32_t* out_buf;

    val = JS_ToStringCheckObject(ctx, this_val);
    if (JS_IsException(val))
        return val;

    if (argc == 0 || JS_IsUndefined(argv[0])) {
        n_type = UNICODE_NFC;
    } else {
        form = JS_ToCStringLen(ctx, &form_len, argv[0]);
        if (!form)
            goto fail1;
        p = form;
        if (p[0] != 'N' || p[1] != 'F')
            goto bad_form;
        p += 2;
        is_compat = FALSE;
        if (*p == 'K') {
            is_compat = TRUE;
            p++;
        }
        if (*p == 'C' || *p == 'D') {
            n_type = UNICODE_NFC + is_compat * 2 + (*p - 'C');
            if ((p + 1 - form) != form_len)
                goto bad_form;
        } else {
        bad_form:
            JS_FreeCString(ctx, form);
            JS_ThrowRangeError(ctx, "bad normalization form");
        fail1:
            JS_FreeValue(ctx, val);
            return JS_EXCEPTION;
        }
        JS_FreeCString(ctx, form);
    }

    if (js_string_is_ascii(JS_VALUE_GET_STRING(val)))
        return val;
    out_len = js_string_normalize1(ctx, &out_buf, val, n_type);
    JS_FreeValue(ctx, val);
    if (out_len < 0)
        return JS_EXCEPTION;
    val = JS_NewUTF32String(ctx, out_buf, out_len);
    js_free(ctx, out_buf);
    return val;
}

static int js_UTF32_compare(const uint32_t* buf1, int buf1_len,
    const uint32_t* buf2, int buf2_len)
{
    int i, len, c, res;
    len = min_int(buf1_len, buf2_len);
    for (i = 0; i < len; i++) {
        c = buf1[i] - buf2[i];
        if (c != 0)
            return c;
    }
    if (buf1_len == buf2_len)
        res = 0;
    else if (buf1_len < buf2_len)
        res = -1;
    else
        res = 1;
    return res;
}

static int js_string_compare_ascii(const JSString* p1, const JSString* p2)
{
    int i, len, c;
    len = min_int(p1->len, p2->len);
    for (i = 0; i < len; i++) {
        c = string_get(p1, i) - string_get(p2, i);
        if (c != 0)
            return c;
    }
    if (p1->len == p2->len)
        return 0;
    return p1->len < p2->len ? -1 : 1;
}

static JSValue js_string_localeCompare(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSValue a, b;
    int cmp, a_len, b_len;
    uint32_t *a_buf, *b_buf;
    const JSString *pa, *pb;

    a = JS_ToStringCheckObject(ctx, this_val);
    if (JS_IsException(a))
        return JS_EXCEPTION;
    b = JS_ToString(ctx, argv[0]);
    if (JS_IsException(b)) {
        JS_FreeValue(ctx, a);
        return JS_EXCEPTION;
    }
    pa = JS_VALUE_GET_STRING(a);
    pb = JS_VALUE_GET_STRING(b);
    if (pa == pb) {
        goto equal;
    }
    if (pa->len == pb->len) {
        if (pa->is_wide_char == pb->is_wide_char) {
            if (memcmp(js_str_data8(pa), js_str_data8(pb),
                    (size_t)pa->len << pa->is_wide_char)
                == 0)
                goto equal;
        } else {
            int i;
            for (i = 0; i < pa->len; i++) {
                if (string_get(pa, i) != string_get(pb, i))
                    break;
            }
            if (i == pa->len)
                goto equal;
        }
    }
    if (js_string_is_ascii(pa) && js_string_is_ascii(pb)) {
        cmp = js_string_compare_ascii(pa, pb);
        JS_FreeValue(ctx, a);
        JS_FreeValue(ctx, b);
        return JS_NewInt32(ctx, cmp);
    }
    a_len = js_string_normalize1(ctx, &a_buf, a, UNICODE_NFC);
    JS_FreeValue(ctx, a);
    if (a_len < 0) {
        JS_FreeValue(ctx, b);
        return JS_EXCEPTION;
    }

    b_len = js_string_normalize1(ctx, &b_buf, b, UNICODE_NFC);
    JS_FreeValue(ctx, b);
    if (b_len < 0) {
        js_free(ctx, a_buf);
        return JS_EXCEPTION;
    }
    cmp = js_UTF32_compare(a_buf, a_len, b_buf, b_len);
    js_free(ctx, a_buf);
    js_free(ctx, b_buf);
    return JS_NewInt32(ctx, cmp);

equal:
    JS_FreeValue(ctx, a);
    JS_FreeValue(ctx, b);
    return JS_NewInt32(ctx, 0);
}
#else
static JSValue js_string_localeCompare(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSValue a, b;
    int cmp;

    a = JS_ToStringCheckObject(ctx, this_val);
    if (JS_IsException(a))
        return JS_EXCEPTION;
    b = JS_ToString(ctx, argv[0]);
    if (JS_IsException(b)) {
        JS_FreeValue(ctx, a);
        return JS_EXCEPTION;
    }
    cmp = js_string_compare(ctx, JS_VALUE_GET_STRING(a), JS_VALUE_GET_STRING(b));
    JS_FreeValue(ctx, a);
    JS_FreeValue(ctx, b);
    return JS_NewInt32(ctx, cmp);
}
#endif

static JSValue js_string_toString(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    return js_thisStringValue(ctx, this_val);
}

static JSValue js_string_iterator_next(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv,
    BOOL* pdone, int magic)
{
    JSArrayIteratorData* it;
    uint32_t idx, c, start;
    JSString* p;

    it = JS_GetOpaque2(ctx, this_val, JS_CLASS_STRING_ITERATOR);
    if (!it) {
        *pdone = FALSE;
        return JS_EXCEPTION;
    }
    if (JS_IsUndefined(it->obj))
        goto done;
    p = JS_VALUE_GET_STRING(it->obj);
    idx = it->idx;
    if (idx >= p->len) {
        JS_FreeValue(ctx, it->obj);
        it->obj = JS_UNDEFINED;
    done:
        *pdone = TRUE;
        return JS_UNDEFINED;
    }

    start = idx;
    c = string_getc(p, (int*)&idx);
    it->idx = idx;
    *pdone = FALSE;
    if (c <= 0xffff) {
        return js_new_string_char(ctx, c);
    } else {
        return js_new_string16_len(ctx, js_str_data16(p) + start, 2);
    }
}

enum {
    magic_string_anchor,
    magic_string_big,
    magic_string_blink,
    magic_string_bold,
    magic_string_fixed,
    magic_string_fontcolor,
    magic_string_fontsize,
    magic_string_italics,
    magic_string_link,
    magic_string_small,
    magic_string_strike,
    magic_string_sub,
    magic_string_sup,
};

static JSValue js_string_CreateHTML(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    JSValue str;
    const JSString* p;
    StringBuffer b_s, *b = &b_s;
    static struct {
        const char *tag, *attr;
    } const defs[] = {
        { "a", "name" },
        { "big", NULL },
        { "blink", NULL },
        { "b", NULL },
        { "tt", NULL },
        { "font", "color" },
        { "font", "size" },
        { "i", NULL },
        { "a", "href" },
        { "small", NULL },
        { "strike", NULL },
        { "sub", NULL },
        { "sup", NULL },
    };

    str = JS_ToStringCheckObject(ctx, this_val);
    if (JS_IsException(str))
        return JS_EXCEPTION;
    string_buffer_init(ctx, b, 7);
    string_buffer_putc8(b, '<');
    string_buffer_puts8(b, defs[magic].tag);
    if (defs[magic].attr) {
        JSValue value;
        int i;

        string_buffer_putc8(b, ' ');
        string_buffer_puts8(b, defs[magic].attr);
        string_buffer_puts8(b, "=\"");
        value = JS_ToStringCheckObject(ctx, argv[0]);
        if (JS_IsException(value)) {
            JS_FreeValue(ctx, str);
            string_buffer_free(b);
            return JS_EXCEPTION;
        }
        p = JS_VALUE_GET_STRING(value);
        for (i = 0; i < p->len; i++) {
            int c = string_get(p, i);
            if (c == '"') {
                string_buffer_puts8(b, "&quot;");
            } else {
                string_buffer_putc16(b, c);
            }
        }
        JS_FreeValue(ctx, value);
        string_buffer_putc8(b, '\"');
    }
    string_buffer_putc8(b, '>');
    string_buffer_concat_value_free(b, str);
    string_buffer_puts8(b, "</");
    string_buffer_puts8(b, defs[magic].tag);
    string_buffer_putc8(b, '>');
    return string_buffer_end(b);
}

static const JSCFunctionListEntry js_string_funcs[] = {
    JS_CFUNC_DEF("fromCharCode", 1, js_string_fromCharCode),
    JS_CFUNC_DEF("fromCodePoint", 1, js_string_fromCodePoint),
    JS_CFUNC_DEF("raw", 1, js_string_raw),
};

static JSValue js_string_ext_isEmpty(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSValue val;
    BOOL empty;
    (void)argc;
    (void)argv;
    val = JS_ToStringCheckObject(ctx, this_val);
    if (JS_IsException(val))
        return val;
    empty = (JS_VALUE_GET_STRING(val)->len == 0);
    JS_FreeValue(ctx, val);
    return JS_NewBool(ctx, empty);
}

static JSValue js_string_ext_isBlank(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSValue val;
    JSString* p;
    BOOL blank = TRUE;
    uint32_t i;
    (void)argc;
    (void)argv;
    val = JS_ToStringCheckObject(ctx, this_val);
    if (JS_IsException(val))
        return val;
    p = JS_VALUE_GET_STRING(val);
    for (i = 0; i < p->len; i++) {
        if (!lre_is_space(string_get(p, i))) {
            blank = FALSE;
            break;
        }
    }
    JS_FreeValue(ctx, val);
    return JS_NewBool(ctx, blank);
}

static JSValue js_string_ext_firstlast(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    JSValue val, ret;
    JSString* p;
    int64_t n = 1, len;
    val = JS_ToStringCheckObject(ctx, this_val);
    if (JS_IsException(val))
        return val;
    if (argc > 0 && !JS_IsUndefined(argv[0])) {
        if (JS_ToInt64Sat(ctx, &n, argv[0])) {
            JS_FreeValue(ctx, val);
            return JS_EXCEPTION;
        }
    }
    p = JS_VALUE_GET_STRING(val);
    len = p->len;
    if (n < 0)
        n = 0;
    if (n > len)
        n = len;
    ret = (magic == 0) ? js_sub_string(ctx, p, 0, (int)n)
                       : js_sub_string(ctx, p, (int)(len - n), (int)len);
    JS_FreeValue(ctx, val);
    return ret;
}

static JSValue js_string_ext_fromto(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    JSValue val, ret;
    JSString* p;
    int64_t idx = 0, len, end;
    BOOL have = (argc > 0 && !JS_IsUndefined(argv[0]));
    val = JS_ToStringCheckObject(ctx, this_val);
    if (JS_IsException(val))
        return val;
    if (have && JS_ToInt64Sat(ctx, &idx, argv[0])) {
        JS_FreeValue(ctx, val);
        return JS_EXCEPTION;
    }
    p = JS_VALUE_GET_STRING(val);
    len = p->len;

    end = len;
    if (magic == 0 && argc > 1 && !JS_IsUndefined(argv[1])) {
        if (JS_ToInt64Sat(ctx, &end, argv[1])) {
            JS_FreeValue(ctx, val);
            return JS_EXCEPTION;
        }
        if (end < 0)
            end += len;
        if (end < 0)
            end = 0;
        if (end > len)
            end = len;
    }
    if (!have && magic == 1)
        idx = len;
    if (idx < 0)
        idx += len;
    if (idx < 0)
        idx = 0;
    if (idx > len)
        idx = len;
    if (magic == 0) {
        if (end < idx)
            end = idx;
        ret = js_sub_string(ctx, p, (int)idx, (int)end);
    } else {
        ret = js_sub_string(ctx, p, 0, (int)idx);
    }
    JS_FreeValue(ctx, val);
    return ret;
}

static JSValue js_string_ext_chars(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSValue val, result, ret = JS_EXCEPTION;
    JSString* p;
    JSValue* dst;
    int i;
    uint32_t len, n;
    (void)argc;
    (void)argv;
    val = JS_ToStringCheckObject(ctx, this_val);
    if (JS_IsException(val))
        return val;
    p = JS_VALUE_GET_STRING(val);
    len = p->len;
    if (len == 0) {
        ret = JS_NewArray(ctx);
        goto done;
    }

    n = 0;
    i = 0;
    while (i < (int)len) {
        string_getc(p, &i);
        n++;
    }
    result = js_allocate_fast_array(ctx, n);
    if (JS_IsException(result))
        goto done;
    dst = JS_VALUE_GET_OBJ(result)->u.array.u.values;
    n = 0;
    i = 0;
    while (i < (int)len) {
        int start = i;
        JSValue s;
        string_getc(p, &i);
        s = js_sub_string(ctx, p, start, i);
        if (JS_IsException(s)) {
            JS_FreeValue(ctx, result);
            goto done;
        }
        dst[n++] = s;
    }
    ret = result;
done:
    JS_FreeValue(ctx, val);
    return ret;
}

static JSValue js_string_ext_codes(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSValue val, result, ret = JS_EXCEPTION;
    JSString* p;
    JSValue* dst;
    int i, c;
    uint32_t len, n;
    (void)argc;
    (void)argv;
    val = JS_ToStringCheckObject(ctx, this_val);
    if (JS_IsException(val))
        return val;
    p = JS_VALUE_GET_STRING(val);
    len = p->len;
    if (len == 0) {
        ret = JS_NewArray(ctx);
        goto done;
    }

    n = 0;
    i = 0;
    while (i < (int)len) {
        c = string_getc(p, &i);
        if (c < 0x80)
            n += 1;
        else if (c < 0x800)
            n += 2;
        else if (c < 0x10000)
            n += 3;
        else
            n += 4;
    }
    result = js_allocate_fast_array(ctx, n);
    if (JS_IsException(result))
        goto done;
    dst = JS_VALUE_GET_OBJ(result)->u.array.u.values;
    n = 0;
    i = 0;
    while (i < (int)len) {
        c = string_getc(p, &i);
        if (c < 0x80) {
            dst[n++] = JS_NewInt32(ctx, c);
        } else if (c < 0x800) {
            dst[n++] = JS_NewInt32(ctx, 0xc0 | (c >> 6));
            dst[n++] = JS_NewInt32(ctx, 0x80 | (c & 0x3f));
        } else if (c < 0x10000) {
            dst[n++] = JS_NewInt32(ctx, (int)(0xe0 | (c >> 12)));
            dst[n++] = JS_NewInt32(ctx, (int)(0x80 | ((c >> 6) & 0x3f)));
            dst[n++] = JS_NewInt32(ctx, (int)(0x80 | (c & 0x3f)));
        } else {
            dst[n++] = JS_NewInt32(ctx, (int)(0xf0 | (c >> 18)));
            dst[n++] = JS_NewInt32(ctx, (int)(0x80 | ((c >> 12) & 0x3f)));
            dst[n++] = JS_NewInt32(ctx, (int)(0x80 | ((c >> 6) & 0x3f)));
            dst[n++] = JS_NewInt32(ctx, (int)(0x80 | (c & 0x3f)));
        }
    }
    ret = result;
done:
    JS_FreeValue(ctx, val);
    return ret;
}

#if defined(__clang__) && !defined(DYN_NO_VEC)
#define DYN_VECTORIZE_LOOP _Pragma("clang loop vectorize(enable) interleave(enable)")
#else
#define DYN_VECTORIZE_LOOP
#endif

static JSValue js_string_ext_reverse(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSValue val;
    JSString *p, *str;
    int i;
    uint32_t len;
    (void)argc;
    (void)argv;
    val = JS_ToStringCheckObject(ctx, this_val);
    if (JS_IsException(val))
        return val;
    p = JS_VALUE_GET_STRING(val);
    len = p->len;
    if (len <= 1)
        return val;

    {
        uint32_t n = 0, k, pos = 0;
        uint32_t* starts = (uint32_t*)js_malloc(ctx, sizeof(uint32_t) * len);
        if (!starts) {
            JS_FreeValue(ctx, val);
            return JS_EXCEPTION;
        }
        i = 0;
        while (i < (int)len) {
            starts[n++] = (uint32_t)i;
            string_getc(p, &i);
        }
        str = js_alloc_string(ctx, len, p->is_wide_char);
        if (!str) {
            js_free(ctx, starts);
            JS_FreeValue(ctx, val);
            return JS_EXCEPTION;
        }
        if (p->is_wide_char) {
            const uint16_t* restrict src = js_str_data16(p);
            uint16_t* restrict dst = str->u.str16;
            for (k = n; k-- > 0;) {
                uint32_t end = (k + 1 < n) ? starts[k + 1] : len, j;
                for (j = starts[k]; j < end; j++)
                    dst[pos++] = src[j];
            }
        } else {
            const uint8_t* restrict src = js_str_data8(p);
            uint8_t* restrict dst = str->u.str8;
            for (k = n; k-- > 0;) {
                uint32_t end = (k + 1 < n) ? starts[k + 1] : len, j;
                for (j = starts[k]; j < end; j++)
                    dst[pos++] = src[j];
            }
            dst[len] = 0;
        }
        js_free(ctx, starts);
    }
    JS_FreeValue(ctx, val);
    return JS_MKPTR(JS_TAG_STRING, str);
}

static JSValue js_string_ext_insert(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSValue val, sval, ret = JS_EXCEPTION;
    JSString *p, *sp;
    StringBuffer b_s, *b = &b_s;
    int64_t idx, len;
    BOOL have_idx = (argc > 1 && !JS_IsUndefined(argv[1]));
    val = JS_ToStringCheckObject(ctx, this_val);
    if (JS_IsException(val))
        return val;
    sval = JS_ToString(ctx, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (JS_IsException(sval)) {
        JS_FreeValue(ctx, val);
        return JS_EXCEPTION;
    }
    len = JS_VALUE_GET_STRING(val)->len;
    idx = len;
    if (have_idx && JS_ToInt64Sat(ctx, &idx, argv[1])) {
        JS_FreeValue(ctx, sval);
        JS_FreeValue(ctx, val);
        return JS_EXCEPTION;
    }
    p = JS_VALUE_GET_STRING(val);
    sp = JS_VALUE_GET_STRING(sval);
    if (idx < 0)
        idx += len;
    if (idx < 0)
        idx = 0;
    if (idx > len)
        idx = len;
    if (string_buffer_init(ctx, b, p->len + sp->len))
        goto done;
    string_buffer_concat(b, p, 0, (uint32_t)idx);
    string_buffer_concat(b, sp, 0, sp->len);
    string_buffer_concat(b, p, (uint32_t)idx, (uint32_t)len);
    ret = string_buffer_end(b);
done:
    JS_FreeValue(ctx, sval);
    JS_FreeValue(ctx, val);
    return ret;
}

static JSValue js_string_ext_remove(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    JSValue val, sval, ret = JS_EXCEPTION;
    JSString *p, *sp;
    StringBuffer b_s, *b = &b_s;
    int pos, from, slen, plen;
    val = JS_ToStringCheckObject(ctx, this_val);
    if (JS_IsException(val))
        return val;
    sval = JS_ToString(ctx, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (JS_IsException(sval)) {
        JS_FreeValue(ctx, val);
        return JS_EXCEPTION;
    }
    p = JS_VALUE_GET_STRING(val);
    sp = JS_VALUE_GET_STRING(sval);
    plen = p->len;
    slen = sp->len;
    if (slen == 0 || (pos = string_indexof(p, sp, 0)) < 0) {
        ret = JS_DupValue(ctx, val);
        goto done;
    }
    if (string_buffer_init(ctx, b, plen))
        goto done;
    from = 0;
    for (;;) {
        string_buffer_concat(b, p, from, pos);
        from = pos + slen;
        if (magic == 0)
            break;
        pos = string_indexof(p, sp, from);
        if (pos < 0)
            break;
    }
    string_buffer_concat(b, p, from, plen);
    ret = string_buffer_end(b);
done:
    JS_FreeValue(ctx, sval);
    JS_FreeValue(ctx, val);
    return ret;
}

static int js_string_ext_ws_bytes(uint8_t set[16])
{
    int c, n = 0;
    for (c = 0; c < 256 && n < 16; c++)
        if (lre_is_space_byte((uint8_t)c))
            set[n++] = (uint8_t)c;
    return n;
}

static JSValue js_string_ext_compact(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSValue val, ret;
    JSString* p;
    StringBuffer b_s, *b = &b_s;
    uint32_t i, len;
    int wrote = 0;
    (void)argc;
    (void)argv;
    val = JS_ToStringCheckObject(ctx, this_val);
    if (JS_IsException(val))
        return val;
    p = JS_VALUE_GET_STRING(val);
    len = p->len;
    if (string_buffer_init(ctx, b, len)) {
        JS_FreeValue(ctx, val);
        return JS_EXCEPTION;
    }
    i = 0;
    if (!p->is_wide_char && len >= STRING_EXT_SIMD_MIN) {
        const uint8_t* s8 = js_str_data8(p);
        uint8_t wsset[16];
        int wsn = js_string_ext_ws_bytes(wsset);
        while (i < len) {
            uint32_t ws_start = i, run_end;
            size_t r;
            while (i < len && lre_is_space_byte(s8[i]))
                i++;
            if (i >= len)
                break;
            r = simd.find_first_of(s8 + i, (size_t)(len - i), wsset, (size_t)wsn);
            run_end = (r == SIZE_MAX) ? len : (uint32_t)(i + r);
            if (i > ws_start && wrote)
                string_buffer_putc8(b, ' ');
            string_buffer_concat(b, p, i, run_end);
            wrote = 1;
            i = run_end;
        }
    } else {
        while (i < len) {
            uint32_t ws_start = i, run_start;
            int had_ws;
            while (i < len && lre_is_space(string_get(p, i)))
                i++;
            had_ws = (i > ws_start);
            if (i >= len)
                break;
            run_start = i;
            while (i < len && !lre_is_space(string_get(p, i)))
                i++;
            if (had_ws && wrote)
                string_buffer_putc8(b, ' ');
            string_buffer_concat(b, p, run_start, i);
            wrote = 1;
        }
    }
    ret = string_buffer_end(b);
    JS_FreeValue(ctx, val);
    return ret;
}

static inline int js_str_upper_ascii(uint32_t c) { return c >= 'A' && c <= 'Z'; }
static inline int js_str_lower_ascii(uint32_t c) { return c >= 'a' && c <= 'z'; }
static inline int js_str_digit_ascii(uint32_t c) { return c >= '0' && c <= '9'; }
static inline int js_str_infl_delim(uint32_t c) { return c == '-' || c == '_' || lre_is_space(c); }

static JSValue js_string_ext_shift(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSValue val, ret;
    JSString* p;
    StringBuffer b_s, *b = &b_s;
    int32_t n = 0;
    uint32_t i, len;
    val = JS_ToStringCheckObject(ctx, this_val);
    if (JS_IsException(val))
        return val;
    if (argc > 0 && !JS_IsUndefined(argv[0]) && JS_ToInt32(ctx, &n, argv[0])) {
        JS_FreeValue(ctx, val);
        return JS_EXCEPTION;
    }
    p = JS_VALUE_GET_STRING(val);
    len = p->len;
    if (n == 0 || len == 0)
        return val;
    if (string_buffer_init2(ctx, b, len, 1)) {
        JS_FreeValue(ctx, val);
        return JS_EXCEPTION;
    }

    {
        int32_t k = n % 26;
        if (k < 0)
            k += 26;
        for (i = 0; i < len; i++) {
            uint32_t c = string_get(p, i);
            if (c >= 'a' && c <= 'z')
                c = (uint32_t)('a' + (int)((c - 'a') + k) % 26);
            else if (c >= 'A' && c <= 'Z')
                c = (uint32_t)('A' + (int)((c - 'A') + k) % 26);
            string_buffer_putc16(b, c);
        }
    }
    ret = string_buffer_end(b);
    JS_FreeValue(ctx, val);
    return ret;
}

static JSValue js_string_ext_pad(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSValue val, padv = JS_UNDEFINED, ret = JS_EXCEPTION;
    JSString *p, *pad = NULL;
    StringBuffer b_s, *b = &b_s;
    int32_t num = 0;
    uint32_t len, plen = 0, total, front, back, i;
    int wide, have_pad = (argc > 1 && !JS_IsUndefined(argv[1]));
    val = JS_ToStringCheckObject(ctx, this_val);
    if (JS_IsException(val))
        return val;
    if (argc > 0 && !JS_IsUndefined(argv[0]) && JS_ToInt32(ctx, &num, argv[0]))
        goto done;
    if (have_pad) {
        padv = JS_ToString(ctx, argv[1]);
        if (JS_IsException(padv))
            goto done;
        pad = JS_VALUE_GET_STRING(padv);
        plen = pad->len;
    }
    p = JS_VALUE_GET_STRING(val);
    len = p->len;
    if (num <= 0 || (uint32_t)num <= len || (have_pad && plen == 0)) {
        ret = JS_DupValue(ctx, val);
        goto done;
    }
    total = (uint32_t)num - len;
    front = total / 2;
    back = total - front;
    wide = p->is_wide_char || (pad && pad->is_wide_char);
    if (string_buffer_init2(ctx, b, num, wide))
        goto done;
    for (i = 0; i < front; i++)
        string_buffer_putc16(b, have_pad ? string_get(pad, i % plen) : ' ');
    string_buffer_concat(b, p, 0, len);
    for (i = 0; i < back; i++)
        string_buffer_putc16(b, have_pad ? string_get(pad, i % plen) : ' ');
    ret = string_buffer_end(b);
done:
    JS_FreeValue(ctx, padv);
    JS_FreeValue(ctx, val);
    return ret;
}

static int js_str_capitalize_boundary(uint32_t c)
{
    if (lre_is_space(c))
        return 1;
    if (c < 0x80) {
        if (js_str_lower_ascii(c) || js_str_upper_ascii(c) || js_str_digit_ascii(c) || c == '\'')
            return 0;
        return 1;
    }
    return 0;
}

static JSValue js_string_ext_capitalize(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSValue val;
    JSString* p;
    StringBuffer b_s, *b = &b_s;
    int lower, all, cap_next = 1, seen_first = 0, i, j, l;
    uint32_t res[LRE_CC_RES_LEN_MAX];
    val = JS_ToStringCheckObject(ctx, this_val);
    if (JS_IsException(val))
        return val;
    lower = (argc > 0) ? JS_ToBool(ctx, argv[0]) : 0;
    all = (argc > 1) ? JS_ToBool(ctx, argv[1]) : 0;
    p = JS_VALUE_GET_STRING(val);
    if (p->len == 0)
        return val;
    if (string_buffer_init(ctx, b, p->len)) {
        JS_FreeValue(ctx, val);
        return JS_EXCEPTION;
    }
    for (i = 0; i < (int)p->len;) {
        uint32_t c = string_getc(p, &i);
        int boundary = js_str_capitalize_boundary(c);
        int cap = 0;
        if (cap_next && !boundary) {
            cap = all ? 1 : !seen_first;
            seen_first = 1;
            cap_next = 0;
        }
        if (boundary)
            cap_next = 1;
        if (cap)
            l = lre_case_conv(res, c, 0);
        else if (lower)
            l = lre_case_conv(res, c, 1);
        else {
            res[0] = c;
            l = 1;
        }
        for (j = 0; j < l; j++)
            if (string_buffer_putc(b, res[j])) {
                string_buffer_free(b);
                JS_FreeValue(ctx, val);
                return JS_EXCEPTION;
            }
    }
    JS_FreeValue(ctx, val);
    return string_buffer_end(b);
}

static int js_string_infl_into(JSContext* ctx, StringBuffer* b, JSString* p, int sep)
{
    int i = 0, len = (int)p->len, prev_cls = 0, emitted_sep = 0, m, l;
    uint32_t res[LRE_CC_RES_LEN_MAX];
    while (i < len) {
        uint32_t c = string_getc(p, &i);
        int cls = js_str_upper_ascii(c) ? 2 : (js_str_lower_ascii(c) || js_str_digit_ascii(c)) ? 1
                                                                                               : 0;
        if (js_str_infl_delim(c)) {
            if (!emitted_sep) {
                if (string_buffer_putc8(b, (uint8_t)sep))
                    return -1;
                emitted_sep = 1;
            }
            prev_cls = 0;
            continue;
        }
        if (cls == 2) {
            int hump = (prev_cls == 1);
            if (!hump && prev_cls == 2) {
                int k = i;
                uint32_t nx = (k < len) ? string_getc(p, &k) : 0;
                hump = js_str_lower_ascii(nx);
            }
            if (hump && !emitted_sep) {
                if (string_buffer_putc8(b, (uint8_t)sep))
                    return -1;
            }
        }
        l = lre_case_conv(res, c, 1);
        for (m = 0; m < l; m++)
            if (string_buffer_putc(b, res[m]))
                return -1;
        emitted_sep = 0;
        prev_cls = cls;
    }
    return 0;
}

static JSValue js_string_ext_inflect(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int sep)
{
    JSValue val, ret;
    JSString* p;
    StringBuffer b_s, *b = &b_s;
    (void)argc;
    (void)argv;
    val = JS_ToStringCheckObject(ctx, this_val);
    if (JS_IsException(val))
        return val;
    p = JS_VALUE_GET_STRING(val);
    if (p->len == 0)
        return val;
    if (string_buffer_init(ctx, b, p->len)) {
        JS_FreeValue(ctx, val);
        return JS_EXCEPTION;
    }
    if (js_string_infl_into(ctx, b, p, sep)) {
        string_buffer_free(b);
        JS_FreeValue(ctx, val);
        return JS_EXCEPTION;
    }
    ret = string_buffer_end(b);
    JS_FreeValue(ctx, val);
    return ret;
}

static JSValue js_string_ext_camelize(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSValue val;
    JSString* p;
    StringBuffer b_s, *b = &b_s;
    int i = 0, len, prev_cls = 0, cap_next, m, l;
    int upper = (argc > 0 && !JS_IsUndefined(argv[0])) ? JS_ToBool(ctx, argv[0]) : 1;
    uint32_t res[LRE_CC_RES_LEN_MAX];
    val = JS_ToStringCheckObject(ctx, this_val);
    if (JS_IsException(val))
        return val;
    p = JS_VALUE_GET_STRING(val);
    len = (int)p->len;
    if (len == 0)
        return val;
    if (string_buffer_init(ctx, b, len)) {
        JS_FreeValue(ctx, val);
        return JS_EXCEPTION;
    }
    cap_next = upper;
    while (i < len) {
        uint32_t c = string_getc(p, &i);
        int cls = js_str_upper_ascii(c) ? 2 : (js_str_lower_ascii(c) || js_str_digit_ascii(c)) ? 1
                                                                                               : 0;
        if (js_str_infl_delim(c)) {
            cap_next = 1;
            prev_cls = 0;
            continue;
        }
        if (cls == 2) {
            int hump = (prev_cls == 1);
            if (!hump && prev_cls == 2) {
                int k = i;
                uint32_t nx = (k < len) ? string_getc(p, &k) : 0;
                hump = js_str_lower_ascii(nx);
            }
            if (hump)
                cap_next = 1;
        }
        l = lre_case_conv(res, c, cap_next ? 0 : 1);
        for (m = 0; m < l; m++)
            if (string_buffer_putc(b, res[m])) {
                string_buffer_free(b);
                JS_FreeValue(ctx, val);
                return JS_EXCEPTION;
            }
        cap_next = 0;
        prev_cls = cls;
    }
    JS_FreeValue(ctx, val);
    return string_buffer_end(b);
}

static uint32_t js_str_word_prefix_cut(JSString* p, uint32_t want)
{
    uint32_t cut = want;
    if (cut < p->len && !lre_is_space(string_get(p, cut)) && cut > 0 && !lre_is_space(string_get(p, cut - 1)))
        while (cut > 0 && !lre_is_space(string_get(p, cut - 1)))
            cut--;
    while (cut > 0 && lre_is_space(string_get(p, cut - 1)))
        cut--;
    return cut;
}

static uint32_t js_str_word_suffix_start(JSString* p, uint32_t want)
{
    uint32_t len = p->len, start = (want >= len) ? 0 : len - want;
    if (start > 0 && !lre_is_space(string_get(p, start)) && !lre_is_space(string_get(p, start - 1)))
        while (start < len && !lre_is_space(string_get(p, start)))
            start++;
    while (start < len && lre_is_space(string_get(p, start)))
        start++;
    return start;
}

static JSValue js_string_ext_truncate(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    JSValue val, ellv = JS_UNDEFINED, ret = JS_EXCEPTION;
    JSString *p, *ell = NULL;
    StringBuffer b_s, *b = &b_s;
    const char* from = NULL;
    int32_t length = 0;
    int on_word = magic & 1, mode;
    uint32_t len, want, front, back, fc, ss;
    val = JS_ToStringCheckObject(ctx, this_val);
    if (JS_IsException(val))
        return val;
    if (argc > 0 && !JS_IsUndefined(argv[0]) && JS_ToInt32(ctx, &length, argv[0]))
        goto done;
    if (argc > 1 && !JS_IsUndefined(argv[1])) {
        from = JS_ToCString(ctx, argv[1]);
        if (!from)
            goto done;
    }
    if (argc > 2 && !JS_IsUndefined(argv[2])) {
        ellv = JS_ToString(ctx, argv[2]);
        if (JS_IsException(ellv))
            goto done;
    } else {
        ellv = js_new_string8_len(ctx, "...", 3);
        if (JS_IsException(ellv))
            goto done;
    }
    mode = (from && !strcmp(from, "left")) ? 1 : (from && !strcmp(from, "middle")) ? 2
                                                                                   : 0;
    p = JS_VALUE_GET_STRING(val);
    ell = JS_VALUE_GET_STRING(ellv);
    len = p->len;
    if (length < 0)
        length = 0;
    want = (uint32_t)length;
    if (len <= want) {
        ret = JS_DupValue(ctx, val);
        goto done;
    }
    if (string_buffer_init2(ctx, b, want + ell->len, p->is_wide_char || ell->is_wide_char))
        goto done;
    switch (mode) {
    case 1:
        ss = on_word ? js_str_word_suffix_start(p, want) : len - want;
        string_buffer_concat(b, ell, 0, ell->len);
        string_buffer_concat(b, p, ss, len);
        break;
    case 2:
        front = (want + 1) / 2;
        back = want - front;
        fc = on_word ? js_str_word_prefix_cut(p, front) : front;
        ss = on_word ? js_str_word_suffix_start(p, back) : len - back;
        string_buffer_concat(b, p, 0, fc);
        string_buffer_concat(b, ell, 0, ell->len);
        string_buffer_concat(b, p, ss, len);
        break;
    default:
        fc = on_word ? js_str_word_prefix_cut(p, want) : want;
        string_buffer_concat(b, p, 0, fc);
        string_buffer_concat(b, ell, 0, ell->len);
        break;
    }
    ret = string_buffer_end(b);
done:
    if (from)
        JS_FreeCString(ctx, from);
    JS_FreeValue(ctx, ellv);
    JS_FreeValue(ctx, val);
    return ret;
}

static int sb_put_ascii(StringBuffer* b, const char* s)
{
    while (*s)
        if (string_buffer_putc8(b, (uint8_t)*s++))
            return -1;
    return 0;
}

static JSValue js_string_ext_escapeHTML(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSValue val, ret;
    JSString* p;
    StringBuffer b_s, *b = &b_s;
    uint32_t i, len;
    static const uint8_t set[3] = { '&', '<', '>' };
    (void)argc;
    (void)argv;
    val = JS_ToStringCheckObject(ctx, this_val);
    if (JS_IsException(val))
        return val;
    p = JS_VALUE_GET_STRING(val);
    len = p->len;
    if (string_buffer_init(ctx, b, len)) {
        JS_FreeValue(ctx, val);
        return JS_EXCEPTION;
    }
    i = 0;
    if (!p->is_wide_char && len >= STRING_EXT_SIMD_MIN) {
        const uint8_t* s8 = js_str_data8(p);
        while (i < len) {
            size_t r = simd.find_first_of(s8 + i, (size_t)(len - i), set, 3);
            uint32_t run_end = (r == SIZE_MAX) ? len : (uint32_t)(i + r);
            string_buffer_concat(b, p, i, run_end);
            if (run_end >= len)
                break;
            switch (s8[run_end]) {
            case '&':
                if (sb_put_ascii(b, "&amp;"))
                    goto fail;
                break;
            case '<':
                if (sb_put_ascii(b, "&lt;"))
                    goto fail;
                break;
            default:
                if (sb_put_ascii(b, "&gt;"))
                    goto fail;
                break;
            }
            i = run_end + 1;
        }
    } else {
        for (i = 0; i < len; i++) {
            uint32_t c = string_get(p, i);
            if (c == '&') {
                if (sb_put_ascii(b, "&amp;"))
                    goto fail;
            } else if (c == '<') {
                if (sb_put_ascii(b, "&lt;"))
                    goto fail;
            } else if (c == '>') {
                if (sb_put_ascii(b, "&gt;"))
                    goto fail;
            } else if (string_buffer_putc16(b, (uint16_t)c))
                goto fail;
        }
    }
    ret = string_buffer_end(b);
    JS_FreeValue(ctx, val);
    return ret;
fail:
    string_buffer_free(b);
    JS_FreeValue(ctx, val);
    return JS_EXCEPTION;
}

static uint32_t js_str_decode_entity(JSString* p, uint32_t start, uint32_t len, uint32_t* cp)
{
    uint32_t i = start + 1, semi;
    if (i >= len)
        return start;
    if (string_get(p, i) == '#') {
        uint32_t v = 0, hex = 0, ndigits = 0;
        i++;
        if (i < len && (string_get(p, i) == 'x' || string_get(p, i) == 'X')) {
            hex = 1;
            i++;
        }
        for (; i < len; i++) {
            uint32_t d = string_get(p, i), dv;
            if (d >= '0' && d <= '9')
                dv = d - '0';
            else if (hex && d >= 'a' && d <= 'f')
                dv = d - 'a' + 10;
            else if (hex && d >= 'A' && d <= 'F')
                dv = d - 'A' + 10;
            else
                break;
            v = v * (hex ? 16 : 10) + dv;
            ndigits++;
            if (v > 0x10FFFF)
                v = 0xFFFD;
        }
        if (!ndigits || i >= len || string_get(p, i) != ';')
            return start;
        *cp = v;
        return i + 1;
    }
    for (semi = i; semi < len && semi < i + 10; semi++)
        if (string_get(p, semi) == ';')
            break;
    if (semi >= len || string_get(p, semi) != ';')
        return start;
    {
        static const struct {
            const char* name;
            uint32_t cp;
        } ents[] = {
            { "lt", '<' },
            { "gt", '>' },
            { "amp", '&' },
            { "nbsp", ' ' },
            { "quot", '"' },
            { "apos", '\'' },
        };
        uint32_t nlen = semi - i, k;
        for (k = 0; k < countof(ents); k++) {
            uint32_t m, en = 0;
            const char* nm = ents[k].name;
            while (nm[en])
                en++;
            if (en != nlen)
                continue;
            for (m = 0; m < nlen; m++)
                if (string_get(p, i + m) != (uint8_t)nm[m])
                    break;
            if (m == nlen) {
                *cp = ents[k].cp;
                return semi + 1;
            }
        }
    }
    return start;
}

static JSValue js_string_ext_unescapeHTML(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSValue val, ret;
    JSString* p;
    StringBuffer b_s, *b = &b_s;
    uint32_t i, len;
    int simd_path;
    (void)argc;
    (void)argv;
    val = JS_ToStringCheckObject(ctx, this_val);
    if (JS_IsException(val))
        return val;
    p = JS_VALUE_GET_STRING(val);
    len = p->len;
    if (string_buffer_init(ctx, b, len)) {
        JS_FreeValue(ctx, val);
        return JS_EXCEPTION;
    }
    simd_path = (!p->is_wide_char && len >= STRING_EXT_SIMD_MIN);
    i = 0;
    while (i < len) {
        uint32_t amp, cp, next;
        if (simd_path) {
            size_t r = simd.find_u8(js_str_data8(p) + i, (uint8_t)'&', (size_t)(len - i));
            amp = (r == SIZE_MAX) ? len : (uint32_t)(i + r);
        } else {
            for (amp = i; amp < len && string_get(p, amp) != '&'; amp++)
                ;
        }
        string_buffer_concat(b, p, i, amp);
        if (amp >= len)
            break;
        next = js_str_decode_entity(p, amp, len, &cp);
        if (next == amp) {
            if (string_buffer_putc8(b, '&'))
                goto fail;
            i = amp + 1;
        } else {
            if (string_buffer_putc(b, cp))
                goto fail;
            i = next;
        }
    }
    ret = string_buffer_end(b);
    JS_FreeValue(ctx, val);
    return ret;
fail:
    string_buffer_free(b);
    JS_FreeValue(ctx, val);
    return JS_EXCEPTION;
}

static JSValue js_string_ext_stripTags(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSValue val, ret;
    JSString* p;
    StringBuffer b_s, *b = &b_s;
    uint32_t i, len;
    int simd_path;
    (void)argc;
    (void)argv;
    val = JS_ToStringCheckObject(ctx, this_val);
    if (JS_IsException(val))
        return val;
    p = JS_VALUE_GET_STRING(val);
    len = p->len;
    if (string_buffer_init(ctx, b, len)) {
        JS_FreeValue(ctx, val);
        return JS_EXCEPTION;
    }
    simd_path = (!p->is_wide_char && len >= STRING_EXT_SIMD_MIN);
    i = 0;
    while (i < len) {
        uint32_t lt, gt;
        if (simd_path) {
            size_t r = simd.find_u8(js_str_data8(p) + i, (uint8_t)'<', (size_t)(len - i));
            lt = (r == SIZE_MAX) ? len : (uint32_t)(i + r);
        } else {
            for (lt = i; lt < len && string_get(p, lt) != '<'; lt++)
                ;
        }
        string_buffer_concat(b, p, i, lt);
        if (lt >= len)
            break;
        for (gt = lt + 2; gt < len && string_get(p, gt) != '>'; gt++)
            ;
        if (gt >= len) {
            string_buffer_concat(b, p, lt, len);
            break;
        }
        i = gt + 1;
    }
    ret = string_buffer_end(b);
    JS_FreeValue(ctx, val);
    return ret;
}

static JSValue js_global_encodeURI(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic);
static JSValue js_global_decodeURI(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic);

static JSValue js_string_ext_escapeURL(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSValue val, ret;
    int magic = ((argc > 0) && JS_ToBool(ctx, argv[0])) ? 1 : 0;
    val = JS_ToStringCheckObject(ctx, this_val);
    if (JS_IsException(val))
        return val;
    ret = js_global_encodeURI(ctx, JS_UNDEFINED, 1, (JSValueConst*)&val, magic);
    JS_FreeValue(ctx, val);
    return ret;
}

static JSValue js_string_ext_unescapeURL(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSValue val, ret;
    int magic = ((argc > 0) && JS_ToBool(ctx, argv[0])) ? 0 : 1;
    val = JS_ToStringCheckObject(ctx, this_val);
    if (JS_IsException(val))
        return val;
    ret = js_global_decodeURI(ctx, JS_UNDEFINED, 1, (JSValueConst*)&val, magic);
    JS_FreeValue(ctx, val);
    return ret;
}

static JSValue js_string_ext_words(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSValue val, result, ret = JS_EXCEPTION;
    JSString* p;
    JSValue* dst;
    uint32_t i, len, idx, nwords = 0;
    (void)argc;
    (void)argv;
    val = JS_ToStringCheckObject(ctx, this_val);
    if (JS_IsException(val))
        return val;
    p = JS_VALUE_GET_STRING(val);
    len = p->len;
    for (i = 0; i < len;) {
        while (i < len && lre_is_space(string_get(p, i)))
            i++;
        if (i >= len)
            break;
        nwords++;
        while (i < len && !lre_is_space(string_get(p, i)))
            i++;
    }
    if (nwords == 0) {
        ret = JS_NewArray(ctx);
        goto done;
    }
    result = js_allocate_fast_array(ctx, nwords);
    if (JS_IsException(result))
        goto done;
    dst = JS_VALUE_GET_OBJ(result)->u.array.u.values;
    idx = 0;
    i = 0;
    while (idx < nwords) {
        uint32_t ws;
        while (i < len && lre_is_space(string_get(p, i)))
            i++;
        ws = i;
        while (i < len && !lre_is_space(string_get(p, i)))
            i++;
        dst[idx] = js_sub_string(ctx, p, (int)ws, (int)i);
        if (JS_IsException(dst[idx])) {
            JS_FreeValue(ctx, result);
            goto done;
        }
        idx++;
    }
    ret = result;
done:
    JS_FreeValue(ctx, val);
    return ret;
}

static JSValue js_string_ext_lines(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSValue val, result, ret = JS_EXCEPTION;
    JSString* p;
    JSValue* dst;
    uint32_t start, end, i, seg, idx, nlines, nl_count = 0;
    int narrow_long;
    (void)argc;
    (void)argv;
    val = JS_ToStringCheckObject(ctx, this_val);
    if (JS_IsException(val))
        return val;
    p = JS_VALUE_GET_STRING(val);
    start = 0;
    end = p->len;
    while (start < end && lre_is_space(string_get(p, start)))
        start++;
    while (end > start && lre_is_space(string_get(p, end - 1)))
        end--;
    narrow_long = (!p->is_wide_char && (end - start) >= STRING_EXT_SIMD_MIN);
    if (narrow_long)
        nl_count = (uint32_t)simd.count_u8(js_str_data8(p) + start, (uint8_t)'\n', (size_t)(end - start));
    else
        for (i = start; i < end; i++)
            if (string_get(p, i) == '\n')
                nl_count++;
    nlines = nl_count + 1;
    result = js_allocate_fast_array(ctx, nlines);
    if (JS_IsException(result))
        goto done;
    dst = JS_VALUE_GET_OBJ(result)->u.array.u.values;
    seg = start;
    idx = 0;
    i = start;
    while (idx < nlines) {
        uint32_t nl, le;
        if (narrow_long && (end - i) >= STRING_EXT_SIMD_MIN) {
            size_t r = simd.find_u8(js_str_data8(p) + i, (uint8_t)'\n', (size_t)(end - i));
            nl = (r == SIZE_MAX) ? end : (uint32_t)(i + r);
        } else {
            for (nl = i; nl < end && string_get(p, nl) != '\n'; nl++)
                ;
        }
        le = nl;
        if (le > seg && string_get(p, le - 1) == '\r')
            le--;
        dst[idx] = js_sub_string(ctx, p, (int)seg, (int)le);
        if (JS_IsException(dst[idx])) {
            JS_FreeValue(ctx, result);
            goto done;
        }
        idx++;
        if (nl >= end)
            break;
        i = nl + 1;
        seg = i;
    }
    ret = result;
done:
    JS_FreeValue(ctx, val);
    return ret;
}

static JSValue js_string_ext_encodeBase64(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSValue val, ret = JS_EXCEPTION;
    const char* utf8;
    char* out;
    size_t ulen, outlen;
    (void)argc;
    (void)argv;
    val = JS_ToStringCheckObject(ctx, this_val);
    if (JS_IsException(val))
        return val;
    utf8 = JS_ToCStringLen(ctx, &ulen, val);
    if (!utf8) {
        JS_FreeValue(ctx, val);
        return JS_EXCEPTION;
    }
    out = js_malloc(ctx, 4 * ((ulen + 2) / 3) + 1);
    if (!out)
        goto done;
    outlen = simd.base64_encode((const uint8_t*)utf8, ulen, out);
    ret = js_new_string8_len(ctx, out, (int)outlen);
    js_free(ctx, out);
done:
    JS_FreeCString(ctx, utf8);
    JS_FreeValue(ctx, val);
    return ret;
}

static JSValue js_string_ext_decodeBase64(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSValue val, ret = JS_EXCEPTION;
    const char* src;
    uint8_t* out;
    size_t slen, outlen;
    (void)argc;
    (void)argv;
    val = JS_ToStringCheckObject(ctx, this_val);
    if (JS_IsException(val))
        return val;
    src = JS_ToCStringLen(ctx, &slen, val);
    if (!src) {
        JS_FreeValue(ctx, val);
        return JS_EXCEPTION;
    }
    out = js_malloc(ctx, 3 * (slen / 4) + 4);
    if (!out)
        goto done;
    outlen = simd.base64_decode(src, slen, out);
    if (outlen == (size_t)-1) {
        js_free(ctx, out);
        JS_ThrowTypeError(ctx, "decodeBase64: invalid base64 input");
        goto done;
    }
    ret = JS_NewStringLen(ctx, (const char*)out, outlen);
    js_free(ctx, out);
done:
    JS_FreeCString(ctx, src);
    JS_FreeValue(ctx, val);
    return ret;
}

static inline uint32_t js_str_cu(JSString* p, uint32_t i)
{
    return p->is_wide_char ? js_str_data16(p)[i] : js_str_data8(p)[i];
}
static inline int js_str_is_alnum_ascii(uint32_t c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static JSValue js_string_ext_count(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSValue val, sub;
    JSString *p, *sp;
    uint32_t i, plen, slen;
    int64_t n = 0;
    val = JS_ToStringCheckObject(ctx, this_val);
    if (JS_IsException(val))
        return val;
    sub = JS_ToString(ctx, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (JS_IsException(sub)) {
        JS_FreeValue(ctx, val);
        return JS_EXCEPTION;
    }
    p = JS_VALUE_GET_STRING(val);
    sp = JS_VALUE_GET_STRING(sub);
    plen = p->len;
    slen = sp->len;
    if (slen == 0 || slen > plen)
        goto done;
    if (slen == 1 && !p->is_wide_char && !sp->is_wide_char) {
        n = (int64_t)simd.count_u8(js_str_data8(p), js_str_data8(sp)[0], plen);
        goto done;
    }
    i = 0;
    while (i + slen <= plen) {
        uint32_t k = 0;
        while (k < slen && js_str_cu(p, i + k) == js_str_cu(sp, k))
            k++;
        if (k == slen) {
            n++;
            i += slen;
        } else
            i++;
    }
done:
    JS_FreeValue(ctx, sub);
    JS_FreeValue(ctx, val);
    return JS_NewInt64(ctx, n);
}

static JSValue js_string_ext_toNumber(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSValue val;
    const char* s;
    char* end;
    int base = 10;
    double d;
    val = JS_ToStringCheckObject(ctx, this_val);
    if (JS_IsException(val))
        return val;
    if (argc > 0 && !JS_IsUndefined(argv[0]) && JS_ToInt32Sat(ctx, &base, argv[0])) {
        JS_FreeValue(ctx, val);
        return JS_EXCEPTION;
    }
    if (base != 10 && (base < 2 || base > 36)) {
        JS_FreeValue(ctx, val);
        return JS_ThrowRangeError(ctx, "base must be 2..36");
    }
    s = JS_ToCString(ctx, val);
    JS_FreeValue(ctx, val);
    if (!s)
        return JS_EXCEPTION;
    if (base == 10) {
        d = strtod(s, &end);
        if (end == s)
            d = DYN_NAN;
    } else {
        long long ll = strtoll(s, &end, base);
        d = (end == s) ? DYN_NAN : (double)ll;
    }
    JS_FreeCString(ctx, s);
    return JS_NewFloat64(ctx, d);
}

static JSValue js_string_ext_humanize(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSValue val, ret;
    JSString* p;
    StringBuffer b_s, *b = &b_s;
    uint32_t len, i;
    int first = 1, m, l;
    uint32_t res[LRE_CC_RES_LEN_MAX];
    (void)argc;
    (void)argv;
    val = JS_ToStringCheckObject(ctx, this_val);
    if (JS_IsException(val))
        return val;
    p = JS_VALUE_GET_STRING(val);
    len = p->len;
    if (len >= 3 && js_str_cu(p, len - 3) == '_' && js_str_cu(p, len - 2) == 'i' && js_str_cu(p, len - 1) == 'd')
        len -= 3;
    if (string_buffer_init(ctx, b, len)) {
        JS_FreeValue(ctx, val);
        return JS_EXCEPTION;
    }
    for (i = 0; i < len; i++) {
        uint32_t c = js_str_cu(p, i);
        if (c == '_' || c == '-') {
            if (string_buffer_putc8(b, ' '))
                goto fail;
            continue;
        }
        if (first && js_str_is_alnum_ascii(c)) {
            l = lre_case_conv(res, c, 0);
            for (m = 0; m < l; m++)
                if (string_buffer_putc(b, res[m]))
                    goto fail;
            first = 0;
        } else {
            if (string_buffer_putc(b, c))
                goto fail;
            if (js_str_is_alnum_ascii(c))
                first = 0;
        }
    }
    ret = string_buffer_end(b);
    JS_FreeValue(ctx, val);
    return ret;
fail:
    string_buffer_free(b);
    JS_FreeValue(ctx, val);
    return JS_EXCEPTION;
}

static JSValue js_string_ext_parameterize(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSValue val, ret;
    JSString* p;
    StringBuffer b_s, *b = &b_s;
    uint32_t len, i;
    int started = 0, pending = 0, m, l;
    uint32_t res[LRE_CC_RES_LEN_MAX];
    (void)argc;
    (void)argv;
    val = JS_ToStringCheckObject(ctx, this_val);
    if (JS_IsException(val))
        return val;
    p = JS_VALUE_GET_STRING(val);
    len = p->len;
    if (string_buffer_init(ctx, b, len)) {
        JS_FreeValue(ctx, val);
        return JS_EXCEPTION;
    }
    for (i = 0; i < len; i++) {
        uint32_t c = js_str_cu(p, i);
        if (js_str_is_alnum_ascii(c)) {
            if (pending && started) {
                if (string_buffer_putc8(b, '-'))
                    goto fail;
            }
            pending = 0;
            l = lre_case_conv(res, c, 1);
            for (m = 0; m < l; m++)
                if (string_buffer_putc(b, res[m]))
                    goto fail;
            started = 1;
        } else {
            pending = 1;
        }
    }
    ret = string_buffer_end(b);
    JS_FreeValue(ctx, val);
    return ret;
fail:
    string_buffer_free(b);
    JS_FreeValue(ctx, val);
    return JS_EXCEPTION;
}

static JSValue js_string_ext_titleize(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    static const char* const stops[] = {
        "a",
        "an",
        "and",
        "as",
        "at",
        "but",
        "by",
        "en",
        "for",
        "from",
        "if",
        "in",
        "into",
        "nor",
        "of",
        "on",
        "onto",
        "or",
        "over",
        "per",
        "the",
        "to",
        "v",
        "via",
        "vs",
        "with",
    };
    JSValue val, ret;
    JSString* p;
    StringBuffer b_s, *b = &b_s;
    uint32_t len, i;
    int word_index = 0;
    (void)argc;
    (void)argv;
    val = JS_ToStringCheckObject(ctx, this_val);
    if (JS_IsException(val))
        return val;
    p = JS_VALUE_GET_STRING(val);
    len = p->len;
    if (string_buffer_init(ctx, b, len)) {
        JS_FreeValue(ctx, val);
        return JS_EXCEPTION;
    }
    i = 0;
    while (i < len) {
        uint32_t c = js_str_cu(p, i);
        if (c == ' ' || c == '_' || c == '-' || c == '\t' || c == '\n') {
            if (string_buffer_putc8(b, ' '))
                goto fail;
            i++;
            continue;
        }
        uint32_t j = i;
        char lc[32];
        int lcn = 0;
        while (j < len) {
            uint32_t wc = js_str_cu(p, j);
            if (wc == ' ' || wc == '_' || wc == '-' || wc == '\t' || wc == '\n')
                break;
            if (lcn < (int)sizeof(lc) - 1 && wc >= 'A' && wc <= 'Z')
                lc[lcn++] = (char)(wc + 32);
            else if (lcn < (int)sizeof(lc) - 1)
                lc[lcn++] = (char)(wc < 128 ? wc : '?');
            j++;
        }
        lc[lcn] = 0;
        int is_stop = 0;
        if (word_index > 0) {
            unsigned k;
            for (k = 0; k < countof(stops); k++)
                if (!strcmp(lc, stops[k])) {
                    is_stop = 1;
                    break;
                }
        }
        {
            uint32_t k;
            uint32_t res[LRE_CC_RES_LEN_MAX];
            int m, l;
            for (k = i; k < j; k++) {
                uint32_t wc = js_str_cu(p, k);
                int upper = (!is_stop && k == i);
                l = lre_case_conv(res, wc, upper ? 0 : 1);
                for (m = 0; m < l; m++)
                    if (string_buffer_putc(b, res[m]))
                        goto fail;
            }
        }
        word_index++;
        i = j;
    }
    ret = string_buffer_end(b);
    JS_FreeValue(ctx, val);
    return ret;
fail:
    string_buffer_free(b);
    JS_FreeValue(ctx, val);
    return JS_EXCEPTION;
}

static const char* const js_infl_uncountable[] = {
    "sheep",
    "fish",
    "series",
    "species",
    "deer",
    "money",
    "information",
    "equipment",
    "rice",
    "news",
};
static const struct {
    const char *sing, *plur;
} js_infl_irregular[] = {
    { "man", "men" },
    { "woman", "women" },
    { "child", "children" },
    { "person", "people" },
    { "foot", "feet" },
    { "tooth", "teeth" },
    { "goose", "geese" },
    { "mouse", "mice" },
    { "ox", "oxen" },
    { "leaf", "leaves" },
    { "life", "lives" },
    { "knife", "knives" },
    { "half", "halves" },
    { "wife", "wives" },
    { "self", "selves" },
};

static int js_str_ends(const char* s, size_t n, const char* suf)
{
    size_t sl = strlen(suf);
    return n >= sl && memcmp(s + n - sl, suf, sl) == 0;
}

static JSValue js_string_ext_inflect_num(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    JSValue val, ret;
    const char* s;
    size_t n, i;
    char *lc = NULL, *out = NULL;
    unsigned k;
    (void)argc;
    (void)argv;
    val = JS_ToStringCheckObject(ctx, this_val);
    if (JS_IsException(val))
        return val;
    s = JS_ToCString(ctx, val);
    JS_FreeValue(ctx, val);
    if (!s)
        return JS_EXCEPTION;
    n = strlen(s);
    lc = js_malloc(ctx, n + 1);
    if (!lc) {
        JS_FreeCString(ctx, s);
        return JS_EXCEPTION;
    }
    for (i = 0; i < n; i++)
        lc[i] = (s[i] >= 'A' && s[i] <= 'Z') ? s[i] + 32 : s[i];
    lc[n] = 0;
    for (k = 0; k < countof(js_infl_uncountable); k++)
        if (!strcmp(lc, js_infl_uncountable[k])) {
            ret = js_new_string8(ctx, s);
            goto done;
        }
    for (k = 0; k < countof(js_infl_irregular); k++) {
        const char* from = magic ? js_infl_irregular[k].plur : js_infl_irregular[k].sing;
        const char* to = magic ? js_infl_irregular[k].sing : js_infl_irregular[k].plur;
        if (!strcmp(lc, from)) {
            ret = js_new_string8(ctx, to);
            goto done;
        }
    }
    out = js_malloc(ctx, n + 4);
    if (!out) {
        ret = JS_EXCEPTION;
        goto done;
    }
    if (magic == 0) {
        if (n >= 2 && js_str_ends(lc, n, "y") && !strchr("aeiou", lc[n - 2])) {
            memcpy(out, s, n - 1);
            memcpy(out + n - 1, "ies", 3);
            out[n + 2] = 0;
        } else if (js_str_ends(lc, n, "s") || js_str_ends(lc, n, "x") || js_str_ends(lc, n, "z") || js_str_ends(lc, n, "ch") || js_str_ends(lc, n, "sh")) {
            memcpy(out, s, n);
            memcpy(out + n, "es", 2);
            out[n + 2] = 0;
        } else {
            memcpy(out, s, n);
            out[n] = 's';
            out[n + 1] = 0;
        }
    } else {
        if (js_str_ends(lc, n, "ies") && n > 3) {
            memcpy(out, s, n - 3);
            out[n - 3] = 'y';
            out[n - 2] = 0;
        } else if ((js_str_ends(lc, n, "ches") || js_str_ends(lc, n, "shes") || js_str_ends(lc, n, "xes") || js_str_ends(lc, n, "zes") || js_str_ends(lc, n, "sses"))) {
            memcpy(out, s, n - 2);
            out[n - 2] = 0;
        } else if (js_str_ends(lc, n, "s") && !js_str_ends(lc, n, "ss") && n > 1) {
            memcpy(out, s, n - 1);
            out[n - 1] = 0;
        } else {
            memcpy(out, s, n);
            out[n] = 0;
        }
    }
    ret = JS_NewString(ctx, out);
done:
    js_free(ctx, out);
    js_free(ctx, lc);
    JS_FreeCString(ctx, s);
    return ret;
}

static JSValue js_string_ext_removeTags(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSValue val, nameval = JS_UNDEFINED, ret;
    JSString *p, *np = NULL;
    StringBuffer b_s, *b = &b_s;
    uint32_t len, i;
    BOOL have_name = (argc > 0 && !JS_IsUndefined(argv[0]));
    val = JS_ToStringCheckObject(ctx, this_val);
    if (JS_IsException(val))
        return val;
    if (have_name) {
        nameval = JS_ToString(ctx, argv[0]);
        if (JS_IsException(nameval)) {
            JS_FreeValue(ctx, val);
            return JS_EXCEPTION;
        }
        np = JS_VALUE_GET_STRING(nameval);
    }
    p = JS_VALUE_GET_STRING(val);
    len = p->len;
    if (string_buffer_init(ctx, b, len))
        goto fail;
    i = 0;
    while (i < len) {
        uint32_t c = js_str_cu(p, i);
        if (c == '<' && i + 1 < len && (js_str_cu(p, i + 1) == '/' || ((js_str_cu(p, i + 1) | 32) >= 'a' && (js_str_cu(p, i + 1) | 32) <= 'z'))) {
            uint32_t j = i + 1, ns, ne, k;
            int closing = (js_str_cu(p, j) == '/');
            if (closing)
                j++;
            ns = j;
            while (j < len && js_str_is_alnum_ascii(js_str_cu(p, j)))
                j++;
            ne = j;
            while (j < len && js_str_cu(p, j) != '>')
                j++;
            uint32_t tagend = (j < len) ? j + 1 : len;
            int match = 1;
            if (have_name) {
                if (ne - ns != np->len)
                    match = 0;
                else
                    for (k = 0; k < np->len; k++)
                        if ((js_str_cu(p, ns + k) | 32) != (js_str_cu(np, k) | 32)) {
                            match = 0;
                            break;
                        }
            }
            if (!closing && match) {
                uint32_t nlen = ne - ns, s2 = tagend;
                while (s2 < len) {
                    if (js_str_cu(p, s2) == '<' && s2 + 1 < len && js_str_cu(p, s2 + 1) == '/') {
                        uint32_t m = s2 + 2, q;
                        int same = 1;
                        for (q = 0; q < nlen; q++)
                            if (m + q >= len || (js_str_cu(p, m + q) | 32) != (js_str_cu(p, ns + q) | 32)) {
                                same = 0;
                                break;
                            }
                        if (same) {
                            uint32_t after = m + nlen;
                            while (after < len && js_str_cu(p, after) != '>')
                                after++;
                            i = (after < len) ? after + 1 : len;
                            goto next;
                        }
                    }
                    s2++;
                }
                i = len;
                goto next;
            }
            if (!have_name) {
                i = tagend;
                goto next;
            }
            for (k = i; k < tagend; k++)
                if (string_buffer_putc(b, js_str_cu(p, k)))
                    goto fail;
            i = tagend;
            goto next;
        }
        if (string_buffer_putc(b, c))
            goto fail;
        i++;
    next:;
    }
    ret = string_buffer_end(b);
    JS_FreeValue(ctx, nameval);
    JS_FreeValue(ctx, val);
    return ret;
fail:
    string_buffer_free(b);
    JS_FreeValue(ctx, nameval);
    JS_FreeValue(ctx, val);
    return JS_EXCEPTION;
}

static JSValue js_string_ext_forEach(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSValue val, arr;
    JSString* p;
    JSValueConst fn = argc > 0 ? argv[0] : JS_UNDEFINED;
    BOOL has_fn = JS_IsFunction(ctx, fn);
    uint32_t i;
    int64_t idx = 0;
    val = JS_ToStringCheckObject(ctx, this_val);
    if (JS_IsException(val))
        return val;
    p = JS_VALUE_GET_STRING(val);
    arr = JS_NewArray(ctx);
    if (JS_IsException(arr)) {
        JS_FreeValue(ctx, val);
        return arr;
    }
    i = 0;
    while (i < p->len) {
        uint32_t start = i;
        uint32_t c = string_getc(p, (int*)&i);
        JSValue ch, item;
        (void)c;
        ch = js_sub_string(ctx, p, start, i);
        if (JS_IsException(ch))
            goto fail;
        if (has_fn) {
            JSValueConst args[2];
            JSValue iv = JS_NewInt64(ctx, idx);
            args[0] = ch;
            args[1] = iv;
            item = JS_Call(ctx, fn, JS_UNDEFINED, 2, args);
            JS_FreeValue(ctx, iv);
            if (JS_IsException(item)) {
                JS_FreeValue(ctx, ch);
                goto fail;
            }
            JS_FreeValue(ctx, item);
        }
        if (JS_SetPropertyInt64(ctx, arr, idx++, ch) < 0)
            goto fail;
    }
    JS_FreeValue(ctx, val);
    return arr;
fail:
    JS_FreeValue(ctx, arr);
    JS_FreeValue(ctx, val);
    return JS_EXCEPTION;
}

static JSValue js_string_ext_format(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSValue val, ret = JS_EXCEPTION;
    JSString* p;
    StringBuffer b_s, *b = &b_s;
    uint32_t len, i;
    val = JS_ToStringCheckObject(ctx, this_val);
    if (JS_IsException(val))
        return val;
    p = JS_VALUE_GET_STRING(val);
    len = p->len;
    if (string_buffer_init(ctx, b, len)) {
        JS_FreeValue(ctx, val);
        return JS_EXCEPTION;
    }
    for (i = 0; i < len; i++) {
        uint32_t c = js_str_cu(p, i);
        if (c == '{' && i + 1 < len && js_str_cu(p, i + 1) == '{') {
            if (string_buffer_putc8(b, '{'))
                goto fail;
            i++;
            continue;
        }
        if (c == '}' && i + 1 < len && js_str_cu(p, i + 1) == '}') {
            if (string_buffer_putc8(b, '}'))
                goto fail;
            i++;
            continue;
        }
        if (c == '{') {
            uint32_t j = i + 1, all_digit = 1, tlen;
            char tok[64];
            int tn = 0;
            JSValue rep;
            while (j < len && js_str_cu(p, j) != '}') {
                uint32_t tc = js_str_cu(p, j);
                if (tc < '0' || tc > '9')
                    all_digit = 0;
                if (tn < (int)sizeof(tok) - 1)
                    tok[tn++] = (char)(tc < 128 ? tc : '?');
                j++;
            }
            tok[tn] = 0;
            tlen = tn;
            if (j >= len) {
                if (string_buffer_putc8(b, '{'))
                    goto fail;
                continue;
            }
            if (all_digit && tlen > 0) {
                int ai = atoi(tok) + 1;
                rep = (ai > 0 && ai - 1 < argc) ? JS_DupValue(ctx, argv[ai - 1]) : JS_UNDEFINED;
            } else if (tlen > 0 && argc > 0) {
                rep = JS_GetPropertyStr(ctx, argv[0], tok);
                if (JS_IsException(rep))
                    goto fail;
            } else {
                rep = JS_UNDEFINED;
            }
            if (!JS_IsUndefined(rep)) {
                JSValue s = JS_ToString(ctx, rep);
                JS_FreeValue(ctx, rep);
                if (JS_IsException(s))
                    goto fail;
                if (string_buffer_concat_value(b, s)) {
                    JS_FreeValue(ctx, s);
                    goto fail;
                }
                JS_FreeValue(ctx, s);
            } else {
                JS_FreeValue(ctx, rep);
            }
            i = j;
            continue;
        }
        if (string_buffer_putc(b, c))
            goto fail;
    }
    ret = string_buffer_end(b);
    JS_FreeValue(ctx, val);
    return ret;
fail:
    string_buffer_free(b);
    JS_FreeValue(ctx, val);
    return ret;
}

static int js_string_ext_two(JSContext* ctx, JSValueConst this_val,
    JSValueConst arg, JSValue* pa, JSValue* pb)
{
    *pa = JS_ToStringCheckObject(ctx, this_val);
    if (JS_IsException(*pa))
        return -1;
    *pb = JS_ToString(ctx, arg);
    if (JS_IsException(*pb)) {
        JS_FreeValue(ctx, *pa);
        return -1;
    }
    return 0;
}

static JSValue js_string_ext_trim_affix(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    JSValue a, b, ret;
    JSString *ps, *pp;
    (void)argc;

    if (js_string_ext_two(ctx, this_val, argv[0], &a, &b))
        return JS_EXCEPTION;
    ps = JS_VALUE_GET_STRING(a);
    pp = JS_VALUE_GET_STRING(b);
    if (pp->len == 0 || pp->len > ps->len) {
        ret = JS_DupValue(ctx, a);
    } else if (magic) {
        ret = string_cmp(ps, pp, ps->len - pp->len, 0, pp->len) == 0
            ? js_sub_string(ctx, ps, 0, ps->len - pp->len)
            : JS_DupValue(ctx, a);
    } else {
        ret = string_cmp(ps, pp, 0, 0, pp->len) == 0
            ? js_sub_string(ctx, ps, pp->len, ps->len)
            : JS_DupValue(ctx, a);
    }
    JS_FreeValue(ctx, a);
    JS_FreeValue(ctx, b);
    return ret;
}

static BOOL js_string_ext_in_set(JSString* p, int c)
{
    uint32_t i;
    for (i = 0; i < p->len; i++)
        if (string_get(p, i) == c)
            return TRUE;
    return FALSE;
}

static JSValue js_string_ext_trim_chars(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSValue a, b, ret;
    JSString *ps, *pc;
    uint32_t start, end;
    (void)argc;

    if (js_string_ext_two(ctx, this_val, argv[0], &a, &b))
        return JS_EXCEPTION;
    ps = JS_VALUE_GET_STRING(a);
    pc = JS_VALUE_GET_STRING(b);
    start = 0;
    end = ps->len;
    while (start < end && js_string_ext_in_set(pc, string_get(ps, start)))
        start++;
    while (end > start && js_string_ext_in_set(pc, string_get(ps, end - 1)))
        end--;
    ret = js_sub_string(ctx, ps, start, end);
    JS_FreeValue(ctx, a);
    JS_FreeValue(ctx, b);
    return ret;
}

static JSValue js_string_ext_any(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv, int magic)
{
    JSValue a, b;
    JSString *ps, *pc;
    uint32_t i;
    int found = -1;
    (void)argc;

    if (js_string_ext_two(ctx, this_val, argv[0], &a, &b))
        return JS_EXCEPTION;
    ps = JS_VALUE_GET_STRING(a);
    pc = JS_VALUE_GET_STRING(b);
    for (i = 0; i < ps->len; i++) {
        if (js_string_ext_in_set(pc, string_get(ps, i))) {
            found = (int)i;
            break;
        }
    }
    JS_FreeValue(ctx, a);
    JS_FreeValue(ctx, b);
    return magic ? JS_NewInt32(ctx, found) : JS_NewBool(ctx, found >= 0);
}

static JSValue js_string_ext_index_of_all(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSValue a, b, arr;
    JSString *ps, *pp;
    int64_t n = 0;
    int pos;
    (void)argc;

    if (js_string_ext_two(ctx, this_val, argv[0], &a, &b))
        return JS_EXCEPTION;
    arr = JS_NewArray(ctx);
    if (JS_IsException(arr)) {
        JS_FreeValue(ctx, a);
        JS_FreeValue(ctx, b);
        return JS_EXCEPTION;
    }
    ps = JS_VALUE_GET_STRING(a);
    pp = JS_VALUE_GET_STRING(b);
    if (pp->len > 0) {
        pos = 0;
        while (pos + (int)pp->len <= (int)ps->len) {
            int at = string_indexof(ps, pp, pos);
            if (at < 0)
                break;
            if (JS_DefinePropertyValueInt64(ctx, arr, n++, JS_NewInt32(ctx, at),
                    JS_PROP_C_W_E)
                < 0) {
                JS_FreeValue(ctx, arr);
                JS_FreeValue(ctx, a);
                JS_FreeValue(ctx, b);
                return JS_EXCEPTION;
            }
            pos = at + 1;
        }
    }
    JS_FreeValue(ctx, a);
    JS_FreeValue(ctx, b);
    return arr;
}

static JSValue js_string_ext_equals_ignore_case(JSContext* ctx,
    JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSValue a, b;
    JSString *ps, *pt;
    uint32_t i;
    BOOL ok;
    (void)argc;

    if (js_string_ext_two(ctx, this_val, argv[0], &a, &b))
        return JS_EXCEPTION;
    ps = JS_VALUE_GET_STRING(a);
    pt = JS_VALUE_GET_STRING(b);
    ok = (ps->len == pt->len);
    for (i = 0; ok && i < ps->len; i++) {
        int c1 = string_get(ps, i), c2 = string_get(pt, i);
        if (c1 >= 'A' && c1 <= 'Z')
            c1 += 'a' - 'A';
        if (c2 >= 'A' && c2 <= 'Z')
            c2 += 'a' - 'A';
        if (c1 != c2)
            ok = FALSE;
    }
    JS_FreeValue(ctx, a);
    JS_FreeValue(ctx, b);
    return JS_NewBool(ctx, ok);
}

static JSValue js_string_ext_compare_bytes(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSValue a, b;
    JSString *ps, *pt;
    int i = 0, j = 0, r = 0;
    (void)argc;

    if (js_string_ext_two(ctx, this_val, argv[0], &a, &b))
        return JS_EXCEPTION;
    ps = JS_VALUE_GET_STRING(a);
    pt = JS_VALUE_GET_STRING(b);
    while (i < (int)ps->len && j < (int)pt->len) {
        int c1 = string_getc(ps, &i);
        int c2 = string_getc(pt, &j);
        if (c1 != c2) {
            r = c1 < c2 ? -1 : 1;
            break;
        }
    }
    if (r == 0 && (i < (int)ps->len || j < (int)pt->len))
        r = (i < (int)ps->len) ? 1 : -1;
    JS_FreeValue(ctx, a);
    JS_FreeValue(ctx, b);
    return JS_NewInt32(ctx, r);
}

static JSValue js_string_ext_split_n(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSValue a, b, arr;
    JSString *ps, *pp;
    int64_t limit = -1, n = 0;
    int pos = 0;

    if (js_string_ext_two(ctx, this_val, argv[0], &a, &b))
        return JS_EXCEPTION;
    if (argc > 1 && !JS_IsUndefined(argv[1])) {
        if (JS_ToInt64Sat(ctx, &limit, argv[1])) {
            JS_FreeValue(ctx, a);
            JS_FreeValue(ctx, b);
            return JS_EXCEPTION;
        }
    }
    arr = JS_NewArray(ctx);
    if (JS_IsException(arr)) {
        JS_FreeValue(ctx, a);
        JS_FreeValue(ctx, b);
        return JS_EXCEPTION;
    }
    ps = JS_VALUE_GET_STRING(a);
    pp = JS_VALUE_GET_STRING(b);
    if (limit != 0) {
        for (;;) {
            int at;
            if (limit > 0 && n == limit - 1)
                break;
            if (pp->len == 0)
                break;
            at = string_indexof(ps, pp, pos);
            if (at < 0)
                break;
            if (JS_DefinePropertyValueInt64(ctx, arr, n++,
                    js_sub_string(ctx, ps, pos, at),
                    JS_PROP_C_W_E)
                < 0)
                goto fail;
            pos = at + (int)pp->len;
        }
        if (JS_DefinePropertyValueInt64(ctx, arr, n,
                js_sub_string(ctx, ps, pos, ps->len),
                JS_PROP_C_W_E)
            < 0)
            goto fail;
    }
    JS_FreeValue(ctx, a);
    JS_FreeValue(ctx, b);
    return arr;
fail:
    JS_FreeValue(ctx, arr);
    JS_FreeValue(ctx, a);
    JS_FreeValue(ctx, b);
    return JS_EXCEPTION;
}

static const JSCFunctionListEntry js_string_ext_funcs[] = {
    JS_CFUNC_MAGIC_DEF("lazy", 0, js_create_array_iterator, JS_ITERATOR_KIND_VALUE | 4),
    JS_CFUNC_DEF("isEmpty", 0, js_string_ext_isEmpty),
    JS_CFUNC_MAGIC_DEF("trimPrefix", 1, js_string_ext_trim_affix, 0),
    JS_CFUNC_MAGIC_DEF("trimSuffix", 1, js_string_ext_trim_affix, 1),
    JS_CFUNC_DEF("trimChars", 1, js_string_ext_trim_chars),
    JS_CFUNC_MAGIC_DEF("containsAny", 1, js_string_ext_any, 0),
    JS_CFUNC_MAGIC_DEF("indexOfAny", 1, js_string_ext_any, 1),
    JS_CFUNC_DEF("indexOfAll", 1, js_string_ext_index_of_all),
    JS_CFUNC_DEF("equalsIgnoreCase", 1, js_string_ext_equals_ignore_case),
    JS_CFUNC_DEF("compareBytes", 1, js_string_ext_compare_bytes),
    JS_CFUNC_DEF("splitN", 2, js_string_ext_split_n),
    JS_CFUNC_DEF("isBlank", 0, js_string_ext_isBlank),
    JS_CFUNC_MAGIC_DEF("first", 0, js_string_ext_firstlast, 0),
    JS_CFUNC_MAGIC_DEF("last", 0, js_string_ext_firstlast, 1),
    JS_CFUNC_MAGIC_DEF("from", 1, js_string_ext_fromto, 0),
    JS_CFUNC_MAGIC_DEF("to", 1, js_string_ext_fromto, 1),
    JS_CFUNC_DEF("chars", 0, js_string_ext_chars),
    JS_CFUNC_DEF("codes", 0, js_string_ext_codes),
    JS_CFUNC_DEF("reverse", 0, js_string_ext_reverse),
    JS_CFUNC_DEF("insert", 1, js_string_ext_insert),
    JS_CFUNC_MAGIC_DEF("remove", 1, js_string_ext_remove, 0),
    JS_CFUNC_MAGIC_DEF("removeAll", 1, js_string_ext_remove, 1),
    JS_CFUNC_DEF("compact", 0, js_string_ext_compact),
    JS_CFUNC_DEF("shift", 0, js_string_ext_shift),
    JS_CFUNC_DEF("pad", 1, js_string_ext_pad),
    JS_CFUNC_DEF("capitalize", 0, js_string_ext_capitalize),
    JS_CFUNC_MAGIC_DEF("underscore", 0, js_string_ext_inflect, '_'),
    JS_CFUNC_MAGIC_DEF("dasherize", 0, js_string_ext_inflect, '-'),
    JS_CFUNC_MAGIC_DEF("spacify", 0, js_string_ext_inflect, ' '),
    JS_CFUNC_DEF("camelize", 0, js_string_ext_camelize),
    JS_CFUNC_MAGIC_DEF("truncate", 1, js_string_ext_truncate, 0),
    JS_CFUNC_MAGIC_DEF("truncateOnWord", 1, js_string_ext_truncate, 1),
    JS_CFUNC_DEF("escapeHTML", 0, js_string_ext_escapeHTML),
    JS_CFUNC_DEF("unescapeHTML", 0, js_string_ext_unescapeHTML),
    JS_CFUNC_DEF("stripTags", 0, js_string_ext_stripTags),
    JS_CFUNC_DEF("count", 1, js_string_ext_count),
    JS_CFUNC_DEF("toNumber", 0, js_string_ext_toNumber),
    JS_CFUNC_DEF("humanize", 0, js_string_ext_humanize),
    JS_CFUNC_DEF("titleize", 0, js_string_ext_titleize),
    JS_CFUNC_DEF("parameterize", 0, js_string_ext_parameterize),
    JS_CFUNC_MAGIC_DEF("pluralize", 0, js_string_ext_inflect_num, 0),
    JS_CFUNC_MAGIC_DEF("singularize", 0, js_string_ext_inflect_num, 1),
    JS_CFUNC_DEF("removeTags", 0, js_string_ext_removeTags),
    JS_CFUNC_DEF("forEach", 1, js_string_ext_forEach),
    JS_CFUNC_DEF("format", 0, js_string_ext_format),
    JS_CFUNC_DEF("words", 0, js_string_ext_words),
    JS_CFUNC_DEF("lines", 0, js_string_ext_lines),
    JS_CFUNC_DEF("encodeBase64", 0, js_string_ext_encodeBase64),
    JS_CFUNC_DEF("decodeBase64", 0, js_string_ext_decodeBase64),
    JS_CFUNC_DEF("escapeURL", 0, js_string_ext_escapeURL),
    JS_CFUNC_DEF("unescapeURL", 0, js_string_ext_unescapeURL),
    JS_CFUNC_DEF("stripAnsi", 0, js_string_ext_stripAnsi),
    JS_CFUNC_DEF("displayWidth", 0, js_string_ext_displayWidth),
    JS_CFUNC_DEF("wrapAnsi", 1, js_string_ext_wrapAnsi),
    JS_CFUNC_DEF("graphemes", 0, js_string_ext_graphemes),
};

static const JSCFunctionListEntry js_string_proto_funcs[] = {
    JS_PROP_INT32_DEF("length", 0, JS_PROP_CONFIGURABLE),
    JS_CFUNC_MAGIC_DEF("at", 1, js_string_charAt, 1),
    JS_CFUNC_DEF("charCodeAt", 1, js_string_charCodeAt),
    JS_CFUNC_MAGIC_DEF("charAt", 1, js_string_charAt, 0),
    JS_CFUNC_DEF("concat", 1, js_string_concat),
    JS_CFUNC_DEF("codePointAt", 1, js_string_codePointAt),
    JS_CFUNC_DEF("isWellFormed", 0, js_string_isWellFormed),
    JS_CFUNC_DEF("toWellFormed", 0, js_string_toWellFormed),
    JS_CFUNC_MAGIC_DEF("indexOf", 1, js_string_indexOf, 0),
    JS_CFUNC_MAGIC_DEF("lastIndexOf", 1, js_string_indexOf, 1),
    JS_CFUNC_MAGIC_DEF("includes", 1, js_string_includes, 0),
    JS_CFUNC_MAGIC_DEF("endsWith", 1, js_string_includes, 2),
    JS_CFUNC_MAGIC_DEF("startsWith", 1, js_string_includes, 1),
    JS_CFUNC_MAGIC_DEF("match", 1, js_string_match, JS_ATOM_Symbol_match),
    JS_CFUNC_MAGIC_DEF("matchAll", 1, js_string_match, JS_ATOM_Symbol_matchAll),
    JS_CFUNC_MAGIC_DEF("search", 1, js_string_match, JS_ATOM_Symbol_search),
    JS_CFUNC_DEF("split", 2, js_string_split),
    JS_CFUNC_DEF("substring", 2, js_string_substring),
    JS_CFUNC_DEF("substr", 2, js_string_substr),
    JS_CFUNC_DEF("slice", 2, js_string_slice),
    JS_CFUNC_DEF("repeat", 1, js_string_repeat),
    JS_CFUNC_MAGIC_DEF("replace", 2, js_string_replace, 0),
    JS_CFUNC_MAGIC_DEF("replaceAll", 2, js_string_replace, 1),
    JS_CFUNC_MAGIC_DEF("padEnd", 1, js_string_pad, 1),
    JS_CFUNC_MAGIC_DEF("padStart", 1, js_string_pad, 0),
    JS_CFUNC_MAGIC_DEF("trim", 0, js_string_trim, 3),
    JS_CFUNC_MAGIC_DEF("trimEnd", 0, js_string_trim, 2),
    JS_ALIAS_DEF("trimRight", "trimEnd"),
    JS_CFUNC_MAGIC_DEF("trimStart", 0, js_string_trim, 1),
    JS_ALIAS_DEF("trimLeft", "trimStart"),
    JS_CFUNC_DEF("toString", 0, js_string_toString),
    JS_CFUNC_DEF("valueOf", 0, js_string_toString),
    JS_CFUNC_MAGIC_DEF("toLowerCase", 0, js_string_toLowerCase, 1),
    JS_CFUNC_MAGIC_DEF("toUpperCase", 0, js_string_toLowerCase, 0),
    JS_CFUNC_MAGIC_DEF("toLocaleLowerCase", 0, js_string_toLowerCase, 1),
    JS_CFUNC_MAGIC_DEF("toLocaleUpperCase", 0, js_string_toLowerCase, 0),
    JS_CFUNC_MAGIC_DEF("[Symbol.iterator]", 0, js_create_array_iterator, JS_ITERATOR_KIND_VALUE | 4),
    JS_CFUNC_MAGIC_DEF("anchor", 1, js_string_CreateHTML, magic_string_anchor),
    JS_CFUNC_MAGIC_DEF("big", 0, js_string_CreateHTML, magic_string_big),
    JS_CFUNC_MAGIC_DEF("blink", 0, js_string_CreateHTML, magic_string_blink),
    JS_CFUNC_MAGIC_DEF("bold", 0, js_string_CreateHTML, magic_string_bold),
    JS_CFUNC_MAGIC_DEF("fixed", 0, js_string_CreateHTML, magic_string_fixed),
    JS_CFUNC_MAGIC_DEF("fontcolor", 1, js_string_CreateHTML, magic_string_fontcolor),
    JS_CFUNC_MAGIC_DEF("fontsize", 1, js_string_CreateHTML, magic_string_fontsize),
    JS_CFUNC_MAGIC_DEF("italics", 0, js_string_CreateHTML, magic_string_italics),
    JS_CFUNC_MAGIC_DEF("link", 1, js_string_CreateHTML, magic_string_link),
    JS_CFUNC_MAGIC_DEF("small", 0, js_string_CreateHTML, magic_string_small),
    JS_CFUNC_MAGIC_DEF("strike", 0, js_string_CreateHTML, magic_string_strike),
    JS_CFUNC_MAGIC_DEF("sub", 0, js_string_CreateHTML, magic_string_sub),
    JS_CFUNC_MAGIC_DEF("sup", 0, js_string_CreateHTML, magic_string_sup),
};

static const JSCFunctionListEntry js_string_iterator_proto_funcs[] = {
    JS_ITERATOR_NEXT_DEF("next", 0, js_string_iterator_next, 0),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "String Iterator", JS_PROP_CONFIGURABLE),
};

static const JSCFunctionListEntry js_string_proto_normalize[] = {
#ifdef CONFIG_ALL_UNICODE
    JS_CFUNC_DEF("normalize", 0, js_string_normalize),
#endif
    JS_CFUNC_DEF("localeCompare", 1, js_string_localeCompare),
};

int JS_AddIntrinsicStringNormalize(JSContext* ctx)
{
    return JS_SetPropertyFunctionList(ctx, ctx->class_proto[JS_CLASS_STRING], js_string_proto_normalize,
        countof(js_string_proto_normalize));
}
