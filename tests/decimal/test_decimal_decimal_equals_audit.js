// covers: dyna:decimal.Decimal.equals
import { sections as __kitSections } from "../lib/kit.js";
import { Decimal, Money } from "dyna:decimal";
import { OR, DIV, MODR, RND, POW, DOUBLES } from "../lib/decimal_audit_table.js";
const __kitS = __kitSections("test_decimal_decimal_equals_audit");

let n = 0, fails = 0;
function fail(kind, got, want, ctx) {
    fails++;
    if (fails <= 25) print("FAIL[" + kind + "] " + ctx + " got=" + JSON.stringify(got) + " want=" + JSON.stringify(want));
}
function ck(kind, got, want, ctx) {
    n++;
    if (got !== want) fail(kind, got, want, ctx);
}
const D = (x) => new Decimal(x);

__kitS.section("OR rows at precision 1, rounding down", () => {
for (const [op, a, b, want] of OR) {
    try {
        const r = D(a)[op](b, { precision: 1, rounding: "down" }).toString();
        n++; if (r !== want) fail(op, r, want, op + "(" + a + "," + b + ")");
    } catch (e) { n++; fail(op, "THROW " + e.message, want, op + "(" + a + "," + b + ")"); }
}
});

__kitS.section("DIV rows by mode and precision", () => {
for (const [, a, b, mode, prec, want] of DIV) {
    try {
        const r = prec ? D(a).div(b, { precision: prec, rounding: mode }).toString()
                       : D(a).div(b).toString();
        n++; if (r !== want) fail("div", r, want, "div(" + a + "," + b + "," + mode + "," + prec + ")");
    } catch (e) { n++; fail("div", "THROW " + e.message, want, "div(" + a + "," + b + "," + mode + "," + prec + ")"); }
}
});

__kitS.section("MOD rows", () => {
for (const [, a, b, want] of MODR) {
    try {
        const r = D(a).mod(b).toString();
        n++; if (r !== want) fail("mod", r, want, "mod(" + a + "," + b + ")");
    } catch (e) { n++; fail("mod", "THROW " + e.message, want, "mod(" + a + "," + b + ")"); }
}
});

__kitS.section("round and truncate rows", () => {
for (const [op, v, dp, mode, want] of RND) {
    try {
        const r = op === "round"
            ? (mode ? D(v).round(dp, mode) : D(v).round(dp)).toString()
            : (mode ? D(v).toFixed(dp, mode) : D(v).toFixed(dp));
        n++; if (r !== want) fail(op, r, want, op + "(" + v + "," + dp + "," + (mode || "default") + ")");
    } catch (e) { n++; fail(op, "THROW " + e.message, want, op + "(" + v + "," + dp + "," + (mode || "default") + ")"); }
}
});

__kitS.section("POW rows", () => {
for (const [op, b, e, , want] of POW) {
    if (b === undefined) continue;
    try {
        const r = D(b).pow(e).toString();
        n++; if (want === "THROW") fail("pow", r, "THROW", "pow(" + b + "," + e + ")");
        else n++, (r === want ? 0 : fail("pow", r, want, "pow(" + b + "," + e + ")"));
    } catch (ev) {
        n++; if (want !== "THROW") fail("pow", "THROW " + ev.message, want, "pow(" + b + "," + e + ")");
    }
}
});

__kitS.section("roundtrip", () => {
for (const d of DOUBLES) {
    n++;
    const dv = D(d);
    if (Number(dv.toString()) !== d) fail("roundtrip", dv.toString(), d, "D(" + d + ")");
    n++;
    if (dv.toNumber() !== d) fail("toNumber", dv.toNumber(), d, "D(" + d + ")");
}
});

const REFUSE = ["", " ", "abc", "0x10", "1e", "1.2.3", "NaN", "Infinity", "-Infinity",
    " 1", "1 ", "1_000", "1,5", "e5", "+", "-", ".", "1e+ 5", "1e99999999999999999", "1.\u00005"];
__kitS.section("ctor-refuse", () => {
n++; ck("ctor-0.1", D(0.1).toString(), "0.1", "D(0.1)");
n++; ck("ctor-1e21", D(1e21).toString(), "1" + "0".repeat(21), "1e21 prints positional");
n++; ck("ctor-neg0", D(-0).toString(), "0", "-0 ctor prints 0");
n++; ck("minsubnormal", D(5e-324).toString(), "0." + "0".repeat(323) + "5", "5e-324");
n++; ck("tonum-1e400", D("1e400").toNumber(), Infinity, "1e400 -> Infinity");
n++; ck("tonum-inf-neg", D("-1e400").toNumber(), -Infinity, "-1e400 -> -Infinity");
for (const s of REFUSE) {
    n++;
    let threw = false;
    try { D(s); } catch (e) { threw = true; }
    if (!threw) fail("ctor-refuse", "accepted", "throw", JSON.stringify(s));
}
});
__kitS.section("plain object", () => {
n++;
try { D({}); fail("ctor-obj", "accepted", "TypeError", "plain object"); } catch (e) {}
});
__kitS.section("no argument", () => {
n++;
try { D(); fail("ctor-argc", "accepted", "TypeError", "no argument"); } catch (e) {}
});
__kitS.section("exp > 2e9", () => {
n++;
try { D("1e2000000001"); fail("ctor-expoverflow", "accepted", "SyntaxError", "exp > 2e9"); } catch (e) {}
});
__kitS.end();

const M = (a, c, o) => new Money(a, c, o);
if (!__kitS.listing) {

n++; ck("mod-truncated", D(-7).mod(3).toString(), "-1", "truncated, sign of dividend");
n++; ck("mod-frac", D("7.5").mod(2).toString(), "1.5", "exact, no rounding");
n++; ck("mod-opts-ignored", D("-7.5").mod(2, { precision: 1, rounding: "up" }).toString(), "-1.5", "mod exact despite opts");
n++; 

ck("equals-1.50", D("1.5").equals("1.50"), true, "trailing zeros");

n++; ck("cmp-neg0", D("-0").cmp("0"), 0, "-0 == 0");
n++; ck("sign-neg0", D("-0").sign(), 0, "-0 sign");
n++; ck("digits-zero", D(0).digits(), 0, "0 has 0 digits");
n++; ck("digits-1.50", D("1.50").digits(), 2, "1.50 -> 2");
n++; ck("tofixed-neg0", D("-0.0001").toFixed(2), "-0.00", "original sign kept");
n++; ck("round-neg0", D("-0.0001").round(2).toString(), "0", "canonical round drops sign");
n++; ck("div-default-prec", D(1).div(3).toString().slice(2).length, 34, "default context is 34 digits");
n++; ck("div-bare-mode", D(1).div(3, "up").toString(), "0.3333333333333333333333333333333334", "bare string mode accepted");
n++; ck("pow-prec-opt", D(3).pow(-1, { precision: 5 }).toString(), "0.33333", "negative pow honors precision");
n++; ck("huge-unprintable", (() => { try { D("1e2000000000").toString(); return "no-throw"; } catch (e) { return "throw"; } })(), "throw", "1e2000000000 constructible but unprintable");
n++; ck("money-alloc-classic", M(100, "USD").allocate([1, 1, 1]).map((m) => m.amount()).join(","), "34,33,33", "remainder to earliest");
n++; ck("money-toDecimal", M(9223372036854774784, "USD").toDecimal().toString(), "92233720368547747.84", "toDecimal near 2^63");

print("AUDIT TOTAL " + n + " checks, " + fails + " failures");
if (fails) throw new Error("decimal audit: " + fails + " of " + n + " checks failed");
}
