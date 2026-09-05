/* Array.prototype: standard methods plus non-ES extensions.
 *
 * Unity-build fragment: #included into src/dynajs.c, never compiled alone.
 * Split out of the former object_array_iterator.inc.c (byte-identical token
 * stream preserved). */
/* Build a fresh array from obj[start..end) (both already clamped, start<=end).
 * Uses a pre-sized fast array + a direct bulk dup when the source is a fast
 * array (the common case; matches js_array_slice's speed), falling back to the
 * generic per-index read otherwise. js_allocate_fast_array pre-fills every slot
 * with JS_UNDEFINED, so bailing mid-fill and freeing the array is safe. */
static JSValue js_array_ext_build_range(JSContext *ctx, JSValueConst obj,
                                        int64_t start, int64_t end)
{
    JSValue arr, *arrp, *pval;
    JSObject *p;
    int64_t i, n = end - start;
    uint32_t count32;

    if (n <= 0)
        return JS_NewArray(ctx);
    arr = js_allocate_fast_array(ctx, n);
    if (JS_IsException(arr))
        return arr;
    p = JS_VALUE_GET_OBJ(arr);
    pval = p->u.array.u.values;
    if (js_get_fast_array(ctx, obj, &arrp, &count32) && (int64_t)count32 >= end) {
        for (i = start; i < end; i++, pval++)
            *pval = JS_DupValue(ctx, arrp[i]);
    } else {
        for (i = start; i < end; i++, pval++) {
            if (JS_TryGetPropertyInt64(ctx, obj, i, pval) < 0) {
                JS_FreeValue(ctx, arr);
                return JS_EXCEPTION;
            }
        }
    }
    return arr;
}

/* _isEmpty() -> length === 0 */
static JSValue js_array_ext_isEmpty(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    JSValue obj;
    int64_t len;
    (void)argc; (void)argv;
    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj)) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, obj);
    return JS_NewBool(ctx, len == 0);
}

/* _first(n?) -> first element (undefined if empty), or a new array of the first
 * n elements when n is given. */
static JSValue js_array_ext_first(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    JSValue obj, ret = JS_EXCEPTION;
    int64_t len, n;
    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj))
        goto done;
    if (argc == 0 || JS_IsUndefined(argv[0])) {
        if (len == 0)
            ret = JS_UNDEFINED;
        else if (js_array_ext_getel(ctx, obj, 0, &ret))
            ret = JS_EXCEPTION;
        goto done;
    }
    if (JS_ToInt64Sat(ctx, &n, argv[0]))
        goto done;
    if (n < 0)
        n = 0;
    if (n > len)
        n = len;
    ret = js_array_ext_build_range(ctx, obj, 0, n);
 done:
    JS_FreeValue(ctx, obj);
    return ret;
}

/* _last(n?) -> last element (undefined if empty), or a new array of the last n
 * elements (in original order) when n is given. */
static JSValue js_array_ext_last(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    JSValue obj, ret = JS_EXCEPTION;
    int64_t len, n;
    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj))
        goto done;
    if (argc == 0 || JS_IsUndefined(argv[0])) {
        if (len == 0)
            ret = JS_UNDEFINED;
        else if (js_array_ext_getel(ctx, obj, len - 1, &ret))
            ret = JS_EXCEPTION;
        goto done;
    }
    if (JS_ToInt64Sat(ctx, &n, argv[0]))
        goto done;
    if (n < 0)
        n = 0;
    if (n > len)
        n = len;
    ret = js_array_ext_build_range(ctx, obj, len - n, len);
 done:
    JS_FreeValue(ctx, obj);
    return ret;
}

/* shared reducer for _sum / _average: accumulate each element coerced to a
 * double. magic 0 = sum, 1 = average (empty average is 0). */
static JSValue js_array_ext_sum_avg(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv, int magic)
{
    JSValue obj, ret = JS_EXCEPTION;
    int64_t len, i;
    double acc = 0;
    (void)argc; (void)argv;
    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj))
        goto done;
    {   /* fast path: a contiguous all-numeric fast array. The homogeneity scan
         * and the sum loop read only tags/payloads and run NO user JS, so
         * holding arrp across them cannot use-after-free. */
        JSValue *arrp;
        uint32_t count;
        if (js_get_fast_array(ctx, obj, &arrp, &count) && (int64_t)count == len) {
            int homogeneous = 1;
            for (i = 0; i < len; i++) {
                int t = JS_VALUE_GET_TAG(arrp[i]);
                if (t != JS_TAG_INT && !JS_TAG_IS_FLOAT64(t)) { homogeneous = 0; break; }
            }
            if (homogeneous) {
                for (i = 0; i < len; i++) {
                    JSValue v = arrp[i];
                    acc += (JS_VALUE_GET_TAG(v) == JS_TAG_INT)
                             ? (double)JS_VALUE_GET_INT(v) : JS_VALUE_GET_FLOAT64(v);
                }
                if (magic == 1) acc = len ? acc / (double)len : 0;
                ret = JS_NewFloat64(ctx, acc);
                goto done;
            }
        }
    }
    for (i = 0; i < len; i++) {
        JSValue v;
        double d;
        int r;
        if (js_array_ext_getel(ctx, obj, i, &v))
            goto done;
        r = JS_ToFloat64(ctx, &d, v);
        JS_FreeValue(ctx, v);
        if (r)
            goto done;
        acc += d;
    }
    if (magic == 1)
        acc = len ? acc / (double)len : 0;
    ret = JS_NewFloat64(ctx, acc);
 done:
    JS_FreeValue(ctx, obj);
    return ret;
}

/* _compact() -> a new array with null and undefined removed. */
static JSValue js_array_ext_compact(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    JSValue obj, a, ret = JS_EXCEPTION;
    int64_t len, i, j = 0;
    (void)argc; (void)argv;
    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj))
        goto done;
    a = JS_NewArray(ctx);
    if (JS_IsException(a))
        goto done;
    for (i = 0; i < len; i++) {
        JSValue v;
        if (js_array_ext_getel(ctx, obj, i, &v)) {
            JS_FreeValue(ctx, a);
            goto done;
        }
        if (JS_IsNull(v) || JS_IsUndefined(v)) {
            JS_FreeValue(ctx, v);
            continue;
        }
        if (JS_DefinePropertyValueInt64(ctx, a, j++, v, JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, a);
            goto done;
        }
    }
    ret = a;
 done:
    JS_FreeValue(ctx, obj);
    return ret;
}

/* Apply a "mapper" to an element: undefined -> the element itself;
 * a function -> fn(element); anything else -> element[key] (a property name).
 * Returns an owned JSValue or JS_EXCEPTION. `el` is borrowed. */
static JSValue js_array_ext_mapval(JSContext *ctx, JSValueConst map,
                                   JSValueConst el)
{
    if (JS_IsUndefined(map))
        return JS_DupValue(ctx, el);
    if (JS_IsFunction(ctx, map))
        return JS_Call(ctx, map, JS_UNDEFINED, 1, &el);
    {
        JSAtom a = JS_ValueToAtom(ctx, map);
        JSValue v;
        if (a == JS_ATOM_NULL)
            return JS_EXCEPTION;
        v = JS_GetProperty(ctx, el, a);
        JS_FreeAtom(ctx, a);
        return v;
    }
}

/* Prepared "matcher" — the overloaded dispatch shared by every matcher method
 * (_count/_none/_any/_all/_partition/_reject/_takeWhile/_dropWhile/...). The
 * matcher KIND is resolved ONCE (js_ext_matcher_begin) so the per-element test
 * has no re-resolution cost; in particular a RegExp's `.test` method is looked
 * up a single time, not once per element. Kinds: a function -> ToBool(fn(el));
 * a RegExp -> regex.test(String(el)), detected by class id so a duck-typed
 * {test(){}} is NOT a regex; otherwise SameValueZero(matcher,el). */
typedef struct JSExtMatcher {
    JSValueConst matcher;   /* borrowed */
    JSValue regex_test;     /* owned; JS_UNDEFINED unless kind == 2 */
    int kind;               /* 0 = value, 1 = function, 2 = RegExp */
} JSExtMatcher;

static int js_ext_matcher_begin(JSContext *ctx, JSExtMatcher *pm,
                                JSValueConst matcher)
{
    pm->matcher = matcher;
    pm->regex_test = JS_UNDEFINED;
    if (JS_IsFunction(ctx, matcher)) {
        pm->kind = 1;
    } else if (JS_VALUE_GET_TAG(matcher) == JS_TAG_OBJECT &&
               JS_VALUE_GET_OBJ(matcher)->class_id == JS_CLASS_REGEXP) {
        pm->kind = 2;
        pm->regex_test = JS_GetPropertyStr(ctx, matcher, "test");
        if (JS_IsException(pm->regex_test)) { pm->regex_test = JS_UNDEFINED; return -1; }
    } else {
        pm->kind = 0;
    }
    return 0;
}

/* Returns 1 (match), 0 (no match), or -1 (exception). */
static int js_ext_matcher_test(JSContext *ctx, JSExtMatcher *pm, JSValueConst el)
{
    switch (pm->kind) {
    case 1: {
        JSValue r = JS_Call(ctx, pm->matcher, JS_UNDEFINED, 1, &el);
        int b;
        if (JS_IsException(r))
            return -1;
        b = JS_ToBool(ctx, r);
        JS_FreeValue(ctx, r);
        return b;
    }
    case 2: {
        JSValue str = JS_ToString(ctx, el), r;
        int b;
        if (JS_IsException(str))
            return -1;
        r = JS_Call(ctx, pm->regex_test, pm->matcher, 1, (JSValueConst *)&str);
        JS_FreeValue(ctx, str);
        if (JS_IsException(r))
            return -1;
        b = JS_ToBool(ctx, r);
        JS_FreeValue(ctx, r);
        return b;
    }
    default:
        return JS_SameValueZero(ctx, pm->matcher, el) ? 1 : 0;
    }
}

static void js_ext_matcher_end(JSContext *ctx, JSExtMatcher *pm)
{
    JS_FreeValue(ctx, pm->regex_test);
    pm->regex_test = JS_UNDEFINED;
}

/* _count(match?) -> length with no argument; else the number of elements the
 * matcher accepts (a value by SameValueZero, or a predicate function). */
static JSValue js_array_ext_count(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    JSValue obj, ret = JS_EXCEPTION;
    JSExtMatcher pm = { JS_UNDEFINED, JS_UNDEFINED, 0 };
    int64_t len, i, c = 0;
    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj))
        goto done;
    if (argc == 0 || JS_IsUndefined(argv[0])) {
        ret = JS_NewInt64(ctx, len);
        goto done;
    }
    if (js_ext_matcher_begin(ctx, &pm, argv[0]))
        goto done;
    for (i = 0; i < len; i++) {
        JSValue el;
        int m;
        if (js_array_ext_getel(ctx, obj, i, &el))
            goto done;
        m = js_ext_matcher_test(ctx, &pm, el);
        JS_FreeValue(ctx, el);
        if (m < 0)
            goto done;
        c += m;
    }
    ret = JS_NewInt64(ctx, c);
 done:
    js_ext_matcher_end(ctx, &pm);
    JS_FreeValue(ctx, obj);
    return ret;
}

/* _none / _any / _all (magic 0/1/2) against a value or predicate matcher. */
static JSValue js_array_ext_quantify(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv, int magic)
{
    JSValue obj, ret = JS_EXCEPTION;
    JSExtMatcher pm = { JS_UNDEFINED, JS_UNDEFINED, 0 };
    int64_t len, i;
    JSValueConst match = argc > 0 ? argv[0] : JS_UNDEFINED;
    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj))
        goto done;
    if (js_ext_matcher_begin(ctx, &pm, match))
        goto done;
    for (i = 0; i < len; i++) {
        JSValue el;
        int m;
        if (js_array_ext_getel(ctx, obj, i, &el))
            goto done;
        m = js_ext_matcher_test(ctx, &pm, el);
        JS_FreeValue(ctx, el);
        if (m < 0)
            goto done;
        if (magic == 1 && m) { ret = JS_TRUE;  goto done; } /* any: found  */
        if (magic == 0 && m) { ret = JS_FALSE; goto done; } /* none: found */
        if (magic == 2 && !m){ ret = JS_FALSE; goto done; } /* all: missed */
    }
    ret = (magic == 1) ? JS_FALSE : JS_TRUE; /* any->false, none/all->true */
 done:
    js_ext_matcher_end(ctx, &pm);
    JS_FreeValue(ctx, obj);
    return ret;
}

/* _min / _max (magic 0/1): return the ELEMENT whose mapped value (undefined =
 * identity, a function, or a property name) is numerically smallest/largest
 * (first on a tie). Empty -> undefined. */
static JSValue js_array_ext_minmax(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv, int magic)
{
    JSValue obj, best = JS_UNDEFINED, ret = JS_EXCEPTION;
    JSValueConst map = argc > 0 ? argv[0] : JS_UNDEFINED;
    int64_t len, i;
    double best_key = 0;
    int have = 0;
    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj))
        goto fail;
    if (JS_IsUndefined(map)) {
        /* fast path: no mapper + a contiguous all-numeric fast array. No user JS
         * runs, so holding arrp is safe; returns the winning element directly. */
        JSValue *arrp;
        uint32_t count;
        if (js_get_fast_array(ctx, obj, &arrp, &count) && (int64_t)count == len && len > 0) {
            int homogeneous = 1;
            for (i = 0; i < len; i++) {
                int t = JS_VALUE_GET_TAG(arrp[i]);
                if (t != JS_TAG_INT && !JS_TAG_IS_FLOAT64(t)) { homogeneous = 0; break; }
            }
            if (homogeneous) {
                int64_t bi = 0;
                double bk = (JS_VALUE_GET_TAG(arrp[0]) == JS_TAG_INT)
                              ? (double)JS_VALUE_GET_INT(arrp[0]) : JS_VALUE_GET_FLOAT64(arrp[0]);
                for (i = 1; i < len; i++) {
                    JSValue v = arrp[i];
                    double d = (JS_VALUE_GET_TAG(v) == JS_TAG_INT)
                                 ? (double)JS_VALUE_GET_INT(v) : JS_VALUE_GET_FLOAT64(v);
                    if (magic == 0 ? d < bk : d > bk) { bk = d; bi = i; }
                }
                ret = JS_DupValue(ctx, arrp[bi]);
                JS_FreeValue(ctx, obj);
                return ret;
            }
        }
    }
    for (i = 0; i < len; i++) {
        JSValue el, key;
        double d;
        int r;
        if (js_array_ext_getel(ctx, obj, i, &el))
            goto fail;
        key = js_array_ext_mapval(ctx, map, el);
        if (JS_IsException(key)) {
            JS_FreeValue(ctx, el);
            goto fail;
        }
        r = JS_ToFloat64(ctx, &d, key);
        JS_FreeValue(ctx, key);
        if (r) {
            JS_FreeValue(ctx, el);
            goto fail;
        }
        if (!have || (magic == 0 ? d < best_key : d > best_key)) {
            JS_FreeValue(ctx, best);
            best = el;
            best_key = d;
            have = 1;
        } else {
            JS_FreeValue(ctx, el);
        }
    }
    ret = best;          /* transfer ownership (JS_UNDEFINED if empty) */
    best = JS_UNDEFINED;
 fail:
    JS_FreeValue(ctx, best);
    JS_FreeValue(ctx, obj);
    return ret;
}

/* _take / _drop / _takeLast / _dropLast (magic 0/1/2/3) -> a new array. n is
 * clamped to [0,len]; a negative or missing n is treated as 0. */
static JSValue js_array_ext_take(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv, int magic)
{
    JSValue obj, ret = JS_EXCEPTION;
    int64_t len, n;
    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj))
        goto done;
    if (JS_ToInt64Sat(ctx, &n, argc > 0 ? argv[0] : JS_UNDEFINED))
        goto done;
    if (n < 0)
        n = 0;
    if (n > len)
        n = len;
    switch (magic) {
    case 0: ret = js_array_ext_build_range(ctx, obj, 0, n); break;       /* take */
    case 1: ret = js_array_ext_build_range(ctx, obj, n, len); break;     /* drop */
    case 2: ret = js_array_ext_build_range(ctx, obj, len - n, len); break;/* takeLast */
    default:ret = js_array_ext_build_range(ctx, obj, 0, len - n); break; /* dropLast */
    }
 done:
    JS_FreeValue(ctx, obj);
    return ret;
}

/* ---- _sortBy: decorate / stable-merge-sort / undecorate ---- */
typedef struct {
    JSValue val;      /* owned element */
    double dkey;      /* numeric sort key (is_num) */
    char *skey;       /* string sort key, owned (else) */
    uint32_t idx;     /* original index (unused: merge sort is already stable) */
    int is_num;
} DynSortItem;

/* A TOTAL order over the decorated keys. NaN must be handled explicitly: every
 * comparison involving it is false, so the naive three-way test reports "equal"
 * against every other key, which is not an ordering at all -- a single NaN in the
 * input then leaves the merge sort's output unsorted rather than merely
 * misplacing the NaN. NaN therefore sorts after all real numbers (and, like any
 * numeric key, before the string keys). */
static int dyn_sortby_cmp(const DynSortItem *a, const DynSortItem *b)
{
    if (a->is_num && b->is_num) {
        int a_nan = (a->dkey != a->dkey);   /* IEEE: only NaN is != itself */
        int b_nan = (b->dkey != b->dkey);
        if (a_nan || b_nan)
            return a_nan - b_nan;           /* NaN last; two NaNs are equal */
        return a->dkey < b->dkey ? -1 : a->dkey > b->dkey ? 1 : 0;
    }
    if (!a->is_num && !b->is_num)
        return strcmp(a->skey, b->skey);
    return a->is_num ? -1 : 1; /* numbers sort before strings */
}

/* stable bottom-up merge sort; `desc` reverses the order (ties keep original
 * order in both directions). Returns 0, or -1 on OOM. */
static int dyn_sortby_sort(JSContext *ctx, DynSortItem *items, int64_t n, int desc)
{
    DynSortItem *tmp, *src, *dst, *swap;
    int64_t width;
    if (n < 2)
        return 0;
    tmp = js_malloc(ctx, (size_t)n * sizeof(*tmp));
    if (!tmp)
        return -1;
    src = items;
    dst = tmp;
    for (width = 1; width < n; width *= 2) {
        int64_t i;
        for (i = 0; i < n; i += 2 * width) {
            int64_t mid = i + width < n ? i + width : n;
            int64_t hi = i + 2 * width < n ? i + 2 * width : n;
            int64_t l = i, r = mid, k = i;
            while (l < mid && r < hi) {
                int c = dyn_sortby_cmp(&src[l], &src[r]);
                if (desc)
                    c = -c;
                dst[k++] = (c <= 0) ? src[l++] : src[r++]; /* stable: left on tie */
            }
            while (l < mid) dst[k++] = src[l++];
            while (r < hi)  dst[k++] = src[r++];
        }
        swap = src; src = dst; dst = swap;
    }
    /* Every pass writes all of [0,n), so dst is never stale -- but an odd pass
       count leaves the result in tmp, and the caller reads `items`. */
    if (src != items)
        memcpy(items, src, (size_t)n * sizeof(*items));
    js_free(ctx, tmp);
    return 0;
}

static void dyn_sortby_free(JSContext *ctx, DynSortItem *items, int64_t n)
{
    int64_t i;
    for (i = 0; i < n; i++) {
        JS_FreeValue(ctx, items[i].val);
        js_free(ctx, items[i].skey);
    }
    js_free(ctx, items);
}

/* _sortBy(map?, desc?) -> a new array sorted by the mapped value (identity /
 * function / property-name). Numeric keys compare numerically, others by their
 * string form (byte order); stable; desc reverses. */
static JSValue js_array_ext_sortby(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    JSValue obj, arr, *pval, ret = JS_EXCEPTION;
    JSValueConst map = argc > 0 ? argv[0] : JS_UNDEFINED;
    DynSortItem *items = NULL;
    JSObject *p;
    int64_t len, i;
    int desc = argc > 1 ? JS_ToBool(ctx, argv[1]) : 0;

    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj))
        goto done;
    if (len == 0) { ret = JS_NewArray(ctx); goto done; }

    items = js_mallocz(ctx, (size_t)len * sizeof(*items));
    if (!items) { JS_ThrowOutOfMemory(ctx); goto done; }
    for (i = 0; i < len; i++) {
        JSValue el, key;
        if (js_array_ext_getel(ctx, obj, i, &el)) goto fail;
        items[i].val = el;               /* owned; cleanup frees it */
        items[i].idx = (uint32_t)i;
        key = js_array_ext_mapval(ctx, map, el);
        if (JS_IsException(key)) goto fail;
        if (JS_IsNumber(key)) {
            items[i].is_num = 1;
            JS_ToFloat64(ctx, &items[i].dkey, key);
            JS_FreeValue(ctx, key);
        } else {
            const char *s = JS_ToCString(ctx, key);
            JS_FreeValue(ctx, key);
            if (!s) goto fail;
            items[i].skey = js_strdup(ctx, s);
            JS_FreeCString(ctx, s);
            if (!items[i].skey) { JS_ThrowOutOfMemory(ctx); goto fail; }
        }
    }
    if (dyn_sortby_sort(ctx, items, len, desc)) { JS_ThrowOutOfMemory(ctx); goto fail; }

    arr = js_allocate_fast_array(ctx, len);
    if (JS_IsException(arr)) goto fail;
    p = JS_VALUE_GET_OBJ(arr);
    pval = p->u.array.u.values;
    for (i = 0; i < len; i++) {
        pval[i] = items[i].val;          /* transfer ownership */
        js_free(ctx, items[i].skey);
    }
    js_free(ctx, items);
    items = NULL;
    ret = arr;
    goto done;
 fail:
    dyn_sortby_free(ctx, items, len);
    items = NULL;
 done:
    JS_FreeValue(ctx, obj);
    return ret;
}

/* ---- _sortedIndexOf: binary search over an already-sorted array ----
 *
 * O(log n) instead of indexOf's O(n), which is the entire point -- but it is only
 * correct if the array really is sorted by the ordering being searched with, and
 * that cannot be checked in less time than the search itself. So the contract is
 * the caller's: this is the counterpart to _sortBy (no comparator: the same
 * numbers-before-strings total order, NaN last) and to toSorted/sortWith (pass
 * the same comparator).
 *
 * Returns the index of SOME matching element, or -1. With duplicates it does not
 * promise the first or the last -- promising either would cost a linear walk in
 * the worst case (an array of equal elements) and defeat the reason to use it. */

/* Decorate one value the way _sortBy would, so both use one ordering. */
static int dyn_sorted_key(JSContext *ctx, JSValueConst v, double *dkey,
                          char **skey, int *is_num)
{
    *skey = NULL;
    if (JS_IsNumber(v)) {
        *is_num = 1;
        return JS_ToFloat64(ctx, dkey, v);
    }
    {
        const char *s = JS_ToCString(ctx, v);
        if (!s)
            return -1;
        *is_num = 0;
        *skey = js_strdup(ctx, s);
        JS_FreeCString(ctx, s);
        if (!*skey) {
            JS_ThrowOutOfMemory(ctx);
            return -1;
        }
    }
    return 0;
}

/* Compare a probe element against the decorated target. Returns <0, 0, >0 with
 * the same meaning as dyn_sortby_cmp, or throws (sets *err). */
static int dyn_sorted_cmp_default(JSContext *ctx, JSValueConst el,
                                  double tkey, const char *tskey, int t_is_num,
                                  int *err)
{
    DynSortItem a, b;
    int r;

    a.skey = NULL;
    if (dyn_sorted_key(ctx, el, &a.dkey, &a.skey, &a.is_num)) {
        *err = 1;
        return 0;
    }
    b.dkey = tkey;
    b.skey = (char *)tskey;
    b.is_num = t_is_num;
    r = dyn_sortby_cmp(&a, &b);
    js_free(ctx, a.skey);
    return r;
}

/* _sortedIndexOf(value, comparator?) -> index of a match, or -1. */
static JSValue js_array_ext_sorted_index_of(JSContext *ctx,
                                            JSValueConst this_val,
                                            int argc, JSValueConst *argv)
{
    JSValueConst target = argc > 0 ? argv[0] : JS_UNDEFINED;
    JSValueConst cmp = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSValue obj, ret = JS_EXCEPTION;
    int64_t len, lo, hi;
    double tkey = 0;
    char *tskey = NULL;
    int t_is_num = 0, use_cmp = JS_IsFunction(ctx, cmp);

    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj))
        goto done;
    if (len == 0) {
        ret = JS_NewInt32(ctx, -1);
        goto done;
    }
    if (!use_cmp && dyn_sorted_key(ctx, target, &tkey, &tskey, &t_is_num))
        goto done;

    lo = 0;
    hi = len - 1;
    while (lo <= hi) {
        int64_t mid = lo + (hi - lo) / 2;   /* no overflow, unlike (lo+hi)/2 */
        JSValue el;
        int c;

        if (js_array_ext_getel(ctx, obj, mid, &el))
            goto done;
        if (use_cmp) {
            JSValueConst ab[2];
            JSValue r;
            double d;
            int e;
            ab[0] = el;
            ab[1] = target;
            r = JS_Call(ctx, cmp, JS_UNDEFINED, 2, ab);
            JS_FreeValue(ctx, el);
            if (JS_IsException(r))
                goto done;
            e = JS_ToFloat64(ctx, &d, r);
            JS_FreeValue(ctx, r);
            if (e)
                goto done;
            /* a NaN comparator result is treated as "equal", matching what
             * Array.prototype.sort does with one */
            c = (d < 0) ? -1 : (d > 0) ? 1 : 0;
        } else {
            int err = 0;
            c = dyn_sorted_cmp_default(ctx, el, tkey, tskey, t_is_num, &err);
            JS_FreeValue(ctx, el);
            if (err)
                goto done;
        }
        if (c == 0) {
            ret = JS_NewInt64(ctx, mid);
            goto done;
        }
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    ret = JS_NewInt32(ctx, -1);
 done:
    js_free(ctx, tskey);
    JS_FreeValue(ctx, obj);
    return ret;
}

/* _groupBy(map?) -> an object mapping each mapped key (identity / function /
 * property-name) to the array of elements that produced it. */
