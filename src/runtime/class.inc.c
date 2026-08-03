/* JSClass support */

#ifdef CONFIG_ATOMICS
static pthread_mutex_t js_class_id_mutex = PTHREAD_MUTEX_INITIALIZER;
#endif

/* a new class ID is allocated if *pclass_id != 0 */
JSClassID JS_NewClassID(JSClassID *pclass_id)
{
    JSClassID class_id;
#ifdef CONFIG_ATOMICS
    pthread_mutex_lock(&js_class_id_mutex);
#endif
    class_id = *pclass_id;
    if (class_id == 0) {
        class_id = js_class_id_alloc++;
        *pclass_id = class_id;
    }
#ifdef CONFIG_ATOMICS
    pthread_mutex_unlock(&js_class_id_mutex);
#endif
    return class_id;
}

JSClassID JS_GetClassID(JSValue v)
{
    JSObject *p;
    if (JS_VALUE_GET_TAG(v) != JS_TAG_OBJECT)
        return JS_INVALID_CLASS_ID;
    p = JS_VALUE_GET_OBJ(v);
    return p->class_id;
}

BOOL JS_IsRegisteredClass(JSRuntime *rt, JSClassID class_id)
{
    return (class_id < rt->class_count &&
            rt->class_array[class_id].class_id != 0);
}

/* create a new object internal class. Return -1 if error, 0 if
   OK. The finalizer can be NULL if none is needed. */
static int JS_NewClass1(JSRuntime *rt, JSClassID class_id,
                        const JSClassDef *class_def, JSAtom name)
{
    int new_size, i;
    JSClass *cl, *new_class_array;
    struct list_head *el;

    if (class_id >= (1 << 16))
        return -1;
    if (class_id < rt->class_count &&
        rt->class_array[class_id].class_id != 0)
        return -1;

    if (class_id >= rt->class_count) {
        new_size = max_int(JS_CLASS_INIT_COUNT,
                           max_int(class_id + 1, rt->class_count * 3 / 2));

        /* reallocate the context class prototype array, if any */
        list_for_each(el, &rt->context_list) {
            JSContext *ctx = list_entry(el, JSContext, link);
            JSValue *new_tab;
            new_tab = js_realloc_rt(rt, ctx->class_proto,
                                    sizeof(ctx->class_proto[0]) * new_size);
            if (!new_tab)
                return -1;
            for(i = rt->class_count; i < new_size; i++)
                new_tab[i] = JS_NULL;
            ctx->class_proto = new_tab;
        }
        /* reallocate the class array */
        new_class_array = js_realloc_rt(rt, rt->class_array,
                                        sizeof(JSClass) * new_size);
        if (!new_class_array)
            return -1;
        memset(new_class_array + rt->class_count, 0,
               (new_size - rt->class_count) * sizeof(JSClass));
        rt->class_array = new_class_array;
        rt->class_count = new_size;
    }
    cl = &rt->class_array[class_id];
    cl->class_id = class_id;
    cl->class_name = JS_DupAtomRT(rt, name);
    cl->finalizer = class_def->finalizer;
    cl->gc_mark = class_def->gc_mark;
    cl->call = class_def->call;
    cl->exotic = class_def->exotic;
    return 0;
}

int JS_NewClass(JSRuntime *rt, JSClassID class_id, const JSClassDef *class_def)
{
    int ret, len;
    JSAtom name;

    len = strlen(class_def->class_name);
    name = __JS_FindAtom(rt, class_def->class_name, len, JS_ATOM_TYPE_STRING);
    if (name == JS_ATOM_NULL) {
        name = __JS_NewAtomInit(rt, class_def->class_name, len, JS_ATOM_TYPE_STRING);
        if (name == JS_ATOM_NULL)
            return -1;
    }
    ret = JS_NewClass1(rt, class_id, class_def, name);
    JS_FreeAtomRT(rt, name);
    return ret;
}

static JSValue js_new_string8_len(JSContext *ctx, const char *buf, int len)
{
    JSString *str;

    if (len <= 0) {
        return JS_AtomToString(ctx, JS_ATOM_empty_string);
    }
    str = js_alloc_string(ctx, len, 0);
    if (!str)
        return JS_EXCEPTION;
    memcpy(str->u.str8, buf, len);
    str->u.str8[len] = '\0';
    return JS_MKPTR(JS_TAG_STRING, str);
}

static JSValue js_new_string8(JSContext *ctx, const char *buf)
{
    return js_new_string8_len(ctx, buf, strlen(buf));
}

static JSValue js_new_string16_len(JSContext *ctx, const uint16_t *buf, int len)
{
    JSString *str;
    str = js_alloc_string(ctx, len, 1);
    if (!str)
        return JS_EXCEPTION;
    memcpy(str->u.str16, buf, len * 2);
    return JS_MKPTR(JS_TAG_STRING, str);
}

static JSValue js_new_string_char(JSContext *ctx, uint16_t c)
{
    if (c < 0x100) {
        uint8_t ch8 = c;
        return js_new_string8_len(ctx, (const char *)&ch8, 1);
    } else {
        uint16_t ch16 = c;
        return js_new_string16_len(ctx, &ch16, 1);
    }
}

static JSValue js_sub_string(JSContext *ctx, JSString *p, int start, int end)
{
    int len = end - start;
    if (start == 0 && end == p->len) {
        return JS_DupValue(ctx, JS_MKPTR(JS_TAG_STRING, p));
    }
    if (p->is_wide_char && len > 0) {
        JSString *str;
        int i;
        uint16_t c = 0;
        for (i = start; i < end; i++) {
            c |= p->u.str16[i];
        }
        if (c > 0xFF)
            return js_new_string16_len(ctx, p->u.str16 + start, len);

        str = js_alloc_string(ctx, len, 0);
        if (!str)
            return JS_EXCEPTION;
        for (i = 0; i < len; i++) {
            str->u.str8[i] = p->u.str16[start + i];
        }
        str->u.str8[len] = '\0';
        return JS_MKPTR(JS_TAG_STRING, str);
    } else {
        return js_new_string8_len(ctx, (const char *)(p->u.str8 + start), len);
    }
}

