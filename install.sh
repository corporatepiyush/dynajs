#!/usr/bin/env bash
#
# install.sh — install, reinstall, or upgrade the DynaJS runtime.
#
# This script always performs a CLEAN install: it (re)clones the source into a
# build cache, builds from scratch with the full native standard library, and
# OVERWRITES any previous `dynajs` binary at the install prefix. Running it again
# is how you upgrade (it pulls the latest source and rebuilds) or repair a broken
# install (it discards the old build tree entirely).
#
# It asks no questions and reads nothing from stdin, so it is safe to pipe from
# curl. Everything it is about to do is printed before it does it.

set -Eeuo pipefail

# ----------------------------------------------------------------------------- config / defaults
REPO_URL_DEFAULT="https://github.com/corporatepiyush/dynajs.git"
REF_DEFAULT="master"
PREFIX_DEFAULT="/usr/local"
BINARY_NAME="dynajs"

REPO_URL="${DYNAJS_REPO:-$REPO_URL_DEFAULT}"
REF="${DYNAJS_REF:-$REF_DEFAULT}"
PREFIX="${DYNAJS_PREFIX:-$PREFIX_DEFAULT}"
JOBS="${DYNAJS_JOBS:-}"
VERBOSE="${DYNAJS_VERBOSE:-0}"
QUIET=0
WITH_DEPS=0
DO_UNINSTALL=0
DRY_RUN=0

BUILD_ROOT="${XDG_CACHE_HOME:-$HOME/.cache}/dynajs-build"
SRC_DIR="$BUILD_ROOT/src"
LOG_FILE="$BUILD_ROOT/install.log"

START_TIME=$SECONDS
INSTALLED_PATH=""
INSTALLED_BINDIR=""
PREVIOUS_VERSION=""
ERR_REPORTED=0

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" 2>/dev/null && pwd || echo "")"
LOCAL_REPO=""
if [ -n "$SCRIPT_DIR" ] && [ -f "$SCRIPT_DIR/src/dynajs.c" ] && [ -f "$SCRIPT_DIR/Makefile" ]; then
    LOCAL_REPO="$SCRIPT_DIR"
fi

usage() {
    cat <<'EOF'
install.sh — install, reinstall, or upgrade the DynaJS runtime.

USAGE
  ./install.sh [options]
  curl -fsSL https://raw.githubusercontent.com/corporatepiyush/dynajs/master/install.sh | bash
  curl -fsSL .../install.sh | bash -s -- --prefix "$HOME/.local"

OPTIONS
  --prefix DIR    Install prefix; the binary goes to DIR/bin. Default /usr/local
                  (falls back to $HOME/.local when /usr/local/bin is not
                  writable and sudo is unavailable).
  --repo URL      Git repository to clone. Default: the DynaJS upstream.
  --ref REF       Branch, tag, or commit to build. Default: master.
  --jobs N        Parallel build jobs. Default: the CPU count.
  --with-deps     Install missing build prerequisites with the system package
                  manager (brew/apt/dnf/pacman/pkg). Off by default.
  --verbose, -v   Stream the build output and print every command that runs.
  --quiet, -q     Only warnings and errors.
  --dry-run       Print the plan and the preflight report, then stop.
  --uninstall     Remove the installed dynajs binary and exit.
  --help, -h      This text.

ENVIRONMENT
  DYNAJS_PREFIX, DYNAJS_REPO, DYNAJS_REF, DYNAJS_JOBS, DYNAJS_VERBOSE
                  The same settings, for when passing flags through a pipe is
                  awkward. A command-line flag wins over the variable.
  HOMEBREW_PREFIX A Homebrew installed somewhere other than the usual prefix.
                  Read, never written.
  DYNAJS_BREW_INSTALLER
                  Where --with-deps fetches Homebrew from. Default: the official
                  installer. Point it at a mirror you trust, or at a copy.
  NO_COLOR        Any value disables colour.

WHAT IT DOES
  Clones into ~/.cache/dynajs-build, builds with CONFIG_NATIVE_MODULES=y, and
  installs one static, dependency-free binary. The full build log is kept at
  ~/.cache/dynajs-build/install.log whether it succeeds or fails.

REQUIREMENTS
  git, make, and a C compiler (clang preferred, gcc accepted), on macOS, Linux
  or FreeBSD. On Windows, use WSL.
EOF
    exit 0
}

# ----------------------------------------------------------------------------- pretty logging
if [ -t 1 ] && [ -z "${NO_COLOR:-}" ]; then
    C_BOLD=$(printf '\033[1m'); C_RED=$(printf '\033[31m'); C_GRN=$(printf '\033[32m')
    C_YLW=$(printf '\033[33m'); C_BLU=$(printf '\033[34m'); C_DIM=$(printf '\033[2m')
    C_OFF=$(printf '\033[0m')
else
    C_BOLD=""; C_RED=""; C_GRN=""; C_YLW=""; C_BLU=""; C_DIM=""; C_OFF=""
fi

