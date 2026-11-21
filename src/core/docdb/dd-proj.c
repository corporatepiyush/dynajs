#include <stdlib.h>
#include "dd.h"

static void pn_free(dd_projnode* n)
{
    uint32_t i;
    for (i = 0; i < n->nkids; i++)
        pn_free(&n->kids[i]);
    free(n->kids);
    free(n->name);
}

void dd_proj_free(dd_proj* p)
{
    pn_free(&p->root);
    memset(p, 0, sizeof *p);
}

static dd_projnode* pn_child(dd_projnode* n, const char* s, uint32_t len, int create)
{
    uint32_t i;
    dd_projnode* k;
    for (i = 0; i < n->nkids; i++)
        if (n->kids[i].nlen == len && memcmp(n->kids[i].name, s, len) == 0)
            return &n->kids[i];
    if (!create)
        return NULL;
    if (n->nkids == n->capkids) {
        uint32_t nc = n->capkids ? n->capkids * 2 : 4;
        dd_projnode* nk = realloc(n->kids, (size_t)nc * sizeof *nk);
        if (!nk)
            return NULL;
        n->kids = nk;
        n->capkids = nc;
    }
    k = &n->kids[n->nkids];
    memset(k, 0, sizeof *k);
    k->name = malloc(len + 1u);
    if (!k->name)
        return NULL;
    memcpy(k->name, s, len);
    k->name[len] = 0;
    k->nlen = len;
    n->nkids++;
    return k;
}

static const dd_projnode* pn_get(const dd_projnode* n, const char* s, uint32_t len)
{
    uint32_t i;
    for (i = 0; i < n->nkids; i++)
        if (n->kids[i].nlen == len && memcmp(n->kids[i].name, s, len) == 0)
            return &n->kids[i];
    return NULL;
}

static int truthy(dyn_bson_val v, int* out)
{
    if (dyn_bson_is_number(v.type)) {
        *out = dyn_bson_num_f64(v) != 0;
        return 0;
    }
    if (v.type == DYN_BSON_BOOL) {
        *out = v.val[0] != 0;
        return 0;
    }
    return -1;
}

static int add_path(dd_proj* p, const char* key, size_t klen, dd_err* e)
{
    dd_projnode* n = &p->root;
    size_t i = 0;
    if (klen == 0 || key[0] == '$')
        return dd_fail(e, DD_E_BADVALUE, "projection field name is not valid: '%.*s'", (int)klen, key);
    while (i <= klen) {
        size_t j = i;
        dd_projnode* k;
        while (j < klen && key[j] != '.')
            j++;
        if (j == i)
            return dd_fail(e, DD_E_BADVALUE, "projection path has an empty component: '%.*s'", (int)klen, key);
        if (n->leaf)
            return dd_fail(e, 31250, "Path collision at %.*s", (int)klen, key);
        k = pn_child(n, key + i, (uint32_t)(j - i), 1);
        if (!k)
            return dd_fail(e, DD_E_OOM, "out of memory");
        if (j >= klen) {
            if (k->leaf || k->nkids)
                return dd_fail(e, 31250, "Path collision at %.*s", (int)klen, key);
            k->leaf = 1;
            return 0;
        }
        n = k;
        i = j + 1;
    }
    return 0;
}

int dd_proj_parse(dd_proj* p, const uint8_t* spec, dd_err* e)
{
    dyn_bson_iter it;
    int incl = -1, id_seen = 0;
    memset(p, 0, sizeof *p);
    p->id_mode = 1;
    dyn_bson_iter_init(&it, spec, dyn_bson_rd32(spec));
    while (dyn_bson_iter_next(&it) > 0) {
        int t;
        if (truthy(dyn_bson_iter_val(&it), &t) < 0) {
            dd_proj_free(p);
            return dd_fail(e, DD_E_BADVALUE, "unsupported projection value for '%.*s': only 1/0/true/false",
                (int)it.klen, it.key);
        }
        if (it.klen == 3 && memcmp(it.key, "_id", 3) == 0) {
            p->id_mode = t;
            id_seen = 1;
            continue;
        }
        if (incl >= 0 && incl != t) {
            dd_proj_free(p);
            return dd_fail(e, 31254, "Cannot do %s on field %.*s in %s projection", t ? "inclusion" : "exclusion",
                (int)it.klen, it.key, incl ? "inclusion" : "exclusion");
        }
        incl = t;
        if (add_path(p, it.key, it.klen, e) < 0) {
            dd_proj_free(p);
            return -1;
        }
    }
    if (incl < 0 && id_seen && p->id_mode)
        incl = 1;
    if (incl < 0) {
        p->inclusion = 0;
        if (p->id_mode) {
            p->empty = 1;
            return 0;
        }
        if (add_path(p, "_id", 3, e) < 0) {
            dd_proj_free(p);
            return -1;
        }
        return 0;
    }
    p->inclusion = incl;
    if (incl && p->id_mode) {
        if (!pn_child(&p->root, "_id", 3, 0) && add_path(p, "_id", 3, e) < 0) {
            dd_proj_free(p);
            return -1;
        }
    } else if (!incl && !p->id_mode) {
        if (add_path(p, "_id", 3, e) < 0) {
            dd_proj_free(p);
            return -1;
        }
    }
    return 0;
}

