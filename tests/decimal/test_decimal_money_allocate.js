// covers: dyna:decimal.Money.allocate
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_decimal_money_allocate");
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

__kitS.section("allocate", () => {
    let bad = 0, checked = 0;
    const weights = [[1, 1, 1], [1, 1], [70, 30], [1, 2, 3], [1], [0, 1, 1],
                     [5, 5, 5, 5, 5, 5, 5]];
    for (let amount = -13; amount <= 101; amount++) {
        for (const w of weights) {
            const parts = new Money(amount, "USD").allocate(w);
            let sum = 0;
            for (const p of parts) sum += p.amount();
            checked++;
            if (sum !== amount || parts.length !== w.length) {
                bad++;
                if (bad < 4)
                    print("  allocate(" + amount + ", " + JSON.stringify(w) +
                          ") sums to " + sum);
            }
        }
    }
    assert(bad === 0, "every allocation sums EXACTLY to the original (" +
                      (checked - bad) + "/" + checked + ")");
    assert(checked === 115 * weights.length, "the sweep ran every case");
});

eq(JSON.stringify(new Money(100, "USD").allocate([1, 1, 1]).map((m) => m.amount())),
   "[34,33,33]", "the remainder goes to the earliest shares");

eq(JSON.stringify(new Money(5, "USD").allocate([70, 30]).map((m) => m.amount())),
   "[4,1]", "a weighted split");

throws(() => new Money(100, "USD").allocate([]), "an empty share list");

throws(() => new Money(100, "USD").allocate([0, 0]), "weights that sum to zero");

throws(() => new Money(100, "USD").allocate([1, -1]), "a negative weight");

throws(() => new Money(100, "USD").allocate([1.5]), "a fractional weight");

throws(() => new Money(100, "USD").allocate("no"), "a non-array");

__kitS.section("weights summing past int64 to zero are refused", () => {
    const w62 = Math.pow(2, 62);
    throws(() => new Money(100, "USD").allocate([w62, w62, w62, w62]),
           "weights summing past int64 to zero are refused");
    throws(() => new Money(1, "USD").allocate([w62, w62, w62]),
           "weights summing to a wrapped nonzero total are refused");
    eq(JSON.stringify(new Money(100, "USD").allocate([40, 30, 20, 10])
                        .map((m) => m.amount())),
       "[40,30,20,10]", "a normal allocate is unchanged");
});

__kitS.section("allocate of an INT64_MIN amount is refused, not UB", () => {
    throws(() => new Money(-9223372036854775808, "USD").allocate([1, 1]),
           "allocate of an INT64_MIN amount is refused, not UB");
    throws(() => new Money(100, "USD").allocate([Math.pow(2, 63)]),
           "a 2^63 weight is refused, not UB-cast");
});
__kitS.end();

if (fails) {
    print("test_decimal: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_decimal failed");
}
print("test_decimal: " + n + " assertions, 0 failures");
