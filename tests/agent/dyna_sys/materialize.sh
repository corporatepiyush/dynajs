#!/bin/bash
# materialize.sh — regenerate every probe from the python generators.
# Probe sources of truth live in gens/*.py; probes/*.js are build artifacts.
set -e
cd "$(dirname "$0")/gens"
for g in gen_file gen_sys gen_config gen_time gen_cli gen_log gen_uring gen_sqlite gen_http gen_net; do
  python3 "$g.py"
done
echo "materialized: probes/ regenerated from gens/"