typedef struct StringBuffer {
    JSContext *ctx;
    JSString *str;
    int len;
    int size;
    int is_wide_char;
    int error_status;
} StringBuffer;

/* It is valid to call string_buffer_end() and all string_buffer functions even
   if string_buffer_init() or another string_buffer function returns an error.
   If the error_status is set, string_buffer_end() returns JS_EXCEPTION.
 */
static int string_buffer_init2(JSContext *ctx, StringBuffer *s, int size,
                               int is_wide)
{
    s->ctx = ctx;
    s->size = size;
    s->len = 0;
    s->is_wide_char = is_wide;
    s->error_status = 0;
    s->str = js_alloc_string(ctx, size, is_wide);
    if (unlikely(!s->str)) {
        s->size = 0;
        return s->error_status = -1;
    }
#ifdef DUMP_LEAKS
    /* the StringBuffer may reallocate the JSString, only link it at the end */
    list_del(&s->str->link);
#endif
    return 0;
}

static inline int string_buffer_init(JSContext *ctx, StringBuffer *s, int size)
{
    return string_buffer_init2(ctx, s, size, 0);
}

static void string_buffer_free(StringBuffer *s)
{
    js_free(s->ctx, s->str);
    s->str = NULL;
}

static int string_buffer_set_error(StringBuffer *s)
{
    js_free(s->ctx, s->str);
    s->str = NULL;
    s->size = 0;
    s->len = 0;
    return s->error_status = -1;
}

static no_inline int string_buffer_widen(StringBuffer *s, int size)
{
    JSString *str;
    size_t slack;
    int i;

    if (s->error_status)
        return -1;

    str = js_realloc2(s->ctx, s->str, sizeof(JSString) + (size << 1), &slack);
    if (!str)
        return string_buffer_set_error(s);
    size += slack >> 1;
    for(i = s->len; i-- > 0;) {
        str->u.str16[i] = str->u.str8[i];
    }
    s->is_wide_char = 1;
    s->size = size;
    s->str = str;
    return 0;
}

static no_inline int string_buffer_realloc(StringBuffer *s, int new_len, int c)
{
    JSString *new_str;
    int new_size;
    size_t new_size_bytes, slack;

    if (s->error_status)
        return -1;

    if (new_len > JS_STRING_LEN_MAX) {
        JS_ThrowInternalError(s->ctx, "string too long");
        return string_buffer_set_error(s);
    }
    /* Floor the geometric growth. A buffer that starts empty would otherwise
       ramp 1,2,3,4,6,9,... and pay ~6 realloc+copy round trips before it even
       reaches 16 bytes; every string built from empty hits this. */
    new_size = min_int(max_int(new_len, max_int(s->size * 3 / 2, 16)),
                       JS_STRING_LEN_MAX);
    if (!s->is_wide_char && c >= 0x100) {
        return string_buffer_widen(s, new_size);
    }
    new_size_bytes = sizeof(JSString) + (new_size << s->is_wide_char) + 1 - s->is_wide_char;
    new_str = js_realloc2(s->ctx, s->str, new_size_bytes, &slack);
    if (!new_str)
        return string_buffer_set_error(s);
    new_size = min_int(new_size + (slack >> s->is_wide_char), JS_STRING_LEN_MAX);
    s->size = new_size;
    s->str = new_str;
    return 0;
}

static no_inline int string_buffer_putc16_slow(StringBuffer *s, uint32_t c)
{
    if (unlikely(s->len >= s->size)) {
        if (string_buffer_realloc(s, s->len + 1, c))
            return -1;
    }
    if (s->is_wide_char) {
        s->str->u.str16[s->len++] = c;
    } else if (c < 0x100) {
        s->str->u.str8[s->len++] = c;
    } else {
        if (string_buffer_widen(s, s->size))
            return -1;
        s->str->u.str16[s->len++] = c;
    }
    return 0;
}

/* 0 <= c <= 0xff */
static int string_buffer_putc8(StringBuffer *s, uint32_t c)
{
    if (unlikely(s->len >= s->size)) {
        if (string_buffer_realloc(s, s->len + 1, c))
            return -1;
    }
    if (s->is_wide_char) {
        s->str->u.str16[s->len++] = c;
    } else {
        s->str->u.str8[s->len++] = c;
    }
    return 0;
}

/* 0 <= c <= 0xffff */
static int string_buffer_putc16(StringBuffer *s, uint32_t c)
{
    if (likely(s->len < s->size)) {
        if (s->is_wide_char) {
            s->str->u.str16[s->len++] = c;
            return 0;
        } else if (c < 0x100) {
            s->str->u.str8[s->len++] = c;
            return 0;
        }
    }
    return string_buffer_putc16_slow(s, c);
}

static int string_buffer_putc_slow(StringBuffer *s, uint32_t c)
{
    if (unlikely(c >= 0x10000)) {
        /* surrogate pair */
        if (string_buffer_putc16(s, get_hi_surrogate(c)))
            return -1;
        c = get_lo_surrogate(c);
    }
    return string_buffer_putc16(s, c);
}

/* 0 <= c <= 0x10ffff */
static inline int string_buffer_putc(StringBuffer *s, uint32_t c)
{
    if (likely(s->len < s->size)) {
        if (s->is_wide_char) {
            if (c < 0x10000) {
                s->str->u.str16[s->len++] = c;
                return 0;
            } else if (likely((s->len + 1) < s->size)) {
                s->str->u.str16[s->len++] = get_hi_surrogate(c);
                s->str->u.str16[s->len++] = get_lo_surrogate(c);
                return 0;
            }
        } else if (c < 0x100) {
            s->str->u.str8[s->len++] = c;
            return 0;
        }
    }
    return string_buffer_putc_slow(s, c);
}

static int string_getc(const JSString *p, int *pidx)
{
    int idx, c, c1;
    idx = *pidx;
    if (p->is_wide_char) {
        c = p->u.str16[idx++];
        if (is_hi_surrogate(c) && idx < p->len) {
            c1 = p->u.str16[idx];
            if (is_lo_surrogate(c1)) {
                c = from_surrogate(c, c1);
                idx++;
            }
        }
    } else {
        c = p->u.str8[idx++];
    }
    *pidx = idx;
    return c;
}

