#include "mlk.h"

double dyn_csr_dot(const double* val, const uint32_t* col, size_t nz,
    const double* dense)
{
    double s = 0.0;
    size_t k;
    for (k = 0; k < nz; k++)
        s += val[k] * dense[col[k]];
    return s;
}

void dyn_csr_axpy(double* dense, double a, const double* val,
    const uint32_t* col, size_t nz)
{
    size_t k;
    for (k = 0; k < nz; k++)
        dense[col[k]] += a * val[k];
}

static int dyn_csr_ent_cmp(const void* a, const void* b)
{
    const dyn_csr_ent_t* x = (const dyn_csr_ent_t*)a;
    const dyn_csr_ent_t* y = (const dyn_csr_ent_t*)b;
    if (x->col != y->col)
        return x->col < y->col ? -1 : 1;
    return x->pos < y->pos ? -1 : x->pos > y->pos;
}

size_t dyn_csr_max_row(const dyn_csr_t* S)
{
    size_t i, m = 0;
    for (i = 0; i < S->rows; i++)
        if (S->ptr[i + 1] - S->ptr[i] > m)
            m = S->ptr[i + 1] - S->ptr[i];
    return m;
}

long dyn_csr_row_reading(const dyn_csr_t* S, size_t i, dyn_csr_ent_t* ent,
    int* bad_nan)
{
    size_t k0 = S->ptr[i], k1 = S->ptr[i + 1], k, m = 0, w, r;
    int descending = 0;

    for (k = k0; k + 1 < k1; k++)
        if (S->col[k] > S->col[k + 1]) {
            descending = 1;
            break;
        }
    for (k = k0; k < k1; k++) {
        ent[m].col = S->col[k];
        ent[m].val = S->val[k];
        ent[m].pos = k - k0;
        m++;
    }
    if (descending)
        qsort(ent, m, sizeof(*ent), dyn_csr_ent_cmp);
    w = 0;
    r = 0;
    while (r < m) {
        uint32_t c = ent[r].col;
        double s = 0.0;
        while (r < m && ent[r].col == c) {
            s += ent[r].val;
            r++;
        }
        if (!isfinite(s)) {
            *bad_nan = isnan(s) != 0;
            return -1;
        }
        if (s != 0.0) {
            ent[w].col = c;
            ent[w].val = s;
            w++;
        }
    }
    return (long)w;
}
