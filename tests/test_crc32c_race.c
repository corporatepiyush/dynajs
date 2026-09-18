/* test_crc32c_race.c -- dyn_crc32c from many threads at once, for TSan.
 *
 * WHY THIS EXISTS. dyn_crc32c used to fill a 64-entry power table lazily on
 * first use, excused in a comment by "every entry is a pure function of the
 * polynomial, so two threads racing to fill it write identical bytes". That is
 * still a data race, and the function is reachable from any JS thread:
 * serialize() via dyn_ser_finish, the dyna:compress dictionary id, the
 * dyna:crypto crc32c binding, and dyn_dict record ids. Two workers hashing
 * concurrently was enough.
 *
 * It survived because the whole hardware block was #if'd out on arm64 until
 * the ARMv8 CRC32 path was added, so the dev host never compiled it -- and
 * because NO TEST CALLED IT FROM TWO THREADS. A sanitizer only reports races
 * that actually execute; a clean TSan run over single-threaded tests says
 * nothing at all about this. Hence a test whose whole job is the concurrency.
 *
 * PROVED TO FIRE: restore the lazy crc32c_pow_init() and this reports
 * "WARNING: ThreadSanitizer: data race" on crc32c_pow / crc32c_pow_ready.
 * With the compile-time table it is silent, and every thread agrees on the
 * checksum -- which is the other half of the check, because a race that TSan
 * happens not to schedule still has to produce the right answer.
 *
 *   make test-crc32c-race     (built with -fsanitize=thread)
 */
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

/* Lengths straddle every branch in dyn_crc32c: below the hardware gate, the
 * 8-byte step, and past 3*64 where the three-chain split and the power table
 * come into play -- the table being the thing that used to be raced on. */
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
            /* NOT xor: `acc ^= crc` over an even NITER cancels to exactly 0 no
             * matter what dyn_crc32c returns, so every thread would "agree" on
             * a checksum that proves nothing. A multiply-accumulate keeps the
             * result a function of the values actually computed. */
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

    /* Every thread starts cold: nothing calls dyn_crc32c before this point, so
     * the threads race on whatever first-use initialisation the function has.
     * Calling it once here first would hide exactly the bug being tested. */
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
