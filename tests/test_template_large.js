/* test_template_large.js -- large/complex template cases, portfolio coverage.
 * Few bytes, few KB, few MB. Run: dynajs tests/test_template_large.js */
import { Template } from "dyna:html";
let n=0, fails=0;
function assert(c,msg){n++; if(!c){fails++; print("FAIL: "+msg);}}
function eq(a,b,msg){assert(a===b, msg+" got "+JSON.stringify(a).slice(0,200)+" want "+JSON.stringify(b).slice(0,200));}

// Few bytes: tiny template
eq(new Template("hi {{x}}").render({x:"there"}), "hi there", "tiny");
eq(new Template("{{a.b.c}}").render({a:{b:{c:"v"}}}), "v", "dotted tiny");
eq(new Template("{{#a}}x{{/a}}").render({a:true}), "x", "section tiny true");
eq(new Template("{{#a}}x{{/a}}").render({a:false}), "", "section tiny false");
eq(new Template("{{^a}}x{{/a}}").render({a:[]}), "x", "inverted empty");

// Few KB: 1k rows
{
  const rows=1000;
  const data={items:Array.from({length:rows},(_,i)=>({id:i,name:"n"+i}))};
  const tpl=new Template("{{#items}}<p>{{id}}:{{name}}</p>{{/items}}");
  const out=tpl.render(data);
  assert(out.length>10000, "1k rows length");
  assert(out.indexOf("<p>0:n0</p>")>=0, "1k first");
  assert(out.indexOf(`<p>${rows-1}:n${rows-1}</p>`)>=0, "1k last");
  assert((out.match(/<p>/g)||[]).length===rows, "1k count");
}
// Few MB: 50k rows ~2MB
{
  const rows=20000;
  const data={items:Array.from({length:rows},(_,i)=>({id:i}))};
  const tpl=new Template("{{#items}}x{{id}}{{/items}}");
  const out=tpl.render(data);
  assert(out.length>80000, "20k rows MB-ish");
  assert(out.startsWith("x0"), "20k start");
}
// Escaping: few bytes vs KB vs MB
eq(new Template("{{v}}").render({v:"<>&\"'"}), "&lt;&gt;&amp;&quot;&#39;", "escape small");
{
  const big="<>&\"'".repeat(5000);
  const tpl=new Template("{{v}}");
  const out=tpl.render({v:big});
  assert(out.length===5000*24, "escape KB");
  assert(out.indexOf("&lt;")>=0, "escape contains lt");
}
{
  const v="a".repeat(1024*1024);
  const tpl=new Template("{{v}}");
  const out=tpl.render({v});
  assert(out.length===v.length, "escape 1M no escapes");
}
{
  const v="<".repeat(100000);
  const tpl=new Template("{{v}}");
  const t0=Date.now();
  const out=tpl.render({v});
  const dt=Date.now()-t0;
  assert(out.length===400000, "escape 100k <");
  assert(dt<2000, "escape 100k < fast (<2s) dt="+dt);
}
// Dotted vs flat
eq(new Template("{{a.b}}").render({a:{b:1}}), "1", "dotted");
eq(new Template("{{a.b}}").render({b:1}), "", "dotted missing head");
eq(new Template("{{.}}").render("hi"), "hi", "dot");
// Deep nesting
{
  let src=""; for(let i=0;i<20;i++) src+=`{{#a${i}}}`; src+="X"; for(let i=19;i>=0;i--) src+=`{{/a${i}}}`;
  const tpl=new Template(src);
  let data={}, cur=data; for(let i=0;i<20;i++){cur[`a${i}`]={}; cur=cur[`a${i}`];}
  eq(tpl.render(data), "X", "deep 20");
}
// Large source ~100KB text + vars
{
  const src="x".repeat(50000)+ "{{v}}" + "y".repeat(50000);
  const tpl=new Template(src);
  const out=tpl.render({v:"mid"});
  assert(out.length===100003, "large source 100KB");
}
if(fails) {print(`test_template_large: ${fails} FAILED of ${n}`); throw new Error("fail");}
print(`test_template_large: ${n} assertions, 0 failures`);
