/* Operation template for bench_ds_parr.c, included once per variant with VAR (the prefix of the
 * variant's primitives) and VT (its state type) defined. Every primitive is static inline, so each
 * variant's loops are compiled with its own accessors inlined: no indirect calls in a timed loop. */
#define CAT2(a, b) a##_##b
#define CAT(a, b) CAT2(a, b)
#define F(name) CAT(VAR, name)

static void F(build)(VT* st, uint32_t N, uint32_t P, uint64_t* H)
{
    F(init)(st, P);
    for (uint32_t i = 0; i < N; i++) {
        uint32_t s = F(alloc)(st);
        rec_t* r = F(ref)(st, s);
        r->key = i;
        r->val = i;
        if (H)
            H[i] = F(handle)(st, s);
    }
}

static double F(op_append)(uint32_t N, uint32_t P, uint32_t K)
{
    VT* st = xcalloc(K, sizeof(VT));
    for (uint32_t k = 0; k < K; k++)
        F(init)(&st[k], P);
    uint64_t t0 = cpu_ns();
    for (uint32_t k = 0; k < K; k++)
        for (uint32_t i = 0; i < N; i++) {
            uint32_t s = F(alloc)(&st[k]);
            rec_t* r = F(ref)(&st[k], s);
            r->key = i;
            r->val = i ^ k;
        }
    uint64_t t1 = cpu_ns();
    for (uint32_t k = 0; k < K; k++) {
        sink += F(ref)(&st[k], N - 1)->val;
        F(destroy)(&st[k]);
    }
    free(st);
    return (double)(t1 - t0) / ((double)K * N);
}

static double F(op_fill)(uint32_t N, uint32_t P, uint32_t K)
{
    VT* st = xcalloc(K, sizeof(VT));
    uint32_t half = N / 2;
    for (uint32_t k = 0; k < K; k++) {
        F(build)(&st[k], N, P, g_h);
        for (uint32_t j = 0; j < half; j++)
            F(free)(&st[k], g_h[g_perm[j]]);
    }
    uint64_t t0 = cpu_ns();
    for (uint32_t k = 0; k < K; k++)
        for (uint32_t j = 0; j < half; j++) {
            uint32_t s = F(alloc)(&st[k]);
            rec_t* r = F(ref)(&st[k], s);
            r->key = j;
            r->val = j;
        }
    uint64_t t1 = cpu_ns();
    for (uint32_t k = 0; k < K; k++)
        F(destroy)(&st[k]);
    free(st);
    return (double)(t1 - t0) / ((double)K * half);
}

static double F(op_randidx)(uint32_t N, uint32_t P)
{
    VT st;
    uint64_t sum = 0;
    F(build)(&st, N, P, NULL);
    uint64_t t0 = cpu_ns();
    for (uint32_t j = 0; j < g_m; j++)
        sum += F(ref)(&st, g_idx[j])->val;
    uint64_t t1 = cpu_ns();
    sink += sum;
    F(destroy)(&st);
    return (double)(t1 - t0) / g_m;
}

static double F(op_chase)(uint32_t N, uint32_t P)
{
    VT st;
    uint32_t x = 0;
    F(build)(&st, N, P, NULL);
    for (uint32_t i = 0; i < N; i++)
        F(ref)(&st, i)->val = g_chain[i];
    uint64_t t0 = cpu_ns();
    for (uint32_t j = 0; j < g_m; j++)
        x = (uint32_t)F(ref)(&st, x)->val;
    uint64_t t1 = cpu_ns();
    sink += x;
    F(destroy)(&st);
    return (double)(t1 - t0) / g_m;
}

static double F(op_randh)(uint32_t N, uint32_t P)
{
    VT st;
    uint64_t sum = 0;
    F(build)(&st, N, P, g_h);
    uint64_t t0 = cpu_ns();
    for (uint32_t j = 0; j < g_m; j++) {
        rec_t* r = F(resolve)(&st, g_h[g_idx[j]]);
        if (r)
            sum += r->val;
    }
    uint64_t t1 = cpu_ns();
    sink += sum;
    F(destroy)(&st);
    return (double)(t1 - t0) / g_m;
}

