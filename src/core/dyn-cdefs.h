#ifndef DYN_CDEFS_H
#define DYN_CDEFS_H



#ifndef DYN_UNCONST
#define DYN_UNCONST(p) ((void*)(uintptr_t)(p))
#endif

#ifndef DYN_NAN
#define DYN_NAN ((double)NAN)
#endif

#ifndef DYN_INFINITY
#define DYN_INFINITY ((double)INFINITY)
#endif

#endif
