#include "structures-internal.h"
#include "core/ds/ds-trie.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_STRUCTURES)

#define DS_TRIE_FRONTCODED 0xFFFFFFFFu

typedef struct {
    dyn_trie_node_t* n;
    size_t depth;
} dsw_trie_frame;

static JSValue dyn_ds_iterator_trie(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    JSValue empty = JS_NewString(ctx, "");
    JSValueConst args[1];
    JSValue r;
    (void)argc;
    (void)argv;
    if (JS_IsException(empty))
        return empty;
    args[0] = empty;
    r = dyn_ds_iter_via(ctx, this_val, "keysWithPrefix", 1, args);
    JS_FreeValue(ctx, empty);
    return r;
}

static void dsw_trie_free(void* native)
{
    dyn_trie_t* t = (dyn_trie_t*)native;
    if (!t)
        return;
    dyn_trie_free(t);
    dyn_ds_free(t);
}

JSClassID dsw_trie_class_id;

DYN_FINALIZER(dsw_trie)

const JSClassDef dsw_trie_class = {
    "Trie",
    .finalizer = dsw_trie_finalizer,
};

JSValue dsw_trie_ctor(JSContext* ctx, JSValueConst new_target,
    int argc, JSValueConst* argv)
{
    dyn_trie_t* t;
    (void)argc;
    (void)argv;
    t = (dyn_trie_t*)dyn_ds_malloc(sizeof(*t));
    if (!t)
        return JS_ThrowOutOfMemory(ctx);
    if (dyn_trie_init(t) < 0) {
        dyn_ds_free(t);
        return JS_ThrowOutOfMemory(ctx);
    }
    return dyn_plain_wrap(ctx, new_target, dsw_trie_class_id, t, dsw_trie_free);
}

static JSValue dsw_trie_insert(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_trie_t* t;
    const char* key;
    size_t klen;
    int rc;
    (void)argc;
    key = JS_ToCStringLen(ctx, &klen, argv[0]);
    if (!key)
        return JS_EXCEPTION;
    t = (dyn_trie_t*)dyn_plain_get(ctx, this_val, dsw_trie_class_id);
    if (!t) {
        JS_FreeCString(ctx, key);
        return JS_EXCEPTION;
    }
    rc = dyn_trie_insert(t, key, klen);
    JS_FreeCString(ctx, key);
    if (rc < 0)
        return JS_ThrowOutOfMemory(ctx);
    return JS_DupValue(ctx, this_val);
}

static JSValue dsw_trie_has(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_trie_t* t;
    const char* key;
    size_t klen;
    (void)argc;
    key = JS_ToCStringLen(ctx, &klen, argv[0]);
    if (!key)
        return JS_EXCEPTION;
    t = (dyn_trie_t*)dyn_plain_get(ctx, this_val, dsw_trie_class_id);
    if (!t) {
        JS_FreeCString(ctx, key);
        return JS_EXCEPTION;
    }
    {
        int r = dyn_trie_has(t, key, klen);
        JS_FreeCString(ctx, key);
        return JS_NewBool(ctx, r);
    }
}

static JSValue dsw_trie_has_prefix(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_trie_t* t;
    const char* p;
    size_t plen;
    int r;
    (void)argc;
    p = JS_ToCStringLen(ctx, &plen, argv[0]);
    if (!p)
        return JS_EXCEPTION;
    t = (dyn_trie_t*)dyn_plain_get(ctx, this_val, dsw_trie_class_id);
    if (!t) {
        JS_FreeCString(ctx, p);
        return JS_EXCEPTION;
    }
    r = dyn_trie_has_prefix(t, p, plen);
    JS_FreeCString(ctx, p);
    return JS_NewBool(ctx, r);
}

