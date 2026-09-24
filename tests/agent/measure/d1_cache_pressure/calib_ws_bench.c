/* calib_ws_bench.c -- host cache-geometry calibration for the D1 study.
 *
 * NOT engine code: this is a reference load used to locate the host's
 * L1D -> L2 -> DRAM working-set crossovers with the same measurement style
 * the engine kernels use (min ns/op over repeated passes under a scaled
 * working set). Compare the engine's degradation onsets against these
 * crossovers to attribute engine ns/op growth to data-side stalls.
 *
 * Load: streaming read + FMA over a Float64-like buffer (the same shape as
 * the engine's f64read kernel), plus a pointer-chase latency probe at each
 * size (dependent loads expose raw cache/DRAM latency).
 *
 * Build: cc -O2 -o calib_ws_bench calib_ws_bench.c
 * Run:   ./calib_ws_bench [size_kib ...]   (default full ladder)
 * Output: RESULT <stream|chase> <bytes> <ns_per_op>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

static double g_sink = 0; /* checksum sink, survives the ladder loop */
static double now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e9 + ts.tv_nsec;
}

static int cmp_d(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

static double median3(double a, double b, double c) {
    double v[3] = { a, b, c };
    qsort(v, 3, sizeof(double), cmp_d);
    return v[1];
}

int main(int argc, char **argv) {
    size_t ladder_kib[] = { 4, 16, 64, 256, 1024, 4096, 16384, 65536, 131072 };
    const int n_ladder = (argc > 1) ? argc - 1 : (int)(sizeof(ladder_kib) / sizeof(ladder_kib[0]));

    for (int li = 0; li < n_ladder; li++) {
        size_t kib = (argc > 1) ? (size_t)strtoul(argv[li + 1], NULL, 10) : ladder_kib[li];
        size_t bytes = kib * 1024;
        size_t n = bytes / sizeof(double);
        if (n < 16) n = 16;
        bytes = n * sizeof(double);

        double *buf = malloc(bytes);
        if (!buf) { perror("malloc"); return 1; }
        for (size_t i = 0; i < n; i++) buf[i] = (double)(i & 1023) * 0.5;

        /* streaming read+FMA: same shape as engine f64read kernel.
         * Rep count tuned so one rep ~= 0.5ms at DRAM bandwidth. */
        size_t inner = n;
        double acc = 0;
        double reps_d = 2.0e8 / (double)bytes;   /* ~200MB touched per rep-set */
        size_t reps = (size_t)(reps_d < 1 ? 1 : (reps_d > 5000 ? 5000 : reps_d));

        /* warmup */
        for (size_t i = 0; i < inner; i++) acc += buf[i] * 1.5;

        double best = 1e30, b2 = 1e30, b3 = 1e30;
        for (int r = 0; r < 3; r++) {
            double t0 = now_ns();
            for (size_t rep = 0; rep < reps; rep++) {
                double s = 0;
                for (size_t i = 0; i < inner; i++) s += buf[i] * 1.5;
                acc += s;
            }
            double dt = now_ns() - t0;
            double per = dt / (double)(reps * inner);
            if (per < best) { b3 = b2; b2 = best; best = per; }
            else if (per < b2) { b3 = b2; b2 = per; }
            else if (per < b3) { b3 = per; }
        }
        g_sink += acc;
        printf("RESULT stream %zu %.4f (acc=%.1f)\n", bytes, median3(best, b2, b3), acc);

        /* pointer-chase: dependent loads -> exposed latency at this ws */
        uint64_t *pbuf = malloc(n < 1024 ? 1024 * sizeof(uint64_t) : bytes);
        size_t pn = (n < 1024 ? 1024 : n);
        for (size_t i = 0; i < pn; i++) pbuf[i] = i;
        /* cyclic permutation so the chase visits every slot */
        for (size_t i = 1; i < pn; i++) {
            size_t j = ((i * 1103515245u + 12345u) % (pn - 1));
            uint64_t t = pbuf[i]; pbuf[i] = pbuf[j]; pbuf[j] = t;
        }
        size_t steps = pn * 4;
        if (steps < 1 << 20) steps = 1 << 20;
        double cbest = 1e30, c2 = 1e30, c3 = 1e30;
        for (int r = 0; r < 3; r++) {
            size_t idx = 0;
            double t0 = now_ns();
            for (size_t i = 0; i < steps; i++) idx = pbuf[idx];
            double dt = now_ns() - t0;
            double per = dt / (double)steps;
            if (per < cbest) { c3 = c2; c2 = cbest; cbest = per; }
            else if (per < c2) { c3 = c2; c2 = per; }
            else if (per < c3) { c3 = per; }
            if (idx == 42) printf(" ");
        }
        printf("RESULT chase %zu %.4f\n", bytes, median3(cbest, c2, c3));
        free(buf);
        free(pbuf);
    }
    return g_sink == 42.0 ? 1 : 0;
}
