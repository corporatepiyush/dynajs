#!/usr/bin/env bash
# tests/ctest/run.sh [--san] [list | all | so | NAME...] -- the C unit/regression programs (data structures,
# codecs, aio, SIMD, crc/sha hardware paths, engine bytecode safety). Each target compiles its own program with
# its own flags (ASan/UBSan, TSan, variants) and runs it; an exit 77 inside a variant loop means "variant n/a
# here". Recipes moved verbatim from the old build.sh targets; the engine-linked ones (test-aio-tls,
# test-{redis,postgres,dns}-final-sweep, test-net-udp-grave, test-aio-echo, test-io-reactor) link the objects of the last
# ./build.sh release (.obj/release/flags.env + objs.list), and the test-<module>-* churn targets drive ./dynajs.
# --san: link those against .obj-san (./build.sh san) and drive .obj-san/dynajs instead; with no NAME it runs
# only the targets --san changes (SAN_TARGETS below), since every other recipe is already ASan/UBSan.
# test-net-hooks-leak: .obj-san/dynajs with detect_leaks=1 on three fetching Workers; FAIL on a dyn_net_on_drain leak (F34).
# test-term-raw-sigterm: forkpty + SIGTERM while keypress() holds raw mode over a Spawn child (F33 handler stacking).
# tsan-workers: builds .obj-tsan/dynajs from .obj/release/rules.mk + -fsanitize=thread (SKIP when the
# compiler cannot) and runs tests/core/tsan_worker_fetch.js and the dyna:ml *_async suites under it.
# persist-seeds / docdb-seeds / bceval-seeds / mail-seeds / scram-seeds / wg_conf-seeds / wg_ip-seeds / wg_tun-seeds / ssh_wire-seeds / wireguard-seeds /
# wg_tcp-seeds / sftp_codec-seeds / fsops_glob-seeds: regenerate the fuzz seed corpora (tests/test-data.tsv run: rows).
# so: builds tests/bjson.so and tests/park_sweep.so, the native-module fixtures test_bjson.js and
# test_shutdown_repark.js load. Output: one PASS/FAIL/SKIP line per target with its wall time; logs in
# .obj/ctest[-san]/NAME.log; exit 1 when any target is not PASS/SKIP. P=jobs (default 4).
set -u
cd "$(dirname "$0")/../.."
export UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1${UBSAN_OPTIONS:+:$UBSAN_OPTIONS}"
ROOT=$PWD
SRC_DIR=src
NCPU=$( (command -v nproc >/dev/null 2>&1 && nproc) || sysctl -n hw.ncpu 2>/dev/null || echo 4 )
DEV_BUILD_J=${DEV_BUILD_J:-$NCPU}
CTEST_RUN_ID=${CTEST_RUN_ID:-$$}
export CTEST_RUN_ID DEV_BUILD_J
TARGETS="
test-io-atomic
test-rtc-core
test-rtc-media
test-rtc-sdp
test-webrtc-dtls-openssl
test-webrtc-srtp
test-exec-close-fds
test-http-static-open
test-http-scan-diff
test-io-slurp-cap
test-regexp-prefilter
test-dtoa-subnormal
test-simd-bitmap
test-simd-f64
test-simd-int
test-simd-reductions
test-simd-sparse
test-text-kernels
test-utf16-kernels
test-simd-tiers
test-simd-x86
test-dyn-common
test-socket-cloexec
test-evloop-cloexec
test-evloop-batch
test-evloop-interest-flip
test-ui-core
test-decnum-core
test-idna-core
test-ctype-core
test-fsops-core
test-fmtcheck-core
test-mlk-core
test-mlk-cluster
test-cfgscan-core
test-graphk-core
test-dfk-core
test-docdb-oom
test-timek-core
test-cryptok-core
test-arck-core
test-netipk-core
test-semverk-core
test-validk-core
test-logk-core
test-exprk-core
test-statk-core
test-sigk-core
test-geok-core
test-csvx-core
test-textdiff-core
test-yamlscalar-core
test-xmlscan-core
test-httpk-core
test-crawlkit-core
test-htmlkit-core
test-ds-core
test-ds-containers
test-ds-algos
test-ds-moved
test-ds-concurrent
test-oracle-path
test-oracle-mathx
test-mathx-debye
test-oracle-dict
test-ac-caps
test-persist-core
test-ds-btree-oom
test-ds-parr
test-par
test-pool
test-pool-stress
test-pool-policy
test-pool-fuzz
test-dns-codec
test-pgwire-core
test-redis-resp-codec
test-frame-core
test-postgres-scram
test-timer
test-bc-read-safety
test-stack-limit
test-interrupt-api
test-native-interrupt-boundary
test-native-interrupt-boundary-switch
test-class-strict-oom
test-stream-interrupt-api
test-http-cookiejar-api
test-file-async-api
test-net-interrupt-api
test-smtp-net-api
test-postgres-copy-api
test-postgres-scram-api
test-postgres-cancel-api
test-postgres-conninfo-api
test-tls-end-point
test-module-isweep
test-stream-isweep
test-async-isweep
test-exception-lost
test-oom-error-object
test-exit-teardown
test-backtrace-oom
test-aio-sendfile-busy
test-aio-hold-flush-order
test-aio-hold-pool
test-aio-udp-mmsg
test-udp-batch
test-aio-cpu-offload
test-aio-connect-offload
test-dns-foreign-answers
test-aio-disk
test-exec-spawn
test-uring-sendfile
test-uring-connect-offload
test-uring-tick
test-uring-taskrun
test-uring-one-enter
test-inflate-stream
test-crc32c-hw
test-crc32c-race
test-crc32-hw
test-xalloc
test-snappend
test-crc32-race
test-sha256-hw
test-aio-tls
test-redis-final-sweep
test-postgres-final-sweep
test-dns-final-sweep
test-net-udp-grave
test-aio-echo
test-io-reactor
test-redis-fdchurn
test-postgres-fdchurn
test-net-udp-selfclose
test-net-teardown-churn
test-redis-teardown-churn
test-postgres-teardown-churn
test-dns-teardown-churn
test-net-eyeballs-churn
test-net-hooks-leak
test-term-raw-sigterm
tsan-workers
test-webrtc-media-e2e
test-ssh-sntrup-kat
test-wireguard-noise
test-wireguard-transport
test-wireguard-device
test-wireguard-glue
test-wireguard-ip
test-wireguard-tcp
test-wireguard-tun
test-wireguard-conf
test-mailk-core
test-mailk-sasl
test-mailk-client
test-mailk-server
test-dkim-core
"
ENG_CTX_TARGETS="
test-aio-tls
test-redis-final-sweep
test-postgres-final-sweep
test-dns-final-sweep
test-net-udp-grave
test-aio-echo
test-io-reactor
test-webrtc-media-e2e
test-stream-interrupt-api
test-http-cookiejar-api
test-file-async-api
test-net-interrupt-api
test-smtp-net-api
test-postgres-copy-api
test-postgres-scram-api
test-postgres-cancel-api
test-postgres-conninfo-api
test-tls-end-point
test-module-isweep
test-stream-isweep
test-async-isweep
"
export ENG_CTX_TARGETS
on_list(){ local x; for x in $2; do [ "$x" = "$1" ] && return 0; done; return 1; }
SAN_TARGETS="
test-aio-tls
test-http-cookiejar-api
test-file-async-api
test-net-interrupt-api
test-smtp-net-api
test-postgres-copy-api
test-postgres-scram-api
test-postgres-cancel-api
test-postgres-conninfo-api
test-tls-end-point
test-redis-final-sweep
test-postgres-final-sweep
test-dns-final-sweep
test-net-udp-grave
test-aio-echo
test-io-reactor
test-redis-fdchurn
test-postgres-fdchurn
test-net-udp-selfclose
test-net-teardown-churn
test-redis-teardown-churn
test-postgres-teardown-churn
test-dns-teardown-churn
test-net-eyeballs-churn
test-webrtc-media-e2e
"
ENG_OBJ=.obj/release ENG_BUILD=release DYNAJS=./dynajs LOGDIR=.obj/ctest
if [ "${1:-}" = --san ]; then
  shift; ENG_OBJ=.obj-san ENG_BUILD=san DYNAJS=./.obj-san/dynajs LOGDIR=.obj/ctest-san
  [ $# -gt 0 ] || set -- $SAN_TARGETS
fi
export ENG_OBJ ENG_BUILD DYNAJS LOGDIR
eng_ctx(){
  [ -f "$ENG_OBJ/flags.env" ] && [ -f "$ENG_OBJ/objs.list" ] || { echo "FAIL: $1 links the objects in $ENG_OBJ: run ./build.sh $ENG_BUILD first"; return 1; }
  . "$ENG_OBJ/flags.env"
  ENG_LIBA=$ROOT/libdynajs.a; [ "$ENG_OBJ" = .obj/release ] || ENG_LIBA=$(cat "$ENG_OBJ/objs.list")
  ENG_CC=$CC ENG_CFLAGS=$CF ENG_LDFLAGS=$LDF ENG_LIBS=$LIBS ENG_LDEXPORT="" OBJDIR=$ENG_OBJ/src ENG_NAT_OBJS=""
  ENG_LIB_OBJS=$(tr ' ' '\n' < "$ENG_OBJ/objs.list" | sed -n "s|^$ENG_OBJ/src/\(.*\)\.o\$|\1|p" | tr '\n' ' ')
}
die(){ echo "FAIL: $*" >&2; exit 1; }
have(){ command -v "$1" >/dev/null 2>&1; }
_dir_objs(){
  local d="$1" f b
  [ -d "$SRC_DIR/$d" ] || return 0
  for f in "$SRC_DIR/$d"/*.c; do
    [ -f "$f" ] || continue
    b=${f##*/}
    case "$b" in *.inc.c) continue ;; esac
    printf '%s ' "$d/${b%.c}"
  done
}
core_objs(){
  [ -f "$SRC_DIR/core/dyn-$1.c" ] && printf '%s ' "dyn-$1"
  _dir_objs "core/$1"
}
core_lib_srcs(){
  local o
  for o in $(core_objs "$1"); do
    case "$o" in */*) printf '%s ' "$SRC_DIR/$o.c" ;; *) printf '%s ' "$SRC_DIR/core/$o.c" ;; esac
  done
}
ENG_CORE_LIBS="hash codec prng compress ds serial persist path mathx ac dict pool par timer dns resp scram snappy httpk"
all_core_srcs(){ local n; for n in $ENG_CORE_LIBS; do core_lib_srcs "$n"; done; }
core_unity(){
  local f
  mkdir -p .obj
  for f in $(core_lib_srcs "$1"); do printf '#include "%s"\n' "$PWD/$f"; done > ".obj/$1-unity.c.$$.tmp" || return 1
  mv ".obj/$1-unity.c.$$.tmp" ".obj/$1-unity.c"
}
core_c17_each(){
  local lib="$1" f; shift
  for f in $(core_lib_srcs "$lib"); do
    "$@" -c "$f" -o ".obj/c17-$(basename "$f" .c).o" || { echo "FAIL: $f does not build alone as C17"; return 1; }
  done
}
engine_core_objs(){ printf '%s ' dynajs engine-parser engine-serialize; _dir_objs builtins; }
engine_core_srcs(){ local o; for o in $(engine_core_objs); do printf '%s ' "$SRC_DIR/$o.c"; done; }
reg_cc(){ have clang && { printf 'clang'; return; }; printf 'gcc'; }
_seed_hex(){ local o="" h=$3 i; for ((i = 0; i < ${#h}; i += 2)); do o="$o\\x${h:i:2}"; done; printf "$o" > "$1/$2"; }
_clib_unit(){
  local libs="$1" t="$2" l srcs="" name
  shift 2
  name=$(basename "$t" .c)
  mkdir -p .obj
  for l in $libs; do
    core_c17_each "$l" $(reg_cc) -std=c17 -pedantic -Wall -Wextra -Werror -Wcast-qual -Wshadow -Wpointer-arith -Wvla -Wformat=2 \
      -Wimplicit-fallthrough -Wstrict-prototypes -Wmissing-prototypes -Wswitch-enum -Wdouble-promotion -Wwrite-strings -Wundef \
      -funsigned-char -Isrc/core -Isrc || return 1
    srcs="$srcs $(core_lib_srcs "$l")"
  done
  # shellcheck disable=SC2086
  $(reg_cc) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer $(ds_san_flags) \
    -std=c17 -pedantic -funsigned-char -Wall -Wextra -Isrc/core -Isrc "$t" $srcs "$@" -lm -o ".obj/$name" || return 1
  "./.obj/$name"
}
ds_san_flags(){
  local f="-fno-sanitize-recover=all"
  echo 'int x;' | $(reg_cc) -fstrict-flex-arrays=2 -x c -c -o /dev/null - 2>/dev/null && f="$f -fstrict-flex-arrays=2"
  printf '%s' "$f"
}
_ctest_src(){ local c; for c in tests/*/"$1".c; do [ -f "$c" ] && { printf '%s' "$c"; return; }; done; printf 'tests/%s.c' "$1"; }
_require_dynajs(){
  [ -x "$DYNAJS" ] || die "${cmd:-run}: no $DYNAJS. Build one first: ./build.sh $ENG_BUILD"
}
_require_native_bin(){
  _require_dynajs
  "$DYNAJS" -e 'import("dyna:mathx")' >/dev/null 2>&1 || {
    echo "FAIL: $DYNAJS cannot load dyna:* modules."
    echo "      Build one first: ./build.sh $ENG_BUILD"
    exit 1
  }
}
_docker_uring(){  # $1 = name, $2 = test source, $3 = out name, $4.. extra defines
  local name="$1" src="$2" out="$3"; shift 3
  if ! docker info >/dev/null 2>&1; then
    echo "SKIP (not run here): $name needs docker (Linux-only io_uring backend)"
    return 0
  fi
  docker run --rm --security-opt seccomp=unconfined --security-opt label=disable --ulimit memlock=-1:-1 -v "$ROOT:/src" -w /src dynajs:deps bash -c "
    cc -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
     -std=gnu17 -D_GNU_SOURCE -DCONFIG_NATIVE_MODULES -DCONFIG_IO_URING $* \
     -Isrc -Isrc/core $src src/dyna-aio-uring.c \
     src/dyna-evloop.c src/dyna-io.c src/core/dyn-pool.c src/core/dyn-timer.c src/cutils.c \
     -lpthread -luring -o /tmp/$out \
     && /tmp/$out"
}
_par_cc(){
  local dir="$1" j="$2" s o rc=0
  shift 2
  local srcs=()
  while [ $# -gt 0 ] && [ "$1" != -- ]; do srcs+=("$1"); shift; done
  [ $# -gt 0 ] && shift
  mkdir -p "$dir"
  PAR_CC_OBJS=""
  for s in "${srcs[@]}"; do
    o="$dir/$(printf '%s' "${s%.c}" | tr '/' '_').o"
    PAR_CC_OBJS="$PAR_CC_OBJS $o"
    rm -f "$o.fail"
    while [ "$(jobs -rp | wc -l | tr -d ' ')" -ge "$j" ]; do wait -n 2>/dev/null || sleep 0.05; done
    { $(reg_cc) "$@" -c "$s" -o "$o" || : > "$o.fail"; } &
  done
  wait
  for s in "${srcs[@]}"; do
    o="$dir/$(printf '%s' "${s%.c}" | tr '/' '_').o"
    [ -f "$o.fail" ] && { echo "FAIL: compile $s"; rc=1; }
  done
  return "$rc"
}
_ctest_shared_objs(){
  local name="$1" dir=".obj/ctest-$1" key="${CTEST_RUN_ID:-solo-$$}" w=0 holder
  shift
  mkdir -p "$dir"
  until mkdir "$dir/.lock" 2>/dev/null; do
    holder=$(cat "$dir/.lock/pid" 2>/dev/null)
    if [ -n "$holder" ] && ! kill -0 "$holder" 2>/dev/null; then rm -rf "$dir/.lock"; continue; fi
    sleep 0.2; w=$((w + 1))
    [ "$w" -lt 6000 ] || { echo "FAIL: ctest objects $name: lock held for 20 min ($dir/.lock)"; return 1; }
  done
  echo "$$" > "$dir/.lock/pid"
  if [ ! -f "$dir/.done.$key" ]; then
    rm -f "$dir"/.done.*
    _par_cc "$dir" "${DEV_BUILD_J:-$NCPU}" "$@" || { rm -rf "$dir/.lock"; return 1; }
    : > "$dir/.done.$key"
  else
    local a=()
    while [ $# -gt 0 ] && [ "$1" != -- ]; do a+=("$1"); shift; done
    PAR_CC_OBJS=""
    for a1 in "${a[@]}"; do PAR_CC_OBJS="$PAR_CC_OBJS $dir/$(printf '%s' "${a1%.c}" | tr '/' '_').o"; done
  fi
  rm -rf "$dir/.lock"
}
_tc_def(){
  local t=".obj/tcprobe-$$"
  mkdir -p "$t"
  if $(reg_cc) -O2 -Werror -o "$t/p" tools/tcprobe/caller.c tools/tcprobe/callee.c tools/tcprobe/ext.c 2>/dev/null && [ "$("$t/p")" = 48 ]; then printf %s -DCONFIG_TAILCALL_INTERP=1; fi
  rm -rf "$t"
}
_ctest_engine_san_objs(){
  # shellcheck disable=SC2046
  _ctest_shared_objs engine-san $(engine_core_srcs) src/dtoa.c src/cutils.c \
    src/libregexp.c src/libunicode.c src/dyna-io.c \
    src/core/simdk/simdk-core.c src/core/simdk/simdk-scalar.c src/core/simdk/simdk-neon.c \
    src/core/simdk/simdk-sse42.c src/core/simdk/simdk-avx2.c src/core/simdk/simdk-avx512.c \
    src/core/simdk/simdk-sve.c $(all_core_srcs) -- \
    -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
    -std=gnu17 -D_GNU_SOURCE '-DCONFIG_VERSION="x"' $(_tc_def) -I. -Isrc -Isrc/core
}
_ctest_engine_san_switch_objs(){
  # shellcheck disable=SC2046
  _ctest_shared_objs engine-san-switch $(engine_core_srcs) src/dtoa.c src/cutils.c \
    src/libregexp.c src/libunicode.c src/dyna-io.c \
    src/core/simdk/simdk-core.c src/core/simdk/simdk-scalar.c src/core/simdk/simdk-neon.c \
    src/core/simdk/simdk-sse42.c src/core/simdk/simdk-avx2.c src/core/simdk/simdk-avx512.c \
    src/core/simdk/simdk-sve.c $(all_core_srcs) -- \
    -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
    -std=gnu17 -D_GNU_SOURCE '-DCONFIG_VERSION="x"' -I. -Isrc -Isrc/core
}
_ctest_engine_rel_objs(){
  # shellcheck disable=SC2046
  _ctest_shared_objs engine-rel $(engine_core_srcs) src/dtoa.c src/cutils.c \
    src/libregexp.c src/libunicode.c src/dyna-io.c \
    src/core/simdk/simdk-core.c src/core/simdk/simdk-scalar.c src/core/simdk/simdk-neon.c \
    src/core/simdk/simdk-sse42.c src/core/simdk/simdk-avx2.c src/core/simdk/simdk-avx512.c \
    src/core/simdk/simdk-sve.c $(all_core_srcs) -- \
    -g -O1 -fno-omit-frame-pointer \
    -std=gnu17 -D_GNU_SOURCE '-DCONFIG_VERSION="x"' $(_tc_def) -I. -Isrc -Isrc/core
}
DOCDB_CORE_SRCS="$(echo src/core/docdb/*.c src/core/bson/*.c) src/core/ds/ds-roaring-bitmap.c src/core/ds/ds-alloc.c \
src/core/dyn-hash.c src/core/dyn-par.c src/core/dyn-pool.c src/core/dyn-prng.c src/libregexp.c src/libunicode.c src/cutils.c"
SIMD_SRCS="src/core/simdk/simdk-core.c src/core/simdk/simdk-scalar.c src/core/simdk/simdk-neon.c \
src/core/simdk/simdk-sse42.c src/core/simdk/simdk-avx2.c src/core/simdk/simdk-avx512.c src/core/simdk/simdk-sve.c"
_c_regression_targets(){
case "$1" in
  test-io-atomic)
    mkdir -p .obj
    $(reg_cc) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
      -Wall -Wextra -std=gnu17 -Isrc \
      tests/file/test_io_atomic_write.c src/dyna-io.c src/cutils.c -o .obj/test_io_atomic || return 1
    ./.obj/test_io_atomic ;;
  test-exec-close-fds)
    mkdir -p .obj
    _ecf_ran=0
    for _ecf_v in default:VARIANT_DEFAULT fail-close-range:VARIANT_FAIL_CLOSE_RANGE:DYN_CLOSE_TEST_FAIL_CLOSE_RANGE \
        getdents:VARIANT_GETDENTS:DYN_CLOSE_TEST_NO_CLOSE_RANGE \
        no-pidinfo:VARIANT_NO_PIDINFO:DYN_CLOSE_TEST_NO_PIDINFO \
        loop:VARIANT_LOOP:DYN_CLOSE_TEST_NO_CLOSE_RANGE:DYN_CLOSE_TEST_NO_GETDENTS:DYN_CLOSE_TEST_NO_PIDINFO:DYN_CLOSE_TEST_NO_CLOSEFROM; do
      _ecf_n=${_ecf_v%%:*}; _ecf_d=""
      for _ecf_x in $(echo "${_ecf_v#*:}" | tr ':' ' '); do _ecf_d="$_ecf_d -D$_ecf_x"; done
      $(reg_cc) -g -O1 -std=gnu17 -D_GNU_SOURCE -Isrc -DDYN_CLOSE_TEST_TRACE -DVARIANT="\"$_ecf_n\"" $_ecf_d \
        tests/core/test_exec_close_fds.c src/dyna-io.c src/cutils.c -lpthread -o .obj/test_exec_close_fds_$_ecf_n || return 1
      ./.obj/test_exec_close_fds_$_ecf_n; _ecf_rc=$?
      [ $_ecf_rc -eq 77 ] && continue
      [ $_ecf_rc -eq 0 ] || return 1
      _ecf_ran=$((_ecf_ran + 1))
    done
    [ $_ecf_ran -ge 3 ] || { echo "test-exec-close-fds: only $_ecf_ran variants ran (want >= 3)"; return 1; } ;;
  test-http-static-open)
    mkdir -p .obj
    $(reg_cc) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all \
      -Wall -Wextra -std=gnu17 -D_GNU_SOURCE -Isrc -Isrc/modules/http -DCONFIG_NATIVE_MODULES=1 -DCONFIG_NATIVE_MODULE_HTTP=1 \
      tests/http/test_http_static_open.c src/modules/http/http-static-open.c -o .obj/test_http_static_open || return 1
    ./.obj/test_http_static_open ;;
  test-http-scan-diff)
    mkdir -p .obj
    $(reg_cc) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all \
      -Wall -Wextra -std=gnu17 -Isrc -Isrc/modules/http -DCONFIG_NATIVE_MODULES=1 -DCONFIG_NATIVE_MODULE_HTTP=1 \
      tests/http/test_http_scan_diff.c src/modules/http/http-scan.c src/core/httpk/httpk-field.c -o .obj/test_http_scan_diff || return 1
    ./.obj/test_http_scan_diff ;;
  test-io-slurp-cap)
    mkdir -p .obj
    $(reg_cc) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
      -Wall -Wextra -std=gnu17 -Isrc \
      tests/file/test_io_slurp_cap.c src/dyna-io.c src/cutils.c -o .obj/test_io_slurp_cap || return 1
    ./.obj/test_io_slurp_cap ;;
  test-regexp-prefilter)
    mkdir -p .obj
    $(reg_cc) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
      -Wall -Wextra -Wno-unused-parameter -std=gnu17 -I. -Isrc \
      tests/core/test_regexp_prefilter.c src/libregexp.c src/cutils.c src/libunicode.c \
      $SIMD_SRCS -o .obj/test_regexp_prefilter || return 1
    ASAN_OPTIONS=detect_leaks=0 ./.obj/test_regexp_prefilter ;;
  test-dtoa-subnormal)
    mkdir -p .obj
    $(reg_cc) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
      -Wall -Wextra -Wno-unused-parameter -std=gnu17 -I. -Isrc \
      tests/core/test_dtoa_subnormal.c src/dtoa.c src/cutils.c -o .obj/test_dtoa_subnormal || return 1
    ASAN_OPTIONS=detect_leaks=0 ./.obj/test_dtoa_subnormal ;;
  test-simd-bitmap)
    mkdir -p .obj
    $(reg_cc) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
      -Wall -Wextra -Wno-unused-parameter -std=gnu17 -I. -Isrc \
      tests/simd/test_simd_bitmap.c $SIMD_SRCS src/cutils.c -o .obj/test_simd_bitmap || return 1
    ASAN_OPTIONS=detect_leaks=0 ./.obj/test_simd_bitmap ;;
  # The SIMD/text kernel proofs had NO target at all (B1-08): the .c files sat
  # in tests/ with no build target naming them, so they ran in no gate and
  # under no manual subcommand. Self-contained: kernels + cutils only.
  test-simd-f64|test-simd-int|test-simd-reductions|test-simd-sparse|test-text-kernels|test-utf16-kernels|test-simd-tiers)
    mkdir -p .obj
    local kern="test_${1#test-}"; kern="${kern//-/_}"   # dashed name -> file stem
    # shellcheck disable=SC2086
    $(reg_cc) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
      -Wall -Wextra -Wno-unused-parameter -std=gnu17 -I. -Isrc \
      "$(_ctest_src "$kern")" $SIMD_SRCS src/cutils.c -lm -o ".obj/$1" || return 1
    ASAN_OPTIONS=detect_leaks=0 ".obj/$1" ;;
  # The x86 tiers, EXECUTED on an Apple-silicon Mac. Code for an instruction
  # set this machine cannot run is unrun, not tested -- but Rosetta 2 runs
  # x86-64 here, advertising SSE4.2 by default and AVX2+FMA when
  # ROSETTA_ADVERTISE_AVX=1. So every kernel harness is built for x86_64 and
  # run twice, once per tier, under ASan+UBSan; each harness prints the tier
  # that actually ran and that line is checked, so a harness that silently
  # fell back to another tier fails instead of passing. Elsewhere (Linux, an
  # Intel Mac, no Rosetta) this is a LOUD skip: those hosts run their own tier
  # through the ordinary targets.
  test-simd-x86)
    if [ "$(uname -s)" != Darwin ] || [ "$(uname -m)" != arm64 ] || ! arch -x86_64 /usr/bin/true 2>/dev/null; then
      echo "test-simd-x86: SKIP -- needs Rosetta 2 on an Apple-silicon Mac (this host runs its own tier via test-simd-*)"
      return 0
    fi
    mkdir -p .obj/x86
    local h mode out rc want x86f="-arch x86_64 -w -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -std=gnu17 -I. -Isrc"
    # shellcheck disable=SC2086
    _par_cc .obj/x86/shared "${DEV_BUILD_J:-$NCPU}" $SIMD_SRCS src/cutils.c -- $x86f || { echo "FAIL: test-simd-x86: the SIMD sources do not build for x86_64"; return 1; }
    for h in test_simd_tiers test_simd_f64 test_simd_int test_simd_reductions test_text_kernels test_utf16_kernels test_simd_bitmap; do
      # shellcheck disable=SC2086
      { clang $x86f "$(_ctest_src "$h")" $PAR_CC_OBJS -lm -o ".obj/x86/$h" || : > ".obj/x86/$h.fail"; } &
    done
    wait
    for h in test_simd_tiers test_simd_f64 test_simd_int test_simd_reductions test_text_kernels test_utf16_kernels test_simd_bitmap; do
      [ -f ".obj/x86/$h.fail" ] && { rm -f ".obj/x86/$h.fail"; echo "FAIL: test-simd-x86: $h does not build for x86_64"; return 1; }
    done
    for h in test_simd_tiers test_simd_f64 test_simd_int test_simd_reductions test_text_kernels test_utf16_kernels test_simd_bitmap; do
      for mode in sse4.2 avx2; do
        if [ "$mode" = avx2 ]; then
          out=$(ROSETTA_ADVERTISE_AVX=1 ASAN_OPTIONS=detect_leaks=0 ".obj/x86/$h" 2>&1); rc=$?
          want='avx2=1|AVX2=1|tier: avx2'
        else
          out=$(ASAN_OPTIONS=detect_leaks=0 ".obj/x86/$h" 2>&1); rc=$?
          want='avx2=0|AVX2=0|tier: sse4.2'
        fi
        if [ "$rc" != 0 ]; then
          printf '%s\n' "$out" | tail -15
          echo "FAIL: test-simd-x86: $h on the $mode tier (rc=$rc)"; return 1
        fi
        printf '%s\n' "$out" | grep -E "$want" >/dev/null || {
          printf '%s\n' "$out" | head -3
          echo "FAIL: test-simd-x86: $h did not run on the $mode tier (its ISA line says otherwise)"; return 1; }
        echo "  $h [$mode]: ok"
      done
    done ;;
  # CLOEXEC / descriptor-inheritance proofs (B1-16: "exist, no build target").
  # dyna-aio.h and dyna-evloop.h only declare their types under
  # CONFIG_NATIVE_MODULES, so a target without that define cannot even compile.
  test-dyn-common)
    mkdir -p .obj
    $(reg_cc) -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
      -Wall -Wextra -Werror -std=gnu17 -D_GNU_SOURCE -Isrc/core \
      tests/core/test_dyn_common.c -o .obj/test_dyn_common || return 1
    ./.obj/test_dyn_common ;;
  test-socket-cloexec)
    mkdir -p .obj
    $(reg_cc) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
      -Wall -Wextra -Wno-unused-parameter -std=gnu17 -D_GNU_SOURCE \
      -DCONFIG_NATIVE_MODULES -I. -Isrc -Isrc/core \
      tests/net/test_socket_cloexec.c src/dyna-aio.c src/dyna-evloop.c src/dyna-io.c \
      src/core/dyn-pool.c src/core/dyn-timer.c src/core/dyn-prng.c src/cutils.c \
      -lpthread -o .obj/test_socket_cloexec || return 1
    ASAN_OPTIONS=detect_leaks=0 ./.obj/test_socket_cloexec ;;
  test-evloop-cloexec)
    mkdir -p .obj
    $(reg_cc) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
      -Wall -Wextra -Wno-unused-parameter -std=gnu17 -D_GNU_SOURCE \
      -DCONFIG_NATIVE_MODULES -I. -Isrc -Isrc/core \
      tests/core/test_evloop_cloexec.c src/dyna-evloop.c src/dyna-io.c \
      src/core/dyn-pool.c src/core/dyn-timer.c src/core/dyn-prng.c src/cutils.c \
      -lpthread -o .obj/test_evloop_cloexec || return 1
    ASAN_OPTIONS=detect_leaks=0 ./.obj/test_evloop_cloexec ;;
  test-evloop-batch)
    mkdir -p .obj
    $(reg_cc) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
      -Wall -Wextra -Wno-unused-parameter -Wno-unused-result -std=gnu17 -D_GNU_SOURCE \
      -DCONFIG_NATIVE_MODULES -I. -Isrc -Isrc/core \
      tests/core/test_evloop_batch.c src/dyna-evloop.c src/dyna-io.c \
      src/core/dyn-pool.c src/core/dyn-timer.c src/core/dyn-prng.c src/cutils.c \
      -lpthread -o .obj/test_evloop_batch || return 1
    UBSAN_OPTIONS=halt_on_error=1 ASAN_OPTIONS=detect_leaks=0 ./.obj/test_evloop_batch ;;
  test-evloop-interest-flip)
    mkdir -p .obj
    $(reg_cc) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
      -Wall -Wextra -Wno-unused-parameter -Wno-unused-result -std=gnu17 -D_GNU_SOURCE \
      -DCONFIG_NATIVE_MODULES -I. -Isrc -Isrc/core \
      tests/core/test_evloop_interest_flip.c src/dyna-evloop.c src/dyna-io.c \
      src/core/dyn-pool.c src/core/dyn-timer.c src/core/dyn-prng.c src/cutils.c \
      -lpthread -o .obj/test_evloop_interest_flip || return 1
    UBSAN_OPTIONS=halt_on_error=1 ASAN_OPTIONS=detect_leaks=0 ./.obj/test_evloop_interest_flip ;;
  test-ui-core)
    tools/fetch-ui-deps.sh --check || { echo "test-ui-core: third_party/ui is missing; run tools/fetch-ui-deps.sh"; return 1; }
    mkdir -p .obj
    $(reg_cc) -std=c17 -pedantic -Wall -Wextra -Werror -Wcast-qual -Wshadow -Wpointer-arith -Wvla -Wformat=2 \
      -Wimplicit-fallthrough -Wstrict-prototypes -Wmissing-prototypes -Wswitch-enum -Wdouble-promotion -Wwrite-strings -Wundef \
      -isystem third_party/ui/stb -Isrc/core -c src/core/dyn-ui.c -o .obj/dyn-ui-c17.o || { echo "FAIL: dyn-ui.c does not build alone as C17"; return 1; }
    $(reg_cc) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer $(ds_san_flags) \
      -Wall -Wextra -std=gnu17 -Isrc/core -isystem third_party/ui/stb -isystem third_party/ui/fonts \
      tests/ui/test_ui_core.c src/core/dyn-ui.c -lm -o .obj/test_ui_core || return 1
    ./.obj/test_ui_core ;;
  test-decnum-core)
    _clib_unit "decnum" tests/decimal/test_decnum_core.c ;;
  test-idna-core)
    _clib_unit "idna urlrec" tests/url/test_idna_core.c src/libunicode.c src/cutils.c ;;
  test-ctype-core)
    mkdir -p .obj
    $(reg_cc) -std=c17 -pedantic -Wall -Wextra -Werror -Wcast-qual -Wshadow -Wconversion -Wsign-conversion -Wundef \
      -Wstrict-prototypes -Wmissing-prototypes -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
      -Isrc/core -include src/core/dyn-ctype.h tests/core/test_ctype_core.c -o .obj/test_ctype_core || return 1
    ./.obj/test_ctype_core ;;
  test-fsops-core)
    _clib_unit "fsops" tests/file/test_fsops_core.c ;;
  test-fmtcheck-core)
    _clib_unit "fmtcheck" tests/schema/test_fmtcheck_core.c ;;
  test-mlk-cluster)
    _clib_unit "mlk" tests/ml/test_mlk_cluster.c src/core/dyn-prng.c src/core/dyn-serial.c src/core/dyn-hash.c ;;
  test-mlk-core)
    _clib_unit "mlk" tests/ml/test_mlk_core.c src/core/dyn-prng.c src/core/dyn-serial.c src/core/dyn-hash.c ;;
  test-cfgscan-core)
    _clib_unit "cfgscan" tests/config/test_cfgscan_core.c ;;
  test-graphk-core)
    _clib_unit "graphk ds prng par pool" tests/graph/test_graphk_core.c && _clib_unit "graphk ds prng par pool" tests/graph/test_graphk_analytics.c ;;
  test-exprk-core)
    _clib_unit "exprk" tests/mathx/test_exprk_core.c ;;
  test-logk-core)
    _clib_unit "logk simdk" tests/log/test_logk_core.c ;;
  test-validk-core)
    _clib_unit "validk" tests/validate/test_validk_core.c ;;
  test-semverk-core)
    _clib_unit "semverk ctype" tests/semver/test_semverk_core.c ;;
  test-rtc-core)
    _clib_unit "rtc hash" tests/webrtc/test_rtc_core.c ;;
  test-frame-core)
    _clib_unit "frame" tests/net/test_frame_decoder.c ;;
  test-rtc-media)
    _clib_unit "rtc hash" tests/webrtc/test_rtc_media.c ;;
  test-rtc-sdp)
    _clib_unit "rtc hash" tests/webrtc/test_rtc_sdp.c ;;
  test-webrtc-dtls-openssl)
    local osslpc
    osslpc=$( (command -v brew >/dev/null && brew --prefix --installed openssl@3.5 2>/dev/null) || true)
    [ -n "$osslpc" ] && osslpc="$osslpc/lib/pkgconfig"
    PKG_CONFIG_PATH="$osslpc:${PKG_CONFIG_PATH:-}" pkg-config --exists openssl || { echo "SKIP: test-webrtc-dtls-openssl needs OpenSSL via pkg-config"; exit 0; }
    mkdir -p .obj/webrtc-dtls-openssl
    # shellcheck disable=SC2046
    _clib_unit "rtc hash" tests/webrtc/test_webrtc_dtls_openssl.c src/modules/webrtc/webrtc-dtls.c src/modules/webrtc/webrtc-srtp.c -D_GNU_SOURCE -DCONFIG_NATIVE_MODULES -DCONFIG_NATIVE_MODULE_WEBRTC -DCONFIG_TLS \
      $(PKG_CONFIG_PATH="$osslpc:${PKG_CONFIG_PATH:-}" pkg-config --cflags --libs openssl) ;;
  test-wireguard-noise)
    local osslpc
    osslpc=$( (command -v brew >/dev/null && brew --prefix --installed openssl@3.5 2>/dev/null) || true)
    [ -n "$osslpc" ] && osslpc="$osslpc/lib/pkgconfig"
    PKG_CONFIG_PATH="$osslpc:${PKG_CONFIG_PATH:-}" pkg-config --exists openssl || { echo "SKIP: test-wireguard-noise needs OpenSSL via pkg-config"; exit 0; }
    # shellcheck disable=SC2046
    _clib_unit "" tests/wireguard/test_wg_noise.c src/modules/wireguard/wg-crypto.c src/modules/wireguard/wg-noise.c -DCONFIG_NATIVE_MODULES -DCONFIG_NATIVE_MODULE_WIREGUARD \
      $(PKG_CONFIG_PATH="$osslpc:${PKG_CONFIG_PATH:-}" pkg-config --cflags --libs openssl) -lpthread ;;
  test-wireguard-ip)
    _clib_unit "" tests/wireguard/test_wg_ip.c src/modules/wireguard/wg-ip.c src/fuzz/fuzz_wg_ip.c -D_GNU_SOURCE ;;
  test-wireguard-tcp)
    _clib_unit "" tests/wireguard/test_wg_tcp.c src/modules/wireguard/wg-tcp.c src/modules/wireguard/wg-tcp-sack.c src/modules/wireguard/wg-tcp-timer.c src/modules/wireguard/wg-tcp-input.c src/modules/wireguard/wg-ip.c src/fuzz/fuzz_wg_tcp.c -D_GNU_SOURCE ;;
  test-wireguard-tun)
    _clib_unit "" tests/wireguard/test_wg_tun.c src/modules/wireguard/wg-tun.c src/fuzz/fuzz_wg_tun.c -D_GNU_SOURCE ;;
  test-wireguard-conf)
    _clib_unit "" tests/wireguard/test_wg_conf.c src/modules/wireguard/wg-conf.c src/fuzz/fuzz_wg_conf.c -D_GNU_SOURCE ;;
  test-wireguard-device)
    local osslpc
    osslpc=$( (command -v brew >/dev/null && brew --prefix --installed openssl@3.5 2>/dev/null) || true)
    [ -n "$osslpc" ] && osslpc="$osslpc/lib/pkgconfig"
    PKG_CONFIG_PATH="$osslpc:${PKG_CONFIG_PATH:-}" pkg-config --exists openssl || { echo "SKIP: test-wireguard-device needs OpenSSL via pkg-config"; exit 0; }
    mkdir -p .obj
    # shellcheck disable=SC2046
    $(reg_cc) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer $(ds_san_flags) -std=gnu17 -D_GNU_SOURCE -c -Ddyn_aio_offload_cpu=wg_test_offload_cpu \
      -DCONFIG_NATIVE_MODULES -DCONFIG_NATIVE_MODULE_WIREGUARD -Isrc -Isrc/core $(PKG_CONFIG_PATH="$osslpc:${PKG_CONFIG_PATH:-}" pkg-config --cflags openssl) \
      src/modules/wireguard/wg-device.c -o .obj/wg_device_inj.o || return 1
    mkdir -p .obj
    # shellcheck disable=SC2046
    $(reg_cc) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer $(ds_san_flags) -std=gnu17 -D_GNU_SOURCE \
      -DCONFIG_NATIVE_MODULES -DCONFIG_NATIVE_MODULE_WIREGUARD -Isrc -Isrc/core tests/wireguard/test_wg_device.c \
      src/modules/wireguard/wg-crypto.c src/modules/wireguard/wg-noise.c src/modules/wireguard/wg-transport.c src/modules/wireguard/wg-timers.c \
      src/modules/wireguard/wg-table.c .obj/wg_device_inj.o src/modules/wireguard/wg-ip.c src/dyna-aio.c src/dyna-evloop.c src/dyna-io.c src/core/dyn-pool.c \
      src/core/dyn-timer.c src/cutils.c $(PKG_CONFIG_PATH="$osslpc:${PKG_CONFIG_PATH:-}" pkg-config --cflags --libs openssl) -lpthread \
      -o .obj/test_wg_device || return 1
    ./.obj/test_wg_device ;;
  test-wireguard-glue)
    local osslpc
    osslpc=$( (command -v brew >/dev/null && brew --prefix --installed openssl@3.5 2>/dev/null) || true)
    [ -n "$osslpc" ] && osslpc="$osslpc/lib/pkgconfig"
    PKG_CONFIG_PATH="$osslpc:${PKG_CONFIG_PATH:-}" pkg-config --exists openssl || { echo "SKIP: test-wireguard-glue needs OpenSSL via pkg-config"; exit 0; }
    mkdir -p .obj
    # shellcheck disable=SC2046
    $(reg_cc) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer $(ds_san_flags) -std=gnu17 -D_GNU_SOURCE \
      -DCONFIG_NATIVE_MODULES -DCONFIG_NATIVE_MODULE_WIREGUARD -Isrc -Isrc/core tests/wireguard/test_wg_glue.c src/modules/wireguard/wg-glue.c \
      src/modules/wireguard/wg-crypto.c src/modules/wireguard/wg-noise.c src/modules/wireguard/wg-transport.c src/modules/wireguard/wg-timers.c \
      src/modules/wireguard/wg-table.c src/modules/wireguard/wg-device.c src/modules/wireguard/wg-ip.c src/modules/wireguard/wg-tun.c \
      src/modules/wireguard/wg-tcp.c src/modules/wireguard/wg-tcp-sack.c src/modules/wireguard/wg-tcp-timer.c src/modules/wireguard/wg-tcp-input.c \
      src/dyna-aio.c src/dyna-evloop.c src/dyna-io.c src/core/dyn-pool.c \
      src/core/dyn-timer.c src/cutils.c $(PKG_CONFIG_PATH="$osslpc:${PKG_CONFIG_PATH:-}" pkg-config --cflags --libs openssl) -lpthread \
      -o .obj/test_wg_glue || return 1
    ./.obj/test_wg_glue ;;
  test-wireguard-transport)
    local osslpc
    osslpc=$( (command -v brew >/dev/null && brew --prefix --installed openssl@3.5 2>/dev/null) || true)
    [ -n "$osslpc" ] && osslpc="$osslpc/lib/pkgconfig"
    PKG_CONFIG_PATH="$osslpc:${PKG_CONFIG_PATH:-}" pkg-config --exists openssl || { echo "SKIP: test-wireguard-transport needs OpenSSL via pkg-config"; exit 0; }
    # shellcheck disable=SC2046
    _clib_unit "" tests/wireguard/test_wg_transport.c src/modules/wireguard/wg-crypto.c src/modules/wireguard/wg-noise.c src/modules/wireguard/wg-transport.c \
      src/modules/wireguard/wg-timers.c src/modules/wireguard/wg-table.c src/fuzz/fuzz_wireguard.c src/core/dyn-pool.c -D_GNU_SOURCE -DCONFIG_NATIVE_MODULES -DCONFIG_NATIVE_MODULE_WIREGUARD \
      $(PKG_CONFIG_PATH="$osslpc:${PKG_CONFIG_PATH:-}" pkg-config --cflags --libs openssl) -lpthread ;;
  test-ssh-sntrup-kat)
    local osslpc
    osslpc=$( (command -v brew >/dev/null && brew --prefix --installed openssl@3.5 2>/dev/null) || true)
    [ -n "$osslpc" ] && osslpc="$osslpc/lib/pkgconfig"
    PKG_CONFIG_PATH="$osslpc:${PKG_CONFIG_PATH:-}" pkg-config --exists openssl || { echo "SKIP: test-ssh-sntrup-kat needs OpenSSL via pkg-config"; exit 0; }
    # shellcheck disable=SC2046
    _clib_unit "" tests/ssh/test_sntrup_kat.c src/modules/ssh/ssh-sntrup.c -Isrc/modules/ssh -DCONFIG_NATIVE_MODULES -DCONFIG_NATIVE_MODULE_SSH \
      $(PKG_CONFIG_PATH="$osslpc:${PKG_CONFIG_PATH:-}" pkg-config --cflags --libs openssl) ;;
  test-mailk-core)
    _clib_unit "mailk idna hash prng" tests/smtp/test_mailk_core.c src/fuzz/fuzz_mailk_reply.c src/libunicode.c src/cutils.c ;;
  test-mailk-sasl)
    _clib_unit "mailk idna hash prng" tests/smtp/test_mailk_sasl.c src/fuzz/fuzz_mailk_sasl.c src/libunicode.c src/cutils.c ;;
  test-mailk-client)
    _clib_unit "mailk idna hash prng" tests/smtp/test_mailk_client.c src/fuzz/fuzz_mailk.c src/libunicode.c src/cutils.c ;;
  test-mailk-server)
    _clib_unit "mailk idna hash prng" tests/smtp/test_mailk_server.c src/fuzz/fuzz_mailk_srv.c src/libunicode.c src/cutils.c ;;
  test-dkim-core)
    _clib_unit "dkim hash" tests/dkim/test_dkim_core.c src/fuzz/fuzz_dkim.c ;;
  test-webrtc-srtp)
    local osslpc
    osslpc=$( (command -v brew >/dev/null && brew --prefix --installed openssl@3.5 2>/dev/null) || true)
    [ -n "$osslpc" ] && osslpc="$osslpc/lib/pkgconfig"
    PKG_CONFIG_PATH="$osslpc:${PKG_CONFIG_PATH:-}" pkg-config --exists openssl || { echo "SKIP: test-webrtc-srtp needs OpenSSL via pkg-config"; exit 0; }
    # shellcheck disable=SC2046
    _clib_unit "" tests/webrtc/test_webrtc_srtp.c src/modules/webrtc/webrtc-srtp.c \
      $(PKG_CONFIG_PATH="$osslpc:${PKG_CONFIG_PATH:-}" pkg-config --cflags --libs openssl) ;;
  test-netipk-core)
    _clib_unit "netipk ctype" tests/netip/test_netipk_core.c ;;
  test-arck-core)
    _clib_unit "arck compress hash par pool" tests/compress/test_arck_core.c -lpthread ;;
  test-cryptok-core)
    _clib_unit "cryptok par pool" tests/crypto/test_cryptok_core.c -lpthread ;;
  test-timek-core)
    _clib_unit "timek ctype" tests/time/test_timek_core.c ;;
  test-docdb-oom)
    mkdir -p .obj
    # shellcheck disable=SC2086
    $(reg_cc) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer $(ds_san_flags) -std=gnu17 -funsigned-char \
      -Dmalloc=dd_t_malloc -Dcalloc=dd_t_calloc -Drealloc=dd_t_realloc -Dfree=dd_t_free -Dstrdup=dd_t_strdup -Dstrndup=dd_t_strndup \
      -Isrc/core -Isrc tests/docdb/test_docdb_oom.c $DOCDB_CORE_SRCS $SIMD_SRCS -lm -lpthread -o .obj/test_docdb_oom || return 1
    ./.obj/test_docdb_oom ;;
  test-dfk-core)
    _clib_unit "dfk" tests/dataframe/test_dfk_core.c src/dtoa.c src/cutils.c src/core/dyn-par.c src/core/dyn-pool.c -lpthread ;;
  test-statk-core)
    _clib_unit "statk mathx par pool" tests/mathx/test_statk_core.c ;;
  test-sigk-core)
    _clib_unit "sigk mathx par pool" tests/mathx/test_sigk_core.c ;;
  test-geok-core)
    _clib_unit "geok par pool" tests/mathx/test_geok_core.c ;;
  test-csvx-core)
    _clib_unit "csvx" tests/csv/test_csvx_core.c src/dtoa.c src/cutils.c src/core/dyn-par.c src/core/dyn-pool.c $SIMD_SRCS -lpthread || return 1
    $(reg_cc) -g -O1 -fsanitize=thread -fno-omit-frame-pointer -std=c17 -funsigned-char -Isrc/core -Isrc \
      tests/csv/test_csvx_core.c $(core_lib_srcs csvx) src/dtoa.c src/cutils.c src/core/dyn-par.c src/core/dyn-pool.c $SIMD_SRCS -lm -lpthread -o .obj/test_csvx_core_tsan || return 1
    ./.obj/test_csvx_core_tsan ;;
  test-textdiff-core)
    _clib_unit "textdiff hash" tests/matcher/test_textdiff_core.c ;;
  test-yamlscalar-core)
    _clib_unit "yamlscalar" tests/yaml/test_yamlscalar_core.c ;;
  test-xmlscan-core)
    mkdir -p .obj/htmlkit-simd
    local xo xml_simd_objs=""
    for xo in $SIMD_SRCS src/cutils.c; do
      $(reg_cc) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -std=gnu17 -I. -Isrc \
        -c "$xo" -o ".obj/htmlkit-simd/$(basename "$xo" .c).o" || return 1
      xml_simd_objs="$xml_simd_objs .obj/htmlkit-simd/$(basename "$xo" .c).o"
    done
    # shellcheck disable=SC2086
    _clib_unit "xmlscan" tests/xml/test_xmlscan_core.c $xml_simd_objs ;;
  test-httpk-core)
    _clib_unit "httpk" tests/http/test_httpk_core.c src/core/dyn-prng.c -Dmalloc=hk_t_malloc -Drealloc=hk_t_realloc ;;
  test-crawlkit-core)
    mkdir -p .obj/htmlkit-simd
    local co crawl_simd_objs=""
    for co in $SIMD_SRCS src/cutils.c; do
      $(reg_cc) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -std=gnu17 -I. -Isrc \
        -c "$co" -o ".obj/htmlkit-simd/$(basename "$co" .c).o" || return 1
      crawl_simd_objs="$crawl_simd_objs .obj/htmlkit-simd/$(basename "$co" .c).o"
    done
    # shellcheck disable=SC2086
    _clib_unit "crawlkit" tests/scrape/test_crawlkit_core.c $crawl_simd_objs ;;
  test-htmlkit-core)
    mkdir -p .obj/htmlkit-simd
    local so simd_objs=""
    for so in $SIMD_SRCS src/cutils.c; do
      $(reg_cc) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -std=gnu17 -I. -Isrc \
        -c "$so" -o ".obj/htmlkit-simd/$(basename "$so" .c).o" || return 1
      simd_objs="$simd_objs .obj/htmlkit-simd/$(basename "$so" .c).o"
    done
    # shellcheck disable=SC2086
    _clib_unit "htmlkit markdown" tests/html/test_htmlkit_core.c $simd_objs ;;
  test-ds-core)
    mkdir -p .obj
    core_c17_each ds $(reg_cc) -std=c17 -pedantic -Wall -Wextra -Werror -Wcast-qual -Wshadow -Wpointer-arith -Wvla -Wformat=2 \
      -Wimplicit-fallthrough -Wstrict-prototypes -Wmissing-prototypes -Wswitch-enum -Wdouble-promotion -Wwrite-strings -Wundef \
      -Isrc/core || return 1
    $(reg_cc) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer $(ds_san_flags) \
      -Wall -Wextra -std=gnu17 -Isrc/core \
      tests/structures/test_ds_core.c $(core_lib_srcs ds) -lm -o .obj/test_ds_core || return 1
    ./.obj/test_ds_core ;;
  test-ds-containers)
    mkdir -p .obj
    core_unity ds || return 1
    $(reg_cc) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer $(ds_san_flags) \
      -Wall -Wextra -std=gnu17 -I.obj -Isrc/core \
      tests/structures/test_ds_containers.c -lm -o .obj/test_ds_containers || return 1
    ./.obj/test_ds_containers ;;
  test-ds-algos)
    mkdir -p .obj
    core_unity ds || return 1
    $(reg_cc) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer $(ds_san_flags) \
      -Wall -Wextra -std=gnu17 -I.obj -Isrc/core \
      tests/structures/test_ds_algos.c -lm -o .obj/test_ds_algos || return 1
    ./.obj/test_ds_algos ;;
  test-ds-moved)
    mkdir -p .obj
    core_unity ds || return 1
    $(reg_cc) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer $(ds_san_flags) \
      -Wall -Wextra -std=gnu17 -I.obj -Isrc/core \
      tests/structures/test_ds_moved.c -lm -o .obj/test_ds_moved || return 1
    ./.obj/test_ds_moved ;;
  test-ds-concurrent)
    mkdir -p .obj
    core_unity ds || return 1
    $(reg_cc) -g -O1 -fsanitize=thread -fno-omit-frame-pointer \
      -Wall -Wextra -std=gnu17 -I.obj -Isrc/core \
      tests/structures/test_ds_concurrent.c -lpthread -lm -o .obj/test_ds_concurrent_tsan || return 1
    ./.obj/test_ds_concurrent_tsan || return 1
    $(reg_cc) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer $(ds_san_flags) \
      -Wall -Wextra -std=gnu17 -I.obj -Isrc/core \
      tests/structures/test_ds_concurrent.c -lpthread -lm -o .obj/test_ds_concurrent || return 1
    ./.obj/test_ds_concurrent ;;
  test-oracle-path)
    mkdir -p .obj
    $(reg_cc) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
      -std=gnu17 -D_GNU_SOURCE -Isrc -Isrc/core \
      tests/file/oracle_path_core.c src/core/dyn-path.c src/cutils.c -lm -o .obj/oracle_path_core || return 1
    ./.obj/oracle_path_core ;;
  test-oracle-mathx)
    mkdir -p .obj
    $(reg_cc) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
      -std=gnu17 -D_GNU_SOURCE -Isrc -Isrc/core \
      tests/mathx/oracle_mathx_tierb.c src/core/dyn-mathx.c src/cutils.c -lm -o .obj/oracle_mathx_tierb || return 1
    ./.obj/oracle_mathx_tierb ;;
  test-mathx-debye)
    _clib_unit "mathx" tests/mathx/test_mathx_debye.c ;;
  test-oracle-dict)
    mkdir -p .obj
    # shellcheck disable=SC2086
    $(reg_cc) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
      -std=gnu17 -D_GNU_SOURCE -I. -Isrc -Isrc/core \
      tests/compress/oracle_dict_codec.c src/core/dyn-dict.c src/core/dyn-codec.c src/core/dyn-ac.c \
      src/core/dyn-hash.c $SIMD_SRCS src/cutils.c -lm -o .obj/oracle_dict_codec || return 1
    ./.obj/oracle_dict_codec ;;
  test-ac-caps)
    mkdir -p .obj
    $(reg_cc) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
      -Wall -Wextra -std=gnu17 -Isrc/core \
      tests/matcher/test_ac_caps.c src/core/dyn-ac.c -o .obj/test_ac_caps || return 1
    ./.obj/test_ac_caps ;;
  persist-seeds)
    mkdir -p .obj src/fuzz/corpus_persist
    # shellcheck disable=SC2086
    $(reg_cc) -O1 -std=gnu17 -Isrc/core -Isrc \
      tests/structures/test_persist_core.c src/core/dyn-persist.c src/core/dyn-hash.c $SIMD_SRCS src/cutils.c \
      -lm -lpthread -o .obj/persist_seeds || return 1
    ./.obj/persist_seeds --emit-seeds src/fuzz/corpus_persist ;;
  docdb-seeds)
    mkdir -p .obj src/fuzz/corpus_docdb
    # shellcheck disable=SC2086
    $(reg_cc) -O1 -std=gnu17 -DDD_FUZZ_EMIT_SEEDS -Isrc/core -Isrc \
      src/fuzz/fuzz_docdb.c $DOCDB_CORE_SRCS $SIMD_SRCS -lm -lpthread -o .obj/docdb_seeds || return 1
    ./.obj/docdb_seeds src/fuzz/corpus_docdb ;;
  test-persist-core)
    mkdir -p .obj
    core_c17_each persist $(reg_cc) -std=c17 -pedantic -Wall -Wextra -Werror -Wcast-qual -Wshadow -Wpointer-arith -Wvla -Wformat=2 \
      -Wimplicit-fallthrough -Wstrict-prototypes -Wmissing-prototypes -Wswitch-enum -Wdouble-promotion -Wwrite-strings -Wundef \
      -Isrc/core || return 1
    # shellcheck disable=SC2086
    $(reg_cc) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
      -Wall -Wextra -std=gnu17 -DDYN_PERSIST_FAILPOINTS -Isrc/core -Isrc \
      tests/structures/test_persist_core.c src/core/dyn-persist.c src/core/dyn-hash.c $SIMD_SRCS src/cutils.c \
      -lm -lpthread -o .obj/test_persist_core || return 1
    rm -rf .obj/persist-core-test
    ./.obj/test_persist_core .obj/persist-core-test ;;
  test-ds-btree-oom)
    mkdir -p .obj
    $(reg_cc) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
      -Wall -Wextra -std=gnu17 -D_GNU_SOURCE -Isrc/core \
      tests/structures/test_ds_btree_oom.c $(core_lib_srcs ds) $(core_lib_srcs hash) \
      -o .obj/test_ds_btree_oom || return 1
    ./.obj/test_ds_btree_oom ;;
  test-ds-parr)
    mkdir -p .obj
    $(reg_cc) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer $(ds_san_flags) \
      -Wall -Wextra -std=gnu17 -Isrc/core \
      tests/structures/test_ds_parr.c -o .obj/test_ds_parr || return 1
    ./.obj/test_ds_parr ;;
  test-par)
    mkdir -p .obj
    $(reg_cc) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
      -Wall -Wextra -std=gnu17 -Isrc/core \
      tests/core/test_par.c src/core/dyn-par.c src/core/dyn-pool.c -lpthread -o .obj/test_par || return 1
    ./.obj/test_par || return 1
    $(reg_cc) -g -O1 -fsanitize=thread -fno-omit-frame-pointer \
      -Wall -Wextra -std=gnu17 -Isrc/core \
      tests/core/test_par.c src/core/dyn-par.c src/core/dyn-pool.c -lpthread -o .obj/test_par_tsan || return 1
    ./.obj/test_par_tsan ;;
  test-pool)
    mkdir -p .obj
    $(reg_cc) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
      -Wall -Wextra -std=gnu17 -Isrc/core \
      tests/core/test_pool.c src/core/dyn-pool.c -lpthread -o .obj/test_pool || return 1
    ./.obj/test_pool || return 1
    $(reg_cc) -g -O1 -fsanitize=thread -fno-omit-frame-pointer \
      -Wall -Wextra -std=gnu17 -Isrc/core \
      tests/core/test_pool.c src/core/dyn-pool.c -lpthread -o .obj/test_pool_tsan || return 1
    ./.obj/test_pool_tsan ;;
  test-pool-fuzz)
    mkdir -p .obj
    $(reg_cc) -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
      -Wall -Wextra -Werror -std=gnu17 -Isrc/core \
      tests/core/test_pool_fuzz.c src/core/dyn-pool.c -lpthread -o .obj/test_pool_fuzz || return 1
    ./.obj/test_pool_fuzz || return 1
    $(reg_cc) -g -O1 -fsanitize=thread -fno-omit-frame-pointer \
      -Wall -Wextra -Werror -std=gnu17 -Isrc/core \
      tests/core/test_pool_fuzz.c src/core/dyn-pool.c -lpthread -o .obj/test_pool_fuzz_tsan || return 1
    DYN_FUZZ_ROUNDS=${DYN_FUZZ_ROUNDS:-8} TSAN_OPTIONS=halt_on_error=1 ./.obj/test_pool_fuzz_tsan ;;
  test-pool-policy)
    mkdir -p .obj
    $(reg_cc) -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
      -DPOOL_ELASTIC_MAX=8 -DPOOL_ELASTIC_KEEP_NS=200000000 -Wall -Wextra -std=gnu17 -Isrc/core \
      tests/core/test_pool_policy.c src/core/dyn-pool.c -lpthread -o .obj/test_pool_policy || return 1
    ./.obj/test_pool_policy || return 1
    $(reg_cc) -g -O1 -fsanitize=thread -fno-omit-frame-pointer \
      -DPOOL_ELASTIC_MAX=8 -DPOOL_ELASTIC_KEEP_NS=200000000 -Wall -Wextra -std=gnu17 -Isrc/core \
      tests/core/test_pool_policy.c src/core/dyn-pool.c -lpthread -o .obj/test_pool_policy_tsan || return 1
    TSAN_OPTIONS="halt_on_error=1" POOL_POLICY_ONLY=elastic,one,overflow,hol,cancel,tiny ./.obj/test_pool_policy_tsan ;;
  test-pool-stress)
    mkdir -p .obj
    $(reg_cc) -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
      -Wall -Wextra -std=gnu17 -Isrc/core \
      tests/core/test_pool_stress.c src/core/dyn-pool.c -lpthread -o .obj/test_pool_stress || return 1
    POOL_STRESS_SCALE=4 ./.obj/test_pool_stress || return 1
    $(reg_cc) -g -O1 -fsanitize=thread -fno-omit-frame-pointer \
      -Wall -Wextra -std=gnu17 -Isrc/core \
      tests/core/test_pool_stress.c src/core/dyn-pool.c -lpthread -o .obj/test_pool_stress_tsan || return 1
    TSAN_OPTIONS="halt_on_error=1" ./.obj/test_pool_stress_tsan || return 1
    $(reg_cc) -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -DDYN_POOL_RACE_WIDEN \
      -Wall -Wextra -std=gnu17 -Isrc/core \
      tests/core/test_pool_stress.c src/core/dyn-pool.c -lpthread -o .obj/test_pool_stress_widen || return 1
    POOL_STRESS_SCALE=4 ./.obj/test_pool_stress_widen || return 1
    $(reg_cc) -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -DDYN_POOL_FORCE_GENERIC \
      -Wall -Wextra -std=gnu17 -Isrc/core \
      tests/core/test_pool_stress.c src/core/dyn-pool.c -lpthread -o .obj/test_pool_stress_generic || return 1
    POOL_STRESS_SCALE=2 ./.obj/test_pool_stress_generic ;;
  test-pgwire-core)
    mkdir -p .obj
    $(reg_cc) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
      -Wall -Wextra -std=gnu17 -Isrc/core \
      tests/postgres/test_pgwire_core.c src/core/dyn-pgwire.c -o .obj/test_pgwire_core || return 1
    ./.obj/test_pgwire_core ;;
  test-dns-codec)
    mkdir -p .obj
    $(reg_cc) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
      -Wall -Wextra -std=gnu17 -Isrc/core \
      tests/dns/test_dns_codec.c src/core/dyn-dns.c -o .obj/test_dns_codec || return 1
    ./.obj/test_dns_codec ;;
  dns-seeds)
    mkdir -p src/fuzz/corpus_dns
    printf -- '\002\000\010\000\000\000\050\050\050\050\050\050\050\050\050' > src/fuzz/corpus_dns/hostile_enc_long_labels.bin
    printf -- '\001\000\020\000\007\007\077\077\077\077\077\077\077\077\077\077\077\077\077\077\077\077' > src/fuzz/corpus_dns/hostile_gen_long_chain.bin
    printf -- '\000\000\002\001\000\000\001\000\000\000\000\000\000\077aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\077aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\077aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa>aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\000\000\001\000\001' > src/fuzz/corpus_dns/hostile_raw_256_octets.bin
    printf -- '\000\000\002\001\000\000\001\000\000\000\000\000\000\077aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\077aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\077aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\077aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\000\000\001\000\001' > src/fuzz/corpus_dns/hostile_raw_257_octets.bin
    printf -- '\000\000\004\050\000\000\001\000\000\000\000\000\000\003www\007example\003com\000\000\001\000\001' > src/fuzz/corpus_dns/hostile_raw_opcode_notify.bin
    printf -- '\000\000\006\201\200\000\001\000\001\000\000\000\000\003www\007example\003com\000\000\001\000\001\300\014\000\001\000\001\000\000\000<\377\377\001' > src/fuzz/corpus_dns/hostile_raw_rdlen_forged.bin
    printf -- '\000\000\005\201\200\000\002\000\000\000\000\000\000\003www\007example\003com\000\000\001\000\001\003www' > src/fuzz/corpus_dns/hostile_raw_second_question_short.bin
    printf -- '\000\000\001\001\000\000\001\000\000\000\000\000\000\300\014\000\001\000\001' > src/fuzz/corpus_dns/hostile_raw_self_ptr.bin
    printf -- '\000\000\003\201\200\000\001\000\001\000\000\000\000\003www\007example\003com\000\000\001\000\001\300\014\000\001\000\001\200\000\000\001\000\004\001\002\003\004' > src/fuzz/corpus_dns/hostile_raw_ttl_top_bit.bin
    printf -- '\002\001\003\001\002\003wwwexamplecom' > src/fuzz/corpus_dns/valid_enc_query.bin
    printf -- '\001\002\005\003\001abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz' > src/fuzz/corpus_dns/valid_gen_compressed.bin
    printf -- '\000\0224\001\000\000\001\000\000\000\000\000\000\003www\007example\003com\000\000\001\000\001' > src/fuzz/corpus_dns/valid_raw_query.bin
    printf -- '\000\0224\201\200\000\001\000\001\000\000\000\000\003www\007example\003com\000\000\001\000\001\300\014\000\001\000\001\000\000\000<\000\004\135\270\330\042' > src/fuzz/corpus_dns/valid_raw_response.bin
    ;;
  httpk-seeds)
    mkdir -p src/fuzz/corpus_httpk
    printf -- '\0031fffffffffffff\015\012' > src/fuzz/corpus_httpk/hostile_chunk_overflow.bin
    printf -- '\0030\015\012\012X: y\015\012\015\012' > src/fuzz/corpus_httpk/hostile_chunk_trailer_lf.bin
    printf -- '\0030\015\012\000X: y\015\012\015\012' > src/fuzz/corpus_httpk/hostile_chunk_trailer_nul.bin
    printf -- '\001HTTP/1.1 200 OK\015\012Content-Length: 18446744073709551616\015\012\015\012' > src/fuzz/corpus_httpk/hostile_head_cl_overflow.bin
    printf -- '\001HTTP/1.1 200 OK\015\012Content-Length: 5\015\012Transfer-Encoding: chunked\015\012\015\012' > src/fuzz/corpus_httpk/hostile_head_cl_te.bin
    printf -- '\001HTTP/1.1 200 OK\015\012Content-Type: a\015\012content-type: b\015\012\015\012' > src/fuzz/corpus_httpk/hostile_head_dup.bin
    printf -- '\001HTTP/1.1 200 OK\015\012A: b\015\012 c\015\012\015\012' > src/fuzz/corpus_httpk/hostile_head_fold.bin
    printf -- '\005id: a\000b\012data\015\015\012data: y\015' > src/fuzz/corpus_httpk/hostile_sse_nul_id_cr.bin
    printf -- '\003005;x=\0421\042\015\012hello\015\0120 \011\015\012T: v\015\012\015\012HTTP/1.1' > src/fuzz/corpus_httpk/valid_chunk.bin
    printf -- '\006\033\001\034\002\030;<' > src/fuzz/corpus_httpk/valid_date.bin
    printf -- '\002\004\001\377\002\003\000\001\002\001\000\001' > src/fuzz/corpus_httpk/valid_gen_chunk.bin
    printf -- '\000\005\003\001\002\004\001\000\007\002\003' > src/fuzz/corpus_httpk/valid_gen_head.bin
    printf -- '\004\027\001\005\010\002\014\017\001\022\002' > src/fuzz/corpus_httpk/valid_gen_sse.bin
    printf -- '\001HTTP/1.1 200 OK\015\012Content-Length: 5\015\012Connection: close\015\012\015\012' > src/fuzz/corpus_httpk/valid_head_cl.bin
    printf -- '\001HTTP/1.0 204 \015\012Transfer-Encoding: gzip, chunked\015\012Connection: keep-alive\015\012\015\012' > src/fuzz/corpus_httpk/valid_head_te.bin
    printf -- '\005\357\273\277event: e\015\012id: 7\015\012retry: 250\015\012data: x\015\012\015\012' > src/fuzz/corpus_httpk/valid_sse.bin
    ;;
  pgwire-seeds)
    mkdir -p src/fuzz/corpus_pgwire
    printf -- '\000D\000\000\000\016\000\002\177\377\377\377ab' > src/fuzz/corpus_pgwire/hostile_raw_column_past.bin
    printf -- '\000Z\000\000\000\003I' > src/fuzz/corpus_pgwire/hostile_raw_len3.bin
    printf -- '\000\000\003\000\000\000\377\377\377\377\377\377\377\377' > src/fuzz/corpus_pgwire/hostile_raw_null_run.bin
    printf -- '\000T\000\000\000\012\000\002ab\000\000\000' > src/fuzz/corpus_pgwire/hostile_raw_rowdesc_short.bin
    printf -- '\001\000\000\050\050\050\050\050\050\050\050\050\050\050\050' > src/fuzz/corpus_pgwire/hostile_stream_wide.bin
    printf -- '\003\000\000\000\000\000\000\000\001\000\005\000\000\000\000' > src/fuzz/corpus_pgwire/hostile_values_int8_edge.bin
    printf -- '\002\005\003\001abcdefghijklmnopqrstuvwxyz\377\376' > src/fuzz/corpus_pgwire/valid_mutated.bin
    printf -- '\000D\000\000\000\024\000\003\000\000\000\002hi\377\377\377\377\000\000\000\000' > src/fuzz/corpus_pgwire/valid_raw_datarow.bin
    printf -- '\000E\000\000\0000SERROR\000C42P01\000Mrelation \042x\042 does not exist\000\000' > src/fuzz/corpus_pgwire/valid_raw_error.bin
    printf -- '\000T\000\000\000\033\000\001id\000\000\000@\001\000\001\000\000\000\027\000\004\377\377\377\377\000\001' > src/fuzz/corpus_pgwire/valid_raw_rowdesc.bin
    printf -- '\001\013\001\005abcdefghijklmnopqrstuvwxyz0123456789\001\002\003\004\005' > src/fuzz/corpus_pgwire/valid_stream.bin
    printf -- '\003\020\001\002\003\004\005\006\007\010\011\012\013\014\015\016\017\020\000\000\000\001\000\001' > src/fuzz/corpus_pgwire/valid_values.bin
    printf -- '\004\001\000\002\000\001\000\000' > src/fuzz/corpus_pgwire/valid_copy_response.bin
    printf -- '\004\000\000\001\000\001' > src/fuzz/corpus_pgwire/hostile_copy_text_binary_col.bin
    printf -- '\035\277' > src/fuzz/corpus_pgwire/hostile_rng_exhausted_input.bin
    ;;
  test-redis-resp-codec)
    # -Isrc and dtoa.c: dyn-resp.c uses js_atod for a correctly-rounded,
    # locale-free float parse. A standalone target inherits NONE of the main
    # build's include paths or objects, so it must name them itself.
    mkdir -p .obj
    $(reg_cc) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
      -Wall -Wextra -std=gnu17 -Isrc/core -Isrc \
      tests/redis/test_resp_codec.c src/core/dyn-resp.c src/dtoa.c src/cutils.c \
      -o .obj/test_resp_codec -lm || return 1
    ./.obj/test_resp_codec ;;
  net-seeds)
    mkdir -p src/fuzz/corpus_net
    printf -- '\003\001\001\003\002\001\001\001\001\000\003\000\000\000\000\000aaaaabaaaabaaab' > src/fuzz/corpus_net/hostile_frame_hold.bin
    printf -- '\001\0522\015\012\043x\015\012,1e\015\012' > src/fuzz/corpus_net/hostile_raw_bool.bin
    printf -- '\001\0521\015\012\0521\015\012\0521\015\012\0521\015\012\0521\015\012\0521\015\012\0521\015\012\0521\015\012\0521\015\012\0521\015\012\0521\015\012\0521\015\012\0521\015\012\0521\015\012\0521\015\012\0521\015\012\0521\015\012\0521\015\012\0521\015\012\0521\015\012\0521\015\012\0521\015\012\0521\015\012\0521\015\012\0521\015\012\0521\015\012\0521\015\012\0521\015\012\0521\015\012\0521\015\012\0521\015\012\0521\015\012\0521\015\012\0521\015\012:1\015\012' > src/fuzz/corpus_net/hostile_raw_deep.bin
    printf -- '\001\0449223372036854775807\015\012ab\015\012' > src/fuzz/corpus_net/hostile_raw_hugebulk.bin
    printf -- '\001:9223372036854775808\015\012' > src/fuzz/corpus_net/hostile_raw_intover.bin
    printf -- '\000\000\034\001\001\001\001\001\001\001\001\001\001\001\001\001\001\001\001\001\001\001\001' > src/fuzz/corpus_net/hostile_tree_deep.bin
    printf -- '\002\004\005\003SET\001k\377\012\015\012' > src/fuzz/corpus_net/valid_cmd.bin
    printf -- '\003\001\000\001\001\004\003\001\002\000\020\001\001ab\015\012cab\012abcab\015\012' > src/fuzz/corpus_net/valid_frame_delim.bin
    printf -- '\003\000\001\002\001\003\000\020\003\001\000\001\001\000\000\000\004abcd\000\002xy' > src/fuzz/corpus_net/valid_frame_length.bin
    printf -- '\001\0523\015\012\0443\015\012SET\015\012\0451\015\012+k\015\012:1\015\012\1741\015\012+ttl\015\012:5\015\012,-inf\015\012' > src/fuzz/corpus_net/valid_raw_attr.bin
    printf -- '\001>2\015\012=7\015\012txt:abc\015\012\0501234567890123456789012\015\012' > src/fuzz/corpus_net/valid_raw_push.bin
    printf -- '\000\007\013\002\010abcdefghijklmnopqrstuvwxyz0123456789' > src/fuzz/corpus_net/valid_tree.bin
    ;;
  oauth2-seeds)
    mkdir -p src/fuzz/corpus_oauth2
    printf -- '\000\002abc===' > src/fuzz/corpus_oauth2/hostile_bearer_pad3.bin
    printf -- '\050\015===' > src/fuzz/corpus_oauth2/hostile_bearer_strict_4103.bin
    printf -- '\010\002abc===' > src/fuzz/corpus_oauth2/hostile_bearer_strict_pad3.bin
    printf -- '\005\002\000ab\001cd\015\012ef' > src/fuzz/corpus_oauth2/hostile_ctl_extra.bin
    printf -- '\017\020eyJhbGciOiJIUzI1NiJ9.eyJzdWIiOiIxIn0.c2ln' > src/fuzz/corpus_oauth2/hostile_jwt.bin
    printf -- '\004\001\000x\000y' > src/fuzz/corpus_oauth2/hostile_nul_key.bin
    printf -- '\017\207invalid_token' > src/fuzz/corpus_oauth2/hostile_www_strict_65.bin
    printf -- 'G\001xrealm---description-----https://e.x/a b' > src/fuzz/corpus_oauth2/hostile_www_uri_space.bin
    printf -- '\005\006client/cb/pathread writestate-xyz' > src/fuzz/corpus_oauth2/valid_auth_url.bin
    printf -- '\010\005abc==' > src/fuzz/corpus_oauth2/valid_bearer_strict.bin
    printf -- '\000\005abcDEF-._\176+/==' > src/fuzz/corpus_oauth2/valid_bearer.bin
    printf -- '\003\011client:id\045secret+x \303\251' > src/fuzz/corpus_oauth2/valid_client_auth.bin
    printf -- '\001\003read write openid' > src/fuzz/corpus_oauth2/valid_scope.bin
    printf -- '\004\004codeAredirectBextra' > src/fuzz/corpus_oauth2/valid_token_body.bin
    printf -- '\006\017mF_9.B5f-4.1JqM\173\042access_token\042:\042mF_9.B5f-4.1JqM\042,\042token_type\042:\042Bearer\042,\042expires_in\042:3600\175' > src/fuzz/corpus_oauth2/valid_token_response.bin
    printf -- '\002\020dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk0123456' > src/fuzz/corpus_oauth2/valid_verifier.bin
    printf -- '\367\015invalid_tokenexample realmThe token expiredhttps://e.example/doread write' > src/fuzz/corpus_oauth2/valid_www_full.bin
    printf -- '\017\201invalid_token' > src/fuzz/corpus_oauth2/valid_www_strict_64.bin
    ;;
  test-postgres-scram)
    # the dyn-simd-stub.c the first shape once compiled is gone, so the first
    # attempt fails silently and the SIMD-sources shape runs -- same as before
    mkdir -p .obj
    $(reg_cc) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
      -Wall -Wextra -std=gnu17 -Isrc/core \
      tests/postgres/test_scram.c src/core/dyn-scram.c src/core/dyn-hash.c \
      src/core/dyn-codec.c src/core/dyn-prng.c src/core/dyn-simd-stub.c \
      -o .obj/test_scram 2>/dev/null || \
    $(reg_cc) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
      -Wall -Wextra -std=gnu17 -Isrc/core -Isrc \
      tests/postgres/test_scram.c src/core/dyn-scram.c src/core/dyn-hash.c \
      src/core/dyn-codec.c src/core/dyn-prng.c $SIMD_SRCS \
      -o .obj/test_scram || return 1
    ./.obj/test_scram ;;
  test-timer)
    mkdir -p .obj
    $(reg_cc) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
      -Wall -Wextra -std=gnu17 -Isrc/core \
      tests/core/test_timer.c src/core/dyn-timer.c -o .obj/test_timer || return 1
    ./.obj/test_timer ;;
  test-bc-read-safety)
    mkdir -p .obj
    _ctest_engine_san_objs || return 1
    # shellcheck disable=SC2086
    $(reg_cc) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
      -std=gnu17 -D_GNU_SOURCE -DCONFIG_VERSION=\"x\" -I. -Isrc -Isrc/core \
      tests/core/test_bc_read_safety.c $PAR_CC_OBJS \
      -lm -lpthread -ldl -o .obj/test_bc_read_safety || return 1
    ./.obj/test_bc_read_safety ;;
  scram-seeds)
    mkdir -p src/fuzz/corpus_scram
    printf -- '\000\242\001\000\004\010\004pwpwsaltsaltsfxs\003\000' > src/fuzz/corpus_scram/hostile_gen_iters.bin
    printf -- '\002\007\000\004\004\010\004pwpwsaltsaltsfxs\003\003' > src/fuzz/corpus_scram/hostile_gen_lastbit.bin
    printf -- '\000\303\003\000\004\010\004pwpwsaltsaltsfxs\003\000' > src/fuzz/corpus_scram/hostile_gen_missing.bin
    printf -- '\000\201\005\000\004\010\004pwpwsaltsaltsfxs\003\000' > src/fuzz/corpus_scram/hostile_gen_nonce.bin
    printf -- '\003e=invalid-proof\012' > src/fuzz/corpus_scram/hostile_raw_e.bin
    printf -- '\003r=\001a,s=QQ==,i=1\012e=other-error' > src/fuzz/corpus_scram/hostile_raw_final_e.bin
    printf -- '\003r=\001a,s=QQ==,i=0000000016\012' > src/fuzz/corpus_scram/hostile_raw_iters.bin
    printf -- '\003m=ext,r=\001a,s=QQ==,i=1\012' > src/fuzz/corpus_scram/hostile_raw_m.bin
    printf -- '\003r=AAAAAAAAAAAAAAAAAAAAAAAAAA,s=QQ==,i=1\012' > src/fuzz/corpus_scram/hostile_raw_nonce.bin
    printf -- '\003r=\001a,s=Q,i=1\012' > src/fuzz/corpus_scram/hostile_raw_salt.bin
    printf -- '\001\003\000\021\004 \020pass0123456789abcdef0123456789abcdefsuffixsuffixsuffix\001\020\002\011\007' > src/fuzz/corpus_scram/valid_gen_short.bin
    printf -- '\000\001\000\000\006\020\010pencilSALTSALTSALTSALTsuffix12\003\000' > src/fuzz/corpus_scram/valid_gen.bin
    printf -- '\003r=\001xyz,s=QUJDRA,i=1,x=ext\012x=1,v=\002' > src/fuzz/corpus_scram/valid_raw_unpadded.bin
    printf -- '\003r=\001abc,s=QUJDRA==,i=2\012v=\002' > src/fuzz/corpus_scram/valid_raw.bin
    ;;
  mail-seeds)
    mkdir -p src/fuzz/corpus_mailk src/fuzz/corpus_mailk_reply src/fuzz/corpus_mailk_sasl src/fuzz/corpus_mailk_srv src/fuzz/corpus_dkim
    printf -- '\004\000\003a\015b\012c\015\015\012.\012.\015\012' > src/fuzz/corpus_mailk/hostile_bare.bin
    printf -- '\001220 mx\015\012250-mx\015\012250 STARTTLS\015\012220 go\015\012250 injected\015\012' > src/fuzz/corpus_mailk/hostile_starttls.bin
    printf -- '\005220 mx\015\012250-mx\015\012250-AUTH PLAIN LOGIN SCRAM-SHA-256\015\012250 CHUNKING\015\012334 \015\012235 ok\015\012250 ok\015\012250 ok\015\012250 done\015\012' > src/fuzz/corpus_mailk/valid_cli_bdat.bin
    printf -- '\004220 mx\015\012250-mx\015\012250 PIPELINING\015\012250 ok\015\012250 ok\015\012354 go\015\012250 queued\015\012' > src/fuzz/corpus_mailk/valid_cli_pipe.bin
    printf -- '\204220 mx\015\012250-mx\015\012250-AUTH SCRAM-SHA-256 PLAIN\015\012250 PIPELINING\015\012334 cj1hYmMscz1RU1hDUitRNnNlazhiZjkyLGk9MTY=\015\012' > src/fuzz/corpus_mailk/valid_cli_scram.bin
    printf -- '\000\010\021hi\015\012.dot\015\012..two\015\012\015\012.\015\012after' > src/fuzz/corpus_mailk/valid_data.bin
    printf -- '\000FROM:<a@b.example> SIZE=10 BODY=8BITMIME' > src/fuzz/corpus_mailk/valid_mail.bin
    printf -- '\002<\042q x\042@\133IPv6:2001:db8::1\135>' > src/fuzz/corpus_mailk/valid_path.bin
    printf -- '\001250-mx greets\015\012250-PIPELINING\015\012250 SIZE 100\015\012' > src/fuzz/corpus_mailk/valid_reply.bin
    printf -- '\000\001250-a\015\012251 b\015\0122500 x\015\012599 y\015\012250\015\012250-\015\012250 \015\012' > src/fuzz/corpus_mailk_reply/hostile_codes.bin
    printf -- '\002\002MAIL FROM:<@a.example,@b.example:x@c.example>\015\012RCPT TO:<x@\133300.1.1.1\135>\015\012RCPT TO:<x@-a.example>\015\012' > src/fuzz/corpus_mailk_reply/hostile_route.bin
    printf -- '\000\003550 5.1.1 <x@y> no such user\015\012452 4.5.3 too many\015\012250 2.0.0 ok\015\012' > src/fuzz/corpus_mailk_reply/valid_esc.bin
    printf -- '\001\005MAIL FROM:<a.b@c.example> SIZE=10 BODY=8BITMIME RET=HDRS ENVID=a+2Bb\015\012RCPT TO:<\042q x\042@\133IPv6:2001:db8::1\135> NOTIFY=SUCCESS,FAILURE ORCPT=rfc822;x@y\015\012' > src/fuzz/corpus_mailk_reply/valid_mail.bin
    printf -- '\000\007250-mx.example greets you\015\012250-PIPELINING\015\012250-SIZE 35882577\015\012250-AUTH PLAIN LOGIN\015\012250 ENHANCEDSTATUSCODES\015\012' > src/fuzz/corpus_mailk_reply/valid_multiline.bin
    printf -- '\001\011MAIL FROM:<\303\244@\303\274.example> SMTPUTF8\015\012RCPT TO:<Postmaster>\015\012MAIL FROM:<>\015\012' > src/fuzz/corpus_mailk_reply/valid_utf8.bin
    printf -- '\000AB==AB====' > src/fuzz/corpus_mailk_sasl/hostile_b64.bin
    printf -- '\000a=2Cb=3D=2c=3d=ZZ\302\255\342\200\213\357\254\201' > src/fuzz/corpus_mailk_sasl/hostile_names.bin
    printf -- '\000\342' > src/fuzz/corpus_mailk_sasl/hostile_saslname_bad_utf8.bin
    printf -- '\000AHRpbQB0YW5zdGFhZ3RoZQ==' > src/fuzz/corpus_mailk_sasl/valid_b64.bin
    printf -- '\000n,a=user@example.com,\001host=server.example.com\001port=587\001auth=Bearer vF9dft4qmTc2Nvb3RlckBhbHRhdmlzdGEuY29tCg==\001\001' > src/fuzz/corpus_mailk_sasl/valid_oauthbearer.bin
    printf -- '\000Ursel\000Kurt\000xipj3plmq' > src/fuzz/corpus_mailk_sasl/valid_plain.bin
    printf -- '\001n,,n=user,r=fyko+d2lbbFgONRv9qkxdawL' > src/fuzz/corpus_mailk_sasl/valid_scram_first.bin
    printf -- '\216user\000\000pencil' > src/fuzz/corpus_mailk_sasl/valid_scram_pair_wrong.bin
    printf -- 'Fus=er,x\000adm=in\000p\303\244ss word' > src/fuzz/corpus_mailk_sasl/valid_scram_pair.bin
    printf -- 'Ar=fyko+d2lbbFgONRv9qkxdawL3rfcNHYJY1ZVvWVs7j,s=QSXCR+Q6sek8bf92,i=4096' > src/fuzz/corpus_mailk_sasl/valid_scram_server.bin
    printf -- '\000user=someuser@example.com\001auth=Bearer vF9dft4qmTc2Nvb3RlckBhdHRhdmlzdGEuY29tCg==\001\001' > src/fuzz/corpus_mailk_sasl/valid_xoauth2.bin
    printf -- '\000\001HELO\015\012foo\015\012bar\015\012MAIL\015\012RCPT\015\012DATA\015\012BDAT x\015\012AUTH BOGUS\015\012\015\012' > src/fuzz/corpus_mailk_srv/hostile_errors.bin
    printf -- '\000\002EHLO aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\015\012NOOP\015\012' > src/fuzz/corpus_mailk_srv/hostile_long.bin
    printf -- 'A\011EHLO c8888888888888888888888888888888888888888888888888888888888\015\012MAIL FROM:<a@b> SIZE=20\015\012RCPT TO:\000\000\000\005<x@y>\015\012RCPT TO c8888888888888888888888888@b.eMAIL FROM:<a@b> SIZE=20\015\012RC\001\000\000\000PT TO:<x@y>\015\012RCPT TO:<z@y>-\012DATA\015\0120123AXXXXXXXXXXXXX\015\012.\015\231\231\231\231\231\231\231\231\231\231O:<z@y>-\012DATA\015\0120123AXXXXXXXXXXXXX\015\012.\015\231\231\231\231\231\231\231\231\231\231\231\231\231\231\231\231\346\346\231\231\231\346\346\346\346\346' > src/fuzz/corpus_mailk_srv/hostile_size_overread.bin
    printf -- '\023\001EHLO c\015\012AUTH PLAIN AHRpbQB4\015\012AUTH LOGIN\015\012dGlt\015\012eA==\015\012MAIL FROM:<q@b>\015\012RSET\015\012' > src/fuzz/corpus_mailk_srv/valid_auth.bin
    printf -- 'O\005EHLO c\015\012MAIL FROM:<a@b.example> BODY=BINARYMIME\015\012RCPT TO:<x@y.example>\015\012BDAT 5\015\012ab\015\012cBDAT 2 LAST\015\012zzNOOP\015\012' > src/fuzz/corpus_mailk_srv/valid_bdat.bin
    printf -- '\007\003EHLO c\015\012MAIL FROM:<a@b.example>\015\012RCPT TO:<x@y.example>\015\012RCPT TO:<no@y.example>\015\012DATA\015\012hi\015\012..x\015\012.\015\012QUIT\015\012' > src/fuzz/corpus_mailk_srv/valid_data.bin
    printf -- 'A\011EHLO c\015\012MAIL FROM:<a@b> SIZE=20\015\012RCPT TO:<x@y>\015\012RCPT TO:<z@y>\015\012DATA\015\0120123456789abcdefghijXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX\015\012.\015\012MAIL FROM:<a@b>\015\012RCPT TO:<x@y>\015\012DATA\015\012ok\015\012.\015\012' > src/fuzz/corpus_mailk_srv/valid_pipeline.bin
    printf -- '\002\011From: a\015\012\015\012 \011 \015\012\015 \012\012.\015\012  \015\012' > src/fuzz/corpus_dkim/hostile_body.bin
    printf -- '\001\003v=1; a=rsa-sha256; d=@; s=s; h=from; bh=2jUSOH9NhtVGCQWNr9BrIAPreKQjO6Sn7XIkfJVOzv8=; b=AIAA' > src/fuzz/corpus_dkim/hostile_d_at.bin
    printf -- '\003\007v=1;a=rsa-sha256;d=x;s=y;h=from;bh=2jUSOH9NhtVGCQWNr9BrIAPreKQjO6Sn7XIkfJVOzv8=;b=AA==;t=9223372036854775807;x=9223372036854775808;i=@y.x' > src/fuzz/corpus_dkim/hostile_tags.bin
    printf -- '\001\003v=1; a=ed25519-sha256; d=b\303\274cher.example; s=s; h=from; bh=2jUSOH9NhtVGCQWNr9BrIAPreKQjO6Sn7XIkfJVOzv8=; b=AIAA' > src/fuzz/corpus_dkim/valid_d_ulabel.bin
    printf -- '\002\001v=DKIM1; k=ed25519; h=sha256; s=email; t=y:s; p=11qYAYKxCrfVS/7TyWQHOg7hcvPapiMlrwIaaPcHURo=' > src/fuzz/corpus_dkim/valid_key_ed.bin
    printf -- '\002\001v=DKIM1; p=' > src/fuzz/corpus_dkim/valid_key_revoked.bin
    printf -- 'G\005From: a@b\015\012To: c\015\012Subject:  s \015\012\011folded\015\012\015\012body  \011 x\015\012\015\012\015\012' > src/fuzz/corpus_dkim/valid_msg.bin
    printf -- '\001\003v=1; a=rsa-sha256; d=example.com; s=sel; c=relaxed/simple; h=from:to; bh=2jUSOH9NhtVGCQWNr9BrIAPreKQjO6Sn7XIkfJVOzv8=; t=10; x=20; l=5; b=AAAA' > src/fuzz/corpus_dkim/valid_sig.bin
    printf -- '\000\002From: a\015\012DKIM-Signature: v=1; a=ed25519-sha256; c=relaxed/relaxed; d=example.com; s=s; h=from:from; bh=2jUSOH9NhtVGCQWNr9BrIAPreKQjO6Sn7XIkfJVOzv8=; b=AAAA\015\012\015\012hi\015\012' > src/fuzz/corpus_dkim/valid_signed.bin
    ;;
  fsops_glob-seeds)
    rm -rf src/fuzz/corpus_fsops_glob; mkdir -p src/fuzz/corpus_fsops_glob
    _seed_hex src/fuzz/corpus_fsops_glob hostile_expand_bracket_comma.bin 017b5b612c625d7d787b2c7d0078
    _seed_hex src/fuzz/corpus_fsops_glob hostile_expand_limit.bin 017b612c627d7b632c647d7b652c667d7b672c687d7b692c6a7d7b6b2c6c7d7b6d2c6e7d7b6f2c707d7b712c727d7b732c747d7b752c767d7b772c787d7b792c7a7d0061636b
    _seed_hex src/fuzz/corpus_fsops_glob hostile_expand_limit_exact.bin 017b612c627d7b612c627d7b612c627d7b612c627d7b612c627d7b612c627d7b612c627d7b612c627d7b612c627d7b612c627d7b612c627d7b612c627d0061
    _seed_hex src/fuzz/corpus_fsops_glob hostile_expand_limit_plus1.bin 017b7b612c627d7b612c627d7b612c627d7b612c627d7b612c627d7b612c627d7b612c627d7b612c627d7b612c627d7b612c627d7b612c627d7b612c627d2c637d0063
    _seed_hex src/fuzz/corpus_fsops_glob hostile_expand_nocomma_outer.bin 017b787b612c627d7d7b7d007b78617d
    _seed_hex src/fuzz/corpus_fsops_glob hostile_glob_bracket_first.bin 005b5d5d2a005d5d5d
    _seed_hex src/fuzz/corpus_fsops_glob hostile_glob_range_high_negated.bin 005b21612d635d0063
    _seed_hex src/fuzz/corpus_fsops_glob hostile_glob_unterminated.bin 00615b6200615b62
    _seed_hex src/fuzz/corpus_fsops_glob hostile_path_dotdot.bin 022e2e2f2e2e2f2e2e002f
    _seed_hex src/fuzz/corpus_fsops_glob valid_expand_inner_comma.bin 017b612c7b622c637d647d006264
    _seed_hex src/fuzz/corpus_fsops_glob valid_expand_nested.bin 017372632f7b612c627b632c647d7d2f2a2e7b6a732c74737d007372632f62642f782e7473
    _seed_hex src/fuzz/corpus_fsops_glob valid_glob_class.bin 005b21612d635d3f7800647178
    _seed_hex src/fuzz/corpus_fsops_glob valid_glob_range_low.bin 005b612d635d78006178
    _seed_hex src/fuzz/corpus_fsops_glob valid_glob_star.bin 002a2e63006d61696e2e63
    _seed_hex src/fuzz/corpus_fsops_glob valid_path_rel.bin 02612f2e2f622f2f2e2e2f632f00612f622f2e2e2f2e2e2f78
    ;;
  wg_conf-seeds)
    rm -rf src/fuzz/corpus_wg_conf; mkdir -p src/fuzz/corpus_wg_conf
    _seed_hex src/fuzz/corpus_wg_conf hostile_dupes.bin 005b496e746572666163655d0a5b496e746572666163655d0a507269766174654b6579203d2079416e7a3554462b6c58584a74653134746a69337a6c4d4e712b68643272595549674a4267423366426d6b5f0a4c697374656e506f7274203d2036353533360a5b506565725d0a456e64706f696e74203d205b312e322e332e345d3a300a416c6c6f776564495073203d2031302e302e302e312f33332c2c2c202c0a5075626c69634b65790a
    _seed_hex src/fuzz/corpus_wg_conf valid_crlf.bin 035b496e746572666163655d0d0a507269766174654b65793d79416e7a3554462b6c58584a74653134746a69337a6c4d4e712b68643272595549674a4267423366426d6b3d0d0a5b506565725d0d0a456e64706f696e74203d203139322e302e322e313a310d0a416c6c6f776564495073203d20302e302e302e302f300d0a
    _seed_hex src/fuzz/corpus_wg_conf valid_full.bin 005b496e746572666163655d0a507269766174654b6579203d2079416e7a3554462b6c58584a74653134746a69337a6c4d4e712b68643272595549674a4267423366426d6b3d0a4c697374656e506f7274203d2035313832300a41646472657373203d2031302e302e302e312f32342c20666430303a3a312f36340a4d5455203d20313432300a444e53203d20312e312e312e310a0a5b506565725d0a5075626c69634b6579203d2078544942413572626f55766e483468746f646a623665363937516a4c455274314e4142346d5a71703844673d0a5072657368617265644b6579203d20414141414141414141414141414141414141414141414141414141414141414141414141414141414141413d0a456e64706f696e74203d205b666430303a3a325d3a35313832300a416c6c6f776564495073203d2031302e302e302e322f33322c203a3a2f300a50657273697374656e744b656570616c697665203d2032350a2320656e640a
    _seed_hex src/fuzz/corpus_wg_conf valid_gen.bin 01010000020003000702000001050c000102030405060708090a0b0c0d0e0f100100010e030101c8050102030405060708090a0b0c0d0e0f10021003020004
    ;;
  wg_ip-seeds)
    rm -rf src/fuzz/corpus_wg_ip; mkdir -p src/fuzz/corpus_wg_ip
    _seed_hex src/fuzz/corpus_wg_ip hostile_frag_opts.bin 014600000000002000400600000a0000010a000002010101010050005000000000000000005002100000000000
    _seed_hex src/fuzz/corpus_wg_ip hostile_v6chain.bin 006000000000200040fd000000000000000000000000000001fd0000000000000000000000000000023c000104000000002b000000000000002c000000000000000000000000000000
    _seed_hex src/fuzz/corpus_wg_ip valid_echo6.bin 016000000000003a40fd000000000000000000000000000001fd000000000000000000000000000002800000000001000170696e67
    _seed_hex src/fuzz/corpus_wg_ip valid_udp4.bin 014500000000004000401100000a0000010a00000204d200350000000068656c6c6f
    ;;
  wg_tun-seeds)
    rm -rf src/fuzz/corpus_wg_tun; mkdir -p src/fuzz/corpus_wg_tun
    _seed_hex src/fuzz/corpus_wg_tun hostile_big.bin 7574756e323134373438333634370d05014500009d00050000000000
    _seed_hex src/fuzz/corpus_wg_tun valid_rt.bin 7574756e370200144500001400000000401100000a0000010a00000205002860000000000011400102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f202122232425262728800000
    ;;
  ssh_wire-seeds)
    rm -rf src/fuzz/corpus_ssh_wire; mkdir -p src/fuzz/corpus_ssh_wire
    _seed_hex src/fuzz/corpus_ssh_wire hostile_b64_afterpad.bin 085a6d39760a20596d46793d78
    _seed_hex src/fuzz/corpus_ssh_wire hostile_pubkey_lines.bin 0509207373682d656432353531392d636572742d763031406f70656e7373682e636f6d204141414149484e7a6143316c5a4449314e5445354c574e6c636e5174646a4178514739775a57357a63326775593239740d0a2320630a0a736b2d7373682d65643235353139406f70656e7373682e636f6d203d3d3d3d0a
    _seed_hex src/fuzz/corpus_ssh_wire valid_b64.bin 014141414143334e7a6143316c5a4449314e5445354141414149413d3d
    _seed_hex src/fuzz/corpus_ssh_wire valid_cert.bin 0400010002020005616c69636503626f62010000010c31302e302e302e302f323430000001020000
    _seed_hex src/fuzz/corpus_ssh_wire valid_cert_force.bin 0400010001010405616c696365000000000e6563686f2068693b206c73202d6c00000001020000
    _seed_hex src/fuzz/corpus_ssh_wire valid_cert_srcaddr.bin 040000000101000105616c69636500010101000101200101000100000000000000000000000000000000000000000000000000000000000000000101
    _seed_hex src/fuzz/corpus_ssh_wire valid_namelist.bin 020b7373682d656432353531390001020001
    _seed_hex src/fuzz/corpus_ssh_wire valid_pubgen.bin 06021b206162636465666768696a6b6c6d6e6f707172737475767778797a30313233
    _seed_hex src/fuzz/corpus_ssh_wire valid_pubkey.bin 057373682d65643235353139204141414143334e7a6143316c5a4449314e54453541414141494f4d71716e6b567a726d3053644736554f6f714b4c73616267483543396f6b5769306468326c39474b4a6c207573657240680a
    _seed_hex src/fuzz/corpus_ssh_wire valid_reader.bin 000600010203000200000003616263050000000109
    _seed_hex src/fuzz/corpus_ssh_wire valid_writer.bin 03004101deadbeef020568656c6c6f030300008003020001
    ;;
  wireguard-seeds)
    rm -rf src/fuzz/corpus_wireguard; mkdir -p src/fuzz/corpus_wireguard
    _seed_hex src/fuzz/corpus_wireguard hostile_data_mtu.bin 030405dc000000000000000000
    _seed_hex src/fuzz/corpus_wireguard hostile_hs_cookie.bin 040201000501
    _seed_hex src/fuzz/corpus_wireguard hostile_replay_window.bin 0107020001000003000100000103040102010001000003010104000100060101
    _seed_hex src/fuzz/corpus_wireguard valid_aip.bin 0200000105080000000105180100010107400205000105050001050200010508040001050300010001000001050900
    _seed_hex src/fuzz/corpus_wireguard valid_data.bin 03010040000000000700000009111111111111111111111111111111111111111111111111111111111111111168656c6c6f
    _seed_hex src/fuzz/corpus_wireguard valid_feed_init.bin 060000
    _seed_hex src/fuzz/corpus_wireguard valid_hs_init.bin 040001000501
    _seed_hex src/fuzz/corpus_wireguard valid_hs_resp_mac2.bin 040100005001
    _seed_hex src/fuzz/corpus_wireguard valid_replay.bin 010000010000010002010600010205010302010003070104000107100001030101060101
    ;;
  wg_tcp-seeds)
    rm -rf src/fuzz/corpus_wg_tcp; mkdir -p src/fuzz/corpus_wg_tcp
    _seed_hex src/fuzz/corpus_wg_tcp hostile_loop_corrupt_pause.bin 01020002000200020000400a01004008000502000a01020009030200
    _seed_hex src/fuzz/corpus_wg_tcp hostile_loop_v6_loss.bin 02010002000200020000ff00ff0701030209030200040306000200090302000200
    _seed_hex src/fuzz/corpus_wg_tcp valid_loop_v4.bin 010001020002000200001402000200010502000200090202000b00020002000200
    _seed_hex src/fuzz/corpus_wg_tcp valid_raw_syn.bin 000a14005001bb00000000000000005002ffff00000000
    ;;
  sftp_codec-seeds)
    rm -rf src/fuzz/corpus_sftp_codec; mkdir -p src/fuzz/corpus_sftp_codec
    _seed_hex src/fuzz/corpus_sftp_codec hostile_ext_count.bin 008000000000000041
    _seed_hex src/fuzz/corpus_sftp_codec hostile_short_size.bin 0000000001000000
    _seed_hex src/fuzz/corpus_sftp_codec valid_attrs_all.bin 000000000f0000000000001000000003e8000003e8000081a46500000065000001
    _seed_hex src/fuzz/corpus_sftp_codec valid_attrs_ext.bin 0080000004000081a400000001000000036578740000000176
    _seed_hex src/fuzz/corpus_sftp_codec valid_gen_ext.bin 010000000f00000000ffffffff0000000100000002000041ed00000000000000000302016101620163016401650166
    _seed_hex src/fuzz/corpus_sftp_codec valid_longname_suid.bin 020000cded000003e80000000000000000000010006500000066696c65206e616d652e747874
    _seed_hex src/fuzz/corpus_sftp_codec valid_status.bin 03000000657061796c6f6164
    ;;
  bceval-seeds)
    mkdir -p .obj src/fuzz/corpus_bceval
    _ctest_engine_san_objs || return 1
    # shellcheck disable=SC2086
    $(reg_cc) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
      -std=gnu17 -D_GNU_SOURCE -DCONFIG_VERSION=\"x\" -I. -Isrc -Isrc/core \
      tests/core/test_bc_read_safety.c $PAR_CC_OBJS \
      -lm -lpthread -ldl -o .obj/bceval_seeds || return 1
    ./.obj/bceval_seeds --emit-seeds src/fuzz/corpus_bceval ;;
  test-stack-limit)
    mkdir -p .obj
    _ctest_engine_san_objs || return 1
    # shellcheck disable=SC2086
    $(reg_cc) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
      -std=gnu17 -D_GNU_SOURCE -DCONFIG_VERSION=\"x\" -I. -Isrc -Isrc/core \
      tests/core/test_stack_limit.c $PAR_CC_OBJS \
      -lm -lpthread -ldl -o .obj/test_stack_limit || return 1
    ./.obj/test_stack_limit ;;
  test-native-interrupt-boundary)
    mkdir -p .obj
    _ctest_engine_san_objs || return 1
    # shellcheck disable=SC2086
    $(reg_cc) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
      -std=gnu17 -D_GNU_SOURCE -DCONFIG_VERSION=\"x\" -I. -Isrc -Isrc/core \
      tests/core/test_native_interrupt_boundary.c $PAR_CC_OBJS \
      -lm -lpthread -ldl -o .obj/test_native_interrupt_boundary || return 1
    ./.obj/test_native_interrupt_boundary ;;
  test-native-interrupt-boundary-switch)
    mkdir -p .obj
    _ctest_engine_san_switch_objs || return 1
    # shellcheck disable=SC2086
    $(reg_cc) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
      -std=gnu17 -D_GNU_SOURCE -DCONFIG_VERSION=\"x\" -I. -Isrc -Isrc/core \
      tests/core/test_native_interrupt_boundary.c $PAR_CC_OBJS \
      -lm -lpthread -ldl -o .obj/test_native_interrupt_boundary_switch || return 1
    ./.obj/test_native_interrupt_boundary_switch ;;
  test-interrupt-api)
    mkdir -p .obj
    _ctest_engine_san_objs || return 1
    # shellcheck disable=SC2086
    $(reg_cc) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
      -std=gnu17 -D_GNU_SOURCE -DCONFIG_VERSION=\"x\" -I. -Isrc -Isrc/core \
      tests/core/test_interrupt_api.c $PAR_CC_OBJS \
      -lm -lpthread -ldl -o .obj/test_interrupt_api || return 1
    ./.obj/test_interrupt_api ;;
  test-class-strict-oom)
    mkdir -p .obj
    _ctest_engine_san_objs || return 1
    # shellcheck disable=SC2086
    $(reg_cc) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
      -std=gnu17 -D_GNU_SOURCE -DCONFIG_VERSION=\"x\" -I. -Isrc -Isrc/core \
      tests/core/test_class_strict_oom.c $PAR_CC_OBJS \
      -lm -lpthread -ldl -o .obj/test_class_strict_oom || return 1
    ./.obj/test_class_strict_oom ;;
  test-stream-interrupt-api)
    # links the engine minus the CLI/REPL entry objects (native modules included) into a one-shot-interrupt embedder
    [ -n "$ENG_LIB_OBJS" ] || { echo "FAIL: test-stream-interrupt-api needs the configured build objects"; return 1; }
    local sobjs="" so sskip=" $OBJDIR/dyna-cli.o $OBJDIR/dyna-repl.o "
    for so in $ENG_LIB_OBJS $ENG_NAT_OBJS; do
      case "$sskip" in *" $OBJDIR/$so.o "*) continue ;; esac
      sobjs="$sobjs $OBJDIR/$so.o"
    done
    # shellcheck disable=SC2086
    $ENG_CC $ENG_CFLAGS -Isrc $ENG_LDFLAGS $ENG_LDEXPORT -o "$OBJDIR/test_stream_interrupt_api" tests/stream/test_stream_interrupt_api.c $sobjs $ENG_LIBS || return 1
    "$OBJDIR/test_stream_interrupt_api" ;;
  test-file-async-api)
    [ -n "$ENG_LIB_OBJS" ] || { echo "FAIL: test-file-async-api needs the configured build objects"; return 1; }
    local fobjs="" fo fskip=" $OBJDIR/dyna-cli.o $OBJDIR/dyna-repl.o $OBJDIR/modules/file/file-module.o $OBJDIR/modules/file/file-fs.o $OBJDIR/modules/file/file-filecopy.o $OBJDIR/modules/file/file-ahandle.o "
    for fo in $ENG_LIB_OBJS $ENG_NAT_OBJS; do
      case "$fskip" in *" $OBJDIR/$fo.o "*) continue ;; esac
      fobjs="$fobjs $OBJDIR/$fo.o"
    done
    # shellcheck disable=SC2086
    $ENG_CC $ENG_CFLAGS -Isrc -Isrc/modules/file $ENG_LDFLAGS $ENG_LDEXPORT -o "$OBJDIR/test_file_async_api" tests/file/test_file_async_api.c src/modules/file/file-module.c src/modules/file/file-fs.c src/modules/file/file-filecopy.c src/modules/file/file-ahandle.c -Dmalloc=fa_t_malloc -Drealloc=fa_t_realloc -Dfree=fa_t_free -Ddyn_aio_offload=fa_t_offload -Ddyn_net_reactor_acquire=fa_t_acquire $fobjs $ENG_LIBS || return 1
    "$OBJDIR/test_file_async_api" ;;
  test-http-cookiejar-api)
    [ -n "$ENG_LIB_OBJS" ] || { echo "FAIL: test-http-cookiejar-api needs the configured build objects"; return 1; }
    local cobjs="" co cskip=" $OBJDIR/dyna-cli.o $OBJDIR/dyna-repl.o $OBJDIR/core/httpk/httpk-cookie.o "
    for co in $ENG_LIB_OBJS $ENG_NAT_OBJS; do
      case "$cskip" in *" $OBJDIR/$co.o "*) continue ;; esac
      cobjs="$cobjs $OBJDIR/$co.o"
    done
    # shellcheck disable=SC2086
    $ENG_CC $ENG_CFLAGS -Isrc $ENG_LDFLAGS $ENG_LDEXPORT -o "$OBJDIR/test_http_cookiejar_api" tests/http/test_http_cookiejar_api.c src/core/httpk/httpk-cookie.c -Dmalloc=cj_t_malloc -Drealloc=cj_t_realloc $cobjs $ENG_LIBS || return 1
    "$OBJDIR/test_http_cookiejar_api" ;;
  test-net-interrupt-api)
    [ -n "$ENG_LIB_OBJS" ] || { echo "FAIL: test-net-interrupt-api needs the configured build objects"; return 1; }
    local nobjs="" no nskip=" $OBJDIR/dyna-cli.o $OBJDIR/dyna-repl.o "
    for no in $ENG_LIB_OBJS $ENG_NAT_OBJS; do
      case "$nskip" in *" $OBJDIR/$no.o "*) continue ;; esac
      nobjs="$nobjs $OBJDIR/$no.o"
    done
    # shellcheck disable=SC2086
    $ENG_CC $ENG_CFLAGS -Isrc $ENG_LDFLAGS $ENG_LDEXPORT -o "$OBJDIR/test_net_interrupt_api" tests/net/test_net_interrupt_api.c $nobjs $ENG_LIBS || return 1
    "$OBJDIR/test_net_interrupt_api" ;;
  test-module-isweep)
    [ -n "$ENG_LIB_OBJS" ] || { echo "FAIL: test-module-isweep needs the configured build objects"; return 1; }
    local mobjs="" mo mskip=" $OBJDIR/dyna-cli.o $OBJDIR/dyna-repl.o "
    for mo in $ENG_LIB_OBJS $ENG_NAT_OBJS; do
      case "$mskip" in *" $OBJDIR/$mo.o "*) continue ;; esac
      mobjs="$mobjs $OBJDIR/$mo.o"
    done
    # shellcheck disable=SC2086
    $ENG_CC $ENG_CFLAGS -Isrc $ENG_LDFLAGS $ENG_LDEXPORT -o "$OBJDIR/test_module_isweep" tests/core/test_module_isweep.c $mobjs $ENG_LIBS || return 1
    isweep_sharded "$OBJDIR/test_module_isweep" ;;
  test-async-isweep)
    [ -n "$ENG_LIB_OBJS" ] || { echo "FAIL: test-async-isweep needs the configured build objects"; return 1; }
    local aobjs="" ao askip=" $OBJDIR/dyna-cli.o $OBJDIR/dyna-repl.o "
    for ao in $ENG_LIB_OBJS $ENG_NAT_OBJS; do
      case "$askip" in *" $OBJDIR/$ao.o "*) continue ;; esac
      aobjs="$aobjs $OBJDIR/$ao.o"
    done
    # shellcheck disable=SC2086
    $ENG_CC $ENG_CFLAGS -Isrc $ENG_LDFLAGS $ENG_LDEXPORT -o "$OBJDIR/test_async_isweep" tests/async/test_async_isweep.c $aobjs $ENG_LIBS || return 1
    isweep_sharded "$OBJDIR/test_async_isweep" ;;
  test-stream-isweep)
    [ -n "$ENG_LIB_OBJS" ] || { echo "FAIL: test-stream-isweep needs the configured build objects"; return 1; }
    local wobjs="" wo wskip=" $OBJDIR/dyna-cli.o $OBJDIR/dyna-repl.o "
    for wo in $ENG_LIB_OBJS $ENG_NAT_OBJS; do
      case "$wskip" in *" $OBJDIR/$wo.o "*) continue ;; esac
      wobjs="$wobjs $OBJDIR/$wo.o"
    done
    # shellcheck disable=SC2086
    $ENG_CC $ENG_CFLAGS -Isrc $ENG_LDFLAGS $ENG_LDEXPORT -o "$OBJDIR/test_stream_isweep" tests/stream/test_stream_isweep.c $wobjs $ENG_LIBS || return 1
    isweep_sharded "$OBJDIR/test_stream_isweep" ;;
  test-exception-lost)
    mkdir -p .obj
    _ctest_engine_san_objs || return 1
    # shellcheck disable=SC2086
    $(reg_cc) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
      -std=gnu17 -D_GNU_SOURCE -DCONFIG_VERSION=\"x\" -I. -Isrc -Isrc/core \
      tests/core/test_exception_lost.c $PAR_CC_OBJS \
      -lm -lpthread -ldl -o .obj/test_exception_lost || return 1
    ./.obj/test_exception_lost ;;
  test-oom-error-object)
    mkdir -p .obj
    _ctest_engine_san_objs || return 1
    # shellcheck disable=SC2086
    $(reg_cc) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
      -std=gnu17 -D_GNU_SOURCE -DCONFIG_VERSION=\"x\" -I. -Isrc -Isrc/core \
      tests/core/test_oom_error_object.c $PAR_CC_OBJS \
      -lm -lpthread -ldl -o .obj/test_oom_error_object || return 1
    ./.obj/test_oom_error_object ;;
  test-postgres-copy-api)
    [ -n "$ENG_LIB_OBJS" ] || { echo "FAIL: test-postgres-copy-api needs the configured build objects"; return 1; }
    local pobjs="" po pskip=" $OBJDIR/dyna-cli.o $OBJDIR/dyna-repl.o $OBJDIR/repl.o $OBJDIR/modules/postgres/postgres-copy.o "
    for po in $ENG_LIB_OBJS $ENG_NAT_OBJS; do
      case "$pskip" in *" $OBJDIR/$po.o "*) continue ;; esac
      pobjs="$pobjs $OBJDIR/$po.o"
    done
    # shellcheck disable=SC2086
    $ENG_CC $ENG_CFLAGS -Isrc $ENG_LDFLAGS $ENG_LDEXPORT -o "$OBJDIR/test_postgres_copy_api" tests/postgres/test_postgres_copy_api.c src/modules/postgres/postgres-copy.c -Dmalloc=pc_t_malloc -Dcalloc=pc_t_calloc -Drealloc=pc_t_realloc $pobjs $ENG_LIBS || return 1
    "$OBJDIR/test_postgres_copy_api" ;;
  test-postgres-cancel-api)
    [ -n "$ENG_LIB_OBJS" ] || { echo "FAIL: test-postgres-cancel-api needs the configured build objects"; return 1; }
    local kobjs="" ko kskip=" $OBJDIR/dyna-cli.o $OBJDIR/dyna-repl.o $OBJDIR/repl.o $OBJDIR/modules/postgres/postgres-cancel.o "
    for ko in $ENG_LIB_OBJS $ENG_NAT_OBJS; do
      case "$kskip" in *" $OBJDIR/$ko.o "*) continue ;; esac
      kobjs="$kobjs $OBJDIR/$ko.o"
    done
    # shellcheck disable=SC2086
    $ENG_CC $ENG_CFLAGS -Isrc $ENG_LDFLAGS $ENG_LDEXPORT -o "$OBJDIR/test_postgres_cancel_api" tests/postgres/test_postgres_cancel_api.c src/modules/postgres/postgres-cancel.c -Dmalloc=pk_t_malloc -Dcalloc=pk_t_calloc -Drealloc=pk_t_realloc -Dstrdup=pk_t_strdup -Dfree=pk_t_free -Ddyn_net_reactor_acquire=pk_t_reactor $kobjs $ENG_LIBS || return 1
    "$OBJDIR/test_postgres_cancel_api" ;;
  test-postgres-conninfo-api)
    [ -n "$ENG_LIB_OBJS" ] || { echo "FAIL: test-postgres-conninfo-api needs the configured build objects"; return 1; }
    local iobjs="" io iskip=" $OBJDIR/dyna-cli.o $OBJDIR/dyna-repl.o $OBJDIR/repl.o $OBJDIR/modules/postgres/postgres-module.o $OBJDIR/modules/postgres/postgres-conninfo.o $OBJDIR/modules/postgres/postgres-protocol.o $OBJDIR/modules/postgres/postgres-query.o $OBJDIR/modules/postgres/postgres-query-queue.o $OBJDIR/modules/postgres/postgres-pipeline.o $OBJDIR/modules/postgres/postgres-copy.o $OBJDIR/modules/postgres/postgres-cancel.o $OBJDIR/modules/postgres/postgres-cps.o $OBJDIR/modules/postgres/postgres-statement-cache.o "
    for io in $ENG_LIB_OBJS $ENG_NAT_OBJS; do
      case "$iskip" in *" $OBJDIR/$io.o "*) continue ;; esac
      iobjs="$iobjs $OBJDIR/$io.o"
    done
    # shellcheck disable=SC2086
    $ENG_CC $ENG_CFLAGS -Isrc $ENG_LDFLAGS $ENG_LDEXPORT -o "$OBJDIR/test_postgres_conninfo_api" tests/postgres/test_postgres_conninfo_api.c src/modules/postgres/postgres-module.c src/modules/postgres/postgres-conninfo.c src/modules/postgres/postgres-protocol.c src/modules/postgres/postgres-query.c src/modules/postgres/postgres-query-queue.c src/modules/postgres/postgres-pipeline.c src/modules/postgres/postgres-copy.c src/modules/postgres/postgres-cancel.c src/modules/postgres/postgres-cps.c src/modules/postgres/postgres-statement-cache.c -Dmalloc=ci_t_malloc -Dcalloc=ci_t_calloc -Drealloc=ci_t_realloc -Dstrdup=ci_t_strdup -Dfree=ci_t_free $iobjs $ENG_LIBS || return 1
    "$OBJDIR/test_postgres_conninfo_api" ;;
  test-postgres-scram-api)
    [ -n "$ENG_LIB_OBJS" ] || { echo "FAIL: test-postgres-scram-api needs the configured build objects"; return 1; }
    local sobjs="" so sskip=" $OBJDIR/dyna-cli.o $OBJDIR/dyna-repl.o $OBJDIR/repl.o $OBJDIR/modules/postgres/postgres-protocol.o "
    for so in $ENG_LIB_OBJS $ENG_NAT_OBJS; do
      case "$sskip" in *" $OBJDIR/$so.o "*) continue ;; esac
      sobjs="$sobjs $OBJDIR/$so.o"
    done
    # shellcheck disable=SC2086
    $ENG_CC $ENG_CFLAGS -Isrc $ENG_LDFLAGS $ENG_LDEXPORT -o "$OBJDIR/test_postgres_scram_api" tests/postgres/test_postgres_scram_api.c src/modules/postgres/postgres-protocol.c -Dmalloc=ps_t_malloc -Dcalloc=ps_t_calloc -Drealloc=ps_t_realloc -Dstrdup=ps_t_strdup -Dfree=ps_t_free -Ddyn_aio_offload_cpu=ps_t_offload $sobjs $ENG_LIBS || return 1
    "$OBJDIR/test_postgres_scram_api" ;;
  test-tls-end-point)
    [ -n "$ENG_LIB_OBJS" ] || { echo "FAIL: test-tls-end-point needs the configured build objects"; return 1; }
    local tobjs="" to
    for to in $ENG_NAT_OBJS; do tobjs="$tobjs $OBJDIR/$to.o"; done
    # shellcheck disable=SC2086
    $ENG_CC -O1 -g $ENG_CFLAGS -Isrc -o "$OBJDIR/test_tls_end_point" tests/net/test_tls_end_point.c $tobjs $ENG_LIBA $ENG_LDFLAGS $ENG_LIBS ${ENG_SQLITE_LIBS:-} || return 1
    OPENSSL="${OPENSSL:-$(command -v /opt/homebrew/bin/openssl || command -v openssl)}" "$OBJDIR/test_tls_end_point" ;;
  test-smtp-net-api)
    [ -n "$ENG_LIB_OBJS" ] || { echo "FAIL: test-smtp-net-api needs the configured build objects"; return 1; }
    local sobjs="" so sskip=" $OBJDIR/dyna-cli.o $OBJDIR/dyna-repl.o $OBJDIR/modules/smtp/smtp-module.o $OBJDIR/modules/smtp/smtp-sopts.o $OBJDIR/modules/smtp/smtp-server.o $OBJDIR/modules/net-tcp/net-tcp-module.o "
    for so in $ENG_LIB_OBJS $ENG_NAT_OBJS; do
      case "$sskip" in *" $OBJDIR/$so.o "*) continue ;; esac
      sobjs="$sobjs $OBJDIR/$so.o"
    done
    # shellcheck disable=SC2086
    $ENG_CC $ENG_CFLAGS -Isrc $ENG_LDFLAGS $ENG_LDEXPORT -o "$OBJDIR/test_smtp_net_api" tests/smtp/test_smtp_net_api.c src/modules/smtp/smtp-module.c src/modules/smtp/smtp-sopts.c src/modules/smtp/smtp-server.c src/modules/net-tcp/net-tcp-module.c -Dmalloc=sn_t_malloc -Dcalloc=sn_t_calloc -Drealloc=sn_t_realloc -Dstrdup=sn_t_strdup $sobjs $ENG_LIBS || return 1
    "$OBJDIR/test_smtp_net_api" ;;
  test-exit-teardown)
    mkdir -p .obj
    _ctest_engine_rel_objs || return 1
    # shellcheck disable=SC2086
    $(reg_cc) -g -O1 -fno-omit-frame-pointer \
      -std=gnu17 -D_GNU_SOURCE -DCONFIG_VERSION=\"x\" -I. -Isrc -Isrc/core \
      tests/core/test_exit_teardown.c $PAR_CC_OBJS \
      -lm -lpthread -ldl -o .obj/test_exit_teardown || return 1
    ./.obj/test_exit_teardown ;;
  test-backtrace-oom)
    mkdir -p .obj
    _ctest_engine_san_objs || return 1
    # shellcheck disable=SC2086
    $(reg_cc) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
      -std=gnu17 -D_GNU_SOURCE -DCONFIG_VERSION=\"x\" -I. -Isrc -Isrc/core \
      tests/core/test_backtrace_oom.c $PAR_CC_OBJS \
      -lm -lpthread -ldl -o .obj/test_backtrace_oom || return 1
    ./.obj/test_backtrace_oom ;;
  test-aio-sendfile-busy)
    mkdir -p .obj
    $(reg_cc) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
      -std=gnu17 -D_GNU_SOURCE -DCONFIG_NATIVE_MODULES -Isrc -Isrc/core \
      tests/net/test_aio_sendfile_busy.c src/dyna-aio.c src/dyna-evloop.c \
      src/dyna-io.c src/core/dyn-pool.c src/core/dyn-timer.c src/cutils.c -lpthread \
      -o .obj/test_aio_sendfile_busy || return 1
    ./.obj/test_aio_sendfile_busy ;;
  test-aio-hold-flush-order)
    mkdir -p .obj
    $(reg_cc) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
      -std=gnu17 -D_GNU_SOURCE -DCONFIG_NATIVE_MODULES -Isrc -Isrc/core \
      tests/net/test_aio_hold_flush_order.c -o .obj/test_aio_hold_flush_order || return 1
    ./.obj/test_aio_hold_flush_order ;;
  test-aio-hold-pool)
    mkdir -p .obj
    $(reg_cc) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
      -std=gnu17 -D_GNU_SOURCE -DCONFIG_NATIVE_MODULES -Isrc -Isrc/core \
      tests/net/test_aio_hold_pool.c -o .obj/test_aio_hold_pool || return 1
    ./.obj/test_aio_hold_pool ;;
  test-aio-udp-mmsg)
    mkdir -p .obj
    $(reg_cc) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
      -std=gnu17 -D_GNU_SOURCE -DCONFIG_NATIVE_MODULES -Isrc -Isrc/core \
      tests/net/test_aio_udp_mmsg.c src/dyna-evloop.c \
      src/dyna-io.c src/core/dyn-pool.c src/core/dyn-timer.c src/cutils.c -lpthread \
      -o .obj/test_aio_udp_mmsg || return 1
    ./.obj/test_aio_udp_mmsg ;;
  test-udp-batch)
    mkdir -p .obj
    $(reg_cc) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
      -std=gnu17 -D_GNU_SOURCE -DCONFIG_NATIVE_MODULES -Isrc -Isrc/core \
      tests/net/test_udp_batch.c src/dyna-udp.c src/dyna-aio.c src/dyna-evloop.c \
      src/dyna-io.c src/core/dyn-pool.c src/core/dyn-timer.c src/cutils.c -lpthread \
      -o .obj/test_udp_batch || return 1
    ./.obj/test_udp_batch ;;
  test-aio-cpu-offload)
    mkdir -p .obj
    $(reg_cc) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
      -std=gnu17 -D_GNU_SOURCE -DCONFIG_NATIVE_MODULES -Isrc -Isrc/core \
      tests/net/test_aio_cpu_offload.c src/dyna-aio.c src/dyna-evloop.c \
      src/dyna-io.c src/core/dyn-pool.c src/core/dyn-timer.c src/cutils.c -lpthread \
      -o .obj/test_aio_cpu_offload || return 1
    ./.obj/test_aio_cpu_offload ;;
  test-aio-connect-offload)
    mkdir -p .obj
    $(reg_cc) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
      -std=gnu17 -D_GNU_SOURCE -DCONFIG_NATIVE_MODULES -Isrc -Isrc/core \
      tests/net/test_aio_connect_offload.c src/dyna-aio.c src/dyna-evloop.c \
      src/dyna-io.c src/core/dyn-pool.c src/core/dyn-timer.c src/cutils.c -lpthread \
      -o .obj/test_aio_connect_offload || return 1
    ./.obj/test_aio_connect_offload ;;
  test-dns-foreign-answers)
    mkdir -p .obj
    $(reg_cc) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
      -std=gnu17 -D_GNU_SOURCE -DCONFIG_NATIVE_MODULES \
      -DCONFIG_NATIVE_MODULE_DNS -I. -Isrc -Isrc/core \
      -Isrc/modules/dns tests/dns/test_dns_foreign_answers.c src/dyna-aio.c src/dyna-evloop.c \
      src/dyna-io.c src/core/dyn-pool.c src/core/dyn-timer.c \
      src/core/dyn-prng.c src/core/dyn-dns.c src/cutils.c -lpthread \
      -o .obj/test_dns_foreign_answers || return 1
    ./.obj/test_dns_foreign_answers ;;
  test-aio-disk)
    mkdir -p .obj
    $(reg_cc) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
      -std=gnu17 -D_GNU_SOURCE -DCONFIG_NATIVE_MODULES -Isrc -Isrc/core \
      tests/net/test_aio_disk.c src/dyna-aio.c src/dyna-evloop.c \
      src/dyna-io.c src/core/dyn-pool.c src/core/dyn-timer.c src/cutils.c -lpthread -o .obj/test_aio_disk || return 1
    ./.obj/test_aio_disk || return 1
    $(reg_cc) -g -O1 -fsanitize=thread -fno-omit-frame-pointer \
      -std=gnu17 -D_GNU_SOURCE -DCONFIG_NATIVE_MODULES -Isrc -Isrc/core \
      tests/net/test_aio_disk.c src/dyna-aio.c src/dyna-evloop.c \
      src/dyna-io.c src/core/dyn-pool.c src/core/dyn-timer.c src/cutils.c -lpthread -o .obj/test_aio_disk_tsan || return 1
    ./.obj/test_aio_disk_tsan ;;
  test-exec-spawn)
    mkdir -p .obj
    { \
      echo '#include <errno.h>'; \
      echo '#include <stdlib.h>'; \
      echo '#include <fcntl.h>'; \
      echo '#include <limits.h>'; \
      echo '#include <signal.h>'; \
      echo '#include <spawn.h>'; \
      echo '#include <stddef.h>'; \
      echo '#include <string.h>'; \
      echo '#include <sys/types.h>'; \
      echo '#include <sys/wait.h>'; \
      echo '#include <unistd.h>'; \
      echo '#include "dyna-io.h"'; \
      sed -n '/^#define DYN_SPAWN_EXTRACT_BEGIN/,/^#define DYN_SPAWN_EXTRACT_END/p' \
        $(ls src/dyna-libc.c src/modules/libc/libc-spawn.c 2>/dev/null); } \
      | sed -e '/^#define DYN_SPAWN_EXTRACT/d' \
            -e 's/^static int os_exec_spawn_child(/int os_exec_spawn_child(/' \
      > .obj/spawn_impl.c
    $(reg_cc) -g -O1 -std=gnu17 -D_GNU_SOURCE -Wno-deprecated-declarations -Isrc \
      tests/core/test_exec_spawn.c .obj/spawn_impl.c src/dyna-io.c src/cutils.c -lpthread \
      -o .obj/test_exec_spawn || return 1
    ./.obj/test_exec_spawn ;;
  test-uring-sendfile)
    _docker_uring test-uring-sendfile tests/net/test_uring_sendfile.c test_uring_sendfile ;;
  test-uring-connect-offload)
    _docker_uring test-uring-connect-offload tests/net/test_aio_connect_offload.c test_uring_connect ;;
  test-uring-tick)
    _docker_uring test-uring-tick tests/net/test_aio_uring_timer.c test_uring_tick ;;
  test-uring-taskrun)
    if [ "$(uname -s)" != Linux ] || [ ! -f /usr/include/liburing.h ]; then
      echo "SKIP (not run here): test-uring-taskrun needs Linux with liburing headers (run tests/ctest/run.sh test-uring-taskrun on such a host or container)"
      return 0
    fi
    mkdir -p .obj
    $(reg_cc) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
      -std=gnu17 -D_GNU_SOURCE -DCONFIG_NATIVE_MODULES -DCONFIG_IO_URING -Isrc -Isrc/core \
      tests/net/test_aio_uring_taskrun.c src/dyna-evloop.c src/dyna-io.c src/core/dyn-pool.c src/core/dyn-timer.c src/cutils.c \
      -lpthread -luring -o .obj/test_aio_uring_taskrun || return 1
    ./.obj/test_aio_uring_taskrun ;;
  test-uring-one-enter)
    if [ "$(uname -s)" != Linux ] || [ ! -f /usr/include/liburing.h ]; then
      echo "SKIP (not run here): test-uring-one-enter needs Linux with liburing headers (run tests/ctest/run.sh test-uring-one-enter on such a host or container)"
      return 0
    fi
    mkdir -p .obj
    $(reg_cc) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
      -std=gnu17 -D_GNU_SOURCE -DCONFIG_NATIVE_MODULES -DCONFIG_IO_URING -Isrc -Isrc/core \
      tests/net/test_aio_uring_one_enter.c src/dyna-evloop.c src/dyna-io.c src/core/dyn-pool.c src/core/dyn-timer.c src/cutils.c \
      -lpthread -luring -o .obj/test_aio_uring_one_enter || return 1
    ./.obj/test_aio_uring_one_enter ;;
  test-inflate-stream)
    mkdir -p .obj
    printf "#include <zlib.h>\nint main(void){return 0;}\n" | $(reg_cc) -x c - -lz -o .obj/zlib_probe 2>/dev/null || { echo "SKIP: no zlib on this host"; return 0; }
    $(reg_cc) -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
      -Wall -Wextra -Werror -std=gnu17 -Isrc/core tests/stream/test_inflate_stream_zlib.c src/core/dyn-inflate-stream.c src/core/dyn-hash.c $SIMD_SRCS -lz -lm \
      -o .obj/test_inflate_stream_zlib || return 1
    ./.obj/test_inflate_stream_zlib ;;
  test-crc32c-hw)
    mkdir -p .obj
    $(reg_cc) -O2 -std=gnu17 -Isrc/core -Isrc -Wall -Wextra \
      tests/crypto/test_crc32c_hw.c src/core/dyn-hash.c $SIMD_SRCS -lm \
      -o .obj/test_crc32c_hw || return 1
    ./.obj/test_crc32c_hw || return 1
    $(reg_cc) -O2 -std=gnu17 -Isrc/core -Isrc -Wall -Wextra "-DDYN_CRC32C_HW_MIN=((size_t)-1)" \
      tests/crypto/test_crc32c_hw.c src/core/dyn-hash.c $SIMD_SRCS -lm \
      -o .obj/test_crc32c_hw_slice8 || return 1
    ./.obj/test_crc32c_hw_slice8 || return 1
    if [ "$(uname -s)" = Darwin ] && [ "$(uname -m)" = arm64 ] && arch -x86_64 /usr/bin/true 2>/dev/null; then
      $(reg_cc) -arch x86_64 -O2 -std=gnu17 -Isrc/core -Isrc -Wall -Wextra \
        tests/crypto/test_crc32c_hw.c src/core/dyn-hash.c $SIMD_SRCS -lm \
        -o .obj/test_crc32c_hw_x86 || return 1
      arch -x86_64 ./.obj/test_crc32c_hw_x86
    fi ;;
  test-crc32c-race)
    mkdir -p .obj
    $(reg_cc) -O1 -g -std=gnu17 -fsanitize=thread -fno-omit-frame-pointer \
      -Isrc/core -Isrc -Wall -Wextra \
      tests/crypto/test_crc32c_race.c src/core/dyn-hash.c $SIMD_SRCS -lm \
      -o .obj/test_crc32c_race || return 1
    ./.obj/test_crc32c_race ;;
  test-xalloc)
    mkdir -p .obj
    $(reg_cc) -g -O1 -std=gnu17 -Wall -Wextra -Werror tests/core/test_xalloc.c -o .obj/test_xalloc || return 1
    ./.obj/test_xalloc ;;
  test-snappend)
    mkdir -p .obj
    $(reg_cc) -g -O1 -std=c17 -pedantic -Wall -Wextra -Werror -Wformat=2 tests/core/test_snappend.c -o .obj/test_snappend || return 1
    ./.obj/test_snappend ;;
  test-crc32-hw)
    mkdir -p .obj
    $(reg_cc) -O2 -std=gnu17 -Isrc/core -Isrc -Wall -Wextra \
      tests/crypto/test_crc32_hw.c src/core/dyn-hash.c $SIMD_SRCS -lm \
      -o .obj/test_crc32_hw || return 1
    ./.obj/test_crc32_hw || return 1
    $(reg_cc) -O2 -std=gnu17 -Isrc/core -Isrc -Wall -Wextra "-DDYN_CRC32C_HW_MIN=((size_t)-1)" \
      tests/crypto/test_crc32_hw.c src/core/dyn-hash.c $SIMD_SRCS -lm \
      -o .obj/test_crc32_hw_slice8 || return 1
    ./.obj/test_crc32_hw_slice8 || return 1
    if [ "$(uname -s)" = Darwin ] && [ "$(uname -m)" = arm64 ] && arch -x86_64 /usr/bin/true 2>/dev/null; then
      $(reg_cc) -arch x86_64 -O2 -std=gnu17 -Isrc/core -Isrc -Wall -Wextra \
        tests/crypto/test_crc32_hw.c src/core/dyn-hash.c $SIMD_SRCS -lm \
        -o .obj/test_crc32_hw_x86 || return 1
      arch -x86_64 ./.obj/test_crc32_hw_x86
    fi ;;
  test-crc32-race)
    mkdir -p .obj
    $(reg_cc) -O1 -g -std=gnu17 -fsanitize=thread -fno-omit-frame-pointer \
      -Isrc/core -Isrc -Wall -Wextra \
      tests/crypto/test_crc32_race.c src/core/dyn-hash.c $SIMD_SRCS -lm \
      -o .obj/test_crc32_race || return 1
    ./.obj/test_crc32_race ;;
  test-sha256-hw)
    mkdir -p .obj
    $(reg_cc) -O2 -std=gnu17 -Isrc/core -Isrc -Wall -Wextra \
      tests/crypto/test_sha256_hw.c src/core/dyn-hash.c $SIMD_SRCS -lm \
      -o .obj/test_sha256_hw || return 1
    $(reg_cc) -O2 -std=gnu17 -Isrc/core -Isrc -Wall -Wextra -DDYN_SHA256_NO_HW=1 \
      tests/crypto/test_sha256_hw.c src/core/dyn-hash.c $SIMD_SRCS -lm \
      -o .obj/test_sha256_sw || return 1
    echo "  hardware:"; ./.obj/test_sha256_hw || return 1
    echo "  scalar:  "; ./.obj/test_sha256_sw ;;
  test-aio-tls)
    # links the CONFIGURED engine (its recipe took the build config's CFLAGS,
    # NAT_MODULE_OBJS and libdynajs.a), so the objects must exist and be fresh
    [ -n "$ENG_LIB_OBJS" ] || { echo "FAIL: test-aio-tls needs the configured build objects"; return 1; }
    mkdir -p .obj
    local objs=""
    local o
    for o in $ENG_NAT_OBJS; do objs="$objs $OBJDIR/$o.o"; done
    # shellcheck disable=SC2086
    $ENG_CC -O1 -g $ENG_CFLAGS -Isrc -o .obj/test_aio_tls tests/net/test_aio_tls.c \
      $objs $ENG_LIBA $ENG_LDFLAGS $ENG_LIBS ${ENG_SQLITE_LIBS:-} || return 1
    ./.obj/test_aio_tls ;;
  test-webrtc-media-e2e)
    # W6 media origin: links the engine (minus the CLI/REPL entry objects) into tests/webrtc/webrtc_media_harness.c
    # and runs tests/webrtc/harness_media_e2e.js under it; SKIP when the build has no webrtc module
    [ -d "$OBJDIR/modules/webrtc" ] || { echo "SKIP: test-webrtc-media-e2e needs CONFIG_NATIVE_MODULE_WEBRTC"; exit 0; }
    local hobjs="" ho hskip=" $OBJDIR/dyna-cli.o $OBJDIR/dyna-repl.o "
    for ho in $ENG_LIB_OBJS $ENG_NAT_OBJS; do
      case "$hskip" in *" $OBJDIR/$ho.o "*) continue ;; esac
      hobjs="$hobjs $OBJDIR/$ho.o"
    done
    # shellcheck disable=SC2086
    $ENG_CC $ENG_CFLAGS -Isrc $ENG_LDFLAGS $ENG_LDEXPORT -o "$OBJDIR/webrtc_media_harness" tests/webrtc/webrtc_media_harness.c $hobjs $ENG_LIBS || return 1
    rm -f .obj/.test-webrtc-media-e2e.bounded
    BOUNDED_RUN_MARKER=.obj/.test-webrtc-media-e2e.bounded ./tools/bounded-run.sh 180 60 test-webrtc-media-e2e -- "$OBJDIR/webrtc_media_harness" tests/webrtc/harness_media_e2e.js </dev/null ;;
  test-redis-final-sweep|test-postgres-final-sweep|test-dns-final-sweep|test-net-udp-grave|test-aio-echo|test-io-reactor)
    # links the engine minus the CLI/REPL entry objects, under the build config
    { [ -f "$OBJDIR/dyna-net.o" ] || [ -d "$OBJDIR/modules/net" ]; } || { echo "SKIP: $1 needs a CONFIG_NATIVE_MODULES=y build"; exit 1; }
    mkdir -p .obj
    local out exe srcs skip="" o bin   # skip MUST be initialized: set -u
    if [ "${1%-final-sweep}" != "$1" ]; then
      local fsm="${1#test-}"; fsm="${fsm%-final-sweep}"
      out="$OBJDIR/test_${fsm}_final_sweep"; srcs="tests/$fsm/test_${fsm}_final_sweep.c"; bin="$out"
    elif [ "$1" = test-aio-echo ] || [ "$1" = test-io-reactor ]; then
      out="$OBJDIR/$(printf '%s' "$1" | tr - _)"; srcs="$(_ctest_src "$(printf '%s' "$1" | tr - _)")"; bin="$out"
    else
      { [ -f "$OBJDIR/dyna-net-tcp.o" ] || [ -d "$OBJDIR/modules/net-tcp" ]; } || { echo "SKIP: test-net-udp-grave needs a CONFIG_NATIVE_MODULES=y build"; exit 1; }
      out="$OBJDIR/test_udp_grave_sweep"; srcs="tests/net/test_udp_grave_sweep.c"; bin="$out"
    fi
    objs=""
    for o in dyna-cli repl; do skip="$skip $OBJDIR/$o.o"; done
    for o in $ENG_LIB_OBJS $ENG_NAT_OBJS; do
      case " $skip " in *" $OBJDIR/$o.o "*) continue ;; esac
      objs="$objs $OBJDIR/$o.o"
    done
    # shellcheck disable=SC2086
    $ENG_CC $ENG_CFLAGS $ENG_LDFLAGS $ENG_LDEXPORT -o "$out" $srcs $objs $ENG_LIBS || return 1
    local marker=".obj/.${1}.bounded" leak=""
    [ "$1" = test-net-udp-grave ] && leak="ASAN_OPTIONS=detect_leaks=1 "
    rm -f .obj/."${1}".bounded
    # shellcheck disable=SC2086
    eval "${leak}BOUNDED_RUN_MARKER=$marker ./tools/bounded-run.sh 120 60 \"$1\" -- \"$bin\"" \
      > ".obj/${1}.out" 2>&1
    local rc=$?
    cat ".obj/${1}.out"
    if [ "$rc" -ne 0 ]; then
      if [ -f "$marker" ]; then
        rm -f "$marker"
        echo "TIMEOUT: $1 hit the external bound (exit $rc)"; exit "$rc"
      fi
      echo "FAIL: $1 exited $rc (a row failed, or a sanitizer reported -- see the rows above)"; exit 1
    fi
    rm -f "$marker"
    grep -q "fails=0" ".obj/${1}.out" || { echo "FAIL: $1 found a regression (see the rows above)"; exit 1; } ;;
  test-redis-fdchurn|test-postgres-fdchurn|test-net-udp-selfclose|test-net-teardown-churn|test-redis-teardown-churn|test-postgres-teardown-churn|test-dns-teardown-churn|test-net-eyeballs-churn)
    _require_native_bin
    case "$1" in
      test-redis-fdchurn|test-postgres-fdchurn)
        local fdm="${1#test-}"; fdm="${fdm%-fdchurn}"; local fdjs
        case "$fdm" in redis) fdjs=tests/redis/test_redis_redis_fdchurn.js ;; *) fdjs=tests/postgres/test_postgres_postgre_sql_fdchurn.js ;; esac
        mkdir -p .obj; rm -f .obj/.$1.bounded
        BOUNDED_RUN_MARKER=.obj/.$1.bounded ./tools/test-slot.sh -- \
          ./tools/bounded-run.sh 120 60 "$1" -- \
          env FDJS="$fdjs" sh -c 'ulimit -n 64; exec "$DYNAJS" --std "$FDJS"' \
          > .obj/$1.out 2>&1
        local rc=$?
        cat .obj/$1.out
        if [ "$rc" -ne 0 ] && [ -f .obj/.$1.bounded ]; then
          rm -f .obj/.$1.bounded
          echo "TIMEOUT: $1 hit the external bound (exit $rc)"; exit "$rc"
        fi
        rm -f .obj/.$1.bounded
        grep -q "errors=0" .obj/$1.out \
          || { echo "FAIL: descriptors leak across the cycle (or a cycle stalled -- see the watchdog diagnostic above)"; exit 1; } ;;
      test-net-udp-selfclose)
        ./tools/test-slot.sh -- "$DYNAJS" --std tests/net/test_net_udp_socket_selfclose.js | tee /dev/stderr | \
          grep -q "fails=0" || { echo "FAIL: UDPSocket self-close reads freed state (see the scenario above)"; exit 1; } ;;
      test-net-teardown-churn|test-redis-teardown-churn|test-postgres-teardown-churn|test-dns-teardown-churn)
        local tdm="${1#test-}"; tdm="${tdm%-teardown-churn}"; local tds
        case "$tdm" in net) tds=test_net_tcp_server_teardown_churn ;; redis) tds=test_redis_redis_teardown_churn ;; postgres) tds=test_postgres_postgre_sql_teardown_churn ;; dns) tds=test_dns_dns_resolver_teardown_churn ;; esac
        ./tools/test-slot.sh -- "$DYNAJS" --std "tests/$tdm/$tds.js" 25 > ".obj/$1.out" 2>&1
        local rc=$?
        cat ".obj/$1.out" >&2
        [ "$rc" -eq 0 ] && grep -qE "^$tds: [1-9][0-9]* passed, 0 failed" ".obj/$1.out" \
          || { echo "FAIL: a teardown path leaked, crashed, or exited through the wrong door (exit $rc)"; exit 1; } ;;
      test-net-eyeballs-churn)
        ( ulimit -n 64; ./tools/test-slot.sh -- "$DYNAJS" tests/net/test_net_connect_happy_eyeballs.js 120 ) | tee /dev/stderr | \
          grep -q "errors=0" || { echo "FAIL: the race loser leaks a descriptor"; exit 1; } ;;
    esac ;;
  test-term-raw-sigterm)
    _require_native_bin
    mkdir -p .obj
    local rtlib=""; [ "$(uname -s)" = Darwin ] || rtlib=-lutil
    $(reg_cc) -g -O1 -std=gnu17 -D_GNU_SOURCE -Wall -Wextra tests/sys/test_sys_raw_mode_sigterm.c $rtlib -o .obj/test_sys_raw_mode_sigterm || return 1
    ./tools/test-slot.sh -- ./.obj/test_sys_raw_mode_sigterm "$DYNAJS"; local rtrc=$?
    [ $rtrc -eq 77 ] && { echo "SKIP: no pty on this host"; return 0; }
    [ $rtrc -eq 0 ] || { echo "FAIL: SIGTERM in raw mode left the terminal raw, a child group alive, or the wrong death signal"; return 1; } ;;
  test-net-hooks-leak)
    # F34: the ASan engine with detect_leaks=1 on tests/http/test_http_app_worker_drain_hooks_leak.js (three Workers
    # fetch, then their runtimes and io reactors are torn down). Any LeakSanitizer report naming
    # dyn_net_on_drain is a FAIL, whatever the exit code; so is a run without the script's ok line.
    [ -x .obj-san/dynajs ] || { echo "SKIP: test-net-hooks-leak needs the ASan engine (./build.sh san)"; return 0; }
    local hlog=.obj/ctest/test-net-hooks-leak.out hrc
    ASAN_OPTIONS=detect_leaks=1 LSAN_OPTIONS=suppressions=tests/asan_suppressions.txt \
      ./tools/test-slot.sh -- ./.obj-san/dynajs --std tests/http/test_http_app_worker_drain_hooks_leak.js > "$hlog" 2>&1; hrc=$?
    cat "$hlog"
    if grep -q "LeakSanitizer has encountered a fatal error\|detect_leaks is not supported" "$hlog"; then
      echo "SKIP: LeakSanitizer cannot run here (see above)"; return 0
    fi
    if grep -q "dyn_net_on_drain" "$hlog"; then
      echo "FAIL: the io reactor drain-hook array leaks per Worker thread (LeakSanitizer report above)"; return 1
    fi
    grep -q "^test_http_app_worker_drain_hooks_leak: ok" "$hlog" && grep -q " 0 failed" "$hlog" || { echo "FAIL: test_http_app_worker_drain_hooks_leak exit $hrc"; return 1; }
    [ "$hrc" = 0 ] || { echo "FAIL: test_http_app_worker_drain_hooks_leak exit $hrc under ASan (see the report above)"; return 1; } ;;
  tsan-workers)
    # The old gate's g_tsan_workers: a ThreadSanitizer engine (.obj-tsan/dynajs) built from the exact
    # per-file commands of the last ./build.sh release (.obj/release/rules.mk, object dir swapped, plus
    # -fsanitize=thread), then three Workers fetching at once, four Workers plus main running dyna:webrtc peer connections, and dyna:ml's thread-per-call suites.
    # A data race is a FAIL whatever the exit code.
    mkdir -p .obj/ctest .obj-tsan
    echo 'int main(void){return 0;}' | $(reg_cc) -fsanitize=thread -x c -o .obj/ctest/tsan-probe - 2>/dev/null \
      || { echo "SKIP: $(reg_cc) cannot build -fsanitize=thread programs on this host"; return 0; }
    [ -f .obj/release/rules.mk ] && [ -f .obj/release/flags.env ] && [ -f .obj/repl_src.inc ] \
      || { echo "FAIL: tsan-workers derives its build from .obj/release: run ./build.sh release first"; return 1; }
    . .obj/release/flags.env
    sed -e 's|\.obj/release|.obj-tsan|g' -e 's| -MMD | -fsanitize=thread -MMD |' .obj/release/rules.mk > .obj-tsan/rules.mk.new
    grep -q -- ' -fsanitize=thread -MMD ' .obj-tsan/rules.mk.new || { echo "FAIL: tsan-workers could not add -fsanitize=thread to the release compile rules"; return 1; }
    { [ "$(cat .obj-tsan/.sig 2>/dev/null)" = "$(cat .obj/release/.sig)" ] && cmp -s .obj-tsan/rules.mk.new .obj-tsan/rules.mk; } || { rm -rf .obj-tsan; mkdir -p .obj-tsan; cp .obj/release/.sig .obj-tsan/.sig; sed -e 's|\.obj/release|.obj-tsan|g' -e 's| -MMD | -fsanitize=thread -MMD |' .obj/release/rules.mk > .obj-tsan/rules.mk.new; }
    mv .obj-tsan/rules.mk.new .obj-tsan/rules.mk
    make -s -j"$NCPU" -f .obj-tsan/rules.mk all > .obj-tsan/build.log 2>&1 || { tail -20 .obj-tsan/build.log; echo "FAIL: the ThreadSanitizer build failed"; return 1; }
    # shellcheck disable=SC2086
    $CC $LDF -fsanitize=thread -o .obj-tsan/dynajs .obj-tsan/src/dyna-cli.o .obj-tsan/src/dyna-repl.o \
      $(sed 's|\.obj/release/|.obj-tsan/|g' .obj/release/objs.list) $LIBS || { echo "FAIL: the ThreadSanitizer link failed"; return 1; }
    nm .obj-tsan/dynajs | grep -q __tsan_init || { echo "FAIL: .obj-tsan/dynajs carries no ThreadSanitizer instrumentation"; return 1; }
    local tlog=.obj-tsan/tsan-workers.log trc
    TSAN_OPTIONS="halt_on_error=0 report_signal_unsafe=0" ./.obj-tsan/dynajs --std tests/core/tsan_worker_fetch.js > "$tlog" 2>&1; trc=$?
    if grep -q "WARNING: ThreadSanitizer" "$tlog"; then
      echo "FAIL: data race(s) with Workers fetching concurrently (full report: $tlog)"
      grep "SUMMARY: ThreadSanitizer" "$tlog" | sort | uniq -c | head -8; return 1
    fi
    [ "$trc" = 0 ] && grep -q "^tsan_worker_fetch: ok" "$tlog" || { echo "FAIL: tests/core/tsan_worker_fetch.js exit $trc"; tail -6 "$tlog"; return 1; }
    echo "tsan: Workers fetching concurrently are race-free ($(grep '^tsan_worker_fetch' "$tlog"))"
    tlog=.obj-tsan/tsan-worker-webrtc.log
    TSAN_OPTIONS="halt_on_error=0 report_signal_unsafe=0" ./.obj-tsan/dynajs --std tests/webrtc/tsan_worker_webrtc.js > "$tlog" 2>&1; trc=$?
    if grep -q "WARNING: ThreadSanitizer" "$tlog"; then
      echo "FAIL: data race(s) with Workers running dyna:webrtc peer connections (full report: $tlog)"
      grep "SUMMARY: ThreadSanitizer" "$tlog" | sort | uniq -c | head -8; return 1
    fi
    [ "$trc" = 0 ] && grep -q "^tsan_worker_webrtc: ok" "$tlog" || { echo "FAIL: tests/webrtc/tsan_worker_webrtc.js exit $trc"; tail -6 "$tlog"; return 1; }
    echo "tsan: Workers running peer connections are race-free ($(grep '^tsan_worker_webrtc' "$tlog"))"
    tlog=.obj-tsan/tsan-file-async.log
    TSAN_OPTIONS="halt_on_error=0 report_signal_unsafe=0" ./.obj-tsan/dynajs --std tests/file/test_file_async_rw.js > "$tlog" 2>&1; trc=$?
    if grep -q "WARNING: ThreadSanitizer" "$tlog"; then
      echo "FAIL: data race(s) with dyna:file.async requests on the I/O pool (full report: $tlog)"
      grep "SUMMARY: ThreadSanitizer" "$tlog" | sort | uniq -c | head -8; return 1
    fi
    [ "$trc" = 0 ] && grep -q "^test_file_async_rw: [0-9]* checks, 0 failed" "$tlog" || { echo "FAIL: tests/file/test_file_async_rw.js exit $trc under TSan"; tail -6 "$tlog"; return 1; }
    echo "tsan: dyna:file.async pool requests race-free ($(grep "^test_file_async_rw" "$tlog"))"
    # One process per suite: the old gate passed all four to one dynajs, which runs only the first.
    local mlf
    for mlf in tests/ml/test_ml_decision_tree_classifier_async.js tests/ml/test_ml_pipeline_async.js \
        tests/ml/test_ml_random_forest_classifier_async.js tests/ml/test_ml_thread_stats_async.js; do
      tlog=.obj-tsan/tsan-$(basename "$mlf" .js).log
      TSAN_OPTIONS="halt_on_error=0 report_signal_unsafe=0" ./.obj-tsan/dynajs --std "$mlf" > "$tlog" 2>&1; trc=$?
      if grep -q "WARNING: ThreadSanitizer\|SUMMARY: ThreadSanitizer" "$tlog"; then
        echo "FAIL: dyna:ml thread-per-call reported by ThreadSanitizer in $mlf (full report: $tlog)"
        grep "SUMMARY: ThreadSanitizer" "$tlog" | sort | uniq -c | head -8; return 1
      fi
      [ "$trc" = 0 ] && grep -q "^test_ml_async: [0-9]* passed, 0 failed" "$tlog" || { echo "FAIL: $mlf exit $trc under TSan"; tail -6 "$tlog"; return 1; }
      echo "tsan: $mlf race-free ($(grep '^test_ml_async' "$tlog"))"
    done ;;
  bench-core-tus)
    mkdir -p .obj
    $(reg_cc) -O2 -std=gnu17 -I src/core -I src -Wall -Wextra -Wno-unused-parameter \
      tests/core/bench_core.c $(core_lib_srcs ds) src/core/dyn-codec.c src/core/dyn-path.c \
      src/core/dyn-prng.c src/core/dyn-ac.c src/core/dyn-serial.c src/core/dyn-hash.c \
      src/core/dyn-mathx.c src/core/dyn-dict.c src/core/dyn-compress.c src/core/dyn-par.c src/core/dyn-pool.c \
      $SIMD_SRCS src/cutils.c -lm -lpthread -o .obj/bench_core || return 1
    ./.obj/bench_core ;;
  bench-ds-parr)
    mkdir -p .obj
    $(reg_cc) -O2 -DNDEBUG -std=gnu17 -Isrc/core -Itests/structures -Wall -Wextra -Wno-unused-function \
      tests/structures/bench_ds_parr.c -o .obj/bench_ds_parr || return 1
    ./.obj/bench_ds_parr "${BENCH_PARR_ARGS:-}" ;;
  bench-ac)
    mkdir -p .obj
    $(reg_cc) -O2 -std=gnu17 -Isrc/core -Wall -Wextra \
      tests/matcher/bench_ac.c src/core/dyn-ac.c -o .obj/bench_ac || return 1
    ./.obj/bench_ac ;;
  *)
    return 99 ;;
