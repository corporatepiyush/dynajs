#ifndef DYN_TEXTDEC_H
#define DYN_TEXTDEC_H

#include <stddef.h>
#include <stdint.h>

#define DYN_TD_ENC_UTF8 0
#define DYN_TD_ENC_REPLACEMENT 36
#define DYN_TD_ENC_UTF16BE 37
#define DYN_TD_ENC_UTF16LE 38

typedef struct {
    uint8_t enc;
    uint8_t bom_seen;
    uint8_t first, second, third;
    uint8_t lead;
    uint8_t jis0212;
    uint8_t iso_state, iso_out_state, iso_output;
    int16_t lead_byte;
    uint32_t lead_surrogate;
} dyn_textdec_t;

int dyn_textdec_label(const char* s, size_t n);
const char* dyn_textdec_name(int enc);
void dyn_textdec_init(dyn_textdec_t* d, int enc);
size_t dyn_textdec_max_out(size_t n);
long dyn_textdec_run(dyn_textdec_t* d, const uint8_t* in, size_t n, int flush, int fatal, int strip_bom,
    uint16_t* out);

#endif
