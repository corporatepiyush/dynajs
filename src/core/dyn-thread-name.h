#ifndef DYN_THREAD_NAME_H
#define DYN_THREAD_NAME_H

#include <pthread.h>

static inline void dyn_thread_name(const char* name)
{
#if defined(__APPLE__)
    pthread_setname_np(name);
#elif defined(__linux__) && defined(_GNU_SOURCE)
    pthread_setname_np(pthread_self(), name);
#else
    (void)name;
#endif
}

#endif
