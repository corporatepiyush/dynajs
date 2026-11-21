// covers: dyna:decimal.Money.toDecimal
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_decimal_money_to_decimal_from");
import { Decimal, Money } from "dyna:decimal";

let n = 0, bad = 0;
function ok(c, what) { n++; if (!c) { bad++; print("FAIL: " + what); } }
function eq(a, b, what) { ok(a === b, what + " (got " + a + ", want " + b + ")"); }
function throws(fn, ErrType, re, what) {
    let t = null;
    try { fn(); } catch (e) { t = e; }
    ok(t !== null, "expected throw: " + what);
    if (t === null) return;
    ok(t instanceof ErrType,
       what + " (wrong type: " + (t && t.constructor && t.constructor.name) + ")");
    ok(!re || re.test(String(t.message)),
       what + " (message [" + String(t && t.message) + "] !~ " + re + ")");
}

__kitS.section("ref", () => {
    const I64_MAX = (1n << 63n) - 1n;
    function moneyRef(str, minor) {
        if (str.length === 0) return ["E", "TypeError"];
        const neg = str[0] === "-";
        let i = neg ? 1 : 0;
        let acc = 0n, frac = 0n, nfrac = 0;
        let seenDot = false, seenDigit = false, comma = false;
        for (; i < str.length; i++) {
            const c = str[i];
            if (c === ".") {
                if (seenDot || nfrac === -1) return ["E", "TypeError"];
                if (!seenDigit) return ["E", "TypeError"];
                seenDot = true;
                nfrac = -1;
            } else if (c === ",") {
                if (seenDot) return ["E", "TypeError"];
                comma = true;
            } else if (c < "0" || c > "9") {
                return ["E", "TypeError"];
            } else if (seenDot) {
                if (nfrac < 0) nfrac = 0;
                nfrac++;
                if (nfrac <= minor) {
                    frac = frac * 10n + BigInt(c.charCodeAt(0) - 48);
                } else if (c !== "0") {
                    return ["E", "RangeError"];
                }
            } else {
                seenDigit = true;
                acc = acc * 10n + BigInt(c.charCodeAt(0) - 48);
                if (acc > I64_MAX + 1n) return ["E", "RangeError"];
            }
        }
        if (nfrac === -1 || !seenDigit) return ["E", "TypeError"];
        if (comma) {
            let g = 0, glen = 0;
            for (let k = neg ? 1 : 0; k < str.length && str[k] !== "."; k++) {
                if (str[k] === ",") {
                    if (glen === 0 || glen > 3 || (g && glen !== 3))
                        return ["E", "TypeError"];
                    g++; glen = 0;
                } else glen++;
            }
            if (glen === 0 || glen > 3 || (g && glen !== 3))
                return ["E", "TypeError"];
        }
        for (let k = 0; k < minor; k++) acc *= 10n;
        for (let k = nfrac; k < minor; k++) frac *= 10n;
        const val = acc + frac;
        if (neg) {
            if (val > I64_MAX + 1n) return ["E", "RangeError"];
            return ["V", -val];
        }
        if (val > I64_MAX) return ["E", "RangeError"];
        return ["V", val];
    }
    function amountExact(m, minor) {
        const s = m.toDecimal().toString();
        const [ip, fp = ""] = s.split(".");
        return BigInt(ip + fp.padEnd(minor, "0"));
    }
    let rows = 0, mism = 0;
    function check(str, minor) {
        rows++;
        const exp = moneyRef(str, minor);
        let act;
        try {
            const m = Money.fromString(str, "USD",
                                       minor === 2 ? undefined : { minorDigits: minor });
            act = ["V", amountExact(m, minor)];
        } catch (e) {
            act = ["E", e.constructor.name];
        }
        if (act[0] !== exp[0] || (act[0] === "V" && act[1] !== exp[1]) ||
            (act[0] === "E" && act[1] !== exp[1])) {
            mism++;
            if (mism <= 5)
                print("FAIL: ref " + JSON.stringify(str) + " minor=" + minor +
                      ": got " + act.join(" ") + ", want " + exp.join(" "));
        }
    }
    for (let intLen = 17; intLen <= 21; intLen++) {
        for (let fracLen = 0; fracLen <= 8; fracLen++) {
            for (const lead of ["1", "9"]) {
                for (const minor of [0, 1, 2, 3, 6]) {
                    const i = lead + "0".repeat(intLen - 2) + "7";
                    const f = fracLen ? "." + "0".repeat(fracLen - 1) + "3" : "";
                    check(i + f, minor);
                    check("-" + i + f, minor);
                }
            }
        }
    }
    for (const s of ["92233720368547758.07", "92233720368547758.08", "92233720368547758.06",
                     "-92233720368547758.08", "-92233720368547758.09",
                     "9223372036.854775807", "9223372036.854775808",
                     "0.000001", "0.0000001", "184467440737095516.15",
                     "9223372036854775807", "9223372036854775808",
                     "-9223372036854775808", "-9223372036854775809",
                     "1.500", "5.0", "242448628.0", "0.0000000", "1.000000"])
        for (const minor of [0, 1, 2, 3, 6]) check(s, minor);
    let xs = 0x9e3779b9;
    function rnd() {
        xs ^= xs << 13; xs >>>= 0;
        xs ^= xs >>> 17;
        xs ^= xs << 5;  xs >>>= 0;
        return xs;
    }
    for (let t = 0; t < 600; t++) {
        const intLen = 1 + (rnd() % 20);
        const fracLen = rnd() % 9;
        const minor = rnd() % 7;
        let i = String(1 + (rnd() % 9));
        for (let k = 1; k < intLen; k++) i += String(rnd() % 10);
        let f = "";
        if (fracLen) {
            const digits = Array.from({ length: fracLen }, () => rnd() % 10);
            if (t % 3 === 2) digits[digits.length - 1] = 0;
            f = "." + digits.join("");
        }
        const s = (t % 4 === 0 ? "-" : "") + i + f;
        if (t % 5 === 1) check(s.replace(/^(-?)(\d{1,3})/, "$1$2,"), minor);
        check(s, minor);
    }
    ok(rows >= 1500, "the differential ran " + rows + " rows");
    eq(mism, 0, "money factories == independent BigInt model of the value rule");
});
__kitS.end();

print("test_money_from: " + n + " assertions, " + bad + " failures");
if (bad) throw new Error(bad + " failures");
