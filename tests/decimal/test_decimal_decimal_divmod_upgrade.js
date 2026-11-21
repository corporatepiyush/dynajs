// covers: dyna:decimal.Decimal.divmod
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_decimal_decimal_divmod_upgrade");
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

__kitS.section("divmod(", () => {
    const vals = ["-7", "-3", "-0.5", "0", "0.5", "2", "7", "7.5", "100",
                  "-100", "1e10", "-1e-10", "123.456", "-0.000001", "999999999999"];
    let bad = 0, checked = 0;
    for (const a of vals) for (const b of vals) {
        if (D(b).isZero()) continue;
        const [q, r] = D(a).divmod(b);
        const A = toCE(a), Q = toCE(q.toString()), R = toCE(r.toString());
        const bs = toCE(b);
        checked++;
        {
            const m = Math.min(A.e, Q.e + bs.e, R.e);
            const ai = A.c * pow10(A.e - m);
            const qb = Q.c * bs.c * pow10(Q.e + bs.e - m);
            const ri = R.c * pow10(R.e - m);
            if (ai !== qb + ri) {
                bad++;
                if (bad < 5) print("  divmod(" + a + ", " + b + ") breaks a = qb+r");
                continue;
            }
        }
        const rs = r.isZero() ? 0 : r.sign();
        if (rs !== (r.isZero() ? 0 : (A.c < 0n ? -1 : 1))) {
            bad++;
            if (bad < 5) print("  divmod(" + a + ", " + b + ") remainder sign " + rs);
        }
        {
            const m2 = Math.min(R.e, bs.e);
            const ri = R.c < 0n ? -R.c : R.c;
            const bi = bs.c < 0n ? -bs.c : bs.c;
            if (ri * pow10(R.e - m2) >= bi * pow10(bs.e - m2) && !r.isZero()) {
                bad++;
                if (bad < 5) print("  divmod(" + a + ", " + b + ") |r| >= |b|");
            }
        }
        if (!q.equals(D(a).div(b).trunc())) {
            bad++;
            if (bad < 5) print("  divmod(" + a + ", " + b + ") q != trunc(a/b)");
        }
    }
    assert(bad === 0, "divmod matches the BigInt oracle on the sign matrix ("
                    + (checked - bad) + "/" + checked + " checks)");
    {
        const [q, r] = D("-7").divmod("2");
        eq(q.toString(), "-3", "divmod(-7,2) quotient");
        eq(r.toString(), "-1", "divmod(-7,2) remainder takes the dividend's sign");
    }
    {
        const [q, r] = D("7.5").divmod("2");
        eq(q.toString(), "3", "divmod(7.5,2) quotient");
        eq(r.toString(), "1.5", "divmod(7.5,2) fractional remainder");
    }
    throws(() => D("1").divmod("0"), "divmod by zero is refused");
    eq(D("0").divmod("5")[0].toString() + "," + D("0").divmod("5")[1].toString(),
       "0,0", "divmod(0, x) is [0, 0]");
    {
        const [q1, r1] = D("-7.5").divmod("2", { precision: 3, rounding: "up" });
        eq(q1.toString(), "-3", "divmod ignores opts: quotient exact");
        eq(r1.toString(), "-1.5", "divmod ignores opts: remainder exact");
    }
});
__kitS.end();

if (fails) {
    print("test_decimal_upgrade: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_decimal_upgrade failed");
}
print("test_decimal_upgrade: " + n + " assertions, 0 failures");
