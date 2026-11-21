// covers: dyna:xml.SAXParser
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_xml_sax_parser_large");
import { XMLParse, XMLStringify, XMLToObject, SAXParser } from "dyna:xml";
import { cpuNow } from "../lib/cpuclock.js";
let n=0, fails=0;
function assert(c,msg){n++; if(!c){fails++; print("FAIL: "+msg);}}
function eq(a,b,msg){assert(a===b,msg+" got "+JSON.stringify(a).slice(0,300)+" want "+String(b).slice(0,300));}
function throws(fn,msg){let t=false; try{fn();}catch(e){t=true;} assert(t,msg);}

__kitS.section("sax tiny", () => {
  const xml="<r><a>hi</a></r>";
  const out=[];
  const p=new SAXParser({onOpen:(n)=>out.push(n)});
  p.write(xml); p.end();
  assert(out[0]==="r","sax tiny");
});

__kitS.section("sax 64B chunks", () => {
  const xml="<r>"+"<item>hello</item>".repeat(1000)+"</r>";
  const whole=[];
  const p1=new SAXParser({onOpen:(n)=>whole.push(n)});
  p1.write(xml); p1.end();
  const chunked=[];
  const p2=new SAXParser({onOpen:(n)=>chunked.push(n)});
  for(let i=0;i<xml.length;i+=64) p2.write(xml.slice(i,i+64));
  p2.end();
  assert(JSON.stringify(whole)===JSON.stringify(chunked),"sax 64B chunks");
});

__kitS.section("sax 10k <2s dt", () => {
  const xml="<r>"+"<item>t</item>".repeat(10000)+"</r>";
  const t0=cpuNow();
  const p=new SAXParser({onOpen:()=>{},onText:()=>{},onClose:()=>{}});
  p.write(xml); p.end();
  const dt=cpuNow()-t0;
  assert(dt<2000,"sax 10k <2s dt="+dt);
});
__kitS.end();
if(fails){print(`test_xml_large: ${fails} FAILED of ${n}`); throw new Error("fail");}
print(`test_xml_large: ${n} assertions, 0 failures`);
