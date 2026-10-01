#!/bin/bash
set -e
cd "$(dirname "$0")"
python3 gen_encoding.py
python3 gen_json.py
python3 gen_csv.py
python3 gen_compress.py
python3 gen_xml.py
python3 gen_yaml.py
python3 gen_html.py
python3 gen_matcher.py
python3 gen_asan.py
echo "materialized: $(find probes -name '*.js' | wc -l | tr -d ' ') probes in $(find probes -mindepth 1 -maxdepth 1 -type d | wc -l | tr -d ' ') families"
echo "reproduce the committed summary: ./run.sh all  (42 probes incl. asan)"
echo "ASan pass:  make CONFIG_NATIVE_MODULES=y CONFIG_TLS=y CONFIG_ASAN=y  (or reuse dynajs-asan); DYNAJS_BIN=...dynajs-asan OUTDIR=out_asan ./run.sh all"
