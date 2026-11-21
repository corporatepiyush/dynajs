#include "ds-internal.h"
#include "ds-heap.h"
#include "ds-suffix-array.h"

#define DS_SA_EMPTY UINT32_MAX

struct dyn_sa {
    uint8_t* text;
    uint32_t n;
    uint32_t* sa;
    uint32_t* lcp;
    uint32_t* bkt;
    uint32_t bk;
};

static int ds_sa_type_s(const uint8_t* t, uint32_t i)
{
    return (t[i >> 3] >> (i & 7)) & 1;
}

static void ds_sa_set_type(uint8_t* t, uint32_t i, int s)
{
    if (s)
        t[i >> 3] |= (uint8_t)(1u << (i & 7));
    else
        t[i >> 3] &= (uint8_t)~(1u << (i & 7));
}

static int ds_sa_is_lms(const uint8_t* t, uint32_t i)
{
    return i > 0 && ds_sa_type_s(t, i) && !ds_sa_type_s(t, i - 1);
}

static void ds_sa_buckets(const uint32_t* s, uint32_t n, uint32_t k, uint32_t* bkt, int end)
{
    uint32_t i, sum = 0;
    memset(bkt, 0, sizeof(uint32_t) * k);
    for (i = 0; i < n; i++)
        bkt[s[i]]++;
    for (i = 0; i < k; i++) {
        sum += bkt[i];
        bkt[i] = end ? sum : sum - bkt[i];
    }
}

static void ds_sa_induce(const uint32_t* s, uint32_t* sa, const uint8_t* t, uint32_t n, uint32_t k, uint32_t* bkt)
{
    uint32_t i;
    ds_sa_buckets(s, n, k, bkt, 0);
    for (i = 0; i < n; i++) {
        uint32_t j = sa[i];
        if (j != DS_SA_EMPTY && j > 0 && !ds_sa_type_s(t, j - 1))
            sa[bkt[s[j - 1]]++] = j - 1;
    }
    ds_sa_buckets(s, n, k, bkt, 1);
    for (i = n; i-- > 0;) {
        uint32_t j = sa[i];
        if (j != DS_SA_EMPTY && j > 0 && ds_sa_type_s(t, j - 1))
            sa[--bkt[s[j - 1]]] = j - 1;
    }
}

