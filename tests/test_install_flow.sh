#!/usr/bin/env bash
# End-to-end test of install.sh's MAIN flow: preflight → clone → build →
# install → verify → report. Run TWICE so the upgrade path — a previous version
# being read and replaced — is exercised too; a clean-run test never sees it.
# Then --uninstall. Hermetic: git, make, clang, brew and rm are stubs, so
# nothing touches the network, a real compiler, Homebrew, or a real prefix.
#
# SAFETY: the stubbed rm REFUSES to delete anything outside $T, and the
# uninstall case skips LOUDLY if the host has a real /usr/local/bin/dynajs --
# a test must never remove somebody's binary.
set -uo pipefail

SRC="${1:-./install.sh}"
T="$(mktemp -d)"
# /bin/rm on purpose: later the stub rm goes on PATH and would refuse $T.
trap '/bin/rm -rf "$T"' EXIT

tail -1 "$SRC" | grep -qx 'main' || { echo "FAIL: last line of $SRC is not 'main'"; exit 1; }

unset DYNAJS_PREFIX DYNAJS_REPO DYNAJS_REF DYNAJS_JOBS DYNAJS_VERBOSE \
      DYNAJS_BREW_INSTALLER HOMEBREW_PREFIX
mkdir -p "$T/bin" "$T/home" "$T/cache"

# ---------------------------------------------------------------- stubs
# fake git: only the subcommands fetch_source issues.
cat > "$T/bin/git" <<EOF
#!/bin/sh
case "\$1" in
  clone)
    dir=""
    for a in "\$@"; do dir="\$a"; done
    mkdir -p "\$dir"
    : > "\$dir/Makefile"
    ;;
  rev-parse) echo "abc1234" ;;
  log)       echo "1 day ago" ;;
esac
exit 0
EOF

# fake make: `--version` answers preflight's tool_version probe WITHOUT counting
# a build; `clean` removes the binary; anything else plants a fake dynajs whose
# --help version embeds a build counter, so run 2 provably REPLACES a binary
# that differs from run 1's.
cat > "$T/bin/make" <<EOF
#!/bin/sh
case "\$1" in
  --version|-v) echo "GNU Make 4.4-fake"; exit 0 ;;
  clean) rm -f ./dynajs; exit 0 ;;
esac
n=0
[ -f "$T/count" ] && n=\$(cat "$T/count")
n=\$((n + 1))
echo "\$n" > "$T/count"
cp "$T/dynajs.tmpl" ./dynajs
chmod +x ./dynajs
exit 0
EOF

# The fake binary answers verify()'s five probes; --help prints the version and
# exits 1 (a usage dump -- the real dynajs has the same contract).
cat > "$T/dynajs.tmpl" <<EOF
#!/bin/sh
n=1
[ -f "$T/count" ] && n=\$(cat "$T/count")
if [ "\$1" = "--help" ]; then echo "DynaJS 0.0.\$n-fake"; exit 1; fi
if [ "\$1" = "-e" ]; then
  case "\$2" in
    *SHA256Hex*) echo "217e2b7d1803b912208726fc6f9cb143768e9b0671994ad281b67f5549cbd94d" ;;
    *u.v7*)      echo "01999999-9999-7999-9999-999999999999" ;;
    *DetectEncoding*) echo "ASCII" ;;
    *Pointer*)   echo "a~1b" ;;
    *)           echo "ok" ;;
  esac
fi
exit 0
EOF

# Detection must find clang and brew; nothing may invoke either.
printf '#!/bin/sh\nexit 0\n' > "$T/bin/clang"
printf '#!/bin/sh\nexit 0\n' > "$T/bin/brew"