info()  { [ "$QUIET" -eq 1 ] || printf '\n%s==>%s %s%s%s\n' "$C_BLU$C_BOLD" "$C_OFF" "$C_BOLD" "$*" "$C_OFF"; }
step()  { [ "$QUIET" -eq 1 ] || printf '  %s-%s %s\n' "$C_GRN" "$C_OFF" "$*"; }
# A two-column detail line: the preflight report is most of this script's value.
item()  { [ "$QUIET" -eq 1 ] || printf '  %s-%s %-11s %s\n' "$C_GRN" "$C_OFF" "$1" "$2"; }
note()  { [ "$QUIET" -eq 1 ] || printf '    %s%s%s\n' "$C_DIM" "$*" "$C_OFF"; }
debug() { [ "$VERBOSE" -eq 1 ] && printf '  %s[debug] %s%s\n' "$C_DIM" "$*" "$C_OFF" >&2; return 0; }
warn()  { printf '%swarning:%s %s\n' "$C_YLW" "$C_OFF" "$*" >&2; }
die()   { printf '\n%serror:%s %s\n' "$C_RED$C_BOLD" "$C_OFF" "$*" >&2; exit 1; }

# `dynajs --help` prints the version on line 1 and exits 1, being a usage dump.
# Under `pipefail` that status propagates, so the `|| true` is load-bearing.
binary_version() { "$1" --help 2>&1 | head -1 || true; }

# Run a command, echoing it under --verbose. Every external call goes through
# this, so `--verbose` is a complete transcript rather than a sample.
run() { debug "\$ $*"; "$@"; }

elapsed() {
    local s=$(( SECONDS - ${1:-$START_TIME} ))
    if [ "$s" -ge 60 ]; then printf '%dm %02ds' $(( s / 60 )) $(( s % 60 ))
    else printf '%ds' "$s"; fi
}

# The log is the only artefact of a failed run, so show it rather than name it.
dump_log() {
    if [ -s "$LOG_FILE" ]; then
        printf '\n%s---- last 25 lines of %s ----%s\n' "$C_DIM" "$LOG_FILE" "$C_OFF" >&2
        tail -25 "$LOG_FILE" >&2
        printf '%s---- end of log ----%s\n' "$C_DIM" "$C_OFF" >&2
    fi
    printf '\n%sFull log: %s%s\n' "$C_DIM" "$LOG_FILE" "$C_OFF" >&2
    printf '%sRe-run with --verbose for a live transcript, or report it at%s\n' \
        "$C_DIM" "$C_OFF" >&2
    printf '%s  https://github.com/corporatepiyush/dynajs/issues%s\n' "$C_DIM" "$C_OFF" >&2
}

# die + the log. Used wherever the failure is expected and already explained.
die_log() { printf '\n%serror:%s %s\n' "$C_RED$C_BOLD" "$C_OFF" "$*" >&2; dump_log; exit 1; }

on_error() {
    local rc=$? line=${1:-?}
    [ "$ERR_REPORTED" -eq 0 ] || exit "$rc"
    ERR_REPORTED=1
    printf '\n%serror:%s the installer stopped unexpectedly at line %s (exit %s).\n' \
        "$C_RED$C_BOLD" "$C_OFF" "$line" "$rc" >&2
    dump_log
    exit "$rc"
}
trap 'on_error $LINENO' ERR
trap 'printf "\n%sinterrupted.%s Nothing was installed.\n" "$C_YLW" "$C_OFF" >&2; exit 130' INT

# ----------------------------------------------------------------------------- arg parsing
# A count that does not parse is REFUSED, not clamped: `--jobs abc` reaching
# `make -j abc` fails somewhere far from the typo that caused it.
need_int() {
    case "$2" in
        ''|*[!0-9]*) die "$1 needs a positive whole number, got '$2'" ;;
    esac
    [ "$2" -gt 0 ] || die "$1 needs a positive whole number, got '$2'"
}

while [ $# -gt 0 ]; do
    case "$1" in
        --prefix)    PREFIX="${2:?--prefix needs a directory}"; shift 2 ;;
        --prefix=*)  PREFIX="${1#*=}"; shift ;;
        --repo)      REPO_URL="${2:?--repo needs a URL}"; shift 2 ;;
        --repo=*)    REPO_URL="${1#*=}"; shift ;;
        --ref)       REF="${2:?--ref needs a ref}"; shift 2 ;;
        --ref=*)     REF="${1#*=}"; shift ;;
        --jobs)      need_int --jobs "${2:-}"; JOBS="$2"; shift 2 ;;
        --jobs=*)    need_int --jobs "${1#*=}"; JOBS="${1#*=}"; shift ;;
        --with-deps) WITH_DEPS=1; shift ;;
        --uninstall) DO_UNINSTALL=1; shift ;;
        -v|--verbose) VERBOSE=1; shift ;;
        -q|--quiet)  QUIET=1; shift ;;
        --dry-run)   DRY_RUN=1; shift ;;
        -h|--help)   usage ;;
        *)           die "unknown option: $1  (try --help)" ;;
    esac
done
[ -n "$PREFIX" ] || die "--prefix cannot be empty"
[ "$VERBOSE" -eq 1 ] && QUIET=0

# ----------------------------------------------------------------------------- platform detection
UNAME_S="$(uname -s)"
UNAME_M="$(uname -m)"
case "$UNAME_S" in
    Darwin)  OS="macos" ;;
    Linux)   OS="linux" ;;
    FreeBSD) OS="freebsd" ;;
    *)       die "unsupported OS: $UNAME_S  (supported: macOS, Linux, FreeBSD; on Windows use WSL)" ;;
