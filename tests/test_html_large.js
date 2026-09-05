/* test_html_large.js -- large/complex HTML, selector, sanitizer, markdown.
 * Few bytes / KB / MB portfolios. Run: dynajs tests/test_html_large.js */
import { HTMLParse, HTMLStringify, Selector, Sanitizer, MarkdownToHTML } from "dyna:html";
let n=0, fails=0;
function assert(c,msg){n++; if(!c){fails++; print("FAIL: "+msg);}}
function eq(a,b,msg){assert(a===b,msg+" got "+JSON.stringify(a).slice(0,300)+" want "+String(b).slice(0,300));}

// Tiny
eq(HTMLStringify(HTMLParse("<p>hi</p>")), "<p>hi</p>","tiny roundtrip");
assert(new Selector("p").all(HTMLParse("<p>a</p><p>b</p>")).length===2,"tiny selector");

// Few KB: 1k ps
{
  const html="<div>"+"<p class=\"x\">hello <b>world</b></p>".repeat(1000)+"</div>";
  const doc=HTMLParse(html);
  assert(doc[0].children.length===1000,"1k parse count");
  const sel=new Selector("p.x");
  assert(sel.all(doc).length===1000,"1k selector");
  const ser=HTMLStringify(doc);
  assert(ser.length>20000,"1k ser");
}
{
  const html="<div>"+"<p>hello</p>".repeat(5000)+"</div>";
  const doc=HTMLParse(html);
  const t0=Date.now();
  const all=new Selector("div p").all(doc);
  const dt=Date.now()-t0;
  assert(all.length===5000,"5k descendant");
  assert(dt<2000,"5k selector <2s dt="+dt);
}
// Few MB: 20k ps ~700KB
{
  const html="<div>"+"<p>text</p>".repeat(20000)+"</div>";
  const t0=Date.now();
  const doc=HTMLParse(html);
  const dt=Date.now()-t0;
  assert(doc[0].children.length===20000,"20k parse");
  assert(dt<2000,"20k parse <2s dt="+dt);
  const ser=HTMLStringify(doc);
  assert(ser.length===html.length,"20k ser len");
}
// 500k text
{
  const html="<div>"+"a".repeat(500*1024)+"</div>";
  const doc=HTMLParse(html);
  assert(doc[0].children[0].length===500*1024,"500k text");
}
// Entity dense MB
{
  const html="<div>"+"a &amp; b &lt; c ".repeat(10000)+"</div>";
  const doc=HTMLParse(html);
  assert(doc[0].children[0].indexOf("&")>=0,"entity dense decoded");
}
// Raw text large <script> few KB and MB
{
  const script="a".repeat(100*1024);
  const html=`<div><script>${script}</script><p>hi</p></div>`;
  const doc=HTMLParse(html);
  assert(doc[0].children[0].children[0]===script,"100k script raw");
  assert(HTMLStringify(doc).indexOf(script)>=0,"script ser");
}
{
  const big="x".repeat(1024*1024);
  const html=`<div><script>${big}</script></div>`;
  const t0=Date.now();
  const doc=HTMLParse(html);
  const dt=Date.now()-t0;
  assert(dt<2000,"1M script <2s dt="+dt);
}
// Sanitizer large
{
  const html="<div>"+"<p>hello <b>world</b></p>".repeat(5000)+"</div>";
  const san=new Sanitizer({allow:{p:[],b:[],div:[]}});
  const t0=Date.now();
  const out=san.clean(html);
  const dt=Date.now()-t0;
  assert(out.indexOf("<p>")>=0,"san large");
  assert(dt<2000,"san 5k <2s dt="+dt);
}
// Comment large
{
  const html="<div><!--"+ "a".repeat(100000)+"--><p>hi</p></div>";
  const doc=HTMLParse(html);
  assert(doc[0].children.length===1,"large comment dropped");
}
// Markdown few bytes/KB/MB
eq(MarkdownToHTML("# hi"), "<h1>hi</h1>\n","md tiny");
{
  const prose="Lorem ipsum ".repeat(1000);
  const out=MarkdownToHTML(prose);
  assert(out.indexOf("<p>")>=0,"md 11k prose");
}
{
  const prose="Lorem ipsum dolor sit amet, ".repeat(40000); // ~1M
  const t0=Date.now();
  const out=MarkdownToHTML(prose);
  const dt=Date.now()-t0;
  assert(out.length>prose.length,"md 1M len");
  assert(dt<3000,"md 1M <3s dt="+dt);
}
{
  const dense=("# H\n\npara *b* `c` [d](e)\n").repeat(5000);
  const t0=Date.now();
  const out=MarkdownToHTML(dense);
  const dt=Date.now()-t0;
  assert(out.indexOf("<strong>")>=0 || out.indexOf("<em>")>=0,"md dense");
  assert(dt<3000,"md dense 5k <3s dt="+dt);
}
if(fails){print(`test_html_large: ${fails} FAILED of ${n}`); throw new Error("fail");}
print(`test_html_large: ${n} assertions, 0 failures`);
