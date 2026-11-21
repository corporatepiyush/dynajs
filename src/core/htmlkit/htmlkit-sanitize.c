#include "htmlkit.h"

void san_free(san_t* s)
{
    uint32_t i, k;
    if (!s)
        return;
    for (i = 0; i < s->n; i++) {
        for (k = 0; k < s->rules[i].nattr; k++)
            free(s->rules[i].attrs[k]);
        free(s->rules[i].attrs);
        free(s->rules[i].tag);
    }
    for (i = 0; i < s->nproto; i++)
        free(s->protos[i]);
    free(s->protos);
    free(s->rules);
    free(s);
}

void san_free_v(void* p) { san_free((san_t*)p); }

char* san_lower_dup(const char* s)
{
    size_t i, n = strlen(s);
    char* d = sel_dup(s, n);
    if (d)
        for (i = 0; i < n; i++)
            d[i] = (char)ht_lower((unsigned char)d[i]);
    return d;
}
