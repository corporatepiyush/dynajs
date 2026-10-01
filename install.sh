#!/usr/bin/env bash
#
# install.sh — install, reinstall, or upgrade the DynaJS runtime.
#
# There is exactly ONE install mechanism: the latest commit on master of the
# upstream repository. No version selection, no alternate sources — re-running
# the script is how you get current. This script always performs a CLEAN
# install: it (re)clones the source into a build cache, builds from scratch
# with the full native standard library (TLS / HTTPS / JWT included when
# OpenSSL >= 3 is visible to pkg-config), and OVERWRITES any previous `dynajs`
# binary at the install prefix. Running it again is how you upgrade (it pulls
# the latest source and rebuilds) or repair a broken install (it discards the
# old build tree entirely).
#
# It asks no questions and reads nothing from stdin, so it is safe to pipe from
# curl (pass options after `bash -s --`). Everything it is about to do is
# printed before it does it.
#
# Modes/flags (--help prints the full text):
#   --prefix DIR     binary goes to DIR/bin; default /usr/local, falling back
#                    to $HOME/.local when /usr/local/bin is unwritable and
#                    sudo is unavailable
#   --jobs N         parallel build jobs (default: CPU count)
#   --with-deps      install missing prerequisites via the system package
#                    manager (brew/apt/dnf/yum/pacman/pkg; installs Homebrew
#                    itself when no manager is found)
#   --verbose / -v   stream the build live      --quiet / -q   warnings only
#   --dry-run        print the preflight report + plan, then stop
#   --uninstall      remove the installed binary and exit (cache is kept)
#
# Environment: DYNAJS_PREFIX / DYNAJS_JOBS / DYNAJS_VERBOSE (same settings as
# the flags; a flag wins), DYNAJS_BREW_INSTALLER (where --with-deps fetches
# Homebrew from), HOMEBREW_PREFIX (read, never written), NO_COLOR (any value
# disables colour), XDG_CACHE_HOME (build-cache root, default ~/.cache).
#
# Flow: preflight — pkg-config probes for sqlite/zstd/brotli/OpenSSL run in
# PARALLEL (cached under <cache>/dynajs-build/probes, invalidated after any
# package install) — then the git clone runs IN THE BACKGROUND while the
# dependency checks proceed, then build (CONFIG_NATIVE_MODULES=y,
# CONFIG_HARDEN=y -- per-flag probed, so a foreign toolchain that rejects one
# hardening flag still gets a working build; NEVER -Werror, a new warning on
# an unknown toolchain must not break an install; plus CONFIG_TLS=y when the
# OpenSSL probe passed, plus CONFIG_CLANG=y on Linux/clang), install (sudo
# only when the bindir needs it), a smoke verify (dyna:hash known-answer,
# uuid/encoding/json/stream), and PATH advice.
#
# Exit codes / failure UX: rc=0 ok; rc=1 first error (the ERR trap names the
# line and prints the log tail); rc=130 interrupted — nothing was installed.
# The full log always lands at <cache>/dynajs-build/install.log.
#
# Gotchas:
# - Optional libraries degrade, never fail: a missing sqlite/zstd/brotli probe
#   just leaves those codecs out; only git/make/compiler are fatal.
# - macOS takes brotli from the system libcompression, not pkg-config.
# - Unwritable /usr/local/bin + no sudo silently retargets to $HOME/.local/bin
#   (the preflight report names the decision).
# - The background clone is killed on any error or interrupt (kill_clone), so a
#   failed install never leaves a half-fetched tree running.

set -Eeuo pipefail

