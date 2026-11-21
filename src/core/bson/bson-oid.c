#include <stdatomic.h>
#include <time.h>
#include "bson.h"
#include "../dyn-prng.h"

static _Atomic uint32_t bson_oid_counter;
static _Atomic int bson_oid_ready;
static uint8_t bson_oid_rand[5];

static void bson_oid_init(void)
{
    uint8_t seed[9];
    int expected = 0;
    if (atomic_load_explicit(&bson_oid_ready, memory_order_acquire) == 2)
        return;
    if (!atomic_compare_exchange_strong(&bson_oid_ready, &expected, 1)) {
        while (atomic_load_explicit(&bson_oid_ready, memory_order_acquire) != 2)
            ;
        return;
    }
    if (dyn_os_entropy(seed, sizeof seed) < 0) {
        uint64_t t = (uint64_t)time(NULL) ^ (uint64_t)(uintptr_t)&seed;
        memcpy(seed, &t, 8);
        seed[8] = (uint8_t)(t >> 13);
    }
    memcpy(bson_oid_rand, seed, 5);
    atomic_store_explicit(&bson_oid_counter, (uint32_t)seed[5] | (uint32_t)seed[6] << 8 | (uint32_t)seed[7] << 16,
        memory_order_relaxed);
    atomic_store_explicit(&bson_oid_ready, 2, memory_order_release);
}

void dyn_bson_oid_generate(uint8_t out[12])
{
    uint32_t secs, ctr;
    bson_oid_init();
    secs = (uint32_t)time(NULL);
    ctr = atomic_fetch_add_explicit(&bson_oid_counter, 1, memory_order_relaxed);
    out[0] = (uint8_t)(secs >> 24);
    out[1] = (uint8_t)(secs >> 16);
    out[2] = (uint8_t)(secs >> 8);
    out[3] = (uint8_t)secs;
    memcpy(out + 4, bson_oid_rand, 5);
    out[9] = (uint8_t)(ctr >> 16);
    out[10] = (uint8_t)(ctr >> 8);
    out[11] = (uint8_t)ctr;
}