static JSValue js_array_ext_groupby(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    JSValue obj, result, ret = JS_EXCEPTION;
    JSValueConst map = argc > 0 ? argv[0] : JS_UNDEFINED;
    int64_t len, i;

    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj))
        goto done;
    result = JS_NewObject(ctx);
    if (JS_IsException(result))
        goto done;
    for (i = 0; i < len; i++) {
        JSValue el, key, bucket;
        JSAtom atom;
        int64_t blen;
        if (js_array_ext_getel(ctx, obj, i, &el)) goto fail;
        key = js_array_ext_mapval(ctx, map, el);
        if (JS_IsException(key)) { JS_FreeValue(ctx, el); goto fail; }
        atom = JS_ValueToAtom(ctx, key);
        JS_FreeValue(ctx, key);
        if (atom == JS_ATOM_NULL) { JS_FreeValue(ctx, el); goto fail; }
        /* DEFINE the bucket, never Set it: JS_SetProperty walks the prototype
           chain, so a group key of "__proto__" retargeted the result object's
           prototype instead of naming a bucket, and that group vanished from
           the result. Define is also faster -- no chain walk per key. */
        bucket = JS_GetProperty(ctx, result, atom);
        if (JS_IsException(bucket)) { JS_FreeAtom(ctx, atom); JS_FreeValue(ctx, el); goto fail; }
        if (!JS_IsArray(ctx, bucket)) {
            JS_FreeValue(ctx, bucket);
            bucket = JS_NewArray(ctx);
            if (JS_IsException(bucket) ||
                JS_DefinePropertyValue(ctx, result, atom, JS_DupValue(ctx, bucket),
                                       JS_PROP_C_W_E) < 0) {
                JS_FreeValue(ctx, bucket); JS_FreeAtom(ctx, atom); JS_FreeValue(ctx, el); goto fail;
            }
        }
        JS_FreeAtom(ctx, atom);
        if (js_get_length64(ctx, &blen, bucket)) {
            JS_FreeValue(ctx, el); JS_FreeValue(ctx, bucket); goto fail;
        }
        if (JS_SetPropertyInt64(ctx, bucket, blen, el) < 0) { /* el consumed */
            JS_FreeValue(ctx, bucket); goto fail;
        }
        JS_FreeValue(ctx, bucket);
    }
    ret = result;
    goto done;
 fail:
    JS_FreeValue(ctx, result);
 done:
    JS_FreeValue(ctx, obj);
    return ret;
}

/* _shuffle() -> a new array with the elements in uniformly-random order
 * (Fisher-Yates over a fast-array copy, using the engine's Math.random PRNG). */
static JSValue js_array_ext_shuffle(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    JSValue obj, arr;
    JSObject *p;
    JSValue *vals;
    int64_t len, i;
    (void)argc; (void)argv;
    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj)) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    arr = js_array_ext_build_range(ctx, obj, 0, len);
    JS_FreeValue(ctx, obj);
    if (JS_IsException(arr) || len < 2)
        return arr;
    p = JS_VALUE_GET_OBJ(arr);
    vals = p->u.array.u.values;
    for (i = len - 1; i > 0; i--) {
        int64_t j = (int64_t)(xorshift64star(&ctx->random_state) % (uint64_t)(i + 1));
        JSValue t = vals[i]; vals[i] = vals[j]; vals[j] = t;
    }
    return arr;
}

/* _sample(n?) -> a uniformly-random element (undefined if empty), or a new array
 * of n distinct random elements (n>len -> all, shuffled). */
static JSValue js_array_ext_sample(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    JSValue obj, full, ret = JS_EXCEPTION;
    JSObject *p;
    JSValue *vals;
    int64_t len, i, n;

    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj))
        goto done;
    if (argc == 0 || JS_IsUndefined(argv[0])) {          /* single element */
        if (len == 0) ret = JS_UNDEFINED;
        else {
            int64_t j = (int64_t)(xorshift64star(&ctx->random_state) % (uint64_t)len);
            if (js_array_ext_getel(ctx, obj, j, &ret)) ret = JS_EXCEPTION;
        }
        goto done;
    }
    if (JS_ToInt64Sat(ctx, &n, argv[0])) goto done;
    if (n < 0) n = 0;
    if (n > len) n = len;
    full = js_array_ext_build_range(ctx, obj, 0, len);   /* fast-array copy */
    if (JS_IsException(full)) goto done;
    p = JS_VALUE_GET_OBJ(full);
    vals = p->u.array.u.values;
    for (i = len - 1; i > 0; i--) {                      /* full Fisher-Yates */
        int64_t j = (int64_t)(xorshift64star(&ctx->random_state) % (uint64_t)(i + 1));
        JSValue t = vals[i]; vals[i] = vals[j]; vals[j] = t;
    }
    ret = js_array_ext_build_range(ctx, full, 0, n);     /* first n */
    JS_FreeValue(ctx, full);
 done:
    JS_FreeValue(ctx, obj);
    return ret;
}

/* ============================================================================
 * DynValSet: a small open-addressing hash set of JSValues with SameValueZero
 * membership, reusing the engine's Map value-hasher. Owns its keys (dup on add,
 * free on destroy). Sized up front from a count hint (load factor <= 0.5, so no
 * resize and probing always terminates). Empty slots use JS_UNINITIALIZED, which
 * no array element or JS return value can ever be. The shared primitive behind
 * _unique/_uniqBy and the set operations.
 * ========================================================================== */
typedef struct {
    JSValue *keys;     /* owned; JS_UNINITIALIZED = empty slot */
    uint32_t mask;     /* slots - 1 (slots is a power of two) */
    uint32_t count;
    int hash_bits;
} DynValSet;

static int dyn_valset_init(JSContext *ctx, DynValSet *s, int64_t hint)
{
    int bits = 3;
    uint32_t slots, i;
    /* size from the hint but CAP the initial table (a high-duplicate source has
     * far fewer distinct keys than elements — oversizing to 2*len wastes an
     * init+free pass over millions of empty slots); dyn_valset_resize grows it
     * on demand for genuinely large distinct sets. */
    while (((int64_t)1 << bits) < hint * 2 && bits < 16)
        bits++;
    slots = (uint32_t)1 << bits;
    s->keys = js_malloc(ctx, (size_t)slots * sizeof(JSValue));
    if (!s->keys)
        return -1;
    for (i = 0; i < slots; i++)
        s->keys[i] = JS_UNINITIALIZED;
    s->mask = slots - 1;
    s->hash_bits = bits;
    s->count = 0;
    return 0;
}

static void dyn_valset_free(JSContext *ctx, DynValSet *s)
{
    uint32_t i;
    if (!s->keys)
        return;
    for (i = 0; i <= s->mask; i++)
        JS_FreeValue(ctx, s->keys[i]); /* no-op for the UNINITIALIZED slots */
    js_free(ctx, s->keys);
    s->keys = NULL;
}

/* double the table and rehash (moves the owned keys, no dup/free). 0 or -1. */
static int dyn_valset_resize(JSContext *ctx, DynValSet *s)
{
    int new_bits = s->hash_bits + 1;
    uint32_t new_slots, new_mask, i;
    JSValue *nk;
    if (new_bits > 30)
        return 0; /* absurdly large: stay put (load still well below 1) */
    new_slots = (uint32_t)1 << new_bits;
    new_mask = new_slots - 1;
    nk = js_malloc(ctx, (size_t)new_slots * sizeof(JSValue));
    if (!nk)
        return -1;
    for (i = 0; i < new_slots; i++)
        nk[i] = JS_UNINITIALIZED;
    for (i = 0; i <= s->mask; i++) {
        JSValue k = s->keys[i];
        uint32_t h;
        if (JS_VALUE_GET_TAG(k) == JS_TAG_UNINITIALIZED)
            continue;
        h = map_hash_key(k, new_bits) & new_mask;
        while (JS_VALUE_GET_TAG(nk[h]) != JS_TAG_UNINITIALIZED)
            h = (h + 1) & new_mask;
        nk[h] = k; /* ownership moves with the value */
    }
    js_free(ctx, s->keys);
    s->keys = nk;
    s->mask = new_mask;
    s->hash_bits = new_bits;
    return 0;
}

/* add `key` (borrowed) -> 1 if newly inserted, 0 if already present, -1 on OOM.
 * Grows the table only on an actual insert at load >= 0.5 (a duplicate returns
 * early and never resizes). No user JS runs (hashing + SameValueZero only), so
 * the caller's source stays valid across the call. */
static int dyn_valset_add(JSContext *ctx, DynValSet *s, JSValueConst key)
{
    JSValueConst nk = map_normalize_key_const(ctx, key);
    uint32_t h = map_hash_key(nk, s->hash_bits) & s->mask;
    for (;;) {
        JSValue slot = s->keys[h];
        if (JS_VALUE_GET_TAG(slot) == JS_TAG_UNINITIALIZED)
            break; /* not present: this is the insertion point */
        if (JS_SameValueZero(ctx, slot, nk))
            return 0; /* already present */
        h = (h + 1) & s->mask;
    }
    if (s->count >= ((s->mask + 1) >> 1)) { /* load would reach 0.5 -> grow first */
        if (dyn_valset_resize(ctx, s))
            return -1;
        h = map_hash_key(nk, s->hash_bits) & s->mask;
        while (JS_VALUE_GET_TAG(s->keys[h]) != JS_TAG_UNINITIALIZED)
            h = (h + 1) & s->mask;
    }
    s->keys[h] = JS_DupValue(ctx, nk);
    s->count++;
    return 1;
}

static int dyn_valset_has(JSContext *ctx, DynValSet *s, JSValueConst key)
{
    JSValueConst nk = map_normalize_key_const(ctx, key);
    uint32_t h = map_hash_key(nk, s->hash_bits) & s->mask;
    for (;;) {
        JSValue slot = s->keys[h];
        if (JS_VALUE_GET_TAG(slot) == JS_TAG_UNINITIALIZED)
            return 0;
        if (JS_SameValueZero(ctx, slot, nk))
            return 1;
        h = (h + 1) & s->mask;
    }
}

/* _unique(map?) / _uniq / _uniqBy(fn) -> a new array with duplicates removed
 * (SameValueZero on the mapped value; identity / function / property-name),
 * keeping the first occurrence's element in original order. */
static JSValue js_array_ext_unique(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    JSValue obj, result, ret = JS_EXCEPTION;
    JSValueConst map = argc > 0 ? argv[0] : JS_UNDEFINED;
    DynValSet seen;
    int64_t len, i, j = 0;

    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj))
        goto done0;
    result = JS_NewArray(ctx);
    if (JS_IsException(result))
        goto done0;
    if (dyn_valset_init(ctx, &seen, len)) { JS_ThrowOutOfMemory(ctx); JS_FreeValue(ctx, result); goto done0; }
    for (i = 0; i < len; i++) {
        JSValue el, key;
        int added;
        if (js_array_ext_getel(ctx, obj, i, &el)) goto fail;
        key = js_array_ext_mapval(ctx, map, el);
        if (JS_IsException(key)) { JS_FreeValue(ctx, el); goto fail; }
        added = dyn_valset_add(ctx, &seen, key);
        JS_FreeValue(ctx, key);
        if (added < 0) { JS_FreeValue(ctx, el); goto fail; }
        if (added) {
            if (JS_DefinePropertyValueInt64(ctx, result, j++, el, JS_PROP_C_W_E) < 0) goto fail;
        } else {
            JS_FreeValue(ctx, el);
        }
    }
    ret = result;
    result = JS_UNDEFINED;
 fail:
    dyn_valset_free(ctx, &seen);
    JS_FreeValue(ctx, result);
 done0:
    JS_FreeValue(ctx, obj);
    return ret;
}

/* _intersect(other)/_intersection, _difference(other), _without(other)
 * (magic 0/1/2). Builds a SameValueZero set from `other`, then filters `this`.
 * intersect/difference dedup the result; without keeps this's duplicates. */
static JSValue js_array_ext_setop(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv, int magic)
{
    JSValue obj, other, result, ret = JS_EXCEPTION;
    DynValSet set, seen;
    int have_seen = 0;
    int64_t len, olen, i, j = 0;

    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj)) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    other = JS_ToObject(ctx, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (JS_IsException(other)) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    if (js_get_length64(ctx, &olen, other)) goto done;
    if (dyn_valset_init(ctx, &set, olen)) { JS_ThrowOutOfMemory(ctx); goto done; }
    for (i = 0; i < olen; i++) {
        JSValue oe;
        int r;
        if (js_array_ext_getel(ctx, other, i, &oe)) goto fail_set;
        r = dyn_valset_add(ctx, &set, oe);
        JS_FreeValue(ctx, oe);
        if (r < 0) goto fail_set;
    }
    result = JS_NewArray(ctx);
    if (JS_IsException(result)) goto fail_set;
    if (magic != 2) { /* intersect/difference dedup the result */
        if (dyn_valset_init(ctx, &seen, len)) { JS_ThrowOutOfMemory(ctx); JS_FreeValue(ctx, result); goto fail_set; }
        have_seen = 1;
    }
    for (i = 0; i < len; i++) {
        JSValue el;
        int in_other, keep;
        if (js_array_ext_getel(ctx, obj, i, &el)) goto fail_result;
        in_other = dyn_valset_has(ctx, &set, el);
        keep = (magic == 0) ? in_other : !in_other; /* intersect vs difference/without */
        if (!keep) { JS_FreeValue(ctx, el); continue; }
        if (have_seen) {
            int added = dyn_valset_add(ctx, &seen, el);
            if (added < 0) { JS_FreeValue(ctx, el); goto fail_result; } /* OOM */
            if (added == 0) { JS_FreeValue(ctx, el); continue; }        /* duplicate */
        }
        if (JS_DefinePropertyValueInt64(ctx, result, j++, el, JS_PROP_C_W_E) < 0) goto fail_result;
    }
    ret = result;
    result = JS_UNDEFINED;
 fail_result:
    if (have_seen) dyn_valset_free(ctx, &seen);
    JS_FreeValue(ctx, result);
 fail_set:
    dyn_valset_free(ctx, &set);
 done:
    JS_FreeValue(ctx, other);
    JS_FreeValue(ctx, obj);
    return ret;
}

/* _union(other) -> the elements of this then other, SameValueZero-deduped. */
static JSValue js_array_ext_union(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    JSValue obj, other, result, ret = JS_EXCEPTION;
    DynValSet seen;
    int64_t len, olen, i, j = 0, pass;

    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj)) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    other = JS_ToObject(ctx, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (JS_IsException(other)) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    if (js_get_length64(ctx, &olen, other)) goto done;
    result = JS_NewArray(ctx);
    if (JS_IsException(result)) goto done;
    if (dyn_valset_init(ctx, &seen, len + olen)) { JS_ThrowOutOfMemory(ctx); JS_FreeValue(ctx, result); goto done; }
    for (pass = 0; pass < 2; pass++) {
        JSValueConst src = pass == 0 ? obj : other;
        int64_t n = pass == 0 ? len : olen;
        for (i = 0; i < n; i++) {
            JSValue el;
            int added;
            if (js_array_ext_getel(ctx, src, i, &el)) goto fail;
            added = dyn_valset_add(ctx, &seen, el);
            if (added < 0) { JS_FreeValue(ctx, el); goto fail; }
            if (added) {
                if (JS_DefinePropertyValueInt64(ctx, result, j++, el, JS_PROP_C_W_E) < 0) goto fail;
            } else {
                JS_FreeValue(ctx, el);
            }
        }
    }
    ret = result;
    result = JS_UNDEFINED;
 fail:
    dyn_valset_free(ctx, &seen);
    JS_FreeValue(ctx, result);
 done:
    JS_FreeValue(ctx, other);
    JS_FreeValue(ctx, obj);
    return ret;
}

/* _zip(other) -> [[this[i], other[i]], ...] truncated to the shorter length. */
static JSValue js_array_ext_zip(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    JSValue obj, other, result, ret = JS_EXCEPTION;
    JSValue *ap, *bp;
    uint32_t acount, bcount;
    int64_t len, olen, n, i;
    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj)) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    other = JS_ToObject(ctx, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (JS_IsException(other)) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    if (js_get_length64(ctx, &olen, other)) goto done;
    n = len < olen ? len : olen;
    /* Fast arm: js_array_ext_build_range's predicate on BOTH operands -- dense
     * JS_CLASS_ARRAYs covering [0,n), so no getter, hole or proxy trap can run.
     * count>=n is what stops a.length=5 over 2 slots reading past the buffer. */
    if (js_get_fast_array(ctx, obj, &ap, &acount) && (int64_t)acount >= n &&
        js_get_fast_array(ctx, other, &bp, &bcount) && (int64_t)bcount >= n) {
        JSValue *dst;
        result = js_allocate_fast_array(ctx, n);
        if (JS_IsException(result)) goto done;
        dst = JS_VALUE_GET_OBJ(result)->u.array.u.values;
        /* ap/bp/dst survive the per-pair allocation: no JS runs, and the GC
         * neither moves a live object nor shrinks its values array. */
        for (i = 0; i < n; i++) {
            JSValue pair = js_allocate_fast_array(ctx, 2);
            JSValue *pv;
            if (JS_IsException(pair)) { JS_FreeValue(ctx, result); goto done; }
            pv = JS_VALUE_GET_OBJ(pair)->u.array.u.values;
            pv[0] = JS_DupValue(ctx, ap[i]);   /* slots were JS_UNDEFINED */
            pv[1] = JS_DupValue(ctx, bp[i]);
            dst[i] = pair;                     /* ownership transferred */
        }
        ret = result;
        goto done;
    }
    result = JS_NewArray(ctx);
    if (JS_IsException(result)) goto done;
    for (i = 0; i < n; i++) {
        JSValue a, b, pair;
        if (js_array_ext_getel(ctx, obj, i, &a)) { JS_FreeValue(ctx, result); goto done; }
        if (js_array_ext_getel(ctx, other, i, &b)) { JS_FreeValue(ctx, a); JS_FreeValue(ctx, result); goto done; }
        pair = JS_NewArray(ctx);
        if (JS_IsException(pair)) { JS_FreeValue(ctx, a); JS_FreeValue(ctx, b); JS_FreeValue(ctx, result); goto done; }
        JS_DefinePropertyValueInt64(ctx, pair, 0, a, JS_PROP_C_W_E);
        JS_DefinePropertyValueInt64(ctx, pair, 1, b, JS_PROP_C_W_E);
        if (JS_DefinePropertyValueInt64(ctx, result, i, pair, JS_PROP_C_W_E) < 0) { JS_FreeValue(ctx, result); goto done; }
    }
    ret = result;
 done:
    JS_FreeValue(ctx, other);
    JS_FreeValue(ctx, obj);
    return ret;
}

/* _zipWith(fn, other) -> [fn(this[i], other[i]), ...] truncated to shorter. */
static JSValue js_array_ext_zipwith(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    JSValue obj, other, result, ret = JS_EXCEPTION;
    JSValueConst fn = argc > 0 ? argv[0] : JS_UNDEFINED;
    int64_t len, olen, n, i;
    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj)) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    other = JS_ToObject(ctx, argc > 1 ? argv[1] : JS_UNDEFINED);
    if (JS_IsException(other)) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    if (js_get_length64(ctx, &olen, other)) goto done;
    n = len < olen ? len : olen;
    result = JS_NewArray(ctx);
    if (JS_IsException(result)) goto done;
    for (i = 0; i < n; i++) {
        JSValue a, b, r;
        JSValueConst args[2];
        if (js_array_ext_getel(ctx, obj, i, &a)) { JS_FreeValue(ctx, result); goto done; }
        if (js_array_ext_getel(ctx, other, i, &b)) { JS_FreeValue(ctx, a); JS_FreeValue(ctx, result); goto done; }
        args[0] = a; args[1] = b;
        r = JS_Call(ctx, fn, JS_UNDEFINED, 2, args);
        JS_FreeValue(ctx, a); JS_FreeValue(ctx, b);
        if (JS_IsException(r)) { JS_FreeValue(ctx, result); goto done; }
        if (JS_DefinePropertyValueInt64(ctx, result, i, r, JS_PROP_C_W_E) < 0) { JS_FreeValue(ctx, result); goto done; }
    }
    ret = result;
 done:
    JS_FreeValue(ctx, other);
    JS_FreeValue(ctx, obj);
    return ret;
}

/* _intersperse(sep) -> a new array with sep between each pair of elements. */
static JSValue js_array_ext_intersperse(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv)
{
    JSValue obj, result, ret = JS_EXCEPTION;
    JSValueConst sep = argc > 0 ? argv[0] : JS_UNDEFINED;
    int64_t len, i, j = 0;
    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj)) goto done;
    result = JS_NewArray(ctx);
    if (JS_IsException(result)) goto done;
    for (i = 0; i < len; i++) {
        JSValue el;
        if (i > 0 && JS_DefinePropertyValueInt64(ctx, result, j++, JS_DupValue(ctx, sep), JS_PROP_C_W_E) < 0) { JS_FreeValue(ctx, result); goto done; }
        if (js_array_ext_getel(ctx, obj, i, &el)) { JS_FreeValue(ctx, result); goto done; }
        if (JS_DefinePropertyValueInt64(ctx, result, j++, el, JS_PROP_C_W_E) < 0) { JS_FreeValue(ctx, result); goto done; }
    }
    ret = result;
 done:
    JS_FreeValue(ctx, obj);
    return ret;
}

/* recursive flatten; c_depth guards the C stack against pathological nesting
 * (past FLATTEN_MAX_DEPTH a nested array is emitted as-is). */
#define FLATTEN_MAX_DEPTH 512
static int js_array_ext_flatten_into(JSContext *ctx, JSValueConst result,
                                     JSValueConst arr, int64_t remaining,
                                     int64_t *j, int c_depth)
{
    int64_t len, i;
    if (js_get_length64(ctx, &len, arr)) return -1;
    for (i = 0; i < len; i++) {
        JSValue el;
        if (js_array_ext_getel(ctx, arr, i, &el)) return -1;
        if (remaining > 0 && c_depth < FLATTEN_MAX_DEPTH && JS_IsArray(ctx, el)) {
            int r = js_array_ext_flatten_into(ctx, result, el, remaining - 1, j, c_depth + 1);
            JS_FreeValue(ctx, el);
            if (r) return -1;
        } else if (JS_DefinePropertyValueInt64(ctx, result, (*j)++, el, JS_PROP_C_W_E) < 0) {
            return -1;
        }
    }
    return 0;
}

/* _flatten(depth?) -> a new array flattened to `depth` (default: fully). */
static JSValue js_array_ext_flatten(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    JSValue obj, result, ret = JS_EXCEPTION;
    int64_t depth = INT64_MAX, j = 0;
    obj = JS_ToObject(ctx, this_val);
    if (argc > 0 && !JS_IsUndefined(argv[0])) {
        if (JS_ToInt64Sat(ctx, &depth, argv[0])) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
        if (depth < 0) depth = 0;
    }
    result = JS_NewArray(ctx);
    if (JS_IsException(result)) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    if (js_array_ext_flatten_into(ctx, result, obj, depth, &j, 0)) { JS_FreeValue(ctx, result); goto done; }
    ret = result;
 done:
    JS_FreeValue(ctx, obj);
    return ret;
}

/* _transpose() -> transpose an array of arrays (ragged: skips missing cells). */
static JSValue js_array_ext_transpose(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    JSValue obj, result, ret = JS_EXCEPTION;
    int64_t nrows, maxcol = 0, r, c;
    (void)argc; (void)argv;
    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &nrows, obj)) goto done;
    for (r = 0; r < nrows; r++) {
        JSValue row;
        int64_t rl;
        if (js_array_ext_getel(ctx, obj, r, &row)) goto done;
        if (JS_IsArray(ctx, row)) {
            if (js_get_length64(ctx, &rl, row)) { JS_FreeValue(ctx, row); goto done; }
            if (rl > maxcol) maxcol = rl;
        }
        JS_FreeValue(ctx, row);
    }
    result = JS_NewArray(ctx);
    if (JS_IsException(result)) goto done;
    for (c = 0; c < maxcol; c++) {
        JSValue col = JS_NewArray(ctx);
        int64_t k = 0;
        if (JS_IsException(col)) { JS_FreeValue(ctx, result); goto done; }
        for (r = 0; r < nrows; r++) {
            JSValue row, cell;
            int64_t rl;
            if (js_array_ext_getel(ctx, obj, r, &row)) { JS_FreeValue(ctx, col); JS_FreeValue(ctx, result); goto done; }
            if (!JS_IsArray(ctx, row)) { JS_FreeValue(ctx, row); continue; }
            if (js_get_length64(ctx, &rl, row)) { JS_FreeValue(ctx, row); JS_FreeValue(ctx, col); JS_FreeValue(ctx, result); goto done; }
            if (c < rl) {
                if (js_array_ext_getel(ctx, row, c, &cell)) { JS_FreeValue(ctx, row); JS_FreeValue(ctx, col); JS_FreeValue(ctx, result); goto done; }
                if (JS_DefinePropertyValueInt64(ctx, col, k++, cell, JS_PROP_C_W_E) < 0) { JS_FreeValue(ctx, row); JS_FreeValue(ctx, col); JS_FreeValue(ctx, result); goto done; }
            }
            JS_FreeValue(ctx, row);
        }
        if (JS_DefinePropertyValueInt64(ctx, result, c, col, JS_PROP_C_W_E) < 0) { JS_FreeValue(ctx, result); goto done; }
    }
    ret = result;
 done:
    JS_FreeValue(ctx, obj);
    return ret;
}

/* _partition(matcher) -> [ [elements the matcher accepts], [the rest] ]
 * (matcher = value via SameValueZero, or a predicate function). */
static JSValue js_array_ext_partition(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    JSValue obj, yes = JS_UNDEFINED, no = JS_UNDEFINED, result, ret = JS_EXCEPTION;
    JSValueConst matcher = argc > 0 ? argv[0] : JS_UNDEFINED;
    JSExtMatcher pm = { JS_UNDEFINED, JS_UNDEFINED, 0 };
    int64_t len, i, jy = 0, jn = 0;

    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj))
        goto done;
    if (js_ext_matcher_begin(ctx, &pm, matcher))
        goto done;
    yes = JS_NewArray(ctx);
    no = JS_NewArray(ctx);
    if (JS_IsException(yes) || JS_IsException(no))
        goto done;
    for (i = 0; i < len; i++) {
        JSValue el;
        int m;
        if (js_array_ext_getel(ctx, obj, i, &el)) goto done;
        m = js_ext_matcher_test(ctx, &pm, el);
        if (m < 0) { JS_FreeValue(ctx, el); goto done; }
        if (JS_DefinePropertyValueInt64(ctx, m ? yes : no, m ? jy++ : jn++, el, JS_PROP_C_W_E) < 0)
            goto done;
    }
    result = JS_NewArray(ctx);
    if (JS_IsException(result)) goto done;
    JS_DefinePropertyValueInt64(ctx, result, 0, yes, JS_PROP_C_W_E); /* consumes yes */
    JS_DefinePropertyValueInt64(ctx, result, 1, no, JS_PROP_C_W_E);  /* consumes no */
    yes = no = JS_UNDEFINED;
    ret = result;
 done:
    js_ext_matcher_end(ctx, &pm);
    JS_FreeValue(ctx, yes);
    JS_FreeValue(ctx, no);
    JS_FreeValue(ctx, obj);
    return ret;
}