REPO_URL="https://github.com/corporatepiyush/dynajs.git"
PREFIX_DEFAULT="/usr/local"
BINARY_NAME="dynajs"

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
CLONE_PID=""
PROBES_DIR="$BUILD_ROOT/probes"
PROBES_FRESH=0
P_SQLITE="" P_ZSTD="" P_TLS="" P_BROTLI=""

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
  --jobs N        Parallel build jobs. Default: the CPU count.
  --with-deps     Install missing build prerequisites with the system package
                  manager (brew/apt/dnf/pacman/pkg). Off by default.
  --verbose, -v   Stream the build output and print every command that runs.
  --quiet, -q     Only warnings and errors.
  --dry-run       Print the plan and the preflight report, then stop.
  --uninstall     Remove the installed dynajs binary and exit.
  --help, -h      This text.

ENVIRONMENT
  DYNAJS_PREFIX, DYNAJS_JOBS, DYNAJS_VERBOSE
                  The same settings, for when passing flags through a pipe is
                  awkward. A command-line flag wins over the variable.
  HOMEBREW_PREFIX A Homebrew installed somewhere other than the usual prefix.
                  Read, never written.
  DYNAJS_BREW_INSTALLER
                  Where --with-deps fetches Homebrew from. Default: the official
                  installer. Point it at a mirror you trust, or at a copy.
  NO_COLOR        Any value disables colour.

WHAT IT DOES
  Clones into ~/.cache/dynajs-build, builds with CONFIG_NATIVE_MODULES=y and
  CONFIG_HARDEN=y, and installs one static, dependency-free binary. The full
  build log is kept at ~/.cache/dynajs-build/install.log whether it succeeds
  or fails.

REQUIREMENTS
  git, make, and a C compiler (clang preferred, gcc accepted), on macOS, Linux
  or FreeBSD. On Windows, use WSL.
EOF
    exit 0
}

if [ -t 1 ] && [ -z "${NO_COLOR:-}" ]; then
    C_BOLD=$(printf '\033[1m'); C_RED=$(printf '\033[31m'); C_GRN=$(printf '\033[32m')
    C_YLW=$(printf '\033[33m'); C_BLU=$(printf '\033[34m'); C_DIM=$(printf '\033[2m')
    C_OFF=$(printf '\033[0m')
else
    C_BOLD=""; C_RED=""; C_GRN=""; C_YLW=""; C_BLU=""; C_DIM=""; C_OFF=""
fi

info()  { [ "$QUIET" -eq 1 ] || printf '\n%s==>%s %s%s%s\n' "$C_BLU$C_BOLD" "$C_OFF" "$C_BOLD" "$*" "$C_OFF"; }
step()  { [ "$QUIET" -eq 1 ] || printf '  %s-%s %s\n' "$C_GRN" "$C_OFF" "$*"; }
item()  { [ "$QUIET" -eq 1 ] || printf '  %s-%s %-11s %s\n' "$C_GRN" "$C_OFF" "$1" "$2"; }
note()  { [ "$QUIET" -eq 1 ] || printf '    %s%s%s\n' "$C_DIM" "$*" "$C_OFF"; }
debug() { [ "$VERBOSE" -eq 1 ] && printf '  %s[debug] %s%s\n' "$C_DIM" "$*" "$C_OFF" >&2; return 0; }
warn()  { printf '%swarning:%s %s\n' "$C_YLW" "$C_OFF" "$*" >&2; }
kill_clone() {
    [ -z "${CLONE_PID:-}" ] && return 0
    command -v pkill >/dev/null 2>&1 && pkill -TERM -P "$CLONE_PID" 2>/dev/null
    kill "$CLONE_PID" 2>/dev/null || true
    CLONE_PID=""
}
die()   { kill_clone; printf '\n%serror:%s %s\n' "$C_RED$C_BOLD" "$C_OFF" "$*" >&2; exit 1; }

binary_version() { "$1" --help 2>&1 | head -1 || true; }

run() { debug "\$ $*"; "$@"; }

elapsed() {
    local s=$(( SECONDS - ${1:-$START_TIME} ))
    if [ "$s" -ge 60 ]; then printf '%dm %02ds' $(( s / 60 )) $(( s % 60 ))
    else printf '%ds' "$s"; fi
}

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

