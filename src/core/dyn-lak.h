#ifndef DYN_LAK_H
#define DYN_LAK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DYN_ND_MAXDIM 8

typedef enum {
    DYN_ND_F64 = 0,
    DYN_ND_F32 = 1,
    DYN_ND_F16 = 2,
    DYN_ND_BF16 = 3,
    DYN_ND_I32 = 4,
    DYN_ND_I16 = 5,
    DYN_ND_I8 = 6,
    DYN_ND_U8 = 7,
    DYN_ND_BOOL = 8,
    DYN_ND_NTYPES = 9
} dyn_nd_dtype_t;

extern const uint8_t dyn_nd_esize_tab[DYN_ND_NTYPES];

static inline size_t dyn_nd_esize(int dtype)
{
    return (unsigned)dtype < DYN_ND_NTYPES ? dyn_nd_esize_tab[dtype] : 0;
}

static inline int dyn_nd_is_float(int dtype)
{
    return dtype == DYN_ND_F64 || dtype == DYN_ND_F32;
}

typedef struct {
    void* base;
    int64_t base_len;
    int64_t offset;
    int64_t size;
    int32_t dtype;
    int32_t ndim;
    int64_t shape[DYN_ND_MAXDIM];
    int64_t strides[DYN_ND_MAXDIM];
} dyn_nd_t;

float dyn_nd_f16_to_f32(uint16_t h);
uint16_t dyn_nd_f32_to_f16(float f);
float dyn_nd_bf16_to_f32(uint16_t h);
uint16_t dyn_nd_f32_to_bf16(float f);
uint16_t dyn_nd_f64_to_f16(double v);
uint16_t dyn_nd_f64_to_bf16(double v);

int dyn_nd_is_contig(const dyn_nd_t* a);
int dyn_nd_bounds_ok(const dyn_nd_t* a);
int64_t dyn_nd_shape_size(int ndim, const int64_t* shape);
void dyn_nd_contig_strides(int ndim, const int64_t* shape, int64_t* strides);
double dyn_nd_load(int dtype, const void* p);
void dyn_nd_store(int dtype, void* p, double v);
double dyn_nd_get(const dyn_nd_t* a, int64_t lin);
void dyn_nd_set(const dyn_nd_t* a, int64_t lin, double v);
int dyn_nd_copy(const dyn_nd_t* dst, const dyn_nd_t* src);
void dyn_nd_to_f64(const dyn_nd_t* src, double* dst);
void dyn_nd_from_f64(const dyn_nd_t* dst, const double* src);
int dyn_nd_overlap(const dyn_nd_t* a, const dyn_nd_t* b);


#ifdef __cplusplus
}
#endif

#endif