/* _pluck(key) -> a new array of element[key] for each element. */
static JSValue js_array_ext_pluck(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    JSValue obj, result, ret = JS_EXCEPTION;
    JSAtom key;
    int64_t len, i;

    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj)) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    key = JS_ValueToAtom(ctx, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (key == JS_ATOM_NULL) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    result = JS_NewArray(ctx);
    if (JS_IsException(result)) goto done;
    for (i = 0; i < len; i++) {
        JSValue el, v;
        if (js_array_ext_getel(ctx, obj, i, &el)) { JS_FreeValue(ctx, result); goto done; }
        v = JS_GetProperty(ctx, el, key);
        JS_FreeValue(ctx, el);
        if (JS_IsException(v)) { JS_FreeValue(ctx, result); goto done; }
        if (JS_DefinePropertyValueInt64(ctx, result, i, v, JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, result); goto done;
        }
    }
    ret = result;
 done:
    JS_FreeAtom(ctx, key);
    JS_FreeValue(ctx, obj);
    return ret;
}

/* _xprod(other) -> cross product [[a,b], ...] for each a in this, b in other. */
static JSValue js_array_ext_xprod(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    JSValue obj, other, result, ret = JS_EXCEPTION;
    int64_t len, olen, i, j, k = 0;
    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj)) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    other = JS_ToObject(ctx, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (JS_IsException(other)) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    if (js_get_length64(ctx, &olen, other)) goto done;
    result = JS_NewArray(ctx);
    if (JS_IsException(result)) goto done;
    for (i = 0; i < len; i++) {
        JSValue a;
        if (js_array_ext_getel(ctx, obj, i, &a)) { JS_FreeValue(ctx, result); goto done; }
        for (j = 0; j < olen; j++) {
            JSValue b, pair;
            if (js_array_ext_getel(ctx, other, j, &b)) { JS_FreeValue(ctx, a); JS_FreeValue(ctx, result); goto done; }
            pair = JS_NewArray(ctx);
            if (JS_IsException(pair)) { JS_FreeValue(ctx, a); JS_FreeValue(ctx, b); JS_FreeValue(ctx, result); goto done; }
            JS_DefinePropertyValueInt64(ctx, pair, 0, JS_DupValue(ctx, a), JS_PROP_C_W_E);
            JS_DefinePropertyValueInt64(ctx, pair, 1, b, JS_PROP_C_W_E);
            if (JS_DefinePropertyValueInt64(ctx, result, k++, pair, JS_PROP_C_W_E) < 0) { JS_FreeValue(ctx, a); JS_FreeValue(ctx, result); goto done; }
        }
        JS_FreeValue(ctx, a);
    }
    ret = result;
 done:
    JS_FreeValue(ctx, other);
    JS_FreeValue(ctx, obj);
    return ret;
}

/* _aperture(n) -> sliding windows of n consecutive elements: len-n+1 of them
 * n<=0 yields len-n+1 empty windows. */
static JSValue js_array_ext_aperture(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    JSValue obj, result, ret = JS_EXCEPTION;
    int64_t len, n, limit, i;
    obj = JS_ToObject(ctx, this_val);
    if (JS_ToInt64Sat(ctx, &n, argc > 0 ? argv[0] : JS_UNDEFINED)) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    /* limit is len-n+1, so a NEGATIVE n makes the window count unbounded by the
       receiver: aperture(-(2**53)) on a 3-element array asks for 9e15 empty
       windows and never returns. splitEvery below already refuses n<=0. */
    if (n < 0) { JS_FreeValue(ctx, obj); return JS_ThrowRangeError(ctx, "aperture: n must not be negative"); }
    if (js_get_length64(ctx, &len, obj)) goto done;
    limit = len - n + 1;
    if (limit < 0) limit = 0;
    result = JS_NewArray(ctx);
    if (JS_IsException(result)) goto done;
    for (i = 0; i < limit; i++) {
        int64_t start = i, end = i + n;
        JSValue win;
        if (end < start) end = start;   /* n<=0: empty window */
        win = js_array_ext_build_range(ctx, obj, start, end);
        if (JS_IsException(win)) { JS_FreeValue(ctx, result); goto done; }
        if (JS_DefinePropertyValueInt64(ctx, result, i, win, JS_PROP_C_W_E) < 0) { JS_FreeValue(ctx, result); goto done; }
    }
    ret = result;
 done:
    JS_FreeValue(ctx, obj);
    return ret;
}

/* _splitEvery(n) -> chunks of n consecutive elements (last may be short).
 * Throws RangeError for n<=0. */
static JSValue js_array_ext_splitevery(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    JSValue obj, result, ret = JS_EXCEPTION;
    int64_t len, n, i, k = 0;
    obj = JS_ToObject(ctx, this_val);
    if (JS_ToInt64Sat(ctx, &n, argc > 0 ? argv[0] : JS_UNDEFINED)) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    if (n <= 0) { JS_FreeValue(ctx, obj); return JS_ThrowRangeError(ctx, "splitEvery: n must be a positive integer"); }
    if (js_get_length64(ctx, &len, obj)) goto done;
    result = JS_NewArray(ctx);
    if (JS_IsException(result)) goto done;
    for (i = 0; i < len; i += n) {
        int64_t end = i + n;
        JSValue chunk;
        if (end > len) end = len;
        chunk = js_array_ext_build_range(ctx, obj, i, end);
        if (JS_IsException(chunk)) { JS_FreeValue(ctx, result); goto done; }
        if (JS_DefinePropertyValueInt64(ctx, result, k++, chunk, JS_PROP_C_W_E) < 0) { JS_FreeValue(ctx, result); goto done; }
    }
    ret = result;
 done:
    JS_FreeValue(ctx, obj);
    return ret;
}

/* _splitAt(index) -> [ take(index), drop(index) ]; negative index from the end. */
static JSValue js_array_ext_splitat(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    JSValue obj, left = JS_UNDEFINED, right = JS_UNDEFINED, result, ret = JS_EXCEPTION;
    int64_t len, idx;
    obj = JS_ToObject(ctx, this_val);
    if (JS_ToInt64Sat(ctx, &idx, argc > 0 ? argv[0] : JS_UNDEFINED)) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    if (js_get_length64(ctx, &len, obj)) goto done;
    if (idx < 0) idx = len + idx;
    if (idx < 0) idx = 0;
    if (idx > len) idx = len;
    left = js_array_ext_build_range(ctx, obj, 0, idx);
    if (JS_IsException(left)) goto done;
    right = js_array_ext_build_range(ctx, obj, idx, len);
    if (JS_IsException(right)) goto done;
    result = JS_NewArray(ctx);
    if (JS_IsException(result)) goto done;
    JS_DefinePropertyValueInt64(ctx, result, 0, left, JS_PROP_C_W_E);   /* consumes left */
    JS_DefinePropertyValueInt64(ctx, result, 1, right, JS_PROP_C_W_E);  /* consumes right */
    left = right = JS_UNDEFINED;
    ret = result;
 done:
    JS_FreeValue(ctx, left);
    JS_FreeValue(ctx, right);
    JS_FreeValue(ctx, obj);
    return ret;
}

/* _adjust(idx, fn) -> a copy with fn applied at idx (negative from the end);
 * an out-of-range idx yields an unchanged copy. */
static JSValue js_array_ext_adjust(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    JSValue obj, result = JS_UNDEFINED, ret = JS_EXCEPTION;
    JSValueConst fn = argc > 1 ? argv[1] : JS_UNDEFINED;
    int64_t len, idx;
    obj = JS_ToObject(ctx, this_val);
    if (JS_ToInt64Sat(ctx, &idx, argc > 0 ? argv[0] : JS_UNDEFINED)) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    if (js_get_length64(ctx, &len, obj)) goto done;
    result = js_array_ext_build_range(ctx, obj, 0, len);
    if (JS_IsException(result)) goto done;
    if (idx < 0) idx += len;
    if (idx >= 0 && idx < len) {
        JSValue old, nv;
        JSValueConst arg;
        if (js_array_ext_getel(ctx, result, idx, &old)) goto done;
        arg = old;
        nv = JS_Call(ctx, fn, JS_UNDEFINED, 1, &arg);
        JS_FreeValue(ctx, old);
        if (JS_IsException(nv)) goto done;
        if (JS_SetPropertyInt64(ctx, result, idx, nv) < 0) goto done;
    }
    ret = result; result = JS_UNDEFINED;
 done:
    JS_FreeValue(ctx, result);
    JS_FreeValue(ctx, obj);
    return ret;
}

/* _update(idx, val) -> a copy with val at idx (negative from the end);
 * an out-of-range idx yields an unchanged copy. */
static JSValue js_array_ext_update(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    JSValue obj, result = JS_UNDEFINED, ret = JS_EXCEPTION;
    JSValueConst val = argc > 1 ? argv[1] : JS_UNDEFINED;
    int64_t len, idx;
    obj = JS_ToObject(ctx, this_val);
    if (JS_ToInt64Sat(ctx, &idx, argc > 0 ? argv[0] : JS_UNDEFINED)) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    if (js_get_length64(ctx, &len, obj)) goto done;
    result = js_array_ext_build_range(ctx, obj, 0, len);
    if (JS_IsException(result)) goto done;
    if (idx < 0) idx += len;
    if (idx >= 0 && idx < len) {
        if (JS_SetPropertyInt64(ctx, result, idx, JS_DupValue(ctx, val)) < 0) goto done;
    }
    ret = result; result = JS_UNDEFINED;
 done:
    JS_FreeValue(ctx, result);
    JS_FreeValue(ctx, obj);
    return ret;
}

/* _move(from, to) -> a copy with the item at `from` relocated to `to`
 * (negative indices from the end); out-of-range returns an unchanged copy.
 * Built as three contiguous bulk blits into a pre-sized fast array (no
 * per-element property dispatch) — the removal+insertion is expressed as
 * disjoint source ranges, so each element is dup'd exactly once. */
static JSValue js_array_ext_move(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    JSValue obj, src = JS_UNDEFINED, result = JS_UNDEFINED, ret = JS_EXCEPTION;
    JSValue *srcp, *dst;
    JSObject *rp;
    uint32_t scount;
    int64_t len, from, to, w = 0;
    obj = JS_ToObject(ctx, this_val);
    if (JS_ToInt64Sat(ctx, &from, argc > 0 ? argv[0] : JS_UNDEFINED)) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    if (JS_ToInt64Sat(ctx, &to, argc > 1 ? argv[1] : JS_UNDEFINED)) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    if (js_get_length64(ctx, &len, obj)) goto done;
    src = js_array_ext_build_range(ctx, obj, 0, len);   /* stable fast copy */
    if (JS_IsException(src)) goto done;
    if (from < 0) from += len;
    if (to < 0) to += len;
    if (from < 0 || from >= len || to < 0 || to >= len || from == to) { ret = src; src = JS_UNDEFINED; goto done; }
    if (!js_get_fast_array(ctx, src, &srcp, &scount) || (int64_t)scount != len) {
        ret = src; src = JS_UNDEFINED; goto done;       /* defensive: shouldn't happen */
    }
    result = js_allocate_fast_array(ctx, len);           /* slots pre-filled UNDEFINED */
    if (JS_IsException(result)) goto done;
    rp = JS_VALUE_GET_OBJ(result);
    dst = rp->u.array.u.values;
    /* item = src[from], then blit the disjoint kept ranges around target `to`. */
    if (from < to) {
        int64_t i;
        for (i = 0; i < from; i++)       dst[w++] = JS_DupValue(ctx, srcp[i]);
        for (i = from + 1; i <= to; i++) dst[w++] = JS_DupValue(ctx, srcp[i]);
        dst[w++] = JS_DupValue(ctx, srcp[from]);
        for (i = to + 1; i < len; i++)   dst[w++] = JS_DupValue(ctx, srcp[i]);
    } else {                              /* from > to */
        int64_t i;
        for (i = 0; i < to; i++)         dst[w++] = JS_DupValue(ctx, srcp[i]);
        dst[w++] = JS_DupValue(ctx, srcp[from]);
        for (i = to; i < from; i++)      dst[w++] = JS_DupValue(ctx, srcp[i]);
        for (i = from + 1; i < len; i++) dst[w++] = JS_DupValue(ctx, srcp[i]);
    }
    ret = result; result = JS_UNDEFINED;
 done:
    JS_FreeValue(ctx, src);
    JS_FreeValue(ctx, result);
    JS_FreeValue(ctx, obj);
    return ret;
}

/* _swap(i, j) -> a copy with the elements at i and j exchanged (negative
 * indices from the end); out-of-range returns an unchanged copy. */
static JSValue js_array_ext_swap(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    JSValue obj, result = JS_UNDEFINED, ret = JS_EXCEPTION;
    int64_t len, i, j;
    obj = JS_ToObject(ctx, this_val);
    if (JS_ToInt64Sat(ctx, &i, argc > 0 ? argv[0] : JS_UNDEFINED)) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    if (JS_ToInt64Sat(ctx, &j, argc > 1 ? argv[1] : JS_UNDEFINED)) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    if (js_get_length64(ctx, &len, obj)) goto done;
    result = js_array_ext_build_range(ctx, obj, 0, len);
    if (JS_IsException(result)) goto done;
    if (i < 0) i += len;
    if (j < 0) j += len;
    if (i >= 0 && i < len && j >= 0 && j < len && i != j) {
        JSValue a, b;
        if (js_array_ext_getel(ctx, result, i, &a)) goto done;
        if (js_array_ext_getel(ctx, result, j, &b)) { JS_FreeValue(ctx, a); goto done; }
        if (JS_SetPropertyInt64(ctx, result, i, b) < 0) { JS_FreeValue(ctx, a); goto done; }
        if (JS_SetPropertyInt64(ctx, result, j, a) < 0) goto done;
    }
    ret = result; result = JS_UNDEFINED;
 done:
    JS_FreeValue(ctx, result);
    JS_FreeValue(ctx, obj);
    return ret;
}

/* _nth(i) -> element at index i (negative from the end); undefined if out of range. */
static JSValue js_array_ext_nth(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    JSValue obj, ret = JS_EXCEPTION;
    int64_t len, i;
    obj = JS_ToObject(ctx, this_val);
    if (JS_ToInt64Sat(ctx, &i, argc > 0 ? argv[0] : JS_UNDEFINED)) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    if (js_get_length64(ctx, &len, obj)) goto done;
    if (i < 0) i += len;
    if (i < 0 || i >= len) { ret = JS_UNDEFINED; goto done; }
    if (js_array_ext_getel(ctx, obj, i, &ret)) ret = JS_EXCEPTION;
 done:
    JS_FreeValue(ctx, obj);
    return ret;
}

/* _init() -> all but the last element. */
static JSValue js_array_ext_init(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    JSValue obj, ret = JS_EXCEPTION;
    int64_t len;
    (void)argc; (void)argv;
    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj)) goto done;
    ret = js_array_ext_build_range(ctx, obj, 0, len > 0 ? len - 1 : 0);
 done:
    JS_FreeValue(ctx, obj);
    return ret;
}

/* _tail() -> all but the first element. */
static JSValue js_array_ext_tail(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    JSValue obj, ret = JS_EXCEPTION;
    int64_t len;
    (void)argc; (void)argv;
    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj)) goto done;
    ret = js_array_ext_build_range(ctx, obj, len > 0 ? 1 : 0, len);
 done:
    JS_FreeValue(ctx, obj);
    return ret;
}

/* _takeWhile/_dropWhile/_takeLastWhile/_dropLastWhile(matcher): matcher is a
 * predicate function or a value (SameValueZero). magic: 0 takeWhile, 1 dropWhile,
 * 2 takeLastWhile, 3 dropLastWhile. */
static JSValue js_array_ext_whilst(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv, int magic)
{
    JSValue obj, ret = JS_EXCEPTION;
    JSValueConst matcher = argc > 0 ? argv[0] : JS_UNDEFINED;
    JSExtMatcher pm = { JS_UNDEFINED, JS_UNDEFINED, 0 };
    int64_t len, i = 0;
    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj)) goto done;
    if (js_ext_matcher_begin(ctx, &pm, matcher)) goto done;
    if (magic < 2) {                        /* scan from the front */
        for (i = 0; i < len; i++) {
            JSValue el;
            int m;
            if (js_array_ext_getel(ctx, obj, i, &el)) goto done;
            m = js_ext_matcher_test(ctx, &pm, el);
            JS_FreeValue(ctx, el);
            if (m < 0) goto done;
            if (!m) break;
        }
        ret = (magic == 0) ? js_array_ext_build_range(ctx, obj, 0, i)
                           : js_array_ext_build_range(ctx, obj, i, len);
    } else {                                /* scan from the back */
        for (i = len; i > 0; i--) {
            JSValue el;
            int m;
            if (js_array_ext_getel(ctx, obj, i - 1, &el)) goto done;
            m = js_ext_matcher_test(ctx, &pm, el);
            JS_FreeValue(ctx, el);
            if (m < 0) goto done;
            if (!m) break;
        }
        ret = (magic == 2) ? js_array_ext_build_range(ctx, obj, i, len)
                           : js_array_ext_build_range(ctx, obj, 0, i);
    }
 done:
    js_ext_matcher_end(ctx, &pm);
    JS_FreeValue(ctx, obj);
    return ret;
}

/* _append(x) -> a copy with x added at the end. */
static JSValue js_array_ext_append(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    JSValue obj, result, ret = JS_EXCEPTION;
    JSValueConst x = argc > 0 ? argv[0] : JS_UNDEFINED;
    int64_t len;
    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj)) goto done;
    result = js_array_ext_build_range(ctx, obj, 0, len);
    if (JS_IsException(result)) goto done;
    if (JS_DefinePropertyValueInt64(ctx, result, len, JS_DupValue(ctx, x), JS_PROP_C_W_E) < 0) { JS_FreeValue(ctx, result); goto done; }
    ret = result;
 done:
    JS_FreeValue(ctx, obj);
    return ret;
}

/* _prepend(x) -> a copy with x added at the front, built as one
 * pre-sized fast array + a bulk blit of the tail. */
static JSValue js_array_ext_prepend(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    JSValue obj, result = JS_UNDEFINED, ret = JS_EXCEPTION;
    JSValueConst x = argc > 0 ? argv[0] : JS_UNDEFINED;
    JSValue *srcp, *dst;
    JSObject *rp;
    uint32_t scount;
    int64_t len, i;
    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj)) goto done;
    result = js_allocate_fast_array(ctx, len + 1);       /* slots pre-filled UNDEFINED */
    if (JS_IsException(result)) goto done;
    rp = JS_VALUE_GET_OBJ(result);
    dst = rp->u.array.u.values;
    dst[0] = JS_DupValue(ctx, x);
    if (js_get_fast_array(ctx, obj, &srcp, &scount) && (int64_t)scount >= len) {
        for (i = 0; i < len; i++)
            dst[1 + i] = JS_DupValue(ctx, srcp[i]);
    } else {
        for (i = 0; i < len; i++)
            if (js_array_ext_getel(ctx, obj, i, &dst[1 + i])) goto done;
    }
    ret = result; result = JS_UNDEFINED;
 done:
    JS_FreeValue(ctx, result);
    JS_FreeValue(ctx, obj);
    return ret;
}

/* _reject(matcher) -> the elements the matcher REJECTS (complement of filter);
 * matcher is a predicate function or a value (SameValueZero). */
static JSValue js_array_ext_reject(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    JSValue obj, result = JS_UNDEFINED, ret = JS_EXCEPTION;
    JSValueConst matcher = argc > 0 ? argv[0] : JS_UNDEFINED;
    JSExtMatcher pm = { JS_UNDEFINED, JS_UNDEFINED, 0 };
    int64_t len, i, j = 0;
    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj)) goto done;
    if (js_ext_matcher_begin(ctx, &pm, matcher)) goto done;
    result = JS_NewArray(ctx);
    if (JS_IsException(result)) { result = JS_UNDEFINED; goto done; }
    for (i = 0; i < len; i++) {
        JSValue el;
        int m;
        if (js_array_ext_getel(ctx, obj, i, &el)) goto done;
        m = js_ext_matcher_test(ctx, &pm, el);
        if (m < 0) { JS_FreeValue(ctx, el); goto done; }
        if (!m) {
            if (JS_DefinePropertyValueInt64(ctx, result, j++, el, JS_PROP_C_W_E) < 0) goto done;
        } else {
            JS_FreeValue(ctx, el);
        }
    }
    ret = result; result = JS_UNDEFINED;
 done:
    js_ext_matcher_end(ctx, &pm);
    JS_FreeValue(ctx, result);
    JS_FreeValue(ctx, obj);
    return ret;
}

/* _insert(idx, elt) -> a copy with elt inserted at idx; an idx outside [0,len)
 * appends. */
static JSValue js_array_ext_insert(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    JSValue obj, result = JS_UNDEFINED, ret = JS_EXCEPTION;
    JSValueConst elt = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSValue *srcp, *dst;
    JSObject *rp;
    uint32_t scount;
    int64_t len, idx, i;
    obj = JS_ToObject(ctx, this_val);
    if (JS_ToInt64Sat(ctx, &idx, argc > 0 ? argv[0] : JS_UNDEFINED)) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    if (js_get_length64(ctx, &len, obj)) goto done;
    if (idx < 0 || idx > len) idx = len;
    result = js_allocate_fast_array(ctx, len + 1);
    if (JS_IsException(result)) goto done;
    rp = JS_VALUE_GET_OBJ(result);
    dst = rp->u.array.u.values;
    if (js_get_fast_array(ctx, obj, &srcp, &scount) && (int64_t)scount >= len) {
        for (i = 0; i < idx; i++)   dst[i] = JS_DupValue(ctx, srcp[i]);
        dst[idx] = JS_DupValue(ctx, elt);
        for (i = idx; i < len; i++) dst[i + 1] = JS_DupValue(ctx, srcp[i]);
    } else {
        for (i = 0; i < idx; i++)   if (js_array_ext_getel(ctx, obj, i, &dst[i])) goto done;
        dst[idx] = JS_DupValue(ctx, elt);
        for (i = idx; i < len; i++) if (js_array_ext_getel(ctx, obj, i, &dst[i + 1])) goto done;
    }
    ret = result; result = JS_UNDEFINED;
 done:
    JS_FreeValue(ctx, result);
    JS_FreeValue(ctx, obj);
    return ret;
}

/* _insertAll(idx, elts) -> a copy with every element of elts inserted at idx;
 * an idx outside [0,len) appends. */
static JSValue js_array_ext_insertall(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    JSValue obj, elts, result = JS_UNDEFINED, ret = JS_EXCEPTION;
    JSObject *rp;
    JSValue *dst;
    int64_t len, elen, idx, i, w = 0;
    obj = JS_ToObject(ctx, this_val);
    if (JS_ToInt64Sat(ctx, &idx, argc > 0 ? argv[0] : JS_UNDEFINED)) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    if (js_get_length64(ctx, &len, obj)) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    elts = JS_ToObject(ctx, argc > 1 ? argv[1] : JS_UNDEFINED);
    if (JS_IsException(elts)) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    if (js_get_length64(ctx, &elen, elts)) goto done;
    if (idx < 0 || idx > len) idx = len;
    result = js_allocate_fast_array(ctx, len + elen);
    if (JS_IsException(result)) goto done;
    rp = JS_VALUE_GET_OBJ(result);
    dst = rp->u.array.u.values;
    for (i = 0; i < idx; i++)   if (js_array_ext_getel(ctx, obj, i, &dst[w++])) goto done;
    for (i = 0; i < elen; i++)  if (js_array_ext_getel(ctx, elts, i, &dst[w++])) goto done;
    for (i = idx; i < len; i++) if (js_array_ext_getel(ctx, obj, i, &dst[w++])) goto done;
    ret = result; result = JS_UNDEFINED;
 done:
    JS_FreeValue(ctx, elts);
    JS_FreeValue(ctx, result);
    JS_FreeValue(ctx, obj);
    return ret;
}

/* _removeAt(idx) -> a copy without the element at idx (negative from the end);
 * out-of-range returns an unchanged copy. */
static JSValue js_array_ext_removeat(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    JSValue obj, result = JS_UNDEFINED, ret = JS_EXCEPTION;
    JSValue *srcp, *dst;
    JSObject *rp;
    uint32_t scount;
    int64_t len, idx, i, w = 0;
    obj = JS_ToObject(ctx, this_val);
    if (JS_ToInt64Sat(ctx, &idx, argc > 0 ? argv[0] : JS_UNDEFINED)) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    if (js_get_length64(ctx, &len, obj)) goto done;
    if (idx < 0) idx += len;
    if (idx < 0 || idx >= len) {          /* out of range -> unchanged copy */
        ret = js_array_ext_build_range(ctx, obj, 0, len);
        goto done;
    }
    result = js_allocate_fast_array(ctx, len - 1);
    if (JS_IsException(result)) goto done;
    rp = JS_VALUE_GET_OBJ(result);
    dst = rp->u.array.u.values;
    if (js_get_fast_array(ctx, obj, &srcp, &scount) && (int64_t)scount >= len) {
        for (i = 0; i < len; i++) if (i != idx) dst[w++] = JS_DupValue(ctx, srcp[i]);
    } else {
        for (i = 0; i < len; i++) if (i != idx) { if (js_array_ext_getel(ctx, obj, i, &dst[w++])) goto done; }
    }
    ret = result; result = JS_UNDEFINED;
 done:
    JS_FreeValue(ctx, result);
    JS_FreeValue(ctx, obj);
    return ret;
}

/* _zipObj(values) -> an object mapping this[i] (as key) to values[i], truncated
 * to the shorter length. */
