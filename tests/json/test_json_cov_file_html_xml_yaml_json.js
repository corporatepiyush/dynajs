// Split from tests/test_cov_file_html_xml_yaml_json.js: the json sections.
import { makeTempDir } from "dyna:file";
import { Pointer, Patch } from "dyna:json";

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
  const D5 = {"foo":["bar","baz"],"":0,"a/b":1,"c%d":2,"e^f":3,"g|h":4,"i\\j":5,"k\"l":6," ":7,"m~n":8};
  eq(j(Pointer.get(D5,"")),j(D5),"pointer whole doc");
  eq(j(Pointer.get(D5,"/foo")),j(["bar","baz"]),"/foo");
  eq(Pointer.get(D5,"/foo/0"),"bar","/foo/0");
  eq(Pointer.get(D5,"/"),0,"/ empty key");
  eq(Pointer.get(D5,"/a~1b"),1,"/a~1b");
  eq(Pointer.get(D5,"/m~0n"),8,"/m~0n");
  assert(Pointer.has(D5,"/foo/0")===true,"has present");
  assert(Pointer.has(D5,"/nope")===false,"has missing");
  assert(Pointer.has({a:5},"/a/b")===false,"has into scalar false");
  throws(()=>Pointer.has({foo:[1,2]},"/foo/01"),/./,"has leading zero throws");
  throws(()=>Pointer.get({foo:[1,2]},"/foo/-"),/./,"get - throws");
  throws(()=>Pointer.get({a:[1,2]},"/a/01"),/./,"leading zero");
  throws(()=>Pointer.get({},"nope"),/./,"no leading /");
  eq(j(Pointer.set({foo:[1,2]},"/foo/-",3)),j({foo:[1,2,3]}), "set - appends");
  let sd={foo:[1,2]}; Pointer.set(sd,"/bar",{x:1}); eq(j(sd.bar),j({x:1}),"set add member");
  let rm={a:1,b:2}; Pointer.remove(rm,"/a"); eq(j(rm),j({b:2}),"remove member");
  throws(()=>Pointer.set({}," /nope/y",1),/./,"set missing parent");
  throws(()=>Pointer.remove({a:1},"/nope"),/./,"remove missing");
  throws(()=>Pointer.remove({a:1},""),/./,"remove root");
  eq(Pointer.unescape("a~1b"),"a/b","unescape ~1");
  eq(Pointer.unescape("m~0n"),"m~n","unescape ~0");
  eq(Pointer.escape("a/b"),"a~1b","escape /");
  eq(Pointer.escape("m~n"),"m~0n","escape ~");
  throws(()=>Pointer.unescape("a~2b"),/./,"unescape ~2");
  throws(()=>Pointer.unescape("a~"),/./,"dangling ~");
  for(let t of ["","a/b","m~n","c%d"," ", "x/y~z"]) assert(Pointer.unescape(Pointer.escape(t))===t,"round trip "+j(t));
  let pa = Patch.apply({foo:"bar"},[{op:"add",path:"/baz",value:"qux"}]);
  eq(pa.foo,"bar","patch add keeps foo"); eq(pa.baz,"qux","patch add new baz");
  eq(j(Patch.apply({foo:["bar","baz"]},[{op:"add",path:"/foo/1",value:"qux"}])),j({foo:["bar","qux","baz"]}),"add array");
  eq(j(Patch.apply({baz:"qux",foo:"bar"},[{op:"remove",path:"/baz"}])),j({foo:"bar"}),"remove member");
  eq(j(Patch.apply({foo:["bar","qux","baz"]},[{op:"remove",path:"/foo/1"}])),j({foo:["bar","baz"]}),"remove array");
  let rep2 = Patch.apply({baz:"qux",foo:"bar"},[{op:"replace",path:"/baz",value:"boo"}]);
  eq(rep2.baz,"boo","replace value2"); eq(rep2.foo,"bar","replace keeps foo");
  let rep = Patch.apply({baz:"qux",foo:"bar"},[{op:"replace",path:"/baz",value:"boo"}]);
  eq(rep.baz,"boo","replace value");
  let moved = Patch.apply({foo:{bar:"baz",waldo:"fred"},qux:{corge:"grault"}},[{op:"move",from:"/foo/waldo",path:"/qux/thud"}]);
  eq(j(moved),j({foo:{bar:"baz"},qux:{corge:"grault",thud:"fred"}}),"move");
  let copied = Patch.apply({foo:["all","grass","cows","eat"]},[{op:"move",from:"/foo/1",path:"/foo/3"}]);
  eq(j(copied),j({foo:["all","cows","eat","grass"]}),"move array");
  let tdoc={a:{b:[1,2,{c:"x"}]}};
  Patch.apply(tdoc,[{op:"test",path:"/a",value:{b:[1,2,{c:"x"}]}}]);
  assert(true,"test succeed");
  throws(()=>Patch.apply(tdoc,[{op:"test",path:"/a",value:{b:[1,2,{c:"y"}]}}]),/./,"test deep mismatch");
  throws(()=>Patch.apply({baz:"qux"},[{op:"test",path:"/baz",value:"bar"}]),/./,"test fails");
  let adoc={a:{b:{c:"foo"}}};
  let before=j(adoc);
  throws(()=>Patch.apply(adoc,[{op:"replace",path:"/a/b/c",value:42},{op:"test",path:"/a/b/c",value:"C"}]),/./,"atomic fail");
  eq(j(adoc),before,"atomic leaves byte-identical");
  let adoc2={a:1,b:[1,2,3]};
  let before2=j(adoc2);
  throws(()=>Patch.apply(adoc2,[{op:"add",path:"/x",value:1},{op:"remove",path:"/b/0"},{op:"move",from:"/b/0",path:"/b/0/child"}]),/./,"mid patch move error");
  eq(j(adoc2),before2,"mid-patch rollback");
  let docC={}, val={nested:1};
  let resC=Patch.apply(docC,[{op:"add",path:"/a",value:val}]);
  val.nested=99;
  eq(resC.a.nested,1,"added value cloned");
  resC.a.nested=5;
  eq(val.nested,99,"mutating result not leak");
  let docS={src:{v:1}};
  let resS=Patch.apply(docS,[{op:"copy",from:"/src",path:"/dst"}]);
  resS.src.v=2;
  eq(resS.dst.v,1,"copy deep clone");
  throws(()=>Patch.apply({},[{op:"splice",path:"/a",value:1}]),/./,"unknown op");
  throws(()=>Patch.apply({},[{op:42,path:"/a"}]),/./,"op must string");
  throws(()=>Patch.apply({},[{path:"/a",value:1}]),/./,"missing op");
  throws(()=>Patch.apply({},[{op:"add",value:1}]),/./,"missing path");
  throws(()=>Patch.apply({},[{op:"add",path:42,value:1}]),/./,"path string");
  throws(()=>Patch.apply({},[{op:"add",path:"/a"}]),/./,"add requires value");
  throws(()=>Patch.apply({},[{op:"move",path:"/a"}]),/./,"move requires from");
  throws(()=>Patch.apply({a:1},[{op:"move",from:"/b",path:"/c"}]),/./,"move from missing");
  throws(()=>Patch.apply({a:{b:1}},[{op:"move",from:"/a",path:"/a/b"}]),/./,"move into child");
  throws(()=>Patch.apply({foo:[1,2]},[{op:"add",path:"/foo/3",value:0}]),/./,"add index above length");
  function chain(d){ let v={}; for(let i=0;i<d;i++) v=[v]; return v; }
  assert(Array.isArray(Patch.apply(chain(127),[])),"depth 127");
  assert(Array.isArray(Patch.apply(chain(128),[])),"depth 128");
  throws(()=>Patch.apply(chain(129),[]),/./,"depth 129");
  throws(()=>Patch.apply({},[{op:"add",path:"/a",value:chain(129)}]),/./,"over-deep add value");
  try{ Patch.apply({a:1},[{op:"add",path:"/x",value:1},{op:"test",path:"/a",value:9}]); assert(false,"should throw"); }catch(e){ let m=String(e); assert(m.indexOf("[1]")>=0,"msg index"); assert(m.indexOf("/a")>=0,"msg path"); }
  let injDoc={ "a/b":1 };
  eq(Pointer.get(injDoc,"/a~1b"),1,"path with slash key");
  print("  dyna:json covered");
}
if(fails){
  print("test_json_cov_file_html_xml_yaml_json: "+fails+" FAILED of "+n+" assertions");
  throw new Error("test_cov failed");
}
print("test_json_cov_file_html_xml_yaml_json: "+n+" assertions, 0 failures");
