#define JS_ATOM_TAG_INT (1U << 31)
#define JS_ATOM_MAX_INT (JS_ATOM_TAG_INT - 1)
#define JS_ATOM_MAX ((1U << 30) - 1)

#define JS_ATOM_COUNT_RESIZE(n) ((n) * 2)

static inline BOOL __JS_AtomIsConst(JSAtom v)
{
#if defined(DUMP_LEAKS) && DUMP_LEAKS > 1
    return (int32_t)v <= 0;
#else
    return (int32_t)v < JS_ATOM_END;
#endif
}

inline BOOL __JS_AtomIsTaggedInt(JSAtom v)
{
    return (v & JS_ATOM_TAG_INT) != 0;
}

inline JSAtom __JS_AtomFromUInt32(uint32_t v)
{
    return v | JS_ATOM_TAG_INT;
}

inline uint32_t __JS_AtomToUInt32(JSAtom atom)
{
    return atom & ~JS_ATOM_TAG_INT;
}

static inline int is_num(int c)
{
    return c >= '0' && c <= '9';
}

static inline BOOL is_num_string(uint32_t* pval, const JSString* p)
{
    uint32_t n;
    uint64_t n64;
    int c, i, len;

    len = p->len;
    if (len == 0 || len > 10)
        return FALSE;
    c = string_get(p, 0);
    if (is_num(c)) {
        if (c == '0') {
            if (len != 1)
                return FALSE;
            n = 0;
        } else {
            n = c - '0';
            for (i = 1; i < len; i++) {
                c = string_get(p, i);
                if (!is_num(c))
                    return FALSE;
                n64 = (uint64_t)n * 10 + (c - '0');
                if ((n64 >> 32) != 0)
                    return FALSE;
                n = n64;
            }
        }
        *pval = n;
        return TRUE;
    } else {
        return FALSE;
    }
}

#define HASH_STRING_MUL1 263u
#define HASH_STRING_MUL2 69169u
#define HASH_STRING_MUL3 18191447u
#define HASH_STRING_MUL4 489383265u
#define HASH_STRING_FOLD_MIN 8

static inline uint32_t hash_string8_serial(const uint8_t* str, size_t len,
    uint32_t h)
{
    size_t i;

    for (i = 0; i < len; i++)
        h = h * HASH_STRING_MUL1 + str[i];
    return h;
}

static no_inline uint32_t hash_string8_folded(const uint8_t* str, size_t len,
    uint32_t h)
{
    size_t i = 0;

    for (; i + 4 <= len; i += 4) {
        h = h * HASH_STRING_MUL4 + (uint32_t)str[i] * HASH_STRING_MUL3 + (uint32_t)str[i + 1] * HASH_STRING_MUL2 + (uint32_t)str[i + 2] * HASH_STRING_MUL1 + (uint32_t)str[i + 3];
    }
    for (; i < len; i++)
        h = h * HASH_STRING_MUL1 + str[i];
    return h;
}

static inline uint32_t hash_string8(const uint8_t* str, size_t len, uint32_t h)
{
    if (unlikely(len >= HASH_STRING_FOLD_MIN))
        return hash_string8_folded(str, len, h);
    return hash_string8_serial(str, len, h);
}

static inline uint32_t hash_string16(const uint16_t* str,
    size_t len, uint32_t h)
{
    size_t i;

    for (i = 0; i < len; i++)
        h = h * HASH_STRING_MUL1 + str[i];
    return h;
}

static uint32_t hash_string(const JSString* str, uint32_t h)
{
    if (str->is_wide_char)
        h = hash_string16(js_str_data16(str), str->len, h);
    else
        h = hash_string8(js_str_data8(str), str->len, h);
    return h;
}

static uint32_t hash_string_rope(JSValueConst val, uint32_t h)
{
    if (JS_VALUE_GET_TAG(val) == JS_TAG_STRING) {
        return hash_string(JS_VALUE_GET_STRING(val), h);
    } else {
        JSStringRope* r = JS_VALUE_GET_STRING_ROPE(val);
        h = hash_string_rope(r->left, h);
        return hash_string_rope(r->right, h);
    }
}

