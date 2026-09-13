#!/bin/bash
# run_one.sh PROBE ENGINE BIN OUTDIR TMO
# Runs one probe under one engine. If the probe's first line declares a control
# server ("// CTL:<mode>"), starts servers/ctl.py on 127.0.0.1:0 (ephemeral),
# exports DYN_CTL_PORT, and kills the server afterwards. TERM + a stdin-EOF
# watchdog inside ctl.py mean no orphan listeners even if the runner dies.
set -u
PROBE="$1"; ENGINE="$2"; BIN="$3"; OUTDIR="$4"; TMO="$5"
BASE=$(basename "$PROBE" .js)
HERE="$(cd "$(dirname "$0")" && pwd)"
mkdir -p "$OUTDIR"

CTL_MODE=$(head -1 "$PROBE" | sed -n 's/^\/\/ CTL:\([a-z0-9-]*\).*/\1/p')
SPID=""
cleanup() {
  if [ -n "$SPID" ]; then
    kill -TERM "$SPID" 2>/dev/null
    wait "$SPID" 2>/dev/null
  fi
}
trap cleanup EXIT

if [ -n "$CTL_MODE" ]; then
  PORTFILE="$OUTDIR/$BASE.port"
  CTLLOG="$OUTDIR/$BASE.ctl.log"
  rm -f "$PORTFILE"
  python3 "$HERE/servers/ctl.py" "$CTL_MODE" --portfile "$PORTFILE" --ppid $$ \
      > "$CTLLOG" 2>&1 &
  SPID=$!
  for i in $(seq 1 50); do
    [ -s "$PORTFILE" ] && break
    kill -0 "$SPID" 2>/dev/null || break
    sleep 0.1
  done
  if [ ! -s "$PORTFILE" ]; then
    echo "FAIL ctl-server $CTL_MODE did not start (see $CTLLOG)" > "$OUTDIR/$BASE.txt"
    echo 98 > "$OUTDIR/$BASE.rc"
    exit 98
  fi
  export DYN_CTL_PORT=$(cat "$PORTFILE")
fi
export DYN_DYNAJS="$BIN"

timeout "$TMO" "$BIN" "$PROBE" > "$OUTDIR/$BASE.txt" 2>&1
RC=$?
echo "$RC" > "$OUTDIR/$BASE.rc"
if [ "$RC" -eq 124 ]; then
  echo "FAIL probe-timeout after ${TMO}s" >> "$OUTDIR/$BASE.txt"
fi
exit $RC