static int string_buffer_write8(StringBuffer *s, const uint8_t *p, int len)
{
    int i;

    if (s->len + len > s->size) {
        if (string_buffer_realloc(s, s->len + len, 0))
            return -1;
    }
    if (s->is_wide_char) {
        for (i = 0; i < len; i++) {
            s->str->u.str16[s->len + i] = p[i];
        }
        s->len += len;
    } else {
        memcpy(&s->str->u.str8[s->len], p, len);
        s->len += len;
    }
    return 0;
}

static int string_buffer_write16(StringBuffer *s, const uint16_t *p, int len)
{
    int c = 0, i;

    /* c only feeds the two narrow-buffer widening decisions below; once the
       buffer is wide it is dead, so skip the scan over the whole source */
    if (!s->is_wide_char) {
        for (i = 0; i < len; i++) {
            c |= p[i];
        }
    }
    if (s->len + len > s->size) {
        if (string_buffer_realloc(s, s->len + len, c))
            return -1;
    } else if (!s->is_wide_char && c >= 0x100) {
        if (string_buffer_widen(s, s->size))
            return -1;
    }
    if (s->is_wide_char) {
        memcpy(&s->str->u.str16[s->len], p, len << 1);
        s->len += len;
    } else {
        for (i = 0; i < len; i++) {
            s->str->u.str8[s->len + i] = p[i];
        }
        s->len += len;
    }
    return 0;
}

/* appending an ASCII string */
static int string_buffer_puts8(StringBuffer *s, const char *str)
{
    return string_buffer_write8(s, (const uint8_t *)str, strlen(str));
}

static int string_buffer_concat(StringBuffer *s, const JSString *p,
                                uint32_t from, uint32_t to)
{
    if (to <= from)
        return 0;
    if (p->is_wide_char)
        return string_buffer_write16(s, p->u.str16 + from, to - from);
    else
        return string_buffer_write8(s, p->u.str8 + from, to - from);
}

static int string_buffer_concat_value(StringBuffer *s, JSValueConst v)
{
    JSString *p;
    JSValue v1;
    int res;

    if (s->error_status) {
        /* prevent exception overload */
        return -1;
    }
    if (unlikely(JS_VALUE_GET_TAG(v) != JS_TAG_STRING)) {
        if (JS_VALUE_GET_TAG(v) == JS_TAG_STRING_ROPE) {
            JSStringRope *r = JS_VALUE_GET_STRING_ROPE(v);
            /* recursion is acceptable because the rope depth is bounded */
            if (string_buffer_concat_value(s, r->left))
                return -1;
            return string_buffer_concat_value(s, r->right);
        } else {
            v1 = JS_ToString(s->ctx, v);
            if (JS_IsException(v1))
                return string_buffer_set_error(s);
            p = JS_VALUE_GET_STRING(v1);
            res = string_buffer_concat(s, p, 0, p->len);
            JS_FreeValue(s->ctx, v1);
            return res;
        }
    }
    p = JS_VALUE_GET_STRING(v);
    return string_buffer_concat(s, p, 0, p->len);
}

static int string_buffer_concat_value_free(StringBuffer *s, JSValue v)
{
    JSString *p;
    int res;

    if (s->error_status) {
        /* prevent exception overload */
        JS_FreeValue(s->ctx, v);
        return -1;
    }
    if (unlikely(JS_VALUE_GET_TAG(v) != JS_TAG_STRING)) {
        v = JS_ToStringFree(s->ctx, v);
        if (JS_IsException(v))
            return string_buffer_set_error(s);
    }
    p = JS_VALUE_GET_STRING(v);
    res = string_buffer_concat(s, p, 0, p->len);
    JS_FreeValue(s->ctx, v);
    return res;
}

/* 0 <= c <= 0xffff */
static int string_buffer_fill(StringBuffer *s, int c, int count)
{
    int i;

    if (count <= 0)
        return 0;
    if (s->len + count > s->size) {
        /* also widens if c does not fit the current width */
        if (string_buffer_realloc(s, s->len + count, c))
            return -1;
    } else if (!s->is_wide_char && c >= 0x100) {
        /* capacity already sufficed so the realloc (which widens) was skipped;
           the buffer must still be widened to hold c */
        if (string_buffer_widen(s, s->size))
            return -1;
    }
    if (s->is_wide_char) {
        uint16_t *q = s->str->u.str16 + s->len;
        for (i = 0; i < count; i++)
            q[i] = c;
    } else {
        memset(s->str->u.str8 + s->len, c, count);
    }
    s->len += count;
    return 0;
}

static JSValue string_buffer_end(StringBuffer *s)
{
    JSString *str;
    str = s->str;
    if (s->error_status)
        return JS_EXCEPTION;
    if (s->len == 0) {
        js_free(s->ctx, str);
        s->str = NULL;
        return JS_AtomToString(s->ctx, JS_ATOM_empty_string);
    }
    if (s->len < s->size) {
        /* smaller size so js_realloc should not fail, but OK if it does */
        /* XXX: should add some slack to avoid unnecessary calls */
        /* XXX: might need to use malloc+free to ensure smaller size */
        str = js_realloc_rt(s->ctx->rt, str, sizeof(JSString) +
                            (s->len << s->is_wide_char) + 1 - s->is_wide_char);
        if (str == NULL)
            str = s->str;
        s->str = str;
    }
    if (!s->is_wide_char)
        str->u.str8[s->len] = 0;
#ifdef DUMP_LEAKS
    list_add_tail(&str->link, &s->ctx->rt->string_list);
#endif
    str->is_wide_char = s->is_wide_char;
    str->len = s->len;
    s->str = NULL;
    return JS_MKPTR(JS_TAG_STRING, str);
}

/* Below this the transcode is not worth two kernel calls and a possible
   discarded allocation; the scalar loop wins on short strings. */
#define JS_UTF8_SIMD_MIN_LEN 64

