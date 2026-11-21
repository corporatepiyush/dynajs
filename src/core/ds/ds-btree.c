#include "ds-internal.h"
#include "ds-heap.h"

typedef struct ds_btree_node {
    struct ds_btree_node* parent;
    uint16_t n;
    uint8_t leaf;
    double key[DYN_BTREE_ORDER];
    union {
        struct {
            dyn_cell_t val[DYN_BTREE_ORDER];
            struct ds_btree_node *next, *prev;
        } l;
        struct {
            struct ds_btree_node* kid[DYN_BTREE_ORDER + 1];
        } i;
    } u;
} ds_btree_node;

struct dyn_btree {
    ds_btree_node* root;
    ds_btree_node* head;
    uint32_t count;
};

#define DYN_BTREE_MAX_CHAIN 16

static uint32_t ds_btree_lower(const ds_btree_node* nd, double k)
{
    uint32_t i = 0;
    while (i < nd->n && nd->key[i] < k)
        i++;
    return i;
}

static ds_btree_node* ds_btree_node_new(int leaf)
{
    ds_btree_node* nd = (ds_btree_node*)ds_calloc(1, sizeof(*nd));
    if (nd)
        nd->leaf = (uint8_t)leaf;
    return nd;
}

dyn_btree_t* dyn_btree_new(void)
{
    dyn_btree_t* t = (dyn_btree_t*)ds_calloc(1, sizeof(*t));
    if (!t)
        return NULL;
    t->root = ds_btree_node_new(1);
    if (!t->root) {
        DYN_DS_FREE(t);
        return NULL;
    }
    t->head = t->root;
    return t;
}

static void dyn_btree_free_kids(ds_btree_node* nd, dyn_cell_free_fn fn, void* ud)
{
    if (nd->leaf) {
        uint32_t i;
        if (fn)
            for (i = 0; i < nd->n; i++)
                fn(ud, &nd->u.l.val[i]);
    } else {
        uint32_t i;
        for (i = 0; i <= nd->n; i++)
            if (nd->u.i.kid[i])
                dyn_btree_free_kids(nd->u.i.kid[i], fn, ud);
    }
    DYN_DS_FREE(nd);
}

void dyn_btree_free(dyn_btree_t* t, dyn_cell_free_fn fn, void* ud)
{
    ds_btree_node** st = NULL;
    uint32_t len = 0, cap = 0;
    if (!t)
        return;
    if (t->root && ds_reserve((void**)&st, &cap, 1, sizeof(*st)) == 0)
        st[len++] = t->root;
    while (len) {
        ds_btree_node* nd = st[--len];
        if (nd->leaf) {
            uint32_t i;
            if (fn)
                for (i = 0; i < nd->n; i++)
                    fn(ud, &nd->u.l.val[i]);
        } else {
            uint32_t i;
            for (i = 0; i <= nd->n; i++) {
                if (!nd->u.i.kid[i])
                    continue;
                if (ds_reserve((void**)&st, &cap, len + 1,
                        sizeof(*st))
                    < 0) {
                    dyn_btree_free_kids(nd->u.i.kid[i], fn, ud);
                    continue;
                }
                st[len++] = nd->u.i.kid[i];
            }
        }
        DYN_DS_FREE(nd);
    }
    DYN_DS_FREE(st);
    DYN_DS_FREE(t);
}

uint32_t dyn_btree_size(const dyn_btree_t* t) { return t->count; }

static ds_btree_node* ds_btree_find_leaf(const dyn_btree_t* t, double k)
{
    ds_btree_node* nd = t->root;
    while (!nd->leaf) {
        uint32_t i = ds_btree_lower(nd, k);
        if (i < nd->n && nd->key[i] == k)
            i++;
        nd = nd->u.i.kid[i];
    }
    return nd;
}

const dyn_cell_t* dyn_btree_get(const dyn_btree_t* t, double k)
{
    ds_btree_node* nd = ds_btree_find_leaf(t, k);
    uint32_t i = ds_btree_lower(nd, k);
    if (i < nd->n && nd->key[i] == k)
        return &nd->u.l.val[i];
    return NULL;
}

static void ds_btree_ins_internal(ds_btree_node* nd, uint32_t at, double key,
    ds_btree_node* kid)
{
    uint32_t j;
    for (j = nd->n; j > at; j--) {
        nd->key[j] = nd->key[j - 1];
        nd->u.i.kid[j + 1] = nd->u.i.kid[j];
    }
    nd->key[at] = key;
    nd->u.i.kid[at + 1] = kid;
    nd->n++;
    kid->parent = nd;
}

