#!/bin/sh
# The LINE FORMAT of dyna:log, which tests/test_log.js cannot see: it writes to
# stderr and a process cannot read its own fd 2. This captures it and asserts
# the shape a log consumer parses.  Usage: tests/test_log_format.sh ./dynajs
set -e
DYNAJS="${1:-./dynajs}"
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
n=0; fails=0
check() { n=$((n+1)); if ! printf '%s' "$2" | grep -qE "$1"; then
    fails=$((fails+1)); echo "FAIL: $3"; echo "  line: $2"; fi }

cat > "$TMP/a.js" <<'EOF'
import { Logger } from "dyna:log";
const log = new Logger({ level: "trace", name: "api", timestamp: false,
                         base: { pid: 7 } });
log.info({ userId: 42 }, "logged in");
log.error(new Error("boom"), "failed");
log.warn('quotes " and \n newline');
log.child({ requestId: "r1" }).info("from a child");
const c = { a: 1 }; c.self = c; log.info(c, "cyclic");
EOF
OUT=$("$DYNAJS" "$TMP/a.js" 2>&1 >/dev/null)

L1=$(printf '%s\n' "$OUT" | sed -n 1p)
check '^\{.*\}$'                  "$L1" "line 1 is a single JSON object"
check '"level":"info"'            "$L1" "line 1 carries its level"
check '"name":"api"'              "$L1" "line 1 carries the logger name"
check '"pid":7'                   "$L1" "line 1 carries the base fields"
check '"userId":42'               "$L1" "line 1 carries the call fields"
check '"msg":"logged in"'         "$L1" "line 1 carries the message"
if printf '%s' "$L1" | grep -q '"time"'; then
  n=$((n+1)); fails=$((fails+1)); echo "FAIL: timestamp:false still emitted a time field"
else n=$((n+1)); fi

L2=$(printf '%s\n' "$OUT" | sed -n 2p)
check '"level":"error"'           "$L2" "line 2 level"
check '"err":\{'                  "$L2" "an Error becomes a structured err object"
check '"type":"Error"'            "$L2" "the error type is kept"
check '"message":"boom"'          "$L2" "the error message is kept"
check '"stack":'                  "$L2" "the stack is kept"

L3=$(printf '%s\n' "$OUT" | sed -n 3p)
check '\\"'                       "$L3" "a quote in the message is escaped"
check '\\n'                       "$L3" "a newline in the message is escaped"

L4=$(printf '%s\n' "$OUT" | sed -n 4p)
check '"requestId":"r1"'          "$L4" "a child binds its fields"
check '"pid":7'                   "$L4" "a child keeps the parent's base fields"

L5=$(printf '%s\n' "$OUT" | sed -n 5p)
check '\{'                        "$L5" "a cyclic object still produced a line"

# Every line must be exactly one line -- framing is what a consumer relies on.
COUNT=$(printf '%s\n' "$OUT" | grep -c '^{')
n=$((n+1))
[ "$COUNT" = 5 ] || { fails=$((fails+1)); echo "FAIL: expected 5 framed lines, got $COUNT"; }

# Cycle, DAG and depth are three different answers and the emitter must not
# conflate them: a self-reference is [Circular], a repeated node still
# serialises, and genuine nesting past the cap is [deep].
cat > "$TMP/c.js" <<'EOF'
import { Logger } from "dyna:log";
const log = new Logger({ level: "info", timestamp: false });
const c = { a: 1 }; c.self = c; log.info(c, "cyc");
const shared = { v: 1 }; log.info({ l: shared, r: shared }, "dag");
let deep = { v: 0 }, cur = deep;
for (let i = 0; i < 30; i++) { cur.next = { v: i }; cur = cur.next; }
log.info(deep, "deep");
EOF
OUT3=$("$DYNAJS" "$TMP/c.js" 2>&1 >/dev/null)
check '"self":"\[Circular\]"' "$(printf '%s\n' "$OUT3" | sed -n 1p)" "a cycle is [Circular]"
check '"l":\{"v":1\},"r":\{"v":1\}' "$(printf '%s\n' "$OUT3" | sed -n 2p)" "a repeated node is NOT a cycle"
check '\[deep\]' "$(printf '%s\n' "$OUT3" | sed -n 3p)" "nesting past the cap is [deep]"

# The ISO timestamp shape, and that it IS present when asked for.
cat > "$TMP/b.js" <<'EOF'
import { Logger } from "dyna:log";
new Logger({ level: "info", timestamp: "iso" }).info("t");
new Logger({ level: "info" }).info("t");
EOF
OUT2=$("$DYNAJS" "$TMP/b.js" 2>&1 >/dev/null)
check '"time":"[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{3}Z"' \
      "$(printf '%s\n' "$OUT2" | sed -n 1p)" "iso timestamps are RFC 3339"
check '"time":[0-9]{10,}' "$(printf '%s\n' "$OUT2" | sed -n 2p)" \
      "the default timestamp is epoch millis"

if [ "$fails" != 0 ]; then
  echo "test_log_format: $fails FAILED of $n checks"; exit 1
fi
echo "test_log_format: $n checks, 0 failures"
