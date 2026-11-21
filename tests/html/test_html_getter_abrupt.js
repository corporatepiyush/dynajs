// flags: --std
// timeout: 240
// covers: contract dyna:html.Sanitizer dyna:html.Selector dyna:html.HTMLStringify dyna:html.HTMLText dyna:html.rewriteLinks -- D49: option, list and child reads propagate
// st1s D49: new Sanitizer read allow and protocols and type-tested them first, so a throwing getter became "allow maps a
// tag" TypeError or was ignored with the error pending; Selector's positional pre-count read children and skipped a
// throwing one, so the walk returned a result with the error pending; HTMLStringify of a raw-text element (script)
// skipped a throwing first child the same way. rewriteLinks freed a node list's length value a second time after ht_len
// had freed it: a Proxy array whose length trap returns a fresh string was a use-after-free (ASan). Oracle: each hostile
// call throws exactly the sentinel; a Proxy length trap throws it from every list reader; GC inside child getters keeps
// the output; a --timeout-ms interrupt inside the allow getter ends a catch-all loop.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_html_getter_abrupt");
import { Exec, args } from "dyna:sys";
import * as std from "std";
const SELF = "tests/html/test_html_getter_abrupt.js";
const SENT = new Error("test_html_getter_abrupt sentinel");
const thrower = () => { throw SENT; };
const spin = () => { let i = 0; for (;;) i++; };
const run = (f) => { try { return { v: f() }; } catch (e) { return { e }; } };
const with_ = (base, k, get = thrower) => { const x = { ...base }; delete x[k]; Object.defineProperty(x, k, { get, enumerable: true, configurable: true }); return x; };
const gcw = (base) => { const x = Array.isArray(base) ? [] : {}; for (const k of Object.keys(base)) { const v = base[k]; Object.defineProperty(x, k, { get() { std.gc(); return v; }, enumerable: true, configurable: true }); } return x; };
const getArr = (vals, i, get = thrower) => { const a = vals.slice(); Object.defineProperty(a, i, { get, enumerable: true, configurable: true }); return a; };
const lenProxy = (vals, len = thrower) => new Proxy(vals.slice(), { get(t, k, r) { return k === "length" ? len() : Reflect.get(t, k, r); } });
const __NAME = "test_html_getter_abrupt";
import { HTMLParse, HTMLStringify, HTMLText, Selector, Sanitizer, rewriteLinks } from "dyna:html";
const DOC = '<div><p>a</p><p>b<a href="https://x/y">l</a></p><script>x<y</script></div>';
const PINNED = [
    ["stringify", () => HTMLStringify(HTMLParse(DOC)[0])],
    ["selector", () => String(new Selector("p:last-child").all(HTMLParse(DOC)).length)],
    ["sanitize", () => new Sanitizer({ allow: { p: [], a: ["href"] }, protocols: { "a.href": ["https"] } }).clean(DOC)],
    ["rewrite", () => { const d = HTMLParse(DOC); return rewriteLinks(d, (u) => u + "#") + HTMLStringify(d[0]); }],
];
const INTERRUPTS = { interrupt: () => new Sanitizer(with_({ allow: { p: [] } }, "allow", spin)) };

if (args().includes("--child")) {
    const mode = args()[args().indexOf("--child") + 1];
    if (INTERRUPTS[mode]) {
        for (;;) { try { INTERRUPTS[mode](); } catch (e) {} }
    }
    std.exit(0);
}
let n = 0, fails = 0;
const ok = (c, w) => { n++; if (!c) { fails++; print("  FAIL  " + w); } };
const desc = (r) => r.e !== undefined ? "threw " + (r.e === SENT ? "SENT" : String(r.e)) : "returned " + String(JSON.stringify(r.v)).slice(0, 160);
const sent = (w, f) => { const r = run(f); ok(r.e === SENT, w + ": " + desc(r)); ok(run(() => JSON.parse("[1]")).v?.[0] === 1, w + ": nothing left pending"); };

const node = () => HTMLParse("<p>z</p>")[0];
const once = (v) => { let first = true; return () => { if (first) { first = false; throw SENT; } return v; }; };
await __kitS.section("Sanitizer options and lists propagate every getter", () => {
    sent("Sanitizer allow getter", () => new Sanitizer(with_({ allow: { p: [] } }, "allow")));
    sent("Sanitizer protocols getter", () => new Sanitizer(with_({ allow: { a: ["href"] } }, "protocols")));
    sent("Sanitizer allow attribute list Proxy length", () => new Sanitizer({ allow: { a: lenProxy(["href"]) } }));
    sent("Sanitizer protocol list Proxy length", () => new Sanitizer({ allow: { a: ["href"] }, protocols: { "a.href": lenProxy(["https"]) } }));
});
await __kitS.section("node-list and child readers propagate", () => {
    sent("HTMLStringify Proxy length", () => HTMLStringify(lenProxy([node()])));
    sent("HTMLText Proxy length", () => HTMLText(lenProxy([node()])));
    sent("rewriteLinks Proxy length", () => rewriteLinks(lenProxy([node()]), (u) => u));
    sent("Selector positional pre-count child getter", () => new Selector("p:first-child").all(getArr([node()], 0, once(node()))));
    sent("HTMLStringify raw-text first child getter", () => HTMLStringify({ name: "script", attrs: {}, children: getArr(["x"], 0) }));
    let r;
    for (let i = 0; i < 300; i++) { r = run(() => rewriteLinks(lenProxy([HTMLParse('<a href="h">x</a>')[0]], () => "1" + " ".repeat(40 + i % 7)), (u) => u + "!")); if (r.e !== undefined) break; if (i % 50 === 0) std.gc(); }
    ok(r.v === 1, "rewriteLinks with a fresh-string Proxy length, 300 rounds: " + desc(r));
    const g = HTMLStringify({ name: "div", attrs: {}, children: gcw(["a", node()]) });
    ok(g === "<div>a<p>z</p></div>", "gc in child getters keeps the output: " + g);
});

await __kitS.section("pinned controls and interrupts that must end a catch-all loop", () => {
    for (const mode of Object.keys(INTERRUPTS)) {
        const t0 = Date.now();
        const i = Exec(args()[0], ["--std", "--timeout-ms", "300", SELF, "--child", mode], { timeoutMs: 20000 });
        ok(i.code !== 0 && !i.timedOut && /interrupt/i.test(i.stderr + i.stdout), mode + " child ends: rc " + i.code + " after " + (Date.now() - t0) + " ms " + i.stderr.slice(-200));
    }
});
if (fails) throw new Error(fails + " of " + n + " checks failed");
print(__NAME + ": " + n + " checks passed");
