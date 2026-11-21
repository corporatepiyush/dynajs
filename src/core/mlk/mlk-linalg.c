#include "mlk.h"

int dyn_solve(double* A, double* b, size_t p, double min_pivot)
{
    size_t col, r, c, pivot;

    for (col = 0; col < p; col++) {
        double maxv = fabs(A[col * p + col]);
        pivot = col;
        for (r = col + 1; r < p; r++) {
            double v = fabs(A[r * p + col]);
            if (v > maxv) {
                maxv = v;
                pivot = r;
            }
        }
        if (!(maxv > 0.0) || !isfinite(maxv) || maxv < min_pivot)
            return -1;
        if (pivot != col) {
            for (c = 0; c < p; c++) {
                double t = A[col * p + c];
                A[col * p + c] = A[pivot * p + c];
                A[pivot * p + c] = t;
            }
            double tb = b[col];
            b[col] = b[pivot];
            b[pivot] = tb;
        }
        for (r = col + 1; r < p; r++) {
            double f = A[r * p + col] / A[col * p + col];
            for (c = col; c < p; c++)
                A[r * p + c] -= f * A[col * p + c];
            b[r] -= f * b[col];
        }
    }
    for (r = p; r-- > 0;) {
        double s = b[r];
        for (c = r + 1; c < p; c++)
            s -= A[r * p + c] * b[c];
        b[r] = s / A[r * p + r];
    }
    return 0;
}

void dyn_jacobi(double* A, double* V, size_t p)
{
    size_t i, j, k, sweep;
    double prev_off = HUGE_VAL;

    for (i = 0; i < p; i++)
        for (j = 0; j < p; j++)
            V[i * p + j] = (i == j) ? 1.0 : 0.0;

    for (sweep = 0; sweep < DYN_JACOBI_SWEEPS; sweep++) {
        double off = 0.0;
        for (i = 0; i < p; i++)
            for (j = i + 1; j < p; j++)
                off += A[i * p + j] * A[i * p + j];
        if (off <= 1e-300 || (sweep >= 2 && off >= prev_off))
            break;
        prev_off = off;
        for (i = 0; i < p; i++) {
            for (j = i + 1; j < p; j++) {
                double aij = A[i * p + j];
                double theta, t, c, s;
                if (fabs(aij) <= 1e-300)
                    continue;
                theta = (A[j * p + j] - A[i * p + i]) / (2.0 * aij);
                t = (theta >= 0.0 ? 1.0 : -1.0) / (fabs(theta) + sqrt(theta * theta + 1.0));
                c = 1.0 / sqrt(t * t + 1.0);
                s = t * c;
                {
                    double aii = A[i * p + i], aij0 = A[i * p + j], aji = A[j * p + i], ajj = A[j * p + j];
                    double bii = c * aii - s * aij0, bij = s * aii + c * aij0;
                    double bji = c * aji - s * ajj, bjj = s * aji + c * ajj;
                    for (k = 0; k < p; k++) {
                        double aik = A[i * p + k], ajk = A[j * p + k];
                        A[i * p + k] = c * aik - s * ajk;
                        A[j * p + k] = s * aik + c * ajk;
                    }
                    A[i * p + i] = c * bii - s * bji;
                    A[j * p + i] = s * bii + c * bji;
                    A[i * p + j] = c * bij - s * bjj;
                    A[j * p + j] = s * bij + c * bjj;
                    for (k = 0; k < i; k++) {
                        A[k * p + i] = A[i * p + k];
                        A[k * p + j] = A[j * p + k];
                    }
                    for (k = i + 1; k < j; k++) {
                        A[k * p + i] = A[i * p + k];
                        A[k * p + j] = A[j * p + k];
                    }
                    for (k = j + 1; k < p; k++) {
                        A[k * p + i] = A[i * p + k];
                        A[k * p + j] = A[j * p + k];
                    }
                }
                for (k = 0; k < p; k++) {
                    double vki = V[i * p + k], vkj = V[j * p + k];
                    V[i * p + k] = c * vki - s * vkj;
                    V[j * p + k] = s * vki + c * vkj;
                }
            }
        }
    }
}
