/* Machine facts for dyna:sys (design 26): CPU, memory, load, uptime, disk.
   `features` reports the SAME bitmask the SIMD dispatcher branches on, so the
   day it stops selecting a fast kernel this is the instrument that says so. */

#if defined(__APPLE__)
#include <mach/mach.h>
#include <sys/mount.h>
#include <sys/sysctl.h>
#else
#include <sys/statvfs.h>
#endif

/* A missing fact is null, never a fabricated zero: a caller charting free
   memory must be able to tell "none left" from "this OS did not say". */
static int dyn_mach_set(JSContext *ctx, JSValue o, const char *k, JSValue v)
{
    return JS_DefinePropertyValueStr(ctx, o, k, v, JS_PROP_C_W_E);
}

static int dyn_mach_num(JSContext *ctx, JSValue o, const char *k, int64_t v)
{
    return dyn_mach_set(ctx, o, k, JS_NewInt64(ctx, v));
}

#if defined(__APPLE__)
static int dyn_mach_sysctl_u64(const char *name, uint64_t *out)
{
    uint64_t v = 0;
    size_t len = sizeof v;

    if (sysctlbyname(name, &v, &len, NULL, 0) != 0)
        return -1;
    *out = len == sizeof(uint32_t) ? (uint64_t)*(uint32_t *)&v : v;
    return 0;
}
#else
/* One key from a "Name: value" file such as /proc/meminfo. Values there are
   kibibytes; the caller scales, because /proc/cpuinfo's are not. */
static int dyn_mach_proc_key(const char *path, const char *key, double *out)
{
    char line[256];
    size_t klen = strlen(key);
    FILE *f = fopen(path, "re");

    if (!f)
        return -1;
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, key, klen) == 0 && line[klen] == ':') {
            *out = strtod(line + klen + 1, NULL);
            fclose(f);
            return 0;
        }
    }
    fclose(f);
    return -1;
}
#endif

static JSValue dyn_sys_cpu_info(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    JSValue o, feats;
    uint64_t caps;
    uint32_t nfeat = 0;
    (void)this_val; (void)argc; (void)argv;

    o = JS_NewObject(ctx);
    if (JS_IsException(o))
        return o;
    feats = JS_NewArray(ctx);
    if (JS_IsException(feats)) {
        JS_FreeValue(ctx, o);
        return JS_EXCEPTION;
    }
    simd_init();                        /* the dispatcher's detection, not ours */
    caps = cpu_features();
#define FEAT(bit, name)                                                       \
    if ((caps & (bit)) &&                                                     \
        JS_DefinePropertyValueUint32(ctx, feats, nfeat++,                     \
                                     JS_NewString(ctx, name),                 \
                                     JS_PROP_C_W_E) < 0)                      \
        goto fail;
    FEAT(CPU_SSE42, "sse42")
    FEAT(CPU_AVX2, "avx2")
    FEAT(CPU_AVX512F, "avx512f")
    FEAT(CPU_AVX512BW, "avx512bw")
    FEAT(CPU_AVX512DQ, "avx512dq")
    FEAT(CPU_NEON, "neon")
    FEAT(CPU_SVE, "sve")
#undef FEAT
#if defined(__APPLE__)
    {
        char brand[256];
        size_t len = sizeof brand;
        uint64_t v;
        if (sysctlbyname("machdep.cpu.brand_string", brand, &len, NULL, 0) == 0
            && dyn_mach_set(ctx, o, "model", JS_NewString(ctx, brand)) < 0)
            goto fail;
        if (dyn_mach_sysctl_u64("hw.physicalcpu", &v) == 0
            && dyn_mach_num(ctx, o, "cores", (int64_t)v) < 0)
            goto fail;
        if (dyn_mach_sysctl_u64("hw.logicalcpu", &v) == 0
            && dyn_mach_num(ctx, o, "threads", (int64_t)v) < 0)
            goto fail;
        /* Apple Silicon does not publish hw.cpufrequency; absent beats wrong. */
        if (dyn_mach_sysctl_u64("hw.cpufrequency", &v) == 0
            && dyn_mach_num(ctx, o, "mhz", (int64_t)(v / 1000000)) < 0)
            goto fail;
    }
#else
    {
        char line[512];
        FILE *f = fopen("/proc/cpuinfo", "re");
        long threads = sysconf(_SC_NPROCESSORS_ONLN);
        double mhz;
        if (f) {
            while (fgets(line, sizeof line, f)) {
                if (strncmp(line, "model name", 10) == 0) {
                    char *c = strchr(line, ':');
                    if (c) {
                        size_t n;
                        c++;
                        while (*c == ' ') c++;
                        n = strlen(c);
                        while (n && (c[n - 1] == '\n' || c[n - 1] == ' ')) n--;
                        if (dyn_mach_set(ctx, o, "model",
                                         JS_NewStringLen(ctx, c, n)) < 0) {
                            fclose(f);
                            goto fail;
                        }
                    }
                    break;
                }
            }
            fclose(f);
        }
        if (threads > 0 && dyn_mach_num(ctx, o, "threads", threads) < 0)
            goto fail;
        if (dyn_mach_proc_key("/proc/cpuinfo", "cpu MHz", &mhz) == 0
            && dyn_mach_num(ctx, o, "mhz", (int64_t)mhz) < 0)
            goto fail;
    }
#endif
    if (dyn_mach_set(ctx, o, "features", feats) < 0) {
        feats = JS_UNDEFINED;
        goto fail;
    }
    return o;
fail:
    JS_FreeValue(ctx, feats);
    JS_FreeValue(ctx, o);
    return JS_EXCEPTION;
}

