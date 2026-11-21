#ifndef DS_PARKING_LOT_H
#define DS_PARKING_LOT_H

#include <stddef.h>
#include <stdint.h>
#include "ds-common.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef DYN_DS_NO_THREADS
typedef int (*dyn_park_validate)(void* ud);

int dyn_park_wait(const void* addr, dyn_park_validate validate, void* ud, int64_t timeout_ns);

unsigned dyn_park_wake(const void* addr, unsigned count);

unsigned dyn_park_waiters(const void* addr);
#endif

#ifdef __cplusplus
}
#endif

#endif
