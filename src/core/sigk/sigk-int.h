#ifndef DYN_SIGK_INT_H
#define DYN_SIGK_INT_H

#define _REENTRANT
#include <math.h>

#include "../dyn-sigk.h"
#include "../dyn-mathx.h"
#include "../dyn-cdefs.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

int dyn_sg_cfft(double* x, uint32_t n, int inverse);

#endif
