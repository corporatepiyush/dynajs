// timeout: 180
// Split from tests/test_data_w3_fixes.js: the serialize sections.
import { TOML } from "dyna:config";
import { hostScale } from "../lib/hostscale.js";
import { Proto } from "dyna:serialize";

let n = 0, fails = 0;
function check(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function eq(a, b, m) { check(a === b, m + " -- got " + JSON.stringify(a) + ", want " + JSON.stringify(b)); }
async function throws(fn, m) { n++; try { await fn(); fails++; print("FAIL: " + m + " (did not throw)"); } catch (e) { } }
function peq(text, expected, m) {
    n++;
    let got;
    try { got = TOML.parse(text).s; }
    catch (e) { fails++; print("FAIL: " + m + " (threw " + e + ")"); return; }
    check(got === expected, m + " -- got " + JSON.stringify(got) + ", want " + JSON.stringify(expected));
}
{
    const U64 = { fields: [{ name: "a", number: 1, type: "uint64" }] };
    const F64 = { fields: [{ name: "a", number: 1, type: "fixed64" }] };
    const big = 18446744073709549568;
    eq(Proto.decode(Proto.encode({ a: big }, U64), U64).a, big,
       "COMPAT-236: a uint64 above 2^63 round-trips");
    eq(Proto.decode(Proto.encode({ a: big }, F64), F64).a, big,
       "COMPAT-236: a fixed64 above 2^63 round-trips");
    eq(Proto.decode(Proto.encode({ a: 9223372036854775808 }, U64), U64).a, 9223372036854775808,
       "COMPAT-236: exactly 2^63 round-trips");
    await throws(() => Proto.encode({ a: 18446744073709551616 }, U64),
           "COMPAT-236: 2^64 is still refused");
}
{
    const fields = [];
    for (let i = 0; i < 40000; i++)
        fields.push({ name: "f" + i, number: i + 1, type: "int32" });
    const t0 = Date.now();
    const out = Proto.encode({}, { fields });
    const ms = Date.now() - t0;
    const cap048 = Math.round(150 * hostScale());
    check(out instanceof Uint8Array && ms < cap048,
          "OPT-048: a 40000-field unique schema compiles in " + ms + " ms (< " + cap048 + ")");
    await throws(() => Proto.encode({}, { fields: [
        { name: "a", number: 1, type: "int32" },
        { name: "b", number: 1, type: "int32" }] }),
        "OPT-048: a duplicate field number is still refused");
    await throws(() => Proto.encode({}, { fields: [
        { name: "a", number: 1, type: "int32" },
        { name: "a", number: 2, type: "int32" }] }),
        "OPT-048: a duplicate field name is still refused");
}
if (fails === 0) print("test_serialize_data_w3_fixes: all " + n + " checks passed");
else print("test_serialize_data_w3_fixes: " + fails + " FAILED of " + n);