/* Gate so a -DCONFIG_UTF8_SIMD=0 build is the differential oracle: the strict
   kernels must never disagree with the lossy scalar loop on any input. */
#ifndef CONFIG_UTF8_SIMD
#define CONFIG_UTF8_SIMD 1
#endif

#if CONFIG_UTF8_SIMD
/* Bulk UTF-8 -> JSString for the non-ASCII case, using the SIMD transcoders.
 *
 * Returns JS_UNINITIALIZED to mean "declined, use the scalar path" -- which is
 * NOT a failure mode but the correctness anchor: the kernels are STRICT (they
 * reject ill-formed UTF-8), whereas this function's contract is LOSSY (invalid
 * sequences become U+FFFD). Anything the kernels refuse is handed to the
 * original byte-at-a-time loop, so malformed input keeps byte-identical
 * behaviour and only well-formed input takes the fast path.
 *
 * Order matters: latin1 is tried first because it yields a NARROW string (half
 * the memory, and it stays narrow for every later operation), and because the
 * kernel bails on the first code point above 0xFF, so the wasted work when it
 * declines is bounded by the position of the first non-latin1 character. */
static JSValue js_new_string_utf8_simd(JSContext *ctx, const uint8_t *p,
                                       size_t n)
{
    JSString *str;
    size_t out_len;

    simd_init(); /* idempotent (pthread_once) */

    /* narrow: every code point <= 0xFF */
    str = js_alloc_string(ctx, n, 0);
    if (!str)
        return JS_EXCEPTION;
    if (simd.utf8_to_latin1(p, n, str->u.str8, &out_len) == 0) {
        str->u.str8[out_len] = '\0';
        str->len = out_len;
        return JS_MKPTR(JS_TAG_STRING, str);
    }
    js_free_string(ctx->rt, str);

    /* wide: well-formed UTF-8 never produces a lone surrogate, so a successful
       decode is always well-formed UTF-16 */
    str = js_alloc_string(ctx, n, 1);
    if (!str)
        return JS_EXCEPTION;
    if (simd.utf8_to_utf16le(p, n, str->u.str16, &out_len) == 0) {
        str->len = out_len;
        return JS_MKPTR(JS_TAG_STRING, str);
    }
    js_free_string(ctx->rt, str);

    return JS_UNINITIALIZED; /* malformed: let the lossy scalar path handle it */
}
#endif /* CONFIG_UTF8_SIMD */

/* create a string from a UTF-8 buffer */
JSValue JS_NewStringLen(JSContext *ctx, const char *buf, size_t buf_len)
{
    const uint8_t *p, *p_end, *p_start, *p_next;
    uint32_t c;
    StringBuffer b_s, *b = &b_s;
    size_t len1;

    p_start = (const uint8_t *)buf;
    p_end = p_start + buf_len;
    len1 = count_ascii(p_start, buf_len);
    p = p_start + len1;
    if (len1 > JS_STRING_LEN_MAX)
        return JS_ThrowInternalError(ctx, "string too long");
    if (p == p_end) {
        /* ASCII string */
        return js_new_string8_len(ctx, buf, buf_len);
    } else {
#if CONFIG_UTF8_SIMD
        if (buf_len >= JS_UTF8_SIMD_MIN_LEN && buf_len <= JS_STRING_LEN_MAX) {
            JSValue v = js_new_string_utf8_simd(ctx, p_start, buf_len);
            if (!JS_IsUninitialized(v))
                return v;   /* transcoded, or a real allocation failure */
        }
#endif
        if (string_buffer_init(ctx, b, buf_len))
            goto fail;
        string_buffer_write8(b, p_start, len1);
        while (p < p_end) {
            if (*p < 128) {
                string_buffer_putc8(b, *p++);
            } else {
                /* parse utf-8 sequence, return 0xFFFFFFFF for error */
                c = unicode_from_utf8(p, p_end - p, &p_next);
                if (c < 0x10000) {
                    p = p_next;
                } else if (c <= 0x10FFFF) {
                    p = p_next;
                    /* surrogate pair */
                    string_buffer_putc16(b, get_hi_surrogate(c));
                    c = get_lo_surrogate(c);
                } else {
                    /* invalid char */
                    c = 0xfffd;
                    /* skip the invalid chars */
                    /* XXX: seems incorrect. Why not just use c = *p++; ? */
                    while (p < p_end && (*p >= 0x80 && *p < 0xc0))
                        p++;
                    if (p < p_end) {
                        p++;
                        while (p < p_end && (*p >= 0x80 && *p < 0xc0))
                            p++;
                    }
                }
                string_buffer_putc16(b, c);
            }
        }
    }
    return string_buffer_end(b);

 fail:
    string_buffer_free(b);
    return JS_EXCEPTION;
}

static JSValue JS_ConcatString3(JSContext *ctx, const char *str1,
                                JSValue str2, const char *str3)
{
    StringBuffer b_s, *b = &b_s;
    int len1, len3;
    JSString *p;

    if (unlikely(JS_VALUE_GET_TAG(str2) != JS_TAG_STRING)) {
        str2 = JS_ToStringFree(ctx, str2);
        if (JS_IsException(str2))
            goto fail;
    }
    p = JS_VALUE_GET_STRING(str2);
    len1 = strlen(str1);
    len3 = strlen(str3);

    if (string_buffer_init2(ctx, b, len1 + p->len + len3, p->is_wide_char))
        goto fail;

    string_buffer_write8(b, (const uint8_t *)str1, len1);
    string_buffer_concat(b, p, 0, p->len);
    string_buffer_write8(b, (const uint8_t *)str3, len3);

    JS_FreeValue(ctx, str2);
    return string_buffer_end(b);

 fail:
    JS_FreeValue(ctx, str2);
    return JS_EXCEPTION;
}

JSValue JS_NewAtomString(JSContext *ctx, const char *str)
{
    JSAtom atom = JS_NewAtom(ctx, str);
    if (atom == JS_ATOM_NULL)
        return JS_EXCEPTION;
    JSValue val = JS_AtomToString(ctx, atom);
    JS_FreeAtom(ctx, atom);
    return val;
}

