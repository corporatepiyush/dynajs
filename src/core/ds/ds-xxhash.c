#include "ds-internal.h"

uint64_t ds_xxh_round(uint64_t acc, uint64_t input)
{
    acc += input * DS_XXH_P2;
    acc = ds_rotl64(acc, 31);
    return acc * DS_XXH_P1;
}

uint64_t ds_xxh_merge(uint64_t acc, uint64_t val)
{
    acc ^= ds_xxh_round(0, val);
    return acc * DS_XXH_P1 + DS_XXH_P4;
}
