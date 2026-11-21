// covers: dyna:decimal.Decimal.div
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_decimal_decimal_div");
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

eq(D("10").div(D("4")).toString(), "2.5", "an exact quotient is exact");

eq(D("1").div(D("8")).toString(), "0.125", "and so is this one");

eq(D("1").div(D("3")).toString(), "0." + "3".repeat(34),
   "an inexact one gets 34 significant digits, which is decimal128");

eq(D("1").div(D("3"), { precision: 5 }).toString(), "0.33333", "precision is a choice");

eq(D("2").div(D("3"), { precision: 5 }).toString(), "0.66667", "and it rounds");

eq(D("2").div(D("3"), { precision: 5, rounding: "down" }).toString(), "0.66666",
   "as the mode says");

eq(D("-1").div(D("3"), { precision: 3 }).toString(), "-0.333", "sign survives");

eq(D("100").div(D("10")).toString(), "10", "a whole quotient");

throws(() => D("1").div(D("0")), "division by zero is refused");

throws(() => D("1").div(D("3"), { precision: 0 }), "precision must be positive");

throws(() => D("1").div(D("3"), { rounding: "sideways" }), "an unknown rounding mode");

__kitS.section("got", () => {
    let bad = 0, checked = 0;
    for (let a = -20; a <= 20; a++) {
        for (let b = 1; b <= 12; b++) {
            const prec = 6;
            const got = D(String(a)).div(D(String(b)), { precision: prec }).toString();
            const A = BigInt(a), B = BigInt(b);
            let scale = 0n, mag = A < 0n ? -A : A;
            if (mag === 0n) { checked++; if (got !== "0") bad++; continue; }
            let digitsA = mag.toString().length, digitsB = B.toString().length;
            const k = BigInt(prec + 1 + digitsB - digitsA > 0 ? prec + 1 + digitsB - digitsA : 0);
            scale = 10n ** k;
            const num = (A < 0n ? -A : A) * scale;
            let q = num / B, r = num % B;
            const qs = q.toString();
            let want;
            if (qs.length > prec) {
                const drop = qs.length - prec;
                let head = BigInt(qs.slice(0, prec));
                const guard = Number(qs[prec]);
                const rest = qs.slice(prec + 1).replace(/0/g, "") !== "" || r !== 0n;
                if (guard > 5 || (guard === 5 && rest) ||
                    (guard === 5 && !rest && head % 2n === 1n)) head += 1n;
                want = fromCE((A < 0n ? -head : head), -Number(k) + drop);
            } else {
                want = fromCE((A < 0n ? -q : q), -Number(k));
            }
            checked++;
            if (got !== want) {
                bad++;
                if (bad < 4) print("  " + a + "/" + b + " got " + got + " want " + want);
            }
        }
    }
    assert(bad === 0, "division matches the BigInt oracle (" + (checked - bad) +
                      "/" + checked + ")");
});

__kitS.section("a 5000x5000-digit division is refused", () => {
    const N = 5000;
    let a = "", b = "";
    const pat = "3141592653589793238462643383279502884197169399375105820974944592";
    for (let i = 0; i < N; i++) { a += pat[i % pat.length]; b += pat[(i + 7) % pat.length]; }
    const t0 = cpuNow();
    let threw = false, msg = null;
    try { new Decimal(a).div(new Decimal(b)).toString(); }
    catch (e) { threw = true; msg = String(e); }
    const ms = cpuNow() - t0;
    assert(threw, "a 5000x5000-digit division is refused (got: " + msg + ")");
    assert(ms < 1000, "the cell cap is enforced quickly (took " + ms + " ms)");
    assert(msg !== null && /too large/i.test(msg),
           "the refusal is the established 'too large' error (got: " + msg + ")");
});

__kitS.section("cell count exactly at the cap still divides", () => {
    const at = new Decimal("9".repeat(2012)).div(new Decimal("7".repeat(2048)));
    assert(at.toString().length > 0, "cell count exactly at the cap still divides");
    let threw = false;
    try { new Decimal("9".repeat(2013)).div(new Decimal("7".repeat(2048))).toString(); }
    catch (e) { threw = true; }
    assert(threw, "a division one cell over the cap is refused");
});

__kitS.section("a division whose exponent overflows is refused", () => {
    let threw = false, msg = null;
    try { D("1e2000000000").div(D("1e-2000000000")).toString(); }
    catch (e) { threw = true; msg = String(e); }
    assert(threw, "a division whose exponent overflows is refused (got: " + msg + ")");
    assert(/result exceeds/.test(msg),
           "the refusal is the established convention (got: " + msg + ")");
    eq(D("1e2000000000").div(D("1e1999999990")).toString(), "10000000000",
       "a legal large division still works");
});

__kitS.section("a fractional precision is a TypeError (was silently truncated)", () => {
    let t = false;
    try { D("1").div(D("3"), { precision: 34.9 }); }
    catch (e) { t = e instanceof TypeError; }
    assert(t, "a fractional precision is a TypeError (was silently truncated)");
    eq(D("1").div(D("3"), { precision: 5 }).toString(), "0.33333",
       "an integer precision is unchanged");
});
__kitS.end();

if (fails) {
    print("test_decimal: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_decimal failed");
}
print("test_decimal: " + n + " assertions, 0 failures");
