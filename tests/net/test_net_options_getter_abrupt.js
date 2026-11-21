// flags: --std
// timeout: 120
// covers: contract dyna:dns.DNSResolver dyna:dns.DNSServer dyna:redis.Redis dyna:redis.Redis.pipeline dyna:postgres.PostgreSQL dyna:postgres.PostgreSQL.pipeline dyna:postgres.PostgreSQL.query dyna:postgres.PostgreSQL.queryIter dyna:net.TCPServer dyna:net.TCPServer.connect dyna:net.connectHappy -- D49: every option and element read propagates a throwing getter or Proxy trap
// D49 (corenet). The Redis and PostgreSQL constructors read numeric options through a helper that returned the
// default when the getter or valueOf threw and read booleans with JS_ToBool on an unchecked get (an exception became
// true), so construction went on with the error pending and later getters ran over it. The pipeline element reads,
// PostgreSQL parameter elements and the queryIter receiver's query lookup type-tested the result first, so a throwing
// getter became "not an array" / an absent value or a module TypeError replaced it. pg_coerce_params set the
// count before the arrays, so a throwing params element in pipeline() freed through NULL (SIGSEGV). DNSResolver/DNSServer
// read server/host with JS_ToCString on an unchecked get and ignored a NULL result, leaving the error pending. Oracle: each hostile call throws
// (or rejects with) exactly the sentinel and leaves no exception pending (JSON.parse('{') must throw a SyntaxError);
// a getter that runs the GC and returns a valid value still constructs; no option is read after an abrupt one; no client
// is left open; a --timeout-ms child end cleanly.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_net_options_getter_abrupt");
import { DNSResolver, DNSServer } from "dyna:dns";
import { Redis } from "dyna:redis";
import { PostgreSQL } from "dyna:postgres";
import { TCPServer, connectHappy } from "dyna:net";
import * as std from "std";
import { Exec, args } from "dyna:sys";

const SENT = new Error("corenet getter sentinel");
const thrower = () => { throw SENT; };
const H = "127.0.0.1";
let n = 0, fails = 0;
const check = (c, m) => { n++; if (!c) { fails++; print("FAIL: " + m); } };
const slot = () => { try { JSON.parse("{"); return "no throw"; } catch (e) { return e === SENT ? "sentinel leaked" : e instanceof SyntaxError ? "clean" : "threw " + e; } };
const with_ = (base, k, get) => { const x = { ...base }; delete x[k]; Object.defineProperty(x, k, { get, enumerable: true, configurable: true }); return x; };
const trapProxy = (base, bad) => new Proxy(base, { get(t, k, r) { if (k === bad) throw SENT; return Reflect.get(t, k, r); } });
const arrProxy = (a, bad) => new Proxy(a, { get(t, k) { if (k === bad) throw SENT; return Reflect.get(t, k); } });
const open = [];
const quiet = (c) => { try { c.on("error", () => {}); } catch (e) {} return c; };

async function expectSent(name, f) {
    let got;
    try { const v = f(); if (v && typeof v.then === "function") await v; got = "no throw"; } catch (e) { got = e === SENT ? "sentinel" : "other: " + e; }
    check(got === "sentinel", name + ": " + got);
    check(slot() === "clean", name + ": pending exception after the call (" + slot() + ")");
}

const CTORS = [
    ["DNSResolver", (o) => new DNSResolver(o), { server: H, port: 53, timeoutMs: 100, ttl: 0 }, ["server", "port", "timeoutMs", "ttl"], ["port", "timeoutMs", "ttl"], ["server"]],
    ["DNSServer", (o) => new DNSServer(o), { port: 0, host: H }, ["port", "host"], ["port"], ["host"]],
    ["Redis", (o) => quiet(new Redis(o)), { host: H, port: 1, connectTimeoutMs: 200 },
        ["tls", "host", "path", "username", "password", "port", "db", "binary", "bigint", "maxReplyBytes", "maxPending", "connectTimeoutMs", "commandTimeoutMs"],
        ["port", "db", "maxReplyBytes", "maxPending", "connectTimeoutMs", "commandTimeoutMs"], ["host", "username", "password"]],
    ["PostgreSQL", (o) => quiet(new PostgreSQL(o)), { host: H, port: 1, connectTimeoutMs: 200 },
        ["tls", "host", "path", "user", "password", "database", "applicationName", "port", "raw", "bytes", "textResults", "statementCacheSize", "prepareAfter", "bigint", "insecureAuth", "maxMessageBytes", "maxResultBytes", "maxPending", "queryTimeoutMs", "connectTimeoutMs"],
        ["port", "statementCacheSize", "prepareAfter", "maxMessageBytes", "maxResultBytes", "maxPending", "queryTimeoutMs", "connectTimeoutMs"], ["host", "user", "password", "database", "applicationName"]],
    ["TCPServer", (o) => new TCPServer(o), { port: 0 }, ["port", "path", "decoder", "maxConnections", "highWaterMark", "idleTimeoutMs", "tls"],
        ["port", "maxConnections", "highWaterMark", "idleTimeoutMs"], []],
];

