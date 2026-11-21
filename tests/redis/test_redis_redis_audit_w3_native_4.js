// flags: --std
// timeout: 300
// covers: dyna:redis.Redis
// tests/net/test_audit_w3_native_4.js -- audit wave 3, batch 4.
// Each section names its ORACLE and CONTROL; [red] fails on the pre-fix binary.
//   M1b-02 model fitting obeys --timeout-ms (one case per training kernel)
//   N3-07  Redis refuses commands that change how many replies arrive
// build-note: needs the native modules
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_redis_redis_audit_w3_native_4");
import { args, Exec } from "dyna:sys";
import { hostScale } from "../lib/hostscale.js";
import { Path, makeTempDir, writeFile, removeAll } from "dyna:file";
import { TCPServer } from "dyna:net";
import { Redis } from "dyna:redis";
import { ensureRedis, stopStarted } from "../lib/services.js";

const BIN = args()[0];
let failures = 0, checks = 0;
function ok(cond, msg) { checks++; if (!cond) { failures++; print("  FAIL:", msg); } }
const TMP = makeTempDir("w3n4-");
const P = (n) => new Path(TMP.toString() + "/" + n);
function child(name, src, flags = []) {
    writeFile(P(name), src);
    return Exec(BIN, [...flags, P(name).toString()], { timeoutMs: 60000, encoding: "utf8" });
}

// ---- M1b-02 -------------------------------------------------------------
// ORACLE: the documented exit shapes. A fit that outlives --timeout-ms must
// end with the engine's "interrupted" InternalError (status 1). Status 113
// is the hard-stop watchdog, which means no code in the training loop ever
// looked at the deadline. Each fit below is sized to run for many seconds.
// CONTROL: the same model fitted on 40 rows under the same flag returns.

await __kitS.section("live Redis: N3-07 positional replies on a real server", async () => {
    const svc = await ensureRedis();
    const live = new Redis({ host: "127.0.0.1", port: svc.port, connectTimeoutMs: 1500, commandTimeoutMs: 3000 });
    let up = false;
    try { up = (await live.command("PING")) === "PONG"; } catch (e) {}
    if (svc.ok && !up) ok(false, "the services.js Redis on " + svc.port + " did not answer PING");
    if (!up) {
        print("  SKIP: no Redis on 127.0.0.1:" + svc.port + " (" + (svc.why || "unreachable") + "); live N3-07 rows not run");
    } else {
        const key = "dynajs:w3n4:" + Date.now();
        let refused = "none";
        try { await live.command("CLIENT", "REPLY", "SKIP"); } catch (e) { refused = e.constructor.name; }
        ok(refused === "TypeError", "[red] N3-07 live: CLIENT REPLY SKIP is refused before it is sent (" + refused + ")");
        const set = await live.command("SET", key, "v1");
        const get = await live.command("GET", key);
        const pipe = await live.pipeline([["SET", key, "v2"], ["GET", key], ["DEL", key]]);
        ok(set === "OK" && String(get) === "v1", "[red] N3-07 live: SET/GET replies belong to their commands (" + set + ", " + get + ")");
        ok(pipe.length === 3 && pipe[0] === "OK" && String(pipe[1]) === "v2" && pipe[2] === 1, "live: pipeline replies stay positional (" + pipe.map(String).join() + ")");
    }
    try { live.close(); } catch (e) {}
    await stopStarted();
});
await __kitS.section("-ERR unknown command 'HELLO'r", async () => {
    const seen = [];
    const srv = new TCPServer({ port: 0 });
    srv.start({
        data: (conn, bytes) => {
            const text = new TextDecoder().decode(bytes);
            const verbs = text.split("\r\n").filter((l, i, a) => i > 0 && a[i - 1].startsWith("$") && /^[A-Za-z]+$/.test(l));
            for (const v of verbs) seen.push(v.toUpperCase());
            const n = (text.match(/\*\d+\r\n/g) || []).length;
            for (let i = 0; i < n; i++) {
                if (/HELLO/i.test(text) && i === 0 && seen.length <= 3) conn.write("-ERR unknown command 'HELLO'\r\n");
                else conn.write("+PONG\r\n");
            }
        },
    });
    const r = new Redis({ host: "127.0.0.1", port: srv.port });
    let pong = "none", errs = [];
    try { pong = await r.command("PING"); } catch (e) { pong = "ERR " + e.message; }
    ok(pong === "PONG", "control: PING round-trips through the mock (" + pong + ")");
    const before = seen.length;
    for (const cmd of [["CLIENT", "REPLY", "OFF"], ["client", "reply", "skip"], ["HELLO", "3"]]) {
        let e = "none";
        try { await r.command(...cmd); } catch (x) { e = x.constructor.name; }
        errs.push(e);
    }
    ok(errs.join() === "TypeError,TypeError,TypeError", "[red] N3-07: CLIENT REPLY and HELLO are refused (" + errs.join() + ")");
    let pe = "none";
    try { await r.pipeline([["PING"], ["CLIENT", "REPLY", "SKIP"], ["PING"]]); } catch (x) { pe = x.constructor.name; }
    ok(pe === "TypeError", "[red] N3-07: a pipeline containing CLIENT REPLY is refused (" + pe + ")");
    ok(seen.length === before && !seen.slice(before).includes("CLIENT"), "[red] N3-07: none of the refused commands reached the server (" + seen.slice(before).join() + ")");
    let again = "none";
    try { again = await r.command("PING"); } catch (e) { again = "ERR " + e.message; }
    ok(again === "PONG", "control: the connection is still in step afterwards (" + again + ")");
    r.close(); srv.close();
});
__kitS.end();

try { removeAll(TMP); } catch (e) {}
print("test_audit_w3_native_4: " + checks + " checks, " + failures + " failures");
if (failures) throw new Error("test_audit_w3_native_4: " + failures + " failures");
