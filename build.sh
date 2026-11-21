#!/usr/bin/env bash
# ./build.sh [release|san|fuzz|all|clean]   (default all)
#   release: ./dynajs ./libdynajs.a from src/**/*.c (+ repl.js -> .obj/repl_src.inc, a C string the REPL evaluates), -Werror + probed hardening, parallel, incremental
#   san:     .obj-san/dynajs, the same sources under ASan+UBSan, proven instrumented
#   fuzz:    .obj-fuzz/bin/fuzz_* libFuzzer targets from src/fuzz/*.c over the same sources, ASan+UBSan+coverage (needs a clang with libFuzzer; FUZZ_CC, default Homebrew llvm);
#            a harness with "#define FZ_COVER_ENGINE" links libfuzzeng.a: coverage only on the JS engine, the rest ASan+UBSan (.obj-fuzz-nc)
# Env: J=jobs CC=compiler WERROR=0 IO_URING=1 SDL3=1 RAYLIB=1 UI=1 EXTRA_CFLAGS=... EXTRA_LDFLAGS=...
# No tests live here: the coding agent runs the suites a change affects (CLAUDE.md).
# With bash >= 5 and DYNA_LOG set or .agent-work present, logs START/END of the mode to that one log (default .agent-work/test.log).
set -euo pipefail
cd "$(dirname "$0")"
if [ "${BASH_VERSINFO[0]:-3}" -ge 5 ] && { [ -n "${DYNA_LOG:-}" ] || [ -d .agent-work ]; }; then
  DYNA_LOG=${DYNA_LOG:-$PWD/.agent-work/test.log}
  dlog(){ local t=$EPOCHREALTIME IFS=$'\t'; local u=${t#*.}; printf '%(%F %T)T.%s\t%s\n' "${t%.*}" "${u:0:3}" "$*" >> "$DYNA_LOG" 2>/dev/null || true; }
else
  dlog(){ :; }
fi
B0=$SECONDS; dlog build START "${1:-all}"
trap 'rc=$?; dlog build END "${1:-all}" "$([ $rc = 0 ] && echo PASS || echo FAIL rc=$rc)" "$((SECONDS-B0))s"' EXIT
OS=$(uname -s); ARCH=$(uname -m)
[ "$OS" = Darwin ] && export ZERO_AR_DATE=1
J=${J:-$( (sysctl -n hw.ncpu || nproc) 2>/dev/null)}
if [ "$OS" = Darwin ]; then CC=${CC:-clang}; else CC=${CC:-gcc}; fi
VERSION=$(cat VERSION)
DEP_SQLITE_MIN=3.54.1
DEP_OPENSSL_SERIES=3.5
DEP_SDL3_MIN=3.4.20
DEP_RAYLIB_MIN=6.2
BREWPC=""
pc(){ PKG_CONFIG_PATH="${PKG_CONFIG_PATH:-}:$BREWPC" pkg-config "${@:2}"; }
need(){ pc "$1" --atleast-version="$3" "$2" || { echo "build: needs $2 >= $3 via pkg-config (brew install ${1%@*})"; exit 1; }; }
opt(){ DEFS="$DEFS $2 $(pc "$1" --cflags "${@:3}" | sed 's/-I/-isystem /g')"; LIBS="$LIBS $(pc "$1" --libs "${@:3}")"; }

flags(){
  local probes p t fl= lf=
  t=$(mktemp -d)
  for p in openssl@3.5 sqlite zstd brotli sdl3 raylib; do (command -v brew >/dev/null && echo "$(brew --prefix --installed "$p" 2>/dev/null)/lib/pkgconfig" > "$t/$p") & done; wait
  for p in openssl@3.5 sqlite zstd brotli sdl3 raylib; do BREWPC="$BREWPC:$(cat "$t/$p" 2>/dev/null || true)"; done
  probes="-fstack-protector-strong -fstack-clash-protection -ftrivial-auto-var-init=zero -fno-delete-null-pointer-checks -fstrict-flex-arrays=2 -fzero-call-used-regs=used-gpr -Wshadow -Wcast-qual -Wpointer-arith -Wvla -Wformat=2 -Wformat-security -Wformat-overflow=2 -Wimplicit-fallthrough -Wstrict-prototypes -Wmissing-prototypes -Wdate-time -Wswitch-enum -Wdouble-promotion"
  [ "$ARCH" = x86_64 ] && probes="$probes -fcf-protection=full"
  case $ARCH in arm64|aarch64) probes="$probes -mbranch-protection=standard" ;; esac
  [ "$OS" = Linux ] && probes="$probes -Wl,-z,relro,-z,now -Wl,-z,noexecstack -Wl,--as-needed"
  for p in $probes; do (echo 'int main(void){return 0;}' | $CC -Werror $p -x c -o /dev/null - 2>/dev/null && touch "$t/ok$p") & done; wait
  for p in $probes; do [ -e "$t/ok$p" ] && case $p in -Wl*) lf="${lf:+$lf }$p" ;; *) fl="${fl:+$fl }$p" ;; esac; done
  WARN="-Wall -Wextra -Wno-sign-compare -Wno-missing-field-initializers -Wno-unused-parameter -Wundef -Wuninitialized -Wunused -Wwrite-strings -Wchar-subscripts"
  [ "${WERROR:-1}" = 0 ] || WARN="$WARN -Werror"
  case "$($CC --version)" in *clang*) WARN="$WARN -funsigned-char" ;; *) WARN="$WARN -Wno-array-bounds -Wno-format-truncation -Wno-infinite-recursion" ;; esac
  [ "$(pc openssl@3.5 --modversion openssl | cut -d. -f1-2)" = "$DEP_OPENSSL_SERIES" ] || { echo "build: needs OpenSSL $DEP_OPENSSL_SERIES.x via pkg-config (brew install openssl@3.5)"; exit 1; }
  need sqlite sqlite3 "$DEP_SQLITE_MIN"
  DEFS="-D_GNU_SOURCE -DCONFIG_VERSION=\"$VERSION\" -DCONFIG_PROP_HASH_MIX -DCONFIG_LITERAL_BOILERPLATE=1 -DCONFIG_FAST_PROP_TEARDOWN=1 -DCONFIG_NATIVE_MODULES"
  for m in random compress stream async net netip http dns proxy metrics sqlite postgres redis smtp structures ml simd file semver bytes crypto matcher encoding time mathx csv dataframe uuid otp config log url term validate json schema xml yaml decimal vserialize docdb html sys scrape oauth2 bench ai svg webrtc vec ssh sftp wireguard graph protobuf asn1 dkim; do
    { [ -f "src/dyna-$m.c" ] || [ -d "src/modules/$m" ]; } && DEFS="$DEFS -DCONFIG_NATIVE_MODULE_$(echo "$m" | tr a-z A-Z)"
  done
  $CC -o /dev/null src/compat/test-closefrom.c 2>/dev/null && DEFS="$DEFS -DHAVE_CLOSEFROM"
  [ "$OS-$ARCH" = Darwin-arm64 ] && DEFS="$DEFS -DCONFIG_SYSTEMLIBM"
  if $CC -O2 -Werror -o "$t/p" tools/tcprobe/caller.c tools/tcprobe/callee.c tools/tcprobe/ext.c 2>/dev/null && [ "$("$t/p")" = 48 ]; then DEFS="$DEFS -DCONFIG_TAILCALL_INTERP=1"; fi; rm -rf "$t"
  LIBS="-lm -lpthread -ldl"
  opt openssl@3.5 "-DCONFIG_OPENSSL -DCONFIG_TLS" openssl; opt sqlite -DCONFIG_SQLITE sqlite3
  if pc zstd --exists libzstd; then opt zstd -DCONFIG_ZSTD libzstd; fi
  if [ "$OS" = Darwin ]; then LIBS="$LIBS -lcompression"; elif pc brotli --exists libbrotlienc libbrotlidec; then opt brotli -DCONFIG_BROTLI libbrotlienc libbrotlidec; fi
  [ "${IO_URING:-0}" = 1 ] && { DEFS="$DEFS -DCONFIG_IO_URING"; LIBS="$LIBS -luring"; }
  [ "${SDL3:-0}" = 1 ] && { need sdl3 sdl3 "$DEP_SDL3_MIN"; opt sdl3 -DCONFIG_SDL3 sdl3; }
  [ "${RAYLIB:-0}" = 1 ] && { need raylib raylib "$DEP_RAYLIB_MIN"; opt raylib -DCONFIG_RAYLIB raylib; }
  [ "${UI:-0}" = 1 ] && { tools/fetch-ui-deps.sh --check >/dev/null || { echo "build: UI=1 needs third_party/ui: tools/fetch-ui-deps.sh"; exit 1; }; DEFS="$DEFS -DCONFIG_UI -isystem third_party/ui/stb -isystem third_party/ui/fonts"; }
  CF="-g -std=gnu17 -fwrapv -O2 $WARN $fl $DEFS -I. -Isrc ${EXTRA_CFLAGS:-}"
  LDF="-g -rdynamic $lf"
  [ -z "${EXTRA_LDFLAGS:-}" ] || LDF="$LDF $EXTRA_LDFLAGS"
}

