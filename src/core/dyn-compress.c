#include "dyn-compress.h"

#include <stdlib.h>
#include <string.h>

#include "dyn-hash.h"

#define DYN_STORED_MAX 65535u

#if defined(__GNUC__) || defined(__clang__)
#define DYN_LIKELY(x) __builtin_expect(!!(x), 1)
#define DYN_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define DYN_INLINE static inline __attribute__((always_inline))
#define DYN_NOINLINE __attribute__((noinline))
#else
#define DYN_LIKELY(x) (x)
#define DYN_UNLIKELY(x) (x)
#define DYN_INLINE static inline
#define DYN_NOINLINE
#endif

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define DYN_BIG_ENDIAN 1
#else
#define DYN_BIG_ENDIAN 0
#endif

DYN_INLINE uint64_t dyn_ld64le(const void* p)
{
    uint64_t v;
    memcpy(&v, p, 8);
#if DYN_BIG_ENDIAN
    v = __builtin_bswap64(v);
#endif
    return v;
}

DYN_INLINE void dyn_st64le(void* p, uint64_t v)
{
#if DYN_BIG_ENDIAN
    v = __builtin_bswap64(v);
#endif
    memcpy(p, &v, 8);
}

#define DYN_OB_CAP_SLACK 320

static int dyn_ob_within(dyn_outbuf_t* o, int rc)
{
    size_t lim = o->limit && o->limit < DYN_MAX_OUTPUT ? o->limit
                                                       : DYN_MAX_OUTPUT;
    if (rc == 0 && o->len > lim) {
        o->exceeded = 1;
        return -1;
    }
    return rc;
}

static size_t dyn_ob_limit(const dyn_outbuf_t* o)
{
    return o->limit && o->limit < DYN_MAX_OUTPUT ? o->limit : DYN_MAX_OUTPUT;
}

DYN_NOINLINE static int dyn_ob_grow(dyn_outbuf_t* o, size_t extra)
{
    size_t need, ncap, lim;
    uint8_t* nb;

    if (extra > SIZE_MAX - o->len)
        return -1;
    need = o->len + extra;
    lim = dyn_ob_limit(o);
    if (o->len > lim || need > lim + DYN_OB_CAP_SLACK) {
        o->exceeded = 1;
        return -1;
    }
    lim += DYN_OB_CAP_SLACK;
    ncap = o->cap ? o->cap : 1024;
    if (ncap > lim)
        ncap = lim;
    while (ncap < need) {
        if (ncap > lim / 2) {
            ncap = need;
            break;
        }
        ncap <<= 1;
    }
    nb = (uint8_t*)realloc(o->buf, ncap);
    if (!nb)
        return -1;
    o->buf = nb;
    o->cap = ncap;
    return 0;
}

DYN_INLINE int dyn_ob_ensure(dyn_outbuf_t* o, size_t extra)
{
    if (DYN_LIKELY(extra <= o->cap - o->len))
        return 0;
    return dyn_ob_grow(o, extra);
}

static int dyn_ob_reserve(dyn_outbuf_t* o, size_t want)
{
    uint8_t* nb;
    size_t lim = dyn_ob_limit(o) + DYN_OB_CAP_SLACK;
    if (want <= o->cap)
        return 0;
    if (want > lim)
        want = lim;
    if (want <= o->cap)
        return 0;
    nb = (uint8_t*)realloc(o->buf, want);
    if (!nb)
        return -1;
    o->buf = nb;
    o->cap = want;
    return 0;
}

static const uint16_t dyn_fix_ll[288] = {
    0x0188,
    0x1188,
    0x0988,
    0x1988,
    0x0588,
    0x1588,
    0x0D88,
    0x1D88,
    0x0388,
    0x1388,
    0x0B88,
    0x1B88,
    0x0788,
    0x1788,
    0x0F88,
    0x1F88,
    0x0048,
    0x1048,
    0x0848,
    0x1848,
    0x0448,
    0x1448,
    0x0C48,
    0x1C48,
    0x0248,
    0x1248,
    0x0A48,
    0x1A48,
    0x0648,
    0x1648,
    0x0E48,
    0x1E48,
    0x0148,
    0x1148,
    0x0948,
    0x1948,
    0x0548,
    0x1548,
    0x0D48,
    0x1D48,
    0x0348,
    0x1348,
    0x0B48,
    0x1B48,
    0x0748,
    0x1748,
    0x0F48,
    0x1F48,
    0x00C8,
    0x10C8,
    0x08C8,
    0x18C8,
    0x04C8,
    0x14C8,
    0x0CC8,
    0x1CC8,
    0x02C8,
    0x12C8,
    0x0AC8,
    0x1AC8,
    0x06C8,
    0x16C8,
    0x0EC8,
    0x1EC8,
    0x01C8,
    0x11C8,
    0x09C8,
    0x19C8,
    0x05C8,
    0x15C8,
    0x0DC8,
    0x1DC8,
    0x03C8,
    0x13C8,
    0x0BC8,
    0x1BC8,
    0x07C8,
    0x17C8,
    0x0FC8,
    0x1FC8,
    0x0028,
    0x1028,
    0x0828,
    0x1828,
    0x0428,
    0x1428,
    0x0C28,
    0x1C28,
    0x0228,
    0x1228,
    0x0A28,
    0x1A28,
    0x0628,
    0x1628,
    0x0E28,
    0x1E28,
    0x0128,
    0x1128,
    0x0928,
    0x1928,
    0x0528,
    0x1528,
    0x0D28,
    0x1D28,
    0x0328,
    0x1328,
    0x0B28,
    0x1B28,
    0x0728,
    0x1728,
    0x0F28,
    0x1F28,
    0x00A8,
    0x10A8,
    0x08A8,
    0x18A8,
    0x04A8,
    0x14A8,
    0x0CA8,
    0x1CA8,
    0x02A8,
    0x12A8,
    0x0AA8,
    0x1AA8,
    0x06A8,
    0x16A8,
    0x0EA8,
    0x1EA8,
    0x01A8,
    0x11A8,
    0x09A8,
    0x19A8,
    0x05A8,
    0x15A8,
    0x0DA8,
    0x1DA8,
    0x03A8,
    0x13A8,
    0x0BA8,
    0x1BA8,
    0x07A8,
    0x17A8,
    0x0FA8,
    0x1FA8,
    0x0269,
    0x2269,
    0x1269,
    0x3269,
    0x0A69,
    0x2A69,
    0x1A69,
    0x3A69,
    0x0669,
    0x2669,
    0x1669,
    0x3669,
    0x0E69,
    0x2E69,
    0x1E69,
    0x3E69,
    0x0169,
    0x2169,
    0x1169,
    0x3169,
    0x0969,
    0x2969,
    0x1969,
    0x3969,
    0x0569,
    0x2569,
    0x1569,
    0x3569,
    0x0D69,
    0x2D69,
    0x1D69,
    0x3D69,
    0x0369,
    0x2369,
    0x1369,
    0x3369,
    0x0B69,
    0x2B69,
    0x1B69,
    0x3B69,
    0x0769,
    0x2769,
    0x1769,
    0x3769,
    0x0F69,
    0x2F69,
    0x1F69,
    0x3F69,
    0x00E9,
    0x20E9,
    0x10E9,
    0x30E9,
    0x08E9,
    0x28E9,
    0x18E9,
    0x38E9,
    0x04E9,
    0x24E9,
    0x14E9,
    0x34E9,
    0x0CE9,
    0x2CE9,
    0x1CE9,
    0x3CE9,
    0x02E9,
    0x22E9,
    0x12E9,
    0x32E9,
    0x0AE9,
    0x2AE9,
    0x1AE9,
    0x3AE9,
    0x06E9,
    0x26E9,
    0x16E9,
    0x36E9,
    0x0EE9,
    0x2EE9,
    0x1EE9,
    0x3EE9,
    0x01E9,
    0x21E9,
    0x11E9,
    0x31E9,
    0x09E9,
    0x29E9,
    0x19E9,
    0x39E9,
    0x05E9,
    0x25E9,
    0x15E9,
    0x35E9,
    0x0DE9,
    0x2DE9,
    0x1DE9,
    0x3DE9,
    0x03E9,
    0x23E9,
    0x13E9,
    0x33E9,
    0x0BE9,
    0x2BE9,
    0x1BE9,
    0x3BE9,
    0x07E9,
    0x27E9,
    0x17E9,
    0x37E9,
    0x0FE9,
    0x2FE9,
    0x1FE9,
    0x3FE9,
    0x0007,
    0x0807,
    0x0407,
    0x0C07,
    0x0207,
    0x0A07,
    0x0607,
    0x0E07,
    0x0107,
    0x0907,
    0x0507,
    0x0D07,
    0x0307,
    0x0B07,
    0x0707,
    0x0F07,
    0x0087,
    0x0887,
    0x0487,
    0x0C87,
    0x0287,
    0x0A87,
    0x0687,
    0x0E87,
    0x0068,
    0x1068,
    0x0868,
    0x1868,
    0x0468,
    0x1468,
    0x0C68,
    0x1C68,
};

static const uint8_t dyn_len_code[256] = {
    0,
    1,
    2,
    3,
    4,
    5,
    6,
    7,
    8,
    8,
    9,
    9,
    10,
    10,
    11,
    11,
    12,
    12,
    12,
    12,
    13,
    13,
    13,
    13,
    14,
    14,
    14,
    14,
    15,
    15,
    15,
    15,
    16,
    16,
    16,
    16,
    16,
    16,
    16,
    16,
    17,
    17,
    17,
    17,
    17,
    17,
    17,
    17,
    18,
    18,
    18,
    18,
    18,
    18,
    18,
    18,
    19,
    19,
    19,
    19,
    19,
    19,
    19,
    19,
    20,
    20,
    20,
    20,
    20,
    20,
    20,
    20,
    20,
    20,
    20,
    20,
    20,
    20,
    20,
    20,
    21,
    21,
    21,
    21,
    21,
    21,
    21,
    21,
    21,
    21,
    21,
    21,
    21,
    21,
    21,
    21,
    22,
    22,
    22,
    22,
    22,
    22,
    22,
    22,
    22,
    22,
    22,
    22,
    22,
    22,
    22,
    22,
    23,
    23,
    23,
    23,
    23,
    23,
    23,
    23,
    23,
    23,
    23,
    23,
    23,
    23,
    23,
    23,
    24,
    24,
    24,
    24,
    24,
    24,
    24,
    24,
    24,
    24,
    24,
    24,
    24,
    24,
    24,
    24,
    24,
    24,
    24,
    24,
    24,
    24,
    24,
    24,
    24,
    24,
    24,
    24,
    24,
    24,
    24,
    24,
    25,
    25,
    25,
    25,
    25,
    25,
    25,
    25,
    25,
    25,
    25,
    25,
    25,
    25,
    25,
    25,
    25,
    25,
    25,
    25,
    25,
    25,
    25,
    25,
    25,
    25,
    25,
    25,
    25,
    25,
    25,
    25,
    26,
    26,
    26,
    26,
    26,
    26,
    26,
    26,
    26,
    26,
    26,
    26,
    26,
    26,
    26,
    26,
    26,
    26,
    26,
    26,
    26,
    26,
    26,
    26,
    26,
    26,
    26,
    26,
    26,
    26,
    26,
    26,
    27,
    27,
    27,
    27,
    27,
    27,
    27,
    27,
    27,
    27,
    27,
    27,
    27,
    27,
    27,
    27,
    27,
    27,
    27,
    27,
    27,
    27,
    27,
    27,
    27,
    27,
    27,
    27,
    27,
    27,
    27,
    28,
};

