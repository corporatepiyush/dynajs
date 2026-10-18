// Split from tests/test_cov_file_html_xml_yaml_json.js: the xml sections.
import { makeTempDir } from "dyna:file";
import { XMLParse, XMLStringify, XMLToObject, SAXParser } from "dyna:xml";

let n=0,fails=0;
function assert(c,m){n++; if(!c){fails++; print("FAIL: "+m)}}
function eq(a,b,m){assert(a===b,m)}
function throws(fn,re,m){n++; try{fn(); fails++; print("FAIL: "+m)}catch(e){if(re && !re.test(String(e.message))) {fails++; print("FAIL: "+m+" wrong msg "+e.message)}}}
function j(v){ return JSON.stringify(v); }
function hex(u){ return Array.from(u,b=>b.toString(16).padStart(2,"0")).join(""); }
function bytes(h){ return new Uint8Array(h.match(/../g) ? h.match(/../g).map(x=>parseInt(x,16)) : []); }
function logStep(s){ try{ let fs=require("fs"); }catch(e){} }
let ROOT = makeTempDir("cov-file-");
{
  throws(()=>XMLParse(""),/root/,"empty doc throws");
  throws(()=>XMLParse("  "),/root/,"whitespace only");
  let t = XMLParse("<a/>");
  eq(t.name,"a","self closing name");
  eq(j(t.attrs),"{}","no attrs");
  eq(t.children.length,0,"no children");
  let t2 = XMLParse('<a id="1" x="y">hi<b/>there</a>');
  eq(t2.attrs.id,"1","attr id");
  eq(t2.attrs.x,"y","attr x");
  eq(t2.children.length,3,"mixed");
  eq(t2.children[0],"hi","text first");
  eq(t2.children[1].name,"b","element mid");
  eq(XMLParse("<a><b><c>deep</c></b></a>").children[0].children[0].children[0],"deep","nested");
  eq(XMLParse("<a>  </a>").children.length,0,"trim default");
  eq(XMLParse("<a>  </a>",{trim:false}).children[0],"  ","trim false");
  eq(XMLParse("<a>&lt;&gt;&amp;&apos;&quot;</a>").children[0],"<>&'\"","five entities");
  eq(XMLParse("<a>&#65;&#x42;</a>").children[0],"AB","numeric");
  eq(XMLParse("<a>&custom;</a>",{entities:"keep"}).children[0],"&custom;","keep custom");
  throws(()=>XMLParse("<a>&custom;</a>"),/unknown entity/,"strict unknown");
  eq(XMLParse('<a b="&lt;&#65;"/>').attrs.b,"<A","attr entities");
  eq(XMLParse("<a><![CDATA[<b>&amp;]]></a>").children[0],"<b>&amp;","cdata literal");
  for(let bad of ["<a>", "</a>", "<a></b>", "<a><b></a></b>", "text", "<a>x", "<a", "<>", "<1a/>", "<a b=/>", "<a>&nope;</a>", "<a>&#xD800;</a>", "<a>&#0;</a>", "<a/><b/>", "<a>]]></a>"])
    throws(()=>XMLParse(bad),/./,"malformed "+j(bad));
  throws(()=>XMLParse(42),/string/,"xml non-string");
  throws(()=>XMLParse("<a/>",{entities:"expand"}),/entities/,"unknown entities mode");
  throws(()=>XMLParse("x".repeat(256*1024*1024+1)),/exceeds/,"256MB cap");
  throws(()=>XMLParse("<a>".repeat(300)+"</a>".repeat(300)),/nesting/,"xml nesting cap");
  let out=[];
  let p = new SAXParser({onOpen:(n,a)=>out.push(["open",n]), onText:t=>out.push(["text",t]), onClose:n=>out.push(["close",n])});
  p.write("<a>hi</a>"); p.end();
  eq(j(out),'[["open","a"],["text","hi"],["close","a"]]','sax basic');
  throws(()=>{ let pp=new SAXParser({}); pp.write("<a>&nope;</a>"); pp.end(); },/unknown entity/,"sax unknown");
  throws(()=>new SAXParser({onOpen:42}),/function/,"sax bad handler");
  throws(()=>new SAXParser(42),/object/,"sax non-object handlers");
  {
    let pp=new SAXParser(); pp.write("<a>&nope;</a>"); pp.end(); let threw=0;
    try { for(;;){ let e=pp.next(); if(e===null) break; } } catch(err) { threw=err.message.indexOf("unknown entity")>=0?1:0; }
    eq(threw,1,"sax pull unknown entity");
  }
  throws(()=>{ let pp=new SAXParser({}); pp.write(); },/chunk/,"sax write no chunk");
  let out2=[];
  let p2 = new SAXParser({onText:t=>out2.push(t)});
  p2.write(new Uint8Array([60,97,62,104,105,60,47,97,62]));
  p2.end();
  eq(out2.join(""),"hi","Uint8Array chunk");
  let threw=false;
  let pr = new SAXParser({onOpen(){ try{ pr.write("<x/>"); }catch(e){ threw=true; } }});
  pr.write("<a/>"); pr.end();
  assert(threw,"write from handler refused");
  let pt = new SAXParser({onOpen(){ throw new Error("stop"); }});
  throws(()=>pt.write("<a/>"),/stop/,"handler throw propagates");
  let pe = new SAXParser({});
  pe.write("<a>x</a>"); pe.end();
  throws(()=>pe.write("<b/>"),/end/,"write after end");
  throws(()=>pe.end(),/end/,"second end");
  let DOC='<?xml version="1.0"?><!-- head --><root a="1"><item>text</item></root>';
  let whole = (()=>{
    let o=[]; let pp=new SAXParser({onOpen:(n,a)=>o.push(["open",n,j(a)]),onText:t=>o.push(["text",t]),onClose:n=>o.push(["close",n])});
    pp.write(DOC); pp.end(); return j(o);
  })();
  let badSplit=0;
  for(let k=1;k<DOC.length;k++){
    let o=[]; let pp=new SAXParser({onOpen:(n,a)=>o.push(["open",n,j(a)]),onText:t=>o.push(["text",t]),onClose:n=>o.push(["close",n])});
    pp.write(DOC.slice(0,k)); pp.write(DOC.slice(k)); pp.end();
    if(j(o)!==whole) badSplit++;
  }
  eq(badSplit,0,"split sweep every offset");
  let big10 = "x".repeat(10*1024);
  let gotComment="";
  let pc = new SAXParser({onComment:t=>gotComment=t});
  pc.write("<r><!--"+big10+"--></r>"); pc.end();
  eq(gotComment.length,10*1024,"10KB comment ok");
  let bigCap = "x".repeat(16*1024*1024+1);
  throws(()=>{ let pp=new SAXParser({}); pp.write("<r><!--"+bigCap+"--></r>"); pp.end(); },/token limit/,"over-cap comment");
  throws(()=>{ let pp=new SAXParser({}); pp.write("<r><![CDATA["+bigCap+"]]></r>"); pp.end(); },/token limit/,"over-cap cdata");
  let atCap = "x".repeat(16*1024*1024);
  let gotCap="";
  let pat = new SAXParser({onComment:t=>gotCap=t});
  pat.write("<r><!--"+atCap+"--></r>"); pat.end();
  eq(gotCap.length,16*1024*1024,"exact cap ok");
  throws(()=>{ let pp=new SAXParser({}); pp.write("<r><!--"); pp.write(bigCap); pp.write("--></r>"); pp.end(); },/token limit/,"streamed over cap");

  eq(j(XMLToObject(XMLParse("<a>x</a>"))),'{"a":"x"}',"toObject text only");
  eq(j(XMLToObject(XMLParse("<a><b>1</b><c>2</c></a>"))),'{"a":{"b":"1","c":"2"}}',"children keys");
  eq(j(XMLToObject(XMLParse("<a><b>1</b><b>2</b></a>"))),'{"a":{"b":["1","2"]}}',"repeat array");
  eq(j(XMLToObject(XMLParse('<a id="7">x</a>'))),'{"a":{"@id":"7","#text":"x"}}',"attr @ and #text");
  eq(j(XMLToObject(XMLParse("<a/>"))),'{"a":{}}',"empty");
  throws(()=>XMLToObject("x"),/element/,"toObject needs element");
  let coll = XMLToObject(XMLParse('<r><a>1</a></r>'));
  let tr = XMLParse('<a __proto__="x"/>');
  eq(tr.attrs.__proto__,"x","proto attr own");
  eq(j(XMLToObject(XMLParse("<a><__proto__>v</__proto__></a>"))),'{"a":{"__proto__":"v"}}',"proto element");

  eq(XMLStringify(XMLParse("<a/>")),"<a/>","stringify self close");
  eq(XMLStringify(XMLParse("<a></a>")),"<a/>","long to self close");
  eq(XMLStringify(XMLParse('<a id="1"/>')),'<a id="1"/>',"attrs survive");
  eq(XMLStringify({name:"a",attrs:{x:"1"},children:["t"]}),'<a x="1">t</a>',"hand built");
  eq(XMLStringify({name:"a"}),"<a/>","attrs optional");
  eq(XMLStringify(XMLParse("<a><b/><c/></a>"),{indent:2}),"<a>\n  <b/>\n  <c/>\n</a>","indent");
  throws(()=>XMLStringify({name:42}),/string/,"node needs string name");
  throws(()=>XMLStringify({name:""}),/valid/,"empty name");
  throws(()=>XMLStringify({name:"1bad"}),/valid/,"bad element name");
  throws(()=>XMLStringify(),/required/,"needs node");
  throws(()=>XMLStringify({name:"a"},{indent:99}),/indent/,"indent bounded");
  let src='<r x="a&amp;b"><i n="1">one</i></r>';
  let once=XMLStringify(XMLParse(src));
  let twice=XMLStringify(XMLParse(once));
  eq(once,twice,"stringify idempotent");

  print("  dyna:xml covered");
}
if(fails){
  print("test_xml_cov_file_html_xml_yaml_json: "+fails+" FAILED of "+n+" assertions");
  throw new Error("test_cov failed");
}
print("test_xml_cov_file_html_xml_yaml_json: "+n+" assertions, 0 failures");
