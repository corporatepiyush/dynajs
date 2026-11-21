#!/usr/bin/env bash
# tests/lib/tree-hash.sh [--ext c,h] PATH... -- prints a 16-hex key of the CONTENT of every file under PATH... (--ext limits
# the PATHs after it to those extensions), plus CC/CFLAGS/LDFLAGS/ISWEEP_*/OOM_*/ASAN_OPTIONS/UBSAN_OPTIONS.
# No git: a commit, an amend or a work dir without .git gives the same key for the same files.
set -u
cd "$(dirname "$0")/../.." || exit 1
files() {
  local ext="" re
  while [ $# -gt 0 ]; do
    if [ "$1" = --ext ]; then ext=$2; shift 2; continue; fi
    if [ -e "$1" ]; then
      if [ -n "$ext" ]; then re="\\.($(printf '%s' "$ext" | tr , '|'))\$"; find "$1" -type f ! -name .DS_Store | grep -E "$re"
      else find "$1" -type f ! -name .DS_Store; fi
    fi
    shift
  done
}
{ files "$@" | LC_ALL=C sort -u | tr '\n' '\0' | xargs -0 shasum 2>/dev/null
  env | grep -E '^(CC|CF|CFLAGS|LDFLAGS|ISWEEP_[A-Z]+|OOM_[A-Z]+|ASAN_OPTIONS|UBSAN_OPTIONS)=' | sort
} | shasum | cut -c1-16
