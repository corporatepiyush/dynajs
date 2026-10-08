#!/usr/bin/env bash
# tools/check-help-flags.sh -- every sandbox/budget flag README documents must
# appear in `./dynajs --help` (B1-16: "flags listed in --help" had no test).
#
# The defect is real today: `--timeout-ms`, `--memory-limit` and
# `--native-memory-limit` are the three flags the README tells an untrusted-code
# user to pass, and `--help` omits all three, so `--help` cannot be used to
# discover the budget controls at all.
#
# The KNOWN_MISSING list pins that gap so it cannot be forgotten, and the rule
# is UN-ROT: if a pinned flag APPEARS in --help the check FAILS with XPASS, so
# the fix cannot land without someone closing the ticket and the entry.
#
# Usage: tools/check-help-flags.sh [path-to-dynajs] [--quiet]
set -uo pipefail
cd "$(dirname "$0")/.." || exit 2
DYN="${1:-./dynajs}"
[ "$DYN" = "--quiet" ] && DYN=./dynajs
QUIET=0
[ "${2:-}" = "--quiet" ] && QUIET=1
[ -x "$DYN" ] || { echo "FAIL: no engine at $DYN (build first)" >&2; exit 2; }

help=$("$DYN" --help 2>&1)

# Every long flag the README documents as a sandbox budget / execution control.
DOCUMENTED="--timeout-ms --memory-limit --native-memory-limit --std --strict
--script --module --no-unhandled-rejection --no-prototypes --io-threads
--include --eval --help"

# Documented, implemented, and NOT YET LISTED by --help. Each is a ticket.
KNOWN_MISSING="--timeout-ms:README calls it the loop-killer but --help omits it
--memory-limit:README calls it the JS heap cap but --help omits it
--native-memory-limit:README calls it the native-allocation cap but --help omits it"

rc=0; n=0; missing=""
for f in $DOCUMENTED; do
  n=$((n+1))
  printf '%s\n' "$help" | grep -q -- "$f" && continue
  case "
$KNOWN_MISSING" in
    *"$f:"*) ;;
    *) missing="$missing $f" ;;
  esac
done

# the un-rot direction: anything pinned as missing that now appears. KNOWN_MISSING
# is one flag per LINE, so read it line by line (word splitting would shred the
# reasons into "flags").
printf '%s\n' "$KNOWN_MISSING" | while IFS= read -r row; do
  [ -n "$row" ] || continue
  f=${row%%:*}
  if printf '%s\n' "$help" | grep -q -- "$f"; then
    echo "XPASS: $f now appears in --help -- remove it from KNOWN_MISSING in $0" >&2
    echo 1 > .check-help-flags-xpass.$$
  fi
done
[ -f .check-help-flags-xpass.$$ ] && { rm -f .check-help-flags-xpass.$$; rc=1; }

if [ -n "$missing" ]; then
  echo "FAIL: --help omits documented flags:$missing" >&2
  echo "      add them to the --help text in src/dyna-cli.c, or pin them in KNOWN_MISSING with a ticket." >&2
  rc=1
fi
[ "$rc" = 0 ] && [ "$QUIET" != 1 ] && echo "  check-help-flags: $n documented flags, $(printf '%s\n' "$KNOWN_MISSING" | grep -c ':') pinned as missing-from-help"
exit "$rc"