/* return (NULL, 0) if exception. */
/* return pointer into a JSString with a live ref_count */
/* cesu8 determines if non-BMP1 codepoints are encoded as 1 or 2 utf-8 sequences */
const uint8_t *JS_GetNarrowStringBytes(JSContext *ctx, JSValueConst val,
                                       size_t *plen)
{
    JSString *p;
    (void)ctx;
    if (JS_VALUE_GET_TAG(val) != JS_TAG_STRING)
        return NULL;
    p = JS_VALUE_GET_STRING(val);
    if (p->is_wide_char)
        return NULL;
    if (plen)
        *plen = p->len;
    return p->u.str8;
}

const char *JS_ToCStringLen2(JSContext *ctx, size_t *plen, JSValueConst val1, BOOL cesu8)
{
    JSValue val;
    JSString *str, *str_new;
    int pos, len, c, c1;
    uint8_t *q;

    if (JS_VALUE_GET_TAG(val1) != JS_TAG_STRING) {
        val = JS_ToString(ctx, val1);
        if (JS_IsException(val))
            goto fail;
    } else {
        val = JS_DupValue(ctx, val1);
    }

    str = JS_VALUE_GET_STRING(val);
    len = str->len;
    if (!str->is_wide_char) {
        const uint8_t *src = str->u.str8;
        int count;

        /* count the number of non-ASCII characters */
        /* Scanning the whole string is required for ASCII strings,
           and computing the number of non-ASCII bytes is less expensive
           than testing each byte, hence this method is faster for ASCII
           strings, which is the most common case.
         */
        count = 0;
        for (pos = 0; pos < len; pos++) {
            count += src[pos] >> 7;
        }
        if (count == 0) {
            if (plen)
                *plen = len;
            return (const char *)src;
        }
        str_new = js_alloc_string(ctx, len + count, 0);
        if (!str_new)
            goto fail;
        q = str_new->u.str8;
        for (pos = 0; pos < len; pos++) {
            c = src[pos];
            if (c < 0x80) {
                *q++ = c;
            } else {
                *q++ = (c >> 6) | 0xc0;
                *q++ = (c & 0x3f) | 0x80;
            }
        }
    } else {
        const uint16_t *src = str->u.str16;
        /* Allocate 3 bytes per 16 bit code point. Surrogate pairs may
           produce 4 bytes but use 2 code points.
         */
        str_new = js_alloc_string(ctx, len * 3, 0);
        if (!str_new)
            goto fail;
        q = str_new->u.str8;
        pos = 0;
        while (pos < len) {
            c = src[pos++];
            if (c < 0x80) {
                *q++ = c;
            } else {
                if (is_hi_surrogate(c)) {
                    if (pos < len && !cesu8) {
                        c1 = src[pos];
                        if (is_lo_surrogate(c1)) {
                            pos++;
                            c = from_surrogate(c, c1);
                        } else {
                            /* Keep unmatched surrogate code points */
                            /* c = 0xfffd; */ /* error */
                        }
                    } else {
                        /* Keep unmatched surrogate code points */
                        /* c = 0xfffd; */ /* error */
                    }
                }
                q += unicode_to_utf8(q, c);
            }
        }
    }

    *q = '\0';
    str_new->len = q - str_new->u.str8;
    JS_FreeValue(ctx, val);
    if (plen)
        *plen = str_new->len;
    return (const char *)str_new->u.str8;
 fail:
    if (plen)
        *plen = 0;
    return NULL;
}

void JS_FreeCString(JSContext *ctx, const char *ptr)
{
    JSString *p;
    if (!ptr)
        return;
    /* purposely removing constness */
    p = container_of(ptr, JSString, u);
    JS_FreeValue(ctx, JS_MKPTR(JS_TAG_STRING, p));
}

static int memcmp16_8(const uint16_t *src1, const uint8_t *src2, int len)
{
    int c, i;
    for(i = 0; i < len; i++) {
        c = src1[i] - src2[i];
        if (c != 0)
            return c;
    }
    return 0;
}

static int memcmp16(const uint16_t *src1, const uint16_t *src2, int len)
{
    int c, i;
    for(i = 0; i < len; i++) {
        c = src1[i] - src2[i];
        if (c != 0)
            return c;
    }
    return 0;
}

static int js_string_memcmp(const JSString *p1, int pos1, const JSString *p2,
                            int pos2, int len)
{
    int res;

    if (likely(!p1->is_wide_char)) {
        if (likely(!p2->is_wide_char))
            res = memcmp(p1->u.str8 + pos1, p2->u.str8 + pos2, len);
        else
            res = -memcmp16_8(p2->u.str16 + pos2, p1->u.str8 + pos1, len);
    } else {
        if (!p2->is_wide_char)
            res = memcmp16_8(p1->u.str16 + pos1, p2->u.str8 + pos2, len);
        else
            res = memcmp16(p1->u.str16 + pos1, p2->u.str16 + pos2, len);
    }
    return res;
}

static BOOL js_string_eq(JSContext *ctx,
                         const JSString *p1, const JSString *p2)
{
    if (p1->len != p2->len)
        return FALSE;
    if (p1 == p2)
        return TRUE;
    return js_string_memcmp(p1, 0, p2, 0, p1->len) == 0;
}

/* return < 0, 0 or > 0 */
static int js_string_compare(JSContext *ctx,
                             const JSString *p1, const JSString *p2)
{
    int res, len;
    len = min_int(p1->len, p2->len);
    res = js_string_memcmp(p1, 0, p2, 0, len);
    if (res == 0) {
        if (p1->len == p2->len)
            res = 0;
        else if (p1->len < p2->len)
            res = -1;
        else
            res = 1;
    }
    return res;
}

static void copy_str16(uint16_t *dst, const JSString *p, int offset, int len)
{
    if (p->is_wide_char) {
        memcpy(dst, p->u.str16 + offset, len * 2);
    } else {
        const uint8_t *src1 = p->u.str8 + offset;
        int i;

        for(i = 0; i < len; i++)
            dst[i] = src1[i];
    }
}

