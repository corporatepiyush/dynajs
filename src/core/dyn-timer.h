#ifndef DYN_TIMER_H
#define DYN_TIMER_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dyn_timers dyn_timers_t;
typedef void (*dyn_timer_cb)(void* arg);

typedef uint32_t dyn_timer_id;
#define DYN_TIMER_NONE ((dyn_timer_id)0)

uint64_t dyn_timer_now_ms(void);

dyn_timers_t* dyn_timers_new(void);
void dyn_timers_free(dyn_timers_t* t);

dyn_timer_id dyn_timer_add(dyn_timers_t* t, uint64_t now, uint64_t delay_ms,
    dyn_timer_cb cb, void* arg);

int dyn_timer_cancel(dyn_timers_t* t, dyn_timer_id id);

int dyn_timer_next_timeout(const dyn_timers_t* t, uint64_t now);

int dyn_timer_run(dyn_timers_t* t, uint64_t now);

size_t dyn_timers_count(const dyn_timers_t* t);

#ifdef __cplusplus
}
#endif

#endif
