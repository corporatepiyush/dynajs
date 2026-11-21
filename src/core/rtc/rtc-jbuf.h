#ifndef DYN_RTC_JBUF_H
#define DYN_RTC_JBUF_H

#include "rtc-payload.h"
#include "rtc-pktring.h"

#define RTC_JB_START_UNKNOWN 2

enum {
    RTC_JB_FMT_RAW,
    RTC_JB_FMT_VP8,
    RTC_JB_FMT_H264
};

typedef struct {
    uint32_t ext;
    uint32_t ts;
    uint32_t at_ms;
    uint8_t used;
    uint8_t marker;
    uint8_t start;
    uint8_t key;
} rtc_jb_slot;

typedef struct {
    uint32_t ts;
    uint32_t first_ext;
    uint32_t last_ext;
    uint32_t first_ms;
    uint32_t npkts;
    uint8_t key;
} rtc_jb_frame;

typedef struct {
    uint64_t emitted;
    uint64_t delay_ms;
    uint64_t target_ms;
    uint64_t frames_dropped;
    uint64_t late;
    uint64_t dups;
    uint64_t multi;
    uint64_t keyframes;
} rtc_jb_stats;

typedef struct {
    rtc_jb_slot* slot;
    rtc_pktring ring;
    rtc_jb_stats st;
    uint32_t mask;
    uint32_t next_ext;
    uint32_t newest_ext;
    uint32_t target_ms;
    uint32_t max_wait_ms;
    uint8_t init;
    uint8_t audio;
    uint8_t head_known;
    uint8_t want_key;
} rtc_jb;

int rtc_jb_init(rtc_jb* j, uint32_t nslot, size_t bytes, int audio);
void rtc_jb_free(rtc_jb* j);
void rtc_jb_set_target(rtc_jb* j, uint32_t target_ms, uint32_t max_wait_ms);
int rtc_jb_insert(rtc_jb* j, uint16_t seq, uint32_t ts, int marker, int start, int key, uint32_t now_ms, const uint8_t* p, size_t n);
int rtc_jb_pop(rtc_jb* j, uint32_t now_ms, rtc_jb_frame* f);
const uint8_t* rtc_jb_pkt(const rtc_jb* j, uint32_t ext, size_t* len);
size_t rtc_jb_bytes(const rtc_jb* j);
int rtc_jb_take_keyreq(rtc_jb* j);
int rtc_jb_frame_copy(rtc_jb* j, const rtc_jb_frame* f, int fmt, uint8_t* out, size_t cap, size_t* len);

#endif
