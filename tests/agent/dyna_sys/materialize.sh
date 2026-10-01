#!/bin/bash
set -e
cd "$(dirname "$0")/gens"
for g in gen_file gen_sys gen_config gen_time gen_cli gen_log gen_uring gen_sqlite gen_http gen_net; do
  python3 "$g.py"
done
echo "materialized: probes/ regenerated from gens/"
