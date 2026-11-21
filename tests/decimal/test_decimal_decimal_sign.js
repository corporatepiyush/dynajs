// covers: dyna:decimal.Decimal.sign
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_decimal_decimal_sign");
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

__kitS.section("sign", () => {
eq(D("-5").sign(), -1, "sign");
});

__kitS.section("of zero", () => {
eq(D("0").sign(), 0, "of zero");
});
__kitS.end();

if (fails) {
    print("test_decimal: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_decimal failed");
}
print("test_decimal: " + n + " assertions, 0 failures");
