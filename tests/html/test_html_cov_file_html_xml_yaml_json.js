// Split from tests/test_cov_file_html_xml_yaml_json.js: the html sections.
import { makeTempDir } from "dyna:file";
import { HTMLParse, HTMLStringify, HTMLText, MarkdownToHTML, Selector, Sanitizer } from "dyna:html";

let n=0,fails=0;
function assert(c,m){n++; if(!c){fails++; print("FAIL: "+m)}}
function eq(a,b,m){assert(a===b,m)}
function throws(fn,re,m){n++; try{fn(); fails++; print("FAIL: "+m)}catch(e){if(re && !re.test(String(e.message))) {fails++; print("FAIL: "+m+" wrong msg "+e.message)}}}
function j(v){ return JSON.stringify(v); }
function hex(u){ return Array.from(u,b=>b.toString(16).padStart(2,"0")).join(""); }
function bytes(h){ return new Uint8Array(h.match(/../g) ? h.match(/../g).map(x=>parseInt(x,16)) : []); }
function logStep(s){ try{ let fs=require("fs"); }catch(e){} }
let ROOT = makeTempDir("cov-file-");
print("  dyna:file covered");
{
  eq(MarkdownToHTML(""),"","md empty");
  eq(MarkdownToHTML("# hi"),"<h1>hi</h1>\n","heading");
  eq(MarkdownToHTML("## h2"),"<h2>h2</h2>\n","h2");
  let ul = MarkdownToHTML("- a\n- b");
  assert(ul.includes("<ul>") && ul.includes("<li>a</li>"),"list");
  let ol = MarkdownToHTML("1. a\n2. b");
  assert(ol.includes("<ol>") && ol.includes("<li>"),"ordered list");
  let code = MarkdownToHTML("```js\ncode\n```");
  assert(code.includes("<pre><code") && code.includes("code"),"code block");
  let inlineCode = MarkdownToHTML("`x`");
  assert(inlineCode.includes("<code>x</code>"),"inline code");
  let para = MarkdownToHTML("hello world");
  assert(para.includes("<p>hello world</p>"),"paragraph");
  let esc = MarkdownToHTML('<script>alert(1)</script>');
  assert(!esc.includes("<script>") && esc.includes("&lt;script&gt;"),"default escapes script");
  let raw = MarkdownToHTML('<script>alert(1)</script>',{allowRawHTML:true});
  assert(raw.includes("<script>alert(1)</script>"),"allowRawHTML true");
  let rawFalse = MarkdownToHTML('<script>alert(1)</script>',{allowRawHTML:false});
  assert(!rawFalse.includes("<script>"),"allowRawHTML false escapes");
  let mdLink = MarkdownToHTML('[x](javascript:alert(1))');
  assert(!mdLink.includes("javascript:"),"md link javascript stripped");
  let mdLink2 = MarkdownToHTML('[x](https://example.com)');
  assert(mdLink2.includes('href="https://example.com"'),"md https link allowed");
  let unk = MarkdownToHTML("hi",{allowRawHTML:true, unknownOption:123});
  assert(unk.includes("<p>hi</p>"),"unknown option ignored");
  throws(()=>MarkdownToHTML(42),/string/,"md non-string throws");
  throws(()=>MarkdownToHTML(),/string/,"md no arg throws");
  let h7 = MarkdownToHTML("####### hi");
  assert(h7.includes("<p>"),"h7 not heading");
  assert(MarkdownToHTML("*em*").includes("<em>em</em>"),"em");
  assert(MarkdownToHTML("**strong**").includes("<strong>strong</strong>"),"strong");
  assert(MarkdownToHTML("> quote").includes("<blockquote>"),"blockquote");
  assert(MarkdownToHTML("---").includes("<hr>"),"hr");

  let doc = HTMLParse("<p>hi</p>");
  eq(j(doc),'[{"name":"p","attrs":{},"children":["hi"]}]',"node shape");
  eq(HTMLStringify(doc[0]),"<p>hi</p>","round trip");
  eq(HTMLStringify(HTMLParse("<P>hi</P>")[0]),"<p>hi</p>","lowercase tag");
  eq(HTMLStringify(HTMLParse("<p CLASS=x>hi</p>")[0]),'<p class="x">hi</p>',"attr lower");
  eq(HTMLStringify(HTMLParse("<input disabled>")[0]),'<input disabled="">',"valueless attr");
  eq(HTMLStringify(HTMLParse("<div><img src=a>text</div>")[0]),'<div><img src="a">text</div>',"void no scope");
  eq(HTMLStringify(HTMLParse("<p>one<p>two")[0]),"<p>one</p>","implied p closes");
  eq(HTMLStringify(HTMLParse("<div>a</span>b</div>")[0]),"<div>ab</div>","stray close ignored");
  let scr = HTMLParse("<script>if (a < b) { x() }</script>");
  eq(scr[0].children[0],"if (a < b) { x() }","script raw");
  eq(HTMLParse("<p>&amp;&lt;&gt;</p>")[0].children[0],"&<>","entities decode");
  eq(HTMLStringify(HTMLParse("<p>a<!--c-->b</p>")[0]),"<p>ab</p>","comment stripped");
  eq(HTMLParse("<!DOCTYPE html><p>x</p>")[0].name,"p","doctype skipped");
  throws(()=>HTMLParse("<div>".repeat(300)),/nesting|exceeds/,"html depth cap");
  throws(()=>HTMLParse(42),/string/,"html parse non-string");

  eq(HTMLText(HTMLParse("<p>hi <b>there</b></p>")[0]),"hi there","HTMLText single");
  eq(HTMLText(HTMLParse("<p>hi</p>")), "hi","HTMLText array");
  eq(HTMLText(HTMLParse("<script>alert(1)</script>")[0]),"","script not text");
  eq(HTMLText(HTMLParse("<style>a>b</style>")[0]),"","style not text");

  let selDoc = HTMLParse('<div id="root" class="a b"><p class="x">one<b>bold</b></p><p class="y">two</p><span><p class="x">three</p></span></div>');
  let allP = new Selector("p").all(selDoc);
  eq(allP.length,3,"sel p all");
  eq(new Selector("#root").all(selDoc).length,1,"id");
  eq(new Selector(".x").all(selDoc).length,2,"class word");
  eq(new Selector("[id]").all(selDoc).length,1,"attr presence");
  eq(new Selector('[id="root"]').all(selDoc).length,1,"attr eq");
  eq(new Selector("div p").all(selDoc).length,3,"descendant");
  eq(new Selector("div > p").all(selDoc).length,2,"child excludes nested");
  eq(new Selector("span > p").all(selDoc).length,1,"span > p");
  eq(new Selector("p, b").all(selDoc).length,4,"group union");
  eq(new Selector("*").all(selDoc).length,6,"universal");
  eq(new Selector("nope").all(selDoc).length,0,"no match");
  eq(new Selector("p").first(selDoc).attrs.class,"x","first doc order");
  eq(new Selector("nope").first(selDoc),undefined,"first undefined");
  assert(new Selector("p").matches(allP[0]),"matches true");
  assert(!new Selector("div").matches(allP[0]),"matches false");
  throws(()=>new Selector("div p").matches(allP[0]),/combinator/,"matches combinator throws");
  for(let bad of ["","  ","#",".", "[", "p >", ">", "p,"] ) throws(()=>new Selector(bad),/Selector|empty|unexpected/,"invalid sel "+j(bad));
  throws(()=>new Selector(42),/string/,"sel non-string");
  eq(new Selector("p:first-child").all(selDoc).length,2,"first-child");
  eq(new Selector("p:last-child").all(selDoc).length,1,"last-child");
  eq(new Selector("[class^=\"a\"]").all(selDoc).length,1,"prefix");
  eq(new Selector("[class$=\"y\"]").all(selDoc).length,1,"suffix");
  eq(new Selector("[class*=\" \"]").all(selDoc).length,1,"substring");
  eq(new Selector("p.x").all(selDoc).length,2,"compound");
  let s = new Selector("p");
  let d1 = HTMLParse("<p>a</p>"), d2 = HTMLParse("<div><p>b</p><p>c</p></div>");
  eq(s.all(d1).length,1,"reuse d1");
  eq(s.all(d2).length,2,"reuse d2");
  let orderDoc = HTMLParse("<b>1</b><b>2</b><b>3</b>");
  let order = new Selector("b").all(orderDoc).map(n=>HTMLText(n));
  eq(j(order),'["1","2","3"]',"document order");

  let san = new Sanitizer({allow:{p:[], b:[], i:[], a:["href","title"], img:["src","alt"]}, protocols:{"a.href":["https","http","mailto"],"img.src":["https"]}});
  eq(san.clean("<p>plain <b>bold</b></p>"),"<p>plain <b>bold</b></p>","san allowed");
  eq(san.clean('<a href="https://e.com" title="t">x</a>'),'<a href="https://e.com" title="t">x</a>',"allowed attrs");
  let san2 = new Sanitizer({allow:{p:[]}});
  eq(san2.clean('<p onclick="alert(1)">x</p>'),"<p>x</p>","strip event");
  eq(san.clean('<a href="javascript:alert(1)">x</a>'),"<a>x</a>","js stripped");
  eq(san.clean('<a href="JaVaScRiPt:alert(1)">x</a>'),"<a>x</a>","js cased");
  eq(san.clean('<img src="javascript:alert(1)">'),"<img>","img js");
  eq(san.clean('<img src="http://e.com/x.png">'),"<img>","http not allowed");
  eq(san.clean('<iframe src="https://evil"></iframe>'),"","iframe stripped");
  eq(san.clean("<script>alert(1)</script>"),"","script stripped");
  eq(san.clean("<style>body{}</style>"),"","style stripped");
  eq(san.clean('<p><svg onload="alert(1)"></svg></p>'),"<p></p>","svg handler");
  let sanNoProto = new Sanitizer({allow:{a:["href"]}});
  eq(sanNoProto.clean('<a href="/relative">x</a>'),'<a href="/relative">x</a>',"relative no protocol check");
  eq(san.clean('<img src="https://a.com/x">'),'<img src="https://a.com/x">',"https allowed");
  eq(san.clean('<div>kept</div>'),"kept","disallowed keeps text");
  eq(san.clean("<div><p>kept</p></div>"),"<p>kept</p>","allowed descendants kept");
  eq(san.clean("<p>a &lt; b</p>"),"<p>a &lt; b &amp; c</p>" ? "<p>a &lt; b</p>" : "<p>a &lt; b</p>","re-escape");
  eq(san.clean("<p>a &lt; b &amp; c</p>"),"<p>a &lt; b &amp; c</p>","text re-escaped");
  let dirty = '<p onclick=x>a<script>b</script><a href="javascript:1">c</a><b>d</b><div>e</div></p>';
  let once = san.clean(dirty);
  eq(san.clean(once),once,"idempotent");
  throws(()=>new Sanitizer(),/allow/,"san no arg");
  throws(()=>new Sanitizer({}),/allow/,"san empty");
  throws(()=>san.clean(42),/string/,"clean non-string");
  print("  dyna:html covered");
}
if(fails){
  print("test_html_cov_file_html_xml_yaml_json: "+fails+" FAILED of "+n+" assertions");
  throw new Error("test_cov failed");
}
print("test_html_cov_file_html_xml_yaml_json: "+n+" assertions, 0 failures");