esac

cpu_count() {
    if command -v nproc >/dev/null 2>&1; then nproc
    elif command -v sysctl >/dev/null 2>&1; then sysctl -n hw.ncpu
    else echo 4; fi
}
[ -n "$JOBS" ] || JOBS="$(cpu_count)"
need_int DYNAJS_JOBS "$JOBS"

os_pretty() {
    case "$OS" in
        macos) printf 'macOS %s' "$(sw_vers -productVersion 2>/dev/null || uname -r)" ;;
        linux) if [ -r /etc/os-release ]; then
                   ( . /etc/os-release; printf '%s' "${PRETTY_NAME:-Linux}" )
               else printf 'Linux %s' "$(uname -r)"; fi ;;
        *)     printf '%s %s' "$UNAME_S" "$(uname -r)" ;;
    esac
}

# ----------------------------------------------------------------------------- uninstall path
resolve_bindir() {
    # An explicitly chosen prefix is ALWAYS honored (install_binary creates it,
    # using sudo if needed) — never silently redirected.
    if [ "$PREFIX" != "$PREFIX_DEFAULT" ]; then
        echo "$PREFIX/bin"; return
    fi
    # The default prefix (/usr/local): use it if writable/creatable or if sudo is
    # available; otherwise fall back to a per-user prefix that needs no privileges.
    if [ -w "$PREFIX/bin" ] || [ -w "$PREFIX" ] || command -v sudo >/dev/null 2>&1; then
        echo "$PREFIX/bin"
    else
        echo "$HOME/.local/bin"
    fi
}

if [ "$DO_UNINSTALL" -eq 1 ]; then
    info "Uninstalling DynaJS"
    removed=0
    for dir in "$PREFIX/bin" "$HOME/.local/bin" "/usr/local/bin"; do
        target="$dir/$BINARY_NAME"
        debug "checking $target"
        if [ -e "$target" ]; then
            step "removing $target"
            if [ -w "$dir" ]; then run rm -f "$target"
            elif command -v sudo >/dev/null 2>&1; then
                note "this needs sudo"
                run sudo rm -f "$target"
            else die "cannot remove $target (no write permission and no sudo)"; fi
            removed=1
        fi
    done
    if [ "$removed" -eq 1 ]; then
        step "uninstalled."
        note "the build cache at $BUILD_ROOT is left alone; remove it with: rm -rf $BUILD_ROOT"
    else
        warn "no $BINARY_NAME binary found in $PREFIX/bin, $HOME/.local/bin or /usr/local/bin."
    fi
    exit 0
fi

# ----------------------------------------------------------------------------- homebrew
# Homebrew earns a section of its own here: the Makefile links SQLite through
# pkg-config, and when either is missing it leaves dyna:net's SQLite class out
# of the binary SILENTLY -- the build succeeds and the class is simply absent.
BREW_INSTALLER_URL="${DYNAJS_BREW_INSTALLER:-https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh}"

brew_prefixes() {
    [ -z "${HOMEBREW_PREFIX:-}" ] || printf '%s\n' "$HOMEBREW_PREFIX"
    case "$OS" in
        macos) printf '%s\n%s\n' /opt/homebrew /usr/local ;;
        linux) printf '%s\n%s\n' /home/linuxbrew/.linuxbrew "$HOME/.linuxbrew" ;;
        *)     : ;;
    esac
}

# brew is routinely installed and NOT on PATH -- a non-login shell, or the first
# run after installing it -- which reads exactly like "not installed" and sends
# the script off to install a second copy.
adopt_brew() {
    if command -v brew >/dev/null 2>&1; then
        debug "brew on PATH at $(command -v brew)"
        return 0
    fi
    local p
    while IFS= read -r p; do
        [ -n "$p" ] || continue
        if [ -x "$p/bin/brew" ]; then
            debug "brew at $p/bin/brew is not on PATH; sourcing its shellenv"
            eval "$("$p/bin/brew" shellenv)"
            return 0
        fi
    done <<EOF
$(brew_prefixes)
EOF
    debug "no brew found under: $(brew_prefixes | tr '\n' ' ')"
    return 1
}

# Root is allowed inside a container and nowhere else, which is Homebrew's own
# rule -- copy the exemption or an image build refuses for the wrong reason.
in_container() { [ -f /.dockerenv ] || [ -f /run/.containerenv ]; }

