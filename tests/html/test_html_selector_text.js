// covers: dyna:html.Selector
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_html_selector_text");
import { HTMLParse, HTMLText, Selector } from "dyna:html";
let n=0,bad=0; const ok=(c,w)=>{n++;if(!c){bad++;print("FAIL: "+w);}};
const doc = HTMLParse(`<div><p class="x">Hello <b>world</b>!</p>
<script>var evil = "NOT TEXT";</script><style>.a{color:red}</style>
<ul><li>one</li><li>two</li></ul><p>A<span>B</span>C</p></div>`);

__kitS.section("per-node extraction", () => {
ok(new Selector("li").all(doc).map(HTMLText).join(",") === "one,two", "per-node extraction");
print("test: " + n + " assertions, " + bad + " failures");
if (bad) throw new Error("fail");
});
__kitS.end();
