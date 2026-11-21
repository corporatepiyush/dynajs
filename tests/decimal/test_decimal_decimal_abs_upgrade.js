// covers: dyna:decimal.Decimal.abs
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_decimal_decimal_abs_upgrade");
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

__kitS.section("exp(a)exp(b) == exp(a+b) to < 1e-32 relative", () => {
    let worst = 0;
    for (const [a, b] of [["1", "2"], ["-3", "0.5"], ["10", "-10"], ["0.001", "-69"]]) {
        const lhs = D(a).exp().mul(D(b).exp());
        const rhs = D(a).add(b).exp();
        const rel = lhs.sub(rhs).abs().div(rhs.abs()).toNumber();
        if (rel > worst) worst = rel;
    }
    assert(worst < 1e-32, "exp(a)exp(b) == exp(a+b) to < 1e-32 relative (worst " + worst + ")");
    worst = 0;
    for (const [x, y] of [["3", "7"], ["0.5", "100"], ["1e-50", "1e60"], ["9.5", "0.125"]]) {
        const lhs = D(x).mul(y).ln();
        const rhs = D(x).ln().add(D(y).ln());
        const rel = lhs.sub(rhs).abs().div(lhs.abs()).toNumber();
        if (rel > worst) worst = rel;
    }
    assert(worst < 1e-32, "ln(xy) == ln(x)+ln(y) to < 1e-32 relative (worst " + worst + ")");
    worst = 0;
    for (const x of ["7", "1e50", "0.0001", "3.14159"]) {
        const back = D(x).ln().exp();
        const rel = back.sub(D(x)).abs().div(D(x)).toNumber();
        if (rel > worst) worst = rel;
    }
    assert(worst < 1e-31, "e^ln(x) == x to < 1e-31 relative through TWO chained nonlinear roundings (worst " + worst + ")");
    let exact = 0;
    for (let k = -300; k <= 300; k += 7) {
        if (D("1e" + k).log10().toString() !== String(k)) { exact++; }
    }
    assert(exact === 0, "log10 of every 1e7k is the exact integer (86 powers)");
    worst = 0;
    for (const [m, k] of [["7.25", "13"], ["3.14159", "-40"], ["9.99", "99"]]) {
        const lhs = D(m).mul("1e" + k).log10();
        const rhs = D(m).log10().add(D(k));
        const rel = lhs.sub(rhs).abs().div(lhs.abs()).toNumber();
        if (rel > worst) worst = rel;
    }
    assert(worst < 1e-32, "log10(m*10^k) == log10(m)+k to < 1e-32 relative (worst " + worst + ")");
    worst = 0;
    for (const x of ["2", "1e-50", "9.85", "1e49"]) {
        const sq = D(x).sqrt();
        const rel = sq.mul(sq).sub(D(x)).abs().div(D(x)).toNumber();
        if (rel > worst) worst = rel;
    }
    assert(worst < 1e-32, "sqrt(x)^2 == x to < 1e-32 relative (worst " + worst + ")");
});
__kitS.end();

if (fails) {
    print("test_decimal_upgrade: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_decimal_upgrade failed");
}
print("test_decimal_upgrade: " + n + " assertions, 0 failures");
