#include "svg-raster.h"

int svg_f2i(float v, int lo, int hi, int nan_val) {
    if (!(v == v)) return nan_val;
    if (v <= (float)lo) return lo;
    if (v >= (float)hi) return hi;
    return (int)v;
}