static __maybe_unused void JS_DumpChar(FILE* fo, int c, int sep)
{
    if (c == sep || c == '\\') {
        fputc('\\', fo);
        fputc(c, fo);
    } else if (c >= ' ' && c <= 126) {
        fputc(c, fo);
    } else if (c == '\n') {
        fputc('\\', fo);
        fputc('n', fo);
    } else {
        fprintf(fo, "\\u%04x", c);
    }
}

static __maybe_unused void JS_DumpString(JSRuntime* rt, const JSString* p)
{
    int i, sep;

    if (p == NULL) {
        printf("<null>");
        return;
    }
    printf("%d", js_rc(DYN_UNCONST(p))->ref_count);
    sep = (js_rc(DYN_UNCONST(p))->ref_count == 1) ? '\"' : '\'';
    putchar(sep);
    for (i = 0; i < p->len; i++) {
        JS_DumpChar(stdout, string_get(p, i), sep);
    }
    putchar(sep);
}

static __maybe_unused void JS_DumpAtoms(JSRuntime* rt)
{
    JSAtomStruct* p;
    int h, i;
    printf("JSAtom count=%d size=%d hash_size=%d:\n",
        rt->atom_count, rt->atom_size, rt->atom_hash_size);
    printf("JSAtom hash table: {\n");
    for (i = 0; i < rt->atom_hash_size; i++) {
        h = rt->atom_hash[i];
        if (h) {
            printf("  %d:", i);
            while (h) {
                p = rt->atom_array[h];
                printf(" ");
                JS_DumpString(rt, p);
                h = p->hash_next;
            }
            printf("\n");
        }
    }
    printf("}\n");
    printf("JSAtom table: {\n");
    for (i = 0; i < rt->atom_size; i++) {
        p = rt->atom_array[i];
        if (!atom_is_free(p)) {
            printf("  %d: { %d %08x ", i, p->atom_type, p->hash);
            if (!(p->len == 0 && p->is_wide_char != 0))
                JS_DumpString(rt, p);
            printf(" %d }\n", p->hash_next);
        }
    }
    printf("}\n");
}

static int JS_ResizeAtomHash(JSRuntime* rt, int new_hash_size)
{
    JSAtomStruct* p;
    uint32_t new_hash_mask, h, i, hash_next1, j, *new_hash;

    assert((new_hash_size & (new_hash_size - 1)) == 0);
    new_hash_mask = new_hash_size - 1;
    new_hash = js_mallocz_rt(rt, sizeof(rt->atom_hash[0]) * new_hash_size);
    if (!new_hash)
        return -1;
    for (i = 0; i < rt->atom_hash_size; i++) {
        h = rt->atom_hash[i];
        while (h != 0) {
            p = rt->atom_array[h];
            hash_next1 = p->hash_next;
            j = p->hash & new_hash_mask;
            p->hash_next = new_hash[j];
            new_hash[j] = h;
            h = hash_next1;
        }
    }
    js_free_rt(rt, rt->atom_hash);
    rt->atom_hash = new_hash;
    rt->atom_hash_size = new_hash_size;
    rt->atom_count_resize = JS_ATOM_COUNT_RESIZE(new_hash_size);
    return 0;
}

static int JS_InitAtoms(JSRuntime* rt)
{
    int i, len, atom_type;
    const char* p;

    rt->atom_hash_size = 0;
    rt->atom_hash = NULL;
    rt->atom_count = 0;
    rt->atom_size = 0;
    rt->atom_free_index = 0;
    if (dyn_os_entropy(&rt->atom_hash_seed, sizeof(rt->atom_hash_seed)) < 0)
        return -1;
    if (JS_ResizeAtomHash(rt, 512))
        return -1;

    p = js_atom_init;
    for (i = 1; i < JS_ATOM_END; i++) {
        if (i == JS_ATOM_Private_brand)
            atom_type = JS_ATOM_TYPE_PRIVATE;
        else if (i >= JS_ATOM_Symbol_toPrimitive)
            atom_type = JS_ATOM_TYPE_SYMBOL;
        else
            atom_type = JS_ATOM_TYPE_STRING;
        len = strlen(p);
        if (__JS_NewAtomInit(rt, p, len, atom_type) == JS_ATOM_NULL)
            return -1;
        p = p + len + 1;
    }
    return 0;
}

