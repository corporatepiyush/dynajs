// flags: --std
import { LinearRegression } from "dyna:ml";
import { makeTempDir, File, Path, removeAll } from "dyna:file";
import * as os from "os";
import { Selector, HTMLParse } from "dyna:html";
import { Extractor } from "dyna:scrape";

let n = 0, fails = 0;
function assert(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function eq(a, b, m) { assert(a === b, m + " (got " + JSON.stringify(a) + " want " + JSON.stringify(b) + ")"); }

{
    const dir = makeTempDir("p2ml-");
    const p = new Path(dir.toString() + "/model.bin");
    const m = new LinearRegression();
    m.fit([[1],[2],[3]], [2,4,6]);
    const bytes = m.save(p);
    assert(bytes > 10, "ML save returns bytes " + bytes);
    const m2 = LinearRegression.load(p);
    eq(JSON.stringify(m2.predict([[4]])), JSON.stringify(m.predict([[4]])), "ML save/load round-trip");
    const f = new File(p);
    const st = f.stat();
    const sz = st ? st.size : 0;
    assert(sz > 10, "ML save file size " + sz);
    removeAll(dir);
}

{
    const t0 = Date.now();
    os.sleep(50);
    const dt = Date.now() - t0;
    assert(dt >= 45 && dt < 200, "os.sleep 50ms actual " + dt + "ms");
}

{
    const doc = HTMLParse("<div><p>hi</p></div>");
    const sel = new Selector("span");
    const first = sel.first(doc);
    assert(first === undefined, "Selector.first returns undefined on no match (got " + String(first) + ")");
    const sel2 = new Selector("p");
    const first2 = sel2.first(doc);
    assert(first2 !== null && first2 !== undefined, "Selector.first returns element on match");
}

{
    const html = `<div><p class="a">hello</p></div>`;
    const doc = HTMLParse(html);
    const ex = new Extractor({
        title: { sel: new Selector("p"), attr: "text" }
    });
    const res = ex.run(doc);
    assert(typeof res.ok === "boolean", "Extractor.run has ok boolean");
    assert(res.value && typeof res.value === "object", "Extractor.run has value object");
    assert(Array.isArray(res.missing), "Extractor.run has missing array");
    const ex2 = new Extractor({
        req: { sel: new Selector("span"), required: true }
    });
    const res2 = ex2.run(doc);
    eq(res2.ok, false, "Extractor.run ok false when required missing");
    assert(res2.missing.includes("req"), "Extractor.run missing includes req");
}

if (fails) { print("test_p2_batch2: " + fails + " FAILED of " + n + " assertions"); throw new Error("test_p2_batch2 failed"); }
print("test_p2_batch2: " + n + " assertions, 0 failures");