sources(){
  local x=(! -path 'src/modules/sdl/*' ! -path 'src/modules/raylib/*' ! -path 'src/modules/ui/*' ! -name dyn-ui.c ! -name dyna-ui-sdl.c ! -name dyna-ui-raylib.c ! -name dyna-uring.c)
  [ "${IO_URING:-0}" = 1 ] && x=("${x[@]/dyna-uring.c/-}")
  [ "${SDL3:-0}" = 1 ] && x=("${x[@]/src\/modules\/sdl\/\*/-}")
  [ "${RAYLIB:-0}" = 1 ] && x=("${x[@]/src\/modules\/raylib\/\*/-}")
  [ "${UI:-0}" = 1 ] && { x=("${x[@]/src\/modules\/ui\/\*/-}"); x=("${x[@]/dyn-ui.c/-}"); }
  [ "${UI:-0}${SDL3:-0}" = 11 ] && x=("${x[@]/dyna-ui-sdl.c/-}")
  [ "${UI:-0}${RAYLIB:-0}" = 11 ] && x=("${x[@]/dyna-ui-raylib.c/-}")
  find src -name '*.c' ! -name '*.inc.c' ! -path 'src/compat/*' ! -path 'src/fuzz/*' ! -name dyna-repl.c ! -name dyna-cli.c "${x[@]}" | sort
}
track(){
  local n=${1#src/}
  case $n in modules/*/*) n=dyna-${n#modules/}; n=${n%%/*}.c ;; core/*/*) n=dyn-${n#core/}; n=${n%%/*}.c ;; *) n=${n##*/} ;; esac
  case $n in dyna-nat.c) ;; dyna-aio.c|dyna-aio-uring.c|dyna-evloop.c|dyn-pool.c|dyn-timer.c) echo "-DDYN_TRK_PLUMBING -include src/dyna-track.h" ;; dyna-*.c|dyn-*.c) echo "-include src/dyna-track.h" ;; esac
}

