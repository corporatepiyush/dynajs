#!/bin/sh
# Differential runner: patched dynajs output must be byte-identical to node's.
cd /tmp/work13
set -e
node slowarr_tests/diff1.js > slowarr_tests/diff_node.txt 2>&1 || { echo "NODE FAILED"; cat slowarr_tests/diff_node.txt; exit 1; }
./dynajs slowarr_tests/diff1.js > slowarr_tests/diff_dyna.txt 2>&1 || { echo "DYNAJS FAILED"; cat slowarr_tests/diff_dyna.txt; exit 1; }
if diff -u slowarr_tests/diff_node.txt slowarr_tests/diff_dyna.txt > slowarr_tests/diff_hole.txt; then
    echo "DIFF OK ($(wc -l < slowarr_tests/diff_node.txt) lines identical)"
else
    echo "DIFF FAILURES: $(grep -c '^[+-][^+-]' slowarr_tests/diff_hole.txt)"
    head -60 slowarr_tests/diff_hole.txt
    exit 1
fi
