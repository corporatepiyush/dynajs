#ifndef DYN_STDIO_H
#define DYN_STDIO_H

#include <stddef.h>
#include <stdint.h>

#define DYN_STDIO_DIRECT 16384
#define DYN_STDIO_CAP 65536

void dyn_stdio_sync(void);
void dyn_stdio_sync_try(void);
void dyn_stdio_idle(void);
int dyn_stdio_write(int fd, const uint8_t* b, size_t n, size_t* done);
int dyn_stdio_flush(int fd);
int dyn_stdio_report_unreported(void);

#endif