static JSAtom JS_DupAtomRT(JSRuntime* rt, JSAtom v)
{
    JSAtomStruct* p;

    if (!__JS_AtomIsConst(v)) {
        p = rt->atom_array[v];
        js_rc(p)->ref_count++;
    }
    return v;
}

JSAtom JS_DupAtom(JSContext* ctx, JSAtom v)
{
    JSRuntime* rt;
    JSAtomStruct* p;

    if (!__JS_AtomIsConst(v)) {
        rt = ctx->rt;
        p = rt->atom_array[v];
        js_rc(p)->ref_count++;
    }
    return v;
}

JSAtomKindEnum JS_AtomGetKind(JSContext* ctx, JSAtom v)
{
    JSRuntime* rt;
    JSAtomStruct* p;

    rt = ctx->rt;
    if (__JS_AtomIsTaggedInt(v))
        return JS_ATOM_KIND_STRING;
    p = rt->atom_array[v];
    switch (p->atom_type) {
    case JS_ATOM_TYPE_STRING:
        return JS_ATOM_KIND_STRING;
    case JS_ATOM_TYPE_GLOBAL_SYMBOL:
        return JS_ATOM_KIND_SYMBOL;
    case JS_ATOM_TYPE_SYMBOL:
        if (p->hash == JS_ATOM_HASH_PRIVATE)
            return JS_ATOM_KIND_PRIVATE;
        else
            return JS_ATOM_KIND_SYMBOL;
    default:
        abort();
    }
}

BOOL JS_AtomIsString(JSContext* ctx, JSAtom v)
{
    return JS_AtomGetKind(ctx, v) == JS_ATOM_KIND_STRING;
}

static JSAtom js_get_atom_index(JSRuntime* rt, JSAtomStruct* p)
{
    uint32_t i = p->hash_next;
    if (p->atom_type != JS_ATOM_TYPE_SYMBOL) {
        JSAtomStruct* p1;

        i = rt->atom_hash[p->hash & (rt->atom_hash_size - 1)];
        p1 = rt->atom_array[i];
        while (p1 != p) {
            assert(i != 0);
            i = p1->hash_next;
            p1 = rt->atom_array[i];
        }
    }
    return i;
}