static JSValue JS_ConcatString1(JSContext *ctx,
                                const JSString *p1, const JSString *p2)
{
    JSString *p;
    uint32_t len;
    int is_wide_char;

    len = p1->len + p2->len;
    if (len > JS_STRING_LEN_MAX)
        return JS_ThrowInternalError(ctx, "string too long");
    is_wide_char = p1->is_wide_char | p2->is_wide_char;
    p = js_alloc_string(ctx, len, is_wide_char);
    if (!p)
        return JS_EXCEPTION;
    if (!is_wide_char) {
        memcpy(p->u.str8, p1->u.str8, p1->len);
        memcpy(p->u.str8 + p1->len, p2->u.str8, p2->len);
        p->u.str8[len] = '\0';
    } else {
        copy_str16(p->u.str16, p1, 0, p1->len);
        copy_str16(p->u.str16 + p1->len, p2, 0, p2->len);
    }
    return JS_MKPTR(JS_TAG_STRING, p);
}

static BOOL JS_ConcatStringInPlace(JSContext *ctx, JSString *p1, JSValueConst op2) {
    if (JS_VALUE_GET_TAG(op2) == JS_TAG_STRING) {
        JSString *p2 = JS_VALUE_GET_STRING(op2);
        size_t size1;

        if (p2->len == 0)
            return TRUE;
        if (js_rc(p1)->ref_count != 1)
            return FALSE;
        size1 = js_malloc_usable_size(ctx, p1);
        if (p1->is_wide_char) {
            if (size1 >= sizeof(*p1) + ((p1->len + p2->len) << 1)) {
                if (p2->is_wide_char) {
                    memcpy(p1->u.str16 + p1->len, p2->u.str16, p2->len << 1);
                    p1->len += p2->len;
                    return TRUE;
                } else {
                    size_t i;
                    for (i = 0; i < p2->len; i++) {
                        p1->u.str16[p1->len++] = p2->u.str8[i];
                    }
                    return TRUE;
                }
            }
        } else if (!p2->is_wide_char) {
            if (size1 >= sizeof(*p1) + p1->len + p2->len + 1) {
                memcpy(p1->u.str8 + p1->len, p2->u.str8, p2->len);
                p1->len += p2->len;
                p1->u.str8[p1->len] = '\0';
                return TRUE;
            }
        }
    }
    return FALSE;
}

static JSValue JS_ConcatString2(JSContext *ctx, JSValue op1, JSValue op2)
{
    JSValue ret;
    JSString *p1, *p2;
    p1 = JS_VALUE_GET_STRING(op1);
    if (JS_ConcatStringInPlace(ctx, p1, op2)) {
        JS_FreeValue(ctx, op2);
        return op1;
    }
    p2 = JS_VALUE_GET_STRING(op2);
    ret = JS_ConcatString1(ctx, p1, p2);
    JS_FreeValue(ctx, op1);
    JS_FreeValue(ctx, op2);
    return ret;
}

/* Return the character at position 'idx'. 'val' must be a string or rope */
static int string_rope_get(JSValueConst val, uint32_t idx)
{
    if (JS_VALUE_GET_TAG(val) == JS_TAG_STRING) {
        return string_get(JS_VALUE_GET_STRING(val), idx);
    } else {
        JSStringRope *r = JS_VALUE_GET_STRING_ROPE(val);
        uint32_t len;
        if (JS_VALUE_GET_TAG(r->left) == JS_TAG_STRING)
            len = JS_VALUE_GET_STRING(r->left)->len;
        else
            len = JS_VALUE_GET_STRING_ROPE(r->left)->len;
        if (idx < len)
            return string_rope_get(r->left, idx);
        else
            return string_rope_get(r->right, idx - len);
    }
}

typedef struct {
    JSValueConst stack[JS_STRING_ROPE_MAX_DEPTH];
    int stack_len;
} JSStringRopeIter;

static void string_rope_iter_init(JSStringRopeIter *s, JSValueConst val)
{
    s->stack_len = 0;
    s->stack[s->stack_len++] = val;
}

/* iterate thru a rope and return the strings in order */
static JSString *string_rope_iter_next(JSStringRopeIter *s)
{
    JSValueConst val;
    JSStringRope *r;

    if (s->stack_len == 0)
        return NULL;
    val = s->stack[--s->stack_len];
    for(;;) {
        if (JS_VALUE_GET_TAG(val) == JS_TAG_STRING)
            return JS_VALUE_GET_STRING(val);
        r = JS_VALUE_GET_STRING_ROPE(val);
        assert(s->stack_len < JS_STRING_ROPE_MAX_DEPTH);
        s->stack[s->stack_len++] = r->right;
        val = r->left;
    }
}

static uint32_t string_rope_get_len(JSValueConst val)
{
    if (JS_VALUE_GET_TAG(val) == JS_TAG_STRING)
        return JS_VALUE_GET_STRING(val)->len;
    else
        return JS_VALUE_GET_STRING_ROPE(val)->len;
}

static int js_string_rope_compare(JSContext *ctx, JSValueConst op1,
                                  JSValueConst op2, BOOL eq_only)
{
    uint32_t len1, len2, len, pos1, pos2, l;
    int res;
    JSStringRopeIter it1, it2;
    JSString *p1, *p2;
    
    len1 = string_rope_get_len(op1);
    len2 = string_rope_get_len(op2);
    /* no need to go further for equality test if
       different length */
    if (eq_only && len1 != len2)
        return 1; 
    len = min_uint32(len1, len2);
    string_rope_iter_init(&it1, op1);
    string_rope_iter_init(&it2, op2);
    p1 = string_rope_iter_next(&it1);
    p2 = string_rope_iter_next(&it2);
    pos1 = 0;
    pos2 = 0;
    while (len != 0) {
        l = min_uint32(p1->len - pos1, p2->len - pos2);
        l = min_uint32(l, len);
        res = js_string_memcmp(p1, pos1, p2, pos2, l);
        if (res != 0)
            return res;
        len -= l;
        pos1 += l;
        if (pos1 >= p1->len) {
            p1 = string_rope_iter_next(&it1);
            pos1 = 0;
        }
        pos2 += l;
        if (pos2 >= p2->len) {
            p2 = string_rope_iter_next(&it2);
            pos2 = 0;
        }
    }

    if (len1 == len2)
        res = 0;
    else if (len1 < len2)
        res = -1;
    else
        res = 1;
    return res;
}