die_log() { kill_clone; printf '\n%serror:%s %s\n' "$C_RED$C_BOLD" "$C_OFF" "$*" >&2; dump_log; exit 1; }

on_error() {
    local rc=$? line=${1:-?}
    kill_clone
    [ "$ERR_REPORTED" -eq 0 ] || exit "$rc"
    ERR_REPORTED=1
    printf '\n%serror:%s the installer stopped unexpectedly at line %s (exit %s).\n' \
        "$C_RED$C_BOLD" "$C_OFF" "$line" "$rc" >&2
    dump_log
    exit "$rc"
}
trap 'on_error $LINENO' ERR
trap 'kill_clone; printf "\n%sinterrupted.%s Nothing was installed.\n" "$C_YLW" "$C_OFF" >&2; exit 130' INT

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
                   # shellcheck disable=SC1091  # /etc/os-release is the distro's own file
                   ( . /etc/os-release; printf '%s' "${PRETTY_NAME:-Linux}" )
               else printf 'Linux %s' "$(uname -r)"; fi ;;
        *)     printf '%s %s' "$UNAME_S" "$(uname -r)" ;;
    esac
}

resolve_bindir() {
    if [ "$PREFIX" != "$PREFIX_DEFAULT" ]; then
        echo "$PREFIX/bin"; return
    fi
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
            else die "cannot remove $target — no write permission and no sudo; by hand: sudo rm -f $target"; fi
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

BREW_INSTALLER_URL="${DYNAJS_BREW_INSTALLER:-https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh}"

brew_prefixes() {
    [ -z "${HOMEBREW_PREFIX:-}" ] || printf '%s\n' "$HOMEBREW_PREFIX"
    case "$OS" in
        macos) printf '%s\n%s\n' /opt/homebrew /usr/local ;;
        linux) printf '%s\n%s\n' /home/linuxbrew/.linuxbrew "$HOME/.linuxbrew" ;;
        *)     : ;;
    esac
}

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

in_container() { [ -f /.dockerenv ] || [ -f /run/.containerenv ]; }

install_brew() {
    case "$OS" in
        macos|linux) ;;
        *) warn "Homebrew does not support $UNAME_S."; return 1 ;;
    esac
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

    local script
    script="$(curl -fsSL "$BREW_INSTALLER_URL" 2>>"$LOG_FILE")" \
        || { warn "could not download the Homebrew installer (curl exit $?) — check your network; tried $BREW_INSTALLER_URL"; return 1; }
    if [ "${#script}" -lt 1000 ]; then
        warn "the Homebrew installer came back short (${#script} bytes); refusing to run it."
        return 1
    fi
    debug "downloaded ${#script} bytes of installer"

    NONINTERACTIVE=1 /bin/bash -c "$script" 2>&1 | tee -a "$LOG_FILE" \
        || { warn "the Homebrew installer failed (exit $?) — the tail of $LOG_FILE names the reason (common: network, disk space, permissions)"; return 1; }
    adopt_brew || { warn "Homebrew installed, but 'brew' is still not on PATH — open a new terminal and re-run."; return 1; }
    step "brew ready: $(brew --version 2>/dev/null | head -1)"
}

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

CC_BIN=""
select_compiler() {
    CC_BIN=""
    if   command -v clang >/dev/null 2>&1; then CC_BIN="clang"
    elif command -v gcc   >/dev/null 2>&1; then CC_BIN="gcc"
    fi
    debug "compiler: ${CC_BIN:-none found}"
}

install_dep_hint() {
    case "$PKG_INSTALL" in
        brew*)   echo "$2" ;;
        *apt*)   echo "$3" ;;
        *dnf*|*yum*) echo "$4" ;;
        *pacman*)echo "$5" ;;
        *pkg*)   echo "$6" ;;
        *)       echo "$2" ;;
    esac
}

