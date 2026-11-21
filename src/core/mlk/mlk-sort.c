#include "mlk.h"

static void dyn_ml_sort_swap(char* a, char* b, size_t w, char* tmp)
{
    if (w == 8) {
        uint64_t t;
        memcpy(&t, a, 8);
        memcpy(a, b, 8);
        memcpy(b, &t, 8);
    } else if (w == 16) {
        uint64_t t0, t1;
        memcpy(&t0, a, 8);
        memcpy(&t1, a + 8, 8);
        memcpy(a, b, 8);
        memcpy(a + 8, b + 8, 8);
        memcpy(b, &t0, 8);
        memcpy(b + 8, &t1, 8);
    } else {
        memcpy(tmp, a, w);
        memcpy(a, b, w);
        memcpy(b, tmp, w);
    }
}

static void dyn_ml_hsort(char* a, size_t n, size_t w, dyn_ml_cmp_fn cmp)
{
    char tmp[32];
    size_t i;
    for (i = n / 2; i > 0; i--) {
        size_t root = i - 1;
        for (;;) {
            size_t l = 2 * root + 1, c;
            if (l >= n)
                break;
            c = (l + 1 < n && cmp(a + (l + 1) * w, a + l * w) > 0) ? l + 1 : l;
            if (cmp(a + root * w, a + c * w) >= 0)
                break;
            dyn_ml_sort_swap(a + root * w, a + c * w, w, tmp);
            root = c;
        }
    }
    for (i = n; i > 1; i--) {
        size_t root = 0, end = i - 1;
        dyn_ml_sort_swap(a, a + end * w, w, tmp);
        for (;;) {
            size_t l = 2 * root + 1, c;
            if (l >= end)
                break;
            c = (l + 1 < end && cmp(a + (l + 1) * w, a + l * w) > 0) ? l + 1 : l;
            if (cmp(a + root * w, a + c * w) >= 0)
                break;
            dyn_ml_sort_swap(a + root * w, a + c * w, w, tmp);
            root = c;
        }
    }
}

static void dyn_ml_isort_r(char* a, size_t n, size_t w, dyn_ml_cmp_fn cmp,
    size_t depth, size_t limit)
{
    char tmp[32];
    size_t i, j;
    while (n > 16) {
        if (depth >= limit) {
            dyn_ml_hsort(a, n, w, cmp);
            return;
        }
        {
            char *x = a, *y = a + (n / 2) * w, *z = a + (n - 1) * w;
            if (cmp(y, x) < 0)
                dyn_ml_sort_swap(x, y, w, tmp);
            if (cmp(z, y) < 0)
                dyn_ml_sort_swap(y, z, w, tmp);
            if (cmp(y, x) < 0)
                dyn_ml_sort_swap(x, y, w, tmp);
            dyn_ml_sort_swap(a, y, w, tmp);
        }
        {
            size_t lo = 1, hi = n;
            for (;;) {
                while (lo < n && cmp(a + lo * w, a) < 0)
                    lo++;
                do {
                    hi--;
                } while (cmp(a + hi * w, a) > 0);
                if (lo >= hi)
                    break;
                dyn_ml_sort_swap(a + lo * w, a + hi * w, w, tmp);
                lo++;
            }
            dyn_ml_sort_swap(a, a + hi * w, w, tmp);
            if (hi > n - 1 - hi) {
                dyn_ml_isort_r(a + (hi + 1) * w, n - 1 - hi, w, cmp,
                    depth + 1, limit);
                n = hi;
            } else {
                dyn_ml_isort_r(a, hi, w, cmp, depth + 1, limit);
                a += (hi + 1) * w;
                n = n - 1 - hi;
            }
            depth++;
        }
    }
    for (i = 1; i < n; i++) {
        memcpy(tmp, a + i * w, w);
        for (j = i; j > 0 && cmp(tmp, a + (j - 1) * w) < 0; j--)
            memcpy(a + j * w, a + (j - 1) * w, w);
        memcpy(a + j * w, tmp, w);
    }
}

void dyn_ml_isort(void* base, size_t n, size_t w, dyn_ml_cmp_fn cmp)
{
    size_t limit = 0, t;
    if (n < 2)
        return;
    for (t = n; t > 1; t >>= 1)
        limit++;
    dyn_ml_isort_r((char*)base, n, w, cmp, 0, 2 * limit + 1);
}

int dyn_ml_kv_cmp(const void* a, const void* b)
{
    const dyn_tree_kv_t *x = (const dyn_tree_kv_t*)a, *y = (const dyn_tree_kv_t*)b;
    if (x->key != y->key)
        return (x->key < y->key) ? -1 : 1;
    return (x->row > y->row) ? 1 : (x->row < y->row) ? -1
                                                     : 0;
}

