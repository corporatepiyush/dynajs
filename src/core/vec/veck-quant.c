#include <math.h>
#include "veck-int.h"

static double vk_sq8_enc(const dyn_vec_t* v, int8_t* dst, const float* x)
{
    uint32_t j;
    double corr = 0;
    for (j = 0; j < v->dim; j++) {
        double t = v->scale > 0 ? ((double)x[j] - v->mid[j]) / v->scale : 0;
        int c = t >= 127 ? 127 : t <= -127 ? -127 : (int)floor(t + 0.5);
        dst[j] = (int8_t)c;
        corr += v->mid[j] * (double)c;
    }
    return corr;
}

static void vk_bin_enc(const dyn_vec_t* v, uint64_t* dst, const float* x)
{
    uint32_t j;
    memset(dst, 0, (size_t)v->words * sizeof(uint64_t));
    for (j = 0; j < v->dim; j++)
        if (x[j] > 0)
            dst[j >> 6] |= (uint64_t)1 << (j & 63);
}

int vk_encode(dyn_vec_t* v, uint32_t slot, const float* x)
{
    if (v->quant == DYN_VEC_SQ8) {
        double c = vk_sq8_enc(v, (int8_t*)v->codes + (size_t)slot * v->cb, x);
        if (v->corr)
            v->corr[slot] = c;
    } else if (v->quant == DYN_VEC_BINARY) {
        vk_bin_enc(v, (uint64_t*)(void*)(v->codes + (size_t)slot * v->cb), x);
    }
    return DYN_VEC_OK;
}

void vk_query_slot(dyn_vec_t* v, uint32_t slot)
{
    if (v->keep) {
        v->qmode = 0;
        v->qf = vk_vec(v, slot);
        return;
    }
    v->qmode = 1;
    v->qc = vk_sq(v, slot);
    v->qb = vk_bits(v, slot);
    v->qcorr = v->corr ? v->corr[slot] : 0;
}

int vk_query_vec(dyn_vec_t* v, const float* q)
{
    if (v->quant == DYN_VEC_F32) {
        v->qmode = 0;
        v->qf = q;
        return DYN_VEC_OK;
    }
    v->qmode = 1;
    if (v->quant == DYN_VEC_SQ8) {
        v->qcorr = vk_sq8_enc(v, v->qcbuf, q);
        v->qcorr = v->corr ? v->qcorr : 0;
        v->qc = v->qcbuf;
    } else {
        vk_bin_enc(v, (uint64_t*)(void*)v->qcbuf, q);
        v->qb = (const uint64_t*)(const void*)v->qcbuf;
    }
    return DYN_VEC_OK;
}

int vk_quant_alloc(dyn_vec_t* v, uint32_t nc)
{
    void* p;
    if (v->quant == DYN_VEC_F32)
        return DYN_VEC_OK;
    p = realloc(v->codes, (size_t)nc * v->cb);
    if (!p)
        return DYN_VEC_ENOMEM;
    v->codes = (uint8_t*)p;
    if (v->quant == DYN_VEC_SQ8 && v->metric != DYN_VEC_L2) {
        p = realloc(v->corr, (size_t)nc * sizeof(double));
        if (!p)
            return DYN_VEC_ENOMEM;
        v->corr = (double*)p;
    }
    return DYN_VEC_OK;
}

void vk_quant_free(dyn_vec_t* v)
{
    free(v->codes);
    free(v->corr);
    free(v->lo);
    free(v->hi);
    free(v->mid);
    free(v->qcbuf);
}

int vk_sq8_refresh(dyn_vec_t* v)
{
    uint32_t j, i;
    double half = 0;
    v->msq = 0;
    for (j = 0; j < v->dim; j++) {
        double h = (v->hi[j] - v->lo[j]) * 0.5;
        v->mid[j] = v->lo[j] + h;
        v->msq += v->mid[j] * v->mid[j];
        if (h > half)
            half = h;
    }
    v->scale = half / 127.0;
    if (v->corr)
        for (i = 0; i < v->n; i++) {
            const int8_t* c = vk_sq(v, i);
            double s = 0;
            for (j = 0; j < v->dim; j++)
                s += v->mid[j] * (double)c[j];
            v->corr[i] = s;
        }
    return DYN_VEC_OK;
}

int vk_sq8_widen(dyn_vec_t* v, const double* blo, const double* bhi)
{
    uint32_t j, i, d = v->dim;
    int grew = 0;
    double *omid, oscale;
    float* tmp;
    if (!v->trained) {
        memcpy(v->lo, blo, (size_t)d * sizeof(double));
        memcpy(v->hi, bhi, (size_t)d * sizeof(double));
        v->trained = 1;
        return vk_sq8_refresh(v);
    }
    omid = (double*)malloc((size_t)d * sizeof(double));
    tmp = (float*)malloc((size_t)d * sizeof(float));
    if (!omid || !tmp) {
        free(omid);
        free(tmp);
        return DYN_VEC_ENOMEM;
    }
    memcpy(omid, v->mid, (size_t)d * sizeof(double));
    oscale = v->scale;
    for (j = 0; j < d; j++) {
        double nlo = blo[j] < v->lo[j] ? blo[j] : v->lo[j];
        double nhi = bhi[j] > v->hi[j] ? bhi[j] : v->hi[j];
        double w = nhi - nlo;
        if (blo[j] < v->lo[j]) {
            nlo -= w * 0.25;
            grew = 1;
        }
        if (bhi[j] > v->hi[j]) {
            nhi += w * 0.25;
            grew = 1;
        }
        v->lo[j] = nlo;
        v->hi[j] = nhi;
    }
    if (grew) {
        double* cs = v->corr;
        v->corr = NULL;
        vk_sq8_refresh(v);
        v->corr = cs;
        for (i = 0; i < v->n; i++) {
            int8_t* c = (int8_t*)v->codes + (size_t)i * v->cb;
            const float* x;
            if (v->keep) {
                x = vk_vec(v, i);
            } else {
                for (j = 0; j < d; j++)
                    tmp[j] = (float)(omid[j] + oscale * (double)c[j]);
                x = tmp;
            }
            vk_encode(v, i, x);
        }
    }
    free(omid);
    free(tmp);
    return DYN_VEC_OK;
}

int vk_sq8_cover(dyn_vec_t* v, const float* x, uint32_t nb)
{
    uint32_t i, j, d = v->dim;
    int out = !v->trained;
    double *blo, *bhi;
    int rc;
    for (i = 0; i < nb && !out; i++)
        for (j = 0; j < d; j++)
            if ((double)x[(size_t)i * d + j] < v->lo[j] || (double)x[(size_t)i * d + j] > v->hi[j]) {
                out = 1;
                break;
            }
    if (!out)
        return DYN_VEC_OK;
    blo = (double*)malloc((size_t)d * sizeof(double));
    bhi = (double*)malloc((size_t)d * sizeof(double));
    if (!blo || !bhi) {
        free(blo);
        free(bhi);
        return DYN_VEC_ENOMEM;
    }
    for (j = 0; j < d; j++) {
        blo[j] = HUGE_VAL;
        bhi[j] = -HUGE_VAL;
    }
    for (i = 0; i < nb; i++)
        for (j = 0; j < d; j++) {
            double t = (double)x[(size_t)i * d + j];
            if (t < blo[j])
                blo[j] = t;
            if (t > bhi[j])
                bhi[j] = t;
        }
    rc = vk_sq8_widen(v, blo, bhi);
    free(blo);
    free(bhi);
    return rc;
}