esac
}
now(){ printf '%s' "${EPOCHREALTIME:-$(date +%s)}"; }
isweep_sharded(){
  local bin=$1 k n=${ISWEEP_SHARDS:-$NCPU} rc=0 d
  d=$(mktemp -d "${TMPDIR:-/tmp}/isw.XXXXXX")
  for ((k = 0; k < n; k++)); do ISWEEP_SHARD="$k/$n" "$bin" > "$d/$k.out" 2>&1 & done
  for ((k = 0; k < n; k++)); do wait -n || rc=1; done
  for ((k = 0; k < n; k++)); do cat "$d/$k.out"; done
  rm -rf "$d"
  return $rc
}
one(){
  local t=$1 s rc
  if [ -z "${CTEST_FRESH:-}" ] && grep -qE "^$t	PASS	$CTEST_TREE	" "$LOGDIR/ledger.tsv" 2>/dev/null; then
    printf '%s %-5s %-28s %6s  (passed on tree %s; CTEST_FRESH=1 reruns)\n' "$(date +%T)" CACHE "$t" "-" "$CTEST_TREE"; dlog "$CTEST_MODE" CACHE "$t"; return 0
  fi
  dlog "$CTEST_MODE" START "$t"
  s=$(now)
  ( if on_list "$t" "$ENG_CTX_TARGETS"; then eng_ctx "$t" || exit 1; fi
    _c_regression_targets "$t" ) > "$LOGDIR/$t.log" 2>&1
  rc=$?
  local v=PASS; [ $rc -eq 0 ] || v=FAIL; [ $rc -eq 99 ] && v=UNKNOWN
  [ $rc -eq 0 ] && grep -q '^SKIP' "$LOGDIR/$t.log" && v=SKIP
  local why=""; [ $v = PASS ] || why="  $(grep -m1 '^SKIP\|^FAIL\|^TIMEOUT' "$LOGDIR/$t.log" || tail -1 "$LOGDIR/$t.log")"
  local secs; secs=$(awk -v a="$(now)" -v b="$s" 'BEGIN{printf "%.1f", a-b}')
  printf '%s\t%s\t%s\t%s\t%s\n' "$t" "$v" "$CTEST_TREE" "$secs" "$(date +%FT%T)" >> "$LOGDIR/ledger.tsv"
  dlog "$CTEST_MODE" END "$t" "$v" "${secs}s"
  printf '%s %-5s %-28s %6.1fs%s\n' "$(date +%T)" "$v" "$t" "$secs" "$why"
  [ $rc -eq 0 ]
}
build_so(){
  local n ld=""
  eng_ctx so || return 1
  [ "$(uname -s)" = Darwin ] && ld="-undefined dynamic_lookup"
  for n in bjson park_sweep; do
    $CC $CF -Wno-missing-prototypes -fPIC -DJS_SHARED_LIBRARY -c "tests/core/$n.c" -o ".obj/ctest/$n.pic.o" || return 1
    $CC $LDF -shared $ld -o "tests/$n.so" ".obj/ctest/$n.pic.o" || return 1
    echo "built tests/$n.so"
  done
}
mkdir -p .obj/ctest "$LOGDIR"
case "${1:-all}" in
  list) printf '%s\n' $TARGETS; exit 0 ;;
  so) build_so; exit $? ;;
  all) set -- $TARGETS ;;
