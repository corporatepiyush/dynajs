#include <stdlib.h>
#include "dd.h"

static int exec_sorted(dd_view v, const dd_idvec* hits, const dd_sort* sort, uint64_t skip, uint64_t limit,
    dd_idvec* out, dd_err* e)
{
    dyn_ks_buf kb;
    dd_sortrec* r;
    uint32_t i;
    int kept;
    uint64_t want = limit ? skip + limit : 0;
    if (hits->n == 0)
        return 0;
    r = malloc((size_t)hits->n * sizeof *r);
    if (!r)
        return dd_fail(e, DD_E_OOM, "out of memory");
    dyn_ks_init(&kb);
    for (i = 0; i < hits->n; i++) {
        size_t from = kb.n;
        dd_sort_key(sort, v.docs[hits->ids[i]], &kb);
        if (kb.oom || kb.n - from > UINT32_MAX || kb.n > UINT32_MAX) {
            free(r);
            dyn_ks_free(&kb);
            return dd_fail(e, DD_E_OOM, "out of memory building sort keys");
        }
        r[i].off = (uint32_t)from;
        r[i].len = (uint32_t)(kb.n - from);
        r[i].id = hits->ids[i];
    }
    kept = dd_sort_records(r, hits->n, kb.p, want);
    if (kept < 0) {
        free(r);
        dyn_ks_free(&kb);
        return dd_fail(e, DD_E_OOM, "out of memory");
    }
    for (i = (uint32_t)(skip < (uint64_t)kept ? skip : (uint64_t)kept); i < (uint32_t)kept; i++) {
        if (dd_idvec_push(out, r[i].id) < 0) {
            free(r);
            dyn_ks_free(&kb);
            return dd_fail(e, DD_E_OOM, "out of memory");
        }
    }
    free(r);
    dyn_ks_free(&kb);
    return 0;
}

int dd_exec_find(dd_view v, const uint32_t* cand, uint32_t ncand, int all, const dd_filter* f, const dd_sort* sort,
    uint64_t skip, uint64_t limit, dd_idvec* out, dd_stats* st, dd_err* e)
{
    dd_idvec hits = { NULL, 0, 0 };
    uint32_t i, n = all ? v.ndocs : ncand;
    int sorting = sort && sort->n > 0;
    int filtering = f && !f->empty;
    uint64_t seen = 0;
    int rc = 0;
    for (i = 0; i < n; i++) {
        uint32_t id = all ? i : cand[i];
        const uint8_t* doc;
        if (id >= v.ndocs || !(doc = v.docs[id]))
            continue;
        if (st)
            st->docs_examined++;
        if (filtering) {
            dd_match_ctx mc = { .pos = -1, .err = e };
            int m = dd_filter_match(f, doc, &mc);
            if (mc.failed) {
                rc = -1;
                goto out;
            }
            if (!m)
                continue;
        }
        if (sorting) {
            if (dd_idvec_push(&hits, id) < 0) {
                rc = dd_fail(e, DD_E_OOM, "out of memory");
                goto out;
            }
            continue;
        }
        if (seen++ < skip)
            continue;
        if (dd_idvec_push(out, id) < 0) {
            rc = dd_fail(e, DD_E_OOM, "out of memory");
            goto out;
        }
        if (limit && out->n >= limit)
            break;
    }
    if (sorting)
        rc = exec_sorted(v, &hits, sort, skip, limit, out, e);
out:
    if (st)
        st->n_returned = out->n;
    dd_idvec_free(&hits);
    return rc;
}
