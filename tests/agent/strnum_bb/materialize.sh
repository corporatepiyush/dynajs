#!/bin/bash
# materialize.sh — regenerate the full probe set (git does not carry the 100MB
# of generated probes; the generators + node-oracle expectations are the source
# of truth). Requires: python3, node on PATH.
set -e
cd "$(dirname "$0")"
python3 gen_strings.py                    # phase G: expect_strings.js + cases.jsonl
node expect_strings.js > exp_node.jsonl   # oracle evaluation (bakes expectations)
python3 gen_strings.py --bake exp_node.jsonl
python3 gen_dtoa.py                       # rows + expectation oracle
node expect_dtoa.js > exp_dtoa.jsonl
python3 gen_dtoa.py --bake exp_dtoa.jsonl
echo "materialized: probes/strings ($(ls probes/strings | wc -l | tr -d ' ') files), probes/dtoa ($(ls probes/dtoa | wc -l | tr -d ' ') files)"
