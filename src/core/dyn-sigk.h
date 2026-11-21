#ifndef DYN_SIGK_H
#define DYN_SIGK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    DYN_SG_OK = 0,
    DYN_SG_E_OOM = -1,
    DYN_SG_E_SIZE = -2,
    DYN_SG_E_DOMAIN = -3,
    DYN_SG_E_UNSTABLE = -4
};

#define DYN_SG_MAX_N (1u << 26)

int dyn_sg_fft(double* re, double* im, uint32_t n, int inverse);
int dyn_sg_rfft(const double* x, uint32_t n, double* re, double* im);
int dyn_sg_irfft(const double* re, const double* im, uint32_t nin, uint32_t n, double* out);
uint32_t dyn_sg_next_fast(uint32_t n);

typedef enum {
    DYN_SG_WIN_BOXCAR = 0,
    DYN_SG_WIN_HANN = 1,
    DYN_SG_WIN_HAMMING = 2,
    DYN_SG_WIN_BLACKMAN = 3,
    DYN_SG_WIN_BARTLETT = 4,
    DYN_SG_WIN_KAISER = 5
} dyn_sg_window_t;

void dyn_sg_window(dyn_sg_window_t kind, double beta, uint32_t n, int symmetric, double* out);
double dyn_sg_kaiser_beta(double atten);
int dyn_sg_firwin(uint32_t numtaps, const double* cutoff, uint32_t ncut, int pass_zero, int scale, dyn_sg_window_t win,
    double beta, double* h);

int dyn_sg_lfilter(const double* b, uint32_t nb, const double* a, uint32_t na, const double* x, uint32_t n, double* y);
void dyn_sg_sosfilt(const double* sos, uint32_t nsec, double* x, uint32_t n);

typedef enum {
    DYN_SG_BQ_LOWPASS = 0,
    DYN_SG_BQ_HIGHPASS = 1,
    DYN_SG_BQ_BANDPASS = 2,
    DYN_SG_BQ_NOTCH = 3,
    DYN_SG_BQ_ALLPASS = 4,
    DYN_SG_BQ_PEAKING = 5,
    DYN_SG_BQ_LOWSHELF = 6,
    DYN_SG_BQ_HIGHSHELF = 7
} dyn_sg_biquad_t;

void dyn_sg_biquad(dyn_sg_biquad_t type, double w0, double q, double gain_db, double* sos6);

typedef enum {
    DYN_SG_LOWPASS = 0,
    DYN_SG_HIGHPASS = 1,
    DYN_SG_BANDPASS = 2,
    DYN_SG_BANDSTOP = 3
} dyn_sg_btype_t;

int dyn_sg_butter(uint32_t order, double w1, double w2, dyn_sg_btype_t btype, double* sos, uint32_t* nsec);

void dyn_sg_freqz(const double* b, uint32_t nb, const double* a, uint32_t na, const double* w, uint32_t nw, double* hre,
    double* him);
void dyn_sg_sosfreqz(const double* sos, uint32_t nsec, const double* w, uint32_t nw, double* hre, double* him);

typedef enum {
    DYN_SG_FULL = 0,
    DYN_SG_SAME = 1,
    DYN_SG_VALID = 2
} dyn_sg_mode_t;

uint32_t dyn_sg_conv_len(uint32_t na, uint32_t nb, dyn_sg_mode_t mode);
int dyn_sg_convolve(const double* a, uint32_t na, const double* b, uint32_t nb, dyn_sg_mode_t mode, double* out);

typedef struct {
    double fs;
    dyn_sg_window_t window;
    double beta;
    uint32_t nperseg, noverlap, nfft;
    int detrend;
    int density;
} dyn_sg_welch_t;

int dyn_sg_welch(const double* x, uint32_t n, const dyn_sg_welch_t* o, double* pxx);

int dyn_sg_hilbert(const double* x, uint32_t nx, uint32_t n, double* re, double* im);
void dyn_sg_goertzel(const double* x, uint32_t n, double w, double* re, double* im);

#define DYN_SG_BLS_MAX_BINS (1u << 20)

typedef struct {
    uint32_t nbins;
    uint32_t min_points;
} dyn_sg_bls_t;

int dyn_sg_bls(const double* t, const double* y, uint32_t n, const double* periods, uint32_t np, const double* durs,
    uint32_t nd, const dyn_sg_bls_t* o, double* power, double* dur, double* depth, double* epoch);

#ifdef __cplusplus
}
#endif

#endif