esac
export -f $(declare -F | awk '{print $3}')
CTEST_TREE=$(bash tests/lib/tree-hash.sh src build.sh tests/lib tests/ctest --ext c,h tests)
export ROOT SRC_DIR NCPU ENG_CORE_LIBS DOCDB_CORE_SRCS SIMD_SRCS CTEST_TREE
DYNA_LOG=${DYNA_LOG:-$PWD/.agent-work/test.log}; CTEST_MODE=ctest; [ "$LOGDIR" = .obj/ctest-san ] && CTEST_MODE=ctest-san
export DYNA_LOG CTEST_MODE
dlog(){ local t=$EPOCHREALTIME IFS=$'\t'; local u=${t#*.}; printf '%(%F %T)T.%s\t%s\n' "${t%.*}" "${u:0:3}" "$*" >> "$DYNA_LOG"; }
export -f dlog
dlog "$CTEST_MODE" RUN "$# targets" "P=${P:-$NCPU}" "tree $CTEST_TREE"
SUMMARY=$LOGDIR/summary.$$.txt
printf '%s\n' "$@" | xargs -P "${P:-$NCPU}" -I{} bash -c 'one "$1"' _ {} | tee "$SUMMARY"
grep -qvE '^[0-9:]{8} (PASS|SKIP|CACHE) ' "$SUMMARY"; bad=$?
mv -f "$SUMMARY" "$LOGDIR/summary.txt"
[ $bad -ne 0 ]