# Homebrew's installer has three preconditions of its own, and each fails late
# and obscurely when it is not checked first: it refuses to run as root outside
# a container, it needs git already present, and it waits for RETURN unless
# NONINTERACTIVE is set -- which it infers only when stdin is not a tty, so a
# run from a real terminal would sit there forever.
install_brew() {
    case "$OS" in
        macos|linux) ;;
        *) warn "Homebrew does not support $UNAME_S."; return 1 ;;
    esac
    # Cheapest and most fundamental first: telling a root user that Homebrew
    # needs curl names the wrong problem, and they hit the root wall next anyway.
    if [ "$(id -u)" = "0" ] && ! in_container; then
        warn "Homebrew refuses to install as root. Re-run as a normal user."
        return 1
    fi
    command -v curl >/dev/null 2>&1 || { warn "installing Homebrew needs curl."; return 1; }
    if [ "$OS" = "linux" ] && ! command -v git >/dev/null 2>&1; then
        warn "Homebrew needs git before it can install. Install git first, then re-run."
        return 1
    fi

    info "Installing Homebrew"
    step "$BREW_INSTALLER_URL"
    note "a large, system-wide change, and it will ask for your password"

    # Download and run are DELIBERATELY separate: piped straight into bash, a
    # truncated or failed download executes as a partial script.
    local script
    script="$(curl -fsSL "$BREW_INSTALLER_URL")" \
        || { warn "could not download the Homebrew installer."; return 1; }
    if [ "${#script}" -lt 1000 ]; then
        warn "the Homebrew installer came back short (${#script} bytes); refusing to run it."
        return 1
    fi
    debug "downloaded ${#script} bytes of installer"

    NONINTERACTIVE=1 /bin/bash -c "$script" 2>&1 | tee -a "$LOG_FILE" \
        || { warn "the Homebrew installer failed; see $LOG_FILE"; return 1; }
    adopt_brew || { warn "Homebrew installed, but 'brew' is still not on PATH."; return 1; }
    step "brew ready: $(brew --version 2>/dev/null | head -1)"
}

# ----------------------------------------------------------------------------- dependency checks
PKG_INSTALL=""
detect_pkg_mgr() {
    PKG_INSTALL=""
    if   command -v brew   >/dev/null 2>&1; then PKG_INSTALL="brew install"
    elif command -v apt-get>/dev/null 2>&1; then PKG_INSTALL="sudo apt-get install -y"
    elif command -v dnf    >/dev/null 2>&1; then PKG_INSTALL="sudo dnf install -y"
    elif command -v yum    >/dev/null 2>&1; then PKG_INSTALL="sudo yum install -y"
    elif command -v pacman >/dev/null 2>&1; then PKG_INSTALL="sudo pacman -S --noconfirm"
    elif command -v pkg    >/dev/null 2>&1; then PKG_INSTALL="sudo pkg install -y"
    fi
    debug "package manager: ${PKG_INSTALL:-none found}"
}

# Pick the C compiler: clang preferred (the project's primary toolchain), gcc accepted.
CC_BIN=""
select_compiler() {
    CC_BIN=""
    if   command -v clang >/dev/null 2>&1; then CC_BIN="clang"
    elif command -v gcc   >/dev/null 2>&1; then CC_BIN="gcc"
    fi
    debug "compiler: ${CC_BIN:-none found}"
}

install_dep_hint() {
    # $1 = human tool name, $2 = brew pkg, $3 = apt pkg, $4 = dnf pkg, $5 = pacman, $6 = pkg(bsd)
    case "$PKG_INSTALL" in
        brew*)   echo "$2" ;;
        *apt*)   echo "$3" ;;
        *dnf*|*yum*) echo "$4" ;;
        *pacman*)echo "$5" ;;
        *pkg*)   echo "$6" ;;
        *)       echo "$2" ;;
    esac
}

# Optional, and their absence changes what gets BUILT rather than failing.
optional_pkgs() {
    case "$PKG_INSTALL" in
        brew*)       echo "pkg-config sqlite zstd" ;;
        *apt*)       echo "pkg-config libsqlite3-dev libzstd-dev libssl-dev" ;;
        *dnf*|*yum*) echo "pkgconf-pkg-config sqlite-devel libzstd-devel openssl-devel" ;;
        *pacman*)    echo "pkgconf sqlite zstd openssl" ;;
        *pkg*)       echo "pkgconf sqlite3 zstd openssl" ;;
        *)           echo "" ;;
    esac
}

# The Makefile prefers Homebrew's keg over the system copy, because on macOS the
# system sqlite is older than the features the client uses. Ask the same way it
# does, or this reports a version the build will not use.
sqlite_version() {
    command -v pkg-config >/dev/null 2>&1 || return 1
    local pc=""
    if command -v brew >/dev/null 2>&1; then
        pc="$(brew --prefix sqlite 2>/dev/null || true)/lib/pkgconfig"
    fi
    PKG_CONFIG_PATH="$pc:${PKG_CONFIG_PATH:-}" pkg-config --modversion sqlite3 2>/dev/null
}

zstd_version() {
    command -v pkg-config >/dev/null 2>&1 || return 1
    local pc=""
    if command -v brew >/dev/null 2>&1; then
        pc="$(brew --prefix zstd 2>/dev/null || true)/lib/pkgconfig"
    fi
    PKG_CONFIG_PATH="$pc:${PKG_CONFIG_PATH:-}" pkg-config --modversion libzstd 2>/dev/null
}

missing_tools() {
    local m=""
    command -v git  >/dev/null 2>&1 || m="$m git"
    command -v make >/dev/null 2>&1 || m="$m make"
    select_compiler
    [ -n "$CC_BIN" ] || m="$m compiler"
    printf '%s' "$m"
}

