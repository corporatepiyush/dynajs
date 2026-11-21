#include "scrape-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_SCRAPE)
#define FE_CACHE_MAX_BYTES (4u << 20)
#define FE_CACHE_BODY_CAP (1u << 20)

void fe_cache_clear(fe_cache_ent_t* e)
{
    free(e->url);
    free(e->etag);
    free(e->lm);
    free(e->ct);
    free(e->body);
    memset(e, 0, sizeof(*e));
}

fe_cache_ent_t* fe_cache_find(fe_t* f, const char* url)
{
    size_t i;
    for (i = 0; i < f->n_cache; i++)
        if (!strcmp(f->cache[i].url, url))
            return &f->cache[i];
    return NULL;
}

static char* fe_strdup_n(const char* s, size_t n)
{
    char* o;
    if (n > FE_CACHE_BODY_CAP)
        return NULL;
    o = (char*)malloc(n + 1);
    if (!o)
        return NULL;
    memcpy(o, s, n);
    o[n] = 0;
    return o;
}

static void fe_cache_evict_url(fe_t* f, const char* url)
{
    size_t i;
    for (i = 0; i < f->n_cache; i++)
        if (!strcmp(f->cache[i].url, url)) {
            fe_cache_clear(&f->cache[i]);
            memmove(&f->cache[i], &f->cache[i + 1],
                (f->n_cache - i - 1) * sizeof(*f->cache));
            f->n_cache--;
            return;
        }
}

fe_cache_ent_t* fe_cache_put(fe_t* f, const char* url,
    const char* etag, const char* lm,
    const char* ct,
    const char* body, size_t blen)
{
    fe_cache_ent_t* e;
    double tot;
    size_t i;

    if (!etag && !lm)
        return NULL;
    if (!body || blen > FE_CACHE_BODY_CAP)
        return NULL;
    fe_cache_evict_url(f, url);

    tot = (double)(blen + strlen(url));
    for (i = 0; i < f->n_cache; i++)
        tot += (double)f->cache[i].blen;
    while ((f->n_cache >= FE_CACHE_MAX_ENTRIES || tot > (double)FE_CACHE_MAX_BYTES) && f->n_cache) {
        tot -= (double)f->cache[0].blen;
        fe_cache_clear(&f->cache[0]);
        memmove(&f->cache[0], &f->cache[1],
            (f->n_cache - 1) * sizeof(*f->cache));
        f->n_cache--;
    }
    if (tot > (double)FE_CACHE_MAX_BYTES)
        return NULL;

    e = &f->cache[f->n_cache++];
    memset(e, 0, sizeof(*e));
    e->url = strdup(url);
    if (!e->url) {
        f->n_cache--;
        memset(e, 0, sizeof(*e));
        return NULL;
    }
    e->etag = etag ? strdup(etag) : NULL;
    e->lm = lm ? strdup(lm) : NULL;
    e->ct = ct ? strdup(ct) : NULL;
    if ((etag && !e->etag) || (lm && !e->lm) || (ct && !e->ct)) {
        fe_cache_evict_url(f, url);
        return NULL;
    }
    e->body = fe_strdup_n(body, blen);
    if (!e->body) {
        fe_cache_evict_url(f, url);
        return NULL;
    }
    e->blen = blen;
    return e;
}
#endif