static void inc_doc(const dd_projnode* n, const uint8_t* doc, dyn_bson_w* w);
static void exc_doc(const dd_projnode* n, const uint8_t* doc, dyn_bson_w* w);

static void inc_array(const dd_projnode* n, dyn_bson_val v, dyn_bson_w* w)
{
    dyn_bson_iter it;
    uint32_t idx = 0;
    dyn_bson_iter_init(&it, v.val, v.vlen);
    while (dyn_bson_iter_next(&it) > 0) {
        if (it.type == DYN_BSON_DOC) {
            dyn_bson_w_index_key(w, DYN_BSON_DOC, idx++);
            inc_doc(n, it.val, w);
        } else if (it.type == DYN_BSON_ARRAY) {
            dyn_bson_w_index_key(w, DYN_BSON_ARRAY, idx++);
            dyn_bson_w_doc_begin(w);
            inc_array(n, dyn_bson_iter_val(&it), w);
            dyn_bson_w_doc_end(w);
        }
    }
}

static void inc_doc(const dd_projnode* n, const uint8_t* doc, dyn_bson_w* w)
{
    dyn_bson_iter it;
    dyn_bson_w_doc_begin(w);
    dyn_bson_iter_init(&it, doc, dyn_bson_rd32(doc));
    while (dyn_bson_iter_next(&it) > 0) {
        const dd_projnode* k = pn_get(n, it.key, (uint32_t)it.klen);
        dyn_bson_val v;
        if (!k)
            continue;
        v = dyn_bson_iter_val(&it);
        if (k->leaf) {
            dyn_bson_w_value(w, it.key, it.klen, v);
        } else if (v.type == DYN_BSON_DOC) {
            dyn_bson_w_key(w, DYN_BSON_DOC, it.key, it.klen);
            inc_doc(k, v.val, w);
        } else if (v.type == DYN_BSON_ARRAY) {
            dyn_bson_w_sub_begin(w, DYN_BSON_ARRAY, it.key, it.klen);
            inc_array(k, v, w);
            dyn_bson_w_sub_end(w);
        }
    }
    dyn_bson_w_doc_end(w);
}

static void exc_array(const dd_projnode* n, dyn_bson_val v, dyn_bson_w* w)
{
    dyn_bson_iter it;
    dyn_bson_iter_init(&it, v.val, v.vlen);
    while (dyn_bson_iter_next(&it) > 0) {
        if (it.type == DYN_BSON_DOC) {
            dyn_bson_w_key(w, DYN_BSON_DOC, it.key, it.klen);
            exc_doc(n, it.val, w);
        } else if (it.type == DYN_BSON_ARRAY) {
            dyn_bson_w_sub_begin(w, DYN_BSON_ARRAY, it.key, it.klen);
            exc_array(n, dyn_bson_iter_val(&it), w);
            dyn_bson_w_sub_end(w);
        } else {
            dyn_bson_w_value(w, it.key, it.klen, dyn_bson_iter_val(&it));
        }
    }
}

static void exc_doc(const dd_projnode* n, const uint8_t* doc, dyn_bson_w* w)
{
    dyn_bson_iter it;
    dyn_bson_w_doc_begin(w);
    dyn_bson_iter_init(&it, doc, dyn_bson_rd32(doc));
    while (dyn_bson_iter_next(&it) > 0) {
        const dd_projnode* k = pn_get(n, it.key, (uint32_t)it.klen);
        dyn_bson_val v = dyn_bson_iter_val(&it);
        if (k && k->leaf)
            continue;
        if (k && v.type == DYN_BSON_DOC) {
            dyn_bson_w_key(w, DYN_BSON_DOC, it.key, it.klen);
            exc_doc(k, v.val, w);
        } else if (k && v.type == DYN_BSON_ARRAY) {
            dyn_bson_w_sub_begin(w, DYN_BSON_ARRAY, it.key, it.klen);
            exc_array(k, v, w);
            dyn_bson_w_sub_end(w);
        } else {
            dyn_bson_w_value(w, it.key, it.klen, v);
        }
    }
    dyn_bson_w_doc_end(w);
}

int dd_proj_apply(const dd_proj* p, const uint8_t* doc, dyn_bson_w* w)
{
    if (p->empty)
        dyn_bson_w_bytes(w, doc, dyn_bson_rd32(doc));
    else if (p->inclusion)
        inc_doc(&p->root, doc, w);
    else
        exc_doc(&p->root, doc, w);
    return w->oom ? -1 : 0;
}