static JSValue dyn_sys_mem_info(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    JSValue o = JS_NewObject(ctx);
    (void)this_val; (void)argc; (void)argv;

    if (JS_IsException(o))
        return o;
#if defined(__APPLE__)
    {
        uint64_t total = 0, page = 0;
        vm_statistics64_data_t vm;
        mach_msg_type_number_t cnt = HOST_VM_INFO64_COUNT;
        if (dyn_mach_sysctl_u64("hw.memsize", &total) == 0
            && dyn_mach_num(ctx, o, "total", (int64_t)total) < 0)
            goto fail;
        if (dyn_mach_sysctl_u64("hw.pagesize", &page) == 0
            && host_statistics64(mach_host_self(), HOST_VM_INFO64,
                                 (host_info64_t)&vm, &cnt) == KERN_SUCCESS) {
            /* "free" is untouched pages; "available" adds what the OS would
               reclaim under pressure, which is the number that answers "can I
               allocate this?" */
            uint64_t freeb = (uint64_t)vm.free_count * page;
            uint64_t avail = freeb + ((uint64_t)vm.inactive_count
                                      + (uint64_t)vm.purgeable_count) * page;
            if (dyn_mach_num(ctx, o, "free", (int64_t)freeb) < 0
                || dyn_mach_num(ctx, o, "available", (int64_t)avail) < 0)
                goto fail;
        }
    }
#else
    {
        double v;
        if (dyn_mach_proc_key("/proc/meminfo", "MemTotal", &v) == 0
            && dyn_mach_num(ctx, o, "total", (int64_t)(v * 1024)) < 0)
            goto fail;
        if (dyn_mach_proc_key("/proc/meminfo", "MemFree", &v) == 0
            && dyn_mach_num(ctx, o, "free", (int64_t)(v * 1024)) < 0)
            goto fail;
        if (dyn_mach_proc_key("/proc/meminfo", "MemAvailable", &v) == 0
            && dyn_mach_num(ctx, o, "available", (int64_t)(v * 1024)) < 0)
            goto fail;
    }
#endif
    return o;
fail:
    JS_FreeValue(ctx, o);
    return JS_EXCEPTION;
}

static JSValue dyn_sys_load_avg(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    double la[3];
    JSValue a;
    int i;
    (void)this_val; (void)argc; (void)argv;

    if (getloadavg(la, 3) != 3)
        return dyn_sys_throw(ctx, errno, "loadAvg", NULL);
    a = JS_NewArray(ctx);
    if (JS_IsException(a))
        return a;
    for (i = 0; i < 3; i++) {
        if (JS_DefinePropertyValueUint32(ctx, a, (uint32_t)i,
                                         JS_NewFloat64(ctx, la[i]),
                                         JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, a);
            return JS_EXCEPTION;
        }
    }
    return a;
}

static JSValue dyn_sys_uptime(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
#if defined(__APPLE__)
    {
        struct timeval boot;
        size_t len = sizeof boot;
        int mib[2] = { CTL_KERN, KERN_BOOTTIME };
        struct timespec now;
        if (sysctl(mib, 2, &boot, &len, NULL, 0) != 0
            || clock_gettime(CLOCK_REALTIME, &now) != 0)
            return dyn_sys_throw(ctx, errno, "uptime", NULL);
        return JS_NewFloat64(ctx, (double)(now.tv_sec - boot.tv_sec));
    }
#else
    {
        FILE *f = fopen("/proc/uptime", "re");
        double up = 0;
        int ok;
        if (!f)
            return dyn_sys_throw(ctx, errno, "uptime", NULL);
        ok = fscanf(f, "%lf", &up) == 1;
        fclose(f);
        if (!ok)
            return JS_ThrowInternalError(ctx, "uptime: /proc/uptime is unreadable");
        return JS_NewFloat64(ctx, up);
    }
#endif
}

static JSValue dyn_sys_disk_usage(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    const char *path;
    JSValue o;
#if defined(__APPLE__)
    struct statfs st;
#else
    struct statvfs st;
#endif
    uint64_t bsize, total, freeb, avail;
    int rc;

    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "diskUsage(path): path is required");
    path = JS_ToCString(ctx, argv[0]);
    if (!path)
        return JS_EXCEPTION;
#if defined(__APPLE__)
    rc = statfs(path, &st);
    bsize = (uint64_t)st.f_bsize;
#else
    rc = statvfs(path, &st);
    bsize = st.f_frsize ? (uint64_t)st.f_frsize : (uint64_t)st.f_bsize;
#endif
    if (rc != 0) {
        JSValue e = dyn_sys_throw(ctx, errno, "diskUsage", path);
        JS_FreeCString(ctx, path);
        return e;
    }
    JS_FreeCString(ctx, path);
    total = (uint64_t)st.f_blocks * bsize;
    freeb = (uint64_t)st.f_bfree * bsize;
    avail = (uint64_t)st.f_bavail * bsize;
    o = JS_NewObject(ctx);
    if (JS_IsException(o))
        return o;
    /* free is what exists, available is what a non-root caller may take: on a
       filesystem with reserved blocks these differ and only one is useful. */
    if (dyn_mach_num(ctx, o, "total", (int64_t)total) < 0
        || dyn_mach_num(ctx, o, "free", (int64_t)freeb) < 0
        || dyn_mach_num(ctx, o, "available", (int64_t)avail) < 0) {
        JS_FreeValue(ctx, o);
        return JS_EXCEPTION;
    }
    return o;
}