# fake rm: real for anything under $T, REFUSES everything else. This is the
# safety net that makes the uninstall case non-destructive on any host. Flags
# are kept, because `rm -rf` on a directory is the installer's own call.
cat > "$T/bin/rm" <<EOF
#!/bin/sh
rc=0
for a in "\$@"; do
  case "\$a" in
    -*) ;;
    /*) ;;
    *) a="\$PWD/\$a" ;;
  esac
  case "\$a" in
    "$T"/*) /bin/rm -rf -- "\$a" || rc=1 ;;
    *) [ -e "\$a" ] && { echo "stub rm refuses: \$a" >&2; rc=1; } ;;
  esac
done
exit \$rc
EOF
chmod +x "$T/bin/git" "$T/bin/make" "$T/bin/clang" "$T/bin/brew" "$T/bin/rm"

# PATH: stubs first, then the host PATH with every directory holding a brew
# stripped -- otherwise adopt_brew sources the REAL shellenv, which prepends
# the brew bin dir and puts the real git/make ahead of the stubs.
PATH_CLEAN="$PATH"
out=""
while IFS= read -r d; do
    [ -n "$d" ] || continue
    [ -x "$d/brew" ] && continue
    case "$d" in *"$T"*) continue ;; esac
    out="$out${out:+:}$d"
done <<< "$(printf '%s' "$PATH_CLEAN" | tr ':' '\n')"
PATH="$T/bin:$out"

n=0; fails=0
check() { n=$((n+1)); if ! eval "$2"; then echo "FAIL: $1"; fails=$((fails+1)); fi; }

run_installer() {  # $1 = output tag, rest = installer args
    local tag="$1"; shift
    env HOME="$T/home" XDG_CACHE_HOME="$T/cache" NO_COLOR=1 \
        PATH="$PATH" bash "$SRC" "$@" > "$T/$tag.out" 2> "$T/$tag.err"
}

# ------------------------------------------------------- run 1: fresh install
run_installer run1 --prefix "$T/prefix" --repo "file://$T/fakerepo" --ref v1 --jobs 1
rc=$?
check "fresh install exits 0"                  "[ $rc -eq 0 ]"
check "  reports the install directory"        "grep -qF \"$T/prefix/bin\" \"$T/run1.out\""
check "  saw nothing to replace"               "grep -q 'replacing.*nothing — this is a fresh install' \"$T/run1.out\""
check "  cloned through the stubbed git"       "grep -q 'at commit abc1234' \"$T/run1.out\""
check "  built the binary"                     "grep -q 'built dynajs' \"$T/run1.out\""
check "  verified the hash probe"              "grep -q 'dyna:hash.*matches' \"$T/run1.out\""
check "  installed a first-build binary"       "grep -q 'DynaJS 0.0.1-fake installed to' \"$T/run1.out\""
check "  and it is executable"                 "[ -x \"$T/prefix/bin/dynajs\" ]"
check "  the stub build ran once"              "[ \"\$(cat \"$T/count\")\" = 1 ]"

# ---------------------------------------------------- run 2: the upgrade path
run_installer run2 --prefix "$T/prefix" --repo "file://$T/fakerepo" --ref v1 --jobs 1
rc=$?
check "upgrade run exits 0"                    "[ $rc -eq 0 ]"
check "  preflight read the old version"       "grep -q 'replacing.*DynaJS 0.0.1-fake' \"$T/run2.out\""
check "  the report says what it replaced"     "grep -q 'replaced: DynaJS 0.0.1-fake' \"$T/run2.out\""
check "  the stub build ran again"             "[ \"\$(cat \"$T/count\")\" = 2 ]"

# ------------------------------------------------------------ uninstall
if [ -e /usr/local/bin/dynajs ]; then
    echo "  SKIP  uninstall case (a real /usr/local/bin/dynajs exists on this host)"
else
    run_installer run3 --prefix "$T/prefix" --uninstall
    rc=$?
    check "--uninstall exits 0"                "[ $rc -eq 0 ]"
    check "  and removed the binary"           "[ ! -e \"$T/prefix/bin/dynajs\" ]"
fi

echo
if [ "$fails" -eq 0 ]; then
    echo "test_install_flow: all $n checks passed"
else
    echo "test_install_flow: $fails FAILED of $n"
    for tag in run1 run2 run3; do
        [ -s "$T/$tag.err" ] || continue
        echo "---- $tag stderr ----"; cat "$T/$tag.err"
        echo "---- $tag stdout ----"; cat "$T/$tag.out"
    done
fi
exit "$fails"
