#include "mathx-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_MATHX)

int mx_f64_view(JSContext* ctx, JSValueConst v, const char* what,
    double** pp, size_t* pn)
{
    JSClassID cid;
    JSValue buf;
    uint8_t* base;
    size_t off, len, bpe, ab;
    JS_GetAnyOpaque(v, &cid);
    if (!cid || cid != (JSClassID)mxst_cid_f64) {
        JS_ThrowTypeError(ctx, "solveSPD: %s must be a Float64Array", what);
        return -1;
    }
    buf = JS_GetTypedArrayBuffer(ctx, v, &off, &len, &bpe);
    if (JS_IsException(buf))
        return -1;
    base = JS_GetArrayBuffer(ctx, &ab, buf);
    JS_FreeValue(ctx, buf);
    if (!base) {
        JS_ThrowTypeError(ctx, "typed array buffer is detached");
        return -1;
    }
    if (off > ab || len > ab - off) {
        JS_ThrowRangeError(ctx, "typed array out of bounds");
        return -1;
    }
    *pp = (double*)(base + off);
    *pn = len / 8;
    return 0;
}

int mx_cholesky(const double* A, double* L, size_t n, size_t* bi,
    size_t* bj, double* pivot)
{
    size_t i, j, k;
    for (i = 0; i < n; i++)
        for (j = 0; j <= i; j++) {
            double sum = A[i * n + j];
            if (!isfinite(sum) || sum != A[j * n + i]) {
                *bi = i;
                *bj = j;
                return 1;
            }
            for (k = 0; k < j; k++)
                sum -= L[i * n + k] * L[j * n + k];
            if (i != j) {
                L[i * n + j] = sum / L[j * n + j];
                continue;
            }
            if (!(sum > 0.0)) {
                *bi = i;
                *pivot = sum;
                return 2;
            }
            L[i * n + i] = sqrt(sum);
        }
    return 0;
}

#endif