static double F(op_scan)(uint32_t N, uint32_t P, uint32_t pct)
{
    VT st;
    uint64_t sum = 0;
    uint32_t del = (uint32_t)((uint64_t)N * pct / 100), R = N >= (1u << 21) ? 1 : (1u << 21) / N;
    F(build)(&st, N, P, g_h);
    for (uint32_t j = 0; j < del; j++)
        F(free)(&st, g_h[g_perm[j]]);
    uint64_t t0 = cpu_ns();
    for (uint32_t r = 0; r < R; r++)
        sum += F(scan)(&st);
    uint64_t t1 = cpu_ns();
    sink += sum;
    F(destroy)(&st);
    return (double)(t1 - t0) / ((double)R * (N - del));
}

static double F(op_delete)(uint32_t N, uint32_t P, uint32_t K)
{
    VT* st = xcalloc(K, sizeof(VT));
    uint64_t* H = xcalloc((size_t)K * N, sizeof(uint64_t));
    for (uint32_t k = 0; k < K; k++)
        F(build)(&st[k], N, P, H + (size_t)k * N);
    uint64_t t0 = cpu_ns();
    for (uint32_t k = 0; k < K; k++)
        for (uint32_t j = 0; j < N; j++)
            F(free)(&st[k], H[(size_t)k * N + g_perm[j]]);
    uint64_t t1 = cpu_ns();
    for (uint32_t k = 0; k < K; k++)
        F(destroy)(&st[k]);
    free(H);
    free(st);
    return (double)(t1 - t0) / ((double)K * N);
}

static double F(op_churn)(uint32_t N, uint32_t P)
{
    VT st;
    F(build)(&st, N, P, g_h);
    uint64_t t0 = cpu_ns();
    for (uint32_t m = 0; m < g_m; m++) {
        uint32_t j = g_idx[m];
        F(free)(&st, g_h[j]);
        uint32_t s = F(alloc)(&st);
        rec_t* r = F(ref)(&st, s);
        r->key = m;
        r->val = j;
        g_h[j] = F(handle)(&st, s);
    }
    uint64_t t1 = cpu_ns();
    F(destroy)(&st);
    return (double)(t1 - t0) / g_m;
}

static void F(op_latency)(uint32_t N, uint32_t P, uint64_t* lat)
{
    VT st;
    F(init)(&st, P);
    for (uint32_t i = 0; i < N; i++) {
        uint64_t t0 = wall_ns();
        uint32_t s = F(alloc)(&st);
        rec_t* r = F(ref)(&st, s);
        r->key = i;
        r->val = i;
        lat[i] = wall_ns() - t0;
    }
    F(destroy)(&st);
}

static size_t F(op_bytes)(uint32_t N, uint32_t P, size_t* after_half)
{
    VT st;
    size_t b;
    F(build)(&st, N, P, g_h);
    b = F(bytes)(&st);
    for (uint32_t j = 0; j < N / 2; j++)
        F(free)(&st, g_h[g_perm[j]]);
    F(shrink)(&st);
    *after_half = F(bytes)(&st);
    F(destroy)(&st);
    return b;
}

static void F(op_rss)(uint32_t N, uint32_t P, int64_t out[3])
{
    VT st;
    int64_t r0 = rss_now();
    F(build)(&st, N, P, g_h);
    out[0] = rss_now() - r0;
    for (uint32_t j = 0; j < N / 2; j++)
        F(free)(&st, g_h[g_perm[j]]);
    F(shrink)(&st);
    out[1] = rss_now() - r0;
    out[2] = rss_peak() - r0;
    F(destroy)(&st);
}

#undef F
#undef CAT
#undef CAT2
