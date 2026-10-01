#!/bin/bash
set -e
cd "$(dirname "$0")"
python3 gen_probes.py
python3 gen_probes.py --final
echo "materialized: $(find probes -name '*.js' | wc -l | tr -d ' ') probes"