optional_pkgs() {
    case "$PKG_INSTALL" in
        brew*)       echo "pkg-config sqlite zstd openssl@3 brotli" ;;
        *apt*)       echo "pkg-config libsqlite3-dev libzstd-dev libbrotli-dev libssl-dev" ;;
        *dnf*|*yum*) echo "pkgconf-pkg-config sqlite-devel libzstd-devel brotli-devel openssl-devel" ;;
        *pacman*)    echo "pkgconf sqlite zstd brotli openssl" ;;
        *pkg*)       echo "pkgconf sqlite3 zstd brotli openssl" ;;
        *)           echo "" ;;
    esac
}

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

brotli_version() {
    command -v pkg-config >/dev/null 2>&1 || return 1
    local pc=""
    if command -v brew >/dev/null 2>&1; then
        pc="$(brew --prefix brotli 2>/dev/null || true)/lib/pkgconfig"
    fi
    PKG_CONFIG_PATH="$pc:${PKG_CONFIG_PATH:-}" pkg-config --modversion libbrotlienc 2>/dev/null
}

openssl_version() {
    command -v pkg-config >/dev/null 2>&1 || return 1
    local pc=""
    if command -v brew >/dev/null 2>&1; then
        pc="$(brew --prefix openssl@3 2>/dev/null || brew --prefix openssl 2>/dev/null || true)/lib/pkgconfig"
    fi
    PKG_CONFIG_PATH="$pc:${PKG_CONFIG_PATH:-}" pkg-config --modversion 'openssl >= 3.0' 2>/dev/null
}

probe_all() {
    local t0=$SECONDS
    rm -rf "$PROBES_DIR"; mkdir -p "$PROBES_DIR"
    local pids=""
    sqlite_version  >"$PROBES_DIR/sqlite" 2>/dev/null & pids="$pids $!"
    zstd_version    >"$PROBES_DIR/zstd"   2>/dev/null & pids="$pids $!"
    openssl_version >"$PROBES_DIR/tls"    2>/dev/null & pids="$pids $!"
    if [ "$OS" != "macos" ]; then
        brotli_version >"$PROBES_DIR/brotli" 2>/dev/null & pids="$pids $!"
    fi
    local p
    # shellcheck disable=SC2086  # pids is a space-separated pid list
    for p in $pids; do wait "$p" || true; done
    P_SQLITE="$(cat "$PROBES_DIR/sqlite"  2>/dev/null || true)"
    P_ZSTD="$(cat "$PROBES_DIR/zstd"    2>/dev/null || true)"
    P_TLS="$(cat "$PROBES_DIR/tls"      2>/dev/null || true)"
    P_BROTLI="$(cat "$PROBES_DIR/brotli" 2>/dev/null || true)"
    PROBES_FRESH=1
    debug "library probes (parallel) took $(elapsed "$t0")"
}

require_probes()    { [ "$PROBES_FRESH" -eq 1 ] || probe_all; }
invalidate_probes() { PROBES_FRESH=0; }

missing_tools() {
    local m=""
    command -v git  >/dev/null 2>&1 || m="$m git"
    command -v make >/dev/null 2>&1 || m="$m make"
    select_compiler
    [ -n "$CC_BIN" ] || m="$m compiler"
    printf '%s' "$m"
}

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
    local pkgs="${*# }"
    [ -n "${pkgs// /}" ] || return 0
    step "$what: $PKG_INSTALL $pkgs"
    local rc=0
    # shellcheck disable=SC2086
    run $PKG_INSTALL $pkgs 2>&1 | tee -a "$LOG_FILE" || rc=$?
    invalidate_probes
    if [ "$rc" -ne 0 ]; then
        warn "$what install exited $rc:$pkgs"
        printf '  retry (installed packages are skipped), or run by hand:  %s %s\n' \
            "$PKG_INSTALL" "$pkgs" >&2
    fi
    return "$rc"
}

