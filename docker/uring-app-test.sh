#!/usr/bin/env bash
# Smoke + integrity test for the dyn_aio io_uring backend driving the App server.
set -u
echo "kernel: $(uname -r)  arch: $(uname -m)"
mkdir -p /tmp/sroot
echo '<h1>ok</h1>' > /tmp/sroot/index.html
head -c 3000000 /dev/urandom > /tmp/sroot/big.bin   # 3MB (exercises sendfile path)

cat > /tmp/app.mjs <<'EOF'
import { App } from "dyna:net";
const app = new App({ port: 8080 });
app.rpc("/rpc", {
  add:   ([a, b]) => a + b,
  greet: (p) => "hi " + p.name,
  slow:  (p) => new Promise(r => setTimeout(() => r("async:" + p.n), 30)),
});
app.static("/s", "/tmp/sroot", {});
app.start();
EOF

/src/dynajs /tmp/app.mjs & SRV=$!
for i in $(seq 1 80); do curl -s -o /dev/null "http://127.0.0.1:8080/rpc" -d '{}' && break; sleep 0.1; done
if ! kill -0 "$SRV" 2>/dev/null; then echo ">> SERVER DIED at startup"; exit 1; fi

fail=0
check() { printf "%-28s %s\n" "$1" "$2"; }
r=$(curl -sS "http://127.0.0.1:8080/rpc" -d '{"jsonrpc":"2.0","method":"add","params":[2,3],"id":1}')
[ "$r" = '{"jsonrpc":"2.0","result":5,"id":1}' ] && check "rpc add" OK || { check "rpc add" "FAIL: $r"; fail=1; }
r=$(curl -sS "http://127.0.0.1:8080/rpc" -d '{"jsonrpc":"2.0","method":"greet","params":{"name":"world"},"id":2}')
echo "$r" | grep -q 'hi world' && check "rpc greet" OK || { check "rpc greet" "FAIL: $r"; fail=1; }
r=$(curl -sS --max-time 5 "http://127.0.0.1:8080/rpc" -d '{"jsonrpc":"2.0","method":"slow","params":{"n":7},"id":3}')
echo "$r" | grep -q 'async:7' && check "rpc async (setTimeout)" OK || { check "rpc async" "FAIL: $r"; fail=1; }
r=$(curl -sS "http://127.0.0.1:8080/rpc" -d '[{"jsonrpc":"2.0","method":"add","params":[1,1],"id":1},{"jsonrpc":"2.0","method":"add","params":[2,2],"id":2}]')
echo "$r" | grep -q '"result":2' && echo "$r" | grep -q '"result":4' && check "rpc batch" OK || { check "rpc batch" "FAIL: $r"; fail=1; }
code=$(curl -s -o /tmp/idx -w "%{http_code}" "http://127.0.0.1:8080/s/index.html")
[ "$code" = 200 ] && grep -q ok /tmp/idx && check "static small" OK || { check "static small" "FAIL: $code"; fail=1; }
curl -s "http://127.0.0.1:8080/s/big.bin" -o /tmp/big.got
cmp -s /tmp/big.got /tmp/sroot/big.bin && check "static 3MB integrity" "OK ($(wc -c </tmp/big.got) bytes)" || { check "static 3MB" "FAIL"; fail=1; }

# throughput sanity (io_uring path)
if command -v ab >/dev/null; then :; fi
# concurrency: 40 parallel requests exercise the provided-buffer pool + -ENOBUFS
# re-arm (a small pool must not drop connections). NOTE: count matches with
# `grep -o | wc -l`, not `grep -c` -- curl responses concatenate onto one line.
# Deeper high-concurrency validation (c=1024) is in docker/profile-run.sh.
( for j in $(seq 1 40); do curl -s --max-time 5 "http://127.0.0.1:8080/rpc" \
    -d '{"jsonrpc":"2.0","method":"add","params":[1,1],"id":1}' & done; wait ) \
  > /tmp/burst.out 2>/dev/null
okc=$(grep -o '"result":2' /tmp/burst.out | wc -l | tr -d ' ')
[ "$okc" -ge 40 ] && check "concurrency (40 parallel)" "OK ($okc/40)" || { check "concurrency" "FAIL ($okc/40)"; fail=1; }

rss=$(awk '/VmRSS/{print $2}' /proc/$SRV/status 2>/dev/null)
echo "server VmRSS: ${rss} kB"
kill -0 "$SRV" 2>/dev/null && check "server alive after tests" OK || { check "server alive" "DIED"; fail=1; }
kill -9 "$SRV" 2>/dev/null

[ "$fail" = 0 ] && echo ">> io_uring backend: ALL PASS" || echo ">> io_uring backend: FAILURES"
exit $fail