static JSAtom __JS_NewAtom(JSRuntime* rt, JSString* str, int atom_type)
{
    uint32_t h, h1, i;
    JSAtomStruct* p;
    int len;

#if 0
    printf("__JS_NewAtom: ");  JS_DumpString(rt, str); printf("\n");
#endif
    if (atom_type < JS_ATOM_TYPE_SYMBOL) {
        if (str->atom_type == atom_type) {
            i = js_get_atom_index(rt, str);
            if (__JS_AtomIsConst(i))
                js_rc(str)->ref_count--;
            return i;
        }
        len = str->len;
        h = hash_string(str, (uint32_t)rt->atom_hash_seed ^ (uint32_t)atom_type);
        h &= JS_ATOM_HASH_MASK;
        h1 = h & (rt->atom_hash_size - 1);
        i = rt->atom_hash[h1];
        while (i != 0) {
            p = rt->atom_array[i];
            if (p->hash == h && p->atom_type == atom_type && p->len == len && js_string_memcmp(p, 0, str, 0, len) == 0) {
                if (!__JS_AtomIsConst(i))
                    js_rc(p)->ref_count++;
                goto done;
            }
            i = p->hash_next;
        }
    } else {
        h1 = 0;
        if (atom_type == JS_ATOM_TYPE_SYMBOL) {
            h = 0;
        } else {
            h = JS_ATOM_HASH_PRIVATE;
            atom_type = JS_ATOM_TYPE_SYMBOL;
        }
    }

    if (rt->atom_free_index == 0) {
        uint32_t new_size, start;
        JSAtomStruct** new_array;

        new_size = max_int(711, rt->atom_size * 3 / 2);
        if (new_size > JS_ATOM_MAX)
            goto fail;
        new_array = js_realloc_rt(rt, rt->atom_array, sizeof(*new_array) * new_size);
        if (!new_array)
            goto fail;
        start = rt->atom_size;
        if (start == 0) {
            p = js_mallocz_rt(rt, sizeof(JSAtomStruct));
            if (!p) {
                js_free_rt(rt, new_array);
                goto fail;
            }
            js_rc(p)->ref_count = 1;
            p->atom_type = JS_ATOM_TYPE_SYMBOL;
#ifdef DUMP_LEAKS
            list_add_tail(&p->link, &rt->string_list);
#endif
            new_array[0] = p;
            rt->atom_count++;
            start = 1;
        }
        rt->atom_size = new_size;
        rt->atom_array = new_array;
        rt->atom_free_index = start;
        for (i = start; i < new_size; i++) {
            uint32_t next;
            if (i == (new_size - 1))
                next = 0;
            else
                next = i + 1;
            rt->atom_array[i] = atom_set_free(next);
        }
    }

    if (str) {
        if (str->atom_type == 0 && !str->is_slice) {
            p = str;
            p->atom_type = atom_type;
        } else if (str->is_slice && str->is_wide_char) {
            const uint16_t* src16 = js_str_data16(str);
            int si;
            uint16_t c = 0;
            for (si = 0; si < str->len; si++)
                c |= src16[si];
            if (c <= 0xFF) {
                p = js_malloc_rt(rt, sizeof(JSString) + str->len + 1);
                if (unlikely(!p))
                    goto fail;
                js_rc(p)->ref_count = 1;
                p->is_wide_char = 0;
                p->is_slice = 0;
                p->len = str->len;
#ifdef DUMP_LEAKS
                list_add_tail(&p->link, &rt->string_list);
#endif
                for (si = 0; si < str->len; si++)
                    p->u.str8[si] = src16[si];
                p->u.str8[str->len] = '\0';
            } else {
                p = js_malloc_rt(rt, sizeof(JSString) + (str->len << 1));
                if (unlikely(!p))
                    goto fail;
                js_rc(p)->ref_count = 1;
                p->is_wide_char = 1;
                p->is_slice = 0;
                p->len = str->len;
#ifdef DUMP_LEAKS
                list_add_tail(&p->link, &rt->string_list);
#endif
                memcpy(p->u.str16, src16, str->len * 2);
            }
            js_free_string(rt, str);
        } else {
            p = js_malloc_rt(rt, sizeof(JSString) + (str->len << str->is_wide_char) + 1 - str->is_wide_char);
            if (unlikely(!p))
                goto fail;
            js_rc(p)->ref_count = 1;
            p->is_wide_char = str->is_wide_char;
            p->is_slice = 0;
            p->len = str->len;
#ifdef DUMP_LEAKS
            list_add_tail(&p->link, &rt->string_list);
#endif
            memcpy(p->u.str8, str->is_wide_char ? (const void*)js_str_data16(str) : (const void*)js_str_data8(str),
                (str->len << str->is_wide_char) + 1 - str->is_wide_char);
            js_free_string(rt, str);
        }
    } else {
        p = js_malloc_rt(rt, sizeof(JSAtomStruct));
        if (!p)
            return JS_ATOM_NULL;
        js_rc(p)->ref_count = 1;
        p->is_wide_char = 1;
        p->is_slice = 0;
        p->len = 0;
#ifdef DUMP_LEAKS
        list_add_tail(&p->link, &rt->string_list);
#endif
    }

    i = rt->atom_free_index;
    rt->atom_free_index = atom_get_free(rt->atom_array[i]);
    rt->atom_array[i] = p;

    p->hash = h;
    p->hash_next = i;
    p->atom_type = atom_type;

    rt->atom_count++;

    if (atom_type != JS_ATOM_TYPE_SYMBOL) {
        p->hash_next = rt->atom_hash[h1];
        rt->atom_hash[h1] = i;
        if (unlikely(rt->atom_count >= rt->atom_count_resize))
            JS_ResizeAtomHash(rt, rt->atom_hash_size * 2);
    }

    return i;

fail:
    i = JS_ATOM_NULL;
done:
    if (str)
        js_free_string(rt, str);
    return i;
}

