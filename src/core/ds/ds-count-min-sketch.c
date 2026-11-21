#include "ds-internal.h"
#include "ds-count-min-sketch.h"

struct dyn_cms {
    uint64_t* c;
    uint32_t width, depth;
    uint64_t total;
};

dyn_cms_t* dyn_cms_new(uint32_t width, uint32_t depth)
{
    dyn_cms_t* s;
    if (width == 0 || depth == 0)
        return NULL;
    if ((uint64_t)width * depth > (uint64_t)1 << 24)
        return NULL;
    s = (dyn_cms_t*)ds_calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->c = (uint64_t*)ds_calloc((size_t)width * depth, sizeof(uint64_t));
    if (!s->c) {
        DYN_DS_FREE(s);
        return NULL;
    }
    s->width = width;
    s->depth = depth;
    return s;
}

void dyn_cms_free(dyn_cms_t* s)
{
    if (!s)
        return;
    DYN_DS_FREE(s->c);
    DYN_DS_FREE(s);
}

static void ds_cms_probe(const dyn_cms_t* s, const char* k, size_t n,
    uint64_t* h1, uint64_t* h2)
{
    (void)s;
    *h1 = dyn_ds_xxh64((const uint8_t*)k, n, 0);
    *h2 = ds_mix64(*h1 ^ 0x9E3779B97F4A7C15ULL) | 1;
}

void dyn_cms_add(dyn_cms_t* s, const char* k, size_t n, uint64_t count)
{
    uint64_t h1, h2;
    uint32_t i;
    ds_cms_probe(s, k, n, &h1, &h2);
    for (i = 0; i < s->depth; i++) {
        uint32_t col = (uint32_t)((h1 + (uint64_t)i * h2) % s->width);
        s->c[(size_t)i * s->width + col] = ds_u64_sat_add(s->c[(size_t)i * s->width + col], count);
    }
    s->total = ds_u64_sat_add(s->total, count);
}

uint64_t dyn_cms_count(const dyn_cms_t* s, const char* k, size_t n)
{
    uint64_t h1, h2, best = UINT64_MAX;
    uint32_t i;
    ds_cms_probe(s, k, n, &h1, &h2);
    for (i = 0; i < s->depth; i++) {
        uint32_t col = (uint32_t)((h1 + (uint64_t)i * h2) % s->width);
        uint64_t v = s->c[(size_t)i * s->width + col];
        if (v < best)
            best = v;
    }
    return best;
}

uint64_t dyn_cms_total(const dyn_cms_t* s) { return s->total; }

uint32_t dyn_cms_width(const dyn_cms_t* s) { return s->width; }

uint32_t dyn_cms_depth(const dyn_cms_t* s) { return s->depth; }

int dyn_cms_merge(dyn_cms_t* a, const dyn_cms_t* b)
{
    size_t i, n;
    if (a->width != b->width || a->depth != b->depth)
        return -1;
    n = (size_t)a->width * a->depth;
    for (i = 0; i < n; i++)
        a->c[i] = ds_u64_sat_add(a->c[i], b->c[i]);
    a->total = ds_u64_sat_add(a->total, b->total);
    return 0;
}

const uint64_t* dyn_cms_counters(const dyn_cms_t* s) { return s->c; }

uint64_t* dyn_cms_counters_mut(dyn_cms_t* s) { return s->c; }

void dyn_cms_set_total(dyn_cms_t* s, uint64_t total) { s->total = total; }

uint64_t* dyn_cms_swap_counters(dyn_cms_t* s, uint64_t* c)
{
    uint64_t* old = s->c;
    s->c = c;
    return old;
}