static const uint8_t dyn_dist_code[512] = {
    0,
    1,
    2,
    3,
    4,
    4,
    5,
    5,
    6,
    6,
    6,
    6,
    7,
    7,
    7,
    7,
    8,
    8,
    8,
    8,
    8,
    8,
    8,
    8,
    9,
    9,
    9,
    9,
    9,
    9,
    9,
    9,
    10,
    10,
    10,
    10,
    10,
    10,
    10,
    10,
    10,
    10,
    10,
    10,
    10,
    10,
    10,
    10,
    11,
    11,
    11,
    11,
    11,
    11,
    11,
    11,
    11,
    11,
    11,
    11,
    11,
    11,
    11,
    11,
    12,
    12,
    12,
    12,
    12,
    12,
    12,
    12,
    12,
    12,
    12,
    12,
    12,
    12,
    12,
    12,
    12,
    12,
    12,
    12,
    12,
    12,
    12,
    12,
    12,
    12,
    12,
    12,
    12,
    12,
    12,
    12,
    13,
    13,
    13,
    13,
    13,
    13,
    13,
    13,
    13,
    13,
    13,
    13,
    13,
    13,
    13,
    13,
    13,
    13,
    13,
    13,
    13,
    13,
    13,
    13,
    13,
    13,
    13,
    13,
    13,
    13,
    13,
    13,
    14,
    14,
    14,
    14,
    14,
    14,
    14,
    14,
    14,
    14,
    14,
    14,
    14,
    14,
    14,
    14,
    14,
    14,
    14,
    14,
    14,
    14,
    14,
    14,
    14,
    14,
    14,
    14,
    14,
    14,
    14,
    14,
    14,
    14,
    14,
    14,
    14,
    14,
    14,
    14,
    14,
    14,
    14,
    14,
    14,
    14,
    14,
    14,
    14,
    14,
    14,
    14,
    14,
    14,
    14,
    14,
    14,
    14,
    14,
    14,
    14,
    14,
    14,
    14,
    15,
    15,
    15,
    15,
    15,
    15,
    15,
    15,
    15,
    15,
    15,
    15,
    15,
    15,
    15,
    15,
    15,
    15,
    15,
    15,
    15,
    15,
    15,
    15,
    15,
    15,
    15,
    15,
    15,
    15,
    15,
    15,
    15,
    15,
    15,
    15,
    15,
    15,
    15,
    15,
    15,
    15,
    15,
    15,
    15,
    15,
    15,
    15,
    15,
    15,
    15,
    15,
    15,
    15,
    15,
    15,
    15,
    15,
    15,
    15,
    15,
    15,
    15,
    15,
    0,
    14,
    16,
    17,
    18,
    18,
    19,
    19,
    20,
    20,
    20,
    20,
    21,
    21,
    21,
    21,
    22,
    22,
    22,
    22,
    22,
    22,
    22,
    22,
    23,
    23,
    23,
    23,
    23,
    23,
    23,
    23,
    24,
    24,
    24,
    24,
    24,
    24,
    24,
    24,
    24,
    24,
    24,
    24,
    24,
    24,
    24,
    24,
    25,
    25,
    25,
    25,
    25,
    25,
    25,
    25,
    25,
    25,
    25,
    25,
    25,
    25,
    25,
    25,
    26,
    26,
    26,
    26,
    26,
    26,
    26,
    26,
    26,
    26,
    26,
    26,
    26,
    26,
    26,
    26,
    26,
    26,
    26,
    26,
    26,
    26,
    26,
    26,
    26,
    26,
    26,
    26,
    26,
    26,
    26,
    26,
    27,
    27,
    27,
    27,
    27,
    27,
    27,
    27,
    27,
    27,
    27,
    27,
    27,
    27,
    27,
    27,
    27,
    27,
    27,
    27,
    27,
    27,
    27,
    27,
    27,
    27,
    27,
    27,
    27,
    27,
    27,
    27,
    28,
    28,
    28,
    28,
    28,
    28,
    28,
    28,
    28,
    28,
    28,
    28,
    28,
    28,
    28,
    28,
    28,
    28,
    28,
    28,
    28,
    28,
    28,
    28,
    28,
    28,
    28,
    28,
    28,
    28,
    28,
    28,
    28,
    28,
    28,
    28,
    28,
    28,
    28,
    28,
    28,
    28,
    28,
    28,
    28,
    28,
    28,
    28,
    28,
    28,
    28,
    28,
    28,
    28,
    28,
    28,
    28,
    28,
    28,
    28,
    28,
    28,
    28,
    28,
    29,
    29,
    29,
    29,
    29,
    29,
    29,
    29,
    29,
    29,
    29,
    29,
    29,
    29,
    29,
    29,
    29,
    29,
    29,
    29,
    29,
    29,
    29,
    29,
    29,
    29,
    29,
    29,
    29,
    29,
    29,
    29,
    29,
    29,
    29,
    29,
    29,
    29,
    29,
    29,
    29,
    29,
    29,
    29,
    29,
    29,
    29,
    29,
    29,
    29,
    29,
    29,
    29,
    29,
    29,
    29,
    29,
    29,
    29,
    29,
    29,
    29,
    29,
    29,
};

static const uint16_t dyn_fixtab_ll[512] = {
    0x1007,
    0x0508,
    0x0108,
    0x1188,
    0x1107,
    0x0708,
    0x0308,
    0x0C09,
    0x1087,
    0x0608,
    0x0208,
    0x0A09,
    0x0008,
    0x0808,
    0x0408,
    0x0E09,
    0x1047,
    0x0588,
    0x0188,
    0x0909,
    0x1147,
    0x0788,
    0x0388,
    0x0D09,
    0x10C7,
    0x0688,
    0x0288,
    0x0B09,
    0x0088,
    0x0888,
    0x0488,
    0x0F09,
    0x1027,
    0x0548,
    0x0148,
    0x11C8,
    0x1127,
    0x0748,
    0x0348,
    0x0C89,
    0x10A7,
    0x0648,
    0x0248,
    0x0A89,
    0x0048,
    0x0848,
    0x0448,
    0x0E89,
    0x1067,
    0x05C8,
    0x01C8,
    0x0989,
    0x1167,
    0x07C8,
    0x03C8,
    0x0D89,
    0x10E7,
    0x06C8,
    0x02C8,
    0x0B89,
    0x00C8,
    0x08C8,
    0x04C8,
    0x0F89,
    0x1017,
    0x0528,
    0x0128,
    0x11A8,
    0x1117,
    0x0728,
    0x0328,
    0x0C49,
    0x1097,
    0x0628,
    0x0228,
    0x0A49,
    0x0028,
    0x0828,
    0x0428,
    0x0E49,
    0x1057,
    0x05A8,
    0x01A8,
    0x0949,
    0x1157,
    0x07A8,
    0x03A8,
    0x0D49,
    0x10D7,
    0x06A8,
    0x02A8,
    0x0B49,
    0x00A8,
    0x08A8,
    0x04A8,
    0x0F49,
    0x1037,
    0x0568,
    0x0168,
    0x11E8,
    0x1137,
    0x0768,
    0x0368,
    0x0CC9,
    0x10B7,
    0x0668,
    0x0268,
    0x0AC9,
    0x0068,
    0x0868,
    0x0468,
    0x0EC9,
    0x1077,
    0x05E8,
    0x01E8,
    0x09C9,
    0x1177,
    0x07E8,
    0x03E8,
    0x0DC9,
    0x10F7,
    0x06E8,
    0x02E8,
    0x0BC9,
    0x00E8,
    0x08E8,
    0x04E8,
    0x0FC9,
    0x1007,
    0x0518,
    0x0118,
    0x1198,
    0x1107,
    0x0718,
    0x0318,
    0x0C29,
    0x1087,
    0x0618,
    0x0218,
    0x0A29,
    0x0018,
    0x0818,
    0x0418,
    0x0E29,
    0x1047,
    0x0598,
    0x0198,
    0x0929,
    0x1147,
    0x0798,
    0x0398,
    0x0D29,
    0x10C7,
    0x0698,
    0x0298,
    0x0B29,
    0x0098,
    0x0898,
    0x0498,
    0x0F29,
    0x1027,
    0x0558,
    0x0158,
    0x11D8,
    0x1127,
    0x0758,
    0x0358,
    0x0CA9,
    0x10A7,
    0x0658,
    0x0258,
    0x0AA9,
    0x0058,
    0x0858,
    0x0458,
    0x0EA9,
    0x1067,
    0x05D8,
    0x01D8,
    0x09A9,
    0x1167,
    0x07D8,
    0x03D8,
    0x0DA9,
    0x10E7,
    0x06D8,
    0x02D8,
    0x0BA9,
    0x00D8,
    0x08D8,
    0x04D8,
    0x0FA9,
    0x1017,
    0x0538,
    0x0138,
    0x11B8,
    0x1117,
    0x0738,
    0x0338,
    0x0C69,
    0x1097,
    0x0638,
    0x0238,
    0x0A69,
    0x0038,
    0x0838,
    0x0438,
    0x0E69,
    0x1057,
    0x05B8,
    0x01B8,
    0x0969,
    0x1157,
    0x07B8,
    0x03B8,
    0x0D69,
    0x10D7,
    0x06B8,
    0x02B8,
    0x0B69,
    0x00B8,
    0x08B8,
    0x04B8,
    0x0F69,
    0x1037,
    0x0578,
    0x0178,
    0x11F8,
    0x1137,
    0x0778,
    0x0378,
    0x0CE9,
    0x10B7,
    0x0678,
    0x0278,
    0x0AE9,
    0x0078,
    0x0878,
    0x0478,
    0x0EE9,
    0x1077,
    0x05F8,
    0x01F8,
    0x09E9,
    0x1177,
    0x07F8,
    0x03F8,
    0x0DE9,
    0x10F7,
    0x06F8,
    0x02F8,
    0x0BE9,
    0x00F8,
    0x08F8,
    0x04F8,
    0x0FE9,
    0x1007,
    0x0508,
    0x0108,
    0x1188,
    0x1107,
    0x0708,
    0x0308,
    0x0C19,
    0x1087,
    0x0608,
    0x0208,
    0x0A19,
    0x0008,
    0x0808,
    0x0408,
    0x0E19,
    0x1047,
    0x0588,
    0x0188,
    0x0919,
    0x1147,
    0x0788,
    0x0388,
    0x0D19,
    0x10C7,
    0x0688,
    0x0288,
    0x0B19,
    0x0088,
    0x0888,
    0x0488,
    0x0F19,
    0x1027,
    0x0548,
    0x0148,
    0x11C8,
    0x1127,
    0x0748,
    0x0348,
    0x0C99,
    0x10A7,
    0x0648,
    0x0248,
    0x0A99,
    0x0048,
    0x0848,
    0x0448,
    0x0E99,
    0x1067,
    0x05C8,
    0x01C8,
    0x0999,
    0x1167,
    0x07C8,
    0x03C8,
    0x0D99,
    0x10E7,
    0x06C8,
    0x02C8,
    0x0B99,
    0x00C8,
    0x08C8,
    0x04C8,
    0x0F99,
    0x1017,
    0x0528,
    0x0128,
    0x11A8,
    0x1117,
    0x0728,
    0x0328,
    0x0C59,
    0x1097,
    0x0628,
    0x0228,
    0x0A59,
    0x0028,
    0x0828,
    0x0428,
    0x0E59,
    0x1057,
    0x05A8,
    0x01A8,
    0x0959,
    0x1157,
    0x07A8,
    0x03A8,
    0x0D59,
    0x10D7,
    0x06A8,
    0x02A8,
    0x0B59,
    0x00A8,
    0x08A8,
    0x04A8,
    0x0F59,
    0x1037,
    0x0568,
    0x0168,
    0x11E8,
    0x1137,
    0x0768,
    0x0368,
    0x0CD9,
    0x10B7,
    0x0668,
    0x0268,
    0x0AD9,
    0x0068,
    0x0868,
    0x0468,
    0x0ED9,
    0x1077,
    0x05E8,
    0x01E8,
    0x09D9,
    0x1177,
    0x07E8,
    0x03E8,
    0x0DD9,
    0x10F7,
    0x06E8,
    0x02E8,
    0x0BD9,
    0x00E8,
    0x08E8,
    0x04E8,
    0x0FD9,
    0x1007,
    0x0518,
    0x0118,
    0x1198,
    0x1107,
    0x0718,
    0x0318,
    0x0C39,
    0x1087,
    0x0618,
    0x0218,
    0x0A39,
    0x0018,
    0x0818,
    0x0418,
    0x0E39,
    0x1047,
    0x0598,
    0x0198,
    0x0939,
    0x1147,
    0x0798,
    0x0398,
    0x0D39,
    0x10C7,
    0x0698,
    0x0298,
    0x0B39,
    0x0098,
    0x0898,
    0x0498,
    0x0F39,
    0x1027,
    0x0558,
    0x0158,
    0x11D8,
    0x1127,
    0x0758,
    0x0358,
    0x0CB9,
    0x10A7,
    0x0658,
    0x0258,
    0x0AB9,
    0x0058,
    0x0858,
    0x0458,
    0x0EB9,
    0x1067,
    0x05D8,
    0x01D8,
    0x09B9,
    0x1167,
    0x07D8,
    0x03D8,
    0x0DB9,
    0x10E7,
    0x06D8,
    0x02D8,
    0x0BB9,
    0x00D8,
    0x08D8,
    0x04D8,
    0x0FB9,
    0x1017,
    0x0538,
    0x0138,
    0x11B8,
    0x1117,
    0x0738,
    0x0338,
    0x0C79,
    0x1097,
    0x0638,
    0x0238,
    0x0A79,
    0x0038,
    0x0838,
    0x0438,
    0x0E79,
    0x1057,
    0x05B8,
    0x01B8,
    0x0979,
    0x1157,
    0x07B8,
    0x03B8,
    0x0D79,
    0x10D7,
    0x06B8,
    0x02B8,
    0x0B79,
    0x00B8,
    0x08B8,
    0x04B8,
    0x0F79,
    0x1037,
    0x0578,
    0x0178,
    0x11F8,
    0x1137,
    0x0778,
    0x0378,
    0x0CF9,
    0x10B7,
    0x0678,
    0x0278,
    0x0AF9,
    0x0078,
    0x0878,
    0x0478,
    0x0EF9,
    0x1077,
    0x05F8,
    0x01F8,
    0x09F9,
    0x1177,
    0x07F8,
    0x03F8,
    0x0DF9,
    0x10F7,
    0x06F8,
    0x02F8,
    0x0BF9,
    0x00F8,
    0x08F8,
    0x04F8,
    0x0FF9,
};