static JSAtom __JS_NewAtomInit(JSRuntime* rt, const char* str, int len,
    int atom_type)
{
    JSString* p;
    p = js_alloc_string_rt(rt, len, 0);
    if (!p)
        return JS_ATOM_NULL;
    memcpy(p->u.str8, str, len);
    p->u.str8[len] = '\0';
    return __JS_NewAtom(rt, p, atom_type);
}

static JSAtom __JS_FindAtom(JSRuntime* rt, const char* str, size_t len,
    int atom_type)
{
    uint32_t h, h1, i;
    JSAtomStruct* p;

    h = hash_string8((const uint8_t*)str, len,
        (uint32_t)rt->atom_hash_seed ^ JS_ATOM_TYPE_STRING);
    h &= JS_ATOM_HASH_MASK;
    h1 = h & (rt->atom_hash_size - 1);
    i = rt->atom_hash[h1];
    while (i != 0) {
        p = rt->atom_array[i];
        if (p->hash == h && p->atom_type == JS_ATOM_TYPE_STRING && p->len == len && p->is_wide_char == 0 && memcmp(p->u.str8, str, len) == 0) {
            if (!__JS_AtomIsConst(i))
                js_rc(p)->ref_count++;
            return i;
        }
        i = p->hash_next;
    }
    return JS_ATOM_NULL;
}

static void JS_FreeAtomStruct(JSRuntime* rt, JSAtomStruct* p)
{
#if 0
    if (unlikely(i == JS_ATOM_NULL)) {
        js_rc(p)->ref_count = INT32_MAX / 2;
        return;
    }
#endif
    uint32_t i = p->hash_next;
    if (p->atom_type != JS_ATOM_TYPE_SYMBOL) {
        JSAtomStruct *p0, *p1;
        uint32_t h0;

        h0 = p->hash & (rt->atom_hash_size - 1);
        i = rt->atom_hash[h0];
        p1 = rt->atom_array[i];
        if (p1 == p) {
            rt->atom_hash[h0] = p1->hash_next;
        } else {
            for (;;) {
                assert(i != 0);
                p0 = p1;
                i = p1->hash_next;
                p1 = rt->atom_array[i];
                if (p1 == p) {
                    p0->hash_next = p1->hash_next;
                    break;
                }
            }
        }
    }
    rt->atom_array[i] = atom_set_free(rt->atom_free_index);
    rt->atom_free_index = i;
#ifdef DUMP_LEAKS
    list_del(&p->link);
#endif
    if (p->atom_type == JS_ATOM_TYPE_SYMBOL && p->hash != JS_ATOM_HASH_PRIVATE && p->hash != 0) {
    } else {
        js_free_rt(rt, p);
    }
    rt->atom_count--;
    assert(rt->atom_count >= 0);
}

static void __JS_FreeAtom(JSRuntime* rt, uint32_t i)
{
    JSAtomStruct* p;

    p = rt->atom_array[i];
    if (--js_rc(p)->ref_count > 0)
        return;
    JS_FreeAtomStruct(rt, p);
}

JSAtom JS_NewAtomStr(JSContext* ctx, JSString* p)
{
    JSRuntime* rt = ctx->rt;
    uint32_t n;
    if (is_num_string(&n, p)) {
        if (n <= JS_ATOM_MAX_INT) {
            js_free_string(rt, p);
            return __JS_AtomFromUInt32(n);
        }
    }
    return __JS_NewAtom(rt, p, JS_ATOM_TYPE_STRING);
}

static size_t count_ascii(const uint8_t* buf, size_t len)
{
    const uint8_t *p = buf, *p_end = buf + len;
    const uint8_t* p_word_end;

    while (p < p_end && ((uintptr_t)p & (sizeof(uint64_t) - 1)) != 0) {
        if (*p >= 128)
            return p - buf;
        p++;
    }

    p_word_end = p + ((size_t)(p_end - p) & ~(size_t)(sizeof(uint64_t) - 1));
    for (; p < p_word_end; p += sizeof(uint64_t)) {
        uint64_t v, hi;
        memcpy(&v, p, sizeof(v));
        hi = v & UINT64_C(0x8080808080808080);
        if (hi) {
#ifdef WORDS_BIGENDIAN
            return (p - buf) + (size_t)(__builtin_clzll(hi) >> 3);
#else
            return (p - buf) + (size_t)(__builtin_ctzll(hi) >> 3);
#endif
        }
    }

    while (p < p_end && *p < 128)
        p++;
    return p - buf;
}

