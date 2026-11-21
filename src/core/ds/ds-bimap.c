#include "ds-internal.h"
#include "ds-bimap.h"

typedef struct {
    ds_key key;
    ds_key val;
} ds_bimap_rec;

struct dyn_bimap {
    ds_bimap_rec* rec;
    uint32_t len, cap;
    dyn_ix fwd;
    dyn_ix rev;
};

dyn_bimap_t* dyn_bimap_new(void)
{
    return (dyn_bimap_t*)ds_calloc(1, sizeof(dyn_bimap_t));
}

void dyn_bimap_clear(dyn_bimap_t* b)
{
    uint32_t i;
    for (i = 0; i < b->len; i++) {
        DYN_DS_FREE(b->rec[i].key.p);
        DYN_DS_FREE(b->rec[i].val.p);
    }
    b->len = 0;
    ds_ix_free(&b->fwd);
    ds_ix_free(&b->rev);
}

void dyn_bimap_free(dyn_bimap_t* b)
{
    if (!b)
        return;
    dyn_bimap_clear(b);
    DYN_DS_FREE(b->rec);
    DYN_DS_FREE(b);
}

static void ds_bimap_erase(dyn_bimap_t* b, uint32_t idx)
{
    uint32_t fp = ds_ix_find(&b->fwd, b->rec[idx].key.h, b->rec[idx].key.p,
        b->rec[idx].key.n);
    uint32_t rp = ds_ix_find(&b->rev, b->rec[idx].val.h, b->rec[idx].val.p,
        b->rec[idx].val.n);
    if (fp != UINT32_MAX)
        ds_ix_erase(&b->fwd, fp);
    if (rp != UINT32_MAX)
        ds_ix_erase(&b->rev, rp);
    DYN_DS_FREE(b->rec[idx].key.p);
    DYN_DS_FREE(b->rec[idx].val.p);
    b->len--;
    if (idx != b->len) {
        b->rec[idx] = b->rec[b->len];
        ds_ix_move(&b->fwd, b->rec[idx].key.h, b->rec[idx].key.p,
            b->rec[idx].key.n, idx);
        ds_ix_move(&b->rev, b->rec[idx].val.h, b->rec[idx].val.p,
            b->rec[idx].val.n, idx);
    }
}

