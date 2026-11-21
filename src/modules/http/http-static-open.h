#ifndef DYNAJS_HTTP_STATIC_OPEN_H
#define DYNAJS_HTTP_STATIC_OPEN_H

#include <sys/types.h>

typedef struct dyn_static_root {
    char* dir;
} dyn_static_root_t;

dyn_static_root_t* dyn_static_root_new(const char* dir);
void dyn_static_root_free(dyn_static_root_t* r);
int dyn_static_open(dyn_static_root_t* r, const char* rel, int* status);

#endif
