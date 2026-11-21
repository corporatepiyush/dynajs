#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libregexp.h"
#include "core/docdb/dd.h"

int lre_check_stack_overflow(void* opaque, size_t alloca_size)
{
    (void)opaque;
    (void)alloca_size;
    return 0;
}

void* lre_realloc(void* opaque, void* ptr, size_t size)
{
    (void)opaque;
    if (size == 0) {
        free(ptr);
        return NULL;
    }
    return realloc(ptr, size);
}

int lre_check_timeout(void* opaque)
{
    (void)opaque;
    return 0;
}

static dd_db* fz_db;
static dd_coll* fz_plain;
static dd_coll* fz_ix;

static void put_val(dyn_bson_w* w, const char* k, unsigned sel, unsigned depth)
{
    static const char* strs[] = { "", "a", "b", "ab", "A", "\xc3\xa9", "\xf0\x9f\x98\x80", "a\0b" };
    static const size_t slen[] = { 0, 1, 1, 2, 1, 2, 4, 3 };
    size_t kl = strlen(k);
    uint8_t oid[12] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, (uint8_t)sel };
    uint8_t dec[16] = { 0 };
    switch (sel % 19) {
    case 0:
        dyn_bson_w_int32(w, k, kl, (int32_t)(sel % 5) - 2);
        break;
    case 1:
        dyn_bson_w_double(w, k, kl, (double)(sel % 7) / 2.0);
        break;
    case 2:
        dyn_bson_w_double(w, k, kl, sel & 1 ? -0.0 : 0.0 / 0.0);
        break;
    case 3:
        dyn_bson_w_double(w, k, kl, sel & 1 ? 1.0 / 0.0 : -1.0 / 0.0);
        break;
    case 4:
        dyn_bson_w_int64(w, k, kl, (int64_t)1 << (50 + sel % 12));
        break;
    case 5:
        dyn_bson_w_string(w, k, kl, strs[sel % 8], slen[sel % 8]);
        break;
    case 6:
        dyn_bson_w_null(w, k, kl);
        break;
    case 7:
        dyn_bson_w_bool(w, k, kl, (int)(sel & 1));
        break;
    case 8:
        dyn_bson_w_date(w, k, kl, (int64_t)sel * 86400000 - 1);
        break;
    case 9:
        dyn_bson_w_oid(w, k, kl, oid);
        break;
    case 10:
        dec[0] = (uint8_t)(sel % 9);
        dec[14] = 0x40;
        dec[15] = 0x30;
        dyn_bson_w_dec128(w, k, kl, dec);
        break;
    case 11:
        dyn_bson_w_regex(w, k, kl, "^a", 2, "i", 1);
        break;
    case 12:
        dyn_bson_w_key(w, DYN_BSON_UNDEFINED, k, kl);
        break;
    case 13:
    case 14:
    case 15:
        if (depth < 4) {
            unsigned i, n = sel % 4;
            dyn_bson_w_sub_begin(w, DYN_BSON_ARRAY, k, kl);
            for (i = 0; i < n; i++) {
                char ik[4];
                snprintf(ik, sizeof ik, "%u", i);
                put_val(w, ik, sel * 7 + i * 3 + 1, depth + 1);
            }
            dyn_bson_w_sub_end(w);
            break;
        }
        dyn_bson_w_int32(w, k, kl, 1);
        break;
    default:
        if (depth < 4) {
            dyn_bson_w_sub_begin(w, DYN_BSON_DOC, k, kl);
            put_val(w, "x", sel * 5 + 2, depth + 1);
            if (sel & 2)
                put_val(w, "y", sel * 3 + 1, depth + 1);
            dyn_bson_w_sub_end(w);
            break;
        }
        dyn_bson_w_string(w, k, kl, "z", 1);
        break;
    }
}

