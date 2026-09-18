#!/usr/bin/env bash
# (1) io_uring vs epoll RSS + throughput at concurrency (validates the reduced
#     provided-buffer pool doesn't starve), (2) perf-profile the interpreter
#     under JetStream to find real hotspots.
set -u
echo "=== kernel $(uname -r)  arch $(uname -m) ==="
have_wrk=0; command -v wrk >/dev/null && have_wrk=1
have_perf=0; command -v perf >/dev/null && have_perf=1

cat > /tmp/hello.mjs <<'EOF'
import { App } from "dyna:net";
const app = new App({ port: 8080 });
app.rpc("/rpc", { hello: () => "Hello, World!" });
app.start();
EOF
cat > /tmp/post.lua <<'EOF'
wrk.method="POST"; wrk.body='{"jsonrpc":"2.0","method":"hello","id":1}'
wrk.headers["Content-Type"]="application/json"
EOF

echo; echo "############ EXPERIMENT 1: io_uring vs epoll (RSS + throughput) ############"
for bin in dynajs-uring dynajs-epoll; do
  echo "--- $bin ---"
  /src/$bin /tmp/hello.mjs >/dev/null 2>&1 & SRV=$!
  for i in $(seq 1 60); do curl -s -o /dev/null http://127.0.0.1:8080/rpc -d '{}' && break; sleep 0.1; done
  kill -0 $SRV 2>/dev/null || { echo "  server died"; continue; }
  idle=$(awk '/VmRSS/{print $2}' /proc/$SRV/status)
  echo "  idle RSS: ${idle} kB"
  if [ $have_wrk = 1 ]; then
    for c in 64 256 1024; do
      rps=$(wrk -t4 -c$c -d5s -s /tmp/post.lua "http://127.0.0.1:8080/rpc" 2>/dev/null | awk '/Requests\/sec/{print $2}')
      rss=$(awk '/VmRSS/{print $2}' /proc/$SRV/status)
      c0=$(awk '{print $14+$15}' /proc/$SRV/stat)
      printf "  c=%-5s rps=%-12s RSS=%s kB\n" "$c" "${rps:-?}" "$rss"
    done
  fi
  kill -9 $SRV 2>/dev/null; sleep 0.6
done

echo; echo "############ EXPERIMENT 2: perf profile the interpreter (JetStream) ############"
if [ $have_perf = 0 ]; then echo "perf not available in image"; else
for b in richards deltablue crypto; do
  cat > /tmp/prof.js <<EOF
$(cat /src/bench/jetstream/$b.js)
;(function(){ const x = new Benchmark(); for (let i = 0; i < 300; i++) x.runIteration(); })();
EOF
  echo "--- $b (top self-time functions) ---"
  if perf record -F 999 -g -o /tmp/perf.data -- /src/dynajs-epoll /tmp/prof.js >/dev/null 2>&1; then
    perf report -i /tmp/perf.data --stdio --percent-limit 1 2>/dev/null \
      | grep -E "^ +[0-9]+\.[0-9]+%" | head -12
  else
    echo "  perf record failed (need --privileged / perf_event_paranoid)"
    break
  fi
done
fi
echo "=== done ==="