static int ds_sa_is(const uint32_t* s, uint32_t* sa, uint32_t n, uint32_t k)
{
    uint8_t* t = (uint8_t*)DYN_DS_CALLOC((n >> 3) + 1, 1);
    uint32_t* bkt = (uint32_t*)DYN_DS_MALLOC(sizeof(uint32_t) * (k ? k : 1));
    uint32_t i, j, n1 = 0, name = 0, prev = DS_SA_EMPTY;
    uint32_t *s1 = NULL, *sa1 = NULL, *names = NULL;
    int rc = -1;
    if (!t || !bkt)
        goto out;
    if (n == 1) {
        sa[0] = 0;
        rc = 0;
        goto out;
    }
    ds_sa_set_type(t, n - 1, 1);
    for (i = n - 1; i-- > 0;)
        ds_sa_set_type(t, i, s[i] < s[i + 1] || (s[i] == s[i + 1] && ds_sa_type_s(t, i + 1)));
    ds_sa_buckets(s, n, k, bkt, 1);
    for (i = 0; i < n; i++)
        sa[i] = DS_SA_EMPTY;
    for (i = 1; i < n; i++)
        if (ds_sa_is_lms(t, i))
            sa[--bkt[s[i]]] = i;
    ds_sa_induce(s, sa, t, n, k, bkt);
    for (i = 0; i < n; i++)
        if (sa[i] != DS_SA_EMPTY && ds_sa_is_lms(t, sa[i]))
            sa[n1++] = sa[i];
    names = (uint32_t*)DYN_DS_MALLOC(sizeof(uint32_t) * n);
    if (!names)
        goto out;
    for (i = 0; i < n; i++)
        names[i] = DS_SA_EMPTY;
    for (i = 0; i < n1; i++) {
        uint32_t pos = sa[i];
        int diff = 0;
        if (prev == DS_SA_EMPTY) {
            diff = 1;
        } else {
            uint32_t d;
            for (d = 0;; d++) {
                if (pos + d >= n || prev + d >= n || s[pos + d] != s[prev + d] || ds_sa_type_s(t, pos + d) != ds_sa_type_s(t, prev + d)) {
                    diff = 1;
                    break;
                }
                if (d > 0 && ds_sa_is_lms(t, pos + d))
                    break;
            }
        }
        if (diff) {
            name++;
            prev = pos;
        }
        names[pos] = name - 1;
    }
    s1 = (uint32_t*)DYN_DS_MALLOC(sizeof(uint32_t) * (n1 + 1));
    sa1 = (uint32_t*)DYN_DS_MALLOC(sizeof(uint32_t) * (n1 ? n1 : 1));
    if (!s1 || !sa1)
        goto out;
    for (i = 0, j = 0; i < n; i++) {
        s1[j] = names[i];
        j += names[i] != DS_SA_EMPTY;
    }
    if (name < n1) {
        if (ds_sa_is(s1, sa1, n1, name) < 0)
            goto out;
    } else {
        for (i = 0; i < n1; i++)
            sa1[s1[i]] = i;
    }
    for (i = 1, j = 0; i < n; i++) {
        s1[j] = i;
        j += ds_sa_is_lms(t, i);
    }
    for (i = 0; i < n1; i++)
        sa1[i] = s1[sa1[i]];
    for (i = 0; i < n; i++)
        sa[i] = DS_SA_EMPTY;
    ds_sa_buckets(s, n, k, bkt, 1);
    for (i = n1; i-- > 0;) {
        j = sa1[i];
        sa[--bkt[s[j]]] = j;
    }
    ds_sa_induce(s, sa, t, n, k, bkt);
    rc = 0;
out:
    DYN_DS_FREE(t);
    DYN_DS_FREE(bkt);
    DYN_DS_FREE(names);
    DYN_DS_FREE(s1);
    DYN_DS_FREE(sa1);
    return rc;
}

dyn_sa_t* dyn_sa_build(const uint8_t* text, size_t n)
{
    dyn_sa_t* r;
    uint32_t *s, *tmp, i;
    if (n >= UINT32_MAX - 1)
        return NULL;
    r = (dyn_sa_t*)DYN_DS_CALLOC(1, sizeof(*r));
    if (!r)
        return NULL;
    r->n = (uint32_t)n;
    r->text = (uint8_t*)DYN_DS_MALLOC(n ? n : 1);
    r->sa = (uint32_t*)DYN_DS_MALLOC(sizeof(uint32_t) * (n ? n : 1));
    s = (uint32_t*)DYN_DS_MALLOC(sizeof(uint32_t) * (n + 1));
    tmp = (uint32_t*)DYN_DS_MALLOC(sizeof(uint32_t) * (n + 1));
    if (!r->text || !r->sa || !s || !tmp)
        goto fail;
    if (n)
        memcpy(r->text, text, n);
    for (i = 0; i < n; i++)
        s[i] = (uint32_t)text[i] + 1;
    s[n] = 0;
    if (ds_sa_is(s, tmp, (uint32_t)n + 1, 257) < 0)
        goto fail;
    if (n)
        memcpy(r->sa, tmp + 1, sizeof(uint32_t) * n);
    DYN_DS_FREE(s);
    DYN_DS_FREE(tmp);
    return r;
fail:
    DYN_DS_FREE(s);
    DYN_DS_FREE(tmp);
    dyn_sa_free(r);
    return NULL;
}

void dyn_sa_free(dyn_sa_t* sa)
{
    if (!sa)
        return;
    DYN_DS_FREE(sa->text);
    DYN_DS_FREE(sa->sa);
    DYN_DS_FREE(sa->lcp);
    DYN_DS_FREE(sa->bkt);
    DYN_DS_FREE(sa);
}

size_t dyn_sa_len(const dyn_sa_t* sa)
{
    return sa->n;
}

