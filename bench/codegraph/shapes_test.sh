#!/usr/bin/env bash
# A shape query is admitted by firing on the defect it is modelled on. Each
# fixture below is that defect, reduced, next to its fixed form: the query must
# list the BAD function and must not list the GOOD one. Run by the gate after
# the build of the tool:   bench/codegraph/shapes_test.sh <codegraph-binary>
set -u
CG=${1:?usage: shapes_test.sh <codegraph binary>}
T=$(mktemp -d) || exit 2
trap 'rm -rf "$T"' EXIT
mkdir -p "$T/src"
cat > "$T/src/fixtures.c" <<'C'
typedef struct JSContext JSContext;
typedef struct { long v; } JSValue;
typedef JSValue JSValueConst;
JSValue JS_Call(JSContext*, JSValue, JSValue, int, JSValue*);
void JS_FreeValue(JSContext*, JSValue);
int JS_IsException(JSValue);
JSValue JS_GetException(JSContext*);
int JS_SetPropertyStr(JSContext*, JSValue, const char*, JSValue);
JSValue JS_NewTypedArray(JSContext*, int, JSValue*, int);
JSValue JS_NewArrayBufferCopy(JSContext*, const unsigned char*, unsigned long);
JSValue JS_NewInt64(JSContext*, long);
double floor(double); double fma(double, double, double);
int io_uring_queue_init(unsigned, void*, unsigned);
int dyn_uring_ring_open(void*, unsigned, const unsigned*, unsigned);
void* dyn_net_reactor_acquire(JSContext*);
JSValue dyn_net_throw_no_reactor(JSContext*, const char*);
JSValue JS_ThrowInternalError(JSContext*, const char*, ...);
int dyn_evloop_add(void*, int, int, void*, void*);
int fsync(int); int fcntl(int, int, ...);
double strtod(const char*, char**);
unsigned sleep(unsigned);
#define F_FULLFSYNC 51
JSValue JS_UNDEFINED_V;