static JSValue js_array_ext_zipobj(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    JSValue obj, vals, result = JS_UNDEFINED, ret = JS_EXCEPTION;
    int64_t klen, vlen, n, i;
    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &klen, obj)) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    vals = JS_ToObject(ctx, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (JS_IsException(vals)) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    if (js_get_length64(ctx, &vlen, vals)) goto done;
    n = klen < vlen ? klen : vlen;
    result = JS_NewObject(ctx);
    if (JS_IsException(result)) goto done;
    for (i = 0; i < n; i++) {
        JSValue k, v;
        JSAtom a;
        if (js_array_ext_getel(ctx, obj, i, &k)) goto done;
        a = JS_ValueToAtom(ctx, k);
        JS_FreeValue(ctx, k);
        if (a == JS_ATOM_NULL) goto done;
        if (js_array_ext_getel(ctx, vals, i, &v)) { JS_FreeAtom(ctx, a); goto done; }
        if (JS_DefinePropertyValue(ctx, result, a, v, JS_PROP_C_W_E) < 0) { JS_FreeAtom(ctx, a); goto done; }
        JS_FreeAtom(ctx, a);
    }
    ret = result; result = JS_UNDEFINED;
 done:
    JS_FreeValue(ctx, result);
    JS_FreeValue(ctx, vals);
    JS_FreeValue(ctx, obj);
    return ret;
}

/* _fromPairs() -> an object built from [key, value] pairs;
 * later pairs win on duplicate keys. */
static JSValue js_array_ext_frompairs(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    JSValue obj, result = JS_UNDEFINED, ret = JS_EXCEPTION;
    int64_t len, i;
    (void)argc; (void)argv;
    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj)) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    result = JS_NewObject(ctx);
    if (JS_IsException(result)) goto done;
    for (i = 0; i < len; i++) {
        JSValue pair, k, v;
        JSAtom a;
        if (js_array_ext_getel(ctx, obj, i, &pair)) goto done;
        if (js_array_ext_getel(ctx, pair, 0, &k)) { JS_FreeValue(ctx, pair); goto done; }
        a = JS_ValueToAtom(ctx, k);
        JS_FreeValue(ctx, k);
        if (a == JS_ATOM_NULL) { JS_FreeValue(ctx, pair); goto done; }
        if (js_array_ext_getel(ctx, pair, 1, &v)) { JS_FreeAtom(ctx, a); JS_FreeValue(ctx, pair); goto done; }
        JS_FreeValue(ctx, pair);
        if (JS_DefinePropertyValue(ctx, result, a, v, JS_PROP_C_W_E) < 0) { JS_FreeAtom(ctx, a); goto done; }
        JS_FreeAtom(ctx, a);
    }
    ret = result; result = JS_UNDEFINED;
 done:
    JS_FreeValue(ctx, result);
    JS_FreeValue(ctx, obj);
    return ret;
}

/* Fallback comparator for the depth-budget path below. Valid only because the
 * buffer is NaN-free there: with a NaN present this returns 0 for every pair
 * involving it, which is not an ordering and leaves qsort unspecified. */
static int js_array_ext_cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

/* Place the k-th smallest of a[0..n) at a[k], with a[0..k) <= a[k] <= a(k..n).
 * REQUIRES a NaN-free buffer: a NaN pivot makes both compares false and the
 * three-way split stops making progress. Ties among equal doubles are arbitrary. */
static void js_array_ext_select_double(double *a, int64_t n, int64_t k)
{
    int64_t lo = 0, hi = n - 1, m;
    int budget = 0;
    for (m = n; m > 1; m >>= 1)
        budget++;
    budget = 2 * budget + 3;                /* introselect: bounds the bad case */
    while (lo < hi) {
        double p, t;
        int64_t lt, i, gt, mid;
        if (hi - lo < 12) {                 /* short range: sort it outright */
            for (i = lo + 1; i <= hi; i++) {
                int64_t j = i;
                t = a[i];
                while (j > lo && a[j - 1] > t) { a[j] = a[j - 1]; j--; }
                a[j] = t;
            }
            return;
        }
        if (budget-- <= 0) {                /* O(n log n) worst case, not O(n^2) */
            qsort(a + lo, (size_t)(hi - lo + 1), sizeof(double),
                  js_array_ext_cmp_double);
            return;
        }
        mid = lo + (hi - lo) / 2;           /* median-of-three: sorted input is linear */
        if (a[mid] < a[lo])  { t = a[mid]; a[mid] = a[lo];  a[lo]  = t; }
        if (a[hi]  < a[lo])  { t = a[hi];  a[hi]  = a[lo];  a[lo]  = t; }
        if (a[hi]  < a[mid]) { t = a[hi];  a[hi]  = a[mid]; a[mid] = t; }
        p = a[mid];
        lt = lo; i = lo; gt = hi;           /* three-way split: all-equal is O(n) */
        while (i <= gt) {
            if (a[i] < p)      { t = a[lt]; a[lt] = a[i]; a[i] = t; lt++; i++; }
            else if (a[i] > p) { t = a[gt]; a[gt] = a[i]; a[i] = t; gt--; }
            else               { i++; }
        }
        if (k < lt)       hi = lt - 1;
        else if (k <= gt) return;           /* a[k] == p: already in place */
        else              lo = gt + 1;
    }
}

/* _median() -> the median of the elements coerced to numbers; NaN if empty.
 * Coerces every element into a C buffer FIRST (valueOf may run JS), then selects
 * in C. NaN anywhere propagates, as _sum/_average/_product already do. */
static JSValue js_array_ext_median(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    JSValue obj, ret = JS_EXCEPTION;
    double *buf = NULL, med, lmax;
    int64_t len, i, k;
    int has_nan = 0;
    (void)argc; (void)argv;
    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj)) goto done;
    if (len == 0) { ret = JS_NewFloat64(ctx, NAN); goto done; }
    buf = js_malloc(ctx, sizeof(double) * len);
    if (!buf) goto done;
    for (i = 0; i < len; i++) {
        JSValue v;
        double d;
        int r;
        if (js_array_ext_getel(ctx, obj, i, &v)) goto done;
        r = JS_ToFloat64(ctx, &d, v);
        JS_FreeValue(ctx, v);
        if (r) goto done;
        has_nan |= (d != d);             /* IEEE: only NaN is != itself */
        buf[i] = d;
    }
    if (has_nan) { ret = JS_NewFloat64(ctx, NAN); goto done; }
    k = len / 2;
    js_array_ext_select_double(buf, len, k);
    if (len & 1) {
        med = buf[k];
    } else {
        /* select leaves buf[0..k) <= buf[k], so their max IS the (k-1)th order
         * statistic. Sum-then-halve keeps the old overflow result (1e308 pair
         * -> Infinity); x/2+y/2 would silently change it. */
        lmax = buf[0];
        for (i = 1; i < k; i++)
            if (buf[i] > lmax) lmax = buf[i];
        med = (lmax + buf[k]) / 2.0;
    }
    ret = JS_NewFloat64(ctx, med);
 done:
    js_free(ctx, buf);
    JS_FreeValue(ctx, obj);
    return ret;
}

/* _product() -> the product of the elements coerced to numbers; 1 if empty. */
static JSValue js_array_ext_product(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    JSValue obj, ret = JS_EXCEPTION;
    int64_t len, i;
    double acc = 1;
    (void)argc; (void)argv;
    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj)) goto done;
    for (i = 0; i < len; i++) {
        JSValue v;
        double d;
        int r;
        if (js_array_ext_getel(ctx, obj, i, &v)) goto done;
        r = JS_ToFloat64(ctx, &d, v);
        JS_FreeValue(ctx, v);
        if (r) goto done;
        acc *= d;
    }
    ret = JS_NewFloat64(ctx, acc);
 done:
    JS_FreeValue(ctx, obj);
    return ret;
}

/* _scan(fn, acc) -> [acc, fn(acc,x0), fn(...,x1), ...] — reduce keeping every
 * intermediate; result length is len+1. */
static JSValue js_array_ext_scan(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    JSValue obj, result, acc, ret = JS_EXCEPTION;
    JSValueConst fn = argc > 0 ? argv[0] : JS_UNDEFINED;
    int64_t len, i;
    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj)) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    acc = JS_DupValue(ctx, argc > 1 ? argv[1] : JS_UNDEFINED);
    result = JS_NewArray(ctx);
    if (JS_IsException(result)) { JS_FreeValue(ctx, acc); goto done; }
    if (JS_DefinePropertyValueInt64(ctx, result, 0, JS_DupValue(ctx, acc), JS_PROP_C_W_E) < 0) { JS_FreeValue(ctx, acc); JS_FreeValue(ctx, result); goto done; }
    for (i = 0; i < len; i++) {
        JSValue el, nv;
        JSValueConst args[2];
        if (js_array_ext_getel(ctx, obj, i, &el)) { JS_FreeValue(ctx, acc); JS_FreeValue(ctx, result); goto done; }
        args[0] = acc; args[1] = el;
        nv = JS_Call(ctx, fn, JS_UNDEFINED, 2, args);
        JS_FreeValue(ctx, el);
        JS_FreeValue(ctx, acc);
        if (JS_IsException(nv)) { JS_FreeValue(ctx, result); goto done; }
        acc = nv;
        if (JS_DefinePropertyValueInt64(ctx, result, i + 1, JS_DupValue(ctx, acc), JS_PROP_C_W_E) < 0) { JS_FreeValue(ctx, acc); JS_FreeValue(ctx, result); goto done; }
    }
    JS_FreeValue(ctx, acc);
    ret = result;
 done:
    JS_FreeValue(ctx, obj);
    return ret;
}

/* _countBy(fn) -> object mapping each key (fn(el), or a property/identity) to
 * the count of elements with that key. */
static JSValue js_array_ext_countby(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    JSValue obj, result = JS_UNDEFINED, ret = JS_EXCEPTION;
    JSValueConst fn = argc > 0 ? argv[0] : JS_UNDEFINED;
    int64_t len, i;
    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj)) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    result = JS_NewObject(ctx);
    if (JS_IsException(result)) goto done;
    for (i = 0; i < len; i++) {
        JSValue el, key, cur;
        JSAtom a;
        int32_t c = 0;
        if (js_array_ext_getel(ctx, obj, i, &el)) goto done;
        key = js_array_ext_mapval(ctx, fn, el);
        JS_FreeValue(ctx, el);
        if (JS_IsException(key)) goto done;
        a = JS_ValueToAtom(ctx, key);
        JS_FreeValue(ctx, key);
        if (a == JS_ATOM_NULL) goto done;
        cur = JS_GetProperty(ctx, result, a);
        if (JS_IsException(cur)) { JS_FreeAtom(ctx, a); goto done; }
        if (!JS_IsUndefined(cur) && JS_ToInt32(ctx, &c, cur)) { JS_FreeValue(ctx, cur); JS_FreeAtom(ctx, a); goto done; }
        JS_FreeValue(ctx, cur);
        if (JS_DefinePropertyValue(ctx, result, a, JS_NewInt32(ctx, c + 1), JS_PROP_C_W_E) < 0) { JS_FreeAtom(ctx, a); goto done; }
        JS_FreeAtom(ctx, a);
    }
    ret = result; result = JS_UNDEFINED;
 done:
    JS_FreeValue(ctx, result);
    JS_FreeValue(ctx, obj);
    return ret;
}

/* _indexBy(fn) -> object mapping each key (fn(el), or a property/identity) to
 * the LAST element with that key. */
static JSValue js_array_ext_indexby(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    JSValue obj, result = JS_UNDEFINED, ret = JS_EXCEPTION;
    JSValueConst fn = argc > 0 ? argv[0] : JS_UNDEFINED;
    int64_t len, i;
    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj)) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    result = JS_NewObject(ctx);
    if (JS_IsException(result)) goto done;
    for (i = 0; i < len; i++) {
        JSValue el, key;
        JSAtom a;
        if (js_array_ext_getel(ctx, obj, i, &el)) goto done;
        key = js_array_ext_mapval(ctx, fn, el);
        if (JS_IsException(key)) { JS_FreeValue(ctx, el); goto done; }
        a = JS_ValueToAtom(ctx, key);
        JS_FreeValue(ctx, key);
        if (a == JS_ATOM_NULL) { JS_FreeValue(ctx, el); goto done; }
        if (JS_DefinePropertyValue(ctx, result, a, el, JS_PROP_C_W_E) < 0) { JS_FreeAtom(ctx, a); goto done; } /* consumes el; last wins */
        JS_FreeAtom(ctx, a);
    }
    ret = result; result = JS_UNDEFINED;
 done:
    JS_FreeValue(ctx, result);
    JS_FreeValue(ctx, obj);
    return ret;
}

/* removeRange(start, count) -> a copy with `count` elements removed starting at
 * `start` (negative from the end), i.e. non-mutating splice. */
static JSValue js_array_ext_removerange(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv)
{
    JSValue obj, result = JS_UNDEFINED, ret = JS_EXCEPTION;
    JSValue *srcp, *dst;
    JSObject *rp;
    uint32_t scount;
    int64_t len, start, count, end, i, w = 0;
    obj = JS_ToObject(ctx, this_val);
    if (JS_ToInt64Sat(ctx, &start, argc > 0 ? argv[0] : JS_UNDEFINED)) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    if (JS_ToInt64Sat(ctx, &count, argc > 1 ? argv[1] : JS_UNDEFINED)) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    if (js_get_length64(ctx, &len, obj)) goto done;
    if (start < 0) start += len;
    if (start < 0) start = 0;
    if (start > len) start = len;
    if (count < 0) count = 0;
    end = start + count;
    if (end > len) end = len;
    result = js_allocate_fast_array(ctx, len - (end - start));
    if (JS_IsException(result)) goto done;
    rp = JS_VALUE_GET_OBJ(result);
    dst = rp->u.array.u.values;
    if (js_get_fast_array(ctx, obj, &srcp, &scount) && (int64_t)scount >= len) {
        for (i = 0; i < start; i++)   dst[w++] = JS_DupValue(ctx, srcp[i]);
        for (i = end; i < len; i++)   dst[w++] = JS_DupValue(ctx, srcp[i]);
    } else {
        for (i = 0; i < start; i++)   if (js_array_ext_getel(ctx, obj, i, &dst[w++])) goto done;
        for (i = end; i < len; i++)   if (js_array_ext_getel(ctx, obj, i, &dst[w++])) goto done;
    }
    ret = result; result = JS_UNDEFINED;
 done:
    JS_FreeValue(ctx, result);
    JS_FreeValue(ctx, obj);
    return ret;
}

/* splitWhen(matcher) -> [ before, fromFirstMatchOnward ] splitting at the first
 * element the matcher accepts (matcher = predicate/value/regex).
 * If none match, [ all, [] ]. */
static JSValue js_array_ext_splitwhen(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    JSValue obj, left = JS_UNDEFINED, right = JS_UNDEFINED, result, ret = JS_EXCEPTION;
    JSExtMatcher pm = { JS_UNDEFINED, JS_UNDEFINED, 0 };
    int64_t len, i;
    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj)) goto done;
    if (js_ext_matcher_begin(ctx, &pm, argc > 0 ? argv[0] : JS_UNDEFINED)) goto done;
    for (i = 0; i < len; i++) {
        JSValue el;
        int m;
        if (js_array_ext_getel(ctx, obj, i, &el)) goto done;
        m = js_ext_matcher_test(ctx, &pm, el);
        JS_FreeValue(ctx, el);
        if (m < 0) goto done;
        if (m) break;
    }
    left = js_array_ext_build_range(ctx, obj, 0, i);
    if (JS_IsException(left)) goto done;
    right = js_array_ext_build_range(ctx, obj, i, len);
    if (JS_IsException(right)) goto done;
    result = JS_NewArray(ctx);
    if (JS_IsException(result)) goto done;
    JS_DefinePropertyValueInt64(ctx, result, 0, left, JS_PROP_C_W_E);
    JS_DefinePropertyValueInt64(ctx, result, 1, right, JS_PROP_C_W_E);
    left = right = JS_UNDEFINED;
    ret = result;
 done:
    js_ext_matcher_end(ctx, &pm);
    JS_FreeValue(ctx, left);
    JS_FreeValue(ctx, right);
    JS_FreeValue(ctx, obj);
    return ret;
}

/* innerJoin(pred, other) -> the elements of this for which pred(element, y) holds
 * for some y in other. */
static JSValue js_array_ext_innerjoin(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    JSValue obj, other, result = JS_UNDEFINED, ret = JS_EXCEPTION;
    JSValueConst pred = argc > 0 ? argv[0] : JS_UNDEFINED;
    int64_t len, olen, i, k, j = 0;
    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj)) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    other = JS_ToObject(ctx, argc > 1 ? argv[1] : JS_UNDEFINED);
    if (JS_IsException(other)) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    if (js_get_length64(ctx, &olen, other)) goto done;
    result = JS_NewArray(ctx);
    if (JS_IsException(result)) goto done;
    for (i = 0; i < len; i++) {
        JSValue x;
        int matched = 0;
        if (js_array_ext_getel(ctx, obj, i, &x)) goto done;
        for (k = 0; k < olen; k++) {
            JSValue y, r;
            JSValueConst args[2];
            int b;
            if (js_array_ext_getel(ctx, other, k, &y)) { JS_FreeValue(ctx, x); goto done; }
            args[0] = x; args[1] = y;
            r = JS_Call(ctx, pred, JS_UNDEFINED, 2, args);
            JS_FreeValue(ctx, y);
            if (JS_IsException(r)) { JS_FreeValue(ctx, x); goto done; }
            b = JS_ToBool(ctx, r);
            JS_FreeValue(ctx, r);
            if (b) { matched = 1; break; }
        }
        if (matched) {
            if (JS_DefinePropertyValueInt64(ctx, result, j++, x, JS_PROP_C_W_E) < 0) goto done;
        } else {
            JS_FreeValue(ctx, x);
        }
    }
    ret = result; result = JS_UNDEFINED;
 done:
    JS_FreeValue(ctx, result);
    JS_FreeValue(ctx, other);
    JS_FreeValue(ctx, obj);
    return ret;
}

/* ============================================================================
 * Array *FromIndex methods (9): map/forEach/filter/find/findIndex/some/
 * every/reduce/reduceRight, each starting at a given index with optional wrap.
 * The quirks are deliberate: positional-by-type [loop] boolean; callbacks see
 * the ORIGINAL element and the de-shifted original index; falsy reduce
 * initialValue is DROPPED (seed quirk); reduceRight non-loop reports shifted
 * indices. Matcher/mapper overloads reuse this file's documented conventions
 * (function / RegExp.test / SameValueZero; function / single-property / identity)
 * — deep-equal, Date, fuzzy-object, dotted-path and array-of-paths
 * overloads are NOT replicated (same divergence as the other ext methods).
 * ========================================================================== */

enum { FI_MAP = 0, FI_FOREACH, FI_FILTER, FI_FIND, FI_FINDINDEX,
       FI_SOME, FI_EVERY, FI_REDUCE, FI_REDUCERIGHT };
enum { FI_OV_FN = 0, FI_OV_REGEX, FI_OV_VALUE, FI_OV_PROP };

/* Normalize a start index: with loop, wrap modulo length; negatives count from
 * the end. */
static int64_t js_fi_norm(int64_t index, int64_t length, int loop)
{
    if (index && loop && length) index = index % length;
    if (index < 0) index += length;
    return index;
}

/* Evaluate the resolved callback/search on one element. Returns an owned value:
 * the mapped value (map family) or a truthy/falsy value (match family). */
static JSValue js_fi_call(JSContext *ctx, int ov, JSValueConst fn, JSValueConst context,
                          JSValueConst regex_test, JSAtom prop_atom,
                          JSValueConst el, int64_t ridx, JSValueConst obj)
{
    switch (ov) {
    case FI_OV_FN: {
        JSValueConst a3[3];
        JSValue idx = JS_NewInt64(ctx, ridx), r;
        a3[0] = el; a3[1] = idx; a3[2] = obj;
        r = JS_Call(ctx, fn, context, 3, a3);
        JS_FreeValue(ctx, idx);
        return r;
    }
    case FI_OV_REGEX: {
        JSValue s = JS_ToString(ctx, el), r;
        if (JS_IsException(s)) return s;
        r = JS_Call(ctx, regex_test, fn, 1, (JSValueConst *)&s);
        JS_FreeValue(ctx, s);
        return r;
    }
    case FI_OV_VALUE:
        return JS_NewBool(ctx, JS_SameValueZero(ctx, fn, el));
    default: /* FI_OV_PROP */
        return JS_GetProperty(ctx, el, prop_atom);
    }
}

static JSValue js_array_fromindex(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv, int kind)
{
    JSValue obj, ret = JS_EXCEPTION, regex_test = JS_UNDEFINED;
    JSValue result = JS_UNDEFINED, acc = JS_UNINITIALIZED;
    JSValueConst fn, context = JS_UNDEFINED, lastArg = JS_UNDEFINED;
    JSAtom prop_atom = JS_ATOM_NULL;
    int64_t len, startIndex, i, w = 0, foundIdx = -1;
    int ai, loop = 0, hasLast, ov, is_match, seeded, started = 0, is_right;

    obj = JS_ToObject(ctx, this_val);
    if (argc < 2) { JS_ThrowTypeError(ctx, "Argument required"); goto done; }
    if (JS_ToInt64Sat(ctx, &startIndex, argv[0])) goto done;
    if (js_get_length64(ctx, &len, obj)) goto done;

    ai = 1;
    if (JS_VALUE_GET_TAG(argv[ai]) == JS_TAG_BOOL) { loop = JS_VALUE_GET_INT(argv[ai]); ai++; }
    fn = ai < argc ? argv[ai] : JS_UNDEFINED; ai++;
    hasLast = ai < argc;
    lastArg = hasLast ? argv[ai] : JS_UNDEFINED;
    context = hasLast ? lastArg : JS_UNDEFINED;

    is_right = (kind == FI_REDUCERIGHT);
    is_match = (kind == FI_FILTER || kind == FI_FIND || kind == FI_FINDINDEX ||
                kind == FI_SOME || kind == FI_EVERY);

    if (startIndex < 0) startIndex += len;
    if (is_right) { if (startIndex < -1) startIndex = -1; if (startIndex > len) startIndex = len; }
    else          { if (startIndex < 0) startIndex = 0;   if (startIndex > len) startIndex = len; }

    /* resolve the callback overload once */
    if (JS_IsFunction(ctx, fn)) {
        ov = FI_OV_FN;
    } else if (is_match) {
        if (JS_VALUE_GET_TAG(fn) == JS_TAG_OBJECT &&
            JS_VALUE_GET_OBJ(fn)->class_id == JS_CLASS_REGEXP) {
            ov = FI_OV_REGEX;
            regex_test = JS_GetPropertyStr(ctx, fn, "test");
            if (JS_IsException(regex_test)) { regex_test = JS_UNDEFINED; goto done; }
        } else {
            ov = FI_OV_VALUE;
        }
    } else if (kind == FI_MAP && JS_ToBool(ctx, fn)) {
        ov = FI_OV_PROP;
        prop_atom = JS_ValueToAtom(ctx, fn);
        if (prop_atom == JS_ATOM_NULL) goto done;
    } else {
        /* map with falsy mapper, or forEach/reduce/reduceRight with a
         * non-function: native throws TypeError. */
        JS_ThrowTypeError(ctx, "callback is not a function");
        goto done;
    }

    switch (kind) {
    case FI_MAP: case FI_FILTER:
        result = JS_NewArray(ctx);
        if (JS_IsException(result)) { result = JS_UNDEFINED; goto done; }
        break;
    case FI_FIND:  result = JS_UNDEFINED; break;
    default: break;
    }

    if (kind == FI_REDUCE || kind == FI_REDUCERIGHT) {
        int64_t sl, p, total;
        seeded = hasLast;
        if (seeded) acc = JS_DupValue(ctx, lastArg);
        if (is_right) {
            sl = startIndex + 1;
            if (loop) sl = len;
            if (sl < 0) sl = 0; if (sl > len) sl = len;
            total = sl;
        } else {
            total = (len - startIndex) + (loop ? startIndex : 0);
        }
        for (p = 0; p < total; p++) {
            int64_t vidx, ridx;
            JSValue el, r;
            JSValueConst a4[4];
            JSValue idx;
            if (is_right) {
                int64_t si = (loop ? len : sl) - 1 - p;
                vidx = si;
                ridx = loop ? js_fi_norm(si + startIndex, len, 1) : (si + startIndex);
            } else if (p < len - startIndex) {
                vidx = ridx = startIndex + p;
            } else {
                vidx = ridx = p - (len - startIndex);
            }
            if (js_array_ext_getel(ctx, obj, vidx, &el)) goto done;
            if (!seeded && !started) { acc = el; started = 1; continue; }
            idx = JS_NewInt64(ctx, ridx);
            a4[0] = acc; a4[1] = el; a4[2] = idx; a4[3] = obj;
            r = JS_Call(ctx, fn, obj, 4, a4);
            JS_FreeValue(ctx, idx);
            JS_FreeValue(ctx, el);
            JS_FreeValue(ctx, acc);
            acc = JS_UNINITIALIZED;
            if (JS_IsException(r)) goto done;
            acc = r;
        }
        if (!seeded && !started) { JS_ThrowTypeError(ctx, "Reduce of empty array with no initial value"); goto done; }
        ret = acc; acc = JS_UNINITIALIZED;
        goto done;
    }

    /* iteration / map / match families (left sequence: [startIndex,len) then
     * (loop) [0,startIndex)). */
    {
        int phase;
        for (phase = 0; phase < 2; phase++) {
            int64_t lo, hi;
            if (phase == 0) { lo = startIndex; hi = len; }
            else { if (!loop) break; lo = 0; hi = startIndex; }
            for (i = lo; i < hi; i++) {
                JSValue el, r;
                int b;
                if (js_array_ext_getel(ctx, obj, i, &el)) goto done;
                r = js_fi_call(ctx, ov, fn, context, regex_test, prop_atom, el, i, obj);
                if (JS_IsException(r)) { JS_FreeValue(ctx, el); goto done; }
                switch (kind) {
                case FI_MAP:
                    JS_FreeValue(ctx, el);
                    if (JS_DefinePropertyValueInt64(ctx, result, w++, r, JS_PROP_C_W_E) < 0) goto done;
                    break;
                case FI_FOREACH:
                    JS_FreeValue(ctx, r);
                    JS_FreeValue(ctx, el);
                    break;
                case FI_FILTER:
                    b = JS_ToBool(ctx, r); JS_FreeValue(ctx, r);
                    if (b) { if (JS_DefinePropertyValueInt64(ctx, result, w++, el, JS_PROP_C_W_E) < 0) goto done; }
                    else JS_FreeValue(ctx, el);
                    break;
                case FI_FIND:
                    b = JS_ToBool(ctx, r); JS_FreeValue(ctx, r);
                    if (b) { result = el; ret = result; result = JS_UNDEFINED; goto done; }
                    JS_FreeValue(ctx, el);
                    break;
                case FI_FINDINDEX:
                    b = JS_ToBool(ctx, r); JS_FreeValue(ctx, r);
                    JS_FreeValue(ctx, el);
                    if (b) { foundIdx = i; ret = JS_NewInt64(ctx, foundIdx); goto done; }
                    break;
                case FI_SOME:
                    b = JS_ToBool(ctx, r); JS_FreeValue(ctx, r);
                    JS_FreeValue(ctx, el);
                    if (b) { ret = JS_TRUE; goto done; }
                    break;
                case FI_EVERY:
                    b = JS_ToBool(ctx, r); JS_FreeValue(ctx, r);
                    JS_FreeValue(ctx, el);
                    if (!b) { ret = JS_FALSE; goto done; }
                    break;
                }
            }
        }
    }
    switch (kind) {
    case FI_MAP: case FI_FILTER: ret = result; result = JS_UNDEFINED; break;
    case FI_FIND: ret = JS_UNDEFINED; break;
    case FI_FINDINDEX: ret = JS_NewInt64(ctx, -1); break;
    case FI_SOME: ret = JS_FALSE; break;
    case FI_EVERY: ret = JS_TRUE; break;
    case FI_FOREACH: ret = JS_UNDEFINED; break;
    default: break;
    }
 done:
    if (JS_VALUE_GET_TAG(acc) != JS_TAG_UNINITIALIZED) JS_FreeValue(ctx, acc);
    if (prop_atom != JS_ATOM_NULL) JS_FreeAtom(ctx, prop_atom);
    JS_FreeValue(ctx, regex_test);
    JS_FreeValue(ctx, result);
    JS_FreeValue(ctx, obj);
    return ret;
}