static void fz_init(void)
{
    static const char* ixs[][3] = { { "a", NULL, NULL }, { "b", NULL, NULL }, { "arr.x", NULL, NULL },
        { "arr", NULL, NULL }, { "a", "b", NULL }, { "n.e", NULL, NULL }, { "s", NULL, NULL }, { "s", "t", NULL },
        { "t", NULL, "p" }, { "u", NULL, "s" } };
    dd_err e = { 0 };
    unsigned d, i;
    fz_db = dd_db_new("fuzz");
    fz_plain = dd_db_create(fz_db, "plain", 5, &e);
    fz_ix = dd_db_create(fz_db, "ix", 2, &e);
    if (!fz_plain || !fz_ix)
        abort();
    for (d = 0; d < 96; d++) {
        dyn_bson_w w;
        dyn_bson_w_init(&w);
        dyn_bson_w_doc_begin(&w);
        dyn_bson_w_int32(&w, "_id", 3, (int32_t)d);
        if (d % 5)
            put_val(&w, "a", d * 13 + 1, 0);
        if (d % 3)
            put_val(&w, "b", d * 7 + 5, 0);
        if (d % 4 != 1)
            put_val(&w, "arr", d * 11 + 13, 0);
        if (d % 6) {
            dyn_bson_w_sub_begin(&w, DYN_BSON_DOC, "n", 1);
            put_val(&w, "e", d * 17 + 3, 1);
            dyn_bson_w_sub_end(&w);
        }
        if (d % 7)
            put_val(&w, "s", (d * 5 + 1) % 13, 0);
        put_val(&w, "t", (d * 3 + 2) % 13, 0);
        if (d % 2)
            put_val(&w, "u", (d * 11 + 4) % 13, 0);
        dyn_bson_w_doc_end(&w);
        if (w.oom || dd_coll_insert(fz_plain, w.p, w.n, NULL, &e) < 0 || dd_coll_insert(fz_ix, w.p, w.n, NULL, &e) < 0)
            abort();
        dyn_bson_w_free(&w);
    }
    for (i = 0; i < sizeof ixs / sizeof ixs[0]; i++) {
        dyn_bson_w w, o;
        dd_index* ix;
        dyn_bson_w_init(&w);
        dyn_bson_w_init(&o);
        dyn_bson_w_doc_begin(&w);
        dyn_bson_w_int32(&w, ixs[i][0], strlen(ixs[i][0]), ixs[i][2] && ixs[i][2][0] == 's' ? -1 : 1);
        if (ixs[i][1])
            dyn_bson_w_int32(&w, ixs[i][1], strlen(ixs[i][1]), -1);
        dyn_bson_w_doc_end(&w);
        dyn_bson_w_doc_begin(&o);
        if (ixs[i][2] && ixs[i][2][0] == 's')
            dyn_bson_w_bool(&o, "sparse", 6, 1);
        if (ixs[i][2] && ixs[i][2][0] == 'p') {
            dyn_bson_w_string(&o, "name", 4, "t_part", 6);
            dyn_bson_w_sub_begin(&o, DYN_BSON_DOC, "partialFilterExpression", 23);
            dyn_bson_w_sub_begin(&o, DYN_BSON_DOC, "s", 1);
            dyn_bson_w_int32(&o, "$gt", 3, 0);
            dyn_bson_w_sub_end(&o);
            dyn_bson_w_sub_end(&o);
        }
        dyn_bson_w_doc_end(&o);
        e.code = 0;
        if (dd_index_create(fz_ix, w.p, o.p, &ix, &e) < 0 && e.code != DD_E_PARALLELARRAYS)
            abort();
        dyn_bson_w_free(&o);
        dyn_bson_w_free(&w);
    }
}

static int run(dd_coll* c, const dd_filter* f, const dd_sort* s, uint64_t skip, uint64_t limit, dd_idvec* out, int* code)
{
    dd_plan plan;
    dd_idvec cand = { NULL, 0, 0 };
    dd_stats st;
    dd_err e = { 0 };
    dd_view v;
    int all = 0, rc;
    memset(&st, 0, sizeof st);
    *code = 0;
    if (dd_plan_make(c, f, s, &plan, &e) < 0) {
        *code = e.code;
        return -1;
    }
    rc = s && limit ? dd_plan_topk(c, &plan, f, skip + limit, &cand, &st) : 1;
    if (rc < 0 || (rc > 0 && dd_plan_candidates(c, &plan, &cand, &all, &st) < 0))
        abort();
    v.docs = c->doc;
    v.ndocs = c->nslots;
    rc = dd_exec_find(v, cand.ids, cand.n, all, f, st.used_index_sort ? NULL : s, skip, limit, out, &st, &e);
    if (rc < 0)
        *code = e.code;
    if (rc == 0 && !s && !(limit || skip)) {
        uint64_t n = 0;
        if (dd_plan_count(c, &plan, f, &n) > 0 && n != out->n)
            abort();
    }
    dd_idvec_free(&cand);
    dd_plan_free(&plan);
    return rc;
}

