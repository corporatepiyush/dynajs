#define JS_DTOA_MAX_DIGITS 101

#define JS_DTOA_FORMAT_FREE (0 << 0)
#define JS_DTOA_FORMAT_FIXED (1 << 0)
#define JS_DTOA_FORMAT_FRAC (2 << 0)
#define JS_DTOA_FORMAT_MASK (3 << 0)

#define JS_DTOA_EXP_AUTO (0 << 2)
#define JS_DTOA_EXP_ENABLED (1 << 2)
#define JS_DTOA_EXP_DISABLED (2 << 2)
#define JS_DTOA_EXP_MASK (3 << 2)

#define JS_DTOA_MINUS_ZERO (1 << 4)

#define JS_ATOD_INT_ONLY (1 << 0)
#define JS_ATOD_ACCEPT_BIN_OCT (1 << 1)
#define JS_ATOD_ACCEPT_LEGACY_OCTAL (1 << 2)
#define JS_ATOD_ACCEPT_UNDERSCORES (1 << 3)

typedef struct {
    uint64_t mem[37];
} JSDTOATempMem;

typedef struct {
    uint64_t mem[27];
} JSATODTempMem;

int js_dtoa_max_len(double d, int radix, int n_digits, int flags);
int js_dtoa(char* buf, double d, int radix, int n_digits, int flags,
    JSDTOATempMem* tmp_mem);
double js_atod(const char* str, const char** pnext, int radix, int flags,
    JSATODTempMem* tmp_mem);

#ifdef JS_DTOA_DUMP_STATS
void js_dtoa_dump_stats(void);
#endif

size_t u32toa(char* buf, uint32_t n);
size_t i32toa(char* buf, int32_t n);
size_t u64toa(char* buf, uint64_t n);
size_t i64toa(char* buf, int64_t n);
size_t u64toa_radix(char* buf, uint64_t n, unsigned int radix);
size_t i64toa_radix(char* buf, int64_t n, unsigned int radix);
