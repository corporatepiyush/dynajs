// flags: --std
// timeout: 120
// covers: contract c:src/core/dyn-snappend.h dyna:oauth2.buildWWWAuthenticate dyna:time.Duration dyna:mathx.NDArray c:src/builtins/builtin-date.c c:src/core/urlrec/urlrec-host.c c:src/modules/libc/libc-std-file.c -- the natives whose x += snprintf accumulation moved to the clamped dyn_snappend append, held to rule 18
// st1l: Date year formatting, URL IPv6 serialization, std.sprintf "*" width/precision, NDArray toString, Duration
// toString and buildWWWAuthenticate append through dyn_snappend (returns the offset clamped to cap - 1, never the
// would-write count). Outputs are pinned at the limit edges (0, max, max + 1) and were diffed identical against the
// pre-change binary. buildWWWAuthenticate read every field with JS_GetPropertyStr and ignored a throwing getter: the
// header was built without that field and the getter's error stayed pending (the next TOML.stringify threw it);
// each field read now propagates it. NDArray shape parsing turned a throwing element getter into its own TypeError
// and treated a revoked Proxy (IsArray -1) as an array; both now propagate the engine's error. Hostile
// valueOf/toString/getters/Proxies, valueOf that runs a GC mid-conversion,
// NUL bytes, and a
// --timeout-ms child whose interrupt must end a loop that
// catches everything.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_api_bounded_append");
import { NDArray } from "dyna:mathx";
import { buildWWWAuthenticate } from "dyna:oauth2";
import { Duration } from "dyna:time";
import { TOML } from "dyna:config";
import { Exec, args } from "dyna:sys";
import * as std from "std";

