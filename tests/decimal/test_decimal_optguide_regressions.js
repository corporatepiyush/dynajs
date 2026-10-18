// Split from tests/test_optguide_regressions.js: the decimal sections.
import { Decimal } from "dyna:decimal";
import { Table } from "dyna:structures";

let pass = 0, fail = 0;
function ok(c, m) { if (c) { pass++; } else { fail++; print("FAIL: " + m); } }
function eq(a, b, m) { ok(Object.is(a, b) || a === b, m + "  (got " + a + ", want " + b + ")"); }
function eqj(a, b, m) {
    const x = JSON.stringify(a), y = JSON.stringify(b);
    ok(x === y, m + "  (got " + x + ", want " + y + ")");
}
{
    const big = new Decimal("1".repeat(9000));
    let threw = null;
    try { big.mul(big); } catch (e) { threw = e; }
    ok(threw && /digit-pairs|too large/.test(threw.message),
       "decimal: an over-large multiply is refused");
    eq(String(new Decimal("123456789").mul(new Decimal("987654321"))),
       "121932631112635269", "decimal: an ordinary multiply is unaffected");
    eq(String(new Decimal("1".repeat(4000)).mul(new Decimal("1"))).length, 4000,
       "decimal: a lopsided multiply still runs");

    const t = new Table();
    for (const n of [1, 50, 175, 183, 184, 185, 200, 1000]) {
        const r = "r".repeat(n);
        t.put(r, "c", n);
        eq(t.get(r, "c"), n, "table key len " + n + " round-trips");
        ok(t.has(r, "c"), "table key len " + n + " has()");
    }
    ok(t.delete("r".repeat(184), "c"), "table delete at the inline boundary");
    ok(!t.has("r".repeat(184), "c"), "...and the entry is gone");
    eq(t.get("r".repeat(183), "c"), 183, "...while its neighbour survives");
}
print("test_decimal_optguide_regressions: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error(fail + " failures");
