#include <math.h>
#include "veck-int.h"

int dyn_vec_ids_sorted(const uint64_t* ids, uint64_t n)
{
    uint64_t i;
    for (i = 1; i < n; i++)
        if (ids[i - 1] >= ids[i])
            return 0;
    return 1;
}

static uint64_t vk_block_for(uint32_t M, uint32_t lvl)
{
    return (uint64_t)(1 + 2 * M) + (uint64_t)lvl * (1 + M);
}

uint64_t dyn_vec_block_words(const dyn_vec_t* v, uint32_t slot)
{
    return vk_block_for(v->M, v->lvl[slot]);
}

void dyn_vec_meta(const dyn_vec_t* v, uint64_t* meta)
{
    uint64_t lw = 0;
    uint32_t i;
    memset(meta, 0, DYN_VEC_META_WORDS * sizeof(uint64_t));
    if (v->kind == DYN_VEC_HNSW)
        for (i = 0; i < v->n; i++)
            lw += dyn_vec_block_words(v, i);
    meta[0] = DYN_VEC_LAYOUT;
    meta[1] = v->dim;
    meta[2] = v->metric;
    meta[3] = v->kind;
    meta[4] = v->M;
    meta[5] = v->efc;
    meta[6] = v->efs;
    meta[7] = (uint64_t)v->heuristic | ((uint64_t)v->exact_fallback << 1) | ((uint64_t)!v->keep << 2)
        | ((uint64_t)v->trained << 3);
    meta[8] = v->quant;
    meta[9] = v->rng;
    meta[10] = v->n;
    meta[11] = v->live;
    meta[12] = v->entry;
    meta[13] = (uint64_t)(int64_t)v->top;
    meta[14] = lw;
    meta[15] = v->pool;
}

#define VK_BAD(msg) \
    do {            \
        *why = msg; \
        goto bad;   \
    } while (0)

static int vk_restore_links(dyn_vec_t* v, const dyn_vec_image_t* img, const char** why)
{
    uint64_t want = 0, at = 0;
    uint32_t i, j, n = v->n, maxl = 0;
    int l;
    for (i = 0; i < n; i++) {
        if (img->lvl[i] > DYN_VEC_MAX_LEVEL) {
            *why = "node level out of range";
            return -1;
        }
        if (img->lvl[i] > maxl)
            maxl = img->lvl[i];
        want += vk_block_for(v->M, img->lvl[i]);
    }
    if (want != img->meta[14] || img->links_bytes != want * 4) {
        *why = "link section size does not match the node levels";
        return -1;
    }
    if (n == 0) {
        if (v->entry != DYN_VEC_NONE || v->top != -1) {
            *why = "empty graph with an entry point";
            return -1;
        }
        return 0;
    }
    if (v->entry >= n || v->top != (int)img->lvl[v->entry] || (uint32_t)v->top != maxl) {
        *why = "entry point is not a top-level node";
        return -1;
    }
    for (i = 0; i < n; i++) {
        const uint32_t* b = img->links + at;
        for (l = 0; l <= img->lvl[i]; l++) {
            const uint32_t* L = l == 0 ? b : b + (1 + 2 * v->M) + (size_t)(l - 1) * (1 + v->M);
            uint32_t cap = l == 0 ? 2 * v->M : v->M;
            if (L[0] > cap) {
                *why = "link count exceeds the degree bound";
                return -1;
            }
            for (j = 0; j < L[0]; j++) {
                uint32_t nb = L[j + 1];
                if (nb >= n || nb == i || img->lvl[nb] < l) {
                    *why = "link points outside its layer";
                    return -1;
                }
            }
        }
        at += vk_block_for(v->M, img->lvl[i]);
    }
    v->lwords = want;
    v->lpool = (uint32_t*)malloc((size_t)(want ? want : 1) * sizeof(uint32_t));
    v->lvl = (uint8_t*)malloc(n);
    v->links = (uint32_t**)malloc((size_t)n * sizeof(uint32_t*));
    v->vis = (uint32_t*)calloc(n, sizeof(uint32_t));
    if (!v->lpool || !v->lvl || !v->links || !v->vis) {
        *why = NULL;
        return -1;
    }
    memcpy(v->lpool, img->links, (size_t)want * sizeof(uint32_t));
    memcpy(v->lvl, img->lvl, n);
    for (i = 0, at = 0; i < n; i++) {
        v->links[i] = v->lpool + at;
        at += vk_block_for(v->M, img->lvl[i]);
    }
    return 0;
}

