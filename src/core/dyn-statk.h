#ifndef DYN_STATK_H
#define DYN_STATK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    DYN_ST_OK = 0,
    DYN_ST_E_OOM = -1,
    DYN_ST_E_SIZE = -2,
    DYN_ST_E_NAN = -3,
    DYN_ST_E_DOMAIN = -4,
    DYN_ST_E_SHAPE = -5,
    DYN_ST_E_CANCELLED = -6
};

typedef enum {
    DYN_ST_TWO_SIDED = 0,
    DYN_ST_LESS = 1,
    DYN_ST_GREATER = 2
} dyn_st_alt_t;

typedef enum {
    DYN_ST_AUTO = 0,
    DYN_ST_EXACT = 1,
    DYN_ST_ASYMP = 2
} dyn_st_method_t;

typedef enum {
    DYN_ST_RANK_AVERAGE = 0,
    DYN_ST_RANK_MIN = 1,
    DYN_ST_RANK_MAX = 2,
    DYN_ST_RANK_DENSE = 3,
    DYN_ST_RANK_ORDINAL = 4
} dyn_st_rank_t;

typedef enum {
    DYN_ST_ADJ_BONFERRONI = 0,
    DYN_ST_ADJ_HOLM = 1,
    DYN_ST_ADJ_HOCHBERG = 2,
    DYN_ST_ADJ_BH = 3,
    DYN_ST_ADJ_BY = 4
} dyn_st_adjust_t;

typedef struct {
    double statistic, pvalue, df;
} dyn_st_result_t;

double dyn_st_norm_pdf(double x);
double dyn_st_norm_cdf(double x);
double dyn_st_norm_sf(double x);
double dyn_st_norm_ppf(double p);
double dyn_st_t_pdf(double x, double df);
double dyn_st_t_cdf(double x, double df);
double dyn_st_t_sf(double x, double df);
double dyn_st_t_ppf(double p, double df);
double dyn_st_chi2_pdf(double x, double k);
double dyn_st_chi2_cdf(double x, double k);
double dyn_st_chi2_sf(double x, double k);
double dyn_st_chi2_ppf(double p, double k);
double dyn_st_f_cdf(double x, double d1, double d2);
double dyn_st_f_sf(double x, double d1, double d2);
double dyn_st_f_ppf(double p, double d1, double d2);
double dyn_st_binom_pmf(double k, double n, double p);
double dyn_st_binom_cdf(double k, double n, double p);
double dyn_st_binom_sf(double k, double n, double p);
double dyn_st_poisson_pmf(double k, double mu);
double dyn_st_poisson_cdf(double k, double mu);
double dyn_st_poisson_sf(double k, double mu);
double dyn_st_kolmogorov_sf(double x);

int dyn_st_argsort(const double* x, uint32_t n, uint32_t* idx);
int dyn_st_rank(const double* x, uint32_t n, dyn_st_rank_t method, double* out, double* tie_term);

int dyn_st_ttest_1samp(const double* a, uint32_t n, double mu, dyn_st_alt_t alt, dyn_st_result_t* r);
int dyn_st_ttest_ind(const double* a, uint32_t na, const double* b, uint32_t nb, int equal_var, dyn_st_alt_t alt,
    dyn_st_result_t* r);
int dyn_st_ttest_rel(const double* a, const double* b, uint32_t n, dyn_st_alt_t alt, dyn_st_result_t* r);
int dyn_st_ztest_prop(double c1, double n1, double c2, double n2, dyn_st_alt_t alt, dyn_st_result_t* r);
int dyn_st_mannwhitney(const double* a, uint32_t na, const double* b, uint32_t nb, dyn_st_alt_t alt,
    dyn_st_method_t method, int continuity, dyn_st_result_t* r);
int dyn_st_ks2(const double* a, uint32_t na, const double* b, uint32_t nb, dyn_st_alt_t alt, dyn_st_method_t method,
    dyn_st_result_t* r);
int dyn_st_chisquare(const double* obs, const double* exp, uint32_t n, uint32_t ddof, dyn_st_result_t* r);
int dyn_st_chi2_contingency(const double* table, uint32_t rows, uint32_t cols, int correction, double* expected,
    dyn_st_result_t* r);
int dyn_st_pearson(const double* x, const double* y, uint32_t n, dyn_st_alt_t alt, dyn_st_result_t* r);
int dyn_st_spearman(const double* x, const double* y, uint32_t n, dyn_st_alt_t alt, dyn_st_result_t* r);
int dyn_st_p_adjust(const double* p, uint32_t n, dyn_st_adjust_t method, double* out);
int dyn_st_psi_counts(const double* expected, const double* actual, uint32_t n, double eps, double* out);
int dyn_st_psi(const double* expected, uint32_t ne, const double* actual, uint32_t na, uint32_t bins, double eps,
    double* out);

typedef enum {
    DYN_ST_BOOT_MEAN = 0,
    DYN_ST_BOOT_MEDIAN = 1
} dyn_st_boot_stat_t;

typedef enum {
    DYN_ST_BOOT_DIFF = 0,
    DYN_ST_BOOT_RATIO = 1
} dyn_st_boot_cmp_t;

typedef int (*dyn_st_stop_fn)(void* ud);

typedef struct {
    dyn_st_boot_stat_t stat;
    dyn_st_boot_cmp_t cmp;
    uint32_t resamples;
    double level;
    uint64_t seed;
    dyn_st_stop_fn stop;
    void* ud;
    double estimate, low, high;
} dyn_st_boot_t;

typedef struct {
    double (*f1)(double);
    double (*f2)(double, double);
    double (*f3)(double, double, double);
    double p0, p1;
} dyn_st_map_t;

void dyn_st_map(const dyn_st_map_t* m, double* v, uint32_t n);

uint64_t dyn_st_boot_draw(uint64_t* state);
int dyn_st_bootstrap(const double* a, uint32_t na, const double* b, uint32_t nb, dyn_st_boot_t* o);

#ifdef __cplusplus
}
#endif

#endif
