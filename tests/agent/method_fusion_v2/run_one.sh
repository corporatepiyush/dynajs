#!/bin/sh
set -eu
BIN=$1
K=$2
OUT=$3
LC_ALL=C timeout 60 "$BIN" "$K" | grep -E '^(cc_loop|method_loc|method_arg|sqrt_loop|empty_loop|call2_ctrl|task_scan|task_churn)	' >> "$OUT"
