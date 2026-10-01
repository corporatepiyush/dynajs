#!/bin/bash
set -e
cd "$(dirname "$0")"
python3 gen_strings.py
node expect_strings.js > exp_node.jsonl
python3 gen_strings.py --bake exp_node.jsonl
python3 gen_dtoa.py
node expect_dtoa.js > exp_dtoa.jsonl
python3 gen_dtoa.py --bake exp_dtoa.jsonl
echo "materialized: probes/strings ($(ls probes/strings | wc -l | tr -d ' ') files), probes/dtoa ($(ls probes/dtoa | wc -l | tr -d ' ') files)"
