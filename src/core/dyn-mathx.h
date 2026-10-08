#ifndef DYN_MATHX_H
#define DYN_MATHX_H

#ifdef __cplusplus
extern "C" {
#endif

double dyn_gammainc_p(double a, double x);
double dyn_gammainc_q(double a, double x);

double dyn_gammaincinv(double a, double p);

double dyn_betainc(double x, double a, double b);

double dyn_betaincinv(double p, double a, double b);

double dyn_besseli_nu(double nu, double x);
double dyn_besselk_nu(double nu, double x);
double dyn_besseli_scaled(double nu, double x);
double dyn_besselk_scaled(double nu, double x);
double dyn_besseli(int n, double x);
double dyn_besselk(int n, double x);

int dyn_bessel_order_ok(int n, double x);
double dyn_besselj(int n, double x);
double dyn_bessely(int n, double x);

int dyn_besselh(int kind, int n, double x, double* re, double* im);

double dyn_ellipk(double m);
double dyn_ellipe(double m);

int dyn_ellipj(double u, double m, double* sn, double* cn, double* dn);

double dyn_legendre(int n, int m, double x);

double dyn_polygamma(int n, double x);

double dyn_digamma(double x);

void dyn_airy(double x, double* ai, double* aip, double* bi, double* bip);

#ifdef __cplusplus
}
#endif

#endif
