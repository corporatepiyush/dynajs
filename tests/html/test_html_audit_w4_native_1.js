// flags: --std
// Split from tests/test_audit_w4_native_1.js: the html sections.
import "../lib/httpc.js";
import * as scrape from "dyna:scrape";
import { Selector, HTMLText, HTMLParse } from "dyna:html";
import { memoryUsage } from "dyna:sys";
import { suite } from "../lib/kit.js";

const t = suite("test_html_audit_w4_native_1");
const per = (f, n = 20000) => { let b = Infinity; for (let r = 0; r < 5; r++) { const t0 = performance.now(); for (let i = 0; i < n; i++) f(); b = Math.min(b, (performance.now() - t0) / n * 1e6); } return b; };
// ORACLE: an object that is built and dropped gives its memory back, so the
// allocator's own byte counter is flat from one round of constructions to the
// next. Each Extractor used to keep one reference to every field NAME: 81
// bytes per field, for ever, visible only when the names are distinct.
// The first round is warm-up; the comparison is the second round against the
// third. CONTROL: the extractor still extracts.
t.test("Extractor releases its field names", ({ ok, eq }) => {
    const sel = new Selector("h1");
    const one = new scrape.Extractor({ title: { sel } }, { text: HTMLText });
    eq(one.run(HTMLParse("<h1>Hi</h1>")).value.title, "Hi", "control: a field is extracted");
    const round = (base) => { for (let i = 0; i < 20000; i++) { const spec = {}; spec["field_with_a_long_distinct_name_" + (base + i)] = { sel }; new scrape.Extractor(spec, { text: HTMLText }); } };
    round(0);
    const m1 = memoryUsage().mallocSize;
    round(100000);
    const m2 = memoryUsage().mallocSize;
    ok(m2 - m1 < 200000, "[red] 20000 more extractors retain nothing (" + (m2 - m1) + " bytes grew)");
});
await t.run();
