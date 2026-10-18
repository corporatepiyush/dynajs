// Split from tests/test_optguide_regressions.js: the serialize sections.
import { ValueHash, structuredClone as sclone } from "dyna:serialize";

let pass = 0, fail = 0;
function ok(c, m) { if (c) { pass++; } else { fail++; print("FAIL: " + m); } }
function eq(a, b, m) { ok(Object.is(a, b) || a === b, m + "  (got " + a + ", want " + b + ")"); }
function eqj(a, b, m) {
    const x = JSON.stringify(a), y = JSON.stringify(b);
    ok(x === y, m + "  (got " + x + ", want " + y + ")");
}
{
    const a = { name: "a" }; a.self = a;
    const c = sclone(a);
    ok(c.self === c, "clone: a self-cycle closes onto the clone");
    ok(c !== a, "clone: and it is a copy, not the original");

    const x = { n: 1 }, y = { n: 2 }; x.y = y; y.x = x;
    const cx = sclone(x);
    ok(cx.y.x === cx, "clone: a two-node cycle closes");

    const shared = { v: 7 };
    const cs = sclone({ p: shared, q: shared });
    ok(cs.p === cs.q, "clone: a shared node stays shared, not duplicated");

    for (const n of [16, 63, 64, 65, 2000]) {
        const root = {}; const child = { c: 1 };
        for (let i = 0; i < n; i++) root["a" + i] = { v: i };
        root.s1 = child; root.s2 = child;
        const r = sclone(root);
        eq(Object.keys(r).length, n + 2, "clone n=" + n + " keeps every key");
        eq(r["a" + (n - 1)].v, n - 1, "clone n=" + n + " copies values");
        ok(r.s1 === r.s2, "clone n=" + n + " preserves sharing across the memo threshold");
    }
    let deep = {}; const top = deep;
    for (let i = 0; i < 400; i++) { deep.n = {}; deep = deep.n; }
    let threw = false;
    try { sclone(top); } catch (e) { threw = true; }
    ok(threw, "clone refuses nesting past its depth cap");
}
{
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
}
print("test_serialize_optguide_regressions: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error(fail + " failures");
