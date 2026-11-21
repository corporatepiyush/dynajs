#include <stdlib.h>
#include "dd.h"

int dd_coll_name_ok(const char* s, size_t n, dd_err* e)
{
    if (n == 0)
        return dd_fail(e, DD_E_INVALIDOPTIONS, "Invalid collection name: the name is empty");
    if (n > 200)
        return dd_fail(e, DD_E_INVALIDOPTIONS, "Invalid collection name: longer than 200 bytes");
    if (memchr(s, 0, n) || memchr(s, '$', n))
        return dd_fail(e, DD_E_INVALIDOPTIONS, "Invalid collection name: '$' and NUL are not allowed");
    if (n >= 7 && memcmp(s, "system.", 7) == 0)
        return dd_fail(e, DD_E_INVALIDOPTIONS, "Invalid collection name: the system. prefix is reserved");
    return 0;
}

dd_coll* dd_coll_new(dd_db* db, const char* name, size_t n)
{
    dd_coll* c = calloc(1, sizeof *c);
    if (!c)
        return NULL;
    c->name = malloc(n + 1);
    if (!c->name) {
        free(c);
        return NULL;
    }
    memcpy(c->name, name, n);
    c->name[n] = 0;
    c->name_n = n;
    c->db = db;
    c->ring_cap = db ? (uint32_t)db->ring_default : 4096;
    dd_arena_init(&c->arena, db);
    dd_kmap_init(&c->ids);
    return c;
}

void dd_coll_clear(dd_coll* c)
{
    int i;
    for (i = 0; i < c->nix; i++)
        dd_index_free(c->ix[i]);
    free(c->ix);
    c->ix = NULL;
    c->nix = 0;
    dd_arena_drop_all(&c->arena);
    free(c->doc);
    free(c->dchunk);
    free(c->freel);
    c->doc = NULL;
    c->dchunk = NULL;
    c->freel = NULL;
    c->nslots = c->capslots = c->nfree = c->capfree = c->count = 0;
    dd_kmap_free(&c->ids);
    dd_kmap_init(&c->ids);
    dyn_ks_free(&c->idkb);
}

int dd_coll_reserve(dd_coll* c, uint32_t n)
{
    if (n > c->capslots) {
        const uint8_t** nd = realloc(c->doc, (size_t)n * sizeof *nd);
        dd_chunk** nch;
        if (!nd)
            return -1;
        c->doc = nd;
        nch = realloc(c->dchunk, (size_t)n * sizeof *nch);
        if (!nch)
            return -1;
        c->dchunk = nch;
        c->capslots = n;
    }
    return dd_kmap_reserve(&c->ids, n);
}

void dd_coll_free(dd_coll* c)
{
    if (!c)
        return;
    dd_coll_clear(c);
    dd_ring_free(&c->ring);
    free(c->options);
    free(c->name);
    free(c);
}

int dd_coll_check_doc(const uint8_t* doc, size_t n, int for_storage, dd_err* e)
{
    dyn_bson_iter it;
    int rc;
    if (n > DD_MAX_DOC)
        return dd_fail(e, DD_E_TOOLARGE, "object to insert too large: %zu bytes (the limit is %u)", n, DD_MAX_DOC);
    if (dyn_bson_iter_init(&it, doc, n) < 0)
        return dd_fail(e, DD_E_BADVALUE, "malformed document");
    while ((rc = dyn_bson_iter_next(&it)) > 0) {
        if (for_storage && it.klen > 0 && it.key[0] == '$')
            return dd_fail(e, DD_E_BADVALUE, "Document can't have $ prefixed field names: %.*s", (int)it.klen, it.key);
        if (it.klen == 3 && memcmp(it.key, "_id", 3) == 0) {
            if (it.type == DYN_BSON_ARRAY)
                return dd_fail(e, DD_E_BADVALUE, "can't use an array for _id");
            if (it.type == DYN_BSON_REGEX)
                return dd_fail(e, DD_E_BADVALUE, "can't use a regex for _id");
            if (it.type == DYN_BSON_UNDEFINED)
                return dd_fail(e, DD_E_BADVALUE, "can't use undefined for _id");
        }
    }
    if (rc < 0)
        return dd_fail(e, DD_E_BADVALUE, "malformed document: %s", dyn_bson_strerror(rc));
    return 0;
}

int dd_coll_id_key(const uint8_t* doc, dyn_ks_buf* kb, dd_err* e)
{
    dyn_bson_val id;
    if (!dyn_bson_find(doc, "_id", 3, &id))
        return dd_fail(e, DD_E_BADVALUE, "document has no _id");
    kb->n = 0;
    kb->oom = 0;
    dyn_ks_value(kb, id, 0);
    if (kb->oom)
        return dd_fail(e, DD_E_OOM, "out of memory");
    return 0;
}

int dd_coll_find_id(const dd_coll* c, dyn_bson_val id, uint32_t* docno)
{
    dyn_ks_buf kb;
    int r;
    dyn_ks_init(&kb);
    dyn_ks_value(&kb, id, 0);
    r = !kb.oom && dd_kmap_get(&c->ids, kb.p, kb.n, docno);
    dyn_ks_free(&kb);
    return r;
}

