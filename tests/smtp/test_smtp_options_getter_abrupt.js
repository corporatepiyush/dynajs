// flags: --std
// timeout: 120
// covers: contract dyna:smtp.SmtpClient dyna:smtp.SmtpClient.send dyna:smtp.SmtpServer -- D49: every option, message field and element read propagates a throwing getter, valueOf or Proxy trap
// D49 (corenet b). SmtpClient/SmtpServer read string options with JS_ToCStringLen on an unchecked get (a TypeError replaced the
// error) and went on reading every later option with the error pending; booleans used JS_ToBool on an unchecked get; send()
// type-tested from/to/data/dsn before the exception check. Oracle: each hostile call throws exactly the sentinel and leaves no
// exception pending (JSON.parse('{') must throw a SyntaxError); no option is read after an abrupt one; a getter that runs the
// GC and returns a valid value still constructs/sends; no client or server is left open; a --timeout-ms child end cleanly.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_smtp_options_getter_abrupt");
import { SmtpClient, SmtpServer } from "dyna:smtp";
import * as std from "std";
import { Exec, args } from "dyna:sys";

const SENT = new Error("smtp getter sentinel");
const thrower = () => { throw SENT; };
const H = "127.0.0.1";
let n = 0, fails = 0;
const check = (c, m) => { n++; if (!c) { fails++; print("FAIL: " + m); } };
const slot = () => { try { JSON.parse("{"); return "no throw"; } catch (e) { return e === SENT ? "sentinel leaked" : e instanceof SyntaxError ? "clean" : "threw " + e; } };
const with_ = (base, k, get) => { const x = { ...base }; delete x[k]; Object.defineProperty(x, k, { get, enumerable: true, configurable: true }); return x; };
const trapProxy = (base, bad) => new Proxy(base, { get(t, k, r) { if (k === bad) throw SENT; return Reflect.get(t, k, r); } });
const arrProxy = (a, bad) => new Proxy(a, { get(t, k) { if (k === bad) throw SENT; return Reflect.get(t, k); } });
const open = [];

async function expectSent(name, f) {
    let got;
    try { const v = f(); if (v && typeof v.then === "function") { v.catch(() => {}); } got = "no throw"; } catch (e) { got = e === SENT ? "sentinel" : "other: " + e; }
    check(got === "sentinel", name + ": " + got);
    check(slot() === "clean", name + ": pending exception after the call (" + slot() + ")");
}

const CLI = { host: H, port: 1, tls: "none", connectTimeoutMs: 200 };
const CTORS = [
    ["SmtpClient", (o) => new SmtpClient(o), CLI,
        ["host", "tls", "ca", "servername", "name", "user", "pass", "token", "authzid", "mechanisms", "insecureAuth", "port", "connectTimeoutMs", "commandTimeoutMs", "idleTimeoutMs", "maxConnections", "maxMessagesPerConnection", "scramMaxIterations"],
        ["port", "connectTimeoutMs", "commandTimeoutMs", "idleTimeoutMs", "maxConnections", "maxMessagesPerConnection", "scramMaxIterations"]],
    ["SmtpServer", (o) => new SmtpServer(o), { port: 0, tls: "none", host: H },
        ["host", "name", "cert", "key", "tls", "mechanisms", "disable", "port", "maxMessageSize", "maxRecipients", "maxErrors", "maxMessagesPerSession", "maxAuthFailures", "maxConnections", "commandTimeoutMs", "dataTimeoutMs", "chunkSize", "requireTls", "requireAuth", "insecureAuth", "onHelo", "onAuth", "onMail", "onRcpt", "onChunk", "onData"],
        ["port", "maxMessageSize", "maxRecipients", "maxErrors", "maxMessagesPerSession", "maxAuthFailures", "maxConnections", "commandTimeoutMs", "dataTimeoutMs", "chunkSize"]],
];
const MSG = { from: "a@b.test", to: "x@y.test", data: "hi", bdat: false, dsn: { ret: "HDRS", envid: "e1", notify: ["SUCCESS"], orcpt: true } };
const MKEYS = ["from", "to", "data", "bdat", "dsn"], DKEYS = ["ret", "envid", "notify", "orcpt"];

function stopsAt(base, keys, k, mk) {
    let after = null;
    const o = {};
    for (const j of Object.keys(base).concat(keys)) if (!(j in o)) Object.defineProperty(o, j, { enumerable: true, configurable: true, get() { if (after) after.push(j); if (j === k) { after = []; throw SENT; } return base[j]; } });
    return [o, () => after];
}

