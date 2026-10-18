// Split from tests/test_cov_file_html_xml_yaml_json.js: the yaml sections.
import { makeTempDir } from "dyna:file";
import { Parse as YAMLParse, ParseAll as YAMLParseAll, Stringify as YAMLStringify } from "dyna:yaml";

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
  eq(j(YAMLParse("a: 1")),j({a:1}),"yaml int");
  eq(j(YAMLParse("a: -3")),j({a:-3}),"neg int");
  eq(j(YAMLParse("a: 0x1F")),j({a:31}),"hex");
  eq(j(YAMLParse("a: 0o17")),j({a:15}),"octal");
  eq(j(YAMLParse("a: 1.5")),j({a:1.5}),"float");
  eq(j(YAMLParse("a: -2.5e3")),j({a:-2500}),"exp");
  eq(j(YAMLParse("a: true\nb: false")),j({a:true,b:false}),"bools");
  eq(j(YAMLParse("a: null\nb: ~\nc:")),j({a:null,b:null,c:null}),"nulls");
  eq(j(YAMLParse("a: hello world")),j({a:"hello world"}),"plain string");
  eq(j(YAMLParse("a: 'it''s'")),j({a:"it's"}),"single quote double");
  eq(j(YAMLParse('a: "tab\\there"')),j({a:"tab\there"}),"escape");
  eq(j(YAMLParse("a: '123'")),j({a:"123"}),"quoted number string");
  eq(j(YAMLParse("a: [1, 2, 3]")),j({a:[1,2,3]}),"flow seq");
  eq(j(YAMLParse("a: {x: 1, y: 2}")),j({a:{x:1,y:2}}),"flow map");
  eq(j(YAMLParse("a:\n  b:\n    c: deep")),j({a:{b:{c:"deep"}}}),"block nested");
  eq(j(YAMLParse("- 1\n- 2")),j([1,2]),"top seq");
  eq(j(YAMLParse("a:\n  - x\n  - y")),j({a:["x","y"]}),"seq under key");
  throws(()=>YAMLParse("a: 1\na: 2"), /duplicate/i, "duplicate key refused");
  throws(()=>YAMLParse("a: {x: 1, x: 2}"), /duplicate/i, "flow duplicate refused");
  throws(()=>YAMLParse("a: &anchor 1"),/anchor/,"anchor throws");
  throws(()=>YAMLParse("b: *anchor"),/alias/,"alias throws");
  throws(()=>YAMLParse("a: !!str 1"),/tag/,"tag throws");
  throws(()=>YAMLParse("%YAML 1.2\n---\na: 1"),/directive/,"directive throws");
  throws(()=>YAMLParse("a: 1\n---\nb: 2"),/ParseAll/,"multi doc needs ParseAll");
  throws(()=>YAMLParse("a: "+ "x".repeat(64*1024*1024+1)),/exceeds/,"64MB cap");
  throws(()=>YAMLParse(42),/string/,"yaml non-string");
  throws(()=>YAMLParse(),/required/,"yaml no arg");
  throws(()=>YAMLParse("a: [1, 2"),/unterminated/,"unterminated flow");
  throws(()=>YAMLParse("a: {b}"),/colon/,"flow mapping colon");
  throws(()=>YAMLParse('a: "\\q"'),/unknown escape/,"unknown escape");
  throws(()=>YAMLParse("a:\n\tb: 1"),/tab/,"tab indent");
  throws(()=>YAMLParse("a: 1\n  b: 2"),/indentation/,"bad indent");
  eq(j(YAMLParse("country: no")),j({country:"no"}),"norway no string");
  eq(j(YAMLParse("a: yes\nb: on")),j({a:"yes",b:"on"}),"yes on strings");
  eq(YAMLParse("s: |\n  one\n  two\n").s,"one\ntwo\n","literal keep");
  eq(YAMLParse("s: >\n  one\n  two\n").s,"one two\n","folded");
  let cases = [
    {a:1,b:"two",c:true,d:null},
    {list:[1,2,3],nested:{x:{y:"z"}}},
    [{name:"a"},{name:"b"}],
    {empty:[],blank:{},s:""},
    {tricky:"yes",also:"no",num:"123"},
  ];
  let bad=0;
  for(let c of cases){
    let text=YAMLStringify(c);
    let back=YAMLParse(text);
    if(j(back)!==j(c)) bad++;
  }
  eq(bad,0,"yaml round-trip "+cases.length);
  eq(YAMLStringify({a:1}),"a: 1\n","simple doc");
  eq(YAMLStringify({a:[1,2]}),"a:\n  - 1\n  - 2\n","seq under key");
  eq(YAMLStringify("scalar"),"scalar\n","bare scalar");
  eq(YAMLStringify(null),"null\n","null doc");
  throws(()=>YAMLStringify(),/required/,"yaml stringify no arg");
  throws(()=>YAMLStringify({}, {indent:0}),/indent/,"indent 0");
  throws(()=>YAMLStringify({}, {indent:11}),/indent/,"indent 11");
  let lone = YAMLParse('a: "\ud800"');
  eq(typeof lone.a,"string","lone surrogate string");
  let sLone = YAMLStringify({a:"\ud800"});
  assert(typeof sLone==="string","stringify lone surrogate produces string");
  throws(()=>YAMLParse("d:\n  <<: x"),/merge/,"merge key");
  throws(()=>YAMLParse("? complex\n: value"),/explicit/,"explicit key");
  let proto = YAMLParse("__proto__: x\nb: 1");
  eq(proto.__proto__,"x","proto own yaml");
  eq(Object.keys(proto).length,2,"proto enumerable");
  eq(j(YAMLParseAll("a: 1\n---\nb: 2")),j([{a:1},{b:2}]),"parseAll two");
  eq(YAMLParseAll("").length,0,"parseAll empty");
  let deepYaml = "a:\n"+ Array.from({length:50},(_,i)=> " ".repeat((i+1)*2)+"b:").join("\n");
  try{ YAMLParse(deepYaml); assert(true,"deep 50 ok"); }catch(e){ assert(false,"deep 50 shouldn't throw"); }

  print("  dyna:yaml covered");
}
if(fails){
  print("test_yaml_cov_file_html_xml_yaml_json: "+fails+" FAILED of "+n+" assertions");
  throw new Error("test_cov failed");
}
print("test_yaml_cov_file_html_xml_yaml_json: "+n+" assertions, 0 failures");
