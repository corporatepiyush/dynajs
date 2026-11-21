// flags: --std
// timeout: 240
// covers: contract dyna:log.Logger Logger.child -- D49: option getters propagate; a throwing array element or length is rendered, never left pending
// st1s D49: dyn_logger_opts read name, timestamp, format, pid, hostname, base, mkdir, rollover (size, frequency, count, symlink)
// and dest, and Logger.child read level, then type-tested the result before any JS_IsException check: a throwing getter was
// read as absent and construction went on with the error pending. dyn_line_value rendered a throwing array element (or a
// Proxy array's throwing length) as null and left the error pending after the log call returned; it now writes
// "[unserializable]" like the object path, drops a catchable error and keeps an uncatchable one. Oracle: every option
// getter throws exactly the sentinel, a log line holds the marker and nothing stays pending; GC inside every option getter
// keeps the logger intact; a --timeout-ms interrupt inside an option getter or an element getter ends a catch-all loop.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_log_getter_abrupt");
import { Exec, args } from "dyna:sys";
import * as std from "std";
const SELF = "tests/log/test_log_getter_abrupt.js";
const SENT = new Error("test_log_getter_abrupt sentinel");
const thrower = () => { throw SENT; };
const spin = () => { let i = 0; for (;;) i++; };
const run = (f) => { try { return { v: f() }; } catch (e) { return { e }; } };
const with_ = (base, k, get = thrower) => { const x = { ...base }; delete x[k]; Object.defineProperty(x, k, { get, enumerable: true, configurable: true }); return x; };
const gcw = (base) => { const x = Array.isArray(base) ? [] : {}; for (const k of Object.keys(base)) { const v = base[k]; Object.defineProperty(x, k, { get() { std.gc(); return v; }, enumerable: true, configurable: true }); } return x; };
const getArr = (vals, i, get = thrower) => { const a = vals.slice(); Object.defineProperty(a, i, { get, enumerable: true, configurable: true }); return a; };
const lenProxy = (vals, len = thrower) => new Proxy(vals.slice(), { get(t, k, r) { return k === "length" ? len() : Reflect.get(t, k, r); } });
const __NAME = "test_log_getter_abrupt";
import { Logger } from "dyna:log";
const mk = (o) => { const out = []; const L = new Logger({ dest: (l) => out.push(l), timestamp: false, pid: false, hostname: false, ...o }); return { L, out }; };
const PINNED = [
    ["Logger+child+array", () => { const { L, out } = mk({ name: "n", base: { b: 1 } }); L.child({ c: 2 }, { level: "info" }).info({ a: [1, "x", [2]] }, "m"); return out.length + ":" + out[0].includes('[1,"x",[2]]'); }],
];
const INTERRUPTS = {
    interrupt: () => new Logger(with_({ dest: () => {} }, "name", spin)),
    element: () => mk({}).L.info({ a: getArr([1], 0, spin) }, "m"),
};

if (args().includes("--child")) {
    const mode = args()[args().indexOf("--child") + 1];
    if (INTERRUPTS[mode]) {
        for (;;) { try { INTERRUPTS[mode](); } catch (e) {} }
    }
    std.exit(0);
}
let n = 0, fails = 0;
const ok = (c, w) => { n++; if (!c) { fails++; print("  FAIL  " + w); } };
const desc = (r) => r.e !== undefined ? "threw " + (r.e === SENT ? "SENT" : String(r.e)) : "returned " + String(JSON.stringify(r.v)).slice(0, 160);
const sent = (w, f) => { const r = run(f); ok(r.e === SENT, w + ": " + desc(r)); ok(run(() => JSON.parse("[1]")).v?.[0] === 1, w + ": nothing left pending"); };

await __kitS.section("Logger options and child level propagate every getter", () => {
    const BASE = { dest: () => {}, timestamp: false };
    for (const k of ["name", "timestamp", "format", "pid", "hostname", "base", "mkdir", "rollover", "dest"])
        sent("Logger option " + k, () => new Logger(with_(BASE, k)));
    for (const k of ["size", "frequency", "count", "symlink"])
        sent("Logger rollover." + k, () => new Logger({ ...BASE, rollover: with_({}, k) }));
    const { L } = mk({});
    sent("Logger.child level", () => L.child({}, with_({}, "level")));
    const g = gcw({ name: "g", timestamp: false, pid: false, hostname: false, format: "json", base: { z: 1 } });
    const out = [];
    Object.defineProperty(g, "dest", { get() { std.gc(); return (l) => out.push(l); }, enumerable: true });
    const r = run(() => { new Logger(g).info("gc"); return out.length; });
    ok(r.v === 1 && out[0].includes('"name":"g"'), "gc in every option getter keeps the logger: " + desc(r) + " " + out[0]);
});

await __kitS.section("a throwing array element or length is rendered as a marker and never left pending", () => {
    const { L, out } = mk({});
    const r1 = run(() => L.info({ a: getArr([1, 2], 1) }, "m"));
    ok(r1.e === undefined && /\[1,"\[unserializable\]"\]/.test(out[0] || ""), "throwing element: " + desc(r1) + " " + out[0]);
    ok(run(() => JSON.parse("[1]")).v?.[0] === 1, "throwing element: nothing left pending");
    const r2 = run(() => L.info({ a: lenProxy([1]) }, "m"));
    ok(r2.e === undefined && /"a":\[\]/.test(out[1] || ""), "throwing Proxy length: " + desc(r2) + " " + out[1]);
    ok(run(() => JSON.parse("[1]")).v?.[0] === 1, "throwing Proxy length: nothing left pending");
    const r3 = run(() => L.info({ a: getArr([1, 2], 0, () => { std.gc(); return 7; }) }, "m"));
    ok(r3.e === undefined && /\[7,2\]/.test(out[2] || ""), "gc in element getter: " + out[2]);
});

await __kitS.section("pinned controls and interrupts that must end a catch-all loop", () => {
    for (const mode of Object.keys(INTERRUPTS)) {
        const t0 = Date.now();
        const i = Exec(args()[0], ["--std", "--timeout-ms", "300", SELF, "--child", mode], { timeoutMs: 20000 });
        ok(i.code !== 0 && !i.timedOut && /interrupt/i.test(i.stderr + i.stdout), mode + " child ends: rc " + i.code + " after " + (Date.now() - t0) + " ms " + i.stderr.slice(-200));
    }
});
if (fails) throw new Error(fails + " of " + n + " checks failed");
print(__NAME + ": " + n + " checks passed");
