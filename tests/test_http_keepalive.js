// flags: --std
import { HTTPServer } from "dyna:net";
import * as os from "os";
import * as std from "std";

function assert(cond, msg) { if (!cond) throw new Error("assertion failed: " + msg); }

function shOut(cmd) {
    const fds = os.pipe();
    const pid = os.exec(["/bin/sh", "-c", cmd], { stdout: fds[1], block: false });
    os.close(fds[1]);
    const f = std.fdopen(fds[0], "r");
    const out = f.readAsString();
    f.close();
    os.waitpid(pid, 0);
    return out;
}

{
    const has = shOut("command -v curl 2>/dev/null").trim().length > 0;
    if (!has) { print("test_http_keepalive: SKIPPED (curl not found)"); }
    else run();
}

function run() {
    const server = new HTTPServer({
        port: 0, workers: 4,
        routes: { "/": "hello world", "/json": { status: 200, contentType: "application/json", body: '{"a":1}' } },
    });
    server.start();
    const port = server.port;
    const base = "http://127.0.0.1:" + port;
    try {
        const urls = [base + "/", base + "/json", base + "/", base + "/json", base + "/"];
        const cmd = "curl -s -w '%{num_connects}' " +
                    urls.map((u) => "-o /dev/null " + u).join(" ");
        const out = shOut(cmd);
        assert(out.length === 5, "got a num_connects digit per request (" + JSON.stringify(out) + ")");
        assert(out[0] === "1", "first request opens one connection");
        assert(out.slice(1) === "0000", "later requests reuse it — keep-alive (" + JSON.stringify(out) + ")");

        const bodies = shOut("curl -s " + base + "/ " + base + "/json");
        assert(bodies.indexOf("hello world") >= 0, "body 1 served");
        assert(bodies.indexOf('{"a":1}') >= 0, "body 2 served on same connection");
    } finally {
        server.close();
    }
    print("test_http_keepalive: all tests passed");
}