static int ds_btree_split(dyn_btree_t* t, ds_btree_node* nd)
{
    ds_btree_node* pre[DYN_BTREE_MAX_CHAIN];
    ds_btree_node* new_root = NULL;
    uint32_t mid = DYN_BTREE_ORDER / 2, j, nn = 0, next_right = 0;
    ds_btree_node* p;

    if (nd->n != DYN_BTREE_ORDER)
        return 0;
    p = nd;
    for (;;) {
        if (nn >= DYN_BTREE_MAX_CHAIN)
            return -1;
        pre[nn] = ds_btree_node_new(p->leaf);
        if (!pre[nn])
            goto oom;
        nn++;
        if (!p->parent)
            break;
        p = p->parent;
        if (p->n != DYN_BTREE_ORDER - 1)
            break;
    }
    if (!p->parent) {
        new_root = ds_btree_node_new(0);
        if (!new_root)
            goto oom;
    }

    while (nd->n == DYN_BTREE_ORDER) {
        ds_btree_node *right = pre[next_right++], *par;
        double up;
        if (nd->leaf) {
            right->n = (uint16_t)(nd->n - mid);
            for (j = 0; j < right->n; j++) {
                right->key[j] = nd->key[mid + j];
                right->u.l.val[j] = nd->u.l.val[mid + j];
            }
            nd->n = (uint16_t)mid;
            right->u.l.next = nd->u.l.next;
            right->u.l.prev = nd;
            if (nd->u.l.next)
                nd->u.l.next->u.l.prev = right;
            nd->u.l.next = right;
            up = right->key[0];
        } else {
            right->n = (uint16_t)(nd->n - mid - 1);
            for (j = 0; j < right->n; j++)
                right->key[j] = nd->key[mid + 1 + j];
            for (j = 0; j <= right->n; j++) {
                right->u.i.kid[j] = nd->u.i.kid[mid + 1 + j];
                right->u.i.kid[j]->parent = right;
            }
            up = nd->key[mid];
            nd->n = (uint16_t)mid;
        }
        par = nd->parent;
        if (par == NULL) {
            par = new_root;
            par->n = 1;
            par->key[0] = up;
            par->u.i.kid[0] = nd;
            par->u.i.kid[1] = right;
            nd->parent = par;
            right->parent = par;
            t->root = par;
            return 0;
        }
        ds_btree_ins_internal(par, ds_btree_lower(par, up), up, right);
        nd = par;
    }
    while (next_right < nn)
        DYN_DS_FREE(pre[next_right++]);
    DYN_DS_FREE(new_root);
    return 0;
oom:
    while (nn > 0)
        DYN_DS_FREE(pre[--nn]);
    DYN_DS_FREE(new_root);
    return -1;
}

int dyn_btree_set(dyn_btree_t* t, double k, const dyn_cell_t* v,
    dyn_cell_t* old, int* replaced)
{
    ds_btree_node* nd = ds_btree_find_leaf(t, k);
    uint32_t i = ds_btree_lower(nd, k), j;

    if (i < nd->n && nd->key[i] == k) {
        *old = nd->u.l.val[i];
        nd->u.l.val[i] = *v;
        *replaced = 1;
        return 0;
    }
    *replaced = 0;
    for (j = nd->n; j > i; j--) {
        nd->key[j] = nd->key[j - 1];
        nd->u.l.val[j] = nd->u.l.val[j - 1];
    }
    nd->key[i] = k;
    nd->u.l.val[i] = *v;
    nd->n++;
    t->count++;
    if (ds_btree_split(t, nd) < 0) {
        for (j = i; j + 1 < nd->n; j++) {
            nd->key[j] = nd->key[j + 1];
            nd->u.l.val[j] = nd->u.l.val[j + 1];
        }
        nd->n--;
        t->count--;
        return -1;
    }
    return 0;
}

int dyn_btree_del(dyn_btree_t* t, double k, dyn_cell_t* out)
{
    ds_btree_node* nd = ds_btree_find_leaf(t, k);
    uint32_t i = ds_btree_lower(nd, k), j;

    if (i >= nd->n || nd->key[i] != k)
        return 0;
    *out = nd->u.l.val[i];
    for (j = i; j + 1 < nd->n; j++) {
        nd->key[j] = nd->key[j + 1];
        nd->u.l.val[j] = nd->u.l.val[j + 1];
    }
    nd->n--;
    t->count--;
    return 1;
}

