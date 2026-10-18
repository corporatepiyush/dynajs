// A script that dies with an uncaught error while an App is live must exit with
// the error, not abort in the runtime's teardown (the App's request template
// pins Object.prototype and has to be visible to the cycle collector).
import { Exec, cwd } from "dyna:sys";
import { makeTempDir, writeFile } from "dyna:file";

let pass = 0, fail = 0;
const ok = (c, w, d) => { if (c) pass++; else { fail++; print("FAIL " + w + (d ? "  [" + d + "]" : "")); } };
const dir = makeTempDir("appdown");
const bin = cwd() + "/dynajs";

const cases = {
    "handler ran, then a top-level throw": `
        import { App } from "dyna:http";
        const app = new App({ port: 0 });
        app.get("/x", () => ({ ok: 1 }));
        app.start();
        await (await fetch("http://127.0.0.1:" + app.port + "/x")).text();
        throw new Error("boom");`,
    "handler ran with middleware and params, then a throw in a callback": `
        import { App } from "dyna:http";
        const app = new App({ port: 0 });
        app.use((req) => { req.seen = 1; });
        app.get("/u/:id", (req) => ({ id: req.params.id }));
        app.start();
        for (let i = 0; i < 5; i++) await (await fetch("http://127.0.0.1:" + app.port + "/u/" + i)).json();
        await new Promise((r) => setTimeout(r, 20));
        throw new Error("boom");`,
    "a request still open on the server side": `
        import { App } from "dyna:http";
        const app = new App({ port: 0 });
        app.get("/x", () => ({ ok: 1 }));
        app.post("/p", (req) => ({ n: req.body.length }));
        app.start();
        await (await fetch("http://127.0.0.1:" + app.port + "/p", { method: "POST", body: "abc" })).text();
        await (await fetch("http://127.0.0.1:" + app.port + "/x")).text();
        throw new Error("boom");`,
};

let n = 0;
for (const [name, src] of Object.entries(cases)) {
    const file = dir.join("case" + (n++) + ".js");
    writeFile(file, src);
    const r = Exec(bin, [file.toString()], { timeoutMs: 20000 });
    const err = String(r.stderr);
    ok(!r.timedOut, name + ": finished", err.slice(-300));
    ok(r.code === 1 && r.signal === null, name + ": exits 1 with no signal", `code=${r.code} signal=${r.signal}`);
    ok(/Error: boom/.test(err), name + ": the error is reported", err.slice(-300));
    ok(!/Assertion failed/.test(err), name + ": no teardown assertion", err.slice(-300));
}

print(`test_http_app_teardown: ${pass} passed, ${fail} failed`);
if (fail) throw new Error(`${fail} failure(s)`);