# macOS keeps clang, git and make in one package, and Homebrew needs it too, so
# on that platform this is the whole prerequisite story.
ensure_clt() {
    xcode-select -p >/dev/null 2>&1 && return 0
    warn "the Xcode Command Line Tools are required — they provide clang, git and make."
    if [ "$WITH_DEPS" -eq 1 ]; then
        info "Requesting the Command Line Tools"
        note "a GUI prompt appears; the download is several hundred MB"
        xcode-select --install 2>/dev/null || true
        die "re-run this script once the Command Line Tools have finished installing."
    fi
    printf '  install them with:  xcode-select --install\n' >&2
    printf '  or re-run this script with --with-deps.\n' >&2
    die "prerequisites missing."
}

install_packages() {
    local what="$1"; shift
    local pkgs="$*"
    [ -n "${pkgs// /}" ] || return 0
    step "$what: $PKG_INSTALL$pkgs"
    # shellcheck disable=SC2086
    run $PKG_INSTALL $pkgs
}

ensure_deps() {
    adopt_brew || true          # detection only; never installs anything here
    detect_pkg_mgr

    local missing; missing="$(missing_tools)"
    [ "$OS" != "macos" ] || [ -z "$missing" ] || ensure_clt

    local want_sqlite=0
    sqlite_version >/dev/null 2>&1 || want_sqlite=1

    if [ "$WITH_DEPS" -eq 1 ] &&
       { [ -n "$missing" ] || [ -z "$PKG_INSTALL" ] || [ "$want_sqlite" -eq 1 ]; }; then
        info "Installing build prerequisites"
        # No package manager at all: Homebrew is the one thing that installs the
        # same way on macOS and Linux, so it is the bootstrap for both.
        if [ -z "$PKG_INSTALL" ]; then
            install_brew && detect_pkg_mgr || \
                warn "continuing without a package manager."
        fi
        if [ -n "$PKG_INSTALL" ]; then
            local pkgs=""
            case "$missing" in *git*)  pkgs="$pkgs $(install_dep_hint git git git git git git)" ;; esac
            case "$missing" in *make*) pkgs="$pkgs $(install_dep_hint make make make make make gmake)" ;; esac
            case "$missing" in *compiler*) pkgs="$pkgs $(install_dep_hint clang llvm clang clang clang llvm)" ;; esac
            install_packages "required" "$pkgs" \
                || die "the dependency install failed; install them by hand and re-run."
            # These decide whether dyna:net's SQLite class exists at all, so
            # --with-deps installs them too rather than leaving a quiet gap.
            if [ "$want_sqlite" -eq 1 ]; then
                install_packages "optional (SQLite support)" "$(optional_pkgs)" \
                    || warn "the optional packages failed; the build will simply omit SQLite."
                sqlite_version >/dev/null 2>&1 \
                    && step "sqlite $(sqlite_version) — dyna:net SQLite will be built in" \
                    || warn "sqlite3 is still not visible to pkg-config; SQLite will be omitted."
            fi
        fi
        missing="$(missing_tools)"
    fi

    [ -z "$missing" ] && return 0

    warn "missing prerequisites:$missing"
    if [ -n "$PKG_INSTALL" ]; then
        printf '  install them with:  %s git make clang\n' "$PKG_INSTALL" >&2
        printf '  or re-run this script with --with-deps to do it automatically.\n' >&2
    elif [ "$OS" != "macos" ]; then
        printf '  no package manager was found. --with-deps installs Homebrew and uses it.\n' >&2
    fi
    die "prerequisites missing."
}

# ----------------------------------------------------------------------------- preflight report
tool_version() { "$1" --version 2>/dev/null | head -1 || echo "unknown"; }

writable_or_creatable() {
    local d="$1"
    while [ ! -d "$d" ] && [ "$d" != "/" ]; do d="$(dirname "$d")"; done
    [ -w "$d" ]
}

# df fails on a path that does not exist yet, and the build cache usually does
# not exist on a first run -- so both space probes walk up to the nearest
# EXISTING ancestor and ask that.
existing_ancestor() {
    local d="$1"
    while [ ! -d "$d" ] && [ "$d" != "/" ]; do d="$(dirname "$d")"; done
    printf '%s' "$d"
}

free_mb() { df -Pk "$(existing_ancestor "$1")" 2>/dev/null | awk 'NR==2 { printf "%d", $4/1024 }'; }

free_space() {
    df -Pk "$(existing_ancestor "$1")" 2>/dev/null | awk 'NR==2 { printf "%.1f GB free on %s", $4/1048576, $6 }'
}