/* ============================================================================
 * Array batch: startsWith/endsWith, unnest, dropRepeats family, sortWith, the
 * predicate set-ops (unionWith/differenceWith/symmetricDifference[With]),
 * reduceBy, and the static Array.repeat.
 * ========================================================================== */

#define ARR_EXT_MAX 100000000   /* DoS cap for count-driven builders (repeat) */

/* startsWith(prefix)/endsWith(suffix): deep-equality element-wise against the
 * first/last arg.length elements. magic 0 = starts, 1 = ends. */
static JSValue js_array_ext_startsends(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv, int magic)
{
    JSValue obj, other, ret = JS_EXCEPTION;
    int64_t len, olen, k;
    int result = 1;
    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj)) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    other = JS_ToObject(ctx, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (JS_IsException(other)) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    if (js_get_length64(ctx, &olen, other)) goto done;
    if (olen > len) { ret = JS_FALSE; goto done; }
    for (k = 0; k < olen; k++) {
        JSValue a, b;
        int r;
        if (js_array_ext_getel(ctx, obj, magic ? (len - olen + k) : k, &a)) goto done;
        if (js_array_ext_getel(ctx, other, k, &b)) { JS_FreeValue(ctx, a); goto done; }
        r = js_deep_equals(ctx, a, b, 0);
        JS_FreeValue(ctx, a); JS_FreeValue(ctx, b);
        if (r < 0) goto done;
        if (!r) { result = 0; break; }
    }
    ret = JS_NewBool(ctx, result);
 done:
    JS_FreeValue(ctx, other);
    JS_FreeValue(ctx, obj);
    return ret;
}

/* unnest() -> a copy flattened by exactly one level. */
static JSValue js_array_ext_unnest(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    JSValue obj, result, ret = JS_EXCEPTION;
    int64_t j = 0;
    (void)argc; (void)argv;
    obj = JS_ToObject(ctx, this_val);
    result = JS_NewArray(ctx);
    if (JS_IsException(result)) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    if (js_array_ext_flatten_into(ctx, result, obj, 1, &j, 0)) { JS_FreeValue(ctx, result); goto done; }
    ret = result;
 done:
    JS_FreeValue(ctx, obj);
    return ret;
}

/* dropRepeats (magic 0, deep-equals) / dropRepeatsWith(pred) (magic 1,
 * pred(lastKept, current)) / dropRepeatsBy(fn) (magic 2, deep-equals of the
 * mapped values). Keeps the first of every adjacent run. */
static JSValue js_array_ext_droprepeats(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv, int magic)
{
    JSValue obj, result = JS_UNDEFINED, ret = JS_EXCEPTION;
    JSValue prev = JS_UNINITIALIZED, prevkey = JS_UNINITIALIZED;
    JSValueConst arg = argc > 0 ? argv[0] : JS_UNDEFINED;
    int64_t len, i, j = 0;
    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj)) goto done;
    result = JS_NewArray(ctx);
    if (JS_IsException(result)) { result = JS_UNDEFINED; goto done; }
    for (i = 0; i < len; i++) {
        JSValue el, curkey = JS_UNINITIALIZED;
        int same = 0;
        if (js_array_ext_getel(ctx, obj, i, &el)) goto done;
        if (magic == 2) {
            curkey = js_array_ext_mapval(ctx, arg, el);   /* fn/prop/identity */
            if (JS_IsException(curkey)) { JS_FreeValue(ctx, el); goto done; }
        }
        if (JS_VALUE_GET_TAG(prev) != JS_TAG_UNINITIALIZED) {
            if (magic == 0) {
                same = js_deep_equals(ctx, prev, el, 0);
            } else if (magic == 1) {
                JSValueConst ab[2]; JSValue r;
                ab[0] = prev; ab[1] = el;
                r = JS_Call(ctx, arg, JS_UNDEFINED, 2, ab);
                if (JS_IsException(r)) same = -1; else { same = JS_ToBool(ctx, r); JS_FreeValue(ctx, r); }
            } else {
                same = js_deep_equals(ctx, prevkey, curkey, 0);
            }
            if (same < 0) { JS_FreeValue(ctx, el); JS_FreeValue(ctx, curkey); goto done; }
        }
        if (!same) {
            JS_FreeValue(ctx, prev);
            prev = JS_DupValue(ctx, el);
            if (magic == 2) { JS_FreeValue(ctx, prevkey); prevkey = JS_DupValue(ctx, curkey); }
            if (JS_DefinePropertyValueInt64(ctx, result, j++, el, JS_PROP_C_W_E) < 0) { JS_FreeValue(ctx, curkey); goto done; }
        } else {
            JS_FreeValue(ctx, el);
        }
        JS_FreeValue(ctx, curkey);
    }
    ret = result; result = JS_UNDEFINED;
 done:
    JS_FreeValue(ctx, prev);
    JS_FreeValue(ctx, prevkey);
    JS_FreeValue(ctx, result);
    JS_FreeValue(ctx, obj);
    return ret;
}

/* sortWith comparator closure: fd[0] = the array of comparator functions.
 * Walks them in order; the first non-zero result wins. */
static JSValue js_fn_sortwith_cmp(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv, int magic, JSValue *fd)
{
    JSValueConst fns = fd[0];
    JSValueConst a = argc > 0 ? argv[0] : JS_UNDEFINED;
    JSValueConst b = argc > 1 ? argv[1] : JS_UNDEFINED;
    int64_t n, i;
    (void)this_val; (void)magic;
    if (js_get_length64(ctx, &n, fns)) return JS_EXCEPTION;
    for (i = 0; i < n; i++) {
        JSValue f = JS_GetPropertyInt64(ctx, fns, i), r;
        JSValueConst ab[2];
        double d;
        int e;
        if (JS_IsException(f)) return JS_EXCEPTION;
        ab[0] = a; ab[1] = b;
        r = JS_Call(ctx, f, JS_UNDEFINED, 2, ab);
        JS_FreeValue(ctx, f);
        if (JS_IsException(r)) return JS_EXCEPTION;
        e = JS_ToFloat64(ctx, &d, r);
        JS_FreeValue(ctx, r);
        if (e) return JS_EXCEPTION;
        if (d < 0) return JS_NewInt32(ctx, -1);
        if (d > 0) return JS_NewInt32(ctx, 1);
    }
    return JS_NewInt32(ctx, 0);
}

/* sortWith(comparators) -> a stable-sorted copy, comparators tried in order
 * (reuses the engine's toSorted so reentrancy/stability match native sort). */
static JSValue js_array_ext_sortwith(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    JSValueConst fns = argc > 0 ? argv[0] : JS_UNDEFINED;
    JSValue obj, cmp, ret;
    obj = JS_ToObject(ctx, this_val);
    cmp = JS_NewCFunctionData(ctx, js_fn_sortwith_cmp, 2, 0, 1, (JSValueConst *)&fns);
    if (JS_IsException(cmp)) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    ret = js_array_toSorted(ctx, obj, 1, (JSValueConst *)&cmp);
    JS_FreeValue(ctx, cmp);
    JS_FreeValue(ctx, obj);
    return ret;
}

/* _includesWith: pred(x, list[k]) truthy for some k. 1/0/-1. x borrowed. */
static int js_array_includes_with(JSContext *ctx, JSValueConst pred, JSValueConst x,
                                  JSValueConst list, int64_t listLen)
{
    int64_t k;
    for (k = 0; k < listLen; k++) {
        JSValue y, r;
        JSValueConst ab[2];
        int b;
        if (js_array_ext_getel(ctx, list, k, &y)) return -1;
        ab[0] = x; ab[1] = y;
        r = JS_Call(ctx, pred, JS_UNDEFINED, 2, ab);
        JS_FreeValue(ctx, y);
        if (JS_IsException(r)) return -1;
        b = JS_ToBool(ctx, r);
        JS_FreeValue(ctx, r);
        if (b) return 1;
    }
    return 0;
}

/* differenceWith(pred, first, second): first's elements not in second (by
 * pred) and not already emitted (self-deduped). Returns a fresh array. */
static JSValue js_array_diffwith(JSContext *ctx, JSValueConst first, int64_t flen,
                                 JSValueConst second, int64_t slen, JSValueConst pred)
{
    JSValue result = JS_NewArray(ctx);
    int64_t i, w = 0;
    if (JS_IsException(result)) return result;
    for (i = 0; i < flen; i++) {
        JSValue x;
        int in2, out;
        if (js_array_ext_getel(ctx, first, i, &x)) goto fail;
        in2 = js_array_includes_with(ctx, pred, x, second, slen);
        if (in2 < 0) { JS_FreeValue(ctx, x); goto fail; }
        if (in2) { JS_FreeValue(ctx, x); continue; }
        out = js_array_includes_with(ctx, pred, x, result, w);
        if (out < 0) { JS_FreeValue(ctx, x); goto fail; }
        if (out) { JS_FreeValue(ctx, x); continue; }
        if (JS_DefinePropertyValueInt64(ctx, result, w++, x, JS_PROP_C_W_E) < 0) goto fail;
    }
    return result;
 fail:
    JS_FreeValue(ctx, result);
    return JS_EXCEPTION;
}

/* differenceWith(pred, other). */
static JSValue js_array_ext_differencewith(JSContext *ctx, JSValueConst this_val,
                                           int argc, JSValueConst *argv)
{
    JSValueConst pred = argc > 0 ? argv[0] : JS_UNDEFINED;
    JSValue obj, other, ret = JS_EXCEPTION;
    int64_t len, olen;
    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj)) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    other = JS_ToObject(ctx, argc > 1 ? argv[1] : JS_UNDEFINED);
    if (JS_IsException(other)) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    if (js_get_length64(ctx, &olen, other)) goto done;
    ret = js_array_diffwith(ctx, obj, len, other, olen, pred);
 done:
    JS_FreeValue(ctx, other);
    JS_FreeValue(ctx, obj);
    return ret;
}

/* unionWith(pred, other) = uniqWith(pred, this ++ other). */
static JSValue js_array_ext_unionwith(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    JSValueConst pred = argc > 0 ? argv[0] : JS_UNDEFINED;
    JSValue obj, other, result = JS_UNDEFINED, ret = JS_EXCEPTION;
    int64_t len, olen, i, w = 0, pass;
    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj)) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    other = JS_ToObject(ctx, argc > 1 ? argv[1] : JS_UNDEFINED);
    if (JS_IsException(other)) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    if (js_get_length64(ctx, &olen, other)) goto done;
    result = JS_NewArray(ctx);
    if (JS_IsException(result)) { result = JS_UNDEFINED; goto done; }
    for (pass = 0; pass < 2; pass++) {
        JSValueConst src = pass == 0 ? (JSValueConst)obj : (JSValueConst)other;
        int64_t n = pass == 0 ? len : olen;
        for (i = 0; i < n; i++) {
            JSValue x;
            int dup;
            if (js_array_ext_getel(ctx, src, i, &x)) goto done;
            dup = js_array_includes_with(ctx, pred, x, result, w);
            if (dup < 0) { JS_FreeValue(ctx, x); goto done; }
            if (dup) { JS_FreeValue(ctx, x); continue; }
            if (JS_DefinePropertyValueInt64(ctx, result, w++, x, JS_PROP_C_W_E) < 0) goto done;
        }
    }
    ret = result; result = JS_UNDEFINED;
 done:
    JS_FreeValue(ctx, result);
    JS_FreeValue(ctx, other);
    JS_FreeValue(ctx, obj);
    return ret;
}

/* Append `src`'s SameValueZero elements that are absent from `exclude` and not
 * yet emitted (seen) into result at *pw. Value-equality half of symdiff. */
static int js_array_symdiff_half(JSContext *ctx, JSValueConst result, int64_t *pw,
                                 JSValueConst src, int64_t n,
                                 DynValSet *exclude, DynValSet *seen)
{
    int64_t i;
    for (i = 0; i < n; i++) {
        JSValue el;
        int added;
        if (js_array_ext_getel(ctx, src, i, &el)) return -1;
        if (dyn_valset_has(ctx, exclude, el)) { JS_FreeValue(ctx, el); continue; }
        added = dyn_valset_add(ctx, seen, el);
        if (added < 0) { JS_FreeValue(ctx, el); return -1; }
        if (!added) { JS_FreeValue(ctx, el); continue; }
        if (JS_DefinePropertyValueInt64(ctx, result, (*pw)++, el, JS_PROP_C_W_E) < 0) return -1;
    }
    return 0;
}

/* symmetricDifference(other) (magic 0, SameValueZero) /
 * symmetricDifferenceWith(pred, other) (magic 1). = diff(this,other) ++
 * diff(other,this). The value form uses DynValSet (consistent with our
 * SameValueZero `difference`); the pred form uses js_array_diffwith. */
static JSValue js_array_ext_symdiff(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv, int magic)
{
    JSValue obj, other, result = JS_UNDEFINED, ret = JS_EXCEPTION;
    JSValueConst pred = magic ? (argc > 0 ? argv[0] : JS_UNDEFINED) : JS_UNDEFINED;
    int64_t len, olen, w = 0;
    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj)) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    other = JS_ToObject(ctx, argv && argc > (magic ? 1 : 0) ? argv[magic ? 1 : 0] : JS_UNDEFINED);
    if (JS_IsException(other)) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    if (js_get_length64(ctx, &olen, other)) goto done;

    if (magic) {                                   /* predicate form */
        JSValue a = js_array_diffwith(ctx, obj, len, other, olen, pred), b;
        int64_t bn, k;
        if (JS_IsException(a)) goto done;
        b = js_array_diffwith(ctx, other, olen, obj, len, pred);
        if (JS_IsException(b)) { JS_FreeValue(ctx, a); goto done; }
        /* a already owns its slots; append b's elements onto a */
        if (js_get_length64(ctx, &bn, b)) { JS_FreeValue(ctx, a); JS_FreeValue(ctx, b); goto done; }
        (void)js_get_length64(ctx, &w, a);
        for (k = 0; k < bn; k++) {
            JSValue el = JS_GetPropertyInt64(ctx, b, k);
            if (JS_IsException(el)) { JS_FreeValue(ctx, a); JS_FreeValue(ctx, b); goto done; }
            if (JS_DefinePropertyValueInt64(ctx, a, w++, el, JS_PROP_C_W_E) < 0) { JS_FreeValue(ctx, a); JS_FreeValue(ctx, b); goto done; }
        }
        JS_FreeValue(ctx, b);
        ret = a;
        goto done;
    }
    {   /* value form */
        DynValSet setThis, setOther, seenA, seenB;
        int64_t i;
        result = JS_NewArray(ctx);
        if (JS_IsException(result)) { result = JS_UNDEFINED; goto done; }
        if (dyn_valset_init(ctx, &setOther, olen)) { JS_ThrowOutOfMemory(ctx); goto done; }
        if (dyn_valset_init(ctx, &setThis, len)) { JS_ThrowOutOfMemory(ctx); dyn_valset_free(ctx, &setOther); goto done; }
        if (dyn_valset_init(ctx, &seenA, len)) { JS_ThrowOutOfMemory(ctx); dyn_valset_free(ctx, &setOther); dyn_valset_free(ctx, &setThis); goto done; }
        if (dyn_valset_init(ctx, &seenB, olen)) { JS_ThrowOutOfMemory(ctx); dyn_valset_free(ctx, &setOther); dyn_valset_free(ctx, &setThis); dyn_valset_free(ctx, &seenA); goto done; }
        for (i = 0; i < olen; i++) { JSValue e; int r; if (js_array_ext_getel(ctx, other, i, &e)) goto vfail; r = dyn_valset_add(ctx, &setOther, e); JS_FreeValue(ctx, e); if (r < 0) goto vfail; }
        for (i = 0; i < len;  i++) { JSValue e; int r; if (js_array_ext_getel(ctx, obj, i, &e))   goto vfail; r = dyn_valset_add(ctx, &setThis,  e); JS_FreeValue(ctx, e); if (r < 0) goto vfail; }
        if (js_array_symdiff_half(ctx, result, &w, obj, len, &setOther, &seenA)) goto vfail;
        if (js_array_symdiff_half(ctx, result, &w, other, olen, &setThis, &seenB)) goto vfail;
        ret = result; result = JS_UNDEFINED;
    vfail:
        dyn_valset_free(ctx, &setOther); dyn_valset_free(ctx, &setThis);
        dyn_valset_free(ctx, &seenA); dyn_valset_free(ctx, &seenB);
    }
 done:
    JS_FreeValue(ctx, result);
    JS_FreeValue(ctx, other);
    JS_FreeValue(ctx, obj);
    return ret;
}

/* Shallow clone: arrays -> top-level element copy, plain objects -> own
 * enumerable data props copied, else the value. */
static JSValue js_ext_shallow_clone(JSContext *ctx, JSValueConst v)
{
    if (JS_IsArray(ctx, v)) {
        int64_t n;
        if (js_get_length64(ctx, &n, v)) return JS_EXCEPTION;
        return js_array_ext_build_range(ctx, v, 0, n);
    }
    if (JS_VALUE_GET_TAG(v) == JS_TAG_OBJECT) {
        JSValue o = JS_NewObject(ctx);
        if (JS_IsException(o)) return o;
        if (JS_CopyDataProperties(ctx, o, v, JS_UNDEFINED, TRUE)) { JS_FreeValue(ctx, o); return JS_EXCEPTION; }
        return o;
    }
    return JS_DupValue(ctx, v);
}

/* reduceBy(valueFn, acc, keyFn) -> object grouping by keyFn(el), each group
 * reduced by valueFn(groupAcc, el) starting from a shallow clone of acc
 * A valueFn returning a reduced wrapper halts the reduction. */
static JSValue js_array_ext_reduceby(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    JSValueConst valueFn = argc > 0 ? argv[0] : JS_UNDEFINED;
    JSValueConst acc     = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSValueConst keyFn   = argc > 2 ? argv[2] : JS_UNDEFINED;
    JSValue obj, result = JS_UNDEFINED, ret = JS_EXCEPTION;
    int64_t len, i;
    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj)) goto done;
    result = JS_NewObject(ctx);
    if (JS_IsException(result)) { result = JS_UNDEFINED; goto done; }
    for (i = 0; i < len; i++) {
        JSValue el, keyv, cur, val;
        JSValueConst va[2];
        JSAtom atom;
        int has;
        if (js_array_ext_getel(ctx, obj, i, &el)) goto done;
        keyv = JS_Call(ctx, keyFn, JS_UNDEFINED, 1, (JSValueConst *)&el);
        if (JS_IsException(keyv)) { JS_FreeValue(ctx, el); goto done; }
        atom = JS_ValueToAtom(ctx, keyv);
        JS_FreeValue(ctx, keyv);
        if (atom == JS_ATOM_NULL) { JS_FreeValue(ctx, el); goto done; }
        has = JS_HasProperty(ctx, result, atom);
        if (has < 0) { JS_FreeAtom(ctx, atom); JS_FreeValue(ctx, el); goto done; }
        if (has) {
            cur = JS_GetProperty(ctx, result, atom);
            if (JS_IsException(cur)) { JS_FreeAtom(ctx, atom); JS_FreeValue(ctx, el); goto done; }
        } else {
            cur = js_ext_shallow_clone(ctx, acc);
            if (JS_IsException(cur)) { JS_FreeAtom(ctx, atom); JS_FreeValue(ctx, el); goto done; }
        }
        va[0] = cur; va[1] = el;
        val = JS_Call(ctx, valueFn, JS_UNDEFINED, 2, va);
        JS_FreeValue(ctx, cur);
        JS_FreeValue(ctx, el);
        if (JS_IsException(val)) { JS_FreeAtom(ctx, atom); goto done; }
        if (JS_VALUE_GET_TAG(val) == JS_TAG_OBJECT) {   /* reduced short-circuit */
            JSValue rd = JS_GetPropertyStr(ctx, val, "@@transducer/reduced");
            int stop = !JS_IsException(rd) && JS_ToBool(ctx, rd);
            JS_FreeValue(ctx, rd);
            if (stop) { JS_FreeValue(ctx, val); JS_FreeAtom(ctx, atom); break; }
        }
        if (JS_DefinePropertyValue(ctx, result, atom, val, JS_PROP_C_W_E) < 0) goto done;
    }
    ret = result; result = JS_UNDEFINED;
 done:
    JS_FreeValue(ctx, result);
    JS_FreeValue(ctx, obj);
    return ret;
}

/* Array.repeat(value, n) -> [value, value, ...] (n copies, same reference). */
static JSValue js_array_static_repeat(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    JSValueConst value = argc > 0 ? argv[0] : JS_UNDEFINED;
    JSValue arr, *pval;
    JSObject *p;
    int64_t n, i;
    (void)this_val;
    if (JS_ToInt64Sat(ctx, &n, argc > 1 ? argv[1] : JS_UNDEFINED)) return JS_EXCEPTION;
    if (n < 0) return JS_ThrowRangeError(ctx, "repeat: count must be non-negative");
    if (n > ARR_EXT_MAX) return JS_ThrowRangeError(ctx, "repeat: count too large");
    if (n == 0) return JS_NewArray(ctx);
    arr = js_allocate_fast_array(ctx, n);
    if (JS_IsException(arr)) return arr;
    p = JS_VALUE_GET_OBJ(arr);
    pval = p->u.array.u.values;
    for (i = 0; i < n; i++)
        pval[i] = JS_DupValue(ctx, value);
    return arr;
}

/* ============================================================================
 * Transducers (transduce/into) + applicative sequence/traverse. The transformer
 * protocol is the community one: string keys "@@transducer/init|step|result"
 * and the reduced wrapper {"@@transducer/reduced":true,"@@transducer/value":x}.
 * ========================================================================== */

/* One transformer object's step. fd[0]=kind (0 array-push, 1 string-concat,
 * 2 object-assign, 3 wrap a plain (acc,x) reducer fd[1]). Returns the new acc. */
static JSValue js_xf_step(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv, int magic, JSValue *fd)
{
    int kind = JS_VALUE_GET_INT(fd[0]);
    JSValueConst acc = argc > 0 ? argv[0] : JS_UNDEFINED;
    JSValueConst x   = argc > 1 ? argv[1] : JS_UNDEFINED;
    (void)this_val; (void)magic;
    switch (kind) {
    case 0: {   /* array push */
        int64_t n;
        if (js_get_length64(ctx, &n, acc)) return JS_EXCEPTION;
        if (JS_SetPropertyInt64(ctx, acc, n, JS_DupValue(ctx, x)) < 0) return JS_EXCEPTION;
        return JS_DupValue(ctx, acc);
    }
    case 1: {   /* string concat */
        JSValue sa = JS_ToString(ctx, acc), sx;
        if (JS_IsException(sa)) return sa;
        sx = JS_ToString(ctx, x);
        if (JS_IsException(sx)) { JS_FreeValue(ctx, sa); return sx; }
        return JS_ConcatString(ctx, sa, sx);   /* consumes sa, sx */
    }
    case 2: {   /* object assign: {k:v} pair-array or a source object */
        if (JS_IsArray(ctx, x)) {
            JSValue k = JS_GetPropertyInt64(ctx, x, 0), v;
            JSAtom atom;
            if (JS_IsException(k)) return JS_EXCEPTION;
            v = JS_GetPropertyInt64(ctx, x, 1);
            if (JS_IsException(v)) { JS_FreeValue(ctx, k); return JS_EXCEPTION; }
            atom = JS_ValueToAtom(ctx, k);
            JS_FreeValue(ctx, k);
            if (atom == JS_ATOM_NULL) { JS_FreeValue(ctx, v); return JS_EXCEPTION; }
            if (JS_SetProperty(ctx, acc, atom, v) < 0) { JS_FreeAtom(ctx, atom); return JS_EXCEPTION; }
            JS_FreeAtom(ctx, atom);
        } else if (JS_CopyDataProperties(ctx, acc, x, JS_UNDEFINED, TRUE)) {
            return JS_EXCEPTION;
        }
        return JS_DupValue(ctx, acc);
    }
    default: {  /* wrap: fd[1](acc, x) */
        JSValueConst a2[2];
        a2[0] = acc; a2[1] = x;
        return JS_Call(ctx, fd[1], JS_UNDEFINED, 2, a2);
    }
    }
}

/* One transformer object's init: a fresh empty container by kind. */
static JSValue js_xf_init(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv, int magic, JSValue *fd)
{
    int kind = JS_VALUE_GET_INT(fd[0]);
    (void)this_val; (void)argc; (void)argv; (void)magic;
    switch (kind) {
    case 0:  return JS_NewArray(ctx);
    case 1:  return JS_NewString(ctx, "");
    case 2:  return JS_NewObject(ctx);
    default: return JS_ThrowTypeError(ctx, "init not implemented on wrapped transformer");
    }
}

