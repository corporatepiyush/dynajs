#ifndef DYN_SNAPPEND_H
#define DYN_SNAPPEND_H

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>

static inline __attribute__((format(printf, 4, 5))) size_t dyn_snappend(char* buf, size_t cap, size_t off, const char* fmt, ...)
{
    va_list ap;
    int n;
    if (!cap)
        return 0;
    if (off >= cap)
        return cap - 1;
    va_start(ap, fmt);
    n = vsnprintf(buf + off, cap - off, fmt, ap);
    va_end(ap);
    if (n < 0) {
        buf[off] = '\0';
        return off;
    }
    return (size_t)n < cap - off ? off + (size_t)n : cap - 1;
}

#endif