void bad_pending(JSContext* ctx, JSValue cb, JSValue* a) {
    JSValue ret;
    ret = JS_Call(ctx, cb, JS_UNDEFINED_V, 2, a);
    JS_FreeValue(ctx, ret);
}
void good_pending(JSContext* ctx, JSValue cb, JSValue* a) {
    JSValue ret = JS_Call(ctx, cb, JS_UNDEFINED_V, 2, a);
    if (JS_IsException(ret))
        JS_FreeValue(ctx, JS_GetException(ctx));
    JS_FreeValue(ctx, ret);
}
double bad_fma(double x, double y) {
    double r = x - floor(x / y) * y;
    return r;
}
double good_fma(double x, double y) {
    return fma(-floor(x / y), y, x);
}
int bad_ring(void* ring) {
    return io_uring_queue_init(8192, ring, 0);
}
int good_ring(void* ring, const unsigned* f) {
    return dyn_uring_ring_open(ring, 8192, f, 1);
}
JSValue bad_reactor(JSContext* ctx) {
    if (!dyn_net_reactor_acquire(ctx))
        return JS_ThrowInternalError(ctx, "no reactor");
    return JS_UNDEFINED_V;
}
JSValue good_reactor(JSContext* ctx) {
    if (!dyn_net_reactor_acquire(ctx))
        return dyn_net_throw_no_reactor(ctx, "Thing");
    return JS_UNDEFINED_V;
}
void bad_setkey(JSContext* ctx, JSValue o, const char* key, JSValue v) {
    JS_SetPropertyStr(ctx, o, key, v);
}
void good_setkey(JSContext* ctx, JSValue o, JSValue v) {
    JS_SetPropertyStr(ctx, o, "fixed", v);
}
JSValue bad_typed(JSContext* ctx, const unsigned char* p, unsigned long n) {
    JSValue buf = JS_NewArrayBufferCopy(ctx, p, n);
    return JS_NewTypedArray(ctx, 1, &buf, 1);
}
JSValue good_typed(JSContext* ctx, unsigned long n) {
    JSValue len = JS_NewInt64(ctx, (long)n);
    return JS_NewTypedArray(ctx, 1, &len, 1);
}
int bad_fsync(int fd) { return fsync(fd); }
int good_fsync(int fd) { if (fcntl(fd, F_FULLFSYNC) == 0) return 0; return fsync(fd); }
double bad_locale(const char* s) { return strtod(s, 0); }
int bad_arm(void* lp, int fd, void* cb) {
    (void)dyn_evloop_add(lp, fd, 1, cb, 0);
    return 0;
}
int good_arm(void* lp, int fd, void* cb) {
    return dyn_evloop_add(lp, fd, 1, cb, 0);
}
C
cat > "$T/src/dyna-aio.c" <<'C'
unsigned sleep(unsigned);
void bad_block(void) { sleep(1); }
void good_block(void) { }
C
cat > "$T/src/offload.c" <<'C'
typedef struct JSContext JSContext;
typedef struct { long v; } JSValue;
void dyn_fiber_offload(void); void dyn_fiber_onjs(void);
void JS_FreeValue(JSContext*, JSValue);
JSValue JS_NewInt64(JSContext*, long);
#define JS_FreeValue(...) (dyn_fiber_onjs(), JS_FreeValue(__VA_ARGS__))
JSValue bad_offload(JSContext* ctx, JSValue v) {
    dyn_fiber_offload();
    JS_FreeValue(ctx, v);
    return JS_NewInt64(ctx, 1);
}
void good_offload(JSContext* ctx, JSValue v) {
    dyn_fiber_offload();
    JS_FreeValue(ctx, v);
}
C
cat > "$T/src/offload_explicit.c" <<'C'
typedef struct JSContext JSContext;
typedef struct { long v; } JSValue;
void dyn_fiber_offload(void); void dyn_fiber_onjs(void);
JSValue JS_NewInt64(JSContext*, long);
JSValue JS_ThrowRangeError(JSContext*, const char*);
long kernel(long);
JSValue bad_region(JSContext* ctx, long x) {
    long r;
    dyn_fiber_offload();
    r = kernel(x);
    if (r < 0)
        return JS_ThrowRangeError(ctx, "no");
    dyn_fiber_onjs();
    return JS_NewInt64(ctx, r);
}
JSValue good_region(JSContext* ctx, long x) {
    long r;
    dyn_fiber_offload();
    r = kernel(x);
    dyn_fiber_onjs();
    if (r < 0)
        return JS_ThrowRangeError(ctx, "no");
    return JS_NewInt64(ctx, r);
}
C
cat > "$T/src/offload_host.c" <<'C'
typedef struct JSContext JSContext;
typedef struct { long v; } JSValue;
void dyn_fiber_offload(void); void dyn_fiber_onjs(void);
void JS_FreeValue(JSContext*, JSValue);
JSValue JS_NewInt64(JSContext*, long);
#define JS_FreeValue(...) (dyn_fiber_onjs(), JS_FreeValue(__VA_ARGS__))
void host_offload(JSContext* ctx, JSValue v) {
    dyn_fiber_offload();
    JS_FreeValue(ctx, v);
}
#include "offload_frag.inc.c"
C
cat > "$T/src/offload_frag.inc.c" <<'C'
JSValue bad_frag(JSContext* ctx, JSValue v) {
    dyn_fiber_offload();
    JS_FreeValue(ctx, v);
    return JS_NewInt64(ctx, 1);
}
void good_frag(JSContext* ctx, JSValue v) {
    dyn_fiber_offload();
    JS_FreeValue(ctx, v);
}
C
rc=0
check() {  # query  must-list  must-not-list
  local out
  out=$(CODEGRAPH_DEFINES=CONFIG_FIXTURE "$CG" "$T/src" --quiet --csv "$1" 2>&1)
  if ! printf '%s\n' "$out" | grep -q "^$2,"; then
    echo "shapes: FAIL $1 does not list $2"; printf '%s\n' "$out" | head -4 | sed 's/^/    /'; rc=1
  elif [ -n "$3" ] && printf '%s\n' "$out" | grep -q "^$3,"; then
    echo "shapes: FAIL $1 lists the FIXED form $3"; rc=1
  fi
}
check pending-exception-dropped   bad_pending good_pending
check fma-dependent-exactness     bad_fma     good_fma
check raw-uring-ring-open         bad_ring    good_ring
check reactor-acquire-error-shape bad_reactor good_reactor
check set-property-dynamic-key    bad_setkey  good_setkey
check typed-array-short-args      bad_typed   good_typed
check fsync-without-fullfsync     bad_fsync   good_fsync
check locale-number-parse         bad_locale  ""
check discarded-arm-result        bad_arm     good_arm
check reactor-blocking-call       bad_block   good_block
check offload-unguarded-runtime-call bad_offload good_offload
check offload-unguarded-runtime-call bad_region  good_region
check offload-unguarded-runtime-call bad_frag    good_frag
[ "$rc" = 0 ] && echo "shapes: 11 shape queries fire on their fixtures and spare the fixed forms"
exit "$rc"