async function main() {
    for (const [cn, mk, base, getters, nums, strs] of CTORS) {
        for (const k of getters) {
            await expectSent(cn + " getter " + k, () => mk(with_(base, k, thrower)));
            await expectSent(cn + " proxy trap " + k, () => mk(trapProxy({ ...base, [k]: base[k] }, k)));
        }
        for (const k of getters) {
            const seen = []; let after = null;
            const o = {};
            for (const j of Object.keys(base).concat(getters)) if (!(j in o)) Object.defineProperty(o, j, { enumerable: true, configurable: true, get() { if (after) after.push(j); seen.push(j); if (j === k) { after = []; throw SENT; } return base[j]; } });
            await expectSent(cn + " stops at " + k, () => mk(o));
            check(after !== null && after.length === 0, cn + " read " + JSON.stringify(after) + " after the abrupt " + k);
        }
        for (const k of nums) await expectSent(cn + " valueOf " + k, () => mk({ ...base, [k]: { valueOf: thrower } }));
        for (const k of strs) await expectSent(cn + " toString " + k, () => mk({ ...base, [k]: { toString: thrower } }));
        for (const k of getters) {
            let c = null, err = null;
            try { c = mk(with_(base, k, () => { std.gc(); return base[k]; })); } catch (e) { err = e; }
            check(err === null && c !== null, cn + " gc getter " + k + ": " + err);
            if (c) open.push(c);
            check(slot() === "clean", cn + " gc getter " + k + ": pending exception");
        }
    }

    const r = quiet(new Redis({ host: H, port: 1, connectTimeoutMs: 200 }));
    open.push(r);
    await expectSent("Redis.pipeline outer element", () => r.pipeline(arrProxy([["PING"], ["PING"]], "1")));
    await expectSent("Redis.pipeline inner element", () => r.pipeline([["PING"], arrProxy(["ECHO", "x"], "1")]));
    await expectSent("Redis.pipeline inner length", () => r.pipeline([arrProxy(["PING"], "length")]));
    await expectSent("Redis.pipeline element toString", () => r.pipeline([["ECHO", { toString: thrower }]]));

    const p = quiet(new PostgreSQL({ host: H, port: 1, connectTimeoutMs: 200 }));
    open.push(p);
    await expectSent("PostgreSQL.pipeline outer element", () => p.pipeline(arrProxy([["select 1"], ["select 2"]], "1")));
    await expectSent("PostgreSQL.pipeline params get", () => p.pipeline([arrProxy(["select $1", [1]], "1")]));
    await expectSent("PostgreSQL.pipeline params element", () => p.pipeline([["select $1", arrProxy([1], "0")]]));
    await expectSent("PostgreSQL.query params element", () => p.query("select $1", arrProxy([1, 2], "1")));
    for (const k of ["batch", "maxRows"]) {
        await expectSent("PostgreSQL.queryIter opts getter " + k, () => p.queryIter("select 1", [], with_({}, k, thrower)));
        await expectSent("PostgreSQL.queryIter opts valueOf " + k, () => p.queryIter("select 1", [], { [k]: { valueOf: thrower } }));
    }
    await expectSent("PostgreSQL.queryIter receiver query getter", () => PostgreSQL.prototype.queryIter.call(with_(p, "query", thrower), "select 1"));
    await expectSent("PostgreSQL.queryIter proxied receiver", () => p.queryIter.call(trapProxy(p, "query"), "select 1"));

    const nop = { error() {}, connect() {} };
    for (const k of ["path", "host", "port", "tls", "decoder", "maxConnections", "highWaterMark", "idleTimeoutMs", "connectTimeoutMs"])
        await expectSent("TCPServer.connect getter " + k, () => TCPServer.connect(with_({ host: H, port: 1, connectTimeoutMs: 200 }, k, thrower), nop));
    for (const k of ["fallbackMs", "tls"])
        await expectSent("connectHappy getter " + k, () => connectHappy(H, 1, with_({}, k, thrower), nop));

    await children();
    for (const c of open) try { c.close(); } catch (e) {}
    print("net_options_getter_abrupt: " + n + " checks, " + fails + " failed");
    if (fails) std.exit(1);
}
function child(mode) {
    let caught = 0;
    for (;;) for (const [, mk, base, getters] of CTORS) for (const k of getters) try { mk(with_(base, k, thrower)); } catch (e) { caught++; }
}

async function children() {
    const SELF = "tests/net/test_net_options_getter_abrupt.js";
    const t0 = Date.now();
    const i = Exec(args()[0], ["--std", "--timeout-ms", "300", SELF, "--child", "interrupt"], { timeoutMs: 20000 });
    check(i.code !== 0 && !i.timedOut && /interrupt/i.test(i.stderr + i.stdout), "interrupt child ends: rc " + i.code + " after " + (Date.now() - t0) + " ms " + i.stderr.slice(-200));
}

if (args().includes("--child")) child(args()[args().indexOf("--child") + 1]);
else await main();
