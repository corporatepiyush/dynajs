#!/bin/sh
# Runs a tests/ref generator in the pinned reference environment
# (tests/ref/requirements.txt): $DYNA_REF_PYTHON, else .venv-ref/bin/python.
py="${DYNA_REF_PYTHON:-.venv-ref/bin/python}"
hint() {
  echo "tests/ref/py.sh: $1; create the reference environment with" >&2
  echo "  python3 -m venv .venv-ref && .venv-ref/bin/pip install -r tests/ref/requirements.txt" >&2
  echo "or point DYNA_REF_PYTHON at a python that has those pinned packages" >&2
}
if ! command -v "$py" >/dev/null 2>&1; then hint "no python at $py"; exit 1; fi
"$py" "$@" && exit 0
rc=$?
hint "$* failed under $py (rc $rc)"
exit "$rc"
