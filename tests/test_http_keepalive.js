/* test_http_keepalive.js — prove the HTTPServer keeps a connection alive across
 * multiple requests (HTTP/1.1 keep-alive) and frames each request correctly.
 *
 * HTTPClient deliberately sends "Connection: close" (fresh connection per
 * request), so it cannot exercise server keep-alive; we drive curl instead and
 * read its %{num_connects} counter: N requests to the same host over a reused
 * connection make exactly ONE new connection. Skips cleanly if curl is absent. */
import { HTTPServer } from "dyna:net";
import * as os from "os";
import * as std from "std";

function assert(cond, msg) { if (!cond) throw new Error("assertion failed: " + msg); }

/* stdout of a shell command. std.popen was removed (it fed the command to
 * the shell verbatim -- a command-injection surface); exec with an explicit
 * argv and a pipe for stdout is the replacement. */
function shOut(cmd) {
    const fds = os.pipe();
    const pid = os.exec(["/bin/sh", "-c", cmd], { stdout: fds[1], block: false });
    os.close(fds[1]);            /* the write end lives only in the child now */
    const f = std.fdopen(fds[0], "r");
    const out = f.readAsString();
    f.close();
    os.waitpid(pid, 0);
    return out;
}

/* curl available? */
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
        /* five requests over one reused connection; -w prints new-connects per
         * transfer, so keep-alive yields "1" then "0","0","0","0". */
        const urls = [base + "/", base + "/json", base + "/", base + "/json", base + "/"];
        /* one -o /dev/null per URL: curl maps -o to URLs positionally, so a
         * single -o would let requests 2..n leak their bodies into stdout. */
        const cmd = "curl -s -w '%{num_connects}' " +
                    urls.map((u) => "-o /dev/null " + u).join(" ");
        const out = shOut(cmd);
        assert(out.length === 5, "got a num_connects digit per request (" + JSON.stringify(out) + ")");
        assert(out[0] === "1", "first request opens one connection");
        assert(out.slice(1) === "0000", "later requests reuse it — keep-alive (" + JSON.stringify(out) + ")");

        /* bodies are still correct across the persistent connection */
        const bodies = shOut("curl -s " + base + "/ " + base + "/json");
        assert(bodies.indexOf("hello world") >= 0, "body 1 served");
        assert(bodies.indexOf('{"a":1}') >= 0, "body 2 served on same connection");
    } finally {
        server.close();
    }
    print("test_http_keepalive: all tests passed");
}