/* 'rope' must be a rope. return a string and modify the rope so that
   it won't need to be linearized again. */
static JSValue js_linearize_string_rope(JSContext *ctx, JSValue rope)
{
    StringBuffer b_s, *b = &b_s;
    JSStringRope *r;
    JSValue ret;
    
    r = JS_VALUE_GET_STRING_ROPE(rope);

    /* check whether it is already linearized */
    if (JS_VALUE_GET_TAG(r->right) == JS_TAG_STRING &&
        JS_VALUE_GET_STRING(r->right)->len == 0) {
        ret = JS_DupValue(ctx, r->left);
        JS_FreeValue(ctx, rope);
        return ret;
    }
    if (string_buffer_init2(ctx, b, r->len, r->is_wide_char))
        goto fail;
    if (string_buffer_concat_value(b, rope))
        goto fail;
    ret = string_buffer_end(b);
    if (js_rc(r)->ref_count > 1) {
        /* update the rope so that it won't need to be linearized again */
        JS_FreeValue(ctx, r->left);
        JS_FreeValue(ctx, r->right);
        r->left = JS_DupValue(ctx, ret);
        r->right = JS_AtomToString(ctx, JS_ATOM_empty_string);
    }
    JS_FreeValue(ctx, rope);
    return ret;
 fail:
    JS_FreeValue(ctx, rope);
    return JS_EXCEPTION;
}

static JSValue js_rebalancee_string_rope(JSContext *ctx, JSValueConst rope);

/* op1 and op2 must be strings or string ropes */
static JSValue js_new_string_rope(JSContext *ctx, JSValue op1, JSValue op2)
{
    uint32_t len;
    int is_wide_char, depth;
    JSStringRope *r;
    JSValue res;
    
    if (JS_VALUE_GET_TAG(op1) == JS_TAG_STRING) {
        JSString *p1 = JS_VALUE_GET_STRING(op1);
        len = p1->len;
        is_wide_char = p1->is_wide_char;
        depth = 0;
    } else {
        JSStringRope *r1 = JS_VALUE_GET_STRING_ROPE(op1);
        len = r1->len;
        is_wide_char = r1->is_wide_char;
        depth = r1->depth;
    }

    if (JS_VALUE_GET_TAG(op2) == JS_TAG_STRING) {
        JSString *p2 = JS_VALUE_GET_STRING(op2);
        len += p2->len;
        is_wide_char |= p2->is_wide_char;
    } else {
        JSStringRope *r2 = JS_VALUE_GET_STRING_ROPE(op2);
        len += r2->len;
        is_wide_char |= r2->is_wide_char;
        depth = max_int(depth, r2->depth);
    }
    if (len > JS_STRING_LEN_MAX) {
        JS_ThrowInternalError(ctx, "string too long");
        goto fail;
    }
    r = js_malloc(ctx, sizeof(*r));
    if (!r)
        goto fail;
    js_rc(r)->ref_count = 1;
    r->len = len;
    r->is_wide_char = is_wide_char;
    r->depth = depth + 1;
    r->left = op1;
    r->right = op2;
    res = JS_MKPTR(JS_TAG_STRING_ROPE, r);
    if (r->depth > JS_STRING_ROPE_MAX_DEPTH) {
        JSValue res2;
#ifdef DUMP_ROPE_REBALANCE
        printf("rebalance: initial depth=%d\n", r->depth);
#endif
        res2 = js_rebalancee_string_rope(ctx, res);
#ifdef DUMP_ROPE_REBALANCE
        if (JS_VALUE_GET_TAG(res2) == JS_TAG_STRING_ROPE) 
            printf("rebalance: final depth=%d\n", JS_VALUE_GET_STRING_ROPE(res2)->depth);
#endif
        JS_FreeValue(ctx, res);
        return res2;
    } else {
        return res;
    }
 fail:
    JS_FreeValue(ctx, op1);
    JS_FreeValue(ctx, op2);
    return JS_EXCEPTION;
}

#define ROPE_N_BUCKETS 44

/* Fibonacii numbers starting from F_2 */
static const uint32_t rope_bucket_len[ROPE_N_BUCKETS] = {
          1,          2,          3,          5,
          8,         13,         21,         34,
         55,         89,        144,        233,
        377,        610,        987,       1597,
       2584,       4181,       6765,      10946,
      17711,      28657,      46368,      75025,
     121393,     196418,     317811,     514229,
     832040,    1346269,    2178309,    3524578,
    5702887,    9227465,   14930352,   24157817,
   39088169,   63245986,  102334155,  165580141,
  267914296,  433494437,  701408733, 1134903170, /* > JS_STRING_LEN_MAX */
};

static int js_rebalancee_string_rope_rec(JSContext *ctx, JSValue *buckets,
                                          JSValueConst val)
{
    if (JS_VALUE_GET_TAG(val) == JS_TAG_STRING) {
        JSString *p = JS_VALUE_GET_STRING(val);
        uint32_t len, i;
        JSValue a, b;
        
        len = p->len;
        if (len == 0)
            return 0; /* nothing to do */
        /* find the bucket i so that rope_bucket_len[i] <= len <
           rope_bucket_len[i + 1] and concatenate the ropes in the
           buckets before */
        a = JS_NULL;
        i = 0;
        while (len >= rope_bucket_len[i + 1]) {
            b = buckets[i];
            if (!JS_IsNull(b)) {
                buckets[i] = JS_NULL;
                if (JS_IsNull(a)) {
                    a = b;
                } else {
                    a = js_new_string_rope(ctx, b, a);
                    if (JS_IsException(a))
                        return -1;
                }
            }
            i++;
        }
        if (!JS_IsNull(a)) {
            a = js_new_string_rope(ctx, a, JS_DupValue(ctx, val));
            if (JS_IsException(a))
                return -1;
        } else {
            a = JS_DupValue(ctx, val);
        }
        while (!JS_IsNull(buckets[i])) {
            a = js_new_string_rope(ctx, buckets[i], a);
            buckets[i] = JS_NULL;
            if (JS_IsException(a))
                return -1;
            i++;
        }
        buckets[i] = a;
    } else {
        JSStringRope *r = JS_VALUE_GET_STRING_ROPE(val);
        js_rebalancee_string_rope_rec(ctx, buckets, r->left);
        js_rebalancee_string_rope_rec(ctx, buckets, r->right);
    }
    return 0;
}

