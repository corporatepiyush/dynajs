#!/bin/bash
set -e
cd "$(dirname "$0")"
python3 gen_x1.py && python3 gen_x2.py && python3 gen_x3.py && python3 gen_x5.py && python3 gen_x6.py
echo "materialized: $(find probes -name '*.js' | wc -l | tr -d ' ') probes"
