#!/usr/bin/env bash
set -uo pipefail

SRC="${1:-./install.sh}"
T="$(mktemp -d)"
trap '/bin/rm -rf "$T"' EXIT

tail -1 "$SRC" | grep -qx 'main' || { echo "FAIL: last line of $SRC is not 'main'"; exit 1; }

unset DYNAJS_PREFIX DYNAJS_JOBS DYNAJS_VERBOSE \
      DYNAJS_BREW_INSTALLER HOMEBREW_PREFIX
mkdir -p "$T/bin" "$T/home" "$T/cache"

sed "s#^REPO_URL=.*#REPO_URL=\"file://$T/fakerepo\"#" "$SRC" > "$T/install.sh"

cat > "$T/bin/git" <<EOF
#!/bin/sh
case "\$1" in
  clone)
    dir=""
    for a in "\$@"; do dir="\$a"; done
    mkdir -p "\$dir"
    # The installer builds with the checkout's own ./build.sh (there is no
    # Makefile), so the fake checkout carries a stub build.sh that counts its
    # runs and drops the template binary -- the role the make stub used to play.
    cat > "\$dir/build.sh" <<'STUB'
#!/bin/sh
case "\$1" in
  clean) rm -f ./dynajs; exit 0 ;;
esac
n=0
[ -f "__T__/count" ] && n=\$(cat "__T__/count")
n=\$((n + 1))
echo "\$n" > "__T__/count"
cp "__T__/dynajs.tmpl" ./dynajs
chmod +x ./dynajs
exit 0
STUB
    sed -i.bak "s#__T__#$T#g" "\$dir/build.sh" && rm -f "\$dir/build.sh.bak"
    chmod +x "\$dir/build.sh"
    ;;
  rev-parse) echo "abc1234" ;;
  log)       echo "1 day ago" ;;
esac
exit 0
EOF

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

cat > "$T/dynajs.tmpl" <<EOF
#!/bin/sh
n=1
[ -f "$T/count" ] && n=\$(cat "$T/count")
if [ "\$1" = "--help" ]; then echo "DynaJS 0.0.\$n-fake"; exit 1; fi
if [ "\$1" = "-e" ]; then
  case "\$2" in
    *SHA256Hex*) if [ -f "$T/baddigest" ]; then echo "0000000000000000000000000000000000000000000000000000000000000000"; else echo "217e2b7d1803b912208726fc6f9cb143768e9b0671994ad281b67f5549cbd94d"; fi ;;
    *u.v7*)      echo "01999999-9999-7999-9999-999999999999" ;;
    *DetectEncoding*) echo "ASCII" ;;
    *Pointer*)   echo "a~1b" ;;
    *)           echo "ok" ;;
  esac
fi
exit 0
EOF

printf '#!/bin/sh\nexit 0\n' > "$T/bin/clang"
printf '#!/bin/sh\nexit 0\n' > "$T/bin/brew"

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

run_installer() {
    local tag="$1"; shift
    env HOME="$T/home" XDG_CACHE_HOME="$T/cache" NO_COLOR=1 \
        PATH="$PATH" bash "$T/install.sh" "$@" > "$T/$tag.out" 2> "$T/$tag.err"
}

run_installer badref --ref v1
rc=$?
check "--ref is refused"                        "[ $rc -ne 0 ]"
check "  naming it as unknown"                  "grep -q 'unknown option: --ref' \"$T/badref.err\""
run_installer badrepo --repo file:///nowhere
rc=$?
check "--repo is refused"                       "[ $rc -ne 0 ]"
check "  naming it as unknown"                  "grep -q 'unknown option: --repo' \"$T/badrepo.err\""

run_installer run1 --prefix "$T/prefix" --jobs 1
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

run_installer run2 --prefix "$T/prefix" --jobs 1
rc=$?
check "upgrade run exits 0"                    "[ $rc -eq 0 ]"
check "  preflight read the old version"       "grep -q 'replacing.*DynaJS 0.0.1-fake' \"$T/run2.out\""
check "  the report says what it replaced"     "grep -q 'replaced: DynaJS 0.0.1-fake' \"$T/run2.out\""
check "  the stub build ran again"             "[ \"\$(cat \"$T/count\")\" = 2 ]"

# A build whose SHA-256 known answer is wrong must not replace the installed
# binary: the installer fails and the previous file is byte-identical.
before=$(cksum < "$T/prefix/bin/dynajs")
: > "$T/baddigest"
run_installer runbad --prefix "$T/prefix" --jobs 1
rc=$?
rm -f "$T/baddigest"
check "wrong self-test digest fails the install" "[ $rc -ne 0 ]"
check "  and says nothing was installed"         "grep -q 'nothing was installed' \"$T/runbad.err\" \"$T/runbad.out\""
check "  the previous binary is untouched"       "[ \"\$(cksum < \"$T/prefix/bin/dynajs\")\" = \"$before\" ]"
check "  no staged file is left behind"          "[ -z \"\$(ls -A \"$T/prefix/bin\" | grep -v '^dynajs\$')\" ]"

if [ -e /usr/local/bin/dynajs ]; then
    echo "  SKIP  uninstall case (a real /usr/local/bin/dynajs exists on this host)"
else
    mkdir -p "$T/home/.local/bin"
    printf '#!/bin/sh\necho "some other tool that happens to share the name"\n' > "$T/home/.local/bin/dynajs"
    run_installer run3 --prefix "$T/prefix" --uninstall
    rc=$?
    check "--uninstall exits 0"                "[ $rc -eq 0 ]"
    check "  and removed the binary"           "[ ! -e \"$T/prefix/bin/dynajs\" ]"
    check "  a same-named file that is not DynaJS is left alone" "[ -e \"$T/home/.local/bin/dynajs\" ]"
    check "  and the installer says so"        "grep -q 'does not identify as a DynaJS binary' \"$T/run3.err\""
    rm -f "$T/home/.local/bin/dynajs"
fi

env -u HOME XDG_CACHE_HOME="$T/cache" NO_COLOR=1 PATH="$PATH" bash "$T/install.sh" --dry-run > "$T/nohome.out" 2> "$T/nohome.err"
rc=$?
check "an unset HOME is refused with a message"  "[ $rc -ne 0 ] && grep -q 'HOME is not set' \"$T/nohome.err\""
if true; then
    :
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
