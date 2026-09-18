#!/bin/bash
# materialize.sh — regenerate every probe from the generators.
# Requires: python3, pip packages mmh3/xxhash/blake3 (for gen_hash), and
# python-dateutil (for gen_time RRule differential).
set -e
cd "$(dirname "$0")/gen"
python3 gen_hash.py
python3 gen_random.py
python3 gen_uuid.py
python3 gen_semver.py
python3 gen_time.py
python3 gen_bytes.py
python3 gen_encoding.py
python3 gen_serialize.py
python3 gen_structures.py
python3 gen_nprobe.py
echo "probes materialized."
