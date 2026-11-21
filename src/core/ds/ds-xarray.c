#include "ds-internal.h"
#include "ds-heap.h"
#include "ds-xarray.h"

#define DS_XA_SHIFT 6
#define DS_XA_FANOUT 64
#define DS_XA_MARKS 3

typedef struct ds_xanode {
    uint8_t shift;
    uint64_t present;
    uint64_t marks[DS_XA_MARKS];
    void* slot[DS_XA_FANOUT];
} ds_xanode;

struct dyn_xarray {
    dyn_ds_ops vops;
    size_t vstride;
    ds_xanode* root;
    uint8_t height;
    size_t count;
};

static uint8_t* ds_xa_leaf_vals(ds_xanode* n)
{
    return (uint8_t*)n->slot[0];
}

static ds_xanode* ds_xa_node_new(dyn_xarray_t* x, uint8_t shift)
{
    ds_xanode* n = (ds_xanode*)DYN_DS_CALLOC(1, sizeof(*n));
    if (!n)
        return NULL;
    n->shift = shift;
    if (shift == 0) {
        size_t leaf_bytes = x->vstride * DS_XA_FANOUT;
        n->slot[0] = DYN_DS_MALLOC(leaf_bytes ? leaf_bytes : 1);
        if (!n->slot[0]) {
            DYN_DS_FREE(n);
            return NULL;
        }
    }
    return n;
}

dyn_xarray_t* dyn_xarray_new(const dyn_ds_ops* val)
{
    dyn_xarray_t* x = (dyn_xarray_t*)DYN_DS_CALLOC(1, sizeof(*x));
    if (!x)
        return NULL;
    ds_ops_init(&x->vops, val);
    x->vstride = ds_align8(x->vops.size);
    return x;
}

static void ds_xa_free_rec(dyn_xarray_t* x, ds_xanode* n)
{
    unsigned i;
    if (!n)
        return;
    if (n->shift == 0) {
        uint8_t* vals = ds_xa_leaf_vals(n);
        uint64_t p = n->present;
        while (p) {
            unsigned b = (unsigned)__builtin_ctzll(p);
            ds_ops_release(&x->vops, vals + (size_t)b * x->vstride);
            p &= p - 1;
        }
        DYN_DS_FREE(vals);
    } else {
        for (i = 0; i < DS_XA_FANOUT; i++)
            ds_xa_free_rec(x, (ds_xanode*)n->slot[i]);
    }
    DYN_DS_FREE(n);
}

void dyn_xarray_free(dyn_xarray_t* x)
{
    if (!x)
        return;
    ds_xa_free_rec(x, x->root);
    DYN_DS_FREE(x);
}

size_t dyn_xarray_count(const dyn_xarray_t* x)
{
    return x->count;
}

static uint64_t ds_xa_span(uint8_t height)
{
    unsigned bits = (unsigned)height * DS_XA_SHIFT;
    return bits >= 64 ? 0 : (uint64_t)1 << bits;
}

void* dyn_xarray_load(const dyn_xarray_t* x, uint64_t index)
{
    ds_xanode* n = x->root;
    uint64_t span = ds_xa_span(x->height);
    if (!n || (span && index >= span))
        return NULL;
    while (n->shift) {
        n = (ds_xanode*)n->slot[(index >> n->shift) & (DS_XA_FANOUT - 1)];
        if (!n)
            return NULL;
    }
    if (!(n->present >> (index & (DS_XA_FANOUT - 1)) & 1))
        return NULL;
    return ds_xa_leaf_vals(n) + (size_t)(index & (DS_XA_FANOUT - 1)) * x->vstride;
}

static int ds_xa_grow(dyn_xarray_t* x, uint64_t index)
{
    if (!x->root) {
        x->root = ds_xa_node_new(x, 0);
        if (!x->root)
            return -1;
        x->height = 1;
    }
    while (ds_xa_span(x->height) && index >= ds_xa_span(x->height)) {
        ds_xanode* r = ds_xa_node_new(x, (uint8_t)(x->height * DS_XA_SHIFT));
        unsigned m;
        if (!r)
            return -1;
        r->slot[0] = x->root;
        if (x->root->present)
            r->present = 1;
        for (m = 0; m < DS_XA_MARKS; m++)
            if (x->root->marks[m])
                r->marks[m] = 1;
        x->root = r;
        x->height++;
    }
    return 0;
}

int dyn_xarray_store(dyn_xarray_t* x, uint64_t index, const void* val)
{
    ds_xanode* path[11];
    ds_xanode* n;
    unsigned depth = 0, b;
    uint8_t* v;
    if (ds_xa_grow(x, index) < 0)
        return -1;
    n = x->root;
    while (n->shift) {
        unsigned i = (unsigned)((index >> n->shift) & (DS_XA_FANOUT - 1));
        path[depth++] = n;
        if (!n->slot[i]) {
            n->slot[i] = ds_xa_node_new(x, (uint8_t)(n->shift - DS_XA_SHIFT));
            if (!n->slot[i])
                return -1;
        }
        n = (ds_xanode*)n->slot[i];
    }
    b = (unsigned)(index & (DS_XA_FANOUT - 1));
    v = ds_xa_leaf_vals(n) + (size_t)b * x->vstride;
    if (n->present >> b & 1) {
        ds_ops_release(&x->vops, v);
        ds_ops_copy(&x->vops, v, val);
        return 0;
    }
    ds_ops_copy(&x->vops, v, val);
    n->present |= (uint64_t)1 << b;
    while (depth--) {
        ds_xanode* p = path[depth];
        p->present |= (uint64_t)1 << ((index >> p->shift) & (DS_XA_FANOUT - 1));
    }
    x->count++;
    return 1;
}