static JSValue dsw_trie_delete(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_trie_t* t;
    const char* key;
    size_t klen;
    (void)argc;
    key = JS_ToCStringLen(ctx, &klen, argv[0]);
    if (!key)
        return JS_EXCEPTION;
    t = (dyn_trie_t*)dyn_plain_get(ctx, this_val, dsw_trie_class_id);
    if (!t) {
        JS_FreeCString(ctx, key);
        return JS_EXCEPTION;
    }
    {
        int r = dyn_trie_delete(t, key, klen);
        JS_FreeCString(ctx, key);
        return JS_NewBool(ctx, r);
    }
}

static int dsw_trie_emit(JSContext* ctx, JSValue arr, uint32_t* out, char** kb,
    size_t* kbcap, size_t base, const uint8_t* extra,
    size_t n)
{
    size_t need = base + n;
    if (need + 1 > *kbcap) {
        size_t nc = *kbcap ? *kbcap : 64;
        char* nk;
        while (nc <= need)
            nc *= 2;
        nk = (char*)dyn_ds_realloc(*kb, nc);
        if (!nk)
            return -1;
        *kb = nk;
        *kbcap = nc;
    }
    if (n)
        memcpy(*kb + base, extra, n);
    return JS_DefinePropertyValueUint32(ctx, arr, (*out)++,
        JS_NewStringLen(ctx, *kb, need),
        JS_PROP_C_W_E);
}

static JSValue dsw_trie_keys_with_prefix(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_trie_t* t;
    const char* pfx;
    size_t plen;
    dyn_trie_node_t* base;
    JSValue arr;
    uint32_t out = 0;
    char* kb = NULL;
    size_t kbcap = 0;
    dyn_trie_node_t** nstk = NULL;
    size_t *dstk = NULL, slen = 0, scap = 0;
    (void)argc;

    pfx = JS_ToCStringLen(ctx, &plen, argv[0]);
    if (!pfx)
        return JS_EXCEPTION;
    t = (dyn_trie_t*)dyn_plain_get(ctx, this_val, dsw_trie_class_id);
    if (!t) {
        JS_FreeCString(ctx, pfx);
        return JS_EXCEPTION;
    }

    arr = JS_NewArray(ctx);
    if (JS_IsException(arr)) {
        JS_FreeCString(ctx, pfx);
        return arr;
    }
    {
        size_t i = 0;
        base = t->root;
        while (i < plen) {
            if (base->tail) {
                size_t rem = plen - i;
                if (rem <= base->tail_len && memcmp(base->tail, pfx + i, rem) == 0) {
                    kbcap = i + base->tail_len + 1;
                    kb = (char*)dyn_ds_malloc(kbcap);
                    if (!kb)
                        goto fail;
                    memcpy(kb, pfx, i);
                    if (dsw_trie_emit(ctx, arr, &out, &kb, &kbcap, i,
                            base->tail, base->tail_len)
                        < 0)
                        goto fail;
                }
                goto done;
            }
            base = dyn_trie_child(base, (uint8_t)pfx[i]);
            if (!base)
                goto done;
            i++;
        }
    }

    kbcap = plen + 16;
    kb = (char*)dyn_ds_malloc(kbcap);
    if (!kb)
        goto fail;
    if (plen)
        memcpy(kb, pfx, plen);

    if (base->is_end) {
        if (JS_DefinePropertyValueUint32(ctx, arr, out++,
                JS_NewStringLen(ctx, pfx, plen), JS_PROP_C_W_E)
            < 0)
            goto fail;
    }
    if (base->tail && dsw_trie_emit(ctx, arr, &out, &kb, &kbcap, plen, base->tail, base->tail_len) < 0)
        goto fail;
    if (base->child) {
        if (scap < 8) {
            void* nn = dyn_ds_realloc(nstk, 8 * sizeof(*nstk));
            void* dd = dyn_ds_realloc(dstk, 8 * sizeof(*dstk));
            if (!nn || !dd) {
                dyn_ds_free(nn ? nn : nstk);
                dyn_ds_free(dd ? dd : dstk);
                nstk = NULL;
                dstk = NULL;
                goto fail;
            }
            nstk = (dyn_trie_node_t**)nn;
            dstk = (size_t*)dd;
            scap = 8;
        }
        nstk[slen] = base->child;
        dstk[slen] = 0;
        slen++;
    }
    while (slen) {
        dyn_trie_node_t* n = nstk[--slen];
        size_t depth = dstk[slen];
        if (plen + depth >= kbcap) {
            size_t nc = kbcap * 2;
            char* nk;
            while (nc <= plen + depth)
                nc *= 2;
            nk = (char*)dyn_ds_realloc(kb, nc);
            if (!nk)
                goto fail;
            kb = nk;
            kbcap = nc;
        }
        kb[plen + depth] = (char)n->byte;
        if (n->is_end && dsw_trie_emit(ctx, arr, &out, &kb, &kbcap, plen + depth + 1, NULL, 0) < 0)
            goto fail;
        if (n->tail && dsw_trie_emit(ctx, arr, &out, &kb, &kbcap, plen + depth + 1, n->tail, n->tail_len) < 0)
            goto fail;
        if (slen + 2 > scap) {
            size_t nc = scap * 2;
            void* nn = dyn_ds_realloc(nstk, nc * sizeof(*nstk));
            void* dd = dyn_ds_realloc(dstk, nc * sizeof(*dstk));
            if (!nn || !dd) {
                if (nn)
                    nstk = nn;
                if (dd)
                    dstk = dd;
                goto fail;
            }
            nstk = (dyn_trie_node_t**)nn;
            dstk = (size_t*)dd;
            scap = nc;
        }
        if (n->sibling) {
            nstk[slen] = n->sibling;
            dstk[slen] = depth;
            slen++;
        }
        if (n->child) {
            nstk[slen] = n->child;
            dstk[slen] = depth + 1;
            slen++;
        }
    }
done:
    JS_FreeCString(ctx, pfx);
    dyn_ds_free(kb);
    dyn_ds_free(nstk);
    dyn_ds_free(dstk);
    return arr;
fail:
    JS_FreeCString(ctx, pfx);
    dyn_ds_free(kb);
    dyn_ds_free(nstk);
    dyn_ds_free(dstk);
    JS_FreeValue(ctx, arr);
    return JS_EXCEPTION;
}