static const uint16_t dyn_fixtab_dist[512] = {
    0x0005,
    0x0105,
    0x0085,
    0x0185,
    0x0045,
    0x0145,
    0x00C5,
    0x01C5,
    0x0025,
    0x0125,
    0x00A5,
    0x01A5,
    0x0065,
    0x0165,
    0x00E5,
    0x0000,
    0x0015,
    0x0115,
    0x0095,
    0x0195,
    0x0055,
    0x0155,
    0x00D5,
    0x01D5,
    0x0035,
    0x0135,
    0x00B5,
    0x01B5,
    0x0075,
    0x0175,
    0x00F5,
    0x0000,
    0x0005,
    0x0105,
    0x0085,
    0x0185,
    0x0045,
    0x0145,
    0x00C5,
    0x01C5,
    0x0025,
    0x0125,
    0x00A5,
    0x01A5,
    0x0065,
    0x0165,
    0x00E5,
    0x0000,
    0x0015,
    0x0115,
    0x0095,
    0x0195,
    0x0055,
    0x0155,
    0x00D5,
    0x01D5,
    0x0035,
    0x0135,
    0x00B5,
    0x01B5,
    0x0075,
    0x0175,
    0x00F5,
    0x0000,
    0x0005,
    0x0105,
    0x0085,
    0x0185,
    0x0045,
    0x0145,
    0x00C5,
    0x01C5,
    0x0025,
    0x0125,
    0x00A5,
    0x01A5,
    0x0065,
    0x0165,
    0x00E5,
    0x0000,
    0x0015,
    0x0115,
    0x0095,
    0x0195,
    0x0055,
    0x0155,
    0x00D5,
    0x01D5,
    0x0035,
    0x0135,
    0x00B5,
    0x01B5,
    0x0075,
    0x0175,
    0x00F5,
    0x0000,
    0x0005,
    0x0105,
    0x0085,
    0x0185,
    0x0045,
    0x0145,
    0x00C5,
    0x01C5,
    0x0025,
    0x0125,
    0x00A5,
    0x01A5,
    0x0065,
    0x0165,
    0x00E5,
    0x0000,
    0x0015,
    0x0115,
    0x0095,
    0x0195,
    0x0055,
    0x0155,
    0x00D5,
    0x01D5,
    0x0035,
    0x0135,
    0x00B5,
    0x01B5,
    0x0075,
    0x0175,
    0x00F5,
    0x0000,
    0x0005,
    0x0105,
    0x0085,
    0x0185,
    0x0045,
    0x0145,
    0x00C5,
    0x01C5,
    0x0025,
    0x0125,
    0x00A5,
    0x01A5,
    0x0065,
    0x0165,
    0x00E5,
    0x0000,
    0x0015,
    0x0115,
    0x0095,
    0x0195,
    0x0055,
    0x0155,
    0x00D5,
    0x01D5,
    0x0035,
    0x0135,
    0x00B5,
    0x01B5,
    0x0075,
    0x0175,
    0x00F5,
    0x0000,
    0x0005,
    0x0105,
    0x0085,
    0x0185,
    0x0045,
    0x0145,
    0x00C5,
    0x01C5,
    0x0025,
    0x0125,
    0x00A5,
    0x01A5,
    0x0065,
    0x0165,
    0x00E5,
    0x0000,
    0x0015,
    0x0115,
    0x0095,
    0x0195,
    0x0055,
    0x0155,
    0x00D5,
    0x01D5,
    0x0035,
    0x0135,
    0x00B5,
    0x01B5,
    0x0075,
    0x0175,
    0x00F5,
    0x0000,
    0x0005,
    0x0105,
    0x0085,
    0x0185,
    0x0045,
    0x0145,
    0x00C5,
    0x01C5,
    0x0025,
    0x0125,
    0x00A5,
    0x01A5,
    0x0065,
    0x0165,
    0x00E5,
    0x0000,
    0x0015,
    0x0115,
    0x0095,
    0x0195,
    0x0055,
    0x0155,
    0x00D5,
    0x01D5,
    0x0035,
    0x0135,
    0x00B5,
    0x01B5,
    0x0075,
    0x0175,
    0x00F5,
    0x0000,
    0x0005,
    0x0105,
    0x0085,
    0x0185,
    0x0045,
    0x0145,
    0x00C5,
    0x01C5,
    0x0025,
    0x0125,
    0x00A5,
    0x01A5,
    0x0065,
    0x0165,
    0x00E5,
    0x0000,
    0x0015,
    0x0115,
    0x0095,
    0x0195,
    0x0055,
    0x0155,
    0x00D5,
    0x01D5,
    0x0035,
    0x0135,
    0x00B5,
    0x01B5,
    0x0075,
    0x0175,
    0x00F5,
    0x0000,
    0x0005,
    0x0105,
    0x0085,
    0x0185,
    0x0045,
    0x0145,
    0x00C5,
    0x01C5,
    0x0025,
    0x0125,
    0x00A5,
    0x01A5,
    0x0065,
    0x0165,
    0x00E5,
    0x0000,
    0x0015,
    0x0115,
    0x0095,
    0x0195,
    0x0055,
    0x0155,
    0x00D5,
    0x01D5,
    0x0035,
    0x0135,
    0x00B5,
    0x01B5,
    0x0075,
    0x0175,
    0x00F5,
    0x0000,
    0x0005,
    0x0105,
    0x0085,
    0x0185,
    0x0045,
    0x0145,
    0x00C5,
    0x01C5,
    0x0025,
    0x0125,
    0x00A5,
    0x01A5,
    0x0065,
    0x0165,
    0x00E5,
    0x0000,
    0x0015,
    0x0115,
    0x0095,
    0x0195,
    0x0055,
    0x0155,
    0x00D5,
    0x01D5,
    0x0035,
    0x0135,
    0x00B5,
    0x01B5,
    0x0075,
    0x0175,
    0x00F5,
    0x0000,
    0x0005,
    0x0105,
    0x0085,
    0x0185,
    0x0045,
    0x0145,
    0x00C5,
    0x01C5,
    0x0025,
    0x0125,
    0x00A5,
    0x01A5,
    0x0065,
    0x0165,
    0x00E5,
    0x0000,
    0x0015,
    0x0115,
    0x0095,
    0x0195,
    0x0055,
    0x0155,
    0x00D5,
    0x01D5,
    0x0035,
    0x0135,
    0x00B5,
    0x01B5,
    0x0075,
    0x0175,
    0x00F5,
    0x0000,
    0x0005,
    0x0105,
    0x0085,
    0x0185,
    0x0045,
    0x0145,
    0x00C5,
    0x01C5,
    0x0025,
    0x0125,
    0x00A5,
    0x01A5,
    0x0065,
    0x0165,
    0x00E5,
    0x0000,
    0x0015,
    0x0115,
    0x0095,
    0x0195,
    0x0055,
    0x0155,
    0x00D5,
    0x01D5,
    0x0035,
    0x0135,
    0x00B5,
    0x01B5,
    0x0075,
    0x0175,
    0x00F5,
    0x0000,
    0x0005,
    0x0105,
    0x0085,
    0x0185,
    0x0045,
    0x0145,
    0x00C5,
    0x01C5,
    0x0025,
    0x0125,
    0x00A5,
    0x01A5,
    0x0065,
    0x0165,
    0x00E5,
    0x0000,
    0x0015,
    0x0115,
    0x0095,
    0x0195,
    0x0055,
    0x0155,
    0x00D5,
    0x01D5,
    0x0035,
    0x0135,
    0x00B5,
    0x01B5,
    0x0075,
    0x0175,
    0x00F5,
    0x0000,
    0x0005,
    0x0105,
    0x0085,
    0x0185,
    0x0045,
    0x0145,
    0x00C5,
    0x01C5,
    0x0025,
    0x0125,
    0x00A5,
    0x01A5,
    0x0065,
    0x0165,
    0x00E5,
    0x0000,
    0x0015,
    0x0115,
    0x0095,
    0x0195,
    0x0055,
    0x0155,
    0x00D5,
    0x01D5,
    0x0035,
    0x0135,
    0x00B5,
    0x01B5,
    0x0075,
    0x0175,
    0x00F5,
    0x0000,
    0x0005,
    0x0105,
    0x0085,
    0x0185,
    0x0045,
    0x0145,
    0x00C5,
    0x01C5,
    0x0025,
    0x0125,
    0x00A5,
    0x01A5,
    0x0065,
    0x0165,
    0x00E5,
    0x0000,
    0x0015,
    0x0115,
    0x0095,
    0x0195,
    0x0055,
    0x0155,
    0x00D5,
    0x01D5,
    0x0035,
    0x0135,
    0x00B5,
    0x01B5,
    0x0075,
    0x0175,
    0x00F5,
    0x0000,
    0x0005,
    0x0105,
    0x0085,
    0x0185,
    0x0045,
    0x0145,
    0x00C5,
    0x01C5,
    0x0025,
    0x0125,
    0x00A5,
    0x01A5,
    0x0065,
    0x0165,
    0x00E5,
    0x0000,
    0x0015,
    0x0115,
    0x0095,
    0x0195,
    0x0055,
    0x0155,
    0x00D5,
    0x01D5,
    0x0035,
    0x0135,
    0x00B5,
    0x01B5,
    0x0075,
    0x0175,
    0x00F5,
    0x0000,
};

static const uint8_t dyn_rev5[32] = { 0x00, 0x10, 0x08, 0x18, 0x04, 0x14, 0x0C, 0x1C, 0x02, 0x12, 0x0A, 0x1A, 0x06, 0x16, 0x0E, 0x1E, 0x01, 0x11, 0x09, 0x19, 0x05, 0x15, 0x0D, 0x1D, 0x03, 0x13, 0x0B, 0x1B, 0x07, 0x17, 0x0F, 0x1F };
static const short dyn_lens[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67,
    83, 99, 115, 131, 163, 195, 227, 258
};
static const short dyn_lext[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5,
    5, 5, 5, 0
};
static const short dyn_dists[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513,
    769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
};
static const short dyn_dext[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10,
    11, 11, 12, 12, 13, 13
};

typedef struct {
    const uint8_t* data;
    size_t len;
    size_t pos;
    uint64_t bitbuf;
    int bitcnt;
    int error;
} dyn_bitreader_t;

DYN_INLINE void dyn_refill(dyn_bitreader_t* s)
{
    if (DYN_LIKELY(s->pos + 8 <= s->len)) {
        s->bitbuf |= dyn_ld64le(s->data + s->pos) << s->bitcnt;
        s->pos += (size_t)((63 - s->bitcnt) >> 3);
        s->bitcnt |= 56;
        s->bitbuf &= ((uint64_t)1 << s->bitcnt) - 1;
    } else {
        while (s->bitcnt <= 56 && s->pos < s->len) {
            s->bitbuf |= (uint64_t)s->data[s->pos++] << s->bitcnt;
            s->bitcnt += 8;
        }
    }
}

DYN_INLINE unsigned dyn_bits(dyn_bitreader_t* s, int need)
{
    unsigned v;
    if (DYN_UNLIKELY(s->bitcnt < need)) {
        dyn_refill(s);
        if (DYN_UNLIKELY(s->bitcnt < need)) {
            s->error = 1;
            return 0;
        }
    }
    v = (unsigned)(s->bitbuf & (((uint64_t)1 << need) - 1));
    s->bitbuf >>= need;
    s->bitcnt -= need;
    return v;
}

#define DYN_MAXBITS 15
#define DYN_MAXLCODES 286
#define DYN_MAXDCODES 30
#define DYN_FIXLCODES 288
#define DYN_MAXCODES (DYN_MAXLCODES + DYN_MAXDCODES)

#define DYN_ROOT 9
#define DYN_ROOT_SIZE (1 << DYN_ROOT)

typedef struct {
    const uint16_t* fast;
    int root_complete;
    short count[DYN_MAXBITS + 1];
    short symbol[DYN_MAXLCODES];
    uint16_t table[DYN_ROOT_SIZE];
} dyn_huff_t;

static int dyn_huff_build(dyn_huff_t* h, const short* length, int n)
{
    short offs[DYN_MAXBITS + 1];
    unsigned nextcode[DYN_MAXBITS + 1];
    unsigned code;
    int sym, len, left;

    for (len = 0; len <= DYN_MAXBITS; len++)
        h->count[len] = 0;
    for (sym = 0; sym < n; sym++)
        h->count[length[sym]]++;
    if (h->count[0] == n) {
        memset(h->table, 0, sizeof h->table);
        h->fast = h->table;
        h->root_complete = 0;
        return 0;
    }
    left = 1;
    for (len = 1; len <= DYN_MAXBITS; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0)
            return left;
    }
    offs[1] = 0;
    for (len = 1; len < DYN_MAXBITS; len++)
        offs[len + 1] = (short)(offs[len] + h->count[len]);
    for (sym = 0; sym < n; sym++)
        if (length[sym] != 0)
            h->symbol[offs[length[sym]]++] = (short)sym;

    memset(h->table, 0, sizeof h->table);
    code = 0;
    for (len = 1; len <= DYN_MAXBITS; len++) {
        code = (code + (unsigned)h->count[len - 1]) << 1;
        nextcode[len] = code;
    }
    for (sym = 0; sym < n; sym++) {
        int l = length[sym];
        if (l == 0 || l > DYN_ROOT) {
            if (l)
                nextcode[l]++;
            continue;
        }
        {
            unsigned c = nextcode[l]++;
            unsigned r = 0, k;
            int b;
            for (b = 0; b < l; b++) {
                r = (r << 1) | (c & 1u);
                c >>= 1;
            }
            for (k = r; k < DYN_ROOT_SIZE; k += (1u << l))
                h->table[k] = (uint16_t)(((unsigned)sym << 4) | (unsigned)l);
        }
    }
    h->fast = h->table;
    {
        int over = 0;
        for (len = DYN_ROOT + 1; len <= DYN_MAXBITS; len++)
            over |= h->count[len];
        h->root_complete = (left == 0 && over == 0);
    }
    return left;
}

