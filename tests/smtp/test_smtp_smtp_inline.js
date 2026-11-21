// flags: --std
// timeout: 240
// covers: dyna:smtp.SmtpClient dyna:smtp.SmtpServer
// test_smtp_smtp_inline.js -- dyna:smtp with no io pool (--io-threads 0). 2026-11-27.
// Bug: engine steps were submitted with dyn_aio_offload_cpu, which fails without a pool; the job parked
// forever (client retry gated on cpu_room == 0, server retried the same failing call), so the client
// timed out waiting for the banner. Fix: parked jobs run inline on the JS thread from the outermost
// unwind (client smtp_leave at depth 1; server drain hook, hook-promise reaction at depth 0, and
// shutdown before the hook is removed), never nested inside submit(), so callers iterating conns keep
// their refs and pipelined steps do not recurse.
// Each side is proven alone: an inline child client against a pooled parent server, an inline child
// server against a pooled parent client, and both inline in one child. Oracles: RFC 5321 replies
// (250 per message, per-RCPT acceptance, 550 for a refused RCPT), exact body bytes at the server, and a
// latency bound (20 sends on one warm connection well under 20 x the 250 ms tick) so a tick-only drain fails.
// The server and fake-peer client suites (STARTTLS, implicit TLS, SCRAM, BDAT, timeouts 421, close 421,
// 4 MiB bodies through timer-settled onChunk) are re-run whole under --io-threads 0 (DYNA_SMTP_INLINE=1).
import { suite } from "../lib/kit.js";
import { SmtpClient, SmtpServer } from "dyna:smtp";
import { Spawn, args } from "dyna:sys";
import * as std from "std";
import * as os from "os";

const t = suite("test_smtp_smtp_inline");
const BIN = args()[0];
const dec = new TextDecoder();
const TMP = (std.getenv("TMPDIR") || "/tmp").replace(/\/$/, "");
const body = (i) => "From: a@b.test\r\nTo: x@y.test\r\nSubject: m" + i + "\r\n\r\nline " + i + "\r\n.dot\r\n";

function writeScript(src) {
    const p = TMP + "/smtp-inline-" + Math.random().toString(36).slice(2) + ".js";
    const f = std.open(p, "w");
    f.puts(src);
    f.close();
    return p;
}

function child(src, onLine) {
    const file = writeScript(src);
    const p = new Spawn(BIN, ["--std", "--io-threads", "0", "--timeout-ms", "60000", file], { timeoutMs: 90000 });
    const done = (async () => {
        let out = "", pend = "";
        for await (const ch of p.stdout) {
            const s = dec.decode(ch);
            out += s;
            pend += s;
            let i;
            while ((i = pend.indexOf("\n")) >= 0) {
                const line = pend.slice(0, i);
                pend = pend.slice(i + 1);
                if (onLine) await onLine(line);
            }
        }
        const w = await p.wait();
        os.remove(file);
        return { out, code: w.code };
    })();
    return done;
}

const lastJson = (out) => {
    const l = out.trim().split("\n").filter((x) => x.startsWith("{")).pop();
    return l ? JSON.parse(l) : null;
};

const CLIENT_SRC = (port) => `
import { SmtpClient } from "dyna:smtp";
const body = ${body.toString()};
const c = new SmtpClient({ host: "127.0.0.1", port: ${port}, tls: "none", name: "client.test", maxConnections: 1 });
const r = { codes: [], multi: null, refused: null, errs: [] };
const t0 = Date.now();
for (let i = 0; i < 20; i++) {
    try { r.codes.push((await c.send({ from: "a@b.test", to: "x@y.test", data: body(i) })).code); } catch (e) { r.errs.push(e.message); }
}
r.ms = Date.now() - t0;
try { const m = await c.send({ from: "a@b.test", to: ["r1@y.test", "r2@y.test", "r3@y.test"], data: body(99) }); r.multi = { code: m.code, n: (m.accepted || []).length }; } catch (e) { r.errs.push(e.message); }
try { await c.send({ from: "a@b.test", to: "bad@y.test", data: body(98) }); r.refused = "sent"; } catch (e) { r.refused = e.code; }
await c.quit();
c.close();
print(JSON.stringify(r));
`;

const SERVER_SRC = (want) => `
import { SmtpServer } from "dyna:smtp";
const dec = new TextDecoder();
const got = [];
let rcpts = 0;
const s = new SmtpServer({ port: 0, tls: "none", name: "mx.test",
    onRcpt(sess, r) { rcpts++; return !r.address.startsWith("bad@"); },
    onData(sess, m) { got.push(dec.decode(m.data)); return new Promise((res) => setTimeout(() => res(true), 2)); } });
s.listen();
print("PORT " + s.port);
const t0 = Date.now();
while (got.length < ${want} && Date.now() - t0 < 40000) await new Promise((r) => setTimeout(r, 5));
await new Promise((r) => setTimeout(r, 50));
s.close();
print(JSON.stringify({ n: got.length, rcpts, bodies: got }));
`;

function checkClient(a, r, label) {
    a.ok(r, label + ": child printed a result");
    if (!r) return;
    a.eq(r.errs.length, 0, label + ": no errors: " + r.errs.join("; "));
    a.eq(r.codes.length, 20, label + ": 20 sends");
    a.ok(r.codes.every((c) => c === 250), label + ": every send 250");
    a.ok(r.ms < 2500, label + ": 20 warm sends in " + r.ms + " ms (tick-only drain is >= 5000)");
    a.eq(r.multi && r.multi.code, 250, label + ": 3-RCPT message 250");
    a.eq(r.multi && r.multi.n, 3, label + ": 3 RCPTs accepted");
    a.eq(r.refused, "SMTP_REJECTED", label + ": refused RCPT rejects");
}