static JSValue dsw_trie_longest_prefix(JSContext* ctx, JSValueConst this_val,
    int argc, JSValueConst* argv)
{
    dyn_trie_t* t;
    const char* s;
    size_t slen, best;
    JSValue out;
    (void)argc;
    s = JS_ToCStringLen(ctx, &slen, argv[0]);
    if (!s)
        return JS_EXCEPTION;
    t = (dyn_trie_t*)dyn_plain_get(ctx, this_val, dsw_trie_class_id);
    if (!t) {
        JS_FreeCString(ctx, s);
        return JS_EXCEPTION;
    }
    best = dyn_trie_longest_prefix(t, s, slen);
    out = JS_NewStringLen(ctx, s, best);
    JS_FreeCString(ctx, s);
    return out;
}

static JSValue dsw_trie_size(JSContext* ctx, JSValueConst this_val)
{
    dyn_trie_t* t = (dyn_trie_t*)dyn_plain_get(ctx, this_val, dsw_trie_class_id);
    if (!t)
        return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)t->count);
}

const JSCFunctionListEntry dsw_trie_proto[] = {
    JS_CFUNC_DEF("insert", 1, dsw_trie_insert),
    JS_CFUNC_DEF("has", 1, dsw_trie_has),
    JS_CFUNC_DEF("delete", 1, dsw_trie_delete),
    JS_CFUNC_DEF("keysWithPrefix", 1, dsw_trie_keys_with_prefix),
    JS_CFUNC_DEF("hasPrefix", 1, dsw_trie_has_prefix),
    JS_CFUNC_DEF("[Symbol.iterator]", 0, dyn_ds_iterator_trie),
    JS_CFUNC_DEF("longestPrefix", 1, dsw_trie_longest_prefix),
    JS_CGETSET_DEF("size", dsw_trie_size, NULL),
};

