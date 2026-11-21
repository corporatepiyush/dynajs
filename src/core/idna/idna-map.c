#include "idna-tables.h"

int dyn_idna_map_cp(uint32_t cp, int transitional, int drop_disallowed,
    dyn_cp_t* out)
{
    int i, st;
    uint32_t v, t0, t1;
    if (cp < 0x80) {
        if (cp >= 'A' && cp <= 'Z')
            return dyn_cp_put(out, cp + 0x20);
        return dyn_cp_put(out, cp);
    }
    if (cp == 0x1E9E) {
        if (transitional)
            return dyn_cp_put(out, 's') | dyn_cp_put(out, 's');
        return dyn_cp_put(out, 0x00DF);
    }
    {
        int lo = 0, hi = (int)(countof(idna_map_delta)) - 1;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            if (cp < idna_map_delta[mid][0])
                hi = mid - 1;
            else if (cp > idna_map_delta[mid][1])
                lo = mid + 1;
            else {
                int32_t d = (int32_t)idna_map_delta[mid][2];
                return dyn_cp_put(out, cp + d);
            }
        }
    }
    {
        int lo = 0, hi = (int)(countof(idna_map_multi)) - 1;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            if (cp < idna_map_multi[mid][0])
                hi = mid - 1;
            else if (cp > idna_map_multi[mid][0])
                lo = mid + 1;
            else {
                uint32_t off = idna_map_multi[mid][1], len = idna_map_multi[mid][2];
                for (i = 0; i < (int)len; i++) {
                    if (dyn_cp_put(out, idna_map_multi_pool[off + i]))
                        return -1;
                }
                return 0;
            }
        }
    }
    st = IDNA_ST_VALID;
    {
        int ri = dyn_tab3(idna_status_ranges, countof(idna_status_ranges), cp);
        if (ri >= 0)
            st = idna_status_ranges[ri][2];
    }
    switch (st) {
    case IDNA_ST_IGNORED:
        return 0;
    case IDNA_ST_DEVIATION:
        if (!transitional)
            return dyn_cp_put(out, cp);
        if (cp == 0x00DF) {
            if (dyn_cp_put(out, 's'))
                return -1;
            return dyn_cp_put(out, 's');
        }
        if (cp == 0x03C2)
            return dyn_cp_put(out, 0x03C3);
        if (cp == 0x200C || cp == 0x200D)
            return 0;
        return dyn_cp_put(out, cp);
    case IDNA_ST_DISALLOWED:
        return drop_disallowed ? 1 : dyn_cp_put(out, cp);
    default:
        v = cp;
        t0 = t1 = 0;
        (void)v;
        (void)t0;
        (void)t1;
        return dyn_cp_put(out, cp);
    }
}
