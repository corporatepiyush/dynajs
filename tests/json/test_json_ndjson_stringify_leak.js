// covers: dyna:json.Ndjson.stringify
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_json_ndjson_stringify_leak");
import { Ndjson } from "dyna:json";

let n = 0, fails = 0;
function assert(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); } }
function eq(a, b, msg) {
    assert(a === b, msg + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")");
}
function throwsMatch(fn, re, msg) {
    let got = "";
    try { fn(); } catch (e) { got = String(e.message); }
    assert(re.test(got), msg + (got ? " (got: " + got + ")" : " (did not throw)"));
}

__kitS.section("round", () => {
    for (let round = 0; round < 20; round++) {
        const items = [];
        for (let i = 0; i < 1000; i++)
            items.push({ r: round, i, s: "s".repeat((i * round) % 512) });
        const text = Ndjson.stringify(items);
        const back = Ndjson.parse(text);
        eq(back.length, 1000, "round " + round + ": stringify/parse round trip");
    }
    const wide = Ndjson.stringify([{ w: "W".repeat(1 << 20) }]);
    assert(wide.length > (1 << 20), "a 1MiB payload stringifies");
    eq(Ndjson.parse(wide)[0].w.length, 1 << 20, "and parses back at full width");
});

__kitS.section("mixed-shape round trip keeps every line", () => {
    const items = [null, 1, "x", true, {}, [], { a: [1, { b: "c" }] },
                   "\u0000\u001f\n\"", 1e308, -0];
    const back = Ndjson.parse(Ndjson.stringify(items));
    eq(back.length, items.length, "mixed-shape round trip keeps every line");
    eq(JSON.stringify(back), JSON.stringify(items), "and every value");
});
__kitS.end();

print("test_json_ndjson_leak: " + (n - fails) + "/" + n + " assertions" +
      (fails ? " -- " + fails + " FAILURES" : " all passed (LSan must be flat at exit)") );
if (fails) throw new Error(fails + " failures");
