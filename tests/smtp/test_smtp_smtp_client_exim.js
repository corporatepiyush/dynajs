// flags: --std
// covers: dyna:smtp.SmtpClient
// test_smtp_smtp_client_exim.js -- dyna:smtp against exim 4.100 (strict reference server) on a private port
// with a temp config that queues and never delivers or relays (tests/lib/services.js ensureExim, stopped
// in teardown). S9 S1b, 2026-11-27. Differential: the same message and envelopes sent by swaks must leave
// the same spool body (-D file) and the same reply codes for refused recipients, relay and bad passwords.
// Covers STARTTLS with a pinned CA, AUTH PLAIN and LOGIN (exim has only the plaintext authenticator;
// SCRAM is checked against mailk's own server in ctest test-mailk-client), PIPELINING, CHUNKING/BDAT,
// 8BITMIME, DSN, the SIZE limit (1 MiB) and a TLS trust failure.
import { suite } from "../lib/kit.js";
import * as std from "std";
import { Exec } from "dyna:sys";
import { SmtpClient } from "dyna:smtp";
import { ensureExim, stopStarted } from "../lib/services.js";

const t = suite("test_smtp_smtp_client_exim");
const PORT = 25251;
let ex = null;

t.before(async () => { ex = await ensureExim(PORT); });
t.after(async () => { await stopStarted(); });

function need(a) { if (!ex || !ex.ok) a.skip("exim: " + (ex ? ex.why : "not started")); }
const client = (o = {}) => new SmtpClient({ host: "127.0.0.1", port: PORT, servername: "localhost", ca: ex.ca, name: "client.test", user: "tim", pass: "pw", ...o });

function spoolBody(marker) {
    const r = Exec("/bin/sh", ["-c", `grep -l '${marker}' '${ex.dir}'/spool/input/*-D 2>/dev/null | head -1`], { encoding: "utf8" });
    const f = r.stdout.trim();
    if (!f) return null;
    const s = std.loadFile(f);
    return s.slice(s.indexOf("\n") + 1);
}

function swaks(args, dataText) {
    const file = ex.dir + "/swaks-" + Math.random().toString(36).slice(2) + ".eml";
    if (dataText !== undefined) { const f = std.open(file, "w"); f.puts(dataText); f.close(); }
    const r = Exec("swaks", ["--server", "127.0.0.1:" + PORT, "--helo", "client.test", ...(dataText !== undefined ? ["--data", "@" + file] : []), ...args], { encoding: "utf8", timeoutMs: 30000 });
    return r.stdout + r.stderr;
}

const msg = (m, extra = "") => `From: a@b.test\r\nTo: u@test.example\r\nSubject: ${m}\r\n\r\nbody ${m}\r\n.dot line\r\n${extra}last`;

t.test("STARTTLS + AUTH PLAIN, body identical to swaks", async (a) => {
    need(a);
    const c = client();
    const r = await c.send({ from: "a@b.test", to: "u@test.example", data: msg("ours-1") });
    a.eq(r.code, 250);
    a.match(r.text, /OK id=/);
    swaks(["--to", "u@test.example", "--from", "a@b.test", "-tls", "--tls-verify", "--tls-ca-path", ex.ca, "-a", "PLAIN", "-au", "tim", "-ap", "pw"], msg("swaks-1"));
    const ours = spoolBody("body ours-1"), theirs = spoolBody("body swaks-1");
    a.ok(ours !== null && theirs !== null, "both in the spool");
    a.eq(ours, theirs.replace("swaks-1", "ours-1"), "same spool body as swaks");
    await c.quit();
});

t.test("LOGIN only; PIPELINING with several recipients", async (a) => {
    need(a);
    const c = client({ mechanisms: ["LOGIN"] });
    const r = await c.send({ from: "a@b.test", to: ["u1@test.example", "u2@test.example", "u3@test.example"], data: msg("login") });
    a.deep(r.accepted, ["u1@test.example", "u2@test.example", "u3@test.example"]);
    c.close();
});