const SELF = "tests/api/test_api_bounded_append.js";
const SENT = new Error("append sentinel");
const T = { toString() { throw SENT; }, valueOf() { throw SENT; } };
const P = new Proxy({}, { get() { throw SENT; }, ownKeys() { throw SENT; } });
const slot = () => { try { TOML.stringify({ a: Symbol() }); return "no throw"; } catch (e) { return e === SENT ? "sentinel leaked" : e instanceof TypeError ? "clean" : "threw " + e; } };
const run = (f) => { try { return { v: f() }; } catch (e) { return { e }; } };
const PINNED = [
    ["iso max", () => new Date(8.64e15).toISOString(), "+275760-09-13T00:00:00.000Z"],
    ["iso min", () => new Date(-8.64e15).toISOString(), "-271821-04-20T00:00:00.000Z"],
    ["iso 10000", () => new Date(Date.UTC(10000, 0, 1)).toISOString(), "+010000-01-01T00:00:00.000Z"],
    ["iso -1", () => new Date(Date.UTC(-1, 0, 1)).toISOString(), "-000001-01-01T00:00:00.000Z"],
    ["iso 9999", () => new Date(Date.UTC(9999, 11, 31)).toISOString(), "9999-12-31T00:00:00.000Z"],
    ["utc max", () => new Date(8.64e15).toUTCString(), "Sat, 13 Sep 275760 00:00:00 GMT"],
    ["utc min", () => new Date(-8.64e15).toUTCString(), "Tue, 20 Apr -271821 00:00:00 GMT"],
    ["ipv6 full", () => new URL("http://[1:2:3:4:5:6:7:8]/").host, "[1:2:3:4:5:6:7:8]"],
    ["ipv6 widest", () => new URL("http://[ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff]/").host, "[ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff]"],
    ["ipv6 zero", () => new URL("http://[::]/").host, "[::]"],
    ["ipv6 loopback", () => new URL("http://[0:0:0:0:0:0:0:1]/").host, "[::1]"],
    ["ipv6 compress", () => new URL("http://[1:0:0:2:0:0:0:3]/").host, "[1:0:0:2::3]"],
    ["ipv6 v4 tail", () => new URL("http://[::ffff:1.2.3.4]/").host, "[::ffff:102:304]"],
    ["sprintf width 0", () => std.sprintf("%*d|", 0, 7), "7|"],
    ["sprintf width max", () => std.sprintf("%*d|", 1000000, 7).length, 1000001],
    ["sprintf width -max", () => std.sprintf("%*d|", -1000000, 7).length, 1000001],
    ["sprintf precision max", () => std.sprintf("%.*f", 1000000, 1).length, 1000002],
    ["sprintf two stars", () => std.sprintf("%*.*f", 12, 3, Math.PI), "       3.142"],
    ["sprintf NUL", () => std.sprintf("a\0b%d", 1), "a\0b1"],
    ["nd 2x3", () => String(NDArray.zeros([2, 3])), "NDArray(float64, [2, 3])"],
    ["nd empty", () => String(NDArray.zeros([0])), "NDArray(float64, [0])"],
    ["nd wide dims", () => String(NDArray.zeros([1, 0, 2147483647])), "NDArray(float64, [1, 0, 2147483647])"],
    ["nd 8 dims", () => String(NDArray.zeros([1, 1, 1, 1, 1, 1, 1, 1])), "NDArray(float64, [1, 1, 1, 1, 1, 1, 1, 1])"],
    ["duration 0", () => new Duration({}).toString(), "P0D"],
    ["duration max", () => new Duration({ milliseconds: 1e12, days: 1e12, months: 1e12 }).toString(), "P83333333333Y4M1000000000000DT277777H46M40S"],
    ["duration -max", () => new Duration({ milliseconds: -1e12, days: -1e12, years: -83333333333 }).toString(), "-P83333333333Y1000000000000DT277777H46M40S"],
    ["www minimal", () => buildWWWAuthenticate({ error: "invalid_token" }), "Bearer error=\"invalid_token\""],
    ["www all", () => buildWWWAuthenticate({ realm: "r", error: "e", errorDescription: "d", errorUri: "u", scope: "s" }), "Bearer realm=\"r\", error=\"e\", error_description=\"d\", error_uri=\"u\", scope=\"s\""],
    ["www past 512", () => buildWWWAuthenticate({ realm: "x".repeat(600), error: "invalid_request", errorDescription: "x".repeat(600), errorUri: "https://e/" + "x".repeat(600), scope: "x".repeat(600) }).length, 2496],
];
const REFUSED = [
    ["sprintf width max+1", () => std.sprintf("%*d", 1000001, 7), RangeError],
    ["sprintf width -max-1", () => std.sprintf("%*d", -1000001, 7), RangeError],
    ["sprintf precision max+1", () => std.sprintf("%.*f", 1000001, 1), RangeError],
    ["nd 9 dims", () => NDArray.zeros([1, 1, 1, 1, 1, 1, 1, 1, 1]), RangeError],
    ["duration max+1", () => new Duration({ milliseconds: 1e12 + 1 }), RangeError],
    ["date max+1", () => new Date(8.64e15 + 1).toISOString(), RangeError],
    ["www NUL", () => buildWWWAuthenticate({ error: "a\0b" }), TypeError],
    ["www empty error", () => buildWWWAuthenticate({ error: "" }), TypeError],
    ["url NUL in ipv6", () => new URL("http://[1::\0]/"), TypeError],
];