static void dyn_ml_kv_hsort(dyn_tree_kv_t* a, size_t n)
{
    size_t i;
    for (i = n / 2; i > 0; i--) {
        size_t root = i - 1;
        for (;;) {
            size_t l = 2 * root + 1, c;
            if (l >= n)
                break;
            c = (l + 1 < n && a[l + 1].key > a[l].key) ? l + 1 : l;
            if (a[root].key >= a[c].key)
                break;
            {
                dyn_tree_kv_t t = a[root];
                a[root] = a[c];
                a[c] = t;
            }
            root = c;
        }
    }
    for (i = n; i > 1; i--) {
        size_t root = 0, end = i - 1;
        {
            dyn_tree_kv_t t = a[0];
            a[0] = a[end];
            a[end] = t;
        }
        for (;;) {
            size_t l = 2 * root + 1, c;
            if (l >= end)
                break;
            c = (l + 1 < end && a[l + 1].key > a[l].key) ? l + 1 : l;
            if (a[root].key >= a[c].key)
                break;
            {
                dyn_tree_kv_t t = a[root];
                a[root] = a[c];
                a[c] = t;
            }
            root = c;
        }
    }
}

void dyn_ml_kv_sort_r(dyn_tree_kv_t* a, size_t n, size_t depth,
    size_t limit)
{
    size_t i, j;
    if (n < 24) {
        for (i = 1; i < n; i++) {
            dyn_tree_kv_t v = a[i];
            for (j = i; j > 0 && a[j - 1].key > v.key; j--)
                a[j] = a[j - 1];
            a[j] = v;
        }
        return;
    }
    if (depth >= limit) {
        dyn_ml_kv_hsort(a, n);
        return;
    }
    {
        double pivot;
        size_t lo = 0, hi = n - 1, mid = n / 2;
        double x = a[0].key, y = a[mid].key, z = a[n - 1].key;
        pivot = (x < y) ? ((y < z) ? y : ((x < z) ? z : x))
                        : ((x < z) ? x : ((y < z) ? z : y));
        while (lo <= hi) {
            while (a[lo].key < pivot)
                lo++;
            while (a[hi].key > pivot) {
                if (hi == 0)
                    break;
                hi--;
            }
            if (lo <= hi) {
                dyn_tree_kv_t t = a[lo];
                a[lo] = a[hi];
                a[hi] = t;
                lo++;
                if (hi == 0)
                    break;
                hi--;
            }
        }
        if (hi + 1 > 1)
            dyn_ml_kv_sort_r(a, hi + 1, depth + 1, limit);
        if (lo < n)
            dyn_ml_kv_sort_r(a + lo, n - lo, depth + 1, limit);
    }
}

static uint64_t dyn_ml_kv_ukey(double d)
{
    uint64_t u;
    memcpy(&u, &d, 8);
    return u ^ ((uint64_t)((int64_t)u >> 63) | 0x8000000000000000ull);
}

int dyn_ml_kv_radix(dyn_tree_kv_t* a, size_t n)
{
    size_t cnt[8][256];
    dyn_tree_kv_t *src = a, *dst, *tmp;
    size_t i, p, b;
    if (n < 2 || n > SIZE_MAX / sizeof(*a))
        return -1;
    tmp = (dyn_tree_kv_t*)malloc(n * sizeof(*a));
    if (!tmp)
        return -1;
    memset(cnt, 0, sizeof(cnt));
    for (i = 0; i < n; i++) {
        uint64_t u = dyn_ml_kv_ukey(a[i].key);
        for (p = 0; p < 8; p++)
            cnt[p][(u >> (8 * p)) & 255]++;
    }
    dst = tmp;
    for (p = 0; p < 8; p++) {
        size_t* c = cnt[p];
        size_t s = 0;
        unsigned sh = (unsigned)(8 * p);
        if (c[(dyn_ml_kv_ukey(a[0].key) >> sh) & 255] == n)
            continue;
        for (b = 0; b < 256; b++) {
            size_t t = c[b];
            c[b] = s;
            s += t;
        }
        for (i = 0; i < n; i++)
            dst[c[(dyn_ml_kv_ukey(src[i].key) >> sh) & 255]++] = src[i];
        {
            dyn_tree_kv_t* t = src;
            src = dst;
            dst = t;
        }
    }
    if (src != a)
        memcpy(a, src, n * sizeof(*a));
    free(tmp);
    return 0;
}
