// covers: contract dyna:mathx.NDArray dyna:config.TOML.stringify dyna:encoding.JSONPath dyna:html.Template dyna:validate.IsJWT dyna:cli.Command -- one D04 rule (length read through dyn_js_len/dyn_js_len_u32) across every native that reads a caller array length, URLSearchParams included
// D04 (ignored error convention): natives read obj.length and dropped the JS_ToInt64/JS_ToIndex result, so an
// array-shaped Proxy whose length is an object with a throwing valueOf left the exception pending while the call
// went on with a stale count: transpose threw an unrelated RangeError, TOML.stringify returned "a = []" (and read
// an uninitialized loop bound). Natives now read length through dyn_js_len (dyna-nat.h) and propagate the throw.
// st1f: the same drop in URLSearchParams (pairs list and each pair), Template sections, JSONPath ==, IsJWT
// opts.algs and Command option defaults; IsJWT/Template now report the throw instead of a false/empty result;
// IsJWT opts became a strict option bag once d.ts typed it (contract-traps). AbortSignal.any reads the length of
// whatever the global Array.from returns, so a patched Array.from reached the same drop.
// Oracle: every call throws exactly the sentinel; the same call with a plain array still succeeds.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_api_length_conversion");
import { NDArray } from "dyna:mathx";
import { TOML } from "dyna:config";
import { JSONPath } from "dyna:encoding";
import { Template } from "dyna:html";
import { IsJWT } from "dyna:validate";
import { Command } from "dyna:cli";

const b64 = (o) => btoa(JSON.stringify(o)).replace(/=+$/, "").replace(/\+/g, "-").replace(/\//g, "_");
const JWT = b64({ alg: "HS256", typ: "JWT" }) + "." + b64({ sub: "x" }) + ".c2ln";

let n = 0, fails = 0;
function ok(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }

const SENT = new Error("length sentinel");
const hostile = (a) => new Proxy(a, { get(t, k) { return k === "length" ? { valueOf() { throw SENT; } } : t[k]; } });
const trapThrows = (a) => new Proxy(a, { get(t, k) { if (k === "length") throw SENT; return t[k]; } });
function throwsSentinel(f, m) {
    let got;
    try { got = "returned " + JSON.stringify(f()); } catch (e) { got = e === SENT ? "sentinel" : "threw " + e; }
    ok(got === "sentinel", m + ": " + got);
}

const CASES = [
    ["NDArray.zeros(shape)", (w) => NDArray.zeros(w([2, 3]))],
    ["NDArray#reshape(shape)", (w) => NDArray.zeros([2, 3]).reshape(w([3, 2]))],
    ["NDArray#broadcastTo(shape)", (w) => NDArray.zeros([1, 3]).broadcastTo(w([2, 3]))],
    ["NDArray#transpose(axes)", (w) => NDArray.zeros([2, 3]).transpose(w([1, 0]))],
    ["new NDArray(data, {strides})", (w) => new NDArray(new Float64Array(6), [2, 3], { strides: w([3, 1]) })],
    ["TOML.stringify(array value)", (w) => TOML.stringify({ a: w([1, 2]) })],
    ["TOML.stringify(nested array value)", (w) => TOML.stringify({ t: { a: [w([1])] } })],
    ["new URLSearchParams(pairs)", (w) => new URLSearchParams(w([["a", "1"]]))],
    ["new URLSearchParams([pair])", (w) => new URLSearchParams([w(["a", "1"])])],
    ["Template#render(section array)", (w) => new Template("{{#xs}}x{{/xs}}").render({ xs: w([1, 2]) })],
    ["JSONPath == on arrays", (w) => new JSONPath("$[?@.a == @.b]").all([{ a: w([1]), b: [1] }])],
    ["IsJWT(t, {algs})", (w) => IsJWT(JWT, { algs: w(["HS256"]) })],
    ["AbortSignal.any(patched Array.from)", (w) => { const f = Array.from; Array.from = () => w([]); try { return AbortSignal.any([]).aborted; } finally { Array.from = f; } }],
    ["Command option default", (w) => new Command("t").option("--xs <v>", "x", { default: w(["a"]) }).parse([])],
];

__kitS.section("a throwing length propagates the sentinel", () => {
    for (const [name, call] of CASES) throwsSentinel(() => call(hostile), name);
});

__kitS.section("a throwing length getter propagates the sentinel", () => {
    for (const [name, call] of CASES) throwsSentinel(() => call(trapThrows), name + " (get trap)");
});

__kitS.section("plain arrays still work", () => {
    for (const [name, call] of CASES) {
        let r;
        try { call((a) => a); r = "ok"; } catch (e) { r = "threw " + e; }
        ok(r === "ok", name + " with a plain array: " + r);
    }
});

__kitS.section("IsJWT opts is a strict option bag", () => {
    const tt = (f) => { try { f(); return "ok"; } catch (e) { return e instanceof TypeError ? "TypeError" : "threw " + e; } };
    ok(tt(() => IsJWT(JWT, { __bogus: 1 })) === "TypeError", "IsJWT unknown opts key throws TypeError");
    ok(tt(() => IsJWT(JWT, 5)) === "TypeError", "IsJWT non-object opts throws TypeError");
    ok(IsJWT(JWT, undefined) === true && IsJWT(JWT, null) === true, "IsJWT undefined/null opts are absent");
    ok(IsJWT(JWT, { algs: ["RS256"] }) === false, "IsJWT algs still filters");
});

if (fails) throw new Error(fails + " of " + n + " checks failed");
print("test_api_length_conversion: " + n + " checks passed");
