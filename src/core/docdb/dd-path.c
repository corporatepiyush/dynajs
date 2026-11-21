#include <stdlib.h>
#include "dd.h"

int dd_path_parse(dd_path* p, const char* s, size_t n, dd_err* e)
{
    size_t i, start = 0;
    int k = 0, parts = 1;
    memset(p, 0, sizeof *p);
    if (n == 0)
        return dd_fail(e, DD_E_BADVALUE, "FieldPath cannot be constructed with empty string");
    for (i = 0; i < n; i++)
        if (s[i] == '.')
            parts++;
    if (parts > DD_MAX_PATH_PARTS)
        return dd_fail(e, DD_E_BADVALUE, "FieldPath is too long (more than %d parts)", DD_MAX_PATH_PARTS);
    p->text = malloc(n + 1);
    p->part = malloc((size_t)parts * sizeof *p->part);
    p->plen = malloc((size_t)parts * sizeof *p->plen);
    p->numeric = malloc((size_t)parts);
    p->index = malloc((size_t)parts * sizeof *p->index);
    if (!p->text || !p->part || !p->plen || !p->numeric || !p->index) {
        dd_path_free(p);
        return dd_fail(e, DD_E_OOM, "out of memory");
    }
    memcpy(p->text, s, n);
    p->text[n] = 0;
    p->tlen = n;
    for (i = 0; i <= n; i++) {
        if (i == n || s[i] == '.') {
            size_t len = i - start, j;
            uint64_t v = 0;
            if (len == 0) {
                dd_path_free(p);
                return dd_fail(e, DD_E_BADVALUE, "FieldPath field names may not be empty strings");
            }
            p->part[k] = p->text + start;
            p->plen[k] = (uint32_t)len;
            p->numeric[k] = len <= 9 && (len == 1 || s[start] != '0');
            for (j = 0; j < len && p->numeric[k]; j++) {
                if (s[start + j] < '0' || s[start + j] > '9')
                    p->numeric[k] = 0;
                else
                    v = v * 10 + (uint64_t)(s[start + j] - '0');
            }
            p->index[k] = (uint32_t)v;
            k++;
            start = i + 1;
        }
    }
    p->n = k;
    return 0;
}

void dd_path_free(dd_path* p)
{
    free(p->text);
    free(p->part);
    free(p->plen);
    free(p->numeric);
    free(p->index);
    memset(p, 0, sizeof *p);
}

static int dd_find_n(const uint8_t* doc, const char* k, uint32_t kl, dyn_bson_val* out)
{
    return dyn_bson_find(doc, k, kl, out);
}

static int dd_array_at(const uint8_t* arr, uint32_t idx, dyn_bson_val* out)
{
    dyn_bson_iter it;
    uint32_t i = 0;
    dyn_bson_iter_init(&it, arr, dyn_bson_rd32(arr));
    while (dyn_bson_iter_next(&it) > 0) {
        if (i == idx) {
            *out = dyn_bson_iter_val(&it);
            return 1;
        }
        i++;
    }
    return 0;
}

static const dyn_bson_val dd_missing = { 0, NULL, 0 };

static int walk_value(dyn_bson_val v, const dd_path* p, int i, dd_visit_fn fn, void* ud, int32_t aidx);

static int walk_array(const uint8_t* arr, const dd_path* p, int i, dd_visit_fn fn, void* ud, int32_t aidx)
{
    dyn_bson_iter it;
    int32_t k = 0;
    int rc, any = 0;
    if (p->numeric[i]) {
        dyn_bson_val el;
        if (dd_array_at(arr, p->index[i], &el)) {
            any = 1;
            rc = walk_value(el, p, i + 1, fn, ud, aidx);
            if (rc)
                return rc;
        }
    }
    dyn_bson_iter_init(&it, arr, dyn_bson_rd32(arr));
    while (dyn_bson_iter_next(&it) > 0) {
        if (it.type == DYN_BSON_DOC) {
            dyn_bson_val f;
            int32_t ai = aidx < 0 ? k : aidx;
            if (dd_find_n(it.val, p->part[i], p->plen[i], &f)) {
                any = 1;
                rc = walk_value(f, p, i + 1, fn, ud, ai);
            } else if (!p->numeric[i]) {
                rc = fn(ud, dd_missing, 0, ai);
            } else {
                rc = 0;
            }
            if (rc)
                return rc;
        } else if (!p->numeric[i]) {
            rc = fn(ud, dd_missing, 0, aidx < 0 ? k : aidx);
            if (rc)
                return rc;
        }
        k++;
    }
    if (!any && (k == 0 || p->numeric[i]))
        return fn(ud, dd_missing, 0, aidx);
    return 0;
}

static int walk_value(dyn_bson_val v, const dd_path* p, int i, dd_visit_fn fn, void* ud, int32_t aidx)
{
    int rc;
    if (i == p->n) {
        rc = fn(ud, v, 0, aidx);
        if (rc)
            return rc;
        if (v.type == DYN_BSON_ARRAY) {
            dyn_bson_iter it;
            int32_t k = 0;
            dyn_bson_iter_init(&it, v.val, v.vlen);
            while (dyn_bson_iter_next(&it) > 0) {
                rc = fn(ud, dyn_bson_iter_val(&it), 1, aidx < 0 ? k : aidx);
                if (rc)
                    return rc;
                k++;
            }
        }
        return 0;
    }
    if (v.type == DYN_BSON_DOC) {
        dyn_bson_val f;
        if (dd_find_n(v.val, p->part[i], p->plen[i], &f))
            return walk_value(f, p, i + 1, fn, ud, aidx);
        return fn(ud, dd_missing, 0, aidx);
    }
    if (v.type == DYN_BSON_ARRAY)
        return walk_array(v.val, p, i, fn, ud, aidx);
    return fn(ud, dd_missing, 0, aidx);
}

int dd_path_walk(const uint8_t* doc, const dd_path* p, dd_visit_fn fn, void* ud)
{
    dyn_bson_val root;
    root.type = DYN_BSON_DOC;
    root.val = doc;
    root.vlen = dyn_bson_rd32(doc);
    return walk_value(root, p, 0, fn, ud, -1);
}

int dd_path_get_exact(const uint8_t* doc, const dd_path* p, dyn_bson_val* out)
{
    dyn_bson_val cur;
    int i;
    cur.type = DYN_BSON_DOC;
    cur.val = doc;
    cur.vlen = dyn_bson_rd32(doc);
    for (i = 0; i < p->n; i++) {
        if (cur.type == DYN_BSON_DOC) {
            if (!dd_find_n(cur.val, p->part[i], p->plen[i], &cur))
                return 0;
        } else if (cur.type == DYN_BSON_ARRAY && p->numeric[i]) {
            if (!dd_array_at(cur.val, p->index[i], &cur))
                return 0;
        } else {
            return 0;
        }
    }
    *out = cur;
    return 1;
}

int dd_lookup_dotted(const uint8_t* doc, const char* key, size_t klen, dyn_bson_val* out)
{
    dyn_bson_val cur;
    size_t start = 0, i;
    cur.type = DYN_BSON_DOC;
    cur.val = doc;
    cur.vlen = dyn_bson_rd32(doc);
    for (i = 0; i <= klen; i++) {
        if (i == klen || key[i] == '.') {
            if (cur.type != DYN_BSON_DOC)
                return 0;
            if (!dd_find_n(cur.val, key + start, (uint32_t)(i - start), &cur))
                return 0;
            start = i + 1;
        }
    }
    *out = cur;
    return 1;
}
