// covers: dyna:decimal.Decimal.floor
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_decimal_decimal_floor_upgrade");
import { Decimal } from "dyna:decimal";
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
function digitsOf(b) { return (b < 0n ? -b : b).toString().length; }
function pow10(k) { return 10n ** BigInt(k); }

function bigIsqrt(n) {
    if (n < 0n) throw new Error("negative");
    if (n < 2n) return n;
    let x0 = 1n << BigInt(Math.ceil((n.toString(2).length + 1) / 2));
    for (;;) {
        const x1 = (x0 + n / x0) >> 1n;
        if (x1 >= x0) return x0;
        x0 = x1;
    }
}

function oracleSqrt(text, p) {
    const { c, e } = toCE(text);
    if (c === 0n) return { c: 0n, e: 0 };
    let s = Math.max(0, Math.ceil((2 * (p + 1) - (digitsOf(c) + e)) / 2));
    while (e + 2 * s < 0) s++;
    const N = c * pow10(e + 2 * s);
    const R = bigIsqrt(N);
    const rem = N - R * R;
    const k = digitsOf(R);
    const cut = k - p;
    let T = R / pow10(cut > 0 ? cut : 0);
    if (cut > 0) {
        const guard = (R / pow10(cut - 1)) % 10n;
        const rest = (R % pow10(cut - 1)) !== 0n || rem !== 0n;
        if (guard > 5n || (guard === 5n && (rest || (T % 2n === 1n)))) T += 1n;
    }
    return { c: T, e: (cut > 0 ? cut : 0) - s };
}

function ceText(c, e) {
    if (c === 0n) return "0";
    let neg = c < 0n;
    let d = (neg ? -c : c).toString();
    while (d.length > 1 && d.endsWith("0") && e < 0) { d = d.slice(0, -1); e++; }
    let out;
    if (e === 0) out = d;
    else if (e > 0) { out = d + "0".repeat(e); }
    else {
        const k = -e;
        out = k >= d.length ? "0." + "0".repeat(k - d.length) + d
                            : d.slice(0, d.length - k) + "." + d.slice(d.length - k);
    }
    return (neg ? "-" : "") + out;
}

__kitS.section(") got", () => {
    function oracleIntegral(text, mode) {
        const { c, e } = toCE(text);
        if (c === 0n) return "0";
        let out;
        if (e >= 0) out = { c: c * pow10(e), e: 0 };
        else {
            const scale = pow10(-e);
            const q = (c < 0n ? -c : c) / scale;
            const rem = (c < 0n ? -c : c) % scale;
            let v;
            if (mode === "trunc") v = q;
            else if (mode === "floor") v = c < 0n && rem !== 0n ? q + 1n : q;
            else v = c > 0n && rem !== 0n ? q + 1n : q;
            out = { c: (c < 0n ? -v : v), e: 0 };
        }
        return ceText(out.c, out.e);
    }
    let bad = 0, checked = 0;
    const vals = ["0", "-0", "0.5", "-0.5", "2.5", "-2.5", "7", "-7", "1e10", "-1e10",
                  "1e-10", "-1e-10", "3.999999", "-3.999999", "0.0000001", "-0.0000001",
                  "123456789.987654321", "-123456789.987654321", "9.99", "-9.99",
                  "1.0000000000000000000000000000001", "-0.0000000000000000000000000000001"];
    let seed = 777;
    const rnd = () => { seed = (seed * 1103515245 + 12345) % 2147483648; return seed; };
    for (let i = 0; i < 150; i++) {
        const nd = rnd() % 25 + 1;
        let m = String(rnd() % 9 + 1);
        for (let j = 1; j < nd; j++) m += String(rnd() % 10);
        const e = rnd() % 40 - 20;
        const neg = rnd() % 2 ? "-" : "";
        vals.push(neg + m + "e" + e);
    }
    for (const v of vals) {
        for (const mode of ["floor", "ceil", "trunc"]) {
            const got = D(v)[mode]().toString();
            const want = oracleIntegral(v, mode);
            checked++;
            if (got !== want) {
                bad++;
                if (bad < 5) print("  " + mode + "(" + v + ") got " + got + " want " + want);
            }
        }
    }
    assert(bad === 0, "floor/ceil/trunc match the BigInt oracle (" + (checked - bad) + "/" + checked + ")");
    eq(D("-2.5").floor().toString(), "-3", "floor is toward -Infinity");
    eq(D("-2.5").ceil().toString(), "-2", "ceil is toward +Infinity");
    eq(D("-2.5").trunc().toString(), "-2", "trunc is toward zero");
    eq(D("-0.5").ceil().toString(), "0", "a zero result is 0, never -0 (module convention, like neg)");
    eq(D("-0.5").trunc().toString(), "0", "trunc(-0.5) is 0");
    eq(D("-0.5").floor().toString(), "-1", "floor(-0.5) is -1");
    assert(D("1e2000000000").floor().cmp(D("1e2000000000")) === 0
           && D("1e2000000000").floor().digits() === 1,
           "an integer passes through unchanged (huge exponent; its positional text is over the print bound, so compare by value)");
});
__kitS.end();

if (fails) {
    print("test_decimal_upgrade: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_decimal_upgrade failed");
}
print("test_decimal_upgrade: " + n + " assertions, 0 failures");
