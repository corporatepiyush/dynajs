#!/bin/bash
set -e
cd "$(dirname "$0")/gen"
python3 gen_decimal.py
python3 gen_simd.py
python3 gen_mathx.py
python3 gen_ml.py
python3 gen_dataframe.py
python3 gen_dataframe_order.py
cd ..
echo "materialized: probes/decimal ($(ls probes/decimal | wc -l | tr -d ' ') files), probes/simd ($(ls probes/simd | wc -l | tr -d ' ')), probes/mathx ($(ls probes/mathx | wc -l | tr -d ' ')), probes/ml ($(ls probes/ml | wc -l | tr -d ' ')), probes/df ($(ls probes/df | wc -l | tr -d ' '))"