/* result: identity (the catenating transformers return the accumulator as-is). */
static JSValue js_xf_result(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv, int magic, JSValue *fd)
{
    (void)this_val; (void)magic; (void)fd;
    return JS_DupValue(ctx, argc > 0 ? argv[0] : JS_UNDEFINED);
}

/* Build a base transformer object exposing the three protocol methods. */
static JSValue js_make_step_transformer(JSContext *ctx, int kind, JSValueConst fn)
{
    JSValue obj, kv, step, init, result;
    JSValueConst sd[2];
    obj = JS_NewObject(ctx);
    if (JS_IsException(obj)) return obj;
    kv = JS_NewInt32(ctx, kind);
    sd[0] = kv; sd[1] = fn;
    step   = JS_NewCFunctionData(ctx, js_xf_step, 2, 0, 2, sd);
    init   = JS_NewCFunctionData(ctx, js_xf_init, 0, 0, 1, (JSValueConst *)&kv);
    result = JS_NewCFunctionData(ctx, js_xf_result, 1, 0, 1, (JSValueConst *)&kv);
    JS_FreeValue(ctx, kv);
    if (JS_IsException(step) || JS_IsException(init) || JS_IsException(result)) {
        JS_FreeValue(ctx, step); JS_FreeValue(ctx, init); JS_FreeValue(ctx, result);
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    if (JS_SetPropertyStr(ctx, obj, "@@transducer/step", step) < 0 ||
        JS_SetPropertyStr(ctx, obj, "@@transducer/init", init) < 0 ||
        JS_SetPropertyStr(ctx, obj, "@@transducer/result", result) < 0) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    return obj;
}

/* Drive a composed transformer over the list: step each element, unwrap+break
 * on a reduced result, then always call result. `acc` is
 * owned and consumed. Returns an owned result or JS_EXCEPTION. */
static JSValue js_x_array_reduce(JSContext *ctx, JSValueConst xf, JSValue acc,
                                 JSValueConst list, int64_t len)
{
    JSValue step, resfn, r;
    int64_t i;
    step = JS_GetPropertyStr(ctx, xf, "@@transducer/step");
    if (JS_IsException(step)) { JS_FreeValue(ctx, acc); return JS_EXCEPTION; }
    for (i = 0; i < len; i++) {
        JSValue el, nacc;
        JSValueConst a2[2];
        if (js_array_ext_getel(ctx, list, i, &el)) { JS_FreeValue(ctx, step); JS_FreeValue(ctx, acc); return JS_EXCEPTION; }
        a2[0] = acc; a2[1] = el;
        nacc = JS_Call(ctx, step, xf, 2, a2);
        JS_FreeValue(ctx, el);
        JS_FreeValue(ctx, acc);
        if (JS_IsException(nacc)) { JS_FreeValue(ctx, step); return JS_EXCEPTION; }
        acc = nacc;
        if (JS_VALUE_GET_TAG(acc) == JS_TAG_OBJECT) {
            JSValue rd = JS_GetPropertyStr(ctx, acc, "@@transducer/reduced");
            int stop = !JS_IsException(rd) && JS_ToBool(ctx, rd);
            JS_FreeValue(ctx, rd);
            if (stop) {
                JSValue v = JS_GetPropertyStr(ctx, acc, "@@transducer/value");
                JS_FreeValue(ctx, acc);
                if (JS_IsException(v)) { JS_FreeValue(ctx, step); return JS_EXCEPTION; }
                acc = v;
                break;
            }
        }
    }
    JS_FreeValue(ctx, step);
    resfn = JS_GetPropertyStr(ctx, xf, "@@transducer/result");
    if (JS_IsException(resfn)) { JS_FreeValue(ctx, acc); return JS_EXCEPTION; }
    r = JS_Call(ctx, resfn, xf, 1, (JSValueConst *)&acc);
    JS_FreeValue(ctx, resfn);
    JS_FreeValue(ctx, acc);
    return r;
}

/* transduce(xf, fn, acc) -> reduce `this` through the transducer, seeded with
 * `acc`. `fn` may be a 2-arity reducer or a transformer object. */
static JSValue js_array_ext_transduce(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    JSValueConst xf = argc > 0 ? argv[0] : JS_UNDEFINED;
    JSValueConst fn = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSValueConst accArg = argc > 2 ? argv[2] : JS_UNDEFINED;
    JSValue obj, base, composed, ret = JS_EXCEPTION;
    int64_t len;
    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj)) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    base = JS_IsFunction(ctx, fn) ? js_make_step_transformer(ctx, 3, fn)
                                  : JS_DupValue(ctx, fn);
    if (JS_IsException(base)) goto done;
    composed = JS_Call(ctx, xf, JS_UNDEFINED, 1, (JSValueConst *)&base);
    JS_FreeValue(ctx, base);
    if (JS_IsException(composed)) goto done;
    ret = js_x_array_reduce(ctx, composed, JS_DupValue(ctx, accArg), obj, len);
    JS_FreeValue(ctx, composed);
 done:
    JS_FreeValue(ctx, obj);
    return ret;
}

/* into(acc, xf) -> transduce `this` into a fresh container chosen by `acc`'s
 * type (array/string/object) or `acc` itself if it is a transformer. */
static JSValue js_array_ext_into(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    JSValueConst accArg = argc > 0 ? argv[0] : JS_UNDEFINED;
    JSValueConst xf     = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSValue obj, base, composed, seed, initfn, ret = JS_EXCEPTION;
    int64_t len;
    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj)) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    {   /* _isTransformer(accArg)? */
        JSValue stepm = JS_GetPropertyStr(ctx, accArg, "@@transducer/step");
        int isxf = JS_IsFunction(ctx, stepm);
        JS_FreeValue(ctx, stepm);
        if (isxf) {
            base = JS_DupValue(ctx, accArg);
        } else if (JS_IsArray(ctx, accArg)) {
            base = js_make_step_transformer(ctx, 0, JS_UNDEFINED);
        } else if (JS_VALUE_GET_TAG(accArg) == JS_TAG_STRING) {
            base = js_make_step_transformer(ctx, 1, JS_UNDEFINED);
        } else if (JS_VALUE_GET_TAG(accArg) == JS_TAG_OBJECT) {
            base = js_make_step_transformer(ctx, 2, JS_UNDEFINED);
        } else {
            JS_ThrowTypeError(ctx, "into: cannot create a transformer for this accumulator");
            goto done;
        }
    }
    if (JS_IsException(base)) goto done;
    composed = JS_Call(ctx, xf, JS_UNDEFINED, 1, (JSValueConst *)&base);
    JS_FreeValue(ctx, base);
    if (JS_IsException(composed)) goto done;
    initfn = JS_GetPropertyStr(ctx, composed, "@@transducer/init");
    if (JS_IsException(initfn)) { JS_FreeValue(ctx, composed); goto done; }
    seed = JS_Call(ctx, initfn, composed, 0, NULL);
    JS_FreeValue(ctx, initfn);
    if (JS_IsException(seed)) { JS_FreeValue(ctx, composed); goto done; }
    ret = js_x_array_reduce(ctx, composed, seed, obj, len);
    JS_FreeValue(ctx, composed);
 done:
    JS_FreeValue(ctx, obj);
    return ret;
}

/* Extract the applicative `of` from a TypeRep F (fantasy-land/of | of | F). */
static JSValue js_applicative_of(JSContext *ctx, JSValueConst F)
{
    JSValue of = JS_GetPropertyStr(ctx, F, "fantasy-land/of");
    if (JS_IsFunction(ctx, of)) return of;
    JS_FreeValue(ctx, of);
    of = JS_GetPropertyStr(ctx, F, "of");
    if (JS_IsFunction(ctx, of)) return of;
    JS_FreeValue(ctx, of);
    return JS_DupValue(ctx, F);
}

/* One reduceRight fold step of the default (list-applicative) sequence:
 * ap(map(prepend, x), acc) => [ [xi, ...a] for xi in x for a in acc ]. */
static JSValue js_seq_fold(JSContext *ctx, JSValueConst x, JSValueConst acc)
{
    JSValue out;
    int64_t xn, an, xi, ai, w = 0;
    if (js_get_length64(ctx, &xn, x)) return JS_EXCEPTION;
    if (js_get_length64(ctx, &an, acc)) return JS_EXCEPTION;
    out = JS_NewArray(ctx);
    if (JS_IsException(out)) return out;
    for (xi = 0; xi < xn; xi++) {
        JSValue xv;
        if (js_array_ext_getel(ctx, x, xi, &xv)) { JS_FreeValue(ctx, out); return JS_EXCEPTION; }
        for (ai = 0; ai < an; ai++) {
            JSValue a, row;
            int64_t al, k;
            if (js_array_ext_getel(ctx, acc, ai, &a)) { JS_FreeValue(ctx, xv); JS_FreeValue(ctx, out); return JS_EXCEPTION; }
            if (js_get_length64(ctx, &al, a)) { JS_FreeValue(ctx, a); JS_FreeValue(ctx, xv); JS_FreeValue(ctx, out); return JS_EXCEPTION; }
            row = js_allocate_fast_array(ctx, al + 1);
            if (JS_IsException(row)) { JS_FreeValue(ctx, a); JS_FreeValue(ctx, xv); JS_FreeValue(ctx, out); return JS_EXCEPTION; }
            {
                JSObject *rp = JS_VALUE_GET_OBJ(row);
                JSValue *dst = rp->u.array.u.values;
                dst[0] = JS_DupValue(ctx, xv);
                for (k = 0; k < al; k++)
                    if (js_array_ext_getel(ctx, a, k, &dst[1 + k])) { JS_FreeValue(ctx, a); JS_FreeValue(ctx, xv); JS_FreeValue(ctx, row); JS_FreeValue(ctx, out); return JS_EXCEPTION; }
            }
            JS_FreeValue(ctx, a);
            if (JS_DefinePropertyValueInt64(ctx, out, w++, row, JS_PROP_C_W_E) < 0) { JS_FreeValue(ctx, xv); JS_FreeValue(ctx, out); return JS_EXCEPTION; }
        }
        JS_FreeValue(ctx, xv);
    }
    return out;
}

/* ---- the generic applicative path ------------------------------------------
 *
 * The fold above is the LIST applicative written out: `ap(map(prepend, x), acc)`
 * with both operations inlined because for an Array they are two nested loops.
 * For any other applicative the same expression has to go through the type's
 * own `map` and `ap`, which is all these three helpers do.
 *
 * Two calling conventions exist and both are accepted, because a value can only
 * carry one of them: fantasy-land puts `ap` on the VALUE (`xs.ap(fs)`) and the
 * plain `ap` spelling puts it on the FUNCTIONS (`fs.ap(xs)`). Trying the
 * fantasy-land name first makes a type that implements both unambiguous. */

static JSValue js_applicative_method(JSContext *ctx, JSValueConst v,
                                     const char *fl, const char *plain)
{
    JSValue m;
    if (!JS_IsObject(v))
        return JS_UNDEFINED;
    m = JS_GetPropertyStr(ctx, v, fl);
    if (JS_IsException(m))
        return m;
    if (JS_IsFunction(ctx, m))
        return m;
    JS_FreeValue(ctx, m);
    if (!plain)
        return JS_UNDEFINED;
    m = JS_GetPropertyStr(ctx, v, plain);
    if (JS_IsException(m))
        return m;
    if (JS_IsFunction(ctx, m))
        return m;
    JS_FreeValue(ctx, m);
    return JS_UNDEFINED;
}

/* map(fn, container) through the container's own map. */
static JSValue js_applicative_map(JSContext *ctx, JSValueConst container,
                                  JSValueConst fn)
{
    JSValue m = js_applicative_method(ctx, container, "fantasy-land/map", "map");
    JSValue r;
    if (JS_IsException(m))
        return m;
    if (JS_IsUndefined(m))
        return JS_ThrowTypeError(ctx, "sequence: an element has no map method");
    r = JS_Call(ctx, m, container, 1, &fn);
    JS_FreeValue(ctx, m);
    return r;
}

/* ap(fs, xs) where fs holds functions and xs holds values. */
static JSValue js_applicative_ap(JSContext *ctx, JSValueConst fs,
                                 JSValueConst xs)
{
    JSValue m, r;
    /* The value side is asked for the fantasy-land name ONLY: a plain `ap` on
     * a value means the other convention, where the receiver holds functions,
     * and calling it here would apply the wrong side. */
    m = js_applicative_method(ctx, xs, "fantasy-land/ap", NULL);
    if (JS_IsException(m))
        return m;
    if (!JS_IsUndefined(m)) {
        r = JS_Call(ctx, m, xs, 1, &fs);
        JS_FreeValue(ctx, m);
        return r;
    }
    m = js_applicative_method(ctx, fs, "fantasy-land/ap", "ap");
    if (JS_IsException(m))
        return m;
    if (!JS_IsUndefined(m)) {
        r = JS_Call(ctx, m, fs, 1, &xs);
        JS_FreeValue(ctx, m);
        return r;
    }
    return JS_ThrowTypeError(ctx,
        "sequence: the applicative has no ap method (fantasy-land/ap on the "
        "value, or ap on the functions)");
}

/* The inner half of the curried prepend: a => [x, ...a]. */
static JSValue js_seq_prepend_inner(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv, int magic,
                                    JSValue *func_data)
{
    JSValue out;
    int64_t n, k;
    (void)this_val; (void)magic;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "sequence: ap called with no argument");
    if (js_get_length64(ctx, &n, argv[0]))
        return JS_EXCEPTION;
    out = JS_NewArray(ctx);
    if (JS_IsException(out))
        return out;
    if (JS_DefinePropertyValueInt64(ctx, out, 0,
                                    JS_DupValue(ctx, func_data[0]),
                                    JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, out);
        return JS_EXCEPTION;
    }
    for (k = 0; k < n; k++) {
        JSValue el;
        if (js_array_ext_getel(ctx, argv[0], k, &el)) {
            JS_FreeValue(ctx, out);
            return JS_EXCEPTION;
        }
        if (JS_DefinePropertyValueInt64(ctx, out, k + 1, el, JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, out);
            return JS_EXCEPTION;
        }
    }
    return out;
}

/* x => a => [x, ...a] */
static JSValue js_seq_prepend_outer(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv, int magic,
                                    JSValue *func_data)
{
    JSValueConst x = argc > 0 ? argv[0] : JS_UNDEFINED;
    (void)this_val; (void)magic; (void)func_data;
    return JS_NewCFunctionData(ctx, js_seq_prepend_inner, 1, 0, 1, &x);
}

/* One fold step for a non-list applicative: ap(map(prepend, x), acc). */
static JSValue js_seq_fold_generic(JSContext *ctx, JSValueConst x,
                                   JSValueConst acc, JSValueConst prepend)
{
    JSValue fs, r;
    fs = js_applicative_map(ctx, x, prepend);
    if (JS_IsException(fs))
        return fs;
    r = js_applicative_ap(ctx, fs, acc);
    JS_FreeValue(ctx, fs);
    return r;
}

/* sequence: reduceRight(fold, of([]), list).
 *
 * The list applicative gets the specialised fold -- for an Array, map and ap
 * are two nested loops and going through the methods would allocate a closure
 * per element to express what the loop already says. Anything else goes
 * through its own map/ap. The two are told apart ONCE, by whether the empty
 * accumulator `of([])` carries an ap method, so a plain Array (which has
 * neither name) takes exactly the path it always did. */
static JSValue js_array_sequence_core(JSContext *ctx, JSValueConst F,
                                      JSValueConst listObj, int64_t len)
{
    JSValue of, empty, acc, probe, prepend = JS_UNDEFINED, ret = JS_EXCEPTION;
    int64_t i;
    int generic;

    of = js_applicative_of(ctx, F);
    if (JS_IsException(of)) return JS_EXCEPTION;
    empty = JS_NewArray(ctx);
    if (JS_IsException(empty)) { JS_FreeValue(ctx, of); return JS_EXCEPTION; }
    acc = JS_Call(ctx, of, JS_UNDEFINED, 1, (JSValueConst *)&empty);
    JS_FreeValue(ctx, empty);
    JS_FreeValue(ctx, of);
    if (JS_IsException(acc)) return JS_EXCEPTION;

    probe = js_applicative_method(ctx, acc, "fantasy-land/ap", "ap");
    if (JS_IsException(probe)) { JS_FreeValue(ctx, acc); return JS_EXCEPTION; }
    generic = !JS_IsUndefined(probe);
    JS_FreeValue(ctx, probe);
    /* No ap and not a list is neither applicative this understands. Falling
     * through to the list fold there would read a length off an object that
     * has none and quietly answer [] -- a wrong answer that looks like a
     * right one. */
    if (!generic && !JS_IsArray(ctx, acc)) {
        JS_FreeValue(ctx, acc);
        return JS_ThrowTypeError(ctx,
            "sequence: the applicative is neither Array nor a type with an ap "
            "method");
    }
    if (generic) {
        prepend = JS_NewCFunctionData(ctx, js_seq_prepend_outer, 1, 0, 0, NULL);
        if (JS_IsException(prepend)) { JS_FreeValue(ctx, acc); return JS_EXCEPTION; }
    }

    for (i = len - 1; i >= 0; i--) {
        JSValue x, nacc;
        if (js_array_ext_getel(ctx, listObj, i, &x)) goto done;
        nacc = generic ? js_seq_fold_generic(ctx, x, acc, prepend)
                       : js_seq_fold(ctx, x, acc);
        JS_FreeValue(ctx, x);
        if (JS_IsException(nacc)) goto done;
        JS_FreeValue(ctx, acc);
        acc = nacc;
    }
    ret = acc;
    acc = JS_UNDEFINED;
 done:
    JS_FreeValue(ctx, acc);
    JS_FreeValue(ctx, prepend);
    return ret;
}

/* sequence(F) -> transpose an Array-of-applicatives into an applicative-of-Array
 * (default list path only). */
static JSValue js_array_ext_sequence(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    JSValueConst F = argc > 0 ? argv[0] : JS_UNDEFINED;
    JSValue obj, ret;
    int64_t len;
    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj)) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    ret = js_array_sequence_core(ctx, F, obj, len);
    JS_FreeValue(ctx, obj);
    return ret;
}

/* traverse(F, fn) = sequence(F, map(fn, this)) (default list path only). */
static JSValue js_array_ext_traverse(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    JSValueConst F  = argc > 0 ? argv[0] : JS_UNDEFINED;
    JSValueConst fn = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSValue obj, mapped, ret = JS_EXCEPTION;
    int64_t len, i;
    obj = JS_ToObject(ctx, this_val);
    if (js_get_length64(ctx, &len, obj)) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    mapped = JS_NewArray(ctx);
    if (JS_IsException(mapped)) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    for (i = 0; i < len; i++) {
        JSValue el, r;
        if (js_array_ext_getel(ctx, obj, i, &el)) goto done;
        r = JS_Call(ctx, fn, JS_UNDEFINED, 1, (JSValueConst *)&el);
        JS_FreeValue(ctx, el);
        if (JS_IsException(r)) goto done;
        if (JS_DefinePropertyValueInt64(ctx, mapped, i, r, JS_PROP_C_W_E) < 0) goto done;
    }
    ret = js_array_sequence_core(ctx, F, mapped, len);
 done:
    JS_FreeValue(ctx, mapped);
    JS_FreeValue(ctx, obj);
    return ret;
}

static const JSCFunctionListEntry js_array_static_ext_funcs[] = {
    JS_CFUNC_DEF("repeat", 2, js_array_static_repeat ),
};

static const JSCFunctionListEntry js_array_ext_funcs[] = {
    JS_CFUNC_DEF("isEmpty", 0, js_array_ext_isEmpty ),
    JS_CFUNC_DEF("first", 0, js_array_ext_first ),
    JS_CFUNC_DEF("last", 0, js_array_ext_last ),
    JS_CFUNC_MAGIC_DEF("sum", 0, js_array_ext_sum_avg, 0 ),
    JS_CFUNC_MAGIC_DEF("average", 0, js_array_ext_sum_avg, 1 ),
    JS_ALIAS_DEF("mean", "average" ),
    JS_CFUNC_DEF("compact", 0, js_array_ext_compact ),
    JS_CFUNC_DEF("count", 0, js_array_ext_count ),
    JS_CFUNC_MAGIC_DEF("none", 1, js_array_ext_quantify, 0 ),
    JS_CFUNC_MAGIC_DEF("any", 1, js_array_ext_quantify, 1 ),
    JS_CFUNC_MAGIC_DEF("all", 1, js_array_ext_quantify, 2 ),
    JS_CFUNC_MAGIC_DEF("min", 0, js_array_ext_minmax, 0 ),
    JS_CFUNC_MAGIC_DEF("max", 0, js_array_ext_minmax, 1 ),
    JS_CFUNC_MAGIC_DEF("take", 1, js_array_ext_take, 0 ),
    JS_CFUNC_MAGIC_DEF("drop", 1, js_array_ext_take, 1 ),
    JS_CFUNC_MAGIC_DEF("takeLast", 1, js_array_ext_take, 2 ),
    JS_CFUNC_MAGIC_DEF("dropLast", 1, js_array_ext_take, 3 ),
    JS_CFUNC_DEF("sortBy", 1, js_array_ext_sortby ),
    JS_CFUNC_DEF("sortedIndexOf", 1, js_array_ext_sorted_index_of ),
    JS_CFUNC_DEF("groupBy", 1, js_array_ext_groupby ),
    JS_CFUNC_DEF("shuffle", 0, js_array_ext_shuffle ),
    JS_CFUNC_DEF("sample", 0, js_array_ext_sample ),
    JS_CFUNC_DEF("unique", 0, js_array_ext_unique ),
    JS_ALIAS_DEF("uniq", "unique" ),
    JS_CFUNC_DEF("uniqBy", 1, js_array_ext_unique ),
    JS_CFUNC_MAGIC_DEF("intersect", 1, js_array_ext_setop, 0 ),
    JS_ALIAS_DEF("intersection", "intersect" ),
    JS_CFUNC_MAGIC_DEF("difference", 1, js_array_ext_setop, 1 ),
    JS_CFUNC_MAGIC_DEF("without", 1, js_array_ext_setop, 2 ),
    JS_CFUNC_DEF("union", 1, js_array_ext_union ),
    JS_CFUNC_DEF("partition", 1, js_array_ext_partition ),
    JS_CFUNC_DEF("pluck", 1, js_array_ext_pluck ),
    JS_CFUNC_DEF("zip", 1, js_array_ext_zip ),
    JS_CFUNC_DEF("zipWith", 2, js_array_ext_zipwith ),
    JS_CFUNC_DEF("intersperse", 1, js_array_ext_intersperse ),
    JS_CFUNC_DEF("flatten", 0, js_array_ext_flatten ),
    JS_CFUNC_DEF("transpose", 0, js_array_ext_transpose ),
    JS_CFUNC_DEF("xprod", 1, js_array_ext_xprod ),
    JS_CFUNC_DEF("aperture", 1, js_array_ext_aperture ),
    JS_CFUNC_DEF("splitEvery", 1, js_array_ext_splitevery ),
    JS_CFUNC_DEF("splitAt", 1, js_array_ext_splitat ),
    JS_CFUNC_DEF("adjust", 2, js_array_ext_adjust ),
    JS_CFUNC_DEF("update", 2, js_array_ext_update ),
    JS_CFUNC_DEF("move", 2, js_array_ext_move ),
    JS_CFUNC_DEF("swap", 2, js_array_ext_swap ),
    JS_CFUNC_DEF("nth", 1, js_array_ext_nth ),
    JS_CFUNC_DEF("init", 0, js_array_ext_init ),
    JS_CFUNC_DEF("tail", 0, js_array_ext_tail ),
    JS_ALIAS_DEF("head", "first" ),
    JS_CFUNC_MAGIC_DEF("takeWhile", 1, js_array_ext_whilst, 0 ),
    JS_CFUNC_MAGIC_DEF("dropWhile", 1, js_array_ext_whilst, 1 ),
    JS_CFUNC_MAGIC_DEF("takeLastWhile", 1, js_array_ext_whilst, 2 ),
    JS_CFUNC_MAGIC_DEF("dropLastWhile", 1, js_array_ext_whilst, 3 ),
    JS_CFUNC_DEF("append", 1, js_array_ext_append ),
    JS_CFUNC_DEF("prepend", 1, js_array_ext_prepend ),
    JS_CFUNC_DEF("reject", 1, js_array_ext_reject ),
    JS_CFUNC_DEF("insert", 2, js_array_ext_insert ),
    JS_CFUNC_DEF("insertAll", 2, js_array_ext_insertall ),
    JS_CFUNC_DEF("removeAt", 1, js_array_ext_removeat ),
    JS_CFUNC_DEF("zipObj", 1, js_array_ext_zipobj ),
    JS_CFUNC_DEF("fromPairs", 0, js_array_ext_frompairs ),
    JS_CFUNC_DEF("median", 0, js_array_ext_median ),
    JS_CFUNC_DEF("product", 0, js_array_ext_product ),
    JS_CFUNC_DEF("scan", 2, js_array_ext_scan ),
    JS_CFUNC_DEF("countBy", 1, js_array_ext_countby ),
    JS_CFUNC_DEF("indexBy", 1, js_array_ext_indexby ),
    JS_ALIAS_DEF("remove", "reject" ),   /* remove(matcher) == reject */
    JS_ALIAS_DEF("exclude", "reject" ),  /* exclude(matcher) == reject */
    JS_CFUNC_DEF("removeRange", 2, js_array_ext_removerange ),
    JS_CFUNC_DEF("splitWhen", 1, js_array_ext_splitwhen ),
    JS_CFUNC_DEF("innerJoin", 2, js_array_ext_innerjoin ),
    JS_CFUNC_MAGIC_DEF("startsWith", 1, js_array_ext_startsends, 0 ),
    JS_CFUNC_MAGIC_DEF("endsWith", 1, js_array_ext_startsends, 1 ),
    JS_CFUNC_DEF("unnest", 0, js_array_ext_unnest ),
    JS_CFUNC_MAGIC_DEF("dropRepeats", 0, js_array_ext_droprepeats, 0 ),
    JS_CFUNC_MAGIC_DEF("dropRepeatsWith", 1, js_array_ext_droprepeats, 1 ),
    JS_CFUNC_MAGIC_DEF("dropRepeatsBy", 1, js_array_ext_droprepeats, 2 ),
    JS_CFUNC_DEF("sortWith", 1, js_array_ext_sortwith ),
    JS_CFUNC_DEF("unionWith", 2, js_array_ext_unionwith ),
    JS_CFUNC_DEF("differenceWith", 2, js_array_ext_differencewith ),
    JS_CFUNC_MAGIC_DEF("symmetricDifference", 1, js_array_ext_symdiff, 0 ),
    JS_CFUNC_MAGIC_DEF("symmetricDifferenceWith", 2, js_array_ext_symdiff, 1 ),
    JS_CFUNC_DEF("reduceBy", 3, js_array_ext_reduceby ),
    JS_CFUNC_DEF("transduce", 3, js_array_ext_transduce ),
    JS_CFUNC_DEF("into", 2, js_array_ext_into ),
    JS_CFUNC_DEF("sequence", 1, js_array_ext_sequence ),
    JS_CFUNC_DEF("traverse", 2, js_array_ext_traverse ),
    JS_CFUNC_MAGIC_DEF("mapFromIndex", 2, js_array_fromindex, FI_MAP ),
    JS_CFUNC_MAGIC_DEF("forEachFromIndex", 2, js_array_fromindex, FI_FOREACH ),
    JS_CFUNC_MAGIC_DEF("filterFromIndex", 2, js_array_fromindex, FI_FILTER ),
    JS_CFUNC_MAGIC_DEF("findFromIndex", 2, js_array_fromindex, FI_FIND ),
    JS_CFUNC_MAGIC_DEF("findIndexFromIndex", 2, js_array_fromindex, FI_FINDINDEX ),
    JS_CFUNC_MAGIC_DEF("someFromIndex", 2, js_array_fromindex, FI_SOME ),
    JS_CFUNC_MAGIC_DEF("everyFromIndex", 2, js_array_fromindex, FI_EVERY ),
    JS_CFUNC_MAGIC_DEF("reduceFromIndex", 2, js_array_fromindex, FI_REDUCE ),
    JS_CFUNC_MAGIC_DEF("reduceRightFromIndex", 2, js_array_fromindex, FI_REDUCERIGHT ),
    /* the entry point to the lazy tier: the array iterator already inherits
     * Iterator.prototype, so this is `values()` under the name that says why */
    JS_CFUNC_MAGIC_DEF("lazy", 0, js_create_array_iterator, JS_ITERATOR_KIND_VALUE ),
};

