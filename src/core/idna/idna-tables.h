#ifndef DYN_IDNA_TABLES_H
#define DYN_IDNA_TABLES_H

#include "idna.h"

extern const uint32_t idna_status_ranges[758][3];
extern const uint32_t idna_map_delta[2961][3];
extern const uint32_t idna_map_multi[1273][3];
extern const uint32_t idna_map_multi_pool[2535];
extern const uint32_t idna_bidi[1287][3];
extern const uint32_t idna_jt[150][3];
extern const uint32_t idna_jt_transparent[196][2];
extern const uint32_t idna_virama[58][2];
extern const uint32_t idna_combining[321][2];

static inline int dyn_tab3(const uint32_t (*tab)[3], int n, uint32_t cp)
{
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (cp < tab[mid][0])
            hi = mid - 1;
        else if (cp > tab[mid][1])
            lo = mid + 1;
        else
            return mid;
    }
    return -1;
}

static inline int dyn_tab2(const uint32_t (*tab)[2], int n, uint32_t cp)
{
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (cp < tab[mid][0])
            hi = mid - 1;
        else if (cp > tab[mid][1])
            lo = mid + 1;
        else
            return mid;
    }
    return -1;
}

static inline int dyn_idna_status(uint32_t cp)
{
    int lo, hi, mid;
    if (cp < 0x80) {
        if (cp >= 'A' && cp <= 'Z')
            return IDNA_ST_MAPPED;
        return IDNA_ST_VALID;
    }
    lo = 0;
    hi = (int)(countof(idna_map_delta)) - 1;
    while (lo <= hi) {
        mid = (lo + hi) / 2;
        if (cp < idna_map_delta[mid][0])
            hi = mid - 1;
        else if (cp > idna_map_delta[mid][1])
            lo = mid + 1;
        else
            return IDNA_ST_MAPPED;
    }
    lo = 0;
    hi = (int)(countof(idna_map_multi)) - 1;
    while (lo <= hi) {
        mid = (lo + hi) / 2;
        if (cp < idna_map_multi[mid][0])
            hi = mid - 1;
        else if (cp > idna_map_multi[mid][0])
            lo = mid + 1;
        else
            return IDNA_ST_MAPPED;
    }
    {
        int i = dyn_tab3(idna_status_ranges, countof(idna_status_ranges), cp);
        if (i >= 0)
            return idna_status_ranges[i][2];
    }
    return IDNA_ST_VALID;
}

static inline int dyn_idna_jt(uint32_t cp)
{
    int i = dyn_tab3(idna_jt, countof(idna_jt), cp);
    if (i >= 0)
        return (int)idna_jt[i][2];
    if (dyn_tab2(idna_jt_transparent, countof(idna_jt_transparent), cp) >= 0)
        return 3;
    return 4;
}

static inline int dyn_idna_virama(uint32_t cp)
{
    return dyn_tab2(idna_virama, countof(idna_virama), cp) >= 0;
}

static inline int dyn_idna_combining(uint32_t cp)
{
    return dyn_tab2(idna_combining, countof(idna_combining), cp) >= 0;
}

static inline int dyn_idna_bidi_class(uint32_t cp)
{
    int i = dyn_tab3(idna_bidi, countof(idna_bidi), cp);
    return i >= 0 ? (int)idna_bidi[i][2] : 6;
}

#endif