const uint32_t* dyn_sa_array(const dyn_sa_t* sa)
{
    return sa->sa;
}

const uint8_t* dyn_sa_text(const dyn_sa_t* sa)
{
    return sa->text;
}

const uint32_t* dyn_sa_lcp(dyn_sa_t* sa)
{
    uint32_t *rank, i, h = 0;
    if (sa->lcp)
        return sa->lcp;
    sa->lcp = (uint32_t*)DYN_DS_MALLOC(sizeof(uint32_t) * (sa->n ? sa->n : 1));
    rank = (uint32_t*)DYN_DS_MALLOC(sizeof(uint32_t) * (sa->n ? sa->n : 1));
    if (!sa->lcp || !rank) {
        DYN_DS_FREE(sa->lcp);
        DYN_DS_FREE(rank);
        sa->lcp = NULL;
        return NULL;
    }
    for (i = 0; i < sa->n; i++)
        rank[sa->sa[i]] = i;
    for (i = 0; i < sa->n; i++) {
        if (rank[i] == 0) {
            sa->lcp[0] = 0;
            h = 0;
            continue;
        }
        {
            uint32_t j = sa->sa[rank[i] - 1];
            while (i + h < sa->n && j + h < sa->n && sa->text[i + h] == sa->text[j + h])
                h++;
            sa->lcp[rank[i]] = h;
            if (h)
                h--;
        }
    }
    DYN_DS_FREE(rank);
    return sa->lcp;
}

static int ds_sa_cmp_prefix(const dyn_sa_t* sa, uint32_t pos, const uint8_t* pat, size_t m)
{
    size_t avail = sa->n - pos, len = avail < m ? avail : m;
    int c = len ? memcmp(sa->text + pos, pat, len) : 0;
    if (c)
        return c;
    return avail < m ? -1 : 0;
}

int dyn_sa_bucket(dyn_sa_t* sa)
{
    uint32_t k = sa->n >= (1u << 18) ? 2 : sa->n >= 1024 ? 1 : 0, nb, key, j = 0;
    uint32_t* b;
    if (!k || sa->bkt)
        return 0;
    nb = k == 2 ? 65536u : 256u;
    b = (uint32_t*)DYN_DS_MALLOC(sizeof(uint32_t) * ((size_t)nb + 1));
    if (!b)
        return -1;
    for (key = 0; key < nb; key++) {
        while (j < sa->n) {
            uint32_t p = sa->sa[j];
            uint32_t v = k == 1 ? sa->text[p] : (uint32_t)sa->text[p] << 8 | (p + 1 < sa->n ? sa->text[p + 1] : 0);
            if (v >= key)
                break;
            j++;
        }
        b[key] = j;
    }
    b[nb] = sa->n;
    sa->bkt = b;
    sa->bk = k;
    return 0;
}

size_t dyn_sa_find(const dyn_sa_t* sa, const uint8_t* pat, size_t m, size_t* first)
{
    size_t lo = 0, hi = sa->n, start, end = sa->n;
    if (sa->bkt && m >= sa->bk) {
        uint32_t key = sa->bk == 1 ? pat[0] : (uint32_t)pat[0] << 8 | pat[1];
        lo = sa->bkt[key];
        hi = end = sa->bkt[key + 1];
    }
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (ds_sa_cmp_prefix(sa, sa->sa[mid], pat, m) < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    start = lo;
    hi = end;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (ds_sa_cmp_prefix(sa, sa->sa[mid], pat, m) <= 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (first)
        *first = start;
    return lo - start;
}

size_t dyn_sa_longest_repeat(dyn_sa_t* sa, size_t* pos)
{
    const uint32_t* lcp = dyn_sa_lcp(sa);
    uint32_t i, best = 0, at = 0;
    if (!lcp)
        return 0;
    for (i = 1; i < sa->n; i++) {
        if (lcp[i] > best) {
            best = lcp[i];
            at = sa->sa[i];
        }
    }
    if (pos)
        *pos = at;
    return best;
}