function checkBodies(a, bodies, label) {
    const norm = (b) => (b || "").replace(/\r\n$/, "");
    for (let i = 0; i < 20; i++)
        a.eq(norm(bodies[i]), norm(body(i)), label + ": body " + i + " exact");
    a.ok(bodies.slice(20).some((b) => b.includes("line 99")), label + ": 3-RCPT body delivered");
}

t.test("inline client (--io-threads 0) against a pooled server", async (a) => {
    const bodies = [];
    let rcpts = 0;
    const s = new SmtpServer({ port: 0, tls: "none", name: "mx.test",
        onRcpt(sess, r) { rcpts++; return !r.address.startsWith("bad@"); },
        onData(sess, m) { bodies.push(dec.decode(m.data)); return new Promise((res) => setTimeout(() => res(true), 2)); } });
    s.listen();
    try {
        const res = await child(CLIENT_SRC(s.port));
        a.eq(res.code, 0, "child exit: " + res.out.slice(-300));
        checkClient(a, lastJson(res.out), "client");
        a.eq(bodies.length, 21, "server got 21 messages");
        checkBodies(a, bodies, "client");
        a.eq(rcpts, 24, "server saw every RCPT");
    } finally { s.close(); }
});

t.test("inline server (--io-threads 0) against a pooled client", async (a) => {
    let sent = null;
    const res = await child(SERVER_SRC(21), async (line) => {
        if (!line.startsWith("PORT ")) return;
        const c = new SmtpClient({ host: "127.0.0.1", port: +line.slice(5), tls: "none", name: "client.test", maxConnections: 1 });
        const r = { codes: [], multi: null, refused: null, errs: [] };
        const t0 = Date.now();
        for (let i = 0; i < 20; i++) {
            try { r.codes.push((await c.send({ from: "a@b.test", to: "x@y.test", data: body(i) })).code); } catch (e) { r.errs.push(e.message); }
        }
        r.ms = Date.now() - t0;
        try { const m = await c.send({ from: "a@b.test", to: ["r1@y.test", "r2@y.test", "r3@y.test"], data: body(99) }); r.multi = { code: m.code, n: (m.accepted || []).length }; } catch (e) { r.errs.push(e.message); }
        try { await c.send({ from: "a@b.test", to: "bad@y.test", data: body(98) }); r.refused = "sent"; } catch (e) { r.refused = e.code; }
        await c.quit();
        c.close();
        sent = r;
    });
    a.eq(res.code, 0, "child exit: " + res.out.slice(-300));
    checkClient(a, sent, "server");
    const r = lastJson(res.out);
    a.ok(r, "server child printed a result");
    if (r) {
        a.eq(r.n, 21, "inline server got 21 messages");
        a.eq(r.rcpts, 24, "inline server saw every RCPT");
        checkBodies(a, r.bodies, "server");
    }
});

t.test("client and server both inline in one process", async (a) => {
    const src = `
import { SmtpClient, SmtpServer } from "dyna:smtp";
const dec = new TextDecoder();
const got = [];
const s = new SmtpServer({ port: 0, tls: "none", name: "mx.test", onData(sess, m) { got.push(dec.decode(m.data)); } });
s.listen();
const c = new SmtpClient({ host: "127.0.0.1", port: s.port, tls: "none", maxConnections: 2 });
const out = { codes: [], errs: [] };
const ps = [];
for (let i = 0; i < 8; i++) ps.push(c.send({ from: "a@example.com", to: "b@example.net", data: "Subject: x" + i + "\\r\\n\\r\\nhi " + i + "\\r\\n" }).then((r) => out.codes.push(r.code), (e) => out.errs.push(e.message)));
await Promise.all(ps);
c.close();
s.close();
out.n = got.length;
out.bodies = got.map((b) => b.replace(/\\r\\n$/, "")).sort();
print(JSON.stringify(out));
`;
    const res = await child(src);
    a.eq(res.code, 0, "child exit: " + res.out.slice(-300));
    const r = lastJson(res.out);
    a.ok(r, "child printed a result");
    if (!r) return;
    a.eq(r.errs.length, 0, "no errors: " + r.errs.join("; "));
    a.eq(r.codes.length, 8, "8 concurrent sends over 2 connections");
    a.ok(r.codes.every((c) => c === 250), "all 250");
    a.eq(r.n, 8, "server got 8");
    a.eq(JSON.stringify(r.bodies), JSON.stringify([0, 1, 2, 3, 4, 5, 6, 7].map((i) => "Subject: x" + i + "\r\n\r\nhi " + i).sort()), "exact bodies");
});

for (const name of ["test_smtp_smtp_server", "test_smtp_smtp_client_fake"]) {
    t.test("whole suite " + name + " under --io-threads 0", async (a) => {
        const p = new Spawn(BIN, ["--std", "--io-threads", "0", "tests/smtp/" + name + ".js"],
            { timeoutMs: 110000, env: { DYNA_SMTP_INLINE: "1" }, extendEnv: true });
        let out = "";
        for await (const ch of p.stdout) out += dec.decode(ch);
        const w = await p.wait();
        const fails = out.split("\n").filter((l) => /^\s*FAIL([: ]|$)/.test(l));
        a.eq(w.code, 0, name + " exit; " + fails.slice(0, 3).join(" | ") + " " + out.slice(-200));
        a.eq(fails.length, 0, name + " FAIL lines: " + fails.slice(0, 3).join(" | "));
        a.ok(/ passed, 0 failed/.test(out), name + " summary: " + out.trim().split("\n").pop());
    }, { timeoutMs: 120000 });
}

await t.run();
