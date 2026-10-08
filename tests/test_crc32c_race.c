#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>

uint32_t dyn_crc32c(const uint8_t *data, size_t len);

#define NTHREADS 8
#define NBUF     (1 << 16)
#define NITER    64

static uint8_t buf[NBUF];
static uint32_t results[NTHREADS];

static const size_t LENS[] = { 0, 1, 7, 8, 15, 64, 191, 192, 1000, NBUF };
#define NLENS (sizeof LENS / sizeof LENS[0])

static void *worker(void *arg)
{
    size_t idx = (size_t)arg;
    uint32_t acc = 2166136261u;
    int it;
    size_t i;
    for (it = 0; it < NITER; it++)
        for (i = 0; i < NLENS; i++)
            acc = acc * 16777619u + dyn_crc32c(buf, LENS[i]);
    results[idx] = acc;
    return NULL;
}

int main(void)
{
    pthread_t t[NTHREADS];
    size_t i;
    int bad = 0;

    for (i = 0; i < NBUF; i++)
        buf[i] = (uint8_t)(i * 167u + 13u);

    for (i = 0; i < NTHREADS; i++)
        if (pthread_create(&t[i], NULL, worker, (void *)i) != 0) {
            fprintf(stderr, "pthread_create failed\n");
            return 1;
        }
    for (i = 0; i < NTHREADS; i++)
        pthread_join(t[i], NULL);

    for (i = 1; i < NTHREADS; i++)
        if (results[i] != results[0]) {
            printf("  MISMATCH thread %zu: %08x != %08x\n",
                   i, results[i], results[0]);
            bad++;
        }
    printf("test_crc32c_race: %d threads x %d iters x %zu lengths, "
           "%d disagreements (checksum %08x)\n",
           NTHREADS, NITER, NLENS, bad, results[0]);
    return bad != 0;
}