static void fz_update(const uint8_t* spec)
{
    dd_upd* u = NULL;
    dd_err e = { 0 };
    uint32_t d;
    if (dd_update_parse(&u, spec, NULL, &e) < 0)
        return;
    for (d = 0; d < fz_plain->nslots; d++) {
        dyn_bson_w out;
        dd_upd_desc desc;
        dd_err ue = { 0 };
        if (!fz_plain->doc[d])
            continue;
        dyn_bson_w_init(&out);
        dd_desc_init(&desc);
        if (dd_update_apply(u, fz_plain->doc[d], -1, 0, 0, &out, &desc, &ue) == 0 && !out.oom) {
            dyn_bson_err be;
            if (dyn_bson_validate(out.p, out.n, 200, &be) != 0)
                abort();
        }
        dd_desc_free(&desc);
        dyn_bson_w_free(&out);
    }
    dd_update_free(u);
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    static const char* sorts[] = { "a", "b", "arr.x", "arr", "n.e", "s", "t", "u" };
    dd_filter f;
    dd_sort s;
    dd_err e = { 0 };
    dyn_bson_err be;
    dd_idvec a = { NULL, 0, 0 }, b = { NULL, 0, 0 };
    uint8_t* doc;
    uint8_t mode;
    int ca, cb, ra, rb, have_sort = 0;
    uint64_t skip, limit;
    if (!fz_db)
        fz_init();
    if (size < 6)
        return 0;
    mode = data[0];
    data++;
    size--;
    if (size > (1u << 16) || dyn_bson_rd32(data) != size || dyn_bson_validate(data, size, 200, &be) != 0)
        return 0;
    doc = malloc(size);
    if (!doc)
        return 0;
    memcpy(doc, data, size);
    if (mode & 0x80) {
        fz_update(doc);
        free(doc);
        return 0;
    }
    if (dd_filter_compile(&f, doc, NULL, &e) < 0) {
        free(doc);
        return 0;
    }
    memset(&s, 0, sizeof s);
    if (mode & 1) {
        dyn_bson_w w;
        const char* k = sorts[(mode >> 1) % 8];
        dyn_bson_w_init(&w);
        dyn_bson_w_doc_begin(&w);
        dyn_bson_w_int32(&w, k, strlen(k), mode & 0x40 ? -1 : 1);
        dyn_bson_w_int32(&w, "_id", 3, 1);
        dyn_bson_w_doc_end(&w);
        have_sort = dd_sort_parse(&s, w.p, &e) == 0;
        dyn_bson_w_free(&w);
    }
    skip = (mode >> 4) & 1 ? 2 : 0;
    limit = (mode >> 5) & 1 ? 3 : 0;
    if (f.has_regex) {
        dd_filter_free(&f);
        free(doc);
        return 0;
    }
    ra = run(fz_plain, &f, have_sort ? &s : NULL, skip, limit, &a, &ca);
    rb = run(fz_ix, &f, have_sort ? &s : NULL, skip, limit, &b, &cb);
    if (ra != rb || ca != cb)
        abort();
    if (ra == 0) {
        uint32_t i;
        if (a.n != b.n)
            abort();
        for (i = 0; i < a.n; i++)
            if (a.ids[i] != b.ids[i])
                abort();
    }
    dd_idvec_free(&a);
    dd_idvec_free(&b);
    if (have_sort)
        dd_sort_free(&s);
    dd_filter_free(&f);
    free(doc);
    return 0;
}

#ifdef DD_FUZZ_EMIT_SEEDS
static void seed(const char* dir, const char* name, uint8_t mode, dyn_bson_w* w)
{
    char path[512];
    FILE* fp;
    snprintf(path, sizeof path, "%s/%s.bin", dir, name);
    fp = fopen(path, "wb");
    if (!fp || fwrite(&mode, 1, 1, fp) != 1 || fwrite(w->p, 1, w->n, fp) != w->n || fclose(fp) != 0) {
        fprintf(stderr, "docdb-seeds: cannot write %s\n", path);
        exit(1);
    }
    dyn_bson_w_reset(w);
}

