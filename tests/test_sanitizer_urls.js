/* test_sanitizer_urls.js -- T1: sanitizer URL-sink matrix (plan S5/S6 locks) */
import { Sanitizer } from "dyna:html";
let pass = 0, fail = 0;
const ok = (c, w) => { if (c) pass++; else { fail++; print("  FAIL  " + w); } };
const san = new Sanitizer({ allow: { a: ["href"], p: [], div: ["style"] },
                             protocols: { "a.href": ["https", "http"] } });
const out = (html) => san.clean(html);
ok(!out('<a href="javascript:alert(1)">x</a>').includes("javascript"), "javascript: dropped");
ok(!out('<a href="java\tscript:alert(1)">x</a>').includes("script"), "java\\tscript dropped");
ok(!out('<a href="data:text/html,x">x</a>').includes("data:"), "data: dropped");
ok(!out('<a href="vbscript:x">x</a>').includes("vbscript"), "vbscript dropped");
ok(out('<a href="https://ok.test/">y</a>').includes("https://ok.test/"), "https kept");
ok(out('<a href="/rel">y</a>').includes('href="/rel"'), "relative kept");
ok(out('<a href="//evil.test/x">//</a>').includes("//evil.test/x"), "S6: protocol-relative passes (documented)");
ok(!out('<div style="color:red">c</div>').includes("style"), "S5: style with colon dropped (documented)");
ok(!out('<a href="x.png 1x, javascript:alert(1) 2x">s</a>').includes("javascript"), "srcset-shaped href dropped");
print("test_sanitizer_urls: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error("test_sanitizer_urls: " + fail + " failures");