/* Return a new rope which is balanced. Algorithm from "Ropes: an
   Alternative to Strings", Hans-J. Boehm, Russ Atkinson and Michael
   Plass. */
static JSValue js_rebalancee_string_rope(JSContext *ctx, JSValueConst rope)
{
    JSValue buckets[ROPE_N_BUCKETS], a, b;
    int i;
    
    for(i = 0; i < ROPE_N_BUCKETS; i++)
        buckets[i] = JS_NULL;
    if (js_rebalancee_string_rope_rec(ctx, buckets, rope))
        goto fail;
    a = JS_NULL;
    for(i = 0; i < ROPE_N_BUCKETS; i++) {
        b = buckets[i];
        if (!JS_IsNull(b)) {
            buckets[i] = JS_NULL;
            if (JS_IsNull(a)) {
                a = b;
            } else {
                a = js_new_string_rope(ctx, b, a);
                if (JS_IsException(a))
                    goto fail;
            }
        }
    }
    /* fail safe */
    if (JS_IsNull(a))
        return JS_AtomToString(ctx, JS_ATOM_empty_string);
    else
        return a;
 fail:
    for(i = 0; i < ROPE_N_BUCKETS; i++) {
        JS_FreeValue(ctx, buckets[i]);
    }
    return JS_EXCEPTION;
}

/* op1 and op2 are converted to strings. For convenience, op1 or op2 =
   JS_EXCEPTION are accepted and return JS_EXCEPTION.  */
static JSValue JS_ConcatString(JSContext *ctx, JSValue op1, JSValue op2)
{
    JSString *p1, *p2;

    if (unlikely(JS_VALUE_GET_TAG(op1) != JS_TAG_STRING &&
                 JS_VALUE_GET_TAG(op1) != JS_TAG_STRING_ROPE)) {
        op1 = JS_ToStringFree(ctx, op1);
        if (JS_IsException(op1)) {
            JS_FreeValue(ctx, op2);
            return JS_EXCEPTION;
        }
    }
    if (unlikely(JS_VALUE_GET_TAG(op2) != JS_TAG_STRING &&
                 JS_VALUE_GET_TAG(op2) != JS_TAG_STRING_ROPE)) {
        op2 = JS_ToStringFree(ctx, op2);
        if (JS_IsException(op2)) {
            JS_FreeValue(ctx, op1);
            return JS_EXCEPTION;
        }
    }

    /* "" + s is s. This was checked only in the branch below where op2 is a
     * ROPE, so in the common string+string case it fell through to
     * JS_ConcatString2 -> JS_ConcatStringInPlace, which refuses because the
     * canonical empty string has ref_count != 1, and then JS_ConcatString1
     * ALLOCATED A FULL COPY of s. Above the rope threshold it instead built a
     * rope node with an empty left child, which some later consumer has to
     * flatten. The identity was already known; it just was not applied on both
     * sides. */
    if (JS_VALUE_GET_TAG(op1) == JS_TAG_STRING) {
        p1 = JS_VALUE_GET_STRING(op1);
        if (p1->len == 0) {
            JS_FreeValue(ctx, op1);
            return op2;
        }
    }

    /* normal concatenation for short strings */
    if (JS_VALUE_GET_TAG(op2) == JS_TAG_STRING) {
        p2 = JS_VALUE_GET_STRING(op2);
        if (p2->len == 0) {
            JS_FreeValue(ctx, op2);
            return op1;
        }
        if (p2->len <= JS_STRING_ROPE_SHORT_LEN) {
            if (JS_VALUE_GET_TAG(op1) == JS_TAG_STRING) {
                p1 = JS_VALUE_GET_STRING(op1);
                if (p1->len <= JS_STRING_ROPE_SHORT2_LEN) {
                    return JS_ConcatString2(ctx, op1, op2);
                } else {
                    return js_new_string_rope(ctx, op1, op2);
                }
            } else {
                JSStringRope *r1;
                r1 = JS_VALUE_GET_STRING_ROPE(op1);
                if (JS_VALUE_GET_TAG(r1->right) == JS_TAG_STRING &&
                    JS_VALUE_GET_STRING(r1->right)->len <= JS_STRING_ROPE_SHORT_LEN) {
                    JSValue val, ret;
                    val = JS_ConcatString2(ctx, JS_DupValue(ctx, r1->right), op2);
                    if (JS_IsException(val)) {
                        JS_FreeValue(ctx, op1);
                        return JS_EXCEPTION;
                    }
                    ret = js_new_string_rope(ctx, JS_DupValue(ctx, r1->left), val);
                    JS_FreeValue(ctx, op1);
                    return ret;
                }
            }
        }
    } else if (JS_VALUE_GET_TAG(op1) == JS_TAG_STRING) {
        JSStringRope *r2;
        p1 = JS_VALUE_GET_STRING(op1);
        if (p1->len == 0) {
            JS_FreeValue(ctx, op1);
            return op2;
        }
        r2 = JS_VALUE_GET_STRING_ROPE(op2);
        if (JS_VALUE_GET_TAG(r2->left) == JS_TAG_STRING &&
            JS_VALUE_GET_STRING(r2->left)->len <= JS_STRING_ROPE_SHORT_LEN) {
            JSValue val, ret;
            val = JS_ConcatString2(ctx, op1, JS_DupValue(ctx, r2->left));
            if (JS_IsException(val)) {
                JS_FreeValue(ctx, op2);
                return JS_EXCEPTION;
            }
            ret = js_new_string_rope(ctx, val, JS_DupValue(ctx, r2->right));
            JS_FreeValue(ctx, op2);
            return ret;
        }
    }
    return js_new_string_rope(ctx, op1, op2);
}

