// flags: --std
// covers: integration dyna:http.App dyna:http.WsClient -- an uncaught error ends a script with live server and client connections
// F-SSE-EXIT (2026-11-26): a script that died on an uncaught error while an App held an open SSE or
// WebSocket connection, or while a WsClient was open, aborted in JS_FreeRuntime on the
// "list_empty(&rt->gc_obj_list)" assertion (rc 134) instead of exiting with its error (rc 1). The
// connection kept C-held references to its handlers object and its own JS wrapper, invisible to the
// cycle collector, and the closures in those handlers kept the App (or WsClient) alive, so neither was
// ever finalized. App and WsClient now register a shutdown sweep that drops those references before
// the final collection; close() removes the sweep. Each case runs the script in a child process and
// asserts the error exit and the absence of the assertion text. The sweep detaches the SSE/WebSocket
// natives from the connection before dropping their wrappers: under ASan, dropping first left
// dyn_app_conn_close writing into the freed native (heap-use-after-free at runtime teardown).
// Mutants: natives not detached in the sweep (sse/sse3/ws cases abort under ASan), the App sweep not registered (sse and ws-server cases abort), the WsClient sweep not
// registered (ws-client cases abort).
import * as os from "os";
import * as std from "std";
import { suite } from "../lib/kit.js";
import { Spawn, args } from "dyna:sys";

const t = suite("http_app_exit_open_conns");

const SCRIPT = `
import { App, HTTPClient, WsClient } from "dyna:http";
import { TCPServer } from "dyna:net";
const mode = scriptArgs[scriptArgs.length - 1];
let opened = 0, srv = null;
const app = new App({ port: 0 });
app.get("/a", () => "hi");
app.sse("/e", { open: () => { opened++; }, close: () => {} });
app.ws("/w", { open: () => { opened++; }, message: () => {}, close: () => {} });
app.start();
const base = "http://127.0.0.1:" + app.port;
const c = new HTTPClient();
if (mode === "sse") { await c.getStream(base + "/e"); }
if (mode === "sse3") { for (let i = 0; i < 3; i++) await c.getStream(base + "/e"); }
if (mode === "keepalive") { await c.get(base + "/a"); opened = 1; }
if (mode === "ws") { new WsClient("ws://127.0.0.1:" + app.port + "/w", { open() {}, message() {}, close() {} }); }
if (mode === "wsraw") { srv = new TCPServer({ port: 0 }); srv.start({ data() {} }); new WsClient("ws://127.0.0.1:" + srv.port + "/w", {}); opened = 1; }
while (opened < 1) await new Promise((r) => setTimeout(r, 2));
throw new Error("deliberate " + mode);
`;

async function runChild(mode) {
    const T = `${std.getenv("TMPDIR") || "/tmp"}/dj_exitconns.${os.getpid()}.${mode}.js`;
    const f = std.open(T, "w");
    f.puts(SCRIPT);
    f.close();
    const child = new Spawn(args()[0], ["--std", T, mode]);
    const buf = new Uint8Array(4096);
    let err = "", n;
    while ((n = await child.stderr.read(buf)) > 0) err += String.fromCharCode.apply(null, buf.subarray(0, n));
    const res = await child.wait();
    child.close();
    os.remove(T);
    return { code: res.code, signal: res.signal, err };
}

for (const mode of ["sse", "sse3", "ws", "wsraw", "keepalive"]) {
    t.test("an uncaught error with an open " + mode + " connection exits with the error, not an abort", async (a) => {
        const r = await runChild(mode);
        a.ok(!/Assertion failed/.test(r.err), "no assertion at runtime teardown: " + r.err.slice(0, 200));
        a.eq(r.code, 1, "exit code 1 (signal " + r.signal + ")");
        a.match(r.err, new RegExp("deliberate " + mode));
    }, { timeoutMs: 20000 });
}

await t.run();
