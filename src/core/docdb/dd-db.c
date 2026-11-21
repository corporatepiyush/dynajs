#include <stdlib.h>
#include "dd.h"

dd_db* dd_db_new(const char* name)
{
    dd_db* db = calloc(1, sizeof *db);
    if (!db)
        return NULL;
    db->name = strdup(name ? name : "test");
    if (!db->name) {
        free(db);
        return NULL;
    }
    db->ring_default = 4096;
    return db;
}

static void dd_chunk_free(dd_chunk* c)
{
    if (c->owns_data)
        free(c->data);
    free(c);
}

void dd_db_free(dd_db* db)
{
    int i;
    if (!db)
        return;
    for (i = 0; i < db->ncoll; i++)
        dd_coll_free(db->coll[i]);
    free(db->coll);
    db->npins = 0;
    dd_db_sweep(db);
    free(db->pins);
    free(db->name);
    free(db);
}

dd_coll* dd_db_find(dd_db* db, const char* name, size_t n)
{
    int i;
    for (i = 0; i < db->ncoll; i++)
        if (db->coll[i]->name_n == n && memcmp(db->coll[i]->name, name, n) == 0)
            return db->coll[i];
    return NULL;
}

int dd_db_install(dd_db* db, dd_coll* c)
{
    if (db->ncoll == db->capcoll) {
        int nc = db->capcoll ? db->capcoll * 2 : 8;
        dd_coll** np = realloc(db->coll, (size_t)nc * sizeof *np);
        if (!np)
            return -1;
        db->coll = np;
        db->capcoll = nc;
    }
    c->db = db;
    c->arena.db = db;
    db->coll[db->ncoll++] = c;
    return 0;
}

dd_coll* dd_db_create(dd_db* db, const char* name, size_t n, dd_err* e)
{
    dd_coll* c;
    if (dd_coll_name_ok(name, n, e) < 0)
        return NULL;
    c = dd_coll_new(db, name, n);
    if (!c || dd_db_install(db, c) < 0) {
        dd_coll_free(c);
        dd_fail(e, DD_E_OOM, "out of memory");
        return NULL;
    }
    return c;
}

int dd_db_drop(dd_db* db, const char* name, size_t n)
{
    int i;
    for (i = 0; i < db->ncoll; i++) {
        if (db->coll[i]->name_n == n && memcmp(db->coll[i]->name, name, n) == 0) {
            if (db->on_drop)
                db->on_drop(db, db->coll[i], db->owner);
            dd_coll_free(db->coll[i]);
            memmove(db->coll + i, db->coll + i + 1, (size_t)(db->ncoll - i - 1) * sizeof *db->coll);
            db->ncoll--;
            return 1;
        }
    }
    return 0;
}

uint64_t dd_db_pin(dd_db* db)
{
    uint64_t ep;
    if (db->npins == db->cappins) {
        int nc = db->cappins ? db->cappins * 2 : 8;
        uint64_t* np = realloc(db->pins, (size_t)nc * sizeof *np);
        if (!np)
            return 0;
        db->pins = np;
        db->cappins = nc;
    }
    ep = ++db->epoch;
    db->pins[db->npins++] = ep;
    return ep;
}

void dd_db_unpin(dd_db* db, uint64_t epoch)
{
    int i;
    for (i = 0; i < db->npins; i++) {
        if (db->pins[i] == epoch) {
            db->pins[i] = db->pins[--db->npins];
            break;
        }
    }
    dd_db_sweep(db);
}

void dd_db_retire(dd_db* db, dd_chunk* c)
{
    if (!db || db->npins == 0) {
        dd_chunk_free(c);
        return;
    }
    c->retired_epoch = db->epoch;
    c->next = db->retired;
    c->prev = NULL;
    db->retired = c;
    db->retired_bytes += c->cap;
}

void dd_db_sweep(dd_db* db)
{
    uint64_t min = UINT64_MAX;
    dd_chunk **pp = &db->retired, *c;
    int i;
    for (i = 0; i < db->npins; i++)
        if (db->pins[i] < min)
            min = db->pins[i];
    while ((c = *pp)) {
        if (db->npins == 0 || c->retired_epoch < min) {
            *pp = c->next;
            db->retired_bytes -= c->cap;
            dd_chunk_free(c);
        } else {
            pp = &c->next;
        }
    }
}
