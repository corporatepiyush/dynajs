/* test_xml_large.js -- large/complex XML: bytes/KB/MB portfolios.
 * Run: dynajs tests/test_xml_large.js */
import { XMLParse, XMLStringify, XMLToObject, SAXParser } from "dyna:xml";
let n=0, fails=0;
function assert(c,msg){n++; if(!c){fails++; print("FAIL: "+msg);}}
function eq(a,b,msg){assert(a===b,msg+" got "+JSON.stringify(a).slice(0,300)+" want "+String(b).slice(0,300));}
function throws(fn,msg){let t=false; try{fn();}catch(e){t=true;} assert(t,msg);}

// Few bytes
eq(XMLParse("<a/>").name,"a","tiny empty");
eq(XMLParse("<a>hi</a>").children[0],"hi","tiny text");
eq(XMLParse("<a x=\"1\"/>").attrs.x,"1","tiny attr");

// Few KB: 1k elements
{
  const xml="<r>"+"<item id=\"x\">hello</item>".repeat(1000)+"</r>";
  const doc=XMLParse(xml);
  assert(doc.children.length===1000,"1k count");
  assert(doc.children[0].attrs.id==="x","1k attr");
  const ser=XMLStringify(doc);
  assert(ser.length>20000,"1k ser len");
  const rt=XMLParse(ser);
  assert(rt.children.length===1000,"1k roundtrip");
}
// Few MB: 20k elements ~600KB, and 1MB text
{
  const xml="<r>"+"<item>text</item>".repeat(20000)+"</r>";
  const t0=Date.now();
  const doc=XMLParse(xml);
  const dt=Date.now()-t0;
  assert(doc.children.length===20000,"20k count");
  assert(dt<2000,"20k parse <2s dt="+dt);
  const ser=XMLStringify(doc);
  assert(ser.length===xml.length,"20k ser len");
}
{
  const big="a".repeat(1024*1024);
  const xml="<r>"+big+"</r>";
  const doc=XMLParse(xml);
  assert(doc.children[0].length===1024*1024,"1M text");
  const t0=Date.now();
  const ser=XMLStringify(doc);
  const dt=Date.now()-t0;
  assert(ser.length===xml.length,"1M roundtrip len");
  assert(dt<2000,"1M stringify <2s dt="+dt);
}
// Entity-dense few KB vs MB
{
  const xml="<r>"+"a &amp; b &lt; c ".repeat(5000)+"</r>";
  const doc=XMLParse(xml);
  assert(doc.children[0].indexOf("&")>=0,"entity dense decoded");
  const ser=XMLStringify(doc);
  assert(ser.indexOf("&amp;")>=0,"entity re-escaped");
}
{
  const xml="<r>"+"ab&amp;".repeat(200*1024)+"</r>";
  const t0=Date.now();
  const doc=XMLParse(xml);
  const dt=Date.now()-t0;
  assert(dt<3000,"1M ents <3s dt="+dt);
}
// Deep nesting
{
  const deep="<a>".repeat(200)+"x"+"</a>".repeat(200);
  const doc=XMLParse(deep);
  let cur=doc; for(let i=0;i<199;i++) cur=cur.children[0];
  assert(cur.children[0]==="x","deep 200");
}
throws(()=>XMLParse("<a>".repeat(300)+"</a>".repeat(300)),"deep over cap");
// Attr heavy
{
  const xml="<r>"+"<x a=\"1\" b=\"2\" c=\"3\" d=\"4\" e=\"5\">t</x>".repeat(1000)+"</r>";
  const doc=XMLParse(xml);
  assert(doc.children[0].attrs.e==="5","attr heavy");
  assert(doc.children.length===1000,"attr count");
}
// SAX: few bytes, KB, MB and chunk sizes
{
  const xml="<r><a>hi</a></r>";
  const out=[];
  const p=new SAXParser({onOpen:(n)=>out.push(n)});
  p.write(xml); p.end();
  assert(out[0]==="r","sax tiny");
}
{
  const xml="<r>"+"<item>hello</item>".repeat(1000)+"</r>";
  const whole=[];
  const p1=new SAXParser({onOpen:(n)=>whole.push(n)});
  p1.write(xml); p1.end();
  const chunked=[];
  const p2=new SAXParser({onOpen:(n)=>chunked.push(n)});
  for(let i=0;i<xml.length;i+=64) p2.write(xml.slice(i,i+64));
  p2.end();
  assert(JSON.stringify(whole)===JSON.stringify(chunked),"sax 64B chunks");
}
{
  const xml="<r>"+"<item>t</item>".repeat(10000)+"</r>";
  const t0=Date.now();
  const p=new SAXParser({onOpen:()=>{},onText:()=>{},onClose:()=>{}});
  p.write(xml); p.end();
  const dt=Date.now()-t0;
  assert(dt<2000,"sax 10k <2s dt="+dt);
}
// XMLToObject large
{
  const xml="<r>"+"<item>1</item>".repeat(5000)+"</r>";
  const doc=XMLParse(xml);
  const obj=XMLToObject(doc);
  assert(Array.isArray(obj.r.item) && obj.r.item.length===5000,"toObject 5k");
}
if(fails){print(`test_xml_large: ${fails} FAILED of ${n}`); throw new Error("fail");}
print(`test_xml_large: ${n} assertions, 0 failures`);
