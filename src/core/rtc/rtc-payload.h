#ifndef DYN_RTC_PAYLOAD_H
#define DYN_RTC_PAYLOAD_H

#include "rtc.h"

#define RTC_PAY_MIN_MTU 16u
#define RTC_OPUS_MAX_FRAMES 48u
#define RTC_OPUS_MAX_FRAME_BYTES 1275u

typedef struct {
    uint32_t samples;
    uint32_t padding;
    uint16_t frame_len[RTC_OPUS_MAX_FRAMES];
    uint16_t frame_off[RTC_OPUS_MAX_FRAMES];
    uint8_t nframes;
    uint8_t config;
    uint8_t stereo;
    uint8_t code;
    uint8_t vbr;
} rtc_opus_info;

typedef struct {
    const uint8_t* s;
    uint32_t n;
    uint32_t off;
    uint32_t per;
} rtc_g711_pay;

typedef struct {
    uint32_t first_part;
    uint16_t width;
    uint16_t height;
    uint8_t key;
    uint8_t version;
    uint8_t show;
    uint8_t hscale;
    uint8_t vscale;
    uint8_t hlen;
} rtc_vp8_hdr;

typedef struct {
    int32_t picture_id;
    uint8_t in_frame;
    uint8_t keyframe;
} rtc_vp8_depay;

typedef struct {
    int32_t picture_id;
    int16_t tl0picidx;
    int8_t tid;
    int8_t keyidx;
    uint8_t start;
    uint8_t partition;
    uint8_t non_ref;
    uint8_t layer_sync;
    uint8_t hlen;
} rtc_vp8_desc;

typedef struct {
    const uint8_t* f;
    uint32_t n;
    uint32_t off;
    uint32_t mtu;
    int32_t picture_id;
} rtc_vp8_pay;

typedef struct {
    const uint8_t* f;
    uint32_t n;
    uint32_t pos;
    uint32_t fu_off;
    uint32_t fu_len;
    uint32_t fu_at;
    uint32_t mtu;
    uint8_t fu_hdr;
    uint8_t in_fu;
} rtc_h264_pay;

typedef struct {
    uint8_t in_fu;
    uint8_t fu_type;
    uint8_t keyframe;
    uint8_t broken;
} rtc_h264_depay;

int rtc_opus_parse(const uint8_t* p, size_t n, rtc_opus_info* o);
int rtc_opus_samples(const uint8_t* p, size_t n);

void rtc_g711_pay_init(rtc_g711_pay* s, const uint8_t* samples, size_t n, uint32_t ptime_ms);
size_t rtc_g711_pay_next(rtc_g711_pay* s, uint8_t* out, size_t cap, uint32_t* ts_delta);

int rtc_vp8_desc_parse(const uint8_t* p, size_t n, rtc_vp8_desc* d);
int rtc_vp8_is_keyframe(const uint8_t* p, size_t n);
size_t rtc_vp8_desc_write(uint8_t* out, size_t cap, int start, int32_t picture_id);
int rtc_vp8_hdr_parse(const uint8_t* f, size_t n, rtc_vp8_hdr* h);
void rtc_vp8_depay_reset(rtc_vp8_depay* s);
int rtc_vp8_depay_push(rtc_vp8_depay* s, const uint8_t* p, size_t n, uint8_t* out, size_t cap, size_t* len);
void rtc_vp8_pay_init(rtc_vp8_pay* s, const uint8_t* frame, size_t n, size_t mtu, int32_t picture_id);
size_t rtc_vp8_pay_next(rtc_vp8_pay* s, uint8_t* out, size_t cap, int* last);

int rtc_h264_next_nal(const uint8_t* f, size_t n, uint32_t* pos, uint32_t* start, uint32_t* len);
void rtc_h264_pay_init(rtc_h264_pay* s, const uint8_t* frame, size_t n, size_t mtu);
size_t rtc_h264_pay_next(rtc_h264_pay* s, uint8_t* out, size_t cap, int* last);
int rtc_h264_is_keyframe(const uint8_t* p, size_t n);
int rtc_h264_start_hint(const uint8_t* p, size_t n);
void rtc_h264_depay_reset(rtc_h264_depay* s);
int rtc_h264_depay_push(rtc_h264_depay* s, const uint8_t* p, size_t n, uint8_t* out, size_t cap, size_t* len);

#endif
