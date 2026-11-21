// covers: dyna:serialize.ValueHash
// Split from tests/test_optguide_regressions.js: the serialize sections.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_serialize_value_hash_optguide_regressions");
import { ValueHash, structuredClone as sclone } from "dyna:serialize";

let pass = 0, fail = 0;
function ok(c, m) { if (c) { pass++; } else { fail++; print("FAIL: " + m); } }
function eq(a, b, m) { ok(Object.is(a, b) || a === b, m + "  (got " + a + ", want " + b + ")"); }
function eqj(a, b, m) {
    const x = JSON.stringify(a), y = JSON.stringify(b);
    ok(x === y, m + "  (got " + x + ", want " + y + ")");
}

__kitS.section("20000 encodes of string-then-integer-key objects completed", () => {
    for (let i = 0; i < 20000; i++) {
        const o = {};
        o["sessionId_" + i] = "v";
        o["tokenName_" + i] = "w";
        o[0] = 1;
        ValueHash(o);
    }
    ok(true, "20000 encodes of string-then-integer-key objects completed");
    const o = { a: "x", 0: 1 };
    eq(ValueHash(o), ValueHash({ a: "x", 0: 1 }), "the fallback path still hashes deterministically");
});
__kitS.end();
print("test_serialize_optguide_regressions: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error(fail + " failures");
