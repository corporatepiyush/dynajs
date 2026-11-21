// covers: c:src/runtime/class.inc.c
// tests/core/test_tocstring_eval_wide.js -- COMPAT-6 (B16), core-only twin of
// test_tocstring_exact.js for the sanitizer legs: eval converts its source with
// JS_ToCStringLen, so a wide source whose 3-per-unit reservation passes 1 MiB
// is sized by the UTF-8 length count instead. An undercount there is a heap
// overflow under ASan, and the 8-unit ASCII copy must keep the source intact.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_tocstring_eval_wide");
let n = 0;
function assert(c, m) { n++; if (!c) throw new Error("assertion failed: " + m); }

__kitS.section("eval of a", () => {
const P = ["a", "\u00e9", "\u4e2d", "\u{1F600}", "\u07ff", "\u0800", "\uffff", "\ud800"];
for (const len of [21000, 400000, 1500000]) {
    let body = "";
    let k = 0;
    while (body.length < len) body += P[k++ % P.length];
    assert(eval("/*" + body + "*/ 40 + 2") === 42, "eval of a " + body.length + "-unit wide source");
    assert(eval("'" + body.replace(/[\ud800-\udfff]/g, "x") + "'").length >= len - 10, "a " + len + "-unit wide string literal survives eval");
}
print("test_tocstring_eval_wide: " + n + " assertions passed");
});
__kitS.end();
