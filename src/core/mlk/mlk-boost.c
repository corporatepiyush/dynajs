#include "mlk.h"

void dyn_boost_leaf_newton(dyn_tree_t* t, const double* X, size_t cols,
    const double* grad, const double* hess,
    const double* w, const uint32_t* rows_in,
    size_t n, double scale, double* num,
    double* den)
{
    size_t i;
    if (t->n_nodes == 0)
        return;
    memset(num, 0, t->n_nodes * sizeof(double));
    memset(den, 0, t->n_nodes * sizeof(double));
    for (i = 0; i < n; i++) {
        uint32_t row = rows_in[i];
        uint32_t leaf = dyn_tree_leaf_index(t, X + (size_t)row * cols, cols);
        if (w) {
            num[leaf] += w[row] * grad[row];
            den[leaf] += w[row] * hess[row];
        } else {
            num[leaf] += grad[row];
            den[leaf] += hess[row];
        }
    }
    for (i = 0; i < t->n_nodes; i++) {
        if (t->node[i].feature >= 0)
            continue;
        t->node[i].value = (den[i] > 1e-12) ? scale * num[i] / den[i] : 0.0;
    }
}

void dyn_boost_init_base(int classifier, size_t n_out, const double* y,
    size_t rows, const double* classes,
    const double* w, double* raw_base, double* pred)
{
    size_t i, k;
    double wtot = 0.0;
    for (i = 0; i < rows; i++)
        wtot += w ? w[i] : 1.0;
    if (wtot <= 0.0)
        wtot = 1.0;

    if (!classifier) {
        double s = 0.0;
        for (i = 0; i < rows; i++)
            s += w ? w[i] * y[i] : y[i];
        raw_base[0] = s / wtot;
        for (i = 0; i < rows; i++)
            pred[i] = raw_base[0];
    } else if (n_out == 1) {
        double pos = 0.0, p;
        for (i = 0; i < rows; i++)
            if (y[i] == classes[1])
                pos += w ? w[i] : 1.0;
        p = pos / wtot;
        if (p < 1e-9)
            p = 1e-9;
        if (p > 1.0 - 1e-9)
            p = 1.0 - 1e-9;
        raw_base[0] = log(p / (1.0 - p));
        for (i = 0; i < rows; i++)
            pred[i] = raw_base[0];
    } else {
        for (k = 0; k < n_out; k++) {
            double cnt = 0.0, p;
            for (i = 0; i < rows; i++)
                if (y[i] == classes[k])
                    cnt += w ? w[i] : 1.0;
            p = cnt / wtot;
            if (p < 1e-9)
                p = 1e-9;
            raw_base[k] = log(p);
        }
        for (i = 0; i < rows; i++)
            for (k = 0; k < n_out; k++)
                pred[i * n_out + k] = raw_base[k];
    }
}

void dyn_xgb_grad_hess(int classifier, size_t n_out, size_t k,
    const double* pred, const double* y,
    const double* classes, const double* w,
    size_t rows, double* g, double* h)
{
    size_t i, j;

    if (!classifier) {
        for (i = 0; i < rows; i++) {
            g[i] = pred[i] - y[i];
            h[i] = 1.0;
        }
    } else if (n_out == 1) {
        for (i = 0; i < rows; i++) {
            double p = 1.0 / (1.0 + exp(-pred[i]));
            double t = (y[i] == classes[1]) ? 1.0 : 0.0;
            g[i] = p - t;
            h[i] = p * (1.0 - p);
            if (h[i] < 1e-16)
                h[i] = 1e-16;
        }
    } else {
        for (i = 0; i < rows; i++) {
            const double* f = pred + i * n_out;
            double mx = f[0], s = 0.0, ek = 0.0, p;
            for (j = 1; j < n_out; j++)
                if (f[j] > mx)
                    mx = f[j];
            for (j = 0; j < n_out; j++) {
                double e = exp(f[j] - mx);
                if (j == k)
                    ek = e;
                s += e;
            }
            p = ek / s;
            g[i] = p - ((y[i] == classes[k]) ? 1.0 : 0.0);
            h[i] = 2.0 * p * (1.0 - p);
            if (h[i] < 1e-16)
                h[i] = 1e-16;
        }
    }
    if (w) {
        for (i = 0; i < rows; i++) {
            g[i] *= w[i];
            h[i] *= w[i];
        }
    }
}

double dyn_xgb_val_loss(int classifier, size_t n_out, const double* pred,
    const double* y, const double* classes,
    const uint8_t* isval, size_t rows)
{
    size_t i, k, nv = 0;
    double acc = 0.0;

    for (i = 0; i < rows; i++) {
        if (!isval[i])
            continue;
        nv++;
        if (!classifier) {
            double d = pred[i] - y[i];
            acc += d * d;
        } else if (n_out == 1) {
            double p = 1.0 / (1.0 + exp(-pred[i]));
            double t = (y[i] == classes[1]) ? 1.0 : 0.0;
            if (p < 1e-15)
                p = 1e-15;
            if (p > 1.0 - 1e-15)
                p = 1.0 - 1e-15;
            acc += t ? -log(p) : -log(1.0 - p);
        } else {
            const double* f = pred + i * n_out;
            double mx = f[0], s = 0.0, own = 0.0;
            for (k = 1; k < n_out; k++)
                if (f[k] > mx)
                    mx = f[k];
            for (k = 0; k < n_out; k++)
                s += exp(f[k] - mx);
            for (k = 0; k < n_out; k++)
                if (y[i] == classes[k])
                    own = f[k] - mx;
            acc += -(own - log(s));
        }
    }
    return nv ? acc / (double)nv : 0.0;
}
