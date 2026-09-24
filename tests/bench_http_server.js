/* bench_http_server.js — start the dyna:net HTTPServer for external load testing.
 * Usage: dynajs tests/bench_http_server.js [port] [workers] [uptimeMs]
 * Prints "LISTENING <port>" once bound, then serves for uptimeMs (default 30s). */
import { HTTPServer } from "dyna:net";
import * as std from "std";

const port = parseInt(scriptArgs[1] || "8098", 10);
const workers = parseInt(scriptArgs[2] || "4", 10);
const uptime = parseInt(scriptArgs[3] || "30000", 10);

const server = new HTTPServer({
    port,
    workers,
    routes: {
        "/": "hello world\n",
        "/json": { status: 200, contentType: "application/json",
                   body: JSON.stringify({ msg: "ok", engine: "dynajs" }) },
    },
});
server.start();
print("LISTENING " + server.port);

/* A benchmark harness that can wait for THIS server instead of guessing: with
 * DYNA_BENCH_PORT_FILE set, the real bound port is published there, so the
 * harness can start on port 0 and never race another run for a fixed port. */
{
    const pf = std.getenv("DYNA_BENCH_PORT_FILE");
    if (pf) { const f = std.open(pf, "w"); f.puts(String(server.port)); f.close(); }
}

std.out.flush(); /* unbuffer so a launcher can read the port immediately */

// keep the JS event loop parked (no busy-wait) while native worker threads serve
setTimeout(() => { server.close(); }, uptime);