async function main() {
    for (const [cn, mk, base, getters, nums] of CTORS) {
        for (const k of getters) {
            await expectSent(cn + " getter " + k, () => mk(with_(base, k, thrower)));
            await expectSent(cn + " proxy trap " + k, () => mk(trapProxy({ ...base, [k]: base[k] }, k)));
            const [o, after] = stopsAt(base, getters, k);
            await expectSent(cn + " stops at " + k, () => mk(o));
            check(after() !== null && after().length === 0, cn + " read " + JSON.stringify(after()) + " after the abrupt " + k);
        }
        for (const k of nums) await expectSent(cn + " valueOf " + k, () => mk({ ...base, [k]: { valueOf: thrower } }));
        const au = cn === "SmtpClient" ? { user: "u", pass: "p" } : {};
        await expectSent(cn + " mechanisms element", () => mk({ ...base, ...au, mechanisms: arrProxy(["PLAIN", "LOGIN"], "1") }));
        await expectSent(cn + " mechanisms element toString", () => mk({ ...base, ...au, mechanisms: ["PLAIN", { toString: thrower }] }));
        await expectSent(cn + " mechanisms length", () => mk({ ...base, ...au, mechanisms: arrProxy(["PLAIN"], "length") }));
        for (const k of getters) {
            let c = null, err = null;
            try { c = mk(with_(base, k, () => { std.gc(); return base[k]; })); } catch (e) { err = e; }
            check(err === null && c !== null, cn + " gc getter " + k + ": " + err);
            if (c) open.push(c);
            check(slot() === "clean", cn + " gc getter " + k + ": pending exception");
        }
    }
    const S = (o) => new SmtpServer({ port: 0, tls: "none", ...o });
    await expectSent("SmtpServer disable element", () => S({ disable: arrProxy(["DSN", "SIZE"], "1") }));
    await expectSent("SmtpServer disable element toString", () => S({ disable: [{ toString: thrower }] }));
    await expectSent("SmtpServer disable length", () => S({ disable: arrProxy(["DSN"], "length") }));

    const c = new SmtpClient(CLI);
    open.push(c);
    const send = (m) => c.send(m);
    for (const k of MKEYS) {
        await expectSent("send getter " + k, () => send(with_(MSG, k, thrower)));
        await expectSent("send proxy trap " + k, () => send(trapProxy({ ...MSG }, k)));
        const [o, after] = stopsAt(MSG, MKEYS, k);
        await expectSent("send stops at " + k, () => send(o));
        check(after() !== null && after().length === 0, "send read " + JSON.stringify(after()) + " after the abrupt " + k);
    }
    for (const k of DKEYS) {
        await expectSent("send dsn getter " + k, () => send({ ...MSG, dsn: with_(MSG.dsn, k, thrower) }));
        const [o, after] = stopsAt(MSG.dsn, DKEYS, k);
        await expectSent("send dsn stops at " + k, () => send({ ...MSG, dsn: o }));
        check(after() !== null && after().length === 0, "send dsn read " + JSON.stringify(after()) + " after the abrupt " + k);
    }
    await expectSent("send to element", () => send({ ...MSG, to: arrProxy(["x@y.test", "z@y.test"], "1") }));
    await expectSent("send to length", () => send({ ...MSG, to: arrProxy(["x@y.test"], "length") }));
    await expectSent("send notify element", () => send({ ...MSG, dsn: { notify: arrProxy(["SUCCESS", "DELAY"], "1") } }));
    await expectSent("send notify element toString", () => send({ ...MSG, dsn: { notify: [{ toString: thrower }] } }));

    for (const k of MKEYS) {
        let err = null, p = null;
        try { p = send(with_(MSG, k, () => { std.gc(); return MSG[k]; })); } catch (e) { err = e; }
        check(err === null && p && typeof p.then === "function", "send gc getter " + k + ": " + err);
        if (p) p.catch(() => {});
        check(slot() === "clean", "send gc getter " + k + ": pending exception");
    }
    await expectSent("send NUL in from", () => { try { send({ ...MSG, from: "a\0@b.test" }); } catch (e) { if (e instanceof TypeError) throw SENT; throw e; } });

    await children();
    for (const x of open) try { x.close(); } catch (e) {}
    print("smtp_options_getter_abrupt: " + n + " checks, " + fails + " failed");
    if (fails) std.exit(1);
}
function child(mode) {
    for (;;) for (const [, mk, base, getters] of CTORS) for (const k of getters) try { mk(with_(base, k, thrower)); } catch (e) {}
}

async function children() {
    const SELF = "tests/smtp/test_smtp_options_getter_abrupt.js";
    const t0 = Date.now();
    const i = Exec(args()[0], ["--std", "--timeout-ms", "300", SELF, "--child", "interrupt"], { timeoutMs: 20000 });
    check(i.code !== 0 && !i.timedOut && /interrupt/i.test(i.stderr + i.stdout), "interrupt child ends: rc " + i.code + " after " + (Date.now() - t0) + " ms " + i.stderr.slice(-200));
}

if (args().includes("--child")) child(args()[args().indexOf("--child") + 1]);
else await main();
