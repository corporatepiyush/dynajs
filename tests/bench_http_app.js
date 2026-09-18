/* bench_http_app.js — the JS-handler App server, for external load testing and
 * profiling. This is the path that matters (CLAUDE.md sec 10): HTTPServerAsync
 * never enters the JSContext, so it cannot show handler-path cost.
 *
 * Usage: dynajs tests/bench_http_app.js [port] [uptimeMs]
 * Prints "LISTENING <port>" once bound.
 *   wrk -t4 -c64 -d10s -s tests/wrk_rpc.lua http://127.0.0.1:8099/rpc
 */
import { App } from "dyna:net";
import * as std from "std";

const port = parseInt(scriptArgs[1] || "8099", 10);
const uptime = parseInt(scriptArgs[2] || "30000", 10);

const app = new App({ port });

app.rpc("/rpc", {
    /* trivial: isolates the RPC envelope cost (parse, dispatch, stringify) */
    ping: () => "pong",
    add: ([a, b]) => a + b,
    /* representative: builds a small object result, so the response path does
       real JSON.stringify work rather than echoing a constant */
    record: ([i]) => ({
        id: i,
        name: "record-name-" + i,
        path: "/api/v1/resource/" + i,
        active: (i & 1) === 0,
        score: i * 1.5
    }),
});

app.start();
print("LISTENING " + app.port);
std.out.flush();

setTimeout(() => { app.close(); }, uptime);