compile(){
  local O=$1 cf=$2 extra_main=$3 sig s mk srcs
  srcs=$(sources)
  sig=$( { $CC --version | head -1; echo "$cf $LDF $LIBS"; } | cksum)
  [ "$(cat "$O/.sig" 2>/dev/null)" = "$sig" ] || { rm -rf "$O"; mkdir -p "$O"; echo "$sig" > "$O/.sig"; }
  printf "CC=%q\nCF=%q\nLDF=%q\nLIBS=%q\n" "$CC" "$cf" "$LDF" "$LIBS" > "$O/flags.env"
  mk="$O/rules.mk"
  { printf 'all:'; for s in $srcs $extra_main; do printf ' %s/%s.o' "$O" "${s%.c}"; done; printf '\n-include $(shell find %s -name "*.d")\n' "$O"
    for s in $srcs $extra_main; do
      printf '%s/%s.o: %s\n\t@mkdir -p $(@D)\n\t@echo "  CC %s"\n\t@%s %s %s -MMD -MP -MF $@.d -c -o $@ $<\n' "$O" "${s%.c}" "$s" "$s" "$CC" "${cf//\"/\\\"}" "$(track "$s")"
    done; } > "$mk"
  make -s -j"$J" -f "$mk" all
  OBJS=$(for s in $srcs; do printf '%s/%s.o ' "$O" "${s%.c}"; done)
  [ "$(cat "$O/objs.list" 2>/dev/null)" = "$OBJS" ] || echo "$OBJS" > "$O/objs.list"
}

link(){ local log; log=$(mktemp); "$@" 2>"$log" || { cat "$log"; rm -f "$log"; exit 1; }; if grep -i 'warning:' "$log"; then rm -f "$log"; echo "build: link warnings are errors"; exit 1; fi; rm -f "$log"; }

