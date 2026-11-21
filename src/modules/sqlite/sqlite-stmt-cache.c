#include "sqlite-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_SQLITE) && defined(CONFIG_SQLITE)

sql_cache_ent_t* sql_cache_find(dyn_sqlite_t* s, const char* sql,
    size_t n, uint32_t h)
{
    int i;
    for (i = 0; i < s->nents; i++)
        if (s->ents[i].hash == h && s->ents[i].sqllen == n && memcmp(s->ents[i].sql, sql, n) == 0) {
            s->ents[i].lastuse = ++s->use_clock;
            return &s->ents[i];
        }
    return NULL;
}

int sql_cache_put(dyn_sqlite_t* s, const char* sql, size_t n,
    uint32_t h, sqlite3_stmt* st)
{
    sql_cache_ent_t* e;

    if (s->cache_max <= 0 || n > (size_t)s->cache_max * 4096)
        return -1;
    if (s->nents >= s->cache_max) {
        int i, victim = 0;
        for (i = 1; i < s->nents; i++)
            if (s->ents[i].lastuse < s->ents[victim].lastuse)
                victim = i;
        free(s->ents[victim].sql);
        sqlite3_finalize(s->ents[victim].st);
        s->ents[victim] = s->ents[--s->nents];
    } else if (s->nents == s->cap_ents) {
        int cap = s->cap_ents ? s->cap_ents * 2 : 8;
        sql_cache_ent_t* ne = (sql_cache_ent_t*)realloc(s->ents,
            (size_t)cap * sizeof(*ne));
        if (!ne)
            return -1;
        s->ents = ne;
        s->cap_ents = cap;
    }
    e = &s->ents[s->nents++];
    e->sql = (char*)malloc(n + 1);
    if (!e->sql) {
        s->nents--;
        return -1;
    }
    memcpy(e->sql, sql, n);
    e->sql[n] = '\0';
    e->sqllen = n;
    e->hash = h;
    e->st = st;
    e->lastuse = ++s->use_clock;
    return 0;
}

void sql_cache_drop_stmt(dyn_sqlite_t* s, sqlite3_stmt* st)
{
    int i;
    for (i = 0; i < s->nents; i++)
        if (s->ents[i].st == st) {
            free(s->ents[i].sql);
            s->ents[i] = s->ents[--s->nents];
            return;
        }
}

void sql_cache_clear(dyn_sqlite_t* s)
{
    int i;
    for (i = 0; i < s->nents; i++) {
        free(s->ents[i].sql);
        sqlite3_finalize(s->ents[i].st);
    }
    free(s->ents);
    s->ents = NULL;
    s->nents = s->cap_ents = 0;
}

#endif