ensure_deps() {
    adopt_brew || true
    detect_pkg_mgr

    local missing; missing="$(missing_tools)"
    [ "$OS" != "macos" ] || [ -z "$missing" ] || ensure_clt

    require_probes
    local want_sqlite=0
    [ -n "$P_SQLITE" ] || want_sqlite=1
    local want_brotli=0
    [ "$OS" = "macos" ] || [ -n "$P_BROTLI" ] || want_brotli=1

    if [ "$WITH_DEPS" -eq 1 ] &&
       { [ -n "$missing" ] || [ -z "$PKG_INSTALL" ] ||
         [ "$want_sqlite" -eq 1 ] || [ "$want_brotli" -eq 1 ]; }; then
        info "Installing build prerequisites"
        if [ -z "$PKG_INSTALL" ]; then
            if install_brew; then
                detect_pkg_mgr
                invalidate_probes
            else
                warn "continuing without a package manager."
            fi
        fi
        if [ -n "$PKG_INSTALL" ]; then
            local pkgs=""
            case "$missing" in *git*)  pkgs="$pkgs $(install_dep_hint git git git git git git)" ;; esac
            case "$missing" in *make*) pkgs="$pkgs $(install_dep_hint make make make make make gmake)" ;; esac
            case "$missing" in *compiler*) pkgs="$pkgs $(install_dep_hint clang llvm clang clang clang llvm)" ;; esac
            install_packages "required" "$pkgs" \
                || die "the required tools could not be installed — run the command printed above by hand, then re-run."
            if [ "$want_sqlite" -eq 1 ] || [ "$want_brotli" -eq 1 ]; then
                install_packages "optional (SQLite and brotli support)" "$(optional_pkgs)" \
                    || warn "the optional packages failed; those features will simply be omitted."
                require_probes
                if [ -n "$P_SQLITE" ]; then
                    step "sqlite $P_SQLITE — dyna:net SQLite will be built in"
                else
                    warn "sqlite3 is still not visible to pkg-config; SQLite will be omitted."
                fi
                if [ "$OS" = "macos" ]; then
                    step "brotli via the system libcompression — dyna:compress + dyna:stream codecs built in"
                elif [ -n "$P_BROTLI" ]; then
                    step "brotli $P_BROTLI — dyna:compress + dyna:stream codecs will be built in"
                else
                    warn "libbrotli is still not visible to pkg-config; brotli codecs will be omitted."
                fi
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

tool_version() { "$1" --version 2>/dev/null | head -1 || echo "unknown"; }

writable_or_creatable() {
    local d="$1"
    while [ ! -d "$d" ] && [ "$d" != "/" ]; do d="$(dirname "$d")"; done
    [ -w "$d" ]
}

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
    local missing sqlite zstd tls brotli
    missing="$(missing_tools)"
    require_probes
    sqlite="$P_SQLITE"; zstd="$P_ZSTD"; tls="$P_TLS"; brotli="$P_BROTLI"
    if [ "$OS" = "macos" ]; then brotli="system (libcompression)"; fi

    info "Checking your system"
    item "os" "$(os_pretty) ($UNAME_M)"
    if [ -n "$CC_BIN" ]; then
        item "compiler" "$CC_BIN — $(tool_version "$CC_BIN")"
    else
        item "compiler" "${C_YLW}not found${C_OFF}"
    fi
    if command -v make >/dev/null 2>&1; then item "make" "$(tool_version make)"
    else item "make" "${C_YLW}not found${C_OFF}"; fi
    if command -v git  >/dev/null 2>&1; then item "git"  "$(tool_version git)"
    else item "git"  "${C_YLW}not found${C_OFF}"; fi
    item "packages" "${PKG_INSTALL:-${C_YLW}no package manager found${C_OFF}}"
    if [ -n "$sqlite" ]; then
        item "sqlite" "$sqlite — dyna:net SQLite included"
    else
        item "sqlite" "${C_YLW}not found — dyna:net SQLite will be left out${C_OFF}"
    fi
    if [ -n "$zstd" ]; then
        item "zstd" "$zstd — dyna:compress + dyna:stream zstandard included"
    else
        item "zstd" "${C_YLW}not found — dyna:compress + dyna:stream zstd will be left out${C_OFF}"
    fi
    if [ -n "$brotli" ]; then
        item "brotli" "$brotli — dyna:compress + dyna:stream brotli included"
    else
        item "brotli" "${C_YLW}not found — dyna:compress + dyna:stream brotli will be left out${C_OFF}"
    fi
    if [ -n "$tls" ]; then
        item "tls" "$tls — HTTPS, JWT and AEAD included"
    else
        item "tls" "${C_YLW}not found — HTTPS and JWT will be left out${C_OFF}"
    fi
    item "jobs" "$JOBS parallel"
    item "cache" "$BUILD_ROOT — $(free_space "$BUILD_ROOT")"

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
    item "source" "$REPO_URL — master, latest commit"
    item "log" "$LOG_FILE"

    if [ "$WITH_DEPS" -eq 1 ]; then
        local todo="$missing"
        [ -n "$PKG_INSTALL" ] || todo="$todo homebrew"
        [ -n "$sqlite" ]      || todo="$todo pkg-config sqlite"
        [ -n "$brotli" ]      || todo="$todo brotli"
        [ -n "$tls" ]         || todo="$todo openssl"
        if [ -n "${todo// /}" ]; then
            note "--with-deps will install:$todo"
        else
            note "--with-deps has nothing to install; everything is already here."
        fi
    elif [ -n "$missing" ] || [ -z "$sqlite" ] || [ -z "$brotli" ] || [ -z "$tls" ]; then
        note "re-run with --with-deps to install what is missing above."
    fi

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

