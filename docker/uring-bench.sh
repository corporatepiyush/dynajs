#!/usr/bin/env bash
# Compare the dyna:net async reactor's epoll vs io_uring backends on this kernel.
# Run under `docker run --security-opt seccomp=unconfined` so io_uring syscalls
# are permitted. Reports throughput AND server CPU-per-request (io_uring's real
# edge is fewer syscalls => less CPU for the same work; on loopback the load
# generator, not the server, caps raw throughput).
set -u

echo "=== kernel ==="; uname -r
HZ=$(getconf CLK_TCK)  # jiffies per second (usually 100)

# utime+stime (jiffies) for a pid from /proc/<pid>/stat (fields 14,15)
cpu_jiffies() { awk '{print $14+$15}' "/proc/$1/stat" 2>/dev/null || echo 0; }

run_bench() {
    local bin=$1 name=$2 port=$3
    echo "=================================================================="
    echo ">> backend: ${name}"
    echo "=================================================================="
    echo "-- functional --"
    "$bin" /src/tests/test_http_async.js || { echo ">> ${name} FUNCTIONAL FAIL"; return 1; }

    cat >/tmp/srv.js <<EOF
import { HTTPServerAsync } from "dyna:net";
import * as std from "std";
const s = new HTTPServerAsync({ port: ${port}, routes: { "/": "hello world\n" }});
s.start(); print("LISTENING " + s.port); std.out.flush();
setTimeout(() => s.close(), 30000);
EOF
    # pin the single-threaded reactor to CPU 0 so CPU accounting is comparable
    taskset -c 0 "$bin" /tmp/srv.js >/tmp/srv.log 2>&1 &
    local pid=$!
    sleep 1
    local idle_rss; idle_rss=$(awk '/VmRSS/{print $2}' /proc/$pid/status)
    echo "idle VmRSS: ${idle_rss} kB"

    for conc in 100 500; do
        local n=400000
        local c0 c1 rps
        c0=$(cpu_jiffies "$pid")
        rps=$(ab -k -c "$conc" -n "$n" "http://127.0.0.1:${port}/" 2>/dev/null \
              | awk '/Requests per second/{print $4}')
        c1=$(cpu_jiffies "$pid")
        local cpu_ms=$(( (c1 - c0) * 1000 / HZ ))
        # server CPU microseconds spent per request
        local us_per_req="n/a"
        [ "$n" -gt 0 ] && us_per_req=$(awk "BEGIN{printf \"%.2f\", ${cpu_ms}*1000.0/${n}}")
        printf "  c=%-4s rps=%-10s server_cpu=%sms  cpu_per_req=%s us\n" \
               "$conc" "$rps" "$cpu_ms" "$us_per_req"
    done
    kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null || true
    return 0
}

rc=0
run_bench /usr/local/bin/dyna-epoll  "epoll"     18080 || rc=1
run_bench /usr/local/bin/dyna-uring  "io_uring"  18081 || rc=1

echo "=================================================================="
echo ">> disk I/O: dyna:uring (io_uring build only)"
echo "=================================================================="
/usr/local/bin/dyna-uring /src/tests/test_uring_disk.js || rc=1
echo "=================================================================="
[ "$rc" -eq 0 ] && echo "RESULT: both backends functional" || echo "RESULT: a backend FAILED"
exit "$rc"