static const JSCFunctionListEntry js_array_proto_funcs[] = {
    JS_CFUNC_DEF("at", 1, js_array_at ),
    JS_CFUNC_DEF("with", 2, js_array_with ),
    JS_CFUNC_DEF("concat", 1, js_array_concat ),
    JS_CFUNC_MAGIC_DEF("every", 1, js_array_every, special_every ),
    JS_CFUNC_MAGIC_DEF("some", 1, js_array_every, special_some ),
    JS_CFUNC_MAGIC_DEF("forEach", 1, js_array_every, special_forEach ),
    JS_CFUNC_MAGIC_DEF("map", 1, js_array_every, special_map ),
    JS_CFUNC_MAGIC_DEF("filter", 1, js_array_every, special_filter ),
    JS_CFUNC_MAGIC_DEF("reduce", 1, js_array_reduce, special_reduce ),
    JS_CFUNC_MAGIC_DEF("reduceRight", 1, js_array_reduce, special_reduceRight ),
    JS_CFUNC_DEF("fill", 1, js_array_fill ),
    JS_CFUNC_MAGIC_DEF("find", 1, js_array_find, ArrayFind ),
    JS_CFUNC_MAGIC_DEF("findIndex", 1, js_array_find, ArrayFindIndex ),
    JS_CFUNC_MAGIC_DEF("findLast", 1, js_array_find, ArrayFindLast ),
    JS_CFUNC_MAGIC_DEF("findLastIndex", 1, js_array_find, ArrayFindLastIndex ),
    JS_CFUNC_DEF("indexOf", 1, js_array_indexOf ),
    JS_CFUNC_DEF("lastIndexOf", 1, js_array_lastIndexOf ),
    JS_CFUNC_DEF("includes", 1, js_array_includes ),
    JS_CFUNC_MAGIC_DEF("join", 1, js_array_join, 0 ),
    JS_CFUNC_DEF("toString", 0, js_array_toString ),
    JS_CFUNC_MAGIC_DEF("toLocaleString", 0, js_array_join, 1 ),
    JS_CFUNC_MAGIC_DEF("pop", 0, js_array_pop, 0 ),
    JS_CFUNC_MAGIC_DEF("push", 1, js_array_push, 0 ),
    JS_CFUNC_MAGIC_DEF("shift", 0, js_array_pop, 1 ),
    JS_CFUNC_MAGIC_DEF("unshift", 1, js_array_push, 1 ),
    JS_CFUNC_DEF("reverse", 0, js_array_reverse ),
    JS_CFUNC_DEF("toReversed", 0, js_array_toReversed ),
    JS_CFUNC_DEF("sort", 1, js_array_sort ),
    JS_CFUNC_DEF("toSorted", 1, js_array_toSorted ),
    JS_CFUNC_DEF("slice", 2, js_array_slice ),
    JS_CFUNC_DEF("splice", 2, js_array_splice ),
    JS_CFUNC_DEF("toSpliced", 2, js_array_toSpliced ),
    JS_CFUNC_DEF("copyWithin", 2, js_array_copyWithin ),
    JS_CFUNC_MAGIC_DEF("flatMap", 1, js_array_flatten, 1 ),
    JS_CFUNC_MAGIC_DEF("flat", 0, js_array_flatten, 0 ),
    JS_CFUNC_MAGIC_DEF("values", 0, js_create_array_iterator, JS_ITERATOR_KIND_VALUE ),
    JS_ALIAS_DEF("[Symbol.iterator]", "values" ),
    JS_CFUNC_MAGIC_DEF("keys", 0, js_create_array_iterator, JS_ITERATOR_KIND_KEY ),
    JS_CFUNC_MAGIC_DEF("entries", 0, js_create_array_iterator, JS_ITERATOR_KIND_KEY_AND_VALUE ),
    JS_OBJECT_DEF("[Symbol.unscopables]", js_array_unscopables_funcs, countof(js_array_unscopables_funcs), JS_PROP_CONFIGURABLE ),
};

static const JSCFunctionListEntry js_array_iterator_proto_funcs[] = {
    JS_ITERATOR_NEXT_DEF("next", 0, js_array_iterator_next, 0 ),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "Array Iterator", JS_PROP_CONFIGURABLE ),
};


/* ============================================================================
 * The lazy tier (W6.2) — the ext methods that are single-pass and O(1)
 * in additional state, installed on Iterator.prototype. Each intermediate one
 * returns an Iterator Helper driven a single element at a time; the eager
 * Array.prototype form above stays the authority on semantics and
 * tests/test_iterator_lazy.js pins the two equal.
 *
 * Admission rule: single-pass, O(1) extra state. `unique` is the documented
 * exception — O(distinct) in a Set. `aperture`/`splitEvery` hold one window,
 * whose size is configuration, not input.
 *
 * The kinds and the JSIteratorHelperData fields live in iterator.inc.c; the
 * bodies live here, next to the eager methods they must agree with.
 * ========================================================================== */

/* symbol.inc.c, later in the unity build — the Set backing `unique`. A real
 * Set, not the private DynValSet, because the helper holds it across calls and
 * the cycle GC has to see the values it owns. */
#ifndef MAGIC_SET
#define MAGIC_SET (1 << 0)      /* symbol.inc.c redefines this identically */
#endif
static JSValue js_map_constructor(JSContext *ctx, JSValueConst new_target,
                                  int argc, JSValueConst *argv, int magic);
static JSValue js_map_set(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv, int magic);
static JSValue js_map_has(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv, int magic);

/* Pull one element from the source. Returns an owned value; *pdone on exhaustion. */
static JSValue js_lazy_pull(JSContext *ctx, JSIteratorHelperData *it, int *pdone)
{
    return JS_IteratorNext(ctx, it->obj, it->next, 0, NULL, pdone);
}

/* The matcher kind is resolved ONCE at construction (kind in count2, the
 * RegExp `test` method in aux), so the per-element test has no re-resolution
 * cost — the same bypass js_ext_matcher_begin buys the eager methods. */
static int js_lazy_match(JSContext *ctx, JSIteratorHelperData *it,
                         JSValueConst el)
{
    JSExtMatcher pm;
    pm.matcher = it->func;
    pm.regex_test = it->aux;
    pm.kind = (int)it->count2;
    return js_ext_matcher_test(ctx, &pm, el);
}

/* tee() shared state, a plain Array: [0] source, [1] next method, [2] queue,
 * [3] absolute index of queue[0], [4] source exhausted, [5+i] branch cursors. */
enum { TEE_SRC = 0, TEE_NEXT, TEE_QUEUE, TEE_BASE, TEE_DONE, TEE_CURSOR0 };

static int js_lazy_tee_geti(JSContext *ctx, JSValueConst st, int idx, int64_t *pv)
{
    JSValue v = JS_GetPropertyInt64(ctx, st, idx);
    if (JS_IsException(v))
        return -1;
    return JS_ToInt64Free(ctx, pv, v);
}

/* Drop the prefix every branch has passed. Compacted only when it is at least
 * half the queue, so the moves stay amortised O(1) per element. */
static int js_lazy_tee_compact(JSContext *ctx, JSValueConst st, int64_t nbranch)
{
    int64_t base, qlen, low = INT64_MAX, i, drop;
    JSValue queue;
    if (js_lazy_tee_geti(ctx, st, TEE_BASE, &base))
        return -1;
    for (i = 0; i < nbranch; i++) {
        int64_t c;
        if (js_lazy_tee_geti(ctx, st, TEE_CURSOR0 + i, &c))
            return -1;
        if (c < low)
            low = c;
    }
    drop = low - base;
    if (drop <= 0)
        return 0;
    queue = JS_GetPropertyInt64(ctx, st, TEE_QUEUE);
    if (JS_IsException(queue))
        return -1;
    if (js_get_length64(ctx, &qlen, queue)) {
        JS_FreeValue(ctx, queue);
        return -1;
    }
    if (drop * 2 < qlen) {          /* not worth moving yet */
        JS_FreeValue(ctx, queue);
        return 0;
    }
    for (i = drop; i < qlen; i++) {
        JSValue v = JS_GetPropertyInt64(ctx, queue, i);
        if (JS_IsException(v)) { JS_FreeValue(ctx, queue); return -1; }
        if (JS_SetPropertyInt64(ctx, queue, i - drop, v) < 0) {
            JS_FreeValue(ctx, queue);
            return -1;
        }
    }
    if (JS_SetPropertyStr(ctx, queue, "length", JS_NewInt64(ctx, qlen - drop)) < 0) {
        JS_FreeValue(ctx, queue);
        return -1;
    }
    JS_FreeValue(ctx, queue);
    return JS_SetPropertyInt64(ctx, st, TEE_BASE, JS_NewInt64(ctx, low)) < 0 ? -1 : 0;
}

static JSValue js_lazy_tee_next(JSContext *ctx, JSIteratorHelperData *it, int *pdone)
{
    JSValueConst st = it->aux;
    JSValue queue = JS_UNDEFINED, item = JS_UNDEFINED, src, nextm;
    int64_t cursor, base, qlen, srcdone, nbranch = it->count;
    int done;

    if (js_lazy_tee_geti(ctx, st, TEE_CURSOR0 + it->count2, &cursor) ||
        js_lazy_tee_geti(ctx, st, TEE_BASE, &base) ||
        js_lazy_tee_geti(ctx, st, TEE_DONE, &srcdone))
        goto fail;
    queue = JS_GetPropertyInt64(ctx, st, TEE_QUEUE);
    if (JS_IsException(queue))
        goto fail;
    if (js_get_length64(ctx, &qlen, queue))
        goto fail;

    if (cursor - base < qlen) {
        item = JS_GetPropertyInt64(ctx, queue, cursor - base);
        if (JS_IsException(item))
            goto fail;
    } else {
        if (srcdone) {
            JS_FreeValue(ctx, queue);
            *pdone = TRUE;
            return JS_UNDEFINED;
        }
        src = JS_GetPropertyInt64(ctx, st, TEE_SRC);
        if (JS_IsException(src))
            goto fail;
        nextm = JS_GetPropertyInt64(ctx, st, TEE_NEXT);
        if (JS_IsException(nextm)) { JS_FreeValue(ctx, src); goto fail; }
        item = JS_IteratorNext(ctx, src, nextm, 0, NULL, &done);
        JS_FreeValue(ctx, src);
        JS_FreeValue(ctx, nextm);
        if (JS_IsException(item))
            goto fail;
        if (done) {
            JS_FreeValue(ctx, item);
            JS_FreeValue(ctx, queue);
            if (JS_SetPropertyInt64(ctx, st, TEE_DONE, JS_NewInt32(ctx, 1)) < 0) {
                *pdone = TRUE;
                return JS_EXCEPTION;
            }
            *pdone = TRUE;
            return JS_UNDEFINED;
        }
        if (JS_SetPropertyInt64(ctx, queue, qlen, JS_DupValue(ctx, item)) < 0)
            goto fail;
    }
    JS_FreeValue(ctx, queue);
    queue = JS_UNDEFINED;
    if (JS_SetPropertyInt64(ctx, st, TEE_CURSOR0 + it->count2,
                            JS_NewInt64(ctx, cursor + 1)) < 0)
        goto fail;
    if (js_lazy_tee_compact(ctx, st, nbranch))
        goto fail;
    return item;
 fail:
    JS_FreeValue(ctx, queue);
    JS_FreeValue(ctx, item);
    *pdone = TRUE;
    return JS_EXCEPTION;
}

static JSValue js_iterator_lazy_next(JSContext *ctx, JSIteratorHelperData *it,
                                     int magic, int *pdone)
{
    JSValue item = JS_UNDEFINED, out;
    int done = 0;

    if (magic == GEN_MAGIC_RETURN) {
        /* .return() on a helper closes what it is driving and latches done.
         * A tee branch shares its source with its siblings and must not. */
        *pdone = TRUE;
        if (it->kind == JS_ITERATOR_HELPER_KIND_ZIP_WITH && JS_IsObject(it->acc))
            JS_IteratorClose(ctx, it->acc, FALSE);
        if (it->kind != JS_ITERATOR_HELPER_KIND_TEE)
            if (JS_IteratorClose(ctx, it->obj, FALSE) < 0)
                return JS_EXCEPTION;
        return JS_UNDEFINED;
    }

    switch (it->kind) {
    case JS_ITERATOR_HELPER_KIND_TAKE_WHILE:
        {
            int m;
            item = js_lazy_pull(ctx, it, &done);
            if (JS_IsException(item))
                goto fail_no_close;
            if (done)
                goto exhausted;
            m = js_lazy_match(ctx, it, item);
            if (m < 0) {
                JS_FreeValue(ctx, item);
                goto fail;
            }
            if (!m) {
                JS_FreeValue(ctx, item);
                *pdone = TRUE;
                if (JS_IteratorClose(ctx, it->obj, FALSE) < 0)
                    return JS_EXCEPTION;
                return JS_UNDEFINED;
            }
            return item;
        }
    case JS_ITERATOR_HELPER_KIND_DROP_WHILE:
        {
            for (;;) {
                int m;
                item = js_lazy_pull(ctx, it, &done);
                if (JS_IsException(item))
                    goto fail_no_close;
                if (done)
                    goto exhausted;
                if (it->started)
                    return item;
                m = js_lazy_match(ctx, it, item);
                if (m < 0) {
                    JS_FreeValue(ctx, item);
                    goto fail;
                }
                if (!m) {
                    it->started = 1;
                    return item;
                }
                JS_FreeValue(ctx, item);
            }
        }
    case JS_ITERATOR_HELPER_KIND_SCAN:
        {
            JSValueConst args[2];
            if (!it->started) {
                it->started = 1;
                return JS_DupValue(ctx, it->acc);
            }
            item = js_lazy_pull(ctx, it, &done);
            if (JS_IsException(item))
                goto fail_no_close;
            if (done)
                goto exhausted;
            args[0] = it->acc;
            args[1] = item;
            out = JS_Call(ctx, it->func, JS_UNDEFINED, 2, args);
            JS_FreeValue(ctx, item);
            if (JS_IsException(out))
                goto fail;
            JS_FreeValue(ctx, it->acc);
            it->acc = out;
            return JS_DupValue(ctx, it->acc);
        }
    case JS_ITERATOR_HELPER_KIND_INTERSPERSE:
        {
            if (it->pending) {          /* the separator has just been emitted */
                it->pending = 0;
                out = it->acc;
                it->acc = JS_UNDEFINED;
                return out;
            }
            item = js_lazy_pull(ctx, it, &done);
            if (JS_IsException(item))
                goto fail_no_close;
            if (done)
                goto exhausted;
            if (!it->started) {
                it->started = 1;
                return item;
            }
            it->acc = item;
            it->pending = 1;
            return JS_DupValue(ctx, it->func);
        }
    case JS_ITERATOR_HELPER_KIND_COMPACT:
        {
            for (;;) {
                item = js_lazy_pull(ctx, it, &done);
                if (JS_IsException(item))
                    goto fail_no_close;
                if (done)
                    goto exhausted;
                if (!JS_IsNull(item) && !JS_IsUndefined(item))
                    return item;
                JS_FreeValue(ctx, item);
            }
        }
    case JS_ITERATOR_HELPER_KIND_DROP_REPEATS:
    case JS_ITERATOR_HELPER_KIND_DROP_REPEATS_WITH:
    case JS_ITERATOR_HELPER_KIND_DROP_REPEATS_BY:
        {
            for (;;) {
                JSValue curkey = JS_UNDEFINED;
                int same = 0;
                item = js_lazy_pull(ctx, it, &done);
                if (JS_IsException(item))
                    goto fail_no_close;
                if (done)
                    goto exhausted;
                if (it->kind == JS_ITERATOR_HELPER_KIND_DROP_REPEATS_BY) {
                    curkey = js_array_ext_mapval(ctx, it->func, item);
                    if (JS_IsException(curkey)) {
                        JS_FreeValue(ctx, item);
                        goto fail;
                    }
                }
                if (it->started) {
                    if (it->kind == JS_ITERATOR_HELPER_KIND_DROP_REPEATS) {
                        same = js_deep_equals(ctx, it->acc, item, 0);
                    } else if (it->kind == JS_ITERATOR_HELPER_KIND_DROP_REPEATS_WITH) {
                        JSValueConst ab[2];
                        JSValue r;
                        ab[0] = it->acc;
                        ab[1] = item;
                        r = JS_Call(ctx, it->func, JS_UNDEFINED, 2, ab);
                        if (JS_IsException(r)) {
                            same = -1;
                        } else {
                            same = JS_ToBool(ctx, r);
                            JS_FreeValue(ctx, r);
                        }
                    } else {
                        same = js_deep_equals(ctx, it->aux, curkey, 0);
                    }
                    if (same < 0) {
                        JS_FreeValue(ctx, item);
                        JS_FreeValue(ctx, curkey);
                        goto fail;
                    }
                }
                if (!same) {
                    it->started = 1;
                    JS_FreeValue(ctx, it->acc);
                    it->acc = JS_DupValue(ctx, item);
                    if (it->kind == JS_ITERATOR_HELPER_KIND_DROP_REPEATS_BY) {
                        JS_FreeValue(ctx, it->aux);
                        it->aux = curkey;
                    } else {
                        JS_FreeValue(ctx, curkey);
                    }
                    return item;
                }
                JS_FreeValue(ctx, curkey);
                JS_FreeValue(ctx, item);
            }
        }
    case JS_ITERATOR_HELPER_KIND_APERTURE:
        {
            int64_t n = it->count2, i;
            if (!it->started) {
                for (i = 0; i < n; i++) {
                    item = js_lazy_pull(ctx, it, &done);
                    if (JS_IsException(item))
                        goto fail_no_close;
                    if (done)
                        goto exhausted;    /* fewer than n elements: no window */
                    if (JS_SetPropertyInt64(ctx, it->aux, i, item) < 0)
                        goto fail;
                }
                it->started = 1;
            } else {
                item = js_lazy_pull(ctx, it, &done);
                if (JS_IsException(item))
                    goto fail_no_close;
                if (done)
                    goto exhausted;
                for (i = 1; i < n; i++) {
                    JSValue v = JS_GetPropertyInt64(ctx, it->aux, i);
                    if (JS_IsException(v)) { JS_FreeValue(ctx, item); goto fail; }
                    if (JS_SetPropertyInt64(ctx, it->aux, i - 1, v) < 0) {
                        JS_FreeValue(ctx, item);
                        goto fail;
                    }
                }
                if (JS_SetPropertyInt64(ctx, it->aux, n - 1, item) < 0)
                    goto fail;
            }
            out = JS_NewArray(ctx);
            if (JS_IsException(out))
                goto fail;
            for (i = 0; i < n; i++) {
                JSValue v = JS_GetPropertyInt64(ctx, it->aux, i);
                if (JS_IsException(v) ||
                    JS_DefinePropertyValueInt64(ctx, out, i, v, JS_PROP_C_W_E) < 0) {
                    JS_FreeValue(ctx, out);
                    goto fail;
                }
            }
            return out;
        }
    case JS_ITERATOR_HELPER_KIND_SPLIT_EVERY:
        {
            int64_t n = it->count2, i;
            out = JS_NewArray(ctx);
            if (JS_IsException(out))
                goto fail;
            for (i = 0; i < n; i++) {
                item = js_lazy_pull(ctx, it, &done);
                if (JS_IsException(item)) {
                    JS_FreeValue(ctx, out);
                    goto fail_no_close;
                }
                if (done) {
                    JS_FreeValue(ctx, item);
                    break;
                }
                if (JS_DefinePropertyValueInt64(ctx, out, i, item, JS_PROP_C_W_E) < 0) {
                    JS_FreeValue(ctx, out);
                    goto fail;
                }
            }
            if (i == 0) {
                JS_FreeValue(ctx, out);
                *pdone = TRUE;
                return JS_UNDEFINED;
            }
            return out;
        }
    case JS_ITERATOR_HELPER_KIND_ZIP_WITH:
        {
            JSValue other;
            JSValueConst args[2];
            int odone = 0;
            item = js_lazy_pull(ctx, it, &done);
            if (JS_IsException(item))
                goto fail_no_close;
            if (done) {
                JS_IteratorClose(ctx, it->acc, FALSE);
                goto exhausted;
            }
            other = JS_IteratorNext(ctx, it->acc, it->aux, 0, NULL, &odone);
            if (JS_IsException(other)) {
                JS_FreeValue(ctx, item);
                goto fail;
            }
            if (odone) {
                JS_FreeValue(ctx, other);
                JS_FreeValue(ctx, item);
                *pdone = TRUE;
                if (JS_IteratorClose(ctx, it->obj, FALSE) < 0)
                    return JS_EXCEPTION;
                return JS_UNDEFINED;
            }
            args[0] = item;
            args[1] = other;
            out = JS_Call(ctx, it->func, JS_UNDEFINED, 2, args);
            JS_FreeValue(ctx, item);
            JS_FreeValue(ctx, other);
            if (JS_IsException(out)) {
                JS_IteratorClose(ctx, it->acc, TRUE);
                goto fail;
            }
            return out;
        }
    case JS_ITERATOR_HELPER_KIND_PLUCK:
        {
            item = js_lazy_pull(ctx, it, &done);
            if (JS_IsException(item))
                goto fail_no_close;
            if (done)
                goto exhausted;
            out = JS_GetPropertyValue(ctx, item, JS_DupValue(ctx, it->func));
            JS_FreeValue(ctx, item);
            if (JS_IsException(out))
                goto fail;
            return out;
        }
    case JS_ITERATOR_HELPER_KIND_INIT:
        {
            if (!it->started) {
                item = js_lazy_pull(ctx, it, &done);
                if (JS_IsException(item))
                    goto fail_no_close;
                if (done)
                    goto exhausted;
                it->acc = item;
                it->started = 1;
            }
            item = js_lazy_pull(ctx, it, &done);
            if (JS_IsException(item))
                goto fail_no_close;
            if (done)
                goto exhausted;             /* the buffered one was the last */
            out = it->acc;
            it->acc = item;
            return out;
        }
    case JS_ITERATOR_HELPER_KIND_TAIL:
        {
            if (!it->started) {
                it->started = 1;
                item = js_lazy_pull(ctx, it, &done);
                if (JS_IsException(item))
                    goto fail_no_close;
                JS_FreeValue(ctx, item);
                if (done)
                    goto exhausted;
            }
            item = js_lazy_pull(ctx, it, &done);
            if (JS_IsException(item))
                goto fail_no_close;
            if (done)
                goto exhausted;
            return item;
        }
    case JS_ITERATOR_HELPER_KIND_UNIQUE:
        {
            for (;;) {
                JSValue key, r;
                int fresh;
                item = js_lazy_pull(ctx, it, &done);
                if (JS_IsException(item))
                    goto fail_no_close;
                if (done)
                    goto exhausted;
                key = js_array_ext_mapval(ctx, it->func, item);
                if (JS_IsException(key)) {
                    JS_FreeValue(ctx, item);
                    goto fail;
                }
                r = js_map_has(ctx, it->aux, 1, (JSValueConst *)&key, MAGIC_SET);
                if (JS_IsException(r)) {
                    JS_FreeValue(ctx, key);
                    JS_FreeValue(ctx, item);
                    goto fail;
                }
                fresh = !JS_ToBool(ctx, r);
                JS_FreeValue(ctx, r);
                if (fresh) {
                    r = js_map_set(ctx, it->aux, 1, (JSValueConst *)&key, MAGIC_SET);
                    JS_FreeValue(ctx, key);
                    if (JS_IsException(r)) {
                        JS_FreeValue(ctx, item);
                        goto fail;
                    }
                    JS_FreeValue(ctx, r);
                    return item;
                }
                JS_FreeValue(ctx, key);
                JS_FreeValue(ctx, item);
            }
        }
    case JS_ITERATOR_HELPER_KIND_TEE:
        return js_lazy_tee_next(ctx, it, pdone);
    default:
        abort();
    }

 exhausted:
    JS_FreeValue(ctx, item);
    *pdone = TRUE;
    return JS_UNDEFINED;
 fail:
    JS_IteratorClose(ctx, it->obj, TRUE);
 fail_no_close:
    *pdone = TRUE;              /* latch done: never re-enter a broken pipeline */
    return JS_EXCEPTION;
}