fetch_source() {
    local t0=$SECONDS
    info "Fetching the source"
    debug "wiping any previous tree at $SRC_DIR"
    run rm -rf "$SRC_DIR"
    run mkdir -p "$BUILD_ROOT"

    step "cloning $REPO_URL @ master (the latest commit)"
    run git clone --depth 1 --branch master "$REPO_URL" "$SRC_DIR" >>"$LOG_FILE" 2>&1 \
        || die_log "could not clone $REPO_URL (git exit $?) — check the URL and your network; by hand: git clone --depth 1 --branch master $REPO_URL $SRC_DIR"
    local head; head="$(cd "$SRC_DIR" && git rev-parse --short HEAD)"
    local when; when="$(cd "$SRC_DIR" && git log -1 --format=%cr 2>/dev/null || echo '')"
    step "at commit $head${when:+ ($when)}, in $(elapsed "$t0")"
}

build() {
    local t0=$SECONDS
    local mk_args="CONFIG_NATIVE_MODULES=y CONFIG_HARDEN=y"
    if [ "$OS" = "linux" ] && [ "$CC_BIN" = "clang" ]; then
        mk_args="$mk_args CONFIG_CLANG=y"
    fi
    require_probes
    if [ -n "$P_TLS" ]; then
        mk_args="$mk_args CONFIG_TLS=y"
    fi

    info "Building DynaJS with the native standard library"
    step "make $mk_args -j$JOBS   (a first build is typically 30-90 seconds)"
    debug "build directory: $SRC_DIR"

    ( trap - ERR; cd "$SRC_DIR" && make clean >/dev/null 2>&1 ) || true

    if [ "$VERBOSE" -eq 1 ]; then
        debug "streaming build output live"
        local brc=0
        # shellcheck disable=SC2086
        ( trap - ERR; cd "$SRC_DIR" && make $mk_args -j"$JOBS" 2>&1 | tee -a "$LOG_FILE" ) \
            || brc=$?
        [ "$brc" -eq 0 ] \
            || die "the build failed (exit $brc) after $(elapsed "$t0") — the first 'error:' line above names the failing file; full log: $LOG_FILE"
    else
        # shellcheck disable=SC2086
        ( trap - ERR; cd "$SRC_DIR" && make $mk_args -j"$JOBS" >>"$LOG_FILE" 2>&1 ) &
        local pid=$! objs=0 spins=0
        while kill -0 "$pid" 2>/dev/null; do
            sleep 2
            spins=$(( spins + 1 ))
            objs="$( { find "$SRC_DIR/.obj" -name '*.o' 2>/dev/null || true; } \
                     | wc -l | tr -d ' ')"
            if [ "$QUIET" -eq 1 ]; then :
            elif [ -t 1 ]; then
                printf '\r  %s-%s compiled %s objects (%s)   ' \
                    "$C_GRN" "$C_OFF" "$objs" "$(elapsed "$t0")"
            elif [ $(( spins % 5 )) -eq 0 ]; then
                printf '  - compiled %s objects (%s)\n' "$objs" "$(elapsed "$t0")"
            fi
        done
        [ "$spins" -eq 0 ] || [ "$QUIET" -eq 1 ] || [ ! -t 1 ] || printf '\r%*s\r' 78 ''
        wait "$pid" || die_log "the build failed (make exit $?) after $(elapsed "$t0") — the first 'error:' line in the tail below names the failing file; re-run with --verbose to watch it live."
    fi

    [ -x "$SRC_DIR/$BINARY_NAME" ] \
        || die_log "the build reported success but produced no $BINARY_NAME — the link step likely failed; see the log tail below."
    local size; size="$(du -h "$SRC_DIR/$BINARY_NAME" | cut -f1)"
    step "built $BINARY_NAME ($size) in $(elapsed "$t0")"
}

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
    if [ "$use_sudo" -eq 1 ]; then
        run sudo install -m 0755 "$SRC_DIR/$BINARY_NAME" "$dest" >>"$LOG_FILE" 2>&1 \
            || die_log "could not install to $dest (install exit $?) — wrong password, or no space left? See the log tail below."
    else
        run install -m 0755 "$SRC_DIR/$BINARY_NAME" "$dest" >>"$LOG_FILE" 2>&1 \
            || die_log "could not install to $dest (install exit $?; full disk?) — re-run with --prefix \"\$HOME/.local\" if it is not writable."
    fi
    INSTALLED_PATH="$dest"; INSTALLED_BINDIR="$bindir"
    step "$dest"
}