int main(int argc, char** argv)
{
    dyn_bson_w w;
    const char* dir = argc > 1 ? argv[1] : ".";
    dyn_bson_w_init(&w);
    dyn_bson_w_doc_begin(&w);
    dyn_bson_w_int32(&w, "a", 1, 1);
    dyn_bson_w_doc_end(&w);
    seed(dir, "valid_eq", 0, &w);
    dyn_bson_w_doc_begin(&w);
    dyn_bson_w_null(&w, "arr.x", 5);
    dyn_bson_w_doc_end(&w);
    seed(dir, "valid_null_through_array", 0, &w);
    dyn_bson_w_doc_begin(&w);
    dyn_bson_w_sub_begin(&w, DYN_BSON_DOC, "a", 1);
    dyn_bson_w_double(&w, "$gte", 4, 0.5);
    dyn_bson_w_string(&w, "$lt", 3, "b", 1);
    dyn_bson_w_sub_end(&w);
    dyn_bson_w_doc_end(&w);
    seed(dir, "valid_range_mixed_types", 0x31, &w);
    dyn_bson_w_doc_begin(&w);
    dyn_bson_w_sub_begin(&w, DYN_BSON_DOC, "b", 1);
    dyn_bson_w_sub_begin(&w, DYN_BSON_ARRAY, "$in", 3);
    dyn_bson_w_null(&w, "0", 1);
    dyn_bson_w_int32(&w, "1", 1, 2);
    dyn_bson_w_string(&w, "2", 1, "a", 1);
    dyn_bson_w_sub_end(&w);
    dyn_bson_w_sub_end(&w);
    dyn_bson_w_doc_end(&w);
    seed(dir, "valid_in_mixed", 0x03, &w);
    dyn_bson_w_doc_begin(&w);
    dyn_bson_w_sub_begin(&w, DYN_BSON_ARRAY, "$or", 3);
    dyn_bson_w_sub_begin(&w, DYN_BSON_DOC, "0", 1);
    dyn_bson_w_sub_begin(&w, DYN_BSON_DOC, "arr", 3);
    dyn_bson_w_sub_begin(&w, DYN_BSON_DOC, "$elemMatch", 10);
    dyn_bson_w_int32(&w, "$gt", 3, 0);
    dyn_bson_w_sub_end(&w);
    dyn_bson_w_sub_end(&w);
    dyn_bson_w_sub_end(&w);
    dyn_bson_w_sub_begin(&w, DYN_BSON_DOC, "1", 1);
    dyn_bson_w_sub_begin(&w, DYN_BSON_DOC, "n.e", 3);
    dyn_bson_w_bool(&w, "$exists", 7, 0);
    dyn_bson_w_sub_end(&w);
    dyn_bson_w_sub_end(&w);
    dyn_bson_w_sub_end(&w);
    dyn_bson_w_doc_end(&w);
    seed(dir, "valid_or_elemmatch_exists", 0x45, &w);
    dyn_bson_w_doc_begin(&w);
    dyn_bson_w_sub_begin(&w, DYN_BSON_DOC, "a", 1);
    dyn_bson_w_sub_begin(&w, DYN_BSON_DOC, "$not", 4);
    dyn_bson_w_sub_begin(&w, DYN_BSON_DOC, "$size", 5);
    dyn_bson_w_sub_end(&w);
    dyn_bson_w_sub_end(&w);
    dyn_bson_w_sub_end(&w);
    dyn_bson_w_doc_end(&w);
    seed(dir, "hostile_not_size_doc", 0, &w);
    dyn_bson_w_doc_begin(&w);
    dyn_bson_w_sub_begin(&w, DYN_BSON_DOC, "s", 1);
    dyn_bson_w_int32(&w, "$lt", 3, 2);
    dyn_bson_w_sub_end(&w);
    dyn_bson_w_doc_end(&w);
    seed(dir, "valid_scalar_upper_only", 0, &w);
    dyn_bson_w_doc_begin(&w);
    dyn_bson_w_sub_begin(&w, DYN_BSON_DOC, "t", 1);
    dyn_bson_w_double(&w, "$gte", 4, -1.5);
    dyn_bson_w_sub_end(&w);
    dyn_bson_w_sub_begin(&w, DYN_BSON_DOC, "s", 1);
    dyn_bson_w_int32(&w, "$gt", 3, 0);
    dyn_bson_w_sub_end(&w);
    dyn_bson_w_doc_end(&w);
    seed(dir, "valid_partial_compound_sorted", 0x2d, &w);
    dyn_bson_w_doc_begin(&w);
    dyn_bson_w_null(&w, "u", 1);
    dyn_bson_w_doc_end(&w);
    seed(dir, "valid_sparse_null", 0x0e, &w);
    dyn_bson_w_doc_begin(&w);
    dyn_bson_w_sub_begin(&w, DYN_BSON_DOC, "$set", 4);
    dyn_bson_w_int32(&w, "arr.1.x", 7, 5);
    dyn_bson_w_sub_end(&w);
    dyn_bson_w_sub_begin(&w, DYN_BSON_DOC, "$push", 5);
    dyn_bson_w_null(&w, "b", 1);
    dyn_bson_w_sub_end(&w);
    dyn_bson_w_doc_end(&w);
    seed(dir, "valid_update_set_push", 0x80, &w);
    dyn_bson_w_free(&w);
    return 0;
}
#endif