/* Build one lazy helper. `magic` IS the kind. Mirrors js_create_iterator_helper:
 * a failure after `this_val` is known to be an iterator closes it. */
static JSValue js_iterator_ext_lazy(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv, int magic)
{
    JSValueConst func = JS_UNDEFINED;
    JSValue obj, method, acc = JS_UNDEFINED, aux = JS_UNDEFINED;
    JSIteratorHelperData *it;
    int64_t count = 0, count2 = 0;

    if (!JS_IsObject(this_val))
        return JS_ThrowTypeErrorNotAnObject(ctx);

    switch (magic) {
    case JS_ITERATOR_HELPER_KIND_TAKE_WHILE:
    case JS_ITERATOR_HELPER_KIND_DROP_WHILE:
        {
            JSExtMatcher pm;
            func = argc > 0 ? argv[0] : JS_UNDEFINED;
            if (js_ext_matcher_begin(ctx, &pm, func))
                goto fail;
            aux = pm.regex_test;        /* owned; the resolution happens once */
            count2 = pm.kind;
        }
        break;
    case JS_ITERATOR_HELPER_KIND_SCAN:
        func = argc > 0 ? argv[0] : JS_UNDEFINED;
        if (check_function(ctx, func))
            goto fail;
        acc = JS_DupValue(ctx, argc > 1 ? argv[1] : JS_UNDEFINED);
        break;
    case JS_ITERATOR_HELPER_KIND_DROP_REPEATS_WITH:
        func = argc > 0 ? argv[0] : JS_UNDEFINED;
        if (check_function(ctx, func))
            goto fail;
        break;
    case JS_ITERATOR_HELPER_KIND_INTERSPERSE:
    case JS_ITERATOR_HELPER_KIND_PLUCK:
    case JS_ITERATOR_HELPER_KIND_DROP_REPEATS_BY:
        func = argc > 0 ? argv[0] : JS_UNDEFINED;
        break;
    case JS_ITERATOR_HELPER_KIND_UNIQUE:
        func = argc > 0 ? argv[0] : JS_UNDEFINED;
        aux = js_map_constructor(ctx, JS_UNDEFINED, 0, NULL, MAGIC_SET);
        if (JS_IsException(aux))
            goto fail;
        break;
    case JS_ITERATOR_HELPER_KIND_APERTURE:
    case JS_ITERATOR_HELPER_KIND_SPLIT_EVERY:
        if (JS_ToInt64Sat(ctx, &count2, argc > 0 ? argv[0] : JS_UNDEFINED))
            goto fail;
        if (count2 < 1) {
            /* the eager form derives its window count from a known length; a
             * single-pass one cannot, so a non-positive size is an error here */
            JS_ThrowRangeError(ctx, "size must be >= 1");
            goto fail;
        }
        if (magic == JS_ITERATOR_HELPER_KIND_APERTURE) {
            aux = JS_NewArray(ctx);
            if (JS_IsException(aux))
                goto fail;
        }
        break;
    case JS_ITERATOR_HELPER_KIND_ZIP_WITH:
        /* (fn, other) — the eager Array.prototype.zipWith order */
        func = argc > 0 ? argv[0] : JS_UNDEFINED;
        if (check_function(ctx, func))
            goto fail;
        acc = JS_GetIterator(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, FALSE);
        if (JS_IsException(acc))
            goto fail;
        aux = JS_GetProperty(ctx, acc, JS_ATOM_next);
        if (JS_IsException(aux))
            goto fail;
        break;
    case JS_ITERATOR_HELPER_KIND_COMPACT:
    case JS_ITERATOR_HELPER_KIND_DROP_REPEATS:
    case JS_ITERATOR_HELPER_KIND_INIT:
    case JS_ITERATOR_HELPER_KIND_TAIL:
        break;
    default:
        abort();
    }

    method = JS_GetProperty(ctx, this_val, JS_ATOM_next);
    if (JS_IsException(method))
        goto fail;
    obj = JS_NewObjectClass(ctx, JS_CLASS_ITERATOR_HELPER);
    if (JS_IsException(obj)) {
        JS_FreeValue(ctx, method);
        goto fail;
    }
    it = js_malloc(ctx, sizeof(*it));
    if (!it) {
        JS_FreeValue(ctx, obj);
        JS_FreeValue(ctx, method);
        goto fail;
    }
    it->kind = magic;
    it->obj = JS_DupValue(ctx, this_val);
    it->func = JS_DupValue(ctx, func);
    it->next = method;
    it->inner = JS_UNDEFINED;
    it->acc = acc;
    it->aux = aux;
    it->count = count;
    it->count2 = count2;
    it->executing = 0;
    it->done = 0;
    it->started = 0;
    it->pending = 0;
    JS_SetOpaque(obj, it);
    return obj;
 fail:
    /* only zipWith's `acc` is an iterator — closing anything else would call a
     * user `return` method on a plain accumulator */
    if (magic == JS_ITERATOR_HELPER_KIND_ZIP_WITH && JS_IsObject(acc))
        JS_IteratorClose(ctx, acc, TRUE);
    JS_FreeValue(ctx, acc);
    JS_FreeValue(ctx, aux);
    JS_IteratorClose(ctx, this_val, TRUE);
    return JS_EXCEPTION;
}

/* tee(n = 2) -> an array of n independent iterators over the same source. The
 * shared queue holds exactly the lag between the fastest and slowest branch:
 * draining one branch fully before the others buffers the whole source. */
static JSValue js_iterator_ext_tee(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    JSValue st = JS_UNDEFINED, out = JS_UNDEFINED, queue, method = JS_UNDEFINED;
    int64_t n = 2, i;

    if (!JS_IsObject(this_val))
        return JS_ThrowTypeErrorNotAnObject(ctx);
    if (argc > 0 && !JS_IsUndefined(argv[0])) {
        if (JS_ToInt64Sat(ctx, &n, argv[0]))
            goto fail;
        if (n < 1 || n > 1024) {
            JS_ThrowRangeError(ctx, "branch count must be 1..1024");
            goto fail;
        }
    }
    method = JS_GetProperty(ctx, this_val, JS_ATOM_next);
    if (JS_IsException(method))
        goto fail;
    st = JS_NewArray(ctx);
    if (JS_IsException(st))
        goto fail;
    queue = JS_NewArray(ctx);
    if (JS_IsException(queue))
        goto fail;
    if (JS_SetPropertyInt64(ctx, st, TEE_QUEUE, queue) < 0 ||
        JS_SetPropertyInt64(ctx, st, TEE_SRC, JS_DupValue(ctx, this_val)) < 0 ||
        JS_SetPropertyInt64(ctx, st, TEE_NEXT, JS_DupValue(ctx, method)) < 0 ||
        JS_SetPropertyInt64(ctx, st, TEE_BASE, JS_NewInt32(ctx, 0)) < 0 ||
        JS_SetPropertyInt64(ctx, st, TEE_DONE, JS_NewInt32(ctx, 0)) < 0)
        goto fail;
    out = JS_NewArray(ctx);
    if (JS_IsException(out))
        goto fail;
    for (i = 0; i < n; i++) {
        JSValue branch;
        JSIteratorHelperData *it;
        if (JS_SetPropertyInt64(ctx, st, TEE_CURSOR0 + i, JS_NewInt32(ctx, 0)) < 0)
            goto fail;
        branch = JS_NewObjectClass(ctx, JS_CLASS_ITERATOR_HELPER);
        if (JS_IsException(branch))
            goto fail;
        it = js_malloc(ctx, sizeof(*it));
        if (!it) {
            JS_FreeValue(ctx, branch);
            goto fail;
        }
        it->kind = JS_ITERATOR_HELPER_KIND_TEE;
        it->obj = JS_DupValue(ctx, this_val);
        it->func = JS_UNDEFINED;
        it->next = JS_DupValue(ctx, method);
        it->inner = JS_UNDEFINED;
        it->acc = JS_UNDEFINED;
        it->aux = JS_DupValue(ctx, st);
        it->count = n;              /* branch count, for the queue compaction */
        it->count2 = i;             /* this branch's cursor slot */
        it->executing = 0;
        it->done = 0;
        it->started = 0;
        it->pending = 0;
        JS_SetOpaque(branch, it);
        if (JS_DefinePropertyValueInt64(ctx, out, i, branch, JS_PROP_C_W_E) < 0)
            goto fail;
    }
    JS_FreeValue(ctx, method);
    JS_FreeValue(ctx, st);
    return out;
 fail:
    JS_FreeValue(ctx, out);
    JS_FreeValue(ctx, st);
    JS_FreeValue(ctx, method);
    JS_IteratorClose(ctx, this_val, TRUE);
    return JS_EXCEPTION;
}

/* The lazy tier's terminals. Each drives the iterator to exhaustion (or closes
 * it on an early exit) and returns a value, matching the eager Array.prototype
 * method of the same name. */
enum {
    ITX_SUM = 0, ITX_AVERAGE, ITX_PRODUCT,
};

static JSValue js_iterator_ext_stat(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv, int magic)
{
    JSValue method, item;
    double acc = (magic == ITX_PRODUCT) ? 1 : 0;
    int64_t n = 0;
    int done;
    (void)argc; (void)argv;

    if (!JS_IsObject(this_val))
        return JS_ThrowTypeErrorNotAnObject(ctx);
    method = JS_GetProperty(ctx, this_val, JS_ATOM_next);
    if (JS_IsException(method))
        return JS_EXCEPTION;
    for (;;) {
        double d;
        int r;
        item = JS_IteratorNext(ctx, this_val, method, 0, NULL, &done);
        if (JS_IsException(item))
            goto fail;
        if (done)
            break;
        r = JS_ToFloat64(ctx, &d, item);
        JS_FreeValue(ctx, item);
        if (r) {
            JS_IteratorClose(ctx, this_val, TRUE);
            goto fail;
        }
        if (magic == ITX_PRODUCT)
            acc *= d;
        else
            acc += d;
        n++;
    }
    JS_FreeValue(ctx, method);
    if (magic == ITX_AVERAGE)
        acc = n ? acc / (double)n : 0;
    return JS_NewFloat64(ctx, acc);
 fail:
    JS_FreeValue(ctx, method);
    return JS_EXCEPTION;
}

/* min/max (magic 0/1): the ELEMENT whose mapped value is numerically smallest /
 * largest, first on a tie — the eager form's rule. */
static JSValue js_iterator_ext_minmax(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv, int magic)
{
    JSValueConst map = argc > 0 ? argv[0] : JS_UNDEFINED;
    JSValue method, item, best = JS_UNDEFINED;
    double best_key = 0;
    int done, have = 0;

    if (!JS_IsObject(this_val))
        return JS_ThrowTypeErrorNotAnObject(ctx);
    method = JS_GetProperty(ctx, this_val, JS_ATOM_next);
    if (JS_IsException(method))
        return JS_EXCEPTION;
    for (;;) {
        JSValue keyv;
        double d;
        int r;
        item = JS_IteratorNext(ctx, this_val, method, 0, NULL, &done);
        if (JS_IsException(item))
            goto fail;
        if (done)
            break;
        keyv = js_array_ext_mapval(ctx, map, item);
        if (JS_IsException(keyv)) {
            JS_FreeValue(ctx, item);
            goto fail_close;
        }
        r = JS_ToFloat64(ctx, &d, keyv);
        JS_FreeValue(ctx, keyv);
        if (r) {
            JS_FreeValue(ctx, item);
            goto fail_close;
        }
        if (!have || (magic ? d > best_key : d < best_key)) {
            JS_FreeValue(ctx, best);
            best = item;
            best_key = d;
            have = 1;
        } else {
            JS_FreeValue(ctx, item);
        }
    }
    JS_FreeValue(ctx, method);
    return best;
 fail_close:
    JS_IteratorClose(ctx, this_val, TRUE);
 fail:
    JS_FreeValue(ctx, best);
    JS_FreeValue(ctx, method);
    return JS_EXCEPTION;
}

/* none/any/all/count (magic 0/1/2/3) against a value, RegExp or predicate.
 * count() with no argument counts every element. */
static JSValue js_iterator_ext_quantify(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv, int magic)
{
    JSValueConst match = argc > 0 ? argv[0] : JS_UNDEFINED;
    JSExtMatcher pm = { JS_UNDEFINED, JS_UNDEFINED, 0 };
    JSValue method, item, r = JS_EXCEPTION;
    int done, counting_all = (magic == 3 && argc == 0);
    int64_t c = 0;

    if (!JS_IsObject(this_val))
        return JS_ThrowTypeErrorNotAnObject(ctx);
    method = JS_GetProperty(ctx, this_val, JS_ATOM_next);
    if (JS_IsException(method))
        return JS_EXCEPTION;
    if (js_ext_matcher_begin(ctx, &pm, match))
        goto fail;
    for (;;) {
        int m;
        item = JS_IteratorNext(ctx, this_val, method, 0, NULL, &done);
        if (JS_IsException(item))
            goto fail;
        if (done)
            break;
        if (counting_all) {
            JS_FreeValue(ctx, item);
            c++;
            continue;
        }
        m = js_ext_matcher_test(ctx, &pm, item);
        JS_FreeValue(ctx, item);
        if (m < 0)
            goto fail_close;
        if (magic == 3) {
            c += (m != 0);
            continue;
        }
        if ((magic == 1 && m) || (magic == 0 && m) || (magic == 2 && !m)) {
            r = (magic == 1) ? JS_TRUE : JS_FALSE;
            if (JS_IteratorClose(ctx, this_val, FALSE) < 0)
                r = JS_EXCEPTION;
            goto done;
        }
    }
    r = (magic == 3) ? JS_NewInt64(ctx, c)
                     : ((magic == 1) ? JS_FALSE : JS_TRUE);
 done:
    js_ext_matcher_end(ctx, &pm);
    JS_FreeValue(ctx, method);
    return r;
 fail_close:
    JS_IteratorClose(ctx, this_val, TRUE);
 fail:
    js_ext_matcher_end(ctx, &pm);
    JS_FreeValue(ctx, method);
    return JS_EXCEPTION;
}

/* last/first/nth/findIndex (magic 0/1/2/3). first/nth/findIndex close the
 * source as soon as they have their answer. */
static JSValue js_iterator_ext_pick(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv, int magic)
{
    JSValue method, item, r = JS_UNDEFINED, last = JS_UNDEFINED;
    int64_t want = 0, idx = 0;
    int done;

    if (!JS_IsObject(this_val))
        return JS_ThrowTypeErrorNotAnObject(ctx);
    if (magic == 2) {
        if (JS_ToInt64Sat(ctx, &want, argc > 0 ? argv[0] : JS_UNDEFINED))
            return JS_EXCEPTION;
        if (want < 0)       /* a negative index needs the end: not single-pass */
            return JS_ThrowRangeError(ctx, "index must be >= 0");
    } else if (magic == 3) {
        if (check_function(ctx, argc > 0 ? argv[0] : JS_UNDEFINED))
            return JS_EXCEPTION;
    }
    method = JS_GetProperty(ctx, this_val, JS_ATOM_next);
    if (JS_IsException(method))
        return JS_EXCEPTION;
    for (;; idx++) {
        item = JS_IteratorNext(ctx, this_val, method, 0, NULL, &done);
        if (JS_IsException(item))
            goto fail;
        if (done)
            break;
        if (magic == 0) {                   /* last */
            JS_FreeValue(ctx, last);
            last = item;
            continue;
        }
        if (magic == 3) {                   /* findIndex */
            JSValue t;
            JSValueConst args[2];
            JSValue iv = JS_NewInt64(ctx, idx);
            args[0] = item;
            args[1] = iv;
            t = JS_Call(ctx, argv[0], JS_UNDEFINED, 2, args);
            JS_FreeValue(ctx, iv);
            JS_FreeValue(ctx, item);
            if (JS_IsException(t))
                goto fail_close;
            if (!JS_ToBoolFree(ctx, t))
                continue;
            r = JS_NewInt64(ctx, idx);
        } else if (magic == 1 || idx == want) {
            r = item;
            item = JS_UNDEFINED;
        } else {
            JS_FreeValue(ctx, item);
            continue;
        }
        if (JS_IteratorClose(ctx, this_val, FALSE) < 0) {
            JS_FreeValue(ctx, r);
            goto fail;
        }
        JS_FreeValue(ctx, method);
        return r;
    }
    JS_FreeValue(ctx, method);
    if (magic == 0)
        return last;
    return (magic == 3) ? JS_NewInt32(ctx, -1) : JS_UNDEFINED;
 fail_close:
    JS_IteratorClose(ctx, this_val, TRUE);
 fail:
    JS_FreeValue(ctx, last);
    JS_FreeValue(ctx, method);
    return JS_EXCEPTION;
}

/* countBy/indexBy/groupBy/reduceBy (magic 0/1/2/3) -> a plain object. */
static JSValue js_iterator_ext_collect(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv, int magic)
{
    JSValueConst fn  = argc > 0 ? argv[0] : JS_UNDEFINED;
    JSValueConst seed = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSValueConst keyFn = argc > 2 ? argv[2] : JS_UNDEFINED;
    JSValue method, item, result;
    int done;

    if (!JS_IsObject(this_val))
        return JS_ThrowTypeErrorNotAnObject(ctx);
    if (magic == 3 && (check_function(ctx, fn) || check_function(ctx, keyFn)))
        return JS_EXCEPTION;
    method = JS_GetProperty(ctx, this_val, JS_ATOM_next);
    if (JS_IsException(method))
        return JS_EXCEPTION;
    result = JS_NewObject(ctx);
    if (JS_IsException(result)) {
        JS_FreeValue(ctx, method);
        return JS_EXCEPTION;
    }
    for (;;) {
        JSValue key, cur;
        JSAtom atom;
        item = JS_IteratorNext(ctx, this_val, method, 0, NULL, &done);
        if (JS_IsException(item))
            goto fail;
        if (done)
            break;
        key = (magic == 3) ? JS_Call(ctx, keyFn, JS_UNDEFINED, 1, (JSValueConst *)&item)
                           : js_array_ext_mapval(ctx, fn, item);
        if (JS_IsException(key)) {
            JS_FreeValue(ctx, item);
            goto fail_close;
        }
        atom = JS_ValueToAtom(ctx, key);
        JS_FreeValue(ctx, key);
        if (atom == JS_ATOM_NULL) {
            JS_FreeValue(ctx, item);
            goto fail_close;
        }
        cur = JS_GetProperty(ctx, result, atom);
        if (JS_IsException(cur)) {
            JS_FreeAtom(ctx, atom);
            JS_FreeValue(ctx, item);
            goto fail_close;
        }
        switch (magic) {
        case 0:                                     /* countBy */
            {
                int32_t c = 0;
                if (!JS_IsUndefined(cur) && JS_ToInt32(ctx, &c, cur)) {
                    JS_FreeValue(ctx, cur);
                    JS_FreeAtom(ctx, atom);
                    JS_FreeValue(ctx, item);
                    goto fail_close;
                }
                JS_FreeValue(ctx, cur);
                JS_FreeValue(ctx, item);
                if (JS_DefinePropertyValue(ctx, result, atom, JS_NewInt32(ctx, c + 1),
                                           JS_PROP_C_W_E) < 0) {
                    JS_FreeAtom(ctx, atom);
                    goto fail_close;
                }
            }
            break;
        case 1:                                     /* indexBy: last wins */
            JS_FreeValue(ctx, cur);
            if (JS_DefinePropertyValue(ctx, result, atom, item, JS_PROP_C_W_E) < 0) {
                JS_FreeAtom(ctx, atom);
                goto fail_close;
            }
            break;
        case 2:                                     /* groupBy */
            {
                int64_t blen;
                if (!JS_IsArray(ctx, cur)) {
                    JS_FreeValue(ctx, cur);
                    cur = JS_NewArray(ctx);
                    if (JS_IsException(cur) ||
                        JS_SetProperty(ctx, result, atom, JS_DupValue(ctx, cur)) < 0) {
                        JS_FreeValue(ctx, cur);
                        JS_FreeAtom(ctx, atom);
                        JS_FreeValue(ctx, item);
                        goto fail_close;
                    }
                }
                if (js_get_length64(ctx, &blen, cur) ||
                    JS_SetPropertyInt64(ctx, cur, blen, item) < 0) {
                    JS_FreeValue(ctx, cur);
                    JS_FreeAtom(ctx, atom);
                    goto fail_close;
                }
                JS_FreeValue(ctx, cur);
            }
            break;
        default:                                    /* reduceBy */
            {
                JSValue val;
                JSValueConst va[2];
                if (JS_IsUndefined(cur)) {
                    JS_FreeValue(ctx, cur);
                    cur = js_ext_shallow_clone(ctx, seed);
                    if (JS_IsException(cur)) {
                        JS_FreeAtom(ctx, atom);
                        JS_FreeValue(ctx, item);
                        goto fail_close;
                    }
                }
                va[0] = cur;
                va[1] = item;
                val = JS_Call(ctx, fn, JS_UNDEFINED, 2, va);
                JS_FreeValue(ctx, cur);
                JS_FreeValue(ctx, item);
                if (JS_IsException(val)) {
                    JS_FreeAtom(ctx, atom);
                    goto fail_close;
                }
                if (JS_DefinePropertyValue(ctx, result, atom, val, JS_PROP_C_W_E) < 0) {
                    JS_FreeAtom(ctx, atom);
                    goto fail_close;
                }
            }
            break;
        }
        JS_FreeAtom(ctx, atom);
    }
    JS_FreeValue(ctx, method);
    return result;
 fail_close:
    JS_IteratorClose(ctx, this_val, TRUE);
 fail:
    JS_FreeValue(ctx, result);
    JS_FreeValue(ctx, method);
    return JS_EXCEPTION;
}

static const JSCFunctionListEntry js_iterator_ext_funcs[] = {
    JS_CFUNC_MAGIC_DEF("takeWhile", 1, js_iterator_ext_lazy, JS_ITERATOR_HELPER_KIND_TAKE_WHILE ),
    JS_CFUNC_MAGIC_DEF("dropWhile", 1, js_iterator_ext_lazy, JS_ITERATOR_HELPER_KIND_DROP_WHILE ),
    JS_CFUNC_MAGIC_DEF("scan", 2, js_iterator_ext_lazy, JS_ITERATOR_HELPER_KIND_SCAN ),
    JS_CFUNC_MAGIC_DEF("intersperse", 1, js_iterator_ext_lazy, JS_ITERATOR_HELPER_KIND_INTERSPERSE ),
    JS_CFUNC_MAGIC_DEF("compact", 0, js_iterator_ext_lazy, JS_ITERATOR_HELPER_KIND_COMPACT ),
    JS_CFUNC_MAGIC_DEF("dropRepeats", 0, js_iterator_ext_lazy, JS_ITERATOR_HELPER_KIND_DROP_REPEATS ),
    JS_CFUNC_MAGIC_DEF("dropRepeatsWith", 1, js_iterator_ext_lazy, JS_ITERATOR_HELPER_KIND_DROP_REPEATS_WITH ),
    JS_CFUNC_MAGIC_DEF("dropRepeatsBy", 1, js_iterator_ext_lazy, JS_ITERATOR_HELPER_KIND_DROP_REPEATS_BY ),
    JS_CFUNC_MAGIC_DEF("aperture", 1, js_iterator_ext_lazy, JS_ITERATOR_HELPER_KIND_APERTURE ),
    JS_CFUNC_MAGIC_DEF("splitEvery", 1, js_iterator_ext_lazy, JS_ITERATOR_HELPER_KIND_SPLIT_EVERY ),
    JS_CFUNC_MAGIC_DEF("zipWith", 2, js_iterator_ext_lazy, JS_ITERATOR_HELPER_KIND_ZIP_WITH ),
    JS_CFUNC_MAGIC_DEF("pluck", 1, js_iterator_ext_lazy, JS_ITERATOR_HELPER_KIND_PLUCK ),
    JS_CFUNC_MAGIC_DEF("init", 0, js_iterator_ext_lazy, JS_ITERATOR_HELPER_KIND_INIT ),
    JS_CFUNC_MAGIC_DEF("tail", 0, js_iterator_ext_lazy, JS_ITERATOR_HELPER_KIND_TAIL ),
    JS_CFUNC_MAGIC_DEF("unique", 1, js_iterator_ext_lazy, JS_ITERATOR_HELPER_KIND_UNIQUE ),
    JS_ALIAS_DEF("uniq", "unique" ),
    JS_ALIAS_DEF("uniqBy", "unique" ),
    JS_CFUNC_DEF("tee", 1, js_iterator_ext_tee ),
    JS_CFUNC_MAGIC_DEF("sum", 0, js_iterator_ext_stat, ITX_SUM ),
    JS_CFUNC_MAGIC_DEF("average", 0, js_iterator_ext_stat, ITX_AVERAGE ),
    JS_ALIAS_DEF("mean", "average" ),
    JS_CFUNC_MAGIC_DEF("product", 0, js_iterator_ext_stat, ITX_PRODUCT ),
    JS_CFUNC_MAGIC_DEF("min", 1, js_iterator_ext_minmax, 0 ),
    JS_CFUNC_MAGIC_DEF("max", 1, js_iterator_ext_minmax, 1 ),
    JS_CFUNC_MAGIC_DEF("none", 1, js_iterator_ext_quantify, 0 ),
    JS_CFUNC_MAGIC_DEF("any", 1, js_iterator_ext_quantify, 1 ),
    JS_CFUNC_MAGIC_DEF("all", 1, js_iterator_ext_quantify, 2 ),
    JS_CFUNC_MAGIC_DEF("count", 1, js_iterator_ext_quantify, 3 ),
    JS_CFUNC_MAGIC_DEF("last", 0, js_iterator_ext_pick, 0 ),
    JS_CFUNC_MAGIC_DEF("first", 0, js_iterator_ext_pick, 1 ),
    JS_ALIAS_DEF("head", "first" ),
    JS_CFUNC_MAGIC_DEF("nth", 1, js_iterator_ext_pick, 2 ),
    JS_CFUNC_MAGIC_DEF("findIndex", 1, js_iterator_ext_pick, 3 ),
    JS_CFUNC_MAGIC_DEF("countBy", 1, js_iterator_ext_collect, 0 ),
    JS_CFUNC_MAGIC_DEF("indexBy", 1, js_iterator_ext_collect, 1 ),
    JS_CFUNC_MAGIC_DEF("groupBy", 1, js_iterator_ext_collect, 2 ),
    JS_CFUNC_MAGIC_DEF("reduceBy", 3, js_iterator_ext_collect, 3 ),
};