static int vk_restore_quant_check(const uint64_t* m, const dyn_vec_image_t* img, const char** why)
{
    uint64_t n = m[10], d = m[1], i, j, w = (d + 63) / 64;
    if (m[8] == DYN_VEC_F32) {
        if (img->codes || img->qparam) {
            *why = "an f32 index carries quantization sections";
            return -1;
        }
        return 0;
    }
    if (!img->codes) {
        *why = "quantized index without codes";
        return -1;
    }
    if (m[8] == DYN_VEC_BINARY) {
        uint64_t tail = d & 63 ? ~(((uint64_t)1 << (d & 63)) - 1) : 0;
        const uint64_t* b = (const uint64_t*)(const void*)img->codes;
        if (img->qparam || img->codes_bytes != n * w * 8) {
            *why = "binary code section size does not match";
            return -1;
        }
        for (i = 0; i < n; i++)
            if (b[i * w + w - 1] & tail) {
                *why = "binary code sets bits past dims";
                return -1;
            }
        return 0;
    }
    if (!img->qparam || img->qparam_bytes != 2 * d * 8 || img->codes_bytes != n * d) {
        *why = "sq8 section size does not match";
        return -1;
    }
    for (j = 0; j < d; j++) {
        double lo = img->qparam[j], hi = img->qparam[d + j];
        if (!isfinite(lo) || !isfinite(hi) || lo > hi || (!(m[7] & 8) && (lo != 0 || hi != 0))) {
            *why = "sq8 range is not finite and ordered";
            return -1;
        }
    }
    if (n && !(m[7] & 8)) {
        *why = "sq8 codes without a trained range";
        return -1;
    }
    for (i = 0; i < n * d; i++)
        if ((int8_t)img->codes[i] == -128) {
            *why = "sq8 code out of range";
            return -1;
        }
    return 0;
}