JSAtom JS_NewAtomLen(JSContext* ctx, const char* str, size_t len)
{
    JSValue val;

    if (len == 0 || (!is_digit(*str) && count_ascii((const uint8_t*)str, len) == len)) {
        JSAtom atom = __JS_FindAtom(ctx->rt, str, len, JS_ATOM_TYPE_STRING);
        if (atom)
            return atom;
    }
    val = JS_NewStringLen(ctx, str, len);
    if (JS_IsException(val))
        return JS_ATOM_NULL;
    return JS_NewAtomStr(ctx, JS_VALUE_GET_STRING(val));
}

JSAtom JS_NewAtom(JSContext* ctx, const char* str)
{
    return JS_NewAtomLen(ctx, str, strlen(str));
}

JSAtom JS_NewAtomUInt32(JSContext* ctx, uint32_t n)
{
    if (n <= JS_ATOM_MAX_INT) {
        return __JS_AtomFromUInt32(n);
    } else {
        char buf[11];
        JSValue val;
        size_t len;
        len = u32toa(buf, n);
        val = js_new_string8_len(ctx, buf, len);
        if (JS_IsException(val))
            return JS_ATOM_NULL;
        return __JS_NewAtom(ctx->rt, JS_VALUE_GET_STRING(val),
            JS_ATOM_TYPE_STRING);
    }
}

static JSAtom JS_NewAtomInt64(JSContext* ctx, int64_t n)
{
    if ((uint64_t)n <= JS_ATOM_MAX_INT) {
        return __JS_AtomFromUInt32((uint32_t)n);
    } else {
        char buf[24];
        JSValue val;
        size_t len;
        len = i64toa(buf, n);
        val = js_new_string8_len(ctx, buf, len);
        if (JS_IsException(val))
            return JS_ATOM_NULL;
        return __JS_NewAtom(ctx->rt, JS_VALUE_GET_STRING(val),
            JS_ATOM_TYPE_STRING);
    }
}

static JSValue JS_NewSymbol(JSContext* ctx, JSString* p, int atom_type)
{
    JSRuntime* rt = ctx->rt;
    JSAtom atom;
    atom = __JS_NewAtom(rt, p, atom_type);
    if (atom == JS_ATOM_NULL)
        return JS_ThrowOutOfMemory(ctx);
    return JS_MKPTR(JS_TAG_SYMBOL, rt->atom_array[atom]);
}

static JSValue JS_NewSymbolFromAtom(JSContext* ctx, JSAtom descr,
    int atom_type)
{
    JSRuntime* rt = ctx->rt;
    JSString* p;

    assert(!__JS_AtomIsTaggedInt(descr));
    assert(descr < rt->atom_size);
    p = rt->atom_array[descr];
    JS_DupValue(ctx, JS_MKPTR(JS_TAG_STRING, p));
    return JS_NewSymbol(ctx, p, atom_type);
}

static const char* JS_AtomGetStrRT(JSRuntime* rt, char* buf, int buf_size,
    JSAtom atom)
{
    if (__JS_AtomIsTaggedInt(atom)) {
        snprintf(buf, buf_size, "%u", __JS_AtomToUInt32(atom));
    } else {
        JSAtomStruct* p;
        assert(atom < rt->atom_size);
        if (atom == JS_ATOM_NULL) {
            snprintf(buf, buf_size, "<null>");
        } else {
            int i, c;
            char* q;
            JSString* str;

            q = buf;
            p = rt->atom_array[atom];
            assert(!atom_is_free(p));
            str = p;
            if (str) {
                if (!str->is_wide_char) {
                    c = 0;
                    for (i = 0; i < str->len; i++) {
                        c |= str->u.str8[i];
                    }
                    if (c < 0x80)
                        return (const char*)str->u.str8;
                }
                for (i = 0; i < str->len; i++) {
                    c = string_get(str, i);
                    if ((q - buf) >= buf_size - UTF8_CHAR_LEN_MAX)
                        break;
                    if (c < 128) {
                        *q++ = c;
                    } else {
                        q += unicode_to_utf8((uint8_t*)q, c);
                    }
                }
            }
            *q = '\0';
        }
    }
    return buf;
}

