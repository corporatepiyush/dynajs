#!/usr/bin/env bash
# Canonical test262 fail-list: strip ALL progress-char runs (they interleave
# mid-line nondeterministically), keep path:line: [strict mode: ]unexpected error:
perl -pe 's/[.\-!]+//g' "$1" | grep -E "unexpected error|strict mode" \
  | perl -pe 's/(:\d+: (?:strict mode: )?unexpected error:).*$/$1/' | sort
