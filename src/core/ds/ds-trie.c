#include "ds-internal.h"
#include "ds-heap.h"
#include "ds-trie.h"

static uint8_t* ds_trie_bytes(dyn_trie_t* t, const uint8_t* src, uint32_t n)
{
    dyn_trie_bytes_t* b = t->bytes;
    uint8_t* p;
    if (!n)
        return NULL;
    if (!b || b->cap - b->used < n) {
        uint32_t cap = n > DYN_TRIE_BYTES_MIN ? n : DYN_TRIE_BYTES_MIN;
        b = (dyn_trie_bytes_t*)DYN_DS_MALLOC(sizeof(*b) + cap);
        if (!b)
            return NULL;
        b->next = t->bytes;
        b->used = 0;
        b->cap = cap;
        t->bytes = b;
    }
    p = DYN_TRIE_BYTES(b) + b->used;
    memcpy(p, src, n);
    b->used += n;
    return p;
}

static dyn_trie_node_t* ds_trie_new_node(dyn_trie_t* t)
{
    dyn_trie_chunk_t* c = t->chunks;
    dyn_trie_node_t* n;
    if (!c || c->used == c->cap) {
        uint32_t cap = c ? (c->cap < DYN_TRIE_CHUNK_MAX ? c->cap * 2
                                                        : DYN_TRIE_CHUNK_MAX)
                         : 32;
        c = (dyn_trie_chunk_t*)DYN_DS_MALLOC(sizeof(*c) + (size_t)cap * sizeof(dyn_trie_node_t));
        if (!c)
            return NULL;
        c->next = t->chunks;
        c->used = 0;
        c->cap = cap;
        t->chunks = c;
    }
    n = DYN_TRIE_CHUNK_NODES(c) + c->used++;
    memset(n, 0, sizeof(*n));
    return n;
}

dyn_trie_node_t* dyn_trie_child(dyn_trie_node_t* node, uint8_t b)
{
    dyn_trie_node_t* c;
    for (c = node->child; c; c = c->sibling)
        if (c->byte == b)
            return c;
    return NULL;
}

dyn_trie_node_t* dyn_trie_walk(dyn_trie_t* t, const char* key, size_t klen,
    int* on_tail)
{
    dyn_trie_node_t* node = t->root;
    size_t i = 0;
    *on_tail = 0;
    while (i < klen) {
        if (node->tail) {
            size_t rem = klen - i;
            if (rem == node->tail_len && memcmp(node->tail, key + i, rem) == 0) {
                *on_tail = 1;
                return node;
            }
            return NULL;
        }
        node = dyn_trie_child(node, (uint8_t)key[i]);
        if (!node)
            return NULL;
        i++;
    }
    return node;
}

static int ds_trie_expand(dyn_trie_t* t, dyn_trie_node_t** pnode, uint32_t keep)
{
    dyn_trie_node_t *node = *pnode, *c;
    uint8_t* tail = node->tail;
    uint32_t tlen = node->tail_len, j;
    dyn_trie_node_t** tmp;
    uint32_t nneed = keep + (keep < tlen ? 1 : 0);

    tmp = (dyn_trie_node_t**)DYN_DS_MALLOC((size_t)nneed * sizeof(*tmp));
    if (!tmp)
        return -1;
    for (j = 0; j < nneed; j++) {
        tmp[j] = ds_trie_new_node(t);
        if (!tmp[j]) {
            DYN_DS_FREE(tmp);
            return -1;
        }
    }
    node->tail = NULL;
    node->tail_len = 0;
    for (j = 0; j < keep; j++) {
        c = tmp[j];
        c->byte = tail[j];
        c->sibling = node->child;
        node->child = c;
        node = c;
    }
    if (keep == tlen) {
        node->is_end = 1;
    } else {
        c = tmp[keep];
        c->byte = tail[keep];
        c->sibling = node->child;
        node->child = c;
        if (keep + 1 == tlen) {
            c->is_end = 1;
        } else {
            c->tail = tail + keep + 1;
            c->tail_len = tlen - keep - 1;
        }
    }
    *pnode = node;
    DYN_DS_FREE(tmp);
    return 0;
}

int dyn_trie_insert_at(dyn_trie_t* t, dyn_trie_node_t* node,
    const char* key, size_t klen, size_t from,
    dyn_trie_node_t** path, size_t* plen)
{
    size_t i = from;

    for (;;) {
        if (path)
            *plen = i;
        if (i == klen) {
            if (!node->is_end) {
                node->is_end = 1;
                t->count++;
            }
            return 0;
        }
        if (node->tail) {
            uint32_t rem = (uint32_t)(klen - i), lim, L = 0;
            lim = rem < node->tail_len ? rem : node->tail_len;
            while (L < lim && node->tail[L] == (uint8_t)key[i + L])
                L++;
            if (L == node->tail_len && L == rem)
                return 0;
            if (path) {
                dyn_trie_node_t* walk = node;
                uint32_t j;
                if (ds_trie_expand(t, &node, L) < 0)
                    return -1;
                for (j = 0; j < L; j++) {
                    walk = walk->child;
                    path[i + j] = walk;
                }
                i += L;
                continue;
            }
            if (ds_trie_expand(t, &node, L) < 0)
                return -1;
            i += L;
            continue;
        }
        if (!node->child) {
            node->tail = ds_trie_bytes(t, (const uint8_t*)key + i,
                (uint32_t)(klen - i));
            if (!node->tail)
                return -1;
            node->tail_len = (uint32_t)(klen - i);
            t->count++;
            return 0;
        }
        {
            dyn_trie_node_t* c = dyn_trie_child(node, (uint8_t)key[i]);
            if (!c) {
                c = ds_trie_new_node(t);
                if (!c)
                    return -1;
                c->byte = (uint8_t)key[i];
                c->sibling = node->child;
                node->child = c;
            }
            if (path)
                path[i] = c;
            node = c;
            i++;
        }
    }
}

