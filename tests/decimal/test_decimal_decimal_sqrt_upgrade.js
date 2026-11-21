// covers: dyna:decimal.Decimal.sqrt
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_decimal_decimal_sqrt_upgrade");
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

__kitS.section("sqrt(", () => {
    let seed = 20250922;
    const rnd = () => { seed = (seed * 1103515245 + 12345) % 2147483648; return seed; };
    const lits = [];
    for (let i = 0; i < 400; i++) {
        const nd = rnd() % 40 + 1;
        let m = String(rnd() % 9 + 1);
        for (let j = 1; j < nd; j++) m += String(rnd() % 10);
        const e = rnd() % 200 - 100;
        lits.push(m + "e" + e);
    }
    const squares = [];
    for (const base of ["2", "3", "7", "997", "123456789", "10", "999999999999",
                        "3162277660168379331998893544432718533719555139325168"]) {
        const b = BigInt(base);
        const sq = b * b;
        squares.push(sq.toString(), (sq + 1n).toString(), (sq - 1n).toString());
        squares.push(sq.toString() + "e-40", sq.toString() + "e40");
    }
    let bad = 0, checked = 0;
    for (const x of lits.concat(squares)) {
        for (const p of [2, 9, 34, 50]) {
            const want = ceText(oracleSqrt(x, p).c, oracleSqrt(x, p).e);
            const got = D(x).sqrt({ precision: p }).toString();
            checked++;
            if (got !== want) {
                bad++;
                if (bad < 5) print("  sqrt(" + x + ", p=" + p + ") got " + got + " want " + want);
            }
        }
    }
    assert(bad === 0, "sqrt matches the BigInt isqrt oracle, all modes of rounding "
                    + "(" + (checked - bad) + "/" + checked + ")");
});

__kitS.section("sqrt of a perfect square is the exact integer root", () => {
    let bad = 0;
    for (const b of [0n, 1n, 2n, 3n, 100n, 12345n, BigInt("3162277660168379331998893544432")]) {
        const sq = (b * b).toString();
        const r = D(sq).sqrt();
        if (!r.mul(r).equals(D(sq)) || !r.equals(D(b.toString()))) bad++;
    }
    assert(bad === 0, "sqrt of a perfect square is the exact integer root");
    eq(D("0").sqrt().toString(), "0", "sqrt(0) = 0");
});

__kitS.section("sqrt/exp/ln/log10 accept every mode name and ignore it", () => {
    const modes = ["up", "down", "ceil", "floor", "halfUp", "halfDown", "halfEven", "halfOdd"];
    let bad = 0;
    for (const m of modes) {
        if (D("2").sqrt({ rounding: m }).toString() !== D("2").sqrt().toString()) bad++;
        if (D("3").ln({ rounding: m }).toString() !== D("3").ln().toString()) bad++;
        if (D("1.5").exp({ rounding: m }).toString() !== D("1.5").exp().toString()) bad++;
        if (D("2").log10({ rounding: m }).toString() !== D("2").log10().toString()) bad++;
    }
    assert(bad === 0, "sqrt/exp/ln/log10 accept every mode name and ignore it (always correctly rounded half-even)");
    eq(D("2").sqrt({ precision: 1 }).toString(), "1", "precision 1 rounds to 1 digit");
    eq(D("2").sqrt({ precision: 5 }).toString(), "1.4142", "precision 5");
});
__kitS.end();

if (fails) {
    print("test_decimal_upgrade: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_decimal_upgrade failed");
}
print("test_decimal_upgrade: " + n + " assertions, 0 failures");
