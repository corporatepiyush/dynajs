#!/bin/sh
D="$(cd "$(dirname "$0")" && pwd)"
E="${1:-$D/../../../dynajs-using}"
if [ "$#" -gt 0 ]; then shift; fi
exec sh "$D/../_h/run.sh" "$E" "$D" "$@"
