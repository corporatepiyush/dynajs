#include "httpk.h"

int httpk_req_line(dyn_sb_t* out, const char* method, const char* target)
{
    static const char hex[] = "0123456789ABCDEF";
    const unsigned char* p = (const unsigned char*)target;
    size_t ml = method ? strlen(method) : 0;
    if (!httpk_name_ok(method, ml))
        return -1;
    dyn_sb_put(out, method, ml);
    dyn_sb_putc(out, ' ');
    if (!p || !*p)
        dyn_sb_putc(out, '/');
    for (; p && *p; p++) {
        if (*p <= 0x20 || *p == 0x7f) {
            char e[3] = { '%', hex[*p >> 4], hex[*p & 15] };
            dyn_sb_put(out, e, 3);
        } else {
            dyn_sb_putc(out, (char)*p);
        }
    }
    dyn_sb_put(out, " HTTP/1.1\r\n", 11);
    return out->oom ? -1 : 0;
}

int httpk_req_header(dyn_sb_t* out, const char* name, size_t nn,
    const char* value, size_t vn)
{
    if (!httpk_name_ok(name, nn) || !httpk_value_ok(value, vn))
        return -1;
    dyn_sb_put(out, name, nn);
    dyn_sb_put(out, ": ", 2);
    dyn_sb_put(out, value, vn);
    dyn_sb_put(out, "\r\n", 2);
    return out->oom ? -1 : 0;
}
