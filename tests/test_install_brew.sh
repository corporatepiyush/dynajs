#!/usr/bin/env bash
# Unit-tests install.sh's Homebrew bootstrap by sourcing the script WITHOUT its
# final `main`, then calling the functions with the environment stubbed.
# Each case must fail for the reason it names, so every one is checked twice:
# the return status, and a side effect that says what actually happened.
#
# SAFETY: install_brew really does install Homebrew. Every call here is fenced
# by must_be_local, which aborts unless the URL is a file:// under $T -- an
# earlier version of this file set DYNAJS_BREW_INSTALLER *after* sourcing, by
# which time BREW_INSTALLER_URL had already been resolved, and it went off and
# ran the genuine installer.
set -uo pipefail

SRC="${1:-./install.sh}"
T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT

tail -1 "$SRC" | grep -qx 'main' || { echo "FAIL: last line of $SRC is not 'main'"; exit 1; }
sed '$d' "$SRC" > "$T/lib.sh"          # drop the trailing `main`

set --                                  # no args for the sourced arg parser
NO_COLOR=1
# shellcheck disable=SC1090
source "$T/lib.sh"
set +e; trap - ERR                      # the sourced script turns on -Ee

n=0; fails=0
check() { n=$((n+1)); if ! eval "$2"; then echo "FAIL: $1"; fails=$((fails+1)); fi; }
must_be_local() {
    case "$BREW_INSTALLER_URL" in
        file://"$T"/*) ;;
        *) echo "ABORT: BREW_INSTALLER_URL is not a local stub: $BREW_INSTALLER_URL"; exit 99 ;;
    esac
}

LOG_FILE="$T/log"; : > "$LOG_FILE"
QUIET=1                                 # keep the pretty output out of the way

# install_brew needs curl, and a slim container has none -- every download case
# then fails for the environment rather than for the code. Skip LOUDLY: a skip
# that prints nothing is how a suite quietly stops testing what it claims.
HAVE_CURL=1
command -v curl >/dev/null 2>&1 || { HAVE_CURL=0; echo "SKIP: no curl here — the download cases cannot run"; }
skip_no_curl() { [ "$HAVE_CURL" -eq 1 ]; }