int dsw_trie_grow(void** p, size_t* cap, size_t need, size_t elem)
{
    size_t nc = *cap ? *cap : 64;
    void* np;
    if (need <= *cap)
        return 0;
    while (nc < need)
        nc *= 2;
    np = dyn_ds_realloc(*p, nc * elem);
    if (!np)
        return -1;
    *p = np;
    *cap = nc;
    return 0;
}

int dsw_trie_write(JSContext* ctx, dyn_ser_t* w, JSValueConst obj)
{
    dyn_trie_t* t = JS_GetOpaque(obj, dsw_trie_class_id);
    dsw_trie_frame* frames = NULL;
    size_t nframes = 0, fcap = 0;
    uint8_t *path = NULL, *prev = NULL;
    size_t pcap = 0, prevcap = 0, prev_len = 0;
    dyn_trie_node_t* kids[256];
    uint32_t emitted = 0;
    if (!t)
        return -1;
    int rc = -1;

    (void)ctx;
    if (dyn_ser_u32(w, DS_TRIE_FRONTCODED) < 0 || dyn_ser_u32(w, t->count) < 0)
        goto done;

    if (t->root->is_end) {
        if (dyn_sk_uvarint(w, 0) < 0 || dyn_sk_uvarint(w, 0) < 0)
            goto done;
        emitted++;
    }
    if (t->root->tail) {
        if (dyn_sk_uvarint(w, 0) < 0 || dyn_sk_uvarint(w, t->root->tail_len) < 0 || dyn_ser_raw(w, t->root->tail, t->root->tail_len) < 0)
            goto done;
        if (dsw_trie_grow((void**)&prev, &prevcap, t->root->tail_len, 1) < 0)
            goto done;
        memcpy(prev, t->root->tail, t->root->tail_len);
        prev_len = t->root->tail_len;
        emitted++;
    }
    {
        dyn_trie_node_t* c;
        size_t k = 0;
        for (c = t->root->child; c; c = c->sibling) {
            if (k == 256)
                goto done;
            kids[k++] = c;
        }
        dyn_trie_order_children(kids, k);
        if (dsw_trie_grow((void**)&frames, &fcap, k, sizeof(*frames)) < 0)
            goto done;
        while (k--) {
            frames[nframes].n = kids[k];
            frames[nframes].depth = 0;
            nframes++;
        }
    }
    while (nframes) {
        dyn_trie_node_t *n = frames[nframes - 1].n, *c;
        size_t depth = frames[nframes - 1].depth, k = 0;
        nframes--;

        if (dsw_trie_grow((void**)&path, &pcap, depth + 1, 1) < 0)
            goto done;
        path[depth] = n->byte;

        {
            size_t which;
            for (which = 0; which < 2; which++) {
                size_t klen, sh = 0, lim;
                if (which == 0) {
                    if (!n->is_end)
                        continue;
                    klen = depth + 1;
                } else {
                    if (!n->tail)
                        continue;
                    klen = depth + 1 + n->tail_len;
                    if (dsw_trie_grow((void**)&path, &pcap, klen, 1) < 0)
                        goto done;
                    memcpy(path + depth + 1, n->tail, n->tail_len);
                }
                lim = klen < prev_len ? klen : prev_len;
                while (sh < lim && path[sh] == prev[sh])
                    sh++;
                if (dyn_sk_uvarint(w, sh) < 0 || dyn_sk_uvarint(w, klen - sh) < 0 || dyn_ser_raw(w, path + sh, klen - sh) < 0)
                    goto done;
                if (dsw_trie_grow((void**)&prev, &prevcap, klen, 1) < 0)
                    goto done;
                memcpy(prev, path, klen);
                prev_len = klen;
                emitted++;
            }
        }

        for (c = n->child; c; c = c->sibling) {
            if (k == 256)
                goto done;
            kids[k++] = c;
        }
        dyn_trie_order_children(kids, k);
        if (dsw_trie_grow((void**)&frames, &fcap, nframes + k,
                sizeof(*frames))
            < 0)
            goto done;
        while (k--) {
            frames[nframes].n = kids[k];
            frames[nframes].depth = depth + 1;
            nframes++;
        }
    }
    if (emitted != t->count)
        goto done;
    rc = 0;
done:
    dyn_ds_free(frames);
    dyn_ds_free(path);
    dyn_ds_free(prev);
    return rc;
}