const char* JS_AtomGetStr(JSContext* ctx, char* buf, int buf_size, JSAtom atom)
{
    return JS_AtomGetStrRT(ctx->rt, buf, buf_size, atom);
}

const char* JS_AtomBorrowASCII(JSContext* ctx, size_t* plen, JSAtom atom)
{
    JSString* str;
    uint32_t i;
    int c = 0;

    if (__JS_AtomIsTaggedInt(atom) || atom == JS_ATOM_NULL
        || atom >= ctx->rt->atom_size)
        return NULL;
    str = ctx->rt->atom_array[atom];
    if (!str || atom_is_free(str) || str->is_wide_char)
        return NULL;
    for (i = 0; i < str->len; i++)
        c |= str->u.str8[i];
    if (c >= 0x80)
        return NULL;
    *plen = str->len;
    return (const char*)str->u.str8;
}

static JSValue __JS_AtomToValue(JSContext* ctx, JSAtom atom, BOOL force_string)
{
    char buf[ATOM_GET_STR_BUF_SIZE];

    if (__JS_AtomIsTaggedInt(atom)) {
        size_t len = u32toa(buf, __JS_AtomToUInt32(atom));
        return js_new_string8_len(ctx, buf, len);
    } else {
        JSRuntime* rt = ctx->rt;
        JSAtomStruct* p;
        assert(atom < rt->atom_size);
        p = rt->atom_array[atom];
        if (p->atom_type == JS_ATOM_TYPE_STRING) {
            goto ret_string;
        } else if (force_string) {
            if (p->len == 0 && p->is_wide_char != 0) {
                p = rt->atom_array[JS_ATOM_empty_string];
            }
        ret_string:
            return JS_DupValue(ctx, JS_MKPTR(JS_TAG_STRING, p));
        } else {
            return JS_DupValue(ctx, JS_MKPTR(JS_TAG_SYMBOL, p));
        }
    }
}

JSValue JS_AtomToValue(JSContext* ctx, JSAtom atom)
{
    return __JS_AtomToValue(ctx, atom, FALSE);
}

JSValue JS_AtomToString(JSContext* ctx, JSAtom atom)
{
    return __JS_AtomToValue(ctx, atom, TRUE);
}

BOOL JS_AtomIsArrayIndex(JSContext* ctx, uint32_t* pval, JSAtom atom)
{
    if (__JS_AtomIsTaggedInt(atom)) {
        *pval = __JS_AtomToUInt32(atom);
        return TRUE;
    } else {
        JSRuntime* rt = ctx->rt;
        JSAtomStruct* p;
        uint32_t val;

        assert(atom < rt->atom_size);
        p = rt->atom_array[atom];
        if (p->atom_type == JS_ATOM_TYPE_STRING && is_num_string(&val, p) && val != -1) {
            *pval = val;
            return TRUE;
        } else {
            *pval = 0;
            return FALSE;
        }
    }
}

static JSValue JS_AtomIsNumericIndex1(JSContext* ctx, JSAtom atom)
{
    JSRuntime* rt = ctx->rt;
    JSAtomStruct* p1;
    JSString* p;
    int c, ret;
    JSValue num, str;

    if (__JS_AtomIsTaggedInt(atom))
        return JS_NewInt32(ctx, __JS_AtomToUInt32(atom));
    assert(atom < rt->atom_size);
    p1 = rt->atom_array[atom];
    if (p1->atom_type != JS_ATOM_TYPE_STRING)
        return JS_UNDEFINED;
    switch (atom) {
    case JS_ATOM_minus_zero:
        return __JS_NewFloat64(ctx, -0.0);
    case JS_ATOM_Infinity:
        return __JS_NewFloat64(ctx, DYN_INFINITY);
    case JS_ATOM_minus_Infinity:
        return __JS_NewFloat64(ctx, -DYN_INFINITY);
    case JS_ATOM_NaN:
        return __JS_NewFloat64(ctx, DYN_NAN);
    default:
        break;
    }
    p = p1;
    if (p->len == 0)
        return JS_UNDEFINED;
    c = string_get(p, 0);
    if (!is_num(c) && c != '-')
        return JS_UNDEFINED;
    num = JS_ToNumber(ctx, JS_MKPTR(JS_TAG_STRING, p));
    if (JS_IsException(num))
        return num;
    str = JS_ToString(ctx, num);
    if (JS_IsException(str)) {
        JS_FreeValue(ctx, num);
        return str;
    }
    ret = js_string_eq(ctx, p, JS_VALUE_GET_STRING(str));
    JS_FreeValue(ctx, str);
    if (ret) {
        return num;
    } else {
        JS_FreeValue(ctx, num);
        return JS_UNDEFINED;
    }
}

