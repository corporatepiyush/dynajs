#!/bin/bash
# materialize.sh — regenerate the probe set (3,693 probes are not carried in git;
# gen_probes.py two-phase: capture under node -> bake expectations into asserts).
# Requires: python3, node on PATH. See coverage.txt for the full matrix cell list.
set -e
cd "$(dirname "$0")"
python3 gen_probes.py            # phase 1: capture probes + run under node
python3 gen_probes.py --final    # phase 2 (if the generator supports it); else rerun as documented in gen_probes.py header
echo "materialized: $(find probes -name '*.js' | wc -l | tr -d ' ') probes"