static int coll_slot(dd_coll* c, uint32_t* out)
{
    if (c->nfree) {
        *out = c->freel[--c->nfree];
        return 0;
    }
    if (c->nslots == c->capslots) {
        uint32_t nc = c->capslots ? c->capslots * 2 : 1024;
        const uint8_t** nd;
        dd_chunk** nch;
        if (nc < c->capslots)
            return -1;
        nd = realloc(c->doc, (size_t)nc * sizeof *nd);
        if (!nd)
            return -1;
        c->doc = nd;
        nch = realloc(c->dchunk, (size_t)nc * sizeof *nch);
        if (!nch)
            return -1;
        c->dchunk = nch;
        c->capslots = nc;
    }
    *out = c->nslots++;
    return 0;
}

static void coll_unslot(dd_coll* c, uint32_t docno)
{
    c->doc[docno] = NULL;
    c->dchunk[docno] = NULL;
    if (docno + 1 == c->nslots) {
        c->nslots--;
        return;
    }
    if (c->nfree == c->capfree) {
        uint32_t nc = c->capfree ? c->capfree * 2 : 256;
        uint32_t* np = realloc(c->freel, (size_t)nc * sizeof *np);
        if (!np)
            return;
        c->freel = np;
        c->capfree = nc;
    }
    c->freel[c->nfree++] = docno;
}

static int coll_dup_id(dd_coll* c, const uint8_t* doc, dd_err* e)
{
    dyn_bson_val id;
    char r[200];
    dyn_bson_find(doc, "_id", 3, &id);
    return dd_fail(e, DD_E_DUPKEY, "E11000 duplicate key error collection: %s.%s index: _id_ dup key: { _id: %s }",
        c->db ? c->db->name : "test", c->name, dd_repr(id, r, sizeof r));
}

int dd_coll_load_insert(dd_coll* c, const uint8_t* doc, size_t n, const uint8_t* key, size_t kn, uint64_t h, dd_err* e)
{
    const uint8_t* stored;
    dd_chunk* ch;
    uint32_t d;
    int r;
    if (coll_slot(c, &d) < 0)
        return dd_fail(e, DD_E_OOM, "out of memory");
    r = dd_kmap_put_h(&c->ids, key, kn, h, d, NULL);
    if (r != 0) {
        coll_unslot(c, d);
        return r < 0 ? dd_fail(e, DD_E_OOM, "out of memory") : coll_dup_id(c, doc, e);
    }
    stored = dd_arena_put(&c->arena, doc, n, &ch);
    if (!stored) {
        dd_kmap_del(&c->ids, key, kn);
        coll_unslot(c, d);
        return dd_fail(e, DD_E_OOM, "out of memory");
    }
    c->doc[d] = stored;
    c->dchunk[d] = ch;
    c->count++;
    c->version++;
    return 0;
}

int dd_coll_insert(dd_coll* c, const uint8_t* doc, size_t n, uint32_t* docno, dd_err* e)
{
    dyn_ks_buf* kb = &c->idkb;
    const uint8_t* stored;
    dd_chunk* ch;
    uint32_t d;
    int i, j;
    if (dd_coll_id_key(doc, kb, e) < 0)
        return -1;
    if (dd_kmap_get(&c->ids, kb->p, kb->n, NULL))
        return coll_dup_id(c, doc, e);
    for (i = 0; i < c->nix; i++)
        if (dd_index_check_unique(c->ix[i], UINT32_MAX, doc, e) < 0)
            return -1;
    if (coll_slot(c, &d) < 0)
        return dd_fail(e, DD_E_OOM, "out of memory");
    stored = dd_arena_put(&c->arena, doc, n, &ch);
    if (!stored) {
        coll_unslot(c, d);
        return dd_fail(e, DD_E_OOM, "out of memory");
    }
    c->doc[d] = stored;
    c->dchunk[d] = ch;
    if (dd_kmap_put(&c->ids, kb->p, kb->n, d, NULL) < 0) {
        dd_arena_release(&c->arena, ch, n);
        coll_unslot(c, d);
        return dd_fail(e, DD_E_OOM, "out of memory");
    }
    for (i = 0; i < c->nix; i++) {
        if (dd_index_add(c->ix[i], d, stored, e) < 0) {
            for (j = 0; j < i; j++)
                dd_index_remove(c->ix[j], d, stored);
            dd_kmap_del(&c->ids, kb->p, kb->n);
            dd_arena_release(&c->arena, ch, n);
            coll_unslot(c, d);
            return -1;
        }
    }
    c->count++;
    c->version++;
    if (docno)
        *docno = d;
    return 0;
}

static int coll_ix_uses(const dd_index* ix, const char* k, uint32_t kl)
{
    int f;
    for (f = 0; f < ix->nf; f++)
        if (ix->f[f].path.plen[0] == kl && memcmp(ix->f[f].path.part[0], k, kl) == 0)
            return 1;
    return 0;
}

