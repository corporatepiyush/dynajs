#!/bin/bash
# materialize.sh — regenerate the full dyna_num probe set (git does not carry
# the ~41MB of generated probes; the python generators bake python-decimal /
# numpy / ctypes-libm / closed-form goldens into the probes and are the source
# of truth). Requires: python3 with numpy on PATH.
#
# After materializing, run everything with:
#   OUTDIR=out bash run.sh probes/decimal
#   OUTDIR=out bash run.sh probes/simd
#   OUTDIR=out bash run.sh probes/mathx
#   OUTDIR=out bash run.sh probes/ml
#   OUTDIR=out bash run.sh probes/df
# (run.sh defaults DYNAJS_BIN to ../../dynajs; override for other builds,
#  e.g. the CONFIG_ASAN one: DYNAJS_BIN=../../dynajs.asan)
set -e
cd "$(dirname "$0")/gen"
python3 gen_decimal.py     # 34 probes / 70,254 asserts (python-decimal oracle)
python3 gen_simd.py        # 4 probes / 1,800 asserts (numpy bit-exact goldens)
python3 gen_mathx.py       # 4 probes (ctypes-libm bit-exact + closed forms; SLOW: gen_libm links libm via ctypes per case batch)
python3 gen_ml.py          # 4 probes / 291 asserts (numpy; exported-parameter exactness)
python3 gen_dataframe.py   # 4 probes / 216 asserts (numpy + dict groupby)
python3 gen_dataframe_order.py
cd ..
echo "materialized: probes/decimal ($(ls probes/decimal | wc -l | tr -d ' ') files), probes/simd ($(ls probes/simd | wc -l | tr -d ' ')), probes/mathx ($(ls probes/mathx | wc -l | tr -d ' ')), probes/ml ($(ls probes/ml | wc -l | tr -d ' ')), probes/df ($(ls probes/df | wc -l | tr -d ' '))"