t.test("refused recipient and relay: same codes as swaks", async (a) => {
    need(a);
    const c = client();
    const r = await c.send({ from: "a@b.test", to: ["u@test.example", "nobody@test.example", "x@other.example"], data: msg("mixed") });
    a.deep(r.accepted, ["u@test.example"]);
    a.eq(r.rejected.length, 2);
    const s1 = swaks(["--to", "nobody@test.example", "--from", "a@b.test", "-tls", "--tls-ca-path", ex.ca, "--quit-after", "RCPT"]);
    const s2 = swaks(["--to", "x@other.example", "--from", "a@b.test", "-tls", "--tls-ca-path", ex.ca, "--quit-after", "RCPT"]);
    const code = (s) => Number((s.match(/<[~*]\* +(\d{3})/) || [])[1]);
    a.eq(r.rejected[0].code, code(s1), "nobody: " + r.rejected[0].text);
    a.eq(r.rejected[1].code, code(s2), "relay: " + r.rejected[1].text);
    a.match(r.rejected[1].text, /relay not permitted/);
    const e = await a.rejects(c.send({ from: "a@b.test", to: ["nobody@test.example"], data: "x" }), { code: "SMTP_REJECTED" });
    a.eq(e && e.rejected && e.rejected[0].recipient, "nobody@test.example");
    c.close();
});

t.test("wrong password: SMTP_AUTH 535 like swaks", async (a) => {
    need(a);
    const c = client({ pass: "nope" });
    const e = await a.rejects(c.send({ from: "a@b.test", to: "u@test.example", data: "x" }), { code: "SMTP_AUTH" });
    const s = swaks(["--to", "u@test.example", "-tls", "--tls-ca-path", ex.ca, "-a", "PLAIN", "-au", "tim", "-ap", "nope", "--quit-after", "AUTH"]);
    a.match(e && e.message, /535/);
    a.match(s, /<[~*]\* +535/);
    c.close();
});

t.test("BDAT (CHUNKING) and 8BITMIME bodies arrive intact", async (a) => {
    need(a);
    const c = client();
    const body = msg("bdat", "café über\r\n");
    const r = await c.send({ from: "a@b.test", to: "u@test.example", data: body, bdat: true });
    a.eq(r.code, 250);
    const r2 = await c.send({ from: "a@b.test", to: "u@test.example", data: msg("eight", "naïve\r\n") });
    a.eq(r2.code, 250);
    const s = spoolBody("body bdat");
    a.ok(s && s.includes("café über"), "8-bit text kept in the BDAT body");
    a.ok(s && s.includes("\n.dot line\n"), "no dot-stuffing on BDAT");
    c.close();
});

t.test("DSN parameters accepted", async (a) => {
    need(a);
    const c = client();
    const r = await c.send({ from: "a@b.test", to: "u@test.example", data: msg("dsn"), dsn: { ret: "HDRS", envid: "id 42+x", notify: ["SUCCESS", "FAILURE"], orcpt: true } });
    a.eq(r.code, 250);
    const h = Exec("/bin/sh", ["-c", `grep -l 'dsn_envid' '${ex.dir}'/spool/input/*-H | xargs grep -h 'dsn_envid' | head -1`], { encoding: "utf8" }).stdout;
    a.match(h, /-dsn_envid id\+2042\+2Bx$/m, "ENVID stored in xtext form");
    c.close();
});

t.test("SIZE: over 1 MiB refused locally, never sent", async (a) => {
    need(a);
    const c = client();
    await a.rejects(c.send({ from: "a@b.test", to: "u@test.example", data: "x".repeat(900) + "\r\n" + ("y".repeat(998) + "\r\n").repeat(1100) }), { code: "SMTP_LOCAL", message: /SIZE/ });
    c.close();
});

t.test("TLS trust failure without the pinned CA", async (a) => {
    need(a);
    const c = new SmtpClient({ host: "127.0.0.1", port: PORT, servername: "localhost" });
    await a.rejects(c.send({ from: "a@b.test", to: "u@test.example", data: "x" }), { code: "SMTP_TLS" });
    c.close();
});

await t.run();