static ds_btree_node* ds_btree_leftmost(const dyn_btree_t* t)
{
    ds_btree_node* nd = t->root;
    while (!nd->leaf)
        nd = nd->u.i.kid[0];
    return nd;
}

int dyn_btree_iter_begin(const dyn_btree_t* t, dyn_btree_iter* it)
{
    ds_btree_node* nd = ds_btree_leftmost(t);
    while (nd && nd->n == 0)
        nd = nd->u.l.next;
    it->leaf = nd;
    it->i = 0;
    return nd != NULL;
}

int dyn_btree_iter_seek(const dyn_btree_t* t, double k, dyn_btree_iter* it)
{
    ds_btree_node* nd = ds_btree_find_leaf(t, k);
    uint32_t i = ds_btree_lower(nd, k);
    while (nd && i >= nd->n) {
        nd = nd->u.l.next;
        i = 0;
        while (nd && nd->n == 0)
            nd = nd->u.l.next;
    }
    it->leaf = nd;
    it->i = i;
    return nd != NULL;
}

int dyn_btree_iter_get(const dyn_btree_iter* it, double* k,
    const dyn_cell_t** v)
{
    const ds_btree_node* nd = (const ds_btree_node*)it->leaf;
    if (!nd || it->i >= nd->n)
        return 0;
    *k = nd->key[it->i];
    *v = &nd->u.l.val[it->i];
    return 1;
}

int dyn_btree_iter_next(dyn_btree_iter* it)
{
    ds_btree_node* nd = (ds_btree_node*)it->leaf;
    if (!nd)
        return 0;
    it->i++;
    while (nd && it->i >= nd->n) {
        nd = nd->u.l.next;
        it->i = 0;
    }
    it->leaf = nd;
    return nd != NULL;
}

int dyn_btree_first(const dyn_btree_t* t, double* out)
{
    dyn_btree_iter it;
    const dyn_cell_t* v;
    if (!dyn_btree_iter_begin(t, &it))
        return 0;
    return dyn_btree_iter_get(&it, out, &v);
}

int dyn_btree_last(const dyn_btree_t* t, double* out)
{
    ds_btree_node* nd = t->root;
    while (!nd->leaf)
        nd = nd->u.i.kid[nd->n];
    while (nd && nd->n == 0)
        nd = nd->u.l.prev;
    if (!nd)
        return 0;
    *out = nd->key[nd->n - 1];
    return 1;
}

uint32_t dyn_btree_get_keys(const dyn_btree_t* t, double* out)
{
    ds_btree_node* nd = ds_btree_leftmost(t);
    uint32_t n = 0;
    while (nd) {
        if (nd->n) {
            memcpy(out + n, nd->key, (size_t)nd->n * sizeof(double));
            n += nd->n;
        }
        nd = nd->u.l.next;
    }
    return n;
}

uint32_t dyn_btree_walk_leaves(const dyn_btree_t* t, dyn_btree_leaf_fn fn,
    void* ud)
{
    ds_btree_node* nd = ds_btree_leftmost(t);
    uint32_t n = 0;
    while (nd) {
        if (nd->n) {
            if (!fn(ud, nd->key, nd->u.l.val, nd->n))
                break;
            n += nd->n;
        }
        nd = nd->u.l.next;
    }
    return n;
}

int dyn_btree_ceil(const dyn_btree_t* t, double k, double* out)
{
    dyn_btree_iter it;
    const dyn_cell_t* v;
    if (!dyn_btree_iter_seek(t, k, &it))
        return 0;
    return dyn_btree_iter_get(&it, out, &v);
}

int dyn_btree_floor(const dyn_btree_t* t, double k, double* out)
{
    ds_btree_node* nd = ds_btree_find_leaf(t, k);
    uint32_t i = ds_btree_lower(nd, k);
    if (i < nd->n && nd->key[i] == k) {
        *out = k;
        return 1;
    }
    while (nd && i == 0) {
        nd = nd->u.l.prev;
        i = nd ? nd->n : 0;
    }
    if (!nd || i == 0)
        return 0;
    *out = nd->key[i - 1];
    return 1;
}
