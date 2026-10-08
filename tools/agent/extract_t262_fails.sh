#!/usr/bin/env bash
perl -pe 's/[.\-!]+//g' "$1" | grep -E "unexpected error|strict mode" \
  | perl -pe 's/(:\d+: (?:strict mode: )?unexpected error:).*$/$1/' | sort
