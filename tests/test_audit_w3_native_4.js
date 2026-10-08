// timeout: 300
// tests/test_audit_w3_native_4.js -- audit wave 3, batch 4.
// Each section names its ORACLE and CONTROL; [red] fails on the pre-fix binary.
//   M1b-02 model fitting obeys --timeout-ms (one case per training kernel)
//   N3-07  Redis refuses commands that change how many replies arrive
// build-note: needs the native modules
import { args, Exec } from "dyna:sys";
import { Path, makeTempDir, writeFile, removeAll } from "dyna:file";
import { Redis, TCPServer } from "dyna:net";

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
{
    const data = `
const rows = 20000, cols = 20;
const X = new Float64Array(rows * cols), y = new Float64Array(rows);
let s = 1; for (let i = 0; i < X.length; i++) { s = (s * 1103515245 + 12345) & 0x7fffffff; X[i] = s / 0x7fffffff; }
for (let i = 0; i < rows; i++) y[i] = X[i * cols] + X[i * cols + 1] > 1 ? 1 : 0;
const Xs = X.subarray(0, 40 * cols), ys = y.subarray(0, 40);
`;
    const families = [
        ["LogisticRegression", `new ml.LogisticRegression({ maxIter: 50 }).fit(Xs, ys, 40, cols)`, `new ml.LogisticRegression({ maxIter: 100000, tol: 0 }).fit(X, y, rows, cols)`],
        ["RandomForestClassifier", `new ml.RandomForestClassifier({ nEstimators: 3, seed: 1 }).fit(Xs, ys, 40, cols)`, `new ml.RandomForestClassifier({ nEstimators: 20000, seed: 1 }).fit(X, y, rows, cols)`],
        ["GradientBoostingClassifier", `new ml.GradientBoostingClassifier({ nEstimators: 3, seed: 1 }).fit(Xs, ys, 40, cols)`, `new ml.GradientBoostingClassifier({ nEstimators: 20000, seed: 1 }).fit(X, y, rows, cols)`],
        ["SVC", `new ml.SVC({ maxIter: 5 }).fit(Xs, ys, 40, cols)`, `const yn = y.slice(0, 4000); for (let i = 0; i < 4000; i++) yn[i] = (i * 7919 % 13) < 6 ? 1 : 0; new ml.SVC({ kernel: "rbf", C: 1000, maxIter: 100000, tol: 1e-15 }).fit(X.subarray(0, 4000 * cols), yn, 4000, cols)`],
        ["GaussianMixture", `new ml.GaussianMixture(2, { seed: 1, maxIter: 5 }).fit(Xs, 40, cols)`, `new ml.GaussianMixture(8, { seed: 1, maxIter: 10000, tol: 0 }).fit(X, rows, cols)`],
    ];
    for (const [name, small, big] of families) {
        const r = child("fit_" + name + ".mjs", `import * as ml from "dyna:ml";${data}
${small}; print("small-ok");
${big}; print("big-finished");
`, ["--timeout-ms", "500"]);
        ok(/small-ok/.test(r.stdout), "control: a small " + name + " fit runs under --timeout-ms (" + (r.stdout + r.stderr).slice(0, 120) + ")");
        ok(!/big-finished/.test(r.stdout), name + ": the long fit did not run to completion");
        ok(/interrupted/.test(r.stderr) && r.code === 1, "[red] M1b-02: " + name + ".fit is interrupted by the deadline (code " + r.code + ", " + r.stderr.split("\n")[0].slice(0, 70) + ")");
    }
}

// ---- N3-07 --------------------------------------------------------------
// ORACLE: RESP is positional. `CLIENT REPLY OFF|SKIP` makes the server send
// fewer replies than commands and HELLO changes the wire protocol; after
// either, reply k no longer belongs to command k. The client cannot track
// that, so it must refuse to send them. The mock server counts what reaches
// it: a refused command must never appear on the wire.
// CONTROL: PING goes out and its reply comes back.
{
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
}

// ---- N3-07 against a real server ----------------------------------------
// The mock above proves nothing reaches the wire; a real Redis proves the
// reply stream stays in step. ORACLE: Redis itself -- after the refused
// commands, SET then GET of a fresh key must return exactly what was set
// (with `CLIENT REPLY SKIP` actually sent, the GET would resolve with the
// SET's "OK" or hang). Skipped loudly when no server listens on 6379.
{
    const live = new Redis({ host: "127.0.0.1", port: 6379, connectTimeoutMs: 1500, commandTimeoutMs: 3000 });
    let up = false;
    try { up = (await live.command("PING")) === "PONG"; } catch (e) {}
    if (!up) {
        print("  SKIP: no Redis on 127.0.0.1:6379 (brew services start redis); live N3-07 rows not run");
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
}

try { removeAll(TMP); } catch (e) {}
print("test_audit_w3_native_4: " + checks + " checks, " + failures + " failures");
if (failures) throw new Error("test_audit_w3_native_4: " + failures + " failures");
