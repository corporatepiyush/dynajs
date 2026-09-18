#!/bin/bash
# materialize.sh — regenerate the 4,677 generated probes (not carried in git).
# Generators bake node v22's exact outcome (error kind, values, or full event-
# trace arrays) into assert-based probes at generation time. Requires node.
set -e
cd "$(dirname "$0")"
python3 gen_x1.py && python3 gen_x2.py && python3 gen_x3.py && python3 gen_x5.py && python3 gen_x6.py
echo "materialized: $(find probes -name '*.js' | wc -l | tr -d ' ') probes"
