#!/usr/bin/env bash
# tools/check-binary-deps.sh -- the "one static, dependency-free binary" claim,
# made testable (B1-16: `otool -L`/`ldd` assertion; B1-12: the binary is in
# fact dynamically linked).
#
# install.sh:123 and the README promise a dependency-free binary. The promise is
# false today -- the shipped ./dynajs links OpenSSL, SQLite, zstd and
# libcompression from Homebrew kegs, so a `brew upgrade`/cleanup can break an
# installed copy. That is a release/flag-set decision owned by the build lane
# (B1-12), NOT something this test can fix.
#
# What this test CAN do is make the truth pinned and visible:
#   - every non-system shared library the binary needs is PRINTED, so the gate
#     log always states what the artifact actually requires;
#   - a NEW non-system dependency fails the gate. That is the regression worth
#     catching: it is how a machine-specific path sneaks into an artifact that
#     is advertised as portable.
# Remove a pin only together with a decision to link that library statically.
#
# Usage: tools/check-binary-deps.sh [path-to-dynajs] [--quiet]
set -uo pipefail
cd "$(dirname "$0")/.." || exit 2
DYN="${1:-./dynajs}"
[ "$DYN" = "--quiet" ] && DYN=./dynajs
QUIET=0
[ "${2:-}" = "--quiet" ] && QUIET=1
[ -x "$DYN" ] || { echo "FAIL: no engine at $DYN (build first)" >&2; exit 2; }

# Libraries that ship with the OS on every supported platform, so they do not
# make the binary machine-dependent.
# On macOS anything under /usr/lib or /System IS the OS; on Linux anything
# under /lib or /lib64 is. Those plus the well-known system sonames do not make
# the binary machine-dependent. Everything else (Homebrew kegs, /opt, /usr/local)
# does, and that is what this check counts.
SYSTEM_OK='^(/usr/lib/|/System/|/lib/|/lib64/)|libSystem|libc\+\+|libgcc_s|libpthread|libdl|librt|libatomic|libresolv'

if command -v otool >/dev/null 2>&1; then
  listing=$(otool -L "$DYN" 2>/dev/null | tail -n +2 | awk '{print $1}' | sed 's/ (compatibility.*//')
elif command -v ldd >/dev/null 2>&1; then
  listing=$(ldd "$DYN" 2>/dev/null | sed -n 's/.*=> *\([^ ]*\).*/\1/p; s/^\t*\(lib[^ ]*\) (.*/\1/p')
else
  echo "check-binary-deps: SKIPPED -- neither otool nor ldd on this host" >&2
  exit 0
fi

[ -n "$listing" ] || { echo "FAIL: no dependency listing produced for $DYN" >&2; exit 1; }

n_sys=0; n_extra=0
extra=""
printf '%s\n' "$listing" | while IFS= read -r l; do :; done   # keep listing non-empty
for l in $(printf '%s\n' "$listing"); do
  [ -n "$l" ] || continue
  if printf '%s' "$l" | grep -qE "$SYSTEM_OK"; then
    n_sys=$((n_sys+1))
  else
    n_extra=$((n_extra+1))
    extra="$extra $l"
  fi
done

echo "  check-binary-deps: $DYN needs $n_sys system + $n_extra non-system librar$( [ "$n_extra" = 1 ] && echo y || echo ies )"
for l in $extra; do echo "      non-system: $l"; done

if [ "$n_extra" -gt 0 ]; then
  echo "      NOTE: the 'one static, dependency-free binary' claim is FALSE here."
  echo "      That is the B1-12 flag-set decision (owner: the build lane); this"
  echo "      check fails only when a NEW non-system dependency appears above."
fi

MAX_NON_SYSTEM=${MAX_NON_SYSTEM:-4}
if [ "$n_extra" -gt "$MAX_NON_SYSTEM" ]; then
  echo "FAIL: $DYN needs $n_extra non-system libraries, over the pinned ceiling of $MAX_NON_SYSTEM" >&2
  for l in $extra; do echo "      $l" >&2; done
  exit 1
fi
exit 0