JSValue dsw_trie_read(JSContext* ctx, dyn_de_t* r, JSValueConst opts)
{
    uint32_t n, i, first;
    JSValue obj;
    dyn_trie_t* t;
    uint8_t* cur = NULL;
    size_t curcap = 0, cur_len = 0;
    dyn_trie_node_t** path = NULL;
    size_t pathcap = 0, path_len = 0;
    (void)opts;

    first = dyn_de_u32(r);
    if (!dyn_de_ok(r))
        return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);

    if (first != DS_TRIE_FRONTCODED) {
        if ((uint64_t)first * 4 > (uint64_t)dyn_de_left(r))
            return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
        obj = dyn_ds_construct(ctx, dsw_trie_class_id, 0, NULL);
        if (JS_IsException(obj))
            return obj;
        t = JS_GetOpaque(obj, dsw_trie_class_id);
        for (i = 0; i < first; i++) {
            size_t kl;
            const char* k = dyn_de_blob(r, &kl);
            if (!dyn_de_ok(r)) {
                JS_FreeValue(ctx, obj);
                return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
            }
            if (dyn_trie_insert(t, k, kl) < 0) {
                JS_FreeValue(ctx, obj);
                return JS_ThrowOutOfMemory(ctx);
            }
        }
        return obj;
    }

    if (dyn_de_count(r, &n, 2) < 0)
        return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
    obj = dyn_ds_construct(ctx, dsw_trie_class_id, 0, NULL);
    if (JS_IsException(obj))
        return obj;
    t = JS_GetOpaque(obj, dsw_trie_class_id);
    for (i = 0; i < n; i++) {
        uint64_t sh, sl;
        const uint8_t* p;
        if (dyn_sk_read_uvarint(r, &sh) < 0 || dyn_sk_read_uvarint(r, &sl) < 0)
            goto truncated;
        if (sh > cur_len || sl > (uint64_t)dyn_de_left(r))
            goto truncated;
        if (dsw_trie_grow((void**)&cur, &curcap, (size_t)(sh + sl), 1) < 0)
            goto oom;
        p = dyn_de_raw(r, (size_t)sl);
        if (!p)
            goto truncated;
        memcpy(cur + sh, p, (size_t)sl);
        cur_len = (size_t)(sh + sl);
        if (dsw_trie_grow((void**)&path, &pathcap,
                (cur_len + 1) * sizeof(*path), 1)
            < 0)
            goto oom;
        {
            size_t from = sh < path_len ? (size_t)sh : path_len;
            dyn_trie_node_t* start = from ? path[from - 1] : t->root;
            if (dyn_trie_insert_at(t, start, (const char*)cur, cur_len, from,
                    path, &path_len)
                < 0)
                goto oom;
        }
    }
    dyn_ds_free(cur);
    dyn_ds_free(path);
    return obj;
truncated:
    dyn_ds_free(cur);
    dyn_ds_free(path);
    JS_FreeValue(ctx, obj);
    return dyn_codec_throw(ctx, DYN_DE_TRUNCATED);
oom:
    dyn_ds_free(cur);
    dyn_ds_free(path);
    JS_FreeValue(ctx, obj);
    return JS_ThrowOutOfMemory(ctx);
}

#endif