static int JS_AtomIsNumericIndex(JSContext* ctx, JSAtom atom)
{
    JSValue num;
    num = JS_AtomIsNumericIndex1(ctx, atom);
    if (likely(JS_IsUndefined(num)))
        return FALSE;
    if (JS_IsException(num))
        return -1;
    JS_FreeValue(ctx, num);
    return TRUE;
}

void JS_FreeAtom(JSContext* ctx, JSAtom v)
{
    if (!__JS_AtomIsConst(v))
        __JS_FreeAtom(ctx->rt, v);
}

void JS_FreeAtomRT(JSRuntime* rt, JSAtom v)
{
    if (!__JS_AtomIsConst(v))
        __JS_FreeAtom(rt, v);
}

static BOOL JS_AtomSymbolHasDescription(JSContext* ctx, JSAtom v)
{
    JSRuntime* rt;
    JSAtomStruct* p;

    rt = ctx->rt;
    if (__JS_AtomIsTaggedInt(v))
        return FALSE;
    p = rt->atom_array[v];
    return (((p->atom_type == JS_ATOM_TYPE_SYMBOL && p->hash != JS_ATOM_HASH_PRIVATE) || p->atom_type == JS_ATOM_TYPE_GLOBAL_SYMBOL) && !(p->len == 0 && p->is_wide_char != 0));
}

const char* JS_AtomToCStringLen(JSContext* ctx, size_t* plen, JSAtom atom)
{
    JSValue str;
    const char* cstr;

    str = JS_AtomToString(ctx, atom);
    if (JS_IsException(str)) {
        if (plen)
            *plen = 0;
        return NULL;
    }
    cstr = JS_ToCStringLen(ctx, plen, str);
    JS_FreeValue(ctx, str);
    return cstr;
}

JSAtom js_atom_concat_str(JSContext* ctx, JSAtom name, const char* str1)
{
    JSValue str;
    JSAtom atom;
    const char* cstr;
    char* cstr2;
    size_t len, len1;

    str = JS_AtomToString(ctx, name);
    if (JS_IsException(str))
        return JS_ATOM_NULL;
    cstr = JS_ToCStringLen(ctx, &len, str);
    if (!cstr)
        goto fail;
    len1 = strlen(str1);
    cstr2 = js_malloc(ctx, len + len1 + 1);
    if (!cstr2)
        goto fail;
    memcpy(cstr2, cstr, len);
    memcpy(cstr2 + len, str1, len1);
    cstr2[len + len1] = '\0';
    atom = JS_NewAtomLen(ctx, cstr2, len + len1);
    js_free(ctx, cstr2);
    JS_FreeCString(ctx, cstr);
    JS_FreeValue(ctx, str);
    return atom;
fail:
    JS_FreeCString(ctx, cstr);
    JS_FreeValue(ctx, str);
    return JS_ATOM_NULL;
}

JSAtom js_atom_concat_num(JSContext* ctx, JSAtom name, uint32_t n)
{
    char buf[16];
    size_t len;
    len = u32toa(buf, n);
    buf[len] = '\0';
    return js_atom_concat_str(ctx, name, buf);
}

inline BOOL JS_IsEmptyString(JSValueConst v)
{
    return JS_VALUE_GET_TAG(v) == JS_TAG_STRING && JS_VALUE_GET_STRING(v)->len == 0;
}