int dyn_trie_insert(dyn_trie_t* t, const char* key, size_t klen)
{
    return dyn_trie_insert_at(t, t->root, key, klen, 0, NULL, NULL);
}

void dyn_trie_order_children(dyn_trie_node_t** v, size_t k)
{
    size_t i, j;
    if (k <= 32) {
        for (i = 1; i < k; i++) {
            dyn_trie_node_t* x = v[i];
            for (j = i; j && v[j - 1]->byte > x->byte; j--)
                v[j] = v[j - 1];
            v[j] = x;
        }
    } else {
        dyn_trie_node_t* slot[256];
        memset(slot, 0, sizeof(slot));
        for (i = 0; i < k; i++)
            slot[v[i]->byte] = v[i];
        for (i = 0, k = 0; i < 256; i++)
            if (slot[i])
                v[k++] = slot[i];
    }
}

int dyn_trie_init(dyn_trie_t* t)
{
    t->chunks = NULL;
    t->bytes = NULL;
    t->count = 0;
    t->root = ds_trie_new_node(t);
    return t->root ? 0 : -1;
}

void dyn_trie_free(dyn_trie_t* t)
{
    dyn_trie_chunk_t *c, *nx;
    dyn_trie_bytes_t *b, *bn;
    for (c = t->chunks; c; c = nx) {
        nx = c->next;
        DYN_DS_FREE(c);
    }
    for (b = t->bytes; b; b = bn) {
        bn = b->next;
        DYN_DS_FREE(b);
    }
    t->chunks = NULL;
    t->bytes = NULL;
    t->root = NULL;
    t->count = 0;
}

int dyn_trie_has(const dyn_trie_t* t, const char* key, size_t klen)
{
    int on_tail;
    dyn_trie_node_t* node = dyn_trie_walk(DS_UNCONST(t), key, klen, &on_tail);
    return node && (on_tail || node->is_end);
}

static void ds_trie_unlink(dyn_trie_node_t* parent, dyn_trie_node_t* child)
{
    dyn_trie_node_t** pp = &parent->child;
    while (*pp && *pp != child)
        pp = &(*pp)->sibling;
    if (*pp)
        *pp = child->sibling;
}

int dyn_trie_delete(dyn_trie_t* t, const char* key, size_t klen)
{
    dyn_trie_node_t *node = t->root, *keep = t->root, *cut = NULL;
    size_t i = 0;
    while (i < klen) {
        dyn_trie_node_t* c;
        if (node->tail) {
            size_t rem = klen - i;
            if (rem != node->tail_len || memcmp(node->tail, key + i, rem) != 0)
                return 0;
            node->tail = NULL;
            node->tail_len = 0;
            t->count--;
            if (!node->is_end && cut)
                ds_trie_unlink(keep, cut);
            return 1;
        }
        c = dyn_trie_child(node, (uint8_t)key[i]);
        if (!c)
            return 0;
        if (node == t->root || node->is_end || node->child->sibling) {
            keep = node;
            cut = c;
        }
        node = c;
        i++;
    }
    if (!node->is_end)
        return 0;
    node->is_end = 0;
    t->count--;
    if (!node->child && !node->tail && cut)
        ds_trie_unlink(keep, cut);
    return 1;
}

int dyn_trie_has_prefix(const dyn_trie_t* t, const char* p, size_t plen)
{
    const dyn_trie_node_t* node = t->root;
    size_t i = 0;
    if (!plen)
        return t->count > 0;
    while (i < plen) {
        if (node->tail) {
            size_t rem = plen - i;
            return rem <= node->tail_len && memcmp(node->tail, p + i, rem) == 0;
        }
        node = dyn_trie_child(DS_UNCONST(node), (uint8_t)p[i]);
        if (!node)
            return 0;
        i++;
    }
    return 1;
}

size_t dyn_trie_longest_prefix(const dyn_trie_t* t, const char* s, size_t slen)
{
    dyn_trie_node_t* node = t->root;
    size_t i, best = 0;
    for (i = 0; i < slen; i++) {
        if (node->tail) {
            size_t rem = slen - i;
            if (rem >= node->tail_len && memcmp(node->tail, s + i, node->tail_len) == 0)
                best = i + node->tail_len;
            break;
        }
        node = dyn_trie_child(node, (uint8_t)s[i]);
        if (!node)
            break;
        if (node->is_end)
            best = i + 1;
    }
    if (node && node->tail && slen >= i && best < slen) {
        size_t rem = slen - i;
        if (rem == node->tail_len && memcmp(node->tail, s + i, rem) == 0)
            best = slen;
    }
    return best;
}