preflight() {
    local missing sqlite zstd
    missing="$(missing_tools)"
    sqlite="$(sqlite_version 2>/dev/null || true)"
    zstd="$(zstd_version 2>/dev/null || true)"

    info "Checking your system"
    item "os" "$(os_pretty) ($UNAME_M)"
    if [ -n "$CC_BIN" ]; then
        item "compiler" "$CC_BIN — $(tool_version "$CC_BIN")"
    else
        item "compiler" "${C_YLW}not found${C_OFF}"
    fi
    command -v make >/dev/null 2>&1 && item "make" "$(tool_version make)" \
                                    || item "make" "${C_YLW}not found${C_OFF}"
    command -v git  >/dev/null 2>&1 && item "git"  "$(tool_version git)" \
                                    || item "git"  "${C_YLW}not found${C_OFF}"
    item "packages" "${PKG_INSTALL:-${C_YLW}no package manager found${C_OFF}}"
    if [ -n "$sqlite" ]; then
        item "sqlite" "$sqlite — dyna:net SQLite included"
    else
        item "sqlite" "${C_YLW}not found — dyna:net SQLite will be left out${C_OFF}"
    fi
    if [ -n "$zstd" ]; then
        item "zstd" "$zstd — dyna:compress zstandard included"
    else
        item "zstd" "${C_YLW}not found — dyna:compress zstd will be left out${C_OFF}"
    fi
    item "jobs" "$JOBS parallel"
    item "cache" "$BUILD_ROOT — $(free_space "$BUILD_ROOT")"

    # Catch a nearly-full disk BEFORE the build spends minutes failing on
    # ENOSPC. A from-scratch build needs a few hundred MB.
    local free_mb; free_mb="$(free_mb "$BUILD_ROOT")"
    [ -z "$free_mb" ] || [ "$free_mb" -ge 1024 ] || \
        warn "only $free_mb MB free where the build cache lives — the build needs a few hundred MB."

    local bindir; bindir="$(resolve_bindir)"
    if writable_or_creatable "$bindir"; then
        item "install to" "$bindir"
    elif command -v sudo >/dev/null 2>&1; then
        item "install to" "$bindir  ${C_YLW}(needs sudo — you will be asked for your password)${C_OFF}"
    else
        item "install to" "$bindir"
    fi
    if [ "$bindir" != "$PREFIX/bin" ]; then
        note "$PREFIX/bin is not writable and sudo is unavailable, so a per-user prefix was chosen."
    fi

    if [ -x "$bindir/$BINARY_NAME" ]; then
        PREVIOUS_VERSION="$(binary_version "$bindir/$BINARY_NAME")"
        item "replacing" "$PREVIOUS_VERSION"
    else
        item "replacing" "nothing — this is a fresh install"
    fi
    item "source" "$REPO_URL @ $REF"
    item "log" "$LOG_FILE"

    if [ "$WITH_DEPS" -eq 1 ]; then
        local todo="$missing"
        [ -n "$PKG_INSTALL" ] || todo="$todo homebrew"
        [ -n "$sqlite" ]      || todo="$todo pkg-config sqlite"
        if [ -n "${todo// /}" ]; then
            note "--with-deps will install:$todo"
        else
            note "--with-deps has nothing to install; everything is already here."
        fi
    elif [ -n "$missing" ] || [ -z "$sqlite" ]; then
        note "re-run with --with-deps to install what is missing above."
    fi

    # Piped from curl, `$0` is the shell itself -- a far more precise signal than
    # "stdin is not a tty", which is also true under CI and any redirect.
    # Nothing may be read from stdin there, so say how to pass options instead.
    case "$0" in
        bash|sh|-bash|-sh|*/bash|*/sh) PIPED=1 ;;
        *) PIPED=0 ;;
    esac
    if [ "$PIPED" -eq 1 ]; then
        note "running from a pipe — pass options after 'bash -s --', e.g."
        note "  curl -fsSL .../install.sh | bash -s -- --prefix \"\$HOME/.local\""
    fi
    debug "SRC_DIR=$SRC_DIR  PREFIX=$PREFIX  OS=$OS  WITH_DEPS=$WITH_DEPS  DRY_RUN=$DRY_RUN"
}