static int coll_ix_same_find(const dd_index* ix, const uint8_t* a, const uint8_t* b)
{
    int f;
    for (f = 0; f < ix->nf; f++) {
        const dd_path* p = &ix->f[f].path;
        dyn_bson_val va, vb;
        int fa = dyn_bson_find(a, p->part[0], p->plen[0], &va);
        if (fa != dyn_bson_find(b, p->part[0], p->plen[0], &vb))
            return 0;
        if (fa && (va.type != vb.type || va.vlen != vb.vlen || memcmp(va.val, vb.val, va.vlen) != 0))
            return 0;
    }
    return 1;
}

static uint64_t coll_same_mask(const dd_coll* c, const uint8_t* a, const uint8_t* b)
{
    dyn_bson_iter ia, ib;
    uint64_t cand = 0;
    int i, n = c->nix < 64 ? c->nix : 64;
    for (i = 0; i < n; i++)
        if (!c->ix[i]->partial)
            cand |= 1ULL << i;
    if (!cand)
        return 0;
    if (dyn_bson_iter_init(&ia, a, dyn_bson_rd32(a)) < 0 || dyn_bson_iter_init(&ib, b, dyn_bson_rd32(b)) < 0)
        goto lookup;
    for (;;) {
        int ra = dyn_bson_iter_next(&ia), rb = dyn_bson_iter_next(&ib);
        const dyn_bson_iter* src;
        if (ra < 0 || rb < 0)
            goto lookup;
        if (ra == 0 && rb == 0)
            return cand;
        if (ra > 0 && rb > 0) {
            if (ia.klen != ib.klen || memcmp(ia.key, ib.key, ia.klen) != 0)
                goto lookup;
            if (ia.type == ib.type && ia.vlen == ib.vlen && memcmp(ia.val, ib.val, ia.vlen) == 0)
                continue;
        }
        src = ra > 0 ? &ia : &ib;
        for (i = 0; i < n; i++)
            if ((cand >> i) & 1 && coll_ix_uses(c->ix[i], src->key, src->klen))
                cand &= ~(1ULL << i);
        if (!cand)
            return 0;
    }
lookup:
    for (i = 0; i < n; i++)
        if ((cand >> i) & 1 && !coll_ix_same_find(c->ix[i], a, b))
            cand &= ~(1ULL << i);
    return cand;
}

static int coll_replace(dd_coll* c, uint32_t docno, const uint8_t* doc, size_t n, int keep_same, dd_err* e)
{
    const uint8_t* old = c->doc[docno];
    dd_chunk* och = c->dchunk[docno];
    size_t on = dyn_bson_rd32(old);
    const uint8_t* stored;
    dd_chunk* ch;
    uint64_t same = keep_same && c->nix ? coll_same_mask(c, old, doc) : 0;
    int i, j;
    for (i = 0; i < c->nix; i++) {
        if (i < 64 && (same >> i) & 1)
            continue;
        if (dd_index_check_unique(c->ix[i], docno, doc, e) < 0)
            return -1;
    }
    stored = dd_arena_put(&c->arena, doc, n, &ch);
    if (!stored)
        return dd_fail(e, DD_E_OOM, "out of memory");
    for (i = 0; i < c->nix; i++) {
        if (i < 64 && (same >> i) & 1)
            continue;
        dd_index_remove(c->ix[i], docno, old);
        if (dd_index_add(c->ix[i], docno, stored, e) < 0) {
            dd_index_add(c->ix[i], docno, old, NULL);
            for (j = 0; j < i; j++) {
                if (j < 64 && (same >> j) & 1)
                    continue;
                dd_index_remove(c->ix[j], docno, stored);
                dd_index_add(c->ix[j], docno, old, NULL);
            }
            dd_arena_release(&c->arena, ch, n);
            return -1;
        }
    }
    c->doc[docno] = stored;
    c->dchunk[docno] = ch;
    dd_arena_release(&c->arena, och, on);
    c->version++;
    return 0;
}

int dd_coll_replace(dd_coll* c, uint32_t docno, const uint8_t* doc, size_t n, dd_err* e)
{
    return coll_replace(c, docno, doc, n, 0, e);
}

int dd_coll_update(dd_coll* c, uint32_t docno, const uint8_t* doc, size_t n, dd_err* e)
{
    return coll_replace(c, docno, doc, n, 1, e);
}

int dd_coll_remove(dd_coll* c, uint32_t docno)
{
    const uint8_t* old = c->doc[docno];
    int i;
    if (!old)
        return 0;
    for (i = 0; i < c->nix; i++)
        dd_index_remove(c->ix[i], docno, old);
    if (dd_coll_id_key(old, &c->idkb, NULL) == 0)
        dd_kmap_del(&c->ids, c->idkb.p, c->idkb.n);
    dd_arena_release(&c->arena, c->dchunk[docno], dyn_bson_rd32(old));
    coll_unslot(c, docno);
    c->count--;
    c->version++;
    return 1;
}
