// flags: --std
// Split from tests/test_audit_w4_native_1.js: the crypto sections.
import "../lib/httpc.js";
import * as crypto from "dyna:crypto";
import * as file from "dyna:file";
import * as scrape from "dyna:scrape";
import * as os from "os";
import { HTTPClient, HTTPServer } from "dyna:net";
import { suite } from "../lib/kit.js";

const t = suite("test_crypto_audit_w4_native_1");
const per = (f, n = 20000) => { let b = Infinity; for (let r = 0; r < 5; r++) { const t0 = performance.now(); for (let i = 0; i < n; i++) f(); b = Math.min(b, (performance.now() - t0) / n * 1e6); } return b; };
// ORACLE: the reference (dynajs.d.ts) declares one blocking form of each of
// these operations and no promise-returning twin. The binary's own reflection
// is the check, walked through prototypes so non-enumerable members count.
// CONTROL: the blocking forms are present, fetch() is the promise-based
// client, and an exchange with a server in this process completes through it.
t.test("no promise twin of a blocking call is exported", async ({ ok, eq }) => {
    const names = (o) => { const s = new Set(); for (let p = o; p && p !== Object.prototype && p !== Function.prototype; p = Object.getPrototypeOf(p)) for (const k of Object.getOwnPropertyNames(p)) s.add(k); return s; };
    const gone = [
        [file, ["readFileAsync", "writeFileAsync", "copyFileAsync", "asyncStats"]],
        [file.FileWriter.prototype, ["syncAsync"]],
        [crypto.Argon2id, ["hashAsync", "verifyAsync", "asyncStats"]],
        [scrape.Fetcher.prototype, ["getAsync"]],
        [os, ["sleepAsync"]],
    ];
    const left = [];
    for (const [o, list] of gone) { const have = names(o); for (const n of list) if (have.has(n)) left.push(n); }
    eq(left.join(","), "", "[red] removed names still present");
    const own = Object.getOwnPropertyNames(Object.getPrototypeOf(new HTTPClient()));
    ok(!own.includes("getAsync") || Object.getOwnPropertyDescriptor(HTTPClient.prototype, "getAsync").value.toString().includes("PENDING"),
        "[red] HTTPClient's own getAsync is the test shim, not a native method");
    for (const [o, list] of [[file, ["readFile", "writeFile", "copyFile"]], [file.FileWriter.prototype, ["sync"]], [crypto.Argon2id, ["hash", "verify"]], [scrape.Fetcher.prototype, ["get"]], [os, ["sleep"]], [HTTPClient.prototype, ["get", "post", "request"]]])
        for (const n of list) ok(names(o).has(n), "control: " + n + " is present");
    const srv = new HTTPServer({ port: 0, routes: { "/ping": "pong" } });
    srv.start();
    const r = await fetch("http://127.0.0.1:" + srv.port + "/ping");
    eq(r.status + " " + (await r.text()), "200 pong", "control: fetch() completes against a server in this process");
    srv.close();
}, { timeoutMs: 15000 });
await t.run();