verify() {
    info "Verifying the install"

    run "$INSTALLED_PATH" -e 'print("ok")' >/dev/null 2>&1 \
        || die_log "the installed binary does not run (exit $?) — wrong architecture for $UNAME_M? Check with: file $INSTALLED_PATH"
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

    local slines
    slines="$("$INSTALLED_PATH" -e 'import("dyna:stream").then(async s => {
        const out = [];
        for await (const l of s.lines(s.fromBytes("a\nb\nc"))) out.push(l);
        print(out.length);
    })' 2>/dev/null || true)"
    [ "$slines" = "3" ] && item "dyna:stream" "ok — streaming lines()"
    return 0
}

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

phase() { local t0=$SECONDS; "$@"; debug "$1 took $(elapsed "$t0")"; }

main() {
    [ "$QUIET" -eq 1 ] || printf '\n%sDynaJS installer%s\n' "$C_BOLD" "$C_OFF"
    mkdir -p "$BUILD_ROOT"
    : > "$LOG_FILE"
    debug "log started at $LOG_FILE"

    adopt_brew || true
    detect_pkg_mgr
    select_compiler
    phase preflight

    if [ "$DRY_RUN" -eq 1 ]; then
        info "Dry run — stopping here"
        step "nothing was downloaded, installed, or built."
        exit 0
    fi

    if command -v git >/dev/null 2>&1; then
        fetch_source &
        CLONE_PID=$!
        debug "source fetch running in background (pid $CLONE_PID) during the dependency checks"
    fi

    phase ensure_deps

    if [ -n "$CLONE_PID" ]; then
        debug "waiting for the background source fetch (pid $CLONE_PID)"
        wait "$CLONE_PID" || die "the source fetch failed — the error above is the real one."
        CLONE_PID=""
    else
        phase fetch_source
    fi

    phase build
    phase install_binary
    phase verify
    phase report
}
main