DYN_NOINLINE static int dyn_huff_slow(dyn_bitreader_t* s, const dyn_huff_t* h)
{
    int code = 0, first = 0, index = 0, len, cnt;
    for (len = 1; len <= DYN_MAXBITS; len++) {
        if (DYN_UNLIKELY(s->bitcnt < 1)) {
            dyn_refill(s);
            if (s->bitcnt < 1) {
                s->error = 1;
                return -1;
            }
        }
        code |= (int)(s->bitbuf & 1u);
        s->bitbuf >>= 1;
        s->bitcnt--;
        cnt = h->count[len];
        if (code - cnt < first)
            return h->symbol[index + (code - first)];
        index += cnt;
        first += cnt;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

DYN_INLINE int dyn_huff_decode(dyn_bitreader_t* s, const dyn_huff_t* h)
{
    unsigned e;
    int l;
    if (DYN_UNLIKELY(s->bitcnt < DYN_MAXBITS))
        dyn_refill(s);
    e = h->fast[s->bitbuf & (DYN_ROOT_SIZE - 1)];
    l = (int)(e & 15u);
    if (DYN_LIKELY(l != 0 && l <= s->bitcnt)) {
        s->bitbuf >>= l;
        s->bitcnt -= l;
        return (int)(e >> 4);
    }
    if (h->root_complete) {
        s->error = 1;
        return -1;
    }
    return dyn_huff_slow(s, h);
}

DYN_INLINE void dyn_copy_match(uint8_t* op, size_t dist, size_t n)
{
    const uint8_t* mp = op - dist;
    size_t k = 0;

    if (DYN_LIKELY(dist >= 8)) {
        do {
            memcpy(op + k, mp + k, 8);
            k += 8;
        } while (k < n);
        return;
    }
    {
        size_t d8 = dist;
        while (d8 < 8)
            d8 += dist;
        while (k < n && k < d8) {
            op[k] = mp[k];
            k++;
        }
        while (k < n) {
            memcpy(op + k, op + k - d8, 8);
            k += 8;
        }
    }
}

#define DYN_OSLACK 320
_Static_assert(DYN_OSLACK <= DYN_OB_CAP_SLACK, "inflate wild-copy slack exceeds the capacity slack");

static int dyn_inflate_codes(dyn_bitreader_t* s, dyn_outbuf_t* o,
    const dyn_huff_t* lh, const dyn_huff_t* dh)
{
    uint8_t* op = o->buf + o->len;
    uint8_t* oend = o->buf + o->cap;

    for (;;) {
        int sym;

        if (DYN_UNLIKELY((size_t)(oend - op) < DYN_OSLACK)) {
            o->len = (size_t)(op - o->buf);
            if (dyn_ob_ensure(o, DYN_OSLACK))
                return -1;
            op = o->buf + o->len;
            oend = o->buf + o->cap;
        }

        sym = dyn_huff_decode(s, lh);
        if (DYN_UNLIKELY(sym < 0))
            return -1;
        if (DYN_LIKELY(sym < 256)) {
            *op++ = (uint8_t)sym;
            continue;
        }
        if (sym == 256) {
            o->len = (size_t)(op - o->buf);
            return 0;
        }
        {
            int isym = sym - 257;
            int dsv;
            size_t length, dist;

            if (DYN_UNLIKELY(isym >= 29))
                return -1;
            length = (size_t)dyn_lens[isym] + dyn_bits(s, dyn_lext[isym]);
            dsv = dyn_huff_decode(s, dh);
            if (DYN_UNLIKELY(dsv < 0 || dsv >= 30))
                return -1;
            dist = (size_t)dyn_dists[dsv] + dyn_bits(s, dyn_dext[dsv]);
            if (DYN_UNLIKELY(s->error))
                return -1;
            if (DYN_UNLIKELY(dist > (size_t)(op - o->buf)))
                return -1;
            dyn_copy_match(op, dist, length);
            op += length;
        }
    }
}

static int dyn_inflate_stored(dyn_bitreader_t* s, dyn_outbuf_t* o)
{
    unsigned len, nlen;

    s->pos -= (size_t)(s->bitcnt >> 3);
    s->bitbuf = 0;
    s->bitcnt = 0;
    if (s->pos + 4 > s->len) {
        s->error = 1;
        return -1;
    }
    len = (unsigned)s->data[s->pos] | ((unsigned)s->data[s->pos + 1] << 8);
    nlen = (unsigned)s->data[s->pos + 2] | ((unsigned)s->data[s->pos + 3] << 8);
    s->pos += 4;
    if ((len ^ 0xffffu) != nlen)
        return -1;
    if (len > s->len - s->pos) {
        s->error = 1;
        return -1;
    }
    if (dyn_ob_ensure(o, len))
        return -1;
    if (len) {
        memcpy(o->buf + o->len, s->data + s->pos, len);
        o->len += len;
        s->pos += len;
    }
    return 0;
}

static int dyn_inflate_fixed(dyn_bitreader_t* s, dyn_outbuf_t* o)
{
    dyn_huff_t lh, dh;

    lh.fast = dyn_fixtab_ll;
    lh.root_complete = 1;
    dh.fast = dyn_fixtab_dist;
    dh.root_complete = 1;
    return dyn_inflate_codes(s, o, &lh, &dh);
}

static int dyn_inflate_dynamic(dyn_bitreader_t* s, dyn_outbuf_t* o,
    dyn_huff_t* lh, dyn_huff_t* dh, dyn_huff_t* ch)
{
    static const short order[19] = {
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
    };
    short lengths[DYN_MAXCODES];
    int hlit, hdist, hclen, idx, err, total, sym;

    hlit = (int)dyn_bits(s, 5) + 257;
    hdist = (int)dyn_bits(s, 5) + 1;
    hclen = (int)dyn_bits(s, 4) + 4;
    if (s->error)
        return -1;
    if (hlit > DYN_MAXLCODES || hdist > DYN_MAXDCODES)
        return -1;

    for (idx = 0; idx < 19; idx++)
        lengths[idx] = 0;
    for (idx = 0; idx < hclen; idx++)
        lengths[order[idx]] = (short)dyn_bits(s, 3);
    if (s->error)
        return -1;
    if (dyn_huff_build(ch, lengths, 19) != 0)
        return -1;

    total = hlit + hdist;
    idx = 0;
    while (idx < total) {
        sym = dyn_huff_decode(s, ch);
        if (sym < 0)
            return -1;
        if (sym < 16) {
            lengths[idx++] = (short)sym;
        } else {
            int rep;
            short val = 0;
            if (sym == 16) {
                if (idx == 0)
                    return -1;
                val = lengths[idx - 1];
                rep = 3 + (int)dyn_bits(s, 2);
            } else if (sym == 17) {
                rep = 3 + (int)dyn_bits(s, 3);
            } else {
                rep = 11 + (int)dyn_bits(s, 7);
            }
            if (s->error)
                return -1;
            if (idx + rep > total)
                return -1;
            while (rep--)
                lengths[idx++] = val;
        }
    }
    if (lengths[256] == 0)
        return -1;

    err = dyn_huff_build(lh, lengths, hlit);
    if (err && (err < 0 || hlit != lh->count[0] + lh->count[1]))
        return -1;
    err = dyn_huff_build(dh, lengths + hlit, hdist);
    if (err && (err < 0 || hdist != dh->count[0] + dh->count[1]))
        return -1;
    return dyn_inflate_codes(s, o, lh, dh);
}

static int dyn_inflate(const uint8_t* src, size_t src_len, dyn_outbuf_t* o,
    size_t* pconsumed)
{
    dyn_bitreader_t s;
    dyn_huff_t* tabs;
    int last, type, rc = -1;

    s.data = src;
    s.len = src_len;
    s.pos = 0;
    s.bitbuf = 0;
    s.bitcnt = 0;
    s.error = 0;

    tabs = (dyn_huff_t*)malloc(sizeof(dyn_huff_t) * 3);
    if (!tabs)
        return -1;

    do {
        last = (int)dyn_bits(&s, 1);
        type = (int)dyn_bits(&s, 2);
        if (s.error)
            goto out;
        if (type == 0)
            rc = dyn_inflate_stored(&s, o);
        else if (type == 1)
            rc = dyn_inflate_fixed(&s, o);
        else if (type == 2)
            rc = dyn_inflate_dynamic(&s, o, &tabs[0], &tabs[1], &tabs[2]);
        else
            goto out;
        if (rc)
            goto out;
    } while (!last);
    rc = 0;
out:
    if (rc == 0)
        *pconsumed = s.pos - (size_t)(s.bitcnt >> 3);
    free(tabs);
    return rc;
}

typedef struct {
    uint8_t* op;
    uint8_t* oend;
    uint64_t acc;
    int nbits;
} dyn_bw_t;

DYN_INLINE void dyn_bw_add(dyn_bw_t* w, unsigned value, int nbits)
{
    w->acc |= (uint64_t)value << w->nbits;
    w->nbits += nbits;
}

DYN_INLINE void dyn_bw_drain(dyn_bw_t* w)
{
    dyn_st64le(w->op, w->acc);
    w->op += (size_t)(w->nbits >> 3);
    w->acc >>= (w->nbits & ~7);
    w->nbits &= 7;
}

#define DYN_MIN_MATCH 3
#define DYN_MAX_MATCH 258
#define DYN_WSIZE ((size_t)32768)
#define DYN_HASH_BITS 15
#define DYN_HASH_SIZE ((size_t)1 << DYN_HASH_BITS)
#define DYN_MAX_CHAIN 256

#define DYN_LZ4_MIN_MATCH 4
#define DYN_LZ4_LAST_LITERALS 5
#define DYN_LZ4_MF_LIMIT 12
#define DYN_LZ4_HASH_BITS 14
#define DYN_LZ4_HASH_SIZE ((size_t)1 << DYN_LZ4_HASH_BITS)
#define DYN_LZ4_MAX_DIST 65535

#define DYN_PREV_SIZE ((size_t)65536)
#define DYN_PREV_MASK (DYN_PREV_SIZE - 1)

DYN_INLINE uint32_t dyn_hash3(const uint8_t* p, int shift)
{
    uint32_t v = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
    return (v * 2654435761u) >> shift;
}

DYN_INLINE uint32_t dyn_lz4_hash_at(const uint8_t* p, int shift)
{
    uint32_t v = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    return (v * 2654435761u) >> shift;
}

int dyn_comp_ctx_init(dyn_comp_ctx_t* c)
{
    c->head = (int32_t*)calloc(DYN_HASH_SIZE, sizeof(int32_t));
    c->prev = (uint16_t*)malloc(DYN_PREV_SIZE * sizeof(uint16_t));
    c->head_cap = DYN_HASH_SIZE;
    c->prev_cap = DYN_PREV_SIZE;
    c->base = 1;
    if (!c->head || !c->prev) {
        free(c->head);
        free(c->prev);
        c->head = NULL;
        c->prev = NULL;
        c->head_cap = c->prev_cap = 0;
        return -1;
    }
    return 0;
}

void dyn_comp_ctx_free(dyn_comp_ctx_t* c)
{
    free(c->head);
    free(c->prev);
    c->head = NULL;
    c->prev = NULL;
    c->prev_cap = c->head_cap = 0;
}

typedef struct {
    int32_t* head;
    uint16_t* prev;
    int shift;
    uint32_t base;
    int owned;
} dyn_scratch_t;

static int dyn_scratch_get(dyn_comp_ctx_t* c, int hash_bits, size_t n,
    dyn_scratch_t* sc, int need_prev)
{
    int bits = hash_bits;
    size_t heads;

    while (bits > 8 && ((size_t)1 << (bits - 1)) >= n + 1)
        bits--;
    heads = (size_t)1 << bits;

    sc->owned = 0;
    sc->shift = 32 - bits;
    if (c && c->head_cap >= heads && (!need_prev || c->prev_cap >= DYN_PREV_SIZE)) {
        sc->head = c->head;
        sc->prev = c->prev;
        if (c->base > (uint32_t)INT32_MAX - (uint32_t)(n + 1)) {
            memset(c->head, 0, c->head_cap * sizeof(int32_t));
            c->base = 1;
        }
        sc->base = c->base;
        c->base += (uint32_t)n + 1;
        return 0;
    }
    sc->head = (int32_t*)calloc(heads, sizeof(int32_t));
    sc->prev = need_prev
        ? (uint16_t*)malloc(DYN_PREV_SIZE * sizeof(uint16_t))
        : NULL;
    if (!sc->head || (need_prev && !sc->prev)) {
        free(sc->head);
        free(sc->prev);
        return -1;
    }
    sc->owned = 1;
    sc->base = 1;
    return 0;
}

static void dyn_scratch_put(dyn_scratch_t* sc)
{
    if (sc->owned) {
        free(sc->head);
        free(sc->prev);
    }
}

#if DYN_BIG_ENDIAN
#define DYN_FIRST_DIFF_BYTE(x) ((unsigned)(__builtin_clzll(x) >> 3))
#else
#define DYN_FIRST_DIFF_BYTE(x) ((unsigned)(__builtin_ctzll(x) >> 3))
#endif

DYN_INLINE size_t dyn_match_len(const uint8_t* a, const uint8_t* b, size_t max_len)
{
    size_t ml = 0;
#if defined(__GNUC__) || defined(__clang__)
    while (ml + 8 <= max_len) {
        uint64_t x = dyn_ld64le(a + ml) ^ dyn_ld64le(b + ml);
        if (x)
            return ml + DYN_FIRST_DIFF_BYTE(x);
        ml += 8;
    }
#endif
    while (ml < max_len && a[ml] == b[ml])
        ml++;
    return ml;
}

DYN_INLINE void dyn_emit_ll(dyn_bw_t* w, unsigned sym)
{
    uint16_t e = dyn_fix_ll[sym];
    dyn_bw_add(w, (unsigned)(e >> 5), (int)(e & 31u));
}

DYN_INLINE unsigned dyn_dist_index(unsigned dist)
{
    unsigned d = dist - 1u;
    return d < 256u ? dyn_dist_code[d] : dyn_dist_code[256u + (d >> 7)];
}

#define DYN_TOK_LIT 0xffffu

typedef struct {
    uint16_t* tok;
    size_t n;
    size_t cap;
    size_t lfreq[DYN_MAXLCODES];
    size_t dfreq[DYN_MAXDCODES];
    unsigned long long extbits;
} dyn_tokrec_t;

static int dyn_tokrec_grow(dyn_tokrec_t* r)
{
    size_t ncap = r->cap ? r->cap * 2 : 4096;
    uint16_t* nb;
    if (ncap < r->cap || ncap > SIZE_MAX / sizeof(uint16_t))
        return -1;
    nb = (uint16_t*)realloc(r->tok, ncap * sizeof(uint16_t));
    if (!nb)
        return -1;
    r->tok = nb;
    r->cap = ncap;
    return 0;
}

static int dyn_tokrec_lit(dyn_tokrec_t* r, unsigned byte)
{
    if (r->n * 2 + 2 > r->cap && dyn_tokrec_grow(r))
        return -1;
    r->tok[r->n * 2] = (uint16_t)byte;
    r->tok[r->n * 2 + 1] = DYN_TOK_LIT;
    r->lfreq[byte]++;
    r->n++;
    return 0;
}

static int dyn_tokrec_match(dyn_tokrec_t* r, size_t mlen, unsigned dist)
{
    unsigned li = dyn_len_code[mlen - 3];
    unsigned di = dyn_dist_index(dist);
    if (r->n * 2 + 2 > r->cap && dyn_tokrec_grow(r))
        return -1;
    r->tok[r->n * 2] = (uint16_t)mlen;
    r->tok[r->n * 2 + 1] = (uint16_t)dist;
    r->lfreq[257u + li]++;
    r->dfreq[di]++;
    r->extbits += (unsigned)dyn_lext[li] + (unsigned)dyn_dext[di];
    r->n++;
    return 0;
}

DYN_INLINE void dyn_emit_fixed_match(dyn_bw_t* w, size_t mlen, unsigned dist)
{
    unsigned li = dyn_len_code[mlen - 3];
    unsigned di = dyn_dist_index(dist);

    dyn_emit_ll(w, 257u + li);
    if (dyn_lext[li])
        dyn_bw_add(w, (unsigned)(mlen - (size_t)dyn_lens[li]), dyn_lext[li]);
    dyn_bw_add(w, dyn_rev5[di], 5);
    if (dyn_dext[di])
        dyn_bw_add(w, (unsigned)(dist - (unsigned)dyn_dists[di]), dyn_dext[di]);
    dyn_bw_drain(w);
}

static int dyn_lz_parse(const uint8_t* src, size_t len, dyn_bw_t* w,
    dyn_comp_ctx_t* cx, dyn_tokrec_t* rec)
{
    dyn_scratch_t sc;
    int32_t* head;
    uint16_t* prev;
    uint32_t base;
    int shift;
    size_t i;
    int rc = 0;

    if (dyn_scratch_get(cx, DYN_HASH_BITS, len, &sc, 1))
        return -1;
    head = sc.head;
    prev = sc.prev;
    base = sc.base;
    shift = sc.shift;

    i = 0;
    while (i < len) {
        size_t best_len = DYN_MIN_MATCH - 1;
        unsigned best_dist = 0;

        if (DYN_UNLIKELY(rec == NULL && w->op >= w->oend)) {
            rc = 1;
            goto done;
        }

        if (DYN_LIKELY(i + DYN_MIN_MATCH <= len)) {
            uint32_t h = dyn_hash3(src + i, shift);
            int32_t hv = head[h];
            head[h] = (int32_t)(i + base);

            if (hv >= (int32_t)base) {
                size_t cur = (size_t)(hv - (int32_t)base);
                size_t dist = i - cur;
                size_t max_len = len - i;
                const uint8_t* a = src + i;
                int chain = DYN_MAX_CHAIN;

                prev[i & DYN_PREV_MASK] = (uint16_t)(dist <= 65535 ? dist : 0);
                if (max_len > DYN_MAX_MATCH)
                    max_len = DYN_MAX_MATCH;

                while (dist <= DYN_WSIZE) {
                    const uint8_t* b = src + cur;
                    uint16_t g = prev[cur & DYN_PREV_MASK];

                    if (b[best_len] == a[best_len] && b[best_len - 1] == a[best_len - 1]) {
                        size_t ml = dyn_match_len(a, b, max_len);
                        if (ml > best_len) {
                            best_len = ml;
                            best_dist = (unsigned)dist;
                            if (ml >= max_len)
                                break;
                        }
                    }
                    if (g == 0 || --chain <= 0)
                        break;
                    cur -= g;
                    dist += g;
                }
            } else {
                prev[i & DYN_PREV_MASK] = 0;
            }
        }

        if (best_len >= DYN_MIN_MATCH) {
            size_t end = i + best_len;
            size_t k;

            if (rec) {
                if (dyn_tokrec_match(rec, best_len, best_dist)) {
                    rc = -1;
                    goto done;
                }
            } else {
                dyn_emit_fixed_match(w, best_len, best_dist);
            }

            for (k = i + 1; k < end; k++) {
                if (k + DYN_MIN_MATCH <= len) {
                    uint32_t hh = dyn_hash3(src + k, shift);
                    int32_t pv = head[hh];
                    size_t g = (pv >= (int32_t)base)
                        ? k - (size_t)(pv - (int32_t)base)
                        : 0;
                    prev[k & DYN_PREV_MASK] = (uint16_t)(g <= 65535 ? g : 0);
                    head[hh] = (int32_t)(k + base);
                }
            }
            i = end;
        } else {
            if (rec) {
                if (dyn_tokrec_lit(rec, src[i])) {
                    rc = -1;
                    goto done;
                }
            } else {
                dyn_emit_ll(w, src[i]);
                dyn_bw_drain(w);
            }
            i++;
        }
    }

done:
    dyn_scratch_put(&sc);
    return rc;
}

static int dyn_deflate_fixed(const uint8_t* src, size_t len,
    uint8_t* dst, size_t cap, dyn_comp_ctx_t* cx,
    size_t* pwritten)
{
    dyn_bw_t w;
    int rc;

    w.op = dst;
    w.oend = dst + cap;
    w.acc = 0;
    w.nbits = 0;

    dyn_bw_add(&w, 3, 3);

    if (len == 0) {
        dyn_emit_ll(&w, 256);
        dyn_bw_drain(&w);
        if (w.nbits > 0)
            *w.op++ = (uint8_t)(w.acc & 0xffu);
        *pwritten = (size_t)(w.op - dst);
        return *pwritten >= cap ? 1 : 0;
    }
    if (len > (size_t)INT32_MAX)
        return -1;

    rc = dyn_lz_parse(src, len, &w, cx, NULL);
    if (rc == 0) {
        dyn_emit_ll(&w, 256);
        dyn_bw_drain(&w);
        if (w.nbits > 0)
            *w.op++ = (uint8_t)(w.acc & 0xffu);
        if (w.op >= w.oend)
            rc = 1;
    }
    *pwritten = (size_t)(w.op - dst);
    return rc;
}

static size_t dyn_deflate_stored(const uint8_t* src, size_t len, uint8_t* dst)
{
    uint8_t* p = dst;
    size_t off;

    if (len == 0) {
        *p++ = 0x01;
        *p++ = 0x00;
        *p++ = 0x00;
        *p++ = 0xff;
        *p++ = 0xff;
        return (size_t)(p - dst);
    }
    off = 0;
    while (off < len) {
        size_t chunk = len - off;
        unsigned l, nl;
        if (chunk > DYN_STORED_MAX)
            chunk = DYN_STORED_MAX;
        l = (unsigned)chunk;
        nl = (~l) & 0xffffu;
        *p++ = (uint8_t)(off + chunk == len ? 0x01 : 0x00);
        *p++ = (uint8_t)(l & 0xff);
        *p++ = (uint8_t)((l >> 8) & 0xff);
        *p++ = (uint8_t)(nl & 0xff);
        *p++ = (uint8_t)((nl >> 8) & 0xff);
        memcpy(p, src + off, chunk);
        p += chunk;
        off += chunk;
    }
    return (size_t)(p - dst);
}

#define DYN_GZIP_DYN_LEVEL 6

#define DYN_DYN_RETRY_FIXED 2

#define DYN_CL_CODES 19
#define DYN_CL_MAXBITS 7

DYN_INLINE void dyn_huff_heap_push(int* heap, const size_t* f, int* phn,
    int node)
{
    int i = (*phn)++;
    heap[i] = node;
    while (i > 0) {
        int p = (i - 1) >> 1;
        int a = heap[p], b = heap[i];
        if (f[a] < f[b] || (f[a] == f[b] && a < b))
            break;
        heap[p] = b;
        heap[i] = a;
        i = p;
    }
}

DYN_INLINE int dyn_huff_heap_pop(int* heap, const size_t* f, int* phn)
{
    int top = heap[0];
    int i = 0;
    heap[0] = heap[--(*phn)];
    for (;;) {
        int l = 2 * i + 1, r = l + 1, s = i;
        if (l < *phn) {
            int a = heap[l], b = heap[s];
            if (f[a] < f[b] || (f[a] == f[b] && a < b))
                s = l;
        }
        if (r < *phn) {
            int a = heap[r], b = heap[s];
            if (f[a] < f[b] || (f[a] == f[b] && a < b))
                s = r;
        }
        if (s == i)
            break;
        l = heap[i];
        heap[i] = heap[s];
        heap[s] = l;
        i = s;
    }
    return top;
}

static int dyn_huff_lengths(const size_t* freq, int n, int maxbits,
    uint8_t* lens)
{
    size_t f[2 * DYN_MAXCODES];
    int parent[2 * DYN_MAXCODES];
    int heap[2 * DYN_MAXCODES];
    int syms[DYN_MAXCODES];
    int order[DYN_MAXCODES];
    uint8_t depth[DYN_MAXCODES];
    int bl_count[DYN_MAXBITS + 1];
    int nsym = 0, hn = 0, top, root;
    int overflow = 0;
    int i, bits, cnt, pos;
    unsigned long long kraft;

    memset(lens, 0, (size_t)n);
    memset(bl_count, 0, sizeof bl_count);
    for (i = 0; i < n; i++) {
        if (freq[i]) {
            f[nsym] = freq[i];
            syms[nsym] = i;
            nsym++;
        }
    }
    if (nsym == 0)
        return 0;
    if (nsym == 1) {
        lens[syms[0]] = 1;
        return 0;
    }

    for (i = 0; i < nsym; i++)
        dyn_huff_heap_push(heap, f, &hn, i);
    top = nsym;
    while (hn > 1) {
        int a = dyn_huff_heap_pop(heap, f, &hn);
        int b = dyn_huff_heap_pop(heap, f, &hn);
        f[top] = f[a] + f[b];
        parent[a] = top;
        parent[b] = top;
        dyn_huff_heap_push(heap, f, &hn, top);
        top++;
    }
    root = dyn_huff_heap_pop(heap, f, &hn);
    parent[root] = -1;

    for (i = 0; i < nsym; i++) {
        int d = 0, node = i;
        while (parent[node] >= 0) {
            node = parent[node];
            d++;
        }
        if (d > maxbits) {
            overflow++;
            d = maxbits;
        }
        depth[i] = (uint8_t)d;
        bl_count[d]++;
    }

    if (overflow) {
        while (overflow > 0) {
            bits = maxbits - 1;
            while (bl_count[bits] == 0)
                bits--;
            bl_count[bits]--;
            bl_count[bits + 1] += 2;
            bl_count[maxbits]--;
            overflow -= 2;
        }
        for (i = 0; i < nsym; i++)
            order[i] = i;
        for (i = 1; i < nsym; i++) {
            int v = order[i], j = i - 1;
            while (j >= 0 && (f[order[j]] < f[v] || (f[order[j]] == f[v] && order[j] > v))) {
                order[j + 1] = order[j];
                j--;
            }
            order[j + 1] = v;
        }
        pos = 0;
        for (bits = 1; bits <= maxbits; bits++)
            for (cnt = 0; cnt < bl_count[bits]; cnt++)
                lens[syms[order[pos++]]] = (uint8_t)bits;
    } else {
        for (i = 0; i < nsym; i++)
            lens[syms[i]] = depth[i];
    }

    kraft = 0;
    bits = 0;
    for (i = 0; i < n; i++) {
        if (lens[i]) {
            kraft += (unsigned long long)1 << (maxbits - lens[i]);
            if (lens[i] > bits)
                bits = lens[i];
        }
    }
    if (kraft != ((unsigned long long)1 << maxbits) || bits > maxbits)
        return -1;
    return 0;
}

static void dyn_huff_codes(const uint8_t* lens, int n, uint32_t* codes)
{
    int bl_count[DYN_MAXBITS + 1];
    unsigned next[DYN_MAXBITS + 1];
    unsigned code = 0;
    int i, bits;

    memset(bl_count, 0, sizeof bl_count);
    for (i = 0; i < n; i++)
        bl_count[lens[i]]++;
    for (bits = 1; bits <= DYN_MAXBITS; bits++) {
        code = (code + (unsigned)bl_count[bits - 1]) << 1;
        next[bits] = code;
    }
    for (i = 0; i < n; i++) {
        unsigned c, r = 0;
        int l = lens[i], b;
        if (l == 0) {
            codes[i] = 0;
            continue;
        }
        c = next[l]++;
        for (b = 0; b < l; b++) {
            r = (r << 1) | (c & 1u);
            c >>= 1;
        }
        codes[i] = (uint32_t)((r << 4) | (unsigned)l);
    }
}

typedef struct {
    uint8_t sym;
    uint8_t nbits;
    uint8_t extra;
} dyn_clrun_t;

static int dyn_cl_runs(const uint8_t* lens, int total, dyn_clrun_t* runs,
    int* pruns, size_t* clfreq)
{
    int i = 0, nr = 0;

    memset(clfreq, 0, DYN_CL_CODES * sizeof *clfreq);
    while (i < total) {
        int v = lens[i];
        int r = 1;
        int k;

        while (i + r < total && lens[i + r] == v)
            r++;
        i += r;

        if (r < 3) {
            for (k = 0; k < r; k++) {
                runs[nr].sym = (uint8_t)v;
                runs[nr].nbits = 0;
                runs[nr].extra = 0;
                nr++;
            }
            clfreq[v] += (size_t)r;
        } else if (v != 0) {
            runs[nr].sym = (uint8_t)v;
            runs[nr].nbits = 0;
            runs[nr].extra = 0;
            nr++;
            clfreq[v]++;
            r--;
            while (r >= 3) {
                k = r > 6 ? 6 : r;
                runs[nr].sym = 16;
                runs[nr].nbits = 2;
                runs[nr].extra = (uint8_t)(k - 3);
                nr++;
                clfreq[16]++;
                r -= k;
            }
            while (r > 0) {
                runs[nr].sym = (uint8_t)v;
                runs[nr].nbits = 0;
                runs[nr].extra = 0;
                nr++;
                clfreq[v]++;
                r--;
            }
        } else {
            while (r >= 11) {
                k = r > 138 ? 138 : r;
                runs[nr].sym = 18;
                runs[nr].nbits = 7;
                runs[nr].extra = (uint8_t)(k - 11);
                nr++;
                clfreq[18]++;
                r -= k;
            }
            if (r >= 3) {
                runs[nr].sym = 17;
                runs[nr].nbits = 3;
                runs[nr].extra = (uint8_t)(r - 3);
                nr++;
                clfreq[17]++;
                r = 0;
            }
            while (r > 0) {
                runs[nr].sym = 0;
                runs[nr].nbits = 0;
                runs[nr].extra = 0;
                nr++;
                clfreq[0]++;
                r--;
            }
        }
    }
    *pruns = nr;
    return nr;
}

DYN_INLINE unsigned dyn_fix_ll_len(unsigned sym)
{
    return sym < 144u ? 8u : sym < 256u ? 9u
        : sym < 280u                    ? 7u
                                        : 8u;
}

static int dyn_deflate_dynamic(const uint8_t* src, size_t len,
    uint8_t* dst, size_t cap, dyn_comp_ctx_t* cx,
    size_t* pwritten)
{
    static const short order[19] = {
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
    };
    dyn_tokrec_t rec;
    dyn_bw_t w;
    dyn_clrun_t runs[DYN_MAXCODES];
    size_t clfreq[DYN_CL_CODES];
    unsigned long long dyn_bits, fix_bits;
    uint8_t lllen[DYN_MAXLCODES], dllen[DYN_MAXDCODES], cllen[DYN_CL_CODES];
    uint8_t clseq[DYN_MAXCODES];
    uint32_t llcodes[DYN_MAXLCODES], dcodes[DYN_MAXDCODES];
    uint32_t clcodes[DYN_CL_CODES];
    int hlit, hdist, hclen, nruns = 0;
    int i, t, rc;

    *pwritten = 0;
    if (len == 0 || len > (size_t)INT32_MAX)
        return DYN_DYN_RETRY_FIXED;

    memset(&rec, 0, sizeof rec);
    if (dyn_lz_parse(src, len, NULL, cx, &rec)) {
        free(rec.tok);
        return -1;
    }
    rec.lfreq[256]++;

    {
        int used = 0, first = -1;
        for (i = 0; i < DYN_MAXLCODES; i++)
            if (rec.lfreq[i]) {
                used++;
                if (first < 0)
                    first = i;
            }
        if (used < 2)
            rec.lfreq[first == 0 ? 1 : 0]++;
    }
    {
        int used = 0, first = -1;
        for (i = 0; i < DYN_MAXDCODES; i++)
            if (rec.dfreq[i]) {
                used++;
                if (first < 0)
                    first = i;
            }
        if (used == 0) {
            rec.dfreq[0]++;
            rec.dfreq[1]++;
        } else if (used == 1) {
            rec.dfreq[first == 0 ? 1 : 0]++;
        }
    }

    if (dyn_huff_lengths(rec.lfreq, DYN_MAXLCODES, DYN_MAXBITS, lllen) || dyn_huff_lengths(rec.dfreq, DYN_MAXDCODES, DYN_MAXBITS, dllen)) {
        free(rec.tok);
        return DYN_DYN_RETRY_FIXED;
    }

    hlit = DYN_MAXLCODES;
    while (hlit > 257 && lllen[hlit - 1] == 0)
        hlit--;
    hdist = DYN_MAXDCODES;
    while (hdist > 1 && dllen[hdist - 1] == 0)
        hdist--;
    memcpy(clseq, lllen, (size_t)hlit);
    memcpy(clseq + hlit, dllen, (size_t)hdist);

    dyn_cl_runs(clseq, hlit + hdist, runs, &nruns, clfreq);
    {
        int used = 0;
        for (i = 0; i < DYN_CL_CODES; i++)
            if (clfreq[i])
                used++;
        if (used < 2)
            clfreq[clfreq[0] ? 1 : 0]++;
    }
    if (dyn_huff_lengths(clfreq, DYN_CL_CODES, DYN_CL_MAXBITS, cllen)) {
        free(rec.tok);
        return DYN_DYN_RETRY_FIXED;
    }
    hclen = DYN_CL_CODES;
    while (hclen > 4 && cllen[order[hclen - 1]] == 0)
        hclen--;

    dyn_huff_codes(lllen, DYN_MAXLCODES, llcodes);
    dyn_huff_codes(dllen, DYN_MAXDCODES, dcodes);
    dyn_huff_codes(cllen, DYN_CL_CODES, clcodes);

    dyn_bits = 17 + 3 * (unsigned long long)hclen;
    for (i = 0; i < nruns; i++)
        dyn_bits += (unsigned long long)cllen[runs[i].sym] + runs[i].nbits;
    for (i = 0; i < DYN_MAXLCODES; i++)
        dyn_bits += (unsigned long long)rec.lfreq[i] * lllen[i];
    for (i = 0; i < DYN_MAXDCODES; i++)
        dyn_bits += (unsigned long long)rec.dfreq[i] * dllen[i];
    dyn_bits += rec.extbits;

    fix_bits = 3;
    for (i = 0; i < DYN_MAXLCODES; i++)
        fix_bits += (unsigned long long)rec.lfreq[i] * dyn_fix_ll_len((unsigned)i);
    for (i = 0; i < DYN_MAXDCODES; i++)
        fix_bits += (unsigned long long)rec.dfreq[i] * 5;
    fix_bits += rec.extbits;

    w.op = dst;
    w.oend = dst + cap;
    w.acc = 0;
    w.nbits = 0;

    if (dyn_bits >= fix_bits) {
        dyn_bw_add(&w, 3, 3);
        rc = 0;
        for (t = 0; t < (int)rec.n && rc == 0; t++) {
            unsigned a = rec.tok[2 * t], b = rec.tok[2 * t + 1];
            if (DYN_UNLIKELY(w.op >= w.oend)) {
                rc = 1;
                break;
            }
            if (b == DYN_TOK_LIT) {
                dyn_emit_ll(&w, a);
                dyn_bw_drain(&w);
            } else {
                dyn_emit_fixed_match(&w, a, b);
            }
        }
        if (rc == 0) {
            dyn_emit_ll(&w, 256);
            dyn_bw_drain(&w);
            if (w.nbits > 0)
                *w.op++ = (uint8_t)(w.acc & 0xffu);
            if (w.op >= w.oend)
                rc = 1;
        }
        free(rec.tok);
        *pwritten = (size_t)(w.op - dst);
        return rc;
    }

    {
        unsigned long long hdr_bits = 17 + 3 * (unsigned long long)hclen;
        for (i = 0; i < nruns; i++)
            hdr_bits += (unsigned long long)cllen[runs[i].sym] + runs[i].nbits;
        if (hdr_bits + 64 > (unsigned long long)cap * 8) {
            free(rec.tok);
            return DYN_DYN_RETRY_FIXED;
        }
    }
    dyn_bw_add(&w, 5, 3);
    dyn_bw_add(&w, (unsigned)(hlit - 257), 5);
    dyn_bw_add(&w, (unsigned)(hdist - 1), 5);
    dyn_bw_add(&w, (unsigned)(hclen - 4), 4);
    dyn_bw_drain(&w);
    for (i = 0; i < hclen; i++) {
        dyn_bw_add(&w, cllen[order[i]], 3);
        dyn_bw_drain(&w);
    }
    for (i = 0; i < nruns; i++) {
        uint32_t c = clcodes[runs[i].sym];
        dyn_bw_add(&w, c >> 4, (int)(c & 15u));
        if (runs[i].nbits)
            dyn_bw_add(&w, runs[i].extra, runs[i].nbits);
        dyn_bw_drain(&w);
    }

    rc = 0;
    for (t = 0; t < (int)rec.n && rc == 0; t++) {
        unsigned a = rec.tok[2 * t], b = rec.tok[2 * t + 1];
        uint32_t c;

        if (DYN_UNLIKELY(w.op >= w.oend)) {
            rc = 1;
            break;
        }
        if (b == DYN_TOK_LIT) {
            c = llcodes[a];
            dyn_bw_add(&w, c >> 4, (int)(c & 15u));
            dyn_bw_drain(&w);
        } else {
            unsigned li = dyn_len_code[a - 3u];
            unsigned di = dyn_dist_index(b);

            c = llcodes[257u + li];
            dyn_bw_add(&w, c >> 4, (int)(c & 15u));
            if (dyn_lext[li])
                dyn_bw_add(&w, a - (unsigned)dyn_lens[li], dyn_lext[li]);
            dyn_bw_drain(&w);
            c = dcodes[di];
            dyn_bw_add(&w, c >> 4, (int)(c & 15u));
            if (dyn_dext[di])
                dyn_bw_add(&w, b - (unsigned)dyn_dists[di], dyn_dext[di]);
            dyn_bw_drain(&w);
        }
    }
    if (rc == 0) {
        uint32_t c = llcodes[256];
        dyn_bw_add(&w, c >> 4, (int)(c & 15u));
        dyn_bw_drain(&w);
        if (w.nbits > 0)
            *w.op++ = (uint8_t)(w.acc & 0xffu);
        if (w.op >= w.oend)
            rc = 1;
    }
    free(rec.tok);
    *pwritten = (size_t)(w.op - dst);
    return rc;
}

int dyn_gzip_build_ctx(const uint8_t* src, size_t src_len, int level,
    dyn_comp_ctx_t* cx, uint8_t** pout, size_t* pout_len)
{
    size_t nblocks, stored_sz, body_len, total;
    uint32_t crc, isize;
    uint8_t *out, *p, *shrunk;
    int rc;

    nblocks = src_len ? (src_len + DYN_STORED_MAX - 1) / DYN_STORED_MAX : 1;
    stored_sz = src_len ? nblocks * 5 + src_len : 5;

    if (stored_sz > SIZE_MAX - 34)
        return -1;
    out = (uint8_t*)malloc(10 + stored_sz + 8 + 16);
    if (!out)
        return -1;
    p = out;

    *p++ = 0x1f;
    *p++ = 0x8b;
    *p++ = 0x08;
    *p++ = 0x00;
    *p++ = 0x00;
    *p++ = 0x00;
    *p++ = 0x00;
    *p++ = 0x00;
    *p++ = 0x00;
    *p++ = 0xff;

    if (level >= DYN_GZIP_DYN_LEVEL) {
        rc = dyn_deflate_dynamic(src, src_len, out + 10, stored_sz, cx,
            &body_len);
        if (rc == DYN_DYN_RETRY_FIXED)
            rc = dyn_deflate_fixed(src, src_len, out + 10, stored_sz, cx,
                &body_len);
    } else {
        rc = dyn_deflate_fixed(src, src_len, out + 10, stored_sz, cx,
            &body_len);
    }
    if (rc != 0) {
        body_len = dyn_deflate_stored(src, src_len, out + 10);
    }

    total = 10 + body_len + 8;
    p = out + 10 + body_len;

    crc = dyn_crc32(src, src_len);
    isize = (uint32_t)(src_len & 0xffffffffu);
    *p++ = (uint8_t)(crc & 0xff);
    *p++ = (uint8_t)((crc >> 8) & 0xff);
    *p++ = (uint8_t)((crc >> 16) & 0xff);
    *p++ = (uint8_t)((crc >> 24) & 0xff);
    *p++ = (uint8_t)(isize & 0xff);
    *p++ = (uint8_t)((isize >> 8) & 0xff);
    *p++ = (uint8_t)((isize >> 16) & 0xff);
    *p++ = (uint8_t)((isize >> 24) & 0xff);

    shrunk = (uint8_t*)realloc(out, total);
    *pout = shrunk ? shrunk : out;
    *pout_len = total;
    return 0;
}

static int gunzip_member(const uint8_t* src, size_t len, dyn_outbuf_t* o,
    size_t* pused)
{
    size_t start, pos = 10, consumed = 0;
    uint8_t flg;
    uint32_t crc, isize;

    *pused = 0;
    if (len < 18)
        return -1;
    if (src[0] != 0x1f || src[1] != 0x8b || src[2] != 0x08)
        return -1;
    flg = src[3];
    if (flg & 0xe0)
        return -1;

    if (flg & 0x04) {
        size_t xlen;
        if (pos + 2 > len)
            return -1;
        xlen = (size_t)src[pos] | ((size_t)src[pos + 1] << 8);
        pos += 2;
        if (xlen > len - pos)
            return -1;
        pos += xlen;
    }
    if (flg & 0x08) {
        while (pos < len && src[pos] != 0)
            pos++;
        if (pos >= len)
            return -1;
        pos++;
    }
    if (flg & 0x10) {
        while (pos < len && src[pos] != 0)
            pos++;
        if (pos >= len)
            return -1;
        pos++;
    }
    if (flg & 0x02) {
        uint32_t hc;
        if (pos + 2 > len)
            return -1;
        hc = dyn_crc32(src, pos) & 0xffffu;
        if (((uint32_t)src[pos] | ((uint32_t)src[pos + 1] << 8)) != hc)
            return -1;
        pos += 2;
    }

    if (pos + 8 > len)
        return -1;

    {
        size_t hint = (uint32_t)src[len - 4] | ((uint32_t)src[len - 3] << 8) | ((uint32_t)src[len - 2] << 16) | ((uint32_t)src[len - 1] << 24);
        size_t ceiling = len - 8 - pos;
        size_t lim = dyn_ob_limit(o);
        if (ceiling > lim / 1032)
            ceiling = lim;
        else
            ceiling = ceiling * 1032 + 4096;
        if (hint > ceiling)
            hint = ceiling;
        if (hint > lim)
            hint = lim;
        if (hint && dyn_ob_reserve(o, o->len + hint + DYN_OSLACK) < 0)
            return -1;
    }

    start = o->len;
    if (dyn_inflate(src + pos, len - pos, o, &consumed) < 0)
        return -1;
    if (consumed + 8 > len - pos)
        return -1;

    crc = (uint32_t)src[pos + consumed] | ((uint32_t)src[pos + consumed + 1] << 8) | ((uint32_t)src[pos + consumed + 2] << 16) | ((uint32_t)src[pos + consumed + 3] << 24);
    isize = (uint32_t)src[pos + consumed + 4] | ((uint32_t)src[pos + consumed + 5] << 8) | ((uint32_t)src[pos + consumed + 6] << 16) | ((uint32_t)src[pos + consumed + 7] << 24);
    if (dyn_crc32(o->buf + start, o->len - start) != crc)
        return -1;
    if ((uint32_t)((o->len - start) & 0xffffffffu) != isize)
        return -1;
    *pused = pos + consumed + 8;
    return 0;
}

static int dyn_gunzip_decode_unbounded(const uint8_t* src, size_t len,
    dyn_outbuf_t* o)
{
    size_t off = 0;
    int members = 0;

    for (;;) {
        size_t rem = len - off, used = 0;
        const uint8_t* p = src + off;
        size_t i;

        if (rem == 0)
            return members ? 0 : -1;
        if (rem >= 3 && p[0] == 0x1f && p[1] == 0x8b && p[2] == 0x08) {
            if (gunzip_member(src + off, rem, o, &used) < 0 || used == 0)
                return -1;
            off += used;
            members++;
            continue;
        }
        for (i = 0; i < rem; i++)
            if (p[i])
                return -1;
        return members ? 0 : -1;
    }
}

#define DYN_LZ4_BOUND(n) ((n) + (n) / 255 + 16)

static int dyn_lz4_block_compress(const uint8_t* src, size_t len,
    const uint8_t* dict, size_t dict_len,
    int chain, dyn_comp_ctx_t* cx, dyn_outbuf_t* o)
{
    dyn_scratch_t sc;
    int32_t* head;
    uint16_t* prev;
    uint32_t base;
    int shift, use_chain;
    size_t i, anchor, total;
    const uint8_t* win;
    uint8_t* op;
    uint8_t* tmp = NULL;
    int rc = -1;

    if (dict_len > DYN_LZ4_MAX_DIST) {
        dict = dict + (dict_len - DYN_LZ4_MAX_DIST);
        dict_len = DYN_LZ4_MAX_DIST;
    }
    total = dict_len + len;
    if (total > (size_t)INT32_MAX)
        return -1;

    if (dict_len) {
        tmp = (uint8_t*)malloc(total ? total : 1);
        if (!tmp)
            return -1;
        memcpy(tmp, dict, dict_len);
        memcpy(tmp + dict_len, src, len);
        win = tmp;
    } else {
        win = src;
    }

    if (dyn_ob_ensure(o, DYN_LZ4_BOUND(len)))
        goto done_nofree;
    if (dyn_scratch_get(cx, DYN_LZ4_HASH_BITS, total, &sc, chain > 1))
        goto done_nofree;
    head = sc.head;
    prev = sc.prev;
    base = sc.base;
    shift = sc.shift;
    use_chain = chain > 1;

    op = o->buf + o->len;

    if (dict_len) {
        for (i = 0; i + DYN_LZ4_MIN_MATCH <= dict_len; i++) {
            uint32_t h = dyn_lz4_hash_at(win + i, shift);
            int32_t pv = head[h];
            if (use_chain) {
                size_t g = (pv >= (int32_t)base)
                    ? i - (size_t)(pv - (int32_t)base)
                    : 0;
                prev[i & DYN_PREV_MASK] = (uint16_t)(g <= 65535 ? g : 0);
            }
            head[h] = (int32_t)(i + base);
        }
    }

    i = dict_len;
    anchor = dict_len;
    while (i + DYN_LZ4_MF_LIMIT <= total) {
        size_t best_len = DYN_LZ4_MIN_MATCH - 1, best_pos = 0;
        uint32_t h = dyn_lz4_hash_at(win + i, shift);
        int32_t hv = head[h];
        size_t max_len = total - i - DYN_LZ4_LAST_LITERALS;

        if (hv >= (int32_t)base) {
            const uint8_t* a = win + i;
            size_t cur = (size_t)(hv - (int32_t)base);
            size_t dist = i - cur;
            int walk = chain;
            while (dist <= DYN_LZ4_MAX_DIST && dist != 0) {
                const uint8_t* b = win + cur;
                uint16_t g = use_chain ? prev[cur & DYN_PREV_MASK] : 0;
                if (b[best_len] == a[best_len] && b[best_len - 1] == a[best_len - 1]) {
                    size_t ml = dyn_match_len(a, b, max_len);
                    if (ml > best_len) {
                        best_len = ml;
                        best_pos = cur;
                        if (ml >= max_len)
                            break;
                    }
                }
                if (!use_chain || g == 0 || --walk <= 0)
                    break;
                cur -= g;
                dist += g;
            }
            if (use_chain) {
                size_t g = i - (size_t)(hv - (int32_t)base);
                prev[i & DYN_PREV_MASK] = (uint16_t)(g <= 65535 ? g : 0);
            }
        } else if (use_chain) {
            prev[i & DYN_PREV_MASK] = 0;
        }
        head[h] = (int32_t)(i + base);

        if (best_len >= DYN_LZ4_MIN_MATCH) {
            size_t k, end = i + best_len;
            size_t lit_len = i - anchor;
            size_t ml = best_len - DYN_LZ4_MIN_MATCH;
            unsigned offset = (unsigned)(i - best_pos);

            *op++ = (uint8_t)(((lit_len >= 15 ? 15u : (unsigned)lit_len) << 4) | (ml >= 15 ? 15u : (unsigned)ml));
            if (lit_len >= 15) {
                size_t r = lit_len - 15;
                while (r >= 255) {
                    *op++ = 255;
                    r -= 255;
                }
                *op++ = (uint8_t)r;
            }
            if (lit_len) {
                memcpy(op, win + anchor, lit_len);
                op += lit_len;
            }
            *op++ = (uint8_t)(offset & 0xff);
            *op++ = (uint8_t)(offset >> 8);
            if (ml >= 15) {
                size_t r = ml - 15;
                while (r >= 255) {
                    *op++ = 255;
                    r -= 255;
                }
                *op++ = (uint8_t)r;
            }

            if (use_chain) {
                int32_t bs = (int32_t)base;
                for (k = i + 1; k < end && k + DYN_LZ4_MIN_MATCH <= total; k++) {
                    uint32_t hh = dyn_lz4_hash_at(win + k, shift);
                    int32_t pv = head[hh];
                    int32_t cur = (int32_t)(k + base);
                    uint32_t g = (uint32_t)(cur - pv);
                    prev[k & DYN_PREV_MASK] = (uint16_t)((pv >= bs && g <= 65535) ? g : 0);
                    head[hh] = cur;
                }
            } else {
                for (k = i + 1; k < end && k + DYN_LZ4_MIN_MATCH <= total; k++)
                    head[dyn_lz4_hash_at(win + k, shift)] = (int32_t)(k + base);
            }
            i = end;
            anchor = end;
        } else {
            i++;
        }
    }
    {
        size_t lit_len = total - anchor;
        *op++ = (uint8_t)((lit_len >= 15 ? 15u : (unsigned)lit_len) << 4);
        if (lit_len >= 15) {
            size_t r = lit_len - 15;
            while (r >= 255) {
                *op++ = 255;
                r -= 255;
            }
            *op++ = (uint8_t)r;
        }
        if (lit_len) {
            memcpy(op, win + anchor, lit_len);
            op += lit_len;
        }
    }
    o->len = (size_t)(op - o->buf);
    rc = 0;
    dyn_scratch_put(&sc);
done_nofree:
    free(tmp);
    return rc;
}

static int dyn_lz4_block_decompress(const uint8_t* src, size_t len,
    const uint8_t* dict, size_t dict_len,
    dyn_outbuf_t* o)
{
    size_t p = 0, produced = 0;
    size_t start = o->len;

    if (len && dyn_ob_reserve(o, o->len + len * 3 + DYN_OSLACK) < 0) {
        if (dyn_ob_ensure(o, len))
            return -1;
    }

    while (p < len) {
        uint8_t token = src[p++];
        size_t lit = token >> 4, ml = token & 15, off, k;

        if (DYN_UNLIKELY(lit == 15)) {
            for (;;) {
                if (p >= len)
                    return -1;
                if (lit > SIZE_MAX - 255)
                    return -1;
                lit += src[p];
                if (src[p++] != 255)
                    break;
            }
        }
        if (lit > len - p)
            return -1;
        if (dyn_ob_ensure(o, lit + 8))
            return -1;
        if (lit) {
            memcpy(o->buf + o->len, src + p, lit);
            o->len += lit;
            produced += lit;
            p += lit;
        }
        if (p == len)
            break;
        if (p + 2 > len)
            return -1;
        off = (size_t)src[p] | ((size_t)src[p + 1] << 8);
        p += 2;
        if (off == 0 || off > produced + dict_len)
            return -1;
        if (DYN_UNLIKELY(ml == 15)) {
            for (;;) {
                if (p >= len)
                    return -1;
                if (ml > SIZE_MAX - 255)
                    return -1;
                ml += src[p];
                if (src[p++] != 255)
                    break;
            }
        }
        ml += DYN_LZ4_MIN_MATCH;
        if (dyn_ob_ensure(o, ml + 8))
            return -1;
        if (DYN_LIKELY(off <= produced)) {
            dyn_copy_match(o->buf + o->len, off, ml);
            o->len += ml;
        } else {
            for (k = 0; k < ml; k++) {
                size_t back = produced + k;
                uint8_t b;
                if (off > back) {
                    size_t d = off - back;
                    b = dict[dict_len - d];
                } else {
                    b = o->buf[start + back - off];
                }
                o->buf[o->len++] = b;
            }
        }
        produced += ml;
    }
    return 0;
}

#define DYN_LZ4F_MAGIC 0x184D2204u
#define DYN_LZ4F_BLOCK_MAX 4194304u

DYN_INLINE void dyn_put32le(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
    p[3] = (uint8_t)((v >> 24) & 0xff);
}

DYN_INLINE uint32_t dyn_get32le(const uint8_t* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int dyn_lz4_chain_for(int level)
{
    return level <= 1 ? 1 : (level >= 12 ? 4096 : level * 16);
}

int dyn_lz4_compress(const uint8_t* src, size_t len,
    const uint8_t* dict, size_t dict_len, int level,
    dyn_comp_ctx_t* cx, uint8_t** pout, size_t* pout_len)
{
    dyn_outbuf_t o = { NULL, 0, 0 };

    if (dyn_lz4_block_compress(src, len, dict, dict_len,
            dyn_lz4_chain_for(level), cx, &o)
        < 0) {
        free(o.buf);
        return -1;
    }
    *pout = o.buf;
    *pout_len = o.len;
    return 0;
}

int dyn_lz4_decompress(const uint8_t* src, size_t len,
    const uint8_t* dict, size_t dict_len, dyn_outbuf_t* o)
{
    return dyn_ob_within(o,
        dyn_lz4_block_decompress(src, len, dict, dict_len, o));
}

int dyn_lz4_frame_build(const uint8_t* src, size_t len, int level,
    int content_checksum, dyn_comp_ctx_t* cx,
    uint8_t** pout, size_t* pout_len)
{
    dyn_outbuf_t o = { NULL, 0, 0 };
    uint8_t desc[3];
    size_t off = 0;
    int chain = dyn_lz4_chain_for(level);

    desc[0] = (uint8_t)(0x40 | 0x20 | (content_checksum ? 0x04 : 0));
    desc[1] = 0x70;
    desc[2] = (uint8_t)((dyn_xxh32(desc, 2, 0) >> 8) & 0xff);

    {
        size_t nblk = len ? (len + DYN_LZ4F_BLOCK_MAX - 1) / DYN_LZ4F_BLOCK_MAX : 1;
        size_t want = 7 + nblk * 4 + DYN_LZ4_BOUND(len) + 8;
        if (dyn_ob_reserve(&o, want) < 0 && dyn_ob_ensure(&o, 7) < 0)
            goto fail;
    }
    if (dyn_ob_ensure(&o, 7))
        goto fail;
    dyn_put32le(o.buf + o.len, DYN_LZ4F_MAGIC);
    o.len += 4;
    memcpy(o.buf + o.len, desc, 3);
    o.len += 3;

    while (off < len || len == 0) {
        size_t chunk = len - off;
        size_t hdr, blk_len;

        if (chunk > DYN_LZ4F_BLOCK_MAX)
            chunk = DYN_LZ4F_BLOCK_MAX;
        hdr = o.len;
        if (dyn_ob_ensure(&o, 4))
            goto fail;
        o.len += 4;
        if (dyn_lz4_block_compress(src + off, chunk, NULL, 0, chain, cx, &o) < 0)
            goto fail;
        blk_len = o.len - hdr - 4;

        if (blk_len >= chunk) {
            o.len = hdr + 4;
            if (dyn_ob_ensure(&o, chunk))
                goto fail;
            memcpy(o.buf + o.len, src + off, chunk);
            o.len += chunk;
            dyn_put32le(o.buf + hdr, (uint32_t)chunk | 0x80000000u);
        } else {
            dyn_put32le(o.buf + hdr, (uint32_t)blk_len);
        }
        off += chunk;
        if (len == 0)
            break;
    }

    if (dyn_ob_ensure(&o, 4))
        goto fail;
    dyn_put32le(o.buf + o.len, 0);
    o.len += 4;
    if (content_checksum) {
        if (dyn_ob_ensure(&o, 4))
            goto fail;
        dyn_put32le(o.buf + o.len, dyn_xxh32(src, len, 0));
        o.len += 4;
    }
    *pout = o.buf;
    *pout_len = o.len;
    return 0;
fail:
    free(o.buf);
    return -1;
}

static int dyn_lz4_frame_decode_unbounded(const uint8_t* src, size_t len,
    dyn_outbuf_t* o)
{
    size_t p = 0;
    uint8_t flg, bd;
    uint32_t max_block;
    uint64_t content_size = 0;
    int has_checksum, has_content_size, has_dict_id, block_checksum;

    if (len < 4)
        return DYN_LZ4F_ERR_TRUNC;
    if (dyn_get32le(src) != DYN_LZ4F_MAGIC)
        return DYN_LZ4F_ERR_MAGIC;
    if (len < 7)
        return DYN_LZ4F_ERR_TRUNC;
    p = 4;
    flg = src[p++];
    bd = src[p++];
    {
        unsigned bdbits = (bd >> 4) & 7u;
        if (bdbits < 4 || bdbits > 7)
            return DYN_LZ4F_ERR_BLOCK_MAX;
        max_block = 1u << (2 * bdbits + 8);
        if (max_block > DYN_LZ4F_BLOCK_MAX)
            return DYN_LZ4F_ERR_BLOCK_MAX;
    }
    if ((flg & 0xc0) != 0x40)
        return DYN_LZ4F_ERR_VERSION;
    if (!(flg & 0x20))
        return DYN_LZ4F_ERR_LINKED;
    has_content_size = (flg & 0x08) != 0;
    has_checksum = (flg & 0x04) != 0;
    has_dict_id = (flg & 0x01) != 0;
    block_checksum = (flg & 0x10) != 0;
    if (has_content_size) {
        if (len - p < 8)
            return DYN_LZ4F_ERR_TRUNC;
        content_size = (uint64_t)dyn_get32le(src + p) | ((uint64_t)dyn_get32le(src + p + 4) << 32);
        p += 8;
    }
    if (has_dict_id) {
        if (len - p < 4)
            return DYN_LZ4F_ERR_TRUNC;
        p += 4;
    }
    if (p >= len)
        return DYN_LZ4F_ERR_TRUNC;
    if (src[p] != (uint8_t)((dyn_xxh32(src + 4, p - 4, 0) >> 8) & 0xff))
        return DYN_LZ4F_ERR_HDR_SUM;
    p++;

    for (;;) {
        uint32_t bs;
        size_t bl;
        if (len - p < 4)
            return DYN_LZ4F_ERR_TRUNC;
        bs = dyn_get32le(src + p);
        p += 4;
        if (bs == 0)
            break;
        bl = (size_t)(bs & 0x7fffffffu);
        if (bl > len - p)
            return DYN_LZ4F_ERR_TRUNC;
        if (bl > max_block)
            return DYN_LZ4F_ERR_BLOCK_CAP;
        if (bs & 0x80000000u) {
            if (dyn_ob_ensure(o, bl))
                return DYN_LZ4F_ERR_OOM;
            memcpy(o->buf + o->len, src + p, bl);
            o->len += bl;
        } else if (dyn_lz4_block_decompress(src + p, bl, NULL, 0, o) < 0) {
            return DYN_LZ4F_ERR_BLOCK;
        }
        if (block_checksum) {
            if (len - p - bl < 4)
                return DYN_LZ4F_ERR_TRUNC;
            if (dyn_get32le(src + p + bl) != dyn_xxh32(src + p, bl, 0))
                return DYN_LZ4F_ERR_BLOCK_SUM;
        }
        p += bl + (block_checksum ? 4 : 0);
    }
    if (has_checksum) {
        if (len - p < 4)
            return DYN_LZ4F_ERR_TRUNC;
        if (dyn_get32le(src + p) != dyn_xxh32(o->buf, o->len, 0))
            return DYN_LZ4F_ERR_CONT_SUM;
    }
    if (has_content_size && content_size != (uint64_t)o->len)
        return DYN_LZ4F_ERR_CONT_SIZE;
    if (has_checksum)
        p += 4;
    if (p != len)
        return DYN_LZ4F_ERR_TRAILING;
    return DYN_LZ4F_OK;
}

int dyn_lz4_frame_decode(const uint8_t* src, size_t len, dyn_outbuf_t* o)
{
    int rc = dyn_lz4_frame_decode_unbounded(src, len, o);
    if (rc == DYN_LZ4F_OK && dyn_ob_within(o, 0) < 0)
        return DYN_LZ4F_ERR_BLOCK;
    return rc;
}

const char* dyn_lz4_frame_reason(int rc)
{
    switch (rc) {
    case DYN_LZ4F_OK:
        return "ok";
    case DYN_LZ4F_ERR_TRAILING:
        return "bytes follow the end of the LZ4 frame";
    case DYN_LZ4F_ERR_TRUNC:
        return "truncated LZ4 frame";
    case DYN_LZ4F_ERR_MAGIC:
        return "not an LZ4 frame (bad magic)";
    case DYN_LZ4F_ERR_VERSION:
        return "unsupported LZ4 frame version";
    case DYN_LZ4F_ERR_BLOCK_MAX:
        return "invalid LZ4 block-max size";
    case DYN_LZ4F_ERR_LINKED:
        return "linked LZ4 blocks are not supported";
    case DYN_LZ4F_ERR_HDR_SUM:
        return "LZ4 frame descriptor checksum mismatch";
    case DYN_LZ4F_ERR_BLOCK_CAP:
        return "LZ4 block exceeds the declared block-max size";
    case DYN_LZ4F_ERR_BLOCK:
        return "corrupt LZ4 block";
    case DYN_LZ4F_ERR_BLOCK_SUM:
        return "LZ4 block checksum mismatch";
    case DYN_LZ4F_ERR_CONT_SUM:
        return "LZ4 content checksum mismatch";
    case DYN_LZ4F_ERR_CONT_SIZE:
        return "LZ4 frame declared content size mismatch";
    case DYN_LZ4F_ERR_OOM:
        return "out of memory";
    default:
        return "invalid LZ4 frame";
    }
}

int dyn_gzip_build(const uint8_t* src, size_t src_len,
    uint8_t** pout, size_t* pout_len)
{
    return dyn_gzip_build_ctx(src, src_len, 1, NULL, pout, pout_len);
}

int dyn_raw_deflate(const uint8_t* src, size_t src_len, int level,
    dyn_comp_ctx_t* cx, uint8_t** pout, size_t* pout_len)
{
    size_t nblocks, stored_sz, body_len;
    uint8_t *out, *shrunk;
    int rc;

    nblocks = src_len ? (src_len + DYN_STORED_MAX - 1) / DYN_STORED_MAX : 1;
    stored_sz = src_len ? nblocks * 5 + src_len : 5;
    if (stored_sz > SIZE_MAX - 16)
        return -1;
    out = (uint8_t*)malloc(stored_sz + 16);
    if (!out)
        return -1;
    if (level >= DYN_GZIP_DYN_LEVEL) {
        rc = dyn_deflate_dynamic(src, src_len, out, stored_sz, cx, &body_len);
        if (rc == DYN_DYN_RETRY_FIXED)
            rc = dyn_deflate_fixed(src, src_len, out, stored_sz, cx, &body_len);
    } else {
        rc = dyn_deflate_fixed(src, src_len, out, stored_sz, cx, &body_len);
    }
    if (rc != 0)
        body_len = dyn_deflate_stored(src, src_len, out);
    shrunk = (uint8_t*)realloc(out, body_len ? body_len : 1);
    *pout = shrunk ? shrunk : out;
    *pout_len = body_len;
    return 0;
}

static int dyn_raw_inflate_unbounded(const uint8_t* src, size_t len,
    dyn_outbuf_t* o)
{
    size_t consumed = 0;

    if (!len)
        return -1;
    if (dyn_inflate(src, len, o, &consumed) < 0)
        return -1;
    if (consumed != len)
        return -1;
    return 0;
}

int dyn_raw_inflate(const uint8_t* src, size_t len, dyn_outbuf_t* o)
{
    return dyn_ob_within(o, dyn_raw_inflate_unbounded(src, len, o));
}

int dyn_gunzip_decode(const uint8_t* src, size_t len, dyn_outbuf_t* o)
{
    return dyn_ob_within(o, dyn_gunzip_decode_unbounded(src, len, o));
}
