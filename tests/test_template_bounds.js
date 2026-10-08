// flags: --std
// timeout: 60
import { Template } from "dyna:html";

let n = 0, failed = 0;
function ok(c, msg) {
    n++;
    if (!c) {
        failed++;
        console.log("  FAIL " + msg);
    }
}

{
    const t = new Template("{{#a}}x{{/a}}");
    const a = [];
    a.length = 1000000000;
    const t0 = Date.now();
    let threw = false;
    try { t.render({ a }); } catch (e) { threw = /budget/i.test(String(e)); }
    const dt = Date.now() - t0;
    ok(threw, "a hostile sparse array is refused by the iteration budget");
    ok(dt < 5000, "the refusal is fast (dt=" + dt + "ms)");
}
{
    const t = new Template("{{#a}}{{x}}{{/a}}");
    const a = [{ x: 1 }, { x: 2 }, { x: 3 }];
    ok(t.render({ a }) === "123", "a normal section still renders");
    ok(new Template("{{#a}}x{{/a}}").render({ a: [] }) === "", "an empty array renders empty");
    ok(new Template("{{^a}}none{{/a}}").render({ a: [] }) === "none", "inverted sections work");
}
{
    const t = new Template("{{a.b.c}}");
    ok(t.render({ a: { b: { c: "deep" } } }) === "deep", "dotted paths resolve");
    ok(t.render({ a: { b: { c: "outer" } }, other: 1 }) === "outer", "dotted path at top scope");
    ok(t.render({ a: { b: {} }, c: "wrong" }) === "", "a missing deep segment does not fall back to an outer full path");
    ok(t.render({ a: { b: { c: "top" } }, c: "bottom" }) === "top", "first-segment hit decides the scope");
    let threw = false;
    try { new Template("{{a.b.c.d.e.f}}"); } catch (e) { threw = true; }
    ok(!threw, "deep paths still compile");
}
console.log("test_template_bounds: " + (n - failed) + " passed, " + failed + " failed");
if (failed)
    throw new Error("test_template_bounds: " + failed + " failures");