mk_brew() {                             # $1 = prefix to plant a fake brew in
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
# A stub installer long enough to clear the size gate.
mk_installer() {                        # $1 = path, $2 = marker to touch
    { printf '#!/bin/sh\ntouch %s\n' "$2"; printf '# padding to clear the size gate\n%.0s' $(seq 1 40); } > "$1"
}

PATH_CLEAN="$PATH"
# Drop every directory that holds a `brew`, the REAL one included: adopt_brew
# checks `command -v brew` first, so leaving /opt/homebrew on PATH made three
# cases pass against the machine's own Homebrew instead of the stub.
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

# ---------------------------------------------------------------- adopt_brew
mk_brew "$T/fakebrew"
brew_prefixes() { printf '%s\n' "$T/fakebrew"; }
hide
( adopt_brew ); rc=$?
check "adopt_brew finds an off-PATH brew"  "[ $rc -eq 0 ]"
adopt_brew
check "  it sources that brew's shellenv"  '[ "${FAKE_BREW_ADOPTED:-0}" = 1 ]'
check "  which puts brew on PATH"          'command -v brew >/dev/null'

# The negative: with no brew anywhere, adopt_brew must say so rather than
# silently succeeding -- that is what decides whether install_brew runs.
unset FAKE_BREW_ADOPTED
hide
brew_prefixes() { printf '%s\n' "$T/nothing-here"; }
( adopt_brew ); rc=$?
check "adopt_brew reports absence"         "[ $rc -ne 0 ]"

# ------------------------------------------------------- install_brew refusals
if skip_no_curl; then
# 1. a download that fails must not run anything
hide
BREW_INSTALLER_URL="file://$T/does-not-exist.sh"; must_be_local
out="$(install_brew 2>&1)"; rc=$?
check "a failed download is refused"       "[ $rc -ne 0 ]"
check "  and names the download"           'printf %s "$out" | grep -q "could not download"'

# 2. a SHORT download must be refused UNRUN: piped straight into bash, a
#    truncated download executes as a partial script.
printf '#!/bin/sh\ntouch %s/RAN_SHORT\n' "$T" > "$T/short.sh"
BREW_INSTALLER_URL="file://$T/short.sh"; must_be_local
out="$(install_brew 2>&1)"; rc=$?
check "a short download is refused"        "[ $rc -ne 0 ]"
check "  and says why"                     'printf %s "$out" | grep -q "came back short"'
check "  and NEVER RAN IT"                 "[ ! -e '$T/RAN_SHORT' ]"

# 3. an installer that runs but leaves no brew must be caught by the re-check
mk_installer "$T/noop.sh" "$T/RAN_NOOP"
BREW_INSTALLER_URL="file://$T/noop.sh"; must_be_local
out="$(install_brew 2>&1)"; rc=$?
check "an installer yielding no brew fails" "[ $rc -ne 0 ]"
check "  it DID run, so the size gate passed it" "[ -e '$T/RAN_NOOP' ]"
check "  and the post-check is what caught it"   'printf %s "$out" | grep -q "still not on PATH"'

fi   # skip_no_curl

# 4. root is refused, because Homebrew refuses it much later and louder -- but
#    NOT inside a container, which is Homebrew's own exemption. Both halves are
#    checked: the container case is why this first ran red under Docker, where
#    the refusal is the wrong answer.
#    The root check precedes the curl check, so the refusal half needs no curl;
#    the exemption half proceeds to the download and does.
id() { echo 0; }
in_container() { return 1; }               # root, not in a container
BREW_INSTALLER_URL="file://$T/noop.sh"; must_be_local
rm -f "$T/RAN_NOOP"
out="$(install_brew 2>&1)"; rc=$?
check "root outside a container is refused" "[ $rc -ne 0 ]"
check "  before anything is downloaded"     "[ ! -e '$T/RAN_NOOP' ]"
check "  and says so"                       'printf %s "$out" | grep -q "refuses to install as root"'

if skip_no_curl; then
in_container() { return 0; }               # root INSIDE a container
rm -f "$T/RAN_NOOP"
out="$(install_brew 2>&1)"; rc=$?
check "root INSIDE a container is allowed"  "[ -e '$T/RAN_NOOP' ]"
fi   # skip_no_curl
unset -f id
# RESTORE the predicate, do not unset it: install.sh:290 defines in_container,
# and `unset -f` deletes THAT definition, so every later case reaching the root
# check calls a function that no longer exists. Invisible on macOS (the test is
# not root, so the check short-circuits) and fatal in CI, which is.
in_container() { [ -f /.dockerenv ] || [ -f /run/.containerenv ]; }

# ------------------------------------------------------- install_brew success
if skip_no_curl; then
mk_brew "$T/planted"
mk_installer "$T/good.sh" "$T/RAN_OK"
brew_prefixes() { printf '%s\n' "$T/planted"; }
hide
BREW_INSTALLER_URL="file://$T/good.sh"; must_be_local
# NOT in a command substitution: the whole point is that the shellenv eval lands
# in the CALLER's PATH, and a subshell would hide a version that did not.
install_brew >/dev/null 2>&1; rc=$?
check "the success path returns 0"         "[ $rc -eq 0 ]"
check "  the installer ran"                "[ -e '$T/RAN_OK' ]"
check "  and brew is usable in the CALLER" 'command -v brew >/dev/null'
fi   # skip_no_curl

# ------------------------------------------------- the URLs the script hands out
# WHY: install.sh pointed at .../dynascript for every user after the repository
# was renamed to dynajs -- a hard 404, so the FIRST command a new user runs
# could not clone anything. Nothing caught it: every case above stubs the
# network, so REPO_URL_DEFAULT was never read by any test.
#
# This check is OFFLINE and deterministic on purpose. Hitting GitHub would make
# the suite fail when the network is down, which is not a defect in install.sh.
# The repository's own `origin` is the authority, and a rename moves it.
origin_url="$(git -C "$(dirname "$SRC")" remote get-url origin 2>/dev/null || true)"
if [ -z "$origin_url" ]; then
    echo "  SKIP  repo-url consistency (no git origin remote here)"
else
    # normalise: strip a trailing .git and any scheme/host, compare owner/name
    slug_of() { printf '%s' "$1" | sed -E 's#\.git$##; s#^.*[/:]([^/]+/[^/]+)$#\1#'; }
    want="$(slug_of "$origin_url")"
    check "install.sh's default repo is this repository ($want)" \
          "[ \"\$(slug_of \"\$REPO_URL_DEFAULT\")\" = \"$want\" ]"
    # every other URL the script prints must name the same repository, or a
    # user is sent to a 404 for docs, issues, or the curl-pipe-bash one-liner
    # Homebrew/install is a genuine third party (the brew bootstrap), named
    # here so any OTHER foreign repository still fails this check.
    stale="$(grep -oE 'github(usercontent)?\.com/[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+' "$SRC" \
             | sed -E 's#^github(usercontent)?\.com/##; s#\.git$##' | sort -u \
             | grep -v -x -e "$want" -e 'Homebrew/install' || true)"
    check "no other repository is named in $SRC (found: ${stale:-none})" \
          "[ -z \"\$stale\" ]"
fi

echo
if [ "$fails" -eq 0 ]; then echo "test_brew: all $n checks passed"; else echo "test_brew: $fails FAILED of $n"; fi
exit "$fails"