int dyn_vec_restore(dyn_vec_t* v, const dyn_vec_image_t* img, const char** why)
{
    const uint64_t* m = img->meta;
    dyn_vec_params_t p;
    uint64_t n, i, live = 0, cells;
    memset(v, 0, sizeof(*v));
    *why = NULL;
    if (m[0] != DYN_VEC_LAYOUT)
        VK_BAD("unknown layout version");
    if (m[1] < 1 || m[1] > DYN_VEC_MAX_DIM || m[2] > DYN_VEC_IP || m[3] > DYN_VEC_HNSW
        || m[4] < DYN_VEC_MIN_M || m[4] > DYN_VEC_MAX_M || m[5] < 1 || m[5] > DYN_VEC_MAX_EF
        || m[6] < 1 || m[6] > DYN_VEC_MAX_EF || m[7] > 15)
        VK_BAD("parameter out of range");
    if (m[8] > DYN_VEC_BINARY)
        VK_BAD("unknown quantization");
    if ((m[8] == DYN_VEC_F32 && (m[7] & 12)) || (m[8] == DYN_VEC_BINARY && (m[7] & 8)))
        VK_BAD("flags do not fit the quantization");
    if ((m[8] == DYN_VEC_F32 || (m[7] & 4)) ? m[15] != 0 : (m[15] < 1 || m[15] > DYN_VEC_MAX_POOL))
        VK_BAD("rerank factor out of range");
    if (m[10] > DYN_VEC_MAX_SIZE || m[11] > m[10])
        VK_BAD("size out of range");
    if (m[12] > DYN_VEC_NONE || (m[13] != UINT64_MAX && m[13] > DYN_VEC_MAX_LEVEL))
        VK_BAD("entry point out of range");
    n = m[10];
    cells = n * m[1];
    if (img->data_bytes != ((m[7] & 4) ? 0 : cells * 4) || img->ext_bytes != n * 8 || img->dead_bytes != n)
        VK_BAD("section size does not match the vector count");
    if (vk_restore_quant_check(m, img, why))
        goto bad;
    if (m[3] == DYN_VEC_FLAT
        && (img->lvl_bytes || img->links_bytes || m[14] || m[12] != DYN_VEC_NONE || m[13] != UINT64_MAX))
        VK_BAD("flat index carries graph data");
    if (m[3] == DYN_VEC_HNSW && img->lvl_bytes != n)
        VK_BAD("level section size does not match the vector count");
    for (i = 0; i < n; i++) {
        if (img->dead[i] > 1)
            VK_BAD("tombstone flag out of range");
        if (!img->dead[i]) {
            if (img->ext[i] == DYN_VEC_NOID)
                VK_BAD("live slot holds the reserved id");
            live++;
        }
    }
    if (live != m[11])
        VK_BAD("live count does not match the tombstones");
    if (!(m[7] & 4))
        for (i = 0; i < cells; i++)
            if (!isfinite(img->data[i]))
                VK_BAD("stored vector holds NaN or Infinity");
    memset(&p, 0, sizeof(p));
    p.dim = (uint32_t)m[1];
    p.metric = (uint32_t)m[2];
    p.kind = (uint32_t)m[3];
    p.M = (uint32_t)m[4];
    p.efc = (uint32_t)m[5];
    p.efs = (uint32_t)m[6];
    p.heuristic = (int)(m[7] & 1);
    p.exact_fallback = (int)((m[7] >> 1) & 1);
    p.quant = (uint32_t)m[8];
    p.rerank = !(m[7] & 4);
    p.pool = (uint32_t)m[15];
    if (dyn_vec_init(v, &p))
        goto bad;
    v->rng = m[9];
    v->n = (uint32_t)n;
    v->cap = (uint32_t)n;
    v->live = (uint32_t)live;
    v->entry = (uint32_t)m[12];
    v->top = (int)(int64_t)m[13];
    v->ext = (uint64_t*)malloc((size_t)(n ? n : 1) * sizeof(uint64_t));
    v->dead = (uint8_t*)malloc((size_t)(n ? n : 1));
    if (!v->ext || !v->dead)
        goto bad;
    if (n) {
        memcpy(v->ext, img->ext, (size_t)n * sizeof(uint64_t));
        memcpy(v->dead, img->dead, (size_t)n);
    }
    if (!v->keep) {
    } else if (img->borrow_data && n) {
        v->data = (float*)(uintptr_t)img->data;
        v->data_borrowed = 1;
    } else {
        v->data = (float*)malloc((size_t)(cells ? cells : 1) * sizeof(float));
        if (!v->data)
            goto bad;
        if (cells)
            memcpy(v->data, img->data, (size_t)cells * sizeof(float));
    }
    if (v->quant != DYN_VEC_F32) {
        if (vk_quant_alloc(v, (uint32_t)(n ? n : 1)))
            goto bad;
        if (n)
            memcpy(v->codes, img->codes, (size_t)n * v->cb);
        if (v->quant == DYN_VEC_SQ8) {
            memcpy(v->lo, img->qparam, (size_t)v->dim * sizeof(double));
            memcpy(v->hi, img->qparam + v->dim, (size_t)v->dim * sizeof(double));
            v->trained = (uint8_t)((m[7] >> 3) & 1);
            vk_sq8_refresh(v);
        }
    }
    if (v->kind == DYN_VEC_HNSW && vk_restore_links(v, img, why))
        goto bad;
    if (n && vk_rehash(v, (uint32_t)n))
        goto bad;
    for (i = 0; i < n; i++)
        if (!v->dead[i] && dyn_vec_find(v, v->ext[i]) != (uint32_t)i)
            VK_BAD("live id appears twice");
    return DYN_VEC_OK;
bad:
    v->n = 0;
    dyn_vec_free(v);
    return *why ? DYN_VEC_ECORRUPT : DYN_VEC_ENOMEM;
}