# ----------------------------------------------------------------------------- clone + build
fetch_source() {
    local t0=$SECONDS
    info "Fetching the source"
    debug "wiping any previous tree at $SRC_DIR"
    run rm -rf "$SRC_DIR"
    run mkdir -p "$BUILD_ROOT"

    # Fast-path for local installations from an existing clone:
    if [ -n "$LOCAL_REPO" ] && [ -f "$LOCAL_REPO/src/dynajs.c" ] && [ -f "$LOCAL_REPO/Makefile" ] && \
       [ "$REPO_URL" = "$REPO_URL_DEFAULT" ] && [ "$REF" = "$REF_DEFAULT" ]; then
        step "using local repository at $LOCAL_REPO"
        if command -v git >/dev/null 2>&1 && [ -d "$LOCAL_REPO/.git" ]; then
            run git clone --quiet --local "$LOCAL_REPO" "$SRC_DIR" >>"$LOG_FILE" 2>&1 || {
                run cp -R "$LOCAL_REPO/." "$SRC_DIR/"
            }
        else
            run cp -R "$LOCAL_REPO/." "$SRC_DIR/"
        fi
        ( cd "$SRC_DIR" && rm -rf .obj ./*.dSYM test262 ) >/dev/null 2>&1 || true
        local head; head="$(cd "$SRC_DIR" && git rev-parse --short HEAD 2>/dev/null || echo 'local')"
        step "prepared local source at $head, in $(elapsed "$t0")"
        return 0
    fi

    step "cloning $REPO_URL @ $REF"
    if ! git clone --depth 1 --branch "$REF" "$REPO_URL" "$SRC_DIR" >>"$LOG_FILE" 2>&1; then
        debug "shallow branch clone failed; retrying as a full clone (--ref may be a commit)"
        run git clone "$REPO_URL" "$SRC_DIR" >>"$LOG_FILE" 2>&1 \
            || die_log "could not clone $REPO_URL — check the URL and your network."
        ( trap - ERR; cd "$SRC_DIR" && run git checkout --quiet "$REF" ) \
            || die "'$REF' is not a branch, tag or commit in $REPO_URL"
    fi
    local head; head="$(cd "$SRC_DIR" && git rev-parse --short HEAD)"
    local when; when="$(cd "$SRC_DIR" && git log -1 --format=%cr 2>/dev/null || echo '')"
    step "at commit $head${when:+ ($when)}, in $(elapsed "$t0")"
}

# Build in the background so progress can be reported. A silent five-minute gap
# is indistinguishable from a hang, which is the complaint this answers.
build() {
    local t0=$SECONDS
    local mk_args="CONFIG_NATIVE_MODULES=y"
    # On Linux the Makefile defaults to gcc; force clang if that's what we found.
    if [ "$OS" = "linux" ] && [ "$CC_BIN" = "clang" ]; then
        mk_args="$mk_args CONFIG_CLANG=y"
    fi

    info "Building DynaJS with the native standard library"
    step "make $mk_args -j$JOBS   (a first build is typically 30-90 seconds)"
    debug "build directory: $SRC_DIR"

    ( trap - ERR; cd "$SRC_DIR" && make clean >/dev/null 2>&1 ) || true   # flag changes need a clean tree

    if [ "$VERBOSE" -eq 1 ]; then
        debug "streaming build output live"
        # shellcheck disable=SC2086
        ( trap - ERR; cd "$SRC_DIR" && make $mk_args -j"$JOBS" 2>&1 | tee -a "$LOG_FILE" ) \
            || die "the build failed — see the output above, and $LOG_FILE"
    else
        # shellcheck disable=SC2086
        ( trap - ERR; cd "$SRC_DIR" && make $mk_args -j"$JOBS" >>"$LOG_FILE" 2>&1 ) &
        local pid=$! objs=0 spins=0
        while kill -0 "$pid" 2>/dev/null; do
            sleep 2
            spins=$(( spins + 1 ))
            objs="$( { find "$SRC_DIR/.obj" -name '*.o' 2>/dev/null || true; } \
                     | wc -l | tr -d ' ')"
            # \r redraws in place on a terminal; into a file or CI log it
            # concatenates, so there print one line every 10s instead.
            if [ "$QUIET" -eq 1 ]; then :
            elif [ -t 1 ]; then
                printf '\r  %s-%s compiled %s objects (%s)   ' \
                    "$C_GRN" "$C_OFF" "$objs" "$(elapsed "$t0")"
            elif [ $(( spins % 5 )) -eq 0 ]; then
                printf '  - compiled %s objects (%s)\n' "$objs" "$(elapsed "$t0")"
            fi
        done
        [ "$spins" -eq 0 ] || [ "$QUIET" -eq 1 ] || [ ! -t 1 ] || printf '\r%*s\r' 78 ''
        wait "$pid" || die_log "the build failed after $(elapsed "$t0")."
    fi

    [ -x "$SRC_DIR/$BINARY_NAME" ] \
        || die_log "the build reported success but produced no $BINARY_NAME."
    local size; size="$(ls -lh "$SRC_DIR/$BINARY_NAME" | awk '{print $5}')"
    step "built $BINARY_NAME ($size) in $(elapsed "$t0")"
}

# ----------------------------------------------------------------------------- install (overwrite)
install_binary() {
    local bindir; bindir="$(resolve_bindir)"
    local use_sudo=0
    info "Installing"
    debug "target directory $bindir"

    if [ ! -d "$bindir" ]; then
        step "creating $bindir"
        mkdir -p "$bindir" 2>/dev/null \
            || { command -v sudo >/dev/null 2>&1 && run sudo mkdir -p "$bindir"; } \
            || die "cannot create $bindir — re-run with --prefix \"\$HOME/.local\""
    fi
    if [ ! -w "$bindir" ]; then
        if command -v sudo >/dev/null 2>&1; then
            use_sudo=1
            note "$bindir needs root; sudo will ask for your password now"
        else
            die "no write permission to $bindir and sudo is unavailable — re-run with --prefix \"\$HOME/.local\""
        fi
    fi

    local dest="$bindir/$BINARY_NAME"
    # The failure gets a message of its own: a plain ERR-trap death names a line
    # number in the installer, which is useless to someone reading their first
    # error from this script. The install output goes to the log, so die_log
    # shows what actually failed.
    if [ "$use_sudo" -eq 1 ]; then
        run sudo install -m 0755 "$SRC_DIR/$BINARY_NAME" "$dest" >>"$LOG_FILE" 2>&1 \
            || die_log "could not install to $dest — the sudo install failed."
    else
        run install -m 0755 "$SRC_DIR/$BINARY_NAME" "$dest" >>"$LOG_FILE" 2>&1 \
            || die_log "could not install to $dest — re-run with --prefix \"\$HOME/.local\" if this directory is not writable."
    fi
    INSTALLED_PATH="$dest"; INSTALLED_BINDIR="$bindir"
    step "$dest"
}

# Each probe is a DIFFERENT claim: that it runs at all, that the native modules
# are compiled in, and that one of them computes a known answer.
verify() {
    info "Verifying the install"

    run "$INSTALLED_PATH" -e 'print("ok")' >/dev/null 2>&1 \
        || die_log "the installed binary does not run."
    item "runs" "ok"

    local sha
    sha="$("$INSTALLED_PATH" -e 'import("dyna:hash").then(h => print(h.SHA256Hex("dynajs")))' 2>/dev/null || true)"
    if [ "$sha" = "217e2b7d1803b912208726fc6f9cb143768e9b0671994ad281b67f5549cbd94d" ]; then
        item "dyna:hash" "ok — SHA256(\"dynajs\") matches"
    elif [ -n "$sha" ]; then
        item "dyna:hash" "loaded, digest ${sha:0:16}..."
    else
        warn "the native standard library did not load — build with CONFIG_NATIVE_MODULES=y."
    fi

    local uuid
    uuid="$("$INSTALLED_PATH" -e 'import("dyna:uuid").then(u => print(u.v7()))' 2>/dev/null || true)"
    [ -n "$uuid" ] && item "dyna:uuid" "ok — $uuid"

    local enc
    enc="$("$INSTALLED_PATH" -e 'import("dyna:encoding").then(e => print(e.DetectEncoding(new Uint8Array([0x61,0x62]))))' 2>/dev/null || true)"
    [ -n "$enc" ] && item "dyna:encoding" "ok — detectEncoding ($enc)"

    local ptr
    ptr="$("$INSTALLED_PATH" -e 'import("dyna:json").then(j => print(j.Pointer.escape("a/b")))' 2>/dev/null || true)"
    [ -n "$ptr" ] && item "dyna:json" "ok — JSON Pointer ($ptr)"
    return 0
}

# The most common post-install surprise is a DIFFERENT dynajs earlier on PATH,
# which looks exactly like "the install did nothing".
path_advice() {
    case ":$PATH:" in
        *":$INSTALLED_BINDIR:"*)
            local found; found="$(command -v "$BINARY_NAME" 2>/dev/null || true)"
            if [ -n "$found" ] && [ "$found" != "$INSTALLED_PATH" ]; then
                warn "another $BINARY_NAME comes first on your PATH:"
                printf '    %s   (this one wins)\n' "$found" >&2
                printf '    %s   (just installed)\n' "$INSTALLED_PATH" >&2
                printf '  Remove the old one, or put %s earlier on PATH.\n' "$INSTALLED_BINDIR" >&2
            fi
            ;;
        *)
            local profile
            case "$(basename "${SHELL:-sh}")" in
                zsh)  profile="$HOME/.zshrc" ;;
                bash) [ "$OS" = "macos" ] && profile="$HOME/.bash_profile" || profile="$HOME/.bashrc" ;;
                fish) profile="$HOME/.config/fish/config.fish" ;;
                *)    profile="your shell profile" ;;
            esac
            printf '\n%s%s is not on your PATH.%s Add it to %s:\n' \
                "$C_YLW" "$INSTALLED_BINDIR" "$C_OFF" "$profile"
            if [ "$(basename "${SHELL:-sh}")" = "fish" ]; then
                printf '    fish_add_path %s\n' "$INSTALLED_BINDIR"
            else
                # shellcheck disable=SC2016  # a literal $PATH is the point
                printf '    export PATH="%s:$PATH"\n' "$INSTALLED_BINDIR"
            fi
            printf '  Until then, run it by its full path: %s\n' "$INSTALLED_PATH"
            ;;
    esac
}

report() {
    local version; version="$(binary_version "$INSTALLED_PATH")"
    printf '\n%s%s%s installed to %s in %s\n' \
        "$C_GRN$C_BOLD" "$version" "$C_OFF" "$INSTALLED_PATH" "$(elapsed)"
    [ -z "$PREVIOUS_VERSION" ] || note "replaced: $PREVIOUS_VERSION"
    path_advice
    printf '\nTry it:\n'
    printf '    %s -e '\''print(1 + 1)'\''\n' "$BINARY_NAME"
    printf '    %s -i%s\n' "$BINARY_NAME" "                      # the REPL"
    printf '    %s -e '\''import("dyna:uuid").then(u => print(u.v7()))'\''\n' "$BINARY_NAME"
    printf '\nDocs: https://github.com/corporatepiyush/dynajs#readme\n'
    printf 'Build log kept at %s\n\n' "$LOG_FILE"
}

# ----------------------------------------------------------------------------- main
main() {
    [ "$QUIET" -eq 1 ] || printf '\n%sDynaJS installer%s\n' "$C_BOLD" "$C_OFF"
    mkdir -p "$BUILD_ROOT"
    : > "$LOG_FILE"
    debug "log started at $LOG_FILE"

    # Report first, act second: --dry-run promises it downloads nothing, and
    # with --with-deps the acting step can install Homebrew.
    adopt_brew || true
    detect_pkg_mgr
    select_compiler
    preflight

    if [ "$DRY_RUN" -eq 1 ]; then
        info "Dry run — stopping here"
        step "nothing was downloaded, installed, or built."
        exit 0
    fi

    ensure_deps

    fetch_source
    build
    install_binary
    verify
    report
}
main
