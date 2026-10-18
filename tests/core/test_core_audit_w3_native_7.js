// flags: --std
// timeout: 300
// Split from tests/test_audit_w3_native_7.js: the core sections.
export {};

let failures = 0, checks = 0;
function ok(cond, msg) { checks++; if (!cond) { failures++; print("  FAIL:", msg); } }
function thrown(fn) { try { fn(); return "none"; } catch (e) { return e.constructor.name + ": " + e.message; } }
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
// ---- P1-25 --------------------------------------------------------------
// ORACLE: arithmetic. A header array whose length is 4e9 would make the
// table reserve four arrays of 4e9 entries (32 GiB each) before looking at a
// single element; a bounded column count must refuse first.
// CONTROL: an ordinary table renders.
{
    const term = await import("dyna:cli");
    ok(term.Table([["a", "b"], ["1", "2"]]).includes("a"), "control: a 2x2 table renders");
    const head = []; head.length = 4e9;
    let e = "none";
    try { term.Table([], { head }); } catch (x) { e = x.constructor.name; }
    if (e === "none") { try { term.Table([head]); } catch (x) { e = x.constructor.name; } }
    ok(e === "RangeError", "[red] P1-25: a 4e9-column header is a RangeError before any allocation (" + e + ")");
}
print("test_core_audit_w3_native_7: " + checks + " checks, " + failures + " failures");
if (failures) throw new Error("test_core_audit_w3_native_7: " + failures + " failures");
