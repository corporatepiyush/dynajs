#!/bin/sh
# tests/agent/features_du/run.sh — run the `using` (explicit resource
# management) probes. Requires an engine built with CONFIG_USING=y
# (default builds reject `using`, matching node). usage:
#   run.sh [engine-binary] [probe-name ...]
# The engine defaults to ../../dynajs-using (the CONFIG_USING=y build).
D="$(cd "$(dirname "$0")" && pwd)"
E="${1:-$D/../../../dynajs-using}"
if [ "$#" -gt 0 ]; then shift; fi
exec sh "$D/../_h/run.sh" "$E" "$D" "$@"