if (args().includes("--child")) {
    const mode = args()[args().indexOf("--child") + 1];
    {
        let caught = 0;
        for (;;) {
            try { buildWWWAuthenticate({ get realm() { throw SENT; }, error: "e" }); } catch (e) { caught++; }
            try { std.sprintf("%*d", { valueOf() { return 3; } }, caught); } catch (e) { caught++; }
        }
    }
} else {
    let n = 0, fails = 0;
    const ok = (c, m) => { n++; if (!c) { fails++; print("FAIL: " + m); } };
    await __kitS.section("outputs pinned at the limit edges", () => {
        for (const [name, f, want] of PINNED) { const r = run(f); ok(!r.e && r.v === want, name + ": " + (r.e ? "threw " + r.e : JSON.stringify(r.v))); }
        for (const [name, f, cls] of REFUSED) { const r = run(f); ok(r.e instanceof cls && slot() === "clean", name + ": " + (r.e ? r.e.name + ": " + r.e.message : "returned " + JSON.stringify(r.v))); }
    });
    await __kitS.section("hostile valueOf, toString, getters and Proxies propagate and leave no exception pending", () => {
        const rows = [
            ["sprintf width valueOf", () => std.sprintf("%*d", T, 1)],
            ["sprintf precision valueOf", () => std.sprintf("%.*f", T, 1)],
            ["sprintf argument valueOf", () => std.sprintf("%d", T)],
            ["sprintf string toString", () => std.sprintf("%s", T)],
            ["Duration getter", () => new Duration({ get years() { throw SENT; } })],
            ["Duration Proxy", () => new Duration(P)],
            ["Duration valueOf", () => new Duration({ days: T })],
            ["NDArray shape Proxy element", () => NDArray.zeros(new Proxy([2, 3], { get(t, k) { if (k === "1") throw SENT; return Reflect.get(t, k); } }))],
            ["NDArray shape Proxy length", () => NDArray.zeros(new Proxy([2, 3], { get(t, k) { if (k === "length") throw SENT; return Reflect.get(t, k); } }))],
            ["NDArray shape revoked Proxy", () => { const p = Proxy.revocable([2], {}); p.revoke(); try { NDArray.zeros(p.proxy); } catch (e) { if (e instanceof TypeError && /revoked/.test(e.message)) throw SENT; throw e; } }],
            ["Date valueOf", () => new Date(T)],
            ["URL toString", () => new URL(T)],
            ["www Proxy", () => buildWWWAuthenticate(P)],
        ];
        for (const k of ["realm", "error", "errorDescription", "error_description", "errorUri", "error_uri", "scope"])
            rows.push(["www getter " + k, () => { const o = { error: "invalid_token" }; Object.defineProperty(o, k, { get() { throw SENT; }, enumerable: true }); return buildWWWAuthenticate(o); }]);
        for (const [name, f] of rows) { const r = run(f); const s = slot(); ok(r.e === SENT && s === "clean", name + ": " + (r.e === SENT ? "sentinel" : r.e ? "threw " + r.e : "returned " + JSON.stringify(r.v)) + ", " + s); }
    });
    await __kitS.section("a GC inside a conversion keeps every result intact", () => {
        const g = (v) => ({ valueOf() { std.gc(); return v; }, toString() { std.gc(); return String(v); } });
        const rows = [
            ["sprintf", () => std.sprintf("%*d|%s", g(5), g(42), g("x")), "   42|x"],
            ["Duration", () => new Duration({ days: g(3), milliseconds: g(1500) }).toString(), "P3DT1.500S"],
            ["NDArray", () => String(NDArray.zeros(new Proxy([2, 3], { get(t, k) { std.gc(); return Reflect.get(t, k); } }))), "NDArray(float64, [2, 3])"],
            ["Date", () => new Date(g(0)).toISOString(), "1970-01-01T00:00:00.000Z"],
            ["www", () => buildWWWAuthenticate({ get realm() { std.gc(); return "r"; }, error: "e" }), "Bearer realm=\"r\", error=\"e\""],
        ];
        for (const [name, f, want] of rows) { const r = run(f); ok(!r.e && r.v === want, "gc in " + name + ": " + (r.e ? "threw " + r.e : JSON.stringify(r.v))); }
    });
    await __kitS.section("an interrupt that must end a catch-all loop", () => {
        const t0 = Date.now();
        const i = Exec(args()[0], ["--std", "--timeout-ms", "300", SELF, "--child", "interrupt"], { timeoutMs: 20000 });
        ok(i.code !== 0 && !i.timedOut && /interrupt/i.test(i.stderr + i.stdout), "interrupt child ends: rc " + i.code + " after " + (Date.now() - t0) + " ms " + i.stderr.slice(-200));
    });
    if (fails) throw new Error(fails + " of " + n + " checks failed");
    print("test_api_bounded_append: " + n + " checks passed");
}
