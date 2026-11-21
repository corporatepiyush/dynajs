#include "scrape-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_SCRAPE)
#define FE_MAX_HOSTS 256

fe_host_t* fe_host(fe_t* f, const char* host)
{
    size_t i;
    for (i = 0; i < f->n_hosts; i++)
        if (!strcmp(f->hosts[i].host, host))
            return &f->hosts[i];
    if (f->n_hosts >= FE_MAX_HOSTS) {
        fe_host_t* v = &f->hosts[0];
        free(v->host);
        if (v->robots)
            rb_free(v->robots);
        memset(v, 0, sizeof(*v));
        v->host = strdup(host);
        return v->host ? v : NULL;
    }
    if (f->n_hosts == f->cap_hosts) {
        size_t nc = f->cap_hosts ? f->cap_hosts * 2 : 8;
        fe_host_t* nh = (fe_host_t*)realloc(f->hosts, nc * sizeof(*nh));
        if (!nh)
            return NULL;
        f->hosts = nh;
        f->cap_hosts = nc;
    }
    memset(&f->hosts[f->n_hosts], 0, sizeof(fe_host_t));
    f->hosts[f->n_hosts].host = strdup(host);
    if (!f->hosts[f->n_hosts].host)
        return NULL;
    return &f->hosts[f->n_hosts++];
}

double fe_host_floor_ms(const fe_t* f, const fe_host_t* h)
{
    double floor_ms = f->min_delay_ms;
    if (h->robots && h->robots->delay > 0) {
        double cd = h->robots->delay * 1000.0;
        if (cd > floor_ms)
            floor_ms = cd;
    }
    return floor_ms;
}
#endif
