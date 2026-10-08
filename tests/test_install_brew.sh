#!/usr/bin/env bash
set -uo pipefail

SRC="${1:-./install.sh}"
T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT

tail -1 "$SRC" | grep -qx 'main' || { echo "FAIL: last line of $SRC is not 'main'"; exit 1; }
sed '$d' "$SRC" > "$T/lib.sh"

set --
NO_COLOR=1
# shellcheck disable=SC1090
source "$T/lib.sh"
set +e; trap - ERR

n=0; fails=0
check() { n=$((n+1)); if ! eval "$2"; then echo "FAIL: $1"; fails=$((fails+1)); fi; }
must_be_local() {
    case "$BREW_INSTALLER_URL" in
        file://"$T"/*) ;;
        *) echo "ABORT: BREW_INSTALLER_URL is not a local stub: $BREW_INSTALLER_URL"; exit 99 ;;
    esac
}

LOG_FILE="$T/log"; : > "$LOG_FILE"
QUIET=1

HAVE_CURL=1
command -v curl >/dev/null 2>&1 || { HAVE_CURL=0; echo "SKIP: no curl here — the download cases cannot run"; }
skip_no_curl() { [ "$HAVE_CURL" -eq 1 ]; }

mk_brew() {
    mkdir -p "$1/bin"
    cat > "$1/bin/brew" <<EOF
#!/bin/sh
[ "\$1" = shellenv ] && { echo "export PATH=\\"$1/bin:\\\$PATH\\"; export FAKE_BREW_ADOPTED=1"; exit 0; }
[ "\$1" = --version ] && { echo "Homebrew 4.9.9-fake"; exit 0; }
[ "\$1" = --prefix ]  && { echo "$1"; exit 0; }
exit 0
EOF
    chmod +x "$1/bin/brew"
}
mk_installer() {
    { printf '#!/bin/sh\ntouch %s\n' "$2"; printf '# padding to clear the size gate\n%.0s' $(seq 1 40); } > "$1"
}

PATH_CLEAN="$PATH"
hide() {
    local d out=""
    while IFS= read -r d; do
        [ -n "$d" ] || continue
        [ -x "$d/brew" ] && continue
        case "$d" in *"$T"*) continue ;; esac
        out="$out${out:+:}$d"
    done <<< "$(printf '%s' "$PATH_CLEAN" | tr ':' '\n')"
    PATH="$out"
}

mk_brew "$T/fakebrew"
brew_prefixes() { printf '%s\n' "$T/fakebrew"; }
hide
( adopt_brew ); rc=$?
check "adopt_brew finds an off-PATH brew"  "[ $rc -eq 0 ]"
adopt_brew
check "  it sources that brew's shellenv"  '[ "${FAKE_BREW_ADOPTED:-0}" = 1 ]'
check "  which puts brew on PATH"          'command -v brew >/dev/null'

unset FAKE_BREW_ADOPTED
hide
brew_prefixes() { printf '%s\n' "$T/nothing-here"; }
( adopt_brew ); rc=$?
check "adopt_brew reports absence"         "[ $rc -ne 0 ]"

if skip_no_curl; then
hide
BREW_INSTALLER_URL="file://$T/does-not-exist.sh"; must_be_local
out="$(install_brew 2>&1)"; rc=$?
check "a failed download is refused"       "[ $rc -ne 0 ]"
check "  and names the download"           'printf %s "$out" | grep -q "could not download"'

printf '#!/bin/sh\ntouch %s/RAN_SHORT\n' "$T" > "$T/short.sh"
BREW_INSTALLER_URL="file://$T/short.sh"; must_be_local
out="$(install_brew 2>&1)"; rc=$?
check "a short download is refused"        "[ $rc -ne 0 ]"
check "  and says why"                     'printf %s "$out" | grep -q "came back short"'
check "  and NEVER RAN IT"                 "[ ! -e '$T/RAN_SHORT' ]"

mk_installer "$T/noop.sh" "$T/RAN_NOOP"
BREW_INSTALLER_URL="file://$T/noop.sh"; must_be_local
out="$(install_brew 2>&1)"; rc=$?
check "an installer yielding no brew fails" "[ $rc -ne 0 ]"
check "  it DID run, so the size gate passed it" "[ -e '$T/RAN_NOOP' ]"
check "  and the post-check is what caught it"   'printf %s "$out" | grep -q "still not on PATH"'

fi

id() { echo 0; }
in_container() { return 1; }
BREW_INSTALLER_URL="file://$T/noop.sh"; must_be_local
rm -f "$T/RAN_NOOP"
out="$(install_brew 2>&1)"; rc=$?
check "root outside a container is refused" "[ $rc -ne 0 ]"
check "  before anything is downloaded"     "[ ! -e '$T/RAN_NOOP' ]"
check "  and says so"                       'printf %s "$out" | grep -q "refuses to install as root"'

if skip_no_curl; then
in_container() { return 0; }
rm -f "$T/RAN_NOOP"
out="$(install_brew 2>&1)"; rc=$?
check "root INSIDE a container is allowed"  "[ -e '$T/RAN_NOOP' ]"
fi
unset -f id
in_container() { [ -f /.dockerenv ] || [ -f /run/.containerenv ]; }

if skip_no_curl; then
mk_brew "$T/planted"
mk_installer "$T/good.sh" "$T/RAN_OK"
brew_prefixes() { printf '%s\n' "$T/planted"; }
hide
BREW_INSTALLER_URL="file://$T/good.sh"; must_be_local
install_brew >/dev/null 2>&1; rc=$?
check "the success path returns 0"         "[ $rc -eq 0 ]"
check "  the installer ran"                "[ -e '$T/RAN_OK' ]"
check "  and brew is usable in the CALLER" 'command -v brew >/dev/null'
fi

origin_url="$(git -C "$(dirname "$SRC")" remote get-url origin 2>/dev/null || true)"
if [ -z "$origin_url" ]; then
    echo "  SKIP  repo-url consistency (no git origin remote here)"
else
    slug_of() { printf '%s' "$1" | sed -E 's#\.git$##; s#^.*[/:]([^/]+/[^/]+)$#\1#'; }
    want="$(slug_of "$origin_url")"
    check "install.sh's default repo is this repository ($want)" \
          "[ \"\$(slug_of \"\$REPO_URL\")\" = \"$want\" ]"
    stale="$(grep -oE 'github(usercontent)?\.com/[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+' "$SRC" \
             | sed -E 's#^github(usercontent)?\.com/##; s#\.git$##' | sort -u \
             | grep -v -x -e "$want" -e 'Homebrew/install' || true)"
    check "no other repository is named in $SRC (found: ${stale:-none})" \
          "[ -z \"\$stale\" ]"
fi

echo
if [ "$fails" -eq 0 ]; then echo "test_brew: all $n checks passed"; else echo "test_brew: $fails FAILED of $n"; fi
exit "$fails"
