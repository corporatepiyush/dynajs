// flags: --std
// timeout: 60
// covers: dyna:http.App
// A request header name is a token (RFC 9110 5.1) with no whitespace before the colon (RFC 9112 5.1): anything else is a 400,
// never a header under another name. Server twin of test_http_http_client_framing's M01 names. Controls: every tchar, obs-text
// in a value, and a long token name are served. The request scanner (src/modules/http/http-scan.c) is the check under test.
import { App } from "dyna:http";
import { connect, latin1 } from "../lib/rawpeer.js";
import { suite } from "../lib/kit.js";

const t = suite("test_http_app_header_name");
let app = null;
t.before(() => { app = new App({ port: 0 }); app.get("/", () => "root"); app.start(); });

async function status(line) {
    const c = await connect(app.port, "127.0.0.1", { connectTimeoutMs: 3000 });
    await c.write("GET / HTTP/1.1\r\nHost: h\r\n" + line + "\r\nConnection: close\r\n\r\n");
    const r = await c.readUntil("\r\n\r\n", { timeoutMs: 3000 });
    c.abort();
    if (!r.found) return r.timedOut ? "timeout" : "eof";
    return latin1(r.bytes).split("\r\n")[0].split(" ")[1] || "";
}

const BAD = [
    ["a space", "X A: v"],
    ["@", "X@A: v"],
    ["a non-ASCII byte 0xe9", "X\xe9A: v"],
    ["a non-ASCII byte 0x80", "X\x80A: v"],
    ["an empty name", ": v"],
    ["a line starting with a colon", ":X-A: v"],
    ["whitespace before the colon", "X-A : v"],
    ["a TAB", "X\tA: v"],
    ["NUL", "X\x00A: v"],
    ["DEL", "X\x7fA: v"],
    ["a separator (", "X(A: v"],
    ["a double quote", "X\"A: v"],
    ["a slash", "X/A: v"],
];
for (const [why, line] of BAD) {
    t.test("a header name with " + why + " is a 400", async (a) => {
        a.eq(await status(line), "400", "name with " + why + " refused");
    });
}
t.test("controls: legal names are served", async (a) => {
    a.eq(await status("X-A: v"), "200", "a plain token name");
    a.eq(await status("!#$%&'*+-.^_`|~09azAZ: v"), "200", "every tchar class in one name");
    a.eq(await status("X-A: caf\xe9"), "200", "obs-text belongs to values, not names");
    a.eq(await status("x".repeat(255) + ": v"), "200", "a 255-byte token name");
});
t.test("the App still answers after every refusal", async (a) => {
    a.eq(await status("X-A: v"), "200", "alive");
});
t.after(() => app.close());
await t.run();