int dyn_bimap_put(dyn_bimap_t* b, const char* k, size_t kn,
    const char* v, size_t vn, int force)
{
    uint64_t kh = ds_ix_hash(k, kn), vh = ds_ix_hash(v, vn);
    uint32_t kp = ds_ix_find(&b->fwd, kh, k, kn);
    uint32_t vp = ds_ix_find(&b->rev, vh, v, vn);
    ds_bimap_rec* r;

    if (kp != UINT32_MAX && vp != UINT32_MAX && b->fwd.slot[kp].idx == b->rev.slot[vp].idx)
        return 0;
    if (vp != UINT32_MAX) {
        if (!force)
            return DYN_BIMAP_VALUE_TAKEN;
        if (kp != UINT32_MAX) {
            ds_key nv;
            if (ds_ix_reserve(&b->rev) < 0)
                return -1;
            if (ds_key_init(&nv, v, vn, vh) < 0)
                return -1;
            vp = ds_ix_find(&b->rev, vh, v, vn);
            ds_bimap_erase(b, b->rev.slot[vp].idx);
            kp = ds_ix_find(&b->fwd, kh, k, kn);
            {
                uint32_t idx = b->fwd.slot[kp].idx;
                uint32_t op = ds_ix_find(&b->rev, b->rec[idx].val.h,
                    b->rec[idx].val.p, b->rec[idx].val.n);
                if (op != UINT32_MAX)
                    ds_ix_erase(&b->rev, op);
                DYN_DS_FREE(b->rec[idx].val.p);
                b->rec[idx].val = nv;
                ds_ix_insert(&b->rev, vh, nv.p, vn, idx);
            }
            return 0;
        }
        r = NULL;
        if (ds_reserve((void**)&b->rec, &b->cap, b->len + 1,
                sizeof(*b->rec))
            < 0)
            return -1;
        {
            ds_bimap_rec tmp;
            tmp.key.p = NULL;
            tmp.val.p = NULL;
            if (ds_key_init(&tmp.key, k, kn, kh) < 0)
                return -1;
            if (ds_key_init(&tmp.val, v, vn, vh) < 0) {
                DYN_DS_FREE(tmp.key.p);
                return -1;
            }
            if (ds_ix_reserve(&b->fwd) < 0 || ds_ix_reserve(&b->rev) < 0) {
                DYN_DS_FREE(tmp.key.p);
                DYN_DS_FREE(tmp.val.p);
                return -1;
            }
            vp = ds_ix_find(&b->rev, vh, v, vn);
            ds_bimap_erase(b, b->rev.slot[vp].idx);
            b->rec[b->len] = tmp;
            ds_ix_insert(&b->fwd, kh, tmp.key.p, kn, b->len);
            ds_ix_insert(&b->rev, vh, tmp.val.p, vn, b->len);
            b->len++;
        }
        return 0;
    }
    if (kp != UINT32_MAX) {
        uint32_t idx = b->fwd.slot[kp].idx;
        uint32_t op = ds_ix_find(&b->rev, b->rec[idx].val.h, b->rec[idx].val.p,
            b->rec[idx].val.n);
        ds_key nv;
        if (ds_ix_reserve(&b->rev) < 0)
            return -1;
        if (ds_key_init(&nv, v, vn, vh) < 0)
            return -1;
        op = ds_ix_find(&b->rev, b->rec[idx].val.h, b->rec[idx].val.p,
            b->rec[idx].val.n);
        if (op != UINT32_MAX)
            ds_ix_erase(&b->rev, op);
        DYN_DS_FREE(b->rec[idx].val.p);
        b->rec[idx].val = nv;
        ds_ix_insert(&b->rev, vh, nv.p, vn, idx);
        return 0;
    }
    if (ds_reserve((void**)&b->rec, &b->cap, b->len + 1, sizeof(*b->rec)) < 0)
        return -1;
    r = &b->rec[b->len];
    if (ds_key_init(&r->key, k, kn, kh) < 0)
        return -1;
    if (ds_key_init(&r->val, v, vn, vh) < 0) {
        DYN_DS_FREE(r->key.p);
        return -1;
    }
    if (ds_ix_insert(&b->fwd, kh, r->key.p, kn, b->len) < 0) {
        DYN_DS_FREE(r->key.p);
        DYN_DS_FREE(r->val.p);
        return -1;
    }
    if (ds_ix_insert(&b->rev, vh, r->val.p, vn, b->len) < 0) {
        uint32_t fp = ds_ix_find(&b->fwd, kh, r->key.p, kn);
        if (fp != UINT32_MAX)
            ds_ix_erase(&b->fwd, fp);
        DYN_DS_FREE(r->key.p);
        DYN_DS_FREE(r->val.p);
        return -1;
    }
    b->len++;
    return 0;
}

const char* dyn_bimap_get(const dyn_bimap_t* b, const char* k, size_t kn,
    size_t* vn)
{
    uint32_t pos = ds_ix_find(&b->fwd, ds_ix_hash(k, kn), k, kn);
    if (pos == UINT32_MAX)
        return NULL;
    *vn = b->rec[b->fwd.slot[pos].idx].val.n;
    return b->rec[b->fwd.slot[pos].idx].val.p;
}

const char* dyn_bimap_key_for(const dyn_bimap_t* b, const char* v, size_t vn,
    size_t* kn)
{
    uint32_t pos = ds_ix_find(&b->rev, ds_ix_hash(v, vn), v, vn);
    if (pos == UINT32_MAX)
        return NULL;
    *kn = b->rec[b->rev.slot[pos].idx].key.n;
    return b->rec[b->rev.slot[pos].idx].key.p;
}

int dyn_bimap_remove(dyn_bimap_t* b, const char* k, size_t kn)
{
    uint32_t pos = ds_ix_find(&b->fwd, ds_ix_hash(k, kn), k, kn);
    if (pos == UINT32_MAX)
        return 0;
    ds_bimap_erase(b, b->fwd.slot[pos].idx);
    return 1;
}

int dyn_bimap_remove_value(dyn_bimap_t* b, const char* v, size_t vn)
{
    uint32_t pos = ds_ix_find(&b->rev, ds_ix_hash(v, vn), v, vn);
    if (pos == UINT32_MAX)
        return 0;
    ds_bimap_erase(b, b->rev.slot[pos].idx);
    return 1;
}

uint32_t dyn_bimap_size(const dyn_bimap_t* b) { return b->len; }

int dyn_bimap_at(const dyn_bimap_t* b, uint32_t i, const char** k, size_t* kn,
    const char** v, size_t* vn)
{
    if (i >= b->len)
        return 0;
    *k = b->rec[i].key.p;
    *kn = b->rec[i].key.n;
    *v = b->rec[i].val.p;
    *vn = b->rec[i].val.n;
    return 1;
}
