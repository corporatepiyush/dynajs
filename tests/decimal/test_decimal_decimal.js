// covers: dyna:decimal.Decimal
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_decimal_decimal");
import { Decimal, Money } from "dyna:decimal";
import { cpuNow } from "../lib/cpuclock.js";

let n = 0, fails = 0;
function assert(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); } }
function eq(a, b, msg) {
    assert(a === b, msg + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")");
}
function throws(fn, msg) {
    let t = false;
    try { fn(); } catch (e) { t = true; }
    assert(t, msg);
}
const D = (x) => new Decimal(x);

function toCE(s) {
    let sign = 1n, i = 0;
    if (s[0] === "-") { sign = -1n; i = 1; } else if (s[0] === "+") { i = 1; }
    let digits = "", e = 0, seenDot = false;
    for (; i < s.length; i++) {
        const ch = s[i];
        if (ch >= "0" && ch <= "9") { digits += ch; if (seenDot) e--; }
        else if (ch === ".") seenDot = true;
        else if (ch === "e" || ch === "E") { e += parseInt(s.slice(i + 1), 10); break; }
        else throw new Error("bad literal " + s);
    }
    return { c: sign * BigInt(digits === "" ? "0" : digits), e };
}
function fromCE(c, e) {
    if (c === 0n) return "0";
    let neg = c < 0n;
    let d = (neg ? -c : c).toString();
    while (d.length > 1 && d.endsWith("0") && e < 0) { d = d.slice(0, -1); e++; }
    while (e > 0) { d += "0"; e--; }
    let out;
    if (e === 0) out = d;
    else {
        const k = -e;
        out = k >= d.length ? "0." + "0".repeat(k - d.length) + d
                            : d.slice(0, d.length - k) + "." + d.slice(d.length - k);
    }
    return (neg ? "-" : "") + out;
}
function align(a, b) {
    const e = Math.min(a.e, b.e);
    const sa = a.c * 10n ** BigInt(a.e - e), sb = b.c * 10n ** BigInt(b.e - e);
    return [sa, sb, e];
}
const oracleAdd = (x, y) => { const [a, b, e] = align(toCE(x), toCE(y)); return fromCE(a + b, e); };
const oracleSub = (x, y) => { const [a, b, e] = align(toCE(x), toCE(y)); return fromCE(a - b, e); };
const oracleMul = (x, y) => {
    const a = toCE(x), b = toCE(y);
    return fromCE(a.c * b.c, a.e + b.e);
};

__kitS.section("refuses", () => {
for (const bad of ["", " 1", "1 ", "abc", "1.2.3", "1e", "1e+", "0x10", "1,5",
                   "--1", "1e999999999999", "+", ".", "Infinity", "NaN"])
    throws(() => D(bad), "refuses " + JSON.stringify(bad));
});

__kitS.section("refuses the number NaN", () => {
throws(() => D(NaN), "refuses the number NaN");
});

__kitS.section("refuses Infinity", () => {
throws(() => D(Infinity), "refuses Infinity");
});

__kitS.section("refuses an object", () => {
throws(() => D({}), "refuses an object");
});

__kitS.section("a value is required", () => {
throws(() => D(), "a value is required");
});

__kitS.section("which the double sum does NOT (that is the point)", () => {
assert(0.1 + 0.2 !== 0.3, "which the double sum does NOT (that is the point)");
});

__kitS.section("toJSON is the exact text", () => {
eq(JSON.stringify({ v: D("1.25") }), '{"v":"1.25"}', "toJSON is the exact text");
});

__kitS.section("a foreign receiver is refused", () => {
throws(() => Decimal.prototype.add.call({}, D("1")), "a foreign receiver is refused");
});
__kitS.end();

if (fails) {
    print("test_decimal: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_decimal failed");
}
print("test_decimal: " + n + " assertions, 0 failures");
