#ifndef DS_WORK_STEALING_DEQUE_H
#define DS_WORK_STEALING_DEQUE_H

#include <stddef.h>
#include <stdint.h>
#include "ds-common.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef DYN_DS_NO_THREADS
typedef struct dyn_wsdeque dyn_wsdeque_t;

dyn_wsdeque_t* dyn_wsdeque_new(size_t initial_capacity);

void dyn_wsdeque_free(dyn_wsdeque_t* q);

int dyn_wsdeque_push(dyn_wsdeque_t* q, uint64_t item);

int dyn_wsdeque_pop(dyn_wsdeque_t* q, uint64_t* item);

int dyn_wsdeque_steal(dyn_wsdeque_t* q, uint64_t* item);

size_t dyn_wsdeque_size_approx(const dyn_wsdeque_t* q);
#endif

#ifdef __cplusplus
}
#endif

#endif