repl_src(){ mkdir -p .obj; { echo 'static const char dyna_repl_src[] ='; sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/^/"/' -e 's/$/\\n"/' repl.js; echo ';'; } > .obj/repl_src.inc.new; if cmp -s .obj/repl_src.inc.new .obj/repl_src.inc; then rm -f .obj/repl_src.inc.new; else mv .obj/repl_src.inc.new .obj/repl_src.inc; fi; }

fresh(){ local out=$1; shift; [ -x "$out" ] && [ -z "$(find "$@" \( -name '*.o' -o -name '*.c' -o -name '*.js' -o -name objs.list \) -newer "$out" -print -quit)" ]; }

release(){
  local O=.obj/release
  repl_src
  local CF="$CF -D_FORTIFY_SOURCE=3 -I.obj"; compile "$O" "$CF" "src/dyna-cli.c src/dyna-repl.c"
  fresh dynajs "$O" && { echo "build: release up to date (./dynajs)"; return; }
  rm -f libdynajs.a; ar rcs libdynajs.a $OBJS
  link $CC $LDF -o dynajs "$O/src/dyna-cli.o" "$O/src/dyna-repl.o" $OBJS $LIBS
  ./dynajs -e 'import("dyna:mathx")' >/dev/null || { echo "build: ./dynajs cannot load dyna:* modules"; exit 1; }
  echo "build: release ok (./dynajs)"
}

san(){
  local O=.obj-san s="-fsanitize=address,undefined -fno-omit-frame-pointer"
  repl_src
  local LDF="$LDF $s"; compile "$O" "$CF $s -I.obj" "src/dyna-cli.c src/dyna-repl.c"
  fresh "$O/dynajs" "$O" && { echo "build: san up to date ($O/dynajs)"; return; }
  link $CC $LDF -o "$O/dynajs" "$O/src/dyna-cli.o" "$O/src/dyna-repl.o" $OBJS $LIBS
  s=$(nm "$O/dynajs"); grep -q __asan_init <<<"$s" && grep -q __ubsan_handle <<<"$s" || { echo "build: $O/dynajs is not instrumented"; exit 1; }
  ASAN_OPTIONS=detect_leaks=0 "$O/dynajs" -e 'import("dyna:csv")' >/dev/null || { echo "build: the sanitizer engine cannot run"; exit 1; }
  echo "build: san ok ($O/dynajs, ASan+UBSan)"
}

fuzz(){
  local O=.obj-fuzz N=.obj-fuzz-nc s="-fsanitize=fuzzer-no-link,address,undefined -fno-omit-frame-pointer" h n x lib ok=0 bad=0
  compile "$N" "$CF -fsanitize=address,undefined -fno-omit-frame-pointer" ""
  compile "$O" "$CF $s" src/fuzz/fuzz_common.c
  mkdir -p "$O/bin"
  if [ ! -f "$O/libfuzzdyn.a" ] || [ -n "$(find $OBJS "$O/src/fuzz/fuzz_common.o" -newer "$O/libfuzzdyn.a" -print -quit)" ]; then rm -f "$O/libfuzzdyn.a"; ar rcs "$O/libfuzzdyn.a" $OBJS "$O/src/fuzz/fuzz_common.o"; fi
  if [ ! -f "$O/libfuzzeng.a" ] || [ -n "$(find $OBJS $N "$O/src/fuzz/fuzz_common.o" -name "*.o" -newer "$O/libfuzzeng.a" -print -quit)" ]; then rm -f "$O/libfuzzeng.a"; ar rcs "$O/libfuzzeng.a" $(for x in $OBJS; do case ${x#$O/} in (src/dynajs.o|src/engine-*.o|src/builtins/*|src/libregexp.o|src/libunicode.o|src/dtoa.o|src/cutils.o) echo "$x" ;; (*) echo "$N/${x#$O/}" ;; esac; done) "$O/src/fuzz/fuzz_common.o"; fi
  for h in src/fuzz/fuzz_*.c; do
    n=$(basename "$h" .c); [ "$n" = fuzz_common ] && continue
    lib=libfuzzdyn.a; grep -q "^#define FZ_COVER_ENGINE" "$h" && lib=libfuzzeng.a
    if fresh "$O/bin/$n" "$O" "$N" src/fuzz; then ok=$((ok+1)); continue; fi
    if $CC $CF -Isrc/core $s -c -o "$O/$n.o" "$h" 2>"$O/$n.log" && $CC $LDF -fsanitize=fuzzer,address,undefined -o "$O/bin/$n" "$O/$n.o" "$O/$lib" $LIBS 2>>"$O/$n.log"; then ok=$((ok+1)); else bad=$((bad+1)); echo "build: fuzz $n failed ($O/$n.log)"; fi
  done
  echo "build: fuzz ok=$ok failed=$bad ($O/bin)"
  [ "$bad" = 0 ]
}

case "${1:-all}" in
  release) flags; release ;;
  san)     flags; san ;;
  fuzz)    CC=${FUZZ_CC:-$(brew --prefix llvm 2>/dev/null)/bin/clang}; WERROR=0; flags; fuzz ;;
  all)     flags; release; san ;;
  clean)   rm -rf .obj/release .obj-san .obj/repl_src.inc dynajs libdynajs.a ;;
  *)       echo "usage: ./build.sh [release|san|all|clean]"; exit 2 ;;
esac
