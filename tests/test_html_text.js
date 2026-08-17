import { HTMLParse, HTMLText, Selector } from "dyna:html";
let n=0,bad=0; const ok=(c,w)=>{n++;if(!c){bad++;print("FAIL: "+w);}};
const doc = HTMLParse(`<div><p class="x">Hello <b>world</b>!</p>
<script>var evil = "NOT TEXT";</script><style>.a{color:red}</style>
<ul><li>one</li><li>two</li></ul><p>A<span>B</span>C</p></div>`);
ok(HTMLText(new Selector("p.x").first(doc)) === "Hello world!", "nested inline text: " + JSON.stringify(HTMLText(new Selector("p.x").first(doc))));
const whole = HTMLText(doc);
ok(whole.indexOf("NOT TEXT") < 0, "script contents are NOT text");
ok(whole.indexOf("color:red") < 0, "style contents are NOT text");
ok(whole.indexOf("Hello world!") >= 0, "real text survives");
ok(new Selector("li").all(doc).map(HTMLText).join(",") === "one,two", "per-node extraction");
ok(HTMLText(new Selector("p").all(doc)[1]) === "ABC", "array arg + sibling concat");
ok(HTMLText([]) === "", "empty array is empty string");
ok(HTMLText("bare string") === "bare string", "a string node is its own text");
/* uppercase tag built by hand must still be skipped */
ok(HTMLText({name:"SCRIPT",attrs:{},children:["x"]}) === "", "uppercase SCRIPT skipped");
print("test: " + n + " assertions, " + bad + " failures");
if (bad) throw new Error("fail");