static void ds_xa_refresh(ds_xanode* p, unsigned i)
{
    ds_xanode* c = (ds_xanode*)p->slot[i];
    unsigned m;
    uint64_t bit = (uint64_t)1 << i;
    if (!c || !c->present)
        p->present &= ~bit;
    else
        p->present |= bit;
    for (m = 0; m < DS_XA_MARKS; m++) {
        if (c && c->marks[m])
            p->marks[m] |= bit;
        else
            p->marks[m] &= ~bit;
    }
}

int dyn_xarray_erase(dyn_xarray_t* x, uint64_t index)
{
    ds_xanode* path[11];
    unsigned idx[11];
    ds_xanode* n = x->root;
    unsigned depth = 0, b, m;
    uint64_t span = ds_xa_span(x->height);
    if (!n || (span && index >= span))
        return 0;
    while (n->shift) {
        unsigned i = (unsigned)((index >> n->shift) & (DS_XA_FANOUT - 1));
        path[depth] = n;
        idx[depth++] = i;
        n = (ds_xanode*)n->slot[i];
        if (!n)
            return 0;
    }
    b = (unsigned)(index & (DS_XA_FANOUT - 1));
    if (!(n->present >> b & 1))
        return 0;
    ds_ops_release(&x->vops, ds_xa_leaf_vals(n) + (size_t)b * x->vstride);
    n->present &= ~((uint64_t)1 << b);
    for (m = 0; m < DS_XA_MARKS; m++)
        n->marks[m] &= ~((uint64_t)1 << b);
    x->count--;
    while (depth--) {
        ds_xanode* p = path[depth];
        ds_xanode* c = (ds_xanode*)p->slot[idx[depth]];
        if (c && !c->present) {
            ds_xa_free_rec(x, c);
            p->slot[idx[depth]] = NULL;
        }
        ds_xa_refresh(p, idx[depth]);
    }
    if (x->count == 0) {
        ds_xa_free_rec(x, x->root);
        x->root = NULL;
        x->height = 0;
    }
    return 1;
}

static int ds_xa_find_rec(const dyn_xarray_t* x, ds_xanode* n, uint64_t base, uint64_t from, uint64_t max, int mark, uint64_t* out)
{
    uint64_t width = (uint64_t)1 << n->shift;
    unsigned start = 0, i;
    uint64_t bits = mark < 0 ? n->present : n->marks[mark];
    if (from > base)
        start = (unsigned)((from - base) >> n->shift);
    if (start >= DS_XA_FANOUT)
        return 0;
    bits &= start ? ~(((uint64_t)1 << start) - 1) : ~(uint64_t)0;
    while (bits) {
        uint64_t child_base;
        i = (unsigned)__builtin_ctzll(bits);
        child_base = base + (uint64_t)i * width;
        if (child_base > max)
            return 0;
        if (n->shift == 0) {
            *out = child_base;
            return 1;
        }
        if (ds_xa_find_rec(x, (ds_xanode*)n->slot[i], child_base, from > child_base ? from : child_base, max, mark, out))
            return 1;
        bits &= bits - 1;
    }
    return 0;
}

static int ds_xa_find(const dyn_xarray_t* x, uint64_t* index, uint64_t max, int mark, void** val)
{
    uint64_t span = ds_xa_span(x->height), found;
    if (!x->root || (span && *index >= span) || *index > max)
        return 0;
    if (!ds_xa_find_rec(x, x->root, 0, *index, max, mark, &found))
        return 0;
    *index = found;
    if (val)
        *val = dyn_xarray_load(x, found);
    return 1;
}

int dyn_xarray_find(const dyn_xarray_t* x, uint64_t* index, uint64_t max, void** val)
{
    return ds_xa_find(x, index, max, -1, val);
}

int dyn_xarray_find_marked(const dyn_xarray_t* x, uint64_t* index, uint64_t max, unsigned mark, void** val)
{
    if (mark >= DS_XA_MARKS)
        return 0;
    return ds_xa_find(x, index, max, (int)mark, val);
}

int dyn_xarray_set_mark(dyn_xarray_t* x, uint64_t index, unsigned mark, int on)
{
    ds_xanode* path[11];
    unsigned idx[11];
    ds_xanode* n = x->root;
    unsigned depth = 0, b;
    uint64_t span = ds_xa_span(x->height);
    if (mark >= DS_XA_MARKS || !n || (span && index >= span))
        return -1;
    while (n->shift) {
        unsigned i = (unsigned)((index >> n->shift) & (DS_XA_FANOUT - 1));
        path[depth] = n;
        idx[depth++] = i;
        n = (ds_xanode*)n->slot[i];
        if (!n)
            return -1;
    }
    b = (unsigned)(index & (DS_XA_FANOUT - 1));
    if (!(n->present >> b & 1))
        return -1;
    if (on)
        n->marks[mark] |= (uint64_t)1 << b;
    else
        n->marks[mark] &= ~((uint64_t)1 << b);
    while (depth--)
        ds_xa_refresh(path[depth], idx[depth]);
    return 0;
}

int dyn_xarray_get_mark(const dyn_xarray_t* x, uint64_t index, unsigned mark)
{
    ds_xanode* n = x->root;
    uint64_t span = ds_xa_span(x->height);
    if (mark >= DS_XA_MARKS || !n || (span && index >= span))
        return 0;
    while (n->shift) {
        n = (ds_xanode*)n->slot[(index >> n->shift) & (DS_XA_FANOUT - 1)];
        if (!n)
            return 0;
    }
    return (int)(n->marks[mark] >> (index & (DS_XA_FANOUT - 1)) & 1);
}
